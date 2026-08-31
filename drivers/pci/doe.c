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
 * [한국어 설명] config space 위로 데이터를 주고받는 메시지 통로 (doe.c)
 *
 * === 파일의 역할 ===
 * DOE(Data Object Exchange)는 PCIe 6.0 에서 도입된 확장 capability 로,
 * config space 를 통해 임의 길이의 데이터를 주고받게 해 준다.
 *
 * 왜 이런 것이 필요한가. config space 는 원래 작은 레지스터들의 모음이라
 * 큰 데이터를 옮길 수단이 없다. BAR 를 쓰면 되지만, BAR 는 장치가
 * 초기화되고 메모리 공간이 배정된 뒤에야 쓸 수 있다. 그보다 이른 시점에,
 * 또는 BAR 와 무관하게 장치와 대화해야 하는 경우가 있다 — 대표적으로
 * 보안 관련 협상이다.
 *
 * 동작 방식은 우편함(mailbox)이다. capability 안에 Write Data Mailbox 와
 * Read Data Mailbox 레지스터가 있고, 4바이트씩 밀어 넣고 꺼낸다.
 *   1) 요청 페이로드를 Write Mailbox 에 dword 단위로 쓴다.
 *   2) DOE Go 비트를 세워 "다 썼다" 고 알린다.
 *   3) Data Object Ready 가 설 때까지 기다린다(폴링 또는 인터럽트).
 *   4) Read Mailbox 에서 응답을 dword 단위로 읽는다.
 *
 * 이 파일은 그 절차를 감싸 비동기 작업 큐로 만든다. 요청 하나가
 * struct pci_doe_task 이고, 워크큐 스레드가 그것을 순서대로 처리한다.
 * 응답이 오래 걸릴 수 있어(암호 연산이 끼면 수백 밀리초) 동기 호출로
 * 두면 곤란하기 때문이다.
 *
 * 프로토콜도 여럿이다. 장치가 어떤 프로토콜을 지원하는지 Discovery
 * 프로토콜(vendor 0x0001, type 0x00)로 먼저 물어보고, 그 목록을 캐시해 둔다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 초기화: 열거 시 pci_doe_init()
 *           -> DOE capability 마다 struct pci_doe_mb 를 만들고
 *           -> Discovery 로 지원 프로토콜 목록을 채운다
 *
 * 사용:  CMA/SPDM 이나 CXL 코드가
 *           -> [이 파일] pci_doe_submit_task() 로 요청을 큐에 넣고
 *              -> 워크큐가 pci_doe_task_complete() 까지 처리
 *           또는 pci_doe() 동기 래퍼로 완료까지 기다린다
 *
 * 실행 컨텍스트: 제출은 어디서나, 실제 처리는 워크큐 스레드.
 * 폴링 대기가 있어 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/pci/cma.c 계열(장치 인증), drivers/cxl/ (CXL 의 여러 협상),
 *   그리고 IDE(Integrity and Data Encryption) 설정.
 * 아래쪽: access.c 의 config 접근, 워크큐, xarray(프로토콜 목록 캐시).
 * 공유 상태: struct pci_doe_mb — 우편함 하나. capability 오프셋,
 *   워크큐, 프로토콜 목록(xarray), 그리고 진행 상태를 담는다.
 *   struct pci_dev 의 doe_mbs xarray 에 등록된다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * 다만 NVMe 와 무관하다고 단정하기는 이르다. DOE 는 장치 인증(SPDM)과
 * 링크 암호화(IDE)의 통로이고, 그 기능들은 "신뢰할 수 없는 환경에서
 * 스토리지를 쓴다" 는 요구에서 나왔다. 기밀 컴퓨팅 환경의 NVMe 가
 * 그 대상이 될 수 있다.
 *
 * 현재 커널에서는 CXL 메모리 장치와 일부 가속기가 DOE 를 쓰고,
 * NVMe 는 아직 쓰지 않는다. NVMe 스펙 자체의 보안 기능(TCG Opal 등)은
 * DOE 가 아니라 NVMe 명령으로 이뤄진다(block/opal_proto.h 참고).
 *
 * === 주요 함수/구조체 요약 ===
 * pci_doe_init()            : 장치의 모든 DOE capability 를 찾아 우편함을 만든다.
 * pcim_doe_create_mb()      : 우편함 하나를 만들고 워크큐를 준비한다.
 *                             devres 로 등록해 자동 해제된다.
 * pci_doe_discovery()       : Discovery 프로토콜로 지원 목록을 조회한다.
 * pci_doe_supports_prot()   : 특정 프로토콜을 지원하는지 캐시에서 확인.
 * pci_doe_submit_task()     : 요청을 큐에 넣는다(비동기).
 * pci_doe()                 : 제출하고 완료까지 기다리는 동기 래퍼.
 * pci_doe_send_req()        : Write Mailbox 에 요청을 밀어 넣고 Go 를 세운다.
 * pci_doe_recv_resp()       : Read Mailbox 에서 응답을 꺼낸다.
 * pci_doe_abort()           : 진행 중인 교환을 취소한다. 타임아웃 시 쓴다.
 * struct pci_doe_mb         : 우편함 하나의 상태.
 * struct pci_doe_task       : 요청 하나. 프로토콜, 요청/응답 버퍼,
 *                             완료 콜백을 담는다.
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
#define PCI_DOE_TIMEOUT HZ
#define PCI_DOE_POLL_INTERVAL	(PCI_DOE_TIMEOUT / 128)

#define PCI_DOE_FLAG_CANCEL	0
#define PCI_DOE_FLAG_DEAD	1

/* Max data object length is 2^18 dwords */
#define PCI_DOE_MAX_LENGTH	(1 << 18)

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
struct pci_doe_mb {
	struct pci_dev *pdev;
	u16 cap_offset;
	struct xarray feats;

	wait_queue_head_t wq;
	struct workqueue_struct *work_queue;
	unsigned long flags;

#ifdef CONFIG_SYSFS
	struct device_attribute *sysfs_attrs;
#endif
};

struct pci_doe_feature {
	u16 vid;
	u8 type;
};

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
struct pci_doe_task {
	struct pci_doe_feature feat;
	const __le32 *request_pl;
	size_t request_pl_sz;
	__le32 *response_pl;
	size_t response_pl_sz;
	int rv;
	void (*complete)(struct pci_doe_task *task);
	void *private;

	/* initialized by pci_doe_submit_task() */
	struct work_struct work;
	struct pci_doe_mb *doe_mb;
};

#ifdef CONFIG_SYSFS

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
	return sysfs_emit(buf, "0001:00\n");
}
static DEVICE_ATTR_RO(doe_discovery);

static struct attribute *pci_doe_sysfs_feature_attrs[] = {
	&dev_attr_doe_discovery.attr,
	NULL
};

static bool pci_doe_features_sysfs_group_visible(struct kobject *kobj)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));

	return !xa_empty(&pdev->doe_mbs);
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(pci_doe_features_sysfs)

const struct attribute_group pci_doe_sysfs_group = {
	.name	    = "doe_features",
	.attrs	    = pci_doe_sysfs_feature_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_doe_features_sysfs),
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
	return sysfs_emit(buf, "%s\n", attr->attr.name);
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
	struct device_attribute *attrs = doe_mb->sysfs_attrs;
	struct device *dev = &pdev->dev;
	unsigned long i;
	void *entry;

	if (!attrs)
		return;

	doe_mb->sysfs_attrs = NULL;
	xa_for_each(&doe_mb->feats, i, entry) {
		if (attrs[i].show)
			sysfs_remove_file_from_group(&dev->kobj, &attrs[i].attr,
						     pci_doe_sysfs_group.name);
		kfree(attrs[i].attr.name);
	}
	kfree(attrs);
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
	struct device *dev = &pdev->dev;
	struct device_attribute *attrs;
	unsigned long num_features = 0;
	unsigned long vid, type;
	unsigned long i;
	void *entry;
	int ret;

	xa_for_each(&doe_mb->feats, i, entry)
		num_features++;

	attrs = kzalloc_objs(*attrs, num_features);
	if (!attrs) {
		pci_warn(pdev, "Failed allocating the device_attribute array\n");
		return -ENOMEM;
	}

	doe_mb->sysfs_attrs = attrs;
	xa_for_each(&doe_mb->feats, i, entry) {
		sysfs_attr_init(&attrs[i].attr);
		vid = xa_to_value(entry) >> 8;
		type = xa_to_value(entry) & 0xFF;

		if (vid == PCI_VENDOR_ID_PCI_SIG &&
		    type == PCI_DOE_FEATURE_DISCOVERY) {

			/*
			 * DOE Discovery, manually displayed by
			 * `dev_attr_doe_discovery`
			 */
			continue;
		}

		attrs[i].attr.name = kasprintf(GFP_KERNEL,
					       "%04lx:%02lx", vid, type);
		if (!attrs[i].attr.name) {
			ret = -ENOMEM;
			pci_warn(pdev, "Failed allocating the attribute name\n");
			goto fail;
		}

		attrs[i].attr.mode = 0444;
		attrs[i].show = pci_doe_sysfs_feature_show;

		ret = sysfs_add_file_to_group(&dev->kobj, &attrs[i].attr,
					      pci_doe_sysfs_group.name);
		if (ret) {
			attrs[i].show = NULL;
			if (ret != -EEXIST) {
				pci_warn(pdev, "Failed adding %s to sysfs group\n",
					 attrs[i].attr.name);
				goto fail;
			} else
				kfree(attrs[i].attr.name);
		}
	}

	return 0;

fail:
	pci_doe_sysfs_feature_remove(pdev, doe_mb);
	return ret;
}


/*
 * pci_doe_sysfs_teardown:
 *   NVMe 장치에 연결된 모든 DOE mailbox의 sysfs 항목을 제거한다.
 *   장치 제거 단계에서 사용자공간이 DOE feature 정보를 더 이상 볼 수 없도록
 *   정리한다.
 */
void pci_doe_sysfs_teardown(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		pci_doe_sysfs_feature_remove(pdev, doe_mb);
}


/*
 * pci_doe_sysfs_init:
 *   NVMe 장치 probe 시 각 DOE mailbox에 대해 sysfs feature 파일을 생성한다.
 *   생성된 /sys/bus/pci/devices/.../doe_features 항목은 nvme-cli 등이
 *   NVMe 컨트롤러의 DOE capability를 확인하는 데 사용된다.
 */
void pci_doe_sysfs_init(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;
	int ret;

	xa_for_each(&pdev->doe_mbs, index, doe_mb) {
		ret = pci_doe_sysfs_feature_populate(pdev, doe_mb);
		if (ret)
			return;
	}
}
#endif


/*
 * pci_doe_wait:
 *   NVMe 장치의 DOE mailbox에서 취소 플래그가 설정될 때까지 대기한다.
 *   DOE 상태 머신이 폴링 중 firmware나 다른 주체에 의한 충돌을 감지하면
 *   작업을 중단하기 위해 이 대기를 사용한다.
 */
static int pci_doe_wait(struct pci_doe_mb *doe_mb, unsigned long timeout)
{
	if (wait_event_timeout(doe_mb->wq,
			       test_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags),
			       timeout))
		return -EIO;
	return 0;
}


/*
 * pci_doe_write_ctrl:
 *   NVMe 장치의 DOE 제어 레지스터(PCI_DOE_CTRL)에 값을 기록한다.
 *   GO/ABORT 비트를 설정하여 DOE 데이터 객체 교환을 시작하거나 중단한다.
 */
static void pci_doe_write_ctrl(struct pci_doe_mb *doe_mb, u32 val)
{
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;

	pci_write_config_dword(pdev, offset + PCI_DOE_CTRL, val);
}


/*
 * pci_doe_abort:
 *   NVMe 장치의 DOE mailbox에 abort를 발행하여 상태 머신을 리셋한다.
 *   probe 시 메일박스 초기화는 물론, 장치 분리(hotplug)나 오류 복구 시
 *   진행 중인 DOE 교환을 정리하는 데 필수적이다.
 */
static int pci_doe_abort(struct pci_doe_mb *doe_mb)
{
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	unsigned long timeout_jiffies;

	pci_dbg(pdev, "[%x] Issuing Abort\n", offset);

	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;
	pci_doe_write_ctrl(doe_mb, PCI_DOE_CTRL_ABORT);

	do {
		int rc;
		u32 val;

		rc = pci_doe_wait(doe_mb, PCI_DOE_POLL_INTERVAL);
		if (rc)
			return rc;
		pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);

		/* Abort success! */
		if (!FIELD_GET(PCI_DOE_STATUS_ERROR, val) &&
		    !FIELD_GET(PCI_DOE_STATUS_BUSY, val))
			return 0;

	} while (!time_after(jiffies, timeout_jiffies));

	/* Abort has timed out and the MB is dead */
	pci_err(pdev, "[%x] ABORT timed out\n", offset);
	return -EIO;
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
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	unsigned long timeout_jiffies;
	size_t length, remainder;
	u32 val;
	int i;

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
	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;
	do {
		pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
	} while (FIELD_GET(PCI_DOE_STATUS_BUSY, val) &&
		 !time_after(jiffies, timeout_jiffies));

	if (FIELD_GET(PCI_DOE_STATUS_BUSY, val))
		return -EBUSY;

	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val))
		return -EIO;

	/* Length is 2 DW of header + length of payload in DW */
	length = 2 + DIV_ROUND_UP(task->request_pl_sz, sizeof(__le32));
	if (length > PCI_DOE_MAX_LENGTH)
		return -EIO;
	if (length == PCI_DOE_MAX_LENGTH)
		length = 0;

	/* Write DOE Header */
	val = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_VID, task->feat.vid) |
		FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, task->feat.type);
	pci_write_config_dword(pdev, offset + PCI_DOE_WRITE, val);
	pci_write_config_dword(pdev, offset + PCI_DOE_WRITE,
			       FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH,
					  length));

	/* Write payload */
	for (i = 0; i < task->request_pl_sz / sizeof(__le32); i++)	/* [한국어] 페이로드를 dword 단위로 순회한다 — DOE 는 4바이트 단위로만 주고받는다 */
		pci_write_config_dword(pdev, offset + PCI_DOE_WRITE,
				       le32_to_cpu(task->request_pl[i]));

	/* Write last payload dword */
	remainder = task->request_pl_sz % sizeof(__le32);
	if (remainder) {
		val = 0;
		memcpy(&val, &task->request_pl[i], remainder);
		le32_to_cpus(&val);
		pci_write_config_dword(pdev, offset + PCI_DOE_WRITE, val);
	}

	pci_doe_write_ctrl(doe_mb, PCI_DOE_CTRL_GO);

	return 0;
}


/*
 * pci_doe_data_obj_ready:
 *   NVMe 장치가 DOE 응답 데이터 객체를 준비했는지 PCI_DOE_STATUS 레지스터의
 *   Data Object Ready 비트를 읽어 확인한다.
 */
static bool pci_doe_data_obj_ready(struct pci_doe_mb *doe_mb)
{
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	u32 val;

	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
	if (FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val))
		return true;
	return false;
}


/*
 * pci_doe_recv_resp:
 *   NVMe 장치의 DOE mailbox 읽기 포트에서 응답 데이터 객체를 수신한다.
 *   헤더(VID/type/length)를 검증하고, 사용자가 요청한 크기만큼 payload를
 *   복사하며 초과 데이터는 flush한다.
 */
static int pci_doe_recv_resp(struct pci_doe_mb *doe_mb, struct pci_doe_task *task)
{
	size_t length, payload_length, remainder, received;
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	int i = 0;
	u32 val;

	/* Read the first dword to get the feature */
	pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
	if ((FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, val) != task->feat.vid) ||
	    (FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, val) != task->feat.type)) {
		dev_err_ratelimited(&pdev->dev, "[%x] expected [VID, Feature] = [%04x, %02x], got [%04x, %02x]\n",
				    doe_mb->cap_offset, task->feat.vid, task->feat.type,
				    FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, val),
				    FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, val));
		return -EIO;
	}

	pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
	/* Read the second dword to get the length */
	pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
	pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);

	length = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, val);
	/* A value of 0x0 indicates max data object length */
	if (!length)
		length = PCI_DOE_MAX_LENGTH;
	if (length < 2)
		return -EIO;

	/* First 2 dwords have already been read */
	length -= 2;
	received = task->response_pl_sz;
	payload_length = DIV_ROUND_UP(task->response_pl_sz, sizeof(__le32));
	remainder = task->response_pl_sz % sizeof(__le32);

	/* remainder signifies number of data bytes in last payload dword */
	if (!remainder)
		remainder = sizeof(__le32);

	if (length < payload_length) {
		received = length * sizeof(__le32);
		payload_length = length;
		remainder = sizeof(__le32);
	}

	if (payload_length) {
		/* Read all payload dwords except the last */
		for (; i < payload_length - 1; i++) {	/* [한국어] 페이로드를 dword 단위로 순회한다 — DOE 는 4바이트 단위로만 주고받는다 */
			pci_read_config_dword(pdev, offset + PCI_DOE_READ,
					      &val);
			task->response_pl[i] = cpu_to_le32(val);
			pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
		}

		/* Read last payload dword */
		pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
		cpu_to_le32s(&val);
		memcpy(&task->response_pl[i], &val, remainder);
		/* Prior to the last ack, ensure Data Object Ready */
		if (!pci_doe_data_obj_ready(doe_mb))
			return -EIO;
		pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
		i++;
	}

	/* Flush excess length */
	for (; i < length; i++) {	/* [한국어] 페이로드를 dword 단위로 순회한다 — DOE 는 4바이트 단위로만 주고받는다 */
		pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
		pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
	}

	/* Final error check to pick up on any since Data Object Ready */
	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val))
		return -EIO;

	return received;
}


/*
 * signal_task_complete:
 *   NVMe 측에 제출된 DOE 태스크의 결과를 기록하고 완료 콜백을 호출한다.
 *   동기 호출이라면 completion을 wake하여 NVMe 호출자가 깨어나게 한다.
 */
static void signal_task_complete(struct pci_doe_task *task, int rv)
{
	task->rv = rv;
	destroy_work_on_stack(&task->work);
	task->complete(task);
}


/*
 * signal_task_abort:
 *   DOE 태스크가 실패했을 때 NVMe 장치에 abort를 시도하고, abort마저
 *   실패하면 해당 mailbox를 dead로 표시한다. 이후 새로운 DOE 요청은
 *   차단되어 NVMe 호스트가 깨진 mailbox를 계속 사용하지 않도록 한다.
 */
static void signal_task_abort(struct pci_doe_task *task, int rv)
{
	struct pci_doe_mb *doe_mb = task->doe_mb;
	struct pci_dev *pdev = doe_mb->pdev;

	if (pci_doe_abort(doe_mb)) {
		/*
		 * If the device can't process an abort; set the mailbox dead
		 *	- no more submissions
		 */
		pci_err(pdev, "[%x] Abort failed marking mailbox dead\n",
			doe_mb->cap_offset);
		set_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags);
	}
	signal_task_complete(task, rv);
}


/*
 * doe_statemachine_work:
 *   NVMe용 DOE 태스크를 비동기적으로 처리하는 상태 머신 work 함수.
 *   요청 전송 -> 응답 폴링 -> 응답 수신 -> 완료 시그널의 전체 흐름을
 *   담당하며, timeout/오류 시 abort를 수행한다.
 */
static void doe_statemachine_work(struct work_struct *work)
{
	struct pci_doe_task *task = container_of(work, struct pci_doe_task,
						 work);
	struct pci_doe_mb *doe_mb = task->doe_mb;
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	unsigned long timeout_jiffies;
	u32 val;
	int rc;

	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags)) {
		signal_task_complete(task, -EIO);
		return;
	}

	/* Send request */
	rc = pci_doe_send_req(doe_mb, task);
	if (rc) {
		/*
		 * The specification does not provide any guidance on how to
		 * resolve conflicting requests from other entities.
		 * Furthermore, it is likely that busy will not be detected
		 * most of the time.  Flag any detection of status busy with an
		 * error.
		 */
		if (rc == -EBUSY)
			dev_err_ratelimited(&pdev->dev, "[%x] busy detected; another entity is sending conflicting requests\n",
					    offset);
		signal_task_abort(task, rc);
		return;
	}

	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;
	/* Poll for response */
retry_resp:
	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {
		signal_task_abort(task, -EIO);
		return;
	}

	if (!FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val)) {
		if (time_after(jiffies, timeout_jiffies)) {
			signal_task_abort(task, -EIO);
			return;
		}
		rc = pci_doe_wait(doe_mb, PCI_DOE_POLL_INTERVAL);
		if (rc) {
			signal_task_abort(task, rc);
			return;
		}
		goto retry_resp;
	}

	rc  = pci_doe_recv_resp(doe_mb, task);
	if (rc < 0) {
		signal_task_abort(task, rc);
		return;
	}

	signal_task_complete(task, rc);
}


/*
 * pci_doe_task_complete:
 *   동기 방식 pci_doe() 호출 시 사용하는 낮은 수준 완료 콜백.
 *   NVMe 호스트가 wait_for_completion()으로 대기 중인 completion 객체를
 *   시그널링하여 결과를 반환한다.
 */
static void pci_doe_task_complete(struct pci_doe_task *task)
{
	complete(task->private);
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
	u32 request_pl = FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_REQ_3_INDEX,
				    *index) |
			 FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_REQ_3_VER,
				    (capver >= 2) ? 2 : 0);
	__le32 request_pl_le = cpu_to_le32(request_pl);
	__le32 response_pl_le;
	u32 response_pl;
	int rc;

	rc = pci_doe(doe_mb, PCI_VENDOR_ID_PCI_SIG, PCI_DOE_FEATURE_DISCOVERY,
		     &request_pl_le, sizeof(request_pl_le),
		     &response_pl_le, sizeof(response_pl_le));
	if (rc < 0)
		return rc;

	if (rc != sizeof(response_pl_le))
		return -EIO;

	response_pl = le32_to_cpu(response_pl_le);
	*vid = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_VID, response_pl);
	*feature = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_TYPE,
			      response_pl);
	*index = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_NEXT_INDEX,
			   response_pl);

	return 0;
}


/*
 * pci_doe_xa_feat_entry:
 *   NVMe 장치의 DOE feature 식별자(vendor ID + type)를 xarray에 저장할 값으로
 *   인코딩한다.
 */
static void *pci_doe_xa_feat_entry(u16 vid, u8 type)
{
	return xa_mk_value((vid << 8) | type);
}


/*
 * pci_doe_cache_features:
 *   NVMe 장치 probe 시 DOE Discovery를 반복 수행하여 지원 feature 목록을
 *   xarray에 캐시한다. 이 목록은 이후 NVMe 관련 코드가 DOE feature 사용 전
 *   pci_doe_supports_feat()로 지원 여부를 빠르게 확인하는 데 쓰인다.
 */
static int pci_doe_cache_features(struct pci_doe_mb *doe_mb)
{
	u8 index = 0;
	u8 xa_idx = 0;
	u32 hdr = 0;

	pci_read_config_dword(doe_mb->pdev, doe_mb->cap_offset, &hdr);

	do {
		int rc;
		u16 vid;
		u8 type;

		rc = pci_doe_discovery(doe_mb, PCI_EXT_CAP_VER(hdr), &index,
				       &vid, &type);
		if (rc)
			return rc;

		pci_dbg(doe_mb->pdev,
			"[%x] Found feature %d vid: %x type: %x\n",
			doe_mb->cap_offset, xa_idx, vid, type);

		rc = xa_insert(&doe_mb->feats, xa_idx++,
			       pci_doe_xa_feat_entry(vid, type), GFP_KERNEL);
		if (rc)
			return rc;
	} while (index);

	return 0;
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
	set_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags);

	/* Cancel an in progress work item, if necessary */
	set_bit(PCI_DOE_FLAG_CANCEL, &doe_mb->flags);
	wake_up(&doe_mb->wq);
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
	struct pci_doe_mb *doe_mb;
	int rc;

	doe_mb = kzalloc_obj(*doe_mb);
	if (!doe_mb)
		return ERR_PTR(-ENOMEM);

	doe_mb->pdev = pdev;
	doe_mb->cap_offset = cap_offset;
	init_waitqueue_head(&doe_mb->wq);
	xa_init(&doe_mb->feats);

	doe_mb->work_queue = alloc_ordered_workqueue("%s %s DOE [%x]", 0,
						dev_bus_name(&pdev->dev),
						pci_name(pdev),
						doe_mb->cap_offset);
	if (!doe_mb->work_queue) {
		pci_err(pdev, "[%x] failed to allocate work queue\n",
			doe_mb->cap_offset);
		rc = -ENOMEM;
		goto err_free;
	}

	/* Reset the mailbox by issuing an abort */
	rc = pci_doe_abort(doe_mb);
	if (rc) {
		pci_err(pdev, "[%x] failed to reset mailbox with abort command : %d\n",
			doe_mb->cap_offset, rc);
		goto err_destroy_wq;
	}

	/*
	 * The state machine and the mailbox should be in sync now;
	 * Use the mailbox to query features.
	 */
	rc = pci_doe_cache_features(doe_mb);
	if (rc) {
		pci_err(pdev, "[%x] failed to cache features : %d\n",
			doe_mb->cap_offset, rc);
		goto err_cancel;
	}

	return doe_mb;

err_cancel:
	pci_doe_cancel_tasks(doe_mb);
	xa_destroy(&doe_mb->feats);
err_destroy_wq:
	destroy_workqueue(doe_mb->work_queue);
err_free:
	kfree(doe_mb);
	return ERR_PTR(rc);
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
	pci_doe_cancel_tasks(doe_mb);
	xa_destroy(&doe_mb->feats);
	destroy_workqueue(doe_mb->work_queue);
	kfree(doe_mb);
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
	unsigned long index;
	void *entry;

	/* The discovery feature must always be supported */
	if (vid == PCI_VENDOR_ID_PCI_SIG && type == PCI_DOE_FEATURE_DISCOVERY)
		return true;

	xa_for_each(&doe_mb->feats, index, entry)
		if (entry == pci_doe_xa_feat_entry(vid, type))
			return true;

	return false;
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
	if (!pci_doe_supports_feat(doe_mb, task->feat.vid, task->feat.type))
		return -EINVAL;

	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags))
		return -EIO;

	task->doe_mb = doe_mb;
	INIT_WORK_ONSTACK(&task->work, doe_statemachine_work);
	queue_work(doe_mb->work_queue, &task->work);
	return 0;
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
	DECLARE_COMPLETION_ONSTACK(c);
	struct pci_doe_task task = {
		.feat.vid = vendor,
		.feat.type = type,
		.request_pl = request,
		.request_pl_sz = request_sz,
		.response_pl = response,
		.response_pl_sz = response_sz,
		.complete = pci_doe_task_complete,
		.private = &c,
	};
	int rc;

	rc = pci_doe_submit_task(doe_mb, &task);
	if (rc)
		return rc;

	wait_for_completion(&c);

	return task.rv;
}
EXPORT_SYMBOL_GPL(pci_doe);

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
	struct pci_doe_mb *doe_mb;
	unsigned long index;

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		if (pci_doe_supports_feat(doe_mb, vendor, type))
			return doe_mb;

	return NULL;
}
EXPORT_SYMBOL_GPL(pci_find_doe_mailbox);


/*
 * pci_doe_init:
 *   NVMe 장치 probe 시 PCI core가 호출하여 모든 DOE 확장 capability를
 *   찾고 각각에 대한 mailbox를 생성한다. 생성된 mailbox는 sysfs를 통해
 *   NVMe 사용자공간 도구에 노출될 수 있다.
 */
void pci_doe_init(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	u16 offset = 0;
	int rc;

	xa_init(&pdev->doe_mbs);

	while ((offset = pci_find_next_ext_capability(pdev, offset,
						      PCI_EXT_CAP_ID_DOE))) {
		doe_mb = pci_doe_create_mb(pdev, offset);
		if (IS_ERR(doe_mb)) {
			pci_err(pdev, "[%x] failed to create mailbox: %ld\n",
				offset, PTR_ERR(doe_mb));
			continue;
		}

		rc = xa_insert(&pdev->doe_mbs, offset, doe_mb, GFP_KERNEL);
		if (rc) {
			pci_err(pdev, "[%x] failed to insert mailbox: %d\n",
				offset, rc);
			pci_doe_destroy_mb(doe_mb);
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
	struct pci_doe_mb *doe_mb;
	unsigned long index;

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		pci_doe_destroy_mb(doe_mb);

	xa_destroy(&pdev->doe_mbs);
}


/*
 * pci_doe_disconnected:
 *   NVMe 장치가 hotplug 등으로 연결이 끊어졌을 때 모든 DOE mailbox의
 *   pending/in-progress 작업을 즉시 취소한다. 장치가 응답하지 않는 상황에서
 *   무한 대기를 방지한다.
 */
void pci_doe_disconnected(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		pci_doe_cancel_tasks(doe_mb);
}
