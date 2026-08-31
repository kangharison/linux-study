// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe Enclosure management driver created for LED interfaces based on
 * indications. It says *what indications* blink but does not specify *how*
 * they blink - it is hardware defined.
 *
 * The driver name refers to Native PCIe Enclosure Management. It is
 * first indication oriented standard with specification.
 *
 * Native PCIe Enclosure Management (NPEM)
 *	PCIe Base Specification r6.1 sec 6.28, 7.9.19
 *
 * _DSM Definitions for PCIe SSD Status LED
 *	 PCI Firmware Specification, r3.3 sec 4.7
 *
 * Two backends are supported to manipulate indications: Direct NPEM register
 * access (npem_ops) and indirect access through the ACPI _DSM (dsm_ops).
 * _DSM is used if supported, else NPEM.
 *
 * Copyright (c) 2021-2022 Dell Inc.
 * Copyright (c) 2023-2024 Intel Corporation
 *	Mariusz Tkaczyk <mariusz.tkaczyk@linux.intel.com>
 */

/*
 * [한국어 설명] 인클로저의 상태 LED 를 제어하는 드라이버 (npem.c)
 *
 * === 파일의 역할 ===
 * 서버 섀시의 드라이브 베이마다 LED 가 있다. "정상", "고장", "재구성 중",
 * "이 드라이브를 찾으세요" 같은 상태를 사람에게 알리는 용도다.
 * 이 파일은 그 LED 를 커널의 LED 서브시스템에 노출한다.
 *
 * 위 원문 주석이 중요한 구분을 밝힌다 — 이 표준은 "무엇을 알릴지"만
 * 정하고 "어떻게 깜빡일지" 는 정하지 않는다. 깜빡임 패턴은 하드웨어가
 * 알아서 한다. 그래서 커널은 "고장 표시를 켜라" 고만 하면 된다.
 *
 * 두 가지 접근 경로를 지원한다.
 *   NPEM (Native PCIe Enclosure Management) — PCIe 확장 capability 로
 *     직접 레지스터를 읽고 쓴다. 표준적이고 빠르다.
 *   _DSM — ACPI 메서드로 펌웨어에게 부탁한다. NPEM capability 가 없거나,
 *     펌웨어가 자기가 관리하겠다고 한 경우에 쓴다.
 * 두 경로가 같은 인터페이스(npem_ops / dsm_ops)를 구현해, 위쪽 코드는
 * 어느 쪽인지 몰라도 된다.
 *
 * 표시 종류는 스펙이 정한 목록이다 — OK, Locate, Fail, Rebuild,
 * PFA(Predicted Failure Analysis), Hot Spare, In Critical Array,
 * In Failed Array, Invalid Device Type, Disabled 등.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: 열거 시 NPEM capability 또는 _DSM 을 가진 장치를 발견
 *         -> [이 파일] pci_npem_create()
 *            -> LED 클래스 장치를 만들어 /sys/class/leds/ 에 등록
 *
 * 제어: echo 1 > /sys/class/leds/<name>:fail/brightness
 *         -> LED 코어 -> [이 파일] npem_set()
 *            -> NPEM 레지스터에 쓰거나 _DSM 을 평가
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. _DSM 평가가 잠들 수 있고,
 * NPEM 레지스터도 완료 대기(Command Completed)가 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: LED 서브시스템(drivers/leds/), sysfs.
 * 아래쪽: access.c 의 확장 capability 접근, ACPI 의 _DSM 평가.
 * 공유 상태: struct npem — LED 클래스 장치, 뮤텍스, 그리고 ops 포인터.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * 하지만 이 파일의 주된 대상이 바로 NVMe SSD 다. 원문 주석이 인용한
 * 스펙 이름이 그것을 말해 준다 — "_DSM Definitions for PCIe SSD Status LED"
 * (PCI Firmware Specification r3.3 sec 4.7).
 *
 * U.2/EDSFF 백플레인에서 드라이브 하나가 고장 났을 때, 관리자가 어느
 * 베이인지 알아야 교체할 수 있다. `ledctl` 같은 도구로 그 드라이브의
 * Locate LED 를 켜면 이 파일을 거쳐 실제로 불이 들어온다.
 *
 * 실무에서는 slot.c 와 함께 쓰인다 — 슬롯 번호로 어느 베이인지 알고,
 * 이 파일로 그 베이의 LED 를 켜는 식이다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_npem_create()      : NPEM 또는 _DSM 지원을 확인하고 LED 장치를 만든다.
 * pci_npem_remove()      : 그 반대.
 * npem_set() / npem_get(): LED 상태를 쓰고 읽는다. ops 로 위임한다.
 * npem_get_ops()         : NPEM 과 _DSM 중 어느 것을 쓸지 정한다.
 * npem_write_ctrl()      : NPEM Control 레지스터에 쓰고 Command Completed 를
 *                          기다린다. 하드웨어가 처리에 시간이 걸린다.
 * dsm_get() / dsm_set()  : _DSM 경로의 구현.
 * struct indication      : 표시 하나(비트 위치와 이름).
 * struct npem            : LED 장치 하나의 상태.
 */

#include <linux/acpi.h>		/* NVMe: ACPI _DSM 평가에 필요한 헤더, PCIe SSD LED 상태를 ACPI를 통해 제어할 때 사용 */
#include <linux/bitops.h>	/* NVMe: indication 비트 마스크 연산에 사용 */
#include <linux/errno.h>	/* NVMe: NVMe SSD LED 제어 실패 시 반환할 오류 코드 정의 */
#include <linux/iopoll.h>	/* NVMe: NPEM 제어 레지스터 쓰기 후 Command Completed 폴림에 사용 */
#include <linux/leds.h>		/* NVMe: LED class device 등록/제어 인터페이스 */
#include <linux/mutex.h>	/* NVMe: 다수 LED 접근 시 NPEM 레지스터 동시 접근 보호 */
#include <linux/pci.h>		/* NVMe: PCI 장치 구조체와 NPEM capability 접근 */
#include <linux/pci_regs.h>	/* NVMe: PCI_NPEM_* 레지스터 오프셋/비트 정의 */
#include <linux/types.h>	/* NVMe: u8/u16/u32 등 고정폭 정수 타입 */
#include <linux/uleds.h>	/* NVMe: 사용자공간 LED 인터페이스 보조 */

#include "pci.h"		/* NVMe: 내부 PCI 헤더, pci_npem_* 선언 및 pci_dev->npem 필드 접근 */

/* NVMe: 하나의 LED indication(OK, LOCATE, FAIL 등)을 비트 위치와 이름으로 표현 */
struct indication {
	u32 bit;		/* NVMe: NPEM Capability/Control 레지스터 내 해당 indication 비트 */
	const char *name;	/* NVMe: sysfs/leds에 노출되는 LED 이름, 예: enclosure:locate */
};

/* NVMe: Native NPEM 레지스터 방식에서 지원하는 indication 목록; NVMe SSD 상태를 LED로 표현할 수 있는 모든 비트 */
static const struct indication npem_indications[] = {
	{PCI_NPEM_IND_OK,	"enclosure:ok"},		/* NVMe: 정상 동작 중인 NVMe 장치 */
	{PCI_NPEM_IND_LOCATE,	"enclosure:locate"},	/* NVMe: 물리적으로 찾기 위한 깜빡임 */
	{PCI_NPEM_IND_FAIL,	"enclosure:fail"},		/* NVMe: 장치 고장 표시 */
	{PCI_NPEM_IND_REBUILD,	"enclosure:rebuild"},	/* NVMe: RAID 재구성 중인 드라이브 */
	{PCI_NPEM_IND_PFA,	"enclosure:pfa"},		/* NVMe: Predictive Failure Analysis */
	{PCI_NPEM_IND_HOTSPARE,	"enclosure:hotspare"},	/* NVMe: 핫스페어 드라이브 */
	{PCI_NPEM_IND_ICA,	"enclosure:ica"},		/* NVMe: Incomplete Chassis Association */
	{PCI_NPEM_IND_IFA,	"enclosure:ifa"},		/* NVMe: Invalid Chassis Association */
	{PCI_NPEM_IND_IDT,	"enclosure:idt"},		/* NVMe: Invalid Device Type */
	{PCI_NPEM_IND_DISABLED,	"enclosure:disabled"},	/* NVMe: 소프트웨어적으로 비활성화된 드라이브 */
	{PCI_NPEM_IND_SPEC_0,	"enclosure:specific_0"},	/* NVMe: 벤더 특정 indication 0 */
	{PCI_NPEM_IND_SPEC_1,	"enclosure:specific_1"},	/* NVMe: 벤더 특정 indication 1 */
	{PCI_NPEM_IND_SPEC_2,	"enclosure:specific_2"},	/* NVMe: 벤더 특정 indication 2 */
	{PCI_NPEM_IND_SPEC_3,	"enclosure:specific_3"},	/* NVMe: 벤더 특정 indication 3 */
	{PCI_NPEM_IND_SPEC_4,	"enclosure:specific_4"},	/* NVMe: 벤더 특정 indication 4 */
	{PCI_NPEM_IND_SPEC_5,	"enclosure:specific_5"},	/* NVMe: 벤더 특정 indication 5 */
	{PCI_NPEM_IND_SPEC_6,	"enclosure:specific_6"},	/* NVMe: 벤더 특정 indication 6 */
	{PCI_NPEM_IND_SPEC_7,	"enclosure:specific_7"},	/* NVMe: 벤더 특정 indication 7 */
	{0,			NULL}				/* NVMe: 배열 종료 마커 */
};

/* _DSM PCIe SSD LED States correspond to NPEM register values */
/* NVMe: ACPI _DSM 방식은 NPEM 레지스터 값과 의미가 동일하지만, SPEC_0~7은 미지원 */
static const struct indication dsm_indications[] = {
	{PCI_NPEM_IND_OK,	"enclosure:ok"},		/* NVMe: _DSM을 통한 정상 상태 */
	{PCI_NPEM_IND_LOCATE,	"enclosure:locate"},	/* NVMe: _DSM을 통한 위치 표시 */
	{PCI_NPEM_IND_FAIL,	"enclosure:fail"},		/* NVMe: _DSM을 통한 고장 표시 */
	{PCI_NPEM_IND_REBUILD,	"enclosure:rebuild"},	/* NVMe: _DSM을 통한 재구성 표시 */
	{PCI_NPEM_IND_PFA,	"enclosure:pfa"},		/* NVMe: _DSM을 통한 PFA 표시 */
	{PCI_NPEM_IND_HOTSPARE,	"enclosure:hotspare"},	/* NVMe: _DSM을 통한 핫스페어 표시 */
	{PCI_NPEM_IND_ICA,	"enclosure:ica"},		/* NVMe: _DSM을 통한 ICA 표시 */
	{PCI_NPEM_IND_IFA,	"enclosure:ifa"},		/* NVMe: _DSM을 통한 IFA 표시 */
	{PCI_NPEM_IND_IDT,	"enclosure:idt"},		/* NVMe: _DSM을 통한 IDT 표시 */
	{PCI_NPEM_IND_DISABLED,	"enclosure:disabled"},	/* NVMe: _DSM을 통한 비활성화 표시 */
	{0,			NULL}				/* NVMe: 배열 종료 마커 */
};

/* NVMe: indication 배열을 순회하는 매크로; NULL 종료 요소까지 순회 */
#define for_each_indication(ind, inds) \
	for (ind = inds; ind->bit; ind++)

/*
 * The driver has internal list of supported indications. Ideally, the driver
 * should not touch bits that are not defined and for which LED devices are
 * not exposed but in reality, it needs to turn them off.
 *
 * Otherwise, there will be no possibility to turn off indications turned on by
 * other utilities or turned on by default and it leads to bad user experience.
 *
 * Additionally, it excludes NPEM commands like RESET or ENABLE.
 */
/* NVMe: NPEM capability 레지스터 값(caps)에서 드라이버가 지원하는 indication 비트만 추출; ENABLE/RESET 같은 명령 비트는 제외 */
static u32 reg_to_indications(u32 caps, const struct indication *inds)
{
	const struct indication *ind;	/* NVMe: 순회용 indication 포인터 */
	u32 supported_indications = 0;	/* NVMe: 최종 지원 비트 마스크, 초기 0 */

	for_each_indication(ind, inds)	/* NVMe: 지정된 indication 테이블을 끝까지 순회 */
		supported_indications |= ind->bit;	/* NVMe: 해당 indication 비트를 지원 마스크에 추가 */

	return caps & supported_indications;	/* NVMe: 하드웨어 capability와 드라이버 지원 마스크의 교집합 반환, NVMe SSD에 실제 노출될 LED 비트만 남김 */
}

/**
 * struct npem_led - LED details
 * @indication: indication details
 * @npem: NPEM device
 * @name: LED name
 * @led: LED device
 */
/* NVMe: NVMe PCIe 장치에 연결된 하나의 LED 클래스 장치와 그 메타데이터 */
struct npem_led {
	const struct indication *indication;	/* NVMe: 이 LED가 표현하는 indication(OK/LOCATE/FAIL 등) 정보 */
	struct npem *npem;			/* NVMe: 이 LED가 속한 NPEM 컨텍스트, 대응하는 PCIe/NVMe 장치 */
	char name[LED_MAX_NAME_SIZE];		/* NVMe: led_classdev에 등록할 LED 이름 버퍼, 예: <pci_name>:enclosure:locate */
	struct led_classdev led;		/* NVMe: Linux LED 서브시스템에 등록되는 LED 장치 구조체 */
};

/**
 * struct npem_ops - backend specific callbacks
 * @get_active_indications: get active indications
 *	npem: NPEM device
 *	inds: response buffer
 * @set_active_indications: set new indications
 *	npem: npem device
 *	inds: bit mask to set
 * @inds: supported indications array, set of indications is backend specific
 * @name: backend name
 */
/* NVMe: NPEM 레지스터 직접 접근과 ACPI _DSM 두 백엔드를 추상화하는 함수 포인터 테이블 */
struct npem_ops {
	int (*get_active_indications)(struct npem *npem, u32 *inds);	/* NVMe: 현재 켜진 LED indication 비트를 읽어오는 콜백 */
	int (*set_active_indications)(struct npem *npem, u32 inds);	/* NVMe: 새로운 LED indication 비트를 설정하는 콜백 */
	const struct indication *inds;	/* NVMe: 이 백엔드가 지원하는 indication 테이블 */
	const char *name;		/* NVMe: 백엔드 이름, 로그용 */
};

/**
 * struct npem - NPEM device properties
 * @dev: PCI device this driver is attached to
 * @ops: backend specific callbacks
 * @lock: serializes concurrent access to NPEM device by multiple LED devices
 * @pos: cached offset of NPEM Capability Register in Configuration Space;
 *	only used if NPEM registers are accessed directly and not through _DSM
 * @supported_indications: cached bit mask of supported indications;
 *	non-indication and reserved bits in the NPEM Capability Register are
 *	cleared in this bit mask
 * @active_indications: cached bit mask of active indications;
 *	non-indication and reserved bits in the NPEM Control Register are
 *	cleared in this bit mask
 * @active_inds_initialized: whether @active_indications has been initialized;
 *	On Dell platforms, it is required that IPMI drivers are loaded before
 *	the GET_STATE_DSM method is invoked: They use an IPMI OpRegion to
 *	get/set the active LEDs. By initializing @active_indications lazily
 *	(on first access to an LED), IPMI drivers are given a chance to load.
 *	If they are not loaded in time, users will see various errors on LED
 *	access in dmesg. Once they are loaded, the errors go away and LED
 *	access becomes possible.
 * @led_cnt: size of @leds array
 * @leds: array containing LED class devices of all supported LEDs
 */
/* NVMe: 하나의 PCIe/NVMe 장치에 대한 NPEM 전체 컨텍스트; PCI 장치와 LED 장치들을 연결 */
struct npem {
	struct pci_dev *dev;			/* NVMe: 이 NPEM이 연결된 PCIe 장치, NVMe SSD의 pci_dev */
	const struct npem_ops *ops;		/* NVMe: 사용 중인 백엔드(직접 NPEM 또는 _DSM) 연산자 */
	struct mutex lock;			/* NVMe: 동시 LED brightness_set/get 접근 직렬화 */
	u16 pos;				/* NVMe: NPEM 확장 capability 레지스터 오프셋, PCI 설정공간 내 위치 */
	u32 supported_indications;		/* NVMe: 이 NVMe 장치/슬롯이 지원하는 indication 비트 마스크 */
	u32 active_indications;			/* NVMe: 현재 켜진 indication 상태의 소프트웨어 캐시 */
	unsigned int active_inds_initialized:1;	/* NVMe: active_indications가 초기화되었는지 여부, Dell IPMI 늦은 로딩 대응 */
	int led_cnt;				/* NVMe: 등록된 LED class device 개수 */
	struct npem_led leds[];			/* NVMe: 지원하는 indication 개수만큼 동적으로 할당된 LED 구조체 배열 */
};

/* NVMe: NPEM capability 레지스터에서 32비트 값을 읽어오는 헬퍼, PCI 설정공간 접근 */
static int npem_read_reg(struct npem *npem, u16 reg, u32 *val)
{
	int ret = pci_read_config_dword(npem->dev, npem->pos + reg, val);
						/* NVMe: NVMe SSD의 pci_dev를 통해 PCI 설정공간에서 32비트 읽기 */

	return pcibios_err_to_errno(ret);	/* NVMe: PCI BIOS 반환값을 errno로 변환하여 NVMe/LED 호출자에게 전달 */
}

/* NVMe: NPEM Control 레지스터에 32비트 값을 쓰는 헬퍼 */
static int npem_write_ctrl(struct npem *npem, u32 reg)
{
	int pos = npem->pos + PCI_NPEM_CTRL;	/* NVMe: Control 레지스터의 PCI 설정공간 절대 주소 계산 */
	int ret = pci_write_config_dword(npem->dev, pos, reg);
						/* NVMe: NVMe SSD의 pci_dev로 Control 레지스터에 값 기록 */

	return pcibios_err_to_errno(ret);	/* NVMe: 쓰기 결과 errno 변환 */
}

/* NVMe: 직접 NPEM 백엔드에서 현재 활성 indication을 읽어오는 함수 */
static int npem_get_active_indications(struct npem *npem, u32 *inds)
{
	u32 ctrl;	/* NVMe: NPEM Control 레지스터 원시 값 */
	int ret;	/* NVMe: PCI 설정공간 접근 결과 */

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, &ctrl);
						/* NVMe: NVMe 장치의 NPEM Control 레지스터 읽기 */
	if (ret)					/* NVMe: 읽기 실패 시 */
		return ret;				/* NVMe: 오류를 LED 서브시스템/호출자에게 전파 */

	/* If PCI_NPEM_CTRL_ENABLE is not set then no indication should blink */
	/* NVMe: NPEM 기능이 비활성화되면 모든 LED는 꺼진 것으로 간주, NVMe SSD LED off 상태 */
	if (!(ctrl & PCI_NPEM_CTRL_ENABLE)) {		/* NVMe: ENABLE 비트가 0인지 확인 */
		*inds = 0;				/* NVMe: 활성 indication이 없음을 호출자에 반환 */
		return 0;				/* NVMe: 정상 완료, LEDs off */
	}

	*inds = ctrl & npem->supported_indications;	/* NVMe: Control 값에서 지원하는 indication 비트만 추출, 실제 NVMe SSD에 연결된 LED 상태 */

	return 0;					/* NVMe: 성공 */
}

/* NVMe: 직접 NPEM 백엔드에서 indication 비트를 설정하고 Command Completed를 기다리는 함수 */
static int npem_set_active_indications(struct npem *npem, u32 inds)
{
	int ctrl, ret, ret_val;	/* NVMe: ctrl=쓸 제어값, ret=함수 반환값, ret_val=폴림 콜백 반환값 */
	u32 cc_status;		/* NVMe: NPEM Status 레지스터 값, Command Completed 비트 확인용 */

	lockdep_assert_held(&npem->lock);	/* NVMe: mutex lock이 잡힌 상태에서만 호출되어야 함을 lockdep이 감시 */

	/* This bit is always required */
	ctrl = inds | PCI_NPEM_CTRL_ENABLE;	/* NVMe: indication 변경 시 NPEM 기능을 ENABLE 비트와 함께 켜야 동작 */

	ret = npem_write_ctrl(npem, ctrl);	/* NVMe: NVMe 장치의 NPEM Control 레지스터에 새 indication 기록 */
	if (ret)				/* NVMe: 쓰기 실패 시 */
		return ret;			/* NVMe: 즉시 오류 반환, NVMe SSD LED 상태는 변경되지 않음 */

	/*
	 * For the case where a NPEM command has not completed immediately,
	 * it is recommended that software not continuously "spin" on polling
	 * the status register, but rather poll under interrupt at a reduced
	 * rate; for example at 10 ms intervals.
	 *
	 * PCIe r6.1 sec 6.28 "Implementation Note: Software Polling of NPEM
	 * Command Completed"
	 */
	/* NVMe: NPEM 명령이 완료될 때까지 10ms 간격으로 최대 1초간 Status 레지스터 폴림 */
	ret = read_poll_timeout(npem_read_reg, ret_val,
				ret_val || (cc_status & PCI_NPEM_STATUS_CC),
				10 * USEC_PER_MSEC, USEC_PER_SEC, false, npem,
				PCI_NPEM_STATUS, &cc_status);
						/* NVMe: npem_read_reg() 폴림, 콜백 오류(ret_val) 또는 Command Completed 비트가 set되면 종료 */
	if (ret)				/* NVMe: 타임아웃 등 폴림 자체 실패 시 */
		return ret;			/* NVMe: -ETIMEDOUT 반환 */
	if (ret_val)				/* NVMe: 폴림 중 레지스터 읽기가 오류 난 경우 */
		return ret_val;			/* NVMe: 해당 오류 반환 */

	/*
	 * All writes to control register, including writes that do not change
	 * the register value, are NPEM commands and should eventually result
	 * in a command completion indication in the NPEM Status Register.
	 *
	 * PCIe Base Specification r6.1 sec 7.9.19.3
	 *
	 * Register may not be updated, or other conflicting bits may be
	 * cleared. Spec is not strict here. Read NPEM Control register after
	 * write to keep cache in-sync.
	 */
	return npem_get_active_indications(npem, &npem->active_indications);
						/* NVMe: 쓰기 후 실제 Control 레지스터를 다시 읽어 active_indications 캐시 동기화 */
}

/* NVMe: 직접 NPEM 레지스터 백엔드 정의; NVMe PCIe capability가 있는 장치에 사용 */
static const struct npem_ops npem_ops = {
	.get_active_indications = npem_get_active_indications,		/* NVMe: 상태 읽기 콜백 연결 */
	.set_active_indications = npem_set_active_indications,		/* NVMe: 상태 쓰기 콜백 연결 */
	.name = "Native PCIe Enclosure Management",			/* NVMe: 백엔드 식별 이름 */
	.inds = npem_indications,					/* NVMe: 지원 indication 테이블 */
};

/* NVMe: ACPI _DSM GUID, PCI Firmware Spec r3.3 sec 4.7에 정의된 PCIe SSD Status LED _DSM */
#define DSM_GUID GUID_INIT(0x5d524d9d, 0xfff9, 0x4d4b, 0x8c, 0xb7, 0x74, 0x7e,\
			   0xd5, 0x1e, 0x19, 0x4d)
#define GET_SUPPORTED_STATES_DSM	1	/* NVMe: _DSM 함수 1, 지원하는 LED 상태 비트 질의 */
#define GET_STATE_DSM			2	/* NVMe: _DSM 함수 2, 현재 LED 상태 읽기 */
#define SET_STATE_DSM			3	/* NVMe: _DSM 함수 3, LED 상태 설정 */

/* NVMe: 위 매크로로 초기화된 _DSM GUID 상수 */
static const guid_t dsm_guid = DSM_GUID;

/* NVMe: NVMe SSD의 PCI 장치가 ACPI _DSM PCIe SSD Status LED를 지원하는지 검사 */
static bool npem_has_dsm(struct pci_dev *pdev)
{
	acpi_handle handle;	/* NVMe: PCI 장치에 연결된 ACPI 핸들 */

	handle = ACPI_HANDLE(&pdev->dev);			/* NVMe: NVMe SSD PCI 장치의 ACPI 핸들 획득 시도 */
	if (!handle)						/* NVMe: ACPI 핸들이 없으면 */
		return false;					/* NVMe: _DSM 백엔드 사용 불가, NPEM 레지스터로 폧백 */

	return acpi_check_dsm(handle, &dsm_guid, 0x1,
			      BIT(GET_SUPPORTED_STATES_DSM) |
			      BIT(GET_STATE_DSM) | BIT(SET_STATE_DSM));
								/* NVMe: GUID revision 1에 대해 필요한 3개 _DSM 함수가 모두 있는지 확인 */
}

/* NVMe: ACPI _DSM 출력 버퍼를 C 구조체로 매핑; NVMe SSD LED 상태/오류 코드 포함 */
struct dsm_output {
	u16 status;			/* NVMe: _DSM 반환 상태 코드 */
	u8 function_specific_err;	/* NVMe: 함수별 오류 코드 */
	u8 vendor_specific_err;		/* NVMe: 벤더별 오류 코드 */
	u32 state;			/* NVMe: 현재 PCIe SSD LED 상태 비트(NPEM 값과 동일) */
};

/**
 * dsm_evaluate() - send DSM PCIe SSD Status LED command
 * @pdev: PCI device
 * @dsm_func: DSM LED Function
 * @output: buffer to copy DSM Response
 * @value_to_set: value for SET_STATE_DSM function
 *
 * To not bother caller with ACPI context, the returned _DSM Output Buffer is
 * copied.
 */
/* NVMe: NVMe SSD에 대해 ACPI _DSM 메서드를 평가하고 응답을 dsm_output에 복사 */
static int dsm_evaluate(struct pci_dev *pdev, u64 dsm_func,
			struct dsm_output *output, u32 value_to_set)
{
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);		/* NVMe: 대상 NVMe SSD의 ACPI 핸들 */
	union acpi_object *out_obj, arg3[2];			/* NVMe: _DSM 반환 객체와 SET_STATE용 인자 패키지 */
	union acpi_object *arg3_p = NULL;			/* NVMe: 인자 패키지 포인터, GET 함수에서는 NULL */

	if (dsm_func == SET_STATE_DSM) {			/* NVMe: LED 상태를 설정하는 _DSM 함수인 경우 */
		arg3[0].type = ACPI_TYPE_PACKAGE;		/* NVMe: _DSM arg3 패키지 객체 타입 설정 */
		arg3[0].package.count = 1;			/* NVMe: 패키지 내 요소 1개(버퍼) */
		arg3[0].package.elements = &arg3[1];		/* NVMe: 패키지 요소가 arg3[1]을 가리키도록 설정 */

		arg3[1].type = ACPI_TYPE_BUFFER;		/* NVMe: arg3[1]은 4바이트 버퍼 타입 */
		arg3[1].buffer.length = 4;			/* NVMe: NVMe SSD LED 상태는 32비트 */
		arg3[1].buffer.pointer = (u8 *)&value_to_set;	/* NVMe: 설정할 indication 비트를 가리킴 */

		arg3_p = arg3;					/* NVMe: acpi_evaluate_dsm_typed에 전달할 인자 포인터 확정 */
	}

	out_obj = acpi_evaluate_dsm_typed(handle, &dsm_guid, 0x1, dsm_func,
					  arg3_p, ACPI_TYPE_BUFFER);
								/* NVMe: ACPI _DSM 평가, 반환 타입은 버퍼로 제한 */
	if (!out_obj)						/* NVMe: _DSM 평가 실패 또는 반환 없음 */
		return -EIO;					/* NVMe: NVMe SSD LED 제어 불가 오류 */

	if (out_obj->buffer.length < sizeof(struct dsm_output)) {
								/* NVMe: 반환 버퍼가 예상보다 짧으면 */
		ACPI_FREE(out_obj);				/* NVMe: ACPI 객체 해제 */
		return -EIO;					/* NVMe: 잘못된 응답으로 실패 */
	}

	memcpy(output, out_obj->buffer.pointer, sizeof(struct dsm_output));
								/* NVMe: _DSM 응답을 호출자가 사용할 output 구조체에 복사 */

	ACPI_FREE(out_obj);					/* NVMe: ACPI 반환 객체 메모리 해제 */
	return 0;						/* NVMe: _DSM 평가 성공 */
}

/* NVMe: _DSM GET 함수(지원 상태 읽기/현재 상태 읽기) 래퍼 */
static int dsm_get(struct pci_dev *pdev, u64 dsm_func, u32 *buf)
{
	struct dsm_output output;				/* NVMe: _DSM 응답 저장 버퍼 */
	int ret = dsm_evaluate(pdev, dsm_func, &output, 0);	/* NVMe: _DSM GET 함수 평가, 설정값은 사용되지 않아 0 전달 */

	if (ret)						/* NVMe: _DSM 평가 실패 시 */
		return ret;					/* NVMe: 오류 전파 */

	if (output.status != 0)					/* NVMe: _DSM 상태 코드가 성공(0)이 아니면 */
		return -EIO;					/* NVMe: NVMe SSD가 상태 반환을 거부한 것으로 처리 */

	*buf = output.state;					/* NVMe: 읽어온 LED 상태 비트를 호출자 버퍼에 저장 */
	return 0;						/* NVMe: 성공 */
}

/* NVMe: _DSM 백엔드에서 현재 활성 indication을 읽어오는 콜백 */
static int dsm_get_active_indications(struct npem *npem, u32 *buf)
{
	int ret = dsm_get(npem->dev, GET_STATE_DSM, buf);	/* NVMe: NVMe SSD의 _DSM 함수 2로 현재 LED 상태 읽기 */

	/* Filter out not supported indications in response */
	*buf &= npem->supported_indications;			/* NVMe: _DSM 응답에서 드라이버가 지원하지 않는 indication 비트 제거, 잘못된 비트 무시 */
	return ret;							/* NVMe: dsm_get 결과 반환 */
}

/* NVMe: _DSM 백엔드에서 indication을 설정하는 콜백 */
static int dsm_set_active_indications(struct npem *npem, u32 value)
{
	struct dsm_output output;				/* NVMe: _DSM SET_STATE 응답 저장 */
	int ret = dsm_evaluate(npem->dev, SET_STATE_DSM, &output, value);
								/* NVMe: NVMe SSD에 _DSM 함수 3으로 새 LED 상태 요청 */

	if (ret)						/* NVMe: _DSM 평가 자체 실패 시 */
		return ret;					/* NVMe: 오류 전파 */

	switch (output.status) {				/* NVMe: _DSM 반환 status 코드별 분기 */
	case 4:
		/*
		 * Not all bits are set. If this bit is set, the platform
		 * disregarded some or all of the request state changes. OSPM
		 * should check the resulting PCIe SSD Status LED States to see
		 * what, if anything, has changed.
		 *
		 * PCI Firmware Specification, r3.3 Table 4-19.
		 */
		if (output.function_specific_err != 1)	/* NVMe: status 4일 때 function_specific_err가 1(부분 성공)이 아니면 */
			return -EIO;				/* NVMe: 오류로 처리, NVMe SSD LED 상태 변경 실패 */
		fallthrough;					/* NVMe: 부분 성공은 최종적으로 상태 0과 동일하게 처리 */
	case 0:
		break;						/* NVMe: 정상 처리, 계속 진행 */
	default:
		return -EIO;					/* NVMe: 그 외 status는 모두 오류 */
	}

	npem->active_indications = output.state;	/* NVMe: _DSM이 반환한 실제 LED 상태로 소프트웨어 캐시 갱신 */

	return 0;						/* NVMe: 설정 완료 */
}

/* NVMe: ACPI _DSM 백엔드 정의; OEM 플랫폼에서 NVMe SSD LED를 ACPI 메서드로 제어할 때 사용 */
static const struct npem_ops dsm_ops = {
	.get_active_indications = dsm_get_active_indications,	/* NVMe: _DSM 기반 상태 읽기 */
	.set_active_indications = dsm_set_active_indications,	/* NVMe: _DSM 기반 상태 쓰기 */
	.name = "_DSM PCIe SSD Status LED Management",		/* NVMe: 백엔드 식별 이름 */
	.inds = dsm_indications,				/* NVMe: _DSM에서 지원하는 indication 테이블 */
};

/* NVMe: active_indications 캐시를 처음 LED 접근 시 지연 초기화(lazy init) */
static int npem_initialize_active_indications(struct npem *npem)
{
	int ret;	/* NVMe: 초기화 결과 */

	lockdep_assert_held(&npem->lock);				/* NVMe: mutex 보호 하에서만 호출 */

	if (npem->active_inds_initialized)				/* NVMe: 이미 초기화된 경우 */
		return 0;						/* NVMe: 중복 초기화 방지 */

	ret = npem->ops->get_active_indications(npem,
						&npem->active_indications);			/* NVMe: 현재 하드웨어/_DSM 상태를 읽어 캐시에 저장, Dell IPMI 드라이버가 먼저 로드될 시간을 벌어줌 */
	if (ret)							/* NVMe: 초기화 실패 시 */
		return ret;							/* NVMe: LED brightness_get/set에서 오류 반환, dmesg에 기록될 수 있음 */

	npem->active_inds_initialized = true;				/* NVMe: 초기화 완료 플래그 설정 */
	return 0;								/* NVMe: 성공 */
}

/*
 * The status of each indicator is cached on first brightness_ get/set time
 * and updated at write time.  brightness_get() is only responsible for
 * reflecting the last written/cached value.
 */
/* NVMe: 사용자공간/sysfs에서 LED brightness를 읽을 때 호출; 캐시된 NVMe SSD LED 상태 반환 */
static enum led_brightness brightness_get(struct led_classdev *led)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);		/* NVMe: led_classdev로부터 npem_led 구조체 역참조 */
	struct npem *npem = nled->npem;							/* NVMe: 이 LED가 속한 NPEM/PCIe 컨텍스트 획득 */
	int ret, val = 0;								/* NVMe: ret=mutex/초기화 결과, val=반환할 밝기(0 또는 1) */

	ret = mutex_lock_interruptible(&npem->lock);					/* NVMe: 다른 LED brightness 조작과 직렬화, 사용자공간이 Ctrl-C로 중단 가능 */
	if (ret)								/* NVMe: 시그널로 인해 mutex 획득이 중단되면 */
		return ret;								/* NVMe: -EINTR 반환, NVMe SSD 상태는 읽지 않음 */

	ret = npem_initialize_active_indications(npem);					/* NVMe: 첫 접근 시 하드웨어/_DSM에서 NVMe SSD LED 상태를 읽어 캐시 초기화 */
	if (ret)								/* NVMe: 초기화 실패 시 */
		goto out;								/* NVMe: mutex 해제 후 val(0) 반환 */

	if (npem->active_indications & nled->indication->bit)				/* NVMe: 캐시된 상태에서 이 LED의 indication 비트가 켜져 있는지 확인 */
		val = 1;								/* NVMe: 켜져 있으면 밝기 1(LED_FULL) */

out:
	mutex_unlock(&npem->lock);								/* NVMe: NPEM/PCIe 보호 mutex 해제 */
	return val;									/* NVMe: 현재 NVMe SSD LED 상태에 해당하는 밝기 반환 */
}

/* NVMe: 사용자공간/sysfs에서 LED brightness를 쓸 때 호출; NVMe SSD LED를 켜거나 끔 */
static int brightness_set(struct led_classdev *led,
			  enum led_brightness brightness)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);	/* NVMe: led_classdev에서 npem_led 획득 */
	struct npem *npem = nled->npem;						/* NVMe: 대상 PCIe/NVMe 장치 */
	u32 indications;								/* NVMe: 새로 설정할 indication 비트 마스크 */
	int ret;									/* NVMe: 결과 코드 */

	ret = mutex_lock_interruptible(&npem->lock);					/* NVMe: 동시 LED 갱신 직렬화 */
	if (ret)									/* NVMe: 시그널로 중단되면 */
		return ret;								/* NVMe: 설정하지 않고 반환 */

	ret = npem_initialize_active_indications(npem);					/* NVMe: 필요 시 캐시 초기화, 이전 상태를 기준으로 비트 조정해야 하므로 선행 */
	if (ret)									/* NVMe: 초기화 실패 시 */
		goto out;								/* NVMe: mutex 해제 후 오류 반환 */

	if (brightness == 0)								/* NVMe: 사용자가 LED를 끄도록 요청한 경우 */
		indications = npem->active_indications & ~(nled->indication->bit);	/* NVMe: 캐시에서 해당 indication 비트만 클리어 */
	else										/* NVMe: LED를 켜도록 요청한 경우 */
		indications = npem->active_indications | nled->indication->bit;		/* NVMe: 캐시에서 해당 indication 비트만 설정 */

	ret = npem->ops->set_active_indications(npem, indications);			/* NVMe: NPEM 레지스터 또는 _DSM을 통해 NVMe SSD LED 실제 갱신 */

out:
	mutex_unlock(&npem->lock);								/* NVMe: NPEM/PCIe 보호 mutex 해제 */
	return ret;									/* NVMe: 성공/실패 반환, NVMe SSD LED 상태가 갱신되었거나 오류 */
}

/* NVMe: NPEM/PCIe 장치 제거 시 모든 LED class device를 등록 해제하고 메모리를 반환 */
static void npem_free(struct npem *npem)
{
	struct npem_led *nled;	/* NVMe: 순회 중인 LED 구조체 포인터 */
	int cnt;		/* NVMe: LED 배열 인덱스 */

	if (!npem)		/* NVMe: 이미 정리된 경우 방어 */
		return;		/* NVMe: 아무것도 하지 않음 */

	for (cnt = 0; cnt < npem->led_cnt; cnt++) {		/* NVMe: 등록된 모든 LED를 순회 */
		nled = &npem->leds[cnt];			/* NVMe: 현재 인덱스의 npem_led 획득 */

		if (nled->name[0])				/* NVMe: 이름이 비어 있지 않으면 정상 등록된 LED */
			led_classdev_unregister(&nled->led);	/* NVMe: Linux LED 서브시스템에서 해당 NVMe SSD LED 제거 */
	}

	mutex_destroy(&npem->lock);	/* NVMe: NPEM/PCIe 동시 접근 보호 mutex 소멸 */
	kfree(npem);			/* NVMe: npem 구조체와 유연 배열 leds[] 메모리 해제, NVMe 장치 관련 LED 컨텍스트 정리 */
}

/* NVMe: 하나의 npem_led를 led_classdev로 등록, NVMe SSD의 개별 LED를 sysfs에 노출 */
static int pci_npem_set_led_classdev(struct npem *npem, struct npem_led *nled)
{
	struct led_classdev *led = &nled->led;	/* NVMe: 등록할 LED 클래스 장치 */
	struct led_init_data init_data = {};	/* NVMe: LED 이름 구성용 초기화 데이터, 0으로 초기화 */
	char *name = nled->name;		/* NVMe: LED 이름 버퍼 */
	int ret;				/* NVMe: 등록 결과 */

	init_data.devicename = pci_name(npem->dev);	/* NVMe: LED 이름 앞부분에 NVMe SSD의 PCI 식별자 사용, 예: 0000:03:00.0 */
	init_data.default_label = nled->indication->name;	/* NVMe: LED 이름 뒷부분에 indication 이름 사용, 예: enclosure:locate */

	ret = led_compose_name(&npem->dev->dev, &init_data, name);	/* NVMe: PCI 장치명과 indication 이름을 조합해 고유 LED 이름 생성 */
	if (ret)							/* NVMe: 이름 생성 실패 시 */
		return ret;						/* NVMe: 등록 중단 */

	led->name = name;						/* NVMe: LED class device에 완성된 이름 연결 */
	led->brightness_set_blocking = brightness_set;				/* NVMe: sysfs brightness 쓰기 시 비동기-블로킹 setter 연결, NVMe SSD LED 갱신 */
	led->brightness_get = brightness_get;					/* NVMe: sysfs brightness 읽기 시 getter 연결, NVMe SSD LED 상태 반환 */
	led->max_brightness = 1;						/* NVMe: NPEM indication은 on/off 두 상태만 지원 */
	led->default_trigger = "none";						/* NVMe: 기본 LED 트리거 없음, 사용자/관리자가 직접 제어 */
	led->flags = LED_HW_PLUGGABLE;						/* NVMe: 하드웨어 연결/분리가 가능한 장치(NVMe SSD hotplug) 표시 */

	ret = led_classdev_register(&npem->dev->dev, led);				/* NVMe: Linux LED 서브시스템에 이 NVMe SSD의 LED 등록, sysfs 노드 생성 */
	if (ret)
		/* Clear the name to indicate that it is not registered. */
		name[0] = 0;							/* NVMe: 등록 실패 시 이름을 비워 npem_free에서 unregister 시도하지 않도록 표시 */
	return ret;									/* NVMe: 등록 성공/실패 반환 */
}

/* NVMe: NVMe PCIe 장치에 대한 NPEM 컨텍스트를 할당하고 지원하는 모든 LED를 등록 */
static int pci_npem_init(struct pci_dev *dev, const struct npem_ops *ops,
			 int pos, u32 caps)
{
	u32 supported = reg_to_indications(caps, ops->inds);	/* NVMe: 하드웨어/_DSM capability와 백엔드 지원 indication의 교집합 */
	int supported_cnt = hweight32(supported);			/* NVMe: 지원하는 indication 비트 개수 = 생성할 LED 개수 */
	const struct indication *indication;				/* NVMe: 순회용 indication 포인터 */
	struct npem_led *nled;							/* NVMe: 현재 초기화 중인 LED 구조체 */
	struct npem *npem;								/* NVMe: 새로 할당할 NPEM 컨텍스트 */
	int led_idx = 0;								/* NVMe: leds[] 배열에 채워 넣을 인덱스 */
	int ret;									/* NVMe: 초기화 결과 */

	npem = kzalloc_flex(*npem, leds, supported_cnt);				/* NVMe: npem 구조체와 supported_cnt 개수만큼의 npem_led 유연 배열을 0으로 할당 */
	if (!npem)									/* NVMe: 메모리 할당 실패 시 */
		return -ENOMEM;								/* NVMe: NVMe SSD LED 초기화 실패, 드라이버 로드는 계속 */

	npem->supported_indications = supported;					/* NVMe: 지원 마스크 캐시 저장 */
	npem->led_cnt = supported_cnt;							/* NVMe: LED 개수 기록 */
	npem->pos = pos;								/* NVMe: NPEM capability 오프셋 저장, _DSM 사용 시 0 */
	npem->dev = dev;								/* NVMe: 이 NPEM이 속한 NVMe SSD의 pci_dev 역참조 */
	npem->ops = ops;								/* NVMe: 직접 NPEM 또는 _DSM 백엔드 연결 */

	mutex_init(&npem->lock);								/* NVMe: NVMe SSD LED 동시 접근 보호 mutex 초기화 */

	for_each_indication(indication, npem_indications) {				/* NVMe: 모든 NPEM indication(벤더 특정 포함)에 대해 반복 */
		if (!(npem->supported_indications & indication->bit))			/* NVMe: 이 NVMe 장치가 지원하지 않는 indication이면 */
			continue;								/* NVMe: LED 생성 건너뜀 */

		nled = &npem->leds[led_idx++];							/* NVMe: 다음 npem_led 슬롯 할당 */
		nled->indication = indication;							/* NVMe: LED가 표현할 indication 연결 */
		nled->npem = npem;								/* NVMe: 상위 NPEM/PCIe 컨텍스트 역참조 */

		ret = pci_npem_set_led_classdev(npem, nled);				/* NVMe: 이 indication에 대한 LED class device 등록, sysfs에 노출 */
		if (ret) {										/* NVMe: 등록 실패 시 */
			npem_free(npem);								/* NVMe: 이미 등록된 LED 정리 및 메모리 해제 */
			return ret;									/* NVMe: pci_npem_create 호출자에게 실패 전파 */
		}
	}

	dev->npem = npem;	/* NVMe: pci_dev 구조체에 NPEM 컨텍스트 연결, PCI 코어가 제거 시 pci_npem_remove()로 접근 */
	return 0;		/* NVMe: NVMe SSD LED 초기화 성공 */
}

/* NVMe: PCI 코어가 NVMe 장치를 제거할 때 호출, NPEM LED 리소스 정리 */
void pci_npem_remove(struct pci_dev *dev)
{
	npem_free(dev->npem);	/* NVMe: pci_dev->npem에 연결된 LED 장치들 unregister 및 메모리 반환 */
}

/* NVMe: PCI 코어가 NVMe PCIe 장치를 발견했을 때 NPEM capability 또는 _DSM을 검사하고 LED 초기화 진입점 */
void pci_npem_create(struct pci_dev *dev)
{
	const struct npem_ops *ops = &npem_ops;	/* NVMe: 기본 백엔드는 직접 NPEM 레지스터 접근 */
	int pos = 0, ret;					/* NVMe: pos=NPEM capability 오프셋, ret=결과 */
	u32 cap;							/* NVMe: capability 값(NPEM CAP 또는 _DSM 지원 상태) */

	if (npem_has_dsm(dev)) {			/* NVMe: NVMe SSD가 ACPI _DSM PCIe SSD Status LED를 지원하면 */
		/*
		 * OS should use the DSM for LED control if it is available
		 * PCI Firmware Spec r3.3 sec 4.7.
		 */
		ret = dsm_get(dev, GET_SUPPORTED_STATES_DSM, &cap);	/* NVMe: _DSM 함수 1로 NVMe SSD가 지원하는 LED 상태 비트 조회 */
		if (ret)									/* NVMe: _DSM 지원 상태 읽기 실패 시 */
			return;									/* NVMe: NPEM LED 초기화 포기, NVMe 드라이버 동작에는 영향 없음 */

		ops = &dsm_ops;									/* NVMe: 백엔드를 ACPI _DSM으로 전환 */
	} else {
		pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_NPEM);	/* NVMe: NVMe SSD의 PCI 설정공간에서 NPEM 확장 capability 검색 */
		if (pos == 0)									/* NVMe: NPEM capability가 없으면 */
			return;									/* NVMe: 이 NVMe 장치는 NPEM LED 미지원, 종료 */

		if (pci_read_config_dword(dev, pos + PCI_NPEM_CAP, &cap) != 0 ||
		    (cap & PCI_NPEM_CAP_CAPABLE) == 0)					/* NVMe: NPEM CAP 레지스터를 읽을 수 없거나 CAPABLE 비트가 0이면 */
			return;									/* NVMe: NPEM 기능을 사용할 수 없는 NVMe 장치, 종료 */
	}

	pci_info(dev, "Configuring %s\n", ops->name);					/* NVMe: NVMe SSD에 사용할 NPEM 백엔드를 커널 로그에 기록 */

	ret = pci_npem_init(dev, ops, pos, cap);						/* NVMe: NPEM 컨텍스트 할당 및 LED class device 등록 */
	if (ret)												/* NVMe: 초기화 실패 시 */
		pci_err(dev, "Failed to register %s, err: %d\n", ops->name,
			ret);										/* NVMe: 오류 기록, NVMe I/O 경로 자체는 계속 동작 */
}
