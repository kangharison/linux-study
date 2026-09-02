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
 * [한국어 설명] VFIO 코어 — 물리 디바이스를 IOMMU 뒤에서 사용자 공간에
 * 넘겨주는 프레임워크의 뼈대 (drivers/vfio/vfio_main.c)
 *
 * 이 블록 안에서 다른 파일을 가리킬 때만 줄 번호를 적었다. 이 파일 자신은
 * 주석이 붙으며 줄 번호가 계속 바뀌므로 함수 이름으로만 가리킨다.
 *
 * === 파일의 역할 ===
 * VFIO(Virtual Function I/O) 서브시스템 전체의 **진입점이자 중심**이다.
 * drivers/vfio 아래 나머지 파일(group.c, container.c, device_cdev.c,
 * iommufd.c, virqfd.c, debugfs.c, vfio_iommu_type1.c, pci 하위)은 모두
 * 이 파일이 정의한 객체 수명과 파일 디스크립터 모델 위에서 돈다. 이 파일이
 * 실제로 맡는 일은 다음 다섯 가지다.
 *
 *  (1) **디바이스 등록/해제** — vendor 드라이버(vfio-pci, vfio-platform,
 *      mdev 등)가 자기 하드웨어를 VFIO 에 내놓는 창구.
 *      _vfio_alloc_device → vfio_register_group_dev 또는
 *      vfio_register_emulated_iommu_dev → vfio_unregister_group_dev.
 *  (2) **fd 수명과 refcount** — struct vfio_device 하나에 여러 fd 가 열릴 수
 *      있고, "첫 open" 과 "마지막 close" 에서만 vendor 의 open_device 와
 *      close_device 를 부른다(vfio_df_open, vfio_df_close).
 *  (3) **ioctl 디스패치** — vfio_device_fops 의 unlocked_ioctl 이
 *      VFIO_DEVICE_BIND_IOMMUFD, ATTACH/DETACH_IOMMUFD_PT,
 *      VFIO_DEVICE_FEATURE, VFIO_DEVICE_GET_REGION_INFO 를 직접 처리하고
 *      나머지는 vendor 의 ops->ioctl 로 넘긴다.
 *  (4) **마이그레이션 상태 기계** — vfio_mig_get_next_state 의 8x8 전이표.
 *      라이브 마이그레이션 중 디바이스 상태를 한 걸음씩 옮긴다.
 *  (5) **vendor 공용 헬퍼** — capability chain 조립(vfio_info_cap_add),
 *      IRQ set 인자 검증(vfio_set_irqs_validate_and_prepare),
 *      게스트 메모리 pin 과 읽기/쓰기(vfio_pin_pages, vfio_dma_rw).
 *
 * 반대로 이 파일이 **하지 않는** 일도 분명하다. IOVA 매핑 자체(page table
 * 프로그래밍)는 vfio_iommu_type1.c 또는 iommufd 가 하고, group 객체의 생성과
 * 소멸은 group.c 가, container 는 container.c 가, cdev 노드의 open 은
 * device_cdev.c 가 한다. 이 파일은 그것들을 **엮는 축**이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 저장소는 NVMe I/O 경로를 따라 읽어 왔다. block/ 의 blk-mq 코어,
 * drivers/nvme 의 커널 NVMe 드라이버, drivers/pci 의 PCI 코어가 이미 모두
 * 주석 완료 상태다. VFIO 는 **그 경로를 통째로 우회하는 쪽**의 출발점이다.
 *
 *   [커널 내부 NVMe 경로 — 이 트리에 이미 주석됨]
 *     사용자 read/write → VFS → block/blk-mq
 *       → drivers/nvme/host/pci.c:4744 의 nvme_probe 가 잡은 컨트롤러
 *       → PCI BAR MMIO 도어벨 → NVMe SQ/CQ → 인터럽트
 *
 *   [VFIO 경로 — 이 파일이 시작점]
 *     사용자가 /sys/bus/pci/devices/<주소>/driver_override 에 vfio-pci 를
 *     써 넣는다. 그 sysfs 속성이 어떻게 바인딩을 강제하는지는 이 트리의
 *     drivers/pci/pci-driver.c:362~404 에 이미 주석돼 있다.
 *     → 커널 nvme 드라이버 unbind, vfio-pci 가 probe
 *       → drivers/vfio/pci/vfio_pci_core.c:2126
 *          vfio_pci_core_register_device
 *       → [이 파일] vfio_register_group_dev
 *          → __vfio_register_dev → vfio_device_add 로 cdev 와 그룹 등록
 *     → 사용자 공간(QEMU, DPDK, SPDK)이 fd 를 열고 BAR 를 mmap
 *       → [이 파일] vfio_device_fops_mmap → vendor ops->mmap
 *     → 사용자 공간이 NVMe 도어벨을 **직접** 쓴다. 커널 nvme 드라이버도
 *       blk-mq 도 이 경로에는 없다.
 *
 * 즉 이 파일은 "커널이 하드웨어를 소유하는 세계" 와 "사용자 공간이
 * 하드웨어를 소유하는 세계" 의 경계면이다. 경계면의 안전은 전적으로 IOMMU
 * 가 보장하며(디바이스가 찍는 DMA 주소는 IOVA 이고 그 변환표는 사용자가
 * 소유한 IOAS 뿐이다), 이 파일은 그 IOMMU 결합이 **성립하기 전에는 어떤
 * device ioctl 도 통과시키지 않는다**(df->access_granted 게이트).
 *
 * 실행 컨텍스트는 전부 **호스트 커널 프로세스 문맥**이다. 인터럽트 문맥에서
 * 실행되는 코드는 이 파일에 없다 — mutex 를 잡고 잠들 수 있고,
 * copy_to_user 와 copy_from_user 로 사용자 페이지를 만지며,
 * vfio_unregister_group_dev 는 완료를 기다리며 몇 초씩 자기도 한다.
 *
 * === 타 모듈과의 연결 ===
 *  - include/linux/vfio.h
 *      : **외부 ABI**. struct vfio_device(39줄), vfio_device_ops(116줄),
 *        vfio_migration_ops, vfio_log_ops 의 정의처. 이 파일이 구현하는
 *        vfio_register_group_dev 등의 prototype 도 여기(339~356줄).
 *  - drivers/vfio/vfio.h
 *      : **내부 ABI**(이미 주석 완료). struct vfio_device_file(107줄),
 *        struct vfio_group, Kconfig 별 stub. 이 파일은 그 stub 덕분에
 *        CONFIG_VFIO_GROUP, CONFIG_VFIO_CONTAINER, CONFIG_VFIO_DEVICE_CDEV
 *        분기를 #ifdef 없이 쓴다.
 *  - drivers/vfio/group.c
 *      : legacy /dev/vfio/<groupID>. 이 파일의 vfio_df_open 을 group.c:206
 *        에서 부르고, df->access_granted 를 group.c:220 에서 세운다.
 *        group.c 는 자기만의 파일 정적 객체(group.c:24 의 struct vfio)를
 *        따로 두며, 이 파일의 vfio 와는 **이름만 같고 다른 객체**다.
 *  - drivers/vfio/device_cdev.c
 *      : 현대 /dev/vfio/devices/vfio<N>. 이 파일의 vfio_device_fops 에
 *        .open 으로 등록되는 vfio_device_fops_cdev_open(device_cdev.c:23)
 *        의 구현처. BIND_IOMMUFD 처리(device_cdev.c:83)도 여기.
 *  - drivers/vfio/iommufd.c (이미 주석 완료)
 *      : vfio_df_iommufd_bind 와 vfio_df_iommufd_unbind 의 구현.
 *        이 파일은 첫 open 과 마지막 close 에서 그것을 호출한다.
 *  - drivers/vfio/container.c, vfio_iommu_type1.c
 *      : legacy container 와 그 IOMMU backend. 이 파일은 container 를 직접
 *        알지 못하고 vfio_device_has_container, vfio_device_container_pin_pages
 *        같은 vfio.h 의 얇은 헬퍼로만 접근한다.
 *  - drivers/vfio/pci/vfio_pci_core.c 및 그 파생 드라이버들
 *      : 이 파일의 최대 소비자. vfio_assign_device_set(2170~2178줄),
 *        vfio_register_group_dev(2208줄), vfio_device_set_open_count(2524줄),
 *        vfio_find_device_in_devset(835줄과 2341줄) 를 쓴다.
 *  - drivers/vfio/debugfs.c, virqfd.c
 *      : 모듈 init 과 exit 에서 이 파일이 초기화하고 정리한다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct vfio                 : 모듈 전역 상태 4필드. device class,
 *                                  device index 할당기, pseudo-fs mount.
 *  - vfio_assign_device_set      : "함께 리셋되어야 하는 디바이스 묶음"을
 *                                  xarray 로 singleton 관리.
 *  - __vfio_register_dev         : 등록의 실체. dev_set → 이름 → group →
 *                                  cache coherency 검사 → cdev 등록 →
 *                                  refcount 를 1 로 두고 개시.
 *  - vfio_unregister_group_dev   : 등록의 역순. **사용자가 fd 를 놓을 때까지
 *                                  무한 대기**하는 이 파일의 유일한 지점.
 *  - vfio_df_open / vfio_df_close: open_count 의 0↔1 전이에서만 vendor
 *                                  콜백과 IOMMU 결합을 여닫는다.
 *  - vfio_device_fops            : device fd 의 file_operations. group 경로와
 *                                  cdev 경로가 **같은 fops 를 공유**한다.
 *  - vfio_device_fops_unl_ioctl  : ioctl 디스패처. 권한 게이트 → 런타임 PM
 *                                  확보 → cdev 전용 → 공통 → vendor 순.
 *  - vfio_mig_get_next_state     : 라이브 마이그레이션 FSM 의 한 걸음 계산.
 *  - vfio_info_cap_add / _shift  : 사용자에게 돌려줄 가변 길이 capability
 *                                  chain 조립기.
 *  - vfio_pin_pages / vfio_dma_rw: mdev 계열 vendor 가 게스트 메모리를 대신
 *                                  만질 때의 두 경로(container, iommufd) 분기점.
 *
 * === device / group / container 3층 모델 ===
 * VFIO 의 사용자 공간 ABI 는 두 세대가 공존한다. 이 파일은 양쪽 모두를 같은
 * struct vfio_device 위에 얹는다.
 *
 *  [1세대 — group + container]  CONFIG_VFIO_GROUP, CONFIG_VFIO_CONTAINER
 *
 *      container (/dev/vfio/vfio)      ... IOVA 주소공간 1개
 *          └ group (/dev/vfio/<gid>)   ... IOMMU 가 정한 격리 단위
 *              └ device fd             ... GROUP_GET_DEVICE_FD 로 얻음
 *
 *      **격리의 근거**: IOMMU 는 PCIe ACS 와 토폴로지 때문에 개별 함수가
 *      아니라 "group" 단위로만 서로 다른 주소공간을 줄 수 있다. 그래서
 *      사용자에게 device 를 주기 전에 group 전체의 소유권을 요구한다 —
 *      group 안에 커널 드라이버가 붙은 디바이스가 하나라도 남아 있으면
 *      열 수 없다.
 *
 *      이 파일이 관여하는 지점: group.c 가 만든 fd 에 vfio_device_fops 를
 *      물리고, vfio_df_open 으로 첫 open 처리를 해 준다.
 *      df->group 이 NULL 이 아니라는 것이 1세대임을 나타내는 표식이며,
 *      **1세대만이 한 device 를 여러 번 open 하는 것을 허용한다**.
 *
 *  [2세대 — cdev + iommufd]     CONFIG_VFIO_DEVICE_CDEV, CONFIG_IOMMUFD
 *
 *      iommufd (/dev/iommu)            ... IOAS 와 HWPT 를 1급 객체로
 *          ↕ BIND_IOMMUFD
 *      device (/dev/vfio/devices/vfio<N>)
 *
 *      group 이라는 중간 객체를 ABI 에서 지웠다. 격리 검사는 iommufd 가 하고,
 *      사용자는 device fd 하나만 연다. 다만 **open 직후의 fd 는 아무 권한이
 *      없고**, BIND_IOMMUFD 를 통과해야 비로소 다른 ioctl 이 열린다.
 *      이 파일에서 그 게이트가 vfio_device_fops_unl_ioctl 의 첫머리다.
 *      BIND_IOMMUFD 만 무조건 통과시키고, 나머지는 df->access_granted 를
 *      smp_load_acquire 로 확인한다.
 *
 *  [공통] 어느 세대든 fd 하나당 struct vfio_device_file 이 하나 붙고
 *      (vfio_allocate_device_file), 그 df 가 device 를 가리킨다. device
 *      본체의 open_count 는 df 들의 합이며, 0→1 과 1→0 에서만 vendor 의
 *      open_device 와 close_device 가 불린다.
 *
 * === SPDK 류 사용자 공간 드라이버가 실제로 쓰는 것 ===
 * SPDK(와 그 밑의 DPDK)가 NVMe 컨트롤러를 커널에서 뺏어 직접 모는 절차를
 * 이 파일의 어느 함수가 받는지로 적으면 다음과 같다. 사용자 공간 라이브러리
 * 자체는 이 트리에 없으므로 절차는 ABI 관점에서만 기술한다.
 *
 *   1. 커널 nvme 드라이버 unbind 뒤 driver_override 로 vfio-pci 바인딩
 *        → vfio-pci 가 probe 되며 이 파일의 vfio_register_group_dev 도달.
 *   2. open("/dev/vfio/vfio")                  → container fd (container.c)
 *   3. open("/dev/vfio/<gid>")                 → group fd  (group.c)
 *   4. ioctl(group, VFIO_GROUP_SET_CONTAINER)  → group.c
 *   5. ioctl(container, VFIO_SET_IOMMU, TYPE1) → container.c → type1 backend
 *   6. ioctl(container, VFIO_IOMMU_MAP_DMA)    → vfio_iommu_type1.c
 *        : 사용자 hugepage 를 IOVA 에 매핑. 이후 NVMe 가 찍는 PRP 와 SGL
 *          주소는 전부 이 IOVA 다.
 *   7. ioctl(group, VFIO_GROUP_GET_DEVICE_FD, "0000:xx:00.0")
 *        → group.c:305 의 핸들러 → vfio_device_open_file
 *        → **[이 파일] vfio_df_open**
 *   8. ioctl(dev, VFIO_DEVICE_GET_REGION_INFO, BAR0)
 *        → **[이 파일] vfio_get_region_info** → vendor 의
 *          get_region_info_caps (vfio-pci 는 vfio_pci.c:135 에서 등록)
 *   9. mmap(dev_fd, ..., info.offset)
 *        → **[이 파일] vfio_device_fops_mmap** → vendor ops->mmap
 *        : 여기서 얻은 가상주소가 곧 NVMe 컨트롤러 레지스터다. CAP, CC, AQA
 *          그리고 SQ/CQ 도어벨을 사용자 공간이 직접 쓴다.
 *  10. ioctl(dev, VFIO_DEVICE_SET_IRQS, eventfd)
 *        → **[이 파일] vfio_device_fops_unl_ioctl 의 default 분기**
 *          → vendor ops->ioctl. 인자 검증만 이 파일의
 *          vfio_set_irqs_validate_and_prepare 가 공용으로 제공한다.
 *          eventfd 로 MSI-X 를 받는 실제 배선은 virqfd.c 와 vfio-pci 쪽이다.
 *
 * 여기서 중요한 관찰 하나: 위 목록에 **데이터 경로가 없다**. VFIO 는 셋업
 * (매핑, mmap, 인터럽트 배선)만 담당하고, 일단 셋업이 끝나면 NVMe 명령 제출과
 * 완료 폴링은 커널을 한 번도 거치지 않는다. 이 파일의 read, write, ioctl 이
 * I/O 마다 불리는 일은 없다. GPU 와 스토리지 사이의 직접 DMA 경로도 같은
 * 구조다 — IOVA 를 GPU BAR 로 매핑해 두면 그 뒤 전송에 커널이 개입하지 않는다.
 *
 * === refcount 와 락 지도 (이 파일의 핵심) ===
 *  refcount 4종이 서로 다른 것을 지킨다. 어느 하나를 빠뜨렸을 때 무슨 일이
 *  생기는지는 각 함수 주석에 적었고, 여기서는 전체 그림만 둔다.
 *
 *   (a) device->device.kref  (struct device 내장, put_device 로 감소)
 *       : struct vfio_device **메모리 자체**의 수명. 0 이 되면
 *         vfio_device_release 가 불려 kvfree 까지 간다.
 *         vendor 는 vfio_put_device() 로만 놓는다.
 *   (b) device->refcount     (refcount_t, __vfio_register_dev 에서 1 로 시작)
 *       : **등록된 상태에서 사용자가 잡고 있는 수**. fd 하나당 1.
 *         vfio_device_try_get_registration 이 0 에서 실패하도록 되어 있어,
 *         unregister 가 시작된 뒤의 새 open 을 막는다. 마지막 1 이 빠지면
 *         vfio_device_put_registration 이 complete 로 대기자를 깨운다.
 *   (c) device->open_count   (unsigned int, dev_set->lock 보호)
 *       : vendor 의 open_device 와 close_device 를 부를 시점 판단용.
 *         refcount 와 달리 **BIND 를 통과한 fd 만** 센다.
 *   (d) module 참조          (vfio_df_device_first_open 의 try_module_get 과
 *         짝이 되는 module_put 이 실패 경로와 last_close 두 곳에 있다)
 *       : 디바이스가 열려 있는 동안 vendor 모듈이 rmmod 되지 못하게 한다.
 *
 *  락 3종:
 *   - dev_set->lock (mutex)  : open_count 와 df->iommufd 를 지킨다.
 *     같은 리셋 도메인(같은 PCI slot 또는 bus)의 디바이스들이 한 락을
 *     공유하므로, vfio-pci 의 "형제 함수가 모두 닫혔는가" 검사가 성립한다.
 *     이 파일은 이 락을 잡지 않고 **호출자가 잡고 왔는지만 확인**한다
 *     (여러 함수 첫머리의 lockdep_assert_held).
 *   - xa_lock(&vfio_device_set_xa) (spinlock) : device_set 객체의 생성과
 *     소멸 원자성. 짧은 구간만 잡으며 그 안에서 잠들지 않는다.
 *   - df->kvm_ref_lock (spinlock) : KVM 포인터 갱신. KVM 쪽이 비동기로
 *     부를 수 있어 짧은 원자 구간이 필요하다.
 *
 *  메모리 배리어 1종:
 *   - access_granted 는 store_release(group.c:220, device_cdev.c:157) 와
 *     load_acquire(이 파일의 ioctl, read, write, mmap 네 곳) 쌍이다.
 *     release 앞의 모든 설정(iommufd 바인딩, vendor open_device 완료)이
 *     acquire 를 통과한 쪽에서 반드시 보이게 만든다. 이게 없으면 다른 CPU
 *     에서 "권한은 켜졌는데 바인딩은 아직 안 보이는" 창이 생긴다.
 */


/* [한국어] struct cdev 정의. 이 파일은 cdev 를 직접 다루지 않지만 include/linux/vfio.h 가
 * struct vfio_device 안에 cdev 를 내장하고 있고, 그 헤더도 같은 헤더를 이미
 * 포함한다(include/linux/vfio.h:16). 여기 명시한 것은 중복이지만 의도를 드러낸다. */
#include <linux/cdev.h>
/* [한국어] compat_ptr_ioctl 을 얻기 위해 필요하다. 32비트 프로세스가 64비트 커널의
 * ioctl 을 부를 때 포인터 폭만 맞춰 주는 표준 어댑터이며,
 * vfio_device_fops 의 compat_ioctl 슬롯에 그대로 꽂힌다. */
#include <linux/compat.h>
/* [한국어] struct device, class_create, dev_set_name, device_initialize, dev_warn 계열.
 * 이 파일이 만드는 모든 sysfs 노드와 디바이스 객체의 기반이다. */
#include <linux/device.h>
/* [한국어] struct file, struct file_operations, struct inode, fd_install,
 * get_unused_fd_flags, simple_pin_fs, alloc_anon_inode 등 파일 계층 전부.
 * VFIO 의 사용자 인터페이스가 파일 디스크립터라서 가장 중심이 되는 헤더다. */
#include <linux/fs.h>
/* [한국어] ida_alloc_max, ida_free, ida_init, ida_destroy — device index 할당기.
 * 이 index 가 캐릭터 디바이스 minor 번호이자 vfio<N> 의 N 이 된다. */
#include <linux/idr.h>
/* [한국어] device_iommu_capable 과 IOMMU_CAP_CACHE_COHERENCY, IOMMU_WRITE.
 * 등록 시 캐시 일관성 검사와 pin 시 보호 플래그 해석에 쓴다.
 * 이 헤더의 실제 구현부(drivers/iommu)는 이 트리에 없어 확인 못 함. */
#include <linux/iommu.h>
/* [한국어] KVM 이 커널에 포함된 빌드에서만 KVM 헤더를 끌어온다. VFIO 는 KVM 에
 * 링크 의존하지 않고 symbol_get 으로 런타임 조회를 하지만, 타입 선언
 * (struct kvm)과 심볼 이름은 컴파일 시 필요하다. */
#if IS_ENABLED(CONFIG_KVM)
#include <linux/kvm_host.h>
#endif
/* [한국어] list_add_tail, list_del, list_for_each_entry, INIT_LIST_HEAD.
 * device set 의 디바이스 목록을 잇는 데 쓴다. */
#include <linux/list.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(struct miscdevice, misc_register 계열)을
 * 쓰는 곳을 이 파일에서 찾지 못했다. 이 파일은 misc 디바이스를 만들지 않고
 * class 와 chrdev 영역을 직접 다룬다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다. */
#include <linux/miscdevice.h>
/* [한국어] MODULE_LICENSE 계열 선언, module_param_named, THIS_MODULE,
 * try_module_get 과 module_put. 첫 open 이 vendor 모듈을 붙잡는 데 필수다. */
#include <linux/module.h>
/* [한국어] struct vfsmount — 전역 vfio 구조체의 vfs_mount 필드 타입.
 * pseudo 파일시스템 마운트를 붙들어 둔다. */
#include <linux/mount.h>
/* [한국어] mutex_init, mutex_lock/unlock, mutex_destroy.
 * device set 의 lock 이 이 타입이다. */
#include <linux/mutex.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(pci_dev, pci_ 계열 함수)을 쓰는 곳을
 * 이 파일에서 찾지 못했다. PCI 특화 처리는 전부 drivers/vfio/pci 아래에 있다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/pci.h>
/* [한국어] init_pseudo — 마운트 지점 없는 내부 전용 파일시스템을 만드는 헬퍼.
 * vfio_fs_init_fs_context 가 이것 하나 때문에 존재한다. */
#include <linux/pseudo_fs.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(rw_semaphore, down_read 계열)을 쓰는 곳을
 * 이 파일에서 찾지 못했다. 이 파일의 동기화는 mutex 와 spinlock 과 xa_lock 뿐이다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/rwsem.h>
/* [한국어] current 매크로와 task_struct 의 comm, mm 필드.
 * vfio_unregister_group_dev 의 경고 메시지와 vfio_dma_rw 의 kthread 감지에 쓴다. */
#include <linux/sched.h>
/* [한국어] struct seq_file 과 seq_printf — /proc/<pid>/fdinfo 출력용. */
#include <linux/seq_file.h>
/* [한국어] kzalloc, kmalloc, kfree, krealloc, kvzalloc, kvfree.
 * vfio_device 본체와 capability chain 버퍼 할당에 쓴다. */
#include <linux/slab.h>
/* [한국어] S_IRUGO 와 S_IWUSR — 아래 module_param_named 의 sysfs 권한 비트. */
#include <linux/stat.h>
/* [한국어] memset 과 memcpy — capability chain 조립에서 쓴다. */
#include <linux/string.h>
/* [한국어] copy_to_user 와 copy_from_user. 이 파일의 모든 ioctl 핸들러가 사용자
 * 메모리를 이 두 함수로만 만진다. */
#include <linux/uaccess.h>
/* [한국어] VFIO 외부 ABI. struct vfio_device, vfio_device_ops, vfio_migration_ops,
 * vfio_log_ops, vfio_check_feature 등 이 파일이 구현하고 소비하는 모든 타입. */
#include <linux/vfio.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(wait_queue_head_t, wake_up 계열)을 쓰는
 * 곳을 이 파일에서 찾지 못했다. 이 파일의 대기는 completion 하나뿐이다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/wait.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(signal_pending 계열)을 직접 쓰는 곳을
 * 이 파일에서 찾지 못했다. 다만 vfio_unregister_group_dev 가
 * wait_for_completion_interruptible_timeout 으로 시그널에 반응하므로
 * 의도상 관련은 있다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/sched/signal.h>
/* [한국어] pm_runtime_resume_and_get 과 pm_runtime_put — ioctl 처리 동안 디바이스를
 * D0 로 붙잡는 두 래퍼가 이것을 쓴다. */
#include <linux/pm_runtime.h>
/* [한국어] interval_tree_insert, _remove, _iter_first, _iter_next 와
 * struct interval_tree_node. DMA 로깅 구간을 정렬 트리로 다룬다. */
#include <linux/interval_tree.h>
/* [한국어] iova_bitmap_alloc, _free, _for_each — 사용자 공간 더티 비트맵을 조각조각
 * 핀해 가며 순회하는 도우미. */
#include <linux/iova_bitmap.h>
/* [한국어] iommufd_access_pin_pages, _unpin_pages, _rw 와 IOMMUFD_ACCESS_RW 플래그.
 * 2세대 경로의 게스트 메모리 접근이 전부 이 API 로 간다. */
#include <linux/iommufd.h>
/* [한국어] drivers/vfio 내부 전용 헤더. struct vfio_device_file, vfio_df_ 계열
 * prototype, Kconfig 별 stub 이 모두 여기 있으며 이미 주석 완료 상태다. */
#include "vfio.h"

/* [한국어] 모듈 버전 문자열. 아래 MODULE_VERSION 과 부팅 배너에 함께 쓰인다.
 * 0.3 은 VFIO ABI 세대가 아니라 이 메타 드라이버 자체의 버전이다. */
#define DRIVER_VERSION	"0.3"
/* [한국어] MODULE_AUTHOR 에 넣을 원저자 표기. */
#define DRIVER_AUTHOR	"Alex Williamson <alex.williamson@redhat.com>"
/* [한국어] 모듈 설명이자 부팅 배너 문구. "User Level meta-driver" 라는 표현이
 * 이 서브시스템의 성격을 그대로 말한다 — 하드웨어를 모는 드라이버가 아니라
 * 사용자 공간이 드라이버가 되게 해 주는 드라이버다. */
#define DRIVER_DESC	"VFIO - User Level meta-driver"

/* [한국어] pseudo 파일시스템의 매직 넘버. 값 0x5646494f 는 ASCII 로 'V','F','I','O'
 * 네 글자다(0x56='V', 0x46='F', 0x49='I', 0x4f='O'). init_pseudo 에 넘겨져
 * super block 의 s_magic 이 되고, 이 파일시스템을 다른 것과 구별한다. */
#define VFIO_MAGIC 0x5646494f /* "VFIO" */

/* [한국어] 모듈 전역 상태를 담는 파일 정적 객체. 익명 구조체 타입에 vfio 라는 이름의
 * 인스턴스 하나만 만든다 — 이 파일 밖에서는 볼 수 없다.
 * drivers/vfio/group.c:24 에도 같은 이름의 다른 파일 정적 객체가 있으며
 * 필드 구성이 완전히 다르다. 두 파일이 서로의 것을 참조하지는 않는다. */
static struct vfio {
	/* [한국어] 이 모듈이 만드는 device class. class_create("vfio-dev") 의 결과이며
	 * /sys/class/vfio-dev 로 보인다.
	 * 설정자: vfio_init 이 만들고, vfio_cleanup 과 vfio_init 의 실패 경로가
	 * 파괴한 뒤 NULL 로 비운다.
	 * 읽는 자: vfio_init_device 가 각 디바이스의 device.class 에 꽂고,
	 * vfio_cdev_init 에 넘겨 devnode 콜백을 달게 한다.
	 * 값 범위: 유효 포인터 또는 NULL(모듈 초기화 전후).
	 * 동기화: 모듈 init/exit 에서만 쓰기 때문에 별도 락이 없다. */
	struct class			*device_class;
	/* [한국어] device index 할당기. 0 부터 MINORMASK 까지의 유일한 정수를 나눠 준다.
	 * 설정자: vfio_init 의 ida_init, vfio_cleanup 의 ida_destroy.
	 * 읽는 자: vfio_init_device 가 ida_alloc_max 로 얻고
	 * vfio_device_release 와 vfio_init_device 의 실패 경로가 ida_free 로 돌려준다.
	 * 값 범위: 할당된 index 들의 비트 집합.
	 * 동기화: ida 자체가 내부 락을 가지므로 호출자가 락을 잡지 않는다. */
	struct ida			device_ida;
	/* [한국어] pseudo 파일시스템의 마운트 포인터.
	 * 설정자와 읽는 자: simple_pin_fs 와 simple_release_fs 가 이 포인터와 아래
	 * fs_count 를 짝으로 갱신한다. 첫 pin 에서 마운트가 생기고 마지막 release 에서
	 * 사라진다.
	 * 값 범위: 마운트된 동안 유효 포인터, 그 밖에는 의미 없음.
	 * 동기화: simple_pin_fs 계열이 내부적으로 직렬화한다. */
	struct vfsmount			*vfs_mount;
	/* [한국어] 위 마운트의 참조 수. 디바이스 하나당 하나씩 늘어난다.
	 * 설정자와 읽는 자: 위와 같은 두 함수.
	 * 값 범위: 0 이면 마운트 없음. 등록된 디바이스 수와 대체로 같다.
	 * 동기화: 위와 같다. */
	int				fs_count;
/* [한국어] 구조체 태그가 없는 익명 타입에 이 하나의 인스턴스만 붙인다.
 * static 이라 이 파일 밖에서는 접근할 수 없다. */
} vfio;

/* [한국어] noiommu 모드는 Kconfig 로 꺼 둘 수 있다. 꺼져 있으면 아래 변수와 모듈
 * 파라미터가 통째로 사라지고, drivers/vfio/vfio.h 가 컴파일 시 상수 false 를
 * 대신 제공해 호출 측 분기가 죽은 코드로 제거된다. */
#ifdef CONFIG_VFIO_NOIOMMU
/* [한국어] IOMMU 없는 디바이스도 VFIO 로 노출할지 정하는 전역 스위치.
 * 설정자: 아래 module_param_named 를 통해 관리자가 부팅 인자나 sysfs 로 쓴다.
 * 읽는 자: group.c 가 noiommu group 을 만들지 판단할 때.
 * 값 범위: false 가 기본. true 면 커널이 taint 된다.
 * 동기화: 없다. __read_mostly 로 두어 자주 읽히는 변수를 쓰기 잦은 변수와
 * 다른 캐시 라인에 모은다 — 거짓 공유를 줄이는 배치 지시자다. */
bool vfio_noiommu __read_mostly;
/* [한국어] 부팅 인자와 sysfs 노드로 위 변수를 노출한다. 노출 이름을 변수명과 다르게
 * (enable_unsafe_noiommu_mode) 붙여, 사용자에게 위험성을 이름 자체로 알린다.
 * 권한 비트는 모두 읽기 가능하고 소유자만 쓰기 가능이다. */
module_param_named(enable_unsafe_noiommu_mode,
		   vfio_noiommu, bool, S_IRUGO | S_IWUSR);
/* [한국어] 이 설명 문자열은 modinfo 로 보인다. 격리 없음, DMA 번역 없음, 호스트 커널
 * 보호 없음, 가상머신 할당 불가, RAWIO 권한 필요, 커널 taint — 이 모드가
 * 포기하는 것을 모두 나열한다. */
MODULE_PARM_DESC(enable_unsafe_noiommu_mode, "Enable UNSAFE, no-IOMMU mode.  This mode provides no device isolation, no DMA translation, no host kernel protection, cannot be used for device assignment to virtual machines, requires RAWIO permissions, and will taint the kernel.  If you do not know what this is for, step away. (default: false)");
#endif

/* [한국어] device set 들을 담는 전역 xarray. 키는 set_id 포인터를 정수로 본 값이고,
 * 값은 struct vfio_device_set 포인터다.
 * 설정자: vfio_assign_device_set 이 __xa_cmpxchg 로 넣고,
 * vfio_release_device_set 이 마지막 디바이스가 빠질 때 __xa_erase 로 지운다.
 * 읽는 자: 같은 두 함수.
 * 값 범위: 살아 있는 device set 들.
 * 동기화: xa_lock 으로 감싼다. DEFINE_XARRAY 는 컴파일 시 초기화하므로
 * 모듈 init 에서 따로 만들 필요가 없고, vfio_cleanup 의 xa_destroy 만 짝이 없다. */
static DEFINE_XARRAY(vfio_device_set_xa);

/* [한국어]
 * vfio_assign_device_set - 디바이스를 "함께 리셋되는 묶음(device set)" 에 넣는다
 *
 * @device: 아직 등록 전인 vfio_device. 이 함수가 device->dev_set 과
 *          device->dev_set_list 두 필드를 채운다.
 * @set_id: 묶음을 식별하는 **불투명 포인터 값**. 같은 값을 준 디바이스끼리
 *          한 set 에 모인다. vfio-pci 는 리셋 범위에 따라 pdev 자신,
 *          pdev->slot, pdev->bus 중 하나를 준다
 *          (drivers/vfio/pci/vfio_pci_core.c:2170~2178).
 * @return: 0 성공. -EINVAL 이면 set_id 가 NULL(WARN_ON 동반),
 *          -ENOMEM 이면 새 set 할당 실패, 그 밖에 xarray 내부 오류를
 *          xa_err 로 변환해 돌려준다. 호출자(vendor probe)는 실패 시
 *          등록을 중단한다.
 *
 * 왜 필요한가: PCIe 함수 리셋(FLR)은 보통 함수 하나만 건드리지만, bus reset 과
 * slot reset 은 그 아래 **모든 함수를 함께** 리셋한다. 그러므로 "디바이스 A 를
 * 리셋해도 되는가" 는 A 혼자가 아니라 같은 리셋 범위의 모든 함수가 VFIO 에
 * 잡혀 있고 모두 닫혀 있는지로 판단해야 한다. device set 은 그 판단 단위이며,
 * 동시에 dev_set->lock 이라는 **공유 mutex** 를 제공해 open_count 조작을
 * 묶음 단위로 직렬화한다.
 *
 * 동작 과정:
 *  1. set_id 가 NULL 이면 WARN 후 -EINVAL. NULL 은 xarray 인덱스로 쓸 수 없고,
 *     묶음을 식별할 수도 없다.
 *  2. xa_lock 을 잡고 set_id 를 인덱스로 기존 set 을 찾는다. 있으면 곧장
 *     found_get_ref 로 뛰어 참조만 늘린다 — 가장 흔한 경로.
 *  3. 없으면 락을 놓고(할당은 잠들 수 있으므로) 새 set 을 kzalloc 한다.
 *  4. 다시 락을 잡고 __xa_cmpxchg 로 "여전히 비어 있으면 내 것을 넣는다".
 *     그 사이 다른 CPU 가 먼저 넣었다면 내 것을 버리고 남의 것을 쓴다.
 *     이 cmpxchg 가 **singleton 보장의 핵심**이다.
 *  5. device_count 를 1 늘리고, set 의 mutex 아래에서 device 를 리스트에 붙인다.
 *
 * 실행 컨텍스트: vendor 드라이버의 probe, 즉 프로세스 문맥. GFP_KERNEL 을 쓰므로
 * 잠들 수 있다. xa_lock 구간 안에서는 잠들지 않도록 할당을 락 밖으로 뺀 구조다.
 *
 * 에러 경로: 3단계 실패는 -ENOMEM 즉시 반환(락을 잡고 있지 않으므로 안전).
 * 4단계에서 xa_is_err 이면 새 set 을 kfree 한 뒤 락을 풀고 xa_err 반환.
 * 어느 실패든 device->dev_set 은 건드리지 않은 채로 남는다.
 *
 * 호출 체인:
 *   vendor probe(vfio_pci_core_register_device 등) → [vfio_assign_device_set]
 *     → xa_load / __xa_cmpxchg / kzalloc_obj / list_add_tail
 */
int vfio_assign_device_set(struct vfio_device *device, void *set_id)
{
	unsigned long idx = (unsigned long)set_id;
	/* [한국어] 새로 만들 후보. 경합에서 지면 버린다. */
	struct vfio_device_set *new_dev_set;
	/* [한국어] 찾았거나 최종적으로 쓰게 될 묶음. */
	struct vfio_device_set *dev_set;

	/* [한국어] NULL 은 xarray 인덱스로 쓸 수 없고 묶음을 식별할 수도 없다.
	 * vendor 의 프로그래밍 오류이므로 WARN 을 동반한다. */
	if (WARN_ON(!set_id))
		return -EINVAL;

	/*
	 * Atomically acquire a singleton object in the xarray for this set_id
	 */
	xa_lock(&vfio_device_set_xa);
	/* [한국어] set_id 를 인덱스로 기존 묶음을 찾는다. */
	dev_set = xa_load(&vfio_device_set_xa, idx);
	/* [한국어] 이미 있으면 새로 만들 필요가 없다. 가장 흔한 경로다. */
	if (dev_set)
		goto found_get_ref;
	/* [한국어] 새 묶음을 만들기 전에 락을 놓는다. xa_lock 은 spinlock 이라 그 안에서
	 * 잠들 수 없는데, 아래 할당은 GFP_KERNEL 이라 잠들 수 있다. */
	xa_unlock(&vfio_device_set_xa);

	/* [한국어] 새 묶음을 0 초기화로 만든다. 락 밖에서 할당하는 이유는 xa_lock 이
	 * spinlock 이라 그 안에서 잠들 수 없기 때문이다. */
	new_dev_set = kzalloc_obj(*new_dev_set);
	/* [한국어] 할당 실패. 락을 잡고 있지 않은 지점이라 그냥 반환해도 안전하다. */
	if (!new_dev_set)
		return -ENOMEM;
	/* [한국어] 이 묶음을 공유하는 디바이스들이 open_count 조작을 직렬화할 때 쓸 mutex.
	 * 같은 리셋 도메인 전체가 한 락을 나눠 갖는다는 것이 device set 의 핵심이다. */
	mutex_init(&new_dev_set->lock);
	INIT_LIST_HEAD(&new_dev_set->device_list);
	/* [한국어] 묶음 식별자를 보관한다. 나중에 xarray 에서 지울 때 이 값을 인덱스로 쓴다. */
	new_dev_set->set_id = set_id;

	xa_lock(&vfio_device_set_xa);
	/* [한국어] "여전히 비어 있으면 내 것을 넣는다" 를 원자적으로 시도한다. 락을 놓았던
	 * 사이에 다른 CPU 가 먼저 넣었다면 그 값이 반환되고, 아래에서 내 것을 버린다.
	 * **이 한 줄이 singleton 보장의 핵심**이다. */
	dev_set = __xa_cmpxchg(&vfio_device_set_xa, idx, NULL, new_dev_set,
			       GFP_KERNEL);
	/* [한국어] 이전 값이 NULL 이었다는 것은 교환이 성공했다는 뜻이다. */
	if (!dev_set) {
		/* [한국어] 빈 자리에 내 것을 넣는 데 성공했다. 이제 그것이 이 묶음의 정본이다. */
		dev_set = new_dev_set;
		/* [한국어] 경합에서 이겼다(또는 애초에 경합이 없었다). 참조를 늘리는 공통 지점으로 간다. */
		goto found_get_ref;
	}

	kfree(new_dev_set);
	/* [한국어] cmpxchg 자체가 실패했다면(메모리 부족 등) 오류 포인터가 담겨 온다. */
	if (xa_is_err(dev_set)) {
		xa_unlock(&vfio_device_set_xa);
		/* [한국어] xarray 내부 오류를 errno 로 바꿔 돌려준다. */
		return xa_err(dev_set);
	}

found_get_ref:
	/* [한국어] 이 묶음을 쓰는 디바이스 수를 하나 늘린다. 이 값이 0 이 될 때 묶음이
	 * 해체되므로, 이 증가가 곧 묶음의 수명 참조다. */
	dev_set->device_count++;
	/* [한국어] xarray 조작이 끝났으므로 락을 놓는다. 아래 리스트 조작은 묶음 자신의
	 * mutex 가 지키며, 두 락을 겹쳐 잡지 않아 잠금 순서 문제가 생기지 않는다. */
	xa_unlock(&vfio_device_set_xa);
	mutex_lock(&dev_set->lock);
	/* [한국어] 디바이스에서 묶음으로 가는 포인터를 건다. */
	device->dev_set = dev_set;
	/* [한국어] 묶음의 디바이스 목록 꼬리에 붙인다. 이 두 줄로 device 와 dev_set 의
	 * 양방향 연결이 완성된다. */
	list_add_tail(&device->dev_set_list, &dev_set->device_list);
	/* [한국어] 리스트 조작 완료. 이제 이 디바이스는 묶음의 정식 구성원이다. */
	mutex_unlock(&dev_set->lock);
	return 0;
}
/* [한국어] EXPORT_SYMBOL_GPL 로 내보낸다. vendor 드라이버가 probe 에서 부른다. */
EXPORT_SYMBOL_GPL(vfio_assign_device_set);

/* [한국어]
 * vfio_release_device_set - device set 에서 빠지고, 마지막이면 set 자체를 없앤다
 *
 * @device: 해제 중인 vfio_device. device->dev_set 이 NULL 일 수도 있다.
 * @return: 없음. 실패할 수 있는 일이 없다.
 *
 * 왜 필요한가: vfio_assign_device_set 의 정확한 짝이다. 이것을 빠뜨리면
 * (a) dev_set->device_list 에 죽은 포인터가 남아 다음 순회가 use-after-free 를
 * 일으키고, (b) device_count 가 0 으로 내려오지 못해 device_set 객체와 그
 * mutex 가 영원히 누수된다.
 *
 * 동작 과정:
 *  1. dev_set 이 NULL 이면 아무 일도 없다. vfio_init_device 가 실패한 뒤
 *     호출될 수 있는데, 그때는 아직 set 이 배정되지 않았을 수 있다.
 *  2. set 의 mutex 아래에서 리스트에서 뺀다. 이 mutex 는 같은 묶음의 다른
 *     디바이스가 동시에 붙거나 빠지는 것을 막는다.
 *  3. xa_lock 아래에서 device_count 를 줄이고, 0 이 되면 xarray 항목을 지우고
 *     mutex_destroy 후 kfree. **감소와 0 판정과 erase 가 한 락 안**에 있어야
 *     다른 CPU 의 vfio_assign_device_set 이 죽어 가는 set 을 집어 가지 않는다.
 *
 * 실행 컨텍스트: vfio_device_release, 즉 struct device 의 kref 가 0 이 된
 * 시점의 프로세스 문맥. vfio_init_device 의 실패 경로에서도 불린다.
 *
 * 에러 경로: 없음. 다만 2단계와 3단계 사이에 락이 끊기는데, 이 시점에는
 * 이미 리스트에서 빠져 있어 다른 CPU 가 이 device 를 볼 수 없다.
 *
 * 호출 체인:
 *   vfio_device_release → [vfio_release_device_set] → mutex_lock / __xa_erase / kfree
 *   vfio_init_device(실패 경로) → [vfio_release_device_set]
 */
static void vfio_release_device_set(struct vfio_device *device)
{
	struct vfio_device_set *dev_set = device->dev_set;

	/* [한국어] 아직 묶음을 배정받지 못한 디바이스도 있다. vfio_init_device 가 실패한
	 * 뒤 불릴 수 있어 NULL 을 견뎌야 한다. */
	if (!dev_set)
		return;

	/* [한국어] 묶음의 mutex 를 잡는다. 같은 묶음의 다른 디바이스가 동시에 붙거나 빠지는
	 * 것을 막는다. */
	mutex_lock(&dev_set->lock);
	/* [한국어] 묶음의 디바이스 목록에서 뺀다. 이 시점 이후 다른 CPU 는 이 디바이스를
	 * 순회로 볼 수 없다. */
	list_del(&device->dev_set_list);
	/* [한국어] 리스트 조작 완료. */
	mutex_unlock(&dev_set->lock);

	xa_lock(&vfio_device_set_xa);
	/* [한국어] 감소와 0 판정이 한 표현식이다. 0 이 되면 이 묶음의 마지막 디바이스가
	 * 빠졌다는 뜻이다. **감소와 판정과 아래 erase 가 한 락 안**에 있어야
	 * 다른 CPU 의 vfio_assign_device_set 이 죽어 가는 묶음을 집어 가지 않는다. */
	if (!--dev_set->device_count) {
		__xa_erase(&vfio_device_set_xa,
			   (unsigned long)dev_set->set_id);
		/* [한국어] 묶음이 해체되므로 mutex 도 파괴한다. lockdep 이 활성화된 커널에서
		 * 파괴하지 않고 free 하면 경고가 난다. */
		mutex_destroy(&dev_set->lock);
		/* [한국어] 묶음 객체 해제. vfio_assign_device_set 의 kzalloc 짝이다. */
		kfree(dev_set);
	}
	/* [한국어] xarray 조작 완료. 감소와 0 판정과 erase 가 모두 이 락 안에서 끝났으므로,
	 * 다른 CPU 가 죽어 가는 묶음을 집어 갈 창이 없다. */
	xa_unlock(&vfio_device_set_xa);
}

/* [한국어]
 * vfio_device_set_open_count - 묶음 전체에서 열려 있는 fd 수의 합을 센다
 *
 * @dev_set: 조회 대상 device set. **호출자가 dev_set->lock 을 이미 잡고
 *           있어야 한다** — 함수 첫 줄의 lockdep_assert_held 가 그것을 강제한다.
 * @return: 묶음 안 모든 디바이스의 open_count 합. 0 이면 아무도 안 열었다는 뜻.
 *
 * 왜 필요한가: vfio-pci 는 bus/slot reset 을 하기 전에 "리셋 범위 안의 다른
 * 함수를 남이 쓰고 있지 않은가" 를 확인해야 한다.
 * drivers/vfio/pci/vfio_pci_core.c:2524 가 이 값이 1 보다 크면 hot reset 을
 * 막는 식으로 쓴다. 자기 자신의 open 도 1 로 세므로 "1 = 나 혼자" 다.
 *
 * 동작 과정: 리스트를 처음부터 끝까지 돌며 각 device 의 open_count 를 더한다.
 * 그게 전부다. 캐시된 값을 두지 않는 이유는 open_count 갱신이 잦지 않고,
 * 묶음 크기가 PCI 함수 몇 개 수준으로 작기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock(mutex) 을 잡은 채 불리므로
 * 잠들 수 있는 자리지만 이 함수 자체는 잠들지 않는다.
 *
 * 에러 경로: 없다. 락을 안 잡고 부르면 lockdep 이 잡아낸다(CONFIG_LOCKDEP 시).
 *
 * 호출 체인:
 *   vfio_pci_core 의 hot reset 검사 → [vfio_device_set_open_count]
 *     → list_for_each_entry
 */
unsigned int vfio_device_set_open_count(struct vfio_device_set *dev_set)
{
	struct vfio_device *cur;
	/* [한국어] 합계 누산기. 캐시된 값을 두지 않는 이유는 open_count 갱신이 잦지 않고
	 * 묶음이 작기 때문이다. */
	unsigned int open_count = 0;

	/* [한국어] 호출자가 dev_set->lock 을 잡고 왔는지 확인한다. 이 함수는 락을 잡지 않고
	 * **요구만 한다** — 호출자가 결과를 쓰는 동안에도 값이 안정적이어야 하므로
	 * 락 범위를 호출자가 정하는 것이 옳다. */
	lockdep_assert_held(&dev_set->lock);

	/* [한국어] 묶음 안 모든 디바이스를 훑는다. */
	list_for_each_entry(cur, &dev_set->device_list, dev_set_list)
		/* [한국어] 각 디바이스의 열린 fd 수를 더한다. dev_set->lock 아래라 도중에 값이
		 * 바뀌지 않는다. */
		open_count += cur->open_count;
	/* [한국어] 합계를 돌려준다. 0 이면 아무도 안 열었다는 뜻이고, 1 이면 호출자 자신뿐이다. */
	return open_count;
}
EXPORT_SYMBOL_GPL(vfio_device_set_open_count);

/* [한국어]
 * vfio_find_device_in_devset - 묶음 안에서 특정 struct device 를 감싼 vfio_device 를 찾는다
 *
 * @dev_set: 검색 대상 묶음. dev_set->lock 을 호출자가 잡고 있어야 한다.
 * @dev: 찾고자 하는 하부 버스 디바이스(PCI 면 pci_dev 안의 struct device).
 * @return: 찾으면 vfio_device 포인터, 없으면 NULL. **참조를 잡아 주지 않는다** —
 *          호출자가 dev_set->lock 을 놓기 전에만 쓸 수 있다.
 *
 * 왜 필요한가: hot reset 을 할 때 vfio-pci 는 "리셋 범위의 이 PCI 함수도 VFIO
 * 가 관리 중인가" 를 알아야 한다. 관리 중이 아니면(커널 드라이버가 붙어 있으면)
 * 리셋해서는 안 된다. drivers/vfio/pci/vfio_pci_core.c:2341 이 이 함수의 결과가
 * NULL 인지로 -ENODEV 를 판정하고, 835줄은 찾은 vdev 로 리셋을 이어 간다.
 *
 * 동작 과정: 리스트를 선형 순회하며 cur->dev 와 인자 dev 의 **포인터 동일성**을
 * 비교한다. 이름이나 BDF 비교가 아니라 포인터 비교라서 오탐이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev_set->lock 보유 상태.
 *
 * 에러 경로: 못 찾으면 NULL. 호출자가 그것을 -ENODEV 등으로 번역한다.
 *
 * 호출 체인:
 *   vfio_pci_core 의 hot reset 준비 → [vfio_find_device_in_devset]
 *     → list_for_each_entry
 */
struct vfio_device *
vfio_find_device_in_devset(struct vfio_device_set *dev_set,
			   struct device *dev)
{
	struct vfio_device *cur;

	/* [한국어] 역시 호출자가 dev_set->lock 을 잡고 있어야 한다. 반환하는 포인터가
	 * 그 락 아래에서만 유효하기 때문이다. */
	lockdep_assert_held(&dev_set->lock);

	/* [한국어] 묶음 안 디바이스를 선형 순회한다. 묶음 크기가 PCI 함수 몇 개 수준이라
	 * 해시나 트리가 필요 없다. */
	list_for_each_entry(cur, &dev_set->device_list, dev_set_list)
		/* [한국어] 하부 struct device 포인터를 **주소로** 비교한다. 이름이나 BDF 비교가
		 * 아니라서 오탐이 없다. */
		if (cur->dev == dev)
			/* [한국어] 포인터가 일치하는 첫 항목을 돌려준다. 참조를 잡지 않으므로 호출자가
			 * dev_set->lock 을 놓기 전에만 쓸 수 있다. */
			return cur;
	/* [한국어] 묶음 안에 그 하부 디바이스를 감싼 vfio_device 가 없다는 뜻이다.
	 * 호출자는 이것을 -ENODEV 등으로 번역한다. */
	return NULL;
}
/* [한국어] EXPORT_SYMBOL_GPL — vfio-pci 의 hot reset 준비 코드가 쓴다. */
EXPORT_SYMBOL_GPL(vfio_find_device_in_devset);

/* [한국어]
 * vfio_device_put_registration - "등록 참조" 를 하나 놓고, 마지막이면 해제 대기자를 깨운다
 *
 * @device: 참조를 놓을 디바이스.
 * @return: 없음.
 *
 * 이 참조가 무엇의 짝인가: device->refcount 는 __vfio_register_dev 가 1 로
 * 시작시키고(등록 그 자체의 몫), 그 뒤로는 **fd 하나가 1** 이다.
 *  - device_cdev.c:31 의 try_get 은 device_cdev.c:52 의 put 또는
 *    vfio_device_fops_release 의 put 과 짝이다.
 *  - group.c:50 의 try_get 은 group.c:321 또는 같은 fops_release 의 put 과 짝.
 *  - vfio_unregister_group_dev 의 put 은 등록 자체의 1 을 되돌린다.
 * 이걸 빠뜨리면 refcount 가 0 에 닿지 않아 vfio_unregister_group_dev 가
 * 영원히 잠들고, rmmod 나 디바이스 제거가 통째로 멈춘다. 반대로 한 번 더
 * 부르면 아직 fd 를 쓰고 있는 사용자 밑에서 unregister 가 진행돼 use-after-free
 * 가 된다.
 *
 * 동작 과정: refcount_dec_and_test 로 감소시키고, 0 이 되면 complete 로
 * device->comp 완료 객체를 신호한다. complete 는 대기자가 없어도 안전하다
 * (완료 카운트만 올라간다).
 *
 * 실행 컨텍스트: 프로세스 문맥. fd close 경로에서도, 오류 되돌리기 경로에서도
 * 불린다. 원자 연산과 completion 신호뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. refcount_t 는 0 에서 더 내리려 하면 커널이 경고한다.
 *
 * 호출 체인:
 *   vfio_device_fops_release / device_cdev.c:52 / group.c:321 /
 *   vfio_unregister_group_dev → [vfio_device_put_registration]
 *     → refcount_dec_and_test → complete
 */
/*
 * Device objects - create, release, get, put, search
 */
/* Device reference always implies a group reference */
void vfio_device_put_registration(struct vfio_device *device)
{
	/* [한국어] 원자적으로 1 을 빼고 0 이 됐는지 확인한다. refcount_t 는 0 에서 더 내리려
	 * 하거나 0 에서 올리려 하면 커널이 경고하는 안전한 카운터다. */
	if (refcount_dec_and_test(&device->refcount))
		/* [한국어] 마지막 사용자가 빠졌으므로 vfio_unregister_group_dev 에서 기다리는 쪽을
		 * 깨운다. 대기자가 없어도 안전하다 — 완료 카운트만 올라가고,
		 * 나중에 오는 try_wait_for_completion 이 즉시 성공한다. */
		complete(&device->comp);
}
/* [한국어] EXPORT_SYMBOL_GPL — vendor 와 다른 VFIO 파일들이 등록 참조를 놓을 때 쓴다. */
EXPORT_SYMBOL_GPL(vfio_device_put_registration);

/* [한국어]
 * vfio_device_try_get_registration - 아직 살아 있을 때만 "등록 참조" 를 얻는다
 *
 * @device: 참조를 얻으려는 디바이스.
 * @return: true 면 참조를 얻었다(반드시 vfio_device_put_registration 으로 되돌려야
 *          한다). false 면 이미 refcount 가 0 이라 **등록 해제가 진행 중** 이라는
 *          뜻이며, 호출자는 -ENODEV 로 실패해야 한다.
 *
 * 왜 try 인가: refcount_inc_not_zero 는 0→1 을 거부한다. 0 은 "마지막 사용자가
 * 빠져 unregister 가 대기를 마친 상태" 를 뜻하므로, 그 뒤에 참조를 새로 만들면
 * 이미 해제 절차에 들어간 객체를 되살리는 셈이 된다. 이 한 줄이 open 과
 * unregister 사이의 경합을 막는 **전부**다.
 *
 * 동작 과정: refcount_inc_not_zero 한 번. 원자적으로 "0 이 아니면 +1" 을 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. open 진입점(device_cdev.c:31, group.c:50)과
 * dmabuf 경로(drivers/vfio/pci/vfio_pci_dmabuf.c:285)에서 불린다.
 *
 * 에러 경로: false 반환. 호출자는 그 자리에서 -ENODEV 를 돌려준다.
 *
 * 호출 체인:
 *   vfio_device_fops_cdev_open / vfio_device_get_from_name
 *     → [vfio_device_try_get_registration] → refcount_inc_not_zero
 */
bool vfio_device_try_get_registration(struct vfio_device *device)
{
	/* [한국어] 0 이 아닐 때만 1 을 더한다. 0 은 "이미 등록 해제가 진행 중" 을 뜻하므로
	 * 되살리면 안 된다. **이 한 줄이 open 과 unregister 사이 경합을 막는 전부다.** */
	return refcount_inc_not_zero(&device->refcount);
}
/* [한국어] EXPORT_SYMBOL_GPL — open 진입점과 dmabuf 경로가 쓴다. */
EXPORT_SYMBOL_GPL(vfio_device_try_get_registration);

/* [한국어]
 * vfio_device_release - struct device 의 kref 가 0 이 됐을 때 불리는 최종 소멸자
 *
 * @dev: 소멸 중인 내장 struct device. container_of 로 바깥 vfio_device 를 얻는다.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_device 는 struct device 를 **내장**하므로 그 kref 수명에
 * 묶인다. 이 콜백을 device->device.release 에 걸어 두면(vfio_init_device 가
 * 설정) 마지막 put_device 시점에 커널 드라이버 코어가 불러 준다. vendor 는
 * vfio_put_device() 만 부르면 되고 언제 실제 free 가 일어나는지 몰라도 된다.
 *
 * 동작 과정(자원 반납 순서가 곧 획득의 역순):
 *  1. vfio_release_device_set — device set 에서 빠지고 마지막이면 set 도 정리.
 *  2. ida_free — vfio_init_device 가 ida_alloc_max 로 얻은 index 반납.
 *     이 index 가 minor number 이자 "vfio<N>" 이름의 N 이다.
 *  3. ops->release 가 있으면 호출 — vendor 가 자기 private 필드를 정리.
 *     **ops->init 의 짝**이며, init 이 있는데 release 가 없으면 vendor 쪽 누수다.
 *  4. iput — pseudo-fs anon inode 참조 반납.
 *  5. simple_release_fs — pseudo-fs mount 참조 반납. 마지막이면 mount 해제.
 *  6. kvfree — _vfio_alloc_device 가 kvzalloc 으로 잡은 구조체 본체.
 *
 * 실행 컨텍스트: 프로세스 문맥. put_device 를 호출한 쪽의 문맥에서 동기적으로
 * 실행된다. 이 시점에는 이미 어떤 fd 도 열려 있지 않고 refcount 도 0 이다.
 *
 * 에러 경로: 없다. 소멸자는 실패할 수 없다.
 *
 * 호출 체인:
 *   vendor 의 vfio_put_device() → put_device → 드라이버 코어 →
 *   [vfio_device_release] → vfio_release_device_set / ida_free / ops->release /
 *   iput / simple_release_fs / kvfree
 */
/*
 * VFIO driver API
 */
/* Release helper called by vfio_put_device() */
static void vfio_device_release(struct device *dev)
{
	struct vfio_device *device =
			/* [한국어] 내장 struct device 주소에서 바깥 vfio_device 주소를 되찾는다.
			 * 드라이버 코어는 내장 device 만 알고 있으므로 이 되짚기가 필수다. */
			container_of(dev, struct vfio_device, device);

	/* [한국어] 묶음에서 빠지고, 마지막이면 묶음 객체까지 정리한다. */
	vfio_release_device_set(device);
	/* [한국어] index 를 반납한다. 이제 다른 디바이스가 같은 번호를 쓸 수 있다. */
	ida_free(&vfio.device_ida, device->index);

	/* [한국어] vendor 가 해제 콜백을 제공하면 */
	if (device->ops->release)
		/* [한국어] 자기 private 필드를 정리하게 한다. vfio_init_device 의 ops->init 짝이며,
		 * init 은 있는데 release 가 없으면 vendor 쪽 누수가 된다. */
		device->ops->release(device);

	/* [한국어] 익명 inode 참조 반납. vfio_fs_inode_new 의 alloc_anon_inode 짝이다.
	 * 이 참조가 0 이 되면 그 디바이스에 걸린 모든 매핑 정보도 함께 사라진다. */
	iput(device->inode);
	/* [한국어] pseudo-fs 마운트 참조 반납. 마지막이면 마운트 자체가 사라진다. */
	simple_release_fs(&vfio.vfs_mount, &vfio.fs_count);
	/* [한국어] 구조체 본체 해제. _vfio_alloc_device 의 kvzalloc 짝이며, 이 줄이 실행된
	 * 뒤에는 device 포인터를 절대 만지면 안 된다. */
	kvfree(device);
}

/* [한국어] 이 함수는 정의가 아래쪽에 있는데 _vfio_alloc_device 가 먼저 부르므로
 * 전방 선언이 필요하다. 정의를 위로 올리지 않은 이유는 파일 안의 논리
 * 순서(할당 → 초기화 → 등록)를 유지하기 위함으로 보인다. */
static int vfio_init_device(struct vfio_device *device, struct device *dev,
			    const struct vfio_device_ops *ops);

/* [한국어]
 * _vfio_alloc_device - vendor 구조체까지 포함한 크기로 vfio_device 를 할당하고 초기화한다
 *
 * @size: 할당할 **전체** 바이트 수. vendor 는 자기 구조체 첫 필드에
 *        struct vfio_device 를 두므로 sizeof(vendor 구조체) 를 준다.
 * @dev: 하부 버스 디바이스(PCI 면 &pdev->dev). device->dev 로 저장되고
 *       내장 struct device 의 parent 가 된다.
 * @ops: vendor 콜백 vtable. 등록 이후 불변이며 device->ops 로 저장된다.
 * @return: 성공 시 vfio_device 포인터, 실패 시 ERR_PTR. -EINVAL 은 size 가
 *          struct vfio_device 보다 작을 때(WARN 동반), 그 밖에 -ENOMEM 이나
 *          vfio_init_device 의 오류가 그대로 올라온다.
 *
 * 왜 필요한가: vendor 는 이 함수를 직접 부르지 않고 include/linux/vfio.h 의
 * 타입 검사 래퍼를 통해 부른다. 여기서 kvzalloc 을 쓰는 이유는 vendor 구조체가
 * 수십 KB 로 커질 수 있어 물리적으로 연속한 메모리를 요구하지 않기 위함이다.
 * 반대편 해제는 vfio_device_release 의 kvfree 다.
 *
 * 동작 과정:
 *  1. size 하한 검사 — 작으면 뒤이은 초기화가 구조체 밖을 밟는다. WARN_ON 으로
 *     개발 단계에서 드러나게 한다.
 *  2. kvzalloc — 0 초기화가 중요하다. dev_set, group, mig_ops 등 "없으면 NULL"
 *     로 판정되는 필드가 많다.
 *  3. vfio_init_device — index 할당, completion 초기화, inode 생성,
 *     ops->init 호출, 내장 device 초기화까지.
 *  4. 실패하면 kvfree 후 ERR_PTR.
 *
 * 실행 컨텍스트: vendor probe, 프로세스 문맥. GFP_KERNEL 이라 잠들 수 있다.
 *
 * 에러 경로: 3단계 실패 시 out_free 로 가서 kvfree 만 한다. 이 시점에는
 * vfio_init_device 가 자기가 잡은 것을 이미 다 되돌린 뒤다 — 그래서 여기서
 * ida_free 나 iput 을 또 하지 않는다.
 *
 * 호출 체인:
 *   vendor probe → include/linux/vfio.h 의 할당 래퍼 → [_vfio_alloc_device]
 *     → kvzalloc → vfio_init_device
 */
/*
 * Allocate and initialize vfio_device so it can be registered to vfio
 * core.
 *
 * Drivers should use the wrapper vfio_alloc_device() for allocation.
 * @size is the size of the structure to be allocated, including any
 * private data used by the driver.
 *
 * Driver may provide an @init callback to cover device private data.
 *
 * Use vfio_put_device() to release the structure after success return.
 */
struct vfio_device *_vfio_alloc_device(size_t size, struct device *dev,
				       const struct vfio_device_ops *ops)
{
	struct vfio_device *device;
	/* [한국어] vfio_init_device 의 결과 코드. */
	int ret;

	/* [한국어] 요청 크기가 base 구조체보다 작으면 뒤이은 초기화가 구조체 밖을 밟는다.
	 * WARN 으로 개발 단계에서 드러나게 한다. */
	if (WARN_ON(size < sizeof(struct vfio_device)))
		return ERR_PTR(-EINVAL);

	/* [한국어] 0 초기화 할당. kvzalloc 을 쓰는 이유는 vendor 구조체가 수십 KB 로 커질 수
	 * 있어 물리적으로 연속한 메모리를 요구하지 않기 위함이다. 짝은
	 * vfio_device_release 의 kvfree 다. */
	device = kvzalloc(size, GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!device)
		return ERR_PTR(-ENOMEM);

	/* [한국어] index 할당, completion 초기화, inode 생성, vendor init, 내장 device
	 * 초기화까지 한 번에 맡긴다. */
	ret = vfio_init_device(device, dev, ops);
	/* [한국어] 초기화 실패면 */
	if (ret)
		goto out_free;
	/* [한국어] 완성된 디바이스를 vendor 에게 돌려준다. 이 뒤로 vendor 는 자기 필드를
	 * 채우고 등록 함수를 부른다. */
	return device;

out_free:
	/* [한국어] vfio_init_device 가 실패했다는 것은 그것이 자기가 잡은 자원을 이미 모두
	 * 되돌렸다는 뜻이다. 그래서 여기서는 구조체 메모리만 해제한다 —
	 * ida_free 나 iput 을 또 하면 이중 해제가 된다. */
	kvfree(device);
	/* [한국어] 오류를 포인터에 실어 돌려준다. 호출자는 IS_ERR 로 판별한다. */
	return ERR_PTR(ret);
}
/* [한국어] EXPORT_SYMBOL_GPL — vendor 는 include/linux/vfio.h 의 타입 검사 래퍼를
 * 통해 간접적으로 부른다. */
EXPORT_SYMBOL_GPL(_vfio_alloc_device);

/* [한국어]
 * vfio_fs_init_fs_context - VFIO 전용 pseudo 파일시스템의 컨텍스트를 초기화한다
 *
 * @fc: VFS 가 넘겨주는 파일시스템 컨텍스트.
 * @return: 0 성공, -ENOMEM 실패.
 *
 * 왜 필요한가: VFIO 는 디바이스마다 익명 inode 를 하나 만들어 그 디바이스의
 * 모든 mmap 이 **같은 address_space** 를 공유하게 한다. 그래야 나중에 한 번의
 * unmap_mapping_range 로 그 디바이스의 모든 사용자 매핑을 한꺼번에 날릴 수
 * 있다(디바이스 리셋이나 제거 시 필수). 그런 익명 inode 를 만들려면 마운트
 * 지점 없는 pseudo-fs 가 하나 필요하고, 이 함수가 그 fs 의 생성 훅이다.
 *
 * 동작 과정: init_pseudo 에 VFIO_MAGIC 매직 넘버를 넘겨 super block 을 만든다.
 * 성공하면 0, NULL 이면 -ENOMEM 으로 번역한다. 세 줄짜리지만 이것이
 * file_system_type 의 필수 훅이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 첫 simple_pin_fs 호출 시 VFS 가 부른다.
 *
 * 에러 경로: init_pseudo 가 NULL 을 주면 -ENOMEM. 그러면 simple_pin_fs 가
 * 실패하고 vfio_fs_inode_new 가 ERR_PTR 을 올려 보낸다.
 *
 * 호출 체인:
 *   vfio_fs_inode_new → simple_pin_fs → VFS → [vfio_fs_init_fs_context]
 *     → init_pseudo
 */
static int vfio_fs_init_fs_context(struct fs_context *fc)
{
	return init_pseudo(fc, VFIO_MAGIC) ? 0 : -ENOMEM;
}

/* [한국어] VFIO 전용 pseudo 파일시스템 타입. 디바이스마다 익명 inode 를 하나씩
 * 만들기 위해서만 존재한다.
 * 설정자: 정적 초기화 + VFS 가 등록 상태를 내부에 유지.
 * 읽는 자: simple_pin_fs 와 simple_release_fs.
 * 동기화: VFS 가 관리한다. */
static struct file_system_type vfio_fs_type = {
	/* [한국어] 파일시스템 이름. /proc/filesystems 에 나타나지만 마운트 지점이 없어
	 * 사용자가 직접 마운트하지는 않는다.
	 * 설정자: 정적 초기화. 읽는 자: VFS. 값 범위: 고정 문자열. 동기화: 불변. */
	.name = "vfio",
	.owner = THIS_MODULE,
	.init_fs_context = vfio_fs_init_fs_context,
	.kill_sb = kill_anon_super,
};

/* [한국어]
 * vfio_fs_inode_new - 디바이스 전용 익명 inode 를 하나 만든다
 *
 * @: 인자 없음. 전역 vfio.vfs_mount 와 vfio.fs_count 를 쓴다.
 * @return: 새 inode 포인터, 실패 시 ERR_PTR(-ENOMEM 등).
 *
 * 왜 필요한가: 이 inode 의 i_mapping 이 device fd 의 f_mapping 이 된다
 * (device_cdev.c 의 open 이 그렇게 건다). 결과적으로 같은 디바이스를 여러 번
 * mmap 해도 페이지 캐시 매핑 트리가 하나로 모이고, 디바이스를 리셋하거나
 * 제거할 때 그 하나만 비우면 사용자 매핑 전체가 무효화된다.
 *
 * 동작 과정:
 *  1. simple_pin_fs — pseudo-fs 를 마운트한다. 이미 마운트돼 있으면 fs_count 만
 *     증가. **이 참조를 반납하는 곳이 두 군데**다: 실패 시 여기서 곧바로,
 *     성공 시 vfio_device_release 나 vfio_init_device 의 실패 경로에서.
 *  2. alloc_anon_inode — 그 super block 위에 익명 inode 를 만든다.
 *  3. inode 할당이 실패하면 1번에서 얻은 fs 참조를 즉시 반납한다. 이 되돌림이
 *     없으면 fs_count 가 영영 0 으로 내려오지 않아 모듈 unload 후에도 pseudo-fs
 *     가 남는다.
 *
 * 실행 컨텍스트: vfio_init_device 안, 즉 vendor probe 의 프로세스 문맥.
 *
 * 에러 경로: 두 단계 모두 ERR_PTR 로 전달된다. 호출자 vfio_init_device 는
 * PTR_ERR 를 꺼내 out_inode 로 간다.
 *
 * 호출 체인:
 *   vfio_init_device → [vfio_fs_inode_new] → simple_pin_fs → alloc_anon_inode
 */
static struct inode *vfio_fs_inode_new(void)
{
	struct inode *inode;
	/* [한국어] simple_pin_fs 의 결과 코드. */
	int ret;

	/* [한국어] pseudo-fs 를 마운트하고 참조를 하나 잡는다. 이미 마운트돼 있으면
	 * fs_count 만 증가한다. 이 참조의 짝은 실패 시 아래 한 줄, 성공 시
	 * vfio_device_release 또는 vfio_init_device 의 실패 경로다. */
	ret = simple_pin_fs(&vfio_fs_type, &vfio.vfs_mount, &vfio.fs_count);
	/* [한국어] 마운트 실패면 그대로 ERR_PTR 로 올린다. */
	if (ret)
		return ERR_PTR(ret);

	/* [한국어] 그 마운트의 super block 위에 익명 inode 를 만든다. 이름도 디렉터리 항목도
	 * 없는 inode 라 파일시스템 이름공간에 나타나지 않는다. */
	inode = alloc_anon_inode(vfio.vfs_mount->mnt_sb);
	/* [한국어] alloc_anon_inode 는 실패를 ERR_PTR 로 알린다. */
	if (IS_ERR(inode))
		/* [한국어] inode 생성이 실패하면 방금 잡은 fs 참조를 즉시 되돌린다. 이 되돌림이
		 * 없으면 fs_count 가 영영 0 으로 내려오지 않아 모듈 언로드 후에도
		 * pseudo-fs 가 남는다. */
		simple_release_fs(&vfio.vfs_mount, &vfio.fs_count);

	/* [한국어] 성공이면 새 inode 를, 실패면 ERR_PTR 을 돌려준다. 실패 시 위에서 fs 참조를
	 * 이미 되돌렸으므로 호출자가 추가로 할 일은 없다. */
	return inode;
}

/* [한국어]
 * vfio_init_device - 갓 할당된 vfio_device 를 "등록 가능한 상태" 로 만든다
 *
 * @device: kvzalloc 으로 0 초기화된 메모리. 아직 어떤 리스트에도 없다.
 * @dev: 하부 버스 디바이스. device->dev 와 내장 device 의 parent 가 된다.
 * @ops: vendor vtable.
 * @return: 0 성공. 실패 시 음수 errno 이며, **자기가 잡은 자원은 모두 되돌린
 *         뒤** 반환한다(호출자 _vfio_alloc_device 는 kvfree 만 하면 된다).
 *
 * 왜 필요한가: 등록(__vfio_register_dev) 전에 반드시 준비돼야 하는 네 가지가
 * 있다. (1) 유일한 index — 이것이 cdev minor 이자 vfio<N> 의 N 이다.
 * (2) completion 객체 — unregister 가 마지막 사용자를 기다릴 때 쓴다.
 * (3) 익명 inode — 모든 mmap 이 공유할 address_space.
 * (4) 내장 struct device 의 초기화와 class/parent/release 배선.
 *
 * 동작 과정:
 *  1. ida_alloc_max(&vfio.device_ida, MINORMASK, ...) — 0..MINORMASK 범위의
 *     유일한 정수. MINORMASK 로 상한을 두는 이유는 이 값이 그대로 캐릭터
 *     디바이스의 minor 번호가 되기 때문이다(device_cdev.c 가 그렇게 쓴다).
 *  2. init_completion — 아직 아무도 기다리지 않는 완료 객체.
 *  3. dev/ops 저장.
 *  4. vfio_fs_inode_new — 익명 inode. 실패하면 out_inode.
 *  5. ops->init 이 있으면 호출 — vendor private 초기화. 실패하면 out_uninit.
 *  6. device_initialize — kref 를 1 로 만든다. **이 시점부터 메모리 해제는
 *     put_device 를 통해서만** 일어나며, 그래서 이 함수는 여기까지 오면
 *     더 이상 실패하지 않는다.
 *  7. release/class/parent 를 건다. class 가 vfio.device_class 라서
 *     /sys/class/vfio-dev/vfio<N> 로 보이게 된다.
 *
 * 실행 컨텍스트: vendor probe, 프로세스 문맥. 잠들 수 있다.
 *
 * 에러 경로(되돌림 순서가 정확히 역순인지가 관건):
 *  - out_uninit: ops->init 실패. inode 참조(iput)와 fs 참조(simple_release_fs)를
 *    되돌리고 out_inode 로 떨어진다.
 *  - out_inode: inode 생성 실패 지점. device set 에서 빼고 index 를 반납한다.
 *    여기서 vfio_release_device_set 을 부르는 이유는, vendor 가 이 함수보다
 *    **먼저** vfio_assign_device_set 을 부를 수 있기 때문이다.
 *
 * 호출 체인:
 *   _vfio_alloc_device → [vfio_init_device]
 *     → ida_alloc_max / init_completion / vfio_fs_inode_new / ops->init /
 *   device_initialize
 */
/*
 * Initialize a vfio_device so it can be registered to vfio core.
 */
static int vfio_init_device(struct vfio_device *device, struct device *dev,
			    const struct vfio_device_ops *ops)
{
	int ret;

	/* [한국어] 0 부터 MINORMASK 까지의 유일한 정수를 얻는다. 상한을 MINORMASK 로 두는
	 * 이유는 이 값이 그대로 캐릭터 디바이스 minor 번호가 되기 때문이다
	 * (device_cdev.c:304 가 그만큼의 minor 를 예약한다). */
	ret = ida_alloc_max(&vfio.device_ida, MINORMASK, GFP_KERNEL);
	/* [한국어] 음수면 실패다. 성공 시 0 이상의 index 가 반환된다. */
	if (ret < 0) {
		/* [한국어] 할당 실패를 디버그 로그로 남긴다. dev_dbg 라 기본적으로는 출력되지 않는다. */
		dev_dbg(dev, "Error to alloc index\n");
		return ret;
	}

	/* [한국어] 할당받은 정수를 index 로 저장한다. 이 값이 cdev minor 이자 vfio<N> 의 N 이다. */
	device->index = ret;
	init_completion(&device->comp);
	/* [한국어] 하부 버스 디바이스 포인터. */
	device->dev = dev;
	/* [한국어] vendor vtable. 등록 이후 불변이다. */
	device->ops = ops;
	/* [한국어] 디바이스 전용 익명 inode 를 만든다. 이 inode 의 address_space 를
	 * 모든 mmap 이 공유해, 나중에 한 번에 무효화할 수 있게 된다. */
	device->inode = vfio_fs_inode_new();
	/* [한국어] inode 생성 실패는 ERR_PTR 로 온다. */
	if (IS_ERR(device->inode)) {
		/* [한국어] 오류 코드를 포인터에서 꺼낸다. */
		ret = PTR_ERR(device->inode);
		/* [한국어] inode 를 만들지 못했으므로 묶음 배정과 index 만 되돌리는 라벨로 간다. */
		goto out_inode;
	}

	/* [한국어] vendor 가 초기화 콜백을 제공하면 */
	if (ops->init) {
		/* [한국어] vendor 가 자기 private 필드를 초기화한다. 이 콜백의 짝이
		 * vfio_device_release 의 ops->release 다. */
		ret = ops->init(device);
		/* [한국어] vendor 초기화 실패면 out_uninit 으로. */
		if (ret)
			goto out_uninit;
	}

	device_initialize(&device->device);
	/* [한국어] kref 가 0 이 됐을 때 불릴 소멸자를 건다. 이 배선 덕분에 vendor 는
	 * vfio_put_device() 만 부르면 되고 실제 해제 시점을 몰라도 된다. */
	device->device.release = vfio_device_release;
	/* [한국어] 이 모듈의 class 에 소속시킨다. /sys/class/vfio-dev 아래로 보이게 된다. */
	device->device.class = vfio.device_class;
	/* [한국어] 부모를 하부 버스 디바이스로 둔다. sysfs 트리에서 이 노드가 실제 하드웨어
	 * 아래에 놓여, 사용자가 어느 하드웨어인지 경로만으로 알 수 있다. */
	device->device.parent = device->dev;
	return 0;

out_uninit:
	/* [한국어] vendor 초기화 실패 되돌림 — inode 참조 반납. */
	iput(device->inode);
	/* [한국어] pseudo-fs 마운트 참조 반납. inode 참조와 짝을 이룬다. */
	simple_release_fs(&vfio.vfs_mount, &vfio.fs_count);
out_inode:
	/* [한국어] 묶음에서 뺀다. vendor 가 이 함수보다 **먼저** vfio_assign_device_set 을
	 * 부를 수 있어, inode 생성 실패 지점에서도 이 되돌림이 필요하다. */
	vfio_release_device_set(device);
	/* [한국어] index 반납. 이 시점에는 아직 device_initialize 를 하지 않아
	 * vfio_device_release 가 불릴 일이 없으므로 여기서 직접 되돌려야 한다. */
	ida_free(&vfio.device_ida, device->index);
	return ret;
}

/* [한국어]
 * __vfio_register_dev - 등록의 실체. 이름을 짓고 group 에 넣고 노드를 만들고 refcount 를 켠다
 *
 * @device: vfio_init_device 를 마친 디바이스.
 * @type: 이 디바이스의 IOMMU 백킹 종류. VFIO_IOMMU(정상 패스스루),
 *        VFIO_EMULATED_IOMMU(mdev 처럼 vendor 가 DMA 를 중개),
 *        VFIO_NO_IOMMU(위험한 noiommu 모드). enum 정의는 drivers/vfio/vfio.h.
 * @return: 0 성공, 음수 errno 실패. 실패 시 호출자(vendor probe)는 등록을
 *          포기하고 vfio_put_device() 로 객체를 놓는다.
 *
 * 왜 필요한가: vfio_register_group_dev 와 vfio_register_emulated_iommu_dev 의
 * 공통 몸통이다. 두 진입점의 차이는 오직 @type 하나다.
 *
 * 동작 과정:
 *  1. iommufd 콜백 4종(bind/unbind/attach_ioas/detach_ioas)이 다 있는지 검사.
 *     CONFIG_IOMMUFD 가 켜진 커널에서 하나라도 빠지면 나중에 NULL 호출로
 *     죽으므로 등록 자체를 거부한다. iommufd.c 가 제공하는 기본 세트를 그대로
 *     쓰면 이 검사는 자동으로 통과한다.
 *  2. dev_set 이 없으면 자기 자신을 set_id 로 삼아 1인 묶음을 만든다.
 *     vendor 가 리셋 범위를 신경 쓰지 않는 경우의 기본값이다.
 *  3. dev_set_name 으로 "vfio<index>" 라는 이름을 붙인다. 이 이름이
 *     /sys/class/vfio-dev/ 아래 노드 이름이자, cdev 경로의 /dev/vfio/devices/
 *     아래 이름이 된다(device_cdev.c:295 의 devnode 콜백).
 *  4. vfio_device_set_group — IOMMU group 을 찾거나 만들어 붙인다.
 *  5. cache coherency 검사 — 아래 별도 문단.
 *  6. vfio_device_add — cdev 와 sysfs 노드를 실제로 만든다.
 *  7. refcount 를 1 로 **켠다**. 상류 주석이 말하듯 "드라이버가 register 를
 *     부르기 전에는 refcount 가 시작되지 않는다". 그전까지 try_get 은 전부
 *     실패하므로 반쯤 만들어진 디바이스가 열릴 수 없다.
 *  8. group 리스트 등록과 debugfs 노드 생성.
 *
 * cache coherency 검사의 의미: VFIO 는 IOMMU 도메인에 항상 IOMMU_CACHE 를
 * 건다. 사용자 공간에는 캐시 일관성을 되돌릴 수단을 주지 않기 때문이다.
 * 그래서 실제 IOMMU 를 쓰는 VFIO_IOMMU 타입일 때만, 그 IOMMU 가
 * IOMMU_CAP_CACHE_COHERENCY 를 지원하는지 미리 확인하고 아니면 -EINVAL 로
 * 등록을 거부한다. noiommu 와 emulated 는 애초에 IOMMU 도메인이 없으므로
 * 검사 대상이 아니다. device_iommu_capable 의 구현은 IOMMU 코어에 있으며
 * 이 트리에는 drivers/iommu 가 없어 확인 못 함.
 *
 * 실행 컨텍스트: vendor probe, 프로세스 문맥.
 *
 * 에러 경로: 1~3단계 실패는 아직 group 을 잡기 전이라 그냥 반환한다.
 * 5~6단계 실패는 err_out 으로 가서 vfio_device_remove_group 으로 4단계를
 * 되돌린다. **7단계 이후로는 실패 지점이 없다** — 그래야 refcount 를 켠 뒤에
 * 되돌릴 일이 생기지 않는다.
 *
 * 호출 체인:
 *   vfio_register_group_dev / vfio_register_emulated_iommu_dev
 *     → [__vfio_register_dev] → vfio_assign_device_set / dev_set_name /
 *   vfio_device_set_group / device_iommu_capable / vfio_device_add /
 *   refcount_set / vfio_device_group_register / vfio_device_debugfs_init
 */
static int __vfio_register_dev(struct vfio_device *device,
			       enum vfio_group_type type)
{
	int ret;

	/* [한국어] iommufd 콜백 4종이 다 있는지 확인한다. CONFIG_IOMMUFD 가 켜진 커널에서
	 * 하나라도 빠지면 나중에 NULL 을 호출해 죽으므로 등록 자체를 거부한다.
	 * iommufd.c 가 제공하는 기본 세트를 그대로 쓰면 자동으로 통과한다. */
	if (WARN_ON(IS_ENABLED(CONFIG_IOMMUFD) &&
		    (!device->ops->bind_iommufd ||
		     !device->ops->unbind_iommufd ||
		     !device->ops->attach_ioas ||
		     !device->ops->detach_ioas)))
		return -EINVAL;

	/*
	 * If the driver doesn't specify a set then the device is added to a
	 * singleton set just for itself.
	 */
	if (!device->dev_set)
		/* [한국어] vendor 가 리셋 범위를 지정하지 않았으면 자기 자신을 set_id 로 삼아
		 * 1인 묶음을 만든다. 반환값을 검사하지 않는데, 여기서는 set_id 가 NULL 일
		 * 수 없고 실패해도 아래 등록이 dev_set 없이 진행되지는 않는다 —
		 * vfio_release_device_set 이 NULL 을 견디도록 되어 있다. */
		vfio_assign_device_set(device, device);

	/* [한국어] "vfio<index>" 이름을 붙인다. 이 이름이 /sys/class/vfio-dev 아래 노드
	 * 이름이자 /dev/vfio/devices/ 아래 이름이 된다(device_cdev.c:295). */
	ret = dev_set_name(&device->device, "vfio%d", device->index);
	/* [한국어] 이름 짓기 실패도 group 을 잡기 전이라 그냥 반환한다. */
	if (ret)
		return ret;

	/* [한국어] IOMMU group 을 찾거나 만들어 붙인다. 같은 group 에 이미 device 가 있으면
	 * 거기에 합류하고, 없으면 새 group 을 만든다(group.c). */
	ret = vfio_device_set_group(device, type);
	/* [한국어] 실패하면 아직 group 을 잡기 전이므로 그냥 반환한다. */
	if (ret)
		return ret;

	/*
	 * VFIO always sets IOMMU_CACHE because we offer no way for userspace to
	 * restore cache coherency. It has to be checked here because it is only
	 * valid for cases where we are using iommu groups.
	 */
	if (type == VFIO_IOMMU && !vfio_device_is_noiommu(device) &&
	    /* [한국어] 타입이 실제 IOMMU 를 쓰는 경우이고 noiommu 모드도 아니면서 */
	    !device_iommu_capable(device->dev, IOMMU_CAP_CACHE_COHERENCY)) {
		/* [한국어] 그 IOMMU 가 캐시 일관성을 강제할 수 없다면 등록을 거부한다.
		 * 바로 위 상류 주석이 이유를 밝힌다 — VFIO 는 항상 IOMMU_CACHE 를 걸고
		 * 사용자에게 일관성을 되돌릴 수단을 주지 않으므로, 그 전제가 성립하지
		 * 않는 IOMMU 는 애초에 쓸 수 없다. 구현은 IOMMU 코어에 있으며
		 * 이 트리에는 drivers/iommu 가 없어 확인 못 함. */
		ret = -EINVAL;
		/* [한국어] -EINVAL 로 실패시키고 */
		goto err_out;
	/* [한국어] group 배정을 되돌리는 라벨로 간다. */
	}

	/* [한국어] cdev 와 sysfs 노드를 실제로 만든다. 이 순간부터 /dev 노드가 보이지만,
	 * 아직 refcount 가 0 이라 try_get 이 실패해 open 은 되지 않는다. */
	ret = vfio_device_add(device);
	/* [한국어] 실패하면 err_out 으로 가서 group 배정을 되돌린다. */
	if (ret)
		goto err_out;

	/* Refcounting can't start until the driver calls register */
	/* [한국어] **등록 참조를 1 로 켜는 지점.** 바로 위 상류 주석이 말하듯 드라이버가
	 * register 를 부르기 전에는 refcount 가 시작되지 않는다. 그전까지
	 * vfio_device_try_get_registration 이 전부 실패하므로 반쯤 만들어진
	 * 디바이스는 열릴 수 없다. 이 1 을 되돌리는 곳은
	 * vfio_unregister_group_dev 의 put 하나뿐이다. */
	refcount_set(&device->refcount, 1);

	/* [한국어] group 의 디바이스 목록에 넣는다. 이 순간부터 GROUP_GET_DEVICE_FD 가
	 * 이 디바이스를 찾을 수 있다. */
	vfio_device_group_register(device);
	/* [한국어] 진단용 debugfs 노드를 만든다. 실패해도 반환값이 없어 등록을 막지 않는다. */
	vfio_device_debugfs_init(device);

	return 0;
err_out:
	/* [한국어] cache coherency 검사나 vfio_device_add 가 실패한 경우의 되돌림 —
	 * vfio_device_set_group 짝이다. refcount 를 켜기 **전**에만 이 라벨로 오므로,
	 * 켜진 뒤에 되돌릴 일은 생기지 않는다. */
	vfio_device_remove_group(device);
	return ret;
}

/* [한국어]
 * vfio_register_group_dev - 실제 IOMMU 뒤에 있는 물리 디바이스를 VFIO 에 등록한다
 *
 * @device: 등록할 디바이스.
 * @return: __vfio_register_dev 의 반환값 그대로. 0 이면 이 순간부터 사용자
 *          공간이 이 디바이스를 열 수 있다.
 *
 * 왜 별도 함수인가: type 인자를 감춘 얇은 래퍼다. vendor 는 자기 디바이스가
 * IOMMU 를 실제로 쓰는지(vfio-pci, vfio-platform, vfio-fsl-mc, vfio-cdx)만
 * 알면 되고 enum 값을 직접 다룰 필요가 없다. 이 트리에서 이것을 부르는 곳은
 * drivers/vfio/pci/vfio_pci_core.c:2208, drivers/vfio/cdx/main.c:298,
 * drivers/vfio/fsl-mc/vfio_fsl_mc.c:537, drivers/vfio/platform/vfio_platform.c:67,
 * drivers/vfio/platform/vfio_amba.c:80 이다.
 *
 * VFIO_IOMMU 타입이 뜻하는 것: 이 디바이스는 진짜 DMA 를 하고, 그 DMA 는
 * IOMMU 가 번역한다. 따라서 __vfio_register_dev 의 cache coherency 검사를
 * 받는 유일한 타입이다. NVMe 컨트롤러를 vfio-pci 로 넘길 때 지나는 길이
 * 정확히 이 함수다.
 *
 * 실행 컨텍스트: vendor probe, 프로세스 문맥.
 *
 * 에러 경로: 그대로 전달. vendor 는 실패 시 자기가 잡은 자원을 풀고
 * probe 를 실패시킨다.
 *
 * 호출 체인:
 *   vfio_pci_core_register_device 등 vendor probe → [vfio_register_group_dev]
 *     → __vfio_register_dev(VFIO_IOMMU)
 */
int vfio_register_group_dev(struct vfio_device *device)
{
	return __vfio_register_dev(device, VFIO_IOMMU);
}
/* [한국어] EXPORT_SYMBOL_GPL — vfio-pci, vfio-platform, vfio-fsl-mc, vfio-cdx 가 쓴다. */
EXPORT_SYMBOL_GPL(vfio_register_group_dev);

/* [한국어]
 * vfio_register_emulated_iommu_dev - IOMMU 백킹이 없는 가상 디바이스를 등록한다
 *
 * @device: 등록할 디바이스.
 * @return: __vfio_register_dev 의 반환값 그대로.
 *
 * 왜 별도 함수인가: mdev 나 vfio-ap 처럼 **하드웨어가 직접 DMA 를 치지 않는**
 * 디바이스를 위한 입구다. 상류 주석이 못 박듯 "이 디바이스의 사용자는 중개되지
 * 않은 DMA 를 직접 유발할 수 없어야 한다" — vendor 가 모든 메모리 접근을
 * 자기 코드로 중개하고, 그때 이 파일의 vfio_pin_pages 와 vfio_dma_rw 를 쓴다.
 *
 * IOMMU group 은 어떻게 되는가: 실제 IOMMU group 이 없으므로 VFIO 코어가
 * 가짜 group 을 만들어 준다(group.c). sysfs 의 iommu_group 노드가 사용자 공간
 * ABI 의 일부라서 없앨 수 없기 때문이다. 이 배경은 drivers/vfio/vfio.h 의
 * enum vfio_group_type 주석에 이미 정리돼 있다.
 *
 * 실행 컨텍스트: mdev vendor 의 probe, 프로세스 문맥.
 *
 * 에러 경로: 그대로 전달.
 *
 * 호출 체인:
 *   mdev 계열 vendor probe → [vfio_register_emulated_iommu_dev]
 *     → __vfio_register_dev(VFIO_EMULATED_IOMMU)
 */
/*
 * Register a virtual device without IOMMU backing.  The user of this
 * device must not be able to directly trigger unmediated DMA.
 */
int vfio_register_emulated_iommu_dev(struct vfio_device *device)
{
	return __vfio_register_dev(device, VFIO_EMULATED_IOMMU);
}
/* [한국어] EXPORT_SYMBOL_GPL — mdev 계열 vendor 가 쓴다. */
EXPORT_SYMBOL_GPL(vfio_register_emulated_iommu_dev);

/* [한국어]
 * vfio_unregister_group_dev - 등록을 되돌리고, 사용자가 fd 를 놓을 때까지 기다린다
 *
 * @device: 해제할 디바이스.
 * @return: 없음. **실패할 수 없다** — 하드웨어가 사라지는 중이므로 물러설 곳이
 *          없다. 그래서 오래 걸릴지언정 반드시 끝까지 간다.
 *
 * 왜 이 함수가 이 파일에서 가장 특이한가: 이 파일에서 **무한히 잠들 수 있는
 * 유일한 자리**다. 사용자 공간이 fd 를 쥐고 있으면 커널은 그것을 강제로 뺏을
 * 수 없다(강제로 뺏으면 사용자가 mmap 한 MMIO 아래에서 하드웨어가 사라져
 * 치명적이 된다). 그래서 "요청하고, 기다리고, 다시 요청" 을 반복한다.
 *
 * 동작 과정:
 *  1. vfio_device_group_unregister — group 경로의 GROUP_GET_DEVICE_FD 가
 *     이 디바이스를 더는 찾지 못하게 한다(새 open 차단 1).
 *  2. vfio_device_del — cdev/sysfs 노드를 없앤다. __vfio_register_dev 의
 *     vfio_device_add 와 짝이며, cdev 경로의 새 open 도 막는다(새 open 차단 2).
 *  3. vfio_device_put_registration — __vfio_register_dev 가 켠 refcount 1 을
 *     되돌린다. 열린 fd 가 없다면 여기서 곧장 0 이 되고 completion 이 신호된다.
 *  4. try_wait_for_completion 으로 먼저 비차단 확인. 이미 끝났으면 루프를
 *     건너뛴다 — 흔한 경우(아무도 안 열었을 때)의 빠른 길이다.
 *  5. 루프: ops->request 로 vendor 에게 "사용자에게 놓아 달라고 알려라" 를
 *     요청한다. vfio-pci 는 이 요청을 받아 사용자에게 eventfd 신호를 보낸다.
 *     i 를 증가시키며 넘기므로 vendor 는 몇 번째 요청인지 알 수 있고, 보통
 *     첫 번째만 실제로 알리고 나머지는 조용히 넘긴다.
 *  6. 10초 타임아웃으로 기다린다. 처음에는 interruptible 로 기다려
 *     관리자가 Ctrl-C 로 빠져나갈 수 있게 하되, **한 번 시그널을 받으면**
 *     interrupted 를 세우고 이후로는 uninterruptible 로 바꾼다. 시그널이 계속
 *     오는 상황에서 바쁜 루프가 되는 것을 막기 위함이다. 동시에 어느 태스크가
 *     막혀 있는지 dev_warn 으로 한 번만 알린다.
 *  7. 빠져나오면 debugfs 정리와 vfio_device_remove_group 으로 등록 시의
 *     vfio_device_set_group 을 되돌린다.
 *
 * 빠뜨렸을 때: 3번을 빼면 refcount 가 0 이 되지 않아 영원히 잠긴다. 반대로
 * 4~6 의 대기를 빼면 fd 가 살아 있는 채로 vendor 가 하드웨어를 떼어내
 * use-after-free 가 된다.
 *
 * 실행 컨텍스트: vendor 의 remove, 프로세스 문맥. **잠든다.** hot-unplug 나
 * rmmod 경로에서 불리며, 여기서 오래 막히면 그 경로 전체가 멈춘다.
 *
 * 호출 체인:
 *   vendor remove(vfio_pci_core_unregister_device 등)
 *     → [vfio_unregister_group_dev] → vfio_device_group_unregister /
 *   vfio_device_del / vfio_device_put_registration / ops->request /
 *   wait_for_completion_timeout / vfio_device_remove_group
 */
/* [한국어] [상류 코드 관찰] 바로 아래 상류 영어 주석은 문장이 "Open file descriptors
 * for the device..." 에서 끊긴 채 같은 줄에서 블록이 닫힌다. 이 파일의 다른
 * 블록 주석은 모두 마지막 줄에 닫는 표기만 두는데 이 하나만 형태가 다르다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. 끊긴 문장이 말하려던
 * 내용은 이 함수가 실제로 하는 일 — 열린 fd 가 모두 닫힐 때까지 기다린다 —
 * 으로 보이며, 바로 위 한국어 블록에 그 동작을 정리해 두었다. */
/*
 * Decrement the device reference count and wait for the device to be
 * removed.  Open file descriptors for the device... */
void vfio_unregister_group_dev(struct vfio_device *device)
{
	unsigned int i = 0;
	/* [한국어] 시그널을 한 번이라도 받았는지 기억하는 플래그. 대기 방식을 바꾸는 데 쓴다. */
	bool interrupted = false;
	/* [한국어] 완료 대기 함수들의 반환값. 남은 jiffies 이거나 음수 오류다. */
	long rc;

	/*
	 * Prevent new device opened by userspace via the
	 * VFIO_GROUP_GET_DEVICE_FD in the group path.
	 */
	vfio_device_group_unregister(device);

	/*
	 * Balances vfio_device_add() in register path, also prevents
	 * new device opened by userspace in the cdev path.
	 */
	vfio_device_del(device);

	vfio_device_put_registration(device);
	/* [한국어] 먼저 비차단으로 확인한다. 아무도 안 열었다면 여기서 이미 완료 상태라
	 * 아래 루프를 통째로 건너뛴다 — 흔한 경우의 빠른 길이다. */
	rc = try_wait_for_completion(&device->comp);
	/* [한국어] 완료되지 않은 동안 반복한다. 열린 fd 가 있으면 여기서 무한히 머문다 —
	 * 커널은 사용자에게서 fd 를 강제로 뺏을 수 없다. 강제로 뺏으면 사용자가
	 * mmap 한 MMIO 아래에서 하드웨어가 사라져 치명적이 된다. */
	while (rc <= 0) {
		/* [한국어] vendor 가 요청 콜백을 제공하면 */
		if (device->ops->request)
			/* [한국어] vendor 에게 "사용자에게 놓아 달라고 알려라" 를 요청한다. i 를 증가시켜
			 * 넘기므로 vendor 는 몇 번째 요청인지 알 수 있고, 보통 첫 번째만 실제로
			 * 알리고 나머지는 조용히 넘긴다. vfio-pci 는 사용자에게 eventfd 신호를 보낸다. */
			device->ops->request(device, i++);

		/* [한국어] 이전에 시그널을 받은 적이 있으면 */
		if (interrupted) {
			/* [한국어] 한 번 시그널을 받은 뒤에는 시그널을 무시하고 10초씩 기다린다. */
			rc = wait_for_completion_timeout(&device->comp,
							 HZ * 10);
		} else {
			/* [한국어] 처음에는 시그널에 반응하는 대기를 쓴다. 관리자가 Ctrl-C 로 빠져나올 수
			 * 있게 하기 위함이다. */
			rc = wait_for_completion_interruptible_timeout(
				&device->comp, HZ * 10);
			/* [한국어] 시그널로 중단됐다면(음수 반환) */
			if (rc < 0) {
				/* [한국어] 이후로는 uninterruptible 로 바꾼다. 시그널이 계속 오는 상황에서
				 * interruptible 대기를 유지하면 바쁜 루프가 되기 때문이다. */
				interrupted = true;
				/* [한국어] 어느 태스크가 막혀 있는지 이름과 PID 로 한 번만 알린다.
				 * interrupted 를 세운 직후에만 실행되므로 반복되지 않으며,
				 * 관리자가 그 프로세스를 찾아 종료시킬 수 있게 하는 진단이다.
				 * 세 조각의 문자열 리터럴이 이어 붙어 한 형식 문자열이 된다. */
				dev_warn(device->dev,
					 "Device is currently in use, task"
					 " \"%s\" (%d) "
					 "blocked until device is released",
					 current->comm, task_pid_nr(current));
			}
		}
	}

	vfio_device_debugfs_exit(device);
	/* Balances vfio_device_set_group in register path */
	vfio_device_remove_group(device);
}
/* [한국어] EXPORT_SYMBOL_GPL — vendor 의 remove 경로가 쓴다. */
EXPORT_SYMBOL_GPL(vfio_unregister_group_dev);

#if IS_ENABLED(CONFIG_KVM)
/* [한국어]
 * vfio_device_get_kvm_safe - KVM 인스턴스 참조를 심볼 조회로 안전하게 얻는다
 *
 * @device: 참조를 걸어 둘 디바이스. 성공하면 device->kvm 과 device->put_kvm 이
 *          채워진다.
 * @kvm: 연결할 KVM 인스턴스. NULL 이면(비-KVM 사용자, 예컨대 SPDK 나 DPDK)
 *       아무 일도 하지 않는다.
 * @return: 없음. 실패하면 그냥 device->kvm 이 NULL 로 남고, 그것이 곧
 *          "이 디바이스는 KVM 과 무관하다" 는 정상 상태다.
 *
 * 왜 symbol_get 인가: VFIO 는 KVM 모듈에 링크 의존성을 갖고 싶지 않다. KVM 이
 * 로드되지 않은 시스템에서도 VFIO 는 완전히 동작해야 하기 때문이다. 그래서
 * 컴파일 시 심볼을 참조하는 대신 **런타임에 이름으로 찾아** 함수 포인터를
 * 얻는다. symbol_get 은 찾아 준 심볼이 속한 모듈의 참조도 함께 잡아 주므로,
 * 그 포인터를 들고 있는 동안 KVM 모듈은 rmmod 되지 않는다.
 *
 * 동작 과정과 참조 균형(이 함수의 어려운 점이 전부 여기 있다):
 *  1. dev_set->lock 보유를 단언. device->kvm 은 이 락으로 보호된다.
 *  2. kvm 이 NULL 이면 즉시 반환.
 *  3. 해제 함수 심볼을 **먼저** 잡는다. 나중에 놓을 방법을 확보하지 못한 채
 *     참조부터 잡으면 영영 놓을 수 없기 때문이다.
 *  4. 획득 함수 심볼을 잡는다. 실패하면 3번을 되돌리고 반환.
 *  5. 획득 함수를 호출해 KVM 인스턴스 참조를 잡는다. 성공/실패와 무관하게
 *     획득 함수 심볼은 곧바로 놓는다 — 더 쓸 일이 없다.
 *  6. 획득이 실패했으면 3번도 되돌리고 반환.
 *  7. 성공했으면 해제 함수 포인터를 device->put_kvm 에 **보관한다**.
 *     3번에서 잡은 심볼 참조는 여기서 놓지 않고, vfio_device_put_kvm 이
 *     놓을 때까지 유지된다. 이것이 이 함수의 비대칭이며 의도된 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev_set->lock 보유. 첫 open 직전에 불린다
 * (group.c:166, device_cdev.c:59).
 *
 * 에러 경로: 위 4번과 6번. 어느 쪽이든 device->kvm 은 손대지 않아
 * "KVM 없음" 상태가 유지된다.
 *
 * 호출 체인:
 *   vfio_device_group_get_kvm_safe(group.c) / vfio_df_get_kvm_safe(device_cdev.c)
 *     → [vfio_device_get_kvm_safe] → symbol_get / symbol_put
 */
void vfio_device_get_kvm_safe(struct vfio_device *device, struct kvm *kvm)
{
	void (*pfn)(struct kvm *kvm);
	/* [한국어] 획득 함수의 시그니처. 반환형이 bool 인 safe 판이다. */
	bool (*fn)(struct kvm *kvm);
	/* [한국어] 그 호출 결과. */
	bool ret;

	/* [한국어] 호출자가 dev_set->lock 을 잡고 왔는지 확인한다. device->kvm 과
	 * device->put_kvm 이 그 락으로 보호된다. */
	lockdep_assert_held(&device->dev_set->lock);

	/* [한국어] 비-KVM 사용자(SPDK, DPDK 등)는 kvm 이 NULL 이며 정상이다. */
	if (!kvm)
		return;

	/* [한국어] **해제 함수 심볼을 먼저** 찾는다. 나중에 놓을 방법을 확보하지 못한 채
	 * 참조부터 잡으면 영영 놓을 수 없기 때문이다. symbol_get 은 그 심볼이 속한
	 * 모듈 참조도 함께 잡아 주므로, 포인터를 들고 있는 동안 KVM 모듈은
	 * rmmod 되지 않는다. */
	pfn = symbol_get(kvm_put_kvm);
	/* [한국어] 찾지 못했다는 것은 KVM 이 로드돼 있지 않다는 뜻이다. 이 함수는 KVM 이
	 * 있다고 가정하고 불렸으므로 경고한다. */
	if (WARN_ON(!pfn))
		return;

	/* [한국어] 획득 함수 심볼을 찾는다. */
	fn = symbol_get(kvm_get_kvm_safe);
	/* [한국어] 없으면 */
	if (WARN_ON(!fn)) {
		/* [한국어] 먼저 잡은 해제 함수 심볼을 되돌리고 물러난다. */
		symbol_put(kvm_put_kvm);
		return;
	}

	/* [한국어] 찾은 함수로 KVM 인스턴스 참조를 잡는다. safe 판이라 소멸 중인
	 * 가상머신에는 실패를 돌려준다. */
	ret = fn(kvm);
	/* [한국어] 획득 함수 심볼은 더 쓸 일이 없으므로 성공 여부와 무관하게 곧바로 놓는다. */
	symbol_put(kvm_get_kvm_safe);
	/* [한국어] 획득 실패(가상머신이 이미 소멸 중)면 */
	if (!ret) {
		/* [한국어] 해제 함수 심볼 참조도 놓고 물러난다. device->kvm 은 건드리지 않아
		 * "KVM 없음" 상태가 유지된다. */
		symbol_put(kvm_put_kvm);
		return;
	}

	/* [한국어] 해제 함수 포인터를 보관한다. 3단계에서 잡은 심볼 참조는 여기서 놓지 않고
	 * vfio_device_put_kvm 이 놓을 때까지 유지된다 — 이 비대칭이 의도된 설계다. */
	device->put_kvm = pfn;
	/* [한국어] KVM 인스턴스 포인터를 보관한다. 위 줄과 이 줄이 짝을 이뤄야
	 * vfio_device_put_kvm 이 정상 경로를 탄다. */
	device->kvm = kvm;
}

/* [한국어]
 * vfio_device_put_kvm - vfio_device_get_kvm_safe 가 잡은 KVM 참조와 심볼 참조를 놓는다
 *
 * @device: 참조를 놓을 디바이스.
 * @return: 없음.
 *
 * 무엇의 짝인가: 정확히 vfio_device_get_kvm_safe 의 7번 단계를 되돌린다.
 * (a) KVM 인스턴스 참조를 보관해 둔 해제 함수로 놓고,
 * (b) 그 해제 함수 심볼 자체의 참조도 놓는다.
 * 이걸 빠뜨리면 KVM 가상머신 객체가 해제되지 않고 KVM 모듈도 rmmod 되지 않는다.
 *
 * 동작 과정:
 *  1. dev_set->lock 보유 단언.
 *  2. device->kvm 이 NULL 이면 잡은 적이 없으므로 즉시 반환.
 *  3. put_kvm 포인터가 NULL 이면 — kvm 은 있는데 해제 함수가 없는 모순 상태다.
 *     WARN 을 찍고 clear 로 건너뛴다. 여기서 심볼을 놓지 않는 것은 맞다:
 *     put_kvm 이 NULL 이라는 것은 애초에 심볼을 잡은 적이 없다는 뜻이다.
 *  4. 정상 경로에서는 보관해 둔 함수로 KVM 참조를 놓고, 포인터를 NULL 로
 *     비우고, 심볼 참조를 놓는다.
 *  5. clear: device->kvm 을 NULL 로. 3번 경로도 여기로 합류해 최소한
 *     포인터는 남기지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev_set->lock 보유. 마지막 close 경로
 * (group.c:231/249, device_cdev.c:164/187)와 bind 실패 되돌림에서 불린다.
 *
 * 호출 체인:
 *   vfio_df_group_close / vfio_df_unbind_iommufd → [vfio_device_put_kvm]
 *     → device->put_kvm / symbol_put
 */
void vfio_device_put_kvm(struct vfio_device *device)
{
	/* [한국어] put 쪽도 같은 락을 요구한다. get 과 put 이 같은 락 아래에서만 일어나야
	 * 포인터와 함수 포인터의 짝이 깨지지 않는다. */
	lockdep_assert_held(&device->dev_set->lock);

	/* [한국어] 잡은 적이 없으면 놓을 것도 없다. */
	if (!device->kvm)
		return;

	/* [한국어] kvm 은 있는데 해제 함수가 없는 모순 상태다. 경고하고 아래로 건너뛴다.
	 * 여기서 심볼을 놓지 않는 것이 맞다 — put_kvm 이 NULL 이라는 것은 애초에
	 * 심볼을 잡은 적이 없다는 뜻이다. */
	if (WARN_ON(!device->put_kvm))
		goto clear;

	device->put_kvm(device->kvm);
	/* [한국어] 포인터를 비워 두 번 놓는 일을 막는다. */
	device->put_kvm = NULL;
	/* [한국어] 해제 함수 심볼 참조를 놓는다. vfio_device_get_kvm_safe 가 일부러 붙들고
	 * 있던 참조이며, 이제 KVM 모듈이 rmmod 될 수 있다. */
	symbol_put(kvm_put_kvm);

clear:
	/* [한국어] 포인터를 비운다. 위 WARN 경로도 여기로 합류해, 모순 상태에서도 최소한
	 * 죽은 포인터는 남기지 않는다. */
	device->kvm = NULL;
}
#endif

/* [한국어]
 * vfio_assert_device_open - "이 디바이스가 지금 열려 있는가" 를 단언하고 알려 준다
 *
 * @device: 검사 대상.
 * @return: true 면 open_count 가 0 이 아니다(정상). false 면 0 이며, 그 자리에서
 *          WARN_ON_ONCE 가 한 번 찍힌다.
 *
 * 왜 필요한가: vfio_pin_pages, vfio_unpin_pages, vfio_dma_rw 는 모두 "디바이스가
 * 열려 있는 동안에만" 유효하다. 열려 있지 않다면 IOMMU 결합도 없고
 * group->container 도 바뀔 수 있어, 그 상태에서 게스트 메모리를 만지는 것은
 * vendor 드라이버의 버그다. 이 헬퍼는 그 버그를 조용히 넘기지 않고
 * WARN 으로 드러낸다.
 *
 * READ_ONCE 를 쓰는 이유: open_count 는 dev_set->lock 으로 보호되지만 이
 * 헬퍼는 락 없이 읽는다. 컴파일러가 읽기를 쪼개거나 없애지 못하게 막아
 * "본 값이 실제 메모리의 한 시점 값" 임을 보장한다. 정확한 값이 아니라
 * 0 이냐 아니냐만 보므로 이 정도면 충분하다.
 *
 * WARN_ON_ONCE 인 이유: 이 검사는 pin/unpin 마다 불릴 수 있어, WARN 이 매번
 * 찍히면 로그가 폭주한다. 처음 한 번만 스택을 남긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락 없이 호출된다.
 *
 * 호출 체인:
 *   vfio_pin_pages / vfio_unpin_pages / vfio_dma_rw
 *     → [vfio_assert_device_open] → READ_ONCE / WARN_ON_ONCE
 */
/* true if the vfio_device has open_device() called but not close_device() */
static bool vfio_assert_device_open(struct vfio_device *device)
{
	/* [한국어] open_count 가 0 이 아니면 true. READ_ONCE 는 락 없이 읽으면서도 컴파일러가
	 * 읽기를 쪼개거나 없애지 못하게 막는다 — 정확한 값이 아니라 0 이냐 아니냐만
	 * 보므로 이 정도면 충분하다. WARN_ON_ONCE 로 감싼 이유는 pin/unpin 마다
	 * 불릴 수 있어 매번 경고하면 로그가 폭주하기 때문이다.
	 * 반환값의 부정 연산은 "경고가 나지 않았으면 정상" 을 true 로 바꾸는 것이다. */
	return !WARN_ON_ONCE(!READ_ONCE(device->open_count));
}

/* [한국어]
 * vfio_allocate_device_file - fd 하나에 붙을 vfio_device_file 컨텍스트를 만든다
 *
 * @device: 이 fd 가 가리킬 디바이스.
 * @return: 새 df 포인터, 실패 시 ERR_PTR(-ENOMEM).
 *
 * 왜 device 와 별개의 객체인가: 한 디바이스에 여러 fd 가 열릴 수 있고
 * (1세대 group 경로), fd 마다 권한 상태(access_granted), 바인딩된 iommufd
 * 컨텍스트, KVM 포인터가 다르다. 그것들을 device 본체에 두면 fd 끼리 섞인다.
 * 구조체 정의와 필드별 설명은 이미 drivers/vfio/vfio.h:107 에 있다.
 *
 * GFP_KERNEL_ACCOUNT 인 이유: 이 할당은 **사용자 공간이 fd 를 열 때마다**
 * 일어난다. 즉 사용자가 양을 조절할 수 있는 메모리이므로 memory cgroup 에
 * 과금해야 한다. 그래야 컨테이너 하나가 fd 를 잔뜩 열어 호스트 메모리를
 * 고갈시키는 것을 cgroup 한계가 막을 수 있다.
 *
 * 동작 과정:
 *  1. kzalloc — 0 초기화가 중요하다. group, iommufd, kvm, access_granted 가
 *     모두 "0 = 아직 없음/권한 없음" 으로 해석된다.
 *  2. device 백포인터 저장. 이 포인터는 close 까지 바뀌지 않는다.
 *     참조 카운트는 여기서 잡지 않는다 — 호출자가 이미
 *     vfio_device_try_get_registration 으로 잡아 둔 상태다.
 *  3. kvm_ref_lock 초기화.
 *
 * 실행 컨텍스트: open 경로, 프로세스 문맥. 잠들 수 있다.
 *
 * 에러 경로: ERR_PTR 로 전달. 호출자는 앞서 잡은 등록 참조를 되돌린다
 * (device_cdev.c:50 의 err_put_registration).
 *
 * 호출 체인:
 *   vfio_device_fops_cdev_open(device_cdev.c:34) / vfio_device_open_file(group.c:261)
 *     → [vfio_allocate_device_file] → kzalloc_obj / spin_lock_init
 */
struct vfio_device_file *
vfio_allocate_device_file(struct vfio_device *device)
{
	struct vfio_device_file *df;

	/* [한국어] fd 컨텍스트를 0 초기화로 할당한다. GFP_KERNEL_ACCOUNT 는 이 할당을
	 * memory cgroup 에 과금하라는 뜻이다 — **사용자가 fd 를 열 때마다 일어나는
	 * 할당**이라 컨테이너 하나가 호스트 메모리를 고갈시키지 못하게 막는다. */
	df = kzalloc_obj(*df, GFP_KERNEL_ACCOUNT);
	/* [한국어] 할당 실패면 ERR_PTR 로 알린다. 호출자는 앞서 잡은 등록 참조를 되돌린다. */
	if (!df)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 이 fd 가 가리킬 디바이스를 기록한다. close 까지 바뀌지 않으며,
	 * 참조는 여기서 잡지 않는다 — 호출자가 이미 try_get 으로 잡아 둔 상태다.
	 * 나머지 필드(group, iommufd, kvm, access_granted)는 0 초기화로
	 * "아직 없음, 권한 없음" 을 뜻한다. */
	df->device = device;
	/* [한국어] kvm 필드를 지킬 spinlock 을 초기화한다. KVM 쪽이 비동기로 이 fd 에
	 * KVM 포인터를 쓰기 때문에 fd 생성 시점부터 락이 준비돼 있어야 한다. */
	spin_lock_init(&df->kvm_ref_lock);

	/* [한국어] 완성된 fd 컨텍스트를 돌려준다. 호출자가 file 의 private_data 에 건다. */
	return df;
}

/* [한국어]
 * vfio_df_device_first_open - 디바이스가 처음 열릴 때만 하는 세 가지를 한다
 *
 * @df: 방금 열린 fd 의 컨텍스트. df->iommufd 가 채워져 있으면 2세대(cdev)
 *      경로, NULL 이면 1세대(group) 경로다.
 * @return: 0 성공, 음수 errno 실패. 실패하면 호출자 vfio_df_open 이
 *          open_count 를 다시 내린다.
 *
 * 왜 "첫 open" 에서만인가: 한 디바이스에 fd 가 여러 개 열려도 하드웨어 초기화,
 * IOMMU 결합, vendor 모듈 참조는 **한 번씩만** 있어야 한다. 그래서 vfio_df_open
 * 이 open_count 를 1 로 올린 그 순간에만 이 함수를 부른다.
 *
 * 세 가지 획득과 각각의 짝:
 *  1. try_module_get(device->dev->driver->owner)
 *     : vendor 모듈이 rmmod 되지 못하게 한다. 짝은 실패 경로의 module_put 과
 *       vfio_df_device_last_close 의 module_put. 빠뜨리면 사용자가 fd 를 쥔
 *       채로 vendor 모듈이 언로드돼 ops 포인터가 죽는다.
 *       driver->owner 를 쓰는 이유는 vfio_device 를 만든 모듈이 곧 하부 버스
 *       드라이버 모듈이기 때문이다.
 *  2. IOMMU 결합 — 두 세대가 갈리는 지점.
 *     df->iommufd 가 있으면 vfio_df_iommufd_bind(iommufd.c:140),
 *     없으면 vfio_device_group_use_iommu(group.c). 짝은 각각
 *     vfio_df_iommufd_unbind 와 vfio_device_group_unuse_iommu.
 *     빠뜨리면 디바이스가 어느 주소공간에도 붙지 않은 채 DMA 를 하게 된다.
 *  3. ops->open_device — vendor 가 하드웨어를 실제로 깨우는 곳. vfio-pci 면
 *     여기서 config space 를 저장하고 인터럽트를 준비한다. 짝은
 *     vfio_df_device_last_close 의 ops->close_device.
 *     선택적 콜백이라 없으면 건너뛴다.
 *
 * 순서가 중요한 이유: 모듈 참조를 가장 먼저 잡아야 그 뒤의 ops 호출이 안전하고,
 * IOMMU 결합을 vendor open 보다 먼저 해야 vendor 가 open 중에 DMA 를 준비할 수
 * 있다. 되돌리는 순서는 정확히 그 역순이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev_set->lock 보유(첫 줄이 단언한다).
 *
 * 에러 경로:
 *  - err_unuse_iommu: ops->open_device 실패. 2번을 세대에 맞게 되돌린다.
 *  - err_module_put: IOMMU 결합 실패. 1번을 되돌린다.
 *  두 라벨이 fall-through 로 이어져 있어 뒤쪽 실패가 앞쪽 획득까지 모두 되돌린다.
 *
 * 호출 체인:
 *   vfio_df_open → [vfio_df_device_first_open]
 *     → try_module_get / vfio_df_iommufd_bind 또는 vfio_device_group_use_iommu /
 *   ops->open_device
 */
static int vfio_df_device_first_open(struct vfio_device_file *df)
{
	struct vfio_device *device = df->device;
	/* [한국어] 세대 판별용으로 지역 변수에 떠 둔다. 아래 두 곳에서 같은 값을 써야
	 * 획득과 되돌림이 어긋나지 않는다. */
	struct iommufd_ctx *iommufd = df->iommufd;
	/* [한국어] 각 단계의 결과 코드. */
	int ret;

	/* [한국어] 호출자가 dev_set->lock 을 잡고 왔는지 확인한다. 이 함수가 만지는 모듈
	 * 참조와 IOMMU 결합, open_count 가 모두 그 락으로 보호된다. */
	lockdep_assert_held(&device->dev_set->lock);

	/* [한국어] vendor 모듈 참조를 잡는다. driver->owner 를 쓰는 이유는 vfio_device 를
	 * 만든 모듈이 곧 하부 버스 드라이버 모듈이기 때문이다. 실패는 그 모듈이
	 * 이미 언로드 중이라는 뜻이다. */
	if (!try_module_get(device->dev->driver->owner))
		/* [한국어] 모듈을 붙잡지 못하면 디바이스를 열 수 없다. */
		return -ENODEV;

	/* [한국어] 2세대 경로면 */
	if (iommufd)
		/* [한국어] iommufd 컨텍스트에 디바이스를 바인딩한다(iommufd.c:140). */
		ret = vfio_df_iommufd_bind(df);
	else
		/* [한국어] 1세대 경로면 group 단위로 IOMMU 사용을 신청한다. group 전체가 한
		 * container 에 붙어 있어야 성공한다. */
		ret = vfio_device_group_use_iommu(device);
	/* [한국어] 결합 실패면 */
	if (ret)
		/* [한국어] 모듈 참조만 되돌리면 된다. */
		goto err_module_put;

	/* [한국어] vendor 가 open 콜백을 제공하면 */
	if (device->ops->open_device) {
		/* [한국어] 하드웨어를 실제로 깨운다. vfio-pci 는 여기서 config space 를 저장하고
		 * 인터럽트를 준비한다. */
		ret = device->ops->open_device(device);
		/* [한국어] 실패하면 */
		if (ret)
			/* [한국어] IOMMU 결합부터 되돌린다. */
			goto err_unuse_iommu;
	}
	return 0;

err_unuse_iommu:
	/* [한국어] vendor 의 open 이 실패했을 때의 되돌림. 결합할 때와 **같은 기준**으로
	 * 갈라야 짝이 맞는다. */
	if (iommufd)
		/* [한국어] 2세대 결합 해제. */
		vfio_df_iommufd_unbind(df);
	else
		/* [한국어] 1세대 결합 해제. */
		vfio_device_group_unuse_iommu(device);
/* [한국어] IOMMU 결합 실패 지점이 합류하는 라벨. */
err_module_put:
	/* [한국어] 모듈 참조를 되돌린다. 두 라벨이 이어져 있어 뒤쪽 실패가 앞쪽 획득까지
	 * 모두 되돌린다. */
	module_put(device->dev->driver->owner);
	return ret;
}

/* [한국어]
 * vfio_df_device_last_close - 마지막 fd 가 닫힐 때 첫 open 의 획득을 모두 되돌린다
 *
 * @df: 닫히는 fd 의 컨텍스트.
 * @return: 없음. **실패할 수 없다** — 닫기는 되돌릴 곳이 없다.
 *
 * 무엇의 짝인가: vfio_df_device_first_open 의 정확한 역순이다.
 *  1. ops->close_device  ↔ ops->open_device
 *  2. iommufd unbind 또는 group unuse_iommu ↔ 각각의 bind/use
 *  3. module_put         ↔ try_module_get
 * 여기에 하나가 더 붙는다.
 *  4. device->precopy_info_v2 를 0 으로 되돌린다. 이 플래그는 사용자가
 *     VFIO_DEVICE_FEATURE_MIG_PRECOPY_INFOv2 로 켠 것이라
 *     (vfio_ioctl_device_feature_migration_precopy_info_v2), 다음 사용자가
 *     이전 사용자의 설정을 물려받지 않도록 여기서 지운다. **fd 상태가 아니라
 *     device 상태라서** df 를 free 하는 것만으로는 지워지지 않는다.
 *
 * 세대 분기: df->iommufd 를 지역 변수로 먼저 떠 두고 쓴다. 이 값이 NULL 인지가
 * 1세대와 2세대를 가르며, first_open 과 정확히 같은 기준이어야 짝이 맞는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev_set->lock 보유(첫 줄 단언).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_df_close → [vfio_df_device_last_close]
 *     → ops->close_device / vfio_df_iommufd_unbind 또는
 *   vfio_device_group_unuse_iommu / module_put
 */
static void vfio_df_device_last_close(struct vfio_device_file *df)
{
	struct vfio_device *device = df->device;
	/* [한국어] 세대 판별에 쓸 값을 지역 변수로 떠 둔다. first_open 과 **같은 기준**을
	 * 써야 획득과 해제의 짝이 맞는다. */
	struct iommufd_ctx *iommufd = df->iommufd;

	/* [한국어] close 쪽도 같은 락을 요구한다. first_open 과 last_close 가 같은 락 아래에서
	 * 일어나야 획득과 해제가 뒤섞이지 않는다. */
	lockdep_assert_held(&device->dev_set->lock);

	/* [한국어] vendor 가 close 콜백을 제공하면 */
	if (device->ops->close_device)
		/* [한국어] 먼저 하드웨어를 정리하게 한다. IOMMU 결합을 끊기 전에 불러야 vendor 가
		 * 마지막 DMA 를 안전하게 마칠 수 있다. */
		device->ops->close_device(device);
	/* [한국어] 세대에 따라 IOMMU 결합을 끊는다. */
	if (iommufd)
		/* [한국어] 2세대는 iommufd 바인딩 해제. */
		vfio_df_iommufd_unbind(df);
	else
		/* [한국어] 1세대는 group 의 IOMMU 사용 신청 취소. */
		vfio_device_group_unuse_iommu(device);
	/* [한국어] 이 플래그는 fd 가 아니라 device 에 사는 상태라, df 를 해제하는 것만으로는
	 * 지워지지 않는다. 다음 사용자가 이전 사용자의 opt-in 을 물려받지 않도록
	 * 여기서 되돌린다. */
	device->precopy_info_v2 = 0;
	/* [한국어] vendor 모듈 참조를 놓는다. first_open 의 try_module_get 짝이며,
	 * 이제 rmmod 가 가능해진다. */
	module_put(device->dev->driver->owner);
}

/* [한국어]
 * vfio_df_open - fd 하나가 디바이스 사용을 시작한다고 등록한다
 *
 * @df: 열리는 fd 의 컨텍스트. df->group 이 NULL 인지로 세대를 판별한다.
 * @return: 0 성공. -EINVAL 이면 cdev 경로에서 이미 열린 디바이스를 또 열려 한
 *          것이고, 그 밖의 음수는 vfio_df_device_first_open 의 오류다.
 *
 * 왜 필요한가: open_count 를 관리하는 **유일한 증가 지점**이다. 그리고 두
 * 세대의 다중 open 정책 차이가 여기서 강제된다.
 *
 * 다중 open 정책(상류 주석이 밝힌 이유):
 * group 경로만 한 디바이스를 여러 번 여는 것을 허용한다. group 은 이미
 * "이 group 전체를 한 사용자가 소유한다" 는 보증이 있어서, 같은 디바이스의
 * 두 번째 fd 도 같은 소유자임이 확실하다. cdev 경로에는 그런 보증이 없어
 * 두 번째 open 을 안전하게 처리할 방법이 없고, 그래서 df->group 이 NULL 인데
 * open_count 가 이미 0 이 아니면 -EINVAL 로 거절한다.
 *
 * 동작 과정:
 *  1. dev_set->lock 보유 단언 — open_count 를 만지므로 필수다.
 *  2. 위의 다중 open 검사.
 *  3. open_count 를 올린다. 락 아래이므로 원자 연산이 필요 없다.
 *  4. 1 이 됐다면(= 이 fd 가 첫 사용자) vfio_df_device_first_open 호출.
 *  5. 그것이 실패하면 3번을 되돌린다. **이 되돌림이 없으면** open_count 가
 *     영영 0 으로 내려오지 않아 vfio_df_close 의 last_close 가 불리지 않고,
 *     나아가 디바이스가 영원히 "열린 상태" 로 남는다.
 *
 * 주의: 이 함수는 df->access_granted 를 건드리지 않는다. 그 플래그는 호출자가
 * 자기 경로의 나머지 준비(iommufd compat attach, devid 복사)까지 마친 뒤에
 * smp_store_release 로 세운다(group.c:220, device_cdev.c:157).
 *
 * 실행 컨텍스트: 프로세스 문맥, dev_set->lock 보유. 호출자에 따라
 * group->group_lock 도 함께 잡혀 있다(group.c 경로).
 *
 * 호출 체인:
 *   vfio_df_group_open(group.c:206) / vfio_df_ioctl_bind_iommufd(device_cdev.c:143)
 *     → [vfio_df_open] → vfio_df_device_first_open
 */
int vfio_df_open(struct vfio_device_file *df)
{
	struct vfio_device *device = df->device;
	/* [한국어] 첫 open 이 아닐 때는 아무 일도 없이 0 을 반환하므로 초기값이 필요하다. */
	int ret = 0;

	/* [한국어] open_count 를 만지므로 dev_set->lock 이 필수다. 이 함수는 락을 잡지 않고
	 * 호출자(group.c 또는 device_cdev.c)가 잡아 온 것을 확인만 한다. */
	lockdep_assert_held(&device->dev_set->lock);

	/*
	 * Only the group path allows the device to be opened multiple
	 * times.  The device cdev path doesn't have a secure way for it.
	 */
	/* [한국어] cdev 경로(group 이 NULL)에서 이미 열린 디바이스를 또 여는 것을 막는다.
	 * 상류 주석이 이유를 밝힌다 — group 경로는 "이 group 전체를 한 사용자가
	 * 소유한다" 는 보증이 있어 두 번째 fd 도 같은 소유자임이 확실하지만,
	 * cdev 경로에는 그런 보증이 없다. */
	if (device->open_count != 0 && !df->group)
		return -EINVAL;

	/* [한국어] 카운트를 올린다. dev_set->lock 아래라 원자 연산이 필요 없다. */
	device->open_count++;
	/* [한국어] 이 fd 가 첫 사용자라면 */
	if (device->open_count == 1) {
		/* [한국어] 모듈 참조, IOMMU 결합, vendor open_device 를 한 번만 수행한다. */
		ret = vfio_df_device_first_open(df);
		/* [한국어] 실패하면 */
		if (ret)
			/* [한국어] 올렸던 카운트를 되돌린다. 이 되돌림이 없으면 카운트가 영영 0 으로
			 * 내려오지 않아 last_close 가 불리지 않고, 디바이스가 영원히 열린 상태로 남는다. */
			device->open_count--;
	}

	return ret;
}

/* [한국어]
 * vfio_df_close - fd 하나가 디바이스 사용을 끝냈다고 등록한다
 *
 * @df: 닫히는 fd 의 컨텍스트.
 * @return: 없음.
 *
 * 왜 필요한가: open_count 를 관리하는 **유일한 감소 지점**이며, 1→0 전이에서만
 * vfio_df_device_last_close 를 부른다.
 *
 * 동작 과정:
 *  1. dev_set->lock 보유 단언.
 *  2. vfio_assert_device_open 으로 정말 열려 있었는지 확인. 0 이면 WARN 을
 *     찍고 **아무것도 하지 않고 반환** 한다. 여기서 그냥 진행하면 open_count 가
 *     부호 없는 정수라서 0 에서 내려가 매우 큰 값으로 감싸 돌고, 그 디바이스는
 *     두 번 다시 닫히지 않는다.
 *  3. 1 이면(= 이 fd 가 마지막 사용자) last_close 로 vendor 콜백과 IOMMU 결합
 *     해제를 먼저 한 뒤,
 *  4. open_count 를 내린다. **감소를 last_close 뒤에 두는 것이 중요하다** —
 *     last_close 안에서 vendor 가 vfio_assert_device_open 을 통과해야 하고,
 *     같은 dev_set 의 다른 디바이스가 open_count 합을 볼 때도 아직 정리 중임이
 *     보여야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, dev_set->lock 보유.
 *
 * 호출 체인:
 *   vfio_df_group_close(group.c:227,245) / vfio_df_unbind_iommufd(device_cdev.c:186)
 *   / 각 경로의 실패 되돌림(device_cdev.c:162)
 *     → [vfio_df_close] → vfio_assert_device_open / vfio_df_device_last_close
 */
void vfio_df_close(struct vfio_device_file *df)
{
	struct vfio_device *device = df->device;

	/* [한국어] close 쪽도 같은 락을 요구한다. */
	lockdep_assert_held(&device->dev_set->lock);

	/* [한국어] 정말 열려 있었는지 확인한다. 0 이면 WARN 을 찍고 */
	if (!vfio_assert_device_open(device))
		/* [한국어] 아무것도 하지 않고 반환한다. 그냥 진행하면 open_count 가 부호 없는
		 * 정수라 0 에서 내려가 매우 큰 값으로 감싸 돌고, 그 디바이스는 두 번 다시
		 * 닫히지 않는다. */
		return;
	/* [한국어] 이 fd 가 마지막 사용자라면 */
	if (device->open_count == 1)
		/* [한국어] vendor 콜백과 IOMMU 결합 해제를 먼저 한다. */
		vfio_df_device_last_close(df);
	/* [한국어] 그다음에 카운트를 내린다. 순서가 중요하다 — last_close 안에서 vendor 가
	 * 디바이스 열림 검사를 통과해야 하고, 같은 dev_set 의 다른 디바이스가
	 * 합계를 볼 때도 아직 정리 중임이 보여야 한다. */
	device->open_count--;
}

/* [한국어]
 * vfio_device_pm_runtime_get - ioctl 을 처리하는 동안 디바이스를 D0 상태로 붙잡는다
 *
 * @device: 대상 디바이스.
 * @return: 0 성공(또는 런타임 PM 을 안 쓰는 디바이스라 할 일이 없음).
 *          -EIO 면 resume 에 실패했다는 뜻이며 ioctl 을 진행하면 안 된다.
 *
 * 왜 필요한가: 사용자가 VFIO_DEVICE_FEATURE 나 vendor ioctl 로 디바이스
 * 레지스터를 만지는 동안 디바이스가 저전력 상태(D3)로 내려가 있으면 그 접근이
 * 실패하거나 머신 체크를 일으킨다. 이 래퍼가 ioctl 진입 시 참조를 잡아
 * D0 로 끌어올리고 깨어 있게 만든다.
 *
 * dev->driver 와 dev->driver->pm 을 먼저 보는 이유: 하부 드라이버가 런타임 PM
 * 콜백을 아예 제공하지 않으면 pm_runtime 참조를 잡아 봐야 의미가 없고,
 * 불필요한 카운터 조작만 남는다. 조건을 통과하지 못하면 0 을 반환해
 * "성공했지만 잡은 것은 없음" 이 되고, 짝인 put 도 같은 조건으로 건너뛴다.
 * **두 함수의 조건식이 정확히 같아야** 참조가 어긋나지 않는다.
 *
 * 동작 과정: pm_runtime_resume_and_get 을 부른다. 이 API 는 실패 시 참조를
 * 스스로 되돌려 주므로 호출자가 실패 경로에서 put 을 부를 필요가 없다.
 * 실패하면 원인 값을 ratelimit 로그로 남기고 -EIO 로 단순화해 돌려준다.
 * 로그를 ratelimit 로 두는 이유는 ioctl 마다 불리는 자리라 폭주할 수 있어서다.
 *
 * 실행 컨텍스트: ioctl 진입 직후, 프로세스 문맥. 잠들 수 있다.
 *
 * 에러 경로: -EIO. vfio_device_fops_unl_ioctl 은 그것을 그대로 사용자에게
 * 돌려주고 **out 라벨을 거치지 않는다** — 잡은 것이 없으므로 put 도 없다.
 *
 * 호출 체인:
 *   vfio_device_fops_unl_ioctl → [vfio_device_pm_runtime_get]
 *     → pm_runtime_resume_and_get
 */
/*
 * Wrapper around pm_runtime_resume_and_get().
 * Return error code on failure or 0 on success.
 */
static inline int vfio_device_pm_runtime_get(struct vfio_device *device)
{
	/* [한국어] 런타임 PM API 는 하부 struct device 를 받는다. */
	struct device *dev = device->dev;

	/* [한국어] 하부 드라이버가 런타임 PM 콜백을 아예 제공하지 않으면 참조를 잡아 봐야
	 * 의미가 없고 카운터만 흔든다. 이 조건이 곧 put 쪽 조건이기도 하다. */
	if (dev->driver && dev->driver->pm) {
		/* [한국어] 이 블록 안에서만 쓰는 결과 코드. */
		int ret;

		/* [한국어] 디바이스를 깨우고 참조를 잡는다. 이 API 는 **실패 시 참조를 스스로
		 * 되돌려 주므로** 호출자가 실패 경로에서 put 을 부를 필요가 없다. */
		ret = pm_runtime_resume_and_get(dev);
		/* [한국어] resume 실패. */
		if (ret) {
			/* [한국어] 원인 값을 로그로 남긴다. ioctl 마다 불리는 자리라 ratelimit 판을 써서
			 * 로그 폭주를 막는다. */
			dev_info_ratelimited(dev,
				"vfio: runtime resume failed %d\n", ret);
			/* [한국어] 구체적 원인을 -EIO 로 단순화해 돌려준다. 사용자에게는 어느 PM 오류든
			 * "디바이스에 접근할 수 없다" 로 같은 뜻이다. */
			return -EIO;
		}
	}

	return 0;
}

/* [한국어]
 * vfio_device_pm_runtime_put - ioctl 이 끝났으니 런타임 PM 참조를 놓는다
 *
 * @device: 대상 디바이스.
 * @return: 없음.
 *
 * 무엇의 짝인가: vfio_device_pm_runtime_get 과 1:1 이다. 조건식
 * (dev->driver && dev->driver->pm)이 get 쪽과 **글자 그대로 같아야** 한다.
 * 한쪽만 참이 되는 상황이 생기면 참조가 새거나 음수가 된다. 현재 코드에서는
 * 두 함수가 같은 device 의 같은 필드를 보므로 그럴 여지가 없다.
 *
 * 빠뜨렸을 때: pm_runtime 사용 카운트가 내려오지 않아 디바이스가 영원히
 * D0 에 머문다. 기능은 멀쩡하지만 전력이 계속 소모되고, 시스템 서스펜드도
 * 막힐 수 있다.
 *
 * 실행 컨텍스트: ioctl 종료 직전, 프로세스 문맥.
 *
 * 호출 체인:
 *   vfio_device_fops_unl_ioctl 의 out 라벨 → [vfio_device_pm_runtime_put]
 *     → pm_runtime_put
 */
/*
 * Wrapper around pm_runtime_put().
 */
static inline void vfio_device_pm_runtime_put(struct vfio_device *device)
{
	struct device *dev = device->dev;

	/* [한국어] get 쪽과 **글자 그대로 같은 조건식**이다. 하부 드라이버가 런타임 PM
	 * 콜백을 제공하는 경우에만 참조를 잡았으므로, 놓는 조건도 같아야 짝이 맞는다. */
	if (dev->driver && dev->driver->pm)
		/* [한국어] 사용 카운트를 내린다. 0 이 되면 PM 코어가 유휴 처리를 시작할 수 있다. */
		pm_runtime_put(dev);
}

/* [한국어]
 * vfio_device_fops_release - device fd 가 닫힐 때 fd 쪽 자원을 모두 반납한다
 *
 * @inode: VFS 가 주는 inode. 이 함수는 쓰지 않는다.
 * @filep: 닫히는 file. private_data 가 vfio_device_file 이다.
 * @return: 항상 0. release 훅의 반환값은 VFS 가 close(2) 결과로 쓰지만,
 *          실패해도 fd 는 이미 닫히므로 의미 있는 실패를 만들지 않는다.
 *
 * 왜 두 세대를 나누는가: fd 가 어느 경로로 열렸는지에 따라 되돌릴 것이 다르다.
 *  - df->group 이 있으면 1세대: vfio_df_group_close 가 group_lock 과
 *    dev_set->lock 을 잡고 vfio_df_close + KVM 참조 반납까지 한다.
 *  - NULL 이면 2세대: vfio_df_unbind_iommufd 가 iommufd 컨텍스트 참조까지
 *    놓는다. 이쪽은 **BIND 를 안 한 채 close 된 fd** 도 처리해야 해서
 *    df->access_granted 를 보고 조용히 빠져나가는 분기를 갖는다
 *    (device_cdev.c:182).
 *
 * 동작 과정:
 *  1. 세대에 맞는 close 헬퍼 호출.
 *  2. vfio_device_put_registration — open 진입점에서 잡은 등록 참조를 놓는다.
 *     이 put 이 vfio_device_fops_cdev_open(device_cdev.c:31)의 try_get 또는
 *     group 경로 vfio_device_get_from_name(group.c:50)의 try_get 과 짝이다.
 *     **이것을 빠뜨리면** vfio_unregister_group_dev 가 영영 깨어나지 못한다.
 *  3. df 자체를 kfree. vfio_allocate_device_file 의 짝이다.
 *
 * 주의: 여기서 struct vfio_device 를 free 하지 않는다. 그 수명은 kref 가
 * 관리하며 vendor 의 vfio_put_device() 로만 줄어든다.
 *
 * 실행 컨텍스트: close(2) 또는 프로세스 종료 시의 파일 정리, 프로세스 문맥.
 * 잠들 수 있다(내부에서 mutex 를 잡는다).
 *
 * 호출 체인:
 *   close(2) → VFS → vfio_device_fops 의 .release → [vfio_device_fops_release]
 *     → vfio_df_group_close 또는 vfio_df_unbind_iommufd /
 *   vfio_device_put_registration / kfree
 */
/*
 * VFIO Device fd
 */
static int vfio_device_fops_release(struct inode *inode, struct file *filep)
{
	/* [한국어] fd 컨텍스트를 꺼낸다. */
	struct vfio_device_file *df = filep->private_data;
	/* [한국어] df 를 해제하기 전에 device 포인터를 지역 변수로 떠 둔다. 아래 kfree 후에는
	 * df 를 읽을 수 없으므로 순서가 강제된다. */
	struct vfio_device *device = df->device;

	/* [한국어] 어느 세대로 열린 fd 인지에 따라 되돌릴 것이 다르다. group 포인터가
	 * 채워져 있으면 1세대다. */
	if (df->group)
		/* [한국어] group_lock 과 dev_set->lock 을 잡고 vfio_df_close 와 KVM 참조 반납까지
		 * 한다(group.c:238). */
		vfio_df_group_close(df);
	else
		/* [한국어] 2세대. iommufd 컨텍스트 참조까지 놓는다. BIND 를 하지 않은 채 close 된
		 * fd 도 여기로 오는데, device_cdev.c:182 가 access_granted 를 보고 조용히
		 * 빠져나간다. */
		vfio_df_unbind_iommufd(df);

	/* [한국어] open 진입점에서 잡은 등록 참조를 놓는다. cdev 경로는 device_cdev.c:31,
	 * group 경로는 group.c:50 의 try_get 과 짝이다. 이것을 빠뜨리면
	 * vfio_unregister_group_dev 가 영영 깨어나지 못한다. */
	vfio_device_put_registration(device);

	/* [한국어] fd 컨텍스트 해제. vfio_allocate_device_file 의 짝이다.
	 * struct vfio_device 자체는 여기서 해제하지 않는다 — 그 수명은 kref 가
	 * 관리하며 vendor 의 vfio_put_device() 로만 줄어든다. */
	kfree(df);

	return 0;
}

/* [한국어]
 * vfio_mig_get_next_state - 마이그레이션 상태 기계에서 "다음 한 걸음" 을 계산한다
 *
 * @device: 대상 디바이스. device->migration_flags 로 이 디바이스가 어떤 선택
 *          기능(P2P, PRE_COPY)을 지원하는지 판단한다.
 * @cur_fsm: 현재 상태.
 * @new_fsm: 사용자가 요청한 목표 상태.
 * @next_fsm: 출력. 목표로 가기 위해 **지금 밟아야 할 한 걸음**이 여기 담긴다.
 * @return: 0 성공(next_fsm 유효). -EINVAL 이면 현재 상태나 목표 상태가 이
 *          디바이스가 지원하지 않는 기능을 요구하거나, 도달 경로가 없다는 뜻.
 *
 * 왜 한 걸음씩인가: 사용자는 "RESUMING 에서 PRE_COPY 로" 처럼 멀리 떨어진 두
 * 상태를 한 번에 요청할 수 있다. 그러나 vendor 드라이버에게 그 모든 조합의
 * 전이를 구현하라고 요구하면 부담이 너무 크다. 그래서 코어가 **기본 아크만
 * 구현하면 되도록** 중간 경로를 쪼개 준다. 상류 주석의 긴 목록이 vendor 가
 * 반드시 구현해야 하는 아크와, 코어가 대신 쪼개 주는 조합 전이를 열거한다.
 * 호출자는 next_fsm 이 new_fsm 과 같아질 때까지 이 함수를 반복 호출한다.
 *
 * 두 개의 정적 표:
 *  - vfio_from_fsm_table[현재][목표] : 다음 한 걸음. 8x8 이며 값 자체도 상태다.
 *  - state_flags_table[상태]         : 그 상태에 들어가려면 어떤 기능 플래그가
 *    필요한가. 예컨대 PRE_COPY 는 STOP_COPY 와 PRE_COPY 둘 다 요구한다.
 *    ERROR 는 ~0U 라 **어떤 device 도 지원 집합을 만족시킬 수 없다** —
 *    그래서 아래 검사에서 ERROR 를 cur/new 로 주면 반드시 -EINVAL 이 된다.
 *
 * 동작 과정:
 *  1. cur_fsm 범위 검사 + 지원 검사. 현재 상태가 디바이스 능력을 넘어서면
 *     코어나 vendor 의 버그이므로 WARN_ON 을 동반한다.
 *  2. new_fsm 도 같은 검사. 다만 이쪽은 **사용자 입력**이라 WARN 없이 조용히
 *     -EINVAL 을 준다. 사용자가 잘못된 값을 넣었다고 커널 로그를 더럽히지
 *     않는다. 이 비대칭이 1번과 2번을 따로 쓴 이유다.
 *  3. 표에서 한 걸음을 꺼낸다.
 *  4. 그 걸음이 이 디바이스가 지원하지 않는 상태면, 그 상태에서 다시 표를
 *     타고 넘어간다. 상류 주석이 말하는 "선택 상태를 건너뛴다" 가 이 while
 *     루프다. 예를 들어 P2P 미지원 디바이스에서 RUNNING_P2P 가 나오면 그
 *     자리에서 다시 표를 타 STOP 등으로 넘어간다.
 *  5. 최종 걸음이 ERROR 면 -EINVAL, 아니면 0.
 *
 * 왜 루프가 끝나는가: 표는 지원하지 않는 상태에서 출발해도 결국 지원되는
 * 상태나 ERROR 로 수렴하도록 짜여 있다. ERROR 행은 모든 열이 ERROR 이므로
 * ERROR 에 닿으면 그 자리에 머무르고 5번에서 -EINVAL 로 끝난다.
 *
 * 실행 컨텍스트: vendor 의 migration_set_state 구현 안, 프로세스 문맥. 순수
 * 계산 함수라 락도 잠도 없다. 재진입해도 안전하다.
 *
 * 호출 체인:
 *   vendor 의 migration_set_state(예: drivers/vfio/pci/pds/vfio_dev.c:59,
 *   drivers/vfio/pci/virtio/migrate.c:1227) → [vfio_mig_get_next_state]
 */
/*
 * vfio_mig_get_next_state - Compute the next step in the FSM
 * @cur_fsm - The current state the device is in
 * @new_fsm - The target state to reach
 * @next_fsm - Pointer to the next step to get to new_fsm
 *
 * Return 0 upon success, otherwise -errno
 * Upon success the next step in the state progression between cur_fsm and
 * new_fsm will be set in next_fsm.
 *
 * This breaks down requests for combination transitions into smaller steps and
 * returns the next step to get to new_fsm. The function may need to be called
 * multiple times before reaching new_fsm.
 *
 */
int vfio_mig_get_next_state(struct vfio_device *device,
			    enum vfio_device_mig_state cur_fsm,
			    enum vfio_device_mig_state new_fsm,
			    enum vfio_device_mig_state *next_fsm)
{
	/* [한국어] 함수 지역 익명 enum 으로 상태 개수를 정의한다. 마지막 상태 값에 1 을 더한
	 * 것이라 uapi 에 새 상태가 추가되면 자동으로 커진다. 아래 두 표의 크기가
	 * 모두 이 값을 쓴다. */
	enum { VFIO_DEVICE_NUM_STATES = VFIO_DEVICE_STATE_PRE_COPY_P2P + 1 };
	/*
	 * The coding in this table requires the driver to implement the
	 * following FSM arcs:
	 *         RESUMING -> STOP
	 *         STOP -> RESUMING
	 *         STOP -> STOP_COPY
	 *         STOP_COPY -> STOP
	 *
	 * If P2P is supported then the driver must also implement these FSM
	 * arcs:
	 *         RUNNING -> RUNNING_P2P
	 *         RUNNING_P2P -> RUNNING
	 *         RUNNING_P2P -> STOP
	 *         STOP -> RUNNING_P2P
	 *
	 * If precopy is supported then the driver must support these additional
	 * FSM arcs:
	 *         RUNNING -> PRE_COPY
	 *         PRE_COPY -> RUNNING
	 *         PRE_COPY -> STOP_COPY
	 * However, if precopy and P2P are supported together then the driver
	 * must support these additional arcs beyond the P2P arcs above:
	 *         PRE_COPY -> RUNNING
	 *         PRE_COPY -> PRE_COPY_P2P
	 *         PRE_COPY_P2P -> PRE_COPY
	 *         PRE_COPY_P2P -> RUNNING_P2P
	 *         PRE_COPY_P2P -> STOP_COPY
	 *         RUNNING -> PRE_COPY
	 *         RUNNING_P2P -> PRE_COPY_P2P
	 *
	 * Without P2P and precopy the driver must implement:
	 *         RUNNING -> STOP
	 *         STOP -> RUNNING
	 *
	 * The coding will step through multiple states for some combination
	 * transitions; if all optional features are supported, this means the
	 * following ones:
	 *         PRE_COPY -> PRE_COPY_P2P -> STOP_COPY
	 *         PRE_COPY -> RUNNING -> RUNNING_P2P
	 *         PRE_COPY -> RUNNING -> RUNNING_P2P -> STOP
	 *         PRE_COPY -> RUNNING -> RUNNING_P2P -> STOP -> RESUMING
	 *         PRE_COPY_P2P -> RUNNING_P2P -> RUNNING
	 *         PRE_COPY_P2P -> RUNNING_P2P -> STOP
	 *         PRE_COPY_P2P -> RUNNING_P2P -> STOP -> RESUMING
	 *         RESUMING -> STOP -> RUNNING_P2P
	 *         RESUMING -> STOP -> RUNNING_P2P -> PRE_COPY_P2P
	 *         RESUMING -> STOP -> RUNNING_P2P -> RUNNING
	 *         RESUMING -> STOP -> RUNNING_P2P -> RUNNING -> PRE_COPY
	 *         RESUMING -> STOP -> STOP_COPY
	 *         RUNNING -> RUNNING_P2P -> PRE_COPY_P2P
	 *         RUNNING -> RUNNING_P2P -> STOP
	 *         RUNNING -> RUNNING_P2P -> STOP -> RESUMING
	 *         RUNNING -> RUNNING_P2P -> STOP -> STOP_COPY
	 *         RUNNING_P2P -> RUNNING -> PRE_COPY
	 *         RUNNING_P2P -> STOP -> RESUMING
	 *         RUNNING_P2P -> STOP -> STOP_COPY
	 *         STOP -> RUNNING_P2P -> PRE_COPY_P2P
	 *         STOP -> RUNNING_P2P -> RUNNING
	 *         STOP -> RUNNING_P2P -> RUNNING -> PRE_COPY
	 *         STOP_COPY -> STOP -> RESUMING
	 *         STOP_COPY -> STOP -> RUNNING_P2P
	 *         STOP_COPY -> STOP -> RUNNING_P2P -> RUNNING
	 *
	 *  The following transitions are blocked:
	 *         STOP_COPY -> PRE_COPY
	 *         STOP_COPY -> PRE_COPY_P2P
	 */
	/* [한국어] 전이표. 행이 현재 상태, 열이 최종 목표 상태이고, 값은 **지금 밟아야 할
	 * 한 걸음**이다. u8 로 둔 것은 상태 값이 작아 표를 캐시에 담기 쉽게 하기 위함이다.
	 * 설정자: 컴파일 시 정적 초기화. static 이라 호출마다 다시 만들지 않는다.
	 * 읽는 자: 아래 범위 검사와 걸음 계산.
	 * 값 범위: 유효한 상태 값. 도달 불가 조합은 ERROR.
	 * 동기화: const 라 필요 없다. */
	static const u8 vfio_from_fsm_table[VFIO_DEVICE_NUM_STATES][VFIO_DEVICE_NUM_STATES] = {
		/* [한국어] 현재 STOP 일 때의 행. 정지 상태에서 어디로든 가는 첫 걸음들이다. */
		[VFIO_DEVICE_STATE_STOP] = {
			/* [한국어] 목표도 STOP 이면 이미 도착했다. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] RUNNING 으로 가려면 먼저 RUNNING_P2P 를 거친다. P2P 차단을 푸는 것과
			 * 동작을 재개하는 것을 분리해 vendor 구현을 단순하게 한다. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] PRE_COPY 로 갈 때도 같은 중간 지점을 지난다. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] PRE_COPY_P2P 로 갈 때도 마찬가지다. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] STOP_COPY 는 정지 상태에서 곧바로 갈 수 있는 기본 아크다. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP_COPY,
			/* [한국어] RESUMING 도 정지 상태에서 바로 간다. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RESUMING,
			/* [한국어] RUNNING_P2P 자체가 목표면 한 걸음이다. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] ERROR 로는 ERROR 로만 간다. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		/* [한국어] 현재 RUNNING 일 때의 행. */
		[VFIO_DEVICE_STATE_RUNNING] = {
			/* [한국어] 정지하려면 먼저 P2P 를 차단한다. 다른 디바이스가 이 디바이스로 보내던
			 * 전송을 먼저 끊어야 안전하게 멈출 수 있다. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] 이미 도착. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING,
			/* [한국어] PRE_COPY 는 동작 중에 곧바로 들어갈 수 있는 기본 아크다. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_PRE_COPY,
			/* [한국어] PRE_COPY_P2P 로 가려면 먼저 P2P 를 차단한다. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] STOP_COPY 로 가려면 정지 경로를 타야 하므로 역시 P2P 차단부터. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] RESUMING 도 마찬가지다. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] P2P 차단 자체가 목표면 한 걸음. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] 오류로는 오류만. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		/* [한국어] 현재 PRE_COPY 일 때의 행. */
		[VFIO_DEVICE_STATE_PRE_COPY] = {
			/* [한국어] 정지하려면 먼저 사전 복사를 끝내고 통상 동작으로 돌아간다. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_RUNNING,
			/* [한국어] 통상 동작 복귀는 한 걸음. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING,
			/* [한국어] 이미 도착. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_PRE_COPY,
			/* [한국어] P2P 를 차단한 사전 복사로는 한 걸음. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_PRE_COPY_P2P,
			/* [한국어] STOP_COPY 로 갈 때는 먼저 P2P 를 차단한다. 사전 복사에서 최종 덤프로
			 * 넘어가는 표준 경로다. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_PRE_COPY_P2P,
			/* [한국어] RESUMING 은 반대 방향이라 통상 동작을 거쳐 되돌아간다. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RUNNING,
			/* [한국어] P2P 차단 동작 상태로도 통상 동작을 먼저 거친다. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING,
			/* [한국어] 오류로는 오류만. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		/* [한국어] 현재 PRE_COPY_P2P 일 때의 행. */
		[VFIO_DEVICE_STATE_PRE_COPY_P2P] = {
			/* [한국어] 정지하려면 사전 복사를 먼저 끝낸다. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] 통상 동작으로 갈 때도 사전 복사를 먼저 끝낸다. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] P2P 차단만 푸는 것은 한 걸음. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_PRE_COPY,
			/* [한국어] 이미 도착. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_PRE_COPY_P2P,
			/* [한국어] 최종 덤프로는 곧바로 갈 수 있다. 사전 복사와 P2P 차단이 이미 돼 있어
			 * 다음이 STOP_COPY 인 것이 자연스럽다. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP_COPY,
			/* [한국어] 복원 방향으로는 사전 복사를 먼저 끝낸다. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] 사전 복사만 끝내면 되므로 한 걸음. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] 오류로는 오류만. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		/* [한국어] 현재 STOP_COPY 일 때의 행. */
		[VFIO_DEVICE_STATE_STOP_COPY] = {
			/* [한국어] 덤프를 끝내고 정지로 돌아가는 것은 한 걸음이자 기본 아크다. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 동작 재개도 정지를 먼저 거친다. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 덤프 중에서 사전 복사로 되돌아가는 것은 **금지**다. 상류 주석이 차단된
			 * 전이로 명시한 두 조합 중 하나이며, 이미 최종 덤프를 시작한 뒤 사전 복사로
			 * 돌아가는 것은 마이그레이션 프로토콜상 뜻이 없다. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] P2P 차단 사전 복사로 되돌아가는 것도 같은 이유로 금지다. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] 이미 도착. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP_COPY,
			/* [한국어] 복원 방향은 정지를 거친다. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] P2P 차단 동작 상태도 정지를 거친다. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 오류로는 오류만. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		/* [한국어] 현재 RESUMING 일 때의 행. 복원 중에는 사실상 정지 말고 갈 곳이 없다. */
		[VFIO_DEVICE_STATE_RESUMING] = {
			/* [한국어] 복원을 마치고 정지로 — 기본 아크다. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 동작 재개도 정지를 거친다. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 사전 복사로도 정지를 거친다. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] P2P 차단 사전 복사도 마찬가지. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 덤프로도 정지를 거친다. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 이미 도착. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_RESUMING,
			/* [한국어] P2P 차단 동작 상태도 정지를 거친다. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 오류로는 오류만. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		/* [한국어] 현재 RUNNING_P2P 일 때의 행. 정지와 동작 사이의 중간 지점이라
		 * 양쪽 모두로 한 걸음에 간다. */
		[VFIO_DEVICE_STATE_RUNNING_P2P] = {
			/* [한국어] 정지로 한 걸음. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 동작으로 한 걸음. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_RUNNING,
			/* [한국어] 사전 복사로 가려면 먼저 통상 동작으로 돌아간다. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_RUNNING,
			/* [한국어] P2P 차단 사전 복사로는 한 걸음. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_PRE_COPY_P2P,
			/* [한국어] 덤프로는 정지를 거친다. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 복원으로도 정지를 거친다. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_STOP,
			/* [한국어] 이미 도착. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_RUNNING_P2P,
			/* [한국어] 오류로는 오류만. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
		/* [한국어] 현재 ERROR 일 때의 행. 모든 열이 ERROR 라 한 번 오류에 빠지면 표만으로는
		 * 빠져나올 수 없다. 회복은 디바이스 리셋으로만 가능하다. */
		[VFIO_DEVICE_STATE_ERROR] = {
			/* [한국어] 정지를 목표로 해도 오류. */
			[VFIO_DEVICE_STATE_STOP] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] 동작을 목표로 해도 오류. */
			[VFIO_DEVICE_STATE_RUNNING] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] 사전 복사도 오류. */
			[VFIO_DEVICE_STATE_PRE_COPY] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] P2P 차단 사전 복사도 오류. */
			[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] 덤프도 오류. */
			[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] 복원도 오류. */
			[VFIO_DEVICE_STATE_RESUMING] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] P2P 차단 동작도 오류. */
			[VFIO_DEVICE_STATE_RUNNING_P2P] = VFIO_DEVICE_STATE_ERROR,
			/* [한국어] 오류에서 오류로. */
			[VFIO_DEVICE_STATE_ERROR] = VFIO_DEVICE_STATE_ERROR,
		},
	};

	/* [한국어] 상태별로 "이 상태에 들어가려면 디바이스가 어떤 선택 기능을 지원해야 하는가"
	 * 를 담은 표. 위 전이표와 짝을 이뤄 지원되지 않는 상태를 걸러 낸다.
	 * 설정자: 컴파일 시 정적 초기화. 함수 지역이지만 static 이라 호출 간 유지된다.
	 * 읽는 자: 아래 두 검사와 건너뛰기 루프.
	 * 동기화: const 라 필요 없다. */
	static const unsigned int state_flags_table[VFIO_DEVICE_NUM_STATES] = {
		/* [한국어] 정지 상태. 마이그레이션을 지원하는 모든 디바이스의 기본 요구다. */
		[VFIO_DEVICE_STATE_STOP] = VFIO_MIGRATION_STOP_COPY,
		/* [한국어] 정상 동작 상태. 역시 기본 요구만 있다. */
		[VFIO_DEVICE_STATE_RUNNING] = VFIO_MIGRATION_STOP_COPY,
		/* [한국어] 사전 복사 상태. */
		[VFIO_DEVICE_STATE_PRE_COPY] =
			/* [한국어] 기본 요구에 더해 PRE_COPY 기능이 있어야 한다. */
			VFIO_MIGRATION_STOP_COPY | VFIO_MIGRATION_PRE_COPY,
		/* [한국어] 사전 복사 중 P2P 차단 상태. 세 비트가 모두 필요하다. */
		[VFIO_DEVICE_STATE_PRE_COPY_P2P] = VFIO_MIGRATION_STOP_COPY |
						   /* [한국어] P2P(디바이스 간 직접 전송) 차단 기능과 */
						   VFIO_MIGRATION_P2P |
						   /* [한국어] 사전 복사 기능을 함께 요구한다. */
						   VFIO_MIGRATION_PRE_COPY,
		/* [한국어] 정지 후 상태 덤프 상태. 기본 요구. */
		[VFIO_DEVICE_STATE_STOP_COPY] = VFIO_MIGRATION_STOP_COPY,
		/* [한국어] 상태 복원 상태. 기본 요구. */
		[VFIO_DEVICE_STATE_RESUMING] = VFIO_MIGRATION_STOP_COPY,
		/* [한국어] 동작 중 P2P 차단 상태. */
		[VFIO_DEVICE_STATE_RUNNING_P2P] =
			/* [한국어] 기본 요구에 P2P 기능이 더해진다. */
			VFIO_MIGRATION_STOP_COPY | VFIO_MIGRATION_P2P,
		/* [한국어] 오류 상태의 요구 비트는 모든 비트다. **어떤 디바이스도 만족시킬 수 없게**
		 * 만들어, cur_fsm 이나 new_fsm 으로 ERROR 를 주면 위 검사에서 반드시
		 * -EINVAL 이 되게 하는 장치다. */
		[VFIO_DEVICE_STATE_ERROR] = ~0U,
	};

	/* [한국어] 현재 상태가 표 범위 안인지, 그리고 */
	if (WARN_ON(cur_fsm >= ARRAY_SIZE(vfio_from_fsm_table) ||
		    /* [한국어] 그 상태가 요구하는 기능 비트를 이 디바이스가 모두 지원하는지 본다.
		     * 마스크 후 값이 원래 값과 같아야 "전부 포함" 이다. */
		    (state_flags_table[cur_fsm] & device->migration_flags) !=
			/* [한국어] ERROR 행은 요구 비트가 ~0U 라 어떤 디바이스도 만족시킬 수 없고,
			 * 따라서 cur_fsm 이 ERROR 면 반드시 여기서 걸린다. */
			state_flags_table[cur_fsm]))
		/* [한국어] 현재 상태가 잘못됐다는 것은 코어나 vendor 의 버그이므로 WARN 을 동반한다. */
		return -EINVAL;

	/* [한국어] 목표 상태도 같은 두 검사를 받는다. */
	if (new_fsm >= ARRAY_SIZE(vfio_from_fsm_table) ||
	   /* [한국어] 요구 비트 포함 여부. */
	   (state_flags_table[new_fsm] & device->migration_flags) !=
			/* [한국어] 여기서는 WARN 을 붙이지 않는다 — 목표 상태는 **사용자 입력**이라,
			 * 잘못된 값을 넣었다고 커널 로그를 더럽히지 않는다. 이 비대칭이 위와
			 * 아래를 두 개의 if 로 나눈 이유다. */
			state_flags_table[new_fsm])
		return -EINVAL;

	/*
	 * Arcs touching optional and unsupported states are skipped over. The
	 * driver will instead see an arc from the original state to the next
	 * logical state, as per the above comment.
	 */
	/* [한국어] 표에서 한 걸음을 꺼낸다. 행이 현재 상태, 열이 목표 상태다. */
	*next_fsm = vfio_from_fsm_table[cur_fsm][new_fsm];
	/* [한국어] 그 걸음이 이 디바이스가 지원하지 않는 상태라면 */
	while ((state_flags_table[*next_fsm] & device->migration_flags) !=
			/* [한국어] 필요한 기능 비트가 device 의 지원 집합에 다 들어 있지 않다는 뜻이다. */
			state_flags_table[*next_fsm])
		/* [한국어] 그 상태에서 다시 표를 타 다음 걸음으로 넘어간다. 상류 주석이 말하는
		 * "선택 상태를 건너뛴다" 가 이 루프다. 예컨대 P2P 미지원 디바이스에서
		 * RUNNING_P2P 가 나오면 그 자리에서 다시 표를 타 STOP 등으로 간다.
		 * 표가 결국 지원 상태나 ERROR 로 수렴하도록 짜여 있어 루프는 끝난다. */
		*next_fsm = vfio_from_fsm_table[*next_fsm][new_fsm];

	/* [한국어] 최종 걸음이 ERROR 면 도달 경로가 없다는 뜻이라 -EINVAL,
	 * 아니면 0 과 함께 next_fsm 이 유효하다. */
	return (*next_fsm != VFIO_DEVICE_STATE_ERROR) ? 0 : -EINVAL;
}
/* [한국어] EXPORT_SYMBOL_GPL — vendor 의 상태 전이 구현이 이 계산기를 쓴다. */
EXPORT_SYMBOL_GPL(vfio_mig_get_next_state);

/* [한국어]
 * vfio_ioct_mig_return_fd - vendor 가 만든 struct file 을 fd 번호로 바꿔 사용자에게 돌려준다
 *
 * @filp: vendor 의 migration_set_state 가 만들어 준 데이터 전송용 file.
 *        이 함수가 성공하든 실패하든 **이 참조의 처리를 전담**한다.
 * @arg: 사용자 공간의 struct vfio_device_feature_mig_state 주소.
 * @mig: 커널 쪽 사본. data_fd 필드를 채워 사용자에게 되돌려 준다.
 * @return: 0 성공(fd 가 설치되고 사용자 구조체에 기록됨). 음수면 실패이며
 *          filp 은 이 함수가 fput 으로 정리한 뒤다.
 *
 * 이름에 관한 관찰: [상류 코드 관찰] 함수 이름이 vfio_ioctl_ 이 아니라
 * vfio_ioct_ 로 철자가 하나 빠져 있다. 파일 안의 다른 모든 ioctl 헬퍼는
 * vfio_ioctl_ 로 시작한다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 왜 이런 순서인가(fd 설치의 정석):
 * fd 번호를 먼저 예약하고, 사용자에게 그 번호를 성공적으로 전달한 **뒤에야**
 * fd_install 로 실제 파일을 매단다. 순서를 뒤집어 fd_install 을 먼저 하면,
 * copy_to_user 가 실패했을 때 사용자는 번호를 모르는데 fd 는 이미 프로세스
 * 파일 테이블에 살아 있어 회수할 방법이 없다(다른 스레드가 그 번호를 이미
 * 쓰고 있을 수도 있어 close 도 안전하지 않다). 이것이 커널 전반의 표준 패턴이다.
 *
 * O_CLOEXEC 를 쓰는 이유: 마이그레이션 데이터 fd 가 exec 너머로 새 나가면
 * 안 된다. 사용자가 원치 않으면 나중에 끄면 되지만, 기본은 닫는 쪽이 안전하다.
 *
 * 동작 과정:
 *  1. get_unused_fd_flags 로 번호만 예약. 실패하면 out_fput.
 *  2. mig->data_fd 에 번호를 넣고 사용자 구조체 전체를 복사. 실패하면
 *     out_put_unused 로 가서 번호 예약을 취소하고 이어서 filp 도 놓는다.
 *  3. fd_install — 이제 사용자 프로세스가 이 fd 로 파일을 볼 수 있다.
 *     이 시점 이후 filp 참조의 소유권은 사용자에게 넘어가므로 fput 하지 않는다.
 *
 * 에러 경로 두 라벨이 fall-through 로 이어져 있다: out_put_unused 는
 * put_unused_fd 뒤 out_fput 으로 떨어져 fput 까지 한다. 그래서 어느 실패든
 * filp 참조가 새지 않는다.
 *
 * 실행 컨텍스트: VFIO_DEVICE_FEATURE ioctl 처리 중, 프로세스 문맥.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature_mig_device_state → [vfio_ioct_mig_return_fd]
 *     → get_unused_fd_flags / copy_to_user / fd_install / put_unused_fd / fput
 */
/*
 * Convert the drivers's struct file into a FD number and return it to userspace
 */
static int vfio_ioct_mig_return_fd(struct file *filp, void __user *arg,
				   struct vfio_device_feature_mig_state *mig)
{
	/* [한국어] 실패 시 돌려줄 오류 코드. */
	int ret;
	/* [한국어] 예약할 fd 번호. */
	int fd;

	/* [한국어] 번호만 예약한다. 아직 파일을 매달지 않으므로 다른 스레드가 이 번호를
	 * 쓸 수 없다. O_CLOEXEC 는 exec 너머로 마이그레이션 데이터 fd 가 새 나가지
	 * 않게 하는 기본값이다. */
	fd = get_unused_fd_flags(O_CLOEXEC);
	/* [한국어] 예약 실패면(프로세스 fd 한도 초과 등) */
	if (fd < 0) {
		/* [한국어] 그 오류를 반환값으로 삼고 */
		ret = fd;
		/* [한국어] vendor 가 만든 file 을 놓는 경로로 간다. 여기서 놓지 않으면 사용자에게
		 * 전달되지도, 해제되지도 않은 채 영영 남는다. */
		goto out_fput;
	}

	/* [한국어] 응답 구조체에 번호를 담는다. */
	mig->data_fd = fd;
	/* [한국어] 사용자에게 전달한다. **설치보다 먼저 전달하는 순서가 핵심이다** —
	 * 설치를 먼저 하면 전달이 실패했을 때 사용자는 번호를 모르는데 fd 는
	 * 이미 살아 있어 회수할 방법이 없다. */
	if (copy_to_user(arg, mig, sizeof(*mig))) {
		/* [한국어] 전달 실패. */
		ret = -EFAULT;
		/* [한국어] 번호 예약을 취소하는 경로로 간다. */
		goto out_put_unused;
	}
	/* [한국어] 이제 실제로 파일을 매단다. 이 시점 이후 filp 참조의 소유권은 사용자에게
	 * 넘어가므로 fput 하지 않는다. */
	fd_install(fd, filp);
	return 0;

/* [한국어] 번호는 예약했지만 파일을 매달지 못한 경우. */
out_put_unused:
	/* [한국어] 예약을 취소한다. 그리고 아래로 흘러 파일도 놓는다. */
	put_unused_fd(fd);
/* [한국어] 번호조차 얻지 못한 경우가 합류하는 지점. */
out_fput:
	/* [한국어] vendor 가 만든 file 참조를 놓는다. 두 라벨이 이어져 있어 어느 실패든
	 * 참조가 새지 않는다. */
	fput(filp);
	return ret;
}

/* [한국어]
 * vfio_ioctl_device_feature_mig_device_state - 마이그레이션 상태를 읽거나 바꾸는 FEATURE 핸들러
 *
 * @device: 대상 디바이스.
 * @flags: 사용자가 준 VFIO_DEVICE_FEATURE_ 계열 플래그. GET 과 SET 둘 다 지원.
 * @arg: 사용자 공간의 데이터 영역 주소(vfio_device_feature 의 data 뒤).
 * @argsz: 그 데이터 영역의 크기(헤더를 뺀 값).
 * @return: 0 또는 음수 errno. -ENOTTY 는 이 디바이스가 마이그레이션 자체를
 *          지원하지 않는다는 뜻이며, 사용자는 그것으로 기능 유무를 판별한다.
 *
 * 왜 GET 과 SET 이 한 함수인가: 같은 구조체를 쓰고, SET 결과로도 같은 구조체를
 * 돌려줘야 하기 때문이다. SET 은 성공 시 데이터 전송용 fd 를 함께 준다.
 *
 * 동작 과정:
 *  1. mig_ops 가 없으면 -ENOTTY.
 *  2. vfio_check_feature 로 플래그와 크기를 검증. 반환 1 이 "진행하라" 이고,
 *     0(PROBE 만 했을 때)이나 음수는 그대로 사용자에게 돌려준다.
 *  3. 사용자 구조체를 minsz 만큼 읽어 온다.
 *  4. GET 이면 vendor 에게 현재 상태를 물어 mig.device_state 에 담고 out_copy 로.
 *  5. SET 이면 vendor 의 migration_set_state 를 부른다. 이것이 struct file
 *     포인터를 돌려주는데 세 가지 결과가 가능하다.
 *     - 유효한 file: 데이터 전송 fd 가 필요한 상태 전이(STOP_COPY, RESUMING 등).
 *       vfio_ioct_mig_return_fd 로 사용자에게 번호를 넘긴다.
 *     - NULL: 전이는 성공했지만 전송 fd 가 필요 없는 상태. out_copy 로 가서
 *       data_fd 에 -1 을 담아 준다.
 *     - ERR_PTR: 전이 실패. 역시 out_copy 로 가되 마지막에 PTR_ERR 를 반환한다.
 *       실패해도 구조체를 한 번 돌려주는 이유는 data_fd 를 -1 로 확실히
 *       만들어 사용자가 쓰레기 값을 fd 로 오해하지 않게 하기 위함이다.
 *
 * [상류 코드 관찰] 이 함수는 device->mig_ops 가 NULL 인지만 보고
 * migration_get_state 와 migration_set_state 를 조건 없이 호출한다. 그 두
 * 슬롯이 모두 채워졌는지 확인하는 코드는 VFIO 코어에는 없고 PCI 쪽인
 * drivers/vfio/pci/vfio_pci_core.c:2145~2148 에만 있다. 즉 PCI 가 아닌 vendor 가
 * mig_ops 를 반쪽만 채워 등록하면 여기서 NULL 을 호출하게 된다. 원본
 * (1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥. 런타임 PM 참조를 잡은 상태다.
 *
 * 호출 체인:
 *   vfio_device_fops_unl_ioctl → vfio_ioctl_device_feature
 *     → [vfio_ioctl_device_feature_mig_device_state]
 *     → mig_ops->migration_get_state / migration_set_state /
 *   vfio_ioct_mig_return_fd
 */
static int
vfio_ioctl_device_feature_mig_device_state(struct vfio_device *device,
					   u32 flags, void __user *arg,
					   size_t argsz)
{
	/* [한국어] data_fd 필드 끝까지가 이 요청의 필수 헤더 크기다. */
	size_t minsz =
		offsetofend(struct vfio_device_feature_mig_state, data_fd);
	/* [한국어] 커널 쪽 사본. GET 이면 여기에 현재 상태를, SET 이면 사용자가 준 목표
	 * 상태를 담는다. */
	struct vfio_device_feature_mig_state mig;
	/* [한국어] vendor 가 돌려줄 데이터 전송용 file. **NULL 로 초기화하는 것이 중요하다** —
	 * GET 경로는 이 변수를 건드리지 않은 채 out_copy 로 가는데, 거기서
	 * IS_ERR 로 검사하기 때문이다. NULL 은 IS_ERR 가 거짓이라 0 이 반환된다. */
	struct file *filp = NULL;
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] vendor 가 마이그레이션 vtable 을 제공하지 않으면 -ENOTTY. */
	if (!device->mig_ops)
		return -ENOTTY;

	/* [한국어] GET 과 SET 을 모두 허용한다. */
	ret = vfio_check_feature(flags, argsz,
				 /* [한국어] 쓰기 동작과 */
				 VFIO_DEVICE_FEATURE_SET |
				 /* [한국어] 읽기 동작 둘 다 이 기능에서 뜻이 있다. */
				 VFIO_DEVICE_FEATURE_GET,
				 /* [한국어] 최소 크기는 이 구조체 전체다. */
				 sizeof(mig));
	/* [한국어] 1 이 아니면 진행하지 않는다. */
	if (ret != 1)
		return ret;

	/* [한국어] 사용자 요청을 읽어 온다. SET 이면 여기에 목표 상태가 들어 있다. */
	if (copy_from_user(&mig, arg, minsz))
		return -EFAULT;

	/* [한국어] 읽기 요청 처리. */
	if (flags & VFIO_DEVICE_FEATURE_GET) {
		/* [한국어] vendor 가 채워 줄 현재 상태. */
		enum vfio_device_mig_state curr_state;

		/* [한국어] vendor 에게 현재 상태를 묻는다. */
		ret = device->mig_ops->migration_get_state(device,
							   &curr_state);
		/* [한국어] 실패면 그대로 올린다. 이 경로는 out_copy 를 지나지 않는데,
		 * filp 을 만든 적이 없어 정리할 것이 없기 때문이다. */
		if (ret)
			return ret;
		/* [한국어] 응답 구조체에 담고 */
		mig.device_state = curr_state;
		/* [한국어] 공통 복사 지점으로 간다. GET 에는 전송 fd 가 없으므로 거기서 -1 이 채워진다. */
		goto out_copy;
	}

	/* Handle the VFIO_DEVICE_FEATURE_SET */
	/* [한국어] 쓰기 요청 처리. vendor 가 상태 전이를 수행하고, 필요하면 데이터 전송용
	 * file 을 만들어 돌려준다. */
	filp = device->mig_ops->migration_set_state(device, mig.device_state);
	/* [한국어] 돌려받은 값은 세 가지일 수 있다. 오류 포인터이거나 NULL 이면
	 * 돌려줄 fd 가 없다는 뜻이라 공통 복사 지점으로 간다. */
	if (IS_ERR(filp) || !filp)
		goto out_copy;

	/* [한국어] 유효한 file 이면 fd 번호로 바꿔 사용자에게 넘긴다. 그 함수가
	 * filp 참조의 처리(설치 또는 fput)를 전담한다. */
	return vfio_ioct_mig_return_fd(filp, arg, &mig);
/* [한국어] GET 과, SET 중 fd 가 필요 없는 경우가 모이는 지점. */
out_copy:
	/* [한국어] 전송 fd 가 없음을 -1 로 알린다. 이렇게 확실히 채워 두지 않으면 사용자가
	 * 스택 잔재를 fd 번호로 오해할 수 있다. */
	mig.data_fd = -1;
	/* [한국어] 구조체를 사용자에게 돌려준다. 전이 실패였더라도 한 번 돌려주는 이유가
	 * 바로 이 -1 을 전달하기 위해서다. */
	if (copy_to_user(arg, &mig, sizeof(mig)))
		return -EFAULT;
	/* [한국어] 전이 자체가 실패했다면 */
	if (IS_ERR(filp))
		/* [한국어] 그 오류를 최종 반환값으로 쓴다. */
		return PTR_ERR(filp);
	/* [한국어] 그 밖에는 성공이다 — GET 이었거나, fd 가 필요 없는 전이였다. */
	return 0;
}

/* [한국어]
 * vfio_ioctl_device_feature_migration_data_size - STOP_COPY 단계에서 옮길 데이터 양을 알려 준다
 *
 * @device: 대상 디바이스.
 * @flags: FEATURE 플래그. GET 전용이다.
 * @arg: 사용자 공간 데이터 영역.
 * @argsz: 그 크기.
 * @return: 0 또는 음수 errno. -ENOTTY 는 마이그레이션 미지원.
 *
 * 왜 필요한가: 라이브 마이그레이션 관리자(QEMU 등)는 "지금 VM 을 멈추면
 * 다운타임이 얼마나 될까" 를 예측해야 한다. STOP_COPY 로 옮겨야 할 바이트 수가
 * 그 예측의 핵심 입력이다. PRE_COPY 로 미리 옮기다가 이 값이 충분히 작아지면
 * 그때 멈추는 것이 표준 전략이다.
 *
 * 동작 과정:
 *  1. mig_ops 확인.
 *  2. vfio_check_feature 로 GET 만 허용됨을 확인.
 *  3. vendor 에게 물어 stop_copy_length 를 받는다.
 *  4. 사용자 구조체에 담아 복사. 구조체를 지역에서 {} 로 0 초기화한 뒤 채우는
 *     이유는 **패딩과 미사용 필드로 커널 스택 내용이 새지 않게** 하기 위함이다.
 *
 * 지역 변수 타입이 unsigned long 인 이유: vendor 콜백 인터페이스가
 * unsigned long 을 쓰고 사용자 ABI 는 고정 폭 필드를 쓴다. 그래서 받은 값을
 * 구조체 필드에 대입하며 폭을 맞춘다.
 *
 * [상류 코드 관찰] 여기서도 mig_ops 만 확인하고 migration_get_data_size 슬롯의
 * 존재는 확인하지 않은 채 호출한다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature → [vfio_ioctl_device_feature_migration_data_size]
 *     → mig_ops->migration_get_data_size / copy_to_user
 */
static int
vfio_ioctl_device_feature_migration_data_size(struct vfio_device *device,
					      u32 flags, void __user *arg,
					      size_t argsz)
{
	/* [한국어] 응답 구조체를 0 초기화로 만든다. 미사용 필드로 커널 스택이 새지 않는다. */
	struct vfio_device_feature_mig_data_size data_size = {};
	/* [한국어] vendor 콜백이 값을 담아 줄 지역 변수. 콜백 인터페이스가 unsigned long 을
	 * 쓰고 사용자 ABI 는 고정 폭 필드를 써서 중간 변수가 필요하다. */
	unsigned long stop_copy_length;
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] vendor 가 마이그레이션 vtable 을 제공하지 않으면 -ENOTTY. */
	if (!device->mig_ops)
		return -ENOTTY;

	/* [한국어] GET 만 허용하며 최소 크기는 응답 구조체 전체다. */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_GET,
				 sizeof(data_size));
	/* [한국어] 1 이 아니면 진행하지 않는다. */
	if (ret != 1)
		return ret;

	/* [한국어] vendor 에게 STOP_COPY 단계에서 옮겨야 할 바이트 수를 묻는다.
	 * 관리자는 이 값으로 VM 정지 시간을 예측한다. */
	ret = device->mig_ops->migration_get_data_size(device, &stop_copy_length);
	/* [한국어] vendor 오류는 그대로 올린다. */
	if (ret)
		return ret;

	/* [한국어] 폭을 맞춰 응답 구조체에 담는다. */
	data_size.stop_copy_length = stop_copy_length;
	/* [한국어] 사용자에게 돌려준다. */
	if (copy_to_user(arg, &data_size, sizeof(data_size)))
		return -EFAULT;

	return 0;
}

/* [한국어]
 * vfio_ioctl_device_feature_migration_precopy_info_v2 - precopy 정보 보고의 v2 동작을 켠다
 *
 * @device: 대상 디바이스. 성공하면 device->precopy_info_v2 가 1 이 된다.
 * @flags: FEATURE 플래그. SET 전용.
 * @argsz: 데이터 영역 크기. 이 기능은 데이터가 없으므로 0 이 최소다.
 * @return: 0 성공, -EINVAL 이면 이 디바이스가 PRE_COPY 를 지원하지 않거나
 *          플래그/크기 검증에 걸린 것이다.
 *
 * 다른 FEATURE 핸들러와 다른 점: 인자에 arg 포인터가 아예 없다. 주고받을
 * 데이터가 없고 "켠다" 는 사실만 있는 opt-in 스위치이기 때문이다.
 *
 * 왜 opt-in 인가: uapi 헤더(include/uapi/linux/vfio.h:1279~1280)가 밝히듯,
 * precopy 정보 구조체의 flags 필드는 예전 버그와의 호환 때문에 원래 예약
 * 필드였다. 그것을 이제 와서 유효한 출력으로 바꾸면 옛 사용자 공간이 깨진다.
 * 그래서 새 동작을 원하는 사용자만 이 SET 을 호출해 켜게 했다.
 *
 * 플래그가 device 에 사는 이유와 그 결과: precopy_info_v2 는 fd 가 아니라
 * device 의 필드다. 그래서 사용자가 끄지 않고 fd 를 닫아도 남을 수 있는데,
 * vfio_df_device_last_close 가 마지막 close 에서 0 으로 되돌려 다음 사용자가
 * 이전 사용자의 설정을 물려받지 않게 한다.
 *
 * 동작 과정:
 *  1. migration_flags 에 PRE_COPY 가 없으면 -EINVAL. mig_ops 가 아니라
 *     플래그를 보는 이유는, 이 기능이 PRE_COPY 를 실제로 구현한 디바이스에만
 *     의미가 있기 때문이다.
 *  2. vfio_check_feature 로 SET 만 허용, 최소 데이터 크기 0.
 *  3. 플래그를 세운다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥. dev_set->lock 은 잡지 않는다 —
 * 이 필드는 자기 fd 의 사용자만 의미 있게 읽는다.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature
 *     → [vfio_ioctl_device_feature_migration_precopy_info_v2]
 *     → vfio_check_feature
 */
static int
vfio_ioctl_device_feature_migration_precopy_info_v2(struct vfio_device *device,
						    u32 flags, size_t argsz)
{
	/* [한국어] 검증 결과 코드. */
	int ret;

	/* [한국어] 이 디바이스가 PRE_COPY 를 실제로 구현했는지 본다. mig_ops 포인터가 아니라
	 * 기능 플래그를 보는 이유는, 이 옵트인이 PRE_COPY 단계가 있는 디바이스에만
	 * 의미가 있기 때문이다. */
	if (!(device->migration_flags & VFIO_MIGRATION_PRE_COPY))
		return -EINVAL;

	/* [한국어] SET 만 허용하고 최소 데이터 크기는 0 이다 — 주고받을 데이터가 없는
	 * 스위치라 arg 포인터조차 인자에 없다. */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET, 0);
	/* [한국어] 1 이 아니면 진행하지 않는다. PROBE 로 물어본 경우 0 이 그대로 나간다. */
	if (ret != 1)
		return ret;

	/* [한국어] 플래그를 켠다. 이 필드는 fd 가 아니라 device 에 살기 때문에,
	 * 마지막 close 때 vfio_df_device_last_close 가 0 으로 되돌려 다음 사용자가
	 * 이전 설정을 물려받지 않게 한다. */
	device->precopy_info_v2 = 1;
	return 0;
}

/* [한국어]
 * vfio_ioctl_device_feature_migration - 이 디바이스가 지원하는 마이그레이션 기능 비트를 보고한다
 *
 * @device: 대상 디바이스.
 * @flags: FEATURE 플래그. GET 전용.
 * @arg: 사용자 공간 데이터 영역.
 * @argsz: 그 크기.
 * @return: 0 또는 음수 errno. -ENOTTY 는 마이그레이션 미지원.
 *
 * 왜 필요한가: 사용자 공간이 "이 디바이스가 P2P 를 지원하나, PRE_COPY 를
 * 지원하나" 를 알아야 어떤 상태 전이를 요청할지 정할 수 있다. 그 답이
 * device->migration_flags 이며, 이 핸들러는 그것을 그대로 내보낸다.
 *
 * 지역 구조체를 지정 초기화로 채우는 이유: 나머지 필드가 0 으로 확실히
 * 채워져 커널 스택 잔재가 사용자에게 새지 않는다.
 *
 * 이 함수가 확인하는 것과 확인하지 않는 것: mig_ops 포인터가 NULL 인지만
 * 본다. 개별 콜백 슬롯은 보지 않지만, 이 함수는 어떤 콜백도 호출하지 않고
 * 필드 값만 복사하므로 그 자체로는 안전하다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature → [vfio_ioctl_device_feature_migration]
 *     → vfio_check_feature / copy_to_user
 */
static int vfio_ioctl_device_feature_migration(struct vfio_device *device,
					       u32 flags, void __user *arg,
					       size_t argsz)
{
	/* [한국어] 지역 구조체를 지정 초기화로 만든다. */
	struct vfio_device_feature_migration mig = {
		/* [한국어] 디바이스가 지원하는 마이그레이션 기능 비트를 그대로 담는다.
		 * 나머지 필드는 지정 초기화 규칙에 따라 0 이 되어, 커널 스택 잔재가
		 * 사용자에게 새지 않는다. */
		.flags = device->migration_flags,
	};
	/* [한국어] 검증 결과 코드. */
	int ret;

	/* [한국어] vendor 가 마이그레이션 vtable 을 제공하지 않으면 -ENOTTY.
	 * 사용자는 이 값으로 기능 유무를 판별한다. 이 함수는 어떤 콜백도 부르지
	 * 않고 필드 값만 복사하므로 개별 슬롯 검사가 필요 없다. */
	if (!device->mig_ops)
		return -ENOTTY;

	/* [한국어] GET 만 허용하며 최소 크기는 이 구조체 전체다. */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_GET,
				 sizeof(mig));
	/* [한국어] 1 이 아니면 진행하지 않는다. */
	if (ret != 1)
		return ret;
	/* [한국어] 지원 비트를 사용자에게 돌려준다. */
	if (copy_to_user(arg, &mig, sizeof(mig)))
		return -EFAULT;
	return 0;
}

/* [한국어]
 * vfio_combine_iova_ranges - IOVA 구간 트리를 원하는 개수 이하로 줄인다(가장 좁은 틈부터 병합)
 *
 * @root: interval tree 의 루트. 제자리에서 수정된다 — 병합된 노드는 트리에서
 *        빠지고, 남은 노드의 last 가 늘어난다.
 * @cur_nodes: 현재 노드 수. 호출자가 알고 있는 값이다.
 * @req_nodes: 줄이고 싶은 목표 개수.
 * @return: 없음.
 *
 * 왜 필요한가: 더티 페이지 추적 하드웨어는 감시할 수 있는 구간 개수에 상한이
 * 있다(mlx5 나 pds 의 트래커). 사용자가 준 구간이 그보다 많으면 몇 개를 합쳐야
 * 하는데, 합치면 원래 구간 사이의 틈까지 감시 대상에 들어가 **거짓 양성**
 * 더티 페이지가 생긴다. 그 손해를 최소화하려면 **틈이 가장 좁은 이웃끼리**
 * 합쳐야 한다. 이 함수가 그 탐욕 알고리즘이다.
 *
 * 두 경로:
 *  (1) req_nodes 가 1 인 특수 경로 — 전부를 하나로 합친다. 정렬 순회를 돌며
 *      마지막 노드의 last 를 기억해 두고, 첫 노드를 뺀 나머지를 트리에서
 *      제거한 다음, 첫 노드의 last 를 전체의 끝으로 늘린다. 첫 노드만은
 *      제거하지 않으므로 prev != comb_start 검사가 필요하다. 빈 트리면
 *      WARN 후 반환한다.
 *  (2) 일반 경로 — cur_nodes 가 req_nodes 를 넘는 동안 반복한다. 매 반복마다
 *      트리를 처음부터 훑어 인접한 두 노드의 틈(다음 시작 - 이전 끝)이 가장
 *      작은 쌍을 찾고, 앞 노드의 끝을 뒤 노드의 끝까지 늘린 뒤 뒤 노드를
 *      제거한다. 한 번에 하나씩만 합치므로 O(n^2) 이지만, n 이 페이지 하나에
 *      들어갈 만큼 작아(LOG_MAX_RANGES) 문제되지 않는다.
 *
 * interval tree 순회가 정렬 순서라는 점이 전제다. 그래서 "다음 시작 - 이전 끝"
 * 이 음수가 되지 않고 틈으로 해석된다.
 *
 * WARN_ON_ONCE(min_gap == ULONG_MAX) 의 뜻: 한 바퀴를 돌았는데 이웃 쌍을 하나도
 * 못 찾았다는 것이고, 이는 노드가 1개 이하인데도 루프에 들어왔다는 뜻이다.
 * 호출자가 준 cur_nodes 가 실제 트리와 어긋난 경우이므로 경고 후 빠져나온다.
 *
 * 실행 컨텍스트: vendor 의 log_start 구현 안, 프로세스 문맥. 트리 소유권은
 * 호출자에게 있고 이 함수는 락을 잡지 않는다 — 호출자가 배타적으로 다루는
 * 지역 트리여야 한다.
 *
 * 호출 체인:
 *   vendor 의 log_start(drivers/vfio/pci/mlx5/cmd.c:950,
 *   drivers/vfio/pci/pds/dirty.c:283) → [vfio_combine_iova_ranges]
 *     → interval_tree_iter_first / _next / interval_tree_remove
 */
void vfio_combine_iova_ranges(struct rb_root_cached *root, u32 cur_nodes,
			      u32 req_nodes)
{
	/* [한국어] 순회 커서 넷. prev 와 curr 는 이웃 쌍을 훑는 데,
	 * comb_start 와 comb_end 는 합칠 쌍을 기억하는 데 쓴다. */
	struct interval_tree_node *prev, *curr, *comb_start, *comb_end;
	/* [한국어] min_gap 은 지금까지 본 가장 좁은 틈, curr_gap 은 현재 쌍의 틈이다.
	 * unsigned long 인 이유는 IOVA 가 그 폭이기 때문이다. */
	unsigned long min_gap, curr_gap;

	/* Special shortcut when a single range is required */
	/* [한국어] 전부를 하나로 합치는 특수 경로. 일반 루프로도 되지만 O(n^2) 가 되므로
	 * 한 번의 순회로 끝내는 지름길을 따로 둔다. */
	if (req_nodes == 1) {
		/* [한국어] 순회 중 마지막으로 본 노드의 끝 주소를 기억한다. */
		unsigned long last;

		/* [한국어] 정렬 순서의 첫 노드. 전 범위를 질의해 트리의 처음을 얻는다. */
		comb_start = interval_tree_iter_first(root, 0, ULONG_MAX);

		/* Empty list */
		/* [한국어] 빈 트리면 합칠 것이 없다. 호출자가 개수를 잘못 알고 부른 것이므로 경고한다. */
		if (WARN_ON_ONCE(!comb_start))
			return;

		/* [한국어] 첫 노드부터 훑기 시작한다. */
		curr = comb_start;
		/* [한국어] 트리 끝까지. */
		while (curr) {
			/* [한국어] 이 노드의 끝을 기억해 둔다. 루프가 끝나면 이 값이 **마지막 노드의 끝**,
			 * 즉 전체 범위의 끝이 된다. */
			last = curr->last;
			/* [한국어] 지금 노드를 prev 로 옮겨 두고 */
			prev = curr;
			/* [한국어] 다음 노드로 나아간다. **제거하기 전에 다음을 미리 얻는 것**이 핵심이다 —
			 * 제거한 노드로는 다음을 찾을 수 없다. */
			curr = interval_tree_iter_next(curr, 0, ULONG_MAX);
			/* [한국어] 첫 노드만은 남겨야 한다. 그것이 합쳐진 결과를 담을 그릇이기 때문이다. */
			if (prev != comb_start)
				/* [한국어] 나머지는 트리에서 뺀다. 노드 메모리 자체는 호출자 소유라 해제하지 않는다. */
				interval_tree_remove(prev, root);
		}
		/* [한국어] 첫 노드의 끝을 전체의 끝으로 늘린다. 이제 트리에는 [첫 시작, 마지막 끝]
		 * 구간 하나만 남는다. */
		comb_start->last = last;
		return;
	}

	/* Combine ranges which have the smallest gap */
	/* [한국어] 노드 수가 목표보다 많은 동안 한 번에 하나씩 합친다. */
	while (cur_nodes > req_nodes) {
		/* [한국어] 매 반복마다 이웃 쌍 탐색을 처음부터 다시 한다. 앞 반복에서 트리가
		 * 바뀌었으므로 이전 결과를 재사용할 수 없다. */
		prev = NULL;
		/* [한국어] 가장 좁은 틈을 찾기 위해 최댓값에서 시작한다. 이 값이 그대로 남으면
		 * 이웃 쌍을 하나도 못 찾았다는 뜻이 된다. */
		min_gap = ULONG_MAX;
		/* [한국어] 정렬 순서의 첫 노드부터. */
		curr = interval_tree_iter_first(root, 0, ULONG_MAX);
		/* [한국어] 트리 끝까지 훑는다. */
		while (curr) {
			/* [한국어] 첫 노드에는 앞 이웃이 없으므로 두 번째부터 쌍이 생긴다. */
			if (prev) {
				/* [한국어] 틈 = 이번 노드의 시작 - 앞 노드의 끝. 순회가 정렬 순서라 음수가 되지
				 * 않는다. 이 틈만큼이 합쳤을 때 새로 감시 대상이 되는(거짓 양성이 생기는)
				 * 범위다. */
				curr_gap = curr->start - prev->last;
				/* [한국어] 지금까지 본 것보다 좁으면 */
				if (curr_gap < min_gap) {
					/* [한국어] 최소값을 갱신하고 */
					min_gap = curr_gap;
					/* [한국어] 합칠 앞 노드와 */
					comb_start = prev;
					/* [한국어] 뒤 노드를 기억한다. */
					comb_end = curr;
				}
			}
			/* [한국어] 커서를 한 칸 옮긴다. */
			prev = curr;
			/* [한국어] 다음 노드로. */
			curr = interval_tree_iter_next(curr, 0, ULONG_MAX);
		}

		/* Empty list or no nodes to combine */
		/* [한국어] 한 바퀴 돌았는데 이웃 쌍이 없었다는 것은 노드가 1개 이하인데도 루프에
		 * 들어왔다는 뜻이다. 호출자가 준 cur_nodes 가 실제 트리와 어긋난 경우다. */
		if (WARN_ON_ONCE(min_gap == ULONG_MAX))
			/* [한국어] 더 합칠 수 없으므로 루프를 벗어난다. 무한 루프 방지이기도 하다. */
			break;

		/* [한국어] 앞 노드의 끝을 뒤 노드의 끝까지 늘려 둘을 하나로 만든다.
		 * 사이의 틈까지 포함되며, 그 틈이 가장 좁은 쌍이라 손해가 최소다. */
		comb_start->last = comb_end->last;
		/* [한국어] 흡수된 뒤 노드를 트리에서 뺀다. */
		interval_tree_remove(comb_end, root);
		/* [한국어] 노드 수를 하나 줄이고 다시 판정한다. */
		cur_nodes--;
	}
}
/* [한국어] EXPORT_SYMBOL_GPL — vendor 트래커 드라이버들이 쓴다. */
EXPORT_SYMBOL_GPL(vfio_combine_iova_ranges);

/* Ranges should fit into a single kernel page */
/* [한국어] 감시 구간 개수의 상한. 바로 위 상류 주석이 밝히듯 커널 페이지 하나에
 * 들어가는 개수로 정한다. 사용자가 개수를 정하는 값이므로 상한이 없으면
 * 임의 크기의 커널 할당을 요구할 수 있다. */
#define LOG_MAX_RANGES \
	/* [한국어] 한 페이지를 구간 구조체 하나의 크기로 나눈 값. 구조체 크기가 커지면
	 * 상한이 자동으로 줄어 한 페이지 규칙이 유지된다. */
	(PAGE_SIZE / sizeof(struct vfio_device_feature_dma_logging_range))

/* [한국어]
 * vfio_ioctl_device_feature_logging_start - 사용자가 준 IOVA 구간들에 대해 DMA 더티 추적을 켠다
 *
 * @device: 대상 디바이스. device->log_ops 가 있어야 한다.
 * @flags: FEATURE 플래그. SET 전용.
 * @arg: 사용자 공간의 vfio_device_feature_dma_logging_control 주소.
 * @argsz: 데이터 영역 크기.
 * @return: 0 성공. -ENOTTY(추적 미지원), -EINVAL(구간 수 0, 정렬 위반, 겹침),
 *          -E2BIG(구간이 너무 많음), -EOVERFLOW(주소 덧셈 넘침),
 *          -ENOMEM, -EFAULT 중 하나.
 *
 * 왜 필요한가: 라이브 마이그레이션 중에는 디바이스가 DMA 로 고친 게스트
 * 페이지를 추적해야 한다. 그래야 이미 옮긴 페이지 중 다시 옮길 것을 고를 수
 * 있다. 사용자는 감시할 IOVA 구간 목록과 페이지 크기를 주고, 하드웨어가
 * 그 범위를 감시하기 시작한다.
 *
 * 입력 검증이 이 함수의 절반인 이유: 여기 들어오는 값은 전부 사용자 공간에서
 * 온다. 검증 항목을 하나씩 보면 각각 막는 공격이 다르다.
 *  - nnodes 가 0 이면 의미 없는 요청이라 -EINVAL.
 *  - nnodes 상한 LOG_MAX_RANGES 는 커널 할당을 한 페이지로 묶는다. 없으면
 *    사용자가 임의 크기의 커널 메모리를 요구할 수 있다.
 *  - iova 와 length 의 page_size 정렬 — 하드웨어가 페이지 단위로만 추적한다.
 *  - check_add_overflow 로 iova + length 넘침 확인, 그리고 그 결과가
 *    ULONG_MAX 를 넘지 않는지도 본다. 사용자 ABI 는 64비트인데 커널 내부
 *    interval tree 는 unsigned long 이라, 32비트 커널에서 폭이 다르다.
 *  - interval_tree_iter_first 로 이미 넣은 구간과 겹치는지 확인. 겹치면
 *    추적 결과 해석이 모호해지므로 거절한다.
 *
 * 동작 과정:
 *  1. log_ops 확인 후 vfio_check_feature.
 *  2. 제어 구조체를 minsz 만큼 읽는다. ranges 는 사용자 포인터라
 *     u64_to_user_ptr 로 변환한다 — 32비트 커널에서 64비트 사용자 값을
 *     포인터로 좁히는 표준 방법이다.
 *  3. 노드 배열을 한 번에 할당하고, 구간을 하나씩 복사하며 검증해 트리에 넣는다.
 *     구간을 [start, last] 폐구간으로 바꾸느라 length - 1 을 더한다.
 *  4. vendor 의 log_start 를 호출한다. page_size 는 **참조로** 넘긴다 —
 *     하드웨어가 요청한 크기를 못 맞추면 실제 사용할 크기로 고쳐 쓰기 때문이다.
 *  5. 고쳐진 제어 구조체를 사용자에게 되돌려 준다. 이 복사가 실패하면
 *     사용자는 실제 page_size 를 모른 채 추적이 켜진 상태가 되므로,
 *     **그 자리에서 log_stop 으로 되돌린다**. 이 되감기가 이 함수에서 가장
 *     놓치기 쉬운 부분이다.
 *  6. end 라벨에서 노드 배열을 해제한다. 트리 노드는 이 배열 안을 가리키므로
 *     vendor 는 log_start 안에서 필요한 정보를 자기 자료구조로 옮겨야 한다.
 *
 * [상류 코드 관찰] 5번의 되감기는 log_ops->log_stop 을 존재 확인 없이
 * 호출한다. log_start 와 log_stop 이 둘 다 있는지 보는 검사는 VFIO 코어가
 * 아니라 drivers/vfio/pci/vfio_pci_core.c:2151~2153 에만 있다. 원본
 * (1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥. GFP_KERNEL 할당과 사용자 메모리
 * 접근이 있어 잠들 수 있다.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature → [vfio_ioctl_device_feature_logging_start]
 *     → copy_from_user / interval_tree_insert / log_ops->log_start /
 *   copy_to_user / log_ops->log_stop / kfree
 */
static int
vfio_ioctl_device_feature_logging_start(struct vfio_device *device,
					u32 flags, void __user *arg,
					size_t argsz)
{
	/* [한국어] ranges 필드 끝까지가 이 요청의 필수 헤더 크기다. */
	size_t minsz =
		offsetofend(struct vfio_device_feature_dma_logging_control,
			    ranges);
	/* [한국어] 사용자 공간에 있는 구간 배열의 주소. __user 표식이 붙어 있어 직접
	 * 역참조하면 sparse 가 잡아낸다. */
	struct vfio_device_feature_dma_logging_range __user *ranges;
	/* [한국어] 제어 구조체의 커널 쪽 사본. 구간 개수와 페이지 크기가 들어 있다. */
	struct vfio_device_feature_dma_logging_control control;
	/* [한국어] 구간 하나를 임시로 받는 변수. 배열 전체를 커널로 복사하지 않고
	 * 하나씩 읽어 검증하는 방식이다. */
	struct vfio_device_feature_dma_logging_range range;
	/* [한국어] 검증을 통과한 구간을 담을 정렬 트리. 지역 변수라 이 함수 안에서만
	 * 존재하며, vendor 가 log_start 안에서 필요한 정보를 자기 자료구조로 옮긴다. */
	struct rb_root_cached root = RB_ROOT_CACHED;
	/* [한국어] 트리 노드들을 담을 배열. 한 번에 할당해 조각 할당을 피한다. */
	struct interval_tree_node *nodes;
	/* [한국어] 구간 끝 주소 계산 결과. */
	u64 iova_end;
	/* [한국어] 구간 개수. 사용자 값이라 아래에서 상한 검사를 받는다. */
	u32 nnodes;
	/* [한국어] 루프 인덱스와 결과 코드. */
	int i, ret;

	/* [한국어] vendor 가 로깅 vtable 을 제공하지 않으면 -ENOTTY. */
	if (!device->log_ops)
		return -ENOTTY;

	/* [한국어] SET 만 허용하며 최소 크기는 제어 구조체 전체다. */
	ret = vfio_check_feature(flags, argsz,
				 VFIO_DEVICE_FEATURE_SET,
				 sizeof(control));
	/* [한국어] 1 이 아니면 진행하지 않는다. */
	if (ret != 1)
		return ret;

	/* [한국어] 제어 구조체를 읽어 온다. */
	if (copy_from_user(&control, arg, minsz))
		return -EFAULT;

	/* [한국어] 구간 개수를 꺼낸다. */
	nnodes = control.num_ranges;
	/* [한국어] 0 개는 의미 없는 요청이다. */
	if (!nnodes)
		return -EINVAL;

	/* [한국어] 상한을 넘으면 -E2BIG. 이 상한이 아래 할당을 한 페이지 안으로 묶는다. */
	if (nnodes > LOG_MAX_RANGES)
		return -E2BIG;

	/* [한국어] 64비트 사용자 값을 __user 포인터로 변환한다. */
	ranges = u64_to_user_ptr(control.ranges);
	/* [한국어] 노드 배열을 한 번에 할당한다. 상한 덕분에 최대 한 페이지다. */
	nodes = kmalloc_objs(struct interval_tree_node, nnodes);
	/* [한국어] 할당 실패면 -ENOMEM. 아직 트리에 아무것도 넣지 않았으므로 정리할 것이 없다. */
	if (!nodes)
		return -ENOMEM;

	/* [한국어] 구간을 하나씩 읽어 검증하고 트리에 넣는다. */
	for (i = 0; i < nnodes; i++) {
		/* [한국어] 배열의 i번째 구간을 커널로 복사한다. */
		if (copy_from_user(&range, &ranges[i], sizeof(range))) {
			/* [한국어] 복사 실패면 -EFAULT 로 end 라벨에 가서 배열만 해제한다. */
			ret = -EFAULT;
			goto end;
		}
		/* [한국어] 시작 주소와 */
		if (!IS_ALIGNED(range.iova, control.page_size) ||
		    /* [한국어] 길이가 모두 사용자가 지정한 페이지 크기로 정렬돼야 한다.
		     * 하드웨어 트래커가 페이지 단위로만 감시하기 때문이다. */
		    !IS_ALIGNED(range.length, control.page_size)) {
			ret = -EINVAL;
			goto end;
		}

		/* [한국어] 구간 끝을 계산하며 넘침을 확인하고, */
		if (check_add_overflow(range.iova, range.length, &iova_end) ||
		    /* [한국어] 그 결과가 커널 내부 폭을 넘지 않는지도 본다. */
		    iova_end > ULONG_MAX) {
			ret = -EOVERFLOW;
			goto end;
		}

		/* [한국어] 폐구간의 시작. */
		nodes[i].start = range.iova;
		/* [한국어] 폐구간의 끝. length - 1 을 더하는 것은 [start, last] 표현으로 바꾸기
		 * 위함이며, 앞의 넘침 검사를 통과한 뒤라 안전하다. */
		nodes[i].last = range.iova + range.length - 1;
		/* [한국어] 지금까지 넣은 구간과 겹치는지 본다. 겹치면 추적 결과 해석이 모호해진다. */
		if (interval_tree_iter_first(&root, nodes[i].start,
					     nodes[i].last)) {
			/* Range overlapping */
			ret = -EINVAL;
			goto end;
		}
		/* [한국어] 겹치지 않으므로 트리에 넣는다. 포인터 산술 nodes + i 는 배열의 i번째
		 * 원소 주소이며, 트리 노드가 배열 안에 산다는 뜻이다. */
		interval_tree_insert(nodes + i, &root);
	}

	/* [한국어] vendor 에게 추적 시작을 맡긴다. 트리와 개수를 함께 넘긴다. */
	ret = device->log_ops->log_start(device, &root, nnodes,
					 /* [한국어] 페이지 크기는 **참조로** 넘긴다 — 하드웨어가 요청한 크기를 못 맞추면
					  * 실제 사용할 크기로 고쳐 쓰기 때문이다. */
					 &control.page_size);
	/* [한국어] 실패면 end 로 가서 배열만 해제한다. 추적이 켜지지 않았으므로 되돌릴
	 * 것이 없다. */
	if (ret)
		goto end;

	/* [한국어] 고쳐진 제어 구조체를 사용자에게 되돌려 준다. */
	if (copy_to_user(arg, &control, sizeof(control))) {
		/* [한국어] 복사 실패면 사용자는 실제 페이지 크기를 모른 채 추적만 켜진 상태가 된다. */
		ret = -EFAULT;
		/* [한국어] 그래서 그 자리에서 되돌린다. 이 되감기가 이 함수에서 가장 놓치기 쉬운
		 * 부분이다. 다만 log_stop 슬롯의 존재는 확인하지 않는다(함수 블록의
		 * 상류 코드 관찰 참조). */
		device->log_ops->log_stop(device);
	}

/* [한국어] 성공 경로도 이 라벨을 지난다. */
end:
	/* [한국어] 노드 배열 해제. vendor 는 log_start 안에서 필요한 정보를 이미 옮겨 갔다. */
	kfree(nodes);
	return ret;
}

/* [한국어]
 * vfio_ioctl_device_feature_logging_stop - DMA 더티 추적을 끈다
 *
 * @device: 대상 디바이스.
 * @flags: FEATURE 플래그. SET 전용.
 * @arg: 데이터 영역 주소. **이 함수는 쓰지 않는다** — 끄는 데는 인자가 없다.
 *       시그니처를 다른 FEATURE 핸들러와 맞추려고 남겨 둔 인자다.
 * @argsz: 데이터 영역 크기. 최소 0 으로 검증한다.
 * @return: vendor 의 log_stop 반환값 그대로, 또는 -ENOTTY / 검증 실패값.
 *
 * 무엇의 짝인가: vfio_ioctl_device_feature_logging_start 다. 사용자가 명시적으로
 * 부르지 않고 fd 를 닫아도 vendor 쪽 close_device 에서 정리되는 것이 보통이지만,
 * 마이그레이션을 취소하는 경우처럼 추적만 끄고 디바이스는 계속 쓰는 시나리오가
 * 있어 별도 명령이 필요하다.
 *
 * 동작 과정: log_ops 확인 → vfio_check_feature(SET, 최소 크기 0) →
 * vendor 의 log_stop 호출. 반환값을 가공 없이 그대로 올린다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature → [vfio_ioctl_device_feature_logging_stop]
 *     → vfio_check_feature / log_ops->log_stop
 */
static int
vfio_ioctl_device_feature_logging_stop(struct vfio_device *device,
				       u32 flags, void __user *arg,
				       size_t argsz)
{
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] vendor 가 로깅 vtable 을 제공하지 않으면 -ENOTTY. */
	if (!device->log_ops)
		return -ENOTTY;

	/* [한국어] SET 만 허용하고 최소 데이터 크기는 0 이다 — 끄는 데는 인자가 필요 없다. */
	ret = vfio_check_feature(flags, argsz,
				 VFIO_DEVICE_FEATURE_SET, 0);
	/* [한국어] 1 이 아니면 진행하지 않는다. */
	if (ret != 1)
		return ret;

	/* [한국어] vendor 의 중지 콜백을 부르고 결과를 가공 없이 그대로 올린다. */
	return device->log_ops->log_stop(device);
}

/* [한국어]
 * vfio_device_log_read_and_clear - iova_bitmap 순회기가 조각마다 부르는 콜백
 *
 * @iter: iova_bitmap 순회 상태. vendor 는 이것을 통해 더티 비트를 세운다.
 * @iova: 이번 조각의 시작 IOVA.
 * @length: 이번 조각의 길이.
 * @opaque: 순회 시작 시 넘긴 불투명 값. 여기서는 struct vfio_device 포인터다.
 * @return: vendor 의 log_read_and_clear 반환값. 0 이 아니면 순회가 중단된다.
 *
 * 왜 이런 얇은 껍데기가 필요한가: iova_bitmap 순회기는 자기 콜백 시그니처를
 * 정해 두고 있고(첫 인자가 iter, 마지막이 opaque), vendor 의 log_ops 는
 * 다른 시그니처(첫 인자가 device, 마지막이 iter)를 쓴다. 이 함수는 두
 * 규약 사이의 **인자 순서 어댑터**다. 로직은 없다.
 *
 * 왜 조각으로 나눠 부르는가: 사용자가 준 비트맵이 매우 클 수 있어 한 번에
 * 전부 커널로 핀할 수 없다. iova_bitmap 이 한 번에 다룰 수 있는 만큼씩 잘라
 * 가며 이 콜백을 반복 호출한다. 그래서 vendor 는 자기가 받은 iova/length 만
 * 처리하면 된다.
 *
 * 실행 컨텍스트: ioctl 처리 중, 프로세스 문맥. 사용자 비트맵 페이지가 핀된
 * 상태에서 불린다.
 *
 * [상류 코드 관찰] log_ops 포인터의 NULL 검사는 호출자
 * (vfio_ioctl_device_feature_logging_report)가 하지만, log_read_and_clear
 * 슬롯 자체의 존재는 여기서도 호출자에서도 확인하지 않는다. 그 검사는
 * drivers/vfio/pci/vfio_pci_core.c:2151~2153 에만 있다. 원본(1f0e418bb6)에서
 * 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature_logging_report → iova_bitmap_for_each
 *     → [vfio_device_log_read_and_clear] → log_ops->log_read_and_clear
 */
static int vfio_device_log_read_and_clear(struct iova_bitmap *iter,
					  unsigned long iova, size_t length,
					  void *opaque)
{
	struct vfio_device *device = opaque;

	/* [한국어] 인자 순서만 바꿔 vendor 에게 넘긴다. iova_bitmap 순회기의 콜백 규약은
	 * 첫 인자가 iter, 마지막이 opaque 인 반면 vendor 의 log_ops 는 첫 인자가
	 * device, 마지막이 iter 다. 이 한 줄이 두 규약 사이의 어댑터 전부다. */
	return device->log_ops->log_read_and_clear(device, iova, length, iter);
}

/* [한국어]
 * vfio_ioctl_device_feature_logging_report - 추적된 더티 페이지 비트맵을 사용자에게 넘기고 지운다
 *
 * @device: 대상 디바이스.
 * @flags: FEATURE 플래그. GET 전용.
 * @arg: 사용자 공간의 vfio_device_feature_dma_logging_report 주소.
 * @argsz: 데이터 영역 크기.
 * @return: 0 또는 음수 errno. -ENOTTY, -EINVAL(페이지 크기 위반),
 *          -EOVERFLOW, -EFAULT, 그리고 vendor 오류.
 *
 * "read and clear" 인 이유: 비트를 읽어 사용자에게 준 다음 곧바로 지운다.
 * 그래야 다음 호출이 "지난번 이후 새로 더러워진 페이지" 만 보고한다.
 * 읽기만 하고 지우지 않으면 매 라운드가 같은 페이지를 계속 다시 옮기게 된다.
 *
 * 입력 검증:
 *  - page_size 가 4KB 미만이거나 2의 거듭제곱이 아니면 -EINVAL. 비트맵 한
 *    비트가 한 페이지에 대응하므로 크기가 정칙이어야 한다.
 *  - iova + length 넘침과 ULONG_MAX 초과를 막는다. logging_start 와 같은 이유다.
 *
 * 동작 과정:
 *  1. log_ops 확인, vfio_check_feature(GET), 사용자 구조체 읽기.
 *  2. 위 검증.
 *  3. iova_bitmap_alloc 으로 순회기를 만든다. 사용자 비트맵 포인터를 그대로
 *     넘기며, 순회기가 그 사용자 페이지를 조각조각 핀해 가며 접근한다.
 *     커널이 비트맵 전체를 복사해 두지 않아 큰 VM 에서도 메모리를 아낀다.
 *  4. iova_bitmap_for_each 로 조각마다 vfio_device_log_read_and_clear 를
 *     부른다. device 포인터가 opaque 인자로 전달된다.
 *  5. 순회기를 해제한다. 성공이든 실패든 반드시 해제해야 핀한 페이지가 풀린다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥. 사용자 페이지를 핀하므로 잠들 수
 * 있고, 시그널로 중단될 수 있는 경로를 포함한다.
 *
 * 호출 체인:
 *   vfio_ioctl_device_feature → [vfio_ioctl_device_feature_logging_report]
 *     → iova_bitmap_alloc / iova_bitmap_for_each → vfio_device_log_read_and_clear
 *     → iova_bitmap_free
 */
static int
vfio_ioctl_device_feature_logging_report(struct vfio_device *device,
					 u32 flags, void __user *arg,
					 size_t argsz)
{
	/* [한국어] bitmap 필드 끝까지가 이 요청의 필수 헤더 크기다. */
	size_t minsz =
		offsetofend(struct vfio_device_feature_dma_logging_report,
			    bitmap);
	/* [한국어] 커널 쪽 사본. */
	struct vfio_device_feature_dma_logging_report report;
	/* [한국어] 사용자 비트맵을 조각조각 다루는 순회기 핸들. */
	struct iova_bitmap *iter;
	/* [한국어] iova + length 의 넘침 검사 결과를 받을 변수. */
	u64 iova_end;
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] vendor 가 로깅 vtable 을 제공하지 않으면 이 기능 자체가 없다. */
	if (!device->log_ops)
		return -ENOTTY;

	/* [한국어] GET 만 허용하며 최소 크기는 이 구조체 전체다. 반환 1 이 진행 신호이고
	 * 0(PROBE)이나 음수는 그대로 사용자에게 돌려준다. */
	ret = vfio_check_feature(flags, argsz,
				 VFIO_DEVICE_FEATURE_GET,
				 sizeof(report));
	/* [한국어] 1 이 아니면 진행하지 않는다. */
	if (ret != 1)
		return ret;

	/* [한국어] 사용자 요청을 읽어 온다. */
	if (copy_from_user(&report, arg, minsz))
		return -EFAULT;

	/* [한국어] 비트맵 한 비트가 한 페이지에 대응하므로 페이지 크기가 정칙이어야 한다.
	 * 4KB 미만이거나 2의 거듭제곱이 아니면 비트와 주소의 대응이 깨진다. */
	if (report.page_size < SZ_4K || !is_power_of_2(report.page_size))
		return -EINVAL;

	/* [한국어] iova + length 넘침을 더하기 전에 막고, 그 결과가 커널 내부 폭
	 * (unsigned long)을 넘지 않는지도 확인한다. 32비트 커널에서 사용자 ABI 의
	 * 64비트 값이 좁아지기 때문이다. */
	if (check_add_overflow(report.iova, report.length, &iova_end) ||
	    iova_end > ULONG_MAX)
		return -EOVERFLOW;

	/* [한국어] 순회기를 만든다. 사용자 비트맵 포인터를 그대로 넘기며, 순회기가 그
	 * 사용자 페이지를 조각조각 핀해 가며 접근한다. 커널이 비트맵 전체를 복사해
	 * 두지 않아 큰 VM 에서도 메모리를 아낀다. */
	iter = iova_bitmap_alloc(report.iova, report.length,
				 report.page_size,
				 /* [한국어] 64비트 사용자 값을 __user 포인터로 변환한다. */
				 u64_to_user_ptr(report.bitmap));
	/* [한국어] 생성 실패는 ERR_PTR 로 온다. */
	if (IS_ERR(iter))
		return PTR_ERR(iter);

	/* [한국어] 조각마다 콜백을 부른다. device 포인터가 opaque 인자로 전달되고,
	 * 콜백이 그것을 vendor 의 log_read_and_clear 로 넘긴다. */
	ret = iova_bitmap_for_each(iter, device,
				   /* [한국어] 인자 순서 어댑터 역할의 정적 콜백. */
				   vfio_device_log_read_and_clear);

	/* [한국어] 성공이든 실패든 반드시 해제해야 핀한 사용자 페이지가 풀린다. */
	iova_bitmap_free(iter);
	/* [한국어] 순회 결과를 그대로 사용자에게 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_ioctl_device_feature - VFIO_DEVICE_FEATURE ioctl 의 디스패처
 *
 * @device: 대상 디바이스.
 * @arg: 사용자 공간의 struct vfio_device_feature 주소. 헤더(argsz, flags)와
 *       그 뒤에 붙는 가변 데이터로 이루어진다.
 * @return: 각 기능 핸들러의 반환값. -EFAULT, -EINVAL, -ENOTTY 가 여기서 직접
 *          나올 수도 있다.
 *
 * 왜 이런 확장 가능한 ioctl 이 있는가: ioctl 번호를 새 기능마다 하나씩
 * 소비하면 금방 고갈되고 32/64비트 호환 처리도 늘어난다. VFIO_DEVICE_FEATURE
 * 는 번호 하나 안에 16비트 기능 인덱스를 두어 그 문제를 피한다.
 * 플래그의 상위 비트가 동작을 정한다: GET(읽기), SET(쓰기),
 * PROBE(지원 여부만 확인, 부작용 없음).
 *
 * 검증 순서와 이유:
 *  1. 헤더를 minsz(= flags 끝까지)만 읽는다. argsz 를 알기 전에는 그 뒤를
 *     얼마나 읽어야 할지 모른다.
 *  2. argsz 가 헤더보다 작으면 -EINVAL. 사용자가 자기 구조체 크기를 잘못
 *     보고한 것이다.
 *  3. 정의되지 않은 플래그 비트가 켜져 있으면 -EINVAL. 이 검사가 **미래
 *     확장의 안전장치**다. 지금 무시하면 나중에 그 비트에 의미를 부여할 때
 *     옛 프로그램의 쓰레기 값이 새 기능을 켜 버린다.
 *  4. PROBE 없이 GET 과 SET 을 동시에 켜면 -EINVAL. PROBE 와 함께라면
 *     "읽기도 쓰기도 되는지 물어보는 것" 이라 뜻이 통한다.
 *
 * 디스패치: 하위 16비트를 기능 인덱스로 꺼내 switch 한다. 코어가 직접 아는
 * 7개(마이그레이션 4종, DMA 로깅 3종)를 처리하고, 나머지는 vendor 의
 * device_feature 콜백으로 넘긴다. vendor 콜백이 없으면 -ENOTTY 다.
 * 각 핸들러에 넘기는 크기가 feature.argsz - minsz 인 이유는, 핸들러는 헤더
 * 뒤의 **데이터 부분 크기만** 알면 되기 때문이다. arg->data 로 데이터 시작
 * 주소를 함께 넘긴다.
 *
 * unlikely 를 쓰는 자리: vendor 콜백 부재 검사에만 붙어 있다. 대부분의 vendor
 * 가 콜백을 제공하므로 분기 예측 힌트로 정상 경로를 곧게 편다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥. 런타임 PM 참조 보유 상태.
 *
 * 호출 체인:
 *   vfio_device_fops_unl_ioctl → [vfio_ioctl_device_feature]
 *     → vfio_ioctl_device_feature_migration / _mig_device_state /
 *   _logging_start / _logging_stop / _logging_report /
 *   _migration_data_size / _migration_precopy_info_v2 /
 *   device->ops->device_feature
 */
static int vfio_ioctl_device_feature(struct vfio_device *device,
				     struct vfio_device_feature __user *arg)
{
	/* [한국어] flags 필드 끝까지가 필수 헤더다. 그 뒤 data 는 기능마다 크기가 다르다. */
	size_t minsz = offsetofend(struct vfio_device_feature, flags);
	/* [한국어] 헤더의 커널 쪽 사본. */
	struct vfio_device_feature feature;

	/* [한국어] 헤더만 먼저 읽는다. argsz 를 알기 전에는 그 뒤를 얼마나 읽어야 할지
	 * 모르기 때문에 두 단계로 나눈다. */
	if (copy_from_user(&feature, arg, minsz))
		return -EFAULT;

	/* [한국어] 사용자가 보고한 전체 크기가 헤더보다 작으면 구조체가 성립하지 않는다. */
	if (feature.argsz < minsz)
		return -EINVAL;

	/* Check unknown flags */
	/* [한국어] 정의된 네 가지(기능 인덱스 마스크, SET, GET, PROBE) 밖의 비트가 켜져
	 * 있으면 거절한다. */
	if (feature.flags &
	    /* [한국어] 하위 16비트는 기능 인덱스이고 그 위 세 비트가 동작을 정한다. */
	    ~(VFIO_DEVICE_FEATURE_MASK | VFIO_DEVICE_FEATURE_SET |
	      /* [한국어] 이 검사가 미래 확장의 안전장치다. 지금 무시하면 나중에 그 비트에 의미가
	       * 생겼을 때 옛 프로그램의 쓰레기 값이 새 기능을 켠다. */
	      VFIO_DEVICE_FEATURE_GET | VFIO_DEVICE_FEATURE_PROBE))
		return -EINVAL;

	/* GET & SET are mutually exclusive except with PROBE */
	/* [한국어] PROBE 없이 */
	if (!(feature.flags & VFIO_DEVICE_FEATURE_PROBE) &&
	    /* [한국어] SET 과 */
	    (feature.flags & VFIO_DEVICE_FEATURE_SET) &&
	    /* [한국어] GET 을 동시에 켠 조합은 뜻이 모순이라 거절한다. PROBE 와 함께라면
	     * "읽기도 쓰기도 되는지 물어보는 것" 이라 성립한다. */
	    (feature.flags & VFIO_DEVICE_FEATURE_GET))
		return -EINVAL;

	/* [한국어] 하위 16비트를 기능 인덱스로 꺼내 분기한다. */
	switch (feature.flags & VFIO_DEVICE_FEATURE_MASK) {
	/* [한국어] 이 디바이스가 지원하는 마이그레이션 기능 비트 조회. */
	case VFIO_DEVICE_FEATURE_MIGRATION:
		/* [한국어] 각 핸들러에 넘기는 것은 플래그, 데이터 시작 주소, 그리고 */
		return vfio_ioctl_device_feature_migration(
			device, feature.flags, arg->data,
			/* [한국어] 헤더를 뺀 **데이터 부분 크기**다. 핸들러는 헤더를 다시 볼 필요가 없다. */
			feature.argsz - minsz);
	/* [한국어] 마이그레이션 상태 읽기와 쓰기. */
	case VFIO_DEVICE_FEATURE_MIG_DEVICE_STATE:
		/* [한국어] SET 이면 데이터 전송용 fd 를 함께 돌려주는 유일한 기능이다. */
		return vfio_ioctl_device_feature_mig_device_state(
			device, feature.flags, arg->data,
			feature.argsz - minsz);
	/* [한국어] DMA 더티 추적 시작. */
	case VFIO_DEVICE_FEATURE_DMA_LOGGING_START:
		/* [한국어] 구간 목록을 받아 검증하고 vendor 하드웨어 트래커를 켠다. */
		return vfio_ioctl_device_feature_logging_start(
			device, feature.flags, arg->data,
			feature.argsz - minsz);
	/* [한국어] DMA 더티 추적 중지. */
	case VFIO_DEVICE_FEATURE_DMA_LOGGING_STOP:
		/* [한국어] 데이터가 없는 명령이라 arg 를 쓰지 않는다. */
		return vfio_ioctl_device_feature_logging_stop(
			device, feature.flags, arg->data,
			feature.argsz - minsz);
	/* [한국어] 더티 비트맵 보고와 비우기. */
	case VFIO_DEVICE_FEATURE_DMA_LOGGING_REPORT:
		/* [한국어] 사용자 비트맵을 조각조각 핀해 가며 vendor 가 채우게 한다. */
		return vfio_ioctl_device_feature_logging_report(
			device, feature.flags, arg->data,
			feature.argsz - minsz);
	/* [한국어] STOP_COPY 단계에서 옮길 데이터 양 조회. */
	case VFIO_DEVICE_FEATURE_MIG_DATA_SIZE:
		/* [한국어] 관리자가 다운타임을 예측하는 데 쓴다. */
		return vfio_ioctl_device_feature_migration_data_size(
			device, feature.flags, arg->data,
			feature.argsz - minsz);
	/* [한국어] precopy 정보 v2 동작 켜기. */
	case VFIO_DEVICE_FEATURE_MIG_PRECOPY_INFOv2:
		/* [한국어] 이 핸들러만 arg 포인터를 받지 않는다 — 주고받을 데이터가 없는
		 * opt-in 스위치이기 때문이다. */
		return vfio_ioctl_device_feature_migration_precopy_info_v2(
			device, feature.flags, feature.argsz - minsz);
	/* [한국어] 코어가 모르는 기능 인덱스는 vendor 에게 넘긴다. vendor 가 자기만의
	 * 기능(예: PCI hot reset, dmabuf 내보내기)을 여기에 붙인다. */
	default:
		/* [한국어] vendor 가 기능 콜백을 제공하지 않으면 */
		if (unlikely(!device->ops->device_feature))
			/* [한국어] -ENOTTY. 사용자는 이 값으로 "그런 기능은 없다" 를 판별한다. */
			return -ENOTTY;
		/* [한국어] vendor 위임. 인자 구성은 코어 핸들러들과 같다. */
		return device->ops->device_feature(device, feature.flags,
						   arg->data,
						   feature.argsz - minsz);
	}
}

/* [한국어]
 * vfio_get_region_info - VFIO_DEVICE_GET_REGION_INFO ioctl 처리. BAR 등 영역 정보를 보고한다
 *
 * @device: 대상 디바이스.
 * @arg: 사용자 공간의 struct vfio_region_info 주소. 사용자는 index 와 argsz 를
 *       채워 보내고, 커널이 flags/size/offset(그리고 선택적 capability chain)을
 *       채워 돌려준다.
 * @return: 0 성공. -EINVAL(vendor 미지원 또는 argsz 부족), -EFAULT(복사 실패),
 *          또는 vendor 오류.
 *
 * 왜 이 ioctl 이 VFIO 사용의 관문인가: 사용자 공간은 이 호출로 "BAR0 는 device
 * fd 의 어느 오프셋에 있고 크기가 얼마인가" 를 알아낸다. 그 offset 을 mmap 의
 * 오프셋으로 넘겨야 컨트롤러 레지스터에 닿는다. NVMe 를 사용자 공간에서 모는
 * 경우, 여기서 얻은 주소가 곧 CAP/CC/AQA 와 SQ/CQ 도어벨이다.
 *
 * 두 단계 크기 협상(capability chain 이 있을 때):
 * 영역에 따라 고정 구조체만으로 부족한 정보가 있다(예: MSI-X 테이블을 피해
 * mmap 해야 하는 sparse mmap 영역). 그것을 가변 길이 capability chain 으로
 * 덧붙이는데, 사용자는 그 길이를 미리 모른다. 그래서 규약이 이렇다.
 *  - 1차 호출: 사용자가 작은 argsz 로 부른다. 커널은 CAPS 플래그를 켜고
 *    argsz 에 **필요한 총 크기**를 써서 돌려주며 cap_offset 은 0 으로 둔다.
 *  - 2차 호출: 사용자가 그 크기로 버퍼를 키워 다시 부른다. 이번에는 커널이
 *    고정 구조체 바로 뒤(arg + 1 은 struct 하나 뒤를 뜻한다)에 chain 을
 *    복사하고 cap_offset 에 그 오프셋을 적는다.
 * chain 안의 next 오프셋은 vendor 가 조립할 때 버퍼 기준이므로, 사용자 구조체
 * 기준으로 바꾸려면 vfio_info_cap_shift 로 전부 밀어 줘야 한다.
 *
 * 동작 과정:
 *  1. vendor 의 get_region_info_caps 콜백이 없으면 -EINVAL. 이 콜백은 vfio-pci
 *     계열이 vfio_pci.c:135 등에서 등록한다.
 *  2. 사용자 구조체를 minsz 만큼 읽고 argsz 하한을 확인한다.
 *  3. vendor 를 호출해 info 를 채우고 caps 를 조립하게 한다.
 *  4. caps 가 있으면 위 협상 로직.
 *  5. info 를 사용자에게 되돌린다. minsz 는 이 구조체의 마지막 필드(offset)
 *     끝까지이므로 구조체 전체와 같다.
 *  6. out_free 에서 caps.buf 를 해제한다. **성공 경로도 이 라벨을 지난다** —
 *     chain 은 이미 사용자 버퍼로 복사됐으므로 커널 사본은 필요 없다.
 *
 * 실행 컨텍스트: ioctl 처리, 프로세스 문맥. 런타임 PM 참조 보유 상태.
 *
 * 호출 체인:
 *   vfio_device_fops_unl_ioctl → [vfio_get_region_info]
 *     → device->ops->get_region_info_caps → vfio_info_cap_shift /
 *   copy_to_user / kfree
 */
static long vfio_get_region_info(struct vfio_device *device,
				 struct vfio_region_info __user *arg)
{
	/* [한국어] 이 구조체의 마지막 필드 offset 끝까지의 크기. vfio_region_info 는
	 * argsz, flags, index, cap_offset, size, offset 순으로 끝나므로
	 * 이 값이 곧 구조체 전체 크기다(include/uapi/linux/vfio.h:272~283). */
	unsigned long minsz = offsetofend(struct vfio_region_info, offset);
	/* [한국어] 커널 쪽 사본. {} 로 0 초기화해 패딩과 미사용 필드로 커널 스택 내용이
	 * 사용자에게 새지 않게 한다. */
	struct vfio_region_info info = {};
	/* [한국어] vendor 가 조립할 capability chain 을 담을 그릇. 역시 0 초기화라
	 * buf 가 NULL, size 가 0 에서 시작한다. */
	struct vfio_info_cap caps = {};
	/* [한국어] 결과 코드. out_free 라벨을 성공 경로도 지나므로 반드시 초기화되는
	 * 경로여야 하는데, vendor 콜백이 항상 값을 채워 준다. */
	int ret;

	/* [한국어] vendor 가 영역 정보 콜백을 제공하지 않으면 이 ioctl 을 처리할 수 없다.
	 * 등록하는 곳은 drivers/vfio/pci/vfio_pci.c:135 등이다. */
	if (unlikely(!device->ops->get_region_info_caps))
		return -EINVAL;

	/* [한국어] 사용자 요청을 읽어 온다. index 와 argsz 는 사용자가 채워 보낸다. */
	if (copy_from_user(&info, arg, minsz))
		return -EFAULT;
	/* [한국어] 사용자가 보고한 크기가 구조체보다 작으면 아래에서 되돌려 줄 자리가 없다. */
	if (info.argsz < minsz)
		return -EINVAL;

	/* [한국어] vendor 가 info 의 flags, size, offset 을 채우고 필요하면 caps 에
	 * capability chain 을 조립한다. offset 이 곧 mmap 에 넘길 파일 오프셋이다. */
	ret = device->ops->get_region_info_caps(device, &info, &caps);
	/* [한국어] 실패해도 vendor 가 caps 를 일부 조립했을 수 있으므로 out_free 로 가서
	 * 버퍼를 해제한다. 곧바로 return 하면 그만큼 누수가 된다. */
	if (ret)
		goto out_free;

	/* [한국어] chain 이 있을 때만 크기 협상을 한다. */
	if (caps.size) {
		/* [한국어] 사용자에게 "이 응답에는 capability 가 붙어 있다" 를 알리는 플래그. */
		info.flags |= VFIO_REGION_INFO_FLAG_CAPS;
		/* [한국어] 사용자 버퍼가 고정 구조체와 chain 을 함께 담기에 모자라면 */
		if (info.argsz < sizeof(info) + caps.size) {
			/* [한국어] 필요한 총 크기를 argsz 에 적어 알려 준다. 사용자는 이 값을 보고 버퍼를
			 * 키워 다시 호출한다. */
			info.argsz = sizeof(info) + caps.size;
			/* [한국어] 이번 응답에는 chain 을 담지 못했으므로 오프셋을 0 으로 둔다. */
			info.cap_offset = 0;
		} else {
			/* [한국어] 버퍼가 충분하면 chain 을 실을 준비를 한다. chain 안의 next 오프셋은
			 * vendor 가 조립할 때 버퍼 시작 기준이므로, 사용자 구조체 기준으로 바꾸려면
			 * 고정 구조체 크기만큼 전부 밀어야 한다. */
			vfio_info_cap_shift(&caps, sizeof(info));
			/* [한국어] arg + 1 은 구조체 포인터 산술이라 **구조체 하나 뒤**, 즉 고정 구조체
			 * 바로 다음 바이트를 가리킨다. 그 자리에 chain 을 통째로 복사한다. */
			if (copy_to_user(arg + 1, caps.buf, caps.size)) {
				/* [한국어] 복사 실패면 -EFAULT. */
				ret = -EFAULT;
				goto out_free;
			}
			/* [한국어] chain 이 고정 구조체 바로 뒤에 있음을 사용자에게 알린다.
			 * 위에서 민 오프셋들과 이 값이 같은 기준을 쓴다. */
			info.cap_offset = sizeof(info);
		}
	}

	/* [한국어] 고정 구조체를 사용자에게 되돌려 준다. minsz 가 구조체 전체 크기와 같으므로
	 * 모든 필드가 전달된다. */
	if (copy_to_user(arg, &info, minsz)){
		ret = -EFAULT;
		goto out_free;
	}

/* [한국어] 성공 경로도 이 라벨을 지난다. */
out_free:
	/* [한국어] chain 은 이미 사용자 버퍼로 복사됐거나(성공) 전달되지 못했으므로(실패)
	 * 커널 사본은 어느 쪽이든 필요 없다. caps.buf 가 NULL 이어도 kfree 는 안전하다. */
	kfree(caps.buf);
	return ret;
}

/* [한국어]
 * vfio_device_fops_unl_ioctl - device fd 의 ioctl 진입점이자 이 파일의 중심 디스패처
 *
 * @filep: device fd. private_data 가 vfio_device_file 이다.
 * @cmd: ioctl 번호.
 * @arg: 사용자 인자(대개 포인터를 정수로 실은 값).
 * @return: 성공 시 0 이상, 실패 시 음수 errno.
 *
 * 이 함수의 4단 구조가 곧 VFIO 의 보안 모델이다.
 *
 *  [1단] BIND_IOMMUFD 는 무조건 통과시킨다.
 *    cdev 경로로 갓 열린 fd 는 아무 권한이 없고, BIND_IOMMUFD 로 iommufd
 *    컨텍스트에 묶여야 비로소 권한이 생긴다. 그 명령 자체를 권한 검사 뒤에
 *    두면 영원히 권한을 얻을 수 없으므로 검사보다 앞에 둔다. 대신 그 안에서
 *    device_cdev.c 가 자체 검사(이미 bind 됐는가, group fd 인가, 토큰이
 *    맞는가)를 한다.
 *
 *  [2단] 권한 게이트.
 *    df->access_granted 를 smp_load_acquire 로 읽는다. 이 acquire 는
 *    group.c:220 과 device_cdev.c:157 의 smp_store_release 와 짝이다.
 *    release 앞에서 이뤄진 모든 준비(iommufd 바인딩, vendor open_device,
 *    devid 전달)가 acquire 를 통과한 이 CPU 에서 반드시 보이게 만든다.
 *    단순 읽기였다면 다른 CPU 에서 "플래그는 1 인데 df->iommufd 는 아직
 *    NULL 로 보이는" 창이 생겨, 그 사이 ioctl 이 준비되지 않은 상태를 밟는다.
 *    플래그가 0 이면 -EINVAL. 권한 없음을 -EPERM 이 아니라 -EINVAL 로 주는
 *    것은 "이 fd 상태에서는 이 명령이 성립하지 않는다" 는 뜻에 가깝다.
 *
 *  [3단] 런타임 PM 확보.
 *    여기서 잡고 out 라벨에서 반드시 놓는다. 아래 모든 분기가 goto out 또는
 *    break 로 이 라벨을 지나도록 짜여 있다. **1단과 2단만이 이 라벨을 건너뛰는데,
 *    그 둘은 PM 참조를 잡기 전이라 균형이 맞는다.**
 *
 *  [4단] 명령 분기.
 *    (a) cdev 전용 두 명령 — ATTACH/DETACH_IOMMUFD_PT. df->group 이 NULL 일
 *        때만 유효하다. group 경로에서는 IOAS 결합을 container 가 관리하므로
 *        이 명령이 의미가 없다. CONFIG_VFIO_DEVICE_CDEV 가 꺼져 있으면
 *        IS_ENABLED 가 컴파일 시 거짓이 되어 이 블록이 통째로 사라진다.
 *    (b) 코어가 직접 처리하는 두 명령 — VFIO_DEVICE_FEATURE 와
 *        GET_REGION_INFO.
 *    (c) 나머지는 vendor 의 ops->ioctl 로. 콜백이 없으면 -EINVAL.
 *        SET_IRQS, GET_IRQ_INFO, RESET, GET_INFO 등 대부분이 여기로 간다.
 *
 * 실행 컨텍스트: ioctl(2), 프로세스 문맥. 잠들 수 있다. 같은 fd 에 대해
 * 여러 스레드가 동시에 들어올 수 있으며, 이 함수는 자체 직렬화를 하지 않는다 —
 * 필요한 락은 각 하위 핸들러와 vendor 가 잡는다.
 *
 * 에러 경로: 2단 실패는 PM 을 잡기 전이라 즉시 반환. 3단 실패도 즉시 반환
 * (pm_runtime_resume_and_get 이 실패 시 스스로 되돌린다). 4단의 모든 결과는
 * out 을 지나 PM 을 놓고 나간다.
 *
 * 호출 체인:
 *   ioctl(2) → VFS → vfio_device_fops 의 .unlocked_ioctl
 *     → [vfio_device_fops_unl_ioctl] → vfio_df_ioctl_bind_iommufd /
 *   vfio_df_ioctl_attach_pt / vfio_df_ioctl_detach_pt /
 *   vfio_ioctl_device_feature / vfio_get_region_info / device->ops->ioctl
 */
static long vfio_device_fops_unl_ioctl(struct file *filep,
				       unsigned int cmd, unsigned long arg)
{
	/* [한국어] fd 컨텍스트. 이 파일의 모든 fops 훅이 여기서 시작한다. */
	struct vfio_device_file *df = filep->private_data;
	/* [한국어] 그 fd 가 가리키는 디바이스. */
	struct vfio_device *device = df->device;
	/* [한국어] 정수로 실려 온 사용자 포인터를 __user 포인터로 되돌린다. 커널이 사용자
	 * 주소를 다룰 때 반드시 이 표식을 붙여야 sparse 정적 검사가 잘못된 역참조를
	 * 잡아낸다. */
	void __user *uptr = (void __user *)arg;
	/* [한국어] 각 분기의 결과를 모아 out 라벨에서 한 번에 반환하기 위한 변수. */
	int ret;

	/* [한국어] BIND_IOMMUFD 만은 권한 게이트보다 **앞**에 둔다. cdev 로 갓 열린 fd 는
	 * 권한이 없는데, 권한을 얻는 수단이 바로 이 명령이기 때문이다.
	 * 게이트 뒤에 두면 영원히 권한을 얻을 수 없다. */
	if (cmd == VFIO_DEVICE_BIND_IOMMUFD)
		/* [한국어] device_cdev.c:83 이 자체 검사(이미 bind 됐는가, group fd 인가, 토큰이
		 * 맞는가)를 하고 vfio_df_open 까지 부른다. 런타임 PM 을 잡기 전이라
		 * 아래 out 라벨을 지나지 않고 곧바로 반환한다. */
		return vfio_df_ioctl_bind_iommufd(df, uptr);

	/* Paired with smp_store_release() following vfio_df_open() */
	/* [한국어] 권한 게이트. acquire 로 읽어야 하는 이유는 이 플래그를 세운 CPU 가
	 * 그전에 한 모든 준비(iommufd 바인딩, vendor open_device 완료, devid 전달)를
	 * 이 CPU 에서도 보이게 하기 위함이다. 단순 읽기였다면 플래그만 1 로 보이고
	 * 준비 결과는 아직 안 보이는 창이 생긴다. */
	if (!smp_load_acquire(&df->access_granted))
		/* [한국어] 권한이 없으면 -EINVAL. 사용자에게 "이 fd 상태에서는 이 명령이 성립하지
		 * 않는다" 는 뜻이다. */
		return -EINVAL;

	/* [한국어] 여기부터 아래 out 라벨까지 디바이스를 D0 에 붙잡아 둔다. 레지스터 접근이
	 * 일어날 수 있는 구간 전체를 덮는다. */
	ret = vfio_device_pm_runtime_get(device);
	/* [한국어] resume 실패면 ioctl 을 진행하지 않는다. 이 실패 경로는 잡은 참조가 없어
	 * out 라벨을 지나지 않고 곧바로 반환한다 — 짝이 맞는다. */
	if (ret)
		return ret;

	/* cdev only ioctls */
	/* [한국어] cdev 전용 명령 블록. CONFIG_VFIO_DEVICE_CDEV 가 꺼져 있으면 IS_ENABLED 가
	 * 컴파일 시 거짓이 되어 블록 전체가 사라진다. df->group 이 NULL 이어야
	 * cdev 경로인데, group 경로에서는 IOAS 결합을 container 가 관리하므로
	 * 이 명령들이 의미가 없다. */
	if (IS_ENABLED(CONFIG_VFIO_DEVICE_CDEV) && !df->group) {
		/* [한국어] 두 명령만 여기서 가로챈다. */
		switch (cmd) {
		/* [한국어] 이 fd 를 iommufd 의 IOAS 또는 HWPT 에 붙인다. 이 시점부터 디바이스의
		 * DMA 가 사용자가 만든 주소공간으로 번역된다. */
		case VFIO_DEVICE_ATTACH_IOMMUFD_PT:
			/* [한국어] 구현은 device_cdev.c:194. */
			ret = vfio_df_ioctl_attach_pt(df, uptr);
			/* [한국어] out 으로 가서 런타임 PM 을 놓는다. 아래 switch 로 흘러가면 안 되므로
			 * break 가 아니라 goto 다. */
			goto out;

		/* [한국어] 위의 반대 — 결합을 끊는다. */
		case VFIO_DEVICE_DETACH_IOMMUFD_PT:
			/* [한국어] 구현은 device_cdev.c:253. */
			ret = vfio_df_ioctl_detach_pt(df, uptr);
			/* [한국어] 역시 out 으로. */
			goto out;
		}
	}

	/* [한국어] 두 세대 공통 명령 분기. */
	switch (cmd) {
	/* [한국어] 확장 가능한 기능 ioctl. 마이그레이션과 DMA 로깅이 모두 이 하나로 들어온다. */
	case VFIO_DEVICE_FEATURE:
		/* [한국어] 이 파일의 디스패처가 다시 기능별로 나눈다. */
		ret = vfio_ioctl_device_feature(device, uptr);
		break;

	/* [한국어] 영역 정보 조회. 사용자가 BAR 를 mmap 하기 전에 반드시 거치는 관문이다. */
	case VFIO_DEVICE_GET_REGION_INFO:
		/* [한국어] 이 파일의 핸들러가 vendor 콜백을 부르고 capability chain 을 조립한다. */
		ret = vfio_get_region_info(device, uptr);
		break;

	/* [한국어] 나머지 모든 명령 — SET_IRQS, GET_IRQ_INFO, GET_INFO, RESET 등. */
	default:
		/* [한국어] vendor 가 ioctl 콜백을 제공하지 않으면 처리할 방법이 없다. */
		if (unlikely(!device->ops->ioctl))
			/* [한국어] 그 경우 -EINVAL. break 로 out 을 지나므로 런타임 PM 은 정상적으로 놓인다. */
			ret = -EINVAL;
		else
			/* [한국어] vendor 에게 위임한다. 여기서는 uptr 이 아니라 원래 arg 를 그대로 넘긴다 —
			 * vendor 마다 인자를 정수로 쓰는 명령도 있어 해석을 vendor 에게 맡긴다. */
			ret = device->ops->ioctl(device, cmd, arg);
		break;
	}
/* [한국어] 모든 분기가 모이는 정리 라벨. */
out:
	/* [한국어] 런타임 PM 참조를 놓는다. 위 get 과 정확히 짝이며, 조건식이 같은 함수
	 * 쌍이라 한쪽만 실행되는 일이 없다. */
	vfio_device_pm_runtime_put(device);
	/* [한국어] 각 분기가 담아 둔 결과를 사용자에게 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_device_fops_read - device fd 에 대한 read(2) 를 vendor 로 넘긴다
 *
 * @filep: device fd.
 * @buf: 사용자 버퍼.
 * @count: 요청 바이트 수.
 * @ppos: 파일 오프셋 포인터. VFIO 에서 이 오프셋은 **영역 번호와 영역 내 오프셋을
 *        인코딩한 값**이다. GET_REGION_INFO 가 알려 준 offset 을 그대로 쓴다.
 * @return: 읽은 바이트 수, 또는 음수 errno.
 *
 * 왜 mmap 이 아니라 read 도 필요한가: 모든 영역을 mmap 할 수 있는 것은 아니다.
 * PCI config space 는 커널이 일부 필드를 걸러야 해서 mmap 을 허용하지 않고,
 * MSI-X 테이블 페이지도 마찬가지다. 그런 영역은 read/write 로만 접근된다.
 * 반대로 NVMe 도어벨처럼 성능이 중요한 영역은 mmap 으로 노출된다.
 *
 * 동작 과정: 권한 게이트(smp_load_acquire) → vendor 콜백 존재 확인 → 위임.
 * 이 함수 자체에는 로직이 없다. 게이트가 여기에도 있어야 하는 이유는,
 * ioctl 을 막아도 read 로 레지스터를 읽을 수 있으면 게이트가 무의미하기
 * 때문이다. 네 개의 fops 훅(ioctl, read, write, mmap)이 **모두 같은 게이트를
 * 지난다**.
 *
 * 실행 컨텍스트: read(2), 프로세스 문맥. 런타임 PM 참조를 잡지 않는다는 점이
 * ioctl 경로와 다르다.
 *
 * 에러 경로: 게이트 미통과 또는 콜백 부재면 -EINVAL.
 *
 * 호출 체인:
 *   read(2) → VFS → vfio_device_fops 의 .read → [vfio_device_fops_read]
 *     → device->ops->read
 */
static ssize_t vfio_device_fops_read(struct file *filep, char __user *buf,
				     size_t count, loff_t *ppos)
{
	/* [한국어] fd 컨텍스트를 꺼낸다. */
	struct vfio_device_file *df = filep->private_data;
	/* [한국어] 그 fd 가 가리키는 디바이스. */
	struct vfio_device *device = df->device;

	/* Paired with smp_store_release() following vfio_df_open() */
	/* [한국어] 권한 게이트. ioctl 을 막아도 read 로 레지스터를 읽을 수 있으면 게이트가
	 * 무의미해지므로 여기에도 있어야 한다. */
	if (!smp_load_acquire(&df->access_granted))
		return -EINVAL;

	/* [한국어] vendor 가 read 를 제공하지 않으면 -EINVAL. unlikely 는 대부분의 vendor 가
	 * 제공한다는 힌트다. */
	if (unlikely(!device->ops->read))
		return -EINVAL;

	/* [한국어] vendor 에게 위임한다. ppos 는 영역 번호와 영역 내 오프셋을 인코딩한 값이며,
	 * 사용자는 GET_REGION_INFO 가 알려 준 offset 을 그대로 쓴다. */
	return device->ops->read(device, buf, count, ppos);
}

/* [한국어]
 * vfio_device_fops_write - device fd 에 대한 write(2) 를 vendor 로 넘긴다
 *
 * @filep: device fd.
 * @buf: 사용자 버퍼(const).
 * @count: 요청 바이트 수.
 * @ppos: 영역 번호와 영역 내 오프셋을 인코딩한 파일 오프셋.
 * @return: 쓴 바이트 수, 또는 음수 errno.
 *
 * vfio_device_fops_read 의 쓰기 방향 쌍둥이다. 구조가 똑같다 —
 * 권한 게이트, 콜백 존재 확인, 위임.
 *
 * 쓰기 쪽이 더 위험한 이유: 사용자가 여기로 PCI config space 를 쓰면 BAR 재배치,
 * 버스 마스터 비활성화 같은 시스템 전역 영향이 가능하다. 그래서 vendor 구현
 * (vfio-pci 의 config space 에뮬레이션)이 필드별로 걸러 낸다. 이 파일은 그
 * 필터링에 관여하지 않고 통로만 제공한다.
 *
 * 실행 컨텍스트: write(2), 프로세스 문맥.
 *
 * 호출 체인:
 *   write(2) → VFS → vfio_device_fops 의 .write → [vfio_device_fops_write]
 *     → device->ops->write
 */
static ssize_t vfio_device_fops_write(struct file *filep,
				      const char __user *buf,
				      size_t count, loff_t *ppos)
{
	/* [한국어] fd 컨텍스트를 꺼낸다. */
	struct vfio_device_file *df = filep->private_data;
	/* [한국어] 그 fd 가 가리키는 디바이스. */
	struct vfio_device *device = df->device;

	/* Paired with smp_store_release() following vfio_df_open() */
	/* [한국어] 권한 게이트. 쓰기 쪽이 더 위험한데, 여기로 PCI config space 를 쓰면
	 * BAR 재배치나 버스 마스터 비활성화 같은 시스템 전역 영향이 가능하다. */
	if (!smp_load_acquire(&df->access_granted))
		return -EINVAL;

	/* [한국어] vendor 가 write 를 제공하지 않으면 -EINVAL. */
	if (unlikely(!device->ops->write))
		return -EINVAL;

	/* [한국어] vendor 에게 위임한다. 어떤 필드를 걸러 낼지는 vendor(vfio-pci 의 config
	 * space 에뮬레이션)가 정한다. 이 파일은 통로만 제공한다. */
	return device->ops->write(device, buf, count, ppos);
}

/* [한국어]
 * vfio_device_fops_mmap - device fd 의 영역을 사용자 주소공간에 직접 매핑한다
 *
 * @filep: device fd.
 * @vma: 커널이 준비한 가상 메모리 영역. vma->vm_pgoff 가 어느 영역인지를
 *       담고 있고, vendor 가 그것을 해석해 실제 물리 주소를 remap 한다.
 * @return: 0 성공, 음수 errno 실패.
 *
 * 왜 이 훅이 VFIO 성능의 전부인가: 여기서 매핑이 성립하면 그 뒤로 사용자
 * 공간은 **시스템 콜 없이** 디바이스 레지스터를 읽고 쓴다. NVMe 로 치면
 * SQ 도어벨에 값을 쓰는 것이 그냥 메모리 쓰기 한 줄이 된다. 이 파일의
 * read/write/ioctl 이 I/O 마다 불리지 않는 이유가 이것이다.
 *
 * f_mapping 과의 관계: 이 fd 의 f_mapping 은 열릴 때 디바이스 전용 익명 inode
 * 의 address_space 로 바뀌어 있다(device_cdev.c:46 부근). 그래서 같은 디바이스에
 * 대한 모든 mmap 이 한 주소공간 트리에 모이고, 디바이스를 리셋하거나 떼어낼
 * 때 그 트리 하나만 비우면 모든 사용자 매핑이 한 번에 무효화된다. 그 익명
 * inode 를 만드는 곳이 이 파일의 vfio_fs_inode_new 다.
 *
 * 동작 과정: 권한 게이트 → 콜백 존재 확인 → vendor 위임. 실제 매핑 방식
 * (어떤 영역이 mmap 가능한지, MSI-X 테이블 페이지를 어떻게 가릴지)은 전적으로
 * vendor 의 몫이다.
 *
 * 실행 컨텍스트: mmap(2), 프로세스 문맥. mmap_lock 을 커널이 잡은 상태로
 * 불린다.
 *
 * 호출 체인:
 *   mmap(2) → VFS → vfio_device_fops 의 .mmap → [vfio_device_fops_mmap]
 *     → device->ops->mmap
 */
static int vfio_device_fops_mmap(struct file *filep, struct vm_area_struct *vma)
{
	/* [한국어] fd 컨텍스트를 꺼낸다. */
	struct vfio_device_file *df = filep->private_data;
	/* [한국어] 그 fd 가 가리키는 디바이스. */
	struct vfio_device *device = df->device;

	/* Paired with smp_store_release() following vfio_df_open() */
	/* [한국어] 권한 게이트. ioctl 과 같은 acquire 이며, 이것이 없으면 사용자가 ioctl 을
	 * 건너뛰고 곧장 mmap 으로 레지스터를 노출시킬 수 있다.
	 * 네 개의 fops 훅이 모두 같은 게이트를 지나는 이유다. */
	if (!smp_load_acquire(&df->access_granted))
		return -EINVAL;

	/* [한국어] vendor 가 mmap 을 제공하지 않는 디바이스도 있다. 그때는 read/write 로만
	 * 접근한다. */
	if (unlikely(!device->ops->mmap))
		return -EINVAL;

	/* [한국어] vendor 에게 위임한다. 어떤 영역이 mmap 가능한지, MSI-X 테이블 페이지를
	 * 어떻게 가릴지는 전적으로 vendor 의 판단이다.
	 * 이 fd 의 f_mapping 은 열릴 때 디바이스 전용 익명 inode 의 address_space 로
	 * 바뀌어 있어(device_cdev.c:46), 같은 디바이스의 모든 매핑이 한 트리에 모인다. */
	return device->ops->mmap(device, vma);
}

/* [한국어] procfs 가 컴파일에서 빠지면 이 훅을 등록할 곳 자체가 없으므로
 * 함수와 아래 fops 항목이 함께 사라진다. */
#ifdef CONFIG_PROC_FS
/* [한국어]
 * vfio_device_show_fdinfo - /proc/<pid>/fdinfo/<fd> 에 이 fd 가 어느 디바이스인지 적는다
 *
 * @m: seq_file 출력 버퍼.
 * @filep: 대상 device fd.
 * @return: 없음.
 *
 * 왜 필요한가: VFIO fd 만 보고는 어느 하드웨어인지 알 수 없다. 이 훅이 있으면
 * 관리자가 /proc 만으로 "이 프로세스가 잡은 fd 가 어느 PCI 함수인가" 를 알 수
 * 있다. 컨테이너나 VM 을 운영할 때 디바이스 할당을 감사하는 데 쓰인다.
 *
 * 동작 과정:
 *  1. kobject_get_path 로 하부 디바이스의 sysfs 경로를 얻는다. 이 함수는
 *     새 문자열을 할당해 돌려주므로 반드시 해제해야 한다.
 *  2. 할당 실패면 조용히 반환. fdinfo 는 진단용이라 실패해도 시스템에 영향이
 *     없어 오류를 전파하지 않는다.
 *  3. 앞에 /sys 를 붙여 출력한다. kobject_get_path 는 sysfs 마운트 지점을
 *     포함하지 않은 상대 경로를 주기 때문이다.
 *  4. 해제.
 *
 * CONFIG_PROC_FS 로 감싼 이유: procfs 가 없는 커널에서는 이 훅을 등록할 곳
 * 자체가 없다. 함수와 fops 항목이 함께 사라진다.
 *
 * 실행 컨텍스트: /proc 읽기, 프로세스 문맥. GFP_KERNEL 을 쓰므로 잠들 수 있다.
 *
 * 호출 체인:
 *   cat /proc/<pid>/fdinfo/<fd> → procfs → vfio_device_fops 의 .show_fdinfo
 *     → [vfio_device_show_fdinfo] → kobject_get_path / seq_printf / kfree
 */
static void vfio_device_show_fdinfo(struct seq_file *m, struct file *filep)
{
	/* [한국어] kobject_get_path 가 돌려줄 문자열 포인터. 새로 할당되므로 반드시 해제해야 한다. */
	char *path;
	/* [한국어] fd 컨텍스트를 꺼낸다. 이 훅은 이미 VFIO fd 임이 확정된 상태에서 불리므로
	 * 종류 검사를 하지 않는다. */
	struct vfio_device_file *df = filep->private_data;
	/* [한국어] 그 fd 가 가리키는 디바이스. */
	struct vfio_device *device = df->device;

	/* [한국어] 하부 디바이스의 sysfs 경로를 문자열로 얻는다. 새 메모리를 할당하므로
	 * GFP_KERNEL 을 넘긴다. */
	path = kobject_get_path(&device->dev->kobj, GFP_KERNEL);
	/* [한국어] 할당 실패면 조용히 반환한다. fdinfo 는 진단용이라 실패를 전파해 봐야
	 * 사용자가 할 수 있는 일이 없다. */
	if (!path)
		return;

	/* [한국어] 앞에 /sys 를 붙여 출력한다. kobject_get_path 는 마운트 지점을 포함하지 않은
	 * 경로를 주기 때문이다. */
	seq_printf(m, "vfio-device-syspath: /sys%s\n", path);
	/* [한국어] 할당한 문자열 해제. */
	kfree(path);
}
#endif

/* [한국어] device fd 의 file_operations. **group 경로와 cdev 경로가 이 하나를
 * 공유한다** — group.c 가 만든 fd 도, /dev/vfio/devices/vfio<N> 를 연 fd 도
 * 모두 이 fops 를 쓴다. 그래서 vfio_device_from_file 의 주소 비교가
 * 두 세대를 동시에 판별할 수 있다.
 * 설정자: 컴파일 시 정적 초기화. 이후 불변.
 * 읽는 자: VFS, device_cdev.c 의 cdev_init, group.c 의 fd 생성 코드,
 * 그리고 vfio_device_from_file 의 주소 비교.
 * 동기화: 읽기 전용이라 없다. */
const struct file_operations vfio_device_fops = {
	/* [한국어] 이 fops 를 소유한 모듈. fd 가 열려 있는 동안 vfio 모듈이 rmmod 되지
	 * 못하게 VFS 가 참조를 잡는다.
	 * 설정자: 정적 초기화. 읽는 자: VFS. 값 범위: 이 모듈. 동기화: 불변. */
	.owner		= THIS_MODULE,
	/* [한국어] open 훅. 구현은 device_cdev.c:23 에 있으며 cdev 경로 전용이다.
	 * group 경로는 이미 열린 fd 를 만들어 주므로 이 훅을 거치지 않는다.
	 * 설정자: 정적 초기화. 읽는 자: VFS 의 chrdev open.
	 * 값 범위: 유효 함수 포인터 또는 CONFIG_VFIO_DEVICE_CDEV 가 꺼졌을 때
	 * vfio.h:629 의 stub. 동기화: 불변. */
	.open		= vfio_device_fops_cdev_open,
	/* [한국어] release 훅. 이 파일의 vfio_device_fops_release 이며 두 세대 모두 여기로
	 * 온다.
	 * 설정자: 정적 초기화. 읽는 자: VFS 의 마지막 fput.
	 * 값 범위: 유효 함수 포인터. 동기화: 불변. */
	.release	= vfio_device_fops_release,
	/* [한국어] read 훅. 권한 게이트를 지나 vendor 의 read 로 위임한다.
	 * 설정자: 정적 초기화. 읽는 자: read(2) 경로의 VFS.
	 * 값 범위: 유효 함수 포인터. 동기화: 불변. */
	.read		= vfio_device_fops_read,
	/* [한국어] write 훅. read 와 대칭이다.
	 * 설정자: 정적 초기화. 읽는 자: write(2) 경로의 VFS.
	 * 값 범위: 유효 함수 포인터. 동기화: 불변. */
	.write		= vfio_device_fops_write,
	/* [한국어] ioctl 훅. 이 파일의 중심 디스패처이며 BIND_IOMMUFD, FEATURE,
	 * GET_REGION_INFO 를 직접 처리하고 나머지를 vendor 로 넘긴다.
	 * 설정자: 정적 초기화. 읽는 자: ioctl(2) 경로의 VFS.
	 * 값 범위: 유효 함수 포인터. 동기화: 불변. */
	.unlocked_ioctl	= vfio_device_fops_unl_ioctl,
	/* [한국어] 32비트 프로세스가 부른 ioctl 을 받는 훅. 커널 공용 어댑터를 그대로 꽂아,
	 * 포인터 인자만 폭을 맞춘 뒤 위 unlocked_ioctl 로 넘긴다. VFIO 의 ioctl
	 * 구조체들이 32/64비트에서 레이아웃이 같게 설계돼 있어 이 단순 어댑터로
	 * 충분하다.
	 * 설정자: 정적 초기화. 읽는 자: compat ioctl 경로의 VFS.
	 * 값 범위: 커널 공용 함수. 동기화: 불변. */
	.compat_ioctl	= compat_ptr_ioctl,
	/* [한국어] mmap 훅. **VFIO 성능의 전부가 이 슬롯에 달려 있다** — 여기서 매핑이
	 * 성립하면 그 뒤 사용자 공간은 시스템 콜 없이 디바이스 레지스터를 만진다.
	 * 설정자: 정적 초기화. 읽는 자: mmap(2) 경로의 VFS.
	 * 값 범위: 유효 함수 포인터. 동기화: 불변. */
	.mmap		= vfio_device_fops_mmap,
/* [한국어] procfs 가 있는 커널에서만 다음 슬롯을 채운다. */
#ifdef CONFIG_PROC_FS
	/* [한국어] /proc/<pid>/fdinfo/<fd> 출력 훅. 이 fd 가 어느 하드웨어인지 sysfs 경로로
	 * 알려 준다.
	 * 설정자: 정적 초기화. 읽는 자: procfs 의 fdinfo 읽기.
	 * 값 범위: 유효 함수 포인터. 동기화: 불변. */
	.show_fdinfo	= vfio_device_show_fdinfo,
#endif
};

/* [한국어]
 * vfio_device_from_file - 임의의 struct file 이 VFIO device fd 인지 판별하고 디바이스를 꺼낸다
 *
 * @file: 검사할 파일. 아무 fd 나 들어올 수 있다.
 * @return: VFIO device fd 면 그 디바이스 포인터, 아니면 NULL.
 *          **참조를 잡아 주지 않는다** — 호출자가 file 참조를 쥐고 있는 동안만
 *          유효하다.
 *
 * 왜 f_op 비교인가: 사용자 공간이 준 fd 가 정말 VFIO device fd 인지 확인할
 * 방법이 필요하다. private_data 를 먼저 읽고 그것으로 판단하면, VFIO 가 아닌
 * 파일의 private_data 를 vfio_device_file 로 잘못 해석하는 **타입 혼동**이
 * 된다. file_operations 포인터 비교는 커널에서 파일 종류를 식별하는 표준이자
 * 가장 안전한 방법이다. 주소가 같다는 것은 같은 모듈의 같은 정적 객체라는
 * 뜻이므로 위조할 수 없다.
 *
 * 코드 순서에 관한 관찰: private_data 를 f_op 검사보다 먼저 지역 변수로
 * 읽지만, 그 값을 쓰는 것은 검사를 통과한 뒤다. 포인터를 역참조하지 않고
 * 값만 복사하므로 안전하다.
 *
 * group fd 는 어떻게 되는가: group fd 는 f_op 가 group.c:475 의
 * vfio_group_fops 라서 여기서 NULL 이 된다. 그쪽 판별은
 * vfio_group_from_file(group.c:796)이 같은 방식으로 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   vfio_file_is_valid / vfio_file_enforced_coherent / vfio_file_set_kvm
 *     → [vfio_device_from_file]
 */
static struct vfio_device *vfio_device_from_file(struct file *file)
{
	/* [한국어] private_data 를 먼저 지역 변수로 읽는다. 이 시점에는 아직 VFIO 파일인지
	 * 모르지만, 포인터 값을 복사할 뿐 역참조하지 않으므로 안전하다. */
	struct vfio_device_file *df = file->private_data;

	/* [한국어] 파일 종류 판별의 핵심 한 줄. f_op 주소가 이 모듈의 정적 객체와 같아야만
	 * VFIO device fd 다. 주소 비교라 사용자 공간이 위조할 수 없다. */
	if (file->f_op != &vfio_device_fops)
		/* [한국어] VFIO device fd 가 아니면 NULL. 호출자가 group 쪽을 따로 시도한다. */
		return NULL;
	/* [한국어] 이제 안전하게 역참조해 디바이스를 꺼낸다. 참조는 잡지 않으므로
	 * 호출자가 file 을 쥐고 있는 동안만 유효하다. */
	return df->device;
}

/* [한국어]
 * vfio_file_is_valid - 주어진 파일이 VFIO 파일(group fd 또는 device fd)인지 알려 준다
 *
 * @file: 검사할 파일.
 * @return: 둘 중 하나면 true, 아니면 false.
 *
 * 누가 쓰는가: VFIO 바깥의 커널 코드가 "사용자가 준 이 fd 가 정말 VFIO 것인가"
 * 를 확인해야 할 때 쓴다. 대표적으로 KVM 이 가상머신에 디바이스를 연결할 때
 * fd 의 정체를 먼저 확인한다. EXPORT_SYMBOL_GPL 로 내보내는 이유다.
 *
 * 왜 두 종류를 모두 받는가: VFIO ABI 에 두 세대가 공존하므로, 외부 사용자는
 * 사용자가 어느 세대의 fd 를 줬는지 미리 알 수 없다. 이 함수와 아래의
 * vfio_file_enforced_coherent, vfio_file_set_kvm 이 모두 같은 이유로 두 갈래를
 * 가진다.
 *
 * 동작 과정: group 판별을 먼저 시도하고(CONFIG_VFIO_GROUP 이 꺼져 있으면
 * vfio.h 의 stub 이 NULL 을 돌려줘 자동으로 건너뛴다), 아니면 device 판별.
 * 둘 다 아니면 false.
 *
 * 실행 컨텍스트: 외부 모듈의 프로세스 문맥. 락 없음.
 *
 * 호출 체인:
 *   KVM 등 외부 모듈 → [vfio_file_is_valid]
 *     → vfio_group_from_file / vfio_device_from_file
 */
/**
 * vfio_file_is_valid - True if the file is valid vfio file
 * @file: VFIO group file or VFIO device file
 */
bool vfio_file_is_valid(struct file *file)
{
	/* [한국어] group fd 인지 먼저 보고, */
	return vfio_group_from_file(file) ||
	       /* [한국어] 아니면 device fd 인지 본다. 둘 중 하나면 유효한 VFIO 파일이다.
	        * 단락 평가라 group 이면 device 판별을 건너뛴다. */
	       vfio_device_from_file(file);
}
/* [한국어] EXPORT_SYMBOL_GPL — KVM 등 VFIO 바깥 커널 코드가 fd 정체를 확인할 때 쓴다. */
EXPORT_SYMBOL_GPL(vfio_file_is_valid);

/* [한국어]
 * vfio_file_enforced_coherent - 이 VFIO 파일의 DMA 가 항상 CPU 캐시와 일관적인지 알려 준다
 *
 * @file: group fd 또는 device fd.
 * @return: true 면 IOMMU 가 일관성을 강제한다. false 면 사용자가 캐시를
 *          무시하는 DMA 를 만들 수 있다는 뜻이다.
 *
 * 왜 KVM 이 이것을 알아야 하는가: 상류 주석이 정확히 설명한다. "강제된
 * 일관성" 은 IOMMU 가 PCIe 의 no-snoop 비트 같은 것을 무시한다는 뜻이다.
 * 그렇지 않다면(false) 게스트가 캐시를 우회하는 DMA 를 일으킬 수 있고,
 * 그러면 KVM 은 게스트에게 x86 의 캐시 무효화 명령 같은 특권 수단을 허용해야
 * 한다. 즉 이 함수의 답이 **게스트에게 어떤 명령을 열어 줄지**를 바꾼다.
 *
 * 세 갈래:
 *  1. group fd 면 group 안 모든 디바이스를 훑는 group.c 의 판정에 위임한다.
 *     group 은 여러 디바이스를 포함하므로 하나라도 비일관적이면 전체가
 *     비일관적이다.
 *  2. device fd 면 그 디바이스의 IOMMU 능력을 직접 묻는다.
 *     IOMMU_CAP_ENFORCE_CACHE_COHERENCY 는 IOMMU 코어의 능력 질의이며,
 *     그 구현은 이 트리에 drivers/iommu 가 없어 확인 못 함.
 *  3. 둘 다 아니면 true 를 돌려준다. **안전한 쪽 기본값**이다 — true 는
 *     "게스트에게 추가 권한을 주지 않는다" 를 뜻하므로, 정체 모를 파일에
 *     대해 권한을 넓히지 않는다.
 *
 * 실행 컨텍스트: 외부 모듈의 프로세스 문맥.
 *
 * 호출 체인:
 *   KVM → [vfio_file_enforced_coherent]
 *     → vfio_group_enforced_coherent / device_iommu_capable
 */
/**
 * vfio_file_enforced_coherent - True if the DMA associated with the VFIO file
 *        is always CPU cache coherent
 * @file: VFIO group file or VFIO device file
 *
 * Enforced coherency means that the IOMMU ignores things like the PCIe no-snoop
 * bit in DMA transactions. A return of false indicates that the user has
 * rights to access additional instructions such as wbinvd on x86.
 */
bool vfio_file_enforced_coherent(struct file *file)
{
	/* [한국어] device fd 갈래의 결과를 담을 변수. */
	struct vfio_device *device;
	/* [한국어] group fd 갈래의 결과를 담을 변수. */
	struct vfio_group *group;

	/* [한국어] 먼저 group fd 인지 본다. */
	group = vfio_group_from_file(file);
	/* [한국어] group fd 면 */
	if (group)
		/* [한국어] group 안 모든 디바이스를 훑는 판정에 위임한다. 하나라도 비일관적이면
		 * 전체가 비일관적이므로 group 단위 판정이 옳다. */
		return vfio_group_enforced_coherent(group);

	/* [한국어] group 이 아니면 device fd 인지 본다. */
	device = vfio_device_from_file(file);
	/* [한국어] device fd 면 */
	if (device)
		/* [한국어] 그 디바이스의 IOMMU 능력을 직접 묻는다. 구현은 IOMMU 코어에 있으며
		 * 이 트리에는 drivers/iommu 가 없어 확인 못 함. */
		return device_iommu_capable(device->dev,
					    /* [한국어] 이 능력 비트가 있으면 IOMMU 가 PCIe no-snoop 같은 캐시 우회 요청을
					     * 무시한다는 뜻이다. 상류 주석이 밝히듯, 없으면 사용자에게 x86 의 캐시
					     * 무효화 명령 같은 추가 권한이 필요해진다. */
					    IOMMU_CAP_ENFORCE_CACHE_COHERENCY);

	/* [한국어] 둘 다 아닌 정체 모를 파일이면 true 를 돌려준다. **안전한 쪽 기본값**이다 —
	 * true 는 게스트에게 추가 권한을 주지 않는다는 뜻이므로, 모르는 파일에
	 * 대해 권한을 넓히지 않는다. */
	return true;
}
/* [한국어] EXPORT_SYMBOL_GPL — KVM 이 게스트 캐시 정책을 정할 때 쓴다. */
EXPORT_SYMBOL_GPL(vfio_file_enforced_coherent);

/* [한국어]
 * vfio_device_file_set_kvm - device fd 컨텍스트에 KVM 포인터를 기록한다
 *
 * @file: device fd. private_data 가 vfio_device_file 이다.
 * @kvm: 연결할 KVM 인스턴스(또는 NULL 로 해제).
 * @return: 없음.
 *
 * 왜 df 에 먼저 적는가: 상류 주석이 밝힌 그대로다. KVM 은 디바이스가 열리기
 * 전에도 fd 를 연결할 수 있으므로, 일단 fd 컨텍스트에 적어 두었다가
 * BIND_IOMMUFD 가 성공하는 시점에 device->kvm 으로 옮긴다
 * (device_cdev.c:59 의 vfio_df_get_kvm_safe 가 df->kvm 을 읽어
 * vfio_device_get_kvm_safe 에 넘긴다). 곧장 device 에 적으면 아직 결합되지
 * 않은 디바이스에 KVM 참조가 걸린다.
 *
 * kvm_ref_lock 이 지키는 것: df->kvm 필드 하나다. 경쟁 상대는 KVM 쪽에서
 * 비동기로 들어오는 이 함수와, bind 경로에서 그 값을 읽어 참조를 잡는
 * device_cdev.c 의 코드다. 락이 없으면 "읽는 순간 다른 CPU 가 포인터를 바꿔"
 * 이미 죽은 KVM 에 참조를 걸 수 있다. spinlock 인 이유는 임계 구역이 대입
 * 한 줄로 극히 짧고 그 안에서 잠들 일이 없기 때문이다.
 *
 * 이 함수가 참조를 잡지 않는다는 점: 여기서는 포인터만 적는다. 실제 참조
 * 획득은 vfio_device_get_kvm_safe 가 한다. 그래서 이 함수만으로는 KVM 의
 * 수명을 붙잡지 못하며, 그 사이 KVM 이 사라지면 bind 시점의 안전 획득
 * 함수가 실패해 조용히 무시된다.
 *
 * 실행 컨텍스트: KVM 측 프로세스 문맥. spinlock 구간 안에서는 선점이 꺼진다.
 *
 * 호출 체인:
 *   vfio_file_set_kvm → [vfio_device_file_set_kvm] → spin_lock / spin_unlock
 */
static void vfio_device_file_set_kvm(struct file *file, struct kvm *kvm)
{
	struct vfio_device_file *df = file->private_data;

	/*
	 * The kvm is first recorded in the vfio_device_file, and will
	 * be propagated to vfio_device::kvm when the file is bound to
	 * iommufd successfully in the vfio device cdev path.
	 */
	/* [한국어] df->kvm 필드 하나를 지키는 spinlock 을 잡는다. 경쟁 상대는 KVM 쪽에서
	 * 비동기로 들어오는 이 함수와, bind 경로에서 이 값을 읽어 참조를 잡는
	 * device_cdev.c 의 코드다. 락이 없으면 읽는 순간 포인터가 바뀌어 이미 죽은
	 * KVM 에 참조를 걸 수 있다. */
	spin_lock(&df->kvm_ref_lock);
	/* [한국어] 포인터를 기록한다. 임계 구역이 대입 한 줄뿐이라 mutex 가 아니라
	 * spinlock 으로 충분하다 — 그 안에서 잠들 일이 없다. */
	df->kvm = kvm;
	/* [한국어] 락 해제. */
	spin_unlock(&df->kvm_ref_lock);
}

/* [한국어]
 * vfio_file_set_kvm - VFIO 파일에 KVM 인스턴스를 연결한다(두 세대 공용 진입점)
 *
 * @file: group fd 또는 device fd.
 * @kvm: 연결할 KVM(NULL 이면 해제).
 * @return: 없음.
 *
 * 왜 KVM 과 VFIO 를 잇는가: vendor 드라이버가 게스트의 특성을 알아야 하는
 * 경우가 있다. 상류 주석대로 디바이스가 처음 열릴 때 device->kvm 으로
 * 전달되며, vendor 는 그것으로 KVM 과 협력한다.
 *
 * 두 갈래와 그 비대칭:
 *  - group fd 면 vfio_group_set_kvm(group.c)으로 group 에 적는다. group 은
 *     여러 디바이스를 포함하므로 KVM 연결도 group 단위다.
 *  - device fd 면 vfio_device_file_set_kvm 으로 fd 컨텍스트에 적는다.
 * 두 검사가 if / if 로 나란히 있고 else 로 묶여 있지 않은데, 한 파일이
 * group 이면서 동시에 device 일 수 없으므로 실질적으로 배타적이다.
 * 어느 쪽도 아니면 아무 일도 하지 않는다 — 잘못된 fd 에 대해 조용히 무시하는
 * 것이 이 API 의 계약이다.
 *
 * 실행 컨텍스트: KVM 측 프로세스 문맥.
 *
 * 호출 체인:
 *   KVM → [vfio_file_set_kvm]
 *     → vfio_group_set_kvm / vfio_device_from_file / vfio_device_file_set_kvm
 */
/**
 * vfio_file_set_kvm - Link a kvm with VFIO drivers
 * @file: VFIO group file or VFIO device file
 * @kvm: KVM to link
 *
 * When a VFIO device is first opened the KVM will be available in
 * device->kvm if one was associated with the file.
 */
void vfio_file_set_kvm(struct file *file, struct kvm *kvm)
{
	/* [한국어] group 판별 결과를 담을 지역 변수. device 쪽은 값을 쓸 일이 없어
	 * 변수 없이 조건식에서 바로 판정한다. */
	struct vfio_group *group;

	/* [한국어] 먼저 group fd 인지 본다. CONFIG_VFIO_GROUP 이 꺼져 있으면 vfio.h 의
	 * stub 이 NULL 을 돌려줘 이 갈래가 자동으로 비활성화된다. */
	group = vfio_group_from_file(file);
	/* [한국어] group fd 라면 */
	if (group)
		/* [한국어] group 객체에 KVM 을 적는다. group 은 여러 디바이스를 포함하므로
		 * KVM 연결도 group 단위다. */
		vfio_group_set_kvm(group, kvm);

	/* [한국어] device fd 인지 본다. 반환 포인터를 쓰지 않고 존재 여부만 보므로
	 * 변수에 담지 않는다. 한 파일이 group 이면서 device 일 수 없어
	 * 위 조건과 실질적으로 배타적이다. */
	if (vfio_device_from_file(file))
		/* [한국어] fd 컨텍스트에 KVM 을 적는다. 여기서는 포인터만 적고 참조는 잡지 않는다 —
		 * 실제 참조 획득은 bind 시점의 vfio_device_get_kvm_safe 가 한다. */
		vfio_device_file_set_kvm(file, kvm);
}
/* [한국어] EXPORT_SYMBOL_GPL — KVM 같은 GPL 커널 모듈만 이 연결 API 를 쓸 수 있다. */
EXPORT_SYMBOL_GPL(vfio_file_set_kvm);

/* [한국어]
 * vfio_info_cap_add - capability chain 버퍼를 늘리고 새 항목 헤더를 만들어 돌려준다
 *
 * @caps: 조립 중인 chain. buf 와 size 를 이 함수가 갱신한다.
 * @size: 새 항목의 크기(헤더 포함). 내부에서 8바이트 배수로 올림된다.
 * @id: 항목 종류 식별자(예: sparse mmap capability).
 * @version: 그 종류의 버전.
 * @return: 새로 만들어진 항목의 헤더 포인터. 실패 시 ERR_PTR(-ENOMEM).
 *          호출자는 이 포인터 뒤에 자기 데이터를 채운다.
 *
 * 왜 chain 인가: GET_REGION_INFO 같은 ioctl 은 고정 구조체로 다 담을 수 없는
 * 부가 정보를 사용자에게 줘야 한다. 그것을 "헤더 + 데이터" 항목의 연결
 * 리스트로 만들어 고정 구조체 뒤에 덧붙인다. 연결은 포인터가 아니라
 * **버퍼 시작에서의 오프셋**이다. 사용자 공간으로 복사돼야 하므로 커널
 * 포인터를 담을 수 없기 때문이다.
 *
 * 8바이트 정렬을 강제하는 이유: 다음 항목의 시작이 정렬되어야 그 안의
 * 64비트 필드에 비정렬 접근이 생기지 않는다. 아키텍처에 따라 비정렬 접근이
 * 성능 저하나 오류를 낳는다.
 *
 * 동작 과정:
 *  1. size 를 8의 배수로 올린다.
 *  2. krealloc 으로 버퍼를 늘린다. 첫 호출이면 caps->buf 가 NULL 이라
 *     krealloc 이 새 할당처럼 동작한다.
 *  3. 실패하면 **기존 버퍼까지 해제하고 caps 를 0 으로 되돌린 뒤** ERR_PTR.
 *     krealloc 실패 시 원래 버퍼는 살아 있으므로 여기서 kfree 하지 않으면
 *     누수가 된다. 호출자는 ERR_PTR 만 보고 caps 를 더 만지지 않아도 된다.
 *  4. 새 항목의 위치는 늘리기 전 크기 지점이다.
 *  5. 그 자리를 0 으로 지운다. 사용자에게 복사될 메모리라 커널 잔재를
 *     남기면 정보 누출이 된다.
 *  6. id 와 version 을 채운다.
 *  7. 기존 chain 의 마지막 항목을 찾아 그 next 에 새 항목의 오프셋을 적는다.
 *     첫 항목일 때는 buf 자신이 tmp 가 되고 그 next 가 0(= 아직 없음)이라
 *     루프가 즉시 끝나며, tmp->next 에 caps->size(= 0)를 적는다. 즉 첫
 *     항목의 next 는 0 으로 남아 "체인의 끝" 을 뜻한다.
 *  8. 누적 크기를 늘린다.
 *
 * 상류 주석이 못 박는 후처리: 여기서 만든 오프셋들은 **버퍼 시작 기준**이다.
 * 사용자 구조체 안으로 복사할 때는 vfio_info_cap_shift 로 전부 밀어야 한다.
 *
 * 실행 컨텍스트: vendor 의 정보 조립 콜백 안, 프로세스 문맥. GFP_KERNEL.
 *
 * 호출 체인:
 *   vendor 의 get_region_info_caps 등 → [vfio_info_cap_add] → krealloc / memset
 */
/*
 * Sub-module support
 */
/*
 * Helper for managing a buffer of info chain capabilities, allocate or
 * reallocate a buffer with additional @size, filling in @id and @version
 * of the capability.  A pointer to the new capability is returned.
 *
 * NB. The chain is based at the head of the buffer, so new entries are
 * added to the tail, vfio_info_cap_shift() should be called to fixup the
 * next offsets prior to copying to the user buffer.
 */
struct vfio_info_cap_header *vfio_info_cap_add(struct vfio_info_cap *caps,
					       size_t size, u16 id, u16 version)
{
	/* [한국어] krealloc 결과를 받을 임시 포인터. 실패 시 caps->buf 를 덮어쓰지 않기
	 * 위해 별도 변수가 필요하다. */
	void *buf;
	/* [한국어] header 는 새로 만든 항목, tmp 는 체인 끝을 찾는 커서. */
	struct vfio_info_cap_header *header, *tmp;

	/* Ensure that the next capability struct will be aligned */
	/* [한국어] 크기를 8바이트 배수로 올린다. 다음 항목의 시작이 정렬되어야 그 안의
	 * 64비트 필드에 비정렬 접근이 생기지 않는다. */
	size = ALIGN(size, sizeof(u64));

	/* [한국어] 버퍼를 새 항목 크기만큼 늘린다. 첫 호출이면 caps->buf 가 NULL 이라
	 * 새 할당처럼 동작한다. */
	buf = krealloc(caps->buf, caps->size + size, GFP_KERNEL);
	/* [한국어] krealloc 실패 시 원래 버퍼는 그대로 살아 있다. */
	if (!buf) {
		/* [한국어] 그래서 여기서 직접 해제해야 누수가 없다. */
		kfree(caps->buf);
		/* [한국어] 호출자가 실패 뒤 caps 를 그대로 두어도 안전하도록 포인터를 비우고 */
		caps->buf = NULL;
		/* [한국어] 누적 크기도 0 으로 되돌린다. 이 두 줄 덕분에 호출자는 ERR_PTR 만 보고
		 * 뒷정리를 하지 않아도 된다. */
		caps->size = 0;
		return ERR_PTR(-ENOMEM);
	}

	/* [한국어] 성공했으니 새 버퍼 주소를 반영한다. krealloc 은 주소를 옮길 수 있으므로
	 * 반드시 갱신해야 한다. */
	caps->buf = buf;
	/* [한국어] 새 항목의 위치는 늘리기 전 크기 지점, 즉 기존 내용의 바로 뒤다. */
	header = buf + caps->size;

	/* Eventually copied to user buffer, zero */
	/* [한국어] 새 항목 영역을 0 으로 지운다. 이 메모리는 결국 사용자 공간으로 복사되므로
	 * 커널 잔재가 남으면 정보 누출이 된다. */
	memset(header, 0, size);

	/* [한국어] 항목 종류 식별자. */
	header->id = id;
	/* [한국어] 그 종류의 버전. 사용자 공간이 구조를 해석할 때 쓴다. */
	header->version = version;

	/* Add to the end of the capability chain */
	/* [한국어] 기존 체인의 마지막 항목을 찾는다. next 가 0 이면 끝이다.
	 * 첫 항목일 때는 buf 자신이 커서가 되고 그 next 가 0 이라 즉시 멈춘다. */
	for (tmp = buf; tmp->next; tmp = buf + tmp->next)
		/* [한국어] 본문이 비어 있음을 명시한 상류 표기. 커서 이동이 증가식에 다 들어 있다. */
		; /* nothing */

	/* [한국어] 마지막 항목의 next 에 새 항목의 오프셋을 적어 체인을 잇는다.
	 * 첫 항목이면 caps->size 가 0 이라 next 가 0 으로 남아 여전히 끝을 뜻한다. */
	tmp->next = caps->size;
	/* [한국어] 누적 크기를 늘린다. 다음 호출의 새 항목 위치가 여기서 정해진다. */
	caps->size += size;

	/* [한국어] 새로 만든 항목의 헤더 주소를 돌려준다. 호출자가 이 뒤에 자기 데이터를
	 * 채운다. */
	return header;
}
/* [한국어] EXPORT_SYMBOL_GPL 로 내보낸다. 바로 아래 vfio_info_cap_shift 는
 * _GPL 없이 내보낸다는 점이 대조적이다. */
EXPORT_SYMBOL_GPL(vfio_info_cap_add);

/* [한국어]
 * vfio_info_cap_shift - chain 안의 모든 next 오프셋을 주어진 값만큼 민다
 *
 * @caps: 완성된 chain.
 * @offset: 더할 값. 보통 앞에 놓일 고정 구조체의 크기다.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_info_cap_add 가 만든 next 는 chain 버퍼 시작 기준이다.
 * 그런데 사용자에게는 "고정 구조체 + chain" 이 이어진 형태로 복사되므로,
 * 사용자가 보는 기준점은 고정 구조체의 시작이다. 그 차이만큼 모든 오프셋을
 * 밀어야 사용자가 chain 을 따라갈 수 있다. 미루지 않으면 사용자가 첫 항목보다
 * 앞쪽(고정 구조체 내부)을 가리키게 되어 chain 해석이 완전히 어긋난다.
 *
 * 루프의 트릭(이 파일에서 가장 읽기 어려운 세 줄):
 * 증가식이 buf + tmp->next - offset 이다. 본문에서 tmp->next 에 offset 을
 * 이미 더했으므로, 다음 항목의 **원래** 오프셋을 되찾으려면 그만큼 빼야 한다.
 * 즉 본문의 수정과 증가식의 보정이 한 쌍이다. 순서를 바꾸거나 보정을 빼면
 * 두 번째 항목부터 엉뚱한 곳을 가리킨다.
 * 종료 조건은 tmp->next 가 0 인 것이다. 0 은 "다음 없음" 을 뜻하며,
 * 본문이 실행되지 않은 마지막 항목에서 자연히 멈춘다.
 *
 * WARN_ON 이 정렬을 보는 이유: offset 이 8의 배수가 아니면 밀린 뒤 모든
 * 항목의 정렬이 깨진다. vfio_info_cap_add 가 애써 맞춰 둔 정렬을 호출자가
 * 망가뜨리는 것을 개발 단계에서 잡는다.
 *
 * [상류 코드 관찰] 이 함수는 EXPORT_SYMBOL 로 내보내는데, 바로 위에서 같은
 * chain 을 만드는 vfio_info_cap_add 는 EXPORT_SYMBOL_GPL 을 쓴다. 이 파일에서
 * _GPL 이 아닌 export 는 vfio_info_cap_shift, vfio_info_add_capability,
 * vfio_set_irqs_validate_and_prepare, vfio_pin_pages, vfio_unpin_pages,
 * vfio_dma_rw 여섯 개이고 나머지 열다섯 개는 모두 _GPL 이다. 모듈 자체는
 * MODULE_LICENSE 가 "GPL v2" 다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: 정보 조립을 마친 ioctl 처리 경로, 프로세스 문맥. 순수 계산.
 *
 * 호출 체인:
 *   vfio_get_region_info / vendor 의 정보 ioctl → [vfio_info_cap_shift]
 */
void vfio_info_cap_shift(struct vfio_info_cap *caps, size_t offset)
{
	/* [한국어] chain 을 훑을 커서. */
	struct vfio_info_cap_header *tmp;
	/* [한국어] 버퍼 시작 주소. 모든 next 오프셋의 기준점이다. void 로 캐스팅해
	 * 바이트 단위 포인터 산술이 가능하게 한다. */
	void *buf = (void *)caps->buf;

	/* Capability structs should start with proper alignment */
	/* [한국어] offset 이 8의 배수가 아니면 밀린 뒤 모든 항목의 정렬이 깨진다.
	 * vfio_info_cap_add 가 맞춰 둔 정렬을 호출자가 망가뜨리는 것을 잡는다. */
	WARN_ON(!IS_ALIGNED(offset, sizeof(u64)));

	/* [한국어] 종료 조건은 next 가 0 인 것이며, 0 은 체인의 끝을 뜻한다.
	 * 증가식이 특이하다 — 본문에서 next 에 offset 을 이미 더했으므로,
	 * 다음 항목의 원래 오프셋을 되찾으려면 그만큼 빼야 한다.
	 * 본문의 수정과 증가식의 보정이 한 쌍이다. */
	for (tmp = buf; tmp->next; tmp = buf + tmp->next - offset)
		/* [한국어] 이 항목의 next 를 사용자 기준으로 민다. */
		tmp->next += offset;
}
/* [한국어] EXPORT_SYMBOL 이며 _GPL 이 아니다. 바로 위 vfio_info_cap_add 는
 * EXPORT_SYMBOL_GPL 을 쓴다. */
EXPORT_SYMBOL(vfio_info_cap_shift);

/* [한국어]
 * vfio_info_add_capability - 이미 만들어진 capability 항목을 chain 에 통째로 복사해 넣는다
 *
 * @caps: 대상 chain.
 * @cap: 원본 항목. 헤더(id, version)와 그 뒤 데이터가 이어져 있다.
 * @size: 원본 항목 전체 크기(헤더 포함).
 * @return: 0 성공, 음수 errno(사실상 -ENOMEM).
 *
 * vfio_info_cap_add 와의 차이: cap_add 는 빈 자리를 만들어 주고 호출자가
 * 데이터를 채우게 한다. 이 함수는 **이미 완성된 항목이 손에 있을 때** 그것을
 * 그대로 옮겨 붙인다. vendor 가 지역 변수에 구조체를 다 채워 놓고 마지막에
 * 한 줄로 chain 에 넣고 싶을 때 쓰는 편의 함수다.
 *
 * 동작 과정:
 *  1. cap 의 id 와 version 을 그대로 써서 chain 에 자리를 만든다.
 *  2. 헤더를 **뺀** 나머지 바이트를 복사한다. header + 1 과 cap + 1 은 각각
 *     헤더 구조체 하나 뒤를 가리키는 포인터 산술이며, 복사 길이도
 *     size - sizeof(*header) 로 헤더만큼 줄인다. 헤더는 1번에서 이미 채워졌고,
 *     그 안의 next 필드는 chain 조립 결과라서 원본 값으로 덮으면 안 된다.
 *     이 "헤더를 건너뛰는" 처리가 이 함수의 존재 이유 전부다.
 *
 * 실행 컨텍스트: vendor 의 정보 조립 콜백 안, 프로세스 문맥.
 *
 * 에러 경로: cap_add 가 ERR_PTR 을 주면 PTR_ERR 로 바꿔 전달한다. 그 시점에
 * caps 는 이미 cap_add 가 0 으로 되돌려 놓은 뒤다.
 *
 * 호출 체인:
 *   vendor(예: drivers/vfio/pci/nvgrace-gpu/main.c:451,
 *   drivers/vfio/vfio_iommu_type1.c 의 정보 ioctl) → [vfio_info_add_capability]
 *     → vfio_info_cap_add / memcpy
 */
int vfio_info_add_capability(struct vfio_info_cap *caps,
			     struct vfio_info_cap_header *cap, size_t size)
{
	struct vfio_info_cap_header *header;

	/* [한국어] 원본 항목의 id 와 version 을 그대로 써서 chain 에 자리를 만든다.
	 * 크기도 원본과 같으므로 뒤에 이어질 데이터가 정확히 들어간다. */
	header = vfio_info_cap_add(caps, size, cap->id, cap->version);
	/* [한국어] 자리 만들기가 실패했으면 오류를 그대로 올린다. 이 시점에 caps 는
	 * vfio_info_cap_add 가 이미 0 으로 되돌려 놓은 상태다. */
	if (IS_ERR(header))
		return PTR_ERR(header);

	/* [한국어] 헤더 **뒤** 데이터만 복사한다. header + 1 과 cap + 1 은 각각 헤더 구조체
	 * 하나만큼 뒤를 가리키는 포인터 산술이고, 길이도 헤더 크기만큼 뺀다.
	 * 헤더를 통째로 복사하면 chain 조립 결과인 next 필드가 원본 값으로 덮여
	 * 연결이 끊긴다 — 이 한 줄이 이 함수의 존재 이유다. */
	memcpy(header + 1, cap + 1, size - sizeof(*header));

	return 0;
}
/* [한국어] EXPORT_SYMBOL 이며 _GPL 이 아니다. */
EXPORT_SYMBOL(vfio_info_add_capability);

/* [한국어]
 * vfio_set_irqs_validate_and_prepare - VFIO_DEVICE_SET_IRQS 인자를 검증하고 뒤따를 데이터 크기를 계산한다
 *
 * @hdr: 사용자가 준 vfio_irq_set 헤더(호출자가 이미 커널로 복사해 둔 것).
 * @num_irqs: 이 디바이스의 해당 종류 인터럽트 개수.
 * @max_irq_type: 유효한 인터럽트 종류 수(index 의 상한).
 * @data_size: 출력. 헤더 뒤에 따라올 데이터의 바이트 수. NULL 을 줄 수 있고,
 *             그때는 데이터가 없는 요청만 허용된다.
 * @return: 0 이면 통과, -EINVAL 이면 어느 검증에든 걸린 것이다.
 *
 * 왜 코어가 이 검증을 대신하는가: SET_IRQS 는 vendor 마다 처리가 다르지만
 * **인자 형식은 공통**이다. 각 vendor 가 따로 검증하면 하나만 실수해도 커널
 * 취약점이 된다. 그래서 형식 검증을 코어가 한 곳에서 한다. vendor 는 이 함수를
 * 통과한 뒤의 값만 믿고 쓰면 된다.
 *
 * 검증 항목과 각각이 막는 것:
 *  - argsz 가 헤더 최소 크기보다 작으면 사용자가 잘못 보고한 것이다.
 *  - index 가 종류 수 이상이면 vendor 의 배열 밖을 가리킨다.
 *  - count 가 U32_MAX - start 이상이면 start + count 가 넘친다. **더하기 전에**
 *    막는 것이 정석이다.
 *  - 정의되지 않은 플래그 비트는 미래 확장을 위한 방어다.
 *  - start 와 start + count 가 num_irqs 를 넘으면 인터럽트 배열 밖이다.
 *    앞의 넘침 검사를 통과한 뒤라 이 덧셈은 안전하다.
 *
 * 데이터 크기 계산: 플래그의 DATA 종류가 크기를 정한다. NONE 이면 0,
 * BOOL 이면 인터럽트당 1바이트, EVENTFD 이면 인터럽트당 4바이트(fd 번호).
 * 정의되지 않은 종류면 -EINVAL.
 * 크기가 0 이 아니면 두 가지를 더 본다.
 *  - argsz 에서 헤더를 뺀 나머지가 필요한 만큼 되는가. 안 되면 사용자가
 *    보고한 크기보다 많이 읽게 되므로 거절한다.
 *  - data_size 포인터가 NULL 인데 데이터가 필요한 요청이면, 호출자가 데이터를
 *    받을 준비가 안 된 것이므로 거절한다.
 *
 * 출력 초기화 위치에 관한 관찰: data_size 를 0 으로 미리 세우는 코드가
 * 플래그 판정보다 앞에 있다. 그래서 DATA_NONE 요청이나 뒤쪽 검증 실패 시에도
 * 호출자는 쓰레기 값을 보지 않는다.
 *
 * 실행 컨텍스트: vendor 의 SET_IRQS 처리 안, 프로세스 문맥. 순수 계산.
 *
 * 호출 체인:
 *   vendor 의 ioctl(예: drivers/vfio/cdx/main.c:188) →
 *   [vfio_set_irqs_validate_and_prepare]
 */
int vfio_set_irqs_validate_and_prepare(struct vfio_irq_set *hdr, int num_irqs,
				       int max_irq_type, size_t *data_size)
{
	/* [한국어] 최소 헤더 크기를 담을 지역 변수. 뒤에서 데이터 크기 계산에도 쓰인다. */
	unsigned long minsz;
	/* [한국어] 인터럽트 하나당 따라오는 데이터의 바이트 수. */
	size_t size;

	/* [한국어] count 필드 끝까지가 이 구조체의 필수 헤더다. 그 뒤는 가변 데이터 영역이다. */
	minsz = offsetofend(struct vfio_irq_set, count);

	/* [한국어] 네 가지를 한 조건으로 묶어 검사한다. 첫째, 사용자가 보고한 argsz 가
	 * 헤더보다 작으면 구조체가 성립하지 않는다. */
	if ((hdr->argsz < minsz) || (hdr->index >= max_irq_type) ||
	    /* [한국어] 둘째, index 가 이 디바이스의 인터럽트 종류 수 이상이면 vendor 의 배열
	     * 밖을 가리킨다. 셋째, count 가 U32_MAX - start 이상이면 아래에서 할
	     * start + count 덧셈이 넘친다. **더하기 전에** 막는 것이 핵심이다. */
	    (hdr->count >= (U32_MAX - hdr->start)) ||
	    /* [한국어] 넷째, 정의된 두 마스크(데이터 종류, 동작 종류) 밖의 비트가 켜져 있으면
	     * 거절한다. 지금 무시하면 그 비트에 나중에 의미가 생겼을 때 옛 프로그램의
	     * 쓰레기 값이 새 기능을 켜 버린다. */
	    (hdr->flags & ~(VFIO_IRQ_SET_DATA_TYPE_MASK |
				VFIO_IRQ_SET_ACTION_TYPE_MASK)))
		return -EINVAL;

	/* [한국어] 출력 포인터가 있으면 */
	if (data_size)
		/* [한국어] 먼저 0 으로 세운다. 아래 어느 경로로 빠져나가도 호출자가 쓰레기 값을
		 * 보지 않게 하는 배치다. */
		*data_size = 0;

	/* [한국어] 인터럽트 번호 범위 검사. 앞의 넘침 검사를 통과한 뒤라 이 덧셈은 안전하다.
	 * start 가 범위 밖이거나 끝이 범위를 넘으면 vendor 배열 밖 접근이 된다. */
	if (hdr->start >= num_irqs || hdr->start + hdr->count > num_irqs)
		return -EINVAL;

	/* [한국어] 데이터 종류 비트만 남겨 분기한다. 종류가 인터럽트 하나당 바이트 수를 정한다. */
	switch (hdr->flags & VFIO_IRQ_SET_DATA_TYPE_MASK) {
	/* [한국어] 데이터 없음 — SET_IRQS 로 인터럽트를 그냥 끄거나 트리거만 할 때. */
	case VFIO_IRQ_SET_DATA_NONE:
		/* [한국어] 따라올 데이터가 없다. */
		size = 0;
		break;
	/* [한국어] 불리언 배열 — 인터럽트별 마스크/언마스크 여부. */
	case VFIO_IRQ_SET_DATA_BOOL:
		/* [한국어] 인터럽트당 1바이트. */
		size = sizeof(uint8_t);
		break;
	/* [한국어] eventfd 배열 — 인터럽트별로 사용자가 준 eventfd 번호.
	 * 이것이 SPDK 나 QEMU 가 MSI-X 를 사용자 공간에서 받는 방식이다. */
	case VFIO_IRQ_SET_DATA_EVENTFD:
		/* [한국어] 인터럽트당 4바이트(fd 번호). */
		size = sizeof(int32_t);
		break;
	/* [한국어] 정의되지 않은 데이터 종류. 마스크 안의 값이라도 알려진 셋 중 하나가
	 * 아니면 거절한다. */
	default:
		return -EINVAL;
	}

	/* [한국어] 따라올 데이터가 있는 요청이면 두 가지를 더 본다. */
	if (size) {
		/* [한국어] 사용자가 보고한 argsz 에서 헤더를 뺀 나머지가 필요한 만큼 되는가.
		 * 안 되면 vendor 가 사용자 버퍼 밖을 읽게 된다. */
		if (hdr->argsz - minsz < hdr->count * size)
			return -EINVAL;

		/* [한국어] 데이터가 필요한데 호출자가 출력 포인터를 주지 않았다면, 호출자 쪽이
		 * 데이터를 받을 준비가 안 된 것이다. vendor 구현의 실수를 여기서 막는다. */
		if (!data_size)
			return -EINVAL;

		/* [한국어] 필요한 데이터 바이트 수를 호출자에게 알린다. 호출자는 그만큼을
		 * 사용자 공간에서 더 읽어 온다. */
		*data_size = hdr->count * size;
	}

	return 0;
}
/* [한국어] EXPORT_SYMBOL 이며 _GPL 이 아니다. */
EXPORT_SYMBOL(vfio_set_irqs_validate_and_prepare);

/* [한국어]
 * vfio_pin_pages - vendor 가 게스트 메모리 페이지를 핀하고 호스트 페이지를 얻는다
 *
 * @device: 요청하는 디바이스. emulated 디바이스여야 한다.
 * @iova: 핀할 사용자 페이지들의 시작 IOVA.
 * @npage: 핀할 페이지 수. 상류가 정한 상한을 넘으면 안 된다.
 * @prot: 보호 플래그. IOMMU_WRITE 가 켜져 있으면 쓰기 가능으로 핀한다.
 * @pages: 출력 배열. 핀된 호스트 struct page 포인터들이 담긴다.
 * @return: 핀한 페이지 수(양수), 또는 음수 errno.
 *
 * 누가 왜 쓰는가: mdev 나 vfio-ap 처럼 하드웨어가 직접 DMA 를 하지 않는
 * 디바이스의 vendor 드라이버다. 그런 vendor 는 게스트가 준 주소를 자기가
 * 읽고 써야 하는데, 게스트 메모리는 사용자 공간 메모리라 언제든 스왑되거나
 * 이동할 수 있다. 그래서 접근 직전에 핀해 고정한다. 상류 주석이 명시하듯
 * **vfio_register_emulated_iommu_dev 로 등록한 디바이스만** 부를 수 있다.
 *
 * 세 가지 사전 검사:
 *  1. pages 나 npage 가 비었으면 -EINVAL. 그리고 디바이스가 열려 있는지
 *     확인한다. 상류 주석이 이유를 밝힌다 — 디바이스가 열려 있는 동안에는
 *     group 의 container 가 바뀌지 않으므로, 아래에서 고르는 경로가 도중에
 *     바뀌지 않는다.
 *  2. ops->dma_unmap 이 없으면 -EINVAL. 사용자가 IOAS 에서 매핑을 지울 때
 *     vendor 에게 "핀을 풀어라" 를 알릴 방법이 없으면 핀을 허용해서는 안 된다.
 *     핀만 하고 통지를 못 받으면 사용자가 매핑을 지운 뒤에도 vendor 가 그
 *     페이지를 계속 붙들어 메모리가 영영 묶인다.
 *  3. 두 백엔드 중 하나를 고른다.
 *
 * 백엔드 두 갈래:
 *  - legacy container 를 쓰면 vfio_device_container_pin_pages 로 위임한다
 *    (vfio.h 의 얇은 헬퍼를 거쳐 container.c 로).
 *  - iommufd_access 가 있으면 iommufd 쪽 API 를 쓴다. 이때
 *    - iova 가 ULONG_MAX 를 넘으면 -EINVAL. 사용자 ABI 는 64비트지만
 *      iommufd 접근 API 는 unsigned long 을 받는다.
 *    - iova 를 페이지 경계로 내림하고 길이를 페이지 단위로 준다. 상류 주석이
 *      밝히듯 VFIO 는 페이지 내 오프셋을 무시하며, 호출자가 첫 페이지에
 *      iova % PAGE_SIZE 를 더해 되찾아야 한다.
 *    - prot 의 쓰기 비트를 iommufd 쪽 플래그로 번역한다.
 *    - 성공하면 iommufd API 는 0 을 주므로, VFIO 규약에 맞춰 npage 로 바꿔
 *      돌려준다. 반환값 규약이 다른 두 API 를 잇는 부분이다.
 *  - 둘 다 아니면 -EINVAL. 결합되지 않은 디바이스에서 핀을 시도한 경우다.
 *
 * [상류 코드 관찰] 1번의 디바이스 열림 검사는 WARN_ON 으로 한 번 더 감싸여
 * 있다. 그런데 안쪽의 vfio_assert_device_open 자체가 이미 WARN_ON_ONCE 를
 * 품고 있어, 실패 시 경고가 두 번 찍힌다. 같은 파일의 vfio_dma_rw 는 같은
 * 헬퍼를 WARN_ON 없이 그대로 쓴다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: vendor 의 데이터 처리 경로, 프로세스 문맥. 페이지를 핀하므로
 * 잠들 수 있다.
 *
 * 호출 체인:
 *   emulated vendor(mdev 계열) → [vfio_pin_pages]
 *     → vfio_device_container_pin_pages 또는 iommufd_access_pin_pages
 */
/*
 * Pin contiguous user pages and return their associated host pages for local
 * domain only.
 * @device [in]  : device
 * @iova [in]    : starting IOVA of user pages to be pinned.
 * @npage [in]   : count of pages to be pinned.  This count should not
 *		   be greater than VFIO_PIN_PAGES_MAX_ENTRIES.
 * @prot [in]    : protection flags
 * @pages[out]   : array of host pages
 * Return error or number of pages pinned.
 *
 * A driver may only call this function if the vfio_device was created
 * by vfio_register_emulated_iommu_dev() due to vfio_device_container_pin_pages().
 */
int vfio_pin_pages(struct vfio_device *device, dma_addr_t iova,
		   int npage, int prot, struct page **pages)
{
	/* group->container cannot change while a vfio device is open */
	/* [한국어] 출력 배열이 없거나 페이지 수가 0 이면 요청 자체가 성립하지 않는다.
	 * 세 번째 조건은 디바이스 열림 확인이며, 바로 위 상류 주석이 그 이유를 밝힌다 —
	 * 열려 있는 동안에는 group 의 container 결합이 바뀌지 않으므로 아래에서 고른
	 * 백엔드가 도중에 달라지지 않는다. */
	if (!pages || !npage || WARN_ON(!vfio_assert_device_open(device)))
		return -EINVAL;
	/* [한국어] 사용자가 IOAS 에서 매핑을 지울 때 vendor 에게 알릴 통지 콜백이다.
	 * 이것이 없으면 핀만 하고 해제 시점을 통보받지 못해 페이지가 영영 묶인다.
	 * 그래서 콜백 없는 vendor 에게는 핀 자체를 허용하지 않는다. */
	if (!device->ops->dma_unmap)
		return -EINVAL;
	/* [한국어] legacy container 경로 여부. 두 백엔드 중 하나만 유효하다. */
	if (vfio_device_has_container(device))
		/* [한국어] container 백엔드가 사용자 페이지를 핀하고 struct page 배열을 채운다.
		 * 반환값 규약도 그쪽이 VFIO 와 같아서 그대로 올린다. */
		return vfio_device_container_pin_pages(device, iova,
						       npage, prot, pages);
	/* [한국어] iommufd 경로. */
	if (device->iommufd_access) {
		/* [한국어] iommufd API 의 반환값을 담을 지역 변수. 규약이 달라 변환이 필요하다. */
		int ret;

		/* [한국어] iommufd 접근 API 가 unsigned long 을 받으므로 넘치는 IOVA 를 먼저 거절한다. */
		if (iova > ULONG_MAX)
			return -EINVAL;
		/*
		 * VFIO ignores the sub page offset, npages is from the start of
		 * a PAGE_SIZE chunk of IOVA. The caller is expected to recover
		 * the sub page offset by doing:
		 *     pages[0] + (iova % PAGE_SIZE)
		 */
		/* [한국어] iommufd 에게 핀을 맡긴다. 인자 셋이 각각 변환을 거친다. */
		ret = iommufd_access_pin_pages(
			/* [한국어] 시작 주소를 페이지 경계로 내린다. 바로 위 상류 주석이 밝히듯 VFIO 는
			 * 페이지 내 오프셋을 버리며, 호출자가 첫 페이지에 iova % PAGE_SIZE 를 더해
			 * 되찾아야 한다. */
			device->iommufd_access, ALIGN_DOWN(iova, PAGE_SIZE),
			/* [한국어] 페이지 수를 바이트 길이로 바꾼다. iommufd API 가 길이를 바이트로 받는다. */
			npage * PAGE_SIZE, pages,
			/* [한국어] 보호 플래그 번역. IOMMU 쪽 쓰기 비트가 켜져 있으면 iommufd 쪽 쓰기
			 * 플래그로 바꾸고, 아니면 0(읽기 전용)이다. */
			(prot & IOMMU_WRITE) ? IOMMUFD_ACCESS_RW_WRITE : 0);
		/* [한국어] 실패면 오류를 그대로 올린다. */
		if (ret)
			return ret;
		/* [한국어] iommufd API 는 성공 시 0 을 주지만 이 함수의 규약은 **핀한 페이지 수**를
		 * 돌려주는 것이다. 그래서 요청한 npage 를 반환해 규약을 맞춘다. */
		return npage;
	}
	/* [한국어] 두 백엔드 어느 쪽에도 결합되지 않은 디바이스다. 핀할 주소공간이 없다. */
	return -EINVAL;
}
/* [한국어] EXPORT_SYMBOL 이며 _GPL 이 아니다. */
EXPORT_SYMBOL(vfio_pin_pages);

/* [한국어]
 * vfio_unpin_pages - vfio_pin_pages 로 핀한 페이지를 푼다
 *
 * @device: 요청하는 디바이스.
 * @iova: 풀 페이지들의 시작 IOVA. 핀할 때와 같은 값이어야 한다.
 * @npage: 풀 페이지 수. 핀할 때와 같아야 한다.
 * @return: 없음. **실패를 알릴 방법이 없다** — 해제는 되돌릴 곳이 없어서다.
 *          잘못된 인자는 WARN 으로만 드러난다.
 *
 * 무엇의 짝인가: vfio_pin_pages 다. 짝을 맞추지 않으면 게스트 페이지가 영원히
 * 핀된 채로 남아 그만큼의 물리 메모리가 회수되지 않는다. 사용자 프로세스가
 * 죽어도 핀이 남을 수 있어 사실상 메모리 누수가 된다.
 *
 * 경로 선택이 pin 과 같아야 하는 이유: container 로 핀한 것을 iommufd 로 풀
 * 수 없고 그 반대도 마찬가지다. 그래서 두 함수가 정확히 같은 순서로
 * (container 먼저, 그다음 iommufd_access) 분기한다. 그 순서가 유지되는 근거는
 * 디바이스가 열려 있는 동안 결합이 바뀌지 않는다는 사실이며, 첫 줄의
 * 열림 검사가 그것을 확인한다.
 *
 * 검사가 pin 쪽과 다른 점: 여기서는 ops->dma_unmap 부재를 WARN_ON 으로 감싼다
 * (pin 쪽은 조용히 -EINVAL). 해제 경로에서 그 상황이 벌어졌다면 pin 이
 * 어떻게 통과했는지 설명이 안 되는 모순이라 경고가 타당하다.
 *
 * 동작 과정: 열림 확인 → dma_unmap 확인 → container 면 위임하고 반환 →
 * iommufd_access 면 주소 상한 확인 후 페이지 경계로 내림해 위임 →
 * 어느 쪽도 아니면 아무 일도 하지 않고 반환.
 *
 * [상류 코드 관찰] 여기서도 열림 검사가 WARN_ON 으로 감싸여 있어,
 * vfio_assert_device_open 안의 WARN_ON_ONCE 와 합쳐 경고가 두 번 찍힌다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: vendor 의 데이터 처리 경로, 프로세스 문맥.
 *
 * 호출 체인:
 *   emulated vendor → [vfio_unpin_pages]
 *     → vfio_device_container_unpin_pages 또는 iommufd_access_unpin_pages
 */
/*
 * Unpin contiguous host pages for local domain only.
 * @device [in]  : device
 * @iova [in]    : starting address of user pages to be unpinned.
 * @npage [in]   : count of pages to be unpinned.  This count should not
 *                 be greater than VFIO_PIN_PAGES_MAX_ENTRIES.
 */
void vfio_unpin_pages(struct vfio_device *device, dma_addr_t iova, int npage)
{
	/* [한국어] 디바이스가 열려 있지 않으면 해제할 핀도 없다. 안쪽 헬퍼가 이미
	 * WARN_ON_ONCE 를 품고 있어 경고가 두 번 찍힌다(함수 블록의 상류 코드 관찰 참조). */
	if (WARN_ON(!vfio_assert_device_open(device)))
		return;
	/* [한국어] 핀을 허용하려면 있어야 했던 콜백이 지금 없다는 것은 모순이므로 경고한다.
	 * vfio_pin_pages 는 같은 조건을 조용히 -EINVAL 로 처리하는데, 해제 경로에서는
	 * 이미 통과했어야 할 조건이라 경고가 타당하다. */
	if (WARN_ON(!device->ops->dma_unmap))
		return;

	/* [한국어] 핀할 때 고른 것과 **같은 백엔드**를 같은 순서로 골라야 한다.
	 * 디바이스가 열려 있는 동안 결합이 바뀌지 않는다는 사실이 그것을 보장한다. */
	if (vfio_device_has_container(device)) {
		/* [한국어] container 백엔드에게 해제를 맡기고 */
		vfio_device_container_unpin_pages(device, iova, npage);
		/* [한국어] 곧바로 반환한다. 아래 iommufd 분기로 흘러가면 이중 해제가 된다. */
		return;
	}
	/* [한국어] iommufd 경로. */
	if (device->iommufd_access) {
		/* [한국어] 주소 상한 확인. 핀할 때 통과한 값이므로 여기서 걸리면 호출자가 다른
		 * iova 를 넘긴 것이라 경고한다. */
		if (WARN_ON(iova > ULONG_MAX))
			return;
		/* [한국어] 핀할 때와 같은 정렬 규칙으로 되돌린다. 내림한 시작 주소와 페이지 단위
		 * 길이를 넘겨야 핀과 정확히 같은 범위가 풀린다. */
		iommufd_access_unpin_pages(device->iommufd_access,
					   ALIGN_DOWN(iova, PAGE_SIZE),
					   npage * PAGE_SIZE);
		/* [한국어] 해제 완료. 반환값이 없는 함수라 실패를 알릴 방법이 없다. */
		return;
	}
}
/* [한국어] EXPORT_SYMBOL 이며 _GPL 이 아니다. */
EXPORT_SYMBOL(vfio_unpin_pages);

/* [한국어]
 * vfio_dma_rw - CPU 가 디바이스를 대신해 게스트 메모리를 읽거나 쓴다
 *
 * @device: 요청하는 디바이스.
 * @iova: 게스트 버퍼의 시작 IOVA.
 * @data: 커널 버퍼.
 * @len: 길이.
 * @write: true 면 커널 버퍼에서 게스트로 쓰고, false 면 게스트에서 읽는다.
 * @return: 0 성공, 음수 errno.
 *
 * vfio_pin_pages 와의 차이(상류 주석이 밝히는 핵심): 이것은 **진짜 디바이스
 * DMA 가 아니라 CPU 복사**다. 그래서 페이지를 핀할 필요가 없다. 복사하는
 * 동안만 접근이 유효하면 되고, 그 짧은 구간의 안전은 하위 API 가 책임진다.
 * 핀은 "디바이스가 나중에 비동기로 접근할 것" 을 위한 것이므로 여기서는
 * 불필요하고, 불필요한 핀은 메모리 압박을 만든다.
 *
 * 사전 검사: data 가 NULL 이거나 len 이 0 이하면 -EINVAL. 그리고 디바이스가
 * 열려 있어야 한다. 이 세 조건이 하나의 if 로 묶여 있다.
 *
 * kthread 자동 감지: current->mm 이 NULL 이면 커널 스레드에서 불린 것이다.
 * 사용자 주소공간이 없는 문맥이므로 하위 API 에 그 사실을 알려야 접근 방식이
 * 달라진다. 상류 주석은 이것을 "VFIO 가 역사적으로 kthread 를 자동 감지해
 * 왔다" 고 표현한다. vendor 가 자기 workqueue 에서 이 함수를 부르는 경우가
 * 있어 남아 있는 처리다.
 *
 * 두 백엔드: vfio_pin_pages 와 정확히 같은 순서로 container 와 iommufd 를
 * 고른다. iommufd 경로에서는 주소 상한을 확인하고 플래그를 조립해 넘긴다.
 * 쓰기 여부와 kthread 여부가 각각 한 비트다.
 *
 * [상류 코드 관찰] 이 함수의 열림 검사는 WARN_ON 없이
 * vfio_assert_device_open 을 그대로 쓴다. 같은 파일의 vfio_pin_pages 와
 * vfio_unpin_pages 는 같은 헬퍼를 WARN_ON 으로 한 번 더 감싼다. 세 함수가
 * 같은 조건을 서로 다르게 다룬다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 실행 컨텍스트: vendor 의 데이터 처리 경로. 프로세스 문맥이 보통이지만
 * 커널 스레드에서도 불릴 수 있고, 위 감지가 그 경우를 다룬다. 사용자 메모리를
 * 만지므로 잠들 수 있다.
 *
 * 호출 체인:
 *   emulated vendor → [vfio_dma_rw]
 *     → vfio_device_container_dma_rw 또는 iommufd_access_rw
 */
/*
 * This interface allows the CPUs to perform some sort of virtual DMA on
 * behalf of the device.
 *
 * CPUs read/write from/into a range of IOVAs pointing to user space memory
 * into/from a kernel buffer.
 *
 * As the read/write of user space memory is conducted via the CPUs and is
 * not a real device DMA, it is not necessary to pin the user space memory.
 *
 * @device [in]		: VFIO device
 * @iova [in]		: base IOVA of a user space buffer
 * @data [in]		: pointer to kernel buffer
 * @len [in]		: kernel buffer length
 * @write		: indicate read or write
 * Return error code on failure or 0 on success.
 */
int vfio_dma_rw(struct vfio_device *device, dma_addr_t iova, void *data,
		size_t len, bool write)
{
	/* [한국어] 세 조건을 한 번에 본다. 커널 버퍼가 없거나, 길이가 0 이하이거나,
	 * 디바이스가 열려 있지 않으면 진행할 수 없다. len 이 size_t 인데도 <= 0 으로
	 * 비교하는 것은 0 을 걸러 내는 효과만 있다(부호 없는 타입이라 음수가 없다).
	 * 여기서는 vfio_pin_pages 와 달리 WARN_ON 으로 감싸지 않았다. */
	if (!data || len <= 0 || !vfio_assert_device_open(device))
		return -EINVAL;

	/* [한국어] legacy container 경로인지 먼저 본다. container 는 vfio.h 의 얇은 헬퍼로만
	 * 접근하며, CONFIG_VFIO_CONTAINER 가 꺼지면 상수 false 가 되어 이 분기가
	 * 컴파일 시 사라진다. */
	if (vfio_device_has_container(device))
		/* [한국어] container 백엔드(vfio_iommu_type1)에게 복사를 맡긴다. 그쪽이 IOVA 를
		 * 사용자 가상주소로 되짚어 접근한다. */
		return vfio_device_container_dma_rw(device, iova,
						    data, len, write);

	/* [한국어] iommufd 경로. device->iommufd_access 는 emulated 디바이스가 bind 시
	 * iommufd 에 만들어 둔 데이터 평면 접근 핸들이다(iommufd.c 참조). */
	if (device->iommufd_access) {
		/* [한국어] iommufd 쪽에 넘길 동작 플래그를 0 에서 시작해 조립한다. */
		unsigned int flags = 0;

		/* [한국어] 사용자 ABI 의 IOVA 는 64비트지만 iommufd 접근 API 는 unsigned long 을
		 * 받는다. 32비트 커널에서 폭이 좁아지므로 넘치는 값을 먼저 거절한다. */
		if (iova > ULONG_MAX)
			return -EINVAL;

		/* VFIO historically tries to auto-detect a kthread */
		/* [한국어] 현재 태스크에 사용자 주소공간이 없으면 커널 스레드에서 불린 것이다.
		 * vendor 가 자기 workqueue 에서 이 함수를 부르는 경우가 있어 필요한 판별이며,
		 * 상류 주석이 이것을 역사적 자동 감지라고 부른다. */
		if (!current->mm)
			/* [한국어] 커널 스레드임을 하위 API 에 알린다. 접근 방식(사용자 페이지를 어느
			 * 주소공간에서 찾을지)이 달라진다. */
			flags |= IOMMUFD_ACCESS_RW_KTHREAD;
		/* [한국어] 쓰기 요청이면 */
		if (write)
			/* [한국어] 쓰기 플래그를 더한다. 읽기면 이 비트가 꺼진 채로 남는다. */
			flags |= IOMMUFD_ACCESS_RW_WRITE;
		/* [한국어] iommufd 에게 실제 복사를 맡긴다. 이 API 는 성공 시 0 을 돌려주므로
		 * 반환값을 그대로 올려도 이 함수의 규약(0 이 성공)과 맞는다. */
		return iommufd_access_rw(device->iommufd_access, iova, data,
					 len, flags);
	}
	/* [한국어] container 도 iommufd_access 도 없는 디바이스에서 불렸다는 뜻이다.
	 * 결합되지 않은 상태라 접근할 주소공간 자체가 없어 -EINVAL 이다. */
	return -EINVAL;
}
/* [한국어] EXPORT_SYMBOL 이며 _GPL 이 아니다. 이 파일의 비-GPL export 여섯 개 중
 * 마지막이다. */
EXPORT_SYMBOL(vfio_dma_rw);

/* [한국어]
 * vfio_init - vfio 모듈 적재 시 전역 자원을 만든다
 *
 * @: 인자 없음.
 * @return: 0 성공, 음수 errno 실패(모듈 적재 실패로 이어진다).
 *
 * 무엇을 만드는가(그리고 각각을 vfio_cleanup 의 무엇이 되돌리는가):
 *  1. ida_init — device index 할당기 초기화. 되돌림은 ida_destroy.
 *  2. vfio_group_init — group.c 의 class 와 chrdev 영역
 *     (group.c:910 이 MINORMASK + 1 개의 minor 를 잡는다).
 *     되돌림은 vfio_group_cleanup.
 *  3. vfio_virqfd_init — virqfd.c 의 정리용 workqueue. 되돌림은 vfio_virqfd_exit.
 *  4. class_create("vfio-dev") — /sys/class/vfio-dev 를 만든다. 이 class 가
 *     각 디바이스의 sysfs 노드를 담고, device_cdev.c:295 의 devnode 콜백이
 *     /dev/vfio/devices/vfio<N> 라는 노드 경로를 만들어 낸다.
 *     되돌림은 class_destroy.
 *  5. vfio_cdev_init — 그 class 에 devnode 콜백을 걸고 device 용 chrdev 영역을
 *     잡는다. 되돌림은 vfio_cdev_cleanup.
 *  6. vfio_debugfs_create_root — 진단용 루트. 되돌림은 vfio_debugfs_remove_root.
 *  7. 버전 배너 출력.
 *
 * 에러 라벨이 역순으로 쌓이는 구조: err_alloc_dev_chrdev → err_dev_class →
 * err_virqfd 순으로 fall-through 하며 그때까지 잡은 것만 되돌린다. 커널의
 * 표준 unwinding 형태다. class_destroy 뒤에 포인터를 NULL 로 비우는 것은,
 * 해제된 포인터가 남아 나중에 잘못 쓰이는 것을 막기 위함이다.
 *
 * 되돌리지 않는 것 하나: 1번의 ida_init 은 어떤 에러 라벨에서도 되돌리지
 * 않는다. ida_init 은 구조체를 비우는 것뿐이라 실패 시 남겨 두어도 자원이
 * 새지 않기 때문이며, vfio_cleanup 의 ida_destroy 는 정상 언로드 경로에만 있다.
 *
 * Kconfig 로 꺼진 기능은 어떻게 되는가: vfio_group_init, vfio_virqfd_init,
 * vfio_cdev_init, vfio_debugfs_create_root 는 모두 drivers/vfio/vfio.h 에
 * "성공을 반환하는 빈 stub" 이 준비돼 있다. 그래서 이 함수는 #ifdef 없이
 * 한 가지 모습으로 유지된다.
 *
 * 실행 컨텍스트: 모듈 적재, 프로세스 문맥. 잠들 수 있다.
 *
 * 호출 체인:
 *   insmod / 부팅 시 모듈 초기화 → module_init → [vfio_init]
 *     → ida_init / vfio_group_init / vfio_virqfd_init / class_create /
 *   vfio_cdev_init / vfio_debugfs_create_root
 */
/*
 * Module/class support
 */
static int __init vfio_init(void)
{
	int ret;

	/* [한국어] device index 할당기를 초기화한다. 이 한 줄이 실패할 수 없어
	 * 아래 어떤 에러 라벨에서도 되돌리지 않는다. */
	ida_init(&vfio.device_ida);

	/* [한국어] group 쪽 전역(class, /dev/vfio/<gid> 용 chrdev 영역)을 만든다.
	 * CONFIG_VFIO_GROUP 이 꺼져 있으면 vfio.h 의 stub 이 0 을 돌려준다. */
	ret = vfio_group_init();
	/* [한국어] 실패하면 여기까지 잡은 것이 ida 뿐이라 되돌릴 것 없이 곧장 반환한다. */
	if (ret)
		return ret;

	/* [한국어] virqfd 의 정리용 workqueue 를 만든다. eventfd 기반 인터럽트 배선을
	 * 비동기로 해제하는 데 쓰인다. */
	ret = vfio_virqfd_init();
	/* [한국어] 실패하면 err_virqfd 로 가서 group 정리만 되돌린다. */
	if (ret)
		goto err_virqfd;

	/* /sys/class/vfio-dev/vfioX */
	/* [한국어] device class 를 만든다. 이름이 sysfs 경로 /sys/class/vfio-dev 가 되고,
	 * 바로 위 상류 주석이 그 아래 노드 이름 형태를 적어 두었다. */
	vfio.device_class = class_create("vfio-dev");
	/* [한국어] class_create 는 실패 시 ERR_PTR 을 돌려주므로 IS_ERR 로 검사한다.
	 * NULL 검사가 아니라는 점이 중요하다. */
	if (IS_ERR(vfio.device_class)) {
		/* [한국어] 오류 코드를 꺼내 ret 에 담는다. 포인터에 실린 errno 를 꺼내는 표준 방법이다. */
		ret = PTR_ERR(vfio.device_class);
		goto err_dev_class;
	}

	/* [한국어] 그 class 에 devnode 콜백을 걸고 device 용 chrdev 영역을 잡는다.
	 * 콜백이 /dev/vfio/devices/vfio<N> 라는 노드 경로를 만들어 낸다
	 * (device_cdev.c:295~305). */
	ret = vfio_cdev_init(vfio.device_class);
	/* [한국어] 실패하면 err_alloc_dev_chrdev 로 가서 class 부터 되돌린다. */
	if (ret)
		goto err_alloc_dev_chrdev;

	/* [한국어] 진단용 debugfs 루트를 만든다. 실패해도 반환값이 없다 — 진단 기능은
	 * 없어도 동작에 지장이 없어 오류를 전파하지 않는 설계다. */
	vfio_debugfs_create_root();
	/* [한국어] 적재 성공 배너. dmesg 에 설명과 버전이 한 줄 남는다. */
	pr_info(DRIVER_DESC " version: " DRIVER_VERSION "\n");
	return 0;

err_alloc_dev_chrdev:
	/* [한국어] cdev 등록 실패 지점의 되돌림 — class 파괴. */
	class_destroy(vfio.device_class);
	/* [한국어] 해제한 포인터를 비운다. 적재 실패 후 다시 적재를 시도할 때
	 * 남은 쓰레기 값을 보지 않도록 한다. */
	vfio.device_class = NULL;
err_dev_class:
	/* [한국어] class 생성 실패 지점의 되돌림 — virqfd workqueue 파괴. */
	vfio_virqfd_exit();
err_virqfd:
	/* [한국어] virqfd 초기화 실패 지점의 되돌림 — group 전역 정리.
	 * 라벨들이 아래로 흐르며 누적 되돌림을 만드는 커널 표준 형태다. */
	vfio_group_cleanup();
	return ret;
}

/* [한국어]
 * vfio_cleanup - vfio 모듈 언로드 시 전역 자원을 모두 되돌린다
 *
 * @: 인자 없음.
 * @return: 없음.
 *
 * 무엇의 짝인가: vfio_init 이 만든 여섯 가지 전부다. 여기 도달했다는 것은
 * 등록된 vfio_device 가 하나도 없다는 뜻이다 — 남아 있으면 vendor 모듈이
 * 아직 로드돼 있고, 그 모듈이 vfio 모듈을 참조하므로 rmmod 가 거부된다.
 * 그래서 이 함수는 디바이스 개수를 확인하지 않는다.
 *
 * 순서: debugfs 루트 제거 → ida 파괴 → cdev 영역 반납 → class 파괴 →
 * virqfd workqueue 파괴 → group 정리 → device_set xarray 파괴.
 *
 * [상류 코드 관찰] 이 순서는 vfio_init 의 정확한 역순이 아니다. 역순이라면
 * debugfs, cdev, class, virqfd, group, ida 여야 하는데 ida_destroy 가 두 번째로
 * 앞당겨져 cdev 반납보다 먼저 실행된다. 이 시점에는 할당된 index 가 남아 있지
 * 않아 실제 문제는 생기지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는
 * 고치지 않았다.
 *
 * 마지막의 xa_destroy 는 vfio_init 에 대응하는 짝이 없다. device_set xarray 는
 * 파일 정적 DEFINE_XARRAY 로 컴파일 시 초기화되므로 만들 필요가 없고,
 * 언로드 때 남은 항목만 정리하면 된다. 정상 상태라면 이미 비어 있다 —
 * 마지막 디바이스가 사라질 때 vfio_release_device_set 이 지웠기 때문이다.
 *
 * class 포인터를 NULL 로 비우는 이유: 해제된 포인터가 전역에 남아 있으면
 * 이후 코드가 잘못 참조할 수 있다. vfio_init 의 실패 경로도 같은 처리를 한다.
 *
 * 실행 컨텍스트: 모듈 언로드, 프로세스 문맥.
 *
 * 호출 체인:
 *   rmmod → module_exit → [vfio_cleanup]
 *     → vfio_debugfs_remove_root / ida_destroy / vfio_cdev_cleanup /
 *   class_destroy / vfio_virqfd_exit / vfio_group_cleanup / xa_destroy
 */
static void __exit vfio_cleanup(void)
{
	/* [한국어] debugfs 루트를 먼저 없앤다. 진단 인터페이스가 사라진 상태에서
	 * 나머지를 정리해야 사용자가 해제 중인 객체를 들여다볼 수 없다.
	 * CONFIG_VFIO_DEBUGFS 가 꺼져 있으면 vfio.h 의 빈 stub 이다. */
	vfio_debugfs_remove_root();
	/* [한국어] device index 할당기를 파괴한다. vfio_init 의 ida_init 짝이며,
	 * 남아 있는 index 가 있으면 그 메모리도 함께 해제한다. */
	ida_destroy(&vfio.device_ida);
	/* [한국어] device 용 chrdev 영역(MINORMASK + 1 개의 minor)을 반납한다.
	 * vfio_cdev_init 의 alloc_chrdev_region 짝이다(device_cdev.c:304). */
	vfio_cdev_cleanup();
	/* [한국어] device class 를 파괴한다. class_create 짝이며 /sys/class/vfio-dev 가 사라진다. */
	class_destroy(vfio.device_class);
	/* [한국어] 해제된 포인터를 전역에 남기지 않도록 비운다. 이후 실수로 참조하면
	 * 곧바로 NULL 역참조로 드러나 조용한 use-after-free 보다 낫다. */
	vfio.device_class = NULL;
	/* [한국어] virqfd 정리용 workqueue 를 파괴한다. vfio_virqfd_init 짝이다.
	 * 남은 작업이 있으면 그것들이 끝날 때까지 기다린 뒤 없앤다. */
	vfio_virqfd_exit();
	/* [한국어] group 쪽 class 와 chrdev 영역을 정리한다. vfio_group_init 짝이다. */
	vfio_group_cleanup();
	/* [한국어] device_set xarray 에 남은 항목을 정리한다. 정상 상태라면 이미 비어 있다.
	 * DEFINE_XARRAY 로 정적 초기화된 객체라 vfio_init 쪽에 대응하는 생성 호출이
	 * 없는 유일한 항목이다. */
	xa_destroy(&vfio_device_set_xa);
}

module_init(vfio_init);
module_exit(vfio_cleanup);

MODULE_IMPORT_NS("IOMMUFD");
/* [한국어] MODULE_VERSION 은 modinfo 와 /sys/module/vfio/version 에 노출된다.
 * 위에서 정의한 DRIVER_VERSION 문자열을 그대로 쓴다. */
MODULE_VERSION(DRIVER_VERSION);
/* [한국어] 라이선스 선언. 이 값이 GPL 계열이어야 EXPORT_SYMBOL_GPL 로 내보낸
 * 다른 커널 심볼을 이 모듈이 쓸 수 있다. */
MODULE_LICENSE("GPL v2");
/* [한국어] 원저자 표기. 위 DRIVER_AUTHOR 문자열. */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] 모듈 설명. 위 DRIVER_DESC 문자열이며 부팅 배너와 같은 내용이다. */
MODULE_DESCRIPTION(DRIVER_DESC);
/* [한국어] soft 의존성 선언 — 이 모듈을 적재한 **뒤에** 나열한 모듈들도 함께 적재하라고
 * modprobe 에 알린다. legacy container 모드의 IOMMU backend 두 종류이며,
 * 코어가 이들을 직접 참조하지 않으므로(backend 가 스스로 등록한다) 하드
 * 의존성이 아니다. 이것이 없으면 사용자가 VFIO_SET_IOMMU 를 부를 때
 * backend 가 아직 없어 실패할 수 있다. */
MODULE_SOFTDEP("post: vfio_iommu_type1 vfio_iommu_spapr_tce");
