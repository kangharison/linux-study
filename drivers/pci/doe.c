// SPDX-License-Identifier: GPL-2.0
/*
 * Data Object Exchange
 *	PCIe r6.0, sec 6.30 DOE
 *
 * Copyright (C) 2021 Huawei
 *	Jonathan Cameron <Jonathan.Cameron@huawei.com>
 *
 * Copyright (C) 2022 Intel Corporation
 *	Ira Weiny <ira.weiny@intel.com>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/doe.c)은 PCIe Data Object Exchange(DOE) 메일박스를
 * 관리한다. DOE는 PCIe 6.0 sec 6.30에 정의된 메커니즘으로, PCI 장치와
 * 호스트 간에 길이가 긴 데이터 객체를 구성 공간을 통해 교환할 수 있게 한다.
 * NVMe SSD 입장에서 DOE는 다음과 같은 보안/관리 기능의 전송로 역할을 한다.
 *   - PCIe Component Measurement and Authentication(CMA) / SPDM: NVMe
 *     컨트롤러의 신원 확인 및 펌웨어 측정값 교환
 *   - Integrity and Data Encryption(IDE) 키 협상: NVMe 트래픽 무결성/암호화
 *   - 벤더별 DOE feature(대역폭/전원/ROM 갱신 등)의 사용자공간 노출
 *   - doe_features sysfs를 통한 NVMe 장치 DOE capability 확인
 * NVMe 드라이버가 직접 DOE를 호출하지는 않지만, NVMe 장치가 노출하는 DOE
 * capability는 PCI core에 의해 본 파일에서 초기화/관리되며, 보안 하위시스템
 * 또는 nvme-cli 등의 사용자공간 도구가 간접적으로 사용한다.
 * 일반적인 호출 경로:
 *   nvme_probe -> pci_device_probe -> pci_doe_init (probe 시 DOE 메일박스 생성)
 *   보안/인증 레이어 -> pci_find_doe_mailbox -> pci_doe_submit_task 또는
 *   pci_doe -> DOE 상태 머신 -> pci_doe_send_req / pci_doe_recv_resp
 *   nvme_remove / hot-unplug -> pci_doe_disconnected / pci_doe_destroy
 * ===================================================================
 */

#define dev_fmt(fmt) "DOE: " fmt

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/pci-doe.h>
#include <linux/sysfs.h>
#include <linux/workqueue.h>

#include "pci.h"

/* Timeout of 1 second from 6.30.2 Operation, PCI Spec r6.0 */
#define PCI_DOE_TIMEOUT HZ	/* NVMe: DOE 작업 타임아웃을 1초(HZ)로 정의한다. */
#define PCI_DOE_POLL_INTERVAL	(PCI_DOE_TIMEOUT / 128)	/* NVMe: DOE 상태 폴링 간격(약 7.8ms)을 정의한다. */

#define PCI_DOE_FLAG_CANCEL	0	/* NVMe: DOE 작업 취소 플래그 비트를 정의한다. */
#define PCI_DOE_FLAG_DEAD	1	/* NVMe: DOE mailbox dead 플래그 비트를 정의한다. */

/* Max data object length is 2^18 dwords */
#define PCI_DOE_MAX_LENGTH	(1 << 18)	/* NVMe: DOE 데이터 객체 최대 길이(2^18 DW)를 정의한다. */

/**
 * struct pci_doe_mb - State for a single DOE mailbox
 *
 * This state is used to manage a single DOE mailbox capability.  All fields
 * should be considered opaque to the consumers and the structure passed into
 * the helpers below after being created by pci_doe_create_mb().
 *
 * @pdev: PCI device this mailbox belongs to
 * @cap_offset: Capability offset
 * @feats: Array of features supported (encoded as long values)
 * @wq: Wait queue for work item
 * @work_queue: Queue of pci_doe_work items
 * @flags: Bit array of PCI_DOE_FLAG_* flags
 * @sysfs_attrs: Array of sysfs device attributes
 */
struct pci_doe_mb {	/* NVMe: NVMe 장치에 속한 하나의 DOE 메일박스 상태를 나타낸다. */
	struct pci_dev *pdev;	/* NVMe: 이 DOE mailbox가 속한 NVMe PCI 장치를 가리킨다. */
	u16 cap_offset;	/* NVMe: DOE 확장 capability의 구성 공간 오프셋을 저장한다. */
	struct xarray feats;	/* NVMe: 이 mailbox가 지원하는 DOE feature 목록(xarray)이다. */

	wait_queue_head_t wq;	/* NVMe: DOE 작업 완료/취소를 기다리는 대기 큐이다. */
	struct workqueue_struct *work_queue;	/* NVMe: DOE 상태 머신 work를 처리하는 순차 workqueue이다. */
	unsigned long flags;	/* NVMe: CANCEL/DEAD 등 mailbox 상태 플래그를 저장한다. */

#ifdef CONFIG_SYSFS	/* NVMe: 조걸 컴파일 블록을 시작한다. */
	struct device_attribute *sysfs_attrs;	/* NVMe: sysfs에 노출할 DOE feature 속성 배열 포인터이다. */
#endif	/* NVMe: 조걸 컴파일 블록을 끝낸다. */
};	/* NVMe: 구조체/공용체/열거형 정의를 마친다. */

struct pci_doe_feature {	/* NVMe: DOE feature 식별자(Vendor ID + type) 구조체이다. */
	u16 vid;	/* NVMe: 변수를 선언한다. */
	u8 type;	/* NVMe: 변수를 선언한다. */
};	/* NVMe: 구조체/공용체/열거형 정의를 마친다. */

/**
 * struct pci_doe_task - represents a single query/response
 *
 * @feat: DOE Feature
 * @request_pl: The request payload
 * @request_pl_sz: Size of the request payload (bytes)
 * @response_pl: The response payload
 * @response_pl_sz: Size of the response payload (bytes)
 * @rv: Return value.  Length of received response or error (bytes)
 * @complete: Called when task is complete
 * @private: Private data for the consumer
 * @work: Used internally by the mailbox
 * @doe_mb: Used internally by the mailbox
 */
struct pci_doe_task {	/* NVMe: NVMe 측에서 제출하는 하나의 DOE 요청/응답 단위이다. */
	struct pci_doe_feature feat;	/* NVMe: 변수를 선언한다. */
	const __le32 *request_pl;	/* NVMe: DOE 요청 payload 버퍼를 가리킨다. */
	size_t request_pl_sz;	/* NVMe: 요청 payload의 바이트 크기이다. */
	__le32 *response_pl;	/* NVMe: DOE 응답 payload 버퍼를 가리킨다. */
	size_t response_pl_sz;	/* NVMe: 응답 버퍼의 바이트 크기이다. */
	int rv;	/* NVMe: DOE 교환 결과(수신 길이 또는 음수 errno)를 저장한다. */
	void (*complete)(struct pci_doe_task *task);	/* NVMe: DOE 완료 시 호출할 NVMe 측 콜백 함수이다. */
	void *private;	/* NVMe: NVMe 호출자가 사용하는 비공개 데이터이다. */

	/* initialized by pci_doe_submit_task() */
	struct work_struct work;	/* NVMe: mailbox workqueue에 예약된 work 구조체이다. */
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
};	/* NVMe: 구조체/공용체/열거형 정의를 마친다. */

#ifdef CONFIG_SYSFS	/* NVMe: 조걸 컴파일 블록을 시작한다. */

/*
 * doe_discovery_show:
 *   NVMe SSD의 doe_features sysfs 디렉터리 아래 "0001:00" 항목을 노출한다.
 *   PCI-SIG DOE Discovery feature를 통해 사용자공간(nvme-cli 등)에서
 *   장치가 지원하는 DOE feature 목록을 확인할 수 있게 한다.
 */
static ssize_t doe_discovery_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	return sysfs_emit(buf, "0001:00\n");	/* NVMe: DOE Discovery sysfs 항목을 "0001:00"으로 노출한다. */
}
static DEVICE_ATTR_RO(doe_discovery);	/* NVMe: doe_discovery sysfs 속성을 읽기 전용으로 정의한다. */

static struct attribute *pci_doe_sysfs_feature_attrs[] = {	/* NVMe: 변수를 선언한다. */
	&dev_attr_doe_discovery.attr,	/* NVMe: doe_discovery 속성을 그룹에 포함한다. */
	NULL
};	/* NVMe: 구조체/공용체/열거형 정의를 마친다. */

static bool pci_doe_features_sysfs_group_visible(struct kobject *kobj)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */

	return !xa_empty(&pdev->doe_mbs);	/* NVMe: NVMe 장치에 DOE mailbox가 있을 때만 그룹을 노출한다. */
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(pci_doe_features_sysfs)	/* NVMe: sysfs 그룹 가시성 매크로를 정의한다. */

const struct attribute_group pci_doe_sysfs_group = {	/* NVMe: 변수를 선언한다. */
	.name	    = "doe_features",	/* NVMe: 멤버 name을(를) 초기화한다. */
	.attrs	    = pci_doe_sysfs_feature_attrs,	/* NVMe: 멤버 attrs을(를) 초기화한다. */
	.is_visible = SYSFS_GROUP_VISIBLE(pci_doe_features_sysfs),	/* NVMe: 멤버 is_visible을(를) 초기화한다. */
};


/*
 * pci_doe_sysfs_feature_show:
 *   NVMe 장치의 개별 DOE feature 이름을 sysfs에 출력한다.
 *   사용자공간에서 /sys/bus/pci/devices/.../doe_features/ 아래 파일로
 *   NVMe 컨트롤러가 지원하는 DOE 기능을 확인하는 데 사용된다.
 */
static ssize_t pci_doe_sysfs_feature_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	return sysfs_emit(buf, "%s\n", attr->attr.name);	/* NVMe: 개별 DOE feature 이름을 sysfs 버퍼에 출력한다. */
}


/*
 * pci_doe_sysfs_feature_remove:
 *   NVMe 장치에서 DOE feature별 sysfs 파일을 제거하고 메모리를 해제한다.
 *   nvme_remove/hotplug 시 pci_doe_sysfs_teardown()을 통해 호출되어
 *   사용자공간 인터페이스를 정리한다.
 */
static void pci_doe_sysfs_feature_remove(struct pci_dev *pdev,
					 struct pci_doe_mb *doe_mb)
{
	struct device_attribute *attrs = doe_mb->sysfs_attrs;	/* NVMe: 값을 설정한다: struct device_attribute *attrs. */
	struct device *dev = &pdev->dev;	/* NVMe: 값을 설정한다: struct device *dev. */
	unsigned long i;	/* NVMe: 변수를 선언한다. */
	void *entry;	/* NVMe: 변수를 선언한다. */

	if (!attrs)	/* NVMe: 조건을 검사한다: (!attrs. */
		return;	/* NVMe: 함수에서 반환한다. */

	doe_mb->sysfs_attrs = NULL;	/* NVMe: 값을 설정한다: doe_mb->sysfs_attrs. */
	xa_for_each(&doe_mb->feats, i, entry) {	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		if (attrs[i].show)	/* NVMe: 조건을 검사한다: (attrs[i].show. */
			sysfs_remove_file_from_group(&dev->kobj, &attrs[i].attr,	/* NVMe: 함수/매크로 sysfs_remove_file_from_group()를 호출한다. */
						     pci_doe_sysfs_group.name);
		kfree(attrs[i].attr.name);	/* NVMe: 함수/매크로 kfree()를 호출한다. */
	}
	kfree(attrs);	/* NVMe: 함수/매크로 kfree()를 호출한다. */
}


/*
 * pci_doe_sysfs_feature_populate:
 *   NVMe 장치의 DOE mailbox가 지원하는 feature마다 sysfs 파일을 생성한다.
 *   discovery feature는 별도의 doe_discovery 항목으로 처리되며, 나머지는
 *   "VID:TYPE" 형태의 파일로 노출되어 관리 도구에서 확인할 수 있다.
 */
static int pci_doe_sysfs_feature_populate(struct pci_dev *pdev,
					  struct pci_doe_mb *doe_mb)
{
	struct device *dev = &pdev->dev;	/* NVMe: 값을 설정한다: struct device *dev. */
	struct device_attribute *attrs;	/* NVMe: 변수를 선언한다. */
	unsigned long num_features = 0;	/* NVMe: 값을 설정한다: unsigned long num_features. */
	unsigned long vid, type;	/* NVMe: 변수를 선언한다. */
	unsigned long i;	/* NVMe: 변수를 선언한다. */
	void *entry;	/* NVMe: 변수를 선언한다. */
	int ret;	/* NVMe: 변수를 선언한다. */

	xa_for_each(&doe_mb->feats, i, entry)	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		num_features++;	/* NVMe: 값을 증가시킨다: num_features. */

	attrs = kzalloc_objs(*attrs, num_features);	/* NVMe: 값을 설정한다: attrs. */
	if (!attrs) {	/* NVMe: 조건을 검사한다: (!attrs) {. */
		pci_warn(pdev, "Failed allocating the device_attribute array\n");	/* NVMe: 함수/매크로 pci_warn()를 호출한다. */
		return -ENOMEM;	/* NVMe: 결과 -ENOMEM를 반환한다. */
	}

	doe_mb->sysfs_attrs = attrs;	/* NVMe: 값을 설정한다: doe_mb->sysfs_attrs. */
	xa_for_each(&doe_mb->feats, i, entry) {	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		sysfs_attr_init(&attrs[i].attr);	/* NVMe: 함수/매크로 sysfs_attr_init()를 호출한다. */
		vid = xa_to_value(entry) >> 8;	/* NVMe: 값을 설정한다: vid. */
		type = xa_to_value(entry) & 0xFF;	/* NVMe: 값을 설정한다: type. */

		if (vid == PCI_VENDOR_ID_PCI_SIG &&	/* NVMe: 조건을 검사한다: (vid == PCI_VENDOR_ID_PCI_SIG &&. */
		    type == PCI_DOE_FEATURE_DISCOVERY) {

			/*
			 * DOE Discovery, manually displayed by
			 * `dev_attr_doe_discovery`
			 */
			continue;	/* NVMe: 다음 반복으로 넘어간다. */
		}

		attrs[i].attr.name = kasprintf(GFP_KERNEL,	/* NVMe: 값을 설정한다: attrs[i].attr.name. */
					       "%04lx:%02lx", vid, type);
		if (!attrs[i].attr.name) {	/* NVMe: 조건을 검사한다: (!attrs[i].attr.name) {. */
			ret = -ENOMEM;	/* NVMe: 값을 설정한다: ret. */
			pci_warn(pdev, "Failed allocating the attribute name\n");	/* NVMe: 함수/매크로 pci_warn()를 호출한다. */
			goto fail;	/* NVMe: 레이블 fail로 이동한다. */
		}

		attrs[i].attr.mode = 0444;	/* NVMe: 값을 설정한다: attrs[i].attr.mode. */
		attrs[i].show = pci_doe_sysfs_feature_show;	/* NVMe: 값을 설정한다: attrs[i].show. */

		ret = sysfs_add_file_to_group(&dev->kobj, &attrs[i].attr,	/* NVMe: 값을 설정한다: ret. */
					      pci_doe_sysfs_group.name);
		if (ret) {	/* NVMe: 조건을 검사한다: (ret) {. */
			attrs[i].show = NULL;	/* NVMe: 값을 설정한다: attrs[i].show. */
			if (ret != -EEXIST) {	/* NVMe: 조건을 검사한다: (ret != -EEXIST) {. */
				pci_warn(pdev, "Failed adding %s to sysfs group\n",	/* NVMe: 함수/매크로 pci_warn()를 호출한다. */
					 attrs[i].attr.name);
				goto fail;	/* NVMe: 레이블 fail로 이동한다. */
			} else	/* NVMe: else 분기로 넘어간다. */
				kfree(attrs[i].attr.name);	/* NVMe: 함수/매크로 kfree()를 호출한다. */
		}
	}

	return 0;	/* NVMe: 결과 0를 반환한다. */

fail:	/* NVMe: 코드 레이블을 정의한다. */
	pci_doe_sysfs_feature_remove(pdev, doe_mb);	/* NVMe: 함수/매크로 pci_doe_sysfs_feature_remove()를 호출한다. */
	return ret;	/* NVMe: 결과 ret를 반환한다. */
}


/*
 * pci_doe_sysfs_teardown:
 *   NVMe 장치에 연결된 모든 DOE mailbox의 sysfs 항목을 제거한다.
 *   장치 제거 단계에서 사용자공간이 DOE feature 정보를 더 이상 볼 수 없도록
 *   정리한다.
 */
void pci_doe_sysfs_teardown(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
	unsigned long index;	/* NVMe: 변수를 선언한다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		pci_doe_sysfs_feature_remove(pdev, doe_mb);	/* NVMe: 함수/매크로 pci_doe_sysfs_feature_remove()를 호출한다. */
}


/*
 * pci_doe_sysfs_init:
 *   NVMe 장치 probe 시 각 DOE mailbox에 대해 sysfs feature 파일을 생성한다.
 *   생성된 /sys/bus/pci/devices/.../doe_features 항목은 nvme-cli 등이
 *   NVMe 컨트롤러의 DOE capability를 확인하는 데 사용된다.
 */
void pci_doe_sysfs_init(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
	unsigned long index;	/* NVMe: 변수를 선언한다. */
	int ret;	/* NVMe: 변수를 선언한다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb) {	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		ret = pci_doe_sysfs_feature_populate(pdev, doe_mb);	/* NVMe: 값을 설정한다: ret. */
		if (ret)	/* NVMe: 조건을 검사한다: (ret. */
			return;	/* NVMe: 함수에서 반환한다. */
	}
}
#endif	/* NVMe: 조걸 컴파일 블록을 끝낸다. */


/*
 * pci_doe_wait:
 *   NVMe 장치의 DOE mailbox에서 취소 플래그가 설정될 때까지 대기한다.
 *   DOE 상태 머신이 폴링 중 firmware나 다른 주체에 의한 충돌을 감지하면
 *   작업을 중단하기 위해 이 대기를 사용한다.
 */
static int pci_doe_wait(struct pci_doe_mb *doe_mb, unsigned long timeout)
{
	if (wait_event_timeout(doe_mb->wq,	/* NVMe: 조건을 검사한다: (wait_event_timeout(doe_mb->wq,. */
			       test_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags),
			       timeout))
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */
	return 0;	/* NVMe: 결과 0를 반환한다. */
}


/*
 * pci_doe_write_ctrl:
 *   NVMe 장치의 DOE 제어 레지스터(PCI_DOE_CTRL)에 값을 기록한다.
 *   GO/ABORT 비트를 설정하여 DOE 데이터 객체 교환을 시작하거나 중단한다.
 */
static void pci_doe_write_ctrl(struct pci_doe_mb *doe_mb, u32 val)
{
	struct pci_dev *pdev = doe_mb->pdev;	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */
	int offset = doe_mb->cap_offset;	/* NVMe: 값을 설정한다: int offset. */

	pci_write_config_dword(pdev, offset + PCI_DOE_CTRL, val);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
}


/*
 * pci_doe_abort:
 *   NVMe 장치의 DOE mailbox에 abort를 발행하여 상태 머신을 리셋한다.
 *   probe 시 메일박스 초기화는 물론, 장치 분리(hotplug)나 오류 복구 시
 *   진행 중인 DOE 교환을 정리하는 데 필수적이다.
 */
static int pci_doe_abort(struct pci_doe_mb *doe_mb)
{
	struct pci_dev *pdev = doe_mb->pdev;	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */
	int offset = doe_mb->cap_offset;	/* NVMe: 값을 설정한다: int offset. */
	unsigned long timeout_jiffies;	/* NVMe: 변수를 선언한다. */

	pci_dbg(pdev, "[%x] Issuing Abort\n", offset);	/* NVMe: 함수/매크로 pci_dbg()를 호출한다. */

	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;	/* NVMe: 값을 설정한다: timeout_jiffies. */
	pci_doe_write_ctrl(doe_mb, PCI_DOE_CTRL_ABORT);	/* NVMe: ABORT 비트를 써서 DOE mailbox를 리셋한다. */

	do {	/* NVMe: do-while 루프 본문을 시작한다. */
		int rc;	/* NVMe: 변수를 선언한다. */
		u32 val;	/* NVMe: 변수를 선언한다. */

		rc = pci_doe_wait(doe_mb, PCI_DOE_POLL_INTERVAL);	/* NVMe: 값을 설정한다: rc. */
		if (rc)	/* NVMe: 조건을 검사한다: (rc. */
			return rc;	/* NVMe: 결과 rc를 반환한다. */
		pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */

		/* Abort success! */
		if (!FIELD_GET(PCI_DOE_STATUS_ERROR, val) &&	/* NVMe: 조건을 검사한다: (!FIELD_GET(PCI_DOE_STATUS_ERROR, val) &&. */
		    !FIELD_GET(PCI_DOE_STATUS_BUSY, val))
			return 0;	/* NVMe: 결과 0를 반환한다. */

	} while (!time_after(jiffies, timeout_jiffies));	/* NVMe: do-while 조건을 검사한다. */

	/* Abort has timed out and the MB is dead */
	pci_err(pdev, "[%x] ABORT timed out\n", offset);	/* NVMe: 함수/매크로 pci_err()를 호출한다. */
	return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */
}


/*
 * pci_doe_send_req:
 *   NVMe 호스트가 DOE 요청 데이터 객체를 NVMe 장치의 mailbox 쓰기 포트로
 *   전송한다. Busy/Error 상태를 확인하고, 헤더와 payload를 DWORD 단위로
 *   기록한 뒤 GO 비트를 설정한다.
 */
static int pci_doe_send_req(struct pci_doe_mb *doe_mb,
			    struct pci_doe_task *task)
{
	struct pci_dev *pdev = doe_mb->pdev;	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */
	int offset = doe_mb->cap_offset;	/* NVMe: 값을 설정한다: int offset. */
	unsigned long timeout_jiffies;	/* NVMe: 변수를 선언한다. */
	size_t length, remainder;	/* NVMe: 변수를 선언한다. */
	u32 val;	/* NVMe: 변수를 선언한다. */
	int i;	/* NVMe: 변수를 선언한다. */

	/*
	 * Check the DOE busy bit is not set. If it is set, this could indicate
	 * someone other than Linux (e.g. firmware) is using the mailbox. Note
	 * it is expected that firmware and OS will negotiate access rights via
	 * an, as yet to be defined, method.
	 *
	 * Wait up to one PCI_DOE_TIMEOUT period to allow the prior command to
	 * finish. Otherwise, simply error out as unable to field the request.
	 *
	 * PCIe r6.2 sec 6.30.3 states no interrupt is raised when the DOE Busy
	 * bit is cleared, so polling here is our best option for the moment.
	 */
	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;	/* NVMe: 값을 설정한다: timeout_jiffies. */
	do {	/* NVMe: do-while 루프 본문을 시작한다. */
		pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
	} while (FIELD_GET(PCI_DOE_STATUS_BUSY, val) &&	/* NVMe: do-while 조건을 검사한다. */
		 !time_after(jiffies, timeout_jiffies));

	if (FIELD_GET(PCI_DOE_STATUS_BUSY, val))	/* NVMe: 조건을 검사한다: (FIELD_GET(PCI_DOE_STATUS_BUSY, val). */
		return -EBUSY;	/* NVMe: 결과 -EBUSY를 반환한다. */

	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val))	/* NVMe: 조건을 검사한다: (FIELD_GET(PCI_DOE_STATUS_ERROR, val). */
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */

	/* Length is 2 DW of header + length of payload in DW */
	length = 2 + DIV_ROUND_UP(task->request_pl_sz, sizeof(__le32));	/* NVMe: 값을 설정한다: length. */
	if (length > PCI_DOE_MAX_LENGTH)	/* NVMe: 조건을 검사한다: (length > PCI_DOE_MAX_LENGTH. */
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */
	if (length == PCI_DOE_MAX_LENGTH)	/* NVMe: 조건을 검사한다: (length == PCI_DOE_MAX_LENGTH. */
		length = 0;	/* NVMe: 값을 설정한다: length. */

	/* Write DOE Header */
	val = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_VID, task->feat.vid) |	/* NVMe: 값을 설정한다: val. */
		FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, task->feat.type);
	pci_write_config_dword(pdev, offset + PCI_DOE_WRITE, val);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
	pci_write_config_dword(pdev, offset + PCI_DOE_WRITE,	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
			       FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH,
					  length));

	/* Write payload */
	for (i = 0; i < task->request_pl_sz / sizeof(__le32); i++)	/* NVMe: 반복문을 순회한다. */
		pci_write_config_dword(pdev, offset + PCI_DOE_WRITE,	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
				       le32_to_cpu(task->request_pl[i]));

	/* Write last payload dword */
	remainder = task->request_pl_sz % sizeof(__le32);	/* NVMe: 값을 설정한다: remainder. */
	if (remainder) {	/* NVMe: 조건을 검사한다: (remainder) {. */
		val = 0;	/* NVMe: 값을 설정한다: val. */
		memcpy(&val, &task->request_pl[i], remainder);	/* NVMe: 함수/매크로 memcpy()를 호출한다. */
		le32_to_cpus(&val);	/* NVMe: 함수/매크로 le32_to_cpus()를 호출한다. */
		pci_write_config_dword(pdev, offset + PCI_DOE_WRITE, val);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
	}

	pci_doe_write_ctrl(doe_mb, PCI_DOE_CTRL_GO);	/* NVMe: GO 비트를 설정해 DOE 데이터 교환을 시작한다. */

	return 0;	/* NVMe: 결과 0를 반환한다. */
}


/*
 * pci_doe_data_obj_ready:
 *   NVMe 장치가 DOE 응답 데이터 객체를 준비했는지 PCI_DOE_STATUS 레지스터의
 *   Data Object Ready 비트를 읽어 확인한다.
 */
static bool pci_doe_data_obj_ready(struct pci_doe_mb *doe_mb)
{
	struct pci_dev *pdev = doe_mb->pdev;	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */
	int offset = doe_mb->cap_offset;	/* NVMe: 값을 설정한다: int offset. */
	u32 val;	/* NVMe: 변수를 선언한다. */

	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
	if (FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val))	/* NVMe: 조건을 검사한다: (FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val). */
		return true;	/* NVMe: 결과 true를 반환한다. */
	return false;	/* NVMe: 결과 false를 반환한다. */
}


/*
 * pci_doe_recv_resp:
 *   NVMe 장치의 DOE mailbox 읽기 포트에서 응답 데이터 객체를 수신한다.
 *   헤더(VID/type/length)를 검증하고, 사용자가 요청한 크기만큼 payload를
 *   복사하며 초과 데이터는 flush한다.
 */
static int pci_doe_recv_resp(struct pci_doe_mb *doe_mb, struct pci_doe_task *task)
{
	size_t length, payload_length, remainder, received;	/* NVMe: 변수를 선언한다. */
	struct pci_dev *pdev = doe_mb->pdev;	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */
	int offset = doe_mb->cap_offset;	/* NVMe: 값을 설정한다: int offset. */
	int i = 0;	/* NVMe: 값을 설정한다: int i. */
	u32 val;	/* NVMe: 변수를 선언한다. */

	/* Read the first dword to get the feature */
	pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
	if ((FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, val) != task->feat.vid) ||	/* NVMe: 조건을 검사한다: ((FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, val) != task->feat.vid) ||. */
	    (FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, val) != task->feat.type)) {
		dev_err_ratelimited(&pdev->dev, "[%x] expected [VID, Feature] = [%04x, %02x], got [%04x, %02x]\n",	/* NVMe: 함수/매크로 dev_err_ratelimited()를 호출한다. */
				    doe_mb->cap_offset, task->feat.vid, task->feat.type,
				    FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, val),
				    FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, val));
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */
	}

	pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
	/* Read the second dword to get the length */
	pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
	pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */

	length = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, val);	/* NVMe: 값을 설정한다: length. */
	/* A value of 0x0 indicates max data object length */
	if (!length)	/* NVMe: 조건을 검사한다: (!length. */
		length = PCI_DOE_MAX_LENGTH;	/* NVMe: 값을 설정한다: length. */
	if (length < 2)	/* NVMe: 조건을 검사한다: (length < 2. */
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */

	/* First 2 dwords have already been read */
	length -= 2;	/* NVMe: 값을 설정한다: length -. */
	received = task->response_pl_sz;	/* NVMe: 값을 설정한다: received. */
	payload_length = DIV_ROUND_UP(task->response_pl_sz, sizeof(__le32));	/* NVMe: 값을 설정한다: payload_length. */
	remainder = task->response_pl_sz % sizeof(__le32);	/* NVMe: 값을 설정한다: remainder. */

	/* remainder signifies number of data bytes in last payload dword */
	if (!remainder)	/* NVMe: 조건을 검사한다: (!remainder. */
		remainder = sizeof(__le32);	/* NVMe: 값을 설정한다: remainder. */

	if (length < payload_length) {	/* NVMe: 조건을 검사한다: (length < payload_length) {. */
		received = length * sizeof(__le32);	/* NVMe: 값을 설정한다: received. */
		payload_length = length;	/* NVMe: 값을 설정한다: payload_length. */
		remainder = sizeof(__le32);	/* NVMe: 값을 설정한다: remainder. */
	}

	if (payload_length) {	/* NVMe: 조건을 검사한다: (payload_length) {. */
		/* Read all payload dwords except the last */
		for (; i < payload_length - 1; i++) {	/* NVMe: 반복문을 순회한다. */
			pci_read_config_dword(pdev, offset + PCI_DOE_READ,	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
					      &val);
			task->response_pl[i] = cpu_to_le32(val);	/* NVMe: 값을 설정한다: task->response_pl[i]. */
			pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
		}

		/* Read last payload dword */
		pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
		cpu_to_le32s(&val);	/* NVMe: 함수/매크로 cpu_to_le32s()를 호출한다. */
		memcpy(&task->response_pl[i], &val, remainder);	/* NVMe: 함수/매크로 memcpy()를 호출한다. */
		/* Prior to the last ack, ensure Data Object Ready */
		if (!pci_doe_data_obj_ready(doe_mb))	/* NVMe: 조건을 검사한다: (!pci_doe_data_obj_ready(doe_mb). */
			return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */
		pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
		i++;	/* NVMe: 값을 증가시킨다: i. */
	}

	/* Flush excess length */
	for (; i < length; i++) {	/* NVMe: 반복문을 순회한다. */
		pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
		pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);	/* NVMe: 함수/매크로 pci_write_config_dword()를 호출한다. */
	}

	/* Final error check to pick up on any since Data Object Ready */
	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val))	/* NVMe: 조건을 검사한다: (FIELD_GET(PCI_DOE_STATUS_ERROR, val). */
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */

	return received;	/* NVMe: 결과 received를 반환한다. */
}


/*
 * signal_task_complete:
 *   NVMe 측에 제출된 DOE 태스크의 결과를 기록하고 완료 콜백을 호출한다.
 *   동기 호출이라면 completion을 wake하여 NVMe 호출자가 깨어나게 한다.
 */
static void signal_task_complete(struct pci_doe_task *task, int rv)
{
	task->rv = rv;	/* NVMe: 값을 설정한다: task->rv. */
	destroy_work_on_stack(&task->work);	/* NVMe: 함수/매크로 destroy_work_on_stack()를 호출한다. */
	task->complete(task);	/* NVMe: 함수/매크로 complete()를 호출한다. */
}


/*
 * signal_task_abort:
 *   DOE 태스크가 실패했을 때 NVMe 장치에 abort를 시도하고, abort마저
 *   실패하면 해당 mailbox를 dead로 표시한다. 이후 새로운 DOE 요청은
 *   차단되어 NVMe 호스트가 깨진 mailbox를 계속 사용하지 않도록 한다.
 */
static void signal_task_abort(struct pci_doe_task *task, int rv)
{
	struct pci_doe_mb *doe_mb = task->doe_mb;	/* NVMe: 값을 설정한다: struct pci_doe_mb *doe_mb. */
	struct pci_dev *pdev = doe_mb->pdev;	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */

	if (pci_doe_abort(doe_mb)) {	/* NVMe: 조건을 검사한다: (pci_doe_abort(doe_mb)) {. */
		/*
		 * If the device can't process an abort; set the mailbox dead
		 *	- no more submissions
		 */
		pci_err(pdev, "[%x] Abort failed marking mailbox dead\n",	/* NVMe: 함수/매크로 pci_err()를 호출한다. */
			doe_mb->cap_offset);
		set_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags);	/* NVMe: mailbox를 dead로 표시하여 새 제출을 차단한다. */
	}
	signal_task_complete(task, rv);	/* NVMe: 함수/매크로 signal_task_complete()를 호출한다. */
}


/*
 * doe_statemachine_work:
 *   NVMe용 DOE 태스크를 비동기적으로 처리하는 상태 머신 work 함수.
 *   요청 전송 -> 응답 폴링 -> 응답 수신 -> 완료 시그널의 전체 흐름을
 *   담당하며, timeout/오류 시 abort를 수행한다.
 */
static void doe_statemachine_work(struct work_struct *work)
{
	struct pci_doe_task *task = container_of(work, struct pci_doe_task,	/* NVMe: 값을 설정한다: struct pci_doe_task *task. */
						 work);
	struct pci_doe_mb *doe_mb = task->doe_mb;	/* NVMe: 값을 설정한다: struct pci_doe_mb *doe_mb. */
	struct pci_dev *pdev = doe_mb->pdev;	/* NVMe: 값을 설정한다: struct pci_dev *pdev. */
	int offset = doe_mb->cap_offset;	/* NVMe: 값을 설정한다: int offset. */
	unsigned long timeout_jiffies;	/* NVMe: 변수를 선언한다. */
	u32 val;	/* NVMe: 변수를 선언한다. */
	int rc;	/* NVMe: 변수를 선언한다. */

	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags)) {	/* NVMe: 조건을 검사한다: (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags)) {. */
		signal_task_complete(task, -EIO);	/* NVMe: 함수/매크로 signal_task_complete()를 호출한다. */
		return;	/* NVMe: 함수에서 반환한다. */
	}

	/* Send request */
	rc = pci_doe_send_req(doe_mb, task);	/* NVMe: 값을 설정한다: rc. */
	if (rc) {	/* NVMe: 조건을 검사한다: (rc) {. */
		/*
		 * The specification does not provide any guidance on how to
		 * resolve conflicting requests from other entities.
		 * Furthermore, it is likely that busy will not be detected
		 * most of the time.  Flag any detection of status busy with an
		 * error.
		 */
		if (rc == -EBUSY)	/* NVMe: 조건을 검사한다: (rc == -EBUSY. */
			dev_err_ratelimited(&pdev->dev, "[%x] busy detected; another entity is sending conflicting requests\n",	/* NVMe: 함수/매크로 dev_err_ratelimited()를 호출한다. */
					    offset);
		signal_task_abort(task, rc);	/* NVMe: 함수/매크로 signal_task_abort()를 호출한다. */
		return;	/* NVMe: 함수에서 반환한다. */
	}

	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;	/* NVMe: 값을 설정한다: timeout_jiffies. */
	/* Poll for response */
retry_resp:	/* NVMe: 코드 레이블을 정의한다. */
	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);	/* NVMe: 함수/매크로 pci_read_config_dword()를 호출한다. */
	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {	/* NVMe: 조건을 검사한다: (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {. */
		signal_task_abort(task, -EIO);	/* NVMe: 함수/매크로 signal_task_abort()를 호출한다. */
		return;	/* NVMe: 함수에서 반환한다. */
	}

	if (!FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val)) {	/* NVMe: 조건을 검사한다: (!FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val)) {. */
		if (time_after(jiffies, timeout_jiffies)) {	/* NVMe: 조건을 검사한다: (time_after(jiffies, timeout_jiffies)) {. */
			signal_task_abort(task, -EIO);	/* NVMe: 함수/매크로 signal_task_abort()를 호출한다. */
			return;	/* NVMe: 함수에서 반환한다. */
		}
		rc = pci_doe_wait(doe_mb, PCI_DOE_POLL_INTERVAL);	/* NVMe: 값을 설정한다: rc. */
		if (rc) {	/* NVMe: 조건을 검사한다: (rc) {. */
			signal_task_abort(task, rc);	/* NVMe: 함수/매크로 signal_task_abort()를 호출한다. */
			return;	/* NVMe: 함수에서 반환한다. */
		}
		goto retry_resp;	/* NVMe: 레이블 retry_resp로 이동한다. */
	}

	rc  = pci_doe_recv_resp(doe_mb, task);	/* NVMe: 값을 설정한다: rc. */
	if (rc < 0) {	/* NVMe: 조건을 검사한다: (rc < 0) {. */
		signal_task_abort(task, rc);	/* NVMe: 함수/매크로 signal_task_abort()를 호출한다. */
		return;	/* NVMe: 함수에서 반환한다. */
	}

	signal_task_complete(task, rc);	/* NVMe: 함수/매크로 signal_task_complete()를 호출한다. */
}


/*
 * pci_doe_task_complete:
 *   동기 방식 pci_doe() 호출 시 사용하는 낮은 수준 완료 콜백.
 *   NVMe 호스트가 wait_for_completion()으로 대기 중인 completion 객체를
 *   시그널링하여 결과를 반환한다.
 */
static void pci_doe_task_complete(struct pci_doe_task *task)
{
	complete(task->private);	/* NVMe: 동기 대기 중인 completion 객체를 시그널링한다. */
}


/*
 * pci_doe_discovery:
 *   NVMe 장치의 DOE Discovery 프로토콜을 이용해 지원 feature를 하나씩 조회한다.
 *   pci_doe_cache_features()에서 반복 호출되어 NVMe 컨트롤러가 지원하는
 *   vendor/type 조합 목록을 구성한다.
 */
static int pci_doe_discovery(struct pci_doe_mb *doe_mb, u8 capver, u8 *index, u16 *vid,
			     u8 *feature)
{
	u32 request_pl = FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_REQ_3_INDEX,	/* NVMe: 값을 설정한다: u32 request_pl. */
				    *index) |
			 FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_REQ_3_VER,
				    (capver >= 2) ? 2 : 0);
	__le32 request_pl_le = cpu_to_le32(request_pl);	/* NVMe: 값을 설정한다: __le32 request_pl_le. */
	__le32 response_pl_le;	/* NVMe: 변수를 선언한다. */
	u32 response_pl;	/* NVMe: 변수를 선언한다. */
	int rc;	/* NVMe: 변수를 선언한다. */

	rc = pci_doe(doe_mb, PCI_VENDOR_ID_PCI_SIG, PCI_DOE_FEATURE_DISCOVERY,	/* NVMe: 값을 설정한다: rc. */
		     &request_pl_le, sizeof(request_pl_le),
		     &response_pl_le, sizeof(response_pl_le));
	if (rc < 0)	/* NVMe: 조건을 검사한다: (rc < 0. */
		return rc;	/* NVMe: 결과 rc를 반환한다. */

	if (rc != sizeof(response_pl_le))	/* NVMe: 조건을 검사한다: (rc != sizeof(response_pl_le). */
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */

	response_pl = le32_to_cpu(response_pl_le);	/* NVMe: 값을 설정한다: response_pl. */
	*vid = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_VID, response_pl);	/* NVMe: 값을 설정한다: *vid. */
	*feature = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_TYPE,	/* NVMe: 값을 설정한다: *feature. */
			      response_pl);
	*index = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_NEXT_INDEX,	/* NVMe: 값을 설정한다: *index. */
			   response_pl);

	return 0;	/* NVMe: 결과 0를 반환한다. */
}


/*
 * pci_doe_xa_feat_entry:
 *   NVMe 장치의 DOE feature 식별자(vendor ID + type)를 xarray에 저장할 값으로
 *   인코딩한다.
 */
static void *pci_doe_xa_feat_entry(u16 vid, u8 type)
{
	return xa_mk_value((vid << 8) | type);	/* NVMe: VID와 type을 하나의 xarray 값으로 인코딩하여 반환한다. */
}


/*
 * pci_doe_cache_features:
 *   NVMe 장치 probe 시 DOE Discovery를 반복 수행하여 지원 feature 목록을
 *   xarray에 캐시한다. 이 목록은 이후 NVMe 관련 코드가 DOE feature 사용 전
 *   pci_doe_supports_feat()로 지원 여부를 빠르게 확인하는 데 쓰인다.
 */
static int pci_doe_cache_features(struct pci_doe_mb *doe_mb)
{
	u8 index = 0;	/* NVMe: 값을 설정한다: u8 index. */
	u8 xa_idx = 0;	/* NVMe: 값을 설정한다: u8 xa_idx. */
	u32 hdr = 0;	/* NVMe: 값을 설정한다: u32 hdr. */

	pci_read_config_dword(doe_mb->pdev, doe_mb->cap_offset, &hdr);	/* NVMe: DOE capability 헤더(버전 등)를 읽는다. */

	do {	/* NVMe: do-while 루프 본문을 시작한다. */
		int rc;	/* NVMe: 변수를 선언한다. */
		u16 vid;	/* NVMe: 변수를 선언한다. */
		u8 type;	/* NVMe: 변수를 선언한다. */

		rc = pci_doe_discovery(doe_mb, PCI_EXT_CAP_VER(hdr), &index,	/* NVMe: 값을 설정한다: rc. */
				       &vid, &type);
		if (rc)	/* NVMe: 조건을 검사한다: (rc. */
			return rc;	/* NVMe: 결과 rc를 반환한다. */

		pci_dbg(doe_mb->pdev,	/* NVMe: 함수/매크로 pci_dbg()를 호출한다. */
			"[%x] Found feature %d vid: %x type: %x\n",
			doe_mb->cap_offset, xa_idx, vid, type);

		rc = xa_insert(&doe_mb->feats, xa_idx++,	/* NVMe: 값을 설정한다: rc. */
			       pci_doe_xa_feat_entry(vid, type), GFP_KERNEL);
		if (rc)	/* NVMe: 조건을 검사한다: (rc. */
			return rc;	/* NVMe: 결과 rc를 반환한다. */
	} while (index);	/* NVMe: do-while 조건을 검사한다. */

	return 0;	/* NVMe: 결과 0를 반환한다. */
}


/*
 * pci_doe_cancel_tasks:
 *   NVMe 장치 제거, suspend, hot-unplug 등에서 DOE mailbox의 pending/in-progress
 *   작업을 모두 취소한다. DEAD/CANCEL 플래그를 설정하고 대기 중인 work를
 *   깨워 정리한다.
 */
static void pci_doe_cancel_tasks(struct pci_doe_mb *doe_mb)
{
	/* Stop all pending work items from starting */
	set_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags);	/* NVMe: mailbox를 dead로 표시하여 새 제출을 차단한다. */

	/* Cancel an in progress work item, if necessary */
	set_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags);	/* NVMe: 현재 진행 중인 DOE 작업 취소 플래그를 설정한다. */
	wake_up(&doe_mb->wq);	/* NVMe: 취소를 대기하는 work를 깨운다. */
}

/**
 * pci_doe_create_mb() - Create a DOE mailbox object
 *
 * @pdev: PCI device to create the DOE mailbox for
 * @cap_offset: Offset of the DOE mailbox
 *
 * Create a single mailbox object to manage the mailbox feature at the
 * cap_offset specified.
 *
 * RETURNS: created mailbox object on success
 *	    ERR_PTR(-errno) on failure
 */

/*
 * pci_doe_create_mb:
 *   NVMe 장치의 한 DOE 확장 capability에 대해 mailbox 객체를 생성한다.
 *   메모리 할당, workqueue 생성, abort로 리셋, feature 캐시까지 수행하며
 *   성공하면 NVMe pci_dev->doe_mbs에 등록된다.
 */
static struct pci_doe_mb *pci_doe_create_mb(struct pci_dev *pdev,
					    u16 cap_offset)
{
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
	int rc;	/* NVMe: 변수를 선언한다. */

	doe_mb = kzalloc_obj(*doe_mb);	/* NVMe: mailbox 구조체 메모리를 할당한다. */
	if (!doe_mb)	/* NVMe: 조건을 검사한다: (!doe_mb. */
		return ERR_PTR(-ENOMEM);	/* NVMe: 결과 ERR_PTR(-ENOMEM)를 반환한다. */

	doe_mb->pdev = pdev;	/* NVMe: mailbox가 속한 NVMe PCI 장치를 연결한다. */
	doe_mb->cap_offset = cap_offset;	/* NVMe: DOE capability 오프셋을 저장한다. */
	init_waitqueue_head(&doe_mb->wq);	/* NVMe: mailbox 완료 대기 큐를 초기화한다. */
	xa_init(&doe_mb->feats);	/* NVMe: feature xarray를 초기화한다. */

	doe_mb->work_queue = alloc_ordered_workqueue("%s %s DOE [%x]", 0,	/* NVMe: 값을 설정한다: doe_mb->work_queue. */
						dev_bus_name(&pdev->dev),
						pci_name(pdev),
						doe_mb->cap_offset);
	if (!doe_mb->work_queue) {	/* NVMe: 조건을 검사한다: (!doe_mb->work_queue) {. */
		pci_err(pdev, "[%x] failed to allocate work queue\n",	/* NVMe: 함수/매크로 pci_err()를 호출한다. */
			doe_mb->cap_offset);
		rc = -ENOMEM;	/* NVMe: 값을 설정한다: rc. */
		goto err_free;	/* NVMe: 레이블 err_free로 이동한다. */
	}

	/* Reset the mailbox by issuing an abort */
	rc = pci_doe_abort(doe_mb);	/* NVMe: 값을 설정한다: rc. */
	if (rc) {	/* NVMe: 조건을 검사한다: (rc) {. */
		pci_err(pdev, "[%x] failed to reset mailbox with abort command : %d\n",	/* NVMe: 함수/매크로 pci_err()를 호출한다. */
			doe_mb->cap_offset, rc);
		goto err_destroy_wq;	/* NVMe: 레이블 err_destroy_wq로 이동한다. */
	}

	/*
	 * The state machine and the mailbox should be in sync now;
	 * Use the mailbox to query features.
	 */
	rc = pci_doe_cache_features(doe_mb);	/* NVMe: 값을 설정한다: rc. */
	if (rc) {	/* NVMe: 조건을 검사한다: (rc) {. */
		pci_err(pdev, "[%x] failed to cache features : %d\n",	/* NVMe: 함수/매크로 pci_err()를 호출한다. */
			doe_mb->cap_offset, rc);
		goto err_cancel;	/* NVMe: 레이블 err_cancel로 이동한다. */
	}

	return doe_mb;	/* NVMe: 결과 doe_mb를 반환한다. */

err_cancel:	/* NVMe: 코드 레이블을 정의한다. */
	pci_doe_cancel_tasks(doe_mb);	/* NVMe: 함수/매크로 pci_doe_cancel_tasks()를 호출한다. */
	xa_destroy(&doe_mb->feats);	/* NVMe: 함수/매크로 xa_destroy()를 호출한다. */
err_destroy_wq:	/* NVMe: 코드 레이블을 정의한다. */
	destroy_workqueue(doe_mb->work_queue);	/* NVMe: 함수/매크로 destroy_workqueue()를 호출한다. */
err_free:	/* NVMe: 코드 레이블을 정의한다. */
	kfree(doe_mb);	/* NVMe: 함수/매크로 kfree()를 호출한다. */
	return ERR_PTR(rc);	/* NVMe: 결과 ERR_PTR(rc)를 반환한다. */
}

/**
 * pci_doe_destroy_mb() - Destroy a DOE mailbox object
 *
 * @doe_mb: DOE mailbox
 *
 * Destroy all internal data structures created for the DOE mailbox.
 */

/*
 * pci_doe_destroy_mb:
 *   NVMe 장치의 한 DOE mailbox를 완전히 해제한다.
 *   pending 작업 취소, feature xarray 제거, workqueue 파괴, 메모리 반납을
 *   순서대로 수행한다.
 */
static void pci_doe_destroy_mb(struct pci_doe_mb *doe_mb)
{
	pci_doe_cancel_tasks(doe_mb);	/* NVMe: 함수/매크로 pci_doe_cancel_tasks()를 호출한다. */
	xa_destroy(&doe_mb->feats);	/* NVMe: 함수/매크로 xa_destroy()를 호출한다. */
	destroy_workqueue(doe_mb->work_queue);	/* NVMe: 함수/매크로 destroy_workqueue()를 호출한다. */
	kfree(doe_mb);	/* NVMe: 함수/매크로 kfree()를 호출한다. */
}

/**
 * pci_doe_supports_feat() - Return if the DOE instance supports the given
 *			     feature
 * @doe_mb: DOE mailbox capability to query
 * @vid: Feature Vendor ID
 * @type: Feature type
 *
 * RETURNS: True if the DOE mailbox supports the feature specified
 */

/*
 * pci_doe_supports_feat:
 *   NVMe 장치의 특정 DOE mailbox가 지정한 vendor/type feature를 지원하는지
 *   확인한다. PCI-SIG DOE Discovery는 항상 지원되는 것으로 처리한다.
 */
static bool pci_doe_supports_feat(struct pci_doe_mb *doe_mb, u16 vid, u8 type)
{
	unsigned long index;	/* NVMe: 변수를 선언한다. */
	void *entry;	/* NVMe: 변수를 선언한다. */

	/* The discovery feature must always be supported */
	if (vid == PCI_VENDOR_ID_PCI_SIG && type == PCI_DOE_FEATURE_DISCOVERY)	/* NVMe: 조건을 검사한다: (vid == PCI_VENDOR_ID_PCI_SIG && type == PCI_DOE_FEATURE_DISCOVERY. */
		return true;	/* NVMe: 결과 true를 반환한다. */

	xa_for_each(&doe_mb->feats, index, entry)	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		if (entry == pci_doe_xa_feat_entry(vid, type))	/* NVMe: 조건을 검사한다: (entry == pci_doe_xa_feat_entry(vid, type). */
			return true;	/* NVMe: 결과 true를 반환한다. */

	return false;	/* NVMe: 결과 false를 반환한다. */
}

/**
 * pci_doe_submit_task() - Submit a task to be processed by the state machine
 *
 * @doe_mb: DOE mailbox capability to submit to
 * @task: task to be queued
 *
 * Submit a DOE task (request/response) to the DOE mailbox to be processed.
 * Returns upon queueing the task object.  If the queue is full this function
 * will sleep until there is room in the queue.
 *
 * task->complete will be called when the state machine is done processing this
 * task.
 *
 * @task must be allocated on the stack.
 *
 * Excess data will be discarded.
 *
 * RETURNS: 0 when task has been successfully queued, -ERRNO on error
 */

/*
 * pci_doe_submit_task:
 *   NVMe 측이나 보안 하위시스템이 DOE 교환을 요청할 때 태스크를 mailbox
 *   workqueue에 제출한다. 지원 feature 검사와 mailbox 상태(DEAD) 검사 후
 *   비동기 상태 머신 work를 예약한다.
 */
static int pci_doe_submit_task(struct pci_doe_mb *doe_mb,
			       struct pci_doe_task *task)
{
	if (!pci_doe_supports_feat(doe_mb, task->feat.vid, task->feat.type))	/* NVMe: 조건을 검사한다: (!pci_doe_supports_feat(doe_mb, task->feat.vid, task->feat.type). */
		return -EINVAL;	/* NVMe: 결과 -EINVAL를 반환한다. */

	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags))	/* NVMe: 조건을 검사한다: (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags). */
		return -EIO;	/* NVMe: 결과 -EIO를 반환한다. */

	task->doe_mb = doe_mb;	/* NVMe: 값을 설정한다: task->doe_mb. */
	INIT_WORK_ONSTACK(&task->work, doe_statemachine_work);	/* NVMe: 스택 work를 DOE 상태 머신 핸들러로 초기화한다. */
	queue_work(doe_mb->work_queue, &task->work);	/* NVMe: DOE 상태 머신 workqueue에 태스크를 예약한다. */
	return 0;	/* NVMe: 결과 0를 반환한다. */
}

/**
 * pci_doe() - Perform Data Object Exchange
 *
 * @doe_mb: DOE Mailbox
 * @vendor: Vendor ID
 * @type: Data Object Type
 * @request: Request payload
 * @request_sz: Size of request payload (bytes)
 * @response: Response payload
 * @response_sz: Size of response payload (bytes)
 *
 * Submit @request to @doe_mb and store the @response.
 * The DOE exchange is performed synchronously and may therefore sleep.
 *
 * Payloads are treated as opaque byte streams which are transmitted verbatim,
 * without byte-swapping.  If payloads contain little-endian register values,
 * the caller is responsible for conversion with cpu_to_le32() / le32_to_cpu().
 *
 * For convenience, arbitrary payload sizes are allowed even though PCIe r6.0
 * sec 6.30.1 specifies the Data Object Header 2 "Length" in dwords.  The last
 * (partial) dword is copied with byte granularity and padded with zeroes if
 * necessary.  Callers are thus relieved of using dword-sized bounce buffers.
 *
 * RETURNS: Length of received response or negative errno.
 * Received data in excess of @response_sz is discarded.
 * The length may be smaller than @response_sz and the caller
 * is responsible for checking that.
 */

/*
 * pci_doe:
 *   NVMe/보안 모듈이 DOE 데이터 객체 교환을 동기적으로 수행하는 주요 API.
 *   요청 payload를 전송하고 응답이 도착할 때까지 sleep하며, 수신된
 *   응답 길이(또는 음수 errno)를 반환한다.
 */
int pci_doe(struct pci_doe_mb *doe_mb, u16 vendor, u8 type,
	    const void *request, size_t request_sz,
	    void *response, size_t response_sz)
{
	DECLARE_COMPLETION_ONSTACK(c);	/* NVMe: 동기 DOE 완료를 기다릴 completion을 선언한다. */
	struct pci_doe_task task = {	/* NVMe: 변수를 선언한다. */
		.feat.vid = vendor,	/* NVMe: 값을 설정한다: .feat.vid. */
		.feat.type = type,	/* NVMe: 값을 설정한다: .feat.type. */
		.request_pl = request,	/* NVMe: 멤버 request_pl을(를) 초기화한다. */
		.request_pl_sz = request_sz,	/* NVMe: 멤버 request_pl_sz을(를) 초기화한다. */
		.response_pl = response,	/* NVMe: 멤버 response_pl을(를) 초기화한다. */
		.response_pl_sz = response_sz,	/* NVMe: 멤버 response_pl_sz을(를) 초기화한다. */
		.complete = pci_doe_task_complete,	/* NVMe: 멤버 complete을(를) 초기화한다. */
		.private = &c,	/* NVMe: 멤버 private을(를) 초기화한다. */
	};
	int rc;	/* NVMe: 변수를 선언한다. */

	rc = pci_doe_submit_task(doe_mb, &task);	/* NVMe: 값을 설정한다: rc. */
	if (rc)	/* NVMe: 조건을 검사한다: (rc. */
		return rc;	/* NVMe: 결과 rc를 반환한다. */

	wait_for_completion(&c);	/* NVMe: DOE 교환이 끝날 때까지 동기 대기한다. */

	return task.rv;	/* NVMe: DOE 교환 결과(길이 또는 오류)를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_doe);	/* NVMe: pci_doe 심볼을 GPL 모듈에 난중한다. */

/**
 * pci_find_doe_mailbox() - Find Data Object Exchange mailbox
 *
 * @pdev: PCI device
 * @vendor: Vendor ID
 * @type: Data Object Type
 *
 * Find first DOE mailbox of a PCI device which supports the given feature.
 *
 * RETURNS: Pointer to the DOE mailbox or NULL if none was found.
 */

/*
 * pci_find_doe_mailbox:
 *   NVMe pci_dev가 지정한 vendor/type의 DOE feature를 지원하는 첫 번째
 *   mailbox를 반환한다. NVMe 관련 보안/인증 코드가 사용할 mailbox를
 *   찾을 때 사용된다.
 */
struct pci_doe_mb *pci_find_doe_mailbox(struct pci_dev *pdev, u16 vendor,
					u8 type)
{
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
	unsigned long index;	/* NVMe: 변수를 선언한다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		if (pci_doe_supports_feat(doe_mb, vendor, type))	/* NVMe: 조건을 검사한다: (pci_doe_supports_feat(doe_mb, vendor, type). */
			return doe_mb;	/* NVMe: 결과 doe_mb를 반환한다. */

	return NULL;	/* NVMe: 결과 NULL를 반환한다. */
}
EXPORT_SYMBOL_GPL(pci_find_doe_mailbox);	/* NVMe: pci_find_doe_mailbox 심볼을 GPL 모듈에 난중한다. */


/*
 * pci_doe_init:
 *   NVMe 장치 probe 시 PCI core가 호출하여 모든 DOE 확장 capability를
 *   찾고 각각에 대한 mailbox를 생성한다. 생성된 mailbox는 sysfs를 통해
 *   NVMe 사용자공간 도구에 노출될 수 있다.
 */
void pci_doe_init(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
	u16 offset = 0;	/* NVMe: 값을 설정한다: u16 offset. */
	int rc;	/* NVMe: 변수를 선언한다. */

	xa_init(&pdev->doe_mbs);	/* NVMe: NVMe pci_dev의 DOE mailbox 컬렉션(xarray)을 초기화한다. */

	while ((offset = pci_find_next_ext_capability(pdev, offset,	/* NVMe: 조건이 참인 동안 반복한다. */
						      PCI_EXT_CAP_ID_DOE))) {
		doe_mb = pci_doe_create_mb(pdev, offset);	/* NVMe: 값을 설정한다: doe_mb. */
		if (IS_ERR(doe_mb)) {	/* NVMe: 조건을 검사한다: (IS_ERR(doe_mb)) {. */
			pci_err(pdev, "[%x] failed to create mailbox: %ld\n",	/* NVMe: 함수/매크로 pci_err()를 호출한다. */
				offset, PTR_ERR(doe_mb));
			continue;	/* NVMe: 다음 반복으로 넘어간다. */
		}

		rc = xa_insert(&pdev->doe_mbs, offset, doe_mb, GFP_KERNEL);	/* NVMe: 값을 설정한다: rc. */
		if (rc) {	/* NVMe: 조건을 검사한다: (rc) {. */
			pci_err(pdev, "[%x] failed to insert mailbox: %d\n",	/* NVMe: 함수/매크로 pci_err()를 호출한다. */
				offset, rc);
			pci_doe_destroy_mb(doe_mb);	/* NVMe: 함수/매크로 pci_doe_destroy_mb()를 호출한다. */
		}
	}
}


/*
 * pci_doe_destroy:
 *   NVMe 장치 제거 시 모든 DOE mailbox를 파괴하고 pci_dev->doe_mbs
 *   컬렉션을 제거한다.
 */
void pci_doe_destroy(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
	unsigned long index;	/* NVMe: 변수를 선언한다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		pci_doe_destroy_mb(doe_mb);	/* NVMe: 함수/매크로 pci_doe_destroy_mb()를 호출한다. */

	xa_destroy(&pdev->doe_mbs);	/* NVMe: NVMe pci_dev의 mailbox 컬렉션을 제거한다. */
}


/*
 * pci_doe_disconnected:
 *   NVMe 장치가 hotplug 등으로 연결이 끊어졌을 때 모든 DOE mailbox의
 *   pending/in-progress 작업을 즉시 취소한다. 장치가 응답하지 않는 상황에서
 *   무한 대기를 방지한다.
 */
void pci_doe_disconnected(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;	/* NVMe: 변수를 선언한다. */
	unsigned long index;	/* NVMe: 변수를 선언한다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)	/* NVMe: 함수/매크로 xa_for_each()를 호출한다. */
		pci_doe_cancel_tasks(doe_mb);	/* NVMe: 함수/매크로 pci_doe_cancel_tasks()를 호출한다. */
}
