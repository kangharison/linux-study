// SPDX-License-Identifier: GPL-2.0+
/*
 * ACPI PCI Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2001 IBM Corp.
 * Copyright (C) 2002 Hiroshi Aono (h-aono@ap.jp.nec.com)
 * Copyright (C) 2002,2003 Takayoshi Kochi (t-kochi@bq.jp.nec.com)
 * Copyright (C) 2002,2003 NEC Corporation
 * Copyright (C) 2003-2005 Matthew Wilcox (willy@infradead.org)
 * Copyright (C) 2003-2005 Hewlett Packard
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 *
 */

#define pr_fmt(fmt) "acpiphp: " fmt

/*
 * [한국어 설명] ACPI 핫플러그의 sysfs 쪽 얼굴 (acpiphp_core.c)
 *
 * === 파일의 역할 ===
 * acpiphp 는 두 파일로 나뉜다. 이 파일은 핫플러그 코어와 맞닿는 부분이고,
 * 실제 ACPI 네임스페이스 처리와 이벤트 수신은 acpiphp_glue.c 가 한다.
 *
 * ACPI 핫플러그가 따로 필요한 이유는 PCIe 표준 핫플러그가 없는 환경 때문이다.
 *   - 구형 PCI/PCI-X 버스에는 표준 핫플러그 레지스터가 아예 없다.
 *   - 가상화 환경에서 장치를 붙이고 떼는 것도 대개 ACPI 이벤트로 온다.
 *   - PCIe 슬롯이라도 펌웨어가 _OSC 로 소유권을 안 넘기면 pciehp 가 못 붙는다.
 * 이런 경우 슬롯 조작은 ACPI 제어 메서드(_EJ0, _PS0, _STA 등)로 한다.
 *
 * 이 파일이 하는 일은 그 메서드들을 hotplug_slot_ops 모양으로 감싸는 것이다.
 * 사용자가 sysfs 의 power 에 0 을 쓰면 disable_slot() 이 불리고, 그것이
 * acpiphp_glue.c 를 거쳐 결국 ACPI _EJ0 메서드 평가로 이어진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * ACPI 코어가 핫플러그 알림(Notify)을 받음
 *   -> acpiphp_glue.c 의 핸들러
 *      -> 슬롯 상태를 갱신하고 열거 또는 제거
 *
 * 사용자가 sysfs 조작
 *   -> pci_hotplug_core.c
 *      -> [이 파일] enable_slot() / disable_slot() / get_*_status()
 *         -> acpiphp_glue.c 의 실제 ACPI 메서드 평가
 *
 * 실행 컨텍스트: sysfs 경로는 프로세스 컨텍스트. ACPI 알림은 ACPI 코어의
 * 워크큐에서 처리된다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci_hotplug_core.c.
 * 아래쪽: acpiphp_glue.c, 그리고 그 아래 ACPI 코어(drivers/acpi/).
 * 옆쪽: pci-acpi.c 가 PCI 장치와 ACPI 노드를 잇는 매핑을 제공한다.
 * 공유 상태: struct acpiphp_slot(acpiphp.h) — ACPI 핸들과 슬롯 정보.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 실무에서 의미가 있는 경우는 두 가지다. 하나는 가상 머신에서 NVMe 를
 * 핫플러그로 붙일 때 — QEMU 등이 ACPI 이벤트로 알리므로 pciehp 가 아니라
 * 이 드라이버가 처리한다. 다른 하나는 펌웨어가 _OSC 협상에서 핫플러그
 * 소유권을 유지하는 서버로, 이때도 이쪽이 담당한다.
 *
 * === 주요 함수/구조체 요약 ===
 * init_acpi() / acpiphp_init() : 모듈 초기화. glue 계층을 준비한다.
 * enable_slot() / disable_slot(): sysfs 전원 조작을 ACPI 쪽으로 넘긴다.
 * set_attention_status() / get_attention_status() : Attention LED.
 *                          ACPI 에서는 _STA 등으로 표현된다.
 * get_power_status() / get_latch_status() / get_adapter_status() : 상태 조회.
 * acpiphp_register_hotplug_slot() : glue 가 슬롯을 발견하면 이 함수로
 *                          핫플러그 코어에 등록한다.
 * acpiphp_unregister_hotplug_slot() : 그 반대.
 * acpiphp_hotplug_slot_ops : 위 콜백들을 묶은 표.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci-acpi.h>
#include <linux/pci_hotplug.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include "acpiphp.h"
/* name size which is used for entries in pcihpfs */
#define SLOT_NAME_SIZE  21              /* {_SUN} */

bool acpiphp_disabled;

/* local variables */
static struct acpiphp_attention_info *attention_info;

#define DRIVER_VERSION	"0.5"

#define DRIVER_AUTHOR	"Greg Kroah-Hartman <gregkh@us.ibm.com>, Takayoshi Kochi <t-kochi@bq.jp.nec.com>, Matthew Wilcox <willy@infradead.org>"
#define DRIVER_DESC	"ACPI Hot Plug PCI Controller Driver"
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_PARM_DESC(disable, "disable acpiphp driver");
/* [한국어] acpiphp_disabled 를 "disable" 이라는 이름의 모듈 파라미터로 노출한다.
 * 권한 0444 는 읽기 전용이라, 부팅 인자로만 지정할 수 있고 런타임에는
 * 바꿀 수 없다 — 드라이버 활성화 여부는 부팅 시점에 확정되어야 하기 때문이다. */
module_param_named(disable, acpiphp_disabled, bool, 0444);

/* [한국어] 아래 ops 테이블에서 참조하기 위한 전방 선언. 정의가 테이블보다 뒤에 있어
 * 일곱 개를 미리 선언해 둔다. */
/* [한국어]
 * enable_slot - 슬롯에 전원을 넣고 장치를 구성한다(공용 코어 콜백)
 *
 * @hotplug_slot: 공용 코어가 주는 슬롯 포인터.
 * @return: acpiphp_enable_slot() 의 결과.
 *
 * 위 커널독대로 실제 작업은 acpiphp_glue.c 의 acpiphp_enable_slot() 이 한다.
 * 이 함수는 공용 코어의 hotplug_slot 을 acpiphp 의 slot 으로 바꾸고 넘기는
 * 얇은 어댑터일 뿐이다.
 *
 * 이 파일 전체가 그런 어댑터 계층이라는 점이 중요하다 — 공용 코어와
 * acpiphp 코어 사이의 타입 변환과 로그만 담당하고, ACPI 메서드 평가나
 * PCI 열거는 전혀 하지 않는다.
 *
 * 실행 컨텍스트: 사용자의 sysfs power 쓰기 문맥, 프로세스 컨텍스트.
 * 하위에서 ACPI 메서드와 PCI 열거가 일어나므로 오래 걸린다.
 *
 * 에러 경로: 하위 결과를 그대로 전달한다.
 *
 * 호출 체인:
 *   사용자의 echo 1 > .../power → 공용 코어
 *     → hotplug_slot_ops.enable_slot == [이 함수]
 *     → to_slot() → acpiphp_enable_slot()
 */
static int enable_slot(struct hotplug_slot *slot);
/* [한국어] 슬롯 비활성화 콜백 선언. */
/* [한국어]
 * disable_slot - 슬롯의 장치를 제거하고 전원을 끊는다(공용 코어 콜백)
 *
 * @hotplug_slot: 공용 코어가 주는 슬롯 포인터.
 * @return: acpiphp_disable_slot() 의 결과.
 *
 * enable_slot 과 정확히 대칭인 어댑터다. 실제 작업은 acpiphp_glue.c 가 한다.
 *
 * 실행 컨텍스트: 사용자의 sysfs power 쓰기 문맥, 프로세스 컨텍스트.
 * 하위 장치의 드라이버 remove 가 연쇄로 불린다.
 *
 * 에러 경로: 하위 결과를 그대로 전달한다.
 *
 * 호출 체인:
 *   사용자의 echo 0 > .../power → 공용 코어
 *     → hotplug_slot_ops.disable_slot == [이 함수]
 *     → to_slot() → acpiphp_disable_slot()
 */
static int disable_slot(struct hotplug_slot *slot);
/* [한국어] attention LED 설정 콜백 선언. */
/* [한국어]
 * set_attention_status - 등록된 확장을 통해 attention LED 를 설정한다
 *
 * @hotplug_slot: 대상 슬롯.
 * @status: 설정할 값.
 * @return: 확장 콜백의 결과, 또는 -ENODEV(확장이 없거나 모듈을 잡을 수 없음).
 *
 * 위 커널독이 설명하듯 ACPI 에는 LED 를 제어하는 표준 메서드가 없어,
 * 등록된 플랫폼별 확장에 위임한다.
 *
 * try_module_get() 이 이 함수의 핵심이다. 확장 모듈이 지금 언로드 중일 수
 * 있으므로, 콜백을 부르기 전에 그 모듈의 참조를 잡아야 한다. 참조를 잡는 데
 * 성공했다면 호출이 끝날 때까지 모듈이 사라지지 않음이 보장된다.
 *
 * 다른 조회 콜백들과 달리 to_slot() 을 쓰지 않고 hotplug_slot 을 그대로
 * 확장에 넘긴다 — 확장 드라이버가 acpiphp 내부 구조를 알 필요가 없게 하려는
 * 설계다.
 *
 * [상류 코드 관찰, 수정하지 않음] else 갈래가 전역 attention_info 를 NULL 로
 * 지운다. 모듈이 언로드 중이라면 합리적인 정리이지만, 확장이 애초에 등록되지
 * 않아 여기 온 경우에는 이미 NULL 인 것을 다시 쓰는 셈이다. 그리고 이 쓰기에
 * 락이 없어, 동시에 register 가 진행 중이라면 방금 등록된 확장을 지울 수 있다.
 *
 * 실행 컨텍스트: 사용자의 sysfs attention 쓰기 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: 확장 부재와 모듈 참조 실패가 같은 갈래로 처리된다.
 *
 * 호출 체인:
 *   사용자의 sysfs attention 쓰기 → 공용 코어
 *     → hotplug_slot_ops.set_attention_status == [이 함수]
 *     → try_module_get() → attention_info->set_attn() → module_put()
 */
static int set_attention_status(struct hotplug_slot *slot, u8 value);
/* [한국어] 전원 상태 조회 콜백 선언. */
/* [한국어]
 * get_power_status - 슬롯의 전원 상태를 조회한다(공용 코어 콜백)
 *
 * @hotplug_slot: 대상 슬롯.
 * @value: 결과를 담을 출력 인자.
 * @return: 언제나 0.
 *
 * acpiphp 코어의 acpiphp_get_power_status() 에 위임한다. 그 함수는 ACPI 의
 * _STA 메서드를 평가해 상태를 얻는다.
 *
 * 위 커널독이 경고를 담고 있다 — 일부 플랫폼이 _STA 를 제대로 구현하지 않아
 * 돌려주는 값을 신뢰할 수 없을 수 있다. 그럼에도 오류를 돌려줄 방법이 없어
 * 언제나 성공으로 답한다.
 *
 * 실행 컨텍스트: 사용자의 sysfs power 읽기 문맥, 프로세스 컨텍스트.
 * ACPI 메서드 평가가 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자의 cat .../power → 공용 코어
 *     → hotplug_slot_ops.get_power_status == [이 함수]
 *     → to_slot() → acpiphp_get_power_status()
 */
static int get_power_status(struct hotplug_slot *slot, u8 *value);
/* [한국어] attention LED 조회 콜백 선언. */
/* [한국어]
 * get_attention_status - 등록된 확장을 통해 attention LED 상태를 조회한다
 *
 * @hotplug_slot: 대상 슬롯.
 * @value: 결과를 담을 출력 인자.
 * @return: 확장 콜백의 결과, 또는 -EINVAL(확장이 없거나 모듈을 잡을 수 없음).
 *
 * set_attention_status 와 완전히 같은 구조다 — 위 커널독이 밝히듯 ACPI 에
 * LED 상태를 알아낼 표준 메서드가 없어 확장에 위임하고, try_module_get 으로
 * 확장 모듈의 수명을 보장한다.
 *
 * [상류 코드 관찰] 같은 조건에서 set 쪽은 -ENODEV 를, 이쪽은 -EINVAL 을
 * 돌려준다. 두 함수의 오류 코드가 비대칭이다.
 * else 갈래가 전역을 NULL 로 지우는 문제도 set 쪽과 같다.
 *
 * 실행 컨텍스트: 사용자의 sysfs attention 읽기 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: 확장 부재와 모듈 참조 실패가 같은 갈래다.
 *
 * 호출 체인:
 *   사용자의 cat .../attention → 공용 코어
 *     → hotplug_slot_ops.get_attention_status == [이 함수]
 *     → try_module_get() → attention_info->get_attn() → module_put()
 */
static int get_attention_status(struct hotplug_slot *slot, u8 *value);
/* [한국어] 래치 상태 조회 콜백 선언. */
/* [한국어]
 * get_latch_status - 슬롯의 이젝터 래치 상태를 조회한다(공용 코어 콜백)
 *
 * @hotplug_slot: 대상 슬롯.
 * @value: 결과를 담을 출력 인자.
 * @return: 언제나 0.
 *
 * 위 커널독이 솔직하게 밝힌다 — ACPI 에는 래치 상태에 접근할 공식 수단이
 * 없어서 _STA 값에서 "지어낸다(fake)". 물리적으로 래치가 열렸는지를 알 수 없고,
 * 장치가 살아 있는지만 알 수 있기 때문이다.
 *
 * 그래서 이 값은 진짜 래치 상태가 아니라 근사이며, sysfs 를 읽는 사용자가
 * 그것을 구분할 방법은 없다.
 *
 * 실행 컨텍스트: 사용자의 sysfs latch 읽기 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자의 cat .../latch → 공용 코어
 *     → hotplug_slot_ops.get_latch_status == [이 함수]
 *     → to_slot() → acpiphp_get_latch_status()
 */
static int get_latch_status(struct hotplug_slot *slot, u8 *value);
/* [한국어] 어댑터(카드) 존재 조회 콜백 선언. */
/* [한국어]
 * get_adapter_status - 슬롯에 카드가 꽂혀 있는지 조회한다(공용 코어 콜백)
 *
 * @hotplug_slot: 대상 슬롯.
 * @value: 결과를 담을 출력 인자.
 * @return: 언제나 0.
 *
 * get_latch_status 와 같은 사정이다 — 위 커널독대로 ACPI 에 어댑터 존재를
 * 확인할 공식 수단이 없어 _STA 에서 지어낸다.
 *
 * 래치와 어댑터가 같은 _STA 값에서 나온다는 것은, 두 상태를 실제로 구분할 수
 * 없다는 뜻이기도 하다. 물리적 감지기를 갖춘 SHPC 나 PCIe 핫플러그와 대비되는
 * ACPI 핫플러그의 근본적 한계다.
 *
 * 실행 컨텍스트: 사용자의 sysfs adapter 읽기 문맥, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자의 cat .../adapter → 공용 코어
 *     → hotplug_slot_ops.get_adapter_status == [이 함수]
 *     → to_slot() → acpiphp_get_adapter_status()
 */
static int get_adapter_status(struct hotplug_slot *slot, u8 *value);

/* [한국어] PCI 핫플러그 공용 코어에 제공할 콜백 테이블. 이 테이블 하나가 acpiphp 와
 * 공용 코어의 전체 접점이다. */
static const struct hotplug_slot_ops acpi_hotplug_slot_ops = {
	/* [한국어] sysfs 의 power 파일에 1 을 쓸 때. */
	.enable_slot		= enable_slot,
	.disable_slot		= disable_slot,
	.set_attention_status	= set_attention_status,
	.get_power_status	= get_power_status,
	.get_attention_status	= get_attention_status,
	.get_latch_status	= get_latch_status,
	.get_adapter_status	= get_adapter_status,
};

/**
 * acpiphp_register_attention - set attention LED callback
 * @info: must be completely filled with LED callbacks
 *
 * Description: This is used to register a hardware specific ACPI
 * driver that manipulates the attention LED.  All the fields in
 * info must be set.
 */
/* [한국어]
 * acpiphp_register_attention - attention LED 제어 확장을 등록한다
 *
 * @info: set_attn / get_attn / owner 가 모두 채워진 확장 서술.
 * @return: 0 = 등록 성공, -EINVAL = 인자가 불완전하거나 이미 다른 확장이 등록됨.
 *
 * 왜 확장이 필요한가: 위 커널독과 set_attention_status() 의 커널독이 밝히듯,
 * ACPI 에는 attention LED 를 제어하는 표준 메서드가 없다. 그래서 플랫폼별
 * 드라이버(예: acpiphp_ampere_altra.c)가 자기 방식으로 LED 를 다루고,
 * acpiphp 는 그것을 콜백으로 받아 쓴다.
 *
 * 등록 조건이 네 가지다 — info 가 유효하고, 두 콜백이 모두 채워져 있고,
 * 아직 등록된 확장이 없어야 한다. 마지막 조건 때문에 확장은 시스템에 하나만
 * 존재할 수 있는데, 전역 변수 하나로 관리하기 때문이다.
 *
 * [상류 코드 관찰] 전역 attention_info 를 락 없이 읽고 쓴다. 등록·해제가
 * 모듈 적재·해제 시점에만 일어난다는 전제 위에 있다.
 *
 * 실행 컨텍스트: 확장 모듈의 probe 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 조건을 만족하지 않으면 아무것도 바꾸지 않고 -EINVAL 을 돌려준다.
 *
 * 호출 체인:
 *   확장 드라이버의 probe(예: altra_led_probe) → [이 함수]
 */
int acpiphp_register_attention(struct acpiphp_attention_info *info)
{
	/* [한국어] 실패를 기본값으로 두고 시작한다 — 아래 조건을 모두 만족할 때만 성공으로 바뀐다. */
	int retval = -EINVAL;

	/* [한국어] 네 조건을 모두 확인한다. info 가 유효하고, 두 콜백이 모두 채워져 있고,
	 * 아직 등록된 확장이 없어야 한다. 위 커널독이 "info 의 모든 필드가
	 * 채워져야 한다"고 요구하는 것을 코드로 강제하는 부분이다.
	 * 확장을 하나만 허용하는 것은 전역 변수 하나로 관리하기 때문이다. */
	if (info && info->set_attn && info->get_attn && !attention_info) {
		/* [한국어] 성공으로 바꾸고, */
		retval = 0;
		/* [한국어] 전역에 등록한다. */
		attention_info = info;
	}
	/* [한국어] 0 또는 -EINVAL 이 나간다. */
	return retval;
}
EXPORT_SYMBOL_GPL(acpiphp_register_attention);


/**
 * acpiphp_unregister_attention - unset attention LED callback
 * @info: must match the pointer used to register
 *
 * Description: This is used to un-register a hardware specific acpi
 * driver that manipulates the attention LED.  The pointer to the
 * info struct must be the same as the one used to set it.
 */
/* [한국어]
 * acpiphp_unregister_attention - 등록했던 확장을 해제한다
 *
 * @info: 등록할 때 쓴 것과 같은 포인터여야 한다.
 * @return: 0 = 해제 성공, -EINVAL = 인자가 NULL 이거나 등록된 것과 다름.
 *
 * 포인터를 대조하는 것이 핵심이다. 다른 포인터로 해제를 시도하면 남의 확장을
 * 지우게 되므로, 위 커널독이 "등록에 쓴 것과 같은 포인터여야 한다"고 명시하고
 * 코드가 그것을 검사한다.
 *
 * 실행 컨텍스트: 확장 모듈의 remove 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 대조에 실패하면 아무것도 바꾸지 않는다.
 *
 * 호출 체인:
 *   확장 드라이버의 remove(예: altra_led_remove) → [이 함수]
 */
int acpiphp_unregister_attention(struct acpiphp_attention_info *info)
{
	/* [한국어] 역시 실패를 기본값으로 둔다. */
	int retval = -EINVAL;

	/* [한국어] 위 커널독대로 등록할 때 쓴 것과 같은 포인터여야 한다. 다른 포인터로
	 * 해제를 시도하면 남의 확장을 지우게 되므로 반드시 대조한다. */
	if (info && attention_info == info) {
		/* [한국어] 전역을 비운다. */
		attention_info = NULL;
		/* [한국어] 성공으로 바꾼다. */
		retval = 0;
	}
	/* [한국어] 0 또는 -EINVAL 이 나간다. */
	return retval;
}
EXPORT_SYMBOL_GPL(acpiphp_unregister_attention);


/**
 * enable_slot - power on and enable a slot
 * @hotplug_slot: slot to enable
 *
 * Actual tasks are done in acpiphp_enable_slot()
 */
static int enable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 공용 코어가 준 포인터에서 acpiphp 의 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 어느 슬롯에 대한 요청인지 디버그 로그로 남긴다. pr_debug 라
	 * 동적 디버그를 켜야 출력된다. */
	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* enable the specified slot */
	return acpiphp_enable_slot(slot->acpi_slot);
}


/**
 * disable_slot - disable and power off a slot
 * @hotplug_slot: slot to disable
 *
 * Actual tasks are done in acpiphp_disable_slot()
 */
static int disable_slot(struct hotplug_slot *hotplug_slot)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 어느 슬롯에 대한 요청인지 디버그 로그로 남긴다. pr_debug 라
	 * 동적 디버그를 켜야 출력된다. */
	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* disable the specified slot */
	return acpiphp_disable_slot(slot->acpi_slot);
}


/**
 * set_attention_status - set attention LED
 * @hotplug_slot: slot to set attention LED on
 * @status: value to set attention LED to (0 or 1)
 *
 * attention status LED, so we use a callback that
 * was registered with us.  This allows hardware specific
 * ACPI implementations to blink the light for us.
 */
static int set_attention_status(struct hotplug_slot *hotplug_slot, u8 status)
{
	/* [한국어] 기본값이 -ENODEV 다 — 확장이 등록되지 않았으면 LED 를 제어할 하드웨어
	 * 수단이 없다는 뜻이라, -EINVAL(잘못된 요청)이 아니라 -ENODEV(장치 없음)가 맞다. */
	int retval = -ENODEV;

	/* [한국어] 이 경로는 slot 객체를 쓰지 않고 hotplug_slot 이름을 직접 찍는다 —
	 * 확장 콜백에 hotplug_slot 을 그대로 넘기기 때문이다. */
	pr_debug("%s - physical_slot = %s\n", __func__,
		hotplug_slot_name(hotplug_slot));

	/* [한국어] 확장이 등록되어 있고, 그 모듈의 참조를 잡을 수 있어야 한다.
	 * try_module_get 이 필요한 이유는 확장 모듈이 지금 언로드 중일 수 있어서다 —
	 * 참조를 잡지 못했다는 것은 그 모듈이 사라지는 중이라는 뜻이다. */
	if (attention_info && try_module_get(attention_info->owner)) {
		/* [한국어] 확장의 콜백을 부른다. 잡아 둔 모듈 참조가 이 호출 동안 모듈이 사라지지 않음을 보장한다. */
		retval = attention_info->set_attn(hotplug_slot, status);
		/* [한국어] 참조를 놓는다. */
		module_put(attention_info->owner);
	/* [한국어] 확장이 없거나 모듈 참조를 못 잡은 경우. */
	} else
		/* [한국어] [상류 코드 관찰] 전역 포인터를 NULL 로 지운다. 모듈이 언로드 중이라면
		 * 합리적인 정리이지만, 애초에 확장이 등록되지 않아 여기 온 경우에도
		 * 이미 NULL 인 것을 다시 NULL 로 쓰는 셈이다. 또 이 쓰기에 락이 없어,
		 * 동시에 register 가 진행 중이면 방금 등록된 확장을 지울 수 있다. */
		attention_info = NULL;
	/* [한국어] 확장 콜백의 결과 또는 -ENODEV 가 나간다. */
	return retval;
}


/**
 * get_power_status - get power status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * Some platforms may not implement _STA method properly.
 * In that case, the value returned may not be reliable.
 */
static int get_power_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 어느 슬롯에 대한 요청인지 디버그 로그로 남긴다. pr_debug 라
	 * 동적 디버그를 켜야 출력된다. */
	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] acpiphp 코어에 실제 조회를 맡긴다. 위 커널독이 경고하듯 _STA 메서드를
	 * 제대로 구현하지 않은 플랫폼에서는 이 값이 신뢰할 수 없다. */
	*value = acpiphp_get_power_status(slot->acpi_slot);

	/* [한국어] 조회 자체는 실패하지 않으므로 언제나 성공이다. */
	return 0;
}


/**
 * get_attention_status - get attention LED status
 * @hotplug_slot: slot to get status from
 * @value: returns with value of attention LED
 *
 * ACPI doesn't have known method to determine the state
 * of the attention status LED, so we use a callback that
 * was registered with us.  This allows hardware specific
 * ACPI implementations to determine its state.
 */
static int get_attention_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 기본값이 -EINVAL 이다. set 쪽이 -ENODEV 인 것과 다른데, 두 함수가
	 * 같은 조건에서 다른 오류를 돌려주는 비대칭이다. */
	int retval = -EINVAL;

	/* [한국어] set 쪽과 같은 방식으로 이름을 찍는다. */
	pr_debug("%s - physical_slot = %s\n", __func__,
		hotplug_slot_name(hotplug_slot));

	/* [한국어] 확장 등록 여부와 모듈 참조를 확인한다. */
	if (attention_info && try_module_get(attention_info->owner)) {
		/* [한국어] 확장의 조회 콜백을 부른다. */
		retval = attention_info->get_attn(hotplug_slot, value);
		/* [한국어] 참조를 놓는다. */
		module_put(attention_info->owner);
	/* [한국어] 확장이 없거나 참조 실패면, */
	} else
		/* [한국어] 전역을 지운다. set 쪽과 같은 관찰이 적용된다. */
		attention_info = NULL;
	/* [한국어] 확장 결과 또는 -EINVAL 이 나간다. */
	return retval;
}


/**
 * get_latch_status - get latch status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * ACPI doesn't provide any formal means to access latch status.
 * Instead, we fake latch status from _STA.
 */
static int get_latch_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 어느 슬롯에 대한 요청인지 디버그 로그로 남긴다. pr_debug 라
	 * 동적 디버그를 켜야 출력된다. */
	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] 위 커널독대로 ACPI 에는 래치 상태를 알아낼 공식 수단이 없어,
	 * _STA 값에서 지어낸다. */
	*value = acpiphp_get_latch_status(slot->acpi_slot);

	/* [한국어] 언제나 성공. */
	return 0;
}


/**
 * get_adapter_status - get adapter status of a slot
 * @hotplug_slot: slot to get status
 * @value: pointer to store status
 *
 * ACPI doesn't provide any formal means to access adapter status.
 * Instead, we fake adapter status from _STA.
 */
static int get_adapter_status(struct hotplug_slot *hotplug_slot, u8 *value)
{
	/* [한국어] 슬롯 객체를 되찾는다. */
	struct slot *slot = to_slot(hotplug_slot);

	/* [한국어] 어느 슬롯에 대한 요청인지 디버그 로그로 남긴다. pr_debug 라
	 * 동적 디버그를 켜야 출력된다. */
	pr_debug("%s - physical_slot = %s\n", __func__, slot_name(slot));

	/* [한국어] 래치와 마찬가지로 _STA 에서 지어낸 값이다. */
	*value = acpiphp_get_adapter_status(slot->acpi_slot);

	/* [한국어] 언제나 성공. */
	return 0;
}

/* callback routine to initialize 'struct slot' for each slot */
/* [한국어]
 * acpiphp_register_hotplug_slot - acpiphp 슬롯 하나를 공용 코어에 등록한다
 *
 * @acpiphp_slot: acpiphp 코어가 만든 슬롯 객체.
 * @sun: ACPI 의 _SUN 메서드가 주는 사용자에게 보이는 슬롯 번호.
 * @return: 0 = 성공. -ENOMEM = 할당 실패. -EBUSY = 같은 이름의 슬롯이 이미 있음.
 *       그 밖의 음수 = 공용 코어 등록 실패.
 *
 * 위 영어 주석이 "각 슬롯마다 struct slot 을 초기화하는 콜백" 이라고 밝힌다.
 * acpiphp 코어가 ACPI 네임스페이스를 훑어 슬롯을 발견할 때마다 부른다.
 *
 * 동작 과정:
 *   1) 슬롯 객체를 0 초기화 할당한다.
 *   2) 콜백 테이블을 걸고 acpiphp 슬롯과 양방향으로 연결한다 — 두 객체가
 *      서로를 가리켜야 콜백에서 어느 쪽으로든 건너갈 수 있다.
 *   3) SUN 을 십진 문자열로 만들어 슬롯 이름으로 쓴다. 사용자가 sysfs 에서
 *      보게 될 디렉터리 이름이 그 번호다.
 *   4) pci_hp_register() 로 공용 코어에 등록한다.
 *
 * -EBUSY 를 로그 없이 조용히 처리하는 것이 눈에 띈다. 여러 ACPI 노드가 같은
 * 물리 슬롯을 가리키는 구성에서 흔히 발생하는 정상적인 상황이기 때문이다.
 *
 * [상류 코드 관찰, 수정하지 않음] 등록 실패 시 슬롯 객체를 해제하지만
 * acpiphp_slot->slot 은 되돌리지 않아, 해제된 포인터가 남는다.
 *
 * 실행 컨텍스트: acpiphp 코어의 슬롯 열거 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 두 라벨(error_slot, error)이 계단식으로 이어진다.
 *
 * 호출 체인:
 *   acpiphp_glue.c 의 슬롯 발견 경로 → [이 함수]
 *     → kzalloc_obj() → pci_hp_register()
 */
int acpiphp_register_hotplug_slot(struct acpiphp_slot *acpiphp_slot,
				  unsigned int sun)
{
	/* [한국어] 만들 슬롯 객체. */
	struct slot *slot;
	/* [한국어] 기본값을 -ENOMEM 으로 두어, 첫 실패 지점의 오류가 그대로 나가게 한다. */
	int retval = -ENOMEM;
	/* [한국어] 슬롯 이름을 만들 버퍼. */
	char name[SLOT_NAME_SIZE];

	/* [한국어] 슬롯 객체를 0 초기화 할당한다. */
	slot = kzalloc_obj(*slot);
	/* [한국어] 메모리 부족. */
	if (!slot)
		/* [한국어] 아무것도 잡지 않았으므로 반환만 하는 라벨로. */
		goto error;

	/* [한국어] 공용 코어가 부를 콜백 테이블을 건다. */
	slot->hotplug_slot.ops = &acpi_hotplug_slot_ops;

	/* [한국어] acpiphp 코어의 슬롯 객체를 가리키게 한다. */
	slot->acpi_slot = acpiphp_slot;

	/* [한국어] 반대 방향 연결도 만든다 — 두 객체가 서로를 가리켜야 콜백에서
	 * 어느 쪽으로든 건너갈 수 있다. */
	acpiphp_slot->slot = slot;
	/* [한국어] SUN(Slot User Number)을 기록한다. ACPI 의 _SUN 메서드가 주는
	 * 사용자에게 보이는 슬롯 번호다. */
	slot->sun = sun;
	/* [한국어] 그 번호를 십진 문자열로 만들어 슬롯 이름으로 쓴다. */
	snprintf(name, SLOT_NAME_SIZE, "%u", sun);

	/* [한국어] 공용 코어에 등록한다. 이 시점부터 sysfs 에 슬롯 디렉터리가 생긴다. */
	retval = pci_hp_register(&slot->hotplug_slot, acpiphp_slot->bus,
				 acpiphp_slot->device, name);
	/* [한국어] -EBUSY 는 같은 이름의 슬롯이 이미 있다는 뜻이다. 로그를 남기지 않고
	 * 조용히 정리하는데, 여러 ACPI 노드가 같은 물리 슬롯을 가리키는 구성에서
	 * 흔히 발생하는 정상적인 상황이기 때문이다. */
	if (retval == -EBUSY)
		goto error_slot;
	/* [한국어] 그 밖의 실패는 진짜 오류다. */
	if (retval) {
		/* [한국어] 무엇이 잘못됐는지 남긴다. */
		pr_err("pci_hp_register failed with error %d\n", retval);
		goto error_slot;
	}

	/* [한국어] 등록 완료를 알린다. */
	pr_info("Slot [%s] registered\n", slot_name(slot));

	/* [한국어] 성공. */
	return 0;
/* [한국어] 등록 실패 정리 라벨. */
error_slot:
	/* [한국어] 슬롯 객체를 해제한다.
	 * [상류 코드 관찰] acpiphp_slot->slot 은 이미 이 객체를 가리키도록 대입해
	 * 두었는데 여기서 되돌리지 않아, 해제된 포인터가 남는다. */
	kfree(slot);
/* [한국어] 할당 실패가 도달하는 라벨. */
error:
	/* [한국어] 기록해 둔 오류를 전달한다. */
	return retval;
}


/* [한국어]
 * acpiphp_unregister_hotplug_slot - 슬롯 등록을 해제하고 객체를 해제한다
 *
 * @acpiphp_slot: acpiphp 코어의 슬롯 객체. 그 안에서 이 파일의 슬롯을 꺼낸다.
 *
 * 순서가 이 함수의 전부다.
 *   1) 해제를 알리는 로그를 먼저 남긴다 — 슬롯 이름을 쓰므로 등록 해제보다
 *      앞서야 한다.
 *   2) pci_hp_deregister() 로 공용 코어에서 지운다. sysfs 항목이 사라지고,
 *      진행 중인 sysfs 접근이 끝날 때까지 코어가 기다려 준다.
 *   3) 그제야 객체를 해제한다. 2)와 3)의 순서를 바꾸면 사용자가 sysfs 를
 *      읽는 도중 그 뒤의 구조체가 사라져 use-after-free 가 된다.
 *
 * 실행 컨텍스트: acpiphp 코어의 슬롯 제거 경로, 프로세스 컨텍스트.
 * pci_hp_deregister 가 진행 중인 접근을 기다리므로 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   acpiphp_glue.c 의 슬롯 제거 경로 → [이 함수]
 *     → pci_hp_deregister() → kfree()
 */
void acpiphp_unregister_hotplug_slot(struct acpiphp_slot *acpiphp_slot)
{
	/* [한국어] acpiphp 코어의 슬롯에서 이 파일의 슬롯 객체를 꺼낸다. */
	struct slot *slot = acpiphp_slot->slot;

	/* [한국어] 해제를 알린다. 이름을 쓰는 로그이므로 아래 등록 해제보다 먼저 와야 한다. */
	pr_info("Slot [%s] unregistered\n", slot_name(slot));

	/* [한국어] 공용 코어에서 등록을 해제한다 — sysfs 항목이 사라지고 진행 중인 접근이
	 * 끝날 때까지 기다려 준다. */
	pci_hp_deregister(&slot->hotplug_slot);
	/* [한국어] 그제야 객체를 해제한다. 순서를 바꾸면 use-after-free 가 된다. */
	kfree(slot);
}


/* [한국어]
 * acpiphp_init - 드라이버 이름과 버전을 부팅 로그에 남긴다
 *
 * 이름과 달리 초기화라 할 만한 일을 하지 않는다. acpiphp 의 실제 초기화는
 * acpiphp_glue.c 가 ACPI 스캔 핸들러로 등록되면서 이루어지고, 이 함수는
 * 사용자에게 존재를 알리는 로그 한 줄이 전부다.
 *
 * disable 파라미터로 꺼져 있으면 그 사실을 함께 찍고 "버그를 신고해 달라"고
 * 덧붙인다 — 이 드라이버를 꺼야 하는 상황 자체가 고쳐야 할 문제라는
 * 상류의 판단이 담긴 문구다.
 *
 * __init 이므로 부팅 후 이 함수의 코드는 메모리에서 회수된다.
 *
 * 실행 컨텍스트: 커널 초기화 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   PCI 핫플러그 초기화 경로 → [acpiphp_init] → pr_info()
 */
void __init acpiphp_init(void)
{
	/* [한국어] 드라이버 이름과 버전을 부팅 로그에 남긴다. */
	pr_info(DRIVER_DESC " version: " DRIVER_VERSION "%s\n",
		/* [한국어] 사용자가 disable 파라미터로 껐다면 그 사실을 함께 알리고 버그 신고를 권한다 —
		 * 이 드라이버를 꺼야 하는 상황 자체가 고쳐야 할 문제라는 상류의 판단이다. */
		acpiphp_disabled ? ", disabled by user; please report a bug"
				 : "");
}
