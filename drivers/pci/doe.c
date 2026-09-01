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
 * === 주요 함수/구조체 요약 ===
 * pci_doe_init()            : 장치의 모든 DOE capability 를 찾아 우편함을 만든다.
 * pci_doe_create_mb()       : 우편함 하나를 만들고 전용 ordered 워크큐를
 *                             준비한 뒤 Abort 로 비우고 기능 목록을 캐시한다.
 *                             **devres 를 쓰지 않는다** -- 이 파일에 devm_
 *                             호출이 한 건도 없고, 해제는 pci_doe_destroy()
 *                             가 명시적으로 한다.
 * pci_doe_destroy_mb()      : 그 반대. cancel_tasks 를 먼저 불러야
 *                             destroy_workqueue 가 매달리지 않는다.
 * pci_doe_discovery()       : Discovery 기능으로 다음 기능 하나를 조회한다.
 * pci_doe_cache_features()  : 그것을 반복해 기능 목록을 xarray 에 채운다.
 * pci_doe_supports_feat()   : 특정 기능을 지원하는지 캐시에서 확인한다.
 *                             Discovery 자체는 캐시와 무관하게 항상 참이다.
 * pci_doe_submit_task()     : 요청을 우편함 전용 큐에 넣는다. static 이며
 *                             이 파일 안에서 pci_doe() 만 부른다.
 * pci_doe()                 : 제출하고 완료까지 기다리는 동기 API.
 *                             이 파일의 유일한 전송 진입점이며 EXPORT 된다.
 * pci_find_doe_mailbox()    : 그 기능을 지원하는 우편함을 찾아 준다.
 *                             EXPORT 되며, 이 트리의 소비자는 tsm.c 다.
 * doe_statemachine_work()   : 전송·대기·수신의 전 과정을 도는 워커.
 * pci_doe_send_req()        : WRITE 레지스터에 요청을 밀어 넣고 GO 를 세운다.
 * pci_doe_recv_resp()       : READ 레지스터에서 응답을 꺼낸다. 워드마다
 *                             읽고 0 을 써야 다음 워드가 올라온다.
 * pci_doe_abort()           : 우편함을 비운다. 생성 시와 작업 실패 시에 쓴다.
 * pci_doe_cancel_tasks()    : DEAD/CANCEL 을 세워 대기 중인 폴링을 끊는다.
 * struct pci_doe_mb         : 우편함 하나의 상태. 전용 워크큐가 **ordered**
 *                             라 작업이 하나씩만 돌고, 그것이 이 파일에
 *                             우편함 잠금이 없는 근거다.
 * struct pci_doe_task       : 요청 하나. 기능, 요청/응답 버퍼, 완료 콜백을
 *                             담으며 **호출자 스택에** 놓인다.
 *
 * === NVMe 관점 (필수 4섹션에 대한 부가 절) ===
 * drivers/nvme 는 이 파일의 어떤 심볼도 호출하지 않는다(이 트리에서 확인).
 * 이 트리에서 pci_doe()/pci_find_doe_mailbox() 를 부르는 곳은
 * drivers/pci/tsm.c 하나다(pci_tsm_doe_transfer, pci_tsm_link_constructor).
 *
 * DOE 는 장치 인증(SPDM)과 링크 암호화(IDE)가 오가는 통로이고, 그 기능들은
 * "신뢰할 수 없는 환경에서 장치를 쓴다" 는 요구에서 나왔다. 기밀 컴퓨팅
 * 환경의 스토리지가 장차 그 대상이 될 수 있지만, **현재 이 트리에서는
 * NVMe 가 DOE 를 쓰지 않는다.** 앞질러 추측하지 않고 사실만 적어 둔다.
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
	/* [한국어] 이 우편함의 DOE 확장 능력 오프셋.
	 * 설정자: pci_doe_create_mb.
	 * 읽는 자: 모든 설정공간 접근이 이 값에 레지스터 오프셋을 더한다.
	 * 값 범위: 0x100 이상의 확장 설정공간 오프셋. 로그 태그로도 쓰인다.
	 * 동기화: 생성 후 불변. */
	u16 cap_offset;
	/* [한국어] 이 우편함이 지원하는 기능 목록. 키는 0 부터의 연속 인덱스, 값은
	 * pci_doe_xa_feat_entry() 가 접은 (vid << 8) | type 이다.
	 * 설정자: pci_doe_cache_features 가 프로브 때 한 번 채운다.
	 * 읽는 자: supports_feat 의 탐색, sysfs populate/remove 의 순회.
	 * 값 범위: 항목이 xa_mk_value 로 만든 값이라 별도 할당이 없다.
	 * 동기화: 생성 후 읽기 전용이라 잠금이 필요 없다. */
	struct xarray feats;
/* [한국어] 아래 세 필드가 작업 실행과 취소를 담당한다. */

	wait_queue_head_t wq;
	/* [한국어] 이 우편함 전용 ordered 작업 큐.
	 * 설정자: create_mb 의 alloc_ordered_workqueue.
	 * 읽는 자: submit_task 의 queue_work, destroy_mb 의 destroy_workqueue.
	 * 값 범위: 유효한 큐.
	 * 동기화: **ordered 라 한 번에 하나만 실행된다** -- 이 파일에 우편함 잠금이
	 * 없는 근거가 바로 이것이다. */
	struct workqueue_struct *work_queue;
	/* [한국어] PCI_DOE_FLAG_CANCEL(0) 과 PCI_DOE_FLAG_DEAD(1) 비트.
	 * 설정자: pci_doe_cancel_tasks 가 둘 다, signal_task_abort 가 DEAD 만.
	 * 읽는 자: pci_doe_wait 의 대기 조건, submit_task 와 워커의 초입 검사.
	 * 값 범위: 두 비트만 쓴다.
	 * 동기화: set_bit/test_bit 의 원자적 연산. 별도 잠금 없이 안전하다. */
	unsigned long flags;
/* [한국어] 아래는 CONFIG_SYSFS 일 때만 존재하는 필드다. */

#ifdef CONFIG_SYSFS
	struct device_attribute *sysfs_attrs;
/* [한국어] CONFIG_SYSFS 분기 끝. 이 필드가 없으면 sysfs 관련 함수도 함께 빠진다. */
#endif
};

struct pci_doe_feature {
	/* [한국어] 기능의 벤더 ID(16비트). PCI-SIG 표준 기능은 PCI_VENDOR_ID_PCI_SIG 다.
	 * 설정자: pci_doe() 호출자 또는 discovery 응답.
	 * 읽는 자: 데이터 오브젝트 헤더 조립과 응답 검증.
	 * 동기화: 값 복사본이라 경쟁 없음. */
	u16 vid;
	/* [한국어] 기능 타입(8비트). 벤더 안에서 기능을 구별한다.
	 * Discovery 는 PCI_DOE_FEATURE_DISCOVERY 로 고정이다. */
	u8 type;
/* [한국어] 이 두 값이 DOE 메시지의 주소 역할을 한다 -- 어느 기능에 보낼지 정한다. */
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
	/* [한국어] 보낼 페이로드. **헤더 두 워드는 포함하지 않는다** -- 그것은
	 * pci_doe_send_req 가 붙인다.
	 * 설정자: pci_doe() 가 호출자 버퍼를 그대로 가리킨다.
	 * 읽는 자: pci_doe_send_req.
	 * 값 범위: __le32 배열. const 라 이 파일이 내용을 바꾸지 않는다.
	 * 동기화: 작업이 큐에 있는 동안 호출자가 건드리면 안 된다 -- pci_doe() 가
	 * 완료를 기다리므로 동기 사용에서는 성립한다. */
	const __le32 *request_pl;
	/* [한국어] 그 크기(바이트). **4의 배수가 아니어도 된다** -- 남는 바이트는
	 * send_req 가 0 으로 채운 워드에 담아 보낸다. */
	size_t request_pl_sz;
	/* [한국어] 응답을 담을 버퍼.
	 * 설정자: pci_doe() 가 호출자 버퍼를 가리킨다.
	 * 읽는 자/쓰는 자: pci_doe_recv_resp 가 여기에 채운다.
	 * 동기화: 위와 같다. */
	__le32 *response_pl;
	/* [한국어] 그 크기. 실제 응답이 더 짧으면 그만큼만 채워지고, 더 길면 남는 부분은
	 * recv_resp 가 읽어서 버린다. */
	size_t response_pl_sz;
	/* [한국어] 작업 결과.
	 * 설정자: signal_task_complete.
	 * 읽는 자: pci_doe() 가 완료를 기다린 뒤 읽는다.
	 * 값 범위: 양수는 응답 바이트 수, 음수는 오류.
	 * 동기화: complete() 가 메모리 배리어 역할을 해, 대기자가 반드시 최신 값을 본다. */
	int rv;
	/* [한국어] 완료 콜백. 동기 API 에서는 pci_doe_task_complete 가 들어간다.
	 * 이 간접층 덕에 나중에 비동기 API 를 붙일 때 이 필드만 바꾸면 된다. */
	void (*complete)(struct pci_doe_task *task);
	/* [한국어] 콜백에 넘길 문맥. 동기 API 에서는 스택의 completion 을 가리킨다. */
	void *private;
/* [한국어] 아래 두 필드는 이 계층이 내부적으로 쓴다. */

	/* initialized by pci_doe_submit_task() */
	struct work_struct work;
	struct pci_doe_mb *doe_mb;
/* [한국어] 이 구조체 전체가 **호출자 스택에** 놓인다 -- 그래서 work 를
 * INIT_WORK_ONSTACK 으로 초기화하고 destroy_work_on_stack 으로 정리한다. */
};

#ifdef CONFIG_SYSFS

/*
 * doe_discovery_show:
 *   NVMe SSD의 doe_features sysfs 디렉터리 아래 "0001:00" 항목을 노출한다.
 *   PCI-SIG DOE Discovery feature를 통해 사용자공간(nvme-cli 등)에서
 *   장치가 지원하는 DOE feature 목록을 확인할 수 있게 한다.
 */
/* [한국어]
 * doe_discovery_show - sysfs 에서 Discovery 기능의 고정 식별자를 보여 준다
 *
 * @dev, @attr: sysfs 규약 인자. 쓰지 않는다.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * "0001:00" 은 [벤더 ID]:[기능 타입] 형식이고, 0001 은 PCI-SIG,
 * 00 은 Discovery 기능을 뜻한다. 이 값이 **상수로 박혀 있는 이유**는
 * Discovery 가 모든 DOE 우편함이 반드시 지원하는 기능이라, 열거 결과를
 * 볼 필요 없이 항상 존재하기 때문이다.
 *
 * 그래서 pci_doe_sysfs_feature_populate() 는 열거 결과에서 Discovery 를
 * 만나면 파일을 따로 만들지 않고 건너뛴다 -- 이 정적 속성이 이미 그 자리를
 * 차지하고 있다.
 *
 * 실행 컨텍스트: sysfs 읽기의 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /sys/.../doe_features/0001:00 → sysfs → [이 함수]
 */
static ssize_t doe_discovery_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	return sysfs_emit(buf, "0001:00\n");
}
/* [한국어] DEVICE_ATTR_RO 매크로가 struct device_attribute dev_attr_doe_discovery
 * 를 만든다. _RO 는 모드 0444(모두 읽기 전용)에 show 만 있는 속성이라는 뜻이고,
 * show 함수 이름은 매크로가 `doe_discovery_show` 로 유도한다 -- 그래서 위
 * 함수 이름이 그 규약을 따라야 한다.
 * 이 속성만 정적으로 만드는 이유: Discovery 기능은 모든 DOE 우편함이 반드시
 * 지원하므로 열거 결과를 볼 필요가 없다. 나머지 기능 파일은
 * pci_doe_sysfs_feature_populate() 가 실행 시점에 만든다. */
static DEVICE_ATTR_RO(doe_discovery);

static struct attribute *pci_doe_sysfs_feature_attrs[] = {
	&dev_attr_doe_discovery.attr,
	NULL
};

/* [한국어]
 * pci_doe_features_sysfs_group_visible - doe_features 디렉터리를 보일지 정한다
 *
 * @kobj: 이 장치의 kobject.
 * @return: true 면 디렉터리를 만든다.
 *
 * DOE 우편함이 하나도 없는 장치에 빈 doe_features 디렉터리를 만들지 않기
 * 위한 판정이다. pdev->doe_mbs xarray 가 비었는지만 본다.
 *
 * DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE 매크로가 이 함수를 sysfs 가 요구하는
 * 형태(속성별 판정까지 포함)로 감싸 준다.
 *
 * 실행 컨텍스트: sysfs 그룹 생성 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   sysfs 코어 → group.is_visible → [이 함수]
 */
static bool pci_doe_features_sysfs_group_visible(struct kobject *kobj)
{
	struct pci_dev *pdev = to_pci_dev(kobj_to_dev(kobj));

	return !xa_empty(&pdev->doe_mbs);
/* [한국어] Discovery 는 정적 속성이므로 열거 결과와 무관하게 항상 이 배열에 있다. */
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(pci_doe_features_sysfs)

const struct attribute_group pci_doe_sysfs_group = {
	/* [한국어] sysfs 에서 이 그룹이 만들 디렉터리 이름. */
	.name	    = "doe_features",
	/* [한국어] 그 디렉터리에 처음부터 들어갈 속성 목록(Discovery 하나뿐). 나머지 기능
	 * 파일은 populate 가 sysfs_add_file_to_group 으로 나중에 붙인다. */
	.attrs	    = pci_doe_sysfs_feature_attrs,
	.is_visible = SYSFS_GROUP_VISIBLE(pci_doe_features_sysfs),
};


/*
 * pci_doe_sysfs_feature_show:
 *   NVMe 장치의 개별 DOE feature 이름을 sysfs에 출력한다.
 *   사용자공간에서 /sys/bus/pci/devices/.../doe_features/ 아래 파일로
 *   NVMe 컨트롤러가 지원하는 DOE 기능을 확인하는 데 사용된다.
 */
/* [한국어]
 * pci_doe_sysfs_feature_show - 기능 파일을 읽으면 자기 이름을 그대로 돌려준다
 *
 * @dev: 대상 장치. 쓰지 않는다.
 * @attr: 이 속성. 이름을 여기서 꺼낸다.
 * @buf: 출력 버퍼.
 * @return: 쓴 바이트 수.
 *
 * 파일 이름이 곧 정보인 구조다. populate 가 "%04lx:%02lx" 형식으로
 * [벤더]:[타입] 이름을 만들어 파일을 세우므로, 내용은 이름을 되풀이할
 * 뿐이다. 그래도 파일에 내용이 있어야 `grep -r . doe_features/` 같은
 * 사용법이 동작한다.
 *
 * 실행 컨텍스트: sysfs 읽기의 프로세스 문맥.
 *
 * 호출 체인:
 *   sysfs 읽기 → attr->show → [이 함수]
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
/* [한국어]
 * pci_doe_sysfs_feature_remove - 이 우편함이 만든 기능 파일과 이름 문자열을 모두 해제한다
 *
 * @pdev: 대상 장치.
 * @doe_mb: 대상 우편함.
 * @return: 없음.
 *
 * populate 의 정확한 역순이다. 두 가지를 되돌린다: sysfs 파일과 kasprintf
 * 로 잡은 이름 문자열.
 *
 * `if (attrs[i].show)` 검사가 요점이다. populate 는 파일 추가에 실패했을
 * 때 show 를 NULL 로 되돌려 놓으므로, 그 표시를 보고 **실제로 만들어진
 * 파일만** 제거한다. 이름 문자열은 그와 무관하게 항상 해제한다 --
 * -EEXIST 경로에서 이미 해제한 경우에는 kfree(NULL) 이 되어 무해하다.
 *
 * 맨 앞의 `if (!attrs) return` 은 populate 가 아예 불리지 않았거나 이미
 * 정리된 경우를 거른다. 그 직후 sysfs_attrs 를 NULL 로 만들어 두어
 * **두 번 불려도 안전하다** -- populate 의 실패 경로와 teardown 이 모두
 * 이 함수를 부를 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_doe_sysfs_feature_populate(실패) / pci_doe_sysfs_teardown
 *     → [이 함수] → sysfs_remove_file_from_group → kfree
 */
static void pci_doe_sysfs_feature_remove(struct pci_dev *pdev,
					 struct pci_doe_mb *doe_mb)
{
	struct device_attribute *attrs = doe_mb->sysfs_attrs;
	struct device *dev = &pdev->dev;
	/* [한국어] xarray 인덱스이자 attrs 배열 인덱스. 두 인덱스가 같다는 전제가
	 * populate 와 공유된다. */
	unsigned long i;
	/* [한국어] xa_for_each 가 채우는 항목 값. 여기서는 쓰지 않고 순회에만 필요하다. */
	void *entry;
/* [한국어] 아래에서 이미 정리됐는지 확인한다. */

	if (!attrs)
		/* [한국어] populate 가 불리지 않았거나 이미 정리된 경우다. */
		return;

	doe_mb->sysfs_attrs = NULL;
	/* [한국어] 기능마다 파일과 이름 문자열을 되돌린다. */
	xa_for_each(&doe_mb->feats, i, entry) {
		/* [한국어] **show 가 채워진 것만** 실제로 만들어진 파일이다. Discovery 를 건너뛴
		 * 칸과 -EEXIST 로 실패한 칸은 show 가 NULL 이다. */
		if (attrs[i].show)
			/* [한국어] 그룹에서 파일을 뺀다. */
			sysfs_remove_file_from_group(&dev->kobj, &attrs[i].attr,
						     /* [한국어] 그룹 이름은 정적 구조체에서 가져온다. */
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
/* [한국어]
 * pci_doe_sysfs_feature_populate - 열거된 기능마다 sysfs 파일을 만든다
 *
 * @pdev: 대상 장치.
 * @doe_mb: 대상 우편함. 그 feats xarray 를 훑는다.
 * @return: 0 성공, -ENOMEM 할당 실패, 그 외는 sysfs 추가 실패값.
 *
 * 먼저 기능 개수를 세어 device_attribute 배열을 한 번에 잡는다. xarray 를
 * 두 번 훑는 셈이지만, 개수를 미리 알아야 배열을 한 덩어리로 잡을 수 있다.
 *
 * 배열 인덱스로 xarray 인덱스 i 를 그대로 쓰는 점에 유의. cache_features 가
 * 0 부터 빈틈없이 채우므로 성립한다.
 *
 * Discovery 기능은 건너뛴다 -- 정적 속성 dev_attr_doe_discovery 가 이미
 * 그 이름의 파일을 만들어 두었기 때문이다. 이때 show 를 채우지 않으므로
 * remove 도 그 칸을 건너뛴다.
 *
 * -EEXIST 처리가 특별하다. 같은 [벤더]:[타입] 기능이 **두 우편함에 모두**
 * 있으면 같은 이름의 파일을 두 번 만들게 되는데, 그것은 오류가 아니라
 * 정상 상황이다. 그래서 show 를 NULL 로 되돌리고 이름을 해제한 뒤 계속
 * 진행한다 -- 첫 우편함이 만든 파일이 그대로 쓰인다.
 *
 * 그 밖의 실패는 fail 라벨로 가 지금까지 만든 것을 모두 되돌린다.
 *
 * 실행 컨텍스트: 장치 추가 시의 프로세스 문맥. GFP_KERNEL 로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_doe_sysfs_init → [이 함수] → kasprintf
 *     → sysfs_add_file_to_group
 */
static int pci_doe_sysfs_feature_populate(struct pci_dev *pdev,
					  struct pci_doe_mb *doe_mb)
{
	struct device *dev = &pdev->dev;
	struct device_attribute *attrs;
	/* [한국어] 기능 개수. xarray 를 한 번 훑어 센다. */
	unsigned long num_features = 0;
	/* [한국어] 인코딩된 값에서 풀어낼 벤더 ID 와 타입. unsigned long 인 것은
	 * xa_to_value 의 반환 타입에 맞춘 것이다. */
	unsigned long vid, type;
	/* [한국어] xarray 인덱스이자 attrs 배열 인덱스. */
	unsigned long i;
	/* [한국어] xa_for_each 가 채우는 항목 값. */
	void *entry;
	/* [한국어] sysfs 추가 결과. */
	int ret;
/* [한국어] 먼저 개수를 세어야 배열을 한 덩어리로 잡을 수 있다. */

	xa_for_each(&doe_mb->feats, i, entry)
		/* [한국어] 항목마다 하나씩 센다. */
		num_features++;
/* [한국어] 이제 배열을 잡는다. */

	attrs = kzalloc_objs(*attrs, num_features);
	/* [한국어] 배열 할당 실패. */
	if (!attrs) {
		/* [한국어] sysfs 파일이 없어도 DOE 자체는 동작하므로 경고에 그친다. */
		pci_warn(pdev, "Failed allocating the device_attribute array\n");
		/* [한국어] 호출자(sysfs_init)가 이 값을 보고 남은 우편함 처리를 멈춘다. */
		return -ENOMEM;
	}

	doe_mb->sysfs_attrs = attrs;
	/* [한국어] 두 번째 순회 -- 이번엔 실제로 파일을 만든다. */
	xa_for_each(&doe_mb->feats, i, entry) {
		/* [한국어] lockdep 이 요구하는 초기화. 정적으로 선언되지 않은 속성에 필요하다. */
		sysfs_attr_init(&attrs[i].attr);
		vid = xa_to_value(entry) >> 8;
		/* [한국어] 하위 8비트가 타입. 인코딩은 pci_doe_xa_feat_entry 와 짝을 이룬다. */
		type = xa_to_value(entry) & 0xFF;
/* [한국어] 아래에서 Discovery 를 걸러 낸다. */

		if (vid == PCI_VENDOR_ID_PCI_SIG &&
		    /* [한국어] Discovery 는 정적 속성 dev_attr_doe_discovery 가 이미 같은 이름의 파일을
		     * 만들어 두었으므로 건너뛴다. show 를 채우지 않으므로 remove 도 이 칸을
		     * 건너뛴다. */
		    type == PCI_DOE_FEATURE_DISCOVERY) {

			/*
			 * DOE Discovery, manually displayed by
			 * `dev_attr_doe_discovery`
			 */
			continue;
		}

		attrs[i].attr.name = kasprintf(GFP_KERNEL,
					       /* [한국어] [벤더]:[타입] 형식. show 함수가 이 이름을 그대로 되돌려주므로,
					        * 파일 이름이 곧 내용이다. */
					       "%04lx:%02lx", vid, type);
		if (!attrs[i].attr.name) {
			/* [한국어] 이름 할당 실패. */
			ret = -ENOMEM;
			pci_warn(pdev, "Failed allocating the attribute name\n");
			/* [한국어] 지금까지 만든 것을 모두 되돌린다. */
			goto fail;
		}

		attrs[i].attr.mode = 0444;
		/* [한국어] show 를 채우는 것이 '이 칸은 실제 파일' 이라는 표시가 된다. */
		attrs[i].show = pci_doe_sysfs_feature_show;
/* [한국어] 이제 sysfs 에 붙인다. */

		ret = sysfs_add_file_to_group(&dev->kobj, &attrs[i].attr,
					      /* [한국어] 위에서 정의한 그룹 이름 아래에 넣는다. */
					      pci_doe_sysfs_group.name);
		if (ret) {
			/* [한국어] 실패했으므로 표시를 되돌린다 -- remove 가 이 칸을 건너뛰게 한다. */
			attrs[i].show = NULL;
			/* [한국어] -EEXIST 는 오류가 아니다. */
			if (ret != -EEXIST) {
				/* [한국어] 그 밖의 실패만 경고로 남긴다. */
				pci_warn(pdev, "Failed adding %s to sysfs group\n",
					 attrs[i].attr.name);
				goto fail;
			} else
				/* [한국어] -EEXIST 경로에서는 이름을 여기서 해제한다. 같은 [벤더]:[타입] 기능을
				 * 두 우편함이 모두 지원하면 첫 우편함이 만든 파일이 그대로 쓰인다. */
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
/* [한국어]
 * pci_doe_sysfs_teardown - 이 장치의 모든 우편함이 만든 sysfs 파일을 걷어낸다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * 우편함마다 remove 를 부른다. remove 가 sysfs_attrs 를 NULL 로 만들고
 * NULL 검사로 시작하므로, 이미 정리된 우편함에 대해 다시 불려도 안전하다.
 *
 * 실행 컨텍스트: 장치 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 코어(장치 제거) → [이 함수] → pci_doe_sysfs_feature_remove
 */
void pci_doe_sysfs_teardown(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;
/* [한국어] 우편함마다 정리한다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		/* [한국어] remove 가 sysfs_attrs 를 NULL 로 만들므로 두 번 불려도 안전하다. */
		pci_doe_sysfs_feature_remove(pdev, doe_mb);
/* [한국어] 이 함수가 pci_doe_destroy 보다 먼저 불려야 한다 -- 그러지 않으면 사라진
 * 우편함의 이름 문자열을 sysfs 가 참조한다. */
}


/*
 * pci_doe_sysfs_init:
 *   NVMe 장치 probe 시 각 DOE mailbox에 대해 sysfs feature 파일을 생성한다.
 *   생성된 /sys/bus/pci/devices/.../doe_features 항목은 nvme-cli 등이
 *   NVMe 컨트롤러의 DOE capability를 확인하는 데 사용된다.
 */
/* [한국어]
 * pci_doe_sysfs_init - 이 장치의 모든 우편함에 대해 sysfs 파일을 만든다
 *
 * @pdev: 대상 장치.
 * @return: 없음. 실패를 알릴 통로가 없다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): 한 우편함의 populate 가 실패하면
 * **그 자리에서 반환한다** -- 남은 우편함은 시도조차 하지 않는다. 이미
 * 성공한 우편함의 파일은 그대로 남고, 실패한 우편함은 populate 안에서
 * 스스로 정리한 상태다. sysfs 파일이 없어도 DOE 기능 자체는 동작하므로
 * 치명적이지 않다는 판단으로 보인다.
 *
 * 실행 컨텍스트: 장치 추가 시의 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 코어(장치 추가) → [이 함수] → pci_doe_sysfs_feature_populate
 */
void pci_doe_sysfs_init(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;
	/* [한국어] populate 결과. */
	int ret;
/* [한국어] 우편함마다 파일을 만든다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb) {
		/* [한국어] 한 우편함의 모든 기능에 대해 파일을 만든다. */
		ret = pci_doe_sysfs_feature_populate(pdev, doe_mb);
		/* [한국어] 코드 관찰 (상류 그대로, 수정하지 않음): 실패하면 **그 자리에서 반환**해
		 * 남은 우편함은 시도조차 하지 않는다. 이미 성공한 것은 그대로 남는다. */
		if (ret)
			/* [한국어] sysfs 파일이 없어도 DOE 기능 자체는 동작하므로 치명적이지 않다는 판단이다. */
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
/* [한국어]
 * pci_doe_wait - 취소 신호를 기다리며 정해진 시간만큼 잠든다
 *
 * @doe_mb: 대상 우편함.
 * @timeout: 기다릴 시간(jiffies).
 * @return: **-EIO 면 취소된 것**, 0 이면 시간이 다 지난 것(정상).
 *
 * 반환값의 의미가 뒤집힌 듯 보이지만 그렇지 않다. 이 함수의 목적은
 * '취소되지 않은 채 timeout 만큼 쉬기' 이고, wait_event_timeout 은 조건
 * (= CANCEL 플래그)이 참이 되면 남은 시간(0 이 아닌 값)을 돌려준다.
 * 즉 **깨어났다는 것 자체가 취소를 뜻하므로** -EIO 다.
 *
 * 폴링 루프에서 CPU 를 놓아 주는 용도라, 취소가 없으면 timeout 을 다 채우고
 * 0 을 돌려주는 것이 정상 경로다.
 *
 * 취소 경로는 pci_doe_cancel_tasks() 가 CANCEL 을 세우고 wake_up 을 부르는
 * 것이다 -- 장치가 사라졌을 때 대기 중인 작업을 즉시 깨우기 위해서다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥. 잠든다.
 *
 * 호출 체인:
 *   pci_doe_abort / doe_statemachine_work → [이 함수]
 *     → wait_event_timeout
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
/* [한국어]
 * pci_doe_write_ctrl - DOE 제어 레지스터에 값을 쓴다
 *
 * @doe_mb: 대상 우편함.
 * @val: 쓸 값(PCI_DOE_CTRL_ABORT 또는 PCI_DOE_CTRL_GO).
 * @return: 없음.
 *
 * DOE 는 MMIO 가 아니라 **확장 설정공간**에 있다. 그래서 접근이
 * pci_write_config_dword 이고, 우편함마다 cap_offset 이 다르다.
 *
 * 얇은 래퍼지만 오프셋 계산을 한곳에 모아, 호출부가 cap_offset 을 잊는
 * 실수를 막는다.
 *
 * 인터럽트 활성 비트를 다루지 않는 점에 유의 -- 이 구현은 인터럽트를 쓰지
 * 않고 전부 폴링한다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_doe_abort / pci_doe_send_req → [이 함수]
 *     → pci_write_config_dword
 */
static void pci_doe_write_ctrl(struct pci_doe_mb *doe_mb, u32 val)
{
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
/* [한국어] 취소 신호로 깨어났다는 뜻이므로 오류로 돌려준다. */

	pci_write_config_dword(pdev, offset + PCI_DOE_CTRL, val);
/* [한국어] 시간이 다 지나 깨어난 것이 정상 경로다. */
}


/*
 * pci_doe_abort:
 *   NVMe 장치의 DOE mailbox에 abort를 발행하여 상태 머신을 리셋한다.
 *   probe 시 메일박스 초기화는 물론, 장치 분리(hotplug)나 오류 복구 시
 *   진행 중인 DOE 교환을 정리하는 데 필수적이다.
 */
/* [한국어]
 * pci_doe_abort - 우편함에 Abort 를 걸고 오류·바쁨이 모두 풀릴 때까지 기다린다
 *
 * @doe_mb: 대상 우편함.
 * @return: 0 성공, -EIO 는 1초 안에 풀리지 않음, 그 외는 취소 신호(-EIO).
 *
 * DOE 규약이 정한 유일한 초기화 수단이다. 우편함이 어떤 상태에 빠져 있든
 * Abort 를 쓰면 하드웨어가 상태를 비우고 ERROR 와 BUSY 를 모두 내려야 한다.
 *
 * 그래서 두 곳에서 쓰인다:
 *  - 우편함을 처음 만들 때(create_mb) -- 부트로더나 이전 커널이 남긴 상태를
 *    지운다.
 *  - 작업이 실패했을 때(signal_task_abort) -- 다음 작업이 깨끗한 상태에서
 *    시작하게 한다.
 *
 * 루프가 pci_doe_wait 로 쉬면서 도는데, 그 함수가 -EIO 를 돌려주면
 * (= 취소 신호) 곧바로 그 값을 올린다. 취소는 장치가 사라졌다는 뜻이라
 * 더 기다릴 이유가 없다.
 *
 * 1초(PCI_DOE_TIMEOUT) 안에 못 풀면 우편함이 응답하지 않는 것이다.
 * 호출자(signal_task_abort)가 그 경우 우편함을 DEAD 로 표시한다.
 *
 * 실행 컨텍스트: 작업 큐 워커 또는 프로브의 프로세스 문맥. 잠든다.
 *
 * 호출 체인:
 *   pci_doe_create_mb / signal_task_abort → [이 함수]
 *     → pci_doe_write_ctrl → pci_doe_wait
 */
static int pci_doe_abort(struct pci_doe_mb *doe_mb)
{
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	/* [한국어] 타임아웃 기준 시각. */
	unsigned long timeout_jiffies;
/* [한국어] 아래에서 Abort 를 건다. */

	pci_dbg(pdev, "[%x] Issuing Abort\n", offset);
/* [한국어] 어느 우편함인지 오프셋으로 표시한다. */

	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;
	/* [한국어] 제어 레지스터에 Abort 를 쓴다. 이 시점부터 하드웨어가 상태를 비운다. */
	pci_doe_write_ctrl(doe_mb, PCI_DOE_CTRL_ABORT);
/* [한국어] 이제 ERROR 와 BUSY 가 모두 내려갈 때까지 기다린다. */

	do {
		int rc;
		/* [한국어] 상태 레지스터 값. */
		u32 val;
/* [한국어] 먼저 쉬었다가 확인한다 -- 곧바로 읽으면 하드웨어가 반영할 시간이 없다. */

		rc = pci_doe_wait(doe_mb, PCI_DOE_POLL_INTERVAL);
		/* [한국어] pci_doe_wait 가 -EIO 를 주면 취소 신호다. */
		if (rc)
			/* [한국어] 장치가 사라졌다는 뜻이라 더 기다릴 이유가 없다. */
			return rc;
		/* [한국어] 상태를 읽는다. */
		pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
/* [한국어] 아래에서 두 비트가 모두 내려갔는지 본다. */

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
/* [한국어]
 * pci_doe_send_req - 요청 데이터 오브젝트를 설정공간 창으로 밀어 넣고 GO 를 건다
 *
 * @doe_mb: 대상 우편함.
 * @task: 보낼 작업.
 * @return: 0 성공, -EBUSY 는 다른 주체가 쓰는 중, -EIO 는 오류 상태이거나
 *          길이 초과.
 *
 * DOE 전송은 설정공간의 WRITE 레지스터에 32비트씩 밀어 넣는 방식이다.
 * MMIO 가 아니라 설정공간이라 한 워드씩 느리게 나가고, 그래서 이 함수가
 * 작업 큐 안에서만 불린다.
 *
 * 단계:
 *  1. BUSY 가 내려갈 때까지 최대 1초 폴링한다. 여전히 BUSY 면 -EBUSY --
 *     상류 주석이 밝히듯 **다른 주체(펌웨어 등)가 같은 우편함을 쓰고 있다**는
 *     뜻이라, 호출자가 그 사실을 로그로 알린다.
 *  2. ERROR 가 서 있으면 -EIO.
 *  3. 길이를 계산한다: 헤더 두 워드 + 페이로드 워드 수. 규약상 길이 필드가
 *     18비트라 최대 2^18 인데, **정확히 최대값이면 0 으로 인코딩**하는
 *     규칙이 있어 그렇게 바꾼다.
 *  4. 헤더 두 워드(벤더+타입, 길이)를 쓴다.
 *  5. 페이로드를 워드 단위로 쓴다. `le32_to_cpu()` 를 거치는 이유:
 *     request_pl 이 __le32 배열이고 pci_write_config_dword 가 CPU 바이트
 *     순서를 받아 다시 LE 로 바꾸므로, 두 변환이 상쇄되어 원래 바이트가
 *     그대로 나간다. 빅엔디언 호스트에서도 성립하게 하는 관용구다.
 *  6. 4의 배수가 아닌 나머지는 0 으로 채운 워드에 memcpy 해서 보낸다.
 *  7. GO 를 걸어 하드웨어에 전송을 알린다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥.
 *
 * 호출 체인:
 *   doe_statemachine_work → [이 함수] → pci_write_config_dword
 *     → pci_doe_write_ctrl(GO)
 */
static int pci_doe_send_req(struct pci_doe_mb *doe_mb,
			    struct pci_doe_task *task)
{
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	/* [한국어] 타임아웃 기준 시각. */
	unsigned long timeout_jiffies;
	/* [한국어] length 는 헤더 포함 워드 수, remainder 는 4로 나눈 나머지 바이트. */
	size_t length, remainder;
	/* [한국어] 읽고 쓸 임시값. */
	u32 val;
	/* [한국어] 페이로드 워드 순회 인덱스. 루프 뒤에도 쓰이므로 밖에 선언한다. */
	int i;
/* [한국어] 먼저 우편함이 비어 있는지 확인한다. */

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
	/* [한국어] BUSY 가 내려가거나 1초가 지날 때까지 돈다. pci_doe_wait 를 쓰지 않고
	 * 바쁜 폴링인 점에 유의 -- 여기서는 취소 신호를 볼 필요가 없다. */
	} while (FIELD_GET(PCI_DOE_STATUS_BUSY, val) &&
		 /* [한국어] 두 조건의 AND 라, 둘 중 하나만 성립해도 루프가 끝난다. */
		 !time_after(jiffies, timeout_jiffies));

	if (FIELD_GET(PCI_DOE_STATUS_BUSY, val))
		/* [한국어] 여전히 BUSY -- 상류 주석대로 **다른 주체가 같은 우편함을 쓰는 중**이다.
		 * 호출자가 그 사실을 별도 메시지로 알린다. */
		return -EBUSY;

	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val))
		/* [한국어] ERROR 가 서 있으면 보낼 수 없다. */
		return -EIO;

	/* Length is 2 DW of header + length of payload in DW */
	length = 2 + DIV_ROUND_UP(task->request_pl_sz, sizeof(__le32));
	if (length > PCI_DOE_MAX_LENGTH)
		/* [한국어] 규약이 정한 최대 길이를 넘었다. */
		return -EIO;
	if (length == PCI_DOE_MAX_LENGTH)
		/* [한국어] 길이 필드가 18비트라 정확히 2^18 은 담을 수 없다. 규약이 **0 을 최대값**
		 * 으로 인코딩하도록 정해 두었으므로 그렇게 바꾼다. */
		length = 0;
/* [한국어] 이제 헤더 두 워드를 쓴다. */

	/* Write DOE Header */
	val = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_VID, task->feat.vid) |
		FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, task->feat.type);
	/* [한국어] 첫 워드: 벤더 ID 와 기능 타입. */
	pci_write_config_dword(pdev, offset + PCI_DOE_WRITE, val);
	/* [한국어] 둘째 워드: 길이. */
	pci_write_config_dword(pdev, offset + PCI_DOE_WRITE,
			       /* [한국어] 위에서 조정한 length 가 여기 들어간다. */
			       FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH,
					  length));

	/* Write payload */
	for (i = 0; i < task->request_pl_sz / sizeof(__le32); i++)	/* [한국어] 페이로드를 dword 단위로 순회한다 — DOE 는 4바이트 단위로만 주고받는다 */
		pci_write_config_dword(pdev, offset + PCI_DOE_WRITE,
				       le32_to_cpu(task->request_pl[i]));

	/* Write last payload dword */
	remainder = task->request_pl_sz % sizeof(__le32);
	if (remainder) {
		/* [한국어] 4의 배수가 아닌 나머지를 담을 워드를 0 으로 초기화한다. */
		val = 0;
		/* [한국어] 남은 바이트만 복사하므로 나머지 자리는 0 으로 남는다. */
		memcpy(&val, &task->request_pl[i], remainder);
		/* [한국어] le32_to_cpus 로 바꾸면 pci_write_config_dword 가 다시 LE 로 되돌려,
		 * 원래 바이트 순서가 그대로 나간다. 빅엔디언 호스트에서도 성립한다. */
		le32_to_cpus(&val);
		pci_write_config_dword(pdev, offset + PCI_DOE_WRITE, val);
	/* [한국어] 나머지가 없으면 이 블록 전체를 건너뛴다. */
	}

	pci_doe_write_ctrl(doe_mb, PCI_DOE_CTRL_GO);
/* [한국어] 이제 GO 를 걸어 하드웨어에 전송을 알린다. */

	return 0;
}


/*
 * pci_doe_data_obj_ready:
 *   NVMe 장치가 DOE 응답 데이터 객체를 준비했는지 PCI_DOE_STATUS 레지스터의
 *   Data Object Ready 비트를 읽어 확인한다.
 */
/* [한국어]
 * pci_doe_data_obj_ready - 응답 오브젝트가 준비됐는지 상태 비트 하나를 본다
 *
 * @doe_mb: 대상 우편함.
 * @return: true 면 읽을 응답이 있다.
 *
 * pci_doe_recv_resp 가 응답을 다 읽었다고 판단한 뒤, **정말로 더 읽을
 * 것이 남았는지** 확인하는 데 쓴다. 규약상 마지막 워드를 읽고 나면 이
 * 비트가 내려가야 하는데, 내려가지 않으면 하드웨어가 우리가 아는 것보다
 * 긴 응답을 갖고 있다는 뜻이라 오류로 처리한다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_doe_recv_resp → [이 함수] → pci_read_config_dword
 */
static bool pci_doe_data_obj_ready(struct pci_doe_mb *doe_mb)
{
	struct pci_dev *pdev = doe_mb->pdev;
	int offset = doe_mb->cap_offset;
	/* [한국어] 상태 레지스터 값. */
	u32 val;

	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
	/* [한국어] 응답 오브젝트가 준비됐다는 비트. */
	if (FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val))
		/* [한국어] 읽을 것이 남았다는 뜻이다. */
		return true;
	return false;
}


/*
 * pci_doe_recv_resp:
 *   NVMe 장치의 DOE mailbox 읽기 포트에서 응답 데이터 객체를 수신한다.
 *   헤더(VID/type/length)를 검증하고, 사용자가 요청한 크기만큼 payload를
 *   복사하며 초과 데이터는 flush한다.
 */
/* [한국어]
 * pci_doe_recv_resp - 응답 오브젝트를 읽어 요청자 버퍼에 담고 남는 것은 버린다
 *
 * @doe_mb: 대상 우편함.
 * @task: 응답 버퍼를 가진 작업.
 * @return: 버퍼에 담은 바이트 수(양수), 또는 -EIO.
 *
 * 읽기 규약이 특이하다. READ 레지스터를 **읽고 나서 0 을 써야** 다음
 * 워드가 올라온다 -- 쓰기가 '이 워드를 소비했다' 는 신호다. 그래서 모든
 * 읽기가 읽기-쓰기 쌍으로 나온다.
 *
 * 단계:
 *  1. 헤더 첫 워드로 벤더/타입이 요청과 같은지 확인한다. 다르면 다른
 *     주체의 응답을 가로챈 것이므로 -EIO.
 *  2. 헤더 둘째 워드에서 길이를 얻는다. 0 은 최대값(2^18)을 뜻하는
 *     인코딩이고, 2 미만은 헤더조차 안 되므로 오류다. 헤더 두 워드를 빼면
 *     페이로드 길이가 된다.
 *  3. 요청자 버퍼보다 응답이 짧으면 짧은 쪽에 맞춘다(received 조정).
 *  4. 마지막 워드 **직전까지** 읽어 버퍼에 담는다.
 *  5. 마지막 워드는 remainder 만큼만 memcpy 한다 -- 버퍼가 4의 배수가
 *     아닐 수 있기 때문이다. 그 뒤 data_obj_ready 로 '정말 끝인지' 확인하고
 *     소비 신호를 보낸다.
 *  6. 버퍼에 담지 못하고 남은 워드는 읽어서 **버린다**. 소비하지 않으면
 *     우편함이 다음 작업을 받지 못한다.
 *  7. 마지막으로 ERROR 비트를 확인한다.
 *
 * cpu_to_le32s() 는 send 쪽의 le32_to_cpu 와 대칭인 관용구다 --
 * pci_read_config_dword 가 LE 를 CPU 순서로 바꿔 주므로 다시 LE 로 되돌려
 * __le32 버퍼에 담는다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥.
 *
 * 호출 체인:
 *   doe_statemachine_work → [이 함수] → pci_read_config_dword
 *     → pci_doe_data_obj_ready
 */
static int pci_doe_recv_resp(struct pci_doe_mb *doe_mb, struct pci_doe_task *task)
{
	size_t length, payload_length, remainder, received;
	struct pci_dev *pdev = doe_mb->pdev;
	/* [한국어] 설정공간 접근의 기준 오프셋. */
	int offset = doe_mb->cap_offset;
	/* [한국어] 응답 버퍼 인덱스. 여러 루프가 이어서 쓰므로 밖에 선언한다. */
	int i = 0;
	/* [한국어] 읽을 임시값. */
	u32 val;
/* [한국어] 먼저 헤더로 신원을 확인한다. */

	/* Read the first dword to get the feature */
	pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
	if ((FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, val) != task->feat.vid) ||
	    /* [한국어] 벤더나 타입이 다르면 우리 응답이 아니다 -- 다른 주체의 응답을 가로챈
	     * 상황이다. */
	    (FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, val) != task->feat.type)) {
		dev_err_ratelimited(&pdev->dev, "[%x] expected [VID, Feature] = [%04x, %02x], got [%04x, %02x]\n",
				    /* [한국어] 기대값과 실제값을 함께 찍어 진단할 수 있게 한다. */
				    doe_mb->cap_offset, task->feat.vid, task->feat.type,
				    FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_VID, val),
				    FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, val));
		return -EIO;
	}

	pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
	/* Read the second dword to get the length */
	pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
	pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
/* [한국어] 헤더 둘째 워드에서 길이를 얻는다. */

	length = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, val);
	/* A value of 0x0 indicates max data object length */
	if (!length)
		length = PCI_DOE_MAX_LENGTH;
	/* [한국어] 길이가 2 미만이면 헤더조차 되지 않는다. */
	if (length < 2)
		/* [한국어] 규약을 따르지 않는 응답이다. */
		return -EIO;

	/* First 2 dwords have already been read */
	length -= 2;
	received = task->response_pl_sz;
	/* [한국어] 요청자 버퍼를 워드 단위로 환산한 길이. */
	payload_length = DIV_ROUND_UP(task->response_pl_sz, sizeof(__le32));
	/* [한국어] 4로 나눈 나머지. 마지막 워드에서 몇 바이트만 쓸지 정한다. */
	remainder = task->response_pl_sz % sizeof(__le32);
/* [한국어] 나머지가 0 이면 마지막 워드를 통째로 쓴다는 뜻이다. */

	/* remainder signifies number of data bytes in last payload dword */
	if (!remainder)
		remainder = sizeof(__le32);
/* [한국어] 응답이 요청자 버퍼보다 짧은 경우. */

	if (length < payload_length) {
		/* [한국어] 실제로 담을 바이트 수를 응답 길이에 맞춘다. */
		received = length * sizeof(__le32);
		/* [한국어] 순회 길이도 줄인다. */
		payload_length = length;
		/* [한국어] 이 경우 마지막 워드는 통째로 쓴다. */
		remainder = sizeof(__le32);
	/* [한국어] 이제 실제 읽기로 넘어간다. */
	}

	if (payload_length) {
		/* Read all payload dwords except the last */
		for (; i < payload_length - 1; i++) {	/* [한국어] 페이로드를 dword 단위로 순회한다 — DOE 는 4바이트 단위로만 주고받는다 */
			pci_read_config_dword(pdev, offset + PCI_DOE_READ,
					      &val);
			task->response_pl[i] = cpu_to_le32(val);
			pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
		/* [한국어] 마지막 워드 직전까지 워드 단위로 담는다. */
		}

		/* Read last payload dword */
		pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
		cpu_to_le32s(&val);
		memcpy(&task->response_pl[i], &val, remainder);
		/* Prior to the last ack, ensure Data Object Ready */
		if (!pci_doe_data_obj_ready(doe_mb))
			return -EIO;
		pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
		/* [한국어] 마지막 워드까지 소비했으므로 인덱스를 하나 올린다. */
		i++;
	/* [한국어] 아래에서 남는 워드를 버린다. */
	}

	/* Flush excess length */
	for (; i < length; i++) {	/* [한국어] 페이로드를 dword 단위로 순회한다 — DOE 는 4바이트 단위로만 주고받는다 */
		pci_read_config_dword(pdev, offset + PCI_DOE_READ, &val);
		pci_write_config_dword(pdev, offset + PCI_DOE_READ, 0);
	}

	/* Final error check to pick up on any since Data Object Ready */
	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val))
		/* [한국어] 전송 도중 오류가 났다. */
		return -EIO;

	return received;
/* [한국어] 성공하면 실제로 담은 바이트 수를 돌려준다 -- 요청한 크기보다 작을 수 있다. */
}


/*
 * signal_task_complete:
 *   NVMe 측에 제출된 DOE 태스크의 결과를 기록하고 완료 콜백을 호출한다.
 *   동기 호출이라면 completion을 wake하여 NVMe 호출자가 깨어나게 한다.
 */
/* [한국어]
 * signal_task_complete - 작업 결과를 담고 완료 콜백을 부른다
 *
 * @task: 끝난 작업.
 * @rv: 결과. 양수면 응답 바이트 수, 음수면 오류.
 * @return: 없음.
 *
 * 순서가 중요하다. destroy_work_on_stack 을 **complete 콜백보다 먼저**
 * 부른다 -- 콜백이 대기자를 깨우면 그 대기자가 스택 프레임을 곧바로
 * 반환할 수 있으므로, 그 전에 work 구조체의 디버그 객체를 정리해야 한다.
 *
 * task->work 가 스택에 있다는 것이 이 설계의 전제다(pci_doe() 가 스택에
 * task 를 잡는다). INIT_WORK_ONSTACK 과 짝을 이룬다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥.
 *
 * 호출 체인:
 *   doe_statemachine_work / signal_task_abort → [이 함수]
 *     → destroy_work_on_stack → task->complete
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
/* [한국어]
 * signal_task_abort - 우편함을 Abort 로 되돌린 뒤 작업을 실패로 끝낸다
 *
 * @task: 실패한 작업.
 * @rv: 실패 코드.
 * @return: 없음.
 *
 * 작업이 실패하면 우편함에 반쯤 처리된 상태가 남을 수 있다. 그대로 두면
 * 다음 작업이 그 잔재를 자기 응답으로 오인하므로, Abort 로 비운다.
 *
 * **Abort 마저 실패하면 우편함을 DEAD 로 표시한다.** 그러면
 * pci_doe_submit_task 가 이후 모든 작업을 -EIO 로 거절하고,
 * doe_statemachine_work 도 곧바로 실패로 끝낸다. 상태를 되돌릴 방법이
 * 없는 우편함을 계속 쓰는 것보다 낫다는 판단이다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥.
 *
 * 호출 체인:
 *   doe_statemachine_work → [이 함수] → pci_doe_abort
 *     → signal_task_complete
 */
static void signal_task_abort(struct pci_doe_task *task, int rv)
{
	struct pci_doe_mb *doe_mb = task->doe_mb;
	struct pci_dev *pdev = doe_mb->pdev;
/* [한국어] Abort 마저 실패하면 되돌릴 방법이 없다. */

	if (pci_doe_abort(doe_mb)) {
		/*
		 * If the device can't process an abort; set the mailbox dead
		 *	- no more submissions
		 */
		pci_err(pdev, "[%x] Abort failed marking mailbox dead\n",
			doe_mb->cap_offset);
		set_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags);
	/* [한국어] 우편함을 DEAD 로 표시해 이후 모든 작업을 거절하게 한다. */
	}
	signal_task_complete(task, rv);
/* [한국어] 어느 경로든 마지막에는 작업을 완료로 표시해 대기자를 깨운다. */
}


/*
 * doe_statemachine_work:
 *   NVMe용 DOE 태스크를 비동기적으로 처리하는 상태 머신 work 함수.
 *   요청 전송 -> 응답 폴링 -> 응답 수신 -> 완료 시그널의 전체 흐름을
 *   담당하며, timeout/오류 시 abort를 수행한다.
 */
/* [한국어]
 * doe_statemachine_work - 작업 하나의 전송·대기·수신을 순서대로 수행하는 워커
 *
 * @work: 작업 큐 항목. container_of 로 pci_doe_task 를 되찾는다.
 * @return: 없음.
 *
 * DOE 전송의 전 과정이 여기 있다. **순서 있는(ordered) 작업 큐**에서
 * 실행되므로 한 우편함의 작업이 한 번에 하나씩만 돈다 -- 그것이 이 파일에
 * 별도의 우편함 잠금이 없는 이유다.
 *
 * 흐름:
 *  1. 우편함이 이미 DEAD 면 곧바로 -EIO 로 끝낸다.
 *  2. 요청을 보낸다. 실패하면 abort 경로로. -EBUSY 는 다른 주체와의 충돌을
 *     뜻하므로 별도 메시지를 남긴다.
 *  3. retry_resp 라벨에서 응답을 기다린다:
 *       - ERROR 가 서면 즉시 abort.
 *       - DATA_OBJECT_READY 가 아직이면 1초 한도 안에서 pci_doe_wait 로
 *         쉬었다가 다시 확인한다. pci_doe_wait 가 -EIO 를 주면(= 취소)
 *         그대로 abort.
 *       - 시간이 다 되면 abort.
 *  4. 응답을 읽는다. 실패하면 abort, 성공하면 읽은 바이트 수를 결과로 담는다.
 *
 * 인터럽트를 쓰지 않고 폴링하는 구조라, 이 워커가 최대 1초까지 큐를
 * 점유할 수 있다. 우편함마다 전용 큐를 두는 것이 그 영향을 가두는 방법이다.
 *
 * 실행 컨텍스트: 우편함 전용 ordered 작업 큐의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_doe_submit_task → queue_work → [이 함수]
 *     → pci_doe_send_req → pci_doe_wait → pci_doe_recv_resp
 */
static void doe_statemachine_work(struct work_struct *work)
{
	struct pci_doe_task *task = container_of(work, struct pci_doe_task,
						 work);
	struct pci_doe_mb *doe_mb = task->doe_mb;
	/* [한국어] 로그 주체. */
	struct pci_dev *pdev = doe_mb->pdev;
	/* [한국어] 설정공간 접근의 기준. */
	int offset = doe_mb->cap_offset;
	/* [한국어] 응답 대기의 타임아웃 기준. */
	unsigned long timeout_jiffies;
	/* [한국어] 상태 레지스터 값. */
	u32 val;
	/* [한국어] 각 단계의 결과. */
	int rc;
/* [한국어] 먼저 우편함이 살아 있는지 본다. */

	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags)) {
		/* [한국어] 이미 죽은 우편함이면 전송을 시도할 이유가 없다. */
		signal_task_complete(task, -EIO);
		/* [한국어] abort 를 거치지 않고 곧바로 완료로 끝낸다 -- 되돌릴 상태가 없다. */
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
					    /* [한국어] 어느 우편함인지 오프셋으로 표시한다. ratelimited 인 것은 충돌이
					     * 반복될 수 있기 때문이다. */
					    offset);
		signal_task_abort(task, rc);
		/* [한국어] abort 로 우편함을 비운 뒤 작업을 실패로 끝낸다. */
		return;
	}

	timeout_jiffies = jiffies + PCI_DOE_TIMEOUT;
	/* Poll for response */
retry_resp:
	pci_read_config_dword(pdev, offset + PCI_DOE_STATUS, &val);
	if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {
		/* [한국어] 전송 도중 오류가 났다. */
		signal_task_abort(task, -EIO);
		/* [한국어] abort 경로로. */
		return;
	}

	if (!FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val)) {
		/* [한국어] 1초가 지났는데도 응답이 없다. */
		if (time_after(jiffies, timeout_jiffies)) {
			/* [한국어] 타임아웃도 오류로 처리한다. */
			signal_task_abort(task, -EIO);
			/* [한국어] abort 로 우편함을 비운다. */
			return;
		}
		rc = pci_doe_wait(doe_mb, PCI_DOE_POLL_INTERVAL);
		/* [한국어] pci_doe_wait 가 -EIO 를 주면 취소 신호다. */
		if (rc) {
			/* [한국어] 그 값을 그대로 결과로 전달한다. */
			signal_task_abort(task, rc);
			/* [한국어] 장치가 사라졌으므로 더 기다리지 않는다. */
			return;
		}
		goto retry_resp;
	}

	rc  = pci_doe_recv_resp(doe_mb, task);
	/* [한국어] 응답 읽기 실패. */
	if (rc < 0) {
		/* [한국어] abort 경로로. */
		signal_task_abort(task, rc);
		/* [한국어] 우편함을 비워 다음 작업이 깨끗하게 시작하게 한다. */
		return;
	}

	signal_task_complete(task, rc);
/* [한국어] 성공 경로만 signal_task_complete 로 끝난다. */
}


/*
 * pci_doe_task_complete:
 *   동기 방식 pci_doe() 호출 시 사용하는 낮은 수준 완료 콜백.
 *   NVMe 호스트가 wait_for_completion()으로 대기 중인 completion 객체를
 *   시그널링하여 결과를 반환한다.
 */
/* [한국어]
 * pci_doe_task_complete - 동기 호출자를 깨우는 완료 콜백
 *
 * @task: 끝난 작업. private 에 completion 이 들어 있다.
 * @return: 없음.
 *
 * pci_doe() 가 스택에 completion 을 잡고 이 함수를 콜백으로 걸어 둔다.
 * 워커가 이것을 부르면 wait_for_completion 에 잠들어 있던 호출자가 깨어난다.
 *
 * 콜백을 통해 간접적으로 깨우는 구조 덕에, 나중에 비동기 API 를 붙일 때
 * complete/private 만 바꾸면 된다.
 *
 * 실행 컨텍스트: 작업 큐 워커의 프로세스 문맥.
 *
 * 호출 체인:
 *   signal_task_complete → task->complete → [이 함수] → complete()
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
/* [한국어]
 * pci_doe_discovery - Discovery 기능으로 다음 기능 하나를 알아낸다
 *
 * @doe_mb: 대상 우편함.
 * @capver: 이 DOE 확장 능력의 버전.
 * @index: 입력은 물어볼 인덱스, 출력은 **다음** 인덱스(0 이면 끝).
 * @vid: 알아낸 벤더 ID를 담을 곳.
 * @feature: 알아낸 기능 타입을 담을 곳.
 * @return: 0 성공, 음수는 전송 실패.
 *
 * DOE 는 자기 자신을 통해 자기 기능 목록을 알려 준다. Discovery 는 모든
 * 우편함이 지원하는 기능이고, 인덱스를 하나씩 물어보면 다음 인덱스를 함께
 * 돌려주는 연결 리스트 형태다.
 *
 * 요청 버전 필드가 capver 에 따라 갈린다: **능력 버전 2 이상이면 요청
 * 버전 2**, 아니면 0 이다. 버전 2 요청이 더 많은 정보를 받게 해 주지만,
 * 구형 하드웨어는 그 값을 이해하지 못하므로 낮춰 보낸다.
 *
 * 응답 크기가 정확히 4바이트여야 한다 -- 다르면 규약을 따르지 않는
 * 하드웨어이므로 -EIO.
 *
 * 실행 컨텍스트: 프로브 경로의 프로세스 문맥. pci_doe() 가 잠든다.
 *
 * 호출 체인:
 *   pci_doe_cache_features → [이 함수] → pci_doe()
 */
static int pci_doe_discovery(struct pci_doe_mb *doe_mb, u8 capver, u8 *index, u16 *vid,
			     u8 *feature)
{
	u32 request_pl = FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_REQ_3_INDEX,
				    *index) |
			 FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_REQ_3_VER,
				    (capver >= 2) ? 2 : 0);
	__le32 request_pl_le = cpu_to_le32(request_pl);
	/* [한국어] 응답을 받을 4바이트. */
	__le32 response_pl_le;
	/* [한국어] CPU 바이트 순서로 바꾼 응답. */
	u32 response_pl;
	/* [한국어] 전송 결과. */
	int rc;
/* [한국어] 아래에서 Discovery 요청을 보낸다. */

	rc = pci_doe(doe_mb, PCI_VENDOR_ID_PCI_SIG, PCI_DOE_FEATURE_DISCOVERY,
		     /* [한국어] 요청과 응답 모두 4바이트 고정이다. */
		     &request_pl_le, sizeof(request_pl_le),
		     &response_pl_le, sizeof(response_pl_le));
	if (rc < 0)
		/* [한국어] 전송 자체가 실패했다. */
		return rc;
/* [한국어] 이제 크기를 확인한다. */

	if (rc != sizeof(response_pl_le))
		/* [한국어] 정확히 4바이트가 아니면 규약을 따르지 않는 하드웨어다. */
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
/* [한국어]
 * pci_doe_xa_feat_entry - [벤더, 타입] 한 쌍을 xarray 값 하나로 접는다
 *
 * @vid: 벤더 ID(16비트).
 * @type: 기능 타입(8비트).
 * @return: xarray 에 저장할 값 포인터.
 *
 * xarray 는 포인터를 저장하는 자료구조지만 xa_mk_value() 로 작은 정수를
 * 포인터 자리에 직접 넣을 수 있다 -- 별도 할당 없이 24비트를 담는 방법이다.
 *
 * vid 를 8비트 밀고 type 을 하위에 놓는 배치는 populate 쪽의
 * `xa_to_value(entry) >> 8` / `& 0xFF` 와 정확히 짝을 이룬다.
 *
 * 이 함수가 따로 있는 덕에 인코딩이 한곳에만 있고, supports_feat 은
 * 값을 풀지 않고 **인코딩된 값끼리 비교**해 탐색을 단순하게 만든다.
 *
 * 실행 컨텍스트: 어디서나. 순수 계산이다.
 *
 * 호출 체인:
 *   pci_doe_cache_features / pci_doe_supports_feat → [이 함수]
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
/* [한국어]
 * pci_doe_cache_features - Discovery 를 반복해 기능 목록을 xarray 에 채운다
 *
 * @doe_mb: 대상 우편함.
 * @return: 0 성공, 음수는 전송 또는 삽입 실패.
 *
 * 인덱스 0 부터 시작해 다음 인덱스가 0 이 될 때까지 Discovery 를 되풀이한다.
 * do-while 인 것이 중요하다 -- 첫 질의는 index 가 0 이어도 반드시 해야 한다.
 *
 * 저장 키로 Discovery 가 준 인덱스가 아니라 **별도의 xa_idx 를 0 부터
 * 증가**시켜 쓴다. 그래야 sysfs populate 가 배열 인덱스를 그대로 쓸 수 있다
 * (하드웨어 인덱스는 빈틈이 있을 수 있다).
 *
 * 능력 버전을 매번 읽지 않고 루프 앞에서 한 번만 읽어 pci_doe_discovery 에
 * 넘긴다.
 *
 * 실패하면 중간까지 채운 xarray 를 그대로 두고 반환한다 -- 정리는 호출자
 * (create_mb 의 err_cancel 라벨)가 xa_destroy 로 한다.
 *
 * 실행 컨텍스트: 프로브 경로의 프로세스 문맥. 잠든다.
 *
 * 호출 체인:
 *   pci_doe_create_mb → [이 함수] → pci_doe_discovery → xa_insert
 */
static int pci_doe_cache_features(struct pci_doe_mb *doe_mb)
{
	u8 index = 0;
	u8 xa_idx = 0;
	/* [한국어] 확장 능력 헤더. 버전을 여기서 얻는다. */
	u32 hdr = 0;
/* [한국어] 아래에서 능력 헤더를 한 번만 읽는다. */

	pci_read_config_dword(doe_mb->pdev, doe_mb->cap_offset, &hdr);
/* [한국어] 루프 안에서 매번 읽지 않고 앞에서 한 번만 읽어 discovery 에 넘긴다. */

	do {
		int rc;
		/* [한국어] 이번 인덱스에서 알아낸 벤더 ID. */
		u16 vid;
		/* [한국어] 기능 타입. */
		u8 type;

		rc = pci_doe_discovery(doe_mb, PCI_EXT_CAP_VER(hdr), &index,
				       /* [한국어] index 는 입출력 겸용 -- 물어본 인덱스가 다음 인덱스로 덮어써진다. */
				       &vid, &type);
		if (rc)
			/* [한국어] 전송 실패. 중간까지 채운 xarray 는 호출자가 xa_destroy 로 정리한다. */
			return rc;
/* [한국어] 찾은 기능을 로그로 남긴다. */

		pci_dbg(doe_mb->pdev,
			"[%x] Found feature %d vid: %x type: %x\n",
			doe_mb->cap_offset, xa_idx, vid, type);

		rc = xa_insert(&doe_mb->feats, xa_idx++,
			       /* [한국어] 저장 키로 하드웨어 인덱스가 아니라 **연속된 xa_idx** 를 쓴다 -- sysfs
			        * populate 가 배열 인덱스를 그대로 쓸 수 있게 하기 위해서다. */
			       pci_doe_xa_feat_entry(vid, type), GFP_KERNEL);
		if (rc)
			/* [한국어] 삽입 실패(중복 키 또는 메모리 부족). */
			return rc;
	/* [한국어] 다음 인덱스가 0 이면 목록의 끝이다. */
	} while (index);

	return 0;
}


/*
 * pci_doe_cancel_tasks:
 *   NVMe 장치 제거, suspend, hot-unplug 등에서 DOE mailbox의 pending/in-progress
 *   작업을 모두 취소한다. DEAD/CANCEL 플래그를 설정하고 대기 중인 work를
 *   깨워 정리한다.
 */
/* [한국어]
 * pci_doe_cancel_tasks - 우편함을 죽은 것으로 표시하고 대기 중인 작업을 깨운다
 *
 * @doe_mb: 대상 우편함.
 * @return: 없음.
 *
 * 두 플래그를 세우는 순서가 의미가 있다:
 *  1. DEAD -- 새 작업이 들어오지 못하게 막는다(submit_task 가 검사).
 *  2. CANCEL -- 이미 대기 중인 작업을 깨운다. pci_doe_wait 가 이 플래그를
 *     조건으로 잠들어 있으므로, wake_up 과 함께 즉시 깨어나 -EIO 로 끝난다.
 *
 * 장치가 사라졌을 때(pci_doe_disconnected) 최대 1초씩 기다리는 폴링 루프를
 * 즉시 끊는 것이 이 함수의 목적이다. 그 상황에서 설정공간 읽기는 전부
 * 0xffffffff 를 돌려주므로 기다려 봐야 소용이 없다.
 *
 * 실행 컨텍스트: 어디서나. 원자적 비트 연산과 wake_up 뿐이라 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_doe_create_mb(실패) / pci_doe_destroy_mb /
 *   pci_doe_disconnected → [이 함수] → set_bit → wake_up
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
/* [한국어]
 * pci_doe_create_mb - 우편함 하나를 만들고 Abort 로 비운 뒤 기능 목록을 캐시한다
 *
 * @pdev: 이 우편함을 가진 장치.
 * @cap_offset: 이 DOE 확장 능력의 설정공간 오프셋.
 * @return: 우편함 포인터, 또는 ERR_PTR 오류.
 *
 * 순서:
 *  1. 구조체 할당과 기본 필드 초기화.
 *  2. **우편함 전용 ordered 작업 큐**를 만든다. ordered 인 것이 핵심 --
 *     한 우편함의 작업이 한 번에 하나씩만 돌아야 하고, 그 덕에 이 파일에
 *     별도의 우편함 잠금이 없다. 큐 이름에 버스·장치·오프셋을 넣어
 *     여러 우편함을 구별할 수 있게 한다.
 *  3. Abort 로 우편함을 비운다. 부트로더나 이전 커널이 남긴 상태를 지우는
 *     단계다. 실패하면 이 우편함은 쓸 수 없다.
 *  4. 기능 목록을 캐시한다. 이 호출이 **작업 큐를 실제로 쓰는 첫 전송**이라,
 *     큐가 살아 있는 뒤에 와야 한다.
 *
 * 되감기 라벨 세 개가 정확히 대칭이다. err_cancel 만 pci_doe_cancel_tasks
 * 를 먼저 부르는데, 이 시점에는 큐에 작업이 떠 있을 수 있어 그것을 먼저
 * 깨워 끝내야 destroy_workqueue 가 매달리지 않기 때문이다.
 *
 * 실행 컨텍스트: 장치 열거 시의 프로세스 문맥. 잠든다.
 *
 * 호출 체인:
 *   pci_doe_init → [이 함수] → alloc_ordered_workqueue → pci_doe_abort
 *     → pci_doe_cache_features
 */
static struct pci_doe_mb *pci_doe_create_mb(struct pci_dev *pdev,
					    u16 cap_offset)
{
	struct pci_doe_mb *doe_mb;
	int rc;
/* [한국어] 먼저 구조체를 잡는다. */

	doe_mb = kzalloc_obj(*doe_mb);
	/* [한국어] 할당 실패. */
	if (!doe_mb)
		/* [한국어] 호출자가 IS_ERR 로 판별하므로 ERR_PTR 로 돌려준다. */
		return ERR_PTR(-ENOMEM);

	doe_mb->pdev = pdev;
	/* [한국어] 이후 모든 설정공간 접근의 기준이 된다. */
	doe_mb->cap_offset = cap_offset;
	/* [한국어] 취소 신호를 기다릴 대기 큐. */
	init_waitqueue_head(&doe_mb->wq);
	xa_init(&doe_mb->feats);

	doe_mb->work_queue = alloc_ordered_workqueue("%s %s DOE [%x]", 0,
						/* [한국어] 큐 이름에 버스·장치·오프셋을 넣어 여러 우편함을 구별할 수 있게 한다. */
						dev_bus_name(&pdev->dev),
						pci_name(pdev),
						doe_mb->cap_offset);
	if (!doe_mb->work_queue) {
		/* [한국어] 큐 생성 실패. */
		pci_err(pdev, "[%x] failed to allocate work queue\n",
			/* [한국어] 어느 우편함인지 표시한다. */
			doe_mb->cap_offset);
		rc = -ENOMEM;
		/* [한국어] 아직 큐가 없으므로 구조체만 해제한다. */
		goto err_free;
	}

	/* Reset the mailbox by issuing an abort */
	rc = pci_doe_abort(doe_mb);
	if (rc) {
		/* [한국어] Abort 실패 -- 이 우편함은 쓸 수 없다. */
		pci_err(pdev, "[%x] failed to reset mailbox with abort command : %d\n",
			/* [한국어] 실패 코드까지 남긴다. */
			doe_mb->cap_offset, rc);
		goto err_destroy_wq;
	}

	/*
	 * The state machine and the mailbox should be in sync now;
	 * Use the mailbox to query features.
	 */
	rc = pci_doe_cache_features(doe_mb);
	if (rc) {
		/* [한국어] 기능 목록 캐시 실패. */
		pci_err(pdev, "[%x] failed to cache features : %d\n",
			/* [한국어] 실패 코드까지 남긴다. */
			doe_mb->cap_offset, rc);
		goto err_cancel;
	}

	return doe_mb;
/* [한국어] 여기까지 오면 우편함이 완전히 준비된 상태다. */

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
/* [한국어]
 * pci_doe_destroy_mb - 우편함 하나를 정리한다
 *
 * @doe_mb: 정리할 우편함.
 * @return: 없음.
 *
 * create 의 역순이다. cancel_tasks 를 **가장 먼저** 부르는 것이 요점 --
 * DEAD 를 세워 새 작업을 막고 CANCEL 로 대기 중인 작업을 깨운 뒤에야
 * destroy_workqueue 가 매달리지 않고 끝난다.
 *
 * xa_destroy 는 xarray 의 내부 노드만 해제한다. 항목 자체는 xa_mk_value 로
 * 만든 값이라 따로 해제할 메모리가 없다.
 *
 * 실행 컨텍스트: 장치 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_doe_init(삽입 실패) / pci_doe_destroy → [이 함수]
 *     → pci_doe_cancel_tasks → xa_destroy → destroy_workqueue → kfree
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
/* [한국어]
 * pci_doe_supports_feat - 이 우편함이 그 [벤더, 타입] 기능을 지원하는지 본다
 *
 * @doe_mb: 대상 우편함.
 * @vid: 벤더 ID.
 * @type: 기능 타입.
 * @return: true 면 지원.
 *
 * Discovery 자체는 캐시에 없어도 **항상 참**이다. 모든 DOE 우편함이
 * 반드시 지원하는 기능이고, 애초에 캐시를 채우는 데 그것을 써야 하므로
 * 닭과 달걀 문제를 이 특례로 푼다.
 *
 * 나머지는 캐시된 xarray 를 훑는다. 값을 풀지 않고 **인코딩된 값끼리**
 * 비교하는 점이 요령이다 -- pci_doe_xa_feat_entry 로 같은 인코딩을 만들어
 * 포인터 비교하면 된다.
 *
 * 선형 탐색이지만 우편함당 기능이 몇 개뿐이라 문제되지 않는다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_doe_submit_task / pci_find_doe_mailbox → [이 함수]
 */
static bool pci_doe_supports_feat(struct pci_doe_mb *doe_mb, u16 vid, u8 type)
{
	unsigned long index;
	void *entry;
/* [한국어] Discovery 가 아닌 기능은 캐시에서 찾는다. */

	/* The discovery feature must always be supported */
	if (vid == PCI_VENDOR_ID_PCI_SIG && type == PCI_DOE_FEATURE_DISCOVERY)
		return true;

	xa_for_each(&doe_mb->feats, index, entry)
		/* [한국어] **인코딩된 값끼리 비교한다** -- 값을 풀 필요 없이 같은 인코딩을 만들어
		 * 포인터 비교하면 된다. */
		if (entry == pci_doe_xa_feat_entry(vid, type))
			/* [한국어] 지원한다. */
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
/* [한국어]
 * pci_doe_submit_task - 작업을 검증하고 우편함 작업 큐에 넣는다
 *
 * @doe_mb: 대상 우편함.
 * @task: 넣을 작업. work 는 **호출자 스택에** 있다.
 * @return: 0 성공, -EINVAL 은 미지원 기능, -EIO 는 죽은 우편함.
 *
 * 두 가지를 먼저 거른다: 그 기능을 지원하지 않는 우편함에 보내는 요청과,
 * 이미 DEAD 로 표시된 우편함이다. 둘 다 큐에 넣어 봐야 워커가 같은 판정을
 * 하고 실패로 끝낼 뿐이다.
 *
 * INIT_WORK_ONSTACK 을 쓰는 이유: task 가 호출자 스택에 있어 일반
 * INIT_WORK 의 디버그 객체 추적이 맞지 않는다. 짝이 되는
 * destroy_work_on_stack 은 signal_task_complete 가 부른다.
 *
 * 큐에 넣은 뒤 곧바로 돌아온다 -- 완료를 기다리는 것은 호출자
 * (pci_doe)의 몫이다.
 *
 * 코드 관찰 (상류 그대로, 수정하지 않음): DEAD 검사와 queue_work 사이에
 * 잠금이 없어, 그 사이에 pci_doe_cancel_tasks 가 DEAD 를 세울 수 있다.
 * 다만 워커가 실행 초입에서 DEAD 를 다시 검사하므로 결과는 -EIO 로 같다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   pci_doe → [이 함수] → pci_doe_supports_feat → queue_work
 */
static int pci_doe_submit_task(struct pci_doe_mb *doe_mb,
			       struct pci_doe_task *task)
{
	if (!pci_doe_supports_feat(doe_mb, task->feat.vid, task->feat.type))
		return -EINVAL;

	if (test_bit(PCI_DOE_FLAG_DEAD, &doe_mb->flags))
		/* [한국어] 이미 죽은 우편함이다. */
		return -EIO;

	task->doe_mb = doe_mb;
	/* [한국어] task 가 호출자 스택에 있어 _ONSTACK 판이 필요하다. 짝이 되는
	 * destroy_work_on_stack 은 signal_task_complete 가 부른다. */
	INIT_WORK_ONSTACK(&task->work, doe_statemachine_work);
	/* [한국어] 큐에 넣고 곧바로 돌아온다 -- 완료 대기는 호출자의 몫이다. */
	queue_work(doe_mb->work_queue, &task->work);
	/* [한국어] 코드 관찰: DEAD 검사와 queue_work 사이에 잠금이 없어 그 사이에
	 * cancel_tasks 가 DEAD 를 세울 수 있다. 다만 워커가 초입에서 다시
	 * 검사하므로 결과는 -EIO 로 같다. */
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
/* [한국어]
 * pci_doe - DOE 메시지를 보내고 응답을 받는 동기 API (외부 공개)
 *
 * @doe_mb: 쓸 우편함. pci_find_doe_mailbox() 로 얻는다.
 * @vendor: 기능의 벤더 ID.
 * @type: 기능 타입.
 * @request: 보낼 페이로드.
 * @request_sz: 그 크기(바이트). 4의 배수가 아니어도 된다.
 * @response: 응답을 담을 버퍼.
 * @response_sz: 그 크기.
 * @return: 응답 바이트 수(양수), 또는 음수 오류.
 *
 * 이 파일의 유일한 전송 API 다. 스택에 completion 과 task 를 잡고 큐에
 * 넣은 뒤 완료를 기다린다 -- 그래서 **반드시 잠들 수 있는 문맥**에서만
 * 부를 수 있다.
 *
 * 반환값이 요청한 크기보다 작을 수 있다. 장치가 더 짧게 답한 경우이고,
 * 호출자가 그것을 확인해야 한다(pci_doe_discovery 가 정확히 그렇게 한다).
 *
 * 헤더 두 워드는 이 계층이 붙이고 떼므로, 호출자는 페이로드만 다루면 된다.
 *
 * 이 트리의 소비자는 drivers/pci/tsm.c 다 -- pci_tsm_doe_transfer() 가
 * SPDM/CMA 메시지를 이 함수로 실어 나른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. wait_for_completion 으로 잠든다.
 *
 * 호출 체인:
 *   pci_tsm_doe_transfer / pci_doe_discovery → [이 함수]
 *     → pci_doe_submit_task → (워커) → wait_for_completion
 */
int pci_doe(struct pci_doe_mb *doe_mb, u16 vendor, u8 type,
	    const void *request, size_t request_sz,
	    void *response, size_t response_sz)
{
	DECLARE_COMPLETION_ONSTACK(c);
	struct pci_doe_task task = {
		/* [한국어] 기능의 벤더 ID. */
		.feat.vid = vendor,
		/* [한국어] 기능 타입. 이 둘이 메시지의 목적지를 정한다. */
		.feat.type = type,
		.request_pl = request,
		.request_pl_sz = request_sz,
		.response_pl = response,
		.response_pl_sz = response_sz,
		.complete = pci_doe_task_complete,
		.private = &c,
	};
	int rc;
/* [한국어] 이제 큐에 넣는다. */

	rc = pci_doe_submit_task(doe_mb, &task);
	/* [한국어] 미지원 기능이거나 죽은 우편함이다. */
	if (rc)
		/* [한국어] 기다릴 필요 없이 곧바로 실패로 끝낸다. */
		return rc;
/* [한국어] 큐에 들어갔으므로 완료를 기다린다. */

	wait_for_completion(&c);

	return task.rv;
/* [한국어] 워커가 채워 둔 결과를 돌려준다. 양수면 응답 바이트 수다. */
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
/* [한국어]
 * pci_find_doe_mailbox - 그 기능을 지원하는 우편함을 이 장치에서 찾는다
 *
 * @pdev: 대상 장치.
 * @vendor: 찾는 기능의 벤더 ID.
 * @type: 기능 타입.
 * @return: 우편함, 또는 없으면 NULL.
 *
 * 장치에 DOE 우편함이 여럿일 수 있고 각자 지원 기능이 다르다. 소비자는
 * 오프셋을 알 필요 없이 '이 기능을 쓸 수 있는 우편함' 을 이 함수로 얻는다.
 *
 * 첫 번째로 맞는 것을 돌려주므로, 같은 기능을 두 우편함이 모두 지원하면
 * xarray 순회 순서(= 오프셋 순서)가 이른 쪽이 선택된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_tsm_link_constructor(tsm.c:1881) → [이 함수]
 *     → pci_doe_supports_feat
 */
struct pci_doe_mb *pci_find_doe_mailbox(struct pci_dev *pdev, u16 vendor,
					u8 type)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;
/* [한국어] 장치의 모든 우편함을 훑는다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		/* [한국어] 그 기능을 지원하는 첫 우편함을 찾는다. */
		if (pci_doe_supports_feat(doe_mb, vendor, type))
			/* [한국어] xarray 키가 오프셋이라, 설정공간 순서가 이른 쪽이 선택된다. */
			return doe_mb;
/* [한국어] 어느 우편함도 지원하지 않는다. */

	return NULL;
}
EXPORT_SYMBOL_GPL(pci_find_doe_mailbox);


/*
 * pci_doe_init:
 *   NVMe 장치 probe 시 PCI core가 호출하여 모든 DOE 확장 capability를
 *   찾고 각각에 대한 mailbox를 생성한다. 생성된 mailbox는 sysfs를 통해
 *   NVMe 사용자공간 도구에 노출될 수 있다.
 */
/* [한국어]
 * pci_doe_init - 장치의 모든 DOE 확장 능력을 찾아 우편함을 만든다
 *
 * @pdev: 대상 장치.
 * @return: 없음. 실패를 알릴 통로가 없다.
 *
 * 확장 능력 사슬을 훑으며 DOE 능력을 만날 때마다 우편함을 만든다.
 * 키로 **오프셋**을 쓰므로, 우편함이 설정공간 순서대로 정렬된다.
 *
 * 실패해도 루프를 계속하는 것이 이 함수의 특징이다:
 *  - 우편함 생성 실패 → 로그를 남기고 `continue`. 다른 우편함은 여전히
 *    쓸 수 있어야 하기 때문이다.
 *  - xarray 삽입 실패 → 방금 만든 우편함을 파괴하고 계속.
 * 그래서 일부만 살아남은 상태로 끝날 수 있고, 그것이 의도된 동작이다.
 *
 * xa_init 을 먼저 부르므로, DOE 능력이 하나도 없는 장치도 빈 xarray 를
 * 갖는다 -- sysfs 가시성 판정(xa_empty)과 destroy 가 그것을 전제한다.
 *
 * 실행 컨텍스트: 장치 열거 시의 프로세스 문맥. 잠든다.
 *
 * 호출 체인:
 *   PCI 코어(장치 추가) → [이 함수] → pci_find_next_ext_capability
 *     → pci_doe_create_mb → xa_insert
 */
void pci_doe_init(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	u16 offset = 0;
	/* [한국어] 삽입 결과. */
	int rc;
/* [한국어] 빈 xarray 를 먼저 만들어 둔다 -- DOE 능력이 없는 장치도 이 상태여야
 * sysfs 가시성 판정과 destroy 가 성립한다. */

	xa_init(&pdev->doe_mbs);

	while ((offset = pci_find_next_ext_capability(pdev, offset,
						      /* [한국어] 확장 능력 사슬을 훑으며 DOE 능력을 모두 찾는다. offset 이 갱신되며
						       * 다음 능력으로 넘어간다. */
						      PCI_EXT_CAP_ID_DOE))) {
		doe_mb = pci_doe_create_mb(pdev, offset);
		/* [한국어] 우편함 생성 실패. */
		if (IS_ERR(doe_mb)) {
			/* [한국어] 어느 오프셋에서 실패했는지 남긴다. */
			pci_err(pdev, "[%x] failed to create mailbox: %ld\n",
				/* [한국어] PTR_ERR 로 오류 코드를 꺼낸다. */
				offset, PTR_ERR(doe_mb));
			continue;
		}

		rc = xa_insert(&pdev->doe_mbs, offset, doe_mb, GFP_KERNEL);
		/* [한국어] xarray 삽입 실패. */
		if (rc) {
			/* [한국어] 어느 오프셋인지 남긴다. */
			pci_err(pdev, "[%x] failed to insert mailbox: %d\n",
				/* [한국어] 방금 만든 우편함을 파괴하고 **루프는 계속한다** -- 다른 우편함은 여전히
				 * 쓸 수 있어야 하기 때문이다. */
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
/* [한국어]
 * pci_doe_destroy - 이 장치의 모든 우편함을 파괴하고 xarray 를 비운다
 *
 * @pdev: 대상 장치.
 * @return: 없음.
 *
 * init 의 역순이다. 우편함마다 destroy_mb 를 부른 뒤 xarray 자체를 비운다.
 * 순서가 반대면 순회 중에 항목이 사라진다.
 *
 * 이 시점에는 sysfs 파일이 이미 pci_doe_sysfs_teardown 으로 걷혔어야 한다 --
 * 그러지 않으면 사라진 우편함의 이름 문자열을 sysfs 가 참조하게 된다.
 *
 * 실행 컨텍스트: 장치 제거 경로의 프로세스 문맥.
 *
 * 호출 체인:
 *   PCI 코어(장치 제거) → [이 함수] → pci_doe_destroy_mb → xa_destroy
 */
void pci_doe_destroy(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;
/* [한국어] 우편함마다 파괴한다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		/* [한국어] 순회 중에 xarray 자체는 건드리지 않고, 아래에서 한꺼번에 비운다. */
		pci_doe_destroy_mb(doe_mb);

	xa_destroy(&pdev->doe_mbs);
}


/*
 * pci_doe_disconnected:
 *   NVMe 장치가 hotplug 등으로 연결이 끊어졌을 때 모든 DOE mailbox의
 *   pending/in-progress 작업을 즉시 취소한다. 장치가 응답하지 않는 상황에서
 *   무한 대기를 방지한다.
 */
/* [한국어]
 * pci_doe_disconnected - 장치가 사라졌을 때 진행 중인 전송을 즉시 끊는다
 *
 * @pdev: 사라진 장치.
 * @return: 없음.
 *
 * 표면 제거(surprise removal)나 링크 다운으로 장치가 응답하지 않게 되면,
 * 폴링 루프들이 각각 최대 1초씩 헛되이 기다린다. 이 함수가 모든 우편함에
 * CANCEL 을 세워 그 대기를 즉시 깨운다.
 *
 * 우편함 자체를 해제하지는 않는다 -- 그것은 pci_doe_destroy 의 몫이고,
 * 이 함수는 '더 기다리지 마라' 는 신호만 보낸다. DEAD 도 함께 세워지므로
 * 이후 새 작업은 곧바로 거절된다.
 *
 * 실행 컨텍스트: 제거·오류 처리 경로. 잠들지 않으므로 인터럽트 문맥에서도
 * 안전하다.
 *
 * 호출 체인:
 *   PCI 코어(장치 연결 끊김) → [이 함수] → pci_doe_cancel_tasks
 */
void pci_doe_disconnected(struct pci_dev *pdev)
{
	struct pci_doe_mb *doe_mb;
	unsigned long index;
/* [한국어] 우편함마다 취소 신호를 보낸다. */

	xa_for_each(&pdev->doe_mbs, index, doe_mb)
		/* [한국어] 해제하지는 않는다 -- '더 기다리지 마라' 는 신호만 보내고, 실제 해제는
		 * pci_doe_destroy 가 한다. */
		pci_doe_cancel_tasks(doe_mb);
}
