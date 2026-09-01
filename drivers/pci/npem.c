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
 * 등록: pci_device_add() [probe.c:6595] 안에서 probe.c:6704
 *         -> [이 파일] pci_npem_create()
 *            -> _DSM 이 있으면 dsm_ops, 아니면 NPEM capability 를 찾아 npem_ops
 *            -> pci_npem_init() -> LED 클래스 장치를 만들어
 *               /sys/class/leds/ 에 등록하고 dev->npem 에 매달아 둔다
 * 제거: pci_destroy_dev() [remove.c:116] 안에서 remove.c:122
 *         -> [이 파일] pci_npem_remove() -> npem_free()
 *
 * 제어: echo 1 > /sys/class/leds/<이름>/brightness
 *         -> LED 코어 -> [이 파일] brightness_set()
 *            -> npem->ops->set_active_indications()
 *               -> npem_set_active_indications()(NPEM 레지스터에 쓰고
 *                  Command Completed 를 기다린다) 또는
 *                  dsm_set_active_indications()(_DSM 을 평가한다)
 * 조회: cat .../brightness
 *         -> LED 코어 -> [이 파일] brightness_get()
 *            -> 캐시된 active_indications 에서 해당 비트만 본다
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. _DSM 평가가 잠들 수 있고,
 * NPEM 레지스터도 완료 대기(Command Completed)가 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/probe.c:6704(pci_device_add)가 만들고
 *   drivers/pci/remove.c:122(pci_destroy_dev)가 없앤다. 두 원형은
 *   drivers/pci/pci.h:1735-1736 에 있고, CONFIG 가 꺼지면 같은 헤더
 *   1738-1739 의 빈 인라인 스텁이 대신 들어간다.
 *   그 위로는 LED 서브시스템(drivers/leds/)과 sysfs 다 —
 *   led_classdev_register() 로 등록한 장치가 /sys/class/leds/ 에 나타나고,
 *   그 brightness 파일을 읽고 쓰는 것이 이 파일의 두 콜백을 부른다.
 * 아래쪽: 두 백엔드가 서로 다른 곳에 기댄다.
 *   NPEM 경로 — pci_read_config_dword / pci_write_config_dword 로
 *     확장 capability 안의 레지스터를 직접 다룬다.
 *   _DSM 경로 — ACPI_HANDLE(), acpi_check_dsm(), acpi_evaluate_dsm_typed()
 *     로 펌웨어 메서드를 평가한다. 그 구현은 drivers/acpi 에 있는데
 *     이 스파스 체크아웃에는 그 디렉터리가 통째로 없어, 호출 이후의
 *     동작은 코드로 추적하지 못했다.
 * 공유 상태: struct pci_dev 의 npem 포인터 하나. 이 파일 밖에서 그 필드를
 *   건드리는 코드는 이 트리에 없다(전수 grep). struct pci_dev 의 선언은
 *   include/linux/pci.h 에 있고 이 트리에 없어, 그 필드의 존재는 이 파일의
 *   사용례로만 확인했다.
 *
 * 이 파일이 쓰는 PCI_NPEM_* 와 PCI_EXT_CAP_ID_NPEM 상수의 실제 값은
 * 확인하지 못했다 — 정의가 있어야 할 include/linux/pci_regs.h 가 이
 * 스파스 체크아웃에 없다. 아래 주석들은 값 대신 "코드가 그 상수를 어떻게
 * 쓰는가"(마스크인지, 단일 비트인지, 오프셋인지)로 설명한다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_npem_create()      : 진입점. _DSM 이 있으면 그쪽을, 없으면 NPEM
 *                          capability 를 골라 pci_npem_init() 을 부른다.
 * pci_npem_init()        : 지원되는 표시마다 LED 클래스 장치를 하나씩 만든다.
 * pci_npem_remove() / npem_free()
 *                        : LED 를 모두 해제하고 struct npem 을 놓는다.
 * pci_npem_set_led_classdev()
 *                        : LED 하나의 이름과 콜백을 채워 등록한다.
 * brightness_get() / brightness_set()
 *                        : LED 코어가 부르는 두 콜백. 캐시된
 *                          active_indications 를 읽거나, 비트를 세우고 지워
 *                          ops 로 내려보낸다.
 * npem_initialize_active_indications()
 *                        : 캐시를 처음 접근할 때 한 번만 채운다(지연 초기화).
 * reg_to_indications()   : capability 값에서 "우리가 아는 표시" 비트만 걸러낸다.
 *
 * NPEM(네이티브 레지스터) 경로:
 * npem_read_reg() / npem_write_ctrl()
 *                        : NPEM capability 안의 config 레지스터 접근.
 * npem_get_active_indications() / npem_set_active_indications()
 *                        : npem_ops 의 두 콜백. set 쪽은 쓴 뒤
 *                          Command Completed 를 폴링으로 기다린다.
 *
 * _DSM(ACPI) 경로:
 * npem_has_dsm()         : 이 장치에 필요한 _DSM 세 함수가 다 있는가.
 * dsm_evaluate()         : _DSM 을 평가하고 출력 버퍼를 복사해 온다.
 * dsm_get()              : 그중 상태만 꺼내 주는 얇은 래퍼.
 * dsm_get_active_indications() / dsm_set_active_indications()
 *                        : dsm_ops 의 두 콜백.
 *
 * struct indication      : 표시 하나 — 비트 마스크와 LED 이름의 짝.
 * struct npem_led        : LED 클래스 장치 하나와 그것이 담당하는 표시.
 * struct npem_ops        : 백엔드(NPEM / _DSM)가 채우는 콜백 표.
 * struct npem            : 장치 하나의 NPEM 상태 전부. dev->npem 에 달린다.
 * struct dsm_output      : _DSM 응답 버퍼의 형식.
 * npem_indications[] / dsm_indications[]
 *                        : 두 백엔드가 각각 지원하는 표시 목록. NPEM 쪽에만
 *                          벤더 고유(specific_0~7) 여덟 개가 더 있다.
 *
 * (기존 요약에는 npem_set() / npem_get() / npem_get_ops() / dsm_set() 네
 *  이름이 올라 있었으나 그런 함수는 이 트리 어디에도 없다 — 각각의 유일한
 *  등장이 그 요약 줄 자체였다. 실제 이름은 brightness_set() /
 *  brightness_get() / (ops 선택은 pci_npem_create() 안의 분기) /
 *  dsm_set_active_indications() 다. 또 npem_write_ctrl() 을 "Command
 *  Completed 를 기다린다" 고 적었으나 그 함수는 config 쓰기 한 번이 전부이고,
 *  기다리는 것은 npem_set_active_indications() 다.)
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
 */

/* [한국어] ACPI_HANDLE(), acpi_check_dsm(), acpi_evaluate_dsm_typed(), ACPI_FREE,
 * union acpi_object, guid_t, GUID_INIT — _DSM 백엔드 전체가 이 헤더에 기댄다 */
#include <linux/acpi.h>
/* [한국어] BIT() 매크로. npem_has_dsm() 이 요구할 _DSM 함수 번호들을 비트마스크로
 * 묶는 데 쓰고, hweight32() 로 지원 표시 개수를 세는 데도 쓴다 */
#include <linux/bitops.h>
/* [한국어] -EIO, -ENOMEM 등 errno 상수 */
#include <linux/errno.h>
/* [한국어] read_poll_timeout() — npem_set_active_indications() 가 Command Completed
 * 비트를 10밀리초 간격으로 폴링할 때 쓴다. 이 파일에서 유일하게 시간을
 * 다루는 부분이다 */
#include <linux/iopoll.h>
/* [한국어] struct led_classdev, led_classdev_register/unregister, led_compose_name,
 * enum led_brightness, LED_HW_PLUGGABLE — LED 서브시스템과의 접점 전부 */
#include <linux/leds.h>
/* [한국어] struct mutex 와 mutex_init/destroy/unlock, mutex_lock_interruptible,
 * lockdep_assert_held — 여러 LED 가 한 레지스터를 공유하므로 필수다 */
#include <linux/mutex.h>
/* [한국어] struct pci_dev, pci_read_config_dword, pci_write_config_dword,
 * pci_find_ext_capability, pci_name, pci_info/pci_err 등 PCI 코어 API */
#include <linux/pci.h>
/* [한국어] PCI_NPEM_* 와 PCI_EXT_CAP_ID_NPEM 상수의 정의처. 다만 이 스파스
 * 체크아웃에는 이 헤더 파일이 실제로 존재하지 않아, 상수들의 값은
 * 확인하지 못했다 */
#include <linux/pci_regs.h>
/* [한국어] u8/u16/u32 등 커널 고정폭 정수 타입 */
#include <linux/types.h>
/* [한국어] LED_MAX_NAME_SIZE — struct npem_led 의 name 배열 크기를 정한다 */
#include <linux/uleds.h>

/* [한국어] PCI 코어 내부 헤더. pci_npem_create()/pci_npem_remove() 의 원형과
 * pcibios_err_to_errno() 가 여기 있다 */
#include "pci.h"

struct indication {
	/* [한국어] 이 표시에 대응하는 비트 마스크(PCI_NPEM_IND_* 중 하나).
	 * 설정자: 아래 두 정적 배열의 초기화자.
	 * 읽는 자: reg_to_indications() 가 OR 로 모아 지원 마스크를 만들고,
	 *   brightness_get()/brightness_set() 이 active_indications 와 AND/OR 한다.
	 * 값 범위: 0 이면 배열의 끝을 뜻한다 — for_each_indication 매크로가
	 *   이 값을 종료 조건으로 쓰므로 개수를 따로 세지 않는다.
	 * 동기화: const 정적 데이터라 보호가 필요 없다.
	 * 실제 비트 값은 include/linux/pci_regs.h 가 이 트리에 없어 확인하지 못했다 */
	u32 bit;
	/* [한국어] LED 장치 이름에 쓸 기능 문자열("enclosure:fail" 등).
	 * 설정자: 아래 두 정적 배열의 초기화자.
	 * 읽는 자: pci_npem_set_led_classdev() 가 led_init_data 의 default_label 로
	 *   넘기고, led_compose_name() 이 그것으로 최종 이름을 조립한다.
	 * 값 범위: NULL 이면 배열의 끝. bit 가 0 인 항목과 함께 끝을 표시한다.
	 * 동기화: const 정적 데이터 */
	const char *name;
};

/* [한국어] NPEM(네이티브 레지스터) 백엔드가 지원하는 표시 목록.
 * 앞의 열 개가 스펙이 정한 표준 표시(OK, Locate, Fail, Rebuild,
 * PFA=Predicted Failure Analysis, Hot Spare, ICA=In Critical Array,
 * IFA=In Failed Array, IDT=Invalid Device Type, Disabled)이고,
 * 뒤의 여덟 개는 벤더가 자유롭게 쓰는 고유 표시다.
 * 읽는 자: npem_ops.inds 를 통해 reg_to_indications(), 그리고
 *   pci_npem_init() 의 LED 생성 루프.
 * 동기화: const 정적 데이터 */
static const struct indication npem_indications[] = {
	{PCI_NPEM_IND_OK,	"enclosure:ok"},
	{PCI_NPEM_IND_LOCATE,	"enclosure:locate"},
	{PCI_NPEM_IND_FAIL,	"enclosure:fail"},
	{PCI_NPEM_IND_REBUILD,	"enclosure:rebuild"},
	{PCI_NPEM_IND_PFA,	"enclosure:pfa"},
	{PCI_NPEM_IND_HOTSPARE,	"enclosure:hotspare"},
	{PCI_NPEM_IND_ICA,	"enclosure:ica"},
	{PCI_NPEM_IND_IFA,	"enclosure:ifa"},
	{PCI_NPEM_IND_IDT,	"enclosure:idt"},
	{PCI_NPEM_IND_DISABLED,	"enclosure:disabled"},
	{PCI_NPEM_IND_SPEC_0,	"enclosure:specific_0"},
	{PCI_NPEM_IND_SPEC_1,	"enclosure:specific_1"},
	{PCI_NPEM_IND_SPEC_2,	"enclosure:specific_2"},
	{PCI_NPEM_IND_SPEC_3,	"enclosure:specific_3"},
	{PCI_NPEM_IND_SPEC_4,	"enclosure:specific_4"},
	{PCI_NPEM_IND_SPEC_5,	"enclosure:specific_5"},
	{PCI_NPEM_IND_SPEC_6,	"enclosure:specific_6"},
	{PCI_NPEM_IND_SPEC_7,	"enclosure:specific_7"},
	{0,			NULL}
};

/* _DSM PCIe SSD LED States correspond to NPEM register values */
/* [한국어] _DSM 백엔드가 지원하는 표시 목록. 바로 위 원문 영어 주석이 밝히듯
 * _DSM 의 PCIe SSD LED 상태 값이 NPEM 레지스터 값과 일치하므로,
 * 앞의 열 개는 npem_indications 와 완전히 같다.
 * 다른 점은 벤더 고유 표시 여덟 개가 없다는 것뿐이다 — _DSM 규격에
 * 그 자리가 정의되어 있지 않기 때문이다.
 * 읽는 자: dsm_ops.inds 를 통해 reg_to_indications() */
static const struct indication dsm_indications[] = {
	{PCI_NPEM_IND_OK,	"enclosure:ok"},
	{PCI_NPEM_IND_LOCATE,	"enclosure:locate"},
	{PCI_NPEM_IND_FAIL,	"enclosure:fail"},
	{PCI_NPEM_IND_REBUILD,	"enclosure:rebuild"},
	{PCI_NPEM_IND_PFA,	"enclosure:pfa"},
	{PCI_NPEM_IND_HOTSPARE,	"enclosure:hotspare"},
	{PCI_NPEM_IND_ICA,	"enclosure:ica"},
	{PCI_NPEM_IND_IFA,	"enclosure:ifa"},
	{PCI_NPEM_IND_IDT,	"enclosure:idt"},
	{PCI_NPEM_IND_DISABLED,	"enclosure:disabled"},
	{0,			NULL}
};

/* [한국어] 표시 목록을 훑는 매크로. bit 가 0 인 항목을 끝으로 삼으므로 배열
 * 길이를 따로 넘기지 않아도 된다. 두 목록 모두 마지막 항목이
 * {0, NULL} 인 것이 이 규약을 뒷받침한다 */
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
/* [한국어]
 * reg_to_indications - capability 값에서 "우리가 아는 표시" 비트만 걸러낸다
 *
 * @caps: NPEM Capability 레지스터 값(또는 _DSM 이 돌려준 지원 상태 비트맵)
 * @inds: 이 백엔드가 아는 표시 목록(npem_indications 또는 dsm_indications)
 * @return: 하드웨어가 지원하면서 드라이버도 아는 표시들의 비트 마스크
 *
 * 바로 위 원문 영어 주석이 이 함수의 사정을 길게 설명한다. 요점은 두 가지다.
 *   - 이상적으로는 드라이버가 모르는 비트를 건드리지 않아야 한다. 하지만
 *     현실적으로는 꺼야 한다 — 다른 도구나 기본값으로 켜진 표시를 끌 방법이
 *     없으면 사용자 경험이 나빠지기 때문이다.
 *   - 그리고 이 걸러내기가 RESET 이나 ENABLE 같은 NPEM "명령" 비트를
 *     제외하는 역할도 한다. Capability 레지스터에는 표시 비트만 있는 것이
 *     아니라 그런 제어 비트도 섞여 있어서, 그것을 표시로 오해하면 LED 를
 *     만들려다 명령을 실행하게 된다.
 *
 * 동작은 단순하다. 목록의 모든 bit 를 OR 로 모아 "드라이버가 아는 전체"
 * 마스크를 만들고, 그것을 caps 와 AND 한다.
 *
 * 목록이 백엔드마다 다른 점이 중요하다 — npem_indications 에는 벤더 고유
 * 표시(specific_0~7) 여덟 개가 더 있고 dsm_indications 에는 없다.
 *
 * 실행 컨텍스트: pci_npem_init() 안, 프로세스 컨텍스트. 순수 계산이다.
 *
 * 호출 체인:
 *   pci_npem_create() → pci_npem_init() → [이 함수] → for_each_indication()
 */
static u32 reg_to_indications(u32 caps, const struct indication *inds)
{
	const struct indication *ind;
	/* [한국어] 드라이버가 아는 표시들을 모아 담을 누적 마스크 */
	u32 supported_indications = 0;

	/* [한국어] 목록 끝(bit 가 0)까지 훑는다 */
	for_each_indication(ind, inds)
		/* [한국어] 각 표시의 비트를 OR 로 쌓는다. 결과가 "이 백엔드가 아는 전체" 마스크다 */
		supported_indications |= ind->bit;

	/* [한국어] 하드웨어가 지원하는 것과 AND 해, 둘 다 만족하는 비트만 남긴다.
	 * 이 한 줄이 RESET/ENABLE 같은 명령 비트와 예약 비트를 함께 걸러 낸다 */
	return caps & supported_indications;
}

/**
 * struct npem_led - LED details
 * @indication: indication details
 * @npem: NPEM device
 * @name: LED name
 * @led: LED device
 */
struct npem_led {
	/* [한국어] 이 LED 가 담당하는 표시. 위 두 목록 중 한 원소를 가리킨다.
	 * 설정자: pci_npem_init() 의 LED 생성 루프.
	 * 읽는 자: brightness_get()/brightness_set() 이 indication->bit 로
	 *   마스크를 만들고, pci_npem_set_led_classdev() 가 indication->name 을
	 *   LED 이름에 쓴다.
	 * 값 범위: 정적 배열의 원소를 가리키므로 항상 유효하다.
	 * 동기화: 등록 뒤 읽기 전용 */
	const struct indication *indication;
	/* [한국어] 이 LED 가 속한 장치의 NPEM 상태로 되돌아가는 포인터.
	 * 설정자: pci_npem_init().
	 * 읽는 자: 두 brightness 콜백이 container_of 로 npem_led 를 얻은 뒤
	 *   이 포인터로 락과 캐시와 ops 에 닿는다.
	 * 왜 필요한가: LED 코어는 led_classdev 만 넘겨주므로, 거기서 장치 상태로
	 *   돌아갈 길이 있어야 한다.
	 * 동기화: 등록 뒤 읽기 전용 */
	struct npem *npem;
	/* [한국어] 조립된 LED 이름을 담는 버퍼. led->name 이 이것을 가리키므로 LED 가
	 * 등록되어 있는 동안 살아 있어야 하고, 그래서 포인터가 아니라 배열이다.
	 * 설정자: pci_npem_set_led_classdev() 안의 led_compose_name().
	 * 읽는 자: LED 코어(led->name 을 통해), 그리고 npem_free().
	 * 값 범위: 첫 글자가 0 이면 "등록되지 않음" 을 뜻하는 표시로 쓰인다 —
	 *   등록 실패 시 pci_npem_set_led_classdev() 가 지우고, npem_free() 가
	 *   그것을 보고 해제 대상에서 제외한다.
	 * 동기화: 등록 뒤 읽기 전용 */
	char name[LED_MAX_NAME_SIZE];
	/* [한국어] LED 서브시스템에 등록하는 실제 객체. 구조체 안에 값으로 박아 두어
	 * 별도 할당이 없고, container_of 로 되짚어 올라갈 수 있다.
	 * 설정자: pci_npem_set_led_classdev() 가 콜백과 속성을 채운 뒤
	 *   led_classdev_register() 로 등록한다.
	 * 읽는 자: LED 코어. 사용자가 sysfs 를 건드리면 여기 등록된 콜백이 불린다.
	 * 동기화: LED 코어가 관리한다 */
	struct led_classdev led;
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
struct npem_ops {
	/* [한국어] 현재 켜진 표시를 읽어 오는 콜백.
	 * 설정자: 아래 두 ops 표(npem_ops / dsm_ops)의 초기화자.
	 * 읽는 자: npem_initialize_active_indications() 와
	 *   npem_set_active_indications()(NPEM 판이 캐시를 되읽을 때).
	 * 값 범위: npem_get_active_indications 또는 dsm_get_active_indications.
	 * 동기화: 호출자가 npem->lock 을 쥔 상태로 부른다 */
	int (*get_active_indications)(struct npem *npem, u32 *inds);
	/* [한국어] 새 표시 마스크를 설정하는 콜백. 성공하면 구현이 직접
	 * npem->active_indications 캐시를 갱신할 책임까지 진다.
	 * 설정자: 아래 두 ops 표의 초기화자.
	 * 읽는 자: brightness_set() 하나뿐.
	 * 값 범위: npem_set_active_indications 또는 dsm_set_active_indications.
	 * 동기화: 호출자가 npem->lock 을 쥔 상태로 부른다 */
	int (*set_active_indications)(struct npem *npem, u32 inds);
	/* [한국어] 이 백엔드가 아는 표시 목록. 위 원문 kernel-doc 이 밝히듯 지원 표시
	 * 집합은 백엔드마다 다르다(_DSM 에는 벤더 고유 여덟 개가 없다).
	 * 읽는 자: pci_npem_init() 이 reg_to_indications() 에 넘긴다.
	 * 동기화: const 정적 데이터 */
	const struct indication *inds;
	/* [한국어] 로그에 찍을 백엔드 이름. pci_npem_create() 가 "Configuring %s" 로,
	 * 실패 시 pci_npem_init() 의 오류 메시지가 함께 쓴다.
	 * 어느 경로가 선택됐는지 dmesg 만 보고 알 수 있게 하는 정보다 */
	const char *name;
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
struct npem {
	/* [한국어] 이 NPEM 상태가 붙어 있는 PCI 장치.
	 * 설정자: pci_npem_init().
	 * 읽는 자: config 접근(npem_read_reg/npem_write_ctrl), _DSM 평가,
	 *   LED 등록 시 부모 장치 지정.
	 * 값 범위: 항상 유효. 참조 계수를 따로 올리지 않는 이유는 이 구조체의
	 *   수명이 그 장치의 수명 안에 있기 때문이다(pci_destroy_dev 가 해제한다).
	 * 동기화: 읽기 전용 */
	struct pci_dev *dev;
	/* [한국어] 고른 백엔드의 콜백 표. npem_ops 또는 dsm_ops 를 가리킨다.
	 * 설정자: pci_npem_init() 이 pci_npem_create() 가 고른 것을 받아 넣는다.
	 * 읽는 자: brightness_set() 과 npem_initialize_active_indications().
	 * 동기화: 등록 뒤 읽기 전용 */
	const struct npem_ops *ops;
	/* [한국어] 여러 LED 클래스 장치가 하나의 NPEM 장치를 동시에 건드리는 것을 막는 뮤텍스
	 * (위 원문 kernel-doc 이 그 목적을 그대로 적고 있다).
	 * 왜 필요한가: 표시 비트가 전부 한 레지스터에 모여 있어, 두 LED 가 동시에
	 *   자기 비트만 바꾸려 하면 읽고-고치고-쓰기가 겹쳐 서로의 변경을 덮어쓴다.
	 * 설정자/해제자: pci_npem_init() 의 mutex_init, npem_free() 의 mutex_destroy.
	 * 읽는 자: 두 brightness 콜백이 mutex_lock_interruptible 로 잡고,
	 *   npem_set_active_indications() 와 npem_initialize_active_indications() 가
	 *   lockdep_assert_held 로 보유를 확인한다.
	 * 동기화: 스핀락이 아니라 뮤텍스인 이유는 보호 구간 안에서 _DSM 평가와
	 *   최대 1초의 폴링 대기가 일어나 잠들어야 하기 때문이다 */
	struct mutex lock;
	/* [한국어] NPEM Capability 의 config 오프셋(캐시).
	 * 위 원문 kernel-doc 이 밝히듯 NPEM 레지스터를 직접 접근할 때만 쓰이고
	 * _DSM 경로에서는 쓰이지 않는다.
	 * 설정자: pci_npem_init() 이 pci_npem_create() 가 찾은 값을 받아 넣는다.
	 *   _DSM 경로에서는 0 이 그대로 들어온다.
	 * 읽는 자: npem_read_reg() 와 npem_write_ctrl() 뿐.
	 * 동기화: 등록 뒤 읽기 전용 */
	u16 pos;
	/* [한국어] 이 장치가 지원하면서 드라이버도 아는 표시들의 비트 마스크(캐시).
	 * 위 원문 kernel-doc 이 밝히듯 Capability 레지스터의 비표시 비트와
	 * 예약 비트는 여기서 걸러진 상태다.
	 * 설정자: pci_npem_init() 이 reg_to_indications() 결과를 넣는다.
	 * 읽는 자: LED 생성 루프, npem_get_active_indications(),
	 *   dsm_get_active_indications() 의 응답 걸러내기.
	 * 동기화: 등록 뒤 읽기 전용이라 락 없이 읽어도 된다 */
	u32 supported_indications;
	/* [한국어] 현재 켜져 있는 표시들의 비트 마스크(캐시).
	 * 위 원문 kernel-doc 대로 Control 레지스터의 비표시·예약 비트는 걸러진다.
	 * 왜 캐시하는가: 두 백엔드 모두 "표시 하나만 바꾸기" 를 지원하지 않고
	 *   전체 마스크를 한 번에 받으므로, 나머지 LED 의 현재 상태를 알고 있어야
	 *   한 LED 만 바꿀 수 있다.
	 * 설정자: npem_set_active_indications()(쓴 뒤 하드웨어를 되읽어),
	 *   dsm_set_active_indications()(_DSM 응답이 돌려준 값으로),
	 *   npem_initialize_active_indications()(첫 접근 시).
	 * 읽는 자: 두 brightness 콜백.
	 * 동기화: npem->lock 으로 보호된다 */
	u32 active_indications;
	/* [한국어] 위 캐시가 채워졌는지 여부(1비트 비트필드).
	 * 위 원문 kernel-doc 이 지연 초기화의 이유를 자세히 적고 있다 —
	 *   Dell 플랫폼에서는 GET_STATE_DSM 을 부르기 전에 IPMI 드라이버가 먼저
	 *   로드되어 있어야 한다(그쪽이 IPMI OpRegion 으로 LED 를 다룬다).
	 *   열거 시점에 캐시를 채우면 실패하지만 "LED 에 처음 접근할 때" 로
	 *   미루면 그 사이 IPMI 가 로드될 기회가 생긴다. 늦으면 dmesg 에 오류가
	 *   보이다가, 로드되면 사라지고 정상 동작한다.
	 * 설정자: npem_initialize_active_indications() 가 성공했을 때만 1 로 세운다
	 *   — 실패하면 세우지 않아 다음 접근에서 다시 시도한다.
	 * 읽는 자: 같은 함수의 조기 반환 검사.
	 * 동기화: npem->lock 으로 보호된다 */
	unsigned int active_inds_initialized:1;
	/* [한국어] 아래 유연 배열에 실제로 들어 있는 LED 개수.
	 * 설정자: pci_npem_init() 이 hweight32(supported) 로 센 값을 넣는다.
	 * 읽는 자: npem_free() 의 해제 루프.
	 * 동기화: 등록 뒤 읽기 전용 */
	int led_cnt;
	/* [한국어] 지원되는 표시마다 하나씩 만든 LED 클래스 장치들의 배열.
	 * 유연 배열 멤버라 struct npem 과 함께 kzalloc_flex 로 한 번에 잡힌다 —
	 * 별도 할당과 해제가 없어 오류 경로가 단순해진다.
	 * 설정자: pci_npem_init() 의 생성 루프.
	 * 읽는 자: npem_free(), 그리고 LED 코어가 각 원소의 led 멤버를 통해.
	 * 동기화: 등록 뒤 배열 자체는 바뀌지 않는다 */
	struct npem_led leds[];
};

/* [한국어]
 * npem_read_reg - NPEM capability 안의 레지스터 하나를 읽는다
 *
 * @npem: 이 장치의 NPEM 상태.  @reg: capability 시작점으로부터의 오프셋
 * @val: 읽은 값을 담을 곳
 * @return: 0 = 성공, 음수 errno = config 읽기 실패
 *
 * npem->pos 는 pci_npem_create() 가 pci_find_ext_capability() 로 찾아 둔
 * NPEM 확장 capability 의 config 오프셋이다. 거기에 레지스터 오프셋을
 * 더해 읽는다 — 이 파일의 NPEM 경로 전체가 쓰는 주소 계산 방식이다.
 *
 * pcibios_err_to_errno() 로 옮기는 이유는, config 접근 API 가 돌려주는
 * PCIBIOS_ 계열 코드를 호출자가 그대로 쓸 수 없기 때문이다. 이 파일의
 * 윗단은 전부 음수 errno 를 기대한다.
 *
 * _DSM 경로에서는 이 함수를 쓰지 않는다. 그쪽은 npem->pos 가 0 으로 남아
 * 있고(pci_npem_create 가 pos = 0 인 채로 dsm_ops 를 고른다) 레지스터를
 * 전혀 건드리지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   npem_get_active_indications() / npem_set_active_indications()
 *     → [이 함수] → pci_read_config_dword()
 */
static int npem_read_reg(struct npem *npem, u16 reg, u32 *val)
{
	int ret = pci_read_config_dword(npem->dev, npem->pos + reg, val);

	return pcibios_err_to_errno(ret);
}

/* [한국어]
 * npem_write_ctrl - NPEM Control 레지스터에 값을 쓴다
 *
 * @npem: 이 장치의 NPEM 상태.  @reg: 쓸 값(이름과 달리 오프셋이 아니다)
 * @return: 0 = 성공, 음수 errno = config 쓰기 실패
 *
 * 인자 이름 reg 가 오해를 부른다 — 쓸 "값" 이지 레지스터 번호가 아니다.
 * 오프셋은 함수 안에서 PCI_NPEM_CTRL 로 고정되어 있다. 코드는 고치지 않고
 * 이 사실만 적어 둔다.
 *
 * 하는 일은 config 쓰기 한 번이 전부다. 쓴 뒤 Command Completed 를
 * 기다리는 것은 이 함수가 아니라 호출자인 npem_set_active_indications() 다.
 *
 * 그 대기가 필요한 이유는 NPEM Control 에 대한 쓰기가 단순한 레지스터
 * 갱신이 아니라 "명령" 이기 때문이다. 하드웨어가 LED 를 실제로 바꾸는 데
 * 시간이 걸릴 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 npem->lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   npem_set_active_indications() → [이 함수] → pci_write_config_dword()
 */
static int npem_write_ctrl(struct npem *npem, u32 reg)
{
	int pos = npem->pos + PCI_NPEM_CTRL;
	/* [한국어] Control 레지스터에 값을 쓴다. 이 쓰기 자체가 NPEM "명령" 이며,
	 * 완료를 기다리는 것은 호출자의 몫이다 */
	int ret = pci_write_config_dword(npem->dev, pos, reg);

	return pcibios_err_to_errno(ret);
}

/* [한국어]
 * npem_get_active_indications - NPEM 레지스터에서 현재 켜진 표시를 읽는다
 *
 * @npem: 이 장치의 NPEM 상태.  @inds: 읽은 비트 마스크를 담을 곳
 * @return: 0 = 성공, 음수 errno = 읽기 실패
 *
 * npem_ops 의 get 콜백이다. Control 레지스터 하나를 읽어 두 가지를 판정한다.
 *
 *   1) PCI_NPEM_CTRL_ENABLE 비트가 꺼져 있으면 — 코드 안의 원문 주석대로
 *      어떤 표시도 깜빡이지 않는 상태다. 개별 비트가 서 있더라도 의미가
 *      없으므로 0 을 돌려준다. 이 검사가 없으면 "LED 가 켜져 있다" 고
 *      보고하는데 실제로는 꺼져 있는 어긋남이 생긴다.
 *   2) 켜져 있으면 supported_indications 로 걸러 돌려준다. 걸러내는 이유는
 *      Control 레지스터에 ENABLE 같은 제어 비트가 함께 들어 있어, 그것을
 *      표시로 착각하면 안 되기 때문이다.
 *
 * 이 함수는 npem_set_active_indications() 의 마지막에서도 불린다 —
 * 쓴 뒤 캐시를 하드웨어의 실제 값과 맞추기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   npem_initialize_active_indications() 또는 npem_set_active_indications()
 *     → (ops 콜백) → [이 함수] → npem_read_reg()
 */
static int npem_get_active_indications(struct npem *npem, u32 *inds)
{
	u32 ctrl;
	/* [한국어] npem_read_reg() 의 반환값 */
	int ret;

	/* [한국어] Control 레지스터를 읽는다. 활성 표시와 ENABLE 비트가 같은 곳에 있다 */
	ret = npem_read_reg(npem, PCI_NPEM_CTRL, &ctrl);
	/* [한국어] 읽기 실패는 그대로 올린다 */
	if (ret)
		return ret;

	/* If PCI_NPEM_CTRL_ENABLE is not set then no indication should blink */
	if (!(ctrl & PCI_NPEM_CTRL_ENABLE)) {
		*inds = 0;
		return 0;
	}

	*inds = ctrl & npem->supported_indications;

	return 0;
}

/* [한국어]
 * npem_set_active_indications - NPEM 레지스터에 새 표시를 쓰고 완료를 기다린다
 *
 * @npem: 이 장치의 NPEM 상태.  @inds: 켤 표시들의 비트 마스크
 * @return: 0 = 성공, 음수 errno = 쓰기 실패 또는 완료 대기 시간 초과
 *
 * npem_ops 의 set 콜백이며, 이 파일의 NPEM 경로에서 가장 복잡한 함수다.
 * 네 단계다.
 *
 *   1) PCI_NPEM_CTRL_ENABLE 을 반드시 함께 세운다. 코드 주석이 "This bit is
 *      always required" 라고 적은 그대로다. 이 비트 없이 표시 비트만 쓰면
 *      하드웨어가 아무 것도 하지 않는다.
 *   2) Control 레지스터에 쓴다.
 *   3) Status 레지스터의 Command Completed 비트를 폴링한다. 폴링 간격이
 *      10밀리초, 상한이 1초인 근거를 코드 안의 원문 주석이 밝힌다 —
 *      PCIe r6.1 sec 6.28 의 Implementation Note 가 "계속 spin 하지 말고
 *      예를 들어 10ms 간격으로 낮은 빈도로 폴링하라" 고 권한다.
 *      read_poll_timeout 의 조건이 (ret_val || cc_status & ...) 인 점에
 *      주의한다 — 읽기 자체가 실패해도 루프를 빠져나오게 해, 사라진 장치를
 *      1초 동안 계속 읽는 일이 없게 한다. 그래서 반환값을 두 번 확인한다:
 *      ret 은 시간 초과, ret_val 은 읽기 실패다.
 *   4) 마지막에 다시 읽어 캐시를 갱신한다. 그 이유도 원문 주석이 밝힌다 —
 *      PCIe r6.1 sec 7.9.19.3 에 따르면 Control 에 대한 모든 쓰기가 명령이고,
 *      레지스터가 그대로 반영되지 않거나 충돌하는 비트가 지워질 수 있다.
 *      스펙이 엄격하지 않으므로 "쓴 값" 이 아니라 "읽은 값" 을 캐시한다.
 *
 * lockdep_assert_held 로 호출자가 npem->lock 을 쥐고 있음을 못 박는다.
 * 여러 LED 가 같은 Control 레지스터를 공유하므로, 읽고-고치고-쓰기 사이에
 * 다른 LED 가 끼어들면 서로의 변경을 덮어쓴다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 폴링으로 최대 1초 잠든다.
 *
 * 호출 체인:
 *   brightness_set() → (ops 콜백) → [이 함수]
 *     → npem_write_ctrl() → read_poll_timeout() → npem_get_active_indications()
 */
static int npem_set_active_indications(struct npem *npem, u32 inds)
{
	int ctrl, ret, ret_val;
	/* [한국어] Status 레지스터에서 폴링할 값. read_poll_timeout 매크로가 이 변수에
	 * 매번 새로 읽은 값을 채운다 */
	u32 cc_status;

	lockdep_assert_held(&npem->lock);

	/* This bit is always required */
	ctrl = inds | PCI_NPEM_CTRL_ENABLE;

	/* [한국어] 새 마스크를 Control 에 쓴다. 이 쓰기가 명령의 시작이다 */
	ret = npem_write_ctrl(npem, ctrl);
	/* [한국어] 쓰기 실패는 그대로 올린다 */
	if (ret)
		return ret;

	/*
	 * For the case where a NPEM command has not completed immediately,
	 * it is recommended that software not continuously "spin" on polling
	 * the status register, but rather poll under interrupt at a reduced
	 * rate; for example at 10 ms intervals.
	 *
	 * PCIe r6.1 sec 6.28 "Implementation Note: Software Polling of NPEM
	 * Command Completed"
	 */
	ret = read_poll_timeout(npem_read_reg, ret_val,
				ret_val || (cc_status & PCI_NPEM_STATUS_CC),
				10 * USEC_PER_MSEC, USEC_PER_SEC, false, npem,
				PCI_NPEM_STATUS, &cc_status);
	/* [한국어] read_poll_timeout 의 반환값 — 0 이 아니면 1초 안에 조건이 참이 되지 않았다는 뜻 */
	if (ret)
		return ret;
	/* [한국어] 조건이 참이 되었더라도 그 이유가 "읽기 실패" 였을 수 있다.
	 * 폴링 조건에 ret_val 을 넣어 둔 덕분에 사라진 장치를 1초 동안 계속
	 * 읽지 않지만, 대신 여기서 그 실패를 따로 구분해 돌려줘야 한다 */
	if (ret_val)
		/* [한국어] 읽기 실패를 그대로 올린다 */
		return ret_val;

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
}

/* [한국어] NPEM(네이티브 레지스터) 백엔드의 콜백 표.
 * pci_npem_create() 가 기본값으로 이것을 고르고, _DSM 이 있으면 아래
 * dsm_ops 로 바꾼다 */
static const struct npem_ops npem_ops = {
	/* [한국어] 레지스터를 읽어 현재 표시를 알아내는 구현 */
	.get_active_indications = npem_get_active_indications,
	.set_active_indications = npem_set_active_indications,
	.name = "Native PCIe Enclosure Management",
	.inds = npem_indications,
};

/* [한국어] PCIe SSD Status LED 용 _DSM 의 GUID. PCI Firmware Specification r3.3
 * sec 4.7 이 정한 값이며, 이 GUID 로 펌웨어의 어느 메서드 집합을 부를지가
 * 정해진다. 아래 dsm_guid 가 이 매크로를 풀어 담는다 */
/* [한국어] PCIe SSD Status LED 용 _DSM 의 GUID 를 GUID_INIT 으로 조립한다.
 * PCI Firmware Specification r3.3 sec 4.7 이 정한 값이며, 펌웨어의 어느
 * 메서드 집합을 부를지가 이 GUID 로 정해진다.
 * 매크로로 한 번 감싸 둔 뒤 아래 dsm_guid 에 담는 이유는, GUID_INIT 이
 * 초기화자 전용이라 그대로는 함수 인자로 쓸 수 없기 때문이다 */
#define DSM_GUID GUID_INIT(0x5d524d9d, 0xfff9, 0x4d4b, 0x8c, 0xb7, 0x74, 0x7e,\
			   0xd5, 0x1e, 0x19, 0x4d)
/* [한국어] 지원 상태 목록 질의. pci_npem_create() 가 이것으로 capability 값에
 * 해당하는 비트맵을 얻는다 */
#define GET_SUPPORTED_STATES_DSM	1
/* [한국어] 현재 상태 질의. dsm_get_active_indications() 가 쓴다 */
#define GET_STATE_DSM			2
/* [한국어] 상태 설정. dsm_set_active_indications() 가 쓴다.
 * 세 값이 1,2,3 인 것은 스펙이 정한 함수 번호이며,
 * npem_has_dsm() 이 BIT() 으로 묶어 "셋 다 있는가" 를 한 번에 묻는다 */
#define SET_STATE_DSM			3

/* [한국어] 위 매크로를 풀어 담은 GUID 상수.
 * 읽는 자: npem_has_dsm() 의 acpi_check_dsm() 과
 *   dsm_evaluate() 의 acpi_evaluate_dsm_typed().
 * 동기화: const 정적 데이터 */
static const guid_t dsm_guid = DSM_GUID;

/* [한국어]
 * npem_has_dsm - 이 장치에 PCIe SSD LED 용 _DSM 이 갖춰져 있는가
 *
 * @pdev: 검사할 PCI 장치.  @return: true 면 _DSM 경로를 쓸 수 있다
 *
 * 두 단계로 확인한다.
 *   1) 이 PCI 장치에 대응하는 ACPI 핸들이 있는가. 없으면 펌웨어가 이 장치를
 *      모르는 것이므로 _DSM 도 없다.
 *   2) acpi_check_dsm() 으로 필요한 세 함수가 모두 구현되어 있는지 묻는다 —
 *      GET_SUPPORTED_STATES_DSM(1), GET_STATE_DSM(2), SET_STATE_DSM(3).
 *      BIT() 으로 만든 마스크를 넘기므로 셋 중 하나라도 없으면 false 다.
 *      세 번째 인자 0x1 은 요구하는 리비전이다.
 *
 * 셋을 한꺼번에 요구하는 이유는 부분 지원이 쓸모없기 때문이다. 읽기만
 * 되고 쓰기가 안 되면 LED 를 제어할 수 없고, 지원 목록을 못 얻으면 어떤
 * LED 를 만들지 정할 수 없다.
 *
 * acpi_check_dsm() 과 ACPI_HANDLE() 의 구현은 drivers/acpi 에 있는데
 * 이 스파스 체크아웃에는 그 디렉터리가 없어, 호출 이후의 동작은 코드로
 * 추적하지 못했다.
 *
 * 실행 컨텍스트: pci_npem_create() 안, 프로세스 컨텍스트. ACPI 평가가
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_npem_create() → [이 함수] → ACPI_HANDLE() → acpi_check_dsm()
 */
static bool npem_has_dsm(struct pci_dev *pdev)
{
	acpi_handle handle;

	/* [한국어] 이 PCI 장치에 대응하는 ACPI 핸들을 얻는다 */
	handle = ACPI_HANDLE(&pdev->dev);
	/* [한국어] 펌웨어가 이 장치를 모르면 _DSM 도 있을 수 없다 */
	if (!handle)
		return false;

	/* [한국어] 필요한 세 함수가 모두 구현되어 있는지 한 번에 묻는다.
	 * 0x1 은 요구하는 _DSM 리비전이다 */
	return acpi_check_dsm(handle, &dsm_guid, 0x1,
			      BIT(GET_SUPPORTED_STATES_DSM) |
			      /* [한국어] 셋을 BIT() 으로 묶어 넘기므로 하나라도 없으면 false 다.
			       * 부분 지원이 쓸모없기 때문이다 — 읽기만 되면 제어할 수 없고,
			       * 지원 목록을 못 얻으면 어떤 LED 를 만들지 정할 수 없다 */
			      BIT(GET_STATE_DSM) | BIT(SET_STATE_DSM));
}

struct dsm_output {
	/* [한국어] _DSM 응답의 상태 코드.
	 * 설정자: dsm_evaluate() 의 memcpy(펌웨어가 채운 값).
	 * 읽는 자: dsm_get() 은 0 만 성공으로 보고, dsm_set_active_indications() 는
	 *   0 과 4(부분 성공)를 성공으로 본다.
	 * 값 범위: 4 의 의미는 PCI Firmware Spec r3.3 Table 4-19 에 있다 —
	 *   플랫폼이 요청한 변경의 일부 또는 전부를 무시했다는 뜻이다.
	 * 동기화: 스택 지역 구조체라 공유되지 않는다 */
	u16 status;
	/* [한국어] 기능별 오류 코드.
	 * 읽는 자: dsm_set_active_indications() 가 status 4 일 때만 본다.
	 *   1 이 아니면 부분 성공으로도 인정하지 않고 -EIO 로 거절한다.
	 * 값 범위: 이 트리의 코드는 1 만 특별히 다룬다 */
	u8 function_specific_err;
	/* [한국어] 벤더별 오류 코드. 이 파일의 어느 코드도 이 필드를 읽지 않는다 —
	 * 응답 버퍼의 배치를 맞추기 위해 존재한다. 그 배치가 맞아야
	 * dsm_evaluate() 의 memcpy 와 아래 state 의 오프셋이 옳다 */
	u8 vendor_specific_err;
	/* [한국어] 현재(또는 설정 후) 표시 비트맵.
	 * 설정자: 펌웨어.
	 * 읽는 자: dsm_get() 이 그대로 꺼내 주고,
	 *   dsm_set_active_indications() 가 캐시 갱신에 쓴다 — 요청한 값이 아니라
	 *   이 값을 캐시하는 것이 요점이다. 플랫폼이 일부를 무시했을 수 있으므로
	 *   "실제로 반영된 값" 을 기억해야 다음 조회가 사실대로 답한다 */
	u32 state;
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
/* [한국어]
 * dsm_evaluate - _DSM PCIe SSD Status LED 명령을 평가한다
 *
 * @pdev: PCI 장치.  @dsm_func: 부를 _DSM 함수 번호(1/2/3)
 * @output: 응답을 복사해 담을 버퍼
 * @value_to_set: SET_STATE_DSM 일 때만 쓰이는 새 상태 값
 * @return: 0 = 성공, -EIO = 평가 실패 또는 응답이 너무 짧음
 *
 * 위 원문 kernel-doc 이 설계 의도를 밝힌다 — 호출자가 ACPI 문맥(union
 * acpi_object, ACPI_FREE 등)을 신경 쓰지 않아도 되도록 응답 버퍼를
 * 평범한 구조체로 복사해 준다.
 *
 * 인자 조립이 이 함수의 핵심이다. SET 일 때만 네 번째 인자(arg3)를 만든다.
 * 그 형식이 스펙에 정해져 있다 — 패키지 하나 안에 버퍼 하나가 들어가고,
 * 그 버퍼가 4바이트짜리 새 상태 값이다. arg3[0] 이 패키지, arg3[1] 이
 * 그 원소인 버퍼이고, package.elements 가 arg3[1] 을 가리키게 해서
 * 배열 두 칸으로 중첩 구조를 표현한다.
 * GET 계열에서는 arg3_p 가 NULL 로 남아 인자 없이 평가된다.
 *
 * acpi_evaluate_dsm_typed(..., ACPI_TYPE_BUFFER) 는 응답이 버퍼 타입일
 * 때만 돌려주므로, 타입 검사를 따로 할 필요가 없다.
 *
 * 길이 검사가 중요하다. 응답이 struct dsm_output 보다 짧으면 memcpy 가
 * 버퍼 밖을 읽게 되므로 -EIO 로 거절한다. 펌웨어를 믿지 않는 방어다.
 *
 * 두 실패 경로 모두 ACPI_FREE 를 잊지 않는다 — acpi_evaluate_dsm_typed 가
 * 돌려준 객체의 해제는 호출자 책임이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 평가가 잠들 수 있다.
 *
 * 호출 체인:
 *   dsm_get() / dsm_set_active_indications() → [이 함수]
 *     → acpi_evaluate_dsm_typed() → memcpy() → ACPI_FREE()
 */
static int dsm_evaluate(struct pci_dev *pdev, u64 dsm_func,
			struct dsm_output *output, u32 value_to_set)
{
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	/* [한국어] out_obj = 평가 결과, arg3 = SET 일 때 넘길 인자 두 칸
	 * (패키지 하나와 그 안의 버퍼 하나) */
	union acpi_object *out_obj, arg3[2];
	/* [한국어] 실제로 넘길 인자 포인터. GET 계열에서는 NULL 로 남아 인자 없이 평가된다 */
	union acpi_object *arg3_p = NULL;

	/* [한국어] SET 일 때만 인자를 조립한다 */
	if (dsm_func == SET_STATE_DSM) {
		/* [한국어] 바깥은 패키지다. 스펙이 정한 형식이다 */
		arg3[0].type = ACPI_TYPE_PACKAGE;
		/* [한국어] 그 패키지에 원소가 하나 들어간다 */
		arg3[0].package.count = 1;
		/* [한국어] 그 원소가 바로 다음 칸(arg3[1])이다. 배열 두 칸으로 중첩 구조를 표현한다 */
		arg3[0].package.elements = &arg3[1];

		/* [한국어] 안쪽 원소는 버퍼 타입이다 */
		arg3[1].type = ACPI_TYPE_BUFFER;
		/* [한국어] 길이는 4바이트 — 표시 비트맵이 u32 이기 때문이다 */
		arg3[1].buffer.length = 4;
		/* [한국어] 호출자가 넘긴 값을 그대로 가리킨다. 복사하지 않으므로 이 함수가
		 * 평가를 마칠 때까지 value_to_set 이 살아 있어야 하는데, 스택 인자라
		 * 그 조건이 자연히 만족된다 */
		arg3[1].buffer.pointer = (u8 *)&value_to_set;

		/* [한국어] 조립이 끝났으니 이제 넘길 준비가 되었다 */
		arg3_p = arg3;
	}

	/* [한국어] _DSM 을 평가한다. _typed 판이라 응답이 버퍼 타입일 때만 돌려주므로
	 * 타입 검사를 따로 할 필요가 없다 */
	out_obj = acpi_evaluate_dsm_typed(handle, &dsm_guid, 0x1, dsm_func,
					  arg3_p, ACPI_TYPE_BUFFER);
	/* [한국어] 평가 실패. 메서드가 없거나 펌웨어가 오류를 냈다 */
	if (!out_obj)
		return -EIO;

	/* [한국어] 응답이 우리가 읽으려는 구조체보다 짧으면 memcpy 가 버퍼 밖을 읽는다.
	 * 펌웨어를 믿지 않는 방어이며, 여기서도 ACPI_FREE 를 잊지 않는다 */
	if (out_obj->buffer.length < sizeof(struct dsm_output)) {
		ACPI_FREE(out_obj);
		return -EIO;
	}

	/* [한국어] ACPI 객체를 곧 해제할 것이므로 필요한 부분을 먼저 복사해 둔다.
	 * 이 복사가 위 원문 kernel-doc 이 말한 "호출자가 ACPI 문맥을 신경 쓰지
	 * 않아도 되게" 하는 장치다 */
	memcpy(output, out_obj->buffer.pointer, sizeof(struct dsm_output));

	ACPI_FREE(out_obj);
	return 0;
}

/* [한국어]
 * dsm_get - _DSM 을 평가해 상태 값만 꺼내 준다
 *
 * @pdev: PCI 장치.  @dsm_func: 부를 _DSM 함수 번호
 * @buf: 상태 값을 담을 곳
 * @return: 0 = 성공, -EIO = 평가 실패 또는 status 가 0 이 아님
 *
 * dsm_evaluate() 위에 얹은 얇은 래퍼다. 차이는 하나 — 응답의 status
 * 필드를 확인해 0 이 아니면 실패로 처리한다. GET 계열에서는 부분 성공이
 * 의미가 없으므로 0 만 성공으로 본다(SET 쪽은 status 4 를 특별히 다루는데,
 * 그 처리는 dsm_set_active_indications() 에 있다).
 *
 * 두 곳에서 쓰인다.
 *   - pci_npem_create() 가 GET_SUPPORTED_STATES_DSM 으로 "이 장치가 어떤
 *     표시를 지원하는가" 를 묻는다. 그 결과가 곧 capability 값 역할을 한다.
 *   - dsm_get_active_indications() 가 GET_STATE_DSM 으로 현재 상태를 묻는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 평가가 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_npem_create() / dsm_get_active_indications() → [이 함수] → dsm_evaluate()
 */
static int dsm_get(struct pci_dev *pdev, u64 dsm_func, u32 *buf)
{
	struct dsm_output output;
	/* [한국어] GET 계열이므로 value_to_set 은 0 을 넘긴다 — 쓰이지 않는 자리다 */
	int ret = dsm_evaluate(pdev, dsm_func, &output, 0);

	/* [한국어] 평가 자체가 실패했다 */
	if (ret)
		return ret;

	/* [한국어] GET 에서는 부분 성공이 의미가 없으므로 0 만 성공으로 본다.
	 * SET 쪽이 status 4 를 따로 다루는 것과 대비된다 */
	if (output.status != 0)
		return -EIO;

	*buf = output.state;
	return 0;
}

/* [한국어]
 * dsm_get_active_indications - _DSM 으로 현재 켜진 표시를 읽는다
 *
 * @npem: 이 장치의 NPEM 상태.  @buf: 읽은 비트 마스크를 담을 곳
 * @return: dsm_get() 의 반환값을 그대로 전달
 *
 * dsm_ops 의 get 콜백이다. NPEM 쪽 짝(npem_get_active_indications)과 달리
 * ENABLE 비트 같은 개념이 없어 훨씬 단순하다.
 *
 * 코드 주석대로 응답을 supported_indications 로 걸러낸다. 펌웨어가 우리가
 * 모르는 비트를 세워 돌려줄 수 있고, 그것을 그대로 캐시하면 LED 가 없는
 * 표시가 켜진 것으로 잡히기 때문이다.
 *
 * 주의할 점: ret 이 0 이 아닌(실패한) 경우에도 *buf 를 걸러 쓴 뒤 반환한다.
 * 그때 buf 는 dsm_get() 이 채우지 않은 값이라 의미가 없지만, 호출자가
 * 반환값을 먼저 확인하므로 실제 문제로 이어지지는 않는다. 코드는 고치지
 * 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 npem->lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   npem_initialize_active_indications() → (ops 콜백) → [이 함수] → dsm_get()
 */
static int dsm_get_active_indications(struct npem *npem, u32 *buf)
{
	int ret = dsm_get(npem->dev, GET_STATE_DSM, buf);

	/* Filter out not supported indications in response */
	*buf &= npem->supported_indications;
	return ret;
}

/* [한국어]
 * dsm_set_active_indications - _DSM 으로 새 표시를 설정한다
 *
 * @npem: 이 장치의 NPEM 상태.  @value: 켤 표시들의 비트 마스크
 * @return: 0 = 성공(부분 성공 포함), -EIO = 실패
 *
 * dsm_ops 의 set 콜백이다. 평가 자체보다 응답 status 해석이 이 함수의
 * 핵심이다.
 *
 *   status 0 — 완전 성공.
 *   status 4 — 코드 안의 원문 주석이 PCI Firmware Spec r3.3 Table 4-19 를
 *      인용해 설명한다. 플랫폼이 요청한 상태 변경의 일부 또는 전부를
 *      무시했다는 뜻이고, OSPM 이 결과 상태를 다시 확인해야 한다.
 *      다만 그 안에서도 function_specific_err 가 1 일 때만 받아들이고
 *      그 밖에는 -EIO 로 거절한다.
 *   그 밖   — -EIO.
 *
 * status 4 갈래에서 fallthrough 로 0 갈래에 합류하는 구조라, 부분 성공도
 * "성공" 으로 취급하고 아래 캐시 갱신까지 진행한다.
 *
 * 마지막 한 줄이 그 처리다 — 요청한 value 가 아니라 응답이 돌려준
 * output.state 를 캐시한다. 플랫폼이 일부를 무시했을 수 있으므로,
 * "우리가 원한 값" 이 아니라 "실제로 반영된 값" 을 기억해야 다음
 * brightness_get() 이 사실대로 답한다. NPEM 쪽이 쓴 뒤 다시 읽는 것과
 * 같은 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 npem->lock 을 쥐고 있다.
 *
 * 호출 체인:
 *   brightness_set() → (ops 콜백) → [이 함수] → dsm_evaluate()
 */
static int dsm_set_active_indications(struct npem *npem, u32 value)
{
	struct dsm_output output;
	/* [한국어] SET 함수를 평가한다. 새 값이 인자로 들어간다 */
	int ret = dsm_evaluate(npem->dev, SET_STATE_DSM, &output, value);

	/* [한국어] 평가 자체가 실패했다 */
	if (ret)
		return ret;

	/* [한국어] 응답 상태에 따라 갈린다 */
	switch (output.status) {
	case 4:
		/*
		 * Not all bits are set. If this bit is set, the platform
		 * disregarded some or all of the request state changes. OSPM
		 * should check the resulting PCIe SSD Status LED States to see
		 * what, if anything, has changed.
		 *
		 * PCI Firmware Specification, r3.3 Table 4-19.
		 */
		if (output.function_specific_err != 1)
			return -EIO;
		/* [한국어] 부분 성공을 성공 갈래에 합류시킨다. 아래 캐시 갱신까지 진행하게 된다 */
		fallthrough;
	/* [한국어] 완전 성공 */
	case 0:
		break;
	default:
		return -EIO;
	}

	/* [한국어] 요청한 value 가 아니라 응답이 돌려준 state 를 캐시한다.
	 * 플랫폼이 일부를 무시했을 수 있으므로 "실제로 반영된 값" 을 기억해야
	 * 다음 brightness_get() 이 사실대로 답한다 */
	npem->active_indications = output.state;

	return 0;
}

/* [한국어] _DSM(ACPI) 백엔드의 콜백 표. npem_ops 와 같은 인터페이스를 구현하므로
 * 위쪽 코드는 어느 쪽이 쓰이는지 몰라도 된다 */
static const struct npem_ops dsm_ops = {
	/* [한국어] _DSM 으로 현재 표시를 알아내는 구현 */
	.get_active_indications = dsm_get_active_indications,
	.set_active_indications = dsm_set_active_indications,
	.name = "_DSM PCIe SSD Status LED Management",
	.inds = dsm_indications,
};

/* [한국어]
 * npem_initialize_active_indications - 활성 표시 캐시를 처음 한 번만 채운다
 *
 * @npem: 이 장치의 NPEM 상태.
 * @return: 0 = 캐시가 준비됨(이미 되어 있었거나 방금 채웠다), 음수 errno = 읽기 실패
 *
 * 지연 초기화(lazy initialization)다. 그 이유를 struct npem 의
 * active_inds_initialized 필드에 붙은 원문 kernel-doc 이 자세히 밝힌다 —
 * Dell 플랫폼에서는 GET_STATE_DSM 을 부르기 전에 IPMI 드라이버가 먼저
 * 로드되어 있어야 한다. 그쪽이 IPMI OpRegion 으로 LED 를 읽고 쓰기
 * 때문이다. 장치 열거 시점에 캐시를 채우면 IPMI 가 아직 없어 실패하지만,
 * "LED 에 처음 접근할 때" 로 미루면 그 사이에 IPMI 가 로드될 기회가 생긴다.
 * 로드가 늦으면 dmesg 에 오류가 보이다가, 로드된 뒤에는 사라지고 정상
 * 동작한다.
 *
 * 두 콜백(brightness_get / brightness_set) 모두 락을 잡은 직후 이것을
 * 부른다. 그래서 어느 쪽이 먼저 불리든 캐시가 준비된다.
 *
 * lockdep_assert_held 로 락 보유를 못 박는다. 검사와 설정 사이에 다른
 * LED 가 끼어들면 초기화가 두 번 일어난다.
 *
 * 실패하면 active_inds_initialized 를 세우지 않으므로 다음 접근에서 다시
 * 시도한다 — 그것이 위 IPMI 시나리오가 저절로 회복되는 이유다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ops 콜백이 잠들 수 있다.
 *
 * 호출 체인:
 *   brightness_get() / brightness_set() → [이 함수]
 *     → npem_get_active_indications() 또는 dsm_get_active_indications()
 */
static int npem_initialize_active_indications(struct npem *npem)
{
	int ret;

	lockdep_assert_held(&npem->lock);

	/* [한국어] 이미 채워져 있으면 다시 읽지 않는다 */
	if (npem->active_inds_initialized)
		return 0;

	/* [한국어] 백엔드에 현재 상태를 묻는다. 여기가 IPMI 가 필요한 그 호출이며,
	 * 지연 초기화의 이유이기도 하다 */
	ret = npem->ops->get_active_indications(npem,
						&npem->active_indications);
	/* [한국어] 실패하면 아래 플래그를 세우지 않는다 — 그래야 다음 접근에서 다시
	 * 시도하고, IPMI 가 뒤늦게 로드된 경우 저절로 회복된다 */
	if (ret)
		return ret;

	/* [한국어] 이제부터는 캐시를 믿는다 */
	npem->active_inds_initialized = true;
	return 0;
}

/*
 * The status of each indicator is cached on first brightness_ get/set time
 * and updated at write time.  brightness_get() is only responsible for
 * reflecting the last written/cached value.
 */
/* [한국어]
 * brightness_get - LED 하나가 켜져 있는지 돌려준다
 *
 * @led: LED 코어가 넘겨준 led_classdev
 * @return: 0 = 꺼짐, 1 = 켜짐, 음수 = 락 획득이 인터럽트로 중단됨
 *
 * /sys/class/leds/<이름>/brightness 를 읽으면 LED 코어를 거쳐 불린다.
 *
 * 바로 위 원문 주석이 정책을 밝힌다 — 각 표시의 상태는 처음 get/set 할 때
 * 캐시되고 write 시점에 갱신되며, 이 함수는 마지막으로 쓴(또는 캐시된)
 * 값을 반영할 뿐이다. 즉 매번 하드웨어를 다시 읽지 않는다. 여러 LED 가
 * 같은 레지스터를 공유하므로 LED 마다 읽으면 config 접근이 배로 늘고,
 * 표시 상태는 커널 밖에서 바뀌는 일이 드물기 때문이다.
 *
 * container_of 로 led_classdev 에서 struct npem_led 를 되찾는 것이 첫
 * 동작이다. LED 코어는 led_classdev 만 알고 그 바깥은 모르므로,
 * 그것을 멤버로 품은 구조체로 되짚어 올라간다.
 *
 * mutex_lock_interruptible 을 쓰는 이유는 sysfs 읽기가 사용자 요청이라
 * Ctrl-C 로 끊을 수 있어야 하기 때문이다. 다만 그 실패값(-EINTR 등)이
 * enum led_brightness 로 반환되는데, 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: sysfs 읽기의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (sysfs read) → LED 코어 → [이 함수]
 *     → npem_initialize_active_indications()
 */
static enum led_brightness brightness_get(struct led_classdev *led)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	/* [한국어] LED 항목에서 장치 상태로 되돌아간다 */
	struct npem *npem = nled->npem;
	/* [한국어] val 은 반환할 밝기. 실패 경로에서도 0(꺼짐)으로 답하도록 미리 초기화한다 */
	int ret, val = 0;

	/* [한국어] sysfs 읽기는 사용자 요청이라 Ctrl-C 로 끊을 수 있어야 한다 */
	ret = mutex_lock_interruptible(&npem->lock);
	/* [한국어] 락을 못 잡았다. 다만 그 값(-EINTR 등)이 enum led_brightness 로
	 * 반환되는데, 코드는 고치지 않고 이 관찰만 적어 둔다 */
	if (ret)
		return ret;

	/* [한국어] 캐시가 아직 없으면 여기서 채운다 */
	ret = npem_initialize_active_indications(npem);
	/* [한국어] 실패하면 val 이 0 인 채로 out 으로 빠져 "꺼짐" 으로 답한다 */
	if (ret)
		goto out;

	/* [한국어] 캐시된 전체 마스크에서 이 LED 의 비트만 본다. 하드웨어를 다시 읽지
	 * 않는 것이 이 함수의 정책이다 */
	if (npem->active_indications & nled->indication->bit)
		/* [한국어] 그 비트가 서 있으면 켜짐 */
		val = 1;

out:
	mutex_unlock(&npem->lock);
	return val;
}

/* [한국어]
 * brightness_set - LED 하나를 켜거나 끈다
 *
 * @led: LED 코어가 넘겨준 led_classdev
 * @brightness: 0 이면 끄기, 그 밖이면 켜기
 * @return: 0 = 성공, 음수 errno = 실패
 *
 * /sys/class/leds/<이름>/brightness 에 쓰면 LED 코어를 거쳐 불린다.
 * brightness_set_blocking 으로 등록되어 있어 잠들어도 된다 —
 * _DSM 평가와 Command Completed 대기가 모두 잠들기 때문이다.
 *
 * 동작은 "캐시된 전체 마스크에서 이 LED 의 비트만 바꿔 통째로 다시 쓰기" 다.
 *   끄기: active_indications & ~(이 비트)
 *   켜기: active_indications | (이 비트)
 * NPEM 도 _DSM 도 "표시 하나만 바꾸기" 를 지원하지 않고 전체 마스크를
 * 한 번에 받으므로, 나머지 LED 의 현재 상태를 정확히 알고 있어야 한다.
 * 그것이 캐시(active_indications)가 필요한 근본 이유이고, 캐시를 먼저
 * 초기화하는 이유이기도 하다.
 *
 * max_brightness 가 1 로 등록되어 있어 LED 코어가 0 또는 1 만 넘긴다.
 * 그래서 0 인지 아닌지만 보면 된다.
 *
 * 캐시 갱신은 이 함수가 하지 않는다. ops 의 set 콜백이 하드웨어의 실제
 * 결과를 반영해 갱신한다 — NPEM 은 쓴 뒤 다시 읽고, _DSM 은 응답이 돌려준
 * 상태를 쓴다.
 *
 * 실행 컨텍스트: sysfs 쓰기의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   (sysfs write) → LED 코어 → [이 함수]
 *     → npem_initialize_active_indications()
 *     → npem_set_active_indications() 또는 dsm_set_active_indications()
 */
static int brightness_set(struct led_classdev *led,
			  enum led_brightness brightness)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	/* [한국어] LED 항목에서 장치 상태로 되돌아간다 */
	struct npem *npem = nled->npem;
	/* [한국어] 새로 쓸 전체 마스크 */
	u32 indications;
	/* [한국어] 반환값 */
	int ret;

	/* [한국어] sysfs 쓰기도 끊을 수 있어야 한다 */
	ret = mutex_lock_interruptible(&npem->lock);
	/* [한국어] 락을 못 잡았으면 그 오류를 그대로 올린다 */
	if (ret)
		return ret;

	/* [한국어] 나머지 LED 의 현재 상태를 알아야 전체 마스크를 만들 수 있다 */
	ret = npem_initialize_active_indications(npem);
	/* [한국어] 캐시를 못 채우면 쓸 수 없다 */
	if (ret)
		goto out;

	/* [한국어] 끄기 요청 */
	if (brightness == 0)
		/* [한국어] 캐시된 마스크에서 이 비트만 지운다 */
		indications = npem->active_indications & ~(nled->indication->bit);
	else
		/* [한국어] 켜기 요청 — 이 비트만 더한다. 두 백엔드 모두 전체 마스크를 받으므로
		 * 나머지 비트를 그대로 실어 보내야 다른 LED 가 꺼지지 않는다 */
		indications = npem->active_indications | nled->indication->bit;

	/* [한국어] 백엔드에 내려보낸다. 캐시 갱신은 그 구현이 하드웨어의 실제 결과를
	 * 반영해 처리한다 */
	ret = npem->ops->set_active_indications(npem, indications);

out:
	mutex_unlock(&npem->lock);
	return ret;
}

/* [한국어]
 * npem_free - LED 들을 해제하고 struct npem 을 놓는다
 *
 * @npem: 해제할 NPEM 상태. NULL 이어도 안전하다.  @return: 없음
 *
 * 두 곳에서 불린다 — pci_npem_remove()(정상 제거)와 pci_npem_init() 의
 * 실패 경로(LED 등록 도중 실패). 후자 때문에 "일부만 등록된 상태" 도
 * 다룰 수 있어야 한다.
 *
 * 그 판정을 nled->name[0] 으로 한다. pci_npem_set_led_classdev() 가
 * 등록에 실패하면 이름의 첫 글자를 0 으로 지워 두므로(그 함수의 원문
 * 주석이 그 의도를 밝힌다), 여기서 그 표시를 보고 등록된 것만 해제한다.
 * kzalloc 으로 잡았으니 아직 손대지 않은 항목도 name[0] 이 0 이다.
 *
 * 맨 앞의 NULL 검사가 pci_npem_remove() 를 단순하게 만든다 —
 * dev->npem 이 NULL 인 장치(대부분)에서도 그냥 부르면 된다.
 *
 * mutex_destroy 는 디버그 커널에서 뮤텍스의 사용 흔적을 검증하는 용도다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_npem_remove() / pci_npem_init()의 실패 경로 → [이 함수]
 *     → led_classdev_unregister() → kfree()
 */
static void npem_free(struct npem *npem)
{
	struct npem_led *nled;
	/* [한국어] 해제 루프의 인덱스 */
	int cnt;

	/* [한국어] NULL 을 안전하게 다루므로 호출부에 조건문이 필요 없다 */
	if (!npem)
		return;

	/* [한국어] 만든 LED 개수만큼 훑는다 */
	for (cnt = 0; cnt < npem->led_cnt; cnt++) {
		/* [한국어] 이번 LED 항목 */
		nled = &npem->leds[cnt];

		/* [한국어] 이름의 첫 글자가 남아 있으면 등록에 성공한 LED 다.
		 * 등록 실패 시 pci_npem_set_led_classdev() 가 지워 두고,
		 * kzalloc 으로 잡았으니 손대지 않은 항목도 0 이다 */
		if (nled->name[0])
			led_classdev_unregister(&nled->led);
	}

	mutex_destroy(&npem->lock);
	kfree(npem);
}

/* [한국어]
 * pci_npem_set_led_classdev - LED 하나를 구성해 LED 서브시스템에 등록한다
 *
 * @npem: 이 장치의 NPEM 상태.  @nled: 채워 넣을 LED 항목
 * @return: 0 = 성공, 음수 errno = 이름 조립 또는 등록 실패
 *
 * LED 이름은 커널이 정한 규칙("장치이름:색:기능")을 따라야 한다.
 * led_compose_name() 이 그 조립을 대신해 주며, 재료는 둘이다 —
 * devicename 으로 pci_name()(BDF 문자열), default_label 로 표시 이름
 * ("enclosure:fail" 등).
 *
 * 콜백과 속성 설정에 각각 이유가 있다.
 *   brightness_set_blocking — 잠들 수 있는 set 콜백임을 알린다. 일반
 *     brightness_set 으로 등록하면 아토믹 문맥에서 불릴 수 있어
 *     _DSM 평가나 폴링 대기가 불가능하다.
 *   max_brightness = 1 — 이 LED 는 켜짐/꺼짐뿐이다. 파일의 역할 절에
 *     적은 대로 "어떻게 깜빡일지" 는 하드웨어가 정하므로 밝기 단계가 없다.
 *   default_trigger = "none" — 커널 트리거(하트비트 등)를 붙이지 않는다.
 *     이 LED 의 의미는 인클로저 상태이지 시스템 활동이 아니다.
 *   LED_HW_PLUGGABLE — 하드웨어가 사라질 수 있는 LED 임을 알린다.
 *
 * 실패 시 name[0] = 0 으로 지우는 한 줄이 중요하다. 원문 주석이 그 의도를
 * 밝히듯 "등록되지 않았음" 을 나타내는 표시이고, npem_free() 가 그것을
 * 보고 해제 대상에서 제외한다.
 *
 * 실행 컨텍스트: probe 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_npem_init() → [이 함수] → led_compose_name() → led_classdev_register()
 */
static int pci_npem_set_led_classdev(struct npem *npem, struct npem_led *nled)
{
	struct led_classdev *led = &nled->led;
	/* [한국어] led_compose_name 에 넘길 재료. = {} 로 0 초기화한다 */
	struct led_init_data init_data = {};
	/* [한국어] 이름을 조립해 담을 버퍼. struct npem_led 안의 배열이라 LED 가 등록되어
	 * 있는 동안 살아 있다 */
	char *name = nled->name;
	/* [한국어] 반환값 */
	int ret;

	/* [한국어] 장치 이름 부분으로 BDF 문자열을 쓴다. 같은 섀시의 여러 장치가
	 * 구분되게 하는 부분이다 */
	init_data.devicename = pci_name(npem->dev);
	/* [한국어] 기능 부분으로 표시 이름을 쓴다("enclosure:fail" 등) */
	init_data.default_label = nled->indication->name;

	/* [한국어] 커널이 정한 LED 이름 규칙에 맞게 조립한다 */
	ret = led_compose_name(&npem->dev->dev, &init_data, name);
	/* [한국어] 조립 실패. 아직 등록 전이라 name[0] 을 지울 필요가 없다 */
	if (ret)
		return ret;

	/* [한국어] 조립된 이름을 LED 코어가 보게 한다. 버퍼가 아니라 포인터를 넘기므로
	 * 그 버퍼의 수명이 LED 보다 길어야 한다 */
	led->name = name;
	/* [한국어] 잠들 수 있는 set 콜백으로 등록한다. 일반 brightness_set 으로 등록하면
	 * 아토믹 문맥에서 불릴 수 있어 _DSM 평가나 폴링 대기가 불가능하다 */
	led->brightness_set_blocking = brightness_set;
	/* [한국어] get 은 캐시만 읽으므로 잠들지 않지만, 뮤텍스를 잡으므로 역시
	 * 프로세스 컨텍스트에서만 불려야 한다 */
	led->brightness_get = brightness_get;
	/* [한국어] 켜짐/꺼짐뿐이다. "어떻게 깜빡일지" 는 하드웨어가 정하므로 밝기 단계가 없다 */
	led->max_brightness = 1;
	/* [한국어] 커널 트리거(하트비트 등)를 붙이지 않는다. 이 LED 의 의미는 인클로저
	 * 상태이지 시스템 활동이 아니다 */
	led->default_trigger = "none";
	/* [한국어] 하드웨어가 사라질 수 있는 LED 임을 알린다 */
	led->flags = LED_HW_PLUGGABLE;

	/* [한국어] LED 서브시스템에 등록한다. 부모를 PCI 장치로 지정해 sysfs 계층에서
	 * 어느 장치의 LED 인지 드러나게 한다 */
	ret = led_classdev_register(&npem->dev->dev, led);
	if (ret)
		/* Clear the name to indicate that it is not registered. */
		name[0] = 0;
	return ret;
}

/* [한국어]
 * pci_npem_init - 지원되는 표시마다 LED 를 하나씩 만든다
 *
 * @dev: 대상 PCI 장치.  @ops: 고른 백엔드(npem_ops 또는 dsm_ops)
 * @pos: NPEM capability 오프셋(_DSM 경로면 0)
 * @caps: 지원 표시 비트맵(NPEM Capability 값 또는 _DSM 응답)
 * @return: 0 = 성공, -ENOMEM, 또는 LED 등록 실패값
 *
 * 절차:
 *   1) reg_to_indications() 로 "하드웨어가 지원하면서 드라이버도 아는"
 *      표시만 걸러내고, hweight32 로 그 개수를 센다. 그 수가 곧 만들 LED
 *      개수이자 유연 배열의 길이가 된다.
 *   2) kzalloc_flex 로 struct npem 과 leds[개수] 를 한 번에 잡는다.
 *      유연 배열 멤버라 별도 할당이 필요 없다.
 *   3) 필드를 채우고 뮤텍스를 초기화한다.
 *   4) 표시 목록을 훑으며 지원되는 것마다 LED 를 하나씩 등록한다.
 *      하나라도 실패하면 npem_free() 로 지금까지 만든 것을 되돌리고
 *      그 오류를 올린다.
 *   5) dev->npem 에 매달아 둔다. 이 대입이 마지막인 것이 중요하다 —
 *      완전히 준비된 뒤에만 외부에서 보이게 하려는 것이다.
 *
 * 4)의 루프가 ops->inds 가 아니라 npem_indications 를 순회하는 점에
 * 주의한다. _DSM 경로에서도 NPEM 목록을 돈다는 뜻인데, dsm_indications 가
 * npem_indications 의 앞부분과 같은 순서·같은 비트라서 결과는 같아진다
 * (_DSM 쪽에 없는 specific_0~7 은 supported 마스크에서 이미 걸러진다).
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 열거 경로의 프로세스 컨텍스트. 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_npem_create() → [이 함수]
 *     → reg_to_indications() → kzalloc_flex() → pci_npem_set_led_classdev()
 */
static int pci_npem_init(struct pci_dev *dev, const struct npem_ops *ops,
			 int pos, u32 caps)
{
	u32 supported = reg_to_indications(caps, ops->inds);
	/* [한국어] 지원 비트의 개수 = 만들 LED 개수 = 유연 배열의 길이 */
	int supported_cnt = hweight32(supported);
	/* [한국어] 표시 목록을 훑을 반복자 */
	const struct indication *indication;
	/* [한국어] 이번에 채울 LED 항목 */
	struct npem_led *nled;
	/* [한국어] 새로 잡을 장치 상태 */
	struct npem *npem;
	/* [한국어] leds[] 에 채워 넣을 다음 자리. 지원되지 않는 표시는 건너뛰므로
	 * 목록 인덱스와 달라진다 */
	int led_idx = 0;
	/* [한국어] 반환값 */
	int ret;

	/* [한국어] struct npem 과 leds[개수] 를 한 번에 잡는다. 유연 배열 멤버라
	 * 별도 할당이 없고, 그래서 오류 경로도 kfree 한 번으로 끝난다 */
	npem = kzalloc_flex(*npem, leds, supported_cnt);
	/* [한국어] 메모리 부족 */
	if (!npem)
		return -ENOMEM;

	/* [한국어] 걸러 낸 지원 마스크를 캐시한다 */
	npem->supported_indications = supported;
	/* [한국어] 해제 루프가 쓸 개수 */
	npem->led_cnt = supported_cnt;
	/* [한국어] NPEM capability 오프셋. _DSM 경로에서는 0 이 들어온다 */
	npem->pos = pos;
	/* [한국어] config 접근과 LED 등록의 대상 장치 */
	npem->dev = dev;
	/* [한국어] 고른 백엔드 */
	npem->ops = ops;

	mutex_init(&npem->lock);

	/* [한국어] 표시 목록을 훑으며 LED 를 만든다. ops->inds 가 아니라 npem_indications
	 * 를 도는데, dsm_indications 가 이 목록의 앞부분과 같은 순서·같은 비트라서
	 * 결과는 같아진다(_DSM 에 없는 specific_0~7 은 supported 마스크에서 이미
	 * 걸러졌다). 코드는 고치지 않고 이 관찰만 적어 둔다 */
	for_each_indication(indication, npem_indications) {
		/* [한국어] 이 장치가 지원하지 않는 표시는 건너뛴다 */
		if (!(npem->supported_indications & indication->bit))
			continue;

		/* [한국어] 다음 빈자리를 차지한다 */
		nled = &npem->leds[led_idx++];
		/* [한국어] 이 LED 가 담당할 표시 */
		nled->indication = indication;
		/* [한국어] 장치 상태로 되돌아갈 포인터 */
		nled->npem = npem;

		/* [한국어] 이름을 조립하고 LED 서브시스템에 등록한다 */
		ret = pci_npem_set_led_classdev(npem, nled);
		/* [한국어] 하나라도 실패하면 지금까지 만든 것을 되돌린다 */
		if (ret) {
			npem_free(npem);
			return ret;
		}
	}

	/* [한국어] 완전히 준비된 뒤에만 외부에서 보이게 한다. 이 대입이 마지막인 이유다 */
	dev->npem = npem;
	return 0;
}

/* [한국어]
 * pci_npem_remove - 장치 제거 시 NPEM LED 들을 해제한다
 *
 * @dev: 제거되는 PCI 장치.  @return: 없음
 *
 * 확인한 유일한 호출자는 drivers/pci/remove.c:122 의 pci_destroy_dev() 다.
 *
 * npem_free() 한 줄이 전부이고, 그 함수가 NULL 을 안전하게 다루므로
 * NPEM 이 없는 장치(대부분)에서도 그냥 부르면 된다. 그래서 호출부에
 * 조건문이 없다.
 *
 * dev->npem 을 NULL 로 되돌리지 않는다. 이 시점 이후 그 포인터를 볼
 * 코드 경로가 없고 struct pci_dev 자체가 곧 해제되기 때문으로 보이나,
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * CONFIG 가 꺼져 있으면 이 함수 대신 drivers/pci/pci.h:1739 의 빈 인라인
 * 스텁이 들어간다.
 *
 * 실행 컨텍스트: 장치 제거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_destroy_dev() [remove.c:122] → [이 함수] → npem_free()
 */
void pci_npem_remove(struct pci_dev *dev)
{
	npem_free(dev->npem);
}

/* [한국어]
 * pci_npem_create - 이 장치의 인클로저 LED 를 지원한다면 등록한다
 *
 * @dev: 갓 열거된 PCI 장치.  @return: 없음(실패해도 알릴 곳이 없다)
 *
 * 이 파일의 진입점이다. 확인한 유일한 호출자는 drivers/pci/probe.c:6704 의
 * pci_device_add() — 즉 커널이 인식하는 모든 PCI 장치가 한 번씩 지나간다.
 *
 * 백엔드 선택이 이 함수의 핵심이고, 순서에 근거가 있다. 코드 안의 원문
 * 주석이 PCI Firmware Spec r3.3 sec 4.7 을 인용해 밝히듯, _DSM 이 있으면
 * OS 는 그것을 써야 한다. 펌웨어가 LED 를 관리하고 있을 수 있고, 그때
 * 커널이 레지스터를 직접 건드리면 충돌하기 때문이다.
 *
 *   _DSM 경로: npem_has_dsm() 이 참이면 GET_SUPPORTED_STATES_DSM 으로
 *      지원 표시 목록을 얻는다. 그것이 곧 caps 역할을 한다. pos 는 0 으로
 *      남으며, 이후 레지스터 접근이 전혀 없으므로 문제가 되지 않는다.
 *   NPEM 경로: 아니면 NPEM 확장 capability 를 찾고, Capability 레지스터를
 *      읽어 PCI_NPEM_CAP_CAPABLE 비트가 서 있는지 확인한다. capability
 *      구조는 있는데 실제 기능이 없는 장치를 걸러내는 검사다.
 *
 * 어느 경로든 실패하면 조용히 돌아선다 — 대부분의 PCI 장치에는 LED 가
 * 없으므로 그것이 정상이고, 로그를 남기면 부팅 때마다 쏟아진다.
 * 반면 지원이 확인된 뒤의 실패(pci_npem_init)는 pci_err 로 남긴다.
 * "할 수 있어야 하는데 못 한" 경우라 사용자가 알아야 하기 때문이다.
 *
 * 반환형이 void 인 이유: LED 등록 실패가 장치 사용을 막을 이유가 없다.
 * 호출자도 반환값을 기대하지 않는다.
 *
 * 실행 컨텍스트: 열거 경로의 프로세스 컨텍스트. ACPI 평가로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_device_add() [probe.c:6704] → [이 함수]
 *     → npem_has_dsm() → dsm_get() 또는 pci_find_ext_capability()
 *     → pci_npem_init()
 */
void pci_npem_create(struct pci_dev *dev)
{
	const struct npem_ops *ops = &npem_ops;
	/* [한국어] pos 는 NPEM capability 오프셋. _DSM 경로에서는 0 인 채로 남는다 */
	int pos = 0, ret;
	/* [한국어] 지원 표시 비트맵. 어느 경로냐에 따라 레지스터 값이거나 _DSM 응답이다 */
	u32 cap;

	if (npem_has_dsm(dev)) {
		/*
		 * OS should use the DSM for LED control if it is available
		 * PCI Firmware Spec r3.3 sec 4.7.
		 */
		ret = dsm_get(dev, GET_SUPPORTED_STATES_DSM, &cap);
		/* [한국어] _DSM 은 있는데 지원 목록을 못 얻었다. 더 진행할 수 없다 */
		if (ret)
			return;

		/* [한국어] _DSM 백엔드로 확정한다. pos 는 0 인 채로 남지만, 이 경로는
		 * 레지스터를 전혀 건드리지 않으므로 문제가 되지 않는다 */
		ops = &dsm_ops;
	} else {
		/* [한국어] _DSM 이 없으면 NPEM 확장 capability 를 찾는다 */
		pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_NPEM);
		/* [한국어] 그것도 없으면 이 장치에는 인클로저 LED 가 없다. 조용히 돌아선다 —
		 * 대부분의 PCI 장치가 여기 해당하므로 로그를 남기면 부팅 때마다 쏟아진다 */
		if (pos == 0)
			return;

		/* [한국어] Capability 레지스터를 읽고 CAPABLE 비트를 확인한다.
		 * capability 구조는 있는데 실제 기능이 없는 장치를 걸러내는 검사이며,
		 * 읽기 실패도 같은 갈래로 처리한다 */
		if (pci_read_config_dword(dev, pos + PCI_NPEM_CAP, &cap) != 0 ||
		    (cap & PCI_NPEM_CAP_CAPABLE) == 0)
			return;
	}

	/* [한국어] 여기까지 왔으면 LED 를 만들 수 있다. 어느 백엔드를 골랐는지 남긴다 —
	 * dmesg 만 보고 _DSM 경로인지 네이티브 경로인지 알 수 있게 한다 */
	pci_info(dev, "Configuring %s\n", ops->name);

	/* [한국어] 실제 LED 생성 */
	ret = pci_npem_init(dev, ops, pos, cap);
	/* [한국어] 지원이 확인된 뒤의 실패는 로그로 남긴다. "할 수 있어야 하는데 못 한"
	 * 경우라 사용자가 알아야 하기 때문이다 */
	if (ret)
		/* [한국어] 어느 백엔드에서 무슨 오류가 났는지 함께 찍는다 */
		pci_err(dev, "Failed to register %s, err: %d\n", ops->name,
			ret);
}
