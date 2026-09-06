// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2014 Intel Corp.
 * Author: Jiang Liu <jiang.liu@linux.intel.com>
 *
 * This file is licensed under GPLv2.
 *
 * This file contains common code to support Message Signaled Interrupts for
 * PCI compatible and non PCI compatible devices.
 */

/*
 * [한국어 설명] 메시지 신호 인터럽트(MSI) 코어 (msi.c)
 *
 * === 파일의 역할 ===
 * MSI(Message Signaled Interrupt)는 인터럽트 선을 흔드는 대신 장치가
 * 정해진 메모리 주소에 정해진 값을 쓰는 방식이다. 그 쓰기를 인터럽트
 * 컨트롤러가 받아 CPU 에 인터럽트를 올린다. 선이 필요 없으므로 장치
 * 하나가 수천 개의 벡터를 쓸 수 있고, 벡터마다 다른 CPU 를 향하게 할
 * 수도 있다. 다중 큐 NVMe 나 네트워크 카드가 그 위에 서 있다.
 *
 * 이 파일은 그 기구의 공용 절반을 구현한다. PCI 든 플랫폼 장치든
 * IMS(Interrupt Message Store)든, 공통으로 필요한 것은 같다 — 장치마다
 * MSI 서술자를 보관하고, 인터럽트 도메인을 만들고, 번호를 할당하고,
 * 메시지(주소+데이터)를 조립해 장치에 써 넣는 일이다.
 *
 * 핵심 자료구조가 셋이다. struct msi_desc 는 벡터 하나(또는 PCI 멀티
 * MSI 에서는 여러 개)의 정보를, struct msi_device_data 는 장치 하나의
 * 서술자 저장소 전체를, struct msi_domain_info 는 도메인 하나의 설정과
 * 콜백을 담는다.
 *
 * 서술자 저장소로 xarray 를 쓰는 것이 이 파일의 중요한 선택이다. MSI-X
 * 인덱스는 듬성듬성 쓰일 수 있고(장치가 4096 개 중 3 개만 쓸 수 있다)
 * 동적으로 늘고 줄기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * MSI 인터럽트 하나가 동작하기까지의 층은 이렇다.
 *
 *   PCI 장치의 MSI-X 테이블 항목
 *     → 장치 MSI 도메인 (per-device, 이 파일이 만든다)
 *     → MSI 부모 도메인 (IOMMU 인터럽트 리매핑 등)
 *     → 벡터 도메인 (아키텍처, CPU 벡터 번호를 배정)
 *     → CPU
 *
 * 이 파일은 위 두 층을 담당한다. 아래 두 층은 아키텍처와 IOMMU 코드가
 * 제공하고, irq_chip_compose_msi_msg() (kernel/irq/chip.c)를 통해
 * "이 벡터의 메시지 주소와 데이터는 무엇인가" 를 물어 온다.
 *
 * 장치별 도메인이라는 개념이 이 파일의 비교적 최근 설계다. 예전에는
 * 버스마다 도메인 하나를 공유했는데, 그러면 장치마다 다른 제약(마스크
 * 가능 여부, 벡터 수 한계)을 표현할 수 없었다. 지금은 장치마다 자기
 * 도메인을 만들고, 부모 도메인이 그 능력을 깎거나 늘린다.
 *
 * 실행 컨텍스트는 대부분 프로세스 문맥이다. 할당·해제 경로가 잠들 수
 * 있는 할당을 쓰고 dev->msi.data->mutex 를 잡는다. 인터럽트 문맥에서
 * 불리는 것은 msi_domain_set_affinity() 정도다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(이 파일을 부르는 쪽):
 *   - drivers/pci/msi/ → PCI MSI/MSI-X 설정. 가장 큰 소비자다.
 *   - drivers/base/platform-msi.c → 플랫폼 장치 MSI
 *   - drivers/irqchip 의 wire-to-MSI 브리지 (MBIGEN 등)
 *
 * 아래쪽(이 파일이 부르는 쪽):
 *   - kernel/irq/irqdomain.c → 도메인 생성과 인터럽트 할당
 *   - kernel/irq/chip.c → irq_chip_compose_msi_msg(), irq_set_msi_desc_off()
 *   - lib/xarray.c → 서술자 저장소
 *
 * 공유 자료구조: struct device 의 msi 필드(dev->msi.data, dev->msi.domain)
 * 가 이 서브시스템의 진입점이다. 그 아래 xarray 들이 장치의 모든 MSI
 * 서술자를 담는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - msi_setup_device_data(): 장치에 MSI 저장소를 붙인다. devres 로
 *   관리되어 장치가 사라질 때 자동 해제된다.
 * - msi_create_device_irq_domain(): 장치 전용 MSI 도메인을 만든다.
 *   부모 도메인이 능력을 조정하는 협상이 여기서 일어난다.
 * - __msi_domain_alloc_irqs(): 서술자들을 훑으며 실제 인터럽트 번호를
 *   할당하고 메시지를 써 넣는다. 이 파일의 심장이다.
 * - msi_domain_alloc_irq_at(): 인덱스 하나에 인터럽트 하나를 할당한다.
 *   IMS 처럼 동적으로 벡터를 늘리는 경우에 쓴다.
 * - struct msi_device_data: 장치 하나의 MSI 상태 전부.
 * - struct msi_ctrl: 할당·해제 연산의 대상 범위를 담는 내부 구조체.
 */

#include <linux/device.h>	/* [한국어] struct device, devres_alloc — MSI 데이터를 장치 수명에 묶는다 */
#include <linux/irq.h>	/* [한국어] struct irq_data, irq_chip — 인터럽트 코어 자료구조 */
#include <linux/irqdomain.h>	/* [한국어] 도메인 생성과 계층 구성. 이 파일 절반이 도메인 조작이다 */
#include <linux/msi.h>	/* [한국어] struct msi_desc, msi_domain_info, MSI_FLAG_ 상수 — 이 파일이 다루는 자료구조의 공개 정의 */
#include <linux/mutex.h>	/* [한국어] 서술자 저장소를 지키는 뮤텍스 */
#include <linux/pci.h>	/* [한국어] dev_is_pci(), to_pci_dev() — PCI 특유의 예외 처리에 필요하다 */
#include <linux/slab.h>	/* [한국어] kzalloc/kfree — 서술자와 도메인 템플릿 할당 */
#include <linux/seq_file.h>	/* [한국어] debugfs 출력용 seq_printf */
#include <linux/sysfs.h>	/* [한국어] /sys/.../msi_irqs/ 항목 생성 */
#include <linux/types.h>	/* [한국어] 기본 타입 정의 */
#include <linux/xarray.h>	/* [한국어] 서술자 저장소. MSI-X 인덱스가 듬성듬성해 배열로는 낭비가 크다 */

#include "internals.h"	/* [한국어] 코어 전용 선언 (irq_debugfs_copy_devname, __irq_domain_alloc_irqs 등) */

/**
 * struct msi_device_data - MSI per device data
 * @properties:		MSI properties which are interesting to drivers
 * @mutex:		Mutex protecting the MSI descriptor store
 * @__domains:		Internal data for per device MSI domains
 * @__iter_idx:		Index to search the next entry for iterators
 */
struct msi_device_data {
	/* [한국어] 장치 하나의 MSI 상태 전부를 담는 구조체.
	 * dev->msi.data 가 이것을 가리키며, devres 로 관리되어 장치가
	 * 사라질 때 자동으로 해제된다. */
	unsigned long			properties;
	/* [한국어] 드라이버가 알아야 하는 MSI 속성 비트들.
	 * 설정자: PCI 계층 등이 msi_device_set_properties() 로 설정한다.
	 * 읽는 자: 드라이버가 msi_device_has_property() 로 묻는다.
	 * 값 범위: MSI_PROP_ 계열 비트 조합.
	 * 동기화: 설정이 초기화 시점에 한 번뿐이라 별도 보호가 없다. */
	struct mutex			mutex;
	/* [한국어] 아래 서술자 저장소를 지키는 뮤텍스.
	 * 설정자: msi_setup_device_data() 가 초기화한다.
	 * 읽는 자: guard(msi_descs_lock) 과 __msi_lock_descs() 계열.
	 * 값 범위: 뮤텍스 — 이 아래 경로가 GFP_KERNEL 할당을 하므로
	 *   잠들 수 있어야 한다. 그래서 스핀락일 수 없다.
	 * 동기화: 이것이 동기화 수단 자체다. 저장소 조작뿐 아니라
	 *   __iter_idx 를 쓰는 순회 전체를 이 락이 감싼다. */
	struct msi_dev_domain		__domains[MSI_MAX_DEVICE_IRQDOMAINS];
	/* [한국어] 도메인 ID 별 저장소와 도메인 포인터의 배열.
	 * 설정자: msi_setup_device_data() 가 xarray 를 초기화하고,
	 *   msi_create_device_irq_domain() 이 도메인 포인터를 채운다.
	 * 읽는 자: 이 파일의 거의 모든 함수가 domid 로 인덱싱한다.
	 * 값 범위: MSI_DEFAULT_DOMAIN(0) 부터 MSI_SECONDARY_DOMAIN 까지.
	 *   한 장치가 여러 종류의 MSI 를 동시에 쓸 수 있어 배열이다 —
	 *   예를 들어 PCI MSI-X 와 장치 고유의 IMS 를 함께 쓰는 경우다.
	 * 동기화: 위 mutex 아래에서만 만진다.
	 * 이름 앞의 두 밑줄: "직접 만지지 말고 접근자를 쓰라" 는 관례다. */
	unsigned long			__iter_idx;
	/* [한국어] 순회 상태를 담는 커서.
	 * 설정자: msi_domain_first_desc() 가 0 으로 놓고,
	 *   msi_next_desc() 와 msi_find_desc() 가 전진시킨다.
	 *   __msi_unlock_descs() 가 MSI_XA_MAX_INDEX 로 무효화한다.
	 * 읽는 자: msi_find_desc().
	 * 값 범위: 0 ~ MSI_MAX_INDEX, 또는 무효 표시인 MSI_XA_MAX_INDEX.
	 * 동기화: mutex 가 지킨다. 순회 상태를 구조체에 두는 것은
	 *   중첩 순회를 허용하지 않는다는 뜻이고, 락을 놓을 때 무효화하는
	 *   것이 그 규약을 강제한다.
	 *
	 * 왜 호출자의 스택이 아니라 여기 두는가: msi_for_each_desc 매크로가
	 *   커서 변수를 노출하지 않고 쓸 수 있게 하려는 것이다. 그 대가로
	 *   순회 중에 락을 놓을 수 없다. */
};

/**
 * struct msi_ctrl - MSI internal management control structure
 * @domid:	ID of the domain on which management operations should be done
 * @first:	First (hardware) slot index to operate on
 * @last:	Last (hardware) slot index to operate on
 * @nirqs:	The number of Linux interrupts to allocate. Can be larger
 *		than the range due to PCI/multi-MSI.
 */
struct msi_ctrl {
	/* [한국어] 할당·해제 연산의 대상을 한 덩어리로 묶은 내부 구조체.
	 * 인자 네 개를 함수마다 늘어놓는 대신 구조체로 넘긴다. 이 파일의
	 * 내부에서만 쓰이며 외부에 노출되지 않는다. */
	unsigned int			domid;
	/* [한국어] 어느 도메인에 대한 연산인가.
	 * 설정자: 각 공개 API 가 자기 인자에서 채운다.
	 * 읽는 자: msi_ctrl_valid(), msi_get_device_domain() 등.
	 * 값 범위: 0 ~ MSI_MAX_DEVICE_IRQDOMAINS-1.
	 * 동기화: 스택에 있는 지역 구조체라 공유되지 않는다. */
	unsigned int			first;
	/* [한국어] 대상 인덱스 범위의 시작 (포함).
	 * 설정자: 공개 API 의 인자.
	 * 읽는 자: xa_for_each_range() 의 하한.
	 * 값 범위: 0 ~ hwsize-1.
	 * 동기화: 위와 같다. */
	unsigned int			last;
	/* [한국어] 대상 인덱스 범위의 끝 (포함).
	 * 설정자·읽는 자: first 와 같다.
	 * 값 범위: first ~ hwsize-1. msi_ctrl_valid() 가 first <= last 를
	 *   확인한다.
	 * 동기화: 위와 같다. */
	unsigned int			nirqs;
	/* [한국어] 할당할 리눅스 인터럽트의 개수.
	 * 설정자: 할당 API 만 채운다. 해제 경로에서는 0 으로 남는다.
	 * 읽는 자: __msi_domain_alloc_irqs() 의 초과 할당 검사.
	 * 값 범위: 1 이상. 원본 주석대로 (last - first + 1) 보다 클 수
	 *   있는데, PCI 멀티 MSI 에서는 서술자 하나가 여러 벡터를
	 *   대표하기 때문이다. 인덱스 범위는 서술자 단위이고 이 값은
	 *   인터럽트 단위라 단위가 다르다.
	 * 동기화: 위와 같다. */
};

/* Invalid Xarray index which is outside of any searchable range */
#define MSI_XA_MAX_INDEX	(ULONG_MAX - 1)	/* [한국어] (위 영어 주석) 순회 커서를 무효화할 때 쓰는 값. 어떤 검색 범위에도 들지 않아, 이 값이 들어 있으면 순회가 아무것도 찾지 못한다. ULONG_MAX 자체가 아니라 하나 작은 값인 이유는 xarray 가 ULONG_MAX 를 내부 표식으로 쓰기 때문이다 */
/* The maximum domain size */
#define MSI_XA_DOMAIN_SIZE	(MSI_MAX_INDEX + 1)	/* [한국어] (위 영어 주석) 도메인이 가질 수 있는 최대 인덱스 수. 하드웨어 크기를 모르는 도메인의 기본값으로도 쓰인다 */

static void msi_domain_free_locked(struct device *dev, struct msi_ctrl *ctrl);
/* [한국어] 아래에서 정의되는 해제 함수의 전방 선언.
 * 설정자: 파일 뒤쪽의 정의.
 * 읽는 자: msi_domain_alloc_locked() 의 실패 경로,
 *   __msi_domain_alloc_irq_at() 의 실패 경로.
 * 값 범위: 해당 없음 (함수).
 * 동기화: 호출자가 dev->msi.data->mutex 를 쥐고 있어야 한다.
 *
 * 전방 선언이 필요한 이유: 할당 함수가 실패 시 해제를 부르는데,
 * 해제 정의는 파일 끝 가까이에 있다. 할당과 해제를 각각 모아 두는
 * 배치를 유지하면서 순환 참조를 푸는 방법이다. */
static unsigned int msi_domain_get_hwsize(struct device *dev, unsigned int domid);
/* [한국어] 도메인의 하드웨어 인덱스 수를 얻는 함수의 전방 선언.
 * 설정자: 파일 중간의 정의.
 * 읽는 자: msi_insert_desc(), msi_ctrl_valid() 등 파일 앞쪽 함수들.
 * 값 범위: 1 ~ MSI_XA_DOMAIN_SIZE.
 * 동기화: 호출자가 뮤텍스를 쥐고 있어야 한다 (내부에서
 *   msi_get_device_domain 을 부르고 그것이 lockdep 으로 확인한다). */
static inline int msi_sysfs_create_group(struct device *dev);
/* [한국어] sysfs 그룹 생성 함수의 전방 선언.
 * 설정자: CONFIG_SYSFS 여부에 따라 두 판 중 하나.
 * 읽는 자: msi_setup_device_data().
 * 값 범위: 0 성공, 음수 오류. sysfs 없는 빌드에서는 항상 0.
 * 동기화: 불필요.
 *
 * inline 인데도 전방 선언이 필요한 이유: 정의가 아래에 있고 호출은
 * 위에 있어서다. inline 은 링크 방식일 뿐 선언 순서를 면제하지 않는다. */
static int msi_domain_prepare_irqs(struct irq_domain *domain, struct device *dev,
				   int nvec, msi_alloc_info_t *arg);
/* [한국어] 도메인의 msi_prepare 콜백을 부르는 함수의 전방 선언.
 * 설정자: 파일 뒤쪽의 정의.
 * 읽는 자: msi_create_device_irq_domain(), populate_alloc_info().
 * 값 범위: 0 성공, 음수 오류.
 * 동기화: 호출자를 따른다. */

/**
 * msi_alloc_desc - Allocate an initialized msi_desc
 * @dev:	Pointer to the device for which this is allocated
 * @nvec:	The number of vectors used in this entry
 * @affinity:	Optional pointer to an affinity mask array size of @nvec
 *
 * If @affinity is not %NULL then an affinity array[@nvec] is allocated
 * and the affinity masks and flags from @affinity are copied.
 *
 * Return: pointer to allocated &msi_desc on success or %NULL on failure
 */
/*
 * [한국어]
 * msi_alloc_desc - MSI 서술자 하나를 할당하고 기본값을 채운다
 *
 * @dev:      이 서술자가 속할 장치
 * @nvec:     이 항목이 대표하는 벡터의 수
 * @affinity: 벡터별 친화도 배열 (nvec 개) 또는 NULL
 * @return:   서술자 포인터, 실패 시 NULL
 *
 * nvec 이 1 보다 큰 경우가 PCI 멀티 MSI 다. 그 방식은 벡터를 2 의
 * 거듭제곱 개수로 연속 할당하고, 장치는 기본 메시지 데이터에 벡터
 * 번호를 더해 보낸다. 그래서 서술자 하나가 여러 인터럽트를 대표한다.
 * MSI-X 는 항목마다 독립적이라 항상 1 이다.
 *
 * 친화도 배열을 복사하는 이유: 호출자가 넘긴 배열이 스택에 있거나
 * 곧 해제될 수 있다. 서술자는 인터럽트가 살아 있는 동안 유지되므로
 * 자기 사본을 가져야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 호출 체인:
 *   msi_domain_insert_msi_desc() / msi_domain_add_simple_msi_descs() /
 *   __msi_domain_alloc_irq_at() → [이 함수]
 */
static struct msi_desc *msi_alloc_desc(struct device *dev, int nvec,
				       const struct irq_affinity_desc *affinity)
{
	struct msi_desc *desc = kzalloc_obj(*desc);	/* [한국어] 0 으로 채워 할당한다. msi_index, irq 등이 0 이 되는 것이 유효한 초기 상태다 */

	if (!desc)	/* [한국어] 할당 실패 */
		return NULL;

	desc->dev = dev;	/* [한국어] 소속 장치. 서술자만 들고 다니는 코드가 장치를 되찾는 통로다 */
	desc->nvec_used = nvec;	/* [한국어] 이 항목이 대표하는 벡터 수. PCI 멀티 MSI 에서만 1 보다 크다 */
	if (affinity) {	/* [한국어] 호출자가 벡터별 친화도를 지정했는가 */
		desc->affinity = kmemdup_array(affinity, nvec, sizeof(*desc->affinity), GFP_KERNEL);	/* [한국어] 복사한다. 호출자의 배열이 스택에 있거나 곧 사라질 수 있어 참조만 들 수 없다 */
		if (!desc->affinity) {	/* [한국어] 복사 실패 */
			kfree(desc);	/* [한국어] 방금 잡은 서술자도 되돌린다 */
			return NULL;	/* [한국어] 친화도 배열 복사에 실패했다. 서술자도 이미 해제했으므로 호출자에게 넘길 것이 없다 */
		}
	}
	return desc;	/* [한국어] 아직 저장소에 넣지 않았다. 넣는 것은 msi_insert_desc 가 한다 */
}

/*
 * [한국어]
 * msi_free_desc - MSI 서술자를 해제한다
 *
 * @desc: 해제할 서술자
 * @return: 없음
 *
 * msi_alloc_desc() 의 반대다. 친화도 배열을 먼저 풀고 본체를 푼다.
 *
 * 주의: 이 함수는 저장소(xarray)에서 빼 주지 않는다. 이미 저장소에
 * 들어간 서술자를 이것으로 풀면 저장소에 대롱거리는 포인터가 남는다.
 * 호출자들이 xa_erase 를 먼저 하거나, 아직 넣지 않은 서술자에만 부른다.
 *
 * desc->affinity 가 NULL 이어도 kfree(NULL) 은 안전하므로 검사가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_insert_desc() 의 실패 경로 / msi_domain_free_descs() → [이 함수]
 */
static void msi_free_desc(struct msi_desc *desc)
{
	kfree(desc->affinity);	/* [한국어] 친화도 배열. NULL 이어도 안전해 검사가 없다 */
	kfree(desc);	/* [한국어] 서술자 본체. 저장소에서 빼는 것은 호출자 몫이다 */
}

/*
 * [한국어]
 * msi_insert_desc - 서술자를 저장소의 지정 인덱스에 넣는다
 *
 * @dev:   대상 장치
 * @desc:  넣을 서술자
 * @domid: 어느 도메인의 저장소인가
 * @index: 넣을 인덱스, 또는 MSI_ANY_INDEX (빈 자리를 찾아 달라)
 * @return: 0 성공, -ERANGE 인덱스가 하드웨어 크기를 넘음,
 *          -EBUSY 이미 쓰이는 인덱스, -ENOMEM 저장소 확장 실패
 *
 * 두 가지 모드가 있다. 인덱스를 지정하는 모드는 PCI MSI-X 처럼
 * 하드웨어 테이블의 특정 항목에 대응해야 하는 경우이고,
 * MSI_ANY_INDEX 모드는 IMS 처럼 소프트웨어가 인덱스를 자유롭게 정할
 * 수 있는 경우다.
 *
 * 실패 시 서술자를 여기서 해제하는 것에 주목: 호출자가 되돌릴 필요가
 * 없다는 규약이다. 넣기에 실패한 서술자는 어차피 쓸 데가 없고,
 * 호출자마다 해제 코드를 두면 빠뜨리기 쉽다.
 *
 * hwsize 검사가 중요한 이유: 인덱스가 하드웨어 테이블 크기를 넘으면
 * 나중에 그 항목을 만지려 할 때 테이블 밖 메모리를 건드리게 된다.
 *
 * 안쪽 index 변수가 바깥 인자를 가리는 것에 주의 — xa_alloc 이 찾은
 * 값을 담을 별도 변수가 필요해서다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev->msi.data->mutex 보유.
 *
 * 호출 체인:
 *   msi_domain_insert_msi_desc() / msi_domain_add_simple_msi_descs() /
 *   __msi_domain_alloc_irq_at() → [이 함수] → xa_alloc() / xa_insert()
 */
static int msi_insert_desc(struct device *dev, struct msi_desc *desc,
			   unsigned int domid, unsigned int index)
{
	struct msi_device_data *md = dev->msi.data;	/* [한국어] 장치의 MSI 상태 */
	struct xarray *xa = &md->__domains[domid].store;	/* [한국어] 이 도메인의 서술자 저장소 */
	unsigned int hwsize;	/* [한국어] 하드웨어 테이블 크기 */
	int ret;	/* [한국어] xarray 연산 결과 */

	hwsize = msi_domain_get_hwsize(dev, domid);	/* [한국어] 이 도메인이 표현할 수 있는 최대 인덱스 수 */

	if (index == MSI_ANY_INDEX) {	/* [한국어] 아무 빈 자리나 좋다는 요청인가 — IMS 처럼 소프트웨어가 인덱스를 정하는 경우다 */
		struct xa_limit limit = { .min = 0, .max = hwsize - 1 };	/* [한국어] 검색 범위를 하드웨어 크기로 제한한다 */
		unsigned int index;	/* [한국어] xa_alloc 이 찾은 인덱스를 담는다. 바깥 인자를 가리지만 그쪽은 이미 MSI_ANY_INDEX 임이 확정됐다 */

		/* Let the xarray allocate a free index within the limit */
		ret = xa_alloc(xa, &index, desc, limit, GFP_KERNEL);	/* [한국어] (위 영어 주석) 빈 자리를 찾아 넣는다. 찾기와 넣기가 원자적이라 경쟁이 없다 */
		if (ret)	/* [한국어] 빈 자리가 없거나 메모리 부족 */
			goto fail;

		desc->msi_index = index;	/* [한국어] 서술자에 자기 인덱스를 기록한다. 나중에 해제할 때 이 값으로 찾는다 */
		return 0;	/* [한국어] 빈 자리를 찾아 넣는 데 성공했다. 배정된 인덱스는 desc->msi_index 에 기록돼 있다 */
	} else {	/* [한국어] 특정 인덱스를 요구하는 경우 — PCI MSI-X 테이블 항목 등 */
		if (index >= hwsize) {	/* [한국어] 하드웨어 테이블 밖인가 */
			ret = -ERANGE;	/* [한국어] 그 항목을 만지면 테이블 밖 메모리를 건드린다 */
			goto fail;	/* [한국어] 아래 fail 로 뛰어 서술자를 해제한다. 여기서 바로 반환하면 서술자가 샌다 */
		}

		desc->msi_index = index;	/* [한국어] 요구된 인덱스를 기록 */
		ret = xa_insert(xa, index, desc, GFP_KERNEL);	/* [한국어] 그 자리가 비어 있을 때만 넣는다. 이미 있으면 -EBUSY */
		if (ret)	/* [한국어] 이미 쓰이는 인덱스이거나 메모리 부족 */
			goto fail;
		return 0;	/* [한국어] 요구된 인덱스에 넣는 데 성공했다 */
	}
fail:	/* [한국어] 어느 모드든 실패하면 여기로 */
	msi_free_desc(desc);	/* [한국어] 서술자를 여기서 해제한다. 호출자가 되돌릴 필요 없다는 규약이라, 호출자마다 해제 코드를 두는 실수를 막는다 */
	return ret;	/* [한국어] 실패 원인을 그대로 올린다 */
}

/**
 * msi_domain_insert_msi_desc - Allocate and initialize a MSI descriptor and
 *				insert it at @init_desc->msi_index
 *
 * @dev:	Pointer to the device for which the descriptor is allocated
 * @domid:	The id of the interrupt domain to which the desriptor is added
 * @init_desc:	Pointer to an MSI descriptor to initialize the new descriptor
 *
 * Return: 0 on success or an appropriate failure code.
 */
/*
 * [한국어]
 * msi_domain_insert_msi_desc - 견본을 복사해 새 서술자를 만들어 넣는다
 *
 * @dev:       대상 장치
 * @domid:     대상 도메인 ID
 * @init_desc: 견본 서술자 (스택에 있어도 된다)
 * @return:    0 성공, 음수 오류
 *
 * PCI 계층이 쓰는 진입점이다. PCI 코드는 설정 공간을 읽어 서술자
 * 내용을 스택에 조립한 뒤 이 함수로 넘긴다. 여기서 힙에 사본을 만들어
 * 저장소에 넣는다.
 *
 * 왜 견본 방식인가: PCI MSI 서술자에는 설정 공간 오프셋, 마스크 가능
 * 여부, MSI-X 테이블 주소 등 PCI 고유 정보가 많다. 그것들을 인자로
 * 늘어놓는 대신 구조체 하나로 받는다.
 *
 * 복사하는 것이 세 가지뿐인 점에 주목: nvec_used, affinity, pci 필드다.
 * 나머지는 msi_alloc_desc 가 0 으로 채운 상태 그대로다 — irq 번호와
 * 메시지는 아직 할당되지 않았기 때문이다.
 *
 * lockdep_assert_held: 이 함수는 락을 잡지 않고 호출자가 이미 쥐고
 * 있어야 한다. 서술자 여러 개를 한 임계 구역 안에서 넣어야 하는
 * 호출자가 많아 그렇게 설계됐다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev->msi.data->mutex 보유.
 *
 * 호출 체인:
 *   drivers/pci/msi/msi.c 의 설정 경로 → [이 함수] →
 *   msi_alloc_desc() → msi_insert_desc()
 */
int msi_domain_insert_msi_desc(struct device *dev, unsigned int domid,
			       struct msi_desc *init_desc)
{
	struct msi_desc *desc;	/* [한국어] 새로 만든 서술자 */

	lockdep_assert_held(&dev->msi.data->mutex);	/* [한국어] 호출자가 락을 쥐고 있어야 한다. 여러 서술자를 한 임계 구역에서 넣는 호출자가 많다 */

	desc = msi_alloc_desc(dev, init_desc->nvec_used, init_desc->affinity);	/* [한국어] 견본에서 벡터 수와 친화도를 가져와 새로 만든다 */
	if (!desc)	/* [한국어] 할당 실패 */
		return -ENOMEM;

	/* Copy type specific data to the new descriptor. */
	desc->pci = init_desc->pci;	/* [한국어] (위 영어 주석) PCI 고유 정보 — 설정 공간 오프셋, 마스크 가능 여부, MSI-X 테이블 위치 등. 나머지 필드는 0 인 채로 두는데, irq 번호와 메시지는 아직 할당되지 않았기 때문이다 */

	return msi_insert_desc(dev, desc, domid, init_desc->msi_index);	/* [한국어] 견본이 지정한 인덱스에 넣는다. 실패하면 그 안에서 해제한다 */
}

/*
 * [한국어]
 * msi_desc_match - 서술자가 필터 조건에 맞는지 판정한다
 *
 * @desc:   검사할 서술자
 * @filter: MSI_DESC_ALL / NOTASSOCIATED / ASSOCIATED
 * @return: true 조건에 맞음, false 아님
 *
 * "연결됨(associated)" 이란 이 서술자에 리눅스 인터럽트 번호가
 * 배정됐다는 뜻이다. 서술자가 만들어진 직후에는 desc->irq 가 0 이고,
 * __msi_domain_alloc_irqs() 가 번호를 배정하면 0 이 아니게 된다.
 *
 * 이 구분이 필요한 이유: 할당 경로는 아직 번호가 없는 서술자만
 * 처리해야 하고(이미 있는 것에 또 배정하면 앞의 것이 샌다), 해제
 * 경로는 번호가 있는 것만 처리해야 한다.
 *
 * 0 을 "없음" 으로 쓰는 것이 가능한 이유: 리눅스 인터럽트 0 은
 * 예약되어 실제 장치에 배정되지 않는다.
 *
 * WARN_ON_ONCE 로 끝나는 것: switch 가 모든 enum 값을 덮으므로
 * 여기 도달하면 필터에 잘못된 값이 들어온 것이다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   msi_find_desc() / msi_domain_free_descs() / __msi_domain_alloc_irqs()
 *   → [이 함수]
 */
static bool msi_desc_match(struct msi_desc *desc, enum msi_desc_filter filter)
{
	switch (filter) {	/* [한국어] 세 가지 필터 조건. 아래 모든 갈래가 값을 반환하므로 switch 를 벗어나는 것은 잘못된 값이 들어온 경우뿐이다 */
	case MSI_DESC_ALL:	/* [한국어] 조건 없이 전부 */
		return true;	/* [한국어] 조건 없이 전부 통과시킨다 */
	case MSI_DESC_NOTASSOCIATED:	/* [한국어] 아직 인터럽트 번호가 없는 것만 — 할당 경로가 쓴다 */
		return !desc->irq;	/* [한국어] 0 이 "없음" 이다. 리눅스 인터럽트 0 은 예약되어 장치에 배정되지 않는다 */
	case MSI_DESC_ASSOCIATED:	/* [한국어] 인터럽트 번호가 배정된 것만 — 해제 경로가 쓴다 */
		return !!desc->irq;	/* [한국어] 이중 부정으로 불린화 */
	}
	WARN_ON_ONCE(1);	/* [한국어] switch 가 모든 enum 값을 덮으므로 여기 오면 잘못된 필터 값이다 */
	return false;	/* [한국어] 안전한 쪽 — 아무것도 처리하지 않는다 */
}

/*
 * [한국어]
 * msi_ctrl_valid - 연산 대상 범위가 유효한지 검사한다
 *
 * @dev:  대상 장치
 * @ctrl: 검사할 제어 구조체
 * @return: true 유효, false 잘못된 범위 (경고를 이미 찍었다)
 *
 * 할당·해제 경로가 공통으로 부르는 방어 검사다. 세 가지를 본다.
 *
 * (1) 도메인 ID 가 배열 범위 안인가.
 * (2) 그 도메인이 실제로 존재하는가 — 다만 dev->msi.domain 이 설정된
 *     경우에만 본다. 그 조건이 붙은 이유는 아키텍처 고유의 옛 PCI MSI
 *     지원 때문이다. 그쪽은 도메인 없이 동작하면서도 서술자 저장소는
 *     쓴다.
 * (3) 인덱스 범위가 뒤집히지 않았고 하드웨어 크기 안인가.
 *
 * 전부 WARN_ON_ONCE 인 것에 주목: 이 조건들이 깨지는 것은 호출자의
 * 버그다. 사용자 입력이 아니라 커널 내부에서 계산된 값이므로,
 * 조용히 거절하지 않고 개발자에게 알린다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_free_descs() / msi_domain_add_simple_msi_descs() /
 *   __msi_domain_alloc_locked() / msi_domain_free_locked() → [이 함수]
 */
static bool msi_ctrl_valid(struct device *dev, struct msi_ctrl *ctrl)
{
	unsigned int hwsize;	/* [한국어] 이 도메인의 하드웨어 인덱스 수 */

	if (WARN_ON_ONCE(ctrl->domid >= MSI_MAX_DEVICE_IRQDOMAINS ||	/* [한국어] 도메인 ID 가 배열 범위 안인가 */
			 (dev->msi.domain &&	/* [한국어] 도메인 기반 장치인가 — 아키텍처 고유의 옛 PCI MSI 는 도메인 없이 저장소만 쓴다 */
			  !dev->msi.data->__domains[ctrl->domid].domain)))	/* [한국어] 그 ID 의 도메인이 실제로 만들어져 있는가 */
		return false;	/* [한국어] 호출자의 버그다. 경고는 위에서 이미 찍혔다 */

	hwsize = msi_domain_get_hwsize(dev, ctrl->domid);	/* [한국어] 인덱스 상한 */
	if (WARN_ON_ONCE(ctrl->first > ctrl->last ||	/* [한국어] 범위가 뒤집혔는가 */
			 ctrl->first >= hwsize ||	/* [한국어] 시작이 상한을 넘는가 */
			 ctrl->last >= hwsize))	/* [한국어] 끝이 상한을 넘는가 */
		return false;	/* [한국어] 범위 밖 접근을 막는 마지막 관문이다 */
	return true;	/* [한국어] 이 범위로 xarray 를 순회해도 안전하다 */
}

/*
 * [한국어]
 * msi_domain_free_descs - 지정 범위의 서술자들을 저장소에서 빼고 해제한다
 *
 * @dev:  대상 장치
 * @ctrl: 대상 범위
 * @return: 없음
 *
 * 인터럽트 번호까지 이미 반납된 서술자들을 정리한다. 순서가 중요하다 —
 * 인터럽트를 먼저 풀고 나서 서술자를 푸는 것이 규칙이고, 그 앞 단계는
 * msi_domain_free_locked() 가 한다.
 *
 * 아직 인터럽트가 연결된 서술자를 만나면 일부러 새게 둔다. 원본
 * 주석의 "Leak the descriptor" 가 그 뜻이다. 왜 그러는가: 해제하면
 * 그 인터럽트의 irq_data->msi_desc 가 해제된 메모리를 가리키게 되고,
 * 인터럽트가 올 때마다 그 포인터를 따라간다. 메모리를 조금 새는 것이
 * 커널 크래시보다 낫다는 판단이다. 물론 그 상황 자체가 버그이므로
 * WARN_ON_ONCE 로 알린다.
 *
 * xa_erase 를 먼저 하고 검사하는 순서에 주목: 저장소에서는 어차피
 * 빼야 한다. 새게 두는 것은 메모리이지 저장소 항목이 아니다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_free_msi_descs_range() / msi_domain_free_locked() /
 *   msi_domain_add_simple_msi_descs() 의 실패 경로 → [이 함수]
 */
static void msi_domain_free_descs(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_desc *desc;	/* [한국어] 순회 중인 서술자 */
	struct xarray *xa;	/* [한국어] 대상 저장소 */
	unsigned long idx;	/* [한국어] 순회 인덱스 */

	lockdep_assert_held(&dev->msi.data->mutex);	/* [한국어] 호출자가 락을 쥐고 있어야 한다 */

	if (!msi_ctrl_valid(dev, ctrl))	/* [한국어] 범위가 유효한가 */
		return;	/* [한국어] 경고는 이미 찍혔다. 잘못된 범위로 순회하면 엉뚱한 서술자를 지운다 */

	xa = &dev->msi.data->__domains[ctrl->domid].store;	/* [한국어] 이 도메인의 저장소 */
	xa_for_each_range(xa, idx, desc, ctrl->first, ctrl->last) {	/* [한국어] 범위 안의 항목만 훑는다. 듬성듬성한 인덱스를 건너뛰는 것은 xarray 가 해 준다 */
		xa_erase(xa, idx);	/* [한국어] 먼저 저장소에서 뺀다. 아래에서 메모리를 새게 두더라도 저장소 항목은 남기지 않는다 */

		/* Leak the descriptor when it is still referenced */
		if (WARN_ON_ONCE(msi_desc_match(desc, MSI_DESC_ASSOCIATED)))	/* [한국어] (위 영어 주석) 아직 인터럽트가 연결돼 있는가 — 해제 순서가 어긋난 버그다 */
			continue;	/* [한국어] 일부러 새게 둔다. 해제하면 그 인터럽트의 msi_desc 포인터가 해제된 메모리를 가리켜 인터럽트마다 따라가게 된다. 메모리 누수가 크래시보다 낫다 */
		msi_free_desc(desc);	/* [한국어] 정상 경로 — 연결이 없으므로 안전하게 해제한다 */
	}
}

/**
 * msi_domain_free_msi_descs_range - Free a range of MSI descriptors of a device in an irqdomain
 * @dev:	Device for which to free the descriptors
 * @domid:	Id of the domain to operate on
 * @first:	Index to start freeing from (inclusive)
 * @last:	Last index to be freed (inclusive)
 */
/*
 * [한국어]
 * msi_domain_free_msi_descs_range - 서술자 범위 해제의 공개 진입점
 *
 * @dev:   대상 장치
 * @domid: 대상 도메인 ID
 * @first: 시작 인덱스 (포함)
 * @last:  끝 인덱스 (포함)
 * @return: 없음
 *
 * 위 msi_domain_free_descs() 를 인자 네 개로 부를 수 있게 감싼
 * 껍데기다. 이 파일 밖에서는 struct msi_ctrl 이 보이지 않으므로
 * 이런 껍데기가 필요하다.
 *
 * 누가 쓰는가: 서술자를 직접 관리하는 MSI 구현이다. PCI 계층이
 * MSI-X 설정에 실패했을 때 만들어 둔 서술자들을 되돌리는 경로 등이다.
 *
 * 락을 잡지 않는 것에 주목: 호출자가 이미 쥐고 있어야 한다. 이름에
 * _locked 가 없는데도 그런 것은 이 API 의 오래된 관례다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev->msi.data->mutex 보유.
 *
 * 호출 체인:
 *   drivers/pci/msi/ 의 되돌리기 경로 → [이 함수] → msi_domain_free_descs()
 */
void msi_domain_free_msi_descs_range(struct device *dev, unsigned int domid,
				     unsigned int first, unsigned int last)
{
	struct msi_ctrl ctrl = {	/* [한국어] 스택에 제어 구조체를 만든다. nirqs 는 0 인데 해제 경로에서는 쓰이지 않는다 */
		.domid	= domid,	/* [한국어] 대상 도메인 */
		.first	= first,	/* [한국어] 범위 시작 */
		.last	= last,	/* [한국어] 범위 끝 */
	};

	msi_domain_free_descs(dev, &ctrl);	/* [한국어] 실제 해제 논리에 위임한다 */
}

/**
 * msi_domain_add_simple_msi_descs - Allocate and initialize MSI descriptors
 * @dev:	Pointer to the device for which the descriptors are allocated
 * @ctrl:	Allocation control struct
 *
 * Return: 0 on success or an appropriate failure code.
 */
/*
 * [한국어]
 * msi_domain_add_simple_msi_descs - 범위 전체에 단순 서술자를 만들어 넣는다
 *
 * @dev:  대상 장치
 * @ctrl: 대상 범위
 * @return: 0 성공, -EINVAL 범위가 잘못됨, -ENOMEM 할당 실패
 *
 * "단순(simple)" 이란 서술자에 담을 특별한 정보가 없다는 뜻이다. PCI
 * MSI 는 설정 공간 오프셋이나 테이블 주소 같은 것을 채워야 하지만,
 * 플랫폼 MSI 나 IMS 는 인덱스만 있으면 된다. 그런 도메인이
 * MSI_FLAG_ALLOC_SIMPLE_MSI_DESCS 를 세우면 코어가 이 함수로 서술자를
 * 대신 만들어 준다.
 *
 * 각 서술자가 벡터 하나(nvec=1)에 친화도 없이 만들어진다. 멀티 MSI 는
 * PCI 전용 개념이라 여기 해당하지 않고, 친화도는 나중에 할당 시점에
 * 정해진다.
 *
 * 에러 처리가 두 레이블로 나뉜 이유: 할당 실패는 반환값이 없어
 * -ENOMEM 을 직접 채워야 하고, 삽입 실패는 이미 ret 에 값이 있다.
 * 두 경우 모두 만들어 둔 것을 전부 되돌린다 — ctrl 범위 전체를
 * 지우는데, 아직 만들지 않은 인덱스는 저장소에 없으므로 그냥 넘어간다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_alloc_simple_msi_descs() → [이 함수] →
 *   msi_alloc_desc() → msi_insert_desc()
 */
static int msi_domain_add_simple_msi_descs(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_desc *desc;	/* [한국어] 만든 서술자 */
	unsigned int idx;	/* [한국어] 순회 인덱스 */
	int ret;	/* [한국어] 삽입 결과 */

	lockdep_assert_held(&dev->msi.data->mutex);	/* [한국어] 호출자가 락을 쥐고 있어야 한다 */

	if (!msi_ctrl_valid(dev, ctrl))	/* [한국어] 범위 검사 */
		return -EINVAL;

	for (idx = ctrl->first; idx <= ctrl->last; idx++) {	/* [한국어] 범위의 모든 인덱스에 하나씩. 여기서는 듬성듬성이 아니라 전부 채운다 */
		desc = msi_alloc_desc(dev, 1, NULL);	/* [한국어] 벡터 하나, 친화도 없음. 멀티 MSI 는 PCI 전용이라 해당 없고 친화도는 할당 시점에 정해진다 */
		if (!desc)	/* [한국어] 메모리 부족 */
			goto fail_mem;	/* [한국어] 반환값이 없어 -ENOMEM 을 채워야 한다 */
		ret = msi_insert_desc(dev, desc, ctrl->domid, idx);	/* [한국어] 지정 인덱스에 넣는다. 실패하면 그 안에서 서술자를 해제한다 */
		if (ret)	/* [한국어] 이미 쓰이는 인덱스이거나 메모리 부족 */
			goto fail;	/* [한국어] ret 에 이미 값이 있다 */
	}
	return 0;	/* [한국어] 범위 전체에 서술자가 준비됐다 */

fail_mem:	/* [한국어] 할당 실패 경로 */
	ret = -ENOMEM;	/* [한국어] msi_alloc_desc 가 오류 코드를 주지 않아 여기서 채운다 */
fail:	/* [한국어] 두 경로가 합류한다 */
	msi_domain_free_descs(dev, ctrl);	/* [한국어] 만들어 둔 것을 전부 되돌린다. 아직 안 만든 인덱스는 저장소에 없어 그냥 넘어간다 */
	return ret;	/* [한국어] 실패 원인 */
}

/*
 * [한국어]
 * __get_cached_msi_msg - 서술자에 저장된 MSI 메시지 사본을 복사한다
 *
 * @entry: 대상 MSI 서술자
 * @msg:   결과를 담을 곳 (출력)
 * @return: 없음
 *
 * MSI 메시지(주소+데이터)는 장치의 레지스터에 써 넣는 값이다. 그런데
 * 그 레지스터를 되읽는 것은 느리고, MSI-X 테이블은 장치가 절전 중이면
 * 읽을 수도 없다. 그래서 코어가 마지막으로 써 넣은 값을 서술자에
 * 사본으로 들고 있다가 이 함수로 돌려준다.
 *
 * 누가 쓰는가: 서스펜드·리줌 경로다. 리줌 때 장치의 MSI 레지스터가
 * 초기화됐을 수 있어 사본을 다시 써 넣어야 한다.
 *
 * "cached" 라는 이름이 정확한 이유: 이 값이 하드웨어의 현재 상태와
 * 다를 수 있다. 하드웨어가 초기화됐거나 펌웨어가 건드렸다면 사본이
 * 옛 값이다. 그래도 코어가 의도한 값이므로 복원의 기준으로 쓴다.
 *
 * 실행 컨텍스트: 제약 없음. 단순 구조체 복사다.
 *
 * 호출 체인:
 *   get_cached_msi_msg() / PCI 리줌 경로 → [이 함수]
 */
void __get_cached_msi_msg(struct msi_desc *entry, struct msi_msg *msg)
{
	*msg = entry->msg;	/* [한국어] 구조체 통째 복사. 코어가 마지막으로 써 넣은 값이지 하드웨어의 현재 값은 아니다 */
}

/*
 * [한국어]
 * get_cached_msi_msg - 인터럽트 번호로 MSI 메시지 사본을 얻는다
 *
 * @irq: 리눅스 인터럽트 번호
 * @msg: 결과를 담을 곳 (출력)
 * @return: 없음
 *
 * 위 함수를 번호로 부를 수 있게 감싼 껍데기다. 드라이버가 서술자
 * 포인터를 들고 있지 않은 경우를 위한 것이다.
 *
 * NULL 검사가 없는 것에 주목: irq_get_msi_desc() 가 NULL 을 돌려주면
 * 곧바로 터진다. MSI 가 아닌 인터럽트 번호를 넘기는 것은 호출자의
 * 버그라는 전제다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   드라이버 / PCI 리줌 경로 → [이 함수] → __get_cached_msi_msg()
 */
void get_cached_msi_msg(unsigned int irq, struct msi_msg *msg)
{
	struct msi_desc *entry = irq_get_msi_desc(irq);	/* [한국어] 번호로 서술자를 찾는다. MSI 가 아니면 NULL 이 나오고 아래에서 터진다 */

	__get_cached_msi_msg(entry, msg);	/* [한국어] 사본 복사 */
}
EXPORT_SYMBOL_GPL(get_cached_msi_msg);	/* [한국어] 드라이버가 모듈인 경우가 많다 */

/*
 * [한국어]
 * msi_device_data_release - 장치가 사라질 때 MSI 데이터를 정리한다
 *
 * @dev: 사라지는 장치
 * @res: devres 가 관리하던 msi_device_data
 * @return: 없음
 *
 * devres 콜백이다. 장치가 제거될 때 커널이 자동으로 부르므로,
 * 드라이버가 MSI 정리를 잊어도 자원이 새지 않는다.
 *
 * 세 가지를 한다. 장치별 MSI 도메인들을 없애고, 저장소가 비었는지
 * 확인하고, xarray 내부 구조를 해제한다.
 *
 * 저장소가 비어 있어야 하는 이유: 여기 도달할 때쯤이면 모든 인터럽트가
 * 이미 해제돼 있어야 한다. 남아 있다면 드라이버가 free_irq 를 빠뜨린
 * 것이고, 그 인터럽트는 이미 동작하지 않는 장치를 가리킨다.
 * xa_destroy 는 남은 항목의 포인터를 그냥 버리므로 서술자 메모리가
 * 샌다 — 경고가 그것을 알린다.
 *
 * dev->msi.data 를 NULL 로 만드는 것이 마지막인 이유: 위 정리 과정이
 * 그 포인터를 통해 저장소에 접근하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 장치 제거 경로.
 *
 * 호출 체인:
 *   device_del() → devres_release_all() → [이 함수] →
 *   msi_remove_device_irq_domain()
 */
static void msi_device_data_release(struct device *dev, void *res)
{
	struct msi_device_data *md = res;	/* [한국어] devres 가 넘겨준 우리 데이터 */
	int i;	/* [한국어] 도메인 ID 순회용 */

	for (i = 0; i < MSI_MAX_DEVICE_IRQDOMAINS; i++) {	/* [한국어] 이 장치가 가질 수 있는 모든 도메인에 대해 */
		msi_remove_device_irq_domain(dev, i);	/* [한국어] 장치별 도메인을 없앤다. 없으면 조용히 넘어간다 */
		WARN_ON_ONCE(!xa_empty(&md->__domains[i].store));	/* [한국어] 서술자가 남아 있으면 드라이버가 free_irq 를 빠뜨린 것이다. 아래 xa_destroy 가 포인터를 버려 메모리가 새므로 알린다 */
		xa_destroy(&md->__domains[i].store);	/* [한국어] xarray 의 내부 노드들을 해제한다. 항목 자체는 해제하지 않는다 */
	}
	dev->msi.data = NULL;	/* [한국어] 마지막에 끊는다. 위 정리가 이 포인터로 저장소에 접근하기 때문이다. md 메모리 자체는 devres 가 해제한다 */
}

/**
 * msi_setup_device_data - Setup MSI device data
 * @dev:	Device for which MSI device data should be set up
 *
 * Return: 0 on success, appropriate error code otherwise
 *
 * This can be called more than once for @dev. If the MSI device data is
 * already allocated the call succeeds. The allocated memory is
 * automatically released when the device is destroyed.
 */
/*
 * [한국어]
 * msi_setup_device_data - 장치에 MSI 저장소를 붙인다
 *
 * @dev: 대상 장치
 * @return: 0 성공(이미 있는 경우 포함), 음수 오류
 *
 * MSI 를 쓰려는 모든 경로가 가장 먼저 부르는 함수다. 여러 번 불러도
 * 안전하도록 만들어져 있는데, 한 장치가 PCI MSI-X 와 IMS 를 함께
 * 쓰는 경우처럼 여러 진입점이 각자 이 함수를 부를 수 있기 때문이다.
 *
 * devres 를 쓰는 것이 이 함수의 핵심이다. 장치가 제거될 때 위
 * release 콜백이 자동으로 불리므로, 드라이버가 정리를 잊어도 자원이
 * 새지 않는다. devres_alloc 은 메모리를 잡기만 하고, devres_add 가
 * 장치에 등록한다 — 그 사이에 실패하면 devres_free 로 되돌린다.
 *
 * 마지막의 조건부 대입이 미묘하다. dev->msi.domain 이 이미 설정돼
 * 있고 그것이 "MSI 부모" 도메인이 아니라면, 그것을 기본 도메인 슬롯에
 * 복사한다. 원본 주석이 설명하듯, 이렇게 해 두면 이 파일의 모든
 * 코드가 도메인 ID 로만 동작할 수 있다.
 *
 * MSI 부모 도메인은 왜 제외하는가: 그것은 장치가 직접 쓰는 도메인이
 * 아니라, 장치별 도메인을 만들 때 부모가 되는 도메인이다. 슬롯에
 * 넣으면 나중에 msi_get_device_domain() 이 그것을 돌려주고, 그 도메인에
 * 대고 할당을 시도하다 실패한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_msi_setup_msi_irqs() / msi_create_device_irq_domain() 등 →
 *   [이 함수] → devres_alloc() → xa_init_flags()
 */
int msi_setup_device_data(struct device *dev)
{
	struct msi_device_data *md;	/* [한국어] 새로 만들 MSI 데이터 */
	int ret, i;	/* [한국어] sysfs 결과와 순회 인덱스 */

	if (dev->msi.data)	/* [한국어] 이미 붙어 있는가 */
		return 0;	/* [한국어] 여러 진입점이 각자 부를 수 있어 두 번째부터는 성공만 알린다 */

	md = devres_alloc(msi_device_data_release, sizeof(*md), GFP_KERNEL);	/* [한국어] devres 로 잡는다. 장치가 사라질 때 위 release 콜백이 자동으로 불린다 */
	if (!md)	/* [한국어] 메모리 부족 */
		return -ENOMEM;

	ret = msi_sysfs_create_group(dev);	/* [한국어] /sys/.../msi_irqs/ 디렉터리를 만든다. sysfs 없는 빌드에서는 항상 성공한다 */
	if (ret) {	/* [한국어] 실패 */
		devres_free(md);	/* [한국어] 아직 devres_add 를 하지 않았으므로 이렇게 되돌린다. add 뒤였다면 release 콜백이 불릴 것이다 */
		return ret;	/* [한국어] sysfs 그룹 생성 실패. 위에서 devres_free 로 되돌렸으므로 새는 것이 없다 */
	}

	for (i = 0; i < MSI_MAX_DEVICE_IRQDOMAINS; i++)	/* [한국어] 모든 도메인 슬롯의 저장소를 */
		xa_init_flags(&md->__domains[i].store, XA_FLAGS_ALLOC);	/* [한국어] ALLOC 플래그는 xa_alloc 으로 빈 자리를 찾을 수 있게 한다. MSI_ANY_INDEX 모드가 그것을 쓴다 */

	/*
	 * If @dev::msi::domain is set and is a global MSI domain, copy the
	 * pointer into the domain array so all code can operate on domain
	 * ids. The NULL pointer check is required to keep the legacy
	 * architecture specific PCI/MSI support working.
	 */
	if (dev->msi.domain && !irq_domain_is_msi_parent(dev->msi.domain))	/* [한국어] (위 영어 주석) 전역 MSI 도메인이 이미 지정돼 있는가. 부모 도메인은 제외한다 — 그것은 장치별 도메인을 만들 때 부모가 될 뿐 직접 쓰는 것이 아니다 */
		md->__domains[MSI_DEFAULT_DOMAIN].domain = dev->msi.domain;	/* [한국어] 기본 슬롯에 복사한다. 이렇게 해 두면 이 파일의 모든 코드가 도메인 ID 로만 동작할 수 있다 */

	mutex_init(&md->mutex);	/* [한국어] 저장소를 지킬 뮤텍스 */
	dev->msi.data = md;	/* [한국어] 장치에 연결한다. 이 줄부터 다른 코드가 저장소를 볼 수 있다 */
	devres_add(dev, md);	/* [한국어] devres 목록에 등록한다. 이제 장치 제거 시 자동 정리된다 */
	return 0;	/* [한국어] 성공 */
}

/**
 * __msi_lock_descs - Lock the MSI descriptor storage of a device
 * @dev:	Device to operate on
 *
 * Internal function for guard(msi_descs_lock). Don't use in code.
 */
/*
 * [한국어]
 * __msi_lock_descs - MSI 서술자 저장소를 잠근다
 *
 * @dev: 대상 장치
 * @return: 없음
 *
 * guard(msi_descs_lock) 매크로가 내부적으로 부르는 함수다. 원본
 * 주석대로 코드에서 직접 부르면 안 된다 — guard 를 쓰면 블록을
 * 벗어날 때 자동으로 풀려 실수가 없다.
 *
 * 그런데도 함수로 내보내는 이유: guard 매크로가 인라인 함수 안에서
 * 이 이름을 부르는데, 그 인라인이 모듈 코드에 펼쳐질 수 있다.
 * 뮤텍스 자체는 static 이 아니지만 그것을 잠그는 정확한 방식을
 * 이 함수가 정의한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스라 잠들 수 있다.
 *
 * 호출 체인:
 *   guard(msi_descs_lock)(dev) → [이 함수]
 */
void __msi_lock_descs(struct device *dev)
{
	mutex_lock(&dev->msi.data->mutex);	/* [한국어] 저장소 조작과 순회 전체를 직렬화한다 */
}
EXPORT_SYMBOL_GPL(__msi_lock_descs);	/* [한국어] guard 매크로가 모듈 코드에 펼쳐질 수 있다 */

/**
 * __msi_unlock_descs - Unlock the MSI descriptor storage of a device
 * @dev:	Device to operate on
 *
 * Internal function for guard(msi_descs_lock). Don't use in code.
 */
/*
 * [한국어]
 * __msi_unlock_descs - MSI 서술자 저장소의 잠금을 푼다
 *
 * @dev: 대상 장치
 * @return: 없음
 *
 * 푸는 것보다 그 앞의 한 줄이 더 중요하다. 순회 커서를 무효값으로
 * 밀어 놓는다.
 *
 * 왜 그래야 하는가: 순회 상태가 장치 구조체에 들어 있어, 락을 놓은
 * 뒤에 그 값이 남아 있으면 다음 순회가 엉뚱한 위치에서 시작할 수
 * 있다. 특히 msi_for_each_desc 루프를 중간에 break 로 빠져나온
 * 경우가 그렇다.
 *
 * MSI_XA_MAX_INDEX 는 어떤 검색 범위에도 들지 않는 값이라, 이 상태로
 * msi_next_desc() 를 부르면 아무것도 찾지 못하고 NULL 이 나온다.
 * 즉 "순회가 유효하지 않다" 를 값으로 표현한 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   guard(msi_descs_lock) 블록 종료 → [이 함수]
 */
void __msi_unlock_descs(struct device *dev)
{
	/* Invalidate the index which was cached by the iterator */
	dev->msi.data->__iter_idx = MSI_XA_MAX_INDEX;	/* [한국어] (위 영어 주석) 순회 커서를 무효화한다. 루프를 break 로 빠져나왔다면 낡은 값이 남아 다음 순회를 엉뚱한 곳에서 시작하게 만든다 */
	mutex_unlock(&dev->msi.data->mutex);	/* [한국어] 잠금 해제 */
}
EXPORT_SYMBOL_GPL(__msi_unlock_descs);	/* [한국어] guard 매크로가 모듈 코드에 펼쳐질 수 있다 */

/*
 * [한국어]
 * msi_find_desc - 커서 위치부터 조건에 맞는 첫 서술자를 찾는다
 *
 * @md:     장치의 MSI 데이터 (커서가 여기 있다)
 * @domid:  대상 도메인 ID
 * @filter: 서술자 조건
 * @return: 찾은 서술자, 없으면 NULL
 *
 * 순회의 실제 구현이다. xa_for_each_start 가 커서를 갱신하면서 훑고,
 * 조건에 맞는 첫 항목에서 멈춘다. 그때 커서는 그 항목의 인덱스를
 * 가리키므로, 다음 호출은 msi_next_desc() 가 커서를 하나 올린 뒤
 * 다시 이 함수를 부른다.
 *
 * 찾지 못하면 커서를 무효화하는 것이 중요하다. 그러지 않으면 루프의
 * 마지막 인덱스가 남아, 그 뒤 다른 순회가 그 위치부터 시작한다.
 *
 * 커서를 md 구조체에 두는 설계의 대가가 여기서 드러난다 — 중첩
 * 순회를 할 수 없고, 순회 중에 락을 놓을 수 없다. 그 대가로
 * msi_for_each_desc 매크로가 커서 변수를 노출하지 않아도 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_first_desc() / msi_next_desc() → [이 함수] →
 *   msi_desc_match()
 */
static struct msi_desc *msi_find_desc(struct msi_device_data *md, unsigned int domid,
				      enum msi_desc_filter filter)
{
	struct xarray *xa = &md->__domains[domid].store;	/* [한국어] 대상 저장소 */
	struct msi_desc *desc;	/* [한국어] 순회 중인 서술자 */

	xa_for_each_start(xa, md->__iter_idx, desc, md->__iter_idx) {	/* [한국어] 커서 위치부터 훑는다. 커서 자체가 순회 변수라 멈춘 위치가 그대로 남는다 */
		if (msi_desc_match(desc, filter))	/* [한국어] 조건에 맞는가 */
			return desc;	/* [한국어] 커서가 이 항목을 가리킨 채로 반환한다. 다음 호출은 여기서 하나 올려 이어 간다 */
	}
	md->__iter_idx = MSI_XA_MAX_INDEX;	/* [한국어] 더 없다. 커서를 무효화하지 않으면 마지막 인덱스가 남아 다음 순회를 오염시킨다 */
	return NULL;	/* [한국어] 순회 종료 */
}

/**
 * msi_domain_first_desc - Get the first MSI descriptor of an irqdomain associated to a device
 * @dev:	Device to operate on
 * @domid:	The id of the interrupt domain which should be walked.
 * @filter:	Descriptor state filter
 *
 * Must be called with the MSI descriptor mutex held, i.e. msi_lock_descs()
 * must be invoked before the call.
 *
 * Return: Pointer to the first MSI descriptor matching the search
 *	   criteria, NULL if none found.
 */
/*
 * [한국어]
 * msi_domain_first_desc - 순회를 시작한다
 *
 * @dev:    대상 장치
 * @domid:  대상 도메인 ID
 * @filter: 서술자 조건
 * @return: 첫 번째로 조건에 맞는 서술자, 없으면 NULL
 *
 * msi_for_each_desc 매크로의 시작 부분이다. 커서를 0 으로 놓고
 * 첫 항목을 찾는다.
 *
 * 커서를 0 으로 놓는 이 한 줄이 순회의 시작을 정의한다. 이 함수를
 * 부르지 않고 msi_next_desc() 를 먼저 부르면 커서가 무효값이라
 * 아무것도 찾지 못한다 — 그것이 규약을 강제하는 방식이다.
 *
 * lockdep_assert_held 로 락 보유를 확인한다. 순회 중에 다른 CPU 가
 * 서술자를 추가하거나 지우면 커서가 가리키는 위치의 뜻이 달라진다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_for_each_desc 매크로 → [이 함수] → msi_find_desc()
 */
struct msi_desc *msi_domain_first_desc(struct device *dev, unsigned int domid,
				       enum msi_desc_filter filter)
{
	struct msi_device_data *md = dev->msi.data;	/* [한국어] 장치의 MSI 데이터 */

	if (WARN_ON_ONCE(!md || domid >= MSI_MAX_DEVICE_IRQDOMAINS))	/* [한국어] MSI 저장소가 없거나 도메인 ID 가 범위 밖인가 — 호출자의 버그다 */
		return NULL;

	lockdep_assert_held(&md->mutex);	/* [한국어] 순회 중에 저장소가 바뀌면 커서의 뜻이 달라진다 */

	md->__iter_idx = 0;	/* [한국어] 커서를 처음으로. 이 한 줄이 순회의 시작을 정의한다 */
	return msi_find_desc(md, domid, filter);	/* [한국어] 첫 항목을 찾는다 */
}
EXPORT_SYMBOL_GPL(msi_domain_first_desc);	/* [한국어] 매크로가 모듈 코드에 펼쳐진다 */

/**
 * msi_next_desc - Get the next MSI descriptor of a device
 * @dev:	Device to operate on
 * @domid:	The id of the interrupt domain which should be walked.
 * @filter:	Descriptor state filter
 *
 * The first invocation of msi_next_desc() has to be preceeded by a
 * successful invocation of __msi_first_desc(). Consecutive invocations are
 * only valid if the previous one was successful. All these operations have
 * to be done within the same MSI mutex held region.
 *
 * Return: Pointer to the next MSI descriptor matching the search
 *	   criteria, NULL if none found.
 */
/*
 * [한국어]
 * msi_next_desc - 순회의 다음 항목을 얻는다
 *
 * @dev:    대상 장치
 * @domid:  대상 도메인 ID
 * @filter: 서술자 조건
 * @return: 다음으로 조건에 맞는 서술자, 없으면 NULL
 *
 * 커서를 하나 올린 뒤 다시 찾는다. 그 "하나 올림" 이 없으면 같은
 * 항목을 무한히 반환해 루프가 끝나지 않는다.
 *
 * 상한 검사가 그 앞에 있는 이유: 커서가 이미 최대 인덱스이거나
 * 무효값이면 올릴 수 없다. 특히 무효값(MSI_XA_MAX_INDEX)일 때
 * 올리면 넘침이 일어난다.
 *
 * 원본 주석이 규약을 명확히 한다 — 반드시 first_desc 로 시작해야
 * 하고, 이전 호출이 성공했을 때만 다음을 부를 수 있고, 전부 같은
 * 뮤텍스 구역 안이어야 한다. 세 조건 모두 커서를 구조체에 둔 설계에서
 * 나온다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_for_each_desc 매크로 → [이 함수] → msi_find_desc()
 */
struct msi_desc *msi_next_desc(struct device *dev, unsigned int domid,
			       enum msi_desc_filter filter)
{
	struct msi_device_data *md = dev->msi.data;	/* [한국어] 장치의 MSI 데이터 */

	if (WARN_ON_ONCE(!md || domid >= MSI_MAX_DEVICE_IRQDOMAINS))	/* [한국어] 저장소가 없거나 ID 가 범위 밖인가 */
		return NULL;

	lockdep_assert_held(&md->mutex);	/* [한국어] first_desc 와 같은 구역 안이어야 한다 */

	if (md->__iter_idx >= (unsigned long)MSI_MAX_INDEX)	/* [한국어] 커서가 이미 끝이거나 무효값인가 */
		return NULL;	/* [한국어] 무효값일 때 아래에서 올리면 넘침이 일어난다 */

	md->__iter_idx++;	/* [한국어] 다음 위치로. 이 한 줄이 없으면 같은 항목을 무한히 반환해 루프가 끝나지 않는다 */
	return msi_find_desc(md, domid, filter);	/* [한국어] 그 위치부터 조건에 맞는 것을 찾는다 */
}
EXPORT_SYMBOL_GPL(msi_next_desc);	/* [한국어] 매크로가 모듈 코드에 펼쳐진다 */

/**
 * msi_domain_get_virq - Lookup the Linux interrupt number for a MSI index on a interrupt domain
 * @dev:	Device to operate on
 * @domid:	Domain ID of the interrupt domain associated to the device
 * @index:	MSI interrupt index to look for (0-based)
 *
 * Return: The Linux interrupt number on success (> 0), 0 if not found
 */
/*
 * [한국어]
 * msi_domain_get_virq - MSI 인덱스로 리눅스 인터럽트 번호를 찾는다
 *
 * @dev:   대상 장치
 * @domid: 대상 도메인 ID
 * @index: MSI 인덱스 (0 부터)
 * @return: 리눅스 인터럽트 번호 (양수), 없으면 0
 *
 * 드라이버가 "내 3 번 MSI-X 벡터의 인터럽트 번호는 몇인가" 를 묻는
 * 통로다. 그 번호를 request_irq 에 넘긴다.
 *
 * PCI MSI 와 그 밖의 방식이 근본적으로 다른 점이 이 함수에 드러난다.
 * PCI MSI(MSI-X 가 아닌)는 벡터 여러 개를 서술자 하나가 대표한다.
 * 그래서 저장소에는 인덱스 0 에 항목 하나만 있고, 실제 번호는
 * desc->irq 부터 연속이다. MSI-X 와 플랫폼 MSI 는 인덱스마다 서술자가
 * 따로 있다.
 *
 * 그 차이를 pcimsi 플래그 하나로 갈라 처리한다. 조회할 때는 0 번을
 * 보고(PCI MSI 는 거기 하나뿐이므로), 반환할 때는 desc->irq + index 를
 * 돌려준다.
 *
 * msi_enabled 를 보는 것에 주목: msix_enabled 가 아니다. PCI 장치는
 * MSI 와 MSI-X 중 하나만 켤 수 있고, MSI 가 켜져 있을 때만 이 특별
 * 처리가 필요하다.
 *
 * 0 을 "없음" 으로 쓰는 이유: 리눅스 인터럽트 0 은 예약되어 장치에
 * 배정되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_irq_vector() (drivers/pci/msi/api.c) / 드라이버 → [이 함수]
 */
unsigned int msi_domain_get_virq(struct device *dev, unsigned int domid, unsigned int index)
{
	struct msi_desc *desc;	/* [한국어] 찾은 서술자 */
	bool pcimsi = false;	/* [한국어] PCI MSI(MSI-X 아님) 특별 처리가 필요한가 */
	struct xarray *xa;	/* [한국어] 대상 저장소 */

	if (!dev->msi.data)	/* [한국어] MSI 저장소가 없는 장치인가 */
		return 0;	/* [한국어] MSI 를 쓴 적이 없다 */

	if (WARN_ON_ONCE(index > MSI_MAX_INDEX || domid >= MSI_MAX_DEVICE_IRQDOMAINS))	/* [한국어] 인덱스나 도메인 ID 가 범위 밖인가 — 호출자의 버그다 */
		return 0;

	/* This check is only valid for the PCI default MSI domain */
	if (dev_is_pci(dev) && domid == MSI_DEFAULT_DOMAIN)	/* [한국어] (위 영어 주석) PCI 장치의 기본 도메인인가. IMS 등 보조 도메인은 이 특별 처리와 무관하다 */
		pcimsi = to_pci_dev(dev)->msi_enabled;	/* [한국어] MSI-X 가 아니라 MSI 가 켜져 있는가. 둘은 배타적이며 MSI 만 서술자 하나가 여러 벡터를 대표한다 */

	guard(msi_descs_lock)(dev);	/* [한국어] 저장소 조회 동안 서술자가 사라지지 않게 한다 */
	xa = &dev->msi.data->__domains[domid].store;	/* [한국어] 대상 저장소 */
	desc = xa_load(xa, pcimsi ? 0 : index);	/* [한국어] PCI MSI 는 0 번에 항목이 하나뿐이다. 나머지는 인덱스마다 항목이 있다 */
	if (desc && desc->irq) {	/* [한국어] 서술자가 있고 인터럽트가 배정됐는가 */
		/*
		 * PCI-MSI has only one descriptor for multiple interrupts.
		 * PCI-MSIX and platform MSI use a descriptor per
		 * interrupt.
		 */
		if (!pcimsi)	/* [한국어] (위 영어 주석) MSI-X 나 플랫폼 MSI 인가 */
			return desc->irq;	/* [한국어] 이 서술자가 곧 그 인터럽트다 */
		if (index < desc->nvec_used)	/* [한국어] PCI MSI — 요청한 인덱스가 이 서술자가 대표하는 범위 안인가 */
			return desc->irq + index;	/* [한국어] 번호가 연속이라 더하면 된다. PCI MSI 의 벡터는 2 의 거듭제곱 개수로 연속 할당된다 */
	}
	return 0;	/* [한국어] 없거나 아직 배정되지 않았다 */
}
EXPORT_SYMBOL_GPL(msi_domain_get_virq);	/* [한국어] PCI 계층과 드라이버가 부른다 */

#ifdef CONFIG_SYSFS	/* [한국어] /sys/bus/pci/devices/<주소>/msi_irqs/ 항목을 만드는 코드. 없는 빌드에서는 아래 #else 의 빈 함수들이 쓰인다 */
static struct attribute *msi_dev_attrs[] = {
	/* [한국어] msi_irqs 그룹의 정적 속성 목록.
	 * 비어 있는 것이 의도적이다. 이 그룹의 실제 파일들은 인터럽트가
	 * 할당될 때마다 sysfs_add_file_to_group() 으로 동적으로 붙는다.
	 * 정적 목록이 필요한 이유는 그룹을 만들려면 attrs 포인터가
	 * 있어야 하기 때문이다. */
	NULL
	/* [한국어] 빈 목록의 끝 표식.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: sysfs 그룹 생성 코드의 순회 루프.
	 * 값 범위: 항상 NULL. 이것 하나뿐이라 그룹은 파일 없이 만들어진다.
	 * 동기화: 불필요. */
};

static const struct attribute_group msi_irqs_group = {
	/* [한국어] 장치 아래 만들어질 msi_irqs 디렉터리의 정의.
	 * 이름만 정하고 내용은 비워 둔 뒤, 인터럽트마다 파일을 동적으로
	 * 추가하는 방식이다. */
	.name	= "msi_irqs",
	/* [한국어] 만들어질 디렉터리 이름.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: devm_device_add_group() 과, 파일을 붙이고 떼는
	 *   sysfs_add_file_to_group()/sysfs_remove_file_from_group().
	 * 값 범위: 항상 "msi_irqs". 이 이름이 사용자 공간 ABI 라 바꿀 수 없다.
	 * 동기화: const 라 변경되지 않는다. */
	.attrs	= msi_dev_attrs,
	/* [한국어] 그룹 생성 시 함께 만들 정적 파일 목록.
	 * 설정자: 정적 초기화.
	 * 읽는 자: sysfs 그룹 생성 코드.
	 * 값 범위: 위의 빈 배열. 실제 파일은 전부 동적으로 붙는다.
	 * 동기화: 위와 같다. */
};

/*
 * [한국어]
 * msi_sysfs_create_group - 장치 아래 msi_irqs 디렉터리를 만든다
 *
 * @dev: 대상 장치
 * @return: 0 성공, 음수 오류
 *
 * devm_ 접두사가 붙은 판을 쓰므로 장치가 사라질 때 디렉터리도 자동
 * 제거된다. 이 파일이 MSI 데이터를 devres 로 관리하는 것과 같은 방식이다.
 *
 * 디렉터리만 만들고 파일은 만들지 않는다. 파일은 인터럽트가 할당될
 * 때마다 msi_sysfs_populate_desc() 가 하나씩 붙인다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_setup_device_data() → [이 함수] → devm_device_add_group()
 */
static inline int msi_sysfs_create_group(struct device *dev)
{
	return devm_device_add_group(dev, &msi_irqs_group);	/* [한국어] devm 판이라 장치 제거 시 자동으로 사라진다 */
}

/*
 * [한국어]
 * msi_mode_show - msi_irqs/<번호> 파일의 내용을 출력한다
 *
 * @dev:  대상 장치
 * @attr: 어떤 속성인지 (쓰지 않는다)
 * @buf:  출력 버퍼
 * @return: 쓴 바이트 수
 *
 * "msi" 또는 "msix" 한 줄을 출력한다. 사용자 공간 도구가 이 장치가
 * 어느 방식을 쓰는지 알아내는 통로다.
 *
 * 원본 주석이 짚는 것이 핵심이다 — MSI 냐 MSI-X 냐는 인터럽트마다가
 * 아니라 장치마다 정해진다. PCI 규격상 둘을 동시에 켤 수 없다.
 * 그래서 모든 파일이 같은 함수를 쓰고 같은 답을 내놓는다.
 *
 * PCI 가 아니면 "msi" 로 답하는 것에 주목: 플랫폼 MSI 나 IMS 는
 * MSI-X 라는 개념 자체가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   cat /sys/.../msi_irqs/42 → sysfs → [이 함수]
 */
static ssize_t msi_mode_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	/* MSI vs. MSIX is per device not per interrupt */
	bool is_msix = dev_is_pci(dev) ? to_pci_dev(dev)->msix_enabled : false;	/* [한국어] (위 영어 주석) 장치 단위 성질이라 모든 파일이 같은 답을 낸다. PCI 가 아니면 MSI-X 라는 개념이 없어 false */

	return sysfs_emit(buf, "%s\n", is_msix ? "msix" : "msi");	/* [한국어] 한 줄 출력. 사용자 공간 도구가 이 값으로 방식을 판별한다 */
}

/*
 * [한국어]
 * msi_sysfs_remove_desc - 서술자에 딸린 sysfs 파일들을 제거한다
 *
 * @dev:  대상 장치
 * @desc: 대상 서술자
 * @return: 없음
 *
 * 서술자 하나가 nvec_used 개의 파일을 가질 수 있다 — PCI 멀티 MSI 는
 * 벡터마다 파일이 하나씩이기 때문이다.
 *
 * attrs 포인터를 먼저 NULL 로 만드는 것이 중요하다. 이 함수는 실패
 * 경로에서도 불리는데(populate 가 중간에 실패한 경우), 그때 두 번
 * 불려도 안전해야 한다. NULL 검사가 그 방어다.
 *
 * attrs[i].show 를 검사하는 이유: populate 가 중간에 실패하면 일부
 * 항목은 아직 파일이 만들어지지 않았다. show 포인터가 채워진 것만
 * 실제로 sysfs 에 붙어 있으므로, 그것만 뗀다. 반대로 이름 문자열은
 * 파일 등록보다 먼저 할당되므로 조건 없이 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __msi_domain_free_irqs() / msi_sysfs_populate_desc() 의 실패 경로 /
 *   msi_device_destroy_sysfs() → [이 함수]
 */
static void msi_sysfs_remove_desc(struct device *dev, struct msi_desc *desc)
{
	struct device_attribute *attrs = desc->sysfs_attrs;	/* [한국어] 이 서술자의 속성 배열 */
	int i;	/* [한국어] 순회용 */

	if (!attrs)	/* [한국어] 파일이 만들어진 적이 없는가 */
		return;	/* [한국어] 두 번 불려도 안전하게 만드는 방어다 */

	desc->sysfs_attrs = NULL;	/* [한국어] 먼저 끊는다. 아래 도중에 다시 불려도 이 함수가 재진입하지 않는다 */
	for (i = 0; i < desc->nvec_used; i++) {	/* [한국어] PCI 멀티 MSI 는 벡터마다 파일이 하나씩이다 */
		if (attrs[i].show)	/* [한국어] 실제로 sysfs 에 붙은 항목인가 — populate 가 중간에 실패했으면 뒤쪽은 안 붙어 있다 */
			sysfs_remove_file_from_group(&dev->kobj, &attrs[i].attr, msi_irqs_group.name);	/* [한국어] 디렉터리에서 파일을 뗀다 */
		kfree(attrs[i].attr.name);	/* [한국어] 이름 문자열은 파일 등록보다 먼저 할당되므로 조건 없이 해제한다 */
	}
	kfree(attrs);	/* [한국어] 배열 자체 */
}

/*
 * [한국어]
 * msi_sysfs_populate_desc - 서술자의 인터럽트마다 sysfs 파일을 만든다
 *
 * @dev:  대상 장치
 * @desc: 대상 서술자 (이미 인터럽트 번호가 배정돼 있어야 한다)
 * @return: 0 성공, -ENOMEM 할당 실패, 그 외 sysfs 오류
 *
 * msi_irqs 디렉터리 아래 인터럽트 번호를 이름으로 하는 파일들을
 * 만든다. 그 파일을 읽으면 "msi" 또는 "msix" 가 나온다.
 *
 * 파일 이름이 인터럽트 번호인 것이 이 인터페이스의 요점이다. 사용자
 * 공간이 디렉터리를 나열하면 이 장치가 쓰는 인터럽트 번호 목록이
 * 나오고, 그것을 /proc/interrupts 와 대조할 수 있다.
 *
 * sysfs_attr_init 이 필요한 이유: lockdep 이 sysfs 속성마다 락 클래스를
 * 추적하는데, 동적으로 할당된 속성은 그 초기화를 명시적으로 해 주어야
 * 한다. 정적 속성은 매크로가 대신 해 준다.
 *
 * 실패 시 attrs[i].show 를 NULL 로 되돌리는 것이 미묘하다. 파일
 * 등록이 실패했으므로 그 항목은 sysfs 에 없고, 위 remove 함수가
 * 그것을 알아야 떼려 시도하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() / msi_device_populate_sysfs() → [이 함수]
 */
static int msi_sysfs_populate_desc(struct device *dev, struct msi_desc *desc)
{
	struct device_attribute *attrs;	/* [한국어] 만들 속성 배열 */
	int ret, i;	/* [한국어] 결과와 순회 인덱스 */

	attrs = kzalloc_objs(*attrs, desc->nvec_used);	/* [한국어] 벡터 수만큼. 0 초기화라 show 포인터가 NULL 로 시작하는 것이 아래 실패 처리의 근거다 */
	if (!attrs)	/* [한국어] 메모리 부족 */
		return -ENOMEM;

	desc->sysfs_attrs = attrs;	/* [한국어] 서술자에 연결한다. 실패 시 remove 함수가 이 포인터로 정리한다 */
	for (i = 0; i < desc->nvec_used; i++) {	/* [한국어] 벡터마다 파일 하나 */
		sysfs_attr_init(&attrs[i].attr);	/* [한국어] lockdep 이 동적 속성의 락 클래스를 추적할 수 있게 한다. 정적 속성은 매크로가 대신 해 준다 */
		attrs[i].attr.name = kasprintf(GFP_KERNEL, "%d", desc->irq + i);	/* [한국어] 파일 이름이 곧 인터럽트 번호다. 사용자 공간이 이것으로 /proc/interrupts 와 대조한다 */
		if (!attrs[i].attr.name) {	/* [한국어] 문자열 할당 실패 */
			ret = -ENOMEM;	/* [한국어] msi_alloc_desc 는 반환값이 없어 여기서 오류 코드를 채운다 */
			goto fail;	/* [한국어] 아래 공통 정리 경로로. 이미 만든 서술자들을 되돌린다 */
		}

		attrs[i].attr.mode = 0444;	/* [한국어] 읽기 전용. 방식을 바꾸는 것은 이 인터페이스의 일이 아니다 */
		attrs[i].show = msi_mode_show;	/* [한국어] 모든 파일이 같은 함수를 쓴다. 답이 장치 단위라 구분할 이유가 없다 */

		ret = sysfs_add_file_to_group(&dev->kobj, &attrs[i].attr, msi_irqs_group.name);	/* [한국어] 미리 만들어 둔 디렉터리에 붙인다 */
		if (ret) {	/* [한국어] 등록 실패 */
			attrs[i].show = NULL;	/* [한국어] 붙지 않았음을 표시한다. remove 함수가 이것을 보고 떼려 시도하지 않는다 */
			goto fail;	/* [한국어] ret 에 msi_insert_desc 의 오류가 이미 들어 있다 */
		}
	}
	return 0;	/* [한국어] 모든 파일이 만들어졌다 */

fail:	/* [한국어] 중간에 실패했을 때 */
	msi_sysfs_remove_desc(dev, desc);	/* [한국어] 만들어 둔 것을 전부 되돌린다. show 검사가 어디까지 붙었는지 구분한다 */
	return ret;	/* [한국어] 실패 원인 */
}

#if defined(CONFIG_PCI_MSI_ARCH_FALLBACKS) || defined(CONFIG_PCI_XEN)	/* [한국어] 아래 두 함수는 도메인을 쓰지 않는 옛 경로 전용이다. 아키텍처 고유 PCI MSI 구현이나 Xen 이 그렇다 */
/**
 * msi_device_populate_sysfs - Populate msi_irqs sysfs entries for a device
 * @dev:	The device (PCI, platform etc) which will get sysfs entries
 */
/*
 * [한국어]
 * msi_device_populate_sysfs - 장치의 모든 MSI 인터럽트에 sysfs 항목을 만든다
 *
 * @dev: 대상 장치
 * @return: 0 성공, 음수 오류
 *
 * 도메인을 쓰지 않는 경로를 위한 함수다. 정상 경로에서는
 * __msi_domain_alloc_irqs() 가 서술자마다 파일을 만들어 주지만,
 * 아키텍처 고유 구현이나 Xen 은 그 경로를 거치지 않는다. 그래서
 * 할당이 끝난 뒤 이 함수로 몰아서 만든다.
 *
 * 이미 파일이 있는 서술자를 건너뛰는 것에 주목: 이 함수가 여러 번
 * 불릴 수 있고, 그때 두 번 만들면 sysfs 가 이름 충돌로 실패한다.
 *
 * 실패 시 되돌리지 않는 것이 눈에 띈다 — 중간에서 그냥 반환한다.
 * 호출자가 전체 MSI 설정을 되돌리면서 정리하게 되어 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유 (msi_for_each_desc 가 요구).
 *
 * 호출 체인:
 *   아키텍처 고유 PCI MSI 설정 / Xen PCI 프론트엔드 → [이 함수]
 */
int msi_device_populate_sysfs(struct device *dev)
{
	struct msi_desc *desc;	/* [한국어] 순회용 */
	int ret;	/* [한국어] 결과 */

	msi_for_each_desc(desc, dev, MSI_DESC_ASSOCIATED) {	/* [한국어] 인터럽트가 배정된 것만. 파일 이름이 인터럽트 번호라 배정 전에는 만들 수 없다 */
		if (desc->sysfs_attrs)	/* [한국어] 이미 만들었는가 */
			continue;	/* [한국어] 두 번 만들면 sysfs 가 이름 충돌로 실패한다 */
		ret = msi_sysfs_populate_desc(dev, desc);	/* [한국어] 이 서술자의 파일들을 만든다 */
		if (ret)	/* [한국어] 실패 */
			return ret;	/* [한국어] 되돌리지 않는다. 호출자가 전체 설정을 되돌리며 정리한다 */
	}
	return 0;	/* [한국어] 성공 */
}

/**
 * msi_device_destroy_sysfs - Destroy msi_irqs sysfs entries for a device
 * @dev:		The device (PCI, platform etc) for which to remove
 *			sysfs entries
 */
/*
 * [한국어]
 * msi_device_destroy_sysfs - 장치의 모든 MSI sysfs 항목을 제거한다
 *
 * @dev: 대상 장치
 * @return: 없음
 *
 * 위 populate 의 반대다. 필터가 MSI_DESC_ALL 인 것에 주목 —
 * populate 는 배정된 것만 보지만 여기서는 전부 본다.
 *
 * 왜 다른가: 해제 시점에는 이미 인터럽트 번호가 반납되어 desc->irq 가
 * 0 이 됐을 수 있다. ASSOCIATED 로 걸러 내면 그런 서술자의 파일이
 * 남는다. remove 함수 자체가 sysfs_attrs 가 NULL 이면 조용히 넘어가므로
 * 전부 훑어도 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   아키텍처 고유 PCI MSI 해제 / Xen → [이 함수] → msi_sysfs_remove_desc()
 */
void msi_device_destroy_sysfs(struct device *dev)
{
	struct msi_desc *desc;	/* [한국어] 순회용 */

	msi_for_each_desc(desc, dev, MSI_DESC_ALL)	/* [한국어] 전부 훑는다. 해제 시점에는 irq 가 이미 0 이 됐을 수 있어 ASSOCIATED 로 거르면 파일이 남는다 */
		msi_sysfs_remove_desc(dev, desc);	/* [한국어] 파일이 없는 서술자는 그 안에서 조용히 넘어간다 */
}
#endif /* CONFIG_PCI_MSI_ARCH_FALLBACK || CONFIG_PCI_XEN */	/* [한국어] 옛 경로 전용 구역의 끝 */
#else /* CONFIG_SYSFS */	/* [한국어] sysfs 를 뺀 빌드 — 아래 세 함수는 전부 빈 껍데기다 */
/*
 * [한국어]
 * msi_sysfs_create_group - msi_irqs 디렉터리 생성 (sysfs 없는 빌드)
 *
 * @dev: 무시
 * @return: 항상 0
 *
 * 만들 파일 시스템이 없다. 호출자인 msi_setup_device_data() 를
 * #ifdef 로 나누지 않으려고 성공만 반환한다.
 *
 * 호출 체인:
 *   msi_setup_device_data() → [이 함수]
 */
static inline int msi_sysfs_create_group(struct device *dev) { return 0; }	/* [한국어] 만들 것이 없어 성공만 알린다 */
/*
 * [한국어]
 * msi_sysfs_populate_desc - 서술자의 sysfs 파일 생성 (sysfs 없는 빌드)
 *
 * @dev:  무시
 * @desc: 무시
 * @return: 항상 0
 *
 * 할당 경로가 조건 없이 부르므로 성공만 반환하는 판이 필요하다.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() → [이 함수]
 */
static inline int msi_sysfs_populate_desc(struct device *dev, struct msi_desc *desc) { return 0; }	/* [한국어] 만들 것이 없다 */
/*
 * [한국어]
 * msi_sysfs_remove_desc - 서술자의 sysfs 파일 제거 (sysfs 없는 빌드)
 *
 * @dev:  무시
 * @desc: 무시
 * @return: 없음
 *
 * populate 가 아무것도 만들지 않았으므로 지울 것도 없다.
 *
 * 호출 체인:
 *   __msi_domain_free_irqs() → [이 함수]
 */
static inline void msi_sysfs_remove_desc(struct device *dev, struct msi_desc *desc) { }	/* [한국어] 지울 것이 없다 */
#endif /* !CONFIG_SYSFS */	/* [한국어] sysfs 분기의 끝 */

/*
 * [한국어]
 * msi_get_device_domain - 장치의 도메인 슬롯에서 도메인을 꺼낸다
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @return: 도메인 포인터, 없거나 부적절하면 NULL
 *
 * 이 파일의 거의 모든 함수가 처음에 부르는 조회 함수다. 세 단계로
 * 걸러 낸다.
 *
 * (1) 도메인 ID 가 배열 범위 안인가 — 호출자의 버그를 잡는다.
 * (2) 그 슬롯에 도메인이 있는가 — 아직 만들지 않았거나 이미 없앤 경우다.
 * (3) 그것이 "MSI 부모" 도메인이 아닌가.
 *
 * 마지막 검사가 이 함수의 존재 이유에 가깝다. MSI 부모 도메인은
 * 장치가 직접 인터럽트를 할당받는 도메인이 아니라, 장치별 도메인을
 * 만들 때 부모가 되는 도메인이다. 그것을 슬롯에서 꺼내 할당에 쓰면
 * 계층이 어긋난다. msi_setup_device_data() 가 애초에 그런 도메인을
 * 슬롯에 넣지 않지만, 여기서 한 번 더 막는다.
 *
 * lockdep_assert_held: 슬롯의 도메인 포인터가 락 아래에서 바뀐다.
 * 락 없이 읽으면 방금 제거된 도메인을 쓸 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   이 파일의 거의 모든 도메인 조작 함수 → [이 함수]
 */
static struct irq_domain *msi_get_device_domain(struct device *dev, unsigned int domid)
{
	struct irq_domain *domain;	/* [한국어] 슬롯에서 꺼낸 도메인 */

	lockdep_assert_held(&dev->msi.data->mutex);	/* [한국어] 슬롯 포인터가 락 아래에서 바뀐다. 락 없이 읽으면 방금 제거된 도메인을 쓸 수 있다 */

	if (WARN_ON_ONCE(domid >= MSI_MAX_DEVICE_IRQDOMAINS))	/* [한국어] ID 가 배열 범위 안인가 — 호출자의 버그다 */
		return NULL;

	domain = dev->msi.data->__domains[domid].domain;	/* [한국어] 슬롯에서 꺼낸다 */
	if (!domain)	/* [한국어] 아직 만들지 않았거나 이미 없앴는가 */
		return NULL;	/* [한국어] 정상적인 경우이므로 경고하지 않는다 */

	if (WARN_ON_ONCE(irq_domain_is_msi_parent(domain)))	/* [한국어] MSI 부모 도메인이 슬롯에 들어 있는가 — 장치별 도메인의 부모일 뿐 직접 쓰는 것이 아니다 */
		return NULL;	/* [한국어] 그것으로 할당하면 계층이 어긋난다. setup 이 애초에 막지만 한 번 더 막는다 */

	return domain;	/* [한국어] 이 도메인에 대고 할당·해제해도 된다 */
}

/*
 * [한국어]
 * msi_domain_get_hwsize - 도메인이 표현할 수 있는 인덱스 수를 얻는다
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @return: 인덱스 상한 (개수), 도메인이 없으면 MSI_XA_DOMAIN_SIZE
 *
 * MSI-X 테이블 크기 같은 하드웨어 한계를 코어가 아는 통로다. 인덱스
 * 검사와 xa_alloc 의 검색 범위가 전부 이 값에 기댄다.
 *
 * 도메인이 없을 때 최대값을 돌려주는 것에 주목: 오류가 아니다.
 * 아키텍처 고유의 옛 PCI MSI 지원은 도메인 없이 서술자 저장소만
 * 쓰는데, 그쪽에 한계를 강요할 근거가 없다. 그래서 인덱스 공간
 * 전체를 허용한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_insert_desc() / msi_ctrl_valid() / msi_domain_alloc_irqs_all_locked()
 *   → [이 함수] → msi_get_device_domain()
 */
static unsigned int msi_domain_get_hwsize(struct device *dev, unsigned int domid)
{
	struct msi_domain_info *info;	/* [한국어] 도메인의 설정 구조체 */
	struct irq_domain *domain;	/* [한국어] 대상 도메인 */

	domain = msi_get_device_domain(dev, domid);	/* [한국어] 슬롯에서 꺼낸다 */
	if (domain) {	/* [한국어] 도메인이 있는가 */
		info = domain->host_data;	/* [한국어] MSI 도메인은 host_data 에 msi_domain_info 를 둔다 */
		return info->hwsize;	/* [한국어] 도메인 생성 시 정해진 하드웨어 테이블 크기 */
	}
	/* No domain, default to MSI_XA_DOMAIN_SIZE */
	return MSI_XA_DOMAIN_SIZE;	/* [한국어] (위 영어 주석) 오류가 아니다. 도메인 없이 저장소만 쓰는 옛 PCI MSI 지원에 한계를 강요할 근거가 없어 인덱스 공간 전체를 허용한다 */
}

/*
 * [한국어]
 * irq_chip_write_msi_msg - 조립된 MSI 메시지를 장치에 써 넣는다
 *
 * @data: 대상 irq_data
 * @msg:  써 넣을 메시지 (주소 + 데이터)
 * @return: 없음
 *
 * 한 줄짜리 래퍼인데도 따로 두는 이유는 이름이다. 호출부에서
 * data->chip->irq_write_msi_msg(data, msg) 라고 쓰는 것보다 무엇을
 * 하는지 분명하다. 이 파일에서 세 곳이 부른다.
 *
 * 실제로 무엇이 일어나는가: 칩 콜백이 장치의 MSI 설정 공간이나 MSI-X
 * 테이블 항목에 주소와 데이터를 써 넣는다. 그 뒤로 장치가 인터럽트를
 * 보낼 때 그 주소에 그 값을 쓴다.
 *
 * NULL 검사가 없는 것에 주목: MSI 도메인의 칩은 이 콜백을 반드시
 * 제공해야 한다. 없으면 메시지를 전달할 방법이 없어 MSI 자체가
 * 성립하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 인터럽트 문맥(친화도 변경 경로).
 *
 * 호출 체인:
 *   msi_domain_set_affinity() / msi_domain_activate() /
 *   msi_domain_deactivate() → [이 함수] → chip->irq_write_msi_msg()
 */
static inline void irq_chip_write_msi_msg(struct irq_data *data,
					  struct msi_msg *msg)
{
	data->chip->irq_write_msi_msg(data, msg);	/* [한국어] 장치의 MSI 레지스터나 MSI-X 테이블 항목에 써 넣는다. NULL 검사가 없는 것은 MSI 도메인의 칩이 반드시 제공해야 하는 콜백이어서다 */
}

/*
 * [한국어]
 * msi_check_level - 두 번째 메시지를 쓸 자격이 있는지 검사한다
 *
 * @domain: 대상 도메인
 * @msg:    조립된 메시지 배열 (두 개)
 * @return: 없음 (문제가 있으면 경고만 찍는다)
 *
 * 왜 메시지가 두 개인가: 대부분의 MSI 는 에지 트리거라 인터럽트를
 * 올리는 메시지 하나면 된다. 그런데 일부 하드웨어는 레벨 트리거
 * MSI 를 지원한다 — 인터럽트를 올리는 메시지와 내리는 메시지가
 * 따로 있다. 그것이 msg[1] 이다.
 *
 * 이 함수는 그 두 번째 메시지를 채운 칩이 실제로 그럴 자격이
 * 있는지 본다. 자격이란 도메인이 MSI_FLAG_LEVEL_CAPABLE 을,
 * 칩이 IRQCHIP_SUPPORTS_LEVEL_MSI 를 세운 것이다.
 *
 * 왜 검사가 필요한가: 자격 없는 칩이 msg[1] 을 채우면 그 값이
 * 조용히 무시되거나 엉뚱하게 쓰인다. 레벨 트리거를 기대한 장치가
 * 인터럽트를 내리지 못해 폭주하게 된다. 조립 단계에서 잡는 편이
 * 훨씬 낫다.
 *
 * 조건식이 읽기 어려운데, "두 플래그가 다 있는 것은 아니면서
 * msg[1] 에 무언가 채워져 있으면" 경고한다는 뜻이다.
 *
 * 실행 컨텍스트: 호출자를 따른다.
 *
 * 호출 체인:
 *   msi_domain_set_affinity() / msi_domain_activate() → [이 함수]
 */
static void msi_check_level(struct irq_domain *domain, struct msi_msg *msg)
{
	struct msi_domain_info *info = domain->host_data;	/* [한국어] 도메인의 능력 플래그를 볼 설정 구조체 */

	/*
	 * If the MSI provider has messed with the second message and
	 * not advertized that it is level-capable, signal the breakage.
	 */
	WARN_ON(!((info->flags & MSI_FLAG_LEVEL_CAPABLE) &&	/* [한국어] (위 영어 주석) 도메인이 레벨 MSI 를 지원한다고 선언했고 */
		  (info->chip->flags & IRQCHIP_SUPPORTS_LEVEL_MSI)) &&	/* [한국어] 칩도 그렇다고 선언했는가. 둘 다 아니면서 */
		(msg[1].address_lo || msg[1].address_hi || msg[1].data));	/* [한국어] 두 번째 메시지에 무언가 채워졌는가. 자격 없이 채우면 값이 무시되어 인터럽트를 내리지 못하는 폭주가 생긴다 */
}

/**
 * msi_domain_set_affinity - Generic affinity setter function for MSI domains
 * @irq_data:	The irq data associated to the interrupt
 * @mask:	The affinity mask to set
 * @force:	Flag to enforce setting (disable online checks)
 *
 * Intended to be used by MSI interrupt controllers which are
 * implemented with hierarchical domains.
 *
 * Return: IRQ_SET_MASK_* result code
 */
/*
 * [한국어]
 * msi_domain_set_affinity - MSI 인터럽트의 친화도를 바꾼다
 *
 * @irq_data: 대상 irq_data
 * @mask:     새 CPU 마스크
 * @force:    온라인 검사를 무시할지
 * @return:   IRQ_SET_MASK_ 계열 결과 코드
 *
 * MSI 의 친화도 변경이 일반 인터럽트와 다른 점이 이 함수의 전부다.
 *
 * 일반 인터럽트는 컨트롤러의 라우팅 레지스터를 고치면 끝난다. MSI 는
 * 그럴 수 없다 — 목적지 CPU 정보가 장치가 보내는 메시지 안에 들어
 * 있기 때문이다. 그래서 CPU 를 바꾸려면 메시지를 다시 조립해 장치에
 * 써 넣어야 한다.
 *
 * 절차가 세 단계다. 먼저 부모(벡터 도메인)에게 새 CPU 의 벡터를
 * 배정받고, 그 결과로 메시지를 다시 조립하고, 장치에 써 넣는다.
 *
 * IRQ_SET_MASK_OK_DONE 인 경우를 건너뛰는 이유: 그 값은 "부모가 다
 * 처리했으니 더 할 일 없음" 을 뜻한다. 인터럽트 리매핑이 있는
 * 시스템이 그렇다 — 메시지는 리매핑 테이블 항목을 가리키고, 그
 * 테이블만 고치면 되므로 장치를 건드릴 필요가 없다. 그것이 MSI
 * 친화도 변경을 훨씬 안전하게 만드는 이유이기도 하다.
 *
 * BUG_ON 을 쓰는 것이 눈에 띈다. 메시지 조립이 실패하면 그 인터럽트는
 * 어디로도 갈 수 없는 상태가 되는데, 그 상태로 계속 진행하면 장치가
 * 옛 CPU 로 인터럽트를 보내고 그 CPU 의 벡터는 이미 회수됐을 수 있다.
 * 조용히 넘어가면 원인 모를 오동작이 된다는 판단이다.
 *
 * 실행 컨텍스트: desc->lock 보유. 인터럽트 문맥일 수 있다.
 *
 * 호출 체인:
 *   irq_do_set_affinity() → chip->irq_set_affinity → [이 함수] →
 *   부모의 irq_set_affinity → irq_chip_compose_msi_msg() →
 *   irq_chip_write_msi_msg()
 */
int msi_domain_set_affinity(struct irq_data *irq_data,
			    const struct cpumask *mask, bool force)
{
	struct irq_data *parent = irq_data->parent_data;	/* [한국어] 벡터를 실제로 배정하는 층 */
	struct msi_msg msg[2] = { [1] = { }, };	/* [한국어] 두 번째 항목을 명시적으로 0 으로 둔다. 레벨 MSI 가 아니면 채워지지 않아야 하고, msi_check_level 이 그것을 검사한다 */
	int ret;	/* [한국어] 부모의 결과 */

	ret = parent->chip->irq_set_affinity(parent, mask, force);	/* [한국어] 먼저 새 CPU 의 벡터를 배정받는다. 이 결과가 메시지 내용을 정한다 */
	if (ret >= 0 && ret != IRQ_SET_MASK_OK_DONE) {	/* [한국어] 성공했고 "더 할 일 없음" 이 아닌가. OK_DONE 은 인터럽트 리매핑처럼 테이블만 고치면 되는 경우다 */
		BUG_ON(irq_chip_compose_msi_msg(irq_data, msg));	/* [한국어] 새 목적지로 메시지를 다시 조립한다. 실패하면 인터럽트가 갈 곳을 잃으므로 조용히 넘어가지 않는다 */
		msi_check_level(irq_data->domain, msg);	/* [한국어] 두 번째 메시지를 채울 자격이 있는지 검사 */
		irq_chip_write_msi_msg(irq_data, msg);	/* [한국어] 장치에 써 넣는다. 이 순간부터 장치가 새 CPU 로 인터럽트를 보낸다 */
	}

	return ret;	/* [한국어] 부모의 결과를 그대로 올린다 */
}

/*
 * [한국어]
 * msi_domain_activate - 인터럽트를 활성화하며 메시지를 장치에 써 넣는다
 *
 * @domain:   대상 도메인
 * @irq_data: 대상 irq_data
 * @early:    이른 활성화인가 (여기서는 쓰지 않는다)
 * @return:   항상 0
 *
 * 도메인의 activate 콜백이다. 활성화란 인터럽트가 실제로 동작할 수
 * 있게 자원을 확정하는 단계인데, MSI 에서는 그것이 곧 "메시지를
 * 장치에 써 넣기" 다.
 *
 * early 인자를 무시하는 것에 주목: 그 값은 예약 모드(reservation
 * mode)에서 의미를 갖는다. 이른 활성화에서는 더미 벡터를 배정하고,
 * 나중에 request_irq 때 진짜 벡터로 다시 활성화한다. 그 구분은 아래
 * 벡터 도메인이 하므로 여기서는 신경 쓰지 않고 조립된 결과를 그대로
 * 써 넣는다.
 *
 * 항상 0 을 돌려주는 이유: 실패할 수 있는 유일한 단계인 메시지
 * 조립을 BUG_ON 으로 처리했다. 위 set_affinity 와 같은 판단이다 —
 * 메시지 없이 활성화된 인터럽트는 원인 모를 오동작을 낳는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 할당 또는 요청 경로.
 *
 * 호출 체인:
 *   irq_domain_activate_irq() → domain->ops->activate → [이 함수]
 */
static int msi_domain_activate(struct irq_domain *domain,
			       struct irq_data *irq_data, bool early)
{
	struct msi_msg msg[2] = { [1] = { }, };	/* [한국어] 레벨 MSI 가 아니면 두 번째는 비어 있어야 한다 */

	BUG_ON(irq_chip_compose_msi_msg(irq_data, msg));	/* [한국어] 조립 실패는 인터럽트가 갈 곳을 잃는다는 뜻이다. early 인자를 쓰지 않는 것은 예약 모드 구분을 아래 벡터 도메인이 하기 때문이다 */
	msi_check_level(irq_data->domain, msg);	/* [한국어] 두 번째 메시지 자격 검사 */
	irq_chip_write_msi_msg(irq_data, msg);	/* [한국어] 장치에 써 넣는다. 이 뒤로 장치가 인터럽트를 보낼 수 있다 */
	return 0;	/* [한국어] 실패 경로가 BUG_ON 뿐이라 여기 도달하면 성공이다 */
}

/*
 * [한국어]
 * msi_domain_deactivate - 인터럽트를 비활성화하며 메시지를 지운다
 *
 * @domain:   대상 도메인
 * @irq_data: 대상 irq_data
 * @return:   없음
 *
 * 위 activate 의 반대다. 0 으로 채운 메시지를 장치에 써 넣는다.
 *
 * 왜 0 인가: 주소 0 은 유효한 MSI 목적지가 아니므로, 장치가 그
 * 메시지로 인터럽트를 보내려 해도 아무 데도 도달하지 않는다. 즉
 * 0 을 쓰는 것이 곧 "인터럽트를 보내지 마라" 다.
 *
 * 이것이 필요한 이유: 비활성화 뒤에는 벡터가 회수되어 다른 인터럽트에
 * 재배정될 수 있다. 옛 메시지가 남아 있으면 장치가 남의 벡터로
 * 인터럽트를 보내게 된다.
 *
 * memset 을 쓰는 것에 주목: 위 두 함수는 지정 초기화로 0 을 만들지만
 * 여기서는 배열 전체를 확실히 밀어야 하므로 memset 이 더 명확하다.
 * 그리고 여기서는 msi_check_level 을 부르지 않는다 — 어차피 전부
 * 0 이라 검사할 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 해제 경로.
 *
 * 호출 체인:
 *   irq_domain_deactivate_irq() → domain->ops->deactivate → [이 함수]
 */
static void msi_domain_deactivate(struct irq_domain *domain,
				  struct irq_data *irq_data)
{
	struct msi_msg msg[2];	/* [한국어] 초기화하지 않고 아래에서 통째로 민다 */

	memset(msg, 0, sizeof(msg));	/* [한국어] 배열 전체를 0 으로. 주소 0 은 유효한 목적지가 아니라 "보내지 마라" 는 뜻이 된다 */
	irq_chip_write_msi_msg(irq_data, msg);	/* [한국어] 장치에 써 넣는다. 이 줄이 없으면 벡터가 재배정된 뒤 장치가 남의 벡터로 인터럽트를 보낸다 */
}

/*
 * [한국어]
 * msi_domain_alloc - 도메인의 alloc 콜백: 인터럽트 여러 개를 할당한다
 *
 * @domain:  대상 도메인
 * @virq:    배정된 첫 리눅스 인터럽트 번호
 * @nr_irqs: 개수
 * @arg:     msi_alloc_info_t — 하드웨어 번호와 서술자 정보
 * @return:  0 성공, 음수 오류
 *
 * 계층형 도메인의 할당 규약을 그대로 따른다. 부모부터 할당하고,
 * 그 다음에 자기 층을 초기화한다.
 *
 * 왜 부모가 먼저인가: 자기 층의 초기화가 부모의 결과에 기댈 수 있다.
 * 예를 들어 부모가 배정한 벡터 번호를 알아야 메시지를 조립할 수 있다.
 *
 * 첫 줄의 중복 검사: 같은 하드웨어 번호가 이미 매핑돼 있으면
 * -EEXIST 다. 두 리눅스 번호가 한 하드웨어 인터럽트를 가리키면
 * 마스크·언마스크가 서로를 덮는다.
 *
 * 에러 처리가 두 겹인 것에 주목: 자기 층 초기화가 중간에 실패하면
 * 이미 초기화한 것들을 msi_free 로 되돌리고, 그 다음에
 * irq_domain_free_irqs_top() 으로 부모 층까지 되돌린다. 두 번째가
 * 없으면 부모가 배정한 벡터가 샌다.
 *
 * i-- 로 시작하는 역순 루프: 실패한 i 번은 초기화되지 않았으므로
 * 그 앞부터 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 뮤텍스 보유.
 *
 * 호출 체인:
 *   __irq_domain_alloc_irqs() → domain->ops->alloc → [이 함수] →
 *   irq_domain_alloc_irqs_parent() → ops->msi_init()
 */
static int msi_domain_alloc(struct irq_domain *domain, unsigned int virq,
			    unsigned int nr_irqs, void *arg)
{
	struct msi_domain_info *info = domain->host_data;	/* [한국어] 도메인 설정 */
	struct msi_domain_ops *ops = info->ops;	/* [한국어] MSI 고유 콜백 묶음 */
	irq_hw_number_t hwirq = ops->get_hwirq(info, arg);	/* [한국어] 이 할당의 하드웨어 번호. 기본 구현은 arg->hwirq 를 그대로 쓴다 */
	int i, ret;	/* [한국어] 순회 인덱스와 결과 */

	if (irq_resolve_mapping(domain, hwirq))	/* [한국어] 이 하드웨어 번호가 이미 매핑돼 있는가 */
		return -EEXIST;	/* [한국어] 두 리눅스 번호가 한 하드웨어 인터럽트를 가리키면 마스크·언마스크가 서로를 덮는다 */

	if (domain->parent) {	/* [한국어] 계층형인가 — MSI 도메인은 거의 항상 그렇다 */
		ret = irq_domain_alloc_irqs_parent(domain, virq, nr_irqs, arg);	/* [한국어] 부모부터 할당한다. 자기 층 초기화가 부모의 결과(벡터 번호 등)에 기대기 때문이다 */
		if (ret < 0)	/* [한국어] 부모가 실패 — 벡터 고갈 등 */
			return ret;	/* [한국어] 아직 자기 층을 건드리지 않았으므로 되돌릴 것이 없다 */
	}

	for (i = 0; i < nr_irqs; i++) {	/* [한국어] 자기 층의 각 인터럽트를 초기화한다 */
		ret = ops->msi_init(domain, info, virq + i, hwirq + i, arg);	/* [한국어] 칩과 흐름 처리기를 건다. 기본 구현은 msi_domain_ops_init 이다 */
		if (ret < 0) {	/* [한국어] 초기화 실패 */
			if (ops->msi_free) {	/* [한국어] 정리 콜백이 있는가 */
				for (i--; i >= 0; i--)	/* [한국어] 실패한 i 번은 초기화되지 않았으므로 그 앞부터 역순으로 */
					ops->msi_free(domain, info, virq + i);	/* [한국어] 자기 층에서 잡은 것을 푼다 */
			}
			irq_domain_free_irqs_top(domain, virq, nr_irqs);	/* [한국어] 부모 층까지 되돌린다. 이 줄이 없으면 부모가 배정한 벡터가 샌다 */
			return ret;	/* [한국어] 준비 콜백이 실패했다. 아직 아무 인터럽트도 배정하지 않아 되돌릴 것이 없다 */
		}
	}

	return 0;	/* [한국어] 모든 층이 준비됐다 */
}

/*
 * [한국어]
 * msi_domain_free - 도메인의 free 콜백: 인터럽트들을 반납한다
 *
 * @domain:  대상 도메인
 * @virq:    첫 리눅스 인터럽트 번호
 * @nr_irqs: 개수
 * @return:  없음
 *
 * 위 alloc 의 반대다. 자기 층을 먼저 풀고 부모 층을 나중에 푼다 —
 * 할당의 정확한 역순이다.
 *
 * 왜 그 순서인가: 자기 층의 정리가 부모의 자원을 참조할 수 있다.
 * 부모를 먼저 풀면 그 참조가 해제된 것을 가리킨다.
 *
 * msi_free 가 선택적인 것에 주목: 대부분의 MSI 도메인은 자기 층에서
 * 따로 잡는 자원이 없어 이 콜백을 제공하지 않는다.
 *
 * irq_domain_free_irqs_top() 이 이름과 달리 부모까지 내려가며 푸는
 * 것이 계층형 도메인의 규약이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 뮤텍스 보유.
 *
 * 호출 체인:
 *   irq_domain_free_irqs() → domain->ops->free → [이 함수]
 */
static void msi_domain_free(struct irq_domain *domain, unsigned int virq,
			    unsigned int nr_irqs)
{
	struct msi_domain_info *info = domain->host_data;	/* [한국어] 도메인 설정 */
	int i;	/* [한국어] 순회용 */

	if (info->ops->msi_free) {	/* [한국어] 자기 층에 정리할 것이 있는 도메인인가. 대부분은 없다 */
		for (i = 0; i < nr_irqs; i++)	/* [한국어] 각 인터럽트에 대해 */
			info->ops->msi_free(domain, info, virq + i);	/* [한국어] 자기 층 정리. 부모보다 먼저 해야 부모 자원 참조가 유효하다 */
	}
	irq_domain_free_irqs_top(domain, virq, nr_irqs);	/* [한국어] 부모 층까지 내려가며 푼다. 이름과 달리 계층 전체를 다룬다 */
}

/*
 * [한국어]
 * msi_domain_translate - 펌웨어 명세를 하드웨어 번호로 해석한다
 *
 * @domain: 대상 도메인
 * @fwspec: 디바이스 트리나 ACPI 에서 온 인터럽트 명세
 * @hwirq:  해석 결과 하드웨어 번호 (출력)
 * @type:   해석 결과 트리거 방식 (출력)
 * @return: 0 성공, -ENOTSUPP 이 도메인은 그런 방식을 지원하지 않음
 *
 * 대부분의 MSI 도메인에서 이 콜백은 함정 역할을 한다. 원본 주석이
 * 그것을 명확히 한다 — "일반 irqdomain 경로를 통한 할당을 잡아낸다".
 *
 * 왜 함정인가: MSI 인터럽트는 디바이스 트리에 적혀 있지 않다. 장치가
 * 몇 개의 MSI 를 쓸지는 실행 시간에 정해지고, 그 할당은 PCI 계층이나
 * 이 파일의 API 를 통해야 한다. 누군가 irq_create_fwspec_mapping()
 * 으로 MSI 도메인에 매핑을 만들려 하면 그것은 잘못된 경로다.
 *
 * 예외가 wire-to-MSI 브리지다. MBIGEN 같은 하드웨어는 실제 배선
 * 인터럽트를 MSI 로 변환하므로, 그 배선이 디바이스 트리에 적혀 있다.
 * 그런 도메인만 msi_translate 콜백을 제공해 이 함정을 통과한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_fwspec_mapping() → domain->ops->translate → [이 함수]
 */
static int msi_domain_translate(struct irq_domain *domain, struct irq_fwspec *fwspec,
				irq_hw_number_t *hwirq, unsigned int *type)
{
	struct msi_domain_info *info = domain->host_data;	/* [한국어] 도메인 설정 */

	/*
	 * This will catch allocations through the regular irqdomain path except
	 * for MSI domains which really support this, e.g. MBIGEN.
	 */
	if (!info->ops->msi_translate)	/* [한국어] (위 영어 주석) 이 도메인이 펌웨어 명세를 해석할 수 있는가. 대부분의 MSI 도메인은 못 한다 */
		return -ENOTSUPP;	/* [한국어] MSI 는 디바이스 트리에 적히지 않는다. 이 경로로 오는 것 자체가 잘못된 할당이다 */
	return info->ops->msi_translate(domain, fwspec, hwirq, type);	/* [한국어] wire-to-MSI 브리지처럼 실제 배선을 MSI 로 바꾸는 도메인만 여기 온다 */
}

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS	/* [한국어] debugfs 로 인터럽트 내부를 들여다보는 빌드에만 */
/*
 * [한국어]
 * msi_domain_debug_show - debugfs 에 MSI 메시지 내용을 출력한다
 *
 * @m:    seq_file 출력 대상
 * @d:    대상 도메인
 * @irqd: 대상 irq_data (도메인 자체를 볼 때는 NULL)
 * @ind:  들여쓰기 칸 수
 * @return: 없음
 *
 * /sys/kernel/debug/irq/irqs/<번호> 에 MSI 메시지의 주소와 데이터를
 * 덧붙인다. 인터럽트가 엉뚱한 CPU 로 가거나 아예 오지 않을 때 가장
 * 먼저 보는 값이다 — 주소가 0 이면 비활성화된 것이고, 데이터의
 * 하위 비트가 벡터 번호를 담는다.
 *
 * irqd 가 NULL 인 경우: 도메인 자체의 정보를 출력하는 호출이다.
 * MSI 메시지는 인터럽트마다 다르므로 그때는 출력할 것이 없다.
 *
 * 캐시된 사본(desc->msg)을 출력하는 것에 주목: 하드웨어를 되읽지
 * 않는다. 장치가 절전 중이면 읽을 수 없고, 코어가 의도한 값을 보는
 * 편이 진단에 더 유용하다.
 *
 * 들여쓰기에 "%*s" 와 빈 문자열을 쓰는 관용구: 폭만큼 공백을 찍는다.
 * ind + 1 인 것은 이 항목들이 상위 항목보다 한 칸 더 들어가야 해서다.
 *
 * 실행 컨텍스트: 프로세스 문맥, debugfs 읽기.
 *
 * 호출 체인:
 *   cat /sys/kernel/debug/irq/irqs/N → irq_debug_show_data() →
 *   domain->ops->debug_show → [이 함수]
 */
static void msi_domain_debug_show(struct seq_file *m, struct irq_domain *d,
				  struct irq_data *irqd, int ind)
{
	struct msi_desc *desc = irqd ? irq_data_get_msi_desc(irqd) : NULL;	/* [한국어] 도메인 자체를 볼 때는 irqd 가 NULL 이고, 그때는 출력할 메시지가 없다 */

	if (!desc)	/* [한국어] 서술자가 없는가 */
		return;	/* [한국어] MSI 가 아니거나 도메인 정보 출력이다 */

	seq_printf(m, "\n%*saddress_hi: 0x%08x", ind + 1, "", desc->msg.address_hi);	/* [한국어] 목적지 주소의 상위 32비트. 64비트 MSI 에서만 의미가 있다 */
	seq_printf(m, "\n%*saddress_lo: 0x%08x", ind + 1, "", desc->msg.address_lo);	/* [한국어] 하위 32비트. x86 에서는 이 안에 목적지 APIC ID 가 들어 있다 */
	seq_printf(m, "\n%*smsg_data:   0x%08x\n", ind + 1, "", desc->msg.data);	/* [한국어] 장치가 그 주소에 쓸 값. 하위 비트가 벡터 번호를 담는다 */
}
#endif	/* [한국어] CONFIG_GENERIC_IRQ_DEBUGFS 분기의 끝 */

static const struct irq_domain_ops msi_domain_ops = {
	/* [한국어] 모든 MSI 도메인이 공유하는 도메인 연산표.
	 * __msi_create_irq_domain() 이 도메인을 만들 때 이 주소를 넘긴다.
	 * MSI 고유의 동작(메시지 조립·쓰기)은 이 표의 콜백들이 다시
	 * msi_domain_info::ops 로 위임하는 이중 구조다. */
	.alloc		= msi_domain_alloc,
	/* [한국어] 인터럽트 할당 콜백.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: __irq_domain_alloc_irqs().
	 * 값 범위: 항상 이 파일의 msi_domain_alloc.
	 * 동기화: const 테이블이라 변경되지 않는다. */
	.free		= msi_domain_free,
	/* [한국어] 인터럽트 반납 콜백.
	 * 설정자·읽는 자: alloc 과 대칭.
	 * 값 범위: 항상 msi_domain_free.
	 * 동기화: 위와 같다. */
	.activate	= msi_domain_activate,
	/* [한국어] 활성화 콜백 — MSI 에서는 메시지를 장치에 써 넣는 일이다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: irq_domain_activate_irq().
	 * 값 범위: 항상 msi_domain_activate.
	 * 동기화: 위와 같다. */
	.deactivate	= msi_domain_deactivate,
	/* [한국어] 비활성화 콜백 — 메시지를 0 으로 밀어 장치를 침묵시킨다.
	 * 설정자·읽는 자: activate 와 대칭.
	 * 값 범위: 항상 msi_domain_deactivate.
	 * 동기화: 위와 같다. */
	.translate	= msi_domain_translate,
	/* [한국어] 펌웨어 명세 해석 콜백 — 대부분의 도메인에서는 함정이다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: irq_create_fwspec_mapping().
	 * 값 범위: 항상 msi_domain_translate. 그 안에서 도메인별
	 *   msi_translate 유무에 따라 -ENOTSUPP 또는 위임으로 갈린다.
	 * 동기화: 위와 같다. */
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS	/* [한국어] debugfs 가 있는 빌드에만 이 필드가 존재한다 */
	.debug_show     = msi_domain_debug_show,
	/* [한국어] debugfs 출력 콜백.
	 * 설정자: 정적 초기화 (이 빌드에서만).
	 * 읽는 자: irq_debug_show_data().
	 * 값 범위: 항상 msi_domain_debug_show.
	 * 동기화: 위와 같다. */
#endif
};

/*
 * [한국어]
 * msi_domain_ops_get_hwirq - 기본 하드웨어 번호 추출 콜백
 *
 * @info: 도메인 설정 (쓰지 않는다)
 * @arg:  할당 정보
 * @return: arg 에 담긴 하드웨어 번호 그대로
 *
 * 대부분의 MSI 도메인은 하드웨어 번호를 계산할 것이 없다. 호출자가
 * 이미 정해서 arg 에 담아 넘기기 때문이다. 그래서 기본 구현은 그것을
 * 그대로 꺼내 준다.
 *
 * 그런데도 콜백으로 두는 이유: 일부 도메인은 계산이 필요하다. 예를
 * 들어 장치의 버스·슬롯·기능 번호를 하드웨어 번호로 인코딩하는
 * 도메인이 있다. 그런 도메인은 자기 구현을 제공한다.
 *
 * 실행 컨텍스트: 할당 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_domain_alloc() → ops->get_hwirq → [이 함수]
 */
static irq_hw_number_t msi_domain_ops_get_hwirq(struct msi_domain_info *info,
						msi_alloc_info_t *arg)
{
	return arg->hwirq;	/* [한국어] 호출자가 이미 정해 둔 값. 계산이 필요한 도메인만 자기 구현을 제공한다 */
}

/*
 * [한국어]
 * msi_domain_ops_prepare - 기본 할당 준비 콜백
 *
 * @domain: 대상 도메인 (쓰지 않는다)
 * @dev:    대상 장치 (쓰지 않는다)
 * @nvec:   할당할 벡터 수 (쓰지 않는다)
 * @arg:    초기화할 할당 정보
 * @return: 항상 0
 *
 * msi_prepare 콜백은 할당을 시작하기 전에 msi_alloc_info_t 를 채우는
 * 자리다. 아키텍처마다 그 구조체의 내용이 완전히 다르다 — x86 은
 * APIC 정보를, ARM 은 ITS 장치 ID 를 담는다.
 *
 * 기본 구현은 0 으로 미는 것이 전부다. 대부분의 도메인이 특별히
 * 준비할 것이 없고, 0 초기화만 되어 있으면 그 뒤 단계가 필요한
 * 필드를 채우기 때문이다.
 *
 * 그래도 이 함수가 필요한 이유: 0 초기화를 아무도 하지 않으면 스택에
 * 남은 쓰레기 값이 그대로 하드웨어 설정에 들어간다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_domain_prepare_irqs() → ops->msi_prepare → [이 함수]
 */
static int msi_domain_ops_prepare(struct irq_domain *domain, struct device *dev,
				  int nvec, msi_alloc_info_t *arg)
{
	memset(arg, 0, sizeof(*arg));	/* [한국어] 0 초기화가 전부다. 이것을 빠뜨리면 스택의 쓰레기 값이 하드웨어 설정에 들어간다 */
	return 0;	/* [한국어] 실패할 여지가 없다 */
}

/*
 * [한국어]
 * msi_domain_ops_teardown - 기본 정리 콜백 (아무 일도 하지 않는다)
 *
 * @domain: 무시
 * @arg:    무시
 * @return: 없음
 *
 * msi_teardown 은 msi_prepare 가 잡은 것을 푸는 자리다. 기본 prepare 가
 * 0 초기화만 하므로 풀 것이 없다.
 *
 * 그래도 빈 함수를 두는 이유: msi_remove_device_irq_domain() 이 이
 * 콜백을 NULL 검사 없이 부른다. 기본값이 없으면 그곳에 검사를
 * 넣어야 하고, 검사를 빠뜨리면 터진다.
 *
 * 자기 구현을 제공하는 도메인의 예: prepare 에서 ITS 장치 테이블
 * 항목을 잡는 ARM GICv3 ITS 가 그렇다. 그 항목을 여기서 반납한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 제거 경로.
 *
 * 호출 체인:
 *   msi_remove_device_irq_domain() → info->ops->msi_teardown → [이 함수]
 */
static void msi_domain_ops_teardown(struct irq_domain *domain, msi_alloc_info_t *arg)
{
}

/*
 * [한국어]
 * msi_domain_ops_set_desc - 기본 서술자 연결 콜백
 *
 * @arg:  할당 정보
 * @desc: 이번에 할당할 서술자
 * @return: 없음
 *
 * 할당 정보에 "지금 다루는 서술자" 를 담아 둔다. 아래 층의 콜백들이
 * 그 서술자에서 필요한 정보를 꺼내 쓴다 — PCI 라면 버스·장치 번호,
 * 플랫폼 MSI 라면 장치 고유 ID 다.
 *
 * 왜 인자로 넘기지 않고 구조체에 담는가: 계층형 도메인의 alloc 콜백
 * 체인이 void 포인터 하나(arg)만 아래로 전달한다. 서술자를 따로
 * 넘길 자리가 없어 그 안에 실어 보낸다.
 *
 * 자기 구현을 제공하는 도메인: PCI MSI 는 여기서 버스 정보를 함께
 * 채워 넣는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 할당 루프 안.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() → ops->set_desc → [이 함수]
 */
static void msi_domain_ops_set_desc(msi_alloc_info_t *arg,
				    struct msi_desc *desc)
{
	arg->desc = desc;	/* [한국어] 계층형 alloc 체인이 void 포인터 하나만 아래로 전달하므로, 서술자를 그 안에 실어 보낸다 */
}

/*
 * [한국어]
 * msi_domain_ops_init - 기본 인터럽트 초기화 콜백
 *
 * @domain: 대상 도메인
 * @info:   도메인 설정
 * @virq:   초기화할 리눅스 인터럽트 번호
 * @hwirq:  대응하는 하드웨어 번호
 * @arg:    할당 정보 (쓰지 않는다)
 * @return: 항상 0
 *
 * 인터럽트 하나를 실제로 동작 가능하게 만드는 곳이다. 도메인 설정에
 * 담긴 칩과 흐름 처리기를 서술자에 건다.
 *
 * irq_domain_set_hwirq_and_chip() 이 하는 일: 이 층의 irq_data 에
 * 하드웨어 번호와 칩 포인터를 채운다. 계층형이므로 이 층의 것만
 * 채우고 부모 층은 이미 자기 것을 채워 두었다.
 *
 * 흐름 처리기 설정이 조건부인 이유: 대부분의 MSI 도메인은 처리기를
 * 지정하지 않고, 그러면 부모 도메인이 정한 것(대개 handle_edge_irq)이
 * 그대로 쓰인다. MSI 는 본질적으로 에지 트리거이기 때문이다.
 * 처리기를 지정하는 도메인은 특별한 흐름이 필요한 경우다.
 *
 * handler 와 handler_name 을 둘 다 검사하는 것에 주목: 이름은
 * /proc/interrupts 에 표시되므로 처리기를 지정하면 이름도 있어야
 * 한다는 규약이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 할당 경로.
 *
 * 호출 체인:
 *   msi_domain_alloc() → ops->msi_init → [이 함수] →
 *   irq_domain_set_hwirq_and_chip()
 */
static int msi_domain_ops_init(struct irq_domain *domain,
			       struct msi_domain_info *info,
			       unsigned int virq, irq_hw_number_t hwirq,
			       msi_alloc_info_t *arg)
{
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, info->chip,	/* [한국어] 이 층의 irq_data 에 하드웨어 번호와 칩을 채운다. 부모 층은 이미 자기 것을 채웠다 */
				      info->chip_data);
	if (info->handler && info->handler_name) {	/* [한국어] 흐름 처리기를 지정한 도메인인가. 대부분은 아니고, 그러면 부모가 정한 handle_edge_irq 가 쓰인다 — MSI 는 본질적으로 에지 트리거다 */
		__irq_set_handler(virq, info->handler, 0, info->handler_name);	/* [한국어] 처리기와 이름을 함께 건다. 이름은 /proc/interrupts 에 표시된다 */
		if (info->handler_data)	/* [한국어] 처리기가 쓸 사설 데이터도 지정했는가 */
			irq_set_handler_data(virq, info->handler_data);	/* [한국어] 건다 */
	}
	return 0;	/* [한국어] 실패할 여지가 없다 */
}

static struct msi_domain_ops msi_domain_ops_default = {
	/* [한국어] MSI 도메인 콜백의 기본 구현 묶음.
	 * 도메인이 ops 를 아예 주지 않으면 이것이 통째로 쓰이고,
	 * MSI_FLAG_USE_DEF_DOM_OPS 를 세우면 비어 있는 항목만 이것으로
	 * 채워진다. 아래 msi_domain_update_dom_ops() 가 그 일을 한다.
	 * const 가 아닌 이유: 채워 넣는 쪽이 아니라 채워지는 쪽의
	 * 구조체가 const 가 아니어야 대입이 되는데, 대칭을 위해 이쪽도
	 * const 를 붙이지 않았다. 내용은 변하지 않는다. */
	.get_hwirq		= msi_domain_ops_get_hwirq,
	/* [한국어] 할당 정보에서 하드웨어 번호를 꺼내는 콜백.
	 * 설정자: 여기 정적 초기화.
	 * 읽는 자: msi_domain_alloc(), 또는 update_dom_ops 의 채우기.
	 * 값 범위: arg->hwirq 를 그대로 돌려주는 기본 구현.
	 * 동기화: 초기화 후 변경되지 않는다. */
	.msi_init		= msi_domain_ops_init,
	/* [한국어] 인터럽트 하나를 초기화하는 콜백.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 칩과 흐름 처리기를 거는 기본 구현.
	 * 동기화: 위와 같다. */
	.msi_prepare		= msi_domain_ops_prepare,
	/* [한국어] 할당 전 준비 콜백.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 할당 정보를 0 으로 미는 기본 구현.
	 * 동기화: 위와 같다. */
	.msi_teardown		= msi_domain_ops_teardown,
	/* [한국어] 도메인 제거 시 정리 콜백.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: 아무 일도 하지 않는 기본 구현. NULL 이 아니어야
	 *   호출부의 검사가 필요 없다.
	 * 동기화: 위와 같다. */
	.set_desc		= msi_domain_ops_set_desc,
	/* [한국어] 할당 정보에 현재 서술자를 담는 콜백.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: arg->desc 에 대입하는 기본 구현.
	 * 동기화: 위와 같다.
	 * msi_free 와 domain_alloc_irqs 등 나머지 콜백이 여기 없는 것에
	 *   주목 — 그것들은 기본 구현이 없고, 없으면 코어가 자기
	 *   경로를 쓴다. */
};

/*
 * [한국어]
 * msi_domain_update_dom_ops - 도메인의 빈 콜백을 기본 구현으로 채운다
 *
 * @info: 대상 도메인 설정
 * @return: 없음
 *
 * 도메인 드라이버가 콜백을 전부 채울 필요가 없게 해 주는 함수다.
 * 두 가지 경우를 다룬다.
 *
 * ops 가 아예 NULL 이면 기본 묶음을 통째로 가리키게 한다. 가장
 * 단순한 도메인이 이렇다.
 *
 * ops 는 있는데 일부만 채운 경우, MSI_FLAG_USE_DEF_DOM_OPS 를
 * 세웠으면 빈 자리를 하나씩 채운다. 그 플래그를 요구하는 이유:
 * 일부러 NULL 로 둔 콜백이 있을 수 있다. 예를 들어 msi_free 를
 * NULL 로 두면 코어가 아무것도 안 하는데, 그것이 의도일 수 있다.
 *
 * 다만 이 함수가 채우는 다섯 개는 전부 "없으면 곤란한" 것들이라,
 * 플래그를 세운 도메인은 다섯 모두를 기본값으로 받는다.
 *
 * info->ops 가 가리키는 구조체를 직접 고치는 것에 주목: 도메인
 * 드라이버가 넘긴 구조체를 코어가 수정한다. 그래서 그 구조체는
 * const 일 수 없고, 도메인마다 자기 사본을 가져야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   __msi_create_irq_domain() → [이 함수]
 */
static void msi_domain_update_dom_ops(struct msi_domain_info *info)
{
	struct msi_domain_ops *ops = info->ops;	/* [한국어] 도메인이 제공한 콜백 묶음 */

	if (ops == NULL) {	/* [한국어] 아예 없는가 — 가장 단순한 도메인 */
		info->ops = &msi_domain_ops_default;	/* [한국어] 기본 묶음을 통째로 가리키게 한다. 이 경우 아래 개별 채우기가 필요 없다 */
		return;
	}

	if (!(info->flags & MSI_FLAG_USE_DEF_DOM_OPS))	/* [한국어] 빈 자리를 채워 달라고 요청했는가 */
		return;	/* [한국어] 아니면 그대로 둔다. 일부러 NULL 로 둔 콜백이 있을 수 있다 */

	if (ops->get_hwirq == NULL)	/* [한국어] 비어 있으면 */
		ops->get_hwirq = msi_domain_ops_default.get_hwirq;	/* [한국어] 기본 구현으로 채운다. 도메인이 넘긴 구조체를 코어가 직접 고치므로 그것은 const 일 수 없다 */
	if (ops->msi_init == NULL)	/* [한국어] 인터럽트 초기화 콜백 */
		ops->msi_init = msi_domain_ops_default.msi_init;
	if (ops->msi_prepare == NULL)	/* [한국어] 할당 준비 콜백 */
		ops->msi_prepare = msi_domain_ops_default.msi_prepare;
	if (ops->msi_teardown == NULL)	/* [한국어] 정리 콜백 */
		ops->msi_teardown = msi_domain_ops_default.msi_teardown;
	if (ops->set_desc == NULL)	/* [한국어] 서술자 연결 콜백 */
		ops->set_desc = msi_domain_ops_default.set_desc;
}

/*
 * [한국어]
 * msi_domain_update_chip_ops - 도메인 칩의 필수 콜백을 확인하고 채운다
 *
 * @info: 대상 도메인 설정
 * @return: 없음
 *
 * 칩 쪽의 대응 함수다. 다만 위 dom_ops 와 달리 채우는 것이 하나뿐이고
 * 대신 검사가 강하다.
 *
 * BUG_ON 을 쓰는 이유: mask 와 unmask 가 없는 MSI 칩은 동작할 수
 * 없다. 코어가 인터럽트를 막아야 할 때 막지 못하면 폭주를 제어할
 * 방법이 사라진다. 도메인 생성 시점에 잡는 편이 나중에 원인 모를
 * 폭주를 겪는 것보다 낫다.
 *
 * 친화도 설정만 채우는 이유: 그것은 MSI 에 공통인 절차가 있다 —
 * 부모에게 벡터를 받고 메시지를 다시 써 넣는 것이다. 위
 * msi_domain_set_affinity() 가 그 구현이고, 칩마다 다시 쓸 이유가 없다.
 *
 * MSI_FLAG_NO_AFFINITY 를 세운 도메인은 제외한다. 친화도 개념이
 * 없는 하드웨어라면 콜백을 두지 않는 편이 낫다 — 코어가 그 부재를
 * 보고 친화도 설정을 아예 거절한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 도메인 생성 경로.
 *
 * 호출 체인:
 *   __msi_create_irq_domain() → [이 함수]
 */
static void msi_domain_update_chip_ops(struct msi_domain_info *info)
{
	struct irq_chip *chip = info->chip;	/* [한국어] 도메인이 제공한 칩 */

	BUG_ON(!chip || !chip->irq_mask || !chip->irq_unmask);	/* [한국어] 막고 열 수 없는 MSI 칩은 동작할 수 없다. 폭주를 제어할 방법이 사라지므로 생성 시점에 잡는다 */
	if (!chip->irq_set_affinity && !(info->flags & MSI_FLAG_NO_AFFINITY))	/* [한국어] 친화도 콜백이 없고, 친화도 개념이 없는 도메인도 아닌가 */
		chip->irq_set_affinity = msi_domain_set_affinity;	/* [한국어] MSI 공통 구현을 꽂는다. 부모에게 벡터를 받고 메시지를 다시 쓰는 절차는 칩마다 같다 */
}

/*
 * [한국어]
 * __msi_create_irq_domain - MSI 인터럽트 도메인을 만드는 공통 구현
 *
 * @fwnode: 펌웨어 노드 (도메인 식별과 이름에 쓴다)
 * @info:   도메인 설정 — 이 함수가 내용을 수정한다
 * @flags:  추가 도메인 플래그
 * @parent: 부모 도메인
 * @return: 만든 도메인, 실패 시 NULL
 *
 * 이 파일의 세 진입점(msi_create_irq_domain,
 * msi_create_device_irq_domain, 그리고 그 사이의 변형)이 모두 여기로
 * 모인다.
 *
 * hwsize 처리가 두 갈래다. 너무 크면 거절하는데, xarray 인덱스 공간을
 * 넘으면 서술자를 저장할 수 없기 때문이다. 0 이면 최대값으로 바꾸는데,
 * 원본 주석대로 하드웨어 테이블이 없는 도메인이나 옛 호환을 위해서다.
 *
 * IRQ_DOMAIN_FLAG_MSI 를 항상 붙이는 것에 주목: 이 플래그로 코어가
 * "이것은 MSI 도메인" 임을 알고, host_data 를 msi_domain_info 로
 * 해석해도 된다고 판단한다.
 *
 * MSI_FLAG_PARENT_PM_DEV: 전원 관리 대상 장치를 부모에게서 물려받는다.
 * 장치별 MSI 도메인은 자기 하드웨어가 없고 부모 컨트롤러에 얹혀
 * 있으므로, 그 컨트롤러의 전원을 붙잡아야 한다.
 *
 * bus_token 을 나중에 설정하는 이유: 도메인이 만들어진 뒤에야
 * 토큰 갱신 함수를 부를 수 있다. 그 토큰은 도메인을 찾을 때 종류를
 * 구분하는 데 쓰인다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_create_irq_domain() / msi_create_device_irq_domain() →
 *   [이 함수] → irq_domain_create_hierarchy()
 */
static struct irq_domain *__msi_create_irq_domain(struct fwnode_handle *fwnode,
						  struct msi_domain_info *info,
						  unsigned int flags,
						  struct irq_domain *parent)
{
	struct irq_domain *domain;	/* [한국어] 만든 도메인 */

	if (info->hwsize > MSI_XA_DOMAIN_SIZE)	/* [한국어] xarray 인덱스 공간을 넘는가 */
		return NULL;	/* [한국어] 그만한 서술자를 저장할 수 없다 */

	/*
	 * Hardware size 0 is valid for backwards compatibility and for
	 * domains which are not backed by a hardware table. Grant the
	 * maximum index space.
	 */
	if (!info->hwsize)	/* [한국어] (위 영어 주석) 크기를 지정하지 않았는가 */
		info->hwsize = MSI_XA_DOMAIN_SIZE;	/* [한국어] 하드웨어 테이블이 없는 도메인이거나 옛 호환이다. 인덱스 공간 전체를 허용한다 */

	msi_domain_update_dom_ops(info);	/* [한국어] 빈 MSI 콜백을 기본 구현으로 채운다 */
	if (info->flags & MSI_FLAG_USE_DEF_CHIP_OPS)	/* [한국어] 칩 콜백도 채워 달라고 요청했는가 */
		msi_domain_update_chip_ops(info);	/* [한국어] 필수 콜백을 검사하고 친화도 구현을 꽂는다 */

	domain = irq_domain_create_hierarchy(parent, flags | IRQ_DOMAIN_FLAG_MSI, 0,	/* [한국어] 계층형으로 만든다. MSI 플래그가 있어야 코어가 host_data 를 msi_domain_info 로 해석한다. 크기 0 은 "동적" 이라는 뜻 */
					     fwnode, &msi_domain_ops, info);

	if (domain) {	/* [한국어] 생성에 성공했는가 */
		irq_domain_update_bus_token(domain, info->bus_token);	/* [한국어] 도메인 종류 표식. 도메인을 찾을 때 PCI MSI 인지 IMS 인지 구분하는 데 쓴다. 생성 뒤에야 설정할 수 있다 */
		domain->dev = info->dev;	/* [한국어] 이 도메인이 속한 장치. 장치별 도메인에서만 의미가 있다 */
		if (info->flags & MSI_FLAG_PARENT_PM_DEV)	/* [한국어] 전원 관리 대상을 부모에게서 물려받는가 */
			domain->pm_dev = parent->pm_dev;	/* [한국어] 장치별 MSI 도메인은 자기 하드웨어가 없고 부모 컨트롤러에 얹혀 있어, 그 컨트롤러의 전원을 붙잡아야 한다 */
	}

	return domain;	/* [한국어] 실패하면 NULL 이 그대로 나간다 */
}

/**
 * msi_create_irq_domain - Create an MSI interrupt domain
 * @fwnode:	Optional fwnode of the interrupt controller
 * @info:	MSI domain info
 * @parent:	Parent irq domain
 *
 * Return: pointer to the created &struct irq_domain or %NULL on failure
 */
/*
 * [한국어]
 * msi_create_irq_domain - 전역 MSI 도메인을 만든다
 *
 * @fwnode: 컨트롤러의 펌웨어 노드 (선택적)
 * @info:   도메인 설정
 * @parent: 부모 도메인
 * @return: 만든 도메인, 실패 시 NULL
 *
 * 위 공통 구현을 플래그 없이 부르는 껍데기다. 여기서 만들어지는
 * 도메인은 여러 장치가 공유하는 전역 도메인이다.
 *
 * 장치별 도메인과의 차이: 전역 도메인은 모든 장치에 같은 능력을
 * 제공한다. 장치마다 다른 제약(MSI-X 테이블 크기, 마스크 가능 여부)을
 * 표현할 수 없어, 요즘은 msi_create_device_irq_domain() 쪽으로
 * 옮겨 가고 있다.
 *
 * 그래도 남아 있는 이유: 아키텍처 수준의 MSI 도메인 — x86 의
 * 벡터 도메인 위에 얹히는 PCI MSI 도메인 같은 — 은 여전히 이 방식이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 대개 부팅 중 컨트롤러 초기화.
 *
 * 호출 체인:
 *   아키텍처 MSI 초기화 / IOMMU 드라이버 → [이 함수] →
 *   __msi_create_irq_domain()
 */
struct irq_domain *msi_create_irq_domain(struct fwnode_handle *fwnode,
					 struct msi_domain_info *info,
					 struct irq_domain *parent)
{
	return __msi_create_irq_domain(fwnode, info, 0, parent);	/* [한국어] 추가 플래그 없이. 장치별 도메인이 아니므로 IRQ_DOMAIN_FLAG_MSI_DEVICE 를 붙이지 않는다 */
}

/**
 * msi_create_parent_irq_domain - Create an MSI-parent interrupt domain
 * @info:		MSI irqdomain creation info
 * @msi_parent_ops:	MSI parent callbacks and configuration
 *
 * Return: pointer to the created &struct irq_domain or %NULL on failure
 */
/*
 * [한국어]
 * msi_create_parent_irq_domain - MSI 부모 도메인을 만든다
 *
 * @info:           도메인 생성 정보 — 이 함수가 내용을 수정한다
 * @msi_parent_ops: 부모 역할을 하는 데 필요한 콜백과 설정
 * @return:         만든 도메인, 실패 시 NULL
 *
 * "MSI 부모" 도메인이란: 장치별 MSI 도메인의 부모가 될 수 있는
 * 도메인이다. GIC ITS 나 IOMMU 인터럽트 리매핑 하드웨어가 그렇다.
 *
 * 이 도메인은 직접 인터럽트를 배정받는 데 쓰이지 않는다. 장치가
 * MSI 를 쓰려 하면 그 위에 장치 전용 도메인이 만들어지고, 그때
 * msi_parent_ops->init_dev_msi_info 가 불려 "내가 지원하는 능력은
 * 이만큼" 이라고 자식의 설정을 깎거나 늘린다.
 *
 * hwirq_max 와 size 를 같게 맞추는 것에 주목: 두 필드가 뜻이 겹치는데
 * 호출자가 둘 중 하나만 채우는 경우가 있다. 큰 쪽으로 통일해 어느
 * 쪽을 채웠든 동작하게 한다.
 *
 * bus_token 을 parent_ops 에서 가져오는 이유: 자식 도메인을 찾을 때
 * "어떤 종류의 MSI 부모인가" 로 검색하는데, 그 토큰은 부모 콜백
 * 묶음이 정한다. 도메인마다 따로 지정하면 어긋날 수 있다.
 *
 * msi_parent_ops 를 생성 뒤에 대입하는 이유: irq_domain_instantiate 가
 * 그 필드를 모르기 때문이다. 다만 그 사이에 다른 CPU 가 이 도메인을
 * 볼 수 있는데, 부모 플래그는 이미 세워져 있으므로 자식 생성 시도가
 * 이 필드가 NULL 인 것을 보고 실패할 수 있다. 실제로는 도메인 생성이
 * 초기화 시점의 단일 스레드 작업이라 문제가 되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 컨트롤러 초기화.
 *
 * 호출 체인:
 *   GIC ITS / IOMMU 리매핑 드라이버 초기화 → [이 함수] →
 *   irq_domain_instantiate()
 */
struct irq_domain *msi_create_parent_irq_domain(struct irq_domain_info *info,
						const struct msi_parent_ops *msi_parent_ops)
{
	struct irq_domain *d;	/* [한국어] 만든 도메인 */

	info->hwirq_max		= max(info->hwirq_max, info->size);	/* [한국어] 두 필드가 뜻이 겹치는데 호출자가 하나만 채우는 경우가 있다. 큰 쪽으로 통일한다 */
	info->size		= info->hwirq_max;	/* [한국어] 나머지 하나도 같은 값으로 */
	info->domain_flags	|= IRQ_DOMAIN_FLAG_MSI_PARENT;	/* [한국어] "나는 장치별 MSI 도메인의 부모가 될 수 있다" 는 표식. msi_get_device_domain 이 이 플래그를 보고 직접 사용을 막는다 */
	info->bus_token		= msi_parent_ops->bus_select_token;	/* [한국어] 부모 콜백 묶음이 정한 종류 표식을 쓴다. 도메인마다 따로 지정하면 자식 검색과 어긋날 수 있다 */

	d = irq_domain_instantiate(info);	/* [한국어] 실제 생성 */
	if (IS_ERR(d))	/* [한국어] 오류 포인터인가 */
		return NULL;	/* [한국어] 호출자들이 NULL 검사만 하도록 눌러 준다 */

	d->msi_parent_ops = msi_parent_ops;	/* [한국어] 생성 뒤에 대입한다. irq_domain_instantiate 가 이 필드를 모르기 때문이다. 도메인 생성이 초기화 시점의 단일 스레드 작업이라 그 사이의 창은 문제가 되지 않는다 */
	return d;	/* [한국어] 이제 이 도메인 위에 장치별 도메인을 만들 수 있다 */
}
EXPORT_SYMBOL_GPL(msi_create_parent_irq_domain);	/* [한국어] GIC ITS 등이 모듈로 빌드될 수 있다 */

/**
 * msi_parent_init_dev_msi_info - Delegate initialization of device MSI info down
 *				  in the domain hierarchy
 * @dev:		The device for which the domain should be created
 * @domain:		The domain in the hierarchy this op is being called on
 * @msi_parent_domain:	The IRQ_DOMAIN_FLAG_MSI_PARENT domain for the child to
 *			be created
 * @msi_child_info:	The MSI domain info of the IRQ_DOMAIN_FLAG_MSI_DEVICE
 *			domain to be created
 *
 * Return: true on success, false otherwise
 *
 * This is the most complex problem of per device MSI domains and the
 * underlying interrupt domain hierarchy:
 *
 * The device domain to be initialized requests the broadest feature set
 * possible and the underlying domain hierarchy puts restrictions on it.
 *
 * That's trivial for a simple parent->child relationship, but it gets
 * interesting with an intermediate domain: root->parent->child.  The
 * intermediate 'parent' can expand the capabilities which the 'root'
 * domain is providing. So that creates a classic hen and egg problem:
 * Which entity is doing the restrictions/expansions?
 *
 * One solution is to let the root domain handle the initialization that's
 * why there is the @domain and the @msi_parent_domain pointer.
 */
/*
 * [한국어]
 * msi_parent_init_dev_msi_info - 능력 협상을 한 층 아래로 넘긴다
 *
 * @dev:               장치별 도메인을 만들려는 장치
 * @domain:            지금 이 콜백이 불린 층
 * @msi_parent_domain: 자식이 붙을 MSI 부모 도메인
 * @msi_child_info:    만들어질 자식 도메인의 설정 — 여기서 수정된다
 * @return:            true 협상 성공, false 실패
 *
 * 원본 주석이 이 파일에서 가장 어려운 문제를 설명한다. 요약하면
 * 이렇다.
 *
 * 장치별 MSI 도메인은 "나는 이런 능력을 원한다" 며 최대한 넓게
 * 요청한다. 그 아래 계층이 실제로 무엇을 지원하는지에 따라 그 요청이
 * 깎여야 한다. 예를 들어 인터럽트 리매핑이 없는 시스템에서는 MSI
 * 격리 기능을 제공할 수 없다.
 *
 * 문제는 계층이 세 겹 이상일 때다. root → parent → child 에서 중간의
 * parent 가 root 에 없는 능력을 *더할* 수도 있다. 리매핑 하드웨어가
 * 그렇다 — 아래 벡터 도메인에 없는 격리 기능을 추가한다. 그러면
 * "누가 최종 판단을 하는가" 라는 순환이 생긴다.
 *
 * 해결책은 판단을 가장 안쪽(root)에 맡기는 것이다. 각 층은 이 함수로
 * 요청을 한 층 아래로 넘기고, 자기 능력은 돌아오는 길에 반영한다.
 * @domain 과 @msi_parent_domain 을 따로 받는 이유가 그것이다 — 전자는
 * "지금 어디까지 내려왔는가", 후자는 "자식이 최종적으로 붙을 곳" 이다.
 *
 * 이 함수 자체는 위임만 한다. 실제 능력 조정은 각 층의
 * init_dev_msi_info 구현이 자기 몫을 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 장치별 도메인 생성 경로.
 *
 * 호출 체인:
 *   msi_create_device_irq_domain() → 최상위 부모의 init_dev_msi_info →
 *   [이 함수] → 그 아래 층의 init_dev_msi_info → ... (재귀)
 */
bool msi_parent_init_dev_msi_info(struct device *dev, struct irq_domain *domain,
				  struct irq_domain *msi_parent_domain,
				  struct msi_domain_info *msi_child_info)
{
	struct irq_domain *parent = domain->parent;	/* [한국어] 한 층 아래. 판단을 가장 안쪽까지 미루는 것이 이 설계의 핵심이다 */

	if (WARN_ON_ONCE(!parent || !parent->msi_parent_ops ||	/* [한국어] 더 내려갈 층이 없거나 그 층이 부모 역할을 못 하는가 */
			 !parent->msi_parent_ops->init_dev_msi_info))	/* [한국어] 협상 콜백을 제공하지 않는가 */
		return false;	/* [한국어] 계층 구성이 잘못됐다. 이 위치에 도달했다는 것은 위층이 부모라고 선언했다는 뜻인데 아래가 받쳐 주지 못한다 */

	return parent->msi_parent_ops->init_dev_msi_info(dev, parent, msi_parent_domain,	/* [한국어] 한 층 아래로 넘긴다. msi_parent_domain 은 바뀌지 않는다 — 자식이 최종적으로 붙을 곳은 고정이다 */
							 msi_child_info);
}

/**
 * msi_create_device_irq_domain - Create a device MSI interrupt domain
 * @dev:		Pointer to the device
 * @domid:		Domain id
 * @template:		MSI domain info bundle used as template
 * @hwsize:		Maximum number of MSI table entries (0 if unknown or unlimited)
 * @domain_data:	Optional pointer to domain specific data which is set in
 *			msi_domain_info::data
 * @chip_data:		Optional pointer to chip specific data which is set in
 *			msi_domain_info::chip_data
 *
 * Return: True on success, false otherwise
 *
 * There is no firmware node required for this interface because the per
 * device domains are software constructs which are actually closer to the
 * hardware reality than any firmware can describe them.
 *
 * The domain name and the irq chip name for a MSI device domain are
 * composed by: "$(PREFIX)$(CHIPNAME)-$(DEVNAME)"
 *
 * $PREFIX:   Optional prefix provided by the underlying MSI parent domain
 *	      via msi_parent_ops::prefix. If that pointer is NULL the prefix
 *	      is empty.
 * $CHIPNAME: The name of the irq_chip in @template
 * $DEVNAME:  The name of the device
 *
 * This results in understandable chip names and hardware interrupt numbers
 * in e.g. /proc/interrupts
 *
 * PCI-MSI-0000:00:1c.0     0-edge  Parent domain has no prefix
 * IR-PCI-MSI-0000:00:1c.4  0-edge  Same with interrupt remapping prefix 'IR-'
 *
 * IR-PCI-MSIX-0000:3d:00.0 0-edge  Hardware interrupt numbers reflect
 * IR-PCI-MSIX-0000:3d:00.0 1-edge  the real MSI-X index on that device
 * IR-PCI-MSIX-0000:3d:00.0 2-edge
 *
 * On IMS domains the hardware interrupt number is either a table entry
 * index or a purely software managed index but it is guaranteed to be
 * unique.
 *
 * The domain pointer is stored in @dev::msi::data::__irqdomains[]. All
 * subsequent operations on the domain depend on the domain id.
 *
 * The domain is automatically freed when the device is removed via devres
 * in the context of @dev::msi::data freeing, but it can also be
 * independently removed via @msi_remove_device_irq_domain().
 */
/*
 * [한국어]
 * msi_create_device_irq_domain - 장치 전용 MSI 도메인을 만든다
 *
 * @dev:         대상 장치
 * @domid:       도메인 ID (기본 도메인 또는 보조 도메인)
 * @template:    도메인 설정·칩·콜백을 한 덩어리로 담은 견본
 * @hwsize:      MSI 테이블 항목 수 (모르거나 무제한이면 0)
 * @domain_data: 도메인 사설 데이터 (선택적)
 * @chip_data:   칩 사설 데이터 (선택적)
 * @return:      true 성공, false 실패
 *
 * 요즘 MSI 설계의 중심이 되는 함수다. 예전에는 버스마다 도메인 하나를
 * 공유했는데, 그러면 장치마다 다른 제약(테이블 크기, 마스크 가능
 * 여부)을 표현할 수 없었다. 이제는 장치마다 자기 도메인을 만든다.
 *
 * 원본 주석이 이름 짓기 규칙을 자세히 설명한다. "IR-PCI-MSIX-0000:
 * 3d:00.0" 처럼 접두사·칩 이름·장치 이름을 이어 붙인다. 그 결과
 * /proc/interrupts 만 봐도 어느 장치의 몇 번 MSI-X 인지, 인터럽트
 * 리매핑을 거치는지가 드러난다.
 *
 * 견본(template)을 복사하는 이유: 도메인마다 자기 사본이 필요하다.
 * 위 msi_domain_update_dom_ops() 가 콜백을 채워 넣으며 구조체를
 * 수정하고, 칩 이름도 장치마다 다르게 만들어 넣는다. 견본을 공유하면
 * 두 번째 장치가 첫 번째의 설정을 덮는다.
 *
 * __free(kfree) 와 retain_and_null_ptr 의 조합에 주목: 실패 경로가
 * 여덟 곳이 넘는데, 각각에서 bundle 과 fwnode 를 해제하면 실수가
 * 생긴다. 자동 정리 속성으로 기본을 "해제" 로 두고, 성공했을 때만
 * 그 정리를 취소한다.
 *
 * 펌웨어 노드가 두 갈래인 이유: wire-to-MSI 도메인은 장치 트리의
 * 배선 정보와 매칭돼야 하므로 장치의 진짜 fwnode 를 써야 한다.
 * 나머지는 이름만 있는 가짜 노드를 만들어 쓴다 — 조회되지 않고
 * 항상 장치 문맥에서 다뤄지기 때문이다.
 *
 * prepare 실패 시 슬롯을 되돌리는 순서가 중요하다. 슬롯을 먼저 NULL 로
 * 만들고 도메인을 제거해야, 그 사이에 누가 슬롯을 통해 제거된
 * 도메인에 접근하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스와 GFP_KERNEL 할당이 있다.
 *
 * 호출 체인:
 *   pci_setup_msi_device_domain() / IMS 드라이버 → [이 함수] →
 *   pops->init_dev_msi_info() → __msi_create_irq_domain()
 */
bool msi_create_device_irq_domain(struct device *dev, unsigned int domid,
				  const struct msi_domain_template *template,
				  unsigned int hwsize, void *domain_data,
				  void *chip_data)
{
	struct irq_domain *domain, *parent = dev->msi.domain;	/* [한국어] 만들 도메인과 그 부모. 부모는 장치에 미리 지정돼 있다 */
	const struct msi_parent_ops *pops;	/* [한국어] 부모의 협상 콜백 묶음 */
	struct fwnode_handle *fwnode;	/* [한국어] 도메인에 붙일 펌웨어 노드 */

	if (!irq_domain_is_msi_parent(parent))	/* [한국어] 부모가 장치별 도메인을 받아 줄 수 있는가 */
		return false;	/* [한국어] 옛 방식의 전역 MSI 도메인이라면 그 위에 장치 도메인을 얹을 수 없다 */

	if (domid >= MSI_MAX_DEVICE_IRQDOMAINS)	/* [한국어] 도메인 ID 가 범위 안인가 */
		return false;

	struct msi_domain_template *bundle __free(kfree) =	/* [한국어] 자동 정리 속성. 실패 경로가 여덟 곳이 넘어 각각에서 해제하면 실수가 생긴다 */
		kmemdup(template, sizeof(*bundle), GFP_KERNEL);	/* [한국어] 견본을 복사한다. 아래에서 콜백을 채우고 칩 이름을 장치별로 만들어 넣으므로 공유하면 두 번째 장치가 첫 번째를 덮는다 */
	if (!bundle)	/* [한국어] 메모리 부족 */
		return false;

	bundle->info.hwsize = hwsize;	/* [한국어] MSI 테이블 크기. 0 이면 아래에서 최대값이 된다 */
	bundle->info.chip = &bundle->chip;	/* [한국어] 복사본 안의 칩을 가리키게 한다. 견본의 것을 가리키면 이름 수정이 공유된다 */
	bundle->info.ops = &bundle->ops;	/* [한국어] 마찬가지로 복사본 안의 콜백 묶음 */
	bundle->info.data = domain_data;	/* [한국어] 도메인 드라이버의 사설 데이터 */
	bundle->info.chip_data = chip_data;	/* [한국어] 칩 콜백이 쓸 사설 데이터 */
	bundle->info.alloc_data = &bundle->alloc_info;	/* [한국어] 할당 정보의 견본. 이것이 있으면 할당 때마다 prepare 를 부르지 않고 복사만 한다 */
	bundle->info.dev = dev;	/* [한국어] 이 도메인이 속한 장치 */

	pops = parent->msi_parent_ops;	/* [한국어] 부모의 협상 콜백과 이름 접두사 */
	snprintf(bundle->name, sizeof(bundle->name), "%s%s-%s",	/* [한국어] "IR-PCI-MSIX-0000:3d:00.0" 같은 이름을 만든다. /proc/interrupts 만 봐도 장치와 리매핑 여부가 드러난다 */
		 pops->prefix ? : "", bundle->chip.name, dev_name(dev));	/* [한국어] 접두사는 부모가 정한다 — 인터럽트 리매핑이 있으면 "IR-" 이 붙는다 */
	bundle->chip.name = bundle->name;	/* [한국어] 칩 이름을 방금 만든 문자열로 바꾼다. 견본을 복사한 덕분에 다른 장치에 영향이 없다 */

	/*
	 * Using the device firmware node is required for wire to MSI
	 * device domains so that the existing firmware results in a domain
	 * match.
	 * All other device domains like PCI/MSI use the named firmware
	 * node as they are not guaranteed to have a fwnode. They are never
	 * looked up and always handled in the context of the device.
	 */
	struct fwnode_handle *fwnode_alloced __free(irq_domain_free_fwnode) = NULL;	/* [한국어] (위 영어 주석) 가짜 노드를 만든 경우에만 채워지고, 실패 시 자동 해제된다 */

	if (!(bundle->info.flags & MSI_FLAG_USE_DEV_FWNODE))	/* [한국어] 장치의 진짜 fwnode 를 써야 하는가 */
		fwnode = fwnode_alloced = irq_domain_alloc_named_fwnode(bundle->name);	/* [한국어] 아니면 이름만 있는 가짜 노드를 만든다. 조회되지 않고 항상 장치 문맥에서 다뤄지므로 충분하다 */
	else	/* [한국어] wire-to-MSI 도메인 */
		fwnode = dev->fwnode;	/* [한국어] 장치 트리의 배선 정보와 매칭돼야 하므로 진짜 노드를 쓴다 */

	if (!fwnode)	/* [한국어] 둘 다 없는가 */
		return false;	/* [한국어] bundle 과 fwnode_alloced 는 자동 정리된다 */

	if (msi_setup_device_data(dev))	/* [한국어] 장치에 MSI 저장소가 없으면 만든다. 여러 번 불러도 안전하다 */
		return false;

	guard(msi_descs_lock)(dev);	/* [한국어] 아래 슬롯 조작을 직렬화한다 */
	if (WARN_ON_ONCE(msi_get_device_domain(dev, domid)))	/* [한국어] 그 슬롯에 이미 도메인이 있는가 */
		return false;	/* [한국어] 덮어쓰면 먼저 것이 새고 그 도메인의 인터럽트들이 고아가 된다 */

	if (!pops->init_dev_msi_info(dev, parent, parent, &bundle->info))	/* [한국어] 능력 협상. 계층을 따라 내려가며 이 자식 도메인이 실제로 가질 수 있는 능력으로 설정을 깎거나 늘린다 */
		return false;	/* [한국어] 이 계층에서 지원할 수 없는 도메인이다 */

	domain = __msi_create_irq_domain(fwnode, &bundle->info, IRQ_DOMAIN_FLAG_MSI_DEVICE, parent);	/* [한국어] 협상된 설정으로 만든다. DEVICE 플래그가 이것을 장치별 도메인으로 표시한다 */
	if (!domain)	/* [한국어] 생성 실패 */
		return false;

	dev->msi.data->__domains[domid].domain = domain;	/* [한국어] 슬롯에 등록한다. 이 줄부터 msi_get_device_domain 이 이 도메인을 돌려준다 */

	if (msi_domain_prepare_irqs(domain, dev, hwsize, &bundle->alloc_info)) {	/* [한국어] 할당 정보 견본을 미리 채워 둔다. 실패하면 이 도메인은 쓸 수 없다 */
		dev->msi.data->__domains[domid].domain = NULL;	/* [한국어] 슬롯을 먼저 비운다. 도메인 제거보다 먼저 해야 그 사이에 누가 제거된 도메인에 접근하지 않는다 */
		irq_domain_remove(domain);	/* [한국어] 도메인 제거 */
		return false;	/* [한국어] bundle 과 fwnode 는 자동 정리된다 */
	}

	/* @bundle and @fwnode_alloced are now in use. Prevent cleanup */
	retain_and_null_ptr(bundle);	/* [한국어] (위 영어 주석) 성공했으므로 자동 해제를 취소한다. 도메인이 이 메모리를 계속 쓴다 — 해제는 msi_remove_device_irq_domain 이 한다 */
	retain_and_null_ptr(fwnode_alloced);	/* [한국어] 마찬가지로 fwnode 도 도메인이 계속 참조한다 */
	return true;	/* [한국어] 이제 이 장치에 MSI 인터럽트를 할당할 수 있다 */
}

/**
 * msi_remove_device_irq_domain - Free a device MSI interrupt domain
 * @dev:	Pointer to the device
 * @domid:	Domain id
 */
/*
 * [한국어]
 * msi_remove_device_irq_domain - 장치별 MSI 도메인을 없앤다
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @return: 없음
 *
 * msi_create_device_irq_domain() 의 반대다. 두 경로에서 불린다 —
 * 드라이버가 명시적으로 부르거나, 장치가 사라질 때 devres 콜백이
 * 부른다. 두 번 불려도 안전하도록 슬롯이 비어 있으면 조용히 넘어간다.
 *
 * 순서가 중요하다. 슬롯을 먼저 비우고 나서 실제 제거를 한다. 그
 * 사이에 다른 CPU 가 슬롯을 통해 제거 중인 도메인에 접근하지 못한다.
 *
 * 마지막 kfree 가 이 함수의 미묘한 지점이다. 해제하는 것은
 * msi_domain_info 가 아니라 그것을 품고 있는 msi_domain_template 이다.
 * 생성 때 템플릿을 통째로 복사했고 info 는 그 안의 필드이므로,
 * container_of 로 바깥 구조체를 되찾아 해제해야 한다. info 만
 * kfree 하면 주소가 어긋나 힙이 망가진다.
 *
 * fwnode 를 조건부로 챙기는 이유: wire-to-MSI 도메인은 장치의
 * fwnode 를 빌려 쓰므로 해제하면 안 된다. 다만 위 조건에서 이미
 * is_msi_device 를 확인했으므로 이 두 번째 검사는 항상 참이다 —
 * 방어적으로 남아 있는 코드다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_device_data_release() / 드라이버 remove → [이 함수] →
 *   ops->msi_teardown() → irq_domain_remove()
 */
void msi_remove_device_irq_domain(struct device *dev, unsigned int domid)
{
	struct fwnode_handle *fwnode = NULL;	/* [한국어] 해제할 가짜 fwnode. 빌려 쓴 것이면 NULL 로 남는다 */
	struct msi_domain_info *info;	/* [한국어] 도메인 설정 */
	struct irq_domain *domain;	/* [한국어] 없앨 도메인 */

	guard(msi_descs_lock)(dev);	/* [한국어] 슬롯 조작을 직렬화한다 */
	domain = msi_get_device_domain(dev, domid);	/* [한국어] 슬롯에서 꺼낸다 */
	if (!domain || !irq_domain_is_msi_device(domain))	/* [한국어] 없거나 장치별 도메인이 아닌가 */
		return;	/* [한국어] 두 번 불려도 안전하게 만드는 방어다. 전역 도메인은 여기서 없애면 안 된다 */

	dev->msi.data->__domains[domid].domain = NULL;	/* [한국어] 슬롯을 먼저 비운다. 실제 제거보다 먼저 해야 그 사이에 누가 접근하지 않는다 */
	info = domain->host_data;	/* [한국어] 도메인 설정. 아래 teardown 과 kfree 에 쓴다 */

	info->ops->msi_teardown(domain, info->alloc_data);	/* [한국어] 도메인 드라이버가 prepare 에서 잡은 것을 푼다. 기본 구현은 빈 함수라 NULL 검사가 필요 없다 */

	if (irq_domain_is_msi_device(domain))	/* [한국어] 위에서 이미 확인했으므로 항상 참이다. 방어적으로 남아 있는 코드 */
		fwnode = domain->fwnode;	/* [한국어] 생성 때 만든 가짜 노드. wire-to-MSI 라면 장치의 것을 빌린 것이지만 그 경우도 여기 온다 */
	irq_domain_remove(domain);	/* [한국어] 도메인 제거. 이 뒤로는 도메인 포인터가 무효다 */
	irq_domain_free_fwnode(fwnode);	/* [한국어] 가짜 노드 해제. 빌린 노드에는 이 함수가 아무것도 하지 않도록 되어 있다 */
	kfree(container_of(info, struct msi_domain_template, info));	/* [한국어] 해제하는 것은 info 가 아니라 그것을 품은 템플릿 전체다. 생성 때 템플릿을 통째로 복사했기 때문이다. info 만 kfree 하면 주소가 어긋나 힙이 망가진다 */
}

/**
 * msi_match_device_irq_domain - Match a device irq domain against a bus token
 * @dev:	Pointer to the device
 * @domid:	Domain id
 * @bus_token:	Bus token to match against the domain bus token
 *
 * Return: True if device domain exists and bus tokens match.
 */
/*
 * [한국어]
 * msi_match_device_irq_domain - 장치 도메인이 특정 종류인지 확인한다
 *
 * @dev:       대상 장치
 * @domid:     도메인 ID
 * @bus_token: 비교할 종류 표식
 * @return:    true 그 종류의 장치별 도메인이 존재함, false 아님
 *
 * PCI 계층이 "이 장치에 MSI-X 용 장치 도메인이 준비돼 있는가" 를
 * 묻는 통로다. 같은 슬롯에 MSI 용과 MSI-X 용이 번갈아 들어올 수
 * 있으므로, 존재만으로는 부족하고 종류까지 봐야 한다.
 *
 * 왜 종류가 다른가: PCI MSI 와 MSI-X 는 마스크 방식과 테이블 구조가
 * 달라 도메인 설정이 다르다. 장치가 MSI 에서 MSI-X 로 전환하면
 * 기존 도메인을 없애고 새로 만들어야 한다. 이 함수가 그 판단의
 * 근거다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/pci/msi/irqdomain.c → [이 함수] → msi_get_device_domain()
 */
bool msi_match_device_irq_domain(struct device *dev, unsigned int domid,
				 enum irq_domain_bus_token bus_token)
{
	struct msi_domain_info *info;	/* [한국어] 도메인 설정 */
	struct irq_domain *domain;	/* [한국어] 대상 도메인 */

	guard(msi_descs_lock)(dev);	/* [한국어] 조회 동안 도메인이 사라지지 않게 한다 */
	domain = msi_get_device_domain(dev, domid);	/* [한국어] 슬롯 조회 */
	if (domain && irq_domain_is_msi_device(domain)) {	/* [한국어] 있고 장치별 도메인인가. 전역 도메인은 종류 비교의 대상이 아니다 */
		info = domain->host_data;	/* [한국어] 설정에서 종류 표식을 꺼낸다 */
		return info->bus_token == bus_token;	/* [한국어] PCI MSI 와 MSI-X 는 설정이 달라 도메인도 다르다. 전환하려면 없애고 새로 만들어야 한다 */
	}
	return false;	/* [한국어] 없거나 전역 도메인이다 */
}

/*
 * [한국어]
 * msi_domain_prepare_irqs - 도메인의 할당 준비 콜백을 부른다
 *
 * @domain: 대상 도메인
 * @dev:    대상 장치
 * @nvec:   할당할 벡터 수
 * @arg:    채울 할당 정보
 * @return: 0 성공, 음수 오류
 *
 * 세 줄짜리 위임 함수다. host_data 에서 설정을 꺼내 콜백을 부르는
 * 것이 전부다.
 *
 * 그래도 따로 두는 이유: 이 위임을 부르는 곳이 세 군데이고,
 * host_data 를 msi_domain_info 로 형변환하는 지식을 한곳에 모아
 * 두는 편이 낫다.
 *
 * NULL 검사가 없는 것에 주목: msi_prepare 는 필수 콜백이고,
 * msi_domain_update_dom_ops() 가 기본 구현으로 채워 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   msi_create_device_irq_domain() / populate_alloc_info() → [이 함수]
 */
static int msi_domain_prepare_irqs(struct irq_domain *domain, struct device *dev,
				   int nvec, msi_alloc_info_t *arg)
{
	struct msi_domain_info *info = domain->host_data;	/* [한국어] MSI 도메인은 host_data 에 설정을 둔다 */
	struct msi_domain_ops *ops = info->ops;	/* [한국어] 콜백 묶음 */

	return ops->msi_prepare(domain, dev, nvec, arg);	/* [한국어] NULL 검사가 없는 것은 update_dom_ops 가 기본 구현으로 채워 주기 때문이다 */
}

/*
 * Carefully check whether the device can use reservation mode. If
 * reservation mode is enabled then the early activation will assign a
 * dummy vector to the device. If the PCI/MSI device does not support
 * masking of the entry then this can result in spurious interrupts when
 * the device driver is not absolutely careful. But even then a malfunction
 * of the hardware could result in a spurious interrupt on the dummy vector
 * and render the device unusable. If the entry can be masked then the core
 * logic will prevent the spurious interrupt and reservation mode can be
 * used. For now reservation mode is restricted to PCI/MSI.
 */
/*
 * [한국어]
 * msi_check_reservation_mode - 예약 모드를 쓸 수 있는 장치인지 판정한다
 *
 * @domain: 대상 도메인
 * @info:   도메인 설정
 * @dev:    대상 장치
 * @return: true 예약 모드 가능, false 불가
 *
 * 예약 모드(reservation mode)란: MSI 를 설정할 때는 더미 벡터를
 * 배정해 두고, 드라이버가 실제로 request_irq() 할 때 진짜 벡터를
 * 주는 방식이다.
 *
 * 왜 그런 것이 필요한가: CPU 벡터는 희소한 자원이다. x86 은 CPU 당
 * 200 개 남짓이고, MSI-X 를 수천 개 쓰는 장치가 여럿 꽂히면 금방
 * 바닥난다. 그런데 장치가 선언한 벡터를 드라이버가 다 쓰는 경우는
 * 드물다. 실제로 요청할 때까지 진짜 벡터를 아끼면 훨씬 많은 장치를
 * 지원할 수 있다.
 *
 * 위험이 무엇인가: 더미 벡터가 배정된 동안 장치가 인터럽트를 보내면
 * 그것은 아무도 기다리지 않는 벡터로 간다. 원본 주석이 설명하듯,
 * 드라이버가 부주의하거나 하드웨어가 오동작하면 실제로 그런 일이
 * 생긴다.
 *
 * 그래서 마스크 가능 여부가 조건이다. 마스크할 수 있으면 코어가
 * 더미 벡터 구간 동안 인터럽트를 막아 위험을 없앤다. MSI-X 는 항상
 * 마스크할 수 있고, MSI 는 장치에 따라 다르다.
 *
 * 첫 서술자만 보는 것에 주목: 원본 주석대로 그것으로 충분하다.
 * 한 장치의 모든 MSI 항목은 같은 마스크 능력을 갖는다.
 *
 * PCI 계열 버스 토큰만 허용하는 것은 현재의 제한이다 — 원본 주석의
 * "for now" 가 그것을 말한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() → [이 함수]
 */
static bool msi_check_reservation_mode(struct irq_domain *domain,
				       struct msi_domain_info *info,
				       struct device *dev)
{
	struct msi_desc *desc;	/* [한국어] 마스크 능력을 볼 첫 서술자 */

	switch(domain->bus_token) {	/* [한국어] 예약 모드는 현재 PCI 계열에만 적용된다 */
	case DOMAIN_BUS_PCI_MSI:	/* [한국어] 전역 PCI MSI 도메인 */
	case DOMAIN_BUS_PCI_DEVICE_MSI:	/* [한국어] 장치별 PCI MSI 도메인 */
	case DOMAIN_BUS_PCI_DEVICE_MSIX:	/* [한국어] 장치별 PCI MSI-X 도메인 */
	case DOMAIN_BUS_VMD_MSI:	/* [한국어] VMD(Volume Management Device) 아래의 PCI 도메인 */
		break;	/* [한국어] 이 넷만 통과한다 */
	default:	/* [한국어] 플랫폼 MSI, IMS 등 */
		return false;	/* [한국어] 원본 주석의 "for now" — 현재의 제한이지 원리적 불가는 아니다 */
	}

	if (!(info->flags & MSI_FLAG_MUST_REACTIVATE))	/* [한국어] 요청 시점에 다시 활성화하는 도메인인가 */
		return false;	/* [한국어] 예약 모드는 더미 벡터를 나중에 진짜로 바꾸는 것이라, 재활성화가 없으면 성립하지 않는다 */

	if (info->flags & MSI_FLAG_NO_MASK)	/* [한국어] 마스크를 아예 지원하지 않는 도메인인가 */
		return false;	/* [한국어] 더미 벡터 구간을 막을 수 없다 */

	/*
	 * Checking the first MSI descriptor is sufficient. MSIX supports
	 * masking and MSI does so when the can_mask attribute is set.
	 */
	desc = msi_first_desc(dev, MSI_DESC_ALL);	/* [한국어] (위 영어 주석) 한 장치의 모든 MSI 항목은 같은 마스크 능력을 가지므로 하나만 보면 된다 */
	return desc->pci.msi_attrib.is_msix || desc->pci.msi_attrib.can_mask;	/* [한국어] MSI-X 는 규격상 항상 마스크 가능하고, MSI 는 장치가 그 기능을 갖췄을 때만이다 */
}

/*
 * [한국어]
 * msi_handle_pci_fail - 할당 실패를 PCI 규칙에 맞게 해석한다
 *
 * @domain:    대상 도메인
 * @desc:      실패한 서술자
 * @allocated: 지금까지 성공한 개수
 * @return:    양수 "더 적은 수로 재시도하라", 0 이상 "이만큼은 됐다",
 *             -ENOSPC 실패
 *
 * PCI MSI 의 특이한 규칙 때문에 있는 함수다. 그 방식은 벡터를 2 의
 * 거듭제곱 개수로만 할당할 수 있다 — 1, 2, 4, 8 개 식이다. 그래서
 * 8 개가 실패하면 4 개로, 4 개가 실패하면 2 개로 줄여 다시 시도하는
 * 것이 정상 절차다.
 *
 * 1 을 돌려주는 것이 그 신호다. 호출자 체인을 거쳐 PCI 계층까지
 * 올라가면, 그쪽이 벡터 수를 절반으로 줄여 다시 부른다.
 *
 * MSI-X 와 플랫폼 MSI 는 그런 제약이 없어 개별 할당이 실패하면
 * 거기까지가 끝이다. 그래서 지금까지 성공한 개수를 돌려준다 —
 * "요청한 만큼은 못 줬지만 이만큼은 됐다" 는 뜻이다. 드라이버가
 * 그 수로 동작할 수 있으면 계속 진행한다.
 *
 * fallthrough 가 붙은 이유: CONFIG_PCI_MSI 가 꺼진 빌드에서는 PCI
 * 버스 토큰이라도 재시도 논리를 쓸 수 없다. default 로 흘러
 * -ENOSPC 가 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() 의 실패 경로 → [이 함수]
 */
static int msi_handle_pci_fail(struct irq_domain *domain, struct msi_desc *desc,
			       int allocated)
{
	switch(domain->bus_token) {	/* [한국어] PCI 계열인가 */
	case DOMAIN_BUS_PCI_MSI:	/* [한국어] 전역 PCI MSI */
	case DOMAIN_BUS_PCI_DEVICE_MSI:	/* [한국어] 장치별 PCI MSI */
	case DOMAIN_BUS_PCI_DEVICE_MSIX:	/* [한국어] 장치별 PCI MSI-X */
	case DOMAIN_BUS_VMD_MSI:	/* [한국어] VMD 아래 PCI */
		if (IS_ENABLED(CONFIG_PCI_MSI))	/* [한국어] PCI MSI 지원이 빌드에 들어 있는가 */
			break;	/* [한국어] 아래 재시도 논리로 */
		fallthrough;	/* [한국어] 지원이 없으면 재시도 논리를 쓸 수 없다. default 로 흘려보낸다 */
	default:	/* [한국어] PCI 가 아니거나 지원이 꺼진 빌드 */
		return -ENOSPC;	/* [한국어] 재시도할 방법이 없다 */
	}

	/* Let a failed PCI multi MSI allocation retry */
	if (desc->nvec_used > 1)	/* [한국어] (위 영어 주석) PCI 멀티 MSI 인가 — 벡터를 2 의 거듭제곱으로만 할당할 수 있다 */
		return 1;	/* [한국어] "절반으로 줄여 다시 시도하라" 는 신호. PCI 계층까지 올라가 그쪽이 재시도한다 */

	/* If there was a successful allocation let the caller know */
	return allocated ? allocated : -ENOSPC;	/* [한국어] (위 영어 주석) MSI-X 는 개별 할당이라 재시도 개념이 없다. "요청한 만큼은 못 줬지만 이만큼은 됐다" 를 개수로 알린다 */
}

#define VIRQ_CAN_RESERVE	0x01	/* [한국어] 이 인터럽트는 예약 모드를 쓸 수 있다. 아래 msi_init_virq 가 이 비트를 보고 더미 벡터로 활성화한 뒤 활성 표시를 지운다 */
#define VIRQ_ACTIVATE		0x02	/* [한국어] 할당 직후에 활성화까지 한다. PCI 계층이 카드에서 MSI 를 켜기 전에 메시지가 들어가 있어야 해서 요구한다 */

/*
 * [한국어]
 * msi_init_virq - 할당된 인터럽트 하나의 활성화 정책을 적용한다
 *
 * @domain: 대상 도메인
 * @virq:   대상 리눅스 인터럽트 번호
 * @vflags: VIRQ_CAN_RESERVE, VIRQ_ACTIVATE 조합
 * @return: 0 성공, 음수 활성화 실패
 *
 * 예약 모드와 이른 활성화라는 두 정책이 얽히는 곳이다.
 *
 * 예약 모드를 못 쓰는 경우, 먼저 그 표시를 지운다. 그러면 아래
 * 활성화가 진짜 벡터를 배정한다.
 *
 * 그 안의 관리형 인터럽트 처리가 미묘하다. 담당 CPU 가 전부
 * 오프라인이면 벡터를 배정할 수 없다. 예약 모드였다면 더미 벡터로
 * 넘어갔겠지만 그럴 수 없으므로, 인터럽트를 꺼진 상태로 두고 CPU 가
 * 올라오기를 기다린다. 원본 주석이 짚듯 x86 은 이 문제를 예약 모드로
 * 푸는 유일한 아키텍처라 이 경로를 타지 않는다.
 *
 * 마지막의 활성 표시 지우기가 예약 모드의 핵심이다. 활성화는 했지만
 * 그것이 더미 벡터이므로, "아직 활성화되지 않았다" 고 표시해 둔다.
 * 나중에 request_irq() 가 그 표시를 보고 다시 활성화하면서 진짜
 * 벡터를 받는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 할당 경로.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() → [이 함수] → irq_domain_activate_irq()
 */
static int msi_init_virq(struct irq_domain *domain, int virq, unsigned int vflags)
{
	struct irq_data *irqd = irq_domain_get_irq_data(domain, virq);	/* [한국어] 이 층의 irq_data */
	int ret;	/* [한국어] 활성화 결과 */

	if (!(vflags & VIRQ_CAN_RESERVE)) {	/* [한국어] 예약 모드를 쓸 수 없는 인터럽트인가 */
		irqd_clr_can_reserve(irqd);	/* [한국어] 표시를 지운다. 그러면 아래 활성화가 더미가 아닌 진짜 벡터를 배정한다 */

		/*
		 * If the interrupt is managed but no CPU is available to
		 * service it, shut it down until better times. Note that
		 * we only do this on the !RESERVE path as x86 (the only
		 * architecture using this flag) deals with this in a
		 * different way by using a catch-all vector.
		 */
		if ((vflags & VIRQ_ACTIVATE) &&	/* [한국어] (위 영어 주석) 지금 활성화하려 하는가 */
		    irqd_affinity_is_managed(irqd) &&	/* [한국어] 커널이 친화도를 관리하는 인터럽트인가 */
		    !cpumask_intersects(irq_data_get_affinity_mask(irqd),	/* [한국어] 담당 CPU 중 살아 있는 것이 하나도 없는가 */
					cpu_online_mask)) {
			    irqd_set_managed_shutdown(irqd);	/* [한국어] 꺼진 상태로 둔다. CPU 가 올라오면 핫플러그 경로가 되살린다 */
			    return 0;	/* [한국어] 오류가 아니다. 활성화만 미뤄졌다 */
		    }
	}

	if (!(vflags & VIRQ_ACTIVATE))	/* [한국어] 지금 활성화하지 않는가 */
		return 0;	/* [한국어] 나중에 request_irq 가 할 것이다 */

	ret = irq_domain_activate_irq(irqd, vflags & VIRQ_CAN_RESERVE);	/* [한국어] 활성화한다. 두 번째 인자가 참이면 더미 벡터를, 거짓이면 진짜 벡터를 배정한다 */
	if (ret)	/* [한국어] 벡터 고갈 등 */
		return ret;
	/*
	 * If the interrupt uses reservation mode, clear the activated bit
	 * so request_irq() will assign the final vector.
	 */
	if (vflags & VIRQ_CAN_RESERVE)	/* [한국어] (위 영어 주석) 더미 벡터로 활성화했는가 */
		irqd_clr_activated(irqd);	/* [한국어] "아직 활성화되지 않았다" 고 표시한다. request_irq 가 그것을 보고 다시 활성화하며 진짜 벡터를 받는다 — 이것이 예약 모드의 핵심이다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * populate_alloc_info - 할당 정보를 준비한다 (견본 또는 prepare 호출)
 *
 * @domain: 대상 도메인
 * @dev:    대상 장치
 * @nirqs:  할당할 벡터 수
 * @arg:    채울 할당 정보 (출력)
 * @return: 0 성공, 음수 오류
 *
 * 두 가지 방식이 공존하는 과도기를 보여 주는 함수다.
 *
 * 옛 방식은 할당할 때마다 도메인의 msi_prepare 콜백을 부른다. 그
 * 콜백이 아키텍처 고유 정보를 채운다.
 *
 * 새 방식은 도메인 생성 시점에 한 번 채워 둔 견본(info->alloc_data)을
 * 복사한다. 같은 도메인에서 여러 번 할당해도 준비 결과가 같다면
 * 매번 부를 이유가 없다.
 *
 * 원본 주석이 방향을 말한다 — msi_create_irq_domain() 을 쓰는
 * 사용자가 사라지면 견본 방식만 남고 prepare 호출은 없앨 수 있다.
 * 장치별 도메인은 이미 생성 때 견본을 채운다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   __msi_domain_alloc_irqs() → [이 함수] → msi_domain_prepare_irqs()
 */
static int populate_alloc_info(struct irq_domain *domain, struct device *dev,
			       unsigned int nirqs, msi_alloc_info_t *arg)
{
	struct msi_domain_info *info = domain->host_data;	/* [한국어] 도메인 설정 */

	/*
	 * If the caller has provided a template alloc info, use that. Once
	 * all users of msi_create_irq_domain() have been eliminated, this
	 * should be the only source of allocation information, and the
	 * prepare call below should be finally removed.
	 */
	if (!info->alloc_data)	/* [한국어] (위 영어 주석) 견본이 없는가 — 옛 방식의 전역 도메인이다 */
		return msi_domain_prepare_irqs(domain, dev, nirqs, arg);	/* [한국어] 할당할 때마다 준비 콜백을 부른다 */

	*arg = *info->alloc_data;	/* [한국어] 도메인 생성 때 채워 둔 견본을 복사한다. 준비 결과가 매번 같다면 다시 계산할 이유가 없다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * __msi_domain_alloc_irqs - 서술자들에 실제 인터럽트를 할당한다
 *
 * @dev:    대상 장치
 * @domain: 대상 도메인
 * @ctrl:   대상 범위와 개수
 * @return: 0 성공, 음수 오류, 양수 "더 적은 수로 재시도" (PCI 멀티 MSI)
 *
 * 이 파일의 심장이다. 저장소에 있는 서술자들을 훑으며 각각에 리눅스
 * 인터럽트 번호를 배정하고, 메시지를 조립해 장치에 써 넣는다. 이
 * 함수가 끝나면 그 인터럽트들은 request_irq() 를 받을 준비가 된다.
 *
 * 두 정책 플래그를 먼저 정한다.
 *
 * VIRQ_ACTIVATE 는 PCI 계층이 요구한다. 원본 주석이 이유를 짚는다 —
 * 카드에서 MSI 를 켜기 전에 메시지가 들어가 있어야 한다. 그러지
 * 않으면 카드가 아직 쓰이지 않은 레지스터 값을 붙들어(latch) 엉뚱한
 * 주소로 인터럽트를 보낸다.
 *
 * VIRQ_CAN_RESERVE 는 위 msi_check_reservation_mode() 의 판정이다.
 * 벡터를 아끼되 오탐 위험이 없는 장치에만 쓴다.
 *
 * 루프 안에서 서술자마다 하는 일이 다섯 가지다. 준비 콜백을 부르고,
 * 서술자를 할당 정보에 담고, 실제 인터럽트 번호를 받고, 각
 * 인터럽트에 서술자를 역연결하며 활성화하고, sysfs 항목을 만든다.
 *
 * NOTASSOCIATED 필터가 중요하다. 이미 번호가 배정된 서술자를 다시
 * 처리하면 앞의 번호가 샌다. 부분 실패 후 재시도하는 경로가 있어
 * 이 검사가 실제로 걸린다.
 *
 * 실패 시 되돌리지 않는 것에 주목: 그냥 반환한다. 정리는 호출자인
 * msi_domain_alloc_locked() 가 msi_domain_free_locked() 로 한다.
 * 부분 성공 상태를 그대로 넘기면 그쪽이 전체를 일관되게 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   __msi_domain_alloc_locked() / __msi_domain_alloc_irq_at() →
 *   [이 함수] → __irq_domain_alloc_irqs() → msi_init_virq()
 */
static int __msi_domain_alloc_irqs(struct device *dev, struct irq_domain *domain,
				   struct msi_ctrl *ctrl)
{
	struct xarray *xa = &dev->msi.data->__domains[ctrl->domid].store;	/* [한국어] 서술자 저장소 */
	struct msi_domain_info *info = domain->host_data;	/* [한국어] 도메인 설정 */
	struct msi_domain_ops *ops = info->ops;	/* [한국어] MSI 콜백 묶음 */
	unsigned int vflags = 0, allocated = 0;	/* [한국어] 활성화 정책 플래그와 성공한 서술자 수 */
	msi_alloc_info_t arg = { };	/* [한국어] 아래 층에 전달할 할당 정보. 0 초기화 후 populate 가 채운다 */
	struct msi_desc *desc;	/* [한국어] 순회 중인 서술자 */
	unsigned long idx;	/* [한국어] 순회 인덱스 */
	int i, ret, virq;	/* [한국어] 벡터 순회, 결과, 배정된 첫 번호 */

	ret = populate_alloc_info(domain, dev, ctrl->nirqs, &arg);	/* [한국어] 견본을 복사하거나 준비 콜백을 부른다 */
	if (ret)	/* [한국어] 준비 실패 */
		return ret;

	/*
	 * This flag is set by the PCI layer as we need to activate
	 * the MSI entries before the PCI layer enables MSI in the
	 * card. Otherwise the card latches a random msi message.
	 */
	if (info->flags & MSI_FLAG_ACTIVATE_EARLY)	/* [한국어] (위 영어 주석) 할당 직후 활성화까지 해야 하는가 */
		vflags |= VIRQ_ACTIVATE;	/* [한국어] 카드가 MSI 를 켜기 전에 메시지가 들어가 있어야 한다. 아니면 카드가 아직 안 쓰인 레지스터 값을 붙들어 엉뚱한 주소로 인터럽트를 보낸다 */

	/*
	 * Interrupt can use a reserved vector and will not occupy
	 * a real device vector until the interrupt is requested.
	 */
	if (msi_check_reservation_mode(domain, info, dev))	/* [한국어] (위 영어 주석) 예약 모드를 쓸 수 있는 장치인가 */
		vflags |= VIRQ_CAN_RESERVE;	/* [한국어] 더미 벡터로 활성화해 두고 request_irq 때 진짜를 준다. 희소한 CPU 벡터를 아끼는 최적화다 */

	xa_for_each_range(xa, idx, desc, ctrl->first, ctrl->last) {	/* [한국어] 범위 안의 서술자들을 훑는다 */
		if (!msi_desc_match(desc, MSI_DESC_NOTASSOCIATED))	/* [한국어] 이미 번호가 배정된 것인가 */
			continue;	/* [한국어] 다시 처리하면 앞의 번호가 샌다. 부분 실패 후 재시도 경로에서 실제로 걸린다 */

		/* This should return -ECONFUSED... */
		if (WARN_ON_ONCE(allocated >= ctrl->nirqs))	/* [한국어] (위 영어 주석) 요청한 수보다 많이 할당하려 하는가 — 범위와 개수가 어긋난 버그다 */
			return -EINVAL;	/* [한국어] 주석의 농담대로 정확한 오류 코드가 없다. 호출자가 되돌린다 */

		if (ops->prepare_desc)	/* [한국어] 서술자별 준비 콜백이 있는가. 기본 구현이 없는 선택적 콜백이다 */
			ops->prepare_desc(domain, &arg, desc);	/* [한국어] 서술자 내용에 따라 할당 정보를 조정한다 */

		ops->set_desc(&arg, desc);	/* [한국어] 지금 다루는 서술자를 할당 정보에 담는다. 계층형 alloc 체인이 void 포인터 하나만 아래로 넘기기 때문이다 */

		virq = __irq_domain_alloc_irqs(domain, -1, desc->nvec_used,	/* [한국어] 실제 인터럽트 번호 할당. -1 은 "아무 번호나", nvec_used 는 PCI 멀티 MSI 에서 1 보다 크다 */
					       dev_to_node(dev), &arg, false,	/* [한국어] 장치가 붙은 NUMA 노드에 서술자를 두어 인터럽트 경로의 캐시 미스를 줄인다 */
					       desc->affinity);	/* [한국어] 벡터별 친화도. NULL 이면 코어가 정한다 */
		if (virq < 0)	/* [한국어] 벡터 고갈 등 */
			return msi_handle_pci_fail(domain, desc, allocated);	/* [한국어] PCI 멀티 MSI 라면 "절반으로 줄여 재시도" 신호가, 아니면 지금까지의 성공 개수나 -ENOSPC 가 나온다 */

		for (i = 0; i < desc->nvec_used; i++) {	/* [한국어] 이 서술자가 대표하는 각 벡터에 대해 */
			irq_set_msi_desc_off(virq, i, desc);	/* [한국어] 인터럽트에서 서술자로 가는 역연결. 첫 번째에서만 desc->irq 가 채워진다 */
			irq_debugfs_copy_devname(virq + i, dev);	/* [한국어] debugfs 항목에 장치 이름을 남긴다. 어느 장치의 인터럽트인지 추적하기 위해서다 */
			ret = msi_init_virq(domain, virq + i, vflags);	/* [한국어] 활성화 정책 적용 — 이른 활성화, 예약 모드, 관리형 유예 */
			if (ret)	/* [한국어] 활성화 실패 */
				return ret;	/* [한국어] 호출자가 전체를 되돌린다 */
		}
		if (info->flags & MSI_FLAG_DEV_SYSFS) {	/* [한국어] 코어가 sysfs 항목을 만들어 주는 도메인인가 */
			ret = msi_sysfs_populate_desc(dev, desc);	/* [한국어] msi_irqs 아래 인터럽트 번호 파일들을 만든다 */
			if (ret)	/* [한국어] 실패 */
				return ret;
		}
		allocated++;	/* [한국어] 성공한 서술자 수. 위 초과 검사와 부분 성공 보고에 쓴다 */
	}
	return 0;	/* [한국어] 범위의 모든 서술자에 인터럽트가 배정됐다 */
}

/*
 * [한국어]
 * msi_domain_alloc_simple_msi_descs - 필요하면 단순 서술자들을 먼저 만든다
 *
 * @dev:  대상 장치
 * @info: 도메인 설정
 * @ctrl: 대상 범위
 * @return: 0 성공(또는 할 일 없음), 음수 오류
 *
 * 서술자를 누가 만드는가에 두 방식이 있다.
 *
 * PCI 는 설정 공간을 읽어야 서술자 내용을 채울 수 있으므로 PCI
 * 계층이 직접 만들어 넣는다. 그런 도메인은 이 플래그를 세우지 않고,
 * 이 함수는 조용히 성공을 돌려준다.
 *
 * 플랫폼 MSI 나 IMS 는 서술자에 담을 특별한 정보가 없으므로 코어가
 * 대신 만들어 준다. MSI_FLAG_ALLOC_SIMPLE_MSI_DESCS 가 그 요청이다.
 *
 * 이 두 줄짜리 함수가 따로 있는 이유: 호출부인
 * __msi_domain_alloc_locked() 를 조건문으로 어지럽히지 않으려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   __msi_domain_alloc_locked() → [이 함수] →
 *   msi_domain_add_simple_msi_descs()
 */
static int msi_domain_alloc_simple_msi_descs(struct device *dev,
					     struct msi_domain_info *info,
					     struct msi_ctrl *ctrl)
{
	if (!(info->flags & MSI_FLAG_ALLOC_SIMPLE_MSI_DESCS))	/* [한국어] 코어가 서술자를 만들어 달라는 요청이 있는가 */
		return 0;	/* [한국어] PCI 처럼 호출자가 이미 만들어 두는 방식이다. 할 일이 없다 */

	return msi_domain_add_simple_msi_descs(dev, ctrl);	/* [한국어] 범위 전체에 빈 서술자를 만들어 넣는다 */
}

/*
 * [한국어]
 * __msi_domain_alloc_locked - 할당의 공통 절차 (되돌리기 없음)
 *
 * @dev:  대상 장치
 * @ctrl: 대상 범위와 개수
 * @return: 0 성공, 음수 오류, 양수 재시도 신호
 *
 * 할당 경로의 갈림길이다. 세 단계로 진행한다 — 유효성 검사, 서술자
 * 준비, 실제 할당.
 *
 * 마지막 갈림이 흥미롭다. 도메인이 domain_alloc_irqs 콜백을 제공하면
 * 할당 전체를 그쪽에 맡긴다. 그러지 않으면 코어의
 * __msi_domain_alloc_irqs() 를 쓴다.
 *
 * 왜 그런 통째 위임이 있는가: Xen 이나 아키텍처 고유 구현처럼
 * 인터럽트 할당 방식이 근본적으로 다른 경우다. 그런 구현은 서술자
 * 저장소만 코어와 공유하고 나머지는 자기 방식대로 한다.
 *
 * 그 경로가 ctrl->nirqs 만 넘기고 범위는 넘기지 않는 것에 주목:
 * 통째 위임을 받는 구현은 자기가 저장소 전체를 훑는다.
 *
 * 이름의 두 밑줄이 "되돌리기를 하지 않는 판" 을 뜻한다. 되돌리기는
 * 아래 msi_domain_alloc_locked() 가 감싸서 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_alloc_locked() → [이 함수] → __msi_domain_alloc_irqs()
 */
static int __msi_domain_alloc_locked(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_domain_info *info;	/* [한국어] 도메인 설정 */
	struct msi_domain_ops *ops;	/* [한국어] 콜백 묶음 */
	struct irq_domain *domain;	/* [한국어] 대상 도메인 */
	int ret;	/* [한국어] 결과 */

	if (!msi_ctrl_valid(dev, ctrl))	/* [한국어] 범위와 도메인 ID 검사 */
		return -EINVAL;

	domain = msi_get_device_domain(dev, ctrl->domid);	/* [한국어] 슬롯에서 도메인을 꺼낸다 */
	if (!domain)	/* [한국어] 도메인이 없는가 */
		return -ENODEV;	/* [한국어] MSI 를 설정하지 않은 장치다 */

	info = domain->host_data;	/* [한국어] 도메인 설정 */

	ret = msi_domain_alloc_simple_msi_descs(dev, info, ctrl);	/* [한국어] 코어가 서술자를 만들어야 하는 도메인이면 여기서 만든다 */
	if (ret)	/* [한국어] 서술자 준비 실패 */
		return ret;

	ops = info->ops;	/* [한국어] 콜백 묶음 */
	if (ops->domain_alloc_irqs)	/* [한국어] 할당 전체를 자기가 하겠다는 도메인인가 — Xen 이나 아키텍처 고유 구현이다 */
		return ops->domain_alloc_irqs(domain, dev, ctrl->nirqs);	/* [한국어] 범위를 넘기지 않는 것에 주목. 통째 위임을 받는 구현은 자기가 저장소 전체를 훑는다 */

	return __msi_domain_alloc_irqs(dev, domain, ctrl);	/* [한국어] 코어의 표준 할당 경로 */
}

/*
 * [한국어]
 * msi_domain_alloc_locked - 할당하고 실패하면 되돌린다
 *
 * @dev:  대상 장치
 * @ctrl: 대상 범위와 개수
 * @return: 0 성공, 음수 오류, 양수 재시도 신호
 *
 * 위 함수에 되돌리기를 씌운 것이 전부다. 그런데 이 분리가 중요하다 —
 * 부분 성공 상태를 그대로 두면 서술자에 번호가 배정된 채 남아,
 * 다음 시도가 NOTASSOCIATED 필터에 걸려 아무것도 못 한다.
 *
 * 양수(재시도 신호)도 되돌린다는 점에 주목: PCI 멀티 MSI 가 절반으로
 * 줄여 다시 시도할 때, 앞선 시도의 흔적이 남아 있으면 안 된다.
 * `if (ret)` 이 0 이 아닌 모든 값을 잡는 것이 그 처리다.
 *
 * msi_domain_free_locked() 가 범위 전체를 되돌리는 것도 의도적이다.
 * 어디까지 성공했는지 추적하지 않고 전부 훑으면, 배정되지 않은
 * 서술자는 그쪽에서 알아서 건너뛴다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_alloc_irqs_range_locked() / msi_domain_alloc_irqs_all_locked()
 *   → [이 함수] → __msi_domain_alloc_locked()
 */
static int msi_domain_alloc_locked(struct device *dev, struct msi_ctrl *ctrl)
{
	int ret = __msi_domain_alloc_locked(dev, ctrl);	/* [한국어] 실제 할당 */

	if (ret)	/* [한국어] 0 이 아닌 모든 값 — 오류든 재시도 신호든 */
		msi_domain_free_locked(dev, ctrl);	/* [한국어] 부분 성공 상태를 전부 되돌린다. 남겨 두면 다음 시도가 NOTASSOCIATED 필터에 걸려 아무것도 못 한다 */
	return ret;	/* [한국어] 결과를 그대로 올린다 */
}

/**
 * msi_domain_alloc_irqs_range_locked - Allocate interrupts from a MSI interrupt domain
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to allocate (inclusive)
 * @last:	Last index to allocate (inclusive)
 *
 * Must be invoked from within a msi_lock_descs() / msi_unlock_descs()
 * pair. Use this for MSI irqdomains which implement their own descriptor
 * allocation/free.
 *
 * Return: %0 on success or an error code.
 */
/*
 * [한국어]
 * msi_domain_alloc_irqs_range_locked - 인덱스 범위에 인터럽트를 할당한다 (락 보유)
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @first: 시작 인덱스 (포함)
 * @last:  끝 인덱스 (포함)
 * @return: 0 성공, 음수 오류
 *
 * 공개 API 다. 인덱스 범위를 받아 제어 구조체로 바꿔 넘긴다.
 *
 * nirqs 를 (last - first + 1) 로 계산하는 것에 주목: 인덱스마다
 * 인터럽트 하나라는 전제다. PCI 멀티 MSI 처럼 서술자 하나가 여러
 * 벡터를 대표하는 경우에는 이 API 를 쓰지 않는다.
 *
 * 원본 주석대로 서술자를 직접 관리하는 도메인이 쓴다. 락을 호출자가
 * 이미 쥐고 있어야 하는데, 서술자를 만들고 인터럽트를 할당하는
 * 두 단계를 한 임계 구역에 묶어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_alloc_irqs_range() / 서술자를 직접 만드는 드라이버 →
 *   [이 함수] → msi_domain_alloc_locked()
 */
int msi_domain_alloc_irqs_range_locked(struct device *dev, unsigned int domid,
				       unsigned int first, unsigned int last)
{
	struct msi_ctrl ctrl = {	/* [한국어] 인자를 제어 구조체로 모은다 */
		.domid	= domid,	/* [한국어] 대상 도메인 */
		.first	= first,	/* [한국어] 범위 시작 */
		.last	= last,	/* [한국어] 범위 끝 */
		.nirqs	= last + 1 - first,	/* [한국어] 인덱스마다 인터럽트 하나라는 전제. PCI 멀티 MSI 는 이 API 를 쓰지 않는다 */
	};

	return msi_domain_alloc_locked(dev, &ctrl);	/* [한국어] 되돌리기까지 포함한 할당 */
}

/**
 * msi_domain_alloc_irqs_range - Allocate interrupts from a MSI interrupt domain
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to allocate (inclusive)
 * @last:	Last index to allocate (inclusive)
 *
 * Return: %0 on success or an error code.
 */
/*
 * [한국어]
 * msi_domain_alloc_irqs_range - 인덱스 범위에 인터럽트를 할당한다 (락 획득)
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @first: 시작 인덱스 (포함)
 * @last:  끝 인덱스 (포함)
 * @return: 0 성공, 음수 오류
 *
 * 위 함수에 락 획득을 씌운 판이다. 이 파일의 API 는 대부분 이렇게
 * _locked 판과 일반 판이 쌍을 이룬다.
 *
 * 왜 두 판이 필요한가: 서술자를 직접 만드는 호출자는 만들기와
 * 할당하기를 한 임계 구역에 묶어야 하므로 _locked 판을 쓴다. 그럴
 * 필요가 없는 호출자는 일반 판으로 간단히 부른다.
 *
 * guard 를 쓰므로 반환 시점에 자동으로 풀린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 MSI 드라이버 → [이 함수] → msi_domain_alloc_irqs_range_locked()
 */
int msi_domain_alloc_irqs_range(struct device *dev, unsigned int domid,
				unsigned int first, unsigned int last)
{

	guard(msi_descs_lock)(dev);	/* [한국어] 반환 시점에 자동으로 풀린다 */
	return msi_domain_alloc_irqs_range_locked(dev, domid, first, last);	/* [한국어] 락 보유 판에 위임 */
}
EXPORT_SYMBOL_GPL(msi_domain_alloc_irqs_range);	/* [한국어] 플랫폼 MSI 드라이버가 모듈일 수 있다 */

/**
 * msi_domain_alloc_irqs_all_locked - Allocate all interrupts from a MSI interrupt domain
 *
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @nirqs:	The number of interrupts to allocate
 *
 * This function scans all MSI descriptors of the MSI domain and allocates interrupts
 * for all unassigned ones. That function is to be used for MSI domain usage where
 * the descriptor allocation is handled at the call site, e.g. PCI/MSI[X].
 *
 * Return: %0 on success or an error code.
 */
/*
 * [한국어]
 * msi_domain_alloc_irqs_all_locked - 배정되지 않은 모든 서술자에 인터럽트를 준다
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @nirqs: 할당할 인터럽트 수
 * @return: 0 성공, 음수 오류
 *
 * PCI 계층이 쓰는 진입점이다. 범위를 인덱스 공간 전체로 잡고
 * nirqs 를 따로 받는 것이 이 함수의 특징이다.
 *
 * 왜 그 둘이 어긋나는가: PCI 는 서술자를 미리 만들어 저장소에 넣어
 * 둔다. 그 인덱스들이 어디에 있는지 코어는 모른다. 그래서 "전부
 * 훑으면서 아직 번호가 없는 것에 배정하되, 총 nirqs 개까지" 라고
 * 지시한다.
 *
 * PCI 멀티 MSI 에서 특히 그렇다. 서술자는 하나뿐인데 벡터는 여덟
 * 개일 수 있다. 인덱스 범위로는 그 수를 표현할 수 없어 nirqs 가
 * 따로 필요하다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   pci_msi_setup_msi_irqs() (drivers/pci/msi/irqdomain.c) → [이 함수] →
 *   msi_domain_alloc_locked()
 */
int msi_domain_alloc_irqs_all_locked(struct device *dev, unsigned int domid, int nirqs)
{
	struct msi_ctrl ctrl = {	/* [한국어] 범위는 전체, 개수는 따로 */
		.domid	= domid,	/* [한국어] 대상 도메인 */
		.first	= 0,	/* [한국어] 인덱스 공간의 처음부터 */
		.last	= msi_domain_get_hwsize(dev, domid) - 1,	/* [한국어] 끝까지. PCI 가 만들어 둔 서술자가 어느 인덱스에 있는지 코어는 모른다 */
		.nirqs	= nirqs,	/* [한국어] 범위와 별개인 개수. PCI 멀티 MSI 는 서술자 하나에 벡터 여덟 개일 수 있어 범위로는 표현되지 않는다 */
	};

	return msi_domain_alloc_locked(dev, &ctrl);	/* [한국어] 되돌리기까지 포함한 할당 */
}

/*
 * [한국어]
 * __msi_domain_alloc_irq_at - 인덱스 하나에 인터럽트 하나를 할당한다
 *
 * @dev:     대상 장치
 * @domid:   도메인 ID
 * @index:   원하는 인덱스, 또는 MSI_ANY_INDEX (빈 자리 아무 데나)
 * @affdesc: 친화도 지정 (선택적)
 * @icookie: 도메인 고유의 인스턴스 쿠키 (선택적)
 * @return:  struct msi_map — 성공이면 index 와 virq, 실패면 index 에 오류
 *
 * 동적 MSI 할당의 진입점이다. 위 범위 기반 API 들과 달리 하나씩
 * 할당하고, 서술자 생성과 인터럽트 배정을 한 번에 한다.
 *
 * 어떤 상황을 위한 것인가: IMS(Interrupt Message Store)처럼 장치가
 * 실행 중에 인터럽트를 늘리고 줄이는 경우다. 가상 함수 하나가
 * 만들어질 때마다 인터럽트 하나를 추가하는 식이다. 부팅 때 전부
 * 잡아 두는 PCI MSI-X 와는 사용 방식이 다르다.
 *
 * 반환 타입이 struct msi_map 인 이유: 인덱스와 인터럽트 번호를 함께
 * 돌려주어야 한다. MSI_ANY_INDEX 로 요청하면 어느 인덱스가 배정됐는지
 * 호출자가 알아야 나중에 해제할 수 있다.
 *
 * 오류를 map.index 에 담는 관례에 주목: 별도의 오류 필드를 두지 않고
 * 인덱스에 음수를 넣는다. 유효한 인덱스는 항상 0 이상이므로 구분된다.
 *
 * icookie 는 도메인이 서술자마다 붙이는 임의의 값이다. 아래
 * msi_device_domain_alloc_wired() 가 하드웨어 번호와 트리거 방식을
 * 여기 실어 보내는 것이 그 용례다.
 *
 * 삽입 실패 시 되돌리지 않는 것에 주목: msi_insert_desc() 가 실패하면
 * 서술자를 자기가 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_alloc_irq_at() / msi_device_domain_alloc_wired() →
 *   [이 함수] → msi_alloc_desc() → msi_insert_desc() →
 *   __msi_domain_alloc_irqs()
 */
static struct msi_map __msi_domain_alloc_irq_at(struct device *dev, unsigned int domid,
						unsigned int index,
						const struct irq_affinity_desc *affdesc,
						union msi_instance_cookie *icookie)
{
	struct msi_ctrl ctrl = { .domid	= domid, .nirqs = 1, };	/* [한국어] 하나만 할당한다. first/last 는 인덱스가 정해진 뒤 아래에서 채운다 */
	struct irq_domain *domain;	/* [한국어] 대상 도메인 */
	struct msi_map map = { };	/* [한국어] 0 초기화. 실패 경로가 index 만 채우고 virq 는 0 으로 남긴다 */
	struct msi_desc *desc;	/* [한국어] 새로 만든 서술자 */
	int ret;	/* [한국어] 결과 */

	domain = msi_get_device_domain(dev, domid);	/* [한국어] 슬롯에서 도메인을 꺼낸다 */
	if (!domain) {	/* [한국어] 없는가 */
		map.index = -ENODEV;	/* [한국어] 오류를 인덱스 필드에 담는다. 유효한 인덱스는 0 이상이라 구분된다 */
		return map;	/* [한국어] virq 는 0 으로 남는다. 호출자가 index 의 부호로 성패를 판별한다 */
	}

	desc = msi_alloc_desc(dev, 1, affdesc);	/* [한국어] 벡터 하나짜리 서술자. 동적 할당은 항상 하나씩이다 */
	if (!desc) {	/* [한국어] 메모리 부족 */
		map.index = -ENOMEM;	/* [한국어] 서술자 할당 실패. 오류를 인덱스 필드에 담는다 */
		return map;	/* [한국어] 아직 저장소를 건드리지 않아 되돌릴 것이 없다 */
	}

	if (icookie)	/* [한국어] 도메인 고유의 값을 실어 보내는가 */
		desc->data.icookie = *icookie;	/* [한국어] 서술자에 담아 둔다. wire-to-MSI 는 여기에 하드웨어 번호와 트리거 방식을 넣는다 */

	ret = msi_insert_desc(dev, desc, domid, index);	/* [한국어] 저장소에 넣는다. MSI_ANY_INDEX 면 빈 자리를 찾아 준다 */
	if (ret) {	/* [한국어] 실패 */
		map.index = ret;	/* [한국어] 서술자는 msi_insert_desc 가 이미 해제했으므로 되돌릴 것이 없다 */
		return map;	/* [한국어] 삽입 실패. msi_insert_desc 가 서술자를 이미 해제했다 */
	}

	ctrl.first = ctrl.last = desc->msi_index;	/* [한국어] 실제로 배정된 인덱스 하나만을 범위로 삼는다. MSI_ANY_INDEX 였다면 이 값이 방금 정해졌다 */

	ret = __msi_domain_alloc_irqs(dev, domain, &ctrl);	/* [한국어] 인터럽트 번호를 배정하고 메시지를 써 넣는다 */
	if (ret) {	/* [한국어] 실패 */
		map.index = ret;	/* [한국어] 오류를 담고 */
		msi_domain_free_locked(dev, &ctrl);	/* [한국어] 방금 넣은 서술자까지 되돌린다. 여기서는 스스로 정리해야 한다 — 호출자가 감싸 주는 계층이 없다 */
	} else {	/* [한국어] 성공 */
		map.index = desc->msi_index;	/* [한국어] 배정된 인덱스. 나중에 해제할 때 이 값이 필요하다 */
		map.virq = desc->irq;	/* [한국어] 리눅스 인터럽트 번호. 이것으로 request_irq 한다 */
	}
	return map;	/* [한국어] 인덱스와 번호를 함께 돌려준다 */
}

/**
 * msi_domain_alloc_irq_at - Allocate an interrupt from a MSI interrupt domain at
 *			     a given index - or at the next free index
 *
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are allocated
 * @domid:	Id of the interrupt domain to operate on
 * @index:	Index for allocation. If @index == %MSI_ANY_INDEX the allocation
 *		uses the next free index.
 * @affdesc:	Optional pointer to an interrupt affinity descriptor structure
 * @icookie:	Optional pointer to a domain specific per instance cookie. If
 *		non-NULL the content of the cookie is stored in msi_desc::data.
 *		Must be NULL for MSI-X allocations
 *
 * This requires a MSI interrupt domain which lets the core code manage the
 * MSI descriptors.
 *
 * Return: struct msi_map
 *
 *	On success msi_map::index contains the allocated index number and
 *	msi_map::virq the corresponding Linux interrupt number
 *
 *	On failure msi_map::index contains the error code and msi_map::virq
 *	is %0.
 */
/*
 * [한국어]
 * msi_domain_alloc_irq_at - 지정 인덱스(또는 빈 자리)에 인터럽트를 할당한다
 *
 * @dev:     대상 장치
 * @domid:   도메인 ID
 * @index:   원하는 인덱스, 또는 MSI_ANY_INDEX
 * @affdesc: 친화도 지정 (선택적)
 * @icookie: 도메인 고유의 인스턴스 쿠키 (선택적)
 * @return:  struct msi_map
 *
 * 위 함수에 락 획득을 씌운 공개 API 다.
 *
 * 원본 주석의 조건이 중요하다 — 코어가 서술자를 관리하는 도메인이어야
 * 한다. PCI MSI-X 처럼 PCI 계층이 서술자를 직접 만드는 도메인에서는
 * 이 API 를 쓸 수 없다. 그래서 icookie 도 MSI-X 할당에는 NULL 이어야
 * 한다고 못 박는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   IMS 드라이버 → [이 함수] → __msi_domain_alloc_irq_at()
 */
struct msi_map msi_domain_alloc_irq_at(struct device *dev, unsigned int domid, unsigned int index,
				       const struct irq_affinity_desc *affdesc,
				       union msi_instance_cookie *icookie)
{
	guard(msi_descs_lock)(dev);	/* [한국어] 반환 시점에 자동으로 풀린다 */
	return __msi_domain_alloc_irq_at(dev, domid, index, affdesc, icookie);	/* [한국어] 락 보유 판에 위임 */
}

/**
 * msi_device_domain_alloc_wired - Allocate a "wired" interrupt on @domain
 * @domain:	The domain to allocate on
 * @hwirq:	The hardware interrupt number to allocate for
 * @type:	The interrupt type
 *
 * This weirdness supports wire to MSI controllers like MBIGEN.
 *
 * @hwirq is the hardware interrupt number which is handed in from
 * irq_create_fwspec_mapping(). As the wire to MSI domain is sparse, but
 * sized in firmware, the hardware interrupt number cannot be used as MSI
 * index. For the underlying irq chip the MSI index is irrelevant and
 * all it needs is the hardware interrupt number.
 *
 * To handle this the MSI index is allocated with MSI_ANY_INDEX and the
 * hardware interrupt number is stored along with the type information in
 * msi_desc::cookie so the underlying interrupt chip and domain code can
 * retrieve it.
 *
 * Return: The Linux interrupt number (> 0) or an error code
 */
/*
 * [한국어]
 * msi_device_domain_alloc_wired - 배선 인터럽트를 MSI 로 변환해 할당한다
 *
 * @domain: wire-to-MSI 도메인
 * @hwirq:  펌웨어가 지정한 하드웨어 인터럽트 번호
 * @type:   트리거 방식
 * @return: 리눅스 인터럽트 번호 (양수), 또는 음수 오류
 *
 * MBIGEN 같은 하드웨어를 위한 함수다. 그것은 실제 배선 인터럽트
 * 수백 개를 받아 MSI 로 바꿔 GIC 에 전달한다. ARM 서버에서 배선을
 * 줄이려는 설계다.
 *
 * 원본 주석이 문제와 해법을 설명한다. 배선 번호는 펌웨어가 정하고
 * 듬성듬성하다 — 4096 개 중 몇 개만 쓰일 수 있다. 그것을 MSI
 * 인덱스로 그대로 쓰면 저장소가 그 범위를 감당해야 한다.
 *
 * 해법: MSI 인덱스는 MSI_ANY_INDEX 로 아무 빈 자리나 받고, 진짜
 * 필요한 하드웨어 번호는 쿠키에 실어 보낸다. 아래 층의 칩 코드는
 * MSI 인덱스에 관심이 없고 하드웨어 번호만 필요하므로 이 방식이
 * 통한다.
 *
 * 쿠키에 두 값을 패킹하는 방식에 주목: 상위 32비트에 트리거 방식,
 * 하위 32비트에 하드웨어 번호다. 쿠키가 u64 하나뿐이라 이렇게
 * 나눠 담는다.
 *
 * 반환값 변환: msi_map 의 인덱스가 음수면 오류이므로 그것을,
 * 아니면 인터럽트 번호를 돌려준다. 이 API 의 호출자(도메인 코어)가
 * 정수 하나만 기대하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_create_fwspec_mapping() → 도메인 alloc 경로 → [이 함수] →
 *   __msi_domain_alloc_irq_at()
 */
int msi_device_domain_alloc_wired(struct irq_domain *domain, unsigned int hwirq,
				  unsigned int type)
{
	unsigned int domid = MSI_DEFAULT_DOMAIN;	/* [한국어] wire-to-MSI 는 항상 기본 도메인을 쓴다 */
	union msi_instance_cookie icookie = { };	/* [한국어] 아래 층에 전달할 값 */
	struct device *dev = domain->dev;	/* [한국어] 이 도메인이 속한 장치 (브리지 컨트롤러) */
	struct msi_map map = { };	/* [한국어] 할당 결과 */

	if (WARN_ON_ONCE(!dev || domain->bus_token != DOMAIN_BUS_WIRED_TO_MSI))	/* [한국어] 장치가 없거나 wire-to-MSI 도메인이 아닌가 — 잘못된 경로로 들어왔다 */
		return -EINVAL;

	icookie.value = ((u64)type << 32) | hwirq;	/* [한국어] 쿠키가 u64 하나뿐이라 두 값을 나눠 담는다. 상위 32비트는 트리거 방식, 하위는 하드웨어 번호다 */

	guard(msi_descs_lock)(dev);	/* [한국어] 저장소 조작을 직렬화한다 */
	if (WARN_ON_ONCE(msi_get_device_domain(dev, domid) != domain))	/* [한국어] 슬롯의 도메인이 인자로 받은 것과 같은가 — 다르면 계층 구성이 어긋난 것이다 */
		map.index = -EINVAL;
	else	/* [한국어] 정상 */
		map = __msi_domain_alloc_irq_at(dev, domid, MSI_ANY_INDEX, NULL, &icookie);	/* [한국어] MSI 인덱스는 아무 빈 자리나. 진짜 필요한 하드웨어 번호는 쿠키로 전달된다 */
	return map.index >= 0 ? map.virq : map.index;	/* [한국어] 도메인 코어가 정수 하나만 기대하므로, 성공이면 인터럽트 번호를 실패면 오류 코드를 돌려준다 */
}

/*
 * [한국어]
 * __msi_domain_free_irqs - 범위 안 서술자들의 인터럽트를 반납한다
 *
 * @dev:    대상 장치
 * @domain: 대상 도메인
 * @ctrl:   대상 범위
 * @return: 없음
 *
 * __msi_domain_alloc_irqs() 의 반대다. 인터럽트 번호를 반납하고
 * 서술자를 "배정되지 않음" 상태로 되돌린다. 서술자 자체는 여기서
 * 해제하지 않는다 — 그것은 호출자가 플래그를 보고 결정한다.
 *
 * 비활성화를 먼저 하는 것이 이 함수의 핵심이다. 비활성화는 장치의
 * MSI 메시지를 0 으로 밀어 인터럽트를 보내지 못하게 한다. 그것 없이
 * 벡터를 반납하면, 벡터가 다른 인터럽트에 재배정된 뒤 이 장치가
 * 남의 벡터로 인터럽트를 보내게 된다.
 *
 * irqd_is_activated 를 확인하는 이유: 예약 모드에서는 활성 표시가
 * 지워진 채로 남아 있을 수 있다. 그런 인터럽트를 비활성화하려 하면
 * 상태 추적이 어긋난다.
 *
 * desc->irq 를 0 으로 미는 마지막 줄이 상태 전환이다. 이 뒤로
 * msi_desc_match(NOTASSOCIATED) 가 참이 되어 다시 할당받을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_free_locked() → [이 함수] → irq_domain_deactivate_irq() →
 *   irq_domain_free_irqs()
 */
static void __msi_domain_free_irqs(struct device *dev, struct irq_domain *domain,
				   struct msi_ctrl *ctrl)
{
	struct xarray *xa = &dev->msi.data->__domains[ctrl->domid].store;	/* [한국어] 서술자 저장소 */
	struct msi_domain_info *info = domain->host_data;	/* [한국어] 도메인 설정 (sysfs 플래그를 본다) */
	struct irq_data *irqd;	/* [한국어] 비활성화할 irq_data */
	struct msi_desc *desc;	/* [한국어] 순회 중인 서술자 */
	unsigned long idx;	/* [한국어] 순회 인덱스 */
	int i;	/* [한국어] 벡터 순회용 */

	xa_for_each_range(xa, idx, desc, ctrl->first, ctrl->last) {	/* [한국어] 범위 안의 서술자들 */
		/* Only handle MSI entries which have an interrupt associated */
		if (!msi_desc_match(desc, MSI_DESC_ASSOCIATED))	/* [한국어] (위 영어 주석) 인터럽트가 배정된 것만 */
			continue;	/* [한국어] 아직 배정 전이면 반납할 것이 없다 */

		/* Make sure all interrupts are deactivated */
		for (i = 0; i < desc->nvec_used; i++) {	/* [한국어] (위 영어 주석) 이 서술자가 대표하는 각 벡터에 대해 */
			irqd = irq_domain_get_irq_data(domain, desc->irq + i);	/* [한국어] 이 층의 irq_data */
			if (irqd && irqd_is_activated(irqd))	/* [한국어] 활성화돼 있는가. 예약 모드에서는 표시가 지워진 채 남을 수 있어 확인이 필요하다 */
				irq_domain_deactivate_irq(irqd);	/* [한국어] 장치의 MSI 메시지를 0 으로 민다. 이것 없이 벡터를 반납하면 재배정된 뒤 장치가 남의 벡터로 인터럽트를 보낸다 */
		}

		irq_domain_free_irqs(desc->irq, desc->nvec_used);	/* [한국어] 인터럽트 번호와 벡터를 계층 전체에서 반납한다 */
		if (info->flags & MSI_FLAG_DEV_SYSFS)	/* [한국어] 코어가 sysfs 항목을 만들어 준 도메인인가 */
			msi_sysfs_remove_desc(dev, desc);	/* [한국어] msi_irqs 아래 파일들을 뗀다 */
		desc->irq = 0;	/* [한국어] "배정되지 않음" 상태로 되돌린다. 이 뒤로 NOTASSOCIATED 필터가 참이 되어 다시 할당받을 수 있다 */
	}
}

/*
 * [한국어]
 * msi_domain_free_locked - 해제의 공통 절차
 *
 * @dev:  대상 장치
 * @ctrl: 대상 범위
 * @return: 없음
 *
 * __msi_domain_alloc_locked() 와 대칭인 해제 경로다. 갈림도 같다 —
 * 도메인이 domain_free_irqs 콜백을 제공하면 그쪽에 통째로 맡기고,
 * 아니면 코어의 경로를 쓴다.
 *
 * 마지막의 서술자 해제가 조건부인 것이 중요하다.
 * MSI_FLAG_FREE_MSI_DESCS 는 "코어가 서술자를 만들었으니 코어가
 * 없애라" 는 뜻이다. PCI 처럼 호출자가 직접 만든 서술자는 호출자가
 * 없애야 하므로 이 플래그를 세우지 않는다.
 *
 * 그 플래그가 MSI_FLAG_ALLOC_SIMPLE_MSI_DESCS 와 짝을 이룬다 —
 * 만드는 쪽과 없애는 쪽이 같아야 한다.
 *
 * 실패를 알리지 않는 것에 주목: 반환 타입이 void 다. 해제는 실패할
 * 수 없고, 잘못된 범위는 msi_ctrl_valid 가 경고한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_free_irqs_range_locked() / msi_domain_alloc_locked() 의
 *   실패 경로 → [이 함수] → __msi_domain_free_irqs()
 */
static void msi_domain_free_locked(struct device *dev, struct msi_ctrl *ctrl)
{
	struct msi_domain_info *info;	/* [한국어] 도메인 설정 */
	struct msi_domain_ops *ops;	/* [한국어] 콜백 묶음 */
	struct irq_domain *domain;	/* [한국어] 대상 도메인 */

	if (!msi_ctrl_valid(dev, ctrl))	/* [한국어] 범위 검사. 잘못된 범위로 순회하면 엉뚱한 서술자를 건드린다 */
		return;

	domain = msi_get_device_domain(dev, ctrl->domid);	/* [한국어] 슬롯에서 도메인을 꺼낸다 */
	if (!domain)	/* [한국어] 없는가 */
		return;	/* [한국어] 이미 제거된 도메인이다. 해제할 것이 없다 */

	info = domain->host_data;	/* [한국어] 도메인 설정 */
	ops = info->ops;	/* [한국어] 콜백 묶음 */

	if (ops->domain_free_irqs)	/* [한국어] 해제 전체를 자기가 하겠다는 도메인인가 */
		ops->domain_free_irqs(domain, dev);	/* [한국어] Xen 이나 아키텍처 고유 구현. 범위를 넘기지 않는 것은 그쪽이 저장소 전체를 훑기 때문이다 */
	else	/* [한국어] 코어의 표준 경로 */
		__msi_domain_free_irqs(dev, domain, ctrl);	/* [한국어] 인터럽트 번호를 반납하고 서술자를 미배정 상태로 되돌린다 */

	if (info->flags & MSI_FLAG_FREE_MSI_DESCS)	/* [한국어] 코어가 서술자를 만들었으니 코어가 없애라는 뜻인가 */
		msi_domain_free_descs(dev, ctrl);	/* [한국어] 서술자 자체를 해제한다. PCI 처럼 호출자가 만든 것은 호출자가 없애야 해 이 플래그를 세우지 않는다 */
}

/**
 * msi_domain_free_irqs_range_locked - Free a range of interrupts from a MSI interrupt domain
 *				       associated to @dev with msi_lock held
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to free (inclusive)
 * @last:	Last index to free (inclusive)
 */
/*
 * [한국어]
 * msi_domain_free_irqs_range_locked - 인덱스 범위의 인터럽트를 반납한다 (락 보유)
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @first: 시작 인덱스 (포함)
 * @last:  끝 인덱스 (포함)
 * @return: 없음
 *
 * msi_domain_alloc_irqs_range_locked() 의 반대다. 인자를 제어
 * 구조체로 바꿔 넘기는 껍데기다.
 *
 * nirqs 를 채우지 않는 것에 주목: 해제 경로는 그 값을 쓰지 않는다.
 * 범위 안에서 배정된 것을 전부 반납하면 되고, 개수를 미리 알 필요가
 * 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   msi_domain_free_irqs_range() / msi_domain_free_irqs_all_locked() /
 *   msi_device_domain_free_wired() → [이 함수] → msi_domain_free_locked()
 */
void msi_domain_free_irqs_range_locked(struct device *dev, unsigned int domid,
				       unsigned int first, unsigned int last)
{
	struct msi_ctrl ctrl = {	/* [한국어] 인자를 제어 구조체로 */
		.domid	= domid,	/* [한국어] 대상 도메인 */
		.first	= first,	/* [한국어] 범위 시작 */
		.last	= last,	/* [한국어] 범위 끝. nirqs 를 채우지 않는 것은 해제 경로가 그 값을 쓰지 않기 때문이다 */
	};
	msi_domain_free_locked(dev, &ctrl);	/* [한국어] 실제 해제 */
}

/**
 * msi_domain_free_irqs_range - Free a range of interrupts from a MSI interrupt domain
 *				associated to @dev
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	Id of the interrupt domain to operate on
 * @first:	First index to free (inclusive)
 * @last:	Last index to free (inclusive)
 */
/*
 * [한국어]
 * msi_domain_free_irqs_range - 인덱스 범위의 인터럽트를 반납한다 (락 획득)
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @first: 시작 인덱스 (포함)
 * @last:  끝 인덱스 (포함)
 * @return: 없음
 *
 * 위 함수에 락 획득을 씌운 판이다.
 *
 * 바로 아래의 EXPORT_SYMBOL_GPL 이 이 함수가 아니라
 * msi_domain_free_irqs_all 을 내보내는 것에 주목: 위치가 어긋나
 * 있다. C 에서 EXPORT_SYMBOL 은 어디에 있어도 되므로 동작에는
 * 문제가 없지만, 읽는 사람은 이 함수가 내보내진다고 오해하기 쉽다.
 * 이 함수 자체는 내보내지지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   플랫폼 MSI 드라이버 → [이 함수] → msi_domain_free_irqs_range_locked()
 */
void msi_domain_free_irqs_range(struct device *dev, unsigned int domid,
				unsigned int first, unsigned int last)
{
	guard(msi_descs_lock)(dev);	/* [한국어] 반환 시점에 자동으로 풀린다 */
	msi_domain_free_irqs_range_locked(dev, domid, first, last);	/* [한국어] 락 보유 판에 위임 */
}
EXPORT_SYMBOL_GPL(msi_domain_free_irqs_all);	/* [한국어] 위치가 어긋나 있다 — 바로 위 함수가 아니라 아래쪽 msi_domain_free_irqs_all 을 내보낸다. C 에서 EXPORT_SYMBOL 은 어디에 있어도 되므로 동작에는 문제가 없다 */

/**
 * msi_domain_free_irqs_all_locked - Free all interrupts from a MSI interrupt domain
 *				     associated to a device
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	The id of the domain to operate on
 *
 * Must be invoked from within a msi_lock_descs() / msi_unlock_descs()
 * pair. Use this for MSI irqdomains which implement their own vector
 * allocation.
 */
/*
 * [한국어]
 * msi_domain_free_irqs_all_locked - 도메인의 모든 인터럽트를 반납한다 (락 보유)
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @return: 없음
 *
 * 범위를 인덱스 공간 전체로 잡아 위 함수를 부른다. 장치가 MSI 를
 * 끄거나 제거될 때 쓴다.
 *
 * msi_domain_alloc_irqs_all_locked() 와 짝을 이루지만, 그쪽이
 * nirqs 를 따로 받은 것과 달리 여기서는 그럴 필요가 없다. 해제는
 * 있는 것을 전부 반납하면 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 뮤텍스 보유.
 *
 * 호출 체인:
 *   pci_free_msi_irqs() / 드라이버 → [이 함수] →
 *   msi_domain_free_irqs_range_locked()
 */
void msi_domain_free_irqs_all_locked(struct device *dev, unsigned int domid)
{
	msi_domain_free_irqs_range_locked(dev, domid, 0,	/* [한국어] 인덱스 0 부터 */
					  msi_domain_get_hwsize(dev, domid) - 1);	/* [한국어] 끝까지. 할당과 달리 개수를 따로 받지 않는 것은 있는 것을 전부 반납하면 되기 때문이다 */
}

/**
 * msi_domain_free_irqs_all - Free all interrupts from a MSI interrupt domain
 *			      associated to a device
 * @dev:	Pointer to device struct of the device for which the interrupts
 *		are freed
 * @domid:	The id of the domain to operate on
 */
/*
 * [한국어]
 * msi_domain_free_irqs_all - 도메인의 모든 인터럽트를 반납한다 (락 획득)
 *
 * @dev:   대상 장치
 * @domid: 도메인 ID
 * @return: 없음
 *
 * 위 함수에 락 획득을 씌운 공개 API 다. 이 심볼이 위쪽의 엉뚱한
 * 위치에서 EXPORT_SYMBOL_GPL 로 내보내진다.
 *
 * 장치가 MSI 를 끌 때 PCI 계층이 부르는 진입점이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   pci_disable_msi() / pci_disable_msix() → [이 함수] →
 *   msi_domain_free_irqs_all_locked()
 */
void msi_domain_free_irqs_all(struct device *dev, unsigned int domid)
{
	guard(msi_descs_lock)(dev);	/* [한국어] 반환 시점에 자동으로 풀린다 */
	msi_domain_free_irqs_all_locked(dev, domid);	/* [한국어] 락 보유 판에 위임 */
}

/**
 * msi_device_domain_free_wired - Free a wired interrupt in @domain
 * @domain:	The domain to free the interrupt on
 * @virq:	The Linux interrupt number to free
 *
 * This is the counterpart of msi_device_domain_alloc_wired() for the
 * weird wired to MSI converting domains.
 */
/*
 * [한국어]
 * msi_device_domain_free_wired - 배선-MSI 변환 인터럽트를 반납한다
 *
 * @domain: wire-to-MSI 도메인
 * @virq:   반납할 리눅스 인터럽트 번호
 * @return: 없음
 *
 * msi_device_domain_alloc_wired() 의 반대다. 인터럽트 번호로
 * 시작해 서술자를 찾고, 그 서술자의 MSI 인덱스로 해제한다.
 *
 * 이 우회가 필요한 이유: 할당 때 MSI 인덱스는 MSI_ANY_INDEX 로
 * 아무 빈 자리나 받았다. 호출자(도메인 코어)는 그 인덱스를 모르고
 * 리눅스 인터럽트 번호만 안다. 그래서 서술자를 거쳐 인덱스를 되찾는다.
 *
 * 세 가지 검사가 앞에 있는 것에 주목: 장치, 서술자, 도메인 종류를
 * 모두 확인한다. 이 API 는 특수한 도메인 전용이라 잘못된 호출을
 * 엄격히 걸러 낸다.
 *
 * 슬롯의 도메인이 인자와 같은지 다시 확인하는 것도 같은 맥락이다.
 * 다르면 계층 구성이 어긋난 것이고, 그대로 진행하면 남의 서술자를
 * 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   irq_dispose_mapping() → 도메인 free 경로 → [이 함수] →
 *   msi_domain_free_irqs_range_locked()
 */
void msi_device_domain_free_wired(struct irq_domain *domain, unsigned int virq)
{
	struct msi_desc *desc = irq_get_msi_desc(virq);	/* [한국어] 인터럽트 번호로 서술자를 찾는다. 여기서 MSI 인덱스를 되찾는 것이 목적이다 */
	struct device *dev = domain->dev;	/* [한국어] 브리지 컨트롤러 장치 */

	if (WARN_ON_ONCE(!dev || !desc || domain->bus_token != DOMAIN_BUS_WIRED_TO_MSI))	/* [한국어] 장치, 서술자, 도메인 종류를 모두 확인한다. 특수 API 라 잘못된 호출을 엄격히 걸러 낸다 */
		return;

	guard(msi_descs_lock)(dev);	/* [한국어] 저장소 조작 직렬화 */
	if (WARN_ON_ONCE(msi_get_device_domain(dev, MSI_DEFAULT_DOMAIN) != domain))	/* [한국어] 슬롯의 도메인이 인자와 같은가 — 다르면 남의 서술자를 해제하게 된다 */
		return;
	msi_domain_free_irqs_range_locked(dev, MSI_DEFAULT_DOMAIN, desc->msi_index,	/* [한국어] 서술자에서 되찾은 인덱스 하나만 해제한다 */
					  desc->msi_index);	/* [한국어] 시작과 끝이 같다 — 하나뿐이다 */
}

/**
 * msi_get_domain_info - Get the MSI interrupt domain info for @domain
 * @domain:	The interrupt domain to retrieve data from
 *
 * Return: the pointer to the msi_domain_info stored in @domain->host_data.
 */
/*
 * [한국어]
 * msi_get_domain_info - 도메인에서 MSI 설정 구조체를 꺼낸다
 *
 * @domain: 대상 도메인
 * @return: host_data 에 저장된 msi_domain_info
 *
 * 형변환 하나가 전부인 함수다. 그래도 따로 두는 이유는 지식의
 * 위치다 — "MSI 도메인은 host_data 에 msi_domain_info 를 둔다" 는
 * 규약을 아는 곳을 이 파일 안에 모아 둔다.
 *
 * 검사가 없는 것에 주목: MSI 도메인이 아닌 것을 넘기면 엉뚱한
 * 포인터가 나온다. 호출자가 도메인 종류를 이미 안다는 전제다.
 *
 * 실행 컨텍스트: 제약 없음.
 *
 * 호출 체인:
 *   PCI 계층 / irqchip 드라이버 → [이 함수]
 */
struct msi_domain_info *msi_get_domain_info(struct irq_domain *domain)
{
	return (struct msi_domain_info *)domain->host_data;	/* [한국어] 형변환뿐이다. MSI 도메인이 아닌 것을 넘기면 엉뚱한 포인터가 나오므로 호출자가 종류를 알아야 한다 */
}

/**
 * msi_device_has_isolated_msi - True if the device has isolated MSI
 * @dev: The device to check
 *
 * Isolated MSI means that HW modeled by an irq_domain on the path from the
 * initiating device to the CPU will validate that the MSI message specifies an
 * interrupt number that the device is authorized to trigger. This must block
 * devices from triggering interrupts they are not authorized to trigger.
 * Currently authorization means the MSI vector is one assigned to the device.
 *
 * This is interesting for securing VFIO use cases where a rouge MSI (eg created
 * by abusing a normal PCI MemWr DMA) must not allow the VFIO userspace to
 * impact outside its security domain, eg userspace triggering interrupts on
 * kernel drivers, a VM triggering interrupts on the hypervisor, or a VM
 * triggering interrupts on another VM.
 *
 */
/*
 * [한국어]
 * msi_device_has_isolated_msi - MSI 격리가 보장되는 장치인지 확인한다
 *
 * @dev: 검사할 장치
 * @return: true 격리됨, false 격리되지 않음
 *
 * 이 파일에서 보안과 가장 직접 관련된 함수다.
 *
 * MSI 는 결국 메모리 쓰기다. 그래서 장치가 DMA 로 임의의 주소에
 * 임의의 값을 쓸 수 있다면, MSI 메시지를 흉내 내 아무 인터럽트나
 * 발생시킬 수 있다. 원본 주석이 짚듯 "정상적인 PCI MemWr DMA 를
 * 악용해" 만들어 낼 수 있다.
 *
 * 왜 위험한가: VFIO 로 장치를 사용자 공간이나 가상 머신에 넘겼을 때,
 * 그 장치를 조종해 커널 드라이버의 인터럽트나 하이퍼바이저의
 * 인터럽트, 심지어 다른 VM 의 인터럽트를 발생시킬 수 있다. 보안
 * 경계를 넘는 것이다.
 *
 * 격리(isolated)란 그것을 막는 하드웨어가 경로 어딘가에 있다는
 * 뜻이다. 인터럽트 리매핑 유닛이 대표적이다 — 그것은 "이 장치가
 * 이 벡터를 쓸 권한이 있는가" 를 검사하고, 없으면 버린다.
 *
 * 계층을 훑는 이유: 그 하드웨어가 어느 층에 있는지 미리 알 수 없다.
 * IOMMU 리매핑일 수도, 그 아래 다른 층일 수도 있다. 하나라도 있으면
 * 격리된 것이다.
 *
 * 마지막의 arch_is_isolated_msi(): 도메인 계층에 표시가 없어도
 * 아키텍처 차원에서 격리가 보장되는 경우가 있다. s390 처럼 MSI 를
 * 하드웨어가 다르게 다루는 플랫폼이다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출 체인:
 *   drivers/vfio/vfio_iommu_type1.c / iommufd → [이 함수]
 */
bool msi_device_has_isolated_msi(struct device *dev)
{
	struct irq_domain *domain = dev_get_msi_domain(dev);	/* [한국어] 이 장치의 MSI 도메인. 여기서 계층을 거슬러 올라간다 */

	for (; domain; domain = domain->parent)	/* [한국어] 계층 전체를 훑는다. 격리 하드웨어가 어느 층에 있는지 미리 알 수 없다 */
		if (domain->flags & IRQ_DOMAIN_FLAG_ISOLATED_MSI)	/* [한국어] 이 층이 벡터 권한을 검사하는가 — 인터럽트 리매핑 유닛이 대표적이다 */
			return true;	/* [한국어] 하나라도 있으면 격리된다. 장치가 남의 벡터로 인터럽트를 흉내 낼 수 없다 */
	return arch_is_isolated_msi();	/* [한국어] 도메인 표시가 없어도 아키텍처 차원에서 보장되는 경우가 있다. s390 처럼 MSI 를 하드웨어가 다르게 다루는 플랫폼이다 */
}
EXPORT_SYMBOL_GPL(msi_device_has_isolated_msi);	/* [한국어] VFIO 와 iommufd 가 장치를 사용자 공간에 넘기기 전에 이것을 확인한다 */
