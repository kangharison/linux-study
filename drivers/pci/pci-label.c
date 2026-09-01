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
enum smbios_attr_enum {
	SMBIOS_ATTR_NONE = 0,
	SMBIOS_ATTR_LABEL_SHOW,
	SMBIOS_ATTR_INSTANCE_SHOW,
};

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

static ssize_t smbios_label_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return find_smbios_instance_string(pdev, buf,
					   SMBIOS_ATTR_LABEL_SHOW);
}
static struct device_attribute dev_attr_smbios_label = __ATTR(label, 0444,
						    smbios_label_show, NULL);

static ssize_t index_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return find_smbios_instance_string(pdev, buf,
					   SMBIOS_ATTR_INSTANCE_SHOW);
}
static DEVICE_ATTR_RO(index);

static struct attribute *smbios_attrs[] = {
	&dev_attr_smbios_label.attr,
	&dev_attr_index.attr,
	NULL,
};

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

const struct attribute_group pci_dev_smbios_attr_group = {
	.attrs = smbios_attrs,
	.is_visible = smbios_attr_is_visible,
};
#endif

#ifdef CONFIG_ACPI
enum acpi_attr_enum {
	ACPI_ATTR_LABEL_SHOW,
	ACPI_ATTR_INDEX_SHOW,
};

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

static ssize_t label_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return dsm_get_label(dev, buf, ACPI_ATTR_LABEL_SHOW);
}
static DEVICE_ATTR_RO(label);

static ssize_t acpi_index_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	return dsm_get_label(dev, buf, ACPI_ATTR_INDEX_SHOW);
}
static DEVICE_ATTR_RO(acpi_index);

static struct attribute *acpi_attrs[] = {
	&dev_attr_label.attr,
	&dev_attr_acpi_index.attr,
	NULL,
};

static umode_t acpi_attr_is_visible(struct kobject *kobj, struct attribute *a,
				    int n)
{
	struct device *dev = kobj_to_dev(kobj);

	if (!device_has_acpi_name(dev))
		return 0;

	return a->mode;
}

const struct attribute_group pci_dev_acpi_attr_group = {
	.attrs = acpi_attrs,
	.is_visible = acpi_attr_is_visible,
};
#endif
