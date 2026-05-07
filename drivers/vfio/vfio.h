/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */
#ifndef __VFIO_VFIO_H__
#define __VFIO_VFIO_H__

/*
 * [한국어 설명] VFIO 코어 내부 전용 헤더 (drivers/vfio/vfio.h)
 *
 * === 파일의 역할 ===
 * drivers/vfio/ 안의 모든 .c 파일이 공유하는 "내부 ABI" 를 정의한다.
 * 사용자 공간이나 외부 모듈(vfio-pci-core, mlx5 등)이 아닌, VFIO 핵심
 * 모듈(vfio_main.c / group.c / container.c / device_cdev.c / iommufd.c /
 * virqfd.c / debugfs.c) 사이에서만 쓰이는 다음 항목을 모은다:
 *
 *  (a) struct vfio_device_file — open() 으로 만들어지는 fd 의 컨텍스트
 *      (어느 device 인지 + 어느 group 인지 + 권한 게이트 + KVM/iommufd 핀).
 *  (b) struct vfio_group — legacy "group" 추상화(여러 device 가 한 IOMMU
 *      group 을 공유). CONFIG_VFIO_GROUP=y 일 때만 정의.
 *  (c) struct vfio_iommu_driver_ops/_driver — legacy container 모드의
 *      IOMMU backend 등록 vtable. CONFIG_VFIO_CONTAINER=y 일 때만 정의.
 *  (d) enum vfio_group_type — IOMMU/EMULATED/NO_IOMMU 3종.
 *  (e) 위 객체를 생성·소멸·조회하는 내부 함수 prototype 모음.
 *  (f) 컴파일 시 비활성화된 기능에 대한 "stub inline" — disable 시
 *      호출자 코드를 #ifdef 없이 그대로 두기 위함(Code coverage 최적화).
 *
 * === 전체 아키텍처에서의 위치 ===
 * VFIO 코어는 두 시대(legacy "group/container" + new "cdev/iommufd") 가
 * 공존한다. 본 헤더는 두 모델 모두를 같은 코어가 다룰 수 있도록 다음
 * Kconfig 게이트 매트릭스를 표현한다:
 *
 *   CONFIG_VFIO_GROUP            : /dev/vfio/<groupID> 인터페이스 활성.
 *   CONFIG_VFIO_CONTAINER        : /dev/vfio/vfio + ioctl(VFIO_SET_IOMMU,
 *                                  TYPE1) 인터페이스 활성.
 *   CONFIG_VFIO_DEVICE_CDEV      : /dev/vfio/devices/vfio<N> + iommufd
 *                                  현대 인터페이스 활성.
 *   CONFIG_VFIO_NOIOMMU          : IOMMU 없이 ad-hoc 사용(매우 위험).
 *   CONFIG_VFIO_VIRQFD           : eventfd 기반 IRQ infra (virqfd.c).
 *   CONFIG_KVM                   : KVM 통합(IOMMU coherency 협상 등).
 *   CONFIG_IOMMUFD               : iommufd 통합 — 새 IOAS/HWPT 모델.
 *   CONFIG_VFIO_DEBUGFS          : 진단 노출(debugfs.c).
 *
 * 본 헤더는 각 기능이 disable 일 때 호출 측이 #ifdef 분기를 갖지 않도록
 * 헤더 자체에서 inline stub 을 제공한다 — "Kconfig disable 시 자연스럽게
 * 0 / -EOPNOTSUPP / noop" 가 되는 컴파일 패턴.
 *
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/vfio_main.c
 *      : vfio_device 라이프사이클 핵심. 본 헤더의 try_get/put_registration,
 *        vfio_df_open/close, vfio_device_fops 를 직접 구현.
 *  - drivers/vfio/group.c
 *      : CONFIG_VFIO_GROUP 가 켜졌을 때 struct vfio_group 의 모든 행위를
 *        구현. vfio_group_init/cleanup 모듈 init/exit 와 짝.
 *  - drivers/vfio/container.c
 *      : CONFIG_VFIO_CONTAINER. legacy IOMMU backend(type1/spapr) 등록·
 *        dispatch.
 *  - drivers/vfio/device_cdev.c
 *      : CONFIG_VFIO_DEVICE_CDEV. cdev_device_add 짝의 fops, bind_iommufd
 *        ioctl 분기.
 *  - drivers/vfio/iommufd.c
 *      : CONFIG_IOMMUFD. iommufd ctx 와의 IOAS attach/detach.
 *  - drivers/vfio/virqfd.c, debugfs.c
 *      : 본 헤더가 virqfd init/exit, debugfs create/remove prototype 를
 *        제공.
 *
 *  - include/linux/vfio.h
 *      : "외부 ABI" — vendor 드라이버에 노출되는 struct vfio_device,
 *        vfio_device_ops, vfio_migration_ops 등의 정의처. 본 내부 헤더와는
 *        역할이 분리.
 *
 * === 주요 함수/구조체 요약 ===
 *  - vfio_device_file: 파일 디스크립터당 컨텍스트. 권한 게이트, KVM 참조,
 *    iommufd ctx, devid 보유.
 *  - vfio_group: IOMMU group 단위 추상화. drivers/users 두 refcount 와
 *    container/iommufd 양쪽 라이프사이클 표현.
 *  - vfio_iommu_driver_ops: legacy container 의 IOMMU backend vtable.
 *    open/release, attach/detach_group, pin/unpin_pages, dma_rw 등 11종.
 *  - vfio_device_fops: vfio_device 파일의 file_operations(외부 노출).
 *  - 다수의 vfio_device_*, vfio_df_* 헬퍼: 사이클·바인딩·해제·iommu 사용
 *    여부를 표현.
 */

#include <linux/file.h>		/* [한국어] struct file 전방 선언 의존 — vfio_group_from_file 등. */
#include <linux/device.h>		/* [한국어] struct device 내장 — vfio_group.dev. */
#include <linux/cdev.h>		/* [한국어] struct cdev 내장 — vfio_group.cdev (legacy /dev/vfio/<gid>). */
#include <linux/module.h>		/* [한국어] struct module — vfio_iommu_driver_ops.owner. */
#include <linux/vfio.h>		/* [한국어] 외부 ABI 헤더 — struct vfio_device 등 본 헤더가 확장하는 모든 base 타입. */

/* [한국어] 전방 선언 — 전체 헤더 포함을 피해 빌드 시간 단축 + 순환 의존 회피. */
struct iommufd_ctx;								/* [한국어] include/linux/iommufd.h 의 본체. iommufd 비활성 시에도 포인터 사용 가능하도록 fwd. */
struct iommu_group;								/* [한국어] include/linux/iommu.h. PCIe ACS / segment 단위 분리 group. */
struct vfio_container;								/* [한국어] container.c 내부 정의 — 외부에는 file 추상으로만 노출. */

/* [한국어] open() 1회당 생성되는 vfio_device 의 fd 컨텍스트.
 * ----------------------------------------------------------
 * 한 vfio_device 에 여러 fd 가 동시에 열릴 수 있으므로, 디바이스
 * 본체와 분리된 "fd 단위 권한·바인딩 상태" 를 별도 객체로 관리한다.
 * 설정자: vfio_allocate_device_file() — fops->open 또는 cdev_open 진입.
 * 읽는 자: 거의 모든 ioctl 핸들러가 file->private_data 로 받음.
 * 동기화: 필드별 주석 참조. */
struct vfio_device_file {
	struct vfio_device *device;
	/* [한국어] 이 fd 가 가리키는 vfio_device. 라이프사이클: open 시 set,
	 * close 까지 불변. 디바이스 해제는 모든 fd 가 close 된 후에만 가능
	 * (try_get_registration refcount 보장). */

	struct vfio_group *group;
	/* [한국어] legacy group 모드일 때만 채워짐. cdev/iommufd 모드면 NULL.
	 * 라이프사이클: open 시 vfio_df_group_open 이 group ref +1 후 set.
	 *   close 에서 vfio_df_group_close 가 ref -1.
	 * 동기화: open/close 짝으로만 set/clear, 사용 시점에는 fd 가 살아 있어
	 *   추가 락 불필요. */

	u8 access_granted;
	/* [한국어] "이 fd 가 device ioctl 을 호출할 권한을 얻었는가" boolean.
	 * 0 → IOCTL 진입 전 BIND_IOMMUFD/SET_CONTAINER 등 초기화 ioctl 만 허용.
	 * 1 → 일반 디바이스 ioctl 허용.
	 * 설정자: vfio_df_open 마지막 단계에서 1 로 갱신.
	 * 읽는 자: vfio_device_fops_unl_ioctl 가 비-bind ioctl 진입 시 검사.
	 * 비고: u8 단일 변수라 atomic 연산 없이 store/load — open/close 가 동일
	 *   thread 에서 직렬 실행되는 fd lifecycle 모델을 활용. */

	u32 devid; /* only valid when iommufd is valid */
	/* [한국어] iommufd 컨텍스트가 부여한 device ID — IOAS 공간 내 유일.
	 * 설정자: vfio_df_iommufd_bind 가 iommufd_alloc_id() 결과 저장.
	 * 읽는 자: GET_INFO 이외 ioctl 들이 iommufd 명령에 인자로 사용.
	 * 값 범위: 0=invalid (iommufd 미바인드 상태). */

	spinlock_t kvm_ref_lock; /* protect kvm field */
	/* [한국어] kvm 필드 갱신 직렬화 spinlock. KVM 측이 set_kvm 을 비동기로
	 * 호출할 수 있어 디바이스 측 라이프사이클과 race — 짧은 critical section
	 * 으로 해결. */

	struct kvm *kvm;
	/* [한국어] 이 fd 가 매핑된 KVM 가상머신 핸들. NULL = 비-KVM 사용자(DPDK 등).
	 * 설정자: ioctl(VFIO_GROUP_SET_KVM) 또는 vfio_file_set_kvm.
	 * 읽는 자: vfio_device_get_kvm_safe — coherency 협상 시 KVM 참조.
	 * 동기화: kvm_ref_lock + KVM 측 refcount 양쪽 보호. */

	struct iommufd_ctx *iommufd; /* protected by struct vfio_device_set::lock */
	/* [한국어] 이 fd 가 바인딩된 iommufd 컨텍스트. legacy container 모드면 NULL.
	 * 설정자: vfio_df_iommufd_bind ioctl.
	 * 읽는 자: vfio_iommufd_compat_attach_ioas 등 IOMMU mapping 변경 경로.
	 * 동기화: 코멘트가 명시 — vfio_device_set::lock 보유 상태에서만 갱신.
	 *   디바이스 set(같은 IOMMU group) 단위 동기화 모델. */
};

/* [한국어] vfio_device 의 등록 refcount 조작 헬퍼 — 디바이스가 unregister 진입 후
 * 새 사용자가 들어오지 못하도록 차단하는 try_get + 마지막 사용자 빠지면 wakeup. */
void vfio_device_put_registration(struct vfio_device *device);				/* [한국어] refcount -1, 0 도달 시 unregister 대기자 wakeup. */
bool vfio_device_try_get_registration(struct vfio_device *device);			/* [한국어] 0→1 acquire 시도. 디바이스 dead 면 false 반환 — open 진입 차단. */
int vfio_df_open(struct vfio_device_file *df);						/* [한국어] fd 단위 device 사용 시작 — vendor open() 호출 + access_granted 갱신. */
void vfio_df_close(struct vfio_device_file *df);					/* [한국어] vendor close() 호출 + iommufd unbind + group ref drop. */
struct vfio_device_file *
vfio_allocate_device_file(struct vfio_device *device);					/* [한국어] kzalloc + spinlock init + device 백포인터 — fops->open 의 첫 단계. */

extern const struct file_operations vfio_device_fops;					/* [한국어] /dev/vfio/devices/vfio<N> 와 group fd 의 open() 후 양쪽 모두 사용하는 fops. ioctl/mmap/read/write/close 정의. */

#ifdef CONFIG_VFIO_NOIOMMU
extern bool vfio_noiommu __read_mostly;						/* [한국어] module param. true 면 IOMMU 없는 디바이스도 VFIO 로 노출 — DPDK 같은 PMD 가 root 권한으로 사용하는 위험 모드. __read_mostly = data cache 친화. */
#else
enum { vfio_noiommu = false };								/* [한국어] Kconfig disable 시 컴파일타임 false 상수 — 호출 측 분기를 dead code 로 제거. */
#endif

/* [한국어] vfio_group 의 IOMMU 백킹 분류.
 * 한 group 에 속한 모든 device 가 같은 type 을 공유한다(IOMMU group 자체가
 * "같이 격리되어야 하는 device 집합" 이므로). */
enum vfio_group_type {
	/*
	 * Physical device with IOMMU backing.
	 */
	VFIO_IOMMU,
	/* [한국어] 정상 PCIe 패스스루의 기본형 — IOMMU 가 device 를 격리하고
	 * 사용자 공간이 IOVA → HPA 매핑을 직접 프로그래밍. */

	/*
	 * Virtual device without IOMMU backing. The VFIO core fakes up an
	 * iommu_group as the iommu_group sysfs interface is part of the
	 * userspace ABI.  The user of these devices must not be able to
	 * directly trigger unmediated DMA.
	 */
	VFIO_EMULATED_IOMMU,
	/* [한국어] mdev / mediated device — vendor 드라이버가 모든 DMA 를
	 * 중개하므로 IOMMU 가 없어도 안전. sysfs ABI 호환을 위해 가짜 group
	 * 생성. */

	/*
	 * Physical device without IOMMU backing. The VFIO core fakes up an
	 * iommu_group as the iommu_group sysfs interface is part of the
	 * userspace ABI.  Users can trigger unmediated DMA by the device,
	 * usage is highly dangerous, requires an explicit opt-in and will
	 * taint the kernel.
	 */
	VFIO_NO_IOMMU,
	/* [한국어] noiommu mode — IOMMU 없는 물리 device 를 root 가 강제로
	 * 노출. unmediated DMA 가능 → kernel taint 플래그 set. 사용자가 명시적
	 * VFIO_NOIOMMU_IOMMU type SET_IOMMU 를 거쳐야 활성. */
};

#if IS_ENABLED(CONFIG_VFIO_GROUP)
/* [한국어] legacy "group" 추상화. /dev/vfio/<groupID> 캐릭터 디바이스 1개당
 * 1개씩 만들어진다. iommu_group 내 device 들이 같은 IOVA 공간을 공유하기
 * 때문에 group 단위로만 attach/detach 를 허용 — VFIO 의 격리 보증의 근간. */
struct vfio_group {
	struct device 			dev;
	/* [한국어] sysfs/cdev 등록을 위한 내장 device. dev->parent = NULL,
	 *   class = vfio_class. dev_name() 이 group ID(숫자) 로 표시됨. */

	struct cdev			cdev;
	/* [한국어] /dev/vfio/<groupID> 캐릭터 디바이스. fops = vfio_group_fops.
	 *   open() → vfio_group_fops_open. */

	/*
	 * When drivers is non-zero a driver is attached to the struct device
	 * that provided the iommu_group and thus the iommu_group is a valid
	 * pointer. When drivers is 0 the driver is being detached. Once users
	 * reaches 0 then the iommu_group is invalid.
	 */
	refcount_t			drivers;
	/* [한국어] 이 group 에 attach 된 vfio 드라이버 인스턴스 수.
	 * 설정자: vfio_device_set_group / remove_group. drivers > 0 인 동안
	 *   iommu_group 포인터가 유효(driver 가 hold).
	 * 0 도달 = 모든 device 가 detach — 후속 group 자체 free 가능. */

	unsigned int			container_users;
	/* [한국어] 이 group 이 attach 된 container 의 사용자 수. legacy 모드에서
	 *   group→container attach 후 fd open count.
	 * 동기화: group_lock 보호. */

	struct iommu_group		*iommu_group;
	/* [한국어] 커널 IOMMU subsystem 의 실제 group. drivers > 0 동안만 유효.
	 *   iommu_group_get/put refcount 와 별도 — drivers 가 holder 역할. */

	struct vfio_container		*container;
	/* [한국어] 이 group 이 attach 된 container. NULL = 미-attach.
	 *   group_lock + container 측 mutex 양쪽 보호. */

	struct list_head		device_list;
	/* [한국어] 이 group 에 등록된 vfio_device 들의 헤드. device 측은
	 *   group_next 로 연결. */

	struct mutex			device_lock;
	/* [한국어] device_list + 그 안의 device 상태 조작 직렬화. */

	struct list_head		vfio_next;
	/* [한국어] 전역 vfio_group_list (group.c 내부 정적) 노드. */

#if IS_ENABLED(CONFIG_VFIO_CONTAINER)
	struct list_head		container_next;
	/* [한국어] container 의 group_list 노드 — container.c 가 enumerate.
	 *   CONTAINER 비활성 시 필드 자체 생략(메모리 절약). */
#endif

	enum vfio_group_type		type;
	/* [한국어] IOMMU/EMULATED/NO_IOMMU 셋 중 하나. group 생성 시 결정 후 불변. */

	struct mutex			group_lock;
	/* [한국어] container/iommufd 바인딩 + cdev open count 등 group 단위 상태
	 *   직렬화 락. drivers refcount 의 0→1 / 1→0 전이도 이 락 보호. */

	struct kvm			*kvm;
	/* [한국어] 이 group 을 사용하는 KVM 가상머신. set_kvm 으로 갱신.
	 *   coherency 협상에 사용. NULL 가능. */

	struct file			*opened_file;
	/* [한국어] /dev/vfio/<id> 를 처음 연 file. 동시에 여러 open 을 막는
	 *   "single-opener" 게이트 — 두 번째 open 은 EBUSY. */

	struct iommufd_ctx		*iommufd;
	/* [한국어] group 이 iommufd 모드로 전환됐을 때의 ctx. legacy container 와
	 *   상호 배타. */

	spinlock_t			kvm_ref_lock;
	/* [한국어] kvm 필드 보호 — set_kvm/get_kvm_safe 가 짧은 atomic 영역으로 race 차단. */

	unsigned int			cdev_device_open_cnt;
	/* [한국어] cdev 모드 device fd 가 동시에 몇 개 열려 있는지. 0 → 1 전이
	 *   시점에 group attach 가 의미 가짐. */
};

/* [한국어] vfio_device 가 group 시스템과 상호작용할 때 호출하는 헬퍼들. */
int vfio_device_block_group(struct vfio_device *device);				/* [한국어] open 차단 모드 진입 — group->opened_file 설정 race 와 동기화. */
void vfio_device_unblock_group(struct vfio_device *device);				/* [한국어] block_group 짝. */
int vfio_device_set_group(struct vfio_device *device,					/* [한국어] device 등록 시 알맞은 group 에 add — 없으면 신규 group 생성. */
			  enum vfio_group_type type);
void vfio_device_remove_group(struct vfio_device *device);				/* [한국어] device unregister 시 group 에서 detach — 마지막 device 면 group 자체도 정리. */
void vfio_device_group_register(struct vfio_device *device);				/* [한국어] device sysfs 등록 후 group/device_list 에 add. */
void vfio_device_group_unregister(struct vfio_device *device);			/* [한국어] register 짝. */
int vfio_device_group_use_iommu(struct vfio_device *device);				/* [한국어] device 가 IOMMU 사용 시작 — group 단위 attach 보장. */
void vfio_device_group_unuse_iommu(struct vfio_device *device);			/* [한국어] use_iommu 짝. */
void vfio_df_group_close(struct vfio_device_file *df);				/* [한국어] fd close 시 group ref drop. */
struct vfio_group *vfio_group_from_file(struct file *file);				/* [한국어] file 이 group fd 인지 검사하고 group 반환 — KVM 등 외부가 사용. */
bool vfio_group_enforced_coherent(struct vfio_group *group);				/* [한국어] 이 group 의 모든 device 가 cache coherent IOMMU 인가 검사 — KVM 가 게스트 캐시 정책 결정에 사용. */
void vfio_group_set_kvm(struct vfio_group *group, struct kvm *kvm);			/* [한국어] kvm_ref_lock 안에서 set. */
bool vfio_device_has_container(struct vfio_device *device);				/* [한국어] legacy container 사용 여부 boolean. */
int __init vfio_group_init(void);							/* [한국어] 모듈 init 단계 — class 생성, chrdev region alloc. */
void vfio_group_cleanup(void);							/* [한국어] init 짝 — 모듈 unload 시. */

/* [한국어] device 가 NO_IOMMU type 인지 검사하는 inline 헬퍼.
 * 호출 측이 매번 group->type == VFIO_NO_IOMMU 를 쓰지 않도록 캡슐화 +
 * Kconfig 미활성 시 컴파일타임 false 로 dead-branch 화. */
static inline bool vfio_device_is_noiommu(struct vfio_device *vdev)
{
	return IS_ENABLED(CONFIG_VFIO_NOIOMMU) &&				/* [한국어] 컴파일 시점 상수 — disable 이면 short-circuit 으로 group->type 접근 자체 제거. */
	       vdev->group->type == VFIO_NO_IOMMU;
}
#else
/* [한국어] CONFIG_VFIO_GROUP 비활성 시 stub. 호출자 코드가 #ifdef 없이 빌드되도록
 * 모든 함수를 noop / -EOPNOTSUPP / NULL / true(coherent) 로 변환. */
struct vfio_group;									/* [한국어] 전방 선언만 — 어차피 사용 안 됨. */

static inline int vfio_device_block_group(struct vfio_device *device)
{
	return 0;
}

static inline void vfio_device_unblock_group(struct vfio_device *device)
{
}

static inline int vfio_device_set_group(struct vfio_device *device,
					enum vfio_group_type type)
{
	return 0;									/* [한국어] group 모드 없을 때 set 도 idempotent — cdev only 빌드. */
}

static inline void vfio_device_remove_group(struct vfio_device *device)
{
}

static inline void vfio_device_group_register(struct vfio_device *device)
{
}

static inline void vfio_device_group_unregister(struct vfio_device *device)
{
}

static inline int vfio_device_group_use_iommu(struct vfio_device *device)
{
	return -EOPNOTSUPP;								/* [한국어] group 없는 빌드에서는 IOMMU 사용 신청 불가 — cdev/iommufd 경로만. */
}

static inline void vfio_device_group_unuse_iommu(struct vfio_device *device)
{
}

static inline void vfio_df_group_close(struct vfio_device_file *df)
{
}

static inline struct vfio_group *vfio_group_from_file(struct file *file)
{
	return NULL;									/* [한국어] KVM 측 코드가 NULL 검사로 graceful 처리. */
}

static inline bool vfio_group_enforced_coherent(struct vfio_group *group)
{
	return true;									/* [한국어] group 자체가 없는 빌드에선 "최대 안전" — KVM 이 캐시 강제하지 않음. */
}

static inline void vfio_group_set_kvm(struct vfio_group *group, struct kvm *kvm)
{
}

static inline bool vfio_device_has_container(struct vfio_device *device)
{
	return false;
}

static inline int __init vfio_group_init(void)
{
	return 0;
}

static inline void vfio_group_cleanup(void)
{
}

static inline bool vfio_device_is_noiommu(struct vfio_device *vdev)
{
	return false;									/* [한국어] noiommu 의미 자체가 사라지므로 false. */
}
#endif /* CONFIG_VFIO_GROUP */

#if IS_ENABLED(CONFIG_VFIO_CONTAINER)
/**
 * struct vfio_iommu_driver_ops - VFIO IOMMU driver callbacks
 */
struct vfio_iommu_driver_ops {
	char		*name;
	/* [한국어] backend 식별자(예: "vfio-iommu-type1", "spapr-tce-iommu").
	 *   ioctl(VFIO_SET_IOMMU) 의 type 인자가 이 이름으로 매핑. */

	struct module	*owner;
	/* [한국어] backend 모듈 — try_module_get 으로 driver 사용 중 unload 방지. */

	void		*(*open)(unsigned long arg);
	/* [한국어] container 가 SET_IOMMU 받았을 때 backend 인스턴스 생성.
	 *   반환: backend-private opaque(이후 모든 콜백의 iommu_data 인자). */

	void		(*release)(void *iommu_data);
	/* [한국어] open 짝 — container 해제 시 backend 정리. */

	long		(*ioctl)(void *iommu_data, unsigned int cmd,
				 unsigned long arg);
	/* [한국어] backend-specific ioctl 디스패처. VFIO_IOMMU_MAP_DMA 등이
	 *   여기로 흘러 들어옴. */

	int		(*attach_group)(void *iommu_data,
					struct iommu_group *group,
					enum vfio_group_type);
	/* [한국어] group 의 모든 device 를 backend IOMMU domain 에 attach.
	 *   group_type 으로 IOMMU/EMU/NOIOMMU 분기. */

	void		(*detach_group)(void *iommu_data,
					struct iommu_group *group);
	/* [한국어] attach 짝. */

	int		(*pin_pages)(void *iommu_data,
				     struct iommu_group *group,
				     dma_addr_t user_iova,
				     int npage, int prot,
				     struct page **pages);
	/* [한국어] mediated 디바이스가 GUP 로 host 페이지 핀하고 IOVA 매핑 갱신.
	 *   npage 는 4KB 단위. prot = IOMMU_READ/WRITE. */

	void		(*unpin_pages)(void *iommu_data,
				       dma_addr_t user_iova, int npage);
	/* [한국어] pin_pages 짝. */

	void		(*register_device)(void *iommu_data,
					   struct vfio_device *vdev);
	void		(*unregister_device)(void *iommu_data,
					     struct vfio_device *vdev);
	/* [한국어] container 가 알고 있는 device 목록에 add/remove — type1 backend 가
	 *   migration dirty page tracking 을 위해 device 단위 관리. */

	int		(*dma_rw)(void *iommu_data, dma_addr_t user_iova,
				  void *data, size_t count, bool write);
	/* [한국어] vendor 드라이버가 게스트 IOVA 를 host kernel 메모리로 직접
	 *   읽고/쓰기 — 마이그레이션 stop_copy 데이터 전달 등. */

	struct iommu_domain *(*group_iommu_domain)(void *iommu_data,
						   struct iommu_group *group);
	/* [한국어] group 이 attached 된 IOMMU domain 반환 — KVM coherency 검사용. */
};

/* [한국어] backend 등록 list 노드. container.c 가 list_for_each 로 dispatch. */
struct vfio_iommu_driver {
	const struct vfio_iommu_driver_ops	*ops;
	struct list_head			vfio_next;
};

int vfio_register_iommu_driver(const struct vfio_iommu_driver_ops *ops);		/* [한국어] type1/spapr backend 가 자기 모듈 init 시 호출. */
void vfio_unregister_iommu_driver(const struct vfio_iommu_driver_ops *ops);

struct vfio_container *vfio_container_from_file(struct file *filep);			/* [한국어] file 이 /dev/vfio/vfio 이면 container 반환. KVM/외부 검사. */
int vfio_group_use_container(struct vfio_group *group);				/* [한국어] group 이 container 사용 시작 — container_users +1. */
void vfio_group_unuse_container(struct vfio_group *group);
int vfio_container_attach_group(struct vfio_container *container,			/* [한국어] SET_CONTAINER ioctl — group→container 결합. */
				struct vfio_group *group);
void vfio_group_detach_container(struct vfio_group *group);
void vfio_device_container_register(struct vfio_device *device);			/* [한국어] device 가 등록될 때 container 에도 알림 — DMA tracking. */
void vfio_device_container_unregister(struct vfio_device *device);
int vfio_device_container_pin_pages(struct vfio_device *device,			/* [한국어] vendor 가 mediated DMA 를 위해 호출. */
				    dma_addr_t iova, int npage,
				    int prot, struct page **pages);
void vfio_device_container_unpin_pages(struct vfio_device *device,
				       dma_addr_t iova, int npage);
int vfio_device_container_dma_rw(struct vfio_device *device,				/* [한국어] vendor 가 게스트 메모리 read/write — 마이그레이션 직렬화. */
				 dma_addr_t iova, void *data,
				 size_t len, bool write);

int __init vfio_container_init(void);							/* [한국어] container 모듈 init — misc device 등록. */
void vfio_container_cleanup(void);
#else
/* [한국어] CONFIG_VFIO_CONTAINER 비활성 — 모든 container 함수가 noop / EOPNOTSUPP / NULL. */
static inline struct vfio_container *
vfio_container_from_file(struct file *filep)
{
	return NULL;
}

static inline int vfio_group_use_container(struct vfio_group *group)
{
	return -EOPNOTSUPP;
}

static inline void vfio_group_unuse_container(struct vfio_group *group)
{
}

static inline int vfio_container_attach_group(struct vfio_container *container,
					      struct vfio_group *group)
{
	return -EOPNOTSUPP;
}

static inline void vfio_group_detach_container(struct vfio_group *group)
{
}

static inline void vfio_device_container_register(struct vfio_device *device)
{
}

static inline void vfio_device_container_unregister(struct vfio_device *device)
{
}

static inline int vfio_device_container_pin_pages(struct vfio_device *device,
						  dma_addr_t iova, int npage,
						  int prot, struct page **pages)
{
	return -EOPNOTSUPP;
}

static inline void vfio_device_container_unpin_pages(struct vfio_device *device,
						     dma_addr_t iova, int npage)
{
}

static inline int vfio_device_container_dma_rw(struct vfio_device *device,
					       dma_addr_t iova, void *data,
					       size_t len, bool write)
{
	return -EOPNOTSUPP;
}

static inline int vfio_container_init(void)
{
	return 0;
}
static inline void vfio_container_cleanup(void)
{
}
#endif

#if IS_ENABLED(CONFIG_IOMMUFD)
/* [한국어] iommufd 통합 — 새 IOAS/HWPT 기반 IOMMU 모델. */
bool vfio_iommufd_device_has_compat_ioas(struct vfio_device *vdev,			/* [한국어] iommufd ctx 가 이미 호환 IOAS 를 갖고 있는지 — vendor 가 dependent device 와 같은 공간 공유 여부 결정. */
					 struct iommufd_ctx *ictx);
int vfio_df_iommufd_bind(struct vfio_device_file *df);				/* [한국어] BIND_IOMMUFD ioctl — fd 를 iommufd ctx 에 결합 + devid 부여. */
void vfio_df_iommufd_unbind(struct vfio_device_file *df);
int vfio_iommufd_compat_attach_ioas(struct vfio_device *device,			/* [한국어] legacy ATTACH_IOAS 호환 — 신규 BIND_IOMMUFD 후 같은 동작 재현. */
				    struct iommufd_ctx *ictx);
#else
/* [한국어] iommufd disable stub — bind 시도는 EOPNOTSUPP. */
static inline bool
vfio_iommufd_device_has_compat_ioas(struct vfio_device *vdev,
				    struct iommufd_ctx *ictx)
{
	return false;
}

static inline int vfio_df_iommufd_bind(struct vfio_device_file *fd)
{
	return -EOPNOTSUPP;
}

static inline void vfio_df_iommufd_unbind(struct vfio_device_file *df)
{
}

static inline int
vfio_iommufd_compat_attach_ioas(struct vfio_device *device,
				struct iommufd_ctx *ictx)
{
	return -EOPNOTSUPP;
}
#endif

/* [한국어] iommufd 의 ATTACH_PT/DETACH_PT ioctl 진입점. CONFIG_IOMMUFD 와 무관하게
 *   prototype 만 노출(disabled 시 본문이 EOPNOTSUPP 반환). */
int vfio_df_ioctl_attach_pt(struct vfio_device_file *df,
			    struct vfio_device_attach_iommufd_pt __user *arg);
int vfio_df_ioctl_detach_pt(struct vfio_device_file *df,
			    struct vfio_device_detach_iommufd_pt __user *arg);

#if IS_ENABLED(CONFIG_VFIO_DEVICE_CDEV)
/* [한국어] 새 cdev 인터페이스 (/dev/vfio/devices/vfio<N>). group/container 우회. */
void vfio_init_device_cdev(struct vfio_device *device);				/* [한국어] device 등록 시 cdev 객체 init — fops 부착. */

/* [한국어] device 추가 헬퍼 — noiommu 와 cdev 를 한 함수로 분기.
 *   noiommu device 는 cdev 인터페이스를 지원하지 않으므로 device_add 만,
 *   그 외에는 cdev_device_add 로 한 번에 cdev + device 등록. */
static inline int vfio_device_add(struct vfio_device *device)
{
	/* cdev does not support noiommu device */
	if (vfio_device_is_noiommu(device))					/* [한국어] noiommu 는 unmediated DMA 위험 — 사용자가 cdev 로 직접 열지 못하도록 sysfs only. */
		return device_add(&device->device);
	vfio_init_device_cdev(device);						/* [한국어] cdev 구조체 초기화 — fops 부착, owner set. */
	return cdev_device_add(&device->cdev, &device->device);		/* [한국어] cdev_add + device_add atomic 결합 — sysfs 등록과 dev_t 동시. */
}

static inline void vfio_device_del(struct vfio_device *device)
{
	if (vfio_device_is_noiommu(device))
		device_del(&device->device);
	else
		cdev_device_del(&device->cdev, &device->device);
}

int vfio_device_fops_cdev_open(struct inode *inode, struct file *filep);		/* [한국어] /dev/vfio/devices/vfio<N> 의 open 진입점. */
long vfio_df_ioctl_bind_iommufd(struct vfio_device_file *df,				/* [한국어] BIND_IOMMUFD ioctl 처리 — fd 를 iommufd ctx 에 등록. */
				struct vfio_device_bind_iommufd __user *arg);
void vfio_df_unbind_iommufd(struct vfio_device_file *df);
int vfio_cdev_init(struct class *device_class);					/* [한국어] cdev 모듈 init — chrdev region alloc, class 등록. */
void vfio_cdev_cleanup(void);
#else
/* [한국어] CDEV disable stub — device_add 만, cdev open 은 0(graceful). */
static inline void vfio_init_device_cdev(struct vfio_device *device)
{
}

static inline int vfio_device_add(struct vfio_device *device)
{
	return device_add(&device->device);					/* [한국어] cdev 없는 빌드 = sysfs/group 모드만. */
}

static inline void vfio_device_del(struct vfio_device *device)
{
	device_del(&device->device);
}

static inline int vfio_device_fops_cdev_open(struct inode *inode,
					     struct file *filep)
{
	return 0;
}

static inline long vfio_df_ioctl_bind_iommufd(struct vfio_device_file *df,
					      struct vfio_device_bind_iommufd __user *arg)
{
	return -ENOTTY;									/* [한국어] cdev 미지원에서 BIND_IOMMUFD 요청 = "그런 ioctl 없음". */
}

static inline void vfio_df_unbind_iommufd(struct vfio_device_file *df)
{
}

static inline int vfio_cdev_init(struct class *device_class)
{
	return 0;
}

static inline void vfio_cdev_cleanup(void)
{
}
#endif /* CONFIG_VFIO_DEVICE_CDEV */

#if IS_ENABLED(CONFIG_VFIO_VIRQFD)
int __init vfio_virqfd_init(void);							/* [한국어] virqfd.c 의 init — cleanup workqueue 생성. */
void vfio_virqfd_exit(void);
#else
/* [한국어] virqfd 미사용 빌드(예: VFIO 가 IRQ 미지원 임베디드) — noop stub. */
static inline int __init vfio_virqfd_init(void)
{
	return 0;
}
static inline void vfio_virqfd_exit(void)
{
}
#endif

#if IS_ENABLED(CONFIG_KVM)
/* [한국어] KVM 통합 — VFIO 디바이스의 IOMMU coherency 정보를 KVM 에 전달.
 *   _safe 변형: KVM module reference 까지 안전하게 acquire(짧은 lockless 패스). */
void vfio_device_get_kvm_safe(struct vfio_device *device, struct kvm *kvm);
void vfio_device_put_kvm(struct vfio_device *device);
#else
static inline void vfio_device_get_kvm_safe(struct vfio_device *device,
					    struct kvm *kvm)
{
}

static inline void vfio_device_put_kvm(struct vfio_device *device)
{
}
#endif

#ifdef CONFIG_VFIO_DEBUGFS
/* [한국어] debugfs.c 가 구현하는 4개 진입점 — vfio_main.c 에서 호출. */
void vfio_debugfs_create_root(void);
void vfio_debugfs_remove_root(void);

void vfio_device_debugfs_init(struct vfio_device *vdev);
void vfio_device_debugfs_exit(struct vfio_device *vdev);
#else
static inline void vfio_debugfs_create_root(void) { }
static inline void vfio_debugfs_remove_root(void) { }

static inline void vfio_device_debugfs_init(struct vfio_device *vdev) { }
static inline void vfio_device_debugfs_exit(struct vfio_device *vdev) { }
#endif /* CONFIG_VFIO_DEBUGFS */

#endif
