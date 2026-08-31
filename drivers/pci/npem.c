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

#include <linux/acpi.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/leds.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/types.h>
#include <linux/uleds.h>

#include "pci.h"

struct indication {
	u32 bit;
	const char *name;
};

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
static u32 reg_to_indications(u32 caps, const struct indication *inds)
{
	const struct indication *ind;
	u32 supported_indications = 0;

	for_each_indication(ind, inds)
		supported_indications |= ind->bit;

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
	const struct indication *indication;
	struct npem *npem;
	char name[LED_MAX_NAME_SIZE];
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
	int (*get_active_indications)(struct npem *npem, u32 *inds);
	int (*set_active_indications)(struct npem *npem, u32 inds);
	const struct indication *inds;
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
	struct pci_dev *dev;
	const struct npem_ops *ops;
	struct mutex lock;
	u16 pos;
	u32 supported_indications;
	u32 active_indications;
	unsigned int active_inds_initialized:1;
	int led_cnt;
	struct npem_led leds[];
};

static int npem_read_reg(struct npem *npem, u16 reg, u32 *val)
{
	int ret = pci_read_config_dword(npem->dev, npem->pos + reg, val);

	return pcibios_err_to_errno(ret);
}

static int npem_write_ctrl(struct npem *npem, u32 reg)
{
	int pos = npem->pos + PCI_NPEM_CTRL;
	int ret = pci_write_config_dword(npem->dev, pos, reg);

	return pcibios_err_to_errno(ret);
}

static int npem_get_active_indications(struct npem *npem, u32 *inds)
{
	u32 ctrl;
	int ret;

	ret = npem_read_reg(npem, PCI_NPEM_CTRL, &ctrl);
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

static int npem_set_active_indications(struct npem *npem, u32 inds)
{
	int ctrl, ret, ret_val;
	u32 cc_status;

	lockdep_assert_held(&npem->lock);

	/* This bit is always required */
	ctrl = inds | PCI_NPEM_CTRL_ENABLE;

	ret = npem_write_ctrl(npem, ctrl);
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
	if (ret)
		return ret;
	if (ret_val)
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

static const struct npem_ops npem_ops = {
	.get_active_indications = npem_get_active_indications,
	.set_active_indications = npem_set_active_indications,
	.name = "Native PCIe Enclosure Management",
	.inds = npem_indications,
};

#define DSM_GUID GUID_INIT(0x5d524d9d, 0xfff9, 0x4d4b, 0x8c, 0xb7, 0x74, 0x7e,\
			   0xd5, 0x1e, 0x19, 0x4d)
#define GET_SUPPORTED_STATES_DSM	1
#define GET_STATE_DSM			2
#define SET_STATE_DSM			3

static const guid_t dsm_guid = DSM_GUID;

static bool npem_has_dsm(struct pci_dev *pdev)
{
	acpi_handle handle;

	handle = ACPI_HANDLE(&pdev->dev);
	if (!handle)
		return false;

	return acpi_check_dsm(handle, &dsm_guid, 0x1,
			      BIT(GET_SUPPORTED_STATES_DSM) |
			      BIT(GET_STATE_DSM) | BIT(SET_STATE_DSM));
}

struct dsm_output {
	u16 status;
	u8 function_specific_err;
	u8 vendor_specific_err;
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
static int dsm_evaluate(struct pci_dev *pdev, u64 dsm_func,
			struct dsm_output *output, u32 value_to_set)
{
	acpi_handle handle = ACPI_HANDLE(&pdev->dev);
	union acpi_object *out_obj, arg3[2];
	union acpi_object *arg3_p = NULL;

	if (dsm_func == SET_STATE_DSM) {
		arg3[0].type = ACPI_TYPE_PACKAGE;
		arg3[0].package.count = 1;
		arg3[0].package.elements = &arg3[1];

		arg3[1].type = ACPI_TYPE_BUFFER;
		arg3[1].buffer.length = 4;
		arg3[1].buffer.pointer = (u8 *)&value_to_set;

		arg3_p = arg3;
	}

	out_obj = acpi_evaluate_dsm_typed(handle, &dsm_guid, 0x1, dsm_func,
					  arg3_p, ACPI_TYPE_BUFFER);
	if (!out_obj)
		return -EIO;

	if (out_obj->buffer.length < sizeof(struct dsm_output)) {
		ACPI_FREE(out_obj);
		return -EIO;
	}

	memcpy(output, out_obj->buffer.pointer, sizeof(struct dsm_output));

	ACPI_FREE(out_obj);
	return 0;
}

static int dsm_get(struct pci_dev *pdev, u64 dsm_func, u32 *buf)
{
	struct dsm_output output;
	int ret = dsm_evaluate(pdev, dsm_func, &output, 0);

	if (ret)
		return ret;

	if (output.status != 0)
		return -EIO;

	*buf = output.state;
	return 0;
}

static int dsm_get_active_indications(struct npem *npem, u32 *buf)
{
	int ret = dsm_get(npem->dev, GET_STATE_DSM, buf);

	/* Filter out not supported indications in response */
	*buf &= npem->supported_indications;
	return ret;
}

static int dsm_set_active_indications(struct npem *npem, u32 value)
{
	struct dsm_output output;
	int ret = dsm_evaluate(npem->dev, SET_STATE_DSM, &output, value);

	if (ret)
		return ret;

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
		fallthrough;
	case 0:
		break;
	default:
		return -EIO;
	}

	npem->active_indications = output.state;

	return 0;
}

static const struct npem_ops dsm_ops = {
	.get_active_indications = dsm_get_active_indications,
	.set_active_indications = dsm_set_active_indications,
	.name = "_DSM PCIe SSD Status LED Management",
	.inds = dsm_indications,
};

static int npem_initialize_active_indications(struct npem *npem)
{
	int ret;

	lockdep_assert_held(&npem->lock);

	if (npem->active_inds_initialized)
		return 0;

	ret = npem->ops->get_active_indications(npem,
						&npem->active_indications);
	if (ret)
		return ret;

	npem->active_inds_initialized = true;
	return 0;
}

/*
 * The status of each indicator is cached on first brightness_ get/set time
 * and updated at write time.  brightness_get() is only responsible for
 * reflecting the last written/cached value.
 */
static enum led_brightness brightness_get(struct led_classdev *led)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	struct npem *npem = nled->npem;
	int ret, val = 0;

	ret = mutex_lock_interruptible(&npem->lock);
	if (ret)
		return ret;

	ret = npem_initialize_active_indications(npem);
	if (ret)
		goto out;

	if (npem->active_indications & nled->indication->bit)
		val = 1;

out:
	mutex_unlock(&npem->lock);
	return val;
}

static int brightness_set(struct led_classdev *led,
			  enum led_brightness brightness)
{
	struct npem_led *nled = container_of(led, struct npem_led, led);
	struct npem *npem = nled->npem;
	u32 indications;
	int ret;

	ret = mutex_lock_interruptible(&npem->lock);
	if (ret)
		return ret;

	ret = npem_initialize_active_indications(npem);
	if (ret)
		goto out;

	if (brightness == 0)
		indications = npem->active_indications & ~(nled->indication->bit);
	else
		indications = npem->active_indications | nled->indication->bit;

	ret = npem->ops->set_active_indications(npem, indications);

out:
	mutex_unlock(&npem->lock);
	return ret;
}

static void npem_free(struct npem *npem)
{
	struct npem_led *nled;
	int cnt;

	if (!npem)
		return;

	for (cnt = 0; cnt < npem->led_cnt; cnt++) {
		nled = &npem->leds[cnt];

		if (nled->name[0])
			led_classdev_unregister(&nled->led);
	}

	mutex_destroy(&npem->lock);
	kfree(npem);
}

static int pci_npem_set_led_classdev(struct npem *npem, struct npem_led *nled)
{
	struct led_classdev *led = &nled->led;
	struct led_init_data init_data = {};
	char *name = nled->name;
	int ret;

	init_data.devicename = pci_name(npem->dev);
	init_data.default_label = nled->indication->name;

	ret = led_compose_name(&npem->dev->dev, &init_data, name);
	if (ret)
		return ret;

	led->name = name;
	led->brightness_set_blocking = brightness_set;
	led->brightness_get = brightness_get;
	led->max_brightness = 1;
	led->default_trigger = "none";
	led->flags = LED_HW_PLUGGABLE;

	ret = led_classdev_register(&npem->dev->dev, led);
	if (ret)
		/* Clear the name to indicate that it is not registered. */
		name[0] = 0;
	return ret;
}

static int pci_npem_init(struct pci_dev *dev, const struct npem_ops *ops,
			 int pos, u32 caps)
{
	u32 supported = reg_to_indications(caps, ops->inds);
	int supported_cnt = hweight32(supported);
	const struct indication *indication;
	struct npem_led *nled;
	struct npem *npem;
	int led_idx = 0;
	int ret;

	npem = kzalloc_flex(*npem, leds, supported_cnt);
	if (!npem)
		return -ENOMEM;

	npem->supported_indications = supported;
	npem->led_cnt = supported_cnt;
	npem->pos = pos;
	npem->dev = dev;
	npem->ops = ops;

	mutex_init(&npem->lock);

	for_each_indication(indication, npem_indications) {
		if (!(npem->supported_indications & indication->bit))
			continue;

		nled = &npem->leds[led_idx++];
		nled->indication = indication;
		nled->npem = npem;

		ret = pci_npem_set_led_classdev(npem, nled);
		if (ret) {
			npem_free(npem);
			return ret;
		}
	}

	dev->npem = npem;
	return 0;
}

void pci_npem_remove(struct pci_dev *dev)
{
	npem_free(dev->npem);
}

void pci_npem_create(struct pci_dev *dev)
{
	const struct npem_ops *ops = &npem_ops;
	int pos = 0, ret;
	u32 cap;

	if (npem_has_dsm(dev)) {
		/*
		 * OS should use the DSM for LED control if it is available
		 * PCI Firmware Spec r3.3 sec 4.7.
		 */
		ret = dsm_get(dev, GET_SUPPORTED_STATES_DSM, &cap);
		if (ret)
			return;

		ops = &dsm_ops;
	} else {
		pos = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_NPEM);
		if (pos == 0)
			return;

		if (pci_read_config_dword(dev, pos + PCI_NPEM_CAP, &cap) != 0 ||
		    (cap & PCI_NPEM_CAP_CAPABLE) == 0)
			return;
	}

	pci_info(dev, "Configuring %s\n", ops->name);

	ret = pci_npem_init(dev, ops, pos, cap);
	if (ret)
		pci_err(dev, "Failed to register %s, err: %d\n", ops->name,
			ret);
}
