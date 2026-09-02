// SPDX-License-Identifier: GPL-2.0
/*
 * Loongson PCI Host Controller Driver
 *
 * Copyright (C) 2020 Jiaxun Yang <jiaxun.yang@flygoat.com>
 */

/*
 * [한국어 설명] Loongson PCI 호스트 컨트롤러 드라이버 (pci-loongson.c)
 *
 * === 파일의 역할 ===
 * Loongson LS2K/LS7A/RS780E 계열의 PCI 호스트 브리지 드라이버다.
 * 그런데 **이 파일의 절반 이상이 호스트 브리지 코드가 아니라 quirk(하드웨어
 * 결함 보정) 코드** 라는 점이 이 파일의 성격을 규정한다.
 *
 * 그래서 두 층으로 나누어 읽어야 한다.
 *
 *   [1층] config 공간 접근 — 이 칩셋의 config 주소 형식은 표준 ECAM 과
 *         다르다. cfg0_map()/cfg1_map() 두 함수가 그 형식을 만든다.
 *         진입점이 둘인데, 디바이스 트리로 부팅하면 플랫폼 드라이버로,
 *         ACPI 로 부팅하면 ECAM ops 로 들어온다. 두 경로가
 *         pci_loongson_map_bus() 에서 합류한다.
 *
 *   [2층] quirk 여섯 벌 — 이 칩셋의 하드웨어·펌웨어 결함을 PCI 코어가
 *         장치를 열거할 때 보정한다. 아래에 따로 정리한다.
 *
 * === quirk 여섯 벌 ===
 * 1) bridge_class_quirk    : PCIe 포트 세 개가 잘못된 클래스 코드를 보고해,
 *                            정상 브리지로 고쳐 준다.
 * 2) system_bus_quirk      : 브리지 자원 밖의 주소를 쓰는 장치 셋에
 *                            "BAR 를 믿지 말라" 고 표시한다.
 * 3) loongson_mrrs_quirk   : 포트 여덟 개에 "MRRS 를 올리지 말라" 고 건다.
 * 4) loongson_set_min_mrrs_quirk : MIPS 판에서만 컴파일되며, 펌웨어가
 *                            MRRS 를 제대로 두지 않은 경우 256바이트로 낮춘다.
 * 5) loongson_pci_pin_quirk: 장치 일곱 개의 INTx 핀 번호를 기능 번호에서
 *                            직접 계산해 덮어쓴다.
 * 6) loongson_pci_msi_quirk: 포트 하나의 MSI 활성 비트를 손수 세운다.
 *
 * === MRRS 이야기 (이 파일에서 가장 중요한 quirk) ===
 * MRRS(Maximum Read Request Size)는 장치가 한 번에 요청할 수 있는 읽기
 * 크기다. 옆의 상류 주석이 밝히듯, 일부 Loongson PCIe 포트는 이 값이
 * 커지면 감당하지 못한다.
 *
 * 대응이 두 갈래다.
 *   - no_inc_mrrs 표시(3번): PCI 코어의 pcie_set_readrq() 가 이 표시를 보고
 *     **현재 값보다 큰 값으로 올리는 것을 거부한다**
 *     (drivers/pci/pci.c:11759). 즉 펌웨어가 정해 둔 값을 그대로 지킨다.
 *   - 강제 하향(4번): MIPS 판 펌웨어는 그 값을 제대로 두지 않으므로,
 *     장치가 활성화될 때 256바이트를 넘으면 낮춘다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 트리 경로:
 *   플랫폼 드라이버 코어 -> loongson_pci_probe()
 *     -> 매칭 데이터로 CFG0/CFG1 창을 잡는다
 *     -> pci_host_probe() -> 버스 스캔
 *        -> config 접근마다 -> [이 파일] pci_loongson_map_bus()
 *        -> 장치를 찾을 때마다 -> [이 파일] quirk 들
 *
 * ACPI 경로:
 *   ACPI PCI 코어 -> loongson_pci_ecam_ops.init == loongson_pci_ecam_init()
 *     -> 상태 구조체를 만들어 pci_config_window 에 매단다
 *     -> config 접근마다 -> [이 파일] pci_loongson_map_bus()
 *
 * 두 경로의 차이는 **상태 구조체를 어디에 매다는가** 뿐이며,
 * pci_bus_to_loongson_pci() 가 그 차이를 흡수한다.
 *
 * 실행 컨텍스트: quirk 는 PCI 코어의 열거 경로에서, map_bus 는 config 접근
 * 경로에서 돈다. 둘 다 프로세스 컨텍스트이며 인터럽트 핸들러는 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어. quirk 는 DECLARE_PCI_FIXUP_ 계열 매크로로 코어의 고정
 *   지점에 등록되고, 브리지는 pci_host_probe() 로 넘긴다.
 * 아래쪽: 없다. 이 파일은 클럭·PHY·리셋을 하나도 다루지 않는다 —
 *   전원 관리와 물리 계층이 모두 펌웨어 몫이다.
 * 옆쪽: drivers/pci/pci.c 의 pcie_set_readrq() 가 이 파일이 세운
 *   no_inc_mrrs 표시를 읽는다. drivers/pci/ecam.c 가 ACPI 경로를 몬다.
 *
 * 데이터 흐름:
 *   디바이스 트리(매칭 데이터, MEM 자원 0·1) -> probe -> struct loongson_pci
 *   config 접근: (버스, devfn, 오프셋) -> 주소 조립 -> 창 안의 포인터
 *
 * 공유 상태: struct loongson_pci 하나이며 probe 이후 불변이다.
 *   quirk 들은 이 구조체를 전혀 보지 않고 PCI 코어의 자료구조만 고친다.
 *
 * === NVMe 관점 ===
 * MRRS 는 NVMe 성능에 직결된다. NVMe 컨트롤러가 호스트 메모리의 PRP/SGL 과
 * 데이터를 읽을 때 한 번에 요청할 수 있는 크기가 그 값으로 제한되므로,
 * 256바이트로 묶이면 같은 전송에 훨씬 많은 읽기 요청이 오간다. 이 칩셋에
 * NVMe SSD 를 붙이면 읽기 대역폭이 그만큼 낮아지는데, 그것은 드라이버
 * 문제가 아니라 이 quirk 가 우회하는 하드웨어 한계다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_loongson_map_bus()      : config 접근의 관문. 두 창 중 하나를 고른다.
 * cfg0_map() / cfg1_map()     : 각 창의 주소 형식. 확장 config 공간을
 *                               다루느냐가 둘을 가른다.
 * pci_bus_to_loongson_pci()   : OF/ACPI 두 경로의 차이를 흡수한다.
 * loongson_pci_probe()        : 디바이스 트리 경로의 진입점.
 * loongson_pci_ecam_init()    : ACPI 경로의 진입점.
 * loongson_map_irq()          : INTx 를 디바이스 트리로, 실패하면 i8259 로.
 * struct loongson_pci         : 두 창과 매칭 데이터.
 * struct loongson_pci_data    : 칩셋별 차이 — 플래그와 config ops.
 */

/* [한국어] of_device_get_match_data 등 디바이스 트리 매칭. */
#include <linux/of.h>
/* [한국어] of_irq_parse_and_map_pci — INTx 를 디바이스 트리로 푸는 함수. */
#include <linux/of_pci.h>
/* [한국어] struct pci_dev, pci_ops, config 접근 도우미 전부. */
#include <linux/pci.h>
/* [한국어] PCI_VENDOR_ID_LOONGSON. 이 파일의 quirk 가 모두 그 상수로 등록된다. */
#include <linux/pci_ids.h>
/* [한국어] acpi_disabled 등 ACPI 연동. 부팅 경로를 가르는 데 쓴다. */
#include <linux/pci-acpi.h>
/* [한국어] struct pci_config_window 와 pci_ecam_ops — ACPI 경로의 뼈대. */
#include <linux/pci-ecam.h>

/* [한국어] PCI 서브시스템 **내부** 헤더. PCI_CFG_SPACE_SIZE(256)와
 * PCI_CFG_SPACE_EXP_SIZE(4096)가 여기 있어 map_bus 의 범위 검사에 쓴다.
 * 상대 경로로 포함하는 것은 이 헤더가 서브시스템 밖으로 공개되지 않기
 * 때문이다. */
#include "../pci.h"

/* Device IDs */
/* [한국어] LS2K 의 PCIe 포트 0. 아래 quirk 들이 이 ID 로 대상을 지목한다.
 * LS2K 는 포트가 하나뿐이라 여기만 있다. */
#define DEV_LS2K_PCIE_PORT0	0x1a05
/* [한국어] LS7A 의 PCIe 포트 0. 아래 7A 포트들은 ID 끝자리가 0x10 씩 늘어난다. */
#define DEV_LS7A_PCIE_PORT0	0x7a09
/* [한국어] LS7A PCIe 포트 1. */
#define DEV_LS7A_PCIE_PORT1	0x7a19
/* [한국어] LS7A PCIe 포트 2. 클래스 코드 quirk 는 여기까지만 적용된다. */
#define DEV_LS7A_PCIE_PORT2	0x7a29
/* [한국어] LS7A PCIe 포트 3. */
#define DEV_LS7A_PCIE_PORT3	0x7a39
/* [한국어] LS7A PCIe 포트 4. */
#define DEV_LS7A_PCIE_PORT4	0x7a49
/* [한국어] LS7A PCIe 포트 5. MSI quirk 가 이 포트에만 걸린다. */
#define DEV_LS7A_PCIE_PORT5	0x7a59
/* [한국어] LS7A PCIe 포트 6. MRRS quirk 는 포트 0~6 과 LS2K 포트 전부에 걸린다. */
#define DEV_LS7A_PCIE_PORT6	0x7a69

/* [한국어] LS2K 의 APB 브리지. 아래 셋(APB, LS7A CONF, LPC)이 브리지 자원 밖
 * 주소를 쓰는 장치들이다. */
#define DEV_LS2K_APB	0x7a02
/* [한국어] LS7A 의 이더넷. INTx 핀 quirk 대상이다. */
#define DEV_LS7A_GMAC	0x7a03
/* [한국어] LS7A 의 디스플레이 컨트롤러 1. 역시 핀 quirk 대상. */
#define DEV_LS7A_DC1	0x7a06
/* [한국어] LS7A 의 LPC 브리지. 자원 밖 주소를 쓴다. */
#define DEV_LS7A_LPC	0x7a0c
/* [한국어] LS7A 의 AHCI 컨트롤러. 핀 quirk 대상. */
#define DEV_LS7A_AHCI	0x7a08
/* [한국어] LS7A 의 설정 장치. 자원 밖 주소를 쓴다. */
#define DEV_LS7A_CONF	0x7a10
/* [한국어] LS7A 의 기가비트 이더넷. 핀 quirk 대상. */
#define DEV_LS7A_GNET	0x7a13
/* [한국어] LS7A 의 EHCI(USB 2.0). 핀 quirk 대상. */
#define DEV_LS7A_EHCI	0x7a14
/* [한국어] LS7A 의 디스플레이 컨트롤러 2. 핀 quirk 대상. */
#define DEV_LS7A_DC2	0x7a36
/* [한국어] LS7A 의 HDMI. 핀 quirk 대상. */
#define DEV_LS7A_HDMI	0x7a37

/* [한국어] CFG0 창을 쓴다. RS780E 만 이 플래그를 쓴다. */
#define FLAG_CFG0	BIT(0)
/* [한국어] CFG1 창을 쓴다. LS2K/LS7A 와 ACPI 경로가 쓴다. */
#define FLAG_CFG1	BIT(1)
/* [한국어] 하위 버스에서 장치 0 만 허용한다. 이 하드웨어가 다른 장치 번호를
 * 읽어도 같은 장치를 되비쳐 유령을 만들기 때문이다.
 * **ACPI 경로에는 이 플래그가 없다.** */
#define FLAG_DEV_FIX	BIT(2)
/* [한국어] 루트 버스의 특정 자리를 아예 접근하지 않는다. pdev_may_exist() 참조. */
#define FLAG_DEV_HIDDEN	BIT(3)

struct loongson_pci_data {
	/* [한국어] 이 칩셋에서 켤 동작 플래그들의 묶음.
	 * 설정자: 아래 세 벌의 정적 표, 또는 ACPI 경로에서 코드가 직접.
	 * 읽는 자: pci_loongson_map_bus() 와 probe.
	 * 값 범위: 위 FLAG_ 계열 넷의 조합.
	 * 동기화: 상수이므로 필요 없다. */
	u32 flags;
	/* [한국어] 이 칩셋이 쓸 config 접근 ops.
	 * 설정자: 아래 세 벌의 정적 표.
	 * 읽는 자: probe 가 브리지에 건다.
	 * 값 범위: 8/16/32비트를 모두 받는 판, 또는 32비트만 받는 판.
	 * 동기화: 상수를 가리킨다.
	 * **ACPI 경로에서는 이 필드를 채우지 않는다** — 그쪽은 ecam ops 안에
	 * 자기 표를 따로 두기 때문이다. */
	struct pci_ops *ops;
};

struct loongson_pci {
	/* [한국어] CFG0 창의 가상 주소.
	 * 설정자: loongson_pci_probe() 가 FLAG_CFG0 일 때만 매핑한다.
	 * 읽는 자: cfg0_map() 과, 창 선택을 판단하는 map_bus.
	 * 값 범위: 유효한 iomem 포인터, 또는 쓰지 않는 칩셋에서는 NULL.
	 * **NULL 여부 자체가 판단 근거** 라 map_bus 가 그것을 확인한다.
	 * 동기화: probe 이후 불변. */
	void __iomem *cfg0_base;
	/* [한국어] CFG1 창의 가상 주소.
	 * 설정자: OF 경로는 probe 가 매핑하고, ACPI 경로는
	 * loongson_pci_ecam_init() 이 코어가 준 창을 보정해 넣는다.
	 * 읽는 자: cfg1_map() 과 map_bus.
	 * 값 범위: 유효한 iomem 포인터 또는 NULL.
	 * 동기화: probe 이후 불변.
	 * **두 부팅 경로가 이 필드에 값을 넣는 방식이 다르다** — 그것이 이
	 * 드라이버가 두 경로를 한 파일에서 다루는 방식이다. */
	void __iomem *cfg1_base;
	/* [한국어] 이 브리지의 플랫폼 장치.
	 * 설정자: loongson_pci_probe().
	 * 읽는 자: [상류 코드 관찰] **없다.** 이 파일 어디에서도 이 필드를
	 * 다시 읽지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
	 * 값 범위: OF 경로에서는 유효한 포인터, ACPI 경로에서는 NULL 인 채로 남는다.
	 * 동기화: 없다. */
	struct platform_device *pdev;
	/* [한국어] 이 칩셋의 동작 플래그와 ops.
	 * 설정자: OF 경로는 디바이스 트리 매칭 데이터를, ACPI 경로는 코드가
	 * 만든 데이터를 넣는다.
	 * 읽는 자: pci_loongson_map_bus() 가 config 접근마다 읽는다.
	 * 값 범위: 정적 표의 항목이나 ACPI 경로가 할당한 것을 가리킨다.
	 * 동기화: 가리키는 대상이 사실상 상수라 필요 없다. */
	const struct loongson_pci_data *data;
};

/* Fixup wrong class code in PCIe bridges */
/* [한국어]
 * bridge_class_quirk - PCIe 포트의 잘못된 클래스 코드를 정상 브리지로 고친다
 *
 * @dev: 열거 중인 PCI 장치.
 *
 * LS7A 의 PCIe 포트 0·1·2 가 config 공간에 잘못된 클래스 코드를 보고한다.
 * 클래스 코드는 리눅스가 "이것이 브리지인가 엔드포인트인가" 를 판단하는
 * 근거이므로, 틀리면 그 아래 버스를 아예 스캔하지 않는다.
 *
 * EARLY 시점에 등록되는 것이 중요하다. 코어가 클래스 코드를 보고 동작을
 * 가르기 **전** 에 고쳐 놓아야 하기 때문이다.
 *
 * 포트 3~6 은 이 목록에 없다 — 그 포트들은 클래스 코드를 제대로 보고하는
 * 것으로 보이나, 근거는 이 트리에서 확인 못 함.
 *
 * 실행 컨텍스트: PCI 코어의 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI 코어의 장치 열거 → DECLARE_PCI_FIXUP_EARLY 로 등록된 [이 함수]
 */
static void bridge_class_quirk(struct pci_dev *dev)
{
	/* [한국어] 클래스 코드를 정상 PCI-to-PCI 브리지로 덮어쓴다. 이 값이라야
	 * 코어가 그 아래 버스를 스캔한다. */
	dev->class = PCI_CLASS_BRIDGE_PCI_NORMAL;
}
/* [한국어] 포트 0 에 이 quirk 를 건다. EARLY 시점이라 코어가 클래스 코드로
 * 동작을 가르기 전에 돈다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT0, bridge_class_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT1, bridge_class_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT2, bridge_class_quirk);

/* [한국어]
 * system_bus_quirk - 브리지 자원 밖 주소를 쓰는 장치에 BAR 를 믿지 말라고 표시한다
 *
 * @pdev: 열거 중인 PCI 장치.
 *
 * 옆의 상류 주석이 밝히듯, 이 장치들이 쓰는 주소 공간은 호스트 브리지의
 * 자원 범위 **밖** 에 있다. 즉 PCI 열거 규칙으로는 설명되지 않는 자리에
 * 매핑돼 있다.
 *
 * 두 가지를 표시한다.
 * - mmio_always_on: MMIO 를 끌 수 없다고 알린다. 코어가 전원 관리나
 *   자원 재배치 과정에서 이 장치의 메모리 디코딩을 끄지 않게 된다.
 * - non_compliant_bars: BAR 값이 규격에 맞지 않으니 읽어서 해석하지
 *   말라고 알린다. 그러지 않으면 코어가 엉뚱한 크기·주소를 잡아낸다.
 *
 * 대상이 셋이다 — LS2K 의 APB 브리지, LS7A 의 설정 장치와 LPC 브리지.
 * 모두 칩셋 내부 장치이며 실제로는 PCI 장치가 아닌 것을 PCI 로 노출한
 * 것들이다.
 *
 * 실행 컨텍스트: PCI 코어의 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI 코어의 장치 열거 → DECLARE_PCI_FIXUP_EARLY 로 등록된 [이 함수]
 */
static void system_bus_quirk(struct pci_dev *pdev)
{
	/*
	 * The address space consumed by these devices is outside the
	 * resources of the host bridge.
	 */
	/* [한국어] MMIO 를 끌 수 없다고 표시한다. 코어가 전원 관리나 자원 재배치
	 * 과정에서 이 장치의 메모리 디코딩을 끄지 않게 된다. */
	pdev->mmio_always_on = 1;
	/* [한국어] BAR 값을 믿지 말라고 표시한다. 그러지 않으면 코어가 엉뚱한
	 * 크기·주소를 잡아낸다. */
	pdev->non_compliant_bars = 1;
}
/* [한국어] LS2K 의 APB 브리지에 건다. 아래 둘도 같은 이유다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS2K_APB, system_bus_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_CONF, system_bus_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_LPC, system_bus_quirk);

/*
 * Some Loongson PCIe ports have hardware limitations on their Maximum Read
 * Request Size. They can't handle anything larger than this.  Sane
 * firmware will set proper MRRS at boot, so we only need no_inc_mrrs for
 * bridges. However, some MIPS Loongson firmware doesn't set MRRS properly,
 * so we have to enforce maximum safe MRRS, which is 256 bytes.
 */
#ifdef CONFIG_MIPS
/* [한국어]
 * loongson_set_min_mrrs_quirk - MIPS 판에서 MRRS 를 256바이트로 낮춘다
 *
 * @pdev: 활성화되는 PCI 장치.
 *
 * 위 상류 주석이 배경을 밝힌다. 제대로 된 펌웨어라면 부팅 때 MRRS 를
 * 알맞게 설정하므로 3번 quirk 의 "올리지 말라" 표시만으로 충분한데,
 * **일부 MIPS Loongson 펌웨어가 그것을 하지 않아** 직접 낮춰야 한다.
 *
 * 그래서 이 함수는 #ifdef CONFIG_MIPS 안에만 있다 — LoongArch 판에서는
 * 컴파일되지 않는다.
 *
 * 동작이 특이하다. **모든 벤더·모든 장치(PCI_ANY_ID)에 등록** 되므로 시스템의
 * 모든 장치에서 돌면서, 자기 위로 버스 계층을 거슬러 올라가 Loongson
 * PCIe 포트가 조상에 있는지 찾는다. 있으면 그 아래 장치의 MRRS 를 확인해
 * 256을 넘으면 낮춘다.
 *
 * 찾자마자 break 하므로, 조상 중 첫 Loongson 포트만 보고 판단한다.
 *
 * ENABLE 시점에 등록되는 것도 요점이다. 장치가 실제로 켜지기 직전이라
 * MRRS 설정이 확정되는 시점이다.
 *
 * 실행 컨텍스트: PCI 코어의 장치 활성화. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. pcie_set_readrq() 의 반환값을 확인하지 않는다.
 *
 * 호출 체인:
 *   PCI 코어의 장치 활성화 → DECLARE_PCI_FIXUP_ENABLE 로 등록된 [이 함수]
 *     → pci_match_id() → pcie_get_readrq() → pcie_set_readrq()
 */
static void loongson_set_min_mrrs_quirk(struct pci_dev *pdev)
{
	struct pci_bus *bus = pdev->bus;
	struct pci_dev *bridge;
	/* [한국어] 찾을 브리지들의 목록. **함수 안의 static 배열** 이라 호출 때마다
	 * 다시 만들지 않는다. */
	static const struct pci_device_id bridge_devids[] = {
		/* [한국어] LS2K 의 유일한 포트. */
		{ PCI_VDEVICE(LOONGSON, DEV_LS2K_PCIE_PORT0) },
		/* [한국어] LS7A 의 포트 0. 아래로 6번까지 이어진다. */
		{ PCI_VDEVICE(LOONGSON, DEV_LS7A_PCIE_PORT0) },
		{ PCI_VDEVICE(LOONGSON, DEV_LS7A_PCIE_PORT1) },
		{ PCI_VDEVICE(LOONGSON, DEV_LS7A_PCIE_PORT2) },
		{ PCI_VDEVICE(LOONGSON, DEV_LS7A_PCIE_PORT3) },
		{ PCI_VDEVICE(LOONGSON, DEV_LS7A_PCIE_PORT4) },
		{ PCI_VDEVICE(LOONGSON, DEV_LS7A_PCIE_PORT5) },
		{ PCI_VDEVICE(LOONGSON, DEV_LS7A_PCIE_PORT6) },
		{ 0, },
	};

	/* look for the matching bridge */
	while (!pci_is_root_bus(bus)) {
		bridge = bus->self;
		/* [한국어] 부모로 한 단계 올라간다. 이 두 줄로 브리지를 보면서 동시에
		 * 다음 순회를 준비한다. */
		bus = bus->parent;

		if (pci_match_id(bridge_devids, bridge)) {
			/* [한국어] 이 장치의 현재 MRRS 가 256을 넘는지 본다. 펌웨어가 제대로
			 * 설정했다면 넘지 않는다. */
			if (pcie_get_readrq(pdev) > 256) {
				/* [한국어] 낮춘다는 것을 로그에 남긴다. 성능이 예상보다 낮을 때 단서가 된다. */
				pci_info(pdev, "limiting MRRS to 256\n");
				/* [한국어] 256바이트로 낮춘다. **NVMe 처럼 큰 읽기를 내는 장치는 이 값이
				 * 곧 성능 상한이 된다.** 반환값은 확인하지 않는다. */
				pcie_set_readrq(pdev, 256);
			}
			break;
		}
	}
}
DECLARE_PCI_FIXUP_ENABLE(PCI_ANY_ID, PCI_ANY_ID, loongson_set_min_mrrs_quirk);
#endif

/* [한국어]
 * loongson_mrrs_quirk - 이 브리지에서 MRRS 를 올리지 못하게 한다
 *
 * @pdev: 열거 중인 PCI 장치(Loongson PCIe 포트).
 *
 * 호스트 브리지를 찾아 no_inc_mrrs 표시를 세운다.
 *
 * 그 표시의 효과는 이 파일이 아니라 **PCI 코어에 있다.**
 * drivers/pci/pci.c:11759 의 pcie_set_readrq() 가 이 표시를 보고, 요청된
 * 값이 현재 값보다 크면 거부하고 -EINVAL 을 돌려준다. 즉 펌웨어가 정해 둔
 * 값이 상한이 된다.
 *
 * 포트를 찾아 그 **호스트 브리지** 에 표시를 세운다는 점이 중요하다.
 * 포트 하나에 문제가 있으면 그 브리지 아래 전체가 제한을 받는다.
 *
 * 포트 여덟 개 모두에 등록돼 있어, 그중 어느 하나만 있어도 표시가 선다.
 *
 * 실행 컨텍스트: PCI 코어의 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. pci_find_host_bridge() 의 결과를 NULL 검사 없이 쓰는데,
 * 열거 중인 장치는 언제나 브리지에 속하므로 성립한다.
 *
 * 호출 체인:
 *   PCI 코어의 장치 열거 → DECLARE_PCI_FIXUP_EARLY 로 등록된 [이 함수]
 *     → pci_find_host_bridge()
 */
static void loongson_mrrs_quirk(struct pci_dev *pdev)
{
	/* [한국어] 이 장치가 속한 호스트 브리지를 찾는다. 포트 자신이 아니라 **브리지에**
	 * 표시를 세우므로, 포트 하나의 결함이 그 아래 전체에 적용된다. */
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);

	/* [한국어] MRRS 를 올리지 말라고 표시한다. 이 표시를 실제로 읽는 곳은
	 * drivers/pci/pci.c:11759 의 pcie_set_readrq() 이며, 현재 값보다 큰
	 * 요청을 -EINVAL 로 거부한다. */
	bridge->no_inc_mrrs = 1;
}
/* [한국어] LS2K 의 포트에 건다. 아래로 LS7A 포트 0~6 까지 여덟 곳에 등록된다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS2K_PCIE_PORT0, loongson_mrrs_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT0, loongson_mrrs_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT1, loongson_mrrs_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT2, loongson_mrrs_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT3, loongson_mrrs_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT4, loongson_mrrs_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT5, loongson_mrrs_quirk);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_PCIE_PORT6, loongson_mrrs_quirk);

/* [한국어]
 * loongson_pci_pin_quirk - INTx 핀 번호를 기능 번호에서 계산해 덮어쓴다
 *
 * @pdev: 열거 중인 PCI 장치.
 *
 * 이 칩셋의 내부 장치들이 config 공간의 인터럽트 핀 필드를 제대로 보고하지
 * 않아, 기능 번호로부터 직접 계산해 넣는다.
 *
 * 계산이 단순하다 — 기능 번호의 아래 2비트에 1을 더한다. 그래서 값이
 * 1~4 가 되며, 그것이 각각 INTA~INTD 에 해당한다. 기능 0 은 INTA,
 * 기능 1 은 INTB 하는 식이다.
 *
 * FINAL 시점에 등록되는 것이 요점이다. 열거가 끝나고 인터럽트 배정이
 * 이뤄지기 전이라, 이 값이 다음 단계의 map_irq 로 넘어간다.
 *
 * 대상이 일곱이다 — 디스플레이 컨트롤러 둘, 이더넷 둘, AHCI, EHCI, HDMI.
 * 모두 칩셋 내부 장치다.
 *
 * 실행 컨텍스트: PCI 코어의 장치 열거 마무리. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI 코어의 장치 열거 → DECLARE_PCI_FIXUP_FINAL 로 등록된 [이 함수]
 */
static void loongson_pci_pin_quirk(struct pci_dev *pdev)
{
	pdev->pin = 1 + (PCI_FUNC(pdev->devfn) & 3);
}
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_DC1, loongson_pci_pin_quirk);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_DC2, loongson_pci_pin_quirk);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_GMAC, loongson_pci_pin_quirk);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_AHCI, loongson_pci_pin_quirk);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_EHCI, loongson_pci_pin_quirk);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_GNET, loongson_pci_pin_quirk);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON,
			DEV_LS7A_HDMI, loongson_pci_pin_quirk);

/* [한국어]
 * loongson_pci_msi_quirk - 호스트 브리지의 MSI 활성 비트를 손수 세운다
 *
 * @dev: 열거 중인 PCI 장치.
 *
 * config 공간의 MSI 능력 구조에서 활성 비트를 직접 세운다. 보통은 장치
 * 드라이버가 MSI 를 요청할 때 커널이 세우는 비트인데, 여기서는 열거
 * 단계에서 미리 켠다.
 *
 * 클래스가 호스트 브리지가 아니면 아무것도 하지 않는다. 등록은 특정
 * PCIe 포트(포트 5)에 걸려 있는데 그 안에서 다시 클래스를 확인하는 셈이라,
 * 같은 장치 ID 가 다른 역할로도 나타날 수 있음을 상정한 것으로 보인다.
 *
 * [상류 코드 관찰] dev->msi_cap 이 0 인지 확인하지 않는다. MSI 능력이 없는
 * 장치라면 그 값이 0 이므로, config 공간의 오프셋 2 — 즉 장치 ID 필드 —
 * 를 읽고 거기에 되쓰게 된다. 위의 클래스 검사가 대상을 좁히기는 하지만
 * 능력 유무를 확인하지는 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 두 config 접근의 반환값도 확인하지 않는다.
 *
 * 실행 컨텍스트: PCI 코어의 장치 열거 마무리. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   PCI 코어의 장치 열거 → DECLARE_PCI_FIXUP_FINAL 로 등록된 [이 함수]
 *     → pci_read_config_word() → pci_write_config_word()
 */
static void loongson_pci_msi_quirk(struct pci_dev *dev)
{
	u16 val, class = dev->class >> 8;

	if (class != PCI_CLASS_BRIDGE_HOST)
		/* [한국어] 호스트 브리지가 아니면 아무것도 하지 않는다. */
		return;

	pci_read_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, &val);
	/* [한국어] MSI 활성 비트를 세운다. 보통은 장치 드라이버가 MSI 를 요청할 때
	 * 커널이 켜는 비트인데, 여기서는 열거 단계에서 미리 켠다. */
	val |= PCI_MSI_FLAGS_ENABLE;
	/* [한국어] 되쓴다. 위 [상류 코드 관찰] 대로 msi_cap 이 0 이면 이 쓰기가
	 * 장치 ID 필드로 간다. */
	pci_write_config_word(dev, dev->msi_cap + PCI_MSI_FLAGS, val);
}
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_LOONGSON, DEV_LS7A_PCIE_PORT5, loongson_pci_msi_quirk);

/* [한국어]
 * pci_bus_to_loongson_pci - OF/ACPI 두 부팅 경로의 차이를 흡수한다
 *
 * @bus: config 접근 대상 버스.
 * @return: 이 브리지의 드라이버 상태.
 *
 * **이 파일에 진입점이 둘이라서 필요한 함수다.**
 *
 * - 디바이스 트리 경로에서는 loongson_pci_probe() 가 bridge->sysdata 에
 *   드라이버 상태를 직접 넣는다. 그래서 sysdata 가 곧 상태다.
 * - ACPI 경로에서는 코어가 sysdata 에 pci_config_window 를 넣으므로,
 *   그 안의 priv 필드를 한 번 더 따라가야 한다.
 *
 * 두 경우를 acpi_disabled 전역으로 가른다. 그 값은 부팅 방식이 정해지는
 * 아주 이른 시점에 확정되고 이후 바뀌지 않으므로, config 접근 경로에서
 * 읽어도 안전하다. 다만 그 변수의 정의는 이 트리에 없다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다. 어느 갈래든 NULL 검사가 없는데, 두 경로 모두 자기
 * 진입점에서 값을 채워 두므로 성립한다.
 *
 * 호출 체인:
 *   pci_loongson_map_bus() → [이 함수]
 */
static struct loongson_pci *pci_bus_to_loongson_pci(struct pci_bus *bus)
{
	struct pci_config_window *cfg;

	if (acpi_disabled)
		/* [한국어] 디바이스 트리 경로 — probe 가 sysdata 에 드라이버 상태를 직접
		 * 넣어 두었으므로 그대로 형변환한다. */
		return (struct loongson_pci *)(bus->sysdata);

	cfg = bus->sysdata;
	/* [한국어] ACPI 경로 — sysdata 가 config 창 서술자이므로 그 안의 priv 를
	 * 한 번 더 따라간다. */
	return (struct loongson_pci *)(cfg->priv);
}

/* [한국어]
 * cfg0_map - CFG0 창의 config 주소를 조립한다 (표준 256바이트 공간 전용)
 *
 * @priv: 드라이버 상태. 창의 기준 주소를 담고 있다.
 * @bus: 대상 버스.
 * @devfn: 장치·기능 번호.
 * @where: config 공간 오프셋.
 * @return: 그 config 레지스터를 가리키는 창 안의 포인터.
 *
 * 이 칩셋의 config 주소 형식은 표준 ECAM 과 다르다. 조립 규칙이 이렇다.
 *
 *   비트 24    : 타입 1 접근 표시. **루트 버스가 아닐 때만** 세운다.
 *   비트 23~16 : 버스 번호. 역시 루트 버스가 아닐 때만 넣는다.
 *   비트 15~8  : 장치·기능 번호.
 *   비트 7~0   : 레지스터 오프셋.
 *
 * 루트 버스에서는 위 두 자리가 0 으로 남는다 — 타입 0 접근이며, 브리지
 * 자신과 그에 직결된 장치를 가리킨다.
 *
 * **오프셋 자리가 8비트뿐** 이라, 이 창으로는 표준 config 공간 256바이트만
 * 닿을 수 있다. 확장 공간은 아래 cfg1_map() 의 몫이다.
 *
 * RS780E 계열이 이 창만 쓴다(매칭 데이터의 FLAG_CFG0).
 *
 * 실행 컨텍스트: config 접근 경로. 순수 계산이며 부수효과가 없다.
 *
 * 에러 경로: 없다. 오프셋 범위 검사는 호출자가 이미 했다.
 *
 * 호출 체인:
 *   pci_loongson_map_bus() → [이 함수]
 */
static void __iomem *cfg0_map(struct loongson_pci *priv, struct pci_bus *bus,
			      unsigned int devfn, int where)
{
	unsigned long addroff = 0x0;
	unsigned char busnum = bus->number;

	if (!pci_is_root_bus(bus)) {
		/* [한국어] 타입 1 접근 표시. 루트 버스가 아니라는 것은 브리지 너머를 가리킨다는
		 * 뜻이고, 그때는 버스 번호가 주소에 들어가야 한다. */
		addroff |= BIT(24); /* Type 1 Access */
		/* [한국어] 버스 번호를 비트 23~16 에 얹는다. */
		addroff |= (busnum << 16);
	}
	addroff |= (devfn << 8) | where;
	/* [한국어] 조립한 오프셋을 창 기준 주소에 더해 최종 포인터를 만든다. */
	return priv->cfg0_base + addroff;
}

/* [한국어]
 * cfg1_map - CFG1 창의 config 주소를 조립한다 (확장 4096바이트 공간까지)
 *
 * @priv: 드라이버 상태.
 * @bus: 대상 버스.
 * @devfn: 장치·기능 번호.
 * @where: config 공간 오프셋.
 * @return: 그 config 레지스터를 가리키는 창 안의 포인터.
 *
 * cfg0_map() 과 같은 일을 하되 **확장 config 공간까지 닿는다.**
 * 그러기 위해 오프셋 12비트를 두 조각으로 쪼개 넣는 것이 이 함수의 요점이다.
 *
 *   비트 28    : 타입 1 접근 표시(cfg0 의 비트 24 와 자리가 다르다).
 *   비트 27~24 : 오프셋의 상위 4비트. `(where & 0xf00) << 16` 이 이 자리를
 *                만든다 — 비트 11~8 을 16칸 올리면 비트 27~24 가 된다.
 *   비트 23~16 : 버스 번호.
 *   비트 15~8  : 장치·기능 번호.
 *   비트 7~0   : 오프셋의 하위 8비트.
 *
 * 즉 오프셋이 연속된 자리에 있지 않고 **버스·장치 번호를 사이에 두고
 * 갈라져 있다.** 표준 ECAM 이 오프셋을 한 덩어리로 두는 것과 다른 점이다.
 *
 * ACPI 경로도 이 함수로 들어온다. loongson_pci_ecam_init() 이 창 주소를
 * 버스 번호만큼 미리 빼 두어, 여기서 더하는 버스 번호와 상쇄되게 맞춰 둔다.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 계산이며 부수효과가 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_loongson_map_bus() → [이 함수]
 */
static void __iomem *cfg1_map(struct loongson_pci *priv, struct pci_bus *bus,
			      unsigned int devfn, int where)
{
	unsigned long addroff = 0x0;
	unsigned char busnum = bus->number;

	if (!pci_is_root_bus(bus)) {
		/* [한국어] 타입 1 접근 표시. **cfg0 의 비트 24 와 자리가 다르다** — 두 창의
		 * 주소 형식이 별개임을 보여 주는 지점이다. */
		addroff |= BIT(28); /* Type 1 Access */
		/* [한국어] 버스 번호를 비트 23~16 에 얹는다. 이 자리는 cfg0 과 같다. */
		addroff |= (busnum << 16);
	}
	addroff |= (devfn << 8) | (where & 0xff) | ((where & 0xf00) << 16);
	/* [한국어] 조립한 오프셋을 창 기준 주소에 더한다. */
	return priv->cfg1_base + addroff;
}

/* [한국어]
 * pdev_may_exist - 있을 수 없는 자리인지 판정한다
 *
 * @bus: 대상 버스.
 * @device: 장치 번호.
 * @function: 기능 번호.
 * @return: true = 있을 수 있다, false = 확실히 없다.
 *
 * **루트 버스의 장치 9~20 번에는 다중 기능 장치가 없다.** 그 범위의
 * 기능 1 이상을 걸러 내는 것이 이 함수의 전부다.
 *
 * 왜 필요한가. 존재하지 않는 자리를 읽으면 이 하드웨어가 오류를 내거나
 * 엉뚱한 값을 돌려주어, 코어가 유령 장치를 만들어 낸다. 표준적인
 * "없으면 0xFFFFFFFF" 규약을 지키지 않는 것이다.
 *
 * 범위가 코드에 그대로 박혀 있다. 칩셋 배치가 고정이라 표로 뺄 이유가
 * 없다고 본 것으로 보인다.
 *
 * 이 검사는 FLAG_DEV_HIDDEN 이 선 칩셋에서만 쓰인다 — LS2K, LS7A,
 * 그리고 ACPI 경로다. RS780E 는 해당하지 않는다.
 *
 * 실행 컨텍스트: config 접근 경로. 순수 계산.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_loongson_map_bus() → [이 함수]
 */
static bool pdev_may_exist(struct pci_bus *bus, unsigned int device,
			   unsigned int function)
{
	return !(pci_is_root_bus(bus) &&
		(device >= 9 && device <= 20) && (function > 0));
}

/* [한국어]
 * pci_loongson_map_bus - config 접근의 관문. 걸러 내고 창을 고른다
 *
 * @bus: 대상 버스.
 * @devfn: 장치·기능 번호.
 * @where: config 공간 오프셋.
 * @return: 접근할 포인터, 또는 NULL(접근 금지).
 *
 * **이 파일의 config 경로가 모두 이 함수를 지난다** — 디바이스 트리
 * 경로의 두 ops 표와 ACPI 경로의 ecam ops 가 전부 이것을 가리킨다.
 *
 * 네 단계다.
 *
 * 1. **FLAG_DEV_FIX 걸러 내기.** 루트 버스가 아닌 버스에서는 장치 0 만
 *    허용한다. PCIe 링크 아래에는 장치가 하나뿐인 것이 정상인데, 이
 *    하드웨어는 다른 장치 번호를 읽어도 같은 장치를 되비쳐 유령을 만든다.
 *    `bus->self` 검사가 함께 있어, 브리지를 통해 도달한 버스에만 적용된다.
 * 2. **FLAG_DEV_HIDDEN 걸러 내기.** 위 pdev_may_exist() 로 있을 수 없는
 *    자리를 막는다.
 * 3. **CFG0 우선.** 오프셋이 표준 공간 안이고 CFG0 창이 있으면 그쪽으로
 *    보낸다.
 * 4. **CFG1 대체.** 확장 공간이거나 CFG0 이 없으면 CFG1 로 보낸다.
 *
 * 3번과 4번의 순서가 요점이다. 두 창이 모두 있는 구성에서는 표준 공간
 * 접근이 언제나 CFG0 으로 가고, CFG1 은 확장 공간에만 쓰인다.
 *
 * NULL 을 돌려주면 상위의 pci_generic_config_read() 가 "장치 없음" 으로
 * 처리한다.
 *
 * 실행 컨텍스트: config 접근 경로. 잠들지 않는다.
 *
 * 에러 경로: 걸러 낸 접근과 어느 창에도 맞지 않는 오프셋은 NULL 을
 * 돌려준다.
 *
 * 호출 체인:
 *   PCI 코어의 config 접근 → pci_ops.map_bus == [이 함수]
 *     → pci_bus_to_loongson_pci() → pdev_may_exist()
 *     → cfg0_map() 또는 cfg1_map()
 */
static void __iomem *pci_loongson_map_bus(struct pci_bus *bus,
					  unsigned int devfn, int where)
{
	unsigned int device = PCI_SLOT(devfn);
	unsigned int function = PCI_FUNC(devfn);
	/* [한국어] 부팅 경로에 관계없이 드라이버 상태를 얻는다. 이 한 줄이 OF/ACPI
	 * 차이를 흡수한다. */
	struct loongson_pci *priv = pci_bus_to_loongson_pci(bus);

	/*
	 * Do not read more than one device on the bus other than
	 * the host bus.
	 */
	if ((priv->data->flags & FLAG_DEV_FIX) && bus->self) {
		if (!pci_is_root_bus(bus) && (device > 0))
			/* [한국어] 루트 버스가 아닌데 장치 0 이 아니면 접근을 막는다. 유령 장치가
			 * 만들어지는 것을 방지한다. */
			return NULL;
	}

	/* Don't access non-existent devices */
	if (priv->data->flags & FLAG_DEV_HIDDEN) {
		if (!pdev_may_exist(bus, device, function))
			/* [한국어] 있을 수 없는 자리이므로 접근을 막는다. */
			return NULL;
	}

	/* CFG0 can only access standard space */
	/* [한국어] 오프셋이 표준 config 공간 안이고 CFG0 창이 있으면 그쪽을 쓴다.
	 * **두 창이 다 있으면 표준 공간은 언제나 CFG0 으로 간다.** */
	if (where < PCI_CFG_SPACE_SIZE && priv->cfg0_base)
		/* [한국어] CFG0 형식으로 주소를 조립해 돌려준다. */
		return cfg0_map(priv, bus, devfn, where);

	/* CFG1 can access extended space */
	/* [한국어] 확장 공간이거나 CFG0 이 없는 경우다. 오프셋이 확장 공간 크기
	 * 안이고 CFG1 창이 있으면 그쪽을 쓴다. */
	if (where < PCI_CFG_SPACE_EXP_SIZE && priv->cfg1_base)
		/* [한국어] CFG1 형식으로 주소를 조립해 돌려준다. */
		return cfg1_map(priv, bus, devfn, where);

	/* [한국어] 어느 창에도 맞지 않는다. NULL 을 돌려주면 상위의 일반 config 구현이
	 * "장치 없음" 으로 처리한다. */
	return NULL;
}

#ifdef CONFIG_OF

/* [한국어]
 * loongson_map_irq - INTx 를 디바이스 트리로 풀고, 실패하면 i8259 로 되돌아간다
 *
 * @dev: 인터럽트를 배정할 장치.
 * @slot: 슬롯 번호.
 * @pin: INTx 핀 번호(1~4). loongson_pci_pin_quirk 가 고쳐 둔 값이 온다.
 * @return: 가상 인터럽트 번호, 또는 0(배정 실패).
 *
 * 두 갈래를 차례로 시도한다.
 *
 * 1. **디바이스 트리 경로.** interrupt-map 속성을 따라 풀어 본다.
 *    정상적인 최신 시스템은 여기서 끝난다.
 * 2. **i8259 되돌아가기.** 옆의 상류 주석이 밝히듯 옛 시스템을 위한
 *    경로다. config 공간의 인터럽트 라인 필드를 그대로 읽어 쓴다.
 *    그 값은 원래 펌웨어가 적어 둔 것으로, 디바이스 트리가 없던 시절의
 *    방식이다.
 *
 * 두 번째 갈래에 상한 검사가 있다. i8259 는 인터럽트가 15개뿐이므로
 * 그보다 큰 값은 유효하지 않은 것으로 보고 0 을 돌려준다. 0 은 "인터럽트
 * 없음" 을 뜻해, 그 장치는 INTx 를 쓸 수 없게 된다.
 *
 * [상류 코드 관찰] 두 번째 갈래에서 config 읽기의 반환값을 확인하지
 * 않는다. 읽기가 실패하면 초기화되지 않은 val 이 그대로 판정에 쓰인다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 디바이스 트리 경로에만 있는 함수다 — #ifdef CONFIG_OF 안에 있다.
 *
 * 실행 컨텍스트: PCI 코어의 인터럽트 배정. 프로세스 컨텍스트.
 *
 * 에러 경로: 어느 갈래로도 풀지 못하면 0 을 돌려준다.
 *
 * 호출 체인:
 *   PCI 코어의 인터럽트 배정 → pci_host_bridge.map_irq == [이 함수]
 *     → of_irq_parse_and_map_pci() → pci_read_config_byte()
 */
static int loongson_map_irq(const struct pci_dev *dev, u8 slot, u8 pin)
{
	int irq;
	u8 val;

	irq = of_irq_parse_and_map_pci(dev, slot, pin);
	/* [한국어] 디바이스 트리로 풀렸다. 정상적인 최신 시스템은 여기서 끝난다. */
	if (irq > 0)
		/* [한국어] 그 가상 인터럽트 번호를 그대로 돌려준다. */
		return irq;

	/* Care i8259 legacy systems */
	/* [한국어] 옆의 상류 주석대로 옛 시스템을 위한 갈래다. 펌웨어가 config 공간에
	 * 적어 둔 인터럽트 번호를 그대로 읽는다.
	 * [상류 코드 관찰] 이 읽기의 반환값을 확인하지 않아, 실패하면 초기화되지
	 * 않은 val 이 아래 판정에 쓰인다. */
	pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &val);
	/* i8259 only have 15 IRQs */
	/* [한국어] i8259 의 인터럽트는 15개뿐이라 그보다 큰 값은 유효하지 않다. */
	if (val > 15)
		/* [한국어] 0 을 돌려주면 이 장치는 INTx 를 쓸 수 없게 된다. */
		return 0;

	/* [한국어] 펌웨어가 적어 둔 번호를 그대로 쓴다. */
	return val;
}

/* LS2K/LS7A accept 8/16/32-bit PCI config operations */
static struct pci_ops loongson_pci_ops = {
	.map_bus = pci_loongson_map_bus,
	/* [한국어] 8/16/32비트 config 읽기를 모두 받는 일반 구현. */
	.read	= pci_generic_config_read,
	.write	= pci_generic_config_write,
};

/* RS780/SR5690 only accept 32-bit PCI config operations */
static struct pci_ops loongson_pci_ops32 = {
	.map_bus = pci_loongson_map_bus,
	/* [한국어] **32비트 읽기만 하는** 구현. 좁은 폭 접근을 받지 못하는 하드웨어를
	 * 위해 코어가 제공하는 판이며, 읽어서 필요한 부분만 잘라 낸다. */
	.read	= pci_generic_config_read32,
	.write	= pci_generic_config_write32,
};

static const struct loongson_pci_data ls2k_pci_data = {
	/* [한국어] LS2K — CFG1 창, 장치 걸러 내기 둘 다 켠다. */
	.flags = FLAG_CFG1 | FLAG_DEV_FIX | FLAG_DEV_HIDDEN,
	/* [한국어] 8/16/32비트를 모두 받는 ops. */
	.ops = &loongson_pci_ops,
};

static const struct loongson_pci_data ls7a_pci_data = {
	/* [한국어] LS7A — LS2K 와 플래그가 완전히 같다. */
	.flags = FLAG_CFG1 | FLAG_DEV_FIX | FLAG_DEV_HIDDEN,
	/* [한국어] 같은 ops 를 쓴다. */
	.ops = &loongson_pci_ops,
};

static const struct loongson_pci_data rs780e_pci_data = {
	/* [한국어] RS780E — **CFG0 창만 쓰고 걸러 내기 플래그가 없다.** 앞의 둘과
	 * 성격이 다른 옛 칩셋이다. */
	.flags = FLAG_CFG0,
	/* [한국어] 32비트 접근만 하는 ops 를 쓴다. */
	.ops = &loongson_pci_ops32,
};

static const struct of_device_id loongson_pci_of_match[] = {
	/* [한국어] LS2K 용 디바이스 트리 호환 문자열. */
	{ .compatible = "loongson,ls2k-pci",
		/* [한국어] 그에 대응하는 매칭 데이터. */
		.data = &ls2k_pci_data, },
	{ .compatible = "loongson,ls7a-pci",
		.data = &ls7a_pci_data, },
	{ .compatible = "loongson,rs780e-pci",
		.data = &rs780e_pci_data, },
	{}
};

/* [한국어]
 * loongson_pci_probe - 디바이스 트리 경로의 진입점. 두 창을 잡고 브리지를 넘긴다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 디바이스 트리로 부팅한 경우의 진입점이다. ACPI 경로는
 * loongson_pci_ecam_init() 이 맡는다.
 *
 * 다섯 단계다.
 * 1. 디바이스 트리 노드가 있는지 확인한다. 없으면 이 경로가 아니다.
 * 2. 호스트 브리지를 할당한다. 드라이버 상태를 그 안에 붙여 얻는다.
 * 3. 매칭 데이터를 담고, 플래그에 따라 CFG0·CFG1 창을 잡는다.
 * 4. 브리지에 상태·ops·인터럽트 매핑 함수를 건다.
 * 5. 코어에 넘겨 버스를 스캔하게 한다.
 *
 * [상류 코드 관찰] 3번의 두 창에 대한 실패 처리가 서로 다르다.
 * - CFG0: 자원이 없으면 dev_err 로 오류를 기록하면서도 **그대로 진행한다.**
 *   cfg0_base 가 NULL 인 채로 스캔이 시작되며, 그 경우 map_bus 가 모든
 *   접근에 NULL 을 돌려주게 된다. 반면 매핑 자체가 실패하면 그 오류를
 *   올려보낸다 — 같은 창에 대해 두 실패의 처리가 갈린다.
 * - CFG1: 자원이 없으면 dev_info(오류가 아니라 정보)로 기록하고, 매핑이
 *   실패하면 **오류를 삼키고 NULL 로 두고** 계속한다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 브리지 할당 실패에 -ENODEV 를 돌려주는 것도 눈에 띈다 — 메모리 부족을
 * "장치 없음" 으로 보고하는 셈이다. 같은 트리의 pcie-xilinx.c 에도 같은
 * 형태가 있다.
 *
 * 되감기 코드가 없다. 잡는 것이 모두 devm 판이라 코어가 되돌린다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트이며 버스
 * 스캔으로 오래 걸린다.
 *
 * 에러 경로: 위에 적은 대로 갈래마다 다르며, 마지막의 pci_host_probe()
 * 결과는 그대로 올려보낸다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → devm_pci_alloc_host_bridge() → of_device_get_match_data()
 *     → platform_get_resource() → devm_pci_remap_cfg_resource()
 *     → pci_host_probe()
 */
static int loongson_pci_probe(struct platform_device *pdev)
{
	struct loongson_pci *priv;
	struct device *dev = &pdev->dev;
	/* [한국어] 디바이스 트리 노드. 이 경로인지 판정하는 근거이기도 하다. */
	struct device_node *node = dev->of_node;
	/* [한국어] PCI 코어에 넘길 호스트 브리지. */
	struct pci_host_bridge *bridge;
	/* [한국어] config 창 자원을 받을 자리. */
	struct resource *regs;

	if (!node)
		/* [한국어] 디바이스 트리 노드가 없다. ACPI 로 부팅한 경우이므로 이 경로가
		 * 맡을 장치가 아니다. */
		return -ENODEV;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*priv));
	/* [한국어] 브리지를 할당하지 못했다. */
	if (!bridge)
		/* [한국어] [상류 코드 관찰] 메모리 부족인데 -ENODEV 를 돌려준다.
		 * 원본에서 확인했으며 코드는 고치지 않았다. */
		return -ENODEV;

	priv = pci_host_bridge_priv(bridge);
	/* [한국어] [상류 코드 관찰] 담아 두지만 이 파일 어디에서도 다시 읽지 않는다. */
	priv->pdev = pdev;
	/* [한국어] 디바이스 트리 매칭 데이터를 담는다. 아래 플래그 판단의 근거다. */
	priv->data = of_device_get_match_data(dev);

	if (priv->data->flags & FLAG_CFG0) {
		/* [한국어] 첫 MEM 자원이 CFG0 창이다. */
		regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
		/* [한국어] 자원이 없다. */
		if (!regs)
			/* [한국어] [상류 코드 관찰] 오류를 기록하면서도 **그대로 진행한다.**
			 * cfg0_base 가 NULL 인 채로 스캔이 시작된다. */
			dev_err(dev, "missing mem resources for cfg0\n");
		/* [한국어] 자원이 있으면 매핑한다. */
		else {
			priv->cfg0_base = devm_pci_remap_cfg_resource(dev, regs);
			/* [한국어] 매핑이 실패했다. */
			if (IS_ERR(priv->cfg0_base))
				/* [한국어] 이쪽은 오류를 올려보낸다 — 같은 창인데 자원 없음과 매핑 실패의
				 * 처리가 갈린다. */
				return PTR_ERR(priv->cfg0_base);
		}
	}

	if (priv->data->flags & FLAG_CFG1) {
		/* [한국어] 둘째 MEM 자원이 CFG1 창이다. */
		regs = platform_get_resource(pdev, IORESOURCE_MEM, 1);
		/* [한국어] 자원이 없다. */
		if (!regs)
			/* [한국어] [상류 코드 관찰] 이쪽은 오류가 아니라 정보로 기록한다. */
			dev_info(dev, "missing mem resource for cfg1\n");
		/* [한국어] 자원이 있으면 매핑한다. */
		else {
			priv->cfg1_base = devm_pci_remap_cfg_resource(dev, regs);
			/* [한국어] 매핑이 실패했다. */
			if (IS_ERR(priv->cfg1_base))
				/* [한국어] [상류 코드 관찰] 오류를 삼키고 NULL 로 두고 계속한다. CFG0 이
				 * 있으면 표준 공간은 여전히 접근할 수 있기 때문으로 보인다. */
				priv->cfg1_base = NULL;
		}
	}

	bridge->sysdata = priv;
	/* [한국어] 이 칩셋에 맞는 config ops 를 건다. */
	bridge->ops = priv->data->ops;
	/* [한국어] INTx 배정 함수를 건다. 이 함수는 OF 경로에만 있다. */
	bridge->map_irq = loongson_map_irq;

	return pci_host_probe(bridge);
}

static struct platform_driver loongson_pci_driver = {
	/* [한국어] 드라이버 이름과 매칭 표. */
	.driver = {
		/* [한국어] sysfs 에 보일 이름. */
		.name = "loongson-pci",
		/* [한국어] 위의 디바이스 트리 매칭 표를 건다. */
		.of_match_table = loongson_pci_of_match,
	},
	.probe = loongson_pci_probe,
};
builtin_platform_driver(loongson_pci_driver);

#endif

#ifdef CONFIG_ACPI

/* [한국어]
 * loongson_pci_ecam_init - ACPI 경로의 진입점. 창 주소를 보정해 둔다
 *
 * @cfg: ACPI 코어가 만든 config 창 서술자.
 * @return: 0 = 성공, -ENOMEM = 실패.
 *
 * ACPI 로 부팅한 경우의 진입점이며, 디바이스 트리 경로의 probe 에 대응한다.
 *
 * 세 가지를 한다.
 * 1. 드라이버 상태와 매칭 데이터를 할당해 창 서술자에 매단다. 디바이스
 *    트리가 없으므로 **매칭 데이터를 코드에서 직접 만든다.**
 * 2. 플래그를 정한다 — CFG1 창과 숨은 장치 걸러 내기다.
 *    디바이스 트리 경로의 LS7A 데이터와 견주면 **FLAG_DEV_FIX 가 없다.**
 *    즉 ACPI 경로에서는 하위 버스의 장치 번호를 걸러 내지 않는다.
 *    그 차이의 근거는 이 트리에서 확인 못 함.
 * 3. **창 주소를 버스 번호만큼 미리 뺀다.** 이것이 이 함수의 핵심이다.
 *
 * 3번을 풀어 보면 이렇다. ACPI 가 준 창은 이 브리지가 맡은 첫 버스 번호에
 * 맞춰져 있는데, cfg1_map() 은 버스 번호를 절대값으로 주소에 얹는다.
 * 그래서 기준 주소에서 첫 버스 번호만큼(16비트 올려서) 빼 두면, 나중에
 * 더해질 때 정확히 상쇄된다.
 *
 * 빼는 폭이 16비트인 것은 ops 표의 bus_shift 가 16 인 것과 짝을 이룬다 —
 * 버스 하나가 차지하는 주소 폭이 64KB 라는 뜻이다.
 *
 * 실행 컨텍스트: ACPI PCI 코어의 초기화. 프로세스 컨텍스트.
 *
 * 에러 경로: 두 할당 중 하나라도 실패하면 -ENOMEM. 첫 할당이 성공한 뒤
 * 둘째가 실패하면 그것을 되돌리지 않는데, devm 판이라 코어가 정리한다.
 *
 * 호출 체인:
 *   ACPI PCI 코어 → pci_ecam_ops.init == [이 함수] → devm_kzalloc() ×2
 */
static int loongson_pci_ecam_init(struct pci_config_window *cfg)
{
	struct device *dev = cfg->parent;
	struct loongson_pci *priv;
	/* [한국어] 이 경로에서 직접 만들 매칭 데이터. */
	struct loongson_pci_data *data;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	/* [한국어] 메모리 부족이다. */
	if (!priv)
		/* [한국어] 코어가 초기화 실패로 처리한다. */
		return -ENOMEM;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	/* [한국어] 매칭 데이터도 못 만들었다. */
	if (!data)
		/* [한국어] 앞서 할당한 것은 devm 판이라 코어가 정리한다. */
		return -ENOMEM;

	cfg->priv = priv;
	/* [한국어] 플래그를 코드에서 직접 정한다. **OF 경로의 LS7A 데이터와 견주면
	 * FLAG_DEV_FIX 가 빠져 있다** — 그 차이의 근거는 이 트리에서 확인 못 함. */
	data->flags = FLAG_CFG1 | FLAG_DEV_HIDDEN;
	/* [한국어] 만든 데이터를 상태에 연결한다. */
	priv->data = data;
	/* [한국어] **이 함수의 핵심.** 코어가 준 창 주소에서 첫 버스 번호만큼
	 * (16비트 올려서) 빼 둔다. 나중에 cfg1_map() 이 버스 번호를 절대값으로
	 * 더할 때 정확히 상쇄되게 하려는 것이다. */
	priv->cfg1_base = cfg->win - (cfg->busr.start << 16);

	return 0;
}

const struct pci_ecam_ops loongson_pci_ecam_ops = {
	/* [한국어] 버스 하나가 차지하는 주소 폭이 2^16 = 64KB 라는 뜻이며,
	 * 위 보정의 시프트 폭과 짝을 이룬다. */
	.bus_shift = 16,
	/* [한국어] 창을 만든 뒤 이 파일의 초기화 함수를 부르게 한다. */
	.init	   = loongson_pci_ecam_init,
	.pci_ops   = {
		.map_bus = pci_loongson_map_bus,
		/* [한국어] 8/16/32비트를 모두 받는 일반 구현. OF 경로의 ops 와 같다. */
		.read	 = pci_generic_config_read,
		.write	 = pci_generic_config_write,
	}
};

#endif
