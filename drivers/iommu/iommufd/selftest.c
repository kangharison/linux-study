// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES.
 *
 * Kernel side components to support tools/testing/selftests/iommu
 */
/*
 * [한국어 설명] 셀프테스트가 조종하는 모의 IOMMU 와 모의 장치 (selftest.c)
 *
 * === 파일의 역할 ===
 * 진짜 IOMMU 하드웨어 없이 iommufd 전체를 시험할 수 있게, 커널 안에
 * 가짜 IOMMU 와 가짜 장치를 만들어 둔 파일이다. 위 영어 주석대로
 * tools/testing/selftests/iommu 의 사용자 공간 프로그램과 짝을 이룬다.
 *
 * 두 부분으로 나뉜다. 앞쪽은 모의 드라이버 — iommu 코어가 요구하는
 * 콜백들을 모두 갖춘 채, 실제로는 generic_pt 의 AMD v1 페이지 테이블을
 * 써서 매핑을 처리한다. 진짜 페이지 테이블 코드를 쓰므로 그 부분까지
 * 함께 시험된다.
 *
 * 뒤쪽은 IOMMU_TEST_CMD 처리기들이다. 사용자 공간이 커널 내부 상태를
 * 들여다보고 조종할 수 있게, 정규 ioctl 로는 할 수 없는 일들을 해 준다 —
 * 폴트를 꾸며 내고, 더티 비트를 손으로 세우고, 매핑이 정말 그 페이지를
 * 가리키는지 커널에게 확인시킨다.
 *
 * 모의 하드웨어는 일부러 까다롭게 만들어 두었다. 페이지 크기가 2KB 라
 * 호스트 페이지보다 작고, 주소 공간의 시작이 0 이 아니며, 장치마다
 * 능력을 다르게 줄 수 있다. 실제 하드웨어에서 드물게 나타나는 상황을
 * 늘 겪게 하려는 것이다.
 *
 * syzkaller 지원도 이 파일의 특징이다. 퍼저는 유효한 IOVA 를 우연히
 * 맞히지 못하므로, 그 모드에서는 64비트 값을 "몇 번째 영역의 몇 번째
 * 바이트"로 해석해 준다.
 *
 * 결함 주입(fault-injection)도 여기서 관리한다. 메모리 부족 같은 드문
 * 실패를 일부러 일으켜 되감기 경로를 시험한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 테스트 프로그램 → ioctl(IOMMU_TEST_CMD) → main.c → iommufd_test()
 *   → 이 파일의 op 별 처리기 → 모의 IOMMU/장치
 *
 * iommufd 정규 경로 → iommu 코어 → mock_ops → 이 파일의 도메인 연산
 *   → generic_pt 의 AMD v1 구현
 *
 * 실행 컨텍스트: 모두 프로세스 문맥. CONFIG_IOMMUFD_TEST 가 켜져야 이
 * 파일이 빌드된다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommufd_test.h 가 정의한 ABI 로 사용자 공간과 이어진다.
 * 아래: iommu 코어(가짜 버스와 장치를 등록한다), generic_pt(진짜 페이지
 *   테이블 구현), dma-buf(장치 메모리 매핑 시험).
 *
 * === 주요 함수/구조체 요약 ===
 * struct mock_iommu_domain: 모의 도메인. union 으로 generic_pt 의 AMD v1
 *   구조체를 겹쳐 두어, 진짜 페이지 테이블 코드를 그대로 쓴다.
 * struct mock_dev: 모의 장치. 능력 플래그와 가짜 캐시 배열을 든다.
 * mock_ops: iommu 코어에 등록하는 연산표. 이 파일의 앞쪽 절반이 그 구현이다.
 * iommufd_test: 테스트 명령을 op 별로 나눠 보내는 입구.
 * iommufd_test_init / exit: 가짜 버스와 장치를 등록하고 걷어 낸다.
 * iommufd_should_fail: 결함 주입 판정. 여러 파일이 이것을 부른다.
 */
#include <linux/anon_inodes.h>	/* [한국어] 접근자 파일처럼 이름 없는 파일을 만드는 데 쓴다. */
#include <linux/debugfs.h>	/* [한국어] 결함 주입 설정을 사용자가 조절할 수 있게 노출한다. */
#include <linux/dma-buf.h>	/* [한국어] 테스트용 dmabuf 를 만들고 무효화하는 데 쓴다. */
#include <linux/dma-resv.h>	/* [한국어] dmabuf 의 예약 락. 상태를 바꿀 때 쥐어야 한다. */
#include <linux/fault-inject.h>	/* [한국어] 일부러 실패를 내는 틀. 되감기 경로를 시험한다. */
#include <linux/file.h>	/* [한국어] fget / fput — fd 로 접근자를 되찾는다. */
#include <linux/iommu.h>	/* [한국어] 드라이버가 채워야 할 연산표와 도메인 정의. */
#include <linux/platform_device.h>	/* [한국어] IOMMU 를 매달 플랫폼 장치를 만든다. */
#include <linux/slab.h>	/* [한국어] kzalloc / kfree. */
#include <linux/xarray.h>	/* [한국어] 가상 장치 표 등에 쓰이는 자료 구조. */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 정규 명령 구조체. */
#include <linux/generic_pt/iommu.h>	/* [한국어] 진짜 페이지 테이블 구현. 모의 도메인이 이것을 그대로 쓴다. */
#include "../iommu-pages.h"	/* [한국어] 페이지 테이블 페이지를 모아 놓는 helper. */

#include "../iommu-priv.h"	/* [한국어] iommu 코어의 비공개 헤더. 코어 안쪽 함수를 쓴다. */
#include "io_pagetable.h"	/* [한국어] 영역과 예약 IOVA 를 직접 다루는 테스트 명령이 쓴다. */
#include "iommufd_private.h"	/* [한국어] 객체 모형과 이 모듈 안의 선언들. */
#include "iommufd_test.h"	/* [한국어] 사용자와 공유하는 테스트 명령 ABI. 이 파일이 그 구현이다. */

/*
 * [한국어] 결함 주입 설정. debugfs 로 확률과 횟수를 조절한다.
 *
 * 메모리 부족처럼 드물게만 일어나는 실패를 일부러 일으켜, 되감기 경로가
 * 제대로 도는지 시험한다. 그런 경로는 자연스럽게는 거의 실행되지 않아
 * 버그가 오래 숨어 있기 쉽다.
 */
static DECLARE_FAULT_ATTR(fail_iommufd);
static struct dentry *dbgfs_root;	/* [한국어] 결함 주입 설정이 놓이는 debugfs 디렉터리. 내릴 때 통째로 지운다. */
static struct platform_device *selftest_iommu_dev;	/* [한국어] 모의 IOMMU 를 매달 플랫폼 장치. 코어는 IOMMU 자신도 어떤 장치이기를 요구한다. */
static const struct iommu_ops mock_ops;	/* [한국어] 아래에서 정의하는 연산표의 전방 선언. 그 표가 여기 위쪽 함수들을 가리키므로 순서를 풀어 준다. */
static struct iommu_domain_ops domain_nested_ops;	/* [한국어] 중첩 도메인 연산표의 전방 선언. 같은 이유다. */

size_t iommufd_test_memory_limit = 65536;	/* [한국어] pages.c 의 임시 버퍼 한도. 테스트가 이 값을 아주 작게 바꿔 여러 조각으로 나뉘는 경로를 타게 한다. */

/*
 * [한국어] 가짜 버스 타입.
 *
 * 리눅스의 장치 모형에서 장치는 반드시 어떤 버스에 속해야 한다. 모의
 * 장치가 붙을 버스가 없으므로 하나 만들어 등록한다.
 *
 * notifier 를 함께 둔 것은 이 구조체가 확장되어 온 흔적이다.
 */
struct mock_bus_type {
	/* [한국어] 가짜 버스 자체. 모의 장치가 여기 등록된다.
	 *  설정자: 파일 위쪽의 정적 초기화가 이름만 채운다.
	 *  읽는 자: mock_probe_device 가 이 주소로 우리 장치인지 가른다. */
	struct bus_type bus;
	/* [한국어] 버스 사건 알림을 받을 자리.
	 *  설정자·읽는 자: iommu_device_register_bus 가 이것을 등록하고 걷는다.
	 *  이 파일은 직접 쓰지 않지만, 그 함수가 요구하는 인자라 함께 둔다. */
	struct notifier_block nb;
};

static struct mock_bus_type iommufd_mock_bus_type = {	/* [한국어] 하나뿐인 가짜 버스. 정적으로 두어 주소가 고정되고, 그 주소가 곧 판별 수단이 된다. */
	.bus = {	/* [한국어] 버스 구조체의 초기화. */
		.name = "iommufd_mock",	/* [한국어] sysfs 의 /sys/bus 아래에 이 이름으로 보인다. */
	},
};

static DEFINE_IDA(mock_dev_ida);	/* [한국어] 장치 번호를 배정하는 id 할당기. 이름을 짓는 데만 쓰인다. */

enum {	/* [한국어] 모의 도메인의 상태 비트. */
	MOCK_DIRTY_TRACK = 1,	/* [한국어] 더티 추적이 켜져 있음. 설정자는 set_dirty_tracking, 읽는 자는 DIRTY 명령이다. */
};

static int mock_dev_enable_iopf(struct device *dev, struct iommu_domain *domain);	/* [한국어] 아래 정의의 전방 선언. 그 앞의 붙이기 함수들이 이것을 부르기 때문이다. */
static void mock_dev_disable_iopf(struct device *dev, struct iommu_domain *domain);	/* [한국어] 같은 이유의 전방 선언. */

/*
 * Syzkaller has trouble randomizing the correct iova to use since it is linked
 * to the map ioctl's output, and it has no ide about that. So, simplify things.
 * In syzkaller mode the 64 bit IOVA is converted into an nth area and offset
 * value. This has a much smaller randomization space and syzkaller can hit it.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * __iommufd_test_syz_conv_iova - 퍼저가 준 값을 유효한 IOVA 로 바꾼다
 *
 * @iopt: 영역 목록을 든 구조.
 * @iova: 들어올 때는 퍼저가 만든 값, 나갈 때는 그 뜻대로 계산한 IOVA.
 * @return: 계산된 IOVA. 해당 영역이 없으면 0.
 *
 * 원 주석이 이유를 밝힌다 — 유효한 IOVA 는 매핑 ioctl 이 돌려준 값이라,
 * 퍼저가 그것을 우연히 맞힐 확률이 사실상 0 이다. 그래서 64비트 값을
 * "몇 번째 영역의 몇 번째 바이트"로 해석해, 훨씬 좁은 공간에서 유효한
 * 주소가 나오게 한다.
 *
 * 이 변환이 없으면 퍼저는 늘 "없는 주소" 오류만 만나고 실제 코드에는
 * 닿지 못한다.
 */
static unsigned long __iommufd_test_syz_conv_iova(struct io_pagetable *iopt,
						  u64 *iova)
{
	struct syz_layout {	/* [한국어] 퍼저가 만든 64비트 값을 두 32비트 필드로 해석하는 틀. */
		__u32 nth_area;	/* [한국어] 몇 번째 영역인가. */
		__u32 offset;	/* [한국어] 그 영역 안에서 몇 바이트째인가. */
	};
	struct syz_layout *syz = (void *)iova;	/* [한국어] 같은 자리를 두 필드로 다시 본다. 값을 복사하지 않고 해석만 바꾼다. */
	unsigned int nth = syz->nth_area;	/* [한국어] 세어 나갈 영역 번호. */
	struct iopt_area *area;	/* [한국어] 훑을 영역. */

	down_read(&iopt->iova_rwsem);	/* [한국어] 영역 트리를 읽는 동안 바뀌지 않게 한다. */
	for (area = iopt_area_iter_first(iopt, 0, ULONG_MAX); area;	/* [한국어] 주소 공간 전체의 영역을 차례로. */
	     area = iopt_area_iter_next(area, 0, ULONG_MAX)) {	/* [한국어] 다음 영역으로. 주소 순서대로 온다. */
		if (nth == 0) {	/* [한국어] 그 번째에 닿았으면 */
			up_read(&iopt->iova_rwsem);	/* [한국어] 락을 놓고 */
			return iopt_area_iova(area) + syz->offset;	/* [한국어] 그 영역의 시작에 오프셋을 더한 값을 준다. 오프셋이 영역을 넘어도 상관없다 — 그 경우를 시험하려는 것이기도 하다. */
		}
		nth--;	/* [한국어] 다음 영역으로. */
	}
	up_read(&iopt->iova_rwsem);	/* [한국어] 락 해제. */

	return 0;	/* [한국어] 그만큼의 영역이 없다. 0 은 유효한 IOVA 가 아니라 실패를 뜻한다. */
}

/*
 * [한국어]
 * iommufd_test_syz_conv_iova - 접근자를 통해 같은 변환을 한다
 *
 * @access: 대상 접근자.
 * @iova: 변환할 값.
 * @return: 계산된 IOVA, 붙어 있지 않으면 0.
 *
 * 접근자의 주소 공간을 찾아 위 함수에 넘긴다. 뮤텍스 아래에서 봐야
 * 그 사이 다른 IOAS 로 바뀌지 않는다.
 */
static unsigned long iommufd_test_syz_conv_iova(struct iommufd_access *access,
						u64 *iova)
{
	unsigned long ret;	/* [한국어] 계산된 IOVA. */

	mutex_lock(&access->ioas_lock);	/* [한국어] 주소 공간 포인터를 지키는 뮤텍스. */
	if (!access->ioas) {	/* [한국어] 붙어 있지 않으면 */
		mutex_unlock(&access->ioas_lock);	/* [한국어] 락을 놓고 */
		return 0;	/* [한국어] 변환할 수 없다. */
	}
	ret = __iommufd_test_syz_conv_iova(&access->ioas->iopt, iova);	/* [한국어] 그 주소 공간의 영역 목록으로 변환한다. */
	mutex_unlock(&access->ioas_lock);	/* [한국어] 락 해제. */
	return ret;	/* [한국어] 계산된 IOVA. */
}

/*
 * [한국어]
 * iommufd_test_syz_conv_iova_id - IOAS id 로 지정해 같은 변환을 한다
 *
 * @ucmd: 처리 중인 명령.
 * @ioas_id: 대상 IOAS 의 id.
 * @iova: 변환할 값(제자리에서 바뀐다).
 * @flags: 퍼저 모드 비트를 여기서 지운다.
 *
 * 정규 명령 처리기들이 이 함수를 부른다. 퍼저 비트가 없으면 아무것도
 * 하지 않으므로, 평소 경로에 아무 영향이 없다.
 *
 * 비트를 지우는 것이 중요하다 — 그러지 않으면 아래 코드가 그것을 모르는
 * 플래그로 보고 거절한다.
 */
void iommufd_test_syz_conv_iova_id(struct iommufd_ucmd *ucmd,
				   unsigned int ioas_id, u64 *iova, u32 *flags)
{
	struct iommufd_ioas *ioas;	/* [한국어] 대상 주소 공간. */

	if (!(*flags & MOCK_FLAGS_ACCESS_SYZ))	/* [한국어] 퍼저 모드가 아니면 */
		return;	/* [한국어] 평소 경로에 아무 영향이 없다. */
	*flags &= ~(u32)MOCK_FLAGS_ACCESS_SYZ;	/* [한국어] 비트를 지운다 — 그러지 않으면 아래 코드가 모르는 플래그로 보고 거절한다. */

	ioas = iommufd_get_ioas(ucmd->ictx, ioas_id);	/* [한국어] id 로 주소 공간을 찾는다. */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return;	/* [한국어] 변환하지 않고 돌아간다. 원래 값이 그대로 쓰여 어차피 실패한다. */
	*iova = __iommufd_test_syz_conv_iova(&ioas->iopt, iova);	/* [한국어] 제자리에서 바꾼다. */
	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 참조를 놓는다. */
}

/*
 * [한국어] 모의 페이징 도메인.
 *
 * union 이 이 구조체의 요점이다. iommu 코어가 보는 iommu_domain 과
 * generic_pt 가 보는 pt_iommu 를 같은 자리에 겹쳐 두어, 어느 쪽에서든
 * 이 구조체를 자기 것으로 다룰 수 있다.
 *
 * 그 덕에 모의 드라이버가 매핑 연산을 흉내 내지 않고 진짜 페이지 테이블
 * 구현(AMD v1)을 그대로 쓴다. 그러면 그 코드까지 함께 시험된다.
 *
 * 아래 PT_IOMMU_CHECK_DOMAIN 이 그 겹침이 실제로 성립하는지 컴파일 시
 * 확인한다.
 */
struct mock_iommu_domain {
	/* [한국어] 코어와 generic_pt 가 같은 자리를 각자 자기 것으로 본다.
	 *  이 겹침이 이 파일의 핵심 수법이다 — 모의 드라이버가 매핑을 흉내 내지
	 *  않고 진짜 페이지 테이블 구현을 그대로 쓴다. */
	union {
		/* [한국어] iommu 코어가 보는 도메인.
		 *  설정자: 이 파일의 할당 함수가 종류·연산표·주소 범위를 채운다.
		 *  읽는 자: 코어의 모든 도메인 경로. */
		struct iommu_domain domain;
		/* [한국어] generic_pt 가 보는 공통 부분.
		 *  읽는 자: 더티 비트 조작처럼 형식과 무관한 연산. */
		struct pt_iommu iommu;
		/* [한국어] AMD v1 형식의 실제 상태.
		 *  설정자: pt_iommu_amdv1_mock_init.
		 *  읽는 자: generic_pt 가 찍어 낸 매핑·해제 함수들. */
		struct pt_iommu_amdv1 amdv1;
	};
	/* [한국어] 모의 도메인의 상태 비트. 지금은 MOCK_DIRTY_TRACK 하나.
	 *  설정자: mock_domain_set_dirty_tracking.
	 *  읽는 자: 더티 비트를 손으로 세우는 테스트 명령이 켜져 있는지 본다. */
	unsigned long flags;
};
PT_IOMMU_CHECK_DOMAIN(struct mock_iommu_domain, iommu, domain);	/* [한국어] union 의 겹침이 실제로 성립하는지 컴파일 시 확인한다. 어긋나면 빌드가 깨진다. */
PT_IOMMU_CHECK_DOMAIN(struct mock_iommu_domain, amdv1.iommu, domain);	/* [한국어] AMD v1 판도 같은 자리에 겹치는지 확인한다. */

/*
 * [한국어]
 * to_mock_domain - 코어 도메인에서 모의 도메인으로 되짚는다
 *
 * @domain: 코어가 준 도메인.
 * @return: 그것을 품은 모의 도메인.
 *
 * union 의 첫 멤버라 오프셋이 0 이지만, container_of 로 적어 두면
 * 배치가 바뀌어도 코드가 따라간다.
 */
static inline struct mock_iommu_domain *
to_mock_domain(struct iommu_domain *domain)
{
	return container_of(domain, struct mock_iommu_domain, domain);	/* [한국어] union 의 첫 멤버라 오프셋이 0 이지만, container_of 로 적어 두면 배치가 바뀌어도 따라간다. */
}

/*
 * [한국어] 모의 중첩 도메인.
 *
 * 게스트가 만든 페이지 테이블을 흉내 내는 자리다. 진짜로 변환을 하지는
 * 않고, 무효화 명령이 닿았는지 보이게 하는 가짜 IOTLB 배열만 든다.
 */
struct mock_iommu_domain_nested {
	/* [한국어] 코어가 보는 도메인. 중첩 종류로 표시된다.
	 *  설정자: __mock_domain_alloc_nested.
	 *  읽는 자: 코어의 붙이기·무효화 경로. */
	struct iommu_domain domain;
	/* [한국어] 이 도메인을 거느린 vIOMMU(없을 수 있다).
	 *  설정자: mock_viommu_alloc_domain_nested.
	 *  읽는 자: mock_domain_nop_attach 가 이것으로 장치의 가상 번호를 얻는다.
	 *  vIOMMU 없이 만든 중첩 도메인이면 NULL 이다. */
	struct mock_viommu *mock_viommu;
	/* [한국어] 가짜 IOTLB 배열.
	 *  설정자: 생성 때 사용자가 준 값으로 채우고, 무효화가 0 으로 만든다.
	 *  읽는 자: MD_CHECK_IOTLB 명령이 값을 견준다.
	 *  진짜 캐시가 아니라, 무효화 경로가 어디까지 닿았는지 보이게 하는 표식이다. */
	u32 iotlb[MOCK_NESTED_DOMAIN_IOTLB_NUM];
};

/*
 * [한국어]
 * to_mock_nested - 코어 도메인에서 모의 중첩 도메인으로 되짚는다
 *
 * @domain: 코어가 준 도메인.
 * @return: 그것을 품은 모의 중첩 도메인.
 */
static inline struct mock_iommu_domain_nested *
to_mock_nested(struct iommu_domain *domain)
{
	return container_of(domain, struct mock_iommu_domain_nested, domain);	/* [한국어] 중첩 도메인으로 되짚는다. */
}

/*
 * [한국어] 모의 vIOMMU.
 *
 * 게스트가 보는 IOMMU 를 흉내 낸다. 여러 하드웨어 큐를 거느릴 수 있고,
 * 게스트가 mmap 할 수 있는 가짜 레지스터 페이지를 하나 든다.
 */
struct mock_viommu {
	/* [한국어] iommufd 코어가 보는 vIOMMU.
	 *  반드시 첫 멤버여야 코어가 한 덩어리로 할당한 뒤 되짚을 수 있다. */
	struct iommufd_viommu core;
	/* [한국어] 2단계 변환의 바깥쪽 도메인.
	 *  설정자: mock_viommu_init.
	 *  읽는 자: 큐를 세울 때 코어의 주소 변환을 검증하는 데 쓴다. */
	struct mock_iommu_domain *s2_parent;
	/* [한국어] 거느린 하드웨어 큐들. 번호가 곧 첨자다.
	 *  설정자·읽는 자: 큐 생성과 파괴.
	 *  동기화: 아래 queue_mutex 가 지킨다. */
	struct mock_hw_queue *hw_queue[IOMMU_TEST_HW_QUEUE_MAX];
	/* [한국어] 위 배열을 지키는 뮤텍스.
	 *  큐 생성이 앞 번호를 들여다보며 의존을 걸므로, 그 사이 배열이 바뀌면 안 된다. */
	struct mutex queue_mutex;

	/* [한국어] 게스트가 레지스터 창을 mmap 할 때 쓸 오프셋.
	 *  설정자: iommufd_viommu_alloc_mmap 이 배정해 준다.
	 *  읽는 자: 파괴할 때 그 등록을 걷는 데 쓴다.
	 *  값 범위: 0 이면 창을 만들지 않았다는 뜻. */
	unsigned long mmap_offset;
	/* [한국어] (위 영어 주석 참고) 게스트가 볼 가짜 레지스터 페이지.
	 *  설정자: mock_viommu_init 이 두 페이지를 잡아 사용자 값을 써 넣는다.
	 *  읽는 자: 사용자가 mmap 해서 그 값을 읽어 되울림을 확인한다.
	 *  u32 인 것은 in_data 의 타입이 그렇기 때문이다. */
	u32 *page; /* Mmap page to test u32 type of in_data */
};

/*
 * [한국어]
 * to_mock_viommu - 코어 vIOMMU 에서 모의 vIOMMU 로 되짚는다
 *
 * @viommu: 코어가 준 vIOMMU.
 * @return: 그것을 품은 모의 vIOMMU.
 */
static inline struct mock_viommu *to_mock_viommu(struct iommufd_viommu *viommu)
{
	return container_of(viommu, struct mock_viommu, core);	/* [한국어] 모의 vIOMMU 로 되짚는다. */
}

/*
 * [한국어] 모의 하드웨어 큐.
 *
 * 게스트가 직접 명령을 넣는 큐를 흉내 낸다. prev 로 앞 번호의 큐를
 * 가리키는데, 큐끼리 순서 의존이 있는 하드웨어를 흉내 내려는 것이다.
 */
struct mock_hw_queue {
	/* [한국어] 코어가 보는 큐. 반드시 첫 멤버여야 한다. */
	struct iommufd_hw_queue core;
	/* [한국어] 이 큐가 매달린 vIOMMU.
	 *  설정자: mock_hw_queue_init_phys.
	 *  읽는 자: 파괴할 때 배열에서 자기를 빼는 데 쓴다. */
	struct mock_viommu *mock_viommu;
	/* [한국어] 앞 번호의 큐(0번이면 NULL).
	 *  설정자: 큐를 세울 때. 읽는 자: 파괴할 때 의존을 푸는 데.
	 *  큐끼리 순서 의존이 있는 하드웨어를 흉내 내려고 만든 관계다 —
	 *  앞 큐가 먼저 사라질 수 없다. */
	struct mock_hw_queue *prev;
	/* [한국어] 이 큐의 번호.
	 *  값 범위: 0 .. IOMMU_TEST_HW_QUEUE_MAX-1.
	 *  읽는 자: 파괴할 때 배열의 어느 칸인지 알아내는 데. */
	u16 index;
};

/*
 * [한국어]
 * to_mock_hw_queue - 코어 큐에서 모의 큐로 되짚는다
 *
 * @hw_queue: 코어가 준 큐.
 * @return: 그것을 품은 모의 큐.
 */
static inline struct mock_hw_queue *
to_mock_hw_queue(struct iommufd_hw_queue *hw_queue)
{
	return container_of(hw_queue, struct mock_hw_queue, core);	/* [한국어] 모의 큐로 되짚는다. */
}

/* [한국어] 셀프테스트 객체의 종류.
 * 지금은 하나뿐이지만, 아래 union 과 함께 종류를 늘릴 수 있게 짜 두었다. */
enum selftest_obj_type {
	/* [한국어] 모의 장치를 담은 객체.
	 *  지금은 이 종류뿐이지만, 아래 union 과 함께 늘릴 수 있게 짜 두었다.
	 *  읽는 자: 파괴 경로가 이 값으로 무엇을 정리할지 고른다. */
	TYPE_IDEV,
};

/*
 * [한국어] 모의 장치.
 *
 * 진짜 struct device 를 품고 있어 리눅스 장치 모형에 그대로 등록된다.
 * 그 위에 시험에 필요한 상태를 얹었다 — 능력 플래그, 가짜 캐시 배열,
 * 일부러 실패를 내기 위한 표시 등이다.
 */
struct mock_dev {
	/* [한국어] 리눅스 장치 모형의 장치. 반드시 첫 멤버여야 되짚기가 쉽다.
	 *  설정자: mock_dev_create 가 이름·버스·release 를 채워 등록한다.
	 *  읽는 자: 코어의 모든 장치 경로. */
	struct device dev;
	/* [한국어] 이 장치가 매인 vIOMMU(없으면 NULL).
	 *  설정자: 중첩 도메인에 붙을 때 mock_domain_nop_attach 가 세운다.
	 *  읽는 자: 이벤트를 올릴 때 어디로 보낼지 정한다.
	 *  동기화: 아래 viommu_rwsem 이 지킨다. */
	struct mock_viommu *viommu;
	/* [한국어] 위 두 필드를 지키는 락.
	 *  읽기가 훨씬 잦아(이벤트를 올릴 때마다) 읽기·쓰기 세마포어를 쓴다. */
	struct rw_semaphore viommu_rwsem;
	/* [한국어] 이 장치의 성격. MOCK_FLAGS_DEVICE_* 조합.
	 *  설정자: mock_dev_create 가 사용자가 준 값을 그대로 넣는다.
	 *  읽는 자: 능력 조회와 도메인 붙이기 검사. */
	unsigned long flags;
	/* [한국어] 이 장치의 가상 장치 번호(게스트가 아는 번호).
	 *  설정자: 중첩 도메인에 붙을 때 vIOMMU 에서 얻어 온다.
	 *  읽는 자: 이벤트에 실어 보낸다.
	 *  값 범위: 0 이면 아직 vIOMMU 에 등록되지 않았다는 뜻. */
	unsigned long vdev_id;
	/* [한국어] 장치 번호. 이름을 짓는 데 쓴다.
	 *  설정자: mock_dev_create 가 ida 에서 배정받는다.
	 *  읽는 자: release 가 반납한다. */
	int id;
	/* [한국어] 가짜 장치 캐시 배열.
	 *  설정자: 생성 때 정해진 값으로 채우고, vIOMMU 무효화가 0 으로 만든다.
	 *  읽는 자: DEV_CHECK_CACHE 명령이 값을 견준다.
	 *  장치별 캐시를 흉내 내, 무효화가 그 단위까지 닿는지 보이게 한다. */
	u32 cache[MOCK_DEV_CACHE_NUM];
	/* [한국어] PASID 1024 에서 일부러 실패를 낼지 표시.
	 *  설정자·읽는 자: mock_domain_set_dev_pasid_nop 이 호출마다 뒤집는다.
	 *  원자 타입인 것은 여러 스레드가 같은 장치를 다룰 수 있어서다.
	 *  코어가 교체 실패 뒤 옛 도메인으로 되돌리는지 시험하는 장치다. */
	atomic_t pasid_1024_fake_error;
	/* [한국어] 폴트 보고를 켠 도메인의 수.
	 *  설정자: mock_dev_enable_iopf / disable_iopf.
	 *  한 장치가 여러 PASID 로 여러 도메인에 붙을 수 있어, 그 중 하나라도
	 *  폴트를 쓰면 켜져 있어야 한다. */
	unsigned int iopf_refcount;
	/* [한국어] 지금 붙어 있는 도메인.
	 *  설정자: mock_domain_nop_attach.
	 *  읽는 자: 다음 붙이기가 옛 도메인의 폴트 설정을 끄는 데 쓴다. */
	struct iommu_domain *domain;
};

/*
 * [한국어]
 * to_mock_dev - 코어 장치에서 모의 장치로 되짚는다
 *
 * @dev: 코어가 준 장치.
 * @return: 그것을 품은 모의 장치.
 *
 * 이 파일의 거의 모든 콜백이 첫 줄에서 이것을 부른다.
 */
static inline struct mock_dev *to_mock_dev(struct device *dev)
{
	return container_of(dev, struct mock_dev, dev);	/* [한국어] 모의 장치로 되짚는다. */
}

/*
 * [한국어] 셀프테스트가 만든 iommufd 객체.
 *
 * 사용자가 만든 모의 장치를 iommufd 객체 틀에 담아, 정규 객체처럼 id 로
 * 지목하고 지울 수 있게 한다.
 */
struct selftest_obj {
	/* [한국어] iommufd 객체 틀. 참조 관리와 id 배정을 물려받는다.
	 *  반드시 첫 멤버여야 되짚기가 쉽다. */
	struct iommufd_object obj;
	/* [한국어] 이 객체가 무엇을 담고 있는지.
	 *  설정자: 생성 함수. 읽는 자: 파괴 경로와 조회 함수. */
	enum selftest_obj_type type;

	/* [한국어] 종류마다 다른 내용. 지금은 하나뿐이다. */
	union {
		/* [한국어] 모의 장치를 담은 경우. */
		struct {
			/* [한국어] 묶은 뒤 얻은 iommufd 장치 객체.
			 *  이후 붙이기·교체·떼기가 모두 이것을 통해 이뤄진다. */
			struct iommufd_device *idev;
			/* [한국어] 속한 문맥.
			 *  참조를 들지 않는다 — 문맥이 이 객체를 거느리므로 먼저 사라지지 않는다. */
			struct iommufd_ctx *ictx;
			/* [한국어] 그 아래의 모의 장치.
			 *  파괴할 때 이것도 함께 없앤다. */
			struct mock_dev *mock_dev;
		} idev;	/* [한국어] union 의 유일한 멤버. 이름을 붙여 두어 코드에서 sobj->idev.idev 처럼 읽는다. */
	};
};

/*
 * [한국어]
 * to_selftest_obj - iommufd 객체에서 셀프테스트 객체로 되짚는다
 *
 * @obj: 코어가 준 객체.
 * @return: 그것을 품은 셀프테스트 객체.
 */
static inline struct selftest_obj *to_selftest_obj(struct iommufd_object *obj)
{
	return container_of(obj, struct selftest_obj, obj);	/* [한국어] 셀프테스트 객체로 되짚는다. */
}

/*
 * [한국어]
 * mock_domain_nop_attach - 모의 장치를 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙는 장치.
 * @old: 붙어 있던 도메인(없으면 NULL).
 * @return: 0 성공, 음수면 실패.
 *
 * 이름은 nop 이지만 하는 일이 있다. 진짜 하드웨어를 건드리지 않을 뿐,
 * 시험에 필요한 상태를 갱신한다.
 *
 * 더티 추적을 못 하는 장치에 더티 도메인을 붙이려 하면 거절한다 —
 * 그 검사가 코어 쪽에서 제대로 도는지 시험하는 자리다.
 *
 * 중첩 도메인이면 그것을 거느린 vIOMMU 를 장치에 기억시킨다. 나중에
 * 이벤트를 올릴 때 어느 가상 장치인지 알아야 하기 때문이다.
 */
static int mock_domain_nop_attach(struct iommu_domain *domain,
				  struct device *dev, struct iommu_domain *old)
{
	struct mock_dev *mdev = to_mock_dev(dev);	/* [한국어] 모의 장치로 내려간다. */
	struct mock_viommu *new_viommu = NULL;	/* [한국어] 이 도메인을 거느린 vIOMMU(중첩이 아니면 없다). */
	unsigned long vdev_id = 0;	/* [한국어] 그 vIOMMU 가 이 장치에 붙인 가상 번호. */
	int rc;	/* [한국어] 결과 코드. */

	if (domain->dirty_ops && (mdev->flags & MOCK_FLAGS_DEVICE_NO_DIRTY))	/* [한국어] 더티 추적을 못 하는 장치에 더티 도메인을 붙이려 하면 */
		return -EINVAL;	/* [한국어] 거절한다. 코어가 이 검사를 제대로 하는지 시험하는 자리다. */

	iommu_group_mutex_assert(dev);	/* [한국어] 코어가 그룹 뮤텍스를 쥔 채 불러야 아래 상태 갱신이 안전하다. */
	if (domain->type == IOMMU_DOMAIN_NESTED) {	/* [한국어] 중첩 도메인이면 */
		new_viommu = to_mock_nested(domain)->mock_viommu;	/* [한국어] 그것을 거느린 vIOMMU 를 꺼낸다. */
		if (new_viommu) {	/* [한국어] 있으면 */
			rc = iommufd_viommu_get_vdev_id(&new_viommu->core, dev,	/* [한국어] 이 장치의 가상 번호를 얻는다. */
							&vdev_id);
			if (rc)	/* [한국어] 그 vIOMMU 에 등록되지 않은 장치면 */
				return rc;	/* [한국어] 붙일 수 없다. */
		}
	}
	if (new_viommu != mdev->viommu) {	/* [한국어] vIOMMU 가 바뀌면 */
		down_write(&mdev->viommu_rwsem);	/* [한국어] 이벤트 경로가 읽는 값이라 쓰기 락으로 지킨다. */
		mdev->viommu = new_viommu;	/* [한국어] 새 vIOMMU 를 기억한다. */
		mdev->vdev_id = vdev_id;	/* [한국어] 가상 번호도. */
		up_write(&mdev->viommu_rwsem);	/* [한국어] 락 해제. */
	}

	rc = mock_dev_enable_iopf(dev, domain);	/* [한국어] 새 도메인이 폴트를 쓰면 켠다. */
	if (rc)	/* [한국어] 켜지 못했으면 */
		return rc;	/* [한국어] 붙일 수 없다. */

	mock_dev_disable_iopf(dev, mdev->domain);	/* [한국어] 옛 도메인 몫을 끈다. 켜기를 먼저 하는 순서에 주의 — 실패해도 옛 상태가 그대로 남는다. */
	mdev->domain = domain;	/* [한국어] 붙은 도메인을 기억한다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * mock_domain_set_dev_pasid_nop - 모의 장치의 PASID 를 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙는 장치.
 * @pasid: 붙일 PASID.
 * @old: 붙어 있던 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 PASID 1024 의 특별 대접을 설명한다 — 두 번째 호출에서
 * 일부러 실패를 내, 코어가 옛 도메인으로 되돌리는 경로를 시험한다.
 * 교체(replace)가 실패했을 때 아무것도 바뀌지 않아야 한다는 것을
 * 확인하려는 것이다.
 *
 * 원 주석의 경고대로 세 번째 호출은 성공한다 — 표시가 두 번째에서
 * 지워지기 때문이다. 테스트를 쓰는 쪽이 그 순서를 알고 있어야 한다.
 */
static int mock_domain_set_dev_pasid_nop(struct iommu_domain *domain,
					 struct device *dev, ioasid_t pasid,
					 struct iommu_domain *old)
{
	struct mock_dev *mdev = to_mock_dev(dev);	/* [한국어] 모의 장치로 내려간다. */
	int rc;	/* [한국어] 결과 코드. */

	/*
	 * Per the first attach with pasid 1024, set the
	 * mdev->pasid_1024_fake_error. Hence the second call of this op
	 * can fake an error to validate the error path of the core. This
	 * is helpful to test the case in which the iommu core needs to
	 * rollback to the old domain due to driver failure. e.g. replace.
	 * User should be careful about the third call of this op, it shall
	 * succeed since the mdev->pasid_1024_fake_error is cleared in the
	 * second call.
	 */
	if (pasid == 1024) {	/* [한국어] 원 주석대로 이 번호만 특별 대접한다. */
		if (domain->type == IOMMU_DOMAIN_BLOCKED) {	/* [한국어] 차단 도메인으로 붙이는 것은 떼기와 같아 */
			atomic_set(&mdev->pasid_1024_fake_error, 0);	/* [한국어] 표시를 지운다. */
		} else if (atomic_read(&mdev->pasid_1024_fake_error)) {	/* [한국어] 표시가 서 있으면 이번이 두 번째 호출이다. */
			/*
			 * Clear the flag, and fake an error to fail the
			 * replacement.
			 */
			atomic_set(&mdev->pasid_1024_fake_error, 0);	/* [한국어] 표시를 지우고 */
			return -ENOMEM;	/* [한국어] 일부러 실패한다. 코어가 옛 도메인으로 되돌리는지 보려는 것이다. */
		} else {
			/* Set the flag to fake an error in next call */
			atomic_set(&mdev->pasid_1024_fake_error, 1);	/* [한국어] 첫 호출이면 다음번에 실패하도록 표시해 둔다. */
		}
	}

	rc = mock_dev_enable_iopf(dev, domain);	/* [한국어] 새 도메인이 폴트를 쓰면 켠다. */
	if (rc)	/* [한국어] 실패하면 */
		return rc;	/* [한국어] 붙일 수 없다. */

	mock_dev_disable_iopf(dev, old);	/* [한국어] 옛 도메인 몫을 끈다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어] 차단 도메인의 연산표.
 *
 * 차단 도메인은 그 장치의 모든 DMA 를 막는다. 붙이기만 하면 되고
 * 매핑 연산이 없어 표가 이렇게 짧다.
 */
static const struct iommu_domain_ops mock_blocking_ops = {
	.attach_dev = mock_domain_nop_attach,	/* [한국어] 붙이기만 있으면 된다 — 차단 도메인에는 매핑이 없다. */
	.set_dev_pasid = mock_domain_set_dev_pasid_nop
};

/*
 * [한국어] 하나뿐인 차단 도메인.
 *
 * 정적으로 두는 이유: 내용이 없어 장치마다 만들 이유가 없고, 할당이
 * 없으니 차단은 결코 실패하지 않는다. DMA 를 막는 일이 메모리 부족으로
 * 실패하면 안 되기 때문에 중요한 성질이다.
 */
static struct iommu_domain mock_blocking_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,	/* [한국어] 이 종류가 곧 "모든 DMA 를 막는다"는 뜻이다. */
	.ops = &mock_blocking_ops,
};

/*
 * [한국어]
 * mock_domain_hw_info - 모의 하드웨어 정보를 만들어 준다
 *
 * @dev: 물어보는 장치.
 * @length: 만든 정보의 길이를 여기에 쓴다.
 * @type: 들어올 때는 사용자가 원한 형식, 나갈 때는 실제 형식.
 * @return: 만든 정보 버퍼, 실패하면 오류 포인터.
 *
 * 값에는 뜻이 없다. 정보가 사용자 공간까지 온전히 전해지는지 보는 것이
 * 목적이라, 정해진 매직 값을 넣어 준다.
 *
 * 호출자가 이 버퍼를 해제한다.
 */
static void *mock_domain_hw_info(struct device *dev, u32 *length,
				 enum iommu_hw_info_type *type)
{
	struct iommu_test_hw_info *info;	/* [한국어] 만들 정보 버퍼. */

	if (*type != IOMMU_HW_INFO_TYPE_DEFAULT &&	/* [한국어] 사용자가 다른 형식을 지정했으면 */
	    *type != IOMMU_HW_INFO_TYPE_SELFTEST)	/* [한국어] 우리 형식도 아니고 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 줄 수 있는 것이 없다. */

	info = kzalloc_obj(*info);	/* [한국어] 호출자가 해제할 버퍼. */
	if (!info)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */

	info->test_reg = IOMMU_HW_INFO_SELFTEST_REGVAL;	/* [한국어] 값에 뜻은 없다. 전달 경로가 온전한지 보는 표식이다. */
	*length = sizeof(*info);	/* [한국어] 커널이 아는 전체 길이를 알린다. */
	*type = IOMMU_HW_INFO_TYPE_SELFTEST;	/* [한국어] 실제로 준 형식을 알린다. */

	return info;	/* [한국어] 만든 버퍼. */
}

/*
 * [한국어]
 * mock_domain_set_dirty_tracking - 더티 추적을 켜거나 끈다
 *
 * @domain: 대상 도메인.
 * @enable: 켤 것인가.
 * @return: 0 성공, 음수면 실패.
 *
 * 진짜 하드웨어라면 레지스터를 건드릴 자리다. 여기서는 플래그만 바꾼다.
 *
 * 이미 그 상태면 아무것도 하지 않고 성공한다 — 코어가 같은 요청을
 * 두 번 보내도 되게 하려는 것이다.
 */
static int mock_domain_set_dirty_tracking(struct iommu_domain *domain,
					  bool enable)
{
	struct mock_iommu_domain *mock = to_mock_domain(domain);	/* [한국어] 모의 도메인으로 내려간다. */
	unsigned long flags = mock->flags;	/* [한국어] 지금 상태. */

	if (enable && !domain->dirty_ops)	/* [한국어] 더티 연산표가 없는 도메인에 켜라는 것은 */
		return -EINVAL;	/* [한국어] 앞뒤가 맞지 않는다. */

	/* No change? */
	if (!(enable ^ !!(flags & MOCK_DIRTY_TRACK)))	/* [한국어] 이미 그 상태면. 배타적 논리합이 0 이라는 것은 두 값이 같다는 뜻이다. */
		return 0;	/* [한국어] 아무것도 하지 않고 성공한다 — 코어가 같은 요청을 두 번 보내도 되게. */

	flags = (enable ? flags | MOCK_DIRTY_TRACK : flags & ~MOCK_DIRTY_TRACK);	/* [한국어] 비트를 세우거나 지운다. */

	mock->flags = flags;	/* [한국어] 상태를 갱신한다. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * __mock_domain_alloc_nested - 모의 중첩 도메인을 만든다
 *
 * @user_data: 사용자가 준 드라이버 고유 데이터.
 * @return: 만든 도메인, 실패하면 오류 포인터.
 *
 * 사용자가 준 값으로 가짜 IOTLB 배열을 채운다. 무효화가 닿으면 그 값이
 * 바뀌므로, 테스트가 그것을 확인해 무효화 경로를 판정한다.
 *
 * 형식 번호가 다르면 거절한다 — 모르는 형식의 데이터를 해석하면 안 된다.
 */
static struct mock_iommu_domain_nested *
__mock_domain_alloc_nested(const struct iommu_user_data *user_data)
{
	struct mock_iommu_domain_nested *mock_nested;	/* [한국어] 만들 중첩 도메인. */
	struct iommu_hwpt_selftest user_cfg;	/* [한국어] 사용자가 준 설정. */
	int rc, i;	/* [한국어] 결과 코드와 순회용 첨자. */

	if (user_data->type != IOMMU_HWPT_DATA_SELFTEST)	/* [한국어] 모르는 형식의 데이터를 해석하면 안 된다. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절. */

	rc = iommu_copy_struct_from_user(&user_cfg, user_data,	/* [한국어] 크기 차이를 코어가 다뤄 준다 — 짧으면 0 으로 채우고 길면 남는 부분이 0 인지 본다. */
					 IOMMU_HWPT_DATA_SELFTEST, iotlb);
	if (rc)	/* [한국어] 복사에 실패하면 */
		return ERR_PTR(rc);	/* [한국어] 그대로 올린다. */

	mock_nested = kzalloc_obj(*mock_nested);	/* [한국어] 중첩 도메인을 만든다. */
	if (!mock_nested)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */
	mock_nested->domain.ops = &domain_nested_ops;	/* [한국어] 중첩 전용 연산표. 매핑 연산이 없다. */
	mock_nested->domain.type = IOMMU_DOMAIN_NESTED;	/* [한국어] 코어가 이 종류를 보고 다르게 다룬다. */
	for (i = 0; i < MOCK_NESTED_DOMAIN_IOTLB_NUM; i++)	/* [한국어] 가짜 IOTLB 네 칸을 */
		mock_nested->iotlb[i] = user_cfg.iotlb;	/* [한국어] 사용자가 준 값으로 채운다. 무효화가 닿으면 0 이 된다. */
	return mock_nested;	/* [한국어] 만든 도메인. */
}

/*
 * [한국어]
 * mock_domain_alloc_nested - 중첩 도메인 할당 콜백
 *
 * @dev: 요청한 장치.
 * @parent: 부모가 될 페이징 도메인(2단계 변환의 바깥쪽).
 * @flags: 할당 플래그.
 * @user_data: 드라이버 고유 데이터.
 * @return: 만든 도메인, 실패하면 오류 포인터.
 *
 * 부모가 반드시 페이징 도메인이어야 한다는 것을 확인한다. 중첩 변환은
 * 게스트 표를 호스트 표로 한 번 더 변환하는 구조라, 바깥쪽이 필요하다.
 */
static struct iommu_domain *
mock_domain_alloc_nested(struct device *dev, struct iommu_domain *parent,
			 u32 flags, const struct iommu_user_data *user_data)
{
	struct mock_iommu_domain_nested *mock_nested;	/* [한국어] 만들 중첩 도메인. */
	struct mock_iommu_domain *mock_parent;	/* [한국어] 부모 페이징 도메인. */

	if (flags & ~IOMMU_HWPT_ALLOC_PASID)	/* [한국어] 아는 플래그는 하나뿐이다. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절. */
	if (!parent || !(parent->type & __IOMMU_DOMAIN_PAGING))	/* [한국어] 부모가 없거나 페이징 도메인이 아니면 */
		return ERR_PTR(-EINVAL);	/* [한국어] 중첩 변환이 성립하지 않는다. */

	mock_parent = to_mock_domain(parent);	/* [한국어] 부모를 모의 도메인으로 본다. */
	if (!mock_parent)	/* [한국어] 있을 수 없는 경우지만 확인해 둔다. */
		return ERR_PTR(-EINVAL);	/* [한국어] 거절. */

	mock_nested = __mock_domain_alloc_nested(user_data);	/* [한국어] 실제 생성. */
	if (IS_ERR(mock_nested))	/* [한국어] 실패하면 */
		return ERR_CAST(mock_nested);	/* [한국어] 오류를 그대로 올린다. */
	return &mock_nested->domain;	/* [한국어] 코어가 보는 도메인을 돌려준다. */
}

/*
 * [한국어]
 * mock_domain_free - 모의 페이징 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 *
 * generic_pt 가 잡아 둔 페이지 테이블을 먼저 걷어 낸다.
 */
static void mock_domain_free(struct iommu_domain *domain)
{
	struct mock_iommu_domain *mock = to_mock_domain(domain);	/* [한국어] 모의 도메인으로 내려간다. */

	pt_iommu_deinit(&mock->iommu);	/* [한국어] generic_pt 가 잡아 둔 페이지 테이블 페이지를 모두 걷어 낸다. */
	kfree(mock);	/* [한국어] 구조체를 해제한다. */
}

/*
 * [한국어]
 * mock_iotlb_sync - 모아 둔 해제 페이지를 실제로 놓는다
 *
 * @domain: 대상 도메인.
 * @gather: 코어가 모아 둔 해제 목록.
 *
 * 진짜 하드웨어라면 여기서 IOTLB 를 비우고 그것이 끝난 뒤에 페이지를
 * 놓아야 한다. 모의 하드웨어는 캐시가 없어 곧바로 놓는다.
 *
 * 이 콜백이 없으면 페이지 테이블 페이지가 샌다.
 */
static void mock_iotlb_sync(struct iommu_domain *domain,
				struct iommu_iotlb_gather *gather)
{
	iommu_put_pages_list(&gather->freelist);	/* [한국어] 모아 둔 페이지 테이블 페이지를 놓는다. 진짜 하드웨어라면 IOTLB 를 비운 뒤에 해야 하지만, 모의 하드웨어는 캐시가 없다. */
}

/*
 * [한국어] 작은 페이지만 쓰는 모의 도메인의 연산표.
 *
 * IOMMU_PT_DOMAIN_OPS 매크로가 generic_pt 의 map/unmap 구현을 통째로
 * 채워 넣는다. 그 아래 몇 줄만 이 파일 것이다.
 */
static const struct iommu_domain_ops amdv1_mock_ops = {
	IOMMU_PT_DOMAIN_OPS(amdv1_mock),	/* [한국어] generic_pt 가 찍어 낸 매핑·해제·조회 함수들을 통째로 채운다. */
	.free = mock_domain_free,
	.attach_dev = mock_domain_nop_attach,
	.set_dev_pasid = mock_domain_set_dev_pasid_nop,
	.iotlb_sync = &mock_iotlb_sync,
};

/*
 * [한국어] 큰 페이지를 쓰는 모의 도메인의 연산표.
 *
 * 위와 내용이 같아 보이지만, 그 사이의 #undef 가 매크로가 채워 넣는
 * 함수 하나를 바꿔 놓는다 — 큰 페이지 전용 매핑 함수를 쓰게 된다.
 */
static const struct iommu_domain_ops amdv1_mock_huge_ops = {
	IOMMU_PT_DOMAIN_OPS(amdv1_mock),	/* [한국어] 같은 매크로지만, 아래 #undef 가 그 중 한 함수를 큰 페이지 판으로 바꿔 놓는다. */
	.free = mock_domain_free,
	.attach_dev = mock_domain_nop_attach,
	.set_dev_pasid = mock_domain_set_dev_pasid_nop,
	.iotlb_sync = &mock_iotlb_sync,
};
#undef pt_iommu_amdv1_mock_map_pages	/* [한국어] 매크로가 가리키던 매핑 함수의 정의를 지운다. 위 두 표가 서로 다른 함수를 갖게 되는 수법이다. */

/*
 * [한국어] 더티 추적 연산표.
 *
 * 비트를 읽고 지우는 부분은 generic_pt 것을 그대로 쓰고, 켜고 끄는
 * 것만 이 파일이 맡는다.
 */
static const struct iommu_dirty_ops amdv1_mock_dirty_ops = {
	IOMMU_PT_DIRTY_OPS(amdv1_mock),	/* [한국어] 더티 비트를 읽고 지우는 부분은 generic_pt 것을 그대로 쓴다. */
	.set_dirty_tracking = mock_domain_set_dirty_tracking,
};

/*
 * [한국어]
 * mock_domain_alloc_pgtable - 모의 페이징 도메인을 만든다
 *
 * @dev: 요청한 장치.
 * @user_cfg: 사용자가 고른 페이지 테이블 형식 등.
 * @flags: 할당 플래그.
 * @return: 만든 도메인, 실패하면 오류 포인터.
 *
 * generic_pt 의 AMD v1 구현을 실제로 초기화한다. 진짜 페이지 테이블
 * 코드를 쓰므로 그 부분까지 함께 시험된다.
 *
 * 원 주석대로 페이지 크기를 2KB 로 잡는 것이 요점이다. 호스트 페이지보다
 * 작은 IOMMU 페이지는 계산이 까다로운데, 그 상황을 늘 겪게 하려는 것이다.
 *
 * 큰 페이지 모드에서 PAGE_SIZE 를 함께 넣는 이유도 원 주석이 밝힌다 —
 * 그러지 않으면 iommufd 가 그 도메인을 받아 주지 않는다.
 *
 * 마지막에 주소 공간 범위를 덮어쓰는 것은 시험용이다. 0 이 아닌 시작
 * 주소를 강제해, 사용자가 0 을 유효한 주소로 착각하지 않게 한다.
 */
static struct mock_iommu_domain *
mock_domain_alloc_pgtable(struct device *dev,
			  const struct iommu_hwpt_selftest *user_cfg, u32 flags)
{
	struct mock_iommu_domain *mock;	/* [한국어] 만들 도메인. */
	int rc;	/* [한국어] 결과 코드. */

	mock = kzalloc_obj(*mock);	/* [한국어] 도메인 구조체를 잡는다. */
	if (!mock)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */
	mock->domain.type = IOMMU_DOMAIN_UNMANAGED;	/* [한국어] 사용자가 매핑을 직접 관리하는 종류. 커널 DMA API 가 쓰는 종류와 다르다. */

	mock->amdv1.iommu.nid = NUMA_NO_NODE;	/* [한국어] 페이지 테이블 페이지를 어느 노드에서 잡을지 정하지 않는다. */

	switch (user_cfg->pagetable_type) {	/* [한국어] 사용자가 고른 형식. */
	case MOCK_IOMMUPT_DEFAULT:	/* [한국어] 작은 페이지만. */
	case MOCK_IOMMUPT_HUGE: {	/* [한국어] 큰 페이지까지. 초기화는 같고 연산표만 다르다. */
		struct pt_iommu_amdv1_cfg cfg = {};	/* [한국어] generic_pt 에 넘길 설정. */

		/* The mock version has a 2k page size */
		cfg.common.hw_max_vasz_lg2 = 56;	/* [한국어] 흉내 낼 가상 주소 폭. AMD v1 의 실제 값과 맞춘다. */
		cfg.common.hw_max_oasz_lg2 = 51;	/* [한국어] 출력(물리) 주소 폭. */
		cfg.starting_level = 2;	/* [한국어] 시작 단계. 이 값이 곧 2KB 페이지 크기를 만든다 — 원 주석이 말하는 "모의 판은 2k 페이지"다. */
		if (user_cfg->pagetable_type == MOCK_IOMMUPT_HUGE)	/* [한국어] 큰 페이지 형식이면 */
			mock->domain.ops = &amdv1_mock_huge_ops;	/* [한국어] 그 연산표를 쓴다. */
		else
			mock->domain.ops = &amdv1_mock_ops;	/* [한국어] 아니면 보통 연산표. */
		rc = pt_iommu_amdv1_mock_init(&mock->amdv1, &cfg, GFP_KERNEL);	/* [한국어] 진짜 페이지 테이블 구현을 초기화한다. 이 덕에 그 코드까지 함께 시험된다. */
		if (rc)	/* [한국어] 페이지 테이블 초기화에 실패했다. */
			goto err_free;	/* [한국어] 실패하면 구조체를 버린다. */

		/*
		 * In huge mode userspace should only provide huge pages, we
		 * have to include PAGE_SIZE for the domain to be accepted by
		 * iommufd.
		 */
		if (user_cfg->pagetable_type == MOCK_IOMMUPT_HUGE)	/* [한국어] 원 주석대로 큰 페이지 모드에서도 */
			mock->domain.pgsize_bitmap = MOCK_HUGE_PAGE_SIZE |	/* [한국어] PAGE_SIZE 를 함께 넣어야 */
						     PAGE_SIZE;	/* [한국어] iommufd 가 이 도메인을 받아 준다. */
		if (flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING)	/* [한국어] 더티 추적을 요청했으면 */
			mock->domain.dirty_ops = &amdv1_mock_dirty_ops;	/* [한국어] 그 연산표를 단다. */
		break;	/* [한국어] 형식 처리를 마쳤다. */
	}
	default:	/* [한국어] 모르는 형식. */
		rc = -EOPNOTSUPP;	/* [한국어] 지원하지 않는다. */
		goto err_free;	/* [한국어] 구조체를 버린다. */
	}

	/*
	 * Override the real aperture to the MOCK aperture for test purposes.
	 */
	if (user_cfg->pagetable_type == MOCK_IOMMUPT_DEFAULT) {	/* [한국어] 기본 형식일 때만 */
		WARN_ON(mock->domain.geometry.aperture_start != 0);	/* [한국어] generic_pt 는 0 부터라고 답할 것이다. */
		WARN_ON(mock->domain.geometry.aperture_end < MOCK_APERTURE_LAST);	/* [한국어] 우리가 쓸 범위를 덮을 만큼 넓어야 한다. */

		mock->domain.geometry.aperture_start = MOCK_APERTURE_START;	/* [한국어] 시작을 0 이 아닌 값으로 덮어쓴다 — 사용자가 0 을 유효한 주소로 착각하지 않게 하려는 것이다. */
		mock->domain.geometry.aperture_end = MOCK_APERTURE_LAST;	/* [한국어] 끝도 좁혀, 범위 밖 요청이 거절되는지 시험할 수 있게 한다. */
	}

	return mock;	/* [한국어] 만든 도메인. */
err_free:	/* [한국어] 실패 경로. */
	kfree(mock);	/* [한국어] 구조체를 해제한다. */
	return ERR_PTR(rc);	/* [한국어] 오류를 올린다. */
}

/*
 * [한국어]
 * mock_domain_alloc_paging_flags - 페이징 도메인 할당 콜백
 *
 * @dev: 요청한 장치.
 * @flags: 할당 플래그.
 * @user_data: 드라이버 고유 데이터(없을 수 있다).
 * @return: 만든 도메인, 실패하면 오류 포인터.
 *
 * 코어가 부르는 진입점이다. 플래그와 데이터를 검사한 뒤 위 함수에
 * 넘긴다.
 *
 * 더티 추적을 못 하는 장치에 그 플래그를 주면 거절한다 — 코어가 장치
 * 능력을 제대로 보는지 시험하는 자리다.
 */
static struct iommu_domain *
mock_domain_alloc_paging_flags(struct device *dev, u32 flags,
			       const struct iommu_user_data *user_data)
{
	bool has_dirty_flag = flags & IOMMU_HWPT_ALLOC_DIRTY_TRACKING;	/* [한국어] 더티 추적을 요청했는가. */
	const u32 PAGING_FLAGS = IOMMU_HWPT_ALLOC_DIRTY_TRACKING |	/* [한국어] 받아들이는 플래그의 전부. */
				 IOMMU_HWPT_ALLOC_NEST_PARENT |
				 IOMMU_HWPT_ALLOC_PASID;
	struct mock_dev *mdev = to_mock_dev(dev);	/* [한국어] 모의 장치로 내려간다. */
	bool no_dirty_ops = mdev->flags & MOCK_FLAGS_DEVICE_NO_DIRTY;	/* [한국어] 이 장치가 더티 추적을 못 하는가. */
	struct iommu_hwpt_selftest user_cfg = {};	/* [한국어] 사용자 설정. 주지 않았을 때를 위해 0 으로. */
	struct mock_iommu_domain *mock;	/* [한국어] 만들 도메인. */
	int rc;	/* [한국어] 결과 코드. */

	if ((flags & ~PAGING_FLAGS) || (has_dirty_flag && no_dirty_ops))	/* [한국어] 모르는 플래그이거나, 못 하는 장치에 더티를 요구하면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절한다. 뒤쪽이 코어의 능력 검사를 시험하는 자리다. */

	if (user_data && (user_data->type != IOMMU_HWPT_DATA_SELFTEST &&	/* [한국어] 데이터를 줬는데 우리 형식도 아니고 */
			  user_data->type != IOMMU_HWPT_DATA_NONE))	/* [한국어] 없음도 아니면 */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 해석할 수 없다. */

	if (user_data) {	/* [한국어] 데이터를 줬으면 */
		rc = iommu_copy_struct_from_user(	/* [한국어] 설정을 가져온다. */
			&user_cfg, user_data, IOMMU_HWPT_DATA_SELFTEST, iotlb);
		if (rc)	/* [한국어] 사용자 설정을 가져오지 못했다. */
			return ERR_PTR(rc);	/* [한국어] 실패하면 그대로 올린다. */
	}

	mock = mock_domain_alloc_pgtable(dev, &user_cfg, flags);	/* [한국어] 실제 생성. */
	if (IS_ERR(mock))	/* [한국어] 실패하면 */
		return ERR_CAST(mock);	/* [한국어] 오류를 올린다. */
	return &mock->domain;	/* [한국어] 코어가 보는 도메인. */
}

/*
 * [한국어]
 * mock_domain_capable - 이 장치가 그 능력을 갖췄는지 답한다
 *
 * @dev: 물어보는 장치.
 * @cap: 물어보는 능력.
 * @return: 갖췄으면 참.
 *
 * 캐시 일관성은 늘 참이다 — 그러지 않으면 iommufd 가 이 장치를 아예
 * 받지 않는다.
 *
 * 더티 추적은 장치를 만들 때 준 플래그에 따라 갈린다. 두 성격의 장치를
 * 만들어 각 경로를 시험할 수 있게 하려는 것이다.
 */
static bool mock_domain_capable(struct device *dev, enum iommu_cap cap)
{
	struct mock_dev *mdev = to_mock_dev(dev);	/* [한국어] 모의 장치로 내려간다. */

	switch (cap) {	/* [한국어] 물어보는 능력. */
	case IOMMU_CAP_CACHE_COHERENCY:	/* [한국어] 캐시 일관성. */
		return true;	/* [한국어] 늘 참이다. 그러지 않으면 iommufd 가 이 장치를 아예 받지 않는다. */
	case IOMMU_CAP_DIRTY_TRACKING:	/* [한국어] 더티 추적. */
		return !(mdev->flags & MOCK_FLAGS_DEVICE_NO_DIRTY);	/* [한국어] 장치를 만들 때 준 플래그에 달렸다. 두 성격을 갈라 각 경로를 시험한다. */
	default:	/* [한국어] 모르는 능력. */
		break;	/* [한국어] 아래에서 거짓을 돌려준다. */
	}

	return false;	/* [한국어] 갖추지 않았다. */
}

/*
 * [한국어] 모의 장치들이 공유하는 폴트 큐.
 *
 * 진짜 하드웨어라면 IOMMU 마다 하나씩 있다. 여기서는 모의 IOMMU 가
 * 하나뿐이라 전역으로 둔다.
 */
static struct iopf_queue *mock_iommu_iopf_queue;

/*
 * [한국어] 하나뿐인 모의 IOMMU 장치.
 *
 * completion 과 참조 수를 함께 둔 것이 요점이다 — 모듈을 내릴 때 아직
 * 이 IOMMU 를 쓰는 장치가 남아 있으면 기다려야 한다.
 */
static struct mock_iommu_device {
	/* [한국어] 코어가 보는 IOMMU 장치.
	 *  설정자: iommufd_test_init 이 등록한다.
	 *  읽는 자: probe 가 이 주소를 돌려주어 장치를 이 IOMMU 아래로 넣는다. */
	struct iommu_device iommu_dev;
	/* [한국어] 사용자가 모두 사라졌음을 알리는 수단.
	 *  설정자: 마지막 vIOMMU 가 파괴될 때 신호를 보낸다.
	 *  읽는 자: 모듈을 내릴 때 기다리는 쪽. */
	struct completion complete;
	/* [한국어] 이 IOMMU 를 쓰는 vIOMMU 의 수 + 기준 참조 하나.
	 *  설정자: vIOMMU 생성·파괴와 모듈 초기화·정리.
	 *  0 이 되면 위 completion 이 울린다. */
	refcount_t users;
} mock_iommu;	/* [한국어] 하나뿐인 모의 IOMMU. 무명 구조체로 정의와 선언을 함께 한다. */

/*
 * [한국어]
 * mock_probe_device - 이 장치를 맡을 IOMMU 를 알려 준다
 *
 * @dev: 검사할 장치.
 * @return: 모의 IOMMU, 우리 장치가 아니면 오류 포인터.
 *
 * 가짜 버스에 속한 장치만 맡는다. 진짜 장치를 실수로 가로채면 안 된다.
 */
static struct iommu_device *mock_probe_device(struct device *dev)
{
	if (dev->bus != &iommufd_mock_bus_type.bus)	/* [한국어] 가짜 버스의 장치가 아니면 */
		return ERR_PTR(-ENODEV);	/* [한국어] 우리가 맡지 않는다. 진짜 장치를 실수로 가로채면 안 된다. */
	return &mock_iommu.iommu_dev;	/* [한국어] 하나뿐인 모의 IOMMU 를 알려 준다. */
}

/*
 * [한국어]
 * mock_domain_page_response - 폴트 응답을 하드웨어에 전한다
 *
 * @dev: 대상 장치.
 * @evt: 응답할 폴트.
 * @msg: 응답 내용.
 *
 * 진짜 하드웨어라면 레지스터에 응답을 써 장치를 깨울 자리다. 모의
 * 장치는 기다리고 있지 않으므로 할 일이 없다.
 *
 * 그래도 이 콜백이 있어야 폴트 큐를 쓸 수 있어, 빈 채로 둔다.
 */
static void mock_domain_page_response(struct device *dev, struct iopf_fault *evt,
				      struct iommu_page_response *msg)
{
}

/*
 * [한국어]
 * mock_dev_enable_iopf - 이 장치의 폴트 보고를 켠다
 *
 * @dev: 대상 장치.
 * @domain: 붙는 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 폴트 큐가 달린 도메인에 붙을 때만 켠다.
 *
 * 참조를 세는 이유: 한 장치가 여러 PASID 로 여러 도메인에 붙을 수 있고,
 * 그 중 하나라도 폴트를 쓰면 켜져 있어야 한다.
 */
static int mock_dev_enable_iopf(struct device *dev, struct iommu_domain *domain)
{
	struct mock_dev *mdev = to_mock_dev(dev);	/* [한국어] 모의 장치로 내려간다. */
	int ret;	/* [한국어] 결과 코드. */

	if (!domain || !domain->iopf_handler)	/* [한국어] 폴트를 다루지 않는 도메인이면 */
		return 0;	/* [한국어] 켤 이유가 없다. */

	if (!mock_iommu_iopf_queue)	/* [한국어] 폴트 큐가 없으면 */
		return -ENODEV;	/* [한국어] 켤 수 없다. */

	if (mdev->iopf_refcount) {	/* [한국어] 이미 켜져 있으면 */
		mdev->iopf_refcount++;	/* [한국어] 참조만 늘린다. 한 장치가 여러 PASID 로 여러 도메인에 붙을 수 있다. */
		return 0;	/* [한국어] 성공. */
	}

	ret = iopf_queue_add_device(mock_iommu_iopf_queue, dev);	/* [한국어] 이 장치를 폴트 큐에 등록한다. */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 그대로 올린다. */

	mdev->iopf_refcount = 1;	/* [한국어] 첫 참조. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * mock_dev_disable_iopf - 이 장치의 폴트 보고를 끈다
 *
 * @dev: 대상 장치.
 * @domain: 떨어지는 도메인.
 *
 * 마지막 참조가 사라질 때만 실제로 끈다.
 */
static void mock_dev_disable_iopf(struct device *dev, struct iommu_domain *domain)
{
	struct mock_dev *mdev = to_mock_dev(dev);	/* [한국어] 모의 장치로 내려간다. */

	if (!domain || !domain->iopf_handler)	/* [한국어] 폴트를 쓰지 않던 도메인이면 */
		return;	/* [한국어] 끌 것이 없다. */

	if (--mdev->iopf_refcount)	/* [한국어] 참조를 줄인 뒤에도 쓰는 곳이 남았으면 */
		return;	/* [한국어] 켜 둔 채로 둔다. */

	iopf_queue_remove_device(mock_iommu_iopf_queue, dev);	/* [한국어] 마지막이면 큐에서 뺀다. */
}

/*
 * [한국어]
 * mock_viommu_destroy - 모의 vIOMMU 를 파괴한다
 *
 * @viommu: 파괴할 vIOMMU.
 *
 * 게스트에게 보여 주던 가짜 레지스터 페이지를 놓는다.
 *
 * 코어가 구조체 자체는 해제하므로 여기서는 딸린 것만 정리한다.
 */
static void mock_viommu_destroy(struct iommufd_viommu *viommu)
{
	struct mock_iommu_device *mock_iommu = container_of(	/* [한국어] vIOMMU 가 매달린 IOMMU 장치로 되짚는다. */
		viommu->iommu_dev, struct mock_iommu_device, iommu_dev);
	struct mock_viommu *mock_viommu = to_mock_viommu(viommu);	/* [한국어] 모의 vIOMMU 로 내려간다. */

	if (refcount_dec_and_test(&mock_iommu->users))	/* [한국어] 마지막 사용자가 사라졌으면 */
		complete(&mock_iommu->complete);	/* [한국어] 모듈을 내리려고 기다리는 쪽을 깨운다. */
	if (mock_viommu->mmap_offset)	/* [한국어] 게스트에게 창을 열어 줬으면 */
		iommufd_viommu_destroy_mmap(&mock_viommu->core,	/* [한국어] 그 오프셋 등록을 걷는다. 이후 사용자가 mmap 할 수 없다. */
					    mock_viommu->mmap_offset);
	free_pages((unsigned long)mock_viommu->page, 1);	/* [한국어] 가짜 레지스터 페이지 두 장을 놓는다. */
	mutex_destroy(&mock_viommu->queue_mutex);	/* [한국어] 큐 목록을 지키던 뮤텍스. */

	/* iommufd core frees mock_viommu and viommu */
}

/*
 * [한국어]
 * mock_viommu_alloc_domain_nested - vIOMMU 아래에 중첩 도메인을 만든다
 *
 * @viommu: 거느릴 vIOMMU.
 * @flags: 할당 플래그.
 * @user_data: 드라이버 고유 데이터.
 * @return: 만든 도메인, 실패하면 오류 포인터.
 *
 * 도메인이 vIOMMU 를 기억하는 것이 요점이다. 그래야 이벤트를 올릴 때
 * 어느 가상 장치인지 알 수 있다.
 */
static struct iommu_domain *
mock_viommu_alloc_domain_nested(struct iommufd_viommu *viommu, u32 flags,
				const struct iommu_user_data *user_data)
{
	struct mock_viommu *mock_viommu = to_mock_viommu(viommu);	/* [한국어] 모의 vIOMMU 로 내려간다. */
	struct mock_iommu_domain_nested *mock_nested;	/* [한국어] 만들 중첩 도메인. */

	if (flags & ~IOMMU_HWPT_ALLOC_PASID)	/* [한국어] 아는 플래그는 하나뿐이다. */
		return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 거절. */

	mock_nested = __mock_domain_alloc_nested(user_data);	/* [한국어] 실제 생성. */
	if (IS_ERR(mock_nested))	/* [한국어] 실패하면 */
		return ERR_CAST(mock_nested);	/* [한국어] 오류를 올린다. */
	mock_nested->mock_viommu = mock_viommu;	/* [한국어] 거느린 vIOMMU 를 기억한다. 이 도메인에 붙는 장치가 그것으로 가상 번호를 얻는다. */
	return &mock_nested->domain;	/* [한국어] 코어가 보는 도메인. */
}

/*
 * [한국어]
 * mock_viommu_cache_invalidate - vIOMMU 단위 무효화 명령을 처리한다
 *
 * @viommu: 대상 vIOMMU.
 * @array: 사용자가 준 명령 배열. 처리한 개수를 여기에 되돌려 쓴다.
 * @return: 0 성공, 음수면 실패.
 *
 * 게스트가 자기 IOMMU 에 낸 무효화 명령을 호스트가 받아 처리하는 경로다.
 * 여러 명령을 배열로 한 번에 받는 것이 요점 — ioctl 왕복을 줄인다.
 *
 * 길이 0 을 허용하는 이유를 원 주석이 밝힌다: 사용자가 이 형식을
 * 지원하는지 미리 물어볼 수 있게 한다.
 *
 * 실패하면 처리한 개수를 돌려주어, 사용자가 어디까지 됐는지 알고 나머지를
 * 다시 보낼 수 있다.
 *
 * 가상 장치 id 를 실제 장치로 옮기는 과정이 이 함수의 핵심이다 — 게스트는
 * 자기가 붙인 번호만 알고 호스트의 장치는 모른다.
 */
static int mock_viommu_cache_invalidate(struct iommufd_viommu *viommu,
					struct iommu_user_data_array *array)
{
	struct iommu_viommu_invalidate_selftest *cmds;	/* [한국어] 사용자에게서 가져올 명령 배열. */
	struct iommu_viommu_invalidate_selftest *cur;	/* [한국어] 지금 처리 중인 명령. */
	struct iommu_viommu_invalidate_selftest *end;	/* [한국어] 배열의 끝. */
	int rc;	/* [한국어] 결과 코드. */

	/* A zero-length array is allowed to validate the array type */
	if (array->entry_num == 0 &&	/* [한국어] 원 주석대로 길이 0 은 허용한다 — */
	    array->type == IOMMU_VIOMMU_INVALIDATE_DATA_SELFTEST) {	/* [한국어] 사용자가 이 형식을 지원하는지 미리 물어보는 방법이다. */
		array->entry_num = 0;	/* [한국어] 처리한 개수도 0. */
		return 0;	/* [한국어] 성공. */
	}

	cmds = kzalloc_objs(*cmds, array->entry_num);	/* [한국어] 명령 배열을 한 번에 잡는다. */
	if (!cmds)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */
	cur = cmds;	/* [한국어] 처음부터. */
	end = cmds + array->entry_num;	/* [한국어] 끝 지점. */

	static_assert(sizeof(*cmds) == 3 * sizeof(u32));	/* [한국어] 구조체에 채움 바이트가 없어야 사용자 배열과 배치가 같다. */
	rc = iommu_copy_struct_from_full_user_array(	/* [한국어] 배열 전체를 한 번에 가져온다. */
		cmds, sizeof(*cmds), array,
		IOMMU_VIOMMU_INVALIDATE_DATA_SELFTEST);
	if (rc)	/* [한국어] 복사에 실패하면 */
		goto out;	/* [한국어] 처리한 개수 0 으로 나간다. */

	while (cur != end) {	/* [한국어] 명령을 차례로. */
		struct mock_dev *mdev;	/* [한국어] 대상 모의 장치. */
		struct device *dev;	/* [한국어] 그 코어 장치. */
		int i;	/* [한국어] 캐시 순회용. */

		if (cur->flags & ~IOMMU_TEST_INVALIDATE_FLAG_ALL) {	/* [한국어] 모르는 플래그면 */
			rc = -EOPNOTSUPP;	/* [한국어] 거절한다. */
			goto out;	/* [한국어] 여기까지 처리한 개수를 알린다. */
		}

		if (cur->cache_id > MOCK_DEV_CACHE_ID_MAX) {	/* [한국어] 범위를 벗어난 첨자면 */
			rc = -EINVAL;	/* [한국어] 거절. */
			goto out;	/* [한국어] 나간다. */
		}

		xa_lock(&viommu->vdevs);	/* [한국어] 가상 장치 표를 지키는 락. */
		dev = iommufd_viommu_find_dev(viommu,	/* [한국어] 게스트가 아는 번호를 실제 장치로 옮긴다. 이것이 vIOMMU 의 핵심 역할이다. */
					      (unsigned long)cur->vdev_id);
		if (!dev) {	/* [한국어] 없는 번호면 */
			xa_unlock(&viommu->vdevs);	/* [한국어] 락을 놓고 */
			rc = -EINVAL;	/* [한국어] 거절한다. */
			goto out;	/* [한국어] 나간다. */
		}
		mdev = container_of(dev, struct mock_dev, dev);	/* [한국어] 모의 장치로 되짚는다. */

		if (cur->flags & IOMMU_TEST_INVALIDATE_FLAG_ALL) {	/* [한국어] 전체 무효화면 */
			/* Invalidate all cache entries and ignore cache_id */
			for (i = 0; i < MOCK_DEV_CACHE_NUM; i++)	/* [한국어] 모든 캐시 항목을 */
				mdev->cache[i] = 0;	/* [한국어] 지운다. 원 주석대로 cache_id 는 무시한다. */
		} else {
			mdev->cache[cur->cache_id] = 0;	/* [한국어] 아니면 지목한 항목만. */
		}
		xa_unlock(&viommu->vdevs);	/* [한국어] 락 해제. */

		cur++;	/* [한국어] 다음 명령. */
	}
out:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	array->entry_num = cur - cmds;	/* [한국어] 처리한 개수를 알린다. 사용자가 어디까지 됐는지 알고 나머지를 다시 보낼 수 있다. */
	kfree(cmds);	/* [한국어] 배열을 해제한다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * mock_viommu_get_hw_queue_size - 하드웨어 큐 구조체의 크기를 알려 준다
 *
 * @viommu: 대상 vIOMMU.
 * @queue_type: 만들려는 큐의 종류.
 * @return: 그 크기, 지원하지 않는 종류면 0.
 *
 * 코어가 드라이버 구조체까지 한 덩어리로 할당하기 위해 먼저 묻는다.
 * 0 을 돌려주면 그 종류를 지원하지 않는다는 뜻이다.
 */
static size_t mock_viommu_get_hw_queue_size(struct iommufd_viommu *viommu,
					    enum iommu_hw_queue_type queue_type)
{
	if (queue_type != IOMMU_HW_QUEUE_TYPE_SELFTEST)	/* [한국어] 우리 종류가 아니면 */
		return 0;	/* [한국어] 0 은 "지원하지 않는다"는 뜻이다. */
	return HW_QUEUE_STRUCT_SIZE(struct mock_hw_queue, core);	/* [한국어] 코어 구조체와 우리 구조체를 한 덩어리로 잡을 크기를 계산한다. */
}

/*
 * [한국어]
 * mock_hw_queue_destroy - 모의 하드웨어 큐를 파괴한다
 *
 * @hw_queue: 파괴할 큐.
 *
 * 앞 번호의 큐에 걸어 둔 의존을 푼다. 큐끼리 순서 의존이 있는 하드웨어를
 * 흉내 내려고 만든 관계다 — 앞 큐가 먼저 사라지면 안 된다.
 */
static void mock_hw_queue_destroy(struct iommufd_hw_queue *hw_queue)
{
	struct mock_hw_queue *mock_hw_queue = to_mock_hw_queue(hw_queue);	/* [한국어] 모의 큐로 내려간다. */
	struct mock_viommu *mock_viommu = mock_hw_queue->mock_viommu;	/* [한국어] 그 큐가 매달린 vIOMMU. */

	mutex_lock(&mock_viommu->queue_mutex);	/* [한국어] 큐 배열을 지키는 뮤텍스. */
	mock_viommu->hw_queue[mock_hw_queue->index] = NULL;	/* [한국어] 배열에서 자기를 뺀다. */
	if (mock_hw_queue->prev)	/* [한국어] 앞 번호 큐에 의존을 걸어 두었으면 */
		iommufd_hw_queue_undepend(mock_hw_queue, mock_hw_queue->prev,	/* [한국어] 푼다. 그 전에는 앞 큐가 사라질 수 없었다. */
					  core);
	mutex_unlock(&mock_viommu->queue_mutex);	/* [한국어] 락 해제. */
}

/* Test iommufd_hw_queue_depend/undepend() */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * mock_hw_queue_init_phys - 모의 하드웨어 큐를 세운다
 *
 * @hw_queue: 세울 큐.
 * @index: 큐 번호.
 * @base_addr_pa: 코어가 변환해 준 물리 주소.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석대로 이 함수의 목적 중 하나가 코어의 주소 변환을 검증하는
 * 것이다 — 게스트가 준 IOVA 를 코어가 물리 주소로 옮겼는데, 그것이
 * 도메인을 직접 물어본 값과 같은지 확인한다.
 *
 * 큐 사이 의존도 여기서 건다. 0 번이 아닌 큐는 그 앞 번호가 반드시
 * 있어야 하고, 그것에 의존을 걸어 먼저 사라지지 못하게 한다.
 */
static int mock_hw_queue_init_phys(struct iommufd_hw_queue *hw_queue, u32 index,
				   phys_addr_t base_addr_pa)
{
	struct mock_viommu *mock_viommu = to_mock_viommu(hw_queue->viommu);	/* [한국어] 이 큐가 매달릴 vIOMMU. */
	struct mock_hw_queue *mock_hw_queue = to_mock_hw_queue(hw_queue);	/* [한국어] 모의 큐로 내려간다. */
	struct mock_hw_queue *prev = NULL;	/* [한국어] 앞 번호의 큐. */
	int rc = 0;	/* [한국어] 결과 코드. */

	if (index >= IOMMU_TEST_HW_QUEUE_MAX)	/* [한국어] 정해진 개수를 넘으면 */
		return -EINVAL;	/* [한국어] 거절한다. 진짜 하드웨어도 큐 개수가 정해져 있다. */

	mutex_lock(&mock_viommu->queue_mutex);	/* [한국어] 큐 배열을 지키는 뮤텍스. */

	if (mock_viommu->hw_queue[index]) {	/* [한국어] 그 번호가 이미 쓰이면 */
		rc = -EEXIST;	/* [한국어] 거절한다. */
		goto unlock;	/* [한국어] 락을 놓고 나간다. */
	}

	if (index) {	/* [한국어] 0 번이 아니면 */
		prev = mock_viommu->hw_queue[index - 1];	/* [한국어] 앞 번호가 반드시 있어야 한다. */
		if (!prev) {	/* [한국어] 없으면 */
			rc = -EIO;	/* [한국어] 순서를 건너뛴 것이다. */
			goto unlock;	/* [한국어] 나간다. */
		}
	}

	/*
	 * Test to catch a kernel bug if the core converted the physical address
	 * incorrectly. Let mock_domain_iova_to_phys() WARN_ON if it fails.
	 */
	if (base_addr_pa != iommu_iova_to_phys(&mock_viommu->s2_parent->domain,	/* [한국어] 원 주석대로 코어가 게스트 IOVA 를 물리 주소로 옮긴 결과가 */
					       hw_queue->base_addr)) {	/* [한국어] 도메인에 직접 물어본 값과 같아야 한다. */
		rc = -EFAULT;	/* [한국어] 다르면 코어 쪽 버그다. */
		goto unlock;	/* [한국어] 나간다. */
	}

	if (prev) {	/* [한국어] 앞 큐가 있으면 */
		rc = iommufd_hw_queue_depend(mock_hw_queue, prev, core);	/* [한국어] 의존을 건다. 그 큐가 먼저 사라지지 못한다. */
		if (rc)	/* [한국어] 걸지 못했으면 */
			goto unlock;	/* [한국어] 나간다. */
	}

	mock_hw_queue->prev = prev;	/* [한국어] 파괴할 때 풀어야 하므로 기억해 둔다. */
	mock_hw_queue->mock_viommu = mock_viommu;	/* [한국어] 매달린 vIOMMU 도. */
	mock_viommu->hw_queue[index] = mock_hw_queue;	/* [한국어] 배열에 넣는다. */

	hw_queue->destroy = &mock_hw_queue_destroy;	/* [한국어] 성공한 뒤에야 파괴 콜백을 단다 — 그래야 실패 경로가 그것을 부르지 않는다. */
unlock:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	mutex_unlock(&mock_viommu->queue_mutex);	/* [한국어] 락 해제. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어] 모의 vIOMMU 의 연산표.
 *
 * 코어가 vIOMMU 를 다룰 때 부르는 콜백들이다. 진짜 드라이버라면 여기서
 * 하드웨어 레지스터를 건드린다.
 */
static struct iommufd_viommu_ops mock_viommu_ops = {
	.destroy = mock_viommu_destroy,	/* [한국어] 코어가 vIOMMU 를 없앨 때 부른다. */
	.alloc_domain_nested = mock_viommu_alloc_domain_nested,
	.cache_invalidate = mock_viommu_cache_invalidate,
	.get_hw_queue_size = mock_viommu_get_hw_queue_size,
	.hw_queue_init_phys = mock_hw_queue_init_phys,
};

/*
 * [한국어]
 * mock_get_viommu_size - vIOMMU 구조체의 크기를 알려 준다
 *
 * @dev: 요청한 장치.
 * @viommu_type: 만들려는 vIOMMU 의 종류.
 * @return: 그 크기, 지원하지 않는 종류면 0.
 *
 * 큐와 같은 방식이다. 코어가 자기 구조체와 드라이버 구조체를 한 덩어리로
 * 잡으려고 먼저 묻는다.
 */
static size_t mock_get_viommu_size(struct device *dev,
				   enum iommu_viommu_type viommu_type)
{
	if (viommu_type != IOMMU_VIOMMU_TYPE_SELFTEST)	/* [한국어] 우리 종류가 아니면 */
		return 0;	/* [한국어] 지원하지 않는다. */
	return VIOMMU_STRUCT_SIZE(struct mock_viommu, core);	/* [한국어] 코어와 드라이버 구조체를 한 덩어리로 잡을 크기. */
}

/*
 * [한국어]
 * mock_viommu_init - 모의 vIOMMU 를 세운다
 *
 * @viommu: 세울 vIOMMU.
 * @parent_domain: 2단계 변환의 바깥쪽 도메인.
 * @user_data: 드라이버 고유 데이터(없을 수 있다).
 * @return: 0 성공, 음수면 실패.
 *
 * 사용자가 데이터를 주면 게스트가 mmap 할 수 있는 가짜 레지스터 페이지를
 * 만들어 준다. 진짜 하드웨어라면 실제 레지스터 페이지가 그 자리에 온다.
 *
 * 원 주석대로 되울림 시험이다 — 사용자가 넣은 값을 그대로 페이지에 쓰고
 * 응답에도 실어 준다. 사용자가 mmap 해서 같은 값을 읽으면 그 경로가
 * 온전하다는 뜻이다.
 *
 * IOMMU 참조를 늘리는 이유: 이 vIOMMU 가 살아 있는 동안 모듈을 내릴 수
 * 없어야 한다.
 */
static int mock_viommu_init(struct iommufd_viommu *viommu,
			    struct iommu_domain *parent_domain,
			    const struct iommu_user_data *user_data)
{
	struct mock_iommu_device *mock_iommu = container_of(	/* [한국어] 매달린 IOMMU 장치로 되짚는다. */
		viommu->iommu_dev, struct mock_iommu_device, iommu_dev);
	struct mock_viommu *mock_viommu = to_mock_viommu(viommu);	/* [한국어] 모의 vIOMMU 로 내려간다. */
	struct iommu_viommu_selftest data;	/* [한국어] 사용자와 주고받을 데이터. */
	int rc;	/* [한국어] 결과 코드. */

	if (user_data) {	/* [한국어] 데이터를 줬으면 창까지 만든다. */
		rc = iommu_copy_struct_from_user(	/* [한국어] 사용자 값을 가져온다. */
			&data, user_data, IOMMU_VIOMMU_TYPE_SELFTEST, out_data);
		if (rc)	/* [한국어] 실패하면 */
			return rc;	/* [한국어] 그대로 올린다. */

		/* Allocate two pages */
		mock_viommu->page =	/* [한국어] 게스트가 볼 가짜 레지스터 페이지. 두 장을 이어서 잡는다. */
			(u32 *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, 1);	/* [한국어] 차수 1 이 곧 두 페이지다. 0 으로 채워 둔다. */
		if (!mock_viommu->page)	/* [한국어] 메모리가 없다. */
			return -ENOMEM;	/* [한국어] 실패. */

		rc = iommufd_viommu_alloc_mmap(&mock_viommu->core,	/* [한국어] 그 물리 페이지를 사용자가 mmap 할 수 있게 오프셋을 배정받는다. */
					       __pa(mock_viommu->page),
					       PAGE_SIZE * 2,
					       &mock_viommu->mmap_offset);
		if (rc)	/* [한국어] 창 오프셋을 배정받지 못했다. */
			goto err_free_page;	/* [한국어] 실패하면 페이지를 놓는다. */

		/* For loopback tests on both the page and out_data */
		*mock_viommu->page = data.in_data;	/* [한국어] 원 주석대로 되울림 시험 — 사용자가 넣은 값을 페이지에도 쓴다. */
		data.out_data = data.in_data;	/* [한국어] 응답에도 같은 값을 싣는다. */
		data.out_mmap_length = PAGE_SIZE * 2;	/* [한국어] 창의 크기를 알린다. */
		data.out_mmap_offset = mock_viommu->mmap_offset;	/* [한국어] mmap 에 쓸 오프셋도. */
		rc = iommu_copy_struct_to_user(	/* [한국어] 사용자 버퍼에 되돌려 쓴다. */
			user_data, &data, IOMMU_VIOMMU_TYPE_SELFTEST, out_data);
		if (rc)	/* [한국어] 사용자에게 되돌려 쓰지 못했다. */
			goto err_destroy_mmap;	/* [한국어] 실패하면 창 등록을 걷는다. */
	}

	refcount_inc(&mock_iommu->users);	/* [한국어] 이 vIOMMU 가 살아 있는 동안 모듈을 내릴 수 없다. */
	mutex_init(&mock_viommu->queue_mutex);	/* [한국어] 큐 배열을 지킬 뮤텍스. */
	mock_viommu->s2_parent = to_mock_domain(parent_domain);	/* [한국어] 2단계 변환의 바깥쪽 도메인. 주소 변환을 검증할 때 쓴다. */

	viommu->ops = &mock_viommu_ops;	/* [한국어] 연산표를 단다. 이 뒤로 코어가 이 vIOMMU 를 다룰 수 있다. */
	return 0;	/* [한국어] 성공. */

err_destroy_mmap:	/* [한국어] 응답 실패 경로. */
	iommufd_viommu_destroy_mmap(&mock_viommu->core,	/* [한국어] 창 등록을 걷는다. */
				    mock_viommu->mmap_offset);
err_free_page:	/* [한국어] 창 등록 실패도 여기로 합류한다. */
	free_pages((unsigned long)mock_viommu->page, 1);	/* [한국어] 페이지를 놓는다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어] 모의 IOMMU 드라이버의 연산표.
 *
 * iommu 코어가 이 표를 통해 모의 하드웨어를 다룬다. 진짜 드라이버와
 * 같은 모양이라, 이 표를 채우는 것만으로 코어의 모든 경로가 시험된다.
 *
 * 원 주석이 default_domain 을 명시적으로 두는 이유를 밝힌다 — 차단
 * 도메인의 종류 값이 0 이라 def_domain_type() 으로는 고를 수 없다.
 *
 * user_pasid_table 이 참인 것은 PASID 표를 게스트가 관리한다는 뜻으로,
 * 중첩 변환에서 PASID 경로를 시험하기 위한 설정이다.
 */
static const struct iommu_ops mock_ops = {
	/*
	 * IOMMU_DOMAIN_BLOCKED cannot be returned from def_domain_type()
	 * because it is zero.
	 */
	.default_domain = &mock_blocking_domain,	/* [한국어] 원 주석대로 차단 도메인의 종류 값이 0 이라 def_domain_type() 으로는 고를 수 없어, 여기서 직접 지정한다. */
	.blocked_domain = &mock_blocking_domain,
	.owner = THIS_MODULE,
	.hw_info = mock_domain_hw_info,
	.domain_alloc_paging_flags = mock_domain_alloc_paging_flags,
	.domain_alloc_nested = mock_domain_alloc_nested,
	.capable = mock_domain_capable,
	.device_group = generic_device_group,
	.probe_device = mock_probe_device,
	.page_response = mock_domain_page_response,
	.user_pasid_table = true,
	.get_viommu_size = mock_get_viommu_size,
	.viommu_init = mock_viommu_init,
};

/*
 * [한국어]
 * mock_domain_free_nested - 모의 중첩 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 *
 * 페이지 테이블이 없어 구조체만 놓으면 된다.
 */
static void mock_domain_free_nested(struct iommu_domain *domain)
{
	kfree(to_mock_nested(domain));	/* [한국어] 페이지 테이블이 없어 구조체만 놓으면 된다. */
}

static int	/* [한국어] 반환형이 다음 줄의 함수 이름과 나뉘어 있다. */
/*
 * [한국어]
 * mock_domain_cache_invalidate_user - 도메인 단위 무효화 명령을 처리한다
 *
 * @domain: 대상 중첩 도메인.
 * @array: 사용자가 준 명령 배열. 처리한 개수를 되돌려 쓴다.
 * @return: 0 성공, 음수면 실패.
 *
 * 위 vIOMMU 판과 짝을 이룬다. 이쪽은 도메인 하나에 매인 무효화라 가상
 * 장치를 지목하지 않는다.
 *
 * 가짜 IOTLB 값을 0 으로 만드는 것이 "무효화"의 전부다. 테스트가 그
 * 값을 읽어 무효화가 닿았는지 판정한다.
 *
 * 처리한 개수를 돌려주는 방식도 같다 — 중간에 실패해도 앞의 것은 이미
 * 반영됐으므로 사용자가 그것을 알아야 한다.
 */
mock_domain_cache_invalidate_user(struct iommu_domain *domain,
				  struct iommu_user_data_array *array)
{
	struct mock_iommu_domain_nested *mock_nested = to_mock_nested(domain);	/* [한국어] 모의 중첩 도메인으로 내려간다. */
	struct iommu_hwpt_invalidate_selftest inv;	/* [한국어] 한 건씩 가져올 명령. */
	u32 processed = 0;	/* [한국어] 실제로 처리한 개수. */
	int i = 0, j;	/* [한국어] 바깥 고리와 안쪽 고리의 첨자. */
	int rc = 0;	/* [한국어] 결과 코드. */

	if (array->type != IOMMU_HWPT_INVALIDATE_DATA_SELFTEST) {	/* [한국어] 모르는 형식이면 */
		rc = -EINVAL;	/* [한국어] 거절한다. */
		goto out;	/* [한국어] 처리한 개수 0 으로 나간다. */
	}

	for ( ; i < array->entry_num; i++) {	/* [한국어] 명령을 차례로. 위쪽 판과 달리 한 건씩 가져온다. */
		rc = iommu_copy_struct_from_user_array(&inv, array,	/* [한국어] i 번째 명령을 가져온다. */
						       IOMMU_HWPT_INVALIDATE_DATA_SELFTEST,
						       i, iotlb_id);
		if (rc)	/* [한국어] 복사에 실패하면 */
			break;	/* [한국어] 여기까지 처리한 개수를 알린다. */

		if (inv.flags & ~IOMMU_TEST_INVALIDATE_FLAG_ALL) {	/* [한국어] 모르는 플래그면 */
			rc = -EOPNOTSUPP;	/* [한국어] 거절. */
			break;	/* [한국어] 고리를 끝낸다. */
		}

		if (inv.iotlb_id > MOCK_NESTED_DOMAIN_IOTLB_ID_MAX) {	/* [한국어] 범위를 벗어난 첨자면 */
			rc = -EINVAL;	/* [한국어] 거절. */
			break;	/* [한국어] 고리를 끝낸다. */
		}

		if (inv.flags & IOMMU_TEST_INVALIDATE_FLAG_ALL) {	/* [한국어] 전체 무효화면 */
			/* Invalidate all mock iotlb entries and ignore iotlb_id */
			for (j = 0; j < MOCK_NESTED_DOMAIN_IOTLB_NUM; j++)	/* [한국어] 모든 항목을 */
				mock_nested->iotlb[j] = 0;	/* [한국어] 지운다. 원 주석대로 iotlb_id 는 무시한다. */
		} else {
			mock_nested->iotlb[inv.iotlb_id] = 0;	/* [한국어] 아니면 지목한 항목만. */
		}

		processed++;	/* [한국어] 한 건을 처리했다. */
	}

out:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	array->entry_num = processed;	/* [한국어] 처리한 개수를 알린다 — 앞의 것은 이미 반영됐으므로 사용자가 알아야 한다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어] 중첩 도메인의 연산표.
 *
 * 매핑 연산이 없는 것이 페이징 도메인과의 차이다. 중첩 도메인의 변환표는
 * 게스트가 관리하므로 호스트가 매핑을 넣을 일이 없다.
 */
static struct iommu_domain_ops domain_nested_ops = {
	.free = mock_domain_free_nested,	/* [한국어] 중첩 도메인은 구조체만 놓으면 된다. */
	.attach_dev = mock_domain_nop_attach,
	.cache_invalidate_user = mock_domain_cache_invalidate_user,
	.set_dev_pasid = mock_domain_set_dev_pasid_nop,
};

/*
 * [한국어]
 * __get_md_pagetable - id 로 페이지 테이블 객체를 찾는다
 *
 * @ucmd: 처리 중인 명령.
 * @mockpt_id: 찾을 객체의 id.
 * @hwpt_type: 기대하는 객체 종류.
 * @return: 참조를 든 도메인, 실패하면 오류 포인터.
 *
 * 테스트 명령들이 도메인 내부를 들여다볼 때 쓰는 공통 조회 함수다.
 * 종류를 함께 확인해, 페이징 도메인을 기대한 자리에 중첩 도메인이
 * 오는 일을 막는다.
 */
static inline struct iommufd_hw_pagetable *
__get_md_pagetable(struct iommufd_ucmd *ucmd, u32 mockpt_id, u32 hwpt_type)
{
	struct iommufd_object *obj;	/* [한국어] 찾은 객체. */

	obj = iommufd_get_object(ucmd->ictx, mockpt_id, hwpt_type);	/* [한국어] 종류까지 맞춰 찾는다. */
	if (IS_ERR(obj))	/* [한국어] 없거나 종류가 다르면 */
		return ERR_CAST(obj);	/* [한국어] 오류를 그대로 올린다. */
	return container_of(obj, struct iommufd_hw_pagetable, obj);	/* [한국어] 도메인으로 되짚는다. */
}

/*
 * [한국어]
 * get_md_pagetable - 모의 페이징 도메인을 찾는다
 *
 * @ucmd: 처리 중인 명령.
 * @mockpt_id: 찾을 객체의 id.
 * @mock: 찾은 모의 도메인을 여기에 쓴다.
 * @return: 참조를 든 도메인, 실패하면 오류 포인터.
 *
 * 위 함수를 페이징 종류로 감싸고, 모의 도메인 포인터까지 함께 꺼내 준다.
 */
static inline struct iommufd_hw_pagetable *
get_md_pagetable(struct iommufd_ucmd *ucmd, u32 mockpt_id,
		 struct mock_iommu_domain **mock)
{
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 찾은 도메인. */

	hwpt = __get_md_pagetable(ucmd, mockpt_id, IOMMUFD_OBJ_HWPT_PAGING);	/* [한국어] 페이징 종류를 기대한다. */
	if (IS_ERR(hwpt))	/* [한국어] 실패하면 */
		return hwpt;	/* [한국어] 그대로 올린다. */
	if (hwpt->domain->type != IOMMU_DOMAIN_UNMANAGED ||	/* [한국어] 사용자가 관리하는 종류가 아니거나 */
	    hwpt->domain->owner != &mock_ops) {	/* [한국어] 우리 드라이버가 만든 도메인이 아니면 — 남의 도메인 내부를 들여다보면 안 된다. */
		iommufd_put_object(ucmd->ictx, &hwpt->obj);	/* [한국어] 참조를 놓고 */
		return ERR_PTR(-EINVAL);	/* [한국어] 거절한다. 남의 도메인 내부를 들여다보면 안 된다. */
	}
	*mock = to_mock_domain(hwpt->domain);	/* [한국어] 모의 도메인 포인터도 함께 준다. */
	return hwpt;	/* [한국어] 찾은 도메인. */
}

/*
 * [한국어]
 * get_md_pagetable_nested - 모의 중첩 도메인을 찾는다
 *
 * @ucmd: 처리 중인 명령.
 * @mockpt_id: 찾을 객체의 id.
 * @mock_nested: 찾은 모의 중첩 도메인을 여기에 쓴다.
 * @return: 참조를 든 도메인, 실패하면 오류 포인터.
 *
 * 위와 같되 중첩 종류를 기대한다.
 */
static inline struct iommufd_hw_pagetable *
get_md_pagetable_nested(struct iommufd_ucmd *ucmd, u32 mockpt_id,
			struct mock_iommu_domain_nested **mock_nested)
{
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 찾은 도메인. */

	hwpt = __get_md_pagetable(ucmd, mockpt_id, IOMMUFD_OBJ_HWPT_NESTED);	/* [한국어] 중첩 종류를 기대한다. */
	if (IS_ERR(hwpt))	/* [한국어] 실패하면 */
		return hwpt;	/* [한국어] 그대로 올린다. */
	if (hwpt->domain->type != IOMMU_DOMAIN_NESTED ||	/* [한국어] 중첩 종류가 아니거나 */
	    hwpt->domain->ops != &domain_nested_ops) {	/* [한국어] 우리 연산표가 아니면 */
		iommufd_put_object(ucmd->ictx, &hwpt->obj);	/* [한국어] 참조를 놓고 */
		return ERR_PTR(-EINVAL);	/* [한국어] 거절한다. */
	}
	*mock_nested = to_mock_nested(hwpt->domain);	/* [한국어] 모의 중첩 도메인 포인터도 함께 준다. */
	return hwpt;	/* [한국어] 찾은 도메인. */
}

/*
 * [한국어]
 * mock_dev_release - 모의 장치의 마지막 참조가 사라졌을 때
 *
 * @dev: 사라지는 장치.
 *
 * 리눅스 장치 모형이 요구하는 콜백이다. 이것이 없으면 등록할 때 경고가
 * 난다 — 장치 구조체의 수명을 누가 책임지는지 분명해야 하기 때문이다.
 *
 * 배정받은 번호를 반납하고 구조체를 해제한다.
 */
static void mock_dev_release(struct device *dev)
{
	struct mock_dev *mdev = to_mock_dev(dev);	/* [한국어] 모의 장치로 내려간다. */

	ida_free(&mock_dev_ida, mdev->id);	/* [한국어] 배정받은 번호를 반납한다. */
	kfree(mdev);	/* [한국어] 구조체를 해제한다. 이 콜백이 없으면 장치 모형이 등록을 거부한다. */
}

/*
 * [한국어]
 * mock_dev_create - 모의 장치를 만들어 등록한다
 *
 * @dev_flags: 만들 장치의 성격. MOCK_FLAGS_DEVICE_* 조합.
 * @return: 만든 장치, 실패하면 오류 포인터.
 *
 * 가짜 버스에 장치를 하나 등록한다. 그 순간 iommu 코어가 probe 를 불러
 * 모의 IOMMU 아래로 편입시킨다.
 *
 * DMA 마스크를 세우는 것에 주의 — 리눅스는 마스크가 없는 장치를 DMA 할
 * 수 없는 것으로 보아, 그러면 매핑 경로가 아예 돌지 않는다.
 *
 * 가짜 캐시 배열을 정해진 값으로 채운다. 무효화가 닿으면 0 이 되므로,
 * 테스트가 그 변화를 보고 판정한다.
 */
static struct mock_dev *mock_dev_create(unsigned long dev_flags)
{
	struct property_entry prop[] = {	/* [한국어] 장치에 붙일 속성 목록. */
		PROPERTY_ENTRY_U32("pasid-num-bits", 0),	/* [한국어] PASID 폭. 기본은 0 = PASID 를 쓰지 않음. */
		{},
	};
	const u32 valid_flags = MOCK_FLAGS_DEVICE_NO_DIRTY |	/* [한국어] 받아들이는 장치 플래그의 전부. */
				MOCK_FLAGS_DEVICE_PASID;
	struct mock_dev *mdev;	/* [한국어] 만들 장치. */
	int rc, i;	/* [한국어] 결과 코드와 순회용 첨자. */

	if (dev_flags & ~valid_flags)	/* [한국어] 아는 플래그가 아니면 */
		return ERR_PTR(-EINVAL);	/* [한국어] 잘못된 인자. */

	mdev = kzalloc_obj(*mdev);	/* [한국어] 장치 구조체를 잡는다. */
	if (!mdev)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */

	init_rwsem(&mdev->viommu_rwsem);	/* [한국어] vIOMMU 포인터를 지킬 락. 이벤트 경로가 읽는다. */
	device_initialize(&mdev->dev);	/* [한국어] 장치 모형의 기본 상태를 세운다. */
	mdev->flags = dev_flags;	/* [한국어] 이 장치의 성격. */
	mdev->dev.release = mock_dev_release;	/* [한국어] 수명을 누가 책임지는지 알린다. 없으면 등록 때 경고가 난다. */
	mdev->dev.bus = &iommufd_mock_bus_type.bus;	/* [한국어] 가짜 버스에 속한다. 이 값으로 probe 가 우리 장치를 알아본다. */
	for (i = 0; i < MOCK_DEV_CACHE_NUM; i++)	/* [한국어] 가짜 캐시 배열을 */
		mdev->cache[i] = IOMMU_TEST_DEV_CACHE_DEFAULT;	/* [한국어] 정해진 값으로 채운다. 무효화가 닿으면 0 이 된다. */

	rc = ida_alloc(&mock_dev_ida, GFP_KERNEL);	/* [한국어] 장치 번호를 배정받는다. */
	if (rc < 0)	/* [한국어] 번호가 없다. */
		goto err_put;
	mdev->id = rc;	/* [한국어] 배정받은 번호. */

	rc = dev_set_name(&mdev->dev, "iommufd_mock%u", mdev->id);	/* [한국어] sysfs 에 보일 이름. */
	if (rc)	/* [한국어] 이름을 짓지 못했다. */
		goto err_put;	/* [한국어] 실패하면 참조를 놓는다 — 이 뒤로는 release 가 정리를 맡는다. */

	if (dev_flags & MOCK_FLAGS_DEVICE_PASID)	/* [한국어] PASID 를 지원하는 장치로 만들려면 */
		prop[0] = PROPERTY_ENTRY_U32("pasid-num-bits", MOCK_PASID_WIDTH);	/* [한국어] 속성 값을 실제 폭으로 바꾼다. 코어가 이것을 보고 PASID 능력을 판정한다. */

	rc = device_create_managed_software_node(&mdev->dev, prop, NULL);	/* [한국어] 속성을 소프트웨어 노드로 붙인다. 장치 트리 없이 속성을 주는 방법이다. */
	if (rc) {	/* [한국어] 속성을 붙이지 못했다. */
		dev_err(&mdev->dev, "add pasid-num-bits property failed, rc: %d", rc);	/* [한국어] 왜 실패했는지 로그에 남긴다 — PASID 시험이 통째로 돌지 않게 되므로. */
		goto err_put;	/* [한국어] 참조를 놓아 정리한다. */
	}

	rc = iommu_mock_device_add(&mdev->dev, &mock_iommu.iommu_dev);	/* [한국어] 장치를 등록하고 이 IOMMU 아래로 넣는다. 셀프테스트 전용 진입점이다. */
	if (rc)	/* [한국어] 등록하지 못했다. */
		goto err_put;
	return mdev;	/* [한국어] 만든 장치. */

err_put:	/* [한국어] 등록 뒤 실패 경로. */
	put_device(&mdev->dev);	/* [한국어] 참조를 놓으면 release 가 정리한다. */
	return ERR_PTR(rc);	/* [한국어] 오류를 올린다. */
}

/*
 * [한국어]
 * mock_dev_destroy - 모의 장치를 걷어 낸다
 *
 * @mdev: 없앨 장치.
 *
 * 등록을 풀면 참조가 놓이고, 마지막이면 release 가 불린다.
 */
static void mock_dev_destroy(struct mock_dev *mdev)
{
	device_unregister(&mdev->dev);	/* [한국어] 등록을 풀고 참조를 놓는다. 마지막이면 release 가 불린다. */
}

/*
 * [한국어]
 * iommufd_selftest_is_mock_dev - 이것이 모의 장치인가
 *
 * @dev: 물어보는 장치.
 * @return: 모의 장치면 참.
 *
 * device.c 가 이것을 보고 몇 가지 검사를 건너뛴다 — 모의 장치에는
 * 진짜 인터럽트도 없고, 문맥 참조를 들면 고리가 생긴다.
 *
 * release 콜백을 비교하는 것이 판별 방법이다. 그것은 이 파일에만 있는
 * 함수라 다른 장치와 겹칠 수 없다.
 */
bool iommufd_selftest_is_mock_dev(struct device *dev)
{
	return dev->release == mock_dev_release;	/* [한국어] release 콜백은 이 파일에만 있어 다른 장치와 겹칠 수 없다. */
}

/* Create an hw_pagetable with the mock domain so we can test the domain ops */
/*
 * [한국어]
 * iommufd_test_mock_domain - MOCK_DOMAIN 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd: 그 인자.
 * @return: 0 성공, 음수면 실패.
 *
 * 모의 장치를 만들어 iommufd 에 묶고, 지정한 IOAS 또는 도메인에 붙인다.
 * 대부분의 테스트가 이 명령으로 시작한다.
 *
 * 정규 API 를 그대로 쓰는 것이 요점이다 — bind 와 attach 를 거치므로
 * device.c 의 경로가 함께 시험된다.
 *
 * 원 주석대로 사용자는 stdev_id 를 지워야 이 객체가 사라진다. 그때
 * iommufd_selftest_destroy 가 떼기와 풀기를 대신 해 준다.
 */
static int iommufd_test_mock_domain(struct iommufd_ucmd *ucmd,
				    struct iommu_test_cmd *cmd)
{
	struct iommufd_device *idev;	/* [한국어] 묶은 뒤 얻는 iommufd 장치 객체. */
	struct selftest_obj *sobj;	/* [한국어] 이 모의 장치를 담을 셀프테스트 객체. */
	u32 pt_id = cmd->id;	/* [한국어] 사용자가 지정한 IOAS 또는 도메인. 실제로 쓴 도메인 id 로 바뀐다. */
	u32 dev_flags = 0;	/* [한국어] 만들 장치의 성격. */
	u32 idev_id;	/* [한국어] 묶은 장치의 객체 id. */
	int rc;	/* [한국어] 결과 코드. */

	sobj = iommufd_object_alloc(ucmd->ictx, sobj, IOMMUFD_OBJ_SELFTEST);	/* [한국어] 객체를 만든다. 아직 공개하지 않는다. */
	if (IS_ERR(sobj))	/* [한국어] 메모리가 없다. */
		return PTR_ERR(sobj);	/* [한국어] 실패. */

	sobj->idev.ictx = ucmd->ictx;	/* [한국어] 속한 문맥. */
	sobj->type = TYPE_IDEV;	/* [한국어] 종류를 표시한다. */

	if (cmd->op == IOMMU_TEST_OP_MOCK_DOMAIN_FLAGS)	/* [한국어] 플래그 판 명령이면 */
		dev_flags = cmd->mock_domain_flags.dev_flags;	/* [한국어] 거기서 성격을 읽는다. 같은 처리기를 두 명령이 나눠 쓴다. */

	sobj->idev.mock_dev = mock_dev_create(dev_flags);	/* [한국어] 모의 장치를 만들어 버스에 등록한다. */
	if (IS_ERR(sobj->idev.mock_dev)) {	/* [한국어] 실패하면 */
		rc = PTR_ERR(sobj->idev.mock_dev);	/* [한국어] 오류를 꺼내 */
		goto out_sobj;	/* [한국어] 객체를 되돌린다. */
	}

	idev = iommufd_device_bind(ucmd->ictx, &sobj->idev.mock_dev->dev,	/* [한국어] 정규 API 로 묶는다 — device.c 의 경로가 함께 시험된다. */
				   &idev_id);
	if (IS_ERR(idev)) {	/* [한국어] 묶지 못했으면 */
		rc = PTR_ERR(idev);	/* [한국어] 오류를 꺼내 */
		goto out_mdev;	/* [한국어] 장치를 없앤다. */
	}
	sobj->idev.idev = idev;	/* [한국어] 묶인 장치를 기억한다. */

	rc = iommufd_device_attach(idev, IOMMU_NO_PASID, &pt_id);	/* [한국어] 지정한 곳에 붙인다. IOAS 를 줬으면 커널이 도메인을 골라 준다. */
	if (rc)	/* [한국어] 장치를 묶지 못했다. */
		goto out_unbind;	/* [한국어] 실패하면 묶음을 푼다. */

	/* Userspace must destroy the device_id to destroy the object */
	cmd->mock_domain.out_hwpt_id = pt_id;	/* [한국어] 실제로 쓴 도메인의 id. */
	cmd->mock_domain.out_stdev_id = sobj->obj.id;	/* [한국어] 이후 명령에서 이 장치를 지목할 id. */
	cmd->mock_domain.out_idev_id = idev_id;	/* [한국어] 정규 명령에 쓸 수 있는 장치 객체 id. */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 세 값을 사용자에게 돌려 쓴다. */
	if (rc)	/* [한국어] 도메인에 붙이지 못했다. */
		goto out_detach;	/* [한국어] 전하지 못했으면 모두 되돌린다. */
	iommufd_object_finalize(ucmd->ictx, &sobj->obj);	/* [한국어] 이제야 객체를 공개한다. */
	return 0;	/* [한국어] 성공. */

out_detach:	/* [한국어] 응답 실패 경로. */
	iommufd_device_detach(idev, IOMMU_NO_PASID);	/* [한국어] 붙임을 푼다. */
out_unbind:	/* [한국어] 붙이기 실패도 여기로 합류한다. */
	iommufd_device_unbind(idev);	/* [한국어] 묶음을 푼다. */
out_mdev:	/* [한국어] 묶기 실패도 여기로. */
	mock_dev_destroy(sobj->idev.mock_dev);	/* [한국어] 장치를 없앤다. */
out_sobj:	/* [한국어] 모든 실패가 마지막으로 합류한다. */
	iommufd_object_abort(ucmd->ictx, &sobj->obj);	/* [한국어] 공개하지 않은 객체를 되돌린다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * iommufd_test_get_selftest_obj - id 로 셀프테스트 객체를 찾는다
 *
 * @ictx: 문맥.
 * @id: 찾을 객체의 id.
 * @return: 참조를 든 객체, 실패하면 오류 포인터.
 *
 * 원 주석이 왜 장치 객체가 아니라 셀프테스트 객체를 쓰는지 밝힌다 —
 * 그쪽 참조가 파괴 세마포어를 함께 잡아, 떼기와 경합하지 않는다.
 * 떼는 도중에 교체를 걸면 안 되기 때문이다.
 */
static struct selftest_obj *
iommufd_test_get_selftest_obj(struct iommufd_ctx *ictx, u32 id)
{
	struct iommufd_object *dev_obj;	/* [한국어] 찾은 객체. */
	struct selftest_obj *sobj;	/* [한국어] 셀프테스트 객체로 본 모습. */

	/*
	 * Prefer to use the OBJ_SELFTEST because the destroy_rwsem will ensure
	 * it doesn't race with detach, which is not allowed.
	 */
	dev_obj = iommufd_get_object(ictx, id, IOMMUFD_OBJ_SELFTEST);	/* [한국어] 원 주석대로 이쪽 참조가 파괴 세마포어까지 잡아, 떼기와 경합하지 않는다. */
	if (IS_ERR(dev_obj))	/* [한국어] 없는 id 다. */
		return ERR_CAST(dev_obj);	/* [한국어] 오류를 올린다. */

	sobj = to_selftest_obj(dev_obj);	/* [한국어] 셀프테스트 객체로 되짚는다. */
	if (sobj->type != TYPE_IDEV) {	/* [한국어] 장치 종류가 아니면 */
		iommufd_put_object(ictx, dev_obj);	/* [한국어] 참조를 놓고 */
		return ERR_PTR(-EINVAL);	/* [한국어] 거절한다. */
	}
	return sobj;	/* [한국어] 참조를 든 채 돌려준다. */
}

/* Replace the mock domain with a manually allocated hw_pagetable */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_test_mock_domain_replace - MOCK_DOMAIN_REPLACE 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @device_id: 모의 장치(stdev)의 id.
 * @pt_id: 새로 붙일 페이지 테이블 또는 IOAS 의 id.
 * @cmd: 명령 인자. 실제로 쓴 도메인 id 를 여기에 되돌려 쓴다.
 * @return: 0 성공, 음수면 실패.
 *
 * 무중단 교체 경로를 시험한다.
 */
static int iommufd_test_mock_domain_replace(struct iommufd_ucmd *ucmd,
					    unsigned int device_id, u32 pt_id,
					    struct iommu_test_cmd *cmd)
{
	struct selftest_obj *sobj;	/* [한국어] 대상 셀프테스트 객체. */
	int rc;	/* [한국어] 결과 코드. */

	sobj = iommufd_test_get_selftest_obj(ucmd->ictx, device_id);	/* [한국어] 떼기와 경합하지 않게 이쪽으로 찾는다. */
	if (IS_ERR(sobj))	/* [한국어] 없으면 */
		return PTR_ERR(sobj);	/* [한국어] 오류를 올린다. */

	rc = iommufd_device_replace(sobj->idev.idev, IOMMU_NO_PASID, &pt_id);	/* [한국어] 정규 API 로 교체한다. */
	if (rc)	/* [한국어] 실패하면 */
		goto out_sobj;	/* [한국어] 참조를 놓고 나간다. */

	cmd->mock_domain_replace.pt_id = pt_id;	/* [한국어] 실제로 쓴 도메인 id 를 알린다. */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 사용자에게 되돌려 쓴다. */

out_sobj:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	iommufd_put_object(ucmd->ictx, &sobj->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/* Add an additional reserved IOVA to the IOAS */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_test_add_reserved - ADD_RESERVED 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @mockpt_id: 대상 IOAS 의 id.
 * @start: 예약할 구간의 시작 IOVA.
 * @length: 그 길이.
 * @return: 0 성공, 음수면 실패.
 *
 * 실제로는 장치의 예약 구간이 이 자리를 차지하지만, 테스트에서는 원하는
 * 구간을 임의로 만들어야 IOVA 배정이 그것을 피하는지 시험할 수 있다.
 */
static int iommufd_test_add_reserved(struct iommufd_ucmd *ucmd,
				     unsigned int mockpt_id,
				     unsigned long start, size_t length)
{
	unsigned long last;	/* [한국어] 구간의 마지막 주소. */
	struct iommufd_ioas *ioas;	/* [한국어] 대상 주소 공간. */
	int rc;	/* [한국어] 결과 코드. */

	if (!length)	/* [한국어] 길이 0 은 뜻이 없다. */
		return -EINVAL;	/* [한국어] 거절. */
	if (check_add_overflow(start, length - 1, &last))	/* [한국어] 주소가 넘치면 */
		return -EOVERFLOW;	/* [한국어] 거절. */

	ioas = iommufd_get_ioas(ucmd->ictx, mockpt_id);	/* [한국어] id 로 주소 공간을 찾는다. */
	if (IS_ERR(ioas))	/* [한국어] 없으면 */
		return PTR_ERR(ioas);	/* [한국어] 오류를 올린다. */
	down_write(&ioas->iopt.iova_rwsem);	/* [한국어] 예약 트리를 고치므로 쓰기 락. */
	rc = iopt_reserve_iova(&ioas->iopt, start, last, NULL);	/* [한국어] 그 구간을 예약한다. 주인을 NULL 로 두어 장치와 무관한 예약임을 표시한다. */
	up_write(&ioas->iopt.iova_rwsem);	/* [한국어] 락 해제. */
	iommufd_put_object(ucmd->ictx, &ioas->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/* Check that every pfn under each iova matches the pfn under a user VA */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_test_md_check_pa - MD_CHECK_MAP 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @mockpt_id: 검사할 도메인의 id.
 * @iova: 검사 시작 IOVA.
 * @length: 검사할 길이.
 * @uptr: 그 IOVA 가 가리켜야 할 사용자 메모리.
 * @return: 0 이면 일치, 음수면 어긋났거나 오류.
 *
 * 사용자 공간은 물리 주소를 알 수 없으므로 커널이 대신 비교해 준다.
 * 도메인에게 물어본 물리 주소와, 그 사용자 주소의 페이지가 같아야 한다.
 *
 * 페이지 안 오프셋까지 더해 비교하는 것에 주의 — 모의 페이지가 2KB 라
 * 호스트 페이지 한가운데를 가리킬 수 있다.
 *
 * 매핑 경로 전체가 실제로 올바른 페이지를 꽂았는지 확인하는, 이 파일에서
 * 가장 근본적인 검사다.
 */
static int iommufd_test_md_check_pa(struct iommufd_ucmd *ucmd,
				    unsigned int mockpt_id, unsigned long iova,
				    size_t length, void __user *uptr)
{
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 검사할 도메인. */
	struct mock_iommu_domain *mock;	/* [한국어] 그 모의 도메인. */
	unsigned int page_size;	/* [한국어] 이 도메인의 최소 페이지 크기. */
	uintptr_t end;	/* [한국어] 넘침 검사용. */
	int rc;	/* [한국어] 결과 코드. */

	hwpt = get_md_pagetable(ucmd, mockpt_id, &mock);	/* [한국어] 도메인을 찾는다. */
	if (IS_ERR(hwpt))	/* [한국어] 없으면 */
		return PTR_ERR(hwpt);	/* [한국어] 오류를 올린다. */

	page_size = 1 << __ffs(mock->domain.pgsize_bitmap);	/* [한국어] 비트맵의 가장 낮은 비트가 최소 페이지 크기다. 모의 도메인에서는 2KB. */
	if (iova % page_size || length % page_size ||	/* [한국어] 세 값 모두 그 크기에 맞아야 한 페이지씩 비교할 수 있다. */
	    (uintptr_t)uptr % page_size ||	/* [한국어] 사용자 주소도 같은 정렬이어야 한다. */
	    check_add_overflow((uintptr_t)uptr, (uintptr_t)length, &end)) {	/* [한국어] 주소가 넘치면 안 된다. */
		rc = -EINVAL;	/* [한국어] 거절. */
		goto out_put;	/* [한국어] 참조를 놓고 나간다. */
	}

	for (; length; length -= page_size) {	/* [한국어] 한 페이지씩 비교한다. */
		struct page *pages[1];	/* [한국어] 사용자 페이지 하나를 받을 배열. */
		phys_addr_t io_phys;	/* [한국어] 도메인이 답한 물리 주소. */
		unsigned long pfn;	/* [한국어] 사용자 페이지의 PFN. */
		long npages;	/* [한국어] 얻은 페이지 수. */

		npages = get_user_pages_fast((uintptr_t)uptr & PAGE_MASK, 1, 0,	/* [한국어] 사용자 주소의 페이지를 잠시 얻는다. 호스트 페이지 경계로 내림한다. */
					     pages);
		if (npages < 0) {	/* [한국어] 얻지 못했으면 */
			rc = npages;	/* [한국어] 오류를 옮기고 */
			goto out_put;	/* [한국어] 나간다. */
		}
		if (WARN_ON(npages != 1)) {	/* [한국어] 한 페이지를 요청했는데 다르면 */
			rc = -EFAULT;	/* [한국어] 있을 수 없는 결과다. */
			goto out_put;	/* [한국어] 나간다. */
		}
		pfn = page_to_pfn(pages[0]);	/* [한국어] 그 페이지의 PFN. */
		put_page(pages[0]);	/* [한국어] 곧바로 놓는다 — PFN 값만 필요했다. */

		io_phys = mock->domain.ops->iova_to_phys(&mock->domain, iova);	/* [한국어] 도메인에게 이 IOVA 의 물리 주소를 묻는다. */
		if (io_phys !=	/* [한국어] 두 값이 같아야 한다. */
		    pfn * PAGE_SIZE + ((uintptr_t)uptr % PAGE_SIZE)) {	/* [한국어] 호스트 페이지 안의 오프셋까지 더한다 — 모의 페이지가 2KB 라 페이지 한가운데를 가리킬 수 있다. */
			rc = -EINVAL;	/* [한국어] 어긋났다. 매핑 경로 어딘가가 잘못된 것이다. */
			goto out_put;	/* [한국어] 나간다. */
		}
		iova += page_size;	/* [한국어] 다음 페이지로. */
		uptr += page_size;	/* [한국어] 사용자 쪽도 함께. */
	}
	rc = 0;	/* [한국어] 모두 일치했다. */

out_put:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	iommufd_put_object(ucmd->ictx, &hwpt->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/* Check that the page ref count matches, to look for missing pin/unpins */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_test_md_check_refs - MD_CHECK_REFS 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @uptr: 검사할 사용자 메모리.
 * @length: 그 길이.
 * @refs: 기대하는 참조 수.
 * @return: 0 이면 일치, 어긋나면 -EIO.
 *
 * 고정(pin)이 새거나 이중으로 잡히지 않는지 보는 검사다. pages.c 의
 * 참조 계산이 주 대상이다.
 *
 * GUP_PIN_COUNTING_BIAS 로 나누는 이유: 고정은 참조 수를 1 이 아니라
 * 큰 값만큼 올린다. 평범한 참조와 고정을 구별하기 위한 mm 의 방식이다.
 *
 * 복합 페이지를 건너뛰는 이유: 큰 페이지는 참조가 머리 페이지에 몰려
 * 세는 방식이 달라, 이 단순한 검사로는 판정할 수 없다.
 */
static int iommufd_test_md_check_refs(struct iommufd_ucmd *ucmd,
				      void __user *uptr, size_t length,
				      unsigned int refs)
{
	uintptr_t end;	/* [한국어] 넘침 검사용. */

	if (length % PAGE_SIZE || (uintptr_t)uptr % PAGE_SIZE ||	/* [한국어] 호스트 페이지 단위로 세므로 그 정렬이어야 한다. */
	    check_add_overflow((uintptr_t)uptr, (uintptr_t)length, &end))	/* [한국어] 주소가 넘치면 안 된다. */
		return -EINVAL;	/* [한국어] 거절. */

	for (; length; length -= PAGE_SIZE) {	/* [한국어] 한 페이지씩. */
		struct page *pages[1];	/* [한국어] 받을 배열. */
		long npages;	/* [한국어] 얻은 개수. */

		npages = get_user_pages_fast((uintptr_t)uptr, 1, 0, pages);	/* [한국어] 그 페이지를 잠시 얻는다. */
		if (npages < 0)	/* [한국어] 얻지 못했으면 */
			return npages;	/* [한국어] 오류를 올린다. */
		if (WARN_ON(npages != 1))	/* [한국어] 있을 수 없는 결과. */
			return -EFAULT;	/* [한국어] 실패. */
		if (!PageCompound(pages[0])) {	/* [한국어] 복합(큰) 페이지는 참조가 머리 페이지에 몰려 세는 방식이 달라 건너뛴다. */
			unsigned int count;	/* [한국어] 참조 수. */

			count = page_ref_count(pages[0]);	/* [한국어] 지금 참조 수를 읽는다. */
			if (count / GUP_PIN_COUNTING_BIAS != refs) {	/* [한국어] 고정은 1 이 아니라 큰 값만큼 올린다. 그 값으로 나누면 고정 횟수가 나온다. */
				put_page(pages[0]);	/* [한국어] 참조를 놓고 */
				return -EIO;	/* [한국어] 기대와 다르다 — 고정이 새거나 이중으로 잡힌 것이다. */
			}
		}
		put_page(pages[0]);	/* [한국어] 확인용 참조를 놓는다. */
		uptr += PAGE_SIZE;	/* [한국어] 다음 페이지로. */
	}
	return 0;	/* [한국어] 모두 일치했다. */
}

/*
 * [한국어]
 * iommufd_test_md_check_iotlb - MD_CHECK_IOTLB 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @mockpt_id: 검사할 중첩 도메인의 id.
 * @iotlb_id: 검사할 IOTLB 항목의 첨자.
 * @iotlb: 그 항목이 가져야 할 값.
 * @return: 0 이면 일치, 어긋나면 -EINVAL.
 *
 * 무효화 명령이 실제로 그 항목까지 닿았는지 확인한다. 모의 IOTLB 는
 * 숫자 배열일 뿐이지만, 무효화 경로의 도달 여부를 보이게 해 준다.
 */
static int iommufd_test_md_check_iotlb(struct iommufd_ucmd *ucmd, u32 mockpt_id,
				       unsigned int iotlb_id, u32 iotlb)
{
	struct mock_iommu_domain_nested *mock_nested;	/* [한국어] 검사할 중첩 도메인. */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 그 코어 도메인. */
	int rc = 0;	/* [한국어] 결과 코드. */

	hwpt = get_md_pagetable_nested(ucmd, mockpt_id, &mock_nested);	/* [한국어] 중첩 도메인을 찾는다. */
	if (IS_ERR(hwpt))	/* [한국어] 없으면 */
		return PTR_ERR(hwpt);	/* [한국어] 오류를 올린다. */

	mock_nested = to_mock_nested(hwpt->domain);	/* [한국어] 위에서 이미 받았지만 한 번 더 되짚는다. */

	if (iotlb_id > MOCK_NESTED_DOMAIN_IOTLB_ID_MAX ||	/* [한국어] 범위를 벗어난 첨자이거나 */
	    mock_nested->iotlb[iotlb_id] != iotlb)	/* [한국어] 값이 기대와 다르면 */
		rc = -EINVAL;	/* [한국어] 어긋났다고 알린다. */
	iommufd_put_object(ucmd->ictx, &hwpt->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_test_dev_check_cache - DEV_CHECK_CACHE 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @idev_id: 검사할 장치의 id.
 * @cache_id: 검사할 캐시 항목의 첨자.
 * @cache: 그 항목이 가져야 할 값.
 * @return: 0 이면 일치, 어긋나면 -EINVAL.
 *
 * 위와 같되 vIOMMU 단위 무효화가 장치별 캐시까지 닿았는지 본다.
 */
static int iommufd_test_dev_check_cache(struct iommufd_ucmd *ucmd, u32 idev_id,
					unsigned int cache_id, u32 cache)
{
	struct iommufd_device *idev;	/* [한국어] 검사할 장치. */
	struct mock_dev *mdev;	/* [한국어] 그 모의 장치. */
	int rc = 0;	/* [한국어] 결과 코드. */

	idev = iommufd_get_device(ucmd, idev_id);	/* [한국어] id 로 장치를 찾는다. */
	if (IS_ERR(idev))	/* [한국어] 없으면 */
		return PTR_ERR(idev);	/* [한국어] 오류를 올린다. */
	mdev = container_of(idev->dev, struct mock_dev, dev);	/* [한국어] 모의 장치로 되짚는다. */

	if (cache_id > MOCK_DEV_CACHE_ID_MAX || mdev->cache[cache_id] != cache)	/* [한국어] 범위를 벗어났거나 값이 다르면 */
		rc = -EINVAL;	/* [한국어] 어긋났다고 알린다. */
	iommufd_put_object(ucmd->ictx, &idev->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어] 테스트가 만든 접근자와 그 상태.
 *
 * 접근자 자체는 iommufd 의 것이고, 이 구조체는 테스트가 그것을 조종하려고
 * 덧붙인 껍데기다. 고정해 둔 구간들을 목록으로 들고 있어, 나중에 하나씩
 * 지목해 놓을 수 있다.
 *
 * 파일 디스크립터로 노출되므로 사용자가 close 만 해도 정리된다.
 */
struct selftest_access {
	/* [한국어] iommufd 쪽 접근자.
	 *  설정자: CREATE_ACCESS 가 만든 뒤 매단다.
	 *  값 범위: NULL 이면 아직 만들지 않았다는 뜻이라, 파일을 닫아도 정리할 것이 없다. */
	struct iommufd_access *access;
	/* [한국어] 이 껍데기를 가리키는 익명 파일.
	 *  설정자: iommufd_test_alloc_access.
	 *  읽는 자: fd 로 되찾을 때와, 참조를 놓을 때.
	 *  이 파일의 수명이 곧 껍데기의 수명이다. */
	struct file *file;
	/* [한국어] 아래 목록과 고정 연산을 지키는 뮤텍스.
	 *  고정하는 동안 unmap 알림이 끼어들지 못하게 하는 역할도 한다 —
	 *  원 주석이 경고하듯 진짜 드라이버는 이보다 정교해야 한다. */
	struct mutex lock;
	/* [한국어] 고정해 둔 구간들의 목록.
	 *  설정자: ACCESS_PAGES 가 넣고, 해제 명령과 unmap 알림이 뺀다. */
	struct list_head items;
	/* [한국어] 다음 항목에 붙일 번호.
	 *  되돌리지 않고 늘어나기만 한다 — 같은 번호가 두 번 쓰이면 사용자가
	 *  엉뚱한 항목을 지목하게 된다. */
	unsigned int next_id;
	/* [한국어] 파괴 중임을 표시하는 자리.
	 *  지금 코드는 세우지 않지만, 파일이 닫히는 중에 새 고정을 막는
	 *  용도로 남겨 둔 필드다. */
	bool destroying;
};

/*
 * [한국어] 고정해 둔 구간 하나.
 *
 * 사용자가 id 로 지목해 놓을 수 있게 번호를 붙여 둔다. 매핑이 풀릴 때
 * 이 목록을 훑어 겹치는 것을 찾아 놓는다.
 */
struct selftest_access_item {
	/* [한국어] 위 목록에 매다는 고리. */
	struct list_head items_elm;
	/* [한국어] 고정한 구간의 시작 IOVA.
	 *  놓을 때 정확히 같은 값을 넘겨야 짝이 맞는다. */
	unsigned long iova;
	/* [한국어] 그 길이. 역시 놓을 때 같아야 한다. */
	size_t length;
	/* [한국어] 사용자가 이 항목을 지목할 번호.
	 *  설정자: 만들 때 next_id 에서 받는다. */
	unsigned int id;
};

static const struct file_operations iommfd_test_staccess_fops;	/* [한국어] 아래 정의의 전방 선언. 위쪽의 판별 코드가 그 주소를 쓰기 때문이다. */

/*
 * [한국어]
 * iommufd_access_get - fd 로 테스트 접근자를 찾는다
 *
 * @fd: 사용자가 준 파일 디스크립터.
 * @return: 그 접근자, 아니면 오류 포인터.
 *
 * 파일 연산표를 비교해 우리 파일인지 확인한다. 그러지 않으면 사용자가
 * 아무 fd 나 주어 남의 구조체를 우리 것으로 해석하게 만들 수 있다.
 *
 * 참조를 든 채 돌려주므로 호출자가 fput 해야 한다.
 */
static struct selftest_access *iommufd_access_get(int fd)
{
	struct file *file;	/* [한국어] fd 가 가리키는 파일. */

	file = fget(fd);	/* [한국어] 참조를 들며 얻는다. */
	if (!file)	/* [한국어] 없는 fd 다. */
		return ERR_PTR(-EBADFD);	/* [한국어] 거절. */

	if (file->f_op != &iommfd_test_staccess_fops) {	/* [한국어] 우리 파일이 아니면 */
		fput(file);	/* [한국어] 참조를 놓고 */
		return ERR_PTR(-EBADFD);	/* [한국어] 거절한다. 그러지 않으면 남의 구조체를 우리 것으로 해석하게 된다. */
	}
	return file->private_data;	/* [한국어] 만들 때 매달아 둔 껍데기. 참조는 호출자가 fput 으로 놓는다. */
}

/*
 * [한국어]
 * iommufd_test_access_unmap - 매핑이 풀린다는 알림을 받는다
 *
 * @data: 접근자를 만들 때 준 값(= 우리 껍데기).
 * @iova: 풀리는 구간의 시작.
 * @length: 그 길이.
 *
 * 겹치는 고정을 모두 놓는다. 이 함수가 돌아온 뒤에는 그 구간을 붙잡은
 * 것이 없어야 한다는 것이 iommufd 와의 약속이다.
 *
 * 겹침 판정이 반대 조건으로 적혀 있다 — "겹치지 않으면 건너뛴다"가
 * 겹침을 직접 쓰는 것보다 짧다.
 */
static void iommufd_test_access_unmap(void *data, unsigned long iova,
				      unsigned long length)
{
	unsigned long iova_last = iova + length - 1;	/* [한국어] 풀리는 구간의 마지막 주소. */
	struct selftest_access *staccess = data;	/* [한국어] 접근자를 만들 때 준 값. */
	struct selftest_access_item *item;	/* [한국어] 훑을 고정 항목. */
	struct selftest_access_item *tmp;	/* [한국어] 지우며 돌기 위한 다음 원소. */

	mutex_lock(&staccess->lock);	/* [한국어] 목록을 지키는 뮤텍스. */
	list_for_each_entry_safe(item, tmp, &staccess->items, items_elm) {	/* [한국어] 고정해 둔 구간들을 훑는다. */
		if (iova > item->iova + item->length - 1 ||	/* [한국어] 이 항목보다 뒤에서 시작하거나 */
		    iova_last < item->iova)	/* [한국어] 앞에서 끝나면 겹치지 않는다. */
			continue;	/* [한국어] 건드리지 않는다. */
		list_del(&item->items_elm);	/* [한국어] 목록에서 뺀다. */
		iommufd_access_unpin_pages(staccess->access, item->iova,	/* [한국어] 고정을 놓는다. 이 함수가 돌아온 뒤에는 붙잡은 것이 없어야 한다. */
					   item->length);
		kfree(item);	/* [한국어] 항목을 해제한다. */
	}
	mutex_unlock(&staccess->lock);	/* [한국어] 락 해제. */
}

/*
 * [한국어]
 * iommufd_test_access_item_destroy - DESTROY_ACCESS_PAGES 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @access_id: 접근자의 fd.
 * @item_id: 놓을 고정 묶음의 id.
 * @return: 0 성공, 그런 id 가 없으면 -ENOENT.
 *
 * 목록에서 찾아 놓는다. 뮤텍스를 놓은 뒤에 해제하는 순서에 주의 —
 * 목록에서 뺀 뒤에는 다른 스레드가 이 항목을 볼 수 없다.
 */
static int iommufd_test_access_item_destroy(struct iommufd_ucmd *ucmd,
					    unsigned int access_id,
					    unsigned int item_id)
{
	struct selftest_access_item *item;	/* [한국어] 찾을 항목. */
	struct selftest_access *staccess;	/* [한국어] 대상 접근자. */

	staccess = iommufd_access_get(access_id);	/* [한국어] fd 로 찾는다. */
	if (IS_ERR(staccess))	/* [한국어] 없으면 */
		return PTR_ERR(staccess);	/* [한국어] 오류를 올린다. */

	mutex_lock(&staccess->lock);	/* [한국어] 목록을 지키는 뮤텍스. */
	list_for_each_entry(item, &staccess->items, items_elm) {	/* [한국어] 항목을 훑으며 */
		if (item->id == item_id) {	/* [한국어] 그 번호를 찾으면 */
			list_del(&item->items_elm);	/* [한국어] 목록에서 빼고 */
			iommufd_access_unpin_pages(staccess->access, item->iova,	/* [한국어] 고정을 놓는다. */
						   item->length);
			mutex_unlock(&staccess->lock);	/* [한국어] 락을 먼저 놓는다 — 목록에서 뺀 뒤라 다른 스레드가 볼 수 없다. */
			kfree(item);	/* [한국어] 항목을 해제한다. */
			fput(staccess->file);	/* [한국어] 조회용 참조를 놓는다. */
			return 0;	/* [한국어] 성공. */
		}
	}
	mutex_unlock(&staccess->lock);	/* [한국어] 찾지 못했다. */
	fput(staccess->file);	/* [한국어] 참조를 놓고 */
	return -ENOENT;	/* [한국어] 그런 번호가 없다. */
}

/*
 * [한국어]
 * iommufd_test_staccess_release - 접근자 파일이 닫힐 때
 *
 * @inode: 닫히는 노드.
 * @filep: 그 파일.
 * @return: 늘 0.
 *
 * 남은 고정을 모두 놓고 접근자를 없앤다. 사용자가 정리를 잊어도
 * 프로세스가 끝나면 여기서 걷힌다.
 */
static int iommufd_test_staccess_release(struct inode *inode,
					 struct file *filep)
{
	struct selftest_access *staccess = filep->private_data;	/* [한국어] 매달아 둔 껍데기. */

	if (staccess->access) {	/* [한국어] 접근자를 실제로 만들었으면 */
		iommufd_test_access_unmap(staccess, 0, ULONG_MAX);	/* [한국어] 남은 고정을 모두 놓는다. 전체 범위를 주어 하나도 남기지 않는다. */
		iommufd_access_destroy(staccess->access);	/* [한국어] 접근자를 없앤다. */
	}
	mutex_destroy(&staccess->lock);	/* [한국어] 뮤텍스 파괴를 기록한다. */
	kfree(staccess);	/* [한국어] 껍데기를 해제한다. */
	return 0;	/* [한국어] close 는 실패하지 않는다. */
}

/*
 * [한국어] 페이지 고정을 쓰는 접근자의 연산표.
 *
 * needs_pin_pages 가 참이면 iommufd 가 IOVA 정렬을 페이지 단위로 강제한다.
 */
static const struct iommufd_access_ops selftest_access_ops_pin = {
	.needs_pin_pages = 1,	/* [한국어] 이 접근자가 고정을 쓸 것임을 알린다. iommufd 가 IOVA 정렬을 페이지 단위로 강제한다. */
	.unmap = iommufd_test_access_unmap,
};

/*
 * [한국어] 고정을 쓰지 않는 접근자의 연산표.
 *
 * 읽고 쓰기만 하므로 정렬 제약이 느슨하다. 두 성격을 갈라 각 경로를
 * 시험한다.
 */
static const struct iommufd_access_ops selftest_access_ops = {
	.unmap = iommufd_test_access_unmap,	/* [한국어] 매핑이 풀릴 때 받을 알림. 고정을 쓰지 않아도 이 콜백은 필요하다. */
};

/*
 * [한국어] 접근자 파일의 연산표.
 *
 * release 하나뿐이다 — 이 파일은 읽고 쓰는 용도가 아니라, 접근자를
 * 가리키는 손잡이이자 수명 관리 수단이다.
 *
 * 그 주소가 곧 "우리 파일인가"의 판별에도 쓰인다.
 */
static const struct file_operations iommfd_test_staccess_fops = {
	.release = iommufd_test_staccess_release,	/* [한국어] 파일이 닫힐 때의 정리. */
};

/*
 * [한국어]
 * iommufd_test_alloc_access - 테스트 접근자 껍데기를 만든다
 *
 * @return: 만든 껍데기, 실패하면 오류 포인터.
 *
 * 익명 파일을 함께 만들어 매달아 둔다. 아직 fd 에 설치하지 않으므로
 * 실패해도 사용자가 모르는 디스크립터가 남지 않는다.
 */
static struct selftest_access *iommufd_test_alloc_access(void)
{
	struct selftest_access *staccess;	/* [한국어] 만들 껍데기. */
	struct file *filep;	/* [한국어] 매달 익명 파일. */

	staccess = kzalloc_obj(*staccess, GFP_KERNEL_ACCOUNT);	/* [한국어] 사용자 앞으로 계산되게 잡는다. */
	if (!staccess)	/* [한국어] 메모리가 없다. */
		return ERR_PTR(-ENOMEM);	/* [한국어] 실패. */
	INIT_LIST_HEAD(&staccess->items);	/* [한국어] 고정 목록을 비운다. */
	mutex_init(&staccess->lock);	/* [한국어] 그 목록을 지킬 뮤텍스. */

	filep = anon_inode_getfile("[iommufd_test_staccess]",	/* [한국어] 파일 시스템에 이름 없는 파일. private_data 로 껍데기를 매단다. */
				   &iommfd_test_staccess_fops, staccess,
				   O_RDWR);
	if (IS_ERR(filep)) {	/* [한국어] 만들지 못했으면 */
		kfree(staccess);	/* [한국어] 껍데기를 버리고 */
		return ERR_CAST(filep);	/* [한국어] 오류를 올린다. */
	}
	staccess->file = filep;	/* [한국어] 나중에 fd 에 설치할 때 쓴다. */
	return staccess;	/* [한국어] 만든 껍데기. */
}

/*
 * [한국어]
 * iommufd_test_create_access - CREATE_ACCESS 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @ioas_id: 붙일 IOAS 의 id.
 * @flags: 접근자의 성격.
 * @return: 0 성공, 음수면 실패.
 *
 * vfio 의 에뮬레이션 장치가 쓰는 경로를 흉내 낸다 — CPU 로 게스트
 * 메모리를 들여다보는 길이다.
 *
 * fd 번호를 잡되 마지막에야 설치하는 것이 요점이다. 사용자에게 응답을
 * 보내는 데 실패하면 그 번호를 반납해야 하기 때문이다.
 */
static int iommufd_test_create_access(struct iommufd_ucmd *ucmd,
				      unsigned int ioas_id, unsigned int flags)
{
	struct iommu_test_cmd *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */
	struct selftest_access *staccess;	/* [한국어] 테스트 쪽 껍데기. */
	struct iommufd_access *access;	/* [한국어] iommufd 쪽 접근자. */
	u32 id;	/* [한국어] 그 객체 id. */
	int fdno;	/* [한국어] 배정받은 fd 번호. */
	int rc;	/* [한국어] 결과 코드. */

	if (flags & ~MOCK_FLAGS_ACCESS_CREATE_NEEDS_PIN_PAGES)	/* [한국어] 아는 플래그는 하나뿐이다. */
		return -EOPNOTSUPP;	/* [한국어] 거절. */

	staccess = iommufd_test_alloc_access();	/* [한국어] 껍데기와 파일을 만든다. */
	if (IS_ERR(staccess))	/* [한국어] 실패하면 */
		return PTR_ERR(staccess);	/* [한국어] 오류를 올린다. */

	fdno = get_unused_fd_flags(O_CLOEXEC);	/* [한국어] 번호만 잡아 둔다. 아직 설치하지 않는다. */
	if (fdno < 0) {	/* [한국어] 번호가 없으면 */
		rc = -ENOMEM;	/* [한국어] 실패. */
		goto out_free_staccess;	/* [한국어] 파일을 놓는다. */
	}

	access = iommufd_access_create(	/* [한국어] 접근자를 만든다. */
		ucmd->ictx,
		(flags & MOCK_FLAGS_ACCESS_CREATE_NEEDS_PIN_PAGES) ?	/* [한국어] 고정을 쓸 것인지에 따라 */
			&selftest_access_ops_pin :	/* [한국어] unmap 알림을 받는 연산표이거나 */
			&selftest_access_ops,	/* [한국어] 그렇지 않은 연산표를 준다. */
		staccess, &id);
	if (IS_ERR(access)) {	/* [한국어] 만들지 못했으면 */
		rc = PTR_ERR(access);	/* [한국어] 오류를 꺼내 */
		goto out_put_fdno;	/* [한국어] 번호를 반납한다. */
	}
	rc = iommufd_access_attach(access, ioas_id);	/* [한국어] 지정한 주소 공간에 붙인다. */
	if (rc)	/* [한국어] 접근자를 주소 공간에 붙이지 못했다. */
		goto out_destroy;	/* [한국어] 실패하면 접근자를 없앤다. */
	cmd->create_access.out_access_fd = fdno;	/* [한국어] 사용자에게 알릴 fd 번호. */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 되돌려 쓴다. */
	if (rc)	/* [한국어] 사용자에게 fd 번호를 전하지 못했다. */
		goto out_destroy;	/* [한국어] 전하지 못했으면 되돌린다. */

	staccess->access = access;	/* [한국어] 껍데기에 접근자를 매단다. 이 뒤로 release 가 정리를 맡는다. */
	fd_install(fdno, staccess->file);	/* [한국어] 이제야 번호에 파일을 붙인다. */
	return 0;	/* [한국어] 성공. */

out_destroy:	/* [한국어] 접근자를 없애야 하는 경로. */
	iommufd_access_destroy(access);	/* [한국어] 없앤다. */
out_put_fdno:	/* [한국어] 번호를 반납해야 하는 경로. */
	put_unused_fd(fdno);	/* [한국어] 반납한다. */
out_free_staccess:	/* [한국어] 파일을 놓아야 하는 경로. */
	fput(staccess->file);	/* [한국어] 마지막 참조라 release 가 불려 껍데기까지 정리된다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * iommufd_test_access_replace_ioas - ACCESS_REPLACE_IOAS 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @access_id: 접근자의 fd.
 * @ioas_id: 새 IOAS 의 id.
 * @return: 0 성공, 음수면 실패.
 *
 * 바꾸는 순간 고정해 둔 페이지들이 어떻게 되는지가 시험의 초점이다.
 */
static int iommufd_test_access_replace_ioas(struct iommufd_ucmd *ucmd,
					    unsigned int access_id,
					    unsigned int ioas_id)
{
	struct selftest_access *staccess;	/* [한국어] 대상 접근자. */
	int rc;	/* [한국어] 결과 코드. */

	staccess = iommufd_access_get(access_id);	/* [한국어] fd 로 찾는다. */
	if (IS_ERR(staccess))	/* [한국어] 없으면 */
		return PTR_ERR(staccess);	/* [한국어] 오류를 올린다. */

	rc = iommufd_access_replace(staccess->access, ioas_id);	/* [한국어] 새 주소 공간으로 바꾼다. */
	fput(staccess->file);	/* [한국어] 조회용 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/* Check that the pages in a page array match the pages in the user VA */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_test_check_pages - 받아 온 페이지가 그 사용자 메모리인지 본다
 *
 * @uptr: 기대하는 사용자 메모리.
 * @pages: 접근자가 돌려준 페이지 배열.
 * @npages: 그 개수.
 * @return: 0 이면 일치, 어긋나면 -EBADE.
 *
 * 같은 페이지 구조체를 가리켜야 한다. 다르면 접근자가 엉뚱한 메모리를
 * 가져온 것이다.
 */
static int iommufd_test_check_pages(void __user *uptr, struct page **pages,
				    size_t npages)
{
	for (; npages; npages--) {	/* [한국어] 페이지마다. */
		struct page *tmp_pages[1];	/* [한국어] 비교용으로 얻을 페이지. */
		long rc;	/* [한국어] 얻은 개수 또는 오류. */

		rc = get_user_pages_fast((uintptr_t)uptr, 1, 0, tmp_pages);	/* [한국어] 사용자 주소의 페이지를 잠시 얻는다. */
		if (rc < 0)	/* [한국어] 얻지 못했으면 */
			return rc;	/* [한국어] 오류를 올린다. */
		if (WARN_ON(rc != 1))	/* [한국어] 있을 수 없는 결과. */
			return -EFAULT;	/* [한국어] 실패. */
		put_page(tmp_pages[0]);	/* [한국어] 비교만 하면 되므로 곧바로 놓는다. */
		if (tmp_pages[0] != *pages)	/* [한국어] 같은 페이지 구조체를 가리켜야 한다. */
			return -EBADE;	/* [한국어] 다르면 접근자가 엉뚱한 메모리를 가져온 것이다. */
		pages++;	/* [한국어] 다음 페이지. */
		uptr += PAGE_SIZE;	/* [한국어] 사용자 쪽도 함께. */
	}
	return 0;	/* [한국어] 모두 일치했다. */
}

/*
 * [한국어]
 * iommufd_test_access_pages - ACCESS_PAGES 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @access_id: 접근자의 fd.
 * @iova: 고정할 구간의 시작.
 * @length: 그 길이.
 * @uptr: 확인용 사용자 메모리(NULL 이면 확인을 건너뛴다).
 * @flags: 접근 성격.
 * @return: 0 성공, 음수면 실패.
 *
 * 원 주석이 드라이버에게 주는 경고가 이 함수의 핵심이다 — 고정하는 동안
 * 코어가 여러 번의 해제를 동시에 걸 수 있고, 그 해제들은 모두 고정이
 * 풀릴 때까지 돌아가지 못한다. 여기서는 단순히 전역 뮤텍스로 감쌌지만,
 * 성능이 중요한 드라이버라면 그렇게 할 수 없다. 잘못 짜면 WARN 과
 * EDEADLOCK 이 사용자에게 나타난다.
 *
 * 크기 상한은 퍼저가 거대한 할당을 요청해 경고를 내는 것을 막는다.
 */
static int iommufd_test_access_pages(struct iommufd_ucmd *ucmd,
				     unsigned int access_id, unsigned long iova,
				     size_t length, void __user *uptr,
				     u32 flags)
{
	struct iommu_test_cmd *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */
	struct selftest_access_item *item;	/* [한국어] 만들 고정 항목. */
	struct selftest_access *staccess;	/* [한국어] 대상 접근자. */
	struct page **pages;	/* [한국어] 받아 올 페이지 배열. */
	size_t npages;	/* [한국어] 그 개수. */
	int rc;	/* [한국어] 결과 코드. */

	/* Prevent syzkaller from triggering a WARN_ON in kvzalloc() */
	if (length > 16 * 1024 * 1024)	/* [한국어] 원 주석대로 퍼저가 거대한 할당을 요청해 경고를 내는 것을 막는다. */
		return -ENOMEM;	/* [한국어] 거절. */

	if (flags & ~(MOCK_FLAGS_ACCESS_WRITE | MOCK_FLAGS_ACCESS_SYZ))	/* [한국어] 아는 플래그가 아니면 */
		return -EOPNOTSUPP;	/* [한국어] 거절. */

	staccess = iommufd_access_get(access_id);	/* [한국어] fd 로 접근자를 찾는다. */
	if (IS_ERR(staccess))	/* [한국어] 없으면 */
		return PTR_ERR(staccess);	/* [한국어] 오류를 올린다. */

	if (staccess->access->ops != &selftest_access_ops_pin) {	/* [한국어] 고정을 쓰지 않는 접근자면 */
		rc = -EOPNOTSUPP;	/* [한국어] 이 명령을 받을 수 없다. */
		goto out_put;	/* [한국어] 참조를 놓고 나간다. */
	}

	if (flags & MOCK_FLAGS_ACCESS_SYZ)	/* [한국어] 퍼저 모드면 */
		iova = iommufd_test_syz_conv_iova(staccess->access,	/* [한국어] 아무 값을 유효한 IOVA 로 바꾼다. */
						  &cmd->access_pages.iova);

	npages = (ALIGN(iova + length, PAGE_SIZE) -	/* [한국어] 구간이 걸치는 호스트 페이지 수를 센다. */
		  ALIGN_DOWN(iova, PAGE_SIZE)) /	/* [한국어] 시작을 내리고 끝을 올려 */
		 PAGE_SIZE;	/* [한국어] 그 차이를 페이지 크기로 나눈다. */
	pages = kvzalloc_objs(*pages, npages, GFP_KERNEL_ACCOUNT);	/* [한국어] 페이지 배열을 잡는다. 클 수 있어 vmalloc 으로 물러설 수 있는 판을 쓴다. */
	if (!pages) {	/* [한국어] 메모리가 없다. */
		rc = -ENOMEM;	/* [한국어] 실패. */
		goto out_put;	/* [한국어] 나간다. */
	}

	/*
	 * Drivers will need to think very carefully about this locking. The
	 * core code can do multiple unmaps instantaneously after
	 * iommufd_access_pin_pages() and *all* the unmaps must not return until
	 * the range is unpinned. This simple implementation puts a global lock
	 * around the pin, which may not suit drivers that want this to be a
	 * performance path. drivers that get this wrong will trigger WARN_ON
	 * races and cause EDEADLOCK failures to userspace.
	 */
	mutex_lock(&staccess->lock);	/* [한국어] 원 주석의 경고대로, 고정을 전역 락으로 감싼다. 성능이 중요한 드라이버라면 이렇게 할 수 없다. */
	rc = iommufd_access_pin_pages(staccess->access, iova, length, pages,	/* [한국어] 구간의 페이지를 고정해 배열에 받는다. */
				      flags & MOCK_FLAGS_ACCESS_WRITE);
	if (rc)	/* [한국어] 실패하면 */
		goto out_unlock;	/* [한국어] 락을 놓고 나간다. */

	/* For syzkaller allow uptr to be NULL to skip this check */
	if (uptr) {	/* [한국어] 원 주석대로 퍼저를 위해 NULL 을 허용한다. */
		rc = iommufd_test_check_pages(	/* [한국어] 받아 온 페이지가 그 사용자 메모리인지 확인한다. */
			uptr - (iova - ALIGN_DOWN(iova, PAGE_SIZE)), pages,	/* [한국어] IOVA 를 페이지 경계로 내린 만큼 사용자 주소도 내려 맞춘다. */
			npages);
		if (rc)	/* [한국어] 받아 온 페이지가 기대한 메모리가 아니었다. */
			goto out_unaccess;	/* [한국어] 어긋나면 고정을 놓는다. */
	}

	item = kzalloc_obj(*item, GFP_KERNEL_ACCOUNT);	/* [한국어] 고정을 기억할 항목. */
	if (!item) {	/* [한국어] 메모리가 없다. */
		rc = -ENOMEM;	/* [한국어] 실패. */
		goto out_unaccess;	/* [한국어] 고정을 놓는다. */
	}

	item->iova = iova;	/* [한국어] 고정한 구간의 시작. */
	item->length = length;	/* [한국어] 그 길이. 놓을 때 정확히 같은 값을 줘야 한다. */
	item->id = staccess->next_id++;	/* [한국어] 사용자가 지목할 번호. */
	list_add_tail(&item->items_elm, &staccess->items);	/* [한국어] 목록에 넣는다. 이 뒤로 unmap 알림이 이것을 찾아 놓는다. */

	cmd->access_pages.out_access_pages_id = item->id;	/* [한국어] 사용자에게 알릴 번호. */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 되돌려 쓴다. */
	if (rc)	/* [한국어] 사용자에게 항목 번호를 전하지 못했다. */
		goto out_free_item;	/* [한국어] 전하지 못했으면 항목을 되돌린다. */
	goto out_unlock;	/* [한국어] 성공 경로. */

out_free_item:	/* [한국어] 응답 실패 경로. */
	list_del(&item->items_elm);	/* [한국어] 목록에서 빼고 */
	kfree(item);	/* [한국어] 해제한다. */
out_unaccess:	/* [한국어] 확인 실패도 여기로 합류한다. */
	iommufd_access_unpin_pages(staccess->access, iova, length);	/* [한국어] 고정을 놓는다. */
out_unlock:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	mutex_unlock(&staccess->lock);	/* [한국어] 락 해제. */
	kvfree(pages);	/* [한국어] 배열을 해제한다. 페이지 자체는 고정이 붙잡고 있다. */
out_put:	/* [한국어] 접근자 참조를 놓아야 하는 경로. */
	fput(staccess->file);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_test_access_rw - ACCESS_RW 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @access_id: 접근자의 fd.
 * @iova: 읽거나 쓸 구간의 시작.
 * @length: 그 길이.
 * @ubuf: 반대편이 되는 사용자 버퍼.
 * @flags: 방향과 경로 선택.
 * @return: 0 성공, 음수면 실패.
 *
 * 고정 없이 그때그때 페이지를 찾아 복사하는 경로를 시험한다.
 *
 * 커널 임시 버퍼를 거치는 이유: iommufd 의 접근 API 는 커널 버퍼를
 * 받으므로, 사용자 버퍼를 직접 넘길 수 없다.
 *
 * 아래 static_assert 둘이 테스트 플래그와 실제 플래그가 같은 값인지
 * 컴파일 시 확인한다 — 그래야 그대로 넘길 수 있다.
 */
static int iommufd_test_access_rw(struct iommufd_ucmd *ucmd,
				  unsigned int access_id, unsigned long iova,
				  size_t length, void __user *ubuf,
				  unsigned int flags)
{
	struct iommu_test_cmd *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */
	struct selftest_access *staccess;	/* [한국어] 대상 접근자. */
	void *tmp;	/* [한국어] 커널 임시 버퍼. */
	int rc;	/* [한국어] 결과 코드. */

	/* Prevent syzkaller from triggering a WARN_ON in kvzalloc() */
	if (length > 16 * 1024 * 1024)	/* [한국어] 퍼저가 거대한 할당을 요청하는 것을 막는다. */
		return -ENOMEM;	/* [한국어] 거절. */

	if (flags & ~(MOCK_ACCESS_RW_WRITE | MOCK_ACCESS_RW_SLOW_PATH |	/* [한국어] 아는 플래그가 아니면 */
		      MOCK_FLAGS_ACCESS_SYZ))	/* [한국어] 거절한다. */
		return -EOPNOTSUPP;	/* [한국어] 지원하지 않는다. */

	staccess = iommufd_access_get(access_id);	/* [한국어] fd 로 접근자를 찾는다. */
	if (IS_ERR(staccess))	/* [한국어] 없으면 */
		return PTR_ERR(staccess);	/* [한국어] 오류를 올린다. */

	tmp = kvzalloc(length, GFP_KERNEL_ACCOUNT);	/* [한국어] iommufd 의 접근 API 는 커널 버퍼를 받으므로 한 번 거쳐야 한다. */
	if (!tmp) {	/* [한국어] 메모리가 없다. */
		rc = -ENOMEM;	/* [한국어] 실패. */
		goto out_put;	/* [한국어] 나간다. */
	}

	if (flags & MOCK_ACCESS_RW_WRITE) {	/* [한국어] 쓰기면 */
		if (copy_from_user(tmp, ubuf, length)) {	/* [한국어] 사용자 버퍼에서 먼저 가져온다. */
			rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */
			goto out_free;	/* [한국어] 버퍼를 놓고 나간다. */
		}
	}

	if (flags & MOCK_FLAGS_ACCESS_SYZ)	/* [한국어] 퍼저 모드면 */
		iova = iommufd_test_syz_conv_iova(staccess->access,	/* [한국어] 유효한 IOVA 로 바꾼다. */
						  &cmd->access_rw.iova);

	rc = iommufd_access_rw(staccess->access, iova, tmp, length, flags);	/* [한국어] 실제 읽기·쓰기. 플래그 값이 같아 그대로 넘긴다. */
	if (rc)	/* [한국어] 실패하면 */
		goto out_free;	/* [한국어] 나간다. */
	if (!(flags & MOCK_ACCESS_RW_WRITE)) {	/* [한국어] 읽기였으면 */
		if (copy_to_user(ubuf, tmp, length)) {	/* [한국어] 사용자 버퍼로 옮긴다. */
			rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */
			goto out_free;	/* [한국어] 나간다. */
		}
	}

out_free:	/* [한국어] 버퍼를 놓아야 하는 경로. */
	kvfree(tmp);	/* [한국어] 임시 버퍼를 해제한다. */
out_put:	/* [한국어] 접근자 참조를 놓아야 하는 경로. */
	fput(staccess->file);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}
static_assert((unsigned int)MOCK_ACCESS_RW_WRITE == IOMMUFD_ACCESS_RW_WRITE);	/* [한국어] 테스트 플래그와 실제 플래그가 같은 값이어야 그대로 넘길 수 있다. */
static_assert((unsigned int)MOCK_ACCESS_RW_SLOW_PATH ==	/* [한국어] 느린 경로 플래그도 같은 값인지 확인한다. */
	      __IOMMUFD_ACCESS_RW_SLOW_PATH);

/*
 * [한국어]
 * iommufd_test_dirty - DIRTY 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @mockpt_id: 대상 도메인의 id.
 * @iova: 표시할 구간의 시작.
 * @length: 그 길이.
 * @page_size: 표시 단위가 되는 페이지 크기.
 * @uptr: 어느 페이지를 더럽힐지 고르는 사용자 비트맵.
 * @flags: 예약(지금은 쓰이지 않는다).
 * @return: 0 성공, 음수면 실패.
 *
 * 하드웨어가 페이지를 더럽혔다고 꾸며 낸다. 그 뒤 테스트가 정규 경로로
 * 더티 비트맵을 읽어, 표시한 것과 같은지 확인한다.
 *
 * 더티 추적이 꺼져 있으면 거절한다 — 켜지 않고 표시하는 것은 뜻이 없다.
 *
 * 사용자 비트맵을 커널 버퍼로 옮겨 쓰는 이유: 비트 연산은 unsigned long
 * 단위라, 바이트 단위로 온 사용자 데이터를 그대로 다룰 수 없다.
 */
static int iommufd_test_dirty(struct iommufd_ucmd *ucmd, unsigned int mockpt_id,
			      unsigned long iova, size_t length,
			      unsigned long page_size, void __user *uptr,
			      u32 flags)
{
	unsigned long i, max;	/* [한국어] 비트맵 순회용 첨자와 전체 비트 수. */
	struct iommu_test_cmd *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 대상 도메인. */
	struct mock_iommu_domain *mock;	/* [한국어] 그 모의 도메인. */
	int rc, count = 0;	/* [한국어] 결과 코드와 실제로 표시한 개수. */
	void *tmp;	/* [한국어] 커널 쪽 비트맵 버퍼. */

	if (!page_size || !length || iova % page_size || length % page_size ||	/* [한국어] 크기가 0 이거나 정렬이 맞지 않거나 */
	    !uptr)	/* [한국어] 비트맵이 없으면 */
		return -EINVAL;	/* [한국어] 거절. */

	hwpt = get_md_pagetable(ucmd, mockpt_id, &mock);	/* [한국어] 도메인을 찾는다. */
	if (IS_ERR(hwpt))	/* [한국어] 없으면 */
		return PTR_ERR(hwpt);	/* [한국어] 오류를 올린다. */

	if (!(mock->flags & MOCK_DIRTY_TRACK) || !mock->iommu.ops->set_dirty) {	/* [한국어] 더티 추적이 꺼져 있거나 그 연산이 없으면 */
		rc = -EINVAL;	/* [한국어] 표시할 수 없다. */
		goto out_put;	/* [한국어] 참조를 놓고 나간다. */
	}

	max = length / page_size;	/* [한국어] 다룰 페이지 수 = 비트 수. */
	tmp = kvzalloc(DIV_ROUND_UP(max, BITS_PER_LONG) * sizeof(unsigned long),	/* [한국어] 비트 연산은 unsigned long 단위라 그 배수로 잡는다. */
		       GFP_KERNEL_ACCOUNT);
	if (!tmp) {	/* [한국어] 메모리가 없다. */
		rc = -ENOMEM;	/* [한국어] 실패. */
		goto out_put;	/* [한국어] 나간다. */
	}

	if (copy_from_user(tmp, uptr, DIV_ROUND_UP(max, BITS_PER_BYTE))) {	/* [한국어] 사용자 비트맵을 가져온다. 사용자 쪽은 바이트 단위다. */
		rc = -EFAULT;	/* [한국어] 사용자 주소 오류. */
		goto out_free;	/* [한국어] 버퍼를 놓고 나간다. */
	}

	for (i = 0; i < max; i++) {	/* [한국어] 비트마다. */
		if (!test_bit(i, (unsigned long *)tmp))	/* [한국어] 서지 않았으면 */
			continue;	/* [한국어] 그 페이지는 건드리지 않는다. */
		mock->iommu.ops->set_dirty(&mock->iommu, iova + i * page_size);	/* [한국어] 하드웨어가 그 페이지를 더럽힌 것처럼 페이지 테이블의 비트를 세운다. */
		count++;	/* [한국어] 표시한 개수를 센다. */
	}

	cmd->dirty.out_nr_dirty = count;	/* [한국어] 사용자에게 알린다. 나중에 읽어 온 개수와 견주게 된다. */
	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 되돌려 쓴다. */
out_free:	/* [한국어] 버퍼를 놓아야 하는 경로. */
	kvfree(tmp);	/* [한국어] 비트맵 버퍼를 해제한다. */
out_put:	/* [한국어] 참조를 놓아야 하는 경로. */
	iommufd_put_object(ucmd->ictx, &hwpt->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_test_trigger_iopf - TRIGGER_IOPF 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd: 그 인자.
 * @return: 늘 0.
 *
 * 폴트를 꾸며 내 iommu 코어에 올린다. 그러면 코어가 도메인의 폴트
 * 처리기를 부르고, 그것이 eventq.c 의 큐로 이어진다.
 *
 * LAST_PAGE 플래그를 늘 세우는 이유: 한 건짜리 묶음이라 그것이 마지막
 * 폴트다. 그러지 않으면 코어가 다음 폴트를 기다린다.
 *
 * 진짜 폴트를 재현하기 어려우므로, 이 명령이 그 경로를 시험하는 유일한
 * 방법이다.
 */
static int iommufd_test_trigger_iopf(struct iommufd_ucmd *ucmd,
				     struct iommu_test_cmd *cmd)
{
	struct iopf_fault event = {};	/* [한국어] 꾸며 낼 폴트. 남는 필드가 쓰레기가 되지 않게 0 으로. */
	struct iommufd_device *idev;	/* [한국어] 폴트를 낼 장치. */

	idev = iommufd_get_device(ucmd, cmd->trigger_iopf.dev_id);	/* [한국어] id 로 찾는다. */
	if (IS_ERR(idev))	/* [한국어] 없으면 */
		return PTR_ERR(idev);	/* [한국어] 오류를 올린다. */

	event.fault.prm.flags = IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE;	/* [한국어] 한 건짜리 묶음이라 이것이 마지막이다. 세우지 않으면 코어가 다음 폴트를 기다린다. */
	if (cmd->trigger_iopf.pasid != IOMMU_NO_PASID)	/* [한국어] PASID 를 지정했으면 */
		event.fault.prm.flags |= IOMMU_FAULT_PAGE_REQUEST_PASID_VALID;	/* [한국어] 그 필드가 유효함을 알린다. */
	event.fault.type = IOMMU_FAULT_PAGE_REQ;	/* [한국어] 페이지 요청 종류의 폴트. */
	event.fault.prm.addr = cmd->trigger_iopf.addr;	/* [한국어] 폴트가 난 것으로 할 주소. */
	event.fault.prm.pasid = cmd->trigger_iopf.pasid;	/* [한국어] 어느 주소 공간인지. */
	event.fault.prm.grpid = cmd->trigger_iopf.grpid;	/* [한국어] 응답이 짝을 찾을 묶음 번호. */
	event.fault.prm.perm = cmd->trigger_iopf.perm;	/* [한국어] 요구한 권한. */

	iommu_report_device_fault(idev->dev, &event);	/* [한국어] 코어에 올린다. 코어가 도메인의 폴트 처리기를 부르고, 그것이 eventq 로 이어진다. */
	iommufd_put_object(ucmd->ictx, &idev->obj);	/* [한국어] 참조를 놓는다. */

	return 0;	/* [한국어] 올리기는 실패하지 않는다. */
}

/*
 * [한국어]
 * iommufd_test_trigger_vevent - TRIGGER_VEVENT 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd: 그 인자.
 * @return: 0 성공, 그 장치가 vIOMMU 에 매여 있지 않으면 -ENOENT.
 *
 * vIOMMU 이벤트를 꾸며 내 큐에 넣는다. 여러 번 부르면 큐가 넘쳐
 * "잃어버림" 표시가 나타나므로, 그 경로도 함께 시험할 수 있다.
 *
 * 가상 장치 번호를 실어 보내는 것이 요점 — 게스트는 그 번호로만 장치를
 * 안다.
 */
static int iommufd_test_trigger_vevent(struct iommufd_ucmd *ucmd,
				       struct iommu_test_cmd *cmd)
{
	struct iommu_viommu_event_selftest test = {};	/* [한국어] 올릴 이벤트 내용. */
	struct iommufd_device *idev;	/* [한국어] 이벤트를 낼 장치. */
	struct mock_dev *mdev;	/* [한국어] 그 모의 장치. */
	int rc = -ENOENT;	/* [한국어] vIOMMU 에 매여 있지 않을 때의 기본값. */

	idev = iommufd_get_device(ucmd, cmd->trigger_vevent.dev_id);	/* [한국어] id 로 찾는다. */
	if (IS_ERR(idev))	/* [한국어] 없으면 */
		return PTR_ERR(idev);	/* [한국어] 오류를 올린다. */
	mdev = to_mock_dev(idev->dev);	/* [한국어] 모의 장치로 내려간다. */

	down_read(&mdev->viommu_rwsem);	/* [한국어] vIOMMU 포인터를 읽는 동안 바뀌지 않게 한다. */
	if (!mdev->viommu || !mdev->vdev_id)	/* [한국어] 중첩 도메인에 붙어 있지 않으면 */
		goto out_unlock;	/* [한국어] 올릴 곳이 없다. */

	test.virt_id = mdev->vdev_id;	/* [한국어] 게스트가 아는 번호를 싣는다. 호스트 id 를 전하면 게스트가 알아보지 못한다. */
	rc = iommufd_viommu_report_event(&mdev->viommu->core,	/* [한국어] 큐에 넣는다. 여러 번 부르면 넘쳐 잃어버림 표시가 나타난다. */
					 IOMMU_VEVENTQ_TYPE_SELFTEST, &test,
					 sizeof(test));
out_unlock:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	up_read(&mdev->viommu_rwsem);	/* [한국어] 락 해제. */
	iommufd_put_object(ucmd->ictx, &idev->obj);	/* [한국어] 참조를 놓는다. */

	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_get_hwpt - id 로 페이지 테이블을 찾는다(종류를 가리지 않고)
 *
 * @ucmd: 처리 중인 명령.
 * @id: 찾을 객체의 id.
 * @return: 참조를 든 도메인, 실패하면 오류 포인터.
 *
 * 페이징이든 중첩이든 받아들인다. PASID 검사에서 두 종류를 모두 다뤄야
 * 하기 때문이다.
 */
static inline struct iommufd_hw_pagetable *
iommufd_get_hwpt(struct iommufd_ucmd *ucmd, u32 id)
{
	struct iommufd_object *pt_obj;	/* [한국어] 찾은 객체. */

	pt_obj = iommufd_get_object(ucmd->ictx, id, IOMMUFD_OBJ_ANY);	/* [한국어] 종류를 가리지 않고 찾는다. */
	if (IS_ERR(pt_obj))	/* [한국어] 없으면 */
		return ERR_CAST(pt_obj);	/* [한국어] 오류를 올린다. */

	if (pt_obj->type != IOMMUFD_OBJ_HWPT_NESTED &&	/* [한국어] 중첩도 아니고 */
	    pt_obj->type != IOMMUFD_OBJ_HWPT_PAGING) {	/* [한국어] 페이징도 아니면 */
		iommufd_put_object(ucmd->ictx, pt_obj);	/* [한국어] 참조를 놓고 */
		return ERR_PTR(-EINVAL);	/* [한국어] 거절한다. */
	}

	return container_of(pt_obj, struct iommufd_hw_pagetable, obj);	/* [한국어] 도메인으로 되짚는다. */
}

/*
 * [한국어]
 * iommufd_test_pasid_check_hwpt - PASID_CHECK_HWPT 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd: 그 인자.
 * @return: 0 이면 기대대로, 어긋나면 -EINVAL.
 *
 * 그 PASID 가 기대한 도메인에 붙어 있는지 커널 쪽에서 확인한다.
 *
 * 원 주석대로 hwpt_id 가 0 이면 "떨어져 있어야 한다"는 뜻이다 — 0 은
 * 유효한 객체 id 가 아니라 그 자리를 이 뜻으로 쓸 수 있다.
 */
static int iommufd_test_pasid_check_hwpt(struct iommufd_ucmd *ucmd,
					 struct iommu_test_cmd *cmd)
{
	u32 hwpt_id = cmd->pasid_check.hwpt_id;	/* [한국어] 기대하는 도메인의 id. 0 이면 "떨어져 있어야 한다". */
	struct iommu_domain *attached_domain;	/* [한국어] 실제로 붙어 있는 도메인. */
	struct iommu_attach_handle *handle;	/* [한국어] 붙일 때 코어에 넘겼던 핸들. */
	struct iommufd_hw_pagetable *hwpt;	/* [한국어] 기대하는 도메인. */
	struct selftest_obj *sobj;	/* [한국어] 대상 셀프테스트 객체. */
	struct mock_dev *mdev;	/* [한국어] 그 모의 장치. */
	int rc = 0;	/* [한국어] 결과 코드. */

	sobj = iommufd_test_get_selftest_obj(ucmd->ictx, cmd->id);	/* [한국어] 떼기와 경합하지 않게 이쪽으로 찾는다. */
	if (IS_ERR(sobj))	/* [한국어] 없으면 */
		return PTR_ERR(sobj);	/* [한국어] 오류를 올린다. */

	mdev = sobj->idev.mock_dev;	/* [한국어] 모의 장치를 꺼낸다. */

	handle = iommu_attach_handle_get(mdev->dev.iommu_group,	/* [한국어] 그 PASID 의 핸들을 코어에서 찾는다. */
					 cmd->pasid_check.pasid, 0);
	if (IS_ERR(handle))	/* [한국어] 없으면 */
		attached_domain = NULL;	/* [한국어] 붙어 있지 않다는 뜻이다. */
	else
		attached_domain = handle->domain;	/* [한국어] 있으면 그 도메인을 꺼낸다. */

	/* hwpt_id == 0 means to check if pasid is detached */
	if (!hwpt_id) {	/* [한국어] 원 주석대로 0 은 "떨어져 있어야 한다"는 뜻이다. */
		if (attached_domain)	/* [한국어] 그런데 붙어 있으면 */
			rc = -EINVAL;	/* [한국어] 기대와 다르다. */
		goto out_sobj;	/* [한국어] 참조를 놓고 나간다. */
	}

	hwpt = iommufd_get_hwpt(ucmd, hwpt_id);	/* [한국어] 기대하는 도메인을 찾는다. */
	if (IS_ERR(hwpt)) {	/* [한국어] 없으면 */
		rc = PTR_ERR(hwpt);	/* [한국어] 오류를 꺼내 */
		goto out_sobj;	/* [한국어] 나간다. */
	}

	if (attached_domain != hwpt->domain)	/* [한국어] 실제와 기대가 다르면 */
		rc = -EINVAL;	/* [한국어] 어긋났다고 알린다. */

	iommufd_put_object(ucmd->ictx, &hwpt->obj);	/* [한국어] 도메인 참조를 놓는다. */
out_sobj:	/* [한국어] 모든 경로가 합류한다. */
	iommufd_put_object(ucmd->ictx, &sobj->obj);	/* [한국어] 객체 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_test_pasid_attach - PASID_ATTACH 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd: 그 인자.
 * @return: 0 성공, 음수면 실패.
 *
 * 응답에 실패하면 붙인 것을 되돌린다 — 사용자가 모르는 상태로 붙어
 * 있으면 안 된다.
 */
static int iommufd_test_pasid_attach(struct iommufd_ucmd *ucmd,
				     struct iommu_test_cmd *cmd)
{
	struct selftest_obj *sobj;	/* [한국어] 대상 객체. */
	int rc;	/* [한국어] 결과 코드. */

	sobj = iommufd_test_get_selftest_obj(ucmd->ictx, cmd->id);	/* [한국어] id 로 찾는다. */
	if (IS_ERR(sobj))	/* [한국어] 없으면 */
		return PTR_ERR(sobj);	/* [한국어] 오류를 올린다. */

	rc = iommufd_device_attach(sobj->idev.idev, cmd->pasid_attach.pasid,	/* [한국어] 정규 API 로 그 PASID 를 붙인다. */
				   &cmd->pasid_attach.pt_id);
	if (rc)	/* [한국어] 실패하면 */
		goto out_sobj;	/* [한국어] 참조를 놓고 나간다. */

	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 실제로 쓴 도메인 id 를 되돌려 쓴다. */
	if (rc)	/* [한국어] 전하지 못했으면 */
		iommufd_device_detach(sobj->idev.idev, cmd->pasid_attach.pasid);	/* [한국어] 붙인 것을 되돌린다 — 사용자가 모르는 상태로 남으면 안 된다. */

out_sobj:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	iommufd_put_object(ucmd->ictx, &sobj->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_test_pasid_replace - PASID_REPLACE 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd: 그 인자.
 * @return: 0 성공, 음수면 실패.
 *
 * 응답에 실패해도 되돌리지 않는 데 주의 — 교체는 옛 상태로 되돌릴 수
 * 없고, 이미 바뀐 것이 정상 상태다.
 */
static int iommufd_test_pasid_replace(struct iommufd_ucmd *ucmd,
				      struct iommu_test_cmd *cmd)
{
	struct selftest_obj *sobj;	/* [한국어] 대상 객체. */
	int rc;	/* [한국어] 결과 코드. */

	sobj = iommufd_test_get_selftest_obj(ucmd->ictx, cmd->id);	/* [한국어] id 로 찾는다. */
	if (IS_ERR(sobj))	/* [한국어] 없으면 */
		return PTR_ERR(sobj);	/* [한국어] 오류를 올린다. */

	rc = iommufd_device_replace(sobj->idev.idev, cmd->pasid_attach.pasid,	/* [한국어] 정규 API 로 교체한다. 인자 이름이 attach 인 것은 두 명령이 같은 union 멤버를 쓰기 때문이다. */
				    &cmd->pasid_attach.pt_id);
	if (rc)	/* [한국어] 실패하면 */
		goto out_sobj;	/* [한국어] 참조를 놓고 나간다. */

	rc = iommufd_ucmd_respond(ucmd, sizeof(*cmd));	/* [한국어] 되돌려 쓴다. 실패해도 되돌리지 않는 데 주의 — 교체는 옛 상태로 돌아갈 수 없다. */

out_sobj:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	iommufd_put_object(ucmd->ictx, &sobj->obj);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_test_pasid_detach - PASID_DETACH 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd: 그 인자.
 * @return: 늘 0.
 *
 * 떼기는 실패하지 않는다.
 */
static int iommufd_test_pasid_detach(struct iommufd_ucmd *ucmd,
				     struct iommu_test_cmd *cmd)
{
	struct selftest_obj *sobj;	/* [한국어] 대상 객체. */

	sobj = iommufd_test_get_selftest_obj(ucmd->ictx, cmd->id);	/* [한국어] id 로 찾는다. */
	if (IS_ERR(sobj))	/* [한국어] 없으면 */
		return PTR_ERR(sobj);	/* [한국어] 오류를 올린다. */

	iommufd_device_detach(sobj->idev.idev, cmd->pasid_detach.pasid);	/* [한국어] 그 PASID 를 뗀다. 떼기는 실패하지 않는다. */
	iommufd_put_object(ucmd->ictx, &sobj->obj);	/* [한국어] 참조를 놓는다. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iommufd_selftest_destroy - 셀프테스트 객체를 파괴한다
 *
 * @obj: 파괴할 객체.
 *
 * 사용자가 stdev_id 를 지울 때 불린다. 붙임과 묶음을 역순으로 풀고
 * 모의 장치를 없앤다.
 *
 * 이 순서가 정규 드라이버가 장치를 뗄 때 하는 일과 같아, 그 경로가
 * 함께 시험된다.
 */
void iommufd_selftest_destroy(struct iommufd_object *obj)
{
	struct selftest_obj *sobj = to_selftest_obj(obj);	/* [한국어] 셀프테스트 객체로 되짚는다. */

	switch (sobj->type) {	/* [한국어] 종류에 따라. 지금은 하나뿐이다. */
	case TYPE_IDEV:	/* [한국어] 모의 장치를 담은 객체. */
		iommufd_device_detach(sobj->idev.idev, IOMMU_NO_PASID);	/* [한국어] 붙임을 푼다. */
		iommufd_device_unbind(sobj->idev.idev);	/* [한국어] 묶음을 푼다. */
		mock_dev_destroy(sobj->idev.mock_dev);	/* [한국어] 장치를 없앤다. 만들 때의 역순이다. */
		break;	/* [한국어] 정리를 마쳤다. */
	}
}

/*
 * [한국어] 테스트용 dmabuf 의 내용.
 *
 * 진짜 장치 메모리 대신 평범한 커널 메모리를 쓴다. iommufd 쪽에서는
 * 물리 주소 하나로 보이므로 구별되지 않는다.
 *
 * revoked 로 "이 메모리는 더 이상 유효하지 않다"를 흉내 낸다.
 */
struct iommufd_test_dma_buf {
	/* [한국어] 흉내 낸 장치 메모리. 실제로는 평범한 커널 메모리다.
	 *  설정자: DMABUF_GET. 읽는 자: 물리 주소를 알려 줄 때. */
	void *memory;
	/* [한국어] 그 크기(페이지 단위로 올림된 값). */
	size_t length;
	/* [한국어] 무효로 표시됐는가.
	 *  설정자: DMABUF_REVOKE. 읽는 자: 물리 주소 조회가 이것을 보고 거절한다.
	 *  장치가 사라져 그 메모리가 더는 유효하지 않은 상황을 흉내 낸다. */
	bool revoked;
};

/*
 * [한국어]
 * iommufd_test_dma_buf_attach - dmabuf 붙임 콜백
 *
 * @dmabuf: 대상 dmabuf.
 * @attachment: 만들어진 붙임.
 * @return: 늘 0.
 *
 * 진짜 내보내는 쪽이라면 여기서 그 장치가 접근할 수 있는지 확인한다.
 * 테스트는 평범한 메모리라 아무나 접근할 수 있다.
 */
static int iommufd_test_dma_buf_attach(struct dma_buf *dmabuf,
				       struct dma_buf_attachment *attachment)
{
	return 0;	/* [한국어] 진짜 내보내는 쪽이라면 그 장치가 접근할 수 있는지 확인할 자리다. 평범한 메모리라 늘 허용한다. */
}

/*
 * [한국어]
 * iommufd_test_dma_buf_detach - dmabuf 붙임 해제 콜백
 *
 * @dmabuf: 대상 dmabuf.
 * @attachment: 풀리는 붙임.
 *
 * 붙일 때 한 일이 없으니 풀 것도 없다.
 */
static void iommufd_test_dma_buf_detach(struct dma_buf *dmabuf,
					struct dma_buf_attachment *attachment)
{
}

/*
 * [한국어]
 * iommufd_test_dma_buf_map - 평범한 DMA 매핑 콜백
 *
 * @attachment: 대상 붙임.
 * @dir: 전송 방향.
 * @return: 늘 -EOPNOTSUPP.
 *
 * 일부러 거절한다. iommufd 는 이 길이 아니라 물리 주소를 곧장 얻는
 * 전용 경로를 쓰기 때문이다. 그래도 이 콜백이 없으면 dmabuf 코어가
 * 등록을 받아 주지 않는다.
 */
static struct sg_table *
iommufd_test_dma_buf_map(struct dma_buf_attachment *attachment,
			 enum dma_data_direction dir)
{
	return ERR_PTR(-EOPNOTSUPP);	/* [한국어] 일부러 거절한다 — iommufd 는 물리 주소를 곧장 얻는 전용 경로를 쓴다. 그래도 이 콜백이 없으면 dmabuf 코어가 등록을 받지 않는다. */
}

/*
 * [한국어]
 * iommufd_test_dma_buf_unmap - DMA 매핑 해제 콜백
 *
 * @attachment: 대상 붙임.
 * @sgt: 해제할 산란-모음 표.
 * @dir: 전송 방향.
 *
 * 매핑이 늘 실패하므로 여기 올 일이 없다. 짝을 맞추려 비워 둔다.
 */
static void iommufd_test_dma_buf_unmap(struct dma_buf_attachment *attachment,
				       struct sg_table *sgt,
				       enum dma_data_direction dir)
{
}

/*
 * [한국어]
 * iommufd_test_dma_buf_release - dmabuf 의 마지막 참조가 사라졌을 때
 *
 * @dmabuf: 사라지는 dmabuf.
 *
 * 흉내 낸 장치 메모리를 해제한다.
 */
static void iommufd_test_dma_buf_release(struct dma_buf *dmabuf)
{
	struct iommufd_test_dma_buf *priv = dmabuf->priv;	/* [한국어] 만들 때 매달아 둔 내용. */

	kfree(priv->memory);	/* [한국어] 흉내 낸 장치 메모리를 놓는다. */
	kfree(priv);	/* [한국어] 내용 구조체도 해제한다. */
}

/*
 * [한국어] 테스트 dmabuf 의 연산표.
 *
 * 이 표의 주소가 "우리 dmabuf 인가"의 판별에도 쓰인다 — 사용자가
 * 아무 dmabuf fd 나 주어 남의 것을 우리 것으로 해석하게 만들 수 없다.
 */
static const struct dma_buf_ops iommufd_test_dmabuf_ops = {
	.attach = iommufd_test_dma_buf_attach,	/* [한국어] 붙임 콜백. 평범한 메모리라 늘 허용한다. */
	.detach = iommufd_test_dma_buf_detach,
	.map_dma_buf = iommufd_test_dma_buf_map,
	.release = iommufd_test_dma_buf_release,
	.unmap_dma_buf = iommufd_test_dma_buf_unmap,
};

/*
 * [한국어]
 * iommufd_test_dma_buf_iommufd_map - 물리 구간을 알려 준다
 *
 * @attachment: 대상 붙임.
 * @phys: 물리 주소와 길이를 여기에 쓴다.
 * @return: 0 성공, 우리 것이 아니면 -EOPNOTSUPP, 무효면 -ENODEV.
 *
 * pages.c 가 vfio 함수보다 먼저 이것을 부른다. 그래서 vfio 없이도
 * dmabuf 경로를 시험할 수 있다.
 *
 * 예약 락을 쥐고 있어야 한다 — 그 사이 무효가 되면 안 되기 때문이다.
 */
int iommufd_test_dma_buf_iommufd_map(struct dma_buf_attachment *attachment,
				     struct phys_vec *phys)
{
	struct iommufd_test_dma_buf *priv = attachment->dmabuf->priv;	/* [한국어] 매달아 둔 내용을 꺼낸다. */

	dma_resv_assert_held(attachment->dmabuf->resv);	/* [한국어] 예약 락을 쥐고 있어야 한다 — 그 사이 무효가 되면 안 된다. */

	if (attachment->dmabuf->ops != &iommufd_test_dmabuf_ops)	/* [한국어] 우리 dmabuf 가 아니면 */
		return -EOPNOTSUPP;	/* [한국어] pages.c 가 다음 후보(vfio)를 시도하게 한다. */

	if (priv->revoked)	/* [한국어] 무효로 표시됐으면 */
		return -ENODEV;	/* [한국어] 그 물리 주소는 더 이상 유효하지 않다. */

	phys->paddr = virt_to_phys(priv->memory);	/* [한국어] 커널 메모리의 물리 주소. 진짜 드라이버라면 장치 레지스터 창의 주소가 온다. */
	phys->len = priv->length;	/* [한국어] 그 길이. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * iommufd_test_dmabuf_get - DMABUF_GET 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @open_flags: 돌려줄 fd 에 줄 플래그.
 * @len: 만들 dmabuf 의 크기.
 * @return: 배정된 fd, 실패하면 음수.
 *
 * 평범한 커널 메모리를 dmabuf 로 감싸 사용자에게 fd 로 내준다.
 *
 * 크기 상한(2MB)은 테스트가 커널 메모리를 지나치게 잡지 못하게 막는다.
 *
 * dma_buf_fd 뒤에는 되돌릴 수 없다 — 그 순간 사용자가 이미 그 fd 를
 * 쓸 수 있고, 해제는 release 콜백이 맡는다.
 */
static int iommufd_test_dmabuf_get(struct iommufd_ucmd *ucmd,
				   unsigned int open_flags,
				   size_t len)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);	/* [한국어] dmabuf 를 만들 때 넘길 설명. 매크로가 지역 변수를 만들어 준다. */
	struct iommufd_test_dma_buf *priv;	/* [한국어] 매달 내용. */
	struct dma_buf *dmabuf;	/* [한국어] 만들 dmabuf. */
	int rc;	/* [한국어] 결과 코드. */

	len = ALIGN(len, PAGE_SIZE);	/* [한국어] 페이지 단위로 올린다 — 매핑은 페이지 단위이기 때문이다. */
	if (len == 0 || len > PAGE_SIZE * 512)	/* [한국어] 0 이거나 2MB 를 넘으면 */
		return -EINVAL;	/* [한국어] 거절한다. 테스트가 커널 메모리를 지나치게 잡지 못하게 막는다. */

	priv = kzalloc_obj(*priv);	/* [한국어] 내용 구조체. */
	if (!priv)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */

	priv->length = len;	/* [한국어] 흉내 낼 장치 메모리의 크기. */
	priv->memory = kzalloc(len, GFP_KERNEL);	/* [한국어] 실제로는 평범한 커널 메모리다. iommufd 쪽에서는 물리 주소 하나로 보여 구별되지 않는다. */
	if (!priv->memory) {	/* [한국어] 메모리가 없다. */
		rc = -ENOMEM;	/* [한국어] 실패. */
		goto err_free;	/* [한국어] 내용 구조체를 버린다. */
	}

	exp_info.ops = &iommufd_test_dmabuf_ops;	/* [한국어] 우리 연산표. 이 주소가 곧 "우리 것인가"의 판별에 쓰인다. */
	exp_info.size = len;	/* [한국어] 크기. */
	exp_info.flags = open_flags;	/* [한국어] 파일 디스크립터에 줄 플래그. */
	exp_info.priv = priv;	/* [한국어] 콜백에서 되짚을 내용. */

	dmabuf = dma_buf_export(&exp_info);	/* [한국어] dmabuf 를 만든다. */
	if (IS_ERR(dmabuf)) {	/* [한국어] 만들지 못했으면 */
		rc = PTR_ERR(dmabuf);	/* [한국어] 오류를 꺼내 */
		goto err_free;	/* [한국어] 정리한다. */
	}

	return dma_buf_fd(dmabuf, open_flags);	/* [한국어] fd 를 배정해 돌려준다. 이 뒤로는 되돌릴 수 없고, 해제는 release 콜백이 맡는다. */

err_free:	/* [한국어] 실패 경로. */
	kfree(priv->memory);	/* [한국어] 메모리와 */
	kfree(priv);	/* [한국어] 구조체를 해제한다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * iommufd_test_dmabuf_revoke - DMABUF_REVOKE 명령을 처리한다
 *
 * @ucmd: 처리 중인 명령.
 * @fd: 대상 dmabuf 의 fd.
 * @revoked: 무효로 만들 것인가.
 * @return: 0 성공, 우리 dmabuf 가 아니면 -EOPNOTSUPP.
 *
 * 장치가 사라져 그 메모리가 더는 유효하지 않게 된 상황을 흉내 낸다.
 *
 * dma_buf_invalidate_mappings 가 붙어 있는 쪽들에게 알림을 보낸다 —
 * iommufd 쪽에서는 pages.c 의 iopt_revoke_notify 가 그것을 받아 매핑을
 * 걷어 낸다. 이 명령의 진짜 목적이 그 경로를 시험하는 것이다.
 */
static int iommufd_test_dmabuf_revoke(struct iommufd_ucmd *ucmd, int fd,
				      bool revoked)
{
	struct iommufd_test_dma_buf *priv;	/* [한국어] 매달아 둔 내용. */
	struct dma_buf *dmabuf;	/* [한국어] 대상 dmabuf. */
	int rc = 0;	/* [한국어] 결과 코드. */

	dmabuf = dma_buf_get(fd);	/* [한국어] fd 로 찾으며 참조를 든다. */
	if (IS_ERR(dmabuf))	/* [한국어] 없는 fd 다. */
		return PTR_ERR(dmabuf);	/* [한국어] 오류를 올린다. */

	if (dmabuf->ops != &iommufd_test_dmabuf_ops) {	/* [한국어] 우리 dmabuf 가 아니면 */
		rc = -EOPNOTSUPP;	/* [한국어] 거절한다. 남의 dmabuf 를 우리 것으로 해석하면 안 된다. */
		goto err_put;	/* [한국어] 참조를 놓고 나간다. */
	}

	priv = dmabuf->priv;	/* [한국어] 내용을 꺼낸다. */
	dma_resv_lock(dmabuf->resv, NULL);	/* [한국어] 상태를 바꾸고 알림을 보내는 동안 예약 락을 쥔다. */
	priv->revoked = revoked;	/* [한국어] 무효 표시를 세우거나 지운다. */
	dma_buf_invalidate_mappings(dmabuf);	/* [한국어] 붙어 있는 쪽들에게 알린다 — iommufd 쪽에서는 iopt_revoke_notify 가 받아 매핑을 걷는다. 이 명령의 진짜 목적이다. */
	dma_resv_unlock(dmabuf->resv);	/* [한국어] 락 해제. */

err_put:	/* [한국어] 성공과 실패가 함께 지나는 지점. */
	dma_buf_put(dmabuf);	/* [한국어] 참조를 놓는다. */
	return rc;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * iommufd_test - 테스트 명령의 입구
 *
 * @ucmd: 처리 중인 명령.
 * @return: 각 처리기의 결과. 모르는 op 면 -EOPNOTSUPP.
 *
 * main.c 의 ioctl 표가 IOMMU_TEST_CMD 를 여기로 보낸다. op 값으로
 * 나눠 보내는 것이 전부다.
 *
 * MOCK_DOMAIN 과 MOCK_DOMAIN_FLAGS 가 같은 처리기로 가는 데 주의 —
 * 그 안에서 op 를 다시 보아 플래그를 읽을지 정한다.
 *
 * SET_TEMP_MEMORY_LIMIT 만 여기서 직접 처리한다. 원 주석대로 한 항목도
 * 담지 못할 크기를 막아야 묶음 초기화가 실패하지 않는다.
 */
int iommufd_test(struct iommufd_ucmd *ucmd)
{
	struct iommu_test_cmd *cmd = ucmd->cmd;	/* [한국어] 사용자 명령 버퍼. */

	switch (cmd->op) {	/* [한국어] op 값으로 나눠 보낸다. */
	case IOMMU_TEST_OP_ADD_RESERVED:	/* [한국어] 예약 구간 추가. */
		return iommufd_test_add_reserved(ucmd, cmd->id,	/* [한국어] @id 는 IOAS 의 id 다. */
						 cmd->add_reserved.start,
						 cmd->add_reserved.length);
	case IOMMU_TEST_OP_MOCK_DOMAIN:	/* [한국어] 모의 장치 생성. */
	case IOMMU_TEST_OP_MOCK_DOMAIN_FLAGS:	/* [한국어] 플래그를 함께 주는 판. 같은 처리기가 op 를 다시 보아 갈린다. */
		return iommufd_test_mock_domain(ucmd, cmd);	/* [한국어] 두 명령을 한 함수가 맡는다. */
	case IOMMU_TEST_OP_MOCK_DOMAIN_REPLACE:	/* [한국어] 붙은 도메인 교체. */
		return iommufd_test_mock_domain_replace(	/* [한국어] @id 는 모의 장치(stdev)의 id 다. */
			ucmd, cmd->id, cmd->mock_domain_replace.pt_id, cmd);
	case IOMMU_TEST_OP_MD_CHECK_MAP:	/* [한국어] 매핑이 그 사용자 메모리를 가리키는지 검사. */
		return iommufd_test_md_check_pa(	/* [한국어] 사용자 포인터를 커널 포인터로 바꿔 넘긴다. */
			ucmd, cmd->id, cmd->check_map.iova,
			cmd->check_map.length,
			u64_to_user_ptr(cmd->check_map.uptr));
	case IOMMU_TEST_OP_MD_CHECK_REFS:	/* [한국어] 페이지 참조 수 검사. */
		return iommufd_test_md_check_refs(	/* [한국어] 고정이 새는지 보는 검사다. */
			ucmd, u64_to_user_ptr(cmd->check_refs.uptr),
			cmd->check_refs.length, cmd->check_refs.refs);
	case IOMMU_TEST_OP_MD_CHECK_IOTLB:	/* [한국어] 중첩 도메인의 가짜 IOTLB 검사. */
		return iommufd_test_md_check_iotlb(ucmd, cmd->id,	/* [한국어] 무효화가 닿았는지 판정한다. */
						   cmd->check_iotlb.id,
						   cmd->check_iotlb.iotlb);
	case IOMMU_TEST_OP_DEV_CHECK_CACHE:	/* [한국어] 장치의 가짜 캐시 검사. */
		return iommufd_test_dev_check_cache(ucmd, cmd->id,	/* [한국어] vIOMMU 무효화가 장치까지 닿았는지 본다. */
						    cmd->check_dev_cache.id,
						    cmd->check_dev_cache.cache);
	case IOMMU_TEST_OP_CREATE_ACCESS:	/* [한국어] 커널 접근자 생성. */
		return iommufd_test_create_access(ucmd, cmd->id,	/* [한국어] @id 는 붙일 IOAS 의 id 다. */
						  cmd->create_access.flags);
	case IOMMU_TEST_OP_ACCESS_REPLACE_IOAS:	/* [한국어] 접근자가 볼 주소 공간 교체. */
		return iommufd_test_access_replace_ioas(	/* [한국어] @id 는 접근자의 fd 다. */
			ucmd, cmd->id, cmd->access_replace_ioas.ioas_id);
	case IOMMU_TEST_OP_ACCESS_PAGES:	/* [한국어] 접근자로 페이지 고정. */
		return iommufd_test_access_pages(	/* [한국어] 확인용 사용자 포인터를 함께 넘긴다. */
			ucmd, cmd->id, cmd->access_pages.iova,
			cmd->access_pages.length,
			u64_to_user_ptr(cmd->access_pages.uptr),
			cmd->access_pages.flags);
	case IOMMU_TEST_OP_ACCESS_RW:	/* [한국어] 접근자로 읽기·쓰기. */
		return iommufd_test_access_rw(	/* [한국어] 반대편 사용자 버퍼를 넘긴다. */
			ucmd, cmd->id, cmd->access_rw.iova,
			cmd->access_rw.length,
			u64_to_user_ptr(cmd->access_rw.uptr),
			cmd->access_rw.flags);
	case IOMMU_TEST_OP_DESTROY_ACCESS_PAGES:	/* [한국어] 고정 해제. */
		return iommufd_test_access_item_destroy(	/* [한국어] 고정할 때 받은 번호로 지목한다. */
			ucmd, cmd->id, cmd->destroy_access_pages.access_pages_id);
	case IOMMU_TEST_OP_SET_TEMP_MEMORY_LIMIT:	/* [한국어] 임시 버퍼 한도 변경. */
		/* Protect _batch_init(), can not be less than elmsz */
		if (cmd->memory_limit.limit <	/* [한국어] 원 주석대로 한 항목도 담지 못할 크기면 */
		    sizeof(unsigned long) + sizeof(u32))	/* [한국어] 묶음 초기화가 실패한다. */
			return -EINVAL;	/* [한국어] 그런 값은 거절한다. */
		iommufd_test_memory_limit = cmd->memory_limit.limit;	/* [한국어] 전역 값을 바꾼다. 아주 작게 잡아 여러 조각으로 나뉘는 경로를 타게 한다. */
		return 0;	/* [한국어] 성공. */
	case IOMMU_TEST_OP_DIRTY:	/* [한국어] 더티 비트를 손으로 세운다. */
		return iommufd_test_dirty(ucmd, cmd->id, cmd->dirty.iova,	/* [한국어] 비트맵으로 어느 페이지를 더럽힐지 고른다. */
					  cmd->dirty.length,
					  cmd->dirty.page_size,
					  u64_to_user_ptr(cmd->dirty.uptr),
					  cmd->dirty.flags);
	case IOMMU_TEST_OP_TRIGGER_IOPF:	/* [한국어] 폴트를 꾸며 낸다. */
		return iommufd_test_trigger_iopf(ucmd, cmd);	/* [한국어] eventq 의 폴트 경로를 시험한다. */
	case IOMMU_TEST_OP_TRIGGER_VEVENT:	/* [한국어] vIOMMU 이벤트를 꾸며 낸다. */
		return iommufd_test_trigger_vevent(ucmd, cmd);	/* [한국어] 넘침 처리까지 시험할 수 있다. */
	case IOMMU_TEST_OP_PASID_ATTACH:	/* [한국어] PASID 붙이기. */
		return iommufd_test_pasid_attach(ucmd, cmd);	/* [한국어] 장치의 PASID 를 도메인에 붙인다. */
	case IOMMU_TEST_OP_PASID_REPLACE:	/* [한국어] PASID 교체. */
		return iommufd_test_pasid_replace(ucmd, cmd);	/* [한국어] 무중단 교체 경로. */
	case IOMMU_TEST_OP_PASID_DETACH:	/* [한국어] PASID 떼기. */
		return iommufd_test_pasid_detach(ucmd, cmd);	/* [한국어] 뗀다. */
	case IOMMU_TEST_OP_PASID_CHECK_HWPT:	/* [한국어] PASID 가 어디에 붙어 있는지 검사. */
		return iommufd_test_pasid_check_hwpt(ucmd, cmd);	/* [한국어] 위 세 명령의 결과를 확인한다. */
	case IOMMU_TEST_OP_DMABUF_GET:	/* [한국어] 테스트 dmabuf 생성. */
		return iommufd_test_dmabuf_get(ucmd, cmd->dmabuf_get.open_flags,	/* [한국어] fd 를 돌려준다. */
					       cmd->dmabuf_get.length);
	case IOMMU_TEST_OP_DMABUF_REVOKE:	/* [한국어] 그 dmabuf 무효화. */
		return iommufd_test_dmabuf_revoke(ucmd,	/* [한국어] 매핑이 걷히는지 시험한다. */
						  cmd->dmabuf_revoke.dmabuf_fd,
						  cmd->dmabuf_revoke.revoked);
	default:	/* [한국어] 모르는 op. */
		return -EOPNOTSUPP;	/* [한국어] 지원하지 않는다. */
	}
}

/*
 * [한국어]
 * iommufd_should_fail - 지금 일부러 실패해야 하는가
 *
 * @return: 실패시켜야 하면 참.
 *
 * 여러 파일이 되감기 경로를 시험하려고 이것을 부른다. debugfs 로 확률과
 * 횟수를 조절해, 특정 지점에서만 실패하게 만들 수 있다.
 */
bool iommufd_should_fail(void)
{
	return should_fail(&fail_iommufd, 1);	/* [한국어] debugfs 로 설정한 확률과 횟수에 따라 참을 돌려준다. */
}

/*
 * [한국어]
 * iommufd_test_init - 가짜 버스와 IOMMU 를 등록한다
 *
 * @return: 0 성공, 음수면 실패.
 *
 * 모듈이 올라올 때 한 번 불린다. 플랫폼 장치를 만들고, 가짜 버스를
 * 등록하고, 그 버스에 모의 IOMMU 를 붙인다.
 *
 * 플랫폼 장치가 필요한 이유: iommu 코어는 IOMMU 자신도 어떤 장치여야
 * 한다고 본다. sysfs 항목이 그 장치 아래에 생긴다.
 *
 * iommu_device_register_bus 는 이 파일 전용에 가까운 함수다 — 보통은
 * 버스가 이미 있고 IOMMU 가 거기에 붙지만, 여기서는 버스까지 우리가
 * 만들었기 때문에 함께 등록해야 한다.
 *
 * 실패 경로가 등록의 역순으로 풀린다.
 */
int __init iommufd_test_init(void)
{
	struct platform_device_info pdevinfo = {	/* [한국어] 만들 플랫폼 장치의 설명. */
		.name = "iommufd_selftest_iommu",	/* [한국어] sysfs 에 보일 이름. */
	};
	int rc;	/* [한국어] 결과 코드. */

	dbgfs_root =	/* [한국어] 결함 주입 설정을 debugfs 에 노출한다. */
		fault_create_debugfs_attr("fail_iommufd", NULL, &fail_iommufd);	/* [한국어] 실패 확률과 횟수를 사용자가 조절할 수 있게 된다. */

	selftest_iommu_dev = platform_device_register_full(&pdevinfo);	/* [한국어] IOMMU 자신도 어떤 장치여야 하므로 하나 만든다. sysfs 항목이 이 아래에 생긴다. */
	if (IS_ERR(selftest_iommu_dev)) {	/* [한국어] 만들지 못했으면 */
		rc = PTR_ERR(selftest_iommu_dev);	/* [한국어] 오류를 꺼내 */
		goto err_dbgfs;	/* [한국어] debugfs 를 걷는다. */
	}

	rc = bus_register(&iommufd_mock_bus_type.bus);	/* [한국어] 가짜 버스를 등록한다. 모의 장치가 붙을 자리다. */
	if (rc)	/* [한국어] 플랫폼 장치를 만들지 못했다. */
		goto err_platform;	/* [한국어] 실패하면 플랫폼 장치를 걷는다. */

	rc = iommu_device_sysfs_add(&mock_iommu.iommu_dev,	/* [한국어] IOMMU 의 sysfs 항목을 만든다. */
				    &selftest_iommu_dev->dev, NULL, "%s",
				    dev_name(&selftest_iommu_dev->dev));
	if (rc)	/* [한국어] 버스를 등록하지 못했다. */
		goto err_bus;	/* [한국어] 실패하면 버스를 걷는다. */

	rc = iommu_device_register_bus(&mock_iommu.iommu_dev, &mock_ops,	/* [한국어] 버스까지 우리가 만들었으므로 함께 등록해야 한다. 보통 드라이버는 이미 있는 버스에 붙는다. */
				       &iommufd_mock_bus_type.bus,
				       &iommufd_mock_bus_type.nb);
	if (rc)	/* [한국어] sysfs 항목을 만들지 못했다. */
		goto err_sysfs;	/* [한국어] 실패하면 sysfs 를 걷는다. */

	refcount_set(&mock_iommu.users, 1);	/* [한국어] 기준이 되는 참조 하나. 내릴 때 이것을 놓는다. */
	init_completion(&mock_iommu.complete);	/* [한국어] 사용자가 없어질 때까지 기다릴 수단. */

	mock_iommu_iopf_queue = iopf_queue_alloc("mock-iopfq");	/* [한국어] 폴트 큐. 실패해도 계속 진행한다 — 폴트 시험만 못 하게 된다. */
	mock_iommu.iommu_dev.max_pasids = (1 << MOCK_PASID_WIDTH);	/* [한국어] 흉내 낼 PASID 개수. 코어가 이 값으로 PASID 능력을 판정한다. */

	return 0;	/* [한국어] 성공. */

err_sysfs:	/* [한국어] 등록 실패 경로들이 역순으로 이어진다. */
	iommu_device_sysfs_remove(&mock_iommu.iommu_dev);	/* [한국어] sysfs 항목을 걷는다. */
err_bus:	/* [한국어] 버스를 걷어야 하는 경로. */
	bus_unregister(&iommufd_mock_bus_type.bus);	/* [한국어] 버스 등록을 푼다. */
err_platform:	/* [한국어] 플랫폼 장치를 걷어야 하는 경로. */
	platform_device_unregister(selftest_iommu_dev);	/* [한국어] 장치를 없앤다. */
err_dbgfs:	/* [한국어] debugfs 를 걷어야 하는 경로. */
	debugfs_remove_recursive(dbgfs_root);	/* [한국어] 항목을 모두 지운다. */
	return rc;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * (아래 영어 주석과 함께 읽을 것)
 * iommufd_test_wait_for_users - 모의 IOMMU 를 쓰는 곳이 없어질 때까지 기다린다
 *
 * 원 주석이 밝히듯 이것은 본보기에 가깝다 — 셀프테스트가 iommufd 모듈
 * 안에 들어 있어, 모듈을 내릴 때만 IOMMU 가 빠진다. 그때는 이미 열린
 * fd 가 없으므로 이 기다림이 실제로 걸릴 일이 없다.
 *
 * 그래도 이렇게 적어 둔 것은, 진짜 드라이버가 hot-unplug 를 다룰 때
 * 무엇을 해야 하는지 보이려는 것이다.
 */
static void iommufd_test_wait_for_users(void)
{
	if (refcount_dec_and_test(&mock_iommu.users))	/* [한국어] 기준 참조를 놓아 0 이 되면 */
		return;	/* [한국어] 쓰는 곳이 없으니 기다릴 필요가 없다. */
	/*
	 * Time out waiting for iommu device user count to become 0.
	 *
	 * Note that this is just making an example here, since the selftest is
	 * built into the iommufd module, i.e. it only unplugs the iommu device
	 * when unloading the module. So, it is expected that this WARN_ON will
	 * not trigger, as long as any iommufd FDs are open.
	 */
	WARN_ON(!wait_for_completion_timeout(&mock_iommu.complete,	/* [한국어] 원 주석대로 실제로는 여기 걸릴 일이 없다. 진짜 드라이버가 hot-unplug 를 다룰 때 무엇을 해야 하는지 보이는 본보기다. */
					     msecs_to_jiffies(10000)));
}

/*
 * [한국어]
 * iommufd_test_exit - 등록한 것을 모두 걷어 낸다
 *
 * 모듈을 내릴 때 불린다. 등록의 역순으로 푼다.
 *
 * 폴트 큐를 가장 먼저 없애는 이유: 그것이 살아 있으면 새 폴트가 들어와
 * 이미 걷어 낸 자료 구조를 건드릴 수 있다.
 */
void iommufd_test_exit(void)
{
	if (mock_iommu_iopf_queue) {	/* [한국어] 폴트 큐가 있으면 */
		iopf_queue_free(mock_iommu_iopf_queue);	/* [한국어] 가장 먼저 없앤다 — 살아 있으면 새 폴트가 들어와 이미 걷어 낸 구조를 건드린다. */
		mock_iommu_iopf_queue = NULL;	/* [한국어] 두 번 놓지 않게 지운다. */
	}

	iommufd_test_wait_for_users();	/* [한국어] 아직 쓰는 곳이 없어질 때까지 기다린다. */
	iommu_device_sysfs_remove(&mock_iommu.iommu_dev);	/* [한국어] sysfs 항목을 걷는다. */
	iommu_device_unregister_bus(&mock_iommu.iommu_dev,	/* [한국어] IOMMU 등록을 푼다. */
				    &iommufd_mock_bus_type.bus,
				    &iommufd_mock_bus_type.nb);
	bus_unregister(&iommufd_mock_bus_type.bus);	/* [한국어] 가짜 버스를 걷는다. */
	platform_device_unregister(selftest_iommu_dev);	/* [한국어] 플랫폼 장치를 없앤다. */
	debugfs_remove_recursive(dbgfs_root);	/* [한국어] debugfs 항목을 지운다. 등록의 역순이다. */
}

MODULE_IMPORT_NS("GENERIC_PT_IOMMU");	/* [한국어] generic_pt 가 그 네임스페이스로 내보낸 심볼을 쓰겠다고 선언한다. 없으면 링크되지 않는다. */
