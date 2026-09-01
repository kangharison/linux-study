// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015, 2016 Cavium, Inc.
 */

/* [한국어] 기본 커널 유틸. */
/*
 * [한국어 설명] Cavium ThunderX 의 없는 EA capability 를 위조해 보여 주는 드라이버 (pci-thunder-ecam.c)
 *
 * === 파일의 역할 ===
 * Cavium(현 Marvell) ThunderX SoC 의 내부 PCI 장치들을 커널이 정상적으로
 * 다룰 수 있게 만드는 쿼크 드라이버다. drivers/pci 에서 가장 대담한 축에
 * 드는 코드로, 하는 일을 한마디로 하면 **config 공간에 존재하지 않는
 * capability 를 읽기 시점에 만들어 내는 것** 이다.
 * 문제의 배경은 이렇다. 이 SoC 의 내부 장치들(NIC, TNS, ZIP, RAD, DFA 등)은
 * BAR 로 주소를 알려 주지 못하고 SoC 안의 고정된 주소에 놓여 있다.
 * 그런 장치를 표현하는 표준 수단이 PCIe 의 EA(Enhanced Allocation)
 * capability 인데, Pass-1 실리콘은 그것을 갖고 있지 않다. 그래서 이 파일이
 * config 읽기를 가로채 EA capability 를 통째로 지어낸다.
 * 위조는 세 겹이다. (1) capability 사슬의 next 포인터를 고쳐 실재하지 않는
 * EA 를 0xbc 자리에 끼워 넣고, (2) 그 자리에 항목 개수가 담긴 EA 헤더를
 * 만들고(장치마다 개수가 다르다 — NIC 4, TNS 3, MSI-X 있으면 2, 그 밖에 1),
 * (3) 각 항목은 실제 BAR 을 읽어 EA 형식으로 옮긴다.
 * 그와 짝을 이루어 BAR 자체는 0 으로 보여 주고, BAR 에 대한 쓰기는 조용히
 * 버린다. 커널이 BAR 을 재배정하면 EA 가 알려 준 고정 주소와 어긋나기 때문이다.
 * 실리콘 리비전이 8 이상(Pass-2)이면 하드웨어가 EA 를 제대로 제공하므로
 * 이 전부를 건너뛰고, 남은 결함 하나 — Base 상위 워드의 12번 비트 누락 —
 * 만 보정한다. 리비전 검사 한 번으로 완전히 다른 경로를 타는 구조다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 드라이버는 PCI 코어의 config 접근 경로에 끼어드는 얇은 층이다.
 * 주소 계산은 손대지 않는다 — map_bus 로 표준 pci_ecam_map_bus() 를 그대로
 * 쓴다. 고치는 것은 읽어 온 **값** 뿐이다.
 *   pci_read_config_*() → PCI 코어 → [이 파일] thunder_ecam_config_read()
 *     → 리비전 검사 → Pass-2 면 thunder_ecam_p2_config_read()
 *                     → Pass-1 이면 위조 분기들 → handle_ea_bar() → set_val()
 *     → 위조 대상이 아니면 pci_generic_config_read() 로 넘긴다
 * probe 는 직접 쓰지 않고 pci-host-common.c 의 pci_host_common_probe() 를
 * 그대로 빌린다. 이 드라이버가 고유하게 하는 일은 ops 표의 두 콜백뿐이다.
 * 진입 경로가 둘이다. DT 로 부팅하면 아래 builtin_platform_driver 가
 * 등록되고, ACPI 로 부팅하면 ACPI 쪽 코드가 pci_thunder_ecam_ops 를 이름으로
 * 참조해 쓴다. 그래서 ops 표만 바깥 #if 안에 있고 드라이버 등록은 DT 조건
 * 안쪽에 따로 들어 있다.
 * 실행 컨텍스트는 전부 config 접근 경로다. 코어가 pci_lock 을 쥔 채 부르므로
 * 잠들면 안 되고, 실제로 이 파일은 readl/writel 과 계산만 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: PCI 코어의 config 접근 경로(drivers/pci/access.c)와
 * pci-host-common.c 의 pci_host_common_probe().
 * 옆쪽: drivers/pci/ecam.c 의 pci_ecam_map_bus() 와 struct pci_config_window.
 * 특히 cfg->res.start 를 Pass-2 보정에서 쓰는데, config 창의 물리 주소에서
 * 빠진 노드 비트를 가져오기 위해서다 — 위조가 아니라 하드웨어 결함 보정이라
 * 창의 실제 주소가 근거가 된다.
 * 아래쪽: access.c 의 pci_generic_config_read/write, 그리고 readl/writel.
 * 규격 근거: PCIe 의 EA(Enhanced Allocation) capability. 항목 하나가
 * 16바이트이고 헤더·Base-L·Offset-L·Base-H 네 워드로 이루어진다는 배치가
 * handle_ea_bar() 의 네 갈래에 그대로 대응한다.
 * 공유 상태: 없다. 전역 변수도 static 변수도 두지 않으며, 위조에 필요한
 * 정보는 모두 그때그때 하드웨어에서 읽거나 상수로 박혀 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - set_val(): 이 파일의 모든 위조 값이 dword 로 만들어지는데 config 접근은
 *   1/2/4바이트로 들어오므로, 폭 맞추기를 한 곳에 모은 함수. 값을 만드는
 *   분기가 수십 개라 이 한 줄 호출이 코드를 읽을 만하게 유지한다.
 * - handle_ea_bar(): EA 항목 하나를 실제 BAR 에서 만들어 낸다. 오프셋의
 *   2~3번 비트로 네 워드 중 어느 것을 묻는지 가른다. 크기를 구할 때
 *   BAR 에 모두 1 을 썼다가 곧바로 되돌리는데, 그 사이 다른 코드가 BAR 을
 *   보면 엉뚱한 주소를 보게 되므로 구간을 최소로 유지한다.
 * - thunder_ecam_p2_config_read(): Pass-2 전용. 결함이 하나뿐이라 네 오프셋만
 *   걸러 12번 비트를 채운다. 나머지는 표준 읽기로 넘긴다.
 * - thunder_ecam_config_read(): 몸통. 리비전으로 Pass-1/Pass-2 를 가르고,
 *   Pass-1 이면 헤더 타입에 따라 일반 장치와 브리지를 다르게 위조한다.
 *   브리지는 EA 항목보다 버스 번호가 중요한데, 하드웨어가 세컨더리·
 *   서보디네이트를 보고하지 않아 devfn 으로 어느 내부 브리지인지 알아내
 *   고정 번호를 알려 준다.
 * - thunder_ecam_config_write(): 읽기 쪽 위조와 짝. BAR 과 SR-IOV BAR 범위의
 *   쓰기만 버리고 나머지는 통과시킨다. 읽기가 수백 줄인 데 비해 이쪽이
 *   짧은 것은, 위조가 "보여 주는 것" 의 문제이고 쓰기는 "무시하는 것" 만으로
 *   충분하기 때문이다.
 * - pci_thunder_ecam_ops: map_bus 는 표준 그대로, read/write 만 자기 것.
 *   const 전역이라 ACPI 쪽에서 이름으로 참조한다.
 *
 * === 이 트리에서 확인하지 못한 것 ===
 * 위조에 쓰이는 상수들(0x80ff0003 계열의 EA 항목 헤더, 0x00008430 같은
 * NIC 의 고정 상위 주소, 브리지별 버스 번호)의 근거는 Cavium 문서에만 있어
 * 확인할 수 없었다. 상류 영어 주석이 필드 이름을 풀어 준 곳은 그것을
 * 인용했고, 나머지는 코드가 하는 일만 적었다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. 이 파일이 다루는 것은 SoC 내부에 통합된 Cavium 전용 장치들
 * (NIC, TNS, ZIP, RAD, DFA)이고, ThunderX 보드에 꽂은 NVMe SSD 는 평범한
 * 외부 PCIe 장치라 위조 대상이 아니다 — thunder_ecam_config_read() 의 벤더
 * 검사(0x177d)와 devfn 검사에 걸리지 않아 no_emulation 경로로 내려간다.
 * 다만 이 파일이 보여 주는 것은 NVMe 독자에게도 의미가 있다. 커널이 장치를
 * 다루는 근거가 결국 config 공간에서 읽은 값이므로, 그 값을 가로채면
 * 하드웨어에 없는 기능도 있는 것처럼 만들 수 있다는 것이다.
 */

#include <linux/kernel.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/init.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/ioport.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/of_pci.h>
/* [한국어] of_device_id 표. */
#include <linux/of.h>
/* [한국어] struct pci_config_window, struct pci_ecam_ops, pci_ecam_map_bus(),
 * pci_generic_config_read/write(). */
#include <linux/pci-ecam.h>
/* [한국어] platform_driver 와 builtin_platform_driver. */
#include <linux/platform_device.h>

/* [한국어] pci_host_common_probe() — 이 드라이버는 probe 를 직접 쓰지 않고
 * 공용 골격을 그대로 빌려 쓴다. */
#include "pci-host-common.h"

/* [한국어] DT 경로(CONFIG_PCI_HOST_THUNDER_ECAM)나 ACPI 쿼크 경로 둘 중 하나라도
 * 켜져 있어야 컴파일된다. ACPI 쪽은 아래 pci_thunder_ecam_ops 만 쓰고
 * DT 쪽은 드라이버 등록까지 한다. */
#if defined(CONFIG_PCI_HOST_THUNDER_ECAM) || (defined(CONFIG_ACPI) && defined(CONFIG_PCI_QUIRKS))

/* [한국어]
 * set_val - dword 로 만든 값을 요청한 폭에 맞춰 호출자에게 돌려준다
 *
 * @v: 돌려줄 dword 값.
 * @where: 원래 요청된 config 오프셋. 하위 2비트가 바이트 위치다.
 * @size: 요청 폭(1/2/4).
 * @val: 결과를 담을 곳.
 *
 * 이 파일의 모든 위조 값은 dword 단위로 만들어지는데, config 접근은 1바이트나
 * 2바이트로도 들어온다. 그 폭 맞추기를 한 곳에 모은 함수다.
 *
 * 오프셋의 하위 2비트에 8을 곱해 시프트를 얻고, 폭에 따라 마스크를 씌운다.
 * 이 파일에 값을 만드는 분기가 수십 개나 되므로, 각 분기가 이 한 줄만 부르면
 * 되게 한 것이 코드를 읽을 만하게 유지한다.
 *
 * 실행 컨텍스트: config 읽기 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   thunder_ecam_config_read() / handle_ea_bar() / thunder_ecam_p2_config_read()
 *     → [이 함수]
 */
static void set_val(u32 v, int where, int size, u32 *val)
{
	/* [한국어] 오프셋 하위 2비트가 dword 안의 바이트 위치이므로 8을 곱해 비트 시프트로 만든다. */
	int shift = (where & 3) * 8;

	/* [한국어] 어느 dword 에 어떤 값을 넣는지 디버그 로그에 남긴다. */
	pr_debug("set_val %04x: %08x\n", (unsigned int)(where & ~3), v);
	/* [한국어] 요청한 바이트 위치까지 값을 내린다. */
	v >>= shift;
	/* [한국어] 1바이트 요청이면, */
	if (size == 1)
		/* [한국어] 하위 8비트만 남긴다. */
		v &= 0xff;
	/* [한국어] 2바이트 요청이면, */
	else if (size == 2)
		/* [한국어] 하위 16비트만. */
		v &= 0xffff;
	/* [한국어] 결과를 호출자에게 준다. 이 함수가 있는 이유는 이 파일의 모든 위조 값이
	 * dword 단위로 만들어지는데 config 접근은 1/2/4바이트 어느 폭으로도 오기
	 * 때문이다 — 폭 맞추기를 한 곳에 모았다. */
	*val = v;
}

/* [한국어]
 * handle_ea_bar - 실제 BAR 을 읽어 EA capability 항목 하나를 위조한다
 *
 * @e0: 항목의 첫 워드(헤더). 호출자가 상수로 준다 — 속성, BAR 번호, 크기를 담는다.
 * @bar: 근거로 삼을 실제 BAR 의 config 오프셋.
 * @bus: 대상 버스.
 * @devfn: 대상 장치.
 * @where: 요청된 config 오프셋.
 * @size: 요청 폭.
 * @val: 결과를 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * EA(Enhanced Allocation) 항목은 16바이트이고 네 워드로 이루어진다 —
 * 헤더, Base-L, Offset-L(크기), Base-H. 이 함수는 오프셋의 2~3번 비트로
 * 그중 어느 워드를 묻는지 판별해 각각 다르게 답한다.
 *
 * 헤더는 호출자가 준 상수를 그대로 돌려준다. 나머지 셋은 실제 BAR 을 읽어
 * 만든다 — 하드웨어가 EA 를 지원하지 않으므로, BAR 에 있는 진짜 주소를
 * EA 형식으로 옮겨 보여 주는 것이다.
 *
 * 크기를 알아내는 대목이 이 함수에서 가장 조심스러운 부분이다. BAR 에
 * 모두 1 을 써 넣고 되읽은 뒤 곧바로 원래 값을 되돌리는데, 그 사이에 다른
 * 코드가 BAR 을 보면 엉뚱한 주소를 보게 되므로 구간을 최대한 짧게 유지한다.
 * 되읽은 값을 뒤집으면 크기 마스크가 된다.
 *
 * 실행 컨텍스트: config 읽기 경로. 코어가 pci_lock 을 쥔 상태라 잠들 수 없다.
 *
 * 에러 경로: 주소 매핑이 실패하면 장치 없음으로 답한다.
 *
 * 호출 체인:
 *   thunder_ecam_config_read() → [이 함수] → map_bus() → readl/writel()
 *     → set_val()
 */
static int handle_ea_bar(u32 e0, int bar, struct pci_bus *bus,
			 unsigned int devfn, int where, int size, u32 *val)
{
	/* [한국어] 실제 BAR 을 읽을 주소. */
	void __iomem *addr;
	/* [한국어] 만들어 낼 값. */
	u32 v;

	/* Entries are 16-byte aligned; bits[2,3] select word in entry */
	/* [한국어] 옆의 영어 주석대로 EA 항목 하나가 16바이트이고, 오프셋의 2~3번 비트가
	 * 그 안의 어느 워드인지를 가리킨다. */
	int where_a = where & 0xc;

	/* [한국어] 첫 워드면, */
	if (where_a == 0) {
		/* [한국어] 호출자가 준 항목 헤더를 그대로 돌려준다. 이 값이 항목의 속성(BAR 번호,
		 * 속성, 크기)을 담고 있으며 위 호출부에서 상수로 준다. */
		set_val(e0, where, size, val);
		return PCIBIOS_SUCCESSFUL;
	}
	/* [한국어] 둘째 워드(Base-L)면, */
	if (where_a == 0x4) {
		/* [한국어] 실제 BAR 을 매핑한다. */
		addr = bus->ops->map_bus(bus, devfn, bar); /* BAR 0 */
		/* [한국어] 실패하면, */
		if (!addr)
			return PCIBIOS_DEVICE_NOT_FOUND;

		/* [한국어] BAR 값을 읽는다. */
		v = readl(addr);
		/* [한국어] 하위 4비트(플래그 자리)를 지우고, */
		v &= ~0xf;
		/* [한국어] EA 규격의 Base-L 표시(2 = 64비트 기준)를 넣는다. 실제 하드웨어 BAR 의
		 * 주소를 EA 항목 형식으로 옮기는 것이다. */
		v |= 2; /* EA entry-1. Base-L */
		/* [한국어] 폭에 맞춰 돌려준다. */
		set_val(v, where, size, val);
		return PCIBIOS_SUCCESSFUL;
	}
	/* [한국어] 셋째 워드(Offset-L, 즉 크기)면, */
	if (where_a == 0x8) {
		/* [한국어] 원래 BAR 값을 보관할 곳. */
		u32 barl_orig;
		/* [한국어] 되읽은 값. */
		u32 barl_rb;

		/* [한국어] BAR 을 매핑한다. */
		addr = bus->ops->map_bus(bus, devfn, bar); /* BAR 0 */
		/* [한국어] 실패하면, */
		if (!addr)
			return PCIBIOS_DEVICE_NOT_FOUND;

		/* [한국어] 현재 값을 보관한다. 아래에서 되돌려야 한다. */
		barl_orig = readl(addr + 0);
		/* [한국어] 모두 1 을 써 넣는다. BAR 크기를 알아내는 표준 방법으로, 장치가 디코딩하는
		 * 상위 비트만 1 로 남는다. */
		writel(0xffffffff, addr + 0);
		/* [한국어] 되읽는다. */
		barl_rb = readl(addr + 0);
		/* [한국어] 곧바로 원래 값을 되돌린다. 이 사이에 다른 코드가 BAR 을 보면 엉뚱한
		 * 주소를 보게 되므로 최대한 짧게 유지한다. */
		writel(barl_orig, addr + 0);
		/* zeros in unsettable bits */
		/* [한국어] 옆의 영어 주석대로 설정할 수 없는 비트를 0 으로 만든다. 되읽은 값을
		 * 뒤집으면 크기 마스크가 되고, 하위 2비트를 지워 정렬을 맞춘다. */
		v = ~barl_rb & ~3;
		/* [한국어] EA 규격의 Offset-L 표시를 넣는다. */
		v |= 0xc; /* EA entry-2. Offset-L */
		/* [한국어] 폭에 맞춰 돌려준다. */
		set_val(v, where, size, val);
		return PCIBIOS_SUCCESSFUL;
	}
	/* [한국어] 넷째 워드(Base-H)면, */
	if (where_a == 0xc) {
		/* [한국어] 다음 BAR(64비트 BAR 의 상위 워드)을 매핑한다. */
		addr = bus->ops->map_bus(bus, devfn, bar + 4); /* BAR 1 */
		/* [한국어] 실패하면, */
		if (!addr)
			return PCIBIOS_DEVICE_NOT_FOUND;

		/* [한국어] 상위 32비트를 그대로 쓴다. 하위 워드와 달리 플래그가 없어 가공이 없다. */
		v = readl(addr); /* EA entry-3. Base-H */
		/* [한국어] 폭에 맞춰 돌려준다. */
		set_val(v, where, size, val);
		return PCIBIOS_SUCCESSFUL;
	}
	/* [한국어] 2~3번 비트가 네 값뿐이라 여기 도달할 수 없지만, 컴파일러를 위해 둔다. */
	return PCIBIOS_DEVICE_NOT_FOUND;
}

/* [한국어]
 * thunder_ecam_p2_config_read - Pass-2 실리콘의 EA Base 상위 비트를 보정한다
 *
 * @bus: 대상 버스.
 * @devfn: 대상 장치.
 * @where: config 오프셋.
 * @size: 요청 폭.
 * @val: 결과를 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * Pass-2 실리콘은 EA capability 를 하드웨어로 제대로 제공한다. 결함이 하나만
 * 남아 있어, 이 파일의 방대한 Pass-1 위조 대신 한 줄만 고치면 된다.
 *
 * 그 결함은 함수 안의 영어 주석에 있다. 64비트 EA Base 의 44번 비트가 config
 * 접근 창의 같은 비트와 일치해야 하는데 하드웨어가 그것을 빠뜨리고 보고한다.
 * 상위 32비트를 다루는 중이므로 44 - 32 = 12번 비트를 창의 물리 주소에서
 * 가져와 채워 넣는다.
 *
 * switch 가 네 오프셋만 걸러 내고 나머지는 평범한 ECAM 읽기로 넘긴다.
 * 그 네 값이 0x14 씩 떨어져 있는 것은 EA 항목이 16바이트 간격이고 Base-H 가
 * 항목 안에서 같은 자리에 있기 때문이다.
 *
 * Pass-1 과의 대비가 이 파일의 구조를 설명해 준다. 같은 하드웨어의 두 리비전을
 * 한 드라이버가 다루되, 리비전 검사 한 번으로 완전히 다른 경로를 탄다.
 *
 * 실행 컨텍스트: config 읽기 경로. 잠들 수 없다.
 *
 * 에러 경로: 주소 매핑 실패는 장치 없음.
 *
 * 호출 체인:
 *   thunder_ecam_config_read() → [이 함수] → map_bus() → readl() → set_val()
 */
static int thunder_ecam_p2_config_read(struct pci_bus *bus, unsigned int devfn,
				       int where, int size, u32 *val)
{
	/* [한국어] ECAM 창 서술자. 아래에서 창의 물리 주소를 얻는 데 쓴다. */
	struct pci_config_window *cfg = bus->sysdata;
	/* [한국어] dword 경계로 내린 오프셋. 아래 switch 가 이 값으로 판정한다. */
	int where_a = where & ~3;
	/* [한국어] 읽을 주소. */
	void __iomem *addr;
	/* [한국어] 보정해 넣을 노드 비트. */
	u32 node_bits;
	/* [한국어] 읽은 값. */
	u32 v;

	/* EA Base[63:32] may be missing some bits ... */
	/* [한국어] 위 영어 주석대로 EA Base 의 상위 32비트가 놓이는 네 자리만 보정 대상이다. */
	switch (where_a) {
	/* [한국어] EA 항목 0 의 Base-H. */
	case 0xa8:
	/* [한국어] 항목 1 의 Base-H. */
	case 0xbc:
	/* [한국어] 항목 2 의 Base-H. */
	case 0xd0:
	/* [한국어] 항목 3 의 Base-H. 항목이 16바이트 간격이라 0x14 씩 떨어져 있다. */
	case 0xe4:
		/* [한국어] 네 자리 중 하나면 아래로 진행한다. */
		break;
	default:
		/* [한국어] 그 밖의 오프셋은 손볼 것이 없으므로 평범한 ECAM 읽기로 넘긴다. */
		return pci_generic_config_read(bus, devfn, where, size, val);
	}

	/* [한국어] 해당 dword 를 매핑한다. */
	addr = bus->ops->map_bus(bus, devfn, where_a);
	/* [한국어] 실패하면, */
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 하드웨어가 보고한 값을 읽는다. */
	v = readl(addr);

	/*
	 * Bit 44 of the 64-bit Base must match the same bit in
	 * the config space access window.  Since we are working with
	 * the high-order 32 bits, shift everything down by 32 bits.
	 */
	/* [한국어] 함수 안의 영어 주석이 이유를 밝힌다. 64비트 Base 의 44번 비트가 config
	 * 접근 창의 같은 비트와 일치해야 하는데, 하드웨어가 그 비트를 빠뜨리고
	 * 보고한다. 상위 32비트를 다루는 중이므로 44 - 32 = 12번 비트를 본다. */
	node_bits = upper_32_bits(cfg->res.start) & (1 << 12);

	/* [한국어] 빠진 비트를 채워 넣는다. 이 한 줄이 Pass-2 실리콘 결함의 보정 전부다. */
	v |= node_bits;
	/* [한국어] 폭에 맞춰 돌려준다. */
	set_val(v, where, size, val);

	return PCIBIOS_SUCCESSFUL;
}

/* [한국어]
 * thunder_ecam_config_read - 하드웨어에 없는 EA capability 를 통째로 위조해 보여 준다
 *
 * @bus: 대상 버스.
 * @devfn: 대상 장치.
 * @where: config 오프셋.
 * @size: 요청 폭.
 * @val: 결과를 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 이 파일의 몸통이자, drivers/pci 에서 가장 대담한 쿼크 중 하나다.
 *
 * 문제는 이렇다. Cavium ThunderX 의 내부 장치들은 BAR 로 주소를 알려 주지
 * 못하고, 대신 고정된 주소에 놓여 있다. 그것을 커널에 알리는 표준 수단이
 * EA(Enhanced Allocation) capability 인데, Pass-1 실리콘은 그 capability 를
 * 갖고 있지 않다. 그래서 이 함수가 config 읽기를 가로채 EA capability 를
 * **없는 자리에 만들어 낸다**.
 *
 * 만들어 내는 방법이 세 겹이다.
 * 1. capability 사슬을 잇는다. 0x70(또는 MSI-X 헤더)의 next 포인터를 0xbc 로
 *    바꿔, 실재하지 않는 EA capability 를 사슬에 끼워 넣는다.
 * 2. 0xbc 에 EA 헤더를 만든다. 항목 개수는 장치마다 다르다 — NIC 은 4,
 *    TNS 는 3, MSI-X 가 있으면 2, 그 밖에는 1.
 * 3. 0xc0 이후의 각 항목은 handle_ea_bar() 가 실제 BAR 을 읽어 만든다.
 *    같은 오프셋 범위를 장치에 따라 다른 BAR 로 채우는 것이 요령이다.
 *
 * BAR 자체는 0 으로 보이게 한다. 커널이 BAR 을 보고 주소를 재배정하려 들면
 * EA 가 알려 준 고정 주소와 어긋나기 때문이며, thunder_ecam_config_write() 가
 * BAR 쓰기를 조용히 버리는 것과 짝을 이룬다.
 *
 * 브리지(헤더 타입 1)는 다른 처리를 받는다. 하드웨어가 세컨더리·서보디네이트
 * 버스 번호를 보고하지 않아, devfn 으로 어느 내부 브리지인지 알아내 고정된
 * 번호를 알려 준다.
 *
 * 리비전이 8 이상이면 Pass-2 이므로 이 전부를 건너뛰고
 * thunder_ecam_p2_config_read() 로 넘긴다.
 *
 * 실행 컨텍스트: config 읽기 경로. 코어가 pci_lock 을 쥔 상태라 잠들 수 없다.
 *
 * 에러 경로: 주소 매핑 실패는 장치 없음. 장치가 없거나(0xffffffff) 위조
 * 대상이 아니면 no_emulation 으로 내려가 하드웨어 값을 그대로 준다.
 *
 * 호출 체인:
 *   pci_read_config_*() → PCI 코어 → [이 함수]
 *     → thunder_ecam_p2_config_read()(Pass-2) 또는
 *   handle_ea_bar() / set_val() / pci_generic_config_read()
 */
static int thunder_ecam_config_read(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 *val)
{
	/* [한국어] 만들어 낼 값. */
	u32 v;
	/* [한국어] 벤더/장치 ID. */
	u32 vendor_device;
	/* [한국어] 클래스 코드와 리비전. */
	u32 class_rev;
	/* [한국어] 읽을 주소. */
	void __iomem *addr;
	/* [한국어] 헤더 종류. */
	int cfg_type;
	/* [한국어] dword 경계로 내린 오프셋. */
	int where_a = where & ~3;

	/* [한국어] 헤더 종류가 담긴 dword(0x0c)를 매핑한다. */
	addr = bus->ops->map_bus(bus, devfn, 0xc);
	/* [한국어] 실패하면, */
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 읽는다. */
	v = readl(addr);

	/* Check for non type-00 header */
	/* [한국어] 옆의 영어 주석대로 헤더 종류를 뽑아낸다. 최상위 비트(멀티 기능 표시)를
	 * 0x7f 로 걸러 낸다. */
	cfg_type = (v >> 16) & 0x7f;

	/* [한국어] 클래스/리비전이 담긴 dword(0x08)를 매핑한다. */
	addr = bus->ops->map_bus(bus, devfn, 8);
	/* [한국어] 실패하면, */
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 읽는다. */
	class_rev = readl(addr);
	/* [한국어] 모든 비트가 1 이면 장치가 없다는 뜻이므로, */
	if (class_rev == 0xffffffff)
		goto no_emulation;

	/* [한국어] 리비전이 8 이상이면 Pass-2 실리콘이다. */
	if ((class_rev & 0xff) >= 8) {
		/* Pass-2 handling */
		/* [한국어] 옆의 영어 주석대로 Pass-2 에서는 브리지 헤더를 손보지 않는다. */
		if (cfg_type)
			goto no_emulation;
		/* [한국어] Pass-2 전용 처리로 넘긴다. 결함이 훨씬 적어 노드 비트 보정만 하면 된다. */
		return thunder_ecam_p2_config_read(bus, devfn, where,
						   size, val);
	}

	/*
	 * All BARs have fixed addresses specified by the EA
	 * capability; they must return zero on read.
	 */
	/* [한국어] 여기서부터 Pass-1 처리다. 일반 장치의 BAR 이나 SR-IOV BAR 범위면, */
	if (cfg_type == 0 &&
	    ((where >= 0x10 && where < 0x2c) ||
	     /* [한국어] 두 구간 모두 대상이다. */
	     (where >= 0x1a4 && where < 0x1bc))) {
		/* BAR or SR-IOV BAR */
		*val = 0;
		/* [한국어] 0 을 반환한다. 위 영어 주석대로 BAR 을 모두 0 으로 보이게 해 커널이
		 * 재배정을 시도하지 않게 하는 것이다 — EA(Enhanced Allocation)로 주소를
		 * 따로 알려 주므로 BAR 은 쓰이지 않아야 한다. */
		return PCIBIOS_SUCCESSFUL;
	}

	/* [한국어] 벤더/장치 ID 가 담긴 dword(0x00)를 매핑한다. */
	addr = bus->ops->map_bus(bus, devfn, 0);
	/* [한국어] 실패하면, */
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 읽는다. */
	vendor_device = readl(addr);
	/* [한국어] 장치가 없으면, */
	if (vendor_device == 0xffffffff)
		goto no_emulation;

	/* [한국어] 어느 장치의 어느 오프셋을 보정하는지 디버그 로그에 남긴다. */
	pr_debug("%04x:%04x - Fix pass#: %08x, where: %03x, devfn: %03x\n",
		 vendor_device & 0xffff, vendor_device >> 16, class_rev,
		 (unsigned int)where, devfn);

	/* Check for non type-00 header */
	/* [한국어] 일반 장치(헤더 타입 0)면, */
	if (cfg_type == 0) {
		/* [한국어] MSI-X capability 가 있는지. */
		bool has_msix;
		/* [한국어] 이 장치가 NIC 인지. 벤더 0x177d(Cavium)와 장치 0xa01e 의 조합이다. */
		bool is_nic = (vendor_device == 0xa01e177d);
		/* [한국어] TNS(네트워크 스위치)인지. 두 장치만 EA 항목 수가 다르다. */
		bool is_tns = (vendor_device == 0xa01f177d);

		/* [한국어] capability 사슬의 0x70 자리를 매핑한다. */
		addr = bus->ops->map_bus(bus, devfn, 0x70);
		/* [한국어] 실패하면, */
		if (!addr)
			return PCIBIOS_DEVICE_NOT_FOUND;

		/* E_CAP */
		/* [한국어] 읽는다. */
		v = readl(addr);
		/* [한국어] 다음 포인터 자리가 0 이 아니면 MSI-X 가 이어져 있다는 뜻이다. */
		has_msix = (v & 0xff00) != 0;

		/* [한국어] MSI-X 가 없고 이 자리를 읽는 중이면, */
		if (!has_msix && where_a == 0x70) {
			/* [한국어] 다음 capability 로 EA(0xbc)를 가리키게 만든다. 위조한 EA capability 를
			 * 사슬에 끼워 넣는 방법이다. */
			v |= 0xbc00; /* next capability is EA at 0xbc */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] MSI-X capability 헤더 자리면, */
		if (where_a == 0xb0) {
			/* [한국어] 매핑해서, */
			addr = bus->ops->map_bus(bus, devfn, where_a);
			/* [한국어] 실패하면, */
			if (!addr)
				return PCIBIOS_DEVICE_NOT_FOUND;

			/* [한국어] 읽는다. */
			v = readl(addr);
			/* [한국어] 다음 포인터가 이미 있으면 예상 밖의 구성이므로, */
			if (v & 0xff00)
				/* [한국어] 오류로 남긴다. */
				pr_err("Bad MSI-X cap header: %08x\n", v);
			/* [한국어] 어느 쪽이든 다음을 EA 로 바꾼다. MSI-X 뒤에 EA 를 잇는 것이다. */
			v |= 0xbc00; /* next capability is EA at 0xbc */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] EA capability 헤더 자리면, */
		if (where_a == 0xbc) {
			/* [한국어] NIC 이면, */
			if (is_nic)
				/* [한국어] 항목 4개짜리 EA 헤더를 만든다. 상위 바이트가 항목 수, 0x0014 가
				 * EA capability ID 와 "사슬의 끝" 표시다. */
				v = 0x40014; /* EA last in chain, 4 entries */
			/* [한국어] TNS 면, */
			else if (is_tns)
				/* [한국어] 항목 3개. */
				v = 0x30014; /* EA last in chain, 3 entries */
			/* [한국어] MSI-X 가 있으면, */
			else if (has_msix)
				/* [한국어] 항목 2개(BAR0 과 BAR4). */
				v = 0x20014; /* EA last in chain, 2 entries */
			else
				/* [한국어] 그 밖에는 항목 1개(BAR0 만). */
				v = 0x10014; /* EA last in chain, 1 entry */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] EA 항목 0 의 범위면, */
		if (where_a >= 0xc0 && where_a < 0xd0)
			/* EA entry-0. PP=0, BAR0 Size:3 */
			/* [한국어] BAR0 을 근거로 항목을 만든다. 0x80ff0003 은 활성화·쓰기 불가·
			 * SP=ff·BEI=0·ES=3 을 담은 항목 헤더다. */
			return handle_ea_bar(0x80ff0003,
					     0x10, bus, devfn, where,
					     size, val);
		/* [한국어] 항목 1 의 범위이고 MSI-X 가 있으면, */
		if (where_a >= 0xd0 && where_a < 0xe0 && has_msix)
			 /* EA entry-1. PP=0, BAR4 Size:3 */
			/* [한국어] BAR4 를 근거로 만든다. MSI-X 테이블이 그 BAR 에 있기 때문이다. */
			return handle_ea_bar(0x80ff0043,
					     0x20, bus, devfn, where,
					     size, val);
		/* [한국어] 항목 2 의 범위이고 TNS 면, */
		if (where_a >= 0xe0 && where_a < 0xf0 && is_tns)
			/* EA entry-2. PP=0, BAR2, Size:3 */
			/* [한국어] BAR2 를 근거로. */
			return handle_ea_bar(0x80ff0023,
					     0x18, bus, devfn, where,
					     size, val);
		/* [한국어] 항목 2 의 범위이고 NIC 이면, */
		if (where_a >= 0xe0 && where_a < 0xf0 && is_nic)
			/* EA entry-2. PP=4, VF_BAR0 (9), Size:3 */
			/* [한국어] VF BAR0 을 근거로 만든다. 같은 자리를 두 장치가 다른 의미로 쓰는 것이
			 * 이 위조의 특징이다 — 장치마다 EA 항목 구성이 다르다. */
			return handle_ea_bar(0x80ff0493,
					     0x1a4, bus, devfn, where,
					     size, val);
		/* [한국어] 항목 3 의 범위이고 NIC 이면, */
		if (where_a >= 0xf0 && where_a < 0x100 && is_nic)
			/* EA entry-3. PP=4, VF_BAR4 (d), Size:3 */
			/* [한국어] VF BAR4 를 근거로. */
			return handle_ea_bar(0x80ff04d3,
					     0x1b4, bus, devfn, where,
					     size, val);
	/* [한국어] 브리지(헤더 타입 1)면, */
	} else if (cfg_type == 1) {
		/* [한국어] devfn 으로 어느 내부 브리지인지 구분한다. 이 SoC 의 내부 브리지들은
		 * 고정된 devfn 을 갖는다. */
		bool is_rsl_bridge = devfn == 0x08;
		/* [한국어] RAD 브리지. */
		bool is_rad_bridge = devfn == 0xa0;
		/* [한국어] ZIP 브리지. */
		bool is_zip_bridge = devfn == 0xa8;
		/* [한국어] DFA 브리지. */
		bool is_dfa_bridge = devfn == 0xb0;
		/* [한국어] NIC 브리지. */
		bool is_nic_bridge = devfn == 0x10;

		/* [한국어] PCIe capability 헤더 자리면, */
		if (where_a == 0x70) {
			/* [한국어] 매핑해서, */
			addr = bus->ops->map_bus(bus, devfn, where_a);
			/* [한국어] 실패하면, */
			if (!addr)
				return PCIBIOS_DEVICE_NOT_FOUND;

			/* [한국어] 읽는다. */
			v = readl(addr);
			/* [한국어] 다음 포인터가 이미 있으면 예상 밖이므로, */
			if (v & 0xff00)
				/* [한국어] 오류로 남긴다. */
				pr_err("Bad PCIe cap header: %08x\n", v);
			/* [한국어] 다음을 EA 로 바꾼다. */
			v |= 0xbc00; /* next capability is EA at 0xbc */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] EA capability 헤더 자리면, */
		if (where_a == 0xbc) {
			/* [한국어] NIC 브리지면, */
			if (is_nic_bridge)
				/* [한국어] 항목 1개. */
				v = 0x10014; /* EA last in chain, 1 entry */
			else
				/* [한국어] 그 밖의 브리지는 항목 없음. 브리지에는 EA 항목이 필요 없지만 버스 번호는
				 * 알려 주어야 하므로 헤더만 둔다. */
				v = 0x00014; /* EA last in chain, no entries */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] EA 의 버스 번호 자리면, */
		if (where_a == 0xc0) {
			/* [한국어] RSL 이나 NIC 브리지는, */
			if (is_rsl_bridge || is_nic_bridge)
				/* [한국어] 세컨더리·서보디네이트를 1 로. */
				v = 0x0101; /* subordinate:secondary = 1:1 */
			/* [한국어] RAD 브리지는, */
			else if (is_rad_bridge)
				/* [한국어] 2 로. */
				v = 0x0202; /* subordinate:secondary = 2:2 */
			/* [한국어] ZIP 브리지는, */
			else if (is_zip_bridge)
				/* [한국어] 3 으로. */
				v = 0x0303; /* subordinate:secondary = 3:3 */
			/* [한국어] DFA 브리지는, */
			else if (is_dfa_bridge)
				/* [한국어] 4 로. 브리지마다 버스 번호를 고정으로 알려 주는 것으로, 하드웨어가
				 * 그 값을 보고하지 않기 때문이다. */
				v = 0x0404; /* subordinate:secondary = 4:4 */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] NIC 브리지의 EA 항목 헤더 자리면, */
		if (where_a == 0xc4 && is_nic_bridge) {
			/* Enabled, not-Write, SP=ff, PP=05, BEI=6, ES=4 */
			/* [한국어] 옆의 영어 주석이 필드를 풀어 준다 — 활성화, 쓰기 불가, SP=ff, PP=05,
			 * BEI=6, ES=4. */
			v = 0x80ff0564;
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] 그 항목의 Base-L 자리면, */
		if (where_a == 0xc8 && is_nic_bridge) {
			/* [한국어] 64비트 기준임을 표시한다. */
			v = 0x00000002; /* Base-L 64-bit */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] MaxOffset-L 자리면, */
		if (where_a == 0xcc && is_nic_bridge) {
			/* [한국어] 거의 전체를 덮는 크기를 알려 준다. */
			v = 0xfffffffe; /* MaxOffset-L 64-bit */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] Base-H 자리면, */
		if (where_a == 0xd0 && is_nic_bridge) {
			/* [한국어] NIC 의 실제 상위 주소를 알려 준다. 하드웨어에서 읽는 것이 아니라
			 * 이 SoC 의 고정 주소를 그대로 박아 둔 것이다. */
			v = 0x00008430; /* NIC Base-H */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
		/* [한국어] MaxOffset-H 자리면, */
		if (where_a == 0xd4 && is_nic_bridge) {
			/* [한국어] 상위 크기. */
			v = 0x0000000f; /* MaxOffset-H */
			/* [한국어] 돌려주고, */
			set_val(v, where, size, val);
			return PCIBIOS_SUCCESSFUL;
		}
	}
/* [한국어] 위 어느 경우에도 해당하지 않으면 위조 없이 하드웨어 값을 그대로 준다. */
no_emulation:
	return pci_generic_config_read(bus, devfn, where, size, val);
}

/* [한국어]
 * thunder_ecam_config_write - BAR 쓰기만 조용히 버리고 나머지는 통과시킨다
 *
 * @bus: 대상 버스.
 * @devfn: 대상 장치.
 * @where: config 오프셋.
 * @size: 요청 폭.
 * @val: 쓸 값.
 * @return: 언제나 PCIBIOS_SUCCESSFUL 또는 아래 호출의 결과.
 *
 * 읽기 쪽의 위조와 짝을 이루는 처리다. thunder_ecam_config_read() 가 BAR 을
 * 0 으로 보여 주므로 커널은 그 BAR 이 배정되지 않았다고 판단하고 주소를
 * 써 넣으려 한다. 그 쓰기를 실제로 반영하면 EA 가 알려 준 고정 주소와
 * 어긋나 장치에 닿지 못하게 된다.
 *
 * 그래서 BAR 과 SR-IOV BAR 범위의 쓰기를 성공으로 답하고 버린다.
 * 읽기 함수의 위조 구간과 정확히 같은 두 범위다.
 *
 * 읽기 쪽이 수백 줄인 데 비해 쓰기 쪽이 이렇게 짧은 것은, 위조가 "보여 주는
 * 것" 의 문제이고 쓰기는 "무시하는 것" 만으로 충분하기 때문이다.
 *
 * 실행 컨텍스트: config 쓰기 경로. 잠들 수 없다.
 *
 * 에러 경로: 없다. 버려진 쓰기도 성공으로 보고한다.
 *
 * 호출 체인:
 *   pci_write_config_*() → PCI 코어 → [이 함수] → pci_generic_config_write()
 */
static int thunder_ecam_config_write(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 val)
{
	/*
	 * All BARs have fixed addresses; ignore BAR writes so they
	 * don't get corrupted.
	 */
	/* [한국어] 쓰기 쪽은 훨씬 단순하다. BAR 이나 SR-IOV BAR 범위면, */
	if ((where >= 0x10 && where < 0x2c) ||
	    (where >= 0x1a4 && where < 0x1bc))
		/* [한국어] 두 구간 모두. */
		/* BAR or SR-IOV BAR */
		/* [한국어] 쓰기를 조용히 버린다. 읽기가 0 을 보여 주므로 커널이 BAR 을 재배정하려
		 * 할 텐데, 실제로 쓰면 EA 가 알려 준 주소와 어긋나기 때문이다.
		 * 읽기의 위조와 짝을 이루는 처리다. */
		return PCIBIOS_SUCCESSFUL;

	/* [한국어] 그 밖의 쓰기는 그대로 통과시킨다. */
	return pci_generic_config_write(bus, devfn, where, size, val);
}

const struct pci_ecam_ops pci_thunder_ecam_ops = {
	.pci_ops	= {
		/* [한국어] 주소 계산은 표준 ECAM 그대로다. 이 드라이버가 손보는 것은 읽어 온
		 * **값** 이지 주소가 아니다. */
		.map_bus        = pci_ecam_map_bus,
		/* [한국어] 위조하는 읽기. */
		.read           = thunder_ecam_config_read,
		/* [한국어] BAR 쓰기를 버리는 쓰기. */
		.write          = thunder_ecam_config_write,
	}
/* [한국어] const 전역이라 ACPI 쪽 코드가 이름으로 참조할 수 있다. */
};

/* [한국어] DT 경로에서만 아래 드라이버 등록이 필요하다. ACPI 경로는 위 ops 표만
 * 쓰기 때문이다. */
#ifdef CONFIG_PCI_HOST_THUNDER_ECAM

static const struct of_device_id thunder_ecam_of_match[] = {
	{
		/* [한국어] 이 드라이버가 맡는 유일한 compatible. */
		.compatible = "cavium,pci-host-thunder-ecam",
		/* [한국어] 위 ops 표를 매치 데이터로 붙인다. */
		.data = &pci_thunder_ecam_ops,
	},
	/* [한국어] 배열 끝. */
	{ },
};

static struct platform_driver thunder_ecam_driver = {
	.driver = {
		/* [한국어] 모듈 이름을 그대로 드라이버 이름으로 쓴다. */
		.name = KBUILD_MODNAME,
		/* [한국어] 위 compatible 표. */
		.of_match_table = thunder_ecam_of_match,
		/* [한국어] sysfs 로 bind/unbind 를 막는다. 호스트 브리지를 런타임에 떼면 그 아래
		 * 모든 장치가 사라지기 때문이다. */
		.suppress_bind_attrs = true,
	},
	/* [한국어] probe 를 직접 쓰지 않고 공용 골격을 그대로 빌린다. 이 드라이버가
	 * 고유하게 하는 일은 위 ops 표의 두 콜백뿐이다. */
	.probe = pci_host_common_probe,
};
/* [한국어] 모듈이 아니라 내장 드라이버로 등록한다. */
builtin_platform_driver(thunder_ecam_driver);

/* [한국어] DT 경로 끝. */
#endif
/* [한국어] 파일 전체를 감싼 바깥 조건 끝. */
#endif
