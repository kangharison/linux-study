/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */
/*
 * [한국어 설명] iommufd 의 객체 모델과 내부 선언 (iommufd_private.h)
 *
 * === 파일의 역할 ===
 * iommufd 는 사용자 공간이 IOMMU 를 직접 다루게 해 주는 인터페이스다.
 * VFIO 의 컨테이너를 대체하며, 파일 디스크립터 하나 아래에 여러 종류의
 * 객체를 두고 id 로 주고받는다.
 *
 * 이 헤더가 그 객체들의 정의를 모아 놓았다. 종류는 이렇다.
 *  - IOAS: 사용자가 관리하는 IOVA 공간. io_pagetable 이 실체다.
 *  - HWPT(hw_pagetable): 실제 iommu_domain 을 감싼 것. 페이징용과 중첩용이
 *    갈리며, 페이징 HWPT 는 IOAS 하나에 매여 그 매핑을 복사받는다.
 *  - device: 드라이버가 iommufd 에 묶어 둔 장치.
 *  - access: 커널 안의 소비자(VFIO 등)가 IOAS 의 페이지를 직접 보는 통로.
 *  - viommu / vdevice: 게스트에게 IOMMU 자체를 보여 주는 중첩 구성.
 *  - fault / veventq: 페이지 폴트와 하드웨어 이벤트를 사용자 공간에 전하는 큐.
 *
 * 객체 수명이 이 파일의 어려운 부분이다. 참조가 두 겹인데, users 는 보통의
 * 참조 계수이고 wait_cnt 는 "파괴를 기다릴 수 있게" 하는 별도 계수다.
 * 사용자가 객체를 지우라고 했을 때, 다른 스레드가 그 객체를 쓰는 중이면
 * 기다렸다가 지워야 하기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간 ioctl → main.c 의 명령 표 → 각 객체의 ioctl 구현
 *   → io_pagetable.c / pages.c → iommu 코어 → 드라이버 → 하드웨어
 * VFIO 는 vfio_compat.c 를 통해 옛 ioctl 로도 들어온다.
 *
 * 실행 컨텍스트: 대부분 프로세스 문맥. 이벤트 큐 쪽만 드라이버의 인터럽트
 * 스레드에서 채워진다.
 *
 * === 타 모듈과의 연결 ===
 * 위: drivers/vfio 가 iommufd 를 백엔드로 쓰고, VMM 이 직접 ioctl 을 낸다.
 * 아래: iommu 코어의 도메인·장치 API, iommu-priv.h 의 내부 API.
 *
 * === 주요 함수/구조체 요약 ===
 * struct iommufd_ctx: 열린 파일 디스크립터 하나. 모든 객체가 여기 매달린다.
 * struct io_pagetable: IOVA → PFN 대응. 여러 도메인에 같은 매핑을 복사해
 *   넣고, 도메인들 사이에서 PFN 을 공유한다. 락 순서가 원 주석에 있다.
 * iommufd_lock_obj / iommufd_put_object: 두 겹 참조를 함께 다루는 짝.
 *   순서가 중요해 원 주석이 그 이유를 밝힌다.
 * iommufd_object_destroy_user / tombstone_user: 사용자 요청에 의한 파괴.
 *   후자는 id 를 비석으로 남겨 재사용을 막는다.
 * iommufd_object_alloc 계열 매크로: 객체를 만들며 obj 필드가 구조체 맨
 *   앞에 있는지 컴파일 시 확인한다.
 * iommufd_vevent_handler: 이벤트를 큐에 넣으며 잃어버림 표시를 재배치한다.
 */
#ifndef __IOMMUFD_PRIVATE_H	/* [한국어] 중복 포함 방지 */
#define __IOMMUFD_PRIVATE_H	/* [한국어] 같은 이름으로 표시 */

#include <linux/iommu.h>	/* [한국어] 도메인과 장치 API */
#include <linux/iommufd.h>	/* [한국어] 드라이버에 공개된 부분 */
#include <linux/iova_bitmap.h>	/* [한국어] 더티 비트를 사용자 비트맵으로 옮긴다 */
#include <linux/maple_tree.h>	/* [한국어] mmap 오프셋 할당 */
#include <linux/rwsem.h>	/* [한국어] IOVA 공간의 읽기·쓰기 락 */
#include <linux/uaccess.h>	/* [한국어] 사용자 버퍼 복사 */
#include <linux/xarray.h>	/* [한국어] id → 객체 대응 */
#include <uapi/linux/iommufd.h>	/* [한국어] 사용자와 주고받는 구조체 */

#include "../iommu-priv.h"	/* [한국어] iommu 코어의 내부 API */

struct iommu_domain;	/* [한국어] 포인터로만 쓰므로 전방 선언 */
struct iommu_group;	/* [한국어] 같은 이유 */
struct iommu_option;	/* [한국어] 같은 이유 */
struct iommufd_device;	/* [한국어] 같은 이유 */
struct dma_buf_attachment;	/* [한국어] 같은 이유 */

/*
 * [한국어] MSI 도어벨 하나에 배정된 IOVA.
 * 같은 도어벨을 쓰는 모든 도메인이 같은 IOVA 를 공유하도록, 파일
 * 디스크립터 단위로 이 배정을 모아 둔다(driver.c 참고).
 */
struct iommufd_sw_msi_map {
	struct list_head sw_msi_item;
	/* [한국어] 파일 디스크립터 전역 목록의 연결 고리.
	 * 설정자: 새 배정을 만들 때.
	 * 읽는 자: 같은 도어벨을 찾는 순회.
	 * 값 범위: ictx->sw_msi_list 에 매달린다.
	 * 동기화: ictx->sw_msi_lock. */
	phys_addr_t sw_msi_start;
	/* [한국어] 이 배정이 속한 IOVA 창의 시작.
	 * 설정자: 만들 때, 장치 그룹의 예약 구간에서.
	 * 읽는 자: 실제 IOVA 를 계산할 때.
	 * 값 범위: 장치가 요구한 MSI 예약 창.
	 * 동기화: 불변이다. */
	phys_addr_t msi_addr;
	/* [한국어] MSI 도어벨의 물리 주소(페이지 정렬).
	 * 설정자: 만들 때.
	 * 읽는 자: 같은 도어벨을 찾을 때, 그리고 매핑할 때.
	 * 값 범위: 장치의 MMIO 주소.
	 * 동기화: 불변이다. */
	unsigned int pgoff;
	/* [한국어] 그 창 안에서의 페이지 번호.
	 * 설정자: 만들 때, 그 창에서 아직 쓰지 않은 번호로.
	 * 읽는 자: 실제 IOVA 를 계산할 때.
	 * 값 범위: 0 부터 창 크기까지.
	 * 동기화: 불변이다. */
	unsigned int id;
	/* [한국어] 비트맵에서의 자리.
	 * 설정자: 만들 때 순번으로.
	 * 읽는 자: 도메인별·그룹별 비트맵 검사.
	 * 값 범위: 0 부터 비트맵 크기-1. 넘으면 새 배정을 만들 수 없다.
	 * 동기화: 불변이다. */
};

/* Bitmap of struct iommufd_sw_msi_map::id */
/*
 * [한국어] (위 영어 주석에 이어)
 * 어느 MSI 배정을 이미 처리했는지를 담는 비트맵.
 * 도메인마다 "이미 매핑한 것", 장치 그룹마다 "필요한 것"을 따로 들고
 * 있어, 도메인을 옮길 때 무엇을 다시 매핑해야 하는지 알 수 있다.
 * 이 비트맵의 크기가 곧 배정 수의 상한이다.
 */
struct iommufd_sw_msi_maps {
	DECLARE_BITMAP(bitmap, 64);
	/* [한국어] 배정 id 를 비트로 담는다.
	 * 설정자: 도메인에 매핑했을 때, 또는 그룹이 그 배정을 필요로 할 때.
	 * 읽는 자: 두 번 매핑하지 않으려는 검사, 도메인을 옮길 때의 재매핑.
	 * 값 범위: 64비트. 이 크기가 배정 수의 상한이다.
	 * 동기화: ictx->sw_msi_lock 또는 그룹 뮤텍스. */
};

#ifdef CONFIG_IRQ_MSI_IOMMU	/* [한국어] MSI 주소도 IOMMU 를 거치는 플랫폼에서만 */
int iommufd_sw_msi_install(struct iommufd_ctx *ictx,
			   struct iommufd_hwpt_paging *hwpt_paging,
			   struct iommufd_sw_msi_map *msi_map);
#endif

/*
 * [한국어] 열린 iommufd 파일 디스크립터 하나.
 * 사용자가 만든 모든 객체가 여기 매달리고, id 로 찾는다.
 * 파일이 닫히면 여기 달린 것이 전부 정리된다.
 */
struct iommufd_ctx {
	struct file *file;
	/* [한국어] 이 문맥을 여는 파일.
	 * 설정자: 열 때.
	 * 읽는 자: mmap 처리와 파일 수명 관리.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct xarray objects;
	/* [한국어] id → 객체 대응.
	 * 설정자: 객체를 만들고 없앨 때.
	 * 읽는 자: 모든 ioctl 이 id 로 객체를 찾는다.
	 * 값 범위: 32비트 id.
	 * 동기화: xarray 자체의 락. */
	struct xarray groups;
	/* [한국어] iommu 그룹 → iommufd_group 대응.
	 * 설정자: 장치를 묶고 풀 때.
	 * 읽는 자: 같은 그룹의 장치가 상태를 공유한다.
	 * 값 범위: 그룹 포인터를 키로 쓴다.
	 * 동기화: xarray 자체의 락. */
	wait_queue_head_t destroy_wait;
	/* [한국어] 객체 파괴를 기다리는 곳.
	 * 설정자: 참조가 0 이 될 때 깨운다.
	 * 읽는 자: REMOVE_WAIT 로 지우려는 쪽이 여기서 잔다.
	 * 값 범위: 대기 큐.
	 * 동기화: 대기 큐 자체. */
	struct rw_semaphore ioas_creation_lock;
	/* [한국어] IOAS 자동 생성을 직렬화한다.
	 * 설정자/읽는 자: VFIO 호환 경로가 기본 IOAS 를 만들 때.
	 * 값 범위: 초기화된 세마포어.
	 * 동기화: 이것 자체가 동기화 수단이다. */
	struct maple_tree mt_mmap;
	/* [한국어] mmap 오프셋 → MMIO 영역 대응.
	 * 설정자: _iommufd_alloc_mmap / destroy_mmap.
	 * 읽는 자: mmap 시스템 콜 처리.
	 * 값 범위: 오프셋 구간.
	 * 동기화: maple tree 자체의 락. */

	struct mutex sw_msi_lock;
	/* [한국어] MSI 배정 목록과 비트맵을 지킨다.
	 * 설정자/읽는 자: driver.c 의 MSI 경로.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: 이것 자체가 동기화 수단이다. */
	struct list_head sw_msi_list;
	/* [한국어] MSI 도어벨 배정 목록.
	 * 설정자: 새 도어벨을 만날 때.
	 * 읽는 자: 같은 도어벨을 찾을 때.
	 * 값 범위: iommufd_sw_msi_map 항목들.
	 * 동기화: sw_msi_lock. */
	unsigned int sw_msi_id;
	/* [한국어] 다음에 배정할 비트맵 자리.
	 * 설정자: 새 배정을 만들 때 하나 올린다.
	 * 읽는 자: 상한 검사.
	 * 값 범위: 0 부터 비트맵 크기까지.
	 * 동기화: sw_msi_lock. */

	u8 account_mode;
	/* [한국어] 고정 페이지를 누구의 한도에 계상할 것인가.
	 * 설정자: 사용자의 옵션 요청.
	 * 읽는 자: 새 iopt_pages 를 만들 때 물려준다.
	 * 값 범위: IOPT_PAGES_ACCOUNT_*.
	 * 동기화: 옵션 경로가 직렬화한다. */
	/* Compatibility with VFIO no iommu */
	u8 no_iommu_mode;
	/* [한국어] (원 주석: VFIO 의 no-iommu 와의 호환)
	 * 설정자: VFIO 호환 경로.
	 * 읽는 자: IOMMU 없이 동작하는 구성을 허용할지 판단할 때.
	 * 값 범위: 0 또는 1.
	 * 동기화: 열린 뒤 거의 바뀌지 않는다. */
	struct iommufd_ioas *vfio_ioas;
	/* [한국어] VFIO 호환 ioctl 이 쓰는 기본 IOAS.
	 * 설정자: 첫 VFIO 스타일 요청이 올 때 만들어 둔다.
	 * 읽는 자: 그 뒤의 VFIO 호환 요청들.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: ioas_creation_lock. */
};

/* Entry for iommufd_ctx::mt_mmap */
/*
 * [한국어] (위 영어 주석에 이어)
 * 사용자 공간에 노출한 MMIO 영역 하나.
 * 드라이버가 장치 레지스터를 게스트에 직접 보여 줄 때 쓰며, 배정된
 * 오프셋으로 iommufd 파일에 mmap 하면 이 영역이 매핑된다.
 */
struct iommufd_mmap {
	struct iommufd_object *owner;
	/* [한국어] 이 영역을 노출한 객체.
	 * 설정자: 배정할 때.
	 * 읽는 자: 거둘 때 짝이 맞는지 확인한다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: maple tree 의 락. */

	/* Page-shifted start position in mt_mmap to validate vma->vm_pgoff */
	unsigned long vm_pgoff;
	/* [한국어] (원 주석: vma->vm_pgoff 를 검증하기 위한 페이지 단위 시작 위치)
	 * 설정자: 배정할 때 오프셋을 페이지로 나눠 둔다.
	 * 읽는 자: mmap 시스템 콜이 준 vm_pgoff 와 비교한다.
	 * 값 범위: 페이지 번호.
	 * 동기화: 불변이다. */

	/* Physical range for io_remap_pfn_range() */
	phys_addr_t mmio_addr;
	/* [한국어] (원 주석: io_remap_pfn_range() 에 넘길 물리 범위)
	 * 설정자: 배정할 때.
	 * 읽는 자: mmap 처리가 실제로 매핑할 때.
	 * 값 범위: 페이지 정렬된 MMIO 주소.
	 * 동기화: 불변이다. */
	size_t length;
	/* [한국어] 그 영역의 길이.
	 * 설정자: 배정할 때.
	 * 읽는 자: mmap 요청 길이가 이보다 크면 거절한다.
	 * 값 범위: 페이지 정렬된 길이.
	 * 동기화: 불변이다. */
};

/*
 * The IOVA to PFN map. The map automatically copies the PFNs into multiple
 * domains and permits sharing of PFNs between io_pagetable instances. This
 * supports both a design where IOAS's are 1:1 with a domain (eg because the
 * domain is HW customized), or where the IOAS is 1:N with multiple generic
 * domains.  The io_pagetable holds an interval tree of iopt_areas which point
 * to shared iopt_pages which hold the pfns mapped to the page table.
 *
 * The locking order is domains_rwsem -> iova_rwsem -> pages::mutex
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * IOVA → PFN 대응 하나.
 *
 * 원 주석이 설계를 밝힌다 — 같은 매핑을 여러 도메인에 자동으로 복사해
 * 넣고, io_pagetable 인스턴스들 사이에서 PFN 을 공유한다. 그래서 IOAS 와
 * 도메인이 1:1 인 구성(하드웨어가 특수한 경우)과 1:N 인 구성(일반 도메인
 * 여럿) 을 모두 지원한다.
 *
 * 락 순서도 원 주석에 못박혀 있다: domains_rwsem → iova_rwsem →
 * pages::mutex. 이 계층에서 교착을 피하려면 이 순서를 지켜야 한다.
 */
struct io_pagetable {
	struct rw_semaphore domains_rwsem;
	/* [한국어] 도메인 목록과 area 의 pages 포인터를 지킨다.
	 * 설정자/읽는 자: 도메인 추가·제거, 구간 세우기·허물기.
	 * 값 범위: 초기화된 세마포어.
	 * 동기화: 락 순서의 맨 바깥이다(원 주석). */
	struct xarray domains;
	/* [한국어] 이 IOVA 공간의 매핑을 받는 도메인들.
	 * 설정자: iopt_table_add/remove_domain.
	 * 읽는 자: 매핑을 모든 도메인에 복사할 때.
	 * 값 범위: 순번 → iommu_domain.
	 * 동기화: domains_rwsem. */
	struct xarray access_list;
	/* [한국어] 이 공간을 보는 커널 쪽 접근들.
	 * 설정자: iopt_add/remove_access.
	 * 읽는 자: unmap 시 그들에게 알릴 때.
	 * 값 범위: 순번 → iommufd_access.
	 * 동기화: domains_rwsem. */
	unsigned int next_domain_id;
	/* [한국어] 다음 도메인에 줄 순번.
	 * 설정자: 도메인을 더할 때.
	 * 읽는 자: 같은 자리.
	 * 값 범위: 단조 증가.
	 * 동기화: domains_rwsem. */

	struct rw_semaphore iova_rwsem;
	/* [한국어] 아래 세 트리와 정렬 규칙을 지킨다.
	 * 설정자/읽는 자: 매핑·해제·예약 경로 전반.
	 * 값 범위: 초기화된 세마포어.
	 * 동기화: 락 순서의 가운데다(원 주석). */
	struct rb_root_cached area_itree;
	/* [한국어] IOVA 구간들의 트리.
	 * 설정자: 구간을 만들고 없앨 때.
	 * 읽는 자: 겹침 검사, 빈자리 찾기, 순회.
	 * 값 범위: iopt_area 노드들.
	 * 동기화: iova_rwsem. */
	/* IOVA that cannot become reserved, struct iopt_allowed */
	struct rb_root_cached allowed_itree;
	/* [한국어] (원 주석: 예약될 수 없는 IOVA, struct iopt_allowed)
	 * 설정자: 사용자의 ALLOW_IOVAS 요청.
	 * 읽는 자: 자동 할당이 후보를 고를 때.
	 * 값 범위: iopt_allowed 노드들. 비어 있으면 제한이 없다.
	 * 동기화: iova_rwsem. */
	/* IOVA that cannot be allocated, struct iopt_reserved */
	struct rb_root_cached reserved_itree;
	/* [한국어] (원 주석: 할당될 수 없는 IOVA, struct iopt_reserved)
	 * 설정자: 장치를 붙일 때 그 예약 구간에서.
	 * 읽는 자: 할당과 매핑이 이 범위를 피한다.
	 * 값 범위: iopt_reserved 노드들.
	 * 동기화: iova_rwsem. */
	u8 disable_large_pages;
	/* [한국어] 큰 페이지를 쓰지 못하게 막을 것인가.
	 * 설정자: 사용자의 옵션 요청, 또는 커널 접근이 요구할 때.
	 * 읽는 자: 도메인에 채울 때 크기 선택.
	 * 값 범위: 0 또는 1.
	 * 동기화: iova_rwsem.
	 * 막는 이유: 커널 접근이 페이지 단위로 세밀히 다루어야 할 때가 있다. */
	unsigned long iova_alignment;
	/* [한국어] IOVA 와 길이가 맞춰야 할 정렬.
	 * 설정자: 도메인이 더해질 때 그 도메인의 최소 페이지로 좁힌다.
	 * 읽는 자: 매핑 요청 검증과 자동 할당.
	 * 값 범위: 2의 거듭제곱.
	 * 동기화: iova_rwsem. */
};

void iopt_init_table(struct io_pagetable *iopt);	/* [한국어] 빈 IOVA 공간을 세운다 */
void iopt_destroy_table(struct io_pagetable *iopt);	/* [한국어] 그 공간과 남은 구간을 모두 허문다 */
int iopt_get_pages(struct io_pagetable *iopt, unsigned long iova,	/* [한국어] 그 범위의 페이지 묶음들을 목록으로 꺼낸다 */
		   unsigned long length, struct list_head *pages_list);
void iopt_free_pages_list(struct list_head *pages_list);	/* [한국어] 그 목록의 참조를 놓는다 */
/*
 * [한국어] iopt_map_* 계열에 넘기는 플래그.
 * IOVA 를 커널이 골라 달라는 요청이면 ALLOC_IOVA 를 준다.
 */
enum {
	IOPT_ALLOC_IOVA = 1 << 0,	/* [한국어] IOVA 를 커널이 골라 달라는 요청 */
};
int iopt_map_user_pages(struct iommufd_ctx *ictx, struct io_pagetable *iopt,	/* [한국어] 사용자 VA 를 IOVA 에 매핑한다 */
			unsigned long *iova, void __user *uptr,
			unsigned long length, int iommu_prot,
			unsigned int flags);
int iopt_map_file_pages(struct iommufd_ctx *ictx, struct io_pagetable *iopt,	/* [한국어] 파일의 일부를 IOVA 에 매핑한다 */
			unsigned long *iova, int fd,
			unsigned long start, unsigned long length,
			int iommu_prot, unsigned int flags);
int iopt_map_pages(struct io_pagetable *iopt, struct list_head *pages_list,	/* [한국어] 이미 얻어 둔 페이지 묶음들을 매핑한다 — IOAS 사이 복사에 쓴다 */
		   unsigned long length, unsigned long *dst_iova,
		   int iommu_prot, unsigned int flags);
int iopt_unmap_iova(struct io_pagetable *iopt, unsigned long iova,	/* [한국어] 그 범위의 매핑을 걷어낸다 */
		    unsigned long length, unsigned long *unmapped);
int iopt_unmap_all(struct io_pagetable *iopt, unsigned long *unmapped);	/* [한국어] 전 범위를 걷어낸다 */

int iopt_read_and_clear_dirty_data(struct io_pagetable *iopt,	/* [한국어] 더티 비트를 사용자 비트맵으로 옮기고 지운다 */
				   struct iommu_domain *domain,
				   unsigned long flags,
				   struct iommu_hwpt_get_dirty_bitmap *bitmap);
int iopt_set_dirty_tracking(struct io_pagetable *iopt,	/* [한국어] 도메인의 더티 추적을 켜고 끈다 */
			    struct iommu_domain *domain, bool enable);

void iommufd_access_notify_unmap(struct io_pagetable *iopt, unsigned long iova,	/* [한국어] 커널 쪽 소비자들에게 이 범위가 사라짐을 알린다 */
				 unsigned long length);
int iopt_table_add_domain(struct io_pagetable *iopt,	/* [한국어] 새 도메인에 기존 매핑을 모두 채운다 */
			  struct iommu_domain *domain);
void iopt_table_remove_domain(struct io_pagetable *iopt,	/* [한국어] 그 도메인에서 매핑을 걷어낸다 */
			      struct iommu_domain *domain);
int iopt_table_enforce_dev_resv_regions(struct io_pagetable *iopt,	/* [한국어] 장치가 요구한 예약 구간을 이 공간에 반영한다 */
					struct device *dev,
					phys_addr_t *sw_msi_start);
int iopt_set_allow_iova(struct io_pagetable *iopt,	/* [한국어] 사용자가 쓰겠다고 등록한 범위를 갈아 끼운다 */
			struct rb_root_cached *allowed_iova);
int iopt_reserve_iova(struct io_pagetable *iopt, unsigned long start,	/* [한국어] 쓸 수 없는 범위를 등록한다 */
		      unsigned long last, void *owner);
void iopt_remove_reserved_iova(struct io_pagetable *iopt, void *owner);	/* [한국어] 그 주인이 등록한 예약만 거둔다 */
int iopt_cut_iova(struct io_pagetable *iopt, unsigned long *iovas,	/* [한국어] 주어진 지점에서 구간을 쪼갠다 — 시험용 */
		  size_t num_iovas);
void iopt_enable_large_pages(struct io_pagetable *iopt);	/* [한국어] 큰 페이지를 다시 허용한다 */
int iopt_disable_large_pages(struct io_pagetable *iopt);	/* [한국어] 큰 페이지를 막고 기존 것을 쪼갠다 */

/*
 * [한국어] 처리 중인 ioctl 하나.
 * 사용자 버퍼의 위치와 크기, 커널로 복사해 온 명령 구조체를 함께 담아
 * 각 명령 구현에 넘긴다.
 */
struct iommufd_ucmd {
	struct iommufd_ctx *ictx;
	/* [한국어] 이 명령이 속한 문맥.
	 * 설정자: ioctl 진입점.
	 * 읽는 자: 객체를 찾고 만드는 모든 곳.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 호출 스택 값. */
	void __user *ubuffer;
	/* [한국어] 사용자 쪽 명령 버퍼.
	 * 설정자: ioctl 진입점.
	 * 읽는 자: 결과를 되돌릴 때.
	 * 값 범위: 사용자 주소.
	 * 동기화: 호출 스택 값. */
	u32 user_size;
	/* [한국어] 사용자가 알고 있는 구조체 크기.
	 * 설정자: ioctl 진입점이 헤더에서 읽는다.
	 * 읽는 자: 되돌릴 때 작은 쪽만 복사하려고.
	 * 값 범위: 최소 크기 이상.
	 * 동기화: 호출 스택 값. */
	void *cmd;
	/* [한국어] 커널로 복사해 온 명령 구조체.
	 * 설정자: ioctl 진입점.
	 * 읽는 자: 각 명령 구현.
	 * 값 범위: 명령 종류에 맞는 구조체.
	 * 동기화: 호출 스택 값. */
	struct iommufd_object *new_obj;
	/* [한국어] 이 명령이 만든 객체.
	 * 설정자: _ucmd 계열 할당기.
	 * 읽는 자: 코어가 성공 시 확정하고 실패 시 되돌린다.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: 호출 스택 값. */
};

int iommufd_vfio_ioctl(struct iommufd_ctx *ictx, unsigned int cmd,	/* [한국어] VFIO 의 옛 ioctl 을 이 계층으로 옮겨 준다 */
		       unsigned long arg);

/* Copy the response in ucmd->cmd back to userspace. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_ucmd_respond - 처리 결과를 사용자 버퍼로 되돌린다
 *
 * @ucmd: 처리 중인 명령.
 * @cmd_len: 커널이 채운 길이.
 * @return: 0 성공, -EFAULT 면 사용자 버퍼가 잘못됐다.
 *
 * 사용자가 준 크기와 커널이 채운 크기 중 작은 쪽만 복사한다. 두 값이
 * 다를 수 있는 이유는 구조체가 버전마다 늘어나기 때문이다 — 옛 사용자는
 * 짧은 구조체를, 새 커널은 긴 것을 안다.
 */
static inline int iommufd_ucmd_respond(struct iommufd_ucmd *ucmd,
				       size_t cmd_len)
{
	if (copy_to_user(ucmd->ubuffer, ucmd->cmd,	/* [한국어] 커널이 채운 구조체를 사용자 버퍼로 */
			 min_t(size_t, ucmd->user_size, cmd_len)))	/* [한국어] 옛 사용자는 짧은 구조체를 안다 — 작은 쪽만 복사한다 */
		return -EFAULT;	/* [한국어] 사용자 버퍼가 잘못됐다 */
	return 0;	/* [한국어] 성공 */
}

/*
 * [한국어]
 * iommufd_lock_obj - 객체를 붙잡는다(두 겹 참조를 함께)
 *
 * @obj: 붙잡을 객체.
 * @return: 성공하면 참, 이미 파괴 중이면 거짓.
 *
 * users 와 wait_cnt 를 함께 올린다. 전자는 보통의 참조 계수이고, 후자는
 * "파괴를 기다리는 쪽"이 언제 깨어날지를 정한다.
 *
 * 둘 다 0 검사를 하는 이유: 파괴가 시작된 객체는 users 를 0 으로 만들고,
 * 그 뒤로는 아무도 새로 붙잡을 수 없어야 한다.
 *
 * 원 주석이 되돌리기가 안전한 근거를 밝힌다 — 호출자가 이미 참조를
 * 갖고 있지 않다면 이 함수는 xa_lock 아래에서 불렸고, 갖고 있다면
 * users 가 1 보다 크므로 여기서 내려도 0 이 되지 않는다.
 */
static inline bool iommufd_lock_obj(struct iommufd_object *obj)
{
	if (!refcount_inc_not_zero(&obj->users))	/* [한국어] 파괴가 시작된 객체면 0 이라 */
		return false;	/* [한국어] 새로 붙잡을 수 없다 */
	if (!refcount_inc_not_zero(&obj->wait_cnt)) {	/* [한국어] 기다림 계수도 함께 */
		/*
		 * If the caller doesn't already have a ref on obj this must be
		 * called under the xa_lock. Otherwise the caller is holding a
		 * ref on users. Thus it cannot be one before this decrement.
		 */
		refcount_dec(&obj->users);	/* [한국어] (원 주석: 호출자가 참조를 갖고 있지 않다면 xa_lock 아래이고, 갖고 있다면 여기서 내려도 0 이 되지 않는다) */
		return false;	/* [한국어] 파괴 중이다 */
	}
	return true;	/* [한국어] 두 계수를 모두 올렸다 */
}

struct iommufd_object *iommufd_get_object(struct iommufd_ctx *ictx, u32 id,	/* [한국어] id 와 종류로 객체를 찾아 붙잡는다 */
					  enum iommufd_object_type type);
/*
 * [한국어]
 * iommufd_put_object - 붙잡았던 객체를 놓는다
 *
 * @ictx: 그 객체가 속한 문맥.
 * @obj: 놓을 객체.
 *
 * 순서가 중요하다. 원 주석이 이유를 밝힌다 — users 를 먼저 내려야
 * REMOVE_WAIT 로 기다리는 쪽이 "users 는 0 이 아닌데 wait_cnt 는 0" 인
 * 잠깐의 거짓 상태를 보지 않는다.
 *
 * wait_cnt 가 0 이 되면 기다리는 쪽을 깨운다.
 */
static inline void iommufd_put_object(struct iommufd_ctx *ictx,
				      struct iommufd_object *obj)
{
	/*
	 * Users first, then wait_cnt so that REMOVE_WAIT never sees a spurious
	 * !0 users with a 0 wait_cnt.
	 */
	refcount_dec(&obj->users);	/* [한국어] (원 주석: users 를 먼저 내려야 REMOVE_WAIT 가 거짓 상태를 보지 않는다) */
	if (refcount_dec_and_test(&obj->wait_cnt))	/* [한국어] 마지막 기다림 계수였으면 */
		wake_up_interruptible_all(&ictx->destroy_wait);	/* [한국어] 파괴를 기다리는 쪽을 깨운다 */
}

void iommufd_object_abort(struct iommufd_ctx *ictx, struct iommufd_object *obj);	/* [한국어] 아직 확정하지 않은 객체를 되돌린다 */
void iommufd_object_abort_and_destroy(struct iommufd_ctx *ictx,	/* [한국어] 되돌리며 타입별 정리도 부른다 */
				      struct iommufd_object *obj);
void iommufd_object_finalize(struct iommufd_ctx *ictx,	/* [한국어] 객체를 사용자에게 보이게 확정한다 */
			     struct iommufd_object *obj);

/*
 * [한국어] iommufd_object_remove 의 동작을 정하는 플래그.
 * REMOVE_WAIT 는 다른 사용자가 놓을 때까지 기다리고, TOMBSTONE 은
 * id 를 비워 두지 않고 비석으로 남겨 재사용을 막는다.
 */
enum {
	REMOVE_WAIT		= BIT(0),	/* [한국어] 다른 사용자가 놓을 때까지 기다린다 */
	REMOVE_OBJ_TOMBSTONE	= BIT(1),	/* [한국어] id 를 비석으로 남겨 재사용을 막는다 */
};
int iommufd_object_remove(struct iommufd_ctx *ictx,	/* [한국어] 객체를 목록에서 빼고 파괴한다 */
			  struct iommufd_object *to_destroy, u32 id,
			  unsigned int flags);

/*
 * The caller holds a users refcount and wants to destroy the object. At this
 * point the caller has no wait_cnt reference and at least the xarray will be
 * holding one.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_object_destroy_user - 사용자 요청으로 객체를 파괴한다
 *
 * @ictx: 그 객체가 속한 문맥.
 * @obj: 파괴할 객체.
 *
 * 다른 사용자가 놓을 때까지 기다렸다가 지운다. 원 주석이 상태를 설명한다 —
 * 이 시점에 호출자는 users 참조만 갖고 wait_cnt 참조는 없으며, 적어도
 * xarray 가 하나를 들고 있다.
 *
 * 실패는 코드 쪽 버그라 경고만 한다. 원 주석대로 호출자의 참조는 되돌려져
 * 있어, 파일이 닫힐 때 다시 해제를 시도하게 된다.
 */
static inline void iommufd_object_destroy_user(struct iommufd_ctx *ictx,
					       struct iommufd_object *obj)
{
	int ret;	/* [한국어] 파괴 결과 */

	ret = iommufd_object_remove(ictx, obj, obj->id, REMOVE_WAIT);	/* [한국어] 다른 사용자가 놓을 때까지 기다린다 */

	/*
	 * If there is a bug and we couldn't destroy the object then we did put
	 * back the caller's users refcount and will eventually try to free it
	 * again during close.
	 */
	WARN_ON(ret);	/* [한국어] (원 주석: 버그로 파괴에 실패했다면 호출자의 참조는 되돌려졌고, 파일이 닫힐 때 다시 시도된다) */
}

/*
 * Similar to iommufd_object_destroy_user(), except that the object ID is left
 * reserved/tombstoned.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_object_tombstone_user - 파괴하되 id 를 비석으로 남긴다
 *
 * @ictx: 그 객체가 속한 문맥.
 * @obj: 파괴할 객체.
 *
 * 보통의 파괴와 달리 id 를 비워 두지 않는다. 그 id 를 다른 객체가 물려받으면
 * 사용자 공간이 옛 id 로 새 객체를 건드리게 되므로, 되돌릴 수 없는 상황에서
 * 그것을 막는다.
 */
static inline void iommufd_object_tombstone_user(struct iommufd_ctx *ictx,
						 struct iommufd_object *obj)
{
	int ret;	/* [한국어] 파괴 결과 */

	ret = iommufd_object_remove(ictx, obj, obj->id,	/* [한국어] 기다린 뒤 파괴하되 */
				    REMOVE_WAIT | REMOVE_OBJ_TOMBSTONE);	/* [한국어] id 는 비석으로 남겨 재사용을 막는다 */

	/*
	 * If there is a bug and we couldn't destroy the object then we did put
	 * back the caller's users refcount and will eventually try to free it
	 * again during close.
	 */
	WARN_ON(ret);	/* [한국어] (원 주석: 실패해도 파일이 닫힐 때 다시 시도된다) */
}

/*
 * The HWPT allocated by autodomains is used in possibly many devices and
 * is automatically destroyed when its refcount reaches zero.
 *
 * If userspace uses the HWPT manually, even for a short term, then it will
 * disrupt this refcounting and the auto-free in the kernel will not work.
 * Userspace that tries to use the automatically allocated HWPT must be careful
 * to ensure that it is consistently destroyed, eg by not racing accesses
 * and by not attaching an automatic HWPT to a device manually.
 */
static inline void	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_object_put_and_try_destroy - 참조를 놓고 마지막이면 파괴한다
 *
 * @ictx: 그 객체가 속한 문맥.
 * @obj: 놓을 객체.
 *
 * 자동으로 만들어진 HWPT 를 위한 경로다. 여러 장치가 그것을 공유하고,
 * 마지막 장치가 떠날 때 저절로 사라져야 한다.
 *
 * 원 주석이 경고하는 것: 사용자가 그 자동 HWPT 를 직접 건드리면 이
 * 참조 계수가 흐트러져 자동 해제가 동작하지 않는다.
 */
iommufd_object_put_and_try_destroy(struct iommufd_ctx *ictx,
				   struct iommufd_object *obj)
{
	iommufd_object_remove(ictx, obj, obj->id, 0);	/* [한국어] 기다리지 않는다 — 다른 사용자가 있으면 그쪽이 마지막에 파괴한다 */
}

/*
 * Callers of these normal object allocators must call iommufd_object_finalize()
 * to finalize the object, or call iommufd_object_abort_and_destroy() to revert
 * the allocation.
 */
struct iommufd_object *_iommufd_object_alloc(struct iommufd_ctx *ictx,	/* [한국어] 객체를 만들고 id 를 배정한다 */
					     size_t size,
					     enum iommufd_object_type type);

/*
 * [한국어] 객체를 만들며 배치를 컴파일 시 확인하는 매크로.
 *
 * BUILD_BUG_ON_ZERO 가 obj 필드의 오프셋이 0 인지 검사한다 — 0 이 아니면
 * 그 식이 컴파일 오류가 된다. 공통 코드가 struct iommufd_object 포인터와
 * 구체 타입 포인터를 형변환만으로 오가므로, 그 필드가 맨 앞이어야 한다.
 */
#define __iommufd_object_alloc(ictx, ptr, type, obj)                           \
	container_of(_iommufd_object_alloc(                                    \
			     ictx,                                             \
			     sizeof(*(ptr)) + BUILD_BUG_ON_ZERO(               \
						      offsetof(typeof(*(ptr)), \
							       obj) != 0),     \
			     type),                                            \
		     typeof(*(ptr)), obj)	/* [한국어] 구체 타입 포인터로 돌려준다 */

/*
 * [한국어] 위 매크로의 흔한 경우 — 필드 이름이 obj 인 경우.
 */
#define iommufd_object_alloc(ictx, ptr, type) \
	__iommufd_object_alloc(ictx, ptr, type, obj)	/* [한국어] 필드 이름이 obj 인 흔한 경우 */

/*
 * Callers of these _ucmd allocators should not call iommufd_object_finalize()
 * or iommufd_object_abort_and_destroy(), as the core automatically does that.
 */
struct iommufd_object *
_iommufd_object_alloc_ucmd(struct iommufd_ucmd *ucmd, size_t size,
			   enum iommufd_object_type type);

/*
 * [한국어] (위 영어 주석에 이어)
 * ioctl 처리 중에 객체를 만드는 매크로.
 * 성공하면 코어가 알아서 확정하고, 실패하면 알아서 되돌린다 — 그래서
 * 호출자가 finalize/abort 를 부르면 안 된다.
 */
#define __iommufd_object_alloc_ucmd(ucmd, ptr, type, obj)                      \
	container_of(_iommufd_object_alloc_ucmd(                               \
			     ucmd,                                             \
			     sizeof(*(ptr)) + BUILD_BUG_ON_ZERO(               \
						      offsetof(typeof(*(ptr)), \
							       obj) != 0),     \
			     type),                                            \
		     typeof(*(ptr)), obj)	/* [한국어] 구체 타입 포인터로 돌려준다 */

/*
 * [한국어] 위 매크로의 흔한 경우 — 필드 이름이 obj 인 경우.
 */
#define iommufd_object_alloc_ucmd(ucmd, ptr, type) \
	__iommufd_object_alloc_ucmd(ucmd, ptr, type, obj)	/* [한국어] 필드 이름이 obj 인 흔한 경우 */

/*
 * The IO Address Space (IOAS) pagetable is a virtual page table backed by the
 * io_pagetable object. It is a user controlled mapping of IOVA -> PFNs. The
 * mapping is copied into all of the associated domains and made available to
 * in-kernel users.
 *
 * Every iommu_domain that is created is wrapped in a iommufd_hw_pagetable
 * object. When we go to attach a device to an IOAS we need to get an
 * iommu_domain and wrapping iommufd_hw_pagetable for it.
 *
 * An iommu_domain & iommfd_hw_pagetable will be automatically selected
 * for a device based on the hwpt_list. If no suitable iommu_domain
 * is found a new iommu_domain will be created.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * 사용자가 관리하는 IOVA 공간 하나.
 *
 * 원 주석이 두 가지를 밝힌다. 이 매핑은 딸린 모든 도메인에 복사되고
 * 커널 안의 소비자에게도 열린다는 것, 그리고 장치를 붙일 때 알맞은
 * 도메인이 hwpt_list 에서 골라지고 없으면 새로 만들어진다는 것이다.
 */
struct iommufd_ioas {
	struct iommufd_object obj;
	/* [한국어] 공통 객체 머리말. 반드시 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: id 조회와 참조 계수.
	 * 값 범위: 초기화된 객체.
	 * 동기화: 코어의 객체 규칙. */
	struct io_pagetable iopt;
	/* [한국어] 이 IOAS 의 IOVA → PFN 대응.
	 * 설정자/읽는 자: 매핑 관련 모든 경로.
	 * 값 범위: 초기화된 페이지 테이블.
	 * 동기화: 그 안의 세마포어들. */
	struct mutex mutex;
	/* [한국어] hwpt_list 를 지킨다.
	 * 설정자/읽는 자: 도메인을 자동으로 만들고 없앨 때.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: 이것 자체가 동기화 수단이다. */
	struct list_head hwpt_list;
	/* [한국어] 이 IOAS 를 쓰는 HWPT 들.
	 * 설정자: HWPT 를 만들고 없앨 때.
	 * 읽는 자: 장치를 붙일 때 재사용할 도메인을 고른다.
	 * 값 범위: iommufd_hwpt_paging 항목들.
	 * 동기화: mutex. */
};

/*
 * [한국어]
 * iommufd_get_ioas - id 로 IOAS 객체를 찾아 붙잡는다
 *
 * @ictx: 문맥.
 * @id: 사용자가 준 객체 id.
 * @return: 그 IOAS, 없거나 종류가 다르면 ERR_PTR.
 *
 * 종류 검사가 함께 이루어진다 — 사용자가 아무 id 나 줄 수 있으므로,
 * 기대한 종류가 아니면 오류다.
 */
static inline struct iommufd_ioas *iommufd_get_ioas(struct iommufd_ctx *ictx,
						    u32 id)
{
	return container_of(iommufd_get_object(ictx, id, IOMMUFD_OBJ_IOAS),	/* [한국어] 종류까지 확인해 찾고 */
			    struct iommufd_ioas, obj);	/* [한국어] 구체 타입으로 되짚는다 */
}

struct iommufd_ioas *iommufd_ioas_alloc(struct iommufd_ctx *ictx);	/* [한국어] 새 IOVA 공간을 만든다 */
int iommufd_ioas_alloc_ioctl(struct iommufd_ucmd *ucmd);	/* [한국어] IOMMU_IOAS_ALLOC 처리 */
void iommufd_ioas_destroy(struct iommufd_object *obj);	/* [한국어] IOAS 파괴 */
int iommufd_ioas_iova_ranges(struct iommufd_ucmd *ucmd);	/* [한국어] 쓸 수 있는 IOVA 범위를 사용자에게 알린다 */
int iommufd_ioas_allow_iovas(struct iommufd_ucmd *ucmd);	/* [한국어] 사용자가 쓸 범위를 지정한다 */
int iommufd_ioas_map(struct iommufd_ucmd *ucmd);	/* [한국어] 사용자 VA 를 매핑한다 */
int iommufd_ioas_map_file(struct iommufd_ucmd *ucmd);	/* [한국어] 파일의 일부를 매핑한다 */
int iommufd_ioas_change_process(struct iommufd_ucmd *ucmd);	/* [한국어] 고정된 페이지의 계상을 다른 프로세스로 옮긴다 */
int iommufd_ioas_copy(struct iommufd_ucmd *ucmd);	/* [한국어] 한 IOAS 의 매핑을 다른 IOAS 로 복사한다 — 다시 고정하지 않는다 */
int iommufd_ioas_unmap(struct iommufd_ucmd *ucmd);	/* [한국어] 매핑을 걷어낸다 */
int iommufd_ioas_option(struct iommufd_ucmd *ucmd);	/* [한국어] IOAS 단위 옵션을 읽고 쓴다 */
int iommufd_option_rlimit_mode(struct iommu_option *cmd,	/* [한국어] 고정 페이지 계상 방식을 바꾼다 */
			       struct iommufd_ctx *ictx);

int iommufd_vfio_ioas(struct iommufd_ucmd *ucmd);	/* [한국어] VFIO 호환 경로가 쓸 기본 IOAS 를 지정한다 */
int iommufd_check_iova_range(struct io_pagetable *iopt,	/* [한국어] 더티 비트맵 요청의 범위를 검증한다 */
			     struct iommu_hwpt_get_dirty_bitmap *bitmap);

/*
 * A HW pagetable is called an iommu_domain inside the kernel. This user object
 * allows directly creating and inspecting the domains. Domains that have kernel
 * owned page tables will be associated with an iommufd_ioas that provides the
 * IOVA to PFN map.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * iommu_domain 을 감싼 사용자 객체.
 * 사용자가 도메인을 직접 만들고 들여다볼 수 있게 해 주며, 커널이 페이지
 * 테이블을 소유하는 도메인은 IOAS 하나에 매여 그 매핑을 받는다.
 */
struct iommufd_hw_pagetable {
	struct iommufd_object obj;
	/* [한국어] 공통 객체 머리말. 반드시 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: id 조회와 참조 계수.
	 * 값 범위: 초기화된 객체.
	 * 동기화: 코어의 객체 규칙. */
	struct iommu_domain *domain;
	/* [한국어] 감싸고 있는 실제 도메인.
	 * 설정자: 만들 때.
	 * 읽는 자: 매핑과 장치 붙임이 모두 이것을 쓴다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct iommufd_fault *fault;
	/* [한국어] 이 도메인의 페이지 폴트를 받을 큐.
	 * 설정자: 만들 때 사용자가 지정하면.
	 * 읽는 자: 폴트 처리기.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: 불변이다. */
	bool pasid_compat : 1;
	/* [한국어] PASID 를 붙일 수 있는 도메인인가.
	 * 설정자: 만들 때 요청 플래그에서.
	 * 읽는 자: PASID 붙임 요청을 검증할 때.
	 * 값 범위: 0 또는 1.
	 * 동기화: 불변이다. */
};

/*
 * [한국어] 페이징 HWPT — IOAS 의 매핑을 받는 보통의 도메인.
 * 사용자가 명시적으로 만들 수도 있고, 장치를 IOAS 에 붙일 때 자동으로
 * 만들어질 수도 있다(auto_domain).
 */
struct iommufd_hwpt_paging {
	struct iommufd_hw_pagetable common;
	/* [한국어] 공통 부분. 맨 앞이어야 되짚기가 성립한다.
	 * 설정자: 할당기.
	 * 읽는 자: 종류를 가리지 않는 코드.
	 * 값 범위: 초기화된 객체.
	 * 동기화: 코어의 객체 규칙. */
	struct iommufd_ioas *ioas;
	/* [한국어] 매핑을 받아 오는 IOAS.
	 * 설정자: 만들 때.
	 * 읽는 자: 매핑 복사와 해제.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	bool auto_domain : 1;
	/* [한국어] 자동으로 만들어진 도메인인가.
	 * 설정자: 만들 때.
	 * 읽는 자: 마지막 참조가 사라질 때 저절로 없앨지 판단.
	 * 값 범위: 0 또는 1.
	 * 동기화: 불변이다. */
	bool enforce_cache_coherency : 1;
	/* [한국어] 일관성 강제를 요구했는가.
	 * 설정자: 만들 때 요청 플래그에서.
	 * 읽는 자: 장치를 붙일 때 그 장치가 그것을 지원하는지 확인.
	 * 값 범위: 0 또는 1.
	 * 동기화: 불변이다. */
	bool nest_parent : 1;
	/* [한국어] 중첩의 부모로 쓸 도메인인가.
	 * 설정자: 만들 때 요청 플래그에서.
	 * 읽는 자: 중첩 HWPT 를 만들 때 부모 자격을 확인.
	 * 값 범위: 0 또는 1.
	 * 동기화: 불변이다. */
	/* Head at iommufd_ioas::hwpt_list */
	struct list_head hwpt_item;
	/* [한국어] (원 주석: iommufd_ioas::hwpt_list 에 매달린다)
	 * 설정자: 만들고 없앨 때.
	 * 읽는 자: 재사용할 도메인을 고르는 순회.
	 * 값 범위: 그 목록의 항목.
	 * 동기화: ioas->mutex. */
	struct iommufd_sw_msi_maps present_sw_msi;
	/* [한국어] 이 도메인에 이미 매핑한 MSI 배정들.
	 * 설정자: iommufd_sw_msi_install.
	 * 읽는 자: 같은 도어벨을 두 번 매핑하지 않으려는 검사.
	 * 값 범위: 배정 id 비트맵.
	 * 동기화: ictx->sw_msi_lock. */
};

/*
 * [한국어] 중첩 HWPT — 게스트가 만든 1단계 표를 가리키는 도메인.
 * 부모 페이징 HWPT 가 2단계(게스트 물리 → 호스트 물리)를 맡고, 이쪽은
 * 게스트가 관리하는 1단계를 하드웨어에 걸어 준다.
 */
struct iommufd_hwpt_nested {
	struct iommufd_hw_pagetable common;
	/* [한국어] 공통 부분. 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: 종류를 가리지 않는 코드.
	 * 값 범위: 초기화된 객체.
	 * 동기화: 코어의 객체 규칙. */
	struct iommufd_hwpt_paging *parent;
	/* [한국어] 2단계를 맡는 부모 도메인.
	 * 설정자: 만들 때 사용자가 지정한다.
	 * 읽는 자: find_hwpt_paging 이 IOAS 나 MSI 를 찾을 때.
	 * 값 범위: nest_parent 로 만들어진 페이징 HWPT.
	 * 동기화: 불변이다. */
	struct iommufd_viommu *viommu;
	/* [한국어] 이 도메인이 속한 vIOMMU.
	 * 설정자: 만들 때.
	 * 읽는 자: 무효화 명령을 그 vIOMMU 로 보낼 때.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: 불변이다. */
};

/*
 * [한국어]
 * hwpt_is_paging - 이 HWPT 가 페이징용인지 답한다
 *
 * @hwpt: 볼 객체.
 * @return: 페이징용이면 참.
 *
 * 중첩 HWPT 는 IOAS 를 갖지 않아 다루는 방식이 다르다.
 */
static inline bool hwpt_is_paging(struct iommufd_hw_pagetable *hwpt)
{
	return hwpt->obj.type == IOMMUFD_OBJ_HWPT_PAGING;	/* [한국어] 중첩 HWPT 는 IOAS 를 갖지 않는다 */
}

static inline struct iommufd_hwpt_paging *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * to_hwpt_paging - 공통 부분에서 페이징 HWPT 로 되짚는다
 *
 * @hwpt: 공통 부분.
 * @return: 그것을 품은 페이징 HWPT.
 *
 * 호출자가 종류를 이미 확인했다고 전제한다.
 */
to_hwpt_paging(struct iommufd_hw_pagetable *hwpt)
{
	return container_of(hwpt, struct iommufd_hwpt_paging, common);	/* [한국어] 호출자가 종류를 확인했다고 전제한다 */
}

static inline struct iommufd_hwpt_nested *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * to_hwpt_nested - 공통 부분에서 중첩 HWPT 로 되짚는다
 *
 * @hwpt: 공통 부분.
 * @return: 그것을 품은 중첩 HWPT.
 */
to_hwpt_nested(struct iommufd_hw_pagetable *hwpt)
{
	return container_of(hwpt, struct iommufd_hwpt_nested, common);	/* [한국어] 호출자가 종류를 확인했다고 전제한다 */
}

static inline struct iommufd_hwpt_paging *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * find_hwpt_paging - 이 HWPT 에 딸린 페이징 HWPT 를 찾는다
 *
 * @hwpt: 볼 객체.
 * @return: 페이징 HWPT, 없으면 NULL.
 *
 * 중첩 HWPT 면 그 부모가 답이다. IOAS 나 MSI 매핑처럼 페이징 쪽에만 있는
 * 것을 찾을 때, 어느 종류가 오든 이 함수 하나로 처리할 수 있다.
 */
find_hwpt_paging(struct iommufd_hw_pagetable *hwpt)
{
	switch (hwpt->obj.type) {	/* [한국어] 종류에 따라 */
	case IOMMUFD_OBJ_HWPT_PAGING:	/* [한국어] 자기 자신이 페이징이면 */
		return to_hwpt_paging(hwpt);	/* [한국어] 그대로 */
	case IOMMUFD_OBJ_HWPT_NESTED:	/* [한국어] 중첩이면 */
		return to_hwpt_nested(hwpt)->parent;	/* [한국어] 그 부모가 페이징이다 */
	default:	/* [한국어] 그 밖의 종류에는 페이징이 없다 */
		return NULL;	/* [한국어] 그 밖의 종류에는 페이징이 없다 */
	}
}

static inline struct iommufd_hwpt_paging *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_get_hwpt_paging - id 로 페이징 HWPT 를 찾아 붙잡는다
 *
 * @ucmd: 처리 중인 명령.
 * @id: 사용자가 준 객체 id.
 * @return: 그 객체, 없거나 종류가 다르면 ERR_PTR.
 */
iommufd_get_hwpt_paging(struct iommufd_ucmd *ucmd, u32 id)
{
	return container_of(iommufd_get_object(ucmd->ictx, id,	/* [한국어] 종류까지 확인해 찾고 */
					       IOMMUFD_OBJ_HWPT_PAGING),	/* [한국어] 페이징 HWPT 여야 한다 */
			    struct iommufd_hwpt_paging, common.obj);	/* [한국어] 구체 타입으로 되짚는다 */
}

static inline struct iommufd_hw_pagetable *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_get_hwpt_nested - id 로 중첩 HWPT 를 찾아 붙잡는다
 *
 * @ucmd: 처리 중인 명령.
 * @id: 사용자가 준 객체 id.
 * @return: 그 객체의 공통 부분, 없거나 종류가 다르면 ERR_PTR.
 */
iommufd_get_hwpt_nested(struct iommufd_ucmd *ucmd, u32 id)
{
	return container_of(iommufd_get_object(ucmd->ictx, id,	/* [한국어] 종류까지 확인해 찾고 */
					       IOMMUFD_OBJ_HWPT_NESTED),	/* [한국어] 중첩 HWPT 여야 한다 */
			    struct iommufd_hw_pagetable, obj);	/* [한국어] 공통 부분으로 돌려준다 */
}

int iommufd_hwpt_set_dirty_tracking(struct iommufd_ucmd *ucmd);	/* [한국어] 더티 추적을 켜고 끈다 */
int iommufd_hwpt_get_dirty_bitmap(struct iommufd_ucmd *ucmd);	/* [한국어] 더티 비트를 사용자 비트맵으로 옮긴다 */

struct iommufd_hwpt_paging *	/* [한국어] 아래 함수의 반환 타입 */
iommufd_hwpt_paging_alloc(struct iommufd_ctx *ictx, struct iommufd_ioas *ioas,	/* [한국어] 페이징 HWPT 를 만든다 */
			  struct iommufd_device *idev, ioasid_t pasid,
			  u32 flags, bool immediate_attach,
			  const struct iommu_user_data *user_data);
int iommufd_hw_pagetable_attach(struct iommufd_hw_pagetable *hwpt,	/* [한국어] 장치를 그 도메인에 붙인다 */
				struct iommufd_device *idev, ioasid_t pasid);
struct iommufd_hw_pagetable *	/* [한국어] 아래 함수의 반환 타입 */
iommufd_hw_pagetable_detach(struct iommufd_device *idev, ioasid_t pasid);	/* [한국어] 장치를 떼고 붙어 있던 도메인을 돌려준다 */
void iommufd_hwpt_paging_destroy(struct iommufd_object *obj);	/* [한국어] 페이징 HWPT 파괴 */
void iommufd_hwpt_paging_abort(struct iommufd_object *obj);	/* [한국어] 확정 전 되돌리기 */
void iommufd_hwpt_nested_destroy(struct iommufd_object *obj);	/* [한국어] 중첩 HWPT 파괴 */
void iommufd_hwpt_nested_abort(struct iommufd_object *obj);	/* [한국어] 확정 전 되돌리기 */
int iommufd_hwpt_alloc(struct iommufd_ucmd *ucmd);	/* [한국어] IOMMU_HWPT_ALLOC 처리 */
int iommufd_hwpt_invalidate(struct iommufd_ucmd *ucmd);	/* [한국어] 게스트가 낸 무효화 명령을 하드웨어에 전한다 */

/*
 * [한국어]
 * iommufd_hw_pagetable_put - HWPT 참조를 놓는다
 *
 * @ictx: 문맥.
 * @hwpt: 놓을 객체.
 *
 * 자동으로 만들어진 페이징 HWPT 만 특별히 다룬다 — 마지막 참조가 사라지면
 * 저절로 사라져야 하므로 파괴를 시도한다. 사용자가 명시적으로 만든 것은
 * 참조만 내리고 그대로 둔다.
 *
 * lockdep 주장이 붙은 이유: 파괴 경로가 그 IOAS 의 뮤텍스를 잡으므로,
 * 이미 쥔 채로 부르면 교착이다.
 */
static inline void iommufd_hw_pagetable_put(struct iommufd_ctx *ictx,
					    struct iommufd_hw_pagetable *hwpt)
{
	if (hwpt->obj.type == IOMMUFD_OBJ_HWPT_PAGING) {	/* [한국어] 페이징 HWPT 만 특별히 다룬다 */
		struct iommufd_hwpt_paging *hwpt_paging = to_hwpt_paging(hwpt);	/* [한국어] 구체 타입으로 */

		if (hwpt_paging->auto_domain) {	/* [한국어] 자동으로 만들어진 것이면 */
			lockdep_assert_not_held(&hwpt_paging->ioas->mutex);	/* [한국어] 파괴 경로가 이 뮤텍스를 잡으므로 쥔 채로 부르면 교착이다 */
			iommufd_object_put_and_try_destroy(ictx, &hwpt->obj);	/* [한국어] 마지막 참조면 저절로 사라진다 */
			return;	/* [한국어] 끝 */
		}
	}
	refcount_dec(&hwpt->obj.users);	/* [한국어] 사용자가 만든 것은 참조만 내린다 */
}

struct iommufd_attach;	/* [한국어] device.c 안에서만 쓰이는 타입 */

/*
 * [한국어] iommu 장치 그룹 하나에 대한 iommufd 쪽 상태.
 * 그룹은 서로 분리할 수 없는 장치들의 묶음이라, 붙임과 MSI 배정도
 * 그룹 단위로 관리된다.
 */
struct iommufd_group {
	struct kref ref;
	/* [한국어] 이 그룹 상태의 참조 수.
	 * 설정자: 그룹의 장치를 묶고 풀 때.
	 * 읽는 자: 0 이 되면 해제된다.
	 * 값 범위: 1 이상.
	 * 동기화: kref 자체가 원자적이다. */
	struct mutex lock;
	/* [한국어] 아래 붙임 상태를 지킨다.
	 * 설정자/읽는 자: 장치 붙임과 뗌.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: 이것 자체가 동기화 수단이다. */
	struct iommufd_ctx *ictx;
	/* [한국어] 이 그룹이 속한 문맥.
	 * 설정자: 만들 때.
	 * 읽는 자: 객체를 찾고 만들 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct iommu_group *group;
	/* [한국어] 코어의 장치 그룹.
	 * 설정자: 만들 때 참조를 잡는다.
	 * 읽는 자: 코어의 붙임 API 에 넘긴다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct xarray pasid_attach;
	/* [한국어] PASID → 붙임 상태 대응.
	 * 설정자: 붙이고 뗄 때.
	 * 읽는 자: 어느 HWPT 에 붙어 있는지 물을 때.
	 * 값 범위: PASID → iommufd_attach. PASID 0 이 기본 붙임이다.
	 * 동기화: lock. */
	struct iommufd_sw_msi_maps required_sw_msi;
	/* [한국어] 이 그룹이 필요로 하는 MSI 배정들.
	 * 설정자: iommufd_sw_msi 가 도어벨을 배정할 때.
	 * 읽는 자: 도메인을 옮길 때 새 도메인에 다시 매핑해야 할 것들.
	 * 값 범위: 배정 id 비트맵.
	 * 동기화: ictx->sw_msi_lock. */
	phys_addr_t sw_msi_start;
	/* [한국어] 이 그룹이 쓸 MSI IOVA 창의 시작.
	 * 설정자: 장치를 붙일 때 그 예약 구간에서.
	 * 읽는 자: 실제 IOVA 를 계산할 때.
	 * 값 범위: 유효한 주소, 또는 PHYS_ADDR_MAX(그 플랫폼이 MSI 를 변환하지 않음).
	 * 동기화: lock. */
};

/*
 * A iommufd_device object represents the binding relationship between a
 * consuming driver and the iommufd. These objects are created/destroyed by
 * external drivers, not by userspace.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * 소비 드라이버와 iommufd 사이의 묶임 하나.
 * 원 주석이 짚듯 이 객체는 사용자 공간이 아니라 외부 드라이버(VFIO 등)가
 * 만들고 없앤다 — 사용자는 그 결과로 생긴 id 만 본다.
 */
struct iommufd_device {
	struct iommufd_object obj;
	/* [한국어] 공통 객체 머리말. 반드시 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: id 조회와 참조 계수.
	 * 값 범위: 초기화된 객체.
	 * 동기화: 코어의 객체 규칙. */
	struct iommufd_ctx *ictx;
	/* [한국어] 이 장치가 묶인 문맥.
	 * 설정자: 묶을 때.
	 * 읽는 자: 객체를 찾고 만들 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct iommufd_group *igroup;
	/* [한국어] 이 장치가 속한 그룹 상태.
	 * 설정자: 묶을 때.
	 * 읽는 자: 붙임과 MSI 처리가 그룹 단위로 이루어진다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct list_head group_item;
	/* [한국어] 그룹의 장치 목록에 매달리는 고리.
	 * 설정자: 묶고 풀 때.
	 * 읽는 자: 그룹의 장치들을 훑을 때.
	 * 값 범위: 그 목록의 항목.
	 * 동기화: igroup->lock. */
	/* always the physical device */
	struct device *dev;
	/* [한국어] (원 주석: 늘 물리 장치)
	 * 설정자: 묶을 때.
	 * 읽는 자: 코어의 붙임 API 와 예약 구간 조회.
	 * 값 범위: 유효한 포인터. 가상 장치가 아니라 실제 장치다.
	 * 동기화: 불변이다. */
	bool enforce_cache_coherency;
	/* [한국어] 이 장치가 일관성 강제를 지원하는가.
	 * 설정자: 묶을 때 코어에 물어본다.
	 * 읽는 자: 그것을 요구하는 도메인에 붙일 수 있는지 판단.
	 * 값 범위: 0 또는 1.
	 * 동기화: 불변이다. */
	struct iommufd_vdevice *vdev;
	/* [한국어] 이 장치에 대응하는 가상 장치.
	 * 설정자: vdevice 를 만들고 없앨 때.
	 * 읽는 자: 게스트 id 와 실제 장치를 오갈 때.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: 객체 참조가 지킨다. */
	bool destroying;
	/* [한국어] 이 장치가 없어지는 중인가.
	 * 설정자: pre_destroy 가 세운다.
	 * 읽는 자: 새 vdevice 를 만들려는 요청을 거절할 때.
	 * 값 범위: 0 또는 1.
	 * 동기화: 코어의 객체 규칙. */
};

static inline struct iommufd_device *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_get_device - id 로 장치 객체를 찾아 붙잡는다
 *
 * @ucmd: 처리 중인 명령.
 * @id: 사용자가 준 객체 id.
 * @return: 그 장치, 없거나 종류가 다르면 ERR_PTR.
 */
iommufd_get_device(struct iommufd_ucmd *ucmd, u32 id)
{
	return container_of(iommufd_get_object(ucmd->ictx, id,	/* [한국어] 종류까지 확인해 찾고 */
					       IOMMUFD_OBJ_DEVICE),	/* [한국어] 장치여야 한다 */
			    struct iommufd_device, obj);	/* [한국어] 구체 타입으로 되짚는다 */
}

void iommufd_device_pre_destroy(struct iommufd_object *obj);	/* [한국어] 파괴 표시를 먼저 세워 새 참조를 막는다 */
void iommufd_device_destroy(struct iommufd_object *obj);	/* [한국어] 장치 객체 파괴 */
int iommufd_get_hw_info(struct iommufd_ucmd *ucmd);	/* [한국어] 하드웨어 능력을 사용자에게 알린다 */

struct device *iommufd_global_device(void);	/* [한국어] 소유자 없는 매핑에 쓸 대표 장치 */

/*
 * [한국어] 커널 안의 소비자가 IOAS 의 페이지를 직접 보는 통로.
 * VFIO 의 일부 경로처럼 장치가 아니라 커널 코드가 그 메모리를 읽고 써야
 * 할 때 쓴다. 이 객체가 살아 있는 동안 그 범위의 페이지가 고정된다.
 */
struct iommufd_access {
	struct iommufd_object obj;
	/* [한국어] 공통 객체 머리말. 반드시 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: id 조회와 참조 계수.
	 * 값 범위: 초기화된 객체.
	 * 동기화: 코어의 객체 규칙. */
	struct iommufd_ctx *ictx;
	/* [한국어] 이 접근이 속한 문맥.
	 * 설정자: 만들 때. 커널 내부용이면 NULL 이다.
	 * 읽는 자: 내부용인지 가르는 검사.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: 불변이다. */
	struct iommufd_ioas *ioas;
	/* [한국어] 지금 붙어 있는 IOAS.
	 * 설정자: attach/detach.
	 * 읽는 자: 페이지를 얻을 때.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: ioas_lock. */
	struct iommufd_ioas *ioas_unpin;
	/* [한국어] 고정을 풀어야 할 옛 IOAS.
	 * 설정자: 붙임을 바꿀 때.
	 * 읽는 자: 정리 경로.
	 * 값 범위: 유효한 포인터 또는 NULL.
	 * 동기화: ioas_lock.
	 * 따로 두는 이유: 새 IOAS 에 붙인 뒤에야 옛 것의 고정을 풀 수 있다. */
	struct mutex ioas_lock;
	/* [한국어] 위 두 포인터를 지킨다.
	 * 설정자/읽는 자: attach/detach 와 페이지 접근.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: 이것 자체가 동기화 수단이다. */
	const struct iommufd_access_ops *ops;
	/* [한국어] 이 접근의 소유자가 준 콜백.
	 * 설정자: 만들 때.
	 * 읽는 자: unmap 이 일어났을 때 알리는 경로.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	void *data;
	/* [한국어] 그 콜백에 함께 넘길 값.
	 * 설정자: 만들 때.
	 * 읽는 자: 콜백 호출부.
	 * 값 범위: 소유자가 정한다.
	 * 동기화: 불변이다. */
	unsigned long iova_alignment;
	/* [한국어] 이 접근이 요구하는 IOVA 정렬.
	 * 설정자: 만들 때 ops 에서.
	 * 읽는 자: IOAS 의 정렬을 좁힐 때.
	 * 값 범위: 2의 거듭제곱.
	 * 동기화: 불변이다. */
	u32 iopt_access_list_id;
	/* [한국어] IOAS 의 접근 목록에서의 자리.
	 * 설정자: iopt_add_access.
	 * 읽는 자: 뗄 때 그 자리를 지운다.
	 * 값 범위: xarray 인덱스.
	 * 동기화: ioas_lock. */
};

int iopt_add_access(struct io_pagetable *iopt, struct iommufd_access *access);	/* [한국어] 커널 쪽 접근을 이 공간에 등록한다 */
void iopt_remove_access(struct io_pagetable *iopt,	/* [한국어] 그 등록을 거둔다 */
			struct iommufd_access *access, u32 iopt_access_list_id);
void iommufd_access_destroy_object(struct iommufd_object *obj);	/* [한국어] 접근 객체 파괴 */

/* iommufd_access for internal use */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * iommufd_access_is_internal - 커널 내부용 접근인지 답한다
 *
 * @access: 볼 객체.
 * @return: 내부용이면 참.
 *
 * 사용자 공간이 만든 접근은 문맥을 갖지만, 커널이 자기 용도로 만든 것은
 * 갖지 않는다. 그 차이로 가른다.
 */
static inline bool iommufd_access_is_internal(struct iommufd_access *access)
{
	return !access->ictx;	/* [한국어] 사용자가 만든 접근만 문맥을 갖는다 */
}

struct iommufd_access *iommufd_access_create_internal(struct iommufd_ctx *ictx);	/* [한국어] 커널이 자기 용도로 만드는 접근 */

static inline void	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_access_destroy_internal - 커널 내부용 접근을 없앤다
 *
 * @ictx: 문맥.
 * @access: 없앨 접근.
 */
iommufd_access_destroy_internal(struct iommufd_ctx *ictx,
				struct iommufd_access *access)
{
	iommufd_object_destroy_user(ictx, &access->obj);	/* [한국어] 다른 사용자가 놓을 때까지 기다렸다가 지운다 */
}

int iommufd_access_attach_internal(struct iommufd_access *access,	/* [한국어] 그 접근을 IOAS 에 붙인다 */
				   struct iommufd_ioas *ioas);

/*
 * [한국어]
 * iommufd_access_detach_internal - 커널 내부용 접근을 IOAS 에서 뗀다
 *
 * @access: 뗄 접근.
 *
 * 고정해 두었던 페이지가 여기서 풀린다.
 */
static inline void iommufd_access_detach_internal(struct iommufd_access *access)
{
	iommufd_access_detach(access);	/* [한국어] 고정해 두었던 페이지가 여기서 풀린다 */
}

/*
 * [한국어] 사용자 공간에 사건을 전하는 큐의 공통 부분.
 * 페이지 폴트(fault)와 vIOMMU 이벤트(veventq)가 이것을 품고 있으며,
 * 각자 별도의 파일 디스크립터로 읽힌다.
 */
struct iommufd_eventq {
	struct iommufd_object obj;
	/* [한국어] 공통 객체 머리말. 반드시 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: id 조회와 참조 계수.
	 * 값 범위: 초기화된 객체.
	 * 동기화: 코어의 객체 규칙. */
	struct iommufd_ctx *ictx;
	/* [한국어] 이 큐가 속한 문맥.
	 * 설정자: 만들 때.
	 * 읽는 자: 객체를 찾을 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct file *filep;
	/* [한국어] 이 큐를 읽는 파일.
	 * 설정자: 만들 때 새 파일을 연다.
	 * 읽는 자: 사용자 공간이 read() 로 사건을 꺼낸다.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */

	spinlock_t lock; /* protects the deliver list */
	/* [한국어] (원 주석: deliver 목록을 지킨다)
	 * 설정자/읽는 자: 사건을 넣고 꺼낼 때.
	 * 값 범위: 초기화된 스핀락.
	 * 동기화: 인터럽트 스레드에서도 잡으므로 스핀락이다. */
	struct list_head deliver;
	/* [한국어] 아직 읽히지 않은 사건들.
	 * 설정자: 사건을 넣을 때, 읽을 때.
	 * 읽는 자: read() 구현.
	 * 값 범위: 사건 항목들.
	 * 동기화: lock. */

	struct wait_queue_head wait_queue;
	/* [한국어] 읽기를 기다리는 곳.
	 * 설정자: 사건이 들어오면 깨운다.
	 * 읽는 자: 빈 큐에서 read() 한 쪽이 여기서 잔다.
	 * 값 범위: 대기 큐.
	 * 동기화: 대기 큐 자체. */
};

/*
 * [한국어] iommu 코어의 attach 핸들에 iommufd 정보를 붙인 것.
 * 코어가 장치별로 들고 있는 핸들에서 어느 iommufd 장치인지 되짚을 수
 * 있어야, 폴트나 MSI 처리에서 문맥을 찾을 수 있다.
 */
struct iommufd_attach_handle {
	struct iommu_attach_handle handle;
	/* [한국어] 코어가 들고 있는 핸들. 맨 앞이어야 되짚기가 성립한다.
	 * 설정자: 붙일 때 코어에 넘긴다.
	 * 읽는 자: 코어가 장치별로 보관한다.
	 * 값 범위: 초기화된 핸들.
	 * 동기화: 코어의 그룹 뮤텍스. */
	struct iommufd_device *idev;
	/* [한국어] 그 핸들이 가리키는 iommufd 장치.
	 * 설정자: 붙일 때.
	 * 읽는 자: 폴트와 MSI 처리가 문맥을 찾을 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
};

/* Convert an iommu attach handle to iommufd handle. */
/*
 * [한국어] (위 영어 주석에 이어)
 * 코어의 핸들에서 iommufd 쪽 표현으로 되짚는다.
 */
#define to_iommufd_handle(hdl)	container_of(hdl, struct iommufd_attach_handle, handle)

/*
 * An iommufd_fault object represents an interface to deliver I/O page faults
 * to the user space. These objects are created/destroyed by the user space and
 * associated with hardware page table objects during page-table allocation.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * I/O 페이지 폴트를 사용자 공간에 전하는 객체.
 * 원 주석대로 사용자가 만들고 없애며, HWPT 를 만들 때 그것에 연결된다.
 * 응답을 되받아 하드웨어에 전하는 것까지 이 객체의 몫이다.
 */
struct iommufd_fault {
	struct iommufd_eventq common;
	/* [한국어] 공통 큐 부분. 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: 큐를 가리지 않는 코드.
	 * 값 범위: 초기화된 큐.
	 * 동기화: 그 안의 스핀락. */
	struct mutex mutex; /* serializes response flows */
	/* [한국어] (원 주석: 응답 흐름을 직렬화한다)
	 * 설정자/읽는 자: 사용자가 폴트 응답을 쓸 때.
	 * 값 범위: 초기화된 뮤텍스.
	 * 동기화: 이것 자체가 동기화 수단이다. */
	struct xarray response;
	/* [한국어] 응답을 기다리는 폴트들.
	 * 설정자: 폴트를 사용자에게 전할 때 넣고, 응답이 오면 뺀다.
	 * 읽는 자: 응답이 어느 폴트에 대한 것인지 찾을 때.
	 * 값 범위: 폴트 식별자 → 폴트 묶음.
	 * 동기화: xarray 의 락과 mutex. */
};

static inline struct iommufd_fault *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * eventq_to_fault - 공통 큐에서 폴트 객체로 되짚는다
 *
 * @eventq: 공통 부분.
 * @return: 그것을 품은 폴트 객체.
 */
eventq_to_fault(struct iommufd_eventq *eventq)
{
	return container_of(eventq, struct iommufd_fault, common);	/* [한국어] 공통 큐를 품은 폴트 객체로 */
}

static inline struct iommufd_fault *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_get_fault - id 로 폴트 객체를 찾아 붙잡는다
 *
 * @ucmd: 처리 중인 명령.
 * @id: 사용자가 준 객체 id.
 * @return: 그 객체, 없거나 종류가 다르면 ERR_PTR.
 */
iommufd_get_fault(struct iommufd_ucmd *ucmd, u32 id)
{
	return container_of(iommufd_get_object(ucmd->ictx, id,	/* [한국어] 종류까지 확인해 찾고 */
					       IOMMUFD_OBJ_FAULT),	/* [한국어] 폴트 객체여야 한다 */
			    struct iommufd_fault, common.obj);	/* [한국어] 구체 타입으로 되짚는다 */
}

int iommufd_fault_alloc(struct iommufd_ucmd *ucmd);	/* [한국어] 폴트 큐를 만든다 */
void iommufd_fault_destroy(struct iommufd_object *obj);	/* [한국어] 폴트 큐 파괴 */
int iommufd_fault_iopf_handler(struct iopf_group *group);	/* [한국어] 드라이버가 낸 폴트를 큐에 넣는다 */
void iommufd_auto_response_faults(struct iommufd_hw_pagetable *hwpt,	/* [한국어] 장치가 떠날 때 답 없는 폴트에 실패로 응답한다 */
				  struct iommufd_attach_handle *handle);

/* An iommufd_vevent represents a vIOMMU event in an iommufd_veventq */
/*
 * [한국어] (위 영어 주석에 이어)
 * 큐에 담긴 vIOMMU 이벤트 하나.
 * 길이가 가변이라 유연 배열로 끝나며, 그래서 이 구조체를 품는 쪽에서는
 * 반드시 마지막 필드여야 한다.
 */
struct iommufd_vevent {
	struct iommufd_vevent_header header;
	/* [한국어] 사용자 공간에 그대로 복사되는 머리말.
	 * 설정자: 큐에 넣을 때 순번과 플래그를 채운다.
	 * 읽는 자: read() 가 그대로 내보낸다.
	 * 값 범위: uapi 가 정한 형식.
	 * 동기화: 큐의 스핀락. */
	struct list_head node; /* for iommufd_eventq::deliver */
	/* [한국어] (원 주석: iommufd_eventq::deliver 에 매달린다)
	 * 설정자: 넣고 꺼낼 때.
	 * 읽는 자: 큐 순회.
	 * 값 범위: 그 목록의 항목.
	 * 동기화: 큐의 스핀락. */
	ssize_t data_len;
	/* [한국어] 이벤트 내용의 바이트 길이.
	 * 설정자: 만들 때.
	 * 읽는 자: 복사할 길이를 정할 때, 그리고 아래 배열의 크기 검사.
	 * 값 범위: 0 보다 크다.
	 * 동기화: 불변이다. */
	u64 event_data[] __counted_by(data_len);
	/* [한국어] 드라이버가 준 이벤트 내용.
	 * 설정자: 만들 때 복사해 넣는다.
	 * 읽는 자: read() 가 사용자 공간으로 복사한다.
	 * 값 범위: uapi 가 종류별로 정한 형식.
	 * 동기화: 불변이다.
	 * 유연 배열이라 이 구조체를 품는 쪽에서는 반드시 마지막이어야 한다. */
};

/*
 * [한국어] 이 항목이 "이벤트를 잃어버렸다"는 표시인지 판별한다.
 * 큐가 가득 차 이벤트를 버렸을 때, 미리 준비된 항목 하나를 대신 넣어
 * 사용자 공간이 그 사실을 알 수 있게 한다.
 */
#define vevent_for_lost_events_header(vevent) \
	(vevent->header.flags & IOMMU_VEVENTQ_FLAG_LOST_EVENTS)	/* [한국어] 그 플래그가 서 있으면 잃어버림 표시다 */

/*
 * An iommufd_veventq object represents an interface to deliver vIOMMU events to
 * the user space. It is created/destroyed by the user space and associated with
 * a vIOMMU object during the allocations.
 */
/*
 * [한국어] (위 영어 주석에 이어)
 * vIOMMU 이벤트를 사용자 공간에 전하는 큐.
 * 종류마다 하나씩 두며, vIOMMU 객체를 만들 때 연결된다.
 */
struct iommufd_veventq {
	struct iommufd_eventq common;
	/* [한국어] 공통 큐 부분. 맨 앞이어야 한다.
	 * 설정자: 할당기.
	 * 읽는 자: 큐를 가리지 않는 코드.
	 * 값 범위: 초기화된 큐.
	 * 동기화: 그 안의 스핀락. */
	struct iommufd_viommu *viommu;
	/* [한국어] 이 큐가 딸린 vIOMMU.
	 * 설정자: 만들 때.
	 * 읽는 자: 파괴 시 목록에서 뺄 때.
	 * 값 범위: 유효한 포인터.
	 * 동기화: 불변이다. */
	struct list_head node; /* for iommufd_viommu::veventqs */
	/* [한국어] (원 주석: iommufd_viommu::veventqs 에 매달린다)
	 * 설정자: 만들고 없앨 때.
	 * 읽는 자: 종류로 큐를 찾는 순회.
	 * 값 범위: 그 목록의 항목.
	 * 동기화: viommu->veventqs_rwsem. */

	enum iommu_veventq_type type;
	/* [한국어] 이 큐가 받는 이벤트 종류.
	 * 설정자: 만들 때 사용자가 지정한다.
	 * 읽는 자: 이벤트를 어느 큐로 보낼지 고를 때.
	 * 값 범위: uapi 가 정한 종류.
	 * 동기화: 불변이다. */
	unsigned int depth;
	/* [한국어] 큐가 담을 수 있는 이벤트 수.
	 * 설정자: 만들 때 사용자가 지정한다.
	 * 읽는 자: 가득 찼는지 판단할 때.
	 * 값 범위: 1 이상.
	 * 동기화: 불변이다. */

	/* Use common.lock for protection */
	u32 num_events;
	/* [한국어] (원 주석: common.lock 이 지킨다) 지금 큐에 든 이벤트 수.
	 * 설정자: 넣을 때 올리고 읽을 때 내린다.
	 * 읽는 자: 가득 찼는지 판단.
	 * 값 범위: 0 부터 depth 까지.
	 * 동기화: common.lock. */
	u32 sequence;
	/* [한국어] 다음 이벤트에 붙일 순번.
	 * 설정자: 이벤트를 넣을 때마다 하나 올린다.
	 * 읽는 자: 사용자 공간이 빠진 이벤트 수를 셀 때.
	 * 값 범위: 0 부터 INT_MAX. 부호 있는 정수로 읽히므로 그 위로 감싼다.
	 * 동기화: common.lock. */

	/* Must be last as it ends in a flexible-array member. */
	struct iommufd_vevent lost_events_header;
	/* [한국어] (원 주석: 유연 배열로 끝나므로 반드시 마지막 필드여야 한다)
	 * 설정자: 큐가 가득 차거나 할당에 실패했을 때 이것을 대신 넣는다.
	 * 읽는 자: 사용자 공간이 그 플래그로 "여기서 빠졌다"를 안다.
	 * 값 범위: 내용이 없는 이벤트 하나.
	 * 동기화: common.lock.
	 * 미리 만들어 두는 이유: 잃어버림을 알리는 데 또 할당이 필요하면 안 된다. */
};

static inline struct iommufd_veventq *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * eventq_to_veventq - 공통 큐에서 vIOMMU 이벤트 큐로 되짚는다
 *
 * @eventq: 공통 부분.
 * @return: 그것을 품은 이벤트 큐.
 */
eventq_to_veventq(struct iommufd_eventq *eventq)
{
	return container_of(eventq, struct iommufd_veventq, common);	/* [한국어] 공통 큐를 품은 이벤트 큐로 */
}

static inline struct iommufd_veventq *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_get_veventq - id 로 vIOMMU 이벤트 큐를 찾아 붙잡는다
 *
 * @ucmd: 처리 중인 명령.
 * @id: 사용자가 준 객체 id.
 * @return: 그 객체, 없거나 종류가 다르면 ERR_PTR.
 */
iommufd_get_veventq(struct iommufd_ucmd *ucmd, u32 id)
{
	return container_of(iommufd_get_object(ucmd->ictx, id,	/* [한국어] 종류까지 확인해 찾고 */
					       IOMMUFD_OBJ_VEVENTQ),	/* [한국어] 이벤트 큐여야 한다 */
			    struct iommufd_veventq, common.obj);	/* [한국어] 구체 타입으로 되짚는다 */
}

int iommufd_veventq_alloc(struct iommufd_ucmd *ucmd);	/* [한국어] vIOMMU 이벤트 큐를 만든다 */
void iommufd_veventq_destroy(struct iommufd_object *obj);	/* [한국어] 그 큐 파괴 */
void iommufd_veventq_abort(struct iommufd_object *obj);	/* [한국어] 확정 전 되돌리기 */

/*
 * [한국어]
 * iommufd_vevent_handler - 이벤트를 큐에 넣고 기다리는 쪽을 깨운다
 *
 * @veventq: 넣을 큐.
 * @vevent: 넣을 이벤트(잃어버림 표시일 수도 있다).
 *
 * 잃어버림 표시의 재배치가 이 함수의 요점이다. 원 주석이 밝히듯, 그
 * 표시는 늘 큐의 맨 뒤에 있어야 한다 — 그래야 사용자 공간이 "여기까지
 * 읽었고 그 뒤로 잃어버렸다"로 읽는다. 그래서 새 이벤트를 넣기 전에
 * 뒤에 있던 표시를 떼어 낸다.
 *
 * 넣는 것 자체가 잃어버림 표시일 수도 있는데, 그 경우는 순번만 갱신하는
 * 것과 같다.
 *
 * 순번을 INT_MAX 로 감싸는 이유: 사용자 공간이 부호 있는 정수로 읽어
 * 빠진 이벤트 수를 계산하므로 음수가 되면 안 된다.
 *
 * 실행 컨텍스트: 큐 락을 쥔 채. 드라이버의 인터럽트 스레드에서 불린다.
 */
static inline void iommufd_vevent_handler(struct iommufd_veventq *veventq,
					  struct iommufd_vevent *vevent)
{
	struct iommufd_eventq *eventq = &veventq->common;	/* [한국어] 공통 큐 */

	lockdep_assert_held(&eventq->lock);	/* [한국어] 목록 조작과 순번 갱신이 원자적이어야 한다 */

	/*
	 * Remove the lost_events_header and add the new node at the same time.
	 * Note the new node can be lost_events_header, for a sequence update.
	 */
	if (list_is_last(&veventq->lost_events_header.node, &eventq->deliver))	/* [한국어] (원 주석: 잃어버림 표시를 떼면서 새 항목을 같은 자리에 넣는다) */
		list_del(&veventq->lost_events_header.node);	/* [한국어] 그 표시는 늘 맨 뒤에 있어야 한다 */
	list_add_tail(&vevent->node, &eventq->deliver);	/* [한국어] (원 주석: 새 항목이 잃어버림 표시일 수도 있다 — 순번만 갱신하는 경우) */
	vevent->header.sequence = veventq->sequence;	/* [한국어] 사용자 공간이 빠진 개수를 셀 수 있게 */
	veventq->sequence = (veventq->sequence + 1) & INT_MAX;	/* [한국어] 부호 있는 정수로 읽히므로 음수가 되면 안 된다 */

	wake_up_interruptible(&eventq->wait_queue);	/* [한국어] 읽기를 기다리는 쪽을 깨운다 */
}

static inline struct iommufd_viommu *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_get_viommu - id 로 vIOMMU 객체를 찾아 붙잡는다
 *
 * @ucmd: 처리 중인 명령.
 * @id: 사용자가 준 객체 id.
 * @return: 그 객체, 없거나 종류가 다르면 ERR_PTR.
 */
iommufd_get_viommu(struct iommufd_ucmd *ucmd, u32 id)
{
	return container_of(iommufd_get_object(ucmd->ictx, id,	/* [한국어] 종류까지 확인해 찾고 */
					       IOMMUFD_OBJ_VIOMMU),	/* [한국어] vIOMMU 여야 한다 */
			    struct iommufd_viommu, obj);	/* [한국어] 구체 타입으로 되짚는다 */
}

static inline struct iommufd_veventq *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_viommu_find_veventq - 그 종류의 이벤트 큐를 찾는다
 *
 * @viommu: 볼 vIOMMU.
 * @type: 찾을 이벤트 종류.
 * @return: 그 큐, 없으면 NULL.
 *
 * 사용자 공간이 구독한 종류만 큐가 있다. 없으면 그 이벤트는 전할 곳이
 * 없어 버려진다.
 */
iommufd_viommu_find_veventq(struct iommufd_viommu *viommu,
			    enum iommu_veventq_type type)
{
	struct iommufd_veventq *veventq, *next;	/* [한국어] 순회용 */

	lockdep_assert_held(&viommu->veventqs_rwsem);	/* [한국어] 순회 중 목록이 바뀌면 안 된다 */

	list_for_each_entry_safe(veventq, next, &viommu->veventqs, node) {	/* [한국어] 구독된 큐들을 훑는다 */
		if (veventq->type == type)	/* [한국어] 종류가 맞으면 */
			return veventq;	/* [한국어] 그 큐로 보낸다 */
	}
	return NULL;	/* [한국어] 구독하지 않은 종류의 이벤트는 버려진다 */
}

int iommufd_viommu_alloc_ioctl(struct iommufd_ucmd *ucmd);	/* [한국어] vIOMMU 객체를 만든다 */
void iommufd_viommu_destroy(struct iommufd_object *obj);	/* [한국어] vIOMMU 파괴 */
int iommufd_vdevice_alloc_ioctl(struct iommufd_ucmd *ucmd);	/* [한국어] 가상 장치를 만든다 */
void iommufd_vdevice_destroy(struct iommufd_object *obj);	/* [한국어] 가상 장치 파괴 */
void iommufd_vdevice_abort(struct iommufd_object *obj);	/* [한국어] 확정 전 되돌리기 */
int iommufd_hw_queue_alloc_ioctl(struct iommufd_ucmd *ucmd);	/* [한국어] 게스트가 직접 쓰는 하드웨어 큐를 만든다 */
void iommufd_hw_queue_destroy(struct iommufd_object *obj);	/* [한국어] 그 큐 파괴 */

static inline struct iommufd_vdevice *	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_get_vdevice - id 로 가상 장치를 찾아 붙잡는다
 *
 * @ictx: 문맥.
 * @id: 사용자가 준 객체 id.
 * @return: 그 객체, 없거나 종류가 다르면 ERR_PTR.
 */
iommufd_get_vdevice(struct iommufd_ctx *ictx, u32 id)
{
	return container_of(iommufd_get_object(ictx, id,	/* [한국어] 종류까지 확인해 찾고 */
					       IOMMUFD_OBJ_VDEVICE),	/* [한국어] 가상 장치여야 한다 */
			    struct iommufd_vdevice, obj);	/* [한국어] 구체 타입으로 되짚는다 */
}

#ifdef CONFIG_IOMMUFD_TEST	/* [한국어] 셀프테스트를 켠 커널에서만 */
int iommufd_test(struct iommufd_ucmd *ucmd);	/* [한국어] 시험 전용 ioctl */
void iommufd_selftest_destroy(struct iommufd_object *obj);	/* [한국어] 시험 객체 파괴 */
extern size_t iommufd_test_memory_limit;	/* [한국어] 시험이 쓸 메모리 상한 */
void iommufd_test_syz_conv_iova_id(struct iommufd_ucmd *ucmd,	/* [한국어] 퍼저가 낸 IOVA 를 유효한 값으로 바꾼다 */
				   unsigned int ioas_id, u64 *iova, u32 *flags);
bool iommufd_should_fail(void);	/* [한국어] 지정한 지점에서 일부러 실패를 만든다 */
int __init iommufd_test_init(void);	/* [한국어] 시험 환경 초기화 */
void iommufd_test_exit(void);	/* [한국어] 그 정리 */
bool iommufd_selftest_is_mock_dev(struct device *dev);	/* [한국어] 가짜 장치인지 판별한다 */
int iommufd_test_dma_buf_iommufd_map(struct dma_buf_attachment *attachment,	/* [한국어] 시험용 dma-buf 매핑 */
				     struct phys_vec *phys);
#else
/*
 * [한국어]
 * iommufd_test_syz_conv_iova_id - 시험을 끈 커널의 빈 구현
 *
 * @ucmd: 무시된다.
 * @ioas_id: 무시된다.
 * @iova: 무시된다.
 * @flags: 무시된다.
 *
 * 퍼저(syzkaller)가 낸 IOVA 를 유효한 값으로 바꿔 주는 시험용 도우미다.
 * 시험을 끄면 호출부를 #ifdef 로 감싸지 않기 위해 빈 함수를 둔다.
 */
static inline void iommufd_test_syz_conv_iova_id(struct iommufd_ucmd *ucmd,
						 unsigned int ioas_id,
						 u64 *iova, u32 *flags)
{
}
/*
 * [한국어]
 * iommufd_should_fail - 시험을 끈 커널의 빈 구현
 *
 * @return: 늘 거짓.
 *
 * 시험 빌드에서는 지정한 지점에서 일부러 실패를 만들어 오류 처리 경로를
 * 검증한다. 운영 커널에서는 상수 거짓이라 그 검사가 통째로 사라진다.
 */
static inline bool iommufd_should_fail(void)
{
	return false;	/* [한국어] 운영 커널에서는 이 검사가 통째로 사라진다 */
}
/*
 * [한국어]
 * iommufd_test_init - 시험을 끈 커널의 빈 구현
 *
 * @return: 늘 0.
 */
static inline int __init iommufd_test_init(void)
{
	return 0;	/* [한국어] 시험을 끈 커널에서는 할 일이 없다 */
}
/*
 * [한국어]
 * iommufd_test_exit - 시험을 끈 커널의 빈 구현
 */
static inline void iommufd_test_exit(void)
{
}
/*
 * [한국어]
 * iommufd_selftest_is_mock_dev - 시험을 끈 커널의 빈 구현
 *
 * @dev: 무시된다.
 * @return: 늘 거짓.
 *
 * 시험용 가짜 장치는 실제 하드웨어가 없어 몇몇 경로를 건너뛰어야 한다.
 */
static inline bool iommufd_selftest_is_mock_dev(struct device *dev)
{
	return false;	/* [한국어] 가짜 장치가 존재하지 않는다 */
}
static inline int	/* [한국어] 아래 함수의 반환 타입 */
/*
 * [한국어]
 * iommufd_test_dma_buf_iommufd_map - 시험을 끈 커널의 빈 구현
 *
 * @attachment: 무시된다.
 * @phys: 무시된다.
 * @return: 늘 -EOPNOTSUPP.
 */
iommufd_test_dma_buf_iommufd_map(struct dma_buf_attachment *attachment,
				 struct phys_vec *phys)
{
	return -EOPNOTSUPP;	/* [한국어] 시험용 dma-buf 경로가 없다 */
}
#endif
#endif
