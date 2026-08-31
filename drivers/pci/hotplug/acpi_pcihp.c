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

#include <linux/module.h>          /* PCI/NVMe: 커널 모듈 인프라; NVMe 호스트 드라이버도 pci.c 모듈에서 동일 헤더 사용 */
#include <linux/moduleparam.h>     /* PCI/NVMe: debug_acpi 모듈 파라미터 등록용; NVMe 호스트의 debug 모듈 파라미터와 유사 */
#include <linux/kernel.h>          /* PCI/NVMe: 커널 기본 매크로/printk; NVMe 호스트의 pr_debug/err 등 동일 인프라 */
#include <linux/types.h>           /* PCI/NVMe: u32, bool 등 기초 타입; NVMe BAR/CSR 접근 시 동일 타입 사용 */
#include <linux/pci.h>             /* PCI/NVMe: PCI 핵심 구조체(struct pci_dev, pci_bus); NVMe 장치 열거/바인딩의 기반 */
#include <linux/pci_hotplug.h>     /* PCI/NVMe: 핫플러그 컨트롤러 인터페이스; NVMe SSD 물리적 교체/제거 시 사용 */
#include <linux/acpi.h>            /* PCI/NVMe: ACPI 평가/네임스페이스 API; NVMe 전원/핫플러그 정책과 연결 */
#include <linux/pci-acpi.h>        /* PCI/NVMe: PCI-ACPI 브리지 매핑; NVMe 장치가 연결된 PCI 루트/브리지 탐색에 사용 */
#include <linux/slab.h>            /* PCI/NVMe: ACPI_ALLOCATE_BUFFER 결과 해제(kfree); NVMe DMA 풀과 무관한 일반 메모리 */

#define MY_NAME	"acpi_pcihp"        /* NVMe: 이 소스의 로그 접두사; NVMe pci.c의 "nvme" 접두사와 대응 */

#define dbg(fmt, arg...) do { if (debug_acpi) printk(KERN_DEBUG "%s: %s: " fmt, MY_NAME, __func__, ## arg); } while (0)  /* PCI/NVMe: ACPI 핫플러그 디버그 로그; NVMe 호스트의 dev_dbg()와 동일 목적의 커널 로깅 */
#define err(format, arg...) printk(KERN_ERR "%s: " format, MY_NAME, ## arg)    /* PCI/NVMe: ACPI 핫플러그 에러 로그; NVMe probe/remove 실패 시 출력과 동일 수준 */
#define info(format, arg...) printk(KERN_INFO "%s: " format, MY_NAME, ## arg)  /* PCI/NVMe: ACPI 핫플러그 정보 로그; NVMe 장치 검출 메시지와 유사 */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format, MY_NAME, ## arg) /* PCI/NVMe: ACPI 핫플러그 경고; NVMe 경고 메시지 처리와 동일 */

#define	METHOD_NAME__SUN	"_SUN"   /* PCI/NVMe: ACPI Slot User Number 메서드; NVMe가 장착된 PCI 슬롯 번호 식별에 사용 */
#define	METHOD_NAME_OSHP	"OSHP"   /* PCI/NVMe: OS Hotplug 처리권한 이양 메서드; NVMe 핫플러그 이벤트를 OS가 직접 처리하기 위해 실행 */

static bool debug_acpi;              /* PCI/NVMe: dbg() 출력 토글; NVMe 호스트의 debug 파라미터와 동일한 디버깅 용도 */

/* acpi_run_oshp - get control of hotplug from the firmware
 *
 * @handle - the handle of the hotplug controller.
 */
static acpi_status acpi_run_oshp(acpi_handle handle)  /* PCI/NVMe: OSHP 메서드 실행; NVMe SSD 장착 슬롯의 핫플러그 제어권을 펌웨어에서 OS로 이양 */
{
	acpi_status		status;        /* PCI/NVMe: ACPI 평가 결과; OSHP 성공/실패 판단, NVMe 핫플러그 준비 여부에 영향 */
	struct acpi_buffer	string = { ACPI_ALLOCATE_BUFFER, NULL };  /* PCI/NVMe: ACPI 경로명 버퍼; ACPI_ALLOCATE_BUFFER로 동적 할당, NVMe 슬롯 경로 디버깅용 */

	acpi_get_name(handle, ACPI_FULL_PATHNAME, &string);  /* PCI/NVMe: 핫플러그 컨트롤러 ACPI 전체 경로 획득; NVMe 장치 경로 추적 시 참고 */

	/* run OSHP */
	status = acpi_evaluate_object(handle, METHOD_NAME_OSHP, NULL, NULL);  /* PCI/NVMe: OSHP 메서드 평가; 성공 시 OS가 핫플러그 인터럽트/이벤트 직접 수신 → NVMe 제거/삽입 감지 가능 */
	if (ACPI_FAILURE(status))               /* PCI/NVMe: OSHP 실패 시 분기; NVMe 핫플러그 경로가 펌웨어/ACPI 의존적임을 의미 */
		if (status != AE_NOT_FOUND)     /* PCI/NVMe: OSHP 존재하나 실행 실패; NVMe 슬롯의 핫플러그 제어권 획득 불가, 드라이버가 SMI/SCL 이벤트 처리 불가할 수 있음 */
			printk(KERN_ERR "%s:%s OSHP fails=0x%x\n",  /* PCI/NVMe: 에러 로그; NVMe 호스트 입장에서는 acpi_pcihp 실패로 핫플러그 안정성 저하 */
			       __func__, (char *)string.pointer, status);
		else                            /* PCI/NVMe: OSHP 메서드 자체가 없음; NVMe 장치가 장착된 브리지가 OSHP 미지원 */
			dbg("%s:%s OSHP not found\n",  /* PCI/NVMe: 디버그 로그; NVMe 핫플러그가 OSHP 없이 동작(브리지 종속) */
			    __func__, (char *)string.pointer);
	else                                    /* PCI/NVMe: OSHP 성공; OS가 NVMe SSD 핫플러그를 직접 제어, 인터럽트 라우팅 OS 전환 */
		pr_debug("%s:%s OSHP passes\n", __func__,
			(char *)string.pointer);

	kfree(string.pointer);                  /* PCI/NVMe: ACPI_ALLOCATE_BUFFER로 할당된 경로 문자열 해제; NVMe 관련 아닌 일반 메모리 정리 */
	return status;                          /* PCI/NVMe: OSHP 평가 결과 반환; 호출자가 NVMe 핫플러그 제어권 획득 여부 판단 */
}

/**
 * acpi_get_hp_hw_control_from_firmware
 * @pdev: the pci_dev of the bridge that has a hotplug controller
 *
 * Attempt to take hotplug control from firmware.
 */
int acpi_get_hp_hw_control_from_firmware(struct pci_dev *pdev)  /* PCI/NVMe: PCI-PCI 브리지의 핫플러그 제어권 획득; NVMe 장치가 연결된 다운스트림 브리지에서 호출되어 핫플러그 제어 준비 */
{
	const struct pci_host_bridge *host;     /* PCI/NVMe: NVMe가 연결된 PCIe 계층의 루트 호스트 브리지; _OSC/핫플러그 정책 수집처 */
	const struct acpi_pci_root *root;       /* PCI/NVMe: ACPI PCI 루트 브리지; _OSC 통해 OS가 핫플러그/ASPM 등 관리 권한 보유 여부 확인 */
	acpi_status status;                     /* PCI/NVMe: ACPI API 반환값; OSHP/_OSC 평가 상태 저장 */
	acpi_handle chandle, handle;            /* PCI/NVMe: ACPI 핸들; NVMe 슬롯이 속한 브리지/루트 탐색용 */
	struct acpi_buffer string = { ACPI_ALLOCATE_BUFFER, NULL };  /* PCI/NVMe: ACPI 경로명 버퍼; OSHP 요청/성공 메시지에 경로 출력 */

	/*
	 * If there's no ACPI host bridge (i.e., ACPI support is compiled
	 * into the kernel but the hardware platform doesn't support ACPI),
	 * there's nothing to do here.
	 */
	host = pci_find_host_bridge(pdev->bus); /* PCI/NVMe: pdev가 속한 버스의 호스트 브리지 검색; NVMe PCIe 계층 루트 식별 */
	root = acpi_pci_find_root(ACPI_HANDLE(&host->dev));  /* PCI/NVMe: 호스트 브리지에 대응하는 ACPI PCI 루트 획득; _OSC 설정 등 NVMe 전원/핫플러그 정책의 출발점 */
	if (!root)                              /* PCI/NVMe: ACPI PCI 루트 없음; non-ACPI 플랫폼이므로 NVMe 핫플러그 제어는 단순 PCI 레지스터 기반으로 진행 */
		return 0;

	/*
	 * If _OSC exists, it determines whether we're allowed to manage
	 * the SHPC.  We executed it while enumerating the host bridge.
	 */
	if (root->osc_support_set) {            /* PCI/NVMe: _OSC가 이미 실행되어 SHPC/핫플러그 관리권이 negotiated 됨; NVMe 호스트는 _OSC 결과에 따라 핫플러그 처리 가능 여부를 존중해야 함 */
		if (host->native_shpc_hotplug)  /* PCI/NVMe: OS가 SHPC 핫플러그를 네이티브로 관리 허용; NVMe SSD 핫플러그 이벤트가 OS에서 직접 처리됨 */
			return 0;
		return -ENODEV;             /* PCI/NVMe: _OSC가 SHPC 제어권을 펌웨어에 남김; NVMe 핫플러그 제어 불가, 드라이버는 firmware-mediated 핫플러그에 의존 */
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
	handle = ACPI_HANDLE(&pdev->dev);       /* PCI/NVMe: 핫플러그 컨트롤러(PCI-PCI 브리지)의 ACPI 핸들; NVMe 장치가 연결된 다운스트림 브리지 */
	if (!handle) {                          /* PCI/NVMe: pdev 자체가 ACPI 네임스페이스에 없음; NVMe 장치가 연결된 상위 브리지 탐색 필요 */
		/*
		 * This hotplug controller was not listed in the ACPI name
		 * space at all. Try to get ACPI handle of parent PCI bus.
		 */
		struct pci_bus *pbus;           /* PCI/NVMe: 상위 PCI 버스 순회 포인터; NVMe가 연결된 슬롯을 감싸는 브리지를 ACPI에서 찾기 위함 */
		for (pbus = pdev->bus; pbus; pbus = pbus->parent) {  /* PCI/NVMe: 버스 계층을 루트 방향으로 순회; NVMe 장치 → 업스트림 브리지 → 루트 브리지까지 ACPI 핸들 탐색 */
			handle = acpi_pci_get_bridge_handle(pbus);  /* PCI/NVMe: 각 버스를 구성하는 브리지의 ACPI 핸드 획득; NVMe 슬롯의 핫플러그 책임 브리지 후보 */
			if (handle)             /* PCI/NVMe: ACPI 핸들 발견; 해당 브리지에서 OSHP 실행 대상 확정 */
				break;
		}
	}

	while (handle) {                        /* PCI/NVMe: OSHP 성공하거나 루트 브리지에 도달할 때까지 상위로 탐색; NVMe 핫플러그 제어권 획득 시도 */
		acpi_get_name(handle, ACPI_FULL_PATHNAME, &string);  /* PCI/NVMe: 현재 후보 브리지의 ACPI 경로 획득; NVMe 슬롯 관련 OSHP 시도 로그 기록 */
		pci_info(pdev, "Requesting control of SHPC hotplug via OSHP (%s)\n",  /* PCI/NVMe: OSHP 요청 정보 로그; NVMe 장치의 핫플러그 제어권을 요청 중임을 알림 */
			 (char *)string.pointer);
		status = acpi_run_oshp(handle); /* PCI/NVMe: 현재 브리지에서 OSHP 실행; 성공 시 NVMe 핫플러그 인터럽트가 OS로 라우팅됨 */
		if (ACPI_SUCCESS(status))       /* PCI/NVMe: OSHP 성공; NVMe SSD의 삽입/제거 이벤트를 OS가 직접 수신 가능 */
			goto got_one;
		if (acpi_is_root_bridge(handle)) /* PCI/NVMe: 루트 브리지까지 실패; NVMe 핫플러그 제어권 획득 불가, 더 이상 상위 없음 */
			break;
		chandle = handle;               /* PCI/NVMe: 현재 핸들 보존; 상위 부모 획득을 위해 사용 */
		status = acpi_get_parent(chandle, &handle);  /* PCI/NVMe: ACPI 계층 상위로 이동; NVMe 장치가 연결된 브리지의 상위에서 OSHP 재시도 */
		if (ACPI_FAILURE(status))       /* PCI/NVMe: 부모 획득 실패; NVMe 핫플러그 OSHP 탐색 종료 */
			break;
	}

	pci_info(pdev, "Cannot get control of SHPC hotplug\n");  /* PCI/NVMe: SHPC 제어권 획득 실패; NVMe SSD 핫플러그 시 OS 직접 처리 불가, firmware 의존 */
	kfree(string.pointer);                  /* PCI/NVMe: ACPI_ALLOCATE_BUFFER 경로 문자열 해제; 메모리 누수 방지 */
	return -ENODEV;                         /* PCI/NVMe: 핫플러그 제어권 획득 실패 반환; NVMe 호스트의 핫플러그 등록/처리 제한 가능 */
got_one:
	pci_info(pdev, "Gained control of SHPC hotplug (%s)\n",  /* PCI/NVMe: OSHP 성공 로그; NVMe 장치 핫플러그 제어권 획득 */
		 (char *)string.pointer);
	kfree(string.pointer);                  /* PCI/NVMe: 경로 버퍼 해제; NVMe 관련 메모리 아님, 일반 정리 */
	return 0;                               /* PCI/NVMe: 제어권 획득 성공; NVMe 핫플러그 이벤트 OS 처리 가능 */
}
EXPORT_SYMBOL(acpi_get_hp_hw_control_from_firmware);  /* PCI/NVMe: pci.c 등 NVMe PCIe 호스트 드라이버가 아닌 PCI 핫플러그 코어에서 호출 가능; NVMe 장치의 다운스트림 브리지 초기화 시 사용 */

static int pcihp_is_ejectable(acpi_handle handle)  /* PCI/NVMe: ACPI 핸들이 제거 가능한 PCI 슬롯인지 판단; NVMe SSD가 장착된 슬롯의 물리적 제거 가능성 확인 */
{
	acpi_status status;                     /* PCI/NVMe: _RMV 평가 결과 저장; NVMe 슬롯 제거 가능성 판단 */
	unsigned long long removable;           /* PCI/NVMe: _RMV 반환값(0/1); 1이면 NVMe 장치가 장착된 슬롯을 제거 가능으로 표시 */
	if (!acpi_has_method(handle, "_ADR"))   /* PCI/NVMe: _ADR 없으면 PCI 장치/슬롯이 아님; NVMe PCIe 장치는 _ADR로 주소 표현 */
		return 0;
	if (acpi_has_method(handle, "_EJ0"))    /* PCI/NVMe: _EJ0 메서드 존재 시 즉시 제거 가능; NVMe SSD 안전 제거(eject) 지원 슬롯 */
		return 1;
	status = acpi_evaluate_integer(handle, "_RMV", NULL, &removable);  /* PCI/NVMe: _RMV 평가로 제거 가능 여부 질의; NVMe SSD를 제거핏은(removable) 슬롯인지 확인 */
	if (ACPI_SUCCESS(status) && removable)  /* PCI/NVMe: _RMV가 1이면 제거 가능; NVMe 장치 제거 시 ACPI eject 시퀀스 진행 가능 */
		return 1;
	return 0;                               /* PCI/NVMe: 제거 불가능한 슬롯; NVMe 장치는 전원 차단 없이 물리적 제거 시 데이터 손실/하드웨어 오류 위험 */
}

/**
 * acpi_pci_check_ejectable - check if handle is ejectable ACPI PCI slot
 * @pbus: the PCI bus of the PCI slot corresponding to 'handle'
 * @handle: ACPI handle to check
 *
 * Return 1 if handle is ejectable PCI slot, 0 otherwise.
 */
int acpi_pci_check_ejectable(struct pci_bus *pbus, acpi_handle handle)  /* PCI/NVMe: 특정 ACPI 핸들이 pbus의 제거 가능 슬롯인지 확인; NVMe 장치 제거 전 ACPI 기반 안전성 검사 */
{
	acpi_handle bridge_handle, parent_handle;  /* PCI/NVMe: 버스 브리지 핸들과 handle의 부모 핸들; NVMe 슬롯이 올바른 브리지 아래 있는지 비교 */

	bridge_handle = acpi_pci_get_bridge_handle(pbus);  /* PCI/NVMe: pbus를 구성하는 PCI-PCI 브리지의 ACPI 핸들; NVMe 장치가 연결된 버스의 업스트림 브리지 */
	if (!bridge_handle)                     /* PCI/NVMe: 브리지 ACPI 핸들 없음; NVMe 장치의 ACPI 핫플러그 맥락을 확인할 수 없음 */
		return 0;
	if ((ACPI_FAILURE(acpi_get_parent(handle, &parent_handle))))  /* PCI/NVMe: handle의 부모 핸들 획득 실패; NVMe 슬롯 계층 파악 불가 */
		return 0;
	if (bridge_handle != parent_handle)     /* PCI/NVMe: handle의 부모가 pbus 브리지가 아님; NVMe 슬롯이 이 버스에 속하지 않음 */
		return 0;
	return pcihp_is_ejectable(handle);      /* PCI/NVMe: 부모 일치 시 _ADR/_EJ0/_RMV 검사; NVMe SSD 제거 가능 여부 최종 판정 */
}
EXPORT_SYMBOL_GPL(acpi_pci_check_ejectable);  /* PCI/NVMe: acpi_pci_hp 커뮤니케이션용; NVMe 장치가 장착된 슬롯의 eject 가능성을 PCI 핫플러그 코어에서 판단 */

static acpi_status
/* PCI/NVMe: acpi_walk_namespace() 콜백; NVMe 장치가 포함될 수 있는 ACPI DEVICE 노드 중 제거 가능한 슬롯 탐색 */
check_hotplug(acpi_handle handle, u32 lvl, void *context, void **rv)  /* PCI/NVMe: ACPI 네임스페이스 순회 콜백; NVMe SSD 슬롯의 핫플러그 가능성을 찾아내는 함수 */
{
	int *found = (int *)context;            /* PCI/NVMe: 발견 플래그 포인터; NVMe 핫플러그 가능 슬롯이 하나라도 있으면 1로 설정 */
	if (pcihp_is_ejectable(handle)) {       /* PCI/NVMe: 현재 ACPI 핸들이 제거 가능 슬롯이면; NVMe SSD 교체 가능 슬롯 발견 */
		*found = 1;                     /* PCI/NVMe: 플래그 설정; 이 버스 아래 NVMe 등 PCI 장치의 핫플러그를 지원함을 표시 */
		return AE_CTRL_TERMINATE;       /* PCI/NVMe: 추가 순회 중단; 하나의 NVMe/PCI 핫플러그 슬롯만 있어도 충분 */
	}
	return AE_OK;                           /* PCI/NVMe: 제거 불가능하면 계속 순회; NVMe 핫플러그 슬롯을 계속 검색 */
}

/**
 * acpi_pci_detect_ejectable - check if the PCI bus has ejectable slots
 * @handle: handle of the PCI bus to scan
 *
 * Returns 1 if the PCI bus has ACPI based ejectable slots, 0 otherwise.
 */
int acpi_pci_detect_ejectable(acpi_handle handle)  /* PCI/NVMe: 주어진 PCI 버스 아래 ACPI 기반 제거 가능 슬롯이 있는지 탐색; NVMe SSD 핫플러그 지원 여부 초기 판별 */
{
	int found = 0;                          /* PCI/NVMe: 핫플러그 슬롯 발견 플래그; 0이면 NVMe 장치의 ACPI eject 미지원 버스 */

	if (!handle)                            /* PCI/NVMe: ACPI 핸들 없음; NVMe 장치가 연결된 버스에 대한 ACPI 정보 부재 */
		return found;

	acpi_walk_namespace(ACPI_TYPE_DEVICE, handle, 1,  /* PCI/NVMe: handle 아래 ACPI DEVICE 객체 1단계 순회; NVMe SSD를 나타내는 PCI 장치/슬롯 후보 검사 */
			    check_hotplug, NULL, (void *)&found, NULL);
	return found;                           /* PCI/NVMe: 1이면 NVMe 등 PCI 장치의 ACPI 핫플러그(eject) 가능; 0이면 불가 */
}
EXPORT_SYMBOL_GPL(acpi_pci_detect_ejectable);  /* PCI/NVMe: PCI 핫플러그 코어에서 버스별 eject 가능성 확인; NVMe 호스트 드라이버는 직접 호출하지 않으나 NVMe SSD의 핫플러그 지원 여부에 영향 */

module_param(debug_acpi, bool, 0644);    /* PCI/NVMe: debug_acpi 모듈 파라미터 노출; NVMe 호스트의 debug 파라미터와 동일하게 런타임 디버깅 제어 */
MODULE_PARM_DESC(debug_acpi, "Debugging mode for ACPI enabled or not");  /* PCI/NVMe: 파라미터 설명; ACPI 핫플러그 디버깅, NVMe 핫플러그 문제 분석 시 활용 */
