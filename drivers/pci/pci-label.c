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

#include <linux/dmi.h>
#include <linux/sysfs.h>
#include <linux/pci.h>
#include <linux/pci_ids.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/nls.h>
#include <linux/acpi.h>
#include <linux/pci-acpi.h>
#include "pci.h"

/*
 * NVMe: 주어진 PCI 장치(예: NVMe 컨트롤러의 struct device)가 ACPI _DSM을 통해
 * firmware 이름을 제공하는지 확인한다. NVMe 장치가 ACPI namespace에 등록되어
 * 있고 _DSM 함수 0x2 비트 DSM_PCI_DEVICE_NAME를 지원하면 true를 반환한다.
 */
static bool device_has_acpi_name(struct device *dev)
{
#ifdef CONFIG_ACPI
	acpi_handle handle = ACPI_HANDLE(dev);

	if (!handle)
		return false;

	return acpi_check_dsm(handle, &pci_acpi_dsm_guid, 0x2,
			      1 << DSM_PCI_DEVICE_NAME);
#else
	return false;
#endif
}

#ifdef CONFIG_DMI
/*
 * NVMe: SMBIOS type 41 레코드에서 노출할 attribute 종류를 구분하는 열거형.
 * NVMe 장치가 메인보드에 온보드로 장착된 경우 이 정보로 슬롯 label과
 * instance 번호를 sysfs에 노출한다.
 */
enum smbios_attr_enum {
	SMBIOS_ATTR_NONE = 0,
	SMBIOS_ATTR_LABEL_SHOW,
	SMBIOS_ATTR_INSTANCE_SHOW,
};

/*
 * NVMe: SMBIOS type 41(DEV_ONBOARD) 레코드를 순회하며, 주어진 pci_dev(예:
 * NVMe 컨트롤러)와 일치하는 segment/bus/devfn 항목을 찾아 label 문자열이나
 * instance 번호를 buf에 기록한다. buf가 NULL이면 문자열 길이만 반환한다.
 */
static size_t find_smbios_instance_string(struct pci_dev *pdev, char *buf,
					  enum smbios_attr_enum attribute)
{
	const struct dmi_device *dmi;
	struct dmi_dev_onboard *donboard;
	int domain_nr = pci_domain_nr(pdev->bus);
	int bus = pdev->bus->number;
	int devfn = pdev->devfn;

	dmi = NULL;
	while ((dmi = dmi_find_device(DMI_DEV_TYPE_DEV_ONBOARD,
				      NULL, dmi)) != NULL) {
		donboard = dmi->device_data;
		if (donboard && donboard->segment == domain_nr &&
			    donboard->bus == bus &&
			    donboard->devfn == devfn) {
			if (buf) {
				if (attribute == SMBIOS_ATTR_INSTANCE_SHOW)
					return sysfs_emit(buf, "%d\n",
							  donboard->instance);
				else if (attribute == SMBIOS_ATTR_LABEL_SHOW)
					return sysfs_emit(buf, "%s\n",
							  dmi->name);
			}
			return strlen(dmi->name);
		}
	}
	return 0;
}

/*
 * NVMe: sysfs "smbios_label" 속성의 show 콜백. NVMe 컨트롤러에 연결된
 * device_attribute를 통해 SMBIOS 라벨 문자열을 사용자 공간에 반환한다.
 */
static ssize_t smbios_label_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return find_smbios_instance_string(pdev, buf,
					   SMBIOS_ATTR_LABEL_SHOW);
}
static struct device_attribute dev_attr_smbios_label = __ATTR(label, 0444,
						    smbios_label_show, NULL);

/*
 * NVMe: sysfs "index" 속성의 show 콜백. SMBIOS type 41의 instance 번호를
 * 사용자 공간에 반환한다.
 */
static ssize_t index_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return find_smbios_instance_string(pdev, buf,
					   SMBIOS_ATTR_INSTANCE_SHOW);
}
static DEVICE_ATTR_RO(index);

/*
 * NVMe: SMBIOS 기반 sysfs 속성 배열. NVMe 컨트롤러의 attribute_group 등록 시
 * 사용되며, label과 index 속성을 포괄한다.
 */
static struct attribute *smbios_attrs[] = {
	&dev_attr_smbios_label.attr,
	&dev_attr_index.attr,
	NULL,
};

/*
 * NVMe: SMBIOS 속성이 sysfs에 보여질지 결정하는 콜백. NVMe 컨트롤러에 대해
 * ACPI _DSM 이름이 우선하면 숨기고, 일치하는 SMBIOS 레코드가 없으면 숨긴다.
 */
static umode_t smbios_attr_is_visible(struct kobject *kobj, struct attribute *a,
				      int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct pci_dev *pdev = to_pci_dev(dev);

	if (device_has_acpi_name(dev))
		return 0;

	if (!find_smbios_instance_string(pdev, NULL, SMBIOS_ATTR_NONE))
		return 0;

	return a->mode;
}

/*
 * NVMe: SMBIOS 기반 attribute_group. PCI core는 NVMe 컨트롤러 등록 시 이
 * 그룹을 사용해 label/index sysfs 파일을 생성할 수 있다.
 */
const struct attribute_group pci_dev_smbios_attr_group = {
	.attrs = smbios_attrs,
	.is_visible = smbios_attr_is_visible,
};
#endif

#ifdef CONFIG_ACPI
/*
 * NVMe: ACPI _DSM에서 반환된 label/index를 노출할 종류를 구분하는 열거형.
 * PCI Firmware Spec 3.1 section 4.6.7에 정의된 이름/인스턴스 번호를
 * NVMe 컨트롤러에 대해 sysfs로 전달한다.
 */
enum acpi_attr_enum {
	ACPI_ATTR_LABEL_SHOW,
	ACPI_ATTR_INDEX_SHOW,
};

/*
 * NVMe: ACPI _DSM이 UTF-16LE 버퍼로 라벨을 반환한 경우, 이를 UTF-8로
 * 변환하여 sysfs 버퍼에 담는다. NVMe 컨트롤러의 firmware 라벨이 유니코드일
 * 때 사용된다.
 */
static int dsm_label_utf16s_to_utf8s(union acpi_object *obj, char *buf)
{
	int len;

	len = utf16s_to_utf8s((const wchar_t *)obj->buffer.pointer,
			      obj->buffer.length,
			      UTF16_LITTLE_ENDIAN,
			      buf, PAGE_SIZE - 1);
	buf[len++] = '\n';

	return len;
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
	union acpi_object *obj, *tmp;
	int len = 0;

	if (!handle)
		return -1;

	obj = acpi_evaluate_dsm(handle, &pci_acpi_dsm_guid, 0x2,
				DSM_PCI_DEVICE_NAME, NULL);
	if (!obj)
		return -1;

	tmp = obj->package.elements;
	if (obj->type == ACPI_TYPE_PACKAGE && obj->package.count == 2 &&
	    tmp[0].type == ACPI_TYPE_INTEGER &&
	    (tmp[1].type == ACPI_TYPE_STRING ||
	     tmp[1].type == ACPI_TYPE_BUFFER)) {
		/*
		 * The second string element is optional even when
		 * this _DSM is implemented; when not implemented,
		 * this entry must return a null string.
		 */
		if (attr == ACPI_ATTR_INDEX_SHOW) {
			len = sysfs_emit(buf, "%llu\n", tmp->integer.value);
		} else if (attr == ACPI_ATTR_LABEL_SHOW) {
			if (tmp[1].type == ACPI_TYPE_STRING)
				len = sysfs_emit(buf, "%s\n",
						 tmp[1].string.pointer);
			else if (tmp[1].type == ACPI_TYPE_BUFFER)
				len = dsm_label_utf16s_to_utf8s(tmp + 1, buf);
		}
	}

	ACPI_FREE(obj);

	return len > 0 ? len : -1;
}

/*
 * NVMe: sysfs "label" 속성의 show 콜백. ACPI _DSM에서 획득한 NVMe 컨트롤러의
 * firmware 라벨을 사용자 공간에 반환한다.
 */
static ssize_t label_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return dsm_get_label(dev, buf, ACPI_ATTR_LABEL_SHOW);
}
static DEVICE_ATTR_RO(label);

/*
 * NVMe: sysfs "acpi_index" 속성의 show 콜백. ACPI _DSM에서 획득한 NVMe
 * 컨트롤러의 firmware instance 번호를 사용자 공간에 반환한다.
 */
static ssize_t acpi_index_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return dsm_get_label(dev, buf, ACPI_ATTR_INDEX_SHOW);
}
static DEVICE_ATTR_RO(acpi_index);

/*
 * NVMe: ACPI _DSM 기반 sysfs 속성 배열. NVMe 컨트롤러의 attribute_group
 * 등록 시 label과 acpi_index 파일을 생성한다.
 */
static struct attribute *acpi_attrs[] = {
	&dev_attr_label.attr,
	&dev_attr_acpi_index.attr,
	NULL,
};

/*
 * NVMe: ACPI 속성이 sysfs에 보여질지 결정하는 콜백. device_has_acpi_name()이
 * true일 때만 NVMe 컨트롤러에 label/acpi_index 파일을 노출한다.
 */
static umode_t acpi_attr_is_visible(struct kobject *kobj, struct attribute *a,
				    int n)
{
	struct device *dev = kobj_to_dev(kobj);

	if (!device_has_acpi_name(dev))
		return 0;

	return a->mode;
}

/*
 * NVMe: ACPI _DSM 기반 attribute_group. PCI core는 NVMe 컨트롤러 등록 시 이
 * 그룹으로 label과 acpi_index sysfs 파일을 만들 수 있다. 이 라벨은 NVMe
 * 장치가 연결된 PCIe 포트/슬롯의 물리적 식별자로, hotplug core의 slot
 * 이벤트, CXL/PCIe AER 복구, 전원 제어, TLP 라우팅 디버깅 등과 연동될 때
 * 활용된다.
 */
const struct attribute_group pci_dev_acpi_attr_group = {
	.attrs = acpi_attrs,
	.is_visible = acpi_attr_is_visible,
};
#endif
