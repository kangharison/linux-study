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
 *         -> [이 파일] smbios_attr_is_visible() / acpi_attr_is_visible()
 *            -> 속성을 만들지 말지를 그때그때 판정한다. 파일을 만드는
 *               별도 함수가 있는 것이 아니라, sysfs 속성 그룹의 is_visible
 *               콜백으로 노출 여부를 정하는 방식이다
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
 * smbios_attr_is_visible() / acpi_attr_is_visible() : sysfs 속성 그룹의
 *                        is_visible 콜백. 값이 있을 때만 속성을 보이게 한다.
 *                        "파일을 만드는 함수" 는 따로 없다.
 * dsm_get_label()      : ACPI _DSM 으로 이름표를 얻는다.
 * device_has_acpi_name() : 이 장치에 해당 _DSM 이 있는지 확인.
 * label_show() / acpi_index_show() : ACPI 쪽 sysfs 읽기 핸들러.
 * smbios_label_show() / index_show() : SMBIOS 쪽 sysfs 읽기 핸들러.
 * find_smbios_instance_string() : SMBIOS type 41 항목을 찾는다.
 * dsm_label_utf16s_to_utf8s() : _DSM 이 준 UTF-16 문자열을 UTF-8 로 옮긴다.
 */

#include <linux/dmi.h>
/* [한국어] sysfs_emit() — 속성 버퍼에 안전하게 쓰는 헬퍼. PAGE_SIZE 경계 검사를
 * 대신해 준다. */
#include <linux/sysfs.h>
/* [한국어] to_pci_dev(), struct pci_dev. */
#include <linux/pci.h>
/* [한국어] 이 파일이 직접 쓰지는 않지만 상류가 남겨 둔 include 다. */
#include <linux/pci_ids.h>
/* [한국어] __ATTR / DEVICE_ATTR_RO 계열 매크로를 끌어오는 경로. */
#include <linux/module.h>
/* [한국어] struct device, kobj_to_dev(). */
#include <linux/device.h>
/* [한국어] utf16s_to_utf8s() — _DSM 이 UTF-16 버퍼로 준 이름을 UTF-8 로 옮긴다.
 * ACPI 문자열이 두 가지 형태로 올 수 있어 필요하다. */
#include <linux/nls.h>
/* [한국어] ACPI_HANDLE(), acpi_check_dsm(), acpi_evaluate_dsm(), ACPI_FREE(). */
#include <linux/acpi.h>
/* [한국어] pci_acpi_dsm_guid 와 DSM_PCI_DEVICE_NAME — 이 파일이 쓰는 _DSM 의
 * GUID 와 함수 번호. */
#include <linux/pci-acpi.h>
/* [한국어] struct attribute_group 두 개(pci_dev_smbios_attr_group,
 * pci_dev_acpi_attr_group)의 선언이 여기 있고, pci-sysfs.c 가 그것을 쓴다. */
#include "pci.h"

/* [한국어]
 * device_has_acpi_name - 이 장치에 ACPI 이름표 _DSM 이 구현되어 있는지 본다
 *
 * @dev: 검사할 장치.
 * @return: true = 구현되어 있음, false = 없거나 ACPI 가 빌드에 없음.
 *
 * 이 파일의 두 속성 그룹을 배타적으로 만드는 판정 함수다. SMBIOS 쪽
 * is_visible 은 이것이 true 면 자기를 감추고, ACPI 쪽은 false 면 자기를 감춘다.
 * 그래서 한 장치에 두 그룹이 동시에 보이는 일이 없다.
 *
 * ACPI 를 우선하는 이유는 두 펌웨어 인터페이스가 같은 의미의 이름표를 주기
 * 때문이다. 둘 다 노출하면 사용자가 어느 쪽을 믿어야 할지 알 수 없다.
 *
 * acpi_check_dsm() 은 실제로 평가하지 않고 지원 여부만 묻는 값싼 호출이다.
 * sysfs 속성이 보일 때마다 불릴 수 있으므로 그 편이 낫다.
 *
 * CONFIG_ACPI 가 꺼진 빌드에서는 함수 몸통이 통째로 `return false` 가 된다.
 * 그러면 SMBIOS 쪽 속성이 언제나 보이고, ACPI 쪽 그룹은 아예 컴파일되지 않는다.
 *
 * 실행 컨텍스트: sysfs 속성 노출 판정. 프로세스 컨텍스트이며 ACPI 평가가
 * 잠들 수 있다.
 *
 * 에러 경로: 없다. 판정 결과가 곧 반환값이다.
 *
 * 호출 체인:
 *   smbios_attr_is_visible() / acpi_attr_is_visible() → [이 함수]
 *     → ACPI_HANDLE() → acpi_check_dsm()
 */
static bool device_has_acpi_name(struct device *dev)
{
/* [한국어] ACPI 가 빌드에 없으면 이 검사 자체가 의미 없으므로, */
#ifdef CONFIG_ACPI
	/* [한국어] 이 장치의 ACPI 핸들을 얻는다. */
	acpi_handle handle = ACPI_HANDLE(dev);

	/* [한국어] 대응하는 ACPI 노드가 없으면, */
	if (!handle)
		/* [한국어] 이름표도 없다. */
		return false;

	/* [한국어] 리비전 2 의 DSM_PCI_DEVICE_NAME 함수가 구현되어 있는지 묻는다.
	 * 실제로 평가하지 않고 지원 여부만 확인하는 것이라 값싸다. */
	return acpi_check_dsm(handle, &pci_acpi_dsm_guid, 0x2,
			      1 << DSM_PCI_DEVICE_NAME);
#else
	/* [한국어] ACPI 가 없는 커널에서는 언제나 false. 그러면 SMBIOS 쪽 속성이 항상 보이고
	 * ACPI 쪽 그룹은 아예 컴파일되지 않는다. */
	return false;
#endif
}

/* [한국어] SMBIOS(DMI) 경로. ACPI 와 별개의 펌웨어 인터페이스라 조건도 따로 붙는다. */
#ifdef CONFIG_DMI
enum smbios_attr_enum {
	/* [한국어] 크기만 재는 모드. buf 가 NULL 일 때 쓰이며, 아래 is_visible 이
	 * "항목이 존재하는가" 만 물을 때 이 값을 넘긴다. */
	SMBIOS_ATTR_NONE = 0,
	/* [한국어] label 속성을 출력하는 모드. */
	SMBIOS_ATTR_LABEL_SHOW,
	/* [한국어] index 속성을 출력하는 모드. 하나의 조회 함수가 세 가지 일을 하도록
	 * 이 enum 이 갈라 준다. */
	SMBIOS_ATTR_INSTANCE_SHOW,
};

/* [한국어]
 * find_smbios_instance_string - SMBIOS 온보드 장치 표에서 이 장치의 항목을 찾는다
 *
 * @pdev: 대상 PCI 장치.
 * @buf: 출력 버퍼. NULL 이면 크기만 잰다.
 * @attribute: 무엇을 출력할지. 크기만 재는 호출에서는 SMBIOS_ATTR_NONE.
 * @return: buf 가 있으면 출력한 바이트 수, 없으면 이름 길이. 못 찾으면 0.
 *
 * 하나의 함수가 세 가지 일을 한다 — 항목이 있는지 확인, label 출력,
 * index 출력. enum 인자가 그것을 갈라 주며, buf 의 NULL 여부가 다시
 * "출력할지 재기만 할지" 를 가른다.
 *
 * 찾는 방법은 DMI 표를 처음부터 훑으며 세그먼트·버스·devfn 세 값이 모두
 * 맞는 항목을 고르는 것이다. SMBIOS 는 이름이 아니라 PCI 위치로 장치를
 * 지목하므로 세 값을 함께 비교해야 한다.
 *
 * 반환값 0 이 "없음" 의 표시라는 점이 중요하다. is_visible 콜백이 buf 를
 * NULL 로 주어 이 함수를 부르고, 0 이면 속성을 감춘다 — 빈 파일을 만들지
 * 않으려는 것이다.
 *
 * 실행 컨텍스트: sysfs 읽기와 속성 노출 판정. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 못 찾으면 0 이다.
 *
 * 호출 체인:
 *   smbios_label_show() / index_show() / smbios_attr_is_visible()
 *     → [이 함수] → dmi_find_device() → sysfs_emit()
 */
static size_t find_smbios_instance_string(struct pci_dev *pdev, char *buf,
					  /* [한국어] DMI 장치 목록을 순회할 커서. */
					  enum smbios_attr_enum attribute)
/* [한국어] 온보드 장치 정보. dmi->device_data 를 이 타입으로 해석한다. */
{
	/* [한국어] 이 장치의 PCI 도메인(세그먼트) 번호. */
	const struct dmi_device *dmi;
	/* [한국어] 버스 번호. */
	struct dmi_dev_onboard *donboard;
	/* [한국어] 장치+기능 번호. 이 셋이 SMBIOS 항목과 맞춰 볼 열쇠다. */
	int domain_nr = pci_domain_nr(pdev->bus);
	int bus = pdev->bus->number;
	/* [한국어] 순회 시작. */
	int devfn = pdev->devfn;
/* [한국어] 온보드 장치 종류의 DMI 항목을 하나씩 꺼낸다. 마지막 인자가 커서라
 * 이전 결과 다음부터 이어서 찾는다. */

	dmi = NULL;
	/* [한국어] 항목의 사설 데이터를 온보드 장치 정보로 본다. */
	while ((dmi = dmi_find_device(DMI_DEV_TYPE_DEV_ONBOARD,
				      /* [한국어] 세그먼트·버스·devfn 이 모두 맞아야 우리 장치다. SMBIOS 는 PCI 위치로
				       * 장치를 지목하므로 세 값을 함께 비교해야 한다. */
				      NULL, dmi)) != NULL) {
		donboard = dmi->device_data;
		if (donboard && donboard->segment == domain_nr &&
			    /* [한국어] 버퍼가 주어졌으면 실제로 출력한다. */
			    donboard->bus == bus &&
			    /* [한국어] 인스턴스 번호를 요구하면, */
			    donboard->devfn == devfn) {
			/* [한국어] 그 숫자를 찍는다. */
			if (buf) {
				if (attribute == SMBIOS_ATTR_INSTANCE_SHOW)
					/* [한국어] 이름표를 요구하면, */
					return sysfs_emit(buf, "%d\n",
							  /* [한국어] DMI 항목의 이름을 찍는다. */
							  donboard->instance);
				else if (attribute == SMBIOS_ATTR_LABEL_SHOW)
					return sysfs_emit(buf, "%s\n",
							  /* [한국어] 버퍼가 없으면(크기만 재는 호출) 이름 길이만 돌려준다. 0 이 아니면
							   * "항목이 있다" 는 뜻이 되어 is_visible 의 판정 근거가 된다. */
							  dmi->name);
			}
			/* [한국어] 끝까지 못 찾으면 0 — 이 장치에는 SMBIOS 이름표가 없다. */
			return strlen(dmi->name);
		}
	}
	return 0;
}

/* [한국어] sysfs 의 device 에서 pci_dev 를 되찾는다. */
/* [한국어]
 * smbios_label_show - SMBIOS 이름표를 sysfs 의 label 파일로 내보낸다
 *
 * @dev: sysfs 가 넘긴 device.
 * @attr: 속성 자체. 쓰지 않는다.
 * @buf: 출력 버퍼(PAGE_SIZE).
 * @return: 출력한 바이트 수. 항목이 없으면 0.
 *
 * 조회를 find_smbios_instance_string() 에 통째로 위임하는 얇은 래퍼다.
 *
 * 함수 이름이 파일 이름과 다르다는 점에 주의할 만하다. 사용자에게 보이는
 * 파일은 label 인데, ACPI 쪽에도 같은 이름의 파일이 있어 함수 이름만
 * 구분해 두었다. 그래서 이 속성만 DEVICE_ATTR_RO 대신 __ATTR 을 직접 쓴다.
 * 두 그룹이 동시에 보이지 않는 것은 두 is_visible 콜백이 보장한다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cat .../label → sysfs → [이 함수] → find_smbios_instance_string()
 */
static ssize_t smbios_label_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
/* [한국어] 이름표 모드로 조회를 위임한다. */
{
	struct pci_dev *pdev = to_pci_dev(dev);

	/* [한국어] 속성 이름을 파일 이름과 다르게 하려고 __ATTR 을 직접 쓴다. 함수 이름은
	 * smbios_label_show 지만 사용자에게 보이는 파일 이름은 label 이다.
	 * ACPI 쪽에도 label 이 있어 함수 이름만 구분한 것으로, 두 그룹이 동시에
	 * 보이지 않도록 아래 is_visible 이 배타적으로 판정한다. */
	return find_smbios_instance_string(pdev, buf,
					   SMBIOS_ATTR_LABEL_SHOW);
}
/* [한국어] index 속성의 출력 함수. */
static struct device_attribute dev_attr_smbios_label = __ATTR(label, 0444,
						    smbios_label_show, NULL);
/* [한국어] pci_dev 복원. */

/* [한국어]
 * index_show - SMBIOS 인스턴스 번호를 sysfs 의 index 파일로 내보낸다
 *
 * @dev: sysfs 가 넘긴 device.
 * @attr: 속성 자체. 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수. 항목이 없으면 0.
 *
 * smbios_label_show() 와 같은 구조이고 enum 인자만 다르다.
 *
 * 이쪽은 함수 이름과 파일 이름이 같아 DEVICE_ATTR_RO 매크로로 충분하다.
 * label 쪽이 __ATTR 을 써야 했던 것과 대비된다.
 *
 * systemd 의 "예측 가능한 네트워크 인터페이스 이름"(eno1 같은 것)이 읽는
 * 값이 이것이다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cat .../index → sysfs → [이 함수] → find_smbios_instance_string()
 */
static ssize_t index_show(struct device *dev, struct device_attribute *attr,
			  /* [한국어] 인스턴스 번호 모드로 위임한다. */
			  char *buf)
{
	struct pci_dev *pdev = to_pci_dev(dev);
/* [한국어] 이쪽은 함수 이름과 파일 이름이 같아 표준 매크로로 충분하다. */

	return find_smbios_instance_string(pdev, buf,
					   /* [한국어] SMBIOS 그룹에 속한 두 속성. */
					   SMBIOS_ATTR_INSTANCE_SHOW);
/* [한국어] label. */
}
/* [한국어] index. */
static DEVICE_ATTR_RO(index);
/* [한국어] 배열 끝 표시. */

static struct attribute *smbios_attrs[] = {
	&dev_attr_smbios_label.attr,
	&dev_attr_index.attr,
	NULL,
};

/* [한국어] kobject 에서 device 를, */
/* [한국어]
 * smbios_attr_is_visible - SMBIOS 속성 두 개를 노출할지 그때그때 정한다
 *
 * @kobj: 속성이 달릴 kobject.
 * @a: 판정 대상 속성.
 * @n: 그룹 안에서의 인덱스. 쓰지 않는다 — 두 속성을 같은 기준으로 다루기 때문이다.
 * @return: 노출할 모드(0444) 또는 0 = 감춤.
 *
 * 이 파일에는 "파일을 만드는 함수" 가 없다. 대신 속성 그룹의 is_visible
 * 콜백이 그 역할을 하며, 0 을 돌려주면 그 파일이 아예 만들어지지 않는다.
 *
 * 두 가지를 확인한다. ACPI 이름표가 있으면 감추고(그쪽을 우선한다),
 * SMBIOS 항목이 없어도 감춘다(빈 파일을 만들지 않는다).
 *
 * find_smbios_instance_string() 을 buf 없이 부르는 것이 "존재 확인" 이다.
 * 같은 함수가 실제 출력에도 쓰이므로, 보이는 파일에는 반드시 내용이 있다.
 *
 * acpi_attr_is_visible() 과 첫 조건이 정확히 반대라, 두 그룹 중 최대 하나만
 * 보인다.
 *
 * 실행 컨텍스트: sysfs 속성 생성 시점과 그 뒤의 재판정. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci-sysfs.c 의 속성 그룹 등록 → sysfs 코어 → [이 함수]
 *     → device_has_acpi_name() → find_smbios_instance_string(NULL)
 */
static umode_t smbios_attr_is_visible(struct kobject *kobj, struct attribute *a,
				      /* [한국어] 거기서 pci_dev 를 얻는다. */
				      int n)
{
	/* [한국어] ACPI 이름표가 있는 장치라면, */
	struct device *dev = kobj_to_dev(kobj);
	/* [한국어] SMBIOS 쪽은 감춘다. 두 펌웨어 인터페이스가 같은 의미의 이름표를 주므로
	 * 둘을 동시에 노출하면 어느 쪽이 맞는지 알 수 없기 때문이다. ACPI 를
	 * 우선한다. */
	struct pci_dev *pdev = to_pci_dev(dev);

	/* [한국어] SMBIOS 항목이 아예 없어도, */
	if (device_has_acpi_name(dev))
		/* [한국어] 감춘다. 빈 파일을 만들지 않으려는 것이다. */
		return 0;

	/* [한국어] 둘 다 통과하면 원래 모드(0444)로 보인다. 파일을 만드는 별도 함수가 있는
	 * 것이 아니라, 이 콜백의 반환값이 곧 노출 여부라는 점이 이 파일의 구조다. */
	if (!find_smbios_instance_string(pdev, NULL, SMBIOS_ATTR_NONE))
		return 0;

	/* [한국어] pci-sysfs.c 가 장치 속성을 만들 때 이 그룹을 넘긴다. */
	return a->mode;
/* [한국어] 위 두 속성. */
}
/* [한국어] 그리고 노출 판정 콜백. */

const struct attribute_group pci_dev_smbios_attr_group = {
	/* [한국어] DMI 조건 끝. */
	.attrs = smbios_attrs,
	.is_visible = smbios_attr_is_visible,
/* [한국어] 여기서부터 ACPI 경로. */
};
#endif
/* [한국어] 이름표 출력 모드. */

/* [한국어] 인덱스 출력 모드. SMBIOS 쪽 enum 과 달리 "크기만 재기" 항목이 없는데,
 * ACPI 쪽 is_visible 은 이름표 존재 여부를 device_has_acpi_name() 으로
 * 따로 확인하기 때문이다. */
#ifdef CONFIG_ACPI
enum acpi_attr_enum {
	ACPI_ATTR_LABEL_SHOW,
	ACPI_ATTR_INDEX_SHOW,
/* [한국어] 변환된 길이. */
};

/* [한국어] UTF-16 리틀 엔디언 버퍼를 UTF-8 로 옮긴다. 마지막 인자가 PAGE_SIZE - 1 인
 * 것은 아래에서 개행 한 글자를 더 넣을 자리를 남기기 위해서다. */
/* [한국어]
 * dsm_label_utf16s_to_utf8s - _DSM 이 버퍼로 준 UTF-16 이름을 UTF-8 로 옮긴다
 *
 * @obj: 문자열이 담긴 ACPI 버퍼 객체.
 * @buf: 출력 버퍼(PAGE_SIZE).
 * @return: 개행까지 포함한 총 길이.
 *
 * ACPI 펌웨어가 이름표를 두 가지 형태로 줄 수 있어 필요한 함수다.
 * ACPI_TYPE_STRING 이면 그대로 쓰면 되지만, ACPI_TYPE_BUFFER 면 UTF-16
 * 리틀 엔디언이라 변환해야 한다.
 *
 * 변환 상한을 PAGE_SIZE - 1 로 두는 것이 핵심이다. 바로 다음 줄에서 개행
 * 한 글자를 더 넣기 때문에, 그 자리를 미리 비워 두어야 버퍼를 넘지 않는다.
 *
 * sysfs_emit() 을 쓰지 않는 이유는 변환 함수가 목적지 버퍼에 직접 쓰는
 * 형태이기 때문이다. 그래서 경계 관리를 이 함수가 직접 한다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 변환이 실패하면 len 이 0 이 되고 개행만 남는데,
 * 호출자가 그 길이를 그대로 쓴다.
 *
 * 호출 체인:
 *   dsm_get_label() → [이 함수] → utf16s_to_utf8s()
 */
static int dsm_label_utf16s_to_utf8s(union acpi_object *obj, char *buf)
{
	int len;

	len = utf16s_to_utf8s((const wchar_t *)obj->buffer.pointer,
			      /* [한국어] 개행을 붙이고 길이를 하나 늘린다. sysfs 관례상 값 뒤에 개행이 온다. */
			      obj->buffer.length,
			      UTF16_LITTLE_ENDIAN,
			      /* [한국어] 최종 길이를 돌려준다. */
			      buf, PAGE_SIZE - 1);
	buf[len++] = '\n';

	return len;
}

/* [한국어] ACPI 핸들. */
/* [한국어]
 * dsm_get_label - ACPI _DSM 으로 이름표나 인덱스를 얻어 출력한다
 *
 * @dev: 대상 장치.
 * @buf: 출력 버퍼.
 * @attr: 이름표를 원하는지 인덱스를 원하는지.
 * @return: 출력한 바이트 수, 또는 -1 = 얻지 못함.
 *
 * ACPI 경로의 조회 본체다. SMBIOS 쪽 find_smbios_instance_string() 에
 * 대응하지만, 펌웨어가 준 자료를 훨씬 꼼꼼히 검사한다는 점이 다르다.
 *
 * 형식 검사가 네 겹이다 — 패키지인가, 원소가 정확히 둘인가, 첫째가 정수인가,
 * 둘째가 문자열이거나 버퍼인가. 펌웨어는 신뢰할 수 없는 입력원이므로,
 * 검사 없이 tmp[1].string.pointer 를 따라가면 커널이 무너진다.
 *
 * len 초기값이 0 이고 마지막에 `len > 0 ? len : -1` 로 거르는 구조가
 * 그 검사와 맞물린다. 형식이 어긋나면 if 블록에 들어가지 않아 0 인 채로
 * 남고, 함수 안의 영어 주석이 말하는 "둘째 문자열은 선택 사항이며 구현하지
 * 않았으면 빈 문자열을 반환해야 한다" 는 경우에도 sysfs_emit 이 0 을 주어
 * 같은 자리로 모인다. 두 경우 모두 -1 이 되어 호출자가 실패로 본다.
 *
 * ACPI_FREE 가 검사 성공 여부와 무관하게 한 번만 불리는 것도 그 구조 덕이다 —
 * 조기 반환이 없어 누수 경로가 생기지 않는다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트이며 ACPI 평가가 잠들 수 있다.
 *
 * 에러 경로: 핸들이 없거나 평가 실패, 형식 불일치, 빈 문자열 모두 -1.
 * sysfs 는 음수를 오류로 해석해 read() 에 그대로 돌려준다.
 *
 * 호출 체인:
 *   label_show() / acpi_index_show() → [이 함수]
 *     → acpi_evaluate_dsm() → sysfs_emit() 또는 dsm_label_utf16s_to_utf8s()
 *     → ACPI_FREE()
 */
static int dsm_get_label(struct device *dev, char *buf,
			 /* [한국어] _DSM 반환 객체와 그 안의 원소들을 가리킬 포인터. */
			 enum acpi_attr_enum attr)
/* [한국어] 출력 길이. 아래 형식 검사를 통과하지 못하면 0 인 채로 남는다. */
{
	acpi_handle handle = ACPI_HANDLE(dev);
	/* [한국어] 핸들이 없으면, */
	union acpi_object *obj, *tmp;
	/* [한국어] 실패. 0 이 아니라 -1 인 것은 0 이 "빈 문자열을 썼다" 와 구분되어야
	 * 하기 때문이다. */
	int len = 0;

	/* [한국어] 리비전 2 의 DSM_PCI_DEVICE_NAME 을 평가한다. */
	if (!handle)
		return -1;
/* [한국어] 평가 실패면, */

	/* [한국어] -1. */
	obj = acpi_evaluate_dsm(handle, &pci_acpi_dsm_guid, 0x2,
				DSM_PCI_DEVICE_NAME, NULL);
	/* [한국어] 패키지의 원소 배열을 가리킨다. 아래 형식 검사보다 **먼저** 대입하는데,
	 * obj 가 패키지가 아니면 이 값은 쓰이지 않으므로 무해하다. */
	if (!obj)
		/* [한국어] 기대하는 형식인지 네 가지를 함께 확인한다 — 패키지인가, 원소가 둘인가,
		 * 첫째가 정수인가, 둘째가 문자열이거나 버퍼인가. 펌웨어가 준 자료라
		 * 믿을 수 없으므로 쓰기 전에 전부 검사한다. */
		return -1;

	tmp = obj->package.elements;
	/* [한국어] 기대하는 형식인지 네 가지를 함께 확인한다 — 패키지인가, 원소가 둘인가,
	 * 첫째가 정수인가, 둘째가 문자열이거나 버퍼인가. 펌웨어가 준 자료라
	 * 믿을 수 없으므로 쓰기 전에 전부 검사한다. 검사를 통과하지 못하면
	 * 아래 블록에 들어가지 않아 len 이 0 인 채로 남고, 함수 끝에서 -1 이 된다. */
	if (obj->type == ACPI_TYPE_PACKAGE && obj->package.count == 2 &&
	    tmp[0].type == ACPI_TYPE_INTEGER &&
	    (tmp[1].type == ACPI_TYPE_STRING ||
	     tmp[1].type == ACPI_TYPE_BUFFER)) {
		/*
		 * The second string element is optional even when
		 * this _DSM is implemented; when not implemented,
		 * this entry must return a null string.
		 */
		/* [한국어] 인덱스를 요구하면, */
		if (attr == ACPI_ATTR_INDEX_SHOW) {
			/* [한국어] 버퍼면, */
			len = sysfs_emit(buf, "%llu\n", tmp->integer.value);
		/* [한국어] UTF-16 으로 보고 UTF-8 로 변환한다. 같은 값을 두 형태로 주는 펌웨어가
		 * 있어 양쪽을 모두 받아 준다. */
		} else if (attr == ACPI_ATTR_LABEL_SHOW) {
			/* [한국어] 둘째 원소가 문자열이면, */
			if (tmp[1].type == ACPI_TYPE_STRING)
				/* [한국어] 그대로 찍고, */
				len = sysfs_emit(buf, "%s\n",
						 tmp[1].string.pointer);
			/* [한국어] 검사를 통과했든 아니든 객체를 놓는다. */
			else if (tmp[1].type == ACPI_TYPE_BUFFER)
				/* [한국어] 이름표를 요구하면, */
				len = dsm_label_utf16s_to_utf8s(tmp + 1, buf);
		/* [한국어] 길이가 양수면 그것을, 아니면 -1 을 돌려준다. 위 영어 주석이 말하는
		 * "둘째 문자열은 선택 사항" 인 경우 — 빈 문자열이 오면 len 이 0 이 되어
		 * 여기서 -1 로 바뀐다. */
		}
	}

	ACPI_FREE(obj);

	/* [한국어] 이름표 모드로 위임. */
	return len > 0 ? len : -1;
}

/* [한국어] 함수 이름과 파일 이름이 같아 표준 매크로를 쓴다. */
/* [한국어]
 * label_show - ACPI 이름표를 sysfs 의 label 파일로 내보낸다
 *
 * @dev: sysfs 가 넘긴 device.
 * @attr: 속성 자체. 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수, 또는 -1.
 *
 * dsm_get_label() 에 위임하는 한 줄짜리 래퍼다.
 *
 * SMBIOS 쪽에도 label 이라는 파일이 있지만, 두 그룹의 is_visible 이 서로
 * 배타적이라 한 장치에 두 파일이 동시에 생기지 않는다. 그래서 이쪽은
 * 함수 이름을 바꿀 필요 없이 DEVICE_ATTR_RO 를 그대로 쓴다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: dsm_get_label() 의 -1 을 그대로 올려보낸다.
 *
 * 호출 체인:
 *   cat .../label → sysfs → [이 함수] → dsm_get_label()
 */
static ssize_t label_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	return dsm_get_label(dev, buf, ACPI_ATTR_LABEL_SHOW);
/* [한국어] 인덱스 모드로 위임. */
}
static DEVICE_ATTR_RO(label);

/* [한국어] 역시 표준 매크로. */
/* [한국어]
 * acpi_index_show - ACPI 가 준 인덱스를 sysfs 의 acpi_index 파일로 내보낸다
 *
 * @dev: sysfs 가 넘긴 device.
 * @attr: 속성 자체. 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 출력한 바이트 수, 또는 -1.
 *
 * label_show() 와 같은 구조이고 enum 인자만 다르다.
 *
 * 파일 이름이 SMBIOS 쪽 index 와 다르게 acpi_index 인 것이 눈에 띈다.
 * 같은 의미의 값인데도 이름을 나눈 것은, 사용자 공간이 어느 펌웨어
 * 인터페이스에서 온 값인지 알 수 있게 하려는 것이다.
 *
 * 실행 컨텍스트: sysfs 읽기. 프로세스 컨텍스트.
 *
 * 에러 경로: dsm_get_label() 의 -1 을 그대로 올려보낸다.
 *
 * 호출 체인:
 *   cat .../acpi_index → sysfs → [이 함수] → dsm_get_label()
 */
static ssize_t acpi_index_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
/* [한국어] ACPI 그룹에 속한 두 속성. */
{
	/* [한국어] label — SMBIOS 쪽 파일 이름과 같지만, 두 그룹이 동시에 보이지 않으므로
	 * 충돌하지 않는다. */
	return dsm_get_label(dev, buf, ACPI_ATTR_INDEX_SHOW);
/* [한국어] acpi_index. 이쪽은 이름이 달라 SMBIOS 의 index 와 구분된다. */
}
/* [한국어] 배열 끝. */
static DEVICE_ATTR_RO(acpi_index);

static struct attribute *acpi_attrs[] = {
	&dev_attr_label.attr,
	&dev_attr_acpi_index.attr,
	NULL,
/* [한국어] kobject 에서 device 를 얻는다. SMBIOS 쪽과 달리 pci_dev 까지 내려갈
 * 필요가 없는데, 판정에 PCI 위치가 아니라 ACPI 핸들만 쓰기 때문이다. */
};

/* [한국어] ACPI 이름표가 없으면, */
/* [한국어]
 * acpi_attr_is_visible - ACPI 속성 두 개를 노출할지 정한다
 *
 * @kobj: 속성이 달릴 kobject.
 * @a: 판정 대상 속성.
 * @n: 그룹 안 인덱스. 쓰지 않는다.
 * @return: 노출할 모드 또는 0 = 감춤.
 *
 * smbios_attr_is_visible() 의 짝이며 판정이 정확히 반대다. ACPI 이름표가
 * 있으면 보이고 없으면 감춘다. 그쪽은 있으면 감추고 없으면(그리고 SMBIOS
 * 항목이 있으면) 보인다. 두 콜백이 이렇게 맞물려 한 장치에 최대 한 그룹만
 * 노출된다.
 *
 * SMBIOS 쪽과 달리 pci_dev 까지 내려가지 않는다. 판정에 PCI 위치가 아니라
 * ACPI 핸들만 쓰기 때문이다.
 *
 * SMBIOS 쪽이 두 가지를 확인해야 했던 것과 달리 여기는 한 가지뿐인데,
 * device_has_acpi_name() 이 이미 "이름표를 얻을 수 있는가" 를 답해 주기
 * 때문이다. 실제로 얻어 봐야 아는 SMBIOS 쪽과의 차이다.
 *
 * 실행 컨텍스트: sysfs 속성 생성 시점. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci-sysfs.c 의 속성 그룹 등록 → sysfs 코어 → [이 함수]
 *     → device_has_acpi_name()
 */
static umode_t acpi_attr_is_visible(struct kobject *kobj, struct attribute *a,
				    /* [한국어] 감춘다. SMBIOS 쪽 판정과 정확히 반대라, 두 그룹 중 최대 하나만 보인다. */
				    int n)
{
	/* [한국어] 있으면 원래 모드로 보인다. */
	struct device *dev = kobj_to_dev(kobj);

	if (!device_has_acpi_name(dev))
		/* [한국어] pci-sysfs.c 가 쓰는 ACPI 쪽 그룹. */
		return 0;
/* [한국어] 위 두 속성. */

	/* [한국어] 노출 판정 콜백. */
	return a->mode;
}
/* [한국어] ACPI 조건 끝. */

const struct attribute_group pci_dev_acpi_attr_group = {
	.attrs = acpi_attrs,
	.is_visible = acpi_attr_is_visible,
};
#endif
