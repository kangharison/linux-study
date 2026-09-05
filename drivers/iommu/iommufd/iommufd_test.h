/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES.
 */
/*
 * [한국어 설명] 셀프테스트가 커널에게 시키는 명령들의 정의 (iommufd_test.h)
 *
 * === 파일의 역할 ===
 * iommufd 의 셀프테스트는 사용자 공간 프로그램(tools/testing/selftests)이
 * 돌린다. 그 프로그램이 커널 안의 모의(mock) 장치와 모의 IOMMU 를 조종하고
 * 결과를 들여다볼 수 있어야 하는데, 정규 ioctl 로는 그런 일을 할 수 없다.
 * 이 헤더가 그 전용 통로의 ABI 를 정의한다.
 *
 * 통로는 IOMMU_TEST_CMD 하나뿐이고, 그 안의 op 필드로 실제 명령을 고른다.
 * 명령마다 필요한 인자가 달라 union 으로 겹쳐 두었다 — 한 번에 하나만
 * 쓰이므로 구조체 크기를 가장 큰 명령에 맞추면 된다.
 *
 * 여기 적힌 매직 값들(0xdeadbeef 류)은 모두 "정규 규격의 어떤 값과도
 * 겹치지 않는 값"이라는 뜻이다. 모의 드라이버가 자기 형식을 정규 형식
 * 번호 공간에 끼워 넣어야 하기 때문에, 겹치지 않을 만한 값을 골라 쓴다.
 *
 * 이 파일은 커널과 테스트 프로그램이 함께 읽는다. 그래서 __u32 같은
 * 사용자 공간용 타입만 쓰고, 커널 전용 타입은 쓰지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 테스트 프로그램 → ioctl(IOMMU_TEST_CMD) → main.c 의 ioctl 표
 *   → selftest.c 의 iommufd_test() → op 별 처리 → 모의 IOMMU/장치
 *
 * 실행 컨텍스트: 모두 프로세스 문맥. CONFIG_IOMMUFD_TEST 가 켜져야
 * 이 통로가 존재한다.
 *
 * === 타 모듈과의 연결 ===
 * 짝이 되는 구현은 selftest.c 다. 이 헤더의 op 하나하나가 그 파일의
 * 함수 하나에 대응한다.
 * tools/testing/selftests/iommu/ 의 테스트가 같은 헤더를 포함해 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * struct iommu_test_cmd: 모든 테스트 명령이 지나는 하나의 구조체.
 *   size/op/id 가 공통 머리말이고 뒤의 union 이 명령별 인자다.
 * MOCK_APERTURE_* / MOCK_PAGE_SIZE: 모의 IOMMU 가 흉내 내는 주소 공간과
 *   페이지 크기. 테스트가 이 값을 알고 매핑을 짠다.
 * struct iommu_hwpt_selftest 등: 모의 드라이버가 정규 ABI 의 "드라이버
 *   고유 데이터" 자리에 끼워 넣는 형식들.
 */
#ifndef _UAPI_IOMMUFD_TEST_H	/* [한국어] 중복 포함 방지 가드. UAPI 접두어는 이 헤더가 사용자 공간과 함께 읽히는 ABI 정의임을 뜻한다. */
#define _UAPI_IOMMUFD_TEST_H	/* [한국어] 가드를 세운다. 두 번째 포함부터는 아래 내용이 통째로 건너뛰어진다. */

#include <linux/iommufd.h>	/* [한국어] IOMMUFD_TYPE 과 IOMMUFD_CMD_BASE 를 쓰려고 포함한다 — 테스트 ioctl 번호를 정규 번호 공간 뒤에 붙이기 위해서다. */
#include <linux/types.h>	/* [한국어] __u32, __aligned_u64 같은 ABI 전용 타입의 정의. */

/* [한국어] 테스트 명령의 종류.
 * IOMMU_TEST_CMD 의 op 필드에 들어가는 값이며, selftest.c 의
 * iommufd_test() 가 이 값으로 switch 한다.
 * 1 부터 시작하는 이유: 0 은 "지정하지 않음"과 구별되지 않는다.
 * 새 항목은 반드시 끝에 붙인다 — 값이 곧 ABI 라 중간에 끼우면
 * 이미 빌드된 테스트 프로그램이 다른 명령을 부르게 된다. */
enum {
	/* [한국어] IOAS 에 예약 구간을 손으로 추가한다.
	 *  설정자: 테스트 프로그램. 읽는 자: selftest.c 가 iopt_reserve_iova 로 옮긴다.
	 *  인자: add_reserved.start / length.
	 *  쓰임: 예약 구간을 피해 IOVA 가 배정되는지 확인할 때. 실제로는 MSI
	 *  창이나 장치의 예약 구간이 이 자리를 차지하지만, 테스트에서는 원하는
	 *  구간을 임의로 만들어야 한다. */
	IOMMU_TEST_OP_ADD_RESERVED = 1,
	/* [한국어] 모의 장치를 하나 만들어 IOAS 에 붙인다.
	 *  출력: mock_domain.out_stdev_id / out_hwpt_id / out_idev_id.
	 *  쓰임: 대부분의 테스트가 여기서 시작한다. 진짜 하드웨어 없이도 장치가
	 *  붙은 상태를 만들어 낸다. */
	IOMMU_TEST_OP_MOCK_DOMAIN,
	/* [한국어] 모의 도메인의 매핑이 기대한 사용자 메모리를 가리키는지 검사한다.
	 *  인자: check_map.iova / length / uptr.
	 *  쓰임: 매핑 경로가 실제로 올바른 물리 페이지를 꽂았는지 확인한다.
	 *  커널이 직접 비교해 주므로 테스트가 물리 주소를 알 필요가 없다. */
	IOMMU_TEST_OP_MD_CHECK_MAP,
	/* [한국어] 사용자 페이지의 참조 수가 기대값인지 검사한다.
	 *  인자: check_refs.uptr / length / refs.
	 *  쓰임: 페이지 고정(pin)이 새거나 이중으로 잡히지 않는지 확인한다.
	 *  iopt_pages 의 참조 계산이 이 테스트의 주 대상이다. */
	IOMMU_TEST_OP_MD_CHECK_REFS,
	/* [한국어] 커널 내부 접근자(access) 객체를 만든다.
	 *  출력: create_access.out_access_fd.
	 *  쓰임: vfio 의 에뮬레이션 장치처럼 CPU 로 게스트 메모리를 읽고 쓰는
	 *  경로를 흉내 낸다. 그 경로가 IOVA 로 페이지를 찾아오는지 시험한다. */
	IOMMU_TEST_OP_CREATE_ACCESS,
	/* [한국어] 접근자가 고정해 둔 페이지 묶음을 놓는다.
	 *  인자: destroy_access_pages.access_pages_id.
	 *  쓰임: ACCESS_PAGES 로 잡아 둔 것을 짝 맞춰 놓는다. 놓지 않고 객체를
	 *  파괴했을 때의 동작도 이 명령의 유무로 시험할 수 있다. */
	IOMMU_TEST_OP_DESTROY_ACCESS_PAGES,
	/* [한국어] 접근자로 IOVA 구간의 페이지를 고정해 가져온다.
	 *  인자: access_pages.iova / length / uptr / flags. 출력: out_access_pages_id.
	 *  쓰임: 고정된 동안 그 구간을 풀려는 시도가 어떻게 되는지 등 어려운
	 *  경합 상황을 만들어 낸다. */
	IOMMU_TEST_OP_ACCESS_PAGES,
	/* [한국어] 접근자로 IOVA 구간을 CPU 로 읽거나 쓴다.
	 *  인자: access_rw.iova / length / uptr / flags.
	 *  쓰임: 고정 없이 그때그때 페이지를 찾아 복사하는 경로의 시험.
	 *  SLOW_PATH 플래그로 느린 경로를 강제할 수 있다. */
	IOMMU_TEST_OP_ACCESS_RW,
	/* [한국어] 임시 버퍼에 쓸 메모리 한도를 바꾼다.
	 *  인자: memory_limit.limit.
	 *  쓰임: 한도를 아주 낮춰 대량 매핑이 여러 조각으로 나뉘어 처리되는
	 *  경로를 억지로 타게 한다. 그 경로는 큰 매핑에서만 나타나 평소에는
	 *  시험하기 어렵다. */
	IOMMU_TEST_OP_SET_TEMP_MEMORY_LIMIT,
	/* [한국어] 모의 장치가 붙은 페이지 테이블을 다른 것으로 바꾼다.
	 *  인자: mock_domain_replace.pt_id. @id 는 stdev_id.
	 *  쓰임: 무중단 교체(replace) 경로의 시험. 게스트가 도메인을 바꿔도
	 *  DMA 가 끊기지 않아야 한다. */
	IOMMU_TEST_OP_MOCK_DOMAIN_REPLACE,
	/* [한국어] 접근자가 보는 IOAS 를 다른 것으로 바꾼다.
	 *  인자: access_replace_ioas.ioas_id.
	 *  쓰임: 접근자 쪽에도 같은 교체 의미가 성립하는지 확인한다. */
	IOMMU_TEST_OP_ACCESS_REPLACE_IOAS,
	/* [한국어] MOCK_DOMAIN 과 같되 장치 능력을 플래그로 지정한다.
	 *  인자: mock_domain_flags.dev_flags.
	 *  쓰임: 더티 추적을 못 하는 장치, PASID 를 지원하는 장치 등 여러 성격의
	 *  장치를 만들어 각 경로를 시험한다. */
	IOMMU_TEST_OP_MOCK_DOMAIN_FLAGS,
	/* [한국어] 모의 IOMMU 의 더티 비트를 손으로 세운다.
	 *  인자: dirty.iova / length / page_size / uptr. 출력: out_nr_dirty.
	 *  쓰임: 하드웨어가 페이지를 더럽혔다고 꾸며 낸 뒤, 더티 비트맵 읽기가
	 *  그것을 제대로 모아 오는지 확인한다. */
	IOMMU_TEST_OP_DIRTY,
	/* [한국어] 중첩 도메인의 모의 IOTLB 값이 기대값인지 검사한다.
	 *  인자: check_iotlb.id / iotlb.
	 *  쓰임: 무효화 명령이 실제로 그 IOTLB 항목을 건드렸는지 확인한다.
	 *  모의 IOTLB 는 그저 숫자 배열이지만, 무효화 경로의 도달 여부를
	 *  보이게 해 준다. */
	IOMMU_TEST_OP_MD_CHECK_IOTLB,
	/* [한국어] 모의 장치가 페이지 폴트를 낸 것처럼 꾸민다.
	 *  인자: trigger_iopf.dev_id / pasid / grpid / perm / addr.
	 *  쓰임: eventq.c 의 폴트 큐 경로를 시험한다. 진짜 폴트를 재현하기
	 *  어려우므로 이렇게 만들어 넣는다. */
	IOMMU_TEST_OP_TRIGGER_IOPF,
	/* [한국어] 모의 장치의 캐시 값이 기대값인지 검사한다.
	 *  인자: check_dev_cache.id / cache.
	 *  쓰임: vIOMMU 단위 무효화가 장치별 캐시까지 닿았는지 확인한다. */
	IOMMU_TEST_OP_DEV_CHECK_CACHE,
	/* [한국어] 모의 vIOMMU 가 이벤트를 낸 것처럼 꾸민다.
	 *  인자: trigger_vevent.dev_id.
	 *  쓰임: eventq.c 의 vIOMMU 이벤트 큐 경로와 넘침 처리(잃어버림 표시)를
	 *  시험한다. 깊이를 넘겨 여러 번 부르면 그 표시가 나타난다. */
	IOMMU_TEST_OP_TRIGGER_VEVENT,
	/* [한국어] 모의 장치의 특정 PASID 를 페이지 테이블에 붙인다.
	 *  인자: pasid_attach.pasid / pt_id. @id 는 stdev_id.
	 *  쓰임: 한 장치가 PASID 마다 다른 주소 공간을 갖는 구조의 시험. */
	IOMMU_TEST_OP_PASID_ATTACH,
	/* [한국어] 특정 PASID 가 붙은 페이지 테이블을 바꾼다.
	 *  인자: pasid_replace.pasid / pt_id.
	 *  쓰임: PASID 단위 무중단 교체 경로의 시험. */
	IOMMU_TEST_OP_PASID_REPLACE,
	/* [한국어] 특정 PASID 를 뗀다.
	 *  인자: pasid_detach.pasid.
	 *  쓰임: 뗀 뒤 그 PASID 의 DMA 가 막히는지, 남은 폴트가 정리되는지 본다. */
	IOMMU_TEST_OP_PASID_DETACH,
	/* [한국어] 특정 PASID 가 기대한 페이지 테이블에 붙어 있는지 검사한다.
	 *  인자: pasid_check.pasid / hwpt_id.
	 *  쓰임: 위 세 명령의 결과를 커널 쪽에서 확인한다. */
	IOMMU_TEST_OP_PASID_CHECK_HWPT,
	/* [한국어] 테스트용 dmabuf 를 하나 만들어 fd 를 돌려준다.
	 *  인자: dmabuf_get.length / open_flags.
	 *  쓰임: 장치 메모리를 dmabuf 로 받아 매핑하는 경로의 시험. */
	IOMMU_TEST_OP_DMABUF_GET,
	/* [한국어] 그 dmabuf 를 무효로 만들거나 되살린다.
	 *  인자: dmabuf_revoke.dmabuf_fd / revoked.
	 *  쓰임: 장치가 사라져 dmabuf 가 더는 유효하지 않게 됐을 때, 그것을
	 *  매핑해 두었던 IOAS 가 어떻게 반응하는지 시험한다. */
	IOMMU_TEST_OP_DMABUF_REVOKE,
};

/* [한국어] 모의 IOMMU 가 쓸 페이지 테이블 형식.
 * 큰 페이지를 지원하는지가 유일한 차이다. 아래 MOCK_APERTURE_* 값들은
 * DEFAULT 를 전제로 한다. */
enum {
	/* [한국어] 작은 페이지만 쓰는 기본 형식.
	 *  설정자: 테스트가 hwpt 를 만들 때 iommu_hwpt_selftest.pagetable_type 에 넣는다.
	 *  값 0 이므로, 지정하지 않으면 자동으로 이것이 된다. */
	MOCK_IOMMUPT_DEFAULT = 0,
	/* [한국어] 큰 페이지를 지원하는 형식.
	 *  쓰임: 매핑이 실제로 큰 페이지로 합쳐지는지, 그 한가운데를 풀 때
	 *  어떻게 되는지 시험한다. */
	MOCK_IOMMUPT_HUGE,
};

/* These values are true for MOCK_IOMMUPT_DEFAULT */
/* [한국어] 모의 IOMMU 가 흉내 내는 주소 공간과 페이지 크기.
 * 테스트 프로그램이 이 값을 알아야 유효한 IOVA 를 골라 매핑을 짤 수 있다.
 * 위 영어 주석대로 MOCK_IOMMUPT_DEFAULT 형식에서만 맞는 값이다. */
enum {
	/* [한국어] 모의 IOMMU 가 다룰 수 있는 가장 낮은 IOVA (16MB).
	 *  0 이 아닌 값을 고른 이유: 진짜 IOMMU 도 낮은 주소를 못 쓰는 경우가 많고,
	 *  테스트가 0 을 유효한 주소로 착각하지 않게 하려는 것이다.
	 *  읽는 자: selftest.c 가 도메인의 geometry 를 이 값으로 채운다. */
	MOCK_APERTURE_START = 1UL << 24,
	/* [한국어] 다룰 수 있는 가장 높은 IOVA (2GB - 1).
	 *  이 범위를 벗어난 매핑 요청은 실패해야 한다 — 그 경계 검사도 시험 대상이다. */
	MOCK_APERTURE_LAST = (1UL << 31) - 1,
	/* [한국어] 모의 IOMMU 의 페이지 크기 (2048).
	 *  호스트 페이지(보통 4096)보다 작게 잡은 것이 요점이다. IOMMU 페이지가
	 *  CPU 페이지보다 작은 실제 하드웨어가 있고, 그 경우의 계산이 까다로워
	 *  테스트가 일부러 그 상황을 만든다. */
	MOCK_PAGE_SIZE = 2048,
	/* [한국어] 큰 페이지 크기 (512 * 2048 = 1MB).
	 *  MOCK_IOMMUPT_HUGE 형식에서만 쓰인다.
	 *  읽는 자: 매핑이 이 크기로 합쳐졌는지 확인하는 테스트. */
	MOCK_HUGE_PAGE_SIZE = 512 * MOCK_PAGE_SIZE,
};

/* [한국어] 접근자(access)를 만들거나 쓸 때 주는 플래그. */
enum {
	/* [한국어] 이 접근이 쓰기임을 알린다.
	 *  설정자: 테스트 프로그램이 access_pages.flags 또는 access_rw.flags 에 넣는다.
	 *  읽는 자: selftest.c 가 IOMMU_ACCESS_RW_WRITE 로 옮긴다.
	 *  읽기 전용 매핑에 쓰기를 시도했을 때 거절되는지 시험할 때 쓴다. */
	MOCK_FLAGS_ACCESS_WRITE = 1 << 0,
	/* [한국어] syzkaller(퍼저)가 쓰는 특별 모드.
	 *  비트 자리를 16 으로 멀리 띄운 이유: 위쪽 비트들은 정규 플래그가
	 *  차지하지 않으므로, 퍼저 전용 표시를 섞어도 부딪히지 않는다.
	 *  이 비트가 서면 selftest.c 가 인자를 유효한 범위로 강제로 접어 넣는다 —
	 *  퍼저가 던지는 아무 값에도 커널이 죽지 않게 하되, 경로 자체는 타게
	 *  하려는 것이다. */
	MOCK_FLAGS_ACCESS_SYZ = 1 << 16,
};

/* [한국어] ACCESS_RW 명령의 플래그. */
enum {
	/* [한국어] 읽기가 아니라 쓰기를 하라는 표시.
	 *  설정자: 테스트. 읽는 자: selftest.c 의 iommufd_test_access_rw().
	 *  값 범위: 서면 사용자 버퍼에서 IOVA 로, 지워지면 그 반대 방향. */
	MOCK_ACCESS_RW_WRITE = 1 << 0,
	/* [한국어] 빠른 경로를 건너뛰고 느린 경로를 강제한다.
	 *  빠른 경로는 페이지가 이미 고정돼 있을 때 곧장 복사하는 길이고,
	 *  느린 경로는 그때그때 찾아오는 길이다. 평소에는 빠른 쪽만 타서
	 *  느린 쪽이 시험되지 않으므로, 이 비트로 억지로 타게 한다.
	 *  비트 2 를 쓰는 이유: 비트 1 자리는 정규 코드의 다른 뜻과 맞추려고
	 *  비워 두었다. */
	MOCK_ACCESS_RW_SLOW_PATH = 1 << 2,
};

/* [한국어] 접근자를 만들 때 주는 플래그. */
enum {
	/* [한국어] 이 접근자가 페이지 고정을 쓸 것임을 미리 알린다.
	 *  설정자: 테스트가 create_access.flags 에 넣는다.
	 *  읽는 자: selftest.c 가 접근자 ops 에 unmap 콜백을 달지 결정한다.
	 *  고정을 쓰는 접근자는 매핑이 풀릴 때 알림을 받아야 하고, 쓰지 않는
	 *  접근자는 그럴 필요가 없다 — 그 두 성격을 갈라 시험한다. */
	MOCK_FLAGS_ACCESS_CREATE_NEEDS_PIN_PAGES = 1 << 0,
};

/* [한국어] 모의 장치를 만들 때 그 장치의 성격을 지정하는 플래그.
 * mock_domain_flags.dev_flags 에 들어간다. */
enum {
	/* [한국어] 더티 추적을 지원하지 않는 장치로 만든다.
	 *  쓰임: 그런 장치가 붙은 도메인에 더티 추적을 켜려 할 때 거절되는지,
	 *  이미 켜진 도메인에 그런 장치를 붙일 수 있는지를 시험한다. */
	MOCK_FLAGS_DEVICE_NO_DIRTY = 1 << 0,
	/* [한국어] PASID 를 지원하는 장치로 만든다.
	 *  비트 2 를 쓰는 이유: 비트 1 은 과거에 쓰이다 없어진 자리로, 옛
	 *  테스트 프로그램과 값이 어긋나지 않게 다시 쓰지 않는다.
	 *  쓰임: PASID_ATTACH 계열 명령을 받을 수 있는 장치를 만든다. */
	MOCK_FLAGS_DEVICE_PASID = 1 << 2,
};

/* [한국어] 모의 중첩 도메인이 들고 있는 가짜 IOTLB 배열의 크기.
 * 무효화 명령이 어느 항목까지 닿았는지 세어 보려고 여러 개를 둔다. */
enum {
	/* [한국어] 쓸 수 있는 가장 큰 IOTLB 첨자 (3).
	 *  읽는 자: selftest.c 의 무효화 처리기가 사용자가 준 iotlb_id 를 이 값과
	 *  견줘 범위를 벗어난 요청을 거절한다 — 그 거절도 시험 대상이다. */
	MOCK_NESTED_DOMAIN_IOTLB_ID_MAX = 3,
	/* [한국어] IOTLB 배열의 길이 (4).
	 *  위 최대 첨자보다 하나 큰 값. 배열 선언과 전체 순회에 쓴다. */
	MOCK_NESTED_DOMAIN_IOTLB_NUM = 4,
};

/* [한국어] 모의 장치가 들고 있는 가짜 캐시 배열의 크기.
 * vIOMMU 단위 무효화가 장치별 캐시까지 닿는지 보려고 둔다. */
enum {
	/* [한국어] 쓸 수 있는 가장 큰 캐시 첨자 (3).
	 *  범위를 벗어난 cache_id 는 거절되어야 한다. */
	MOCK_DEV_CACHE_ID_MAX = 3,
	/* [한국어] 캐시 배열의 길이 (4).
	 *  전체 무효화(FLAG_ALL)일 때 이 개수만큼 모두 지운다. */
	MOCK_DEV_CACHE_NUM = 4,
};

/* Reserved for special pasid replace test */
/* [한국어] 교체 시험 전용으로 비워 둔 PASID 값.
 * 위 영어 주석대로 특별한 교체 테스트가 이 번호를 쓴다. 다른 테스트가
 * 같은 번호를 쓰면 서로 간섭하므로 한 곳에 정의해 예약해 둔다. */
#define IOMMU_TEST_PASID_RESERVED 1024

/* [한국어] 모든 테스트 명령이 지나는 하나의 구조체.
 *
 * 앞의 네 필드가 공통 머리말이고, 뒤의 union 이 명령마다 다른 인자다.
 * 명령을 늘려도 ioctl 번호는 하나로 유지되므로 ABI 가 어지러워지지 않는다.
 *
 * union 을 쓰는 이유: 한 번에 한 명령만 쓰이므로 겹쳐 두면 구조체가
 * 가장 큰 명령의 크기로 끝난다. 각 멤버에 이름을 붙여 두어 코드에서
 * cmd->access_pages.iova 처럼 읽을 수 있다. */
struct iommu_test_cmd {
	/* [한국어] 사용자가 넘긴 구조체의 크기.
	 *  설정자: 테스트 프로그램. 읽는 자: main.c 의 ioctl 공통 처리.
	 *  커널이 아는 크기보다 작으면 뒷부분을 0 으로 채우고, 크면 남는 부분이
	 *  0 인지 확인한다 — 그래야 구조체를 뒤로 늘려도 옛 프로그램이 돈다. */
	__u32 size;
	/* [한국어] 실제로 수행할 테스트 명령. 위 IOMMU_TEST_OP_* 중 하나.
	 *  설정자: 테스트 프로그램. 읽는 자: selftest.c 의 iommufd_test() 가
	 *  이 값으로 switch 한다.
	 *  모르는 값이면 -EOPNOTSUPP. */
	__u32 op;
	/* [한국어] 명령이 대상으로 삼는 객체의 id.
	 *  무엇의 id 인지는 명령마다 다르다 — IOAS id 이기도 하고, 아래 몇몇
	 *  명령의 주석이 밝히듯 모의 장치(stdev)의 id 이기도 하다.
	 *  설정자: 테스트 프로그램. 읽는 자: 각 op 처리 함수. */
	__u32 id;
	/* [한국어] 나중을 위해 비워 둔 자리.
	 *  값 범위: 반드시 0. 커널이 0 이 아니면 거절한다.
	 *  거절하는 이유: 지금 아무 뜻이 없는 값을 조용히 무시하면, 나중에
	 *  의미를 붙였을 때 옛 프로그램이 뜻하지 않은 동작을 하게 된다.
	 *  또한 64비트 정렬을 맞추는 채움 역할도 한다. */
	__u32 __reserved;
	/* [한국어] 명령별 인자. 위 op 값이 어느 멤버를 쓸지 정한다. */
	union {
		/* [한국어] ADD_RESERVED 의 인자. */
		struct {
			/* [한국어] 예약할 구간의 시작 IOVA.
			 *  __aligned_u64 를 쓰는 이유: 32비트 사용자 프로그램에서도 8바이트
			 *  정렬이 되어야 커널 구조체와 배치가 어긋나지 않는다. */
			__aligned_u64 start;
			/* [한국어] 예약할 구간의 길이(바이트).
			 *  읽는 자: selftest.c 가 iopt_reserve_iova(start, start+length-1) 로 옮긴다. */
			__aligned_u64 length;
		} add_reserved;
		/* [한국어] MOCK_DOMAIN 의 출력. */
		struct {
			/* [한국어] 만들어진 모의 장치(selftest device)의 객체 id.
			 *  이후 REPLACE, PASID_* 명령의 @id 자리에 이 값을 넣는다.
			 *  이 객체를 지우면 모의 장치가 사라진다. */
			__u32 out_stdev_id;
			/* [한국어] 그 장치에 자동으로 붙은 페이지 테이블의 id.
			 *  IOAS 에 처음 붙는 장치라면 커널이 도메인을 하나 만들어 준다. */
			__u32 out_hwpt_id;
			/* out_idev_id is the standard iommufd_bind object */
			/* [한국어] 그 장치의 iommufd 장치 객체 id.
			 *  위 영어 주석대로 이것은 정규 IOMMU_DEVICE_BIND 가 돌려주는 것과
			 *  같은 종류의 객체라, 정규 명령에 그대로 쓸 수 있다. */
			__u32 out_idev_id;
		} mock_domain;
		/* [한국어] MOCK_DOMAIN_FLAGS 의 입출력. 위 mock_domain 을 늘린 것이다. */
		struct {
			/* [한국어] 만들어진 모의 장치의 id. mock_domain 과 같다. */
			__u32 out_stdev_id;
			/* [한국어] 자동으로 붙은 페이지 테이블의 id. */
			__u32 out_hwpt_id;
			/* [한국어] 그 장치의 iommufd 장치 객체 id. */
			__u32 out_idev_id;
			/* Expand mock_domain to set mock device flags */
			/* [한국어] 만들 장치의 성격. MOCK_FLAGS_DEVICE_* 조합.
			 *  설정자: 테스트 프로그램(입력이다 — 이 구조체에서 유일하게).
			 *  위 영어 주석대로 이 필드 하나를 더하려고 mock_domain 을 늘렸다. */
			__u32 dev_flags;
		} mock_domain_flags;
		/* [한국어] MOCK_DOMAIN_REPLACE 의 인자. @id 는 stdev_id 다. */
		struct {
			/* [한국어] 새로 붙일 페이지 테이블 또는 IOAS 의 id.
			 *  IOAS 를 주면 그에 딸린 도메인으로, HWPT 를 주면 그것으로 바꾼다.
			 *  교체는 무중단이어야 한다 — 옛 것을 떼고 새 것을 붙이는 것이 아니라
			 *  한 번에 바꿔치기해야 그 사이 DMA 가 막히지 않는다. */
			__u32 pt_id;
		} mock_domain_replace;
		/* [한국어] MD_CHECK_MAP 의 인자. @id 는 검사할 도메인(hwpt)의 id. */
		struct {
			/* [한국어] 검사를 시작할 IOVA. */
			__aligned_u64 iova;
			/* [한국어] 검사할 길이(바이트). */
			__aligned_u64 length;
			/* [한국어] 이 IOVA 가 가리켜야 할 사용자 메모리의 주소.
			 *  커널이 도메인을 따라 내려가 얻은 물리 페이지와, 이 사용자 주소가
			 *  가리키는 물리 페이지가 같은지 비교한다.
			 *  테스트가 물리 주소를 알 방법이 없어 커널에 비교를 시키는 것이다. */
			__aligned_u64 uptr;
		} check_map;
		/* [한국어] MD_CHECK_REFS 의 인자. */
		struct {
			/* [한국어] 검사할 사용자 메모리의 길이. */
			__aligned_u64 length;
			/* [한국어] 검사할 사용자 메모리의 시작 주소. */
			__aligned_u64 uptr;
			/* [한국어] 그 페이지들이 가져야 할 참조 수.
			 *  고정(pin)이 새면 이 값보다 크고, 이중으로 놓았으면 작다.
			 *  읽는 자: selftest.c 가 페이지마다 page_count 를 견줘 본다. */
			__u32 refs;
		} check_refs;
		/* [한국어] CREATE_ACCESS 의 입출력. */
		struct {
			/* [한국어] 만들어진 접근자의 파일 디스크립터.
			 *  테스트는 이 fd 를 닫아 접근자를 없앨 수 있다 — 객체 파괴 시점을
			 *  조종해 경합 상황을 만드는 데 쓴다. */
			__u32 out_access_fd;
			/* [한국어] 접근자의 성격. MOCK_FLAGS_ACCESS_CREATE_* 조합.
			 *  설정자: 테스트 프로그램(입력). */
			__u32 flags;
		} create_access;
		/* [한국어] DESTROY_ACCESS_PAGES 의 인자. @id 는 접근자의 id. */
		struct {
			/* [한국어] 놓을 고정 묶음의 id. ACCESS_PAGES 가 돌려준 값.
			 *  이 id 를 통해 어느 고정을 놓을지 지목한다 — 한 접근자가 여러 구간을
			 *  동시에 고정할 수 있기 때문이다. */
			__u32 access_pages_id;
		} destroy_access_pages;
		/* [한국어] ACCESS_PAGES 의 입출력. @id 는 접근자의 id. */
		struct {
			/* [한국어] 접근 성격. MOCK_FLAGS_ACCESS_WRITE 등.
			 *  쓰기로 고정하려면 매핑에도 쓰기 권한이 있어야 한다. */
			__u32 flags;
			/* [한국어] 만들어진 고정 묶음의 id.
			 *  DESTROY_ACCESS_PAGES 에 이 값을 넣어 놓는다. */
			__u32 out_access_pages_id;
			/* [한국어] 고정할 구간의 시작 IOVA. */
			__aligned_u64 iova;
			/* [한국어] 고정할 길이. */
			__aligned_u64 length;
			/* [한국어] 그 구간이 가리켜야 할 사용자 메모리의 주소.
			 *  고정해 온 페이지가 정말 그 메모리인지 커널이 함께 확인한다. */
			__aligned_u64 uptr;
		} access_pages;
		/* [한국어] ACCESS_RW 의 인자. @id 는 접근자의 id. */
		struct {
			/* [한국어] 읽거나 쓸 구간의 시작 IOVA. */
			__aligned_u64 iova;
			/* [한국어] 그 길이. */
			__aligned_u64 length;
			/* [한국어] 반대편 버퍼가 되는 사용자 메모리 주소.
			 *  읽기면 여기로 담고, 쓰기면 여기서 가져간다. */
			__aligned_u64 uptr;
			/* [한국어] MOCK_ACCESS_RW_* 조합. 방향과 경로 선택을 담는다. */
			__u32 flags;
		} access_rw;
		/* [한국어] SET_TEMP_MEMORY_LIMIT 의 인자. */
		struct {
			/* [한국어] 임시 버퍼에 쓸 수 있는 메모리 한도(페이지 수).
			 *  작게 잡으면 큰 매핑이 여러 조각으로 나뉘어 처리된다.
			 *  읽는 자: pages.c 의 배치 처리 경로.
			 *  동기화: 전역 값이라 테스트가 직렬로 돌아야 한다. */
			__u32 limit;
		} memory_limit;
		/* [한국어] ACCESS_REPLACE_IOAS 의 인자. @id 는 접근자의 id. */
		struct {
			/* [한국어] 접근자가 앞으로 볼 IOAS 의 id.
			 *  바꾸는 순간 고정해 둔 페이지들이 어떻게 되는지가 시험의 초점이다. */
			__u32 ioas_id;
		} access_replace_ioas;
		/* [한국어] DIRTY 의 입출력. @id 는 도메인(hwpt)의 id. */
		struct {
			/* [한국어] 더티 표시 방식을 고르는 플래그.
			 *  읽는 자: selftest.c 의 더티 비트 조작 함수. */
			__u32 flags;
			/* [한국어] 더럽혔다고 표시할 구간의 시작 IOVA. */
			__aligned_u64 iova;
			/* [한국어] 그 길이. */
			__aligned_u64 length;
			/* [한국어] 표시 단위가 되는 페이지 크기.
			 *  이 크기로 구간을 나눠 각 페이지의 더티 비트를 다룬다.
			 *  큰 페이지 도메인인지 아닌지에 따라 다른 값을 준다. */
			__aligned_u64 page_size;
			/* [한국어] 어느 페이지를 더럽힐지 고르는 비트맵의 사용자 주소.
			 *  비트 n 이 서 있으면 n 번째 페이지를 더럽힌 것으로 꾸민다. */
			__aligned_u64 uptr;
			/* [한국어] 실제로 더티로 표시한 페이지 수.
			 *  테스트는 나중에 정규 경로로 읽어 온 개수와 이 값을 견준다. */
			__aligned_u64 out_nr_dirty;
		} dirty;
		/* [한국어] MD_CHECK_IOTLB 의 인자. @id 는 중첩 도메인의 id. */
		struct {
			/* [한국어] 검사할 IOTLB 항목의 첨자.
			 *  값 범위: 0 .. MOCK_NESTED_DOMAIN_IOTLB_ID_MAX. */
			__u32 id;
			/* [한국어] 그 항목이 가져야 할 값.
			 *  무효화가 닿았으면 기본값에서 바뀌어 있어야 한다. */
			__u32 iotlb;
		} check_iotlb;
		/* [한국어] TRIGGER_IOPF 의 인자. 폴트를 꾸며 낸다. */
		struct {
			/* [한국어] 폴트를 낸 것으로 할 장치의 id. */
			__u32 dev_id;
			/* [한국어] 어느 주소 공간에서 난 폴트인지.
			 *  PASID 를 지원하지 않는 장치면 0 을 준다. */
			__u32 pasid;
			/* [한국어] 하드웨어가 붙였을 요청 묶음 번호.
			 *  응답이 이 값으로 짝을 찾으므로, 테스트가 응답까지 시험하려면
			 *  읽어 온 값과 같은지 확인해야 한다. */
			__u32 grpid;
			/* [한국어] 장치가 요구한 권한(읽기/쓰기).
			 *  사용자가 그에 맞는 매핑을 만들어 응답하는 흐름을 시험한다. */
			__u32 perm;
			/* [한국어] 폴트가 난 것으로 할 가상 주소.
			 *  __aligned_u64 가 아닌 이유: 앞의 __u32 네 개가 이미 8의 배수라
			 *  정렬이 저절로 맞는다. */
			__u64 addr;
		} trigger_iopf;
		/* [한국어] DEV_CHECK_CACHE 의 인자. @id 는 모의 장치의 id. */
		struct {
			/* [한국어] 검사할 캐시 항목의 첨자.
			 *  값 범위: 0 .. MOCK_DEV_CACHE_ID_MAX. */
			__u32 id;
			/* [한국어] 그 항목이 가져야 할 값. */
			__u32 cache;
		} check_dev_cache;
		/* [한국어] TRIGGER_VEVENT 의 인자. */
		struct {
			/* [한국어] 이벤트를 낸 것으로 할 장치의 id.
			 *  이 값이 이벤트 내용으로 사용자 공간까지 전해지므로, 테스트는 읽어 온
			 *  이벤트에서 같은 값이 나오는지 확인한다. */
			__u32 dev_id;
		} trigger_vevent;
		/* [한국어] PASID_ATTACH 의 인자. */
		struct {
			/* [한국어] 붙일 PASID 번호.
			 *  값 범위: 0 .. (1<<MOCK_PASID_WIDTH)-1. */
			__u32 pasid;
			/* [한국어] 그 PASID 를 붙일 페이지 테이블 또는 IOAS 의 id. */
			__u32 pt_id;
			/* @id is stdev_id */
		} pasid_attach;
		/* [한국어] PASID_REPLACE 의 인자. */
		struct {
			/* [한국어] 교체할 PASID 번호. */
			__u32 pasid;
			/* [한국어] 새로 붙일 페이지 테이블의 id.
			 *  무중단 교체라 DMA 가 끊기지 않아야 한다. */
			__u32 pt_id;
			/* @id is stdev_id */
		} pasid_replace;
		/* [한국어] PASID_DETACH 의 인자. */
		struct {
			/* [한국어] 뗄 PASID 번호. */
			__u32 pasid;
			/* @id is stdev_id */
		} pasid_detach;
		/* [한국어] PASID_CHECK_HWPT 의 인자. */
		struct {
			/* [한국어] 검사할 PASID 번호. */
			__u32 pasid;
			/* [한국어] 그 PASID 가 붙어 있어야 할 페이지 테이블의 id.
			 *  읽는 자: selftest.c 가 실제로 붙은 도메인과 견준다. */
			__u32 hwpt_id;
			/* @id is stdev_id */
		} pasid_check;
		/* [한국어] DMABUF_GET 의 인자. */
		struct {
			/* [한국어] 만들 dmabuf 의 크기(바이트). */
			__u32 length;
			/* [한국어] 돌려줄 파일 디스크립터에 줄 플래그(O_CLOEXEC 등).
			 *  테스트가 fd 의 성격까지 조종할 수 있게 열어 둔 자리다. */
			__u32 open_flags;
		} dmabuf_get;
		/* [한국어] DMABUF_REVOKE 의 인자. */
		struct {
			/* [한국어] 대상 dmabuf 의 파일 디스크립터.
			 *  부호 있는 타입인 이유: 파일 디스크립터는 -1 이 "없음"을 뜻하는
			 *  부호 있는 값이다. */
			__s32 dmabuf_fd;
			/* [한국어] 1 이면 무효로 만들고, 0 이면 되살린다.
			 *  무효가 된 dmabuf 를 매핑해 두었던 IOAS 가 그 사실을 알아채고
			 *  매핑을 걷어 내는지가 시험의 초점이다. */
			__u32 revoked;
		} dmabuf_revoke;	/* [한국어] union 의 마지막 멤버. 여기까지가 명령별 인자다. */
	};
	/* [한국어] 구조체의 끝을 표시하는 자리.
	 *  읽는 자: 셀프테스트가 offsetofend(...,last) 로 전체 크기를 구한다.
	 *  union 뒤에 두어야 어떤 명령을 쓰든 같은 크기가 나온다. */
	__u32 last;
};
/* [한국어] 테스트 전용 ioctl 번호.
 * 정규 명령들의 뒤쪽(+32)에 멀리 떨어뜨려 두었다 — 정규 명령이 늘어나도
 * 번호가 부딪히지 않게 하려는 것이다.
 * CONFIG_IOMMUFD_TEST 가 꺼져 있으면 main.c 의 표에 이 항목이 없어
 * -ENOTTY 가 된다. */
#define IOMMU_TEST_CMD _IO(IOMMUFD_TYPE, IOMMUFD_CMD_BASE + 32)

/* Mock device/iommu PASID width */
/* [한국어] 모의 장치와 모의 IOMMU 가 지원하는 PASID 폭(비트).
 * 위 영어 주석이 밝히듯 장치와 IOMMU 양쪽 모두의 폭이다.
 * 20 은 PCIe 규격의 PASID 폭과 같은 값이라, 실제 하드웨어의 한계를
 * 그대로 흉내 낸다. 이 폭을 넘는 PASID 요청은 거절되어야 한다. */
#define MOCK_PASID_WIDTH 20

/* Mock structs for IOMMU_DEVICE_GET_HW_INFO ioctl */
/* [한국어] 모의 드라이버가 자기 하드웨어 정보 형식임을 알리는 번호.
 * 사용자가 IOMMU_DEVICE_GET_HW_INFO 로 물으면 이 값이 돌아온다.
 * 정규 드라이버들이 쓰는 번호와 겹치지 않게 눈에 띄는 값을 골랐다. */
#define IOMMU_HW_INFO_TYPE_SELFTEST	0xfeedbeef
/* [한국어] 모의 하드웨어 레지스터가 늘 돌려주는 값.
 * 테스트는 읽어 온 값이 이것과 같은지 보아, 정보 전달 경로가 온전한지
 * 확인한다. 값 자체에는 아무 뜻이 없다. */
#define IOMMU_HW_INFO_SELFTEST_REGVAL	0xdeadbeef

/* [한국어] 모의 드라이버의 하드웨어 정보.
 * 정규 IOMMU_DEVICE_GET_HW_INFO 가 돌려주는 "드라이버 고유 데이터"
 * 자리에 그대로 들어간다. 진짜 드라이버라면 여기에 그 IOMMU 의 능력
 * 레지스터 값이 담긴다. */
struct iommu_test_hw_info {
	/* [한국어] 능력 플래그. 모의 드라이버는 늘 0 을 넣는다.
	 *  자리만 잡아 둔 필드로, 정규 드라이버의 구조와 모양을 맞춘 것이다.
	 *  읽는 자: 테스트가 0 인지 확인한다. */
	__u32 flags;
	/* [한국어] 모의 레지스터 값. 늘 IOMMU_HW_INFO_SELFTEST_REGVAL.
	 *  설정자: selftest.c 의 hw_info 콜백.
	 *  읽는 자: 테스트 프로그램이 기대값과 견준다. */
	__u32 test_reg;
};

/* Should not be equal to any defined value in enum iommu_hwpt_data_type */
/* [한국어] 모의 드라이버의 중첩 도메인 데이터 형식 번호.
 * 위 영어 주석대로 정규 enum iommu_hwpt_data_type 의 어떤 값과도 겹치지
 * 않아야 한다 — 겹치면 사용자가 준 데이터를 다른 드라이버 형식으로
 * 해석하게 된다. */
#define IOMMU_HWPT_DATA_SELFTEST 0xdead
/* [한국어] 모의 IOTLB 항목의 초기값.
 * 무효화가 닿으면 이 값에서 바뀐다. 테스트는 바뀌었는지를 보고 무효화
 * 경로의 도달을 판정한다. */
#define IOMMU_TEST_IOTLB_DEFAULT 0xbadbeef
/* [한국어] 모의 장치 캐시 항목의 초기값.
 * 위와 같은 방식으로, vIOMMU 단위 무효화가 장치까지 닿았는지 본다. */
#define IOMMU_TEST_DEV_CACHE_DEFAULT 0xbaddad

/**
 * struct iommu_hwpt_selftest
 *
 * @iotlb: default mock iotlb value, IOMMU_TEST_IOTLB_DEFAULT
 */
/* [한국어]
 * (위 영어 주석에 이어)
 * 모의 드라이버의 중첩 도메인 생성 데이터.
 * 진짜 드라이버라면 여기에 게스트가 만든 페이지 테이블의 주소와 형식이
 * 담긴다. 모의 드라이버는 그 자리에 시험에 필요한 값만 넣는다. */
struct iommu_hwpt_selftest {
	/* [한국어] 이 중첩 도메인의 모의 IOTLB 초기값.
	 *  위 영어 주석대로 보통 IOMMU_TEST_IOTLB_DEFAULT 를 넣는다.
	 *  설정자: 테스트 프로그램. 읽는 자: selftest.c 가 IOTLB 배열 네 칸을
	 *  모두 이 값으로 채운다. */
	__u32 iotlb;
	/* [한국어] 이 도메인이 쓸 페이지 테이블 형식. MOCK_IOMMUPT_* 중 하나.
	 *  설정자: 테스트 프로그램. 0 이면 작은 페이지만 쓰는 기본 형식.
	 *  큰 페이지 형식을 고르면 매핑이 합쳐지는 경로를 시험할 수 있다. */
	__u32 pagetable_type;
};

/* Should not be equal to any defined value in enum iommu_hwpt_invalidate_data_type */
/* [한국어] 모의 드라이버의 무효화 데이터 형식 번호.
 * 사용자가 IOMMU_HWPT_INVALIDATE 에 이 번호와 함께 아래 구조체를 준다. */
#define IOMMU_HWPT_INVALIDATE_DATA_SELFTEST 0xdeadbeef
/* [한국어] 일부러 틀린 형식 번호.
 * 커널이 모르는 형식을 제대로 거절하는지 시험하려고 준비해 둔 값이다.
 * 위 값과 한 글자만 다르게 지은 것이 눈에 띈다 — 사람이 보기에 헷갈리게
 * 만들어야 실수로 통과하는 경로가 드러난다. */
#define IOMMU_HWPT_INVALIDATE_DATA_SELFTEST_INVALID 0xdadbeef

/**
 * struct iommu_hwpt_invalidate_selftest - Invalidation data for Mock driver
 *                                         (IOMMU_HWPT_INVALIDATE_DATA_SELFTEST)
 * @flags: Invalidate flags
 * @iotlb_id: Invalidate iotlb entry index
 *
 * If IOMMU_TEST_INVALIDATE_ALL is set in @flags, @iotlb_id will be ignored
 */
/* [한국어]
 * (위 영어 주석에 이어)
 * 모의 드라이버의 무효화 명령 하나. 진짜 드라이버라면 여기에 무효화할
 * 주소 범위나 태그가 담긴다. */
struct iommu_hwpt_invalidate_selftest {
	/* [한국어] 전체 무효화를 뜻하는 플래그.
	 *  구조체 안에 #define 을 둔 것은 커널 UAPI 의 흔한 관례다 — 그 플래그가
	 *  어느 필드에 속하는지 눈으로 바로 알 수 있다.
	 *  이 비트가 서면 아래 iotlb_id 는 무시된다(위 영어 주석). */
#define IOMMU_TEST_INVALIDATE_FLAG_ALL	(1 << 0)
	/* [한국어] 무효화 플래그. 지금은 ALL 하나뿐이다.
	 *  설정자: 테스트 프로그램. 읽는 자: selftest.c 의 무효화 처리기.
	 *  모르는 비트가 켜져 있으면 거절해야 한다 — 그것도 시험 대상이다. */
	__u32 flags;
	/* [한국어] 무효화할 IOTLB 항목의 첨자.
	 *  값 범위: 0 .. MOCK_NESTED_DOMAIN_IOTLB_ID_MAX. 벗어나면 거절.
	 *  ALL 플래그가 서 있으면 이 값은 쓰이지 않는다. */
	__u32 iotlb_id;
};

/* [한국어] 모의 vIOMMU 의 종류 번호.
 * IOMMU_VIOMMU_ALLOC 의 type 자리에 이 값을 주면 모의 vIOMMU 가 만들어진다.
 * 정규 드라이버의 종류 번호와 겹치지 않아야 한다. */
#define IOMMU_VIOMMU_TYPE_SELFTEST 0xdeadbeef

/**
 * struct iommu_viommu_selftest - vIOMMU data for Mock driver
 *                                (IOMMU_VIOMMU_TYPE_SELFTEST)
 * @in_data: Input random data from user space
 * @out_data: Output data (matching @in_data) to user space
 * @out_mmap_offset: The offset argument for mmap syscall
 * @out_mmap_length: The length argument for mmap syscall
 *
 * Simply set @out_data=@in_data for a loopback test
 */
/* [한국어]
 * (위 영어 주석에 이어)
 * 모의 vIOMMU 를 만들 때 주고받는 데이터.
 * 진짜 드라이버라면 게스트가 보는 IOMMU 레지스터 창의 위치 같은 것이
 * 담긴다. 모의 쪽은 되울림(loopback) 시험과 mmap 창 시험만 한다. */
struct iommu_viommu_selftest {
	/* [한국어] 사용자가 넣어 보내는 아무 값.
	 *  설정자: 테스트 프로그램이 무작위 값을 넣는다.
	 *  읽는 자: selftest.c 가 그대로 out_data 에 옮긴다.
	 *  쓰임: 드라이버 고유 데이터가 커널까지 온전히 전해지는지 확인한다. */
	__u32 in_data;
	/* [한국어] 커널이 돌려주는 값. 위 in_data 와 같아야 한다.
	 *  위 영어 주석대로 되울림 시험이므로 값 자체에 뜻은 없다.
	 *  다르면 데이터 전달 경로 어딘가가 잘못된 것이다. */
	__u32 out_data;
	/* [한국어] 게스트가 볼 레지스터 창을 mmap 할 때 쓸 오프셋.
	 *  설정자: 커널. 읽는 자: 테스트가 iommufd fd 에 mmap 할 때 넘긴다.
	 *  진짜 드라이버에서는 이것이 실제 하드웨어 레지스터 페이지로 이어져,
	 *  게스트가 IOMMU 를 직접 두드릴 수 있게 한다. */
	__aligned_u64 out_mmap_offset;
	/* [한국어] 그 창의 길이(바이트).
	 *  설정자: 커널. mmap 에 이 길이를 그대로 넘긴다.
	 *  커널이 정해 주는 이유: 창의 크기는 하드웨어가 정하는 것이지
	 *  사용자가 고를 수 있는 값이 아니다. */
	__aligned_u64 out_mmap_length;
};

/* Should not be equal to any defined value in enum iommu_viommu_invalidate_data_type */
/* [한국어] 모의 vIOMMU 의 무효화 데이터 형식 번호.
 * 도메인 단위가 아니라 vIOMMU 단위 무효화에 쓰인다. */
#define IOMMU_VIOMMU_INVALIDATE_DATA_SELFTEST 0xdeadbeef
/* [한국어] 일부러 틀린 형식 번호.
 * 거절 경로를 시험하려고 둔다. */
#define IOMMU_VIOMMU_INVALIDATE_DATA_SELFTEST_INVALID 0xdadbeef

/**
 * struct iommu_viommu_invalidate_selftest - Invalidation data for Mock VIOMMU
 *                                        (IOMMU_VIOMMU_INVALIDATE_DATA_SELFTEST)
 * @flags: Invalidate flags
 * @cache_id: Invalidate cache entry index
 *
 * If IOMMU_TEST_INVALIDATE_ALL is set in @flags, @cache_id will be ignored
 */
/* [한국어]
 * (위 영어 주석에 이어)
 * 모의 vIOMMU 의 무효화 명령 하나.
 * 도메인 단위와 달리 어느 가상 장치의 어느 캐시인지까지 지목한다 —
 * vIOMMU 는 여러 장치를 거느리기 때문이다. */
struct iommu_viommu_invalidate_selftest {
	/* [한국어] 전체 무효화 플래그.
	 *  위 hwpt 판과 같은 이름·같은 값이지만 다른 구조체에 속한다.
	 *  두 번 정의해도 값이 같아 문제가 없고, 각 구조체를 따로 읽어도
	 *  뜻이 통하게 하려는 의도다. */
#define IOMMU_TEST_INVALIDATE_FLAG_ALL (1 << 0)
	/* [한국어] 무효화 플래그. ALL 이 서면 아래 cache_id 는 무시된다.
	 *  설정자: 테스트 프로그램. 읽는 자: selftest.c 의 vIOMMU 무효화 처리기. */
	__u32 flags;
	/* [한국어] 무효화할 가상 장치의 id.
	 *  게스트가 보는 번호이지 호스트의 장치 id 가 아니다 — vIOMMU 가 그
	 *  번호를 실제 장치로 옮겨 준다.
	 *  없는 번호를 주면 거절되어야 한다. */
	__u32 vdev_id;
	/* [한국어] 그 장치의 어느 캐시 항목을 무효화할지.
	 *  값 범위: 0 .. MOCK_DEV_CACHE_ID_MAX. 벗어나면 거절. */
	__u32 cache_id;
};

/* [한국어] 모의 vIOMMU 이벤트 큐의 종류 번호.
 * IOMMU_VEVENTQ_ALLOC 의 type 자리에 준다. 한 vIOMMU 에 종류마다 큐가
 * 하나뿐이라, 같은 값을 두 번 주면 -EEXIST 가 되어야 한다. */
#define IOMMU_VEVENTQ_TYPE_SELFTEST 0xbeefbeef

/* [한국어] 모의 vIOMMU 가 올리는 이벤트의 내용.
 * eventq.c 가 머리말 뒤에 이 구조체를 이어 붙여 사용자에게 전한다.
 * 진짜 드라이버라면 여기에 하드웨어가 올린 사건 기록이 담긴다. */
struct iommu_viommu_event_selftest {
	/* [한국어] 사건을 낸 가상 장치의 id.
	 *  설정자: TRIGGER_VEVENT 명령이 지정한 장치에서 selftest.c 가 꺼낸다.
	 *  읽는 자: 테스트 프로그램이 읽어 온 이벤트에서 이 값을 확인한다.
	 *  게스트가 아는 번호여야 뜻이 있다 — 호스트 id 를 전하면 게스트가
	 *  어느 장치인지 알 수 없다. */
	__u32 virt_id;
};

/* [한국어] 모의 하드웨어 큐의 종류 번호.
 * 게스트가 직접 명령을 넣는 큐를 흉내 낸다. viommu.c 의 hw_queue
 * 객체가 이 번호로 만들어진다. */
#define IOMMU_HW_QUEUE_TYPE_SELFTEST 0xdeadbeef
/* [한국어] 모의 vIOMMU 가 거느릴 수 있는 하드웨어 큐의 최대 개수.
 * 진짜 하드웨어도 큐 개수가 정해져 있고, 그 한계를 넘겨 만들려 할 때
 * 거절되는지 시험하려고 작은 값(2)으로 잡았다. */
#define IOMMU_TEST_HW_QUEUE_MAX 2

#endif	/* [한국어] _UAPI_IOMMUFD_TEST_H 가드의 끝. */
