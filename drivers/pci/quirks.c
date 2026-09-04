// SPDX-License-Identifier: GPL-2.0
/*
 * This file contains work-arounds for many known PCI hardware bugs.
 * Devices present only on certain architectures (host bridges et cetera)
 * should be handled in arch-specific code.
 *
 * Note: any quirks for hotpluggable devices must _NOT_ be declared __init.
 *
 * Copyright (c) 1999 Martin Mares <mj@ucw.cz>
 *
 * Init/reset quirks for USB host controllers should be in the USB quirks
 * file, where their drivers can use them.
 */

/*
 * [한국어 설명] PCI 하드웨어 버그 우회(quirk) 모음 (drivers/pci/quirks.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 "PCI/PCIe 스펙대로 동작하지 않는 실제 하드웨어"를 커널이 알고
 * 피해 가도록 만드는 예외 처리(quirk)를 한곳에 모은 곳이다. PCI 열거 코드는
 * 스펙을 신뢰하고 작성되어 있으므로, Configuration Space 에 거짓말을 적어
 * 두거나(예: BAR 크기·클래스 코드·capability 목록을 잘못 보고) 규정된 동작을
 * 하지 않는 칩(예: MSI 를 광고하지만 실제로는 인터럽트를 쏘지 못하는 칩)이
 * 있으면 열거 자체가 깨지거나 장치가 조용히 오동작한다. 그래서 벤더 ID /
 * 디바이스 ID / 클래스 코드로 문제 있는 칩을 골라내어, 열거의 특정 시점에
 * 보정 함수를 끼워 넣는다. 그 보정 함수 하나하나가 이 파일의 quirk 이다.
 * 동시에 이 파일은 quirk 를 "등록·검색·실행"하는 엔진(pci_do_fixups(),
 * pci_fixup_device())과, 장치별 리셋 방법 표(pci_dev_reset_methods[]),
 * ACS(Access Control Services) 우회 표(pci_dev_acs_enabled[]),
 * DMA alias 고정표(fixed_dma_alias_tbl[]) 같은 "표 기반 예외"도 담는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 서브시스템의 흐름은 크게 (1) 버스 스캔/열거 -> (2) 리소스 할당 ->
 * (3) 드라이버 바인딩/enable -> (4) 런타임 전원관리·리셋·오류복구 이다.
 * 이 파일은 그 네 단계 "사이사이"에 갈고리(hook)로 끼어든다. 커널은 각
 * 단계에서 pci_fixup_device(pass, dev) 를 부르고, 이 함수가 해당 pass 에
 * 등록된 quirk 배열을 훑어 (vendor, device, class) 가 맞는 것만 실행한다.
 * 실행 컨텍스트는 대부분 부팅 시 프로세스 문맥(초기화 스레드)이며, 핫플러그
 * 경로에서는 핫플러그 워커 문맥, resume 계열은 시스템 재개 경로에서 불린다.
 * 인터럽트 문맥에서 불리지 않으므로 quirk 안에서 msleep() 같은 잠자는 호출이
 * 허용된다(실제로 여러 quirk 가 msleep 을 쓴다).
 * NVMe SSD 는 PCIe 엔드포인트이므로, NVMe 드라이버가 장치를 켜기 전에 이미
 * early/header 단계 quirk 가 그 장치에 적용되어 있다. 켜는 경로는
 * drivers/nvme/host/pci.c 의 nvme_probe() -> nvme_pci_enable() ->
 * pci_enable_device_mem() 이며, 그 pci_enable_device_mem() 호출이
 * enable 단계 quirk 를 지나간다.
 *
 * === 타 모듈과의 연결 ===
 * - drivers/pci/pci.h ("pci.h" include): pci_dev_specific_reset(),
 *   pci_dev_specific_acs_enabled() 등 이 파일이 밖으로 내주는 내부 API 의
 *   선언과, pci_cache_line_size 같은 서브시스템 전역 변수가 여기에 있다.
 * - include/linux/pci.h: struct pci_fixup 정의와 DECLARE_PCI_FIXUP_* 매크로
 *   정의가 있다. 이 sparse checkout 에는 그 헤더가 없어 매크로 본문과 링커
 *   섹션 배치는 이 트리만으로는 확인할 수 없다. 다만 이 파일 자신이
 *   __start_pci_fixups_early[] 같은 경계 심볼을 extern 으로 선언하고
 *   pci_do_fixups() 가 그 배열을 훑으므로, "링커가 모아 준 함수 포인터 배열"
 *   이라는 구조는 이 파일 안에서 확인된다.
 * - IOMMU 계층: pci_dev_specific_acs_enabled() 와 DMA alias quirk 는
 *   IOMMU 그룹 분리 판정에 쓰인다. ACS 를 제대로 구현하지 않은 스위치·루트
 *   포트를 그대로 두면 서로 다른 장치가 한 IOMMU 그룹으로 묶여 VFIO 패스스루가
 *   막히거나, 반대로 격리되지 않은 장치를 격리된 것으로 오인하게 된다.
 * - 전원 관리 코어: resume/suspend pass 의 quirk 가 시스템 재개 시 불린다.
 * - drivers/nvme/host/pci.c: 이 파일의 nvme_disable_and_flr() 는 NVMe
 *   컨트롤러를 CC.EN=0 으로 내린 뒤 FLR 을 거는 장치별 리셋 방법이다.
 *   NVMe 드라이버가 직접 부르지는 않고, PCI 코어의 리셋 경로가 부른다.
 *
 * === 주요 함수/구조체 요약 ===
 * - pci_do_fixups(): quirk 배열 [f, end) 구간을 훑어 vendor/device/class 가
 *   맞는 항목의 hook 을 호출하는 실행 엔진.
 * - pci_fixup_device(): pass(단계) 를 받아 그 단계의 배열 경계를 고른 뒤
 *   pci_do_fixups() 에 넘긴다. PCI 코어 곳곳에서 불리는 유일한 진입점.
 * - pci_apply_final_quirks(): fs_initcall 로 도는 늦은 단계. 모든 PCI 장치에
 *   final quirk 를 적용하고 시스템 공통 Cache Line Size 를 정한다.
 * - pci_dev_specific_reset(): 표준 리셋(FLR 등)으로는 살아나지 않는 장치를
 *   위한 벤더별 리셋 루틴 디스패처. NVMe 접점이 여기 있다.
 * - pci_dev_specific_acs_enabled(): ACS capability 를 광고하지 않지만 실제로는
 *   피어투피어 전달을 하지 않는(=격리된) 칩을 화이트리스트로 인정해 준다.
 *
 * === DECLARE_PCI_FIXUP_* 매크로가 동작하는 방식 ===
 * 이 파일의 quirk 함수들은 파일 안 어디에서도 이름으로 호출되지 않는다.
 * 대신 함수 정의 바로 아래의 DECLARE_PCI_FIXUP_<단계>(vendor, device, hook)
 * 한 줄이 "이 vendor:device 를 만나면 <단계> 시점에 hook 을 불러라" 라는
 * 항목(struct pci_fixup) 하나를 만들어 특별한 링커 섹션에 넣는다. 링커는
 * 같은 섹션의 항목을 커널 이미지 안에 연속 배열로 모아 주고, 그 배열의
 * 시작/끝 주소가 __start_pci_fixups_<단계> / __end_pci_fixups_<단계> 심볼로
 * 나온다(이 파일 상단에 extern 선언되어 있다). 그래서 "호출자가 없어 보이는데
 * 실행되는" 현상이 생긴다 - 호출은 이름이 아니라 섹션 순회로 이뤄진다.
 * DECLARE_PCI_FIXUP_CLASS_* 계열은 vendor/device 대신(또는 함께) 클래스 코드로
 * 매칭하는 변종이며, class_shift 로 클래스 코드의 상위 몇 바이트만 비교할지
 * 정한다(예: 스토리지 전체 vs NVMe 프로그래밍 인터페이스까지).
 *
 * 각 단계(pass)가 언제인지 - pci_fixup_device() 의 switch 가 그대로 목록이다:
 * - pci_fixup_early: 장치를 막 발견해 struct pci_dev 를 채우기 시작한 아주
 *   이른 시점. 헤더 타입·클래스 코드처럼 "이후 열거 판단의 전제"가 되는 값을
 *   고쳐야 하는 quirk 가 여기 온다.
 * - pci_fixup_header: 표준 헤더(BAR, 클래스, 인터럽트 핀 등)를 읽어 넣은 직후.
 *   BAR 값 보정, 숨은 리소스 예약, 잘못된 클래스 코드 수정이 여기 온다.
 *   이 파일에서 가장 많이 쓰이는 단계다.
 * - pci_fixup_final: 열거가 모두 끝난 뒤. fs_initcall 인
 *   pci_apply_final_quirks() 가 한 번에 돌린다. 그래서 이 단계 이전에 이미
 *   등록된 장치에도 소급 적용된다.
 * - pci_fixup_enable: pci_enable_device() 경로에서 장치를 실제로 켤 때.
 *   장치를 쓰기 직전에만 필요한 보정(예: 특정 브리지의 IO 창 열기).
 * - pci_fixup_resume / pci_fixup_resume_early: 시스템 재개 시. early 쪽은
 *   장치 상태 복원(pci_restore_state) 전후의 이른 시점으로, 펌웨어가 되돌려
 *   놓은 레지스터를 다시 고쳐야 하는 quirk 가 온다.
 * - pci_fixup_suspend / pci_fixup_suspend_late: 절전 진입 시.
 *
 * === 이 파일을 읽을 때의 요령 ===
 * quirk 함수 하나를 읽을 때 물어야 할 것은 늘 같다. (1) 어떤 벤더의 어떤
 * 칩인가, (2) 그 칩이 스펙의 무엇을 어겼거나 무엇을 잘못 보고하는가,
 * (3) 그대로 두면 무슨 증상이 생기는가, (4) 커널이 어떻게 우회하는가.
 * 원본 영어 주석에 그 배경이 적혀 있는 경우가 많으므로, 아래 한국어 주석은
 * 영어 주석이 말하는 바를 옮기고 부족한 곳만 코드로 확인해 채웠다. 원본에도
 * 없고 코드로도 알 수 없는 배경은 "이 트리의 정보만으로는 확인할 수 없다"고
 * 밝혀 두었다.
 */

/* [한국어] AER(Advanced Error Reporting) capability 의 레지스터 오프셋과
 * 비트 정의. 이 파일의 일부 quirk 가 잘못 배치된 AER capability 를 찾아
 * 고치거나 AER 을 무력화하는 데 쓴다. */
#include <linux/aer.h>
/* [한국어] ALIGN()/ALIGN_DOWN() 등 정렬 매크로. BAR 리소스 크기를 정렬 경계에
 * 맞춰 예약하는 quirk 에서 쓴다. */
#include <linux/align.h>
/* [한국어] FIELD_GET()/FIELD_PREP() 비트필드 추출 매크로. 레지스터의 특정
 * 비트 구간만 뽑아 쓰는 quirk 에서 매직 시프트 대신 쓴다. */
#include <linux/bitfield.h>
/* [한국어] u8/u16/u32 등 커널 고정폭 정수형. Configuration Space 접근 API 가
 * 이 형을 그대로 쓰므로 필수다. */
#include <linux/types.h>
/* [한국어] ARRAY_SIZE(), min/max, printk 계열 등 범용 매크로. */
#include <linux/kernel.h>
/* [한국어] EXPORT_SYMBOL(). 이 파일은 pci_fixup_device() 등을 모듈에
 * 내보내므로 필요하다. */
#include <linux/export.h>
/* [한국어] struct pci_dev, struct pci_fixup, pci_read_config_ 계열 API,
 * PCI_VENDOR_ID_* 상수, DECLARE_PCI_FIXUP_* 매크로가 모두 여기 있다.
 * 이 파일의 핵심 의존성이다. */
#include <linux/pci.h>
/* [한국어] isa_dma_bridge_buggy 전역 플래그의 선언. 특정 사우스브리지에서
 * ISA DMA 가 PCI 버스 마스터와 동시에 돌면 데이터가 깨지는 문제가 있어,
 * 해당 칩을 만난 quirk 가 이 플래그를 올리면 ISA DMA 사용자가 회피한다. */
#include <linux/isa-dma.h> /* isa_dma_bridge_buggy */
/* [한국어] __init/__initdata 섹션 표시자. 부팅 후 버려도 되는 quirk 는
 * __init 를 붙인다(단, 핫플러그 장치용 quirk 는 붙이면 안 된다 - 위 영어
 * 주석의 경고 참조). */
#include <linux/init.h>
/* [한국어] IOMMU 코어 API. ACS/DMA alias quirk 가 IOMMU 그룹 판정과 얽히고,
 * 리셋 경로에서 pci_dev_reset_iommu_prepare() 를 쓰기 위해 필요하다. */
#include <linux/iommu.h>
/* [한국어] udelay()/msleep(). 하드웨어가 상태를 바꿀 때까지 기다려야 하는
 * quirk 가 많다(예: NVMe CC.EN 을 내린 뒤 CSTS.RDY 가 0 이 되기를 대기). */
#include <linux/delay.h>
/* [한국어] ACPI 인터페이스. 일부 quirk 는 펌웨어(ACPI) 가 알려 준 정보를
 * 참고하거나 ACPI 가 이미 손댄 장치를 건드리지 않도록 판단한다. */
#include <linux/acpi.h>
/* [한국어] DMI(메인보드 식별) 테이블 조회. 같은 칩이라도 특정 보드에서만
 * 문제가 되는 경우가 있어 보드 모델로 quirk 대상을 좁힌다. */
#include <linux/dmi.h>
/* [한국어] struct resource 와 request_region() 계열. 펌웨어가 BAR 로
 * 광고하지 않은 숨은 IO/MEM 영역을 예약하는 quirk 에서 쓴다. */
#include <linux/ioport.h>
/* [한국어] task_pid_nr(current) 등. fixup 실행 시간을 찍는 디버그 출력에서
 * 현재 태스크 정보를 얻는 데 쓴다. */
#include <linux/sched.h>
/* [한국어] ktime_get()/ktime_sub(). 각 quirk 가 얼마나 오래 걸렸는지 재어
 * 10ms 를 넘으면 경고를 찍는 데 쓴다(fixup_debug_report()). */
#include <linux/ktime.h>
/* [한국어] 메모리 관리 상수(PAGE_SIZE 등). 리소스 크기 계산에 쓴다. */
#include <linux/mm.h>
/* [한국어] NVMe 컨트롤러 레지스터 정의(NVME_REG_CC, NVME_REG_CSTS,
 * NVME_CC_ENABLE, NVME_CAP_TIMEOUT 등). nvme_disable_and_flr() 이 BAR0 에
 * 매핑된 NVMe 레지스터를 직접 건드리므로 필요하다. 이 파일이 NVMe 를
 * 아는 유일한 이유가 이 헤더다. */
#include <linux/nvme.h>
/* [한국어] x86_apple_machine 판정. 애플 하드웨어에서만 필요한 quirk 를
 * 구분하는 데 쓴다. */
#include <linux/platform_data/x86/apple.h>
/* [한국어] 런타임 전원관리 API. 일부 quirk 가 장치의 runtime PM 참조수를
 * 조정하거나 D3 진입을 막는다. */
#include <linux/pm_runtime.h>
/* [한국어] SZ_1M, SZ_256M 같은 크기 상수. 하드코딩된 리소스 크기를 읽기 쉽게
 * 적기 위해 쓴다. */
#include <linux/sizes.h>
/* [한국어] 시스템 절전 상태 질의(pm_suspend_target_state 등). 절전 종류에
 * 따라 다르게 동작해야 하는 quirk 가 참고한다. */
#include <linux/suspend.h>
/* [한국어] Microsemi Switchtec PCIe 스위치의 관리 인터페이스 정의.
 * 파일 끝의 SWITCHTEC_QUIRK 계열이 이 스위치의 NTB 클래스 코드를 고친다. */
#include <linux/switchtec.h>
/* [한국어] drivers/pci/pci.h - PCI 서브시스템 내부 전용 헤더. 이 파일이
 * 구현해 코어에 제공하는 pci_dev_specific_reset() 등의 선언, 그리고
 * pci_cache_line_size 같은 내부 전역이 여기 있다. */
#include "pci.h"

/*
 * [한국어]
 * pcie_lbms_seen - 링크 대역폭 관리 상태(LBMS)가 관측된 적이 있는지 판정
 *
 * @dev: 검사 대상 PCIe 다운스트림 포트
 * @lnksta: 방금 읽어 둔 Link Status 레지스터(PCI_EXP_LNKSTA) 값
 * @return: 과거에 한 번이라도 LBMS 가 섰던 적이 있거나 지금 서 있으면 true
 *
 * LBMS(Link Bandwidth Management Status)는 하드웨어가 링크의 속도나 폭을
 * 스스로 바꿨을 때 서는 비트다. 이 비트는 읽고 지우는(RW1C) 성격이라 다른
 * 코드가 이미 지워 버렸을 수 있다. 그래서 PCI 코어는 이 비트를 볼 때마다
 * dev->priv_flags 의 PCI_LINK_LBMS_SEEN 에 "본 적 있음"을 기록해 둔다.
 * 이 함수는 그 기록과 현재 레지스터 값을 OR 로 합쳐 판정한다 - 즉 "지금
 * 서 있거나, 과거에 섰던 적이 있으면" 참이다.
 *
 * 실행 컨텍스트: 아래 pcie_failed_link_retrain() 에서만 불리며 프로세스
 * 문맥이다. test_bit() 은 원자적 읽기지만 별도 락은 쓰지 않는다 - 이 플래그는
 * 단조 증가(한 번 서면 지워지지 않음)라 경쟁해도 결과가 뒤집히지 않는다.
 *
 * 호출 체인:
 *   pcie_failed_link_retrain() -> [pcie_lbms_seen] -> test_bit()
 */
static bool pcie_lbms_seen(struct pci_dev *dev, u16 lnksta)
{
	/* [한국어] PCI 코어가 남겨 둔 "LBMS 를 본 적 있음" 기록을 먼저 확인한다.
	 * dev->priv_flags 는 PCI 코어 내부용 비트맵이고 PCI_LINK_LBMS_SEEN 이
	 * 그중 한 비트다. 기록이 있으면 레지스터를 볼 필요도 없다. */
	if (test_bit(PCI_LINK_LBMS_SEEN, &dev->priv_flags))
		return true;

	/* [한국어] 기록이 없으면 방금 읽은 Link Status 의 LBMS 비트를 본다.
	 * PCI_EXP_LNKSTA_LBMS 는 PCIe 스펙의 Link Status 레지스터 bit 14 다.
	 * 마스킹 결과(0 또는 0x4000)가 bool 로 변환되어 반환된다. */
	return lnksta & PCI_EXP_LNKSTA_LBMS;
}

/*
 * Retrain the link of a downstream PCIe port by hand if necessary.
 *
 * This is needed at least where a downstream port of the ASMedia ASM2824
 * Gen 3 switch is wired to the upstream port of the Pericom PI7C9X2G304
 * Gen 2 switch, and observed with the Delock Riser Card PCI Express x1 >
 * 2 x PCIe x1 device, P/N 41433, plugged into the SiFive HiFive Unmatched
 * board.
 *
 * In such a configuration the switches are supposed to negotiate the link
 * speed of preferably 5.0GT/s, falling back to 2.5GT/s.  However the link
 * continues switching between the two speeds indefinitely and the data
 * link layer never reaches the active state, with link training reported
 * repeatedly active ~84% of the time.  Forcing the target link speed to
 * 2.5GT/s with the upstream ASM2824 device makes the two switches talk to
 * each other correctly however.  And more interestingly retraining with a
 * higher target link speed afterwards lets the two successfully negotiate
 * 5.0GT/s.
 *
 * With the ASM2824 we can rely on the otherwise optional Data Link Layer
 * Link Active status bit and in the failed link training scenario it will
 * be off along with the Link Bandwidth Management Status indicating that
 * hardware has changed the link speed or width in an attempt to correct
 * unreliable link operation.  For a port that has been left unconnected
 * both bits will be clear.  So use this information to detect the problem
 * rather than polling the Link Training bit and watching out for flips or
 * at least the active status.
 *
 * Since the exact nature of the problem isn't known and in principle this
 * could trigger where an ASM2824 device is downstream rather upstream,
 * apply this erratum workaround to any downstream ports as long as they
 * support Link Active reporting and have the Link Control 2 register.
 * Restrict the speed to 2.5GT/s then with the Target Link Speed field,
 * request a retrain and check the result.
 *
 * If this turns out successful and we know by the Vendor:Device ID it is
 * safe to do so, then lift the restriction, letting the devices negotiate
 * a higher speed.  Also check for a similar 2.5GT/s speed restriction the
 * firmware may have already arranged and lift it with ports that already
 * report their data link being up.
 *
 * Otherwise revert the speed to the original setting and request a retrain
 * again to remove any residual state, ignoring the result as it's supposed
 * to fail anyway.
 *
 * Return 0 if the link has been successfully retrained.  Return an error
 * if retraining was not needed or we attempted a retrain and it failed.
 */
/*
 * [한국어]
 * pcie_failed_link_retrain - 링크 트레이닝에 실패한 다운스트림 포트를 손으로 재훈련
 *
 * @dev: 재훈련 대상 PCIe 다운스트림 포트(루트 포트 또는 스위치 다운스트림 포트)
 * @return: 재훈련에 성공했으면 0. 재훈련이 필요 없었거나(-ENOTTY) 시도했다가
 *          실패하면 음수 오류값. 호출자는 0 일 때만 "고쳤다"고 본다.
 *
 * [어떤 하드웨어가] 위 영어 주석이 밝힌 관측 사례는 ASMedia ASM2824 (Gen3
 * 스위치)의 다운스트림 포트에 Pericom PI7C9X2G304 (Gen2 스위치)의 업스트림
 * 포트가 물린 구성이다. Delock 라이저 카드(P/N 41433)를 SiFive HiFive
 * Unmatched 보드에 꽂았을 때 재현되었다.
 *
 * [스펙의 무엇을 어겼는가] 두 스위치는 5.0GT/s 를 우선 협상하고 안 되면
 * 2.5GT/s 로 물러나야 하는데, 실제로는 두 속도 사이를 무한히 오가며 데이터
 * 링크 계층이 끝내 active 상태가 되지 않는다(링크 트레이닝이 시간의 약 84%
 * 동안 켜져 있는 것으로 관측). 즉 속도 협상 수렴 자체가 안 된다.
 *
 * [그대로 두면] 그 아래에 달린 장치가 아예 보이지 않는다. 링크가 올라오지
 * 않으므로 열거가 되지 않고, 그 밑의 엔드포인트(NVMe SSD 를 포함한 무엇이든)는
 * 시스템에 존재하지 않는 것이 된다.
 *
 * [커널이 어떻게 우회하는가] 목표 링크 속도(Target Link Speed)를 2.5GT/s 로
 * 강제한 뒤 재훈련을 요청하면 두 스위치가 서로 말이 통한다. 그리고 흥미롭게도
 * 그 상태에서 더 높은 목표 속도로 다시 재훈련하면 이번엔 5.0GT/s 협상에
 * 성공한다. 그래서 이 함수는 "2.5GT/s 로 내려 붙인 뒤, 붙었으면 제한을 푼다"
 * 는 두 단계를 밟는다.
 *
 * [문제를 어떻게 감지하는가] 링크 트레이닝 비트를 폴링하며 깜빡임을 세는 대신,
 * ASM2824 가 지원하는 Data Link Layer Link Active(DLLLA) 비트와 Link Bandwidth
 * Management Status(LBMS) 비트의 조합을 본다. 고장 상황에서는 DLLLA 가 꺼져
 * 있고 동시에 LBMS 가 서 있다(하드웨어가 불안정한 링크를 고치려고 속도/폭을
 * 바꿨다는 뜻). 아무것도 꽂히지 않은 포트는 두 비트가 모두 꺼져 있으므로
 * 빈 포트와 고장 난 링크를 구별할 수 있다.
 *
 * [적용 범위를 왜 넓게 잡는가] 문제의 정확한 원인이 밝혀지지 않았고 ASM2824 가
 * 업스트림이 아니라 다운스트림에 있을 때도 원리상 같은 일이 생길 수 있어,
 * Link Active 보고와 Link Control 2 레지스터를 지원하는 모든 다운스트림
 * 포트에 대해 감지를 시도한다. 다만 "제한을 푸는" 두 번째 단계는 벤더:디바이스
 * ID 로 안전하다고 아는 장치(여기서는 ASM2824)에만 적용한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. Configuration Space 접근과 재훈련 대기가
 * 있으므로 잠들 수 있다. 이 함수는 quirk 표에 등록되지 않고 PCI 코어가
 * 이름으로 직접 부르는 예외적인 경우다(EXPORT 되지 않은 커널 내부 API).
 *
 * 호출 체인:
 *   PCI 코어의 링크 복구 경로 -> [pcie_failed_link_retrain]
 *     -> pcie_capability_read_word() / pcie_set_target_speed() / pcie_lbms_seen()
 */
int pcie_failed_link_retrain(struct pci_dev *dev)
{
	/* [한국어] "제한 해제"까지 해 줘도 안전하다고 확인된 장치 목록.
	 * PCI_VDEVICE(ASMEDIA, 0x2824) 는 vendor=ASMedia, device=0x2824 로
	 * 채운 struct pci_device_id 를 만드는 매크로다. static const 이므로
	 * 함수가 재진입되어도 공유해 문제없다. */
	static const struct pci_device_id ids[] = {
		{ PCI_VDEVICE(ASMEDIA, 0x2824) }, /* ASMedia ASM2824 */
		/* [한국어] 표의 끝을 나타내는 모두 0 인 항목. pci_match_id() 가
		 * vendor==0 을 종료 표시로 삼는다. */
		{}
	};
	/* [한국어] lnksta: Link Status 레지스터(PCIe capability + 0x12) 값.
	 * lnkctl2: Link Control 2 레지스터 값 - 하위 4비트가 Target Link Speed. */
	u16 lnksta, lnkctl2;
	/* [한국어] 기본 반환값을 -ENOTTY 로 둔다. PCI 코어의 관례상 "이 장치에는
	 * 이 방법이 해당되지 않는다"는 뜻이며, 아래 어떤 보정도 하지 않고
	 * 빠져나가면 이 값이 그대로 반환된다. */
	int ret = -ENOTTY;

	/* [한국어] 적용 전제 검사. PCIe 장치여야 하고(pci_is_pcie), 아래로 링크를
	 * 내리는 포트여야 하며(pcie_downstream_port - 루트 포트나 스위치 다운스트림
	 * 포트), Link Control 2 레지스터가 있어야 목표 속도를 바꿀 수 있고
	 * (pcie_cap_has_lnkctl2 - PCIe capability 버전이 2 이상), Data Link Layer
	 * Link Active 보고를 지원해야(link_active_reporting) 고장 여부를 판정할 수
	 * 있다. 하나라도 어긋나면 -ENOTTY 로 즉시 반환한다. */
	if (!pci_is_pcie(dev) || !pcie_downstream_port(dev) ||
	    !pcie_cap_has_lnkctl2(dev) || !dev->link_active_reporting)
		return ret;

	/* [한국어] Link Status 를 읽는다. pcie_capability_read_word() 는 PCIe
	 * capability 구조체의 시작 오프셋을 알아서 더해 주는 헬퍼다. */
	pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
	/* [한국어] 고장 판정: 데이터 링크 계층이 active 가 아닌데(DLLLA=0)
	 * 하드웨어가 속도/폭을 바꾼 흔적이 있으면(LBMS=1) 협상이 수렴하지 못하고
	 * 왕복하는 중이다. 빈 슬롯이면 LBMS 도 0 이라 여기 들어오지 않는다. */
	if (!(lnksta & PCI_EXP_LNKSTA_DLLLA) && pcie_lbms_seen(dev, lnksta)) {
		/* [한국어] 실패 시 되돌릴 원래 목표 속도를 담아 둘 변수. */
		u16 oldlnkctl2;

		/* [한국어] 사용자에게 무슨 일이 벌어지는지 알린다. 이 메시지가
		 * dmesg 에 보이면 이 quirk 가 발동한 것이다. */
		pci_info(dev, "broken device, retraining non-functional downstream link at 2.5GT/s\n");

		/* [한국어] 되돌리기용으로 현재 Link Control 2 를 저장한다. */
		pcie_capability_read_word(dev, PCI_EXP_LNKCTL2, &oldlnkctl2);
		/* [한국어] 1단계: 목표 속도를 2.5GT/s(Gen1)로 낮추고 재훈련을
		 * 요청한다. 세 번째 인자 false 는 "실패해도 원래 값으로 되돌리지
		 * 말라"는 뜻으로, 되돌리기는 아래에서 직접 한다. */
		ret = pcie_set_target_speed(dev, PCIE_SPEED_2_5GT, false);
		if (ret) {
			/* [한국어] 2.5GT/s 로도 링크가 올라오지 않았다. 이 포트의
			 * 문제는 이 quirk 로 고칠 수 있는 것이 아니다. */
			pci_info(dev, "retraining failed\n");
			/* [한국어] 잔여 상태를 지우기 위해 원래 목표 속도로 되돌려
			 * 다시 재훈련을 건다. PCIE_LNKCTL2_TLS2SPEED() 는 저장해 둔
			 * LNKCTL2 의 Target Link Speed 필드를 속도 enum 으로 바꾼다.
			 * 세 번째 인자 true 는 실패를 무시하라는 뜻 - 어차피 실패할
			 * 것으로 보고 결과를 보지 않는다. */
			pcie_set_target_speed(dev, PCIE_LNKCTL2_TLS2SPEED(oldlnkctl2),
					      true);
			return ret;
		}

		/* [한국어] 2.5GT/s 로 링크가 붙었으므로 상태를 다시 읽어 아래
		 * "제한 해제" 판정에 쓸 최신 DLLLA 값을 얻는다. */
		pcie_capability_read_word(dev, PCI_EXP_LNKSTA, &lnksta);
	}

	/* [한국어] 현재 목표 속도를 읽는다. 위 블록을 타지 않았더라도, 펌웨어가
	 * 이미 2.5GT/s 로 묶어 둔 포트를 여기서 풀어 줄 수 있다. */
	pcie_capability_read_word(dev, PCI_EXP_LNKCTL2, &lnkctl2);

	/* [한국어] 2단계 조건: (a) 데이터 링크가 실제로 올라와 있고,
	 * (b) 목표 속도가 2.5GT/s 로 묶여 있으며,
	 * (c) 제한을 풀어도 안전하다고 표에 등록된 장치일 때만 해제한다.
	 * PCI_EXP_LNKCTL2_TLS 는 Target Link Speed 필드 마스크(하위 4비트),
	 * PCI_EXP_LNKCTL2_TLS_2_5GT 는 그 필드의 2.5GT/s 인코딩이다. */
	if ((lnksta & PCI_EXP_LNKSTA_DLLLA) &&
	    (lnkctl2 & PCI_EXP_LNKCTL2_TLS) == PCI_EXP_LNKCTL2_TLS_2_5GT &&
	    pci_match_id(ids, dev)) {
		/* [한국어] Link Capabilities 레지스터 값 - 이 포트가 지원하는
		 * 최대 링크 속도(Max Link Speed 필드)가 들어 있다. */
		u32 lnkcap;

		pci_info(dev, "removing 2.5GT/s downstream link speed restriction\n");
		/* [한국어] 하드웨어가 광고하는 최대 속도를 읽는다. */
		pcie_capability_read_dword(dev, PCI_EXP_LNKCAP, &lnkcap);
		/* [한국어] PCIE_LNKCAP_SLS2SPEED() 로 LNKCAP 의 Supported Link
		 * Speeds 인코딩을 속도 enum 으로 바꾼 뒤 그 값으로 다시 훈련한다.
		 * 성공하면 ret 가 0 이 되어 "고쳤다"로 보고된다. */
		ret = pcie_set_target_speed(dev, PCIE_LNKCAP_SLS2SPEED(lnkcap), false);
		if (ret) {
			/* [한국어] 제한 해제에 실패. 이때 pcie_set_target_speed() 가
			 * 내부적으로 원래 속도로 되돌리므로 링크 자체는 살아 있다. */
			pci_info(dev, "retraining failed\n");
			return ret;
		}
	}

	/* [한국어] 여기까지 왔다면 ret 는 0(재훈련 성공) 또는 -ENOTTY(할 일이
	 * 없었음)다. 호출자는 0 일 때만 링크를 고쳤다고 판단한다. */
	return ret;
}
/*
 * [한국어]
 * fixup_debug_start - quirk 하나를 실행하기 직전의 시각을 찍는다
 *
 * @dev: quirk 가 적용될 장치
 * @fn: 곧 호출할 quirk 함수 포인터
 * @return: 지금 시각(ktime_t). 짝이 되는 fixup_debug_report() 에 그대로 넘긴다.
 *
 * quirk 는 링커 섹션을 통해 간접 호출되므로 어떤 함수가 언제 불렸는지
 * 추적하기 어렵다. 부팅 인자 initcall_debug 가 켜져 있으면 호출 직전에
 * 함수 이름을 찍어 두어 어느 quirk 에서 부팅이 멈췄는지 알 수 있게 한다.
 *
 * 실행 컨텍스트: pci_do_fixups() 안, 프로세스 문맥. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [fixup_debug_start] -> pci_info() / ktime_get()
 */
static ktime_t fixup_debug_start(struct pci_dev *dev,
				 void (*fn)(struct pci_dev *dev))
{
	/* [한국어] initcall_debug 는 커널 부팅 파라미터로 켜지는 전역 플래그다.
	 * 평소에는 거짓이라 이 출력이 나가지 않는다. */
	if (initcall_debug)
		/* [한국어] %pS 는 함수 포인터를 심볼 이름으로 풀어 찍는 커널 전용
		 * 포맷이다. task_pid_nr(current) 로 어느 태스크가 부르는지도 남겨,
		 * 부팅 초기화 스레드인지 핫플러그 워커인지 구별할 수 있게 한다. */
		pci_info(dev, "calling  %pS @ %i\n", fn, task_pid_nr(current));

	/* [한국어] 단조 증가 시계를 읽어 반환한다. 벽시계가 아니라 ktime 을
	 * 쓰는 이유는 도중에 시간이 조정돼도 경과 시간이 음수가 되지 않게
	 * 하기 위해서다. */
	return ktime_get();
}

/*
 * [한국어]
 * fixup_debug_report - quirk 하나가 얼마나 오래 걸렸는지 보고한다
 *
 * @dev: quirk 가 적용된 장치
 * @calltime: fixup_debug_start() 가 돌려준 시작 시각
 * @fn: 방금 호출을 마친 quirk 함수 포인터
 * @return: 없음
 *
 * quirk 중에는 msleep() 으로 수백 ms 를 자는 것이 있어 부팅 시간을 눈에
 * 띄게 늘린다. 그런 범인을 찾을 수 있도록, initcall_debug 가 꺼져 있어도
 * 10ms 를 넘으면 무조건 경고성 정보를 남긴다.
 *
 * 실행 컨텍스트: pci_do_fixups() 안, 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [fixup_debug_report] -> ktime_get() / pci_info()
 */
static void fixup_debug_report(struct pci_dev *dev, ktime_t calltime,
			       void (*fn)(struct pci_dev *dev))
{
	/* [한국어] delta: 경과 시간(ns 단위 ktime), rettime: 지금 시각. */
	ktime_t delta, rettime;
	/* [한국어] 마이크로초로 환산한 경과 시간. 64비트로 두어 오래 걸린
	 * quirk 에서도 넘치지 않게 한다. */
	unsigned long long duration;

	/* [한국어] quirk 가 끝난 시각을 읽는다. */
	rettime = ktime_get();
	/* [한국어] 끝 - 시작 = 경과. ktime_sub() 는 오버플로 안전한 뺄셈이다. */
	delta = ktime_sub(rettime, calltime);
	/* [한국어] 나노초를 10비트 오른쪽 시프트해 근사 마이크로초를 얻는다.
	 * 1024 로 나누는 셈이라 정확히 1000 은 아니지만, 나눗셈 명령을 피하려는
	 * 커널의 흔한 관용이다(로그용이므로 오차가 문제되지 않는다). */
	duration = (unsigned long long) ktime_to_ns(delta) >> 10;
	/* [한국어] 디버그 모드이거나, 10000 (근사 10ms) 를 넘긴 느린 quirk 면
	 * 이름과 소요 시간을 남긴다. 부팅 지연 원인 추적용이다. */
	if (initcall_debug || duration > 10000)
		pci_info(dev, "%pS took %lld usecs\n", fn, duration);
}

/*
 * [한국어]
 * pci_do_fixups - 한 단계(pass)의 quirk 배열을 훑어 매칭되는 것을 실행한다
 *
 * @dev: quirk 적용 대상 장치
 * @f: 이 단계 quirk 배열의 시작(링커가 만들어 준 __start_pci_fixups_* )
 * @end: 배열의 끝(__end_pci_fixups_* ). 반열린 구간 [f, end) 를 훑는다.
 * @return: 없음
 *
 * 이 함수가 "quirk 가 호출자 없이 실행되는" 수수께끼의 정답이다. 각
 * DECLARE_PCI_FIXUP_* 매크로는 struct pci_fixup 항목 하나를 특정 링커
 * 섹션에 넣어 두고, 링커가 그것을 연속 배열로 모아 준다. 이 함수는 그
 * 배열을 처음부터 끝까지 순회하며 (class, vendor, device) 세 조건이 모두
 * 맞는 항목만 골라 hook 을 부른다. 즉 디스패치는 이름이 아니라 표 검색이다.
 *
 * 매칭 규칙은 세 항목의 AND 다. 각 항목은 "값이 같거나" 또는 "PCI_ANY_ID
 * (와일드카드)" 면 통과한다. 클래스는 class_shift 만큼 오른쪽으로 민 뒤
 * 비교하므로, shift 를 8 로 주면 상위 16비트(베이스 클래스 + 서브클래스)만,
 * 16 으로 주면 베이스 클래스만 보는 식으로 정밀도를 조절할 수 있다.
 *
 * 실행 컨텍스트: pci_fixup_device() 를 통해서만 불린다. 프로세스 문맥이며
 * quirk 안에서 잠들 수 있다. 이 함수 자체는 락을 잡지 않는다 - 배열은
 * 커널 이미지에 박힌 읽기 전용 데이터라 경쟁이 없다.
 *
 * 호출 체인:
 *   pci_fixup_device() -> [pci_do_fixups]
 *     -> fixup_debug_start() -> hook(dev) -> fixup_debug_report()
 */
static void pci_do_fixups(struct pci_dev *dev, struct pci_fixup *f,
			  struct pci_fixup *end)
{
	/* [한국어] 각 hook 의 시작 시각을 담아 두는 지역 변수. 루프마다 갱신된다. */
	ktime_t calltime;

	/* [한국어] 초기식이 비어 있는 for 문 - f 는 인자로 이미 배열 시작을
	 * 가리키고 있다. 포인터를 하나씩 증가시키며 배열 끝까지 훑는다. */
	for (; f < end; f++)
		/* [한국어] 클래스 매칭. dev->class 는 24비트 클래스 코드
		 * (베이스클래스:서브클래스:프로그래밍인터페이스)를 담는다.
		 * f->class_shift 만큼 밀어 비교 정밀도를 맞춘다. 예를 들어
		 * NVMe 컨트롤러의 클래스 코드는 PCI_CLASS_STORAGE_EXPRESS 이고,
		 * shift 8 이면 그중 상위 16비트(스토리지 : 비휘발성 메모리)까지만
		 * 비교한다. 그 상수의 정의는 include/linux/pci_ids.h 에 있으며
		 * 이 sparse checkout 에는 그 헤더가 없다. */
		if ((f->class == (u32) (dev->class >> f->class_shift) ||
		     /* [한국어] PCI_ANY_ID 는 "클래스를 따지지 않는다"는 와일드카드.
		      * vendor/device 로만 고르는 대부분의 quirk 가 이 값을 쓴다. */
		     f->class == (u32) PCI_ANY_ID) &&
		    /* [한국어] 벤더 ID 매칭. Configuration Space 오프셋 0x00 의
		     * 하위 16비트로, 예컨대 삼성은 0x144d 다. */
		    (f->vendor == dev->vendor ||
		     /* [한국어] 벤더 와일드카드 - 모든 벤더에 적용되는 quirk. */
		     f->vendor == (u16) PCI_ANY_ID) &&
		    /* [한국어] 디바이스 ID 매칭. Configuration Space 오프셋 0x02. */
		    (f->device == dev->device ||
		     /* [한국어] 디바이스 와일드카드 - 한 벤더의 모든 칩에 적용. */
		     f->device == (u16) PCI_ANY_ID)) {
			/* [한국어] 실제로 부를 quirk 함수 포인터. 아래 두 갈래로
			 * 얻는 방식이 다르다. */
			void (*hook)(struct pci_dev *dev);
/* [한국어] PREL32 재배치를 지원하는 아키텍처에서는 struct pci_fixup 이 함수의
 * 절대 주소 대신 32비트 상대 오프셋(hook_offset)만 담는다. 64비트 포인터를
 * 32비트로 줄여 커널 이미지 크기를 아끼고, 부팅 시 재배치 처리도 줄이기
 * 위해서다. */
#ifdef CONFIG_HAVE_ARCH_PREL32_RELOCATIONS
			/* [한국어] offset_to_ptr() 은 "그 필드의 주소 + 저장된
			 * 상대 오프셋" 으로 실제 함수 주소를 복원한다. */
			hook = offset_to_ptr(&f->hook_offset);
/* [한국어] 그 밖의 아키텍처에서는 함수 포인터를 그대로 저장한다. */
#else
			hook = f->hook;
#endif
			/* [한국어] 실행 시간 측정 시작 + initcall_debug 시 호출 기록. */
			calltime = fixup_debug_start(dev, hook);
			/* [한국어] ★ 여기가 quirk 가 실제로 실행되는 지점이다.
			 * 이 파일 아래쪽의 quirk_* 함수들은 모두 이 한 줄을 통해
			 * 불린다 - 그래서 소스에서 호출자를 찾을 수 없다. */
			hook(dev);
			/* [한국어] 소요 시간을 보고한다(10ms 초과 시 항상 기록). */
			fixup_debug_report(dev, calltime, hook);
		}
}

/* [한국어] 아래 16개 심볼은 링커가 만들어 주는 "구역 경계" 표식이다. 이
 * 파일 어디에도 정의가 없고 링커 스크립트가 각 quirk 섹션의 시작/끝 주소로
 * 채워 준다. 배열형(빈 대괄호)으로 선언한 이유는 주소 자체를 값으로 쓰기
 * 위해서다 - 포인터 변수로 선언하면 그 주소에 있는 값을 읽어 버린다. */

/* [한국어] early 단계 배열의 시작. 장치를 막 발견해 헤더를 읽기 시작한
 * 아주 이른 시점에 실행되는 quirk 들이 모인다. */
extern struct pci_fixup __start_pci_fixups_early[];
/* [한국어] early 단계 배열의 끝(반열린 구간의 상한). */
extern struct pci_fixup __end_pci_fixups_early[];
/* [한국어] header 단계 배열의 시작. BAR/클래스/인터럽트 핀 등 표준 헤더를
 * 읽어 struct pci_dev 를 채운 직후 실행된다. 이 파일에서 가장 많이 쓰인다. */
extern struct pci_fixup __start_pci_fixups_header[];
/* [한국어] header 단계 배열의 끝. */
extern struct pci_fixup __end_pci_fixups_header[];
/* [한국어] final 단계 배열의 시작. 열거가 모두 끝난 뒤 fs_initcall 에서
 * 일괄 적용된다. */
extern struct pci_fixup __start_pci_fixups_final[];
/* [한국어] final 단계 배열의 끝. */
extern struct pci_fixup __end_pci_fixups_final[];
/* [한국어] enable 단계 배열의 시작. pci_enable_device() 경로에서 장치를
 * 실제로 켤 때 실행된다. NVMe 드라이버의 pci_enable_device_mem() 도
 * 이 경로를 지난다. */
extern struct pci_fixup __start_pci_fixups_enable[];
/* [한국어] enable 단계 배열의 끝. */
extern struct pci_fixup __end_pci_fixups_enable[];
/* [한국어] resume 단계 배열의 시작. 시스템 재개 시 실행된다. */
extern struct pci_fixup __start_pci_fixups_resume[];
/* [한국어] resume 단계 배열의 끝. */
extern struct pci_fixup __end_pci_fixups_resume[];
/* [한국어] resume_early 단계 배열의 시작. 재개 경로 중에서도 이른 시점으로,
 * 장치 상태 복원 근처에서 레지스터를 다시 고쳐야 하는 quirk 가 온다. */
extern struct pci_fixup __start_pci_fixups_resume_early[];
/* [한국어] resume_early 단계 배열의 끝. */
extern struct pci_fixup __end_pci_fixups_resume_early[];
/* [한국어] suspend 단계 배열의 시작. 절전 진입 시 실행된다. */
extern struct pci_fixup __start_pci_fixups_suspend[];
/* [한국어] suspend 단계 배열의 끝. */
extern struct pci_fixup __end_pci_fixups_suspend[];
/* [한국어] suspend_late 단계 배열의 시작. 절전 진입의 늦은 시점. */
extern struct pci_fixup __start_pci_fixups_suspend_late[];
/* [한국어] suspend_late 단계 배열의 끝. */
extern struct pci_fixup __end_pci_fixups_suspend_late[];

/* [한국어] final quirk 를 적용해도 되는 시점이 되었는지 나타내는 플래그.
 * 설정자: 아래 pci_apply_final_quirks() 가 fs_initcall 시점에 true 로 올린다.
 * 읽는 자: pci_fixup_device() 의 pci_fixup_final 분기.
 * 값 범위: false(아직 이르다) / true(적용 허용).
 * 왜 필요한가: final quirk 는 "열거가 다 끝난 뒤"를 전제로 쓰였는데,
 * 부팅 초기에 장치가 하나씩 등록될 때마다 pci_fixup_device(final) 이
 * 불릴 수 있다. 그때 실행해 버리면 전제가 깨진다. 그래서 이 플래그가
 * 올라가기 전의 final 요청은 조용히 무시한다.
 * 동기화: 부팅 초기 단일 문맥에서 한 번만 true 로 바뀌고 이후 변하지
 * 않으므로 별도 락이 없다. */
static bool pci_apply_fixup_final_quirks;

/*
 * [한국어]
 * pci_fixup_device - 지정한 단계(pass)의 quirk 를 이 장치에 적용한다
 *
 * @pass: 어느 시점의 quirk 를 돌릴지(enum pci_fixup_pass). 아래 switch 의
 *        case 목록이 곧 이 enum 의 전체 값이다.
 * @dev: quirk 적용 대상 장치
 * @return: 없음
 *
 * PCI 코어가 열거·enable·절전·재개의 각 길목에서 이 함수를 부르는 것이
 * quirk 체계의 유일한 진입점이다. 이 함수는 pass 에 해당하는 링커 섹션의
 * 시작/끝 주소를 골라 pci_do_fixups() 에 넘기는 얇은 디스패처일 뿐이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(부팅 초기화 스레드, 핫플러그 워커, PM 코어의
 * 재개/절전 스레드). quirk 가 잠들 수 있으므로 인터럽트 문맥에서 부르면 안 된다.
 * EXPORT_SYMBOL 로 모듈에도 열려 있다 - 버스 드라이버가 자체 열거를 마친 뒤
 * 직접 부르는 경우가 있기 때문이다.
 *
 * 호출 체인:
 *   PCI 코어(열거/enable/PM 경로) -> [pci_fixup_device] -> pci_do_fixups()
 */
void pci_fixup_device(enum pci_fixup_pass pass, struct pci_dev *dev)
{
	/* [한국어] 이번에 훑을 배열의 시작과 끝. 아래 switch 에서 채워진다. */
	struct pci_fixup *start, *end;

	/* [한국어] 단계별로 서로 다른 링커 섹션을 고른다. 이 switch 가 사실상
	 * "어떤 단계가 존재하는가"의 완전한 목록이다. */
	switch (pass) {
	/* [한국어] EARLY: 장치를 막 찾아 struct pci_dev 를 채우기 시작한 시점.
	 * 이후 열거 판단의 전제가 되는 값(헤더 타입, 클래스 코드 등)을 고쳐야
	 * 하는 quirk 가 여기 온다. */
	case pci_fixup_early:
		start = __start_pci_fixups_early;
		end = __end_pci_fixups_early;
		break;

	/* [한국어] HEADER: 표준 헤더(BAR, 클래스, IRQ 핀)를 읽어 넣은 직후.
	 * 잘못 보고된 BAR 를 고치거나 숨은 리소스를 예약하는 quirk 가 온다. */
	case pci_fixup_header:
		start = __start_pci_fixups_header;
		end = __end_pci_fixups_header;
		break;

	/* [한국어] FINAL: 열거가 전부 끝난 뒤. 아래 pci_apply_final_quirks() 가
	 * 플래그를 올리기 전에는 아무것도 하지 않고 돌아간다. */
	case pci_fixup_final:
		/* [한국어] 아직 "열거 완료" 시점이 아니면 조용히 반환한다.
		 * 이 검사가 없으면 부팅 도중 장치가 등록될 때마다 final quirk 가
		 * 전제 조건 없이 실행되어 버린다. */
		if (!pci_apply_fixup_final_quirks)
			return;
		start = __start_pci_fixups_final;
		end = __end_pci_fixups_final;
		break;

	/* [한국어] ENABLE: pci_enable_device() 로 장치를 켜는 시점. 장치를
	 * 실제로 쓰기 직전에만 필요한 보정이 온다. */
	case pci_fixup_enable:
		start = __start_pci_fixups_enable;
		end = __end_pci_fixups_enable;
		break;

	/* [한국어] RESUME: 시스템 재개 시. 절전 중에 하드웨어가 잃어버린
	 * 설정을 다시 써 주는 quirk 가 온다. */
	case pci_fixup_resume:
		start = __start_pci_fixups_resume;
		end = __end_pci_fixups_resume;
		break;

	/* [한국어] RESUME_EARLY: 재개 경로의 이른 시점. 표준 상태 복원과
	 * 순서를 맞춰야 하는 보정이 온다. */
	case pci_fixup_resume_early:
		start = __start_pci_fixups_resume_early;
		end = __end_pci_fixups_resume_early;
		break;

	/* [한국어] SUSPEND: 절전 진입 시. */
	case pci_fixup_suspend:
		start = __start_pci_fixups_suspend;
		end = __end_pci_fixups_suspend;
		break;

	/* [한국어] SUSPEND_LATE: 절전 진입의 늦은 시점. */
	case pci_fixup_suspend_late:
		start = __start_pci_fixups_suspend_late;
		end = __end_pci_fixups_suspend_late;
		break;

	/* [한국어] 알 수 없는 pass 값. 아래 영어 주석대로, enum 을 다 처리했는데도
	 * 컴파일러가 경고를 내기 때문에 넣어 둔 방어 분기다. start/end 가
	 * 초기화되지 않은 채 아래로 내려가는 것을 막는 역할도 한다. */
	default:
		/* stupid compiler warning, you would think with an enum... */
		return;
	}
	/* [한국어] 고른 구간을 순회하며 매칭되는 quirk 를 모두 실행한다. */
	pci_do_fixups(dev, start, end);
}
/* [한국어] 모듈에서도 부를 수 있도록 심볼을 공개한다. EXPORT_SYMBOL(GPL 이
 * 아닌 쪽)인 것은 오래된 호환성 때문이며, 이 파일에서 이 심볼만 공개된다. */
EXPORT_SYMBOL(pci_fixup_device);

/*
 * [한국어]
 * pci_apply_final_quirks - 열거 완료 후 final quirk 를 일괄 적용하고 CLS 를 정한다
 *
 * @return: 항상 0(initcall 규약상 성공)
 *
 * 두 가지 일을 한다. 첫째, pci_apply_fixup_final_quirks 플래그를 올려 이후의
 * final quirk 실행을 허용하고, 지금까지 등록된 모든 PCI 장치에 대해 final
 * quirk 를 소급 적용한다. 둘째, 시스템 공통 Cache Line Size(CLS)를 정한다.
 *
 * CLS 가 왜 여기서 정해지는가: PCI Configuration Space 오프셋 0x0C 의
 * Cache Line Size 레지스터는 값을 DWORD(4바이트) 개수로 담는다. 이 값은
 * Memory Write and Invalidate 같은 버스 명령의 동작에 영향을 주므로 시스템
 * 안에서 일관돼야 한다. 아키텍처가 값을 정해 두지 않았다면, 펌웨어가 각
 * 장치에 써 둔 값을 모아 보고 모두 같으면 그 값을 채택하고, 서로 다르면
 * 기본값으로 물러선다.
 *
 * 실행 컨텍스트: fs_initcall_sync 로 등록되어 부팅 중 단일 스레드에서
 * 한 번만 실행된다. for_each_pci_dev() 가 장치 참조수를 잡았다 놓으므로
 * 잠들 수 있는 문맥이어야 한다.
 *
 * 호출 체인:
 *   커널 initcall 순회 -> [pci_apply_final_quirks]
 *     -> pci_fixup_device(pci_fixup_final) -> pci_do_fixups()
 */
static int __init pci_apply_final_quirks(void)
{
	/* [한국어] for_each_pci_dev() 의 반복자. NULL 로 시작해야 목록의
	 * 처음부터 훑는다(pci_get_device() 의 규약). */
	struct pci_dev *dev = NULL;
	/* [한국어] 지금까지 본 장치들이 공통으로 가진 CLS 값(DWORD 단위).
	 * 0 은 "아직 아무것도 못 봤다"는 뜻이다. */
	u8 cls = 0;
	/* [한국어] 현재 장치에서 읽은 CLS 값을 담는 임시 변수. */
	u8 tmp;

	/* [한국어] 아키텍처 코드가 이미 CLS 를 정해 두었다면 그 값을 알리기만
	 * 하고 아래의 추론 과정은 건너뛴다. */
	if (pci_cache_line_size)
		/* [한국어] 레지스터는 DWORD 개수이므로 <<2 (x4) 해야 바이트다. */
		pr_info("PCI: CLS %u bytes\n", pci_cache_line_size << 2);

	/* [한국어] 이 시점부터 final quirk 실행을 허용한다. 아래 루프에서
	 * 곧바로 사용되므로 루프보다 먼저 올려야 한다. */
	pci_apply_fixup_final_quirks = true;
	/* [한국어] 시스템의 모든 PCI 장치를 순회한다. 이 매크로는 내부적으로
	 * pci_get_device() 를 써서 장치 참조수를 잡고, 다음 반복에서 놓는다. */
	for_each_pci_dev(dev) {
		/* [한국어] 이 장치에 등록된 final quirk 를 모두 실행한다.
		 * 부팅 도중 등록되어 final 을 건너뛰었던 장치도 여기서 받는다. */
		pci_fixup_device(pci_fixup_final, dev);
		/*
		 * If arch hasn't set it explicitly yet, use the CLS
		 * value shared by all PCI devices.  If there's a
		 * mismatch, fall back to the default value.
		 */
		/* [한국어] 위 영어 주석대로: 아키텍처가 CLS 를 정하지 않았을
		 * 때만 장치들의 값을 모아 추론한다. */
		if (!pci_cache_line_size) {
			/* [한국어] PCI_CACHE_LINE_SIZE 는 Configuration Space
			 * 오프셋 0x0C. 표준 헤더의 한 바이트 필드다. */
			pci_read_config_byte(dev, PCI_CACHE_LINE_SIZE, &tmp);
			/* [한국어] 아직 기준값이 없으면 이번 장치 값을 기준으로 잡는다. */
			if (!cls)
				cls = tmp;
			/* [한국어] 이 장치가 0(설정 안 됨)이거나 기준값과 같으면
			 * 아직 불일치가 없다 - 다음 장치로 넘어간다. */
			if (!tmp || cls == tmp)
				continue;

			/* [한국어] 여기 왔다면 서로 다른 CLS 를 쓰는 장치가 있다는
			 * 뜻이다. 공통값을 정할 수 없으므로 아키텍처 기본값으로
			 * 물러선다는 사실을 알린다. 모두 <<2 로 바이트 환산. */
			pci_info(dev, "CLS mismatch (%u != %u), using %u bytes\n",
			         cls << 2, tmp << 2,
				 pci_dfl_cache_line_size << 2);
			/* [한국어] 기본값을 채택한다. 이 대입으로 pci_cache_line_size
			 * 가 0 이 아니게 되므로, 이후 반복에서는 이 블록에 들어오지
			 * 않아 추론이 자연스럽게 멈춘다. */
			pci_cache_line_size = pci_dfl_cache_line_size;
		}
	}

	/* [한국어] 루프를 다 돌았는데도 값이 정해지지 않았다면(=불일치가 없어
	 * 위 블록에서 기본값을 채택할 일이 없었던 경우) 여기서 확정한다. */
	if (!pci_cache_line_size) {
		pr_info("PCI: CLS %u bytes, default %u\n", cls << 2,
			pci_dfl_cache_line_size << 2);
		/* [한국어] 장치들이 공통으로 가진 값 cls 를 쓰되, 아무 장치도
		 * 값을 갖고 있지 않았다면(cls==0) 아키텍처 기본값을 쓴다. */
		pci_cache_line_size = cls ? cls : pci_dfl_cache_line_size;
	}

	/* [한국어] initcall 은 0 을 반환해야 성공으로 간주된다. */
	return 0;
}
/* [한국어] fs_initcall_sync 로 등록 - 파일시스템 초기화 단계의 끝. PCI 열거는
 * 그보다 이른 subsys_initcall 에서 끝나므로, 이 시점이면 모든 장치가 이미
 * 등록되어 있다는 전제가 성립한다. */
fs_initcall_sync(pci_apply_final_quirks);

/*
 * [한국어]
 * quirk_mmio_always_on - 호스트 브리지의 MMIO 디코딩을 절대 끄지 못하게 표시
 *
 * @dev: 클래스 코드가 Host Bridge 인 장치
 * @return: 없음
 *
 * [배경] BAR 크기를 알아내려면 BAR 에 0xffffffff 를 쓰고 되읽어 하위 비트가
 * 0 으로 고정되는 범위를 봐야 한다. 그 순간 BAR 값이 쓰레기가 되므로, 커널은
 * 크기 측정 동안 Command 레지스터의 Memory Space Enable 비트를 꺼서 장치가
 * 그 주소를 디코딩하지 않게 한다.
 *
 * [무엇이 문제인가] 호스트 브리지처럼 시스템의 근간이 되는 장치는 디코딩을
 * 잠깐이라도 끄면 시스템이 멈추거나 오동작한다. 스펙 위반이라기보다는
 * '끄면 안 되는 장치'라는 현실이다.
 *
 * [우회] dev->mmio_always_on 을 1 로 세워 두면 PCI 코어가 이 장치에 대해서는
 * 크기 측정 시에도 디코딩을 끄지 않는다.
 *
 * 실행 컨텍스트: EARLY 단계. BAR 를 읽기 전에 플래그가 서 있어야 하므로
 * 반드시 header 보다 이른 early 여야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_mmio_always_on]
 */
/*
 * Decoding should be disabled for a PCI device during BAR sizing to avoid
 * conflict. But doing so may cause problems on host bridge and perhaps other
 * key system devices. For devices that need to have mmio decoding always-on,
 * we need to set the dev->mmio_always_on bit.
 */
static void quirk_mmio_always_on(struct pci_dev *dev)
{
	dev->mmio_always_on = 1;	/* [한국어] PCI 코어가 참조하는 플래그 - BAR 크기 측정 중에도 Memory Space Enable 을 끄지 말라는 표시. */
}
/* [한국어] 클래스 코드로만 매칭하는 EARLY 등록. vendor/device 는 PCI_ANY_ID
 * (아무거나), 클래스는 PCI_CLASS_BRIDGE_HOST, class_shift 는 8 이다.
 * shift 8 은 24비트 클래스 코드에서 하위 8비트(프로그래밍 인터페이스)를
 * 버리고 상위 16비트(베이스클래스:서브클래스)만 비교하겠다는 뜻으로,
 * 결과적으로 '모든 호스트 브리지'가 대상이 된다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_ANY_ID, PCI_ANY_ID,
				PCI_CLASS_BRIDGE_HOST, 8, quirk_mmio_always_on);

/* [한국어] [어떤 하드웨어] Mellanox Tavor(InfiniBand HCA)와 그 브리지.
 * [무엇이 문제] 아래 영어 주석대로 실제로는 오류가 없는데도 패리티 오류를
 * 보고한다(false positive). PCI 의 패리티 검사는 데이터 무결성 확인 수단인데,
 * 이 칩은 멀쩡한 전송에도 오류 비트를 세운다.
 * [그대로 두면] 커널이 있지도 않은 버스 오류를 계속 보고하거나 오류 처리
 * 경로를 타게 된다.
 * [우회] pci_disable_parity() 를 final 단계에 걸어 Command 레지스터의
 * Parity Error Response 비트를 꺼 버린다. pci_disable_parity() 는 이 파일이
 * 아니라 PCI 코어에 있는 공용 헬퍼다. */
/*
 * The Mellanox Tavor device gives false positive parity errors.  Disable
 * parity error reporting.
 */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_MELLANOX, PCI_DEVICE_ID_MELLANOX_TAVOR, pci_disable_parity);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_MELLANOX, PCI_DEVICE_ID_MELLANOX_TAVOR_BRIDGE, pci_disable_parity);

/*
 * [한국어]
 * quirk_passive_release - PIIX3 ISA 브리지의 Passive Release 를 강제로 켠다
 *
 * @dev: 매칭된 Intel 82441 호스트 브리지(하지만 실제로 고치는 대상은 아래에서
 *       따로 찾아내는 PIIX3 ISA 브리지다)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 82441FX(호스트 브리지) + 82371SB PIIX3(ISA 브리지)
 * 조합.
 * [무엇이 문제] 아래 영어 주석대로 일부 BIOS 가 Passive Release 를 켜지 않고
 * 넘어간다. Passive Release 는 ISA 마스터가 버스를 오래 잡고 있을 때 PCI
 * 쪽 전송이 끼어들 수 있게 해 주는 기능이다.
 * [그대로 두면] Pentium Pro 의 MTRR 설정과 겹칠 때 문제가 생긴다(영어 주석).
 * [우회] PIIX3 의 Configuration Space 오프셋 0x82 (Deterministic Latency
 * Control, DLC) 레지스터의 비트 1 을 켠다.
 *
 * [구조상 특이점] quirk 는 82441 에 걸려 있는데 정작 고치는 대상은 PIIX3 다.
 * 그래서 함수 안에서 pci_get_device() 로 PIIX3 를 직접 찾아 나선다. 이렇게
 * '매칭된 장치가 아닌 다른 장치를 고치는' 패턴이 이 파일에 자주 나온다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME 두 단계에 등록되어 있다. RESUME 에도 있는
 * 이유는 절전에서 깨어나면 이 비트가 다시 꺼져 있기 때문이다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_passive_release]
 *     -> pci_get_device() -> pci_read_config_byte() -> pci_write_config_byte()
 */
/*
 * Deal with broken BIOSes that neglect to enable passive release,
 * which can cause problems in combination with the 82441FX/PPro MTRRs
 */
static void quirk_passive_release(struct pci_dev *dev)
{
	struct pci_dev *d = NULL;	/* [한국어] 찾아낸 PIIX3 장치를 담을 포인터. pci_get_device() 규약상 NULL 부터 시작해야 목록의 처음부터 훑는다. */
	unsigned char dlc;	/* [한국어] PIIX3 의 0x82 레지스터(Deterministic Latency Control) 값을 담는 한 바이트. */

	/*
	 * We have to make sure a particular bit is set in the PIIX3
	 * ISA bridge, so we have to go out and find it.
	 */
	while ((d = pci_get_device(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82371SB_0, d))) {	/* [한국어] PIIX3 를 모두 찾아 순회한다. pci_get_device() 는 이전 결과를 넘기면 그다음 것을 돌려주고, 없으면 NULL 을 반환해 루프가 끝난다. 이 함수가 이전 인자의 참조수를 대신 놓아 주므로 별도 pci_dev_put() 이 필요 없다. */
		pci_read_config_byte(d, 0x82, &dlc);	/* [한국어] 오프셋 0x82 - PIIX3 고유 레지스터로 표준 헤더가 아니라 칩 전용 영역이다. */
		if (!(dlc & 1<<1)) {	/* [한국어] 비트 1 이 Passive Release enable. 꺼져 있을 때만 손댄다. */
			pci_info(d, "PIIX3: Enabling Passive Release\n");	/* [한국어] 무엇을 고쳤는지 dmesg 에 남긴다. */
			dlc |= 1<<1;	/* [한국어] 비트 1 만 세운다 - 다른 비트는 BIOS 설정을 그대로 보존해야 한다. */
			pci_write_config_byte(d, 0x82, dlc);	/* [한국어] 고친 값을 다시 써 넣는다. */
		}
	}
}
/* [한국어] 82441 호스트 브리지를 만나면 FINAL 단계에서 한 번 적용한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82441,	quirk_passive_release);
/* [한국어] 시스템 재개 시에도 다시 적용한다 - 절전 중 PIIX3 의 DLC 비트가
 * 초기화되어 돌아오기 때문이다. quirk 를 두 단계에 중복 등록하는 전형적인 예. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82441,	quirk_passive_release);

/* [한국어] 아래 ISA DMA 관련 quirk 는 32비트 x86 에서만 의미가 있다.
 * 해당 칩셋들이 그 시대 하드웨어이고, ISA DMA 자체가 그 플랫폼의 유산이다. */
#ifdef CONFIG_X86_32
/*
 * [한국어]
 * quirk_isa_dma_hangs - ISA DMA 가 멈추는 칩셋을 전역 플래그로 알린다
 *
 * @dev: 문제 있는 사우스브리지/호스트 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] 아래 영어 주석대로 VIA VP2/VP3/MVP3 계열이며, 등록표를
 * 보면 VIA 82C586/82C596, Intel PIIX3, ALi M1533, NEC CBUS 도 포함된다.
 * [무엇이 문제] 이 칩셋들에서 ISA DMA 전송이 멈춘다(hang). 원인은 밝혀지지
 * 않았고 VIA 는 문의에 답하지 않았다는 것이 영어 주석의 내용이다. BIOS
 * 버전과 무관하므로 칩셋 차원의 결함으로 보인다.
 * [그대로 두면] ISA DMA 를 쓰는 장치(사운드 카드, 플로피 등)에서 전송이
 * 멈춘 채 돌아오지 않는다.
 * [우회] 이 파일은 직접 고치지 않고 isa_dma_bridge_buggy 전역 플래그만
 * 올린다. 실제 회피 동작은 이 플래그를 읽는 ISA DMA 사용자 쪽에서 한다
 * (그 코드는 이 sparse checkout 에 없어 확인할 수 없다).
 *
 * 실행 컨텍스트: FINAL 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_isa_dma_hangs] -> pci_info()
 */
/*
 * The VIA VP2/VP3/MVP3 seem to have some 'features'. There may be a
 * workaround but VIA don't answer queries. If you happen to have good
 * contacts at VIA ask them for me please -- Alan
 *
 * This appears to be BIOS not version dependent. So presumably there is a
 * chipset level fix.
 */
static void quirk_isa_dma_hangs(struct pci_dev *dev)
{
	if (!isa_dma_bridge_buggy) {	/* [한국어] 이미 다른 장치가 플래그를 올렸다면 메시지를 두 번 찍지 않는다. */
		isa_dma_bridge_buggy = 1;	/* [한국어] 전역 플래그를 올린다. 부팅 초기 단일 문맥이라 별도 동기화가 없다. */
		pci_info(dev, "Activating ISA DMA hang workarounds\n");	/* [한국어] 회피 동작이 켜졌음을 알린다. */
	}
}
/*
 * It's not totally clear which chipsets are the problematic ones.  We know
 * 82C586 and 82C596 variants are affected.
 */
/* [한국어] 아래 7건 모두 같은 quirk_isa_dma_hangs 를 FINAL 단계에 건다.
 * 하나의 quirk 함수를 여러 vendor:device 에 중복 등록하는 것이 이 파일의
 * 표준 관용이다 - struct pci_fixup 항목만 7개 생기고 함수는 하나다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C586_0,	quirk_isa_dma_hangs);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C596,	quirk_isa_dma_hangs);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82371SB_0,  quirk_isa_dma_hangs);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AL,	PCI_DEVICE_ID_AL_M1533,		quirk_isa_dma_hangs);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_NEC_CBUS_1, PCI_DEVICE_ID_NEC_CBUS_2,
 * PCI_DEVICE_ID_NEC_CBUS_3. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NEC,	PCI_DEVICE_ID_NEC_CBUS_1,	quirk_isa_dma_hangs);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NEC,	PCI_DEVICE_ID_NEC_CBUS_2,	quirk_isa_dma_hangs);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NEC,	PCI_DEVICE_ID_NEC_CBUS_3,	quirk_isa_dma_hangs);
/* [한국어] CONFIG_X86_32 블록의 끝. */
#endif

/* [한국어] 아래 quirk 는 inw()/outw() 로 IO 포트에 직접 접근하므로,
 * IO 포트 공간 자체가 없는 아키텍처에서는 컴파일할 수 없다. */
#ifdef CONFIG_HAS_IOPORT
/*
 * [한국어]
 * quirk_tigerpoint_bm_sts - Intel NM10(Tiger Point) LPC 의 BM_STS 비트를 지운다
 *
 * @dev: Intel Tiger Point LPC 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel NM10 칩셋의 LPC(Low Pin Count) 브리지.
 * [무엇이 문제] 아래 영어 주석대로 PM1a_STS 레지스터의 BM_STS(Bus Master
 * Status) 비트가 서 있는 채로 부팅되는 경우가 있다. BM_STS 는 '버스 마스터
 * 활동이 있었다'는 표시로, ACPI 가 깊은 C-state 진입 가능 여부를 판단할 때
 * 본다.
 * [그대로 두면] 하이퍼스레딩 장비에서 C4 상태로 들어가려 할 때 시스템이
 * 멈춘다.
 * [우회] 부팅 시 이 비트를 한 번 지워 준다. 펌웨어 버그이므로 로그에
 * FW_BUG 접두를 붙여 남긴다.
 *
 * [주소를 어떻게 찾는가] LPC 브리지의 Configuration Space 오프셋 0x40 은
 * ACPI 레지스터 블록의 IO 베이스 주소(PMBASE)를 담는 칩 전용 레지스터다.
 * 하위 비트는 다른 용도라 0xff80 으로 마스크해 정렬된 베이스만 남긴다.
 * PM1a_STS 는 그 블록의 오프셋 0 에 있다.
 *
 * 실행 컨텍스트: HEADER 단계. C-state 를 쓰기 전에 지워져 있어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_tigerpoint_bm_sts] -> inw() / outw()
 */
/*
 * Intel NM10 "Tiger Point" LPC PM1a_STS.BM_STS must be clear
 * for some HT machines to use C4 w/o hanging.
 */
static void quirk_tigerpoint_bm_sts(struct pci_dev *dev)
{
	u32 pmbase;	/* [한국어] PMBASE 레지스터(오프셋 0x40) 원본 값. ACPI IO 블록의 베이스 주소가 들어 있다. */
	u16 pm1a;	/* [한국어] PM1a_STS(ACPI Power Management 1a Status) 레지스터의 16비트 값. */

	pci_read_config_dword(dev, 0x40, &pmbase);	/* [한국어] 칩 전용 레지스터 0x40 을 읽는다 - 표준 PCI 헤더가 아니다. */
	pmbase = pmbase & 0xff80;	/* [한국어] 0xff80 마스크로 하위 7비트(용도가 다른 비트)를 떨어내고 128바이트 정렬된 IO 베이스만 남긴다. */
	pm1a = inw(pmbase);	/* [한국어] 그 IO 베이스에서 16비트를 읽는다. ACPI 규약상 오프셋 0 이 PM1a_STS 다. */

	if (pm1a & 0x10) {	/* [한국어] 비트 4(0x10)가 BM_STS. 서 있으면 펌웨어가 지우지 않고 넘긴 것이다. */
		pci_info(dev, FW_BUG "Tiger Point LPC.BM_STS cleared\n");	/* [한국어] FW_BUG 접두는 '펌웨어 잘못'임을 dmesg 에서 구별하기 위한 커널 관용 표시다. */
		outw(0x10, pmbase);	/* [한국어] ACPI 상태 비트는 1 을 써야 지워지는(W1C) 규약이라 0x10 을 그대로 쓴다. 0 을 쓰면 아무 일도 일어나지 않는다. */
	}
}
/* [한국어] Tiger Point LPC 브리지를 만나면 HEADER 단계에서 적용한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_TGP_LPC, quirk_tigerpoint_bm_sts);
/* [한국어] CONFIG_HAS_IOPORT 블록의 끝. */
#endif

/*
 * [한국어]
 * quirk_nopcipci - PCI 장치끼리의 직접 전송이 사라지거나 멈추는 칩셋 표시
 *
 * @dev: 문제 있는 칩셋(SiS 5597, SiS 496)
 * @return: 없음
 *
 * [어떤 하드웨어] SiS 5597, SiS 496 칩셋.
 * [무엇이 문제] 아래 영어 주석대로 PCI 장치 간 직접 전송(peer-to-peer)이
 * 조용히 사라지거나 버스가 멈춘다. PCI 스펙상 두 장치가 서로에게 직접
 * 쓰기를 보내는 것은 허용되는데 이 칩셋의 중재 로직이 이를 처리하지 못한다.
 * [그대로 두면] peer-to-peer 를 쓰는 드라이버가 데이터를 잃거나 멈춘다.
 * [우회] 전역 pci_pci_problems 비트필드에 PCIPCI_FAIL 을 세워 두면,
 * 이 값을 읽는 드라이버가 peer-to-peer 대신 시스템 메모리를 경유하는
 * 경로를 택한다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nopcipci] -> pci_info()
 */
/* Chipsets where PCI->PCI transfers vanish or hang */
static void quirk_nopcipci(struct pci_dev *dev)
{
	if ((pci_pci_problems & PCIPCI_FAIL) == 0) {	/* [한국어] 이미 세워져 있으면 메시지를 반복하지 않는다. */
		pci_info(dev, "Disabling direct PCI/PCI transfers\n");	/* [한국어] 회피가 켜졌음을 알린다. */
		pci_pci_problems |= PCIPCI_FAIL;	/* [한국어] 전역 비트필드에 PCIPCI_FAIL 비트를 OR 로 추가한다 - 다른 quirk 가 세운 비트를 지우지 않기 위해 대입이 아니라 OR 이다. */
	}
}
/* [한국어] SiS 5597 과 SiS 496 두 칩셋에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_5597,		quirk_nopcipci);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_496,		quirk_nopcipci);

/*
 * [한국어]
 * quirk_nopciamd - AMD 8151 AGP 브리지 리비전 0x13 의 Erratum 24 회피
 *
 * @dev: AMD 8151 AGP 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] AMD 8151 AGP 터널, 리비전 ID 0x13 인 것만 해당한다.
 * [무엇이 문제] 아래 코드의 영어 주석이 밝히는 대로 이 리비전에는
 * Erratum 24 가 있다. 에라타의 구체적 내용은 원본 주석에도 없어 이 트리의
 * 정보만으로는 확인할 수 없다. 코드가 하는 일로 보아 PCI 와 AGP 사이의
 * 직접 전송이 문제가 된다.
 * [그대로 두면] PCI 장치와 AGP 장치 간 직접 전송이 실패한다.
 * [우회] pci_pci_problems 에 PCIAGP_FAIL 을 세워 드라이버가 그 경로를
 * 쓰지 않게 한다.
 *
 * [리비전을 왜 따지는가] 같은 device ID 라도 실리콘 리비전에 따라 결함이
 * 고쳐졌을 수 있다. DECLARE 매크로는 리비전으로 매칭할 수 없으므로,
 * 함수 안에서 Revision ID 를 직접 읽어 걸러 낸다. 이 패턴도 이 파일에
 * 자주 나온다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nopciamd] -> pci_read_config_byte()
 */
static void quirk_nopciamd(struct pci_dev *dev)
{
	u8 rev;	/* [한국어] Revision ID 를 담을 한 바이트. */
	pci_read_config_byte(dev, 0x08, &rev);	/* [한국어] Configuration Space 오프셋 0x08 은 표준 헤더의 Revision ID 필드다(그 위 0x09~0x0b 이 클래스 코드). */
	if (rev == 0x13) {	/* [한국어] 리비전 0x13 에만 결함이 있다. 다른 리비전은 건드리지 않는다. */
		/* Erratum 24 */
		pci_info(dev, "Chipset erratum: Disabling direct PCI/AGP transfers\n");	/* [한국어] 어떤 에라타 때문인지 dmesg 에 남긴다. */
		pci_pci_problems |= PCIAGP_FAIL;	/* [한국어] 전역 비트필드에 PCIAGP_FAIL 을 추가한다. */
	}
}
/* [한국어] AMD 8151 AGP 브리지에 FINAL 단계로 등록. 리비전 판별은 함수 안에서 한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_8151_0,	quirk_nopciamd);

/*
 * [한국어]
 * quirk_triton - Intel Triton 계열 칩셋의 PCI 대 PCI 직접 전송 제한 표시
 *
 * @dev: Intel 82437/82437VX/82439/82439TX 호스트 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel Triton 세대 호스트 브리지(82437, 82437VX, 82439,
 * 82439TX).
 * [무엇이 문제] 아래 영어 주석은 'Triton 은 드라이버가 회피책을 써야 한다'고만
 * 밝힌다. 구체적 결함 내용은 원본 주석에도 없어 이 트리의 정보만으로는
 * 확인할 수 없다.
 * [그대로 두면] PCI 장치 간 직접 전송이 제대로 동작하지 않는다.
 * [우회] pci_pci_problems 에 PCIPCI_TRITON 을 세운다. PCIPCI_FAIL 과 달리
 * '금지'가 아니라 '제한'으로, 드라이버가 조건부로 회피하도록 하는 신호다
 * (메시지도 Disabling 이 아니라 Limiting 이다).
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_triton] -> pci_info()
 */
/* Triton requires workarounds to be used by the drivers */
static void quirk_triton(struct pci_dev *dev)
{
	if ((pci_pci_problems&PCIPCI_TRITON) == 0) {	/* [한국어] 이미 세워져 있으면 메시지를 반복하지 않는다. */
		pci_info(dev, "Limiting direct PCI/PCI transfers\n");	/* [한국어] 제한이 걸렸음을 알린다. */
		pci_pci_problems |= PCIPCI_TRITON;	/* [한국어] PCIPCI_TRITON 비트를 OR 로 추가한다. */
	}
}
/* [한국어] Triton 세대 호스트 브리지 4종에 같은 quirk 를 FINAL 단계로 건다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82437,	quirk_triton);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82437VX,	quirk_triton);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82439,	quirk_triton);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82439TX,	quirk_triton);

/*
 * [한국어]
 * quirk_vialatency - VIA Apollo KT133 계열의 PCI 버스 중재 우선순위를 고친다
 *
 * @dev: 매칭된 VIA 노스브리지(8363_0 / 8371_1 / 8361). 실제로 레지스터를
 *       고치는 대상도 이 노스브리지다.
 * @return: 없음
 *
 * [어떤 하드웨어] VIA Apollo KT133/KT133A 계열 노스브리지 + VIA 82C686B
 * 또는 VT8231 사우스브리지 조합.
 * [무엇이 문제] 아래 영어 주석대로, PCI 부하가 높을 때 CPU 가 연속 3회의
 * 버스 마스터 요청 동안 버스를 잡지 못하면 데이터가 유실된다. UDMA IDE
 * 컨트롤러가 여럿 붙은 시스템에서 실제 데이터 손상이 관측되었다. 이는 PCI
 * 중재(arbitration) 정책의 결함이다.
 * [그대로 두면] 디스크 데이터가 조용히 깨진다 - 가장 나쁜 종류의 고장이다.
 * [우회] 노스브리지의 칩 전용 레지스터 0x76 을 고쳐 '모든 마스터 grant 마다
 * 우선순위를 회전시키도록' 바꾼다. 그러면 CPU 가 연속으로 밀려나지 않는다.
 *
 * [VIA 권고와 다른 점] 영어 주석에 따르면 VIA 는 SB Live! 사운드카드가 있을
 * 때만 이 수정을 적용하라고 했지만, 리눅스와 윈도우 모두에서 그것만으로는
 * 부족했고 SB Live! 없이도 손상이 관측되어 그 조건을 무시한다.
 *
 * [대상 판별 방식] 노스브리지만 보고는 문제 유무를 알 수 없어, 함수 안에서
 * 사우스브리지를 찾아 리비전까지 확인한다. 82C686 은 리비전 0x40~0x42
 * (686B), VT8231 은 0x10~0x12 만 결함이 있다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME 두 단계. 재개 시 레지스터가 초기화되므로
 * 다시 적용해야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_vialatency]
 *     -> pci_get_device() -> pci_read_config_byte() -> pci_write_config_byte()
 *     -> pci_dev_put()
 */
/*
 * VIA Apollo KT133 needs PCI latency patch
 * Made according to a Windows driver-based patch by George E. Breese;
 * see PCI Latency Adjust on http://www.viahardware.com/download/viatweak.shtm
 * Also see http://www.au-ja.org/review-kt133a-1-en.phtml for the info on
 * which Mr Breese based his work.
 *
 * Updated based on further information from the site and also on
 * information provided by VIA
 */
static void quirk_vialatency(struct pci_dev *dev)
{
	struct pci_dev *p;	/* [한국어] 리비전을 확인할 사우스브리지를 담을 포인터. 아래에서 참조수를 잡으므로 반드시 pci_dev_put() 으로 놓아야 한다. */
	u8 busarb;	/* [한국어] 노스브리지 오프셋 0x76(버스 중재 제어) 레지스터 값. */

	/*
	 * Ok, we have a potential problem chipset here. Now see if we have
	 * a buggy southbridge.
	 */
	p = pci_get_device(PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_82C686, NULL);	/* [한국어] 먼저 VIA 82C686 사우스브리지를 찾는다. 세 번째 인자 NULL 은 목록의 처음부터 찾으라는 뜻이며, 찾으면 참조수를 하나 올려서 돌려준다. */
	if (p != NULL) {	/* [한국어] 82C686 이 있으면 리비전으로 686A/686B 를 가른다. */

		/*
		 * 0x40 - 0x4f == 686B, 0x10 - 0x2f == 686A;
		 * thanks Dan Hollis.
		 * Check for buggy part revisions
		 */
		if (p->revision < 0x40 || p->revision > 0x42)	/* [한국어] 결함은 686B(리비전 0x40~0x42)에만 있다. 686A(0x10~0x2f) 나 범위 밖이면 손대지 않고 exit 로 빠져 참조수만 놓는다. */
			goto exit;	/* [한국어] 결함 리비전이 아니면 아무것도 고치지 않고 정리 지점으로 뛴다. */
	} else {
		p = pci_get_device(PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_8231, NULL);	/* [한국어] VT8231 을 찾는다. 여기서도 참조수가 올라간다. */
		/* [한국어] 두 사우스브리지 중 아무것도 없으면 이 보드는 문제가 없다.
		 * p 가 NULL 이지만 pci_dev_put(NULL) 은 안전하므로 그대로 exit 로 간다. */
		if (p == NULL)	/* No problem parts */
			goto exit;	/* [한국어] 문제 있는 사우스브리지가 없으므로 아무것도 하지 않고 정리 지점으로 뛴다. */

		/* Check for buggy part revisions */
		/* [한국어] VT8231 은 리비전 0x10~0x12 만 결함이 있다. */
		if (p->revision < 0x10 || p->revision > 0x12)
			goto exit;	/* [한국어] VT8231 도 결함 리비전이 아니면 손대지 않는다. */
	}

	/*
	 * Ok we have the problem. Now set the PCI master grant to occur
	 * every master grant. The apparent bug is that under high PCI load
	 * (quite common in Linux of course) you can get data loss when the
	 * CPU is held off the bus for 3 bus master requests.  This happens
	 * to include the IDE controllers....
	 *
	 * VIA only apply this fix when an SB Live! is present but under
	 * both Linux and Windows this isn't enough, and we have seen
	 * corruption without SB Live! but with things like 3 UDMA IDE
	 * controllers. So we ignore that bit of the VIA recommendation..
	 */
	pci_read_config_byte(dev, 0x76, &busarb);	/* [한국어] 노스브리지의 칩 전용 레지스터 0x76 을 읽는다. 표준 PCI 헤더가 아니라 VIA 고유 영역이며, 대상은 사우스브리지 p 가 아니라 매칭된 노스브리지 dev 다. */

	/*
	 * Set bit 4 and bit 5 of byte 76 to 0x01
	 * "Master priority rotation on every PCI master grant"
	 */
	busarb &= ~(1<<5);	/* [한국어] 비트 5 를 0 으로 만든다. 위 영어 주석대로 비트 5:4 를 값 0b01 로 만들기 위한 첫 단계다. */
	busarb |= (1<<4);	/* [한국어] 비트 4 를 1 로 만든다. 결과적으로 비트 5:4 == 01 = 'PCI 마스터 grant 마다 우선순위 회전'. */
	pci_write_config_byte(dev, 0x76, busarb);	/* [한국어] 고친 값을 되쓴다. 다른 비트는 그대로 보존된다. */
	pci_info(dev, "Applying VIA southbridge workaround\n");	/* [한국어] 회피가 적용되었음을 dmesg 에 남긴다. */
/* [한국어] 공통 정리 지점. 어느 경로로 왔든 사우스브리지 참조수를 놓아야
 * 하므로 goto 로 한 곳에 모았다. */
exit:	/* [한국어] 정리 전용 레이블 - 참조수 해제를 한 곳에 모으기 위한 것이다. */
	pci_dev_put(p);	/* [한국어] pci_get_device() 가 올린 참조수를 놓는다. p 가 NULL 이어도 안전하다. */
}
/* [한국어] 문제의 노스브리지 3종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8363_0,	quirk_vialatency);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8371_1,	quirk_vialatency);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8361,		quirk_vialatency);
/* Must restore this on a resume from RAM */
/* [한국어] 위 영어 주석대로, 램 절전(S3)에서 깨어나면 0x76 값이 날아가므로
 * RESUME 단계에도 같은 quirk 를 등록해 다시 써 준다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8363_0,	quirk_vialatency);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8371_1,	quirk_vialatency);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8361,		quirk_vialatency);

/*
 * [한국어]
 * quirk_viaetbf - VIA Apollo VP3 의 PCI 대 PCI 직접 전송 제한 표시
 *
 * @dev: VIA 82C597 노스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] VIA Apollo VP3(82C597).
 * [무엇이 문제] 아래 영어 주석은 'BT848/878 (Brooktree 비디오 캡처 칩)을 쓸 때
 * ETBF 가 필요하다'고만 적혀 있다. ETBF 라는 약어의 뜻과 결함의 구체적
 * 내용은 원본 주석에도 없어 이 트리의 정보만으로는 확인할 수 없다.
 * [그대로 두면] 이 칩셋에서 PCI 장치 간 직접 전송이 제대로 동작하지 않는다.
 * [우회] 전역 pci_pci_problems 에 PCIPCI_VIAETBF 비트를 세워, 이 값을 읽는
 * 드라이버가 직접 전송을 피하도록 한다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_viaetbf] -> pci_info()
 */
/* VIA Apollo VP3 needs ETBF on BT848/878 */
static void quirk_viaetbf(struct pci_dev *dev)
{
	if ((pci_pci_problems&PCIPCI_VIAETBF) == 0) {	/* [한국어] 이미 세워져 있으면 중복 메시지를 내지 않는다. */
		pci_info(dev, "Limiting direct PCI/PCI transfers\n");	/* [한국어] 제한이 걸렸음을 알린다. */
		pci_pci_problems |= PCIPCI_VIAETBF;	/* [한국어] PCIPCI_VIAETBF 비트를 OR 로 추가한다. */
	}
}
/* [한국어] VIA 82C597 노스브리지에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C597_0,	quirk_viaetbf);

/*
 * [한국어]
 * quirk_vsfx - VIA 82C576 의 PCI 대 PCI 직접 전송 제한 표시
 *
 * @dev: VIA 82C576 노스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] VIA 82C576.
 * [무엇이 문제] 이 quirk 에는 원본 영어 주석이 없다. 결함의 구체적 내용은
 * 이 트리의 정보만으로는 확인할 수 없다. 코드가 하는 일로 보아 PCI 장치 간
 * 직접 전송에 제약이 있다는 뜻이다.
 * [우회] 전역 pci_pci_problems 에 PCIPCI_VSFX 비트를 세운다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_vsfx] -> pci_info()
 */
static void quirk_vsfx(struct pci_dev *dev)
{
	if ((pci_pci_problems&PCIPCI_VSFX) == 0) {	/* [한국어] 이미 세워져 있으면 중복 메시지를 내지 않는다. */
		pci_info(dev, "Limiting direct PCI/PCI transfers\n");	/* [한국어] 제한이 걸렸음을 알린다. */
		pci_pci_problems |= PCIPCI_VSFX;	/* [한국어] PCIPCI_VSFX 비트를 OR 로 추가한다. */
	}
}
/* [한국어] VIA 82C576 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C576,	quirk_vsfx);

/*
 * [한국어]
 * quirk_alimagik - ALi Magik 칩셋의 AGP DMA 제약 표시
 *
 * @dev: ALi M1647 / M1651 노스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] ALi Magik 칩셋(M1647, M1651).
 * [무엇이 문제] 아래 영어 주석대로, AGP 공간으로 DMA 하는 드라이버가
 * 회피책을 써야 한다. 레이턴시를 0xA 로 맞추고 Triton 회피책도 함께
 * 적용해야 한다는 것이 ALi 가 알려 준 정보다.
 * [그대로 두면] AGP 로 DMA 하는 드라이버에서 전송이 실패한다.
 * [우회] pci_pci_problems 에 PCIPCI_ALIMAGIK 와 PCIPCI_TRITON 을 함께
 * 세운다. 두 비트를 한 번에 세우는 것이 이 quirk 의 특징으로, ALi 의
 * 권고 중 'Triton 회피책도 적용' 부분에 해당한다. 레이턴시 0xA 설정은
 * 이 함수가 하지 않는다 - 그 부분이 어디서 처리되는지는 이 트리의
 * 정보만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_alimagik] -> pci_info()
 */
/*
 * ALi Magik requires workarounds to be used by the drivers that DMA to AGP
 * space. Latency must be set to 0xA and Triton workaround applied too.
 * [Info kindly provided by ALi]
 */
static void quirk_alimagik(struct pci_dev *dev)
{
	if ((pci_pci_problems&PCIPCI_ALIMAGIK) == 0) {	/* [한국어] 이미 세워져 있으면 중복 메시지를 내지 않는다. */
		pci_info(dev, "Limiting direct PCI/PCI transfers\n");	/* [한국어] 제한이 걸렸음을 알린다. */
		pci_pci_problems |= PCIPCI_ALIMAGIK|PCIPCI_TRITON;	/* [한국어] 두 비트를 한 번에 세운다 - ALi Magik 고유 제약과 Triton 계열 회피책을 동시에 요구하기 때문이다. */
	}
}
/* [한국어] ALi Magik 노스브리지 2종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AL,	PCI_DEVICE_ID_AL_M1647,		quirk_alimagik);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AL,	PCI_DEVICE_ID_AL_M1651,		quirk_alimagik);

/*
 * [한국어]
 * quirk_natoma - Intel Natono(440FX/LX/BX) 계열의 PCI 대 PCI 전송 제한 표시
 *
 * @dev: Intel 82441 / 82443LX / 82443BX 계열 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel Natoma 세대 칩셋(82441FX, 82443LX, 82443BX 의
 * 여러 함수 번호).
 * [무엇이 문제] 아래 영어 주석은 'Zoran 계열 칩과 함께 쓸 때 흥미로운 경계
 * 조건이 있다'고만 밝힌다. 정확한 결함 내용은 원본 주석에도 없어 이 트리의
 * 정보만으로는 확인할 수 없다.
 * [그대로 두면] Zoran 비디오 칩 등과의 PCI 직접 전송에서 문제가 생긴다.
 * [우회] pci_pci_problems 에 PCIPCI_NATOMA 비트를 세운다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_natoma] -> pci_info()
 */
/* Natoma has some interesting boundary conditions with Zoran stuff at least */
static void quirk_natoma(struct pci_dev *dev)
{
	if ((pci_pci_problems&PCIPCI_NATOMA) == 0) {	/* [한국어] 이미 세워져 있으면 중복 메시지를 내지 않는다. */
		pci_info(dev, "Limiting direct PCI/PCI transfers\n");	/* [한국어] 제한이 걸렸음을 알린다. */
		pci_pci_problems |= PCIPCI_NATOMA;	/* [한국어] PCIPCI_NATOMA 비트를 OR 로 추가한다. */
	}
}
/* [한국어] Natoma 세대 브리지들에 FINAL 단계로 등록한다. 같은 칩의 서로
 * 다른 PCI 함수(_0, _1)마다 별도 device ID 가 있어 각각 걸어 준다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82441,	quirk_natoma);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82443LX_0,	quirk_natoma);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82443LX_1,	quirk_natoma);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82443BX_0,	quirk_natoma);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82443BX_1,	quirk_natoma);
/* [한국어] Natoma 계열의 마지막 등록 항목(82443BX_2). */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82443BX_2,	quirk_natoma);

/*
 * [한국어]
 * quirk_citrine - IBM Citrine 의 Configuration Space 접근 범위를 0xA0 미만으로 제한
 *
 * @dev: IBM Citrine 칩
 * @return: 없음
 *
 * [어떤 하드웨어] IBM Citrine.
 * [무엇이 문제] 아래 영어 주석대로, DMA 가 진행 중일 때 Configuration Space
 * 오프셋 0xA0 을 읽으면 PCI 패리티 오류가 발생한다. 즉 이 칩은 config 읽기가
 * 데이터 경로에 부작용을 일으키는데, 이는 PCI 스펙이 보장해야 할 성질을
 * 어기는 것이다(config 읽기는 부작용이 없어야 한다).
 * [그대로 두면] lspci 처럼 config space 전체를 훑는 아무 코드나 DMA 중에
 * 돌면 버스 패리티 오류가 터진다.
 * [우회] dev->cfg_size 를 0xA0 으로 줄인다. PCI 코어의 config 접근 함수는
 * 이 값을 넘는 오프셋 접근을 거부하므로, 문제의 레지스터에 아무도 닿지
 * 못하게 된다. 표준 헤더(0x00~0x3f)와 그 위 일부는 그대로 쓸 수 있다.
 *
 * 실행 컨텍스트: HEADER 단계. 다른 코드가 config space 를 훑기 전에
 * 제한이 걸려 있어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_citrine]
 */
/*
 * This chip can cause PCI parity errors if config register 0xA0 is read
 * while DMAs are occurring.
 */
static void quirk_citrine(struct pci_dev *dev)
{
	dev->cfg_size = 0xA0;	/* [한국어] cfg_size 는 이 장치에서 접근이 허용되는 Configuration Space 의 길이다. 기본값은 256(PCI) 또는 4096(PCIe)이며, 여기서 0xA0(160)으로 줄여 0xA0 이상을 차단한다. */
}
/* [한국어] IBM Citrine 을 만나면 HEADER 단계에서 접근 범위를 줄인다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_IBM,	PCI_DEVICE_ID_IBM_CITRINE,	quirk_citrine);

/*
 * [한국어]
 * quirk_nfp6000 - Netronome NFP 계열의 Configuration Space 접근을 0x600 미만으로 제한
 *
 * @dev: Netronome NFP4000/NFP5000/NFP6000 및 그 가상 함수(VF)
 * @return: 없음
 *
 * [어떤 하드웨어] Netronome NFP4000 / NFP5000 / NFP6000 스마트 NIC 과 VF.
 * [무엇이 문제] 아래 영어 주석대로, Configuration Space 오프셋 0x600 이상을
 * 읽거나 쓰면 버스가 잠긴다(lockup). PCIe 확장 config space 는 0xfff 까지
 * 있어야 하는데 이 칩은 그 범위 접근을 견디지 못한다.
 * [그대로 두면] 확장 capability 목록을 끝까지 훑는 코드가 시스템을 멈춘다.
 * [우회] dev->cfg_size 를 0x600 으로 줄여 그 위를 아예 접근하지 못하게 한다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nfp6000]
 */
/*
 * This chip can cause bus lockups if config addresses above 0x600
 * are read or written.
 */
static void quirk_nfp6000(struct pci_dev *dev)
{
	dev->cfg_size = 0x600;	/* [한국어] 접근 허용 범위를 0x600(1536)바이트로 자른다. PCIe 확장 config 영역의 앞부분만 남는다. */
}
/* [한국어] NFP 계열 4종(물리 함수 3종 + VF)에 HEADER 단계로 등록한다.
 * VF 까지 거는 이유는 SR-IOV 가상 함수도 같은 실리콘이기 때문이다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NETRONOME,	PCI_DEVICE_ID_NETRONOME_NFP4000,	quirk_nfp6000);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NETRONOME,	PCI_DEVICE_ID_NETRONOME_NFP6000,	quirk_nfp6000);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NETRONOME,	PCI_DEVICE_ID_NETRONOME_NFP5000,	quirk_nfp6000);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NETRONOME,	PCI_DEVICE_ID_NETRONOME_NFP6000_VF,	quirk_nfp6000);

/*
 * [한국어]
 * quirk_extend_bar_to_page - 페이지보다 작은 MEM BAR 를 페이지 크기로 늘린다
 *
 * @dev: IBM Crocodile ipr SAS 어댑터(device ID 0x034a)
 * @return: 없음
 *
 * [어떤 하드웨어] IBM Crocodile ipr SAS 어댑터.
 * [무엇이 문제] 아래 영어 주석은 'BAR 를 시스템 페이지 크기로 늘린다'는
 * 사실만 밝히고 그 동기는 적지 않았다. 왜 늘려야 하는지는 이 트리의
 * 정보만으로는 확인할 수 없다.
 * [커널이 하는 일] 페이지보다 작은 메모리 BAR 를 발견하면 크기를 PAGE_SIZE
 * 로 키우고 IORESOURCE_UNSET 을 세워 '아직 주소가 정해지지 않았다'고 표시한다.
 * 그러면 이후 리소스 할당 단계에서 커널이 새 주소를 배정한다.
 *
 * 실행 컨텍스트: HEADER 단계. BAR 를 읽어 dev->resource[] 에 채운 직후이자
 * 리소스 할당 전이어야 이 수정이 반영된다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_extend_bar_to_page]
 *     -> resource_set_range() / pci_resource_name()
 */
/*  On IBM Crocodile ipr SAS adapters, expand BAR to system page size */
static void quirk_extend_bar_to_page(struct pci_dev *dev)
{
	int i;	/* [한국어] BAR 번호(0~5). PCI_STD_NUM_BARS 는 표준 헤더의 BAR 개수 6 이다. */

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {	/* [한국어] 표준 BAR 6개를 모두 훑는다. 브리지가 아닌 일반 장치(헤더 타입 0)의 BAR 개수다. */
		struct resource *r = &dev->resource[i];	/* [한국어] dev->resource[i] 는 i 번 BAR 를 CPU 주소 공간의 리소스로 표현한 것이다. 열거 시 BAR 크기 측정 결과가 여기 들어 있다. */
		const char *r_name = pci_resource_name(dev, i);	/* [한국어] 로그에 찍을 리소스 이름(예: BAR 0)을 얻는다. */

		if (r->flags & IORESOURCE_MEM && resource_size(r) < PAGE_SIZE) {	/* [한국어] 메모리 BAR(IORESOURCE_MEM)이면서 크기가 한 페이지보다 작은 것만 대상이다. IO BAR 나 이미 충분히 큰 BAR 는 건드리지 않는다. */
			resource_set_range(r, 0, PAGE_SIZE);	/* [한국어] 시작 주소를 0 으로, 크기를 PAGE_SIZE 로 다시 잡는다. 시작이 0 인 것은 '아직 미정'이라는 뜻이며 짝이 되는 UNSET 플래그와 함께 쓰인다. */
			r->flags |= IORESOURCE_UNSET;	/* [한국어] IORESOURCE_UNSET 은 '이 리소스의 주소가 아직 배정되지 않았다'는 표시다. 이 비트가 있어야 이후 할당 단계가 새 주소를 잡아 준다. */
			pci_info(dev, "%s %pR: expanded to page size\n",	/* [한국어] 무엇을 어떻게 바꿨는지 남긴다. %pR 은 struct resource 를 범위 문자열로 찍는 커널 전용 포맷이다. */
				 r_name, r);
		}
	}
}
/* [한국어] IBM device ID 0x034a 에 HEADER 단계로 등록한다. 이 ID 는
 * 이름 있는 상수가 없어 숫자를 그대로 적었다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_IBM, 0x034a, quirk_extend_bar_to_page);

/*
 * [한국어]
 * quirk_s3_64M - S3 868/968 이 잘못 보고한 BAR 크기(32M)를 실제값(64M)으로 고친다
 *
 * @dev: S3 868 또는 968 그래픽 칩
 * @return: 없음
 *
 * [어떤 하드웨어] S3 868, S3 968 그래픽 칩.
 * [무엇이 문제] 아래 영어 주석대로, 이 칩은 BAR 크기 측정에 32M 라고
 * 응답하지만 실제로는 64M 를 디코딩한다. BAR 크기 측정(모두 1 을 쓰고
 * 되읽어 0 으로 고정되는 하위 비트 수를 세는 방식)의 결과가 하드웨어의
 * 실제 디코딩 범위와 다르다 - 명백한 스펙 위반이다.
 * [그대로 두면] 커널은 32M 만 예약해 두고 나머지 32M 를 다른 장치에
 * 배정할 수 있다. 그러면 두 장치가 같은 주소를 디코딩해 충돌한다.
 * [우회] 시작 주소가 64M 정렬이 아니거나 크기가 64M 가 아니면, 리소스를
 * 64M 로 다시 잡고 UNSET 을 세워 재배치를 요청한다. 64M 정렬을 요구하는
 * 이유는 64M 를 디코딩하는 장치가 그 경계 안에 들어와야 겹침이 없기
 * 때문이다.
 *
 * 실행 컨텍스트: HEADER 단계(리소스 할당 전).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_s3_64M] -> resource_set_range()
 */
/*
 * S3 868 and 968 chips report region size equal to 32M, but they decode 64M.
 * If it's needed, re-allocate the region.
 */
static void quirk_s3_64M(struct pci_dev *dev)
{
	struct resource *r = &dev->resource[0];	/* [한국어] 이 칩의 프레임버퍼는 BAR0 에 있다. dev->resource[0] 이 그 리소스다. */

	if (!IS_ALIGNED(r->start, SZ_64M) || resource_size(r) != SZ_64M) {	/* [한국어] IS_ALIGNED(r->start, SZ_64M) 로 64M 경계 정렬을 확인하고, 크기가 정확히 64M 인지도 본다. 둘 중 하나라도 어긋나면 고쳐야 한다. */
		r->flags |= IORESOURCE_UNSET;	/* [한국어] 먼저 UNSET 을 세워 '주소 미정'으로 만든다 - 이후 할당 단계가 64M 정렬을 만족하는 자리를 새로 찾는다. */
		resource_set_range(r, 0, SZ_64M);	/* [한국어] 크기를 64M 로 바로잡는다. 시작 0 은 미정이라는 뜻이다. */
	}
}
/* [한국어] S3 868 과 968 두 칩에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_S3,	PCI_DEVICE_ID_S3_868,		quirk_s3_64M);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_S3,	PCI_DEVICE_ID_S3_968,		quirk_s3_64M);

/*
 * [한국어]
 * quirk_io - 펌웨어가 잘못 보고한 IO BAR 를 지정한 크기로 강제로 다시 세운다
 *
 * @dev: 대상 장치
 * @pos: 고칠 BAR 번호(0~5)
 * @size: 이 BAR 가 실제로 차지하는 바이트 수(하드웨어의 진짜 크기)
 * @name: 로그에 남길 장치 설명 문자열
 * @return: 없음
 *
 * 아래 quirk_cs5536_vsa() 만을 위한 보조 함수다. BAR 크기 측정 결과를
 * 믿을 수 없는 장치에 대해, '주소는 펌웨어가 써 둔 것을 쓰되 크기는 우리가
 * 아는 값으로 바꾼다'를 수행한다.
 *
 * 단계: (1) BAR 원본 값을 config space 에서 직접 읽는다. (2) 값이 0 이면
 * 펌웨어가 배정하지 않은 것이므로 손대지 않는다. (3) 리소스의 플래그를
 * IO 로 세우고 고정(FIXED)으로 표시한다. (4) 주소를 size 경계로 내림
 * 정렬한다. (5) PCI 버스 주소를 CPU 리소스 주소로 변환해 채운다.
 *
 * 실행 컨텍스트: quirk_cs5536_vsa() 안, HEADER 단계.
 *
 * 호출 체인:
 *   quirk_cs5536_vsa() -> [quirk_io]
 *     -> pci_read_config_dword() -> pcibios_bus_to_resource()
 */
static void quirk_io(struct pci_dev *dev, int pos, unsigned int size,
		     const char *name)
{
	u32 region;	/* [한국어] BAR 레지스터의 원본 32비트 값. */
	struct pci_bus_region bus_region;	/* [한국어] PCI 버스 주소 공간에서의 구간. CPU 주소와 다를 수 있어 변환이 필요하다. */
	struct resource *res = pci_resource_n(dev, pos);	/* [한국어] pos 번 BAR 에 대응하는 struct resource 포인터. */
	const char *res_name = pci_resource_name(dev, pos);	/* [한국어] 로그용 리소스 이름. */

	pci_read_config_dword(dev, PCI_BASE_ADDRESS_0 + (pos << 2), &region);	/* [한국어] BAR 를 직접 읽는다. PCI_BASE_ADDRESS_0 은 오프셋 0x10 이고 BAR 하나가 4바이트이므로 (pos << 2) 를 더해 pos 번 BAR 의 오프셋을 만든다. */

	if (!region)	/* [한국어] 0 이면 펌웨어가 이 BAR 에 주소를 배정하지 않은 것이다. 고칠 대상이 없으므로 그대로 둔다. */
		return;

	res->name = pci_name(dev);	/* [한국어] 리소스 이름을 장치 이름(예: 0000:00:0f.0)으로 세운다. /proc/iomem 등에 이 이름이 보인다. */
	res->flags = region & ~PCI_BASE_ADDRESS_IO_MASK;	/* [한국어] PCI_BASE_ADDRESS_IO_MASK 는 IO BAR 의 주소 비트 마스크(하위 2비트를 뺀 나머지)다. 그 여집합과 AND 하면 주소가 아닌 하위 타입 비트만 남고, 그것을 flags 의 출발점으로 삼는다. */
	res->flags |=	/* [한국어] 그 위에 세 플래그를 얹는다. */
		(IORESOURCE_IO | IORESOURCE_PCI_FIXED | IORESOURCE_SIZEALIGN);	/* [한국어] IORESOURCE_IO 는 IO 포트 공간, IORESOURCE_PCI_FIXED 는 '이 주소를 옮기지 말라'(펌웨어가 정한 자리에 고정), IORESOURCE_SIZEALIGN 은 정렬 요구가 크기와 같다는 뜻이다. */
	region &= ~(size - 1);	/* [한국어] 주소를 size 경계로 내림 정렬한다. size 가 2의 거듭제곱이라 (size-1) 이 하위 비트 마스크가 된다 - BAR 의 하위 타입 비트도 이때 함께 떨어진다. */

	/* Convert from PCI bus to resource space */
	bus_region.start = region;	/* [한국어] PCI 버스 주소 기준 시작. */
	bus_region.end = region + size - 1;	/* [한국어] 끝 주소는 시작 + 크기 - 1 (구간을 닫힌 구간으로 표현하는 커널 관례). */
	pcibios_bus_to_resource(dev->bus, res, &bus_region);	/* [한국어] PCI 버스 주소를 CPU 물리 주소로 변환해 res 에 채운다. 호스트 브리지가 주소 변환을 하는 플랫폼에서는 두 값이 다르다. */

	pci_info(dev, FW_BUG "%s %pR: %s quirk\n", res_name, res, name);	/* [한국어] FW_BUG 접두로 '펌웨어 잘못'임을 표시하며 고친 결과를 남긴다. */
}

/*
 * [한국어]
 * quirk_cs5536_vsa - AMD CS5536 ISA 브리지의 잘못된 BAR 헤더를 바로잡는다
 *
 * @dev: AMD CS5536 ISA 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] AMD(구 National/Geode) CS5536 의 ISA 브리지 함수. 아래
 * 영어 주석은 Soekris NET5501 보드의 comBIOS 1.33 을 구체적 사례로 든다.
 * [무엇이 문제] 일부 BIOS 가 ISA PCI 영역의 헤더 정보를 잘못 써 둔다.
 * BAR0 은 8바이트여야 하는데 8k 같은 엉뚱한 크기로 설정되어, BAR1 의 메모리
 * 범위와 겹쳐 버린다. 게다가 CS553x 의 ISA PCI BAR 는 읽기 전용일 수도 있어
 * (영어 주석의 버그질라 링크 참조) 커널이 다시 써서 고칠 수도 없다.
 * [그대로 두면] 겹친 리소스 때문에 할당이 실패하거나 엉뚱한 장치가 같은
 * IO 범위를 잡는다.
 * [우회] BAR 를 다시 쓰는 대신, 커널이 들고 있는 struct resource 쪽을
 * 올바른 크기로 직접 고쳐 놓는다(quirk_io). BAR0=8바이트(SMB), BAR1=256바이트
 * (GPIO), BAR2=64바이트(MFGPT) 가 이 칩의 실제 크기다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_cs5536_vsa] -> quirk_io()
 */
/*
 * Some CS5536 BIOSes (for example, the Soekris NET5501 board w/ comBIOS
 * ver. 1.33  20070103) don't set the correct ISA PCI region header info.
 * BAR0 should be 8 bytes; instead, it may be set to something like 8k
 * (which conflicts w/ BAR1's memory range).
 *
 * CS553x's ISA PCI BARs may also be read-only (ref:
 * https://bugzilla.kernel.org/show_bug.cgi?id=85991 - Comment #4 forward).
 */
static void quirk_cs5536_vsa(struct pci_dev *dev)
{
	static char *name = "CS5536 ISA bridge";	/* [한국어] 세 번 넘길 로그용 이름. static 으로 두어 매번 스택에 만들지 않는다. */

	if (pci_resource_len(dev, 0) != 8) {	/* [한국어] BAR0 의 길이가 8 이 아니면 BIOS 가 잘못 써 둔 경우다. 올바른 보드에서는 아무것도 하지 않는다. */
		/* [한국어] BAR0: SMB(System Management Bus) 컨트롤러, 실제 8바이트. */
		quirk_io(dev, 0,   8, name);	/* SMB */
		/* [한국어] BAR1: GPIO 블록, 실제 256바이트. */
		quirk_io(dev, 1, 256, name);	/* GPIO */
		/* [한국어] BAR2: MFGPT(Multi-Function General Purpose Timer), 실제 64바이트. */
		quirk_io(dev, 2,  64, name);	/* MFGPT */
		pci_info(dev, "%s bug detected (incorrect header); workaround applied\n",	/* [한국어] 회피가 적용되었음을 알린다. */
			 name);
	}
}
/* [한국어] CS5536 ISA 브리지에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_CS5536_ISA, quirk_cs5536_vsa);

/*
 * [한국어]
 * quirk_io_region - BAR 가 아닌 칩 전용 레지스터에 숨어 있는 IO 영역을 리소스로 등록
 *
 * @dev: 대상 장치
 * @port: 그 IO 베이스 주소가 들어 있는 Configuration Space 오프셋(표준 BAR 가 아님)
 * @size: 그 IO 영역의 바이트 크기
 * @nr: 이 영역을 담아 둘 dev->resource[] 슬롯 번호
 * @name: 로그에 남길 설명
 * @return: 없음
 *
 * [왜 필요한가] 일부 칩은 IO 영역을 표준 BAR 로 광고하지 않고 벤더 전용
 * 레지스터에만 적어 둔다. 커널의 리소스 관리자는 BAR 만 보므로 그 영역을
 * 모르고, 그러면 다른 장치에 같은 IO 범위를 배정해 충돌이 난다. 이 함수는
 * 숨은 영역을 찾아 리소스로 등록해(claim) 다른 장치가 못 쓰게 막는다.
 *
 * [슬롯을 빌려 쓰는 이유] 아래 영어 주석대로, 여기 쓰이는 resource 슬롯은
 * 보통 '브리지 창으로 쓰이지 않는 브리지 창 리소스'다. 즉 남는 칸을 임시
 * 보관함으로 빌려 쓰는 것이라, 그 번호를 로그에 찍는 것은 의미가 없다.
 *
 * 실행 컨텍스트: 여러 quirk 함수가 HEADER 단계에서 부른다.
 *
 * 호출 체인:
 *   quirk_ali7101_acpi() / quirk_piix4_acpi() / quirk_ich4_lpc_acpi() /
 *   ich6_lpc_acpi_gpio() / quirk_vt82c586_acpi() / quirk_vt82c686_acpi() /
 *   quirk_vt8235_acpi() -> [quirk_io_region]
 *     -> pci_read_config_word() -> pcibios_bus_to_resource() -> pci_claim_resource()
 */
static void quirk_io_region(struct pci_dev *dev, int port,
			    unsigned int size, int nr, const char *name)
{
	u16 region;	/* [한국어] 칩 전용 레지스터에서 읽은 IO 베이스 주소(16비트). */
	struct pci_bus_region bus_region;	/* [한국어] PCI 버스 주소 공간 기준 구간. */
	struct resource *res = pci_resource_n(dev, nr);	/* [한국어] 이 영역을 보관할 리소스 슬롯 포인터. */

	pci_read_config_word(dev, port, &region);	/* [한국어] 표준 BAR 가 아니라 인자로 받은 임의 오프셋에서 IO 베이스를 읽는다. */
	region &= ~(size - 1);	/* [한국어] size 경계로 내림 정렬한다 - 하위 비트에 다른 용도의 플래그가 섞여 있을 수 있기 때문이다. */

	if (!region)	/* [한국어] 0 이면 이 기능이 꺼져 있거나 주소가 배정되지 않은 것이므로 등록하지 않는다. */
		return;

	res->name = pci_name(dev);	/* [한국어] 리소스 이름을 장치 이름으로 세운다. */
	res->flags = IORESOURCE_IO;	/* [한국어] IO 포트 공간 리소스로 표시한다. */

	/* Convert from PCI bus to resource space */
	bus_region.start = region;	/* [한국어] PCI 버스 주소 기준 시작. */
	bus_region.end = region + size - 1;	/* [한국어] 닫힌 구간의 끝. */
	pcibios_bus_to_resource(dev->bus, res, &bus_region);	/* [한국어] 버스 주소를 CPU 주소로 변환해 res 에 채운다. */

	/*
	 * "res" is typically a bridge window resource that's not being
	 * used for a bridge window, so it's just a place to stash this
	 * non-standard resource.  Printing "nr" or pci_resource_name() of
	 * it doesn't really make sense.
	 */
	if (!pci_claim_resource(dev, nr))	/* [한국어] pci_claim_resource() 는 이 구간을 시스템 리소스 트리에 등록해 다른 장치가 겹쳐 쓰지 못하게 한다. 0 을 반환하면 성공이다. */
		pci_info(dev, "quirk: %pR claimed by %s\n", res, name);	/* [한국어] 성공했을 때만 어떤 구간을 누가 가져갔는지 남긴다. */
}

/*
 * [한국어]
 * quirk_ati_exploding_mce - ATI 노스브리지에서 읽으면 CPU 가 죽는 IO 포트를 선점한다
 *
 * @dev: ATI RS100 노스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] ATI RS100(Radeon IGP) 노스브리지.
 * [무엇이 문제] 아래 영어 주석대로, IO 포트 0x3b0~0x3bb 사이 또는 0x3d3 을
 * '읽기만 해도' 프로세서가 MCE(Machine Check Exception)를 낸다. 정상적인
 * 하드웨어라면 읽기는 부작용이 없거나 최소한 CPU 를 죽이지는 않는다.
 * [그대로 두면] VGA 레거시 포트를 탐색하는 드라이버(예: 콘솔/VGA 관련
 * 코드가 0x3b0 대역을 훑는다)가 실행되는 순간 시스템이 즉사한다.
 * [우회] 그 IO 구간을 커널 리소스로 미리 예약(request_region)해 버린다.
 * 예약된 구간은 다른 드라이버가 request_region 에 실패해 접근을 포기하므로,
 * 아무도 그 포트를 건드리지 않게 된다. 즉 '고치는' 것이 아니라 '아무도 못
 * 만지게 막는' 방식의 회피다.
 *
 * 실행 컨텍스트: FINAL 단계. 다른 드라이버가 뜨기 전에 선점해야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_ati_exploding_mce] -> request_region()
 */
/*
 * ATI Northbridge setups MCE the processor if you even read somewhere
 * between 0x3b0->0x3bb or read 0x3d3
 */
static void quirk_ati_exploding_mce(struct pci_dev *dev)
{
	pci_info(dev, "ATI Northbridge, reserving I/O ports 0x3b0 to 0x3bb\n");	/* [한국어] 어떤 구간을 왜 예약하는지 dmesg 에 남긴다. */
	/* Mae rhaid i ni beidio ag edrych ar y lleoliadiau I/O hyn */
	/* [한국어] 바로 위 원본 주석은 영어가 아니라 웨일스어로 적혀 있으며,
	 * 뜻은 '우리는 이 IO 위치들을 들여다보아서는 안 된다' 이다. */
	request_region(0x3b0, 0x0C, "RadeonIGP");	/* [한국어] 0x3b0 부터 12바이트(0x0C), 즉 0x3b0~0x3bb 를 RadeonIGP 이름으로 예약한다. 영어 주석이 말한 위험 구간과 정확히 일치한다. */
	request_region(0x3d3, 0x01, "RadeonIGP");	/* [한국어] 0x3d3 한 바이트도 따로 예약한다. 반환값을 보지 않는 것은 이미 누가 잡고 있어도 어차피 목적(다른 드라이버 차단)은 달성되기 때문이다. */
}
/* [한국어] ATI RS100 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI,	PCI_DEVICE_ID_ATI_RS100,   quirk_ati_exploding_mce);

/*
 * [한국어]
 * quirk_amd_dwc_class - AMD 의 DesignWare USB 컨트롤러 클래스 코드를 바꿔 xhci 대신 dwc3 가 잡게 한다
 *
 * @pdev: AMD NL [1022:7912] 또는 VanGogh [1022:163a] USB 장치
 * @return: 없음
 *
 * [어떤 하드웨어] AMD NL 플랫폼의 [1022:7912], VanGogh 플랫폼의 [1022:163a].
 * [무엇이 문제] 아래 영어 주석대로, 이 장치는 클래스 코드를
 * PCI_CLASS_SERIAL_USB_XHCI(0x0c0330) 로 광고한다. 스펙 위반은 아니지만,
 * 그 결과 범용 xhci 드라이버가 이 장치를 가져가 버린다.
 * [그대로 두면] 이 실리콘(Synopsys DesignWare USB3)에 더 잘 맞는 dwc3
 * 드라이버 대신 xhci 가 붙어 기능이 제한된다.
 * [우회] 클래스 코드를 0x0c03fe 로 바꾼다. 이 값은 PCI r3.0 스펙이
 * 'USB device (not host controller)' 로 정의한 코드로, xhci 는 매칭되지
 * 않고 dwc3 가 vendor/device ID 로 이 장치를 잡을 수 있게 된다. 즉 클래스
 * 코드를 드라이버 선택의 지렛대로 쓰는 quirk 다.
 *
 * [주의] 이 quirk 는 하드웨어 레지스터를 바꾸지 않는다. 커널이 들고 있는
 * struct pci_dev 의 class 필드만 고친다 - 드라이버 매칭은 그 필드로 하기
 * 때문이다.
 *
 * 실행 컨텍스트: HEADER 단계. 드라이버 매칭이 일어나기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_amd_dwc_class]
 */
/*
 * In the AMD NL platform, this device ([1022:7912]) has a class code of
 * PCI_CLASS_SERIAL_USB_XHCI (0x0c0330), which means the xhci driver will
 * claim it. The same applies on the VanGogh platform device ([1022:163a]).
 *
 * But the dwc3 driver is a more specific driver for this device, and we'd
 * prefer to use it instead of xhci. To prevent xhci from claiming the
 * device, change the class code to 0x0c03fe, which the PCI r3.0 spec
 * defines as "USB device (not host controller)". The dwc3 driver can then
 * claim it based on its Vendor and Device ID.
 */
static void quirk_amd_dwc_class(struct pci_dev *pdev)
{
	u32 class = pdev->class;	/* [한국어] 바꾸기 전 원래 클래스 코드를 로그에 찍기 위해 보관한다. */

	if (class != PCI_CLASS_SERIAL_USB_DEVICE) {	/* [한국어] 이미 목표 값이면 아무것도 하지 않는다 - 같은 장치에 quirk 가 두 번 적용돼도 안전하게 만드는 방어다. */
		/* Use "USB Device (not host controller)" class */
		pdev->class = PCI_CLASS_SERIAL_USB_DEVICE;	/* [한국어] 커널이 기억하는 클래스 코드를 'USB 장치(호스트 컨트롤러 아님)' 로 바꾼다. 하드웨어는 그대로다. */
		pci_info(pdev,	/* [한국어] 무엇이 무엇으로 바뀌었는지 남긴다. */
			"PCI class overridden (%#08x -> %#08x) so dwc3 driver can claim this instead of xhci\n",
			class, pdev->class);
	}
}
/* [한국어] AMD NL 플랫폼의 USB 장치에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_NL_USB,
		quirk_amd_dwc_class);
/* [한국어] VanGogh 플랫폼의 USB 장치도 같은 처리를 한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_VANGOGH_USB,
		quirk_amd_dwc_class);

/*
 * [한국어]
 * quirk_synopsys_haps - Synopsys HAPS 검증 보드의 USB 클래스 코드를 바꾼다
 *
 * @pdev: Synopsys 벤더의 xHCI 클래스 장치
 * @return: 없음
 *
 * [어떤 하드웨어] Synopsys USB 3.x 호스트 HAPS 플랫폼(FPGA 기반 검증 보드).
 * [무엇이 문제] 위 quirk_amd_dwc_class 와 같은 상황이다. 클래스 코드가
 * xHCI 라서 xhci-pci 드라이버가 가져가지만, 실제로는 dwc3-haps 드라이버가
 * 맡아야 하는 장치다.
 * [우회] 클래스 코드를 PCI_CLASS_SERIAL_USB_DEVICE 로 바꿔 xhci-pci 의
 * 매칭에서 빠지게 한다.
 *
 * [등록 방식의 차이] 이 quirk 는 DECLARE_PCI_FIXUP_CLASS_HEADER 로
 * 등록된다. device ID 는 PCI_ANY_ID 로 두고 대신 클래스 코드가 xHCI 인
 * 것만 고르며, class_shift 가 0 이라 24비트 클래스 코드 전체를 정확히
 * 비교한다. 그런 뒤 함수 안에서 device ID 로 한 번 더 걸러 낸다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_synopsys_haps]
 */
/*
 * Synopsys USB 3.x host HAPS platform has a class code of
 * PCI_CLASS_SERIAL_USB_XHCI, and xhci driver can claim it.  However, these
 * devices should use dwc3-haps driver.  Change these devices' class code to
 * PCI_CLASS_SERIAL_USB_DEVICE to prevent the xhci-pci driver from claiming
 * them.
 */
static void quirk_synopsys_haps(struct pci_dev *pdev)
{
	u32 class = pdev->class;	/* [한국어] 바꾸기 전 클래스 코드를 로그용으로 보관한다. */

	switch (pdev->device) {	/* [한국어] 클래스로 걸러진 Synopsys 장치 중에서도 아래 세 모델만 대상이다. */
	case PCI_DEVICE_ID_SYNOPSYS_HAPSUSB3:	/* [한국어] HAPS USB3 보드. */
	case PCI_DEVICE_ID_SYNOPSYS_HAPSUSB3_AXI:	/* [한국어] HAPS USB3 AXI 인터페이스 변종. case 를 이어 붙여 같은 처리를 한다. */
	case PCI_DEVICE_ID_SYNOPSYS_HAPSUSB31:	/* [한국어] HAPS USB3.1 보드. */
		pdev->class = PCI_CLASS_SERIAL_USB_DEVICE;	/* [한국어] 세 모델 모두 클래스 코드를 'USB 장치' 로 바꾼다. */
		pci_info(pdev, "PCI class overridden (%#08x -> %#08x) so dwc3 driver can claim this instead of xhci\n",	/* [한국어] 무엇이 무엇으로 바뀌었는지 남긴다. */
			 class, pdev->class);
		break;
	}
}
/* [한국어] 벤더는 Synopsys, device 는 아무거나, 클래스는 정확히 xHCI 인
 * 장치를 HEADER 단계에서 잡는다. class_shift 0 은 클래스 코드 24비트를
 * 하나도 버리지 않고 비교한다는 뜻이다. */
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_SYNOPSYS, PCI_ANY_ID,
			       PCI_CLASS_SERIAL_USB_XHCI, 0,
			       quirk_synopsys_haps);

/*
 * [한국어]
 * quirk_ali7101_acpi - ALi M7101 사우스브리지의 숨은 ACPI/SMB IO 영역을 리소스로 등록
 *
 * @dev: ALi M7101 사우스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] ALi M7101 사우스브리지(전원관리 함수).
 * [무엇이 문제] 이 칩은 ACPI 레지스터와 SMB(System Management Bus)
 * 레지스터가 놓인 IO 영역을 표준 BAR 로 광고하지 않고 벤더 전용 레지스터
 * 0xE0 / 0xE2 에만 적어 둔다. 커널의 리소스 관리자는 BAR 만 보므로 이
 * 영역의 존재를 모른다.
 * [그대로 두면] 아래 영어 주석이 든 예처럼, 누군가 그 IO 대역을 탐색하다
 * 잘못된 ACPI 레지스터를 읽으면 기계가 잠들어 다시 깨울 방법이 없어진다.
 * 또 커널이 같은 IO 범위를 다른 장치에 배정해 충돌할 수도 있다.
 * [우회] 두 영역을 quirk_io_region() 으로 찾아 리소스 트리에 등록해 둔다.
 * 그러면 다른 드라이버가 그 범위를 잡지 못하고, /proc/ioports 에도 드러난다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_ali7101_acpi] -> quirk_io_region()
 */
/*
 * Let's make the southbridge information explicit instead of having to
 * worry about people probing the ACPI areas, for example.. (Yes, it
 * happens, and if you read the wrong ACPI register it will put the machine
 * to sleep with no way of waking it up again. Bummer).
 *
 * ALI M7101: Two IO regions pointed to by words at
 *	0xE0 (64 bytes of ACPI registers)
 *	0xE2 (32 bytes of SMB registers)
 */
static void quirk_ali7101_acpi(struct pci_dev *dev)
{
	quirk_io_region(dev, 0xE0, 64, PCI_BRIDGE_RESOURCES, "ali7101 ACPI");	/* [한국어] 오프셋 0xE0 의 16비트 값이 ACPI 레지스터 블록(64바이트)의 IO 베이스다. PCI_BRIDGE_RESOURCES 는 브리지 창용 리소스 슬롯의 첫 번호로, 여기서는 비어 있는 칸을 임시 보관함으로 빌려 쓴다. */
	quirk_io_region(dev, 0xE2, 32, PCI_BRIDGE_RESOURCES+1, "ali7101 SMB");	/* [한국어] 오프셋 0xE2 의 16비트 값이 SMB 레지스터 블록(32바이트)의 IO 베이스다. 슬롯은 그다음 칸을 쓴다. */
}
/* [한국어] ALi M7101 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AL,	PCI_DEVICE_ID_AL_M7101,		quirk_ali7101_acpi);

/*
 * [한국어]
 * piix4_io_quirk - PIIX4 의 프로그래머블 IO 디바이스 리소스를 해독해 출력한다
 *
 * @dev: Intel PIIX4 계열 브리지
 * @name: 로그에 남길 리소스 이름(예: PIIX4 devres B)
 * @port: 그 디바이스 리소스 서술자가 놓인 Configuration Space 오프셋
 * @enable: 이 리소스가 켜져 있는지 판정할 비트 마스크
 * @return: 없음
 *
 * [왜 필요한가] PIIX4 는 BAR 가 아닌 벤더 전용 32비트 레지스터에 '이
 * 브리지가 디코딩하는 추가 IO 영역'을 담는다. 커널은 그것을 모르므로
 * 리소스 충돌이 날 수 있다. 다만 아래 영어 주석대로, 지금 이 코드는
 * 예약까지 하지 않고 '무엇이 있는지 출력만' 한다. 충분한 확인 보고가
 * 모이면 예약하도록 바꾸겠다는 것이 원저자의 의도다.
 *
 * [서술자 해독] 32비트 값 devres 에서
 *   - 하위 16비트: IO 베이스 주소
 *   - 비트 19:16: 주소 마스크(무시할 하위 주소 비트를 나타냄)
 *   - enable 로 지정된 비트들: 이 리소스가 켜져 있는지
 *
 * 실행 컨텍스트: quirk_piix4_acpi() 안, HEADER 단계.
 *
 * 호출 체인:
 *   quirk_piix4_acpi() -> [piix4_io_quirk] -> pci_read_config_dword()
 */
static void piix4_io_quirk(struct pci_dev *dev, const char *name, unsigned int port, unsigned int enable)
{
	u32 devres;	/* [한국어] 디바이스 리소스 서술자 32비트 원본. */
	u32 mask, size, base;	/* [한국어] mask: 주소의 무시 비트, size: 계산된 영역 크기, base: 정렬된 시작 주소. */

	pci_read_config_dword(dev, port, &devres);	/* [한국어] 지정된 오프셋에서 서술자를 읽는다. 표준 헤더가 아닌 PIIX4 고유 영역이다. */
	if ((devres & enable) != enable)	/* [한국어] enable 로 지정된 비트가 '모두' 서 있어야 이 리소스가 활성이다. 하나라도 빠지면 꺼진 것으로 보고 아무것도 하지 않는다. */
		return;
	mask = (devres >> 16) & 15;	/* [한국어] 비트 19:16 을 뽑는다(>>16 후 하위 4비트). 이 4비트가 주소 마스크다. */
	base = devres & 0xffff;	/* [한국어] 하위 16비트가 IO 베이스 주소다. */
	size = 16;	/* [한국어] 크기 후보를 16바이트부터 시작한다 - 이 서술자가 표현할 수 있는 최대 크기다. */
	for (;;) {	/* [한국어] 크기를 반씩 줄여 가며 마스크와 맞는 지점을 찾는 무한 루프. 아래 break 로만 빠져나온다. */
		unsigned int bit = size >> 1;	/* [한국어] 현재 크기의 절반 - 마스크에서 확인할 비트 위치다. */
		if ((bit & mask) == bit)	/* [한국어] 그 비트가 마스크에 서 있으면 그 비트까지가 무시 대상이므로, 영역 크기는 지금의 size 다. */
			break;
		size = bit;	/* [한국어] 아직 아니면 크기를 반으로 줄여 다음 비트를 본다. 마스크가 0 이면 size 는 1 까지 줄어든 뒤 멈춘다. */
	}
	/*
	 * For now we only print it out. Eventually we'll want to
	 * reserve it (at least if it's in the 0x1000+ range), but
	 * let's get enough confirmation reports first.
	 */
	base &= -size;	/* [한국어] -size 는 2의 보수에서 ~(size-1) 과 같아, 주소를 size 경계로 내림 정렬하는 관용 표현이다. */
	pci_info(dev, "%s PIO at %04x-%04x\n", name, base, base + size - 1);	/* [한국어] 찾아낸 구간을 닫힌 범위로 출력한다. 예약은 하지 않는다. */
}

/*
 * [한국어]
 * piix4_mem_quirk - PIIX4 의 프로그래머블 MMIO 디바이스 리소스를 해독해 출력한다
 *
 * @dev: Intel PIIX4 계열 브리지
 * @name: 로그에 남길 리소스 이름
 * @port: 서술자가 놓인 Configuration Space 오프셋
 * @enable: 활성 여부를 판정할 비트 마스크
 * @return: 없음
 *
 * piix4_io_quirk() 의 메모리 판이다. 서술자의 필드 배치가 달라 해독 방법만
 * 다르고 나머지 구조는 같다.
 *   - 상위 16비트(0xffff0000): MMIO 베이스 주소
 *   - 비트 5:0 을 16 만큼 왼쪽으로 민 값: 주소 마스크
 *   - 최대 크기는 128 << 16 = 8MB
 * 아래 영어 주석대로 이 코드도 아직 예약은 하지 않고 출력만 한다.
 *
 * 실행 컨텍스트: quirk_piix4_acpi() 안, HEADER 단계.
 *
 * 호출 체인:
 *   quirk_piix4_acpi() -> [piix4_mem_quirk] -> pci_read_config_dword()
 */
static void piix4_mem_quirk(struct pci_dev *dev, const char *name, unsigned int port, unsigned int enable)
{
	u32 devres;	/* [한국어] 디바이스 리소스 서술자 32비트 원본. */
	u32 mask, size, base;	/* [한국어] mask: 주소 무시 비트, size: 계산된 크기, base: 정렬된 시작 주소. */

	pci_read_config_dword(dev, port, &devres);	/* [한국어] 서술자를 읽는다. */
	if ((devres & enable) != enable)	/* [한국어] enable 비트가 모두 서 있어야 활성이다. */
		return;
	base = devres & 0xffff0000;	/* [한국어] 상위 16비트가 MMIO 베이스 - IO 판과 달리 주소 해상도가 64KB 단위다. */
	mask = (devres & 0x3f) << 16;	/* [한국어] 하위 6비트가 마스크이고, 주소와 자릿수를 맞추기 위해 16비트 왼쪽으로 민다. */
	size = 128 << 16;	/* [한국어] 크기 후보의 최대값 8MB(128 * 64KB)에서 시작한다. */
	for (;;) {	/* [한국어] IO 판과 같은 방식으로 크기를 반씩 줄여 가며 마스크와 맞는 지점을 찾는다. */
		unsigned int bit = size >> 1;	/* [한국어] 현재 크기의 절반 - 마스크에서 확인할 비트. */
		if ((bit & mask) == bit)	/* [한국어] 그 비트가 마스크에 서 있으면 지금의 size 가 영역 크기다. */
			break;
		size = bit;	/* [한국어] 아니면 절반으로 줄여 다시 본다. */
	}

	/*
	 * For now we only print it out. Eventually we'll want to
	 * reserve it, but let's get enough confirmation reports first.
	 */
	base &= -size;	/* [한국어] 주소를 size 경계로 내림 정렬한다. */
	pci_info(dev, "%s MMIO at %04x-%04x\n", name, base, base + size - 1);	/* [한국어] 찾아낸 MMIO 구간을 출력한다. 예약은 하지 않는다. */
}

/*
 * [한국어]
 * quirk_piix4_acpi - PIIX4 의 숨은 ACPI/SMB IO 영역 등록과 디바이스 리소스 보고
 *
 * @dev: Intel 82371AB(PIIX4) 또는 82443MX 의 전원관리 함수
 * @return: 없음
 *
 * [어떤 하드웨어] Intel PIIX4 계열 사우스브리지의 전원관리 함수.
 * [무엇이 문제] ALi M7101 과 같은 부류의 문제다. ACPI 레지스터(64바이트)와
 * SMB 레지스터(16바이트)의 IO 베이스가 표준 BAR 가 아니라 벤더 전용
 * 롱워드(0x40, 0x90)에만 적혀 있다. 게다가 PIIX4 에는 프로그래머블
 * 디바이스 리소스(devres A~J)라는 추가 디코딩 창이 여럿 있다.
 * [그대로 두면] 커널이 모르는 IO 영역이 생겨 다른 장치와 충돌하거나,
 * 탐색 코드가 ACPI 레지스터를 잘못 건드려 기계가 잠들 수 있다.
 * [우회] ACPI/SMB 두 영역은 quirk_io_region() 으로 리소스 트리에 등록하고,
 * 나머지 devres 창들은 아직 예약하지 않고 내용을 해독해 로그로만 남긴다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_piix4_acpi]
 *     -> quirk_io_region() / piix4_io_quirk() / piix4_mem_quirk()
 */
/*
 * PIIX4 ACPI: Two IO regions pointed to by longwords at
 *	0x40 (64 bytes of ACPI registers)
 *	0x90 (16 bytes of SMB registers)
 * and a few strange programmable PIIX4 device resources.
 */
static void quirk_piix4_acpi(struct pci_dev *dev)
{
	u32 res_a;	/* [한국어] Device Resource A(오프셋 0x5c) 값. 다른 디바이스 리소스의 활성 여부를 담고 있다. */

	quirk_io_region(dev, 0x40, 64, PCI_BRIDGE_RESOURCES, "PIIX4 ACPI");	/* [한국어] 0x40 롱워드가 가리키는 ACPI 레지스터 64바이트를 리소스로 등록한다. */
	quirk_io_region(dev, 0x90, 16, PCI_BRIDGE_RESOURCES+1, "PIIX4 SMB");	/* [한국어] 0x90 롱워드가 가리키는 SMB 레지스터 16바이트를 등록한다. */

	/* Device resource A has enables for some of the other ones */
	pci_read_config_dword(dev, 0x5c, &res_a);	/* [한국어] 위 영어 주석대로 Device Resource A 는 다른 리소스들의 활성 비트를 모아 둔 레지스터다. 먼저 읽어 두고 아래에서 비트를 검사한다. */

	piix4_io_quirk(dev, "PIIX4 devres B", 0x60, 3 << 21);	/* [한국어] devres B(0x60): 활성 판정 비트는 21:20 두 개(3 << 21 은 비트 22:21). */
	piix4_io_quirk(dev, "PIIX4 devres C", 0x64, 3 << 21);	/* [한국어] devres C(0x64): B 와 같은 활성 비트 배치. */

	/* Device resource D is just bitfields for static resources */

	/* Device 12 enabled? */
	/* [한국어] 위 영어 주석대로 Device Resource D 는 고정 리소스용 비트필드라
	 * 별도 해독이 필요 없다. 그래서 D 는 건너뛴다. */
	if (res_a & (1 << 29)) {	/* [한국어] res_a 의 비트 29 가 서 있으면 12번 디바이스가 켜져 있다는 뜻이고, 그때만 E/F 창이 의미를 갖는다. */
		piix4_io_quirk(dev, "PIIX4 devres E", 0x68, 1 << 20);	/* [한국어] devres E(0x68): IO 창, 활성 비트는 20. */
		piix4_mem_quirk(dev, "PIIX4 devres F", 0x6c, 1 << 7);	/* [한국어] devres F(0x6c): MMIO 창, 활성 비트는 7. */
	}
	/* Device 13 enabled? */
	if (res_a & (1 << 30)) {	/* [한국어] 비트 30 은 13번 디바이스의 활성 표시다. */
		piix4_io_quirk(dev, "PIIX4 devres G", 0x70, 1 << 20);	/* [한국어] devres G(0x70): IO 창. */
		piix4_mem_quirk(dev, "PIIX4 devres H", 0x74, 1 << 7);	/* [한국어] devres H(0x74): MMIO 창. */
	}
	piix4_io_quirk(dev, "PIIX4 devres I", 0x78, 1 << 20);	/* [한국어] devres I(0x78): 별도 활성 조건 없이 항상 검사한다. */
	piix4_io_quirk(dev, "PIIX4 devres J", 0x7c, 1 << 20);	/* [한국어] devres J(0x7c): 마찬가지. */
}
/* [한국어] PIIX4(82371AB) 와 82443MX 의 전원관리 함수에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82371AB_3,	quirk_piix4_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82443MX_3,	quirk_piix4_acpi);

/* [한국어] 아래 10개 매크로는 Intel ICH 계열 LPC 브리지의 벤더 전용
 * Configuration Space 오프셋과 활성 비트다. 표준 PCI 헤더가 아니므로
 * 스펙이 아니라 각 칩셋 데이터시트에서 온 값이다. ICH4 세대와 ICH6 세대는
 * 같은 기능이 서로 다른 오프셋/비트에 놓여 있어 이름을 나눠 두었다. */
#define ICH_PMBASE	0x40	/* [한국어] PMBASE - ACPI 전원관리 레지스터 블록의 IO 베이스가 들어 있는 오프셋. ICH4 와 ICH6 이 같다. */
#define ICH_ACPI_CNTL	0x44	/* [한국어] ACPI Control - ACPI 블록 디코딩 활성 비트가 들어 있는 오프셋. */
#define  ICH4_ACPI_EN	0x10	/* [한국어] ICH4 세대의 ACPI 디코딩 활성 비트(비트 4). */
#define  ICH6_ACPI_EN	0x80	/* [한국어] ICH6 세대의 ACPI 디코딩 활성 비트(비트 7) - 같은 레지스터인데 비트 위치가 다르다. */
#define ICH4_GPIOBASE	0x58	/* [한국어] ICH4 의 GPIO IO 베이스 오프셋. */
#define ICH4_GPIO_CNTL	0x5c	/* [한국어] ICH4 의 GPIO 제어 레지스터 오프셋. */
#define  ICH4_GPIO_EN	0x10	/* [한국어] ICH4 의 GPIO 디코딩 활성 비트(비트 4). */
#define ICH6_GPIOBASE	0x48	/* [한국어] ICH6 의 GPIO IO 베이스 오프셋 - ICH4 와 다르다. */
#define ICH6_GPIO_CNTL	0x4c	/* [한국어] ICH6 의 GPIO 제어 레지스터 오프셋. */
#define  ICH6_GPIO_EN	0x10	/* [한국어] ICH6 의 GPIO 디코딩 활성 비트(비트 4). */

/*
 * [한국어]
 * quirk_ich4_lpc_acpi - ICH4/ICH5 LPC 브리지의 숨은 ACPI/GPIO IO 영역을 등록
 *
 * @dev: Intel ICH4/ICH5 세대 LPC 브리지(82801AA~82801EB, ESB)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel ICH4, ICH4-M, ICH5, ICH5-M 및 ESB 사우스브리지의
 * LPC 브리지 함수.
 * [무엇이 문제] ALi M7101, PIIX4 와 같은 부류다. ACPI/GPIO/TCO 레지스터
 * 블록(128바이트)과 GPIO IO 공간(64바이트)의 베이스 주소가 표준 BAR 가
 * 아니라 벤더 전용 롱워드 0x40, 0x58 에만 적혀 있다.
 * [그대로 두면] 커널이 모르는 IO 영역이 남아 다른 장치와 충돌할 수 있고,
 * 탐색 코드가 ACPI 레지스터를 잘못 건드릴 위험이 있다.
 * [우회] 각 블록의 활성 비트를 확인한 뒤, 켜져 있는 것만 quirk_io_region()
 * 으로 리소스 트리에 등록한다. 꺼져 있는 블록을 등록하면 있지도 않은 IO
 * 영역을 예약하게 되므로 반드시 활성 검사를 먼저 한다.
 *
 * [주의 - 주석과 코드의 불일치] 아래 영어 주석은 PCIBIOS_MIN_IO 검사를
 * 설명하지만, 현재 이 함수에는 그 검사가 없다. 이 파일 전체에서
 * PCIBIOS_MIN_IO 는 그 주석 안에서 딱 한 번 나올 뿐 코드에는 쓰이지 않는다.
 * 과거 코드의 흔적으로 남은 주석으로 보인다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_ich4_lpc_acpi] -> quirk_io_region()
 */
/*
 * ICH4, ICH4-M, ICH5, ICH5-M ACPI: Three IO regions pointed to by longwords at
 *	0x40 (128 bytes of ACPI, GPIO & TCO registers)
 *	0x58 (64 bytes of GPIO I/O space)
 */
static void quirk_ich4_lpc_acpi(struct pci_dev *dev)
{
	u8 enable;	/* [한국어] 제어 레지스터에서 읽은 활성 비트들을 담는 한 바이트. ACPI 검사와 GPIO 검사에 재사용된다. */

	/*
	 * The check for PCIBIOS_MIN_IO is to ensure we won't create a conflict
	 * with low legacy (and fixed) ports. We don't know the decoding
	 * priority and can't tell whether the legacy device or the one created
	 * here is really at that address.  This happens on boards with broken
	 * BIOSes.
	 */
	pci_read_config_byte(dev, ICH_ACPI_CNTL, &enable);	/* [한국어] ACPI Control 레지스터(0x44)를 읽는다. */
	if (enable & ICH4_ACPI_EN)	/* [한국어] ICH4 세대의 ACPI 활성 비트가 서 있을 때만 등록한다. */
		quirk_io_region(dev, ICH_PMBASE, 128, PCI_BRIDGE_RESOURCES,	/* [한국어] PMBASE(0x40)가 가리키는 128바이트 블록을 등록한다. 이 블록에 ACPI, GPIO, TCO 레지스터가 함께 들어 있다. */
				 "ICH4 ACPI/GPIO/TCO");

	pci_read_config_byte(dev, ICH4_GPIO_CNTL, &enable);	/* [한국어] GPIO Control 레지스터(0x5c)를 읽는다. enable 변수를 재사용한다. */
	if (enable & ICH4_GPIO_EN)	/* [한국어] GPIO 활성 비트가 서 있을 때만 등록한다. */
		quirk_io_region(dev, ICH4_GPIOBASE, 64, PCI_BRIDGE_RESOURCES+1,	/* [한국어] GPIOBASE(0x58)가 가리키는 64바이트 GPIO IO 공간을 그다음 리소스 슬롯에 등록한다. */
				"ICH4 GPIO");
}
/* [한국어] ICH4/ICH5 세대 LPC 브리지 10종에 HEADER 단계로 등록한다.
 * 같은 칩셋이라도 모델과 함수 번호마다 device ID 가 달라 하나씩 나열한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801AA_0,		quirk_ich4_lpc_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801AB_0,		quirk_ich4_lpc_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801BA_0,		quirk_ich4_lpc_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801BA_10,	quirk_ich4_lpc_acpi);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_82801CA_0, PCI_DEVICE_ID_INTEL_82801CA_12,
 * PCI_DEVICE_ID_INTEL_82801DB_0, PCI_DEVICE_ID_INTEL_82801DB_12. 위 블록 주석의 설명이
 * 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801CA_0,		quirk_ich4_lpc_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801CA_12,	quirk_ich4_lpc_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801DB_0,		quirk_ich4_lpc_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801DB_12,	quirk_ich4_lpc_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_82801EB_0,		quirk_ich4_lpc_acpi);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_ESB_1. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,    PCI_DEVICE_ID_INTEL_ESB_1,		quirk_ich4_lpc_acpi);

/*
 * [한국어]
 * ich6_lpc_acpi_gpio - ICH6 이후 세대가 공유하는 ACPI/GPIO 영역 등록 루틴
 *
 * @dev: ICH6 이상 세대의 LPC 브리지
 * @return: 없음
 *
 * quirk_ich4_lpc_acpi() 와 하는 일은 같지만 활성 비트와 GPIO 베이스
 * 오프셋이 ICH6 세대에서 바뀌었기 때문에 별도 함수로 나뉘어 있다.
 * ICH6 전용 quirk 와 ICH7~10 전용 quirk 가 이 함수를 공유한다.
 *
 * 실행 컨텍스트: quirk_ich6_lpc()/quirk_ich7_lpc() 안, HEADER 단계.
 *
 * 호출 체인:
 *   quirk_ich6_lpc() 또는 quirk_ich7_lpc() -> [ich6_lpc_acpi_gpio]
 *     -> quirk_io_region()
 */
static void ich6_lpc_acpi_gpio(struct pci_dev *dev)
{
	u8 enable;	/* [한국어] 활성 비트를 담는 한 바이트. 두 번 재사용한다. */

	pci_read_config_byte(dev, ICH_ACPI_CNTL, &enable);	/* [한국어] ACPI Control 레지스터(0x44)를 읽는다 - 오프셋은 ICH4 와 같다. */
	if (enable & ICH6_ACPI_EN)	/* [한국어] ICH6 세대의 활성 비트는 비트 7 이다(ICH4 는 비트 4). */
		quirk_io_region(dev, ICH_PMBASE, 128, PCI_BRIDGE_RESOURCES,	/* [한국어] PMBASE 가 가리키는 128바이트 ACPI/GPIO/TCO 블록을 등록한다. */
				 "ICH6 ACPI/GPIO/TCO");

	pci_read_config_byte(dev, ICH6_GPIO_CNTL, &enable);	/* [한국어] ICH6 의 GPIO Control 레지스터(0x4c)를 읽는다 - ICH4 의 0x5c 와 다르다. */
	if (enable & ICH6_GPIO_EN)	/* [한국어] GPIO 활성 비트 확인. */
		quirk_io_region(dev, ICH6_GPIOBASE, 64, PCI_BRIDGE_RESOURCES+1,	/* [한국어] ICH6 의 GPIOBASE(0x48)가 가리키는 64바이트를 등록한다. */
				"ICH6 GPIO");
}

/*
 * [한국어]
 * ich6_lpc_generic_decode - ICH6 의 LPC 범용 IO 디코드 창을 해독해 출력한다
 *
 * @dev: ICH6 LPC 브리지
 * @reg: 디코드 창 서술자가 놓인 Configuration Space 오프셋
 * @name: 로그에 남길 이름
 * @dynsize: 크기가 가변인 창이면 1, 고정 128바이트면 0
 * @return: 없음
 *
 * [무엇을 하는가] LPC 브리지에는 '이 IO 주소 범위는 LPC 버스로 내려보낸다'
 * 를 정하는 범용 디코드 창이 있다. 이 창도 BAR 가 아니라 벤더 전용
 * 레지스터에만 적혀 있어 커널이 모른다. 이 함수는 그 서술자를 해독해
 * 어떤 범위가 열려 있는지 로그로 알린다.
 *
 * [아직 예약하지 않는 이유] 아래 영어 주석대로, 지금은 출력만 하고 리소스
 * 예약은 하지 않는다. 더 많은 확인이 필요하다는 것이 원저자의 판단이다.
 *
 * [크기 계산의 한계] dynsize 인 창의 실제 크기는 D31:F0 의 오프셋 0xAD
 * 레지스터 비트 5:4 에 따라 16/32/64 바이트로 달라지는데, 이 코드는 그것을
 * 읽지 않고 16 으로 가정한다. 영어 주석이 '이것은 정확하지 않지만 적어도
 * 일부는 알려 준다'고 스스로 밝히고 있다.
 *
 * 실행 컨텍스트: quirk_ich6_lpc() 안, HEADER 단계.
 *
 * 호출 체인:
 *   quirk_ich6_lpc() -> [ich6_lpc_generic_decode] -> pci_read_config_dword()
 */
static void ich6_lpc_generic_decode(struct pci_dev *dev, unsigned int reg,
				    const char *name, int dynsize)
{
	u32 val;	/* [한국어] 디코드 창 서술자 32비트 원본. */
	u32 size, base;	/* [한국어] size: 창 크기, base: 정렬된 IO 시작 주소. */

	pci_read_config_dword(dev, reg, &val);	/* [한국어] 지정된 오프셋에서 서술자를 읽는다. */

	/* Enabled? */
	if (!(val & 1))	/* [한국어] 위 영어 주석대로 비트 0 이 이 창의 활성 비트다. 꺼져 있으면 창이 없는 것이다. */
		return;
	base = val & 0xfffc;	/* [한국어] 비트 15:2 가 IO 베이스다. 0xfffc 마스크는 하위 2비트(활성 비트 등)를 떨어낸다 - LPC 디코드는 dword 단위라 하위 2비트가 주소로 쓰이지 않는다. */
	if (dynsize) {	/* [한국어] 가변 크기 창인지에 따라 크기 가정이 달라진다. */
		/*
		 * This is not correct. It is 16, 32 or 64 bytes depending on
		 * register D31:F0:ADh bits 5:4.
		 *
		 * But this gets us at least _part_ of it.
		 */
		size = 16;	/* [한국어] 가변 창은 실제로 16/32/64 중 하나지만, 그 값을 읽지 않고 최소값 16 으로 가정한다(위 영어 주석의 한계). */
	} else {
		size = 128;	/* [한국어] 고정 창은 128바이트다. */
	}
	base &= ~(size-1);	/* [한국어] 베이스를 창 크기 경계로 내림 정렬한다. */

	/*
	 * Just print it out for now. We should reserve it after more
	 * debugging.
	 */
	pci_info(dev, "%s PIO at %04x-%04x\n", name, base, base+size-1);	/* [한국어] 찾아낸 IO 범위를 닫힌 구간으로 출력한다. 예약은 하지 않는다. */
}

/*
 * [한국어]
 * quirk_ich6_lpc - ICH6 LPC 브리지의 숨은 IO 영역을 등록/보고한다
 *
 * @dev: Intel ICH6 LPC 브리지(ICH6_0 또는 ICH6_1)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel ICH6 사우스브리지의 LPC 브리지 함수.
 * [무엇이 문제] ACPI/GPIO 블록과 LPC 범용 IO 디코드 창이 모두 표준 BAR
 * 밖의 벤더 전용 레지스터에만 기록되어 커널이 알지 못한다.
 * [우회] 공통 ACPI/GPIO 부분은 ich6_lpc_acpi_gpio() 로 리소스 등록까지
 * 하고, ICH6 고유의 범용 디코드 창 2개는 해독해 로그로만 남긴다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_ich6_lpc]
 *     -> ich6_lpc_acpi_gpio() / ich6_lpc_generic_decode()
 */
static void quirk_ich6_lpc(struct pci_dev *dev)
{
	/* Shared ACPI/GPIO decode with all ICH6+ */
	ich6_lpc_acpi_gpio(dev);	/* [한국어] 위 영어 주석대로 ICH6 이후 세대가 공통으로 쓰는 ACPI/GPIO 등록을 먼저 한다. */

	/* ICH6-specific generic IO decode */
	ich6_lpc_generic_decode(dev, 0x84, "LPC Generic IO decode 1", 0);	/* [한국어] 범용 IO 디코드 창 1(오프셋 0x84) - 고정 128바이트(dynsize=0). */
	ich6_lpc_generic_decode(dev, 0x88, "LPC Generic IO decode 2", 1);	/* [한국어] 범용 IO 디코드 창 2(오프셋 0x88) - 가변 크기(dynsize=1). */
}
/* [한국어] ICH6 LPC 브리지 2종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH6_0, quirk_ich6_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH6_1, quirk_ich6_lpc);

/*
 * [한국어]
 * ich7_lpc_generic_decode - ICH7 이후의 LPC 범용 IO 디코드 창을 해독해 출력한다
 *
 * @dev: ICH7~ICH10 LPC 브리지
 * @reg: 디코드 창 서술자가 놓인 오프셋
 * @name: 로그에 남길 이름
 * @return: 없음
 *
 * ICH6 판과 하는 일은 같지만 서술자 형식이 바뀌었다. ICH7 부터는 크기를
 * '길이'가 아니라 '주소 마스크'로 표현한다 - 비트 23:18 이 마스크이고,
 * 마스크에서 1 인 비트는 주소 비교에서 무시된다. 그래서 크기를 계산하는
 * 루프 없이 마스크를 그대로 출력한다.
 *
 * 이 함수도 ICH6 판과 마찬가지로 아직 리소스 예약은 하지 않는다.
 *
 * 실행 컨텍스트: quirk_ich7_lpc() 안, HEADER 단계.
 *
 * 호출 체인:
 *   quirk_ich7_lpc() -> [ich7_lpc_generic_decode] -> pci_read_config_dword()
 */
static void ich7_lpc_generic_decode(struct pci_dev *dev, unsigned int reg,
				    const char *name)
{
	u32 val;	/* [한국어] 서술자 32비트 원본. */
	u32 mask, base;	/* [한국어] mask: 주소 무시 비트, base: IO 시작 주소. */

	pci_read_config_dword(dev, reg, &val);	/* [한국어] 지정된 오프셋에서 서술자를 읽는다. */

	/* Enabled? */
	if (!(val & 1))	/* [한국어] 비트 0 이 활성 비트다. */
		return;

	/* IO base in bits 15:2, mask in bits 23:18, both are dword-based */
	base = val & 0xfffc;	/* [한국어] 위 영어 주석대로 비트 15:2 가 IO 베이스다(dword 단위). */
	mask = (val >> 16) & 0xfc;	/* [한국어] 비트 23:18 이 마스크다. >>16 후 0xfc 로 하위 2비트를 떨어내면 그 6비트가 남는다. */
	mask |= 3;	/* [한국어] 하위 2비트를 강제로 1 로 만든다. 디코드가 dword 단위라 주소 하위 2비트는 언제나 무시되기 때문이다. */

	/*
	 * Just print it out for now. We should reserve it after more
	 * debugging.
	 */
	pci_info(dev, "%s PIO at %04x (mask %04x)\n", name, base, mask);	/* [한국어] 베이스와 마스크를 그대로 출력한다. 예약은 하지 않는다. */
}

/*
 * [한국어]
 * quirk_ich7_lpc - ICH7~ICH10 LPC 브리지의 숨은 IO 영역을 등록/보고한다
 *
 * @dev: Intel ICH7 이후 세대의 LPC 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] 아래 영어 주석대로 ICH7 부터 ICH10 까지가 같은 형식의
 * 범용 IO 디코드 레지스터를 쓴다.
 * [무엇이 문제] ICH6 과 같은 부류 - 숨은 IO 영역이 BAR 밖에 있다.
 * [우회] ACPI/GPIO 는 ICH6 과 같은 루틴을 재사용해 등록하고, ICH7 세대의
 * 범용 디코드 창 4개는 해독해 로그로 남긴다. ICH6 의 2개보다 늘어난 것이
 * 세대 차이다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_ich7_lpc]
 *     -> ich6_lpc_acpi_gpio() / ich7_lpc_generic_decode()
 */
/* ICH7-10 has the same common LPC generic IO decode registers */
static void quirk_ich7_lpc(struct pci_dev *dev)
{
	/* We share the common ACPI/GPIO decode with ICH6 */
	ich6_lpc_acpi_gpio(dev);	/* [한국어] 위 영어 주석대로 ACPI/GPIO 처리는 ICH6 과 동일하므로 그대로 재사용한다. */

	/* And have 4 ICH7+ generic decodes */
	ich7_lpc_generic_decode(dev, 0x84, "ICH7 LPC Generic IO decode 1");	/* [한국어] 범용 IO 디코드 창 1(오프셋 0x84). */
	ich7_lpc_generic_decode(dev, 0x88, "ICH7 LPC Generic IO decode 2");	/* [한국어] 범용 IO 디코드 창 2(오프셋 0x88). */
	ich7_lpc_generic_decode(dev, 0x8c, "ICH7 LPC Generic IO decode 3");	/* [한국어] 범용 IO 디코드 창 3(오프셋 0x8c). */
	ich7_lpc_generic_decode(dev, 0x90, "ICH7 LPC Generic IO decode 4");	/* [한국어] 범용 IO 디코드 창 4(오프셋 0x90) - ICH7 세대에 늘어난 네 번째 창이다. */
}
/* [한국어] ICH7 부터 ICH10 까지 13종의 LPC 브리지에 HEADER 단계로 등록한다.
 * 세대가 달라도 범용 디코드 레지스터 형식이 같아 함수 하나를 공유한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH7_0, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH7_1, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH7_31, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH8_0, quirk_ich7_lpc);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_ICH8_2, PCI_DEVICE_ID_INTEL_ICH8_3,
 * PCI_DEVICE_ID_INTEL_ICH8_1, PCI_DEVICE_ID_INTEL_ICH8_4. 위 블록 주석의 설명이 그대로
 * 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH8_2, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH8_3, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH8_1, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH8_4, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH9_2, quirk_ich7_lpc);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_ICH9_4, PCI_DEVICE_ID_INTEL_ICH9_7,
 * PCI_DEVICE_ID_INTEL_ICH9_8, PCI_DEVICE_ID_INTEL_ICH10_1. 위 블록 주석의 설명이 그대로
 * 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH9_4, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH9_7, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH9_8, quirk_ich7_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,   PCI_DEVICE_ID_INTEL_ICH10_1, quirk_ich7_lpc);

/*
 * [한국어]
 * quirk_vt82c586_acpi - VIA 82C586 의 숨은 ACPI IO 영역을 리소스로 등록
 *
 * @dev: VIA 82C586 사우스브리지의 전원관리 함수
 * @return: 없음
 *
 * [어떤 하드웨어] VIA 82C586(_3 함수).
 * [무엇이 문제] 256바이트 ACPI 레지스터 블록의 IO 베이스가 표준 BAR 가
 * 아니라 벤더 전용 롱워드(0x48, 리비전에 따라서는 0x20)에만 적혀 있다.
 * [그대로 두면] 커널이 모르는 IO 영역이 남아 충돌하거나, 탐색 코드가
 * ACPI 레지스터를 잘못 건드릴 수 있다.
 * [우회] 리비전을 확인해 해당 세대일 때만 0x48 이 가리키는 256바이트를
 * 리소스 트리에 등록한다.
 *
 * [리비전 검사] dev->revision 의 비트 4 가 서 있는 리비전에서만 등록한다.
 * 그 비트가 무엇을 뜻하는지는 원본 주석에 없어 이 트리의 정보만으로는
 * 확인할 수 없다. 아래 영어 주석이 '0x48 또는 0x20' 이라고 두 오프셋을
 * 언급하는 것으로 보아 세대에 따라 위치가 달랐던 것으로 보인다.
 *
 * 실행 컨텍스트: HEADER 단계. 아래 quirk_vt82c686_acpi() 도 이 함수를 부른다.
 *
 * 호출 체인:
 *   pci_do_fixups() 또는 quirk_vt82c686_acpi() -> [quirk_vt82c586_acpi]
 *     -> quirk_io_region()
 */
/*
 * VIA ACPI: One IO region pointed to by longword at
 *	0x48 or 0x20 (256 bytes of ACPI registers)
 */
static void quirk_vt82c586_acpi(struct pci_dev *dev)
{
	if (dev->revision & 0x10)	/* [한국어] 리비전의 비트 4 가 서 있는 세대만 0x48 형식을 쓴다. */
		quirk_io_region(dev, 0x48, 256, PCI_BRIDGE_RESOURCES,	/* [한국어] 0x48 이 가리키는 256바이트 ACPI 블록을 등록한다. */
				"vt82c586 ACPI");
}
/* [한국어] VIA 82C586 의 전원관리 함수(_3)에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C586_3,	quirk_vt82c586_acpi);

/*
 * [한국어]
 * quirk_vt82c686_acpi - VIA VT82C686 의 숨은 IO 영역 3개를 리소스로 등록
 *
 * @dev: VIA VT82C686 사우스브리지의 전원관리 함수(_4)
 * @return: 없음
 *
 * [어떤 하드웨어] VIA VT82C686.
 * [무엇이 문제] 82C586 과 같은 부류인데 숨은 영역이 하나가 아니라 셋이다.
 * 아래 영어 주석대로 ACPI(0x48, 256바이트), 하드웨어 모니터링(0x70,
 * 128바이트), SMB(0x90, 16바이트)가 모두 BAR 밖에 있다.
 * [우회] 82C586 용 함수를 그대로 불러 ACPI 를 처리하고, 나머지 둘을
 * 이어서 등록한다. 세대가 겹치는 칩끼리 quirk 함수를 재사용하는 예다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_vt82c686_acpi]
 *     -> quirk_vt82c586_acpi() -> quirk_io_region()
 */
/*
 * VIA VT82C686 ACPI: Three IO region pointed to by (long)words at
 *	0x48 (256 bytes of ACPI registers)
 *	0x70 (128 bytes of hardware monitoring register)
 *	0x90 (16 bytes of SMB registers)
 */
static void quirk_vt82c686_acpi(struct pci_dev *dev)
{
	quirk_vt82c586_acpi(dev);	/* [한국어] ACPI 블록(0x48) 처리는 82C586 용 함수가 그대로 해 준다 - 형식이 같기 때문이다. */

	quirk_io_region(dev, 0x70, 128, PCI_BRIDGE_RESOURCES+1,	/* [한국어] 0x70 이 가리키는 하드웨어 모니터링 레지스터 128바이트를 등록한다. */
				 "vt82c686 HW-mon");

	quirk_io_region(dev, 0x90, 16, PCI_BRIDGE_RESOURCES+2, "vt82c686 SMB");	/* [한국어] 0x90 이 가리키는 SMB 레지스터 16바이트를 세 번째 슬롯에 등록한다. */
}
/* [한국어] VT82C686 의 전원관리 함수(_4)에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C686_4,	quirk_vt82c686_acpi);

/*
 * [한국어]
 * quirk_vt8235_acpi - VIA VT8235 ISA 브리지의 숨은 IO 영역 2개를 등록
 *
 * @dev: VIA VT8235 ISA 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] VIA VT8235 ISA 브리지.
 * [무엇이 문제] 아래 영어 주석대로 전원관리 레지스터(0x88, 128바이트)와
 * SMB 레지스터(0xd0, 16바이트)의 IO 베이스가 벤더 전용 워드에만 있다.
 * 앞의 VIA 칩들과 오프셋만 다를 뿐 같은 문제다.
 * [우회] 두 영역을 리소스 트리에 등록한다. 이 칩은 활성 비트 검사를
 * 하지 않는데, quirk_io_region() 이 베이스가 0 이면 알아서 건너뛴다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_vt8235_acpi] -> quirk_io_region()
 */
/*
 * VIA VT8235 ISA Bridge: Two IO regions pointed to by words at
 *	0x88 (128 bytes of power management registers)
 *	0xd0 (16 bytes of SMB registers)
 */
static void quirk_vt8235_acpi(struct pci_dev *dev)
{
	quirk_io_region(dev, 0x88, 128, PCI_BRIDGE_RESOURCES, "vt8235 PM");	/* [한국어] 0x88 이 가리키는 전원관리 레지스터 128바이트를 등록한다. */
	quirk_io_region(dev, 0xd0, 16, PCI_BRIDGE_RESOURCES+1, "vt8235 SMB");	/* [한국어] 0xd0 이 가리키는 SMB 레지스터 16바이트를 등록한다. */
}
/* [한국어] VT8235 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8235,	quirk_vt8235_acpi);

/*
 * [한국어]
 * quirk_xio2000a - TI XIO2000a 브리지 아래 장치들의 Fast Back-to-Back 을 끈다
 *
 * @dev: TI XIO2000A PCIe-PCI 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] TI XIO2000a PCIe-to-PCI 브리지.
 * [무엇이 문제] 아래 영어 주석대로, 이 브리지는 Fast Back-to-Back 전송을
 * 지원한다고 잘못 보고한다. Fast Back-to-Back 은 PCI 버스에서 두 전송
 * 사이의 유휴 사이클을 없애는 최적화로, 버스의 모든 대상이 지원해야만
 * 켤 수 있다. 브리지가 거짓말을 하면 커널은 그 아래 장치들에 이 기능을
 * 켜도 된다고 판단한다.
 * [그대로 두면] 하위 버스에서 전송이 깨진다.
 * [우회] 브리지의 하위(secondary) 버스에 달린 모든 장치를 순회하며
 * Command 레지스터의 Fast Back-to-Back Enable 비트를 강제로 끈다.
 *
 * [FINAL 단계인 이유] 하위 버스의 장치들이 이미 모두 열거되어
 * dev->subordinate 목록에 들어와 있어야 순회할 수 있다. 그래서 열거가
 * 끝난 뒤인 FINAL 단계에 등록되어 있다.
 *
 * 실행 컨텍스트: FINAL 단계. 이 시점에는 버스 목록이 안정되어 있어
 * 별도 락 없이 순회한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_xio2000a] -> pci_write_config_word()
 */
/*
 * TI XIO2000a PCIe-PCI Bridge erroneously reports it supports fast
 * back-to-back: Disable fast back-to-back on the secondary bus segment
 */
static void quirk_xio2000a(struct pci_dev *dev)
{
	struct pci_dev *pdev;	/* [한국어] 하위 버스에 달린 장치를 하나씩 가리킬 반복자. */
	u16 command;	/* [한국어] 각 장치의 Command 레지스터(오프셋 0x04) 값. */

	pci_warn(dev, "TI XIO2000a quirk detected; secondary bus fast back-to-back transfers disabled\n");	/* [한국어] 이 quirk 가 발동했음을 경고 수준으로 남긴다 - 성능이 떨어지는 변경이라 사용자가 알아야 한다. */
	list_for_each_entry(pdev, &dev->subordinate->devices, bus_list) {	/* [한국어] dev->subordinate 는 이 브리지가 만들어 낸 하위 버스이고, 그 devices 목록이 거기 달린 장치들이다. bus_list 가 그 연결 고리다. */
		pci_read_config_word(pdev, PCI_COMMAND, &command);	/* [한국어] 각 장치의 Command 레지스터를 읽는다. PCI_COMMAND 는 표준 헤더 오프셋 0x04 다. */
		if (command & PCI_COMMAND_FAST_BACK)	/* [한국어] Fast Back-to-Back Enable(비트 9)이 켜져 있는 장치만 손댄다. */
			pci_write_config_word(pdev, PCI_COMMAND, command & ~PCI_COMMAND_FAST_BACK);	/* [한국어] 그 비트만 지우고 나머지 Command 비트(메모리/IO 디코딩, 버스 마스터 등)는 그대로 보존한다. */
	}
}
/* [한국어] TI XIO2000A 에 FINAL 단계로 등록한다 - 하위 버스 열거가 끝난 뒤여야 한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_TI, PCI_DEVICE_ID_TI_XIO2000A,
			quirk_xio2000a);

/* [한국어] 아래 세 quirk 는 x86 의 IO-APIC 인터럽트 라우팅과 관련되어
 * 있어, IO-APIC 이 있는 빌드에서만 컴파일한다. */
#ifdef CONFIG_X86_IO_APIC

/* [한국어] nr_ioapics 전역(시스템에 있는 IO-APIC 개수)을 쓰기 위한 헤더.
 * x86 전용이라 위 #ifdef 안쪽에서만 include 한다. */
#include <asm/io_apic.h>

/*
 * [한국어]
 * quirk_via_ioapic - VIA 686A/B 의 온칩 장치 인터럽트를 외부 APIC 으로 돌린다
 *
 * @dev: VIA 82C686 사우스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] VIA 686A/686B 사우스브리지.
 * [무엇이 문제] 스펙 위반이라기보다 칩 고유의 라우팅 설정 문제다. 이 칩은
 * 온칩 장치들의 인터럽트를 내부 PIC 로 보낼지 외부 APIC 으로 보낼지를
 * 벤더 전용 레지스터 0x58 로 정하는데, 커널이 IO-APIC 을 쓰기로 했다면
 * 그 설정을 맞춰 줘야 한다. 펌웨어가 늘 맞게 설정해 주지는 않는다.
 * [그대로 두면] IO-APIC 모드에서 온칩 장치의 인터럽트가 오지 않는다.
 * [우회] 시스템에 IO-APIC 이 있으면 알려진 5비트(4~0)를 모두 세워 외부
 * APIC 라우팅을 켜고, 없으면 0 을 써서 끈다.
 *
 * [TODO 주석] 아래 영어 주석은 장치별 인터럽트 라우터가 생기면 이 코드가
 * quirks 에서 사라질 것이라고 적고 있다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY 두 단계. 재개 시 이 레지스터가
 * 초기화되므로 인터럽트를 다시 쓰기 전(이른 시점)에 복원해야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_via_ioapic] -> pci_write_config_byte()
 */
/*
 * VIA 686A/B: If an IO-APIC is active, we need to route all on-chip
 * devices to the external APIC.
 *
 * TODO: When we have device-specific interrupt routers, this code will go
 * away from quirks.
 */
static void quirk_via_ioapic(struct pci_dev *dev)
{
	u8 tmp;	/* [한국어] 0x58 에 쓸 값. 라우팅 비트 다섯 개를 담는다. */

	if (nr_ioapics < 1)	/* [한국어] nr_ioapics 는 시스템에서 발견된 IO-APIC 개수다. 하나도 없으면 외부 APIC 라우팅을 켜면 안 된다. */
		/* [한국어] 라우팅 비트를 모두 0 으로 - 인터럽트는 내부 경로로 간다. */
		tmp = 0;    /* nothing routed to external APIC */
	else
		/* [한국어] 알려진 5비트(4~0)를 모두 1 로 - 온칩 장치 인터럽트를 외부 APIC 으로 보낸다. */
		tmp = 0x1f; /* all known bits (4-0) routed to external APIC */

	pci_info(dev, "%s VIA external APIC routing\n",	/* [한국어] 어느 쪽으로 설정했는지 남긴다. */
		 tmp ? "Enabling" : "Disabling");	/* [한국어] tmp 가 0 이 아니면 Enabling, 0 이면 Disabling 으로 문자열을 고른다. */

	/* Offset 0x58: External APIC IRQ output control */
	pci_write_config_byte(dev, 0x58, tmp);	/* [한국어] 위 영어 주석대로 0x58 이 외부 APIC IRQ 출력 제어 레지스터다. 벤더 전용 오프셋이다. */
}
/* [한국어] VIA 82C686 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C686,	quirk_via_ioapic);
/* [한국어] 재개 시에도 다시 적용한다. RESUME_EARLY 인 이유는 인터럽트를
 * 다시 쓰기 시작하기 전에 라우팅이 복원되어 있어야 하기 때문이다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C686,	quirk_via_ioapic);

/*
 * [한국어]
 * quirk_via_vt8237_bypass_apic_deassert - VT8237 의 APIC De-Assert 메시지를 우회 설정
 *
 * @dev: VIA VT8237 사우스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] VIA VT8237.
 * [무엇이 문제] 아래 영어 주석대로 일부 BIOS 가 'Bypass APIC De-Assert
 * Message' 비트를 세우지 않는다. 그러면 레벨 트리거 인터럽트마다 De-Assert
 * 메시지가 한 번 더 오가 인터럽트 처리 횟수가 두 배가 된다.
 * [그대로 두면] 레벨 인터럽트 비율이 두 배가 되어 CPU 사이클을 낭비한다.
 * 영어 주석은 그 밖에는 치명적이지 않다(uncritical)고 덧붙인다.
 * [우회] 벤더 전용 레지스터 0x5B 의 비트 3 을 세운다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY. 재개 시 다시 세워야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_via_vt8237_bypass_apic_deassert]
 *     -> pci_read_config_byte() -> pci_write_config_byte()
 */
/*
 * VIA 8237: Some BIOSes don't set the 'Bypass APIC De-Assert Message' Bit.
 * This leads to doubled level interrupt rates.
 * Set this bit to get rid of cycle wastage.
 * Otherwise uncritical.
 */
static void quirk_via_vt8237_bypass_apic_deassert(struct pci_dev *dev)
{
	u8 misc_control2;	/* [한국어] 0x5B(Miscellaneous Control 2) 레지스터 값. */
/* [한국어] 그 레지스터의 비트 3(값 8)이 Bypass APIC De-Assert Message 다.
 * 함수 안에서 #define 을 하는 드문 형태인데, 이 한 곳에서만 쓰이기 때문이다. */
#define BYPASS_APIC_DEASSERT 8

	pci_read_config_byte(dev, 0x5B, &misc_control2);	/* [한국어] 0x5B 를 읽는다 - 벤더 전용 오프셋이다. */
	if (!(misc_control2 & BYPASS_APIC_DEASSERT)) {	/* [한국어] 비트가 이미 서 있으면 BIOS 가 제대로 설정한 것이므로 손대지 않는다. */
		pci_info(dev, "Bypassing VIA 8237 APIC De-Assert Message\n");	/* [한국어] 무엇을 고쳤는지 남긴다. */
		pci_write_config_byte(dev, 0x5B, misc_control2|BYPASS_APIC_DEASSERT);	/* [한국어] 그 비트만 OR 로 추가해 되쓴다 - 나머지 제어 비트는 보존한다. */
	}
}
/* [한국어] VT8237 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8237,		quirk_via_vt8237_bypass_apic_deassert);
/* [한국어] 재개 시에도 다시 세운다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8237,		quirk_via_vt8237_bypass_apic_deassert);

/*
 * [한국어]
 * quirk_amd_ioapic - AMD Viper 7410 의 IO-APIC 에라타(#22)를 사용자에게 경고
 *
 * @dev: AMD Viper 7410 사우스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] AMD Viper 7410, 리비전 0x02(B0) 이상.
 * [무엇이 문제] 아래 영어 주석대로 APIC IRQ 를 마스크하면 시스템이 멈출 수
 * 있다. AMD 는 이 에라타를 NoFix(고치지 않음)로 표시했다.
 * [그대로 두면] 인터럽트 마스킹이 일어나는 시점에 기계가 멈춘다.
 * [우회] 커널이 자동으로 고칠 수 있는 방법이 없다. 그래서 이 quirk 는
 * 아무것도 고치지 않고 경고만 남긴다 - 불안정하면 noapic 부팅 옵션을
 * 쓰라는 안내다. 영어 주석은 '이 칩셋에서 멈춤이 여러 번 보고되었고
 * noapic 을 주면 사라졌다. 지금은 이 에라타 때문이라고 가정한다. 틀릴
 * 수도 있지만 조언 자체는 유효하다'고 솔직히 적고 있다.
 *
 * [이 파일에서 드문 형태] 대부분의 quirk 는 하드웨어나 커널 상태를 고치는데,
 * 이것은 순수하게 진단 메시지만 내는 quirk 다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_amd_ioapic] -> pci_warn()
 */
/*
 * The AMD IO-APIC can hang the box when an APIC IRQ is masked.
 * We check all revs >= B0 (yet not in the pre production!) as the bug
 * is currently marked NoFix
 *
 * We have multiple reports of hangs with this chipset that went away with
 * noapic specified. For the moment we assume it's the erratum. We may be wrong
 * of course. However the advice is demonstrably good even if so.
 */
static void quirk_amd_ioapic(struct pci_dev *dev)
{
	if (dev->revision >= 0x02) {	/* [한국어] 리비전 0x02(B0) 이상만 대상이다. 위 영어 주석대로 그 이전의 시제품 실리콘은 제외한다. */
		pci_warn(dev, "I/O APIC: AMD Erratum #22 may be present. In the event of instability try\n");	/* [한국어] 에라타 가능성을 경고한다. 고치는 것이 아니라 알리는 것이 목적이다. */
		pci_warn(dev, "        : booting with the \"noapic\" option\n");	/* [한국어] 회피 방법(noapic 부팅 옵션)을 이어서 안내한다. */
	}
}
/* [한국어] AMD Viper 7410 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_VIPER_7410,	quirk_amd_ioapic);
/* [한국어] CONFIG_X86_IO_APIC 블록의 끝. */
#endif /* CONFIG_X86_IO_APIC */

/* [한국어] 아래 quirk 는 ARM64 에서 SR-IOV 와 ATS(Address Translation
 * Services)를 함께 쓰는 Cavium 칩 전용이라 두 조건이 모두 켜진 빌드에서만
 * 컴파일한다. */
#if defined(CONFIG_ARM64) && defined(CONFIG_PCI_ATS)

/*
 * [한국어]
 * quirk_cavium_sriov_rnm_link - Cavium cn88xx RNM 장치의 SR-IOV link 값을 바로잡는다
 *
 * @dev: Cavium device 0xa018 중 서브시스템 ID 가 0xa118 인 것
 * @return: 없음
 *
 * [어떤 하드웨어] Cavium cn88xx 의 RNM(난수 생성) 장치.
 * [무엇이 문제] 아래 영어 주석대로 SR-IOV 설정이 잘못되어 있다. SR-IOV
 * capability 의 필드로부터 커널이 계산한 sriov->link 값이 실제와 맞지 않는
 * 것으로 보인다(구체적으로 어떤 필드가 틀렸는지는 원본 주석에 없어 이
 * 트리의 정보만으로는 확인할 수 없다).
 * [그대로 두면] 가상 함수(VF)의 라우팅 ID 계산이 어긋나 ATS/IOMMU 매핑이
 * 엉뚱한 장치를 가리킬 수 있다.
 * [우회] sriov->link 를 물리 함수 자신의 devfn 으로 덮어쓴다.
 *
 * [서브시스템 ID 로 좁히는 이유] DECLARE 매크로는 vendor/device 만 볼 수
 * 있으므로, 같은 device ID 를 쓰는 다른 변종을 건드리지 않도록 함수 안에서
 * subsystem_device 를 한 번 더 확인한다.
 *
 * 실행 컨텍스트: FINAL 단계. SR-IOV capability 파싱이 끝나 dev->sriov 가
 * 만들어진 뒤여야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_cavium_sriov_rnm_link]
 */
static void quirk_cavium_sriov_rnm_link(struct pci_dev *dev)
{
	/* Fix for improper SR-IOV configuration on Cavium cn88xx RNM device */
	if (dev->subsystem_device == 0xa118)	/* [한국어] 서브시스템 device ID 0xa118 인 변종만 대상이다. 위 영어 주석이 말하는 'cn88xx RNM 장치'가 이것이다. */
		dev->sriov->link = dev->devfn;	/* [한국어] SR-IOV 의 link 값을 물리 함수 자신의 devfn 으로 맞춘다. dev->sriov 는 SR-IOV capability 를 파싱할 때 만들어지는 구조체다. */
}
/* [한국어] Cavium device 0xa018 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_CAVIUM, 0xa018, quirk_cavium_sriov_rnm_link);
/* [한국어] ARM64 + PCI_ATS 조건부 블록의 끝. */
#endif

/*
 * [한국어]
 * quirk_amd_8131_mmrbc - AMD 8131 브리지 하위 버스에서 PCI-X MMRBC 변경을 금지
 *
 * @dev: AMD 8131 HyperTransport PCI-X 터널 브리지, 리비전 0x12 이하
 * @return: 없음
 *
 * [어떤 하드웨어] AMD 8131 HyperTransport PCI-X 터널, 리비전 0x12 이하.
 * [무엇이 문제] 아래 영어 주석대로 MMRBC(Maximum Memory Read Byte Count)를
 * 어떤 값으로 설정하면 데이터가 손상된다. MMRBC 는 PCI-X 에서 한 번의 메모리
 * 읽기로 가져올 최대 바이트 수를 정하는 값으로, 크게 잡을수록 성능이 좋다.
 * 근거는 'AMD 8131 HyperTransport PCI-X Tunnel Revision Guide' 다.
 * [그대로 두면] 성능을 위해 MMRBC 를 올리는 드라이버가 데이터를 깨뜨린다.
 * [우회] 이 브리지가 만든 하위 버스에 PCI_BUS_FLAGS_NO_MMRBC 를 세워,
 * 그 버스에 달린 장치들의 MMRBC 변경 자체를 막는다. 개별 장치가 아니라
 * 버스 단위로 금지하는 것이 이 quirk 의 특징이다.
 *
 * 실행 컨텍스트: FINAL 단계. 하위 버스(dev->subordinate)가 만들어진 뒤여야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_amd_8131_mmrbc]
 */
/*
 * Some settings of MMRBC can lead to data corruption so block changes.
 * See AMD 8131 HyperTransport PCI-X Tunnel Revision Guide
 */
static void quirk_amd_8131_mmrbc(struct pci_dev *dev)
{
	if (dev->subordinate && dev->revision <= 0x12) {	/* [한국어] 하위 버스가 실제로 있고(브리지로 동작 중이고) 리비전이 0x12 이하인 것만 대상이다. 이후 리비전에서는 고쳐졌다는 뜻이다. */
		pci_info(dev, "AMD8131 rev %x detected; disabling PCI-X MMRBC\n",	/* [한국어] 어떤 리비전에서 무엇을 껐는지 남긴다. */
			 dev->revision);
		dev->subordinate->bus_flags |= PCI_BUS_FLAGS_NO_MMRBC;	/* [한국어] 하위 버스 전체에 '이 버스에서는 MMRBC 를 바꾸지 말라'는 플래그를 세운다. PCI-X 드라이버가 이 플래그를 보고 설정을 포기한다. */
	}
}
/* [한국어] AMD 8131 브리지에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_8131_BRIDGE, quirk_amd_8131_mmrbc);

/*
 * [한국어]
 * quirk_via_acpi - VIA ACPI 장치의 IRQ 번호를 벤더 레지스터에서 읽어 채운다
 *
 * @d: VIA 82C586_3 또는 82C686_4 (ACPI/전원관리 함수)
 * @return: 없음
 *
 * [어떤 하드웨어] VIA 82C586 / 82C686 의 ACPI 함수.
 * [무엇이 문제] 이 장치는 ISA 브리지로 보이고 표준 헤더의
 * PCI_INTERRUPT_LINE(오프셋 0x3c) 레지스터를 아예 지원하지 않는다. 대신
 * ACPI SCI 인터럽트 번호가 벤더 전용 바이트 0x42 의 하위 4비트에 들어 있다.
 * [커널이 하는 일] 그 값을 읽어 struct pci_dev 의 irq 필드에 넣어 준다.
 *
 * [원저자의 의문] 아래 영어 주석에서 jgarzik 은 이 quirk 가 정말 필요한지
 * 의문을 제기한다. 표준 IRQ 레지스터를 지원하지 않는 장치이므로, pci_dev 의
 * irq 를 ACPI SCI 값으로 채우는 것은 편의를 위한 것일 뿐이라는 지적이다.
 *
 * 실행 컨텍스트: HEADER 단계. 드라이버가 dev->irq 를 보기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_via_acpi] -> pci_read_config_byte()
 */
/*
 * FIXME: it is questionable that quirk_via_acpi() is needed.  It shows up
 * as an ISA bridge, and does not support the PCI_INTERRUPT_LINE register
 * at all.  Therefore it seems like setting the pci_dev's IRQ to the value
 * of the ACPI SCI interrupt is only done for convenience.
 *	-jgarzik
 */
static void quirk_via_acpi(struct pci_dev *d)
{
	u8 irq;	/* [한국어] 0x42 에서 읽은 SCI IRQ 번호. */

	/* VIA ACPI device: SCI IRQ line in PCI config byte 0x42 */
	pci_read_config_byte(d, 0x42, &irq);	/* [한국어] 위 영어 주석대로 벤더 전용 바이트 0x42 에 SCI IRQ 선 번호가 들어 있다. */
	irq &= 0xf;	/* [한국어] 하위 4비트만 IRQ 번호다(0~15 의 레거시 IRQ 범위). 상위 비트는 다른 용도다. */
	if (irq && (irq != 2))	/* [한국어] 0 은 '배정 없음', 2 는 x86 에서 캐스케이드용으로 예약된 번호라 둘 다 유효한 IRQ 가 아니다. */
		d->irq = irq;	/* [한국어] 유효할 때만 커널이 기억하는 IRQ 번호를 덮어쓴다. 하드웨어는 건드리지 않는다. */
}
/* [한국어] VIA 82C586 과 82C686 의 ACPI 함수에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C586_3,	quirk_via_acpi);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C686_4,	quirk_via_acpi);

/* [한국어] VLink 를 쓰는 VIA 사우스브리지가 발견되면 그 온보드 장치가
 * 놓이는 슬롯 번호 범위를 여기 기록해 둔다.
 * 설정자: 아래 quirk_via_bridge() 가 사우스브리지 종류에 따라 채운다.
 * 읽는 자: quirk_via_vlink() 가 '이 장치가 온보드 VLink 장치인가'를 판정할 때.
 * 값 범위: lo 는 -1(아직 VLink 사우스브리지를 못 봄) 또는 슬롯 번호,
 *          hi 는 기본 18 이며 82C686 인 경우에만 lo 와 같은 값으로 좁혀진다.
 * 동기화: 부팅 중 열거 문맥에서만 쓰이고 사우스브리지는 하나뿐이라
 *          별도 락이 없다. */
/* VIA bridges which have VLink */
static int via_vlink_dev_lo = -1, via_vlink_dev_hi = 18;

/*
 * [한국어]
 * quirk_via_bridge - 발견한 VIA 사우스브리지 종류에 맞춰 VLink 슬롯 범위를 기록
 *
 * @dev: VIA 사우스브리지(82C686, 8231, 8233 계열, 8235, 8237 계열)
 * @return: 없음
 *
 * [무엇을 하는가] 아래 quirk_via_vlink() 가 'IRQ 를 고쳐 줘야 하는 온보드
 * 장치'를 가려내려면, 그 보드의 사우스브리지가 온보드 장치를 어느 슬롯
 * 번호 대역에 놓는지 알아야 한다. 그 대역은 사우스브리지 모델마다 다르다.
 * 이 함수는 사우스브리지를 만났을 때 그 대역을 전역 변수에 적어 둔다.
 *
 * [82C686 이 특별한 이유] 아래 영어 주석대로 VT82C686 은 PCI 에 직접 붙어
 * 임의의 디바이스 번호를 가질 수 있고, 그 하위 장치들은 모두 같은 디바이스의
 * 함수들이다. 그래서 범위를 그 장치의 슬롯 하나로 좁힌다.
 *
 * 실행 컨텍스트: HEADER 단계. quirk_via_vlink() 는 ENABLE 단계라 항상
 * 이 함수보다 나중에 돌아 값이 준비되어 있다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_via_bridge]
 */
static void quirk_via_bridge(struct pci_dev *dev)
{
	/* See what bridge we have and find the device ranges */
	switch (dev->device) {	/* [한국어] 위 영어 주석대로 어떤 브리지인지 보고 장치 번호 범위를 정한다. */
	case PCI_DEVICE_ID_VIA_82C686:	/* [한국어] VT82C686 - 아래 영어 주석이 설명하는 특수 사례. */
		/*
		 * The VT82C686 is special; it attaches to PCI and can have
		 * any device number. All its subdevices are functions of
		 * that single device.
		 */
		via_vlink_dev_lo = PCI_SLOT(dev->devfn);	/* [한국어] 하한을 이 장치가 실제로 꽂힌 슬롯 번호로 잡는다. PCI_SLOT() 은 devfn 에서 상위 5비트(디바이스 번호)를 뽑는 매크로다. */
		via_vlink_dev_hi = PCI_SLOT(dev->devfn);	/* [한국어] 상한도 같은 값으로 좁힌다 - 모든 하위 장치가 이 한 디바이스의 함수들이기 때문이다. */
		break;
	case PCI_DEVICE_ID_VIA_8237:	/* [한국어] VT8237. */
	case PCI_DEVICE_ID_VIA_8237A:	/* [한국어] VT8237A - 8237 과 같은 배치라 case 를 이어 붙였다. */
		via_vlink_dev_lo = 15;	/* [한국어] 이 세대는 온보드 장치가 슬롯 15 이상에 놓인다. 상한은 기본값 18 을 그대로 쓴다. */
		break;
	case PCI_DEVICE_ID_VIA_8235:	/* [한국어] VT8235. */
		via_vlink_dev_lo = 16;	/* [한국어] 이 세대는 슬롯 16 이상. */
		break;
	case PCI_DEVICE_ID_VIA_8231:	/* [한국어] VT8231. */
	case PCI_DEVICE_ID_VIA_8233_0:	/* [한국어] VT8233. */
	case PCI_DEVICE_ID_VIA_8233A:	/* [한국어] VT8233A. */
	case PCI_DEVICE_ID_VIA_8233C_0:	/* [한국어] VT8233C - 네 모델이 같은 배치를 쓴다. */
		via_vlink_dev_lo = 17;	/* [한국어] 이 세대는 슬롯 17 이상. */
		break;
	}
}
/* [한국어] VLink 를 쓰는 VIA 사우스브리지 8종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C686,	quirk_via_bridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8231,		quirk_via_bridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8233_0,	quirk_via_bridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8233A,	quirk_via_bridge);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_VIA_8233C_0, PCI_DEVICE_ID_VIA_8235,
 * PCI_DEVICE_ID_VIA_8237, PCI_DEVICE_ID_VIA_8237A. 위 블록 주석의 설명이 그대로 적용되며 대상 ID
 * 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8233C_0,	quirk_via_bridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8235,		quirk_via_bridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8237,		quirk_via_bridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8237A,	quirk_via_bridge);

/*
 * [한국어]
 * quirk_via_vlink - VIA VLink 온보드 장치의 IRQ Line 레지스터를 실제로 써 준다
 *
 * @dev: VIA 벤더의 임의 장치(ENABLE 단계에서 모두 검사된다)
 * @return: 없음
 *
 * [어떤 하드웨어] VIA VLink 사우스브리지가 있는 보드의 온보드 VIA 장치.
 * [무엇이 문제] PCI 스펙에서 PCI_INTERRUPT_LINE(오프셋 0x3c)은 소프트웨어가
 * 기록해 두는 정보일 뿐 하드웨어 동작에 영향을 주지 않는 것이 원칙이다.
 * 그런데 VLink 로 붙은 온보드 장치는 이 레지스터의 값에 따라 인터럽트가
 * 실제로 어디로 가는지가 달라진다. 아래 영어 주석대로 BIOS 가 이 값을
 * 제대로 써 뒀어야 하지만 그러지 않는 경우가 있다.
 * [그대로 두면] 인터럽트가 엉뚱한 곳으로 가 장치가 동작하지 않는다.
 * [우회] 커널이 정한 IRQ 번호(dev->irq)를 IRQ Line 레지스터에 다시 써 준다.
 *
 * [적용 범위를 어떻게 좁히는가] 세 겹으로 거른다. (1) VLink 사우스브리지가
 * 발견된 보드인가, (2) 레거시 PIC IRQ 범위(1~15)인가, (3) 버스 0 의
 * quirk_via_bridge() 가 기록한 슬롯 범위 안에 있는 온보드 장치인가.
 *
 * 실행 컨텍스트: ENABLE 단계. 장치를 켤 때마다 검사하며, 이때는 커널이
 * dev->irq 를 이미 확정해 둔 상태다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_via_vlink] -> pci_write_config_byte()
 */
/*
 * quirk_via_vlink		-	VIA VLink IRQ number update
 * @dev: PCI device
 *
 * If the device we are dealing with is on a PIC IRQ we need to ensure that
 * the IRQ line register which usually is not relevant for PCI cards, is
 * actually written so that interrupts get sent to the right place.
 *
 * We only do this on systems where a VIA south bridge was detected, and
 * only for VIA devices on the motherboard (see quirk_via_bridge above).
 */
static void quirk_via_vlink(struct pci_dev *dev)
{
	u8 irq, new_irq;	/* [한국어] irq: 레지스터에 현재 적힌 값, new_irq: 커널이 정한 값. */

	/* Check if we have VLink at all */
	if (via_vlink_dev_lo == -1)	/* [한국어] 위 영어 주석대로, VLink 사우스브리지를 만난 적이 없으면(-1) 이 보드는 대상이 아니다. */
		return;

	new_irq = dev->irq;	/* [한국어] 커널이 이 장치에 배정한 IRQ 번호. */

	/* Don't quirk interrupts outside the legacy IRQ range */
	if (!new_irq || new_irq > 15)	/* [한국어] 위 영어 주석대로 레거시 PIC IRQ 범위(1~15) 밖이면 건드리지 않는다. 0 은 배정 없음이고 16 이상은 APIC 영역이라 이 레지스터와 무관하다. */
		return;

	/* Internal device ? */
	if (dev->bus->number != 0 || PCI_SLOT(dev->devfn) > via_vlink_dev_hi ||	/* [한국어] 버스 0(온보드)이면서 quirk_via_bridge() 가 기록한 슬롯 범위 안에 있어야 온보드 VLink 장치다. */
	    PCI_SLOT(dev->devfn) < via_vlink_dev_lo)	/* [한국어] 범위의 하한 검사. 확장 슬롯에 꽂힌 카드는 여기서 걸러진다. */
		return;

	/*
	 * This is an internal VLink device on a PIC interrupt. The BIOS
	 * ought to have set this but may not have, so we redo it.
	 */
	pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &irq);	/* [한국어] 현재 IRQ Line 레지스터에 무엇이 적혀 있는지 읽는다. */
	if (new_irq != irq) {	/* [한국어] 커널이 정한 값과 다를 때만 고친다. */
		pci_info(dev, "VIA VLink IRQ fixup, from %d to %d\n",	/* [한국어] 무엇을 무엇으로 바꾸는지 남긴다. */
			irq, new_irq);
		/* [한국어] 15 마이크로초 대기. 옆의 영어 주석이 '정말 필요한지 모른다'고
		 * 스스로 밝히고 있다 - 근거 없이 넣은 보수적 지연이다. */
		udelay(15);	/* unknown if delay really needed */
		pci_write_config_byte(dev, PCI_INTERRUPT_LINE, new_irq);	/* [한국어] 커널이 정한 IRQ 번호를 레지스터에 써 넣는다. VLink 장치에서는 이 쓰기가 실제 라우팅에 영향을 준다. */
	}
}
/* [한국어] VIA 벤더의 모든 장치를 ENABLE 단계에서 검사한다. device 가
 * PCI_ANY_ID 인 것은 어떤 VIA 장치가 온보드인지 미리 알 수 없어 함수 안에서
 * 걸러 내기 때문이다. */
DECLARE_PCI_FIXUP_ENABLE(PCI_VENDOR_ID_VIA, PCI_ANY_ID, quirk_via_vlink);

/*
 * [한국어]
 * quirk_vt82c598_id - VT82C598 이 VT82C597 인 척하는 것을 되돌린다
 *
 * @dev: VT82C597 로 보고된 VIA 노스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] VIA VT82C598.
 * [무엇이 문제] 아래 영어 주석대로 이 칩은 자신의 device ID 를 소프트웨어로
 * 바꿀 수 있고, 많은 BIOS 가 하위 호환을 위해 VT82C597 의 ID 로 설정해 둔다.
 * 즉 Configuration Space 의 Device ID 필드가 실제 칩과 다른 값을 보고한다.
 * [그대로 두면] 커널이 이 칩을 597 로 오인해, 598 에 필요한 다른 quirk 나
 * 드라이버 설정이 적용되지 않는다.
 * [우회] 벤더 전용 레지스터 0xfc 에 0 을 써서 ID 위장을 끈 뒤, Device ID 를
 * 다시 읽어 dev->device 를 진짜 값으로 갱신한다.
 *
 * [주의] 이 quirk 는 커널의 기억뿐 아니라 하드웨어 레지스터도 바꾼다.
 * 그래서 이후 다른 코드가 config space 를 읽어도 진짜 ID 가 보인다.
 *
 * 실행 컨텍스트: HEADER 단계. 이 단계에서 ID 를 바로잡아야 이후의 다른
 * quirk 매칭과 드라이버 매칭이 올바른 ID 로 이뤄진다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_vt82c598_id] -> pci_write_config_byte()
 */
/*
 * VIA VT82C598 has its device ID settable and many BIOSes set it to the ID
 * of VT82C597 for backward compatibility.  We need to switch it off to be
 * able to recognize the real type of the chip.
 */
static void quirk_vt82c598_id(struct pci_dev *dev)
{
	pci_write_config_byte(dev, 0xfc, 0);	/* [한국어] 벤더 전용 레지스터 0xfc 에 0 을 써서 하위 호환 ID 위장을 끈다. 이 쓰기 이후 Device ID 필드가 진짜 값으로 바뀐다. */
	pci_read_config_word(dev, PCI_DEVICE_ID, &dev->device);	/* [한국어] 표준 헤더의 Device ID(오프셋 0x02)를 다시 읽어 dev->device 를 갱신한다. 이 필드가 이후의 quirk 매칭과 드라이버 매칭에 쓰인다. */
}
/* [한국어] 597 로 보고된 장치를 HEADER 단계에서 검사한다. 실제로 598 이면
 * 이 quirk 가 ID 를 바로잡고, 진짜 597 이면 0xfc 쓰기가 무해하게 끝난다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_82C597_0,	quirk_vt82c598_id);

/*
 * [한국어]
 * quirk_cardbus_legacy - CardBus 컨트롤러의 i82365 레거시 흉내를 끈다
 *
 * @dev: 클래스 코드가 CardBus 브리지인 모든 장치
 * @return: 없음
 *
 * [어떤 하드웨어] 모든 CardBus 컨트롤러(벤더 무관, 클래스로 매칭).
 * [무엇이 문제] 스펙 위반은 아니지만 위험한 기능이다. 아래 영어 주석대로
 * CardBus 컨트롤러에는 자신을 옛 i82365 PCMCIA 컨트롤러처럼 IO 주소로
 * 응답하게 만드는 레거시 베이스 주소가 있다.
 * [그대로 두면] 리눅스 i82365 드라이버가 이 장치를 PCMCIA 컨트롤러로
 * 잡아 버린다. 그 드라이버는 CardBus 를 다루지 못하고, 다뤄서도 안 된다.
 * CardBus 드라이버가 로드되지 않은 상태에서도 이 위험이 있다.
 * [우회] 레거시 모드 베이스 주소 레지스터에 0 을 써서 그 기능을 끈다.
 *
 * 실행 컨텍스트: FINAL 단계, 클래스 코드로 매칭(class_shift 8 - 베이스와
 * 서브클래스만 비교).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_cardbus_legacy] -> pci_write_config_dword()
 */
/*
 * CardBus controllers have a legacy base address that enables them to
 * respond as i82365 pcmcia controllers.  We don't want them to do this
 * even if the Linux CardBus driver is not loaded, because the Linux i82365
 * driver does not (and should not) handle CardBus.
 */
static void quirk_cardbus_legacy(struct pci_dev *dev)
{
	pci_write_config_dword(dev, PCI_CB_LEGACY_MODE_BASE, 0);	/* [한국어] PCI_CB_LEGACY_MODE_BASE 는 CardBus 브리지 헤더(헤더 타입 2)의 레거시 모드 베이스 주소 레지스터다. 0 을 쓰면 레거시 IO 응답이 꺼진다. */
}
/* [한국어] 벤더/디바이스를 가리지 않고 클래스가 CardBus 브리지인 모든
 * 장치에 FINAL 단계로 등록한다. class_shift 8 은 클래스 코드의 하위
 * 8비트(프로그래밍 인터페이스)를 무시하고 비교한다는 뜻이다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_ANY_ID, PCI_ANY_ID,
			PCI_CLASS_BRIDGE_CARDBUS, 8, quirk_cardbus_legacy);
/* [한국어] 재개 시에도 다시 끈다. 절전에서 깨어나면 레거시 모드가 다시
 * 켜져 있을 수 있기 때문이다. CLASS 계열 매크로의 RESUME_EARLY 판이다. */
DECLARE_PCI_FIXUP_CLASS_RESUME_EARLY(PCI_ANY_ID, PCI_ANY_ID,
			PCI_CLASS_BRIDGE_CARDBUS, 8, quirk_cardbus_legacy);

/*
 * [한국어]
 * quirk_amd_ordering - AMD 762 에서 BIOS 가 꺼 둔 PCI 순서 규칙 준수를 다시 켠다
 *
 * @dev: AMD FE Gate 700C(AMD 762) 노스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] AMD 762 칩셋.
 * [무엇이 문제] PCI 스펙은 트랜잭션의 순서 규칙(ordering rules)을 정하고
 * 있고, 드라이버는 그 규칙을 전제로 쓰기 후 읽기로 플러시를 보장한다.
 * 그런데 이 칩은 그 준수 여부를 설정으로 끌 수 있게 만들어 두었다. 아래
 * 영어 주석은 설계자가 무슨 생각이었는지 모르겠다고 비꼬면서도, AMD 를
 * 위해 공정하게 말하자면 기본값은 스펙 준수이고 그것을 꺼 버리는 것은
 * BIOS 쪽이라고 덧붙인다.
 * [그대로 두면] 쓰기가 순서대로 도달한다고 믿는 드라이버가 오동작한다.
 * [우회] 벤더 전용 레지스터 0x4C 의 비트 1,2 를 켜서 스펙 준수 모드로
 * 되돌리고, 그 모드에서 함께 요구되는 0x84 의 비트 23 도 켠다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY. 재개 후 BIOS 가 다시 꺼 놓을 수
 * 있으므로 두 단계 모두에 등록되어 있다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_amd_ordering] -> pci_write_config_dword()
 */
/*
 * Following the PCI ordering rules is optional on the AMD762. I'm not sure
 * what the designers were smoking but let's not inhale...
 *
 * To be fair to AMD, it follows the spec by default, it's BIOS people who
 * turn it off!
 */
static void quirk_amd_ordering(struct pci_dev *dev)
{
	u32 pcic;	/* [한국어] 벤더 전용 레지스터 값을 담는 임시 변수. 0x4C 와 0x84 에 재사용된다. */
	pci_read_config_dword(dev, 0x4C, &pcic);	/* [한국어] 0x4C 를 읽는다 - PCI 순서 규칙 준수 설정이 여기 있다. */
	if ((pcic & 6) != 6) {	/* [한국어] 비트 1 과 2(값 6)가 '둘 다' 서 있어야 스펙 준수 모드다. 하나라도 빠지면 BIOS 가 꺼 놓은 것이다. */
		pcic |= 6;	/* [한국어] 두 비트를 모두 세운다. */
		pci_warn(dev, "BIOS failed to enable PCI standards compliance; fixing this error\n");	/* [한국어] BIOS 잘못임을 경고 수준으로 알리고 고친다고 남긴다. */
		pci_write_config_dword(dev, 0x4C, pcic);	/* [한국어] 고친 값을 되쓴다. */
		pci_read_config_dword(dev, 0x84, &pcic);	/* [한국어] 이어서 0x84 를 읽는다. */
		/* [한국어] 옆의 영어 주석대로 이 비트는 스펙 준수 모드에서 반드시
		 * 함께 켜져 있어야 한다. 무엇을 제어하는 비트인지는 원본 주석에 없어
		 * 이 트리의 정보만으로는 확인할 수 없다. */
		pcic |= (1 << 23);	/* Required in this mode */
		pci_write_config_dword(dev, 0x84, pcic);	/* [한국어] 0x84 도 되쓴다. */
	}
}
/* [한국어] AMD 762 노스브리지에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_FE_GATE_700C, quirk_amd_ordering);
/* [한국어] 재개 시에도 다시 켠다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_FE_GATE_700C, quirk_amd_ordering);

/*
 * [한국어]
 * quirk_dunord - Dunord I-3000 이 남의 주소에 응답하지 못하도록 넓게 예약한다
 *
 * @dev: Dunord I-3000 카드
 * @return: 없음
 *
 * [어떤 하드웨어] Dunord I-3000.
 * [무엇이 문제] 아래 영어 주석대로 이 카드는 자신에게 배정되지 않은 주소에도
 * 응답한다(디코딩한다). BAR 로 광고한 범위 밖의 주소를 가로채는 것이므로
 * 명백한 스펙 위반이다. 회피책은 DreamWorks 가 제공했다.
 * [그대로 두면] 이 카드 근처 주소에 다른 장치를 배정하면 그 장치로 가야 할
 * 트랜잭션을 이 카드가 가로채 오동작한다.
 * [우회] 고치는 방법이 없으므로, BAR1 리소스를 실제보다 훨씬 큰 16MB 로
 * 부풀려 잡는다. 그러면 커널이 그 16MB 안에는 다른 장치를 배정하지 않아,
 * '너무 가까운 곳'에 아무것도 놓이지 않게 된다. 완충 지대를 만드는 방식이다.
 *
 * 실행 컨텍스트: HEADER 단계(리소스 할당 전이어야 새 크기가 반영된다).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_dunord] -> resource_set_range()
 */
/*
 * DreamWorks-provided workaround for Dunord I-3000 problem
 *
 * This card decodes and responds to addresses not apparently assigned to
 * it.  We force a larger allocation to ensure that nothing gets put too
 * close to it.
 */
static void quirk_dunord(struct pci_dev *dev)
{
	struct resource *r = &dev->resource[1];	/* [한국어] 문제가 되는 것은 BAR1 이다. dev->resource[1] 이 그 리소스다. */

	r->flags |= IORESOURCE_UNSET;	/* [한국어] IORESOURCE_UNSET 을 세워 '주소 미정'으로 만들어야 이후 할당 단계가 새 크기에 맞는 자리를 다시 찾는다. */
	resource_set_range(r, 0, SZ_16M);	/* [한국어] 크기를 16MB 로 부풀린다. 실제 필요한 크기가 아니라 완충 지대를 포함한 크기다. */
}
/* [한국어] Dunord I-3000 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_DUNORD,	PCI_DEVICE_ID_DUNORD_I3000,	quirk_dunord);

/*
 * [한국어]
 * quirk_transparent_bridge - ProgIf 를 잘못 보고하는 브리지를 투명 브리지로 표시
 *
 * @dev: Intel 82380FB 또는 Toshiba 0x605 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel i82380FB 모바일 도킹 컨트롤러의 PCI-to-PCI 브리지,
 * 그리고 Toshiba device 0x605.
 * [무엇이 문제] 아래 영어 주석대로, 이 브리지는 subtractive decoding
 * (투명 브리지)로 동작하며 그 사실을 클래스 코드의 ProgIf 필드로 알리기는
 * 한다. 그런데 값이 틀렸다 - 스펙이 정한 0x01 대신 0x80 을 보고한다.
 * [투명 브리지란] 하위 버스의 창(window)에 속하지 않는 주소도 아래로
 * 내려보내는 브리지다. 커널이 이를 알아야 브리지 창을 계산할 때 예외로
 * 다룰 수 있다.
 * [그대로 두면] 커널이 이 브리지를 일반 브리지로 보고 창을 좁게 잡아,
 * 그 아래 장치가 접근되지 않는다.
 * [우회] dev->transparent 를 1 로 세워 커널이 투명 브리지로 다루게 한다.
 *
 * 실행 컨텍스트: HEADER 단계. 브리지 창 계산 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_transparent_bridge]
 */
/*
 * i82380FB mobile docking controller: its PCI-to-PCI bridge is subtractive
 * decoding (transparent), and does indicate this in the ProgIf.
 * Unfortunately, the ProgIf value is wrong - 0x80 instead of 0x01.
 */
static void quirk_transparent_bridge(struct pci_dev *dev)
{
	dev->transparent = 1;	/* [한국어] PCI 코어가 브리지 창을 계산할 때 참조하는 플래그다. 하드웨어는 건드리지 않는다. */
}
/* [한국어] 같은 증상을 보이는 두 브리지에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82380FB,	quirk_transparent_bridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_TOSHIBA,	0x605,	quirk_transparent_bridge);

/*
 * [한국어]
 * quirk_p45_bw_notifications - Intel P45 에서 링크 대역폭 알림 인터럽트를 끈다
 *
 * @dev: Intel device 0x2e21 (P45 칩셋의 PCIe 포트)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel P45 칩셋.
 * [무엇이 문제] 아래 영어 주석대로 Link Bandwidth Management Interrupt
 * (대역폭 알림)를 켜면 부팅 중에 시스템이 멈춘다. PCIe 스펙의 선택적
 * 기능인데 이 칩의 구현이 온전하지 않다.
 * [대역폭 알림이란] 링크 속도나 폭이 바뀌면 포트가 인터럽트로 알려 주는
 * 기능이다. 커널은 이것으로 링크 열화를 감지한다.
 * [그대로 두면] 부팅이 멈춘다.
 * [우회] dev->no_bw_notif 를 1 로 세워 PCI 코어가 이 포트에서 대역폭 알림
 * 인터럽트를 켜지 않게 한다.
 *
 * 실행 컨텍스트: HEADER 단계. 포트 서비스가 알림을 켜기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_p45_bw_notifications]
 */
/*
 * Enabling Link Bandwidth Management Interrupts (BW notifications) can cause
 * boot hangs on P45.
 */
static void quirk_p45_bw_notifications(struct pci_dev *dev)
{
	dev->no_bw_notif = 1;	/* [한국어] PCI 코어가 이 포트에 대해 대역폭 알림을 켜지 않도록 하는 플래그. */
}
/* [한국어] P45 의 해당 device ID 에 HEADER 단계로 등록한다. 이름 있는
 * 상수가 없어 숫자를 그대로 적었다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2e21, quirk_p45_bw_notifications);

/*
 * [한국어]
 * quirk_mediagx_master - MediaGX/Geode PCI 마스터의 잘못된 설정을 바로잡는다
 *
 * @dev: Cyrix PCI Master(MediaGX/Geode)
 * @return: 없음
 *
 * [어떤 하드웨어] National/Cyrix MediaGX, Geode(GXM/GXLV/GX1).
 * [무엇이 문제] 아래 영어 주석대로 이 칩의 PCI 마스터가 흔히 잘못 설정되어
 * 있어 PCI 대역폭이 70MB/s 에서 25MB/s 로 떨어진다. 정확한 비트의 의미는
 * 데이터시트를 봐야 알 수 있다고 주석이 안내한다(현재 그 링크는 국립반도체
 * 사이트를 가리키며 이 트리에는 자료가 없다). 코드가 찍는 메시지로 보아
 * Slave Disconnect Boundary 설정이다.
 * [그대로 두면] 성능이 3분의 1 수준으로 떨어진다. 오동작은 아니다.
 * [우회] 벤더 전용 레지스터 0x41 의 비트 1 을 끈다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME. 재개 후 다시 설정해야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_mediagx_master] -> pci_write_config_byte()
 */
/*
 * Common misconfiguration of the MediaGX/Geode PCI master that will reduce
 * PCI bandwidth from 70MB/s to 25MB/s.  See the GXM/GXLV/GX1 datasheets
 * found at http://www.national.com/analog for info on what these bits do.
 * <christer@weinigel.se>
 */
static void quirk_mediagx_master(struct pci_dev *dev)
{
	u8 reg;	/* [한국어] 0x41 레지스터 값. */

	pci_read_config_byte(dev, 0x41, &reg);	/* [한국어] 벤더 전용 레지스터 0x41 을 읽는다. */
	if (reg & 2) {	/* [한국어] 비트 1 이 서 있으면 성능을 떨어뜨리는 설정이 켜져 있는 것이다. */
		reg &= ~2;	/* [한국어] 그 비트만 끈다. */
		pci_info(dev, "Fixup for MediaGX/Geode Slave Disconnect Boundary (0x41=0x%02x)\n",	/* [한국어] 무엇을 어떤 값으로 고쳤는지 남긴다. */
			 reg);
		pci_write_config_byte(dev, 0x41, reg);	/* [한국어] 고친 값을 되쓴다. */
	}
}
/* [한국어] Cyrix PCI Master 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_CYRIX,	PCI_DEVICE_ID_CYRIX_PCI_MASTER, quirk_mediagx_master);
/* [한국어] 재개 시에도 다시 적용한다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_CYRIX,	PCI_DEVICE_ID_CYRIX_PCI_MASTER, quirk_mediagx_master);

/*
 * [한국어]
 * quirk_disable_pxb - Intel 450NX C0 리비전의 PCI restreaming 을 끈다
 *
 * @pdev: Intel 82454NX 브리지, 리비전 0x04(C0)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 82454NX(450NX 칩셋의 PCI 확장 브리지), C0 리비전.
 * [무엇이 문제] 아래 영어 주석대로 C0 리비전에서 restreaming 기능을 켜 두면
 * 데이터가 손상된다. 보통은 BIOS 가 꺼 주지만 그러지 않는 보드가 있어
 * 리눅스가 직접 확인한다.
 * [그대로 두면] 데이터가 조용히 깨진다.
 * [우회] 벤더 전용 레지스터 0x40 의 비트 6 을 끈다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_pxb] -> pci_write_config_word()
 */
/*
 * Ensure C0 rev restreaming is off. This is normally done by the BIOS but
 * in the odd case it is not the results are corruption hence the presence
 * of a Linux check.
 */
static void quirk_disable_pxb(struct pci_dev *pdev)
{
	u16 config;	/* [한국어] 0x40 레지스터의 16비트 값. */

	/* [한국어] 옆의 영어 주석대로 C0 리비전(0x04)에서만 이 문제가 있다.
	 * 다른 리비전은 손대지 않는다. */
	if (pdev->revision != 0x04)		/* Only C0 requires this */
		return;
	pci_read_config_word(pdev, 0x40, &config);	/* [한국어] 벤더 전용 레지스터 0x40 을 읽는다. */
	if (config & (1<<6)) {	/* [한국어] 비트 6 이 restreaming 활성 비트다. */
		config &= ~(1<<6);	/* [한국어] 그 비트만 끈다. */
		pci_write_config_word(pdev, 0x40, config);	/* [한국어] 고친 값을 되쓴다. */
		pci_info(pdev, "C0 revision 450NX. Disabling PCI restreaming\n");	/* [한국어] 무엇을 껐는지 남긴다. */
	}
}
/* [한국어] Intel 82454NX 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82454NX,	quirk_disable_pxb);
/* [한국어] 재개 시에도 다시 끈다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82454NX,	quirk_disable_pxb);

/*
 * [한국어]
 * quirk_amd_ide_mode - IDE 모드로 노출된 SATA 컨트롤러를 AHCI 모드로 되돌린다
 *
 * @pdev: ATI IXP600/IXP700 또는 AMD Hudson-2/SB7x0 계열 SATA 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] ATI IXP600/IXP700 SATA, AMD SBx00/Hudson-2 SATA(0x7900 포함).
 * [무엇이 문제] 이 컨트롤러들은 BIOS 설정에 따라 자신을 레거시 IDE
 * 컨트롤러(클래스 서브코드 0x01)로 보고할 수 있다. 하드웨어는 AHCI 를
 * 지원하는데 클래스 코드만 IDE 로 위장하는 것이다.
 * [그대로 두면] 커널이 옛 IDE 드라이버로 붙어 NCQ, 핫플러그 같은 AHCI
 * 기능을 못 쓴다.
 * [우회] 벤더 전용 레지스터 0x40 의 비트 0 을 잠시 세워 클래스 코드
 * 레지스터를 쓰기 가능하게 만든 뒤, 클래스 코드를 SATA/AHCI 로 바꾸고
 * 다시 잠근다. 하드웨어의 클래스 코드 자체를 바꾸는 드문 quirk 다.
 *
 * [주의 - RESUME_EARLY 에도 등록되는 이유] 절전에서 깨어나면 BIOS 가
 * 다시 IDE 모드로 되돌려 놓으므로, 재개 경로의 이른 시점에 같은 작업을
 * 반복해야 한다.
 *
 * 실행 컨텍스트: HEADER 와 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_amd_ide_mode] -> pci_write_config_byte()
 */
static void quirk_amd_ide_mode(struct pci_dev *pdev)
{
	/* set SBX00/Hudson-2 SATA in IDE mode to AHCI mode */
	/* [한국어] 클래스 코드 읽기와 쓰기에 함께 쓰는 임시 바이트. */
	u8 tmp;

	pci_read_config_byte(pdev, PCI_CLASS_DEVICE, &tmp);	/* [한국어] PCI_CLASS_DEVICE 는 클래스 코드의 상위 워드가 있는 오프셋 0x0a 다. 여기서 한 바이트만 읽으면 서브클래스가 나온다. */
	if (tmp == 0x01) {	/* [한국어] 서브클래스 0x01 은 IDE 컨트롤러다. 즉 지금 IDE 로 위장 중이라는 뜻이다. */
		pci_read_config_byte(pdev, 0x40, &tmp);	/* [한국어] 벤더 전용 레지스터 0x40 의 현재 값을 저장해 둔다 - 끝에 원래대로 되돌려야 한다. */
		pci_write_config_byte(pdev, 0x40, tmp|1);	/* [한국어] 비트 0 을 세워 클래스 코드 레지스터의 쓰기 잠금을 푼다. */
		pci_write_config_byte(pdev, 0x9, 1);	/* [한국어] 오프셋 0x09 는 프로그래밍 인터페이스(ProgIf) - AHCI 를 뜻하는 1 을 쓴다. */
		pci_write_config_byte(pdev, 0xa, 6);	/* [한국어] 오프셋 0x0a 는 서브클래스 - SATA 를 뜻하는 6 을 쓴다. 결과적으로 클래스 코드가 0x010601(Mass Storage : SATA : AHCI)이 된다. */
		pci_write_config_byte(pdev, 0x40, tmp);	/* [한국어] 저장해 둔 원래 값을 되써서 클래스 코드 레지스터를 다시 잠근다. */

		pdev->class = PCI_CLASS_STORAGE_SATA_AHCI;	/* [한국어] 커널이 기억하는 클래스 코드도 SATA/AHCI 로 맞춘다 - 드라이버 매칭은 이 필드로 한다. */
		pci_info(pdev, "set SATA to AHCI mode\n");	/* [한국어] 모드를 바꿨음을 남긴다. */
	}
}
/* [한국어] 대상 칩 4종에 대해 HEADER 와 RESUME_EARLY 를 짝지어 등록한다.
 * 재개 후 BIOS 가 다시 IDE 모드로 돌려놓기 때문에 두 단계가 모두 필요하다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATI, PCI_DEVICE_ID_ATI_IXP600_SATA, quirk_amd_ide_mode);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_ATI, PCI_DEVICE_ID_ATI_IXP600_SATA, quirk_amd_ide_mode);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATI, PCI_DEVICE_ID_ATI_IXP700_SATA, quirk_amd_ide_mode);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_ATI, PCI_DEVICE_ID_ATI_IXP700_SATA, quirk_amd_ide_mode);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_AMD_HUDSON2_SATA_IDE, 0x7900. 위 블록 주석의 설명이 그대로 적용되며
 * 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_HUDSON2_SATA_IDE, quirk_amd_ide_mode);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_HUDSON2_SATA_IDE, quirk_amd_ide_mode);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AMD, 0x7900, quirk_amd_ide_mode);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_AMD, 0x7900, quirk_amd_ide_mode);

/*
 * [한국어]
 * quirk_svwks_csb5ide - Serverworks CSB5 IDE 를 레거시(compat) 모드로 되돌린다
 *
 * @pdev: Serverworks CSB5 IDE 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] Serverworks CSB5 IDE.
 * [무엇이 문제] 아래 영어 주석대로 이 컨트롤러는 native 모드를 완전히
 * 지원하지 못한다. 그런데 클래스 코드의 ProgIf 로는 native 모드를 지원한다고
 * 보고한다. IDE ProgIf 의 비트 0 과 2 는 각각 1차/2차 채널이 native 모드임을
 * 뜻한다(비트 1,3 은 그 모드를 바꿀 수 있는지).
 * [그대로 두면] 커널이 native 모드로 동작시키려다 실패한다.
 * [우회] ProgIf 의 비트 0 과 2 를 지워 레거시(compat) 모드로 보고하게 하고,
 * 커널이 기억하는 클래스 코드도 함께 고친다. 그러면 PCI 계층이 레거시 IDE
 * 의 고정 IO 주소를 리소스로 잡아 준다.
 *
 * [EARLY 단계인 이유] IDE 모드는 BAR 해석 방식을 바꾼다. 레거시 모드에서는
 * BAR 대신 고정된 레거시 포트를 쓰므로, 커널이 BAR 를 읽기 전인 EARLY 에
 * 고쳐야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_svwks_csb5ide] -> pci_write_config_byte()
 */
/* Serverworks CSB5 IDE does not fully support native mode */
static void quirk_svwks_csb5ide(struct pci_dev *pdev)
{
	/* [한국어] ProgIf(프로그래밍 인터페이스) 값을 담는 바이트. */
	u8 prog;
	pci_read_config_byte(pdev, PCI_CLASS_PROG, &prog);	/* [한국어] PCI_CLASS_PROG 는 클래스 코드의 최하위 바이트(오프셋 0x09), 즉 ProgIf 다. */
	if (prog & 5) {	/* [한국어] 비트 0 또는 2 가 서 있으면 어느 한 채널이라도 native 모드로 보고하는 중이다. */
		prog &= ~5;	/* [한국어] 두 비트를 지워 양쪽 채널 모두 레거시 모드로 만든다. */
		pdev->class &= ~5;	/* [한국어] 커널이 기억하는 클래스 코드에서도 같은 비트를 지운다. 하드웨어와 커널의 인식을 함께 맞춘다. */
		pci_write_config_byte(pdev, PCI_CLASS_PROG, prog);	/* [한국어] 고친 ProgIf 를 하드웨어에 되쓴다. */
		/* PCI layer will sort out resources */
	}
}
/* [한국어] CSB5 IDE 에 EARLY 단계로 등록한다 - BAR 해석 전이어야 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, PCI_DEVICE_ID_SERVERWORKS_CSB5IDE, quirk_svwks_csb5ide);

/* Intel 82801CAM ICH3-M datasheet says IDE modes must be the same */
/*
 * [한국어]
 * quirk_ide_samemode - 두 IDE 채널의 모드가 서로 다르면 둘 다 레거시로 맞춘다
 *
 * @pdev: Intel 82801CA(ICH3-M) IDE 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 82801CAM(ICH3-M).
 * [무엇이 문제] 위 영어 주석대로 이 칩의 데이터시트는 두 IDE 채널이 같은
 * 모드여야 한다고 못 박고 있다. 그런데 펌웨어가 한 채널은 native, 다른
 * 채널은 레거시로 설정해 두는 경우가 있다.
 * [그대로 두면] 하드웨어가 지원하지 않는 조합으로 동작해 오동작한다.
 * [우회] ProgIf 의 비트 0(1차 채널 native)과 비트 2(2차 채널 native)가
 * 서로 다르면, 둘 다 지워 양쪽을 레거시 모드로 통일한다.
 *
 * 실행 컨텍스트: EARLY 단계 - BAR 해석 방식이 모드에 따라 달라지므로
 * 커널이 BAR 를 읽기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_ide_samemode] -> pci_write_config_byte()
 */
static void quirk_ide_samemode(struct pci_dev *pdev)
{
	u8 prog;	/* [한국어] ProgIf(클래스 코드 최하위 바이트) 값. */

	pci_read_config_byte(pdev, PCI_CLASS_PROG, &prog);	/* [한국어] PCI_CLASS_PROG(오프셋 0x09)에서 ProgIf 를 읽는다. */

	if (((prog & 1) && !(prog & 4)) || ((prog & 4) && !(prog & 1))) {	/* [한국어] 비트 0 과 비트 2 중 정확히 하나만 서 있는지 검사한다 - 즉 두 채널의 모드가 서로 다른 경우다. 배타적 논리합을 직접 풀어 쓴 형태다. */
		pci_info(pdev, "IDE mode mismatch; forcing legacy mode\n");	/* [한국어] 모드 불일치를 발견해 레거시로 통일한다고 알린다. */
		prog &= ~5;	/* [한국어] 두 native 비트를 모두 지운다. */
		pdev->class &= ~5;	/* [한국어] 커널이 기억하는 클래스 코드도 함께 맞춘다. */
		pci_write_config_byte(pdev, PCI_CLASS_PROG, prog);	/* [한국어] 고친 ProgIf 를 하드웨어에 되쓴다. */
	}
}
/* [한국어] ICH3-M 의 IDE 함수에 EARLY 단계로 등록한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82801CA_10, quirk_ide_samemode);

/*
 * [한국어]
 * quirk_no_ata_d3 - D3 절전 상태로 들어가면 망가지는 ATA 장치를 표시
 *
 * @pdev: 레거시 IDE 클래스의 ATA 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] Serverworks, ATI, ALi, VIA 의 레거시 IDE 컨트롤러 전부
 * (아래 등록표 참조). AHCI 컨트롤러는 대상이 아니다.
 * [무엇이 문제] PCI 전원관리 스펙상 D3hot 에서 D0 로 돌아오면 장치가
 * 정상 동작해야 하는데, 이 칩들은 그러지 못한다. 등록표의 영어 주석이
 * 각각의 사정을 밝힌다 - ALi 는 복원할 수 없는 레지스터 설정을 잃어버리고,
 * VIA 는 되돌아오기는 하지만 살려 두지 않으면 모드 탐지 중 ACPI GTM 실패가
 * 생긴다.
 * [그대로 두면] 절전 후 디스크가 사라지거나 오동작한다.
 * [우회] dev_flags 에 PCI_DEV_FLAGS_NO_D3 을 세워 PCI 전원관리 코어가 이
 * 장치를 D3 로 내리지 않게 한다.
 *
 * 실행 컨텍스트: EARLY 단계, 클래스 코드로 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_no_ata_d3]
 */
/* Some ATA devices break if put into D3 */
static void quirk_no_ata_d3(struct pci_dev *pdev)
{
	pdev->dev_flags |= PCI_DEV_FLAGS_NO_D3;	/* [한국어] PCI 전원관리 코어가 보는 플래그. 이 비트가 서 있으면 장치를 D3 로 내리지 않는다. */
}
/* Quirk the legacy ATA devices only. The AHCI ones are ok */
/* [한국어] Serverworks 의 모든 레거시 IDE 컨트롤러. class_shift 8 로
 * 베이스/서브클래스(Mass Storage : IDE)까지만 비교한다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_SERVERWORKS, PCI_ANY_ID,
				PCI_CLASS_STORAGE_IDE, 8, quirk_no_ata_d3);
/* [한국어] ATI 의 모든 레거시 IDE 컨트롤러. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_ATI, PCI_ANY_ID,
				PCI_CLASS_STORAGE_IDE, 8, quirk_no_ata_d3);
/* ALi loses some register settings that we cannot then restore */
/* [한국어] ALi - 위 영어 주석대로 복원 불가능한 레지스터 설정을 잃는다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_AL, PCI_ANY_ID,
				PCI_CLASS_STORAGE_IDE, 8, quirk_no_ata_d3);
/* VIA comes back fine but we need to keep it alive or ACPI GTM failures
   occur when mode detecting */
/* [한국어] VIA - 위 영어 주석대로 D3 에서 정상 복귀하기는 하지만,
 * 살려 두지 않으면 모드 탐지 중 ACPI GTM 이 실패한다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_VIA, PCI_ANY_ID,
				PCI_CLASS_STORAGE_IDE, 8, quirk_no_ata_d3);

/*
 * [한국어]
 * quirk_eisa_bridge - 클래스 코드가 비어 있는 i82375 를 EISA 브리지로 표시
 *
 * @dev: Intel 82375 PCI/EISA 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel i82375 PCI-to-EISA 브리지.
 * [무엇이 문제] 아래 영어 주석대로 이 칩은 클래스 코드를 분류 없음
 * (non-classified)으로 보고한다. 실제로는 EISA 브리지인데 자신이 무엇인지
 * 말하지 않는 것이다.
 * [그대로 두면] 커널이 이 장치를 브리지로 인식하지 못해 EISA 쪽 장치를
 * 다루지 못한다.
 * [우회] dev->class 를 EISA 브리지 클래스로 덮어쓴다. << 8 은 dev->class 가
 * 24비트(베이스:서브:ProgIf) 형식인데 PCI_CLASS_BRIDGE_EISA 는 상위
 * 16비트만 담은 상수이기 때문이다.
 *
 * [원래 알파 전용이었다는 주석] 영어 주석은 이것이 원래 Alpha 아키텍처
 * 전용 처리였지만 여기(공용 quirks)에 두는 것이 맞다고 밝힌다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_eisa_bridge]
 */
/*
 * This was originally an Alpha-specific thing, but it really fits here.
 * The i82375 PCI/EISA bridge appears as non-classified. Fix that.
 */
static void quirk_eisa_bridge(struct pci_dev *dev)
{
	dev->class = PCI_CLASS_BRIDGE_EISA << 8;	/* [한국어] 8비트 왼쪽 시프트로 ProgIf 자리를 0 으로 두고 베이스/서브클래스를 채운다. */
}
/* [한국어] Intel 82375 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82375,	quirk_eisa_bridge);

/*
 * On ASUS P4B boards, the SMBus PCI Device within the ICH2/4 southbridge
 * is not activated. The myth is that Asus said that they do not want the
 * users to be irritated by just another PCI Device in the Win98 device
 * manager. (see the file prog/hotplug/README.p4b in the lm_sensors
 * package 2.7.0 for details)
 *
 * The SMBus PCI Device can be activated by setting a bit in the ICH LPC
 * bridge. Unfortunately, this device has no subvendor/subdevice ID. So it
 * becomes necessary to do this tweak in two steps -- the chosen trigger
 * is either the Host bridge (preferred) or on-board VGA controller.
 *
 * Note that we used to unhide the SMBus that way on Toshiba laptops
 * (Satellite A40 and Tecra M2) but then found that the thermal management
 * was done by SMM code, which could cause unsynchronized concurrent
 * accesses to the SMBus registers, with potentially bad effects. Thus you
 * should be very careful when adding new entries: if SMM is accessing the
 * Intel SMBus, this is a very good reason to leave it hidden.
 *
 * Likewise, many recent laptops use ACPI for thermal management. If the
 * ACPI DSDT code accesses the SMBus, then Linux should not access it
 * natively, and keeping the SMBus hidden is the right thing to do. If you
 * are about to add an entry in the table below, please first disassemble
 * the DSDT and double-check that there is no code accessing the SMBus.
 */
/* [한국어] '이 보드는 SMBus 를 숨기고 있으니 드러내도 된다'는 표식.
 * 설정자: asus_hides_smbus_hostbridge() 가 보드 모델을 확인하고 1 로 올린다.
 * 읽는 자: asus_hides_smbus_lpc() 계열이 실제로 숨김을 푸는 조건으로 본다.
 * 값 범위: 0(대상 아님) 또는 1(대상 보드).
 * 왜 두 단계인가: 숨겨진 SMBus 장치 자체에는 서브벤더/서브디바이스 ID 가
 *   없어 보드를 식별할 수 없다. 그래서 보드를 식별할 수 있는 다른 장치
 *   (호스트 브리지, 없으면 온보드 VGA 나 USB 컨트롤러)를 방아쇠로 삼아
 *   먼저 이 플래그를 세우고, 나중에 LPC 브리지 quirk 가 그 플래그를 보고
 *   숨김을 푼다.
 * 동기화: 부팅 중 열거 문맥에서만 접근하므로 별도 락이 없다. */
static int asus_hides_smbus;

/*
 * [한국어]
 * asus_hides_smbus_hostbridge - 이 보드가 SMBus 를 숨기는 모델인지 판정해 표식을 남긴다
 *
 * @dev: 호스트 브리지(또는 보드 식별용 대체 장치)
 * @return: 없음
 *
 * [어떤 하드웨어] ASUS P4B 계열 메인보드와 노트북, 일부 HP/Compaq/삼성
 * 노트북 및 데스크톱. 아래 switch 의 서브디바이스 ID 목록이 전체 대상이다.
 * [무엇이 문제] 위 영어 주석대로, ICH2/ICH4 사우스브리지 안의 SMBus PCI
 * 장치가 활성화되지 않은 채 출하된다. 정설은 ASUS 가 Win98 장치 관리자에
 * 장치가 하나 더 보이는 것을 사용자가 싫어할까 봐 숨겼다는 것이다.
 * [그대로 두면] 리눅스가 SMBus 를 못 써서 온도/전압 센서를 읽지 못한다.
 * [우회] ICH LPC 브리지의 비트 하나를 지우면 숨김이 풀린다. 그런데 숨겨진
 * SMBus 장치에는 서브벤더/서브디바이스 ID 가 없어 '이 보드가 그 대상인가'를
 * 판정할 수 없다. 그래서 두 단계로 나눈다 - 이 함수가 보드를 식별해
 * asus_hides_smbus 플래그를 세우고, asus_hides_smbus_lpc() 가 실제로 푼다.
 *
 * [방아쇠 장치 선택] 우선순위는 호스트 브리지다. 호스트 브리지에
 * 서브벤더/서브디바이스 ID 가 없는 보드에서는 온보드 VGA 컨트롤러나 USB
 * UHCI 컨트롤러를 대신 쓴다(아래 Compaq 항목의 영어 주석 참조).
 *
 * [★ 새 항목을 추가할 때의 경고] 영어 주석이 강하게 경고한다. 예전에
 * 도시바 노트북(Satellite A40, Tecra M2)에서도 같은 방법으로 SMBus 를
 * 드러냈다가, 그 기종은 열 관리를 SMM 코드가 하고 있어 SMBus 레지스터에
 * 동기화되지 않은 동시 접근이 생긴다는 것을 알게 되었다. 최근 노트북은
 * ACPI 로 열 관리를 하는 경우가 많은데, ACPI DSDT 코드가 SMBus 를 건드린다면
 * 리눅스는 건드리면 안 된다. 표에 항목을 추가하기 전에 DSDT 를 역어셈블해
 * SMBus 접근 코드가 없는지 반드시 확인하라는 것이 원저자의 요구다.
 *
 * 실행 컨텍스트: HEADER 단계. 아래 LPC quirk 도 HEADER 이므로, 등록 순서가
 * 아니라 '장치 열거 순서'에 의존한다 - 호스트 브리지가 LPC 브리지보다
 * 먼저 열거되기 때문에 성립하는 구조다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [asus_hides_smbus_hostbridge]
 */
static void asus_hides_smbus_hostbridge(struct pci_dev *dev)
{
	if (unlikely(dev->subsystem_vendor == PCI_VENDOR_ID_ASUSTEK)) {	/* [한국어] 서브벤더가 ASUS 인 경우. unlikely() 는 대부분의 시스템이 해당하지 않음을 컴파일러에 알려 분기 예측을 돕는다. */
		if (dev->device == PCI_DEVICE_ID_INTEL_82845_HB)	/* [한국어] Intel 82845 호스트 브리지에 붙은 ASUS 보드들. */
			switch (dev->subsystem_device) {	/* [한국어] 서브디바이스 ID 로 정확한 보드 모델을 가른다. */
			case 0x8025: /* P4B-LX */
			case 0x8070: /* P4B */
			case 0x8088: /* P4B533 */
			case 0x1626: /* L3C notebook */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인 - 나중에 LPC quirk 가 숨김을 풀도록 표식을 남긴다. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82845G_HB)	/* [한국어] 82845G 호스트 브리지 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 ASUS 보드 모델 목록. */
			case 0x80b1: /* P4GE-V */
			case 0x80b2: /* P4PE */
			case 0x8093: /* P4B533-V */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82850_HB)	/* [한국어] 82850 호스트 브리지 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 모델 목록. */
			case 0x8030: /* P4T533 */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_7205_0)	/* [한국어] Intel 7205 호스트 브리지 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 모델 목록. */
			case 0x8070: /* P4G8X Deluxe */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_E7501_MCH)	/* [한국어] E7501 MCH 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 모델 목록. */
			case 0x80c9: /* PU-DLS */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82855GM_HB)	/* [한국어] 82855GM(모바일) 호스트 브리지 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 노트북 모델 목록. */
			case 0x1751: /* M2N notebook */
			case 0x1821: /* M5N notebook */
			case 0x1897: /* A6L notebook */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82855PM_HB)	/* [한국어] 82855PM(모바일) 호스트 브리지 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 노트북 모델 목록. */
			case 0x184b: /* W1N notebook */
			case 0x186a: /* M6Ne notebook */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82865_HB)	/* [한국어] 82865 호스트 브리지 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 모델 목록. */
			case 0x80f2: /* P4P800-X */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82915GM_HB)	/* [한국어] 82915GM(모바일) 호스트 브리지 계열. */
			switch (dev->subsystem_device) {	/* [한국어] 해당 세대의 노트북 모델 목록. */
			case 0x1882: /* M6V notebook */
			case 0x1977: /* A6VA notebook */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
	} else if (unlikely(dev->subsystem_vendor == PCI_VENDOR_ID_HP)) {	/* [한국어] 서브벤더가 HP 인 경우 - 같은 인텔 칩셋을 쓰는 HP 기종에도 같은 숨김이 있다. */
		if (dev->device ==  PCI_DEVICE_ID_INTEL_82855PM_HB)	/* [한국어] 82855PM 을 쓰는 HP 노트북. */
			switch (dev->subsystem_device) {	/* [한국어] 모델 목록. */
			case 0x088C: /* HP Compaq nc8000 */
			case 0x0890: /* HP Compaq nc6000 */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82865_HB)	/* [한국어] 82865 를 쓰는 HP 데스크톱. */
			switch (dev->subsystem_device) {	/* [한국어] 모델 목록. */
			case 0x12bc: /* HP D330L */
			case 0x12bd: /* HP D530 */
			case 0x006a: /* HP Compaq nx9500 */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82875_HB)	/* [한국어] 82875 를 쓰는 HP 워크스테이션. */
			switch (dev->subsystem_device) {	/* [한국어] 모델 목록. */
			case 0x12bf: /* HP xw4100 */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
	} else if (unlikely(dev->subsystem_vendor == PCI_VENDOR_ID_SAMSUNG)) {	/* [한국어] 서브벤더가 삼성인 경우. */
		if (dev->device ==  PCI_DEVICE_ID_INTEL_82855PM_HB)	/* [한국어] 82855PM 을 쓰는 삼성 노트북. */
			switch (dev->subsystem_device) {	/* [한국어] 모델 목록. */
			case 0xC00C: /* Samsung P35 notebook */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
		}
	} else if (unlikely(dev->subsystem_vendor == PCI_VENDOR_ID_COMPAQ)) {	/* [한국어] 서브벤더가 Compaq 인 경우. */
		if (dev->device == PCI_DEVICE_ID_INTEL_82855PM_HB)	/* [한국어] 82855PM 을 쓰는 Compaq 노트북. */
			switch (dev->subsystem_device) {	/* [한국어] 모델 목록. */
			case 0x0058: /* Compaq Evo N620c */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82810_IG3)	/* [한국어] 82810 내장 그래픽 - 아래 영어 주석대로 이 보드는 호스트 브리지에 서브벤더/서브디바이스 ID 가 없어 온보드 VGA 컨트롤러를 방아쇠로 쓴다. */
			switch (dev->subsystem_device) {	/* [한국어] VGA 컨트롤러의 서브디바이스 ID 로 보드를 식별한다. */
			case 0xB16C: /* Compaq Deskpro EP 401963-001 (PCA# 010174) */
				/* Motherboard doesn't have Host bridge
				 * subvendor/subdevice IDs, therefore checking
				 * its on-board VGA controller */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82801DB_2)	/* [한국어] 82801DB 의 USB UHCI 컨트롤러 #1 - 아래 영어 주석대로 이 보드는 호스트 브리지에 ID 가 없고 AGP 카드를 꽂으면 온보드 VGA 도 꺼지므로, 항상 존재하는 USB 컨트롤러를 방아쇠로 쓴다. */
			switch (dev->subsystem_device) {	/* [한국어] USB 컨트롤러의 서브디바이스 ID 로 보드를 식별한다. */
			case 0x00b8: /* Compaq Evo D510 CMT */
			case 0x00b9: /* Compaq Evo D510 SFF */
			case 0x00ba: /* Compaq Evo D510 USDT */
				/* Motherboard doesn't have Host bridge
				 * subvendor/subdevice IDs and on-board VGA
				 * controller is disabled if an AGP card is
				 * inserted, therefore checking USB UHCI
				 * Controller #1 */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
		else if (dev->device == PCI_DEVICE_ID_INTEL_82815_CGC)	/* [한국어] 82815 내장 그래픽 - 여기도 온보드 VGA 를 방아쇠로 쓴다. */
			switch (dev->subsystem_device) {	/* [한국어] VGA 컨트롤러의 서브디바이스 ID 로 보드를 식별한다. */
			case 0x001A: /* Compaq Deskpro EN SSF P667 815E */
				/* Motherboard doesn't have host bridge
				 * subvendor/subdevice IDs, therefore checking
				 * its on-board VGA controller */
				asus_hides_smbus = 1;	/* [한국어] 대상 보드 확인. */
			}
	}
}
/* [한국어] 보드 식별의 방아쇠가 되는 호스트 브리지 10종에 HEADER 단계로
 * 등록한다. 이 중 실제로 그 보드에 꽂혀 있는 하나만 실행된다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82845_HB,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82845G_HB,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82850_HB,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82865_HB,	asus_hides_smbus_hostbridge);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_82875_HB, PCI_DEVICE_ID_INTEL_7205_0,
 * PCI_DEVICE_ID_INTEL_E7501_MCH, PCI_DEVICE_ID_INTEL_82855PM_HB. 위 블록 주석의 설명이
 * 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82875_HB,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_7205_0,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_E7501_MCH,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82855PM_HB,	asus_hides_smbus_hostbridge);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_82855GM_HB, PCI_DEVICE_ID_INTEL_82915GM_HB. 위
 * 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82855GM_HB,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82915GM_HB, asus_hides_smbus_hostbridge);

/* [한국어] 호스트 브리지에 서브벤더/서브디바이스 ID 가 없는 보드를 위해,
 * 온보드 VGA(82810_IG3, 82815_CGC)와 USB UHCI(82801DB_2)도 방아쇠로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82810_IG3,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801DB_2,	asus_hides_smbus_hostbridge);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82815_CGC,	asus_hides_smbus_hostbridge);

/*
 * [한국어]
 * asus_hides_smbus_lpc - ICH LPC 브리지에서 숨겨진 i801 SMBus 장치를 드러낸다
 *
 * @dev: Intel ICH 계열 LPC 브리지
 * @return: 없음
 *
 * [무엇을 하는가] 위 asus_hides_smbus_hostbridge() 가 세운 플래그를 보고,
 * 대상 보드일 때만 LPC 브리지의 벤더 전용 레지스터 0xF2 의 비트 3 을 지운다.
 * 그 비트가 SMBus 함수를 감추는 스위치다.
 * [확인까지 하는 이유] 비트를 지운 뒤 다시 읽어 실제로 지워졌는지 확인한다.
 * 지워지지 않으면 (펌웨어가 되돌려 놓는 등의 이유로) 숨바꼭질이 계속되는
 * 것이므로 그 사실을 로그로 남긴다.
 *
 * 실행 컨텍스트: HEADER 와 RESUME_EARLY. 재개 후 다시 숨겨지므로 재개
 * 경로에서도 풀어 줘야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [asus_hides_smbus_lpc] -> pci_write_config_word()
 */
static void asus_hides_smbus_lpc(struct pci_dev *dev)
{
	u16 val;	/* [한국어] 0xF2 레지스터 값. */

	if (likely(!asus_hides_smbus))	/* [한국어] 대상 보드가 아니면 아무것도 하지 않는다. likely() 는 '대부분의 시스템은 대상이 아니다'를 컴파일러에 알린다. */
		return;

	pci_read_config_word(dev, 0xF2, &val);	/* [한국어] 벤더 전용 레지스터 0xF2 를 읽는다 - 여기에 기능 숨김 비트들이 있다. */
	if (val & 0x8) {	/* [한국어] 비트 3(값 0x8)이 SMBus 함수를 감추는 비트다. */
		pci_write_config_word(dev, 0xF2, val & (~0x8));	/* [한국어] 그 비트만 지워 되쓴다 - 다른 기능의 숨김 설정은 건드리지 않는다. */
		pci_read_config_word(dev, 0xF2, &val);	/* [한국어] 정말 지워졌는지 되읽어 확인한다. */
		if (val & 0x8)	/* [한국어] 여전히 서 있으면 무언가가 되돌려 놓은 것이다. */
			pci_info(dev, "i801 SMBus device continues to play 'hide and seek'! 0x%x\n",	/* [한국어] 숨바꼭질이 계속된다는 뜻의 메시지를 값과 함께 남긴다. */
				 val);
		else
			pci_info(dev, "Enabled i801 SMBus device\n");	/* [한국어] 성공적으로 드러났음을 알린다. */
	}
}
/* [한국어] 숨김 비트를 가진 ICH LPC 브리지 7종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801AA_0,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801DB_0,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801BA_0,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801CA_0,	asus_hides_smbus_lpc);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_82801CA_12, PCI_DEVICE_ID_INTEL_82801DB_12,
 * PCI_DEVICE_ID_INTEL_82801EB_0. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801CA_12,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801DB_12,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801EB_0,	asus_hides_smbus_lpc);
/* [한국어] 재개 시에도 다시 드러낸다. 절전에서 깨어나면 펌웨어가 SMBus 를
 * 다시 숨기기 때문이다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801AA_0,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801DB_0,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801BA_0,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801CA_0,	asus_hides_smbus_lpc);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_82801CA_12, PCI_DEVICE_ID_INTEL_82801DB_12,
 * PCI_DEVICE_ID_INTEL_82801EB_0. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801CA_12,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801DB_12,	asus_hides_smbus_lpc);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_82801EB_0,	asus_hides_smbus_lpc);

/* It appears we just have one such device. If not, we have a warning */
/* [한국어] ICH6 의 RCBA(Root Complex Base Address) 창을 절전 진입 때
 * ioremap 해 두고 재개가 끝난 뒤 해제하기 위한 전역 포인터.
 * 설정자: asus_hides_smbus_lpc_ich6_suspend() 가 매핑 주소를 넣는다.
 * 읽는 자: resume_early 가 레지스터를 쓰고, resume 가 해제한 뒤 NULL 로 되돌린다.
 * 값 범위: NULL(매핑 없음) 또는 유효한 MMIO 가상 주소.
 * 왜 전역인가: 절전과 재개는 서로 다른 quirk 함수에서 처리되므로 그 사이에
 *   매핑을 넘길 곳이 필요하다. 위 영어 주석대로 이런 장치가 시스템에 하나뿐
 *   이라고 보고 전역 하나로 처리하며, 둘 이상이면 아래 WARN_ON 이 잡아 준다.
 * 동기화: 절전/재개는 단일 스레드 문맥이라 별도 락이 없다. */
static void __iomem *asus_rcba_base;
/*
 * [한국어]
 * asus_hides_smbus_lpc_ich6_suspend - 절전 진입 시 ICH6 의 RCBA 창을 미리 매핑해 둔다
 *
 * @dev: Intel ICH6 LPC 브리지
 * @return: 없음
 *
 * [왜 필요한가] ICH6 세대에서는 SMBus 숨김 스위치가 Configuration Space 가
 * 아니라 RCBA(Root Complex Base Address)가 가리키는 MMIO 창의 Function
 * Disable 레지스터에 있다. 재개 경로의 이른 시점(resume_early)에서는 새로
 * ioremap 을 하기에 적절하지 않으므로, 절전에 들어가기 전에 미리 매핑해 두고
 * 그 주소를 전역에 남긴다.
 *
 * 실행 컨텍스트: SUSPEND 단계. 그리고 HEADER 단계의 asus_hides_smbus_lpc_ich6()
 * 가 부팅 시에도 이 함수를 부른다.
 *
 * 호출 체인:
 *   pci_do_fixups() 또는 asus_hides_smbus_lpc_ich6()
 *     -> [asus_hides_smbus_lpc_ich6_suspend] -> ioremap()
 */
static void asus_hides_smbus_lpc_ich6_suspend(struct pci_dev *dev)
{
	u32 rcba;	/* [한국어] RCBA 레지스터 값 - MMIO 창의 물리 주소와 활성 비트가 함께 들어 있다. */

	if (likely(!asus_hides_smbus))	/* [한국어] 대상 보드가 아니면 아무것도 하지 않는다. */
		return;
	WARN_ON(asus_rcba_base);	/* [한국어] 이미 매핑이 남아 있으면 짝이 맞지 않는 호출이라는 뜻이므로 경고를 낸다. 전역 하나로 처리한다는 가정이 깨졌는지 확인하는 방어다. */

	pci_read_config_dword(dev, 0xF0, &rcba);	/* [한국어] 벤더 전용 레지스터 0xF0 이 RCBA 다. */
	/* use bits 31:14, 16 kB aligned */
	asus_rcba_base = ioremap(rcba & 0xFFFFC000, 0x4000);	/* [한국어] 옆의 영어 주석대로 비트 31:14 만 주소이고 하위 비트는 활성 표시 등이라, 0xFFFFC000 마스크로 16KB 정렬된 물리 주소만 남겨 0x4000(16KB) 크기로 매핑한다. */
	if (asus_rcba_base == NULL)	/* [한국어] 매핑 실패 시 전역은 NULL 이므로 뒤의 함수들이 알아서 건너뛴다. */
		return;
}

/*
 * [한국어]
 * asus_hides_smbus_lpc_ich6_resume_early - 재개 직후 SMBus 함수를 다시 드러낸다
 *
 * @dev: Intel ICH6 LPC 브리지
 * @return: 없음
 *
 * 절전에서 깨어나면 펌웨어가 SMBus 함수를 다시 감춘다. 미리 매핑해 둔 RCBA
 * 창의 Function Disable 레지스터에서 SMBus 비활성 비트를 지워 되살린다.
 *
 * 실행 컨텍스트: RESUME_EARLY 단계. 이 시점에는 ioremap 이 이미 되어 있어야
 * 하므로 suspend 단계와 짝을 이룬다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [asus_hides_smbus_lpc_ich6_resume_early] -> writel()
 */
static void asus_hides_smbus_lpc_ich6_resume_early(struct pci_dev *dev)
{
	u32 val;	/* [한국어] Function Disable 레지스터 값. */

	if (likely(!asus_hides_smbus || !asus_rcba_base))	/* [한국어] 대상 보드가 아니거나 매핑이 없으면 할 수 있는 일이 없다. */
		return;

	/* read the Function Disable register, dword mode only */
	val = readl(asus_rcba_base + 0x3418);	/* [한국어] 위 영어 주석대로 Function Disable 레지스터는 RCBA + 0x3418 에 있고 dword 단위로만 접근해야 한다. */

	/* enable the SMBus device */
	writel(val & 0xFFFFFFF7, asus_rcba_base + 0x3418);	/* [한국어] 비트 3(0x8)이 SMBus 함수 비활성 비트다. 그 비트만 지워 되쓴다. */
}

/*
 * [한국어]
 * asus_hides_smbus_lpc_ich6_resume - 재개가 끝난 뒤 RCBA 매핑을 해제한다
 *
 * @dev: Intel ICH6 LPC 브리지
 * @return: 없음
 *
 * resume_early 에서 레지스터를 다 쓴 뒤이므로 매핑을 놓아준다. 매핑을
 * 계속 들고 있으면 커널 주소 공간이 낭비되고, 다음 절전에서 WARN_ON 이
 * 걸린다.
 *
 * 실행 컨텍스트: RESUME 단계(resume_early 보다 나중).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [asus_hides_smbus_lpc_ich6_resume] -> iounmap()
 */
static void asus_hides_smbus_lpc_ich6_resume(struct pci_dev *dev)
{
	if (likely(!asus_hides_smbus || !asus_rcba_base))	/* [한국어] 대상 보드가 아니거나 매핑이 없으면 할 일이 없다. */
		return;

	iounmap(asus_rcba_base);	/* [한국어] MMIO 매핑을 해제한다. */
	asus_rcba_base = NULL;	/* [한국어] 다음 절전 진입 시 WARN_ON 이 잘못 울리지 않도록 전역을 비운다. */
	pci_info(dev, "Enabled ICH6/i801 SMBus device\n");	/* [한국어] SMBus 를 되살렸음을 알린다. */
}

/*
 * [한국어]
 * asus_hides_smbus_lpc_ich6 - 부팅 시 세 단계를 한 번에 수행한다
 *
 * @dev: Intel ICH6 LPC 브리지
 * @return: 없음
 *
 * 절전/재개 경로에서는 세 함수가 각각 다른 단계에 등록되어 순서대로
 * 불린다. 그러나 부팅 시(HEADER 단계)에는 그런 순서가 없으므로, 매핑 ->
 * 레지스터 수정 -> 해제를 한 함수 안에서 연달아 수행해 같은 효과를 낸다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [asus_hides_smbus_lpc_ich6]
 *     -> suspend -> resume_early -> resume
 */
static void asus_hides_smbus_lpc_ich6(struct pci_dev *dev)
{
	asus_hides_smbus_lpc_ich6_suspend(dev);	/* [한국어] RCBA 창을 매핑한다. */
	asus_hides_smbus_lpc_ich6_resume_early(dev);	/* [한국어] SMBus 비활성 비트를 지운다. */
	asus_hides_smbus_lpc_ich6_resume(dev);	/* [한국어] 매핑을 해제하고 전역을 비운다. */
}
/* [한국어] 부팅 시에는 세 단계를 한 번에 수행하는 함수를 HEADER 에 건다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH6_1,	asus_hides_smbus_lpc_ich6);
/* [한국어] 절전 진입 시 RCBA 를 미리 매핑한다. */
DECLARE_PCI_FIXUP_SUSPEND(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH6_1,	asus_hides_smbus_lpc_ich6_suspend);
/* [한국어] 재개가 끝나면 매핑을 해제한다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH6_1,	asus_hides_smbus_lpc_ich6_resume);
/* [한국어] 재개 직후 SMBus 를 되살린다. RESUME_EARLY 가 RESUME 보다
 * 먼저 불리므로, 레지스터 수정이 매핑 해제보다 앞선다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ICH6_1,	asus_hides_smbus_lpc_ich6_resume_early);

/*
 * [한국어]
 * quirk_sis_96x_smbus - SiS 96x 사우스브리지가 숨긴 SMBus 장치를 드러낸다
 *
 * @dev: SiS 961/962/963 또는 SiS LPC 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] SiS 96x 사우스브리지.
 * [무엇이 문제] 아래 영어 주석대로 BIOS 가 SMBus 장치를 숨겨 놓는 것이
 * 보통이다. ASUS 사례와 같은 종류의 문제다.
 * [그대로 두면] 리눅스가 SMBus 를 못 써 센서를 읽지 못한다.
 * [우회] 벤더 전용 레지스터 0x77 의 비트 4 를 지운다. ASUS 사례와 달리
 * 보드 식별이 필요 없어 방아쇠 장치 없이 바로 처리한다.
 *
 * 실행 컨텍스트: HEADER 와 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() 또는 quirk_sis_503() -> [quirk_sis_96x_smbus]
 *     -> pci_write_config_byte()
 */
/* SiS 96x south bridge: BIOS typically hides SMBus device...  */
static void quirk_sis_96x_smbus(struct pci_dev *dev)
{
	u8 val = 0;	/* [한국어] 0x77 레지스터 값. 읽기 실패에 대비해 0 으로 초기화한다. */
	pci_read_config_byte(dev, 0x77, &val);	/* [한국어] 벤더 전용 레지스터 0x77 을 읽는다. */
	if (val & 0x10) {	/* [한국어] 비트 4(0x10)가 SMBus 숨김 비트다. */
		pci_info(dev, "Enabling SiS 96x SMBus\n");	/* [한국어] 드러낸다는 사실을 남긴다. */
		pci_write_config_byte(dev, 0x77, val & ~0x10);	/* [한국어] 그 비트만 지워 되쓴다. */
	}
}
/* [한국어] SiS 96x 계열 4종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_961,		quirk_sis_96x_smbus);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_962,		quirk_sis_96x_smbus);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_963,		quirk_sis_96x_smbus);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_LPC,		quirk_sis_96x_smbus);
/* [한국어] 재개 시에도 다시 드러낸다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_961,		quirk_sis_96x_smbus);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_962,		quirk_sis_96x_smbus);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_963,		quirk_sis_96x_smbus);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_LPC,		quirk_sis_96x_smbus);

/* [한국어] 아래 SIS_DETECT_REGISTER 는 SiS 사우스브리지의 벤더 전용
 * 레지스터 오프셋 0x40 이다. 이 레지스터의 비트 6 을 세우면 칩이 자신의
 * 진짜 Device ID 를 드러낸다. */
/*
 * ... This is further complicated by the fact that some SiS96x south
 * bridges pretend to be 85C503/5513 instead.  In that case see if we
 * spotted a compatible north bridge to make sure.
 * (pci_find_device() doesn't work yet)
 *
 * We can also enable the sis96x bit in the discovery register..
 */
/* [한국어] SiS 사우스브리지의 탐지 레지스터 오프셋 0x40. 위 한국어 설명 참조 -
 * 비트 6 을 세우면 칩이 위장을 걷고 진짜 Device ID 를 드러낸다. */
#define SIS_DETECT_REGISTER 0x40

/*
 * [한국어]
 * quirk_sis_503 - 85C503/5513 인 척하는 SiS 96x 를 진짜 정체로 드러낸다
 *
 * @dev: SiS 503 으로 보고된 사우스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] SiS 96x 사우스브리지 중 일부.
 * [무엇이 문제] 위 영어 주석대로 일부 SiS96x 는 자신을 옛 85C503/5513 인
 * 것처럼 보고한다. 즉 Device ID 가 진짜가 아니다.
 * [그대로 두면] 위 quirk_sis_96x_smbus() 가 매칭되지 않아 SMBus 가
 * 숨겨진 채 남는다.
 * [우회] 탐지 레지스터의 비트 6 을 세워 진짜 Device ID 를 드러내게 한 뒤,
 * 그 값이 96x 계열(0x096x)이거나 0x0018 이면 진짜 96x 로 판정한다. 아니면
 * 레지스터를 원래대로 되돌리고 물러난다.
 *
 * [96x quirk 를 손으로 부르는 이유] 아래 영어 주석대로, 이 quirk 와
 * quirk_sis_96x_smbus() 의 실행 순서는 링커 순서에 달려 있어 보장되지
 * 않는다. 96x quirk 가 이미 지나갔을 수도 있으므로 여기서 직접 한 번 더
 * 부른다. Device ID 를 먼저 진짜 값으로 갱신하는 것도 그 때문이다.
 *
 * 실행 컨텍스트: HEADER 와 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_sis_503] -> quirk_sis_96x_smbus()
 */
static void quirk_sis_503(struct pci_dev *dev)
{
	u8 reg;	/* [한국어] 탐지 레지스터의 원래 값 - 정체가 96x 가 아니면 되돌려야 한다. */
	u16 devid;	/* [한국어] 드러난 진짜 Device ID. */

	pci_read_config_byte(dev, SIS_DETECT_REGISTER, &reg);	/* [한국어] 탐지 레지스터 0x40 의 현재 값을 저장해 둔다. */
	pci_write_config_byte(dev, SIS_DETECT_REGISTER, reg | (1 << 6));	/* [한국어] 비트 6 을 세워 진짜 Device ID 가 보이게 한다. */
	pci_read_config_word(dev, PCI_DEVICE_ID, &devid);	/* [한국어] 표준 헤더의 Device ID(오프셋 0x02)를 다시 읽는다 - 이제 진짜 값이 나온다. */
	if (((devid & 0xfff0) != 0x0960) && (devid != 0x0018)) {	/* [한국어] 0x0960~0x096f 대역이거나 0x0018 이면 96x 계열이다. 그 밖이면 진짜 503 이므로 손대면 안 된다. */
		pci_write_config_byte(dev, SIS_DETECT_REGISTER, reg);	/* [한국어] 원래 값을 되써서 탐지 모드를 끈다 - 부작용을 남기지 않는다. */
		return;
	}

	/*
	 * Ok, it now shows up as a 96x.  Run the 96x quirk by hand in case
	 * it has already been processed.  (Depends on link order, which is
	 * apparently not guaranteed)
	 */
	dev->device = devid;	/* [한국어] 커널이 기억하는 Device ID 를 진짜 값으로 갱신한다. 아래에서 부를 96x quirk 와 이후의 드라이버 매칭이 이 값을 본다. */
	quirk_sis_96x_smbus(dev);	/* [한국어] 96x quirk 를 직접 호출한다 - 링커 순서 때문에 이미 지나갔을 수 있기 때문이다. */
}
/* [한국어] SiS 503 으로 보고되는 장치에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_503,		quirk_sis_503);
/* [한국어] 재개 시에도 같은 절차를 반복한다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_SI,	PCI_DEVICE_ID_SI_503,		quirk_sis_503);

/*
 * [한국어]
 * asus_hides_ac97_lpc - ASUS A8V 보드에서 꺼져 있는 온보드 AC97/MC97 을 켠다
 *
 * @dev: VIA VT8237 ISA 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] ASUS A8V / A8V Deluxe 보드의 VIA VT8237 ISA 브리지.
 * [무엇이 문제] 아래 영어 주석대로, 두 번째 PCI 사운드카드가 꽂혀 있으면
 * 온보드 AC97 오디오와 MC97 모뎀 컨트롤러가 비활성화된 채로 나온다.
 * [그대로 두면] 온보드 오디오와 모뎀을 쓸 수 없다.
 * [우회] VT8237 ISA 브리지의 벤더 전용 레지스터 0x50 의 비트 7:6 을 지워
 * 두 장치를 되살린다. SMBus 사례와 마찬가지로 지운 뒤 되읽어 확인한다.
 *
 * [보드 판별] 서브벤더가 ASUS 인지로만 거른다. asus_hides_smbus 와 달리
 * 전역 플래그를 쓰지 않고 지역 변수로 처리하는데, ISA 브리지 자신이
 * 서브벤더 ID 를 갖고 있어 방아쇠 장치가 필요 없기 때문이다.
 *
 * 실행 컨텍스트: HEADER 와 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [asus_hides_ac97_lpc] -> pci_write_config_byte()
 */
/*
 * On ASUS A8V and A8V Deluxe boards, the onboard AC97 audio controller
 * and MC97 modem controller are disabled when a second PCI soundcard is
 * present. This patch, tweaking the VT8237 ISA bridge, enables them.
 * -- bjd
 */
static void asus_hides_ac97_lpc(struct pci_dev *dev)
{
	u8 val;	/* [한국어] 0x50 레지스터 값. */
	int asus_hides_ac97 = 0;	/* [한국어] 이 보드가 대상인지 나타내는 지역 플래그. */

	if (likely(dev->subsystem_vendor == PCI_VENDOR_ID_ASUSTEK)) {	/* [한국어] 서브벤더가 ASUS 인지 본다. */
		if (dev->device == PCI_DEVICE_ID_VIA_8237)	/* [한국어] 그리고 이 장치가 VT8237 ISA 브리지인지 확인한다. */
			asus_hides_ac97 = 1;	/* [한국어] 대상 보드로 표시한다. */
	}

	if (!asus_hides_ac97)	/* [한국어] 대상이 아니면 아무것도 하지 않는다. */
		return;

	pci_read_config_byte(dev, 0x50, &val);	/* [한국어] 벤더 전용 레지스터 0x50 을 읽는다. */
	if (val & 0xc0) {	/* [한국어] 비트 7 과 6(0xc0)이 AC97/MC97 비활성 비트다. */
		pci_write_config_byte(dev, 0x50, val & (~0xc0));	/* [한국어] 두 비트를 지워 되쓴다. */
		pci_read_config_byte(dev, 0x50, &val);	/* [한국어] 정말 지워졌는지 되읽어 확인한다. */
		if (val & 0xc0)	/* [한국어] 여전히 서 있으면 무언가가 되돌려 놓은 것이다. */
			pci_info(dev, "Onboard AC97/MC97 devices continue to play 'hide and seek'! 0x%x\n",	/* [한국어] 숨바꼭질이 계속된다는 메시지를 값과 함께 남긴다. */
				 val);
		else
			pci_info(dev, "Enabled onboard AC97/MC97 devices\n");	/* [한국어] 성공적으로 켜졌음을 알린다. */
	}
}
/* [한국어] VT8237 에 HEADER 단계로 등록한다. 실제 적용 여부는 함수 안에서
 * 서브벤더로 판정한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8237, asus_hides_ac97_lpc);
/* [한국어] 재개 시에도 다시 켠다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_VIA,	PCI_DEVICE_ID_VIA_8237, asus_hides_ac97_lpc);

/* [한국어] 아래 JMicron quirk 는 libata 로 이 칩을 제대로 몰 수 있을 때만
 * 의미가 있다. ATA 계층이 빌드에 없으면 클래스 코드를 바꿔 봐야 붙을
 * 드라이버가 없으므로 통째로 제외한다. */
#if defined(CONFIG_ATA) || defined(CONFIG_ATA_MODULE)

/*
 * If we are using libata we can drive this chip properly but must do this
 * early on to make the additional device appear during the PCI scanning.
 */
/*
 * [한국어]
 * quirk_jmicron_ata - JMicron ATA 컨트롤러를 리눅스가 몰 수 있는 모드로 설정한다
 *
 * @pdev: JMicron JMB36x 계열 ATA 컨트롤러의 함수 0
 * @return: 없음
 *
 * [어떤 하드웨어] JMicron JMB360/361/362/363/364/365/366/368/369.
 * [무엇이 문제] 이 칩은 벤더 전용 레지스터 설정에 따라 자신을 여러 모습으로
 * 보여 준다 - 단일 함수 AHCI, 이중 함수(AHCI + IDE), 단일 함수 IDE 등.
 * 펌웨어가 설정해 둔 모드가 리눅스 libata 가 다루기 좋은 형태가 아닐 수
 * 있고, 모드에 따라 PCI 함수의 개수와 클래스 코드가 달라진다.
 * [그대로 두면] 두 번째 함수가 PCI 스캔에서 아예 보이지 않거나, 잘못된
 * 클래스 코드로 엉뚱한 드라이버가 붙는다.
 * [우회] 위 영어 주석대로, libata 가 있으면 이 칩을 제대로 몰 수 있으므로
 * 모델별로 정해진 모드가 되도록 0x40 과 0x80 레지스터를 다시 쓴다. 그리고
 * 바뀐 헤더 타입과 클래스 코드를 즉시 다시 읽어 struct pci_dev 에 반영한다.
 *
 * [★ EARLY 단계여야 하는 이유] 영어 주석이 밝히는 핵심이다. 설정을 바꾸면
 * 함수 1 이 새로 나타나는데, 그것이 PCI 스캔에 잡히려면 스캔이 그 함수를
 * 훑기 전에 설정이 끝나 있어야 한다. 그래서 가장 이른 EARLY 단계에 건다.
 *
 * 실행 컨텍스트: EARLY 와 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_jmicron_ata] -> pci_write_config_dword()
 */
static void quirk_jmicron_ata(struct pci_dev *pdev)
{
	u32 conf1, conf5, class;	/* [한국어] conf1: 0x40 레지스터, conf5: 0x80 레지스터, class: 다시 읽은 클래스 코드. */
	u8 hdr;	/* [한국어] 다시 읽은 헤더 타입 바이트. */

	/* Only poke fn 0 */
	if (PCI_FUNC(pdev->devfn))	/* [한국어] 위 영어 주석대로 함수 0 에서만 설정한다. PCI_FUNC() 는 devfn 의 하위 3비트(함수 번호)를 뽑는다. 함수 1 은 함수 0 의 설정에 따라 생겨나는 것이므로 손대면 안 된다. */
		return;

	pci_read_config_dword(pdev, 0x40, &conf1);	/* [한국어] 벤더 전용 설정 레지스터 1(오프셋 0x40)을 읽는다. */
	pci_read_config_dword(pdev, 0x80, &conf5);	/* [한국어] 벤더 전용 설정 레지스터 5(오프셋 0x80)를 읽는다. */

	conf1 &= ~0x00CFF302; /* Clear bit 1, 8, 9, 12-19, 22, 23 */
	conf5 &= ~(1 << 24);  /* Clear bit 24 */

	switch (pdev->device) {	/* [한국어] 모델마다 필요한 모드가 달라 device ID 로 가른다. */
	case PCI_DEVICE_ID_JMICRON_JMB360: /* SATA single port */
	case PCI_DEVICE_ID_JMICRON_JMB362: /* SATA dual ports */
	case PCI_DEVICE_ID_JMICRON_JMB364: /* SATA dual ports */
		/* The controller should be in single function ahci mode */
		conf1 |= 0x0002A100; /* Set 8, 13, 15, 17 */
		break;	/* [한국어] SATA 전용 모델 3종의 설정 완료. */

	case PCI_DEVICE_ID_JMICRON_JMB365:	/* [한국어] JMB365 - IDE 두 번째 PATA 포트가 있는 모델. */
	case PCI_DEVICE_ID_JMICRON_JMB366:	/* [한국어] JMB366 - 365 와 같은 처리. */
		/* Redirect IDE second PATA port to the right spot */
		conf5 |= (1 << 24);	/* [한국어] 위 영어 주석대로 IDE 두 번째 PATA 포트를 올바른 자리로 돌린다. */
		fallthrough;	/* [한국어] fallthrough 는 다음 case 로 의도적으로 흘러 내려간다는 표시다. 365/366 도 아래의 이중 함수 설정이 필요하기 때문이다. */
	case PCI_DEVICE_ID_JMICRON_JMB361:	/* [한국어] JMB361 - 이중 함수 모델. */
	case PCI_DEVICE_ID_JMICRON_JMB363:	/* [한국어] JMB363 - 이중 함수 모델. */
	case PCI_DEVICE_ID_JMICRON_JMB369:	/* [한국어] JMB369 - 이중 함수 모델. */
		/* Enable dual function mode, AHCI on fn 0, IDE fn1 */
		/* Set the class codes correctly and then direct IDE 0 */
		conf1 |= 0x00C2A1B3; /* Set 0, 1, 4, 5, 7, 8, 13, 15, 17, 22, 23 */
		break;	/* [한국어] 이중 함수 모델의 설정 완료. */

	case PCI_DEVICE_ID_JMICRON_JMB368:	/* [한국어] JMB368 - IDE 전용 모델. */
		/* The controller should be in single function IDE mode */
		conf1 |= 0x00C00000; /* Set 22, 23 */
		break;	/* [한국어] IDE 전용 모델의 설정 완료. */
	}

	pci_write_config_dword(pdev, 0x40, conf1);	/* [한국어] 바꾼 설정을 하드웨어에 쓴다. 이 쓰기로 함수 구성과 클래스 코드가 실제로 달라진다. */
	pci_write_config_dword(pdev, 0x80, conf5);	/* [한국어] 두 번째 설정 레지스터도 쓴다. */

	/* Update pdev accordingly */
	pci_read_config_byte(pdev, PCI_HEADER_TYPE, &hdr);	/* [한국어] 위 영어 주석대로 커널이 들고 있는 정보를 새 설정에 맞춰 갱신한다. PCI_HEADER_TYPE 은 오프셋 0x0e 다. */
	pdev->hdr_type = hdr & PCI_HEADER_TYPE_MASK;	/* [한국어] 헤더 타입의 하위 7비트가 실제 타입(0=일반, 1=PCI 브리지, 2=CardBus)이다. */
	pdev->multifunction = FIELD_GET(PCI_HEADER_TYPE_MFD, hdr);	/* [한국어] 최상위 비트(PCI_HEADER_TYPE_MFD)가 다기능 장치 표시다. 이 비트가 서야 스캔이 함수 1 도 훑는다. */

	pci_read_config_dword(pdev, PCI_CLASS_REVISION, &class);	/* [한국어] PCI_CLASS_REVISION(오프셋 0x08)에서 dword 를 읽으면 하위 8비트가 리비전, 상위 24비트가 클래스 코드다. */
	pdev->class = class >> 8;	/* [한국어] >>8 로 리비전을 떨어내 클래스 코드만 남긴다. */
}
/* [한국어] JMB36x 9종에 EARLY 단계로 등록한다 - 함수 1 이 스캔에 잡히려면
 * 반드시 스캔 전에 설정이 끝나야 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB360, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB361, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB362, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB363, quirk_jmicron_ata);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_JMICRON_JMB364, PCI_DEVICE_ID_JMICRON_JMB365,
 * PCI_DEVICE_ID_JMICRON_JMB366, PCI_DEVICE_ID_JMICRON_JMB368. 위 블록 주석의 설명이 그대로
 * 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB364, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB365, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB366, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB368, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB369, quirk_jmicron_ata);
/* [한국어] 재개 시에도 같은 설정을 다시 한다. 절전 중 레지스터가
 * 초기화되어 모드가 돌아가기 때문이다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB360, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB361, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB362, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB363, quirk_jmicron_ata);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_JMICRON_JMB364, PCI_DEVICE_ID_JMICRON_JMB365,
 * PCI_DEVICE_ID_JMICRON_JMB366, PCI_DEVICE_ID_JMICRON_JMB368. 위 블록 주석의 설명이 그대로
 * 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB364, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB365, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB366, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB368, quirk_jmicron_ata);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_JMICRON, PCI_DEVICE_ID_JMICRON_JMB369, quirk_jmicron_ata);

/* [한국어] CONFIG_ATA 조건부 블록의 끝. */
#endif

/*
 * [한국어]
 * quirk_jmicron_async_suspend - JMicron 다기능 장치의 비동기 절전을 끈다
 *
 * @dev: JMicron 스토리지 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] JMicron 의 IDE/AHCI 클래스 장치와 device 0x2362, 0x236f.
 * [무엇이 문제] 리눅스 전원관리는 성능을 위해 여러 장치를 병렬로(비동기)
 * 절전/재개시킨다. 그런데 이 칩의 두 함수는 전원이 켜지는 순서에 의존성이
 * 있어, 병렬로 처리하면 순서가 어긋나 문제가 생긴다. 아래 메시지가 그
 * 사정을 그대로 밝힌다.
 * [그대로 두면] 절전/재개 시 함수 간 전원 켜짐 순서가 뒤바뀌어 오동작한다.
 * [우회] device_disable_async_suspend() 로 이 장치를 동기(순차) 절전
 * 대상으로 되돌린다. 부팅/재개가 조금 느려지는 대신 순서가 보장된다.
 *
 * 실행 컨텍스트: FINAL 단계. multifunction 플래그가 확정된 뒤여야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_jmicron_async_suspend]
 *     -> device_disable_async_suspend()
 */
static void quirk_jmicron_async_suspend(struct pci_dev *dev)
{
	if (dev->multifunction) {	/* [한국어] 다기능 장치일 때만 문제가 된다 - 함수가 하나뿐이면 순서 의존이 없다. */
		device_disable_async_suspend(&dev->dev);	/* [한국어] 이 장치를 비동기 절전 대상에서 뺀다. */
		pci_info(dev, "async suspend disabled to avoid multi-function power-on ordering issue\n");	/* [한국어] 왜 껐는지 남긴다. */
	}
}
/* [한국어] JMicron 의 IDE 클래스 장치 전부(class_shift 8). */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_JMICRON, PCI_ANY_ID, PCI_CLASS_STORAGE_IDE, 8, quirk_jmicron_async_suspend);
/* [한국어] JMicron 의 AHCI 클래스 장치 전부. class_shift 0 이라 ProgIf 까지
 * 정확히 AHCI 인 것만 고른다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_JMICRON, PCI_ANY_ID, PCI_CLASS_STORAGE_SATA_AHCI, 0, quirk_jmicron_async_suspend);
/* [한국어] 클래스로 잡히지 않는 두 모델은 device ID 로 직접 건다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_JMICRON, 0x2362, quirk_jmicron_async_suspend);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_JMICRON, 0x236f, quirk_jmicron_async_suspend);

/* [한국어] 아래 quirk 는 IO-APIC 이 BAR 로 노출되는 x86 전용 상황을
 * 다루므로 IO-APIC 빌드에서만 컴파일한다. */
#ifdef CONFIG_X86_IO_APIC
/*
 * [한국어]
 * quirk_alder_ioapic - Alder(EESSC) 장치의 BAR 를 IO-APIC 용으로 정리한다
 *
 * @pdev: Intel EESSC 장치
 * @return: 없음
 *
 * [어떤 하드웨어] Intel EESSC(Alder) - 클래스 코드가 0xff00 인 장치.
 * [무엇이 문제] 이 장치의 BAR0 은 실제로는 IO-APIC 의 위치이고, 나머지
 * 다섯 BAR 에는 아래 영어 주석의 표현대로 쓰레기 값이 들어 있다.
 * [그대로 두면] 커널이 쓰레기 BAR 를 진짜 리소스로 믿고 배정하려다
 * 충돌을 일으키고, IO-APIC 영역은 리소스 트리에 등록되지 않아 다른
 * 장치가 그 주소를 가져갈 수 있다.
 * [우회] BAR0 은 리소스 트리에 강제로 끼워 넣어(insert_resource) 선점하고,
 * BAR1~5 는 통째로 0 으로 지워 없는 것으로 만든다.
 *
 * [왜 insert_resource 인가] IO-APIC 영역은 이미 fixmap 으로 매핑되어 있어
 * 커널이 건드리면 안 된다. 일반적인 할당 경로 대신 리소스 트리에 직접
 * 끼워 넣어 '이미 임자가 있다'고 표시하는 것이다.
 *
 * 실행 컨텍스트: HEADER 단계(리소스 할당 전).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_alder_ioapic] -> insert_resource()
 */
static void quirk_alder_ioapic(struct pci_dev *pdev)
{
	int i;	/* [한국어] BAR 번호 반복자. */

	if ((pdev->class >> 8) != 0xff00)	/* [한국어] 클래스 코드 상위 16비트가 0xff00 인 장치만 대상이다. 0xff 는 '분류되지 않음' 베이스 클래스다. */
		return;

	/*
	 * The first BAR is the location of the IO-APIC... we must
	 * not touch this (and it's already covered by the fixmap), so
	 * forcibly insert it into the resource tree.
	 */
	if (pci_resource_start(pdev, 0) && pci_resource_len(pdev, 0))	/* [한국어] BAR0 에 실제 주소와 길이가 있을 때만 등록한다. */
		insert_resource(&iomem_resource, &pdev->resource[0]);	/* [한국어] iomem_resource(시스템 MMIO 리소스 트리의 루트) 아래에 BAR0 을 직접 끼워 넣는다. */

	/*
	 * The next five BARs all seem to be rubbish, so just clean
	 * them out.
	 */
	for (i = 1; i < PCI_STD_NUM_BARS; i++)	/* [한국어] BAR1 부터 마지막 표준 BAR 까지. */
		memset(&pdev->resource[i], 0, sizeof(pdev->resource[i]));	/* [한국어] 리소스 구조체를 통째로 0 으로 지워 '없는 리소스'로 만든다. 이러면 할당 단계가 이 BAR 를 무시한다. */
}
/* [한국어] Intel EESSC 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_EESSC,	quirk_alder_ioapic);
/* [한국어] CONFIG_X86_IO_APIC 블록의 끝. */
#endif

/*
 * [한국어]
 * quirk_no_msi - 하드웨어 결함이 있는 장치에서 MSI 사용을 금지한다
 *
 * @dev: ATI 사우스브리지의 USB 컨트롤러 계열(0x4386~0x438b)
 * @return: 없음
 *
 * [어떤 하드웨어] ATI device 0x4386~0x438b.
 * [무엇이 문제] 이 장치들은 MSI(Message Signaled Interrupt) capability 를
 * 광고하지만 실제로는 제대로 동작하지 않는다. MSI 는 인터럽트를 별도의
 * 핀 대신 메모리 쓰기 트랜잭션으로 보내는 방식으로, capability 를 광고하면
 * 커널은 그것이 동작한다고 믿는다.
 * [그대로 두면] 인터럽트가 오지 않아 장치가 멈춘다.
 * [우회] dev->no_msi 를 1 로 세워 커널이 이 장치에 MSI 를 쓰지 않고 레거시
 * INTx 인터럽트로 물러나게 한다.
 *
 * [NVMe 와의 관계] NVMe 컨트롤러는 큐마다 인터럽트 벡터를 받기 위해 MSI-X 를
 * 쓴다(drivers/nvme/host/pci.c 의 pci_alloc_irq_vectors_affinity() 참조).
 * 그러나 이 quirk 가 걸린 장치는 NVMe 가 아니라 ATI 사우스브리지의 장치이며,
 * NVMe 컨트롤러에 이 플래그가 걸리는 일은 이 파일 안에 없다.
 *
 * 실행 컨텍스트: FINAL 단계. 드라이버가 MSI 를 요청하기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_no_msi]
 */
static void quirk_no_msi(struct pci_dev *dev)
{
	pci_info(dev, "avoiding MSI to work around a hardware defect\n");	/* [한국어] 왜 MSI 를 피하는지 사용자에게 알린다. */
	dev->no_msi = 1;	/* [한국어] PCI 코어의 MSI 할당 경로가 보는 플래그. 서 있으면 MSI 요청이 거부되고 INTx 로 물러난다. */
}
/* [한국어] MSI 가 깨진 ATI 장치 6종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4386, quirk_no_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4387, quirk_no_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4388, quirk_no_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4389, quirk_no_msi);
/* [한국어] 이어지는 등록 줄: 0x438a, 0x438b. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x438a, quirk_no_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x438b, quirk_no_msi);

/*
 * [한국어]
 * quirk_pcie_mch - MSI 가 동작하지 않는 메모리 컨트롤러 허브에서 MSI 를 금지
 *
 * @pdev: Intel E7520/E7320/E7525 MCH, 또는 Huawei 0x1610 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel E7520/E7320/E7525 메모리 컨트롤러 허브와 Huawei
 * device 0x1610(PCI 브리지 클래스).
 * [무엇이 문제] 이 칩들에서 MSI 가 제대로 동작하지 않는다. 구체적인 결함
 * 내용은 원본 주석에 없어 이 트리의 정보만으로는 확인할 수 없다.
 * [그대로 두면] 이 브리지 아래 경로의 MSI 인터럽트가 전달되지 않는다.
 * [우회] no_msi 플래그를 세운다. quirk_no_msi() 와 하는 일은 같지만
 * 로그를 남기지 않는 점만 다르다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_pcie_mch]
 */
static void quirk_pcie_mch(struct pci_dev *pdev)
{
	pdev->no_msi = 1;	/* [한국어] 이 장치에 MSI 를 쓰지 않도록 표시한다. */
}
/* [한국어] Intel MCH 3종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_E7520_MCH,	quirk_pcie_mch);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_E7320_MCH,	quirk_pcie_mch);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_E7525_MCH,	quirk_pcie_mch);

/* [한국어] Huawei 0x1610 은 PCI 브리지 클래스인 것만 대상이다. 같은
 * device ID 를 다른 클래스로 쓰는 변종을 건드리지 않기 위한 좁히기다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_HUAWEI, 0x1610, PCI_CLASS_BRIDGE_PCI, 8, quirk_pcie_mch);

/*
 * [한국어]
 * quirk_huawei_pcie_sva - PCI 인 척하는 AMBA 장치에 SMMU stall 을 허용한다
 *
 * @pdev: HiSilicon KunPeng920 / KunPeng930 의 가짜 PCI 장치
 * @return: 없음
 *
 * [어떤 하드웨어] HiSilicon KunPeng920/930, 리비전 0x21 또는 0x30.
 * [무엇이 문제] 아래 영어 주석대로, 이 장치들은 PCI 장치처럼 보이지만
 * 실제로는 AMBA 버스에 붙어 있다. RCiEP(Root Complex integrated Endpoint)
 * 로 분장한 평범한 SoC 장치인 셈이다.
 * [왜 stall 이 문제인가] 보통 PCI 장치에는 SMMU stall 을 켜면 안 된다.
 * PCI 는 쓰기가 막히지 않고 흘러야 한다는(free-flowing writes) 요구가
 * 있는데, stall 은 그것을 어겨 교착에 빠질 수 있기 때문이다. PCI 장치가
 * 결함 내성을 원하면 ATS 와 PRI 를 지원해야 하고, 그래서 ACPI 에는 그
 * 밖의 방식을 서술할 바인딩 자체가 없다.
 * [우회] 이 장치는 실제로는 PCI 가 아니므로 그 규칙이 적용되지 않는다.
 * dma-can-stall 속성을 붙여 SMMU stall 기반 SVA(Shared Virtual Addressing)
 * 를 쓸 수 있게 한다. 디바이스 트리 플랫폼은 직접 설정할 수 있으므로
 * ACPI 플랫폼을 위한 보정이다.
 *
 * 실행 컨텍스트: 아래 등록 단계 참조.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_huawei_pcie_sva]
 */
/*
 * HiSilicon KunPeng920 and KunPeng930 have devices appear as PCI but are
 * actually on the AMBA bus. These fake PCI devices can support SVA via
 * SMMU stall feature, by setting dma-can-stall for ACPI platforms.
 *
 * Normally stalling must not be enabled for PCI devices, since it would
 * break the PCI requirement for free-flowing writes and may lead to
 * deadlock.  We expect PCI devices to support ATS and PRI if they want to
 * be fault-tolerant, so there's no ACPI binding to describe anything else,
 * even when a "PCI" device turns out to be a regular old SoC device
 * dressed up as a RCiEP and normal rules don't apply.
 */
static void quirk_huawei_pcie_sva(struct pci_dev *pdev)
{
	struct property_entry properties[] = {	/* [한국어] 장치에 붙일 펌웨어 속성 목록. 지역 변수에 만들어 두고 아래에서 등록한다. */
		PROPERTY_ENTRY_BOOL("dma-can-stall"),	/* [한국어] dma-can-stall 불리언 속성 - IOMMU 계층이 이 속성을 보고 stall 을 허용한다. */
		{},	/* [한국어] 목록의 끝을 나타내는 빈 항목. */
	};

	if (pdev->revision != 0x21 && pdev->revision != 0x30)	/* [한국어] 리비전 0x21 과 0x30 만 대상이다. 다른 리비전은 사정이 다르다는 뜻이다. */
		return;

	pdev->pasid_no_tlp = 1;	/* [한국어] PASID 를 쓰는 TLP 를 보내지 않는 장치임을 표시한다 - 진짜 PCI 가 아니기 때문이다. */

	/*
	 * Set the dma-can-stall property on ACPI platforms. Device tree
	 * can set it directly.
	 */
	if (!pdev->dev.of_node &&	/* [한국어] 디바이스 트리 노드가 이미 있으면 그쪽에서 속성을 주므로 손대지 않는다. ACPI 플랫폼일 때만 소프트웨어 노드를 만들어 속성을 붙인다. */
	    device_create_managed_software_node(&pdev->dev, properties, NULL))	/* [한국어] device_create_managed_software_node() 는 장치 수명에 묶인 가짜 펌웨어 노드를 만들어 위 properties 를 붙인다. 0 이 아니면 실패다. */
		pci_warn(pdev, "could not add stall property");	/* [한국어] 실패해도 치명적이지 않으므로 경고만 남기고 계속한다 - SVA 를 못 쓸 뿐이다. */
}
/* [한국어] KunPeng 의 가짜 PCI 장치 6종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HUAWEI, 0xa250, quirk_huawei_pcie_sva);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HUAWEI, 0xa251, quirk_huawei_pcie_sva);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HUAWEI, 0xa255, quirk_huawei_pcie_sva);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HUAWEI, 0xa256, quirk_huawei_pcie_sva);
/* [한국어] 이어지는 등록 줄: 0xa258, 0xa259. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HUAWEI, 0xa258, quirk_huawei_pcie_sva);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HUAWEI, 0xa259, quirk_huawei_pcie_sva);

/*
 * [한국어]
 * quirk_pcie_pxh - PXH 기반 시스템에서 SHPC 장치의 MSI 를 끈다
 *
 * @dev: Intel PXH / PXHD / PXHV 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel PXH 계열 PCI Express-to-PCI-X 브리지.
 * [무엇이 문제] 아래 영어 주석대로, SHPC(Standard Hot-Plug Controller)와
 * ACPI 를 함께 쓰는 일부 PXH 기반 시스템에서 MSI 가 손상될 수 있다.
 * [그대로 두면] 잘못된 MSI 가 전달되어 엉뚱한 인터럽트 처리가 일어난다.
 * [우회] no_msi 를 세워 이 브리지에서 MSI 를 쓰지 않게 한다.
 *
 * [EARLY 단계인 이유] 다른 no_msi quirk 들이 FINAL 인 것과 달리 이것은
 * EARLY 다. 핫플러그 컨트롤러가 이른 시점에 MSI 를 잡을 수 있기 때문으로
 * 보이나, 그 근거가 원본 주석에 없어 이 트리의 정보만으로는 확인할 수 없다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_pcie_pxh]
 */
/*
 * It's possible for the MSI to get corrupted if SHPC and ACPI are used
 * together on certain PXH-based systems.
 */
static void quirk_pcie_pxh(struct pci_dev *dev)
{
	dev->no_msi = 1;	/* [한국어] 이 브리지에서 MSI 사용을 금지한다. */
	pci_warn(dev, "PXH quirk detected; SHPC device MSI disabled\n");	/* [한국어] 왜 껐는지 경고 수준으로 남긴다. */
}
/* [한국어] PXH 계열 5종에 EARLY 단계로 등록한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXHD_0,	quirk_pcie_pxh);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXHD_1,	quirk_pcie_pxh);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXH_0,	quirk_pcie_pxh);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXH_1,	quirk_pcie_pxh);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_PXHV. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXHV,	quirk_pcie_pxh);

/*
 * [한국어]
 * quirk_intel_pcie_pm - 하위 장치 전원관리가 불안한 Intel PCIe 칩셋 보정
 *
 * @dev: Intel PCIe 포트(0x25e2~0x260b 대역)
 * @return: 없음
 *
 * [어떤 하드웨어] 아래 등록표의 Intel PCI Express 칩셋 21종.
 * [무엇이 문제] 아래 영어 주석대로 하위(downstream) 장치의 전원관리에
 * 문제가 있다. 구체적 증상은 원본 주석에 없어 이 트리의 정보만으로는
 * 확인할 수 없다. 코드가 하는 일로 보아 (1) D3hot 에서 D0 로 올라온 뒤
 * 장치가 준비되기까지 시간이 더 걸리고, (2) D1/D2 중간 절전 상태가
 * 제대로 동작하지 않는다.
 * [그대로 두면] 전원 상태를 바꾼 직후 장치에 접근해 실패한다.
 * [우회] D3hot 복귀 후 대기 시간을 120ms 로 늘리고, D1/D2 사용을 금지한다.
 *
 * [★ 전역을 바꾸는 부작용] pci_pm_d3hot_delay 는 이 장치만의 값이 아니라
 * PCI 서브시스템 전역이다. 즉 이 칩셋이 하나라도 있으면 시스템의 모든
 * 장치가 120ms 대기를 하게 된다. 다음 함수 quirk_d3hot_delay() 가 장치별
 * dev->d3hot_delay 를 쓰는 것과 대조된다.
 *
 * [NVMe 와의 관계] NVMe 컨트롤러도 D3 절전을 쓰므로 이 전역 지연의 영향을
 * 받는다. 다만 NVMe 의 APST(Autonomous Power State Transition)는 컨트롤러
 * 내부의 NVMe 전원 상태 전환이라 PCI 의 D 상태와는 다른 층위다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_intel_pcie_pm]
 */
/*
 * Some Intel PCI Express chipsets have trouble with downstream device
 * power management.
 */
static void quirk_intel_pcie_pm(struct pci_dev *dev)
{
	pci_pm_d3hot_delay = 120;	/* [한국어] PCI 전역 - D3hot 에서 D0 로 돌아온 뒤 config 접근까지 기다릴 밀리초. 스펙 기본값보다 크게 잡아 안전을 산다. */
	dev->no_d1d2 = 1;	/* [한국어] D1/D2 중간 절전 상태를 쓰지 않도록 이 장치에 표시한다. */
}
/* [한국어] 문제 있는 Intel PCIe 칩셋 21종에 FINAL 단계로 등록한다.
 * 이름 있는 상수가 없어 device ID 숫자를 그대로 나열한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25e2, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25e3, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25e4, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25e5, quirk_intel_pcie_pm);
/* [한국어] 이어지는 등록 줄: 0x25e6, 0x25e7, 0x25f7, 0x25f8. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25e6, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25e7, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25f7, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25f8, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25f9, quirk_intel_pcie_pm);
/* [한국어] 이어지는 등록 줄: 0x25fa, 0x2601, 0x2602, 0x2603. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x25fa, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2601, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2602, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2603, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2604, quirk_intel_pcie_pm);
/* [한국어] 이어지는 등록 줄: 0x2605, 0x2606, 0x2607, 0x2608. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2605, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2606, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2607, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2608, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2609, quirk_intel_pcie_pm);
/* [한국어] 이어지는 등록 줄: 0x260a, 0x260b. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x260a, quirk_intel_pcie_pm);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x260b, quirk_intel_pcie_pm);

/*
 * [한국어]
 * quirk_d3hot_delay - 장치별 D3hot 복귀 대기 시간을 늘리는 공용 헬퍼
 *
 * @dev: 대상 장치
 * @delay: 보장하고 싶은 최소 대기 시간(밀리초)
 * @return: 없음
 *
 * [왜 필요한가] PCI 전원관리 스펙은 D3hot 에서 D0 로 돌아온 뒤 일정 시간이
 * 지나야 Configuration Space 에 접근할 수 있다고 정한다. 그 시간이 스펙
 * 기본값보다 더 필요한 장치들이 있어, 아래 여러 quirk 가 이 헬퍼로 값을
 * 키운다.
 *
 * [값을 줄이지 않는 이유] 이미 더 큰 값이 설정되어 있으면 그대로 둔다.
 * 여러 quirk 가 같은 장치에 걸릴 수 있는데, 더 보수적인(긴) 값이 이겨야
 * 안전하기 때문이다.
 *
 * [전역이 아니라 장치별] 위 quirk_intel_pcie_pm() 과 달리 dev->d3hot_delay
 * 만 건드리므로 다른 장치의 부팅/재개 속도에 영향을 주지 않는다.
 *
 * 실행 컨텍스트: 여러 quirk 함수 안, 주로 FINAL 단계.
 *
 * 호출 체인:
 *   quirk_radeon_pm() / quirk_nvidia_hda_pm() / quirk_ryzen_xhci_d3hot() 등
 *     -> [quirk_d3hot_delay]
 */
static void quirk_d3hot_delay(struct pci_dev *dev, unsigned int delay)
{
	if (dev->d3hot_delay >= delay)	/* [한국어] 이미 요구치 이상이면 줄이지 않는다 - 더 긴 대기가 늘 안전한 쪽이다. */
		return;

	dev->d3hot_delay = delay;	/* [한국어] 이 장치에만 적용되는 대기 시간을 늘린다. */
	pci_info(dev, "extending delay after power-on from D3hot to %d msec\n",	/* [한국어] 얼마로 늘렸는지 남긴다. */
		 dev->d3hot_delay);
}

/*
 * [한국어]
 * quirk_radeon_pm - 애플 보드의 특정 Radeon 에 D3hot 복귀 지연을 준다
 *
 * @dev: ATI device 0x6741 (Radeon)
 * @return: 없음
 *
 * [어떤 하드웨어] ATI Radeon 0x6741 중 서브벤더가 Apple 이고 서브디바이스가
 * 0x00e2 인 것 - 즉 특정 애플 기기에 들어간 변종만 해당한다.
 * [무엇이 문제] 이 quirk 에는 원본 영어 주석이 없다. D3hot 복귀 후 20ms 를
 * 더 기다리게 하는 것으로 보아 전원 복귀가 느린 것이지만, 구체적인 증상은
 * 이 트리의 정보만으로는 확인할 수 없다.
 * [우회] quirk_d3hot_delay() 로 대기 시간을 20ms 로 올린다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_radeon_pm] -> quirk_d3hot_delay()
 */
static void quirk_radeon_pm(struct pci_dev *dev)
{
	if (dev->subsystem_vendor == PCI_VENDOR_ID_APPLE &&	/* [한국어] 서브벤더가 애플인지 확인한다 - 같은 GPU 라도 다른 보드에서는 문제가 없다. */
	    dev->subsystem_device == 0x00e2)	/* [한국어] 서브디바이스까지 맞아야 그 애플 기기의 변종이다. */
		quirk_d3hot_delay(dev, 20);	/* [한국어] D3hot 복귀 대기를 20ms 로 늘린다. */
}
/* [한국어] ATI 0x6741 에 FINAL 단계로 등록한다. 애플 보드 여부는 함수 안에서 판정한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x6741, quirk_radeon_pm);

/*
 * [한국어]
 * quirk_nvidia_hda_pm - NVIDIA HDA 컨트롤러의 D3hot 복귀 지연을 되돌린다
 *
 * @dev: NVIDIA 의 HD Audio 클래스 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] NVIDIA GPU 에 딸린 HD Audio 컨트롤러 전부(클래스 매칭).
 * [무엇이 문제] 아래 영어 주석대로, Ampere 기반 HDA 컨트롤러는 D0 로
 * 전환한 직후 너무 빨리 버스 리셋을 걸면 장치 전체가 먹통이 된다.
 * [그대로 두면] 리셋 타이밍에 따라 GPU 와 오디오가 함께 응답하지 않는다.
 * [우회] d3hot_delay 를 20ms 로 늘려 예전의 기본값 수준으로 되돌린다.
 * 특정 모델을 가려내는 대신 NVIDIA 의 모든 HDA 컨트롤러에 적용한다.
 *
 * 실행 컨텍스트: FINAL 단계, 클래스 매칭(class_shift 8).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nvidia_hda_pm] -> quirk_d3hot_delay()
 */
/*
 * NVIDIA Ampere-based HDA controllers can wedge the whole device if a bus
 * reset is performed too soon after transition to D0, extend d3hot_delay
 * to previous effective default for all NVIDIA HDA controllers.
 */
static void quirk_nvidia_hda_pm(struct pci_dev *dev)
{
	quirk_d3hot_delay(dev, 20);	/* [한국어] D3hot 복귀 대기를 20ms 로 늘린다. */
}
/* [한국어] NVIDIA 벤더의 HD Audio 클래스 장치 전부에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID,
			      PCI_CLASS_MULTIMEDIA_HD_AUDIO, 8,
			      quirk_nvidia_hda_pm);

/*
 * [한국어]
 * quirk_ryzen_xhci_d3hot - Ryzen 의 xHCI 컨트롤러에 D3hot 복귀 지연을 준다
 *
 * @dev: AMD device 0x15e0 / 0x15e1 / 0x1639 (Ryzen5/7 의 xHCI)
 * @return: 없음
 *
 * [어떤 하드웨어] AMD Ryzen5/Ryzen7 플랫폼의 xHCI USB 컨트롤러.
 * [무엇이 문제] 아래 영어 주석과 버그질라(205587)가 밝히는 대로, 런타임
 * 절전이나 s2idle 에서 깨어날 때 실패한다. 커널은 이 장치를 D3cold 로
 * 내리려 하지만 해당 플랫폼에서는 그것이 먹히지 않아 장치가 사실상
 * D3hot 에 머문다. 그 상태에서 D0 로 올라오려면 더 긴 대기가 필요하다.
 * [그대로 두면] 절전 복귀 후 USB 컨트롤러가 살아나지 않는다.
 * [우회] d3hot_delay 를 20ms 로 늘린다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_ryzen_xhci_d3hot] -> quirk_d3hot_delay()
 */
/*
 * Ryzen5/7 XHCI controllers fail upon resume from runtime suspend or s2idle.
 * https://bugzilla.kernel.org/show_bug.cgi?id=205587
 *
 * The kernel attempts to transition these devices to D3cold, but that seems
 * to be ineffective on the platforms in question; the PCI device appears to
 * remain on in D3hot state. The D3hot-to-D0 transition then requires an
 * extended delay in order to succeed.
 */
static void quirk_ryzen_xhci_d3hot(struct pci_dev *dev)
{
	quirk_d3hot_delay(dev, 20);	/* [한국어] D3hot 복귀 대기를 20ms 로 늘린다. */
}
/* [한국어] Ryzen xHCI 3종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD, 0x15e0, quirk_ryzen_xhci_d3hot);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD, 0x15e1, quirk_ryzen_xhci_d3hot);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD, 0x1639, quirk_ryzen_xhci_d3hot);

/* [한국어] 아래 부트 인터럽트 재라우팅 quirk 는 x86 IO-APIC 전용이다. */
#ifdef CONFIG_X86_IO_APIC
/*
 * [한국어]
 * dmi_disable_ioapicreroute - 특정 보드에서 부트 인터럽트 재라우팅을 끄는 DMI 콜백
 *
 * @d: 매칭된 DMI 항목(보드 이름 등이 들어 있다)
 * @return: 항상 0 (DMI 콜백 규약)
 *
 * 아래 quirk_reroute_to_boot_interrupts_intel() 이 적용하는 재라우팅이
 * 오히려 해가 되는 보드가 있다. 그런 보드를 DMI(메인보드 식별 정보)로
 * 알아내 전역 noioapicreroute 를 세운다.
 *
 * 실행 컨텍스트: dmi_check_system() 안에서 호출되는 콜백.
 *
 * 호출 체인:
 *   quirk_reroute_to_boot_interrupts_intel() -> dmi_check_system()
 *     -> [dmi_disable_ioapicreroute]
 */
static int dmi_disable_ioapicreroute(const struct dmi_system_id *d)
{
	noioapicreroute = 1;	/* [한국어] 재라우팅을 하지 말라는 전역 플래그를 세운다. */
	pr_info("%s detected: disable boot interrupt reroute\n", d->ident);	/* [한국어] 어떤 보드 때문에 껐는지 남긴다. d->ident 가 아래 표에 적힌 사람이 읽을 수 있는 이름이다. */

	return 0;	/* [한국어] DMI 콜백은 0 을 반환해야 이후 항목도 계속 검사한다. */
}

/* [한국어] 부트 인터럽트 재라우팅에서 제외할 보드 목록.
 * 설정자: 컴파일 시 고정된 상수 표.
 * 읽는 자: quirk_reroute_to_boot_interrupts_intel() 이 dmi_check_system() 으로 훑는다.
 * 값 범위: 마지막에 빈 항목 {} 이 와야 표의 끝으로 인식된다.
 * 동기화: const 이므로 경쟁이 없다. */
static const struct dmi_system_id boot_interrupt_dmi_table[] = {
	/*
	 * Systems to exclude from boot interrupt reroute quirks
	 */
	{
		.callback = dmi_disable_ioapicreroute,	/* [한국어] 이 보드가 매칭되면 부를 함수. */
		.ident = "ASUSTek Computer INC. M2N-LR",	/* [한국어] 로그에 찍을 사람이 읽을 수 있는 보드 이름. */
		.matches = {	/* [한국어] 아래 조건이 모두 맞아야 이 항목이 매칭된다. */
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTek Computer INC."),	/* [한국어] DMI 의 시스템 제조사 문자열이 정확히 일치해야 한다. */
			DMI_MATCH(DMI_PRODUCT_NAME, "M2N-LR"),	/* [한국어] 제품 이름도 일치해야 한다. */
		},
	},
	{}	/* [한국어] 표의 끝을 나타내는 빈 항목. */
};

/*
 * [한국어]
 * quirk_reroute_to_boot_interrupts_intel - 끌 수 없는 부트 인터럽트에 맞춰 IRQ 를 재배치
 *
 * @dev: Intel 80332/80333/ESB2/PXH 계열 브리지
 * @return: 없음
 *
 * [부트 인터럽트란] 레거시 PCI INTx 인터럽트를 IO-APIC 으로 보내도록
 * 설정해 두어도, 일부 칩셋은 같은 인터럽트를 예전 방식의 '부트 인터럽트'
 * 선으로도 함께 내보낸다. 원래는 부팅 초기에 IO-APIC 이 설정되기 전을
 * 위한 기능이다.
 * [무엇이 문제] 아래 영어 주석대로 일부 칩셋에서는 이 부트 인터럽트를
 * 끌 수가 없다.
 * [그대로 두면] 인터럽트가 원래 선과 부트 인터럽트 선 양쪽으로 올라오는데,
 * 부트 인터럽트 선에는 처리기가 없어 처리되지 않은 인터럽트가 쌓인다.
 * [우회] 커널의 인터럽트 처리기를 원래 선이 아니라 부트 인터럽트 선에
 * 설치하도록 표시한다(irq_reroute_variant). 끌 수 없다면 그쪽을 쓰자는
 * 발상이다.
 *
 * [예외 보드] 이 재라우팅이 해가 되는 보드가 있어, DMI 표를 먼저 확인해
 * 제외 대상이면 아무것도 하지 않는다. 부팅 파라미터 noioapicquirk /
 * noioapicreroute 로도 끌 수 있다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_reroute_to_boot_interrupts_intel]
 *     -> dmi_check_system()
 */
/*
 * Boot interrupts on some chipsets cannot be turned off. For these chipsets,
 * remap the original interrupt in the Linux kernel to the boot interrupt, so
 * that a PCI device's interrupt handler is installed on the boot interrupt
 * line instead.
 */
static void quirk_reroute_to_boot_interrupts_intel(struct pci_dev *dev)
{
	dmi_check_system(boot_interrupt_dmi_table);	/* [한국어] 제외 보드 목록을 확인한다. 매칭되면 콜백이 noioapicreroute 를 세운다. */
	if (noioapicquirk || noioapicreroute)	/* [한국어] 사용자가 부팅 파라미터로 껐거나 위 DMI 검사가 제외 보드로 판정했으면 아무것도 하지 않는다. */
		return;

	dev->irq_reroute_variant = INTEL_IRQ_REROUTE_VARIANT;	/* [한국어] 이 장치의 인터럽트를 부트 인터럽트 선으로 재배치하라는 표시. IO-APIC 설정 코드가 이 값을 본다. */
	pci_info(dev, "rerouting interrupts for [%04x:%04x]\n",	/* [한국어] 어느 장치에 대해 재배치했는지 vendor:device 와 함께 남긴다. */
		 dev->vendor, dev->device);
}
/* [한국어] 부트 인터럽트를 끌 수 없는 Intel 브리지 8종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80333_0,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80333_1,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ESB2_0,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXH_0,	quirk_reroute_to_boot_interrupts_intel);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_PXH_1, PCI_DEVICE_ID_INTEL_PXHV,
 * PCI_DEVICE_ID_INTEL_80332_0, PCI_DEVICE_ID_INTEL_80332_1. 위 블록 주석의 설명이 그대로
 * 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXH_1,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXHV,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80332_0,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80332_1,	quirk_reroute_to_boot_interrupts_intel);
/* [한국어] 재개 시에도 같은 재배치를 다시 적용한다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80333_0,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80333_1,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ESB2_0,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXH_0,	quirk_reroute_to_boot_interrupts_intel);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_PXH_1, PCI_DEVICE_ID_INTEL_PXHV,
 * PCI_DEVICE_ID_INTEL_80332_0, PCI_DEVICE_ID_INTEL_80332_1. 위 블록 주석의 설명이 그대로
 * 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXH_1,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_PXHV,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80332_0,	quirk_reroute_to_boot_interrupts_intel);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_80332_1,	quirk_reroute_to_boot_interrupts_intel);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/*
 * On some chipsets we can disable the generation of legacy INTx boot
 * interrupts.
 */

/*
 * IO-APIC1 on 6300ESB generates boot interrupts, see Intel order no
 * 300641-004US, section 5.7.3.
 *
 * Core IO on Xeon E5 1600/2600/4600, see Intel order no 326509-003.
 * Core IO on Xeon E5 v2, see Intel order no 329188-003.
 * Core IO on Xeon E7 v2, see Intel order no 329595-002.
 * Core IO on Xeon E5 v3, see Intel order no 330784-003.
 * Core IO on Xeon E7 v3, see Intel order no 332315-001US.
 * Core IO on Xeon E5 v4, see Intel order no 333810-002US.
 * Core IO on Xeon E7 v4, see Intel order no 332315-001US.
 * Core IO on Xeon D-1500, see Intel order no 332051-001.
 * Core IO on Xeon Scalable, see Intel order no 610950.
 */
/* [한국어] 아래 4개 매크로는 Intel 칩셋에서 부트 인터럽트를 끄는 데 쓰는
 * 벤더 전용 레지스터 오프셋과 비트다. 옆의 영어 주석에 어느 버스/디바이스/
 * 함수의 레지스터인지 적혀 있고, 위 영어 주석에는 그 값의 출처가 되는
 * 인텔 데이터시트 주문 번호가 나열되어 있다. */
#define INTEL_6300_IOAPIC_ABAR		0x40	/* Bus 0, Dev 29, Func 5 */
#define INTEL_6300_DISABLE_BOOT_IRQ	(1<<14)	/* [한국어] ABAR 의 비트 14 가 부트 인터럽트 비활성 비트다. */

#define INTEL_CIPINTRC_CFG_OFFSET	0x14C	/* Bus 0, Dev 5, Func 0 */
#define INTEL_CIPINTRC_DIS_INTX_ICH	(1<<25)	/* [한국어] CIPINTRC 의 비트 25 가 ICH 로 가는 INTx 를 끄는 비트다. */

/*
 * [한국어]
 * quirk_disable_intel_boot_interrupt - Intel 칩셋에서 부트 인터럽트를 끈다
 *
 * @dev: Intel ESB IO-APIC(Dev 29 Func 5) 또는 Xeon 계열 Core IO 허브(Dev 5 Func 0)
 * @return: 없음
 *
 * [부트 인터럽트란] 레거시 INTx 인터럽트를 IO-APIC 으로 보내도록 설정해도
 * 칩셋이 같은 인터럽트를 옛 경로로도 함께 내보내는 기능이다. IO-APIC 이
 * 설정되기 전의 부팅 초기를 위한 것이라 그 뒤에는 방해만 된다.
 * [그대로 두면] 처리기가 없는 선으로 인터럽트가 올라와 '처리되지 않은
 * 인터럽트' 로그가 쌓이거나 인터럽트 폭주가 일어난다.
 * [우회] 위 quirk_reroute_to_boot_interrupts_intel() 이 '끌 수 없는' 칩셋을
 * 위한 회피였다면, 이 함수는 '끌 수 있는' 칩셋에서 실제로 꺼 버린다.
 * 칩 세대에 따라 레지스터가 달라 device ID 로 갈라 처리한다.
 *
 * [부팅 파라미터] noioapicquirk 가 주어지면 아무것도 하지 않는다 - 사용자가
 * 이런 종류의 자동 보정을 원치 않는 경우다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_intel_boot_interrupt]
 *     -> pci_write_config_word() 또는 pci_write_config_dword()
 */
static void quirk_disable_intel_boot_interrupt(struct pci_dev *dev)
{
	u16 pci_config_word;	/* [한국어] ESB 세대에서 쓰는 16비트 레지스터 값. */
	u32 pci_config_dword;	/* [한국어] Xeon Core IO 세대에서 쓰는 32비트 레지스터 값. */

	if (noioapicquirk)	/* [한국어] 사용자가 부팅 파라미터로 이 보정을 껐으면 아무것도 하지 않는다. */
		return;

	switch (dev->device) {	/* [한국어] 칩 세대마다 레지스터 위치와 폭이 달라 device ID 로 가른다. */
	case PCI_DEVICE_ID_INTEL_ESB_10:	/* [한국어] ESB(6300ESB) 세대 - IO-APIC 의 ABAR 레지스터를 쓴다. */
		pci_read_config_word(dev, INTEL_6300_IOAPIC_ABAR,	/* [한국어] ABAR(오프셋 0x40)를 읽는다. */
				     &pci_config_word);
		pci_config_word |= INTEL_6300_DISABLE_BOOT_IRQ;	/* [한국어] 비트 14 를 세우면 부트 인터럽트가 꺼진다. */
		pci_write_config_word(dev, INTEL_6300_IOAPIC_ABAR,	/* [한국어] 고친 값을 되쓴다. */
				      pci_config_word);
		break;
	case 0x3c28:	/* Xeon E5 1600/2600/4600	*/
	case 0x0e28:	/* Xeon E5/E7 V2		*/
	case 0x2f28:	/* Xeon E5/E7 V3,V4		*/
	case 0x6f28:	/* Xeon D-1500			*/
	case 0x2034:	/* Xeon Scalable Family		*/
		pci_read_config_dword(dev, INTEL_CIPINTRC_CFG_OFFSET,	/* [한국어] Xeon 세대 - Core IO 허브의 CIPINTRC(오프셋 0x14C)를 읽는다. */
				      &pci_config_dword);
		pci_config_dword |= INTEL_CIPINTRC_DIS_INTX_ICH;	/* [한국어] 비트 25 를 세워 ICH 로 가는 INTx 전달을 끊는다. */
		pci_write_config_dword(dev, INTEL_CIPINTRC_CFG_OFFSET,	/* [한국어] 고친 값을 되쓴다. */
				       pci_config_dword);
		break;
	default:	/* [한국어] 표에 없는 device ID - 어떤 레지스터를 써야 할지 모르므로 아무것도 하지 않는다. */
		return;
	}
	pci_info(dev, "disabled boot interrupts on device [%04x:%04x]\n",	/* [한국어] 어느 장치에서 껐는지 vendor:device 와 함께 남긴다. */
		 dev->vendor, dev->device);
}
/*
 * Device 29 Func 5 Device IDs of IO-APIC
 * containing ABAR—APIC1 Alternate Base Address Register
 */
/* [한국어] ESB 세대 IO-APIC 에 FINAL 단계로 등록한다. 위 영어 주석이
 * 이 장치가 Dev 29 Func 5 의 IO-APIC 이며 ABAR 를 갖고 있다고 밝힌다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ESB_10,
		quirk_disable_intel_boot_interrupt);
/* [한국어] 재개 시에도 다시 끈다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	PCI_DEVICE_ID_INTEL_ESB_10,
		quirk_disable_intel_boot_interrupt);

/*
 * Device 5 Func 0 Device IDs of Core IO modules/hubs
 * containing Coherent Interface Protocol Interrupt Control
 *
 * Device IDs obtained from volume 2 datasheets of commented
 * families above.
 */
/* [한국어] Xeon 세대 Core IO 허브 5종에 FINAL 단계로 등록한다. 위 영어
 * 주석대로 이 device ID 들은 각 제품군 데이터시트 2권에서 가져온 것이다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x3c28,
		quirk_disable_intel_boot_interrupt);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x0e28,
		quirk_disable_intel_boot_interrupt);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2f28,
		quirk_disable_intel_boot_interrupt);
/* [한국어] 이어지는 등록 줄: 0x6f28, 0x2034. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x6f28,
		quirk_disable_intel_boot_interrupt);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL,	0x2034,
		quirk_disable_intel_boot_interrupt);
/* [한국어] 같은 5종을 재개 단계에도 등록한다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	0x3c28,
		quirk_disable_intel_boot_interrupt);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	0x0e28,
		quirk_disable_intel_boot_interrupt);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	0x2f28,
		quirk_disable_intel_boot_interrupt);
/* [한국어] 이어지는 등록 줄: 0x6f28, 0x2034. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	0x6f28,
		quirk_disable_intel_boot_interrupt);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_INTEL,	0x2034,
		quirk_disable_intel_boot_interrupt);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/* Disable boot interrupts on HT-1000 */
/* [한국어] 아래 4개 매크로는 Broadcom/ServerWorks HT-1000 사우스브리지에서
 * 부트 인터럽트를 끄기 위한 값이다. 이 칩은 Configuration Space 가 아니라
 * IO 포트 기반의 색인/데이터 쌍으로 인터럽트 매핑 표에 접근한다. */
#define BC_HT1000_FEATURE_REG		0x64	/* [한국어] 기능 제어 레지스터의 Configuration Space 오프셋. */
#define BC_HT1000_PIC_REGS_ENABLE	(1<<0)	/* [한국어] 그 레지스터의 비트 0 - PIC 레지스터 창을 IO 공간에 열어 주는 비트다. */
#define BC_HT1000_MAP_IDX		0xC00	/* [한국어] 인터럽트 매핑 표의 색인 포트(IO 주소). */
#define BC_HT1000_MAP_DATA		0xC01	/* [한국어] 인터럽트 매핑 표의 데이터 포트(IO 주소). */

/*
 * [한국어]
 * quirk_disable_broadcom_boot_interrupt - HT-1000 의 부트 인터럽트 매핑을 모두 지운다
 *
 * @dev: ServerWorks HT-1000 사우스브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Broadcom/ServerWorks HT-1000 사우스브리지.
 * [무엇이 문제] 위 Intel 사례와 같은 부트 인터럽트 문제다.
 * [우회] 이 칩에는 부트 인터럽트를 끄는 단일 비트가 없다. 대신 인터럽트
 * 매핑 표의 32개 항목을 모두 0 으로 지워 어디로도 라우팅되지 않게 만든다.
 * 그 표는 Configuration Space 가 아니라 IO 포트 0xC00(색인)/0xC01(데이터)
 * 쌍으로 접근하며, 그 창은 평소 닫혀 있어 먼저 열어야 한다.
 *
 * [순서가 중요한 이유] (1) 기능 레지스터의 원래 값을 저장하고 창을 연다,
 * (2) 표를 지운다, (3) 저장해 둔 원래 값을 되써서 창을 닫는다. 창을 열어
 * 둔 채로 두면 다른 코드가 그 IO 포트를 건드릴 수 있다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_broadcom_boot_interrupt]
 *     -> outb()
 */
static void quirk_disable_broadcom_boot_interrupt(struct pci_dev *dev)
{
	u32 pci_config_dword;	/* [한국어] 기능 제어 레지스터의 원래 값 - 끝에 되돌려야 한다. */
	u8 irq;	/* [한국어] 인터럽트 매핑 표를 훑을 색인. */

	if (noioapicquirk)	/* [한국어] 사용자가 부팅 파라미터로 껐으면 아무것도 하지 않는다. */
		return;

	pci_read_config_dword(dev, BC_HT1000_FEATURE_REG, &pci_config_dword);	/* [한국어] 기능 제어 레지스터의 현재 값을 저장한다. */
	pci_write_config_dword(dev, BC_HT1000_FEATURE_REG, pci_config_dword |	/* [한국어] 비트 0 을 세워 PIC 레지스터 창을 IO 공간에 연다. 저장해 둔 값 자체는 바뀌지 않는다. */
			BC_HT1000_PIC_REGS_ENABLE);

	for (irq = 0x10; irq < 0x10 + 32; irq++) {	/* [한국어] 인터럽트 매핑 표의 32개 항목(색인 0x10~0x2f)을 훑는다. */
		outb(irq, BC_HT1000_MAP_IDX);	/* [한국어] 색인 포트에 항목 번호를 쓴다 - 색인/데이터 방식의 첫 단계다. */
		outb(0x00, BC_HT1000_MAP_DATA);	/* [한국어] 데이터 포트에 0 을 써서 그 항목의 라우팅을 없앤다. */
	}

	pci_write_config_dword(dev, BC_HT1000_FEATURE_REG, pci_config_dword);	/* [한국어] 저장해 둔 원래 값을 되써서 PIC 레지스터 창을 다시 닫는다. */

	pci_info(dev, "disabled boot interrupts on device [%04x:%04x]\n",	/* [한국어] 어느 장치에서 껐는지 남긴다. */
		 dev->vendor, dev->device);
}
/* [한국어] HT-1000 사우스브리지에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SERVERWORKS,   PCI_DEVICE_ID_SERVERWORKS_HT1000SB,	quirk_disable_broadcom_boot_interrupt);
/* [한국어] 재개 시에도 다시 지운다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_SERVERWORKS,   PCI_DEVICE_ID_SERVERWORKS_HT1000SB,	quirk_disable_broadcom_boot_interrupt);

/* Disable boot interrupts on AMD and ATI chipsets */

/*
 * NOIOAMODE needs to be disabled to disable "boot interrupts". For AMD 8131
 * rev. A0 and B0, NOIOAMODE needs to be disabled anyway to fix IO-APIC mode
 * (due to an erratum).
 */
/* [한국어] 아래 4개 매크로는 AMD 813x HyperTransport 터널에서 부트
 * 인터럽트를 끄기 위한 값이다. 위 영어 주석대로 NOIOAMODE 비트를 꺼야
 * 부트 인터럽트가 꺼지며, 8131 의 A0/B0 리비전에서는 에라타 때문에
 * IO-APIC 모드를 고치려면 어차피 이 비트를 꺼야 한다. */
#define AMD_813X_MISC			0x40	/* [한국어] MISC 레지스터의 Configuration Space 오프셋. */
#define AMD_813X_NOIOAMODE		(1<<0)	/* [한국어] 그 레지스터의 비트 0 - NOIOAMODE. 이 비트를 꺼야 부트 인터럽트가 사라진다. */
#define AMD_813X_REV_B1			0x12	/* [한국어] 리비전 B1 의 값. 아래에서 제외 대상으로 쓰인다. */
#define AMD_813X_REV_B2			0x13	/* [한국어] 리비전 B2 의 값. 마찬가지로 제외 대상이다. */

/*
 * [한국어]
 * quirk_disable_amd_813x_boot_interrupt - AMD 813x 에서 부트 인터럽트를 끈다
 *
 * @dev: AMD 8131 또는 8132 HyperTransport PCI-X 터널
 * @return: 없음
 *
 * [어떤 하드웨어] AMD 8131 / 8132 브리지. 단 리비전 B1(0x12)과 B2(0x13)는
 * 제외한다.
 * [무엇이 문제] 위 Intel/Broadcom 사례와 같은 부트 인터럽트 문제다.
 * [우회] MISC 레지스터의 NOIOAMODE 비트를 끈다.
 * [B1/B2 를 제외하는 이유] 원본 주석에 그 이유가 적혀 있지 않다. 위
 * 영어 주석은 A0/B0 에서는 에라타 때문에 어차피 꺼야 한다고만 말할 뿐,
 * B1/B2 를 왜 건너뛰는지는 이 트리의 정보만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_amd_813x_boot_interrupt]
 *     -> pci_write_config_dword()
 */
static void quirk_disable_amd_813x_boot_interrupt(struct pci_dev *dev)
{
	u32 pci_config_dword;	/* [한국어] MISC 레지스터 값. */

	if (noioapicquirk)	/* [한국어] 사용자가 부팅 파라미터로 껐으면 아무것도 하지 않는다. */
		return;
	if ((dev->revision == AMD_813X_REV_B1) ||	/* [한국어] 리비전 B1 이면 건드리지 않는다. */
	    (dev->revision == AMD_813X_REV_B2))	/* [한국어] 리비전 B2 도 마찬가지다. */
		return;

	pci_read_config_dword(dev, AMD_813X_MISC, &pci_config_dword);	/* [한국어] MISC 레지스터(0x40)를 읽는다. */
	pci_config_dword &= ~AMD_813X_NOIOAMODE;	/* [한국어] NOIOAMODE 비트를 끈다 - 이 비트가 꺼져야 IO-APIC 모드가 되고 부트 인터럽트가 사라진다. */
	pci_write_config_dword(dev, AMD_813X_MISC, pci_config_dword);	/* [한국어] 고친 값을 되쓴다. */

	pci_info(dev, "disabled boot interrupts on device [%04x:%04x]\n",	/* [한국어] 어느 장치에서 껐는지 남긴다. */
		 dev->vendor, dev->device);
}
/* [한국어] AMD 8131 에 FINAL 과 RESUME 을 짝지어 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_8131_BRIDGE,	quirk_disable_amd_813x_boot_interrupt);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_8131_BRIDGE,	quirk_disable_amd_813x_boot_interrupt);
/* [한국어] AMD 8132 에도 같은 쌍을 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_8132_BRIDGE,	quirk_disable_amd_813x_boot_interrupt);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_AMD,	PCI_DEVICE_ID_AMD_8132_BRIDGE,	quirk_disable_amd_813x_boot_interrupt);

/* [한국어] AMD 8111 사우스브리지의 PCI IRQ 라우팅 레지스터 오프셋.
 * 이 레지스터가 0 이면 어떤 인터럽트도 옛 경로로 나가지 않는다. */
#define AMD_8111_PCI_IRQ_ROUTING	0x56

/*
 * [한국어]
 * quirk_disable_amd_8111_boot_interrupt - AMD 8111 의 부트 인터럽트 라우팅을 지운다
 *
 * @dev: AMD 8111 사우스브리지의 SMBus 함수
 * @return: 없음
 *
 * [어떤 하드웨어] AMD 8111 사우스브리지.
 * [무엇이 문제] 앞의 사례들과 같은 부트 인터럽트 문제다.
 * [우회] IRQ 라우팅 레지스터(0x56)에 0 을 써서 모든 라우팅을 없앤다.
 * 이미 0 이면 아무것도 할 필요가 없으므로 그 사실만 알리고 돌아간다.
 *
 * [SMBus 함수에 거는 이유] 이 레지스터가 8111 의 SMBus 함수에 있기
 * 때문이다. 사우스브리지의 어느 함수에 어떤 레지스터가 놓이는지는
 * 칩마다 다르다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_amd_8111_boot_interrupt]
 *     -> pci_write_config_word()
 */
static void quirk_disable_amd_8111_boot_interrupt(struct pci_dev *dev)
{
	u16 pci_config_word;	/* [한국어] IRQ 라우팅 레지스터 값. */

	if (noioapicquirk)	/* [한국어] 사용자가 부팅 파라미터로 껐으면 아무것도 하지 않는다. */
		return;

	pci_read_config_word(dev, AMD_8111_PCI_IRQ_ROUTING, &pci_config_word);	/* [한국어] 라우팅 레지스터(0x56)를 읽는다. */
	if (!pci_config_word) {	/* [한국어] 이미 0 이면 라우팅이 없으므로 손댈 것이 없다. */
		pci_info(dev, "boot interrupts on device [%04x:%04x] already disabled\n",	/* [한국어] 이미 꺼져 있었다는 사실만 남긴다. */
			 dev->vendor, dev->device);
		return;
	}
	pci_write_config_word(dev, AMD_8111_PCI_IRQ_ROUTING, 0);	/* [한국어] 0 을 써서 모든 부트 인터럽트 라우팅을 없앤다. */
	pci_info(dev, "disabled boot interrupts on device [%04x:%04x]\n",	/* [한국어] 껐음을 남긴다. */
		 dev->vendor, dev->device);
}
/* [한국어] AMD 8111 의 SMBus 함수에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD,   PCI_DEVICE_ID_AMD_8111_SMBUS,	quirk_disable_amd_8111_boot_interrupt);
/* [한국어] 재개 시에도 다시 지운다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_AMD,   PCI_DEVICE_ID_AMD_8111_SMBUS,	quirk_disable_amd_8111_boot_interrupt);
/* [한국어] CONFIG_X86_IO_APIC 블록의 끝 - 여기까지가 부트 인터럽트 관련 quirk 다. */
#endif /* CONFIG_X86_IO_APIC */

/*
 * [한국어]
 * quirk_tc86c001_ide - BAR0 이 홀수 8바이트 경계에 놓이면 재배치한다
 *
 * @dev: Toshiba TC86C001 IDE 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] Toshiba TC86C001 IDE 컨트롤러.
 * [무엇이 문제] 아래 영어 주석대로 BAR0 크기를 표준대로 8바이트라고
 * 보고하지만, BAR0 이 '홀수 번째 8바이트' 자리(주소의 비트 3 이 1인 자리)에
 * 배정되면 PIO 전송이 동작하지 않는다. 즉 실제 정렬 요구가 보고한 크기보다
 * 크다 - 크기 측정으로 알아낼 수 없는 숨은 제약이다.
 * [그대로 두면] 커널이 8바이트 정렬만 맞춰 배정할 수 있고, 그러면 디스크
 * 전송이 실패한다.
 * [우회] 시작 주소의 비트 3 이 서 있으면 리소스를 16바이트로 키우고
 * UNSET 을 세워 재배치를 요청한다. 크기가 16 이면 정렬 요구도 16 이 되어
 * 자동으로 짝수 8바이트 경계에 놓인다.
 *
 * 실행 컨텍스트: HEADER 단계(리소스 할당 전).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_tc86c001_ide] -> resource_set_range()
 */
/*
 * Toshiba TC86C001 IDE controller reports the standard 8-byte BAR0 size
 * but the PIO transfers won't work if BAR0 falls at the odd 8 bytes.
 * Re-allocate the region if needed...
 */
static void quirk_tc86c001_ide(struct pci_dev *dev)
{
	struct resource *r = &dev->resource[0];	/* [한국어] BAR0 리소스. */

	if (r->start & 0x8) {	/* [한국어] 비트 3 이 서 있으면 홀수 번째 8바이트 자리다 - 이 칩이 견디지 못하는 배치다. */
		r->flags |= IORESOURCE_UNSET;	/* [한국어] 주소 미정으로 표시해 재배치를 요청한다. */
		resource_set_range(r, 0, SZ_16);	/* [한국어] 크기를 16바이트로 키운다. IORESOURCE_SIZEALIGN 관례상 크기가 곧 정렬 요구가 되므로, 16바이트 경계에 배정되어 비트 3 이 0 이 된다. */
	}
}
/* [한국어] Toshiba TC86C001 IDE 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_TOSHIBA_2,
			 PCI_DEVICE_ID_TOSHIBA_TC86C001_IDE,
			 quirk_tc86c001_ide);

/*
 * [한국어]
 * quirk_plx_pci9050 - PLX PCI 9050 의 BAR 를 256바이트 경계로 재배치한다
 *
 * @dev: PLX PCI 9050 브리지 컨트롤러(리비전 2 미만) 또는 그것을 쓰는 카드
 * @return: 없음
 *
 * [어떤 하드웨어] PLX PCI 9050 PCI Target 브리지 컨트롤러. 리비전 2
 * (PCI 9052)에서 고쳐졌다.
 * [무엇이 문제] 아래 영어 주석대로, 베이스 주소의 비트 7 이 서 있으면
 * BAR0(메모리) 또는 BAR1(IO)로 접근하는 로컬 설정 레지스터를 제대로 읽을
 * 수 없다. BAR 영역은 꺼져 있거나(크기 0) 켜져 있으며(크기 128) 128바이트
 * 크기이므로, 커널이 128바이트 정렬로 배정하면 비트 7 이 1 인 자리에
 * 놓일 수 있다.
 * [그대로 두면] 설정 레지스터를 읽을 때 엉뚱한 값이 나온다.
 * [우회] 크기를 256바이트로 키워 재배치를 요청한다. 256 정렬이면 비트 7 이
 * 반드시 0 이 된다.
 *
 * [Meilhaus 카드도 대상인 이유] 아래 등록표의 영어 주석대로, 여러 Meilhaus
 * (벤더 0x1402) 카드가 이 PLX 칩을 쓴다. 그 카드들은 PLX 의 vendor/device
 * ID 가 아니라 자기 ID 로 보이므로 따로 등록해야 한다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_plx_pci9050] -> resource_set_range()
 */
/*
 * PLX PCI 9050 PCI Target bridge controller has an erratum that prevents the
 * local configuration registers accessible via BAR0 (memory) or BAR1 (i/o)
 * being read correctly if bit 7 of the base address is set.
 * The BAR0 or BAR1 region may be disabled (size 0) or enabled (size 128).
 * Re-allocate the regions to a 256-byte boundary if necessary.
 */
static void quirk_plx_pci9050(struct pci_dev *dev)
{
	unsigned int bar;	/* [한국어] 검사할 BAR 번호(0 과 1). */

	/* Fixed in revision 2 (PCI 9052). */
	if (dev->revision >= 2)	/* [한국어] 옆의 영어 주석대로 리비전 2(PCI 9052)에서 고쳐졌으므로 그 이상은 건드리지 않는다. */
		return;
	for (bar = 0; bar <= 1; bar++)	/* [한국어] BAR0(메모리)과 BAR1(IO) 두 개만 이 문제의 대상이다. */
		if (pci_resource_len(dev, bar) == 0x80 &&	/* [한국어] 크기가 128(0x80)인 BAR 만 - 크기 0 이면 그 창이 꺼져 있는 것이다. */
		    (pci_resource_start(dev, bar) & 0x80)) {	/* [한국어] 그리고 시작 주소의 비트 7 이 서 있어야 문제가 된다. */
			struct resource *r = &dev->resource[bar];	/* [한국어] 고칠 리소스. */
			pci_info(dev, "Re-allocating PLX PCI 9050 BAR %u to length 256 to avoid bit 7 bug\n",	/* [한국어] 어느 BAR 를 왜 재배치하는지 남긴다. */
				 bar);
			r->flags |= IORESOURCE_UNSET;	/* [한국어] 주소 미정으로 표시해 재배치를 요청한다. */
			resource_set_range(r, 0, SZ_256);	/* [한국어] 크기를 256 으로 키운다. 256 정렬이면 비트 7 이 반드시 0 이 된다. */
		}
}
/* [한국어] PLX 자신의 ID 로 보이는 장치에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_PLX, PCI_DEVICE_ID_PLX_9050,
			 quirk_plx_pci9050);
/*
 * The following Meilhaus (vendor ID 0x1402) device IDs (amongst others)
 * may be using the PLX PCI 9050: 0x0630, 0x0940, 0x0950, 0x0960, 0x100b,
 * 0x1400, 0x140a, 0x140b, 0x14e0, 0x14ea, 0x14eb, 0x1604, 0x1608, 0x160c,
 * 0x168f, 0x2000, 0x2600, 0x3000, 0x810a, 0x810b.
 *
 * Currently, device IDs 0x2000 and 0x2600 are used by the Comedi "me_daq"
 * driver.
 */
/* [한국어] 아래 두 Meilhaus 카드는 위 영어 주석이 나열한 것 중 실제로
 * 리눅스 드라이버(Comedi me_daq)가 쓰는 것들이라 등록해 둔다. */
DECLARE_PCI_FIXUP_HEADER(0x1402, 0x2000, quirk_plx_pci9050);
DECLARE_PCI_FIXUP_HEADER(0x1402, 0x2600, quirk_plx_pci9050);

/*
 * [한국어]
 * quirk_netmos - 병렬 포트가 있는 Netmos 콤보 카드의 클래스 코드를 바꾼다
 *
 * @dev: Netmos 의 직렬 통신 클래스 장치
 * @return: 없음
 *
 * [어떤 하드웨어] Netmos 9735/9745/9835/9845/9855 다중 포트 직렬 카드.
 * [무엇이 문제] 아래 영어 주석대로, 이 카드들은 병렬 포트가 함께 달려
 * 있어도 클래스를 SERIAL 로 보고한다. 그러면 직렬 드라이버가 카드를
 * 가져가 버려 병렬 포트를 쓸 수 없다.
 * [그대로 두면] 콤보 카드의 병렬 포트가 동작하지 않는다.
 * [우회] 병렬 포트가 있는 경우에만 클래스를 COMMUNICATION_OTHER 로 바꾼다.
 * 그러면 직렬 드라이버가 매칭되지 않고, 두 가지를 함께 다루는
 * parport_serial 드라이버가 가져간다.
 *
 * [포트 개수를 어떻게 아는가] 영어 주석대로 서브디바이스 ID 가 0x00PS
 * 형식으로, 상위 니블 P 가 병렬 포트 수, 하위 니블 S 가 직렬 포트 수다.
 * 하드웨어 구성을 서브디바이스 ID 에 인코딩해 둔 셈이다.
 *
 * [예외] 9835 중 IBM 서브시스템 0x0299 는 이 규칙이 적용되지 않아 건너뛴다.
 *
 * 실행 컨텍스트: HEADER 단계, 클래스 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_netmos]
 */
static void quirk_netmos(struct pci_dev *dev)
{
	unsigned int num_parallel = (dev->subsystem_device & 0xf0) >> 4;	/* [한국어] 서브디바이스 ID 의 상위 니블(비트 7:4)이 병렬 포트 개수다. */
	unsigned int num_serial = dev->subsystem_device & 0xf;	/* [한국어] 하위 니블(비트 3:0)이 직렬 포트 개수다. */

	/*
	 * These Netmos parts are multiport serial devices with optional
	 * parallel ports.  Even when parallel ports are present, they
	 * are identified as class SERIAL, which means the serial driver
	 * will claim them.  To prevent this, mark them as class OTHER.
	 * These combo devices should be claimed by parport_serial.
	 *
	 * The subdevice ID is of the form 0x00PS, where <P> is the number
	 * of parallel ports and <S> is the number of serial ports.
	 */
	switch (dev->device) {	/* [한국어] 모델마다 예외가 있어 device ID 로 가른다. */
	case PCI_DEVICE_ID_NETMOS_9835:	/* [한국어] 9835 - 아래 예외가 있는 모델. */
		/* Well, this rule doesn't hold for the following 9835 device */
		if (dev->subsystem_vendor == PCI_VENDOR_ID_IBM &&	/* [한국어] 옆의 영어 주석대로 이 규칙이 통하지 않는 9835 변종이 하나 있다. 서브벤더가 IBM 이고 */
				dev->subsystem_device == 0x0299)	/* [한국어] 서브디바이스가 0x0299 인 것이 그 예외다. */
			return;
		fallthrough;	/* [한국어] 예외가 아니면 아래의 공통 처리로 흘러 내려간다. */
	case PCI_DEVICE_ID_NETMOS_9735:	/* [한국어] 9735 - 아래 공통 처리 대상. */
	case PCI_DEVICE_ID_NETMOS_9745:	/* [한국어] 9745. */
	case PCI_DEVICE_ID_NETMOS_9845:	/* [한국어] 9845. */
	case PCI_DEVICE_ID_NETMOS_9855:	/* [한국어] 9855. */
		if (num_parallel) {	/* [한국어] 병렬 포트가 하나라도 있을 때만 클래스를 바꾼다. 순수 직렬 카드는 직렬 드라이버가 맡는 것이 맞다. */
			pci_info(dev, "Netmos %04x (%u parallel, %u serial); changing class SERIAL to OTHER (use parport_serial)\n",	/* [한국어] 어떤 구성이라 클래스를 바꾸는지, 어떤 드라이버를 써야 하는지 알린다. */
				dev->device, num_parallel, num_serial);
			dev->class = (PCI_CLASS_COMMUNICATION_OTHER << 8) |	/* [한국어] 베이스/서브클래스를 COMMUNICATION_OTHER 로 바꾼다. << 8 은 24비트 클래스 코드에서 상위 16비트 자리에 놓기 위한 것이다. */
			    (dev->class & 0xff);	/* [한국어] ProgIf(하위 8비트)는 원래 값을 그대로 유지한다. */
		}
	}
}
/* [한국어] Netmos 벤더의 직렬 통신 클래스 장치 전부를 HEADER 단계에서
 * 검사한다. 실제 처리 대상은 함수 안에서 device ID 로 좁힌다. */
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_NETMOS, PCI_ANY_ID,
			 PCI_CLASS_COMMUNICATION_SERIAL, 8, quirk_netmos);

/*
 * [한국어]
 * quirk_e100_interrupt - 펌웨어가 켜 둔 채 넘긴 e100 인터럽트를 끈다
 *
 * @dev: Intel 이더넷 클래스 장치 중 e100 계열
 * @return: 없음
 *
 * [어떤 하드웨어] Intel e100 계열 이더넷 컨트롤러(아래 switch 의 device ID
 * 목록이 전체 대상이며, 그 목록은 drivers/net/e100.c 에서 가져온 것이다).
 * [무엇이 문제] 아래 영어 주석대로 일부 펌웨어가 인터럽트를 켜 둔 채
 * 커널에 장치를 넘긴다. 스펙 위반이라기보다 인수인계의 문제다.
 * [그대로 두면] 드라이버가 붙기 전에 패킷이 들어오면 처리기가 없는
 * 인터럽트가 폭주해 시스템이 마비될 수 있다.
 * [우회] 장치의 제어 레지스터를 직접 매핑해 인터럽트 마스크 비트를 세운다.
 * 드라이버가 준비되면 알아서 다시 켠다.
 *
 * [조심스럽게 접근하는 이유] quirk 는 드라이버가 없는 상태에서 도는
 * 코드이므로, 레지스터를 건드리기 전에 (1) 메모리 디코딩이 켜져 있고 BAR0 이
 * 배정되었는지, (2) 장치가 D0 전원 상태인지를 확인한다. D3 상태에서
 * MMIO 를 건드리면 마스터 어보트가 난다.
 *
 * 실행 컨텍스트: FINAL 단계, 클래스 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_e100_interrupt] -> ioremap() -> writeb()
 */
static void quirk_e100_interrupt(struct pci_dev *dev)
{
	u16 command, pmcsr;	/* [한국어] command: Command 레지스터, pmcsr: 전원관리 제어/상태 레지스터. */
	u8 __iomem *csr;	/* [한국어] 장치 제어 레지스터(CSR)를 매핑할 MMIO 포인터. */
	u8 cmd_hi;	/* [한국어] CSR 의 오프셋 3 바이트 - 인터럽트 마스크가 있는 자리. */

	switch (dev->device) {	/* [한국어] 아래 영어 주석대로 e100 계열인지 device ID 로 확인한다. */
	/* PCI IDs taken from drivers/net/e100.c */
	case 0x1029:	/* [한국어] 82559 계열. */
	case 0x1030 ... 0x1034:	/* [한국어] 0x1030~0x1034 구간 - GCC 확장 문법인 case 범위를 쓴다. */
	case 0x1038 ... 0x103E:	/* [한국어] 0x1038~0x103E 구간. */
	case 0x1050 ... 0x1057:	/* [한국어] 0x1050~0x1057 구간. */
	case 0x1059:	/* [한국어] 단일 ID 0x1059. */
	case 0x1064 ... 0x106B:	/* [한국어] 0x1064~0x106B 구간. */
	case 0x1091 ... 0x1095:	/* [한국어] 0x1091~0x1095 구간. */
	case 0x1209:	/* [한국어] 0x1209. */
	case 0x1229:	/* [한국어] 0x1229 - 고전적인 eepro100 ID. */
	case 0x2449:	/* [한국어] 0x2449. */
	case 0x2459:	/* [한국어] 0x2459. */
	case 0x245D:	/* [한국어] 0x245D. */
	case 0x27DC:	/* [한국어] 0x27DC - 여기까지가 e100 계열이다. */
		break;	/* [한국어] 목록에 있으면 아래로 진행한다. */
	default:	/* [한국어] 그 밖의 인텔 이더넷(e1000 등)은 대상이 아니다. */
		return;
	}

	/*
	 * Some firmware hands off the e100 with interrupts enabled,
	 * which can cause a flood of interrupts if packets are
	 * received before the driver attaches to the device.  So
	 * disable all e100 interrupts here.  The driver will
	 * re-enable them when it's ready.
	 */
	pci_read_config_word(dev, PCI_COMMAND, &command);	/* [한국어] Command 레지스터(오프셋 0x04)를 읽는다. */

	if (!(command & PCI_COMMAND_MEMORY) || !pci_resource_start(dev, 0))	/* [한국어] 메모리 디코딩이 꺼져 있거나 BAR0 이 배정되지 않았으면 MMIO 로 접근할 수 없다. */
		return;

	/*
	 * Check that the device is in the D0 power state. If it's not,
	 * there is no point to look any further.
	 */
	if (dev->pm_cap) {	/* [한국어] 전원관리 capability 가 있는 장치만 전원 상태를 확인할 수 있다. */
		pci_read_config_word(dev, dev->pm_cap + PCI_PM_CTRL, &pmcsr);	/* [한국어] PM capability 의 오프셋 + PCI_PM_CTRL(4)이 PMCSR 레지스터다. */
		if ((pmcsr & PCI_PM_CTRL_STATE_MASK) != PCI_D0)	/* [한국어] 하위 2비트가 현재 D 상태다. D0 가 아니면 MMIO 접근이 실패하므로 물러난다. */
			return;
	}

	/* Convert from PCI bus to resource space.  */
	csr = ioremap(pci_resource_start(dev, 0), 8);	/* [한국어] BAR0 의 앞 8바이트만 매핑한다 - 필요한 것은 CSR 의 앞부분뿐이다. */
	if (!csr) {	/* [한국어] 매핑 실패 시 아무것도 할 수 없다. */
		pci_warn(dev, "Can't map e100 registers\n");	/* [한국어] 왜 못 했는지 남긴다. */
		return;
	}

	cmd_hi = readb(csr + 3);	/* [한국어] CSR 오프셋 3 은 e100 의 인터럽트 마스크 바이트다. 0 이면 마스크가 풀려 있다는 뜻, 즉 인터럽트가 켜져 있다. */
	if (cmd_hi == 0) {	/* [한국어] 펌웨어가 인터럽트를 켜 둔 채 넘긴 경우다. */
		pci_warn(dev, "Firmware left e100 interrupts enabled; disabling\n");	/* [한국어] 그 사실을 경고로 남긴다. */
		writeb(1, csr + 3);	/* [한국어] 1 을 써서 모든 인터럽트를 마스크한다. 드라이버가 준비되면 스스로 푼다. */
	}

	iounmap(csr);	/* [한국어] 임시 매핑을 해제한다 - quirk 는 자원을 남기지 않아야 한다. */
}
/* [한국어] Intel 벤더의 이더넷 클래스 장치 전부를 FINAL 단계에서 검사한다.
 * 실제 대상은 함수 안의 device ID 목록으로 좁힌다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_INTEL, PCI_ANY_ID,
			PCI_CLASS_NETWORK_ETHERNET, 8, quirk_e100_interrupt);

/*
 * The 82575 and 82598 may experience data corruption issues when transitioning
 * out of L0S.  To prevent this we need to disable L0S on the PCIe link.
 */
/*
 * [한국어]
 * quirk_disable_aspm_l0s - 특정 Intel NIC 에서 ASPM L0s 를 광고 자체에서 지운다
 *
 * @dev: 아래 등록표의 Intel 이더넷 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 82575 와 82598 계열 이더넷 컨트롤러(등록표의
 * device ID 14종).
 * [ASPM L0s 란] ASPM(Active State Power Management)은 링크가 놀고 있을 때
 * 자동으로 저전력 상태로 내리는 PCIe 기능이다. L0s 는 그중 가벼운 단계로,
 * 한쪽 방향만 잠재우고 빠르게 복귀한다.
 * [무엇이 문제] 위 영어 주석대로, 이 칩들은 L0s 에서 빠져나올 때 데이터가
 * 손상될 수 있다. 링크 복귀 처리의 결함이다.
 * [그대로 두면] 절전이 동작하는 조용한 순간마다 데이터가 깨질 수 있다.
 * [우회] pcie_aspm_remove_cap() 으로 이 장치의 Link Capabilities 에서 L0s
 * 지원 표시 자체를 지운다. ASPM 정책 코드가 지원하지 않는 상태로 보게 되어
 * 절대 L0s 로 내리지 않는다.
 *
 * [NVMe 와의 관계] 대상은 모두 Intel 이더넷 컨트롤러이며, 이 quirk 가
 * NVMe 컨트롤러에 걸리는 일은 없다. ASPM 자체는 NVMe SSD 의 링크에도
 * 적용되는 공통 기능이지만, 이 항목의 근거는 위 영어 주석이 말하는
 * 82575/82598 의 결함뿐이다.
 *
 * 실행 컨텍스트: HEADER 단계. ASPM 정책이 결정되기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_aspm_l0s] -> pcie_aspm_remove_cap()
 */
static void quirk_disable_aspm_l0s(struct pci_dev *dev)
{
	pcie_aspm_remove_cap(dev, PCI_EXP_LNKCAP_ASPM_L0S);	/* [한국어] Link Capabilities 의 ASPM 지원 필드에서 L0s 비트를 지운다. 하드웨어 레지스터가 아니라 커널이 기억하는 capability 를 고치는 것으로, ASPM 정책이 이 값을 보고 판단한다. */
}
/* [한국어] 문제 있는 Intel NIC 14종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10a7, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10a9, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10b6, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10c6, quirk_disable_aspm_l0s);
/* [한국어] 이어지는 등록 줄: 0x10c7, 0x10c8, 0x10d6, 0x10db. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10c7, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10c8, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10d6, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10db, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10dd, quirk_disable_aspm_l0s);
/* [한국어] 이어지는 등록 줄: 0x10e1, 0x10ec, 0x10f1, 0x10f4. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10e1, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10ec, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10f1, quirk_disable_aspm_l0s);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x10f4, quirk_disable_aspm_l0s);
/* [한국어] 이어지는 등록 줄: 0x1508. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1508, quirk_disable_aspm_l0s);

/*
 * [한국어]
 * quirk_disable_aspm_l0s_l1 - L0s 와 L1 을 모두 광고에서 지운다
 *
 * @dev: 아래 등록표의 브리지들(ASMedia ASM1080, Freescale 0x0451,
 *       PA Semi 0xa002, Huawei 0x1105)
 * @return: 없음
 *
 * [무엇이 문제] 아래 등록표 위의 영어 주석이 근거다. ASM1083/1085
 * PCIe-to-PCI 브리지는 ASPM 이 켜져 있으면 상위 PCIe 루트 포트에서 AER
 * 타임아웃 오류를 일으킨다. 적어도 L0s 모드가 영향을 받는 것이 확인되었고,
 * 안전을 위해 지금은 L0s 와 L1 을 모두 끈다는 것이 주석의 설명이다.
 * 나머지 세 장치(Freescale, PA Semi, Huawei)에 대해서는 원본 주석에 배경이
 * 적혀 있지 않아 이 트리의 정보만으로는 확인할 수 없다.
 * [L1 이란] ASPM 의 더 깊은 절전 단계로, 양방향을 모두 잠재워 전력을 더
 * 아끼지만 복귀에 시간이 더 걸린다.
 * [그대로 두면] 상위 루트 포트에 AER 오류가 보고되어 오류 복구 경로가
 * 불필요하게 돈다.
 * [우회] L0s 와 L1 지원 표시를 함께 지워 ASPM 이 아예 켜지지 않게 한다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_aspm_l0s_l1] -> pcie_aspm_remove_cap()
 */
static void quirk_disable_aspm_l0s_l1(struct pci_dev *dev)
{
	pcie_aspm_remove_cap(dev,	/* [한국어] 두 비트를 한 번에 지운다. */
			     PCI_EXP_LNKCAP_ASPM_L0S | PCI_EXP_LNKCAP_ASPM_L1);	/* [한국어] PCI_EXP_LNKCAP_ASPM_L0S 와 _L1 은 Link Capabilities 의 ASPM Support 필드(비트 11:10)의 각 비트다. */
}

/*
 * ASM1083/1085 PCIe-PCI bridge devices cause AER timeout errors on the
 * upstream PCIe root port when ASPM is enabled. At least L0s mode is affected;
 * disable both L0s and L1 for now to be safe.
 */
/* [한국어] ASM1083/1085 - 위 영어 주석이 밝힌 AER 타임아웃의 당사자다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ASMEDIA, 0x1080, quirk_disable_aspm_l0s_l1);
/* [한국어] Freescale 0x0451 - 배경은 원본 주석에 없다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_FREESCALE, 0x0451, quirk_disable_aspm_l0s_l1);
/* [한국어] PA Semi 0xa002 - 배경은 원본 주석에 없다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_PASEMI, 0xa002, quirk_disable_aspm_l0s_l1);
/* [한국어] Huawei 0x1105 - 배경은 원본 주석에 없다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HUAWEI, 0x1105, quirk_disable_aspm_l0s_l1);

/*
 * Some Pericom PCIe-to-PCI bridges in reverse mode need the PCIe Retrain
 * Link bit cleared after starting the link retrain process to allow this
 * process to finish.
 *
 * Affected devices: PI7C9X110, PI7C9X111SL, PI7C9X130.  See also the
 * Pericom Errata Sheet PI7C9X111SLB_errata_rev1.2_102711.pdf.
 */
/*
 * [한국어]
 * quirk_enable_clear_retrain_link - 재훈련 후 Retrain Link 비트를 지우도록 표시
 *
 * @dev: Pericom PI7C9X110 / PI7C9X111SL / PI7C9X130 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Pericom PCIe-to-PCI 브리지 3종을 reverse 모드로 쓸 때.
 * [무엇이 문제] 위 영어 주석과 Pericom 에라타 시트
 * (PI7C9X111SLB_errata_rev1.2_102711.pdf)가 근거다. PCIe 스펙상 Link
 * Control 의 Retrain Link 비트는 1 을 쓰면 재훈련이 시작되고 하드웨어가
 * 알아서 처리하는 자기 소거(self-clearing) 비트다. 그런데 이 칩들은
 * 소프트웨어가 그 비트를 명시적으로 0 으로 되돌려 주어야 재훈련 과정이
 * 끝난다.
 * [그대로 두면] 링크 재훈련이 끝나지 않아 링크가 올라오지 않는다.
 * [우회] dev->clear_retrain_link 플래그를 세워, PCI 코어의 재훈련 코드가
 * 시작 직후 비트를 손으로 지우게 한다. 이 함수 자체는 하드웨어를 건드리지
 * 않고 표시만 남긴다.
 *
 * 실행 컨텍스트: EARLY 단계. 링크를 다루는 코드가 이 플래그를 보기 전에
 * 세워져 있어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_enable_clear_retrain_link]
 */
static void quirk_enable_clear_retrain_link(struct pci_dev *dev)
{
	dev->clear_retrain_link = 1;	/* [한국어] PCI 코어의 링크 재훈련 코드가 참조하는 플래그. */
	pci_info(dev, "Enable PCIe Retrain Link quirk\n");	/* [한국어] 이 quirk 가 켜졌음을 남긴다. */
}
/* [한국어] Pericom 브리지 3종에 EARLY 단계로 등록한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_PERICOM, 0xe110, quirk_enable_clear_retrain_link);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_PERICOM, 0xe111, quirk_enable_clear_retrain_link);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_PERICOM, 0xe130, quirk_enable_clear_retrain_link);

/*
 * [한국어]
 * fixup_rev1_53c810 - 클래스 코드를 전혀 채우지 않는 NCR 53c810 을 SCSI 로 표시
 *
 * @dev: NCR 53c810 (리비전 1)
 * @return: 없음
 *
 * [어떤 하드웨어] NCR 53c810 SCSI 컨트롤러의 리비전 1.
 * [무엇이 문제] 아래 영어 주석대로 이 리비전은 클래스 코드를 아예 설정하지
 * 않는다(0 으로 읽힌다). 클래스 코드는 표준 헤더의 필수 필드이므로 명백한
 * 스펙 위반이다.
 * [그대로 두면] 커널이 이 장치가 무엇인지 몰라 리소스를 재배치해 주지
 * 않는다(영어 주석의 표현대로 resources remapped 가 되지 않는다).
 * [우회] 클래스가 0 일 때만 SCSI 스토리지 클래스로 채워 준다. 리비전으로
 * 거르는 대신 '클래스가 비어 있는지'로 판정하므로, 클래스를 제대로 채우는
 * 후기 리비전은 자동으로 제외된다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [fixup_rev1_53c810]
 */
static void fixup_rev1_53c810(struct pci_dev *dev)
{
	u32 class = dev->class;	/* [한국어] 원래 클래스 값을 보관한다 - 로그에 무엇이 무엇으로 바뀌었는지 찍기 위해서다. */

	/*
	 * rev 1 ncr53c810 chips don't set the class at all which means
	 * they don't get their resources remapped. Fix that here.
	 */
	if (class)	/* [한국어] 클래스가 0 이 아니면 정상적으로 채워진 리비전이므로 손대지 않는다. */
		return;

	dev->class = PCI_CLASS_STORAGE_SCSI << 8;	/* [한국어] SCSI 스토리지 클래스로 채운다. << 8 은 24비트 클래스 코드의 상위 16비트 자리에 놓기 위한 것이다. */
	pci_info(dev, "NCR 53c810 rev 1 PCI class overridden (%#08x -> %#08x)\n",	/* [한국어] 무엇이 무엇으로 바뀌었는지 남긴다. */
		 class, dev->class);
}
/* [한국어] NCR 53c810 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NCR, PCI_DEVICE_ID_NCR_53C810, fixup_rev1_53c810);

/*
 * [한국어]
 * quirk_p64h2_1k_io - Intel P64H2 의 1KB 단위 IO 창을 커널에 알린다
 *
 * @dev: Intel P64H2 (device 0x1460)
 * @return: 없음
 *
 * [무엇이 문제] 스펙 위반이 아니라 스펙 밖의 확장이다. PCI-to-PCI 브리지의
 * IO 창은 표준상 4KB 단위로만 설정할 수 있다. 그래서 브리지 아래에 IO 를
 * 조금만 쓰는 장치가 있어도 4KB 를 통째로 잡아먹어, IO 공간(전체 64KB)이
 * 금세 바닥난다.
 * [P64H2 의 확장] 이 칩은 벤더 전용 레지스터 0x40 의 비트 9 로 IO 창을
 * 1KB 단위로 쪼갤 수 있게 해 준다. 그러나 커널이 그 사실을 알아야만
 * 활용할 수 있다.
 * [우회] 그 비트가 켜져 있으면 dev->io_window_1k 를 세워, 브리지 창을
 * 계산하는 코드가 1KB 단위로 잡게 한다.
 *
 * 실행 컨텍스트: HEADER 단계(브리지 창 계산 전).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_p64h2_1k_io]
 */
/* Enable 1k I/O space granularity on the Intel P64H2 */
static void quirk_p64h2_1k_io(struct pci_dev *dev)
{
	u16 en1k;	/* [한국어] 0x40 레지스터 값 - 1KB 단위 활성 비트가 들어 있다. */

	pci_read_config_word(dev, 0x40, &en1k);	/* [한국어] 벤더 전용 레지스터 0x40 을 읽는다. */

	if (en1k & 0x200) {	/* [한국어] 비트 9(0x200)가 서 있으면 이 브리지가 1KB 단위 IO 창을 쓰도록 설정되어 있다. */
		pci_info(dev, "Enable I/O Space to 1KB granularity\n");	/* [한국어] 그 사실을 알린다. */
		dev->io_window_1k = 1;	/* [한국어] 브리지 창 계산 코드가 참조하는 플래그를 세운다. */
	}
}
/* [한국어] Intel P64H2 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x1460, quirk_p64h2_1k_io);

/*
 * Under some circumstances, AER is not linked with extended capabilities.
 * Force it to be linked by setting the corresponding control bit in the
 * config space.
 */
/*
 * [한국어]
 * quirk_nvidia_ck804_pcie_aer_ext_cap - 연결되지 않은 AER capability 를 목록에 잇는다
 *
 * @dev: NVIDIA CK804 PCIe 브리지
 * @return: 없음
 *
 * [확장 capability 목록이란] PCIe 는 설정 공간 0x100 부터 확장 capability
 * 들을 연결 리스트로 늘어놓는다. 각 항목의 헤더에 다음 항목의 오프셋이
 * 들어 있고, 커널은 그 사슬을 따라가며 AER 같은 기능을 찾는다.
 * [무엇이 문제] 위 영어 주석대로, 어떤 상황에서는 이 칩의 AER capability
 * 가 그 사슬에 연결되어 있지 않다. 하드웨어에는 있는데 목록에서 빠져 있어
 * 커널이 찾지 못하는 것이다.
 * [그대로 두면] AER(Advanced Error Reporting)이 없는 장치로 취급되어,
 * 정정 가능/불가능 오류가 보고되지 않고 조용히 지나간다.
 * [우회] 벤더 전용 레지스터 0xf41 의 비트 5 를 세우면 AER 이 목록에
 * 연결된다. 설정 비트 하나로 capability 를 사슬에 붙이는 셈이다.
 *
 * [주소가 0xf41 인 점] 표준 PCI 설정 공간은 256바이트뿐이지만 PCIe 는
 * 4KB 까지 있으므로 0xf41 도 유효한 오프셋이다. 실패할 수 있으므로
 * 읽기 반환값을 확인한다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nvidia_ck804_pcie_aer_ext_cap]
 *     -> pci_write_config_byte()
 */
static void quirk_nvidia_ck804_pcie_aer_ext_cap(struct pci_dev *dev)
{
	uint8_t b;	/* [한국어] 0xf41 레지스터 값. */

	if (pci_read_config_byte(dev, 0xf41, &b) == 0) {	/* [한국어] 확장 설정 공간 접근은 실패할 수 있으므로 반환값이 0(PCIBIOS_SUCCESSFUL)일 때만 진행한다. */
		if (!(b & 0x20)) {	/* [한국어] 비트 5 가 AER 연결 비트다. 이미 서 있으면 손댈 것이 없다. */
			pci_write_config_byte(dev, 0xf41, b | 0x20);	/* [한국어] 비트 5 를 세워 AER capability 를 확장 목록에 잇는다. */
			pci_info(dev, "Linking AER extended capability\n");	/* [한국어] 무엇을 했는지 남긴다. */
		}
	}
}
/* [한국어] NVIDIA CK804 PCIe 브리지에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA,  PCI_DEVICE_ID_NVIDIA_CK804_PCIE,
			quirk_nvidia_ck804_pcie_aer_ext_cap);
/* [한국어] 재개 시에도 다시 이어 준다 - 절전 후 비트가 초기화된다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_NVIDIA,  PCI_DEVICE_ID_NVIDIA_CK804_PCIE,
			quirk_nvidia_ck804_pcie_aer_ext_cap);

/*
 * [한국어]
 * quirk_via_cx700_pci_parking_caching - CX700 의 버스 파킹과 읽기 캐싱을 끈다
 *
 * @dev: VIA CX700 (device 0x324e)
 * @return: 없음
 *
 * [어떤 하드웨어] VIA CX700 칩셋. 단 외부 PCI 버스에 VT6212L USB 컨트롤러가
 * 꽂혀 있을 때만 적용된다.
 * [무엇이 문제] 아래 영어 주석대로, PCI Bus Parking 과 PCI Master 읽기
 * 캐싱이 켜져 있으면 PCI 버스의 VT6212L 과 사이에서 원인 불명의 타이밍
 * 오류가 생기고 USB2.0 패킷이 유실된다.
 * [Bus Parking 이란] 아무도 버스를 요청하지 않을 때 마지막 마스터에게
 * 버스 소유를 그대로 두는 최적화다. 다음 전송의 중재 지연을 줄여 준다.
 * [그대로 두면] USB 장치에서 데이터가 사라진다.
 * [우회] 벤더 전용 레지스터들을 고쳐 파킹과 읽기 캐싱을 끄고, 마스터 버스
 * 타임아웃과 Read FIFO 타이머도 안전한 값으로 바꾼다.
 *
 * [적용 조건이 까다로운 이유] 아래 영어 주석대로, CX700 코어 자체에도
 * VT6212L 과 같은 PCI ID 를 쓰는 USB 호스트 컨트롤러가 들어 있다. 그래서
 * 그 ID 를 가진 장치를 '두 번째'로 찾아냈을 때만(즉 외부 버스에 진짜
 * VT6212L 이 있을 때만) 성능을 희생하는 이 quirk 를 적용한다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_via_cx700_pci_parking_caching]
 *     -> pci_get_device() -> pci_write_config_byte()
 */
static void quirk_via_cx700_pci_parking_caching(struct pci_dev *dev)
{
	/*
	 * Disable PCI Bus Parking and PCI Master read caching on CX700
	 * which causes unspecified timing errors with a VT6212L on the PCI
	 * bus leading to USB2.0 packet loss.
	 *
	 * This quirk is only enabled if a second (on the external PCI bus)
	 * VT6212L is found -- the CX700 core itself also contains a USB
	 * host controller with the same PCI ID as the VT6212L.
	 */

	/* Count VT6212L instances */
	struct pci_dev *p = pci_get_device(PCI_VENDOR_ID_VIA,	/* [한국어] 위 영어 주석대로 VT6212L 을 세어 본다. 첫 번째로 찾은 것은 CX700 내부의 USB 컨트롤러다. */
		PCI_DEVICE_ID_VIA_8235_USB_2, NULL);
	uint8_t b;	/* [한국어] 벤더 전용 레지스터 값을 담는 임시 바이트. */

	/*
	 * p should contain the first (internal) VT6212L -- see if we have
	 * an external one by searching again.
	 */
	p = pci_get_device(PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_8235_USB_2, p);	/* [한국어] 같은 ID 로 한 번 더 찾는다. 여기서 나오면 외부 PCI 버스에 진짜 VT6212L 이 있다는 뜻이다. pci_get_device() 는 이전 인자 p 의 참조수를 대신 놓아 준다. */
	if (!p)	/* [한국어] 두 번째가 없으면 문제가 되는 조합이 아니므로 아무것도 하지 않는다. */
		return;
	pci_dev_put(p);	/* [한국어] 두 번째 장치의 참조수를 놓는다. 필요한 것은 존재 여부뿐이었다. */

	if (pci_read_config_byte(dev, 0x76, &b) == 0) {	/* [한국어] 벤더 전용 레지스터 0x76 을 읽는다. 확장 접근이 아니지만 실패 검사를 해 둔다. */
		if (b & 0x40) {	/* [한국어] 비트 6 이 PCI Bus Parking 활성 비트다. */
			/* Turn off PCI Bus Parking */
			pci_write_config_byte(dev, 0x76, b ^ 0x40);	/* [한국어] XOR 로 그 비트만 뒤집는다 - 위에서 서 있음을 확인했으므로 결과는 0 이 된다. */

			pci_info(dev, "Disabling VIA CX700 PCI parking\n");	/* [한국어] 파킹을 껐음을 남긴다. */
		}
	}

	if (pci_read_config_byte(dev, 0x72, &b) == 0) {	/* [한국어] 벤더 전용 레지스터 0x72(마스터 읽기 캐싱 설정)를 읽는다. */
		if (b != 0) {	/* [한국어] 0 이 아니면 캐싱이 어떤 형태로든 켜져 있다는 뜻이다. */
			/* Turn off PCI Master read caching */
			pci_write_config_byte(dev, 0x72, 0x0);	/* [한국어] 0 을 써서 읽기 캐싱을 완전히 끈다. */

			/* Set PCI Master Bus time-out to "1x16 PCLK" */
			pci_write_config_byte(dev, 0x75, 0x1);	/* [한국어] 0x75 에 1 을 써서 마스터 버스 타임아웃을 옆의 영어 주석이 말하는 '1x16 PCLK' 로 맞춘다. */

			/* Disable "Read FIFO Timer" */
			pci_write_config_byte(dev, 0x77, 0x0);	/* [한국어] 0x77 에 0 을 써서 Read FIFO 타이머를 끈다. */

			pci_info(dev, "Disabling VIA CX700 PCI caching\n");	/* [한국어] 캐싱을 껐음을 남긴다. */
		}
	}
}
/* [한국어] VIA CX700(0x324e)에 FINAL 단계로 등록한다. 실제 적용 여부는
 * 함수 안에서 외부 VT6212L 존재로 판정한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA, 0x324e, quirk_via_cx700_pci_parking_caching);

/*
 * [한국어]
 * quirk_brcm_5719_limit_mrrs - BCM5719 A0 의 Max Read Request Size 를 2048 로 제한
 *
 * @dev: Broadcom Tigon3 5719 이더넷 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] Broadcom BCM5719 의 A0 스테핑(리비전 레지스터 0xf4 가
 * 0x05719000 인 것).
 * [MRRS 란] PCIe 장치가 한 번의 메모리 읽기 요청으로 요구할 수 있는 최대
 * 바이트 수다. Device Control 레지스터에 있으며 크게 잡을수록 효율이 좋다.
 * [무엇이 문제] 이 quirk 에는 원본 영어 주석에 배경 설명이 없다. 코드가
 * 하는 일로 보아 A0 스테핑이 2048 을 넘는 읽기 요청을 감당하지 못한다는
 * 뜻이지만, 구체적 증상은 이 트리의 정보만으로는 확인할 수 없다.
 * [우회] 현재 MRRS 가 2048 보다 크면 2048 로 낮춘다.
 *
 * [ENABLE 단계인 이유] MRRS 는 장치를 켤 때 PCI 코어나 드라이버가 다시
 * 설정할 수 있으므로, 장치를 켜는 시점에 덮어써야 값이 유지된다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_brcm_5719_limit_mrrs] -> pcie_set_readrq()
 */
static void quirk_brcm_5719_limit_mrrs(struct pci_dev *dev)
{
	u32 rev;	/* [한국어] 벤더 전용 레지스터 0xf4 의 값 - 칩 리비전을 담고 있다. */

	pci_read_config_dword(dev, 0xf4, &rev);	/* [한국어] 표준 Revision ID(오프셋 0x08)가 아니라 벤더 전용 0xf4 에서 세부 스테핑을 읽는다. */

	/* Only CAP the MRRS if the device is a 5719 A0 */
	if (rev == 0x05719000) {	/* [한국어] 옆의 영어 주석대로 5719 A0 스테핑만 대상이다. */
		int readrq = pcie_get_readrq(dev);	/* [한국어] 현재 설정된 Max Read Request Size 를 바이트 단위로 얻는다. */
		if (readrq > 2048)	/* [한국어] 2048 을 넘을 때만 낮춘다 - 이미 작으면 건드릴 이유가 없다. */
			pcie_set_readrq(dev, 2048);	/* [한국어] MRRS 를 2048 바이트로 제한한다. */
	}
}
/* [한국어] BCM5719 에 ENABLE 단계로 등록한다 - 장치를 켤 때마다 다시 제한한다. */
DECLARE_PCI_FIXUP_ENABLE(PCI_VENDOR_ID_BROADCOM,
			 PCI_DEVICE_ID_TIGON3_5719,
			 quirk_brcm_5719_limit_mrrs);

/*
 * [한국어]
 * quirk_unhide_mch_dev6 - i865/i875 MCH 의 숨겨진 디바이스 6 을 드러낸다
 *
 * @dev: Intel 82865 또는 82875 호스트 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 82865/82875 메모리 컨트롤러 허브.
 * [무엇이 문제] 아래 영어 주석대로, 인텔은 BIOS 개발자에게 디바이스 6 을
 * 숨기라고 안내한다. 그 디바이스에 DRB(DRAM Row Boundary) 등 메모리 구성
 * 정보를 담은 오버플로 장치 접근 설정이 들어 있다.
 * [그대로 두면] EDAC(메모리 오류 정정 감시) 드라이버가 DRB 를 읽을 수
 * 없어 메모리 구성과 오류를 보고하지 못한다.
 * [우회] 벤더 전용 레지스터 0xF4 의 비트 1 을 세워 디바이스 6 을 드러낸다.
 * 이 코드의 출처는 i82875P 용 EDAC 소스이며, 영어 주석의 링크가 근거다.
 *
 * [EARLY 단계인 이유] 숨김을 풀면 버스에 장치가 하나 늘어난다. 그것이
 * PCI 스캔에 잡히려면 스캔 전에 드러나 있어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_unhide_mch_dev6] -> pci_write_config_byte()
 */
/*
 * Originally in EDAC sources for i82875P: Intel tells BIOS developers to
 * hide device 6 which configures the overflow device access containing the
 * DRBs - this is where we expose device 6.
 * http://www.x86-secret.com/articles/tweak/pat/patsecrets-2.htm
 */
static void quirk_unhide_mch_dev6(struct pci_dev *dev)
{
	u8 reg;	/* [한국어] 0xF4 레지스터 값. */

	if (pci_read_config_byte(dev, 0xF4, &reg) == 0 && !(reg & 0x02)) {	/* [한국어] 0xF4 를 읽는 데 성공했고 비트 1 이 꺼져 있을 때만 - 즉 아직 숨겨져 있을 때만 손댄다. */
		pci_info(dev, "Enabling MCH 'Overflow' Device\n");	/* [한국어] 무엇을 드러내는지 남긴다. */
		pci_write_config_byte(dev, 0xF4, reg | 0x02);	/* [한국어] 비트 1 을 세워 디바이스 6 을 노출시킨다. */
	}
}
/* [한국어] i865 호스트 브리지에 EARLY 단계로 등록한다 - 스캔 전이어야 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82865_HB,
			quirk_unhide_mch_dev6);
/* [한국어] i875 호스트 브리지에도 같은 처리를 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82875_HB,
			quirk_unhide_mch_dev6);

/* [한국어] 아래 MSI 관련 quirk 는 MSI 지원이 빌드에 들어 있을 때만 의미가
 * 있다. MSI 가 없는 커널에서는 애초에 켜질 일이 없기 때문이다. */
#ifdef CONFIG_PCI_MSI
/*
 * Some chipsets do not support MSI. We cannot easily rely on setting
 * PCI_BUS_FLAGS_NO_MSI in its bus flags because there are actually some
 * other buses controlled by the chipset even if Linux is not aware of it.
 * Instead of setting the flag on all buses in the machine, simply disable
 * MSI globally.
 */
/*
 * [한국어]
 * quirk_disable_all_msi - 시스템 전체에서 MSI 를 끈다
 *
 * @dev: MSI 를 지원하지 못하는 칩셋(아래 등록표 9종)
 * @return: 없음
 *
 * [무엇이 문제] 위 영어 주석대로 일부 칩셋은 MSI 를 지원하지 못한다.
 * 각 장치가 어떤 식으로 실패하는지는 원본 주석에 없어 이 트리의
 * 정보만으로는 확인할 수 없다.
 * [왜 버스 플래그로 부족한가] 영어 주석이 그 이유를 밝힌다. 문제의 칩셋이
 * 제어하는 버스 중에는 리눅스가 인지하지 못하는 것도 있어,
 * PCI_BUS_FLAGS_NO_MSI 를 특정 버스에 세우는 것만으로는 새는 곳이 생긴다.
 * 기계의 모든 버스에 플래그를 세우는 대신 MSI 를 전역으로 꺼 버린다.
 * [우회] pci_no_msi() 로 PCI 서브시스템 전체의 MSI 사용을 금지한다.
 *
 * [★ 영향 범위가 시스템 전체라는 점] 이 quirk 는 문제의 칩셋뿐 아니라
 * 그 기계에 꽂힌 모든 장치의 MSI 를 막는다. NVMe 컨트롤러도 예외가 아니다 -
 * drivers/nvme/host/pci.c 의 nvme_setup_irqs() 는
 * PCI_IRQ_ALL_TYPES(MSI-X/MSI/INTx 모두 허용)로
 * pci_alloc_irq_vectors_affinity() 를 부르므로, MSI 계열이 막히면 INTx
 * 하나로 물러난다. 큐마다 인터럽트 벡터를 나눠 갖는 NVMe 의 구조가
 * 무너지는 셈이다. 다만 이 quirk 의 대상 목록에 NVMe 컨트롤러는 없다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_disable_all_msi] -> pci_no_msi()
 */
static void quirk_disable_all_msi(struct pci_dev *dev)
{
	pci_no_msi();	/* [한국어] PCI 서브시스템 전역의 MSI 사용을 끈다. 이후 모든 장치의 MSI 요청이 거부된다. */
	pci_warn(dev, "MSI quirk detected; MSI disabled\n");	/* [한국어] 왜 껐는지 경고 수준으로 남긴다 - 성능에 영향을 주는 결정이라 사용자가 알아야 한다. */
}
/* [한국어] MSI 를 지원하지 못하는 칩셋 9종에 FINAL 단계로 등록한다.
 * 이 중 하나라도 시스템에 있으면 기계 전체의 MSI 가 꺼진다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SERVERWORKS, PCI_DEVICE_ID_SERVERWORKS_GCNB_LE, quirk_disable_all_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, PCI_DEVICE_ID_ATI_RS400_200, quirk_disable_all_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, PCI_DEVICE_ID_ATI_RS480, quirk_disable_all_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_VT3336, quirk_disable_all_msi);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_VIA_VT3351, PCI_DEVICE_ID_VIA_VT3364,
 * PCI_DEVICE_ID_VIA_8380_0, 0x0761. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_VT3351, quirk_disable_all_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_VT3364, quirk_disable_all_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA, PCI_DEVICE_ID_VIA_8380_0, quirk_disable_all_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SI, 0x0761, quirk_disable_all_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SAMSUNG, 0xa5e3, quirk_disable_all_msi);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/* Disable MSI on chipsets that are known to not support it */
/*
 * [한국어]
 * quirk_disable_msi - 이 브리지 아래의 버스에서만 MSI 를 끈다
 *
 * @dev: MSI 를 전달하지 못하는 브리지(아래 등록표 4종)
 * @return: 없음
 *
 * [무엇이 문제] 위 영어 주석대로 MSI 를 지원하지 못한다고 알려진 칩셋들이다.
 * 각 장치의 구체적 증상은 원본 주석에 없어 이 트리의 정보만으로는 확인할
 * 수 없다. MSI 는 인터럽트를 메모리 쓰기 트랜잭션으로 보내는 방식이므로,
 * 그 트랜잭션을 제대로 전달하지 못하는 브리지 아래에서는 쓸 수 없다.
 * [그대로 두면] 그 브리지 아래 장치의 MSI 인터럽트가 도착하지 않는다.
 * [우회] 브리지가 만든 하위 버스에 PCI_BUS_FLAGS_NO_MSI 를 세운다.
 * quirk_disable_all_msi() 와 달리 영향 범위가 그 버스 아래로 한정된다.
 *
 * [NVMe 와의 관계] 이 브리지 아래에 NVMe SSD 를 연결하면 그 SSD 도 MSI/
 * MSI-X 를 쓸 수 없어 INTx 로 물러난다. 다만 이 표의 장치는 모두
 * 브리지이고 NVMe 컨트롤러 자체는 대상이 아니다.
 *
 * 실행 컨텍스트: FINAL 단계. 하위 버스가 만들어진 뒤여야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() 또는 quirk_amd_780_apc_msi() -> [quirk_disable_msi]
 */
static void quirk_disable_msi(struct pci_dev *dev)
{
	if (dev->subordinate) {	/* [한국어] 하위 버스가 실제로 있는 브리지일 때만 의미가 있다. */
		pci_warn(dev, "MSI quirk detected; subordinate MSI disabled\n");	/* [한국어] 어떤 브리지 아래에서 MSI 를 껐는지 경고로 남긴다. */
		dev->subordinate->bus_flags |= PCI_BUS_FLAGS_NO_MSI;	/* [한국어] 그 버스에 'MSI 금지' 플래그를 세운다. MSI 할당 코드가 버스를 거슬러 올라가며 이 플래그를 확인한다. */
	}
}
/* [한국어] MSI 를 전달하지 못하는 브리지 4종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_8131_BRIDGE, quirk_disable_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_VIA, 0xa238, quirk_disable_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x5a3f, quirk_disable_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_RDC, 0x1031, quirk_disable_msi);

/*
 * [한국어]
 * quirk_amd_780_apc_msi - 벤더 ID 가 무작위인 APC 브리지를 슬롯 번호로 찾아 MSI 를 끈다
 *
 * @host_bridge: AMD 780 계열 노스브리지(device 0x9600 또는 0x9601)
 * @return: 없음
 *
 * [어떤 하드웨어] AMD 780 계열 노스브리지 안의 APC 브리지.
 * [무엇이 문제] 아래 영어 주석대로 에라타 18 이다. APC 브리지의 벤더 ID
 * 레지스터에 OEM 이 넣은 무작위 서브시스템 ID 가 들어 있어, vendor/device
 * ID 로는 이 브리지를 찾을 수 없다. DECLARE_PCI_FIXUP_* 매크로는 vendor 와
 * device 로만 매칭하므로 이 장치를 직접 겨냥할 방법이 없다.
 * [우회] ID 를 신뢰할 수 있는 호스트 브리지를 방아쇠로 걸고, 함수 안에서
 * 같은 버스의 슬롯 1 함수 0 을 슬롯 번호로 직접 찾아간다. 그 자리에 있는
 * 장치의 device ID 가 0x9602 면 APC 브리지가 맞으므로 MSI 를 끈다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_amd_780_apc_msi]
 *     -> pci_get_slot() -> quirk_disable_msi()
 */
/*
 * The APC bridge device in AMD 780 family northbridges has some random
 * OEM subsystem ID in its vendor ID register (erratum 18), so instead
 * we use the possible vendor/device IDs of the host bridge for the
 * declared quirk, and search for the APC bridge by slot number.
 */
static void quirk_amd_780_apc_msi(struct pci_dev *host_bridge)
{
	struct pci_dev *apc_bridge;	/* [한국어] 찾아낸 APC 브리지를 담을 포인터. */

	apc_bridge = pci_get_slot(host_bridge->bus, PCI_DEVFN(1, 0));	/* [한국어] 호스트 브리지와 같은 버스의 디바이스 1, 함수 0 을 직접 조회한다. PCI_DEVFN(1, 0) 은 디바이스/함수 번호를 devfn 한 바이트로 합치는 매크로다. 찾으면 참조수가 올라간다. */
	if (apc_bridge) {	/* [한국어] 그 자리에 장치가 있을 때만 진행한다. */
		if (apc_bridge->device == 0x9602)	/* [한국어] device ID 가 0x9602 여야 APC 브리지다. 벤더 ID 는 못 믿지만 device ID 는 쓸 수 있다. */
			quirk_disable_msi(apc_bridge);	/* [한국어] 위 함수를 재사용해 그 브리지 아래 버스의 MSI 를 끈다. */
		pci_dev_put(apc_bridge);	/* [한국어] 조회로 올린 참조수를 놓는다. */
	}
}
/* [한국어] AMD 780 계열 호스트 브리지 2종을 방아쇠로 FINAL 단계에 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD, 0x9600, quirk_amd_780_apc_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AMD, 0x9601, quirk_amd_780_apc_msi);

/*
 * [한국어]
 * msi_ht_cap_enabled - HyperTransport MSI Mapping capability 가 켜져 있는지 확인
 *
 * @dev: 검사할 HyperTransport 장치
 * @return: HT MSI Mapping 이 켜져 있으면 1, 아니면 0
 *
 * [배경] AMD 계열 시스템에서 PCI 의 MSI 는 HyperTransport 링크를 타고
 * 전달된다. HT 장치에는 'MSI Mapping' 이라는 capability 가 있어, MSI
 * 메모리 쓰기를 인터럽트로 변환할지 여부를 켜고 끌 수 있다. 이것이 꺼져
 * 있으면 MSI 를 써도 인터럽트가 도착하지 않는다.
 * [무엇을 하는가] capability 목록에서 HT MSI Mapping 항목을 찾아 활성
 * 비트를 읽어 알려 준다. 첫 번째로 성공적으로 읽은 항목의 값으로 판정한다.
 *
 * [TTL 로 무한 루프를 막는 이유] capability 목록은 연결 리스트라, 하드웨어가
 * 고장 나 항목이 자기 자신이나 앞쪽을 가리키면 무한 루프에 빠진다.
 * PCI_FIND_CAP_TTL 은 그 방어용 상한이다.
 *
 * 실행 컨텍스트: 아래 두 quirk 안, FINAL/HEADER 단계.
 *
 * 호출 체인:
 *   quirk_msi_ht_cap() / quirk_nvidia_ck804_msi_ht_cap()
 *     -> [msi_ht_cap_enabled] -> pci_find_ht_capability()
 */
/*
 * Go through the list of HyperTransport capabilities and return 1 if a HT
 * MSI capability is found and enabled.
 */
static int msi_ht_cap_enabled(struct pci_dev *dev)
{
	int pos, ttl = PCI_FIND_CAP_TTL;	/* [한국어] pos: 현재 capability 의 오프셋, ttl: 목록 순회 횟수 상한(순환 방어). */

	pos = pci_find_ht_capability(dev, HT_CAPTYPE_MSI_MAPPING);	/* [한국어] HT capability 목록에서 MSI Mapping 형식의 첫 항목을 찾는다. 0 이면 없는 것이다. */
	while (pos && ttl--) {	/* [한국어] 항목이 있고 상한에 걸리지 않는 동안 반복한다. */
		u8 flags;	/* [한국어] 그 항목의 플래그 바이트. */

		if (pci_read_config_byte(dev, pos + HT_MSI_FLAGS,	/* [한국어] 항목 오프셋 + HT_MSI_FLAGS 가 플래그 바이트의 위치다. 읽기에 성공했을 때만 판정한다. */
					 &flags) == 0) {
			pci_info(dev, "Found %s HT MSI Mapping\n",	/* [한국어] 찾은 매핑이 켜져 있는지 꺼져 있는지 남긴다. */
				flags & HT_MSI_FLAGS_ENABLE ?	/* [한국어] 삼항 연산자로 로그 문자열을 고른다. */
				"enabled" : "disabled");
			return (flags & HT_MSI_FLAGS_ENABLE) != 0;	/* [한국어] 활성 비트를 0/1 로 정규화해 반환한다. 첫 항목의 값으로 판정을 끝낸다. */
		}

		pos = pci_find_next_ht_capability(dev, pos,	/* [한국어] 읽기에 실패했다면 다음 MSI Mapping 항목을 찾아 계속한다. */
						  HT_CAPTYPE_MSI_MAPPING);
	}
	return 0;	/* [한국어] 항목을 못 찾았거나 모두 읽기에 실패하면 '켜져 있지 않음'으로 본다 - 안전한 쪽 판정이다. */
}

/*
 * [한국어]
 * quirk_msi_ht_cap - HT MSI Mapping 이 꺼져 있으면 그 아래 버스의 MSI 를 끈다
 *
 * @dev: ServerWorks HT2000 PCIe 브리지
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, HyperTransport MSI 매핑 설정을 보고
 * MSI 가 실제로 쓸 수 있는지 판단해야 한다. 매핑이 꺼져 있는데 MSI 를
 * 쓰면 인터럽트가 도착하지 않는다.
 * [우회] 매핑이 꺼져 있으면 quirk_disable_msi() 를 불러 이 브리지 아래
 * 버스에 MSI 금지 플래그를 세운다. 즉 '하드웨어 설정을 읽어 조건부로
 * 적용하는' quirk 다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_msi_ht_cap]
 *     -> msi_ht_cap_enabled() -> quirk_disable_msi()
 */
/* Check the HyperTransport MSI mapping to know whether MSI is enabled or not */
static void quirk_msi_ht_cap(struct pci_dev *dev)
{
	if (!msi_ht_cap_enabled(dev))	/* [한국어] HT MSI 매핑이 꺼져 있으면 MSI 를 쓸 수 없다. */
		quirk_disable_msi(dev);	/* [한국어] 그 브리지 아래 버스에 MSI 금지 플래그를 세운다. */
}
/* [한국어] ServerWorks HT2000 PCIe 브리지에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_SERVERWORKS, PCI_DEVICE_ID_SERVERWORKS_HT2000_PCIE,
			quirk_msi_ht_cap);

/*
 * [한국어]
 * quirk_nvidia_ck804_msi_ht_cap - CK804 의 두 HT MSI 매핑 중 하나라도 켜져 있으면 인정
 *
 * @dev: NVIDIA CK804 PCIe 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] NVIDIA CK804 칩셋.
 * [무엇이 문제] 아래 영어 주석대로 이 칩셋에는 HT MSI 매핑이 두 개 있을
 * 수 있다. 둘 중 하나라도 MSI capability 가 설정되어 있으면 MSI 를 쓸 수
 * 있는데, 이 장치만 보고 판단하면 잘못된 결론을 내릴 수 있다.
 * [우회] 이 장치와 같은 버스의 슬롯 0 장치(루트 쪽)를 함께 확인한다.
 * 루트 쪽 매핑이 켜져 있으면 MSI 가 된다고 보고 아무것도 하지 않는다.
 * 꺼져 있을 때만 이 장치 자신의 매핑을 검사하는 quirk_msi_ht_cap() 으로
 * 넘긴다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nvidia_ck804_msi_ht_cap]
 *     -> pci_get_slot() -> msi_ht_cap_enabled() -> quirk_msi_ht_cap()
 */
/*
 * The nVidia CK804 chipset may have 2 HT MSI mappings.  MSI is supported
 * if the MSI capability is set in any of these mappings.
 */
static void quirk_nvidia_ck804_msi_ht_cap(struct pci_dev *dev)
{
	struct pci_dev *pdev;	/* [한국어] 같은 버스의 슬롯 0 장치를 담을 포인터. */

	/*
	 * Check HT MSI cap on this chipset and the root one.  A single one
	 * having MSI is enough to be sure that MSI is supported.
	 */
	pdev = pci_get_slot(dev->bus, 0);	/* [한국어] 위 영어 주석대로 이 칩셋과 루트 쪽을 함께 본다. pci_get_slot() 은 참조수를 올려 돌려준다. */
	if (!pdev)	/* [한국어] 슬롯 0 에 아무것도 없으면 판단할 근거가 없으므로 물러난다. */
		return;
	if (!msi_ht_cap_enabled(pdev))	/* [한국어] 루트 쪽 매핑이 켜져 있으면 MSI 가 되므로 아무것도 하지 않는다. */
		quirk_msi_ht_cap(dev);	/* [한국어] 꺼져 있을 때만 이 장치 자신의 매핑까지 확인하는 함수로 넘긴다. */
	pci_dev_put(pdev);	/* [한국어] 조회로 올린 참조수를 놓는다. */
}
/* [한국어] NVIDIA CK804 PCIe 브리지에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA, PCI_DEVICE_ID_NVIDIA_CK804_PCIE,
			quirk_nvidia_ck804_msi_ht_cap);

/*
 * [한국어]
 * ht_enable_msi_mapping - HT 브리지의 MSI Mapping 을 강제로 켠다
 *
 * @dev: ServerWorks HT1000 PXB 또는 AMD 8132 브리지
 * @return: 없음
 *
 * [무엇이 문제] 위 함수들이 '매핑이 꺼져 있으면 MSI 를 포기'하는 쪽이었다면,
 * 이 함수는 반대로 '켤 수 있으니 켜자'는 쪽이다. 아래 영어 주석대로 HT
 * 브리지의 MSI 매핑 capability 를 강제로 활성화한다. 펌웨어가 켜 주지
 * 않아 MSI 를 못 쓰는 상황을 되돌리는 것이다.
 * [그대로 두면] 그 브리지 아래 장치들이 MSI 를 쓰지 못한다.
 * [우회] 찾은 모든 MSI Mapping 항목의 활성 비트를 세운다. 첫 항목에서
 * 멈추지 않고 목록 끝까지 훑는 점이 msi_ht_cap_enabled() 와 다르다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [ht_enable_msi_mapping] -> pci_write_config_byte()
 */
/* Force enable MSI mapping capability on HT bridges */
static void ht_enable_msi_mapping(struct pci_dev *dev)
{
	int pos, ttl = PCI_FIND_CAP_TTL;	/* [한국어] pos: capability 오프셋, ttl: 순환 방어용 상한. */

	pos = pci_find_ht_capability(dev, HT_CAPTYPE_MSI_MAPPING);	/* [한국어] HT MSI Mapping 형식의 첫 항목을 찾는다. */
	while (pos && ttl--) {	/* [한국어] 항목이 있고 상한에 걸리지 않는 동안 계속한다. */
		u8 flags;	/* [한국어] 플래그 바이트. */

		if (pci_read_config_byte(dev, pos + HT_MSI_FLAGS,	/* [한국어] 항목의 플래그를 읽는다. */
					 &flags) == 0) {
			pci_info(dev, "Enabling HT MSI Mapping\n");	/* [한국어] 무엇을 켜는지 남긴다. */

			pci_write_config_byte(dev, pos + HT_MSI_FLAGS,	/* [한국어] 활성 비트를 OR 로 추가해 되쓴다. */
					      flags | HT_MSI_FLAGS_ENABLE);	/* [한국어] HT_MSI_FLAGS_ENABLE 이 그 활성 비트다. */
		}
		pos = pci_find_next_ht_capability(dev, pos,	/* [한국어] 다음 MSI Mapping 항목으로 넘어간다 - 매핑이 여럿일 수 있으므로 전부 켠다. */
						  HT_CAPTYPE_MSI_MAPPING);
	}
}
/* [한국어] ServerWorks HT1000 PXB 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_SERVERWORKS,
			 PCI_DEVICE_ID_SERVERWORKS_HT1000_PXB,
			 ht_enable_msi_mapping);
/* [한국어] AMD 8132 브리지에도 같은 처리를 한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_AMD, PCI_DEVICE_ID_AMD_8132_BRIDGE,
			 ht_enable_msi_mapping);

/*
 * [한국어]
 * nvenet_msi_disable - 특정 ASUS 보드에서 MCP55 NIC 의 MSI 를 끈다
 *
 * @dev: NVIDIA MCP55 내장 이더넷 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] ASUS P5N32-SLI PREMIUM 과 P5N32-E SLI 보드의 MCP55 NIC.
 * [무엇이 문제] 아래 영어 주석대로 이 보드들에서 MCP55 NIC 의 MSI 에
 * 문제가 있다. 같은 문제가 다른 장치에도 있는지는 아직 밝혀지지 않아,
 * 지금은 이 장치의 MSI 만 끈다.
 * [그대로 두면] 네트워크 인터럽트가 도착하지 않는다.
 * [우회] DMI 로 보드 이름을 확인하고, 두 모델 중 하나면 no_msi 를 세운다.
 *
 * [DMI 부분 문자열 비교] strstr() 로 부분 일치를 보는 것은 같은 보드가
 * 리비전 표기 등으로 이름이 조금씩 다를 수 있기 때문이다.
 *
 * 실행 컨텍스트: EARLY 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [nvenet_msi_disable] -> dmi_get_system_info()
 */
/*
 * The P5N32-SLI motherboards from Asus have a problem with MSI
 * for the MCP55 NIC. It is not yet determined whether the MSI problem
 * also affects other devices. As for now, turn off MSI for this device.
 */
static void nvenet_msi_disable(struct pci_dev *dev)
{
	const char *board_name = dmi_get_system_info(DMI_BOARD_NAME);	/* [한국어] DMI 에서 메인보드 이름 문자열을 얻는다. 없으면 NULL 이다. */

	if (board_name &&	/* [한국어] 이름을 얻지 못했으면 판정할 수 없다. */
	    (strstr(board_name, "P5N32-SLI PREMIUM") ||	/* [한국어] P5N32-SLI PREMIUM 이거나 */
	     strstr(board_name, "P5N32-E SLI"))) {	/* [한국어] P5N32-E SLI 이면 대상 보드다. */
		pci_info(dev, "Disabling MSI for MCP55 NIC on P5N32-SLI\n");	/* [한국어] 어떤 보드에서 무엇을 껐는지 남긴다. */
		dev->no_msi = 1;	/* [한국어] 이 장치에 MSI 를 쓰지 않도록 표시한다. */
	}
}
/* [한국어] MCP55 NIC 에 EARLY 단계로 등록한다. 보드 판정은 함수 안에서 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA,
			PCI_DEVICE_ID_NVIDIA_NVENET_15,
			nvenet_msi_disable);

/*
 * PCIe spec r6.0 sec 6.1.4.3 says that if MSI/MSI-X is enabled, the device
 * can't use INTx interrupts. Tegra's PCIe Root Ports don't generate MSI
 * interrupts for PME and AER events; instead only INTx interrupts are
 * generated. Though Tegra's PCIe Root Ports can generate MSI interrupts
 * for other events, since PCIe specification doesn't support using a mix of
 * INTx and MSI/MSI-X, it is required to disable MSI interrupts to avoid port
 * service drivers registering their respective ISRs for MSIs.
 */
/*
 * [한국어]
 * pci_quirk_nvidia_tegra_disable_rp_msi - Tegra 루트 포트의 MSI 를 끈다
 *
 * @dev: NVIDIA Tegra 의 PCIe 루트 포트(PCI 브리지 클래스)
 * @return: 없음
 *
 * [어떤 하드웨어] NVIDIA Tegra SoC 의 PCIe 루트 포트(등록표의 device ID
 * 여러 종).
 * [무엇이 문제] 위 영어 주석이 근거를 정확히 밝힌다. PCIe 스펙 r6.0
 * 6.1.4.3 절은 MSI/MSI-X 가 켜져 있으면 그 장치가 INTx 인터럽트를 쓸 수
 * 없다고 정한다. 그런데 Tegra 의 루트 포트는 PME 와 AER 이벤트에 대해서만은
 * MSI 를 만들지 못하고 INTx 만 낸다. 다른 이벤트에는 MSI 를 낼 수 있지만,
 * 스펙이 INTx 와 MSI 를 섞어 쓰는 것을 허용하지 않으므로 둘 중 하나를
 * 골라야 한다.
 * [그대로 두면] 포트 서비스 드라이버가 MSI 용 인터럽트 처리기를 등록해
 * 두고 기다리는데, PME/AER 은 INTx 로만 올라와 그 이벤트를 놓친다.
 * [우회] no_msi 를 세워 이 루트 포트에서 MSI 를 아예 쓰지 않게 한다.
 * 그러면 모든 이벤트가 INTx 로 일관되게 처리된다.
 *
 * [주의 - 대상은 루트 포트뿐] 이 플래그는 루트 포트 자신에게만 걸린다.
 * 그 아래에 꽂힌 엔드포인트(예: NVMe SSD)의 MSI/MSI-X 사용을 막지는
 * 않는다 - dev->no_msi 는 해당 장치에만 적용되고, 버스 단위로 막는
 * PCI_BUS_FLAGS_NO_MSI 와는 다르다.
 *
 * 실행 컨텍스트: EARLY 단계, 클래스 매칭. 포트 서비스가 인터럽트를
 * 설정하기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [pci_quirk_nvidia_tegra_disable_rp_msi]
 */
static void pci_quirk_nvidia_tegra_disable_rp_msi(struct pci_dev *dev)
{
	dev->no_msi = 1;	/* [한국어] 이 루트 포트에서 MSI 사용을 금지한다 - PME/AER 을 INTx 로 받기 위해서다. */
}
/* [한국어] Tegra 세대별 루트 포트 device ID 를 하나씩 EARLY 단계에 등록한다.
 * 클래스를 PCI 브리지로 함께 지정하는 것은, 같은 device ID 를 브리지가 아닌
 * 다른 기능으로 쓰는 변종을 건드리지 않기 위해서다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x1ad0,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x1ad1,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 이어지는 등록 줄: 0x1ad2. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x1ad2,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0bf0,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 이어지는 등록 줄: 0x0bf1. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0bf1,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0e1c,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 이어지는 등록 줄: 0x0e1d. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0e1d,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0e12,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 이어지는 등록 줄: 0x0e13. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0e13,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0fae,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 이어지는 등록 줄: 0x0faf. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x0faf,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x10e5,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 이어지는 등록 줄: 0x10e6. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x10e6,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x229a,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 이어지는 등록 줄: 0x229c. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x229c,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_NVIDIA, 0x229e,
			      PCI_CLASS_BRIDGE_PCI, 8,
			      pci_quirk_nvidia_tegra_disable_rp_msi);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/*
 * [한국어]
 * nvbridge_check_legacy_irq_routing - MCP55 의 레거시 IRQ 라우팅 레지스터를 바로잡는다
 *
 * @dev: NVIDIA MCP55 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] NVIDIA MCP55 브리지의 일부 버전.
 * [무엇이 문제] 아래 영어 주석대로, 이 브리지에는 레거시 인터럽트 라우팅을
 * 정하는 문서화되지 않은 설정 레지스터가 있다. 잘못 설정되어 있으면
 * 인터럽트가 모든 온라인 CPU 로 브로드캐스트되지 않고 BSP(부트스트랩
 * 프로세서)에만 전달된다.
 * [그대로 두면] kdump 가 제대로 부팅되지 못한다. kdump 커널은 BSP 가 아닌
 * 다른 CPU 에서 도는 경우가 있어, 인터럽트가 BSP 로만 가면 받을 수 없다.
 * [우회] 벤더 전용 레지스터 0x74 의 비트 2 와 비트 15 를 지워 브로드캐스트
 * 동작으로 되돌린다.
 *
 * [문서화되지 않은 레지스터] 영어 주석이 스스로 밝히듯 이 레지스터는
 * 공개 문서에 없다. 비트의 정확한 의미는 이 트리의 정보만으로는 확인할
 * 수 없고, 알려진 것은 '두 비트를 지우면 라우팅이 정상이 된다'는 사실뿐이다.
 *
 * 실행 컨텍스트: EARLY 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [nvbridge_check_legacy_irq_routing]
 *     -> pci_write_config_dword()
 */
/*
 * Some versions of the MCP55 bridge from Nvidia have a legacy IRQ routing
 * config register.  This register controls the routing of legacy
 * interrupts from devices that route through the MCP55.  If this register
 * is misprogrammed, interrupts are only sent to the BSP, unlike
 * conventional systems where the IRQ is broadcast to all online CPUs.  Not
 * having this register set properly prevents kdump from booting up
 * properly, so let's make sure that we have it set correctly.
 * Note that this is an undocumented register.
 */
static void nvbridge_check_legacy_irq_routing(struct pci_dev *dev)
{
	u32 cfg;	/* [한국어] 0x74 라우팅 레지스터 값. */

	if (!pci_find_capability(dev, PCI_CAP_ID_HT))	/* [한국어] HyperTransport capability 가 있는 버전만 이 레지스터를 갖는다. 없으면 대상이 아니다. */
		return;

	pci_read_config_dword(dev, 0x74, &cfg);	/* [한국어] 문서화되지 않은 레지스터 0x74 를 읽는다. */

	if (cfg & ((1 << 2) | (1 << 15))) {	/* [한국어] 비트 2 또는 비트 15 가 서 있으면 라우팅이 잘못 설정된 상태다. */
		pr_info("Rewriting IRQ routing register on MCP55\n");	/* [한국어] 고친다는 사실을 남긴다. pci_info 가 아니라 pr_info 를 쓰는 것은 이 코드가 오래된 형태이기 때문이다. */
		cfg &= ~((1 << 2) | (1 << 15));	/* [한국어] 두 비트를 함께 지운다. */
		pci_write_config_dword(dev, 0x74, cfg);	/* [한국어] 고친 값을 되쓴다. */
	}
}
/* [한국어] MCP55 브리지의 두 버전에 EARLY 단계로 등록한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA,
			PCI_DEVICE_ID_NVIDIA_MCP55_BRIDGE_V0,
			nvbridge_check_legacy_irq_routing);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_NVIDIA,
			PCI_DEVICE_ID_NVIDIA_MCP55_BRIDGE_V4,
			nvbridge_check_legacy_irq_routing);

/*
 * [한국어]
 * ht_check_msi_mapping - HT MSI Mapping 의 존재 여부와 활성 여부를 3단계로 구분
 *
 * @dev: 검사할 장치
 * @return: 0 = MSI Mapping capability 자체가 없음,
 *          1 = 있지만 어느 것도 켜져 있지 않음,
 *          2 = 있고 하나 이상 켜져 있음
 *
 * [왜 3단계인가] 앞의 msi_ht_cap_enabled() 는 켜짐/꺼짐만 알려 주는데,
 * 아래 __nv_msi_ht_cap_quirk() 는 '없다'와 '있지만 꺼져 있다'를 구별해야
 * 한다. 없으면 손댈 것이 없고, 있는데 꺼져 있으면 켜 볼 여지가 있기
 * 때문이다.
 *
 * 실행 컨텍스트: 아래 HT MSI quirk 들 안, FINAL/RESUME_EARLY 단계.
 *
 * 호출 체인:
 *   __nv_msi_ht_cap_quirk() / host_bridge_with_leaf()
 *     -> [ht_check_msi_mapping] -> pci_find_ht_capability()
 */
static int ht_check_msi_mapping(struct pci_dev *dev)
{
	int pos, ttl = PCI_FIND_CAP_TTL;	/* [한국어] pos: capability 오프셋, ttl: 순환 방어용 상한. */
	int found = 0;	/* [한국어] 판정 결과를 누적하는 변수. 0 에서 시작해 최대 2 까지 올라간다. */

	/* Check if there is HT MSI cap or enabled on this device */
	pos = pci_find_ht_capability(dev, HT_CAPTYPE_MSI_MAPPING);	/* [한국어] HT MSI Mapping 형식의 첫 항목을 찾는다. */
	while (pos && ttl--) {	/* [한국어] 항목이 있고 상한에 걸리지 않는 동안 반복한다. */
		u8 flags;	/* [한국어] 항목의 플래그 바이트. */

		if (found < 1)	/* [한국어] 항목이 하나라도 있으면 최소 1 이다. */
			found = 1;	/* [한국어] 'capability 는 있다' 로 올린다. */
		if (pci_read_config_byte(dev, pos + HT_MSI_FLAGS,	/* [한국어] 플래그를 읽는다. */
					 &flags) == 0) {
			if (flags & HT_MSI_FLAGS_ENABLE) {	/* [한국어] 활성 비트가 서 있으면 */
				if (found < 2) {	/* [한국어] 아직 2 로 올리지 않았다면 */
					found = 2;	/* [한국어] '있고 켜져 있다' 로 올리고 */
					break;	/* [한국어] 더 볼 필요가 없으므로 루프를 끝낸다. */
				}
			}
		}
		pos = pci_find_next_ht_capability(dev, pos,	/* [한국어] 다음 MSI Mapping 항목으로 넘어간다. */
						  HT_CAPTYPE_MSI_MAPPING);
	}

	return found;	/* [한국어] 0/1/2 중 하나를 돌려준다. */
}

/*
 * [한국어]
 * host_bridge_with_leaf - 이 호스트 브리지 아래에 HT MSI 를 쓰는 장치가 있는지 본다
 *
 * @host_bridge: 검사 기준이 되는 호스트 브리지
 * @return: 그런 장치가 있으면 1, 없으면 0
 *
 * [무엇을 하는가] 같은 버스에서 이 호스트 브리지 다음 슬롯부터 위로 훑으며,
 * (a) 다음 호스트 브리지를 만나면 거기서 멈추고(그 뒤는 다른 브리지의
 * 영역이다), (b) 그 전에 HT MSI Mapping 을 가진 장치를 찾으면 1 을
 * 돌려준다.
 *
 * [왜 이런 탐색이 필요한가] HyperTransport 시스템에서는 여러 호스트
 * 브리지가 하나의 버스 번호 공간을 나눠 쓴다. '이 브리지에 딸린 장치'를
 * 슬롯 번호 범위로 추정할 수밖에 없다.
 *
 * 실행 컨텍스트: nv_ht_enable_msi_mapping() 안.
 *
 * 호출 체인:
 *   nv_ht_enable_msi_mapping() -> [host_bridge_with_leaf]
 *     -> pci_get_slot() -> ht_check_msi_mapping()
 */
static int host_bridge_with_leaf(struct pci_dev *host_bridge)
{
	struct pci_dev *dev;	/* [한국어] 훑는 중인 장치. */
	int pos;	/* [한국어] capability 오프셋. */
	int i, dev_no;	/* [한국어] i: 슬롯 번호 반복자, dev_no: 기준 브리지의 슬롯 번호. */
	int found = 0;	/* [한국어] 찾았는지 여부. */

	dev_no = host_bridge->devfn >> 3;	/* [한국어] devfn 을 3비트 오른쪽으로 밀면 슬롯(디바이스) 번호가 나온다. 하위 3비트는 함수 번호다. */
	for (i = dev_no + 1; i < 0x20; i++) {	/* [한국어] 기준 브리지 다음 슬롯부터 최대 슬롯 번호(0x1f)까지 훑는다. */
		dev = pci_get_slot(host_bridge->bus, PCI_DEVFN(i, 0));	/* [한국어] 각 슬롯의 함수 0 을 조회한다. 참조수가 올라간다. */
		if (!dev)	/* [한국어] 그 슬롯이 비어 있으면 건너뛴다. */
			continue;

		/* found next host bridge? */
		pos = pci_find_ht_capability(dev, HT_CAPTYPE_SLAVE);	/* [한국어] 위 영어 주석대로 HT Slave capability 가 있으면 그것이 다음 호스트 브리지다. */
		if (pos != 0) {	/* [한국어] 다음 호스트 브리지를 만났다면 */
			pci_dev_put(dev);	/* [한국어] 참조수를 놓고 */
			break;	/* [한국어] 탐색을 끝낸다 - 그 뒤는 이 브리지의 영역이 아니다. */
		}

		if (ht_check_msi_mapping(dev)) {	/* [한국어] HT MSI Mapping 을 가진 장치를 찾았다면 */
			found = 1;	/* [한국어] 찾았다고 표시하고 */
			pci_dev_put(dev);	/* [한국어] 참조수를 놓고 */
			break;	/* [한국어] 탐색을 끝낸다. */
		}
		pci_dev_put(dev);	/* [한국어] 해당 없는 장치였으므로 참조수만 놓고 다음 슬롯으로 간다. */
	}

	return found;	/* [한국어] 찾았으면 1, 아니면 0. */
}

/* [한국어] 아래 두 매크로는 HT Slave capability 안의 링크 제어 레지스터
 * 오프셋이다. HT 슬레이브에는 링크가 둘이라 제어 레지스터도 두 벌이며,
 * 어느 쪽이 '이 장치로 들어오는' 링크인지는 capability 플래그로 알 수 있다. */
#define PCI_HT_CAP_SLAVE_CTRL0     4    /* link control */
#define PCI_HT_CAP_SLAVE_CTRL1     8    /* link control to */

/*
 * [한국어]
 * is_end_of_ht_chain - 이 장치가 HyperTransport 사슬의 끝인지 판정한다
 *
 * @dev: 검사할 HT 장치
 * @return: 사슬의 끝이면 1, 아니면 0
 *
 * [HT 사슬이란] HyperTransport 는 장치들을 데이지 체인으로 잇는다. 사슬의
 * 끝에 있는 장치는 아래로 더 내려보낼 곳이 없다.
 * [무엇을 하는가] HT Slave capability 의 링크 제어 레지스터에서 '끝단'을
 * 나타내는 비트 6 을 읽어 판정한다. 두 개의 링크 제어 레지스터 중 어느
 * 것을 볼지는 capability 플래그의 비트 10 으로 고른다.
 *
 * [비트의 근거] 비트 10 과 비트 6 의 정확한 의미는 HyperTransport 스펙에
 * 있으며 원본 주석에는 설명이 없다. 이 트리의 정보만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: nv_ht_enable_msi_mapping() 안.
 *
 * 호출 체인:
 *   nv_ht_enable_msi_mapping() -> [is_end_of_ht_chain]
 *     -> pci_find_ht_capability()
 */
static int is_end_of_ht_chain(struct pci_dev *dev)
{
	int pos, ctrl_off;	/* [한국어] pos: capability 오프셋, ctrl_off: 볼 링크 제어 레지스터의 오프셋. */
	int end = 0;	/* [한국어] 판정 결과. 기본은 '끝이 아님'. */
	u16 flags, ctrl;	/* [한국어] flags: capability 플래그, ctrl: 링크 제어 레지스터 값. */

	pos = pci_find_ht_capability(dev, HT_CAPTYPE_SLAVE);	/* [한국어] HT Slave capability 를 찾는다. */

	if (!pos)	/* [한국어] 없으면 HT 슬레이브가 아니므로 판정할 수 없다. */
		goto out;

	pci_read_config_word(dev, pos + PCI_CAP_FLAGS, &flags);	/* [한국어] capability 오프셋 + PCI_CAP_FLAGS(2)가 플래그 워드의 위치다. */

	ctrl_off = ((flags >> 10) & 1) ?	/* [한국어] 플래그의 비트 10 으로 두 링크 중 어느 쪽 제어 레지스터를 볼지 고른다. */
			PCI_HT_CAP_SLAVE_CTRL0 : PCI_HT_CAP_SLAVE_CTRL1;	/* [한국어] 비트가 서 있으면 CTRL0(오프셋 4), 아니면 CTRL1(오프셋 8). */
	pci_read_config_word(dev, pos + ctrl_off, &ctrl);	/* [한국어] 고른 링크 제어 레지스터를 읽는다. */

	if (ctrl & (1 << 6))	/* [한국어] 비트 6 이 사슬의 끝을 나타낸다. */
		end = 1;	/* [한국어] 끝이라고 표시한다. */

out:	/* [한국어] 실패 경로와 정상 경로가 함께 나가는 지점. */
	return end;	/* [한국어] 0 또는 1 을 돌려준다. */
}

/*
 * [한국어]
 * nv_ht_enable_msi_mapping - 조건을 따져 이 장치의 HT MSI Mapping 을 켠다
 *
 * @dev: HT MSI Mapping 을 켤 후보 장치
 * @return: 없음
 *
 * [무엇을 하는가] 무턱대고 켜지 않고 두 가지 예외를 걸러 낸다.
 * (1) 아래 영어 주석대로, 자기 자신이 호스트 브리지이면서 HT 사슬의 끝이고
 *     그 아래에 이미 HT MSI 를 쓰는 장치가 있으면 여기서 직접 켜지 않는다.
 * (2) 루트(호스트 브리지)에서 이미 켜 두었으면 - 영어 주석의 표현대로
 *     'root did that' - 다시 켤 필요가 없다.
 * 그 밖의 경우에만 ht_enable_msi_mapping() 으로 실제로 켠다.
 *
 * [호스트 브리지를 어떻게 찾는가] 같은 버스에서 자기 슬롯 번호부터
 * 아래로 내려가며 HT Slave capability 를 가진 첫 장치를 찾는다. HT
 * 시스템에서 호스트 브리지가 슬롯 번호가 더 작은 쪽에 있다는 관례를
 * 이용한 탐색이다.
 *
 * 실행 컨텍스트: __nv_msi_ht_cap_quirk() 안, FINAL/RESUME_EARLY.
 *
 * 호출 체인:
 *   __nv_msi_ht_cap_quirk() -> [nv_ht_enable_msi_mapping]
 *     -> is_end_of_ht_chain() / host_bridge_with_leaf() / ht_enable_msi_mapping()
 */
static void nv_ht_enable_msi_mapping(struct pci_dev *dev)
{
	struct pci_dev *host_bridge;	/* [한국어] 찾아낸 호스트 브리지. */
	int pos;	/* [한국어] capability 오프셋. */
	int i, dev_no;	/* [한국어] i: 슬롯 반복자, dev_no: 이 장치의 슬롯 번호. */
	int found = 0;	/* [한국어] 호스트 브리지를 찾았는지 여부. */

	dev_no = dev->devfn >> 3;	/* [한국어] 자신의 슬롯 번호를 구한다. */
	for (i = dev_no; i >= 0; i--) {	/* [한국어] 자기 슬롯부터 0 번까지 내려가며 훑는다. */
		host_bridge = pci_get_slot(dev->bus, PCI_DEVFN(i, 0));	/* [한국어] 각 슬롯의 함수 0 을 조회한다. 참조수가 올라간다. */
		if (!host_bridge)	/* [한국어] 빈 슬롯은 건너뛴다. */
			continue;

		pos = pci_find_ht_capability(host_bridge, HT_CAPTYPE_SLAVE);	/* [한국어] HT Slave capability 가 있으면 호스트 브리지다. */
		if (pos != 0) {	/* [한국어] 찾았다면 */
			found = 1;	/* [한국어] 표시하고 */
			break;	/* [한국어] 루프를 끝낸다. 이때 참조수를 놓지 않는 것은 아래에서 계속 쓰기 때문이며, out 레이블에서 놓는다. */
		}
		pci_dev_put(host_bridge);	/* [한국어] 호스트 브리지가 아니었으므로 참조수를 놓고 다음으로 간다. */
	}

	if (!found)	/* [한국어] 호스트 브리지를 못 찾았으면 판단 근거가 없어 아무것도 하지 않는다. 이 경로에서는 참조수를 잡은 것이 없다. */
		return;

	/* don't enable end_device/host_bridge with leaf directly here */
	if (host_bridge == dev && is_end_of_ht_chain(host_bridge) &&	/* [한국어] 예외 (1): 자기 자신이 호스트 브리지이고 HT 사슬의 끝이면서 */
	    host_bridge_with_leaf(host_bridge))	/* [한국어] 그 아래에 이미 HT MSI 를 쓰는 장치가 있으면 여기서 켜지 않는다. */
		goto out;

	/* root did that ! */
	if (msi_ht_cap_enabled(host_bridge))	/* [한국어] 예외 (2): 루트가 이미 켜 두었으면 다시 켤 필요가 없다. */
		goto out;

	ht_enable_msi_mapping(dev);	/* [한국어] 예외에 걸리지 않았으므로 이 장치의 HT MSI Mapping 을 켠다. */

out:	/* [한국어] 공통 정리 지점. */
	pci_dev_put(host_bridge);	/* [한국어] 위 루프에서 잡아 둔 호스트 브리지 참조수를 놓는다. */
}

/*
 * [한국어]
 * ht_disable_msi_mapping - HT 장치의 MSI Mapping 을 모두 끈다
 *
 * @dev: 대상 장치
 * @return: 없음
 *
 * ht_enable_msi_mapping() 의 반대다. 비HT 호스트 브리지 아래에 있는
 * 장치에서는 HT MSI 매핑이 오히려 잘못된 동작을 낳으므로 꺼야 한다
 * (아래 __nv_msi_ht_cap_quirk() 의 판단 참조).
 *
 * 실행 컨텍스트: __nv_msi_ht_cap_quirk() 안.
 *
 * 호출 체인:
 *   __nv_msi_ht_cap_quirk() -> [ht_disable_msi_mapping]
 *     -> pci_write_config_byte()
 */
static void ht_disable_msi_mapping(struct pci_dev *dev)
{
	int pos, ttl = PCI_FIND_CAP_TTL;	/* [한국어] pos: capability 오프셋, ttl: 순환 방어용 상한. */

	pos = pci_find_ht_capability(dev, HT_CAPTYPE_MSI_MAPPING);	/* [한국어] HT MSI Mapping 형식의 첫 항목을 찾는다. */
	while (pos && ttl--) {	/* [한국어] 항목이 있고 상한에 걸리지 않는 동안 반복한다. */
		u8 flags;	/* [한국어] 플래그 바이트. */

		if (pci_read_config_byte(dev, pos + HT_MSI_FLAGS,	/* [한국어] 항목의 플래그를 읽는다. */
					 &flags) == 0) {
			pci_info(dev, "Disabling HT MSI Mapping\n");	/* [한국어] 무엇을 끄는지 남긴다. */

			pci_write_config_byte(dev, pos + HT_MSI_FLAGS,	/* [한국어] 활성 비트만 지워 되쓴다. */
					      flags & ~HT_MSI_FLAGS_ENABLE);	/* [한국어] HT_MSI_FLAGS_ENABLE 의 여집합과 AND 한다. */
		}
		pos = pci_find_next_ht_capability(dev, pos,	/* [한국어] 다음 MSI Mapping 항목으로 넘어간다 - 매핑이 여럿이면 전부 끈다. */
						  HT_CAPTYPE_MSI_MAPPING);
	}
}

/*
 * [한국어]
 * __nv_msi_ht_cap_quirk - 호스트 브리지의 종류에 맞춰 HT MSI Mapping 을 켜거나 끈다
 *
 * @dev: 대상 장치
 * @all: 1 이면 조건 없이 켠다, 0 이면 nv_ht_enable_msi_mapping() 의 조건 판정을 거친다
 * @return: 없음
 *
 * [핵심 규칙] 아래 영어 주석이 명확히 밝힌다. HT MSI 매핑은 '비
 * HyperTransport 호스트 브리지 아래에 있는 장치'에서는 꺼야 한다. 반대로
 * HT 호스트 브리지 아래라면 켜져 있어야 MSI 가 동작한다. 그래서 이 함수는
 * 도메인의 0:00.0 장치(호스트 브리지)를 찾아 그것이 HT 인지 보고 방향을
 * 정한다.
 *
 * [판단 표]
 *   - MSI Mapping 자체가 없음(found==0): 할 일 없음.
 *   - 호스트 브리지가 HT 이고 매핑이 꺼져 있음(found==1): 켠다.
 *   - 호스트 브리지가 HT 이고 매핑이 켜져 있음(found==2): 그대로 둔다.
 *   - 호스트 브리지가 비HT 이고 매핑이 꺼져 있음(found==1): 그대로 둔다.
 *   - 호스트 브리지가 비HT 이고 매핑이 켜져 있음(found==2): 끈다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY. MSI 가 빌드에 없으면 바로 돌아간다.
 *
 * 호출 체인:
 *   nv_msi_ht_cap_quirk_all() / nv_msi_ht_cap_quirk_leaf()
 *     -> [__nv_msi_ht_cap_quirk]
 *     -> ht_check_msi_mapping() / ht_enable_msi_mapping() / ht_disable_msi_mapping()
 */
static void __nv_msi_ht_cap_quirk(struct pci_dev *dev, int all)
{
	struct pci_dev *host_bridge;	/* [한국어] 도메인의 호스트 브리지. */
	int pos;	/* [한국어] capability 오프셋. */
	int found;	/* [한국어] ht_check_msi_mapping() 의 3단계 판정 결과. */

	if (!pci_msi_enabled())	/* [한국어] MSI 가 꺼져 있는 커널이나 부팅 옵션이면 할 일이 없다. */
		return;

	/* check if there is HT MSI cap or enabled on this device */
	found = ht_check_msi_mapping(dev);	/* [한국어] 이 장치의 HT MSI 매핑 상태를 3단계로 조사한다. */

	/* no HT MSI CAP */
	if (found == 0)	/* [한국어] 매핑 capability 자체가 없으면 손댈 것이 없다. */
		return;

	/*
	 * HT MSI mapping should be disabled on devices that are below
	 * a non-HyperTransport host bridge. Locate the host bridge.
	 */
	host_bridge = pci_get_domain_bus_and_slot(pci_domain_nr(dev->bus), 0,	/* [한국어] 위 영어 주석대로 호스트 브리지를 찾는다. 같은 PCI 도메인의 버스 0, 디바이스 0, 함수 0 이 관례상 호스트 브리지다. */
						  PCI_DEVFN(0, 0));
	if (host_bridge == NULL) {	/* [한국어] 호스트 브리지를 못 찾으면 방향을 정할 수 없다. */
		pci_warn(dev, "nv_msi_ht_cap_quirk didn't locate host bridge\n");	/* [한국어] 찾지 못했음을 경고로 남긴다. */
		return;
	}

	pos = pci_find_ht_capability(host_bridge, HT_CAPTYPE_SLAVE);	/* [한국어] 호스트 브리지에 HT Slave capability 가 있으면 HyperTransport 브리지다. */
	if (pos != 0) {	/* [한국어] HT 호스트 브리지인 경우 */
		/* Host bridge is to HT */
		if (found == 1) {	/* [한국어] 매핑은 있는데 켜져 있지 않다면 켜 볼 여지가 있다. */
			/* it is not enabled, try to enable it */
			if (all)	/* [한국어] all 이면 조건 없이 켠다(ALi 계열). */
				ht_enable_msi_mapping(dev);	/* [한국어] 모든 매핑을 켠다. */
			else
				nv_ht_enable_msi_mapping(dev);	/* [한국어] 예외 조건을 따진 뒤에만 켠다. */
		}
		goto out;	/* [한국어] 이미 켜져 있었다면 아무것도 하지 않고 나간다. */
	}

	/* HT MSI is not enabled */
	if (found == 1)	/* [한국어] 비HT 호스트 브리지이면서 매핑이 꺼져 있으면 그대로 두는 것이 맞다. */
		goto out;

	/* Host bridge is not to HT, disable HT MSI mapping on this device */
	ht_disable_msi_mapping(dev);	/* [한국어] 비HT 호스트 브리지인데 매핑이 켜져 있다면 잘못된 상태이므로 끈다. */

out:	/* [한국어] 공통 정리 지점. */
	pci_dev_put(host_bridge);	/* [한국어] 호스트 브리지 참조수를 놓는다. */
}

/*
 * [한국어]
 * nv_msi_ht_cap_quirk_all - 조건 없이 켜는 쪽으로 __nv_msi_ht_cap_quirk() 를 부른다
 *
 * @dev: ALi 벤더의 모든 장치
 * @return: 없음(void 함수를 return 문으로 감싸 호출한 형태)
 *
 * DECLARE_PCI_FIXUP_* 는 인자가 하나뿐인 함수만 등록할 수 있으므로,
 * all 인자를 고정한 얇은 껍데기 함수를 두 개 만들어 각각 등록한다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [nv_msi_ht_cap_quirk_all] -> __nv_msi_ht_cap_quirk()
 */
static void nv_msi_ht_cap_quirk_all(struct pci_dev *dev)
{
	return __nv_msi_ht_cap_quirk(dev, 1);	/* [한국어] all=1 - 조건 판정 없이 켠다. void 함수를 return 으로 감싼 것은 C 에서 허용되는 관용이다. */
}
/* [한국어] ALi 벤더의 모든 장치에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_AL, PCI_ANY_ID, nv_msi_ht_cap_quirk_all);
/* [한국어] 재개 시에도 다시 판정한다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_AL, PCI_ANY_ID, nv_msi_ht_cap_quirk_all);

/*
 * [한국어]
 * nv_msi_ht_cap_quirk_leaf - 조건 판정을 거치는 쪽으로 __nv_msi_ht_cap_quirk() 를 부른다
 *
 * @dev: NVIDIA 벤더의 모든 장치
 * @return: 없음
 *
 * all=0 이므로 nv_ht_enable_msi_mapping() 의 예외 판정을 거친다. NVIDIA
 * 칩셋은 사슬 끝단과 루트의 상태를 함께 봐야 해서 이 쪽을 쓴다.
 *
 * 실행 컨텍스트: FINAL 과 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [nv_msi_ht_cap_quirk_leaf] -> __nv_msi_ht_cap_quirk()
 */
static void nv_msi_ht_cap_quirk_leaf(struct pci_dev *dev)
{
	return __nv_msi_ht_cap_quirk(dev, 0);	/* [한국어] all=0 - 예외 조건을 따져 켠다. */
}
/* [한국어] NVIDIA 벤더의 모든 장치에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, nv_msi_ht_cap_quirk_leaf);
/* [한국어] 재개 시에도 다시 판정한다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID, nv_msi_ht_cap_quirk_leaf);

/*
 * [한국어]
 * quirk_msi_intx_disable_bug - MSI 를 켤 때 INTx 를 끄면 안 되는 장치를 표시
 *
 * @dev: 아래 등록표의 Broadcom Tigon3 / ATI / Attansic 장치들
 * @return: 없음
 *
 * [배경] PCI 스펙상 MSI 를 켜면 그 장치는 레거시 INTx 인터럽트를 쏘면
 * 안 된다. 그래서 커널은 MSI 를 켤 때 Command 레지스터의 INTx Disable
 * 비트를 함께 세운다.
 * [무엇이 문제] 이 장치들은 INTx Disable 비트를 세우면 MSI 마저 나오지
 * 않는다. 두 인터럽트 경로가 하드웨어 안에서 잘못 얽혀 있는 것이다.
 * [그대로 두면] MSI 를 켠 순간 인터럽트가 완전히 끊긴다.
 * [우회] dev_flags 에 PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG 를 세워, MSI 를
 * 켤 때 INTx Disable 비트를 건드리지 않게 한다. 스펙에는 어긋나지만
 * 실제 하드웨어가 그것을 요구한다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_msi_intx_disable_bug]
 */
static void quirk_msi_intx_disable_bug(struct pci_dev *dev)
{
	dev->dev_flags |= PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG;	/* [한국어] MSI 활성화 시 INTx Disable 비트를 세우지 말라는 표시. */
}

/*
 * [한국어]
 * quirk_msi_intx_disable_ati_bug - SB700 리비전을 확인해 조건부로 같은 표시를 한다
 *
 * @dev: ATI SB700 계열 장치(0x4390~0x4394)
 * @return: 없음
 *
 * [어떤 하드웨어] AMD/ATI SB700 사우스브리지 계열.
 * [무엇이 문제] 위 quirk_msi_intx_disable_bug() 와 같은 결함이지만, 아래
 * 영어 주석대로 리비전 A21 부터 하드웨어에서 고쳐졌다.
 * [리비전을 어떻게 아는가] 문제의 장치 자신이 아니라 같은 칩의 SMBus
 * 컨트롤러의 Revision ID 를 봐야 SB700 의 실리콘 리비전을 알 수 있다.
 * 그래서 pci_get_device() 로 SMBus 함수를 찾아가 리비전을 읽는다.
 * [우회] 리비전이 0x30 이상 0x3B 미만인 실리콘에만 플래그를 세운다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_msi_intx_disable_ati_bug]
 *     -> pci_get_device() -> pci_dev_put()
 */
static void quirk_msi_intx_disable_ati_bug(struct pci_dev *dev)
{
	struct pci_dev *p;	/* [한국어] 리비전을 읽어 올 SMBus 컨트롤러. */

	/*
	 * SB700 MSI issue will be fixed at HW level from revision A21;
	 * we need check PCI REVISION ID of SMBus controller to get SB700
	 * revision.
	 */
	p = pci_get_device(PCI_VENDOR_ID_ATI, PCI_DEVICE_ID_ATI_SBX00_SMBUS,	/* [한국어] 위 영어 주석대로 SB700 의 리비전은 SMBus 컨트롤러의 Revision ID 로 알 수 있다. 참조수가 올라간다. */
			   NULL);
	if (!p)	/* [한국어] SMBus 함수를 못 찾으면 리비전을 판정할 수 없으므로 손대지 않는다. */
		return;

	if ((p->revision < 0x3B) && (p->revision >= 0x30))	/* [한국어] 0x30 이상 0x3B 미만 - A21(0x3B) 미만의 결함 있는 실리콘 범위다. */
		dev->dev_flags |= PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG;	/* [한국어] 그 범위일 때만 플래그를 세운다. */
	pci_dev_put(p);	/* [한국어] 조회로 올린 참조수를 놓는다. */
}

/*
 * [한국어]
 * quirk_msi_intx_disable_qca_bug - QCA/Attansic NIC 의 리비전을 보고 같은 표시를 한다
 *
 * @dev: Attansic/QCA AR816X/AR817X/E210X 이더넷 컨트롤러
 * @return: 없음
 *
 * [어떤 하드웨어] Atheros/QCA AR816X, AR817X, E210X 이더넷 컨트롤러.
 * [무엇이 문제] 위와 같은 MSI/INTx Disable 결함이다. 아래 영어 주석대로
 * 리비전 0x18 부터 하드웨어에서 고쳐졌다.
 * [ATI 판과의 차이] 이 칩은 자기 자신의 Revision ID 로 판정할 수 있어
 * 다른 장치를 찾아갈 필요가 없다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_msi_intx_disable_qca_bug]
 */
static void quirk_msi_intx_disable_qca_bug(struct pci_dev *dev)
{
	/* AR816X/AR817X/E210X MSI is fixed at HW level from revision 0x18 */
	if (dev->revision < 0x18) {	/* [한국어] 옆의 영어 주석대로 리비전 0x18 미만만 결함이 있다. */
		pci_info(dev, "set MSI_INTX_DISABLE_BUG flag\n");	/* [한국어] 플래그를 세운다는 사실을 남긴다. */
		dev->dev_flags |= PCI_DEV_FLAGS_MSI_INTX_DISABLE_BUG;	/* [한국어] MSI 활성화 시 INTx Disable 비트를 건드리지 말라는 표시. */
	}
}
/* [한국어] Broadcom Tigon3 계열 6종에 FINAL 단계로 등록한다 - 리비전과
 * 무관하게 결함이 있는 모델들이다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_BROADCOM,
			PCI_DEVICE_ID_TIGON3_5780,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_BROADCOM,
			PCI_DEVICE_ID_TIGON3_5780S,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_BROADCOM,
			PCI_DEVICE_ID_TIGON3_5714,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_BROADCOM,
			PCI_DEVICE_ID_TIGON3_5714S,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_BROADCOM,
			PCI_DEVICE_ID_TIGON3_5715,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_BROADCOM,
			PCI_DEVICE_ID_TIGON3_5715S,
			quirk_msi_intx_disable_bug);

/* [한국어] ATI SB700 계열 5종은 리비전 판정이 필요한 쪽으로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4390,
			quirk_msi_intx_disable_ati_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4391,
			quirk_msi_intx_disable_ati_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4392,
			quirk_msi_intx_disable_ati_bug);
/* [한국어] 이어지는 등록 줄: 0x4393, 0x4394. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4393,
			quirk_msi_intx_disable_ati_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4394,
			quirk_msi_intx_disable_ati_bug);

/* [한국어] ATI 0x4373~0x4375 는 리비전과 무관하게 결함이 있어 무조건 표시한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4373,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4374,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x4375,
			quirk_msi_intx_disable_bug);

/* [한국어] Attansic 구형 모델들은 무조건 표시하고(아래 6종), 신형
 * AR816X/AR817X/E210X 계열은 리비전 판정을 거치는 함수로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x1062,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x1063,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x2060,
			quirk_msi_intx_disable_bug);
/* [한국어] 이어지는 등록 줄: 0x2062, 0x1073. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x2062,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x1073,
			quirk_msi_intx_disable_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x1083,
			quirk_msi_intx_disable_bug);
/* [한국어] 이어지는 등록 줄: 0x1090, 0x1091. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x1090,
			quirk_msi_intx_disable_qca_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x1091,
			quirk_msi_intx_disable_qca_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x10a0,
			quirk_msi_intx_disable_qca_bug);
/* [한국어] 이어지는 등록 줄: 0x10a1, 0xe091. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0x10a1,
			quirk_msi_intx_disable_qca_bug);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATTANSIC, 0xe091,
			quirk_msi_intx_disable_qca_bug);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/*
 * [한국어]
 * quirk_al_msi_disable - Annapurna Labs 루트 포트의 MSI/MSI-X 를 끈다
 *
 * @dev: Amazon Annapurna Labs device 0x0031 중 PCI 브리지 클래스인 것
 * @return: 없음
 *
 * [어떤 하드웨어] Amazon Annapurna Labs 의 1c36:0031 루트 포트.
 * [무엇이 문제] 아래 영어 주석대로 이 루트 포트는 MSI-X 를 지원하지 않는데
 * 일부 플랫폼에서 잘못 광고한다. capability 로 거짓말을 하는 전형적인 예다.
 * [그대로 두면] MSI-X 를 켜도 인터럽트가 오지 않는다.
 * [우회] no_msi 를 세운다. 영어 주석이 스스로 밝히듯 이 플래그는 MSI 까지
 * 함께 끈다 - MSI 는 동작할지도 모르지만 시험되지 않았고, 지금은 MSI-X 만
 * 골라 끄는 표준적인 방법이 없기 때문이다.
 *
 * [클래스로 좁히는 이유] 영어 주석대로 0x0031 이라는 device ID 는 루트
 * 포트가 아닌 다른 장치 종류에도 재사용된다. 그래서 PCI 브리지 클래스인
 * 것만 고르도록 CLASS 계열 매크로로 등록한다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_al_msi_disable]
 */
/*
 * Amazon's Annapurna Labs 1c36:0031 Root Ports don't support MSI-X, so it
 * should be disabled on platforms where the device (mistakenly) advertises it.
 *
 * Notice that this quirk also disables MSI (which may work, but hasn't been
 * tested), since currently there is no standard way to disable only MSI-X.
 *
 * The 0031 device id is reused for other non Root Port device types,
 * therefore the quirk is registered for the PCI_CLASS_BRIDGE_PCI class.
 */
static void quirk_al_msi_disable(struct pci_dev *dev)
{
	dev->no_msi = 1;	/* [한국어] 이 장치에서 MSI 와 MSI-X 를 모두 금지한다. */
	pci_warn(dev, "Disabling MSI/MSI-X\n");	/* [한국어] 무엇을 껐는지 경고로 남긴다. */
}
/* [한국어] 벤더 Annapurna Labs, device 0x0031, 클래스 PCI 브리지인
 * 것에만 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_AMAZON_ANNAPURNA_LABS, 0x0031,
			      PCI_CLASS_BRIDGE_PCI, 8, quirk_al_msi_disable);
/* [한국어] CONFIG_PCI_MSI 블록의 끝 - 여기까지가 MSI 관련 quirk 다. */
#endif /* CONFIG_PCI_MSI */

/*
 * [한국어]
 * quirk_hotplug_bridge - 이 브리지를 핫플러그 브리지로 표시한다
 *
 * @dev: HINT 0x0020 (PLX 6254, 옛 HINT HB6) 브리지
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, 일부 PCI-PCI 핫플러그 브리지는 커널이
 * 핫플러그 브리지로 인식하지 못한다. 그러면 나중에 장치를 꽂고 버스를
 * 다시 스캔할 때 리소스를 배정하지 못해 실패한다.
 * [그대로 두면] 핫플러그로 꽂은 장치가 동작하지 않는다.
 * [우회] dev->is_hotplug_bridge 를 세운다. 그러면 커널이 이 브리지 창에
 * 미리 여유 공간을 잡아 두며, pci=hpmemsize=nnM / pci=hpiosize=nnM 부팅
 * 파라미터로 그 크기를 수동 지정할 수 있게 된다.
 *
 * 실행 컨텍스트: HEADER 단계. 브리지 창을 계산하기 전이어야 여유 공간이
 * 반영된다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_hotplug_bridge]
 */
/*
 * Allow manual resource allocation for PCI hotplug bridges via
 * pci=hpmemsize=nnM and pci=hpiosize=nnM parameters. For some PCI-PCI
 * hotplug bridges, like PLX 6254 (former HINT HB6), kernel fails to
 * allocate resources when hotplug device is inserted and PCI bus is
 * rescanned.
 */
static void quirk_hotplug_bridge(struct pci_dev *dev)
{
	dev->is_hotplug_bridge = 1;	/* [한국어] 리소스 할당 코드가 참조하는 플래그 - 핫플러그를 대비해 창에 여유를 둔다. */
}
/* [한국어] HINT 0x0020 브리지에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_HINT, 0x0020, quirk_hotplug_bridge);

/*
 * This is a quirk for the Ricoh MMC controller found as a part of some
 * multifunction chips.
 *
 * This is very similar and based on the ricoh_mmc driver written by
 * Philip Langdale. Thank you for these magic sequences.
 *
 * These chips implement the four main memory card controllers (SD, MMC,
 * MS, xD) and one or both of CardBus or FireWire.
 *
 * It happens that they implement SD and MMC support as separate
 * controllers (and PCI functions). The Linux SDHCI driver supports MMC
 * cards but the chip detects MMC cards in hardware and directs them to the
 * MMC controller - so the SDHCI driver never sees them.
 *
 * To get around this, we must disable the useless MMC controller.  At that
 * point, the SDHCI controller will start seeing them.  It seems to be the
 * case that the relevant PCI registers to deactivate the MMC controller
 * live on PCI function 0, which might be the CardBus controller or the
 * FireWire controller, depending on the particular chip in question
 *
 * This has to be done early, because as soon as we disable the MMC controller
 * other PCI functions shift up one level, e.g. function #2 becomes function
 * #1, and this will confuse the PCI core.
 */
/* [한국어] 아래 Ricoh MMC quirk 는 MMC 컨트롤러를 꺼서 SDHCI 드라이버가
 * 카드를 보게 만드는 것이므로, 그 처리를 담당하는 설정이 켜져 있을 때만
 * 의미가 있다. */
#ifdef CONFIG_MMC_RICOH_MMC
/*
 * [한국어]
 * ricoh_mmc_fixup_rl5c476 - CardBus 함수를 통해 쓸모없는 MMC 컨트롤러를 끈다
 *
 * @dev: Ricoh RL5C476 계열 다기능 칩의 CardBus 함수(함수 0)
 * @return: 없음
 *
 * [어떤 하드웨어] Ricoh 의 다기능 카드 리더 칩. 위 영어 주석의 설명이
 * 배경 전부다.
 * [무엇이 문제] 이 칩은 SD 와 MMC 를 서로 다른 PCI 함수(컨트롤러)로
 * 구현해 두고, 꽂힌 카드가 MMC 면 하드웨어가 알아서 MMC 컨트롤러 쪽으로
 * 보낸다. 그런데 리눅스의 SDHCI 드라이버는 MMC 카드도 다룰 수 있는 반면
 * 전용 MMC 컨트롤러용 드라이버는 없다. 결과적으로 MMC 카드를 SDHCI
 * 드라이버가 아예 보지 못한다.
 * [그대로 두면] MMC 카드를 인식하지 못한다.
 * [우회] 쓸모없는 MMC 컨트롤러를 꺼 버린다. 그러면 하드웨어가 MMC 카드를
 * SD 컨트롤러 쪽으로 보내 SDHCI 드라이버가 보게 된다. 그 설정 레지스터는
 * 함수 0 (칩에 따라 CardBus 또는 FireWire 컨트롤러)에 있다.
 *
 * [★ EARLY 단계여야 하는 이유] 영어 주석의 마지막 문단이 핵심이다. MMC
 * 컨트롤러를 끄면 그보다 뒤에 있던 PCI 함수들의 번호가 하나씩 당겨진다
 * (함수 2 가 함수 1 이 되는 식). 스캔 도중에 그런 일이 벌어지면 PCI 코어가
 * 혼란에 빠지므로, 스캔이 시작되기 전에 끝내야 한다.
 *
 * [매직 시퀀스의 출처] 영어 주석대로 Philip Langdale 이 쓴 ricoh_mmc
 * 드라이버에서 가져온 것이다. 각 레지스터 값의 의미는 원본 주석에도 없어
 * 이 트리의 정보만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: EARLY 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [ricoh_mmc_fixup_rl5c476]
 *     -> pci_write_config_byte()
 */
static void ricoh_mmc_fixup_rl5c476(struct pci_dev *dev)
{
	u8 write_enable;	/* [한국어] 쓰기 잠금을 푸는 레지스터의 원래 값. */
	u8 write_target;	/* [한국어] 어떤 대상에 쓸지 고르는 레지스터의 원래 값. */
	u8 disable;	/* [한국어] MMC 컨트롤러 비활성 비트가 들어 있는 레지스터 값. */

	/*
	 * Disable via CardBus interface
	 *
	 * This must be done via function #0
	 */
	if (PCI_FUNC(dev->devfn))	/* [한국어] 위 영어 주석대로 이 설정은 함수 0 을 통해서만 할 수 있다. 다른 함수에 quirk 가 걸려도 아무것도 하지 않는다. */
		return;

	pci_read_config_byte(dev, 0xB7, &disable);	/* [한국어] 0xB7 에 MMC 컨트롤러 비활성 비트가 있다. 벤더 전용 레지스터다. */
	if (disable & 0x02)	/* [한국어] 비트 1 이 이미 서 있으면 MMC 컨트롤러가 이미 꺼져 있다. */
		return;

	pci_read_config_byte(dev, 0x8E, &write_enable);	/* [한국어] 0x8E(쓰기 허용 레지스터)의 원래 값을 저장해 둔다 - 끝에 되돌려야 한다. */
	pci_write_config_byte(dev, 0x8E, 0xAA);	/* [한국어] 0xAA 는 벤더 전용 잠금 해제 값이다. 이 값을 써야 아래 쓰기가 먹힌다. */
	pci_read_config_byte(dev, 0x8D, &write_target);	/* [한국어] 0x8D(쓰기 대상 선택 레지스터)의 원래 값도 저장한다. */
	pci_write_config_byte(dev, 0x8D, 0xB7);	/* [한국어] 고칠 레지스터 번호(0xB7)를 대상으로 지정한다. */
	pci_write_config_byte(dev, 0xB7, disable | 0x02);	/* [한국어] 비트 1 을 세워 MMC 컨트롤러를 끈다. 이 순간 뒤쪽 PCI 함수 번호가 하나씩 당겨진다. */
	pci_write_config_byte(dev, 0x8E, write_enable);	/* [한국어] 쓰기 허용 레지스터를 원래대로 되돌려 다시 잠근다. */
	pci_write_config_byte(dev, 0x8D, write_target);	/* [한국어] 쓰기 대상 선택 레지스터도 원래대로 되돌린다. */

	pci_notice(dev, "proprietary Ricoh MMC controller disabled (via CardBus function)\n");	/* [한국어] 무엇을 껐는지 알린다. notice 수준인 것은 사용자가 알아야 할 구성 변경이기 때문이다. */
	pci_notice(dev, "MMC cards are now supported by standard SDHCI controller\n");	/* [한국어] 그 결과 표준 SDHCI 드라이버가 MMC 카드를 다루게 된다는 안내. */
}
/* [한국어] Ricoh RL5C476 에 EARLY 단계로 등록한다 - PCI 스캔 전이어야 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_RL5C476, ricoh_mmc_fixup_rl5c476);
/* [한국어] 재개 시에도 다시 꺼야 한다. 절전에서 깨어나면 MMC 컨트롤러가
 * 되살아나기 때문이다. */
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_RL5C476, ricoh_mmc_fixup_rl5c476);

/*
 * [한국어]
 * ricoh_mmc_fixup_r5c832 - FireWire 함수를 통해 MMC 컨트롤러를 끄고 SD 클럭을 낮춘다
 *
 * @dev: Ricoh R5C832 / R5CE822 / R5CE823 의 함수 0(FireWire 컨트롤러)
 * @return: 없음
 *
 * [어떤 하드웨어] Ricoh R5C832 계열 다기능 카드 리더 칩.
 * [무엇이 문제 (1)] 위 ricoh_mmc_fixup_rl5c476() 과 같은 문제다. 전용 MMC
 * 컨트롤러가 MMC 카드를 가로채 SDHCI 드라이버가 보지 못한다. 다만 설정
 * 레지스터가 CardBus 함수가 아니라 FireWire 함수에 있고 오프셋도 다르다.
 * [무엇이 문제 (2)] 아래 영어 주석대로 0xe822 와 0xe823 리더는 특정 종류의
 * SD/MMC 카드를 인식하지 못한다. SD 기준 클럭을 200MHz 에서 50MHz 로
 * 낮추면 해결된다.
 * [우회] 해당 모델이면 먼저 기준 클럭을 낮추고, 그다음 모든 모델에 대해
 * MMC 컨트롤러를 끈다.
 *
 * [매직 값의 의미] 클럭 변경에 쓰이는 레지스터와 값은 위 영어 주석이
 * 하나씩 밝혀 두었다 - 0x150 은 클럭 변경을 위한 SD2.0 모드 활성, 0xe1 은
 * 기준 클럭 레지스터, 0x32 는 50MHz 를 뜻하는 값, 0xf9 와 0xfc 는 각각
 * 0x150 과 0xe1 을 열어 주는 키 레지스터다. MMC 를 끄는 쪽(0xCA/0xCB)의
 * 값들은 설명이 없어 이 트리의 정보만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: EARLY 와 RESUME_EARLY.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [ricoh_mmc_fixup_r5c832] -> pci_write_config_byte()
 */
static void ricoh_mmc_fixup_r5c832(struct pci_dev *dev)
{
	u8 write_enable;	/* [한국어] 쓰기 잠금을 푸는 레지스터의 원래 값. */
	u8 disable;	/* [한국어] MMC 컨트롤러 비활성 비트가 들어 있는 레지스터 값. */

	/*
	 * Disable via FireWire interface
	 *
	 * This must be done via function #0
	 */
	if (PCI_FUNC(dev->devfn))	/* [한국어] 위 영어 주석대로 함수 0 을 통해서만 설정할 수 있다. */
		return;
	/*
	 * RICOH 0xe822 and 0xe823 SD/MMC card readers fail to recognize
	 * certain types of SD/MMC cards. Lowering the SD base clock
	 * frequency from 200Mhz to 50Mhz fixes this issue.
	 *
	 * 0x150 - SD2.0 mode enable for changing base clock
	 *	   frequency to 50Mhz
	 * 0xe1  - Base clock frequency
	 * 0x32  - 50Mhz new clock frequency
	 * 0xf9  - Key register for 0x150
	 * 0xfc  - key register for 0xe1
	 */
	if (dev->device == PCI_DEVICE_ID_RICOH_R5CE822 ||	/* [한국어] 클럭 문제가 있는 모델인지 확인한다 - 0xe822 이거나 */
	    dev->device == PCI_DEVICE_ID_RICOH_R5CE823) {	/* [한국어] 0xe823 인 경우다. */
		pci_write_config_byte(dev, 0xf9, 0xfc);	/* [한국어] 키 레지스터 0xf9 에 0xfc 를 써서 0x150 을 열어 준다. */
		pci_write_config_byte(dev, 0x150, 0x10);	/* [한국어] 0x150 에 0x10 을 써서 클럭 변경을 위한 SD2.0 모드를 켠다. */
		pci_write_config_byte(dev, 0xf9, 0x00);	/* [한국어] 키를 0 으로 되돌려 0x150 을 다시 잠근다. */
		pci_write_config_byte(dev, 0xfc, 0x01);	/* [한국어] 키 레지스터 0xfc 에 1 을 써서 0xe1 을 열어 준다. */
		pci_write_config_byte(dev, 0xe1, 0x32);	/* [한국어] 기준 클럭 레지스터 0xe1 에 0x32 를 써서 50MHz 로 낮춘다. */
		pci_write_config_byte(dev, 0xfc, 0x00);	/* [한국어] 키를 0 으로 되돌려 0xe1 을 다시 잠근다. */

		pci_notice(dev, "MMC controller base frequency changed to 50Mhz.\n");	/* [한국어] 클럭을 낮췄음을 알린다. */
	}

	pci_read_config_byte(dev, 0xCB, &disable);	/* [한국어] 0xCB 에 MMC 컨트롤러 비활성 비트가 있다 - RL5C476 의 0xB7 에 해당한다. */

	if (disable & 0x02)	/* [한국어] 비트 1 이 이미 서 있으면 이미 꺼져 있다. */
		return;

	pci_read_config_byte(dev, 0xCA, &write_enable);	/* [한국어] 0xCA(쓰기 허용 레지스터)의 원래 값을 저장한다. */
	pci_write_config_byte(dev, 0xCA, 0x57);	/* [한국어] 0x57 은 벤더 전용 잠금 해제 값이다. */
	pci_write_config_byte(dev, 0xCB, disable | 0x02);	/* [한국어] 비트 1 을 세워 MMC 컨트롤러를 끈다. */
	pci_write_config_byte(dev, 0xCA, write_enable);	/* [한국어] 쓰기 허용 레지스터를 원래대로 되돌린다. */

	pci_notice(dev, "proprietary Ricoh MMC controller disabled (via FireWire function)\n");	/* [한국어] 무엇을 껐는지 알린다. */
	pci_notice(dev, "MMC cards are now supported by standard SDHCI controller\n");	/* [한국어] 그 결과 표준 SDHCI 드라이버가 MMC 카드를 다루게 된다는 안내. */

}
/* [한국어] Ricoh R5C832 계열 3종에 EARLY 와 RESUME_EARLY 를 짝지어
 * 등록한다. EARLY 인 이유는 함수 번호가 밀리기 전에 끝내야 하기 때문이다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_R5C832, ricoh_mmc_fixup_r5c832);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_R5C832, ricoh_mmc_fixup_r5c832);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_R5CE822, ricoh_mmc_fixup_r5c832);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_R5CE822, ricoh_mmc_fixup_r5c832);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_RICOH_R5CE823. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_R5CE823, ricoh_mmc_fixup_r5c832);
DECLARE_PCI_FIXUP_RESUME_EARLY(PCI_VENDOR_ID_RICOH, PCI_DEVICE_ID_RICOH_R5CE823, ricoh_mmc_fixup_r5c832);
#endif /*CONFIG_MMC_RICOH_MMC*/

/* [한국어] 아래 quirk 는 Intel VT-d(DMA 재매핑) 테이블을 쓰는 빌드에서만
 * 의미가 있다. */
#ifdef CONFIG_DMAR_TABLE
#define VTUNCERRMSK_REG	0x1ac	/* [한국어] VT-d 정정 불가 오류 마스크 레지스터의 Configuration Space 오프셋(칩셋 전용). */
#define VTD_MSK_SPEC_ERRORS	(1 << 31)	/* [한국어] 그 레지스터의 비트 31 - VT-d 스펙이 정의한 오류들을 플랫폼 오류 처리로 넘기지 않도록 막는 마스크 비트. */
/*
 * [한국어]
 * vtd_mask_spec_errors - VT-d 스펙 오류가 NMI/SMI 로 올라가지 않게 막는다
 *
 * @dev: Intel 7500/5500 계열 칩셋 장치(0x342e, 0x3c28)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 7500, 5500 칩셋과 그 파생(X58 등).
 * [무엇이 문제] 아래 영어 주석대로, VT-d 결함(fault)이 생기면 플랫폼의
 * RAS 설정에 따라 NMI 나 SMI 가 발생한다. 그 SMI 때문에 시스템이 멈춘다.
 * [그대로 두면] VT-d 결함이 한 번이라도 나면 기계가 멈춘다.
 * [우회] VT-d 스펙이 정의한 오류들은 이미 VT-d OS 코드가 처리하므로 같은
 * 오류를 다른 경로로 또 보고할 이유가 없다. 마스크 레지스터의 비트 31 을
 * 세워 플랫폼 오류 처리 로직으로 넘어가지 않게 한다.
 *
 * 실행 컨텍스트: EARLY 단계 - VT-d 결함이 발생하기 전에 막아야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [vtd_mask_spec_errors] -> pci_write_config_dword()
 */
/*
 * This is a quirk for masking VT-d spec-defined errors to platform error
 * handling logic. Without this, platforms using Intel 7500, 5500 chipsets
 * (and the derivative chipsets like X58 etc) seem to generate NMI/SMI (based
 * on the RAS config settings of the platform) when a VT-d fault happens.
 * The resulting SMI caused the system to hang.
 *
 * VT-d spec-related errors are already handled by the VT-d OS code, so no
 * need to report the same error through other channels.
 */
static void vtd_mask_spec_errors(struct pci_dev *dev)
{
	u32 word;	/* [한국어] 마스크 레지스터의 현재 값. */

	pci_read_config_dword(dev, VTUNCERRMSK_REG, &word);	/* [한국어] 칩셋 전용 오프셋 0x1ac 를 읽는다. */
	pci_write_config_dword(dev, VTUNCERRMSK_REG, word | VTD_MSK_SPEC_ERRORS);	/* [한국어] 비트 31 을 OR 로 추가해 되쓴다 - 다른 마스크 설정은 보존한다. */
}
/* [한국어] 해당 칩셋 2종에 EARLY 단계로 등록한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x342e, vtd_mask_spec_errors);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x3c28, vtd_mask_spec_errors);
/* [한국어] CONFIG_DMAR_TABLE 블록의 끝. */
#endif

/*
 * [한국어]
 * fixup_ti816x_class - 클래스 코드가 비어 있는 TI 816x 를 비디오 클래스로 채운다
 *
 * @dev: TI device 0xb800 중 클래스가 '정의되지 않음' 인 것
 * @return: 없음
 *
 * [어떤 하드웨어] TI 816x SoC 가 PCIe 부트 모드로 동작할 때.
 * [무엇이 문제] 아래 영어 주석대로 PCIe 부트 모드에서는 클래스 코드가
 * 설정되지 않는다. 클래스 코드는 표준 헤더의 필수 필드다.
 * [그대로 두면] 커널이 이 장치가 무엇인지 몰라 적절한 드라이버를 붙이지
 * 못한다.
 * [우회] 클래스를 멀티미디어 비디오로 채워 준다.
 *
 * [등록 방식] 클래스가 PCI_CLASS_NOT_DEFINED 인 것만 매칭하도록 CLASS
 * 계열 매크로를 쓴다. 그래서 함수 안에서는 조건 없이 덮어써도 안전하다 -
 * 이미 클래스가 채워진 장치는 애초에 매칭되지 않는다.
 *
 * 실행 컨텍스트: EARLY 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [fixup_ti816x_class]
 */
static void fixup_ti816x_class(struct pci_dev *dev)
{
	u32 class = dev->class;	/* [한국어] 원래 클래스 값을 보관한다 - 로그에 무엇이 무엇으로 바뀌었는지 찍기 위해서다. */

	/* TI 816x devices do not have class code set when in PCIe boot mode */
	dev->class = PCI_CLASS_MULTIMEDIA_VIDEO << 8;	/* [한국어] 멀티미디어 비디오 클래스로 채운다. << 8 은 24비트 클래스 코드의 상위 16비트 자리에 놓기 위한 것이다. */
	pci_info(dev, "PCI class overridden (%#08x -> %#08x)\n",	/* [한국어] 무엇이 무엇으로 바뀌었는지 남긴다. */
		 class, dev->class);
}
/* [한국어] TI 0xb800 중 클래스가 정의되지 않은 것만 EARLY 단계에서 잡는다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_TI, 0xb800,
			      PCI_CLASS_NOT_DEFINED, 8, fixup_ti816x_class);

/*
 * [한국어]
 * fixup_mpss_256 - 광고한 최대 페이로드 크기를 믿을 수 없는 장치를 256B 로 제한
 *
 * @dev: Solarflare SFC4000 계열 또는 ASMedia 0x0612
 * @return: 없음
 *
 * [MPS 란] Maximum Payload Size - PCIe TLP 하나에 실을 수 있는 데이터의
 * 최대 바이트 수다. 각 장치가 Device Capabilities 로 자신이 감당할 수 있는
 * 값을 광고하고, 커널은 한 계층(fabric)의 모든 장치가 감당하는 최소값으로
 * 통일해 설정한다. 클수록 헤더 오버헤드가 줄어 효율이 좋다.
 * [무엇이 문제] 아래 영어 주석대로 일부 PCIe 장치는 자신이 광고한 최대
 * 페이로드 크기로는 안정적으로 동작하지 않는다. 즉 capability 가 거짓말이다.
 * [그대로 두면] 큰 TLP 를 주고받을 때 오동작한다.
 * [우회] dev->pcie_mpss 를 1(=256바이트)로 덮어써 커널이 광고 대신 이 값을
 * 쓰게 한다. pcie_mpss 는 128 << n 형식의 인코딩이라 1 이 256바이트다.
 *
 * 실행 컨텍스트: EARLY 단계 - MPS 협상이 이뤄지기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [fixup_mpss_256]
 */
/*
 * Some PCIe devices do not work reliably with the claimed maximum
 * payload size supported.
 */
static void fixup_mpss_256(struct pci_dev *dev)
{
	/* [한국어] pcie_mpss 는 '128 << n' 형식의 인코딩이라 1 이 256바이트를 뜻한다.
	 * 옆의 영어 주석이 그 값을 확인해 준다. 커널의 MPS 협상 코드가 장치가
	 * 광고한 값 대신 이 필드를 쓴다. */
	dev->pcie_mpss = 1; /* 256 bytes */
}
/* [한국어] 문제 있는 장치 4종에 EARLY 단계로 등록한다 - MPS 협상 전이어야 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SOLARFLARE,
			PCI_DEVICE_ID_SOLARFLARE_SFC4000A_0, fixup_mpss_256);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SOLARFLARE,
			PCI_DEVICE_ID_SOLARFLARE_SFC4000A_1, fixup_mpss_256);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SOLARFLARE,
			PCI_DEVICE_ID_SOLARFLARE_SFC4000B, fixup_mpss_256);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_ASMEDIA, 0x0612, fixup_mpss_256);

/*
 * [한국어]
 * quirk_intel_mc_errata - Intel 5000/5100 메모리 컨트롤러의 읽기 완료 병합을 끈다
 *
 * @dev: Intel 5000/5100 메모리 컨트롤러 계열(아래 등록표)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 5000 과 5100 메모리 컨트롤러.
 * [무엇이 문제] 아래 영어 주석대로, 읽기 완료 병합(read completion
 * coalescing)이 켜져 있고 MPS 가 256B 이면 에라타가 발동한다. 병합은 일부
 * BIOS 에서 기본으로 켜져 있다.
 * [왜 그냥 끄는가] 어느 fabric 의 MPS 가 얼마가 될지는 모든 장치를 찾아
 * 버스를 다 훑기 전에는 알 수 없다. 그래서 '256B 가 될지도 모르니' 미리
 * 병합을 꺼 둔다. 그리고 나중에 다시 켤 수도 없다 - MPS 가 256B 인 장치를
 * 핫플러그로 꽂을 수 있기 때문이다.
 * [우회] 칩셋 전용 레지스터 0x48 의 비트 10 을 끈다.
 *
 * [매직 넘버인 이유] 영어 주석이 솔직히 밝힌다. 인텔 에라타는 바꿀 비트를
 * 지정하기만 하고 그것이 무엇인지는 말하지 않는다. 레지스터와 값이 설명될
 * 때까지 매직으로 남겨 둔다는 것이 원저자의 입장이다.
 *
 * [MPS 튜닝이 꺼져 있으면] pcie_bus_config 가 TUNE_OFF 이거나 DEFAULT 면
 * 커널이 MPS 를 건드리지 않으므로 이 회피도 필요 없다.
 *
 * 실행 컨텍스트: 아래 등록 단계 참조.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_intel_mc_errata] -> pci_write_config_word()
 */
/*
 * Intel 5000 and 5100 Memory controllers have an erratum with read completion
 * coalescing (which is enabled by default on some BIOSes) and MPS of 256B.
 * Since there is no way of knowing what the PCIe MPS on each fabric will be
 * until all of the devices are discovered and buses walked, read completion
 * coalescing must be disabled.  Unfortunately, it cannot be re-enabled because
 * it is possible to hotplug a device with MPS of 256B.
 */
static void quirk_intel_mc_errata(struct pci_dev *dev)
{
	int err;	/* [한국어] config 접근의 반환값. */
	u16 rcc;	/* [한국어] 읽기 완료 병합 레지스터의 값. */

	if (pcie_bus_config == PCIE_BUS_TUNE_OFF ||	/* [한국어] 커널이 MPS 를 조정하지 않는 설정이면 */
	    pcie_bus_config == PCIE_BUS_DEFAULT)	/* [한국어] 이 회피가 필요 없다. */
		return;

	/*
	 * Intel erratum specifies bits to change but does not say what
	 * they are.  Keeping them magical until such time as the registers
	 * and values can be explained.
	 */
	err = pci_read_config_word(dev, 0x48, &rcc);	/* [한국어] 칩셋 전용 레지스터 0x48 을 읽는다. 위 영어 주석대로 이 레지스터의 의미는 공개되어 있지 않다. */
	if (err) {	/* [한국어] 읽기에 실패하면 판단할 수 없다. */
		pci_err(dev, "Error attempting to read the read completion coalescing register\n");	/* [한국어] 실패를 오류 수준으로 남긴다. */
		return;
	}

	if (!(rcc & (1 << 10)))	/* [한국어] 비트 10 이 꺼져 있으면 이미 병합이 꺼진 상태다. */
		return;

	rcc &= ~(1 << 10);	/* [한국어] 비트 10 만 끈다. */

	err = pci_write_config_word(dev, 0x48, rcc);	/* [한국어] 고친 값을 되쓴다. */
	if (err) {	/* [한국어] 쓰기에 실패하면 병합이 꺼졌다고 알릴 수 없다. */
		pci_err(dev, "Error attempting to write the read completion coalescing register\n");	/* [한국어] 실패를 오류 수준으로 남긴다. */
		return;
	}

	pr_info_once("Read completion coalescing disabled due to hardware erratum relating to 256B MPS\n");	/* [한국어] 성공했음을 한 번만 알린다. _once 를 쓰는 것은 이 quirk 가 여러 장치에 걸려 같은 메시지가 반복되기 때문이다. */
}
/* Intel 5000 series memory controllers and ports 2-7 */
/* [한국어] 옆의 영어 주석대로 Intel 5000 계열 메모리 컨트롤러와 포트 2~7 이다.
 * 각 포트마다 별도 device ID 라 하나씩 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25c0, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25d0, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25d4, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25d8, quirk_intel_mc_errata);
/* [한국어] 이어지는 등록 줄: 0x25e2, 0x25e3, 0x25e4, 0x25e5. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25e2, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25e3, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25e4, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25e5, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25e6, quirk_intel_mc_errata);
/* [한국어] 이어지는 등록 줄: 0x25e7, 0x25f7, 0x25f8, 0x25f9. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25e7, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25f7, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25f8, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25f9, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x25fa, quirk_intel_mc_errata);
/* Intel 5100 series memory controllers and ports 2-7 */
/* [한국어] 옆의 영어 주석대로 Intel 5100 계열 메모리 컨트롤러와 포트 2~7 이다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65c0, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65e2, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65e3, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65e4, quirk_intel_mc_errata);
/* [한국어] 이어지는 등록 줄: 0x65e5, 0x65e6, 0x65e7, 0x65f7. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65e5, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65e6, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65e7, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65f7, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65f8, quirk_intel_mc_errata);
/* [한국어] 이어지는 등록 줄: 0x65f9, 0x65fa. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65f9, quirk_intel_mc_errata);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x65fa, quirk_intel_mc_errata);

/*
 * [한국어]
 * quirk_intel_ntb - Ivytown NTB 가 잘못 보고한 BAR 크기를 장치에 물어 바로잡는다
 *
 * @dev: Intel Ivytown NTB(Non-Transparent Bridge) 장치(0x0e08, 0x0e0d)
 * @return: 없음
 *
 * [NTB 란] Non-Transparent Bridge - 두 시스템을 PCIe 로 잇되 서로의 열거
 * 영역을 침범하지 않게 막고, 정해진 창(BAR)을 통해서만 상대 메모리에
 * 접근하게 하는 장치다. 그 창의 크기가 곧 BAR 크기다.
 * [무엇이 문제] 아래 영어 주석대로 에라타 때문에 BAR 크기가 하드웨어에서
 * 잘못 보고된다. 표준 BAR 크기 측정(모두 1 을 쓰고 되읽기) 결과를 믿을 수
 * 없다는 뜻이다.
 * [그대로 두면] 창의 크기를 잘못 알아 리소스 배정이 어긋난다.
 * [우회] 장치에 '실제로 설정되어야 할 크기'를 물어본다. 벤더 전용 레지스터
 * 0x00D0 과 0x00D1 이 각각 BAR2 와 BAR4 의 크기를 2의 지수로 담고 있어,
 * 1 << val 로 바이트 크기를 만들어 리소스 끝을 다시 정한다.
 *
 * [BAR 2 와 4 인 이유] NTB 의 메모리 창이 64비트 BAR 라 두 슬롯을 차지한다.
 * 그래서 BAR0/1 이 한 쌍, BAR2/3 이 한 쌍, BAR4/5 가 한 쌍이 되어 창의
 * 시작 슬롯이 2 와 4 다.
 *
 * 실행 컨텍스트: HEADER 단계(리소스 할당 전).
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_intel_ntb] -> resource_set_size()
 */
/*
 * Ivytown NTB BAR sizes are misreported by the hardware due to an erratum.
 * To work around this, query the size it should be configured to by the
 * device and modify the resource end to correspond to this new size.
 */
static void quirk_intel_ntb(struct pci_dev *dev)
{
	int rc;	/* [한국어] config 접근의 반환값. */
	u8 val;	/* [한국어] 장치가 알려 주는 BAR 크기의 2의 지수. */

	rc = pci_read_config_byte(dev, 0x00D0, &val);	/* [한국어] 벤더 전용 레지스터 0x00D0 에서 BAR2 의 크기 지수를 읽는다. */
	if (rc)	/* [한국어] 읽기에 실패하면 고칠 근거가 없다. */
		return;

	resource_set_size(&dev->resource[2], (resource_size_t)1 << val);	/* [한국어] 1 << val 로 바이트 크기를 만들어 BAR2 리소스의 크기를 다시 정한다. resource_size_t 로 캐스팅하는 것은 val 이 32 이상일 때 int 로는 넘치기 때문이다. */

	rc = pci_read_config_byte(dev, 0x00D1, &val);	/* [한국어] 0x00D1 에서 BAR4 의 크기 지수를 읽는다. */
	if (rc)	/* [한국어] 읽기 실패 시 BAR2 만 고친 채로 끝난다. */
		return;

	resource_set_size(&dev->resource[4], (resource_size_t)1 << val);	/* [한국어] BAR4 리소스의 크기도 같은 방식으로 바로잡는다. */
}
/* [한국어] Ivytown NTB 2종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0e08, quirk_intel_ntb);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x0e0d, quirk_intel_ntb);

/*
 * [한국어]
 * disable_igfx_irq - BIOS 가 켜 둔 채 넘긴 인텔 내장 GPU 인터럽트를 끈다
 *
 * @dev: Intel 내장 그래픽(아래 등록표의 device ID 7종)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel 내장 GPU(Ironlake~Ivy Bridge 세대).
 * [무엇이 문제] 아래 영어 주석대로, 일부 BIOS 는 GPU 인터럽트를 켜 둔 채
 * 커널에 넘긴다. 처리할 드라이버가 없는데다(예: i915 를 아예 로드하지 않는
 * 경우) 인터럽트 목적지 설정도 제대로 되어 있지 않아 인터럽트가 엉뚱한
 * 곳으로 간다.
 * [그대로 두면] 이 헛인터럽트는 지워지지 않고(sticky) 계속 올라오며,
 * 10만 번을 넘기면 커널이 그 공유 인터럽트 선 전체를 꺼 버린다. 그 선을
 * 함께 쓰는 다른 장치까지 인터럽트를 잃는다. 모니터를 뽑을 때 자주 보이는
 * 크래시가 이 때문이다.
 * [우회] BAR0 을 매핑해 GPU 의 인터럽트 활성 레지스터(DEIER)를 직접 0 으로
 * 만든다. e100 quirk 와 같은 방식의 '드라이버 없이 하드웨어를 잠재우는' 처리다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [disable_igfx_irq] -> pci_iomap() -> writel()
 */
/*
 * Some BIOS implementations leave the Intel GPU interrupts enabled, even
 * though no one is handling them (e.g., if the i915 driver is never
 * loaded).  Additionally the interrupt destination is not set up properly
 * and the interrupt ends up -somewhere-.
 *
 * These spurious interrupts are "sticky" and the kernel disables the
 * (shared) interrupt line after 100,000+ generated interrupts.
 *
 * Fix it by disabling the still enabled interrupts.  This resolves crashes
 * often seen on monitor unplug.
 */
#define I915_DEIER_REG 0x4400c	/* [한국어] DEIER(Display Engine Interrupt Enable Register)의 MMIO 오프셋. i915 하드웨어 문서의 값이다. */
static void disable_igfx_irq(struct pci_dev *dev)
{
	void __iomem *regs = pci_iomap(dev, 0, 0);	/* [한국어] BAR0 전체를 매핑한다. 세 번째 인자 0 은 '가능한 만큼 전부' 를 뜻한다. */
	if (regs == NULL) {	/* [한국어] 매핑에 실패하면 레지스터를 건드릴 수 없다. */
		pci_warn(dev, "igfx quirk: Can't iomap PCI device\n");	/* [한국어] 왜 못 했는지 남긴다. */
		return;
	}

	/* Check if any interrupt line is still enabled */
	if (readl(regs + I915_DEIER_REG) != 0) {	/* [한국어] 위 영어 주석대로 인터럽트 활성 레지스터가 0 이 아니면 아직 켜져 있는 인터럽트가 있다는 뜻이다. */
		pci_warn(dev, "BIOS left Intel GPU interrupts enabled; disabling\n");	/* [한국어] BIOS 가 켜 둔 채 넘겼음을 경고로 남긴다. */

		writel(0, regs + I915_DEIER_REG);	/* [한국어] 0 을 써서 모든 디스플레이 엔진 인터럽트를 끈다. i915 가 로드되면 스스로 다시 켠다. */
	}

	pci_iounmap(dev, regs);	/* [한국어] 임시 매핑을 해제한다. */
}
/* [한국어] 문제 있는 내장 GPU 7종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0042, disable_igfx_irq);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0046, disable_igfx_irq);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x004a, disable_igfx_irq);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0102, disable_igfx_irq);
/* [한국어] 이어지는 등록 줄: 0x0106, 0x010a, 0x0152. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0106, disable_igfx_irq);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x010a, disable_igfx_irq);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0152, disable_igfx_irq);

/*
 * [한국어]
 * quirk_remove_d3hot_delay - 10ms D3hot 대기가 필요 없는 인텔 장치의 지연을 없앤다
 *
 * @dev: 아래 등록표의 Intel C600 / Lynxpoint-H PCH / Cherrytrail 장치들
 * @return: 없음
 *
 * [배경] PCI 전원관리 스펙은 D0 에서 D3hot 으로 내린 뒤 다음 동작까지
 * 최소 10ms 를 기다리라고 정한다. 커널은 안전을 위해 모든 장치에 이 지연을
 * 적용한다.
 * [무엇이 문제] 스펙 위반이 아니라 그 반대다. 아래 영어 주석대로 인텔
 * 칩에 붙어 있는 이 장치들은 그 10ms 를 건너뛰어도 된다. 지키면 안전하지만
 * 절전 진입이 그만큼 느려진다.
 * [우회] dev->d3hot_delay 를 0 으로 만들어 지연을 없앤다.
 *
 * [앞의 quirk_d3hot_delay() 와 정반대] 그쪽은 지연이 모자란 장치의 값을
 * 키웠고, 이쪽은 지연이 필요 없는 장치의 값을 없앤다. 두 방향 모두
 * 하드웨어의 실제 성질에 맞추는 것이 목적이다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_remove_d3hot_delay]
 */
/*
 * PCI devices which are on Intel chips can skip the 10ms delay
 * before entering D3 mode.
 */
static void quirk_remove_d3hot_delay(struct pci_dev *dev)
{
	dev->d3hot_delay = 0;	/* [한국어] D3hot 관련 대기 시간을 0 으로 만든다. */
}
/* C600 Series devices do not need 10ms d3hot_delay */
/* [한국어] 옆의 영어 주석대로 C600 계열 3종. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0412, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0c00, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x0c0c, quirk_remove_d3hot_delay);
/* Lynxpoint-H PCH devices do not need 10ms d3hot_delay */
/* [한국어] 옆의 영어 주석대로 Lynxpoint-H PCH 계열 12종. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c02, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c18, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c1c, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c20, quirk_remove_d3hot_delay);
/* [한국어] 이어지는 등록 줄: 0x8c22, 0x8c26, 0x8c2d, 0x8c31. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c22, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c26, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c2d, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c31, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c3a, quirk_remove_d3hot_delay);
/* [한국어] 이어지는 등록 줄: 0x8c3d, 0x8c4e. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c3d, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x8c4e, quirk_remove_d3hot_delay);
/* Intel Cherrytrail devices do not need 10ms d3hot_delay */
/* [한국어] 옆의 영어 주석대로 Cherrytrail 계열 9종. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x2280, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x2298, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x229c, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x22b0, quirk_remove_d3hot_delay);
/* [한국어] 이어지는 등록 줄: 0x22b5, 0x22b7, 0x22b8, 0x22d8. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x22b5, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x22b7, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x22b8, quirk_remove_d3hot_delay);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x22d8, quirk_remove_d3hot_delay);
/* [한국어] 이어지는 등록 줄: 0x22dc. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x22dc, quirk_remove_d3hot_delay);

/*
 * [한국어]
 * quirk_broken_intx_masking - INTx 마스킹이 실제로는 동작하지 않는 장치를 표시
 *
 * @dev: 아래 등록표의 장치들
 * @return: 없음
 *
 * [INTx 마스킹이란] Command 레지스터의 INTx Disable 비트(DisINTx)를 세우면
 * 장치가 레거시 인터럽트를 쏘지 않아야 하고, Status 레지스터의 Interrupt
 * Status 비트로 '지금 인터럽트를 걸고 싶은 상태인지'를 알 수 있어야 한다.
 * 이 두 가지가 되어야 인터럽트 선을 공유하는 장치를 VFIO 로 게스트에
 * 넘길 수 있다.
 * [무엇이 문제] 아래 영어 주석대로, 일부 장치는 DisINTx 쓰기 자체는
 * 동작해서 pci_intx_mask_supported() 의 검사를 통과하지만 실제로는 이
 * 기능을 제대로 지원하지 않는다. 아래 등록표의 개별 주석이 각각의 사정을
 * 밝힌다 - 예를 들어 Intel i40e 는 DisINTx 는 설정되지만 인터럽트 상태
 * 비트가 동작하지 않는다.
 * [그대로 두면] 장치 할당(VFIO 패스스루) 시 인터럽트가 제어되지 않는다.
 * [우회] dev->broken_intx_masking 을 세워 커널이 이 기능을 쓰지 않게 한다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_broken_intx_masking]
 */
/*
 * Some devices may pass our check in pci_intx_mask_supported() if
 * PCI_COMMAND_INTX_DISABLE works though they actually do not properly
 * support this feature.
 */
static void quirk_broken_intx_masking(struct pci_dev *dev)
{
	dev->broken_intx_masking = 1;	/* [한국어] INTx 마스킹을 신뢰할 수 없다는 표시. VFIO 등이 이 값을 본다. */
}
/* [한국어] Chelsio 0x0030. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_CHELSIO, 0x0030,
			quirk_broken_intx_masking);
/* [한국어] 옆의 영어 주석대로 Ralink RT2800 802.11n 무선 카드. */
DECLARE_PCI_FIXUP_FINAL(0x1814, 0x0601, /* Ralink RT2800 802.11n PCI */
			quirk_broken_intx_masking);
/* [한국어] 옆의 영어 주석대로 Ceton InfiniTV4 캡처 카드. */
DECLARE_PCI_FIXUP_FINAL(0x1b7c, 0x0004, /* Ceton InfiniTV4 */
			quirk_broken_intx_masking);
/* [한국어] Creative 20K2 사운드 칩. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_CREATIVE, PCI_DEVICE_ID_CREATIVE_20K2,
			quirk_broken_intx_masking);

/*
 * Realtek RTL8169 PCI Gigabit Ethernet Controller (rev 10)
 * Subsystem: Realtek RTL8169/8110 Family PCI Gigabit Ethernet NIC
 *
 * RTL8110SC - Fails under PCI device assignment using DisINTx masking.
 */
/* [한국어] 위 영어 주석대로 Realtek RTL8110SC 는 DisINTx 마스킹을 쓰는
 * 장치 할당에서 실패한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_REALTEK, 0x8169,
			quirk_broken_intx_masking);

/*
 * Intel i40e (XL710/X710) 10/20/40GbE NICs all have broken INTx masking,
 * DisINTx can be set but the interrupt status bit is non-functional.
 */
/* [한국어] 위 영어 주석대로 Intel i40e(XL710/X710) 계열은 DisINTx 를
 * 세울 수는 있지만 인터럽트 상태 비트가 동작하지 않는다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1572, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1574, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1580, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1581, quirk_broken_intx_masking);
/* [한국어] 이어지는 등록 줄: 0x1583, 0x1584, 0x1585, 0x1586. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1583, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1584, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1585, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1586, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1587, quirk_broken_intx_masking);
/* [한국어] 이어지는 등록 줄: 0x1588, 0x1589, 0x158a, 0x158b. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1588, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1589, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x158a, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x158b, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x37d0, quirk_broken_intx_masking);
/* [한국어] 이어지는 등록 줄: 0x37d1, 0x37d2. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x37d1, quirk_broken_intx_masking);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x37d2, quirk_broken_intx_masking);

/* [한국어] INTx 마스킹이 깨져 있다고 알려진 Mellanox 장치의 device ID 표.
 * 설정자: 컴파일 시 고정된 배열.
 * 읽는 자: 아래 mellanox_check_broken_intx_masking() 이 선형 탐색으로 훑는다.
 * 값 범위: Hermon 세대와 ConnectX-2/3 계열의 device ID 들.
 * 왜 표인가: Mellanox 는 벤더 단위로 quirk 를 걸고 함수 안에서 세대를
 *   가려내야 한다. ConnectX-4 이후는 펌웨어 버전까지 봐야 하므로 단순
 *   device ID 매칭으로는 부족하다.
 * 동기화: 읽기 전용이라 경쟁이 없다. */
static u16 mellanox_broken_intx_devs[] = {
	PCI_DEVICE_ID_MELLANOX_HERMON_SDR,	/* [한국어] Hermon SDR. */
	PCI_DEVICE_ID_MELLANOX_HERMON_DDR,	/* [한국어] Hermon DDR. */
	PCI_DEVICE_ID_MELLANOX_HERMON_QDR,	/* [한국어] Hermon QDR. */
	PCI_DEVICE_ID_MELLANOX_HERMON_DDR_GEN2,	/* [한국어] Hermon DDR 2세대. */
	PCI_DEVICE_ID_MELLANOX_HERMON_QDR_GEN2,	/* [한국어] Hermon QDR 2세대. */
	PCI_DEVICE_ID_MELLANOX_HERMON_EN,	/* [한국어] Hermon 이더넷 판. */
	PCI_DEVICE_ID_MELLANOX_HERMON_EN_GEN2,	/* [한국어] Hermon 이더넷 2세대. */
	PCI_DEVICE_ID_MELLANOX_CONNECTX_EN,	/* [한국어] ConnectX 이더넷. */
	PCI_DEVICE_ID_MELLANOX_CONNECTX_EN_T_GEN2,	/* [한국어] ConnectX 이더넷 T 2세대. */
	PCI_DEVICE_ID_MELLANOX_CONNECTX_EN_GEN2,	/* [한국어] ConnectX 이더넷 2세대. */
	PCI_DEVICE_ID_MELLANOX_CONNECTX_EN_5_GEN2,	/* [한국어] ConnectX 이더넷 5포트 2세대. */
	PCI_DEVICE_ID_MELLANOX_CONNECTX2,	/* [한국어] ConnectX-2. */
	PCI_DEVICE_ID_MELLANOX_CONNECTX3,	/* [한국어] ConnectX-3. */
	PCI_DEVICE_ID_MELLANOX_CONNECTX3_PRO,	/* [한국어] ConnectX-3 Pro - 여기까지가 무조건 깨져 있는 세대다. */
};

/* [한국어] 아래 두 상수는 ConnectX-4 펌웨어 버전 판정 기준이다. 위
 * 영어 주석이 그 의미를 밝힌다 - minor 가 99 를 넘으면 옛 버전 표기법이라
 * INTx 마스킹을 지원하지 않고, 14 미만이면 새 표기법이지만 아직 지원 전이다. */
#define CONNECTX_4_CURR_MAX_MINOR 99	/* [한국어] 옛 버전 표기법과 새 표기법을 가르는 minor 상한. */
#define CONNECTX_4_INTX_SUPPORT_MINOR 14	/* [한국어] 새 표기법에서 INTx 마스킹을 지원하기 시작하는 minor. */

/*
 * [한국어]
 * mellanox_check_broken_intx_masking - Mellanox 카드의 INTx 마스킹 지원 여부를 판정
 *
 * @pdev: Mellanox 벤더의 모든 장치
 * @return: 없음
 *
 * [어떤 하드웨어] Mellanox 인피니밴드/이더넷 카드 전체.
 * [무엇이 문제] 세대마다 사정이 다르다.
 *   - Hermon ~ ConnectX-3 Pro: INTx 마스킹이 아예 깨져 있다(위 표).
 *   - Connect-IB: INTx 지원 자체가 없어 볼 필요가 없다.
 *   - ConnectX-4 / ConnectX-4 LX: 펌웨어 버전에 따라 다르다. 아래 영어
 *     주석대로 펌웨어를 확인해 지원하면 깨졌다고 표시하지 않는다.
 * [그대로 두면] 지원하지 않는 카드에서 장치 할당 시 인터럽트가 제어되지
 * 않는다. 반대로 지원하는 펌웨어인데도 깨졌다고 표시하면 쓸 수 있는 기능을
 * 잃는다.
 * [우회] 표 검사 -> Connect-IB 제외 -> ConnectX-4 펌웨어 버전 확인의
 * 3단계로 판정한다. 펌웨어 버전은 BAR0 의 초기화 세그먼트를 매핑해 읽는다.
 *
 * [★ 이 quirk 가 특이한 점] 펌웨어 버전을 읽으려면 MMIO 접근이 필요하고,
 * 그러려면 장치를 켜야 한다. 그래서 이 quirk 는 pci_enable_device_mem() 으로
 * 장치를 켰다가 끝에 pci_disable_device() 로 되돌린다. quirk 가 장치를
 * 직접 켜는 드문 경우다.
 *
 * 실행 컨텍스트: FINAL 단계, 프로세스 문맥. 장치를 켰다 끄고 MMIO 를
 * 매핑하므로 잠들 수 있는 문맥이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [mellanox_check_broken_intx_masking]
 *     -> pci_enable_device_mem() -> ioremap() -> pci_disable_device()
 */
/*
 * Check ConnectX-4/LX FW version to see if it supports legacy interrupts.
 * If so, don't mark it as broken.
 * FW minor > 99 means older FW version format and no INTx masking support.
 * FW minor < 14 means new FW version format and no INTx masking support.
 */
static void mellanox_check_broken_intx_masking(struct pci_dev *pdev)
{
	__be32 __iomem *fw_ver;	/* [한국어] 펌웨어 버전이 놓인 MMIO 주소. __be32 인 것은 이 레지스터가 빅엔디언이기 때문이다. */
	u16 fw_major;	/* [한국어] 펌웨어 major 버전. */
	u16 fw_minor;	/* [한국어] 펌웨어 minor 버전 - 판정의 핵심 값이다. */
	u16 fw_subminor;	/* [한국어] 펌웨어 subminor 버전 - 로그에만 쓴다. */
	u32 fw_maj_min;	/* [한국어] major 와 minor 가 함께 들어 있는 첫 dword. */
	u32 fw_sub_min;	/* [한국어] subminor 가 들어 있는 두 번째 dword. */
	int i;	/* [한국어] 표를 훑는 반복자. */

	for (i = 0; i < ARRAY_SIZE(mellanox_broken_intx_devs); i++) {	/* [한국어] 깨진 것으로 알려진 device ID 표를 선형 탐색한다. */
		if (pdev->device == mellanox_broken_intx_devs[i]) {	/* [한국어] 이 장치가 표에 있으면 */
			pdev->broken_intx_masking = 1;	/* [한국어] 더 볼 것 없이 깨졌다고 표시하고 */
			return;	/* [한국어] 돌아간다. */
		}
	}

	/*
	 * Getting here means Connect-IB cards and up. Connect-IB has no INTx
	 * support so shouldn't be checked further
	 */
	if (pdev->device == PCI_DEVICE_ID_MELLANOX_CONNECTIB)	/* [한국어] 위 영어 주석대로 Connect-IB 는 INTx 지원 자체가 없어 더 볼 필요가 없다. */
		return;

	if (pdev->device != PCI_DEVICE_ID_MELLANOX_CONNECTX4 &&	/* [한국어] ConnectX-4 도 아니고 */
	    pdev->device != PCI_DEVICE_ID_MELLANOX_CONNECTX4_LX)	/* [한국어] ConnectX-4 LX 도 아니면 펌웨어를 볼 대상이 아니다. */
		return;

	/* For ConnectX-4 and ConnectX-4LX, need to check FW support */
	if (pci_enable_device_mem(pdev)) {	/* [한국어] 펌웨어 버전을 MMIO 로 읽으려면 메모리 디코딩을 켜야 한다. quirk 가 장치를 직접 켜는 드문 경우다. */
		pci_warn(pdev, "Can't enable device memory\n");	/* [한국어] 켜지 못하면 판정할 수 없다. */
		return;
	}

	fw_ver = ioremap(pci_resource_start(pdev, 0), 4);	/* [한국어] BAR0 의 앞 4바이트만 매핑한다 - 초기화 세그먼트의 시작에 버전이 있다. */
	if (!fw_ver) {	/* [한국어] 매핑 실패 시 장치를 다시 꺼야 하므로 out 으로 간다. */
		pci_warn(pdev, "Can't map ConnectX-4 initialization segment\n");	/* [한국어] 왜 못 했는지 남긴다. */
		goto out;
	}

	/* Reading from resource space should be 32b aligned */
	fw_maj_min = ioread32be(fw_ver);	/* [한국어] 위 영어 주석대로 32비트 정렬로 읽어야 한다. ioread32be 는 빅엔디언 레지스터를 호스트 바이트 순서로 바꿔 읽는다. */
	fw_sub_min = ioread32be(fw_ver + 1);	/* [한국어] 다음 dword 를 읽는다. fw_ver 가 __be32 포인터라 +1 은 4바이트 이동이다. */
	fw_major = fw_maj_min & 0xffff;	/* [한국어] 첫 dword 의 하위 16비트가 major. */
	fw_minor = fw_maj_min >> 16;	/* [한국어] 상위 16비트가 minor. */
	fw_subminor = fw_sub_min & 0xffff;	/* [한국어] 두 번째 dword 의 하위 16비트가 subminor. */
	if (fw_minor > CONNECTX_4_CURR_MAX_MINOR ||	/* [한국어] minor 가 99 를 넘으면 옛 표기법이라 지원하지 않고, */
	    fw_minor < CONNECTX_4_INTX_SUPPORT_MINOR) {	/* [한국어] 14 미만이면 새 표기법이지만 아직 지원 전이다. */
		pci_warn(pdev, "ConnectX-4: FW %u.%u.%u doesn't support INTx masking, disabling. Please upgrade FW to %d.14.1100 and up for INTx support\n",	/* [한국어] 어떤 펌웨어 버전이라 껐는지, 어느 버전으로 올려야 하는지 안내한다. */
			 fw_major, fw_minor, fw_subminor, pdev->device ==	/* [한국어] 모델에 따라 권장 major 버전이 달라 */
			 PCI_DEVICE_ID_MELLANOX_CONNECTX4 ? 12 : 14);	/* [한국어] ConnectX-4 는 12, LX 는 14 로 안내한다. */
		pdev->broken_intx_masking = 1;	/* [한국어] 지원하지 않는 펌웨어이므로 깨졌다고 표시한다. */
	}

	iounmap(fw_ver);	/* [한국어] MMIO 매핑을 해제한다. */

out:	/* [한국어] 공통 정리 지점 - 매핑 실패 경로도 여기로 온다. */
	pci_disable_device(pdev);	/* [한국어] 위에서 켠 장치를 다시 끈다. quirk 는 부작용을 남기지 않아야 한다. */
}
/* [한국어] Mellanox 벤더의 모든 장치를 FINAL 단계에서 검사한다. 세대별
 * 판정은 함수 안에서 한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_MELLANOX, PCI_ANY_ID,
			mellanox_check_broken_intx_masking);

/*
 * [한국어]
 * quirk_no_bus_reset - Secondary Bus Reset 을 걸면 안 되는 장치를 표시
 *
 * @dev: 아래 여러 등록표의 장치들
 * @return: 없음
 *
 * [Secondary Bus Reset(SBR)이란] 브리지의 Bridge Control 레지스터에
 * Secondary Bus Reset 비트를 세워 하위 버스 전체를 리셋하는 방법이다.
 * FLR 을 지원하지 않는 장치를 초기화하는 마지막 수단이라 VFIO 장치 할당
 * 경로에서 자주 쓰인다.
 * [무엇이 문제] 아래 여러 등록표 각각의 영어 주석이 사정을 밝힌다. 링크가
 * 재훈련되지 않거나(NVIDIA GB10 루트 포트, ASM1164), 리셋 후 config
 * 접근이 영영 되지 않거나(Atheros AR9xxx/QCA988x - 이 경우 AER 시스템에서는
 * Link Down 오류가 나고 접근을 시도하면 시스템이 멈추거나 리셋된다),
 * 리셋 자체가 완료되지 않는다(Cavium CN8xxx 루트 포트). TI KeyStone C667X
 * 는 SBR 을 받으면 PCIESS 가 LTSSM 을 자동으로 꺼 버려 장치가 멈춘다.
 * [그대로 두면] 리셋을 시도한 순간 장치가 사라지거나 시스템이 멈춘다.
 * [우회] PCI_DEV_FLAGS_NO_BUS_RESET 플래그를 세워 커널이 이 장치에
 * 버스 리셋을 쓰지 않게 한다. 다른 리셋 방법(FLR, PM 리셋)이 있으면
 * 그쪽으로 물러난다.
 *
 * [대가] TI KeyStone 항목의 영어 주석이 솔직히 밝히듯, 이렇게 하면 VFIO 로
 * VM 에 할당할 수는 있지만 VM 사이에 장치 상태가 새어 나간다. 리셋 없이
 * 넘기기 때문이다.
 *
 * [NVMe 와의 관계] 버스 리셋은 NVMe 컨트롤러 복구 경로에서도 쓰이는 수단
 * 이지만, 아래 등록표에 NVMe 컨트롤러는 없다. 이 파일에서 NVMe 리셋을
 * 직접 다루는 것은 nvme_disable_and_flr() 과 delay_250ms_after_flr() 이다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() 또는 quirk_nvidia_no_bus_reset() -> [quirk_no_bus_reset]
 */
static void quirk_no_bus_reset(struct pci_dev *dev)
{
	dev->dev_flags |= PCI_DEV_FLAGS_NO_BUS_RESET;	/* [한국어] 이 장치에 Secondary Bus Reset 을 쓰지 말라는 표시. */
}

/*
 * After asserting Secondary Bus Reset to downstream devices via a GB10
 * Root Port, the link may not retrain correctly.
 * https://lore.kernel.org/r/20251113084441.2124737-1-Johnny-CC.Chang@mediatek.com
 */
/* [한국어] 위 영어 주석대로 NVIDIA GB10 루트 포트 2종은 SBR 뒤 링크가
 * 제대로 재훈련되지 않는다. 근거 링크가 주석에 있다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NVIDIA, 0x22CE, quirk_no_bus_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NVIDIA, 0x22D0, quirk_no_bus_reset);

/*
 * [한국어]
 * quirk_nvidia_no_bus_reset - device ID 대역으로 NVIDIA GPU 를 골라 SBR 을 막는다
 *
 * @dev: NVIDIA 벤더의 모든 장치
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로 일부 NVIDIA GPU 가 버스 리셋 후
 * 동작하지 않는다.
 * [대상을 어떻게 고르는가] DECLARE 매크로는 device ID 하나만 지정할 수
 * 있는데, 대상이 0x2340~0x237f 라는 연속 대역이다. 그래서 벤더 전체에
 * quirk 를 걸고 함수 안에서 마스크로 대역을 판정한다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nvidia_no_bus_reset] -> quirk_no_bus_reset()
 */
/*
 * Some NVIDIA GPU devices do not work with bus reset, SBR needs to be
 * prevented for those affected devices.
 */
static void quirk_nvidia_no_bus_reset(struct pci_dev *dev)
{
	if ((dev->device & 0xffc0) == 0x2340)	/* [한국어] 0xffc0 마스크로 하위 6비트를 무시하면 0x2340~0x237f 의 64개 device ID 대역을 한 번에 판정할 수 있다. */
		quirk_no_bus_reset(dev);	/* [한국어] 대역에 들면 버스 리셋 금지 플래그를 세운다. */
}
/* [한국어] NVIDIA 벤더의 모든 장치를 HEADER 단계에서 검사한다. 대역
 * 판정은 함수 안에서 한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID,
			 quirk_nvidia_no_bus_reset);

/*
 * Some Atheros AR9xxx and QCA988x chips do not behave after a bus reset.
 * The device will throw a Link Down error on AER-capable systems and
 * regardless of AER, config space of the device is never accessible again
 * and typically causes the system to hang or reset when access is attempted.
 * https://lore.kernel.org/r/20140923210318.498dacbd@dualc.maya.org/
 */
/* [한국어] 위 영어 주석대로 Atheros AR9xxx / QCA988x 6종은 버스 리셋 후
 * config 접근이 영영 불가능해지고, 접근을 시도하면 시스템이 멈추거나
 * 리셋된다. AER 이 있는 시스템에서는 Link Down 오류로 나타난다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATHEROS, 0x0030, quirk_no_bus_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATHEROS, 0x0032, quirk_no_bus_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATHEROS, 0x003c, quirk_no_bus_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATHEROS, 0x0033, quirk_no_bus_reset);
/* [한국어] 이어지는 등록 줄: 0x0034, 0x003e. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATHEROS, 0x0034, quirk_no_bus_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ATHEROS, 0x003e, quirk_no_bus_reset);

/*
 * Root port on some Cavium CN8xxx chips do not successfully complete a bus
 * reset when used with certain child devices.  After the reset, config
 * accesses to the child may fail.
 */
/* [한국어] 위 영어 주석대로 Cavium CN8xxx 의 루트 포트는 특정 하위
 * 장치와 함께 쓰일 때 버스 리셋을 끝내지 못한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_CAVIUM, 0xa100, quirk_no_bus_reset);

/*
 * Some TI KeyStone C667X devices do not support bus/hot reset.  The PCIESS
 * automatically disables LTSSM when Secondary Bus Reset is received and
 * the device stops working.  Prevent bus reset for these devices.  With
 * this change, the device can be assigned to VMs with VFIO, but it will
 * leak state between VMs.  Reference
 * https://e2e.ti.com/support/processors/f/791/t/954382
 */
/* [한국어] 위 영어 주석대로 TI KeyStone C667X 는 SBR 을 받으면 PCIESS 가
 * LTSSM 을 자동으로 꺼 장치가 멈춘다. 이 플래그 덕에 VFIO 할당은 가능해지지만
 * VM 사이에 장치 상태가 새어 나가는 대가가 있다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_TI, 0xb005, quirk_no_bus_reset);

/*
 * Reports from users making use of PCI device assignment with ASM1164
 * controllers indicate an issue with bus reset where the device fails to
 * retrain.  The issue appears more common in configurations with multiple
 * controllers.  The device does indicate PM reset support (NoSoftRst-),
 * therefore this still leaves a viable reset method.
 * https://forum.proxmox.com/threads/problems-with-pcie-passthrough-with-two-identical-devices.149003/
 */
/* [한국어] 위 영어 주석대로 ASM1164 컨트롤러는 장치 할당 중 버스 리셋 뒤
 * 링크 재훈련에 실패한다는 사용자 보고가 있다. 같은 컨트롤러가 여럿 있는
 * 구성에서 더 자주 나타난다. 다행히 이 장치는 PM 리셋을 지원한다고
 * 보고하므로(NoSoftRst-), 버스 리셋을 막아도 쓸 수 있는 리셋 방법이 남는다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ASMEDIA, 0x1164, quirk_no_bus_reset);

/*
 * [한국어]
 * quirk_no_pm_reset - PM 리셋이 실제로는 아무 일도 하지 않는 장치를 표시
 *
 * @dev: 아래 등록표의 ATI VGA 와 Mellanox Spectrum 계열
 * @return: 없음
 *
 * [PM 리셋이란] 장치를 D3hot 으로 내렸다가 D0 로 올리면 내부 상태가
 * 초기화되는 것을 이용한 리셋이다. 장치가 전원관리 capability 에서
 * NoSoftRst 비트를 0 으로(즉 NoSoftRst-) 보고하면 그 전환이 리셋 효과를
 * 낸다는 뜻이다.
 * [무엇이 문제] 아래 두 등록표의 영어 주석이 사정을 밝힌다. AMD/ATI 의
 * 일부 GPU(HD8570 - Oland)와 Mellanox Spectrum-1~4 는 NoSoftRst- 를
 * 보고하지만 실제로는 D3hot->D0 전환이 장치에 아무 영향도 주지 않는다.
 * GPU 는 프레임버퍼 내용과 모니터 동기를 그대로 유지하고, Spectrum 은
 * 계속 동작하며 네트워크 포트도 살아 있다. 즉 capability 가 거짓말이다.
 * [그대로 두면] VFIO 같은 상위 계층이 pci_reset_function() 이 이 장치에
 * 통한다고 믿는다. 실제로는 초기화되지 않은 장치가 게스트에 넘어간다.
 * [우회] PCI_DEV_FLAGS_NO_PM_RESET 을 세워 리셋 방법을 고를 때 PM 리셋을
 * 건너뛰게 한다.
 *
 * [루트 버스 장치를 제외하는 이유] 아래 영어 주석대로, 루트 버스에 직접
 * 붙은 장치는 버스 리셋을 쓸 수 없다. 그런 장치에서는 효과 없는 PM 리셋이라도
 * 아무것도 없는 것보다는 낫다고 보아 플래그를 세우지 않는다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_no_pm_reset]
 */
static void quirk_no_pm_reset(struct pci_dev *dev)
{
	/*
	 * We can't do a bus reset on root bus devices, but an ineffective
	 * PM reset may be better than nothing.
	 */
	if (!pci_is_root_bus(dev->bus))	/* [한국어] 루트 버스 장치가 아닐 때만 플래그를 세운다 - 위 영어 주석이 그 이유를 밝힌다. */
		dev->dev_flags |= PCI_DEV_FLAGS_NO_PM_RESET;	/* [한국어] 리셋 방법 선택에서 PM 리셋을 건너뛰게 한다. */
}

/*
 * Some AMD/ATI GPUS (HD8570 - Oland) report that a D3hot->D0 transition
 * causes a reset (i.e., they advertise NoSoftRst-).  This transition seems
 * to have no effect on the device: it retains the framebuffer contents and
 * monitor sync.  Advertising this support makes other layers, like VFIO,
 * assume pci_reset_function() is viable for this device.  Mark it as
 * unavailable to skip it when testing reset methods.
 */
/* [한국어] 위 영어 주석대로 ATI 의 VGA 클래스 장치 전부가 대상이다.
 * 개별 모델을 고르지 않고 클래스로 넓게 잡았다. */
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_ATI, PCI_ANY_ID,
			       PCI_CLASS_DISPLAY_VGA, 8, quirk_no_pm_reset);

/*
 * Spectrum-{1,2,3,4} devices report that a D3hot->D0 transition causes a reset
 * (i.e., they advertise NoSoftRst-). However, this transition does not have
 * any effect on the device: It continues to be operational and network ports
 * remain up. Advertising this support makes it seem as if a PM reset is viable
 * for these devices. Mark it as unavailable to skip it when testing reset
 * methods.
 */
/* [한국어] 위 영어 주석대로 Mellanox Spectrum-1/2/3/4 스위치 4종. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MELLANOX, 0xcb84, quirk_no_pm_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MELLANOX, 0xcf6c, quirk_no_pm_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MELLANOX, 0xcf70, quirk_no_pm_reset);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MELLANOX, 0xcf80, quirk_no_pm_reset);

/*
 * [한국어]
 * quirk_thunderbolt_hotplug_msi - 핫플러그 MSI 신호가 깨진 썬더볼트 포트의 MSI 를 끈다
 *
 * @pdev: Intel 썬더볼트 컨트롤러의 PCIe 포트
 * @return: 없음
 *
 * [어떤 하드웨어] 아래 영어 주석대로 1세대 전부(Light Ridge, Eagle Ridge,
 * Light Peak)와 2세대 일부(리비전 1 이하의 Cactus Ridge 4C, Port Ridge).
 * [무엇이 문제] 이 컨트롤러들의 PCIe 핫플러그 이벤트가 MSI 로 제대로
 * 전달되지 않는다.
 * [그대로 두면] 장치를 꽂거나 뽑아도 커널이 알아채지 못한다.
 * [우회] 핫플러그 포트인 경우에만 no_msi 를 세워 INTx 로 이벤트를 받게 한다.
 *
 * [is_pciehp 를 확인하는 이유] 같은 컨트롤러 안에도 핫플러그 포트가 아닌
 * 브리지가 있다. 그런 포트까지 MSI 를 끄면 얻는 것 없이 성능만 잃는다.
 *
 * [리비전 조건] Cactus Ridge 4C 는 리비전 1 이하만 결함이 있어 그 모델일
 * 때만 리비전을 추가로 확인한다. 다른 모델은 리비전과 무관하게 대상이다.
 *
 * 실행 컨텍스트: FINAL 단계. is_pciehp 가 확정된 뒤여야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_thunderbolt_hotplug_msi]
 */
/*
 * Thunderbolt controllers with broken MSI hotplug signaling:
 * Entire 1st generation (Light Ridge, Eagle Ridge, Light Peak) and part
 * of the 2nd generation (Cactus Ridge 4C up to revision 1, Port Ridge).
 */
static void quirk_thunderbolt_hotplug_msi(struct pci_dev *pdev)
{
	if (pdev->is_pciehp &&	/* [한국어] PCIe 핫플러그 포트일 때만 의미가 있다. */
	    (pdev->device != PCI_DEVICE_ID_INTEL_CACTUS_RIDGE_4C ||	/* [한국어] Cactus Ridge 4C 가 아니면 리비전과 무관하게 대상이고, */
	     pdev->revision <= 1))	/* [한국어] Cactus Ridge 4C 이면 리비전 1 이하만 대상이다. */
		pdev->no_msi = 1;	/* [한국어] 핫플러그 이벤트를 INTx 로 받도록 MSI 를 끈다. */
}
/* [한국어] 문제 있는 썬더볼트 컨트롤러 5종에 FINAL 단계로 등록한다.
 * 실제 적용 여부는 함수 안에서 핫플러그 포트인지와 리비전으로 판정한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_LIGHT_RIDGE,
			quirk_thunderbolt_hotplug_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_EAGLE_RIDGE,
			quirk_thunderbolt_hotplug_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_LIGHT_PEAK,
			quirk_thunderbolt_hotplug_msi);
/* [한국어] 이어지는 등록 줄: PCI_DEVICE_ID_INTEL_CACTUS_RIDGE_4C,
 * PCI_DEVICE_ID_INTEL_PORT_RIDGE. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_CACTUS_RIDGE_4C,
			quirk_thunderbolt_hotplug_msi);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_PORT_RIDGE,
			quirk_thunderbolt_hotplug_msi);

/* [한국어] 아래 quirk 는 ACPI 메서드를 직접 실행하므로 ACPI 가 있는
 * 빌드에서만 컴파일한다. */
#ifdef CONFIG_ACPI
/*
 * [한국어]
 * quirk_apple_poweroff_thunderbolt - 절전 전에 썬더볼트 컨트롤러의 전원을 끊는다
 *
 * @dev: Cactus Ridge 4C 썬더볼트 컨트롤러의 업스트림 브리지
 * @return: 없음
 *
 * [어떤 하드웨어] 애플 하드웨어의 Cactus Ridge 썬더볼트 컨트롤러.
 * [무엇이 문제] 아래 영어 주석대로, 절전 전에 이 컨트롤러를 끄지 않으면,
 * 절전 전에 장치가 꽂혀 있었을 경우 재개 후 NHI(Native Host Interface)가
 * 나타나지 않는다.
 * [그대로 두면] 절전에서 깨어난 뒤 썬더볼트가 동작하지 않는다.
 * [우회] ACPI 가 제공하는 SXIO/SXFP/SXLV 메서드를 정해진 순서로 실행해
 * 칩 전체의 전원을 끊는다. 전원은 재개 전에 펌웨어가 알아서 되돌려 주므로
 * 커널이 다시 켤 필요가 없다.
 *
 * [SUSPEND_LATE 단계인 이유] 영어 주석대로 이 quirk 는 칩 전체의 전원을
 * 끊는다. 그러려면 업스트림 브리지의 suspend_noirq 시점, 즉 하위 장치들이
 * 모두 절전에 들어간 뒤여야 한다.
 *
 * [펌웨어 절전일 때만 하는 이유] 영어 주석대로 커널은 이 전원을 다시 켜는
 * 방법을 모르고 펌웨어만 안다. 그래서 펌웨어를 경유하는 절전일 때만 쓴다.
 *
 * [ACPI 메서드 존재로 대상을 확인하는 이유] 영어 주석대로, 외장 썬더볼트
 * 장치 안의 브리지도 호스트의 것과 같은 device ID 를 가질 수 있다. 하지만
 * 그것들에는 이 ACPI 메서드가 딸려 있지 않으므로, 메서드를 찾을 수 있는지
 * 자체가 '올바른 브리지인가' 를 가르는 암묵적 검사가 된다.
 *
 * 실행 컨텍스트: SUSPEND_LATE 단계. msleep() 을 쓰므로 잠들 수 있는
 * 문맥이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_apple_poweroff_thunderbolt]
 *     -> acpi_get_handle() -> acpi_execute_simple_method()
 */
/*
 * Apple: Shutdown Cactus Ridge Thunderbolt controller.
 *
 * On Apple hardware the Cactus Ridge Thunderbolt controller needs to be
 * shutdown before suspend. Otherwise the native host interface (NHI) will not
 * be present after resume if a device was plugged in before suspend.
 *
 * The Thunderbolt controller consists of a PCIe switch with downstream
 * bridges leading to the NHI and to the tunnel PCI bridges.
 *
 * This quirk cuts power to the whole chip. Therefore we have to apply it
 * during suspend_noirq of the upstream bridge.
 *
 * Power is automagically restored before resume. No action is needed.
 */
static void quirk_apple_poweroff_thunderbolt(struct pci_dev *dev)
{
	acpi_handle bridge, SXIO, SXFP, SXLV;	/* [한국어] bridge: 이 PCI 장치에 대응하는 ACPI 노드, 나머지 셋은 전원을 끊는 ACPI 메서드 핸들. */

	if (!x86_apple_machine)	/* [한국어] 애플 하드웨어가 아니면 이 ACPI 메서드가 없다. */
		return;
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_UPSTREAM)	/* [한국어] 썬더볼트 스위치의 업스트림 포트에서만 실행한다 - 칩 전체 전원을 끊는 것이므로 가장 위에서 해야 한다. */
		return;

	/*
	 * SXIO/SXFP/SXLF turns off power to the Thunderbolt controller.
	 * We don't know how to turn it back on again, but firmware does,
	 * so we can only use SXIO/SXFP/SXLF if we're suspending via
	 * firmware.
	 */
	if (!pm_suspend_via_firmware())	/* [한국어] 위 영어 주석대로 펌웨어를 경유하는 절전일 때만 안전하다. 커널은 전원을 다시 켜는 방법을 모른다. */
		return;

	bridge = ACPI_HANDLE(&dev->dev);	/* [한국어] 이 PCI 장치에 대응하는 ACPI 노드를 얻는다. */
	if (!bridge)	/* [한국어] ACPI 노드가 없으면 메서드를 찾을 수 없다. */
		return;

	/*
	 * SXIO and SXLV are present only on machines requiring this quirk.
	 * Thunderbolt bridges in external devices might have the same
	 * device ID as those on the host, but they will not have the
	 * associated ACPI methods. This implicitly checks that we are at
	 * the right bridge.
	 */
	if (ACPI_FAILURE(acpi_get_handle(bridge, "DSB0.NHI0.SXIO", &SXIO))	/* [한국어] SXIO 메서드를 찾는다. 경로는 이 브리지 아래의 DSB0.NHI0 이다. */
	    || ACPI_FAILURE(acpi_get_handle(bridge, "DSB0.NHI0.SXFP", &SXFP))	/* [한국어] SXFP 메서드도 있어야 하고, */
	    || ACPI_FAILURE(acpi_get_handle(bridge, "DSB0.NHI0.SXLV", &SXLV)))	/* [한국어] SXLV 메서드도 있어야 한다. 하나라도 없으면 이 브리지는 대상이 아니다. */
		return;
	pci_info(dev, "quirk: cutting power to Thunderbolt controller...\n");	/* [한국어] 전원을 끊는다는 사실을 남긴다. */

	/* magic sequence */
	acpi_execute_simple_method(SXIO, NULL, 1);	/* [한국어] SXIO(1) - 아래 순서는 옆의 영어 주석대로 '매직 시퀀스' 이며 각 단계의 의미는 원본 주석에 없어 이 트리의 정보만으로는 확인할 수 없다. */
	acpi_execute_simple_method(SXFP, NULL, 0);	/* [한국어] SXFP(0). */
	msleep(300);	/* [한국어] 하드웨어가 상태를 바꿀 시간을 준다. 300ms 라는 값의 근거는 원본 주석에 없다. */
	acpi_execute_simple_method(SXLV, NULL, 0);	/* [한국어] SXLV(0). */
	acpi_execute_simple_method(SXIO, NULL, 0);	/* [한국어] SXIO(0). */
	acpi_execute_simple_method(SXLV, NULL, 0);	/* [한국어] SXLV(0) 를 한 번 더 - 시퀀스의 마지막 단계다. */
}
/* [한국어] Cactus Ridge 4C 에 SUSPEND_LATE 단계로 등록한다. 하위 장치가
 * 모두 절전에 들어간 뒤여야 칩 전원을 끊을 수 있다. */
DECLARE_PCI_FIXUP_SUSPEND_LATE(PCI_VENDOR_ID_INTEL,
			       PCI_DEVICE_ID_INTEL_CACTUS_RIDGE_4C,
			       quirk_apple_poweroff_thunderbolt);
/* [한국어] CONFIG_ACPI 블록의 끝. */
#endif

/*
 * [한국어]
 * reset_intel_82599_sfp_virtfn - 82599 가상 함수를 FLR 로 리셋한다
 *
 * @dev: Intel 82599 SFP 가상 함수(VF)
 * @probe: true 면 '이 방법을 쓸 수 있는가' 만 답하고 실제로 리셋하지 않는다
 * @return: 0 (이 방법을 쓸 수 있음)
 *
 * [장치별 리셋 방법이란] 아래 영어 주석대로, 여기서부터는 표준 방법(FLR,
 * D0->D3->D0 PM 리셋 등)을 쓸 수 없는 장치를 위한 벤더별 리셋 루틴이다.
 * 아래 pci_dev_reset_methods[] 표에 등록되어 pci_dev_specific_reset() 이
 * 디스패치한다.
 *
 * [probe 인자의 규약] PCI 코어는 먼저 probe=true 로 불러 '이 방법이
 * 가능한가' 를 묻고, 가능하면 probe=false 로 다시 불러 실제로 리셋한다.
 *
 * [무엇이 문제] 아래 영어 주석대로 82599 는 VF 에 대해 FLR 을 지원하지만,
 * FLR 지원 사실이 PF 의 DEVCAP(데이터시트 9.3.10.4)에만 적혀 있고 VF 의
 * DEVCAP(9.5)에는 없다. 즉 VF 의 capability 만 보면 FLR 을 못 쓰는 것처럼
 * 보인다.
 * [그대로 두면] 커널이 VF 를 리셋할 방법이 없다고 판단한다.
 * [우회] capability 검사를 건너뛰고 pcie_flr() 을 직접 호출한다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_dev_specific_reset() -> [reset_intel_82599_sfp_virtfn] -> pcie_flr()
 */
/*
 * Following are device-specific reset methods which can be used to
 * reset a single function if other methods (e.g. FLR, PM D0->D3) are
 * not available.
 */
static int reset_intel_82599_sfp_virtfn(struct pci_dev *dev, bool probe)
{
	/*
	 * http://www.intel.com/content/dam/doc/datasheet/82599-10-gbe-controller-datasheet.pdf
	 *
	 * The 82599 supports FLR on VFs, but FLR support is reported only
	 * in the PF DEVCAP (sec 9.3.10.4), not in the VF DEVCAP (sec 9.5).
	 * Thus we must call pcie_flr() directly without first checking if it is
	 * supported.
	 */
	if (!probe)	/* [한국어] probe 호출이면 아무것도 하지 않고 '가능하다' 만 알린다. */
		pcie_flr(dev);	/* [한국어] capability 확인 없이 곧바로 FLR 을 건다 - 위 영어 주석이 그 이유다. */
	return 0;	/* [한국어] 언제나 성공으로 보고한다. probe 든 실제 실행이든 0 을 돌려준다. */
}

/* [한국어] 아래 6개 매크로는 Ivy Bridge 내장 그래픽(IGD)을 리셋할 때 쓰는
 * MMIO 레지스터 오프셋과 타임아웃이다. 표준 PCI 가 아니라 i915 하드웨어
 * 문서의 값이며, BAR0 에 매핑된 GPU 레지스터 창의 오프셋이다. */
#define SOUTH_CHICKEN2		0xc2004	/* [한국어] 디스플레이 엔진의 클럭 게이팅 제어 레지스터. */
#define PCH_PP_STATUS		0xc7200	/* [한국어] 패널 전원 상태 레지스터 - 패널이 꺼졌는지 확인하는 데 쓴다. */
#define PCH_PP_CONTROL		0xc7204	/* [한국어] 패널 전원 제어 레지스터 - 패널 전원을 끄는 데 쓴다. */
#define MSG_CTL			0x45010	/* [한국어] 메시지 버스 제어 레지스터. */
#define NSDE_PWR_STATE		0xd0100	/* [한국어] 노스/사우스 디스플레이 엔진의 전원 상태 레지스터. */
#define IGD_OPERATION_TIMEOUT	10000     /* set timeout 10 seconds */

/*
 * [한국어]
 * reset_ivb_igd - Ivy Bridge 내장 그래픽을 MMIO 시퀀스로 리셋한다
 *
 * @dev: Intel Ivy Bridge 모바일 VGA(0x0156, 0x0166)
 * @probe: true 면 이 방법을 쓸 수 있는지만 답한다
 * @return: 0 성공, -ENOMEM 이면 BAR 매핑 실패
 *
 * [무엇이 문제] 이 내장 그래픽에는 표준 FLR 이 없다. 그래서 장치 할당
 * 등을 위해 리셋하려면 GPU 레지스터를 직접 만져 디스플레이 엔진과 패널
 * 전원을 정리하는 수밖에 없다. 구체적인 결함이 아니라 '표준 리셋 수단이
 * 없다' 는 것이 이 함수의 존재 이유다.
 * [무엇을 하는가] BAR0 을 매핑해 (1) 메시지 버스를 초기화하고, (2) 클럭
 * 게이팅 레지스터를 정해진 값으로 덮어쓰고, (3) 패널 전원을 끈 뒤 실제로
 * 꺼질 때까지 최대 10초를 기다리고, (4) 디스플레이 엔진 전원 상태를
 * 설정한다.
 *
 * [SOUTH_CHICKEN2 를 덮어써도 되는 이유] 아래 영어 주석이 밝힌다. 원래
 * 이 레지스터를 함부로 덮어쓰면 안 되지만, 이것은 리셋이고 다음에 로드될
 * 드라이버가 필요한 비트를 다시 설정한다. 게다가 지금 들어 있는 값은
 * 앞서 i915 가 설정한 것이므로 잃어도 무방하다.
 *
 * [매직 값] 각 레지스터에 쓰는 값(0x00000002, 0x00000005 등)의 의미는
 * 원본 주석에 없어 이 트리의 정보만으로는 확인할 수 없다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥. msleep() 을 쓰므로 잠들 수
 * 있어야 한다.
 *
 * 호출 체인:
 *   pci_dev_specific_reset() -> [reset_ivb_igd] -> pci_iomap() -> iowrite32()
 */
static int reset_ivb_igd(struct pci_dev *dev, bool probe)
{
	void __iomem *mmio_base;	/* [한국어] BAR0 에 매핑된 GPU 레지스터 창의 시작 주소. */
	unsigned long timeout;	/* [한국어] 패널 전원이 꺼지기를 기다리는 마감 시각(jiffies). */
	u32 val;	/* [한국어] 레지스터 값을 담는 임시 변수. */

	if (probe)	/* [한국어] probe 호출이면 이 방법을 쓸 수 있다고만 답한다. */
		return 0;

	mmio_base = pci_iomap(dev, 0, 0);	/* [한국어] BAR0 전체를 매핑한다. 세 번째 인자 0 은 '가능한 만큼 전부'. */
	if (!mmio_base)	/* [한국어] 매핑 실패는 메모리 부족으로 보고한다. */
		return -ENOMEM;

	iowrite32(0x00000002, mmio_base + MSG_CTL);	/* [한국어] 메시지 버스 제어 레지스터에 2 를 쓴다. 값의 의미는 원본 주석에 없다. */

	/*
	 * Clobbering SOUTH_CHICKEN2 register is fine only if the next
	 * driver loaded sets the right bits. However, this's a reset and
	 * the bits have been set by i915 previously, so we clobber
	 * SOUTH_CHICKEN2 register directly here.
	 */
	iowrite32(0x00000005, mmio_base + SOUTH_CHICKEN2);	/* [한국어] 위 영어 주석의 판단에 따라 클럭 게이팅 레지스터를 그대로 덮어쓴다. */

	val = ioread32(mmio_base + PCH_PP_CONTROL) & 0xfffffffe;	/* [한국어] 패널 전원 제어 레지스터를 읽어 비트 0(패널 전원 켜기)만 지운다. */
	iowrite32(val, mmio_base + PCH_PP_CONTROL);	/* [한국어] 그 값을 되써서 패널 전원을 끈다. */

	timeout = jiffies + msecs_to_jiffies(IGD_OPERATION_TIMEOUT);	/* [한국어] 최대 10초의 마감 시각을 잡는다. */
	do {	/* [한국어] 패널이 실제로 꺼질 때까지 폴링한다. */
		val = ioread32(mmio_base + PCH_PP_STATUS);	/* [한국어] 패널 전원 상태 레지스터를 읽는다. */
		if ((val & 0xb0000000) == 0)	/* [한국어] 상위 비트들(0xb0000000)이 모두 0 이면 패널이 완전히 꺼진 것이다. */
			goto reset_complete;	/* [한국어] 정리 지점으로 뛴다. */
		msleep(10);	/* [한국어] 10ms 쉬고 다시 확인한다 - 바쁜 대기를 피한다. */
	} while (time_before(jiffies, timeout));	/* [한국어] 마감 시각을 넘지 않는 동안 반복한다. */
	pci_warn(dev, "timeout during reset\n");	/* [한국어] 10초 안에 꺼지지 않았다. 경고만 남기고 계속 진행한다 - 리셋 자체는 마쳐야 한다. */

reset_complete:	/* [한국어] 정상 완료와 타임아웃이 함께 지나는 지점. */
	iowrite32(0x00000002, mmio_base + NSDE_PWR_STATE);	/* [한국어] 디스플레이 엔진 전원 상태 레지스터에 2 를 쓴다. 값의 의미는 원본 주석에 없다. */

	pci_iounmap(dev, mmio_base);	/* [한국어] 임시 매핑을 해제한다. */
	return 0;	/* [한국어] 타임아웃이 있었더라도 성공으로 보고한다 - 더 나은 리셋 수단이 없기 때문이다. */
}

/*
 * [한국어]
 * reset_chelsio_generic_dev - Chelsio T4 어댑터를 안전하게 FLR 한다
 *
 * @dev: Chelsio 벤더의 장치(T4 계열인지는 함수 안에서 확인)
 * @probe: true 면 이 방법을 쓸 수 있는지만 답한다
 * @return: 0 성공, -ENOTTY 면 T4 계열이 아니라 이 방법을 쓸 수 없음
 *
 * [무엇이 문제] 표준 FLR 만으로는 이 칩이 안전하게 리셋되지 않는다. 아래
 * 영어 주석이 두 가지 이유를 밝힌다.
 *   (1) 칩 안에 진행 중인 DMA 가 있는데 Bus Master 가 꺼져 있으면 T4 가
 *       멈춰 버린다(wedge). 그런데 pci_reset_function() 은 리셋 전에
 *       Bus Master 를 끈다. 그래서 FLR 이 끝날 때까지 다시 켜 두어야 한다.
 *   (2) MSI-X 인터럽트가 꺼진 상태에서 MSI-X 메시지를 보내야 하는 상황이
 *       되면 Head-Of-Line 블로킹이 생긴다. 그래서 FLR 동안만 잠시 MSI-X 를
 *       켜 둔다(모든 벡터를 마스크한 채로).
 * [우회] Command 레지스터와 MSI-X 설정을 임시로 바꿔 두고 FLR 을 건 뒤,
 * pci_restore_state() 와 Command 되쓰기로 원래 상태를 복원한다.
 *
 * [T4 판별] Chelsio 벤더 전체에 등록되어 있어, device ID 의 상위 니블이
 * 4 인지(0x4xxx)로 T4 계열을 가려낸다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_dev_specific_reset() -> [reset_chelsio_generic_dev] -> pcie_flr()
 */
/* Device-specific reset method for Chelsio T4-based adapters */
static int reset_chelsio_generic_dev(struct pci_dev *dev, bool probe)
{
	u16 old_command;	/* [한국어] 리셋 전 Command 레지스터 값 - 끝에 그대로 되돌린다. */
	u16 msix_flags;	/* [한국어] MSI-X 제어 워드의 원래 값. */

	/*
	 * If this isn't a Chelsio T4-based device, return -ENOTTY indicating
	 * that we have no device-specific reset method.
	 */
	if ((dev->device & 0xf000) != 0x4000)	/* [한국어] 위 영어 주석대로 T4 계열(0x4xxx)이 아니면 이 방법을 쓸 수 없다. */
		return -ENOTTY;	/* [한국어] -ENOTTY 는 '이 방법은 이 장치에 해당하지 않는다' 는 뜻이다. */

	/*
	 * If this is the "probe" phase, return 0 indicating that we can
	 * reset this device.
	 */
	if (probe)	/* [한국어] probe 호출이면 쓸 수 있다고만 답한다. */
		return 0;

	/*
	 * T4 can wedge if there are DMAs in flight within the chip and Bus
	 * Master has been disabled.  We need to have it on till the Function
	 * Level Reset completes.  (BUS_MASTER is disabled in
	 * pci_reset_function()).
	 */
	pci_read_config_word(dev, PCI_COMMAND, &old_command);	/* [한국어] 현재 Command 값을 저장한다. */
	pci_write_config_word(dev, PCI_COMMAND,	/* [한국어] Bus Master 비트를 켜 둔다 - 위 영어 주석대로 진행 중인 DMA 가 있는 채로 Bus Master 가 꺼져 있으면 칩이 멈춘다. */
			      old_command | PCI_COMMAND_MASTER);

	/*
	 * Perform the actual device function reset, saving and restoring
	 * configuration information around the reset.
	 */
	pci_save_state(dev);	/* [한국어] BAR 값 등 config space 상태를 저장해 둔다. FLR 은 그것을 모두 날린다. */

	/*
	 * T4 also suffers a Head-Of-Line blocking problem if MSI-X interrupts
	 * are disabled when an MSI-X interrupt message needs to be delivered.
	 * So we briefly re-enable MSI-X interrupts for the duration of the
	 * FLR.  The pci_restore_state() below will restore the original
	 * MSI-X state.
	 */
	pci_read_config_word(dev, dev->msix_cap+PCI_MSIX_FLAGS, &msix_flags);	/* [한국어] MSI-X capability 의 제어 워드를 읽는다. dev->msix_cap 이 그 capability 의 시작 오프셋이다. */
	if ((msix_flags & PCI_MSIX_FLAGS_ENABLE) == 0)	/* [한국어] MSI-X 가 꺼져 있을 때만 임시로 켠다. */
		pci_write_config_word(dev, dev->msix_cap+PCI_MSIX_FLAGS,	/* [한국어] 제어 워드를 다시 쓴다. */
				      msix_flags |	/* [한국어] 원래 값에 */
				      PCI_MSIX_FLAGS_ENABLE |	/* [한국어] MSI-X 활성 비트와 */
				      PCI_MSIX_FLAGS_MASKALL);	/* [한국어] 전체 마스크 비트를 함께 세운다. 마스크를 걸어 두면 실제 인터럽트는 나가지 않으면서 Head-Of-Line 블로킹만 피할 수 있다. */

	pcie_flr(dev);	/* [한국어] 이제 안전하게 Function Level Reset 을 건다. */

	/*
	 * Restore the configuration information (BAR values, etc.) including
	 * the original PCI Configuration Space Command word, and return
	 * success.
	 */
	pci_restore_state(dev);	/* [한국어] 위 영어 주석대로 저장해 둔 config space 상태를 되돌린다. 여기서 MSI-X 설정도 원래대로 복원된다. */
	pci_write_config_word(dev, PCI_COMMAND, old_command);	/* [한국어] Command 레지스터를 리셋 전 값으로 되돌린다 - 임시로 켠 Bus Master 도 여기서 원래대로 간다. */
	return 0;	/* [한국어] 성공을 보고한다. */
}

/* [한국어] 아래 세 device ID 는 위 리셋 함수들이 등록될 대상이다.
 * include/linux/pci_ids.h 에 정의되어 있지 않아 이 파일에서 직접 정의한다. */
#define PCI_DEVICE_ID_INTEL_82599_SFP_VF   0x10ed	/* [한국어] Intel 82599 SFP 의 가상 함수(VF). */
#define PCI_DEVICE_ID_INTEL_IVB_M_VGA      0x0156	/* [한국어] Ivy Bridge 모바일 내장 그래픽. */
#define PCI_DEVICE_ID_INTEL_IVB_M2_VGA     0x0166	/* [한국어] Ivy Bridge 모바일 내장 그래픽(두 번째 변종). */

/*
 * The Samsung SM961/PM961 controller can sometimes enter a fatal state after
 * FLR where config space reads from the device return -1.  We seem to be
 * able to avoid this condition if we disable the NVMe controller prior to
 * FLR.  This quirk is generic for any NVMe class device requiring similar
 * assistance to quiesce the device prior to FLR.
 *
 * NVMe specification: https://nvmexpress.org/resources/specifications/
 * Revision 1.0e:
 *    Chapter 2: Required and optional PCI config registers
 *    Chapter 3: NVMe control registers
 *    Chapter 7.3: Reset behavior
 */
/*
 * [한국어]
 * nvme_disable_and_flr - NVMe 컨트롤러를 재운 뒤 FLR 을 건다 (이 파일의 NVMe 접점)
 *
 * @dev: 클래스 코드가 PCI_CLASS_STORAGE_EXPRESS 인 장치. 등록표에는 삼성
 *       0xa804 로 걸려 있다.
 * @probe: true 면 이 방법을 쓸 수 있는지만 답하고 실제 리셋은 하지 않는다
 * @return: 0 성공, -ENOTTY 면 이 방법을 쓸 수 없음
 *
 * [어떤 하드웨어] 위 영어 주석대로 삼성 SM961/PM961 컨트롤러에서 관측된
 * 문제다. 다만 이 함수 자체는 NVMe 클래스 장치 일반에 쓸 수 있게 작성되어
 * 있다(영어 주석의 표현대로 generic for any NVMe class device).
 * [무엇이 문제] FLR 을 걸고 나면 컨트롤러가 치명적 상태에 빠져 config
 * space 읽기가 전부 -1 로 돌아오는 경우가 있다. -1 은 응답하지 않는 장치를
 * 읽었을 때 버스가 돌려주는 값이므로, 사실상 장치가 사라진 것이다.
 * [그대로 두면] 리셋 후 장치를 다시 쓸 수 없다.
 * [우회] FLR 을 걸기 전에 NVMe 컨트롤러를 스스로 정지시킨다. 구체적으로는
 * BAR0 에 매핑된 NVMe 레지스터에서 CC(Controller Configuration)의 EN 비트를
 * 0 으로 만들고, CSTS(Controller Status)의 RDY 비트가 0 이 될 때까지
 * 기다린 뒤 FLR 을 건다.
 *
 * [★ 이 파일이 NVMe 를 아는 유일한 이유] 이 함수 때문에 파일 상단에서
 * linux/nvme.h 를 포함한다. NVME_REG_CC, NVME_REG_CSTS, NVME_REG_CAP,
 * NVME_CC_ENABLE, NVME_CSTS_RDY, NVME_CAP_TIMEOUT 이 모두 그 헤더의 것이다.
 * PCI 계층이 특정 장치 클래스의 레지스터를 직접 만지는 드문 예다.
 *
 * [클래스 코드] PCI_CLASS_STORAGE_EXPRESS 의 상수 정의는
 * include/linux/pci_ids.h 에 있으며 이 sparse checkout 에는 그 헤더가 없다.
 *
 * [BAR0 에 무엇이 있는가] NVMe 스펙상 컨트롤러 레지스터는 BAR0 에 매핑된다.
 * CAP(0x00), VS(0x08), CC(0x14), CSTS(0x1c) 순으로 놓이고 그 뒤에 도어벨이
 * 이어진다. 그래서 pci_iomap 의 길이를 NVME_REG_CC + 4 로 잡으면 CC 까지
 * 닿을 수 있다(CSTS 는 CC 바로 뒤라 같은 페이지 안에 들어온다).
 *
 * [누가 이 함수를 부르는가] 아래 pci_dev_reset_methods[] 표에 등록되어
 * pci_dev_specific_reset() 이 부른다. NVMe 드라이버가 직접 부르지는 않는다 -
 * drivers/nvme/host/pci.c 에는 pci_reset_function() 계열 호출이 없다.
 * 이 경로는 PCI 코어의 리셋 기능(예: sysfs 의 reset, VFIO 장치 할당)에서
 * 들어온다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥. msleep() 을 쓰므로 잠들 수
 * 있어야 한다.
 *
 * 호출 체인:
 *   pci_dev_specific_reset() -> [nvme_disable_and_flr]
 *     -> pci_iomap() -> readl/writel -> pcie_flr()
 */
static int nvme_disable_and_flr(struct pci_dev *dev, bool probe)
{
	void __iomem *bar;	/* [한국어] BAR0 에 매핑된 NVMe 컨트롤러 레지스터 창의 시작 주소. */
	u16 cmd;	/* [한국어] Command 레지스터 값 - 메모리 디코딩을 켜기 위해 읽는다. */
	u32 cfg;	/* [한국어] CC(Controller Configuration) 레지스터 값. */

	if (dev->class != PCI_CLASS_STORAGE_EXPRESS ||	/* [한국어] 클래스 코드가 NVMe(PCI_CLASS_STORAGE_EXPRESS)여야 하고, */
	    pcie_reset_flr(dev, PCI_RESET_PROBE) || !pci_resource_start(dev, 0))	/* [한국어] 표준 FLR 을 쓸 수 있어야 하며(probe 로 확인), BAR0 이 배정되어 있어야 레지스터를 만질 수 있다. */
		return -ENOTTY;	/* [한국어] 하나라도 어긋나면 이 방법을 쓸 수 없다. */

	if (probe)	/* [한국어] probe 호출이면 여기까지의 검사만으로 '쓸 수 있다' 고 답한다. */
		return 0;

	bar = pci_iomap(dev, 0, NVME_REG_CC + sizeof(cfg));	/* [한국어] BAR0 을 CC 레지스터까지 닿도록 매핑한다. sizeof(cfg) 를 더해 CC 자체를 포함시킨다. */
	if (!bar)	/* [한국어] 매핑에 실패하면 이 방법을 쓸 수 없다. */
		return -ENOTTY;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);	/* [한국어] 현재 Command 레지스터를 읽는다. */
	pci_write_config_word(dev, PCI_COMMAND, cmd | PCI_COMMAND_MEMORY);	/* [한국어] Memory Space Enable 을 켠다 - 이것이 꺼져 있으면 방금 매핑한 MMIO 로 접근해도 응답이 없다. */

	cfg = readl(bar + NVME_REG_CC);	/* [한국어] CC 레지스터를 읽는다. NVMe 스펙상 오프셋 0x14 다. */

	/* Disable controller if enabled */	/* [한국어] CC.EN 이 서 있으면 컨트롤러가 동작 중이라는 뜻이다. */
	if (cfg & NVME_CC_ENABLE) {	/* [한국어] CC.EN(비트 0)이 서 있으면 컨트롤러가 동작 중이므로 먼저 재워야 한다. */
		u32 cap = readl(bar + NVME_REG_CAP);	/* [한국어] CAP(Capabilities) 레지스터를 읽는다. 타임아웃 정보가 여기 있다. */
		unsigned long timeout;	/* [한국어] RDY 가 내려가기를 기다릴 마감 시각. */

		/*
		 * Per nvme_disable_ctrl() skip shutdown notification as it
		 * could complete commands to the admin queue.  We only intend
		 * to quiesce the device before reset.
		 */
		cfg &= ~(NVME_CC_SHN_MASK | NVME_CC_ENABLE);	/* [한국어] 위 영어 주석대로 CC.SHN(Shutdown Notification) 필드를 함께 0 으로 만든다. 정상 종료 통지를 보내면 admin 큐의 명령이 완료 처리될 수 있는데, 여기서는 리셋 전에 조용히 재우는 것만이 목적이기 때문이다. */

		writel(cfg, bar + NVME_REG_CC);	/* [한국어] 고친 CC 를 써 넣는다. 이 쓰기가 컨트롤러를 정지시킨다. */

		/*
		 * Some controllers require an additional delay here, see
		 * NVME_QUIRK_DELAY_BEFORE_CHK_RDY.  None of those are yet
		 * supported by this quirk.
		 */

		/* Cap register provides max timeout in 500ms increments */
		timeout = ((NVME_CAP_TIMEOUT(cap) + 1) * HZ / 2) + jiffies;	/* [한국어] 옆의 영어 주석대로 CAP.TO 필드는 최대 타임아웃을 500ms 단위로 담는다. (TO+1) * HZ/2 가 그 값을 jiffies 로 바꾼 것이다. */

		for (;;) {	/* [한국어] RDY 가 내려갈 때까지 도는 무한 루프. 아래 두 break 로만 빠져나온다. */
			u32 status = readl(bar + NVME_REG_CSTS);	/* [한국어] CSTS(Controller Status) 레지스터를 읽는다. NVMe 스펙상 오프셋 0x1c 다. */

			/* Ready status becomes zero on disable complete */	/* [한국어] NVMe 스펙상 컨트롤러가 완전히 멈추면 CSTS.RDY 가 0 이 된다. */
			if (!(status & NVME_CSTS_RDY))	/* [한국어] 옆의 영어 주석대로 정지가 끝나면 RDY 비트가 0 이 된다. */
				break;	/* [한국어] 정상적으로 재워졌다. */

			msleep(100);	/* [한국어] 100ms 쉬고 다시 확인한다. */

			if (time_after(jiffies, timeout)) {	/* [한국어] CAP.TO 가 정한 마감 시각을 넘었다면 */
				pci_warn(dev, "Timeout waiting for NVMe ready status to clear after disable\n");	/* [한국어] 경고만 남기고 */
				break;	/* [한국어] 루프를 빠져나간다 - 리셋 자체는 진행해야 한다. */
			}
		}
	}

	pci_iounmap(dev, bar);	/* [한국어] 임시 매핑을 해제한다. FLR 이후에는 BAR 값이 초기화되므로 미리 풀어야 한다. */

	pcie_flr(dev);	/* [한국어] 이제 안전하게 Function Level Reset 을 건다. 컨트롤러가 이미 정지해 있으므로 문제의 치명적 상태에 빠지지 않는다. */

	return 0;	/* [한국어] 성공을 보고한다. */
}

/*
 * Some NVMe controllers such as Intel DC P3700 and Solidigm P44 Pro will
 * timeout waiting for ready status to change after NVMe enable if the driver
 * starts interacting with the device too soon after FLR.  A 250ms delay after
 * FLR has heuristically proven to produce reliably working results for device
 * assignment cases.
 */
/*
 * [한국어]
 * delay_250ms_after_flr - FLR 뒤 250ms 를 기다려 NVMe 컨트롤러가 자리잡게 한다
 *
 * @dev: Intel 0x0953 / 0x0a54, Solidigm 0xf1ac 로 등록된 NVMe 컨트롤러
 * @probe: true 면 표준 FLR 을 쓸 수 있는지 확인만 한다
 * @return: 0 성공(probe 시에는 pcie_reset_flr 의 확인 결과)
 *
 * [어떤 하드웨어] 위 영어 주석대로 Intel DC P3700 과 Solidigm P44 Pro 같은
 * 일부 NVMe 컨트롤러.
 * [무엇이 문제] FLR 직후 드라이버가 너무 빨리 장치와 상호작용을 시작하면,
 * NVMe enable 이후 ready 상태가 되기를 기다리는 단계에서 타임아웃이 난다.
 * 즉 FLR 이 끝났다고 보고된 뒤에도 컨트롤러 내부 준비가 덜 끝나 있다.
 * [그대로 두면] 장치 할당 시 NVMe 초기화가 실패한다.
 * [우회] 표준 FLR 을 건 뒤 250ms 를 무조건 기다린다. 영어 주석이 밝히듯
 * 이 값은 이론적 근거가 아니라 경험적으로(heuristically) 장치 할당
 * 사례에서 안정적으로 동작한다고 확인된 값이다.
 *
 * [nvme_disable_and_flr() 과의 차이] 그쪽은 FLR '전에' 컨트롤러를 재우고,
 * 이쪽은 FLR '후에' 기다린다. 같은 NVMe 리셋 문제라도 증상과 해법이 다르다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥. msleep() 을 쓴다.
 *
 * 호출 체인:
 *   pci_dev_specific_reset() -> [delay_250ms_after_flr] -> pcie_reset_flr()
 */
static int delay_250ms_after_flr(struct pci_dev *dev, bool probe)
{
	if (probe)	/* [한국어] probe 호출이면 */
		return pcie_reset_flr(dev, PCI_RESET_PROBE);	/* [한국어] 표준 FLR 을 쓸 수 있는지 확인한 결과를 그대로 돌려준다. 이 quirk 는 FLR 위에 지연만 얹는 것이라 FLR 자체가 안 되면 쓸 수 없다. */

	pcie_reset_flr(dev, PCI_RESET_DO_RESET);	/* [한국어] 실제 리셋 - 표준 FLR 을 건다. */

	msleep(250);	/* [한국어] 경험적으로 정해진 250ms 를 기다린다. 이 대기가 이 quirk 의 전부다. */

	return 0;	/* [한국어] 성공을 보고한다. */
}

/* [한국어] 아래 6개 매크로는 Huawei Intelligent NIC 의 가상 함수(VF)를
 * 리셋할 때 쓰는 device ID, MMIO 오프셋, 비트, 타임아웃이다. 표준 PCI 가
 * 아니라 이 NIC 의 펌웨어 인터페이스 값이다. */
#define PCI_DEVICE_ID_HINIC_VF      0x375E	/* [한국어] Huawei Intelligent NIC 가상 함수의 device ID. */
#define HINIC_VF_FLR_TYPE           0x1000	/* [한국어] 펌웨어 능력 비트가 놓인 MMIO 오프셋. */
#define HINIC_VF_FLR_CAP_BIT        (1UL << 30)	/* [한국어] 그 값의 비트 30 - 펌웨어가 FLR 협조를 지원하는지 나타낸다. */
#define HINIC_VF_OP                 0xE80	/* [한국어] FLR 진행 상태를 주고받는 MMIO 오프셋. */
#define HINIC_VF_FLR_PROC_BIT       (1UL << 18)	/* [한국어] 그 값의 비트 18 - 소프트웨어가 세우고 펌웨어가 지우는 'FLR 진행 중' 표시. */
#define HINIC_OPERATION_TIMEOUT     15000	/* 15 seconds */

/* Device-specific reset method for Huawei Intelligent NIC virtual functions */
/*
 * [한국어]
 * reset_hinic_vf_dev - Huawei NIC 가상 함수를 펌웨어와 협조해 리셋한다
 *
 * @pdev: Huawei Intelligent NIC 의 가상 함수(VF)
 * @probe: true 면 이 방법을 쓸 수 있는지만 답한다
 * @return: 0 성공, -ENOTTY 면 BAR 매핑 실패이거나 펌웨어가 지원하지 않음
 *
 * [무엇이 문제] 이 NIC 의 VF 는 FLR 만으로는 온전히 리셋되지 않는다.
 * 펌웨어가 리셋을 인지하고 내부 상태를 정리해야 하므로, MMIO 레지스터로
 * 펌웨어에 '지금부터 FLR 을 건다' 고 알리고 완료를 기다려야 한다.
 * [무엇을 하는가] (1) 펌웨어가 이 협조 기능을 지원하는지 능력 비트를
 * 확인하고, (2) 진행 표시 비트를 세워 알리고, (3) FLR 을 걸고,
 * (4) 펌웨어가 그 비트를 지울 때까지 최대 15초 기다린다.
 *
 * [FLR 뒤 config 쓰기의 이유] 함수 안의 영어 주석대로, 장치는 FLR 후 자신의
 * 버스/디바이스 번호를 다시 파악해야 Completion 을 만들 수 있다. config
 * 쓰기를 한 번 보내면 그 트랜잭션의 주소에서 번호를 다시 잡는다. 쓰는
 * 대상이 PCI_VENDOR_ID(읽기 전용 필드)인 것은 값을 바꾸려는 것이 아니라
 * 트랜잭션 자체가 목적이기 때문이다.
 *
 * [빅엔디언 접근] ioread32be/iowrite32be 를 쓰는 것은 이 NIC 의 펌웨어
 * 인터페이스 레지스터가 빅엔디언이기 때문이다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥. msleep() 을 쓴다.
 *
 * 호출 체인:
 *   pci_dev_specific_reset() -> [reset_hinic_vf_dev] -> pcie_flr()
 */
static int reset_hinic_vf_dev(struct pci_dev *pdev, bool probe)
{
	unsigned long timeout;	/* [한국어] 펌웨어가 FLR 을 마치기를 기다릴 마감 시각. */
	void __iomem *bar;	/* [한국어] BAR0 에 매핑된 펌웨어 인터페이스 창. */
	u32 val;	/* [한국어] 레지스터 값을 담는 임시 변수. */

	if (probe)	/* [한국어] probe 호출이면 쓸 수 있다고만 답한다. */
		return 0;

	bar = pci_iomap(pdev, 0, 0);	/* [한국어] BAR0 전체를 매핑한다. */
	if (!bar)	/* [한국어] 매핑에 실패하면 이 방법을 쓸 수 없다. */
		return -ENOTTY;

	/* Get and check firmware capabilities */
	val = ioread32be(bar + HINIC_VF_FLR_TYPE);	/* [한국어] 위 영어 주석대로 펌웨어 능력 레지스터를 읽는다. 빅엔디언이라 be 판을 쓴다. */
	if (!(val & HINIC_VF_FLR_CAP_BIT)) {	/* [한국어] 비트 30 이 서 있어야 펌웨어가 FLR 협조를 지원한다. */
		pci_iounmap(pdev, bar);	/* [한국어] 지원하지 않으면 매핑을 풀고 */
		return -ENOTTY;	/* [한국어] 이 방법을 쓸 수 없다고 알린다. */
	}

	/* Set HINIC_VF_FLR_PROC_BIT for the start of FLR */
	val = ioread32be(bar + HINIC_VF_OP);	/* [한국어] 위 영어 주석대로 FLR 시작을 알리는 비트를 세우기 위해 현재 값을 읽는다. */
	val = val | HINIC_VF_FLR_PROC_BIT;	/* [한국어] 진행 표시 비트를 추가한다. */
	iowrite32be(val, bar + HINIC_VF_OP);	/* [한국어] 되써서 펌웨어에 알린다. */

	pcie_flr(pdev);	/* [한국어] 이제 표준 FLR 을 건다. */

	/*
	 * The device must recapture its Bus and Device Numbers after FLR
	 * in order generate Completions.  Issue a config write to let the
	 * device capture this information.
	 */
	pci_write_config_word(pdev, PCI_VENDOR_ID, 0);	/* [한국어] 위 영어 주석대로 장치가 버스/디바이스 번호를 다시 잡게 하는 config 쓰기다. 값 0 과 대상 필드에는 의미가 없고 트랜잭션이 발생하는 것 자체가 목적이다. */

	/* Firmware clears HINIC_VF_FLR_PROC_BIT when reset is complete */
	timeout = jiffies + msecs_to_jiffies(HINIC_OPERATION_TIMEOUT);	/* [한국어] 위 영어 주석대로 펌웨어가 진행 비트를 지우면 리셋이 끝난 것이다. 최대 15초를 기다린다. */
	do {	/* [한국어] 폴링 루프. */
		val = ioread32be(bar + HINIC_VF_OP);	/* [한국어] 진행 상태 레지스터를 읽는다. */
		if (!(val & HINIC_VF_FLR_PROC_BIT))	/* [한국어] 비트가 지워졌으면 펌웨어가 리셋을 마친 것이다. */
			goto reset_complete;	/* [한국어] 정리 지점으로 뛴다. */
		msleep(20);	/* [한국어] 20ms 쉬고 다시 확인한다. */
	} while (time_before(jiffies, timeout));	/* [한국어] 마감 시각을 넘지 않는 동안 반복한다. */

	val = ioread32be(bar + HINIC_VF_OP);	/* [한국어] 마감 직후 한 번 더 읽는다 - 마지막 msleep 사이에 완료되었을 수 있어 놓치지 않기 위해서다. */
	if (!(val & HINIC_VF_FLR_PROC_BIT))	/* [한국어] 그 사이에 완료되었다면 */
		goto reset_complete;	/* [한국어] 정상 경로로 간다. */

	pci_warn(pdev, "Reset dev timeout, FLR ack reg: %#010x\n", val);	/* [한국어] 정말로 시간 안에 끝나지 않았다. 레지스터 값과 함께 경고를 남긴다. */

reset_complete:	/* [한국어] 정상 완료와 타임아웃이 함께 지나는 지점. */
	pci_iounmap(pdev, bar);	/* [한국어] 임시 매핑을 해제한다. */

	return 0;	/* [한국어] 타임아웃이어도 성공으로 보고한다 - FLR 자체는 이미 걸렸기 때문이다. */
}

/* [한국어] 장치별 리셋 방법 표.
 * 설정자: 컴파일 시 고정된 const 배열.
 * 읽는 자: 아래 pci_dev_specific_reset() 이 위에서부터 훑어 첫 매칭을 쓴다.
 * 값 범위: {vendor, device, reset 함수} 세 쌍이며 마지막은 { 0 } 로 끝난다.
 * 왜 표인가: DECLARE_PCI_FIXUP_* 와 달리 리셋 방법은 '필요할 때 골라 쓰는'
 *   것이라 링커 섹션이 아니라 평범한 배열로 관리한다.
 * 순서가 중요한 이유: 위에서부터 첫 매칭에서 멈추므로, 넓은 매칭
 *   (PCI_ANY_ID)은 좁은 매칭보다 뒤에 와야 한다.
 * 동기화: const 이므로 경쟁이 없다. */
static const struct pci_dev_reset_methods pci_dev_reset_methods[] = {
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_82599_SFP_VF,	/* [한국어] Intel 82599 SFP 의 VF - VF DEVCAP 에 FLR 지원이 표시되지 않는 문제. */
		 reset_intel_82599_sfp_virtfn },
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IVB_M_VGA,	/* [한국어] Ivy Bridge 모바일 내장 그래픽 - 표준 FLR 이 없다. */
		reset_ivb_igd },
	{ PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_IVB_M2_VGA,	/* [한국어] 같은 세대의 두 번째 변종. */
		reset_ivb_igd },
	{ PCI_VENDOR_ID_SAMSUNG, 0xa804, nvme_disable_and_flr },	/* [한국어] ★ NVMe 접점 - 삼성 vendor 0x144d, device 0xa804(SM961/PM961). FLR 전에 컨트롤러를 재우는 방법을 쓴다. */
	{ PCI_VENDOR_ID_INTEL, 0x0953, delay_250ms_after_flr },	/* [한국어] ★ NVMe 접점 - Intel DC P3700 계열. FLR 후 250ms 대기. */
	{ PCI_VENDOR_ID_INTEL, 0x0a54, delay_250ms_after_flr },	/* [한국어] ★ NVMe 접점 - Intel 의 또 다른 NVMe 컨트롤러. */
	{ PCI_VENDOR_ID_SOLIDIGM, 0xf1ac, delay_250ms_after_flr },	/* [한국어] ★ NVMe 접점 - Solidigm P44 Pro. 같은 250ms 대기. */
	{ PCI_VENDOR_ID_CHELSIO, PCI_ANY_ID,	/* [한국어] Chelsio 벤더 전체 - T4 여부는 함수 안에서 device ID 로 가린다. */
		reset_chelsio_generic_dev },
	{ PCI_VENDOR_ID_HUAWEI, PCI_DEVICE_ID_HINIC_VF,	/* [한국어] Huawei Intelligent NIC 의 VF - 펌웨어 협조가 필요하다. */
		reset_hinic_vf_dev },
	{ 0 }	/* [한국어] 표의 끝. reset 필드가 NULL 이라 아래 루프의 종료 조건이 된다. */
};

/*
 * [한국어]
 * __pci_dev_specific_reset - IOMMU 를 잠시 멈추고 장치별 리셋 콜백을 실행한다
 *
 * @dev: 리셋할 장치
 * @probe: true 면 콜백에 '가능 여부만 답하라' 고 전달한다
 * @i: 위 표에서 골라 낸 항목(그 안의 reset 함수 포인터를 쓴다)
 * @return: 콜백의 반환값, 또는 IOMMU 정지 실패 시 그 오류값
 *
 * [왜 IOMMU 를 멈추는가] 리셋은 장치의 요청자 ID(RID) 관련 상태와 진행 중인
 * DMA 를 뒤흔든다. IOMMU 가 그 장치에 대한 변환을 유지한 채로 리셋이
 * 일어나면 변환 오류나 미해결 페이지 요청이 남을 수 있다. 그래서
 * pci_dev_reset_iommu_prepare() 로 잠시 준비 상태를 만들고, 리셋이 끝나면
 * pci_dev_reset_iommu_done() 으로 되돌린다.
 *
 * [실패 시] IOMMU 를 멈추지 못하면 리셋을 시도하지 않고 오류를 돌려준다 -
 * 억지로 리셋하면 IOMMU 상태가 깨지기 때문이다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_dev_specific_reset() -> [__pci_dev_specific_reset]
 *     -> pci_dev_reset_iommu_prepare() -> i->reset() -> pci_dev_reset_iommu_done()
 */
static int __pci_dev_specific_reset(struct pci_dev *dev, bool probe,
				    const struct pci_dev_reset_methods *i)
{
	int ret;	/* [한국어] 콜백과 IOMMU 준비 함수의 반환값. */

	ret = pci_dev_reset_iommu_prepare(dev);	/* [한국어] IOMMU 를 리셋에 대비한 상태로 만든다. */
	if (ret) {	/* [한국어] 실패하면 리셋을 시도하지 않는다. */
		pci_err(dev, "failed to stop IOMMU for a PCI reset: %d\n", ret);	/* [한국어] 왜 못 했는지 오류 수준으로 남긴다. */
		return ret;
	}

	ret = i->reset(dev, probe);	/* [한국어] 표에서 고른 장치별 리셋 함수를 부른다. probe 값이 그대로 전달된다. */
	pci_dev_reset_iommu_done(dev);	/* [한국어] 성공이든 실패든 IOMMU 를 원래 상태로 되돌린다. */
	return ret;	/* [한국어] 콜백의 결과를 그대로 돌려준다. */
}

/*
 * These device-specific reset methods are here rather than in a driver
 * because when a host assigns a device to a guest VM, the host may need
 * to reset the device but probably doesn't have a driver for it.
 */
/*
 * [한국어]
 * pci_dev_specific_reset - 이 장치에 맞는 장치별 리셋 방법을 찾아 실행한다
 *
 * @dev: 리셋할 장치
 * @probe: true 면 '이 방법을 쓸 수 있는가' 만 확인한다
 * @return: 리셋 결과(0 성공), 또는 표에 맞는 항목이 없으면 -ENOTTY
 *
 * [왜 드라이버가 아니라 여기 있는가] 위 영어 주석이 그 이유를 밝힌다.
 * 호스트가 장치를 게스트 VM 에 할당할 때 호스트는 그 장치를 리셋해야
 * 하지만 그 장치의 드라이버를 갖고 있지 않을 가능성이 높다. 그래서 리셋
 * 방법이 드라이버가 아니라 PCI 코어 쪽에 있어야 한다.
 *
 * [★ NVMe 와의 관계] 위 표에 NVMe 항목이 넷 있다. 삼성 144d:a804 는
 * nvme_disable_and_flr() 이, Intel 0x0953/0x0a54 와 Solidigm 0xf1ac 는
 * delay_250ms_after_flr() 이 선택된다. 다만 drivers/nvme/host/pci.c 에는
 * pci_reset_function() 계열 호출이 없으므로, NVMe 드라이버가 이 경로를
 * 직접 타지는 않는다. 이 경로는 PCI 코어의 리셋 기능(sysfs 의 reset,
 * VFIO 장치 할당 등)에서 들어온다.
 *
 * 실행 컨텍스트: PCI 리셋 경로, 프로세스 문맥. drivers/pci/pci.h 에
 * 선언되어 PCI 코어 안에서만 쓰인다.
 *
 * 호출 체인:
 *   PCI 코어의 리셋 경로 -> [pci_dev_specific_reset]
 *     -> __pci_dev_specific_reset() -> 표의 reset 함수
 */
int pci_dev_specific_reset(struct pci_dev *dev, bool probe)
{
	const struct pci_dev_reset_methods *i;	/* [한국어] 표를 훑는 반복자. */

	for (i = pci_dev_reset_methods; i->reset; i++) {	/* [한국어] 표의 끝(reset 이 NULL)까지 위에서부터 훑는다. 첫 매칭에서 바로 반환하므로 순서가 곧 우선순위다. */
		if ((i->vendor == dev->vendor ||	/* [한국어] 벤더가 일치하거나 */
		     i->vendor == (u16)PCI_ANY_ID) &&	/* [한국어] 벤더 와일드카드이고, */
		    (i->device == dev->device ||	/* [한국어] 디바이스가 일치하거나 */
		     i->device == (u16)PCI_ANY_ID))	/* [한국어] 디바이스 와일드카드이면 이 항목이 이 장치에 해당한다. */
			return __pci_dev_specific_reset(dev, probe, i);	/* [한국어] IOMMU 를 다루는 껍데기를 거쳐 실제 리셋 함수를 부른다. */
	}

	return -ENOTTY;	/* [한국어] 표에 없는 장치다. -ENOTTY 는 '장치별 리셋 방법이 없다' 는 뜻이며, 호출자는 표준 방법(FLR, PM 리셋, 버스 리셋)으로 넘어간다. */
}

/*
 * [한국어]
 * quirk_dma_func0_alias - DMA 요청자 ID 로 함수 0 을 쓰는 장치의 별칭을 등록
 *
 * @dev: 아래 등록표의 장치들 중 함수 번호가 0 이 아닌 것
 * @return: 없음
 *
 * [요청자 ID(RID)란] PCIe 트랜잭션에는 그것을 보낸 장치의 버스:디바이스:함수
 * 번호가 실린다. IOMMU 는 이 값으로 '누가 보낸 DMA 인가' 를 판별해 그 장치의
 * 주소 변환 표를 고른다.
 * [무엇이 문제] 아래 영어 주석대로 일부 장치는 자신의 실제 함수 번호가
 * 아니라 함수 0 의 번호를 요청자 ID 로 써서 DMA 를 보낸다. 명백한 스펙
 * 위반이다.
 * [그대로 두면] IOMMU 가 그 DMA 를 함수 0 의 것으로 보고 엉뚱한 변환 표를
 * 적용한다. 결과는 DMA 실패나 잘못된 메모리 접근이다.
 * [우회] pci_add_dma_alias() 로 '이 장치는 함수 0 의 ID 로도 DMA 를 보낸다'
 * 고 등록한다. 그러면 IOMMU 계층이 두 ID 를 같은 그룹으로 묶어 다룬다.
 *
 * [함수 0 자신은 제외] 함수 0 은 원래 자기 번호를 쓰므로 별칭이 필요 없다.
 *
 * 실행 컨텍스트: HEADER 단계. IOMMU 그룹이 만들어지기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_dma_func0_alias] -> pci_add_dma_alias()
 */
static void quirk_dma_func0_alias(struct pci_dev *dev)
{
	if (PCI_FUNC(dev->devfn) != 0)	/* [한국어] 함수 0 자신은 별칭이 필요 없다. */
		pci_add_dma_alias(dev, PCI_DEVFN(PCI_SLOT(dev->devfn), 0), 1);	/* [한국어] 같은 슬롯의 함수 0 을 별칭으로 등록한다. PCI_DEVFN(slot, 0) 이 그 devfn 이고, 마지막 인자 1 은 별칭 개수다. */
}

/*
 * https://bugzilla.redhat.com/show_bug.cgi?id=605888
 *
 * Some Ricoh devices use function 0 as the PCIe requester ID for DMA.
 */
/* [한국어] 위 영어 주석과 버그질라 링크대로, 일부 Ricoh 장치가 DMA 의
 * 요청자 ID 로 함수 0 을 쓴다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_RICOH, 0xe832, quirk_dma_func0_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_RICOH, 0xe476, quirk_dma_func0_alias);

/* [한국어] 옆의 영어 주석대로 일부 Glenfly 칩도 같은 문제가 있다. */
/* Some Glenfly chips use function 0 as the PCIe Requester ID for DMA */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_GLENFLY, 0x3d40, quirk_dma_func0_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_GLENFLY, 0x3d41, quirk_dma_func0_alias);

/*
 * [한국어]
 * quirk_dma_func1_alias - DMA 요청자 ID 로 함수 1 을 쓰는 장치의 별칭을 등록
 *
 * @dev: 아래 등록표의 장치들 중 함수 번호가 1 이 아닌 것
 * @return: 없음
 *
 * [무엇이 문제] quirk_dma_func0_alias() 와 같은 부류지만 대상이 함수 1 이다.
 * 아래 Marvell 항목의 영어 주석이 대표 사례를 설명한다. 88SE9123 은 DMA 의
 * 요청자 ID 로 함수 1 을 쓴다. 그런데 SKU 에 따라 함수 1 이 레거시 IDE
 * 컨트롤러로 실제 존재하기도 하고, 아예 존재하지 않기도 한다. 후자의 경우
 * 존재하지 않는 함수의 ID 로 DMA 가 올라오는 셈이라 영어 주석은 이를
 * '유령 요청자(ghost requester)' 라고 부른다.
 * [그대로 두면] IOMMU 가 그 DMA 를 알 수 없는 장치의 것으로 보고 막는다.
 * [우회] 같은 슬롯의 함수 1 을 DMA 별칭으로 등록해 IOMMU 가 같은 그룹으로
 * 다루게 한다.
 *
 * [등록표의 버그질라 링크] 아래 각 항목에 달린 링크는 그 device ID 가
 * 실제로 이 증상을 보인다고 보고된 근거다. 대부분 같은 버그(42679)의
 * 서로 다른 댓글 번호로, 사용자들이 하나씩 확인해 추가해 온 목록이다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_dma_func1_alias] -> pci_add_dma_alias()
 */
static void quirk_dma_func1_alias(struct pci_dev *dev)
{
	if (PCI_FUNC(dev->devfn) != 1)	/* [한국어] 함수 1 자신은 별칭이 필요 없다. */
		pci_add_dma_alias(dev, PCI_DEVFN(PCI_SLOT(dev->devfn), 1), 1);	/* [한국어] 같은 슬롯의 함수 1 을 DMA 별칭으로 등록한다. */
}

/*
 * Marvell 88SE9123 uses function 1 as the requester ID for DMA.  In some
 * SKUs function 1 is present and is a legacy IDE controller, in other
 * SKUs this function is not present, making this a ghost requester.
 * https://bugzilla.kernel.org/show_bug.cgi?id=42679
 */
/* [한국어] 위 영어 주석이 설명하는 Marvell 88SE912x 계열. 아래 목록은
 * 같은 증상이 확인된 device ID 들이며, 각 줄의 링크가 그 근거다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9120,
			 quirk_dma_func1_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9123,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c136 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9125,
			 quirk_dma_func1_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9128,
			 quirk_dma_func1_alias);
/* [한국어] 이어지는 등록 줄: 0x9130. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c14 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9130,
			 quirk_dma_func1_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9170,
			 quirk_dma_func1_alias);
/* [한국어] 이어지는 등록 줄: 0x9172. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c47 + c57 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9172,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c59 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x917a,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c78 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9182,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c134 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9183,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c46 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x91a0,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c135 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9215,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c127 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9220,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c49 */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9230,
			 quirk_dma_func1_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_MARVELL_EXT, 0x9235,
			 quirk_dma_func1_alias);
/* [한국어] TTI(Highpoint) 컨트롤러 2종도 같은 증상을 보인다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_TTI, 0x0642,
			 quirk_dma_func1_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_TTI, 0x0645,
			 quirk_dma_func1_alias);
/* https://bugs.gentoo.org/show_bug.cgi?id=497630 */
/* [한국어] 옆의 링크대로 JMicron JMB388 ESD 컨트롤러도 같은 증상이다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_JMICRON,
			 PCI_DEVICE_ID_JMICRON_JMB388_ESD,
			 quirk_dma_func1_alias);
/* https://bugzilla.kernel.org/show_bug.cgi?id=42679#c117 */
DECLARE_PCI_FIXUP_HEADER(0x1c28, /* Lite-On */
			 0x0122, /* Plextor M6E (Marvell 88SS9183)*/
			 quirk_dma_func1_alias);

/* [한국어] 고정 DMA 별칭 표.
 * 설정자: 컴파일 시 고정된 const 배열.
 * 읽는 자: 아래 quirk_fixed_dma_alias() 가 pci_match_id() 로 조회한다.
 * 값 범위: 각 항목의 driver_data 에 별칭으로 쓸 devfn 이 들어 있다.
 * 왜 '고정' 인가: 위 func0/func1 별칭은 '같은 슬롯의 다른 함수' 라는
 *   상대적 규칙이었지만, 여기 항목은 장치의 devfn 과 무관하게 정해진
 *   devfn 을 별칭으로 쓴다. 아래 영어 주석의 Adaptec 3405 예가 그 이유를
 *   설명한다 - 소프트웨어에 보이는 0e.0 장치는 사실 I/O 프로세서의 ATU
 *   이고, 실제로 DMA 를 보내는 것은 config space 에서 숨겨진 01.0 의
 *   DMA 엔진이다. 숨겨진 장치이므로 상대 규칙으로는 표현할 수 없다.
 * 동기화: const 이므로 경쟁이 없다. */
/*
 * Some devices DMA with the wrong devfn, not just the wrong function.
 * quirk_fixed_dma_alias() uses this table to create fixed aliases, where
 * the alias is "fixed" and independent of the device devfn.
 *
 * For example, the Adaptec 3405 is a PCIe card with an Intel 80333 I/O
 * processor.  To software, this appears as a PCIe-to-PCI/X bridge with a
 * single device on the secondary bus.  In reality, the single exposed
 * device at 0e.0 is the Address Translation Unit (ATU) of the controller
 * that provides a bridge to the internal bus of the I/O processor.  The
 * controller supports private devices, which can be hidden from PCI config
 * space.  In the case of the Adaptec 3405, a private device at 01.0
 * appears to be the DMA engine, which therefore needs to become a DMA
 * alias for the device.
 */
static const struct pci_device_id fixed_dma_alias_tbl[] = {
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_ADAPTEC2, 0x0285,	/* [한국어] Adaptec 3405 - 서브시스템 ID 까지 지정해 정확히 이 모델만 고른다. */
			 PCI_VENDOR_ID_ADAPTEC2, 0x02bb), /* Adaptec 3405 */
	  .driver_data = PCI_DEVFN(1, 0) },	/* [한국어] 별칭으로 쓸 devfn: 디바이스 1, 함수 0(숨겨진 DMA 엔진). */
	{ PCI_DEVICE_SUB(PCI_VENDOR_ID_ADAPTEC2, 0x0285,	/* [한국어] Adaptec 3805 - 같은 구조의 다른 모델. */
			 PCI_VENDOR_ID_ADAPTEC2, 0x02bc), /* Adaptec 3805 */
	  .driver_data = PCI_DEVFN(1, 0) },	/* [한국어] 같은 별칭 devfn. */
	{ 0 }	/* [한국어] 표의 끝을 나타내는 항목. */
};

/*
 * [한국어]
 * quirk_fixed_dma_alias - 표에서 고정 별칭 devfn 을 찾아 등록한다
 *
 * @dev: Adaptec2 0x0285 로 매칭된 장치
 * @return: 없음
 *
 * DECLARE 매크로는 서브시스템 ID 로 매칭할 수 없으므로, device ID 로 넓게
 * 잡고 함수 안에서 pci_match_id() 로 서브시스템까지 확인해 좁힌다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_fixed_dma_alias]
 *     -> pci_match_id() -> pci_add_dma_alias()
 */
static void quirk_fixed_dma_alias(struct pci_dev *dev)
{
	const struct pci_device_id *id;	/* [한국어] 표에서 찾은 항목. */

	id = pci_match_id(fixed_dma_alias_tbl, dev);	/* [한국어] 서브시스템 ID 까지 포함해 표를 조회한다. */
	if (id)	/* [한국어] 표에 있는 모델일 때만 별칭을 등록한다. */
		pci_add_dma_alias(dev, id->driver_data, 1);	/* [한국어] driver_data 에 담아 둔 devfn 을 별칭으로 쓴다. */
}
/* [한국어] Adaptec2 0x0285 에 HEADER 단계로 등록한다. 정확한 모델 판별은
 * 함수 안에서 서브시스템 ID 로 한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ADAPTEC2, 0x0285, quirk_fixed_dma_alias);

/*
 * [한국어]
 * quirk_use_pcie_bridge_dma_alias - PCIe capability 를 감춘 PCIe-to-PCI 브리지를 표시
 *
 * @pdev: 아래 등록표의 브리지들
 * @return: 없음
 *
 * [DMA 별칭과 브리지] PCI(레거시) 버스에 달린 장치의 DMA 는 그 위의
 * PCIe-to-PCI 브리지가 자신의 요청자 ID 로 바꿔 내보낸다. 그래서 IOMMU 는
 * 브리지를 기준으로 그 아래 장치들을 한 그룹으로 다뤄야 한다. 커널은
 * 브리지가 PCIe capability 를 갖고 있고 그 타입이 PCI 브리지인지를 보고
 * 이를 판별한다.
 * [무엇이 문제] 아래 영어 주석대로, 몇몇 PCIe-to-PCI 브리지는 PCIe
 * capability 를 아예 노출하지 않는다. 그러면 커널이 이 브리지를 평범한
 * PCI-to-PCI 브리지로 오인해 잘못된 DMA 별칭을 쓴다.
 * [그대로 두면] IOMMU 그룹 판정이 어긋나 DMA 가 막히거나 격리가 깨진다.
 * [우회] 다음 네 조건을 모두 만족하면 사실상 PCIe-to-PCI 브리지라고 보고
 * PCI_DEV_FLAG_PCIE_BRIDGE_ALIAS 를 세운다.
 *   (1) 루트 버스가 아니다(위에 부모가 있다),
 *   (2) 헤더 타입이 브리지다,
 *   (3) 자신은 PCIe capability 가 없다,
 *   (4) 부모는 PCIe 이고 그 타입이 PCIe-to-PCI 브리지가 아니다.
 * 조건 (4)가 핵심이다 - 부모가 이미 PCIe-to-PCI 브리지라면 이 장치는 그
 * 아래의 평범한 PCI 브리지일 뿐이다.
 *
 * [정방향/역방향 모드] 영어 주석대로 이 칩들 중 일부는 forward 와 reverse
 * 양쪽으로 쓸 수 있어, 지금 어느 모드로 동작하는지 검사해야 한다. 위 네
 * 조건이 곧 그 검사다. 원저자는 PCI_ANY_ID 로 넓혀도 될 것 같지만 지금은
 * 알려진 문제 장치에만 적용한다고 밝힌다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_use_pcie_bridge_dma_alias]
 */
/*
 * A few PCIe-to-PCI bridges fail to expose a PCIe capability, resulting in
 * using the wrong DMA alias for the device.  Some of these devices can be
 * used as either forward or reverse bridges, so we need to test whether the
 * device is operating in the correct mode.  We could probably apply this
 * quirk to PCI_ANY_ID, but for now we'll just use known offenders.  The test
 * is for a non-root, non-PCIe bridge where the upstream device is PCIe and
 * is not a PCIe-to-PCI bridge, then @pdev is actually a PCIe-to-PCI bridge.
 */
static void quirk_use_pcie_bridge_dma_alias(struct pci_dev *pdev)
{
	if (!pci_is_root_bus(pdev->bus) &&	/* [한국어] 조건 (1) - 루트 버스에 직접 붙은 장치는 위에 부모가 없어 판정할 수 없다. */
	    pdev->hdr_type == PCI_HEADER_TYPE_BRIDGE &&	/* [한국어] 조건 (2) - 헤더 타입 1(PCI-to-PCI 브리지)이어야 한다. */
	    !pci_is_pcie(pdev) && pci_is_pcie(pdev->bus->self) &&	/* [한국어] 조건 (3) 자신은 PCIe capability 가 없고, 조건 (4) 앞부분 - 부모는 PCIe 여야 한다. */
	    pci_pcie_type(pdev->bus->self) != PCI_EXP_TYPE_PCI_BRIDGE)	/* [한국어] 조건 (4) 뒷부분 - 부모가 이미 PCIe-to-PCI 브리지면 이 장치는 그 아래의 평범한 PCI 브리지다. */
		pdev->dev_flags |= PCI_DEV_FLAG_PCIE_BRIDGE_ALIAS;	/* [한국어] 네 조건을 모두 만족하면 이 장치를 PCIe 브리지 별칭 대상으로 표시한다. */
}
/* ASM1083/1085, https://bugzilla.kernel.org/show_bug.cgi?id=44881#c46 */
/* [한국어] 옆의 영어 주석과 링크대로 ASMedia ASM1083/1085. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ASMEDIA, 0x1080,
			 quirk_use_pcie_bridge_dma_alias);
/* Tundra 8113, https://bugzilla.kernel.org/show_bug.cgi?id=44881#c43 */
/* [한국어] 옆의 영어 주석과 링크대로 Tundra 8113. */
DECLARE_PCI_FIXUP_HEADER(0x10e3, 0x8113, quirk_use_pcie_bridge_dma_alias);
/* ITE 8892, https://bugzilla.kernel.org/show_bug.cgi?id=73551 */
/* [한국어] 옆의 영어 주석과 링크대로 ITE 8892. */
DECLARE_PCI_FIXUP_HEADER(0x1283, 0x8892, quirk_use_pcie_bridge_dma_alias);
/* ITE 8893 has the same problem as the 8892 */
/* [한국어] 옆의 영어 주석대로 ITE 8893 도 8892 와 같은 문제가 있다. */
DECLARE_PCI_FIXUP_HEADER(0x1283, 0x8893, quirk_use_pcie_bridge_dma_alias);
/* Intel 82801, https://bugzilla.kernel.org/show_bug.cgi?id=44881#c49 */
/* [한국어] 옆의 영어 주석과 링크대로 Intel 82801 의 브리지 함수. */
DECLARE_PCI_FIXUP_HEADER(0x8086, 0x244e, quirk_use_pcie_bridge_dma_alias);

/*
 * [한국어]
 * quirk_mic_x200_dma_alias - MIC x200 NTB 가 쓰는 외래 요청자 ID 셋을 별칭으로 등록
 *
 * @pdev: Intel MIC x200 NTB(0x2260, 0x2264)
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, 이 NTB 는 PCIe 트래픽을 전달할 때
 * 자신의 것이 아닌 여러 요청자 ID(alien RID)를 쓴다. NTB 는 반대편 시스템의
 * 트랜잭션을 이쪽 버스로 내보내는 장치이므로, 그 트래픽에 붙는 ID 가
 * 이쪽에서 보면 낯선 값이 되는 것이다.
 * [그대로 두면] IOMMU 가 켜져 있을 때 그 ID 로 오는 DMA 를 막아 버퍼
 * 접근이 실패한다.
 * [우회] 그 세 ID 를 모두 DMA 별칭으로 등록한다.
 *
 * [값의 출처] 영어 주석대로 이 devfn 들은 EEPROM 에 프로그래밍된 RIT-LUT
 * 표와 일치해야 한다. 즉 하드웨어 설정에서 온 고정값이며, 이 트리에서는
 * 그 표를 확인할 수 없다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_mic_x200_dma_alias] -> pci_add_dma_alias()
 */
/*
 * MIC x200 NTB forwards PCIe traffic using multiple alien RIDs. They have to
 * be added as aliases to the DMA device in order to allow buffer access
 * when IOMMU is enabled. Following devfns have to match RIT-LUT table
 * programmed in the EEPROM.
 */
static void quirk_mic_x200_dma_alias(struct pci_dev *pdev)
{
	pci_add_dma_alias(pdev, PCI_DEVFN(0x10, 0x0), 1);	/* [한국어] 디바이스 0x10, 함수 0 을 별칭으로 등록한다. */
	pci_add_dma_alias(pdev, PCI_DEVFN(0x11, 0x0), 1);	/* [한국어] 디바이스 0x11, 함수 0. */
	pci_add_dma_alias(pdev, PCI_DEVFN(0x12, 0x3), 1);	/* [한국어] 디바이스 0x12, 함수 3 - 세 값 모두 EEPROM 의 RIT-LUT 표에서 온 것이다. */
}
/* [한국어] MIC x200 NTB 2종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2260, quirk_mic_x200_dma_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2264, quirk_mic_x200_dma_alias);

/*
 * [한국어]
 * quirk_pex_vca_alias - VCA 카드의 모든 슬롯을 DMA 별칭으로 등록해 한 IOMMU 그룹으로 묶는다
 *
 * @pdev: Intel VCA(Visual Compute Accelerator) 장치
 * @return: 없음
 *
 * [어떤 하드웨어] 아래 영어 주석대로 Intel VCA - NTB(PEX 87xx)를 통해
 * 연산 유닛을 노출하는 PCIe 애드인 카드 계열이다.
 * [무엇이 문제] MIC x200 과 같은 부류다. 카드 반대편의 연산 유닛이 호스트
 * 메모리에 접근할 때 이쪽에서 보면 낯선 요청자 ID 로 DMA 가 올라온다.
 * [그대로 두면] IOMMU 가 켜져 있으면 그 접근이 막힌다.
 * [우회] 가능한 모든 슬롯 번호(0x20 = 32개)를 별칭으로 등록해 VCA 장치
 * 전체를 하나의 IOMMU 그룹으로 만든다.
 *
 * [왜 전부 거는가] 영어 주석이 그 이유를 밝힌다. 반대편에서 어느 슬롯이
 * 쓰이는지 알 수 없기 때문이다. 이 quirk 는 호스트 쪽과 연산 유닛 쪽
 * 양쪽에서 쓰이도록 만들어졌고, VCA 장치는 함수가 최대 다섯 개다
 * (DMA 채널 넷 + 추가 하나). 그래서 슬롯마다 함수 5개씩을 별칭으로 건다.
 *
 * [격리를 포기하는 대가] 32개 슬롯을 모두 별칭으로 걸면 사실상 그 버스
 * 전체가 한 IOMMU 그룹이 된다. 세밀한 격리를 포기하고 동작을 택한 것이다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_pex_vca_alias] -> pci_add_dma_alias()
 */
/*
 * Intel Visual Compute Accelerator (VCA) is a family of PCIe add-in devices
 * exposing computational units via Non Transparent Bridges (NTB, PEX 87xx).
 *
 * Similarly to MIC x200, we need to add DMA aliases to allow buffer access
 * when IOMMU is enabled.  These aliases allow computational unit access to
 * host memory.  These aliases mark the whole VCA device as one IOMMU
 * group.
 *
 * All possible slot numbers (0x20) are used, since we are unable to tell
 * what slot is used on other side.  This quirk is intended for both host
 * and computational unit sides.  The VCA devices have up to five functions
 * (four for DMA channels and one additional).
 */
static void quirk_pex_vca_alias(struct pci_dev *pdev)
{
	const unsigned int num_pci_slots = 0x20;	/* [한국어] PCI 버스 하나에 들어갈 수 있는 슬롯(디바이스) 개수 32. */
	unsigned int slot;	/* [한국어] 슬롯 번호 반복자. */

	for (slot = 0; slot < num_pci_slots; slot++)	/* [한국어] 모든 슬롯 번호를 훑는다. */
		pci_add_dma_alias(pdev, PCI_DEVFN(slot, 0x0), 5);	/* [한국어] 각 슬롯의 함수 0 부터 5개를 별칭으로 등록한다. 마지막 인자 5 가 그 개수이며, VCA 의 최대 함수 수와 같다. */
}
/* [한국어] VCA 계열 6종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2954, quirk_pex_vca_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2955, quirk_pex_vca_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2956, quirk_pex_vca_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2958, quirk_pex_vca_alias);
/* [한국어] 이어지는 등록 줄: 0x2959, 0x295A. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x2959, quirk_pex_vca_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x295A, quirk_pex_vca_alias);

/*
 * [한국어]
 * quirk_bridge_cavm_thrx2_pcie_root - IOMMU 기준점이 루트가 아닌 브리지임을 표시
 *
 * @pdev: Broadcom Vulcan / Cavium ThunderX2 의 브리지(0x9000, 0x9084)
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, 이 SoC 에서는 IOMMU 와 인터럽트
 * 컨트롤러가 루트 버스가 아니라 그 아래의 브리지에 연결되어 있다. 커널은
 * 보통 루트 버스를 기준으로 DMA 별칭 사슬을 계산하는데, 그러면 이
 * 플랫폼에서는 존재하지 않는 별칭이 만들어진다.
 * [그대로 두면] 잘못된 DMA 별칭 때문에 IOMMU 매핑이 어긋난다.
 * [우회] PCI_DEV_FLAGS_BRIDGE_XLATE_ROOT 를 세워 '주소 변환의 기준점이
 * 여기다' 라고 알린다. 그러면 별칭 계산이 이 브리지에서 멈춘다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_bridge_cavm_thrx2_pcie_root]
 */
/*
 * The IOMMU and interrupt controller on Broadcom Vulcan/Cavium ThunderX2 are
 * associated not at the root bus, but at a bridge below. This quirk avoids
 * generating invalid DMA aliases.
 */
static void quirk_bridge_cavm_thrx2_pcie_root(struct pci_dev *pdev)
{
	pdev->dev_flags |= PCI_DEV_FLAGS_BRIDGE_XLATE_ROOT;	/* [한국어] 이 브리지가 주소 변환의 기준점임을 표시한다. */
}
/* [한국어] 해당 브리지 2종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_BROADCOM, 0x9000,
				quirk_bridge_cavm_thrx2_pcie_root);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_BROADCOM, 0x9084,
				quirk_bridge_cavm_thrx2_pcie_root);

/*
 * [한국어]
 * quirk_aspeed_pci_bridge_no_alias - 요청자 ID 를 바꾸지 않는 가짜 브리지를 표시
 *
 * @pdev: ASPEED AST1150 (0x1150)
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, AST1150 은 실제 PCI 버스를 쓰지 않고
 * 하위 장치의 요청자 ID 를 그대로 전달한다. 진짜 PCI 브리지라면 하위
 * 장치의 DMA 를 자신의 ID 로 바꿔 내보내야 하는데 그러지 않는 것이다.
 * [그대로 두면] 커널이 이 브리지의 ID 를 DMA 별칭으로 잡는데, 실제로는
 * 하위 장치의 ID 가 그대로 올라오므로 별칭이 틀린다.
 * [우회] PCI_DEV_FLAGS_PCI_BRIDGE_NO_ALIAS 를 세워 이 브리지에서는 별칭을
 * 만들지 않게 한다. 위 quirk 들이 별칭을 '추가' 했다면 이것은 '빼는' 쪽이다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_aspeed_pci_bridge_no_alias]
 */
/*
 * AST1150 doesn't use a real PCI bus and always forwards the requester ID
 * from downstream devices.
 */
static void quirk_aspeed_pci_bridge_no_alias(struct pci_dev *pdev)
{
	pdev->dev_flags |= PCI_DEV_FLAGS_PCI_BRIDGE_NO_ALIAS;	/* [한국어] 이 브리지에서는 DMA 별칭을 만들지 말라는 표시. */
}
/* [한국어] ASPEED AST1150 에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_ASPEED, 0x1150, quirk_aspeed_pci_bridge_no_alias);

/*
 * [한국어]
 * quirk_tw686x_class - 클래스 코드가 비어 있는 TW686x 캡처 카드를 분류해 준다
 *
 * @pdev: Intersil/Techwell TW6864/6865/6868/6869 캡처 카드
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로 이 카드들은 클래스 코드가 0(비어 있음)
 * 이다. 클래스 코드는 표준 헤더의 필수 필드이므로 스펙 위반이다.
 * [그대로 두면] 커널이 이 장치를 분류하지 못한다.
 * [우회] 클래스를 '멀티미디어 컨트롤러(기타)' 로 채운다. ProgIf 자리에
 * 0x01 을 함께 넣는 것은 이 카드의 프로그래밍 인터페이스 값이다.
 *
 * [등록 방식] 클래스가 PCI_CLASS_NOT_DEFINED 인 것만 매칭하므로, 이미
 * 클래스가 채워진 변종은 건드리지 않는다.
 *
 * 실행 컨텍스트: EARLY 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_tw686x_class]
 */
/*
 * Intersil/Techwell TW686[4589]-based video capture cards have an empty (zero)
 * class code.  Fix it.
 */
static void quirk_tw686x_class(struct pci_dev *pdev)
{
	u32 class = pdev->class;	/* [한국어] 원래 클래스 값을 보관한다 - 로그용이다. */

	/* Use "Multimedia controller" class */
	pdev->class = (PCI_CLASS_MULTIMEDIA_OTHER << 8) | 0x01;	/* [한국어] 옆의 영어 주석대로 멀티미디어 컨트롤러 클래스로 채운다. << 8 로 상위 16비트에 놓고, 하위 8비트(ProgIf)에 0x01 을 넣는다. */
	pci_info(pdev, "TW686x PCI class overridden (%#08x -> %#08x)\n",	/* [한국어] 무엇이 무엇으로 바뀌었는지 남긴다. */
		 class, pdev->class);
}
/* [한국어] TW686x 4종 중 클래스가 정의되지 않은 것만 EARLY 단계에서 잡는다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(0x1797, 0x6864, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_tw686x_class);
DECLARE_PCI_FIXUP_CLASS_EARLY(0x1797, 0x6865, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_tw686x_class);
DECLARE_PCI_FIXUP_CLASS_EARLY(0x1797, 0x6868, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_tw686x_class);
/* [한국어] 이어지는 등록 줄: 0x6869. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(0x1797, 0x6869, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_tw686x_class);

/*
 * [한국어]
 * quirk_relaxedordering_disable - Relaxed Ordering 을 쓰면 안 되는 경로를 표시
 *
 * @dev: 아래 등록표의 Intel Xeon 루트 콤플렉스와 AMD SEATTLE SoC
 * @return: 없음
 *
 * [Relaxed Ordering(RO)이란] PCIe TLP 헤더의 속성 비트로, '이 트랜잭션은
 * 앞선 것들을 추월해도 된다' 는 뜻이다. 순서 제약을 풀면 성능이 오르므로
 * 대역폭이 중요한 장치가 즐겨 쓴다.
 * [무엇이 문제] 아래 두 등록표의 영어 주석이 각각의 사정을 밝힌다.
 *   - Broadwell/Haswell 기반 Xeon 루트 콤플렉스: Flow Control Credit 문제로,
 *     RO 가 설정된 업스트림 TLP 에서 성능 문제가 생긴다.
 *   - AMD ARM A1100(SEATTLE) SoC: RO 가 꺼진 업스트림 TLP 가 RO 가 켜진
 *     앞선 TLP 를 추월할 수 있다. 이것은 PCIe 3.0 스펙 2.4.1 절의 트랜잭션
 *     순서 규칙 위반이다. 그래서 이 플랫폼에서는 업스트림 TLP 에 RO 를
 *     쓸 수 없다.
 * [그대로 두면] 앞의 경우 성능이 나빠지고, 뒤의 경우 데이터 순서가 깨진다.
 * [우회] PCI_DEV_FLAGS_NO_RELAXED_ORDERING 을 세운다. 아래 영어 주석대로
 * 이런 장치는 스스로 표시하고, 다른 드라이버가 RO 를 켠 TLP 를 보내기
 * 전에 이 표시를 확인해야 한다.
 *
 * [루트 콤플렉스에 걸리는 이유] 문제가 있는 것은 개별 엔드포인트가 아니라
 * 그것들이 지나가는 루트 콤플렉스다. 그래서 quirk 는 루트 쪽에 걸리고,
 * 엔드포인트 드라이버가 자기 위쪽 경로를 거슬러 올라가며 이 플래그를 본다.
 *
 * 실행 컨텍스트: EARLY 단계, 클래스가 정의되지 않은 장치로 좁혀 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_relaxedordering_disable]
 */
/*
 * Some devices have problems with Transaction Layer Packets with the Relaxed
 * Ordering Attribute set.  Such devices should mark themselves and other
 * device drivers should check before sending TLPs with RO set.
 */
static void quirk_relaxedordering_disable(struct pci_dev *dev)
{
	dev->dev_flags |= PCI_DEV_FLAGS_NO_RELAXED_ORDERING;	/* [한국어] 이 경로에서 Relaxed Ordering 을 쓰지 말라는 표시. */
	pci_info(dev, "Disable Relaxed Ordering Attributes to avoid PCIe Completion erratum\n");	/* [한국어] 왜 껐는지 남긴다. */
}

/*
 * Intel Xeon processors based on Broadwell/Haswell microarchitecture Root
 * Complex have a Flow Control Credit issue which can cause performance
 * problems with Upstream Transaction Layer Packets with Relaxed Ordering set.
 */
/* [한국어] 위 영어 주석대로 Broadwell/Haswell 기반 Xeon 루트 콤플렉스.
 * 0x6f0x 는 Broadwell, 0x2f0x 는 Haswell 세대의 device ID 대역이다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f01, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f02, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f03, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x6f04, 0x6f05. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f04, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f05, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f06, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x6f07, 0x6f08. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f07, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f08, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f09, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x6f0a, 0x6f0b. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f0a, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f0b, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f0c, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x6f0d, 0x6f0e. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f0d, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x6f0e, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f01, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x2f02, 0x2f03. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f02, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f03, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f04, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x2f05, 0x2f06. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f05, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f06, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f07, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x2f08, 0x2f09. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f08, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f09, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f0a, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x2f0b, 0x2f0c. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f0b, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f0c, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f0d, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 이어지는 등록 줄: 0x2f0e. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_INTEL, 0x2f0e, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);

/*
 * The AMD ARM A1100 (aka "SEATTLE") SoC has a bug in its PCIe Root Complex
 * where Upstream Transaction Layer Packets with the Relaxed Ordering
 * Attribute clear are allowed to bypass earlier TLPs with Relaxed Ordering
 * set.  This is a violation of the PCIe 3.0 Transaction Ordering Rules
 * outlined in Section 2.4.1 (PCI Express(r) Base Specification Revision 3.0
 * November 10, 2010).  As a result, on this platform we can't use Relaxed
 * Ordering for Upstream TLPs.
 */
/* [한국어] 위 영어 주석대로 AMD ARM A1100(SEATTLE)의 루트 콤플렉스 3종.
 * 이쪽은 성능 문제가 아니라 PCIe 3.0 순서 규칙 위반이라 더 심각하다. */
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_AMD, 0x1a00, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_AMD, 0x1a01, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
DECLARE_PCI_FIXUP_CLASS_EARLY(PCI_VENDOR_ID_AMD, 0x1a02, PCI_CLASS_NOT_DEFINED, 8,
			      quirk_relaxedordering_disable);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/*
 * [한국어]
 * quirk_disable_root_port_attributes - 루트 포트에서 RO/NoSnoop 속성을 꺼 버린다
 *
 * @pdev: 문제가 되는 엔드포인트(고치는 대상은 그 위의 루트 포트다)
 * @return: 없음
 *
 * [스펙이 요구하는 것] 아래 영어 주석이 PCIe r3.0 2.2.9 절을 그대로
 * 인용한다. Completion 헤더는 대응하는 Request 헤더에 실렸던 Attribute 를
 * 그대로 실어야 한다(IDO 를 쓸 때의 명시적 예외만 빼고).
 * [무엇이 문제] 스펙을 지키지 않는 장치가 요청과 다른 속성으로 Completion
 * 을 만들면, 받는 쪽이 그것을 그냥 받아들이거나(그 자체도 2.3.2 절에
 * 비추어 비준수로 보인다) Malformed TLP 또는 Unexpected Completion 으로
 * 처리한다. 후자면 장치 접근 타임아웃으로 이어진다.
 * [그대로 두면] 그 장치와의 통신이 타임아웃으로 실패한다.
 * [우회] 문제의 장치가 속성을 0 으로 채운 Completion 을 만든다면, 애초에
 * 요청 쪽 속성도 0 이 되게 만들면 아귀가 맞는다. 그래서 상위 루트 포트의
 * Device Control 레지스터에서 Relaxed Ordering Enable 과 No Snoop Enable 을
 * 꺼, 그 아래로 나가는 요청이 늘 속성 0 을 갖게 한다.
 *
 * [부작용] 같은 루트 포트 아래의 다른 장치들도 영향을 받는다. 다만 영어
 * 주석대로 이 속성들은 성능 힌트일 뿐이라 기능상 문제는 없다.
 *
 * [HEADER 단계여도 되는 이유] 영어 주석의 마지막 문단이 설명한다.
 * Configuration Space 접근에는 애초에 TLP Attribute 가 붙지 않으므로,
 * config 접근이 끝난 뒤에 루트 포트를 고쳐도 늦지 않다.
 *
 * [매칭된 장치가 아닌 다른 장치를 고치는 패턴] 이 파일에서 반복되는
 * 구조다. quirk 는 문제 있는 엔드포인트에 걸리지만, 실제로 손대는 것은
 * pcie_find_root_port() 로 찾아 올라간 루트 포트다.
 *
 * 실행 컨텍스트: HEADER 단계(호출자인 Chelsio quirk 를 통해).
 *
 * 호출 체인:
 *   quirk_chelsio_T5_disable_root_port_attributes()
 *     -> [quirk_disable_root_port_attributes]
 *     -> pcie_find_root_port() -> pcie_capability_clear_word()
 */
/*
 * Per PCIe r3.0, sec 2.2.9, "Completion headers must supply the same
 * values for the Attribute as were supplied in the header of the
 * corresponding Request, except as explicitly allowed when IDO is used."
 *
 * If a non-compliant device generates a completion with a different
 * attribute than the request, the receiver may accept it (which itself
 * seems non-compliant based on sec 2.3.2), or it may handle it as a
 * Malformed TLP or an Unexpected Completion, which will probably lead to a
 * device access timeout.
 *
 * If the non-compliant device generates completions with zero attributes
 * (instead of copying the attributes from the request), we can work around
 * this by disabling the "Relaxed Ordering" and "No Snoop" attributes in
 * upstream devices so they always generate requests with zero attributes.
 *
 * This affects other devices under the same Root Port, but since these
 * attributes are performance hints, there should be no functional problem.
 *
 * Note that Configuration Space accesses are never supposed to have TLP
 * Attributes, so we're safe waiting till after any Configuration Space
 * accesses to do the Root Port fixup.
 */
/*
 * [한국어]
 * quirk_disable_root_port_attributes - 상위 루트 포트의 RO/NoSnoop 을 꺼 준다
 *
 * @pdev: 스펙을 어기는 당사자 장치. 고치는 대상은 이 장치가 아니라 그 위의 루트 포트다.
 * @return: 없음. 실패해도 경고만 남기고 진행한다 - quirk 는 부팅을 막지 않는다.
 *
 * [왜 필요한가] 위 영어 주석이 배경을 설명한다. PCIe 규약상 Completion 은
 * 대응하는 Request 의 TLP Attribute(Relaxed Ordering, No Snoop)를 그대로
 * 복사해야 한다. 일부 장치가 이를 어기고 속성이 0 인 Completion 을 돌려주면,
 * 요청자는 자신이 보낸 것과 다른 속성의 응답을 받고 이를 짝짓지 못해
 * 장치 접근 타임아웃에 빠진다.
 *
 * [우회 방법] 고장난 장치를 고칠 수는 없으므로 반대편을 맞춘다. 상위 루트
 * 포트에서 두 속성을 아예 꺼 두면 그 포트를 지나는 요청은 늘 속성이 0 이 되고,
 * 속성 0 인 Completion 과 일치하게 되어 짝짓기가 성립한다.
 *
 * [부작용] 같은 루트 포트 아래의 다른 장치도 RO/NoSnoop 을 잃는다. 다만 이
 * 두 속성은 성능 힌트일 뿐이라 기능적 문제는 없다는 것이 위 영어 주석의 판단이다.
 *
 * [실행 컨텍스트] DECLARE_PCI_FIXUP_CLASS_EARLY 계열 fixup 으로 열거 도중
 * 호출된다. 위 영어 주석이 밝히듯 Configuration Space 접근에는 TLP Attribute 가
 * 실리지 않으므로, config 접근이 끝난 뒤 루트 포트를 고쳐도 안전하다.
 *
 * 호출 체인:
 *   pci_fixup_device() → quirk_chelsio_T5_disable_root_port_attributes()
 *     → [이 함수] → pcie_find_root_port() / pcie_capability_clear_word()
 */
static void quirk_disable_root_port_attributes(struct pci_dev *pdev)
{
	struct pci_dev *root_port = pcie_find_root_port(pdev);	/* [한국어] 이 장치가 매달린 루트 포트를 거슬러 올라가 찾는다. 실제로 고칠 대상이다. */

	if (!root_port) {	/* [한국어] 루트 포트를 찾지 못하면 고칠 방법이 없다. */
		pci_warn(pdev, "PCIe Completion erratum may cause device errors\n");	/* [한국어] 그 경우 이 에라타가 장치 오류를 일으킬 수 있다고 경고만 남긴다. */
		return;
	}

	pci_info(root_port, "Disabling No Snoop/Relaxed Ordering Attributes to avoid PCIe Completion erratum in %s\n",	/* [한국어] 어느 장치 때문에 루트 포트의 속성을 끄는지 남긴다. 로그의 주체는 루트 포트이고, 원인 장치 이름을 인자로 넣는다. */
		 dev_name(&pdev->dev));
	pcie_capability_clear_word(root_port, PCI_EXP_DEVCTL,	/* [한국어] PCIe capability 의 Device Control 레지스터에서 두 비트를 지운다. */
				   PCI_EXP_DEVCTL_RELAX_EN |	/* [한국어] Relaxed Ordering Enable(비트 4)과 */
				   PCI_EXP_DEVCTL_NOSNOOP_EN);	/* [한국어] No Snoop Enable(비트 11)을 함께 끈다. 두 비트가 꺼지면 이 포트에서 나가는 요청의 Attribute 가 늘 0 이 된다. */
}

/*
 * [한국어]
 * quirk_chelsio_T5_disable_root_port_attributes - Chelsio T5 를 만나면 루트 포트를 고친다
 *
 * @pdev: Chelsio 벤더의 장치
 * @return: 없음
 *
 * [어떤 하드웨어] Chelsio T5 칩.
 * [무엇이 문제] 아래 영어 주석대로, T5 는 Request 의 TLP Attribute 를
 * Completion 으로 복사하지 못한다. 위 함수가 설명한 스펙 위반의 당사자다.
 * [우회] 상위 루트 포트의 RO/NoSnoop 을 꺼 요청 속성을 0 으로 만든다.
 *
 * [PF4 만 고르는 이유] 아래 영어 주석이 설명한다. 루트 포트는 한 번만
 * 고치면 되므로 T5 의 여러 물리 함수 중 하나만 골라야 한다. PF0~3 은
 * device ID 가 0x50xx 인데 PF4 만 0x54xx 로 구별되므로 그것을 쓴다.
 * 마스크 0xff00 으로 상위 바이트만 비교하는 것이 그 선택이다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_chelsio_T5_disable_root_port_attributes]
 *     -> quirk_disable_root_port_attributes()
 */
/*
 * The Chelsio T5 chip fails to copy TLP Attributes from a Request to the
 * Completion it generates.
 */
static void quirk_chelsio_T5_disable_root_port_attributes(struct pci_dev *pdev)
{
	/*
	 * This mask/compare operation selects for Physical Function 4 on a
	 * T5.  We only need to fix up the Root Port once for any of the
	 * PFs.  PF[0..3] have PCI Device IDs of 0x50xx, but PF4 is uniquely
	 * 0x54xx so we use that one.
	 */
	if ((pdev->device & 0xff00) == 0x5400)	/* [한국어] 위 영어 주석대로 device ID 의 상위 바이트가 0x54 인 물리 함수 4 만 고른다. */
		quirk_disable_root_port_attributes(pdev);	/* [한국어] 그때만 루트 포트를 고친다 - 여러 번 고칠 필요가 없다. */
}
/* [한국어] Chelsio 벤더의 모든 장치를 HEADER 단계에서 검사한다. T5 의
 * PF4 판별은 함수 안에서 한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_CHELSIO, PCI_ANY_ID,
			 quirk_chelsio_T5_disable_root_port_attributes);

/*
 * [한국어]
 * === 여기서부터 ACS(Access Control Services) 우회 영역 ===
 *
 * [ACS 란] PCIe 스위치나 루트 포트가 '아래에 달린 장치들끼리 서로 직접
 * 트래픽을 주고받지 못하게 막는' 기능이다. 막지 않으면 두 장치가 IOMMU 를
 * 거치지 않고 서로의 메모리에 접근할 수 있어 격리가 깨진다. 그래서 커널의
 * IOMMU 계층은 ACS 가 없는(또는 켜지지 않은) 포트 아래의 장치들을 하나의
 * IOMMU 그룹으로 묶어 버린다. 한 그룹은 통째로만 VM 에 넘길 수 있으므로,
 * ACS 가 없으면 VFIO 패스스루가 사실상 막힌다.
 *
 * [왜 quirk 가 필요한가] ACS capability 를 광고하지 않지만 하드웨어
 * 설계상 실제로는 피어투피어 전달을 하지 않는 칩이 많다. 그런 칩을
 * 그대로 두면 없어도 되는 격리 제약을 받는다. 그래서 '이 칩은 ACS 를
 * 광고하지 않지만 그에 준하는 보호를 실제로 제공한다' 고 화이트리스트에
 * 등록해 주는 것이 아래 함수들이다.
 *
 * [반대 방향의 quirk 도 있다] 뒤에 나오는 pci_dev_specific_disable_acs_redir()
 * 처럼, 광고는 하지만 실제로는 동작하지 않는 ACS 기능을 무효화하는 것도 있다.
 *
 * [반환값 규약] 아래 ACS 판정 함수들은 공통으로
 *   1  = 요청한 ACS 제어를 모두 제공한다,
 *   0  = 제공하지 않는다,
 *   음수(-ENOTTY / -ENODEV) = 이 quirk 는 이 장치에 해당하지 않는다
 * 를 돌려준다. 마지막 경우 호출자는 표준 ACS capability 검사로 넘어간다.
 */

/*
 * [한국어]
 * pci_acs_ctrl_enabled - 요청한 ACS 제어가 제공되는 것에 모두 포함되는지 본다
 *
 * @acs_ctrl_req: 호출자가 원하는 ACS 제어 비트마스크
 * @acs_ctrl_ena: 하드웨어 설계상 켜져 있거나 암묵적으로 제공되는 비트마스크
 * @return: 요청한 비트가 모두 제공 비트에 들어 있으면 1, 아니면 0
 *
 * 아래 ACS quirk 들이 공통으로 쓰는 작은 판정 함수다. '이 하드웨어가
 * 실질적으로 제공하는 보호' 를 두 번째 인자에 적어 두면, 호출자가 원하는
 * 것을 만족하는지 한 줄로 답할 수 있다.
 *
 * 실행 컨텍스트: ACS 판정 경로, 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   각종 pci_quirk_*_acs() -> [pci_acs_ctrl_enabled]
 */
/*
 * pci_acs_ctrl_enabled - compare desired ACS controls with those provided
 *			  by a device
 * @acs_ctrl_req: Bitmask of desired ACS controls
 * @acs_ctrl_ena: Bitmask of ACS controls enabled or provided implicitly by
 *		  the hardware design
 *
 * Return 1 if all ACS controls in the @acs_ctrl_req bitmask are included
 * in @acs_ctrl_ena, i.e., the device provides all the access controls the
 * caller desires.  Return 0 otherwise.
 */
static int pci_acs_ctrl_enabled(u16 acs_ctrl_req, u16 acs_ctrl_ena)
{
	if ((acs_ctrl_req & acs_ctrl_ena) == acs_ctrl_req)	/* [한국어] 요청 비트를 제공 비트와 AND 한 결과가 요청 비트 그대로면 요청이 모두 포함된 것이다(부분집합 판정). */
		return 1;	/* [한국어] 모두 제공된다. */
	return 0;	/* [한국어] 하나라도 빠지면 0 이다. */
}

/*
 * [한국어]
 * pci_quirk_amd_sb_acs - AMD 사우스브리지의 다기능 장치를 ACS 상당으로 인정
 *
 * @dev: AMD/ATI 사우스브리지의 다기능 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음, -ENODEV 이 quirk 는 해당 없음
 *
 * [근거] 아래 영어 주석대로 AMD 가 밝힌 사실이다. 나열된 장치들은 시스템에
 * AMD IOMMU 가 있는 사우스브리지 구성에서 피어투피어를 전혀 지원하지
 * 않는다. 함수 간 피어투피어를 지원하지 않는 다기능 장치는 ACS 의 일부를
 * 지원한다고 주장할 수 있다 - 모든 트랜잭션이 어차피 상위 루트
 * 콤플렉스로 재지향되므로, 사실상 Request Redirect(RR)와 Completion
 * Redirect(CR)가 켜진 것과 같기 때문이다.
 * [대상 목록] 아래 영어 주석에 device ID 와 이름이 나열되어 있다 -
 * SBx00 SMBus, IDE, Azalia(HDA), LPC, PCI-to-PCI 브리지, USB OHCI2,
 * 그리고 FCH PCI 브리지와 FCH USB OHCI.
 *
 * [AMD IOMMU 가 있는지 어떻게 아는가] ACPI 의 IVRS 테이블이 AMD IOMMU 를
 * 기술한다. 그 테이블이 있으면 AMD IOMMU 가 있는 것이다.
 *
 * 실행 컨텍스트: ACS 판정 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_amd_sb_acs]
 *     -> acpi_get_table() -> pci_acs_ctrl_enabled()
 */
/*
 * AMD has indicated that the devices below do not support peer-to-peer
 * in any system where they are found in the southbridge with an AMD
 * IOMMU in the system.  Multifunction devices that do not support
 * peer-to-peer between functions can claim to support a subset of ACS.
 * Such devices effectively enable request redirect (RR) and completion
 * redirect (CR) since all transactions are redirected to the upstream
 * root complex.
 *
 * https://lore.kernel.org/r/201207111426.q6BEQTbh002928@mail.maya.org/
 * https://lore.kernel.org/r/20120711165854.GM25282@amd.com/
 * https://lore.kernel.org/r/20121005130857.GX4009@amd.com/
 *
 * 1002:4385 SBx00 SMBus Controller
 * 1002:439c SB7x0/SB8x0/SB9x0 IDE Controller
 * 1002:4383 SBx00 Azalia (Intel HDA)
 * 1002:439d SB7x0/SB8x0/SB9x0 LPC host controller
 * 1002:4384 SBx00 PCI to PCI Bridge
 * 1002:4399 SB7x0/SB8x0/SB9x0 USB OHCI2 Controller
 *
 * https://bugzilla.kernel.org/show_bug.cgi?id=81841#c15
 *
 * 1022:780f [AMD] FCH PCI Bridge
 * 1022:7809 [AMD] FCH USB OHCI Controller
 */
/*
 * [한국어]
 * pci_quirk_amd_sb_acs - AMD 사우스브리지가 ACS 를 갖춘 것처럼 취급한다
 *
 * @dev: 검사 대상 장치. 위 영어 주석이 나열한 SBx00/FCH 계열이 해당한다.
 * @return: 요청한 격리 수준을 만족하면 1, 아니면 0. 대상이 아니면 -ENODEV 로
 *          "판단 보류"를 알려 호출자가 표준 ACS 검사로 넘어가게 한다.
 *
 * [무엇이 문제인가] 이들 사우스브리지 장치는 ACS(Access Control Services)
 * capability 를 광고하지 않는다. 그대로 두면 커널은 같은 다기능 장치의 함수들
 * 사이에 P2P 트래픽이 새는 것을 막을 수 없다고 보고, 전부 하나의 IOMMU 그룹에
 * 묶어 버린다. 그러면 그중 하나만 VFIO 로 넘기는 일이 불가능해진다.
 *
 * [근거] AMD 가 하드웨어적으로는 격리가 보장된다고 밝혔고, 그 전제가 성립하는
 * 시스템인지를 IVRS 테이블 존재 여부로 확인한다. IVRS 는 AMD IOMMU 를 기술하는
 * ACPI 테이블이므로, 그것이 있다는 것은 AMD IOMMU 를 갖춘 플랫폼이라는 뜻이다.
 *
 * [반환값 세 갈래의 의미] -ENODEV 는 실패가 아니라 "이 quirk 의 대상이 아님"이다.
 * 1/0 은 요청한 acs_flags 를 만족하는지에 대한 실제 판정이다. 호출자
 * pci_dev_specific_acs_enabled() 는 -ENODEV 를 받으면 다음 판정 경로로 넘어간다.
 *
 * [실행 컨텍스트] IOMMU 그룹 구성 시점에 호출된다. ACPI 테이블을 읽으므로
 * 인터럽트 컨텍스트가 아니며, acpi_get_table 과 acpi_put_table 이 짝을 이룬다.
 *
 * 호출 체인:
 *   iommu_group_get_for_dev() → pci_acs_enabled()
 *     → pci_dev_specific_acs_enabled() → [이 함수] → acpi_get_table("IVRS")
 */
static int pci_quirk_amd_sb_acs(struct pci_dev *dev, u16 acs_flags)
{
/* [한국어] IVRS 테이블을 읽어야 판정할 수 있으므로 ACPI 가 없는 빌드에서는
 * 이 quirk 를 적용할 수 없다. */
#ifdef CONFIG_ACPI
	struct acpi_table_header *header = NULL;	/* [한국어] ACPI 테이블 헤더를 받을 포인터. */
	acpi_status status;	/* [한국어] acpi_get_table() 의 반환 상태. */

	/* Targeting multifunction devices on the SB (appears on root bus) */
	if (!dev->multifunction || !pci_is_root_bus(dev->bus))	/* [한국어] 위 영어 주석대로 사우스브리지의 다기능 장치만 대상이다. 사우스브리지 장치는 루트 버스에 나타난다. */
		return -ENODEV;	/* [한국어] 해당 없음을 알린다 - 호출자는 표준 ACS 검사로 넘어간다. */

	/* The IVRS table describes the AMD IOMMU */
	status = acpi_get_table("IVRS", 0, &header);	/* [한국어] 위 영어 주석대로 IVRS 테이블이 AMD IOMMU 를 기술한다. */
	if (ACPI_FAILURE(status))	/* [한국어] IVRS 가 없으면 AMD IOMMU 가 없는 시스템이므로 AMD 가 밝힌 전제가 성립하지 않는다. */
		return -ENODEV;

	acpi_put_table(header);	/* [한국어] 내용을 볼 필요는 없고 존재 여부만 확인했으므로 곧바로 참조를 놓는다. */

	/* Filter out flags not applicable to multifunction */
	acs_flags &= (PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC | PCI_ACS_DT);	/* [한국어] 위 영어 주석대로 다기능 장치에 해당하지 않는 플래그를 걸러 낸다. 남는 것은 RR/CR/EC/DT 뿐이다. */

	return pci_acs_ctrl_enabled(acs_flags, PCI_ACS_RR | PCI_ACS_CR);	/* [한국어] 그중 이 하드웨어가 실질적으로 제공하는 것은 RR 과 CR 이다. 요청이 그 안에 들어오면 1 을 돌려준다. */
#else	/* [한국어] ACPI 가 없는 빌드에서는 */
	return -ENODEV;	/* [한국어] 판정할 수 없으므로 해당 없음을 알린다. */
#endif
}

/*
 * [한국어]
 * pci_quirk_cavium_acs_match - Cavium ThunderX 계열 루트 포트인지 판별한다
 *
 * @dev: 검사할 장치
 * @return: ThunderX1/X2/X3 의 루트 포트면 true
 *
 * ThunderX1 은 아래 영어 주석대로 device ID 대역 0xa000~0xa7ff 전체가
 * 다운스트림 포트다(SoC 8개분에 해당한다). X2 와 X3 은 각각 하나의
 * device ID 를 쓴다.
 *
 * 실행 컨텍스트: pci_quirk_cavium_acs() 안.
 *
 * 호출 체인:
 *   pci_quirk_cavium_acs() -> [pci_quirk_cavium_acs_match]
 */
static bool pci_quirk_cavium_acs_match(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev) || pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT)	/* [한국어] PCIe 루트 포트가 아니면 대상이 아니다 - ACS 는 포트의 기능이다. */
		return false;

	switch (dev->device) {	/* [한국어] 세대별로 device ID 를 확인한다. */
	/*
	 * Effectively selects all downstream ports for whole ThunderX1
	 * (which represents 8 SoCs).
	 */
	case 0xa000 ... 0xa7ff: /* ThunderX1 */
	case 0xaf84:  /* ThunderX2 */
	case 0xb884:  /* ThunderX3 */
		return true;	/* [한국어] 세 세대 중 하나면 대상이다. */
	default:	/* [한국어] 그 밖의 Cavium 장치는 */
		return false;	/* [한국어] 대상이 아니다. */
	}
}

/*
 * [한국어]
 * pci_quirk_cavium_acs - Cavium 루트 포트를 ACS 상당으로 인정한다
 *
 * @dev: Cavium 벤더의 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음, -ENOTTY 해당 없음
 *
 * [무엇이 문제] 아래 영어 주석대로 Cavium 루트 포트는 ACS capability 를
 * 광고하지 않는다. 그러나 RTL(하드웨어 구현) 차원에서 Source Validation,
 * Request Redirection, Completion Redirection, Upstream Forwarding 이
 * 켜진 ACS 와 같은 보호를 이미 제공한다.
 * [그대로 두면] 광고가 없으니 커널이 격리되지 않은 포트로 보고, 그 아래
 * 장치들을 하나의 IOMMU 그룹으로 묶어 패스스루를 막는다.
 * [우회] 그 네 기능이 구현되고 켜져 있는 것과 동등하다고 단언한다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_cavium_acs]
 *     -> pci_quirk_cavium_acs_match() -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_cavium_acs(struct pci_dev *dev, u16 acs_flags)
{
	if (!pci_quirk_cavium_acs_match(dev))	/* [한국어] ThunderX 루트 포트가 아니면 */
		return -ENOTTY;	/* [한국어] 이 quirk 는 해당 없음을 알린다. */

	/*
	 * Cavium Root Ports don't advertise an ACS capability.  However,
	 * the RTL internally implements similar protection as if ACS had
	 * Source Validation, Request Redirection, Completion Redirection,
	 * and Upstream Forwarding features enabled.  Assert that the
	 * hardware implements and enables equivalent ACS functionality for
	 * these flags.
	 */
	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 위 영어 주석이 단언하는 네 기능을 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV(Source Validation), RR(Request Redirect), CR(Completion Redirect), UF(Upstream Forwarding). */
}

/*
 * [한국어]
 * pci_quirk_xgene_acs - X-Gene 루트 포트를 ACS 상당으로 인정한다
 *
 * @dev: X-Gene 루트 포트(등록표에서 매칭된 것)
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음
 *
 * [무엇이 문제] 아래 영어 주석대로, 이 quirk 에 매칭되는 X-Gene 루트
 * 포트는 다른 장치와 피어투피어 트랜잭션을 하지 않는다. 그래서 해당
 * ACS 비트들이 구현되지 않은 것처럼 마스킹해도 된다.
 * [우회] Cavium 과 같은 네 기능을 실질 제공으로 단언한다.
 *
 * [Cavium 판과의 차이] 이쪽은 별도의 매칭 함수 없이 등록표에서 이미
 * 대상을 좁혔으므로 곧바로 판정만 한다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_xgene_acs]
 *     -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_xgene_acs(struct pci_dev *dev, u16 acs_flags)
{
	/*
	 * X-Gene Root Ports matching this quirk do not allow peer-to-peer
	 * transactions with others, allowing masking out these bits as if they
	 * were unimplemented in the ACS capability.
	 */
	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 위 Cavium 과 같은 네 기능을 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */
}

/*
 * [한국어]
 * pci_quirk_zhaoxin_pcie_ports_acs - Zhaoxin 포트를 ACS 상당으로 인정한다
 *
 * @dev: Zhaoxin 벤더의 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, false(0) 목록에 없음, -ENOTTY 포트가 아님
 *
 * [무엇이 문제] 아래 영어 주석대로 많은 Zhaoxin 루트 포트와 스위치
 * 다운스트림 포트가 ACS capability 를 갖고 있지 않다. 그러나 구현상
 * 포트 사이의 피어투피어 트랜잭션을 막고 ACS 와 같은 기능을 제공한다.
 * [우회] 알려진 device ID 대역에 대해 SV/RR/CR/UF 를 실질 제공으로 단언한다.
 *
 * [대역으로 좁히는 이유] 아래 영어 주석대로 앞으로 나올 Zhaoxin 포트는
 * 스펙대로 ACS capability 를 구현할 예정이다. 그래서 지금 알려진 구형
 * device ID 대역에만 이 인정을 적용하고, 새 칩은 표준 검사를 받게 둔다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_zhaoxin_pcie_ports_acs]
 *     -> pci_acs_ctrl_enabled()
 */
/*
 * Many Zhaoxin Root Ports and Switch Downstream Ports have no ACS capability.
 * But the implementation could block peer-to-peer transactions between them
 * and provide ACS-like functionality.
 */
static int pci_quirk_zhaoxin_pcie_ports_acs(struct pci_dev *dev, u16 acs_flags)
{
	if (!pci_is_pcie(dev) ||	/* [한국어] PCIe 가 아니거나 */
	    ((pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT) &&	/* [한국어] 루트 포트도 아니고 */
	     (pci_pcie_type(dev) != PCI_EXP_TYPE_DOWNSTREAM)))	/* [한국어] 스위치 다운스트림 포트도 아니면 ACS 를 따질 대상이 아니다. */
		return -ENOTTY;	/* [한국어] 해당 없음을 알린다. */

	/*
	 * Future Zhaoxin Root Ports and Switch Downstream Ports will
	 * implement ACS capability in accordance with the PCIe Spec.
	 */
	switch (dev->device) {	/* [한국어] 위 영어 주석대로 지금 알려진 구형 device ID 대역만 인정한다. */
	case 0x0710 ... 0x071e:	/* [한국어] 0x0710~0x071e 대역. */
	case 0x0721:	/* [한국어] 단일 ID 0x0721. */
	case 0x0723 ... 0x0752:	/* [한국어] 0x0723~0x0752 대역. */
		return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 이 대역들은 SV/RR/CR/UF 를 실질적으로 제공한다. */
			PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV(Source Validation), RR, CR, UF. */
	}

	return false;	/* [한국어] 목록에 없는 새 칩은 이 quirk 로 인정하지 않는다. 스펙대로 ACS 를 구현했을 것이므로 표준 검사에 맡긴다. */
}

/* [한국어] ACS capability 없이 ACS 상당 기능을 제공하는 Intel PCH 루트 포트 목록.
 * 설정자: 컴파일 시 고정된 const 배열.
 * 읽는 자: pci_quirk_intel_pch_acs_match() 가 선형 탐색으로 훑는다.
 * 값 범위: PCH 세대별 루트 포트 device ID 들. 각 줄 위의 영어 주석이 세대명이다.
 * 근거: 아래 영어 주석대로 인텔이 Red Hat 버그질라 1037684 에 제공한 목록이다.
 * 동기화: 읽기 전용이라 경쟁이 없다. */
/*
 * Many Intel PCH Root Ports do provide ACS-like features to disable peer
 * transactions and validate bus numbers in requests, but do not provide an
 * actual PCIe ACS capability.  This is the list of device IDs known to fall
 * into that category as provided by Intel in Red Hat bugzilla 1037684.
 */
static const u16 pci_quirk_intel_pch_acs_ids[] = {
	/* Ibexpeak PCH */
	0x3b42, 0x3b43, 0x3b44, 0x3b45, 0x3b46, 0x3b47, 0x3b48, 0x3b49,	/* [한국어] Ibexpeak PCH 의 루트 포트 앞 8개. */
	0x3b4a, 0x3b4b, 0x3b4c, 0x3b4d, 0x3b4e, 0x3b4f, 0x3b50, 0x3b51,	/* [한국어] Ibexpeak PCH 의 나머지 8개. */
	/* Cougarpoint PCH */
	0x1c10, 0x1c11, 0x1c12, 0x1c13, 0x1c14, 0x1c15, 0x1c16, 0x1c17,	/* [한국어] Cougarpoint PCH 앞 8개. */
	0x1c18, 0x1c19, 0x1c1a, 0x1c1b, 0x1c1c, 0x1c1d, 0x1c1e, 0x1c1f,	/* [한국어] Cougarpoint PCH 나머지 8개. */
	/* Pantherpoint PCH */
	0x1e10, 0x1e11, 0x1e12, 0x1e13, 0x1e14, 0x1e15, 0x1e16, 0x1e17,	/* [한국어] Pantherpoint PCH 앞 8개. */
	0x1e18, 0x1e19, 0x1e1a, 0x1e1b, 0x1e1c, 0x1e1d, 0x1e1e, 0x1e1f,	/* [한국어] Pantherpoint PCH 나머지 8개. */
	/* Lynxpoint-H PCH */
	0x8c10, 0x8c11, 0x8c12, 0x8c13, 0x8c14, 0x8c15, 0x8c16, 0x8c17,	/* [한국어] Lynxpoint-H PCH 앞 8개. */
	0x8c18, 0x8c19, 0x8c1a, 0x8c1b, 0x8c1c, 0x8c1d, 0x8c1e, 0x8c1f,	/* [한국어] Lynxpoint-H PCH 나머지 8개. */
	/* Lynxpoint-LP PCH */
	0x9c10, 0x9c11, 0x9c12, 0x9c13, 0x9c14, 0x9c15, 0x9c16, 0x9c17,	/* [한국어] Lynxpoint-LP PCH 앞 8개. */
	0x9c18, 0x9c19, 0x9c1a, 0x9c1b,	/* [한국어] Lynxpoint-LP PCH 나머지 4개. */
	/* Wildcat PCH */
	0x9c90, 0x9c91, 0x9c92, 0x9c93, 0x9c94, 0x9c95, 0x9c96, 0x9c97,	/* [한국어] Wildcat PCH 앞 8개. */
	0x9c98, 0x9c99, 0x9c9a, 0x9c9b,	/* [한국어] Wildcat PCH 나머지 4개. */
	/* Patsburg (X79) PCH */
	0x1d10, 0x1d12, 0x1d14, 0x1d16, 0x1d18, 0x1d1a, 0x1d1c, 0x1d1e,	/* [한국어] Patsburg(X79) PCH - 짝수 ID 만 쓰인다. */
	/* Wellsburg (X99) PCH */
	0x8d10, 0x8d11, 0x8d12, 0x8d13, 0x8d14, 0x8d15, 0x8d16, 0x8d17,	/* [한국어] Wellsburg(X99) PCH 앞 8개. */
	0x8d18, 0x8d19, 0x8d1a, 0x8d1b, 0x8d1c, 0x8d1d, 0x8d1e,	/* [한국어] Wellsburg(X99) PCH 나머지 7개. */
	/* Lynx Point (9 series) PCH */
	0x8c90, 0x8c92, 0x8c94, 0x8c96, 0x8c98, 0x8c9a, 0x8c9c, 0x8c9e,	/* [한국어] Lynx Point(9 시리즈) PCH - 짝수 ID 만 쓰인다. */
};

/*
 * [한국어]
 * pci_quirk_intel_pch_acs_match - 이 장치가 위 목록의 PCH 루트 포트인지 본다
 *
 * @dev: 검사할 장치
 * @return: 목록에 있는 루트 포트면 true
 *
 * 목록이 100개가 넘어 선형 탐색을 하지만, ACS 판정은 열거 시점에 한 번씩만
 * 일어나므로 성능 문제가 되지 않는다.
 *
 * 실행 컨텍스트: pci_quirk_intel_pch_acs() 안.
 *
 * 호출 체인:
 *   pci_quirk_intel_pch_acs() -> [pci_quirk_intel_pch_acs_match]
 */
static bool pci_quirk_intel_pch_acs_match(struct pci_dev *dev)
{
	int i;	/* [한국어] 목록을 훑는 반복자. */

	/* Filter out a few obvious non-matches first */
	if (!pci_is_pcie(dev) || pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT)	/* [한국어] 위 영어 주석대로 명백히 아닌 것부터 걸러 낸다 - PCIe 루트 포트가 아니면 볼 필요가 없다. */
		return false;

	for (i = 0; i < ARRAY_SIZE(pci_quirk_intel_pch_acs_ids); i++)	/* [한국어] 목록 전체를 선형 탐색한다. */
		if (pci_quirk_intel_pch_acs_ids[i] == dev->device)	/* [한국어] device ID 가 일치하면 */
			return true;	/* [한국어] 이 quirk 의 대상이다. */

	return false;	/* [한국어] 목록에 없으면 대상이 아니다. */
}

/*
 * [한국어]
 * pci_quirk_intel_pch_acs - PCH 루트 포트의 ACS 상당 기능을 조건부로 인정한다
 *
 * @dev: Intel 벤더의 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음, -ENOTTY 해당 없음
 *
 * [무엇이 문제] 위 배열 앞의 영어 주석대로 많은 Intel PCH 루트 포트가 피어 트랜잭션을
 * 막고 요청의 버스 번호를 검증하는 ACS 상당 기능을 제공하지만, 실제 PCIe
 * ACS capability 는 노출하지 않는다.
 * [★ 다른 ACS quirk 와 다른 점] 무조건 인정하지 않고
 * PCI_DEV_FLAGS_ACS_ENABLED_QUIRK 플래그가 서 있을 때만 인정한다. 이 플래그는
 * PCI 코어가 '이 장치에 ACS 상당 기능을 켰다' 고 표시해 둔 것으로, 실제로
 * 켜 두지 않은 채 인정하면 격리되지 않은 장치를 격리된 것으로 오인하게
 * 되기 때문이다.
 * [플래그가 없을 때] 제공 목록을 0 으로 넘긴다. 그러면 호출자가 아무
 * 제어도 요구하지 않은 경우(acs_flags 가 0)에만 1 이 되고, 하나라도
 * 요구하면 0 이 된다. 즉 '아무것도 제공하지 않는다' 는 답이다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_intel_pch_acs]
 *     -> pci_quirk_intel_pch_acs_match() -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_intel_pch_acs(struct pci_dev *dev, u16 acs_flags)
{
	if (!pci_quirk_intel_pch_acs_match(dev))	/* [한국어] 목록에 없는 장치면 */
		return -ENOTTY;	/* [한국어] 이 quirk 는 해당 없음을 알린다. */

	if (dev->dev_flags & PCI_DEV_FLAGS_ACS_ENABLED_QUIRK)	/* [한국어] PCI 코어가 ACS 상당 기능을 실제로 켜 두었을 때만 인정한다. */
		return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 그때는 네 기능을 실질 제공으로 본다. */
			PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */

	return pci_acs_ctrl_enabled(acs_flags, 0);	/* [한국어] 켜 두지 않았다면 제공하는 것이 없다고 답한다. */
}

/*
 * [한국어]
 * pci_quirk_qcom_rp_acs - QCOM 루트 포트를 ACS 상당으로 인정한다
 *
 * @dev: 등록표에서 매칭된 QCOM 루트 포트
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음
 *
 * [무엇이 문제] 아래 영어 주석이 상세히 밝힌다. 이 QCOM 루트 포트들은
 * 피어 트랜잭션을 막고 요청의 버스 번호를 검증하는 ACS 상당 기능을
 * 제공하지만 실제 ACS capability 는 없다. 하드웨어가 Source Validation 을
 * 지원하기는 하는데, 위반을 ACS Violation 이 아니라 Completer Abort 로
 * 보고한다는 차이가 있다.
 * [왜 격리가 확실한가] 하드웨어가 피어투피어를 지원하지 않고, 각 루트
 * 포트가 고유한 세그먼트 번호를 가진 별도의 루트 콤플렉스다. 한 루트
 * 포트가 다른 루트 포트로 트래픽을 넘길 방법이 없으며, 모든 PCIe
 * 트랜잭션이 루트 포트 안에서 종료된다.
 * [우회] 네 기능을 실질 제공으로 단언한다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_qcom_rp_acs]
 *     -> pci_acs_ctrl_enabled()
 */
/*
 * These QCOM Root Ports do provide ACS-like features to disable peer
 * transactions and validate bus numbers in requests, but do not provide an
 * actual PCIe ACS capability.  Hardware supports source validation but it
 * will report the issue as Completer Abort instead of ACS Violation.
 * Hardware doesn't support peer-to-peer and each Root Port is a Root
 * Complex with unique segment numbers.  It is not possible for one Root
 * Port to pass traffic to another Root Port.  All PCIe transactions are
 * terminated inside the Root Port.
 */
static int pci_quirk_qcom_rp_acs(struct pci_dev *dev, u16 acs_flags)
{
	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 네 기능을 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */
}

/*
 * [한국어]
 * pci_quirk_nxp_rp_acs - NXP 루트 포트를 ACS 상당으로 인정한다
 *
 * @dev: 등록표에서 매칭된 NXP 루트 포트
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음
 *
 * [무엇이 문제] 아래 영어 주석대로, 이 NXP 루트 포트들은 각각 고유한
 * 세그먼트 번호를 가진 루트 콤플렉스 안에 있고 피어 트랜잭션을 막고
 * 버스 번호를 검증하는 격리 기능을 제공하지만 ACS capability 는 없다.
 * QCOM 판과 같은 구조의 판정이다.
 * [우회] 네 기능을 실질 제공으로 단언한다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_nxp_rp_acs]
 *     -> pci_acs_ctrl_enabled()
 */
/*
 * Each of these NXP Root Ports is in a Root Complex with a unique segment
 * number and does provide isolation features to disable peer transactions
 * and validate bus numbers in requests, but does not provide an ACS
 * capability.
 */
static int pci_quirk_nxp_rp_acs(struct pci_dev *dev, u16 acs_flags)
{
	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 네 기능을 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */
}

/*
 * [한국어]
 * pci_quirk_al_acs - Annapurna Labs 루트 포트를 ACS 상당으로 인정한다
 *
 * @dev: Amazon Annapurna Labs 의 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 요구한 것 중 제공하지 못하는 것이 있음, -ENOTTY 해당 없음
 *
 * [무엇이 문제] 아래 영어 주석대로 이 루트 포트들도 ACS capability 는
 * 없지만 ACS 상당 기능을 갖고 있다. 하드웨어가 루트 포트를 경유하는
 * 피어투피어를 지원하지 않고, 각각 고유한 세그먼트 번호를 가지며,
 * 루트 포트끼리도 서로에게 트래픽을 보낼 수 없다.
 * [우회] 네 기능을 제공하는 것으로 본다.
 *
 * [구현 방식의 차이] 위 함수들이 pci_acs_ctrl_enabled() 를 쓰는 것과 달리,
 * 이 함수는 요청 마스크에서 제공하는 비트를 직접 지운 뒤 남은 것이 있는지
 * 본다. 결과는 같지만 표현이 다르다 - 남은 비트가 없으면 1(모두 제공),
 * 있으면 0(일부는 제공하지 못함)이다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_al_acs]
 */
static int pci_quirk_al_acs(struct pci_dev *dev, u16 acs_flags)
{
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT)	/* [한국어] 루트 포트가 아니면 */
		return -ENOTTY;	/* [한국어] 이 quirk 는 해당 없음을 알린다. */

	/*
	 * Amazon's Annapurna Labs root ports don't include an ACS capability,
	 * but do include ACS-like functionality. The hardware doesn't support
	 * peer-to-peer transactions via the root port and each has a unique
	 * segment number.
	 *
	 * Additionally, the root ports cannot send traffic to each other.
	 */
	acs_flags &= ~(PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] 요청 마스크에서 이 하드웨어가 제공하는 네 비트를 지운다. */

	return acs_flags ? 0 : 1;	/* [한국어] 남은 요구가 없으면 1(모두 제공), 남아 있으면 0(일부 미제공). */
}

/*
 * [한국어]
 * pci_quirk_intel_spt_pch_acs_match - Sunrise Point 계열 PCH 루트 포트인지 본다
 *
 * @dev: 검사할 장치
 * @return: 아래 영어 주석이 나열한 device ID 대역의 루트 포트면 true
 *
 * [무엇이 문제] 아래 영어 주석이 매우 자세히 밝힌다. 이 PCH 루트 포트들은
 * ACS 를 '구현하기는 한다'. 문제는 레지스터의 배치다. PCIe 스펙(Rev 3.0,
 * 7.16 ACS Extended Capability)은 ACS Capability 와 Control 레지스터를
 * 워드(2바이트) 단위로 붙여 놓지만, 이 칩셋은 dword(4바이트) 단위로
 * 배치했다. 그래서 Control 레지스터가 표준 오프셋 6 이 아니라 8 에 있다.
 * 비트 정의 자체는 맞다.
 * [그대로 두면] 커널이 표준 오프셋 6 을 읽어 엉뚱한 값을 ACS Control 로
 * 해석한다. 즉 ACS 설정을 잘못 읽는다.
 * [우회] 이 계열임을 알아보고, 아래 pci_quirk_intel_spt_pch_acs() 가
 * 오프셋 8 에서 dword 로 Control 을 읽는다.
 *
 * [영향받는 칩셋] 영어 주석이 근거 문서와 함께 나열한다 - 100 시리즈
 * (Sunrise Point, 에라타 #23), 200 시리즈(Union Point, 에라타 22), 그리고
 * 7세대/8세대 모바일 플랫폼(에라타 22, 수정 계획 없음).
 *
 * [한계] 영어 주석이 밝히듯 이 quirk 는 lspci 가 보여 주는 값까지 고치지는
 * 못한다. 커널 내부 판정만 바로잡는다.
 *
 * 실행 컨텍스트: pci_quirk_intel_spt_pch_acs() 안.
 *
 * 호출 체인:
 *   pci_quirk_intel_spt_pch_acs() -> [pci_quirk_intel_spt_pch_acs_match]
 */
/*
 * Sunrise Point PCH root ports implement ACS, but unfortunately as shown in
 * the datasheet (Intel 100 Series Chipset Family PCH Datasheet, Vol. 2,
 * 12.1.46, 12.1.47)[1] this chipset uses dwords for the ACS capability and
 * control registers whereas the PCIe spec packs them into words (Rev 3.0,
 * 7.16 ACS Extended Capability).  The bit definitions are correct, but the
 * control register is at offset 8 instead of 6 and we should probably use
 * dword accesses to them.  This applies to the following PCI Device IDs, as
 * found in volume 1 of the datasheet[2]:
 *
 * 0xa110-0xa11f Sunrise Point-H PCI Express Root Port #{0-16}
 * 0xa167-0xa16a Sunrise Point-H PCI Express Root Port #{17-20}
 *
 * N.B. This doesn't fix what lspci shows.
 *
 * The 100 series chipset specification update includes this as errata #23[3].
 *
 * The 200 series chipset (Union Point) has the same bug according to the
 * specification update (Intel 200 Series Chipset Family Platform Controller
 * Hub, Specification Update, January 2017, Revision 001, Document# 335194-001,
 * Errata 22)[4].  Per the datasheet[5], root port PCI Device IDs for this
 * chipset include:
 *
 * 0xa290-0xa29f PCI Express Root port #{0-16}
 * 0xa2e7-0xa2ee PCI Express Root port #{17-24}
 *
 * Mobile chipsets are also affected, 7th & 8th Generation
 * Specification update confirms ACS errata 22, status no fix: (7th Generation
 * Intel Processor Family I/O for U/Y Platforms and 8th Generation Intel
 * Processor Family I/O for U Quad Core Platforms Specification Update,
 * August 2017, Revision 002, Document#: 334660-002)[6]
 * Device IDs from I/O datasheet: (7th Generation Intel Processor Family I/O
 * for U/Y Platforms and 8th Generation Intel ® Processor Family I/O for U
 * Quad Core Platforms, Vol 1 of 2, August 2017, Document#: 334658-003)[7]
 *
 * 0x9d10-0x9d1b PCI Express Root port #{1-12}
 *
 * [1] https://www.intel.com/content/www/us/en/chipsets/100-series-chipset-datasheet-vol-2.html
 * [2] https://www.intel.com/content/www/us/en/chipsets/100-series-chipset-datasheet-vol-1.html
 * [3] https://www.intel.com/content/www/us/en/chipsets/100-series-chipset-spec-update.html
 * [4] https://www.intel.com/content/www/us/en/chipsets/200-series-chipset-pch-spec-update.html
 * [5] https://www.intel.com/content/www/us/en/chipsets/200-series-chipset-pch-datasheet-vol-1.html
 * [6] https://www.intel.com/content/www/us/en/processors/core/7th-gen-core-family-mobile-u-y-processor-lines-i-o-spec-update.html
 * [7] https://www.intel.com/content/www/us/en/processors/core/7th-gen-core-family-mobile-u-y-processor-lines-i-o-datasheet-vol-1.html
 */
/*
 * [한국어]
 * pci_quirk_intel_spt_pch_acs_match - 이 quirk 를 적용할 Intel PCH 루트 포트인가
 *
 * @dev: 검사할 장치
 * @return: 위 영어 주석이 나열한 세 device ID 대역에 드는 루트 포트면 true
 *
 * [왜 필요한가] Sunrise Point / Union Point PCH 와 7·8세대 모바일 PCH 의 루트
 * 포트는 ACS 를 지원하면서도 capability 로 광고하지 않는다(위 영어 주석이 인용한
 * Errata 22, 수정 예정 없음). 그래서 커널이 ACS 유무를 표준 경로로는 알아낼 수
 * 없고, device ID 로 직접 식별해야 한다.
 *
 * [식별 방식] 위 영어 주석이 데이터시트에서 옮겨 온 세 대역을 그대로 대조한다.
 * 0xa110~0xa11f 와 0xa167~0xa16a 는 Sunrise Point, 0xa290~0xa29f 와
 * 0xa2e7~0xa2ee 는 Union Point, 0x9d10~0x9d1b 는 7·8세대 모바일이다.
 *
 * [이 판정 뒤에 오는 것] true 를 받은 장치에 대해서는 같은 파일의
 * pci_quirk_intel_spt_pch_acs 계열이 INTEL_SPT_ACS_CTRL 오프셋으로 ACS Control
 * 레지스터를 직접 다룬다. 바로 아래 정의된 그 매크로가 +4 를 쓰는 이유도
 * 이 칩셋이 스펙과 다른 배치를 갖기 때문이다.
 *
 * [실행 컨텍스트] IOMMU 그룹 구성과 ACS 활성화 시점에 호출되는 순수 판정
 * 함수다. 부수 효과가 없고 레지스터도 건드리지 않는다.
 *
 * 호출 체인:
 *   pci_dev_specific_enable_acs() / _acs_enabled() → [이 함수]
 */
static bool pci_quirk_intel_spt_pch_acs_match(struct pci_dev *dev)
{
	if (!pci_is_pcie(dev) || pci_pcie_type(dev) != PCI_EXP_TYPE_ROOT_PORT)	/* [한국어] PCIe 루트 포트가 아니면 대상이 아니다. */
		return false;

	switch (dev->device) {	/* [한국어] 위 영어 주석이 나열한 device ID 대역과 대조한다. */
	case 0xa110 ... 0xa11f: case 0xa167 ... 0xa16a: /* Sunrise Point */
	case 0xa290 ... 0xa29f: case 0xa2e7 ... 0xa2ee: /* Union Point */
	case 0x9d10 ... 0x9d1b: /* 7th & 8th Gen Mobile */
		return true;	/* [한국어] 세 대역 중 하나면 이 quirk 의 대상이다. */
	}

	return false;	/* [한국어] 그 밖의 Intel 장치는 대상이 아니다. */
}

/* [한국어] 이 칩셋에서 ACS Control 레지스터의 실제 오프셋.
 * PCI_ACS_CAP 은 ACS capability 안에서 Capability 레지스터의 오프셋이고,
 * 스펙대로라면 Control 은 그 2바이트 뒤에 있다. 그러나 이 칩셋은 dword
 * 단위로 배치해 4바이트 뒤에 있으므로 +4 를 쓴다. */
#define INTEL_SPT_ACS_CTRL (PCI_ACS_CAP + 4)

/*
 * [한국어]
 * pci_quirk_intel_spt_pch_acs - 어긋난 오프셋에서 ACS Control 을 읽어 판정한다
 *
 * @dev: Intel 벤더의 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음, -ENOTTY 해당 없음
 *
 * [다른 ACS quirk 와 근본적으로 다른 점] 앞의 quirk 들이 '광고는 없지만
 * 실제로는 격리된다' 고 단언하는 화이트리스트였다면, 이것은 진짜 ACS 를
 * 읽는다. 다만 레지스터 위치가 스펙과 달라서 그 위치를 바로잡아 읽을
 * 뿐이다. 즉 판정 결과는 하드웨어의 실제 설정에 따라 달라진다.
 *
 * [pci_acs_flags_enabled() 를 흉내 낸다] 아래 영어 주석이 그 함수를
 * 참조하라고 안내한다. 표준 판정과 같은 절차를 밟되 Control 레지스터의
 * 오프셋과 접근 폭만 다르다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_intel_spt_pch_acs]
 *     -> pci_quirk_intel_spt_pch_acs_match() -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_intel_spt_pch_acs(struct pci_dev *dev, u16 acs_flags)
{
	int pos;	/* [한국어] ACS capability 의 시작 오프셋. */
	u32 cap, ctrl;	/* [한국어] cap: ACS Capability 레지스터 값, ctrl: ACS Control 레지스터 값. 둘 다 dword 로 읽는다. */

	if (!pci_quirk_intel_spt_pch_acs_match(dev))	/* [한국어] 대상 칩셋이 아니면 */
		return -ENOTTY;	/* [한국어] 이 quirk 는 해당 없음을 알린다. */

	pos = dev->acs_cap;	/* [한국어] PCI 코어가 찾아 둔 ACS capability 의 오프셋. */
	if (!pos)	/* [한국어] ACS capability 자체가 없으면 이 quirk 로 판정할 수 없다. */
		return -ENOTTY;

	/* see pci_acs_flags_enabled() */
	pci_read_config_dword(dev, pos + PCI_ACS_CAP, &cap);	/* [한국어] 위 영어 주석이 참조하라는 표준 판정과 같은 절차다. Capability 레지스터를 읽어 하드웨어가 무엇을 구현했는지 본다. */
	acs_flags &= (cap | PCI_ACS_EC);	/* [한국어] 구현되지 않은 제어는 요청에서 지운다. PCI_ACS_EC 를 함께 허용하는 것은 표준 판정과 같은 처리다. */

	pci_read_config_dword(dev, pos + INTEL_SPT_ACS_CTRL, &ctrl);	/* [한국어] ★ 여기가 이 quirk 의 핵심이다. 표준 오프셋(+2)이 아니라 +4 에서 dword 로 Control 레지스터를 읽는다. */

	return pci_acs_ctrl_enabled(acs_flags, ctrl);	/* [한국어] 요청한 제어가 실제로 켜져 있는지 판정한다. */
}

/*
 * [한국어]
 * pci_quirk_mf_endpoint_acs - 피어투피어를 하지 않는 다기능 엔드포인트를 인정한다
 *
 * @dev: 등록표에서 매칭된 다기능 엔드포인트
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음
 *
 * [무엇이 문제] 아래 영어 주석이 설명한다. SV/TB/UF 는 다기능 엔드포인트와
 * 무관한 제어다. 그리고 다기능 장치는 피어투피어 트랜잭션을 지원할 때만
 * ACS capability 에 RR/CR/DT 를 구현하면 된다. 이 quirk 에 매칭되는
 * 장치들은 벤더가 '다른 함수와 피어투피어를 하지 않는다' 고 확인해 준
 * 것들이라, 그 비트들이 구현되지 않은 것처럼 마스킹해도 된다.
 * [그대로 두면] 함수끼리 격리되지 않은 것으로 보여 한 IOMMU 그룹에
 * 묶이고, 함수 단위 패스스루가 막힌다.
 * [우회] 여섯 제어를 모두 실질 제공으로 단언한다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_mf_endpoint_acs]
 *     -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_mf_endpoint_acs(struct pci_dev *dev, u16 acs_flags)
{
	/*
	 * SV, TB, and UF are not relevant to multifunction endpoints.
	 *
	 * Multifunction devices are only required to implement RR, CR, and DT
	 * in their ACS capability if they support peer-to-peer transactions.
	 * Devices matching this quirk have been verified by the vendor to not
	 * perform peer-to-peer with other functions, allowing us to mask out
	 * these bits as if they were unimplemented in the ACS capability.
	 */
	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 여섯 제어를 모두 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_TB | PCI_ACS_RR |	/* [한국어] SV(Source Validation), TB(Translation Blocking), RR(Request Redirect), */
		PCI_ACS_CR | PCI_ACS_UF | PCI_ACS_DT);	/* [한국어] CR(Completion Redirect), UF(Upstream Forwarding), DT(Direct Translated P2P). */
}

/*
 * [한국어]
 * pci_quirk_rciep_acs - Intel RCiEP 를 ACS 상당으로 인정한다
 *
 * @dev: 검사할 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음, -ENOTTY RCiEP 가 아님
 *
 * [RCiEP 란] Root Complex integrated EndPoint - 루트 콤플렉스 안에 통합된
 * 엔드포인트다. 물리적인 PCIe 링크 없이 SoC 내부에 붙어 있다.
 * [근거] 아래 영어 주석대로 Intel VT-d 스펙 r3.1 3.16 절
 * 'Root-Complex Peer to Peer Considerations' 가 근거다. Intel RCiEP 는
 * 변환된 주소에 대해서만 피어투피어를 허용해야 한다.
 * [우회] 그 제약 덕분에 SV/RR/CR/UF 가 켜진 ACS 와 동등한 격리가 보장되므로
 * 그렇게 인정한다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_rciep_acs]
 *     -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_rciep_acs(struct pci_dev *dev, u16 acs_flags)
{
	/*
	 * Intel RCiEP's are required to allow p2p only on translated
	 * addresses.  Refer to Intel VT-d specification, r3.1, sec 3.16,
	 * "Root-Complex Peer to Peer Considerations".
	 */
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_RC_END)	/* [한국어] RCiEP 가 아니면 위 영어 주석의 근거가 적용되지 않는다. */
		return -ENOTTY;	/* [한국어] 해당 없음을 알린다. */

	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 네 기능을 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */
}

/*
 * [한국어]
 * pci_quirk_brcm_acs - Broadcom iProc PAXB 루트 포트를 ACS 상당으로 인정한다
 *
 * @dev: 등록표에서 매칭된 iProc PAXB 루트 포트
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음
 *
 * [무엇이 문제] 아래 영어 주석대로 iProc PAXB 루트 포트는 ACS capability 를
 * 광고하지 않지만 루트 포트끼리의 피어투피어 트랜잭션을 허용하지 않는다.
 * [그대로 두면] 여러 루트 포트가 한 IOMMU 그룹으로 묶인다.
 * [우회] SV/RR/CR/UF 를 마스킹해 각 루트 포트가 별도의 IOMMU 그룹이 되게
 * 한다 - 영어 주석이 그 목적을 명시한다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_brcm_acs]
 *     -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_brcm_acs(struct pci_dev *dev, u16 acs_flags)
{
	/*
	 * iProc PAXB Root Ports don't advertise an ACS capability, but
	 * they do not allow peer-to-peer transactions between Root Ports.
	 * Allow each Root Port to be in a separate IOMMU group by masking
	 * SV/RR/CR/UF bits.
	 */
	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 네 기능을 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */
}

/*
 * [한국어]
 * pci_quirk_loongson_acs - Loongson 루트 포트를 ACS 상당으로 인정한다
 *
 * @dev: 등록표에서 매칭된 Loongson 루트 포트
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, 0 제공하지 않음
 *
 * [무엇이 문제] 아래 영어 주석대로 Broadcom iProc 와 같은 상황이다. ACS
 * capability 를 광고하지 않지만 루트 포트끼리의 피어투피어를 허용하지
 * 않는다.
 * [우회] SV/RR/CR/UF 를 마스킹해 각 루트 포트를 별도 IOMMU 그룹으로 만든다.
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_loongson_acs]
 *     -> pci_acs_ctrl_enabled()
 */
static int pci_quirk_loongson_acs(struct pci_dev *dev, u16 acs_flags)
{
	/*
	 * Loongson PCIe Root Ports don't advertise an ACS capability, but
	 * they do not allow peer-to-peer transactions between Root Ports.
	 * Allow each Root Port to be in a separate IOMMU group by masking
	 * SV/RR/CR/UF bits.
	 */
	return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 네 기능을 실질 제공 목록으로 넘긴다. */
		PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */
}

/*
 * [한국어]
 * pci_quirk_wangxun_nic_acs - Wangxun NIC 의 함수 간 격리를 ACS 상당으로 인정
 *
 * @dev: Wangxun 벤더의 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: 1 제공함, false(0) 목록에 없거나 제공하지 않음
 *
 * [무엇이 문제] 아래 영어 주석대로 Wangxun 의 40G/25G/10G/1G NIC 은 ACS
 * capability 가 없다. 그러나 다기능 장치에서 하드웨어가 모든 피어투피어
 * 트래픽을 상위로 올려보내 함수들을 격리한다 - 즉 PCI_ACS_RR 과
 * PCI_ACS_CR 이 설정된 것과 같은 효과다.
 * [우회] 아래 device ID 목록에 대해 네 기능을 실질 제공으로 단언한다.
 * 영어 주석이 모델군을 밝힌다 - SFxxx 1G(em), RP1000/RP2000 10G(sp),
 * FF5xxx 40G/25G/10G(aml).
 *
 * 실행 컨텍스트: ACS 판정 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_acs_enabled() -> [pci_quirk_wangxun_nic_acs]
 *     -> pci_acs_ctrl_enabled()
 */
/*
 * Wangxun 40G/25G/10G/1G NICs have no ACS capability, but on
 * multi-function devices, the hardware isolates the functions by
 * directing all peer-to-peer traffic upstream as though PCI_ACS_RR and
 * PCI_ACS_CR were set.
 * SFxxx 1G NICs(em).
 * RP1000/RP2000 10G NICs(sp).
 * FF5xxx 40G/25G/10G NICs(aml).
 */
static int  pci_quirk_wangxun_nic_acs(struct pci_dev *dev, u16 acs_flags)
{
	switch (dev->device) {	/* [한국어] 벤더 전체가 등록되어 있어 함수 안에서 모델을 가려낸다. */
	case 0x0100 ... 0x010F: /* EM */
	case 0x1001: case 0x2001: /* SP */
	case 0x5010: case 0x5025: case 0x5040: /* AML */
	case 0x5110: case 0x5125: case 0x5140: /* AML */
		return pci_acs_ctrl_enabled(acs_flags,	/* [한국어] 목록에 있는 모델이면 네 기능을 실질 제공으로 본다. */
			PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF);	/* [한국어] SV / RR / CR / UF. */
	}

	return false;	/* [한국어] 목록에 없는 Wangxun 장치는 이 인정을 받지 못한다. */
}

/*
 * [한국어] ACS 판정 quirk 표와 그 항목 타입.
 *
 * 이 표는 '광고된 ACS capability 만으로는 판단할 수 없는 장치' 를 모아
 * 두고, 각 장치에 어떤 판정 함수를 쓸지 지정한다. 위쪽에 정의된
 * pci_quirk_*_acs() 함수들이 그 판정 함수다.
 *
 * 순서가 중요하다: 아래 pci_dev_specific_acs_enabled() 는 표를 위에서부터
 * 훑되 판정 함수가 음수(-ENOTTY/-ENODEV, '해당 없음')를 돌려주면 다음
 * 항목으로 넘어간다. 그래서 같은 벤더에 PCI_ANY_ID 로 걸린 넓은 항목과
 * 구체적 device ID 항목이 공존할 수 있다 - 예컨대 Intel 은 개별 NIC
 * 항목들 뒤에 RCiEP, PCH, SPT PCH 용 PCI_ANY_ID 항목이 차례로 놓인다.
 */
static const struct pci_dev_acs_enabled {	/* [한국어] 표 항목의 타입을 익명 구조체로 정의하면서 곧바로 배열을 만든다. */
	u16 vendor;	/* [한국어] 벤더 ID. PCI_ANY_ID 면 그 필드를 따지지 않는다. */
	u16 device;	/* [한국어] 디바이스 ID. PCI_ANY_ID 면 그 필드를 따지지 않는다. */
	int (*acs_enabled)(struct pci_dev *dev, u16 acs_flags);	/* [한국어] 이 항목에 맞는 장치를 만났을 때 부를 판정 함수. 1(제공) / 0(미제공) / 음수(해당 없음)를 돌려준다. */
} pci_dev_acs_enabled[] = {	/* [한국어] 타입 정의와 동시에 표 자체를 정의한다. */
	/* [한국어] AMD/ATI 사우스브리지의 다기능 장치들 - AMD 가 피어투피어
	 * 미지원을 확인해 준 목록이다(pci_quirk_amd_sb_acs 의 주석 참조). */
	{ PCI_VENDOR_ID_ATI, 0x4385, pci_quirk_amd_sb_acs },
	{ PCI_VENDOR_ID_ATI, 0x439c, pci_quirk_amd_sb_acs },
	{ PCI_VENDOR_ID_ATI, 0x4383, pci_quirk_amd_sb_acs },
	{ PCI_VENDOR_ID_ATI, 0x439d, pci_quirk_amd_sb_acs },
	{ PCI_VENDOR_ID_ATI, 0x4384, pci_quirk_amd_sb_acs },
	{ PCI_VENDOR_ID_ATI, 0x4399, pci_quirk_amd_sb_acs },
	{ PCI_VENDOR_ID_AMD, 0x780f, pci_quirk_amd_sb_acs },
	{ PCI_VENDOR_ID_AMD, 0x7809, pci_quirk_amd_sb_acs },
	/* [한국어] 여기부터 pci_quirk_mf_endpoint_acs 를 쓰는 다기능 엔드포인트들.
	 * 벤더가 '함수 간 피어투피어를 하지 않는다' 고 확인해 준 장치들이다. */
	{ PCI_VENDOR_ID_SOLARFLARE, 0x0903, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_SOLARFLARE, 0x0923, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_SOLARFLARE, 0x0A03, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10C6, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10DB, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10DD, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10E1, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10F1, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10F7, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10F8, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10F9, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10FA, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10FB, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10FC, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1507, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1514, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x151C, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1529, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x152A, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x154D, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x154F, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1551, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1558, pci_quirk_mf_endpoint_acs },
	/* 82580 */
	{ PCI_VENDOR_ID_INTEL, 0x1509, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x150E, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x150F, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1510, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1511, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1516, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1527, pci_quirk_mf_endpoint_acs },
	/* 82576 */
	{ PCI_VENDOR_ID_INTEL, 0x10C9, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10E6, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10E7, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10E8, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x150A, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x150D, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1518, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1526, pci_quirk_mf_endpoint_acs },
	/* 82575 */
	{ PCI_VENDOR_ID_INTEL, 0x10A7, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10A9, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10D6, pci_quirk_mf_endpoint_acs },
	/* I350 */
	{ PCI_VENDOR_ID_INTEL, 0x1521, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1522, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1523, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1524, pci_quirk_mf_endpoint_acs },
	/* 82571 (Quads omitted due to non-ACS switch) */
	{ PCI_VENDOR_ID_INTEL, 0x105E, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x105F, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x1060, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x10D9, pci_quirk_mf_endpoint_acs },
	/* I219 */
	{ PCI_VENDOR_ID_INTEL, 0x15b7, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_INTEL, 0x15b8, pci_quirk_mf_endpoint_acs },
	/* [한국어] Intel 벤더 전체에 대한 RCiEP 판정. 위의 개별 NIC 항목들이
	 * 먼저 놓여 있으므로, 그것들에 맞지 않는 Intel 장치만 여기로 내려온다. */
	{ PCI_VENDOR_ID_INTEL, PCI_ANY_ID, pci_quirk_rciep_acs },
	/* QCOM QDF2xxx root ports */
	{ PCI_VENDOR_ID_QCOM, 0x0400, pci_quirk_qcom_rp_acs },
	{ PCI_VENDOR_ID_QCOM, 0x0401, pci_quirk_qcom_rp_acs },
	/* QCOM SA8775P root port */
	{ PCI_VENDOR_ID_QCOM, 0x0115, pci_quirk_qcom_rp_acs },
	/* QCOM Hamoa root port */
	{ PCI_VENDOR_ID_QCOM, 0x0111, pci_quirk_qcom_rp_acs },
	/* QCOM Glymur root port */
	{ PCI_VENDOR_ID_QCOM, 0x0120, pci_quirk_qcom_rp_acs },
	/* HXT SD4800 root ports. The ACS design is same as QCOM QDF2xxx */
	{ PCI_VENDOR_ID_HXT, 0x0401, pci_quirk_qcom_rp_acs },
	/* Intel PCH root ports */
	/* [한국어] Intel PCH 루트 포트 판정 둘. 앞의 것은 목록 기반 인정,
	 * 뒤의 것은 오프셋이 어긋난 ACS 레지스터를 바로 읽는 판정이다. 앞이
	 * 해당 없음을 돌려주면 뒤가 시도된다. */
	{ PCI_VENDOR_ID_INTEL, PCI_ANY_ID, pci_quirk_intel_pch_acs },
	{ PCI_VENDOR_ID_INTEL, PCI_ANY_ID, pci_quirk_intel_spt_pch_acs },
	{ 0x19a2, 0x710, pci_quirk_mf_endpoint_acs }, /* Emulex BE3-R */
	{ 0x10df, 0x720, pci_quirk_mf_endpoint_acs }, /* Emulex Skyhawk-R */
	/* Cavium ThunderX */
	{ PCI_VENDOR_ID_CAVIUM, PCI_ANY_ID, pci_quirk_cavium_acs },
	/* Cavium multi-function devices */
	{ PCI_VENDOR_ID_CAVIUM, 0xA026, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_CAVIUM, 0xA059, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_CAVIUM, 0xA060, pci_quirk_mf_endpoint_acs },
	/* APM X-Gene */
	{ PCI_VENDOR_ID_AMCC, 0xE004, pci_quirk_xgene_acs },
	/* Ampere Computing */
	{ PCI_VENDOR_ID_AMPERE, 0xE005, pci_quirk_xgene_acs },
	{ PCI_VENDOR_ID_AMPERE, 0xE006, pci_quirk_xgene_acs },
	{ PCI_VENDOR_ID_AMPERE, 0xE007, pci_quirk_xgene_acs },
	{ PCI_VENDOR_ID_AMPERE, 0xE008, pci_quirk_xgene_acs },
	{ PCI_VENDOR_ID_AMPERE, 0xE009, pci_quirk_xgene_acs },
	{ PCI_VENDOR_ID_AMPERE, 0xE00A, pci_quirk_xgene_acs },
	{ PCI_VENDOR_ID_AMPERE, 0xE00B, pci_quirk_xgene_acs },
	{ PCI_VENDOR_ID_AMPERE, 0xE00C, pci_quirk_xgene_acs },
	/* Broadcom multi-function device */
	{ PCI_VENDOR_ID_BROADCOM, 0x16D7, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0x1750, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0x1751, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0x1752, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0x1760, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0x1761, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0x1762, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0x1763, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_BROADCOM, 0xD714, pci_quirk_brcm_acs },
	/* Loongson PCIe Root Ports */
	{ PCI_VENDOR_ID_LOONGSON, 0x3C09, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x3C19, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x3C29, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x7A09, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x7A19, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x7A29, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x7A39, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x7A49, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x7A59, pci_quirk_loongson_acs },
	{ PCI_VENDOR_ID_LOONGSON, 0x7A69, pci_quirk_loongson_acs },
	/* Amazon Annapurna Labs */
	{ PCI_VENDOR_ID_AMAZON_ANNAPURNA_LABS, 0x0031, pci_quirk_al_acs },
	/* Zhaoxin multi-function devices */
	{ PCI_VENDOR_ID_ZHAOXIN, 0x3038, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_ZHAOXIN, 0x3104, pci_quirk_mf_endpoint_acs },
	{ PCI_VENDOR_ID_ZHAOXIN, 0x9083, pci_quirk_mf_endpoint_acs },
	/* NXP root ports, xx=16, 12, or 08 cores */
	/* LX2xx0A : without security features + CAN-FD */
	{ PCI_VENDOR_ID_NXP, 0x8d81, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8da1, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d83, pci_quirk_nxp_rp_acs },
	/* LX2xx0C : security features + CAN-FD */
	{ PCI_VENDOR_ID_NXP, 0x8d80, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8da0, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d82, pci_quirk_nxp_rp_acs },
	/* LX2xx0E : security features + CAN */
	{ PCI_VENDOR_ID_NXP, 0x8d90, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8db0, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d92, pci_quirk_nxp_rp_acs },
	/* LX2xx0N : without security features + CAN */
	{ PCI_VENDOR_ID_NXP, 0x8d91, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8db1, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d93, pci_quirk_nxp_rp_acs },
	/* LX2xx2A : without security features + CAN-FD */
	{ PCI_VENDOR_ID_NXP, 0x8d89, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8da9, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d8b, pci_quirk_nxp_rp_acs },
	/* LX2xx2C : security features + CAN-FD */
	{ PCI_VENDOR_ID_NXP, 0x8d88, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8da8, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d8a, pci_quirk_nxp_rp_acs },
	/* LX2xx2E : security features + CAN */
	{ PCI_VENDOR_ID_NXP, 0x8d98, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8db8, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d9a, pci_quirk_nxp_rp_acs },
	/* LX2xx2N : without security features + CAN */
	{ PCI_VENDOR_ID_NXP, 0x8d99, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8db9, pci_quirk_nxp_rp_acs },
	{ PCI_VENDOR_ID_NXP, 0x8d9b, pci_quirk_nxp_rp_acs },
	/* Zhaoxin Root/Downstream Ports */
	{ PCI_VENDOR_ID_ZHAOXIN, PCI_ANY_ID, pci_quirk_zhaoxin_pcie_ports_acs },
	/* Wangxun nics */
	{ PCI_VENDOR_ID_WANGXUN, PCI_ANY_ID, pci_quirk_wangxun_nic_acs },
	{ 0 }	/* [한국어] 표의 끝. acs_enabled 가 NULL 이라 아래 루프의 종료 조건이 된다. */
};

/*
 * [한국어]
 * pci_dev_specific_acs_enabled - 이 장치가 원하는 ACS 제어를 제공하는지 판정
 *
 * @dev: 판정할 장치
 * @acs_flags: 호출자가 원하는 ACS 제어 비트마스크
 * @return: >0 모두 제공함, 0 제공하지 못함, -ENOTTY 이 표로는 알 수 없음
 *
 * [무엇을 하는가] 위 표를 훑어 이 장치에 맞는 판정 함수를 찾아 부른다.
 * 판정 함수가 음수를 돌려주면 '이 quirk 는 해당 없음' 이라는 뜻이므로
 * 표를 계속 훑는다. 끝까지 아무도 답하지 않으면 -ENOTTY 를 돌려주고,
 * 호출자는 표준 ACS capability 검사로 넘어간다.
 *
 * [왜 필요한가] 아래 영어 주석대로, 표준 PCIe ACS capability 나 제어를
 * 노출하지 않는 장치가 여기서 자신의 지원 사실을 알릴 수 있게 하기
 * 위해서다. 함수 간 내부 피어투피어를 허용하지 않지만 ACS 를 구현하지
 * 않은 다기능 익스프레스 장치가 여기서 참을 돌려주고 싶어 할 수 있다.
 *
 * [IOMMU 그룹과의 관계] 이 반환값이 IOMMU 계층의 그룹 분리 판정에 쓰인다.
 * 여기서 '제공한다' 고 답하면 그 장치(또는 그 아래)가 별도 그룹이 되어
 * VFIO 패스스루가 가능해진다. 잘못 답하면 격리되지 않은 장치를 격리된
 * 것으로 오인하게 되므로, 표의 각 항목은 벤더 확인이나 문서 근거를 갖는다.
 *
 * 실행 컨텍스트: IOMMU 그룹 판정 경로, 프로세스 문맥.
 * drivers/pci/pci.h 에 선언되어 PCI 코어 안에서 쓰인다.
 *
 * 호출 체인:
 *   PCI 코어의 ACS 판정 -> [pci_dev_specific_acs_enabled] -> 표의 판정 함수
 */
/*
 * pci_dev_specific_acs_enabled - check whether device provides ACS controls
 * @dev:	PCI device
 * @acs_flags:	Bitmask of desired ACS controls
 *
 * Returns:
 *   -ENOTTY:	No quirk applies to this device; we can't tell whether the
 *		device provides the desired controls
 *   0:		Device does not provide all the desired controls
 *   >0:	Device provides all the controls in @acs_flags
 */
int pci_dev_specific_acs_enabled(struct pci_dev *dev, u16 acs_flags)
{
	const struct pci_dev_acs_enabled *i;	/* [한국어] 표를 훑는 반복자. */
	int ret;	/* [한국어] 판정 함수의 반환값. */

	/*
	 * Allow devices that do not expose standard PCIe ACS capabilities
	 * or control to indicate their support here.  Multi-function express
	 * devices which do not allow internal peer-to-peer between functions,
	 * but do not implement PCIe ACS may wish to return true here.
	 */
	for (i = pci_dev_acs_enabled; i->acs_enabled; i++) {	/* [한국어] 표의 끝(acs_enabled 가 NULL)까지 훑는다. */
		if ((i->vendor == dev->vendor ||	/* [한국어] 벤더가 일치하거나 */
		     i->vendor == (u16)PCI_ANY_ID) &&	/* [한국어] 벤더 와일드카드이고, */
		    (i->device == dev->device ||	/* [한국어] 디바이스가 일치하거나 */
		     i->device == (u16)PCI_ANY_ID)) {	/* [한국어] 디바이스 와일드카드이면 이 항목이 이 장치에 해당한다. */
			ret = i->acs_enabled(dev, acs_flags);	/* [한국어] 그 항목의 판정 함수를 부른다. */
			if (ret >= 0)	/* [한국어] 음수가 아니면 확실한 답(제공/미제공)을 얻은 것이다. */
				return ret;	/* [한국어] 그 답을 그대로 돌려준다. */
		}
	}

	return -ENOTTY;	/* [한국어] 표의 어떤 quirk 도 이 장치를 판정하지 못했다. 호출자는 표준 ACS capability 검사로 넘어간다. */
}

/* [한국어] 아래 매크로들은 Intel LPC/PCH 에서 ACS 상당 기능을 '켜기' 위한
 * 레지스터 정의다. 지금까지의 ACS quirk 가 '이미 격리되어 있음을 인정' 하는
 * 쪽이었다면, 이제부터는 백본(내부 버스)의 피어투피어 경로를 실제로 꺼서
 * 격리를 만들어 내는 쪽이다. 각 매크로 위의 영어 주석이 그 이름을 밝힌다. */
/* Config space offset of Root Complex Base Address register */
#define INTEL_LPC_RCBA_REG 0xf0	/* [한국어] RCBA - Root Complex Base Address 가 들어 있는 LPC 브리지의 벤더 전용 오프셋. */
/* 31:14 RCBA address */
#define INTEL_LPC_RCBA_MASK 0xffffc000	/* [한국어] 그 값의 비트 31:14 만 주소이므로 나머지를 떨어내는 마스크. */
/* RCBA Enable */
#define INTEL_LPC_RCBA_ENABLE (1 << 0)	/* [한국어] 그 값의 비트 0 - RCBA 창이 활성인지 나타낸다. */

/* Backbone Scratch Pad Register */
#define INTEL_BSPR_REG 0x1104	/* [한국어] BSPR - Backbone Scratch Pad Register 의 RCBA 창 안 오프셋. */
/* Backbone Peer Non-Posted Disable */
#define INTEL_BSPR_REG_BPNPD (1 << 8)	/* [한국어] 그 레지스터의 비트 8 - 백본 피어 간 Non-Posted 트랜잭션을 막는다. */
/* Backbone Peer Posted Disable */
#define INTEL_BSPR_REG_BPPD  (1 << 9)	/* [한국어] 그 레지스터의 비트 9 - 백본 피어 간 Posted 트랜잭션을 막는다. */

/* Upstream Peer Decode Configuration Register */
#define INTEL_UPDCR_REG 0x1014	/* [한국어] UPDCR - Upstream Peer Decode Configuration Register 의 RCBA 창 안 오프셋. */
/* 5:0 Peer Decode Enable bits */
#define INTEL_UPDCR_REG_MASK 0x3f	/* [한국어] 그 레지스터의 하위 6비트 - 각 비트가 피어 디코드 활성 비트다. 이 비트들을 지우면 피어 간 직접 전달이 꺼진다. */

/*
 * [한국어]
 * pci_quirk_enable_intel_lpc_acs - 백본의 피어 디코드를 꺼서 ACS 상당 격리를 만든다
 *
 * @dev: Intel PCH 루트 포트
 * @return: 0 성공, -EINVAL RCBA 창이 꺼져 있음, -ENOMEM 매핑 실패
 *
 * [무엇을 하는가] 지금까지의 ACS quirk 가 '이미 격리되어 있음을 인정' 하는
 * 쪽이었다면 이 함수는 격리를 실제로 만들어 낸다. PCH 내부 백본에서 포트
 * 사이의 피어 전달을 꺼 버리는 것이다.
 *
 * [두 단계의 검사] 먼저 BSPR(Backbone Scratch Pad Register)을 본다. 아래
 * 영어 주석대로 BSPR 은 피어 사이클을 금지할 수 있지만 soft strap 으로
 * 설정되어 읽기 전용이다. Posted 와 Non-Posted 가 모두 금지되어 있으면
 * 이미 격리된 것이므로 할 일이 없다. 둘 중 하나라도 허용되어 있으면
 * UPDCR 로 포트별 피어 디코드를 꺼야 한다. 그렇게 하면 PCIe ACS 의
 * RR | CR | UF 에 해당하는 격리가 된다.
 *
 * [★ 왜 pci_bus_read_config_dword 를 쓰는가] 아래 영어 주석이 그 이유를
 * 밝힌다. RCBA 는 LPC 브리지(D31:F0)에 있는데, PCH 루트 포트는 D28:F* 이라
 * LPC 보다 먼저 열거된다. 즉 LPC 의 struct pci_dev 가 아직 없어
 * pci_get_slot()/pci_read_config_dword() 를 쓸 수 없다. 그래서 버스와
 * devfn 을 직접 지정해 읽는 저수준 함수를 쓴다.
 *
 * 실행 컨텍스트: ACS 활성화 경로, 프로세스 문맥. ioremap 을 쓰므로
 * 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   pci_quirk_enable_intel_pch_acs() -> [pci_quirk_enable_intel_lpc_acs]
 *     -> pci_bus_read_config_dword() -> ioremap() -> writel()
 */
static int pci_quirk_enable_intel_lpc_acs(struct pci_dev *dev)
{
	u32 rcba, bspr, updcr;	/* [한국어] rcba: RCBA 레지스터 값, bspr: 백본 스크래치 패드, updcr: 상위 피어 디코드 설정. */
	void __iomem *rcba_mem;	/* [한국어] RCBA 가 가리키는 MMIO 창의 매핑 주소. */

	/*
	 * Read the RCBA register from the LPC (D31:F0).  PCH root ports
	 * are D28:F* and therefore get probed before LPC, thus we can't
	 * use pci_get_slot()/pci_read_config_dword() here.
	 */
	pci_bus_read_config_dword(dev->bus, PCI_DEVFN(31, 0),	/* [한국어] 위 영어 주석대로 LPC 브리지가 아직 열거되지 않았을 수 있어, 버스와 devfn(31,0)을 직접 지정해 읽는다. */
				  INTEL_LPC_RCBA_REG, &rcba);	/* [한국어] RCBA 레지스터(오프셋 0xf0). */
	if (!(rcba & INTEL_LPC_RCBA_ENABLE))	/* [한국어] RCBA 창이 활성이 아니면 그 MMIO 영역에 접근할 수 없다. */
		return -EINVAL;	/* [한국어] 잘못된 상태로 보고한다. */

	rcba_mem = ioremap(rcba & INTEL_LPC_RCBA_MASK,	/* [한국어] RCBA 주소(비트 31:14)를 마스크로 뽑아 매핑한다. */
				   PAGE_ALIGN(INTEL_UPDCR_REG));	/* [한국어] UPDCR 오프셋까지 닿도록 페이지 단위로 올림한 크기를 매핑한다. */
	if (!rcba_mem)	/* [한국어] 매핑 실패. */
		return -ENOMEM;	/* [한국어] 메모리 부족으로 보고한다. */

	/*
	 * The BSPR can disallow peer cycles, but it's set by soft strap and
	 * therefore read-only.  If both posted and non-posted peer cycles are
	 * disallowed, we're ok.  If either are allowed, then we need to use
	 * the UPDCR to disable peer decodes for each port.  This provides the
	 * PCIe ACS equivalent of PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF
	 */
	bspr = readl(rcba_mem + INTEL_BSPR_REG);	/* [한국어] BSPR 을 읽는다 - 백본 피어 사이클의 허용 여부가 들어 있다. */
	bspr &= INTEL_BSPR_REG_BPNPD | INTEL_BSPR_REG_BPPD;	/* [한국어] 관심 있는 두 비트만 남긴다. */
	if (bspr != (INTEL_BSPR_REG_BPNPD | INTEL_BSPR_REG_BPPD)) {	/* [한국어] Posted 와 Non-Posted 가 '모두' 금지된 것이 아니라면 아직 피어 전달이 가능하므로 손을 써야 한다. */
		updcr = readl(rcba_mem + INTEL_UPDCR_REG);	/* [한국어] UPDCR 을 읽는다. */
		if (updcr & INTEL_UPDCR_REG_MASK) {	/* [한국어] 피어 디코드 비트가 하나라도 켜져 있으면 */
			pci_info(dev, "Disabling UPDCR peer decodes\n");	/* [한국어] 무엇을 끄는지 남기고 */
			updcr &= ~INTEL_UPDCR_REG_MASK;	/* [한국어] 하위 6비트를 모두 지워 피어 디코드를 끈다. */
			writel(updcr, rcba_mem + INTEL_UPDCR_REG);	/* [한국어] 고친 값을 되쓴다 - 이 쓰기가 실제로 격리를 만들어 낸다. */
		}
	}

	iounmap(rcba_mem);	/* [한국어] 임시 매핑을 해제한다. */
	return 0;	/* [한국어] 성공을 보고한다. */
}

/* [한국어] 아래 두 매크로는 루트 포트의 Miscellaneous Port Configuration
 * 레지스터와 그 안의 Source Validation 상당 비트다. 위 LPC 처리가 피어
 * 전달을 껐다면, 이쪽은 요청자 ID 검증을 켠다. */
/* Miscellaneous Port Configuration register */
#define INTEL_MPC_REG 0xd8	/* [한국어] MPC 레지스터의 Configuration Space 오프셋(루트 포트의 벤더 전용 영역). */
/* MPC: Invalid Receive Bus Number Check Enable */
#define INTEL_MPC_REG_IRBNCE (1 << 26)	/* [한국어] 그 레지스터의 비트 26 - Invalid Receive Bus Number Check Enable. */

/*
 * [한국어]
 * pci_quirk_enable_intel_rp_mpc_acs - 루트 포트의 요청자 ID 검증을 켠다
 *
 * @dev: Intel PCH 루트 포트
 * @return: 없음
 *
 * [무엇을 하는가] 아래 영어 주석대로 MPC 레지스터의 IRBNCE 비트를 켜면
 * PCI ACS 의 Source Validation(PCI_ACS_SV)에 해당하는 검증이 동작한다.
 * 요청자 ID 가 이 브리지의 버스 번호 범위 안에 드는지 확인하는 것이다.
 * 아직 켜져 있지 않으면 켠다.
 *
 * [SV 가 왜 필요한가] 어떤 장치가 남의 요청자 ID 를 사칭하면 IOMMU 가
 * 다른 장치의 주소 변환 표를 쓰게 되어 격리가 무너진다. SV 는 그 사칭을
 * 브리지에서 걸러 낸다.
 *
 * 실행 컨텍스트: ACS 활성화 경로.
 *
 * 호출 체인:
 *   pci_quirk_enable_intel_pch_acs() -> [pci_quirk_enable_intel_rp_mpc_acs]
 *     -> pci_write_config_word()
 */
static void pci_quirk_enable_intel_rp_mpc_acs(struct pci_dev *dev)
{
	u32 mpc;	/* [한국어] MPC 레지스터 값. */

	/*
	 * When enabled, the IRBNCE bit of the MPC register enables the
	 * equivalent of PCI ACS Source Validation (PCI_ACS_SV), which
	 * ensures that requester IDs fall within the bus number range
	 * of the bridge.  Enable if not already.
	 */
	pci_read_config_dword(dev, INTEL_MPC_REG, &mpc);	/* [한국어] MPC 레지스터를 읽는다. */
	if (!(mpc & INTEL_MPC_REG_IRBNCE)) {	/* [한국어] IRBNCE 가 아직 꺼져 있으면 켜야 한다. */
		pci_info(dev, "Enabling MPC IRBNCE\n");	/* [한국어] 무엇을 켜는지 남긴다. */
		mpc |= INTEL_MPC_REG_IRBNCE;	/* [한국어] IRBNCE 비트를 추가한다. */
		pci_write_config_word(dev, INTEL_MPC_REG, mpc);	/* [한국어] 고친 값을 되쓴다. */
	}
}

/*
 * [한국어]
 * pci_quirk_enable_intel_pch_acs - PCH 루트 포트에 ACS 상당 격리를 실제로 켠다
 *
 * @dev: Intel 벤더의 장치
 * @return: 0 (처리했거나 실패해도 0), -ENOTTY 대상이 아님
 *
 * [무엇을 하는가] 두 가지를 조합한다. LPC 쪽에서 백본 피어 디코드를 끄고
 * (RR/CR/UF 상당), 루트 포트 쪽에서 요청자 ID 검증을 켠다(SV 상당). 둘 다
 * 성공하면 PCI_DEV_FLAGS_ACS_ENABLED_QUIRK 를 세워, 앞의
 * pci_quirk_intel_pch_acs() 가 이 장치를 ACS 상당으로 인정하게 만든다.
 *
 * [★ 두 함수의 짝] pci_quirk_intel_pch_acs() 는 '인정' 하는 쪽이고 이
 * 함수는 '켜는' 쪽이다. 인정 쪽이 이 플래그를 조건으로 삼으므로, 실제로
 * 켜지지 않은 장치를 인정하는 일이 없다.
 *
 * [TODO] 아래 영어 주석대로, 외부 노출 포트이거나 신뢰할 수 없는 장치라면
 * PCI_ACS_TB(Translation Blocking) 상당도 해야 하는데 아직 하지 않는다.
 *
 * [LPC 실패 시 0 을 돌려주는 이유] -ENOTTY 가 아니라 0 을 돌려주므로,
 * 호출자는 '이 quirk 가 처리했다(다만 켜지지는 않았다)' 로 받아들이고
 * 다음 항목을 시도하지 않는다. 플래그를 세우지 않았으므로 인정도 받지
 * 못해 안전한 쪽으로 귀결된다.
 *
 * 실행 컨텍스트: ACS 활성화 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_enable_acs() -> [pci_quirk_enable_intel_pch_acs]
 *     -> pci_quirk_enable_intel_lpc_acs() / pci_quirk_enable_intel_rp_mpc_acs()
 */
/*
 * Currently this quirk does the equivalent of
 * PCI_ACS_SV | PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_UF
 *
 * TODO: This quirk also needs to do equivalent of PCI_ACS_TB,
 * if dev->external_facing || dev->untrusted
 */
static int pci_quirk_enable_intel_pch_acs(struct pci_dev *dev)
{
	if (!pci_quirk_intel_pch_acs_match(dev))	/* [한국어] 위 목록의 PCH 루트 포트가 아니면 */
		return -ENOTTY;	/* [한국어] 이 quirk 는 해당 없음을 알린다. */

	if (pci_quirk_enable_intel_lpc_acs(dev)) {	/* [한국어] 백본 피어 디코드 끄기가 실패하면 */
		pci_warn(dev, "Failed to enable Intel PCH ACS quirk\n");	/* [한국어] 경고를 남기고 */
		return 0;	/* [한국어] 플래그를 세우지 않은 채 0 으로 끝낸다 - 인정도 받지 못하므로 안전하다. */
	}

	pci_quirk_enable_intel_rp_mpc_acs(dev);	/* [한국어] 루트 포트의 요청자 ID 검증을 켠다. */

	dev->dev_flags |= PCI_DEV_FLAGS_ACS_ENABLED_QUIRK;	/* [한국어] ★ 이 플래그가 pci_quirk_intel_pch_acs() 의 인정 조건이다. */

	pci_info(dev, "Intel PCH root port ACS workaround enabled\n");	/* [한국어] 회피가 켜졌음을 알린다. */

	return 0;	/* [한국어] 성공을 보고한다. */
}

/*
 * [한국어]
 * pci_quirk_enable_intel_spt_pch_acs - 오프셋이 어긋난 ACS Control 에 직접 써서 켠다
 *
 * @dev: Intel 벤더의 장치
 * @return: 0 성공, -ENOTTY 대상이 아니거나 ACS capability 없음
 *
 * [무엇을 하는가] Sunrise Point 계열은 진짜 ACS 를 구현하고 있으므로
 * '인정' 이 아니라 '켜기' 가 가능하다. 다만 Control 레지스터가 표준 오프셋
 * (+2)이 아니라 +4 에 dword 로 있으므로 그 자리에 쓴다. Capability 에
 * 구현되어 있다고 표시된 기능만 골라 켜는 것이 요령이다.
 *
 * [TB 를 조건부로 켜는 이유] Translation Blocking 은 변환된 주소를 쓰는
 * 요청을 막는다. ATS 를 쓰는 정상 장치에는 해가 되므로 평소에는 켜지
 * 않지만, ATS 가 아예 꺼져 있거나(pci_ats_disabled) 외부에 노출된
 * 포트이거나 신뢰할 수 없는 장치라면 보안을 위해 켠다.
 *
 * 실행 컨텍스트: ACS 활성화 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_enable_acs() -> [pci_quirk_enable_intel_spt_pch_acs]
 *     -> pci_write_config_dword()
 */
static int pci_quirk_enable_intel_spt_pch_acs(struct pci_dev *dev)
{
	int pos;	/* [한국어] ACS capability 의 오프셋. */
	u32 cap, ctrl;	/* [한국어] cap: 무엇이 구현되어 있는지, ctrl: 지금 무엇이 켜져 있는지. */

	if (!pci_quirk_intel_spt_pch_acs_match(dev))	/* [한국어] Sunrise Point 계열이 아니면 */
		return -ENOTTY;	/* [한국어] 해당 없음을 알린다. */

	pos = dev->acs_cap;	/* [한국어] PCI 코어가 찾아 둔 ACS capability 오프셋. */
	if (!pos)	/* [한국어] ACS capability 자체가 없으면 켤 것이 없다. */
		return -ENOTTY;

	pci_read_config_dword(dev, pos + PCI_ACS_CAP, &cap);	/* [한국어] 구현 여부가 담긴 Capability 레지스터를 읽는다. */
	pci_read_config_dword(dev, pos + INTEL_SPT_ACS_CTRL, &ctrl);	/* [한국어] ★ 표준 오프셋이 아니라 +4 에서 Control 을 읽는다. */

	ctrl |= (cap & PCI_ACS_SV);	/* [한국어] Source Validation 이 구현되어 있으면 켠다. */
	ctrl |= (cap & PCI_ACS_RR);	/* [한국어] Request Redirect 도 마찬가지. */
	ctrl |= (cap & PCI_ACS_CR);	/* [한국어] Completion Redirect 도 마찬가지. */
	ctrl |= (cap & PCI_ACS_UF);	/* [한국어] Upstream Forwarding 도 마찬가지. */

	if (pci_ats_disabled() || dev->external_facing || dev->untrusted)	/* [한국어] ATS 가 꺼져 있거나, 외부에 노출된 포트이거나, 신뢰할 수 없는 장치일 때만 */
		ctrl |= (cap & PCI_ACS_TB);	/* [한국어] Translation Blocking 도 켠다 - 평소에는 ATS 를 쓰는 장치를 막아 버리므로 켜지 않는다. */

	pci_write_config_dword(dev, pos + INTEL_SPT_ACS_CTRL, ctrl);	/* [한국어] ★ 표준 오프셋이 아니라 +4 에 Control 을 되쓴다. */

	pci_info(dev, "Intel SPT PCH root port ACS workaround enabled\n");	/* [한국어] 회피가 켜졌음을 알린다. */

	return 0;	/* [한국어] 성공을 보고한다. */
}

/*
 * [한국어]
 * pci_quirk_disable_intel_spt_pch_acs_redir - ACS 재지향 제어만 도로 끈다
 *
 * @dev: Intel 벤더의 장치
 * @return: 0 성공, -ENOTTY 대상이 아니거나 ACS capability 없음
 *
 * [왜 끄는 기능이 필요한가] ACS 의 Request/Completion Redirect 는 피어
 * 트래픽을 상위로 돌려 격리를 만들지만, 그만큼 피어투피어 성능을 죽인다.
 * 격리보다 성능이 중요한 상황(예: 같은 스위치 아래 장치끼리 직접 DMA 를
 * 주고받는 구성)에서는 사용자가 부팅 파라미터로 재지향을 끌 수 있어야
 * 한다. 그 요청이 이 경로로 들어온다.
 * [무엇을 하는가] RR, CR, EC 세 비트를 Control 에서 지운다. 여기서도
 * Control 의 오프셋이 표준과 달라 +4 에 접근한다.
 *
 * 실행 컨텍스트: ACS 재지향 해제 경로.
 *
 * 호출 체인:
 *   pci_dev_specific_disable_acs_redir() -> [pci_quirk_disable_intel_spt_pch_acs_redir]
 *     -> pci_write_config_dword()
 */
static int pci_quirk_disable_intel_spt_pch_acs_redir(struct pci_dev *dev)
{
	int pos;	/* [한국어] ACS capability 의 오프셋. */
	u32 cap, ctrl;	/* [한국어] cap: 구현 여부(읽기만 하고 쓰지는 않는다), ctrl: 현재 설정. */

	if (!pci_quirk_intel_spt_pch_acs_match(dev))	/* [한국어] Sunrise Point 계열이 아니면 */
		return -ENOTTY;	/* [한국어] 해당 없음을 알린다. */

	pos = dev->acs_cap;	/* [한국어] ACS capability 오프셋. */
	if (!pos)	/* [한국어] capability 가 없으면 끌 것도 없다. */
		return -ENOTTY;

	pci_read_config_dword(dev, pos + PCI_ACS_CAP, &cap);	/* [한국어] Capability 를 읽는다. 이 함수에서는 실제로 쓰이지 않지만 대칭을 위해 함께 읽는다. */
	pci_read_config_dword(dev, pos + INTEL_SPT_ACS_CTRL, &ctrl);	/* [한국어] ★ +4 에서 Control 을 읽는다. */

	ctrl &= ~(PCI_ACS_RR | PCI_ACS_CR | PCI_ACS_EC);	/* [한국어] RR(Request Redirect), CR(Completion Redirect), EC(Egress Control)를 지운다. */

	pci_write_config_dword(dev, pos + INTEL_SPT_ACS_CTRL, ctrl);	/* [한국어] ★ +4 에 되쓴다. */

	pci_info(dev, "Intel SPT PCH root port workaround: disabled ACS redirect\n");	/* [한국어] 재지향을 껐음을 알린다. */

	return 0;	/* [한국어] 성공을 보고한다. */
}

/*
 * [한국어] ACS 를 켜거나 재지향을 끄는 연산 표와 그 항목 타입.
 *
 * 앞의 pci_dev_acs_enabled[] 가 '판정' 표였다면 이것은 '조작' 표다.
 * 같은 장치에 대해 켜기와 재지향 해제라는 두 가지 연산을 각각 지정할
 * 수 있다.
 *
 * 순서: 아래 두 디스패처도 위에서부터 훑되 음수(해당 없음)면 다음
 * 항목으로 넘어간다. 그래서 Intel 에 PCI_ANY_ID 항목이 두 개 있어도
 * 앞의 PCH 판정이 해당 없음을 돌려주면 뒤의 SPT PCH 판정이 시도된다.
 */
static const struct pci_dev_acs_ops {	/* [한국어] 항목 타입을 익명 구조체로 정의하면서 곧바로 배열을 만든다. */
	u16 vendor;	/* [한국어] 벤더 ID. PCI_ANY_ID 면 따지지 않는다. */
	u16 device;	/* [한국어] 디바이스 ID. PCI_ANY_ID 면 따지지 않는다. */
	int (*enable_acs)(struct pci_dev *dev);	/* [한국어] ACS 상당 격리를 켜는 함수. NULL 일 수 있다. */
	int (*disable_acs_redir)(struct pci_dev *dev);	/* [한국어] ACS 재지향을 끄는 함수. NULL 일 수 있다(위 PCH 항목이 그렇다). */
} pci_dev_acs_ops[] = {	/* [한국어] 타입 정의와 동시에 표를 정의한다. */
	{ PCI_VENDOR_ID_INTEL, PCI_ANY_ID,	/* [한국어] Intel 벤더 전체 - 일반 PCH 루트 포트용. */
	    .enable_acs = pci_quirk_enable_intel_pch_acs,	/* [한국어] 켜기만 지정한다. 이 방식은 백본 레지스터를 건드리는 것이라 되돌리는 함수가 없다. */
	},
	{ PCI_VENDOR_ID_INTEL, PCI_ANY_ID,	/* [한국어] Intel 벤더 전체 - Sunrise Point 계열용. 위 항목이 해당 없음을 돌려주면 여기로 온다. */
	    .enable_acs = pci_quirk_enable_intel_spt_pch_acs,	/* [한국어] 진짜 ACS Control 에 쓰는 방식이라 켜기와 */
	    .disable_acs_redir = pci_quirk_disable_intel_spt_pch_acs_redir,	/* [한국어] 재지향 끄기가 모두 가능하다. */
	},
};

/*
 * [한국어]
 * pci_dev_specific_enable_acs - 이 장치에 맞는 ACS 켜기 quirk 를 찾아 실행한다
 *
 * @dev: 대상 장치
 * @return: 켜기 함수의 반환값, 또는 맞는 항목이 없으면 -ENOTTY
 *
 * 위 표를 위에서부터 훑어 벤더/디바이스가 맞고 enable_acs 가 있는 항목을
 * 찾아 부른다. 음수(해당 없음)면 다음 항목을 시도한다.
 *
 * 실행 컨텍스트: ACS 초기화 경로, 프로세스 문맥.
 * drivers/pci/pci.h 에 선언되어 PCI 코어 안에서 쓰인다.
 *
 * 호출 체인:
 *   PCI 코어의 ACS 설정 -> [pci_dev_specific_enable_acs] -> 표의 enable_acs
 */
int pci_dev_specific_enable_acs(struct pci_dev *dev)
{
	const struct pci_dev_acs_ops *p;	/* [한국어] 표 항목을 가리키는 포인터. */
	int i, ret;	/* [한국어] i: 표 반복자, ret: 켜기 함수의 반환값. */

	for (i = 0; i < ARRAY_SIZE(pci_dev_acs_ops); i++) {	/* [한국어] 표 전체를 훑는다. 여기서는 ARRAY_SIZE 로 개수를 세므로 종료용 빈 항목이 없다. */
		p = &pci_dev_acs_ops[i];	/* [한국어] 현재 항목. */
		if ((p->vendor == dev->vendor ||	/* [한국어] 벤더가 일치하거나 */
		     p->vendor == (u16)PCI_ANY_ID) &&	/* [한국어] 벤더 와일드카드이고, */
		    (p->device == dev->device ||	/* [한국어] 디바이스가 일치하거나 */
		     p->device == (u16)PCI_ANY_ID) &&	/* [한국어] 디바이스 와일드카드이며, */
		    p->enable_acs) {	/* [한국어] 켜기 함수가 지정되어 있어야 한다. */
			ret = p->enable_acs(dev);	/* [한국어] 그 함수를 부른다. */
			if (ret >= 0)	/* [한국어] 음수가 아니면 이 항목이 처리한 것이다. */
				return ret;	/* [한국어] 결과를 그대로 돌려준다. */
		}
	}

	return -ENOTTY;	/* [한국어] 표의 어떤 항목도 이 장치를 처리하지 못했다. */
}

/*
 * [한국어]
 * pci_dev_specific_disable_acs_redir - ACS 재지향을 끄는 quirk 를 찾아 실행한다
 *
 * @dev: 대상 장치
 * @return: 해제 함수의 반환값, 또는 맞는 항목이 없으면 -ENOTTY
 *
 * pci_dev_specific_enable_acs() 와 구조가 같고 부르는 콜백만 다르다.
 * 사용자가 격리보다 피어투피어 성능을 택했을 때 이 경로로 들어온다.
 *
 * 실행 컨텍스트: 프로세스 문맥. drivers/pci/pci.h 에 선언되어 PCI 코어
 * 안에서 쓰인다.
 *
 * 호출 체인:
 *   PCI 코어 -> [pci_dev_specific_disable_acs_redir] -> 표의 disable_acs_redir
 */
int pci_dev_specific_disable_acs_redir(struct pci_dev *dev)
{
	const struct pci_dev_acs_ops *p;	/* [한국어] 표 항목 포인터. */
	int i, ret;	/* [한국어] i: 표 반복자, ret: 해제 함수의 반환값. */

	for (i = 0; i < ARRAY_SIZE(pci_dev_acs_ops); i++) {	/* [한국어] 표 전체를 훑는다. */
		p = &pci_dev_acs_ops[i];	/* [한국어] 현재 항목. */
		if ((p->vendor == dev->vendor ||	/* [한국어] 벤더가 일치하거나 */
		     p->vendor == (u16)PCI_ANY_ID) &&	/* [한국어] 벤더 와일드카드이고, */
		    (p->device == dev->device ||	/* [한국어] 디바이스가 일치하거나 */
		     p->device == (u16)PCI_ANY_ID) &&	/* [한국어] 디바이스 와일드카드이며, */
		    p->disable_acs_redir) {	/* [한국어] 해제 함수가 지정되어 있어야 한다. 위 표의 첫 항목처럼 없는 경우도 있다. */
			ret = p->disable_acs_redir(dev);	/* [한국어] 그 함수를 부른다. */
			if (ret >= 0)	/* [한국어] 음수가 아니면 이 항목이 처리한 것이다. */
				return ret;	/* [한국어] 결과를 그대로 돌려준다. */
		}
	}

	return -ENOTTY;	/* [한국어] 표의 어떤 항목도 처리하지 못했다. */
}

/*
 * [한국어]
 * quirk_intel_qat_vf_cap - 잘려 나간 capability 목록의 뒷부분을 손으로 복원한다
 *
 * @pdev: Intel DH895xCC QAT 의 가상 함수(device 0x0443)
 * @return: 없음
 *
 * [어떤 하드웨어] Intel DH895xCC QuickAssist Technology(QAT) 가상 함수.
 * [무엇이 문제] 아래 영어 주석대로 capability 목록이 하드웨어에서 일찍
 * 끊긴다. MSI Capability 구조체의 Next Capability 포인터가 PCIe Capability
 * 구조체를 가리켜야 하는데 0 으로 하드와이어되어 목록이 거기서 끝나 버린다.
 * [그대로 두면] 커널이 이 장치에 PCIe Capability 가 없다고 판단한다.
 * 그러면 링크 상태, MPS, 확장 config 공간 등 PCIe 관련 처리가 전부 빠진다.
 * [우회] PCIe Capability 가 있어야 할 자리(0x50)를 직접 읽어 확인하고,
 * 커널 자료구조를 손으로 채워 넣는다. 영어 주석대로 set_pcie_port_type()
 * 과 pci_cfg_space_size_ext() 의 일부를 여기에 펼쳐 쓴 것이다. 하드웨어
 * 버그 때문에 이미 잘못 설정된 값들을 바로잡는 셈이다.
 *
 * [저장 상태까지 만드는 이유] 절전/재개 시 PCIe capability 를 복원하려면
 * 커널이 그 내용을 저장해 두어야 한다. 정상 경로에서는 capability 를
 * 발견하면서 자동으로 만들어지지만, 여기서는 발견 자체가 안 되었으므로
 * 저장 구조체(pci_cap_saved_state)도 직접 할당해 채워 넣는다.
 *
 * 실행 컨텍스트: EARLY 단계. capability 목록을 쓰는 다른 코드보다 먼저
 * 복원되어야 한다. kzalloc(GFP_KERNEL) 을 쓰므로 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_intel_qat_vf_cap]
 *     -> pci_find_capability() -> kzalloc() -> hlist_add_head()
 */
/*
 * The PCI capabilities list for Intel DH895xCC VFs (device ID 0x0443) with
 * QuickAssist Technology (QAT) is prematurely terminated in hardware.  The
 * Next Capability pointer in the MSI Capability Structure should point to
 * the PCIe Capability Structure but is incorrectly hardwired as 0 terminating
 * the list.
 */
static void quirk_intel_qat_vf_cap(struct pci_dev *pdev)
{
	int pos, i = 0, ret;	/* [한국어] pos: capability 오프셋, i: 저장 배열 인덱스, ret: config 읽기 결과. */
	u8 next_cap;	/* [한국어] MSI capability 의 Next Capability 포인터 값. */
	u16 reg16, *cap;	/* [한국어] reg16: 임시 워드 값, cap: 저장 배열을 가리키는 포인터. */
	struct pci_cap_saved_state *state;	/* [한국어] PCIe capability 의 저장 상태 구조체. */

	/* Bail if the hardware bug is fixed */
	if (pdev->pcie_cap || pci_find_capability(pdev, PCI_CAP_ID_EXP))	/* [한국어] 위 영어 주석대로, PCIe capability 가 이미 발견되었다면 하드웨어 버그가 고쳐진 것이므로 손대지 않는다. */
		return;

	/* Bail if MSI Capability Structure is not found for some reason */
	pos = pci_find_capability(pdev, PCI_CAP_ID_MSI);	/* [한국어] 위 영어 주석대로 MSI capability 를 찾는다 - 목록이 끊긴 지점이다. */
	if (!pos)	/* [한국어] MSI capability 조차 없으면 예상한 상황이 아니다. */
		return;

	/*
	 * Bail if Next Capability pointer in the MSI Capability Structure
	 * is not the expected incorrect 0x00.
	 */
	pci_read_config_byte(pdev, pos + 1, &next_cap);	/* [한국어] capability 구조체의 오프셋 +1 이 Next Capability 포인터다(오프셋 +0 은 Capability ID). */
	if (next_cap)	/* [한국어] 위 영어 주석대로 그 값이 예상한 잘못된 값 0x00 이 아니면 다른 상황이므로 건드리지 않는다. */
		return;

	/*
	 * PCIe Capability Structure is expected to be at 0x50 and should
	 * terminate the list (Next Capability pointer is 0x00).  Verify
	 * Capability Id and Next Capability pointer is as expected.
	 * Open-code some of set_pcie_port_type() and pci_cfg_space_size_ext()
	 * to correctly set kernel data structures which have already been
	 * set incorrectly due to the hardware bug.
	 */
	pos = 0x50;	/* [한국어] 위 영어 주석대로 PCIe Capability 구조체는 0x50 에 있을 것으로 예상된다. */
	pci_read_config_word(pdev, pos, &reg16);	/* [한국어] 그 자리의 첫 워드를 읽는다 - 하위 바이트가 Capability ID, 상위 바이트가 Next 포인터다. */
	if (reg16 == (0x0000 | PCI_CAP_ID_EXP)) {	/* [한국어] ID 가 PCIe(PCI_CAP_ID_EXP)이고 Next 가 0x00(목록의 끝)이면 예상대로다. */
		u32 status;	/* [한국어] 확장 config 공간 접근 결과를 담을 변수. */
/* [한국어] 저장할 PCIe 제어 레지스터 개수. 다른 헤더에서 정의되지 않았을
 * 때만 여기서 정의한다 - 이 quirk 는 PCI 코어 내부 상수를 흉내 내야 하는데
 * 그 상수가 외부에 공개되어 있지 않기 때문이다. */
#ifndef PCI_EXP_SAVE_REGS
#define PCI_EXP_SAVE_REGS     7
#endif
		int size = PCI_EXP_SAVE_REGS * sizeof(u16);	/* [한국어] 저장 영역의 바이트 크기 - 워드 7개. */

		pdev->pcie_cap = pos;	/* [한국어] 커널이 기억하는 PCIe capability 오프셋을 채운다. 이 한 줄로 pci_is_pcie() 가 참이 된다. */
		pci_read_config_word(pdev, pos + PCI_EXP_FLAGS, &reg16);	/* [한국어] PCIe Capabilities 레지스터를 읽어 */
		pdev->pcie_flags_reg = reg16;	/* [한국어] 장치 타입/버전 정보를 채운다. set_pcie_port_type() 이 하는 일의 일부다. */
		pci_read_config_word(pdev, pos + PCI_EXP_DEVCAP, &reg16);	/* [한국어] Device Capabilities 레지스터를 읽어 */
		pdev->pcie_mpss = reg16 & PCI_EXP_DEVCAP_PAYLOAD;	/* [한국어] 지원하는 최대 페이로드 크기(MPS)를 채운다. */

		pdev->cfg_size = PCI_CFG_SPACE_EXP_SIZE;	/* [한국어] PCIe 이므로 확장 config 공간(4KB)을 쓸 수 있다고 가정하고 크기를 늘린다. */
		ret = pci_read_config_dword(pdev, PCI_CFG_SPACE_SIZE, &status);	/* [한국어] 실제로 확장 영역에 접근되는지 확인한다. PCI_CFG_SPACE_SIZE(256)는 확장 영역의 첫 오프셋이다. */
		if ((ret != PCIBIOS_SUCCESSFUL) || (PCI_POSSIBLE_ERROR(status)))	/* [한국어] 읽기가 실패했거나 응답이 모두 1(장치 없음)이면 */
			pdev->cfg_size = PCI_CFG_SPACE_SIZE;	/* [한국어] 확장 영역이 없는 것이므로 크기를 표준 256바이트로 되돌린다. pci_cfg_space_size_ext() 가 하는 검사다. */

		if (pci_find_saved_cap(pdev, PCI_CAP_ID_EXP))	/* [한국어] 저장 상태가 이미 있으면(재적용된 경우) 중복으로 만들지 않는다. */
			return;

		/* Save PCIe cap */
		state = kzalloc(sizeof(*state) + size, GFP_KERNEL);	/* [한국어] 저장 상태 구조체와 데이터 영역을 한 번에 할당한다. */
		if (!state)	/* [한국어] 할당 실패 시 여기까지 고친 것만 남기고 끝낸다. */
			return;

		state->cap.cap_nr = PCI_CAP_ID_EXP;	/* [한국어] 이 저장 상태가 PCIe capability 의 것임을 표시한다. */
		state->cap.cap_extended = 0;	/* [한국어] 표준 capability(확장 capability 가 아님)임을 표시한다. */
		state->cap.size = size;	/* [한국어] 저장 데이터의 크기. */
		cap = (u16 *)&state->cap.data[0];	/* [한국어] 데이터 영역을 워드 배열로 본다. */
		pcie_capability_read_word(pdev, PCI_EXP_DEVCTL, &cap[i++]);	/* [한국어] Device Control 을 저장한다. i 가 후위 증가하며 배열을 채운다. */
		pcie_capability_read_word(pdev, PCI_EXP_LNKCTL, &cap[i++]);	/* [한국어] Link Control 을 저장한다. */
		pcie_capability_read_word(pdev, PCI_EXP_SLTCTL, &cap[i++]);	/* [한국어] Slot Control 을 저장한다. */
		pcie_capability_read_word(pdev, PCI_EXP_RTCTL,  &cap[i++]);	/* [한국어] Root Control 을 저장한다. */
		pcie_capability_read_word(pdev, PCI_EXP_DEVCTL2, &cap[i++]);	/* [한국어] Device Control 2 를 저장한다. */
		pcie_capability_read_word(pdev, PCI_EXP_LNKCTL2, &cap[i++]);	/* [한국어] Link Control 2 를 저장한다. */
		pcie_capability_read_word(pdev, PCI_EXP_SLTCTL2, &cap[i++]);	/* [한국어] Slot Control 2 를 저장한다 - 여기까지 7개로 PCI_EXP_SAVE_REGS 와 맞는다. */
		hlist_add_head(&state->next, &pdev->saved_cap_space);	/* [한국어] 완성한 저장 상태를 이 장치의 저장 목록에 매단다. 이후 절전/재개가 이것을 쓴다. */
	}
}
/* [한국어] QAT 가상 함수에 EARLY 단계로 등록한다 - capability 를 쓰는
 * 다른 코드보다 먼저 복원되어야 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x443, quirk_intel_qat_vf_cap);

/*
 * [한국어]
 * quirk_no_flr - FLR 을 걸면 멈추는 장치를 표시한다
 *
 * @dev: 아래 등록표의 장치들
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석이 대상을 나열한다. FLR(Function Level
 * Reset)을 걸면 이 장치들이 멈춘다. FLR 은 PCIe 가 정의한 표준 리셋
 * 방법이고 장치가 capability 로 지원을 광고하지만, 실제로는 그 리셋을
 * 견디지 못하는 것이다.
 * [그대로 두면] 리셋을 시도한 순간 장치가 응답하지 않는다.
 * [우회] PCI_DEV_FLAGS_NO_FLR_RESET 을 세워 커널이 이 장치에 FLR 을
 * 쓰지 않게 한다. 다른 리셋 방법(PM 리셋, 버스 리셋)이 있으면 그쪽으로
 * 물러난다.
 *
 * [★ NVMe 와의 대비] 앞서 본 nvme_disable_and_flr() 은 'FLR 전에 준비를
 * 하면 쓸 수 있다' 는 쪽이었고, 이것은 '아예 쓰지 말라' 는 쪽이다. 같은
 * FLR 문제라도 대응이 다르다. 아래 목록에 NVMe 컨트롤러는 없다.
 *
 * 실행 컨텍스트: EARLY 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() 또는 quirk_no_flr_snet() -> [quirk_no_flr]
 */
/*
 * FLR may cause the following to devices to hang:
 *
 * AMD Starship/Matisse HD Audio Controller 0x1487
 * AMD Starship USB 3.0 Host Controller 0x148c
 * AMD Matisse USB 3.0 Host Controller 0x149c
 * AMD Neural Processing Unit 0x1502 0x17f0
 * Intel 82579LM Gigabit Ethernet Controller 0x1502
 * Intel 82579V Gigabit Ethernet Controller 0x1503
 * Mediatek MT7922 802.11ax PCI Express Wireless Network Adapter
 */
static void quirk_no_flr(struct pci_dev *dev)
{
	dev->dev_flags |= PCI_DEV_FLAGS_NO_FLR_RESET;	/* [한국어] 이 장치에 FLR 을 쓰지 말라는 표시. */
}
/* [한국어] 위 영어 주석이 나열한 장치들에 EARLY 단계로 등록한다.
 * AMD 오디오/USB 컨트롤러와 NPU, Intel 기가비트 NIC, Mediatek 무선 어댑터다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_AMD, 0x1487, quirk_no_flr);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_AMD, 0x148c, quirk_no_flr);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_AMD, 0x149c, quirk_no_flr);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_AMD, 0x7901, quirk_no_flr);
/* [한국어] 이어지는 등록 줄: 0x1502, 0x17f0, 0x1503. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_AMD, 0x1502, quirk_no_flr);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_AMD, 0x17f0, quirk_no_flr);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x1502, quirk_no_flr);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x1503, quirk_no_flr);
/* [한국어] 이어지는 등록 줄: 0x0616. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_MEDIATEK, 0x0616, quirk_no_flr);

/*
 * [한국어]
 * quirk_no_flr_snet - SolidRun SNET DPU 의 특정 리비전에만 FLR 을 막는다
 *
 * @dev: SolidRun 0x1000
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로 리비전 0x1 에서 FLR 이 장치를 멈춘다.
 * [리비전으로 좁히는 이유] DECLARE 매크로는 리비전으로 매칭할 수 없으므로,
 * 함수 안에서 확인해 그 리비전에만 적용한다. 이후 리비전에서는 고쳐졌다는
 * 뜻이다.
 *
 * 실행 컨텍스트: EARLY 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_no_flr_snet] -> quirk_no_flr()
 */
/* FLR may cause the SolidRun SNET DPU (rev 0x1) to hang */
static void quirk_no_flr_snet(struct pci_dev *dev)
{
	if (dev->revision == 0x1)	/* [한국어] 리비전 0x1 에만 문제가 있다. */
		quirk_no_flr(dev);	/* [한국어] 그때만 FLR 금지 플래그를 세운다. */
}
/* [한국어] SolidRun 0x1000 에 EARLY 단계로 등록한다. 리비전 판별은 함수 안에서 한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SOLIDRUN, 0x1000, quirk_no_flr_snet);

/*
 * [한국어]
 * quirk_no_ext_tags - Extended Tag 를 다루지 못하는 장치가 있으면 그 계층 전체에서 끈다
 *
 * @pdev: 아래 등록표의 장치들
 * @return: 없음
 *
 * [Extended Tag 란] PCIe 요청에는 완료를 짝지을 Tag 가 붙는다. 기본은
 * 5비트(32개)이고, Extended Tag 를 켜면 8비트(256개)가 되어 동시에 더 많은
 * 요청을 띄울 수 있다. 대역폭이 중요한 장치에 유리하다.
 * [무엇이 문제] 아래 등록표의 장치들은 8비트 Tag 가 붙은 완료를 제대로
 * 처리하지 못한다. 로그 메시지가 그 사정을 그대로 말한다.
 * [그대로 두면] 그 장치와의 전송이 깨진다.
 * [우회] 이 장치가 속한 호스트 브리지에 no_ext_tags 를 세우고,
 * pci_walk_bus() 로 그 브리지 아래의 모든 장치를 다시 훑어 Extended Tag
 * 설정을 다시 계산하게 한다.
 *
 * [★ 왜 장치 하나가 아니라 계층 전체인가] Extended Tag 는 요청자와
 * 완료자 사이의 약속이라, 문제 있는 장치가 하나라도 있으면 그 장치가
 * 완료를 받을 수 있는 경로 전체에서 꺼야 안전하다. 그래서 개별 장치가
 * 아니라 호스트 브리지 단위로 끄고 이미 설정된 장치들도 다시 훑는다.
 *
 * 실행 컨텍스트: EARLY 단계. pci_walk_bus() 를 쓰므로 프로세스 문맥이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_no_ext_tags]
 *     -> pci_find_host_bridge() -> pci_walk_bus(pci_configure_extended_tags)
 */
static void quirk_no_ext_tags(struct pci_dev *pdev)
{
	struct pci_host_bridge *bridge = pci_find_host_bridge(pdev->bus);	/* [한국어] 이 장치가 매달린 호스트 브리지를 찾는다 - 설정을 끌 범위의 최상위다. */

	if (!bridge)	/* [한국어] 호스트 브리지를 찾지 못하면 범위를 정할 수 없다. */
		return;

	bridge->no_ext_tags = 1;	/* [한국어] 이 브리지 아래에서는 Extended Tag 를 쓰지 말라고 표시한다. */
	pci_info(pdev, "disabling Extended Tags (this device can't handle them)\n");	/* [한국어] 왜 끄는지 알린다. */

	pci_walk_bus(bridge->bus, pci_configure_extended_tags, NULL);	/* [한국어] 브리지 아래의 모든 장치를 훑으며 Extended Tag 설정을 다시 계산한다. 이미 켜 둔 장치가 있으면 여기서 꺼진다. */
}
/* [한국어] Extended Tag 를 다루지 못하는 장치 9종에 EARLY 단계로 등록한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_3WARE, 0x1004, quirk_no_ext_tags);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_3WARE, 0x1005, quirk_no_ext_tags);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, 0x0132, quirk_no_ext_tags);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, 0x0140, quirk_no_ext_tags);
/* [한국어] 이어지는 등록 줄: 0x0141, 0x0142, 0x0144, 0x0420. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, 0x0141, quirk_no_ext_tags);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, 0x0142, quirk_no_ext_tags);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, 0x0144, quirk_no_ext_tags);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, 0x0420, quirk_no_ext_tags);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_SERVERWORKS, 0x0422, quirk_no_ext_tags);

/* [한국어] 아래 quirk 는 ATS(Address Translation Services)를 다루므로
 * ATS 지원이 빌드에 있을 때만 컴파일한다. */
#ifdef CONFIG_PCI_ATS
/*
 * [한국어]
 * quirk_no_ats - ATS 를 쓰지 못하게 capability 를 지운다
 *
 * @pdev: 아래 등록표의 장치들
 * @return: 없음
 *
 * [ATS 란] Address Translation Services - 장치가 IOMMU 에 주소 변환을
 * 미리 물어 캐시(ATC)에 담아 두고, DMA 를 보낼 때 변환된 주소를 직접
 * 쓰는 기능이다. IOMMU 를 거치는 지연을 줄여 준다.
 * [무엇이 문제] 아래 영어 주석대로, 어떤 장치는 ATS 를 쓰려면 드라이버가
 * 추가로 설정을 해 줘야 한다. 그런데 커널은 드라이버가 로드되기 전에
 * ATS 를 켜 버린다. 즉 준비되지 않은 상태로 ATS 가 활성화된다.
 * [그대로 두면] 잘못된 변환 주소로 DMA 가 나가 메모리가 깨질 수 있다.
 * [우회] pdev->ats_cap 을 0 으로 만들어 커널이 이 장치에 ATS capability 가
 * 없다고 보게 한다. 하드웨어는 건드리지 않고 커널의 인식만 지운다.
 *
 * 실행 컨텍스트: 아래 등록 단계 참조.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_no_ats]
 */
static void quirk_no_ats(struct pci_dev *pdev)
{
	pci_info(pdev, "disabling ATS\n");	/* [한국어] 무엇을 끄는지 남긴다. */
	pdev->ats_cap = 0;	/* [한국어] 커널이 기억하는 ATS capability 오프셋을 지운다. 이후 ATS 관련 코드가 이 장치를 건너뛴다. */
}

/*
 * [한국어]
 * quirk_amd_harvest_no_ats - AMD GPU 계열에서 ATS 를 끈다(일부 모델은 서브시스템까지 확인)
 *
 * @pdev: 아래 등록표의 AMD/ATI GPU
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석이 quirk_no_ats() 와 공유하는 사정을 밝힌다.
 * 이 장치들은 ATS 를 쓰려면 드라이버의 추가 설정이 필요한데, 커널은
 * 드라이버가 로드되기 전에 ATS 를 켜 버린다.
 * [우회] ATS capability 인식을 지운다.
 *
 * [0x15d8 만 조건이 붙는 이유] Raven 플랫폼의 내장 GPU(0x15d8)는 같은
 * device ID 로 여러 변종이 나오는데, 그중 특정 리비전(0xcf)과 특정
 * 서브시스템 조합에서만 문제가 확인되었다. 그래서 그 모델만 리비전과
 * 서브시스템 ID 로 좁혀 적용하고, 나머지 등록 대상(Stoney, Iceland,
 * Navi10/14)은 조건 없이 끈다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_amd_harvest_no_ats] -> quirk_no_ats()
 */
/*
 * Some devices require additional driver setup to enable ATS.  Don't use
 * ATS for those devices as ATS will be enabled before the driver has had a
 * chance to load and configure the device.
 */
static void quirk_amd_harvest_no_ats(struct pci_dev *pdev)
{
	if (pdev->device == 0x15d8) {	/* [한국어] Raven 내장 GPU 만 추가 조건을 따진다. */
		if (pdev->revision == 0xcf &&	/* [한국어] 리비전 0xcf 이고 */
		    pdev->subsystem_vendor == 0xea50 &&	/* [한국어] 서브벤더가 0xea50 이며 */
		    (pdev->subsystem_device == 0xce19 ||	/* [한국어] 서브디바이스가 0xce19 이거나 */
		     pdev->subsystem_device == 0xcc10 ||	/* [한국어] 0xcc10 이거나 */
		     pdev->subsystem_device == 0xcc08))	/* [한국어] 0xcc08 인 조합에서만 문제가 확인되었다. */
			quirk_no_ats(pdev);	/* [한국어] 그때만 ATS 를 끈다. */
	} else {
		quirk_no_ats(pdev);	/* [한국어] 조건 없이 ATS 를 끈다. */
	}
}

/* [한국어] 아래 등록표는 각 줄 위의 영어 주석이 모델명을 밝힌다.
 * 0x15d8(Raven)만 함수 안에서 추가 조건을 따진다. */
/* AMD Stoney platform GPU */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x98e4, quirk_amd_harvest_no_ats);
/* AMD Iceland dGPU */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x6900, quirk_amd_harvest_no_ats);
/* AMD Navi10 dGPU */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x7310, quirk_amd_harvest_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x7312, quirk_amd_harvest_no_ats);
/* [한국어] 이어지는 등록 줄: 0x7318, 0x7319, 0x731a, 0x731b. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x7318, quirk_amd_harvest_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x7319, quirk_amd_harvest_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x731a, quirk_amd_harvest_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x731b, quirk_amd_harvest_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x731e, quirk_amd_harvest_no_ats);
/* [한국어] 이어지는 등록 줄: 0x731f, 0x7340, 0x7341. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x731f, quirk_amd_harvest_no_ats);
/* AMD Navi14 dGPU */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x7340, quirk_amd_harvest_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x7341, quirk_amd_harvest_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x7347, quirk_amd_harvest_no_ats);
/* [한국어] 이어지는 등록 줄: 0x734f, 0x15d8. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x734f, quirk_amd_harvest_no_ats);
/* AMD Raven platform iGPU */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ATI, 0x15d8, quirk_amd_harvest_no_ats);

/*
 * [한국어]
 * quirk_intel_e2000_no_ats - C0 이전 리비전의 IPU E2000 에서 ATS 를 끈다
 *
 * @pdev: Intel IPU E2000 계열
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, C0 이전 리비전은 ATS Invalidate
 * Request 메시지 본문의 엔디언을 잘못 구현했다. IOMMU 가 보내는 무효화
 * 요청의 바이트 순서가 스펙과 다르다는 뜻이다.
 * [그대로 두면] 무효화가 엉뚱한 주소에 적용되어 장치의 주소 변환 캐시가
 * 낡은 값을 계속 쓰게 된다 - 메모리 손상으로 이어질 수 있다.
 * [우회] 그 리비전에서는 ATS 자체를 끈다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_intel_e2000_no_ats] -> quirk_no_ats()
 */
/*
 * Intel IPU E2000 revisions before C0 implement incorrect endianness
 * in ATS Invalidate Request message body. Disable ATS for those devices.
 */
static void quirk_intel_e2000_no_ats(struct pci_dev *pdev)
{
	if (pdev->revision < 0x20)	/* [한국어] 리비전 0x20(C0) 미만만 결함이 있다. */
		quirk_no_ats(pdev);	/* [한국어] 그때만 ATS 를 끈다. */
}
/* [한국어] IPU E2000 계열 9종에 FINAL 단계로 등록한다. 리비전 판별은 함수 안에서 한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1451, quirk_intel_e2000_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1452, quirk_intel_e2000_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1453, quirk_intel_e2000_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1454, quirk_intel_e2000_no_ats);
/* [한국어] 이어지는 등록 줄: 0x1455, 0x1457, 0x1459, 0x145a. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1455, quirk_intel_e2000_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1457, quirk_intel_e2000_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x1459, quirk_intel_e2000_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x145a, quirk_intel_e2000_no_ats);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_INTEL, 0x145c, quirk_intel_e2000_no_ats);
/* [한국어] CONFIG_PCI_ATS 블록의 끝. */
#endif /* CONFIG_PCI_ATS */

/*
 * [한국어]
 * quirk_fsl_no_msi - Freescale PCIe 루트 포트에서 MSI 를 끈다
 *
 * @pdev: Freescale 벤더의 장치
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로 Freescale PCIe 는 RC(Root Complex) 모드에서
 * MSI 를 지원하지 않는다. 같은 IP 가 엔드포인트 모드로도 쓰이므로, 지금
 * 어느 모드인지 확인해야 한다.
 * [그대로 두면] 루트 포트에 MSI 를 켜도 인터럽트가 오지 않는다.
 * [우회] PCIe 타입이 루트 포트일 때만 no_msi 를 세운다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_fsl_no_msi]
 */
/* Freescale PCIe doesn't support MSI in RC mode */
static void quirk_fsl_no_msi(struct pci_dev *pdev)
{
	if (pci_pcie_type(pdev) == PCI_EXP_TYPE_ROOT_PORT)	/* [한국어] 루트 포트일 때만 - 엔드포인트 모드에서는 MSI 가 동작한다. */
		pdev->no_msi = 1;	/* [한국어] 이 장치에서 MSI 사용을 금지한다. */
}
/* [한국어] Freescale 벤더의 모든 장치를 FINAL 단계에서 검사한다.
 * 루트 포트인지는 함수 안에서 판정한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_FREESCALE, PCI_ANY_ID, quirk_fsl_no_msi);

/*
 * [한국어]
 * pci_create_device_link - 같은 슬롯의 두 함수 사이에 전원 의존성 링크를 만든다
 *
 * @pdev: 소비자(consumer) 쪽 장치 - 다른 함수가 켜져 있어야 동작하는 쪽
 * @consumer: 소비자여야 할 함수 번호(이 값이 아니면 아무것도 하지 않는다)
 * @supplier: 공급자(supplier) 함수 번호 - 같은 슬롯 안에서 찾는다
 * @class: 공급자가 가져야 할 클래스 코드(오인 방지용)
 * @class_shift: 그 클래스를 비교할 때 밀 비트 수
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, 스펙이 허용하지 않는데도 어떤 다기능
 * 장치는 한 함수가 다른 함수에 의존한다. 소비자가 D0(동작 상태)에서
 * 제대로 동작하려면 공급자도 D0 이어야 한다. 대표적인 예가 GPU 에 딸린
 * HDA 오디오 컨트롤러다 - GPU 가 절전에 들어가면 오디오도 죽는다.
 * [그대로 두면] 전원관리 코어가 두 함수를 독립적으로 다뤄, 공급자를 먼저
 * 재우고 소비자를 살려 두는 순서가 생길 수 있다.
 * [우회] device_link_add() 로 소비자에서 공급자로 향하는 장치 링크를
 * 만들어 전원 순서를 강제한다.
 *
 * [runtime PM 을 허용하는 이유] 영어 주석대로, 링크를 만들면 소비자가
 * 공급자를 계속 깨워 둘 수 있다. 그래서 소비자 쪽 runtime PM 을 기본
 * 허용해, 소비자가 놀 때는 스스로 잠들고 공급자도 함께 잠들 수 있게 한다.
 *
 * [클래스까지 확인하는 이유] 같은 슬롯의 그 함수 번호에 엉뚱한 장치가
 * 있을 수 있다. 공급자가 기대한 클래스(예: 디스플레이)가 아니면 링크를
 * 만들지 않는다.
 *
 * 실행 컨텍스트: FINAL 단계, 프로세스 문맥.
 *
 * 호출 체인:
 *   quirk_gpu_hda() / quirk_gpu_usb() / quirk_gpu_usb_typec_ucsi()
 *     -> [pci_create_device_link] -> device_link_add() -> pm_runtime_allow()
 */
/*
 * Although not allowed by the spec, some multi-function devices have
 * dependencies of one function (consumer) on another (supplier).  For the
 * consumer to work in D0, the supplier must also be in D0.  Create a
 * device link from the consumer to the supplier to enforce this
 * dependency.  Runtime PM is allowed by default on the consumer to prevent
 * it from permanently keeping the supplier awake.
 */
static void pci_create_device_link(struct pci_dev *pdev, unsigned int consumer,
				   unsigned int supplier, unsigned int class,
				   unsigned int class_shift)
{
	struct pci_dev *supplier_pdev;	/* [한국어] 찾아낸 공급자 장치. */

	if (PCI_FUNC(pdev->devfn) != consumer)	/* [한국어] 이 장치가 소비자 함수 번호가 아니면 대상이 아니다. */
		return;

	supplier_pdev = pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus),	/* [한국어] 같은 도메인, 같은 버스, 같은 슬롯의 공급자 함수를 찾는다. */
				pdev->bus->number,	/* [한국어] 버스 번호는 소비자와 같다. */
				PCI_DEVFN(PCI_SLOT(pdev->devfn), supplier));	/* [한국어] 슬롯도 같고 함수 번호만 supplier 로 바꾼다. */
	if (!supplier_pdev || (supplier_pdev->class >> class_shift) != class) {	/* [한국어] 공급자가 없거나 기대한 클래스가 아니면 잘못 짚은 것이다. */
		pci_dev_put(supplier_pdev);	/* [한국어] 조회로 올린 참조수를 놓는다. NULL 이어도 안전하다. */
		return;
	}

	if (device_link_add(&pdev->dev, &supplier_pdev->dev,	/* [한국어] 소비자에서 공급자로 향하는 장치 링크를 만든다. */
			    DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME))	/* [한국어] STATELESS 는 드라이버 바인딩 상태와 무관하게 유지한다는 뜻이고, PM_RUNTIME 은 런타임 전원 순서를 강제한다는 뜻이다. */
		pci_info(pdev, "D0 power state depends on %s\n",	/* [한국어] 어느 장치에 의존하는지 남긴다. */
			 pci_name(supplier_pdev));
	else
		pci_err(pdev, "Cannot enforce power dependency on %s\n",	/* [한국어] 의존성을 강제할 수 없다고 오류로 남긴다. */
			pci_name(supplier_pdev));

	pm_runtime_allow(&pdev->dev);	/* [한국어] 소비자 쪽 runtime PM 을 허용한다 - 위 영어 주석대로 소비자가 공급자를 영원히 깨워 두지 않게 하기 위해서다. */
	pci_dev_put(supplier_pdev);	/* [한국어] 조회로 올린 참조수를 놓는다. */
}

/*
 * [한국어]
 * quirk_gpu_hda - GPU 에 딸린 HDA 오디오 컨트롤러의 전원 의존성을 건다
 *
 * @hda: GPU 슬롯의 HD Audio 클래스 장치
 * @return: 없음
 *
 * [배경] 아래 영어 주석대로, 요즘 GPU 는 디스플레이로 오디오를 흘려보내기
 * 위해 HDA 컨트롤러를 함께 담고 있다. 그 HDA 는 같은 슬롯의 함수 1 로
 * 나타나고, GPU 본체(VGA)는 함수 0 이다. HDA 는 GPU 가 켜져 있어야 동작한다.
 * [우회] 함수 1(HDA)에서 함수 0(디스플레이 클래스)으로 장치 링크를 만든다.
 * class_shift 16 은 클래스 코드의 베이스 클래스만 비교하겠다는 뜻이다.
 *
 * 실행 컨텍스트: FINAL 단계, 클래스 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_gpu_hda] -> pci_create_device_link()
 */
/*
 * Create device link for GPUs with integrated HDA controller for streaming
 * audio to attached displays.
 */
static void quirk_gpu_hda(struct pci_dev *hda)
{
	pci_create_device_link(hda, 1, 0, PCI_BASE_CLASS_DISPLAY, 16);	/* [한국어] 소비자=함수 1(HDA), 공급자=함수 0 이며 그 클래스가 디스플레이여야 한다. */
}
/* [한국어] ATI/AMD/NVIDIA 의 HD Audio 클래스 장치에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_ATI, PCI_ANY_ID,
			      PCI_CLASS_MULTIMEDIA_HD_AUDIO, 8, quirk_gpu_hda);
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_AMD, PCI_ANY_ID,
			      PCI_CLASS_MULTIMEDIA_HD_AUDIO, 8, quirk_gpu_hda);
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID,
			      PCI_CLASS_MULTIMEDIA_HD_AUDIO, 8, quirk_gpu_hda);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/*
 * [한국어]
 * quirk_gpu_usb - GPU 에 딸린 xHCI USB 컨트롤러의 전원 의존성을 건다
 *
 * @usb: GPU 슬롯의 USB 클래스 장치
 * @return: 없음
 *
 * [배경] 아래 영어 주석대로, 일부 GPU 는 USB-C 출력을 위해 xHCI 호스트
 * 컨트롤러를 함께 담고 있다(NVIDIA 의 VirtualLink 계열이 대표적이다).
 * 그것은 같은 슬롯의 함수 2 로 나타나며 GPU 본체에 의존한다.
 * [우회] 함수 2 에서 함수 0(VGA)으로 장치 링크를 만든다.
 *
 * 실행 컨텍스트: FINAL 단계, 클래스 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_gpu_usb] -> pci_create_device_link()
 */
/*
 * Create device link for GPUs with integrated USB xHCI Host
 * controller to VGA.
 */
static void quirk_gpu_usb(struct pci_dev *usb)
{
	pci_create_device_link(usb, 2, 0, PCI_BASE_CLASS_DISPLAY, 16);	/* [한국어] 소비자=함수 2(USB), 공급자=함수 0(디스플레이). */
}
/* [한국어] NVIDIA 와 ATI 의 USB 클래스 장치에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID,
			      PCI_CLASS_SERIAL_USB, 8, quirk_gpu_usb);
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_ATI, PCI_ANY_ID,
			      PCI_CLASS_SERIAL_USB, 8, quirk_gpu_usb);

/*
 * [한국어]
 * quirk_gpu_usb_typec_ucsi - GPU 에 딸린 Type-C UCSI 컨트롤러의 전원 의존성을 건다
 *
 * @ucsi: GPU 슬롯의 UCSI 컨트롤러
 * @return: 없음
 *
 * [배경] 아래 영어 주석대로, GPU 의 Type-C 포트를 관리하는 UCSI
 * (USB Type-C Connector System Software Interface) 컨트롤러가 함수 3 으로
 * 나타난다. 이것도 GPU 본체에 의존한다.
 * [클래스 코드가 없는 문제] 영어 주석대로 PCI 로 노출되는 UCSI 장치를
 * 위한 클래스 코드가 아직 정의되어 있지 않다. 그래서 지금은 UNKNOWN
 * 클래스(0x0c80)를 쓰고, 표준 클래스 코드가 생기면 갱신할 예정이다.
 * [우회] 함수 3 에서 함수 0(VGA)으로 장치 링크를 만든다.
 *
 * 실행 컨텍스트: FINAL 단계, 클래스 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_gpu_usb_typec_ucsi] -> pci_create_device_link()
 */
/*
 * Create device link for GPUs with integrated Type-C UCSI controller
 * to VGA. Currently there is no class code defined for UCSI device over PCI
 * so using UNKNOWN class for now and it will be updated when UCSI
 * over PCI gets a class code.
 */
#define PCI_CLASS_SERIAL_UNKNOWN	0x0c80	/* [한국어] 위 영어 주석대로 UCSI 용 표준 클래스 코드가 없어 임시로 쓰는 값이다. 0x0c80 은 Serial Bus Controller 베이스 클래스의 '기타' 서브클래스다. */
static void quirk_gpu_usb_typec_ucsi(struct pci_dev *ucsi)
{
	pci_create_device_link(ucsi, 3, 0, PCI_BASE_CLASS_DISPLAY, 16);	/* [한국어] 소비자=함수 3(UCSI), 공급자=함수 0(디스플레이). */
}
/* [한국어] NVIDIA 와 ATI 의 UNKNOWN 시리얼 클래스 장치에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID,
			      PCI_CLASS_SERIAL_UNKNOWN, 8,
			      quirk_gpu_usb_typec_ucsi);
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_ATI, PCI_ANY_ID,
			      PCI_CLASS_SERIAL_UNKNOWN, 8,
			      quirk_gpu_usb_typec_ucsi);
/* [한국어] 위 블록 주석의 설명이 이어지는 줄들에도 그대로 적용된다. */

/*
 * [한국어]
 * quirk_nvidia_hda - BIOS 가 꺼 둔 NVIDIA GPU 의 내장 HDA 컨트롤러를 켠다
 *
 * @gpu: NVIDIA 디스플레이 클래스 장치
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석과 그 링크대로, 일부 BIOS 가 NVIDIA GPU 의
 * 내장 HDA 컨트롤러를 비활성 상태로 두고 넘긴다.
 * [그대로 두면] HDMI/DisplayPort 오디오를 쓸 수 없다.
 * [우회] 벤더 전용 레지스터 0x488 의 비트 25 를 켜서 HDA 컨트롤러를
 * 되살리고, GPU 가 다기능 장치가 되었으므로 헤더 타입을 다시 읽어
 * multifunction 플래그를 갱신한다.
 *
 * [★ multifunction 갱신이 중요한 이유] 이 비트를 켜면 없던 함수 1 이
 * 생긴다. 커널이 그 함수를 스캔하려면 헤더 타입의 다기능 비트가 서 있어야
 * 한다. 그래서 하드웨어를 고친 뒤 커널의 인식도 함께 갱신한다.
 *
 * [세대 제한] MCP89 이전에는 내장 HDA 컨트롤러가 없었으므로 그보다 낮은
 * device ID 는 건너뛴다.
 *
 * 실행 컨텍스트: HEADER 와 RESUME_EARLY. 재개 후 다시 꺼져 있을 수 있고,
 * HEADER 인 것은 함수 1 이 스캔되기 전에 켜져야 하기 때문이다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_nvidia_hda] -> pci_write_config_dword()
 */
/*
 * Enable the NVIDIA GPU integrated HDA controller if the BIOS left it
 * disabled.  https://devtalk.nvidia.com/default/topic/1024022
 */
static void quirk_nvidia_hda(struct pci_dev *gpu)
{
	u8 hdr_type;	/* [한국어] 다시 읽은 헤더 타입 바이트. */
	u32 val;	/* [한국어] 0x488 레지스터 값. */

	/* There was no integrated HDA controller before MCP89 */
	if (gpu->device < PCI_DEVICE_ID_NVIDIA_GEFORCE_320M)	/* [한국어] 위 영어 주석대로 MCP89(GeForce 320M) 이전 세대에는 내장 HDA 가 없다. */
		return;

	/* Bit 25 at offset 0x488 enables the HDA controller */
	pci_read_config_dword(gpu, 0x488, &val);	/* [한국어] 위 영어 주석대로 오프셋 0x488 의 비트 25 가 HDA 활성 비트다. */
	if (val & BIT(25))	/* [한국어] 이미 켜져 있으면 손댈 것이 없다. */
		return;

	pci_info(gpu, "Enabling HDA controller\n");	/* [한국어] 무엇을 켜는지 남긴다. */
	pci_write_config_dword(gpu, 0x488, val | BIT(25));	/* [한국어] 비트 25 를 세워 HDA 컨트롤러를 켠다. 이 순간 없던 함수 1 이 생긴다. */

	/* The GPU becomes a multi-function device when the HDA is enabled */
	pci_read_config_byte(gpu, PCI_HEADER_TYPE, &hdr_type);	/* [한국어] 위 영어 주석대로 HDA 가 켜지면 GPU 가 다기능 장치가 되므로 헤더 타입을 다시 읽는다. */
	gpu->multifunction = FIELD_GET(PCI_HEADER_TYPE_MFD, hdr_type);	/* [한국어] 최상위 비트(다기능 표시)를 뽑아 커널의 인식을 갱신한다. 이 비트가 서야 스캔이 함수 1 을 훑는다. */
}
/* [한국어] NVIDIA 의 디스플레이 클래스 장치에 HEADER 단계로 등록한다.
 * class_shift 16 은 베이스 클래스(디스플레이)만 비교한다는 뜻이다. */
DECLARE_PCI_FIXUP_CLASS_HEADER(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID,
			       PCI_BASE_CLASS_DISPLAY, 16, quirk_nvidia_hda);
/* [한국어] 재개 시에도 다시 켠다 - 절전에서 깨어나면 다시 꺼져 있을 수 있다. */
DECLARE_PCI_FIXUP_CLASS_RESUME_EARLY(PCI_VENDOR_ID_NVIDIA, PCI_ANY_ID,
			       PCI_BASE_CLASS_DISPLAY, 16, quirk_nvidia_hda);

/*
 * [한국어]
 * pci_disable_broken_acs_cap - ACS Source Validation 이 잘못 구현된 IDT 스위치에서 그 기능을 지운다
 *
 * @pdev: 검사할 장치(IDT 0x80b5 / 0x8090 일 때만 동작)
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석이 IDT 89H32H8G3-YC 에라타 #36 을 그대로
 * 인용한다. PCIe r7.0 6.12.1.1 절은 Completion 이 ACS Source Validation 의
 * 영향을 받지 않는다고 못 박고 있다. 그런데 이 스위치의 다운스트림 포트는
 * 아직 PCIe 버스 번호를 확보하지 못한 장치에서 온 Completion 을 ACS Source
 * Validation 으로 잘못 버린다.
 * [왜 버스 번호가 없는 상태가 생기는가] PCIe 장치는 자신의 버스/디바이스
 * 번호를 미리 알지 못하고, 처음 도착하는 config 쓰기의 주소에서 그것을
 * 확보한다(PCIe r7.0 2.2.9.1). 그래서 열거 초기에는 번호가 없는 상태가 있다.
 * [IDT 의 권고와 그 한계] 영어 주석대로 IDT 는 '첫 config 읽기 전에 config
 * 쓰기를 한 번 보내라' 고 권한다. 그러면 장치가 번호를 확보해 문제가
 * 사라진다. 그러나 커널은 장치가 언제 config 쓰기를 받을 준비가 되는지
 * 알 수 없고, 이 문제는 열거뿐 아니라 스위치 리셋에도 영향을 준다.
 * [우회] 그래서 아예 이 장치들에서 ACS SV 사용을 꺼 버린다. 로그가
 * 밝히듯 하위 장치의 격리 수준이 낮아지는 대가를 치른다.
 *
 * [★ 이 파일에서 드문 형태] DECLARE_PCI_FIXUP_* 로 등록되지 않고 PCI
 * 코어가 이름으로 직접 부르는 비정적(non-static) 함수다. ACS capability 를
 * 파싱하는 시점에 불려야 하기 때문이다.
 *
 * 실행 컨텍스트: ACS capability 파싱 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 코어의 ACS 초기화 -> [pci_disable_broken_acs_cap]
 */
/*
 * Some IDT switches incorrectly flag an ACS Source Validation error on
 * completions for config read requests even though PCIe r7.0, sec
 * 6.12.1.1, says that completions are never affected by ACS Source
 * Validation.  Here's the text of IDT 89H32H8G3-YC, erratum #36:
 *
 *   Item #36 - Downstream port applies ACS Source Validation to Completions
 *   Section 6.12.1.1 of the PCI Express Base Specification 3.1 states that
 *   completions are never affected by ACS Source Validation.  However,
 *   completions received by a downstream port of the PCIe switch from a
 *   device that has not yet captured a PCIe bus number are incorrectly
 *   dropped by ACS Source Validation by the switch downstream port.
 *
 * The workaround suggested by IDT is to issue a config write to the
 * downstream device before issuing the first config read.  This allows the
 * downstream device to capture its bus and device numbers (see PCIe r7.0,
 * sec 2.2.9.1), thus avoiding the ACS error on the completion.
 *
 * However, we don't know when the device is ready to accept the config
 * write, and the issue affects resets of the switch as well as enumeration,
 * so disable use of ACS SV for these devices altogether.
 */
void pci_disable_broken_acs_cap(struct pci_dev *pdev)
{
	if (pdev->vendor == PCI_VENDOR_ID_IDT &&	/* [한국어] IDT 벤더이면서 */
	    (pdev->device == 0x80b5 || pdev->device == 0x8090)) {	/* [한국어] 문제가 확인된 두 스위치 모델일 때만 동작한다. */
		pci_info(pdev, "Disabling broken ACS SV; downstream device isolation reduced\n");	/* [한국어] 격리 수준이 낮아진다는 사실을 사용자에게 알린다 - 보안에 영향을 주는 결정이다. */
		pdev->acs_capabilities &= ~PCI_ACS_SV;	/* [한국어] 커널이 기억하는 ACS 능력에서 Source Validation 을 지운다. 하드웨어는 건드리지 않는다. */
	}
}

/*
 * [한국어]
 * quirk_switchtec_ntb_dma_alias - Switchtec NTB 의 프록시 ID 를 모두 DMA 별칭으로 등록
 *
 * @pdev: Microsemi Switchtec PCIe 스위치(NTB 클래스)
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, Switchtec NTB 는 내부 스위치 패브릭을
 * 통해 NT 엔드포인트 사이로 TLP 를 옮길 때 devfn 프록시 ID 를 쓴다. 그
 * ID 가 원래의 요청자 ID 를 대체하므로, 상대 NTB 포트에서 호스트 메모리에
 * 접근하는 트래픽은 낯선 ID 를 달고 올라온다.
 * [그대로 두면] IOMMU 가 켜져 있으면 그 접근이 막힌다.
 * [우회] 모든 프록시 ID 를 NTB 장치의 DMA 별칭으로 등록한다. MIC x200 이나
 * VCA quirk 와 같은 부류지만, 이쪽은 별칭 목록이 하드웨어 안의 표
 * (Requester ID Table)에 들어 있어 그것을 읽어 와야 한다.
 *
 * [★ 이 quirk 가 무거운 이유] 표를 읽으려면 MMIO 접근이 필요하고, 그러려면
 * 장치를 켜야 한다. 그래서 pci_enable_device() 로 켜고, BAR0 을 매핑해
 * 파티션마다 Requester ID Table 을 훑은 뒤, 매핑을 풀고 장치를 다시 끈다.
 * quirk 가 장치를 직접 켰다 끄는 드문 경우다(앞의 Mellanox INTx quirk 와
 * 같은 패턴).
 *
 * [파티션이란] Switchtec 은 하나의 물리 스위치를 여러 논리 파티션으로
 * 나눌 수 있다. 각 파티션이 독립된 PCIe 계층처럼 동작하고, NTB 는 그
 * 파티션들 사이를 잇는다. 그래서 '자기 파티션을 제외한 모든 파티션' 의
 * 프록시 ID 를 별칭으로 잡아야 한다.
 *
 * 실행 컨텍스트: FINAL 단계, 프로세스 문맥. 장치를 켜고 ioremap 하므로
 * 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_switchtec_ntb_dma_alias]
 *     -> pci_enable_device() -> pci_iomap() -> pci_add_dma_alias()
 */
/*
 * Microsemi Switchtec NTB uses devfn proxy IDs to move TLPs between
 * NT endpoints via the internal switch fabric. These IDs replace the
 * originating Requester ID TLPs which access host memory on peer NTB
 * ports. Therefore, all proxy IDs must be aliased to the NTB device
 * to permit access when the IOMMU is turned on.
 */
static void quirk_switchtec_ntb_dma_alias(struct pci_dev *pdev)
{
	void __iomem *mmio;	/* [한국어] BAR0 에 매핑된 스위치의 전역 주소 공간(GAS) 시작 주소. */
	struct ntb_info_regs __iomem *mmio_ntb;	/* [한국어] 그중 NTB 정보 레지스터 블록. */
	struct ntb_ctrl_regs __iomem *mmio_ctrl;	/* [한국어] 그중 NTB 제어 레지스터 블록(파티션마다 하나씩 배열로 이어진다). */
	u64 partition_map;	/* [한국어] 어느 파티션이 존재하는지 나타내는 64비트 비트맵. */
	u8 partition;	/* [한국어] 이 장치 자신이 속한 파티션 번호. */
	int pp;	/* [한국어] 파티션 번호 반복자. */

	if (pci_enable_device(pdev)) {	/* [한국어] MMIO 를 읽으려면 장치를 켜야 한다. */
		pci_err(pdev, "Cannot enable Switchtec device\n");	/* [한국어] 켜지 못하면 표를 읽을 수 없다. */
		return;
	}

	mmio = pci_iomap(pdev, 0, 0);	/* [한국어] BAR0 전체를 매핑한다 - 스위치의 전역 주소 공간이다. */
	if (mmio == NULL) {	/* [한국어] 매핑 실패 시 */
		pci_disable_device(pdev);	/* [한국어] 위에서 켠 장치를 반드시 다시 꺼야 한다. */
		pci_err(pdev, "Cannot iomap Switchtec device\n");	/* [한국어] 왜 못 했는지 남긴다. */
		return;
	}

	pci_info(pdev, "Setting Switchtec proxy ID aliases\n");	/* [한국어] 무엇을 하는지 남긴다. */

	mmio_ntb = mmio + SWITCHTEC_GAS_NTB_OFFSET;	/* [한국어] GAS 안에서 NTB 정보 블록의 오프셋을 더한다. */
	mmio_ctrl = (void __iomem *) mmio_ntb + SWITCHTEC_NTB_REG_CTRL_OFFSET;	/* [한국어] 그 안에서 다시 제어 레지스터 블록의 오프셋을 더한다. 이 블록이 파티션 개수만큼 배열로 놓인다. */

	partition = ioread8(&mmio_ntb->partition_id);	/* [한국어] 이 장치가 속한 파티션 번호를 읽는다. */

	partition_map = ioread32(&mmio_ntb->ep_map);	/* [한국어] 존재하는 파티션 비트맵의 하위 32비트를 읽는다. */
	partition_map |= ((u64) ioread32(&mmio_ntb->ep_map + 4)) << 32;	/* [한국어] 상위 32비트를 읽어 이어 붙인다. 레지스터가 32비트 단위라 두 번 읽어야 한다. */
	partition_map &= ~(1ULL << partition);	/* [한국어] 자기 파티션은 제외한다 - 자기 자신에게는 별칭이 필요 없다. */

	for (pp = 0; pp < (sizeof(partition_map) * 8); pp++) {	/* [한국어] 비트맵의 모든 비트 위치를 훑는다. sizeof * 8 로 64 를 얻는다. */
		struct ntb_ctrl_regs __iomem *mmio_peer_ctrl;	/* [한국어] 이 파티션의 제어 레지스터 블록. */
		u32 table_sz = 0;	/* [한국어] 그 파티션의 Requester ID Table 항목 개수. */
		int te;	/* [한국어] 표 항목 반복자. */

		if (!(partition_map & (1ULL << pp)))	/* [한국어] 이 비트가 꺼져 있으면 그 파티션은 존재하지 않는다. */
			continue;

		pci_dbg(pdev, "Processing partition %d\n", pp);	/* [한국어] 어느 파티션을 처리 중인지 디버그 로그로 남긴다. */

		mmio_peer_ctrl = &mmio_ctrl[pp];	/* [한국어] 이 파티션의 제어 레지스터 블록을 가리킨다. */

		table_sz = ioread16(&mmio_peer_ctrl->req_id_table_size);	/* [한국어] 그 파티션의 표 크기를 읽는다. */
		if (!table_sz) {	/* [한국어] 표가 비어 있으면 */
			pci_warn(pdev, "Partition %d table_sz 0\n", pp);	/* [한국어] 이상 상황이므로 경고만 남기고 */
			continue;	/* [한국어] 다음 파티션으로 넘어간다. */
		}

		if (table_sz > 512) {	/* [한국어] 표 크기가 하드웨어가 지원하는 최대(512)를 넘으면 값이 깨진 것이다. */
			pci_warn(pdev,	/* [한국어] 잘못된 값이라고 경고하고 */
				 "Invalid Switchtec partition %d table_sz %d\n",
				 pp, table_sz);
			continue;	/* [한국어] 다음 파티션으로 넘어간다 - 그대로 읽으면 범위를 벗어난 MMIO 접근이 된다. */
		}

		for (te = 0; te < table_sz; te++) {	/* [한국어] 표의 각 항목을 훑는다. */
			u32 rid_entry;	/* [한국어] 한 항목의 32비트 값. */
			u8 devfn;	/* [한국어] 그 안에서 뽑아낸 devfn. */

			rid_entry = ioread32(&mmio_peer_ctrl->req_id_table[te]);	/* [한국어] 표 항목을 읽는다. */
			devfn = (rid_entry >> 1) & 0xFF;	/* [한국어] 비트 8:1 이 devfn 이다. >>1 로 밀고 하위 8비트만 남긴다(비트 0 은 다른 용도). */
			pci_dbg(pdev,	/* [한국어] 어떤 프록시 ID 를 별칭으로 잡는지 디버그 로그로 남긴다. */
				"Aliasing Partition %d Proxy ID %02x.%d\n",
				pp, PCI_SLOT(devfn), PCI_FUNC(devfn));	/* [한국어] PCI_SLOT/PCI_FUNC 로 devfn 을 사람이 읽는 형태로 나눠 찍는다. */
			pci_add_dma_alias(pdev, devfn, 1);	/* [한국어] 그 devfn 을 이 NTB 장치의 DMA 별칭으로 등록한다. */
		}
	}

	pci_iounmap(pdev, mmio);	/* [한국어] 매핑을 해제한다. */
	pci_disable_device(pdev);	/* [한국어] 위에서 켠 장치를 다시 끈다 - quirk 는 부작용을 남기지 않아야 한다. */
}
/* [한국어] Microsemi Switchtec PCIe 스위치용 quirk 등록 매크로.
 * 주의: 줄 끝 백슬래시는 매크로 연속을 뜻하며 **그 줄의 마지막 문자여야 한다.**
 * 뒤에 주석이나 공백이 오면 연속이 끊겨 컴파일 오류가 난다. */
#define SWITCHTEC_QUIRK(vid) \
	DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_MICROSEMI, vid, \
		PCI_CLASS_BRIDGE_OTHER, 8, quirk_switchtec_ntb_dma_alias)

/* [한국어] 아래는 위 매크로로 Switchtec 모델을 하나씩 등록하는 목록이다.
 * 각 줄 옆의 영어 주석이 모델명이며, 접두어의 뜻은 다음과 같다 -
 * PFX(Fabric eXpansion), PSX(Storage eXpansion), PAX(Advanced eXpansion),
 * PFXL/PFXI 는 그 변종, 끝의 XG3/XG4/XG5 는 PCIe Gen3/Gen4/Gen5 세대,
 * 앞의 숫자는 레인 수다. 모두 같은 quirk 함수를 쓰며 클래스는
 * PCI_CLASS_BRIDGE_OTHER 로 좁혀 NTB 로 동작하는 것만 잡는다. */
SWITCHTEC_QUIRK(0x8531);  /* PFX 24xG3 */
SWITCHTEC_QUIRK(0x8532);  /* PFX 32xG3 */
SWITCHTEC_QUIRK(0x8533);  /* PFX 48xG3 */
SWITCHTEC_QUIRK(0x8534);  /* PFX 64xG3 */
SWITCHTEC_QUIRK(0x8535);  /* PFX 80xG3 */
SWITCHTEC_QUIRK(0x8536);  /* PFX 96xG3 */
SWITCHTEC_QUIRK(0x8541);  /* PSX 24xG3 */
SWITCHTEC_QUIRK(0x8542);  /* PSX 32xG3 */
SWITCHTEC_QUIRK(0x8543);  /* PSX 48xG3 */
SWITCHTEC_QUIRK(0x8544);  /* PSX 64xG3 */
SWITCHTEC_QUIRK(0x8545);  /* PSX 80xG3 */
SWITCHTEC_QUIRK(0x8546);  /* PSX 96xG3 */
SWITCHTEC_QUIRK(0x8551);  /* PAX 24XG3 */
SWITCHTEC_QUIRK(0x8552);  /* PAX 32XG3 */
SWITCHTEC_QUIRK(0x8553);  /* PAX 48XG3 */
SWITCHTEC_QUIRK(0x8554);  /* PAX 64XG3 */
SWITCHTEC_QUIRK(0x8555);  /* PAX 80XG3 */
SWITCHTEC_QUIRK(0x8556);  /* PAX 96XG3 */
SWITCHTEC_QUIRK(0x8561);  /* PFXL 24XG3 */
SWITCHTEC_QUIRK(0x8562);  /* PFXL 32XG3 */
SWITCHTEC_QUIRK(0x8563);  /* PFXL 48XG3 */
SWITCHTEC_QUIRK(0x8564);  /* PFXL 64XG3 */
SWITCHTEC_QUIRK(0x8565);  /* PFXL 80XG3 */
SWITCHTEC_QUIRK(0x8566);  /* PFXL 96XG3 */
SWITCHTEC_QUIRK(0x8571);  /* PFXI 24XG3 */
SWITCHTEC_QUIRK(0x8572);  /* PFXI 32XG3 */
SWITCHTEC_QUIRK(0x8573);  /* PFXI 48XG3 */
SWITCHTEC_QUIRK(0x8574);  /* PFXI 64XG3 */
SWITCHTEC_QUIRK(0x8575);  /* PFXI 80XG3 */
SWITCHTEC_QUIRK(0x8576);  /* PFXI 96XG3 */
SWITCHTEC_QUIRK(0x4000);  /* PFX 100XG4 */
SWITCHTEC_QUIRK(0x4084);  /* PFX 84XG4  */
SWITCHTEC_QUIRK(0x4068);  /* PFX 68XG4  */
SWITCHTEC_QUIRK(0x4052);  /* PFX 52XG4  */
SWITCHTEC_QUIRK(0x4036);  /* PFX 36XG4  */
SWITCHTEC_QUIRK(0x4028);  /* PFX 28XG4  */
SWITCHTEC_QUIRK(0x4100);  /* PSX 100XG4 */
SWITCHTEC_QUIRK(0x4184);  /* PSX 84XG4  */
SWITCHTEC_QUIRK(0x4168);  /* PSX 68XG4  */
SWITCHTEC_QUIRK(0x4152);  /* PSX 52XG4  */
SWITCHTEC_QUIRK(0x4136);  /* PSX 36XG4  */
/* [한국어] 이어지는 등록 줄: 0x4128, 0x4200, 0x4284, 0x4268. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x4128);  /* PSX 28XG4  */
SWITCHTEC_QUIRK(0x4200);  /* PAX 100XG4 */
SWITCHTEC_QUIRK(0x4284);  /* PAX 84XG4  */
SWITCHTEC_QUIRK(0x4268);  /* PAX 68XG4  */
SWITCHTEC_QUIRK(0x4252);  /* PAX 52XG4  */
/* [한국어] 이어지는 등록 줄: 0x4236, 0x4228, 0x4352, 0x4336. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x4236);  /* PAX 36XG4  */
SWITCHTEC_QUIRK(0x4228);  /* PAX 28XG4  */
SWITCHTEC_QUIRK(0x4352);  /* PFXA 52XG4 */
SWITCHTEC_QUIRK(0x4336);  /* PFXA 36XG4 */
SWITCHTEC_QUIRK(0x4328);  /* PFXA 28XG4 */
/* [한국어] 이어지는 등록 줄: 0x4452, 0x4436, 0x4428, 0x4552. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x4452);  /* PSXA 52XG4 */
SWITCHTEC_QUIRK(0x4436);  /* PSXA 36XG4 */
SWITCHTEC_QUIRK(0x4428);  /* PSXA 28XG4 */
SWITCHTEC_QUIRK(0x4552);  /* PAXA 52XG4 */
SWITCHTEC_QUIRK(0x4536);  /* PAXA 36XG4 */
/* [한국어] 이어지는 등록 줄: 0x4528, 0x5000, 0x5084, 0x5068. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x4528);  /* PAXA 28XG4 */
SWITCHTEC_QUIRK(0x5000);  /* PFX 100XG5 */
SWITCHTEC_QUIRK(0x5084);  /* PFX 84XG5 */
SWITCHTEC_QUIRK(0x5068);  /* PFX 68XG5 */
SWITCHTEC_QUIRK(0x5052);  /* PFX 52XG5 */
/* [한국어] 이어지는 등록 줄: 0x5036, 0x5028, 0x5100, 0x5184. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x5036);  /* PFX 36XG5 */
SWITCHTEC_QUIRK(0x5028);  /* PFX 28XG5 */
SWITCHTEC_QUIRK(0x5100);  /* PSX 100XG5 */
SWITCHTEC_QUIRK(0x5184);  /* PSX 84XG5 */
SWITCHTEC_QUIRK(0x5168);  /* PSX 68XG5 */
/* [한국어] 이어지는 등록 줄: 0x5152, 0x5136, 0x5128, 0x5200. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x5152);  /* PSX 52XG5 */
SWITCHTEC_QUIRK(0x5136);  /* PSX 36XG5 */
SWITCHTEC_QUIRK(0x5128);  /* PSX 28XG5 */
SWITCHTEC_QUIRK(0x5200);  /* PAX 100XG5 */
SWITCHTEC_QUIRK(0x5284);  /* PAX 84XG5 */
/* [한국어] 이어지는 등록 줄: 0x5268, 0x5252, 0x5236, 0x5228. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x5268);  /* PAX 68XG5 */
SWITCHTEC_QUIRK(0x5252);  /* PAX 52XG5 */
SWITCHTEC_QUIRK(0x5236);  /* PAX 36XG5 */
SWITCHTEC_QUIRK(0x5228);  /* PAX 28XG5 */
SWITCHTEC_QUIRK(0x5300);  /* PFXA 100XG5 */
/* [한국어] 이어지는 등록 줄: 0x5384, 0x5368, 0x5352, 0x5336. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x5384);  /* PFXA 84XG5 */
SWITCHTEC_QUIRK(0x5368);  /* PFXA 68XG5 */
SWITCHTEC_QUIRK(0x5352);  /* PFXA 52XG5 */
SWITCHTEC_QUIRK(0x5336);  /* PFXA 36XG5 */
SWITCHTEC_QUIRK(0x5328);  /* PFXA 28XG5 */
/* [한국어] 이어지는 등록 줄: 0x5400, 0x5484, 0x5468, 0x5452. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x5400);  /* PSXA 100XG5 */
SWITCHTEC_QUIRK(0x5484);  /* PSXA 84XG5 */
SWITCHTEC_QUIRK(0x5468);  /* PSXA 68XG5 */
SWITCHTEC_QUIRK(0x5452);  /* PSXA 52XG5 */
SWITCHTEC_QUIRK(0x5436);  /* PSXA 36XG5 */
/* [한국어] 이어지는 등록 줄: 0x5428, 0x5500, 0x5584, 0x5568. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
SWITCHTEC_QUIRK(0x5428);  /* PSXA 28XG5 */
SWITCHTEC_QUIRK(0x5500);  /* PAXA 100XG5 */
SWITCHTEC_QUIRK(0x5584);  /* PAXA 84XG5 */
SWITCHTEC_QUIRK(0x5568);  /* PAXA 68XG5 */
SWITCHTEC_QUIRK(0x5552);  /* PAXA 52XG5 */
/* [한국어] 이어지는 등록 줄: 0x5536, 0x5528. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
SWITCHTEC_QUIRK(0x5536);  /* PAXA 36XG5 */
SWITCHTEC_QUIRK(0x5528);  /* PAXA 28XG5 */

/* [한국어] Switchtec PCI100x 계열용 quirk 등록 매크로. 위와 같은 이유로
 * 줄 끝 백슬래시 뒤에는 아무것도 올 수 없다. */
#define SWITCHTEC_PCI100X_QUIRK(vid) \
	DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_EFAR, vid, \
		PCI_CLASS_BRIDGE_OTHER, 8, quirk_switchtec_ntb_dma_alias)
/* [한국어] Switchtec PCI100x 계열 6종을 등록한다. 벤더 ID 가
 * PCI_VENDOR_ID_MICROSEMI 가 아니라 PCI_VENDOR_ID_EFAR 인 것이 위 목록과
 * 다른 점으로, 같은 실리콘이 다른 벤더 ID 로 출하되기 때문이다. */
SWITCHTEC_PCI100X_QUIRK(0x1001);  /* PCI1001XG4 */
SWITCHTEC_PCI100X_QUIRK(0x1002);  /* PCI1002XG4 */
SWITCHTEC_PCI100X_QUIRK(0x1003);  /* PCI1003XG4 */
SWITCHTEC_PCI100X_QUIRK(0x1004);  /* PCI1004XG4 */
SWITCHTEC_PCI100X_QUIRK(0x1005);  /* PCI1005XG4 */
SWITCHTEC_PCI100X_QUIRK(0x1006);  /* PCI1006XG4 */


/*
 * [한국어]
 * quirk_plx_ntb_dma_alias - PLX NTB 의 모든 가능한 devfn 을 DMA 별칭으로 등록
 *
 * @pdev: PLX NTB(0x87b0, 0x87b1)
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석대로, PLX NTB 도 Switchtec 과 마찬가지로
 * NT 엔드포인트 사이로 TLP 를 옮길 때 devfn 프록시 ID 를 쓴다. 그 ID 는
 * NTB 반대편의 원 요청자에게 응답을 되돌리는 데 쓰인다.
 * [그대로 두면] IOMMU 가 켜져 있으면 그 접근이 막힌다.
 * [우회] Switchtec 처럼 하드웨어 표를 읽는 대신, 가능한 devfn 256개를
 * 통째로 별칭으로 등록한다. 옆의 영어 주석이 그 근거를 밝힌다 - PLX NTB 는
 * 256개 devfn 을 모두 쓸 수 있다.
 *
 * [대가] 256개를 다 걸면 그 버스의 모든 ID 가 이 장치와 같은 IOMMU
 * 그룹으로 묶인다. 세밀한 격리를 포기하고 동작을 택한 것으로, VCA quirk 와
 * 같은 절충이다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_plx_ntb_dma_alias] -> pci_add_dma_alias()
 */
/*
 * The PLX NTB uses devfn proxy IDs to move TLPs between NT endpoints.
 * These IDs are used to forward responses to the originator on the other
 * side of the NTB.  Alias all possible IDs to the NTB to permit access when
 * the IOMMU is turned on.
 */
static void quirk_plx_ntb_dma_alias(struct pci_dev *pdev)
{
	pci_info(pdev, "Setting PLX NTB proxy ID aliases\n");	/* [한국어] 무엇을 하는지 남긴다. */
	/* PLX NTB may use all 256 devfns */
	pci_add_dma_alias(pdev, 0, 256);	/* [한국어] devfn 0 부터 256개를 한 번에 별칭으로 등록한다. */
}
/* [한국어] PLX NTB 2종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_PLX, 0x87b0, quirk_plx_ntb_dma_alias);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_PLX, 0x87b1, quirk_plx_ntb_dma_alias);

/*
 * [한국어]
 * quirk_reset_lenovo_thinkpad_p50_nvgpu - BIOS 가 리셋하지 않고 넘긴 GPU 를 리셋한다
 *
 * @pdev: Lenovo ThinkPad P50 의 NVIDIA Quadro M1000M (서브시스템 0x222e)
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석이 사정을 자세히 밝힌다. Hybrid Graphics
 * 모드로 설정된 ThinkPad P50 에서는 BIOS 가 재부팅 사이에 보조 NVIDIA GPU 를
 * 리셋하지 않는 경우가 있다. 그러면 GPU 가 '이전 부팅' 때의 상태 그대로
 * 남아 헛인터럽트를 쏜다. 커널은 그 인터럽트를 보고 엉뚱한 IRQ 를 꺼
 * 터치패드까지 망가뜨린다. nouveau 드라이버도 완전히 동작하지 않는다.
 * [그대로 두면] 터치패드가 죽고 GPU 드라이버가 붙지 못한다.
 * [우회] 영어 주석대로, 단순히 GPU 를 리셋하면 깨끗한 상태로 돌아와 문제가
 * 모두 사라진다. 그래서 GPU 가 이미 초기화되어 있는지 MMIO 로 확인하고,
 * 그렇다면 버스 리셋을 건다.
 *
 * [Dedicated 모드를 어떻게 피하는가] 영어 주석대로 전용 디스플레이 모드
 * 에서는 이 문제가 생기지 않고, 그 모드에서는 GPU 가 NoReset+ 를 광고한다.
 * pci_reset_supported() 가 그것을 보고 거짓을 돌려주므로 자동으로 걸러진다.
 *
 * [MMIO 오프셋의 출처] 영어 주석대로 nouveau 의 nvkm_device_ctor() 를
 * 참고한 값이다. 0x2240c 의 비트 1 이 서 있으면 GPU 가 POST 된 상태다.
 *
 * 실행 컨텍스트: FINAL 단계, 프로세스 문맥. 장치를 켜고 ioremap 하며
 * 버스 리셋까지 하므로 잠들 수 있어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [quirk_reset_lenovo_thinkpad_p50_nvgpu]
 *     -> pci_enable_device_mem() -> pci_iomap() -> pci_reset_bus()
 */
/*
 * On Lenovo Thinkpad P50 SKUs with a Nvidia Quadro M1000M, the BIOS does
 * not always reset the secondary Nvidia GPU between reboots if the system
 * is configured to use Hybrid Graphics mode.  This results in the GPU
 * being left in whatever state it was in during the *previous* boot, which
 * causes spurious interrupts from the GPU, which in turn causes us to
 * disable the wrong IRQ and end up breaking the touchpad.  Unsurprisingly,
 * this also completely breaks nouveau.
 *
 * Luckily, it seems a simple reset of the Nvidia GPU brings it back to a
 * clean state and fixes all these issues.
 *
 * When the machine is configured in Dedicated display mode, the issue
 * doesn't occur.  Fortunately the GPU advertises NoReset+ when in this
 * mode, so we can detect that and avoid resetting it.
 */
static void quirk_reset_lenovo_thinkpad_p50_nvgpu(struct pci_dev *pdev)
{
	void __iomem *map;	/* [한국어] GPU 레지스터의 MMIO 매핑 주소. */
	int ret;	/* [한국어] 리셋 결과. */

	if (pdev->subsystem_vendor != PCI_VENDOR_ID_LENOVO ||	/* [한국어] 서브벤더가 Lenovo 이고 */
	    pdev->subsystem_device != 0x222e ||	/* [한국어] 서브디바이스가 P50 의 것이며 */
	    !pci_reset_supported(pdev))	/* [한국어] 리셋이 지원되어야 한다. 전용 디스플레이 모드에서는 NoReset+ 라 이 검사에서 걸러진다. */
		return;

	if (pci_enable_device_mem(pdev))	/* [한국어] MMIO 를 읽으려면 메모리 디코딩을 켜야 한다. */
		return;	/* [한국어] 켜지 못하면 상태를 확인할 수 없다. */

	/*
	 * Based on nvkm_device_ctor() in
	 * drivers/gpu/drm/nouveau/nvkm/engine/device/base.c
	 */
	map = pci_iomap(pdev, 0, 0x23000);	/* [한국어] 위 영어 주석이 밝힌 nouveau 코드를 참고해 필요한 범위만 매핑한다. */
	if (!map) {	/* [한국어] 매핑 실패 시 */
		pci_err(pdev, "Can't map MMIO space\n");	/* [한국어] 오류를 남기고 */
		goto out_disable;	/* [한국어] 장치를 다시 끄는 정리 경로로 간다. */
	}

	/*
	 * Make sure the GPU looks like it's been POSTed before resetting
	 * it.
	 */
	if (ioread32(map + 0x2240c) & 0x2) {	/* [한국어] 위 영어 주석대로 GPU 가 POST 된 것처럼 보이는지 확인한다. 0x2240c 의 비트 1 이 그 표시다. */
		pci_info(pdev, FW_BUG "GPU left initialized by EFI, resetting\n");	/* [한국어] 펌웨어 잘못임을 FW_BUG 접두로 표시하고 리셋한다고 남긴다. */
		ret = pci_reset_bus(pdev);	/* [한국어] 버스 리셋을 건다 - GPU 를 깨끗한 상태로 되돌리는 유일한 방법이다. */
		if (ret < 0)	/* [한국어] 리셋 실패 시 */
			pci_err(pdev, "Failed to reset GPU: %d\n", ret);	/* [한국어] 오류만 남긴다. 여기서 할 수 있는 다른 조치가 없다. */
	}

	iounmap(map);	/* [한국어] 매핑을 해제한다. */
out_disable:	/* [한국어] 매핑 실패 경로가 합류하는 정리 지점. */
	pci_disable_device(pdev);	/* [한국어] 위에서 켠 장치를 다시 끈다. */
}
/* [한국어] NVIDIA 0x13b1(Quadro M1000M) 중 VGA 클래스인 것에 FINAL
 * 단계로 등록한다. 보드 판별은 함수 안에서 서브시스템 ID 로 한다. */
DECLARE_PCI_FIXUP_CLASS_FINAL(PCI_VENDOR_ID_NVIDIA, 0x13b1,
			      PCI_CLASS_DISPLAY_VGA, 8,
			      quirk_reset_lenovo_thinkpad_p50_nvgpu);

/*
 * Device [1b21:2142]
 * When in D0, PME# doesn't get asserted when plugging USB 3.0 device.
 */
/*
 * [한국어]
 * pci_fixup_no_d0_pme - D0 에서 PME# 를 걸지 못하는 장치의 광고를 지운다
 *
 * @dev: ASMedia device 0x2142 (USB 3.0 컨트롤러)
 * @return: 없음
 *
 * [PME# 란] Power Management Event - 장치가 저전력 상태에서 시스템을
 * 깨우거나 이벤트를 알리기 위해 거는 신호다. 전원관리 capability 의
 * PME_Support 필드로 '어느 D 상태에서 PME 를 걸 수 있는지' 를 광고한다.
 * [무엇이 문제] 위 영어 주석대로, 이 장치는 D0 상태에서 USB 3.0 장치를
 * 꽂아도 PME# 가 어서트되지 않는다. 즉 D0 에서의 PME 지원을 광고하면서
 * 실제로는 걸지 못한다.
 * [그대로 두면] 커널이 D0 PME 를 기다리며 이벤트를 놓친다.
 * [우회] dev->pme_support 에서 D0 비트만 지운다. 다른 D 상태의 PME 지원은
 * 그대로 두므로, 절전 복귀용 wake 는 계속 동작한다.
 *
 * [비트 위치 계산] PCI_PM_CAP_PME_D0 은 PM Capabilities 레지스터 안에서의
 * 비트 위치이고, dev->pme_support 는 그 필드만 잘라 낸 값이다. 그래서
 * PCI_PM_CAP_PME_SHIFT 만큼 오른쪽으로 밀어 자리를 맞춘 뒤 지운다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [pci_fixup_no_d0_pme]
 */
static void pci_fixup_no_d0_pme(struct pci_dev *dev)
{
	pci_info(dev, "PME# does not work under D0, disabling it\n");	/* [한국어] 무엇을 끄는지 남긴다. */
	dev->pme_support &= ~(PCI_PM_CAP_PME_D0 >> PCI_PM_CAP_PME_SHIFT);	/* [한국어] PME_Support 에서 D0 비트만 지운다. 위 설명대로 SHIFT 로 자리를 맞춘다. */
}
/* [한국어] ASMedia 0x2142 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_ASMEDIA, 0x2142, pci_fixup_no_d0_pme);

/*
 * Device 12d8:0x400e [OHCI] and 12d8:0x400f [EHCI]
 *
 * These devices advertise PME# support in all power states but don't
 * reliably assert it.
 *
 * These devices also advertise MSI, but documentation (PI7C9X440SL.pdf)
 * says "The MSI Function is not implemented on this device" in chapters
 * 7.3.27, 7.3.29-7.3.31.
 */
/*
 * [한국어]
 * pci_fixup_no_msi_no_pme - MSI 미구현이고 PME# 도 못 믿을 장치의 광고를 모두 지운다
 *
 * @dev: Pericom 12d8:0x400e(OHCI), 12d8:0x400f(EHCI)
 * @return: 없음
 *
 * [무엇이 문제] 위 영어 주석이 두 가지를 밝힌다.
 *   (1) 이 장치들은 모든 전원 상태에서 PME# 를 지원한다고 광고하지만
 *       실제로는 신뢰할 수 있게 어서트하지 않는다.
 *   (2) MSI 도 광고하지만, 데이터시트(PI7C9X440SL.pdf)의 7.3.27,
 *       7.3.29~7.3.31 장이 'MSI 기능은 이 장치에 구현되어 있지 않다' 고
 *       명시한다. 즉 capability 가 거짓말이다.
 * [그대로 두면] MSI 인터럽트가 오지 않고, PME 를 기다리는 코드가 이벤트를
 * 놓친다.
 * [우회] MSI 지원이 빌드에 있으면 no_msi 를 세우고, pme_support 는 통째로
 * 0 으로 만들어 어느 상태에서도 PME 를 기대하지 않게 한다. 위
 * pci_fixup_no_d0_pme() 가 D0 비트만 지운 것과 대비된다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [pci_fixup_no_msi_no_pme]
 */
static void pci_fixup_no_msi_no_pme(struct pci_dev *dev)
{
/* [한국어] MSI 지원이 빌드에 없으면 no_msi 를 세울 필요도 없다. */
#ifdef CONFIG_PCI_MSI
	pci_info(dev, "MSI is not implemented on this device, disabling it\n");	/* [한국어] MSI 가 미구현이라는 사실을 알린다. */
	dev->no_msi = 1;	/* [한국어] 이 장치에서 MSI 사용을 금지한다. */
#endif
	pci_info(dev, "PME# is unreliable, disabling it\n");	/* [한국어] PME 를 믿을 수 없다는 사실을 알린다. */
	dev->pme_support = 0;	/* [한국어] 모든 전원 상태의 PME 지원을 지운다. */
}
/* [한국어] Pericom OHCI/EHCI 두 함수에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_PERICOM, 0x400e, pci_fixup_no_msi_no_pme);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_PERICOM, 0x400f, pci_fixup_no_msi_no_pme);

/*
 * [한국어]
 * apex_pci_fixup_class - 클래스 코드가 비어 있는 Apex 가속기를 분류해 준다
 *
 * @pdev: device 0x1ac1:0x089a (Google Edge TPU / Apex 가속기)
 * @return: 없음
 *
 * [무엇이 문제] 이 quirk 에는 원본 영어 주석이 없다. 등록이
 * PCI_CLASS_NOT_DEFINED 로 좁혀져 있고 함수가 클래스를 채우는 것으로 보아,
 * 이 장치가 클래스 코드를 보고하지 않는다는 뜻이다. 구체적 배경은 이
 * 트리의 정보만으로는 확인할 수 없다.
 * [우회] System Peripheral(기타) 클래스를 얹어 준다. 앞의 다른 클래스
 * 보정 quirk 들이 대입으로 덮어쓴 것과 달리 OR 로 얹는데, 원래 클래스가
 * 0 이라 결과는 같다.
 *
 * 실행 컨텍스트: HEADER 단계, 클래스가 정의되지 않은 장치만 매칭.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [apex_pci_fixup_class]
 */
static void apex_pci_fixup_class(struct pci_dev *pdev)
{
	pdev->class = (PCI_CLASS_SYSTEM_OTHER << 8) | pdev->class;	/* [한국어] System Peripheral 베이스 클래스를 상위 16비트에 얹는다. 기존 값이 0 이므로 결과는 대입과 같다. */
}
/* [한국어] 0x1ac1:0x089a 중 클래스가 정의되지 않은 것만 HEADER 단계에서 잡는다. */
DECLARE_PCI_FIXUP_CLASS_HEADER(0x1ac1, 0x089a,
			       PCI_CLASS_NOT_DEFINED, 8, apex_pci_fixup_class);

/*
 * [한국어]
 * pci_fixup_pericom_acs_store_forward - ACS RR 이 켜진 Pericom 스위치를 store-and-forward 로 돌린다
 *
 * @pdev: Pericom PI7C9X2G404/304/303 스위치의 다운스트림 포트
 * @return: 없음
 *
 * [무엇이 문제] 아래 영어 주석이 에라타 E5 를 인용한다. ACS P2P Request
 * Redirect 가 켜져 있고 업스트림과 다운스트림 포트의 대역폭이 맞지 않으면,
 * 패킷이 CPLD 패킷이 올 때까지 내부 버퍼에 쌓인다. 즉 ACS 재지향이
 * 제대로 동작하지 않는다.
 * [그대로 두면] 그 조합에서 트래픽이 멈춘다.
 * [우회] 스위치를 store-and-forward 모드로 돌린다. 패킷을 통째로 받은 뒤
 * 전달하는 방식이라 대역폭 불일치를 흡수할 수 있다. 벤더 전용 레지스터
 * 0x74 의 비트 0 이 그 모드 스위치다.
 *
 * [★ 조건부로만 적용하는 이유] store-and-forward 는 지연이 늘어난다.
 * 그래서 실제로 ACS Request Redirect 가 켜져 있을 때만 적용한다.
 *
 * [매칭된 장치와 고치는 장치가 다르다] quirk 는 다운스트림 포트에 걸리지만
 * 모드 레지스터는 업스트림 포트에 있다. 그래서 pci_upstream_bridge() 로
 * 거슬러 올라가 그쪽을 고친다.
 *
 * 실행 컨텍스트: ENABLE 과 RESUME. 아래 영어 주석대로 ACS 설정이 바뀌거나
 * 스위치 모드가 초기화될 때마다 다시 적용되도록 두 단계에 걸어 둔다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [pci_fixup_pericom_acs_store_forward]
 *     -> pci_upstream_bridge() -> pci_write_config_word()
 */
/*
 * Pericom PI7C9X2G404/PI7C9X2G304/PI7C9X2G303 switch erratum E5 -
 * ACS P2P Request Redirect is not functional
 *
 * When ACS P2P Request Redirect is enabled and bandwidth is not balanced
 * between upstream and downstream ports, packets are queued in an internal
 * buffer until CPLD packet. The workaround is to use the switch in store and
 * forward mode.
 */
#define PI7C9X2Gxxx_MODE_REG		0x74	/* [한국어] 이 스위치의 모드 제어 레지스터 오프셋(벤더 전용). */
#define PI7C9X2Gxxx_STORE_FORWARD_MODE	BIT(0)	/* [한국어] 그 레지스터의 비트 0 - store-and-forward 모드 활성 비트. */
static void pci_fixup_pericom_acs_store_forward(struct pci_dev *pdev)
{
	struct pci_dev *upstream;	/* [한국어] 실제로 고칠 대상인 업스트림 포트. */
	u16 val;	/* [한국어] 레지스터 값을 담는 임시 워드. ACS 제어와 모드 레지스터에 재사용된다. */

	/* Downstream ports only */
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_DOWNSTREAM)	/* [한국어] 위 영어 주석대로 다운스트림 포트에서만 판정한다. */
		return;

	/* Check for ACS P2P Request Redirect use */
	if (!pdev->acs_cap)	/* [한국어] ACS capability 가 없으면 Request Redirect 도 없으니 이 문제가 생기지 않는다. */
		return;
	pci_read_config_word(pdev, pdev->acs_cap + PCI_ACS_CTRL, &val);	/* [한국어] ACS Control 레지스터를 읽는다. */
	if (!(val & PCI_ACS_RR))	/* [한국어] Request Redirect 가 꺼져 있으면 에라타 조건이 아니므로 지연을 감수할 이유가 없다. */
		return;

	upstream = pci_upstream_bridge(pdev);	/* [한국어] 모드 레지스터는 업스트림 포트에 있으므로 거슬러 올라간다. */
	if (!upstream)	/* [한국어] 업스트림이 없으면 고칠 수 없다. */
		return;

	pci_read_config_word(upstream, PI7C9X2Gxxx_MODE_REG, &val);	/* [한국어] 업스트림의 모드 레지스터를 읽는다. */
	if (!(val & PI7C9X2Gxxx_STORE_FORWARD_MODE)) {	/* [한국어] 이미 store-and-forward 모드면 손댈 것이 없다. */
		pci_info(upstream, "Setting PI7C9X2Gxxx store-forward mode to avoid ACS erratum\n");	/* [한국어] 무엇을 왜 바꾸는지 남긴다. */
		pci_write_config_word(upstream, PI7C9X2Gxxx_MODE_REG, val |	/* [한국어] 비트 0 을 세워 store-and-forward 모드로 바꾼다. */
				      PI7C9X2Gxxx_STORE_FORWARD_MODE);
	}
}
/*
 * Apply fixup on enable and on resume, in order to apply the fix up whenever
 * ACS configuration changes or switch mode is reset
 */
/* [한국어] Pericom 2404 에 ENABLE 단계로 등록한다. 위 영어 주석대로
 * ACS 설정이 바뀌거나 스위치 모드가 초기화될 때마다 다시 적용되어야 한다. */
DECLARE_PCI_FIXUP_ENABLE(PCI_VENDOR_ID_PERICOM, 0x2404,
			 pci_fixup_pericom_acs_store_forward);
/* [한국어] 같은 함수를 Pericom 스위치 4종(2404, 2304, 2303, b404)에 대해
 * ENABLE 과 RESUME 을 짝지어 등록한다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_PERICOM, 0x2404,
			 pci_fixup_pericom_acs_store_forward);
DECLARE_PCI_FIXUP_ENABLE(PCI_VENDOR_ID_PERICOM, 0x2304,
			 pci_fixup_pericom_acs_store_forward);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_PERICOM, 0x2304,
			 pci_fixup_pericom_acs_store_forward);
/* [한국어] 이어지는 등록 줄: 0x2303. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_ENABLE(PCI_VENDOR_ID_PERICOM, 0x2303,
			 pci_fixup_pericom_acs_store_forward);
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_PERICOM, 0x2303,
			 pci_fixup_pericom_acs_store_forward);
DECLARE_PCI_FIXUP_ENABLE(PCI_VENDOR_ID_PERICOM, 0xb404,
			 pci_fixup_pericom_acs_store_forward);
/* [한국어] 이어지는 등록 줄: 0xb404. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_RESUME(PCI_VENDOR_ID_PERICOM, 0xb404,
			 pci_fixup_pericom_acs_store_forward);

/*
 * [한국어]
 * nvidia_ion_ahci_fixup - MSI 마스킹이 실제로 동작함을 표시한다
 *
 * @pdev: NVIDIA ION 플랫폼의 AHCI 컨트롤러(0x0ab8)
 * @return: 없음
 *
 * [배경] MSI capability 에는 개별 벡터를 마스킹하는 기능이 선택 사항으로
 * 있고, 지원 여부는 Message Control 레지스터의 Per-Vector Masking 비트로
 * 광고한다. 커널은 그 비트를 보고 마스킹을 쓸지 정한다.
 * [무엇이 문제] 이 장치에는 원본 영어 주석이 없다. 코드가 하는 일로 보아,
 * 마스킹 기능이 실제로는 동작하는데 capability 로 광고하지 않는 경우다.
 * 구체적 배경은 이 트리의 정보만으로는 확인할 수 없다.
 * [우회] PCI_DEV_FLAGS_HAS_MSI_MASKING 을 세워 커널이 마스킹을 쓸 수
 * 있다고 보게 한다. 지금까지 본 대부분의 quirk 가 '광고를 지우는' 쪽이었던
 * 것과 반대로, 이것은 '없는 광고를 더해 주는' 드문 방향이다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [nvidia_ion_ahci_fixup]
 */
static void nvidia_ion_ahci_fixup(struct pci_dev *pdev)
{
	pdev->dev_flags |= PCI_DEV_FLAGS_HAS_MSI_MASKING;	/* [한국어] 이 장치가 MSI 개별 벡터 마스킹을 실제로 지원한다고 표시한다. */
}
/* [한국어] NVIDIA 0x0ab8 에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_NVIDIA, 0x0ab8, nvidia_ion_ahci_fixup);

/*
 * [한국어]
 * rom_bar_overlap_defect - ROM BAR 가 다른 BAR 와 겹치는 결함을 표시한다
 *
 * @dev: Intel 0x1533/0x1536/0x1537/0x1538 (i210 계열 이더넷)
 * @return: 없음
 *
 * [ROM BAR 란] 표준 헤더 오프셋 0x30 에 있는 Expansion ROM Base Address
 * 레지스터다. 장치에 옵션 ROM 이 있으면 그 주소를 여기로 광고한다.
 * [무엇이 문제] 이 quirk 에는 원본 영어 주석이 없다. 이름과 로그 메시지로
 * 보아 ROM BAR 가 다른 BAR 의 주소 범위와 겹치는 결함이다. 구체적인
 * 겹침 양상은 이 트리의 정보만으로는 확인할 수 없다.
 * [그대로 두면] 커널이 ROM BAR 와 다른 BAR 를 각각 독립된 영역으로 잡아
 * 리소스 배정이 어긋난다.
 * [우회] dev->rom_bar_overlap 을 세워 PCI 코어가 그 겹침을 감안해
 * 처리하게 한다.
 *
 * 실행 컨텍스트: EARLY 단계 - BAR 를 읽어 리소스를 만들기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [rom_bar_overlap_defect]
 */
static void rom_bar_overlap_defect(struct pci_dev *dev)
{
	pci_info(dev, "working around ROM BAR overlap defect\n");	/* [한국어] 회피가 적용되었음을 남긴다. */
	dev->rom_bar_overlap = 1;	/* [한국어] PCI 코어의 ROM BAR 처리 코드가 참조하는 플래그. */
}
/* [한국어] 해당 Intel 이더넷 4종에 EARLY 단계로 등록한다. */
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x1533, rom_bar_overlap_defect);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x1536, rom_bar_overlap_defect);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x1537, rom_bar_overlap_defect);
DECLARE_PCI_FIXUP_EARLY(PCI_VENDOR_ID_INTEL, 0x1538, rom_bar_overlap_defect);

/* [한국어] 아래 quirk 는 ASPM 정책 코드가 참조하는 값을 고치므로
 * ASPM 지원이 빌드에 있을 때만 의미가 있다. */
#ifdef CONFIG_PCIEASPM
/*
 * [한국어]
 * aspm_l1_acceptable_latency - 지나치게 엄격한 L1 허용 지연 광고를 완화한다
 *
 * @dev: Intel DG2 계열 그래픽 장치
 * @return: 없음
 *
 * [L1 허용 지연이란] Device Capabilities 레지스터의 Endpoint L1 Acceptable
 * Latency 필드다. 장치가 'L1 에서 L0 로 돌아올 때 이만큼의 지연까지는
 * 견딜 수 있다' 고 광고하는 값으로, 커널의 ASPM 정책은 경로 전체의
 * 복귀 지연이 이 값 안에 들어올 때만 L1 을 켠다.
 * [무엇이 문제] 아래 영어 주석대로, 여러 Intel DG2 그래픽 장치가 1us 만
 * 견딜 수 있다고 광고한다. 그 값으로는 사실상 어떤 경로에서도 L1 을 켤
 * 수 없다. 그런데 실제로는 무제한 지연을 견딜 수 있다.
 * [그대로 두면] 켤 수 있는 ASPM L1 이 켜지지 않아 전력을 낭비한다.
 * [우회] Device Capabilities 값을 최댓값 7(무제한)로 덮어써 L1 이 켜질 수
 * 있게 한다. 하드웨어가 아니라 커널이 기억하는 dev->devcap 을 고친다.
 *
 * [다른 ASPM quirk 와의 대비] 앞의 quirk_disable_aspm_l0s() 는 ASPM 을
 * '끄는' 쪽이었고 이것은 '켤 수 있게 하는' 쪽이다. 하드웨어의 실제 성질에
 * 맞추는 것이 목적이라는 점은 같다.
 *
 * 실행 컨텍스트: HEADER 단계 - ASPM 정책이 계산되기 전이어야 한다.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [aspm_l1_acceptable_latency]
 */
/*
 * Several Intel DG2 graphics devices advertise that they can only tolerate
 * 1us latency when transitioning from L1 to L0, which may prevent ASPM L1
 * from being enabled.  But in fact these devices can tolerate unlimited
 * latency.  Override their Device Capabilities value to allow ASPM L1 to
 * be enabled.
 */
static void aspm_l1_acceptable_latency(struct pci_dev *dev)
{
	u32 l1_lat = FIELD_GET(PCI_EXP_DEVCAP_L1, dev->devcap);	/* [한국어] 현재 광고된 L1 허용 지연 값을 뽑는다. FIELD_GET 이 마스크와 시프트를 대신해 준다. */

	if (l1_lat < 7) {	/* [한국어] 7 이 최댓값(무제한)이므로 그보다 작으면 고칠 여지가 있다. */
		dev->devcap |= FIELD_PREP(PCI_EXP_DEVCAP_L1, 7);	/* [한국어] 값을 7 로 덮어쓴다. FIELD_PREP 이 자리에 맞게 밀어 준다. */
		pci_info(dev, "ASPM: overriding L1 acceptable latency from %#x to 0x7\n",	/* [한국어] 무엇을 무엇으로 바꿨는지 남긴다. */
			 l1_lat);
	}
}
/* [한국어] Intel DG2 계열 그래픽 장치 25종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f80, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f81, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f82, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f83, aspm_l1_acceptable_latency);
/* [한국어] 이어지는 등록 줄: 0x4f84, 0x4f85, 0x4f86, 0x4f87. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f84, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f85, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f86, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f87, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x4f88, aspm_l1_acceptable_latency);
/* [한국어] 이어지는 등록 줄: 0x5690, 0x5691, 0x5692, 0x5693. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x5690, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x5691, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x5692, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x5693, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x5694, aspm_l1_acceptable_latency);
/* [한국어] 이어지는 등록 줄: 0x5695, 0x56a0, 0x56a1, 0x56a2. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x5695, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56a0, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56a1, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56a2, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56a3, aspm_l1_acceptable_latency);
/* [한국어] 이어지는 등록 줄: 0x56a4, 0x56a5, 0x56a6, 0x56b0. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56a4, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56a5, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56a6, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56b0, aspm_l1_acceptable_latency);
/* [한국어] 이어지는 등록 줄: 0x56b1, 0x56c0, 0x56c1. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56b1, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56c0, aspm_l1_acceptable_latency);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x56c1, aspm_l1_acceptable_latency);
/* [한국어] CONFIG_PCIEASPM 블록의 끝. */
#endif

/* [한국어] 아래 quirk 는 DPC(Downstream Port Containment) 관련 값을
 * 고치므로 DPC 지원이 빌드에 있을 때만 의미가 있다. */
#ifdef CONFIG_PCIE_DPC
/*
 * [한국어]
 * dpc_log_size - BIOS 가 지워 버린 DPC RP PIO Log Size 를 되돌린다
 *
 * @dev: Intel Ice Lake / Tiger Lake / Alder Lake 의 내장 썬더볼트 루트 포트
 * @return: 없음
 *
 * [DPC 란] Downstream Port Containment - 하위 포트에서 치명적 오류가 나면
 * 그 아래 링크를 즉시 끊어 오류가 시스템으로 번지는 것을 막는 기능이다.
 * 오류를 일으킨 TLP 의 헤더를 로그로 남기는데, 그 로그의 크기를 DPC
 * Capability 의 RP PIO Log Size 필드가 알려 준다.
 * [무엇이 문제] 아래 영어 주석대로, 이 세대의 BIOS 에 내장 썬더볼트 루트
 * 포트의 RP PIO Log Size 를 0 으로 지워 버리는 버그가 있다.
 * [그대로 두면] 커널이 로그 크기를 0 으로 알고 오류 헤더를 읽지 못한다.
 * [우회] 값이 0 이면 PCIe 표준 헤더 로그 개수로 덮어쓴다.
 *
 * 실행 컨텍스트: HEADER 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [dpc_log_size] -> pci_find_ext_capability()
 */
/*
 * Intel Ice Lake, Tiger Lake and Alder Lake BIOS has a bug that clears
 * the DPC RP PIO Log Size of the integrated Thunderbolt PCIe Root
 * Ports.
 */
static void dpc_log_size(struct pci_dev *dev)
{
	u16 dpc, val;	/* [한국어] dpc: DPC 확장 capability 의 오프셋, val: 그 Capability 레지스터 값. */

	dpc = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_DPC);	/* [한국어] 확장 capability 목록에서 DPC 를 찾는다. */
	if (!dpc)	/* [한국어] DPC 가 없으면 고칠 것이 없다. */
		return;

	pci_read_config_word(dev, dpc + PCI_EXP_DPC_CAP, &val);	/* [한국어] DPC Capability 레지스터를 읽는다. */
	if (!(val & PCI_EXP_DPC_CAP_RP_EXT))	/* [한국어] RP Extensions for DPC 가 없으면 RP PIO Log 자체가 없다. */
		return;

	if (FIELD_GET(PCI_EXP_DPC_RP_PIO_LOG_SIZE, val) == 0) {	/* [한국어] RP PIO Log Size 필드가 0 이면 BIOS 가 지운 것이다 - 정상이라면 0 일 수 없다. */
		pci_info(dev, "Overriding RP PIO Log Size to %d\n",	/* [한국어] 무엇으로 덮어쓰는지 남긴다. */
			 PCIE_STD_NUM_TLP_HEADERLOG);
		dev->dpc_rp_log_size = PCIE_STD_NUM_TLP_HEADERLOG;	/* [한국어] 커널이 기억하는 로그 크기를 PCIe 표준 헤더 로그 개수로 세운다. */
	}
}
/* [한국어] 해당 세대의 내장 썬더볼트 루트 포트 19종에 HEADER 단계로 등록한다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x461f, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x462f, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x463f, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x466e, dpc_log_size);
/* [한국어] 이어지는 등록 줄: 0x8a1d, 0x8a1f, 0x8a21, 0x8a23. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x8a1d, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x8a1f, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x8a21, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x8a23, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a23, dpc_log_size);
/* [한국어] 이어지는 등록 줄: 0x9a25, 0x9a27, 0x9a29, 0x9a2b. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a25, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a27, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a29, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a2b, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a2d, dpc_log_size);
/* [한국어] 이어지는 등록 줄: 0x9a2f, 0x9a31, 0xa72f, 0xa73f. 위 블록 주석의 설명이 그대로 적용되며 대상 ID 만
 * 다르다. */
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a2f, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0x9a31, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0xa72f, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0xa73f, dpc_log_size);
DECLARE_PCI_FIXUP_HEADER(PCI_VENDOR_ID_INTEL, 0xa76e, dpc_log_size);
/* [한국어] CONFIG_PCIE_DPC 블록의 끝. */
#endif

/*
 * For a PCI device with multiple downstream devices, its driver may use
 * a flattened device tree to describe the downstream devices.
 * To overlay the flattened device tree, the PCI device and all its ancestor
 * devices need to have device tree nodes on system base device tree. Thus,
 * before driver probing, it might need to add a device tree node as the final
 * fixup.
 */
/* [한국어] 위 영어 주석이 배경을 밝힌다. 하위 장치가 여럿인 PCI 장치의
 * 드라이버가 flattened device tree 로 그 하위 장치들을 기술하는 경우가
 * 있는데, 그 트리를 겹쳐 놓으려면 해당 PCI 장치와 그 조상들이 모두 시스템
 * 기본 디바이스 트리에 노드를 갖고 있어야 한다. 그래서 드라이버 probe
 * 전에 FINAL 단계에서 디바이스 트리 노드를 만들어 준다.
 *
 * of_pci_make_dev_node() 는 이 파일이 아니라 PCI 코어의 디바이스 트리
 * 지원 코드에 있는 함수로, quirk 함수를 따로 두지 않고 그것을 직접
 * 등록하는 드문 형태다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_XILINX, 0x5020, of_pci_make_dev_node);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_XILINX, 0x5021, of_pci_make_dev_node);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_REDHAT, 0x0005, of_pci_make_dev_node);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_EFAR, 0x9660, of_pci_make_dev_node);

/*
 * [한국어]
 * pci_fixup_d3cold_delay_1sec - D3cold 복귀 후 첫 config 접근까지 1초를 기다린다
 *
 * @pdev: VideoPropulsion(Genroco) Torrent QN16e MPEG QAM 변조기(0x5555:0x0004)
 * @return: 없음
 *
 * [D3cold 란] 장치의 전원이 완전히 끊긴 상태다. 다시 켜면 하드웨어가
 * 처음부터 초기화되므로 config space 에 응답할 준비가 되기까지 시간이
 * 걸린다. PCI 스펙은 그 대기 시간의 하한을 정하고 커널이 그것을 지킨다.
 * [무엇이 문제] 아래 영어 주석대로, 이 장치는 리셋 복구나 D3cold 복귀 후
 * 첫 config 접근까지 표준보다 훨씬 긴 대기가 필요하다.
 * [그대로 두면] 표준 대기만 하고 접근하면 장치가 응답하지 않는다.
 * [우회] dev->d3cold_delay 를 1000ms 로 늘린다. 앞의 quirk_d3hot_delay()
 * 가 D3hot 쪽 대기를 늘린 것과 같은 부류이고, 여기는 D3cold 쪽이다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [pci_fixup_d3cold_delay_1sec]
 */
/*
 * Devices known to require a longer delay before first config space access
 * after reset recovery or resume from D3cold:
 *
 * VideoPropulsion (aka Genroco) Torrent QN16e MPEG QAM Modulator
 */
static void pci_fixup_d3cold_delay_1sec(struct pci_dev *pdev)
{
	pdev->d3cold_delay = 1000;	/* [한국어] D3cold 복귀 후 대기 시간을 1000ms 로 늘린다. */
}
/* [한국어] 해당 장치에 FINAL 단계로 등록한다. 이름 있는 상수가 없어
 * vendor/device 숫자를 그대로 적었다. */
DECLARE_PCI_FIXUP_FINAL(0x5555, 0x0004, pci_fixup_d3cold_delay_1sec);

/* [한국어] 아래 quirk 는 AER 의 정정 가능 오류 마스크를 건드리므로
 * AER 지원이 빌드에 있을 때만 컴파일한다. */
#ifdef CONFIG_PCIEAER
/*
 * [한국어]
 * pci_mask_replay_timer_timeout - 상위 포트에서 Replay Timer Timeout 오류 보고를 막는다
 *
 * @pdev: GLI 0x9750 / 0x9755 (SD 호스트 컨트롤러)
 * @return: 없음
 *
 * [Replay Timer Timeout 이란] PCIe 데이터 링크 계층은 보낸 TLP 에 대한 ACK
 * 이 정해진 시간 안에 오지 않으면 재전송한다. 그 타이머가 만료된 사건이
 * AER 의 정정 가능 오류(Correctable Error)로 보고된다. 정정 가능이므로
 * 데이터는 재전송으로 살아나지만, 로그는 계속 쌓인다.
 * [무엇이 문제] 이 quirk 에는 원본 영어 주석이 없다. 다만 로그 메시지가
 * 'due to %s hardware defect' 라고 밝히듯, 이 장치의 하드웨어 결함 때문에
 * 그 오류가 반복해서 발생한다. 결함의 구체적 내용은 이 트리의 정보만으로는
 * 확인할 수 없다.
 * [그대로 두면] 정정 가능 오류 로그가 끝없이 쌓여 dmesg 를 채운다.
 * [우회] 상위 브리지의 AER Correctable Error Mask 에서 Replay Timer
 * Timeout 비트를 세워 그 오류만 보고되지 않게 한다. 오류 자체를 없애는
 * 것이 아니라 보고를 끄는 것이다.
 *
 * [상위 브리지를 고치는 이유] AER 의 정정 가능 오류는 링크의 반대편,
 * 즉 이 장치가 매달린 상위 포트에서 검출되어 보고된다. 그래서 마스크도
 * 그쪽에 걸어야 한다.
 *
 * 실행 컨텍스트: FINAL 단계.
 *
 * 호출 체인:
 *   pci_do_fixups() -> [pci_mask_replay_timer_timeout]
 *     -> pci_upstream_bridge() -> pci_write_config_dword()
 */
static void pci_mask_replay_timer_timeout(struct pci_dev *pdev)
{
	struct pci_dev *parent = pci_upstream_bridge(pdev);	/* [한국어] 실제로 마스크를 걸 대상인 상위 브리지. */
	u32 val;	/* [한국어] AER Correctable Error Mask 레지스터 값. */

	if (!parent || !parent->aer_cap)	/* [한국어] 상위 브리지가 없거나 그쪽에 AER capability 가 없으면 마스크를 걸 수 없다. */
		return;

	pci_info(parent, "mask Replay Timer Timeout Correctable Errors due to %s hardware defect",	/* [한국어] 어느 장치의 결함 때문에 무엇을 막는지 남긴다. 로그의 주체는 상위 브리지다. */
		 pci_name(pdev));

	pci_read_config_dword(parent, parent->aer_cap + PCI_ERR_COR_MASK, &val);	/* [한국어] AER capability 안의 Correctable Error Mask 레지스터를 읽는다. */
	val |= PCI_ERR_COR_REP_TIMER;	/* [한국어] Replay Timer Timeout 비트를 세운다 - AER 에서 1 은 '보고하지 않음' 을 뜻한다. */
	pci_write_config_dword(parent, parent->aer_cap + PCI_ERR_COR_MASK, val);	/* [한국어] 고친 값을 되쓴다. 다른 오류의 보고 설정은 그대로 보존된다. */
}
/* [한국어] GLI SD 호스트 컨트롤러 2종에 FINAL 단계로 등록한다. */
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_GLI, 0x9750, pci_mask_replay_timer_timeout);
DECLARE_PCI_FIXUP_FINAL(PCI_VENDOR_ID_GLI, 0x9755, pci_mask_replay_timer_timeout);
/* [한국어] CONFIG_PCIEAER 블록의 끝 - 이것이 이 파일의 마지막 줄이다. */
#endif
