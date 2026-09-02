// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO core
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */
/*
 * [한국어 설명] VFIO 1세대 group 계층 — /dev/vfio/<groupID> 의 전부 (drivers/vfio/group.c)
 *
 * 이 블록 안에서 다른 파일을 가리킬 때만 줄 번호를 적었다. 이 파일 자신은 주석이
 * 붙으며 줄 번호가 계속 바뀌므로 함수 이름으로만 가리킨다. container.c 를 가리키는
 * 줄 번호는 원본 스냅숏(1f0e418bb6) 기준이며, vfio_iommu_type1.c 의 기존 주석이
 * 쓰는 기준과 같다.
 *
 * === 파일의 역할 ===
 * VFIO 의 세 층(device / group / container) 가운데 **가운데 층인 group 을 통째로**
 * 구현한다. group 은 "IOMMU 가 서로 격리할 수 있는 가장 작은 디바이스 묶음" 이며,
 * VFIO 가 스스로 정하는 것이 아니라 IOMMU 드라이버가 PCIe ACS 와 버스 토폴로지를
 * 보고 미리 만들어 둔 iommu_group 을 그대로 받아들인 것이다. 이 파일이 맡는 일은
 * 다섯 가지다. (1) **group 객체의 생성과 소멸** — vfio_group_alloc, vfio_create_group,
 * vfio_group_find_or_alloc, vfio_device_remove_group 이 iommu_group 하나당 vfio_group
 * 하나라는 대응을 drivers refcount 로 유지한다. (2) **캐릭터 디바이스
 * /dev/vfio/<groupID>** — vfio_group_fops 와 그 open/release/ioctl. (3) **네 가지 GROUP 계열
 * ioctl** — SET_CONTAINER, UNSET_CONTAINER, GET_STATUS, GET_DEVICE_FD.
 * (4) **1세대 경로의 device fd 열기와 닫기** — vfio_device_open_file,
 * vfio_df_group_open, vfio_df_group_close 가 vfio_main.c 의 공통 절차를 감싸
 * group 고유의 일(KVM 참조, iommufd 호환 IOAS, 결합 확인)을 덧붙인다.
 * (5) **noiommu 가짜 group 만들기** — IOMMU 하드웨어가 없는 디바이스와 mdev 를 위해
 * 겉모습만 갖춘 iommu_group 을 만들어, 사용자 공간 ABI 를 깨지 않는다.
 *
 * 반대로 이 파일이 **하지 않는** 일도 분명하다. IOVA 주소공간 자체는 container.c 와
 * 그 뒤의 백엔드(vfio_iommu_type1.c)가 소유하고, 디바이스 등록과 fd 공통 동작은
 * vfio_main.c 가, 2세대 cdev 경로는 device_cdev.c 가 맡는다. 이 파일은 "누가 어느
 * 주소공간에 속할 자격이 있는가" 만 판정한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * VFIO 사용자 공간 ABI 는 두 세대가 공존하고, 이 파일은 1세대의 중간 마디다.
 *
 *   [1세대 — 이 파일이 가운데]
 *     container (/dev/vfio/vfio)          ... container.c, IOVA 주소공간 1개
 *         └ group (/dev/vfio/<gid>)       ... **이 파일**, IOMMU 격리 단위
 *             └ device fd (익명 inode)    ... GROUP_GET_DEVICE_FD 로 얻음
 *
 *   [2세대 — group 을 ABI 에서 지움]
 *     iommufd (/dev/iommu)                ... IOAS 와 HWPT 가 1급 객체
 *         ↕ VFIO_DEVICE_BIND_IOMMUFD
 *     device (/dev/vfio/devices/vfio<N>)  ... device_cdev.c
 *
 * 사용자 공간(QEMU, DPDK, SPDK)이 NVMe 컨트롤러를 커널에서 뺏어 직접 모는 절차에서
 * 이 파일이 받는 것은 3, 4, 7 번이다.
 *   1. driver_override 로 vfio-pci 바인딩 → vfio_pci_core_register_device
 *      → vfio_main.c 의 vfio_register_group_dev → **이 파일의 vfio_device_set_group**
 *      이 iommu_group 을 찾아 group 을 만들고 /dev/vfio/<gid> 노드를 띄운다.
 *   2. open("/dev/vfio/vfio")                  → container fd (container.c)
 *   3. open("/dev/vfio/<gid>")                 → **이 파일의 vfio_group_fops_open**
 *   4. ioctl(group, VFIO_GROUP_SET_CONTAINER)  → **이 파일의
 *      vfio_group_ioctl_set_container** → container.c:416
 *   5. ioctl(container, VFIO_SET_IOMMU, TYPE1) → container.c → type1 백엔드
 *   6. ioctl(container, VFIO_IOMMU_MAP_DMA)    → vfio_iommu_type1.c
 *   7. ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:xx:00.0")
 *      → **이 파일의 vfio_group_ioctl_get_device_fd** → vfio_device_open_file
 *      → vfio_df_group_open → vfio_main.c 의 vfio_df_open
 *   8. 이후 mmap 과 SET_IRQS 는 device fd 위에서 vfio_main.c 와 vendor 가 처리한다.
 *
 * 실행 컨텍스트는 전부 **호스트 커널 프로세스 문맥**이다. 인터럽트 문맥에서 도는
 * 코드가 이 파일에는 없다 — 뮤텍스를 잡고 잠들 수 있으며, copy_from_user 와
 * copy_to_user 로 사용자 페이지를 만지고, GFP_KERNEL 할당을 한다. 유일한 원자 구간은
 * group->kvm_ref_lock spinlock 두 곳(vfio_device_group_get_kvm_safe,
 * vfio_group_set_kvm)이다.
 *
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/vfio.h (이미 주석 완료)
 *      : struct vfio_group 의 정의(vfio.h:206)와 enum vfio_group_type(vfio.h:170)이
 *        여기 있다. 이 파일이 구현하는 함수의 프로토타입(vfio.h:287~299)과,
 *        container.c 가 구현하는 함수의 프로토타입(vfio.h:457~478)도 마찬가지다.
 *        Kconfig 별 stub 덕분에 이 파일은 #ifdef 없이 container 함수를 부른다.
 *  - drivers/vfio/vfio_main.c (이미 주석 완료)
 *      : 이 파일이 감싸는 공통 절차의 구현처. vfio_allocate_device_file,
 *        vfio_df_open, vfio_df_close, vfio_device_fops, vfio_device_try_get_registration,
 *        vfio_device_put_registration, vfio_device_get_kvm_safe, vfio_device_put_kvm.
 *        반대로 vfio_main.c 는 이 파일의 vfio_device_set_group, vfio_device_remove_group,
 *        vfio_device_group_register/unregister, vfio_device_group_use_iommu/unuse_iommu,
 *        vfio_device_has_container, vfio_group_from_file, vfio_group_enforced_coherent,
 *        vfio_group_set_kvm, vfio_group_init/cleanup 을 부른다.
 *  - drivers/vfio/container.c
 *      : 이 파일이 SET_CONTAINER 로 넘겨주는 상대. vfio_container_from_file(:395),
 *        vfio_container_attach_group(:416), vfio_group_detach_container(:468),
 *        vfio_group_use_container(:503), vfio_group_unuse_container(:522),
 *        vfio_device_container_register(:164)/unregister(:174),
 *        vfio_container_init(:573)/cleanup(:598) 여덟 개를 쓴다.
 *        **group fd 의 참조를 거는 get_file(group->opened_file) 이 container.c:517 에
 *        있다는 점이 두 파일을 잇는 가장 중요한 매듭이다.**
 *  - drivers/vfio/vfio_iommu_type1.c (이미 주석 완료)
 *      : container 가 dispatch 하는 백엔드. 이 파일은 그것을 직접 부르지 않지만,
 *        SET_CONTAINER 와 첫 device open 이 결국 vfio_iommu_type1_attach_group 과
 *        vfio_iommu_type1_register_device 로 이어진다.
 *  - drivers/vfio/device_cdev.c
 *      : 2세대 경로. 이 파일의 vfio_device_block_group 과 vfio_device_unblock_group 을
 *        불러 legacy group 노드를 잠가 두 세대가 한 group 을 동시에 쓰지 못하게 한다.
 *  - drivers/vfio/iommufd.c (이미 주석 완료)
 *      : vfio_iommufd_device_has_compat_ioas(:113)와
 *        vfio_iommufd_compat_attach_ioas(:172)의 구현처. 1세대 ABI 위에서 2세대
 *        백엔드를 쓰는 호환 경로가 이 둘에 의존한다.
 *  - include/linux/iommufd.h
 *      : iommufd_ctx_from_file(:201), iommufd_ctx_put(:203),
 *        iommufd_vfio_compat_ioas_create(:214), iommufd_vfio_compat_set_no_iommu(:215).
 *        구현부(drivers/iommu 아래)는 이 트리에 없어 확인 못 함.
 *  - include/uapi/linux/vfio.h
 *      : 네 GROUP ioctl 번호와 struct vfio_group_status(:147),
 *        VFIO_GROUP_FLAGS_VIABLE(:150), VFIO_GROUP_FLAGS_CONTAINER_SET(:151).
 *  - drivers/vfio/pci/vfio_pci_core.c 및 vendor 드라이버
 *      : 이 파일을 직접 부르지는 않고, vfio_main.c 의 vfio_register_group_dev 를 거쳐
 *        간접적으로 vfio_device_set_group 에 도달한다.
 *  - drivers/iommu (이 트리에 없음)
 *      : iommu_group_get/put/alloc/set_name/add_device/remove_device/ref_get,
 *        iommu_group_dma_owner_claimed, device_iommu_capable 의 구현처.
 *        이 트리에서 확인 못 함이므로 이 파일의 주석은 호출 형태와 반환값 사용
 *        방식에서 확인 가능한 것만 적었다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct vfio(파일 정적)     : group_list, group_lock, group_ida, group_devt 네 필드.
 *                                 vfio_main.c 의 동명 객체와는 전혀 다른 별개 객체다.
 *  - vfio_class                 : /sys/class/vfio. devnode 콜백으로 /dev/vfio/ 접두사를 준다.
 *  - vfio_group_fops            : group fd 의 file_operations. read/write/mmap 이 없는
 *                                 순수 제어용 fd 이며, **주소 자체가 fd 신원 증명**으로도 쓰인다.
 *  - vfio_group_find_or_alloc   : iommu_group 하나당 vfio_group 하나라는 대응을 유지하는 축.
 *  - vfio_device_set_group /
 *    vfio_device_remove_group   : 등록/해제 시 group 참조(drivers)를 잡고 놓는 짝.
 *  - vfio_group_ioctl_set_container : 1세대의 결합 지점. 컨테이너 fd 와 iommufd fd 를
 *                                 모두 받아들여 두 세대를 잇는다.
 *  - vfio_device_open_file /
 *    vfio_df_group_open         : device fd 를 만드는 절차 전체. access_granted 를
 *                                 store-release 로 켜는 곳이기도 하다.
 *  - vfio_device_group_use_iommu /
 *    _unuse_iommu               : 첫 open 과 마지막 close 에서만 불리며,
 *                                 container_users 와 group fd 참조를 함께 움직인다.
 *  - vfio_group_from_file       : f_op 주소 비교로 fd 종류를 판정하는 위조 불가 검사.
 *
 * === refcount 와 수명 지도 (이 파일의 핵심) ===
 * vfio_main.c 의 지도(device.kref / device->refcount / open_count / 모듈 참조)에
 * 이 파일이 더하는 것은 다음 다섯 가지다. 각각 "무엇의 짝인가" 와 "빠뜨리면 무엇이
 * 깨지는가" 를 함께 적는다.
 *
 *  (a) group->dev 의 kref  (내장 struct device, put_device 로 감소)
 *      : vfio_group **메모리 자체**의 수명. vfio_group_alloc 의 device_initialize 가
 *        1 로 시작하고, vfio_device_remove_group 의 마지막 put_device 가 0 으로 만든다.
 *        0 이 되면 vfio_group_release 가 불려 minor 반환과 kfree 까지 간다.
 *        vfio_create_group 의 실패 경로도 put_device 로 같은 소멸자를 탄다.
 *
 *  (b) group->drivers  (refcount_t)
 *      : "이 group 에 붙어 있는 vfio 드라이버 인스턴스 수".
 *        vfio_group_alloc 의 refcount_set(1) 과 vfio_group_find_or_alloc 의
 *        refcount_inc 가 올리고, vfio_device_remove_group 의
 *        refcount_dec_and_mutex_lock 이 내린다. 감소와 락 획득이 원자적으로 묶여 있어
 *        "0 을 본 뒤 락을 잡기까지" 의 창에서 group 이 되살아나는 경합이 없다.
 *        0 인 group 은 vfio_group_fops_open 이 -ENODEV 로 막는다.
 *        빠뜨리면 사용 중인 group 이 해제되어 use-after-free 가 된다.
 *
 *  (c) iommu_group 참조
 *      : vfio_group_alloc 의 iommu_group_ref_get 이 vfio_group 자신의 몫으로 하나
 *        잡고, vfio_device_remove_group 의 iommu_group_put 이 놓는다.
 *        vfio_group_find_or_alloc 과 vfio_noiommu_group_alloc 안의 iommu_group_put 은
 *        그것과 별개인 **호출자 몫**을 놓는 것이다.
 *
 *  (d) group->container_users 와 group fd 참조 (한 쌍으로 움직인다)
 *      : container.c 의 vfio_container_attach_group 이 1 로 시작하고
 *        (그것이 "group 자신의 몫"), device fd 가 하나 열릴 때마다
 *        vfio_group_use_container(container.c:503)가 +1 하면서 **동시에**
 *        get_file(group->opened_file)로 group 파일 참조를 하나 건다(container.c:517).
 *        짝은 vfio_group_unuse_container 의 -1 과 fput 이다.
 *        이 get_file 덕분에 device fd 가 살아 있는 동안 group fd 의 release 가 불리지
 *        않고, 그래서 vfio_group_fops_release 는 container_users 검사 없이 곧장
 *        detach 할 수 있다. UNSET_CONTAINER 가 "1 이 아니면 -EBUSY" 로 거절하는 것도
 *        같은 카운터를 본다.
 *
 *  (e) group->iommufd 참조와 group->cdev_device_open_cnt
 *      : 전자는 vfio_group_ioctl_set_container 의 iommufd_ctx_from_file 이 잡고,
 *        vfio_group_ioctl_unset_container 와 vfio_group_fops_release 두 곳에서만 놓는다.
 *        device fd 는 df->iommufd 로 **빌려 쓸 뿐 참조를 잡지 않는다**
 *        (2세대 cdev 경로는 반대로 fd 마다 자기 참조를 잡는다).
 *        후자는 refcount 가 아니라 두 세대 상호 배제용 카운터로,
 *        vfio_device_block_group 이 올리고 vfio_device_unblock_group 이 내린다.
 *
 *  락 3종과 순서:
 *   - vfio.group_lock (mutex) : 전역 group_list 와 그 안 원소의 iommu_group 유효성.
 *   - group->group_lock (mutex) : container, container_users, iommufd, opened_file,
 *     cdev_device_open_cnt.
 *   - group->device_lock (mutex) : device_list.
 *   - group->kvm_ref_lock (spinlock) : kvm 포인터.
 *   락 순서는 언제나 vfio.group_lock → group->group_lock → dev_set->lock 이고,
 *   group->device_lock 은 vfio.group_lock 아래에서(vfio_group_has_device) 또는
 *   단독으로 잡힌다. 역순은 이 파일 어디에도 없다.
 *
 *  메모리 배리어 1종:
 *   - vfio_df_group_open 의 smp_store_release(&df->access_granted, true) 는
 *     vfio_main.c 의 ioctl/read/write/mmap 과 vfio_file_has_device_access 에 있는
 *     smp_load_acquire 와 쌍이다. release 앞의 모든 설정(KVM 참조, IOMMU 결합,
 *     vendor open_device, IOAS attach)이 acquire 를 통과한 쪽에서 반드시 보이게 한다.
 *
 * === noiommu 경로 — 무엇을 포기하고 무엇이 막는가 ===
 * noiommu 는 **IOMMU 하드웨어나 드라이버가 없는 물리 디바이스를 그대로 사용자에게
 * 넘기는 모드**다. 이 파일에서 그것을 만드는 곳은 vfio_noiommu_group_alloc 이고,
 * 들어가는 조건은 vfio_group_find_or_alloc 에서 "iommu_group_get 이 NULL 을 주었고
 * 모듈 파라미터 vfio_noiommu 가 켜져 있다" 이다.
 *
 * 포기하는 것:
 *  - **IOVA 번역이 없다.** 디바이스가 찍는 DMA 주소가 곧 물리 주소다. 그래서
 *    사용자 공간이 디바이스를 시켜 커널 메모리 어디든 읽고 쓸 수 있다.
 *  - **격리가 없다.** 다른 디바이스나 다른 프로세스의 메모리를 막을 수단이 없다.
 *  - **가상머신에 할당할 수 없다.** 게스트 물리주소를 호스트 물리주소로 바꿔 줄
 *    번역이 없기 때문이다.
 *  - **커널이 taint 된다.** vfio_group_find_or_alloc 이 add_taint(TAINT_USER,
 *    LOCKDEP_STILL_OK)를 부르고 dev_warn 을 남긴다. 이후 이 커널의 버그 리포트는
 *    지원 대상이 아니게 된다.
 *  - 그래도 iommu_group sysfs 인터페이스만은 흉내 낸다. 그것이 사용자 공간 ABI 의
 *    일부이고, 도구들이 /sys/bus/.../iommu_group 링크를 따라 노드를 찾기 때문이다.
 *    노드 이름에 "noiommu-" 접두사가 붙어 사용자가 구별할 수 있다.
 *
 * 이것을 막는 게이트는 **CAP_SYS_RAWIO** 하나이며, 경로마다 반복해서 걸려 있다.
 *  - 시스템 수준 opt-in : 모듈 파라미터 enable_unsafe_noiommu_mode
 *    (vfio_main.c 의 vfio_noiommu). Kconfig CONFIG_VFIO_NOIOMMU 로 아예 뺄 수도 있다.
 *  - 이 파일: vfio_group_fops_open 이 noiommu group 노드 열기를 CAP_SYS_RAWIO 로 막고,
 *    vfio_df_group_open 은 iommufd 호환 경로에서 CAP_SYS_RAWIO 와
 *    "호환 IOAS 가 없을 것" 을 함께 요구한다.
 *  - container.c: vfio_noiommu_open 이 VFIO_SET_IOMMU 때, vfio_container_attach_group 이
 *    SET_CONTAINER 때, vfio_group_use_container 가 첫 device open 때 각각 같은
 *    capability 를 다시 확인한다.
 *  - 섞임 방지: 진짜 group 과 noiommu group 은 한 컨테이너에 함께 붙을 수 없다
 *    (container.c 의 vfio_container_attach_group 과 vfio_iommu_driver_allowed).
 */

/* [한국어] VFIO 외부 ABI 헤더. struct vfio_device 와 vfio_device_ops(ops->match 슬롯을 이 파일이
 * 직접 부른다), 그리고 이 파일이 구현해 EXPORT_SYMBOL_GPL 로 내보내는
 * vfio_file_iommu_group / vfio_file_is_group / vfio_file_has_dev 의 프로토타입이 여기 있다.
 * 이 트리의 include/linux/vfio.h 가 그 실물이다. */
#include <linux/vfio.h>
/* [한국어] iommufd 컨텍스트 API — iommufd_ctx_from_file, iommufd_ctx_put,
 * iommufd_vfio_compat_ioas_create, iommufd_vfio_compat_set_no_iommu.
 * VFIO_GROUP_SET_CONTAINER 에 컨테이너 fd 대신 /dev/iommu fd 를 넘기는 호환 경로가
 * 이 네 함수 위에 서 있다. 선언은 이 트리의 include/linux/iommufd.h:201~215 에 있고
 * 구현부(drivers/iommu 아래)는 이 트리에 없어 내부 동작은 확인 못 함. */
#include <linux/iommufd.h>
/* [한국어] anon_inode_getfile_fmode 하나 때문에 필요하다. device fd 는 파일시스템에 이름이 없는
 * 익명 inode 위의 struct file 이라, GROUP_GET_DEVICE_FD 가 이 헬퍼로 파일을 만든 뒤
 * fd 에 설치한다. */
#include <linux/anon_inodes.h>
/* [한국어] drivers/vfio 내부 전용 헤더(이미 주석 완료). struct vfio_group 정의(vfio.h:206),
 * enum vfio_group_type(vfio.h:170), container.c 쪽 프로토타입(vfio.h:457~478),
 * 그리고 Kconfig 별 stub 이 전부 여기 있다. 이 파일이 #ifdef 없이
 * vfio_group_use_container 나 vfio_container_attach_group 을 부를 수 있는 이유다. */
#include "vfio.h"

/* [한국어] vfio_devnode 의 전방 선언. 바로 아래 vfio_class 초기화자가 이 함수를 가리켜야 하는데
 * 정의는 파일 맨 끝에 있어 선언을 먼저 둔다. 이 파일의 함수 블록 주석은 선언이 아니라
 * **정의** 위에 붙어 있다. */
static char *vfio_devnode(const struct device *, umode_t *);
/* [한국어] /dev/vfio/<groupID> 노드를 만들어 주는 device class. 이름이 "vfio" 라 sysfs 에서는
 * /sys/class/vfio 로 보인다. vfio_main.c 가 만드는 "vfio-dev" class(2세대 cdev 노드용)와는
 * **서로 다른 class** 다 — 1세대 group 노드와 2세대 device 노드가 sysfs 에서 갈라진다.
 * const 라 등록 후에는 바뀌지 않는다. */
static const struct class vfio_class = {
	/* [한국어] class 이름. sysfs 에 /sys/class/vfio 로 나타나고, udev 규칙이 이 이름으로 group 노드를
	 * 식별한다.
	 * 설정자: 컴파일 시 고정. const 객체라 런타임 변경 불가.
	 * 읽는 자: vfio_group_init 안의 class_register 와 sysfs 코어.
	 * 값 범위: 문자열 리터럴 "vfio" 하나로 고정.
	 * 동기화: 불변이라 필요 없다. */
	.name	= "vfio",
	/* [한국어] 이 class 에 속한 device 의 /dev 아래 경로 이름을 정하는 콜백.
	 * 기본값이면 /dev/<dev_name> 이 되는데 VFIO 는 /dev/vfio/<dev_name> 을 원하므로
	 * 이 콜백에서 앞에 디렉터리를 붙인다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: devtmpfs 와 udev 가 device 를 추가할 때 커널이 부른다.
	 * 값 범위: 이 파일 맨 끝의 vfio_devnode 정의 하나.
	 * 동기화: 상태를 공유하지 않고 kasprintf 만 하므로 재진입해도 안전하다. */
	.devnode = vfio_devnode
};

/* [한국어] 이 파일만의 모듈 전역 상태. vfio_main.c 에도 같은 이름 vfio 의 파일 정적 객체가 있지만
 * **타입도 필드도 완전히 다른 별개 객체**이며, static 이라 서로를 볼 수 없다. */
static struct vfio {
	/* [한국어] 살아 있는 모든 vfio_group 의 목록 머리. group 쪽 링크 노드는 vfio_group.vfio_next 다
	 * (vfio.h:248).
	 * 설정자: vfio_create_group 이 list_add 로 넣고, vfio_device_remove_group 이 list_del 로 뺀다.
	 * 읽는 자: vfio_group_find_from_iommu 가 iommu_group 포인터로 선형 검색하고,
	 * vfio_group_cleanup 이 비었는지 확인한다.
	 * 값 범위: 빈 목록(VFIO 디바이스가 하나도 없음) ~ group 개수만큼.
	 * 동기화: 아래 group_lock 을 반드시 쥐고 만져야 한다. */
	struct list_head		group_list;
	/* [한국어] 위 group_list 와, 그 안 원소의 iommu_group 포인터 유효성을 지키는 뮤텍스.
	 * 설정자/읽는 자: vfio_group_find_or_alloc, vfio_noiommu_group_alloc,
	 * vfio_device_remove_group 이 직접 잡고, vfio_create_group 과 vfio_group_find_from_iommu 는
	 * lockdep_assert_held 로 호출자가 잡고 왔는지만 확인한다.
	 * 값 범위: 뮤텍스라 잠들 수 있는 프로세스 문맥에서만 잡는다.
	 * 동기화: 이 락을 쥔 채 group->group_lock 을 추가로 잡는 곳이 vfio_device_remove_group 이다.
	 * 락 순서는 언제나 vfio.group_lock → group->group_lock 이며 역순은 없다. */
	struct mutex			group_lock; /* locks group_list */
	/* [한국어] group 캐릭터 디바이스의 minor 번호 할당기.
	 * 설정자: vfio_group_init 의 ida_init, vfio_group_cleanup 의 ida_destroy.
	 * 읽는 자: vfio_group_alloc 이 ida_alloc_max 로 하나 얻고, vfio_group_release 가
	 * ida_free 로 돌려준다.
	 * 값 범위: 0 ~ MINORMASK.
	 * 동기화: ida 가 내부 락을 가지므로 호출자가 따로 잡지 않는다. */
	struct ida			group_ida;
	/* [한국어] alloc_chrdev_region 으로 예약한 캐릭터 디바이스 번호 영역의 시작값.
	 * major 만 의미가 있고 minor 는 위 group_ida 가 준다.
	 * 설정자: vfio_group_init 의 alloc_chrdev_region.
	 * 읽는 자: vfio_group_alloc 이 MKDEV(MAJOR(vfio.group_devt), minor) 로 각 group 의
	 * devt 를 조립한다.
	 * 값 범위: 커널이 동적으로 배정한 major 와 minor 0.
	 * 동기화: 모듈 init/exit 에서만 건드리므로 락이 없다. */
	dev_t				group_devt;
/* [한국어] 인스턴스는 이 하나뿐이고 static 이라 파일 밖에서는 보이지 않는다. */
} vfio;

/* [한국어]
 * vfio_device_get_from_name - group 안에서 이름으로 디바이스를 찾고 등록 참조를 잡는다
 *
 * @group: 검색 대상 group. 호출자가 group fd 를 쥐고 있으므로 살아 있음이 보장된다.
 * @buf: 사용자가 GROUP_GET_DEVICE_FD 로 넘긴 이름 문자열의 커널 복사본.
 *       PCI 면 "0000:03:00.0" 같은 BDF, mdev 면 UUID 문자열이다.
 * @return: 성공 시 **등록 참조를 1 잡은** vfio_device 포인터. 실패 시 ERR_PTR —
 *          이름이 없으면 -ENODEV, vendor 의 match 가 오류를 내면 그 코드.
 *
 * 왜 필요한가: 1세대 ABI 에서 사용자는 device fd 를 오직 group fd 를 통해서만 얻는다.
 * 그때 디바이스를 지정하는 수단이 문자열 이름 하나뿐이라, 이 함수가 그 문자열을
 * 실제 vfio_device 객체로 바꾼다. 동시에 **해제 중인 디바이스를 걸러내는** 관문이기도
 * 하다.
 *
 * 동작 과정:
 *  1. group->device_lock 을 잡아 목록을 고정한다.
 *  2. 각 디바이스에 대해 vendor 의 ops->match 가 있으면 그것을, 없으면
 *     dev_name() 과의 strcmp 를 쓴다.
 *  3. 이름이 맞으면 vfio_device_try_get_registration 으로 참조를 시도한다.
 *     refcount 가 0(등록 해제 진행 중)이면 실패하고 계속 순회한다.
 *  4. 첫 성공에서 멈춘다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). group->device_lock(뮤텍스)을 잡으므로 잠들 수
 * 있다. vendor 의 match 콜백도 이 락 아래에서 불린다.
 *
 * 에러 경로: 못 찾으면 초기값 ERR_PTR(-ENODEV) 가 그대로 나간다. vendor 오류는
 * 그 코드를 ERR_PTR 로 감싸 돌려준다. 어느 경우든 참조를 잡은 것이 없다.
 *
 * refcount: 여기서 얻은 +1 은 성공하면 device fd 로 넘어가고
 * (vfio_device_fops_release 가 놓는다), fd 만들기에 실패하면 호출자가
 * vfio_device_put_registration 으로 즉시 놓는다. 빠뜨리면 refcount 가 0 에 닿지 못해
 * vfio_unregister_group_dev 가 영원히 잠들고, 반대로 두 번 놓으면 사용 중인 디바이스가
 * 해제된다.
 *
 * 호출 체인:
 *   ioctl(VFIO_GROUP_GET_DEVICE_FD) → vfio_group_fops_unl_ioctl
 *     → vfio_group_ioctl_get_device_fd → [vfio_device_get_from_name]
 *     → ops->match(vendor) / vfio_device_try_get_registration(vfio_main.c)
 */
static struct vfio_device *vfio_device_get_from_name(struct vfio_group *group,
						     char *buf)
{
	/* [한국어] 순회 커서 it 와 결과 device. 아무것도 못 찾았을 때의 기본 답을 미리 ERR_PTR(-ENODEV) 로
	 * 넣어 둔다 — 일치하는 디바이스를 찾으면 그때 덮어쓴다. */
	struct vfio_device *it, *device = ERR_PTR(-ENODEV);

	/* [한국어] group->device_list 를 도는 동안 목록이 바뀌지 않게 막는다. 같은 락으로
	 * vfio_device_group_register / vfio_device_group_unregister 가 목록을 고친다. */
	mutex_lock(&group->device_lock);
	/* [한국어] 이 group 에 등록된 vfio_device 를 하나씩 본다. 링크 필드는 device->group_next 다. */
	list_for_each_entry(it, &group->device_list, group_next) {
		/* [한국어] 이번 항목의 이름 비교 결과. 1 이면 일치, 0 이면 불일치, 음수면 vendor 가 낸 오류. */
		int ret;

		/* [한국어] vendor 가 자기만의 이름 규칙을 가진 경우. mdev 는 디바이스 이름이 BDF 가 아니라 UUID 라
		 * 이 콜백이 필요하다. ops->match 는 include/linux/vfio.h 의 vfio_device_ops 슬롯이다. */
		if (it->ops->match) {
			/* [한국어] vendor 에게 이름 비교를 맡긴다. */
			ret = it->ops->match(it, buf);
			/* [한국어] vendor 가 오류를 냈으면 더 볼 것 없이 그 오류를 그대로 사용자에게 돌려준다. */
			if (ret < 0) {
				/* [한국어] 오류 코드를 포인터로 감싸 결과 변수에 싣는다. 호출자는 IS_ERR 로 가른다. */
				device = ERR_PTR(ret);
				/* [한국어] 순회를 즉시 끝낸다. 참조를 잡은 것이 없으므로 놓을 것도 없다. */
				break;
			}
		/* [한국어] vendor 가 match 를 주지 않았으면 코어의 기본 규칙을 쓴다. */
		} else {
			/* [한국어] 하부 struct device 의 이름(PCI 면 "0000:03:00.0" 같은 BDF 문자열)과 사용자가 준
			 * 문자열을 그대로 비교한다. strcmp 는 같을 때 0 을 주므로 ! 로 뒤집어 1(일치)로 만든다. */
			ret = !strcmp(dev_name(it->dev), buf);
		}

		/* [한국어] 이름이 맞고 **동시에** 등록 참조를 얻을 수 있을 때만 채택한다.
		 * vfio_device_try_get_registration(vfio_main.c)은 refcount_inc_not_zero 라 0→1 전이를
		 * 거부한다. 0 은 vfio_unregister_group_dev 가 이미 해제 대기에 들어갔다는 뜻이므로,
		 * 여기서 되살리면 use-after-free 가 된다.
		 * 여기서 얻은 +1 의 짝: fd 만들기에 성공하면 참조가 device fd 로 넘어가
		 * vfio_device_fops_release 가 놓고, 실패하면 vfio_group_ioctl_get_device_fd 가
		 * 그 자리에서 vfio_device_put_registration 으로 놓는다. 빠뜨리면 refcount 가 0 에 닿지
		 * 못해 unregister 가 영원히 잠든다. */
		if (ret && vfio_device_try_get_registration(it)) {
			/* [한국어] 결과로 확정한다. 참조를 이미 쥐었으므로 락을 놓아도 유효하다. */
			device = it;
			/* [한국어] 첫 일치에서 멈춘다. 한 group 안에 같은 이름의 디바이스가 둘 있을 수 없다. */
			break;
		}
	}
	/* [한국어] 순회가 끝났으니 목록 락을 놓는다. 성공 경로는 이미 참조를 쥐고 있어 락 밖에서도
	 * device 포인터가 유효하다. */
	mutex_unlock(&group->device_lock);

	/* [한국어] 성공이면 등록 참조를 하나 쥔 디바이스, 실패면 ERR_PTR(-ENODEV) 또는 vendor 오류.
	 * 호출자 vfio_group_ioctl_get_device_fd 가 IS_ERR 로 가른다. */
	return device;
}

/* [한국어]
 * vfio_group_has_iommu - 이 group 이 어떤 IOVA 주소공간에든 결합돼 있는가
 *
 * @group: 검사할 group. 호출자가 group->group_lock 을 잡고 와야 한다.
 * @return: container(1세대) 또는 iommufd(2세대 호환) 중 하나라도 있으면 true.
 *
 * 왜 필요한가: VFIO 의 기본 규칙은 "IOMMU 결합이 성립하기 전에는 디바이스를 사용자에게
 * 넘기지 않는다" 이다. 이 함수가 그 조건을 한 곳에 모아 놓은 술어이며,
 * SET_CONTAINER(이미 붙어 있으면 거절), UNSET_CONTAINER(안 붙어 있으면 거절),
 * GET_STATUS(플래그 계산), 첫 device open(안 붙어 있으면 거절) 네 곳이 모두 이것을 쓴다.
 *
 * 동작 과정: lockdep 으로 락을 확인하고, container 와 container_users 가 서로 어긋나지
 * 않았는지 WARN_ON 으로 검사한 뒤, 두 포인터를 OR 해서 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, group->group_lock 을 쥔 상태. 잠들지 않는다.
 *
 * 에러 경로: 없다. 불변식 위반은 WARN_ON 으로 알리기만 하고 반환값을 바꾸지 않는다.
 *
 * 호출 체인:
 *   vfio_group_ioctl_unset_container / vfio_group_ioctl_set_container /
 *   vfio_group_ioctl_get_status / vfio_df_group_open → [vfio_group_has_iommu]
 */
/*
 * VFIO Group fd, /dev/vfio/$GROUP
 */
static bool vfio_group_has_iommu(struct vfio_group *group)
{
	/* [한국어] 호출자가 group_lock 을 잡고 왔는지 확인한다. 아래에서 읽는 container,
	 * container_users, iommufd 세 필드가 모두 그 락으로 보호된다. lockdep 이 꺼진 빌드에서는
	 * 아무 코드도 만들지 않는다. */
	lockdep_assert_held(&group->group_lock);
	/*
	 * There can only be users if there is a container, and if there is a
	 * container there must be users.
	 */
	/* [한국어] 불변식 검사: container 포인터가 있으면 container_users 도 0 이 아니어야 하고, 반대도
	 * 같다. ! 를 씌워 둘 다 0/1 로 정규화한 뒤 != 로 비교하므로, 둘 중 하나만 설정된
	 * 어긋난 상태에서만 경고가 뜬다. 이 짝은 container.c 의 vfio_container_attach_group 이
	 * 동시에 세우고(container 대입 + container_users = 1) vfio_group_detach_container 가
	 * 동시에 지운다. */
	WARN_ON(!group->container != !group->container_users);

	/* [한국어] 이 group 이 어떤 IOVA 주소공간에든 결합돼 있는가. 1세대(container)든
	 * 2세대 호환(iommufd)이든 하나라도 있으면 참이다. 이 함수가 참을 돌려주기 전에는
	 * device fd 를 열 수 없다 — 격리가 성립하지 않은 채로 디바이스를 사용자에게
	 * 넘기지 않겠다는 뜻이다. */
	return group->container || group->iommufd;
}

/* [한국어]
 * vfio_group_ioctl_unset_container - VFIO_GROUP_UNSET_CONTAINER ioctl 핸들러
 *
 * @group: group fd 의 private_data 에서 온 group.
 * @return: 0 성공. -EINVAL 이면 애초에 결합이 없었고, -EBUSY 면 이 group 을 통해 열린
 *          device fd 가 아직 살아 있다.
 *
 * 왜 필요한가: 사용자가 group 을 컨테이너에서 떼어 다른 컨테이너에 붙이거나, 그냥
 * 초기 상태로 되돌리고 싶을 때 쓴다. 상류 주석대로 group fd 를 쥐고 부르는 ioctl 이므로
 * group 자체는 반드시 살아 있고, 따라서 유효한 전이는 container_users 의 1→0 뿐이다.
 *
 * 동작 과정:
 *  1. group_lock 을 잡는다.
 *  2. 결합이 없으면 -EINVAL.
 *  3. 컨테이너에 붙어 있으면 container_users 가 정확히 1 인지 본다. 1 이 아니면
 *     device fd 가 남아 있다는 뜻이라 -EBUSY.
 *  4. vfio_group_detach_container(container.c)로 백엔드 detach, DMA 소유권 반환,
 *     마지막 group 이면 백엔드 release 와 module_put, 그리고 컨테이너 참조 반환까지 한다.
 *  5. iommufd 호환 모드였다면 SET_CONTAINER 가 잡아 둔 컨텍스트 참조를 놓고 NULL 로 비운다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl), group->group_lock 을 쥔 채 진행한다.
 *
 * 에러 경로: 두 실패 모두 아무것도 바꾸지 않고 락만 풀고 나간다.
 *
 * refcount: 4단계가 vfio_container_attach_group 의 vfio_container_get 을 되돌리고,
 * 5단계가 iommufd_ctx_from_file 의 참조를 되돌린다. 5단계를 빠뜨리면 /dev/iommu 를
 * 닫아도 IOAS 와 그 핀된 페이지가 통째로 샌다.
 *
 * 호출 체인:
 *   ioctl(VFIO_GROUP_UNSET_CONTAINER) → vfio_group_fops_unl_ioctl
 *     → [vfio_group_ioctl_unset_container]
 *     → vfio_group_has_iommu / vfio_group_detach_container(container.c) / iommufd_ctx_put
 */
/*
 * VFIO_GROUP_UNSET_CONTAINER should fail if there are other users or
 * if there was no container to unset.  Since the ioctl is called on
 * the group, we know that still exists, therefore the only valid
 * transition here is 1->0.
 */
static int vfio_group_ioctl_unset_container(struct vfio_group *group)
{
	/* [한국어] 결과 코드. 성공 경로는 0 그대로 나간다. */
	int ret = 0;

	/* [한국어] group 단위 상태(container, container_users, iommufd, opened_file)를 직렬화한다.
	 * 이 ioctl 은 그 셋을 한꺼번에 바꾸므로 전체를 한 임계구역에 넣는다. */
	mutex_lock(&group->group_lock);
	/* [한국어] 애초에 결합된 것이 없으면 풀 것도 없다. 상류 주석대로 "풀 컨테이너가 없었다" 는
	 * 실패로 취급한다. */
	if (!vfio_group_has_iommu(group)) {
		/* [한국어] 결합이 없는 상태에서의 UNSET 은 잘못된 인자다. */
		ret = -EINVAL;
		/* [한국어] 락을 놓는 공통 출구로 간다. */
		goto out_unlock;
	}
	/* [한국어] 1세대 컨테이너에 붙어 있는 경우. */
	if (group->container) {
		/* [한국어] container_users 가 1 이 아니면 이 group 을 통해 열린 device fd 가 아직 살아 있다는
		 * 뜻이다. 1 은 SET_CONTAINER 가 세운 "group 자신의 몫"이고, device fd 하나마다
		 * vfio_group_use_container 가 +1 을 더한다(container.c:518). 상류 주석이 말하는
		 * "유일하게 유효한 전이는 1→0" 이 이 검사다. */
		if (group->container_users != 1) {
			/* [한국어] 아직 쓰는 사람이 있으니 -EBUSY. 사용자는 device fd 를 모두 닫고 다시 시도해야 한다. */
			ret = -EBUSY;
			/* [한국어] 공통 출구. */
			goto out_unlock;
		}
		/* [한국어] container.c:468 의 vfio_group_detach_container 를 부른다. 그 안에서
		 * 백엔드의 detach_group(type1 이면 vfio_iommu_type1_detach_group)을 부르고,
		 * iommu_group_release_dma_owner 로 group 소유권을 커널에 돌려주며,
		 * 마지막 group 이었다면 백엔드 release 와 module_put 까지 한 뒤
		 * vfio_container_put 으로 컨테이너 참조 하나를 놓는다.
		 * 여기서 놓는 참조는 vfio_container_attach_group 의 vfio_container_get 짝이다. */
		vfio_group_detach_container(group);
	}
	/* [한국어] 2세대 호환 경로로 iommufd 컨텍스트에 묶여 있던 경우. */
	if (group->iommufd) {
		/* [한국어] SET_CONTAINER 가 iommufd_ctx_from_file 로 얻어 둔 참조를 놓는다. 이것을 빠뜨리면
		 * /dev/iommu 를 닫아도 컨텍스트가 살아남아 IOAS 와 그 핀된 페이지가 통째로 샌다. */
		iommufd_ctx_put(group->iommufd);
		/* [한국어] 포인터를 지워 vfio_group_has_iommu 가 다시 거짓이 되게 한다. 이 대입이 없으면
		 * 이미 놓은 컨텍스트를 다음 open 이 다시 쓰는 use-after-free 가 된다. */
		group->iommufd = NULL;
	}

/* [한국어] 성공과 실패가 모두 합류하는 출구 라벨. */
out_unlock:
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&group->group_lock);
	/* [한국어] 0 또는 -EINVAL/-EBUSY 를 ioctl 반환값으로 사용자에게 그대로 준다. */
	return ret;
}

/* [한국어]
 * vfio_group_ioctl_set_container - VFIO_GROUP_SET_CONTAINER ioctl 핸들러
 *
 * @group: group fd 의 private_data 에서 온 group.
 * @arg: 사용자 공간의 int 포인터. 그 안에 결합할 fd 번호가 들어 있다.
 * @return: 0 성공. -EFAULT(사용자 포인터 오류), -EBADF(그런 fd 없음),
 *          -EINVAL(이미 결합됨), -ENODEV(group 이 이미 무효화됨),
 *          -EBADFD(fd 는 있으나 컨테이너도 iommufd 도 아님), 그 밖에 결합 중 발생한 오류.
 *
 * 왜 필요한가: **1세대 3층 모델에서 group 을 주소공간에 묶는 유일한 지점**이다.
 * 이 ioctl 이 성공해야 비로소 GROUP_GET_DEVICE_FD 가 열리고, 컨테이너 쪽에서는
 * VFIO_SET_IOMMU 가 가능해진다(컨테이너는 group 이 하나라도 붙어야 권한이 생긴다).
 * 또한 2세대 백엔드(iommufd)를 1세대 ABI 로 쓰는 호환 경로의 분기점이기도 하다.
 *
 * 동작 과정:
 *  1. get_user 로 fd 번호를 읽고, CLASS(fd, f) 스코프 가드로 파일을 잡는다.
 *     이 가드 덕분에 어느 return 으로 나가도 fdput 이 자동으로 실행된다.
 *  2. group_lock 을 잡고, 이미 결합돼 있으면 -EINVAL, group 이 무효화됐으면 -ENODEV.
 *  3. vfio_container_from_file 로 /dev/vfio/vfio 인지 본다. 맞으면
 *     vfio_container_attach_group(container.c)에 위임하고 끝.
 *  4. 아니면 iommufd_ctx_from_file 로 /dev/iommu 인지 본다. 맞으면 noiommu 여부에 따라
 *     iommufd_vfio_compat_set_no_iommu 또는 iommufd_vfio_compat_ioas_create 를 부르고,
 *     성공하면 컨텍스트 참조의 소유권을 group 에 넘긴다.
 *  5. 둘 다 아니면 -EBADFD.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). group->group_lock 을 쥔 채 컨테이너 쪽
 * rw_semaphore 까지 잡으러 들어간다(container.c 의 down_write). 락 순서는
 * group->group_lock → container->group_lock 이며 역순은 이 트리에 없다.
 *
 * 에러 경로: 4단계에서 호환 설정이 실패하면 그 자리에서 iommufd_ctx_put 으로
 * 방금 얻은 참조를 되돌린다. 3단계 실패는 container.c 안에서 완전히 되감긴다.
 *
 * 호출 체인:
 *   ioctl(VFIO_GROUP_SET_CONTAINER) → vfio_group_fops_unl_ioctl
 *     → [vfio_group_ioctl_set_container]
 *     → vfio_container_from_file / vfio_container_attach_group(container.c)
 *     → iommufd_ctx_from_file / iommufd_vfio_compat_ioas_create
 */
static int vfio_group_ioctl_set_container(struct vfio_group *group,
					  int __user *arg)
{
	/* [한국어] 사용자가 준 fd 가 컨테이너 fd 였을 때 여기에 담긴다. 아니면 끝까지 미초기화로 남지 않고 vfio_container_from_file 이 NULL 을 준다. */
	struct vfio_container *container;
	/* [한국어] 사용자가 준 fd 가 /dev/iommu fd 였을 때의 iommufd 컨텍스트. 성공 시 group 이 이 참조를 소유한다. */
	struct iommufd_ctx *iommufd;
	/* [한국어] 결과 코드. 세 갈래(컨테이너/iommufd/미인식) 중 어디로 가든 out_unlock 에서 이 값이 나간다. */
	int ret;
	/* [한국어] 사용자가 넘긴 파일 디스크립터 번호. 커널 포인터가 아니라 정수라 get_user 로 복사해야 한다. */
	int fd;

	/* [한국어] 사용자 공간 포인터에서 fd 정수 하나를 안전하게 읽는다. 페이지가 없거나 권한이 없으면 0 이 아닌 값을 준다. */
	if (get_user(fd, arg))
		/* [한국어] 사용자 포인터가 잘못됐다. 아무것도 잡지 않은 상태라 그냥 돌아가면 된다. */
		return -EFAULT;
/* [한국어] CLASS(fd, f)(fd) 는 __cleanup 기반 스코프 가드다. fdget 으로 파일을 잡고, 이 함수의
 * 어느 return 으로 나가든 컴파일러가 자동으로 fdput 을 넣어 준다. 이 파일에는 명시적
 * fdput 이 한 줄도 없는 이유가 이것이다. 정의는 include/linux/file.h 에 있고 그 헤더는
 * 이 트리에 없어 구현은 확인 못 함. */

	CLASS(fd, f)(fd);
	/* [한국어] 그 fd 번호에 해당하는 열린 파일이 없다. */
	if (fd_empty(f))
		/* [한국어] -EBADF: "그런 fd 가 없다". 아래 -EBADFD(fd 는 있지만 종류가 틀렸다)와 다른 오류다. */
		return -EBADF;

	/* [한국어] group 의 결합 상태를 바꾸는 구간 전체를 직렬화한다. */
	mutex_lock(&group->group_lock);
	/* [한국어] 이미 컨테이너나 iommufd 에 붙어 있으면 갈아탈 수 없다. group 은 한 번에 한
	 * IOVA 주소공간에만 속한다 — 이것이 VFIO 격리 모델의 기본 규칙이다.
	 * 바꾸려면 먼저 UNSET_CONTAINER 를 해야 한다. */
	if (vfio_group_has_iommu(group)) {
		/* [한국어] 이미 결합돼 있다는 뜻이므로 잘못된 인자. */
		ret = -EINVAL;
		/* [한국어] 락 해제 공통 출구. */
		goto out_unlock;
	}
	/* [한국어] iommu_group 이 NULL 이면 vfio_device_remove_group 이 이 group 의 마지막 디바이스를
	 * 떼면서 이미 무효화한 것이다(그 함수가 group_lock 아래에서 NULL 을 넣는다). */
	if (!group->iommu_group) {
		/* [한국어] 실체가 사라진 group 이므로 -ENODEV. */
		ret = -ENODEV;
		/* [한국어] 공통 출구. */
		goto out_unlock;
	}

	/* [한국어] 이 파일이 정말 /dev/vfio/vfio 인지 확인한다. container.c:395 의
	 * vfio_container_from_file 은 file->f_op 를 자기 fops 주소와 비교하는 방식이라
	 * 위조가 불가능하다. 아니면 NULL 을 준다. */
	container = vfio_container_from_file(fd_file(f));
	/* [한국어] 1세대 컨테이너 fd 였다. */
	if (container) {
		/* [한국어] container.c:416 의 vfio_container_attach_group 으로 넘긴다. 그 안에서
		 * noiommu 혼용 금지 검사 → iommu_group_claim_dma_owner 로 커널 드라이버 축출 →
		 * 이미 SET_IOMMU 가 끝난 컨테이너면 백엔드 attach_group(type1 이면
		 * vfio_iommu_type1_attach_group) 호출 → group->container 대입과 container_users = 1 →
		 * vfio_container_get 으로 컨테이너 참조 +1 순으로 진행한다.
		 * 그 +1 의 짝은 UNSET_CONTAINER 나 group fd 해제가 부르는
		 * vfio_group_detach_container 안의 vfio_container_put 이다. */
		ret = vfio_container_attach_group(container, group);
		/* [한국어] 성공이든 실패든 그 결과가 그대로 ioctl 반환값이 된다. */
		goto out_unlock;
	}

	/* [한국어] 컨테이너가 아니었으니 이번엔 /dev/iommu 인지 본다. 성공하면 **참조를 하나 잡아**
	 * 돌려준다(include/linux/iommufd.h:201). 실패하면 ERR_PTR 이며 그때는 잡은 것이 없다. */
	iommufd = iommufd_ctx_from_file(fd_file(f));
	/* [한국어] iommufd 컨텍스트가 맞다 — 2세대 백엔드를 1세대 group ABI 로 쓰는 호환 경로다. */
	if (!IS_ERR(iommufd)) {
		/* [한국어] noiommu group 을 iommufd 에 붙이는 경우. IS_ENABLED 는 Kconfig 가 꺼져 있으면
		 * 컴파일 시 0 이 되어 이 분기가 통째로 사라진다. */
		if (IS_ENABLED(CONFIG_VFIO_NOIOMMU) &&
		    group->type == VFIO_NO_IOMMU)
			/* [한국어] iommufd 컨텍스트에 "이 컨텍스트에는 번역이 없다" 고 표시한다. noiommu 는 IOVA 번역
			 * 자체가 없으므로 IOAS 를 만들면 안 된다. 구현은 drivers/iommu 아래라 이 트리에서
			 * 확인 못 함. */
			ret = iommufd_vfio_compat_set_no_iommu(iommufd);
		/* [한국어] noiommu 가 아닌 보통 group 이거나 CONFIG_VFIO_NOIOMMU 가 꺼진 빌드일 때 이 갈래로 온다. */
		else
			/* [한국어] 보통 경로: 1세대 ABI 가 기대하는 "컨테이너 = 주소공간 하나" 를 흉내 내도록
			 * iommufd 안에 호환 IOAS 를 하나 만든다. 이후 VFIO_IOMMU_MAP_DMA 류 ioctl 이
			 * 그 IOAS 로 간다. 구현은 이 트리에 없어 확인 못 함. */
			ret = iommufd_vfio_compat_ioas_create(iommufd);

		/* [한국어] 호환 설정에 실패했다. */
		if (ret) {
			/* [한국어] 바로 위 iommufd_ctx_from_file 이 잡아 준 참조를 되돌린다. 이 실패 경로에서만
			 * 우리가 참조의 주인이므로, 여기서 놓지 않으면 컨텍스트가 영원히 샌다. */
			iommufd_ctx_put(iommufd);
			/* [한국어] 공통 출구. ret 에는 호환 설정 실패 코드가 들어 있다. */
			goto out_unlock;
		}

		/* [한국어] 성공. iommufd_ctx_from_file 이 잡은 참조의 소유권이 이제 group 으로 넘어간다.
		 * 그 참조를 놓는 곳은 vfio_group_ioctl_unset_container 와 vfio_group_fops_release
		 * 두 곳뿐이다. */
		group->iommufd = iommufd;
		/* [한국어] 성공했으므로 ret 은 0 인 채로 출구로 간다. */
		goto out_unlock;
	}

	/* The FD passed is not recognized. */
	/* [한국어] 컨테이너도 iommufd 도 아니었다. -EBADFD 는 "fd 자체는 열려 있으나 상태/종류가
	 * 이 요청에 맞지 않다" 는 뜻으로, 위쪽의 -EBADF(그런 fd 없음)와 구분된다. */
	ret = -EBADFD;

/* [한국어] 세 갈래가 모두 합류하는 출구. */
out_unlock:
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&group->group_lock);
	/* [한국어] 0 또는 오류 코드가 ioctl 반환값이 된다. */
	return ret;
}

/* [한국어]
 * vfio_device_group_get_kvm_safe - group 에 붙은 KVM 포인터를 안전하게 읽어 참조를 잡는다
 *
 * @device: 지금 처음 열리는 디바이스. 결과는 device->kvm 에 저장된다.
 * @return: 없음. KVM 이 없으면 아무 일도 일어나지 않는다.
 *
 * 왜 필요한가: KVM 은 vfio_file_set_kvm(vfio_main.c)을 통해 group 에 자기 포인터를
 * 비동기로 심어 두고, 사라질 때 NULL 로 다시 심는다. 그 갱신과 디바이스 첫 open 이
 * 겹치면 이미 해제된 kvm 포인터를 잡을 수 있다. 이 함수는 spinlock 안에서 읽고
 * 그 자리에서 참조까지 잡아 그 창을 없앤다 — 이름의 "safe" 가 그 뜻이다.
 *
 * 동작 과정: group->kvm_ref_lock 을 잡고, vfio_device_get_kvm_safe(vfio_main.c)에
 * group->kvm 을 넘긴 뒤 락을 푼다. 그 함수가 KVM 모듈을 붙잡고 kvm_get_kvm 참조를
 * 얻어 device->kvm 에 저장한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. spinlock 을 잡으므로 그 안에서는 잠들 수 없다.
 * 호출자는 이미 group->group_lock 과 dev_set->lock 을 쥐고 있다.
 *
 * 에러 경로: 없다. group->kvm 이 NULL 이면 하위 함수가 조용히 아무것도 하지 않는다.
 *
 * refcount: 여기서 얻은 KVM 참조의 짝은 vfio_device_put_kvm 이며,
 * open_count 가 다시 0 이 되는 시점(vfio_df_group_open 의 실패 경로 또는
 * vfio_df_group_close)에 놓는다. 빠뜨리면 게스트 VM 객체가 해제되지 못한다.
 *
 * 호출 체인:
 *   vfio_df_group_open → [vfio_device_group_get_kvm_safe]
 *     → vfio_device_get_kvm_safe(vfio_main.c)
 */
static void vfio_device_group_get_kvm_safe(struct vfio_device *device)
{
	/* [한국어] group->kvm 포인터를 읽는 동안 KVM 쪽이 vfio_group_set_kvm 으로 그것을 바꾸지 못하게
	 * 막는다. spinlock 인 이유는 KVM 이 이 값을 갱신하는 문맥이 짧고 잠들 수 없기 때문이다. */
	spin_lock(&device->group->kvm_ref_lock);
	/* [한국어] vfio_main.c 의 vfio_device_get_kvm_safe 를 부른다. 그 함수는 kvm 포인터가 NULL 이
	 * 아니면 KVM 모듈을 symbol_get 으로 붙잡고 kvm_get_kvm 참조를 얻어 device->kvm 에
	 * 저장한다. 여기서 얻은 참조의 짝은 vfio_device_put_kvm 이며,
	 * open_count 가 다시 0 이 되는 시점(vfio_df_group_open 의 실패 경로 또는
	 * vfio_df_group_close)에 놓는다. 빠뜨리면 VM 이 파괴되지 못한다. */
	vfio_device_get_kvm_safe(device, device->group->kvm);
	/* [한국어] 짧은 원자 구간을 닫는다. 이 안에서는 잠들 수 없다. */
	spin_unlock(&device->group->kvm_ref_lock);
}

/* [한국어]
 * vfio_df_group_open - 1세대 group 경로에서 device fd 하나를 실제로 여는 절차 전체
 *
 * @df: 방금 할당된 fd 문맥. df->device 와 df->group 이 채워져 있다.
 * @return: 0 성공. -EINVAL(group 이 어디에도 결합돼 있지 않음),
 *          -EPERM(noiommu 인데 RAWIO 가 없거나 호환 IOAS 가 이미 있음),
 *          그 밖에 vfio_df_open 이나 IOAS attach 가 낸 오류.
 *
 * 왜 필요한가: device fd 를 열 때 해야 하는 일이 vfio_main.c 의 공통 부분(open_count,
 * vendor 콜백, 모듈 참조)과 group 경로 고유의 부분(KVM 참조, iommufd 호환 IOAS,
 * group 결합 확인)으로 나뉜다. 이 함수가 후자를 감싸고 전자를 vfio_df_open 에 위임한다.
 * 2세대 대응물은 device_cdev.c 의 vfio_df_ioctl_bind_iommufd 다.
 *
 * 동작 과정:
 *  1. group_lock → dev_set->lock 순으로 두 락을 잡는다.
 *  2. vfio_group_has_iommu 로 결합 여부를 확인한다(없으면 -EINVAL).
 *  3. 첫 open(open_count == 0)이면 group 의 KVM 참조를 잡아 둔다.
 *  4. group->iommufd 를 df 에 복사한다(참조는 새로 잡지 않는다).
 *  5. iommufd 호환 + noiommu + 첫 open 이면 CAP_SYS_RAWIO 와 "호환 IOAS 가 없을 것" 을
 *     요구한다.
 *  6. vfio_df_open(vfio_main.c)으로 open_count 증가와 vendor 콜백을 처리한다.
 *  7. iommufd 호환이고 이번이 첫 open 이면 호환 IOAS 에 디바이스를 붙인다.
 *  8. smp_store_release 로 access_granted 를 켜 이 fd 의 ioctl/read/write/mmap 을 연다.
 *
 * 실행 컨텍스트: 프로세스 문맥(GROUP_GET_DEVICE_FD ioctl). 두 뮤텍스를 쥐고 있어
 * 잠들 수 있다. 락 순서는 언제나 group->group_lock → dev_set->lock 이다.
 *
 * 에러 경로: 세 라벨이 계단식으로 이어진다. out_close_device 는 vfio_df_open 을
 * 되돌리고 out_put_kvm 으로 떨어지며, out_put_kvm 은 df->iommufd 를 비우고
 * open_count 가 0 이면 KVM 참조를 놓는다. out_unlock 은 아직 안쪽 락을 잡기 전이라
 * 바깥 락만 푼다.
 *
 * 호출 체인:
 *   vfio_group_ioctl_get_device_fd → vfio_device_open_file → [vfio_df_group_open]
 *     → vfio_group_has_iommu / vfio_device_group_get_kvm_safe /
 *       vfio_iommufd_device_has_compat_ioas(iommufd.c) / vfio_df_open(vfio_main.c) /
 *       vfio_iommufd_compat_attach_ioas(iommufd.c)
 */
static int vfio_df_group_open(struct vfio_device_file *df)
{
	/* [한국어] 이 fd 가 가리키는 디바이스. df 하나가 fd 하나이고, 한 device 에 여러 df 가 붙을 수 있다. */
	struct vfio_device *device = df->device;
	/* [한국어] 각 단계의 결과 코드. */
	int ret;

	/* [한국어] group 결합 상태(container/iommufd)를 읽는 동안 SET/UNSET_CONTAINER 가 끼어들지 못하게
	 * 막는다. 락 순서는 group_lock → dev_set->lock 이며 이 파일 어디서도 역순은 없다. */
	mutex_lock(&device->group->group_lock);
	/* [한국어] 결합이 없으면 device fd 를 열어 줄 수 없다. IOMMU 격리가 성립하지 않은 상태에서
	 * 디바이스를 사용자에게 넘기지 않겠다는 VFIO 의 핵심 규칙이다. */
	if (!vfio_group_has_iommu(device->group)) {
		/* [한국어] container 도 iommufd 도 없는 group 이다. */
		ret = -EINVAL;
		/* [한국어] 락 하나만 푸는 출구로 간다. */
		goto out_unlock;
	}

	/* [한국어] open_count 와 df->iommufd 를 지키는 락. 같은 리셋 도메인(같은 PCI slot/bus)의
	 * 디바이스들이 이 락 하나를 공유하므로, vfio-pci 의 "형제 함수가 모두 닫혔는가" 판단이
	 * 성립한다. */
	mutex_lock(&device->dev_set->lock);

	/*
	 * Before the first device open, get the KVM pointer currently
	 * associated with the group (if there is one) and obtain a reference
	 * now that will be held until the open_count reaches 0 again.  Save
	 * the pointer in the device for use by drivers.
	 */
	/* [한국어] 첫 open 일 때만. open_count 는 아직 증가 전이므로 0 이 곧 "내가 첫 번째" 다. */
	if (device->open_count == 0)
		/* [한국어] group 에 연결된 KVM 이 있으면 참조를 잡아 device->kvm 에 저장한다. vendor 드라이버가
		 * 게스트와 협력해야 할 때(예: 캐시 일관성 정책) 쓴다. */
		vfio_device_group_get_kvm_safe(device);

	/* [한국어] group 이 iommufd 호환 모드로 붙어 있으면 그 컨텍스트를 fd 문맥에 복사한다.
	 * **참조를 새로 잡지 않는다** — 컨텍스트의 유일한 참조는 group 이 SET_CONTAINER 때
	 * 잡아 둔 것이고, group fd 가 닫힐 때 놓는다. 2세대 cdev 경로(device_cdev.c)는 반대로
	 * fd 마다 iommufd_ctx_from_fd 로 자기 참조를 잡고 unbind 에서 놓는다.
	 * NULL 이면 1세대 컨테이너 경로라는 뜻이다. */
	df->iommufd = device->group->iommufd;
	/* [한국어] iommufd 호환 모드인데 이 디바이스가 noiommu 인 경우의 특수 검사. 첫 open 에서만 한다.
	 * vfio_device_is_noiommu 는 vfio.h:304 의 인라인으로 group->type 이 VFIO_NO_IOMMU 인지
	 * 본다. */
	if (df->iommufd && vfio_device_is_noiommu(device) && device->open_count == 0) {
		/*
		 * Require no compat ioas to be assigned to proceed.  The basic
		 * statement is that the user cannot have done something that
		 * implies they expected translation to exist
		 */
		/* [한국어] noiommu 는 IOMMU 번역도 격리도 없이 물리 DMA 를 사용자에게 그대로 주는 모드라
		 * CAP_SYS_RAWIO 를 요구한다. capable() 은 현재 프로세스의 유효 capability 집합을 본다. */
		if (!capable(CAP_SYS_RAWIO) ||
		    /* [한국어] 게다가 이 iommufd 컨텍스트에 호환 IOAS 가 이미 만들어져 있으면 안 된다.
		     * IOAS 가 있다는 것은 사용자가 "번역이 존재한다" 고 가정하고 준비했다는 뜻인데,
		     * noiommu 에는 번역이 없으므로 그 가정 자체가 틀렸다. 구현은 iommufd.c:113 의
		     * vfio_iommufd_device_has_compat_ioas. */
		    vfio_iommufd_device_has_compat_ioas(device, df->iommufd)) {
			/* [한국어] 권한이 없거나 기대가 어긋났다 — 둘 다 -EPERM. */
			ret = -EPERM;
			/* [한국어] 아직 vfio_df_open 을 부르지 않았지만, 바로 위에서 KVM 참조를 잡았을 수 있으므로
			 * 그것을 되돌리는 라벨로 간다. */
			goto out_put_kvm;
		}
	}

	/* [한국어] vfio_main.c 의 vfio_df_open. open_count 를 올리고 0→1 전이에서만
	 * vfio_df_device_first_open 을 통해 vendor 모듈 참조 획득 →
	 * vfio_device_group_use_iommu(이 파일) → vendor 의 open_device 를 차례로 부른다.
	 * 1세대는 한 device 를 여러 번 여는 것을 허용하므로 두 번째 이후 open 은 카운트만 올린다. */
	ret = vfio_df_open(df);
	/* [한국어] 첫 open 안에서 vendor 나 IOMMU 결합이 실패했다. */
	if (ret)
		/* [한국어] KVM 참조만 되돌리면 되는 라벨로 간다. open_count 는 vfio_df_open 이 이미 원복했다. */
		goto out_put_kvm;

	/* [한국어] iommufd 호환 모드이면서 **이번이 첫 open** 일 때만. open_count 는 vfio_df_open 이
	 * 이미 1 로 올려 둔 상태라 여기서는 0 이 아니라 1 과 비교한다. */
	if (df->iommufd && device->open_count == 1) {
		/* [한국어] iommufd.c:172 의 vfio_iommufd_compat_attach_ioas. SET_CONTAINER 때 만들어 둔
		 * 호환 IOAS 에 이 디바이스를 붙여, 1세대 MAP_DMA 로 넣은 매핑이 실제로 이 디바이스에
		 * 적용되게 한다. */
		ret = vfio_iommufd_compat_attach_ioas(device, df->iommufd);
		/* [한국어] IOAS 결합 실패. */
		if (ret)
			/* [한국어] 이미 성공한 vfio_df_open 부터 되돌려야 한다. */
			goto out_close_device;
	}

	/*
	 * Paired with smp_load_acquire() in vfio_device_fops::ioctl/
	 * read/write/mmap and vfio_file_has_device_access()
	 */
	/* [한국어] 이 store-release 가 이 fd 의 **권한 스위치**다. 이 시점 이전에 한 모든 설정
	 * (KVM 참조, IOMMU 결합, vendor open_device, IOAS attach)이 다른 CPU 에서
	 * smp_load_acquire 로 true 를 본 순간 반드시 보이도록 순서를 못 박는다.
	 * 짝이 되는 load-acquire 는 vfio_main.c 의 vfio_device_fops 의 ioctl/read/write/mmap 과
	 * vfio_file_has_device_access 다. 배리어가 없으면 "권한은 켜졌는데 바인딩은 아직
	 * 안 보이는" 창이 생겨 NULL 역참조로 이어진다. */
	smp_store_release(&df->access_granted, true);

	/* [한국어] 안쪽 락부터 푼다. 잡은 역순으로 푸는 것이 규칙이다. */
	mutex_unlock(&device->dev_set->lock);
	/* [한국어] 바깥 락을 푼다. */
	mutex_unlock(&device->group->group_lock);
	/* [한국어] 성공. 호출자 vfio_device_open_file 이 이어서 anon inode 파일을 만든다. */
	return 0;

/* [한국어] IOAS attach 실패만 여기로 온다 — vfio_df_open 이 이미 성공했으므로 그것부터 되돌린다. */
out_close_device:
	/* [한국어] vfio_main.c 의 vfio_df_close. open_count 를 내리고 1→0 전이에서
	 * vfio_df_device_last_close 로 vendor close_device, IOMMU 결합 해제, module_put 을 한다.
	 * 이 호출 뒤 open_count 는 다시 0 이 되어 아래 KVM 되돌림 조건이 성립한다. */
	vfio_df_close(df);
/* [한국어] 권한 검사 실패와 vfio_df_open 실패가 합류하는 라벨. */
out_put_kvm:
	/* [한국어] fd 문맥에 복사해 둔 컨텍스트 포인터를 지운다. 참조를 우리가 가진 것이 아니므로
	 * put 은 하지 않는다 — 참조의 주인은 group 이다. */
	df->iommufd = NULL;
	/* [한국어] 아직 아무도 이 디바이스를 열고 있지 않을 때만 KVM 참조를 되돌린다.
	 * 다른 fd 가 이미 열려 있으면 그쪽이 계속 써야 하므로 놓으면 안 된다. */
	if (device->open_count == 0)
		/* [한국어] vfio_device_group_get_kvm_safe 가 잡은 참조를 놓는다. 빠뜨리면 게스트 VM 객체가
		 * 해제되지 못하고, KVM 모듈도 언로드되지 않는다. */
		vfio_device_put_kvm(device);
	/* [한국어] 안쪽 락 해제. */
	mutex_unlock(&device->dev_set->lock);
/* [한국어] vfio_group_has_iommu 실패만 여기로 온다 — 아직 안쪽 락을 잡기 전이다. */
out_unlock:
	/* [한국어] 바깥 락 해제. */
	mutex_unlock(&device->group->group_lock);
	/* [한국어] -EINVAL / -EPERM / vendor 오류 중 하나가 나간다. */
	return ret;
}

/* [한국어]
 * vfio_df_group_close - 1세대 group 경로에서 device fd 하나를 닫는다
 *
 * @df: 닫히는 fd 의 문맥.
 * @return: 없음. **닫기는 실패할 수 없다** — 되돌릴 곳이 없기 때문이다.
 *
 * 왜 필요한가: vfio_df_group_open 의 정확한 짝이다. 열 때 잡은 것(open_count,
 * KVM 참조)을 같은 락 아래에서, 같은 기준으로 되돌린다.
 *
 * 동작 과정:
 *  1. open 과 같은 순서로 group_lock → dev_set->lock 을 잡는다.
 *  2. vfio_df_close(vfio_main.c)로 open_count 를 내리고, 1→0 이면 vendor close_device,
 *     IOMMU 결합 해제(vfio_device_group_unuse_iommu), 모듈 참조 반환까지 처리한다.
 *  3. df->iommufd 를 비운다(참조의 주인은 group 이므로 put 은 하지 않는다).
 *  4. open_count 가 0 이 됐으면 KVM 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. device fd 의 마지막 fput 경로
 * (vfio_device_fops_release)와 vfio_device_open_file 의 실패 되돌림 두 곳에서 불린다.
 * 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_device_fops_release(vfio_main.c) 또는 vfio_device_open_file 의 err_close_device
 *     → [vfio_df_group_close] → vfio_df_close(vfio_main.c) / vfio_device_put_kvm
 */
void vfio_df_group_close(struct vfio_device_file *df)
{
	/* [한국어] 닫히는 fd 가 가리키던 디바이스. */
	struct vfio_device *device = df->device;

	/* [한국어] open 과 **같은 순서**로 락을 잡는다(group_lock → dev_set->lock). 순서가 어긋나면
	 * 교착이 생긴다. */
	mutex_lock(&device->group->group_lock);
	/* [한국어] open_count 를 내리는 동안 형제 디바이스와 직렬화한다. */
	mutex_lock(&device->dev_set->lock);

	/* [한국어] open_count 를 내리고 1→0 이면 vendor close_device 와 IOMMU 결합 해제, module_put 까지
	 * 한다. 1세대에서 여러 fd 가 열려 있었다면 마지막 것에서만 실제 정리가 일어난다. */
	vfio_df_close(df);
	/* [한국어] 빌려 쓰던 컨텍스트 포인터를 지운다. put 은 하지 않는다 — 참조의 주인은 group 이고
	 * group fd 가 닫힐 때 놓는다. */
	df->iommufd = NULL;

	/* [한국어] 이 디바이스를 여는 fd 가 하나도 남지 않았을 때만. */
	if (device->open_count == 0)
		/* [한국어] 첫 open 때 잡은 KVM 참조를 되돌린다. open/close 를 통틀어 정확히 한 번씩만 실행된다. */
		vfio_device_put_kvm(device);

	/* [한국어] 잡은 역순으로 푼다. */
	mutex_unlock(&device->dev_set->lock);
	/* [한국어] 바깥 락 해제. 반환값이 없다 — 닫기는 실패할 수 없다. */
	mutex_unlock(&device->group->group_lock);
}

/* [한국어]
 * vfio_device_open_file - 디바이스를 열고 그것을 가리키는 익명 inode 파일을 만든다
 *
 * @device: 이미 등록 참조를 1 잡아 둔 디바이스(vfio_device_get_from_name 의 결과).
 * @return: 성공 시 struct file 포인터. 실패 시 ERR_PTR.
 *
 * 왜 필요한가: 1세대에서 device fd 는 /dev 아래 노드가 아니라 group fd 의 ioctl 이
 * 만들어 주는 익명 파일이다. 그 파일을 만들고, 열기 절차를 수행하고, mmap 을 위한
 * address_space 를 연결하는 세 가지를 한 함수에 모았다.
 *
 * 동작 과정:
 *  1. vfio_allocate_device_file 로 df 를 할당한다.
 *  2. df->group 을 채워 이 fd 가 1세대임을 표시한다(vfio_main.c 는 이 필드로 세대를 가른다).
 *  3. vfio_df_group_open 으로 실제 열기 절차를 수행한다.
 *  4. anon_inode_getfile_fmode 로 vfio_device_fops 를 단 파일을 만든다.
 *     **1세대와 2세대가 같은 fops 를 공유**하므로 열린 뒤의 동작은 동일하다.
 *  5. f_mapping 을 device 전용 pseudo-fs inode 의 것으로 바꿔, 이 디바이스의 모든 mmap 이
 *     한 address_space 에 매달리게 한다(리셋/제거 시 일괄 unmap 을 위해).
 *  6. noiommu 면 커널 로그에 경고를 남긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). 메모리 할당과 뮤텍스가 있어 잠들 수 있다.
 *
 * 에러 경로: 세 라벨. err_close_device 는 vfio_df_group_open 을 되돌리고,
 * err_free 는 df 를 해제하며, err_out 은 ERR_PTR 을 만든다.
 * **디바이스 등록 참조는 어느 경로에서도 여기서 놓지 않는다** — 호출자가 놓는다.
 *
 * refcount: 성공하면 등록 참조의 소유권이 struct file 로 넘어가고
 * vfio_device_fops_release 가 놓는다. 상류 주석이 그 이동을 명시하고 있다.
 *
 * 호출 체인:
 *   vfio_group_ioctl_get_device_fd → [vfio_device_open_file]
 *     → vfio_allocate_device_file(vfio_main.c) / vfio_df_group_open /
 *       anon_inode_getfile_fmode / vfio_df_group_close(실패 시)
 */
static struct file *vfio_device_open_file(struct vfio_device *device)
{
	/* [한국어] 이 fd 의 문맥 객체. 성공하면 struct file 의 private_data 가 된다. */
	struct vfio_device_file *df;
	/* [한국어] 만들어질 익명 inode 파일. */
	struct file *filep;
	/* [한국어] 오류 코드 보관용. */
	int ret;

	/* [한국어] vfio_main.c 의 vfio_allocate_device_file. df 를 kzalloc 하고 df->device 를 채운다.
	 * **device 의 등록 참조를 새로 잡지는 않는다** — 호출자가 이미
	 * vfio_device_get_from_name 에서 잡아 온 그 참조를 그대로 물려받는다. */
	df = vfio_allocate_device_file(device);
	/* [한국어] 할당 실패. */
	if (IS_ERR(df)) {
		/* [한국어] -ENOMEM 등을 꺼낸다. */
		ret = PTR_ERR(df);
		/* [한국어] 아무것도 만들지 않았으므로 그대로 ERR_PTR 로 나간다. */
		goto err_out;
	}

	/* [한국어] 이 fd 가 1세대(group) 경로임을 표시한다. vfio_main.c 는 df->group 이 NULL 인지로
	 * 1세대와 2세대를 가르므로, 이 한 줄이 세대 표식이다. */
	df->group = device->group;

	/* [한국어] 위에서 본 첫 open 절차 전체를 수행한다. */
	ret = vfio_df_group_open(df);
	/* [한국어] IOMMU 결합이나 vendor open 이 실패했다. */
	if (ret)
		/* [한국어] df 만 해제하는 라벨로 간다. */
		goto err_free;

	/* [한국어] 파일시스템에 이름이 없는 익명 inode 위에 struct file 을 만든다. 첫 인자는
	 * /proc/<pid>/fd 에 보이는 표시 이름이고, fops 는 vfio_main.c 의 vfio_device_fops 다.
	 * **1세대 group 경로와 2세대 cdev 경로가 같은 fops 를 공유한다** — 열린 뒤의 동작은
	 * 완전히 같고 여는 방법만 다르다. df 가 private_data 가 되고, O_RDWR 에 더해
	 * FMODE_PREAD/FMODE_PWRITE 를 줘 pread/pwrite 를 허용한다. */
	filep = anon_inode_getfile_fmode("[vfio-device]", &vfio_device_fops,
				   df, O_RDWR, FMODE_PREAD | FMODE_PWRITE);
	/* [한국어] 파일 생성 실패(fd 부족이 아니라 inode/파일 할당 실패). */
	if (IS_ERR(filep)) {
		/* [한국어] 오류 코드를 꺼낸다. */
		ret = PTR_ERR(filep);
		/* [한국어] 이미 열어 둔 디바이스를 닫는 라벨로 간다. */
		goto err_close_device;
	}
	/*
	 * Use the pseudo fs inode on the device to link all mmaps
	 * to the same address space, allowing us to unmap all vmas
	 * associated to this device using unmap_mapping_range().
	 */
	/* [한국어] 이 파일의 주소공간을 device 전용 pseudo-fs inode 의 것으로 바꾼다. 같은 디바이스를
	 * 여러 번 mmap 해도 모두 한 address_space 에 매달리므로, 나중에
	 * unmap_mapping_range 한 번으로 그 디바이스의 모든 사용자 매핑을 걷어낼 수 있다.
	 * 디바이스가 리셋되거나 뽑힐 때 사용자에게 남은 BAR 매핑을 끊는 근거다. */
	filep->f_mapping = device->inode->i_mapping;

	/* [한국어] 격리 없이 열린 디바이스라면. */
	if (device->group->type == VFIO_NO_IOMMU)
		/* [한국어] 커널 로그에 누가 열었는지 남긴다. noiommu 는 커널 taint 를 동반하는 위험 모드라
		 * 사후 추적을 위해 프로세스 이름과 PID 를 함께 기록한다. */
		dev_warn(device->dev, "vfio-noiommu device opened by user "
			 "(%s:%d)\n", current->comm, task_pid_nr(current));
	/*
	 * On success the ref of device is moved to the file and
	 * put in vfio_device_fops_release()
	 */
	/* [한국어] 성공. 호출자가 이 파일을 fd 에 설치한다. 디바이스 등록 참조는 이 파일이 소유하게 되고
	 * vfio_device_fops_release 가 놓는다. */
	return filep;

/* [한국어] anon inode 파일 생성 실패만 여기로 온다. */
err_close_device:
	/* [한국어] 열어 둔 디바이스를 닫는다. vfio_df_group_open 의 정확한 짝이다. */
	vfio_df_group_close(df);
/* [한국어] vfio_df_group_open 실패가 합류한다 — 이 시점에는 device 가 열려 있지 않다. */
err_free:
	/* [한국어] df 를 해제한다. 성공 경로에서는 file 이 소유하며 vfio_device_fops_release 가
	 * 해제한다. */
	kfree(df);
/* [한국어] df 할당 실패가 합류한다. */
err_out:
	/* [한국어] 오류를 포인터로 감싸 돌려준다. **디바이스 등록 참조는 여기서 놓지 않는다** —
	 * 호출자 vfio_group_ioctl_get_device_fd 가 fd < 0 을 보고 놓는다. */
	return ERR_PTR(ret);
}

/* [한국어]
 * vfio_group_ioctl_get_device_fd - VFIO_GROUP_GET_DEVICE_FD ioctl 핸들러
 *
 * @group: group fd 의 private_data 에서 온 group.
 * @arg: 사용자 공간의 문자열 포인터(디바이스 이름).
 * @return: 성공 시 0 이상의 새 fd 번호. 실패 시 음수 오류 —
 *          -EFAULT/-ENAMETOOLONG(이름 복사 실패), -ENODEV(그런 디바이스 없음),
 *          그 밖에 열기 과정의 오류.
 *
 * 왜 필요한가: **1세대에서 device fd 를 얻는 유일한 통로**다. group fd 를 가졌다는 것이
 * 곧 그 group 안 모든 디바이스에 대한 권한이라는 VFIO 권한 모델이 여기에 그대로 나타난다.
 *
 * 동작 과정:
 *  1. strndup_user 로 이름을 커널로 복사한다(PAGE_SIZE 로 잘라 무한 문자열을 막는다).
 *  2. vfio_device_get_from_name 으로 디바이스를 찾고 등록 참조를 1 잡는다.
 *  3. 이름 버퍼를 해제한다.
 *  4. FD_ADD 로 vfio_device_open_file 이 만든 파일을 새 fd 에 설치한다. O_CLOEXEC 를 줘
 *     exec 때 자동으로 닫히게 한다.
 *  5. 실패하면 2단계에서 잡은 등록 참조를 그 자리에서 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). 사용자 메모리 복사와 할당이 있어 잠들 수 있다.
 * group->group_lock 은 잡지 않는다 — 필요한 직렬화는 하위 함수들이 각자 한다.
 *
 * 에러 경로: 1~2단계 실패는 잡은 것이 없으므로 그대로 반환한다. 4단계 실패는
 * 5단계가 참조를 되돌린다.
 *
 * refcount: 2단계의 +1 이 성공하면 struct file 로, 실패하면 5단계로 소비된다.
 * 어느 쪽도 하지 않으면 vfio_unregister_group_dev 가 영원히 잠든다.
 *
 * 호출 체인:
 *   ioctl(VFIO_GROUP_GET_DEVICE_FD) → vfio_group_fops_unl_ioctl
 *     → [vfio_group_ioctl_get_device_fd]
 *     → strndup_user / vfio_device_get_from_name / vfio_device_open_file /
 *       vfio_device_put_registration(vfio_main.c)
 */
static int vfio_group_ioctl_get_device_fd(struct vfio_group *group,
					  char __user *arg)
{
	/* [한국어] 이름으로 찾아낸 디바이스. 찾으면 등록 참조를 하나 쥔 상태다. */
	struct vfio_device *device;
	/* [한국어] 사용자가 준 디바이스 이름 문자열의 커널 복사본. */
	char *buf;
	/* [한국어] 새로 만든 fd 번호 또는 음수 오류. */
	int fd;

	/* [한국어] 사용자 공간 문자열을 커널로 복사한다. 최대 PAGE_SIZE 로 잘라 무한 문자열 공격을
	 * 막고, NUL 종료를 보장하며, 버퍼는 kmalloc 으로 할당된다. */
	buf = strndup_user(arg, PAGE_SIZE);
	/* [한국어] 복사 실패(EFAULT) 또는 길이 초과(ENAMETOOLONG). */
	if (IS_ERR(buf))
		/* [한국어] 그대로 오류 반환. */
		return PTR_ERR(buf);

	/* [한국어] 이 group 안에서 그 이름의 디바이스를 찾고 등록 참조를 하나 잡는다. */
	device = vfio_device_get_from_name(group, buf);
	/* [한국어] 이름 버퍼는 더 필요 없다. 성공/실패와 무관하게 여기서 해제한다. */
	kfree(buf);
	/* [한국어] 그런 이름의 디바이스가 없거나 vendor 의 match 가 오류를 냈다. */
	if (IS_ERR(device))
		/* [한국어] 참조를 잡은 것이 없으므로 그냥 반환한다. */
		return PTR_ERR(device);

	/* [한국어] 파일을 만들어 새 fd 에 설치한다. O_CLOEXEC 는 exec 시 이 fd 를 자동으로 닫게 해,
	 * 자식 프로세스로 디바이스 접근 권한이 새어 나가지 않게 한다.
	 * 인자로 준 vfio_device_open_file(device) 가 먼저 평가되어 struct file 또는 ERR_PTR 을
	 * 만들고, FD_ADD 가 그것을 fd 로 바꾼다. 성공하면 fd 번호, 실패하면 음수 오류다.
	 * FD_ADD 의 정의는 include/linux/file.h 에 있고 그 헤더는 이 트리에 없어 확인 못 함. */
	fd = FD_ADD(O_CLOEXEC, vfio_device_open_file(device));
	/* [한국어] 파일 생성이나 fd 할당이 실패했다. */
	if (fd < 0)
		/* [한국어] vfio_device_get_from_name 이 잡아 둔 등록 참조를 되돌린다. 성공했다면 이 참조는
		 * struct file 이 소유하고 vfio_device_fops_release 가 놓는다.
		 * 이 줄이 없으면 실패할 때마다 refcount 가 새어 vfio_unregister_group_dev 가
		 * 영원히 잠든다. */
		vfio_device_put_registration(device);
	/* [한국어] fd 번호(0 이상) 또는 오류가 그대로 ioctl 반환값이 된다. */
	return fd;
}

/* [한국어]
 * vfio_group_ioctl_get_status - VFIO_GROUP_GET_STATUS ioctl 핸들러
 *
 * @group: group fd 의 private_data 에서 온 group.
 * @arg: 사용자 공간의 struct vfio_group_status 포인터. argsz 를 사용자가 채워 온다.
 * @return: 0 성공. -EFAULT(사용자 포인터 오류), -EINVAL(argsz 가 최소보다 작음),
 *          -ENODEV(group 이 이미 무효화됨).
 *
 * 왜 필요한가: 사용자 공간이 group 을 열자마자 "이 group 을 쓸 수 있는가" 를 묻는
 * 표준 절차다. VIABLE 이 서지 않으면 group 안에 아직 커널 드라이버에 잡힌 디바이스가
 * 있다는 뜻이라, 사용자는 그것들을 먼저 unbind 해야 한다.
 *
 * 동작 과정:
 *  1. offsetofend 로 커널이 요구하는 최소 크기를 계산하고 사용자 구조체를 그만큼 읽는다.
 *  2. argsz 가 그보다 작으면 -EINVAL(ABI 확장 규약).
 *  3. flags 를 0 에서 시작한다.
 *  4. group_lock 아래에서 iommu_group 유효성을 확인하고,
 *     이미 결합돼 있으면 CONTAINER_SET 과 VIABLE 을 함께,
 *     아직이면 iommu_group_dma_owner_claimed 가 거짓일 때만 VIABLE 을 세운다.
 *  5. 결과를 사용자 버퍼로 되돌려 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). copy_from_user/copy_to_user 는 락 밖에서 한다.
 *
 * 에러 경로: 모든 실패가 상태를 바꾸지 않는다.
 *
 * 주의: 상류 주석이 지적하듯 iommufd 호환 경로에서는 iommu_group_claim_dma_owner 가
 * SET_CONTAINER 가 아니라 GET_DEVICE_FD 시점에 일어나므로, VIABLE 이 서 있어도
 * GET_DEVICE_FD 가 viability 때문에 실패할 수 있다.
 *
 * 호출 체인:
 *   ioctl(VFIO_GROUP_GET_STATUS) → vfio_group_fops_unl_ioctl
 *     → [vfio_group_ioctl_get_status] → vfio_group_has_iommu / iommu_group_dma_owner_claimed
 */
static int vfio_group_ioctl_get_status(struct vfio_group *group,
				       struct vfio_group_status __user *arg)
{
	/* [한국어] 사용자가 최소한 채워 줘야 하는 크기 = argsz 와 flags 까지의 오프셋. offsetofend 는
	 * "그 필드의 끝까지" 를 준다. VFIO ABI 는 구조체 뒤에 필드를 덧붙여 확장하므로,
	 * 커널은 자신이 아는 최소 크기만 읽고 쓴다. */
	unsigned long minsz = offsetofend(struct vfio_group_status, flags);
	/* [한국어] 커널 쪽 사본. 사용자 메모리를 직접 다루지 않기 위한 중간 버퍼다. */
	struct vfio_group_status status;

	/* [한국어] argsz 와 flags 만 복사해 온다. */
	if (copy_from_user(&status, arg, minsz))
		/* [한국어] 사용자 포인터가 잘못됐다. */
		return -EFAULT;

	/* [한국어] 사용자가 선언한 구조체 크기가 커널이 요구하는 최소보다 작다 — 오래된/잘못된 ABI 사용. */
	if (status.argsz < minsz)
		/* [한국어] -EINVAL 로 거절한다. */
		return -EINVAL;

	/* [한국어] 커널이 채우는 필드이므로 사용자가 넣은 값을 무시하고 0 에서 시작한다. */
	status.flags = 0;

	/* [한국어] iommu_group 과 결합 상태를 함께 보는 동안 바뀌지 않게 막는다. */
	mutex_lock(&group->group_lock);
	/* [한국어] 마지막 디바이스가 빠져 group 이 이미 무효화된 상태다. */
	if (!group->iommu_group) {
		/* [한국어] 이 경로는 goto 를 쓰지 않고 직접 푼다. */
		mutex_unlock(&group->group_lock);
		/* [한국어] -ENODEV. */
		return -ENODEV;
	}

	/*
	 * With the container FD the iommu_group_claim_dma_owner() is done
	 * during SET_CONTAINER but for IOMMFD this is done during
	 * VFIO_GROUP_GET_DEVICE_FD. Meaning that with iommufd
	 * VFIO_GROUP_FLAGS_VIABLE could be set but GET_DEVICE_FD will fail due
	 * to viability.
	 */
	/* [한국어] 이미 컨테이너나 iommufd 에 붙어 있다면. */
	if (vfio_group_has_iommu(group))
		/* [한국어] 두 플래그를 함께 세운다. CONTAINER_SET(비트 1) = 결합됨,
		 * VIABLE(비트 0) = 이 group 을 사용자에게 줄 수 있다. 이미 결합에 성공했다는 것은
		 * 그때 viability 검사를 통과했다는 뜻이므로 둘을 같이 세운다. 값은
		 * include/uapi/linux/vfio.h:150~151 에 정의돼 있다. */
		status.flags |= VFIO_GROUP_FLAGS_CONTAINER_SET |
				VFIO_GROUP_FLAGS_VIABLE;
	/* [한국어] 아직 결합 전이면, group 안 어떤 디바이스도 다른 커널 드라이버에 잡혀 있지 않은지로
	 * 판단한다. iommu_group_dma_owner_claimed 는 IOMMU 코어가 관리하는 "이 group 의 DMA
	 * 소유권을 이미 누가 가져갔는가" 플래그를 본다. 구현은 drivers/iommu 아래라 이
	 * 트리에서 확인 못 함. */
	else if (!iommu_group_dma_owner_claimed(group->iommu_group))
		/* [한국어] 소유권이 비어 있으니 사용자가 가져갈 수 있다 — VIABLE 만 세운다. */
		status.flags |= VFIO_GROUP_FLAGS_VIABLE;
	/* [한국어] 임계구역을 닫는다. 아래 사용자 복사는 잠들 수 있으므로 락 밖에서 한다. */
	mutex_unlock(&group->group_lock);

	/* [한국어] 채운 상태를 사용자 버퍼로 되돌려 준다. minsz 만큼만 쓴다. */
	if (copy_to_user(arg, &status, minsz))
		/* [한국어] 사용자 포인터가 잘못됐다. */
		return -EFAULT;
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * vfio_group_fops_unl_ioctl - group fd 의 ioctl 디스패처
 *
 * @filep: group fd 의 struct file. private_data 에 group 포인터가 들어 있다.
 * @cmd: VFIO_GROUP_ 계열 명령 코드.
 * @arg: 사용자 공간 포인터를 담은 정수(명령에 따라 쓰이지 않을 수도 있다).
 * @return: 각 핸들러의 반환값. 모르는 명령은 -ENOTTY.
 *
 * 왜 필요한가: group fd 가 받는 네 가지 명령을 한 곳에서 갈라 준다. device fd 쪽
 * 디스패처(vfio_main.c 의 vfio_device_fops_unl_ioctl)와 달리 **권한 게이트가 없다** —
 * group fd 를 여는 것 자체(vfio_group_fops_open)가 이미 권한 검사였기 때문이다.
 *
 * 동작 과정: private_data 에서 group 을 꺼내고, arg 를 __user 포인터로 캐스팅한 뒤
 * cmd 에 따라 네 핸들러 중 하나로 보낸다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 여러 스레드가 같은 group fd 에 동시에 진입할 수 있고,
 * 직렬화는 각 핸들러가 group->group_lock 으로 한다.
 *
 * 에러 경로: 모르는 명령은 -ENOTTY. 사용자 공간은 이 값으로 커널의 기능 유무를 탐지한다.
 *
 * 호출 체인:
 *   ioctl(2) → VFS → [vfio_group_fops_unl_ioctl]
 *     → vfio_group_ioctl_get_device_fd / _get_status / _set_container / _unset_container
 */
static long vfio_group_fops_unl_ioctl(struct file *filep,
				      unsigned int cmd, unsigned long arg)
{
	/* [한국어] vfio_group_fops_open 이 넣어 둔 group 포인터. 이 fd 가 살아 있는 동안 group 도
	 * 살아 있으므로 별도 참조가 필요 없다. */
	struct vfio_group *group = filep->private_data;
	/* [한국어] 정수로 전달된 사용자 포인터를 __user 표시가 붙은 포인터로 바꾼다. sparse 정적
	 * 검사기가 커널/사용자 포인터 혼동을 잡아낼 수 있게 하는 주석성 캐스트다. */
	void __user *uarg = (void __user *)arg;

	/* [한국어] group fd 가 받는 ioctl 은 네 종류뿐이다. */
	switch (cmd) {
	/* [한국어] 이름으로 디바이스를 찾아 새 fd 를 준다. 1세대에서 device fd 를 얻는 유일한 길이다. */
	case VFIO_GROUP_GET_DEVICE_FD:
		/* [한국어] 핸들러로 넘긴다. */
		return vfio_group_ioctl_get_device_fd(group, uarg);
	/* [한국어] 이 group 의 결합 상태와 viability 를 알려 준다. */
	case VFIO_GROUP_GET_STATUS:
		/* [한국어] 핸들러로 넘긴다. */
		return vfio_group_ioctl_get_status(group, uarg);
	/* [한국어] 컨테이너 fd 또는 /dev/iommu fd 를 이 group 에 결합한다. */
	case VFIO_GROUP_SET_CONTAINER:
		/* [한국어] 핸들러로 넘긴다. */
		return vfio_group_ioctl_set_container(group, uarg);
	/* [한국어] 결합을 푼다. 인자가 없다. */
	case VFIO_GROUP_UNSET_CONTAINER:
		/* [한국어] 핸들러로 넘긴다. */
		return vfio_group_ioctl_unset_container(group);
	/* [한국어] 그 밖의 명령. */
	default:
		/* [한국어] -ENOTTY 는 "이 파일은 그런 ioctl 을 모른다" 는 표준 답이다. 사용자 공간은 이것으로
		 * 기능 유무를 탐지한다. */
		return -ENOTTY;
	}
}

/* [한국어]
 * vfio_device_block_group - 2세대 cdev 경로가 이 디바이스를 잡는 동안 legacy group 노드를 막는다
 *
 * @device: 지금 BIND_IOMMUFD 로 열리려는 디바이스.
 * @return: 0 성공(카운트 증가). -EBUSY 면 이미 legacy group 노드가 열려 있어 막을 수 없다.
 *
 * 왜 필요한가: 한 group 을 1세대(group fd + container)와 2세대(cdev + iommufd)가 동시에
 * 쓰면 IOVA 주소공간의 주인이 둘이 되어 격리가 깨진다. 그래서 두 세대가 **상호 배타**여야
 * 하고, 이 함수가 2세대 → 1세대 방향의 빗장이다. 반대 방향 빗장은
 * vfio_group_fops_open 의 cdev_device_open_cnt 검사다.
 *
 * 동작 과정: group_lock 을 잡고 group->opened_file 이 있는지 본다. 있으면 이미 1세대가
 * 쓰고 있으므로 -EBUSY. 없으면 cdev_device_open_cnt 를 1 늘린다.
 *
 * 실행 컨텍스트: 프로세스 문맥(device cdev fd 의 BIND_IOMMUFD ioctl). 뮤텍스를 잡는다.
 *
 * 에러 경로: -EBUSY 만 있고 아무 상태도 바꾸지 않는다.
 *
 * 카운터: 여기서 늘린 값의 짝은 vfio_device_unblock_group 의 감소이며,
 * device_cdev.c:169(bind 실패 되돌림)와 :191(unbind)에서 부른다. 짝이 어긋나면
 * legacy group 노드가 영원히 -EBUSY 이고 vfio_group_release 의 WARN_ON 이 터진다.
 *
 * 호출 체인:
 *   ioctl(VFIO_DEVICE_BIND_IOMMUFD) → vfio_df_ioctl_bind_iommufd(device_cdev.c:113)
 *     → [vfio_device_block_group]
 */
int vfio_device_block_group(struct vfio_device *device)
{
	/* [한국어] 이 디바이스가 속한 group. */
	struct vfio_group *group = device->group;
	/* [한국어] 성공이면 0 그대로 나간다. */
	int ret = 0;

	/* [한국어] opened_file 과 cdev_device_open_cnt 를 함께 보는 동안 직렬화한다. */
	mutex_lock(&group->group_lock);
	/* [한국어] 이 group 의 legacy 노드(/dev/vfio/<gid>)가 이미 열려 있다. 1세대와 2세대가 같은
	 * group 을 동시에 쓰면 IOVA 주소공간의 주인이 둘이 되므로 금지한다. */
	if (group->opened_file) {
		/* [한국어] -EBUSY 로 2세대 bind 를 거절한다. */
		ret = -EBUSY;
		/* [한국어] 락 해제 출구. */
		goto out_unlock;
	}

	/* [한국어] 2세대(cdev) 경로가 이 group 의 디바이스를 하나 잡았다고 세어 둔다.
	 * 이 카운트가 0 이 아니면 vfio_group_fops_open 이 legacy 노드 열기를 -EBUSY 로 막는다.
	 * 짝은 vfio_device_unblock_group 의 -- 이며, device_cdev.c:169 와 :191 에서 부른다.
	 * 빠뜨리면 group 노드가 영원히 열리지 않고, vfio_group_release 의
	 * WARN_ON(group->cdev_device_open_cnt) 이 터진다. */
	group->cdev_device_open_cnt++;

/* [한국어] 성공과 실패가 합류하는 출구. */
out_unlock:
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&group->group_lock);
	/* [한국어] 0 또는 -EBUSY. */
	return ret;
}

/* [한국어]
 * vfio_device_unblock_group - vfio_device_block_group 의 짝, 빗장을 푼다
 *
 * @device: 2세대 경로에서 놓여나는 디바이스.
 * @return: 없음. 실패할 수 없다.
 *
 * 왜 필요한가: 2세대 bind 가 실패하거나 unbind 될 때 legacy group 노드를 다시 열 수
 * 있게 해 줘야 한다. 이것을 빠뜨리면 그 group 의 /dev/vfio/<gid> 가 영원히 -EBUSY 가
 * 되고, group 이 해제될 때 vfio_group_release 의 WARN_ON(cdev_device_open_cnt) 이 터진다.
 *
 * 동작 과정: group_lock 을 잡고 cdev_device_open_cnt 를 1 줄인다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_df_ioctl_bind_iommufd 의 실패 되돌림(device_cdev.c:169) 또는
 *   vfio_df_unbind_iommufd(device_cdev.c:191) → [vfio_device_unblock_group]
 */
void vfio_device_unblock_group(struct vfio_device *device)
{
	/* [한국어] 이 디바이스가 속한 group. */
	struct vfio_group *group = device->group;

	/* [한국어] 카운터를 만지는 동안 직렬화한다. */
	mutex_lock(&group->group_lock);
	/* [한국어] vfio_device_block_group 의 ++ 를 정확히 되돌린다. 0 이 되면 legacy group 노드를
	 * 다시 열 수 있다. */
	group->cdev_device_open_cnt--;
	/* [한국어] 임계구역을 닫는다. 반환값이 없다 — 실패할 수 없다. */
	mutex_unlock(&group->group_lock);
}

/* [한국어]
 * vfio_group_fops_open - /dev/vfio/<groupID> 를 열 때의 네 가지 게이트
 *
 * @inode: 열리는 캐릭터 디바이스의 inode. i_cdev 에서 group 을 되찾는다.
 * @filep: 새로 만들어지는 struct file. private_data 에 group 을 심는다.
 * @return: 0 성공. -ENODEV(group 이 사라지는 중), -EPERM(noiommu 인데 RAWIO 없음),
 *          -EBUSY(2세대가 쓰는 중이거나 이미 열려 있음).
 *
 * 왜 필요한가: group fd 를 갖는다는 것이 곧 그 group 안 모든 디바이스에 대한 권한이므로,
 * **open 그 자체가 VFIO 의 권한 검사 지점**이다. 이 함수를 통과한 뒤의 ioctl 들은
 * 추가 권한 검사를 하지 않는다.
 *
 * 동작 과정(모두 group_lock 아래):
 *  1. refcount_read(drivers) == 0 이면 vfio_device_remove_group 이 진행 중이라 -ENODEV.
 *     상류 주석대로 그 함수가 락을 잡고 진행하므로 여기서 본 0 은 안정된 값이다.
 *  2. noiommu group 이면 CAP_SYS_RAWIO 를 요구한다.
 *  3. cdev_device_open_cnt 가 0 이 아니면 2세대가 쓰는 중이라 -EBUSY.
 *  4. opened_file 이 이미 있으면 -EBUSY(single opener).
 *  5. opened_file 과 private_data 를 채운다.
 *
 * 실행 컨텍스트: 프로세스 문맥(open 시스템 콜). 뮤텍스를 잡는다.
 *
 * 에러 경로: 네 실패가 모두 out_unlock 으로 합류하며 아무 상태도 바꾸지 않는다.
 *
 * noiommu: 2단계가 noiommu 를 막는 첫 관문이다. 같은 CAP_SYS_RAWIO 검사가
 * container.c 의 vfio_noiommu_open, vfio_container_attach_group,
 * vfio_group_use_container 에도 반복돼 있어, 어느 경로로 들어오든 권한 없이는
 * noiommu 를 쓸 수 없다.
 *
 * 호출 체인:
 *   open("/dev/vfio/<gid>") → VFS → cdev → [vfio_group_fops_open]
 */
static int vfio_group_fops_open(struct inode *inode, struct file *filep)
{
	/* [한국어] 커널이 준 inode 의 i_cdev 로부터 그것을 감싸고 있는 vfio_group 을 되찾는다.
	 * container_of 는 멤버 주소에서 구조체 시작 주소를 빼는 컴파일 시 계산이라
	 * 런타임 비용이 없다. group->cdev 는 vfio_group_alloc 에서 cdev_init 으로 초기화됐다. */
	struct vfio_group *group =
		container_of(inode->i_cdev, struct vfio_group, cdev);
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] 아래 네 가지 검사와 opened_file 대입 전체를 한 임계구역에 넣는다. */
	mutex_lock(&group->group_lock);

	/*
	 * drivers can be zero if this races with vfio_device_remove_group(), it
	 * will be stable at 0 under the group rwsem
	 */
	/* [한국어] drivers 는 이 group 에 붙어 있는 vfio 드라이버 인스턴스 수다.
	 * 0 이면 vfio_device_remove_group 이 마지막 디바이스를 떼는 중이라는 뜻이고,
	 * 그 함수는 refcount_dec_and_mutex_lock 으로 vfio.group_lock 을 잡은 뒤 진행하므로
	 * 여기서 본 0 은 안정된 값이다. */
	if (refcount_read(&group->drivers) == 0) {
		/* [한국어] 곧 사라질 group 이므로 -ENODEV. */
		ret = -ENODEV;
		/* [한국어] 락 해제 출구. */
		goto out_unlock;
	}

	/* [한국어] noiommu group 은 IOMMU 격리가 전혀 없는 물리 디바이스를 그대로 넘기는 모드라
	 * CAP_SYS_RAWIO 를 요구한다. 이 게이트가 noiommu 를 막는 첫 관문이고,
	 * container.c 의 vfio_noiommu_open 과 vfio_container_attach_group,
	 * vfio_group_use_container 에도 같은 검사가 반복된다. */
	if (group->type == VFIO_NO_IOMMU && !capable(CAP_SYS_RAWIO)) {
		/* [한국어] 권한 부족. */
		ret = -EPERM;
		/* [한국어] 락 해제 출구. */
		goto out_unlock;
	}

	/* [한국어] 2세대 cdev 경로가 이 group 의 디바이스를 잡고 있다. 두 세대가 한 group 을 동시에
	 * 쓰지 못하게 하는 반대 방향 게이트다(vfio_device_block_group 참조). */
	if (group->cdev_device_open_cnt) {
		/* [한국어] -EBUSY. */
		ret = -EBUSY;
		/* [한국어] 락 해제 출구. */
		goto out_unlock;
	}

	/*
	 * Do we need multiple instances of the group open?  Seems not.
	 */
	/* [한국어] group 노드는 동시에 하나만 열 수 있다. 여러 프로세스가 같은 group 을 열면
	 * container 결합 상태를 서로 덮어쓰게 되므로 원천 차단한다. */
	if (group->opened_file) {
		/* [한국어] -EBUSY. */
		ret = -EBUSY;
		/* [한국어] 락 해제 출구. */
		goto out_unlock;
	}
	/* [한국어] 이 fd 를 group 의 유일한 opener 로 기록한다. 이 포인터는 두 가지로 쓰인다 —
	 * 위 single-opener 게이트, 그리고 container.c:517 의 get_file(group->opened_file) 이
	 * device fd 마다 이 파일의 참조를 하나씩 더 잡는 대상.
	 * 그 덕분에 device fd 가 하나라도 열려 있으면 group fd 의 release 가 불리지 않는다. */
	group->opened_file = filep;
	/* [한국어] 이후 ioctl 이 이 fd 에서 group 을 되찾을 수 있게 한다. */
	filep->private_data = group;
	/* [한국어] 성공. */
	ret = 0;
/* [한국어] 네 실패와 성공이 모두 합류하는 출구. */
out_unlock:
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&group->group_lock);
	/* [한국어] 0 또는 -ENODEV/-EPERM/-EBUSY. */
	return ret;
}

/* [한국어]
 * vfio_group_fops_release - group fd 의 마지막 참조가 사라질 때 결합을 모두 푼다
 *
 * @inode: 닫히는 노드의 inode(쓰이지 않는다).
 * @filep: 닫히는 파일. private_data 에서 group 을 되찾는다.
 * @return: 항상 0. 닫기는 실패할 수 없다.
 *
 * 왜 필요한가: 사용자가 group fd 를 놓으면 그 group 은 초기 상태로 돌아가야 한다.
 * 컨테이너 결합, iommufd 컨텍스트 참조, single-opener 표식 세 가지를 여기서 정리한다.
 *
 * 동작 과정:
 *  1. private_data 를 먼저 끊는다.
 *  2. group_lock 아래에서 container 가 있으면 vfio_group_detach_container(container.c),
 *     iommufd 가 있으면 iommufd_ctx_put 후 NULL.
 *  3. opened_file 을 비워 다른 프로세스가 이 group 을 열 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(마지막 fput). 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 수명: 상류 주석이 "device fd 가 group 파일 참조를 쥐고 있으므로 여기 도달했다는 것은
 * 열린 디바이스가 없다는 뜻" 이라고 밝힌다. 그 참조를 거는 곳이
 * container.c:517 의 get_file(group->opened_file) — 즉 **1세대 컨테이너 경로**다.
 * 그래서 container 를 뗄 때 container_users 검사 없이 바로 detach 할 수 있다.
 * [상류 코드 관찰] iommufd 호환 경로(group->iommufd 만 설정된 경우)에는 그 get_file 이
 * 없다 — 이 트리에서 get_file(group->opened_file) 은 container.c:517 한 곳뿐이다.
 * iommufd 컨텍스트의 수명 규칙(drivers/iommu 아래)은 이 트리에 없어 그 경로의
 * 안전성은 확인 못 함. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   close(group fd) → VFS 마지막 fput → [vfio_group_fops_release]
 *     → vfio_group_detach_container(container.c) / iommufd_ctx_put
 */
static int vfio_group_fops_release(struct inode *inode, struct file *filep)
{
	/* [한국어] open 때 넣어 둔 group 포인터를 되찾는다. */
	struct vfio_group *group = filep->private_data;

	/* [한국어] 먼저 끊어 둔다. 이 뒤로는 이 파일로 group 에 접근할 수 없다. */
	filep->private_data = NULL;

	/* [한국어] 결합 해제와 opened_file 지우기를 한 임계구역에서 한다. */
	mutex_lock(&group->group_lock);
	/*
	 * Device FDs hold a group file reference, therefore the group release
	 * is only called when there are no open devices.
	 */
	/* [한국어] 1세대 컨테이너에 붙어 있었다면 여기서 뗀다. 상류 주석대로 device fd 들이 이 파일의
	 * 참조를 쥐고 있으므로(container.c:517 의 get_file), 여기 도달했다는 것은 이미 모든
	 * device fd 가 닫혔다는 뜻이다 — 그래서 container_users 검사 없이 바로 뗀다. */
	if (group->container)
		/* [한국어] container.c:468. 백엔드 detach_group(type1 이면 vfio_iommu_type1_detach_group),
		 * iommu_group_release_dma_owner, 마지막 group 이면 백엔드 release 와 module_put,
		 * 그리고 vfio_container_put 까지 한다. */
		vfio_group_detach_container(group);
	/* [한국어] 2세대 호환 모드로 iommufd 에 붙어 있었다면. */
	if (group->iommufd) {
		/* [한국어] SET_CONTAINER 가 잡아 둔 컨텍스트 참조를 놓는다. group 이 그 참조의 유일한 주인이다. */
		iommufd_ctx_put(group->iommufd);
		/* [한국어] 포인터를 지워 되쓰기를 막는다. */
		group->iommufd = NULL;
	}
	/* [한국어] 이제 다른 프로세스가 이 group 노드를 열 수 있다. */
	group->opened_file = NULL;
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&group->group_lock);
	/* [한국어] release 콜백의 반환값은 close(2) 결과가 되지만, 여기서는 실패할 일이 없어 항상 0 이다. */
	return 0;
}

/* [한국어] /dev/vfio/<groupID> 노드의 file_operations. vfio_group_alloc 이 cdev_init 으로 이
 * 테이블을 group->cdev 에 붙인다. 이 테이블의 **주소 자체**가 신원 증명으로도 쓰인다 —
 * vfio_group_from_file 이 file->f_op 를 이것과 비교해 "정말 group fd 인가" 를 판정한다.
 * read/write/mmap 이 없다는 점에 주의: group fd 는 순수한 제어용 fd 이고 데이터 경로는
 * device fd 쪽에 있다. */
static const struct file_operations vfio_group_fops = {
	/* [한국어] 이 fops 를 소유한 모듈. 파일이 열려 있는 동안 vfio 모듈이 언로드되지 않게 한다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 가 open 시 try_module_get 으로 사용한다.
	 * 값 범위: THIS_MODULE(vfio 코어 모듈).
	 * 동기화: 불변이라 필요 없다. */
	.owner		= THIS_MODULE,
	/* [한국어] group fd 의 ioctl 진입점. 네 가지 GROUP 계열 명령을 처리한다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 ioctl 시스템 콜 경로.
	 * 값 범위: 이 파일의 vfio_group_fops_unl_ioctl.
	 * 동기화: 여러 스레드가 동시에 진입할 수 있으며 핸들러가 group_lock 으로 직렬화한다. */
	.unlocked_ioctl	= vfio_group_fops_unl_ioctl,
	/* [한국어] 32비트 프로세스가 64비트 커널에 ioctl 을 부를 때의 표준 어댑터. 포인터 폭만 맞춰
	 * unlocked_ioctl 로 넘긴다. group ioctl 의 인자는 전부 포인터나 정수라 별도 변환 구조체가
	 * 필요 없다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 compat ioctl 경로.
	 * 값 범위: 커널 공용 compat_ptr_ioctl.
	 * 동기화: 위와 같다. */
	.compat_ioctl	= compat_ptr_ioctl,
	/* [한국어] 노드를 열 때 불린다. drivers 카운트, RAWIO 권한, cdev 점유, single-opener 네 게이트를
	 * 차례로 검사한다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 open 경로.
	 * 값 범위: 이 파일의 vfio_group_fops_open.
	 * 동기화: group_lock 아래에서 전체를 처리한다. */
	.open		= vfio_group_fops_open,
	/* [한국어] 마지막 참조가 사라질 때 불린다. container/iommufd 결합을 풀고 opened_file 을 비운다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 마지막 fput 경로.
	 * 값 범위: 이 파일의 vfio_group_fops_release.
	 * 동기화: group_lock 아래에서 처리한다. */
	.release	= vfio_group_fops_release,
};

/* [한국어]
 * vfio_group_find_from_iommu - iommu_group 으로 이미 만들어진 vfio_group 을 찾는다
 *
 * @iommu_group: IOMMU 코어가 만든 격리 단위 객체.
 * @return: 그것을 감싼 vfio_group, 없으면 NULL. **참조를 잡지 않는다**.
 *
 * 왜 필요한가: 한 iommu_group 에는 vfio_group 이 정확히 하나만 있어야 한다. 같은
 * 격리 단위 안의 두 번째 디바이스가 probe 될 때 새로 만드는 대신 기존 것을 찾아
 * drivers 카운트만 올리도록 하는 것이 이 함수다.
 *
 * 동작 과정: vfio.group_lock 을 쥐고 있는지 확인한 뒤 전역 목록을 선형 순회하며
 * iommu_group 포인터를 주소로 비교한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, vfio.group_lock 을 쥔 상태. 참조를 잡지 않으므로
 * 반환값은 그 락 안에서만 유효하다.
 *
 * 에러 경로: 없다. 못 찾으면 NULL.
 *
 * 호출 체인:
 *   vfio_group_find_or_alloc → [vfio_group_find_from_iommu]
 */
/*
 * Group objects - create, release, get, put, search
 */
static struct vfio_group *
vfio_group_find_from_iommu(struct iommu_group *iommu_group)
{
	/* [한국어] 순회 커서 겸 결과. */
	struct vfio_group *group;

	/* [한국어] 호출자가 vfio.group_lock 을 잡고 왔는지 확인한다. 이 락이 없으면 목록이 순회 중에
	 * 바뀌거나, 찾아낸 group 이 곧바로 사라질 수 있다. */
	lockdep_assert_held(&vfio.group_lock);

	/*
	 * group->iommu_group from the vfio.group_list cannot be NULL
	 * under the vfio.group_lock.
	 */
	/* [한국어] 전역 group 목록을 선형으로 훑는다. group 수가 IOMMU group 수준(보통 수십)이라 자료구조를 더 쓰지 않는다. */
	list_for_each_entry(group, &vfio.group_list, vfio_next) {
		/* [한국어] 커널 IOMMU 코어의 iommu_group 포인터를 **주소로** 비교한다. 상류 주석대로
		 * vfio.group_lock 아래에서는 목록 안 group 의 iommu_group 이 NULL 일 수 없다 —
		 * vfio_device_remove_group 이 그것을 NULL 로 만들기 **전에** 이미 list_del 을 하기
		 * 때문이다. */
		if (group->iommu_group == iommu_group)
			/* [한국어] 첫 일치를 그대로 돌려준다. 참조를 잡지 않으므로 호출자는 락 안에서 처리해야 한다. */
			return group;
	}
	/* [한국어] 이 iommu_group 을 감싼 vfio_group 이 아직 없다는 뜻. 호출자가 새로 만든다. */
	return NULL;
}

/* [한국어]
 * vfio_group_release - 내장 struct device 의 마지막 참조가 사라질 때의 소멸자
 *
 * @dev: 해제되는 struct device. container_of 로 vfio_group 을 되찾는다.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_group 의 수명을 내장 struct device 의 kref 에 맡겼기 때문에,
 * 실제 메모리 해제는 이 콜백에서 일어난다. vfio_group_alloc 이 이 함수를
 * group->dev.release 에 꽂아 둔다.
 *
 * 동작 과정: 두 뮤텍스를 파괴하고, 불변식 두 가지를 WARN_ON 으로 확인한 뒤,
 * minor 번호를 ida 에 돌려주고 구조체를 kfree 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. put_device 를 부른 쪽의 문맥에서 동기적으로 실행된다
 * (vfio_create_group 의 실패 경로, vfio_device_remove_group 의 마지막 줄).
 *
 * 에러 경로: 없다. 불변식 위반은 경고만 남긴다.
 *
 * [상류 코드 관찰] vfio_group_alloc 의 iommu_group_ref_get 위에는
 * "put in vfio_group_release()" 라는 상류 주석이 있으나, 이 함수에는 iommu_group_put
 * 호출이 없다. 실제 put 은 vfio_device_remove_group 에서 하고 이 함수는 그것이
 * 끝났음을 WARN_ON 으로 확인만 한다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   put_device(&group->dev) → device_release → [vfio_group_release] → ida_free / kfree
 */
static void vfio_group_release(struct device *dev)
{
	/* [한국어] struct device 의 release 콜백이라 인자가 struct device 다. container_of 로 그것을
	 * 품고 있는 vfio_group 을 되찾는다. */
	struct vfio_group *group = container_of(dev, struct vfio_group, dev);

	/* [한국어] 뮤텍스 디버깅 상태를 정리한다. 잠긴 채로 파괴하면 커널이 경고한다. */
	mutex_destroy(&group->device_lock);
	/* [한국어] group 단위 락도 같이 파괴한다. */
	mutex_destroy(&group->group_lock);
	/* [한국어] 여기 도달했을 때 iommu_group 은 이미 NULL 이어야 한다 —
	 * vfio_device_remove_group 이 group_lock 아래에서 NULL 을 넣고 put 까지 마친 뒤에야
	 * put_device 로 이 경로에 온다. 0 이 아니면 누군가 짝을 어겼다는 뜻이다.
	 * [상류 코드 관찰] vfio_group_alloc 의 iommu_group_ref_get 위에는
	 * "put in vfio_group_release()" 라는 상류 주석이 붙어 있지만, 이 함수에는
	 * iommu_group_put 호출이 없다. 실제 put 은 vfio_device_remove_group 에서 하며,
	 * 이 WARN_ON 은 그때 NULL 이 됐음을 확인하는 검사다. 또한 vfio_create_group 이
	 * dev_set_name 이나 cdev_device_add 에서 실패하면 iommu_group 이 아직 설정된 채로
	 * put_device 를 거쳐 이 함수에 도달하므로, 그 경로에서는 이 WARN_ON 이 뜨고
	 * ref_get 으로 얻은 참조가 반환되지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는
	 * 고치지 않았다. */
	WARN_ON(group->iommu_group);
	/* [한국어] 2세대 cdev 경로가 이 group 의 디바이스를 잡은 채로 남아 있으면 안 된다.
	 * vfio_device_block_group 의 ++ 와 vfio_device_unblock_group 의 -- 가 짝을 이루지
	 * 못했다는 뜻이다. */
	WARN_ON(group->cdev_device_open_cnt);
	/* [한국어] minor 번호를 할당기에 돌려준다. devt 에서 MINOR 매크로로 다시 뽑아내는 이유는
	 * 할당 당시의 minor 값을 따로 보관하지 않기 때문이다. 놓치면 minor 공간이 서서히 고갈된다. */
	ida_free(&vfio.group_ida, MINOR(group->dev.devt));
	/* [한국어] 구조체 메모리를 해제한다. 이 시점 이후 group 포인터는 무효다. */
	kfree(group);
}

/* [한국어]
 * vfio_group_alloc - vfio_group 한 개를 할당하고 모든 내부 상태를 초기화한다
 *
 * @iommu_group: 이 group 이 감쌀 IOMMU 격리 단위. 여기서 참조를 하나 더 잡는다.
 * @type: VFIO_IOMMU / VFIO_EMULATED_IOMMU / VFIO_NO_IOMMU 중 하나. 이후 불변이다.
 * @return: 초기화된 vfio_group, 또는 ERR_PTR(-ENOMEM) / ida 오류.
 *
 * 왜 필요한가: vfio_create_group 에서 "메모리와 상태 준비" 부분만 떼어낸 함수다.
 * 여기까지 성공하면 group 은 완전히 사용 가능한 상태지만, 아직 전역 목록에도
 * sysfs 에도 올라가 있지 않다.
 *
 * 동작 과정:
 *  1. kzalloc_obj 로 0 초기화 할당.
 *  2. ida_alloc_max 로 minor 번호 하나(0 ~ MINORMASK).
 *  3. device_initialize 로 내장 device 의 kref 를 1 로 세운다 — 이 뒤로는 kfree 가 아니라
 *     put_device 로 놓아야 한다.
 *  4. devt, class, release 콜백을 꽂고 cdev 에 vfio_group_fops 를 붙인다.
 *  5. drivers 를 1 로, 두 뮤텍스와 spinlock, device_list 를 초기화한다.
 *  6. iommu_group 포인터를 저장하고 참조를 하나 더 잡는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 할당이라 잠들 수 있다. 호출자
 * (vfio_create_group)는 vfio.group_lock 을 쥐고 있다.
 *
 * 에러 경로: 1단계 실패는 -ENOMEM. 2단계 실패는 아직 device_initialize 전이므로
 * kfree 로 직접 해제한다.
 *
 * refcount 두 개가 여기서 시작한다:
 *  - group->drivers = 1 : 지금 만드는 이유가 된 첫 디바이스의 몫.
 *    짝은 vfio_device_remove_group 의 refcount_dec_and_mutex_lock.
 *  - iommu_group 참조 +1 : vfio_group 자신의 몫.
 *    실제 put 은 vfio_device_remove_group 에 있다(vfio_group_release 의 관찰 참조).
 *
 * 호출 체인:
 *   vfio_create_group → [vfio_group_alloc] → kzalloc_obj / ida_alloc_max /
 *     device_initialize / cdev_init / iommu_group_ref_get
 */
static struct vfio_group *vfio_group_alloc(struct iommu_group *iommu_group,
					   enum vfio_group_type type)
{
	/* [한국어] 새로 만들 group. */
	struct vfio_group *group;
	/* [한국어] 캐릭터 디바이스 minor 번호. 음수면 할당 실패다. */
	int minor;

	/* [한국어] 0 으로 초기화해 할당한다. kzalloc_obj 는 대상 객체 크기를 인자에서 추론하는 매크로라
	 * sizeof 오타로 인한 크기 불일치를 막는다. */
	group = kzalloc_obj(*group);
	/* [한국어] 메모리 부족. */
	if (!group)
		/* [한국어] -ENOMEM 을 포인터로 감싸 돌려준다. */
		return ERR_PTR(-ENOMEM);

	/* [한국어] 0 ~ MINORMASK 범위에서 비어 있는 minor 하나를 얻는다. 이 값이 곧
	 * /dev/vfio/<groupID> 노드의 minor 이며, vfio_group_release 가 ida_free 로 돌려준다. */
	minor = ida_alloc_max(&vfio.group_ida, MINORMASK, GFP_KERNEL);
	/* [한국어] minor 공간이 다 찼거나 메모리가 없다. */
	if (minor < 0) {
		/* [한국어] 아직 device 를 초기화하기 전이라 put_device 가 아니라 kfree 로 직접 해제해야 한다. */
		kfree(group);
		/* [한국어] ida 가 준 음수 오류를 그대로 포인터로 감싼다. */
		return ERR_PTR(minor);
	}

	/* [한국어] 내장 struct device 의 kref 를 1 로 세우고 필드를 초기화한다. 이 시점 이후로는
	 * kfree 가 아니라 put_device 로 놓아야 한다 — 아래 vfio_create_group 의 err_put 이
	 * 그렇게 한다. */
	device_initialize(&group->dev);
	/* [한국어] major 는 모듈 전역이 alloc_chrdev_region 으로 받은 값, minor 는 방금 ida 에서 얻은 값을
	 * 합쳐 이 group 노드의 디바이스 번호를 만든다. */
	group->dev.devt = MKDEV(MAJOR(vfio.group_devt), minor);
	/* [한국어] 이 device 를 /sys/class/vfio 아래에 넣고, devnode 콜백으로 /dev/vfio/ 접두사를 얻게 한다. */
	group->dev.class = &vfio_class;
	/* [한국어] 마지막 참조가 사라질 때 불릴 소멸자. struct device 의 수명 관리가 곧 vfio_group 의
	 * 수명 관리다. */
	group->dev.release = vfio_group_release;
	/* [한국어] 캐릭터 디바이스에 이 파일의 vfio_group_fops 를 붙인다. 이후 /dev/vfio/<gid> 를 열면
	 * vfio_group_fops_open 이 불린다. */
	cdev_init(&group->cdev, &vfio_group_fops);
	/* [한국어] cdev 가 열려 있는 동안 vfio 모듈이 언로드되지 않게 한다. */
	group->cdev.owner = THIS_MODULE;

	/* [한국어] drivers 는 이 group 에 붙은 vfio 드라이버 인스턴스 수다. 지금 만드는 이유가 첫
	 * 디바이스 때문이므로 1 에서 시작한다.
	 * 짝: vfio_group_find_or_alloc 의 refcount_inc(같은 group 에 디바이스가 더 붙을 때)와
	 * vfio_device_remove_group 의 refcount_dec_and_mutex_lock.
	 * 0 이 되는 순간 group 이 목록에서 빠지고 cdev 가 내려간다. */
	refcount_set(&group->drivers, 1);
	/* [한국어] group 단위 상태(container/iommufd/opened_file)를 지킬 뮤텍스. */
	mutex_init(&group->group_lock);
	/* [한국어] KVM 포인터 갱신용 spinlock. KVM 쪽이 짧은 원자 문맥에서 이 값을 바꾸므로 뮤텍스가
	 * 아니라 spinlock 이다. */
	spin_lock_init(&group->kvm_ref_lock);
	/* [한국어] 이 group 에 등록될 vfio_device 들의 목록 머리. */
	INIT_LIST_HEAD(&group->device_list);
	/* [한국어] 그 목록을 지킬 뮤텍스. */
	mutex_init(&group->device_lock);
	/* [한국어] 커널 IOMMU 코어의 group 객체를 붙든다. 이 포인터가 곧 격리 단위의 실체다. */
	group->iommu_group = iommu_group;
	/* put in vfio_group_release() */
	/* [한국어] iommu_group 참조를 하나 더 잡는다. 호출자가 가진 참조와 별개로 vfio_group 자신의
	 * 몫이며, 실제 반환은 vfio_device_remove_group 의 iommu_group_put 에서 한다
	 * (바로 위 상류 주석과 vfio_group_release 의 관찰 참조). */
	iommu_group_ref_get(iommu_group);
	/* [한국어] IOMMU / EMULATED / NO_IOMMU 중 하나. 생성 시 정해지고 이후 불변이다. */
	group->type = type;

	/* [한국어] 완성된 group. 아직 목록에도 sysfs 에도 올라가 있지 않다. */
	return group;
}

/* [한국어]
 * vfio_create_group - vfio_group 을 만들어 sysfs 와 /dev 와 전역 목록에 올린다
 *
 * @iommu_group: 감쌀 IOMMU 격리 단위.
 * @type: group 의 IOMMU 백킹 분류.
 * @return: 등록까지 마친 vfio_group, 또는 ERR_PTR.
 *
 * 왜 필요한가: vfio_group_alloc 이 만든 객체를 실제로 사용자에게 보이게 만드는 단계다.
 * 이름을 짓고, cdev 와 device 를 함께 등록해 /dev/vfio/<이름> 노드를 만들고,
 * 마지막에 전역 목록에 넣어 다른 디바이스가 찾을 수 있게 한다.
 *
 * 동작 과정:
 *  1. 호출자가 vfio.group_lock 을 쥐고 왔는지 확인한다.
 *  2. vfio_group_alloc.
 *  3. dev_set_name 으로 이름을 짓는다. noiommu 면 "noiommu-" 접두사가 붙어
 *     사용자와 udev 규칙이 위험한 group 을 이름만으로 구별할 수 있다.
 *  4. cdev_device_add 로 cdev 와 sysfs device 를 함께 등록한다. 이 순간부터
 *     /dev/vfio/<이름> 을 열 수 있다.
 *  5. 전역 목록에 넣는다 — 찾을 수 있게 되는 시점이 이미 완전히 쓸 수 있는 시점이 되도록
 *     맨 마지막에 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor probe). vfio.group_lock 을 쥔 상태이며
 * 잠들 수 있다.
 *
 * 에러 경로: 3단계와 4단계 실패가 err_put 으로 합류해 put_device 로 참조를 놓는다.
 * 그 put 이 vfio_group_release 를 부른다.
 * [상류 코드 관찰] 그 경로에서는 group->iommu_group 이 아직 설정된 채이므로
 * vfio_group_release 의 WARN_ON(group->iommu_group) 이 뜨고, vfio_group_alloc 의
 * iommu_group_ref_get 으로 얻은 참조가 반환되지 않는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_group_find_or_alloc 또는 vfio_noiommu_group_alloc → [vfio_create_group]
 *     → vfio_group_alloc / dev_set_name / cdev_device_add / list_add
 */
static struct vfio_group *vfio_create_group(struct iommu_group *iommu_group,
		enum vfio_group_type type)
{
	/* [한국어] 만들어질 group. */
	struct vfio_group *group;
	/* [한국어] 오류일 때 돌려줄 포인터. 성공 경로는 group 을 그대로 쓴다. */
	struct vfio_group *ret;
	/* [한국어] 각 단계의 오류 코드. */
	int err;

	/* [한국어] 전역 목록에 list_add 를 하므로 호출자가 vfio.group_lock 을 잡고 와야 한다.
	 * vfio_group_find_or_alloc 과 vfio_noiommu_group_alloc 이 그렇게 한다. */
	lockdep_assert_held(&vfio.group_lock);

	/* [한국어] 메모리와 minor, 내장 device/cdev 를 준비한다. */
	group = vfio_group_alloc(iommu_group, type);
	/* [한국어] 할당 실패. */
	if (IS_ERR(group))
		/* [한국어] ERR_PTR 을 그대로 전달한다. */
		return group;

	/* [한국어] sysfs 와 /dev 에 쓸 이름을 정한다. noiommu group 은 앞에 "noiommu-" 를 붙여
	 * 사용자와 udev 규칙이 위험한 group 을 이름만으로 구별할 수 있게 한다 —
	 * 따라서 노드는 /dev/vfio/noiommu-<N> 이 된다. */
	err = dev_set_name(&group->dev, "%s%d",
			   group->type == VFIO_NO_IOMMU ? "noiommu-" : "",
			   /* [한국어] IOMMU 코어가 부여한 group 번호가 이름의 숫자 부분이 된다. */
			   iommu_group_id(iommu_group));
	/* [한국어] 이름 문자열 할당 실패. */
	if (err) {
		/* [한국어] 오류를 포인터로 감싼다. */
		ret = ERR_PTR(err);
		/* [한국어] device 참조를 놓는 라벨로 간다. */
		goto err_put;
	}

	/* [한국어] cdev 를 커널에 등록하고 동시에 device 를 sysfs 에 올린다. 이 호출이 성공한 뒤부터
	 * /dev/vfio/<이름> 노드가 생기고 사용자가 열 수 있다. 같은 이름이 이미 있으면 실패하는데,
	 * 그래서 vfio_device_remove_group 이 cdev_device_del 을 vfio.group_lock 아래에서 한다. */
	err = cdev_device_add(&group->cdev, &group->dev);
	/* [한국어] 등록 실패(이름 충돌, 메모리 부족 등). */
	if (err) {
		/* [한국어] 오류를 포인터로 감싼다. */
		ret = ERR_PTR(err);
		/* [한국어] device 참조를 놓는 라벨로 간다. */
		goto err_put;
	}

	/* [한국어] 전역 목록에 넣는다. 이 시점부터 vfio_group_find_from_iommu 가 이 group 을 찾을 수
	 * 있다. 목록 등록을 sysfs 등록 뒤에 두는 이유는, 찾을 수 있게 되는 순간 이미 완전히
	 * 쓸 수 있는 상태여야 하기 때문이다. */
	list_add(&group->vfio_next, &vfio.group_list);

	/* [한국어] 완성된 group. 호출자가 drivers 참조 1개를 물려받는다. */
	return group;

/* [한국어] 두 실패가 합류하는 라벨. */
err_put:
	/* [한국어] device_initialize 가 세운 참조 1 을 놓는다. 0 이 되어 vfio_group_release 가 불리고
	 * minor 반환과 kfree 까지 간다. */
	put_device(&group->dev);
	/* [한국어] ERR_PTR 을 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_noiommu_group_alloc - IOMMU 가 없는 디바이스를 위해 가짜 iommu_group 을 만들어 얹는다
 *
 * @dev: 이 group 에 넣을 하부 struct device.
 * @type: VFIO_NO_IOMMU(물리 디바이스, 격리 없음) 또는 VFIO_EMULATED_IOMMU(mdev).
 * @return: 만들어진 vfio_group, 또는 ERR_PTR.
 *
 * 왜 필요한가: iommu_group sysfs 인터페이스는 **사용자 공간 ABI 의 일부**다.
 * libvirt 나 DPDK 같은 사용자 도구는 /sys/bus/.../iommu_group 링크를 따라가
 * /dev/vfio/<gid> 를 찾는다. 그래서 IOMMU 하드웨어가 없어도, 또는 mdev 처럼 IOMMU 가
 * 필요 없어도, 겉모습만은 갖춰 줘야 한다. 이 함수가 그 가짜 group 을 만든다.
 *
 * 동작 과정:
 *  1. iommu_group_alloc 으로 빈 group 하나.
 *  2. iommu_group_set_name 으로 "vfio-noiommu" 라는 이름을 붙여 진짜가 아님을 드러낸다.
 *  3. iommu_group_add_device 로 디바이스를 넣는다(sysfs 링크가 여기서 생긴다).
 *  4. vfio.group_lock 아래에서 vfio_create_group.
 *  5. iommu_group_alloc 이 준 호출자 몫 참조를 놓는다(vfio_group 이 자기 몫을 따로 쥐었다).
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor probe). 할당과 뮤텍스가 있어 잠들 수 있다.
 *
 * 에러 경로: out_remove_device 는 3단계를 되돌리고 out_put_group 으로 떨어지며,
 * out_put_group 은 1단계를 되돌린다.
 *
 * noiommu 가 포기하는 것: 이 경로로 만들어진 VFIO_NO_IOMMU group 은 IOVA 번역도
 * 디바이스 격리도 없다 — 디바이스가 찍는 DMA 주소가 곧 물리 주소이므로 사용자 공간이
 * 커널 메모리 어디든 덮어쓸 수 있다. 그래서 가상머신에 할당할 수 없고, CAP_SYS_RAWIO 를
 * 요구하며, 커널을 taint 한다(vfio_group_find_or_alloc 의 add_taint).
 *
 * 호출 체인:
 *   vfio_device_set_group 또는 vfio_group_find_or_alloc → [vfio_noiommu_group_alloc]
 *     → iommu_group_alloc / iommu_group_set_name / iommu_group_add_device /
 *       vfio_create_group
 */
static struct vfio_group *vfio_noiommu_group_alloc(struct device *dev,
		enum vfio_group_type type)
{
	/* [한국어] 이 디바이스만을 위해 새로 만들 가짜 iommu_group. */
	struct iommu_group *iommu_group;
	/* [한국어] 그 위에 얹을 vfio_group. */
	struct vfio_group *group;
	/* [한국어] 각 단계의 오류 코드. */
	int ret;

	/* [한국어] IOMMU 코어에 빈 group 을 하나 만들어 달라고 한다. 진짜 IOMMU 하드웨어가 없어도
	 * iommu_group sysfs 인터페이스가 사용자 ABI 의 일부라 가짜라도 있어야 한다.
	 * 이 호출의 구현(drivers/iommu)은 이 트리에 없어 확인 못 함. */
	iommu_group = iommu_group_alloc();
	/* [한국어] group 생성 실패. */
	if (IS_ERR(iommu_group))
		/* [한국어] ERR_CAST 는 오류 포인터의 타입만 바꾼다 — 값은 그대로다. */
		return ERR_CAST(iommu_group);

	/* [한국어] sysfs 에 보일 group 이름을 "vfio-noiommu" 로 붙여, 이것이 진짜 IOMMU 격리 group 이
	 * 아님을 드러낸다. */
	ret = iommu_group_set_name(iommu_group, "vfio-noiommu");
	/* [한국어] 이름 설정 실패. */
	if (ret)
		/* [한국어] 방금 만든 iommu_group 을 놓는 라벨로 간다. */
		goto out_put_group;
	/* [한국어] 디바이스를 이 가짜 group 에 넣는다. 이 뒤로 /sys/bus/.../iommu_group 심볼릭 링크가
	 * 생겨 사용자 공간이 평소와 같은 방식으로 group 을 찾을 수 있다. */
	ret = iommu_group_add_device(iommu_group, dev);
	/* [한국어] 추가 실패. */
	if (ret)
		/* [한국어] iommu_group 을 놓는 라벨로 간다. */
		goto out_put_group;

	/* [한국어] vfio_create_group 이 요구하는 전역 락을 잡는다. */
	mutex_lock(&vfio.group_lock);
	/* [한국어] 이 가짜 iommu_group 위에 vfio_group 을 만든다. type 은 호출자가 준 VFIO_NO_IOMMU
	 * 또는 VFIO_EMULATED_IOMMU 다. */
	group = vfio_create_group(iommu_group, type);
	/* [한국어] 만들자마자 놓는다 — 이후 작업은 group 참조만으로 충분하다. */
	mutex_unlock(&vfio.group_lock);
	/* [한국어] 생성 실패. */
	if (IS_ERR(group)) {
		/* [한국어] 오류 코드를 꺼낸다. */
		ret = PTR_ERR(group);
		/* [한국어] 디바이스를 가짜 group 에서 빼는 라벨로 간다. */
		goto out_remove_device;
	}
	/* [한국어] iommu_group_alloc 이 준 **호출자 몫** 참조를 놓는다. vfio_group 이 자기 몫을
	 * vfio_group_alloc 안에서 따로 잡아 두었으므로 여기서 놓아도 group 은 살아 있다. */
	iommu_group_put(iommu_group);
	/* [한국어] 완성된 vfio_group. */
	return group;

/* [한국어] vfio_create_group 실패만 여기로 온다. */
out_remove_device:
	/* [한국어] iommu_group_add_device 를 되돌린다. */
	iommu_group_remove_device(dev);
/* [한국어] 이름 설정 실패와 디바이스 추가 실패가 합류한다. */
out_put_group:
	/* [한국어] iommu_group_alloc 을 되돌린다. 마지막 참조라면 가짜 group 이 해제된다. */
	iommu_group_put(iommu_group);
	/* [한국어] 오류를 포인터로 감싸 돌려준다. */
	return ERR_PTR(ret);
}

/* [한국어]
 * vfio_group_has_device - 이 group 에 그 하부 device 가 이미 등록돼 있는가
 *
 * @group: 검사할 group.
 * @dev: 찾을 하부 struct device.
 * @return: 있으면 true.
 *
 * 왜 필요한가: 같은 struct device 에 vfio 드라이버가 두 번 바인딩되는 일은 있을 수
 * 없으므로, 그런 상태를 발견하면 커널 버그다. vfio_group_find_or_alloc 이 이 함수를
 * WARN_ON 으로 감싸 그 버그를 즉시 드러낸다.
 *
 * 동작 과정: device_lock 아래에서 목록을 순회하며 device->dev 포인터를 주소로 비교한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스를 잡으므로 잠들 수 있다.
 * 호출자는 이미 vfio.group_lock 을 쥐고 있으므로 락 순서는
 * vfio.group_lock → group->device_lock 이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_group_find_or_alloc → [vfio_group_has_device]
 */
static bool vfio_group_has_device(struct vfio_group *group, struct device *dev)
{
	/* [한국어] 순회 커서. */
	struct vfio_device *device;

	/* [한국어] 목록이 바뀌지 않게 막는다. */
	mutex_lock(&group->device_lock);
	/* [한국어] 이 group 에 등록된 vfio_device 를 하나씩 본다. */
	list_for_each_entry(device, &group->device_list, group_next) {
		/* [한국어] 하부 struct device 포인터를 주소로 비교한다. 같은 물리 디바이스가 두 번 등록되는
		 * 것을 잡아내려는 검사다. */
		if (device->dev == dev) {
			/* [한국어] 찾았으니 바로 락을 풀고. */
			mutex_unlock(&group->device_lock);
			/* [한국어] 참을 돌려준다. */
			return true;
		}
	}
	/* [한국어] 끝까지 못 찾았으면 락을 푼다. */
	mutex_unlock(&group->device_lock);
	/* [한국어] 중복 등록이 아니다. */
	return false;
}

/* [한국어]
 * vfio_group_find_or_alloc - 디바이스가 속할 vfio_group 을 찾거나 새로 만든다
 *
 * @dev: 방금 vfio 드라이버가 probe 한 하부 struct device.
 * @return: drivers 참조를 1 얻은 vfio_group, 또는 ERR_PTR
 *          (-EINVAL 이면 IOMMU 도 없고 noiommu 도 꺼져 있다).
 *
 * 왜 필요한가: **격리 단위를 VFIO 가 정하지 않는다는 원칙**이 여기 나타난다.
 * IOMMU 드라이버가 PCIe ACS 와 토폴로지를 보고 미리 만들어 둔 iommu_group 을 그대로
 * 받아들이고, 그것을 감싼 vfio_group 이 없으면 만든다. 같은 격리 단위의 두 번째
 * 디바이스는 기존 group 에 합류한다.
 *
 * 동작 과정:
 *  1. iommu_group_get 으로 디바이스의 iommu_group 을 참조와 함께 얻는다.
 *  2. 없는데 vfio_noiommu 가 켜져 있으면 vfio_noiommu_group_alloc 으로 가짜 group 을
 *     만들고 커널을 taint 한다.
 *  3. 없고 noiommu 도 꺼져 있으면 -EINVAL — 격리를 보장할 수 없으므로 거절한다.
 *  4. vfio.group_lock 아래에서 vfio_group_find_from_iommu 로 기존 group 을 찾는다.
 *     있으면 중복 등록을 WARN_ON 으로 걸러낸 뒤 drivers 를 +1, 없으면 vfio_create_group.
 *  5. 1단계의 호출자 몫 iommu_group 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor probe). 잠들 수 있다.
 *
 * 에러 경로: 모든 실패가 ERR_PTR 로 나가며, 그 경우 drivers 참조를 얻은 것이 없다.
 *
 * noiommu 게이트: 이 함수 자체는 CAP_SYS_RAWIO 를 요구하지 않는다. 관리자가
 * enable_unsafe_noiommu_mode 모듈 파라미터를 켰다는 것 자체가 시스템 수준의 opt-in 이고,
 * 사용자 수준의 권한 검사는 노드를 여는 vfio_group_fops_open 과 container.c 쪽에서 한다.
 *
 * 호출 체인:
 *   vfio_register_group_dev → __vfio_register_dev → vfio_device_set_group
 *     → [vfio_group_find_or_alloc]
 *     → iommu_group_get / vfio_noiommu_group_alloc / vfio_group_find_from_iommu /
 *       vfio_create_group / iommu_group_put
 */
static struct vfio_group *vfio_group_find_or_alloc(struct device *dev)
{
	/* [한국어] 이 디바이스가 속한 커널 IOMMU group. */
	struct iommu_group *iommu_group;
	/* [한국어] 찾거나 새로 만들 vfio_group. */
	struct vfio_group *group;

	/* [한국어] 디바이스가 이미 속해 있는 iommu_group 을 참조와 함께 얻는다. IOMMU 드라이버가
	 * PCIe ACS 와 토폴로지를 보고 미리 만들어 둔 것이라, VFIO 가 격리 단위를 스스로
	 * 정하지 않고 그대로 따른다는 뜻이다. NULL 이면 IOMMU 가 이 디바이스를 모른다. */
	iommu_group = iommu_group_get(dev);
	/* [한국어] IOMMU 가 없는 디바이스인데 관리자가 noiommu 모드를 켜 둔 경우.
	 * vfio_noiommu 는 vfio_main.c 의 모듈 파라미터 enable_unsafe_noiommu_mode 이며,
	 * Kconfig 로 꺼 두면 vfio.h:164 가 컴파일 시 false 상수로 만들어 이 분기가 통째로 사라진다. */
	if (!iommu_group && vfio_noiommu) {
		/*
		 * With noiommu enabled, create an IOMMU group for devices that
		 * don't already have one, implying no IOMMU hardware/driver
		 * exists.  Taint the kernel because we're about to give a DMA
		 * capable device to a user without IOMMU protection.
		 */
		/* [한국어] 가짜 iommu_group 을 만들고 그 위에 VFIO_NO_IOMMU 타입 group 을 얹는다. */
		group = vfio_noiommu_group_alloc(dev, VFIO_NO_IOMMU);
		/* [한국어] 생성에 성공했다면. */
		if (!IS_ERR(group)) {
			/* [한국어] 커널을 taint 한다. IOMMU 보호 없이 DMA 가능한 디바이스를 사용자에게 넘기려는
			 * 참이라, 이후 이 커널에서 나온 버그 리포트는 신뢰할 수 없다는 표시를 남긴다.
			 * TAINT_USER 는 "사용자가 명시적으로 지원 불가 설정을 켰다", LOCKDEP_STILL_OK 는
			 * 이 taint 가 lockdep 검증의 신뢰성까지 깨뜨리지는 않는다는 뜻이다. */
			add_taint(TAINT_USER, LOCKDEP_STILL_OK);
			/* [한국어] 무슨 일이 있었는지 커널 로그에도 남긴다. */
			dev_warn(dev, "Adding kernel taint for vfio-noiommu group on device\n");
		}
		/* [한국어] 성공이든 실패든 noiommu 경로는 여기서 끝난다. */
		return group;
	}

	/* [한국어] IOMMU 가 모르는 디바이스인데 noiommu 도 꺼져 있다. */
	if (!iommu_group)
		/* [한국어] -EINVAL. 이 디바이스는 VFIO 로 넘길 수 없다 — 격리를 보장할 방법이 없기 때문이다. */
		return ERR_PTR(-EINVAL);

	/* [한국어] 목록 검색과 생성을 한 임계구역에서 해야 같은 iommu_group 에 group 이 둘 생기지 않는다. */
	mutex_lock(&vfio.group_lock);
	/* [한국어] 이미 이 iommu_group 을 감싼 vfio_group 이 있는지 본다. */
	group = vfio_group_find_from_iommu(iommu_group);
	/* [한국어] 있다 — 같은 격리 단위 안의 다른 디바이스가 먼저 등록된 것이다. */
	if (group) {
		/* [한국어] 같은 하부 device 가 이미 이 group 에 등록돼 있으면 커널 버그다. 한 struct device 에
		 * vfio 드라이버가 두 번 바인딩될 수 없다. */
		if (WARN_ON(vfio_group_has_device(group, dev)))
			/* [한국어] 중복 등록은 -EINVAL 로 거절한다. */
			group = ERR_PTR(-EINVAL);
		/* [한국어] 중복 등록이 아니라면 — 즉 같은 격리 단위의 **다른** 디바이스가 새로 붙는 정상 경로일 때. */
		else
			/* [한국어] 기존 group 에 디바이스가 하나 더 붙었다고 세어 둔다. vfio_group_alloc 의
			 * refcount_set(.., 1) 과 같은 카운터이며, 짝은 vfio_device_remove_group 의
			 * refcount_dec_and_mutex_lock 이다. 이 참조가 있는 동안 group->iommu_group 포인터가
			 * 유효하다. 빠뜨리면 다른 디바이스가 빠질 때 group 이 먼저 해제돼 use-after-free 가 된다. */
			refcount_inc(&group->drivers);
	} else {
		/* [한국어] 이 iommu_group 을 감싼 첫 디바이스다 — group 을 새로 만든다(drivers 는 1 로 시작). */
		group = vfio_create_group(iommu_group, VFIO_IOMMU);
	}
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&vfio.group_lock);

	/* The vfio_group holds a reference to the iommu_group */
	/* [한국어] iommu_group_get 이 준 호출자 몫 참조를 놓는다. 상류 주석대로 vfio_group 이 자기
	 * 몫을 따로 쥐고 있으므로 안전하다. */
	iommu_group_put(iommu_group);
	/* [한국어] group 포인터 또는 ERR_PTR. */
	return group;
}

/* [한국어]
 * vfio_device_set_group - 등록되는 디바이스에 group 을 붙인다
 *
 * @device: 아직 등록 전인 vfio_device. 성공하면 device->group 이 채워진다.
 * @type: vendor 가 선언한 group 종류. vfio_register_group_dev 는 VFIO_IOMMU 를,
 *        vfio_register_emulated_iommu_dev 는 VFIO_EMULATED_IOMMU 를 준다.
 * @return: 0 성공, 아니면 하위 함수의 오류.
 *
 * 왜 필요한가: vfio_main.c 의 등록 절차에서 "이 디바이스는 어느 격리 단위에 속하는가" 를
 * 정하는 단계다. type 에 따라 진짜 IOMMU group 을 찾거나 가짜 group 을 만든다.
 *
 * 동작 과정: VFIO_IOMMU 면 vfio_group_find_or_alloc, 아니면 vfio_noiommu_group_alloc.
 * 성공하면 그 group 참조(drivers 카운트 1개)의 소유권을 device 로 옮긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor probe). 잠들 수 있다.
 *
 * 에러 경로: 오류를 그대로 올려 등록을 중단시킨다. 이 경우 device->group 은
 * 건드리지 않는다.
 *
 * refcount: 여기서 device 로 넘어간 drivers 참조를 놓는 곳은 vfio_device_remove_group
 * 하나뿐이다. vfio_main.c 의 __vfio_register_dev 실패 경로와 vfio_unregister_group_dev
 * 가 그것을 부른다(vfio_main.c:1318, :1512).
 *
 * 호출 체인:
 *   __vfio_register_dev(vfio_main.c:1268) → [vfio_device_set_group]
 *     → vfio_group_find_or_alloc 또는 vfio_noiommu_group_alloc
 */
int vfio_device_set_group(struct vfio_device *device,
			  enum vfio_group_type type)
{
	/* [한국어] 이 디바이스에 붙일 group. */
	struct vfio_group *group;

	/* [한국어] 진짜 IOMMU 가 있는 물리 디바이스. */
	if (type == VFIO_IOMMU)
		/* [한국어] IOMMU 코어가 정한 격리 단위를 찾아 붙거나, 첫 디바이스면 새로 만든다. */
		group = vfio_group_find_or_alloc(device->dev);
	/* [한국어] VFIO_EMULATED_IOMMU(mdev) 또는 VFIO_NO_IOMMU. */
	else
		/* [한국어] 가짜 iommu_group 을 하나 만들어 이 디바이스 전용 group 을 세운다. mdev 는 vendor 가
		 * 모든 DMA 를 중개하므로 격리가 필요 없고, noiommu 는 격리를 포기한 것이다. */
		group = vfio_noiommu_group_alloc(device->dev, type);

	/* [한국어] 어느 경로든 실패면. */
	if (IS_ERR(group))
		/* [한국어] 오류 코드를 꺼내 돌려준다. 호출자(__vfio_register_dev)는 등록을 중단한다. */
		return PTR_ERR(group);

	/* Our reference on group is moved to the device */
	/* [한국어] group 참조(drivers 카운트 1개)의 소유권이 device 로 넘어간다. 되돌리는 곳은
	 * vfio_device_remove_group 하나뿐이며, vfio_main.c 의 등록 실패 경로와
	 * vfio_unregister_group_dev 가 그것을 부른다. */
	device->group = group;
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * vfio_device_remove_group - 디바이스를 group 에서 떼고, 마지막이면 group 자체를 없앤다
 *
 * @device: 등록 해제되는 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_device_set_group 의 짝이다. drivers 카운트를 하나 줄이고,
 * 0 이 되면 group 을 사용자 세계에서 완전히 걷어낸다 — 노드 제거, 목록 제거,
 * 컨테이너 강제 detach, iommu_group 무효화까지.
 *
 * 동작 과정:
 *  1. 가짜 group 타입이면 iommu_group_remove_device 로 디바이스를 먼저 뺀다.
 *  2. refcount_dec_and_mutex_lock 으로 drivers 를 줄이고, **0 이 됐을 때만**
 *     vfio.group_lock 을 잡은 채 계속 진행한다. 감소와 락 획득이 하나의 원자적 동작이라
 *     "0 을 본 뒤 락을 잡기까지" 의 틈에 다른 CPU 가 이 group 을 되살리는 경합이 없다.
 *  3. 전역 목록에서 뺀다.
 *  4. cdev_device_del 로 /dev 노드와 sysfs 를 내린다. 상류 주석대로 이것을 락 안에서
 *     하는 이유는 동시 probe 가 같은 이름으로 cdev_device_add 를 하다 충돌하지 않게
 *     하기 위함이다.
 *  5. group_lock 아래에서 device_list 가 비었는지 확인하고, 아직 컨테이너에 붙어 있으면
 *     강제로 뗀 뒤, iommu_group 을 NULL 로 만들어 **모든 신규 사용자를 차단**한다.
 *  6. 락을 모두 풀고 iommu_group 참조와 device 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor remove 또는 등록 실패 되돌림). 두 뮤텍스를
 * 차례로 잡으며 잠들 수 있다. 락 순서는 vfio.group_lock → group->group_lock.
 *
 * 에러 경로: 없다. 2단계에서 0 이 아니면 그냥 돌아간다.
 *
 * refcount 정리: 6단계의 iommu_group_put 이 vfio_group_alloc 의 iommu_group_ref_get 짝,
 * put_device 가 device_initialize 짝이다. 후자가 0 을 만들어 vfio_group_release 로 간다.
 *
 * 호출 체인:
 *   vfio_unregister_group_dev(vfio_main.c:1512) 또는 __vfio_register_dev 의 실패 경로
 *     → [vfio_device_remove_group]
 *     → iommu_group_remove_device / cdev_device_del /
 *       vfio_group_detach_container(container.c) / iommu_group_put / put_device
 */
void vfio_device_remove_group(struct vfio_device *device)
{
	/* [한국어] 이 디바이스가 붙어 있던 group. */
	struct vfio_group *group = device->group;
	/* [한국어] 마지막에 놓을 iommu_group 을 임시로 담아 둘 변수. */
	struct iommu_group *iommu_group;

	/* [한국어] 가짜 group 을 쓰는 두 타입이면, 등록 때 iommu_group_add_device 로 넣어 둔 것을
	 * 먼저 빼야 한다. 진짜 IOMMU group 은 IOMMU 코어가 관리하므로 건드리지 않는다. */
	if (group->type == VFIO_NO_IOMMU || group->type == VFIO_EMULATED_IOMMU)
		/* [한국어] 가짜 group 에서 디바이스를 뺀다. vfio_noiommu_group_alloc 의 add 짝이다. */
		iommu_group_remove_device(device->dev);

	/* Pairs with vfio_create_group() / vfio_group_get_from_iommu() */
	/* [한국어] drivers 를 1 줄이고, **0 이 됐을 때만** vfio.group_lock 을 잡은 채 참을 준다.
	 * 감소와 락 획득을 하나의 원자적 동작으로 묶어, 0 을 본 뒤 락을 잡기까지의 틈에
	 * 다른 CPU 가 vfio_group_find_or_alloc 으로 이 group 을 되살리는 경합을 없앤다.
	 * 상류 주석대로 vfio_create_group 의 refcount_set(1) 과 vfio_group_find_or_alloc 의
	 * refcount_inc 양쪽의 짝이다. */
	if (!refcount_dec_and_mutex_lock(&group->drivers, &vfio.group_lock))
		/* [한국어] 아직 이 group 에 다른 디바이스가 남아 있다 — 락도 잡히지 않았고 할 일도 없다. */
		return;
	/* [한국어] 전역 목록에서 뺀다. 이 시점 이후 vfio_group_find_from_iommu 는 이 group 을 찾지
	 * 못한다 — 그래서 아래에서 iommu_group 을 NULL 로 만들어도 목록을 보는 쪽과
	 * 충돌하지 않는다. */
	list_del(&group->vfio_next);

	/*
	 * We could concurrently probe another driver in the group that might
	 * race vfio_device_remove_group() with vfio_get_group(), so we have to
	 * ensure that the sysfs is all cleaned up under lock otherwise the
	 * cdev_device_add() will fail due to the name aready existing.
	 */
	/* [한국어] /dev 노드와 sysfs 항목을 내린다. 상류 주석대로 이것을 vfio.group_lock 안에서 하는
	 * 이유는, 같은 이름의 group 을 만들려는 동시 probe 가 cdev_device_add 에서 이름 충돌로
	 * 실패하지 않게 하기 위함이다. 이 호출 뒤로는 새 open 이 불가능하다. */
	cdev_device_del(&group->cdev, &group->dev);

	/* [한국어] group 내부 상태를 정리하는 구간. 락 순서는 vfio.group_lock → group->group_lock 이다. */
	mutex_lock(&group->group_lock);
	/*
	 * These data structures all have paired operations that can only be
	 * undone when the caller holds a live reference on the device. Since
	 * all pairs must be undone these WARN_ON's indicate some caller did not
	 * properly hold the group reference.
	 */
	/* [한국어] 마지막 디바이스를 떼는 중이므로 목록은 비어 있어야 한다. 비어 있지 않다면
	 * vfio_device_group_register 와 unregister 의 짝이 어긋난 것이다. */
	WARN_ON(!list_empty(&group->device_list));

	/*
	 * Revoke all users of group->iommu_group. At this point we know there
	 * are no devices active because we are unplugging the last one. Setting
	 * iommu_group to NULL blocks all new users.
	 */
	/* [한국어] 아직 컨테이너에 붙어 있다면(사용자가 group fd 를 닫지 않은 채 디바이스가 뽑힌 경우)
	 * 여기서 강제로 뗀다. 상류 주석대로 이 시점에는 활성 디바이스가 없으므로 안전하다. */
	if (group->container)
		/* [한국어] container.c:468. 백엔드 detach, DMA 소유권 반환, 마지막이면 백엔드 release 까지 한다. */
		vfio_group_detach_container(group);
	/* [한국어] put 은 락 밖에서 해야 하므로 포인터를 먼저 떠 둔다. */
	iommu_group = group->iommu_group;
	/* [한국어] 상류 주석대로 이 대입이 **모든 신규 사용자를 차단**한다.
	 * vfio_group_ioctl_set_container 와 vfio_group_ioctl_get_status 가 이 NULL 을 보고
	 * -ENODEV 를 준다. 또한 vfio_group_release 의 WARN_ON 이 기대하는 상태를 만든다. */
	group->iommu_group = NULL;
	/* [한국어] 안쪽 락 해제. */
	mutex_unlock(&group->group_lock);
	/* [한국어] 바깥 락 해제. 이제 잠들 수 있는 정리 작업을 한다. */
	mutex_unlock(&vfio.group_lock);

	/* [한국어] vfio_group_alloc 의 iommu_group_ref_get 짝. 이것을 빠뜨리면 IOMMU 코어의 group
	 * 객체가 영원히 살아남는다. */
	iommu_group_put(iommu_group);
	/* [한국어] vfio_group_alloc 의 device_initialize 가 세운 마지막 참조를 놓는다. 0 이 되면
	 * vfio_group_release 가 불려 minor 반환과 kfree 까지 간다. 이 시점 이후 group 포인터는
	 * 무효다. */
	put_device(&group->dev);
}

/* [한국어]
 * vfio_device_group_register - 디바이스를 group 의 목록에 넣어 사용자에게 보이게 한다
 *
 * @device: 등록을 마친 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: 이 목록에 들어가는 순간부터 사용자가 GROUP_GET_DEVICE_FD 로 이 디바이스를
 * 이름으로 찾아 열 수 있다. 그래서 vfio_main.c 의 __vfio_register_dev 는 다른 준비
 * (dev_set, cdev, refcount)가 모두 끝난 **마지막**에 이것을 부른다.
 *
 * 동작 과정: group->device_lock 아래에서 device->group_next 를 목록에 넣는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor probe). 뮤텍스를 잡는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   __vfio_register_dev(vfio_main.c:1309) → [vfio_device_group_register]
 */
void vfio_device_group_register(struct vfio_device *device)
{
	/* [한국어] 목록을 고치는 동안 vfio_device_get_from_name 등의 순회와 직렬화한다. */
	mutex_lock(&device->group->device_lock);
	/* [한국어] 이 디바이스를 group 의 목록에 넣는다. 이 순간부터 사용자가 GROUP_GET_DEVICE_FD 로
	 * 이 디바이스를 이름으로 찾을 수 있다. 그래서 vfio_main.c 의 __vfio_register_dev 는
	 * 다른 준비가 모두 끝난 마지막에 이것을 부른다. */
	list_add(&device->group_next, &device->group->device_list);
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&device->group->device_lock);
}

/* [한국어]
 * vfio_device_group_unregister - 디바이스를 group 목록에서 빼 더 이상 찾을 수 없게 한다
 *
 * @device: 등록 해제가 시작된 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_unregister_group_dev 가 **가장 먼저** 하는 일이다. 이 뒤로는
 * GROUP_GET_DEVICE_FD 가 이 디바이스를 찾지 못하므로 새 fd 가 생기지 않고,
 * 이미 열린 fd 들은 device->refcount 가 0 이 될 때까지 기다려 정리한다.
 * 목록에서 빼는 것과 refcount 를 기다리는 것이 함께 있어야 "새 사용자 차단 +
 * 기존 사용자 대기" 가 성립한다.
 *
 * 동작 과정: group->device_lock 아래에서 device->group_next 를 목록에서 뺀다.
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor remove). 뮤텍스를 잡는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_unregister_group_dev(vfio_main.c:1459) → [vfio_device_group_unregister]
 */
void vfio_device_group_unregister(struct vfio_device *device)
{
	/* [한국어] 목록을 고치는 동안 순회와 직렬화한다. */
	mutex_lock(&device->group->device_lock);
	/* [한국어] 목록에서 뺀다. vfio_unregister_group_dev 가 가장 먼저 하는 일이며, 이 뒤로는
	 * GROUP_GET_DEVICE_FD 가 이 디바이스를 찾지 못한다. 이미 열려 있는 fd 는
	 * device->refcount 가 0 이 될 때까지 기다려 정리한다. */
	list_del(&device->group_next);
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&device->group->device_lock);
}

/* [한국어]
 * vfio_device_group_use_iommu - 첫 device open 이 컨테이너의 IOMMU 사용을 신청한다
 *
 * @device: 지금 처음 열리는 디바이스.
 * @return: 0 성공. -EINVAL(container 가 없음 — 커널 버그),
 *          그 밖에 vfio_group_use_container 가 낸 오류(-EINVAL 이면 SET_IOMMU 미완,
 *          -EPERM 이면 noiommu 인데 RAWIO 없음).
 *
 * 왜 필요한가: vfio_main.c 의 vfio_df_device_first_open 은 세대에 따라 iommufd 바인딩
 * 또는 이 함수를 부른다. 1세대에서 "디바이스가 IOMMU 를 쓰기 시작했다" 를 컨테이너와
 * 백엔드에 알리는 지점이 여기다.
 *
 * 동작 과정:
 *  1. 호출자가 group->group_lock 을 쥐고 왔는지 확인한다.
 *  2. container 가 NULL 이면 호출 순서가 깨진 것이라 WARN 후 -EINVAL.
 *  3. vfio_group_use_container(container.c:503) — SET_IOMMU 완료 여부와 noiommu 권한을
 *     확인한 뒤 group->opened_file 에 get_file 로 참조를 걸고 container_users 를 +1.
 *  4. vfio_device_container_register(container.c:164) — 백엔드가 device 단위 목록을
 *     유지한다면 등록한다(type1 이면 vfio_iommu_type1_register_device).
 *
 * 실행 컨텍스트: 프로세스 문맥. group->group_lock 과 dev_set->lock 을 모두 쥔 상태.
 *
 * 에러 경로: 3단계 실패는 그대로 반환하며 4단계로 가지 않는다.
 *
 * refcount: 3단계의 get_file 이 **device fd 가 살아 있는 한 group fd 의 release 가
 * 불리지 않는다** 를 보장한다. 짝은 vfio_device_group_unuse_iommu 안의 fput 이다.
 *
 * 호출 체인:
 *   vfio_df_open → vfio_df_device_first_open(vfio_main.c:1841)
 *     → [vfio_device_group_use_iommu]
 *     → vfio_group_use_container(container.c) / vfio_device_container_register(container.c)
 */
int vfio_device_group_use_iommu(struct vfio_device *device)
{
	/* [한국어] 이 디바이스가 속한 group. */
	struct vfio_group *group = device->group;
	/* [한국어] 결과 코드. */
	int ret = 0;

	/* [한국어] 호출자(vfio_df_device_first_open ← vfio_df_open ← vfio_df_group_open)가
	 * group_lock 을 이미 잡고 왔다. container 포인터를 보는 동안 SET/UNSET_CONTAINER 가
	 * 끼어들면 안 된다. */
	lockdep_assert_held(&group->group_lock);

	/* [한국어] 첫 open 은 vfio_group_has_iommu 검사를 통과한 뒤에만 도달하므로 container 가 NULL 일 수
	 * 없다. NULL 이면 호출 순서가 깨진 커널 버그다. */
	if (WARN_ON(!group->container))
		/* [한국어] -EINVAL 로 첫 open 을 실패시킨다. */
		return -EINVAL;

	/* [한국어] container.c:503. container->iommu_driver 가 이미 정해졌는지(= SET_IOMMU 완료),
	 * noiommu 면 CAP_SYS_RAWIO 가 있는지 확인한 뒤 group->opened_file 에 get_file 로
	 * 참조를 하나 더 걸고 container_users 를 +1 한다.
	 * 그 get_file 이 "device fd 가 살아 있는 한 group fd 의 release 는 불리지 않는다" 를
	 * 보장한다. */
	ret = vfio_group_use_container(group);
	/* [한국어] SET_IOMMU 가 아직이거나 권한이 없다. */
	if (ret)
		/* [한국어] 오류를 그대로 올려 첫 open 을 실패시킨다. */
		return ret;
	/* [한국어] container.c:164. 백엔드가 device 단위 목록을 유지한다면 거기에 등록한다.
	 * type1 은 vfio_iommu_type1_register_device 로 이 디바이스를 언맵 통지 대상에 넣는다.
	 * 짝은 vfio_device_group_unuse_iommu 의 vfio_device_container_unregister 다. */
	vfio_device_container_register(device);
	/* [한국어] 성공. 호출자가 이어서 vendor 의 open_device 를 부른다. */
	return 0;
}

/* [한국어]
 * vfio_device_group_unuse_iommu - 마지막 device close 가 IOMMU 사용 신청을 되돌린다
 *
 * @device: 마지막 fd 가 닫히는 디바이스.
 * @return: 없음. 실패할 수 없다.
 *
 * 왜 필요한가: vfio_device_group_use_iommu 의 정확한 짝이다. 순서까지 뒤집어
 * 백엔드 등록 해제를 먼저, 컨테이너 사용 반환을 나중에 한다.
 *
 * 동작 과정:
 *  1. 호출자가 group->group_lock 을 쥐고 왔는지 확인한다.
 *  2. container 가 NULL 이면 짝이 어긋난 것이라 WARN 후 그냥 나간다.
 *  3. vfio_device_container_unregister(container.c:174) — type1 이면
 *     vfio_iommu_type1_unregister_device.
 *  4. vfio_group_unuse_container(container.c:522) — container_users 를 -1 하고
 *     group->opened_file 을 fput 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. group->group_lock 과 dev_set->lock 을 모두 쥔 상태.
 *
 * 에러 경로: 없다.
 *
 * refcount: 4단계의 fput 을 빠뜨리면 group fd 를 닫아도 release 가 불리지 않아
 * 컨테이너와 그 안의 IOVA 매핑, 핀된 페이지가 통째로 샌다.
 *
 * 호출 체인:
 *   vfio_df_close → vfio_df_device_last_close(vfio_main.c:1927)
 *     → [vfio_device_group_unuse_iommu]
 *     → vfio_device_container_unregister(container.c) / vfio_group_unuse_container(container.c)
 */
void vfio_device_group_unuse_iommu(struct vfio_device *device)
{
	/* [한국어] 이 디바이스가 속한 group. */
	struct vfio_group *group = device->group;

	/* [한국어] use 쪽과 같은 락을 요구한다 — 획득과 해제가 같은 락 아래에서 일어나야 짝이 어긋나지 않는다. */
	lockdep_assert_held(&group->group_lock);

	/* [한국어] use 에 성공했다면 container 는 반드시 있어야 한다. 없으면 짝이 어긋난 커널 버그다. */
	if (WARN_ON(!group->container))
		/* [한국어] 되돌릴 것이 없으므로 그냥 나간다. */
		return;

	/* [한국어] 백엔드의 device 목록에서 뺀다(type1 이면 vfio_iommu_type1_unregister_device). */
	vfio_device_container_unregister(device);
	/* [한국어] container.c:522. container_users 를 -1 하고 group->opened_file 의 참조를 fput 으로
	 * 놓는다. 이 두 줄이 use 쪽 get_file 과 ++ 의 정확한 짝이다.
	 * 빠뜨리면 group fd 를 닫아도 release 가 불리지 않아 컨테이너와 IOVA 매핑이 통째로 샌다. */
	vfio_group_unuse_container(group);
}

/* [한국어]
 * vfio_device_has_container - 이 디바이스가 1세대 컨테이너 경로인가
 *
 * @device: 검사할 디바이스.
 * @return: group->container 가 NULL 이 아니면 true.
 *
 * 왜 필요한가: vfio_main.c 는 container 구조체의 정의를 알지 못한다(container.c 안에
 * 숨겨져 있다). 그래서 "이 디바이스가 컨테이너 경로인가" 를 이 한 줄짜리 함수로만
 * 물어보고, 그 결과에 따라 vfio_pin_pages, vfio_dma_rw 등에서 container 경로와
 * iommufd 경로를 가른다.
 *
 * 동작 과정: 포인터를 bool 로 암묵 변환해 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다 — 호출자가 device fd 를 쥐고 있어
 * 결합이 바뀌지 않는 상태에서만 부른다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pin_pages / vfio_unpin_pages / vfio_dma_rw(vfio_main.c:5002, :5103, :5200)
 *     → [vfio_device_has_container]
 */
bool vfio_device_has_container(struct vfio_device *device)
{
	/* [한국어] 이 디바이스가 1세대 컨테이너 경로인지 한 줄로 답한다. vfio_main.c 가
	 * vfio_pin_pages / vfio_dma_rw / vfio_device_fops 의 여러 곳에서 container 경로와
	 * iommufd 경로를 가를 때 쓴다. 포인터를 bool 로 암묵 변환한다. */
	return device->group->container;
}

/* [한국어]
 * vfio_group_from_file - struct file 이 정말 group fd 인지 확인하고 group 을 꺼낸다
 *
 * @file: 검사할 파일.
 * @return: group fd 이면 그 vfio_group, 아니면 NULL. **참조를 잡지 않는다**.
 *
 * 왜 필요한가: KVM 같은 외부 모듈이 사용자에게서 받은 fd 를 그대로 믿을 수 없다.
 * 파일의 file_operations 주소를 이 파일 안의 vfio_group_fops 와 비교하는 방식은
 * 커널 내부 심볼 주소를 쓰므로 사용자 공간이 위조할 수 없다. 같은 기법을
 * container.c:400 의 vfio_container_from_file 과 vfio_main.c 의
 * vfio_device_from_file 이 쓴다.
 *
 * 동작 과정: private_data 를 먼저 읽어 두고(역참조하지 않으므로 안전하다),
 * f_op 를 검사해 아니면 NULL 을 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다. 반환된 group 은 호출자가 file
 * 참조를 쥐고 있는 동안만 유효하다.
 *
 * 에러 경로: 종류가 다르면 NULL.
 *
 * 호출 체인:
 *   vfio_file_iommu_group / vfio_file_is_group / vfio_file_has_dev(이 파일) 및
 *   vfio_file_is_valid / vfio_file_enforced_coherent / vfio_file_set_kvm(vfio_main.c)
 *     → [vfio_group_from_file]
 */
struct vfio_group *vfio_group_from_file(struct file *file)
{
	/* [한국어] 먼저 private_data 를 읽어 둔다. f_op 검사 전이라 아직 group 이라는 보장이 없지만
	 * 역참조하지 않고 값만 읽으므로 안전하다. */
	struct vfio_group *group = file->private_data;

	/* [한국어] **신원 검사**: 이 파일의 file_operations 주소가 이 파일 안의 vfio_group_fops 와
	 * 같은지 본다. fops 주소는 커널 내부 심볼이라 사용자 공간이 위조할 수 없다.
	 * vfio_main.c 의 vfio_device_from_file 과 container.c:400 의 vfio_container_from_file 도
	 * 같은 방식으로 fd 종류를 판정한다. */
	if (file->f_op != &vfio_group_fops)
		/* [한국어] group fd 가 아니다. 호출자는 NULL 을 "다른 종류의 파일" 로 해석한다. */
		return NULL;
	/* [한국어] 검증된 group. 참조를 잡지 않으므로 호출자가 file 참조를 쥐고 있는 동안에만 유효하다. */
	return group;
}

/* [한국어]
 * vfio_file_iommu_group - group fd 에서 iommu_group 을 참조와 함께 꺼낸다 (deprecated)
 *
 * @file: VFIO group 파일.
 * @return: 참조가 걸린 iommu_group, 또는 NULL. 호출자가 iommu_group_put 으로 놓아야 한다.
 *
 * 왜 필요한가: 상류 주석대로 **deprecated 이며 KVM 의 SPAPR 경로만 부르기로 되어 있다**.
 * POWER 계열의 SPAPR TCE IOMMU 는 KVM 이 IOMMU 테이블을 직접 다뤄야 해서 group 객체가
 * 필요했다. 그 외 아키텍처에서는 CONFIG_SPAPR_TCE_IOMMU 가 꺼져 있어 이 함수가
 * 컴파일 시 사실상 "항상 NULL" 로 접힌다.
 *
 * 동작 과정:
 *  1. vfio_group_from_file 로 fd 종류를 확인한다.
 *  2. CONFIG_SPAPR_TCE_IOMMU 가 꺼져 있으면 즉시 NULL.
 *  3. group_lock 아래에서 iommu_group 이 아직 유효한지 보고, 유효하면
 *     iommu_group_ref_get 으로 참조를 하나 잡아 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥(KVM 의 ioctl 경로). 뮤텍스를 잡는다.
 *
 * 에러 경로: 종류가 다르거나 group 이 이미 무효화됐으면 NULL.
 *
 * refcount: 여기서 잡은 참조는 **호출자의 몫**이며 반드시 iommu_group_put 으로
 * 놓아야 한다. 이 트리 안에는 호출 지점이 없다 — KVM 은 sparse 체크아웃에 없다.
 *
 * 호출 체인:
 *   KVM(이 트리에 없음) → [vfio_file_iommu_group] → vfio_group_from_file /
 *     iommu_group_ref_get
 */
/**
 * vfio_file_iommu_group - Return the struct iommu_group for the vfio group file
 * @file: VFIO group file
 *
 * The returned iommu_group is valid as long as a ref is held on the file. This
 * returns a reference on the group. This function is deprecated, only the SPAPR
 * path in kvm should call it.
 */
struct iommu_group *vfio_file_iommu_group(struct file *file)
{
	/* [한국어] 파일이 정말 group fd 인지 확인하며 group 을 꺼낸다. */
	struct vfio_group *group = vfio_group_from_file(file);
	/* [한국어] 기본 반환값. 조건을 통과하지 못하면 NULL 이 나간다. */
	struct iommu_group *iommu_group = NULL;

	/* [한국어] SPAPR TCE IOMMU(POWER 계열)가 빌드에 없으면 이 함수는 항상 NULL 이다.
	 * 상류 주석대로 이 함수는 deprecated 이며 KVM 의 SPAPR 경로만 부르기로 되어 있다.
	 * IS_ENABLED 는 컴파일 시 상수라 그 구성에서는 아래 코드가 통째로 제거된다. */
	if (!IS_ENABLED(CONFIG_SPAPR_TCE_IOMMU))
		/* [한국어] 그 구성에서는 항상 NULL. */
		return NULL;

	/* [한국어] group fd 가 아니었다. */
	if (!group)
		/* [한국어] NULL. */
		return NULL;

	/* [한국어] iommu_group 을 읽고 참조를 잡는 동안 vfio_device_remove_group 이 NULL 로 바꾸지 못하게 막는다. */
	mutex_lock(&group->group_lock);
	/* [한국어] 이미 무효화된 group 이면 줄 것이 없다. */
	if (group->iommu_group) {
		/* [한국어] 락 안에서 포인터를 떠 둔다. */
		iommu_group = group->iommu_group;
		/* [한국어] 호출자에게 넘길 참조를 하나 잡는다. 상류 주석대로 반환된 iommu_group 은
		 * 호출자가 file 참조를 쥐고 있는 동안 유효하며, 이 참조 자체는 호출자가
		 * iommu_group_put 으로 놓아야 한다. 이 트리 안에는 호출 지점이 없다(KVM 은 sparse
		 * 체크아웃에 포함되지 않음). */
		iommu_group_ref_get(iommu_group);
	}
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&group->group_lock);
	/* [한국어] 참조가 걸린 iommu_group 또는 NULL. */
	return iommu_group;
}
/* [한국어] EXPORT_SYMBOL_GPL — 모듈(KVM)에서 부를 수 있게 내보낸다. GPL 모듈만 쓸 수 있다. */
EXPORT_SYMBOL_GPL(vfio_file_iommu_group);

/* [한국어]
 * vfio_file_is_group - 이 파일이 VFIO group fd 인가
 *
 * @file: 검사할 파일.
 * @return: group fd 이면 true.
 *
 * 왜 필요한가: 외부 모듈(KVM)과 vfio_main.c 의 vfio_file_is_valid 가 사용자에게서 받은
 * fd 가 VFIO 계열인지 판별할 때 쓴다. group fd 인지 device fd 인지에 따라 이후 처리가
 * 갈린다.
 *
 * 동작 과정: vfio_group_from_file 의 결과 포인터를 bool 로 암묵 변환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_file_is_valid(vfio_main.c:4359) 및 외부 모듈 → [vfio_file_is_group]
 *     → vfio_group_from_file
 */
/**
 * vfio_file_is_group - True if the file is a vfio group file
 * @file: VFIO group file
 */
bool vfio_file_is_group(struct file *file)
{
	/* [한국어] group fd 인지 여부만 필요하므로 포인터를 bool 로 암묵 변환해 돌려준다.
	 * vfio_main.c 의 vfio_file_is_valid 가 device fd 검사와 함께 이것을 쓴다. */
	return vfio_group_from_file(file);
}
/* [한국어] EXPORT_SYMBOL_GPL — KVM 등 외부 모듈이 fd 종류를 판별할 때 쓴다. */
EXPORT_SYMBOL_GPL(vfio_file_is_group);

/* [한국어]
 * vfio_group_enforced_coherent - group 의 모든 디바이스가 캐시 일관성을 강제할 수 있는가
 *
 * @group: 검사할 group.
 * @return: 모든 디바이스가 IOMMU_CAP_ENFORCE_CACHE_COHERENCY 를 가지면 true.
 *          디바이스가 하나도 없으면 true 가 그대로 나간다.
 *
 * 왜 필요한가: PCIe 의 No-Snoop 속성을 쓰는 DMA 는 CPU 캐시를 우회한다. IOMMU 가 그
 * 속성을 벗겨낼 수 있으면 게스트가 캐시 일관성을 깨는 DMA 를 할 수 없고, 그러면 KVM 은
 * 게스트 페이지의 메모리 타입을 완화해 성능을 얻을 수 있다. 이 함수가 그 판단의
 * 근거를 준다.
 *
 * 동작 과정: device_lock 아래에서 group 의 모든 디바이스를 순회하며
 * device_iommu_capable 을 묻고, 하나라도 못 하면 즉시 false 로 떨어진다.
 * 격리 단위 전체가 같은 IOMMU domain 을 쓰므로 group 단위로 판단해야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 뮤텍스를 잡는다.
 *
 * 에러 경로: 없다.
 *
 * 확인 범위: device_iommu_capable 의 구현(drivers/iommu)은 이 트리에 없어 확인 못 함.
 *
 * 호출 체인:
 *   KVM → vfio_file_enforced_coherent(vfio_main.c:4419)
 *     → [vfio_group_enforced_coherent] → device_iommu_capable
 */
bool vfio_group_enforced_coherent(struct vfio_group *group)
{
	/* [한국어] 순회 커서. */
	struct vfio_device *device;
	/* [한국어] 기본값은 참이다 — 하나라도 능력이 없는 디바이스를 만나면 거짓으로 떨어진다.
	 * 디바이스가 하나도 없으면 참이 그대로 나간다. */
	bool ret = true;

	/*
	 * If the device does not have IOMMU_CAP_ENFORCE_CACHE_COHERENCY then
	 * any domain later attached to it will also not support it. If the cap
	 * is set then the iommu_domain eventually attached to the device/group
	 * must use a domain with enforce_cache_coherency().
	 */
	/* [한국어] 목록을 순회하는 동안 등록/해제와 직렬화한다. */
	mutex_lock(&group->device_lock);
	/* [한국어] 이 group 의 모든 디바이스를 본다. 격리 단위 전체가 같은 IOMMU domain 을 쓰기 때문이다. */
	list_for_each_entry(device, &group->device_list, group_next) {
		/* [한국어] 이 디바이스의 IOMMU 가 캐시 일관성을 **강제**할 수 있는지 묻는다.
		 * PCIe 의 No-Snoop 속성을 IOMMU 가 벗겨낼 수 있으면 게스트가 캐시를 우회하는
		 * DMA 를 할 수 없고, 그러면 KVM 이 게스트의 캐시 정책을 완화해도 안전하다.
		 * device_iommu_capable 의 구현(drivers/iommu)은 이 트리에 없어 확인 못 함. */
		if (!device_iommu_capable(device->dev,
					  IOMMU_CAP_ENFORCE_CACHE_COHERENCY)) {
			/* [한국어] 하나라도 못 하면 group 전체가 못 하는 것으로 본다. */
			ret = false;
			/* [한국어] 더 볼 필요 없다. */
			break;
		}
	}
	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&group->device_lock);
	/* [한국어] vfio_main.c 의 vfio_file_enforced_coherent 가 이 값을 KVM 에 전달하고,
	 * KVM 은 이것으로 게스트 페이지의 메모리 타입을 강제할지 결정한다. */
	return ret;
}

/* [한국어]
 * vfio_group_set_kvm - group 에 KVM 포인터를 심거나 지운다
 *
 * @group: 대상 group.
 * @kvm: 연결할 KVM 가상머신 포인터. 연결을 끊을 때는 NULL 이 온다.
 * @return: 없음.
 *
 * 왜 필요한가: KVM 이 vfio group fd 를 자기 디바이스로 붙일 때 "이 group 은 이 VM 의
 * 것" 이라고 알려 준다. 이후 이 group 의 디바이스가 처음 열릴 때
 * vfio_device_group_get_kvm_safe 가 이 값을 읽어 참조를 잡고 device->kvm 에 저장하며,
 * vendor 드라이버가 그것을 쓴다.
 *
 * 동작 과정: kvm_ref_lock(spinlock) 아래에서 포인터를 대입한다.
 * **참조는 잡지 않는다** — 참조 획득/반환은 디바이스 open/close 쪽 책임이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(KVM 의 ioctl). spinlock 이라 그 안에서 잠들지 않는다.
 * 뮤텍스가 아니라 spinlock 인 이유는 읽는 쪽(vfio_device_group_get_kvm_safe)이
 * 짧은 원자 구간이어야 하기 때문이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   KVM → vfio_file_set_kvm(vfio_main.c:4539) → [vfio_group_set_kvm]
 */
void vfio_group_set_kvm(struct vfio_group *group, struct kvm *kvm)
{
	/* [한국어] 포인터 갱신을 원자 구간에 넣는다. vfio_device_group_get_kvm_safe 가 같은 락 아래에서
	 * 이 값을 읽으므로, 읽는 쪽이 반쯤 갱신된 포인터를 보는 일이 없다. */
	spin_lock(&group->kvm_ref_lock);
	/* [한국어] group 이 어느 KVM 가상머신에 속하는지 기록한다. **참조는 잡지 않는다** —
	 * 참조는 device 를 열 때 vfio_device_get_kvm_safe 가 잡고 닫을 때 놓는다.
	 * KVM 이 사라지면 vfio_main.c 의 vfio_file_set_kvm 이 NULL 로 다시 부른다. */
	group->kvm = kvm;
	/* [한국어] 짧은 원자 구간을 닫는다. */
	spin_unlock(&group->kvm_ref_lock);
}

/* [한국어]
 * vfio_file_has_dev - 이 VFIO 파일이 그 디바이스를 다룰 권한을 갖는가
 *
 * @file: 검사할 VFIO 파일(여기서는 group fd 여야 한다).
 * @device: 그 파일이 다룰 수 있어야 하는 디바이스.
 * @return: 파일이 group fd 이고 그 group 이 device 의 group 과 같으면 true.
 *
 * 왜 필요한가: KVM 이 "사용자가 준 vfio fd 가 정말 이 디바이스의 핸들인가" 를 확인할 때
 * 쓴다. **격리 단위가 group 이므로 group fd 를 쥔 사용자는 그 안의 모든 디바이스를
 * 다룰 자격이 있다** — VFIO 권한 모델의 요약이 이 한 줄 비교다.
 *
 * 동작 과정: vfio_group_from_file 로 종류를 확인하고, group 포인터를 주소로 비교한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다 — 호출자가 file 참조를 쥐고 있고
 * device->group 은 등록 이후 불변이다.
 *
 * 에러 경로: group fd 가 아니면 false.
 *
 * 호출 체인:
 *   KVM(이 트리에 없음) → [vfio_file_has_dev] → vfio_group_from_file
 */
/**
 * vfio_file_has_dev - True if the VFIO file is a handle for device
 * @file: VFIO file to check
 * @device: Device that must be part of the file
 *
 * Returns true if given file has permission to manipulate the given device.
 */
bool vfio_file_has_dev(struct file *file, struct vfio_device *device)
{
	/* [한국어] 파일이 group fd 인지 확인하며 group 을 꺼낸다. */
	struct vfio_group *group = vfio_group_from_file(file);

	/* [한국어] group fd 가 아니면 이 디바이스에 대한 권한을 증명하지 못한다. */
	if (!group)
		/* [한국어] 거짓. */
		return false;

	/* [한국어] 같은 group 에 속하면 권한이 있다고 본다. 격리 단위가 group 이므로, group fd 를 쥔
	 * 사용자는 그 안의 모든 디바이스를 다룰 자격이 있다 — 이것이 VFIO 권한 모델의 요약이다.
	 * KVM 이 "이 vfio fd 가 정말 이 디바이스의 핸들인가" 를 확인할 때 쓴다. */
	return group == device->group;
}
/* [한국어] EXPORT_SYMBOL_GPL — KVM 등 외부 모듈용. */
EXPORT_SYMBOL_GPL(vfio_file_has_dev);

/* [한국어]
 * vfio_devnode - group device 의 /dev 아래 경로 이름을 만든다
 *
 * @dev: 노드를 만들 device(여기서는 vfio_group 의 내장 device).
 * @mode: 노드 권한을 돌려줄 출력 인자. **이 함수는 건드리지 않는다**.
 * @return: kasprintf 로 할당한 "vfio/<이름>" 문자열. devtmpfs 가 쓰고 kfree 한다.
 *          할당 실패면 NULL 이고 그때는 커널이 기본 경로를 쓴다.
 *
 * 왜 필요한가: 기본값이면 노드가 /dev/<이름> 에 생기는데 VFIO 는 /dev/vfio/ 아래에
 * 모으고 싶어 한다. 그래야 컨테이너 노드(/dev/vfio/vfio)와 group 노드
 * (/dev/vfio/0, /dev/vfio/noiommu-0)가 한 디렉터리에 나란히 놓인다.
 *
 * 동작 과정: dev_name(dev) 앞에 "vfio/" 를 붙인 문자열을 GFP_KERNEL 로 할당한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(device 등록 시 devtmpfs 가 부른다). GFP_KERNEL 이라
 * 잠들 수 있다.
 *
 * 에러 경로: 할당 실패 시 NULL 을 돌려주며, 커널은 그것을 "기본 이름을 쓰라" 로 해석한다.
 *
 * 권한: mode 를 설정하지 않으므로 노드 권한은 커널 기본값이 되고, 실제 접근 권한은
 * udev 규칙이 소유자를 바꿔 부여한다. container.c 의 miscdevice 가 .mode 로
 * S_IRUGO | S_IWUGO 를 직접 지정하는 것과 대조된다 — 컨테이너는 아무나 열어도 안전한
 * 비특권 인터페이스이지만, group 노드는 곧 디바이스 접근 권한이기 때문이다.
 *
 * 호출 체인:
 *   cdev_device_add → devtmpfs/udev → [vfio_devnode] → kasprintf
 */
static char *vfio_devnode(const struct device *dev, umode_t *mode)
{
	/* [한국어] /dev 아래 경로를 "vfio/<장치이름>" 으로 만든다. 그래서 노드가 /dev/vfio/0 이나
	 * /dev/vfio/noiommu-0 으로 생긴다. 반환한 문자열은 devtmpfs 가 쓰고 kfree 한다.
	 * 두 번째 인자 mode 는 건드리지 않으므로 노드 권한은 커널 기본값이 되고, 실제 접근
	 * 권한은 udev 규칙이 소유자를 바꿔 부여한다 — container.c 의 miscdevice 가
	 * .mode 로 S_IRUGO | S_IWUGO 를 직접 지정하는 것과 대조된다. */
	return kasprintf(GFP_KERNEL, "vfio/%s", dev_name(dev));
}

/* [한국어]
 * vfio_group_init - 모듈 적재 시 group 계층과 container 계층을 세운다
 *
 * @return: 0 성공. 실패하면 그 단계의 오류를 그대로 올려 모듈 적재를 막는다.
 *
 * 왜 필요한가: /dev/vfio 아래의 모든 노드가 존재하려면 (1) 컨테이너 misc 디바이스,
 * (2) vfio class, (3) 캐릭터 디바이스 번호 영역 세 가지가 먼저 있어야 한다.
 * 이 함수가 그것을 순서대로 세우고, 어느 단계가 실패하든 앞 단계를 정확히 되감는다.
 *
 * 동작 과정:
 *  1. group_ida, group_lock, group_list 초기화(이것들은 실패할 수 없다).
 *  2. vfio_container_init(container.c:573) — /dev/vfio/vfio misc 디바이스 등록과
 *     (Kconfig 가 켜져 있으면) noiommu 백엔드 등록. container 를 먼저 세우는 이유는
 *     group 이 SET_CONTAINER 로 container 를 참조하기 때문이다.
 *  3. class_register(&vfio_class) — /sys/class/vfio.
 *  4. alloc_chrdev_region — major 하나와 minor 전체 범위(MINORMASK + 1 개)를 예약한다.
 *
 * 실행 컨텍스트: 모듈 init(프로세스 문맥). 잠들 수 있다.
 *
 * 에러 경로: err_alloc_chrdev 는 class 를, err_group_class 는 container 를 되감는다.
 * 두 라벨이 계단식으로 이어져 뒤쪽 실패가 앞쪽 획득을 모두 되돌린다.
 *
 * 호출 체인:
 *   module_init → vfio_init(vfio_main.c:5297) → [vfio_group_init]
 *     → vfio_container_init(container.c) / class_register / alloc_chrdev_region
 */
int __init vfio_group_init(void)
{
	/* [한국어] 각 단계의 결과 코드. */
	int ret;

	/* [한국어] minor 번호 할당기를 초기화한다. */
	ida_init(&vfio.group_ida);
	/* [한국어] 전역 group 목록을 지킬 뮤텍스를 초기화한다. */
	mutex_init(&vfio.group_lock);
	/* [한국어] 전역 group 목록을 빈 상태로 만든다. */
	INIT_LIST_HEAD(&vfio.group_list);

	/* [한국어] container.c:573. /dev/vfio/vfio misc 디바이스를 등록하고, Kconfig 가 켜져 있으면
	 * noiommu 백엔드까지 등록한다. group 계층보다 container 계층을 먼저 세우는 이유는,
	 * group 이 SET_CONTAINER 로 container 를 참조하기 때문이다. */
	ret = vfio_container_init();
	/* [한국어] container 초기화 실패. */
	if (ret)
		/* [한국어] 아직 아무것도 등록하지 않았으므로 그냥 돌아간다. */
		return ret;

	/* /dev/vfio/$GROUP */
	/* [한국어] /sys/class/vfio 를 만든다. 이후 만들어지는 모든 group device 가 이 class 에 속하고,
	 * devnode 콜백으로 /dev/vfio/ 아래에 노드를 얻는다. */
	ret = class_register(&vfio_class);
	/* [한국어] class 등록 실패. */
	if (ret)
		/* [한국어] container 초기화를 되돌리는 라벨로 간다. */
		goto err_group_class;

	/* [한국어] major 하나와 minor 전체 범위를 예약한다. MINORMASK + 1 개를 한꺼번에 잡아 두고
	 * 그 안에서 group_ida 가 하나씩 나눠 준다. 마지막 인자 "vfio" 는
	 * /proc/devices 에 표시될 이름이다. */
	ret = alloc_chrdev_region(&vfio.group_devt, 0, MINORMASK + 1, "vfio");
	/* [한국어] 번호 영역 예약 실패. */
	if (ret)
		/* [한국어] class 부터 되돌리는 라벨로 간다. */
		goto err_alloc_chrdev;
	/* [한국어] 성공. 이 시점부터 vendor 드라이버가 디바이스를 등록하면 group 노드가 생긴다. */
	return 0;

/* [한국어] chrdev 영역 예약 실패만 여기로 온다. */
err_alloc_chrdev:
	/* [한국어] class_register 를 되돌린다. */
	class_unregister(&vfio_class);
/* [한국어] class 등록 실패가 합류한다. */
err_group_class:
	/* [한국어] container.c:598. misc 디바이스와 noiommu 백엔드 등록을 되돌린다. */
	vfio_container_cleanup();
	/* [한국어] 오류를 그대로 vfio_init 에 올린다. vfio_init 은 모듈 적재를 실패시킨다. */
	return ret;
}

/* [한국어]
 * vfio_group_cleanup - vfio_group_init 을 역순으로 되감는다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 모듈 언로드 시 등록한 것을 모두 반납해야 한다. 순서가 init 의 역순인
 * 이유는 의존 관계 때문이다 — container 는 group 보다 먼저 세웠으므로 나중에 내린다.
 *
 * 동작 과정:
 *  1. 남아 있는 group 이 없는지 WARN_ON 으로 확인한다. 남아 있다면 vendor 드라이버가
 *     먼저 정리되지 않은 것이고, 그대로 두면 남은 group 의 cdev 가 해제된 fops 를 가리킨다.
 *  2. ida_destroy — 아직 할당된 minor 가 있으면 커널이 경고한다.
 *  3. unregister_chrdev_region — init 과 같은 개수를 넘겨야 한다.
 *  4. class_unregister — /sys/class/vfio 제거.
 *  5. vfio_container_cleanup(container.c:598) — misc 디바이스와 noiommu 백엔드 해제.
 *
 * 실행 컨텍스트: 모듈 exit(프로세스 문맥). 잠들 수 있다.
 *
 * 에러 경로: 없다. 되감기는 실패할 수 없다.
 *
 * 호출 체인:
 *   module_exit → vfio_exit(vfio_main.c) → [vfio_group_cleanup]
 *     → ida_destroy / unregister_chrdev_region / class_unregister /
 *       vfio_container_cleanup(container.c)
 */
void vfio_group_cleanup(void)
{
	/* [한국어] 모듈이 내려가는데 group 이 남아 있으면 vendor 드라이버가 먼저 정리되지 않은 것이다.
	 * 이 상태에서 계속 진행하면 남은 group 의 cdev 가 해제된 fops 를 가리키게 된다. */
	WARN_ON(!list_empty(&vfio.group_list));
	/* [한국어] minor 할당기를 파괴한다. 아직 할당된 것이 있으면 커널이 경고한다. */
	ida_destroy(&vfio.group_ida);
	/* [한국어] vfio_group_init 의 alloc_chrdev_region 을 되돌린다. 같은 개수를 넘겨야 한다. */
	unregister_chrdev_region(vfio.group_devt, MINORMASK + 1);
	/* [한국어] /sys/class/vfio 를 없앤다. */
	class_unregister(&vfio_class);
	/* [한국어] container 계층을 마지막에 정리한다. init 의 역순이다 — init 은 container 를 먼저
	 * 세웠으므로 cleanup 은 나중에 내린다. */
	vfio_container_cleanup();
}
