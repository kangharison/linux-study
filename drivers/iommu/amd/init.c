// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2007-2010 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 *         Leo Duran <leo.duran@amd.com>
 */

/*
 * [한국어 설명] AMD IOMMU 를 발견하고 초기화하는 부팅 경로 전체 (init.c)
 *
 * === 파일의 역할 ===
 * 펌웨어가 남긴 ACPI IVRS 표를 읽어 "이 기계에 IOMMU 가 몇 개 있고, 각각
 * 어떤 장치를 담당하며, 어떤 특별 요구가 있는가"를 알아낸 뒤, 그에 맞춰
 * 하드웨어를 켜는 것이 이 파일의 일이다.
 *
 * 부팅 경로가 이렇게 큰 이유는 IOMMU 를 켜는 일이 순서에 극히 민감하기
 * 때문이다. 켜는 순간부터 모든 DMA 가 변환을 거치므로, 그 전에 장치 테이블
 * ·명령 버퍼·이벤트 로그가 모두 준비되어 있어야 하고, 펌웨어가 요구한
 * 항등 매핑도 미리 들어가 있어야 한다. 하나라도 어긋나면 부팅 중인
 * 시스템의 디스크나 콘솔이 그 자리에서 멈춘다.
 *
 * 그래서 초기화가 여러 단계로 나뉜다: 표를 훑어 크기를 재고(one pass),
 * 자료구조를 잡고, 다시 훑어 채우고(second pass), 마지막에 켠다. 실패할 수
 * 있는 일을 모두 앞단계에 몰아 두어, 켜는 단계는 되돌릴 필요가 없게 한다.
 *
 * kdump 가 또 하나의 축이다. 크래시한 커널이 IOMMU 를 켜 둔 채 죽었다면
 * 장치들은 여전히 옛 변환을 쓰고 있다. 그것을 무시하고 새 표로 갈아타면
 * 진행 중이던 DMA 가 엉뚱한 곳에 닿는다. 그래서 옛 장치 테이블을 복사해
 * 물려받는 경로가 곳곳에 섞여 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * x86 부팅 경로의 아주 이른 시점에 시작한다. 인터럽트 재매핑을 위한
 * amd_iommu_prepare/enable 은 APIC 초기화보다도 먼저 불리고, DMA 쪽
 * 초기화는 그 뒤에 이어진다.
 *
 * 아래로는 하드웨어 레지스터와 ACPI 표를, 위로는 iommu.c 가 등록할
 * amd_iommu_ops 와 인터럽트 재매핑 코어를 향한다.
 *
 * 실행 컨텍스트: 부팅 초기의 단일 스레드(__init). 서스펜드/레주메 콜백만
 * 나중에 다시 불린다.
 *
 * 호출 체인:
 *   x86 부팅 → amd_iommu_prepare() → early_amd_iommu_init()
 *     → init_iommu_all() → init_iommu_one() → init_iommu_devices()
 *   → amd_iommu_enable() → enable_iommus()
 *   → amd_iommu_init() → amd_iommu_init_dma() (iommu.c 의 ops 등록)
 *
 * === 타 모듈과의 연결 ===
 * ACPI(IVRS 표), PCI(장치 열거와 설정 공간), x86 인터럽트 재매핑
 * (irq_remapping.h), 그리고 iommu-pages.h 의 하드웨어용 페이지 할당기.
 * quirks.c 가 IVRS 의 빠진 항목을 채워 주고, iommu.c 가 여기서 만든
 * 자료구조 위에서 동작한다.
 *
 * 데이터 흐름: IVRS 표 → struct amd_iommu 배열과 세그먼트별 조회 표 →
 * 하드웨어 레지스터. 반대 방향으로는 하드웨어의 능력 비트(EFR)가 올라와
 * 드라이버가 무엇을 쓸지 결정한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - early_amd_iommu_init(): IVRS 를 두 번 훑어 유닛과 표를 모두 준비한다.
 * - init_iommu_all()/init_iommu_one(): 유닛 하나를 발견하고 자원을 갖춘다.
 * - init_device_table_dma()/copy_device_table(): 새 장치 테이블을 짓거나
 *   kdump 에서 옛 것을 물려받는다.
 * - enable_iommus()/early_enable_iommus(): 실제로 하드웨어를 켠다.
 * - init_iommu_from_acpi(): IVHD 항목을 훑어 장치 범위와 특별 장치를 등록한다.
 * - struct ivhd_header/ivhd_entry/ivmd_header: ACPI 표 위에 겹쳐 놓는 형식.
 * - amd_iommu_detect()/prepare()/enable()/reenable(): 바깥에서 부르는 진입점.
 */
#define pr_fmt(fmt)     "AMD-Vi: " fmt	/* [한국어] 이 파일의 로그 접두사 */
#define dev_fmt(fmt)    pr_fmt(fmt)	/* [한국어] dev_err 계열도 같은 접두사 */

#include <linux/pci.h>	/* [한국어] 장치 열거와 설정 공간 접근 */
#include <linux/acpi.h>	/* [한국어] IVRS 표를 찾고 매핑한다 */
#include <linux/list.h>	/* [한국어] 유닛·세그먼트 목록 */
#include <linux/bitmap.h>	/* [한국어] 도메인 id 할당 등에 쓰는 비트맵 */
#include <linux/syscore_ops.h>	/* [한국어] 서스펜드/레주메 콜백 등록 */
#include <linux/interrupt.h>	/* [한국어] 이벤트/PPR/GA 로그 인터럽트 등록 */
#include <linux/msi.h>	/* [한국어] IOMMU 자신의 MSI 설정 */
#include <linux/irq.h>	/* [한국어] 인터럽트 코어 타입 */
#include <linux/amd-iommu.h>	/* [한국어] 드라이버 외부 인터페이스 */
#include <linux/export.h>	/* [한국어] 다른 모듈에 심볼을 내보낸다 */
#include <linux/kmemleak.h>	/* [한국어] early 할당한 메모리를 누수 검사에서 제외하는 데 쓴다 */
#include <linux/cc_platform.h>	/* [한국어] 기밀 컴퓨팅(SEV 등) 환경 판별 */
#include <linux/iopoll.h>	/* [한국어] 레지스터가 원하는 상태가 될 때까지 도는 도우미 */
#include <asm/pci-direct.h>	/* [한국어] PCI 서브시스템 초기화 전에 설정 공간을 직접 읽는다 */
#include <asm/iommu.h>	/* [한국어] 아키텍처별 IOMMU 정의 */
#include <asm/apic.h>	/* [한국어] APIC 초기화 순서와 x2APIC 판별 */
#include <asm/gart.h>	/* [한국어] 예전 GART IOMMU 와의 공존 처리 */
#include <asm/x86_init.h>	/* [한국어] 부팅 초기 훅 등록 */
#include <asm/io_apic.h>	/* [한국어] IOAPIC 정보 — 인터럽트 재매핑 대상이다 */
#include <asm/irq_remapping.h>	/* [한국어] 아키텍처별 재매핑 인터페이스 */
#include <asm/set_memory.h>	/* [한국어] SEV 환경에서 페이지의 암호화 속성을 바꾼다 */
#include <asm/sev.h>	/* [한국어] SEV-SNP 관련 판별 */

#include <linux/crash_dump.h>	/* [한국어] kdump 커널인지 판별 — 물려받기 경로의 분기 기준 */

#include "amd_iommu.h"	/* [한국어] 드라이버 내부 함수 선언 */
#include "../irq_remapping.h"	/* [한국어] 코어의 벤더 중립 재매핑 인터페이스 */
#include "../iommu-pages.h"	/* [한국어] 하드웨어가 읽을 표를 잡는 공용 할당기 */

/*
 * definitions for the ACPI scanning code
 */
#define IVRS_HEADER_LENGTH 48	/* [한국어] IVRS 표의 고정 헤더 길이. 그 뒤부터 IVHD/IVMD 항목이 이어진다 */

#define ACPI_IVHD_TYPE_MAX_SUPPORTED	0x40	/* [한국어] 드라이버가 이해하는 최대 IVHD 타입. 그보다 큰 타입은 건너뛴다 */
#define ACPI_IVMD_TYPE_ALL              0x20	/* [한국어] 모든 장치에 적용되는 메모리 정의 */
#define ACPI_IVMD_TYPE                  0x21	/* [한국어] 장치 하나에 적용되는 메모리 정의 */
#define ACPI_IVMD_TYPE_RANGE            0x22	/* [한국어] 장치 범위에 적용되는 메모리 정의 */

#define IVHD_DEV_ALL                    0x01	/* [한국어] 이 유닛이 모든 장치를 담당한다 */
#define IVHD_DEV_SELECT                 0x02	/* [한국어] 장치 하나를 지정 */
#define IVHD_DEV_SELECT_RANGE_START     0x03	/* [한국어] 장치 범위의 시작 — 다음에 RANGE_END 가 와야 짝이 맞는다 */
#define IVHD_DEV_RANGE_END              0x04	/* [한국어] 그 범위의 끝 */
#define IVHD_DEV_ALIAS                  0x42	/* [한국어] 장치 하나와 그 별칭 id */
#define IVHD_DEV_ALIAS_RANGE            0x43	/* [한국어] 범위 전체가 하나의 별칭을 쓴다 — 브리지 뒤 장치들이 그렇다 */
#define IVHD_DEV_EXT_SELECT             0x46	/* [한국어] 확장 플래그가 딸린 장치 하나 */
#define IVHD_DEV_EXT_SELECT_RANGE       0x47	/* [한국어] 확장 플래그가 딸린 범위 */
#define IVHD_DEV_SPECIAL		0x48	/* [한국어] IOAPIC/HPET 같은 특수 장치. PCI 열거로 발견되지 않아 표가 알려 줘야 한다 */
#define IVHD_DEV_ACPI_HID		0xf0	/* [한국어] ACPI HID 로만 식별되는 플랫폼 장치 */

#define UID_NOT_PRESENT                 0	/* [한국어] ACPI HID 항목에 UID 가 없다 */
#define UID_IS_INTEGER                  1	/* [한국어] UID 가 정수로 주어졌다 */
#define UID_IS_CHARACTER                2	/* [한국어] UID 가 문자열로 주어졌다 */

#define IVHD_SPECIAL_IOAPIC		1	/* [한국어] 특수 장치가 IOAPIC 임을 뜻하는 값 */
#define IVHD_SPECIAL_HPET		2	/* [한국어] HPET 임을 뜻하는 값 */

#define IVHD_FLAG_HT_TUN_EN_MASK        0x01	/* [한국어] HyperTransport 터널 변환을 켜라는 펌웨어 지시 */
#define IVHD_FLAG_PASSPW_EN_MASK        0x02	/* [한국어] posted write 통과를 켜라는 지시 */
#define IVHD_FLAG_RESPASSPW_EN_MASK     0x04	/* [한국어] 응답 통과를 켜라는 지시 */
#define IVHD_FLAG_ISOC_EN_MASK          0x08	/* [한국어] 등시성 트래픽 처리를 켜라는 지시 */

#define IVMD_FLAG_EXCL_RANGE            0x08	/* [한국어] 이 메모리 구간을 변환에서 제외하라 */
#define IVMD_FLAG_IW                    0x04	/* [한국어] 쓰기 권한을 주라 */
#define IVMD_FLAG_IR                    0x02	/* [한국어] 읽기 권한을 주라 */
#define IVMD_FLAG_UNITY_MAP             0x01	/* [한국어] IOVA == PA 로 항등 매핑하라 */

#define ACPI_DEVFLAG_INITPASS           0x01	/* [한국어] INIT 인터럽트를 재매핑 없이 통과시켜라 */
#define ACPI_DEVFLAG_EXTINT             0x02	/* [한국어] 외부 인터럽트를 통과시켜라 */
#define ACPI_DEVFLAG_NMI                0x04	/* [한국어] NMI 를 통과시켜라 — 재매핑하면 놓칠 수 있어서 */
#define ACPI_DEVFLAG_SYSMGT1            0x10	/* [한국어] 시스템 관리 메시지 처리 방식 비트 1 */
#define ACPI_DEVFLAG_SYSMGT2            0x20	/* [한국어] 같은 필드의 비트 2 */
#define ACPI_DEVFLAG_LINT0              0x40	/* [한국어] 로컬 인터럽트 0 을 통과시켜라 */
#define ACPI_DEVFLAG_LINT1              0x80	/* [한국어] 로컬 인터럽트 1 을 통과시켜라 */
#define ACPI_DEVFLAG_ATSDIS             0x10000000	/* [한국어] 이 장치의 ATS 를 쓰지 말라. 결함이 알려진 장치를 위한 지시다 */

/* [한국어] IVRS 표의 세그먼트/버스/장치/기능을 드라이버의 조회 키로 합친다.
 * 표는 네 값을 따로 주지만 내부에서는 항상 합친 32비트로 다룬다. */
#define IVRS_GET_SBDF_ID(seg, bus, dev, fn)	(((seg & 0xffff) << 16) | ((bus & 0xff) << 8) \
						 | ((dev & 0x1f) << 3) | (fn & 0x7))

/*
 * ACPI table definitions
 *
 * These data structures are laid over the table to parse the important values
 * out of it.
 */

/*
 * structure describing one IOMMU in the ACPI table. Typically followed by one
 * or more ivhd_entrys.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct ivhd_header — IVRS 표 안의 IOMMU 하나에 대한 서술
 *
 * ACPI 표 위에 그대로 겹쳐 놓는 형식이라 packed 다 — 컴파일러가 정렬을
 * 위해 빈 공간을 넣으면 필드가 어긋난다.
 *
 * 뒤에 ivhd_entry 가 여러 개 이어 붙는다. length 가 그 전체 길이를
 * 알려 주므로, 파서는 헤더 뒤부터 length 까지를 항목 구간으로 본다.
 *
 * 타입(type)에 따라 헤더의 크기가 다른 것이 이 구조체의 함정이다. 10h
 * 타입에는 efr_reg 가 없고, 11h/40h 에만 있다. 그래서 그 필드를 읽기 전에
 * 반드시 타입을 확인해야 한다.
 */
struct ivhd_header {
	u8 type;
	/* [한국어] IVHD 타입(10h/11h/40h). 헤더의 크기와 뒤따르는 항목의 형식을 결정한다.
	 * 읽는 자: 파서가 가장 먼저 보는 값이며, 지원하지 않는 타입은 건너뛴다. */
	u8 flags;
	/* [한국어] 이 유닛에 적용할 설정 플래그(IVHD_FLAG_*).
	 * HT 터널, posted write 통과 등 펌웨어가 요구하는 동작을 지시한다.
	 * 읽는 자: init_iommu_one_late() 가 제어 레지스터에 반영한다. */
	u16 length;
	/* [한국어] 이 IVHD 항목의 전체 길이(헤더 + 이어지는 장치 항목들).
	 * 파서가 항목 구간의 끝을 아는 유일한 근거다. 표가 스스로 크기를 알려 주는
	 *   형식이라 이 값 없이는 다음 IVHD 의 시작을 찾을 수 없다. */
	u16 devid;
	/* [한국어] 이 IOMMU 자신의 요청자 id.
	 * IOMMU 도 표를 읽느라 DMA 를 내므로 자기 id 를 갖는다. */
	u16 cap_ptr;
	/* [한국어] PCI 설정 공간에서 이 IOMMU 능력 구조의 오프셋.
	 * 한 PCI 함수에 IOMMU 능력이 여럿 있을 수 있어 유닛마다 다르다. */
	u64 mmio_phys;
	/* [한국어] 이 유닛의 MMIO 영역 물리 시작 주소.
	 * 드라이버가 가장 먼저 매핑하는 것이 이 주소다 — 그래야 능력 비트를 읽을 수 있다. */
	u16 pci_seg;
	/* [한국어] 이 유닛이 담당하는 PCI 세그먼트 번호.
	 * 세그먼트마다 장치 테이블과 조회 표가 따로 있어, 유닛을 어느 세그먼트에
	 *   붙일지가 이 값으로 정해진다. */
	u16 info;
	/* [한국어] MSI 번호 등 부가 정보. */
	u32 efr_attr;
	/* [한국어] 확장 기능의 일부를 담은 필드(타입 10h 에서 쓰인다).
	 * 11h 이상에서는 아래 efr_reg 가 정확한 사본을 주므로 이쪽은 참고용이다. */

	/* Following only valid on IVHD type 11h and 40h */
	u64 efr_reg; /* Exact copy of MMIO_EXT_FEATURES */
	/* [한국어] (원 주석: 11h 와 40h 타입에서만 유효)
	 * MMIO_EXT_FEATURES 레지스터의 정확한 사본이다(원 주석: Exact copy).
	 * 왜 표에 사본을 두는가: MMIO 를 매핑하기 전에도 능력을 알아야 하는 결정이
	 *   있다. 특히 인터럽트 재매핑은 매핑보다 먼저 판단해야 한다.
	 * 주의: 타입 10h 헤더에는 이 필드가 아예 없으므로 읽기 전에 타입을 확인해야 한다. */
	u64 efr_reg2;
	/* [한국어] 두 번째 확장 기능 워드의 사본. 같은 제약이 적용된다. */
} __attribute__((packed));

/*
 * A device entry describing which devices a specific IOMMU translates and
 * which requestor ids they use.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct ivhd_entry — 이 IOMMU 가 담당하는 장치 하나(또는 범위)의 서술
 *
 * 이것도 표에 겹쳐 놓는 형식이지만, 위 헤더와 달리 항목마다 실제 크기가
 * 다르다. type 값에 따라 4바이트짜리도 있고 32바이트짜리도 있어서, 파서는
 * 타입을 보고 얼마나 건너뛸지 정한다 — 이 구조체의 sizeof 를 쓰면 안 된다.
 *
 * struct_group(ext_hid, ...) 은 ext 와 hidh 를 한 덩어리로 다룰 수 있게
 * 묶은 것이다. ACPI HID 항목에서는 그 8바이트가 두 개의 플래그가 아니라
 * HID 문자열이라, 통째로 복사해야 한다.
 */
struct ivhd_entry {
	u8 type;
	/* [한국어] 항목의 종류(IVHD_DEV_*). 이 값이 항목의 실제 크기까지 결정한다.
	 * 읽는 자: init_iommu_from_acpi() 의 switch. 크기를 이 값으로 계산해
	 *   다음 항목으로 건너뛴다 — sizeof 로는 알 수 없다. */
	u16 devid;
	/* [한국어] 대상 장치의 요청자 id. 범위 항목에서는 시작 또는 끝을 나타낸다. */
	u8 flags;
	/* [한국어] 그 장치의 DTE 에 반영할 플래그(ACPI_DEVFLAG_*).
	 * NMI 통과나 시스템 관리 메시지 처리 같은 펌웨어의 지시다. */
	struct_group(ext_hid,
		u32 ext;
		/* [한국어] 확장 항목의 첫 워드. ATS 비활성 지시 등이 들어 있다.
		 * ACPI HID 항목에서는 이 자리가 HID 문자열의 앞부분이 된다 —
		 *   그래서 아래 hidh 와 함께 struct_group 으로 묶여 있다. */
		u32 hidh;
		/* [한국어] 확장 항목의 둘째 워드, 또는 HID 문자열의 뒷부분. */
	);
	u64 cid;
	/* [한국어] ACPI CID(호환 id) 문자열. */
	u8 uidf;
	/* [한국어] UID 의 형식 — 없음/정수/문자열 중 하나.
	 * 이 값에 따라 아래 uid 를 어떻게 읽을지가 갈린다. */
	u8 uidl;
	/* [한국어] UID 의 길이(문자열인 경우). */
	u8 uid;
	/* [한국어] UID 의 첫 바이트. 실제로는 uidl 만큼 이어지는 가변 길이라,
	 *   이 필드의 주소를 시작점으로 삼아 읽는다.
	 *   구조체 정의로는 표현할 수 없는 부분이다. */
} __attribute__((packed));

/*
 * An AMD IOMMU memory definition structure. It defines things like exclusion
 * ranges for devices and regions that should be unity mapped.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * struct ivmd_header — 특정 장치(들)에 필요한 메모리 구간의 서술
 *
 * 펌웨어가 "이 장치는 이 물리 주소 구간을 그대로 써야 한다"고 알리는
 * 수단이다. 부팅 전부터 진행 중인 DMA 나 펌웨어와의 통신 버퍼가 그런
 * 구간이며, IOMMU 를 켜면서 그 주소가 갑자기 변환 대상이 되면 통신이 끊긴다.
 *
 * flags 가 그 구간을 어떻게 다룰지 정한다 — 항등 매핑할지(UNITY_MAP),
 * 아예 변환에서 제외할지(EXCL_RANGE), 어떤 권한을 줄지.
 *
 * type 이 적용 대상을 정한다: 모든 장치, 장치 하나, 또는 장치 범위.
 */
struct ivmd_header {
	u8 type;
	/* [한국어] 적용 대상 — 모든 장치(ALL), 장치 하나, 또는 장치 범위. */
	u8 flags;
	/* [한국어] 이 구간을 어떻게 다룰지(IVMD_FLAG_*).
	 * 항등 매핑할지, 변환에서 제외할지, 어떤 권한을 줄지를 정한다. */
	u16 length;
	/* [한국어] 이 IVMD 항목의 길이. 파서가 다음 항목으로 건너뛰는 근거다. */
	u16 devid;
	/* [한국어] 대상 장치의 id. 범위 타입에서는 시작 장치다. */
	u16 aux;
	/* [한국어] 범위 타입에서 끝 장치의 id. */
	u16 pci_seg;
	/* [한국어] 대상 세그먼트. */
	u8  resv[6];
	/* [한국어] 예약. 표의 자리를 맞추기 위한 공백이다. */
	u64 range_start;
	/* [한국어] 그 메모리 구간의 시작 물리 주소.
	 * 항등 매핑이면 이 주소가 곧 IOVA 이기도 하다. */
	u64 range_length;
	/* [한국어] 구간의 길이.
	 * 이 구간만큼은 장치가 물리 메모리에 직접 닿게 되므로, IOMMU 격리의
	 *   의도된 예외다. */
} __attribute__((packed));

bool amd_iommu_dump;	/* [한국어] 부팅 옵션으로 켜는 상세 로그. IVRS 항목을 하나하나 찍는 데 쓴다 */
bool amd_iommu_irq_remap __read_mostly;	/* [한국어] 인터럽트 재매핑이 실제로 동작 중인가. 초기화가 성공해야 참이 된다 */

enum protection_domain_mode amd_iommu_pgtable = PD_MODE_V1;	/* [한국어] 기본 페이지 테이블 형식. v2 는 하드웨어와 옵션이 허락할 때만 쓴다 */
/* Host page table level */
u8 amd_iommu_hpt_level;	/* [한국어] 호스트 페이지 테이블의 레벨 수 (원 주석). 하드웨어의 HATS 능력에서 정해진다 */
/* Guest page table level */
int amd_iommu_gpt_level = PAGE_MODE_4_LEVEL;	/* [한국어] 게스트 페이지 테이블의 레벨 수 (원 주석). 기본은 4단계(48비트) */

int amd_iommu_guest_ir = AMD_IOMMU_GUEST_IR_VAPIC;	/* [한국어] 인터럽트 전달 모드. 최선을 기본으로 두고, 조건이 안 되면 아래로 내린다 */
static int amd_iommu_xt_mode = IRQ_REMAP_XAPIC_MODE;	/* [한국어] x2APIC 을 쓸지. 능력을 확인한 뒤 올라간다 */

static bool amd_iommu_detected;	/* [한국어] IVRS 표를 찾아 IOMMU 의 존재를 확인했는가 */
static bool amd_iommu_disabled __initdata;	/* [한국어] 커널 명령줄로 껐는가 */
static bool amd_iommu_force_enable __initdata;	/* [한국어] 알려진 문제가 있어도 강제로 켜라는 옵션 */
static bool amd_iommu_irtcachedis;	/* [한국어] 인터럽트 재매핑 캐시를 끌지. 특정 errata 우회용 */
static int amd_iommu_target_ivhd_type;	/* [한국어] 이 기계에서 쓸 IVHD 타입. 표에 여러 타입이 섞여 있어 하나를 골라야 한다 */

/* Global EFR and EFR2 registers */
u64 amd_iommu_efr;	/* [한국어] 모든 유닛의 공통 확장 기능 (원 주석). 유닛마다 다르면 공통분만 남는다 */
u64 amd_iommu_efr2;	/* [한국어] 두 번째 기능 워드의 공통분 */

/* Host (v1) page table is not supported*/
bool amd_iommu_hatdis;	/* [한국어] 호스트 페이지 테이블(v1)을 쓸 수 없다 (원 주석). 펌웨어가 그렇게 지시한 경우 */

/* SNP is enabled on the system? */
bool amd_iommu_snp_en;	/* [한국어] SEV-SNP 가 켜져 있는가 (원 주석). PPR 로그의 errata 처리 등이 이 값에 따라 갈린다 */
EXPORT_SYMBOL(amd_iommu_snp_en);	/* [한국어] KVM 등 다른 모듈이 이 상태를 알아야 한다 */

LIST_HEAD(amd_iommu_pci_seg_list);	/* list of all PCI segments */	/* [한국어] 모든 PCI 세그먼트 (원 주석) */
LIST_HEAD(amd_iommu_list);		/* list of all AMD IOMMUs in the system */	/* [한국어] 시스템의 모든 AMD IOMMU (원 주석) */
LIST_HEAD(amd_ivhd_dev_flags_list);	/* list of all IVHD device entry settings */	/* [한국어] IVHD 가 지정한 장치별 DTE 설정 (원 주석) */

/* Number of IOMMUs present in the system */
static int amd_iommus_present;	/* [한국어] 발견된 유닛의 수 (원 주석). 상한 검사에 쓴다 */

/* IOMMUs have a non-present cache? */
bool amd_iommu_np_cache __read_mostly;	/* [한국어] 존재하지 않는 항목도 캐시하는가 (원 주석). 그렇다면 매핑 생성 시에도 무효화가 필요하다 */
bool amd_iommu_iotlb_sup __read_mostly = true;	/* [한국어] 모든 유닛이 장치 IOTLB 를 지원하는가. 참으로 시작해 하나라도 못 하면 내린다 */

static bool amd_iommu_pc_present __read_mostly;	/* [한국어] 성능 카운터가 있는가 */
bool amdr_ivrs_remap_support __read_mostly;	/* [한국어] IVRS 가 "부팅 전부터 재매핑이 켜져 있었다"고 알리는가 */

bool amd_iommu_force_isolation __read_mostly;	/* [한국어] 장치를 그룹으로 묶지 않고 하나씩 격리할지 */

unsigned long amd_iommu_pgsize_bitmap __ro_after_init = AMD_IOMMU_PGSIZES;	/* [한국어] 코어에 알릴 지원 페이지 크기. 초기화가 끝나면 읽기 전용이 된다 */

/*
 * [한국어] enum iommu_init_state — 초기화가 어디까지 진행됐는가
 *
 * IOMMU 초기화는 한 함수에서 끝나지 않는다. 인터럽트 재매핑을 위해 아주
 * 이른 시점에 시작해서, PCI 열거가 끝난 뒤에 이어지고, 마지막에 DMA 계층에
 * 등록된다. 그 사이사이 다른 서브시스템이 끼어든다.
 *
 * 그래서 "지금 어디까지 왔는가"를 기억할 상태가 필요하다. 이 값이 두 가지
 * 일을 한다:
 *  - 다음 단계가 전제 조건을 만족하는지 확인한다.
 *  - 실패했을 때 어디까지 되감아야 하는지 정한다. 상태가 없으면 무엇을
 *    이미 만들었는지 알 수 없어 정확히 되돌릴 수 없다.
 *
 * 뒤쪽 세 값(NOT_FOUND, INIT_ERROR, CMDLINE_DISABLED)은 진행 단계가 아니라
 * 종착점이다. 셋을 구별해 두는 이유: "IOMMU 가 없다"와 "있는데 실패했다"와
 * "사용자가 껐다"는 이후 동작이 서로 다르다.
 */
enum iommu_init_state {
	IOMMU_START_STATE,
	/* [한국어] 아무것도 시작하지 않은 상태. */
	IOMMU_IVRS_DETECTED,
	/* [한국어] IVRS 표를 찾아 IOMMU 의 존재를 확인했다.
	 * 아직 하드웨어에는 손대지 않았다. */
	IOMMU_ACPI_FINISHED,
	/* [한국어] 표를 모두 파싱하고 자료구조를 만들었다.
	 * 이 시점에 유닛 목록과 세그먼트별 조회 표가 완성된다. */
	IOMMU_ENABLED,
	/* [한국어] 하드웨어를 켰다. 이 순간부터 모든 DMA 가 변환을 거친다. */
	IOMMU_PCI_INIT,
	/* [한국어] PCI 서브시스템과의 연결이 끝났다.
	 * 표의 장치 경로를 실제 struct pci_dev 와 이을 수 있게 된다. */
	IOMMU_INTERRUPTS_EN,
	/* [한국어] 이벤트/PPR/GA 로그 인터럽트를 등록했다.
	 * 이제부터 하드웨어의 보고를 받을 수 있다. */
	IOMMU_INITIALIZED,
	/* [한국어] DMA 계층까지 등록을 마쳤다. 초기화 완료. */
	IOMMU_NOT_FOUND,
	/* [한국어] 이 기계에 IOMMU 가 없다. 오류가 아니라 사실이다. */
	IOMMU_INIT_ERROR,
	/* [한국어] 있는데 초기화에 실패했다. 되감기가 필요한 상태다. */
	IOMMU_CMDLINE_DISABLED,
	/* [한국어] 사용자가 명령줄로 껐다.
	 * NOT_FOUND 와 구별하는 이유: 하드웨어는 있으므로, 다른 코드가
	 *   "IOMMU 가 있었다면"을 전제로 하는 판단을 달리해야 한다. */
};

/* Early ioapic and hpet maps from kernel command line */
#define EARLY_MAP_SIZE		4	/* [한국어] 명령줄로 지정할 수 있는 대응의 최대 개수 (원 주석) */
static struct devid_map __initdata early_ioapic_map[EARLY_MAP_SIZE];	/* [한국어] 명령줄의 ivrs_ioapic= 값을 담아 두는 곳. 목록 자료구조가 준비되기 전이라 배열이다 */
static struct devid_map __initdata early_hpet_map[EARLY_MAP_SIZE];	/* [한국어] 같은 목적의 HPET 용 */
static struct acpihid_map_entry __initdata early_acpihid_map[EARLY_MAP_SIZE];	/* [한국어] 같은 목적의 ACPI HID 용 */

static int __initdata early_ioapic_map_size;	/* [한국어] 그 배열에 실제로 담긴 개수 */
static int __initdata early_hpet_map_size;	/* [한국어] 같은 목적 */
static int __initdata early_acpihid_map_size;	/* [한국어] 같은 목적 */

static bool __initdata cmdline_maps;	/* [한국어] 명령줄로 지정된 대응이 하나라도 있는가. 있으면 표의 값보다 우선한다 */

static enum iommu_init_state init_state = IOMMU_START_STATE;	/* [한국어] 초기화가 어디까지 진행됐는지. 실패 시 되감기 범위를 이 값으로 정한다 */

static int amd_iommu_enable_interrupts(void);	/* [한국어] 아래에서 정의되지만 상태 기계가 먼저 쓴다 */
static void init_device_table_dma(struct amd_iommu_pci_seg *pci_seg);	/* [한국어] 같은 이유의 전방 선언 */

static bool amd_iommu_pre_enabled = true;	/* [한국어] 물려받은 변환이 있다고 일단 가정한다. 확인 후 내려간다 — 안전한 쪽을 기본으로 */

static u32 amd_iommu_ivinfo __initdata;	/* [한국어] IVRS 표의 IVinfo 필드 사본. 펌웨어가 알리는 전역 설정이 들어 있다 */

/*
 * [한국어]
 * translation_pre_enabled - 커널 진입 전부터 변환이 켜져 있었는가
 *
 * @iommu: 대상 유닛.
 * @return: 물려받은 상태이면 참.
 *
 * kdump 경로의 판단 기준이다. 크래시한 커널이 IOMMU 를 켜 둔 채 죽었다면
 * 장치들은 여전히 그 변환을 쓰고 있고, 진행 중이던 DMA 도 있을 수 있다.
 * 그것을 무시하고 새 표로 갈아타면 그 DMA 가 엉뚱한 곳에 닿는다.
 *
 * 그래서 이 플래그가 참이면 초기화가 다른 길을 간다 — 옛 장치 테이블을
 * 복사해 물려받고, 하드웨어를 끄지 않은 채 새 표로 넘긴다.
 *
 * 호출 체인:
 *   copy_device_table()/early_enable_iommus() → [이 함수]
 */
bool translation_pre_enabled(struct amd_iommu *iommu)
{
	return (iommu->flags & AMD_IOMMU_FLAG_TRANS_PRE_ENABLED);	/* [한국어] 진입 시점에 이미 켜져 있었다는 표시 */
}

/*
 * [한국어]
 * clear_translation_pre_enabled - "물려받음" 표시를 지운다
 *
 * @iommu: 대상 유닛.
 *
 * 옛 표에서 새 표로 완전히 넘어갔거나, 물려받기를 포기하고 하드웨어를
 * 껐을 때 불린다. 이후로는 일반 경로와 똑같이 다룬다.
 *
 * 호출 체인:
 *   copy_device_table() 실패 경로/early_enable_iommus() → [이 함수]
 */
static void clear_translation_pre_enabled(struct amd_iommu *iommu)
{
	iommu->flags &= ~AMD_IOMMU_FLAG_TRANS_PRE_ENABLED;	/* [한국어] 표시를 내린다 — 이후로는 일반 경로와 같이 다룬다 */
}

/*
 * [한국어]
 * init_translation_status - 하드웨어에게 지금 변환이 켜져 있는지 묻는다
 *
 * @iommu: 대상 유닛.
 *
 * 제어 레지스터의 IOMMU_EN 비트를 읽어 플래그를 세운다. 이 한 번의 판독이
 * 이후 kdump 경로 전체의 분기 기준이 되므로, MMIO 를 매핑한 직후에
 * 불려야 한다 — 그 사이에 드라이버가 무엇을 켜면 판별이 무의미해진다.
 *
 * Intel 쪽과 달리 제어 레지스터를 그대로 읽을 수 있다. AMD 의 CONTROL 은
 * 읽기/쓰기 모두 가능해서, gcmd 같은 소프트웨어 사본이 필요 없다.
 *
 * 호출 체인:
 *   init_iommu_one() → [이 함수]
 */
static void init_translation_status(struct amd_iommu *iommu)
{
	u64 ctrl;	/* [한국어] 제어 레지스터 값 */

	ctrl = readq(iommu->mmio_base + MMIO_CONTROL_OFFSET);	/* [한국어] AMD 의 CONTROL 은 읽을 수 있어 소프트웨어 사본이 필요 없다 */
	if (ctrl & (1<<CONTROL_IOMMU_EN))	/* [한국어] 이미 켜져 있는가 */
		iommu->flags |= AMD_IOMMU_FLAG_TRANS_PRE_ENABLED;	/* [한국어] 물려받은 상태임을 기록. 이후 kdump 경로 전체의 분기 기준이 된다 */
}

/*
 * [한국어]
 * amd_iommu_get_num_iommus - 발견된 IOMMU 유닛의 수
 *
 * @return: 유닛 개수.
 *
 * 성능 카운터 코드가 "이 시스템에 유닛이 몇 개인가"를 알아야 해서 밖으로
 * 열어 둔 접근자다. 목록을 직접 훑게 하지 않는 이유는 그 목록이 이 파일의
 * 내부 상태이기 때문이다.
 */
int amd_iommu_get_num_iommus(void)
{
	return amd_iommus_present;	/* [한국어] 목록을 밖에 노출하지 않기 위한 접근자 */
}

/*
 * [한국어]
 * amd_iommu_ht_range_ignore - HyperTransport 주소 범위를 예약하지 않아도 되는가
 *
 * @return: 하드웨어가 그 범위를 무시해도 된다고 알리면 참.
 *
 * HyperTransport 는 물리 주소 공간의 일부를 자기 용도로 쓴다. 예전에는
 * 그 구간을 IOVA 할당에서 빼 두어야 했다 — 장치가 그 주소로 DMA 를 내면
 * 메모리가 아니라 HT 링크로 가기 때문이다.
 *
 * 최근 하드웨어는 그 구간도 정상적으로 변환하므로 예약할 필요가 없고,
 * 그만큼 IOVA 공간이 넓어진다. 이 능력 비트가 그것을 알린다.
 *
 * 호출 체인:
 *   iommu.c 의 예약 영역 계산 → [이 함수]
 */
bool amd_iommu_ht_range_ignore(void)
{
	return check_feature2(FEATURE_HT_RANGE_IGNORE);	/* [한국어] 참이면 HT 구간을 IOVA 에서 예약하지 않아도 되어 주소 공간이 넓어진다 */
}

/*
 * Iterate through all the IOMMUs to get common EFR
 * masks among all IOMMUs and warn if found inconsistency.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * get_global_efr - 모든 유닛이 공통으로 지원하는 기능만 남긴다
 *
 * 드라이버는 유닛마다 다른 기능을 쓸 수 없다. 한 유닛에서만 되는 기능을
 * 상위 계층에 광고하면, 그 유닛에 붙지 않은 장치에서 실패한다. 그래서
 * 전역 능력값은 모든 유닛의 EFR 을 AND 한 것이 된다.
 *
 * 첫 유닛의 값으로 시작해 나머지를 AND 해 가는 구조다. 값이 다른 유닛을
 * 만나면 FW_BUG 으로 보고하는 이유: 같은 시스템의 IOMMU 들이 서로 다른
 * 능력을 광고하는 것은 정상적인 하드웨어 구성이 아니라 펌웨어가 표를
 * 잘못 채운 경우가 대부분이다.
 *
 * 보고만 하고 진행하는 것은 실용적인 선택이다 — 공통분으로 내려가면
 * 적어도 동작은 한다.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → [이 함수]
 */
static __init void get_global_efr(void)
{
	struct amd_iommu *iommu;	/* [한국어] 유닛 순회용 */

	for_each_iommu(iommu) {
		u64 tmp = iommu->features;	/* [한국어] 이 유닛의 능력 */
		u64 tmp2 = iommu->features2;	/* [한국어] 두 번째 워드 */

		if (list_is_first(&iommu->list, &amd_iommu_list)) {	/* [한국어] 첫 유닛이면 기준값이 된다 */
			amd_iommu_efr = tmp;	/* [한국어] 그대로 시작값으로 */
			amd_iommu_efr2 = tmp2;	/* [한국어] 두 번째 워드도 */
			continue;	/* [한국어] 다음 유닛으로 */
		}

		if (amd_iommu_efr == tmp &&	/* [한국어] 기준과 같고 */
		    amd_iommu_efr2 == tmp2)	/* [한국어] 두 번째도 같으면 */
			continue;	/* [한국어] 일치하므로 조정할 것이 없다 */

		pr_err(FW_BUG	/* [한국어] 같은 시스템의 유닛들이 다른 능력을 광고하는 것은 대개 펌웨어가 표를 잘못 채운 경우다 */
		       "Found inconsistent EFR/EFR2 %#llx,%#llx (global %#llx,%#llx) on iommu%d (%04x:%02x:%02x.%01x).\n",
		       tmp, tmp2, amd_iommu_efr, amd_iommu_efr2,
		       iommu->index, iommu->pci_seg->id,
		       PCI_BUS_NUM(iommu->devid), PCI_SLOT(iommu->devid),
		       PCI_FUNC(iommu->devid));

		amd_iommu_efr &= tmp;	/* [한국어] 공통분만 남긴다 — 한 유닛에서만 되는 기능은 쓸 수 없다 */
		amd_iommu_efr2 &= tmp2;	/* [한국어] 두 번째 워드도 같은 방식 */
	}

	pr_info("Using global IVHD EFR:%#llx, EFR2:%#llx\n", amd_iommu_efr, amd_iommu_efr2);	/* [한국어] 최종적으로 쓸 능력을 남긴다 */
}

/*
 * For IVHD type 0x11/0x40, EFR is also available via IVHD.
 * Default to IVHD EFR since it is available sooner
 * (i.e. before PCI init).
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * early_iommu_features_init - IVHD 표에서 능력 비트를 미리 읽어 둔다
 *
 * @iommu: 대상 유닛.
 * @h: 그 유닛의 IVHD 헤더.
 *
 * 원 주석이 이유를 밝힌다: 타입 11h/40h 의 IVHD 에는 EFR 레지스터의 사본이
 * 들어 있고, 그것이 MMIO 보다 먼저 읽힌다(PCI 초기화 전에).
 *
 * 왜 먼저 알아야 하는가: 인터럽트 재매핑을 켤지는 APIC 초기화보다도 먼저
 * 결정해야 하는데, 그 시점에는 아직 MMIO 를 매핑하지 못한 유닛이 있을 수
 * 있다. 표의 사본이 그 간극을 메운다.
 *
 * IVINFO_EFRSUP 를 확인하는 이유: 펌웨어가 그 사본이 유효하다고 알렸을
 * 때만 믿을 수 있다. 아니면 값이 0 이거나 쓰레기다.
 *
 * 호출 체인:
 *   init_iommu_one() → [이 함수]
 */
static void __init early_iommu_features_init(struct amd_iommu *iommu,
					     struct ivhd_header *h)
{
	if (amd_iommu_ivinfo & IOMMU_IVINFO_EFRSUP) {	/* [한국어] 펌웨어가 표의 EFR 사본이 유효하다고 알렸는가 */
		iommu->features = h->efr_reg;	/* [한국어] MMIO 를 매핑하기 전에도 능력을 알 수 있게 된다 */
		iommu->features2 = h->efr_reg2;	/* [한국어] 두 번째 워드도 */
	}
	if (amd_iommu_ivinfo & IOMMU_IVINFO_DMA_REMAP)	/* [한국어] 펌웨어가 DMA 재매핑이 필요하다고 알렸는가 */
		amdr_ivrs_remap_support = true;	/* [한국어] 부팅 전부터 재매핑이 켜져 있었다는 뜻이라, kdump 판단에 쓰인다 */
}

/* Access to l1 and l2 indexed register spaces */

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_read_l1 - L1 간접 레지스터를 읽는다
 *
 * @iommu: 대상 유닛.
 * @l1: L1 블록 번호(여섯 개 중 하나).
 * @address: 그 블록 안의 레지스터 주소.
 * @return: 읽은 값.
 *
 * 간접 접근이라 두 단계다: 설정 공간의 0xf8 에 "무엇을 읽을지"를 쓰고,
 * 0xfc 에서 그 값을 읽는다. 레지스터가 너무 많아 설정 공간에 직접 배치할
 * 수 없어 이런 창 방식을 쓴다.
 *
 * 왜 이 레지스터들이 필요한가: RD890 같은 칩셋은 레주메 때 BIOS 가 이
 * 값들을 복원해 주지 않는다. 그래서 서스펜드 전에 전부 읽어 두었다가
 * (stored_l1) 레주메 때 되쓴다.
 *
 * 동기화가 없는 것에 유의: 창 방식이라 두 접근 사이에 다른 스레드가
 * 끼어들면 엉뚱한 값을 읽는다. 서스펜드/레주메 경로에서만 쓰이고 그때는
 * 단일 스레드라 문제가 되지 않는다.
 *
 * 호출 체인:
 *   iommu_apply_resume_quirks() → [이 함수]
 */
static u32 iommu_read_l1(struct amd_iommu *iommu, u16 l1, u8 address)
{
	u32 val;	/* [한국어] 읽어 온 값 */

	pci_write_config_dword(iommu->dev, 0xf8, (address | l1 << 16));	/* [한국어] 창에 "무엇을 읽을지"를 지정한다 */
	pci_read_config_dword(iommu->dev, 0xfc, &val);	/* [한국어] 그 창을 통해 값을 읽는다 */
	return val;	/* [한국어] 두 단계가 원자적이지 않지만 단일 스레드 경로에서만 쓰인다 */
}

/*
 * [한국어]
 * iommu_write_l1 - L1 간접 레지스터에 쓴다
 *
 * @iommu: 대상 유닛.
 * @l1: L1 블록 번호.
 * @address: 블록 안의 주소.
 * @val: 쓸 값.
 *
 * 읽기와 같은 창 방식이지만 세 단계다. 주소에 비트 31 을 함께 세워 "쓰기"를
 * 알리고, 값을 쓰고, 마지막에 비트 31 없이 주소를 다시 써서 창을 읽기
 * 모드로 되돌린다.
 *
 * 마지막 단계가 없으면 창이 쓰기 모드로 남아, 다음 읽기가 의도치 않은
 * 쓰기가 될 수 있다.
 *
 * 호출 체인:
 *   iommu_apply_resume_quirks() → [이 함수]
 */
static void iommu_write_l1(struct amd_iommu *iommu, u16 l1, u8 address, u32 val)
{
	pci_write_config_dword(iommu->dev, 0xf8, (address | l1 << 16 | 1 << 31));	/* [한국어] 비트 31 로 쓰기 모드임을 알린다 */
	pci_write_config_dword(iommu->dev, 0xfc, val);	/* [한국어] 값을 쓴다 */
	pci_write_config_dword(iommu->dev, 0xf8, (address | l1 << 16));	/* [한국어] 창을 읽기 모드로 되돌린다. 빠뜨리면 다음 읽기가 쓰기가 될 수 있다 */
}

/*
 * [한국어]
 * iommu_read_l2 - L2 간접 레지스터를 읽는다
 *
 * @iommu: 대상 유닛.
 * @address: 레지스터 주소.
 * @return: 읽은 값.
 *
 * L1 과 같은 창 방식이고 창의 위치(0xf0/0xf4)와 블록 번호가 없다는 점만
 * 다르다. L2 는 블록으로 나뉘지 않는다.
 */
static u32 iommu_read_l2(struct amd_iommu *iommu, u8 address)
{
	u32 val;	/* [한국어] 읽어 온 값 */

	pci_write_config_dword(iommu->dev, 0xf0, address);	/* [한국어] L2 는 블록으로 나뉘지 않아 주소만 지정한다 */
	pci_read_config_dword(iommu->dev, 0xf4, &val);	/* [한국어] 창을 통해 읽는다 */
	return val;	/* [한국어] 값 */
}

/*
 * [한국어]
 * iommu_write_l2 - L2 간접 레지스터에 쓴다
 *
 * @iommu: 대상 유닛.
 * @address: 레지스터 주소.
 * @val: 쓸 값.
 *
 * 주소에 비트 8 을 세워 쓰기를 알린다. L1 과 달리 창을 되돌리는 단계가
 * 없는데, L2 의 쓰기 비트는 다음 접근에 영향을 주지 않기 때문이다.
 */
static void iommu_write_l2(struct amd_iommu *iommu, u8 address, u32 val)
{
	pci_write_config_dword(iommu->dev, 0xf0, (address | 1 << 8));	/* [한국어] 비트 8 이 L2 의 쓰기 표시다 */
	pci_write_config_dword(iommu->dev, 0xf4, val);	/* [한국어] 값을 쓴다. L1 과 달리 창을 되돌릴 필요가 없다 */
}

/****************************************************************************
 *
 * AMD IOMMU MMIO register space handling functions
 *
 * These functions are used to program the IOMMU device registers in
 * MMIO space required for that driver.
 *
 ****************************************************************************/

/*
 * This function set the exclusion range in the IOMMU. DMA accesses to the
 * exclusion range are passed through untranslated
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_set_exclusion_range - 변환을 거치지 않고 통과할 주소 구간을 정한다
 *
 * @iommu: 대상 유닛.
 *
 * 이 구간의 DMA 는 IOMMU 를 그대로 지나간다. 펌웨어가 예약한 메모리를
 * 장치가 직접 써야 하거나, 부팅 전부터 진행 중인 DMA 가 있을 때 필요하다.
 *
 * IOMMU 격리의 의도적인 구멍이라, 펌웨어가 IVMD 로 요구한 경우에만 설정한다.
 * exclusion_start 가 0 이면 요구가 없었다는 뜻이라 아무것도 하지 않는다.
 *
 * 주소를 PAGE_MASK 로 정렬하는 이유: 하드웨어가 페이지 단위로만 이 구간을
 * 다룬다. limit 를 start + length - 1 로 계산하는 것은 끝 주소가 포함
 * (inclusive)이기 때문이다.
 *
 * 호출 체인:
 *   iommu_init_flags() → [이 함수]
 */
static void iommu_set_exclusion_range(struct amd_iommu *iommu)
{
	u64 start = iommu->exclusion_start & PAGE_MASK;	/* [한국어] 하드웨어가 페이지 단위로만 다루므로 정렬한다 */
	u64 limit = (start + iommu->exclusion_length - 1) & PAGE_MASK;	/* [한국어] 끝 주소는 포함이라 -1 을 한 뒤 정렬한다 */
	u64 entry;	/* [한국어] 레지스터에 쓸 값 */

	if (!iommu->exclusion_start)	/* [한국어] 펌웨어가 제외 범위를 요구하지 않았다 */
		return;	/* [한국어] 격리에 구멍을 내지 않는다 */

	entry = start | MMIO_EXCL_ENABLE_MASK;	/* [한국어] 시작 주소에 활성화 비트를 함께 */
	memcpy_toio(iommu->mmio_base + MMIO_EXCL_BASE_OFFSET,	/* [한국어] 64비트를 통째로 쓴다 */
			&entry, sizeof(entry));	/* [한국어] 시작 주소 설정 */

	entry = limit;	/* [한국어] 끝 주소는 플래그 없이 */
	memcpy_toio(iommu->mmio_base + MMIO_EXCL_LIMIT_OFFSET,	/* [한국어] 끝 주소 레지스터에 */
			&entry, sizeof(entry));	/* [한국어] 이 구간의 DMA 는 이제 변환을 거치지 않는다 */
}

/*
 * [한국어]
 * iommu_set_cwwb_range - 완료 대기 값을 쓸 주소 범위를 알린다 (SNP 전용)
 *
 * @iommu: 대상 유닛.
 *
 * SEV-SNP 환경에서만 필요하다. SNP 는 하드웨어가 아무 물리 주소에나 쓰는
 * 것을 막는데, 완료 대기 명령이 cmd_sem 에 값을 쓰는 것도 그 검사에 걸린다.
 * 그래서 "여기에는 써도 된다"고 미리 알려야 한다.
 *
 * 원 주석이 밝히는 재사용이 이 함수의 요점이다: 제외 범위 레지스터를
 * 그 용도로 다시 쓴다. SNP 환경에서는 제외 범위가 허용되지 않으므로 그
 * 레지스터가 놀고 있고, 하드웨어가 SNP 모드에서는 그것을 완료 대기 범위로
 * 해석한다.
 *
 * base 와 limit 에 같은 값을 쓰는 이유도 원 주석에 있다 — 그것이 "4KB
 * 한 페이지"를 뜻하는 표현이다.
 *
 * 호출 체인:
 *   iommu_init_flags() → [이 함수]
 */
static void iommu_set_cwwb_range(struct amd_iommu *iommu)
{
	u64 start = iommu_virt_to_phys((void *)iommu->cmd_sem);	/* [한국어] 완료 대기 값이 쓰일 메모리의 물리 주소 */
	u64 entry = start & PM_ADDR_MASK;	/* [한국어] 플래그 비트를 떼고 순수 주소만 */

	if (!check_feature(FEATURE_SNP))	/* [한국어] SNP 가 없으면 이 제약 자체가 없다 */
		return;	/* [한국어] 평범한 시스템에서는 할 일이 없다 */

	/* Note:
	 * Re-purpose Exclusion base/limit registers for Completion wait
	 * write-back base/limit.
	 */
	memcpy_toio(iommu->mmio_base + MMIO_EXCL_BASE_OFFSET,	/* [한국어] (원 주석: 제외 범위 레지스터를 완료 대기 쓰기 범위로 재사용한다) */
		    &entry, sizeof(entry));	/* [한국어] SNP 모드에서는 하드웨어가 이 레지스터를 그렇게 해석한다 */

	/* Note:
	 * Default to 4 Kbytes, which can be specified by setting base
	 * address equal to the limit address.
	 */
	memcpy_toio(iommu->mmio_base + MMIO_EXCL_LIMIT_OFFSET,	/* [한국어] (원 주석: 시작과 끝을 같게 두면 4KB 한 페이지를 뜻한다) */
		    &entry, sizeof(entry));	/* [한국어] 이제 하드웨어가 그 페이지에 완료 값을 쓸 수 있다 */
}

/* Programs the physical address of the device table into the IOMMU hardware */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_set_device_table - 장치 테이블의 물리 주소를 하드웨어에 알린다
 *
 * @iommu: 대상 유닛.
 *
 * 변환 사슬의 출발점을 하드웨어에 알리는, 초기화에서 가장 결정적인 한
 * 걸음이다. 이 레지스터를 쓴 뒤부터 하드웨어는 이 표를 보고 장치를 판별한다.
 *
 * kdump 에서 그냥 돌아가는 것이 핵심이다. 물려받은 커널에서는 하드웨어가
 * 이미 옛 표를 가리키고 있고, 그 표의 내용을 새 표로 복사해 두었다. 여기서
 * 주소를 바꾸면 그 전환이 원자적이지 않아 그 사이의 DMA 가 어느 표를 볼지
 * 알 수 없다. 그래서 kdump 에서는 옛 표를 계속 쓰고, 내용만 우리가 관리한다.
 *
 * 호출 체인:
 *   early_enable_iommu() → [이 함수]
 */
static void iommu_set_device_table(struct amd_iommu *iommu)
{
	u64 entry;	/* [한국어] 레지스터에 쓸 주소+크기 워드 */
	u32 dev_table_size = iommu->pci_seg->dev_table_size;	/* [한국어] 표의 크기도 함께 알려야 한다 */
	void *dev_table = (void *)get_dev_table(iommu);	/* [한국어] 이 유닛이 실제로 쓰는 표 */

	BUG_ON(iommu->mmio_base == NULL);	/* [한국어] MMIO 를 매핑하기 전에 부르면 아무것도 할 수 없다 */

	if (is_kdump_kernel())	/* [한국어] 물려받은 커널에서는 하드웨어가 이미 옛 표를 가리키고 있다 */
		return;	/* [한국어] 주소를 바꾸면 전환이 원자적이지 않아 그 사이 DMA 가 어느 표를 볼지 알 수 없다 */

	entry = iommu_virt_to_phys(dev_table);	/* [한국어] SME 비트를 포함한 물리 주소 */
	entry |= (dev_table_size >> 12) - 1;
	memcpy_toio(iommu->mmio_base + MMIO_DEV_TABLE_OFFSET,
			&entry, sizeof(entry));
}

/*
 * [한국어]
 * iommu_feature_set - 제어 레지스터의 한 필드를 원하는 값으로 바꾼다
 *
 * @iommu: 대상 유닛.
 * @val: 넣을 값.
 * @mask: 그 필드의 폭(시프트 전).
 * @shift: 필드의 위치.
 *
 * 읽고-고치고-쓰는 세 단계다. 다른 비트를 건드리지 않는 것이 요점이며,
 * AMD 의 CONTROL 레지스터가 읽기 가능하기 때문에 이 방식이 성립한다 —
 * Intel 의 GCMD 는 쓰기 전용이라 소프트웨어 사본을 따로 들고 있어야 한다.
 *
 * 동기화가 없는 것에 유의: 두 스레드가 서로 다른 비트를 동시에 바꾸면
 * 하나가 사라진다. 초기화와 서스펜드/레주메 경로에서만 쓰이고 그때는
 * 단일 스레드라 문제가 되지 않는다.
 *
 * 호출 체인:
 *   iommu_feature_enable()/disable() → [이 함수]
 */
static void iommu_feature_set(struct amd_iommu *iommu, u64 val, u64 mask, u8 shift)
{
	u64 ctrl;	/* [한국어] 현재 제어 레지스터 값 */

	ctrl = readq(iommu->mmio_base +  MMIO_CONTROL_OFFSET);	/* [한국어] 다른 비트를 보존하려면 먼저 읽어야 한다 */
	mask <<= shift;	/* [한국어] 필드의 실제 위치로 마스크를 민다 */
	ctrl &= ~mask;	/* [한국어] 그 필드만 지우고 */
	ctrl |= (val << shift) & mask;	/* [한국어] 새 값을 넣는다. 마스크로 한 번 더 걸러 넘치는 비트를 막는다 */
	writeq(ctrl, iommu->mmio_base +  MMIO_CONTROL_OFFSET);	/* [한국어] 통째로 되쓴다 */
}

/* Generic functions to enable/disable certain features of the IOMMU. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_feature_enable - 제어 레지스터의 비트 하나를 켠다
 *
 * @iommu: 대상 유닛.
 * @bit: CONTROL_* 비트 번호.
 *
 * 초기화가 기능을 하나씩 활성화하는 통로다. 이 파일과 ppr.c 가 모두 이
 * 함수를 통해 하드웨어를 켠다.
 */
void iommu_feature_enable(struct amd_iommu *iommu, u8 bit)
{
	iommu_feature_set(iommu, 1ULL, 1ULL, bit);	/* [한국어] 폭 1비트 필드에 1 을 넣는 것이 곧 "켜기"다 */
}

/*
 * [한국어]
 * iommu_feature_disable - 제어 레지스터의 비트 하나를 끈다
 *
 * @iommu: 대상 유닛.
 * @bit: CONTROL_* 비트 번호.
 *
 * enable 의 짝. 끄는 순서가 중요한 경우가 있어(iommu_disable 참고) 개별
 * 비트 단위로 다룰 수 있게 해 둔다.
 */
static void iommu_feature_disable(struct amd_iommu *iommu, u8 bit)
{
	iommu_feature_set(iommu, 0ULL, 1ULL, bit);	/* [한국어] 같은 자리에 0 을 */
}

/* Function to enable the hardware */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_enable - IOMMU 하드웨어를 켠다
 *
 * @iommu: 대상 유닛.
 *
 * 비트 하나를 세우는 것이 전부지만, 그 한 줄이 시스템의 성격을 바꾼다.
 * 이 순간부터 이 유닛 아래 모든 DMA 가 장치 테이블을 거치며, 테이블에
 * 없는 장치의 요청은 차단된다.
 *
 * 그래서 이 함수가 불리기 전에 장치 테이블·명령 버퍼·이벤트 로그가 모두
 * 준비되어 있어야 하고, 펌웨어가 요구한 항등 매핑도 들어가 있어야 한다.
 *
 * 호출 체인:
 *   early_enable_iommu()/enable_iommus() → [이 함수]
 */
static void iommu_enable(struct amd_iommu *iommu)
{
	iommu_feature_enable(iommu, CONTROL_IOMMU_EN);	/* [한국어] 이 한 줄부터 모든 DMA 가 장치 테이블을 거친다 */
}

/*
 * [한국어]
 * iommu_disable - IOMMU 와 그에 딸린 기능을 모두 끈다
 *
 * @iommu: 대상 유닛.
 *
 * 끄는 순서가 이 함수의 전부다. 하드웨어 본체보다 로그와 명령 버퍼를 먼저
 * 끈다 — 반대로 하면 IOMMU 가 꺼진 뒤에도 로그 인터럽트가 올라오거나,
 * 명령 버퍼에 남은 명령을 하드웨어가 계속 처리하려 든다.
 *
 * 각 로그마다 인터럽트를 먼저 끄고 기록을 나중에 끄는 것도 같은 이유다.
 * 기록을 먼저 끄면 그 사이에 이미 쌓인 항목에 대한 인터럽트가 남는다.
 *
 * IRTCACHEDIS 를 마지막에 지우는 이유: 그 비트는 errata 우회를 위한
 * 것이고, 하드웨어를 끈 뒤에는 의미가 없다. 다음에 켤 때 깨끗한 상태에서
 * 시작하도록 되돌려 둔다.
 *
 * mmio_base 검사: 초기화 실패 경로에서 매핑도 하기 전에 불릴 수 있다.
 *
 * 호출 체인:
 *   disable_iommus()/초기화 실패 정리 → [이 함수]
 */
static void iommu_disable(struct amd_iommu *iommu)
{
	if (!iommu->mmio_base)	/* [한국어] 매핑도 하기 전에 실패 정리로 불릴 수 있다 */
		return;	/* [한국어] 건드릴 레지스터가 없다 */

	/* Disable command buffer */
	iommu_feature_disable(iommu, CONTROL_CMDBUF_EN);	/* [한국어] (원 주석) 남은 명령을 하드웨어가 계속 처리하지 않도록 먼저 끈다 */

	/* Disable event logging and event interrupts */
	iommu_feature_disable(iommu, CONTROL_EVT_INT_EN);	/* [한국어] (원 주석) 인터럽트를 먼저 — 기록을 먼저 끄면 이미 쌓인 항목의 인터럽트가 남는다 */
	iommu_feature_disable(iommu, CONTROL_EVT_LOG_EN);	/* [한국어] 그다음 기록 */

	/* Disable IOMMU GA_LOG */
	iommu_feature_disable(iommu, CONTROL_GALOG_EN);	/* [한국어] (원 주석) GA 로그 기록 */
	iommu_feature_disable(iommu, CONTROL_GAINT_EN);	/* [한국어] 그 인터럽트 */

	/* Disable IOMMU PPR logging */
	iommu_feature_disable(iommu, CONTROL_PPRLOG_EN);	/* [한국어] (원 주석) PPR 로그 기록 */
	iommu_feature_disable(iommu, CONTROL_PPRINT_EN);	/* [한국어] 그 인터럽트 */

	/* Disable IOMMU hardware itself */
	iommu_feature_disable(iommu, CONTROL_IOMMU_EN);	/* [한국어] (원 주석) 마지막에 본체를 끈다 */

	/* Clear IRTE cache disabling bit */
	iommu_feature_disable(iommu, CONTROL_IRTCACHEDIS);	/* [한국어] (원 주석) errata 우회 비트도 되돌려 다음에 깨끗한 상태에서 시작하게 한다 */
}

/*
 * mapping and unmapping functions for the IOMMU MMIO space. Each AMD IOMMU in
 * the system has one.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_map_mmio_space - 유닛의 MMIO 영역을 예약하고 매핑한다
 *
 * @address: 물리 시작 주소.
 * @end: 영역의 크기.
 * @return: 매핑된 가상 주소, 실패하면 NULL.
 *
 * request_mem_region 이 먼저인 이유: 다른 드라이버가 같은 영역을 쓰겠다고
 * 나서는 것을 막는다. 실패한다는 것은 이미 누군가 그 영역을 예약했다는
 * 뜻이고, IOMMU 의 MMIO 를 다른 장치와 공유할 일은 없으므로 펌웨어가 표에
 * 잘못된 주소를 적었다는 신호다 — 그래서 BIOS 버그라고 알린다.
 *
 * 이 함수가 성공해야 능력 비트를 읽을 수 있고, 그것 없이는 이 유닛에 대해
 * 아무 판단도 할 수 없다.
 *
 * 호출 체인:
 *   init_iommu_one() → [이 함수]
 */
static u8 __iomem * __init iommu_map_mmio_space(u64 address, u64 end)
{
	if (!request_mem_region(address, end, "amd_iommu")) {	/* [한국어] 다른 드라이버가 같은 영역을 쓰지 못하게 예약한다 */
		pr_err("Can not reserve memory region %llx-%llx for mmio\n",	/* [한국어] IOMMU 의 MMIO 를 공유할 일은 없으므로 예약 실패는 표가 잘못됐다는 뜻이다 */
			address, end);
		pr_err("This is a BIOS bug. Please contact your hardware vendor\n");	/* [한국어] 펌웨어 문제임을 분명히 알린다 */
		return NULL;	/* [한국어] 이 유닛은 쓸 수 없다 */
	}

	return (u8 __iomem *)ioremap(address, end);	/* [한국어] 매핑에 성공해야 능력 비트를 읽을 수 있다 */
}

/*
 * [한국어]
 * iommu_unmap_mmio_space - MMIO 매핑과 예약을 되돌린다
 *
 * @iommu: 대상 유닛.
 *
 * 초기화 실패 정리 경로 전용(__init)이다. 매핑이 없을 수도 있어 검사하지만,
 * 예약 해제는 조건 없이 부른다 — request 는 성공했는데 ioremap 에서
 * 실패한 경우가 있기 때문이다.
 *
 * 호출 체인:
 *   free_iommu_one() → [이 함수]
 */
static void __init iommu_unmap_mmio_space(struct amd_iommu *iommu)
{
	if (iommu->mmio_base)	/* [한국어] 매핑까지 갔는지 */
		iounmap(iommu->mmio_base);	/* [한국어] 매핑을 푼다 */
	release_mem_region(iommu->mmio_phys, iommu->mmio_phys_end);	/* [한국어] 예약은 조건 없이 해제한다 — request 만 성공한 상태일 수 있다 */
}

/*
 * [한국어]
 * get_ivhd_header_size - 이 IVHD 타입의 헤더가 몇 바이트인지
 *
 * @h: IVHD 헤더.
 * @return: 헤더 크기. 모르는 타입이면 0.
 *
 * 타입마다 헤더 크기가 다르다는 것이 IVRS 파싱의 첫 함정이다. 10h 는
 * 24바이트, 11h/40h 는 40바이트다 — 뒤의 둘에만 EFR 사본이 들어 있기
 * 때문이다.
 *
 * 이 값이 곧 "장치 항목이 어디서 시작하는가"이므로, 틀리면 항목 파싱이
 * 통째로 어긋난다. sizeof(struct ivhd_header) 를 쓸 수 없는 이유가 이것이다.
 *
 * 0 을 돌려주면 호출자가 그 IVHD 를 건너뛴다.
 *
 * 호출 체인:
 *   find_last_devid_from_ivhd()/init_iommu_from_acpi() → [이 함수]
 */
static inline u32 get_ivhd_header_size(struct ivhd_header *h)
{
	u32 size = 0;	/* [한국어] 모르는 타입이면 0 이 그대로 반환된다 */

	switch (h->type) {	/* [한국어] 타입마다 헤더 크기가 다르다 */
	case 0x10:	/* [한국어] 가장 오래된 형식 */
		size = 24;	/* [한국어] EFR 사본이 없어 짧다 */
		break;	/* [한국어] 결정 */
	case 0x11:	/* [한국어] EFR 사본이 추가된 형식 */
	case 0x40:	/* [한국어] 같은 크기의 최신 형식 */
		size = 40;	/* [한국어] efr_reg 와 efr_reg2 만큼 길다 */
		break;	/* [한국어] 결정 */
	}
	return size;
}

/****************************************************************************
 *
 * The functions below belong to the first pass of AMD IOMMU ACPI table
 * parsing. In this pass we try to find out the highest device id this
 * code has to handle. Upon this information the size of the shared data
 * structures is determined later.
 *
 ****************************************************************************/

/*
 * This function calculates the length of a given IVHD entry
 */
/*
 * [한국어]
 * ivhd_entry_length - IVHD 장치 항목 하나가 몇 바이트인지 계산한다
 *
 * @ivhd: 항목의 시작 주소.
 * @return: 그 항목의 크기. 모르는 형식이면 0.
 *
 * IVRS 파싱의 두 번째 함정이다. 항목마다 크기가 다르고, 그 크기가 타입
 * 값의 상위 두 비트에 인코딩되어 있다 — 0x04 << (type >> 6) 이 그 해독이다.
 * 그래서 4, 8, 16, 32바이트 네 가지가 나온다.
 *
 * ACPI HID 항목만 예외다. UID 문자열이 가변 길이라 크기를 인코딩할 수 없어,
 * 원 주석대로 오프셋 21 에 그 길이가 따로 적혀 있다. 고정 부분 22바이트에
 * 그것을 더한 것이 전체 크기다.
 *
 * 0 을 돌려주면 호출자의 순회가 제자리에 머물러 무한 루프가 된다 — 실제로는
 * 알려진 타입만 표에 나타나므로 그런 일은 없지만, 방어적으로 0 을 둔다.
 *
 * 호출 체인:
 *   find_last_devid_from_ivhd()/init_iommu_from_acpi() → [이 함수]
 */
static inline int ivhd_entry_length(u8 *ivhd)
{
	u32 type = ((struct ivhd_entry *)ivhd)->type;	/* [한국어] 타입이 곧 크기의 인코딩이다 */

	if (type < 0x80) {	/* [한국어] 일반 항목은 상위 두 비트에 크기가 들어 있다 */
		return 0x04 << (*ivhd >> 6);	/* [한국어] 4, 8, 16, 32 네 가지가 나온다 */
	} else if (type == IVHD_DEV_ACPI_HID) {	/* [한국어] HID 항목만 가변 길이다 */
		/* For ACPI_HID, offset 21 is uid len */
		return *((u8 *)ivhd + 21) + 22;	/* [한국어] (원 주석: 오프셋 21 이 uid 길이) 고정 22바이트 + UID 길이 */
	}
	return 0;	/* [한국어] 알려지지 않은 형식. 실제 표에는 나타나지 않는다 */
}

/*
 * After reading the highest device id from the IOMMU PCI capability header
 * this function looks if there is a higher device id defined in the ACPI table
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * find_last_devid_from_ivhd - 이 IVHD 가 언급하는 가장 큰 장치 id 를 찾는다
 *
 * @h: IVHD 헤더.
 * @return: 가장 큰 장치 id. 지원하지 않는 타입이면 -EINVAL.
 *
 * 왜 이 값이 필요한가: 세그먼트의 조회 표들(장치 테이블, rlookup, 별칭
 * 표)이 모두 장치 id 를 인덱스로 쓰는 평평한 배열이다. 16비트 전체를
 * 잡으면 세그먼트마다 수 MB 를 낭비하므로, 실제로 나타나는 최대값까지만
 * 잡는다.
 *
 * IVHD_DEV_ALL 을 만나면 곧바로 0xffff 를 돌려주는 이유: 그 항목은 "모든
 * 장치"를 뜻하므로 어떤 id 든 나타날 수 있고, 표를 최대 크기로 잡아야 한다.
 *
 * 나머지 타입 중 devid 필드가 의미 있는 것들만 골라 최대값을 갱신한다.
 * 원 주석이 그 넷을 묶어 설명한다.
 *
 * WARN_ON(p != end): 항목 크기 계산이 틀리면 순회가 구간의 끝을 정확히
 * 짚지 못한다. 그 어긋남을 여기서 드러낸다 — 조용히 넘어가면 표를 잘못
 * 읽은 채로 부팅이 진행된다.
 *
 * 호출 체인:
 *   find_last_devid_acpi() → [이 함수] → ivhd_entry_length()
 */
static int __init find_last_devid_from_ivhd(struct ivhd_header *h)
{
	u8 *p = (void *)h, *end = (void *)h;	/* [한국어] 항목 구간을 훑을 커서와 끝 */
	struct ivhd_entry *dev;	/* [한국어] 현재 항목 */
	int last_devid = -EINVAL;	/* [한국어] 아직 아무 장치도 못 봤다는 표시 */

	u32 ivhd_size = get_ivhd_header_size(h);	/* [한국어] 타입마다 헤더 크기가 달라 먼저 구한다 */

	if (!ivhd_size) {	/* [한국어] 드라이버가 모르는 타입 */
		pr_err("Unsupported IVHD type %#x\n", h->type);	/* [한국어] 해석할 수 없다 */
		return -EINVAL;	/* [한국어] 이 IVHD 를 건너뛰게 한다 */
	}

	p += ivhd_size;	/* [한국어] 헤더를 지나 첫 장치 항목으로 */
	end += h->length;	/* [한국어] 항목 구간의 끝. 표가 스스로 알려 준다 */

	while (p < end) {	/* [한국어] 항목을 하나씩 */
		dev = (struct ivhd_entry *)p;	/* [한국어] 현재 위치를 항목으로 해석 */
		switch (dev->type) {	/* [한국어] 타입에 따라 devid 필드의 의미가 다르다 */
		case IVHD_DEV_ALL:	/* [한국어] (원 주석: DEV_ALL 은 최대 BDF 를 쓴다) */
			/* Use maximum BDF value for DEV_ALL */
			return 0xffff;	/* [한국어] 모든 장치가 대상이라 표를 최대로 잡아야 한다 */
		case IVHD_DEV_SELECT:	/* [한국어] (원 주석: 아래 네 종류는 모두 장치 id 를 가리킨다) */
		case IVHD_DEV_RANGE_END:	/* [한국어] 범위의 끝 */
		case IVHD_DEV_ALIAS:	/* [한국어] 별칭이 딸린 장치 */
		case IVHD_DEV_EXT_SELECT:	/* [한국어] 확장 플래그가 딸린 장치 */
			/* all the above subfield types refer to device ids */
			if (dev->devid > last_devid)	/* [한국어] 더 큰 id 를 만났으면 */
				last_devid = dev->devid;	/* [한국어] 갱신한다 */
			break;	/* [한국어] 다음 항목으로 */
		default:	/* [한국어] 나머지는 devid 필드가 장치를 뜻하지 않는다 */
			break;	/* [한국어] 건너뛴다 */
		}
		p += ivhd_entry_length(p);	/* [한국어] 항목마다 크기가 달라 계산해서 건너뛴다 */
	}

	WARN_ON(p != end);	/* [한국어] 크기 계산이 틀렸다면 끝을 정확히 짚지 못한다 — 조용히 넘기지 않는다 */

	return last_devid;	/* [한국어] 이 값이 세그먼트 표들의 크기를 정한다 */
}

/*
 * [한국어]
 * check_ivrs_checksum - IVRS 표가 손상되지 않았는지 확인한다
 *
 * @table: ACPI 표 헤더.
 * @return: 0 이면 정상, -ENODEV 면 체크섬 불일치.
 *
 * ACPI 표의 모든 바이트를 더해 0 이 나와야 한다는 규약이다. 표 안에 이미
 * 그렇게 되도록 맞춘 체크섬 바이트가 들어 있다.
 *
 * 왜 확인하는가: 이 표를 잘못 읽으면 존재하지 않는 주소를 MMIO 로 매핑하거나
 * 엉뚱한 크기의 표를 잡는다. 손상된 표로 진행하느니 IOMMU 를 포기하는 편이
 * 안전하다.
 *
 * FW_BUG 로 보고하는 이유: 커널이 고칠 수 없는 펌웨어 문제다.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → [이 함수]
 */
static int __init check_ivrs_checksum(struct acpi_table_header *table)
{
	int i;	/* [한국어] 순회 인덱스 */
	u8 checksum = 0, *p = (u8 *)table;	/* [한국어] 바이트 합계와 표의 시작 */

	for (i = 0; i < table->length; ++i)	/* [한국어] 표 전체를 바이트 단위로 */
		checksum += p[i];	/* [한국어] 더한다. 8비트 랩어라운드가 곧 검사 방식이다 */
	if (checksum != 0) {	/* [한국어] 0 이 아니면 표가 손상됐다 */
		/* ACPI table corrupt */
		pr_err(FW_BUG "IVRS invalid checksum\n");	/* [한국어] (원 주석: ACPI 표 손상) 커널이 고칠 수 없는 펌웨어 문제다 */
		return -ENODEV;	/* [한국어] 손상된 표로 진행하느니 IOMMU 를 포기한다 */
	}

	return 0;	/* [한국어] 정상 */
}

/*
 * Iterate over all IVHD entries in the ACPI table and find the highest device
 * id which we need to handle. This is the first of three functions which parse
 * the ACPI table. So we check the checksum here.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * find_last_devid_acpi - 한 세그먼트의 모든 IVHD 를 훑어 최대 장치 id 를 구한다
 *
 * @table: IVRS 표.
 * @pci_seg: 대상 세그먼트.
 * @return: 그 세그먼트의 최대 장치 id. 파싱 실패면 -EINVAL.
 *
 * 원 주석이 밝히듯 표를 세 번 훑는 것 중 첫 번째다. 이 단계에서는 아무것도
 * 만들지 않고 크기만 잰다 — 자료구조를 잡으려면 얼마나 커야 하는지 먼저
 * 알아야 하기 때문이다.
 *
 * 한 세그먼트에 IVHD 가 여럿일 수 있다(유닛이 여럿이면). 그 모두를 훑어
 * 최대값을 구해야 그 세그먼트의 표 크기가 정해진다.
 *
 * target_ivhd_type 만 보는 이유: 표에는 같은 유닛에 대해 여러 타입의
 * IVHD 가 함께 들어 있을 수 있다(하위 호환을 위해). 그중 하나만 골라 써야
 * 하고, 그 선택이 이미 끝나 있어야 이 함수가 일관된 결과를 낸다.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → [이 함수] → find_last_devid_from_ivhd()
 */
static int __init find_last_devid_acpi(struct acpi_table_header *table, u16 pci_seg)
{
	u8 *p = (u8 *)table, *end = (u8 *)table;	/* [한국어] 표를 훑을 커서와 끝 */
	struct ivhd_header *h;	/* [한국어] 현재 IVHD */
	int last_devid, last_bdf = 0;	/* [한국어] 이번 IVHD 의 최대값과 세그먼트 전체의 최대값 */

	p += IVRS_HEADER_LENGTH;	/* [한국어] 고정 헤더를 지나 첫 IVHD 로 */

	end += table->length;	/* [한국어] 표 전체의 끝 */
	while (p < end) {	/* [한국어] IVHD 를 하나씩 */
		h = (struct ivhd_header *)p;	/* [한국어] 현재 위치를 IVHD 로 해석 */
		if (h->pci_seg == pci_seg &&	/* [한국어] 우리가 찾는 세그먼트이고 */
		    h->type == amd_iommu_target_ivhd_type) {	/* [한국어] 고른 타입인가 — 같은 유닛에 여러 타입이 있을 수 있다 */
			last_devid = find_last_devid_from_ivhd(h);	/* [한국어] 그 IVHD 의 최대 장치 id */

			if (last_devid < 0)	/* [한국어] 파싱 실패 */
				return -EINVAL;	/* [한국어] 표가 이상하면 진행하지 않는다 */
			if (last_devid > last_bdf)	/* [한국어] 세그먼트 전체의 최대값을 */
				last_bdf = last_devid;	/* [한국어] 갱신한다 — 한 세그먼트에 유닛이 여럿일 수 있다 */
		}
		p += h->length;	/* [한국어] 다음 IVHD 로. 길이는 헤더가 알려 준다 */
	}
	WARN_ON(p != end);	/* [한국어] 순회가 끝을 정확히 짚지 못하면 표를 잘못 읽은 것이다 */

	return last_bdf;	/* [한국어] 이 값으로 세그먼트의 표 크기가 정해진다 */
}

/****************************************************************************
 *
 * The following functions belong to the code path which parses the ACPI table
 * the second time. In this ACPI parsing iteration we allocate IOMMU specific
 * data structures, initialize the per PCI segment device/alias/rlookup table
 * and also basically initialize the hardware.
 *
 ****************************************************************************/

/* Allocate per PCI segment device table */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * alloc_dev_table - 세그먼트의 장치 테이블을 잡는다
 *
 * @pci_seg: 대상 세그먼트.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * 하드웨어가 직접 읽는 표라 물리적으로 연속이어야 한다. GFP_DMA32 를 쓰는
 * 이유: 일부 하드웨어가 이 표의 주소를 32비트로만 다룬다.
 *
 * 크기는 앞 단계에서 구한 last_bdf 로 이미 계산되어 있다.
 *
 * 호출 체인:
 *   alloc_pci_segment() → [이 함수]
 */
static inline int __init alloc_dev_table(struct amd_iommu_pci_seg *pci_seg)
{
	pci_seg->dev_table = iommu_alloc_pages_sz(GFP_KERNEL | GFP_DMA32,	/* [한국어] 하드웨어가 직접 읽으므로 물리적으로 연속이어야 한다 */
						  pci_seg->dev_table_size);	/* [한국어] 앞 단계에서 last_bdf 로 계산해 둔 크기 */
	if (!pci_seg->dev_table)	/* [한국어] 연속 메모리 부족 */
		return -ENOMEM;	/* [한국어] 이 세그먼트는 쓸 수 없다 */

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * free_dev_table - 장치 테이블을 놓는다
 *
 * @pci_seg: 대상 세그먼트.
 *
 * kdump 인지에 따라 해제 방법이 다르다는 것이 이 함수의 요점이다. 물려받은
 * 커널에서는 표를 새로 잡지 않고 옛 커널이 남긴 물리 메모리를 매핑해 쓰므로,
 * 해제도 페이지 반납이 아니라 매핑 해제여야 한다.
 *
 * NULL 로 되돌려 두 번 해제하는 일을 막는다.
 *
 * 호출 체인:
 *   free_pci_segments() → [이 함수]
 */
static inline void free_dev_table(struct amd_iommu_pci_seg *pci_seg)
{
	if (is_kdump_kernel())	/* [한국어] 물려받은 표는 새로 잡은 것이 아니라 옛 메모리를 매핑한 것이다 */
		memunmap((void *)pci_seg->dev_table);	/* [한국어] 그래서 페이지 반납이 아니라 매핑 해제 */
	else
		iommu_free_pages(pci_seg->dev_table);	/* [한국어] 평범한 경우에는 페이지를 놓는다 */
	pci_seg->dev_table = NULL;	/* [한국어] 두 번 해제를 막는다 */
}

/* Allocate per PCI segment IOMMU rlookup table. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * alloc_rlookup_table - 장치 id → 담당 유닛 조회 표를 잡는다
 *
 * @pci_seg: 대상 세그먼트.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * 하드웨어가 읽지 않는 순수한 소프트웨어 표라 물리적 연속성이 필요 없다.
 * 그래서 kvzalloc 을 쓴다 — 크면 vmalloc 으로 떨어져 연속 페이지 부족으로
 * 실패하지 않는다.
 *
 * 0 으로 초기화하는 것이 중요하다. NULL 이 "담당 유닛 없음"을 뜻하므로,
 * 표에 나타나지 않은 장치는 자연스럽게 미할당 상태가 된다.
 *
 * 호출 체인:
 *   alloc_pci_segment() → [이 함수]
 */
static inline int __init alloc_rlookup_table(struct amd_iommu_pci_seg *pci_seg)
{
	pci_seg->rlookup_table = kvzalloc_objs(*pci_seg->rlookup_table,	/* [한국어] 하드웨어가 읽지 않아 물리적 연속성이 필요 없다 — 크면 vmalloc 으로 떨어진다 */
					       pci_seg->last_bdf + 1);	/* [한국어] 실제로 나타나는 최대 id 까지만 */
	if (pci_seg->rlookup_table == NULL)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 실패 보고 */

	return 0;	/* [한국어] 0 초기화 덕에 표에 없는 장치는 자연히 "담당 유닛 없음"이 된다 */
}

/*
 * [한국어]
 * free_rlookup_table - 그 조회 표를 놓는다
 *
 * @pci_seg: 대상 세그먼트.
 *
 * kvfree 는 kmalloc/vmalloc 어느 쪽이든 알아서 처리한다. NULL 로 되돌려
 * 두 번 해제를 막는다.
 */
static inline void free_rlookup_table(struct amd_iommu_pci_seg *pci_seg)
{
	kvfree(pci_seg->rlookup_table);	/* [한국어] kmalloc/vmalloc 어느 쪽이든 처리한다 */
	pci_seg->rlookup_table = NULL;	/* [한국어] 두 번 해제 방지 */
}

/*
 * [한국어]
 * alloc_irq_lookup_table - 장치 id → 인터럽트 재매핑 표 조회를 잡는다
 *
 * @pci_seg: 대상 세그먼트.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * rlookup 과 같은 구조이고 담는 것만 다르다. AMD 는 재매핑 표가 장치마다
 * 있어, 인터럽트를 다룰 때마다 "이 장치의 표가 어디인가"를 물어야 한다.
 *
 * 별칭을 공유하는 장치들은 같은 표를 가리키므로, 이 배열의 여러 칸이
 * 하나의 표를 가리킬 수 있다.
 *
 * 재매핑을 쓰지 않는 시스템에서는 아예 잡지 않는다 — 그래서 alloc_pci_segment
 * 가 아니라 재매핑 초기화 경로에서 불린다.
 */
static inline int __init alloc_irq_lookup_table(struct amd_iommu_pci_seg *pci_seg)
{
	pci_seg->irq_lookup_table = kvzalloc_objs(*pci_seg->irq_lookup_table,	/* [한국어] rlookup 과 같은 구조. 담는 것만 다르다 */
						  pci_seg->last_bdf + 1);	/* [한국어] 같은 크기 */
	if (pci_seg->irq_lookup_table == NULL)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 재매핑을 쓸 수 없다 */

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * free_irq_lookup_table - 그 조회 표를 놓는다
 *
 * @pci_seg: 대상 세그먼트.
 *
 * 표 자체(irq_remap_table)는 여기서 놓지 않는다 — 여러 칸이 하나를 가리킬
 * 수 있어 이 배열만 보고는 해제 시점을 알 수 없다. 표는 인터럽트 해제
 * 경로가 관리한다.
 */
static inline void free_irq_lookup_table(struct amd_iommu_pci_seg *pci_seg)
{
	kvfree(pci_seg->irq_lookup_table);	/* [한국어] 배열만 놓는다 — 표 자체는 여러 칸이 공유할 수 있어 여기서 판단할 수 없다 */
	pci_seg->irq_lookup_table = NULL;	/* [한국어] 두 번 해제 방지 */
}

/*
 * [한국어]
 * alloc_alias_table - 장치 id → 실제 요청자 id 표를 잡고 항등으로 채운다
 *
 * @pci_seg: 대상 세그먼트.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * 원 주석이 밝히듯 모든 항목이 자기 자신을 가리키게 시작한다. 그것이 기본
 * 상태이기 때문이다 — 대부분의 장치는 자기 이름으로 요청을 낸다.
 *
 * 예외만 IVHD 파싱이 나중에 덮어쓴다. PCIe-to-PCI 브리지 뒤의 장치들이
 * 브리지 이름으로 요청을 내는 경우가 그것이며, 하드웨어가 그 이름으로만
 * DTE 를 찾으므로 드라이버도 같은 이름으로 설정해야 한다.
 *
 * kvzalloc 이 아니라 kvmalloc 을 쓰는 이유: 어차피 전부 덮어쓰므로 0
 * 초기화가 낭비다.
 *
 * 호출 체인:
 *   alloc_pci_segment() → [이 함수]
 */
static int __init alloc_alias_table(struct amd_iommu_pci_seg *pci_seg)
{
	int i;	/* [한국어] 초기화 루프 인덱스 */

	pci_seg->alias_table = kvmalloc_objs(*pci_seg->alias_table,	/* [한국어] 전부 덮어쓸 것이라 0 초기화가 낭비다 */
					     pci_seg->last_bdf + 1);	/* [한국어] 같은 크기 */
	if (!pci_seg->alias_table)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 실패 보고 */

	/*
	 * let all alias entries point to itself
	 */
	for (i = 0; i <= pci_seg->last_bdf; ++i)	/* [한국어] (원 주석: 모든 별칭 항목이 자기 자신을 가리키게 한다) */
		pci_seg->alias_table[i] = i;	/* [한국어] 대부분의 장치는 자기 이름으로 요청을 내므로 그것이 기본 상태다 */

	return 0;	/* [한국어] 예외만 IVHD 파싱이 나중에 덮어쓴다 */
}

/*
 * [한국어]
 * free_alias_table - 별칭 표를 놓는다
 *
 * @pci_seg: 대상 세그먼트.
 */
static void __init free_alias_table(struct amd_iommu_pci_seg *pci_seg)
{
	kvfree(pci_seg->alias_table);	/* [한국어] 배열 해제 */
	pci_seg->alias_table = NULL;	/* [한국어] 두 번 해제 방지 */
}

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * iommu_memremap - kdump 에서 옛 커널이 남긴 물리 메모리를 매핑한다
 *
 * @paddr: 옛 커널이 하드웨어에 알렸던 물리 주소.
 * @size: 매핑할 크기.
 * @return: 커널 가상 주소, 실패하거나 주소가 0 이면 NULL.
 *
 * 물려받기 경로 전용이다. 옛 커널의 장치 테이블·명령 버퍼·완료 대기 메모리를
 * 이 함수로 매핑해 그대로 이어 쓴다.
 *
 * SME 가 이 함수를 필요하게 만든다. 하드웨어에 알린 주소에는 암호화 비트가
 * 붙어 있으므로 __sme_clr 로 그것을 떼어야 진짜 물리 주소가 나온다. 그리고
 * 그 메모리는 암호화된 상태이므로 ioremap_encrypted 로 매핑해야 내용을
 * 제대로 읽는다 — 평범한 memremap 으로는 암호문을 보게 된다.
 *
 * 원 주석이 밝히는 제약: 옛 커널은 SME 를 켜고 kdump 커널은 껐다면 이
 * 변환이 성립하지 않으며, 그 조합은 지원하지 않는다.
 *
 * 호출 체인:
 *   remap_command_buffer()/remap_event_buffer()/copy_device_table() → [이 함수]
 */
static inline void *iommu_memremap(unsigned long paddr, size_t size)
{
	phys_addr_t phys;	/* [한국어] 암호화 비트를 뗀 진짜 물리 주소 */

	if (!paddr)	/* [한국어] 옛 커널이 그 버퍼를 쓰지 않았다 */
		return NULL;	/* [한국어] 물려받을 것이 없다 */

	/*
	 * Obtain true physical address in kdump kernel when SME is enabled.
	 * Currently, previous kernel with SME enabled and kdump kernel
	 * with SME support disabled is not supported.
	 */
	phys = __sme_clr(paddr);	/* [한국어] (원 주석: SME 가 켜진 kdump 에서 진짜 물리 주소를 얻는다) */

	if (cc_platform_has(CC_ATTR_HOST_MEM_ENCRYPT))	/* [한국어] 메모리 암호화가 켜진 시스템인가 */
		return (__force void *)ioremap_encrypted(phys, size);	/* [한국어] 암호화된 메모리로 매핑해야 내용을 제대로 읽는다 */
	else
		return memremap(phys, size, MEMREMAP_WB);	/* [한국어] 아니면 평범한 매핑 */
}

/*
 * Allocates the command buffer. This buffer is per AMD IOMMU. We can
 * write commands to that buffer later and the IOMMU will execute them
 * asynchronously
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * alloc_command_buffer - 명령 버퍼를 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * 드라이버 → 하드웨어 방향의 링이다. 무효화와 완료 대기가 여기 실린다.
 * 하드웨어가 직접 읽으므로 물리적으로 연속이어야 하고, 크기는 512개
 * 항목(8KB)으로 고정이다.
 *
 * 이 버퍼가 없으면 캐시를 무효화할 수단이 없어 IOMMU 를 켤 수 없다.
 */
static int __init alloc_command_buffer(struct amd_iommu *iommu)
{
	iommu->cmd_buf = iommu_alloc_pages_sz(GFP_KERNEL, CMD_BUFFER_SIZE);	/* [한국어] 하드웨어가 읽으므로 물리적으로 연속인 8KB */

	return iommu->cmd_buf ? 0 : -ENOMEM;	/* [한국어] 이것이 없으면 캐시를 무효화할 수단이 없어 IOMMU 를 켤 수 없다 */
}

/*
 * Interrupt handler has processed all pending events and adjusted head
 * and tail pointer. Reset overflow mask and restart logging again.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_restart_log - 넘친 로그를 되살린다 (세 로그 공통 절차)
 *
 * @iommu: 대상 유닛.
 * @evt_type: 로그 이름(로그 메시지용).
 * @cntrl_intr: 그 로그의 인터럽트 제어 비트.
 * @cntrl_log: 그 로그의 기록 제어 비트.
 * @status_run_mask: "동작 중"을 나타내는 상태 비트.
 * @status_overflow_mask: 오버플로 표시 비트.
 *
 * 이벤트·PPR·GA 세 로그가 같은 절차를 쓰므로, 어떤 비트를 다룰지만 인자로
 * 받아 하나로 합쳤다.
 *
 * 절차의 순서가 전부다:
 *  1) 여전히 동작 중이면 아무것도 하지 않는다. 오버플로가 이미 해소됐거나
 *     다른 CPU 가 먼저 되살렸다는 뜻이다.
 *  2) 기록과 인터럽트를 끈다. 켜진 채로 상태 비트를 지우면 그 사이의
 *     새 항목이 오버플로를 다시 세워 지우기가 무의미해진다.
 *  3) 오버플로 표시를 지운다. 상태 레지스터는 "1을 써서 지우는" 방식이다.
 *  4) 다시 켠다. 인터럽트를 먼저 켜는 것은 기록을 켠 직후의 항목을 놓치지
 *     않기 위해서다.
 *
 * 되살린다고 잃어버린 항목이 돌아오지는 않는다. PPR 의 경우 그 사이의
 * 페이지 요청이 응답받지 못해 장치가 멈출 수 있다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 원 주석대로 핸들러가 이미 머리/꼬리를
 * 정리한 뒤에 불린다.
 *
 * 호출 체인:
 *   amd_iommu_restart_event_logging()/ga_log()/ppr_log() → [이 함수]
 */
void amd_iommu_restart_log(struct amd_iommu *iommu, const char *evt_type,
			   u8 cntrl_intr, u8 cntrl_log,
			   u32 status_run_mask, u32 status_overflow_mask)
{
	u32 status;	/* [한국어] 상태 레지스터 값 */

	status = readl(iommu->mmio_base + MMIO_STATUS_OFFSET);	/* [한국어] 지금 상태를 본다 */
	if (status & status_run_mask)	/* [한국어] 아직 동작 중이면 */
		return;	/* [한국어] 오버플로가 해소됐거나 다른 CPU 가 먼저 되살렸다 */

	pr_info_ratelimited("IOMMU %s log restarting\n", evt_type);	/* [한국어] 반복될 수 있어 속도를 제한한다 */

	iommu_feature_disable(iommu, cntrl_log);	/* [한국어] 기록을 먼저 끈다 */
	iommu_feature_disable(iommu, cntrl_intr);	/* [한국어] 인터럽트도 — 켜진 채로 상태를 지우면 새 항목이 다시 오버플로를 세운다 */

	writel(status_overflow_mask, iommu->mmio_base + MMIO_STATUS_OFFSET);	/* [한국어] 1 을 써서 지우는 방식이라 읽은 비트를 그대로 되쓴다 */

	iommu_feature_enable(iommu, cntrl_intr);	/* [한국어] 인터럽트를 먼저 켜야 기록 직후의 항목을 놓치지 않는다 */
	iommu_feature_enable(iommu, cntrl_log);	/* [한국어] 기록 재개. 잃어버린 항목은 돌아오지 않는다 */
}

/*
 * This function restarts event logging in case the IOMMU experienced
 * an event log buffer overflow.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_restart_event_logging - 넘친 이벤트 로그를 되살린다
 *
 * @iommu: 대상 유닛.
 *
 * 이벤트 로그가 넘쳤다는 것은 그 사이의 오류 보고를 잃었다는 뜻이다.
 * PPR 과 달리 잃어도 장치가 멈추지는 않지만, 문제의 원인을 알려 줄 기록이
 * 사라진다.
 *
 * 공통 절차에 이벤트 로그의 비트만 넘긴다.
 *
 * 호출 체인:
 *   amd_iommu_int_thread_evtlog() → [이 함수] → amd_iommu_restart_log()
 */
void amd_iommu_restart_event_logging(struct amd_iommu *iommu)
{
	amd_iommu_restart_log(iommu, "Event", CONTROL_EVT_INT_EN,	/* [한국어] 세 로그가 같은 절차를 쓴다 */
			      CONTROL_EVT_LOG_EN, MMIO_STATUS_EVT_RUN_MASK,	/* [한국어] 다룰 제어 비트와 확인할 상태 비트만 다르다 */
			      MMIO_STATUS_EVT_OVERFLOW_MASK);	/* [한국어] 지울 오버플로 비트 */
}

/*
 * This function restarts event logging in case the IOMMU experienced
 * GA log overflow.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_restart_ga_log - 넘친 GA 로그를 되살린다
 *
 * @iommu: 대상 유닛.
 *
 * GA 로그가 넘치면 게스트에 전달하지 못한 인터럽트의 기록이 사라지고,
 * KVM 이 그것을 대신 주입할 수 없게 된다 — 게스트가 인터럽트를 놓친다.
 *
 * 호출 체인:
 *   amd_iommu_int_thread_galog() → [이 함수] → amd_iommu_restart_log()
 */
void amd_iommu_restart_ga_log(struct amd_iommu *iommu)
{
	amd_iommu_restart_log(iommu, "GA", CONTROL_GAINT_EN,	/* [한국어] GA 로그용 비트들 */
			      CONTROL_GALOG_EN, MMIO_STATUS_GALOG_RUN_MASK,	/* [한국어] 같은 절차 */
			      MMIO_STATUS_GALOG_OVERFLOW_MASK);	/* [한국어] 넘친 동안의 게스트 인터럽트는 KVM 이 주입하지 못한다 */
}

/*
 * This function resets the command buffer if the IOMMU stopped fetching
 * commands from it.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_reset_cmd_buffer - 명령 버퍼를 빈 상태로 되돌린다
 *
 * @iommu: 대상 유닛.
 *
 * 하드웨어가 명령을 가져가지 않고 멈췄을 때, 그리고 버퍼를 처음 걸 때
 * 불린다.
 *
 * 버퍼를 끈 채로 포인터를 되돌리는 것이 중요하다. 켜진 상태에서 머리/꼬리를
 * 만지면 하드웨어가 그 순간 어느 명령을 처리 중인지 알 수 없어 명령이
 * 유실되거나 중복 실행된다.
 *
 * 하드웨어 레지스터와 드라이버의 사본을 함께 0 으로 맞춘다. 사본을 두는
 * 이유는 명령을 넣을 때마다 MMIO 를 읽지 않기 위해서이고, 그래서 여기서
 * 반드시 함께 되돌려야 어긋나지 않는다.
 *
 * 버려지는 명령이 있을 수 있다는 것을 감수한다 — 멈춘 하드웨어를 되살리는
 * 것이 우선이고, 잃은 무효화는 상위 경로가 다시 보낸다.
 *
 * 호출 체인:
 *   iommu_enable_command_buffer()/명령 타임아웃 복구 → [이 함수]
 */
static void amd_iommu_reset_cmd_buffer(struct amd_iommu *iommu)
{
	iommu_feature_disable(iommu, CONTROL_CMDBUF_EN);	/* [한국어] 켜진 채로 포인터를 만지면 명령이 유실되거나 중복 실행된다 */

	writel(0x00, iommu->mmio_base + MMIO_CMD_HEAD_OFFSET);	/* [한국어] 머리를 0 으로 */
	writel(0x00, iommu->mmio_base + MMIO_CMD_TAIL_OFFSET);	/* [한국어] 꼬리도 0 으로 */
	iommu->cmd_buf_head = 0;	/* [한국어] 드라이버의 사본도 함께 — 어긋나면 명령을 엉뚱한 자리에 넣는다 */
	iommu->cmd_buf_tail = 0;	/* [한국어] 같은 이유 */

	iommu_feature_enable(iommu, CONTROL_CMDBUF_EN);	/* [한국어] 빈 상태에서 다시 시작한다. 남아 있던 명령은 버려진다 */
}

/*
 * This function writes the command buffer address to the hardware and
 * enables it.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_enable_command_buffer - 명령 버퍼의 주소를 알리고 켠다
 *
 * @iommu: 대상 유닛.
 *
 * kdump 분기가 이 함수의 요점이다. 원 주석이 밝히듯 물려받은 커널에서는
 * 옛 커널의 버퍼를 그대로 쓰므로(remap_command_buffer 가 매핑해 두었다)
 * 주소를 다시 알릴 필요가 없다. 알리면 그 순간 하드웨어가 처리 중이던
 * 명령이 유실된다.
 *
 * 주소와 크기를 한 워드에 담아 쓰고, 마지막에 reset 으로 포인터를 맞추면서
 * 버퍼를 켠다.
 *
 * 호출 체인:
 *   early_enable_iommu()/enable_iommus() → [이 함수]
 *     → amd_iommu_reset_cmd_buffer()
 */
static void iommu_enable_command_buffer(struct amd_iommu *iommu)
{
	u64 entry;	/* [한국어] 주소+크기를 담을 워드 */

	BUG_ON(iommu->cmd_buf == NULL);	/* [한국어] 버퍼 없이 켜면 하드웨어가 0 번지에서 명령을 읽는다 */

	if (!is_kdump_kernel()) {	/* [한국어] (원 주석: kdump 에서는 버퍼를 재사용하므로 레지스터 설정이 필요 없다) */
		/*
		 * Command buffer is re-used for kdump kernel and setting
		 * of MMIO register is not required.
		 */
		entry = iommu_virt_to_phys(iommu->cmd_buf);	/* [한국어] SME 비트를 포함한 물리 주소 */
		entry |= MMIO_CMD_SIZE_512;	/* [한국어] 512개 항목이라는 크기 인코딩 */
		memcpy_toio(iommu->mmio_base + MMIO_CMD_BUF_OFFSET,	/* [한국어] 한 워드로 통째로 쓴다 */
			    &entry, sizeof(entry));	/* [한국어] 주소를 바꾸면 처리 중이던 명령이 유실되므로 kdump 에서는 건너뛴다 */
	}

	amd_iommu_reset_cmd_buffer(iommu);	/* [한국어] 포인터를 맞추면서 버퍼를 켠다 */
}

/*
 * This function disables the command buffer
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_disable_command_buffer - 명령 버퍼를 끈다
 *
 * @iommu: 대상 유닛.
 *
 * 끈 뒤에는 명령을 넣어도 하드웨어가 가져가지 않는다. 그래서 이것을 끄기
 * 전에 남은 무효화가 모두 완료되어 있어야 한다.
 */
static void iommu_disable_command_buffer(struct amd_iommu *iommu)
{
	iommu_feature_disable(iommu, CONTROL_CMDBUF_EN);	/* [한국어] 끈 뒤에는 명령을 넣어도 하드웨어가 가져가지 않는다 */
}

/*
 * [한국어]
 * free_command_buffer - 명령 버퍼를 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * 초기화 실패 정리 전용(__init)이다. 동작 중에 놓으면 하드웨어가 해제된
 * 메모리에서 명령을 읽는다.
 */
static void __init free_command_buffer(struct amd_iommu *iommu)
{
	iommu_free_pages(iommu->cmd_buf);	/* [한국어] 초기화 실패 정리 전용. 동작 중에 놓으면 하드웨어가 해제된 메모리를 읽는다 */
}

/*
 * [한국어]
 * iommu_alloc_4k_pages - 하드웨어가 읽을 버퍼를 4KB 매핑으로 잡는다
 *
 * @iommu: 대상 유닛(NUMA 노드를 얻는 데 쓴다).
 * @gfp: 할당 플래그.
 * @size: 크기.
 * @return: 커널 가상 주소, 실패하면 NULL.
 *
 * 이름의 "4k"가 이 함수의 존재 이유다. SNP 환경에서는 하드웨어가 접근하는
 * 페이지가 반드시 4KB 단위로 매핑되어 있어야 한다 — 커널의 직접 매핑은
 * 보통 2MB 큰 페이지를 쓰므로, set_memory_4k 로 그 구간만 쪼개야 한다.
 *
 * 쪼개기에 실패하면 버퍼를 놓고 NULL 을 돌려준다. 큰 페이지로 남은 채
 * 하드웨어에 넘기면 SNP 의 무결성 검사에 걸려 접근이 실패한다.
 *
 * 유닛과 같은 NUMA 노드에서 잡는 이유: 하드웨어가 이 메모리를 자주 읽으므로
 * 노드가 멀면 지연이 커진다.
 *
 * 호출 체인:
 *   alloc_event_buffer()/alloc_cwwb_sem()/amd_iommu_alloc_ppr_log() → [이 함수]
 */
void *__init iommu_alloc_4k_pages(struct amd_iommu *iommu, gfp_t gfp,
				  size_t size)
{
	int nid = iommu->dev ? dev_to_node(&iommu->dev->dev) : NUMA_NO_NODE;	/* [한국어] 유닛과 같은 노드에서 잡아 접근 지연을 줄인다 */
	void *buf;	/* [한국어] 할당한 버퍼 */

	size = PAGE_ALIGN(size);	/* [한국어] 페이지 단위로 올림 */
	buf = iommu_alloc_pages_node_sz(nid, gfp, size);	/* [한국어] 그 노드에서 연속 페이지를 */
	if (!buf)	/* [한국어] 메모리 부족 */
		return NULL;	/* [한국어] 실패 */
	if (check_feature(FEATURE_SNP) &&	/* [한국어] SNP 환경에서는 하드웨어가 접근하는 페이지가 4KB 매핑이어야 한다 */
	    set_memory_4k((unsigned long)buf, size / PAGE_SIZE)) {	/* [한국어] 커널의 2MB 큰 페이지 매핑을 그 구간만 쪼갠다 */
		iommu_free_pages(buf);	/* [한국어] 쪼개기에 실패하면 */
		return NULL;	/* [한국어] 큰 페이지로 남은 채 넘기면 SNP 무결성 검사에 걸린다 */
	}

	return buf;	/* [한국어] 하드웨어가 안전하게 접근할 수 있는 버퍼 */
}

/* allocates the memory where the IOMMU will log its events to */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * alloc_event_buffer - 이벤트 로그 버퍼를 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * 하드웨어 → 드라이버 방향의 링이다. 변환 실패, 잘못된 명령, 하드웨어
 * 오류가 여기 쌓인다.
 *
 * 명령 버퍼와 달리 iommu_alloc_4k_pages 를 쓰는 이유: 하드웨어가 이 버퍼에
 * 쓰기를 하므로 SNP 의 4KB 매핑 요구가 적용된다.
 */
static int __init alloc_event_buffer(struct amd_iommu *iommu)
{
	iommu->evt_buf = iommu_alloc_4k_pages(iommu, GFP_KERNEL,	/* [한국어] 하드웨어가 쓰기를 하므로 SNP 의 4KB 요구가 적용된다 */
					      EVT_BUFFER_SIZE);	/* [한국어] 512개 항목(8KB) */

	return iommu->evt_buf ? 0 : -ENOMEM;	/* [한국어] 실패하면 오류를 보고할 수단이 없어진다 */
}

/*
 * [한국어]
 * iommu_enable_event_buffer - 이벤트 로그의 주소를 알리고 켠다
 *
 * @iommu: 대상 유닛.
 *
 * 명령 버퍼와 같은 kdump 분기가 있다(원 주석). 다만 머리/꼬리 초기화는
 * kdump 여부와 무관하게 한다 — 이벤트 로그는 명령 버퍼와 달리 진행 중인
 * 작업이 없어, 옛 항목을 버리고 처음부터 시작해도 잃는 것이 없다.
 *
 * 원 주석이 "수동으로"라고 밝히듯 하드웨어가 포인터를 초기화해 주지 않는다.
 * 남은 값이 있으면 하드웨어가 엉뚱한 위치에서 쓰기 시작한다.
 *
 * 인터럽트는 여기서 켜지 않는다. 핸들러를 등록하기 전에 인터럽트가 오면
 * 처리할 곳이 없으므로, 그것은 나중 단계가 맡는다.
 *
 * 호출 체인:
 *   early_enable_iommu()/enable_iommus() → [이 함수]
 */
static void iommu_enable_event_buffer(struct amd_iommu *iommu)
{
	u64 entry;	/* [한국어] 주소+크기 워드 */

	BUG_ON(iommu->evt_buf == NULL);	/* [한국어] 버퍼 없이 켤 수 없다 */

	if (!is_kdump_kernel()) {	/* [한국어] (원 주석: kdump 에서는 버퍼를 재사용한다) */
		/*
		 * Event buffer is re-used for kdump kernel and setting
		 * of MMIO register is not required.
		 */
		entry = iommu_virt_to_phys(iommu->evt_buf) | EVT_LEN_MASK;	/* [한국어] 물리 주소와 크기를 한 워드에 */
		memcpy_toio(iommu->mmio_base + MMIO_EVT_BUF_OFFSET,	/* [한국어] 하드웨어에 알린다 */
			    &entry, sizeof(entry));	/* [한국어] kdump 에서는 옛 주소를 그대로 둔다 */
	}

	/* set head and tail to zero manually */
	writel(0x00, iommu->mmio_base + MMIO_EVT_HEAD_OFFSET);	/* [한국어] (원 주석: 수동으로 초기화) 이벤트 로그는 진행 중인 작업이 없어 kdump 에서도 되돌린다 */
	writel(0x00, iommu->mmio_base + MMIO_EVT_TAIL_OFFSET);	/* [한국어] 남은 값이 있으면 하드웨어가 엉뚱한 위치에서 쓴다 */

	iommu_feature_enable(iommu, CONTROL_EVT_LOG_EN);	/* [한국어] 기록만 켠다 — 인터럽트는 핸들러 등록 후에 */
}

/*
 * This function disables the event log buffer
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_disable_event_buffer - 이벤트 로그 기록을 끈다
 *
 * @iommu: 대상 유닛.
 *
 * 인터럽트는 별도로 꺼야 한다. iommu_disable() 이 그 순서를 지킨다.
 */
static void iommu_disable_event_buffer(struct amd_iommu *iommu)
{
	iommu_feature_disable(iommu, CONTROL_EVT_LOG_EN);	/* [한국어] 기록만 끈다. 인터럽트는 iommu_disable() 이 순서에 맞춰 따로 끈다 */
}

/*
 * [한국어]
 * free_event_buffer - 이벤트 로그 버퍼를 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * 초기화 실패 정리 전용이다.
 */
static void __init free_event_buffer(struct amd_iommu *iommu)
{
	iommu_free_pages(iommu->evt_buf);	/* [한국어] 초기화 실패 정리 전용 */
}

/*
 * [한국어]
 * free_ga_log - GA 로그와 그 꼬리 포인터 메모리를 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * 두 개를 놓는 것이 눈에 띈다. GA 로그는 다른 로그와 달리 꼬리 포인터를
 * 레지스터가 아니라 메모리에 두므로, 그 한 워드짜리 페이지도 함께 잡혀 있다.
 *
 * 함수 몸통 전체가 #ifdef 안에 있고 껍데기는 밖에 있다. 호출부를 조건부로
 * 만들지 않기 위해서다.
 */
static void free_ga_log(struct amd_iommu *iommu)
{
#ifdef CONFIG_IRQ_REMAP
	iommu_free_pages(iommu->ga_log);	/* [한국어] 로그 본체 */
	iommu_free_pages(iommu->ga_log_tail);	/* [한국어] 꼬리 포인터용 한 페이지. GA 로그만 꼬리를 메모리에 둔다 */
#endif
}

#ifdef CONFIG_IRQ_REMAP
/*
 * [한국어]
 * iommu_ga_log_enable - GA 로그를 하드웨어에 걸고 동작을 확인한다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -EINVAL 이면 로그가 없거나 동작 확인에 실패.
 *
 * GA 로그는 게스트에 직접 전달하지 못한 인터럽트가 기록되는 곳이다. KVM 이
 * 그것을 보고 대신 주입하므로, 이 로그가 동작하지 않으면 게스트가 인터럽트를
 * 놓친다.
 *
 * 꼬리 포인터 주소를 마스킹하는 것이 이 함수의 특이한 부분이다. 52비트로
 * 자르고 하위 3비트를 지우는데, 전자는 물리 주소 폭의 상한이고 후자는
 * 8바이트 정렬 요구다 — 그 비트들은 다른 용도로 쓰이므로 남겨 두면 하드웨어가
 * 오해한다.
 *
 * 마지막에 상태 비트를 폴링해 실제로 동작을 시작했는지 확인한다. 다른
 * 로그와 달리 이 확인을 하는 이유: GA 로그는 하드웨어가 거부할 수 있는
 * 조건(vAPIC 설정 등)이 더 많고, 조용히 실패하면 게스트 인터럽트가
 * 사라지는 것으로만 드러나 원인을 찾기 어렵다.
 *
 * 호출 체인:
 *   amd_iommu_enable_interrupts() → [이 함수]
 */
static int iommu_ga_log_enable(struct amd_iommu *iommu)
{
	u32 status, i;	/* [한국어] 상태 값과 폴링 카운터 */
	u64 entry;	/* [한국어] 레지스터에 쓸 워드 */

	if (!iommu->ga_log)	/* [한국어] 로그를 잡지 않았다(vAPIC 모드가 아니다) */
		return -EINVAL;	/* [한국어] 켤 것이 없다 */

	entry = iommu_virt_to_phys(iommu->ga_log) | GA_LOG_SIZE_512;	/* [한국어] 주소와 크기 인코딩 */
	memcpy_toio(iommu->mmio_base + MMIO_GA_LOG_BASE_OFFSET,	/* [한국어] 로그 본체의 위치를 알린다 */
		    &entry, sizeof(entry));	/* [한국어] 설정 */
	entry = (iommu_virt_to_phys(iommu->ga_log_tail) &	/* [한국어] 꼬리 포인터가 놓일 메모리의 주소 */
		 (BIT_ULL(52)-1)) & ~7ULL;	/* [한국어] 52비트로 자르고 하위 3비트를 지운다 — 그 자리는 다른 용도라 남기면 오해된다 */
	memcpy_toio(iommu->mmio_base + MMIO_GA_LOG_TAIL_OFFSET,	/* [한국어] 꼬리 위치를 알린다 */
		    &entry, sizeof(entry));	/* [한국어] 설정 */
	writel(0x00, iommu->mmio_base + MMIO_GA_HEAD_OFFSET);	/* [한국어] 머리를 0 으로 */
	writel(0x00, iommu->mmio_base + MMIO_GA_TAIL_OFFSET);	/* [한국어] 꼬리도 0 으로 */


	iommu_feature_enable(iommu, CONTROL_GAINT_EN);	/* [한국어] 인터럽트를 먼저 */
	iommu_feature_enable(iommu, CONTROL_GALOG_EN);	/* [한국어] 그다음 기록 */

	for (i = 0; i < MMIO_STATUS_TIMEOUT; ++i) {	/* [한국어] 실제로 동작을 시작했는지 확인한다 */
		status = readl(iommu->mmio_base + MMIO_STATUS_OFFSET);	/* [한국어] 상태를 읽어 */
		if (status & (MMIO_STATUS_GALOG_RUN_MASK))	/* [한국어] 동작 중 비트가 섰으면 */
			break;	/* [한국어] 성공 */
		udelay(10);	/* [한국어] 짧게 기다린다 */
	}

	if (WARN_ON(i >= MMIO_STATUS_TIMEOUT))	/* [한국어] 끝내 동작하지 않았다 */
		return -EINVAL;	/* [한국어] 조용히 실패하면 게스트 인터럽트가 사라지는 것으로만 드러나 원인을 찾기 어렵다 */

	return 0;	/* [한국어] GA 로그가 동작을 시작했다 */
}

/*
 * [한국어]
 * iommu_init_ga_log - GA 로그 버퍼와 꼬리 포인터 메모리를 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공(vAPIC 모드가 아니면 그대로 0), -EINVAL 이면 실패.
 *
 * vAPIC 모드가 아니면 아무것도 하지 않고 성공을 돌려준다. GA 로그는 게스트
 * 직접 전달의 부산물이라, 그 기능을 쓰지 않으면 로그도 필요 없다.
 *
 * 꼬리 포인터를 위해 8바이트짜리 할당을 따로 하는 것이 눈에 띈다. 하드웨어가
 * 그 주소에 꼬리 값을 쓰므로 페이지 정렬된 별도 메모리가 필요하다.
 *
 * 호출 체인:
 *   init_iommu_one() → [이 함수]
 */
static int iommu_init_ga_log(struct amd_iommu *iommu)
{
	int nid = iommu->dev ? dev_to_node(&iommu->dev->dev) : NUMA_NO_NODE;	/* [한국어] 유닛과 같은 노드에서 */

	if (!AMD_IOMMU_GUEST_IR_VAPIC(amd_iommu_guest_ir))	/* [한국어] 게스트 직접 전달을 쓰지 않으면 */
		return 0;	/* [한국어] GA 로그도 필요 없다 */

	iommu->ga_log = iommu_alloc_pages_node_sz(nid, GFP_KERNEL, GA_LOG_SIZE);	/* [한국어] 로그 본체(항목 8바이트 × 512) */
	if (!iommu->ga_log)	/* [한국어] 할당 실패 */
		goto err_out;	/* [한국어] 둘 다 놓고 나간다 */

	iommu->ga_log_tail = iommu_alloc_pages_node_sz(nid, GFP_KERNEL, 8);	/* [한국어] 하드웨어가 꼬리 값을 쓸 8바이트 */
	if (!iommu->ga_log_tail)	/* [한국어] 할당 실패 */
		goto err_out;	/* [한국어] 정리 */

	return 0;	/* [한국어] 준비 완료 */
err_out:
	free_ga_log(iommu);	/* [한국어] 부분적으로 잡힌 것을 놓는다 */
	return -EINVAL;	/* [한국어] vAPIC 을 쓸 수 없게 된다 */
}
#endif /* CONFIG_IRQ_REMAP */

/*
 * [한국어]
 * alloc_cwwb_sem - 완료 대기 값이 쓰일 메모리를 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * AMD 에서 "명령이 끝났다"를 아는 유일한 방법이 이것이다. 완료 대기 명령이
 * 이 주소에 지정한 값을 쓰고, 드라이버가 그 값을 폴링한다.
 *
 * 물리 주소를 미리 구해 두는 이유: 명령에는 물리 주소를 실어야 하는데,
 * 무효화는 핫패스라 매번 변환하기보다 한 번 구해 보관하는 편이 낫다.
 * kdump 에서 virt_to_phys 가 기대대로 동작하지 않는 문제도 함께 피한다.
 *
 * 4k 할당을 쓰는 이유: 하드웨어가 이 페이지에 쓰기를 하므로 SNP 의 매핑
 * 요구가 적용된다.
 */
static int __init alloc_cwwb_sem(struct amd_iommu *iommu)
{
	iommu->cmd_sem = iommu_alloc_4k_pages(iommu, GFP_KERNEL, 1);	/* [한국어] 하드웨어가 여기에 완료 값을 쓴다 — SNP 의 4KB 요구가 적용된다 */
	if (!iommu->cmd_sem)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 완료를 알 방법이 없어진다 */
	iommu->cmd_sem_paddr = iommu_virt_to_phys((void *)iommu->cmd_sem);	/* [한국어] 명령마다 변환하지 않도록 미리 구해 둔다. kdump 의 변환 문제도 함께 피한다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * remap_event_buffer - kdump 에서 옛 커널의 이벤트 버퍼를 이어 쓴다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENOMEM 이면 매핑 실패.
 *
 * 하드웨어 레지스터에 남아 있는 주소를 읽어 그 물리 메모리를 매핑한다.
 * 새 버퍼를 잡아 주소를 바꾸지 않는 이유: 그 전환이 원자적이지 않아
 * 그 사이의 이벤트가 어느 버퍼로 갈지 알 수 없다.
 *
 * 마스킹으로 크기 인코딩 비트를 떼고 순수 주소만 남긴다.
 *
 * 호출 체인:
 *   alloc_iommu_buffers() (kdump) → [이 함수] → iommu_memremap()
 */
static int __init remap_event_buffer(struct amd_iommu *iommu)
{
	u64 paddr;	/* [한국어] 옛 커널이 하드웨어에 알렸던 주소 */

	pr_info_once("Re-using event buffer from the previous kernel\n");	/* [한국어] 한 번만 알린다 */
	paddr = readq(iommu->mmio_base + MMIO_EVT_BUF_OFFSET) & PM_ADDR_MASK;	/* [한국어] 레지스터에서 읽고 크기 인코딩 비트를 뗀다 */
	iommu->evt_buf = iommu_memremap(paddr, EVT_BUFFER_SIZE);	/* [한국어] 그 물리 메모리를 매핑해 이어 쓴다 */

	return iommu->evt_buf ? 0 : -ENOMEM;	/* [한국어] 매핑 실패 */
}

/*
 * [한국어]
 * remap_command_buffer - kdump 에서 옛 커널의 명령 버퍼를 이어 쓴다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENOMEM 이면 매핑 실패.
 *
 * 이벤트 버퍼와 같은 이유와 방법이다. 명령 버퍼 쪽은 이어 쓰는 것이 더
 * 중요한데, 하드웨어가 처리 중인 명령이 있을 수 있기 때문이다.
 *
 * 호출 체인:
 *   alloc_iommu_buffers() (kdump) → [이 함수] → iommu_memremap()
 */
static int __init remap_command_buffer(struct amd_iommu *iommu)
{
	u64 paddr;	/* [한국어] 옛 주소 */

	pr_info_once("Re-using command buffer from the previous kernel\n");	/* [한국어] 한 번만 알린다 */
	paddr = readq(iommu->mmio_base + MMIO_CMD_BUF_OFFSET) & PM_ADDR_MASK;	/* [한국어] 레지스터에서 읽는다 */
	iommu->cmd_buf = iommu_memremap(paddr, CMD_BUFFER_SIZE);	/* [한국어] 처리 중인 명령이 있을 수 있어 이어 쓰는 것이 특히 중요하다 */

	return iommu->cmd_buf ? 0 : -ENOMEM;	/* [한국어] 매핑 실패 */
}

/*
 * [한국어]
 * remap_or_alloc_cwwb_sem - kdump 에서 완료 대기 메모리를 이어 쓰거나 새로 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENOMEM 이면 실패.
 *
 * SNP 여부로 길이 갈린다. 원 주석이 이유를 밝힌다: SNP 환경에서는 제외
 * 범위 레지스터가 완료 대기 버퍼 주소로 재사용되므로, 거기서 옛 주소를
 * 읽어 이어 쓸 수 있다.
 *
 * SNP 가 아니면 그 주소를 알 방법이 없어 새로 잡는다. 그래도 문제가 없는
 * 이유: 완료 대기 메모리는 명령 버퍼와 달리 "진행 중인 상태"를 담지 않는다.
 * 새 값을 새 주소에 쓰기 시작하면 그만이다.
 *
 * 호출 체인:
 *   alloc_iommu_buffers() (kdump) → [이 함수] → alloc_cwwb_sem()
 */
static int __init remap_or_alloc_cwwb_sem(struct amd_iommu *iommu)
{
	u64 paddr;	/* [한국어] 옛 주소 */

	if (check_feature(FEATURE_SNP)) {	/* [한국어] (원 주석: SNP 에서는 제외 범위 레지스터가 완료 대기 버퍼 주소로 쓰인다) */
		/*
		 * When SNP is enabled, the exclusion base register is used for the
		 * completion wait buffer (CWB) address. Read and re-use it.
		 */
		pr_info_once("Re-using CWB buffers from the previous kernel\n");	/* [한국어] 한 번만 알린다 */
		paddr = readq(iommu->mmio_base + MMIO_EXCL_BASE_OFFSET) & PM_ADDR_MASK;	/* [한국어] 그래서 거기서 옛 주소를 읽을 수 있다 */
		iommu->cmd_sem = iommu_memremap(paddr, PAGE_SIZE);	/* [한국어] 그 메모리를 이어 쓴다 */
		if (!iommu->cmd_sem)	/* [한국어] 매핑 실패 */
			return -ENOMEM;	/* [한국어] 실패 보고 */
		iommu->cmd_sem_paddr = paddr;	/* [한국어] 이미 물리 주소를 알고 있으므로 그대로 보관 */
	} else {
		return alloc_cwwb_sem(iommu);	/* [한국어] SNP 가 아니면 옛 주소를 알 방법이 없어 새로 잡는다 — 진행 중인 상태가 없어 문제되지 않는다 */
	}

	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * alloc_iommu_buffers - 유닛의 세 버퍼를 모두 준비한다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, 음수면 어느 하나가 실패.
 *
 * kdump 인지에 따라 통째로 길이 갈린다. 물려받은 커널에서는 세 버퍼를 모두
 * 옛 커널의 것으로 이어 쓰고(원 주석), 평범한 부팅에서는 새로 잡는다.
 *
 * 순서가 같은 것이 눈에 띈다: 완료 대기 → 명령 → 이벤트. 완료 대기가
 * 먼저인 이유는 명령 버퍼를 켤 때 이미 그 주소가 필요하기 때문이다.
 *
 * 실패하면 되감지 않고 그대로 돌아가는데, 호출자(init_iommu_one)가
 * free_iommu_buffers 로 부분적으로 잡힌 것까지 함께 정리한다.
 *
 * 호출 체인:
 *   init_iommu_one() → [이 함수]
 */
static int __init alloc_iommu_buffers(struct amd_iommu *iommu)
{
	int ret;	/* [한국어] 각 단계의 결과 */

	/*
	 * Reuse/Remap the previous kernel's allocated completion wait
	 * command and event buffers for kdump boot.
	 */
	if (is_kdump_kernel()) {	/* [한국어] (원 주석: kdump 에서는 옛 커널의 버퍼를 이어 쓴다) */
		ret = remap_or_alloc_cwwb_sem(iommu);	/* [한국어] 완료 대기 메모리부터 — 명령 버퍼를 켤 때 이미 필요하다 */
		if (ret)
			return ret;

		ret = remap_command_buffer(iommu);	/* [한국어] 명령 버퍼. 처리 중인 명령이 있을 수 있어 이어 쓰는 것이 중요하다 */
		if (ret)
			return ret;

		ret = remap_event_buffer(iommu);	/* [한국어] 이벤트 버퍼 */
		if (ret)
			return ret;
	} else {
		ret = alloc_cwwb_sem(iommu);	/* [한국어] 평범한 부팅에서는 새로 잡는다. 순서는 같다 */
		if (ret)
			return ret;

		ret = alloc_command_buffer(iommu);	/* [한국어] 명령 버퍼 */
		if (ret)
			return ret;

		ret = alloc_event_buffer(iommu);	/* [한국어] 이벤트 버퍼 */
		if (ret)
			return ret;
	}

	return 0;	/* [한국어] 세 버퍼가 모두 준비됐다 */
}

/*
 * [한국어]
 * free_cwwb_sem - 완료 대기 메모리를 놓는다 (평범한 부팅)
 *
 * @iommu: 대상 유닛.
 */
static void __init free_cwwb_sem(struct amd_iommu *iommu)
{
	if (iommu->cmd_sem)	/* [한국어] 잡히지 않았을 수 있다 */
		iommu_free_pages((void *)iommu->cmd_sem);	/* [한국어] 평범한 부팅에서는 우리가 잡은 페이지다 */
}
/*
 * [한국어]
 * unmap_cwwb_sem - 완료 대기 메모리를 놓는다 (kdump)
 *
 * @iommu: 대상 유닛.
 *
 * kdump 안에서도 한 번 더 갈린다. SNP 환경에서는 옛 커널의 메모리를
 * 매핑해 이어 썼으므로 매핑 해제이고, 아니면 새로 잡았으므로 페이지
 * 반납이다 — remap_or_alloc_cwwb_sem 의 두 갈래와 정확히 짝이 맞는다.
 */
static void __init unmap_cwwb_sem(struct amd_iommu *iommu)
{
	if (iommu->cmd_sem) {	/* [한국어] 잡히지 않았을 수 있다 */
		if (check_feature(FEATURE_SNP))	/* [한국어] SNP 에서는 옛 커널의 메모리를 매핑해 이어 썼다 */
			memunmap((void *)iommu->cmd_sem);	/* [한국어] 그래서 매핑 해제 */
		else
			iommu_free_pages((void *)iommu->cmd_sem);	/* [한국어] 아니면 새로 잡았으므로 페이지 반납 */
	}
}

/*
 * [한국어]
 * unmap_command_buffer - 물려받은 명령 버퍼의 매핑을 푼다
 *
 * @iommu: 대상 유닛.
 *
 * kdump 전용이다. 옛 커널의 메모리이므로 해제하지 않고 매핑만 푼다 —
 * 그 페이지의 주인은 우리가 아니다.
 */
static void __init unmap_command_buffer(struct amd_iommu *iommu)
{
	memunmap((void *)iommu->cmd_buf);	/* [한국어] 옛 커널의 페이지이므로 해제하지 않고 매핑만 푼다 */
}

/*
 * [한국어]
 * unmap_event_buffer - 물려받은 이벤트 버퍼의 매핑을 푼다
 *
 * @iommu: 대상 유닛.
 */
static void __init unmap_event_buffer(struct amd_iommu *iommu)
{
	memunmap(iommu->evt_buf);	/* [한국어] 같은 이유 */
}

/*
 * [한국어]
 * free_iommu_buffers - 유닛의 세 버퍼를 모두 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * alloc_iommu_buffers 와 같은 갈림길을 그대로 쓴다. 잡은 방법과 놓는 방법이
 * 짝을 이루어야 하므로 두 함수가 같은 조건으로 갈리는 것이 중요하다.
 *
 * 호출 체인:
 *   free_iommu_one() → [이 함수]
 */
static void __init free_iommu_buffers(struct amd_iommu *iommu)
{
	if (is_kdump_kernel()) {	/* [한국어] 잡은 방법과 놓는 방법이 짝을 이뤄야 한다 */
		unmap_cwwb_sem(iommu);	/* [한국어] 완료 대기 */
		unmap_command_buffer(iommu);	/* [한국어] 명령 버퍼 */
		unmap_event_buffer(iommu);	/* [한국어] 이벤트 버퍼 */
	} else {
		free_cwwb_sem(iommu);	/* [한국어] 평범한 부팅에서는 페이지 반납 */
		free_command_buffer(iommu);	/* [한국어] 명령 버퍼 */
		free_event_buffer(iommu);	/* [한국어] 이벤트 버퍼 */
	}
}

/*
 * [한국어]
 * iommu_enable_xt - x2APIC 목적지(32비트 APIC id)를 켠다
 *
 * @iommu: 대상 유닛.
 *
 * 원 주석이 전제 조건을 밝힌다: XT 모드는 GA 모드(128비트 IRTE)가 있어야
 * 성립한다. 32비트 APIC id 를 담으려면 항목이 커야 하는데, 32비트 IRTE 의
 * 목적지 필드는 8비트뿐이기 때문이다.
 *
 * 그래서 두 조건을 함께 본다 — 128비트 형식을 쓰고 있고, x2APIC 모드로
 * 가기로 결정됐는가.
 *
 * 함수 몸통이 통째로 #ifdef 안에 있고 껍데기는 밖에 있다. 재매핑을 끈
 * 커널에서는 아무 일도 하지 않는다.
 *
 * 호출 체인:
 *   early_enable_iommu()/enable_iommus() → [이 함수]
 */
static void iommu_enable_xt(struct amd_iommu *iommu)
{
#ifdef CONFIG_IRQ_REMAP
	/*
	 * XT mode (32-bit APIC destination ID) requires
	 * GA mode (128-bit IRTE support) as a prerequisite.
	 */
	if (AMD_IOMMU_GUEST_IR_GA(amd_iommu_guest_ir) &&	/* [한국어] (원 주석: XT 모드는 GA 모드를 전제로 한다) */
	    amd_iommu_xt_mode == IRQ_REMAP_X2APIC_MODE)	/* [한국어] 32비트 APIC id 는 128비트 항목에만 담긴다 */
		iommu_feature_enable(iommu, CONTROL_XT_EN);	/* [한국어] 두 조건이 맞을 때만 켠다 */
#endif /* CONFIG_IRQ_REMAP */
}

/*
 * [한국어]
 * iommu_enable_gt - 게스트 변환(PASID 별 변환)을 켠다
 *
 * @iommu: 대상 유닛.
 *
 * PASID 마다 다른 페이지 테이블을 쓰는 기능이며, SVA 와 중첩 변환의 전제다.
 * 하드웨어가 지원하지 않으면 조용히 돌아간다.
 *
 * GCR3TRPMODE 를 함께 켜는 것의 순서 제약을 원 주석이 설명한다: 그 기능은
 * iommu_snp_enable() 보다 먼저 켜져 있어야 하고, 이 함수가
 * early_enable_iommu() 에서 불리므로 여기서 켜는 것이 안전하다.
 *
 * 호출 체인:
 *   early_enable_iommu()/enable_iommus() → [이 함수]
 */
static void iommu_enable_gt(struct amd_iommu *iommu)
{
	if (!check_feature(FEATURE_GT))	/* [한국어] 하드웨어가 게스트 변환을 지원하지 않으면 */
		return;	/* [한국어] SVA 와 중첩 변환을 쓸 수 없다 */

	iommu_feature_enable(iommu, CONTROL_GT_EN);	/* [한국어] PASID 별 변환을 켠다 */

	/*
	 * This feature needs to be enabled prior to a call
	 * to iommu_snp_enable(). Since this function is called
	 * in early_enable_iommu(), it is safe to enable here.
	 */
	if (check_feature2(FEATURE_GCR3TRPMODE))	/* [한국어] (원 주석: iommu_snp_enable() 보다 먼저 켜져 있어야 한다) */
		iommu_feature_enable(iommu, CONTROL_GCR3TRPMODE);	/* [한국어] early_enable_iommu 에서 불리므로 이 순서가 보장된다 */
}

/* sets a specific bit in the device table entry. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * set_dte_bit - DTE 의 비트 하나를 세운다
 *
 * @dte: 대상 장치 테이블 항목.
 * @bit: 0~255 의 비트 번호.
 *
 * DTE 는 256비트라 u64 배열 넷으로 표현된다. 비트 번호를 그 배열의 인덱스와
 * 워드 안의 위치로 쪼개는 것이 이 함수의 전부다 — 상위 2비트가 인덱스,
 * 하위 6비트가 워드 안의 자리다.
 *
 * 이런 도우미가 필요한 이유: IVHD 가 알려 주는 플래그는 DTE 안의 절대 비트
 * 번호(DEV_ENTRY_*)로 주어지는데, 그것을 배열 접근으로 옮기는 계산을
 * 호출부마다 반복할 수 없다.
 *
 * 호출 체인:
 *   set_dev_entry_from_acpi() → [이 함수]
 */
static void set_dte_bit(struct dev_table_entry *dte, u8 bit)
{
	int i = (bit >> 6) & 0x03;	/* [한국어] 상위 2비트가 u64 배열의 인덱스 */
	int _bit = bit & 0x3f;	/* [한국어] 하위 6비트가 그 워드 안의 자리 */

	dte->data[i] |= (1UL << _bit);	/* [한국어] 256비트를 배열로 다루기 위한 주소 계산 */
}

/*
 * [한국어]
 * __reuse_device_table - kdump 에서 옛 커널의 장치 테이블을 이어받는다
 *
 * @iommu: 대상 유닛.
 * @return: 이어받기에 성공하면 참.
 *
 * kdump 경로에서 가장 조심스러운 함수다. 크래시한 커널이 만든 장치 테이블을
 * 그대로 쓰면 살아 있는 장치의 DMA 가 끊기지 않지만, 그러려면 그 표가
 * 우리가 기대하는 형태인지 확인해야 한다.
 *
 * 세 가지를 검증한다.
 *  1) 크기가 우리 계산과 같은가. 다르면 인덱스가 어긋나 엉뚱한 항목을 읽는다.
 *  2) 주소가 4G 아래인가. 원 주석이 "신뢰할 수 없다"고 표현하는데, 옛
 *     커널이 정상적으로 잡았다면 GFP_DMA32 로 4G 아래에 있어야 하고,
 *     아니라면 레지스터 값 자체가 깨졌을 가능성이 높다.
 *  3) SME 비트를 떼어 진짜 물리 주소를 얻는다(원 주석).
 *
 * 그다음이 이 함수의 핵심이다: 옛 표에서 쓰이던 도메인 id 를 모두 예약한다.
 * 그러지 않으면 새 커널이 같은 id 를 다른 도메인에 발급하고, 하드웨어는
 * 두 도메인의 캐시를 같은 태그로 섞어 버린다.
 *
 * -ENOSPC 를 무시하는 이유도 원 주석에 있다: 여러 장치가 같은 도메인을
 * 쓰면 같은 id 를 여러 번 예약하게 되고, 두 번째부터는 -ENOSPC 가 정상이다.
 * 진짜 문제는 -ENOMEM 뿐이다.
 *
 * 호출 체인:
 *   reuse_device_table() → [이 함수] → iommu_memremap()
 *     → amd_iommu_pdom_id_reserve()
 */
static bool __reuse_device_table(struct amd_iommu *iommu)
{
	struct amd_iommu_pci_seg *pci_seg = iommu->pci_seg;	/* [한국어] 이 유닛이 속한 세그먼트 */
	struct dev_table_entry *old_dev_tbl_entry;	/* [한국어] 옛 표의 항목 */
	u32 lo, hi, old_devtb_size, devid;	/* [한국어] 레지스터 두 조각, 옛 표의 크기, 순회 인덱스 */
	phys_addr_t old_devtb_phys;	/* [한국어] 옛 표의 진짜 물리 주소 */
	u16 dom_id;	/* [한국어] 옛 항목이 쓰던 도메인 id */
	bool dte_v;	/* [한국어] 그 항목이 유효했는가 */
	u64 entry;	/* [한국어] 레지스터 값 전체 */

	/* Each IOMMU use separate device table with the same size */
	lo = readl(iommu->mmio_base + MMIO_DEV_TABLE_OFFSET);	/* [한국어] (원 주석: 유닛마다 같은 크기의 별도 장치 테이블을 쓴다) */
	hi = readl(iommu->mmio_base + MMIO_DEV_TABLE_OFFSET + 4);	/* [한국어] 32비트씩 두 번 읽어 */
	entry = (((u64) hi) << 32) + lo;	/* [한국어] 64비트로 합친다 */

	old_devtb_size = ((entry & ~PAGE_MASK) + 1) << 12;	/* [한국어] 하위 비트에 페이지 수가 인코딩되어 있다 */
	if (old_devtb_size != pci_seg->dev_table_size) {	/* [한국어] 우리 계산과 다르면 */
		pr_err("The device table size of IOMMU:%d is not expected!\n",	/* [한국어] 인덱스가 어긋나 엉뚱한 항목을 읽게 된다 */
			iommu->index);
		return false;	/* [한국어] 물려받기를 포기한다 */
	}

	/*
	 * When SME is enabled in the first kernel, the entry includes the
	 * memory encryption mask(sme_me_mask), we must remove the memory
	 * encryption mask to obtain the true physical address in kdump kernel.
	 */
	old_devtb_phys = __sme_clr(entry) & PAGE_MASK;	/* [한국어] (원 주석: 첫 커널이 SME 를 켰다면 암호화 마스크를 떼어야 진짜 주소가 나온다) */

	if (old_devtb_phys >= 0x100000000ULL) {	/* [한국어] 옛 커널이 정상적으로 잡았다면 GFP_DMA32 로 4G 아래에 있어야 한다 */
		pr_err("The address of old device table is above 4G, not trustworthy!\n");	/* [한국어] (원 주석: 신뢰할 수 없다) 레지스터 값 자체가 깨졌을 가능성이 높다 */
		return false;	/* [한국어] 포기 */
	}

	/*
	 * Re-use the previous kernel's device table for kdump.
	 */
	pci_seg->old_dev_tbl_cpy = iommu_memremap(old_devtb_phys, pci_seg->dev_table_size);	/* [한국어] (원 주석: 옛 커널의 장치 테이블을 재사용한다) */
	if (pci_seg->old_dev_tbl_cpy == NULL) {	/* [한국어] 매핑 실패 */
		pr_err("Failed to remap memory for reusing old device table!\n");	/* [한국어] 읽을 수 없으면 물려받을 수 없다 */
		return false;	/* [한국어] 포기 */
	}

	for (devid = 0; devid <= pci_seg->last_bdf; devid++) {	/* [한국어] 옛 표의 모든 항목을 훑는다 */
		old_dev_tbl_entry = &pci_seg->old_dev_tbl_cpy[devid];	/* [한국어] 그 항목 */
		dte_v = FIELD_GET(DTE_FLAG_V, old_dev_tbl_entry->data[0]);	/* [한국어] 유효했는가 */
		dom_id = FIELD_GET(DTE_DOMID_MASK, old_dev_tbl_entry->data[1]);	/* [한국어] 어떤 도메인 id 를 쓰고 있었는가 */

		if (!dte_v || !dom_id)	/* [한국어] 쓰이지 않던 항목 */
			continue;	/* [한국어] 예약할 것이 없다 */
		/*
		 * ID reservation can fail with -ENOSPC when there
		 * are multiple devices present in the same domain,
		 * hence check only for -ENOMEM.
		 */
		if (amd_iommu_pdom_id_reserve(dom_id, GFP_KERNEL) == -ENOMEM)	/* [한국어] (원 주석: 여러 장치가 같은 도메인을 쓰면 -ENOSPC 가 나므로 -ENOMEM 만 본다) */
			return false;	/* [한국어] 예약하지 않으면 새 커널이 같은 id 를 재발급해 캐시가 섞인다 */
	}

	return true;	/* [한국어] 옛 표를 안전하게 이어받을 수 있다 */
}

/*
 * [한국어]
 * reuse_device_table - 모든 세그먼트에 대해 옛 장치 테이블을 이어받는다
 *
 * @return: 모두 성공하면 참.
 *
 * amd_iommu_pre_enabled 가 거짓이면 물려받을 것이 없다 — 옛 커널이 IOMMU 를
 * 켜지 않은 채 죽었거나, 애초에 kdump 가 아니다.
 *
 * 원 주석이 밝히는 최적화: 한 세그먼트의 모든 유닛이 같은 장치 테이블을
 * 공유하므로, 세그먼트마다 유닛 하나만 처리하면 된다. 안쪽 루프의 break 가
 * 그것이다.
 *
 * 하나라도 실패하면 전체를 포기한다. 일부만 물려받으면 어떤 장치는 옛
 * 매핑을, 어떤 장치는 빈 표를 보게 되어 상태를 알 수 없게 된다.
 *
 * 호출 체인:
 *   early_enable_iommus() → [이 함수] → __reuse_device_table()
 */
static bool reuse_device_table(void)
{
	struct amd_iommu *iommu;	/* [한국어] 유닛 순회용 */
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 세그먼트 순회용 */

	if (!amd_iommu_pre_enabled)	/* [한국어] 옛 커널이 IOMMU 를 켜지 않았거나 kdump 가 아니다 */
		return false;	/* [한국어] 물려받을 것이 없다 */

	pr_warn("Translation is already enabled - trying to reuse translation structures\n");	/* [한국어] 이 상황 자체가 특별하므로 알린다 */

	/*
	 * All IOMMUs within PCI segment shares common device table.
	 * Hence reuse device table only once per PCI segment.
	 */
	for_each_pci_segment(pci_seg) {	/* [한국어] (원 주석: 한 세그먼트의 모든 유닛이 장치 테이블을 공유한다) */
		for_each_iommu(iommu) {	/* [한국어] 그 세그먼트의 유닛을 찾는다 */
			if (pci_seg->id != iommu->pci_seg->id)	/* [한국어] 다른 세그먼트의 유닛 */
				continue;	/* [한국어] 건너뛴다 */
			if (!__reuse_device_table(iommu))	/* [한국어] 이어받기 시도 */
				return false;	/* [한국어] 하나라도 실패하면 전체를 포기한다 — 일부만 물려받으면 상태를 알 수 없다 */
			break;	/* [한국어] 세그먼트당 한 번이면 충분하다 */
		}
	}

	return true;	/* [한국어] 모든 세그먼트의 표를 이어받았다 */
}

/*
 * [한국어]
 * amd_iommu_get_ivhd_dte_flags - 이 장치에 적용할 IVHD 지정 플래그를 찾는다
 *
 * @segid: PCI 세그먼트.
 * @devid: 장치 id.
 * @return: 적용할 플래그가 담긴 DTE, 없으면 NULL.
 *
 * 가장 좁은 범위를 고르는 것이 이 함수의 요점이다. IVHD 는 같은 장치를
 * 여러 항목이 덮을 수 있다 — "모든 장치"를 지정한 항목과 "이 장치 하나"를
 * 지정한 항목이 함께 있을 수 있다.
 *
 * 그때 더 구체적인 지시가 이겨야 한다. 그래서 목록을 끝까지 훑으며 범위의
 * 길이가 가장 짧은 것을 고른다(원 주석). 먼저 찾은 것을 쓰면 목록의 순서에
 * 따라 결과가 달라진다.
 *
 * 호출 체인:
 *   amd_iommu_make_clear_dte()/set_dev_entry_from_acpi() → [이 함수]
 */
struct dev_table_entry *amd_iommu_get_ivhd_dte_flags(u16 segid, u16 devid)
{
	struct ivhd_dte_flags *e;	/* [한국어] 목록 커서 */
	unsigned int best_len = UINT_MAX;	/* [한국어] 지금까지 찾은 가장 좁은 범위의 길이 */
	struct dev_table_entry *dte = NULL;	/* [한국어] 그 범위의 플래그 */

	for_each_ivhd_dte_flags(e) {	/* [한국어] (원 주석: devid 를 포함하는 가장 작은 범위를 찾으려면 끝까지 훑어야 한다) */
		/*
		 * Need to go through the whole list to find the smallest range,
		 * which contains the devid.
		 */
		if ((e->segid == segid) &&	/* [한국어] 세그먼트가 맞고 */
		    (e->devid_first <= devid) && (devid <= e->devid_last)) {	/* [한국어] 이 장치를 덮는 범위인가 */
			unsigned int len = e->devid_last - e->devid_first;	/* [한국어] 그 범위의 길이 */

			if (len < best_len) {	/* [한국어] 더 구체적인 지시인가 */
				dte = &(e->dte);	/* [한국어] 그것을 쓴다 */
				best_len = len;	/* [한국어] 기준을 갱신. 먼저 찾은 것을 쓰면 목록 순서에 결과가 좌우된다 */
			}
		}
	}
	return dte;	/* [한국어] 가장 좁은 범위의 플래그, 없으면 NULL */
}

/*
 * [한국어]
 * search_ivhd_dte_flags - 이 범위의 플래그가 이미 등록되어 있는지 본다
 *
 * @segid: PCI 세그먼트.
 * @first: 범위의 시작 장치 id.
 * @last: 범위의 끝.
 * @return: 정확히 같은 범위의 항목이 이미 있으면 참.
 *
 * amd_iommu_get_ivhd_dte_flags 와 달리 "포함"이 아니라 "정확히 일치"를
 * 찾는다. 목적이 다르기 때문이다 — 그쪽은 적용할 플래그를 고르는 것이고,
 * 이쪽은 같은 항목을 두 번 등록하지 않기 위한 중복 검사다.
 *
 * 왜 중복이 생기는가: 한 IVRS 표에 같은 유닛에 대한 IVHD 가 여럿 있을 수
 * 있고, 같은 장치 범위가 반복해서 나타날 수 있다.
 *
 * 호출 체인:
 *   set_dev_entry_from_acpi_range() → [이 함수]
 */
static bool search_ivhd_dte_flags(u16 segid, u16 first, u16 last)
{
	struct ivhd_dte_flags *e;	/* [한국어] 목록 커서 */

	for_each_ivhd_dte_flags(e) {	/* [한국어] 등록된 항목을 훑는다 */
		if ((e->segid == segid) &&	/* [한국어] 세그먼트가 같고 */
		    (e->devid_first == first) &&	/* [한국어] 시작이 같고 */
		    (e->devid_last == last))	/* [한국어] 끝도 같은가 — "포함"이 아니라 "정확히 일치"를 본다 */
			return true;	/* [한국어] 같은 범위가 이미 등록됐다 */
	}
	return false;	/* [한국어] 새로 등록해야 한다 */
}

/*
 * This function takes the device specific flags read from the ACPI
 * table and sets up the device table entry with that information
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * set_dev_entry_from_acpi_range - IVHD 가 지정한 플래그를 장치 범위에 적용한다
 *
 * @iommu: 이 장치들을 담당하는 유닛.
 * @first: 범위의 시작 장치 id.
 * @last: 범위의 끝.
 * @flags: ACPI_DEVFLAG_* 조합.
 * @ext_flags: 확장 플래그(여기서는 쓰지 않는다).
 *
 * 두 가지 일을 함께 한다.
 *
 * 하나는 플래그를 DTE 형태로 번역해 두 곳에 남기는 것이다. 하나는 실제
 * 장치 테이블이고, 다른 하나는 amd_ivhd_dev_flags_list 다. 후자가 필요한
 * 이유: 장치를 도메인에서 뗄 때 DTE 를 초기 상태로 되돌리는데, 그때 이
 * 플래그들을 되살려야 한다(amd_iommu_make_clear_dte 참고). 표를 다시 읽을
 * 수는 없으므로 파싱 때 보관해 둔다.
 *
 * 다른 하나는 rlookup 표를 채우는 것이다. 이 범위의 장치들은 이 유닛이
 * 담당한다는 기록이며, 플래그가 없어도 반드시 해야 한다 — 그래서 그 호출만
 * if 밖에 있다.
 *
 * erratum 63 처리가 눈에 띈다. 시스템 관리 필드가 특정 값일 때 쓰기 권한을
 * 함께 줘야 하는 하드웨어 결함이며, 초기 DTE 에 그 정보가 있어야 하므로
 * 여기서 미리 얹는다.
 *
 * 호출 체인:
 *   init_iommu_from_acpi() → [이 함수] → set_dte_bit()
 *     → amd_iommu_set_rlookup_table()
 */
static void __init
set_dev_entry_from_acpi_range(struct amd_iommu *iommu, u16 first, u16 last,
			      u32 flags, u32 ext_flags)
{
	int i;	/* [한국어] 범위 순회 인덱스 */
	struct dev_table_entry dte = {};	/* [한국어] 플래그를 번역해 담을 DTE */

	/* Parse IVHD DTE setting flags and store information */
	if (flags) {	/* [한국어] (원 주석: IVHD 의 DTE 설정 플래그를 파싱해 보관한다) */
		struct ivhd_dte_flags *d;	/* [한국어] 목록에 남길 항목 */

		if (search_ivhd_dte_flags(iommu->pci_seg->id, first, last))	/* [한국어] 같은 범위가 이미 등록됐는가 */
			return;	/* [한국어] 한 표에 같은 범위가 반복해 나타날 수 있다 */

		d = kzalloc_obj(struct ivhd_dte_flags);	/* [한국어] 보관할 항목 */
		if (!d)	/* [한국어] 할당 실패 */
			return;	/* [한국어] 플래그를 보관하지 못하면 나중에 되살릴 수 없지만, 부팅은 계속한다 */

		pr_debug("%s: devid range %#x:%#x\n", __func__, first, last);	/* [한국어] 어떤 범위를 등록하는지 */

		if (flags & ACPI_DEVFLAG_INITPASS)	/* [한국어] INIT 인터럽트를 통과시키라는 지시 */
			set_dte_bit(&dte, DEV_ENTRY_INIT_PASS);	/* [한국어] DTE 의 해당 비트로 번역 */
		if (flags & ACPI_DEVFLAG_EXTINT)	/* [한국어] 외부 인터럽트 통과 */
			set_dte_bit(&dte, DEV_ENTRY_EINT_PASS);	/* [한국어] 번역 */
		if (flags & ACPI_DEVFLAG_NMI)	/* [한국어] NMI 통과 — 재매핑하면 놓칠 수 있어서 */
			set_dte_bit(&dte, DEV_ENTRY_NMI_PASS);	/* [한국어] 번역 */
		if (flags & ACPI_DEVFLAG_SYSMGT1)	/* [한국어] 시스템 관리 메시지 처리 비트 1 */
			set_dte_bit(&dte, DEV_ENTRY_SYSMGT1);	/* [한국어] 번역 */
		if (flags & ACPI_DEVFLAG_SYSMGT2)	/* [한국어] 같은 필드의 비트 2 */
			set_dte_bit(&dte, DEV_ENTRY_SYSMGT2);	/* [한국어] 번역 */
		if (flags & ACPI_DEVFLAG_LINT0)	/* [한국어] 로컬 인터럽트 0 통과 */
			set_dte_bit(&dte, DEV_ENTRY_LINT0_PASS);	/* [한국어] 번역 */
		if (flags & ACPI_DEVFLAG_LINT1)	/* [한국어] 로컬 인터럽트 1 통과 */
			set_dte_bit(&dte, DEV_ENTRY_LINT1_PASS);	/* [한국어] 번역 */

		/* Apply erratum 63, which needs info in initial_dte */
		if (FIELD_GET(DTE_DATA1_SYSMGT_MASK, dte.data[1]) == 0x1)	/* [한국어] (원 주석: erratum 63 — 초기 DTE 에 그 정보가 있어야 한다) */
			dte.data[0] |= DTE_FLAG_IW;	/* [한국어] 시스템 관리 필드가 이 값이면 쓰기 권한을 함께 줘야 하는 하드웨어 결함 */

		memcpy(&d->dte, &dte, sizeof(dte));	/* [한국어] 번역 결과를 보관한다 — 장치를 뗄 때 되살리기 위해서다 */
		d->segid = iommu->pci_seg->id;	/* [한국어] 적용 세그먼트 */
		d->devid_first = first;	/* [한국어] 범위의 시작 */
		d->devid_last = last;	/* [한국어] 범위의 끝 */
		list_add_tail(&d->list, &amd_ivhd_dev_flags_list);	/* [한국어] 목록에 등록. 표를 다시 읽을 수 없으므로 여기 남겨 둔다 */
	}

	for (i = first; i <= last; i++)  {	/* [한국어] 범위의 모든 장치에 대해 */
		if (flags) {
			struct dev_table_entry *dev_table = get_dev_table(iommu);	/* [한국어] 이 유닛이 쓰는 장치 테이블 */

			memcpy(&dev_table[i], &dte, sizeof(dte));	/* [한국어] 플래그가 있으면 DTE 에 미리 얹는다 */
		}
		amd_iommu_set_rlookup_table(iommu, i);	/* [한국어] 플래그가 없어도 담당 유닛 기록은 반드시 한다 — 그래서 if 밖에 있다 */
	}
}

/*
 * [한국어]
 * set_dev_entry_from_acpi - 장치 하나에 IVHD 플래그를 적용한다
 *
 * @iommu: 담당 유닛.
 * @devid: 장치 id.
 * @flags: ACPI_DEVFLAG_* 조합.
 * @ext_flags: 확장 플래그.
 *
 * 범위 버전에 시작과 끝을 같게 넘기는 얇은 껍데기다. IVHD 항목이 장치
 * 하나를 지정하는 경우와 범위를 지정하는 경우가 모두 있어, 호출부가
 * 어느 쪽인지 신경 쓰지 않게 두 입구를 둔 것이다.
 */
static void __init set_dev_entry_from_acpi(struct amd_iommu *iommu,
					   u16 devid, u32 flags, u32 ext_flags)
{
	set_dev_entry_from_acpi_range(iommu, devid, devid, flags, ext_flags);	/* [한국어] 시작과 끝을 같게 넘기면 장치 하나가 된다 */
}

/*
 * [한국어]
 * add_special_device - IOAPIC/HPET 의 id → 요청자 id 대응을 등록한다
 *
 * @type: IVHD_SPECIAL_IOAPIC 또는 IVHD_SPECIAL_HPET.
 * @id: 그 장치의 번호.
 * @devid: 요청자 id. 명령줄 우선 항목이 있으면 그 값으로 덮어써서 돌려준다.
 * @cmd_line: 이 등록이 커널 명령줄에서 온 것인지.
 * @return: 0 성공, -EINVAL 이면 모르는 종류, -ENOMEM 이면 할당 실패.
 *
 * 명령줄이 표를 이긴다는 규칙이 이 함수의 핵심이다. 목록에 같은 id 의
 * 명령줄 항목이 이미 있으면 새로 등록하지 않고, 오히려 호출자의 devid 를
 * 그 값으로 덮어쓴다 — 표를 파싱하던 코드가 그 뒤로는 사용자가 지정한
 * 값을 쓰게 된다.
 *
 * 왜 그런 우선순위가 필요한가: 펌웨어가 IOAPIC 대응을 잘못 적거나 빠뜨린
 * 기계가 실제로 있고(quirks.c 참고), 그때 사용자가 ivrs_ioapic= 로 고칠
 * 수 있어야 한다.
 *
 * 호출 체인:
 *   init_iommu_from_acpi()/add_early_maps()/ivrs_ioapic_quirk_cb() → [이 함수]
 */
int __init add_special_device(u8 type, u8 id, u32 *devid, bool cmd_line)
{
	struct devid_map *entry;	/* [한국어] 목록 커서이자 새 항목 */
	struct list_head *list;	/* [한국어] 대상 목록(IOAPIC 또는 HPET) */

	if (type == IVHD_SPECIAL_IOAPIC)	/* [한국어] IOAPIC 인가 */
		list = &ioapic_map;	/* [한국어] 그 목록으로 */
	else if (type == IVHD_SPECIAL_HPET)	/* [한국어] HPET 인가 */
		list = &hpet_map;	/* [한국어] 그 목록으로 */
	else
		return -EINVAL;	/* [한국어] 그 밖의 종류는 모른다 */

	list_for_each_entry(entry, list, list) {	/* [한국어] 기존 항목을 훑는다 */
		if (!(entry->id == id && entry->cmd_line))	/* [한국어] 같은 id 의 명령줄 항목만 우선한다 */
			continue;	/* [한국어] 표에서 온 항목은 우선순위가 없다 */

		pr_info("Command-line override present for %s id %d - ignoring\n",	/* [한국어] 사용자 지정이 이겼음을 알린다 */
			type == IVHD_SPECIAL_IOAPIC ? "IOAPIC" : "HPET", id);

		*devid = entry->devid;	/* [한국어] 호출자의 값을 사용자 지정으로 덮어쓴다 — 이후 파싱이 그 값을 쓴다 */

		return 0;	/* [한국어] 새로 등록하지 않는다 */
	}

	entry = kzalloc_obj(*entry);	/* [한국어] 새 항목 */
	if (!entry)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 이 대응을 등록할 수 없다 */

	entry->id	= id;	/* [한국어] 장치 번호 */
	entry->devid	= *devid;	/* [한국어] 요청자 id */
	entry->cmd_line	= cmd_line;	/* [한국어] 출처. 이후 우선순위 판단의 근거가 된다 */

	list_add_tail(&entry->list, list);	/* [한국어] 목록 끝에 붙인다 */

	return 0;	/* [한국어] 등록 완료 */
}

/*
 * [한국어]
 * add_acpi_hid_device - ACPI HID 장치의 대응을 등록한다
 *
 * @hid: ACPI HID 문자열.
 * @uid: UID 문자열(비어 있을 수 있다).
 * @devid: 요청자 id. 명령줄 우선 항목이 있으면 덮어써서 돌려준다.
 * @cmd_line: 명령줄에서 온 등록인지.
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * add_special_device 와 같은 우선순위 규칙을 따르되, 비교가 더 복잡하다.
 * HID 는 반드시 같아야 하고, UID 는 양쪽 모두 값이 있을 때만 비교한다 —
 * 한쪽이 비어 있으면 "지정하지 않음"이라 어느 UID 든 맞는 것으로 본다.
 *
 * root_devid 를 devid & ~0x7 로 두는 이유: 기능 번호(하위 3비트)를 지운
 * 값이 그 장치의 기능 0 이고, 여러 기능을 가진 플랫폼 장치의 대표가 된다.
 *
 * 호출 체인:
 *   init_iommu_from_acpi()/add_early_maps() → [이 함수]
 */
static int __init add_acpi_hid_device(u8 *hid, u8 *uid, u32 *devid,
				      bool cmd_line)
{
	struct acpihid_map_entry *entry;	/* [한국어] 목록 커서이자 새 항목 */
	struct list_head *list = &acpihid_map;	/* [한국어] 대상 목록 */

	list_for_each_entry(entry, list, list) {	/* [한국어] 기존 항목을 훑는다 */
		if (strcmp(entry->hid, hid) ||	/* [한국어] HID 는 반드시 같아야 하고 */
		    (*uid && *entry->uid && strcmp(entry->uid, uid)) ||	/* [한국어] UID 는 양쪽 다 값이 있을 때만 비교한다 — 비어 있으면 "지정 안 함"이다 */
		    !entry->cmd_line)	/* [한국어] 명령줄 항목만 우선한다 */
			continue;	/* [한국어] 조건이 안 맞으면 다음 */

		pr_info("Command-line override for hid:%s uid:%s\n",	/* [한국어] 사용자 지정이 이겼음을 알린다 */
			hid, uid);
		*devid = entry->devid;	/* [한국어] 호출자의 값을 덮어쓴다 */
		return 0;	/* [한국어] 새로 등록하지 않는다 */
	}

	entry = kzalloc_obj(*entry);	/* [한국어] 새 항목 */
	if (!entry)	/* [한국어] 할당 실패 */
		return -ENOMEM;	/* [한국어] 등록 불가 */

	memcpy(entry->uid, uid, strlen(uid));	/* [한국어] UID 문자열 복사 */
	memcpy(entry->hid, hid, strlen(hid));	/* [한국어] HID 문자열 복사 */
	entry->devid = *devid;	/* [한국어] 요청자 id */
	entry->cmd_line	= cmd_line;	/* [한국어] 출처 */
	entry->root_devid = (entry->devid & (~0x7));	/* [한국어] 기능 번호를 지운 값 — 여러 기능을 가진 장치의 대표가 된다 */

	pr_info("%s, add hid:%s, uid:%s, rdevid:%#x\n",	/* [한국어] 표에서 왔는지 명령줄에서 왔는지 함께 남긴다 */
		entry->cmd_line ? "cmd" : "ivrs",
		entry->hid, entry->uid, entry->root_devid);

	list_add_tail(&entry->list, list);	/* [한국어] 목록에 등록 */
	return 0;	/* [한국어] 완료 */
}

/*
 * [한국어]
 * add_early_maps - 커널 명령줄로 지정된 대응들을 목록에 옮긴다
 *
 * @return: 0 성공, 음수면 어느 하나가 실패.
 *
 * 명령줄은 아주 이른 시점에 파싱되는데, 그때는 아직 목록 자료구조를 쓸 수
 * 없어 고정 배열(early_*_map)에 담아 두었다. 이제 목록으로 옮긴다.
 *
 * 순서가 중요하다: IVHD 파싱보다 먼저 불려야 명령줄 항목이 목록에 이미
 * 있고, 그래야 add_special_device 의 우선순위 규칙이 동작한다. 반대로
 * 하면 표의 값이 먼저 등록되어 사용자의 지정이 무시된다.
 *
 * 호출 체인:
 *   init_iommu_from_acpi() (맨 앞) → [이 함수] → add_special_device()
 *     → add_acpi_hid_device()
 */
static int __init add_early_maps(void)
{
	int i, ret;	/* [한국어] 순회 인덱스와 결과 */

	for (i = 0; i < early_ioapic_map_size; ++i) {	/* [한국어] 명령줄로 지정된 IOAPIC 대응들 */
		ret = add_special_device(IVHD_SPECIAL_IOAPIC,	/* [한국어] 목록으로 옮긴다 */
					 early_ioapic_map[i].id,	/* [한국어] IOAPIC 번호 */
					 &early_ioapic_map[i].devid,	/* [한국어] 요청자 id */
					 early_ioapic_map[i].cmd_line);	/* [한국어] 명령줄 출처 표시 — 이것이 이후 우선순위의 근거다 */
		if (ret)	/* [한국어] 등록 실패 */
			return ret;	/* [한국어] 그대로 보고 */
	}

	for (i = 0; i < early_hpet_map_size; ++i) {	/* [한국어] HPET 대응들 */
		ret = add_special_device(IVHD_SPECIAL_HPET,	/* [한국어] 같은 방식 */
					 early_hpet_map[i].id,	/* [한국어] HPET 번호 */
					 &early_hpet_map[i].devid,	/* [한국어] 요청자 id */
					 early_hpet_map[i].cmd_line);	/* [한국어] 출처 */
		if (ret)
			return ret;
	}

	for (i = 0; i < early_acpihid_map_size; ++i) {	/* [한국어] ACPI HID 대응들 */
		ret = add_acpi_hid_device(early_acpihid_map[i].hid,	/* [한국어] HID 문자열 */
					  early_acpihid_map[i].uid,	/* [한국어] UID 문자열 */
					  &early_acpihid_map[i].devid,	/* [한국어] 요청자 id */
					  early_acpihid_map[i].cmd_line);	/* [한국어] 출처 */
		if (ret)
			return ret;
	}

	return 0;	/* [한국어] 명령줄 항목이 모두 목록에 들어갔다 — 이제 표 파싱이 그것을 존중한다 */
}

/*
 * Takes a pointer to an AMD IOMMU entry in the ACPI table and
 * initializes the hardware and our data structures with it.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * init_iommu_from_acpi - IVHD 의 장치 항목을 모두 훑어 자료구조를 채운다
 *
 * @iommu: 이 IVHD 가 서술하는 유닛.
 * @h: 그 IVHD 헤더.
 * @return: 0 성공, 음수면 파싱이나 등록 실패.
 *
 * 표를 세 번 훑는 것 중 두 번째의 핵심이다. 여기서 채워지는 것이 이
 * 드라이버의 조회 경로 전부다: 어느 장치가 이 유닛에 속하는지(rlookup),
 * 어떤 장치가 다른 이름으로 요청을 내는지(alias), 어떤 특별 요구가
 * 있는지(DTE 플래그), 그리고 PCI 가 아닌 장치들의 대응(ioapic/hpet/acpihid).
 *
 * add_early_maps 를 맨 먼저 부르는 순서가 중요하다. 명령줄로 지정된 대응이
 * 목록에 먼저 들어가 있어야, 아래에서 표의 값이 그것을 덮어쓰지 않는다.
 *
 * 항목 종류가 많지만 구조는 셋으로 나뉜다.
 *  - 즉시 적용: DEV_ALL, DEV_SELECT, DEV_ALIAS, DEV_EXT_SELECT.
 *  - 범위의 시작만 기억: *_RANGE_START 계열. 실제 적용은 RANGE_END 에서
 *    한꺼번에 일어난다. 그래서 devid_start/flags/alias 같은 지역 변수가
 *    루프를 가로질러 상태를 나른다 — 표가 그렇게 짝을 이뤄 적혀 있기 때문이다.
 *  - 특별 장치: DEV_SPECIAL, DEV_ACPI_HID. PCI 열거로 발견되지 않는
 *    장치들의 요청자 id 를 목록에 등록한다.
 *
 * 두 특별 장치 경로에서 원 주석이 같은 주의를 준다: add_*_device 가
 * 명령줄 우선 항목 때문에 devid 를 바꿀 수 있으므로, DTE 설정은 반드시
 * 그 뒤에 해야 한다. 순서를 바꾸면 사용자가 지정한 장치가 아니라 표의
 * 장치에 플래그를 얹게 된다.
 *
 * 호출 체인:
 *   init_iommu_one()/init_iommu_all() → [이 함수]
 *     → add_early_maps() → amd_iommu_apply_ivrs_quirks()
 *     → set_dev_entry_from_acpi_range() → add_special_device()
 */
static int __init init_iommu_from_acpi(struct amd_iommu *iommu,
					struct ivhd_header *h)
{
	u8 *p = (u8 *)h;	/* [한국어] 항목 구간을 훑을 커서 */
	u8 *end = p, flags = 0;	/* [한국어] 구간의 끝과, 범위 항목이 나를 플래그 */
	u16 devid = 0, devid_start = 0, devid_to = 0, seg_id;	/* [한국어] 현재 장치, 범위 시작, 별칭 대상, 세그먼트 */
	u32 dev_i, ext_flags = 0;	/* [한국어] 범위 순회 인덱스와 확장 플래그 */
	bool alias = false;	/* [한국어] 지금 모으는 범위가 별칭 범위인가 */
	struct ivhd_entry *e;	/* [한국어] 현재 항목 */
	struct amd_iommu_pci_seg *pci_seg = iommu->pci_seg;	/* [한국어] 이 유닛의 세그먼트 — 조회 표들이 거기 있다 */
	u32 ivhd_size;	/* [한국어] 헤더 크기(타입마다 다르다) */
	int ret;	/* [한국어] 하위 호출의 결과 */


	ret = add_early_maps();	/* [한국어] 명령줄 대응을 먼저 목록에 넣는다 — 그래야 표의 값이 그것을 이기지 못한다 */
	if (ret)	/* [한국어] 등록 실패 */
		return ret;	/* [한국어] 진행할 수 없다 */

	amd_iommu_apply_ivrs_quirks();	/* [한국어] 표에 빠진 항목이 있는 알려진 기종이면 여기서 채워 넣는다 */

	/*
	 * First save the recommended feature enable bits from ACPI
	 */
	iommu->acpi_flags = h->flags;	/* [한국어] (원 주석: ACPI 가 권장하는 기능 활성화 비트를 먼저 저장한다) */

	/*
	 * Done. Now parse the device entries
	 */
	ivhd_size = get_ivhd_header_size(h);	/* [한국어] (원 주석: 이제 장치 항목을 파싱한다) */
	if (!ivhd_size) {	/* [한국어] 모르는 타입 */
		pr_err("Unsupported IVHD type %#x\n", h->type);	/* [한국어] 해석할 수 없다 */
		return -EINVAL;	/* [한국어] 이 IVHD 를 포기한다 */
	}

	p += ivhd_size;	/* [한국어] 헤더를 지나 첫 항목으로 */

	end += h->length;	/* [한국어] 구간의 끝 */


	while (p < end) {	/* [한국어] 항목을 하나씩 */
		e = (struct ivhd_entry *)p;	/* [한국어] 현재 위치를 항목으로 해석 */
		seg_id = pci_seg->id;	/* [한국어] 로그에 쓸 세그먼트 번호 */

		switch (e->type) {	/* [한국어] 항목 종류에 따라 하는 일이 다르다 */
		case IVHD_DEV_ALL:	/* [한국어] 이 유닛이 세그먼트의 모든 장치를 담당한다 */

			DUMP_printk("  DEV_ALL\t\t\tsetting: %#02x\n", e->flags);	/* [한국어] 상세 로그를 켰을 때만 찍힌다 */
			set_dev_entry_from_acpi_range(iommu, 0, pci_seg->last_bdf, e->flags, 0);	/* [한국어] 0 부터 최대 id 까지 전부에 적용 */
			break;	/* [한국어] 다음 항목 */
		case IVHD_DEV_SELECT:	/* [한국어] 장치 하나를 지정 */

			DUMP_printk("  DEV_SELECT\t\t\tdevid: %04x:%02x:%02x.%x flags: %#02x\n",	/* [한국어] 상세 로그 */
				    seg_id, PCI_BUS_NUM(e->devid),
				    PCI_SLOT(e->devid),
				    PCI_FUNC(e->devid),
				    e->flags);

			devid = e->devid;	/* [한국어] 그 장치 */
			set_dev_entry_from_acpi(iommu, devid, e->flags, 0);	/* [한국어] 즉시 적용 */
			break;	/* [한국어] 다음 */
		case IVHD_DEV_SELECT_RANGE_START:	/* [한국어] 범위의 시작. 적용은 RANGE_END 에서 */

			DUMP_printk("  DEV_SELECT_RANGE_START\tdevid: %04x:%02x:%02x.%x flags: %#02x\n",	/* [한국어] 상세 로그 */
				    seg_id, PCI_BUS_NUM(e->devid),
				    PCI_SLOT(e->devid),
				    PCI_FUNC(e->devid),
				    e->flags);

			devid_start = e->devid;	/* [한국어] 시작을 기억해 둔다 */
			flags = e->flags;	/* [한국어] 플래그도 — 끝 항목에는 없기 때문이다 */
			ext_flags = 0;	/* [한국어] 이 종류에는 확장 플래그가 없다 */
			alias = false;	/* [한국어] 별칭 범위가 아니다 */
			break;	/* [한국어] 끝 항목을 기다린다 */
		case IVHD_DEV_ALIAS:	/* [한국어] 이 장치는 다른 이름으로 요청을 낸다 */

			DUMP_printk("  DEV_ALIAS\t\t\tdevid: %04x:%02x:%02x.%x flags: %#02x devid_to: %02x:%02x.%x\n",	/* [한국어] 상세 로그 */
				    seg_id, PCI_BUS_NUM(e->devid),
				    PCI_SLOT(e->devid),
				    PCI_FUNC(e->devid),
				    e->flags,
				    PCI_BUS_NUM(e->ext >> 8),
				    PCI_SLOT(e->ext >> 8),
				    PCI_FUNC(e->ext >> 8));

			devid = e->devid;	/* [한국어] 원래 장치 */
			devid_to = e->ext >> 8;	/* [한국어] 실제로 나타나는 이름 */
			set_dev_entry_from_acpi(iommu, devid   , e->flags, 0);	/* [한국어] 원래 장치에도 */
			set_dev_entry_from_acpi(iommu, devid_to, e->flags, 0);	/* [한국어] 별칭 쪽에도 플래그를 적용한다 — 하드웨어가 보는 것은 별칭 쪽이다 */
			pci_seg->alias_table[devid] = devid_to;	/* [한국어] 조회 표에 대응을 기록 */
			break;	/* [한국어] 다음 */
		case IVHD_DEV_ALIAS_RANGE:	/* [한국어] 범위 전체가 하나의 별칭을 쓴다 — 브리지 뒤 장치들 */

			DUMP_printk("  DEV_ALIAS_RANGE\t\tdevid: %04x:%02x:%02x.%x flags: %#02x devid_to: %04x:%02x:%02x.%x\n",	/* [한국어] 상세 로그 */
				    seg_id, PCI_BUS_NUM(e->devid),
				    PCI_SLOT(e->devid),
				    PCI_FUNC(e->devid),
				    e->flags,
				    seg_id, PCI_BUS_NUM(e->ext >> 8),
				    PCI_SLOT(e->ext >> 8),
				    PCI_FUNC(e->ext >> 8));

			devid_start = e->devid;	/* [한국어] 범위의 시작 */
			flags = e->flags;	/* [한국어] 플래그 */
			devid_to = e->ext >> 8;	/* [한국어] 모두가 쓸 별칭 */
			ext_flags = 0;	/* [한국어] 확장 플래그 없음 */
			alias = true;	/* [한국어] 끝 항목에서 별칭 처리를 하도록 표시 */
			break;	/* [한국어] 끝 항목을 기다린다 */
		case IVHD_DEV_EXT_SELECT:	/* [한국어] 확장 플래그가 딸린 장치 하나 */

			DUMP_printk("  DEV_EXT_SELECT\t\tdevid: %04x:%02x:%02x.%x flags: %#02x ext: %08x\n",	/* [한국어] 상세 로그 */
				    seg_id, PCI_BUS_NUM(e->devid),
				    PCI_SLOT(e->devid),
				    PCI_FUNC(e->devid),
				    e->flags, e->ext);

			devid = e->devid;
			set_dev_entry_from_acpi(iommu, devid, e->flags,	/* [한국어] 기본 플래그와 */
						e->ext);	/* [한국어] 확장 플래그를 함께 */
			break;
		case IVHD_DEV_EXT_SELECT_RANGE:	/* [한국어] 확장 플래그가 딸린 범위 */

			DUMP_printk("  DEV_EXT_SELECT_RANGE\tdevid: %04x:%02x:%02x.%x flags: %#02x ext: %08x\n",	/* [한국어] 상세 로그 */
				    seg_id, PCI_BUS_NUM(e->devid),
				    PCI_SLOT(e->devid),
				    PCI_FUNC(e->devid),
				    e->flags, e->ext);

			devid_start = e->devid;
			flags = e->flags;
			ext_flags = e->ext;	/* [한국어] 확장 플래그도 끝 항목까지 나른다 */
			alias = false;
			break;
		case IVHD_DEV_RANGE_END:	/* [한국어] 범위의 끝. 여기서 실제 적용이 일어난다 */

			DUMP_printk("  DEV_RANGE_END\t\tdevid: %04x:%02x:%02x.%x\n",	/* [한국어] 상세 로그 */
				    seg_id, PCI_BUS_NUM(e->devid),
				    PCI_SLOT(e->devid),
				    PCI_FUNC(e->devid));

			devid = e->devid;
			if (alias) {	/* [한국어] 별칭 범위였다면 */
				for (dev_i = devid_start; dev_i <= devid; ++dev_i)	/* [한국어] 범위의 모든 장치가 */
					pci_seg->alias_table[dev_i] = devid_to;	/* [한국어] 같은 별칭을 쓰게 기록한다 */
				set_dev_entry_from_acpi(iommu, devid_to, flags, ext_flags);	/* [한국어] 별칭 쪽에도 플래그를 적용 — 하드웨어가 보는 이름이다 */
			}
			set_dev_entry_from_acpi_range(iommu, devid_start, devid, flags, ext_flags);	/* [한국어] 시작 항목에서 기억해 둔 값으로 범위 전체에 적용 */
			break;
		case IVHD_DEV_SPECIAL: {	/* [한국어] IOAPIC/HPET 처럼 PCI 열거로 발견되지 않는 장치 */
			u8 handle, type;	/* [한국어] 장치 번호와 종류 */
			const char *var;	/* [한국어] 로그에 쓸 이름 */
			u32 devid;
			int ret;

			handle = e->ext & 0xff;	/* [한국어] 그 장치의 번호 */
			devid = PCI_SEG_DEVID_TO_SBDF(seg_id, (e->ext >> 8));	/* [한국어] 인터럽트가 나타날 요청자 id */
			type   = (e->ext >> 24) & 0xff;	/* [한국어] IOAPIC 인지 HPET 인지 */

			if (type == IVHD_SPECIAL_IOAPIC)	/* [한국어] IOAPIC 이면 */
				var = "IOAPIC";	/* [한국어] 로그용 이름 */
			else if (type == IVHD_SPECIAL_HPET)	/* [한국어] HPET 이면 */
				var = "HPET";	/* [한국어] 로그용 이름 */
			else
				var = "UNKNOWN";	/* [한국어] 모르는 종류도 로그에는 남긴다 */

			DUMP_printk("  DEV_SPECIAL(%s[%d])\t\tdevid: %04x:%02x:%02x.%x, flags: %#02x\n",	/* [한국어] 상세 로그 */
				    var, (int)handle,
				    seg_id, PCI_BUS_NUM(devid),
				    PCI_SLOT(devid),
				    PCI_FUNC(devid),
				    e->flags);

			ret = add_special_device(type, handle, &devid, false);	/* [한국어] 대응을 등록한다. 명령줄 우선 항목이 있으면 devid 가 바뀐다 */
			if (ret)
				return ret;

			/*
			 * add_special_device might update the devid in case a
			 * command-line override is present. So call
			 * set_dev_entry_from_acpi after add_special_device.
			 */
			set_dev_entry_from_acpi(iommu, devid, e->flags, 0);	/* [한국어] (원 주석: add_special_device 가 devid 를 바꿀 수 있으므로 반드시 그 뒤에 부른다) */

			break;
		}
		case IVHD_DEV_ACPI_HID: {	/* [한국어] ACPI HID 로만 식별되는 플랫폼 장치 */
			u32 devid;
			u8 hid[ACPIHID_HID_LEN];	/* [한국어] HID 문자열 버퍼 */
			u8 uid[ACPIHID_UID_LEN];	/* [한국어] UID 문자열 버퍼 */
			int ret;

			if (h->type != 0x40) {	/* [한국어] 이 항목 종류는 40h 타입 IVHD 에만 있을 수 있다 */
				pr_err(FW_BUG "Invalid IVHD device type %#x\n",	/* [한국어] 다른 타입에 나타났다면 표가 잘못된 것 */
				       e->type);
				break;	/* [한국어] 건너뛴다 */
			}

			BUILD_BUG_ON(sizeof(e->ext_hid) != ACPIHID_HID_LEN - 1);	/* [한국어] 표의 HID 필드와 우리 버퍼의 크기가 어긋나면 빌드가 멈춘다 */
			memcpy(hid, &e->ext_hid, ACPIHID_HID_LEN - 1);	/* [한국어] ext 와 hidh 를 묶은 8바이트가 HID 문자열이다 */
			hid[ACPIHID_HID_LEN - 1] = '\0';	/* [한국어] 표에는 종료 문자가 없어 직접 붙인다 */

			if (!(*hid)) {	/* [한국어] 빈 HID */
				pr_err(FW_BUG "Invalid HID.\n");	/* [한국어] 장치를 식별할 수 없다 */
				break;	/* [한국어] 건너뛴다 */
			}

			uid[0] = '\0';	/* [한국어] UID 는 없을 수 있으므로 빈 문자열로 시작 */
			switch (e->uidf) {	/* [한국어] UID 의 형식에 따라 읽는 법이 다르다 */
			case UID_NOT_PRESENT:	/* [한국어] UID 가 없다 */

				if (e->uidl != 0)	/* [한국어] 그런데 길이가 0 이 아니면 */
					pr_warn(FW_BUG "Invalid UID length.\n");	/* [한국어] 표가 모순된 것이라 알린다 */

				break;
			case UID_IS_INTEGER:	/* [한국어] 정수로 주어졌다 */

				sprintf(uid, "%d", e->uid);	/* [한국어] 문자열로 바꿔 담는다 — 목록의 비교가 문자열이기 때문이다 */

				break;
			case UID_IS_CHARACTER:	/* [한국어] 문자열로 주어졌다 */

				memcpy(uid, &e->uid, e->uidl);	/* [한국어] uid 필드의 주소부터 uidl 바이트가 실제 내용이다 */
				uid[e->uidl] = '\0';	/* [한국어] 종료 문자를 붙인다 */

				break;
			default:
				break;
			}

			devid = PCI_SEG_DEVID_TO_SBDF(seg_id, e->devid);	/* [한국어] 이 장치의 요청자 id */
			DUMP_printk("  DEV_ACPI_HID(%s[%s])\t\tdevid: %04x:%02x:%02x.%x, flags: %#02x\n",	/* [한국어] 상세 로그 */
				    hid, uid, seg_id,
				    PCI_BUS_NUM(devid),
				    PCI_SLOT(devid),
				    PCI_FUNC(devid),
				    e->flags);

			flags = e->flags;	/* [한국어] 플래그 */

			ret = add_acpi_hid_device(hid, uid, &devid, false);	/* [한국어] 대응을 등록. 여기서도 devid 가 바뀔 수 있다 */
			if (ret)
				return ret;

			/*
			 * add_special_device might update the devid in case a
			 * command-line override is present. So call
			 * set_dev_entry_from_acpi after add_special_device.
			 */
			set_dev_entry_from_acpi(iommu, devid, e->flags, 0);

			break;
		}
		default:	/* [한국어] 드라이버가 모르는 항목 종류 */
			break;
		}

		p += ivhd_entry_length(p);	/* [한국어] 항목마다 크기가 달라 계산해서 건너뛴다 */
	}

	return 0;	/* [한국어] 이 유닛의 장치 정보가 모두 자료구조에 들어갔다 */
}

/* Allocate PCI segment data structure */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * alloc_pci_segment - PCI 세그먼트 하나의 조회 표를 모두 만든다
 *
 * @id: 세그먼트 번호.
 * @ivrs_base: IVRS 표(크기를 재기 위해 다시 훑는다).
 * @return: 새 세그먼트 구조체, 실패하면 NULL.
 *
 * 원 주석이 순서를 밝힌다: 먼저 표를 훑어 이 세그먼트의 최대 장치 id 를
 * 알아내고, 그 값으로 공유 자료구조의 크기를 정한다. 크기를 먼저 알아야
 * 하는 이유는 표들이 모두 장치 id 를 인덱스로 쓰는 평평한 배열이기
 * 때문이다.
 *
 * 장치 테이블 크기 계산이 눈에 띈다. 항목 수 × 32바이트를 2의 거듭제곱으로
 * 올리고, 최소 4KB 를 보장한다. 거듭제곱이어야 하는 이유는 하드웨어가
 * 크기를 "페이지 수 - 1"의 형태로 인코딩하기 때문이고, 최소 4KB 는 장치가
 * 몇 개뿐인 세그먼트에서도 한 페이지는 있어야 하기 때문이다.
 *
 * 목록에 먼저 넣고 표를 잡는 순서에 유의: 실패 경로가 list_del 로 되돌리며,
 * 그래야 부분적으로 만들어진 세그먼트가 목록에 남지 않는다.
 *
 * 호출 체인:
 *   get_pci_segment() → [이 함수] → find_last_devid_acpi()
 *     → alloc_dev_table() → alloc_alias_table() → alloc_rlookup_table()
 */
static struct amd_iommu_pci_seg *__init alloc_pci_segment(u16 id,
					  struct acpi_table_header *ivrs_base)
{
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 만들 세그먼트 */
	int last_bdf;	/* [한국어] 이 세그먼트의 최대 장치 id */

	/*
	 * First parse ACPI tables to find the largest Bus/Dev/Func we need to
	 * handle in this PCI segment. Upon this information the shared data
	 * structures for the PCI segments in the system will be allocated.
	 */
	last_bdf = find_last_devid_acpi(ivrs_base, id);	/* [한국어] (원 주석: 먼저 표를 훑어 다뤄야 할 최대 BDF 를 찾는다) */
	if (last_bdf < 0)	/* [한국어] 파싱 실패 */
		return NULL;	/* [한국어] 표가 이상하면 이 세그먼트를 만들 수 없다 */

	pci_seg = kzalloc_obj(struct amd_iommu_pci_seg);	/* [한국어] 세그먼트 구조체 */
	if (pci_seg == NULL)	/* [한국어] 메모리 부족 */
		return NULL;	/* [한국어] 실패 */

	pci_seg->last_bdf = last_bdf;	/* [한국어] 표들의 크기를 정하는 근거 */
	DUMP_printk("PCI segment : 0x%0x, last bdf : 0x%04x\n", id, last_bdf);	/* [한국어] 상세 로그 */
	pci_seg->dev_table_size =	/* [한국어] 장치 테이블의 크기 */
		max(roundup_pow_of_two((last_bdf + 1) * DEV_TABLE_ENTRY_SIZE),	/* [한국어] 2의 거듭제곱이어야 한다 — 하드웨어가 "페이지 수 - 1"로 인코딩한다 */
		    SZ_4K);	/* [한국어] 장치가 몇 개뿐이어도 최소 한 페이지 */

	pci_seg->id = id;	/* [한국어] 세그먼트 번호 */
	init_llist_head(&pci_seg->dev_data_list);	/* [한국어] 장치 상태 목록. 락 없이 밀어 넣을 수 있는 형태다 */
	INIT_LIST_HEAD(&pci_seg->unity_map);	/* [한국어] 펌웨어가 요구할 항등 매핑 목록 */
	list_add_tail(&pci_seg->list, &amd_iommu_pci_seg_list);	/* [한국어] 전역 목록에 먼저 넣는다 — 실패 경로가 list_del 로 되돌린다 */

	if (alloc_dev_table(pci_seg))	/* [한국어] 하드웨어가 읽을 장치 테이블 */
		goto err_free_pci_seg;	/* [한국어] 실패하면 구조체만 되돌린다 */
	if (alloc_alias_table(pci_seg))	/* [한국어] 장치 id → 실제 요청자 id */
		goto err_free_dev_table;	/* [한국어] 장치 테이블까지 되돌린다 */
	if (alloc_rlookup_table(pci_seg))	/* [한국어] 장치 id → 담당 유닛 */
		goto err_free_alias_table;	/* [한국어] 별칭 표까지 되돌린다 */

	return pci_seg;	/* [한국어] 세 표가 모두 준비됐다 */

err_free_alias_table:
	free_alias_table(pci_seg);	/* [한국어] 계단식 되감기 */
err_free_dev_table:
	free_dev_table(pci_seg);	/* [한국어] 장치 테이블 */
err_free_pci_seg:
	list_del(&pci_seg->list);	/* [한국어] 목록에서 빼고 */
	kfree(pci_seg);	/* [한국어] 구조체를 놓는다 */
	return NULL;	/* [한국어] 이 세그먼트는 쓸 수 없다 */
}

/*
 * [한국어]
 * get_pci_segment - 그 번호의 세그먼트를 찾거나 새로 만든다
 *
 * @id: 세그먼트 번호.
 * @ivrs_base: IVRS 표.
 * @return: 세그먼트 구조체, 실패하면 NULL.
 *
 * 한 세그먼트에 유닛이 여럿일 수 있고 각 유닛의 IVHD 가 따로 나타나므로,
 * 두 번째 유닛부터는 이미 만들어진 세그먼트를 찾아 써야 한다. 그 판단을
 * 여기 모아 두어 호출부가 신경 쓰지 않게 한다.
 *
 * 호출 체인:
 *   init_iommu_one() → [이 함수] → alloc_pci_segment()
 */
static struct amd_iommu_pci_seg *__init get_pci_segment(u16 id,
					struct acpi_table_header *ivrs_base)
{
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 목록 커서 */

	for_each_pci_segment(pci_seg) {	/* [한국어] 이미 만들어진 세그먼트를 찾는다 */
		if (pci_seg->id == id)	/* [한국어] 번호가 맞으면 */
			return pci_seg;	/* [한국어] 그것을 쓴다 — 한 세그먼트에 유닛이 여럿일 수 있다 */
	}

	return alloc_pci_segment(id, ivrs_base);	/* [한국어] 없으면 새로 만든다 */
}

/*
 * [한국어]
 * free_pci_segments - 모든 세그먼트와 그 조회 표를 놓는다
 *
 * 해제 순서가 alloc 의 역순이 아니라는 점이 눈에 띈다. irq_lookup 이 가장
 * 먼저인데, 그것만 다른 경로(재매핑 초기화)에서 잡히기 때문이다. 나머지
 * 셋은 서로 의존하지 않아 순서가 자유롭다.
 *
 * _safe 순회를 쓰는 이유: 목록에서 빼면서 훑는다.
 *
 * 호출 체인:
 *   free_iommu_resources() → [이 함수]
 */
static void __init free_pci_segments(void)
{
	struct amd_iommu_pci_seg *pci_seg, *next;	/* [한국어] 목록에서 빼며 훑으므로 다음을 미리 잡는다 */

	for_each_pci_segment_safe(pci_seg, next) {	/* [한국어] 모든 세그먼트 */
		list_del(&pci_seg->list);	/* [한국어] 먼저 목록에서 뺀다 */
		free_irq_lookup_table(pci_seg);	/* [한국어] 재매핑 초기화가 따로 잡은 것이라 먼저 놓는다 */
		free_rlookup_table(pci_seg);	/* [한국어] 담당 유닛 표 */
		free_alias_table(pci_seg);	/* [한국어] 별칭 표 */
		free_dev_table(pci_seg);	/* [한국어] 장치 테이블 */
		kfree(pci_seg);	/* [한국어] 구조체 */
	}
}

/*
 * [한국어]
 * free_sysfs - 코어 IOMMU 계층과 sysfs 에서 이 유닛을 뗀다
 *
 * @iommu: 대상 유닛.
 *
 * 등록의 역순으로 뗀다: 먼저 코어에서 unregister 하고 그다음 sysfs 를
 * 지운다. 반대로 하면 코어가 이미 사라진 sysfs 항목을 참조할 수 있다.
 *
 * iommu.dev 검사가 등록 여부의 표식이다 — 초기화가 그 단계까지 가지
 * 못했으면 뗄 것도 없다.
 */
static void __init free_sysfs(struct amd_iommu *iommu)
{
	if (iommu->iommu.dev) {	/* [한국어] 등록까지 갔는지의 표식 */
		iommu_device_unregister(&iommu->iommu);	/* [한국어] 코어에서 먼저 뗀다 */
		iommu_device_sysfs_remove(&iommu->iommu);	/* [한국어] 그다음 sysfs — 반대로 하면 코어가 사라진 항목을 참조한다 */
	}
}

/*
 * [한국어]
 * free_iommu_one - 유닛 하나가 잡은 자원을 모두 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * MMIO 매핑을 늦게 푸는 것이 중요하다. 그 앞의 해제들이 하드웨어를 건드릴
 * 수 있는데, 매핑이 없으면 그것을 할 수 없다.
 *
 * 각 해제 함수가 "잡히지 않았다"를 스스로 판별하므로, 초기화가 어디까지
 * 갔든 이 하나로 정리된다.
 *
 * 호출 체인:
 *   free_iommu_all()/init_iommu_one() 실패 경로 → [이 함수]
 */
static void __init free_iommu_one(struct amd_iommu *iommu)
{
	free_sysfs(iommu);	/* [한국어] 코어 등록 해제 */
	free_iommu_buffers(iommu);	/* [한국어] 명령/이벤트/완료 대기 버퍼 */
	amd_iommu_free_ppr_log(iommu);	/* [한국어] PPR 로그 */
	free_ga_log(iommu);	/* [한국어] GA 로그와 꼬리 포인터 */
	iommu_unmap_mmio_space(iommu);	/* [한국어] MMIO 는 마지막에 — 앞의 해제들이 하드웨어를 건드릴 수 있다 */
	amd_iommu_iopf_uninit(iommu);	/* [한국어] 페이지 폴트 처리 큐 */
}

/*
 * [한국어]
 * free_iommu_all - 모든 유닛을 목록에서 빼고 자원을 놓는다
 *
 * 초기화 실패 정리 경로다. 목록에서 먼저 빼는 이유: 다른 코드가 그 사이에
 * 목록을 훑으며 해제된 유닛에 닿는 것을 막는다.
 *
 * 호출 체인:
 *   free_iommu_resources() → [이 함수] → free_iommu_one()
 */
static void __init free_iommu_all(void)
{
	struct amd_iommu *iommu, *next;	/* [한국어] 목록에서 빼며 훑는다 */

	for_each_iommu_safe(iommu, next) {	/* [한국어] 모든 유닛 */
		list_del(&iommu->list);	/* [한국어] 먼저 목록에서 뺀다 — 다른 코드가 해제된 유닛에 닿지 않게 */
		free_iommu_one(iommu);	/* [한국어] 자원 해제 */
		kfree(iommu);	/* [한국어] 구조체 */
	}
}

/*
 * Family15h Model 10h-1fh erratum 746 (IOMMU Logging May Stall Translations)
 * Workaround:
 *     BIOS should disable L2B micellaneous clock gating by setting
 *     L2_L2B_CK_GATE_CONTROL[CKGateL2BMiscDisable](D0F2xF4_x90[2]) = 1b
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_erratum_746_workaround - 로그 기록이 변환을 멈추는 결함을 우회한다
 *
 * @iommu: 대상 유닛.
 *
 * 원 주석이 증상과 처방을 밝힌다: Family15h Model 10h-1fh 에서 IOMMU 가
 * 로그를 쓰는 동안 변환이 멈출 수 있고, L2B 클록 게이팅을 끄면 해결된다.
 *
 * BIOS 가 해야 할 일이지만 하지 않는 기계가 있어 커널이 대신 한다. 모델
 * 번호로 대상을 좁히는 것이 유일한 판별 방법이다 — 이 결함을 알리는 능력
 * 비트가 없다.
 *
 * 이미 설정되어 있으면 아무것도 하지 않는다(BIOS 가 제대로 한 기계).
 *
 * L2 간접 레지스터를 직접 다루는 것에 유의: iommu_write_l2 를 쓰지 않고
 * 설정 공간을 직접 건드리는데, 이 시점에는 아직 그 도우미가 쓰는 상태가
 * 준비되지 않았을 수 있다.
 *
 * 호출 체인:
 *   iommu_init_pci() → [이 함수]
 */
static void amd_iommu_erratum_746_workaround(struct amd_iommu *iommu)
{
	u32 value;	/* [한국어] 레지스터 값 */

	if ((boot_cpu_data.x86 != 0x15) ||	/* [한국어] Family 15h 이고 */
	    (boot_cpu_data.x86_model < 0x10) ||	/* [한국어] Model 10h 이상 */
	    (boot_cpu_data.x86_model > 0x1f))	/* [한국어] 1fh 이하인가 — 결함을 알리는 능력 비트가 없어 모델로 판별한다 */
		return;	/* [한국어] 해당 없음 */

	pci_write_config_dword(iommu->dev, 0xf0, 0x90);	/* [한국어] NB 간접 레지스터 0x90 을 읽기 위해 창을 연다 */
	pci_read_config_dword(iommu->dev, 0xf4, &value);	/* [한국어] 현재 값 */

	if (value & BIT(2))	/* [한국어] BIOS 가 이미 설정했다면 */
		return;	/* [한국어] 할 일이 없다 */

	/* Select NB indirect register 0x90 and enable writing */
	pci_write_config_dword(iommu->dev, 0xf0, 0x90 | (1 << 8));	/* [한국어] (원 주석: 0x90 을 선택하고 쓰기를 허용한다) */

	pci_write_config_dword(iommu->dev, 0xf4, value | 0x4);	/* [한국어] L2B 클록 게이팅을 끈다 — 그래야 로그 기록이 변환을 멈추지 않는다 */
	pci_info(iommu->dev, "Applying erratum 746 workaround\n");	/* [한국어] BIOS 가 해야 할 일을 커널이 대신했음을 알린다 */

	/* Clear the enable writing bit */
	pci_write_config_dword(iommu->dev, 0xf0, 0x90);	/* [한국어] (원 주석: 쓰기 허용 비트를 지운다) 창을 읽기 모드로 되돌린다 */
}

/*
 * Family15h Model 30h-3fh (IOMMU Mishandles ATS Write Permission)
 * Workaround:
 *     BIOS should enable ATS write permission check by setting
 *     L2_DEBUG_3[AtsIgnoreIWDis](D0F2xF4_x47[0]) = 1b
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_ats_write_check_workaround - ATS 쓰기 권한 검사를 강제로 켠다
 *
 * @iommu: 대상 유닛.
 *
 * 원 주석대로 Family15h Model 30h-3fh 는 ATS 요청의 쓰기 권한을 제대로
 * 검사하지 않는다. 그러면 읽기 전용으로 매핑한 페이지에 장치가 쓸 수 있어,
 * 격리가 부분적으로 깨진다.
 *
 * L2_DEBUG_3 의 비트 하나를 세워 검사를 강제한다. 앞의 746 우회와 달리
 * iommu_read_l2/write_l2 도우미를 쓰는데, 이 함수는 유닛 초기화가 더
 * 진행된 뒤에 불리기 때문이다.
 *
 * 호출 체인:
 *   iommu_init_pci() → [이 함수] → iommu_read_l2()/iommu_write_l2()
 */
static void amd_iommu_ats_write_check_workaround(struct amd_iommu *iommu)
{
	u32 value;	/* [한국어] 레지스터 값 */

	if ((boot_cpu_data.x86 != 0x15) ||	/* [한국어] Family 15h 이고 */
	    (boot_cpu_data.x86_model < 0x30) ||	/* [한국어] Model 30h 이상 */
	    (boot_cpu_data.x86_model > 0x3f))	/* [한국어] 3fh 이하인가 */
		return;	/* [한국어] 해당 없음 */

	/* Test L2_DEBUG_3[AtsIgnoreIWDis] == 1 */
	value = iommu_read_l2(iommu, 0x47);	/* [한국어] (원 주석: L2_DEBUG_3[AtsIgnoreIWDis] 를 확인한다) */

	if (value & BIT(0))	/* [한국어] 이미 설정되어 있으면 */
		return;	/* [한국어] 할 일이 없다 */

	/* Set L2_DEBUG_3[AtsIgnoreIWDis] = 1 */
	iommu_write_l2(iommu, 0x47, value | BIT(0));	/* [한국어] (원 주석: 그 비트를 세운다) 켜지 않으면 읽기 전용 페이지에 장치가 쓸 수 있다 */

	pci_info(iommu->dev, "Applying ATS write check workaround\n");	/* [한국어] 우회가 적용됐음을 알린다 */
}

/*
 * This function glues the initialization function for one IOMMU
 * together and also allocates the command buffer and programs the
 * hardware. It does NOT enable the IOMMU. This is done afterwards.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * init_iommu_one - 유닛 하나를 발견해 쓸 수 있는 상태까지 갖춘다
 *
 * @iommu: 채울 유닛 구조체.
 * @h: 그 유닛의 IVHD 헤더.
 * @ivrs_base: IVRS 표.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 범위를 분명히 한다: 하드웨어를 프로그램하지만 켜지는 않는다.
 * 켜는 것은 나중에 별도 단계로 이루어진다 — 실패할 수 있는 일을 모두 여기서
 * 끝내야 켜는 단계가 되돌릴 필요 없이 진행된다.
 *
 * 하는 일의 순서:
 *  1) 세그먼트를 찾거나 만들고 자신을 등록한다.
 *  2) IVHD 에서 기본 정보(devid, cap_ptr, MMIO 주소)를 복사한다.
 *  3) 타입별로 능력을 해석한다. 이 부분이 가장 미묘한데, 타입마다 능력이
 *     어디에 적혀 있는지가 다르고 그에 따라 인터럽트 모드가 아래로
 *     내려가기도 한다.
 *  4) MMIO 를 매핑하고 IVHD 의 장치 항목을 파싱한다.
 *
 * 버퍼 할당과 물려받은 상태 확인은 여기가 아니라 init_iommu_one_late 가
 * 맡는다. 그 둘을 나눈 이유: 이 함수는 IVHD 를 훑는 중에 유닛마다 불리는데,
 * 늦은 단계의 일은 모든 유닛의 능력을 다 본 뒤에 해야 하기 때문이다.
 *
 * MMIO 끝 주소를 능력에 따라 달리 잡는 이유: 성능 카운터 영역까지 있는
 * 유닛과 없는 유닛이 있어, 없는데 끝까지 매핑하면 존재하지 않는 주소를
 * 예약하게 된다.
 *
 * 인터럽트 모드가 이 함수에서 아래로만 내려가는 것도 눈여겨볼 만하다.
 * 전역 값이라 한 유닛이라도 GA 를 지원하지 않으면 전체가 내려간다 —
 * 유닛마다 다른 모드를 쓸 수 없기 때문이다.
 *
 * 호출 체인:
 *   init_iommu_all() → [이 함수] → get_pci_segment()
 *     → iommu_map_mmio_space() → init_iommu_from_acpi()
 */
static int __init init_iommu_one(struct amd_iommu *iommu, struct ivhd_header *h,
				 struct acpi_table_header *ivrs_base)
{
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 이 유닛이 속할 세그먼트 */

	pci_seg = get_pci_segment(h->pci_seg, ivrs_base);	/* [한국어] 없으면 만들고, 있으면 그것을 쓴다 */
	if (pci_seg == NULL)	/* [한국어] 세그먼트를 만들 수 없다 */
		return -ENOMEM;	/* [한국어] 이 유닛도 쓸 수 없다 */
	iommu->pci_seg = pci_seg;	/* [한국어] 조회 표들이 거기 있다 */

	raw_spin_lock_init(&iommu->lock);	/* [한국어] 명령 버퍼와 레지스터를 지킬 락 */
	iommu->cmd_sem_val = 0;	/* [한국어] 완료 대기 값의 시작. 매번 증가시켜 옛 완료와 구별한다 */

	/* Add IOMMU to internal data structures */
	list_add_tail(&iommu->list, &amd_iommu_list);	/* [한국어] (원 주석: 내부 자료구조에 유닛을 추가한다) */
	iommu->index = amd_iommus_present++;	/* [한국어] 배열에서의 위치이자 로그의 유닛 번호 */

	if (unlikely(iommu->index >= MAX_IOMMUS)) {	/* [한국어] 드라이버가 다룰 수 있는 상한 */
		WARN(1, "System has more IOMMUs than supported by this driver\n");	/* [한국어] 상한을 넘는 기계는 지원 범위 밖이다 */
		return -ENOSYS;	/* [한국어] 이 유닛은 쓸 수 없다 */
	}

	/*
	 * Copy data from ACPI table entry to the iommu struct
	 */
	iommu->devid   = h->devid;	/* [한국어] (원 주석: ACPI 표의 값을 구조체로 복사한다) */
	iommu->cap_ptr = h->cap_ptr;	/* [한국어] 설정 공간에서 능력 구조의 위치 */
	iommu->mmio_phys = h->mmio_phys;	/* [한국어] MMIO 영역의 시작 */

	switch (h->type) {	/* [한국어] 타입마다 능력이 어디에 적혀 있는지가 다르다 */
	case 0x10:	/* [한국어] 가장 오래된 형식 — efr_attr 만 있다 */
		/* Check if IVHD EFR contains proper max banks/counters */
		if ((h->efr_attr != 0) &&	/* [한국어] (원 주석: IVHD EFR 에 뱅크/카운터 정보가 제대로 들어 있는지) */
		    ((h->efr_attr & (0xF << 13)) != 0) &&	/* [한국어] 성능 카운터 뱅크 수가 있고 */
		    ((h->efr_attr & (0x3F << 17)) != 0))	/* [한국어] 카운터 수도 있으면 */
			iommu->mmio_phys_end = MMIO_REG_END_OFFSET;	/* [한국어] 성능 카운터 영역까지 매핑한다 */
		else
			iommu->mmio_phys_end = MMIO_CNTR_CONF_OFFSET;	/* [한국어] 없으면 그 앞까지만 — 없는 주소를 예약하면 안 된다 */

		/* GAM requires GA mode. */
		if ((h->efr_attr & (0x1 << IOMMU_FEAT_GASUP_SHIFT)) == 0)	/* [한국어] (원 주석: GAM 은 GA 모드를 전제로 한다) */
			amd_iommu_guest_ir = AMD_IOMMU_GUEST_IR_LEGACY;	/* [한국어] GA 가 없으면 게스트 직접 전달을 포기하고 32비트 형식으로 내려간다 */
		break;
	case 0x11:	/* [한국어] EFR 사본이 추가된 형식 */
	case 0x40:	/* [한국어] 같은 크기의 최신 형식 */
		if (h->efr_reg & (1 << 9))	/* [한국어] 성능 카운터 지원 비트 */
			iommu->mmio_phys_end = MMIO_REG_END_OFFSET;
		else
			iommu->mmio_phys_end = MMIO_CNTR_CONF_OFFSET;

		/* XT and GAM require GA mode. */
		if ((h->efr_reg & (0x1 << IOMMU_EFR_GASUP_SHIFT)) == 0) {	/* [한국어] (원 주석: XT 와 GAM 모두 GA 모드를 전제로 한다) */
			amd_iommu_guest_ir = AMD_IOMMU_GUEST_IR_LEGACY;	/* [한국어] GA 가 없으면 아래 XT 판단도 무의미하다 */
			break;	/* [한국어] 더 볼 것이 없다 */
		}

		if (h->efr_reg & BIT(IOMMU_EFR_XTSUP_SHIFT))	/* [한국어] x2APIC 목적지를 지원하는가 */
			amd_iommu_xt_mode = IRQ_REMAP_X2APIC_MODE;	/* [한국어] 32비트 APIC id 를 쓸 수 있다 */

		if (h->efr_attr & BIT(IOMMU_IVHD_ATTR_HATDIS_SHIFT)) {	/* [한국어] 펌웨어가 호스트 주소 변환을 쓰지 말라고 했는가 */
			pr_warn_once("Host Address Translation is not supported.\n");	/* [한국어] v1 페이지 테이블을 쓸 수 없다는 뜻이다 */
			amd_iommu_hatdis = true;	/* [한국어] 도메인 형식 결정이 이 값을 본다 */
		}

		early_iommu_features_init(iommu, h);	/* [한국어] 표의 EFR 사본을 구조체로 옮긴다 — MMIO 매핑 전에도 능력을 알 수 있게 */

		break;
	default:
		return -EINVAL;	/* [한국어] 드라이버가 모르는 IVHD 타입 */
	}

	iommu->mmio_base = iommu_map_mmio_space(iommu->mmio_phys,	/* [한국어] 능력에 따라 정한 끝 주소까지 매핑한다 */
						iommu->mmio_phys_end);	/* [한국어] 성능 카운터 영역이 없는 유닛은 그 앞까지만 */
	if (!iommu->mmio_base)	/* [한국어] 매핑 실패 */
		return -ENOMEM;	/* [한국어] 레지스터를 읽을 수 없으면 아무것도 할 수 없다 */

	return init_iommu_from_acpi(iommu, h);	/* [한국어] IVHD 의 장치 항목을 훑어 조회 표들을 채운다 */
}

/*
 * [한국어]
 * init_iommu_one_late - 모든 유닛의 능력을 확인한 뒤 마무리한다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, 음수면 실패.
 *
 * init_iommu_one 과 나뉜 이유가 이 함수의 존재 근거다. 여기서 하는 일들은
 * 전역 결정(인터럽트 모드, 물려받기 여부)에 의존하는데, 그 결정은 모든
 * 유닛의 IVHD 를 다 본 뒤에야 확정된다.
 *
 * 하는 일:
 *  - 명령/이벤트/완료 대기 버퍼를 잡는다(또는 kdump 면 물려받는다).
 *  - 하드웨어에게 지금 변환이 켜져 있는지 물어 물려받음 표시를 세운다.
 *  - kdump 가 아닌데 켜져 있으면 끄고 시작한다. 정상 부팅에서는 있을 수
 *    없는 조합이라, 옛 설정을 믿을 수 없다.
 *  - 재매핑을 쓰기로 했으면 이 유닛의 인터럽트 도메인을 만든다.
 *
 * amd_iommu_pre_enabled 를 AND 하듯 좁히는 것에 유의: 전역 물려받기는 모든
 * 유닛이 켜져 있을 때만 참이어야 한다. 일부만 물려받으면 어떤 장치는 옛
 * 매핑을, 어떤 장치는 빈 표를 보게 된다.
 *
 * 마지막 한 줄이 이 함수에서 가장 눈에 띈다. 원 주석이 "IVRS 표가 그렇게
 * 말하지만 그것은 거짓"이라고 한다 — 표는 IOMMU 자신도 담당 장치로 적지만,
 * IOMMU 가 자기 DMA(표 읽기)를 변환하게 두면 표를 읽으려다 다시 표를 읽는
 * 순환이 생긴다.
 *
 * 호출 체인:
 *   init_iommu_all() (2단계) → [이 함수] → alloc_iommu_buffers()
 *     → init_translation_status() → amd_iommu_create_irq_domain()
 */
static int __init init_iommu_one_late(struct amd_iommu *iommu)
{
	int ret;	/* [한국어] 하위 호출의 결과 */

	ret = alloc_iommu_buffers(iommu);	/* [한국어] 명령/이벤트/완료 대기 버퍼. kdump 면 물려받는다 */
	if (ret)	/* [한국어] 할당 실패 */
		return ret;	/* [한국어] 이 유닛은 쓸 수 없다 */

	iommu->int_enabled = false;	/* [한국어] 인터럽트는 아직 등록하지 않았다 */

	init_translation_status(iommu);	/* [한국어] 하드웨어에게 지금 변환이 켜져 있는지 묻는다 */
	if (translation_pre_enabled(iommu) && !is_kdump_kernel()) {	/* [한국어] 물려받았는데 kdump 가 아니다 — 정상 부팅에서는 있을 수 없는 조합 */
		iommu_disable(iommu);	/* [한국어] 옛 설정을 믿을 수 없으므로 끄고 시작한다 */
		clear_translation_pre_enabled(iommu);	/* [한국어] 물려받음 표시도 지운다 */
		pr_warn("Translation was enabled for IOMMU:%d but we are not in kdump mode\n",	/* [한국어] 비정상 상황이므로 알린다 */
			iommu->index);
	}
	if (amd_iommu_pre_enabled)	/* [한국어] 전역 물려받기 판단 */
		amd_iommu_pre_enabled = translation_pre_enabled(iommu);	/* [한국어] 하나라도 아니면 거짓으로 좁힌다 — 일부만 물려받으면 상태를 알 수 없다 */

	if (amd_iommu_irq_remap) {	/* [한국어] 재매핑을 쓰기로 했으면 */
		ret = amd_iommu_create_irq_domain(iommu);	/* [한국어] 이 유닛의 인터럽트 도메인을 만든다 */
		if (ret)
			return ret;	/* [한국어] 실패하면 재매핑을 쓸 수 없다 */
	}

	/*
	 * Make sure IOMMU is not considered to translate itself. The IVRS
	 * table tells us so, but this is a lie!
	 */
	iommu->pci_seg->rlookup_table[iommu->devid] = NULL;	/* [한국어] (원 주석: IVRS 는 IOMMU 자신도 담당 장치로 적지만 거짓이다) — 자기 표 읽기를 변환하면 순환이 된다 */

	return 0;	/* [한국어] 이 유닛이 켜질 준비를 마쳤다 */
}

/**
 * get_highest_supported_ivhd_type - Look up the appropriate IVHD type
 * @ivrs: Pointer to the IVRS header
 *
 * This function search through all IVDB of the maximum supported IVHD
 */
/*
 * [한국어]
 * (위 영어 kernel-doc 에 이어)
 * get_highest_supported_ivhd_type - 이 표에서 쓸 IVHD 타입을 고른다
 *
 * @ivrs: IVRS 표 헤더.
 * @return: 고른 타입.
 *
 * 하나의 IVRS 표에 같은 유닛에 대한 IVHD 가 여러 타입으로 함께 들어 있다.
 * 하위 호환을 위해 펌웨어가 옛 형식과 새 형식을 나란히 적어 두기 때문이다.
 * 그중 하나를 골라 일관되게 써야 하며, 새 형식일수록 정보가 많으므로
 * 가장 높은 것을 고른다.
 *
 * 첫 IVHD 의 devid 를 기준으로 삼는 것이 이 함수의 요령이다. 같은 유닛의
 * 항목들만 비교해야 하는데, 서로 다른 유닛의 항목이 섞여 있어도 devid 로
 * 구별되기 때문이다.
 *
 * 순회 조건에 타입 상한이 들어 있는 이유: 드라이버가 모르는 더 높은 타입이
 * 나타나면 그 지점에서 멈춘다 — 해석할 수 없는 형식을 고르면 안 된다.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → [이 함수]
 */
static u8 get_highest_supported_ivhd_type(struct acpi_table_header *ivrs)
{
	u8 *base = (u8 *)ivrs;	/* [한국어] 표의 시작. 순회 종료 판단에 쓴다 */
	struct ivhd_header *ivhd = (struct ivhd_header *)	/* [한국어] 첫 IVHD */
					(base + IVRS_HEADER_LENGTH);	/* [한국어] 고정 헤더 뒤부터 */
	u8 last_type = ivhd->type;	/* [한국어] 지금까지 본 가장 높은 타입 */
	u16 devid = ivhd->devid;	/* [한국어] 기준이 될 유닛. 다른 유닛의 항목과 섞이지 않게 한다 */

	while (((u8 *)ivhd - base < ivrs->length) &&	/* [한국어] 표의 끝에 닿기 전이고 */
	       (ivhd->type <= ACPI_IVHD_TYPE_MAX_SUPPORTED)) {	/* [한국어] 드라이버가 아는 타입인 동안 — 모르는 형식을 고르면 안 된다 */
		u8 *p = (u8 *) ivhd;	/* [한국어] 다음으로 건너뛸 기준 */

		if (ivhd->devid == devid)	/* [한국어] 같은 유닛의 항목이면 */
			last_type = ivhd->type;	/* [한국어] 더 높은 타입으로 갱신한다. 새 형식일수록 정보가 많다 */
		ivhd = (struct ivhd_header *)(p + ivhd->length);	/* [한국어] 다음 IVHD 로 */
	}

	return last_type;	/* [한국어] 이 값이 이후 모든 파싱의 기준이 된다 */
}

/*
 * Iterates over all IOMMU entries in the ACPI table, allocates the
 * IOMMU structure and initializes it with init_iommu_one()
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * init_iommu_all - 표의 모든 IOMMU 를 발견해 초기화한다
 *
 * @table: IVRS 표.
 * @return: 0 성공, 음수면 어느 유닛에서 실패.
 *
 * 세 단계로 나뉘어 있고, 그 나눔이 이 함수의 설계다.
 *
 * 1단계: IVHD 를 훑으며 유닛 구조체를 만들고 기본 정보를 채운다. 이 시점에는
 *   각 유닛이 자기 능력만 안다.
 * 2단계: 모든 유닛의 능력을 AND 해 전역 EFR 을 만든다. 이제서야 "이
 *   시스템이 무엇을 할 수 있는가"가 확정된다.
 * 3단계: 그 전역 결정을 전제로 각 유닛을 마무리한다.
 *
 * 왜 이렇게 나눠야 하는가: 인터럽트 모드나 물려받기 여부 같은 결정은 모든
 * 유닛을 본 뒤에야 내릴 수 있는데, 버퍼 할당과 도메인 생성은 그 결정에
 * 의존한다. 한 번에 처리하면 첫 유닛이 아직 모르는 사실을 전제로 자원을
 * 잡게 된다.
 *
 * target_ivhd_type 만 처리하는 이유: 같은 유닛에 대해 여러 타입의 IVHD 가
 * 있고, 그중 하나만 골라 써야 유닛이 중복 생성되지 않는다.
 *
 * 실패 시 되감지 않는 것에 유의: 호출자가 free_iommu_resources 로 부분적으로
 * 만들어진 것까지 함께 정리한다.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → [이 함수] → init_iommu_one()
 *     → get_global_efr() → init_iommu_one_late()
 */
static int __init init_iommu_all(struct acpi_table_header *table)
{
	u8 *p = (u8 *)table, *end = (u8 *)table;	/* [한국어] 표를 훑을 커서와 끝 */
	struct ivhd_header *h;	/* [한국어] 현재 IVHD */
	struct amd_iommu *iommu;	/* [한국어] 만들 유닛 */
	int ret;	/* [한국어] 하위 호출의 결과 */

	end += table->length;	/* [한국어] 표의 끝 */
	p += IVRS_HEADER_LENGTH;	/* [한국어] 첫 IVHD 로 */

	/* Phase 1: Process all IVHD blocks */
	while (p < end) {	/* [한국어] (원 주석: 1단계 — 모든 IVHD 블록을 처리한다) */
		h = (struct ivhd_header *)p;	/* [한국어] 현재 위치를 IVHD 로 */
		if (*p == amd_iommu_target_ivhd_type) {	/* [한국어] 고른 타입만 처리 — 아니면 유닛이 중복 생성된다 */

			DUMP_printk("device: %04x:%02x:%02x.%01x cap: %04x "	/* [한국어] 상세 로그: 유닛의 위치와 능력 */
				    "flags: %01x info %04x\n",
				    h->pci_seg, PCI_BUS_NUM(h->devid),
				    PCI_SLOT(h->devid), PCI_FUNC(h->devid),
				    h->cap_ptr, h->flags, h->info);
			DUMP_printk("       mmio-addr: %016llx\n",	/* [한국어] MMIO 주소 */
				    h->mmio_phys);

			iommu = kzalloc_obj(struct amd_iommu);	/* [한국어] 유닛 구조체 */
			if (iommu == NULL)	/* [한국어] 메모리 부족 */
				return -ENOMEM;	/* [한국어] 호출자가 부분적으로 만들어진 것까지 정리한다 */

			ret = init_iommu_one(iommu, h, table);	/* [한국어] 세그먼트를 잇고 MMIO 를 매핑하고 장치 항목을 파싱한다 */
			if (ret)
				return ret;	/* [한국어] 실패하면 그대로 보고 */
		}
		p += h->length;	/* [한국어] 다음 IVHD 로 */

	}
	WARN_ON(p != end);	/* [한국어] 순회가 끝을 정확히 짚지 못하면 표를 잘못 읽은 것이다 */

	/* Phase 2 : Early feature support check */
	get_global_efr();	/* [한국어] (원 주석: 2단계 — 이른 기능 지원 확인) 모든 유닛의 공통분을 확정한다 */

	/* Phase 3 : Enabling IOMMU features */
	for_each_iommu(iommu) {	/* [한국어] (원 주석: 3단계 — IOMMU 기능 활성화) */
		ret = init_iommu_one_late(iommu);	/* [한국어] 전역 결정을 전제로 각 유닛을 마무리한다 */
		if (ret)
			return ret;
	}

	return 0;	/* [한국어] 모든 유닛이 켜질 준비를 마쳤다 */
}

/*
 * [한국어]
 * init_iommu_perf_ctr - 성능 카운터의 규모를 읽어 둔다
 *
 * @iommu: 대상 유닛.
 *
 * 뱅크 수와 뱅크당 카운터 수를 읽는다. perf 계층이 이 값으로 "이 유닛에서
 * 동시에 몇 개를 셀 수 있는가"를 판단한다.
 *
 * 전역 amd_iommu_pc_present 를 세우는 것이 눈에 띈다 — 유닛별 능력을 읽는
 * 함수인데 전역 플래그를 켠다. 성능 카운터 인터페이스가 유닛 단위가 아니라
 * 시스템 단위로 노출되기 때문이다.
 *
 * 지원하지 않으면 조용히 돌아간다. 성능 카운터는 없어도 IOMMU 동작에
 * 아무 영향이 없다.
 *
 * 호출 체인:
 *   iommu_init_pci() → [이 함수]
 */
static void init_iommu_perf_ctr(struct amd_iommu *iommu)
{
	u64 val;	/* [한국어] 설정 레지스터 값 */
	struct pci_dev *pdev = iommu->dev;	/* [한국어] 로그용 */

	if (!check_feature(FEATURE_PC))	/* [한국어] 성능 카운터가 없으면 */
		return;	/* [한국어] 없어도 IOMMU 동작에는 지장이 없다 */

	amd_iommu_pc_present = true;	/* [한국어] 전역 플래그 — 카운터 인터페이스가 시스템 단위로 노출되기 때문 */

	pci_info(pdev, "IOMMU performance counters supported\n");	/* [한국어] 쓸 수 있음을 알린다 */

	val = readl(iommu->mmio_base + MMIO_CNTR_CONF_OFFSET);	/* [한국어] 카운터 설정 레지스터 */
	iommu->max_banks = (u8) ((val >> 12) & 0x3f);	/* [한국어] 뱅크 수 */
	iommu->max_counters = (u8) ((val >> 7) & 0xf);	/* [한국어] 뱅크당 카운터 수. 둘을 곱한 것이 동시에 셀 수 있는 이벤트 수다 */

	return;	/* [한국어] 설정 완료 */
}

/*
 * [한국어]
 * amd_iommu_show_cap - sysfs 로 이 유닛의 능력 값을 보여 준다
 *
 * @dev: sysfs 의 device.
 * @attr: 속성(쓰지 않는다).
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * PCI 능력 구조에서 읽은 값을 그대로 낸다. 유닛마다 다를 수 있어 유닛별
 * 속성이다.
 */
static ssize_t amd_iommu_show_cap(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct amd_iommu *iommu = dev_to_amd_iommu(dev);	/* [한국어] sysfs device 에서 유닛으로 되짚는다 */
	return sysfs_emit(buf, "%x\n", iommu->cap);	/* [한국어] 유닛별 값이라 그대로 낸다 */
}
static DEVICE_ATTR(cap, S_IRUGO, amd_iommu_show_cap, NULL);

/*
 * [한국어]
 * amd_iommu_show_features - sysfs 로 확장 기능 비트를 보여 준다
 *
 * @dev: sysfs 의 device(쓰지 않는다).
 * @attr: 속성(쓰지 않는다).
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * 유닛별 속성인데 전역 값을 낸다는 점이 cap 과 다르다. 드라이버가 모든
 * 유닛의 공통분만 쓰므로, 유닛별 값을 보여 주면 실제로 쓸 수 없는 기능을
 * 광고하게 된다.
 */
static ssize_t amd_iommu_show_features(struct device *dev,
				       struct device_attribute *attr,
				       char *buf)
{
	return sysfs_emit(buf, "%llx:%llx\n", amd_iommu_efr, amd_iommu_efr2);	/* [한국어] 전역 공통분을 낸다 — 유닛별 값은 실제로 쓸 수 없는 기능을 광고하게 된다 */
}
static DEVICE_ATTR(features, S_IRUGO, amd_iommu_show_features, NULL);

/*
 * [한국어] sysfs 에 노출할 유닛 속성 목록
 *
 * 둘뿐이지만 성격이 다르다. cap 은 유닛별 값이고, features 는 전역
 * 공통분이다 — 유닛별 능력을 보여 주면 드라이버가 실제로 쓰지 않는 기능을
 * 사용자가 있다고 오해한다.
 */
static struct attribute *amd_iommu_attrs[] = {
	&dev_attr_cap.attr,
	/* [한국어] 이 유닛의 PCI 능력 값. 유닛마다 다를 수 있다. */
	&dev_attr_features.attr,
	/* [한국어] 확장 기능 비트. 모든 유닛의 공통분을 낸다. */
	NULL,
	/* [한국어] 목록의 끝 표시. */
};

/*
 * [한국어] 위 속성들을 담을 sysfs 디렉터리
 *
 * 유닛의 sysfs 디렉터리 아래 amd-iommu/ 로 나타난다. 이름을 두는 이유:
 * 코어 IOMMU 계층이 만든 속성들과 벤더 고유 속성을 섞지 않기 위해서다.
 */
static struct attribute_group amd_iommu_group = {
	.name = "amd-iommu",
	/* [한국어] sysfs 하위 디렉터리 이름. */
	.attrs = amd_iommu_attrs,
	/* [한국어] 그 디렉터리에 놓일 속성들. */
};

/*
 * [한국어] 유닛 등록 때 코어에 넘길 속성 그룹 목록
 *
 * 그룹이 하나뿐이라 배열이 과해 보이지만, 코어의 등록 인터페이스가 목록을
 * 요구한다. 나중에 그룹을 늘리기 쉽게 하는 형태이기도 하다.
 */
static const struct attribute_group *amd_iommu_groups[] = {
	&amd_iommu_group,
	/* [한국어] 위에서 정의한 amd-iommu 그룹. */
	NULL,
	/* [한국어] 목록의 끝 표시. */
};

/*
 * Note: IVHD 0x11 and 0x40 also contains exact copy
 * of the IOMMU Extended Feature Register [MMIO Offset 0030h].
 * Default to EFR in IVHD since it is available sooner (i.e. before PCI init).
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * late_iommu_features_init - MMIO 에서 읽은 능력이 표와 일치하는지 확인한다
 *
 * @iommu: 대상 유닛.
 *
 * 이 함수는 값을 쓰기보다 검증하기 위해 있다. 원 주석대로 IVHD 11h/40h 에
 * 이미 EFR 사본이 있고 그것을 먼저 쓰기로 했으므로, MMIO 에서 읽은 값은
 * 대조용이다.
 *
 * 왜 표를 우선하는가: 표는 PCI 초기화 전에도 읽을 수 있고, 인터럽트 재매핑
 * 결정이 그보다 먼저 이루어져야 한다. 나중에 MMIO 값이 다르다고 바꾸면
 * 이미 내린 결정과 어긋난다.
 *
 * 그래서 불일치를 발견해도 값을 바꾸지 않고 경고만 한다 — 어느 쪽이 맞든
 * 이미 표의 값으로 시스템이 구성됐기 때문이다. FW_WARN 인 이유는 두 값이
 * 다르다는 것 자체가 펌웨어의 문제이기 때문이다.
 *
 * amd_iommu_efr 이 아직 0 이면(타입 10h 라 표에 사본이 없었다면) 여기서
 * 처음 채운다.
 *
 * 호출 체인:
 *   iommu_init_pci() → [이 함수]
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * late_iommu_features_init - MMIO 에서 읽은 능력이 표와 일치하는지 확인한다
 *
 * @iommu: 대상 유닛.
 *
 * 이 함수는 값을 쓰기보다 검증하기 위해 있다. 원 주석대로 IVHD 11h/40h 에
 * 이미 EFR 사본이 있고 그것을 먼저 쓰기로 했으므로, MMIO 에서 읽은 값은
 * 대조용이다.
 *
 * 왜 표를 우선하는가: 표는 PCI 초기화 전에도 읽을 수 있고, 인터럽트 재매핑
 * 결정이 그보다 먼저 이루어져야 한다. 나중에 MMIO 값이 다르다고 바꾸면
 * 이미 내린 결정과 어긋난다.
 *
 * 그래서 불일치를 발견해도 값을 바꾸지 않고 경고만 한다 — 어느 쪽이 맞든
 * 이미 표의 값으로 시스템이 구성됐기 때문이다. FW_WARN 인 이유는 두 값이
 * 다르다는 것 자체가 펌웨어의 문제이기 때문이다.
 *
 * amd_iommu_efr 이 아직 0 이면(타입 10h 라 표에 사본이 없었다면) 여기서
 * 처음 채운다.
 *
 * 호출 체인:
 *   iommu_init_pci() → [이 함수]
 */
static void __init late_iommu_features_init(struct amd_iommu *iommu)
{
	u64 features, features2;	/* [한국어] MMIO 에서 읽은 능력 */

	if (!(iommu->cap & (1 << IOMMU_CAP_EFR)))	/* [한국어] 확장 기능 레지스터 자체가 없는 유닛 */
		return;	/* [한국어] 읽을 것이 없다 */

	/* read extended feature bits */
	features = readq(iommu->mmio_base + MMIO_EXT_FEATURES);	/* [한국어] (원 주석: 확장 기능 비트를 읽는다) */
	features2 = readq(iommu->mmio_base + MMIO_EXT_FEATURES2);	/* [한국어] 두 번째 워드 */

	if (!amd_iommu_efr) {	/* [한국어] 표에 사본이 없었다면(타입 10h) */
		amd_iommu_efr = features;	/* [한국어] 여기서 처음 채운다 */
		amd_iommu_efr2 = features2;	/* [한국어] 두 번째 워드도 */
		return;	/* [한국어] 대조할 대상이 없다 */
	}

	/*
	 * Sanity check and warn if EFR values from
	 * IVHD and MMIO conflict.
	 */
	if (features != amd_iommu_efr ||	/* [한국어] (원 주석: IVHD 와 MMIO 의 EFR 이 충돌하면 확인하고 경고한다) */
	    features2 != amd_iommu_efr2) {	/* [한국어] 두 번째 워드도 비교 */
		pr_warn(FW_WARN	/* [한국어] 값을 바꾸지 않고 경고만 한다 — 이미 표의 값으로 시스템이 구성됐다 */
			"EFR mismatch. Use IVHD EFR (%#llx : %#llx), EFR2 (%#llx : %#llx).\n",
			features, amd_iommu_efr,
			features2, amd_iommu_efr2);
	}
}

/*
 * [한국어]
 * iommu_init_pci - PCI 열거가 끝난 뒤 유닛을 마저 갖춘다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, 음수면 실패.
 *
 * 초기화의 세 번째 단계다. 앞의 두 단계는 PCI 서브시스템 없이 진행됐지만,
 * 여기서는 struct pci_dev 를 얻어 설정 공간을 정상적으로 다룰 수 있다.
 *
 * 하는 일:
 *  - IOMMU 자신의 PCI 장치를 찾는다. 이것이 없으면 아래 모든 것이 불가능하다.
 *  - PASID 능력을 계산한다. PASMAX 에서 최대 PASID 수를, GLX 에서 GCR3
 *    테이블의 레벨 수를 얻고, 후자는 모든 유닛의 최소값으로 좁힌다.
 *  - PPR 로그를 잡고, 성능 카운터 규모를 읽고, 기종별 errata 를 우회한다.
 *  - sysfs 에 등록하고 코어 IOMMU 계층에 유닛을 등록한다.
 *
 * NPCACHE 처리가 눈에 띈다. 하드웨어가 존재하지 않는 항목도 캐시하면
 * 매핑을 새로 만들 때도 무효화가 필요해져 지연 무효화를 쓸 수 없다. 그래서
 * strict 모드로 강제한다 — 원 주석이 "가상화 때문"이라고 하는 것은 이
 * 능력이 주로 가상 IOMMU 에서 나타나기 때문이다.
 *
 * RD890 블록은 레주메 대비다. 그 칩셋은 BIOS 가 레지스터를 복원해 주지
 * 않으므로, 지금 전부 읽어 두었다가 레주메 때 되쓴다. 원 주석이 그 사정을
 * 밝힌다.
 *
 * 마지막 등록 실패 처리가 미묘하다. 원 주석대로, DMA 변환을 지원하지 않아도
 * 오류를 돌려주지 않는다 — 인터럽트 재매핑만이라도 켜야 하고, 그러려면
 * 상태 기계가 계속 진행해야 하기 때문이다. sysfs 만 지워 사용자에게
 * 반쪽짜리 유닛을 노출하지 않는다.
 *
 * 호출 체인:
 *   amd_iommu_init_pci() → [이 함수] → late_iommu_features_init()
 *     → iommu_enable_gt() → amd_iommu_alloc_ppr_log() → init_iommu_perf_ctr()
 *     → iommu_device_register()
 */
static int __init iommu_init_pci(struct amd_iommu *iommu)
{
	int cap_ptr = iommu->cap_ptr;	/* [한국어] 설정 공간에서 능력 구조의 위치 */
	int ret;	/* [한국어] 하위 호출의 결과 */

	iommu->dev = pci_get_domain_bus_and_slot(iommu->pci_seg->id,	/* [한국어] IOMMU 자신의 PCI 장치를 찾는다 */
						 PCI_BUS_NUM(iommu->devid),	/* [한국어] IVHD 가 알려 준 위치로 */
						 iommu->devid & 0xff);	/* [한국어] devfn */
	if (!iommu->dev)	/* [한국어] 열거되지 않았다 */
		return -ENODEV;	/* [한국어] 설정 공간을 다룰 수 없어 아무것도 못 한다 */

	/* ACPI _PRT won't have an IRQ for IOMMU */
	iommu->dev->irq_managed = 1;	/* [한국어] (원 주석: ACPI _PRT 에는 IOMMU 용 IRQ 가 없다) PCI 계층이 인터럽트를 배정하려 들지 않게 한다 */

	pci_read_config_dword(iommu->dev, cap_ptr + MMIO_CAP_HDR_OFFSET,	/* [한국어] 능력 구조의 헤더를 읽어 */
			      &iommu->cap);	/* [한국어] 보관한다. 아래 판단들이 이 값을 본다 */

	if (!(iommu->cap & (1 << IOMMU_CAP_IOTLB)))	/* [한국어] 이 유닛이 장치 IOTLB(ATS)를 지원하지 않으면 */
		amd_iommu_iotlb_sup = false;	/* [한국어] 전역으로 내린다 — 유닛마다 다른 기능은 쓸 수 없다 */

	late_iommu_features_init(iommu);	/* [한국어] MMIO 의 능력이 표와 일치하는지 대조한다 */

	if (check_feature(FEATURE_GT)) {	/* [한국어] 게스트 변환(PASID 별 변환)을 지원하면 */
		int glxval;	/* [한국어] GCR3 테이블의 레벨 수 */
		u64 pasmax;	/* [한국어] PASID 폭의 인코딩 */

		pasmax = FIELD_GET(FEATURE_PASMAX, amd_iommu_efr);	/* [한국어] 몇 비트의 PASID 를 쓸 수 있는지 */
		iommu->iommu.max_pasids = (1 << (pasmax + 1)) - 1;	/* [한국어] 비트 수를 개수로 — 0 은 예약값이라 -1 한다 */

		BUG_ON(iommu->iommu.max_pasids & ~PASID_MASK);	/* [한국어] 드라이버가 16비트만 다루므로 그보다 크면 가정이 깨진다 */

		glxval = FIELD_GET(FEATURE_GLX, amd_iommu_efr);	/* [한국어] GCR3 테이블 레벨 수 */

		if (amd_iommu_max_glx_val == -1)	/* [한국어] 첫 유닛이면 */
			amd_iommu_max_glx_val = glxval;	/* [한국어] 기준값으로 */
		else
			amd_iommu_max_glx_val = min(amd_iommu_max_glx_val, glxval);	/* [한국어] 아니면 최소값으로 좁힌다 — 모든 유닛이 다룰 수 있어야 한다 */

		iommu_enable_gt(iommu);	/* [한국어] 게스트 변환을 켠다 */
	}

	if (check_feature(FEATURE_PPR) && amd_iommu_alloc_ppr_log(iommu))	/* [한국어] PPR 을 지원하면 로그 버퍼를 잡는다 */
		return -ENOMEM;	/* [한국어] 실패하면 페이지 폴트를 받을 수 없다 */

	if (iommu->cap & (1UL << IOMMU_CAP_NPCACHE)) {	/* [한국어] 존재하지 않는 항목도 캐시하는 하드웨어인가 */
		pr_info("Using strict mode due to virtualization\n");	/* [한국어] (원 주석) 이 능력은 주로 가상 IOMMU 에서 나타난다 */
		iommu_set_dma_strict();	/* [한국어] 매핑 생성 때도 무효화가 필요해 지연 무효화를 쓸 수 없다 */
		amd_iommu_np_cache = true;	/* [한국어] 매핑 경로가 이 값을 보고 추가 무효화를 낸다 */
	}

	init_iommu_perf_ctr(iommu);	/* [한국어] 성능 카운터의 규모를 읽어 둔다 */

	if (is_rd890_iommu(iommu->dev)) {	/* [한국어] 레주메 때 BIOS 가 레지스터를 복원해 주지 않는 칩셋인가 */
		int i, j;	/* [한국어] 간접 레지스터 순회 인덱스 */

		iommu->root_pdev =	/* [한국어] 루트 장치를 미리 잡아 둔다 */
			pci_get_domain_bus_and_slot(iommu->pci_seg->id,	/* [한국어] 같은 세그먼트의 */
						    iommu->dev->bus->number,	/* [한국어] 같은 버스에서 */
						    PCI_DEVFN(0, 0));	/* [한국어] 기능 0 장치가 루트다 */

		/*
		 * Some rd890 systems may not be fully reconfigured by the
		 * BIOS, so it's necessary for us to store this information so
		 * it can be reprogrammed on resume
		 */
		pci_read_config_dword(iommu->dev, iommu->cap_ptr + 4,	/* [한국어] (원 주석: BIOS 가 완전히 재구성해 주지 않으므로 저장해 두었다가 레주메 때 되쓴다) */
				&iommu->stored_addr_lo);	/* [한국어] BAR 하위 */
		pci_read_config_dword(iommu->dev, iommu->cap_ptr + 8,	/* [한국어] BAR 상위 */
				&iommu->stored_addr_hi);	/* [한국어] 보관 */

		/* Low bit locks writes to configuration space */
		iommu->stored_addr_lo &= ~1;	/* [한국어] (원 주석: 최하위 비트는 설정 공간 쓰기를 잠근다) 복원 때 잠긴 채로 쓰지 않도록 미리 지운다 */

		for (i = 0; i < 6; i++)	/* [한국어] L1 은 여섯 블록 */
			for (j = 0; j < 0x12; j++)	/* [한국어] 블록마다 0x12 개 레지스터 */
				iommu->stored_l1[i][j] = iommu_read_l1(iommu, i, j);	/* [한국어] 전부 읽어 보관한다 */

		for (i = 0; i < 0x83; i++)	/* [한국어] L2 는 블록 구분 없이 */
			iommu->stored_l2[i] = iommu_read_l2(iommu, i);	/* [한국어] 0x83 개 */
	}

	amd_iommu_erratum_746_workaround(iommu);	/* [한국어] 로그가 변환을 멈추는 결함 우회 */
	amd_iommu_ats_write_check_workaround(iommu);	/* [한국어] ATS 쓰기 권한 검사 강제 */

	ret = iommu_device_sysfs_add(&iommu->iommu, &iommu->dev->dev,	/* [한국어] sysfs 에 ivhd<n> 이름으로 등록 */
			       amd_iommu_groups, "ivhd%d", iommu->index);	/* [한국어] 유닛별 속성 그룹과 함께 */
	if (ret)
		return ret;

	/*
	 * Allocate per IOMMU IOPF queue here so that in attach device path,
	 * PRI capable device can be added to IOPF queue
	 */
	if (amd_iommu_gt_ppr_supported()) {	/* [한국어] (원 주석: attach 경로에서 PRI 장치를 큐에 넣을 수 있도록 여기서 미리 만든다) */
		ret = amd_iommu_iopf_init(iommu);	/* [한국어] 페이지 폴트 처리 큐 */
		if (ret)
			return ret;
	}

	ret = iommu_device_register(&iommu->iommu, &amd_iommu_ops, NULL);	/* [한국어] 코어 IOMMU 계층에 이 유닛을 등록한다 */
	if (ret || amd_iommu_pgtable == PD_MODE_NONE) {	/* [한국어] 등록 실패이거나 DMA 변환을 아예 쓰지 않는 구성 */
		/*
		 * Remove sysfs if DMA translation is not supported by the
		 * IOMMU. Do not return an error to enable IRQ remapping
		 * in state_next(), DTE[V, TV] must eventually be set to 0.
		 */
		iommu_device_sysfs_remove(&iommu->iommu);	/* [한국어] (원 주석: 오류를 돌려주지 않는다 — 인터럽트 재매핑을 켜려면 상태 기계가 계속 진행해야 한다) */
	}

	return pci_enable_device(iommu->dev);	/* [한국어] IOMMU 자신을 PCI 장치로 활성화한다 */
}

/*
 * [한국어]
 * print_iommu_info - 초기화 결과를 사람이 읽을 수 있게 한 줄로 요약한다
 *
 * 부팅 로그에서 "이 기계의 IOMMU 가 무엇을 할 수 있는가"를 한눈에 보여
 * 주는 곳이다. 문제를 진단할 때 가장 먼저 확인하는 줄이기도 하다.
 *
 * feat_str 배열의 인덱스가 곧 EFR 의 비트 번호라는 것이 이 함수의 요령이다.
 * "[5]"라는 이름이 그 증거로, 5번 비트는 정의되지 않았지만 배열의 자리를
 * 비울 수 없어 이름을 그렇게 붙였다.
 *
 * 뒤쪽 세 기능(GAM_vAPIC, SNP, SEV-TIO)은 비트 번호가 배열 범위를 넘어
 * 따로 확인한다.
 *
 * 호출 체인:
 *   amd_iommu_init() → [이 함수]
 */
static void print_iommu_info(void)
{
	int i;	/* [한국어] 비트 순회 인덱스 */
	static const char * const feat_str[] = {	/* [한국어] 배열 인덱스가 곧 EFR 의 비트 번호다 */
		"PreF", "PPR", "X2APIC", "NX", "GT", "[5]",	/* [한국어] 5번은 정의되지 않았지만 자리를 비울 수 없어 이름을 그렇게 붙였다 */
		"IA", "GA", "HE", "PC"	/* [한국어] 6~9번 비트 */
	};

	if (amd_iommu_efr) {	/* [한국어] 능력을 하나라도 읽었으면 */
		pr_info("Extended features (%#llx, %#llx):", amd_iommu_efr, amd_iommu_efr2);	/* [한국어] 원시값을 먼저 찍는다 — 이름이 없는 비트도 확인할 수 있게 */

		for (i = 0; i < ARRAY_SIZE(feat_str); ++i) {	/* [한국어] 이름이 붙은 비트들 */
			if (check_feature(1ULL << i))	/* [한국어] 그 비트가 서 있으면 */
				pr_cont(" %s", feat_str[i]);	/* [한국어] 이름을 이어 붙인다 */
		}

		if (check_feature(FEATURE_GAM_VAPIC))	/* [한국어] 비트 번호가 배열 범위를 넘는 기능들은 따로 */
			pr_cont(" GA_vAPIC");	/* [한국어] 게스트 vAPIC */

		if (check_feature(FEATURE_SNP))	/* [한국어] SEV-SNP */
			pr_cont(" SNP");	/* [한국어] 보안 중첩 페이징 */

		if (check_feature2(FEATURE_SEVSNPIO_SUP))	/* [한국어] 두 번째 워드의 기능 */
			pr_cont(" SEV-TIO");	/* [한국어] SEV 환경의 I/O 지원 */

		pr_cont("\n");	/* [한국어] 줄을 마친다 */
	}

	if (irq_remapping_enabled) {	/* [한국어] 인터럽트 재매핑이 켜졌으면 */
		pr_info("Interrupt remapping enabled\n");	/* [한국어] 알린다 */
		if (amd_iommu_xt_mode == IRQ_REMAP_X2APIC_MODE)	/* [한국어] x2APIC 모드까지 갔는가 */
			pr_info("X2APIC enabled\n");	/* [한국어] CPU 255개를 넘는 목적지를 쓸 수 있다는 뜻 */
	}
	if (amd_iommu_pgtable == PD_MODE_V2) {	/* [한국어] v2 페이지 테이블을 쓰는가 */
		pr_info("V2 page table enabled (Paging mode : %d level)\n",	/* [한국어] SVA 가 가능하다는 뜻이기도 하다 */
			amd_iommu_gpt_level);
	}
}

/*
 * [한국어]
 * amd_iommu_init_pci - PCI 단계의 유닛 초기화를 모두 마치고 캐시를 비운다
 *
 * @return: 0 성공, 음수면 어느 유닛에서 실패.
 *
 * 함수 안의 긴 영어 주석이 이 함수의 전부다: 순서가 중요하다.
 *
 * 1) iommu_init_pci 가 각 유닛을 마무리하면서, 그 과정에서 펌웨어가 요구한
 *    항등 매핑이 만들어져 장치 테이블에 써진다.
 * 2) 그다음 init_device_table_dma 가 "아직 설정되지 않은 DTE 는 DMA 를
 *    차단하도록" 만든다. 이것이 나중이어야 하는 이유는, 먼저 하면 항등
 *    매핑을 쓰는 장치까지 차단되기 때문이다.
 * 3) 마지막으로 모든 유닛의 캐시를 비운다. 그래야 장치 테이블의 변경이
 *    실제로 반영된다.
 *
 * 항등 도메인을 맨 먼저 만드는 것도 순서 문제다. 유닛을 코어에 등록하면
 * 곧바로 장치가 붙을 수 있고, 그때 기본 도메인이 있어야 한다.
 *
 * iommu_set_cwwb_range 를 PCI 초기화 뒤에 부르는 이유는 원 주석이 밝힌다 —
 * 그 설정에 필요한 정보가 PCI 단계에서야 갖춰진다.
 *
 * 호출 체인:
 *   state_next() → [이 함수] → iommu_init_pci() → init_device_table_dma()
 *     → amd_iommu_flush_all_caches()
 */
static int __init amd_iommu_init_pci(void)
{
	struct amd_iommu *iommu;	/* [한국어] 유닛 순회용 */
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 세그먼트 순회용 */
	int ret;	/* [한국어] 결과 */

	/* Init global identity domain before registering IOMMU */
	amd_iommu_init_identity_domain();	/* [한국어] (원 주석: 유닛을 등록하기 전에 전역 항등 도메인을 만든다) 등록 직후 장치가 붙을 수 있어서다 */

	for_each_iommu(iommu) {	/* [한국어] 유닛마다 */
		ret = iommu_init_pci(iommu);	/* [한국어] PCI 단계의 초기화 */
		if (ret) {
			pr_err("IOMMU%d: Failed to initialize IOMMU Hardware (error=%d)!\n",	/* [한국어] 어느 유닛에서 실패했는지 */
			       iommu->index, ret);
			goto out;	/* [한국어] 더 진행하지 않는다 */
		}
		/* Need to setup range after PCI init */
		iommu_set_cwwb_range(iommu);	/* [한국어] (원 주석: PCI 초기화 뒤에 범위를 설정해야 한다) */
	}

	/*
	 * Order is important here to make sure any unity map requirements are
	 * fulfilled. The unity mappings are created and written to the device
	 * table during the iommu_init_pci() call.
	 *
	 * After that we call init_device_table_dma() to make sure any
	 * uninitialized DTE will block DMA, and in the end we flush the caches
	 * of all IOMMUs to make sure the changes to the device table are
	 * active.
	 */
	for_each_pci_segment(pci_seg)	/* [한국어] (위 영어 주석: 항등 매핑이 먼저 만들어진 뒤여야 한다) */
		init_device_table_dma(pci_seg);	/* [한국어] 설정되지 않은 DTE 가 DMA 를 차단하도록 만든다 */

	for_each_iommu(iommu)	/* [한국어] 모든 유닛에 대해 */
		amd_iommu_flush_all_caches(iommu);	/* [한국어] 장치 테이블의 변경을 실제로 반영시킨다 */

	print_iommu_info();	/* [한국어] 결과를 한 줄로 요약해 로그에 남긴다 */

out:
	return ret;	/* [한국어] 성공이면 0 */
}

/****************************************************************************
 *
 * The following functions initialize the MSI interrupts for all IOMMUs
 * in the system. It's a bit challenging because there could be multiple
 * IOMMUs per PCI BDF but we can call pci_enable_msi(x) only once per
 * pci_dev.
 *
 ****************************************************************************/

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommu_setup_msi - 유닛의 인터럽트를 평범한 MSI 로 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, 음수면 실패.
 *
 * x2APIC 모드가 아닐 때 쓰는 경로다. IOMMU 자신도 PCI 장치이므로 평범한
 * MSI 를 쓸 수 있다.
 *
 * 원 주석이 밝히는 어려움: 한 PCI 함수에 IOMMU 가 여럿 있을 수 있는데
 * pci_enable_msi 는 장치당 한 번만 부를 수 있다. 그래서 두 번째 유닛의
 * 호출은 실패하고, 상위 경로가 그것을 감안한다.
 *
 * threaded IRQ 를 쓰는 이유: 핸들러가 이벤트 로그를 훑고 페이지 요청을
 * 큐에 넣는 등 짧지 않은 일을 한다. IRQF_ONESHOT 은 그 스레드가 끝날
 * 때까지 같은 인터럽트를 막아 로그 처리가 겹치지 않게 한다.
 *
 * 호출 체인:
 *   iommu_init_irq() → [이 함수]
 */
static int iommu_setup_msi(struct amd_iommu *iommu)
{
	int r;	/* [한국어] 결과 */

	r = pci_enable_msi(iommu->dev);	/* [한국어] (원 주석) 한 PCI 함수에 여러 유닛이 있으면 두 번째 호출은 실패한다 */
	if (r)	/* [한국어] 실패 */
		return r;	/* [한국어] 호출자가 다른 방법을 찾는다 */

	r = request_threaded_irq(iommu->dev->irq, NULL, amd_iommu_int_thread,	/* [한국어] 상단 핸들러 없이 스레드만 — 로그 처리가 짧지 않다 */
				 IRQF_ONESHOT, "AMD-Vi", iommu);	/* [한국어] 스레드가 끝날 때까지 같은 인터럽트를 막아 로그 처리가 겹치지 않게 */
	if (r) {	/* [한국어] 등록 실패 */
		pci_disable_msi(iommu->dev);	/* [한국어] 잡은 MSI 를 되돌리고 */
		return r;	/* [한국어] 실패 보고 */
	}

	return 0;	/* [한국어] 인터럽트 준비 완료 */
}

/*
 * [한국어] union intcapxt — x2APIC 방식 IOMMU 인터럽트의 설정 레지스터 형식
 *
 * MSI 를 PCI 설정 공간이 아니라 IOMMU 의 MMIO 레지스터로 설정하는 방식이다.
 * 그래야 32비트 APIC id 를 담을 수 있고, 한 PCI 함수의 여러 유닛이 각자
 * 인터럽트를 가질 수 있다 — pci_enable_msi 의 "장치당 한 번" 제약을
 * 우회하는 것이 이 방식을 쓰는 실질적인 이유이기도 하다.
 *
 * 목적지 id 가 destid_0_23 과 destid_24_31 로 나뉜 것이 눈에 띈다. 32비트를
 * 담을 연속된 자리가 없어 두 조각으로 흩어졌다.
 *
 * union 인 이유: 레지스터에는 64비트를 통째로 써야 하므로, 필드로 조립한 뒤
 * capxt 로 한 번에 쓴다.
 */
union intcapxt {
	u64	capxt;
	/* [한국어] 레지스터에 통째로 쓸 64비트 값.
	 * 아래 비트필드로 조립한 뒤 이 창으로 한 번에 쓴다 — MMIO 는 부분 쓰기를
	 *   허용하지 않는다. */
	struct {
		u64	reserved_0		:  2,
		/* [한국어] 예약. 0 이어야 한다. */
			dest_mode_logical	:  1,
			/* [한국어] 목적지 모드 — 논리인지 물리인지.
			 * 현재 APIC 드라이버가 쓰는 방식을 그대로 따른다. */
			reserved_1		:  5,
			/* [한국어] 예약. */
			destid_0_23		: 24,
			/* [한국어] 목적지 APIC id 의 하위 24비트. */
			vector			:  8,
			/* [한국어] CPU 를 깨울 벡터 번호. */
			reserved_2		: 16,
			/* [한국어] 예약. */
			destid_24_31		:  8;
			/* [한국어] 목적지 id 의 상위 8비트.
			 * 32비트를 담을 연속된 자리가 없어 하위와 떨어져 놓였다.
			 * 이 필드가 있기 때문에 x2APIC 의 넓은 목적지를 쓸 수 있다. */
	};
} __attribute__ ((packed));


static struct irq_chip intcapxt_controller;

/*
 * [한국어]
 * intcapxt_irqdomain_activate - 활성화 콜백 (하는 일 없음)
 *
 * @domain: 인터럽트 도메인.
 * @irqd: 인터럽트 데이터.
 * @reserve: 예약만 하는 호출인지.
 * @return: 항상 0.
 *
 * 이 도메인에서 활성화 시점에 할 일이 없다. 실제 하드웨어 설정은
 * unmask 에서 일어나며, 그것이 인터럽트를 켜는 시점과 자연스럽게 맞는다.
 *
 * 그래도 콜백을 두는 이유: 코어가 NULL 을 허용하지 않는 자리가 있고,
 * 명시적인 빈 구현이 "일부러 아무것도 하지 않는다"를 분명히 한다.
 */
static int intcapxt_irqdomain_activate(struct irq_domain *domain,
				       struct irq_data *irqd, bool reserve)
{
	return 0;	/* [한국어] 실제 하드웨어 설정은 unmask 가 한다 */
}

/*
 * [한국어]
 * intcapxt_irqdomain_deactivate - 비활성화 콜백 (하는 일 없음)
 *
 * @domain: 인터럽트 도메인.
 * @irqd: 인터럽트 데이터.
 *
 * activate 와 같은 이유로 비어 있다. 인터럽트를 끊는 일은 mask 가 한다.
 */
static void intcapxt_irqdomain_deactivate(struct irq_domain *domain,
					  struct irq_data *irqd)
{
}


/*
 * [한국어]
 * intcapxt_irqdomain_alloc - IOMMU 인터럽트를 이 도메인에서 잡는다
 *
 * @domain: 인터럽트 도메인.
 * @virq: 첫 가상 인터럽트 번호.
 * @nr_irqs: 개수.
 * @arg: irq_alloc_info. AMDVI 타입이어야 한다.
 * @return: 0 성공, 음수면 실패.
 *
 * 계층 구조의 한 단계다. 부모(vector 도메인)가 실제 CPU 벡터를 잡고, 이
 * 계층은 그 위에 "어느 MMIO 레지스터에 설정을 쓸 것인가"를 얹는다.
 *
 * hwirq 에 담기는 것이 그 레지스터의 오프셋이라는 점이 이 도메인의 특징이다.
 * 보통 hwirq 는 하드웨어 인터럽트 번호지만, 여기서는 설정을 쓸 위치다 —
 * unmask/mask 가 그것을 그대로 주소로 쓴다.
 *
 * chip_data 에 유닛 포인터를 넣어, 나중에 어느 유닛의 MMIO 인지 알 수 있게
 * 한다.
 *
 * edge 핸들러를 붙이는 이유: IOMMU 의 로그 인터럽트는 에지 트리거다.
 *
 * 호출 체인:
 *   iommu_setup_intcapxt() → 코어 → [이 함수]
 */
static int intcapxt_irqdomain_alloc(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs, void *arg)
{
	struct irq_alloc_info *info = arg;	/* [한국어] 인터럽트의 종류와 레지스터 오프셋 */
	int i, ret;	/* [한국어] 순회 인덱스와 결과 */

	if (!info || info->type != X86_IRQ_ALLOC_TYPE_AMDVI)	/* [한국어] 이 도메인이 다룰 수 있는 종류인가 */
		return -EINVAL;	/* [한국어] 아니면 거절 */

	ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);	/* [한국어] 부모가 실제 CPU 벡터를 잡는다 */
	if (ret < 0)	/* [한국어] 벡터 부족 */
		return ret;	/* [한국어] 실패 보고 */

	for (i = virq; i < virq + nr_irqs; i++) {	/* [한국어] 잡은 인터럽트마다 */
		struct irq_data *irqd = irq_domain_get_irq_data(domain, i);	/* [한국어] 이 계층의 데이터 */

		irqd->chip = &intcapxt_controller;	/* [한국어] MMIO 로 설정하는 칩 */
		irqd->hwirq = info->hwirq;	/* [한국어] 보통과 달리 이 값은 설정을 쓸 MMIO 오프셋이다 */
		irqd->chip_data = info->data;	/* [한국어] 어느 유닛의 MMIO 인지 */
		__irq_set_handler(i, handle_edge_irq, 0, "edge");	/* [한국어] IOMMU 의 로그 인터럽트는 에지 트리거다 */
	}

	return ret;	/* [한국어] 배선 완료 */
}

/*
 * [한국어]
 * intcapxt_irqdomain_free - 그 인터럽트를 해제한다
 *
 * @domain: 인터럽트 도메인.
 * @virq: 첫 가상 인터럽트 번호.
 * @nr_irqs: 개수.
 *
 * 이 계층이 따로 잡은 자원이 없어(hwirq 와 chip_data 는 참조일 뿐) 부모의
 * 해제만 부르면 된다.
 */
static void intcapxt_irqdomain_free(struct irq_domain *domain, unsigned int virq,
				    unsigned int nr_irqs)
{
	irq_domain_free_irqs_top(domain, virq, nr_irqs);	/* [한국어] 이 계층이 따로 잡은 자원이 없어 부모의 해제만 부른다 */
}


/*
 * [한국어]
 * intcapxt_unmask_irq - 목적지와 벡터를 써 넣어 인터럽트를 켠다
 *
 * @irqd: 대상 인터럽트.
 *
 * 이 도메인에서 하드웨어를 실제로 건드리는 유일한 곳이다. 상위 계층이 정한
 * 벡터와 목적지 CPU 를 레지스터 형식으로 조립해 한 번에 쓴다.
 *
 * 목적지 id 를 두 조각으로 쪼개는 것이 x2APIC 지원의 핵심이다 — 32비트를
 * 담을 연속된 자리가 레지스터에 없어 상위 8비트가 따로 놓인다.
 *
 * 64비트를 통째로 쓰는 것이 곧 "켜기"다. 별도의 활성화 비트가 없고, 유효한
 * 목적지가 적혀 있으면 인터럽트가 나간다 — 그래서 mask 는 0 을 쓰는 것으로
 * 충분하다.
 *
 * 호출 체인:
 *   인터럽트 활성화/affinity 변경 → [이 함수]
 */
static void intcapxt_unmask_irq(struct irq_data *irqd)
{
	struct amd_iommu *iommu = irqd->chip_data;	/* [한국어] 설정을 쓸 유닛 */
	struct irq_cfg *cfg = irqd_cfg(irqd);	/* [한국어] 상위 계층이 정한 벡터와 목적지 */
	union intcapxt xt;	/* [한국어] 조립할 레지스터 값 */

	xt.capxt = 0ULL;	/* [한국어] 명시하지 않는 비트를 0 으로 */
	xt.dest_mode_logical = apic->dest_mode_logical;	/* [한국어] 현재 APIC 드라이버가 쓰는 목적지 모드 */
	xt.vector = cfg->vector;	/* [한국어] CPU 를 깨울 벡터 */
	xt.destid_0_23 = cfg->dest_apicid & GENMASK(23, 0);	/* [한국어] 목적지 id 의 하위 24비트 */
	xt.destid_24_31 = cfg->dest_apicid >> 24;	/* [한국어] 상위 8비트 — 자리가 떨어져 있어 따로 넣는다. 이것이 x2APIC 지원의 핵심이다 */

	writeq(xt.capxt, iommu->mmio_base + irqd->hwirq);	/* [한국어] 64비트를 통째로. 유효한 목적지가 적히는 것이 곧 "켜기"다 */
}

/*
 * [한국어]
 * intcapxt_mask_irq - 레지스터를 0 으로 만들어 인터럽트를 끊는다
 *
 * @irqd: 대상 인터럽트.
 *
 * 목적지가 없으면 하드웨어가 인터럽트를 내지 않는다. 별도의 마스크 비트를
 * 쓰지 않고 설정 전체를 지우는 방식이라, unmask 가 매번 전부 다시 쓴다.
 */
static void intcapxt_mask_irq(struct irq_data *irqd)
{
	struct amd_iommu *iommu = irqd->chip_data;	/* [한국어] 대상 유닛 */

	writeq(0, iommu->mmio_base + irqd->hwirq);	/* [한국어] 목적지를 지우면 인터럽트가 나가지 않는다. 별도의 마스크 비트가 없다 */
}


/*
 * [한국어]
 * intcapxt_set_affinity - 인터럽트를 다른 CPU 로 옮긴다
 *
 * @irqd: 대상 인터럽트.
 * @mask: 허용 CPU 집합.
 * @force: 강제 여부.
 * @return: 0 이면 이 계층이 추가 작업을 했다는 뜻(코어가 unmask 를 다시 부른다).
 *
 * 부모가 새 벡터를 잡게 하고, 그 결과를 레지스터에 반영하는 것은 이 함수가
 * 직접 하지 않는다. 0 을 돌려주면 코어가 unmask 를 다시 불러 주고, 거기서
 * 새 벡터와 목적지가 써진다.
 *
 * IRQCHIP_MOVE_DEFERRED 플래그와 짝을 이루는 설계다 — 인터럽트가 도착하지
 * 않는 안전한 시점에 옮기기 위해서다.
 *
 * 호출 체인:
 *   irq_set_affinity() → [이 함수] → parent->chip->irq_set_affinity()
 */
static int intcapxt_set_affinity(struct irq_data *irqd,
				 const struct cpumask *mask, bool force)
{
	struct irq_data *parent = irqd->parent_data;	/* [한국어] 상위 vector 도메인 */
	int ret;	/* [한국어] 부모의 판단 */

	ret = parent->chip->irq_set_affinity(parent, mask, force);	/* [한국어] 새 CPU 의 벡터를 잡게 한다 */
	if (ret < 0 || ret == IRQ_SET_MASK_OK_DONE)	/* [한국어] 실패했거나 부모가 이미 다 처리했으면 */
		return ret;	/* [한국어] 그대로 전달 */
	return 0;	/* [한국어] 0 을 돌려주면 코어가 unmask 를 다시 불러 새 설정을 써 준다 */
}

/*
 * [한국어]
 * intcapxt_set_wake - 이 인터럽트로 시스템을 깨울 수 있는가
 *
 * @irqd: 대상 인터럽트.
 * @on: 깨우기를 켜려는 요청인지.
 * @return: 끄는 요청이면 0, 켜려는 요청이면 -EOPNOTSUPP.
 *
 * IOMMU 의 로그 인터럽트로 시스템을 깨울 이유가 없고, 서스펜드 중에는
 * IOMMU 도 꺼져 있다. 그래서 켜려는 요청만 거절한다.
 *
 * 끄는 요청에 0 을 돌려주는 이유: 그것은 이미 만족된 상태이므로 오류가
 * 아니다. 무조건 거절하면 정리 경로가 실패한다.
 */
static int intcapxt_set_wake(struct irq_data *irqd, unsigned int on)
{
	return on ? -EOPNOTSUPP : 0;	/* [한국어] 켜려는 요청만 거절한다. 끄는 요청은 이미 만족된 상태라 성공이다 */
}

/*
 * [한국어] struct irq_chip intcapxt_controller — MMIO 로 설정하는 IOMMU 인터럽트
 *
 * 평범한 MSI 칩과 달리 설정을 PCI 설정 공간이 아니라 IOMMU 의 MMIO
 * 레지스터에 쓴다. 그래야 32비트 APIC id 를 담을 수 있고, 한 PCI 함수의
 * 여러 유닛이 각자 인터럽트를 가질 수 있다.
 *
 * 두 플래그가 이 칩의 성격을 정한다:
 *  - MASK_ON_SUSPEND: 서스펜드 때 마스크한다. IOMMU 도 함께 꺼지므로
 *    깨우기 용도로 쓸 수 없고, 그렇다면 막아 두는 편이 안전하다.
 *  - MOVE_DEFERRED: 인터럽트를 옮기는 것을 안전한 시점까지 미룬다.
 *    설정을 통째로 갈아 쓰는 방식이라, 도착 중인 인터럽트가 있으면
 *    중간 상태를 볼 수 있다.
 */
static struct irq_chip intcapxt_controller = {
	.name			= "IOMMU-MSI",
	/* [한국어] /proc/interrupts 에 보이는 이름. */
	.irq_unmask		= intcapxt_unmask_irq,
	/* [한국어] 목적지와 벡터를 써 넣어 켠다. 실제 하드웨어 설정은 여기서만 일어난다. */
	.irq_mask		= intcapxt_mask_irq,
	/* [한국어] 레지스터를 0 으로 만들어 끊는다. */
	.irq_ack		= irq_chip_ack_parent,
	/* [한국어] 수신 확인은 부모(APIC)가 한다 — 이 계층은 목적지만 관리한다. */
	.irq_retrigger		= irq_chip_retrigger_hierarchy,
	/* [한국어] 재발생 요청도 계층을 따라 위로 넘긴다. */
	.irq_set_affinity       = intcapxt_set_affinity,
	/* [한국어] 부모가 벡터를 잡게 하고, 반영은 코어가 부를 unmask 에 맡긴다. */
	.irq_set_wake		= intcapxt_set_wake,
	/* [한국어] 깨우기는 지원하지 않는다 — 서스펜드 중에는 IOMMU 도 꺼져 있다. */
	.flags			= IRQCHIP_MASK_ON_SUSPEND | IRQCHIP_MOVE_DEFERRED,
	/* [한국어] 서스펜드 때 마스크하고, 이동은 안전한 시점까지 미룬다. */
};

/*
 * [한국어] struct irq_domain_ops intcapxt_domain_ops — 이 도메인의 콜백 표
 *
 * activate/deactivate 가 비어 있는 것이 이 도메인의 특징이다. 하드웨어
 * 설정이 mask/unmask 에서만 일어나므로, 활성화 시점에 따로 할 일이 없다.
 *
 * select 가 없는 것도 눈에 띈다 — 이 도메인의 인터럽트는 드라이버가 직접
 * 잡으므로, "이게 네 것이냐"를 물어올 일이 없다.
 */
static const struct irq_domain_ops intcapxt_domain_ops = {
	.alloc			= intcapxt_irqdomain_alloc,
	/* [한국어] 부모의 벡터 위에 MMIO 오프셋과 유닛 정보를 얹는다. */
	.free			= intcapxt_irqdomain_free,
	/* [한국어] 이 계층이 잡은 자원이 없어 부모의 해제만 부른다. */
	.activate		= intcapxt_irqdomain_activate,
	/* [한국어] 하는 일 없음. 설정은 unmask 가 한다. */
	.deactivate		= intcapxt_irqdomain_deactivate,
	/* [한국어] 하는 일 없음. 끊는 것은 mask 가 한다. */
};


static struct irq_domain *iommu_irqdomain;

/*
 * [한국어]
 * iommu_get_irqdomain - 모든 유닛이 공유하는 인터럽트 도메인을 얻는다
 *
 * @return: 그 도메인, 실패하면 NULL.
 *
 * 도메인이 유닛마다가 아니라 시스템에 하나뿐이라는 점이 이 함수의 요점이다.
 * 도메인은 "설정을 MMIO 로 쓴다"는 방식을 나타낼 뿐이고, 어느 유닛의 어느
 * 레지스터인지는 인터럽트마다 hwirq/chip_data 에 담기기 때문이다.
 *
 * 첫 호출에서 만들고 이후에는 그것을 돌려준다. 원 주석이 밝히듯 초기화가
 * 단일 스레드라 락이 필요 없다 — "아직은(yet)"이라는 단서가 그 전제가
 * 깨질 수 있음을 시사한다.
 *
 * x86_vector_domain 을 부모로 삼는다. 실제 CPU 벡터는 그쪽이 잡고, 이
 * 계층은 그 위에 MMIO 설정을 얹는다.
 *
 * 호출 체인:
 *   __iommu_setup_intcapxt() → [이 함수]
 */
static struct irq_domain *iommu_get_irqdomain(void)
{
	struct fwnode_handle *fn;	/* [한국어] 도메인을 이름으로 식별할 펌웨어 노드 */

	/* No need for locking here (yet) as the init is single-threaded */
	if (iommu_irqdomain)	/* [한국어] (원 주석: 초기화가 단일 스레드라 아직은 락이 필요 없다) */
		return iommu_irqdomain;	/* [한국어] 이미 만들었으면 그것을 공유한다 — 도메인은 시스템에 하나뿐이다 */

	fn = irq_domain_alloc_named_fwnode("AMD-Vi-MSI");	/* [한국어] /proc/interrupts 등에 이 이름으로 보인다 */
	if (!fn)	/* [한국어] 노드 할당 실패 */
		return NULL;	/* [한국어] 도메인을 만들 수 없다 */

	iommu_irqdomain = irq_domain_create_hierarchy(x86_vector_domain, 0, 0,	/* [한국어] 실제 CPU 벡터는 부모가 잡는다 */
						      fn, &intcapxt_domain_ops,	/* [한국어] 이 계층은 그 위에 MMIO 설정을 얹는다 */
						      NULL);	/* [한국어] 호스트 데이터 없음 — 유닛 정보는 인터럽트마다 chip_data 에 담긴다 */
	if (!iommu_irqdomain)	/* [한국어] 도메인 생성 실패 */
		irq_domain_free_fwnode(fn);	/* [한국어] 노드를 되돌린다 */

	return iommu_irqdomain;	/* [한국어] 성공하면 도메인, 실패하면 NULL */
}

/*
 * [한국어]
 * __iommu_setup_intcapxt - 로그 인터럽트 하나를 잡고 핸들러를 건다
 *
 * @iommu: 대상 유닛.
 * @devname: /proc/interrupts 에 보일 이름.
 * @hwirq: 설정을 쓸 MMIO 레지스터의 오프셋.
 * @thread_fn: 그 인터럽트의 스레드 핸들러.
 * @return: 0 성공, 음수면 실패.
 *
 * 세 로그(이벤트/PPR/GA)가 같은 절차를 쓰므로 다른 점만 인자로 받는다.
 *
 * hwirq 로 MMIO 오프셋을 넘기는 것이 이 도메인의 규약이다. 그 값이
 * irq_data 에 실려 unmask 가 설정을 쓸 주소가 된다.
 *
 * 유닛과 같은 NUMA 노드에서 인터럽트를 잡는 이유: 핸들러가 그 유닛의
 * 로그 버퍼를 읽으므로, 같은 노드의 CPU 에서 처리하는 편이 빠르다.
 *
 * 실패 시 irq_domain_remove 까지 부르는 것이 눈에 띈다. 도메인은 공유
 * 자원인데 여기서 없애면 다른 유닛의 인터럽트도 사라진다 — 다만 이 경로가
 * 실패하면 어차피 초기화 전체가 중단되므로 실질적인 문제는 되지 않는다.
 *
 * 호출 체인:
 *   iommu_setup_intcapxt() → [이 함수] → iommu_get_irqdomain()
 */
static int __iommu_setup_intcapxt(struct amd_iommu *iommu, const char *devname,
				  int hwirq, irq_handler_t thread_fn)
{
	struct irq_domain *domain;	/* [한국어] 공유 도메인 */
	struct irq_alloc_info info;	/* [한국어] 도메인에 넘길 요청 정보 */
	int irq, ret;	/* [한국어] 잡은 인터럽트 번호와 결과 */
	int node = dev_to_node(&iommu->dev->dev);	/* [한국어] 핸들러가 이 유닛의 버퍼를 읽으므로 같은 노드가 낫다 */

	domain = iommu_get_irqdomain();	/* [한국어] 없으면 여기서 만들어진다 */
	if (!domain)	/* [한국어] 도메인이 없다 */
		return -ENXIO;	/* [한국어] 인터럽트를 잡을 수 없다 */

	init_irq_alloc_info(&info, NULL);	/* [한국어] 요청 정보를 초기화 */
	info.type = X86_IRQ_ALLOC_TYPE_AMDVI;	/* [한국어] 이 도메인이 받아들이는 종류 */
	info.data = iommu;	/* [한국어] chip_data 가 되어 어느 유닛인지 알려 준다 */
	info.hwirq = hwirq;	/* [한국어] 설정을 쓸 MMIO 오프셋. 이 도메인의 규약이다 */

	irq = irq_domain_alloc_irqs(domain, 1, node, &info);	/* [한국어] 부모의 벡터 위에 이 계층을 얹어 인터럽트 하나를 잡는다 */
	if (irq < 0) {	/* [한국어] 벡터 부족이나 배선 실패 */
		irq_domain_remove(domain);	/* [한국어] 초기화가 어차피 중단되므로 공유 도메인도 정리한다 */
		return irq;	/* [한국어] 실패 보고 */
	}

	ret = request_threaded_irq(irq, NULL, thread_fn, IRQF_ONESHOT, devname,	/* [한국어] 로그 처리가 짧지 않아 스레드 핸들러를 쓴다 */
				   iommu);	/* [한국어] 핸들러가 받을 유닛 */
	if (ret) {	/* [한국어] 핸들러 등록 실패 */
		irq_domain_free_irqs(irq, 1);	/* [한국어] 잡은 인터럽트를 되돌리고 */
		irq_domain_remove(domain);	/* [한국어] 도메인도 */
		return ret;	/* [한국어] 실패 보고 */
	}

	return 0;	/* [한국어] 이 로그의 인터럽트가 준비됐다 */
}

/*
 * [한국어]
 * iommu_setup_intcapxt - 유닛의 세 로그 인터럽트를 모두 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, 음수면 어느 하나가 실패.
 *
 * x2APIC 모드에서 쓰는 경로다. MSI 와 달리 로그마다 별도의 인터럽트를
 * 가질 수 있어, 이벤트·PPR·GA 를 각각 다른 CPU 에서 처리할 수 있다.
 *
 * 이름을 구조체 안의 버퍼에 만드는 이유: request_irq 는 이름 포인터를
 * 보관만 하고 복사하지 않는다. 지역 변수를 넘기면 함수가 끝난 뒤
 * /proc/interrupts 가 사라진 메모리를 읽는다.
 *
 * GA 인터럽트만 #ifdef 안에 있다. 재매핑을 끈 커널에서는 GA 로그 자체가
 * 없기 때문이다.
 *
 * 호출 체인:
 *   iommu_init_irq() → [이 함수] → __iommu_setup_intcapxt()
 */
static int iommu_setup_intcapxt(struct amd_iommu *iommu)
{
	int ret;	/* [한국어] 각 단계의 결과 */

	snprintf(iommu->evt_irq_name, sizeof(iommu->evt_irq_name),	/* [한국어] 구조체 안의 버퍼에 만든다 — request_irq 는 이름을 복사하지 않는다 */
		 "AMD-Vi%d-Evt", iommu->index);	/* [한국어] 유닛 번호를 넣어 구별할 수 있게 */
	ret = __iommu_setup_intcapxt(iommu, iommu->evt_irq_name,	/* [한국어] 이벤트 로그 인터럽트 */
				     MMIO_INTCAPXT_EVT_OFFSET,	/* [한국어] 그 설정 레지스터의 오프셋 */
				     amd_iommu_int_thread_evtlog);	/* [한국어] 전용 핸들러 */
	if (ret)
		return ret;

	snprintf(iommu->ppr_irq_name, sizeof(iommu->ppr_irq_name),	/* [한국어] PPR 로그 인터럽트의 이름 */
		 "AMD-Vi%d-PPR", iommu->index);	/* [한국어] 유닛 번호와 함께 */
	ret = __iommu_setup_intcapxt(iommu, iommu->ppr_irq_name,	/* [한국어] PPR 인터럽트 */
				     MMIO_INTCAPXT_PPR_OFFSET,	/* [한국어] 그 설정 레지스터 */
				     amd_iommu_int_thread_pprlog);	/* [한국어] 전용 핸들러 */
	if (ret)
		return ret;

#ifdef CONFIG_IRQ_REMAP
	snprintf(iommu->ga_irq_name, sizeof(iommu->ga_irq_name),	/* [한국어] GA 로그 인터럽트의 이름 */
		 "AMD-Vi%d-GA", iommu->index);	/* [한국어] 유닛 번호와 함께 */
	ret = __iommu_setup_intcapxt(iommu, iommu->ga_irq_name,	/* [한국어] GA 인터럽트 — 재매핑을 켠 커널에서만 */
				     MMIO_INTCAPXT_GALOG_OFFSET,	/* [한국어] 그 설정 레지스터 */
				     amd_iommu_int_thread_galog);	/* [한국어] 전용 핸들러 */
#endif

	return ret;	/* [한국어] 세 인터럽트가 모두 준비됐다 */
}

/*
 * [한국어]
 * iommu_init_irq - 유닛의 인터럽트를 잡고 폴트 보고를 켠다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENODEV 면 인터럽트를 잡을 방법이 없다.
 *
 * 두 방식 중 하나를 고른다. x2APIC 모드면 MMIO 방식(intcapxt)을, 아니면
 * 평범한 MSI 를 쓴다. 후자는 장치에 MSI 능력이 있어야 하므로 그것도 확인한다.
 *
 * int_enabled 로 중복 등록을 막는 것이 눈에 띈다. 이 함수는 서스펜드에서
 * 깨어날 때도 불리는데, 그때는 인터럽트가 이미 잡혀 있고 하드웨어의
 * 활성화 비트만 다시 세우면 된다 — enable_faults 레이블이 그 진입점이다.
 *
 * 마지막에 이벤트 인터럽트를 켜는 것이 순서상 중요하다. 앞 단계에서 로그
 * 기록만 켜 두고 인터럽트는 미뤄 두었는데, 핸들러가 등록되기 전에 인터럽트가
 * 오면 처리할 곳이 없기 때문이다.
 *
 * 호출 체인:
 *   amd_iommu_enable_interrupts() → [이 함수]
 *     → iommu_setup_intcapxt()/iommu_setup_msi()
 */
static int iommu_init_irq(struct amd_iommu *iommu)
{
	int ret;	/* [한국어] 결과 */

	if (iommu->int_enabled)	/* [한국어] 서스펜드에서 깨어난 경우 — 인터럽트는 이미 잡혀 있다 */
		goto enable_faults;	/* [한국어] 하드웨어의 활성화 비트만 다시 세우면 된다 */

	if (amd_iommu_xt_mode == IRQ_REMAP_X2APIC_MODE)	/* [한국어] x2APIC 모드면 */
		ret = iommu_setup_intcapxt(iommu);	/* [한국어] 로그마다 별도 인터럽트를 잡는 MMIO 방식 */
	else if (iommu->dev->msi_cap)	/* [한국어] 아니면 장치에 MSI 능력이 있는지 보고 */
		ret = iommu_setup_msi(iommu);	/* [한국어] 평범한 MSI 하나를 잡는다 */
	else
		ret = -ENODEV;	/* [한국어] 둘 다 안 되면 인터럽트를 받을 방법이 없다 */

	if (ret)
		return ret;	/* [한국어] 실패 보고 */

	iommu->int_enabled = true;	/* [한국어] 다음에 불려도 다시 잡지 않게 */
enable_faults:

	if (amd_iommu_xt_mode == IRQ_REMAP_X2APIC_MODE)	/* [한국어] MMIO 방식을 쓴다면 */
		iommu_feature_enable(iommu, CONTROL_INTCAPXT_EN);	/* [한국어] 하드웨어에 그 방식을 쓰라고 알린다 */

	iommu_feature_enable(iommu, CONTROL_EVT_INT_EN);	/* [한국어] 이제 핸들러가 있으므로 이벤트 인터럽트를 켠다 — 앞 단계가 미뤄 둔 일이다 */

	return 0;	/* [한국어] 인터럽트 준비 완료 */
}

/****************************************************************************
 *
 * The next functions belong to the third pass of parsing the ACPI
 * table. In this last pass the memory mapping requirements are
 * gathered (like exclusion and unity mapping ranges).
 *
 ****************************************************************************/

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * free_unity_maps - 모든 세그먼트의 항등 매핑 목록을 놓는다
 *
 * 초기화 실패 정리 경로다. 목록이 세그먼트마다 있으므로 이중 순회가 된다.
 *
 * 항등 매핑 정보는 도메인을 만들 때마다 참조되므로, 정상 동작 중에는
 * 놓지 않는다.
 *
 * 호출 체인:
 *   free_iommu_resources() → [이 함수]
 */
static void __init free_unity_maps(void)
{
	struct unity_map_entry *entry, *next;	/* [한국어] 목록에서 빼며 훑는다 */
	struct amd_iommu_pci_seg *p, *pci_seg;	/* [한국어] 목록이 세그먼트마다 있어 이중 순회 */

	for_each_pci_segment_safe(pci_seg, p) {	/* [한국어] 모든 세그먼트 */
		list_for_each_entry_safe(entry, next, &pci_seg->unity_map, list) {	/* [한국어] 그 세그먼트의 항등 매핑들 */
			list_del(&entry->list);	/* [한국어] 목록에서 빼고 */
			kfree(entry);	/* [한국어] 놓는다 */
		}
	}
}

/* called for unity map ACPI definition */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * init_unity_map_range - IVMD 항목 하나를 항등 매핑 목록에 등록한다
 *
 * @m: IVMD 헤더.
 * @ivrs_base: IVRS 표(세그먼트를 찾는 데 쓴다).
 * @return: 0 성공(모르는 타입도 0), -ENOMEM 이면 할당 실패.
 *
 * 펌웨어가 "이 장치들은 이 물리 주소 구간을 그대로 써야 한다"고 요구한
 * 것을 목록에 담는다. 도메인을 만들 때마다 이 목록을 훑어 매핑을 넣어 준다.
 *
 * 타입에 따라 적용 대상이 다르다: 장치 하나, 세그먼트 전체, 또는 범위.
 * 모르는 타입은 조용히 무시한다 — 표에 우리가 이해하지 못하는 항목이 있어도
 * 부팅을 막을 이유가 없다.
 *
 * 주소를 페이지 단위로 올림하는 이유: IOMMU 는 페이지 단위로만 매핑한다.
 *
 * prot 를 flags >> 1 로 얻는 것은 IVMD_FLAG_IR/IW 가 IOMMU_PROT_IR/IW 와
 * 한 비트 어긋나 있기 때문이다.
 *
 * 제외 범위를 읽기/쓰기 항등 매핑으로 바꿔 다루는 것이 이 함수에서 가장
 * 중요한 판단이다. 원 주석이 이유를 밝힌다: IVMD 항목이 여럿이면 일부
 * BIOS 가 제외 범위 레지스터를 덮어써 앞의 설정을 잃는다. 항등 매핑으로
 * 다루면 레지스터가 아니라 페이지 테이블에 담기므로 그런 충돌이 없다.
 *
 * 호출 체인:
 *   init_memory_definitions() → [이 함수] → get_pci_segment()
 */
static int __init init_unity_map_range(struct ivmd_header *m,
				       struct acpi_table_header *ivrs_base)
{
	struct unity_map_entry *e = NULL;	/* [한국어] 목록에 담을 항목 */
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 대상 세그먼트 */
	char *s;	/* [한국어] 로그에 쓸 타입 이름 */

	pci_seg = get_pci_segment(m->pci_seg, ivrs_base);	/* [한국어] 없으면 여기서 만들어진다 */
	if (pci_seg == NULL)	/* [한국어] 세그먼트를 만들 수 없다 */
		return -ENOMEM;	/* [한국어] 등록 불가 */

	e = kzalloc_obj(*e);	/* [한국어] 항목 할당 */
	if (e == NULL)	/* [한국어] 메모리 부족 */
		return -ENOMEM;	/* [한국어] 등록 불가 */

	switch (m->type) {	/* [한국어] 적용 대상이 타입마다 다르다 */
	default:	/* [한국어] 드라이버가 모르는 타입 */
		kfree(e);	/* [한국어] 할당한 항목을 놓고 */
		return 0;	/* [한국어] 조용히 무시한다 — 부팅을 막을 이유가 없다 */
	case ACPI_IVMD_TYPE:	/* [한국어] 장치 하나 */
		s = "IVMD_TYPEi\t\t\t";	/* [한국어] 로그용 이름 */
		e->devid_start = e->devid_end = m->devid;	/* [한국어] 시작과 끝이 같다 */
		break;
	case ACPI_IVMD_TYPE_ALL:	/* [한국어] 세그먼트의 모든 장치 */
		s = "IVMD_TYPE_ALL\t\t";	/* [한국어] 로그용 이름 */
		e->devid_start = 0;	/* [한국어] 0 부터 */
		e->devid_end = pci_seg->last_bdf;	/* [한국어] 최대 id 까지 */
		break;
	case ACPI_IVMD_TYPE_RANGE:	/* [한국어] 장치 범위 */
		s = "IVMD_TYPE_RANGE\t\t";	/* [한국어] 로그용 이름 */
		e->devid_start = m->devid;	/* [한국어] 범위의 시작 */
		e->devid_end = m->aux;	/* [한국어] 끝은 aux 필드에 있다 */
		break;
	}
	e->address_start = PAGE_ALIGN(m->range_start);	/* [한국어] IOMMU 는 페이지 단위로만 매핑한다 */
	e->address_end = e->address_start + PAGE_ALIGN(m->range_length);	/* [한국어] 길이도 올림해 구간을 넉넉히 잡는다 */
	e->prot = m->flags >> 1;	/* [한국어] IVMD 의 IR/IW 비트가 IOMMU_PROT 와 한 비트 어긋나 있다 */

	/*
	 * Treat per-device exclusion ranges as r/w unity-mapped regions
	 * since some buggy BIOSes might lead to the overwritten exclusion
	 * range (exclusion_start and exclusion_length members). This
	 * happens when there are multiple exclusion ranges (IVMD entries)
	 * defined in ACPI table.
	 */
	if (m->flags & IVMD_FLAG_EXCL_RANGE)	/* [한국어] (원 주석: 장치별 제외 범위를 읽기/쓰기 항등 매핑으로 다룬다) */
		e->prot = (IVMD_FLAG_IW | IVMD_FLAG_IR) >> 1;	/* [한국어] IVMD 가 여럿이면 일부 BIOS 가 제외 범위 레지스터를 덮어써 앞 설정을 잃는다. 페이지 테이블에 담으면 그런 충돌이 없다 */

	DUMP_printk("%s devid_start: %04x:%02x:%02x.%x devid_end: "	/* [한국어] 상세 로그: 적용 범위와 주소 구간 */
		    "%04x:%02x:%02x.%x range_start: %016llx range_end: %016llx"
		    " flags: %x\n", s, m->pci_seg,
		    PCI_BUS_NUM(e->devid_start), PCI_SLOT(e->devid_start),
		    PCI_FUNC(e->devid_start), m->pci_seg,
		    PCI_BUS_NUM(e->devid_end),
		    PCI_SLOT(e->devid_end), PCI_FUNC(e->devid_end),
		    e->address_start, e->address_end, m->flags);

	list_add_tail(&e->list, &pci_seg->unity_map);	/* [한국어] 도메인을 만들 때마다 이 목록을 훑는다 */

	return 0;	/* [한국어] 등록 완료 */
}

/* iterates over all memory definitions we find in the ACPI table */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * init_memory_definitions - 표의 모든 IVMD 를 훑는다
 *
 * @table: IVRS 표.
 * @return: 항상 0.
 *
 * 표를 세 번 훑는 것 중 마지막이다. 앞의 둘이 유닛과 장치를 다뤘다면
 * 여기서는 메모리 요구사항만 모은다.
 *
 * 항등 매핑이나 제외 범위 플래그가 있는 항목만 처리한다. 그 둘 중 하나도
 * 없는 IVMD 는 이 드라이버가 할 일이 없는 항목이다.
 *
 * 실패해도 0 을 돌려주는 것에 유의: init_unity_map_range 의 반환값을 보지
 * 않는다. 항등 매핑 하나를 등록하지 못해도 부팅은 계속하는 편이 낫다는
 * 판단이며, 그 결과는 해당 장치가 오동작하는 것으로 나타난다.
 *
 * 호출 체인:
 *   early_amd_iommu_init() → [이 함수] → init_unity_map_range()
 */
static int __init init_memory_definitions(struct acpi_table_header *table)
{
	u8 *p = (u8 *)table, *end = (u8 *)table;	/* [한국어] 표를 훑을 커서와 끝 */
	struct ivmd_header *m;	/* [한국어] 현재 IVMD */

	end += table->length;	/* [한국어] 표의 끝 */
	p += IVRS_HEADER_LENGTH;	/* [한국어] 첫 항목으로 */

	while (p < end) {	/* [한국어] 항목을 하나씩 */
		m = (struct ivmd_header *)p;	/* [한국어] 현재 위치를 IVMD 로 */
		if (m->flags & (IVMD_FLAG_UNITY_MAP | IVMD_FLAG_EXCL_RANGE))	/* [한국어] 둘 중 하나라도 없으면 이 드라이버가 할 일이 없다 */
			init_unity_map_range(m, table);	/* [한국어] 반환값을 보지 않는다 — 하나를 놓쳐도 부팅은 계속하는 편이 낫다 */

		p += m->length;	/* [한국어] 다음 항목으로 */
	}

	return 0;	/* [한국어] 세 번째 순회 완료 */
}

/*
 * Init the device table to not allow DMA access for devices
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * init_device_table_dma - 설정되지 않은 장치의 DMA 를 차단한다
 *
 * @pci_seg: 대상 세그먼트.
 *
 * 이 함수가 IOMMU 격리의 기본값을 정한다. 표의 모든 항목에 V(유효)와
 * TV(변환 유효)를 세우면, 페이지 테이블이 없는 장치의 요청은 "변환할
 * 매핑이 없음"으로 차단된다.
 *
 * V 만 세우고 TV 를 세우지 않으면 변환 없이 통과시킨다는 뜻이 되므로,
 * 둘을 함께 세우는 것이 곧 "차단"이다.
 *
 * SNP 환경에서 TV 를 세우지 않는 이유: 그 모드에서는 하드웨어가 별도의
 * 무결성 검사를 하고, TV 의 의미가 달라진다.
 *
 * 순서가 중요하다 — 항등 매핑이 먼저 만들어진 뒤에 불려야 한다. 반대로
 * 하면 그 매핑을 쓰는 장치까지 차단된다(amd_iommu_init_pci 참고).
 *
 * 호출 체인:
 *   amd_iommu_init_pci() → [이 함수]
 */
static void init_device_table_dma(struct amd_iommu_pci_seg *pci_seg)
{
	u32 devid;	/* [한국어] 순회 인덱스 */
	struct dev_table_entry *dev_table = pci_seg->dev_table;	/* [한국어] 대상 표 */

	if (!dev_table || amd_iommu_pgtable == PD_MODE_NONE)	/* [한국어] 표가 없거나 DMA 변환을 아예 쓰지 않는 구성 */
		return;	/* [한국어] 차단할 것이 없다 */

	for (devid = 0; devid <= pci_seg->last_bdf; ++devid) {	/* [한국어] 모든 항목에 */
		set_dte_bit(&dev_table[devid], DEV_ENTRY_VALID);	/* [한국어] V — 하드웨어가 아는 장치로 만든다 */
		if (!amd_iommu_snp_en)	/* [한국어] SNP 에서는 TV 의 의미가 달라 세우지 않는다 */
			set_dte_bit(&dev_table[devid], DEV_ENTRY_TRANSLATION);	/* [한국어] TV — V 와 함께 서야 "변환하되 매핑이 없으므로 차단"이 된다 */
	}
}

/*
 * [한국어]
 * uninit_device_table_dma - 장치 테이블을 통째로 지운다
 *
 * @pci_seg: 대상 세그먼트.
 *
 * 초기화 실패 정리 경로다. 모든 항목의 앞 두 워드를 0 으로 만들면 V 가
 * 내려가 그 장치들이 IOMMU 에게 "모르는 장치"가 된다.
 *
 * 그 상태의 요청은 이벤트 로그를 채우는 오류가 되지만, 어차피 IOMMU 를
 * 끄고 물러나는 경로이므로 문제되지 않는다.
 *
 * 뒤 두 워드(인터럽트 재매핑 정보)를 남기는 것에 유의: DMA 를 포기해도
 * 인터럽트 재매핑은 계속 쓸 수 있다.
 *
 * 호출 체인:
 *   amd_iommu_uninit_devices()/실패 정리 → [이 함수]
 */
static void __init uninit_device_table_dma(struct amd_iommu_pci_seg *pci_seg)
{
	u32 devid;	/* [한국어] 순회 인덱스 */
	struct dev_table_entry *dev_table = pci_seg->dev_table;	/* [한국어] 대상 표 */

	if (dev_table == NULL)	/* [한국어] 표가 없으면 */
		return;	/* [한국어] 지울 것이 없다 */

	for (devid = 0; devid <= pci_seg->last_bdf; ++devid) {	/* [한국어] 모든 항목의 */
		dev_table[devid].data[0] = 0ULL;	/* [한국어] 앞 워드를 지운다 — V 가 내려가 "모르는 장치"가 된다 */
		dev_table[devid].data[1] = 0ULL;	/* [한국어] 두 번째 워드까지. 뒤 두 워드(인터럽트 재매핑)는 남긴다 */
	}
}

/*
 * [한국어]
 * init_device_table - 모든 장치의 인터럽트 재매핑을 켠다
 *
 * 이름과 달리 DMA 가 아니라 인터럽트 쪽만 건드린다. 모든 DTE 에 IRQ_TBL_EN
 * 을 세워, 그 장치의 인터럽트가 재매핑 표를 거치게 만든다.
 *
 * 재매핑을 쓰지 않으면 곧바로 돌아간다.
 *
 * 왜 모든 장치에 미리 세우는가: 표 자체는 장치가 인터럽트를 실제로 쓸 때
 * 만들어지지만, 이 비트가 없으면 그 사이에 온 인터럽트가 재매핑을 우회한다.
 * 미리 켜 두면 표가 없는 장치의 인터럽트는 폴트가 되어 차단된다 — 우회보다
 * 안전한 실패다.
 *
 * 호출 체인:
 *   early_enable_iommus() → [이 함수]
 */
static void init_device_table(void)
{
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 세그먼트 순회용 */
	u32 devid;	/* [한국어] 장치 순회용 */

	if (!amd_iommu_irq_remap)	/* [한국어] 재매핑을 쓰지 않으면 */
		return;	/* [한국어] 할 일이 없다 */

	for_each_pci_segment(pci_seg) {	/* [한국어] 모든 세그먼트의 */
		for (devid = 0; devid <= pci_seg->last_bdf; ++devid)	/* [한국어] 모든 장치에 */
			set_dte_bit(&pci_seg->dev_table[devid], DEV_ENTRY_IRQ_TBL_EN);	/* [한국어] 미리 켜 둔다. 표가 없는 장치의 인터럽트는 폴트가 되어 차단된다 — 우회보다 안전한 실패다 */
	}
}

/*
 * [한국어]
 * iommu_init_flags - 펌웨어가 권장한 설정과 드라이버가 요구하는 설정을 적용한다
 *
 * @iommu: 대상 유닛.
 *
 * 앞의 네 항목은 IVHD 가 권장한 것을 그대로 따른다. 삼항 연산자를 문장처럼
 * 쓴 형태가 낯설지만, "표가 켜라면 켜고 아니면 끈다"를 한 줄로 나타낸다.
 * 명시적으로 끄는 것이 중요하다 — 앞선 부팅이나 펌웨어가 켜 둔 상태가
 * 남아 있을 수 있기 때문이다.
 *
 * 뒤의 셋은 표와 무관하게 드라이버가 정한다.
 *  - COHERENT_EN: IOMMU 의 표 접근을 캐시 코히런트하게 만든다. 그래야
 *    드라이버가 표를 고칠 때마다 캐시를 직접 밀어내지 않아도 된다.
 *  - 무효화 타임아웃 1초: 응답하지 않는 장치가 시스템을 영원히 붙잡지
 *    못하게 한다. 없음(무한 대기)과 100초 사이에서 고른 값이다.
 *  - EPH: 향상된 PPR 처리. SVA 의 응답 프로토콜이 이것을 요구한다.
 *
 * 호출 체인:
 *   early_enable_iommu() → [이 함수]
 */
static void iommu_init_flags(struct amd_iommu *iommu)
{
	iommu->acpi_flags & IVHD_FLAG_HT_TUN_EN_MASK ?	/* [한국어] 표가 HT 터널 변환을 켜라고 했는가 */
		iommu_feature_enable(iommu, CONTROL_HT_TUN_EN) :	/* [한국어] 켜거나 */
		iommu_feature_disable(iommu, CONTROL_HT_TUN_EN);	/* [한국어] 명시적으로 끈다 — 앞선 부팅이 켜 둔 상태가 남을 수 있다 */

	iommu->acpi_flags & IVHD_FLAG_PASSPW_EN_MASK ?	/* [한국어] posted write 통과를 켜라고 했는가 */
		iommu_feature_enable(iommu, CONTROL_PASSPW_EN) :	/* [한국어] 켜거나 */
		iommu_feature_disable(iommu, CONTROL_PASSPW_EN);	/* [한국어] 끈다 */

	iommu->acpi_flags & IVHD_FLAG_RESPASSPW_EN_MASK ?	/* [한국어] 응답 통과는 */
		iommu_feature_enable(iommu, CONTROL_RESPASSPW_EN) :	/* [한국어] 켜거나 */
		iommu_feature_disable(iommu, CONTROL_RESPASSPW_EN);	/* [한국어] 끈다 */

	iommu->acpi_flags & IVHD_FLAG_ISOC_EN_MASK ?	/* [한국어] 등시성 트래픽 처리는 */
		iommu_feature_enable(iommu, CONTROL_ISOC_EN) :	/* [한국어] 켜거나 */
		iommu_feature_disable(iommu, CONTROL_ISOC_EN);	/* [한국어] 끈다 */

	/*
	 * make IOMMU memory accesses cache coherent
	 */
	iommu_feature_enable(iommu, CONTROL_COHERENT_EN);	/* [한국어] (원 주석: IOMMU 의 메모리 접근을 캐시 코히런트하게) 표를 고칠 때마다 캐시를 밀어내지 않아도 된다 */

	/* Set IOTLB invalidation timeout to 1s */
	iommu_feature_set(iommu, CTRL_INV_TO_1S, CTRL_INV_TO_MASK, CONTROL_INV_TIMEOUT);	/* [한국어] (원 주석: 무효화 타임아웃 1초) 응답 없는 장치가 시스템을 영원히 붙잡지 못하게 */

	/* Enable Enhanced Peripheral Page Request Handling */
	if (check_feature(FEATURE_EPHSUP))	/* [한국어] (원 주석: 향상된 PPR 처리) */
		iommu_feature_enable(iommu, CONTROL_EPH_EN);	/* [한국어] SVA 의 응답 프로토콜이 이것을 요구한다 */
}

/*
 * [한국어]
 * iommu_apply_resume_quirks - RD890 에서 레주메 뒤 레지스터를 손수 복원한다
 *
 * @iommu: 대상 유닛.
 *
 * 원 주석이 사정을 밝힌다: RD890 의 BIOS 는 레주메 때 IOMMU 를 완전히
 * 재구성해 주지 않는다. 그래서 서스펜드 전에 iommu_init_pci 가 읽어 둔
 * 값들(stored_addr, stored_l1, stored_l2)을 여기서 되쓴다.
 *
 * 순서가 정해져 있다.
 *  1) 노스브리지의 레지스터로 IOMMU 자체를 켠다. 이것이 꺼져 있으면 아래
 *     설정을 아무리 써도 동작하지 않는다.
 *  2) BAR 을 복원한다 — MMIO 주소를 되찾는 것이 먼저다.
 *  3) L1/L2 간접 레지스터를 전부 되쓴다.
 *  4) 마지막에 BAR 의 최하위 비트를 세워 설정 공간 쓰기를 잠근다. 복원이
 *     끝난 뒤 다른 코드가 실수로 바꾸는 것을 막는다.
 *
 * 잠금 비트를 저장할 때 지워 두었던 것(iommu_init_pci 참고)이 여기서
 * 짝을 이룬다 — 잠긴 값을 그대로 되쓰면 복원 도중에 잠겨 버린다.
 *
 * 호출 체인:
 *   amd_iommu_resume() → [이 함수] → iommu_write_l1()/iommu_write_l2()
 */
static void iommu_apply_resume_quirks(struct amd_iommu *iommu)
{
	int i, j;	/* [한국어] 간접 레지스터 순회 인덱스 */
	u32 ioc_feature_control;	/* [한국어] 노스브리지의 IOMMU 활성화 레지스터 */
	struct pci_dev *pdev = iommu->root_pdev;	/* [한국어] 미리 잡아 둔 루트 장치 */

	/* RD890 BIOSes may not have completely reconfigured the iommu */
	if (!is_rd890_iommu(iommu->dev) || !pdev)	/* [한국어] (원 주석: RD890 BIOS 가 IOMMU 를 완전히 재구성하지 않을 수 있다) */
		return;	/* [한국어] 다른 칩셋은 BIOS 가 알아서 한다 */

	/*
	 * First, we need to ensure that the iommu is enabled. This is
	 * controlled by a register in the northbridge
	 */

	/* Select Northbridge indirect register 0x75 and enable writing */
	pci_write_config_dword(pdev, 0x60, 0x75 | (1 << 7));	/* [한국어] (원 주석: 노스브리지 간접 레지스터 0x75 를 선택하고 쓰기를 허용한다) */
	pci_read_config_dword(pdev, 0x64, &ioc_feature_control);	/* [한국어] 현재 값 */

	/* Enable the iommu */
	if (!(ioc_feature_control & 0x1))	/* [한국어] (원 주석: IOMMU 를 켠다) 이것이 꺼져 있으면 아래 설정이 무의미하다 */
		pci_write_config_dword(pdev, 0x64, ioc_feature_control | 1);	/* [한국어] 활성화 비트를 세운다 */

	/* Restore the iommu BAR */
	pci_write_config_dword(iommu->dev, iommu->cap_ptr + 4,	/* [한국어] (원 주석: IOMMU BAR 복원) MMIO 주소를 먼저 되찾아야 한다 */
			       iommu->stored_addr_lo);	/* [한국어] 저장해 둔 하위 */
	pci_write_config_dword(iommu->dev, iommu->cap_ptr + 8,	/* [한국어] 상위도 */
			       iommu->stored_addr_hi);	/* [한국어] 복원 */

	/* Restore the l1 indirect regs for each of the 6 l1s */
	for (i = 0; i < 6; i++)	/* [한국어] (원 주석: 여섯 개 L1 각각의 간접 레지스터 복원) */
		for (j = 0; j < 0x12; j++)	/* [한국어] 블록마다 0x12 개 */
			iommu_write_l1(iommu, i, j, iommu->stored_l1[i][j]);	/* [한국어] 서스펜드 전에 읽어 둔 값을 되쓴다 */

	/* Restore the l2 indirect regs */
	for (i = 0; i < 0x83; i++)	/* [한국어] (원 주석: L2 간접 레지스터 복원) */
		iommu_write_l2(iommu, i, iommu->stored_l2[i]);	/* [한국어] 같은 방식 */

	/* Lock PCI setup registers */
	pci_write_config_dword(iommu->dev, iommu->cap_ptr + 4,	/* [한국어] (원 주석: PCI 설정 레지스터를 잠근다) */
			       iommu->stored_addr_lo | 1);	/* [한국어] 저장할 때 지워 두었던 잠금 비트를 이제 세운다 — 먼저 세우면 복원 도중에 잠긴다 */
}

/*
 * [한국어]
 * iommu_enable_ga - 게스트 APIC 을 켜고 IRTE 형식을 고른다
 *
 * @iommu: 대상 유닛.
 *
 * 두 가지를 함께 정하는 것이 이 함수의 요점이다. GA 를 켜는 것과 128비트
 * IRTE 형식을 쓰는 것은 짝을 이룬다 — 게스트에 직접 전달하려면 항목에
 * 게스트 정보를 담을 자리가 필요하고, 32비트로는 부족하다.
 *
 * irte_ops 를 여기서 정하므로, 이후 모든 인터럽트 조작이 형식에 맞는
 * 함수를 쓰게 된다. LEGACY_GA 가 VAPIC 과 같은 처리를 받는 것이 눈에
 * 띄는데, 그 모드는 형식은 유지하고 게스트 전달만 끈 상태이기 때문이다.
 *
 * 호출 체인:
 *   early_enable_iommu()/early_enable_iommus() → [이 함수]
 */
static void iommu_enable_ga(struct amd_iommu *iommu)
{
#ifdef CONFIG_IRQ_REMAP
	switch (amd_iommu_guest_ir) {	/* [한국어] 인터럽트 전달 모드에 따라 */
	case AMD_IOMMU_GUEST_IR_VAPIC:	/* [한국어] 게스트 직접 전달 */
	case AMD_IOMMU_GUEST_IR_LEGACY_GA:	/* [한국어] 형식만 유지하고 전달은 끈 상태 — 둘 다 128비트를 쓴다 */
		iommu_feature_enable(iommu, CONTROL_GA_EN);	/* [한국어] 게스트 APIC 을 켠다 */
		iommu->irte_ops = &irte_128_ops;	/* [한국어] 이후 모든 인터럽트 조작이 128비트 형식 함수를 쓴다 */
		break;	/* [한국어] 결정 */
	default:	/* [한국어] 그 밖(레거시) */
		iommu->irte_ops = &irte_32_ops;	/* [한국어] 32비트 형식 함수 */
		break;
	}
#endif
}

/*
 * [한국어]
 * iommu_disable_irtcachedis - 인터럽트 재매핑 캐시를 다시 켠다
 *
 * @iommu: 대상 유닛.
 *
 * 이름이 이중 부정이라 헷갈린다. IRTCACHEDIS 는 "캐시 비활성화" 비트이므로,
 * 그것을 disable 한다는 것은 캐시를 켠다는 뜻이다.
 *
 * kdump 물려받기 경로에서 쓴다 — 옛 커널이 켜 둔 설정을 지우고 알려진
 * 상태에서 다시 시작하기 위해서다.
 */
static void iommu_disable_irtcachedis(struct amd_iommu *iommu)
{
	iommu_feature_disable(iommu, CONTROL_IRTCACHEDIS);	/* [한국어] 이중 부정 — "캐시 비활성화"를 끄므로 캐시가 켜진다 */
}

/*
 * [한국어]
 * iommu_enable_irtcachedis - 요청이 있으면 재매핑 캐시를 끈다
 *
 * @iommu: 대상 유닛.
 *
 * 원 주석이 이 함수의 요령을 밝힌다: 이 기능을 지원하는지 알아내는 방법이
 * "비트를 써 보고 남아 있는지 확인하는 것"뿐이다. 능력 레지스터에 그것을
 * 알리는 비트가 없기 때문이다.
 *
 * 그래서 켜 보고 다시 읽어, 값이 유지되면 지원하는 것으로 판단한다.
 * 지원하지 않는 하드웨어에서는 쓰기가 무시되어 0 으로 읽힌다.
 *
 * 왜 캐시를 끄고 싶은가: 특정 하드웨어의 errata 를 피하기 위해서다. 캐시가
 * 없으면 인터럽트마다 표를 다시 읽어 느려지지만, 잘못된 항목을 재사용하는
 * 것보다 낫다.
 *
 * 결과를 항상 로그에 남기는 이유: 요청했는데 안 된 경우를 사용자가 알아야
 * 한다.
 *
 * 호출 체인:
 *   early_enable_iommu()/early_enable_iommus() → [이 함수]
 */
static void iommu_enable_irtcachedis(struct amd_iommu *iommu)
{
	u64 ctrl;	/* [한국어] 다시 읽은 제어 레지스터 */

	if (!amd_iommu_irtcachedis)	/* [한국어] 사용자가 요청하지 않았으면 */
		return;	/* [한국어] 캐시를 그대로 둔다 */

	/*
	 * Note:
	 * The support for IRTCacheDis feature is dertermined by
	 * checking if the bit is writable.
	 */
	iommu_feature_enable(iommu, CONTROL_IRTCACHEDIS);	/* [한국어] (원 주석: 이 기능의 지원 여부는 비트가 쓰이는지로 판별한다) */
	ctrl = readq(iommu->mmio_base +  MMIO_CONTROL_OFFSET);	/* [한국어] 써 보고 다시 읽어 */
	ctrl &= (1ULL << CONTROL_IRTCACHEDIS);	/* [한국어] 그 비트만 남긴다 */
	if (ctrl)	/* [한국어] 값이 유지됐으면 지원하는 하드웨어다 */
		iommu->irtcachedis_enabled = true;	/* [한국어] 실제로 꺼졌음을 기록 */
	pr_info("iommu%d (%#06x) : IRT cache is %s\n",	/* [한국어] 요청했는데 안 된 경우를 사용자가 알 수 있게 항상 남긴다 */
		iommu->index, iommu->devid,
		iommu->irtcachedis_enabled ? "disabled" : "enabled");
}

/*
 * [한국어]
 * iommu_enable_2k_int - 장치당 인터럽트 재매핑 항목을 2048개로 늘린다
 *
 * @iommu: 대상 유닛.
 *
 * 기본은 512개다. MSI-X 벡터를 많이 쓰는 장치(고성능 NIC 등)가 그 한계에
 * 닿을 수 있어, 하드웨어가 지원하면 네 배로 늘린다.
 *
 * 표가 네 배로 커지는 대가가 있지만, 표는 장치가 인터럽트를 실제로 쓸 때만
 * 만들어지므로 대부분의 장치에는 영향이 없다.
 *
 * 호출 체인:
 *   early_enable_iommu()/early_enable_iommus() → [이 함수]
 */
static void iommu_enable_2k_int(struct amd_iommu *iommu)
{
	if (!FEATURE_NUM_INT_REMAP_SUP_2K(amd_iommu_efr2))	/* [한국어] 하드웨어가 2048개를 지원하지 않으면 */
		return;	/* [한국어] 기본 512개를 쓴다 */

	iommu_feature_set(iommu,	/* [한국어] MSI-X 벡터를 많이 쓰는 장치를 위해 */
			  CONTROL_NUM_INT_REMAP_MODE_2K,	/* [한국어] 네 배로 늘린다 */
			  CONTROL_NUM_INT_REMAP_MODE_MASK,	/* [한국어] 그 필드의 폭 */
			  CONTROL_NUM_INT_REMAP_MODE);	/* [한국어] 필드의 위치 */
}

/*
 * [한국어]
 * early_enable_iommu - 유닛 하나를 알려진 상태에서 출발시켜 켠다
 *
 * @iommu: 대상 유닛.
 *
 * 순서가 이 함수의 전부이고, 그 순서에는 이유가 있다.
 *
 * 맨 앞의 iommu_disable 이 눈에 띈다. 켜려는 함수가 끄기로 시작하는 이유는
 * 앞선 부팅이나 펌웨어가 남긴 설정을 지우기 위해서다 — 알려지지 않은
 * 상태 위에 설정을 얹으면 무엇이 켜져 있는지 알 수 없다.
 *
 * 그다음 자료구조를 하드웨어에 알린다: 장치 테이블, 명령 버퍼, 이벤트
 * 버퍼, 제외 범위. 이것들이 모두 준비되어야 켤 수 있다.
 *
 * 기능들(GT/GA/XT/IRT 캐시/2K 인터럽트)을 켜는 것이 그다음이고, 마지막이
 * IOMMU 본체다. 이 한 줄부터 모든 DMA 가 변환을 거친다.
 *
 * 캐시 비우기로 끝나는 이유: 앞선 설정 전부가 실제로 반영되도록 보장한다.
 *
 * 호출 체인:
 *   early_enable_iommus() → [이 함수]
 */
static void early_enable_iommu(struct amd_iommu *iommu)
{
	iommu_disable(iommu);	/* [한국어] 켜기 전에 먼저 끈다 — 앞선 부팅이나 펌웨어가 남긴 설정을 지우기 위해서다 */
	iommu_init_flags(iommu);	/* [한국어] 표가 권장한 설정과 드라이버가 요구하는 설정 */
	iommu_set_device_table(iommu);	/* [한국어] 변환 사슬의 출발점을 알린다 */
	iommu_enable_command_buffer(iommu);	/* [한국어] 무효화를 보낼 수단 */
	iommu_enable_event_buffer(iommu);	/* [한국어] 오류를 보고받을 수단 */
	iommu_set_exclusion_range(iommu);	/* [한국어] 펌웨어가 요구한 통과 구간 */
	iommu_enable_gt(iommu);	/* [한국어] 게스트 변환(PASID 별 변환) */
	iommu_enable_ga(iommu);	/* [한국어] 게스트 APIC 과 IRTE 형식 */
	iommu_enable_xt(iommu);	/* [한국어] x2APIC 목적지 */
	iommu_enable_irtcachedis(iommu);	/* [한국어] 요청이 있으면 재매핑 캐시를 끈다 */
	iommu_enable_2k_int(iommu);	/* [한국어] 인터럽트 항목 수를 늘린다 */
	iommu_enable(iommu);	/* [한국어] 마지막에 본체를 켠다. 이 줄부터 모든 DMA 가 변환을 거친다 */
	amd_iommu_flush_all_caches(iommu);	/* [한국어] 앞선 설정이 모두 반영되도록 캐시를 비운다 */
}

/*
 * This function finally enables all IOMMUs found in the system after
 * they have been initialized.
 *
 * Or if in kdump kernel and IOMMUs are all pre-enabled, try to reuse
 * the old content of device table entries. Not this case or reuse failed,
 * just continue as normal kernel does.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * early_enable_iommus - 모든 유닛을 켠다. kdump 면 옛 표를 물려받아 켠다
 *
 * 두 갈래가 완전히 다른 절차라는 것이 이 함수의 핵심이다.
 *
 * 물려받기에 실패한 경우(또는 kdump 가 아닌 경우): 각 유닛을 끄고 새 표로
 * 처음부터 켠다. 그 과정에서 진행 중이던 DMA 는 끊긴다.
 *
 * 물려받기에 성공한 경우: 하드웨어를 끄지 않는다. 새로 잡은 표를 버리고
 * 옛 표를 그대로 쓰며, 명령/이벤트 버퍼만 껐다 켠다. 장치 테이블 주소는
 * 바뀌지 않으므로(kdump 에서 iommu_set_device_table 이 아무것도 하지 않는다)
 * 살아 있는 장치의 DMA 가 끊기지 않는다.
 *
 * SNP 에서 BUG_ON 을 거는 이유가 강하다. 원 주석대로, SNP 환경에서 장치
 * 테이블 없이 진행하면 IOMMU 명령이 전부 타임아웃되어 kdump 부팅 자체가
 * 패닉으로 끝난다. 그럴 바에는 여기서 분명히 멈추는 편이 낫다.
 *
 * 실패 경로에서 old_dev_tbl_cpy 를 놓는 것도 중요하다 — 물려받기를 포기한
 * 이상 그 매핑을 들고 있을 이유가 없다.
 *
 * 호출 체인:
 *   enable_iommus() → [이 함수] → reuse_device_table() → early_enable_iommu()
 */
static void early_enable_iommus(void)
{
	struct amd_iommu *iommu;	/* [한국어] 유닛 순회용 */
	struct amd_iommu_pci_seg *pci_seg;	/* [한국어] 세그먼트 순회용 */

	if (!reuse_device_table()) {	/* [한국어] 옛 표를 물려받지 못했거나 kdump 가 아니다 */
		/*
		 * If come here because of failure in reusing device table from old
		 * kernel with all IOMMUs enabled, print error message and try to
		 * free allocated old_dev_tbl_cpy.
		 */
		if (amd_iommu_pre_enabled) {	/* [한국어] (원 주석: 모든 IOMMU 가 켜진 옛 커널에서 표 재사용에 실패한 경우) */
			pr_err("Failed to reuse DEV table from previous kernel.\n");	/* [한국어] 물려받으려 했는데 실패했다 */
			/*
			 * Bail out early if unable to remap/reuse DEV table from
			 * previous kernel if SNP enabled as IOMMU commands will
			 * time out without DEV table and cause kdump boot panic.
			 */
			BUG_ON(check_feature(FEATURE_SNP));	/* [한국어] (원 주석: SNP 에서는 장치 테이블 없이 진행하면 명령이 전부 타임아웃되어 kdump 부팅이 패닉으로 끝난다) */
		}

		for_each_pci_segment(pci_seg) {	/* [한국어] 물려받기를 포기했으므로 */
			if (pci_seg->old_dev_tbl_cpy != NULL) {	/* [한국어] 매핑해 둔 옛 표가 있으면 */
				memunmap((void *)pci_seg->old_dev_tbl_cpy);	/* [한국어] 들고 있을 이유가 없다 */
				pci_seg->old_dev_tbl_cpy = NULL;	/* [한국어] 포인터도 지운다 */
			}
		}

		for_each_iommu(iommu) {	/* [한국어] 모든 유닛을 */
			clear_translation_pre_enabled(iommu);	/* [한국어] 물려받음 표시를 지우고 */
			early_enable_iommu(iommu);	/* [한국어] 끄고 새 표로 처음부터 켠다. 진행 중이던 DMA 는 끊긴다 */
		}
	} else {
		pr_info("Reused DEV table from previous kernel.\n");	/* [한국어] 물려받기 성공 */

		for_each_pci_segment(pci_seg) {	/* [한국어] 새로 잡았던 표는 */
			iommu_free_pages(pci_seg->dev_table);	/* [한국어] 놓고 */
			pci_seg->dev_table = pci_seg->old_dev_tbl_cpy;	/* [한국어] 옛 표를 그대로 쓴다 */
		}

		for_each_iommu(iommu) {	/* [한국어] 하드웨어는 끄지 않는다 — 그것이 물려받기의 핵심이다 */
			iommu_disable_command_buffer(iommu);	/* [한국어] 버퍼만 껐다가 */
			iommu_disable_event_buffer(iommu);	/* [한국어] 이벤트 버퍼도 */
			iommu_disable_irtcachedis(iommu);	/* [한국어] 캐시 설정도 알려진 상태로 되돌리고 */
			iommu_enable_command_buffer(iommu);	/* [한국어] 다시 켠다. 주소는 옛 것 그대로다 */
			iommu_enable_event_buffer(iommu);	/* [한국어] 이벤트 버퍼도 */
			iommu_enable_ga(iommu);	/* [한국어] 게스트 APIC 과 IRTE 형식 */
			iommu_enable_xt(iommu);	/* [한국어] x2APIC 목적지 */
			iommu_enable_irtcachedis(iommu);	/* [한국어] 재매핑 캐시 설정 */
			iommu_enable_2k_int(iommu);	/* [한국어] 인터럽트 항목 수 */
			iommu_set_device_table(iommu);	/* [한국어] kdump 에서는 아무것도 하지 않는다 — 주소를 바꾸면 전환이 원자적이지 않다 */
			amd_iommu_flush_all_caches(iommu);	/* [한국어] 설정을 반영시킨다 */
		}
	}
}

/*
 * [한국어]
 * enable_iommus_ppr - 모든 유닛의 PPR 로그를 켠다
 *
 * SVA 에 필요한 세 조건이 모두 갖춰졌을 때만 켠다. 하나라도 없으면 페이지
 * 폴트를 받아도 처리할 수 없으므로, 아예 받지 않는 편이 낫다 — 받고
 * 응답하지 않으면 장치가 멈춘다.
 *
 * 호출 체인:
 *   amd_iommu_enable_interrupts() → [이 함수] → amd_iommu_enable_ppr_log()
 */
static void enable_iommus_ppr(void)
{
	struct amd_iommu *iommu;	/* [한국어] 유닛 순회용 */

	if (!amd_iommu_gt_ppr_supported())	/* [한국어] SVA 의 세 조건이 갖춰지지 않았으면 */
		return;	/* [한국어] 폴트를 받아도 처리할 수 없다. 받고 응답하지 않으면 장치가 멈춘다 */

	for_each_iommu(iommu)	/* [한국어] 모든 유닛의 */
		amd_iommu_enable_ppr_log(iommu);	/* [한국어] PPR 로그를 켠다 */
}

static void enable_iommus_vapic(void)
{
#ifdef CONFIG_IRQ_REMAP
	u32 status, i;
	struct amd_iommu *iommu;

	for_each_iommu(iommu) {
		/*
		 * Disable GALog if already running. It could have been enabled
		 * in the previous boot before kdump.
		 */
		status = readl(iommu->mmio_base + MMIO_STATUS_OFFSET);
		if (!(status & MMIO_STATUS_GALOG_RUN_MASK))
			continue;

		iommu_feature_disable(iommu, CONTROL_GALOG_EN);
		iommu_feature_disable(iommu, CONTROL_GAINT_EN);

		/*
		 * Need to set and poll check the GALOGRun bit to zero before
		 * we can set/ modify GA Log registers safely.
		 */
		for (i = 0; i < MMIO_STATUS_TIMEOUT; ++i) {
			status = readl(iommu->mmio_base + MMIO_STATUS_OFFSET);
			if (!(status & MMIO_STATUS_GALOG_RUN_MASK))
				break;
			udelay(10);
		}

		if (WARN_ON(i >= MMIO_STATUS_TIMEOUT))
			return;
	}

	if (AMD_IOMMU_GUEST_IR_VAPIC(amd_iommu_guest_ir) &&
	    !check_feature(FEATURE_GAM_VAPIC)) {
		amd_iommu_guest_ir = AMD_IOMMU_GUEST_IR_LEGACY_GA;
		return;
	}

	if (amd_iommu_snp_en &&
	    !FEATURE_SNPAVICSUP_GAM(amd_iommu_efr2)) {
		pr_warn("Force to disable Virtual APIC due to SNP\n");
		amd_iommu_guest_ir = AMD_IOMMU_GUEST_IR_LEGACY_GA;
		return;
	}

	/* Enabling GAM and SNPAVIC support */
	for_each_iommu(iommu) {
		if (iommu_init_ga_log(iommu) ||
		    iommu_ga_log_enable(iommu))
			return;

		iommu_feature_enable(iommu, CONTROL_GAM_EN);
		if (amd_iommu_snp_en)
			iommu_feature_enable(iommu, CONTROL_SNPAVIC_EN);
	}

	amd_iommu_irq_ops.capability |= (1 << IRQ_POSTING_CAP);
	pr_info("Virtual APIC enabled\n");
#endif
}

static void disable_iommus(void)
{
	struct amd_iommu *iommu;

	for_each_iommu(iommu)
		iommu_disable(iommu);

#ifdef CONFIG_IRQ_REMAP
	if (AMD_IOMMU_GUEST_IR_VAPIC(amd_iommu_guest_ir))
		amd_iommu_irq_ops.capability &= ~(1 << IRQ_POSTING_CAP);
#endif
}

/*
 * Suspend/Resume support
 * disable suspend until real resume implemented
 */

static void amd_iommu_resume(void *data)
{
	struct amd_iommu *iommu;

	for_each_iommu(iommu)
		iommu_apply_resume_quirks(iommu);

	/* re-load the hardware */
	for_each_iommu(iommu)
		early_enable_iommu(iommu);

	amd_iommu_enable_interrupts();
}

static int amd_iommu_suspend(void *data)
{
	/* disable IOMMUs to go out of the way for BIOS */
	disable_iommus();

	return 0;
}

static const struct syscore_ops amd_iommu_syscore_ops = {
	.suspend = amd_iommu_suspend,
	.resume = amd_iommu_resume,
};

static struct syscore amd_iommu_syscore = {
	.ops = &amd_iommu_syscore_ops,
};

static void __init free_iommu_resources(void)
{
	free_iommu_all();
	free_pci_segments();
}

/* SB IOAPIC is always on this device in AMD systems */
#define IOAPIC_SB_DEVID		((0x00 << 8) | PCI_DEVFN(0x14, 0))

static bool __init check_ioapic_information(void)
{
	const char *fw_bug = FW_BUG;
	bool ret, has_sb_ioapic;
	int idx;

	has_sb_ioapic = false;
	ret           = false;

	/*
	 * If we have map overrides on the kernel command line the
	 * messages in this function might not describe firmware bugs
	 * anymore - so be careful
	 */
	if (cmdline_maps)
		fw_bug = "";

	for (idx = 0; idx < nr_ioapics; idx++) {
		int devid, id = mpc_ioapic_id(idx);

		devid = get_ioapic_devid(id);
		if (devid < 0) {
			pr_err("%s: IOAPIC[%d] not in IVRS table\n",
				fw_bug, id);
			ret = false;
		} else if (devid == IOAPIC_SB_DEVID) {
			has_sb_ioapic = true;
			ret           = true;
		}
	}

	if (!has_sb_ioapic) {
		/*
		 * We expect the SB IOAPIC to be listed in the IVRS
		 * table. The system timer is connected to the SB IOAPIC
		 * and if we don't have it in the list the system will
		 * panic at boot time.  This situation usually happens
		 * when the BIOS is buggy and provides us the wrong
		 * device id for the IOAPIC in the system.
		 */
		pr_err("%s: No southbridge IOAPIC found\n", fw_bug);
	}

	if (!ret)
		pr_err("Disabling interrupt remapping\n");

	return ret;
}

static void __init free_dma_resources(void)
{
	amd_iommu_pdom_id_destroy();
	free_unity_maps();
}

static void __init ivinfo_init(void *ivrs)
{
	amd_iommu_ivinfo = *((u32 *)(ivrs + IOMMU_IVINFO_OFFSET));
}

/*
 * This is the hardware init function for AMD IOMMU in the system.
 * This function is called either from amd_iommu_init or from the interrupt
 * remapping setup code.
 *
 * This function basically parses the ACPI table for AMD IOMMU (IVRS)
 * four times:
 *
 *	1 pass) Discover the most comprehensive IVHD type to use.
 *
 *	2 pass) Find the highest PCI device id the driver has to handle.
 *		Upon this information the size of the data structures is
 *		determined that needs to be allocated.
 *
 *	3 pass) Initialize the data structures just allocated with the
 *		information in the ACPI table about available AMD IOMMUs
 *		in the system. It also maps the PCI devices in the
 *		system to specific IOMMUs
 *
 *	4 pass) After the basic data structures are allocated and
 *		initialized we update them with information about memory
 *		remapping requirements parsed out of the ACPI table in
 *		this last pass.
 *
 * After everything is set up the IOMMUs are enabled and the necessary
 * hotplug and suspend notifiers are registered.
 */
static int __init early_amd_iommu_init(void)
{
	struct acpi_table_header *ivrs_base;
	int ret;
	acpi_status status;
	u8 efr_hats;

	if (!amd_iommu_detected)
		return -ENODEV;

	status = acpi_get_table("IVRS", 0, &ivrs_base);
	if (status == AE_NOT_FOUND)
		return -ENODEV;
	else if (ACPI_FAILURE(status)) {
		const char *err = acpi_format_exception(status);
		pr_err("IVRS table error: %s\n", err);
		return -EINVAL;
	}

	if (!boot_cpu_has(X86_FEATURE_CX16)) {
		pr_err("Failed to initialize. The CMPXCHG16B feature is required.\n");
		ret = -EINVAL;
		goto out;
	}

	/*
	 * Validate checksum here so we don't need to do it when
	 * we actually parse the table
	 */
	ret = check_ivrs_checksum(ivrs_base);
	if (ret)
		goto out;

	ivinfo_init(ivrs_base);

	amd_iommu_target_ivhd_type = get_highest_supported_ivhd_type(ivrs_base);
	DUMP_printk("Using IVHD type %#x\n", amd_iommu_target_ivhd_type);

	/*
	 * now the data structures are allocated and basically initialized
	 * start the real acpi table scan
	 */
	ret = init_iommu_all(ivrs_base);
	if (ret)
		goto out;

	/* 5 level guest page table */
	if (cpu_feature_enabled(X86_FEATURE_LA57) &&
	    FIELD_GET(FEATURE_GATS, amd_iommu_efr) == GUEST_PGTABLE_5_LEVEL)
		amd_iommu_gpt_level = PAGE_MODE_5_LEVEL;

	efr_hats = FIELD_GET(FEATURE_HATS, amd_iommu_efr);
	if (efr_hats != 0x3) {
		/*
		 * efr[HATS] bits specify the maximum host translation level
		 * supported, with LEVEL 4 being initial max level.
		 */
		amd_iommu_hpt_level = efr_hats + PAGE_MODE_4_LEVEL;
	} else {
		pr_warn_once(FW_BUG "Disable host address translation due to invalid translation level (%#x).\n",
			     efr_hats);
		amd_iommu_hatdis = true;
	}

	if (amd_iommu_pgtable == PD_MODE_V2) {
		if (!amd_iommu_v2_pgtbl_supported()) {
			pr_warn("Cannot enable v2 page table for DMA-API. Fallback to v1.\n");
			amd_iommu_pgtable = PD_MODE_V1;
		}
	}

	if (amd_iommu_hatdis) {
		/*
		 * Host (v1) page table is not available. Attempt to use
		 * Guest (v2) page table.
		 */
		if (amd_iommu_v2_pgtbl_supported())
			amd_iommu_pgtable = PD_MODE_V2;
		else
			amd_iommu_pgtable = PD_MODE_NONE;
	}

	/* Disable any previously enabled IOMMUs */
	if (!is_kdump_kernel() || amd_iommu_disabled)
		disable_iommus();

	if (amd_iommu_irq_remap)
		amd_iommu_irq_remap = check_ioapic_information();

	if (amd_iommu_irq_remap) {
		struct amd_iommu_pci_seg *pci_seg;
		ret = -ENOMEM;
		for_each_pci_segment(pci_seg) {
			if (alloc_irq_lookup_table(pci_seg))
				goto out;
		}
	}

	ret = init_memory_definitions(ivrs_base);
	if (ret)
		goto out;

	/* init the device table */
	init_device_table();

out:
	/* Don't leak any ACPI memory */
	acpi_put_table(ivrs_base);

	return ret;
}

static int amd_iommu_enable_interrupts(void)
{
	struct amd_iommu *iommu;
	int ret = 0;

	for_each_iommu(iommu) {
		ret = iommu_init_irq(iommu);
		if (ret)
			goto out;
	}

	/*
	 * Interrupt handler is ready to process interrupts. Enable
	 * PPR and GA log interrupt for all IOMMUs.
	 */
	enable_iommus_vapic();
	enable_iommus_ppr();

out:
	return ret;
}

static bool __init detect_ivrs(void)
{
	struct acpi_table_header *ivrs_base;
	acpi_status status;
	int i;

	status = acpi_get_table("IVRS", 0, &ivrs_base);
	if (status == AE_NOT_FOUND)
		return false;
	else if (ACPI_FAILURE(status)) {
		const char *err = acpi_format_exception(status);
		pr_err("IVRS table error: %s\n", err);
		return false;
	}

	acpi_put_table(ivrs_base);

	if (amd_iommu_force_enable)
		goto out;

	/* Don't use IOMMU if there is Stoney Ridge graphics */
	for (i = 0; i < 32; i++) {
		u32 pci_id;

		pci_id = read_pci_config(0, i, 0, 0);
		if ((pci_id & 0xffff) == 0x1002 && (pci_id >> 16) == 0x98e4) {
			pr_info("Disable IOMMU on Stoney Ridge\n");
			return false;
		}
	}

out:
	/* Make sure ACS will be enabled during PCI probe */
	pci_request_acs();

	return true;
}

static __init void iommu_snp_enable(void)
{
#ifdef CONFIG_KVM_AMD_SEV
	if (!cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		return;
	/*
	 * The SNP support requires that IOMMU must be enabled, and is
	 * configured with V1 page table (DTE[Mode] = 0 is not supported).
	 */
	if (no_iommu || iommu_default_passthrough()) {
		pr_warn("SNP: IOMMU disabled or configured in passthrough mode, SNP cannot be supported.\n");
		goto disable_snp;
	}

	if (amd_iommu_pgtable != PD_MODE_V1) {
		pr_warn("SNP: IOMMU is configured with V2 page table mode, SNP cannot be supported.\n");
		goto disable_snp;
	}

	amd_iommu_snp_en = check_feature(FEATURE_SNP);
	if (!amd_iommu_snp_en) {
		pr_warn("SNP: IOMMU SNP feature not enabled, SNP cannot be supported.\n");
		goto disable_snp;
	}

	/*
	 * Enable host SNP support once SNP support is checked on IOMMU.
	 */
	if (snp_rmptable_init()) {
		pr_warn("SNP: RMP initialization failed, SNP cannot be supported.\n");
		goto disable_snp;
	}

	pr_info("IOMMU SNP support enabled.\n");
	return;

disable_snp:
	cc_platform_clear(CC_ATTR_HOST_SEV_SNP);
#endif
}

/****************************************************************************
 *
 * AMD IOMMU Initialization State Machine
 *
 ****************************************************************************/

static int __init state_next(void)
{
	int ret = 0;

	switch (init_state) {
	case IOMMU_START_STATE:
		if (!detect_ivrs()) {
			init_state	= IOMMU_NOT_FOUND;
			ret		= -ENODEV;
		} else {
			init_state	= IOMMU_IVRS_DETECTED;
		}
		break;
	case IOMMU_IVRS_DETECTED:
		if (amd_iommu_disabled) {
			init_state = IOMMU_CMDLINE_DISABLED;
			ret = -EINVAL;
		} else {
			ret = early_amd_iommu_init();
			init_state = ret ? IOMMU_INIT_ERROR : IOMMU_ACPI_FINISHED;
		}
		break;
	case IOMMU_ACPI_FINISHED:
		early_enable_iommus();
		x86_platform.iommu_shutdown = disable_iommus;
		init_state = IOMMU_ENABLED;
		break;
	case IOMMU_ENABLED:
		register_syscore(&amd_iommu_syscore);
		iommu_snp_enable();
		ret = amd_iommu_init_pci();
		init_state = ret ? IOMMU_INIT_ERROR : IOMMU_PCI_INIT;
		break;
	case IOMMU_PCI_INIT:
		ret = amd_iommu_enable_interrupts();
		init_state = ret ? IOMMU_INIT_ERROR : IOMMU_INTERRUPTS_EN;
		break;
	case IOMMU_INTERRUPTS_EN:
		init_state = IOMMU_INITIALIZED;
		break;
	case IOMMU_INITIALIZED:
		/* Nothing to do */
		break;
	case IOMMU_NOT_FOUND:
	case IOMMU_INIT_ERROR:
	case IOMMU_CMDLINE_DISABLED:
		/* Error states => do nothing */
		ret = -EINVAL;
		break;
	default:
		/* Unknown state */
		BUG();
	}

	if (ret) {
		free_dma_resources();
		if (!irq_remapping_enabled) {
			disable_iommus();
			free_iommu_resources();
		} else {
			struct amd_iommu *iommu;
			struct amd_iommu_pci_seg *pci_seg;

			for_each_pci_segment(pci_seg)
				uninit_device_table_dma(pci_seg);

			for_each_iommu(iommu)
				amd_iommu_flush_all_caches(iommu);
		}
	}
	return ret;
}

static int __init iommu_go_to_state(enum iommu_init_state state)
{
	int ret = -EINVAL;

	while (init_state != state) {
		if (init_state == IOMMU_NOT_FOUND         ||
		    init_state == IOMMU_INIT_ERROR        ||
		    init_state == IOMMU_CMDLINE_DISABLED)
			break;
		ret = state_next();
	}

	/*
	 * SNP platform initilazation requires IOMMUs to be fully configured.
	 * If the SNP support on IOMMUs has NOT been checked, simply mark SNP
	 * as unsupported. If the SNP support on IOMMUs has been checked and
	 * host SNP support enabled but RMP enforcement has not been enabled
	 * in IOMMUs, then the system is in a half-baked state, but can limp
	 * along as all memory should be Hypervisor-Owned in the RMP. WARN,
	 * but leave SNP as "supported" to avoid confusing the kernel.
	 */
	if (ret && cc_platform_has(CC_ATTR_HOST_SEV_SNP) &&
	    !WARN_ON_ONCE(amd_iommu_snp_en))
		cc_platform_clear(CC_ATTR_HOST_SEV_SNP);

	return ret;
}

#ifdef CONFIG_IRQ_REMAP
int __init amd_iommu_prepare(void)
{
	int ret;

	amd_iommu_irq_remap = true;

	ret = iommu_go_to_state(IOMMU_ACPI_FINISHED);
	if (ret) {
		amd_iommu_irq_remap = false;
		return ret;
	}

	return amd_iommu_irq_remap ? 0 : -ENODEV;
}

int __init amd_iommu_enable(void)
{
	int ret;

	ret = iommu_go_to_state(IOMMU_ENABLED);
	if (ret)
		return ret;

	irq_remapping_enabled = 1;
	return amd_iommu_xt_mode;
}

void amd_iommu_disable(void)
{
	amd_iommu_suspend(NULL);
}

int amd_iommu_reenable(int mode)
{
	amd_iommu_resume(NULL);

	return 0;
}

int amd_iommu_enable_faulting(unsigned int cpu)
{
	/* We enable MSI later when PCI is initialized */
	return 0;
}
#endif

/*
 * This is the core init function for AMD IOMMU hardware in the system.
 * This function is called from the generic x86 DMA layer initialization
 * code.
 */
static int __init amd_iommu_init(void)
{
	int ret;

	ret = iommu_go_to_state(IOMMU_INITIALIZED);
#ifdef CONFIG_GART_IOMMU
	if (ret && list_empty(&amd_iommu_list)) {
		/*
		 * We failed to initialize the AMD IOMMU - try fallback
		 * to GART if possible.
		 */
		gart_iommu_init();
	}
#endif

	if (!ret)
		amd_iommu_debugfs_setup();

	return ret;
}

static bool amd_iommu_sme_check(void)
{
	if (!cc_platform_has(CC_ATTR_HOST_MEM_ENCRYPT) ||
	    (boot_cpu_data.x86 != 0x17))
		return true;

	/* For Fam17h, a specific level of support is required */
	if (boot_cpu_data.microcode >= 0x08001205)
		return true;

	if ((boot_cpu_data.microcode >= 0x08001126) &&
	    (boot_cpu_data.microcode <= 0x080011ff))
		return true;

	pr_notice("IOMMU not currently supported when SME is active\n");

	return false;
}

/****************************************************************************
 *
 * Early detect code. This code runs at IOMMU detection time in the DMA
 * layer. It just looks if there is an IVRS ACPI table to detect AMD
 * IOMMUs
 *
 ****************************************************************************/
void __init amd_iommu_detect(void)
{
	int ret;

	if (no_iommu || (iommu_detected && !gart_iommu_aperture))
		goto disable_snp;

	if (!amd_iommu_sme_check())
		goto disable_snp;

	ret = iommu_go_to_state(IOMMU_IVRS_DETECTED);
	if (ret)
		goto disable_snp;

	amd_iommu_detected = true;
	iommu_detected = 1;
	x86_init.iommu.iommu_init = amd_iommu_init;
	return;

disable_snp:
	if (cc_platform_has(CC_ATTR_HOST_SEV_SNP))
		cc_platform_clear(CC_ATTR_HOST_SEV_SNP);
}

/****************************************************************************
 *
 * Parsing functions for the AMD IOMMU specific kernel command line
 * options.
 *
 ****************************************************************************/

static int __init parse_amd_iommu_dump(char *str)
{
	amd_iommu_dump = true;

	return 1;
}

static int __init parse_amd_iommu_intr(char *str)
{
	for (; *str; ++str) {
		if (strncmp(str, "legacy", 6) == 0) {
			amd_iommu_guest_ir = AMD_IOMMU_GUEST_IR_LEGACY_GA;
			break;
		}
		if (strncmp(str, "vapic", 5) == 0) {
			amd_iommu_guest_ir = AMD_IOMMU_GUEST_IR_VAPIC;
			break;
		}
	}
	return 1;
}

static int __init parse_amd_iommu_options(char *str)
{
	if (!str)
		return -EINVAL;

	while (*str) {
		if (strncmp(str, "fullflush", 9) == 0) {
			pr_warn("amd_iommu=fullflush deprecated; use iommu.strict=1 instead\n");
			iommu_set_dma_strict();
		} else if (strncmp(str, "force_enable", 12) == 0) {
			amd_iommu_force_enable = true;
		} else if (strncmp(str, "off", 3) == 0) {
			amd_iommu_disabled = true;
		} else if (strncmp(str, "force_isolation", 15) == 0) {
			amd_iommu_force_isolation = true;
		} else if (strncmp(str, "pgtbl_v1", 8) == 0) {
			amd_iommu_pgtable = PD_MODE_V1;
		} else if (strncmp(str, "pgtbl_v2", 8) == 0) {
			amd_iommu_pgtable = PD_MODE_V2;
		} else if (strncmp(str, "irtcachedis", 11) == 0) {
			amd_iommu_irtcachedis = true;
		} else if (strncmp(str, "nohugepages", 11) == 0) {
			pr_info("Restricting V1 page-sizes to 4KiB");
			amd_iommu_pgsize_bitmap = AMD_IOMMU_PGSIZES_4K;
		} else if (strncmp(str, "v2_pgsizes_only", 15) == 0) {
			pr_info("Restricting V1 page-sizes to 4KiB/2MiB/1GiB");
			amd_iommu_pgsize_bitmap = AMD_IOMMU_PGSIZES_V2;
		} else {
			pr_notice("Unknown option - '%s'\n", str);
		}

		str += strcspn(str, ",");
		while (*str == ',')
			str++;
	}

	return 1;
}

static int __init parse_ivrs_ioapic(char *str)
{
	u32 seg = 0, bus, dev, fn;
	int id, i;
	u32 devid;

	if (sscanf(str, "=%d@%x:%x.%x", &id, &bus, &dev, &fn) == 4 ||
	    sscanf(str, "=%d@%x:%x:%x.%x", &id, &seg, &bus, &dev, &fn) == 5)
		goto found;

	if (sscanf(str, "[%d]=%x:%x.%x", &id, &bus, &dev, &fn) == 4 ||
	    sscanf(str, "[%d]=%x:%x:%x.%x", &id, &seg, &bus, &dev, &fn) == 5) {
		pr_warn("ivrs_ioapic%s option format deprecated; use ivrs_ioapic=%d@%04x:%02x:%02x.%d instead\n",
			str, id, seg, bus, dev, fn);
		goto found;
	}

	pr_err("Invalid command line: ivrs_ioapic%s\n", str);
	return 1;

found:
	if (early_ioapic_map_size == EARLY_MAP_SIZE) {
		pr_err("Early IOAPIC map overflow - ignoring ivrs_ioapic%s\n",
			str);
		return 1;
	}

	devid = IVRS_GET_SBDF_ID(seg, bus, dev, fn);

	cmdline_maps			= true;
	i				= early_ioapic_map_size++;
	early_ioapic_map[i].id		= id;
	early_ioapic_map[i].devid	= devid;
	early_ioapic_map[i].cmd_line	= true;

	return 1;
}

static int __init parse_ivrs_hpet(char *str)
{
	u32 seg = 0, bus, dev, fn;
	int id, i;
	u32 devid;

	if (sscanf(str, "=%d@%x:%x.%x", &id, &bus, &dev, &fn) == 4 ||
	    sscanf(str, "=%d@%x:%x:%x.%x", &id, &seg, &bus, &dev, &fn) == 5)
		goto found;

	if (sscanf(str, "[%d]=%x:%x.%x", &id, &bus, &dev, &fn) == 4 ||
	    sscanf(str, "[%d]=%x:%x:%x.%x", &id, &seg, &bus, &dev, &fn) == 5) {
		pr_warn("ivrs_hpet%s option format deprecated; use ivrs_hpet=%d@%04x:%02x:%02x.%d instead\n",
			str, id, seg, bus, dev, fn);
		goto found;
	}

	pr_err("Invalid command line: ivrs_hpet%s\n", str);
	return 1;

found:
	if (early_hpet_map_size == EARLY_MAP_SIZE) {
		pr_err("Early HPET map overflow - ignoring ivrs_hpet%s\n",
			str);
		return 1;
	}

	devid = IVRS_GET_SBDF_ID(seg, bus, dev, fn);

	cmdline_maps			= true;
	i				= early_hpet_map_size++;
	early_hpet_map[i].id		= id;
	early_hpet_map[i].devid		= devid;
	early_hpet_map[i].cmd_line	= true;

	return 1;
}

#define ACPIID_LEN (ACPIHID_UID_LEN + ACPIHID_HID_LEN)

static int __init parse_ivrs_acpihid(char *str)
{
	u32 seg = 0, bus, dev, fn;
	char *hid, *uid, *p, *addr;
	char acpiid[ACPIID_LEN + 1] = { }; /* size with NULL terminator */
	int i;

	addr = strchr(str, '@');
	if (!addr) {
		addr = strchr(str, '=');
		if (!addr)
			goto not_found;

		++addr;

		if (strlen(addr) > ACPIID_LEN)
			goto not_found;

		if (sscanf(str, "[%x:%x.%x]=%s", &bus, &dev, &fn, acpiid) == 4 ||
		    sscanf(str, "[%x:%x:%x.%x]=%s", &seg, &bus, &dev, &fn, acpiid) == 5) {
			pr_warn("ivrs_acpihid%s option format deprecated; use ivrs_acpihid=%s@%04x:%02x:%02x.%d instead\n",
				str, acpiid, seg, bus, dev, fn);
			goto found;
		}
		goto not_found;
	}

	/* We have the '@', make it the terminator to get just the acpiid */
	*addr++ = 0;

	if (strlen(str) > ACPIID_LEN)
		goto not_found;

	if (sscanf(str, "=%s", acpiid) != 1)
		goto not_found;

	if (sscanf(addr, "%x:%x.%x", &bus, &dev, &fn) == 3 ||
	    sscanf(addr, "%x:%x:%x.%x", &seg, &bus, &dev, &fn) == 4)
		goto found;

not_found:
	pr_err("Invalid command line: ivrs_acpihid%s\n", str);
	return 1;

found:
	p = acpiid;
	hid = strsep(&p, ":");
	uid = p;

	if (!hid || !(*hid) || !uid) {
		pr_err("Invalid command line: hid or uid\n");
		return 1;
	}

	/*
	 * Ignore leading zeroes after ':', so e.g., AMDI0095:00
	 * will match AMDI0095:0 in the second strcmp in acpi_dev_hid_uid_match
	 */
	while (*uid == '0' && *(uid + 1))
		uid++;

	if (strlen(hid) >= ACPIHID_HID_LEN) {
		pr_err("Invalid command line: hid is too long\n");
		return 1;
	} else if (strlen(uid) >= ACPIHID_UID_LEN) {
		pr_err("Invalid command line: uid is too long\n");
		return 1;
	}

	i = early_acpihid_map_size++;
	memcpy(early_acpihid_map[i].hid, hid, strlen(hid));
	memcpy(early_acpihid_map[i].uid, uid, strlen(uid));
	early_acpihid_map[i].devid = IVRS_GET_SBDF_ID(seg, bus, dev, fn);
	early_acpihid_map[i].cmd_line	= true;

	return 1;
}

__setup("amd_iommu_dump",	parse_amd_iommu_dump);
__setup("amd_iommu=",		parse_amd_iommu_options);
__setup("amd_iommu_intr=",	parse_amd_iommu_intr);
__setup("ivrs_ioapic",		parse_ivrs_ioapic);
__setup("ivrs_hpet",		parse_ivrs_hpet);
__setup("ivrs_acpihid",		parse_ivrs_acpihid);

bool amd_iommu_pasid_supported(void)
{
	/* CPU page table size should match IOMMU guest page table size */
	if (cpu_feature_enabled(X86_FEATURE_LA57) &&
	    amd_iommu_gpt_level != PAGE_MODE_5_LEVEL)
		return false;

	/*
	 * Since DTE[Mode]=0 is prohibited on SNP-enabled system
	 * (i.e. EFR[SNPSup]=1), IOMMUv2 page table cannot be used without
	 * setting up IOMMUv1 page table.
	 */
	return amd_iommu_gt_ppr_supported() && !amd_iommu_snp_en;
}

struct amd_iommu *get_amd_iommu(unsigned int idx)
{
	unsigned int i = 0;
	struct amd_iommu *iommu;

	for_each_iommu(iommu)
		if (i++ == idx)
			return iommu;
	return NULL;
}

/****************************************************************************
 *
 * IOMMU EFR Performance Counter support functionality. This code allows
 * access to the IOMMU PC functionality.
 *
 ****************************************************************************/

u8 amd_iommu_pc_get_max_banks(unsigned int idx)
{
	struct amd_iommu *iommu = get_amd_iommu(idx);

	if (iommu)
		return iommu->max_banks;

	return 0;
}

bool amd_iommu_pc_supported(void)
{
	return amd_iommu_pc_present;
}

u8 amd_iommu_pc_get_max_counters(unsigned int idx)
{
	struct amd_iommu *iommu = get_amd_iommu(idx);

	if (iommu)
		return iommu->max_counters;

	return 0;
}

static int iommu_pc_get_set_reg(struct amd_iommu *iommu, u8 bank, u8 cntr,
				u8 fxn, u64 *value, bool is_write)
{
	u32 offset;
	u32 max_offset_lim;

	/* Make sure the IOMMU PC resource is available */
	if (!amd_iommu_pc_present)
		return -ENODEV;

	/* Check for valid iommu and pc register indexing */
	if (WARN_ON(!iommu || (fxn > 0x28) || (fxn & 7)))
		return -ENODEV;

	offset = (u32)(((0x40 | bank) << 12) | (cntr << 8) | fxn);

	/* Limit the offset to the hw defined mmio region aperture */
	max_offset_lim = (u32)(((0x40 | iommu->max_banks) << 12) |
				(iommu->max_counters << 8) | 0x28);
	if ((offset < MMIO_CNTR_REG_OFFSET) ||
	    (offset > max_offset_lim))
		return -EINVAL;

	if (is_write) {
		u64 val = *value & GENMASK_ULL(47, 0);

		writel((u32)val, iommu->mmio_base + offset);
		writel((val >> 32), iommu->mmio_base + offset + 4);
	} else {
		*value = readl(iommu->mmio_base + offset + 4);
		*value <<= 32;
		*value |= readl(iommu->mmio_base + offset);
		*value &= GENMASK_ULL(47, 0);
	}

	return 0;
}

int amd_iommu_pc_get_reg(struct amd_iommu *iommu, u8 bank, u8 cntr, u8 fxn, u64 *value)
{
	if (!iommu)
		return -EINVAL;

	return iommu_pc_get_set_reg(iommu, bank, cntr, fxn, value, false);
}

int amd_iommu_pc_set_reg(struct amd_iommu *iommu, u8 bank, u8 cntr, u8 fxn, u64 *value)
{
	if (!iommu)
		return -EINVAL;

	return iommu_pc_get_set_reg(iommu, bank, cntr, fxn, value, true);
}

#ifdef CONFIG_KVM_AMD_SEV
static int iommu_page_make_shared(void *page)
{
	unsigned long paddr, pfn;

	paddr = iommu_virt_to_phys(page);
	/* Cbit maybe set in the paddr */
	pfn = __sme_clr(paddr) >> PAGE_SHIFT;

	if (!(pfn % PTRS_PER_PMD)) {
		int ret, level;
		bool assigned;

		ret = snp_lookup_rmpentry(pfn, &assigned, &level);
		if (ret) {
			pr_warn("IOMMU PFN %lx RMP lookup failed, ret %d\n", pfn, ret);
			return ret;
		}

		if (!assigned) {
			pr_warn("IOMMU PFN %lx not assigned in RMP table\n", pfn);
			return -EINVAL;
		}

		if (level > PG_LEVEL_4K) {
			ret = psmash(pfn);
			if (!ret)
				goto done;

			pr_warn("PSMASH failed for IOMMU PFN %lx huge RMP entry, ret: %d, level: %d\n",
				pfn, ret, level);
			return ret;
		}
	}

done:
	return rmp_make_shared(pfn, PG_LEVEL_4K);
}

static int iommu_make_shared(void *va, size_t size)
{
	void *page;
	int ret;

	if (!va)
		return 0;

	for (page = va; page < (va + size); page += PAGE_SIZE) {
		ret = iommu_page_make_shared(page);
		if (ret)
			return ret;
	}

	return 0;
}

int amd_iommu_snp_disable(void)
{
	struct amd_iommu *iommu;
	int ret;

	if (!amd_iommu_snp_en)
		return 0;

	for_each_iommu(iommu) {
		ret = iommu_make_shared(iommu->evt_buf, EVT_BUFFER_SIZE);
		if (ret)
			return ret;

		ret = iommu_make_shared(iommu->ppr_log, PPR_LOG_SIZE);
		if (ret)
			return ret;

		ret = iommu_make_shared((void *)iommu->cmd_sem, PAGE_SIZE);
		if (ret)
			return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(amd_iommu_snp_disable);

bool amd_iommu_sev_tio_supported(void)
{
	return check_feature2(FEATURE_SEVSNPIO_SUP);
}
EXPORT_SYMBOL_GPL(amd_iommu_sev_tio_supported);
#endif
