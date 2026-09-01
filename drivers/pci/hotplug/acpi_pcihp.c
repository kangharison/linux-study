// SPDX-License-Identifier: GPL-2.0+
/*
 * Common ACPI functions for hot plug platforms
 *
 * Copyright (C) 2006 Intel Corporation
 *
 * All rights reserved.
 *
 * Send feedback to <kristen.c.accardi@intel.com>
 */

/*
 * [한국어 설명] 펌웨어와 핫플러그 소유권을 협상한다 (acpi_pcihp.c)
 *
 * === 파일의 역할 ===
 * 짧은 파일이지만 하는 일이 중요하다. PCI 핫플러그를 펌웨어가 다룰지
 * 운영체제가 다룰지를 정하는 협상, 그리고 그 결과를 판단하는 코드가 여기 있다.
 *
 * 왜 협상이 필요한가. 둘이 동시에 슬롯을 만지면 안 되기 때문이다. 펌웨어가
 * SMI 로 핫플러그를 처리하는 중에 운영체제가 같은 레지스터를 건드리면
 * 상태가 어긋난다. 그래서 부팅 시 _OSC 메서드로 "이 기능은 내가 맡겠다" 를
 * 주고받는다.
 *
 * 핵심 함수는 acpi_get_hp_hw_control_from_firmware() 다. 이름 그대로
 * 펌웨어에게서 하드웨어 제어권을 받아 오는 일을 한다. 실패하면 그 슬롯의
 * 핫플러그 드라이버는 바인딩을 포기해야 한다.
 *
 * 여기에 얽힌 실무 함정이 하나 있다. 서버 BIOS 가 "OS 는 관여하지 말라"
 * 고 답하면 pciehp 가 붙지 않아 드라이브를 뽑았다 꽂아도 커널이 모른다.
 * 그럴 때 쓰는 것이 pciehp_force 부팅 인자로, 협상 결과를 무시하고
 * 강제로 잡는 것이다. 권장되지는 않지만 필요할 때가 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pciehp_probe() 또는 다른 핫플러그 드라이버의 probe
 *   -> [이 파일] acpi_get_hp_hw_control_from_firmware()
 *      -> pci-acpi.c 의 _OSC 협상 (pci_acpi_osc_support 등)
 *         -> ACPI 코어의 메서드 평가
 *      -> 성공하면 드라이버가 계속 진행, 실패하면 -ENODEV
 *
 * 실행 컨텍스트: probe 시점의 프로세스 컨텍스트. ACPI 메서드 평가는
 * 잠들 수 있으므로 인터럽트 컨텍스트에서 부를 수 없다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: hotplug/ 아래의 각 드라이버.
 * 아래쪽: pci-acpi.c 의 _OSC 처리, ACPI 코어.
 * 공유 상태: 협상 결과는 host bridge 의 ACPI 컨텍스트에 남아,
 *   나중에 다른 코드가 "핫플러그를 OS 가 소유하는가" 를 물을 때 쓰인다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(전수 확인).
 *
 * 하지만 NVMe 핫스왑이 되느냐 안 되느냐가 여기서 갈린다. U.2 백플레인이
 * 있는 서버에서 드라이브를 교체해도 커널이 반응하지 않으면, 먼저 확인할
 * 곳이 이 협상의 결과다. dmesg 에 "Requesting control of ... via _OSC"
 * 와 그 결과가 찍히므로 그것으로 판단할 수 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * acpi_get_hp_hw_control_from_firmware() : 핵심. 펌웨어와 소유권을 협상하고
 *                          결과를 반환한다. 실패 시 드라이버는 물러나야 한다.
 * acpi_pci_check_ejectable() : 그 슬롯이 ACPI 로 뽑을 수 있는지 확인.
 *                          _EJ0 메서드의 존재 여부로 판단한다.
 * acpi_pci_detect_ejectable() : 버스 아래에 뽑을 수 있는 슬롯이 있는지.
 * check_hotplug() / decode_type0_hpx_record() 계열 : _HPX/_HPP 로 펌웨어가
 *                          지정한 PCI 설정값을 읽는 보조 함수들.
 * debug_acpi 모듈 파라미터 : 이 협상 과정을 자세히 찍게 한다.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/pci_hotplug.h>
#include <linux/acpi.h>
#include <linux/pci-acpi.h>
#include <linux/slab.h>

#define MY_NAME	"acpi_pcihp"

#define dbg(fmt, arg...) do { if (debug_acpi) printk(KERN_DEBUG "%s: %s: " fmt, MY_NAME, __func__, ## arg); } while (0)
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg)

#define	METHOD_NAME__SUN	"_SUN"
#define	METHOD_NAME_OSHP	"OSHP"

static bool debug_acpi;

/* acpi_run_oshp - get control of hotplug from the firmware
 *
 * @handle - the handle of the hotplug controller.
 */
/* [한국어]
 * acpi_run_oshp - 지정한 ACPI 노드의 OSHP 메서드를 실행한다
 *
 * @handle: OSHP 를 찾을 ACPI 노드의 핸들.
 * @return: acpi_status. 성공이면 그 노드에서 제어권을 넘겨받았다는 뜻이다.
 *       AE_NOT_FOUND 는 "그 노드에 OSHP 가 없다"는 흔한 경우이고,
 *       그 밖의 실패만 진짜 오류다.
 *
 * OSHP(Operating System Hot Plug)는 _OSC 이전 세대의 제어권 이양 메서드다.
 * 펌웨어가 이 메서드를 제공하면 OS 가 그것을 실행함으로써 SHPC 하드웨어의
 * 소유권을 넘겨받고, 그때 펌웨어는 예컨대 인터럽트를 자기 대신 OS 로
 * 향하게 바꾼다.
 *
 * 로그 처리가 세 갈래로 나뉘는 것이 이 함수의 대부분이다 — 진짜 오류만
 * KERN_ERR 로 남기고, 메서드 부재와 성공은 디버그 로그로만 남긴다.
 * 부재가 흔한 이유는 호출자가 ACPI 계층을 거슬러 올라가며 반복 시도하기
 * 때문이다.
 *
 * [상류 코드 관찰] acpi_get_name() 의 반환값을 검사하지 않는다. 실패하면
 * string.pointer 가 NULL 인 채로 아래 %s 서식에 넘어간다.
 *
 * 실행 컨텍스트: 드라이버 probe 경로, 프로세스 컨텍스트.
 * ACPI 메서드 평가는 인터프리터를 돌리므로 잠들 수 있다.
 *
 * 에러 경로: 자체 오류를 만들지 않고 acpi_status 를 그대로 전달한다.
 * 이름 버퍼는 어느 경로로든 반드시 해제한다.
 *
 * 호출 체인:
 *   acpi_get_hp_hw_control_from_firmware() → [이 함수]
 *     → acpi_get_name() → acpi_evaluate_object(OSHP) → kfree()
 */
static acpi_status acpi_run_oshp(acpi_handle handle)
{
	/* [한국어] ACPI 메서드 평가 결과. acpi_status 는 errno 와 별개의 값 체계라
	 * ACPI_SUCCESS/ACPI_FAILURE 매크로로 판정해야 한다. */
	acpi_status		status;
	/* [한국어] ACPI 객체의 전체 경로 이름을 받을 버퍼. ACPI_ALLOCATE_BUFFER 를 넣으면
	 * ACPI 코어가 필요한 크기만큼 할당해 주고, 호출자가 kfree 로 해제한다. */
	struct acpi_buffer	string = { ACPI_ALLOCATE_BUFFER, NULL };

	/* [한국어] 핸들의 전체 네임스페이스 경로를 얻는다. 로그에 어느 ACPI 노드에서
	 * 일어난 일인지 찍기 위한 것이며, 반환값을 검사하지 않아 실패하면
	 * string.pointer 가 NULL 인 채로 아래 %s 에 넘어간다. */
	acpi_get_name(handle, ACPI_FULL_PATHNAME, &string);

	/* run OSHP */
	status = acpi_evaluate_object(handle, METHOD_NAME_OSHP, NULL, NULL);
	/* [한국어] 메서드 평가가 실패한 경우. */
	if (ACPI_FAILURE(status))
		/* [한국어] AE_NOT_FOUND 는 "이 노드에 OSHP 메서드가 없다"는 뜻이라 정상적인 경우다.
		 * 그 밖의 실패만 진짜 오류로 취급한다. */
		if (status != AE_NOT_FOUND)
			/* [한국어] 실제 오류를 KERN_ERR 로 남긴다. */
			printk(KERN_ERR "%s:%s OSHP fails=0x%x\n",
			       __func__, (char *)string.pointer, status);
		else
			/* [한국어] 메서드 부재는 디버그 로그로만 남긴다 — 상위 계층이 부모 노드로 올라가며
			 * 계속 시도하기 때문에 흔한 상황이다. */
			dbg("%s:%s OSHP not found\n",
			    __func__, (char *)string.pointer);
	else
		/* [한국어] 성공도 디버그 로그로만 남긴다. */
		pr_debug("%s:%s OSHP passes\n", __func__,
			(char *)string.pointer);

	/* [한국어] ACPI 코어가 할당한 이름 버퍼를 해제한다. */
	kfree(string.pointer);
	/* [한국어] acpi_status 를 그대로 돌려준다 — 호출자가 ACPI_SUCCESS 로 판정한다. */
	return status;
}

/**
 * acpi_get_hp_hw_control_from_firmware
 * @pdev: the pci_dev of the bridge that has a hotplug controller
 *
 * Attempt to take hotplug control from firmware.
 */
/* [한국어]
 * acpi_get_hp_hw_control_from_firmware - 펌웨어에서 SHPC 제어권을 넘겨받는다
 *
 * @pdev: 핫플러그 컨트롤러를 가진 브리지의 pci_dev.
 * @return: 0 = 제어권을 갖고 있거나 넘겨받음(하드웨어를 만져도 된다).
 *       -ENODEV = 펌웨어가 계속 쥐고 있음(만지면 안 된다).
 *
 * 왜 필요한가: SHPC 하드웨어를 펌웨어와 OS 가 동시에 만지면 서로의 상태를
 * 망가뜨린다. 그래서 누가 주인인지 먼저 협상해야 하고, 그 협상 방식이
 * 플랫폼 세대에 따라 셋으로 갈린다. 이 함수가 그 셋을 순서대로 다룬다.
 *
 *   1) ACPI 자체가 없는 플랫폼 — 넘겨받을 상대가 없으므로 그냥 성공이다.
 *      위 영어 주석이 "ACPI 지원이 커널에 컴파일되어 있어도 하드웨어가
 *      ACPI 를 쓰지 않을 수 있다"고 밝힌다.
 *   2) _OSC 가 있는 현대 플랫폼 — 협상은 호스트 브리지 열거 시점에 이미
 *      끝났고 결과가 host->native_shpc_hotplug 에 담겨 있다. 그 값을 읽어
 *      답하기만 하면 된다.
 *   3) _OSC 가 없는 옛 플랫폼 — OSHP 메서드를 실행해 제어권을 넘겨받아야 한다.
 *      장치 자신의 ACPI 핸들에서 시작해 실패하면 부모로 올라가며 반복하고,
 *      루트 브리지까지 가도 성공하지 못하면 포기한다.
 *
 * 위 영어 주석이 3)의 상향 탐색에 대해 스스로 경고한다 — PCI Firmware Spec
 * r3.2 sec 4.8 은 그런 탐색을 보증하지 않으므로 "이 루프들은 미심쩍다".
 *
 * [상류 코드 관찰, 수정하지 않음] while 루프가 매 반복마다 acpi_get_name() 으로
 * 새 버퍼를 할당하지만 이전 것을 해제하지 않는다. 여러 단계를 올라가면
 * 그만큼 버퍼가 새어 나가고, 마지막 하나만 함수 끝에서 kfree 된다.
 *
 * 실행 컨텍스트: shpchp 등의 probe 경로, 프로세스 컨텍스트.
 * ACPI 메서드 평가가 잠들 수 있다.
 *
 * 에러 경로: 세 경로 모두 명확한 반환값을 준다. 호출자는 -ENODEV 를 보면
 * 하드웨어를 만지지 않고 물러난다.
 *
 * 호출 체인:
 *   shpchp_core.c 의 shpc_probe() → [이 함수]
 *     → pci_find_host_bridge() → acpi_pci_find_root()
 *     → (옛 경로) acpi_pci_get_bridge_handle() → acpi_run_oshp()
 *     → acpi_get_parent()
 */
int acpi_get_hp_hw_control_from_firmware(struct pci_dev *pdev)
{
	/* [한국어] 이 장치가 속한 PCI 호스트 브리지. */
	const struct pci_host_bridge *host;
	/* [한국어] 그에 대응하는 ACPI 루트 브리지 객체. */
	const struct acpi_pci_root *root;
	/* [한국어] ACPI 호출 결과. */
	acpi_status status;
	/* [한국어] chandle: 부모를 찾을 때 쓰는 임시 핸들. handle: 현재 검사 중인 ACPI 노드. */
	acpi_handle chandle, handle;
	/* [한국어] 로그용 이름 버퍼. */
	struct acpi_buffer string = { ACPI_ALLOCATE_BUFFER, NULL };

	/*
	 * If there's no ACPI host bridge (i.e., ACPI support is compiled
	 * into the kernel but the hardware platform doesn't support ACPI),
	 * there's nothing to do here.
	 */
	/* [한국어] PCI 버스를 거슬러 올라가 호스트 브리지를 찾는다. */
	host = pci_find_host_bridge(pdev->bus);
	/* [한국어] 그 브리지의 ACPI 핸들로 ACPI 루트 객체를 조회한다. */
	root = acpi_pci_find_root(ACPI_HANDLE(&host->dev));
	/* [한국어] 위 영어 주석대로, ACPI 지원이 커널에 컴파일되어 있어도 플랫폼이
	 * ACPI 를 쓰지 않으면 루트가 없다. */
	if (!root)
		/* [한국어] 그 경우 펌웨어에서 가져올 제어권 자체가 없으므로 성공으로 답한다. */
		return 0;

	/*
	 * If _OSC exists, it determines whether we're allowed to manage
	 * the SHPC.  We executed it while enumerating the host bridge.
	 */
	/* [한국어] _OSC 가 존재하면 그것이 SHPC 제어권을 결정한다(위 영어 주석).
	 * _OSC 협상은 호스트 브리지 열거 시점에 이미 끝났고, 그 결과가
	 * host->native_shpc_hotplug 에 담겨 있다. */
	if (root->osc_support_set) {
		/* [한국어] 협상 결과 커널이 SHPC 를 쥐었다면, */
		if (host->native_shpc_hotplug)
			/* [한국어] 곧바로 성공이다 — OSHP 를 실행할 필요가 없다. */
			return 0;
		/* [한국어] 펌웨어가 계속 쥐겠다고 했다면 이 드라이버가 하드웨어를 만지면 안 된다. */
		return -ENODEV;
	}

	/*
	 * In the absence of _OSC, we're always allowed to manage the SHPC.
	 * However, if an OSHP method is present, we must execute it so the
	 * firmware can transfer control to the OS, e.g., direct interrupts
	 * to the OS instead of to the firmware.
	 *
	 * N.B. The PCI Firmware Spec (r3.2, sec 4.8) does not endorse
	 * searching up the ACPI hierarchy, so the loops below are suspect.
	 */
	/* [한국어] 위 영어 주석이 설명하는 경로 — _OSC 가 없는 옛 펌웨어에서는 OSHP 메서드를
	 * 실행해 제어권을 넘겨받아야 한다. 먼저 이 장치 자신의 ACPI 핸들을 얻는다. */
	handle = ACPI_HANDLE(&pdev->dev);
	/* [한국어] 장치가 ACPI 네임스페이스에 아예 없는 경우(옆의 상류 주석). */
	if (!handle) {
		/*
		 * This hotplug controller was not listed in the ACPI name
		 * space at all. Try to get ACPI handle of parent PCI bus.
		 */
		/* [한국어] 부모 버스를 거슬러 올라갈 순회 변수. */
		struct pci_bus *pbus;
		/* [한국어] 버스를 하나씩 올라가며, */
		for (pbus = pdev->bus; pbus; pbus = pbus->parent) {
			/* [한국어] 브리지의 ACPI 핸들을 찾는다. */
			handle = acpi_pci_get_bridge_handle(pbus);
			/* [한국어] 찾았으면, */
			if (handle)
				/* [한국어] 거기서 멈춘다. */
				break;
		}
	}

	/* [한국어] 핸들을 부모 방향으로 올라가며 OSHP 를 시도한다.
	 * 위 영어 주석이 스스로 인정하듯, PCI Firmware Spec r3.2 sec 4.8 은
	 * 이런 상향 탐색을 보증하지 않는다 — 상류가 "이 루프는 미심쩍다"고
	 * 명시해 둔 부분이다. */
	while (handle) {
		/* [한국어] 로그에 찍을 노드 경로를 얻는다.
		 * [상류 코드 관찰] 루프를 돌 때마다 새 버퍼를 할당하지만 이전 것을
		 * 해제하지 않는다. 여러 단계를 올라가면 그만큼 버퍼가 새어 나가고,
		 * 마지막 하나만 아래에서 kfree 된다. */
		acpi_get_name(handle, ACPI_FULL_PATHNAME, &string);
		/* [한국어] 어느 노드에 제어권을 요청하는지 알린다. */
		pci_info(pdev, "Requesting control of SHPC hotplug via OSHP (%s)\n",
			 (char *)string.pointer);
		/* [한국어] 그 노드의 OSHP 메서드를 실행한다. */
		status = acpi_run_oshp(handle);
		/* [한국어] 성공하면, */
		if (ACPI_SUCCESS(status))
			/* [한국어] 제어권을 얻었으므로 성공 구간으로. */
			goto got_one;
		/* [한국어] 루트 브리지까지 올라왔으면 더 갈 곳이 없다. */
		if (acpi_is_root_bridge(handle))
			/* [한국어] 탐색 종료. */
			break;
		/* [한국어] 부모를 찾기 위해 현재 핸들을 임시 변수에 옮긴다. */
		chandle = handle;
		/* [한국어] ACPI 부모 노드를 얻는다. */
		status = acpi_get_parent(chandle, &handle);
		/* [한국어] 실패하면 더 올라갈 수 없다. */
		if (ACPI_FAILURE(status))
			/* [한국어] 탐색 종료. */
			break;
	}

	/* [한국어] 모든 후보에서 실패했다. */
	pci_info(pdev, "Cannot get control of SHPC hotplug\n");
	/* [한국어] 마지막으로 할당된 이름 버퍼를 해제한다. */
	kfree(string.pointer);
	/* [한국어] 제어권을 못 얻었으므로 -ENODEV. 호출자(shpchp 등)는 이 값을 보고
	 * 하드웨어를 만지지 않고 물러난다. */
	return -ENODEV;
got_one:
	pci_info(pdev, "Gained control of SHPC hotplug (%s)\n",
		 (char *)string.pointer);
	kfree(string.pointer);
	return 0;
}
EXPORT_SYMBOL(acpi_get_hp_hw_control_from_firmware);

/* [한국어]
 * pcihp_is_ejectable - ACPI 노드가 배출 가능한 PCI 슬롯인지 판정한다
 *
 * @handle: 검사할 ACPI 노드의 핸들.
 * @return: 1 = 배출 가능한 PCI 슬롯, 0 = 아니다.
 *
 * 세 가지 ACPI 메서드로 판정한다.
 *   - _ADR 이 없으면 PCI 주소를 갖지 않는 노드이므로 애초에 PCI 슬롯이 아니다.
 *   - _EJ0 이 있으면 소프트웨어로 배출을 지시할 수 있다는 뜻이라 그것만으로 충분하다.
 *   - _EJ0 이 없어도 _RMV(Removable)가 0 이 아니면 물리적으로 뽑을 수 있다는
 *     표시이므로 핫플러그 대상이다.
 *
 * _EJ0 과 _RMV 를 둘 다 보는 이유가 여기 있다 — 전자는 "OS 가 배출을 명령할 수
 * 있다", 후자는 "사람이 뽑을 수 있다" 로 의미가 다르고, 둘 중 하나만 있어도
 * 핫플러그 슬롯으로 다뤄야 한다.
 *
 * 실행 컨텍스트: ACPI 네임스페이스 순회 경로, 프로세스 컨텍스트.
 * 메서드 평가가 잠들 수 있다.
 *
 * 에러 경로: 없다. 판정에 실패하면 "아니다" 쪽으로 떨어진다.
 *
 * 호출 체인:
 *   acpi_pci_check_ejectable() / check_hotplug() → [이 함수]
 *     → acpi_has_method() ×2 → acpi_evaluate_integer(_RMV)
 */
static int pcihp_is_ejectable(acpi_handle handle)
{
	/* [한국어] ACPI 평가 결과. */
	acpi_status status;
	/* [한국어] _RMV 메서드가 돌려줄 값. */
	unsigned long long removable;
	/* [한국어] _ADR 이 없으면 PCI 주소를 갖지 않는 노드이므로 PCI 슬롯이 아니다. */
	if (!acpi_has_method(handle, "_ADR"))
		/* [한국어] 슬롯이 아니라고 답한다. */
		return 0;
	/* [한국어] _EJ0 은 "이 장치를 배출(eject)할 수 있다"는 뜻의 표준 메서드다. */
	if (acpi_has_method(handle, "_EJ0"))
		/* [한국어] 있으면 그것만으로 배출 가능이다. */
		return 1;
	/* [한국어] _EJ0 이 없으면 _RMV(Removable) 값을 확인한다. _EJ0 이 소프트웨어 배출을
	 * 뜻하는 반면 _RMV 는 "물리적으로 뽑을 수 있다"는 표시라, 둘 중 하나만
	 * 있어도 핫플러그 대상이 된다. */
	status = acpi_evaluate_integer(handle, "_RMV", NULL, &removable);
	/* [한국어] 평가에 성공했고 값이 0 이 아니면, */
	if (ACPI_SUCCESS(status) && removable)
		/* [한국어] 배출 가능이다. */
		return 1;
	return 0;
}

/**
 * acpi_pci_check_ejectable - check if handle is ejectable ACPI PCI slot
 * @pbus: the PCI bus of the PCI slot corresponding to 'handle'
 * @handle: ACPI handle to check
 *
 * Return 1 if handle is ejectable PCI slot, 0 otherwise.
 */
/* [한국어]
 * acpi_pci_check_ejectable - 그 ACPI 노드가 이 버스에 속한 배출 가능 슬롯인지 확인한다
 *
 * @pbus: 검사 대상이 속해야 할 PCI 버스.
 * @handle: 검사할 ACPI 노드.
 * @return: 1 = 이 버스의 배출 가능한 PCI 슬롯, 0 = 아니다.
 *
 * pcihp_is_ejectable() 에 위치 검사를 더한 것이다. 배출 가능하기만 해서는
 * 부족하고, 그 노드가 실제로 이 버스에 매달려 있어야 한다.
 *
 * 위치 검사 방법이 흥미롭다 — ACPI 노드의 부모가 이 PCI 버스의 브리지
 * 핸들과 같은지 본다. ACPI 네임스페이스의 부모-자식 관계와 PCI 위상이
 * 일치해야 한다는 전제이며, 펌웨어가 그 대응을 올바르게 서술했다면 성립한다.
 *
 * 실행 컨텍스트: acpiphp 의 슬롯 열거 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 세 검사 모두 실패 시 0 을 돌려준다 — 판정할 수 없으면
 * 슬롯이 아니라고 보는 보수적 태도다.
 *
 * 호출 체인:
 *   acpiphp_glue.c 의 슬롯 등록 경로 → [이 함수]
 *     → acpi_pci_get_bridge_handle() → acpi_get_parent()
 *     → pcihp_is_ejectable()
 */
int acpi_pci_check_ejectable(struct pci_bus *pbus, acpi_handle handle)
{
	/* [한국어] bridge_handle: 버스의 브리지 ACPI 핸들. parent_handle: 검사 대상의 부모 핸들. */
	acpi_handle bridge_handle, parent_handle;

	/* [한국어] 이 PCI 버스에 대응하는 ACPI 브리지 핸들을 얻는다. */
	bridge_handle = acpi_pci_get_bridge_handle(pbus);
	/* [한국어] 없으면 ACPI 로 관리되는 버스가 아니다. */
	if (!bridge_handle)
		/* [한국어] 슬롯이 아니라고 답한다. */
		return 0;
	/* [한국어] 검사 대상 노드의 부모를 얻는다. */
	if ((ACPI_FAILURE(acpi_get_parent(handle, &parent_handle))))
		/* [한국어] 실패하면 판정할 수 없다. */
		return 0;
	/* [한국어] 부모가 이 버스의 브리지가 아니면, 그 노드는 이 버스에 속한 슬롯이 아니다.
	 * ACPI 네임스페이스의 부모-자식 관계와 PCI 위상이 일치해야 한다는 검사다. */
	if (bridge_handle != parent_handle)
		/* [한국어] 아니라고 답한다. */
		return 0;
	/* [한국어] 위치가 맞으면 배출 가능 여부만 확인하면 된다. */
	return pcihp_is_ejectable(handle);
}
EXPORT_SYMBOL_GPL(acpi_pci_check_ejectable);

static acpi_status
/* [한국어]
 * check_hotplug - acpi_walk_namespace 가 노드마다 부르는 콜백
 *
 * @handle: 지금 방문 중인 ACPI 노드.
 * @lvl: 순회 깊이. 쓰지 않는다.
 * @context: 호출자가 넘긴 문맥. 실제로는 "찾았다" 플래그를 가리키는 int 포인터다.
 * @rv: 반환값 자리. 쓰지 않는다.
 * @return: AE_OK = 계속 순회. AE_CTRL_TERMINATE = 즉시 멈춤.
 *
 * 배출 가능한 슬롯을 하나라도 찾으면 플래그를 세우고 순회를 멈춘다.
 * "하나라도 있는가" 를 묻는 것이므로 나머지를 훑을 이유가 없고,
 * AE_CTRL_TERMINATE 가 그 조기 종료를 ACPI 코어에 알리는 값이다.
 *
 * 결과를 반환값이 아니라 context 포인터로 전달하는 것은 콜백 시그니처가
 * acpi_status 로 고정되어 있기 때문이다 — 순회 제어와 결과 전달을
 * 분리해야 해서 나온 구조다.
 *
 * 실행 컨텍스트: acpi_walk_namespace 안, 프로세스 컨텍스트.
 * pcihp_is_ejectable() 이 ACPI 메서드를 평가하므로 잠들 수 있다.
 *
 * 에러 경로: 없다. 판정에 실패하면 "아니다" 쪽으로 떨어져 순회가 계속된다.
 *
 * 호출 체인:
 *   acpi_pci_detect_ejectable() → acpi_walk_namespace() → [이 함수]
 *     → pcihp_is_ejectable()
 */
check_hotplug(acpi_handle handle, u32 lvl, void *context, void **rv)
{
	/* [한국어] acpi_walk_namespace 가 넘겨 준 문맥을 원래 타입으로 되돌린다.
	 * 여기서는 "찾았다" 플래그를 가리키는 포인터다. */
	int *found = (int *)context;
	/* [한국어] 이 노드가 배출 가능한 PCI 슬롯이면, */
	if (pcihp_is_ejectable(handle)) {
		/* [한국어] 플래그를 세우고, */
		*found = 1;
		/* [한국어] AE_CTRL_TERMINATE 로 순회를 즉시 멈춘다 — 하나만 찾으면 되므로
		 * 나머지를 훑을 이유가 없다. */
		return AE_CTRL_TERMINATE;
	}
	/* [한국어] 아니면 계속 순회하라고 답한다. */
	return AE_OK;
}

/**
 * acpi_pci_detect_ejectable - check if the PCI bus has ejectable slots
 * @handle: handle of the PCI bus to scan
 *
 * Returns 1 if the PCI bus has ACPI based ejectable slots, 0 otherwise.
 */
/* [한국어]
 * acpi_pci_detect_ejectable - 그 PCI 버스에 배출 가능한 슬롯이 하나라도 있는지 본다
 *
 * @handle: 훑을 PCI 버스의 ACPI 핸들.
 * @return: 1 = 배출 가능한 슬롯이 있다, 0 = 없다(핸들이 NULL 인 경우 포함).
 *
 * acpiphp 가 "이 버스에 핫플러그 슬롯이 있는가" 를 판단할 때 쓴다. 있으면
 * 슬롯 객체를 만들고 이벤트 처리를 준비하며, 없으면 그 버스를 건너뛴다.
 *
 * acpi_walk_namespace 의 깊이를 1 로 제한하는 것이 핵심이다. 버스에 직접
 * 매달린 노드만 슬롯 후보이고, 그보다 깊은 곳은 슬롯에 꽂힌 장치의 내부
 * 구조라 검사 대상이 아니다.
 *
 * 결과를 지역 변수 found 에 담고 그 주소를 콜백 문맥으로 넘기는 구조다.
 * 콜백이 acpi_status 만 돌려줄 수 있어 결과 전달 통로가 따로 필요했다.
 *
 * 실행 컨텍스트: acpiphp 의 버스 등록 경로, 프로세스 컨텍스트.
 * 네임스페이스 순회와 메서드 평가가 잠들 수 있다.
 *
 * 에러 경로: 핸들이 NULL 이면 순회 없이 0 을 돌려준다.
 * 순회 자체의 실패는 검사하지 않는데, 실패하면 found 가 0 으로 남아
 * "없다"와 같은 결과가 되기 때문이다.
 *
 * 호출 체인:
 *   acpiphp_glue.c 의 버스 등록 경로 → [이 함수]
 *     → acpi_walk_namespace(ACPI_TYPE_DEVICE, 깊이 1, check_hotplug)
 */
int acpi_pci_detect_ejectable(acpi_handle handle)
{
	/* [한국어] 찾았는지 여부. 콜백이 포인터로 갱신한다. */
	int found = 0;

	/* [한국어] 핸들이 없으면 훑을 네임스페이스도 없다. */
	if (!handle)
		/* [한국어] 0(없음)을 그대로 돌려준다. */
		return found;

	/* [한국어] 이 노드 아래를 깊이 1 까지 훑으며 DEVICE 타입 노드마다 콜백을 부른다.
	 * 깊이를 1 로 제한하는 것은 버스에 직접 매달린 슬롯만 보면 되기 때문이다.
	 * &found 를 문맥으로 넘겨 콜백이 결과를 남길 수 있게 한다. */
	acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,
			    check_hotplug, NULL, (void *)&found, NULL);
	/* [한국어] 콜백이 남긴 결과를 돌려준다. */
	return found;
}
EXPORT_SYMBOL_GPL(acpi_pci_detect_ejectable);

/* [한국어] 디버그 플래그를 모듈 파라미터로 노출한다. 권한 0644 라 런타임에
 * sysfs 로 켜고 끌 수 있다. */
module_param(debug_acpi, bool, 0644);
MODULE_PARM_DESC(debug_acpi, "Debugging mode for ACPI enabled or not");
