// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 - 2016 Cavium, Inc.
 */

/*
 * [한국어 설명] Cavium ThunderX PEM 호스트 브리지의 config 접근 우회 드라이버 (pci-thunder-pem.c)
 *
 * === 파일의 역할 ===
 * Cavium(현 Marvell) ThunderX SoC 의 PEM(PCI Express MAC) 호스트 브리지를
 * 리눅스 PCI 코어에 붙이는 드라이버다. 이름은 컨트롤러 드라이버지만 실제
 * 하는 일의 대부분은 **하드웨어가 틀리게 내놓는 config 공간을 고쳐서
 * 보여 주는 것** 이다. 링크 훈련이나 전원·클럭 관리는 이 파일에 없다.
 *
 * 고쳐야 하는 것이 두 종류다.
 *   1) 브리지 자신(버스 번호가 busr.start 인 devfn 0)의 config 공간은
 *      표준 ECAM 창으로 읽을 수 없다. PEM_CFG_RD 라는 **주소 래치 레지스터**
 *      에 오프셋을 쓰고 되읽어야 데이터가 상위 32비트에 실려 나온다.
 *      쓰기도 PEM_CFG_WR 에 "상위 32비트=데이터, 하위=주소" 를 한 번에 쓴다.
 *      그리고 그렇게 읽어 온 값에도 쓰레기가 섞여 있어, 특정 오프셋마다
 *      값을 덧칠한다(MSI capability 건너뛰기, PME 벡터 보정, MSI-X 테이블
 *      크기 보정 등).
 *   2) 이 브리지에는 MSI-X 용 BAR 가 config 공간에 정상적으로 보이지 않는다.
 *      그래서 이 파일이 EA(Enhanced Allocation) capability 구조체를 **통째로
 *      지어내서** 0xb0~0xd0 구간에 얹어 준다. PCI 코어는 그것을 진짜
 *      capability 로 읽어 MSI-X 테이블과 PBA 의 물리 주소를 알아낸다.
 *
 * 브리지 아래에 달린 실제 장치들의 config 접근은 손대지 않는다 —
 * pci_generic_config_read()/write() 로 그대로 흘려보낸다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 "PCI 코어 <-> ECAM 공통 계층" 사이에서 config 접근만 가로채는
 * 얇은 층이다. 진입 경로가 두 갈래다.
 *
 * (가) 디바이스 트리 경로 (CONFIG_PCI_HOST_THUNDER_PEM):
 *   플랫폼 드라이버 코어 -> pci_host_common_probe()  [pci-host-common.c:306]
 *     -> of_device_get_match_data() 로 pci_thunder_pem_ops 를 얻는다
 *     -> pci_host_common_init() -> pci_ecam_create()  [ecam.c:180]
 *        -> ECAM 창을 매핑하고 ops->init 을 부른다
 *           -> [이 파일] thunder_pem_platform_init()
 *              -> reg[1] 자원(PEM 레지스터 창)을 찾아 thunder_pem_init() 으로
 *     -> pci_host_probe() 로 버스를 스캔. 그 스캔의 모든 config 접근이
 *        [이 파일] thunder_pem_config_read()/write() 를 지난다.
 *
 * (나) ACPI 경로 (CONFIG_ACPI && CONFIG_PCI_QUIRKS):
 *   ARM64 의 MCFG 쿼크 표가 이 파일이 내보내는 thunder_pem_ecam_ops 를
 *   특정 OEM ID 에 묶어 둔다. 그 표는 arch/arm64 아래에 있어 이 트리
 *   (drivers/{block,nvme,pci,s390,vfio} 만 있는 희소 체크아웃)에서는 확인 못 함.
 *   MCFG 처리가 pci_ecam_create() 를 부르면 ops->init 으로
 *   [이 파일] thunder_pem_acpi_init() 이 불리고, 거기서
 *   acpi_get_rc_resources()  [pci-acpi.c:359] 로 PEM 레지스터 창을 찾는다.
 *   구형 펌웨어라 그 조회가 실패하면 thunder_pem_legacy_fw() 가 주소를
 *   **손으로 계산** 한다.
 *
 * 실행 컨텍스트: init 계열(platform_init / acpi_init / thunder_pem_init /
 * reserve_range / legacy_fw)은 호스트 브리지 probe 중의 프로세스 컨텍스트라
 * 잠들 수 있다. 반대로 config 접근 계열(config_read/write, bridge_read/write,
 * w1c_bits, w1_bits)은 **PCI 코어가 raw spinlock 인 pci_lock 을 쥔 채**
 * 부르므로 절대 잠들면 안 된다(drivers/pci/access.c:103, :313).
 * 그 잠금 덕분에 PEM_CFG_RD 의 "주소 쓰기 -> 되읽기" 두 단계가 다른 config
 * 접근에 끼어들려 깨지지 않는다 — 이 하드웨어 설계가 성립하는 전제다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어(drivers/pci/access.c 의 pci_bus_read_config_ 계열)가
 *   bus->ops 를 통해 이 파일의 read/write 로 내려온다.
 * 아래쪽: drivers/pci/ecam.c 의 pci_ecam_create()/pci_ecam_map_bus() 와
 *   drivers/pci/controller/pci-host-common.c 의 pci_host_common_probe().
 *   접점이 struct pci_ecam_ops 표다 — bus_shift, init 훅, 그리고 pci_ops
 *   (map_bus/read/write) 셋.
 * 옆쪽: drivers/pci/pci-acpi.c 의 acpi_get_rc_resources(). 그 함수의 주석이
 *   이 파일을 유일한 호출자로 지목하고 있을 만큼 서로를 위해 만들어진 짝이다.
 *   ACPI 코어(acpi_get_node, acpi_driver_data, struct acpi_pci_root)와
 *   자원 관리(request_mem_region, resource_set_size)는 이 트리에 헤더가 없어
 *   내부는 확인 대상 밖이며, 이 파일에서 읽히는 호출 규약까지만 적었다.
 *
 * 데이터 흐름:
 *   디바이스 트리 reg[1] 또는 ACPI _CRS -> PEM 레지스터 창의 물리 주소
 *     -> thunder_pem_init() 이 64KB 를 ioremap -> pem_reg_base
 *     -> 그 창의 PEM_CFG_RD / PEM_CFG_WR 로 브리지 config 가 오간다.
 *   같은 물리 주소 + 0xf00000 -> 합성 EA 항목 세 워드(ea_entry[])
 *     -> 브리지 config 오프셋 0xc8/0xcc/0xd0 을 읽을 때 그대로 흘러 나간다
 *     -> PCI 코어가 그것을 MSI-X BAR 로 인식한다.
 *
 * 공유 상태: struct thunder_pem_pci 하나이며 cfg->priv 에 매달아 둔다.
 *   init 에서 채운 뒤로 불변이고 별도 잠금이 없다 — 위에서 말한 pci_lock 이
 *   그 위의 모든 접근을 직렬화한다.
 *
 * === 주요 함수/구조체 요약 ===
 * thunder_pem_bridge_read()   : 브리지 config 읽기. PEM_CFG_RD 래치 + 오프셋별 덧칠.
 *                               이 파일에서 가장 긴 함수이자 핵심이다.
 * thunder_pem_bridge_write()  : 브리지 config 쓰기. 32비트로 넓힌 뒤 W1C 비트를
 *                               피하고, 읽기 전용 1 비트를 강제로 세워 쓴다.
 * thunder_pem_bridge_w1c_bits(): 오프셋별로 "1 을 쓰면 지워지는" 비트 마스크를 준다.
 * thunder_pem_bridge_w1_bits() : 오프셋별로 "항상 1 이어야 하는" 비트 마스크를 준다.
 * thunder_pem_config_read()/write() : 브리지인지 하위 장치인지를 갈라 보낸다.
 * thunder_pem_init()          : PEM 레지스터 창을 매핑하고 합성 EA 항목을 만든다.
 * thunder_pem_acpi_init()     : ACPI 경로의 자원 확보. 구형 펌웨어 우회를 포함한다.
 * thunder_pem_legacy_fw()     : ACPI 가 자원을 못 줄 때 주소를 손으로 계산한다.
 * struct thunder_pem_pci      : PEM 레지스터 창과 합성 EA 항목 세 워드.
 *
 * === 왜 파일 전체가 #if 로 감싸여 있는가 ===
 * 맨 위의 `#if defined(CONFIG_PCI_HOST_THUNDER_PEM) || (defined(CONFIG_ACPI)
 * && defined(CONFIG_PCI_QUIRKS))` 가 파일 전체를 감싸고, 파일 끝의 두 `#endif`
 * 가 그것과 안쪽 ACPI 블록을 함께 닫는다. 두 진입 경로가 서로 다른 설정으로
 * 켜지는데 공통 코드(bridge_read/write, thunder_pem_init)를 나눠 쓰기 때문에,
 * "둘 중 하나라도 켜지면 공통 부분을 넣고, 각 경로 고유 부분은 다시 자기
 * 조건으로 감싸는" 이중 구조가 되었다.
 *
 * === NVMe 관점 ===
 * 이 브리지 아래에 NVMe 컨트롤러를 달면, 그 컨트롤러의 config 접근은
 * thunder_pem_config_read()/write() 의 **두 번째 갈래**(버스 번호가
 * busr.start 가 아닌 경우)로 가 pci_generic_config_read()/write() 로
 * 그대로 흘러간다. 즉 이 파일의 덧칠은 브리지 자신에게만 적용되고
 * NVMe 컨트롤러의 BAR·MSI-X·capability 는 손대지 않는다. 이 파일이 합성하는
 * EA 항목도 상류 주석이 밝히듯 **브리지 자신의 PEM/AER 인터럽트용 MSI-X BAR**
 * 를 가리키는 것이지, 하위 장치의 MSI-X 와는 관계가 없다. 다만 브리지의
 * config 공간이 잘못 보이면 그 아래 버스 열거 자체가 어긋나므로, NVMe 가
 * 제대로 잡히는지는 결국 이 덧칠이 옳은지에 달려 있다.
 */

/* [한국어] FIELD_PREP() 매크로. 아래 ACPI 구형 펌웨어 경로에서 PEM_NODE_MASK 와
 * PEM_INDX_MASK 자리에 노드 번호와 인덱스를 밀어 넣는 데 쓴다. */
#include <linux/bitfield.h>
/* [한국어] 기본 커널 관용구와 lower_32_bits()/upper_32_bits(). 합성 EA 항목을 만들 때
 * 64비트 물리 주소를 두 워드로 쪼개는 데 쓴다. */
#include <linux/kernel.h>
/* [한국어] __init 계열 선언. 이 파일은 builtin_platform_driver 로 등록된다. */
#include <linux/init.h>
/* [한국어] struct pci_bus, PCIBIOS_SUCCESSFUL / PCIBIOS_DEVICE_NOT_FOUND 반환 코드,
 * pci_generic_config_read()/write(). */
#include <linux/pci.h>
/* [한국어] of_ 주소 변환 헤더. 이 파일이 직접 부르는 of_ 주소 함수는 없다(전수 확인) —
 * 디바이스 트리 자원은 platform_get_resource() 로 얻는다. */
#include <linux/of_address.h>
/* [한국어] of_pci 헤더. 역시 이 파일이 직접 부르는 함수는 없다(전수 확인). */
#include <linux/of_pci.h>
/* [한국어] struct acpi_pci_root, acpi_driver_data(), to_acpi_device(), acpi_get_node().
 * ACPI 경로에서 루트 브리지 노드의 세그먼트 번호와 NUMA 노드를 알아내는 데 쓴다. */
#include <linux/pci-acpi.h>
/* [한국어] struct pci_config_window, struct pci_ecam_ops, pci_ecam_map_bus().
 * 이 드라이버가 ECAM 공통 계층에 자기를 끼워 넣는 접점이 전부 여기 있다.
 * 이 트리(희소 체크아웃)에는 이 헤더가 없어 구조체 정의는 확인 못 함 —
 * 대신 drivers/pci/ecam.c 의 사용처로 필드 의미를 확인했다. */
#include <linux/pci-ecam.h>
/* [한국어] struct platform_device, platform_get_resource(), builtin_platform_driver(). */
#include <linux/platform_device.h>
/* [한국어] readq()/writeq(). **이 파일의 하드웨어 접근이 전부 64비트다** —
 * PEM_CFG_RD 는 상위 32비트에 데이터, 하위에 주소를 담는 구조라 64비트
 * 접근이 아니면 동작하지 않는다. lo-hi 판을 쓰면 64비트 접근을 지원하지 않는
 * 아키텍처에서 하위 워드부터 두 번 나눠 접근하는 대체 구현이 들어간다. */
#include <linux/io-64-nonatomic-lo-hi.h>
/* [한국어] drivers/pci 내부 헤더. 여기서 필요한 것은 acpi_get_rc_resources() 선언
 * 하나다(drivers/pci/pci.h:2760, 조건이 맞지 않으면 :2771 의 스텁). */
#include "../pci.h"
/* [한국어] pci_host_common_probe() 선언. 아래 platform_driver 의 .probe 로 그대로 건다. */
#include "pci-host-common.h"

/* [한국어] **파일 전체를 감싸는 조건이다.** 디바이스 트리 경로와 ACPI 경로가 서로 다른
 * 설정으로 켜지지만 공통 코드(브리지 config 접근, thunder_pem_init)를 나눠 쓰므로,
 * 둘 중 하나라도 켜져 있으면 여기부터 파일 끝까지를 컴파일한다.
 * 각 경로 고유 부분은 아래에서 다시 자기 조건으로 감싼다. */
#if defined(CONFIG_PCI_HOST_THUNDER_PEM) || (defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS))

/* [한국어] config **쓰기** 창(PEM 레지스터 창 기준 0x28). 한 번의 64비트 쓰기로
 * 하위 32비트에 주소, 상위 32비트에 데이터를 함께 보낸다. */
#define PEM_CFG_WR 0x28
/* [한국어] config **읽기** 창(0x30). 주소 래치 역할을 겸한다 — 오프셋을 쓴 뒤 되읽으면
 * 그 자리의 데이터가 상위 32비트에 실려 나온다. 즉 한 번의 논리적 읽기가
 * **두 번의 MMIO** 다. */
#define PEM_CFG_RD 0x30

/*
 * Enhanced Configuration Access Mechanism (ECAM)
 *
 * N.B. This is a non-standard platform-specific ECAM bus shift value.  For
 * standard values defined in the PCI Express Base Specification see
 * include/linux/pci-ecam.h.
 */
/* [한국어] ECAM 창에서 버스 번호를 미는 비트 수. **표준값이 20 인데 여기서는 24 다** —
 * 버스 하나가 1MB 가 아니라 16MB 를 차지하고, devfn 하나가 4KB 가 아니라
 * 64KB(24 - 8 = 16 비트 자리)를 차지한다. 위 상류 주석이 말하듯 플랫폼 고유값이다.
 * drivers/pci/ecam.c:583 의 pci_ecam_map_bus() 가 ops->bus_shift 가 0 이 아닌 것을
 * 보고 표준 계산 대신 이 값을 쓴 계산으로 갈라진다. */
#define THUNDER_PCIE_ECAM_BUS_SHIFT	24

/* [한국어] 이 드라이버가 브리지 하나에 대해 들고 있는 상태 전부.
 * thunder_pem_init() 이 devm_kzalloc 으로 잡아 cfg->priv 에 매달아 두고,
 * config 접근 함수들이 bus->sysdata -> cfg->priv 로 되찾는다.
 * devm 이라 별도 해제 코드가 없다. */
struct thunder_pem_pci {
	/* [한국어] 합성해서 내보낼 EA(Enhanced Allocation) 항목의 세 워드.
	 * 왜 필요한가: 이 브리지의 MSI-X 용 BAR 가 config 공간에 정상적으로 보이지
	 *   않는다. 그래서 EA capability 를 지어내 PCI 코어에 그 물리 주소를 알린다.
	 *   ES(Entry Size)=3 이라 헤더 뒤에 세 워드가 따라오며 그것이 이 배열이다.
	 *   [0] = Base Low(하위 두 비트로 64비트 형식을 표시), [1] = MaxOffset Low,
	 *   [2] = Base High.
	 * 설정자: thunder_pem_init() 이 PEM 레지스터 창의 물리 주소로부터 계산한다.
	 * 읽는 자: thunder_pem_bridge_read() 의 case 0xc8 / 0xcc / 0xd0.
	 * 값 범위: 위 계산 결과. 항목 자체의 비트 의미는 PCIe 기본 규격의 EA 절을
	 *   따르며, 그 규격 문서는 이 트리에 없다.
	 * 동기화: init 에서 한 번 쓰고 이후 읽기 전용. 읽기는 pci_lock 아래에서만 일어난다. */
	u32		ea_entry[3];
	/* [한국어] PEM 컨트롤러 레지스터 창의 매핑 주소. 위 PEM_CFG_WR / PEM_CFG_RD 오프셋이
	 * 이 주소를 기준으로 더해진다.
	 * 설정자: thunder_pem_init() 의 devm_ioremap(dev, res_pem->start, 0x10000).
	 *   ECAM config 창과는 **완전히 다른 물리 창** 이라는 점이 중요하다.
	 * 읽는 자: thunder_pem_bridge_read()/write() 의 readq()/writeq().
	 * 값 범위: 유효한 __iomem 포인터. 실패는 NULL 로 오며 init 이 -ENOMEM 으로 접는다.
	 * 동기화: init 에서 한 번 쓰고 이후 읽기 전용. */
	void __iomem	*pem_reg_base;
};

/* [한국어]
 * thunder_pem_bridge_read - PEM 브리지 자신의 config 를 읽고 값을 덧칠해 돌려준다
 *
 * @bus: 접근 대상 버스. bus->sysdata 에 struct pci_config_window 가 매달려 있다.
 * @devfn: 장치/함수 번호. 브리지는 devfn 0 하나뿐이라 그 외는 거절한다.
 * @where: config 공간 바이트 오프셋.
 * @size: 1, 2, 4 중 하나.
 * @val: [출력] 읽은 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * **이 파일에서 가장 중요한 함수다.** 두 가지 일을 한다.
 *
 * 첫째, 이 브리지의 config 공간은 표준 ECAM 창으로 읽을 수 없다.
 * PEM_CFG_RD 라는 레지스터에 읽고 싶은 오프셋을 64비트로 쓰고, 같은
 * 레지스터를 다시 읽으면 그 자리의 데이터가 **상위 32비트에 실려** 나온다.
 * 즉 한 번의 논리적 읽기가 MMIO 두 번이다. 게다가 32비트 단위로만 되므로
 * 오프셋의 하위 두 비트를 잘라 정렬한 뒤 읽고, 나중에 다시 밀어서 요청한
 * 폭만 꺼낸다.
 *
 * 둘째, 그렇게 읽어 온 값에 쓰레기가 섞여 있어 오프셋별로 덧칠한다.
 * 크게 두 부류다.
 *   (가) 실제 값을 고치는 것 — 0x40(capability 사슬에서 MSI 를 건너뛰게
 *        다음 포인터를 0x70 으로 바꿈), 0x70(PME 인터럽트 벡터가 0 으로
 *        읽히는 T88 에서 2 로 보정), 0xb0(MSI-X 테이블 크기 보정).
 *   (나) 아예 없는 것을 지어내는 것 — 0xb4/0xb8(MSI-X 테이블·PBA 위치),
 *        0xbc/0xc0/0xc4(EA capability 헤더와 항목 헤더),
 *        0xc8/0xcc/0xd0(thunder_pem_init() 이 계산해 둔 EA 항목 세 워드).
 *   즉 0xbc 부터 0xd0 까지는 하드웨어에서 읽은 값을 아예 버리고
 *   소프트웨어가 만든 값으로 대체한다.
 *
 * 0xb0 갈래 안에서 **또 한 번 PEM_CFG_RD 를 써서 0x70 을 읽는다.** 같은 칩의
 * 두 변종(T88 여부)을 그 자리에서 구별해야 MSI-X 테이블 크기를 옳게
 * 보고할 수 있기 때문이다. 그 결과 이 한 번의 config 읽기가 MMIO 네 번이 된다.
 * 바깥 읽기는 이미 데이터를 read_val 에 담아 둔 뒤라 래치를 덮어써도 안전하다.
 *
 * 실행 컨텍스트: **PCI 코어가 raw spinlock 인 pci_lock 을 쥔 채 부른다**
 * (drivers/pci/access.c:103, :313). 그래서 "주소 쓰기 -> 되읽기" 두 단계가
 * 다른 config 접근에 끼어들려 깨지지 않는다. 잠들면 안 된다.
 *
 * 에러 경로: devfn 이 0 이 아니거나 오프셋이 2048 이상이면
 * PCIBIOS_DEVICE_NOT_FOUND. 그 밖에는 실패가 없다 — 하드웨어 읽기 자체의
 * 실패를 감지할 방법이 없다.
 *
 * 호출 체인:
 *   pci_bus_read_config_ 계열 → bus->ops->read == thunder_pem_config_read()
 *     → [이 함수] → writeq()/readq()
 */
static int thunder_pem_bridge_read(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	/* [한국어] read_val 은 하드웨어에서 읽은 64비트 워드이자 덧칠 대상. tmp_val 은 0xb0 갈래에서
	 * 칩 변종을 알아보려고 0x70 을 곁다리로 읽을 때만 쓴다. **둘 다 64비트인 이유** 는
	 * PEM_CFG_RD 가 상위 32비트에 데이터를 싣기 때문이다. */
	u64 read_val, tmp_val;
	/* [한국어] PCI 코어는 버스마다 sysdata 에 드라이버가 정한 것을 매달아 둔다. ECAM 계층은
	 * 거기에 pci_config_window 를 넣으므로, 그것을 통해 이 창의 정보에 닿는다. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] 그 창의 priv 에 thunder_pem_init() 이 매달아 둔 이 드라이버의 상태를 되찾는다.
	 * void 포인터라 명시적 캐스트를 붙였다. */
	struct thunder_pem_pci *pem_pci = (struct thunder_pem_pci *)cfg->priv;

	/* [한국어] 브리지는 devfn 0 하나뿐이고, 이 창이 노출하는 config 공간은 2048바이트로 한정된다.
	 * 확장 config 공간 전체(4096)의 절반인데, PEM_CFG_RD 의 주소 필드 폭에서 온
	 * 제약으로 보이나 이 트리에서 확인 못 함. */
	if (devfn != 0 || where >= 2048)
		/* [한국어] 둘 중 하나라도 벗어나면 '장치 없음' 으로 답한다. 열거 중이라면 PCI 코어가
		 * 그 자리를 비어 있는 것으로 처리한다. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/*
	 * 32-bit accesses only.  Write the address to the low order
	 * bits of PEM_CFG_RD, then trigger the read by reading back.
	 * The config data lands in the upper 32-bits of PEM_CFG_RD.
	 */
	/* [한국어] 오프셋의 하위 두 비트를 잘라 4바이트 경계로 정렬한다. ~3ull 로 **64비트** 마스크를
	 * 쓰는 것이 중요하다 — 아래 writeq 가 64비트 값을 요구하기 때문이다. */
	read_val = where & ~3ull;
	/* [한국어] 주소를 래치에 쓴다. 이것만으로는 아직 데이터가 오지 않는다. */
	writeq(read_val, pem_pci->pem_reg_base + PEM_CFG_RD);
	/* [한국어] 같은 레지스터를 되읽으면 그 자리의 config 데이터가 상위 32비트에 실려 나온다.
	 * **읽기 한 번이 MMIO 두 번** 인 구조이며, 두 접근 사이가 pci_lock 으로 보호된다. */
	read_val = readq(pem_pci->pem_reg_base + PEM_CFG_RD);
	/* [한국어] 상위 32비트를 아래로 내려 실제 config 워드만 남긴다. */
	read_val >>= 32;

	/*
	 * The config space contains some garbage, fix it up.  Also
	 * synthesize an EA capability for the BAR used by MSI-X.
	 */
	/* [한국어] 정렬된 오프셋으로 갈라 오프셋별 덧칠을 한다. 여기서는 `& ~3` 라 int 연산이지만,
	 * 위 274 의 `& ~3ull` 와 값은 같다. */
	switch (where & ~3) {
	/* [한국어] capability 사슬의 한 마디 — */
	case 0x40:
		/* [한국어] 바이트 1(비트 15..8)을 지운다. 그 자리가 '다음 capability 오프셋' 이다. */
		read_val &= 0xffff00ff;
		/* [한국어] 그 자리에 0x70 을 넣어 사슬이 MSI capability 를 건너뛰고 PCIe capability 로
		 * 바로 가게 만든다. 이 브리지의 MSI 가 쓸 수 없는 상태이기 때문으로 보이나
		 * 그 하드웨어적 근거는 이 트리에서 확인 못 함. */
		read_val |= 0x00007000; /* Skip MSI CAP */
		break;
	/* [한국어] PCIe capability 헤더 — */
	case 0x70: /* Express Cap */
		/*
		 * Change PME interrupt to vector 2 on T88 where it
		 * reads as 0, else leave it alone.
		 */
		/* [한국어] 비트 29..25 는 이 dword 상위 절반(PCIe Capabilities 레지스터)의 비트 13..9,
		 * 곧 Interrupt Message Number 필드다. T88 에서는 이것이 0 으로 읽히는데,
		 * 0 은 '벡터 0' 을 뜻해 실제 배선과 어긋난다. */
		if (!(read_val & (0x1f << 25)))
			/* [한국어] 그 경우에만 2 로 채운다. 0 이 아니면 하드웨어가 옳은 값을 준 것이므로 건드리지 않는다. */
			read_val |= (2u << 25);
		break;
	/* [한국어] MSI-X capability 헤더 — 여기가 이 함수에서 가장 손이 많이 가는 갈래다. */
	case 0xb0: /* MSI-X Cap */
		/* TableSize=2 or 4, Next Cap is EA */
		/* [한국어] 비트 31(MSI-X Enable), 비트 30(Function Mask), 비트 7..0(capability ID)만 남기고
		 * 나머지(다음 capability 포인터와 Table Size)를 지운다. 그 둘을 아래에서 다시 채운다. */
		read_val &= 0xc00000ff;
		/*
		 * If Express Cap(0x70) raw PME vector reads as 0 we are on
		 * T88 and TableSize is reported as 4, else TableSize
		 * is 2.
		 */
		/* [한국어] **갈래 안에서 다시 config 를 읽는다.** 0x70 을 래치에 써서, */
		writeq(0x70, pem_pci->pem_reg_base + PEM_CFG_RD);
		/* [한국어] PCIe capability 헤더를 곁다리로 읽어 온다. 바깥 읽기는 이미 데이터를 read_val 에
		 * 담아 둔 뒤라 래치를 덮어써도 안전하다. 이 한 번의 config 읽기가 MMIO 네 번이 된다. */
		tmp_val = readq(pem_pci->pem_reg_base + PEM_CFG_RD);
		/* [한국어] 역시 상위 32비트를 내린다. */
		tmp_val >>= 32;
		/* [한국어] 위 293 과 같은 판정 — PME 벡터가 0 으로 읽히면 T88 이다. */
		if (!(tmp_val & (0x1f << 25)))
			/* [한국어] T88: 다음 capability 포인터를 0xbc(합성 EA)로, Table Size 필드를 3 으로 채운다.
			 * Table Size 는 'N-1' 로 인코딩되므로 3 은 항목 **4개** 를 뜻한다. */
			read_val |= 0x0003bc00;
		/* [한국어] T88 이 아니면 — */
		else
			/* [한국어] 같은 0xbc 포인터에 Table Size 를 1 로 채운다. 곧 항목 **2개** 다. */
			read_val |= 0x0001bc00;
		break;
	/* [한국어] MSI-X Table Offset / BIR — */
	case 0xb4:
		/* Table offset=0, BIR=0 */
		/* [한국어] 하드웨어 값을 버리고 0 으로 대체한다. 오프셋 0, BIR 0 —
		 * 곧 'BAR0 상당 영역의 맨 앞' 이라는 뜻이다. */
		read_val = 0x00000000;
		break;
	/* [한국어] MSI-X PBA(Pending Bit Array) Offset / BIR — */
	case 0xb8:
		/* BPA offset=0xf0000, BIR=0 */
		/* [한국어] 오프셋 0xf0000, BIR 0. 하위 세 비트가 BIR 이므로 0 이고 나머지가 오프셋이다. */
		read_val = 0x000f0000;
		break;
	/* [한국어] 여기서부터 0xd0 까지는 하드웨어에 **없는 것을 지어내는** 구간이다.
	 * EA(Enhanced Allocation) capability 헤더 — */
	case 0xbc:
		/* EA, 1 entry, no next Cap */
		/* [한국어] 바이트 0 = 0x14(EA capability ID), 바이트 1 = 0x00(다음 capability 없음),
		 * 바이트 2 = 0x01(항목 한 개). 사슬이 여기서 끝난다. */
		read_val = 0x00010014;
		break;
	/* [한국어] EA capability 의 두 번째 dword. type-1(브리지) 함수에서는 고정 secondary/
	 * subordinate 버스 번호가 들어가는 자리다 — */
	case 0xc0:
		/* DW2 for type-1 */
		/* [한국어] 둘 다 0 으로 둔다. */
		read_val = 0x00000000;
		break;
	/* [한국어] EA 항목의 헤더 dword — */
	case 0xc4:
		/* Entry BEI=0, PP=0x00, SP=0xff, ES=3 */
		/* [한국어] 상류 주석이 밝히는 그대로다: 비트 2..0 = ES(Entry Size) 3, 비트 7..4 = BEI 0,
		 * 비트 15..8 = PP(Primary Properties) 0x00, 비트 23..16 = SP(Secondary Properties) 0xff,
		 * 비트 31 = Enable. ES 가 3 이므로 헤더 뒤에 세 워드가 따라오며, 그것이
		 * thunder_pem_init() 이 계산해 둔 ea_entry[] 다. 각 필드의 규격상 정확한 의미는
		 * PCIe 기본 규격의 EA 절을 따르며 그 문서는 이 트리에 없다. */
		read_val = 0x80ff0003;
		break;
	/* [한국어] EA 항목의 첫 워드 — Base Low. */
	case 0xc8:
		/* [한국어] init 이 계산해 둔 값을 그대로 내보낸다. 하위 두 비트가 64비트 형식을 표시한다. */
		read_val = pem_pci->ea_entry[0];
		break;
	/* [한국어] EA 항목의 둘째 워드 — MaxOffset Low(영역 크기). */
	case 0xcc:
		/* [한국어] 역시 init 이 계산해 둔 값. */
		read_val = pem_pci->ea_entry[1];
		break;
	/* [한국어] EA 항목의 셋째 워드 — Base High. ES=3 이라 여기까지가 이 항목의 끝이다. */
	case 0xd0:
		/* [한국어] 물리 주소의 상위 32비트. */
		read_val = pem_pci->ea_entry[2];
		break;
	/* [한국어] 덧칠 대상이 아닌 모든 오프셋 — */
	default:
		/* [한국어] 하드웨어가 준 값을 그대로 쓴다. */
		break;
	}
	/* [한국어] 이제 요청한 오프셋의 하위 두 비트만큼 밀어, 정렬된 워드 안에서 원하는 바이트를
	 * 맨 아래로 내린다. **덧칠을 정렬된 상태에서 끝낸 뒤에 미는** 순서가 중요하다 —
	 * 먼저 밀었다면 위 case 라벨들과 마스크가 전부 어긋났을 것이다. */
	read_val >>= (8 * (where & 3));
	/* [한국어] 요청한 폭만 남기기 위해 갈라진다. */
	switch (size) {
	/* [한국어] 1바이트 요청 — */
	case 1:
		/* [한국어] 최하위 바이트만 남긴다. */
		read_val &= 0xff;
		break;
	/* [한국어] 2바이트 요청 — */
	case 2:
		/* [한국어] 최하위 두 바이트만 남긴다. */
		read_val &= 0xffff;
		break;
	/* [한국어] 4바이트 요청 — */
	default:
		/* [한국어] 자를 것이 없다. 아래 u32 대입에서 상위 32비트가 자연히 잘린다. */
		break;
	}
	/* [한국어] 64비트 read_val 을 u32 출력에 담는다. 위 마스크 덕에 요청한 폭만 유효하다. */
	*val = read_val;
	/* [한국어] 성공. PCIBIOS_ 계열 코드는 errno 가 아니라 PCI 접근 전용 반환 규약이다. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * thunder_pem_config_read - 브리지인지 하위 장치인지를 갈라 보낸다
 *
 * @bus: 접근 대상 버스.
 * @devfn: 장치/함수 번호.
 * @where: config 공간 오프셋.
 * @size: 1, 2, 4.
 * @val: [출력] 읽은 값.
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_DEVICE_NOT_FOUND, 또는 하위 호출의 결과.
 *
 * pci_ecam_ops.pci_ops.read 로 걸리는 함수이며, 이 파일이 PCI 코어에서
 * config 읽기를 넘겨받는 유일한 입구다. 하는 일은 갈래 나누기뿐이다.
 *
 *   담당 버스 범위 밖         -> PCIBIOS_DEVICE_NOT_FOUND
 *   버스 번호 == busr.start   -> thunder_pem_bridge_read()  (PEM 레지스터 창)
 *   그 밖                     -> pci_generic_config_read()  (표준 ECAM 창)
 *
 * 버스 범위 검사를 여기서 또 하는 이유가 있다. 표준 경로인
 * pci_generic_config_read() 는 map_bus 콜백(pci_ecam_map_bus, ecam.c:563)이
 * 같은 검사를 해 주지만, 브리지 갈래는 map_bus 를 **아예 지나지 않으므로**
 * 그 보호를 받지 못한다. 그래서 갈라지기 전에 미리 걸러야 한다.
 *
 * "버스의 첫 장치가 PEM 브리지" 라는 전제는 상류 주석이 그대로 밝히고 있다.
 * 그 브리지는 devfn 0 하나뿐이며, 그 검사는 thunder_pem_bridge_read() 안에 있다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 채. 잠들 수 없다.
 *
 * 에러 경로: 범위 밖이면 PCIBIOS_DEVICE_NOT_FOUND. 나머지는 하위 함수의
 * 반환값을 그대로 흘린다.
 *
 * 호출 체인:
 *   pci_bus_read_config_ 계열 → bus->ops->read == [이 함수]
 *     → thunder_pem_bridge_read() 또는 pci_generic_config_read()
 */
static int thunder_pem_config_read(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *val)
{
	/* [한국어] 이 창의 서술자. 아래 담당 버스 범위 검사에 쓴다. */
	struct pci_config_window *cfg = bus->sysdata;

	/* [한국어] 담당 버스 범위 밖인지 먼저 본다. **이 검사를 여기서 하는 이유** 는,
	 * 브리지 갈래가 map_bus 를 지나지 않아 pci_ecam_map_bus() 의 같은 검사
	 * (ecam.c:563)를 못 받기 때문이다. */
	if (bus->number < cfg->busr.start ||
	    bus->number > cfg->busr.end)
		/* [한국어] 범위 밖이면 장치 없음. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/*
	 * The first device on the bus is the PEM PCIe bridge.
	 * Special case its config access.
	 */
	/* [한국어] 이 창에서 가장 낮은 버스 번호가 곧 PEM 브리지 자신이다. */
	if (bus->number == cfg->busr.start)
		/* [한국어] 브리지는 PEM 레지스터 창을 통한 특수 경로로 보낸다. */
		return thunder_pem_bridge_read(bus, devfn, where, size, val);

	/* [한국어] 그 밖의 버스는 표준 ECAM 창으로 그대로 흘려보낸다. 그 안에서 map_bus 콜백
	 * (pci_ecam_map_bus)이 bus_shift 24 를 반영한 주소를 계산한다. */
	return pci_generic_config_read(bus, devfn, where, size, val);
}

/*
 * Some of the w1c_bits below also include read-only or non-writable
 * reserved bits, this makes the code simpler and is OK as the bits
 * are not affected by writing zeros to them.
 */
/* [한국어]
 * thunder_pem_bridge_w1c_bits - 이 오프셋에서 "1 을 쓰면 지워지는" 비트 마스크를 준다
 *
 * @where_aligned: 4바이트 정렬된 config 오프셋.
 * @return: 그 오프셋의 W1C(Write-1-to-Clear) 비트 마스크. 해당 없으면 0.
 *
 * 왜 필요한가: 이 하드웨어는 config 쓰기를 32비트로만 받는다. 그래서 1바이트나
 * 2바이트 쓰기 요청이 오면 주변을 읽어 합쳐 32비트로 넓혀야 하는데, 그
 * 과정에서 **건드릴 생각이 없던 W1C 비트에 1 이 실려 나갈 수 있다.**
 * 그러면 사용자가 요청하지도 않은 오류 상태가 조용히 지워진다. 이 함수가
 * 주는 마스크로 그 비트를 미리 0 으로 눌러 그 사고를 막는다.
 *
 * 돌려주는 값들은 PCI/PCIe 규격에서 상태 레지스터가 놓이는 자리들이다.
 *   0x04, 0x1c : 상위 바이트에 Status / Secondary Status 가 있어 0xff000000
 *   0x44       : 전원 관리 제어/상태 — 0xfffffe00
 *   0x78 ~ 0xa0: PCIe capability 안의 여러 Status 레지스터가 상위 16비트에
 *                있어 0xffff0000
 *   0x104~0x160: AER 등 확장 capability 의 상태 레지스터라 워드 전체가 0xffffffff
 *
 * 상류 주석이 밝히듯, 이 마스크에는 읽기 전용이거나 쓸 수 없는 예약 비트도
 * 섞여 있다. 그 비트에 0 을 쓰는 것은 무해하므로 마스크를 굳이 정밀하게
 * 깎지 않고 코드를 단순하게 유지했다.
 *
 * 실행 컨텍스트: config 쓰기 경로. pci_lock 아래이며 잠들지 않는다.
 * 순수 함수라 부작용도 상태도 없다.
 *
 * 에러 경로: 없다. 표에 없는 오프셋은 0 을 돌려주고, 호출자는 그것을
 * "보호할 W1C 비트가 없다" 로 읽는다.
 *
 * 호출 체인:
 *   thunder_pem_bridge_write() → [이 함수]
 */
static u32 thunder_pem_bridge_w1c_bits(u64 where_aligned)
{
	/* [한국어] 0 으로 초기화해 두면 default 갈래에서 따로 대입할 필요가 없다.
	 * 아래 w1_bits 함수는 초기화 없이 선언해 default 에서 0 을 넣는데, 결과는 같다. */
	u32 w1c_bits = 0;

	/* [한국어] 오프셋별로 갈린다. 여기 없는 오프셋에는 보호할 W1C 비트가 없다는 뜻이다. */
	switch (where_aligned) {
	/* [한국어] Command/Status. 상위 16비트가 Status 레지스터이며 그중 상위 바이트에
	 * 오류 상태 비트(Detected Parity Error 등)가 몰려 있다. */
	case 0x04: /* Command/Status */
	/* [한국어] I/O Base·Limit 와 Secondary Status. 역시 상위 바이트가 상태 비트다. */
	case 0x1c: /* Base and I/O Limit/Secondary Status */
		/* [한국어] 두 경우 모두 상위 바이트만 보호한다. */
		w1c_bits = 0xff000000;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] 전원 관리 제어/상태 레지스터. */
	case 0x44: /* Power Management Control and Status */
		/* [한국어] 비트 8 이상을 전부 보호한다 — 상위 16비트의 데이터 레지스터와 PME_Status 를
		 * 한꺼번에 덮는 넓은 마스크다. 상류 주석이 밝히듯 읽기 전용 비트가 섞여 있어도
		 * 0 을 쓰는 것은 무해하므로 마스크를 정밀하게 깎지 않았다. */
		w1c_bits = 0xfffffe00;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] Device Control(하위 16) / Device Status(상위 16). */
	case 0x78: /* Device Control/Device Status */
	/* [한국어] Link Control / Link Status. */
	case 0x80: /* Link Control/Link Status */
	/* [한국어] Slot Control / Slot Status. */
	case 0x88: /* Slot Control/Slot Status */
	/* [한국어] Root Status. */
	case 0x90: /* Root Status */
	/* [한국어] Link Control 2 / Link Status 2. */
	case 0xa0: /* Link Control 2 Registers/Link Status 2 */
		/* [한국어] 다섯 경우 모두 상위 16비트가 Status 쪽이므로 그 절반을 보호한다. */
		w1c_bits = 0xffff0000;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] AER 의 Uncorrectable Error Status. */
	case 0x104: /* Uncorrectable Error Status */
	/* [한국어] AER 의 Correctable Error Status. */
	case 0x110: /* Correctable Error Status */
	/* [한국어] Error Status. */
	case 0x130: /* Error Status */
	/* [한국어] Link Control 4. */
	case 0x160: /* Link Control 4 */
		/* [한국어] 네 경우 모두 워드 전체가 상태 비트라 32비트 전부를 보호한다. */
		w1c_bits = 0xffffffff;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] 표에 없는 오프셋 — */
	default:
		/* [한국어] 0 을 그대로 둔다. 호출자는 그것을 '보호할 것이 없다' 로 읽는다. */
		break;
	}
	/* [한국어] 마스크를 돌려준다. */
	return w1c_bits;
}

/* Some bits must be written to one so they appear to be read-only. */
/* [한국어]
 * thunder_pem_bridge_w1_bits - 이 오프셋에서 "항상 1 이어야 하는" 비트 마스크를 준다
 *
 * @where_aligned: 4바이트 정렬된 config 오프셋.
 * @return: 강제로 1 로 만들어야 하는 비트 마스크. 해당 없으면 0.
 *
 * 위 w1c 함수와 정반대의 문제를 다룬다. 규격상 **읽으면 항상 1 인 읽기 전용
 * 비트** 인데, 이 하드웨어의 접근 방식으로는 0 을 쓰면 실제로 0 이 되어 버린다.
 * 그래서 쓰기 직전에 그 비트를 강제로 세워 규격이 요구하는 모습을 유지한다.
 *
 * 두 자리뿐이다.
 *   0x1c : I/O Base / I/O Limit 의 각 하위 니블에 "I/O 주소 지정 폭" 이 들어
 *          있는데, 0x0101 은 두 바이트 모두의 비트 0 을 세워 **32비트 I/O
 *          주소 지정** 을 못박는다.
 *   0x24 : Prefetchable Memory Base / Limit 의 각 하위 니블도 같은 구조로,
 *          0x00010001 이 양쪽의 비트 0 을 세워 **64비트 주소 지정** 을 못박는다.
 * 각 비트가 규격의 어느 필드인지는 PCI-to-PCI Bridge 규격을 따르며, 그 문서는
 * 이 트리에 없다 — 위 해석은 상류 주석("Force 32-bit I/O addressing",
 * "Force 64-bit addressing")과 값의 비트 위치에서 온 것이다.
 *
 * w1c 쪽과 달리 default 갈래에서도 w1_bits 에 명시적으로 0 을 넣는다.
 * w1c 쪽은 선언에서 0 으로 초기화했기 때문에 생긴 차이일 뿐 동작은 같다.
 *
 * 실행 컨텍스트: config 쓰기 경로. pci_lock 아래이며 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   thunder_pem_bridge_write() → [이 함수]
 */
static u32 thunder_pem_bridge_w1_bits(u64 where_aligned)
{
	/* [한국어] **초기화 없이 선언한다.** 아래 모든 갈래(default 포함)가 반드시 대입하므로
	 * 안전하다. w1c 쪽과 스타일이 다를 뿐 동작은 같다. */
	u32 w1_bits;

	/* [한국어] 오프셋별로 갈린다. 강제할 자리가 두 곳뿐이다. */
	switch (where_aligned) {
	/* [한국어] I/O Base(바이트 0)와 I/O Limit(바이트 1)이 있는 dword — */
	case 0x1c: /* I/O Base / I/O Limit, Secondary Status */
		/* Force 32-bit I/O addressing. */
		/* [한국어] 두 바이트의 비트 0 을 각각 세운다. 그 비트가 'I/O 주소 지정 폭' 을 나타내며,
		 * 1 이면 32비트 주소 지정이다. 상류 주석의 "Force 32-bit I/O addressing" 이 그 뜻이다. */
		w1_bits = 0x0101;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] Prefetchable Memory Base(하위 16)와 Limit(상위 16)이 있는 dword — */
	case 0x24: /* Prefetchable Memory Base / Prefetchable Memory Limit */
		/* Force 64-bit addressing */
		/* [한국어] 양쪽 16비트 각각의 비트 0 을 세운다. 그 비트가 1 이면 64비트 주소 지정이다.
		 * 상류 주석의 "Force 64-bit addressing" 이 그 뜻이다. */
		w1_bits = 0x00010001;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] 그 밖의 오프셋 — */
	default:
		/* [한국어] 강제할 비트가 없다. */
		w1_bits = 0;
		/* [한국어] 이 갈래 끝. */
		break;
	}
	/* [한국어] 마스크를 돌려준다. 호출자가 이것을 쓸 값에 OR 로 얹는다. */
	return w1_bits;
}

/* [한국어]
 * thunder_pem_bridge_write - PEM 브리지 자신의 config 에 쓴다(32비트로 넓혀서)
 *
 * @bus: 접근 대상 버스.
 * @devfn: 장치/함수 번호. 브리지는 devfn 0 뿐이다.
 * @where: config 공간 바이트 오프셋.
 * @size: 1, 2, 4.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * thunder_pem_bridge_read() 의 쓰기 짝이다. 쓰기 창은 PEM_CFG_WR 이고,
 * 읽기와 달리 **한 번의 64비트 쓰기로 끝난다** — 하위 32비트에 주소,
 * 상위 32비트에 데이터를 담아 보낸다.
 *
 * 어려운 부분은 폭 맞추기다. 하드웨어가 32비트 쓰기만 받으므로 1바이트나
 * 2바이트 요청은 다음 순서로 넓힌다.
 *   1) PEM_CFG_RD 로 그 자리의 32비트를 먼저 읽는다(읽기 함수와 같은 두 단계).
 *   2) 바꾸지 않을 부분만 남기는 mask 를 만들어 읽은 값에 적용한다.
 *   3) 요청한 값을 제자리로 밀어 넣어 합친다.
 *   4) size 가 4 면 이 과정 자체가 없고 mask 는 0 으로 남는다.
 *
 * 그렇게 넓히면 새 문제가 둘 생기고, 각각을 뒤이은 두 블록이 처리한다.
 *   (가) 넓힌 32비트 안에 W1C 비트가 섞이면, 요청하지도 않은 상태 비트가
 *        지워진다. thunder_pem_bridge_w1c_bits() 로 그 자리를 알아내
 *        해당 비트를 0 으로 눌러 둔다. mask 가 0 인 4바이트 쓰기에서는
 *        이 블록을 통째로 건너뛴다 — 그 경우 호출자가 32비트 전체를 의도해
 *        썼으므로 손댈 이유가 없다.
 *   (나) 읽으면 항상 1 이어야 하는 비트가 0 으로 쓰이면 규격을 어긴다.
 *        thunder_pem_bridge_w1_bits() 가 주는 마스크를 OR 로 얹어 강제한다.
 *
 * [상류 코드 관찰] (가)에서 thunder_pem_bridge_w1c_bits(where) 로 **정렬되지
 * 않은 where** 를 넘긴다. 반면 (나)는 thunder_pem_bridge_w1_bits(where_aligned)
 * 로 정렬된 값을 넘긴다. w1c 함수의 파라미터 이름은 where_aligned 이고 그
 * switch 의 case 라벨도 전부 4바이트 정렬된 오프셋이므로, 예컨대 where 가
 * 0x05/0x06/0x07 인 1바이트 쓰기는 case 0x04 에 걸리지 않아 W1C 보호가
 * 적용되지 않는다. mask 가 0 이 아닌 경우(size 1 또는 2)에만 이 블록이
 * 도는데, 그것이 바로 where 가 정렬되지 않을 수 있는 경우와 겹친다.
 * 원본(1f0e418bb6:263, :276)에서 확인했으며 코드는 고치지 않았다.
 *
 * [상류 코드 관찰] mask 계산의 `0xff << (8 * (where & 3))` 와
 * `0xffff << (8 * (where & 3))` 는 상수가 int 타입이라, 시프트 양이 24(또는 16)
 * 일 때 결과가 int 범위를 넘는다. 실제 컴파일러들은 기대대로 동작하지만
 * 표준상 정의되지 않은 동작이다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: **pci_lock 을 쥔 채** 불린다. 그래서 1)의 "주소 쓰기 ->
 * 되읽기" 와 마지막 쓰기 사이에 다른 config 접근이 끼어들지 않는다.
 * 잠들면 안 된다.
 *
 * 에러 경로: devfn 이 0 이 아니거나 오프셋이 2048 이상이면
 * PCIBIOS_DEVICE_NOT_FOUND. 그 밖에는 실패가 없다.
 *
 * 호출 체인:
 *   pci_bus_write_config_ 계열 → bus->ops->write == thunder_pem_config_write()
 *     → [이 함수] → writeq()/readq()
 *     → thunder_pem_bridge_w1c_bits() → thunder_pem_bridge_w1_bits()
 */
static int thunder_pem_bridge_write(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 val)
{
	/* [한국어] 버스에 매달린 ECAM 창 서술자. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] 그 창의 priv 에서 이 드라이버 상태를 되찾는다. */
	struct thunder_pem_pci *pem_pci = (struct thunder_pem_pci *)cfg->priv;
	/* [한국어] write_val 은 PEM_CFG_WR 에 보낼 64비트 조합(상위=데이터, 하위=주소),
	 * read_val 은 폭을 넓힐 때 먼저 읽어 오는 32비트 값이다. */
	u64 write_val, read_val;
	/* [한국어] 4바이트 경계로 정렬한 오프셋. **읽기 함수와 달리 지역 변수로 따로 둔다** —
	 * 아래에서 여러 번 쓰이기 때문이다. */
	u64 where_aligned = where & ~3ull;
	/* [한국어] 0 으로 초기화하는 것이 중요하다. size 가 4 면 아래 switch 의 default 로 빠져
	 * mask 가 0 인 채 남고, 그것이 곧 '폭을 넓히지 않았다 = W1C 보호가 불필요하다'
	 * 는 표시로 쓰인다. */
	u32 mask = 0;


	/* [한국어] 읽기 쪽과 같은 검사 — 브리지는 devfn 0 뿐이고 공간은 2048바이트로 한정된다. */
	if (devfn != 0 || where >= 2048)
		/* [한국어] 벗어나면 장치 없음. */
		return PCIBIOS_DEVICE_NOT_FOUND;

	/*
	 * 32-bit accesses only.  If the write is for a size smaller
	 * than 32-bits, we must first read the 32-bit value and merge
	 * in the desired bits and then write the whole 32-bits back
	 * out.
	 */
	/* [한국어] 요청한 폭에 따라 갈린다. 하드웨어가 32비트 쓰기만 받으므로 1/2바이트는 넓혀야 한다. */
	switch (size) {
	/* [한국어] 1바이트 쓰기 — */
	case 1:
		/* [한국어] 먼저 그 자리를 읽어야 한다. 정렬된 오프셋을 래치에 쓰고, */
		writeq(where_aligned, pem_pci->pem_reg_base + PEM_CFG_RD);
		/* [한국어] 되읽어 데이터를 받는다. 읽기 함수와 똑같은 두 단계다. */
		read_val = readq(pem_pci->pem_reg_base + PEM_CFG_RD);
		/* [한국어] 상위 32비트를 내린다. */
		read_val >>= 32;
		/* [한국어] 바꿀 바이트만 0 이고 나머지가 1 인 마스크를 만든다.
		 * [상류 코드 관찰] 0xff 는 int 타입이라 (where & 3)이 3 이면 0xff << 24 가 int 범위를
		 * 넘는다. 실제 컴파일러는 기대대로 동작하지만 표준상 정의되지 않은 동작이다.
		 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		mask = ~(0xff << (8 * (where & 3)));
		/* [한국어] 읽은 값에서 바꿀 바이트 자리를 비운다. */
		read_val &= mask;
		/* [한국어] 쓸 값의 하위 바이트를 그 빈자리로 민다. */
		val = (val & 0xff) << (8 * (where & 3));
		/* [한국어] 둘을 합쳐 완전한 32비트 값을 만든다. */
		val |= (u32)read_val;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] 2바이트 쓰기 — 위와 폭만 다르고 절차가 같다. */
	case 2:
		/* [한국어] 정렬된 오프셋을 래치에 쓰고, */
		writeq(where_aligned, pem_pci->pem_reg_base + PEM_CFG_RD);
		/* [한국어] 되읽어 데이터를 받는다. */
		read_val = readq(pem_pci->pem_reg_base + PEM_CFG_RD);
		/* [한국어] 상위 32비트를 내린다. */
		read_val >>= 32;
		/* [한국어] 바꿀 두 바이트만 0 인 마스크. 여기도 0xffff 가 int 라 같은 오버플로 문제가 있다. */
		mask = ~(0xffff << (8 * (where & 3)));
		/* [한국어] 읽은 값에서 그 자리를 비운다. */
		read_val &= mask;
		/* [한국어] 쓸 값의 하위 두 바이트를 제자리로 민다. */
		val = (val & 0xffff) << (8 * (where & 3));
		/* [한국어] 둘을 합친다. */
		val |= (u32)read_val;
		/* [한국어] 이 갈래 끝. */
		break;
	/* [한국어] 4바이트 쓰기 — */
	default:
		/* [한국어] 넓힐 것이 없다. **mask 는 0 으로 남고** 아래 W1C 보호 블록을 통째로 건너뛴다. */
		break;
	}

	/*
	 * By expanding the write width to 32 bits, we may
	 * inadvertently hit some W1C bits that were not intended to
	 * be written.  Calculate the mask that must be applied to the
	 * data to be written to avoid these cases.
	 */
	/* [한국어] mask 가 0 이 아니라는 것은 폭을 넓혔다는 뜻이다. 그때만 W1C 보호가 필요하다. */
	if (mask) {
		/* [한국어] 이 오프셋에 W1C 비트가 있는지 묻는다.
		 * [상류 코드 관찰] **정렬되지 않은 where 를 넘긴다.** 그 함수의 파라미터 이름은
		 * where_aligned 이고 case 라벨도 모두 4바이트 정렬 오프셋이라, where 가
		 * 0x05/0x06/0x07 같은 값이면 case 0x04 에 걸리지 않아 보호가 적용되지 않는다.
		 * 아래 739 는 반대로 where_aligned 를 넘겨 두 호출의 인자 규약이 어긋나 있다.
		 * 원본(1f0e418bb6:263, :276)에서 확인했으며 코드는 고치지 않았다. */
		u32 w1c_bits = thunder_pem_bridge_w1c_bits(where);

		/* [한국어] 보호할 비트가 있으면 — */
		if (w1c_bits) {
			/* [한국어] '바꾸지 않을 자리' 와 'W1C 자리' 의 교집합을 구한다. 곧 의도치 않게 1 이 실릴 수 있는 자리다. */
			mask &= w1c_bits;
			/* [한국어] 그 자리를 쓸 값에서 0 으로 눌러, 실수로 상태 비트를 지우지 않게 한다. */
			val &= ~mask;
		}
	}

	/*
	 * Some bits must be read-only with value of one.  Since the
	 * access method allows these to be cleared if a zero is
	 * written, force them to one before writing.
	 */
	/* [한국어] 항상 1 이어야 하는 비트를 강제로 세운다. 이쪽은 **정렬된 오프셋** 을 넘긴다.
	 * OR 이라 위 W1C 처리로 눌러 둔 비트와 겹치지 않는 한 서로 간섭하지 않는다. */
	val |= thunder_pem_bridge_w1_bits(where_aligned);

	/*
	 * Low order bits are the config address, the high order 32
	 * bits are the data to be written.
	 */
	/* [한국어] 쓰기용 64비트 워드를 조립한다 — 상위 32비트에 데이터, 하위에 정렬된 주소.
	 * 주소가 4바이트 정렬이라 하위 두 비트가 0 이고, 데이터는 32비트를 통째로 밀어
	 * 올렸으므로 두 자리가 겹치지 않는다. 그래서 더하기 대신 OR 로 충분하다. */
	write_val = (((u64)val) << 32) | where_aligned;
	/* [한국어] 한 번의 64비트 쓰기로 config 쓰기가 끝난다. 읽기와 달리 되읽기 단계가 없다. */
	writeq(write_val, pem_pci->pem_reg_base + PEM_CFG_WR);
	/* [한국어] 성공. */
	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * thunder_pem_config_write - 브리지인지 하위 장치인지를 갈라 보낸다(쓰기 판)
 *
 * @bus: 접근 대상 버스.
 * @devfn: 장치/함수 번호.
 * @where: config 공간 오프셋.
 * @size: 1, 2, 4.
 * @val: 쓸 값.
 * @return: PCIBIOS_SUCCESSFUL / PCIBIOS_DEVICE_NOT_FOUND, 또는 하위 호출의 결과.
 *
 * thunder_pem_config_read() 와 같은 구조의 쓰기 판이다.
 *
 *   담당 버스 범위 밖         -> PCIBIOS_DEVICE_NOT_FOUND
 *   버스 번호 == busr.start   -> thunder_pem_bridge_write()  (PEM 레지스터 창)
 *   그 밖                     -> pci_generic_config_write()  (표준 ECAM 창)
 *
 * 읽기 쪽과 마찬가지로, 브리지 갈래가 map_bus 를 지나지 않으므로 버스 범위
 * 검사를 여기서 직접 해야 한다.
 *
 * 실행 컨텍스트: pci_lock 을 쥔 채. 잠들 수 없다.
 *
 * 에러 경로: 범위 밖이면 PCIBIOS_DEVICE_NOT_FOUND. 나머지는 하위 함수의
 * 반환값을 그대로 흘린다.
 *
 * 호출 체인:
 *   pci_bus_write_config_ 계열 → bus->ops->write == [이 함수]
 *     → thunder_pem_bridge_write() 또는 pci_generic_config_write()
 */
static int thunder_pem_config_write(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 val)
{
	/* [한국어] 이 창의 서술자. */
	struct pci_config_window *cfg = bus->sysdata;

	/* [한국어] 읽기 쪽과 같은 이유로 담당 버스 범위를 직접 검사한다 —
	 * 브리지 갈래가 map_bus 를 지나지 않기 때문이다. */
	if (bus->number < cfg->busr.start ||
	    bus->number > cfg->busr.end)
		/* [한국어] 범위 밖이면 장치 없음. */
		return PCIBIOS_DEVICE_NOT_FOUND;
	/*
	 * The first device on the bus is the PEM PCIe bridge.
	 * Special case its config access.
	 */
	/* [한국어] 가장 낮은 버스 번호가 PEM 브리지 자신이다. */
	if (bus->number == cfg->busr.start)
		/* [한국어] 브리지는 PEM 레지스터 창을 통한 특수 경로로 보낸다. */
		return thunder_pem_bridge_write(bus, devfn, where, size, val);


	/* [한국어] 그 밖의 버스는 표준 ECAM 쓰기로 흘려보낸다. */
	return pci_generic_config_write(bus, devfn, where, size, val);
}

/* [한국어]
 * thunder_pem_init - PEM 레지스터 창을 매핑하고 합성 EA 항목을 만든다
 *
 * @dev: devm 할당의 주인이 될 장치. 디바이스 트리 경로면 플랫폼 장치,
 *   ACPI 경로면 ACPI 장치다.
 * @cfg: ECAM 창 서술자. 이 함수가 cfg->priv 에 자기 상태를 매단다.
 * @res_pem: PEM 컨트롤러 레지스터 창의 물리 자원. **호출자가 어떻게 구했는지가
 *   두 진입 경로의 유일한 차이** 이며, 그 뒤는 이 함수로 합류한다.
 * @return: 0 = 성공. -ENOMEM = 할당 또는 매핑 실패.
 *
 * 두 진입 경로(디바이스 트리 / ACPI)가 만나는 지점이다. 하는 일이 셋이다.
 *   1) struct thunder_pem_pci 를 devm 으로 잡는다.
 *   2) res_pem 의 시작 주소부터 **64KB(0x10000)** 를 ioremap 한다. 그 안에
 *      PEM_CFG_WR(0x28)과 PEM_CFG_RD(0x30)가 있다.
 *   3) MSI-X 용 BAR 를 가리키는 EA 항목 세 워드를 계산해 담아 둔다.
 *      그 BAR 는 PEM 레지스터 창의 시작에서 **고정 오프셋 0xf00000** 에
 *      놓인다고 상류 주석이 밝힌다.
 *         ea_entry[0] = 하위 32비트 | 2   (하위 두 비트가 64비트 형식을 표시)
 *         ea_entry[1] = (창의 끝 - BAR 시작)의 하위 32비트에서 하위 두 비트를 뗀 값
 *         ea_entry[2] = 상위 32비트
 *      이 세 워드가 나중에 thunder_pem_bridge_read() 의 0xc8/0xcc/0xd0 갈래로
 *      그대로 흘러 나가 PCI 코어에 BAR 로 보인다.
 *   4) cfg->priv 에 매단다. 이후 모든 config 접근이 bus->sysdata -> cfg->priv 로
 *      이 상태를 되찾는다.
 *
 * **ioremap 은 64KB 인데 EA 계산은 res_pem->end 를 쓴다** 는 점이 중요하다.
 * 그래서 ACPI 경로의 thunder_pem_acpi_init() 은 매핑 크기와 무관하게
 * res_pem 의 크기를 16MB 로 되돌린 뒤 이 함수를 부른다. 그 순서를 어기면
 * ea_entry[1] 이 엉뚱한 값이 된다.
 *
 * [상류 코드 관찰] res_pem 의 크기가 0xf00000 보다 작으면
 * `res_pem->end - bar4_start` 가 언더플로해 ea_entry[1] 이 거대한 값이 된다.
 * ACPI 구형 펌웨어 경로는 그 앞에서 크기를 16MB 로 맞춰 두어 안전하지만,
 * acpi_get_rc_resources() 가 성공한 경로와 디바이스 트리 경로는 펌웨어/DT 가
 * 준 크기를 그대로 믿는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 호스트 브리지 probe 중의 프로세스 컨텍스트. devm_kzalloc 과
 * devm_ioremap 이 잠들 수 있다.
 *
 * 에러 경로: 할당과 매핑 실패 모두 -ENOMEM. 되감을 것은 devm 이 알아서 한다.
 * pci_ecam_create() 가 이 오류를 받으면 err_exit 로 가 ECAM 창까지 정리한다.
 *
 * 호출 체인:
 *   thunder_pem_platform_init() 또는 thunder_pem_acpi_init()
 *     → [이 함수] → devm_kzalloc() → devm_ioremap()
 */
static int thunder_pem_init(struct device *dev, struct pci_config_window *cfg,
			    struct resource *res_pem)
{
	/* [한국어] 이번 브리지의 상태를 담을 구조체. */
	struct thunder_pem_pci *pem_pci;
	/* [한국어] 합성 EA 항목이 가리킬 MSI-X BAR 의 시작 물리 주소.
	 * resource_size_t 라 32비트 커널에서도 64비트 물리 주소를 담을 수 있다. */
	resource_size_t bar4_start;

	/* [한국어] devm 으로 잡으므로 이 함수에 해제 코드가 없다. 프로브가 실패하거나 장치가
	 * 사라질 때 커널이 알아서 반납한다. */
	pem_pci = devm_kzalloc(dev, sizeof(*pem_pci), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!pem_pci)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] PEM 컨트롤러 레지스터 창을 **64KB(0x10000)만** 매핑한다. 실제로 접근하는
	 * PEM_CFG_WR(0x28)과 PEM_CFG_RD(0x30)가 그 안에 있기 때문이다.
	 * **res_pem 이 기술하는 크기(16MB)와 다르다는 점** 이 아래 EA 계산의 전제가 된다. */
	pem_pci->pem_reg_base = devm_ioremap(dev, res_pem->start, 0x10000);
	/* [한국어] 매핑 실패는 NULL 로 온다(ERR_PTR 가 아니다). */
	if (!pem_pci->pem_reg_base)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/*
	 * The MSI-X BAR for the PEM and AER interrupts is located at
	 * a fixed offset from the PEM register base.  Generate a
	 * fragment of the synthesized Enhanced Allocation capability
	 * structure here for the BAR.
	 */
	/* [한국어] MSI-X BAR 는 PEM 레지스터 창의 시작에서 **고정 오프셋 0xf00000** 에 놓인다고
	 * 상류 주석이 밝힌다. 그 근거 문서는 이 트리에 없다. */
	bar4_start = res_pem->start + 0xf00000;
	/* [한국어] Base Low. 하위 두 비트에 2(=0b10)를 얹어 '64비트 형식' 을 표시한다.
	 * BAR 주소가 최소 16바이트 정렬이라 하위 두 비트가 비어 있어 OR 로 넣어도 안전하다. */
	pem_pci->ea_entry[0] = lower_32_bits(bar4_start) | 2;
	/* [한국어] MaxOffset Low = (창의 끝 - BAR 시작). 16MB 창이면 0xFFFFF 가 되어 약 1MB 영역이 된다.
	 * 하위 두 비트를 떼는 것은 그 자리가 offset 값이 아니라 형식 표시 비트이기 때문이다(0 = 32비트).
	 * **res_pem->end 를 쓰므로 위 ioremap 크기(64KB)가 아니라 자원이 기술한 크기가 반영된다.** */
	pem_pci->ea_entry[1] = lower_32_bits(res_pem->end - bar4_start) & ~3u;
	/* [한국어] Base High. 물리 주소가 0x87e0... 대로 32비트를 넘으므로 이 워드가 반드시 필요하며,
	 * 그래서 EA 항목의 ES 가 3 인 것이다. */
	pem_pci->ea_entry[2] = upper_32_bits(bar4_start);

	/* [한국어] 완성한 상태를 ECAM 창에 매단다. 이 줄 뒤라야 config 접근 함수들이 cfg->priv 로
	 * 이 상태를 되찾을 수 있다. */
	cfg->priv = pem_pci;
	/* [한국어] 성공. */
	return 0;
}

/* [한국어] **ACPI 경로만의 블록.** 아래 상수 다섯과 함수 셋, 그리고 내보내는 ops 표가
 * 여기 들어간다. 파일 맨 위 조건이 이미 통과했더라도 디바이스 트리만 켜진
 * 빌드에서는 이 블록이 통째로 빠진다. */
#if defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS)

/* [한국어] 이 SoC 계열에서 PEM 컨트롤러 레지스터 창들이 놓이는 기준 물리 주소.
 * 구형 펌웨어가 자원을 기술하지 않을 때 손으로 주소를 만들기 위한 상수다.
 * ULL 접미사가 있어야 32비트 컴파일에서도 잘리지 않는다. */
#define PEM_RES_BASE		0x87e0c0000000ULL
/* [한국어] 기준 주소 안에서 NUMA 노드 번호가 들어갈 자리(비트 45..44). 노드 최대 4개. */
#define PEM_NODE_MASK		GENMASK_ULL(45, 44)
/* [한국어] 노드 안에서 몇 번째 PEM 인지가 들어갈 자리(비트 26..24).
 * 세 비트라 노드당 최대 8개까지 표현된다. */
#define PEM_INDX_MASK		GENMASK_ULL(26, 24)
/* [한국어] 세그먼트 번호가 노드 안에서 시작하는 값. 전역 세그먼트 번호에서 이만큼 빼야
 * 노드 안의 인덱스가 된다. */
#define PEM_MIN_DOM_IN_NODE	4
/* [한국어] 노드 하나가 소비하는 세그먼트 개수. 이름은 '최댓값' 처럼 읽히지만
 * 실제로는 **노드당 도메인 개수** 로 쓰인다 — 앞선 노드들이 소비한 몫을
 * 빼는 곱셈의 계수다. */
#define PEM_MAX_DOM_IN_NODE	10

/* [한국어]
 * thunder_pem_reserve_range - 이 세그먼트의 PEM 자원 구간을 iomem 트리에 등록한다
 *
 * @dev: 로그를 남길 장치.
 * @seg: PCI 세그먼트(도메인) 번호. 등록할 구간의 이름에 넣어 사람이 구별하게 한다.
 * @r: 등록할 구간. start 와 end 만 읽고 구조체 자체는 건드리지 않는다.
 * @return: 없음. 실패해도 조용히 넘어간다.
 *
 * ACPI 구형 펌웨어 경로에서만 쓴다. 펌웨어가 자원을 제대로 기술하지 못했으므로,
 * 커널이 스스로 계산한 구간을 iomem 자원 트리에 등록해 **다른 드라이버가
 * 같은 주소를 잡아 가지 않도록** 표시해 두는 것이다.
 *
 * 두 가지 세부가 눈여겨볼 만하다.
 *   (가) 등록에 성공하면 곧바로 IORESOURCE_BUSY 를 지운다. BUSY 인 채로 두면
 *        이 구간을 진짜로 쓰려는 드라이버(바로 아래에서 devm_ioremap 하는
 *        thunder_pem_init() 을 포함해)가 충돌로 거절당한다. 즉 "예약은 하되
 *        점유는 하지 않는다" 는 뜻이다.
 *   (나) 이름 문자열 regionid 는 성공 시 **일부러 해제하지 않는다.**
 *        request_mem_region() 이 그 포인터를 자원 구조체에 그대로 보관하기
 *        때문에, 해제하면 나중에 /proc/iomem 이 죽은 메모리를 읽는다.
 *        실패한 경우에만 kfree 한다.
 *
 * 실행 컨텍스트: 호스트 브리지 probe 중의 프로세스 컨텍스트.
 * kasprintf(GFP_KERNEL) 이 잠들 수 있다.
 *
 * 에러 경로: kasprintf 가 실패하면 아무것도 하지 않고 조용히 반환한다 —
 * 예약은 최선 노력일 뿐이라 실패해도 프로브를 접지 않는다. request 실패도
 * 마찬가지로 로그만 남기고 넘어간다(dev_info 가 "has been" 대신
 * "could not be" 를 찍는다).
 *
 * 호출 체인:
 *   thunder_pem_acpi_init() → [이 함수]
 *     → kasprintf() → request_mem_region() → dev_info()
 */
static void thunder_pem_reserve_range(struct device *dev, int seg,
				      struct resource *r)
{
	/* [한국어] 자원의 시작과 끝을 지역 변수로 복사해 둔다. 아래 request_mem_region 이
	 * 둘을 따로 요구하기 때문이며, 원본 구조체는 건드리지 않는다. */
	resource_size_t start = r->start, end = r->end;
	/* [한국어] 등록 결과. 성공하면 새로 만들어진 자원 노드를 가리킨다. */
	struct resource *res;
	/* [한국어] 등록할 구간의 이름. /proc/iomem 에 이 문자열이 보인다. */
	const char *regionid;

	/* [한국어] 세그먼트 번호를 넣어 사람이 어느 PEM 인지 구별할 수 있게 한다.
	 * GFP_KERNEL 이라 잠들 수 있다. */
	regionid = kasprintf(GFP_KERNEL, "PEM RC:%d", seg);
	/* [한국어] 이름 할당 실패 — */
	if (!regionid)
		/* [한국어] 조용히 반환한다. 예약은 최선 노력일 뿐이라 실패해도 프로브를 접지 않는다. */
		return;

	/* [한국어] iomem 자원 트리에 구간을 등록한다. 크기는 (end - start + 1) 로,
	 * 자원의 end 가 마지막 바이트를 가리키는 포함 표기이기 때문이다. */
	res = request_mem_region(start, end - start + 1, regionid);
	/* [한국어] 등록에 성공했다면 — */
	if (res)
		/* [한국어] **곧바로 BUSY 를 지운다.** BUSY 인 채로 두면 이 구간을 진짜로 쓰려는 쪽
		 * (바로 뒤 thunder_pem_init() 의 devm_ioremap 을 포함해)이 충돌로 거절당한다.
		 * 즉 '예약은 하되 점유는 하지 않는다'. */
		res->flags &= ~IORESOURCE_BUSY;
	/* [한국어] 등록에 실패했다면 — */
	else
		/* [한국어] 이름 문자열을 여기서만 해제한다. **성공한 경우에는 일부러 해제하지 않는다** —
		 * request_mem_region() 이 그 포인터를 자원 구조체에 그대로 보관하므로,
		 * 해제하면 나중에 /proc/iomem 이 죽은 메모리를 읽는다. */
		kfree(regionid);

	/* [한국어] 결과를 남긴다. res 가 NULL 인지에 따라 문구가 "has been"/"could not be" 로 갈려
	 * 한 줄로 성공과 실패를 모두 표현한다. %pR 은 자원 범위를 사람이 읽는 형식으로 찍는다. */
	dev_info(dev, "%pR %s reserved\n", r,
		 res ? "has been" : "could not be");
}

/* [한국어]
 * thunder_pem_legacy_fw - 구형 펌웨어를 위해 PEM 레지스터 창 주소를 손으로 계산한다
 *
 * @root: 이 루트 브리지의 ACPI 서술자. 세그먼트 번호와 ACPI 핸들을 여기서 얻는다.
 * @res_pem: [출력] 계산한 물리 주소를 담을 자원. start 와 flags 만 채우고
 *   **end 는 건드리지 않는다** — 크기는 호출자가 resource_set_size() 로 정한다.
 * @return: 없음. 실패 개념이 없다.
 *
 * acpi_get_rc_resources() 가 실패하는 구형 펌웨어를 위한 우회다. 그런 펌웨어는
 * PEM 컨트롤러 레지스터 창을 ACPI 로 기술하지 않으므로, 커널이 하드웨어
 * 주소 배치 규칙을 그대로 코드에 박아 두고 계산한다.
 *
 * 주소 조립이 세 조각이다.
 *   PEM_RES_BASE(0x87e0c0000000)  : 이 SoC 계열의 PEM 창 기준 주소.
 *   PEM_NODE_MASK(비트 45..44)    : NUMA 노드 번호가 들어가는 자리.
 *   PEM_INDX_MASK(비트 26..24)    : 노드 안에서 몇 번째 PEM 인지.
 * 세 조각을 OR 로 합쳐 res_pem->start 를 만든다. 자리가 겹치지 않으므로
 * 더하기 대신 OR 로 충분하다.
 *
 * 인덱스 계산이 두 단계인 이유: 세그먼트 번호가 전역으로 매겨지므로,
 * 먼저 노드 안에서의 시작 번호(PEM_MIN_DOM_IN_NODE, 4)를 빼고,
 * 다시 앞선 노드들이 소비한 몫(node x PEM_MAX_DOM_IN_NODE, 10)을 뺀다.
 * 즉 PEM_MAX_DOM_IN_NODE 는 "최댓값" 이라기보다 **노드당 도메인 개수** 로
 * 쓰인다 — 이름과 쓰임이 다소 어긋난다.
 *
 * [상류 코드 관찰] 계산한 index 의 범위를 검사하지 않는다. 세그먼트 번호가
 * 예상 범위를 벗어나면 index 가 음수가 되거나 3비트를 넘고, FIELD_PREP 이
 * 그것을 마스크로 잘라 **조용히 엉뚱한 물리 주소** 를 만든다. 그 주소는
 * 이후 request_mem_region 과 ioremap 을 그대로 통과할 수 있다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 호스트 브리지 probe 중의 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. acpi_get_node() 가 NUMA_NO_NODE 를 주면 0 으로 대체할 뿐이다.
 *
 * 호출 체인:
 *   thunder_pem_acpi_init() → [이 함수] → acpi_get_node() → FIELD_PREP()
 */
static void thunder_pem_legacy_fw(struct acpi_pci_root *root,
				 struct resource *res_pem)
{
	/* [한국어] 이 루트 브리지가 어느 NUMA 노드에 속하는지 ACPI 핸들로 알아낸다.
	 * 노드 번호가 곧 물리 주소의 한 조각이 된다. */
	int node = acpi_get_node(root->device->handle);
	/* [한국어] 노드 안에서 몇 번째 PEM 인지. 아래에서 두 단계로 계산한다. */
	int index;

	/* [한국어] 펌웨어가 근접성 정보를 주지 않으면 — */
	if (node == NUMA_NO_NODE)
		/* [한국어] 0 번 노드로 가정한다. 단일 노드 장비에서는 이것이 옳다. */
		node = 0;

	/* [한국어] 먼저 노드 안에서의 시작 번호를 뺀다. */
	index = root->segment - PEM_MIN_DOM_IN_NODE;
	/* [한국어] 다시 앞선 노드들이 소비한 몫을 뺀다. 여기서 PEM_MAX_DOM_IN_NODE 가
	 * '노드당 도메인 개수' 로 쓰인다.
	 * [상류 코드 관찰] 결과 index 의 범위를 검사하지 않는다. 세그먼트 번호가
	 * 예상 범위를 벗어나면 index 가 음수가 되거나 3비트를 넘고, 아래 FIELD_PREP 이
	 * 그것을 마스크로 잘라 조용히 엉뚱한 물리 주소를 만든다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	index -= node * PEM_MAX_DOM_IN_NODE;
	/* [한국어] 기준 주소에 노드 번호와 인덱스를 각자의 자리로 밀어 넣어 합친다.
	 * 세 조각의 비트 자리가 겹치지 않으므로 더하기 대신 OR 로 충분하다. */
	res_pem->start = PEM_RES_BASE | FIELD_PREP(PEM_NODE_MASK, node) |
					FIELD_PREP(PEM_INDX_MASK, index);
	/* [한국어] 메모리 자원임을 표시한다. **end 는 채우지 않는다** — 크기는 호출자가
	 * resource_set_size() 로 정하며, 그것이 64KB/16MB 를 오가는 이유다. */
	res_pem->flags = IORESOURCE_MEM;
}

/* [한국어]
 * thunder_pem_acpi_init - ACPI 경로에서 PEM 레지스터 창을 확보해 공통 init 으로 넘긴다
 *
 * @cfg: ECAM 창 서술자. cfg->parent 가 ACPI 장치이며, cfg->res 는 ECAM config
 *   창 자신의 자원이다.
 * @return: 0 = 성공. -ENOMEM 또는 thunder_pem_init() 의 오류.
 *
 * pci_ecam_ops.init 훅의 ACPI 판이다. ARM64 의 MCFG 쿼크 표가 이 파일의
 * thunder_pem_ecam_ops 를 골라 주면 pci_ecam_create() 안에서 불린다.
 * 그 쿼크 표는 arch/arm64 아래에 있어 이 트리에서는 확인 못 함.
 *
 * 두 갈래로 갈린다.
 *   (가) 정상 펌웨어: acpi_get_rc_resources(dev, "CAVA02B", segment, res_pem)
 *        이 성공한다. "CAVA02B" 는 이 컨트롤러의 ACPI _HID 이며, 그 함수가
 *        같은 _HID 를 가진 노드들 중 세그먼트가 일치하는 것을 찾아 _CRS 의
 *        첫 메모리 자원을 준다(drivers/pci/pci-acpi.c:359). 그대로 다음으로 간다.
 *   (나) 구형 펌웨어: 실패하면 thunder_pem_legacy_fw() 로 주소를 계산한 뒤,
 *        **크기를 두 번 바꿔 가며** 예약과 초기화를 한다.
 *          - 64KB 로 줄여 예약한다. 실제로 접근하는 레지스터가 그 안에만
 *            있으므로, 16MB 를 통째로 예약해 다른 드라이버를 막을 이유가 없다.
 *          - ECAM config 창(cfg->res)도 함께 예약한다. 구형 펌웨어에서는
 *            그것 역시 기술되지 않았기 때문이다.
 *          - 다시 16MB 로 되돌린다. thunder_pem_init() 의 EA 항목 계산이
 *            res_pem->end 를 쓰기 때문이며, 상류 주석이 그 이유를 명시한다.
 *
 * resource_set_size() 의 정의는 이 트리에 없다(include/linux/ioport.h).
 * 이름과 이 파일·drivers/pci/ecam.c:241 의 쓰임으로 보아 start 를 유지한 채
 * 크기를 지정한 값으로 맞춘다.
 *
 * 실행 컨텍스트: 호스트 브리지 probe 중의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 에러 경로: res_pem 할당 실패만 -ENOMEM 으로 반환한다. 예약 실패는 오류로
 * 치지 않고 로그만 남긴다 — 예약은 최선 노력이기 때문이다.
 * 마지막으로 thunder_pem_init() 의 결과를 그대로 돌려준다.
 *
 * 호출 체인:
 *   ARM64 MCFG 쿼크 → pci_ecam_create() → ops->init == [이 함수]
 *     → acpi_get_rc_resources() → (실패 시) thunder_pem_legacy_fw()
 *     → thunder_pem_reserve_range() x2 → thunder_pem_init()
 */
static int thunder_pem_acpi_init(struct pci_config_window *cfg)
{
	/* [한국어] ECAM 창을 만든 쪽의 장치. ACPI 경로에서는 ACPI 장치다. */
	struct device *dev = cfg->parent;
	/* [한국어] 그것을 acpi_device 로 되돌린다. */
	struct acpi_device *adev = to_acpi_device(dev);
	/* [한국어] 거기 매달린 acpi_pci_root 를 꺼낸다. 세그먼트 번호와 ACPI 핸들이 여기 있다. */
	struct acpi_pci_root *root = acpi_driver_data(adev);
	/* [한국어] PEM 레지스터 창을 담을 자원. */
	struct resource *res_pem;
	/* [한국어] acpi_get_rc_resources() 의 결과. */
	int ret;

	/* [한국어] 자원 구조체를 devm 으로 잡는다. **주인이 dev 가 아니라 &adev->dev 인데**
	 * 둘은 같은 장치이며(위 1135 에서 되돌린 것), 여기서는 acpi_device 쪽 표기를 썼다. */
	res_pem = devm_kzalloc(&adev->dev, sizeof(*res_pem), GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!res_pem)
		/* [한국어] 메모리 부족. */
		return -ENOMEM;

	/* [한국어] _HID 가 "CAVA02B" 이고 세그먼트가 일치하는 ACPI 노드를 찾아 그 _CRS 의 첫
	 * 메모리 자원을 가져온다(drivers/pci/pci-acpi.c:359). 그 함수의 주석이 이 파일을
	 * 유일한 호출자로 지목할 만큼 서로를 위해 만들어진 짝이다. */
	ret = acpi_get_rc_resources(dev, "CAVA02B", root->segment, res_pem);

	/*
	 * If we fail to gather resources it means that we run with old
	 * FW where we need to calculate PEM-specific resources manually.
	 */
	/* [한국어] 실패했다 — 자원을 기술하지 않는 구형 펌웨어다. */
	if (ret) {
		/* [한국어] 주소를 손으로 계산한다. 이 호출은 start 와 flags 만 채우고 end 는 남겨 둔다. */
		thunder_pem_legacy_fw(root, res_pem);
		/*
		 * Reserve 64K size PEM specific resources. The full 16M range
		 * size is required for thunder_pem_init() call.
		 */
		/* [한국어] 크기를 64KB 로 정한다. 실제로 접근하는 레지스터가 그 안에만 있으므로,
		 * 16MB 를 통째로 예약해 다른 드라이버를 막을 이유가 없다. */
		resource_set_size(res_pem, SZ_64K);
		/* [한국어] 그 64KB 를 iomem 트리에 예약한다. */
		thunder_pem_reserve_range(dev, root->segment, res_pem);
		/* [한국어] **다시 16MB 로 되돌린다.** 아래 thunder_pem_init() 의 EA 항목 계산이
		 * res_pem->end 를 쓰기 때문이며, 상류 주석이 그 이유를 명시한다.
		 * resource_set_size() 의 정의는 이 트리에 없다(include/linux/ioport.h) —
		 * 이름과 drivers/pci/ecam.c:241 의 쓰임으로 보아 start 를 유지한 채 크기를 맞춘다. */
		resource_set_size(res_pem, SZ_16M);

		/* Reserve PCI configuration space as well. */
		/* [한국어] ECAM config 창 자신도 예약한다. 구형 펌웨어에서는 그것 역시 기술되지 않았다. */
		thunder_pem_reserve_range(dev, root->segment, &cfg->res);
	}

	/* [한국어] 두 경로가 여기서 합류한다. 확보한 자원으로 공통 초기화를 하고 그 결과를 그대로 돌려준다. */
	return thunder_pem_init(dev, cfg, res_pem);
}

/* [한국어] **ACPI 경로가 쓰는 ops 표. static 이 아니라 파일 밖으로 내보낸다** —
 * ARM64 의 MCFG 쿼크 표가 특정 OEM ID 에 이 표를 묶어 두기 때문이다.
 * 그 표는 arch/arm64 아래에 있어 이 트리에서는 확인 못 함.
 * 설정자: 컴파일 시점 초기화자. const 라 불변.
 * 읽는 자: MCFG 처리와 pci_ecam_create(), 그리고 그 뒤 모든 config 접근.
 * 값 범위: 아래 필드 조합 그대로.
 * 동기화: 읽기 전용 상수. */
const struct pci_ecam_ops thunder_pem_ecam_ops = {
	/* [한국어] 버스 번호를 24비트 자리로 민다. 표준(20)이 아닌 플랫폼 고유값이다. */
	.bus_shift	= THUNDER_PCIE_ECAM_BUS_SHIFT,
	/* [한국어] ECAM 창을 만든 뒤 불릴 초기화 훅. 여기서 PEM 레지스터 창을 확보한다. */
	.init		= thunder_pem_acpi_init,
	/* [한국어] PCI 코어가 config 접근에 쓸 함수 셋. */
	.pci_ops	= {
		/* [한국어] 주소 계산은 ECAM 공통 함수를 그대로 쓴다. **브리지 자신의 접근은 이 콜백을
		 * 지나지 않는다** — read/write 가 그 전에 갈라내기 때문이다. */
		.map_bus	= pci_ecam_map_bus,
		/* [한국어] 읽기는 이 파일의 갈래 나누기 함수로. */
		.read		= thunder_pem_config_read,
		/* [한국어] 쓰기도 마찬가지. */
		.write		= thunder_pem_config_write,
	}
};

/* [한국어] ACPI 전용 블록의 끝. */
#endif

/* [한국어] **디바이스 트리 경로만의 블록.** 아래 init 훅, ops 표, match 표, 플랫폼
 * 드라이버가 여기 들어간다. */
#ifdef CONFIG_PCI_HOST_THUNDER_PEM

/* [한국어]
 * thunder_pem_platform_init - 디바이스 트리 경로에서 두 번째 reg 자원을 찾아 공통 init 으로 넘긴다
 *
 * @cfg: ECAM 창 서술자. cfg->parent 가 플랫폼 장치다.
 * @return: 0 = 성공. -EINVAL = 디바이스 트리 노드가 없거나 reg[1] 이 없음.
 *   그 밖은 thunder_pem_init() 의 오류.
 *
 * pci_ecam_ops.init 훅의 디바이스 트리 판이다. ACPI 판과 달리 할 일이
 * 아주 단순하다 — 디바이스 트리의 **두 번째** reg 항목이 곧 PEM 컨트롤러
 * 레지스터 창이기 때문이다. 첫 번째 reg 는 ECAM config 창이며 그것은
 * pci_ecam_create() 가 이미 처리했다.
 *
 * of_node 검사가 먼저 오는 이유: 같은 파일 안에 ACPI 진입점이 따로 있으므로,
 * 이 함수가 ACPI 장치에 대해 불릴 일은 없어야 한다. 그 전제가 깨지면
 * platform_get_resource() 가 엉뚱한 것을 줄 수 있어 미리 막는다.
 *
 * 상류 주석이 밝히듯, 이 두 번째 창이 필요한 이유는 브리지가 그 아래 장치들과
 * **다른 config 접근 방식** 을 쓰기 때문이다 — 그것이 이 파일 전체의 존재 이유다.
 *
 * 실행 컨텍스트: 호스트 브리지 probe 중의 프로세스 컨텍스트.
 *
 * 에러 경로: 두 검사 모두 -EINVAL. reg[1] 이 없을 때만 dev_err 로 무엇이
 * 빠졌는지 남긴다 — of_node 가 없는 경우는 애초에 일어나면 안 되는 상황이라
 * 로그가 없다.
 *
 * 호출 체인:
 *   pci_host_common_probe() → pci_host_common_init() → pci_ecam_create()
 *     → ops->init == [이 함수] → platform_get_resource() → thunder_pem_init()
 */
static int thunder_pem_platform_init(struct pci_config_window *cfg)
{
	/* [한국어] ECAM 창을 만든 쪽의 장치. 디바이스 트리 경로에서는 플랫폼 장치다. */
	struct device *dev = cfg->parent;
	/* [한국어] platform_get_resource() 가 struct platform_device 를 요구하므로 되돌린다. */
	struct platform_device *pdev = to_platform_device(dev);
	/* [한국어] 두 번째 reg 자원을 담을 포인터. 여기서는 새로 할당하지 않고
	 * 플랫폼 장치가 이미 들고 있는 자원 배열을 가리킬 뿐이다. */
	struct resource *res_pem;

	/* [한국어] 디바이스 트리 노드가 없다면 — */
	if (!dev->of_node)
		/* [한국어] 이 함수가 불릴 상황이 아니다. 같은 파일에 ACPI 진입점이 따로 있으므로,
		 * 그 전제가 깨진 채 platform_get_resource() 를 부르면 엉뚱한 자원을 얻을 수 있다. */
		return -EINVAL;

	/*
	 * The second register range is the PEM bridge to the PCIe
	 * bus.  It has a different config access method than those
	 * devices behind the bridge.
	 */
	/* [한국어] **두 번째** 메모리 자원(reg[1])이 PEM 컨트롤러 레지스터 창이다.
	 * 첫 번째(reg[0])는 ECAM config 창이며 pci_ecam_create() 가 이미 처리했다. */
	res_pem = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	/* [한국어] reg[1] 이 없다 — 디바이스 트리가 불완전하다. */
	if (!res_pem) {
		/* [한국어] 무엇이 빠졌는지 남긴다. 위 of_node 검사와 달리 이쪽은 실제로 일어날 수 있는
		 * 설정 오류라 로그가 있다. */
		dev_err(dev, "missing \"reg[1]\"property\n");
		/* [한국어] 잘못된 인자로 거절한다. */
		return -EINVAL;
	}

	/* [한국어] ACPI 경로와 같은 공통 초기화로 합류한다. */
	return thunder_pem_init(dev, cfg, res_pem);
}

/* [한국어] 디바이스 트리 경로가 쓰는 ops 표. init 훅만 다르고 나머지는 위 ACPI 판과 같다.
 * 설정자: 컴파일 시점 초기화자. static const 라 불변.
 * 읽는 자: pci_host_common_probe() 가 of_device_get_match_data() 로 얻어
 *   pci_ecam_create() 에 넘긴다.
 * 값 범위: 아래 필드 조합 그대로.
 * 동기화: 읽기 전용 상수. */
static const struct pci_ecam_ops pci_thunder_pem_ops = {
	/* [한국어] 같은 비표준 bus_shift 24. */
	.bus_shift	= THUNDER_PCIE_ECAM_BUS_SHIFT,
	/* [한국어] **여기만 ACPI 판과 다르다** — 디바이스 트리에서 reg[1] 을 찾는 훅이다. */
	.init		= thunder_pem_platform_init,
	/* [한국어] config 접근 함수 셋은 두 경로가 완전히 같다. */
	.pci_ops	= {
		/* [한국어] 주소 계산은 ECAM 공통 함수. */
		.map_bus	= pci_ecam_map_bus,
		/* [한국어] 읽기 갈래 나누기. */
		.read		= thunder_pem_config_read,
		/* [한국어] 쓰기 갈래 나누기. */
		.write		= thunder_pem_config_write,
	}
};

/* [한국어] 디바이스 트리 매칭 표.
 * 설정자: 컴파일 시점 초기화자. static const 라 불변.
 * 읽는 자: 드라이버 코어의 매칭 로직과 of_device_get_match_data().
 * 값 범위: 항목 하나와 종료 표시 하나.
 * 동기화: 읽기 전용 상수. */
static const struct of_device_id thunder_pem_of_match[] = {
	{
		/* [한국어] 이 컨트롤러의 compatible 문자열. */
		.compatible = "cavium,pci-host-thunder-pem",
		/* [한국어] 이 노드를 만나면 위 ops 표를 쓰라는 표시. pci_host_common_probe() 가
		 * 이것을 꺼내 쓰기 때문에 이 드라이버에는 자기 probe 함수가 아예 없다. */
		.data = &pci_thunder_pem_ops,
	},
	/* [한국어] 표의 끝. MODULE_DEVICE_TABLE 이 없는 것은 이 드라이버가 빌트인 전용이기 때문이다. */
	{ },
};

/* [한국어] 플랫폼 드라이버 서술자.
 * 설정자: 컴파일 시점 초기화자. const 가 아닌 것은 드라이버 코어가 등록 과정에서
 *   내부 필드를 갱신하기 때문이다.
 * 읽는 자: 드라이버 코어(platform_driver_register).
 * 값 범위: 아래 필드 조합 그대로.
 * 동기화: 등록 이후 코어가 관리한다. */
static struct platform_driver thunder_pem_driver = {
	.driver = {
		/* [한국어] 드라이버 이름을 소스 파일 이름에서 자동으로 가져온다 —
		 * 곧 "pci-thunder-pem" 이 된다. */
		.name = KBUILD_MODNAME,
		/* [한국어] 위 매칭 표를 건다. */
		.of_match_table = thunder_pem_of_match,
		/* [한국어] sysfs 의 bind/unbind 속성을 만들지 않는다. 손으로 언바인드할 방법을 막는
		 * 것으로, 호스트 브리지를 실행 중에 떼어 내는 것이 안전하지 않기 때문이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] **자기 probe 함수를 두지 않고 공통 함수를 그대로 건다.** SoC 별 차이가
	 * ops 표 하나에 모여 있어, 프로브 절차 자체는 나눠 쓸 수 있다. */
	.probe = pci_host_common_probe,
};
/* [한국어] 모듈이 아니라 빌트인 전용으로 등록한다. 이 파일에 MODULE_ 계열 매크로가
 * 하나도 없는 이유이기도 하다. */
builtin_platform_driver(thunder_pem_driver);

/* [한국어] 디바이스 트리 전용 블록의 끝. */
#endif
/* [한국어] 파일 맨 위(161줄)에서 연 전체 조건의 끝. 두 #endif 가 연달아 오는 것은
 * 바깥 조건과 안쪽 블록을 각각 닫기 때문이다. */
#endif
