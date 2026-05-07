// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021-2022, NVIDIA CORPORATION & AFFILIATES
 */

/*
 * [한국어 설명] VFIO ↔ iommufd 통합 (drivers/vfio/iommufd.c)
 *
 * === 파일의 역할 ===
 * 현대 VFIO 의 IOMMU 인터페이스인 "iommufd" 와 VFIO 코어를 연결한다.
 * 과거에는 사용자 공간이 /dev/vfio/vfio (container) 를 열고
 * VFIO_SET_IOMMU + VFIO_IOMMU_MAP_DMA ioctl 로 IOVA 매핑을 직접
 * 프로그래밍했지만, iommufd 는 그것을 별도 캐릭터 디바이스
 * (/dev/iommu) 로 분리하고 IOAS(IO Address Space)/HWPT(Hardware Page
 * Table) 같은 1급 객체로 표현한다. VFIO device 는 "어느 iommufd ctx
 * 에 binding" 되어 있는지를 가리키고, 그 ctx 가 만든 IOAS 에
 * "attach" 한다.
 *
 * 본 파일은 두 종류의 vfio_device 가 iommufd 와 어떻게 결합하는지
 * 표준 콜백 세트로 제공한다:
 *
 *  (1) **physical** 디바이스 (vfio-pci-core, fsl-mc, cdx 등 실제 H/W 패스스루)
 *       - iommufd_device_bind() 로 H/W IOMMU domain 에 바로 attach.
 *       - PASID 다중 어드레스 공간도 지원(SR-IOV/SIOV 시나리오).
 *  (2) **emulated** 디바이스 (mdev, vfio-ap 등 vendor 가 DMA 를 중개)
 *       - iommufd_access_create() 로 "데이터 평면 access" 핸들 생성.
 *       - vendor 가 vfio_pin_pages()/vfio_dma_rw() 로 게스트 메모리 접근.
 *       - 사용자가 IOAS unmap 하면 vfio_emulated_unmap 콜백으로 invalidate.
 *
 * 추가로 BIND_IOMMUFD ioctl 진입점(vfio_df_iommufd_bind/unbind), 그리고
 * legacy "compat" 경로(VFIO_GROUP_SET_CONTAINER + iommufd ctx 동작)를
 * 위한 호환 함수(vfio_iommufd_compat_attach_ioas) 도 포함한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *  사용자 공간                                    Kernel
 *  -----------                                    ------
 *  open(/dev/iommu)         ─────────────────►   iommufd_ctx 생성
 *  open(/dev/vfio/devices/vfio<N>)               vfio_device_file df 생성
 *  ioctl(VFIO_DEVICE_BIND_IOMMUFD, fd=iommufd) ► vfio_df_iommufd_bind
 *                                                 → vdev->ops->bind_iommufd
 *                                                   (이 파일의 *_physical_bind /
 *                                                    *_emulated_bind 가 디폴트)
 *  ioctl(VFIO_DEVICE_ATTACH_IOMMUFD_PT, ioas)  ► vfio_df_ioctl_attach_pt
 *                                                 → vdev->ops->attach_ioas
 *                                                   (*_physical_attach_ioas /
 *                                                    *_emulated_attach_ioas)
 *  ioctl(IOMMUFD_IOAS_MAP, ...)                ► iommufd 가 IOMMU domain 에
 *                                                 page-table 매핑 추가
 *  DMA on hardware  ──────────────────────────►  H/W IOMMU walks
 *
 * vendor 드라이버는 위 콜백 세트를 default 로 받아 자기 vfio_device_ops
 * 에 부착하기만 하면 되고, 특이한 케이스(e.g. PASID 자체 관리) 는
 * 부분적으로만 override 한다.
 *
 * === 타 모듈과의 연결 ===
 *  - drivers/iommu/iommufd/* (별도 IOMMUFD 모듈)
 *      : iommufd_device_bind/_attach/_detach/_unbind, iommufd_access_*,
 *        iommufd_vfio_compat_ioas_get_id 등 모든 핵심 API 의 구현처.
 *        본 파일은 MODULE_IMPORT_NS("IOMMUFD"/"IOMMUFD_VFIO") 로 그
 *        export 네임스페이스에서 가져옴 — Linux 커널의 "심볼 네임스페이스"
 *        강제로 의도하지 않은 사용자 차단.
 *  - drivers/vfio/device_cdev.c
 *      : BIND_IOMMUFD ioctl 진입점이 본 파일의 vfio_df_iommufd_bind 호출.
 *  - drivers/vfio/group.c
 *      : SET_CONTAINER ioctl 이 iommufd ctx 인 경우 compat 경로 사용
 *        (vfio_iommufd_compat_attach_ioas).
 *  - drivers/vfio/pci/vfio_pci_core.c, mdev/* 등
 *      : 기본 ops 로 _physical_*/_emulated_* 콜백을 채택. 자체 변형
 *        구현 시 본 파일 함수를 helper 로 호출하기도 함.
 *  - include/linux/vfio.h
 *      : vfio_device 구조체의 iommufd_device, iommufd_access,
 *        iommufd_attached, pasids 필드 정의처.
 *
 * === 주요 함수/구조체 요약 ===
 *  - vfio_iommufd_device_has_compat_ioas: 주어진 ictx 에 호환 IOAS 존재 여부.
 *  - vfio_df_iommufd_bind/unbind: BIND_IOMMUFD ioctl 핸들러의 디스패처.
 *  - vfio_iommufd_compat_attach_ioas: legacy SET_CONTAINER(iommufd ctx) 호환 경로.
 *  - vfio_iommufd_device_ictx / _id / _get_dev_id: vendor 가 device 의
 *    iommufd 컨텍스트와 ID 를 조회.
 *  - vfio_iommufd_physical_*: vfio-pci 등 H/W 디바이스용 vtable 6개.
 *      bind/unbind/attach_ioas/detach_ioas/pasid_attach_ioas/pasid_detach_ioas.
 *  - vfio_iommufd_emulated_*: mdev 등 software 디바이스용 vtable 4개.
 *      bind/unbind/attach_ioas/detach_ioas + vfio_emulated_unmap 콜백.
 *  - vfio_user_ops (정적): emulated 디바이스가 iommufd 에 등록하는 access vtable.
 */

#include <linux/vfio.h>		/* [한국어] struct vfio_device, vfio_device_ops 정의처. */
#include <linux/iommufd.h>		/* [한국어] iommufd_ctx, iommufd_device, iommufd_access, *_bind/_attach/_detach/_unbind 모든 API. */

#include "vfio.h"			/* [한국어] struct vfio_device_file, vfio_df_iommufd_*, vfio_device_is_noiommu prototype. */

/* [한국어] 심볼 네임스페이스 import — iommufd 가 EXPORT_SYMBOL_NS_GPL 로
 * "IOMMUFD" 네임스페이스에 export 하는 함수들을 사용하기 위함.
 * 의도: 의도치 않은 외부 모듈이 iommufd 내부 API 를 끌어가지 못하게
 * 컴파일타임 강제. */
MODULE_IMPORT_NS("IOMMUFD");						/* [한국어] iommufd_device_bind/_attach/_detach/_unbind, iommufd_access_create/_destroy/_attach/_detach 등 일반 API. */
MODULE_IMPORT_NS("IOMMUFD_VFIO");						/* [한국어] iommufd_vfio_compat_ioas_get_id, iommufd_ctx_has_group 등 VFIO compat 전용 함수. */

/*
 * [한국어]
 * vfio_iommufd_device_has_compat_ioas - ictx 에 legacy 호환 IOAS 가 있는지
 *
 * @vdev: 검사 대상 디바이스(현재 미사용, 시그니처 호환).
 * @ictx: iommufd 컨텍스트.
 * @return: true = 호환 IOAS 존재, false = 없음.
 *
 * legacy /dev/vfio/vfio container 가 iommufd 로 마이그레이션될 때, 이미
 * 다른 group 이 같은 IOAS 를 share 하는지 검사한다. iommufd 가 ctx 마다
 * "compat IOAS" 1개를 자동 생성하므로(legacy MAP_DMA ioctl 처리용),
 * 그 ID 를 조회만 하면 된다.
 */
bool vfio_iommufd_device_has_compat_ioas(struct vfio_device *vdev,
					 struct iommufd_ctx *ictx)
{
	u32 ioas_id;								/* [한국어] iommufd 가 채울 출력 변수. 사용은 안 하고 존재 여부만 검사. */

	return !iommufd_vfio_compat_ioas_get_id(ictx, &ioas_id);		/* [한국어] 0 반환 = 존재. 음수 errno = 없음 → "!" 로 boolean 변환. */
}

/*
 * [한국어]
 * vfio_df_iommufd_bind - BIND_IOMMUFD ioctl 의 코어 디스패처
 *
 * @df: vfio_device_file. df->iommufd 에는 호출자가 ictx_acquire 로 잡은
 *      iommufd 컨텍스트가 이미 들어 있어야 함.
 * @return: vendor bind_iommufd 콜백의 errno (성공 0).
 *
 * 동작:
 *  1) noiommu 모드 short-circuit: IOMMU 자체가 없는 디바이스는 iommufd 에
 *     bind 할 의미가 없으므로 0 반환. 그래도 device 를 열 수 있게 허용
 *     (사용자가 access_granted 절차는 정상적으로 통과).
 *  2) vendor ops->bind_iommufd 위임. 디바이스 종류(physical/emulated/...)
 *     에 따라 _physical_bind 또는 _emulated_bind 가 default. devid 는
 *     해당 콜백이 채움.
 *
 * 동기화: vdev->dev_set->lock 보유 필수 — 같은 IOMMU group 의 다른
 *   device 와 bind/unbind race 차단. lockdep_assert_held 로 명시.
 */
int vfio_df_iommufd_bind(struct vfio_device_file *df)
{
	struct vfio_device *vdev = df->device;					/* [한국어] fd → device 역참조. */
	struct iommufd_ctx *ictx = df->iommufd;				/* [한국어] 호출자가 fd_acquire 로 미리 채워둔 컨텍스트. */

	lockdep_assert_held(&vdev->dev_set->lock);				/* [한국어] device_set 단위 직렬화 보장 — 디버그 빌드에서 위반 시 BUG. */

	/* Returns 0 to permit device opening under noiommu mode */
	if (vfio_device_is_noiommu(vdev))					/* [한국어] noiommu 는 IOMMU domain 자체가 없어 iommufd bind 불가 — 그러나 사용자 공간에서 실패시키지 않고 graceful 통과. */
		return 0;

	return vdev->ops->bind_iommufd(vdev, ictx, &df->devid);		/* [한국어] vendor 콜백 dispatch — devid 출력은 fd 에 보관(이후 ioctl 인자로 사용). */
}

/*
 * [한국어]
 * vfio_iommufd_compat_attach_ioas - legacy compat 경로의 IOAS attach
 *
 * @vdev: device.
 * @ictx: iommufd 컨텍스트(legacy SET_CONTAINER 가 매핑한).
 * @return: 0 성공, errno 실패.
 *
 * legacy 사용자 공간이 /dev/vfio/vfio 대신 /dev/iommu fd 를
 * VFIO_GROUP_SET_CONTAINER 에 넘기면 본 함수가 호출되어 compat IOAS 에
 * 자동 attach 한다. 새 ATTACH_IOMMUFD_PT ioctl 과 달리 pt_id 를 호출자에게
 * 알려줄 방법이 없다(legacy ABI).
 *
 * 동작:
 *  1) noiommu 면 noop(0 반환).
 *  2) compat IOAS ID 조회.
 *  3) vendor ops->attach_ioas 호출 — physical/emulated 분기 자동.
 */
int vfio_iommufd_compat_attach_ioas(struct vfio_device *vdev,
				    struct iommufd_ctx *ictx)
{
	u32 ioas_id;								/* [한국어] compat IOAS ID. ictx 당 1개 자동 존재. */
	int ret;

	lockdep_assert_held(&vdev->dev_set->lock);				/* [한국어] attach_ioas 도 dev_set 직렬화 필요. */

	/* compat noiommu does not need to do ioas attach */
	if (vfio_device_is_noiommu(vdev))					/* [한국어] noiommu = IOMMU domain 부재 → attach 불필요. */
		return 0;

	ret = iommufd_vfio_compat_ioas_get_id(ictx, &ioas_id);		/* [한국어] ictx 의 자동 compat IOAS ID 가져오기 — legacy MAP_DMA 가 사용. */
	if (ret)
		return ret;

	/* The legacy path has no way to return the selected pt_id */
	return vdev->ops->attach_ioas(vdev, &ioas_id);			/* [한국어] new path 와 동일 콜백 사용 — 다만 pt_id 가 사용자 공간에 노출 안 됨. */
}

/*
 * [한국어]
 * vfio_df_iommufd_unbind - BIND_IOMMUFD 짝의 unbind. fd close / explicit unbind 시.
 *
 * 동작: noiommu 는 noop, vendor unbind_iommufd 가 등록돼 있으면 호출.
 *   physical/emulated default ops 는 unbind 콜백을 가지므로 거의 항상 호출됨.
 */
void vfio_df_iommufd_unbind(struct vfio_device_file *df)
{
	struct vfio_device *vdev = df->device;

	lockdep_assert_held(&vdev->dev_set->lock);				/* [한국어] bind/unbind 짝 직렬화. */

	if (vfio_device_is_noiommu(vdev))
		return;

	if (vdev->ops->unbind_iommufd)						/* [한국어] vendor 가 자체 라이프사이클을 위해 unbind 콜백을 옵션으로 둘 수 있음(현재는 default 가 항상 채움). */
		vdev->ops->unbind_iommufd(vdev);
}

/*
 * [한국어]
 * vfio_iommufd_device_ictx - device 가 현재 바인딩된 iommufd_ctx 반환
 *
 * @vdev: device.
 * @return: iommufd_ctx 또는 NULL(미-bind / emulated).
 *
 * vendor 드라이버가 자기 device 의 iommufd 컨텍스트가 필요할 때 사용.
 * 예: mlx5 vfio_pci 가 같은 ictx 에 binding 된 다른 디바이스(보조 PF)
 * 와 같은 IOAS 공유 여부 결정.
 *
 * physical 디바이스만 iommufd_device 를 가짐(emulated 는 access). 따라서
 * emulated 는 NULL 반환 — 호출자가 이 한계를 알고 사용해야 함.
 */
struct iommufd_ctx *vfio_iommufd_device_ictx(struct vfio_device *vdev)
{
	if (vdev->iommufd_device)						/* [한국어] physical bind 후 set 됨. */
		return iommufd_device_to_ictx(vdev->iommufd_device);		/* [한국어] iommufd 가 device → ctx 역방향 lookup 제공. */
	return NULL;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_device_ictx);

/* [한국어] device 의 iommufd 측 ID 조회 — bind 시 부여된 unique ID.
 *   vfio_iommufd_get_dev_id 의 helper. */
static int vfio_iommufd_device_id(struct vfio_device *vdev)
{
	if (vdev->iommufd_device)
		return iommufd_device_to_id(vdev->iommufd_device);
	return -EINVAL;
}

/*
 * Return devid for a device.
 *  valid ID for the device that is owned by the ictx
 *  -ENOENT = device is owned but there is no ID
 *  -ENODEV or other error = device is not owned
 */
/*
 * [한국어]
 * vfio_iommufd_get_dev_id - device 와 임의 ictx 사이의 ID 관계 조회
 *
 * @vdev: device.
 * @ictx: 비교할 iommufd 컨텍스트.
 * @return:
 *   ≥ 0   : device 가 이 ictx 에 bind 되어 있고 ID 부여됨.
 *  -ENOENT: device 의 IOMMU group 이 이 ictx 에 owner 등록은 됐지만 ID 없음
 *           (보조 디바이스). vendor 가 multi-device share 시 이 케이스 사용.
 *  -ENODEV: device 가 이 ictx 의 owner 가 아님. vendor 측 거절.
 *
 * 사용 예: vfio-pci-core 의 mlx5/main 같은 vendor 가 "추가 PF 가 같은
 * IOMMU group 인지" 검사할 때.
 */
int vfio_iommufd_get_dev_id(struct vfio_device *vdev, struct iommufd_ctx *ictx)
{
	struct iommu_group *group;
	int devid;

	if (vfio_iommufd_device_ictx(vdev) == ictx)				/* [한국어] 가장 빠른 case — 같은 ctx 면 ID 바로 반환. */
		return vfio_iommufd_device_id(vdev);

	group = iommu_group_get(vdev->dev);					/* [한국어] device → IOMMU group ref +1(반드시 put). */
	if (!group)								/* [한국어] IOMMU 미연결 device(noiommu) — 검사 의미 없음. */
		return -ENODEV;

	if (iommufd_ctx_has_group(ictx, group))				/* [한국어] ctx 가 이 group 의 owner 인가? — 같은 group 의 다른 device 가 이미 bind 했으면 true. */
		devid = -ENOENT;						/* [한국어] owner 이지만 vdev 자체에는 ID 없음(보조 디바이스 의미). */
	else
		devid = -ENODEV;						/* [한국어] owner 도 아님 — 사용자 공간 잘못 사용. */

	iommu_group_put(group);						/* [한국어] iommu_group_get 짝. */

	return devid;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_get_dev_id);

/* ============================================================
 * [한국어] === Physical 디바이스용 표준 ops 묶음 ===
 *
 * vfio-pci-core, vfio-fsl-mc, vfio-cdx 등 실제 H/W 패스스루 드라이버는
 * 자기 vfio_device_ops 의 bind_iommufd/attach_ioas/... 슬롯에 아래
 * 6 함수를 그대로 등록한다. 호출자는 vfio_register_group_dev() 로 등록.
 * iommufd_device 자체가 H/W IOMMU domain 의 1급 객체로, attach 는
 * 실제 page-table 을 바꾼다(또는 단계 IOMMU 의 pgtable replace 수행).
 * ============================================================
 */

/*
 * The physical standard ops mean that the iommufd_device is bound to the
 * physical device vdev->dev that was provided to vfio_init_group_dev(). Drivers
 * using this ops set should call vfio_register_group_dev()
 */
/*
 * [한국어]
 * vfio_iommufd_physical_bind - physical device 를 iommufd 에 bind
 *
 * @vdev:           대상 device.
 * @ictx:           iommufd 컨텍스트.
 * @out_device_id:  iommufd 가 부여한 unique ID 출력.
 * @return: 0 성공, errno.
 *
 * 동작:
 *  1) iommufd_device_bind: ictx 가 vdev->dev 의 iommu_group ownership 을
 *     획득하고, internal device 객체 + ID 발급.
 *  2) vdev->iommufd_device 보관 — 이후 attach/detach 에 사용.
 *  3) vdev->pasids ida 초기화 — PASID 다중 attach 추적.
 */
int vfio_iommufd_physical_bind(struct vfio_device *vdev,
			       struct iommufd_ctx *ictx, u32 *out_device_id)
{
	struct iommufd_device *idev;

	idev = iommufd_device_bind(ictx, vdev->dev, out_device_id);		/* [한국어] iommufd 측 메인 entry — ownership 검증, ID 발급, IOMMU sysfs reservation. */
	if (IS_ERR(idev))
		return PTR_ERR(idev);						/* [한국어] -ENOMEM/-EBUSY/-EPERM 등 그대로 전달. */
	vdev->iommufd_device = idev;						/* [한국어] 후속 attach/detach/unbind 가 사용. */
	ida_init(&vdev->pasids);						/* [한국어] PASID ID allocator 초기화 — 빈 set. */
	return 0;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_physical_bind);

/*
 * [한국어]
 * vfio_iommufd_physical_unbind - bind 짝
 *
 * 동작:
 *  1) 잔여 PASID 모두 detach + ID 해제 — 사용자 공간이 detach 누락한 경우 안전망.
 *  2) IOMMU_NO_PASID(=기본 컨텍스트) 가 attach 되어 있으면 detach.
 *  3) iommufd_device_unbind 로 ownership/ID 반환.
 *  4) vdev->iommufd_device = NULL.
 */
void vfio_iommufd_physical_unbind(struct vfio_device *vdev)
{
	int pasid;

	lockdep_assert_held(&vdev->dev_set->lock);

	while ((pasid = ida_find_first(&vdev->pasids)) >= 0) {		/* [한국어] 사용자가 명시적 detach 안 했어도 무조건 정리 — 누수 방지. */
		iommufd_device_detach(vdev->iommufd_device, pasid);
		ida_free(&vdev->pasids, pasid);
	}

	if (vdev->iommufd_attached) {						/* [한국어] 기본 컨텍스트 attach 상태면 detach. */
		iommufd_device_detach(vdev->iommufd_device, IOMMU_NO_PASID);	/* [한국어] PASID 0 = 기본(레거시 single addr space). */
		vdev->iommufd_attached = false;
	}
	iommufd_device_unbind(vdev->iommufd_device);				/* [한국어] ownership release + ID free. */
	vdev->iommufd_device = NULL;						/* [한국어] 재-bind 가능 상태로 reset. */
}
EXPORT_SYMBOL_GPL(vfio_iommufd_physical_unbind);

/*
 * [한국어]
 * vfio_iommufd_physical_attach_ioas - 기본 컨텍스트(NO_PASID) 를 IOAS 에 attach
 *
 * @vdev:   device.
 * @pt_id:  IN/OUT — 사용자가 지정한 IOAS/HWPT id, attach 후 실제 부착된 id.
 *
 * 처음 attach 와 replace(이미 attach 된 상태에서 다른 IOAS 로 변경) 두
 * 케이스를 모두 처리한다. replace 는 기존 IOMMU mapping 을 일관되게
 * 새 page-table 로 swap — DMA 중에도 안전(iommufd 가 race 차단).
 */
int vfio_iommufd_physical_attach_ioas(struct vfio_device *vdev, u32 *pt_id)
{
	int rc;

	lockdep_assert_held(&vdev->dev_set->lock);

	if (WARN_ON(!vdev->iommufd_device))					/* [한국어] bind 없이 attach = 사용자 공간 잘못. */
		return -EINVAL;

	if (vdev->iommufd_attached)
		rc = iommufd_device_replace(vdev->iommufd_device,		/* [한국어] 이미 attach 됐으면 swap — page-table 무중단 교체. */
					    IOMMU_NO_PASID, pt_id);
	else
		rc = iommufd_device_attach(vdev->iommufd_device,		/* [한국어] 첫 attach — domain 결합 + DMA enable. */
					   IOMMU_NO_PASID, pt_id);
	if (rc)
		return rc;
	vdev->iommufd_attached = true;
	return 0;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_physical_attach_ioas);

/*
 * [한국어]
 * vfio_iommufd_physical_detach_ioas - attach 짝
 *
 * 사용자 공간이 명시적으로 DETACH 호출 시. unbind 와 달리 device 는
 * 살려두고 IOAS 만 분리 — 이후 다른 IOAS 로 attach 가능.
 */
void vfio_iommufd_physical_detach_ioas(struct vfio_device *vdev)
{
	lockdep_assert_held(&vdev->dev_set->lock);

	if (WARN_ON(!vdev->iommufd_device) || !vdev->iommufd_attached)	/* [한국어] 잘못된 호출 보호. */
		return;

	iommufd_device_detach(vdev->iommufd_device, IOMMU_NO_PASID);
	vdev->iommufd_attached = false;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_physical_detach_ioas);

/*
 * [한국어]
 * vfio_iommufd_physical_pasid_attach_ioas - PASID 별 IOAS attach
 *
 * @vdev:   device.
 * @pasid:  Process Address Space ID — 같은 device 가 다중 어드레스 공간을
 *          가질 때 식별자(SVM/SIOV).
 * @pt_id:  IN/OUT IOAS/HWPT id.
 *
 * 동작:
 *  1) ida 에 이미 등록된 pasid 면 replace.
 *  2) 새 pasid 면 ida 에 alloc 후 attach. attach 실패 시 ida 롤백.
 *
 * PASID 는 같은 device 의 분리된 IOAS 공간을 가능케 함 — VM 안에서
 * 게스트 user-space 가 device 와 직접 IOMMU 공간 공유(SVM, Shared
 * Virtual Memory) 하는 시나리오의 기반.
 */
int vfio_iommufd_physical_pasid_attach_ioas(struct vfio_device *vdev,
					    u32 pasid, u32 *pt_id)
{
	int rc;

	lockdep_assert_held(&vdev->dev_set->lock);

	if (WARN_ON(!vdev->iommufd_device))
		return -EINVAL;

	if (ida_exists(&vdev->pasids, pasid))					/* [한국어] 같은 PASID 재-attach = replace 의미. */
		return iommufd_device_replace(vdev->iommufd_device,
					      pasid, pt_id);

	rc = ida_alloc_range(&vdev->pasids, pasid, pasid, GFP_KERNEL);	/* [한국어] PASID 점유 — 동일 ID 재할당 차단. */
	if (rc < 0)
		return rc;

	rc = iommufd_device_attach(vdev->iommufd_device, pasid, pt_id);	/* [한국어] 실제 IOMMU domain 에 PASID 별 page-table 부착. */
	if (rc)
		ida_free(&vdev->pasids, pasid);				/* [한국어] attach 실패 시 ida 롤백 — leak 방지. */

	return rc;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_physical_pasid_attach_ioas);

/*
 * [한국어]
 * vfio_iommufd_physical_pasid_detach_ioas - PASID 별 detach
 */
void vfio_iommufd_physical_pasid_detach_ioas(struct vfio_device *vdev,
					     u32 pasid)
{
	lockdep_assert_held(&vdev->dev_set->lock);

	if (WARN_ON(!vdev->iommufd_device))
		return;

	if (!ida_exists(&vdev->pasids, pasid))					/* [한국어] 등록 안 된 pasid 는 noop — graceful. */
		return;

	iommufd_device_detach(vdev->iommufd_device, pasid);
	ida_free(&vdev->pasids, pasid);
}
EXPORT_SYMBOL_GPL(vfio_iommufd_physical_pasid_detach_ioas);

/* ============================================================
 * [한국어] === Emulated 디바이스용 표준 ops 묶음 ===
 *
 * mdev / vfio-ap / vfio-ccw 등 vendor 가 H/W DMA 를 중개하는 모드.
 * H/W 의 IOMMU 와 직접 연결되지 않고 iommufd_access(소프트웨어 access
 * 핸들) 를 통해 게스트 IOVA 에 대한 read/write 권한을 얻는다.
 * vendor 는 vfio_pin_pages()/vfio_dma_rw() 헬퍼로 페이지 핀+읽기/쓰기.
 * 사용자가 IOAS 에서 page 를 unmap 하면 vfio_emulated_unmap 콜백으로
 * vendor 에 invalidate 알림 — vendor 가 진행 중 DMA 에 대한 graceful
 * 정리(우리 측 페이지 캐시 invalidate, in-flight wait) 수행.
 * ============================================================
 */

/*
 * The emulated standard ops mean that vfio_device is going to use the
 * "mdev path" and will call vfio_pin_pages()/vfio_dma_rw(). Drivers using this
 * ops set should call vfio_register_emulated_iommu_dev(). Drivers that do
 * not call vfio_pin_pages()/vfio_dma_rw() have no need to provide dma_unmap.
 */

/*
 * [한국어]
 * vfio_emulated_unmap - iommufd_access 가 unmap 알림으로 호출하는 콜백
 *
 * @data:   access_create 시 우리가 등록한 컨텍스트(여기선 vfio_device).
 * @iova:   언맵된 영역 시작.
 * @length: 길이(바이트).
 *
 * 사용자가 IOMMUFD_IOAS_UNMAP 등으로 IOVA range 를 풀면 본 콜백이
 * 동기적으로 호출된다. vendor 가 dma_unmap 콜백을 등록했으면 위임 —
 * vendor 가 in-flight DMA 정리 + 자체 metadata 무효화. dma_unmap 미등록
 * vendor(vfio_pin_pages/vfio_dma_rw 미사용) 에는 noop 안전.
 */
static void vfio_emulated_unmap(void *data, unsigned long iova,
				unsigned long length)
{
	struct vfio_device *vdev = data;					/* [한국어] access_create 인자 echo. */

	if (vdev->ops->dma_unmap)						/* [한국어] vendor 가 게스트 메모리 핀을 안 쓰면 NULL — 빠른 skip. */
		vdev->ops->dma_unmap(vdev, iova, length);
}

/* [한국어] emulated device 가 iommufd 에 등록하는 access vtable.
 *  - needs_pin_pages = 1: vendor 가 vfio_pin_pages() 로 게스트 페이지를
 *    핀할 것이므로 iommufd 가 그 라이프사이클을 추적하라는 hint.
 *  - unmap: 위 vfio_emulated_unmap 으로 invalidate 콜백. */
static const struct iommufd_access_ops vfio_user_ops = {
	.needs_pin_pages = 1,							/* [한국어] PIN 모드 — DMA pinned page 회계 활성. */
	.unmap = vfio_emulated_unmap,
};

/*
 * [한국어]
 * vfio_iommufd_emulated_bind - emulated device 의 iommufd bind
 *
 * physical 과 달리 iommufd_access 를 생성. access 는 IOMMU domain 에
 * 직접 영향 주지 않고, 그저 "이 ctx 의 IOVA 공간을 read/write 할
 * 권한을 받은 software 사용자" 를 표현.
 */
int vfio_iommufd_emulated_bind(struct vfio_device *vdev,
			       struct iommufd_ctx *ictx, u32 *out_device_id)
{
	struct iommufd_access *user;

	lockdep_assert_held(&vdev->dev_set->lock);

	user = iommufd_access_create(ictx, &vfio_user_ops, vdev, out_device_id); /* [한국어] vfio_user_ops 등록 + ID 발급 + ictx ownership. */
	if (IS_ERR(user))
		return PTR_ERR(user);
	vdev->iommufd_access = user;						/* [한국어] 이후 attach/detach/unmap 콜백 contextion. */
	return 0;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_emulated_bind);

/*
 * [한국어]
 * vfio_iommufd_emulated_unbind - emulated bind 짝
 *
 * access destroy 시 자동으로 진행 중인 unmap 콜백 완료 대기 — 안전한
 * vendor 측 cleanup 가능.
 */
void vfio_iommufd_emulated_unbind(struct vfio_device *vdev)
{
	lockdep_assert_held(&vdev->dev_set->lock);

	if (vdev->iommufd_access) {						/* [한국어] 멱등 — 이미 풀린 경우 noop. */
		iommufd_access_destroy(vdev->iommufd_access);
		vdev->iommufd_attached = false;				/* [한국어] attach 상태도 invalidate. */
		vdev->iommufd_access = NULL;
	}
}
EXPORT_SYMBOL_GPL(vfio_iommufd_emulated_unbind);

/*
 * [한국어]
 * vfio_iommufd_emulated_attach_ioas - access 를 IOAS 에 attach (또는 replace)
 *
 * physical 의 attach 는 H/W IOMMU page-table 변경이지만, 본 함수는
 * iommufd 측 권한 매핑만 갱신 — H/W 와 무관. vendor 가 vfio_pin_pages()
 * 호출 시 어떤 IOAS 에서 가져올지 지정.
 */
int vfio_iommufd_emulated_attach_ioas(struct vfio_device *vdev, u32 *pt_id)
{
	int rc;

	lockdep_assert_held(&vdev->dev_set->lock);

	if (vdev->iommufd_attached)
		rc = iommufd_access_replace(vdev->iommufd_access, *pt_id);	/* [한국어] software replace — 진행 중 pin 의 IOAS 변경. */
	else
		rc = iommufd_access_attach(vdev->iommufd_access, *pt_id);	/* [한국어] 첫 attach. */
	if (rc)
		return rc;
	vdev->iommufd_attached = true;
	return 0;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_emulated_attach_ioas);

/*
 * [한국어]
 * vfio_iommufd_emulated_detach_ioas - emulated detach
 */
void vfio_iommufd_emulated_detach_ioas(struct vfio_device *vdev)
{
	lockdep_assert_held(&vdev->dev_set->lock);

	if (WARN_ON(!vdev->iommufd_access) ||
	    !vdev->iommufd_attached)
		return;

	iommufd_access_detach(vdev->iommufd_access);
	vdev->iommufd_attached = false;
}
EXPORT_SYMBOL_GPL(vfio_iommufd_emulated_detach_ioas);
