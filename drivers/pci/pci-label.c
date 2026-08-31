// SPDX-License-Identifier: GPL-2.0
/*
 * Export the firmware instance and label associated with a PCI device to
 * sysfs
 *
 * Copyright (C) 2010 Dell Inc.
 * by Narendra K <Narendra_K@dell.com>,
 * Jordan Hargrave <Jordan_Hargrave@dell.com>
 *
 * PCI Firmware Specification Revision 3.1 section 4.6.7 (DSM for Naming a
 * PCI or PCI Express Device Under Operating Systems) defines an instance
 * number and string name. This code retrieves them and exports them to sysfs.
 * If the system firmware does not provide the ACPI _DSM (Device Specific
 * Method), then the SMBIOS type 41 instance number and string is exported to
 * sysfs.
 *
 * SMBIOS defines type 41 for onboard pci devices. This code retrieves
 * the instance number and string from the type 41 record and exports
 * it to sysfs.
 *
 * Please see https://linux.dell.com/files/biosdevname/ for more
 * information.
 */

/*
 * [한국어 설명] 펌웨어가 붙인 장치 이름표를 sysfs 에 노출한다 (pci-label.c)
 *
 * === 파일의 역할 ===
 * 위 원문 주석이 목적과 근거를 모두 밝힌다. 요약하면 — 시스템 펌웨어가
 * 온보드 PCI 장치에 붙여 둔 "인스턴스 번호" 와 "이름표" 를 읽어
 * sysfs 에 내보낸다.
 *
 * 왜 필요한가. 서버에 온보드 네트워크 포트가 네 개 있다고 하자.
 * 커널이 보는 것은 "0000:01:00.0~0000:01:00.3" 이지만, 섀시 뒤판에는
 * "NIC1"~"NIC4" 라고 인쇄되어 있다. 그 대응을 아무도 모르면 케이블을
 * 어디 꽂을지 알 수 없다. 펌웨어만 그 대응을 알고 있고, 이 파일이
 * 그것을 커널로 가져온다.
 *
 * 출처가 둘이다.
 *   ACPI _DSM (PCI Firmware Spec r3.1 sec 4.6.7) — 우선.
 *     인스턴스 번호와 문자열을 함께 준다.
 *   SMBIOS type 41 — _DSM 이 없을 때의 대안. 온보드 장치를 기술하는
 *     SMBIOS 항목에서 같은 정보를 얻는다.
 *
 * 결과는 두 개의 sysfs 파일이 된다:
 *   .../label     — 이름표 문자열("NIC1" 등)
 *   .../index     — 인스턴스 번호
 * systemd 의 "예측 가능한 네트워크 인터페이스 이름"(eno1, ens3 같은 것)이
 * 이 값을 읽어 만들어진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: pci-sysfs.c 가 장치의 속성을 만들 때
 *         -> [이 파일] pci_create_firmware_label_files()
 *            -> _DSM 또는 SMBIOS 를 조회해 값이 있으면 속성을 만든다
 *
 * 읽기: cat /sys/bus/pci/devices/.../label
 *         -> [이 파일] label_show() / index_show()
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 평가가 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci-sysfs.c, 그리고 그 값을 읽는 userspace(systemd-udevd).
 * 아래쪽: ACPI 의 _DSM 평가, drivers/firmware/dmi_scan.c 의 SMBIOS 파서.
 * 공유 상태: 없다. 조회할 때마다 다시 읽는다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일과 관련이 없다(전수 확인).
 *
 * 다만 온보드 NVMe 컨트롤러(메인보드에 납땜된 것)에 펌웨어가 이름표를
 * 붙여 두었다면 여기 나타난다. 다만 대부분의 NVMe 는 슬롯이나 백플레인에
 * 꽂는 형태라 이름표보다 슬롯 번호(slot.c)가 더 유용하다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_create_firmware_label_files() : label / index 속성을 만든다.
 *                                     값이 없으면 아무것도 만들지 않는다.
 * pci_remove_firmware_label_files() : 그 반대.
 * dsm_get_label()      : ACPI _DSM 으로 이름표를 얻는다.
 * device_has_dsm()     : 이 장치에 해당 _DSM 이 있는지 확인.
 * label_show() / index_show() : sysfs 읽기 핸들러.
 * smbios_instance_string_exist() : SMBIOS type 41 항목이 있는지 확인.
 */

#include <linux/dmi.h>		/* NVMe: DMI/SMBIOS 테이블 접근을 위한 헤더; NVMe 보드 내장 정보 조회용 */
#include <linux/sysfs.h>	/* NVMe: sysfs attribute 생성 및 파일 입출력 매크로 제공 */
#include <linux/pci.h>		/* NVMe: PCI 버스, pci_dev, pci_bus 등 NVMe가 탑재되는 PCIe 구조체 정의 */
#include <linux/pci_ids.h>	/* NVMe: PCI 장치/벤더 ID 정의; NVMe 컨트롤러 식별 시 사용되는 ID 테이블 */
#include <linux/module.h>	/* NVMe: 커널 모듈 매크로 및 module_init/module_exit 지원 */
#include <linux/device.h>	/* NVMe: struct device, device_attribute, kobject 등 sysfs 연동 핵심 구조체 */
#include <linux/nls.h>		/* NVMe: UTF-16 <-> UTF-8 변환을 위한 native language support 헤더 */
#include <linux/acpi.h>		/* NVMe: ACPI _DSM 평가(handle, union acpi_object)를 위한 헤더 */
#include <linux/pci-acpi.h>	/* NVMe: PCI-ACPI 연동 정의, pci_acpi_dsm_guid 및 DSM 상수 포함 */
#include "pci.h"		/* NVMe: PCI 서브시스템 낶부 정의, DSM_PCI_DEVICE_NAME 등 */

/*
 * NVMe: 주어진 PCI 장치(예: NVMe 컨트롤러의 struct device)가 ACPI _DSM을 통해
 * firmware 이름을 제공하는지 확인한다. NVMe 장치가 ACPI namespace에 등록되어
 * 있고 _DSM 함수 0x2 비트 DSM_PCI_DEVICE_NAME를 지원하면 true를 반환한다.
 */
static bool device_has_acpi_name(struct device *dev)
{
#ifdef CONFIG_ACPI		/* NVMe: ACPI가 커널 설정에 포함된 경우에만 컴파일 */
	acpi_handle handle = ACPI_HANDLE(dev);	/* NVMe: 이 PCI 장치(=NVMe endpoint)에 대한 ACPI handle 획득 */

	if (!handle)		/* NVMe: ACPI handle이 없으면 _DSM 평가 불가 */
		return false;	/* NVMe: false를 반환하여 SMBIOS 경로를 사용하도록 유도 */

	return acpi_check_dsm(handle, &pci_acpi_dsm_guid, 0x2,
			      1 << DSM_PCI_DEVICE_NAME);
				/* NVMe: revision 0x2, DSM_PCI_DEVICE_NAME 기능 비트가
				 * 지원되는지 확인; NVMe 컨트롤러의 firmware 라벨/인덱스
				 * 가용성을 판단 */
#else
	return false;		/* NVMe: ACPI 미지원 커널에서는 무조건 ACPI 이름 없음 */
#endif
}

#ifdef CONFIG_DMI		/* NVMe: DMI/SMBIOS 지원 시에만 SMBIOS 기반 label/index 기능 포함 */
/*
 * NVMe: SMBIOS type 41 레코드에서 노출할 attribute 종류를 구분하는 열거형.
 * NVMe 장치가 메인보드에 온보드로 장착된 경우 이 정보로 슬롯 label과
 * instance 번호를 sysfs에 노출한다.
 */
enum smbios_attr_enum {
	SMBIOS_ATTR_NONE = 0,	/* NVMe: 단순히 레코드 존재 여부만 확인할 때 사용 */
	SMBIOS_ATTR_LABEL_SHOW,	/* NVMe: SMBIOS name 문자열(label) sysfs 노출 */
	SMBIOS_ATTR_INSTANCE_SHOW,	/* NVMe: SMBIOS instance 번호(index) sysfs 노출 */
};

/*
 * NVMe: SMBIOS type 41(DEV_ONBOARD) 레코드를 순회하며, 주어진 pci_dev(예:
 * NVMe 컨트롤러)와 일치하는 segment/bus/devfn 항목을 찾아 label 문자열이나
 * instance 번호를 buf에 기록한다. buf가 NULL이면 문자열 길이만 반환한다.
 */
static size_t find_smbios_instance_string(struct pci_dev *pdev, char *buf,
					  enum smbios_attr_enum attribute)
{
	const struct dmi_device *dmi;	/* NVMe: DMI 장치 레코드를 가리키는 포인터 */
	struct dmi_dev_onboard *donboard;	/* NVMe: 온보드 PCI 장치 정보(segment/bus/devfn/instance) */
	int domain_nr = pci_domain_nr(pdev->bus);	/* NVMe: NVMe 컨트롤러가 속한 PCI segment(domain) 번호 */
	int bus = pdev->bus->number;			/* NVMe: NVMe 컨트롤러의 PCI bus 번호 */
	int devfn = pdev->devfn;			/* NVMe: NVMe 컨트롤러의 device/function 번호 */

	dmi = NULL;			/* NVMe: dmi_find_device() 반복 검색을 위해 초기화 */
	while ((dmi = dmi_find_device(DMI_DEV_TYPE_DEV_ONBOARD,
				      NULL, dmi)) != NULL) {
				      /* NVMe: SMBIOS DEV_ONBOARD(type 41) 레코드들을
				       * 순회; NVMe SSD가 온보드로 등록된 경우 해당
				       * 레코드를 찾음 */
		donboard = dmi->device_data;	/* NVMe: 레코드의 온보드 장치 세부 데이터 획득 */
		if (donboard && donboard->segment == domain_nr &&
			    donboard->bus == bus &&
			    donboard->devfn == devfn) {
			    /* NVMe: segment/bus/devfn이 NVMe 컨트롤러와 일치하면
			     * 일치하는 레코드 발견 */
			if (buf) {	/* NVMe: sysfs에 실제로 쓸 버퍼가 제공된 경우 */
				if (attribute == SMBIOS_ATTR_INSTANCE_SHOW)
					return sysfs_emit(buf, "%d\n",
							  donboard->instance);
					  /* NVMe: instance 번호를 "<숫자>\n" 형식으로
					   * sysfs 버퍼에 출력; NVMe 슬롯 인덱스 */
				else if (attribute == SMBIOS_ATTR_LABEL_SHOW)
					return sysfs_emit(buf, "%s\n",
							  dmi->name);
					  /* NVMe: 레코드 이름을 "<라벨>\n" 형식으로
					   * sysfs 버퍼에 출력; NVMe 장치 물리 라벨 */
			}
			return strlen(dmi->name);	/* NVMe: buf가 NULL이면 label 문자열 길이 반환; visible 체크용 */
		}
	}
	return 0;	/* NVMe: 일치하는 SMBIOS 레코드가 없으면 0 반환 */
}

/*
 * NVMe: sysfs "smbios_label" 속성의 show 콜백. NVMe 컨트롤러에 연결된
 * device_attribute를 통해 SMBIOS 라벨 문자열을 사용자 공간에 반환한다.
 */
static ssize_t smbios_label_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	/* NVMe: sysfs의 struct device에서 struct pci_dev(=NVMe 컨트롤러) 포인터 획득 */

	return find_smbios_instance_string(pdev, buf,
					   SMBIOS_ATTR_LABEL_SHOW);
					   /* NVMe: SMBIOS 라벨을 buf에 기록하고 길이 반환 */
}
static struct device_attribute dev_attr_smbios_label = __ATTR(label, 0444,
						    smbios_label_show, NULL);
						    /* NVMe: 읽기 전용(0444) sysfs 속성 정의;
						     * NVMe 컨트롤러의 SMBIOS label 노출 */

/*
 * NVMe: sysfs "index" 속성의 show 콜백. SMBIOS type 41의 instance 번호를
 * 사용자 공간에 반환한다.
 */
static ssize_t index_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
	/* NVMe: sysfs device에서 pci_dev(=NVMe 컨트롤러) 획득 */

	return find_smbios_instance_string(pdev, buf,
					   SMBIOS_ATTR_INSTANCE_SHOW);
					   /* NVMe: SMBIOS instance 번호를 buf에 기록하고 길이 반환 */
}
static DEVICE_ATTR_RO(index);	/* NVMe: 읽기 전용 "index" device_attribute 생성 및 등록용 매크로 */

/*
 * NVMe: SMBIOS 기반 sysfs 속성 배열. NVMe 컨트롤러의 attribute_group 등록 시
 * 사용되며, label과 index 속성을 포괄한다.
 */
static struct attribute *smbios_attrs[] = {
	&dev_attr_smbios_label.attr,	/* NVMe: SMBIOS label 속성 포인터 */
	&dev_attr_index.attr,		/* NVMe: SMBIOS index 속성 포인터 */
	NULL,				/* NVMe: attribute 배열 종료 표시 */
};

/*
 * NVMe: SMBIOS 속성이 sysfs에 보여질지 결정하는 콜백. NVMe 컨트롤러에 대해
 * ACPI _DSM 이름이 우선하면 숨기고, 일치하는 SMBIOS 레코드가 없으면 숨긴다.
 */
static umode_t smbios_attr_is_visible(struct kobject *kobj, struct attribute *a,
				      int n)
{
	struct device *dev = kobj_to_dev(kobj);
	/* NVMe: kobject에서 struct device(=NVMe 컨트롤러의 device) 획득 */
	struct pci_dev *pdev = to_pci_dev(dev);
	/* NVMe: struct device에서 pci_dev 포인터 획득 */

	if (device_has_acpi_name(dev))
		return 0;
		/* NVMe: ACPI _DSM 기반 이름이 있으면 SMBIOS 속성은 노출하지 않음;
		 * ACPI가 우선 */

	if (!find_smbios_instance_string(pdev, NULL, SMBIOS_ATTR_NONE))
		return 0;
		/* NVMe: 일치하는 SMBIOS type 41 레코드가 없으면 속성 숨김 */

	return a->mode;
	/* NVMe: SMBIOS 레코드가 존재하고 ACPI 이름이 없으면 해당 속성의 mode 반환 */
}

/*
 * NVMe: SMBIOS 기반 attribute_group. PCI core는 NVMe 컨트롤러 등록 시 이
 * 그룹을 사용해 label/index sysfs 파일을 생성할 수 있다.
 */
const struct attribute_group pci_dev_smbios_attr_group = {
	.attrs = smbios_attrs,		/* NVMe: SMBIOS 속성 배열 연결 */
	.is_visible = smbios_attr_is_visible,	/* NVMe: sysfs 노출 여부 판단 콜백 연결 */
};
#endif

#ifdef CONFIG_ACPI		/* NVMe: ACPI 지원 시에만 ACPI _DSM 기반 label/index 기능 포함 */
/*
 * NVMe: ACPI _DSM에서 반환된 label/index를 노출할 종류를 구분하는 열거형.
 * PCI Firmware Spec 3.1 section 4.6.7에 정의된 이름/인스턴스 번호를
 * NVMe 컨트롤러에 대해 sysfs로 전달한다.
 */
enum acpi_attr_enum {
	ACPI_ATTR_LABEL_SHOW,	/* NVMe: _DSM에서 얻은 문자열/버퍼 라벨 노출 */
	ACPI_ATTR_INDEX_SHOW,	/* NVMe: _DSM에서 얻은 정수 인스턴스 번호 노출 */
};

/*
 * NVMe: ACPI _DSM이 UTF-16LE 버퍼로 라벨을 반환한 경우, 이를 UTF-8로
 * 변환하여 sysfs 버퍼에 담는다. NVMe 컨트롤러의 firmware 라벨이 유니코드일
 * 때 사용된다.
 */
static int dsm_label_utf16s_to_utf8s(union acpi_object *obj, char *buf)
{
	int len;
	/* NVMe: 변환 후 UTF-8 문자열 길이 저장 변수 */

	len = utf16s_to_utf8s((const wchar_t *)obj->buffer.pointer,
			      obj->buffer.length,
			      UTF16_LITTLE_ENDIAN,
			      buf, PAGE_SIZE - 1);
			      /* NVMe: ACPI buffer의 UTF-16LE 데이터를 UTF-8로 변환;
			       * PAGE_SIZE-1로 오버런 방지; NVMe 라벨의 유니코드 처리 */
	buf[len++] = '\n';
	/* NVMe: sysfs에서 한 줄로 출력되도록 개행 문자 추가 */

	return len;
	/* NVMe: 변환된 UTF-8 문자열 길이(개행 포함) 반환 */
}

/*
 * NVMe: ACPI _DSM(DSM_PCI_DEVICE_NAME)을 평가하여 NVMe 컨트롤러의 label 또는
 * instance 번호를 buf에 기록한다. _DSM은 Package { Integer instance, String|Buffer label }
 * 형태를 반환한다.
 */
static int dsm_get_label(struct device *dev, char *buf,
			 enum acpi_attr_enum attr)
{
	acpi_handle handle = ACPI_HANDLE(dev);
	/* NVMe: NVMe 컨트롤러의 ACPI handle 획득; _DSM 평가 대상 */
	union acpi_object *obj, *tmp;
	/* NVMe: _DSM 반환 객체 포인터(obj)와 package 요소 접근 포인터(tmp) */
	int len = 0;
	/* NVMe: 출력된 문자열 길이 초기화; 실패 시 -1 반환 기준 */

	if (!handle)
		return -1;
		/* NVMe: ACPI handle이 없으면 _DSM 평가 불가; NVMe 장치가 ACPI
		 * namespace에 없는 경우, 예를 들어 bridge emulation header로
		 * 노출된 단순 엔드포인트 등 */

	obj = acpi_evaluate_dsm(handle, &pci_acpi_dsm_guid, 0x2,
				DSM_PCI_DEVICE_NAME, NULL);
				/* NVMe: PCI _DSM 함수 DSM_PCI_DEVICE_NAME를 평가;
				 * revision 0x2, 인자 없음; NVMe 컨트롤러의 firmware
				 * 이름/인덱스 획득 시도 */
	if (!obj)
		return -1;
		/* NVMe: _DSM이 존재하지 않거나 반환 실패 시 -1 반환 */

	tmp = obj->package.elements;
	/* NVMe: _DSM 반환 객체가 Package일 경우 첫 번째 요소(=Integer instance) */
	if (obj->type == ACPI_TYPE_PACKAGE && obj->package.count == 2 &&
	    tmp[0].type == ACPI_TYPE_INTEGER &&
	    (tmp[1].type == ACPI_TYPE_STRING ||
	     tmp[1].type == ACPI_TYPE_BUFFER)) {
	     /* NVMe: _DSM 반환 형식이 Package 2개 요소이고, 첫 요소가 정수,
	      * 두 번째 요소가 문자열 또는 버퍼(UTF-16LE)인지 검증; NVMe 장치의
	      * ACPI firmware 이름 형식이 올바른지 확인 */
		/*
		 * The second string element is optional even when
		 * this _DSM is implemented; when not implemented,
		 * this entry must return a null string.
		 */
		if (attr == ACPI_ATTR_INDEX_SHOW) {
			len = sysfs_emit(buf, "%llu\n", tmp->integer.value);
			/* NVMe: 인스턴스 번호를 "<정수>\n"로 sysfs에 출력;
			 * tmp->integer.value가 NVMe 컨트롤러의 firmware instance */
		} else if (attr == ACPI_ATTR_LABEL_SHOW) {
			if (tmp[1].type == ACPI_TYPE_STRING)
				len = sysfs_emit(buf, "%s\n",
						 tmp[1].string.pointer);
						 /* NVMe: ACPI가 ASCII/UTF-8 문자열로 라벨을
						  * 제공한 경우 그대로 sysfs에 출력 */
			else if (tmp[1].type == ACPI_TYPE_BUFFER)
				len = dsm_label_utf16s_to_utf8s(tmp + 1, buf);
						/* NVMe: ACPI가 UTF-16LE 버퍼로 라벨을
						 * 제공한 경우 UTF-8로 변환 후 출력; NVMe
						 * 슬롯/트레이 라벨에 자주 사용 */
		}
	}

	ACPI_FREE(obj);
	/* NVMe: acpi_evaluate_dsm()가 할당한 ACPI 객체 메모리 해제; NVMe 장치의
	 * _DSM 평가 후 누수 방지 */

	return len > 0 ? len : -1;
	/* NVMe: 출력된 길이가 양수면 반환, 아니면 -1 반환; NVMe sysfs show
	 * 콜백에서 에러 처리 */
}

/*
 * NVMe: sysfs "label" 속성의 show 콜백. ACPI _DSM에서 획득한 NVMe 컨트롤러의
 * firmware 라벨을 사용자 공간에 반환한다.
 */
static ssize_t label_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return dsm_get_label(dev, buf, ACPI_ATTR_LABEL_SHOW);
	/* NVMe: ACPI _DSM 기반 라벨을 buf에 기록하고 길이 반환 */
}
static DEVICE_ATTR_RO(label);	/* NVMe: 읽기 전용 "label" device_attribute 생성 및 등록용 매크로 */

/*
 * NVMe: sysfs "acpi_index" 속성의 show 콜백. ACPI _DSM에서 획득한 NVMe
 * 컨트롤러의 firmware instance 번호를 사용자 공간에 반환한다.
 */
static ssize_t acpi_index_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return dsm_get_label(dev, buf, ACPI_ATTR_INDEX_SHOW);
	/* NVMe: ACPI _DSM 기반 인스턴스 번호를 buf에 기록하고 길이 반환 */
}
static DEVICE_ATTR_RO(acpi_index);	/* NVMe: 읽기 전용 "acpi_index" device_attribute 생성 및 등록용 매크로 */

/*
 * NVMe: ACPI _DSM 기반 sysfs 속성 배열. NVMe 컨트롤러의 attribute_group
 * 등록 시 label과 acpi_index 파일을 생성한다.
 */
static struct attribute *acpi_attrs[] = {
	&dev_attr_label.attr,		/* NVMe: ACPI label 속성 포인터 */
	&dev_attr_acpi_index.attr,	/* NVMe: ACPI index 속성 포인터 */
	NULL,				/* NVMe: attribute 배열 종료 표시 */
};

/*
 * NVMe: ACPI 속성이 sysfs에 보여질지 결정하는 콜백. device_has_acpi_name()이
 * true일 때만 NVMe 컨트롤러에 label/acpi_index 파일을 노출한다.
 */
static umode_t acpi_attr_is_visible(struct kobject *kobj, struct attribute *a,
				    int n)
{
	struct device *dev = kobj_to_dev(kobj);
	/* NVMe: kobject에서 struct device(=NVMe 컨트롤러의 device) 획득 */

	if (!device_has_acpi_name(dev))
		return 0;
		/* NVMe: _DSM 기반 이름을 지원하지 않는 NVMe 장치는 ACPI
		 * 속성 숨김; SMBIOS 경로로 대체 가능 */

	return a->mode;
	/* NVMe: _DSM 지원 시 해당 속성의 mode 반환(읽기 전용 0444) */
}

/*
 * NVMe: ACPI _DSM 기반 attribute_group. PCI core는 NVMe 컨트롤러 등록 시 이
 * 그룹으로 label과 acpi_index sysfs 파일을 만들 수 있다. 이 라벨은 NVMe
 * 장치가 연결된 PCIe 포트/슬롯의 물리적 식별자로, hotplug core의 slot
 * 이벤트, CXL/PCIe AER 복구, 전원 제어, TLP 라우팅 디버깅 등과 연동될 때
 * 활용된다.
 */
const struct attribute_group pci_dev_acpi_attr_group = {
	.attrs = acpi_attrs,		/* NVMe: ACPI 속성 배열 연결 */
	.is_visible = acpi_attr_is_visible,	/* NVMe: sysfs 노출 여부 판단 콜백 연결 */
};
#endif
