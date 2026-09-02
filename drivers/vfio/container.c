// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *
 * VFIO container (/dev/vfio/vfio)
 */
/*
 * [한국어 설명] VFIO 1세대 container 계층 — /dev/vfio/vfio 와 IOMMU 백엔드 디스패치
 * (drivers/vfio/container.c)
 *
 * 이 블록 안에서 다른 파일을 가리킬 때만 줄 번호를 적었다. 이 파일 자신은 주석이
 * 붙으며 줄 번호가 계속 바뀌므로 함수 이름으로만 가리킨다. group.c 는 함수 이름으로만
 * 가리킨다(그 파일도 함께 주석 중이다).
 *
 * === 파일의 역할 ===
 * VFIO 의 세 층(device / group / container) 가운데 **가장 바깥층인 container 를
 * 통째로** 구현한다. container 는 **IOVA 주소공간 하나**이며, 여러 group 이 붙어
 * 그 주소공간을 공유한다. 이 파일이 맡는 일은 네 가지다.
 * (1) **컨테이너 객체의 수명** — /dev/vfio/vfio 를 열 때 만들어지고, 컨테이너 fd 와
 * 붙어 있는 group 들이 각각 kref 를 쥐다가 마지막 하나가 빠질 때 해제된다.
 * 상류 주석대로 컨테이너/group/device 를 **어떤 순서로 닫아도** 안전해야 한다.
 * (2) **IOMMU 백엔드 등록과 선택** — vfio_register_iommu_driver 로 등록된 후보들
 * (type1, spapr-tce, 그리고 이 파일의 noiommu) 가운데 VFIO_SET_IOMMU 가 요청한 타입을
 * 지원하는 것을 골라 인스턴스를 만들고 컨테이너에 물린다.
 * (3) **백엔드로의 dispatch** — 코어가 직접 답하는 세 ioctl 을 뺀 나머지 전부와,
 * group 결합/해제, mdev 용 페이지 핀, DMA 읽기/쓰기를 vfio_iommu_driver_ops 의
 * 함수 포인터로 넘긴다. 그래서 VFIO 코어는 VFIO_IOMMU_MAP_DMA 의 인자 형식조차 알
 * 필요가 없다.
 * (4) **noiommu 백엔드의 구현** — 격리도 번역도 제공하지 않는 빈 vtable.
 * 이 파일 안에 그 백엔드가 통째로 들어 있다.
 *
 * 반대로 이 파일이 **하지 않는** 일: 실제 IOVA 매핑(페이지 테이블 프로그래밍)은
 * vfio_iommu_type1.c 가, group 객체의 생성과 소멸은 group.c 가, 디바이스 등록과
 * fd 공통 동작은 vfio_main.c 가 한다. struct vfio_container 의 정의가 이 파일 안에만
 * 있다는 점이 그 경계를 강제한다 — 바깥 세계는 vfio_container_from_file 이 돌려주는
 * 불투명 포인터로만 컨테이너를 만진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 1세대 ABI 에서 이 파일은 맨 바깥 껍질이다.
 *
 *   container (/dev/vfio/vfio)          ... **이 파일**, IOVA 주소공간 1개
 *       └ group (/dev/vfio/<gid>)       ... group.c, IOMMU 격리 단위
 *           └ device fd (익명 inode)    ... vfio_main.c 의 공용 fops
 *
 *   그 아래로 백엔드가 붙는다.
 *   container.iommu_driver ──ops──▶ vfio_iommu_type1.c (진짜 IOMMU domain)
 *                            └────▶ 이 파일의 vfio_noiommu_ops (아무것도 안 함)
 *
 * 사용자 공간(QEMU, DPDK, SPDK)이 NVMe 컨트롤러를 직접 모는 절차에서 이 파일이 받는
 * 것은 2, 5, 6 번이다.
 *   1. driver_override 로 vfio-pci 바인딩 → vfio_main.c → group.c 가 group 노드 생성
 *   2. open("/dev/vfio/vfio")                  → **이 파일의 vfio_fops_open**
 *      : 이 시점의 컨테이너는 group 도 백엔드도 없는 **비특권 빈 껍데기**다.
 *   3. open("/dev/vfio/<gid>")                 → group.c
 *   4. ioctl(group, VFIO_GROUP_SET_CONTAINER)  → group.c
 *      → **이 파일의 vfio_container_attach_group** 이 DMA 소유권을 커널에서 빼앗는다.
 *   5. ioctl(container, VFIO_SET_IOMMU, TYPE1v2)
 *      → **이 파일의 vfio_ioctl_set_iommu** → 후보 순회 → ops->open
 *      → vfio_iommu_type1_open
 *   6. ioctl(container, VFIO_IOMMU_MAP_DMA)
 *      → **이 파일의 vfio_fops_unl_ioctl 의 default 갈래** → ops->ioctl
 *      → vfio_iommu_type1_ioctl → vfio_dma_do_map
 *      : 사용자 hugepage 를 핀해 IOVA 에 매핑한다. 이후 NVMe 가 PRP 와 SGL 에 싣는
 *        주소가 전부 이 IOVA 다.
 *   7. ioctl(group, VFIO_GROUP_GET_DEVICE_FD) → group.c → 첫 open 이
 *      **이 파일의 vfio_group_use_container** 와 vfio_device_container_register 를 부른다.
 *
 * 실행 컨텍스트는 전부 **호스트 커널 프로세스 문맥**이다. 인터럽트 문맥에서 도는 코드가
 * 이 파일에는 없다 — 뮤텍스와 rw_semaphore 를 잡고 잠들 수 있으며, GFP_KERNEL 할당을
 * 한다. 원자 구간(spinlock)은 하나도 없다.
 *
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/vfio.h (이미 주석 완료)
 *      : struct vfio_iommu_driver_ops(vfio.h:392)와 struct vfio_iommu_driver(vfio.h:452)의
 *        정의처. 이 파일이 구현하는 함수들의 프로토타입도 여기(vfio.h:457~478)에 있고,
 *        struct vfio_container 는 전방 선언(vfio.h:94)만 있다.
 *        CONFIG_VFIO_CONTAINER 가 꺼지면 vfio.h:481~537 의 stub 이 대신 들어가
 *        group.c 와 vfio_main.c 가 #ifdef 없이 컴파일된다.
 *  - drivers/vfio/group.c
 *      : 이 파일의 유일한 상위 호출자. vfio_container_from_file 과
 *        vfio_container_attach_group(SET_CONTAINER), vfio_group_detach_container
 *        (UNSET_CONTAINER / group fd 해제 / 디바이스 제거),
 *        vfio_group_use_container 와 vfio_group_unuse_container(첫 open / 마지막 close),
 *        vfio_device_container_register 와 unregister,
 *        vfio_container_init 과 vfio_container_cleanup(모듈 init/exit)을 부른다.
 *        반대로 이 파일은 group.c 의 상태(group->container, container_users,
 *        opened_file, iommu_group, type)를 직접 읽고 쓴다 — 두 파일은 한 쌍이다.
 *  - drivers/vfio/vfio_iommu_type1.c (이미 주석 완료)
 *      : 이 파일이 dispatch 하는 주 백엔드. 그 파일의 vtable
 *        vfio_iommu_driver_ops_type1 이 이 파일의 모든 ops-> 호출의 실제 목적지다.
 *        대응은 다음과 같다.
 *          ops->open           → vfio_iommu_type1_open
 *          ops->release        → vfio_iommu_type1_release
 *          ops->ioctl          → vfio_iommu_type1_ioctl
 *          ops->attach_group   → vfio_iommu_type1_attach_group
 *          ops->detach_group   → vfio_iommu_type1_detach_group
 *          ops->pin_pages      → vfio_iommu_type1_pin_pages
 *          ops->unpin_pages    → vfio_iommu_type1_unpin_pages
 *          ops->register_device→ vfio_iommu_type1_register_device
 *          ops->unregister_device → vfio_iommu_type1_unregister_device
 *          ops->dma_rw         → vfio_iommu_type1_dma_rw
 *        그 파일은 vfio_iommu_type1_init 에서 이 파일의 vfio_register_iommu_driver 를
 *        불러 자기를 등록한다.
 *  - drivers/vfio/vfio_iommu_spapr_tce.c
 *      : 같은 방식으로 등록되는 POWER 계열 백엔드. 이 파일은 그것을 이름으로 알지 못하고
 *        목록의 한 후보로만 다룬다.
 *  - drivers/vfio/vfio_main.c (이미 주석 완료)
 *      : 이 파일의 vfio_device_container_pin_pages / unpin_pages / dma_rw 를
 *        vfio_pin_pages, vfio_unpin_pages, vfio_dma_rw 에서 부른다. 그 세 함수는
 *        group.c 의 vfio_device_has_container 로 경로를 먼저 가른다.
 *  - include/uapi/linux/vfio.h
 *      : VFIO_API_VERSION(:19), VFIO_TYPE1_IOMMU(:26), VFIO_NOIOMMU_IOMMU(:48),
 *        VFIO_GET_API_VERSION(:112), VFIO_CHECK_EXTENSION(:121), VFIO_SET_IOMMU(:134).
 *  - drivers/iommu (이 트리에 없음)
 *      : iommu_group_claim_dma_owner 와 iommu_group_release_dma_owner 의 구현처.
 *        이 두 함수가 격리의 실체인데 내부 동작은 이 트리에서 확인 못 함.
 *  - include/linux/miscdevice.h (이 트리에 없음)
 *      : VFIO_MINOR 값과 misc_register 의 구현. 값은 확인 못 함.
 *
 * === 주요 함수/구조체 요약 ===
 *  - struct vfio_container      : kref, group_list, group_lock, iommu_driver,
 *                                 iommu_data, noiommu 여섯 필드. **정의가 이 파일
 *                                 안에만 있어** 바깥은 불투명 포인터로만 다룬다.
 *  - struct vfio(파일 정적)     : 등록된 백엔드 목록과 그 뮤텍스 두 필드.
 *  - vfio_noiommu_ops           : 격리 없는 백엔드의 vtable. open 이 권한만 검사하고,
 *                                 attach/detach 가 빈 함수이며, pin/unpin/dma_rw 슬롯이
 *                                 아예 없다.
 *  - vfio_register_iommu_driver : 백엔드가 후보 목록에 자기를 올리는 창구.
 *  - vfio_ioctl_set_iommu       : 후보를 순회해 백엔드를 고르고 인스턴스를 만들어
 *                                 컨테이너에 물리는 핵심.
 *  - vfio_fops_unl_ioctl        : 세 명령을 직접 처리하고 나머지는 백엔드로 통과시킨다.
 *  - vfio_container_attach_group / vfio_group_detach_container
 *                               : group 과 컨테이너를 잇고 끊는 짝. DMA 소유권 획득과
 *                                 반환, 컨테이너 참조, 마지막 group 의 백엔드 해제까지.
 *  - vfio_group_use_container / _unuse_container
 *                               : device fd 하나가 컨테이너를 쓰기 시작/끝냄을 세는 짝.
 *                                 **get_file / fput 으로 group fd 참조까지 함께 움직인다.**
 *  - vfio_container_from_file   : f_op 주소 비교로 fd 종류를 판정하는 위조 불가 검사.
 *
 * === 백엔드 dispatch 지도 (이 파일의 핵심) ===
 * 이 파일에서 함수 포인터를 통해 백엔드로 나가는 지점은 다음과 같다. 괄호 안은
 * type1 백엔드에서의 실제 목적지 함수 이름이다.
 *
 *   [백엔드 선택 단계 — 인스턴스가 아직 없음]
 *    vfio_container_ioctl_check_extension
 *      → ops->ioctl(NULL, VFIO_CHECK_EXTENSION, arg)   (vfio_iommu_type1_ioctl)
 *        : 등록된 모든 후보에게 "이 확장을 지원하느냐" 를 묻는다. iommu_data 가 NULL 인
 *          이유는 아직 인스턴스가 없기 때문이고, 그 갈래는 인스턴스를 만지지 않는다.
 *    vfio_ioctl_set_iommu
 *      → ops->ioctl(NULL, VFIO_CHECK_EXTENSION, arg)   (vfio_iommu_type1_ioctl)
 *      → ops->open(arg)                                 (vfio_iommu_type1_open)
 *      → ops->release(data)  [결합 실패 되감기]         (vfio_iommu_type1_release)
 *
 *   [인스턴스가 정해진 뒤]
 *    vfio_container_ioctl_check_extension
 *      → ops->ioctl(iommu_data, VFIO_CHECK_EXTENSION, arg) (vfio_iommu_type1_ioctl)
 *    vfio_fops_unl_ioctl 의 default
 *      → ops->ioctl(iommu_data, cmd, arg)               (vfio_iommu_type1_ioctl)
 *        : **가장 뜨거운 경로.** MAP_DMA, UNMAP_DMA, GET_INFO, DIRTY_PAGES 가 모두
 *          여기를 지난다.
 *    __vfio_container_attach_groups (SET_IOMMU 시 일괄 결합)
 *      → ops->attach_group                              (vfio_iommu_type1_attach_group)
 *      → ops->detach_group  [되감기]                    (vfio_iommu_type1_detach_group)
 *    vfio_container_attach_group (SET_CONTAINER 시 개별 결합)
 *      → ops->attach_group                              (vfio_iommu_type1_attach_group)
 *    vfio_group_detach_container
 *      → ops->detach_group                              (vfio_iommu_type1_detach_group)
 *      → ops->release  [마지막 group 일 때]             (vfio_iommu_type1_release)
 *    vfio_device_container_register / _unregister
 *      → ops->register_device / ops->unregister_device
 *        (vfio_iommu_type1_register_device / _unregister_device)
 *    vfio_device_container_pin_pages / _unpin_pages / _dma_rw
 *      → ops->pin_pages / ops->unpin_pages / ops->dma_rw
 *        (vfio_iommu_type1_pin_pages / _unpin_pages / _dma_rw)
 *
 * NULL 검사 관례: pin_pages 와 dma_rw 는 슬롯이 NULL 인지 확인하고 -ENOTTY 를 주지만,
 * unpin_pages 는 확인 없이 역참조한다(그 함수 주석의 [상류 코드 관찰] 참조).
 * attach_group / detach_group / release / open / ioctl 은 모든 백엔드가 반드시
 * 제공한다는 전제로 검사하지 않는다 — noiommu 백엔드가 하는 일 없는 빈 함수라도
 * 그 다섯 슬롯을 채워 두는 이유가 이것이다.
 *
 * === refcount 와 수명 지도 ===
 * vfio_main.c 의 지도(device.kref / device->refcount / open_count / 모듈 참조)와
 * group.c 의 지도(group->dev.kref / drivers / iommu_group 참조)에 이 파일이 더하는
 * 것은 넷이다.
 *
 *  (a) container->kref
 *      : 컨테이너 메모리의 수명. vfio_fops_open 의 kref_init 이 1(컨테이너 fd 의 몫)로
 *        시작하고, 붙는 group 마다 vfio_container_attach_group 의 vfio_container_get 이
 *        +1 한다. 놓는 곳은 vfio_fops_release 와 vfio_group_detach_container 두 곳이며,
 *        **어느 쪽이 나중이든 마지막 하나가 vfio_container_release 로 kfree 한다.**
 *        이 구조가 "컨테이너/group/device 를 어떤 순서로 닫아도 안전" 을 만든다.
 *        빠뜨리면 group 이 붙어 있는데 fd 를 닫는 순간 use-after-free 가 된다.
 *
 *  (b) group->container_users 와 group fd 참조 (한 쌍으로 움직인다)
 *      : vfio_container_attach_group 이 1 로 시작한다(group 자신의 몫).
 *        device fd 가 하나 열릴 때마다 vfio_group_use_container 가 +1 하면서 **동시에**
 *        get_file(group->opened_file) 로 group 파일 참조를 하나 건다. 짝은
 *        vfio_group_unuse_container 의 -1 과 fput.
 *        이 get_file 이 **이 파일에서 가장 중요한 한 줄**이다 — device fd 가 살아 있는
 *        동안 group fd 의 release 가 불리지 않게 만들어, group.c 의
 *        vfio_group_fops_release 가 "여기 왔다면 열린 device 가 없다" 고 단정할 수 있게
 *        한다. group.c 의 UNSET_CONTAINER 가 "1 이 아니면 -EBUSY" 로 거절하는 것도
 *        같은 카운터를 본다. fput 을 빠뜨리면 group fd 가 영원히 살아남아 컨테이너와
 *        IOVA 매핑, 핀된 페이지가 모두 샌다.
 *
 *  (c) 백엔드 모듈 참조 (try_module_get / module_put)
 *      : 세 가지 쓰임이 있다.
 *        - vfio_container_ioctl_check_extension : 콜백 한 번 동안만 잡았다 즉시 놓는다.
 *        - vfio_ioctl_set_iommu 의 후보 시험 : 실패하면 그 자리에서 놓는다.
 *        - vfio_ioctl_set_iommu 의 성공 : **놓지 않고 컨테이너가 물려받는다.**
 *          짝은 vfio_group_detach_container 가 마지막 group 을 뗄 때의 module_put 이다.
 *          이 참조가 있어 컨테이너가 백엔드를 쓰는 동안 rmmod 자체가 되지 않고,
 *          그래서 vfio_unregister_iommu_driver 가 사용 중인 백엔드를 지우는 일이 없다.
 *
 *  (d) IOMMU group 의 DMA 소유권 (참조가 아니라 배타 소유권)
 *      : vfio_container_attach_group 의 iommu_group_claim_dma_owner 가 가져오고,
 *        vfio_group_detach_container 의 iommu_group_release_dma_owner 가 돌려준다.
 *        가져오는 데 성공하면 group 안 모든 디바이스에서 커널 드라이버가 쫓겨나고
 *        이후 바인딩도 막힌다 — **이것이 VFIO 격리의 실체다.**
 *        attach 도중 백엔드 결합이 실패하면 그 자리에서 되돌린다. 빠뜨리면 group 이
 *        영원히 잠겨 커널 드라이버도 다른 컨테이너도 그 디바이스를 쓸 수 없다.
 *
 *  락 2종과 순서:
 *   - vfio.iommu_drivers_lock (mutex) : 등록된 백엔드 목록.
 *   - container->group_lock (rw_semaphore) : group_list, iommu_driver, iommu_data,
 *     noiommu. 조회만 하는 CHECK_EXTENSION 은 down_read 로 동시 접근을 허용하고,
 *     상태를 바꾸는 SET_IOMMU 와 attach/detach 는 down_write 를 쓴다.
 *   락 순서는 group->group_lock(group.c) → container->group_lock →
 *   vfio.iommu_drivers_lock 이며 역순은 이 트리에 없다.
 *   vfio_fops_unl_ioctl 의 default 갈래와 pin/unpin/dma_rw 헬퍼는 **락 없이**
 *   iommu_driver 를 읽는다 — 호출자가 device fd 를 쥐고 있어 그 사이 백엔드가 바뀔 수
 *   없다는 전제 위에 서 있다.
 *
 * === noiommu 경로 — 무엇을 포기하고 무엇이 막는가 ===
 * noiommu 백엔드의 구현이 이 파일 안에 통째로 들어 있다(vfio_noiommu_open,
 * vfio_noiommu_release, vfio_noiommu_ioctl, vfio_noiommu_attach_group,
 * vfio_noiommu_detach_group 과 그 vtable vfio_noiommu_ops).
 *
 * 포기하는 것:
 *  - **IOVA 번역이 없다.** vfio_noiommu_attach_group 이 빈 함수라는 것이 그 실체다.
 *    type1 이라면 여기서 IOMMU domain 을 만들고 iommu_attach_group 을 부르지만,
 *    noiommu 는 할 일이 없다. 디바이스가 찍는 DMA 주소가 곧 물리 주소가 되고,
 *    사용자 공간이 디바이스를 시켜 커널 메모리 어디든 읽고 쓸 수 있다.
 *  - **MAP_DMA 가 없다.** vfio_noiommu_ioctl 은 VFIO_CHECK_EXTENSION 외의 모든 명령에
 *    -ENOTTY 를 준다. 번역이 없으니 매핑할 것도 없다.
 *    상류 uapi 주석(include/uapi/linux/vfio.h:45~47)이 그 사실을 명시한다.
 *  - **mdev 계열 헬퍼가 없다.** vfio_noiommu_ops 는 pin_pages, unpin_pages,
 *    register_device, unregister_device, dma_rw, group_iommu_domain 슬롯을 아예
 *    초기화하지 않아 NULL 이다. 그래서 vfio_device_container_pin_pages 와
 *    vfio_device_container_dma_rw 가 -ENOTTY 로 실패한다.
 *  - **가상머신에 할당할 수 없다.** 게스트 물리주소를 호스트 물리주소로 바꿀 번역이
 *    없기 때문이다.
 *  - **커널이 taint 된다.** taint 를 찍는 곳은 group.c 의 vfio_group_find_or_alloc 이다.
 *
 * 이것을 막는 게이트는 **CAP_SYS_RAWIO** 하나이며, 경로마다 반복해서 걸려 있다.
 *  - 시스템 수준 opt-in : 모듈 파라미터 enable_unsafe_noiommu_mode(vfio_main.c 의
 *    vfio_noiommu). vfio_noiommu_ioctl 이 그 값을 보고 CHECK_EXTENSION 에 답하므로,
 *    꺼져 있으면 SET_IOMMU 에서 이 백엔드가 아예 선택되지 않는다.
 *    Kconfig CONFIG_VFIO_NOIOMMU 로 등록 자체를 뺄 수도 있다(vfio_container_init).
 *  - 이 파일: vfio_noiommu_open 이 VFIO_SET_IOMMU 때,
 *    vfio_container_attach_group 이 SET_CONTAINER 때,
 *    vfio_group_use_container 가 첫 device open 때 각각 확인한다.
 *  - group.c: vfio_group_fops_open 이 노드 열기 때,
 *    vfio_df_group_open 이 iommufd 호환 경로에서 확인한다.
 *  - 섞임 방지: vfio_iommu_driver_allowed 가 "noiommu 컨테이너만 vfio-noiommu 를 쓰고,
 *    vfio-noiommu 는 noiommu 컨테이너에서만 쓴다" 를 강제하고,
 *    vfio_container_attach_group 이 진짜 group 과 가짜 group 의 혼용을 -EPERM 으로
 *    거절한다. 진짜 격리와 가짜 격리가 한 IOVA 주소공간을 공유하면 격리 보증 자체가
 *    무의미해지기 때문이다.
 */
/* [한국어] struct file 과 fget/fput 계열. 이 파일은 컨테이너 fd 를 file 추상으로만 다루며,
 * vfio_container_from_file 이 file->f_op 를 신원 증명으로 쓴다. 또 group fd 참조를
 * get_file / fput 으로 잡고 놓는 코드도 여기 있다. 헤더 실물은 이 트리에 없다. */
#include <linux/file.h>
/* [한국어] kzalloc_obj, kfree — vfio_container 와 vfio_iommu_driver 두 객체를 이 두 함수로만 만들고 없앤다. */
#include <linux/slab.h>
/* [한국어] struct file_operations, struct inode. 컨테이너 노드의 fops 정의에 필요하다. */
#include <linux/fs.h>
/* [한국어] capable() 과 CAP_SYS_RAWIO. noiommu 를 막는 게이트가 이 파일에만 세 곳 있다
 * (vfio_noiommu_open, vfio_container_attach_group, vfio_group_use_container). */
#include <linux/capability.h>
/* [한국어] iommu_group_claim_dma_owner 와 iommu_group_release_dma_owner, struct iommu_group.
 * group 을 컨테이너에 붙일 때 커널 드라이버로부터 DMA 소유권을 빼앗는 두 함수가
 * 격리 보증의 실체다. 구현부(drivers/iommu)는 이 트리에 없어 확인 못 함. */
#include <linux/iommu.h>
/* [한국어] struct miscdevice 와 misc_register / misc_deregister, VFIO_MINOR.
 * 컨테이너 노드는 group 노드처럼 동적 major 를 쓰지 않고 **고정 minor 를 가진 misc
 * 디바이스** 다 — /dev/vfio/vfio 는 시스템에 단 하나뿐이기 때문이다. */
#include <linux/miscdevice.h>
/* [한국어] VFIO 외부 ABI. struct vfio_device 와 VFIO_PIN_PAGES_MAX_ENTRIES 를 쓴다. */
#include <linux/vfio.h>
/* [한국어] 사용자 공간과 공유하는 ioctl 번호와 상수. VFIO_GET_API_VERSION(:112),
 * VFIO_CHECK_EXTENSION(:121), VFIO_SET_IOMMU(:134), VFIO_API_VERSION(:19),
 * VFIO_NOIOMMU_IOMMU(:48)이 이 파일에서 직접 쓰인다. */
#include <uapi/linux/vfio.h>

/* [한국어] drivers/vfio 내부 전용 헤더(이미 주석 완료). struct vfio_group(vfio.h:206),
 * struct vfio_iommu_driver_ops(vfio.h:392)와 struct vfio_iommu_driver(vfio.h:452),
 * 그리고 이 파일이 구현하는 함수의 프로토타입(vfio.h:457~478)이 모두 여기 있다. */
#include "vfio.h"

/* [한국어] **컨테이너의 정의는 이 파일 안에만 있다.** vfio.h 는 struct vfio_container 를
 * 전방 선언(vfio.h:94)만 해 두고, 바깥 세계는 vfio_container_from_file 이 돌려주는
 * 불투명 포인터로만 컨테이너를 만진다. 그래서 group.c 도 vfio_main.c 도 아래 필드를
 * 직접 읽지 못하고, 이 파일이 내보내는 함수들을 거쳐야 한다.
 * 컨테이너는 곧 **IOVA 주소공간 하나**이며, 여러 group 이 붙어 그 주소공간을 공유한다. */
struct vfio_container {
	/* [한국어] 컨테이너 메모리의 수명을 세는 참조 카운터.
	 * 설정자: vfio_fops_open 의 kref_init 이 1 로 시작하고, vfio_container_get 이 올리고
	 * vfio_container_put 이 내린다.
	 * 읽는 자: kref 내부에서만. 0 이 되면 vfio_container_release 가 kfree 한다.
	 * 값 범위: 1(컨테이너 fd 만 열림) + 붙어 있는 group 수.
	 * 동기화: kref 자체가 원자 연산이라 별도 락이 없다. 상류 주석대로 컨테이너/group/device
	 * 가 **어떤 순서로 닫혀도** 안전해야 해서 kref 를 쓴다. */
	struct kref			kref;
	/* [한국어] 이 컨테이너에 붙어 있는 group 들의 목록 머리. group 쪽 노드는
	 * vfio_group.container_next(vfio.h:250)다.
	 * 설정자: vfio_container_attach_group 의 list_add, vfio_group_detach_container 의 list_del.
	 * 읽는 자: vfio_container_ioctl_check_extension(비었는지), vfio_ioctl_set_iommu(비었으면
	 * -EINVAL), __vfio_container_attach_groups(순회), vfio_group_detach_container(비었으면
	 * 백엔드 해제).
	 * 값 범위: 빈 목록이면 "비특권 상태" — SET_IOMMU 를 할 수 없다.
	 * 동기화: 아래 group_lock 을 쓰기 모드로 잡고 고쳐야 한다. */
	struct list_head		group_list;
	/* [한국어] 위 group_list 와 iommu_driver/iommu_data 를 지키는 읽기-쓰기 세마포어.
	 * 설정자: vfio_fops_open 의 init_rwsem.
	 * 읽는 자: CHECK_EXTENSION 은 down_read(동시 조회 허용), SET_IOMMU 와
	 * attach/detach 는 down_write.
	 * 값 범위: 세마포어이므로 잠들 수 있는 문맥에서만 잡는다.
	 * 동기화: group->group_lock 을 이미 쥔 채로 이 락을 잡는다
	 * (vfio_container_attach_group, vfio_group_detach_container 의 lockdep_assert_held).
	 * 락 순서는 언제나 group->group_lock → container->group_lock 이다. */
	struct rw_semaphore		group_lock;
	/* [한국어] 선택된 IOMMU 백엔드. NULL 이면 아직 VFIO_SET_IOMMU 를 하지 않은 "비특권" 상태다.
	 * 설정자: vfio_ioctl_set_iommu 가 성공한 백엔드를 넣고,
	 * vfio_group_detach_container 가 마지막 group 이 빠질 때 NULL 로 되돌린다.
	 * 읽는 자: 이 파일의 거의 모든 dispatch 지점.
	 * 값 범위: type1(vfio_iommu_type1.c 의 vfio_iommu_driver_ops_type1),
	 * spapr-tce, 또는 이 파일의 vfio_noiommu_ops.
	 * 동기화: group_lock 아래에서 바뀐다. 다만 vfio_fops_unl_ioctl 의 default 갈래와
	 * pin/unpin/dma_rw 헬퍼는 락 없이 읽는다 — 그때는 호출자가 group 과 device fd 를 쥐고
	 * 있어 백엔드가 바뀔 수 없다는 전제 위에 서 있다. */
	struct vfio_iommu_driver	*iommu_driver;
	/* [한국어] 백엔드가 open 콜백에서 돌려준 불투명 인스턴스 포인터. 이후 모든 콜백의 첫 인자다.
	 * 설정자: vfio_ioctl_set_iommu, 그리고 vfio_group_detach_container 가 NULL 로 되돌린다.
	 * 읽는 자: 백엔드 dispatch 지점 전부.
	 * 값 범위: type1 이면 struct vfio_iommu(vfio_iommu_type1.c) 포인터. VFIO 코어는 그
	 * 내부를 알지 못한다.
	 * 동기화: iommu_driver 와 항상 함께 설정되고 함께 지워진다. */
	void				*iommu_data;
	/* [한국어] 이 컨테이너가 noiommu 전용인가. 진짜 group 과 가짜 group 을 섞지 못하게 하는 표식이다.
	 * 설정자: vfio_container_attach_group 이 group 을 붙일 때마다 그 group 의 type 으로 갱신한다.
	 * 읽는 자: vfio_iommu_driver_allowed(백엔드 선택 제한)와 vfio_container_attach_group
	 * (혼용 거절).
	 * 값 범위: false 가 기본. group 이 하나도 없으면 이 값은 의미가 없다.
	 * 동기화: group_lock 쓰기 모드 아래에서만 바뀐다. */
	bool				noiommu;
};

/* [한국어] 이 파일만의 모듈 전역 상태. group.c 와 vfio_main.c 에도 같은 이름 vfio 의 파일
 * 정적 객체가 있으나 셋 다 타입도 필드도 다른 별개 객체다. static 이라 서로를 볼 수 없다. */
static struct vfio {
	/* [한국어] 등록된 IOMMU 백엔드들의 목록 머리. 각 원소는 struct vfio_iommu_driver(vfio.h:452)이며
	 * 링크 노드는 그 안의 vfio_next 다.
	 * 설정자: vfio_register_iommu_driver 의 list_add, vfio_unregister_iommu_driver 의 list_del.
	 * 읽는 자: vfio_container_ioctl_check_extension 과 vfio_ioctl_set_iommu 가 순회하며
	 * 요청한 IOMMU 타입을 지원하는 백엔드를 찾는다.
	 * 값 범위: 보통 vfio_iommu_type1 하나, noiommu 가 켜져 있으면 둘.
	 * 동기화: 아래 iommu_drivers_lock 을 잡고 만져야 한다. */
	struct list_head		iommu_drivers_list;
	/* [한국어] 위 목록을 지키는 뮤텍스.
	 * 설정자: vfio_container_init 의 mutex_init, vfio_container_cleanup 의 mutex_destroy.
	 * 읽는 자: 등록/해제 두 함수와, 목록을 순회하는 두 dispatch 함수.
	 * 값 범위: 뮤텍스라 잠들 수 있는 문맥에서만.
	 * 동기화: 이 락을 쥔 채 백엔드의 ioctl/open/attach 콜백을 부른다 —
	 * vfio_ioctl_set_iommu 는 container->group_lock 쓰기 락까지 함께 쥐고 있다. */
	struct mutex			iommu_drivers_lock;
/* [한국어] 인스턴스는 이 하나뿐이고 static 이라 파일 밖에서는 보이지 않는다. 이름이 group.c 및 vfio_main.c 의 파일 정적 객체와 같지만 서로 다른 타입의 별개 객체다. */
} vfio;

/* [한국어]
 * vfio_noiommu_open - noiommu 백엔드의 인스턴스 생성 콜백 (실제로는 권한 검사만 한다)
 *
 * @arg: 사용자가 VFIO_SET_IOMMU 에 넘긴 IOMMU 타입 값. VFIO_NOIOMMU_IOMMU(8,
 *       include/uapi/linux/vfio.h:48)여야 한다.
 * @return: 성공 시 **NULL**(상태가 없으므로 인스턴스도 없다). 실패 시 ERR_PTR —
 *          -EINVAL(타입 불일치), -EPERM(CAP_SYS_RAWIO 없음).
 *
 * 왜 필요한가: noiommu 는 "IOMMU 가 없는 물리 디바이스를 그대로 사용자에게 넘기는"
 * 모드이며, IOVA 번역도 격리도 제공하지 않는다. 그래도 컨테이너 ABI 를 그대로 쓰려면
 * vfio_iommu_driver_ops 자리를 채울 무언가가 있어야 하고, 이 함수가 그 open 슬롯이다.
 * 동시에 **사용자가 그 위험을 명시적으로 선택했는지** 확인하는 관문이기도 하다 —
 * 타입을 정확히 VFIO_NOIOMMU_IOMMU 로 지정해야 하고 CAP_SYS_RAWIO 를 가져야 한다.
 *
 * 동작 과정: 타입 확인 → capability 확인 → NULL 반환.
 *
 * 실행 컨텍스트: 프로세스 문맥(VFIO_SET_IOMMU ioctl). 호출자 vfio_ioctl_set_iommu 가
 * vfio.iommu_drivers_lock 과 container->group_lock 쓰기 락을 모두 쥔 상태다.
 *
 * 에러 경로: 두 실패 모두 ERR_PTR 이며, 호출자는 그것을 보고 다음 후보 백엔드로 넘어간다.
 *
 * 주의: 성공값이 NULL 이므로 호출자는 반드시 IS_ERR 로만 성공/실패를 갈라야 한다.
 * vfio_ioctl_set_iommu 가 실제로 그렇게 한다.
 *
 * 호출 체인:
 *   ioctl(container, VFIO_SET_IOMMU, VFIO_NOIOMMU_IOMMU) → vfio_fops_unl_ioctl
 *     → vfio_ioctl_set_iommu → ops->open → [vfio_noiommu_open]
 */
static void *vfio_noiommu_open(unsigned long arg)
{
	/* [한국어] 사용자가 VFIO_SET_IOMMU 에 넘긴 타입이 정확히 VFIO_NOIOMMU_IOMMU(값 8,
	 * include/uapi/linux/vfio.h:48)여야 한다. 다른 타입이면 이 백엔드가 답할 일이 아니다.
	 * 즉 사용자는 "나는 번역도 격리도 없는 모드를 쓰겠다" 를 **명시적으로** 선언해야 한다. */
	if (arg != VFIO_NOIOMMU_IOMMU)
		/* [한국어] 타입이 맞지 않으면 -EINVAL 을 포인터로 감싸 돌려준다. */
		return ERR_PTR(-EINVAL);
	/* [한국어] noiommu 는 IOVA 번역 없이 물리 DMA 를 사용자에게 그대로 주는 모드라
	 * CAP_SYS_RAWIO 를 요구한다. 이 파일에만 같은 검사가 세 곳(여기,
	 * vfio_container_attach_group, vfio_group_use_container) 있고, group.c 의
	 * vfio_group_fops_open 과 vfio_df_group_open 에도 있다. */
	if (!capable(CAP_SYS_RAWIO))
		/* [한국어] 권한이 없으면 -EPERM. */
		return ERR_PTR(-EPERM);

	/* [한국어] **NULL 을 성공값으로 돌려준다.** noiommu 백엔드는 상태를 하나도 갖지 않으므로
	 * 인스턴스가 필요 없다. 호출자 vfio_ioctl_set_iommu 가 IS_ERR 로만 성공/실패를 가르므로
	 * NULL 은 정상적인 성공이다. 이후 모든 콜백의 iommu_data 인자가 NULL 로 들어온다. */
	return NULL;
}

/* [한국어]
 * vfio_noiommu_release - noiommu 백엔드의 인스턴스 해제 콜백 (빈 함수)
 *
 * @iommu_data: vfio_noiommu_open 이 돌려준 값, 즉 항상 NULL.
 * @return: 없음.
 *
 * 왜 필요한가: open 이 아무것도 할당하지 않았으므로 해제할 것도 없다. 그런데도 슬롯을
 * 채워 두는 이유는 vfio_ioctl_set_iommu 의 실패 되감기와 vfio_group_detach_container 가
 * ops->release 를 **슬롯별 NULL 검사 없이** 부르기 때문이다. 빈 함수가 없으면 그 두
 * 지점이 NULL 을 호출한다.
 *
 * 동작 과정: 없음.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 container->group_lock 쓰기 락을 쥐고 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_ioctl_set_iommu(결합 실패 되감기) 또는 vfio_group_detach_container(마지막 group)
 *     → ops->release → [vfio_noiommu_release]
 */
static void vfio_noiommu_release(void *iommu_data)
{
/* [한국어] 본문이 비어 있다. open 이 아무것도 할당하지 않았으므로 해제할 것도 없다.
 * 그래도 슬롯을 채워 두는 이유는 vfio_ioctl_set_iommu 와 vfio_group_detach_container 가
 * ops->release 를 **NULL 검사 없이** 부르기 때문이다. */
}

/* [한국어]
 * vfio_noiommu_ioctl - noiommu 백엔드의 ioctl 디스패처 (CHECK_EXTENSION 하나만 답한다)
 *
 * @iommu_data: 항상 NULL. 조회 단계에서는 인스턴스가 없어 코어가 NULL 을 주기도 한다.
 * @cmd: 사용자가 부른 ioctl 번호.
 * @arg: CHECK_EXTENSION 이면 물어보는 확장 번호.
 * @return: CHECK_EXTENSION 이고 조건이 맞으면 1, 아니면 0. 그 밖의 명령은 -ENOTTY.
 *
 * 왜 필요한가: 두 가지 일을 한다. (1) vfio_ioctl_set_iommu 가 후보 백엔드를 고를 때
 * "너는 이 타입을 지원하느냐" 고 묻는데, 그 답이 이 함수의 1 이다. (2) 사용자가
 * SET_IOMMU 전에 CHECK_EXTENSION 으로 커널 지원 여부를 탐지할 때도 같은 경로로 온다.
 * 그 밖의 모든 IOMMU 계열 ioctl 은 -ENOTTY 다 — **번역이 없으므로 MAP_DMA 도 없다**.
 * 상류 uapi 주석(include/uapi/linux/vfio.h:45~47)이 그 사실을 명시한다.
 *
 * 동작 과정: cmd 가 VFIO_CHECK_EXTENSION 이면 모듈 파라미터 vfio_noiommu 가 켜져 있고
 * arg 가 VFIO_NOIOMMU_IOMMU 인지 보고 1 또는 0. 아니면 -ENOTTY.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출 지점에 따라 group_lock 읽기 락 아래이거나
 * (CHECK_EXTENSION 경로) 쓰기 락 아래(SET_IOMMU 경로)이거나 무락(default 경로)이다.
 *
 * 에러 경로: -ENOTTY 로 "모른다" 를 알린다.
 *
 * 호출 체인:
 *   vfio_container_ioctl_check_extension 또는 vfio_ioctl_set_iommu 또는
 *   vfio_fops_unl_ioctl 의 default → ops->ioctl → [vfio_noiommu_ioctl]
 */
static long vfio_noiommu_ioctl(void *iommu_data,
			       unsigned int cmd, unsigned long arg)
{
	/* [한국어] 이 백엔드가 답하는 명령은 VFIO_CHECK_EXTENSION 하나뿐이다.
	 * 상류 uapi 주석(include/uapi/linux/vfio.h:45~47)도 "No-IOMMU 는 CHECK_EXTENSION 외의
	 * 어떤 ioctl 도 지원하지 않는다" 고 못 박는다 — MAP_DMA 가 없다는 뜻이고, 번역이
	 * 없으니 매핑할 것도 없다는 뜻이다. */
	if (cmd == VFIO_CHECK_EXTENSION)
		/* [한국어] 모듈 파라미터가 켜져 있고 사용자가 물어본 확장이 VFIO_NOIOMMU_IOMMU 일 때만 1.
		 * && 가 ?: 보다 결합력이 높으므로 (vfio_noiommu && arg==...) ? 1 : 0 으로 읽는다.
		 * vfio_noiommu 는 vfio_main.c 의 모듈 파라미터이며, Kconfig 로 꺼 두면 vfio.h:164 가
		 * 컴파일 시 false 상수를 제공해 이 식이 항상 0 이 된다.
		 * 1 을 돌려주는 것이 곧 vfio_ioctl_set_iommu 가 이 백엔드를 고르는 조건이다. */
		return vfio_noiommu && (arg == VFIO_NOIOMMU_IOMMU) ? 1 : 0;

	/* [한국어] 그 밖의 모든 명령은 -ENOTTY — "이 백엔드는 그런 ioctl 을 모른다". */
	return -ENOTTY;
}

/* [한국어]
 * vfio_noiommu_attach_group - noiommu 백엔드의 group 결합 콜백 (빈 함수, 항상 성공)
 *
 * @iommu_data: 항상 NULL.
 * @iommu_group: 붙일 IOMMU group(가짜 group 이다).
 * @type: group 의 종류. noiommu 컨테이너에는 VFIO_NO_IOMMU 만 온다.
 * @return: 항상 0(성공).
 *
 * 왜 필요한가: **이 빈 함수가 곧 "격리가 없다" 의 코드상 실체다.**
 * type1 백엔드의 대응물인 vfio_iommu_type1_attach_group 은 IOMMU domain 을 만들거나
 * 재사용하고 iommu_attach_group 으로 실제 결합을 수행하며 IOVA 창을 계산한다.
 * noiommu 에는 domain 도 IOVA 도 없으므로 할 일이 하나도 없고, 그래서 그냥 성공을
 * 돌려준다. 디바이스가 찍는 DMA 주소는 번역 없이 그대로 물리 주소가 된다.
 *
 * 동작 과정: 없음.
 *
 * 실행 컨텍스트: 프로세스 문맥. container->group_lock 쓰기 락 아래.
 *
 * 에러 경로: 없다 — 실패할 수 없다.
 *
 * 호출 체인:
 *   vfio_container_attach_group(SET_CONTAINER 이후) 또는
 *   __vfio_container_attach_groups(SET_IOMMU) → ops->attach_group
 *     → [vfio_noiommu_attach_group]
 */
static int vfio_noiommu_attach_group(void *iommu_data,
		struct iommu_group *iommu_group, enum vfio_group_type type)
{
	/* [한국어] **아무것도 하지 않고 성공만 돌려준다.** 진짜 백엔드라면 여기서 group 을 IOMMU
	 * domain 에 붙이겠지만(type1 은 vfio_iommu_type1_attach_group 에서 domain 을 만들고
	 * iommu_attach_group 을 부른다), noiommu 에는 domain 자체가 없다.
	 * **이 빈 함수가 곧 "격리가 없다" 의 코드상 실체다.** */
	return 0;
}

/* [한국어]
 * vfio_noiommu_detach_group - noiommu 백엔드의 group 해제 콜백 (빈 함수)
 *
 * @iommu_data: 항상 NULL.
 * @iommu_group: 뗄 group.
 * @return: 없음.
 *
 * 왜 필요한가: attach 가 한 일이 없으므로 뗄 것도 없다. 슬롯을 채워 두는 이유는
 * __vfio_container_attach_groups 의 되감기 루프와 vfio_group_detach_container 가
 * ops->detach_group 을 NULL 검사 없이 부르기 때문이다.
 *
 * 동작 과정: 없음.
 *
 * 실행 컨텍스트: 프로세스 문맥. container->group_lock 쓰기 락 아래.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   __vfio_container_attach_groups(되감기) 또는 vfio_group_detach_container
 *     → ops->detach_group → [vfio_noiommu_detach_group]
 */
static void vfio_noiommu_detach_group(void *iommu_data,
				      struct iommu_group *iommu_group)
{
/* [한국어] attach 가 한 일이 없으므로 뗄 것도 없다. 슬롯이 NULL 검사 없이 불리므로 빈 함수라도 있어야 한다. */
}

/* [한국어] noiommu 백엔드의 vtable. type1 과 똑같은 자리에 꽂히지만 실제로 하는 일은
 * 권한 검사와 확장 신고 두 가지뿐이다. vfio_container_init 이 CONFIG_VFIO_NOIOMMU 가
 * 켜져 있을 때만 이것을 등록하고, vfio_iommu_driver_allowed 가 **이 주소와 같은지**로
 * noiommu 백엔드인지를 판별한다. */
static const struct vfio_iommu_driver_ops vfio_noiommu_ops = {
	/* [한국어] 백엔드 이름. 진단용이며 사용자에게 노출되지 않는다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: 이 트리 안에서 이 필드를 읽는 코드는 없다.
	 * 값 범위: 문자열 리터럴 "vfio-noiommu".
	 * 동기화: 불변. */
	.name = "vfio-noiommu",
	/* [한국어] 이 vtable 을 소유한 모듈. 컨테이너가 이 백엔드를 쓰는 동안 try_module_get 으로
	 * 붙잡아 둔다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: vfio_ioctl_set_iommu 의 try_module_get,
	 * vfio_group_detach_container 와 실패 경로의 module_put.
	 * 값 범위: THIS_MODULE — noiommu 백엔드는 vfio 코어 모듈에 내장돼 있다.
	 * 동기화: 불변. */
	.owner = THIS_MODULE,
	/* [한국어] SET_IOMMU 때 인스턴스를 만든다. 여기서는 인자 검증과 권한 검사만 하고 NULL 을 준다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: vfio_ioctl_set_iommu 의 driver->ops->open(arg).
	 * 값 범위: 이 파일의 vfio_noiommu_open.
	 * 동기화: iommu_drivers_lock 과 container->group_lock 쓰기 락을 모두 쥔 채 불린다. */
	.open = vfio_noiommu_open,
	/* [한국어] 컨테이너 해제 시 정리. 빈 함수다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: vfio_ioctl_set_iommu 의 실패 되감기와 vfio_group_detach_container.
	 * 값 범위: 이 파일의 vfio_noiommu_release.
	 * 동기화: 부르는 쪽이 group_lock 쓰기 락을 쥐고 있다. */
	.release = vfio_noiommu_release,
	/* [한국어] ioctl 디스패처. CHECK_EXTENSION 만 답하고 나머지는 -ENOTTY.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: vfio_container_ioctl_check_extension(두 곳), vfio_ioctl_set_iommu(선별),
	 * vfio_fops_unl_ioctl 의 default 갈래.
	 * 값 범위: 이 파일의 vfio_noiommu_ioctl.
	 * 동기화: 호출 지점에 따라 group_lock 읽기/쓰기 또는 무락. */
	.ioctl = vfio_noiommu_ioctl,
	/* [한국어] group 결합. 빈 함수 — noiommu 에는 IOMMU domain 이 없다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: __vfio_container_attach_groups 와 vfio_container_attach_group.
	 * 값 범위: 이 파일의 vfio_noiommu_attach_group.
	 * 동기화: container->group_lock 쓰기 락 아래. */
	.attach_group = vfio_noiommu_attach_group,
	/* [한국어] group 해제. 역시 빈 함수다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: __vfio_container_attach_groups 의 되감기 루프와 vfio_group_detach_container.
	 * 값 범위: 이 파일의 vfio_noiommu_detach_group.
	 * 동기화: container->group_lock 쓰기 락 아래.
	 * **나머지 슬롯(pin_pages, unpin_pages, register_device, unregister_device, dma_rw,
	 * group_iommu_domain)은 초기화하지 않아 NULL 이다.** 그래서 mdev 계열 헬퍼가
	 * noiommu 컨테이너에서 -ENOTTY 로 실패한다. */
	.detach_group = vfio_noiommu_detach_group,
};

/* [한국어]
 * vfio_iommu_driver_allowed - 이 컨테이너가 이 백엔드를 써도 되는가
 *
 * @container: 검사 대상 컨테이너. noiommu 필드가 판단 근거다.
 * @driver: 후보 백엔드.
 * @return: 허용되면 true.
 *
 * 왜 필요한가: 상류 주석이 규칙을 그대로 적어 두었다 — "noiommu 컨테이너만
 * vfio-noiommu 를 쓸 수 있고, vfio-noiommu 는 noiommu 컨테이너에서만 쓸 수 있다".
 * 진짜 IOMMU 격리를 받는 group 과 격리가 전혀 없는 group 이 **한 IOVA 주소공간을
 * 공유하면** 격리 보증 자체가 무의미해지기 때문이다. 컨테이너의 noiommu 성격은
 * vfio_container_attach_group 이 첫 group 을 붙일 때 확정한다.
 *
 * 동작 과정: Kconfig 로 noiommu 가 아예 빠진 빌드면 섞일 백엔드가 없으므로 무조건 true.
 * 아니면 container->noiommu 와 "driver->ops 가 vfio_noiommu_ops 인가" 가 정확히
 * 일치하는지 본다. **vtable 주소를 비교하므로 이름 위조가 통하지 않는다.**
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 container->group_lock(읽기 또는 쓰기)과
 * vfio.iommu_drivers_lock 을 쥐고 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_container_ioctl_check_extension 또는 vfio_ioctl_set_iommu
 *     → [vfio_iommu_driver_allowed]
 */
/*
 * Only noiommu containers can use vfio-noiommu and noiommu containers can only
 * use vfio-noiommu.
 */
static bool vfio_iommu_driver_allowed(struct vfio_container *container,
				      const struct vfio_iommu_driver *driver)
{
	/* [한국어] Kconfig 로 noiommu 를 아예 뺐다면 vfio_noiommu_ops 도 등록되지 않으므로 섞일 일이
	 * 없다. 컴파일 시 상수라 이 갈래가 통째로 남고 아래 줄이 죽은 코드가 된다. */
	if (!IS_ENABLED(CONFIG_VFIO_NOIOMMU))
		return true;
	/* [한국어] 컨테이너가 noiommu 인 것과 백엔드가 noiommu 인 것이 **정확히 일치할 때만** 허용한다.
	 * vtable **주소**를 비교하므로 이름 위조가 통하지 않는다. 상류 주석대로
	 * "noiommu 컨테이너만 vfio-noiommu 를 쓸 수 있고, vfio-noiommu 는 noiommu 컨테이너
	 * 에서만 쓸 수 있다" — 진짜 격리와 가짜 격리가 한 주소공간에 섞이는 것을 막는다. */
	return container->noiommu == (driver->ops == &vfio_noiommu_ops);
}

/* [한국어]
 * vfio_register_iommu_driver - IOMMU 백엔드를 VFIO 코어의 후보 목록에 등록한다
 *
 * @ops: 백엔드가 제공하는 vtable. 백엔드 모듈의 정적 const 객체다.
 * @return: 0 성공. -EINVAL(register_device/unregister_device 짝이 안 맞거나 이미 등록됨),
 *          -ENOMEM.
 *
 * 왜 필요한가: 컨테이너는 자신이 어떤 IOMMU 구현 위에서 돌지 모른 채 만들어진다.
 * VFIO_SET_IOMMU 가 들어오면 그때 등록된 후보들을 훑어 요청한 타입을 지원하는 것을
 * 고른다. 이 함수가 그 후보 목록에 자기를 올리는 창구이며, type1(별도 모듈)과
 * spapr-tce(별도 모듈), 그리고 이 파일의 noiommu 가 쓴다.
 *
 * 동작 과정:
 *  1. register_device 와 unregister_device 가 짝인지 WARN_ON 으로 확인한다.
 *     하나만 있으면 vfio_device_container_register 와 unregister 의 조건이 어긋나
 *     백엔드의 device 목록이 깨진다.
 *  2. 목록 원소(struct vfio_iommu_driver)를 하나 할당하고 vtable 포인터만 담는다.
 *  3. iommu_drivers_lock 아래에서 같은 vtable 이 이미 있는지 확인하고, 없으면
 *     목록 **앞** 에 넣는다.
 *
 * 실행 컨텍스트: 백엔드 모듈의 init(프로세스 문맥). 할당과 뮤텍스가 있어 잠들 수 있다.
 *
 * 에러 경로: 중복이면 방금 만든 원소를 kfree 하고 -EINVAL.
 *
 * 호출 체인:
 *   vfio_iommu_type1_init(vfio_iommu_type1.c) 또는 vfio_container_init(이 파일)
 *     → [vfio_register_iommu_driver]
 */
/*
 * IOMMU driver registration
 */
int vfio_register_iommu_driver(const struct vfio_iommu_driver_ops *ops)
{
	/* [한국어] 새로 만들 목록 원소 driver 와, 중복 검사용 순회 커서 tmp. */
	struct vfio_iommu_driver *driver, *tmp;

	/* [한국어] register_device 와 unregister_device 는 반드시 짝으로 있어야 한다.
	 * ! 를 씌워 둘 다 0/1 로 정규화한 뒤 != 로 비교하므로 하나만 있는 vtable 에서만
	 * 경고가 뜬다. 한쪽만 있으면 vfio_device_container_register 와 unregister 의 짝이
	 * 깨져 백엔드의 device 목록이 어긋난다. */
	if (WARN_ON(!ops->register_device != !ops->unregister_device))
		/* [한국어] 잘못 만들어진 vtable 은 등록을 거절한다. */
		return -EINVAL;

	/* [한국어] 목록 원소를 0 초기화로 할당한다. vtable 자체는 백엔드 모듈의 정적 객체라 복사하지 않는다. */
	driver = kzalloc_obj(*driver);
	/* [한국어] 메모리 부족. */
	if (!driver)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] vtable 포인터만 보관한다. 이 포인터가 곧 백엔드의 신원이다. */
	driver->ops = ops;

	/* [한국어] 목록을 고치는 동안 순회하는 쪽과 직렬화한다. */
	mutex_lock(&vfio.iommu_drivers_lock);

	/* Check for duplicates */
	/* [한국어] 이미 같은 vtable 이 등록돼 있는지 본다. */
	list_for_each_entry(tmp, &vfio.iommu_drivers_list, vfio_next) {
		/* [한국어] **주소**를 비교한다 — 같은 모듈이 두 번 등록하는 것을 막는다. */
		if (tmp->ops == ops) {
			/* [한국어] 중복이므로 되돌려야 한다. 락부터 푼다. */
			mutex_unlock(&vfio.iommu_drivers_lock);
			/* [한국어] 방금 만든 원소를 해제한다. */
			kfree(driver);
			/* [한국어] -EINVAL. */
			return -EINVAL;
		}
	}

	/* [한국어] 목록 **앞** 에 넣는다. 그래서 나중에 등록된 백엔드가 먼저 조회된다 —
	 * vfio_container_init 이 noiommu 를 마지막에 등록하므로, noiommu 가 type1 보다
	 * 앞에 오게 된다. 다만 vfio_iommu_driver_allowed 가 섞임을 막으므로 순서가
	 * 선택 결과를 바꾸지는 않는다. */
	list_add(&driver->vfio_next, &vfio.iommu_drivers_list);

	/* [한국어] 임계구역을 닫는다. */
	mutex_unlock(&vfio.iommu_drivers_lock);

	/* [한국어] 성공. 백엔드 모듈의 init 이 이 값으로 적재 성공 여부를 정한다. */
	return 0;
}
/* [한국어] EXPORT_SYMBOL_GPL — vfio_iommu_type1 과 vfio_iommu_spapr_tce 가 별도 모듈이라
 * 내보내야 한다. type1 쪽 호출 지점은 vfio_iommu_type1.c 의 vfio_iommu_type1_init 이다. */
EXPORT_SYMBOL_GPL(vfio_register_iommu_driver);

/* [한국어]
 * vfio_unregister_iommu_driver - 백엔드를 후보 목록에서 뺀다
 *
 * @ops: 등록할 때 준 것과 같은 vtable 포인터.
 * @return: 없음. 못 찾아도 조용히 돌아간다.
 *
 * 왜 필요한가: vfio_register_iommu_driver 의 짝이다. 백엔드 모듈이 언로드될 때
 * 자기를 목록에서 빼야 이후 SET_IOMMU 가 사라진 코드를 부르지 않는다.
 *
 * 동작 과정: iommu_drivers_lock 아래에서 vtable 주소가 같은 원소를 찾아 list_del 하고
 * kfree 한다.
 *
 * 실행 컨텍스트: 백엔드 모듈의 exit(프로세스 문맥). 뮤텍스를 잡는다.
 *
 * 에러 경로: 없다. 못 찾으면 아무것도 하지 않는다.
 *
 * 안전성: 이 시점에 그 백엔드를 쓰는 컨테이너가 남아 있으면 안 되는데, 그것은
 * module 참조가 보장한다 — vfio_ioctl_set_iommu 의 try_module_get 이 걸려 있는 동안
 * rmmod 자체가 되지 않으므로 exit 이 불리지 않는다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_cleanup(vfio_iommu_type1.c) 또는 vfio_container_cleanup(이 파일)
 *     → [vfio_unregister_iommu_driver]
 */
void vfio_unregister_iommu_driver(const struct vfio_iommu_driver_ops *ops)
{
	/* [한국어] 순회 커서. */
	struct vfio_iommu_driver *driver;

	/* [한국어] 목록을 고치는 동안 직렬화한다. */
	mutex_lock(&vfio.iommu_drivers_lock);
	/* [한국어] 등록된 원소를 훑는다. */
	list_for_each_entry(driver, &vfio.iommu_drivers_list, vfio_next) {
		/* [한국어] vtable 주소가 같은 원소를 찾는다. */
		if (driver->ops == ops) {
			/* [한국어] 목록에서 뺀다. 이 뒤로는 새 컨테이너가 이 백엔드를 고를 수 없다. */
			list_del(&driver->vfio_next);
			/* [한국어] kfree 전에 락을 푼다. */
			mutex_unlock(&vfio.iommu_drivers_lock);
			/* [한국어] 등록 때 만든 원소를 해제한다. **이 시점에 이 백엔드를 쓰는 컨테이너가 남아 있으면
			 * 안 되는데**, 그것은 module_put 짝이 보장한다 — 컨테이너가 백엔드를 쓰는 동안
			 * try_module_get 이 걸려 있어 rmmod 자체가 되지 않는다. */
			kfree(driver);
			/* [한국어] 찾아서 지웠으니 끝. */
			return;
		}
	}
	/* [한국어] 못 찾았으면 조용히 아무것도 하지 않는다. */
	mutex_unlock(&vfio.iommu_drivers_lock);
}
/* [한국어] EXPORT_SYMBOL_GPL — 백엔드 모듈의 exit 이 부른다(vfio_iommu_type1.c 의 vfio_iommu_type1_cleanup). */
EXPORT_SYMBOL_GPL(vfio_unregister_iommu_driver);

/* [한국어]
 * vfio_container_release - 컨테이너의 마지막 참조가 사라졌을 때의 소멸자
 *
 * @kref: 0 에 도달한 참조 카운터. container_of 로 컨테이너를 되찾는다.
 * @return: 없음.
 *
 * 왜 필요한가: 상류 주석이 밝히듯 컨테이너는 /dev/vfio/vfio 를 열 때 만들어지지만
 * 그 수명은 **마지막 사용자가 끝날 때까지** 이어지고, 컨테이너와 group 과 device 가
 * 어떤 순서로 닫혀도 안전해야 한다. 그래서 fd 수명이 아니라 kref 로 관리하고,
 * 0 이 되는 지점을 이 함수 하나로 모았다.
 *
 * 동작 과정: container_of 로 구조체를 되찾아 kfree 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. kref_put 을 부른 쪽(vfio_fops_release 또는
 * vfio_group_detach_container)의 문맥에서 동기적으로 실행된다.
 *
 * 에러 경로: 없다.
 *
 * 여기서 iommu_driver 를 정리하지 않는 이유: 여기 도달했다는 것은 붙어 있던 group 이
 * 하나도 없다는 뜻이고, 마지막 group 이 빠질 때 vfio_group_detach_container 가 이미
 * 백엔드 release 와 module_put 을 마쳤기 때문이다.
 *
 * 호출 체인:
 *   vfio_container_put → kref_put → [vfio_container_release] → kfree
 */
/*
 * Container objects - containers are created when /dev/vfio/vfio is
 * opened, but their lifecycle extends until the last user is done, so
 * it's freed via kref.  Must support container/group/device being
 * closed in any order.
 */
static void vfio_container_release(struct kref *kref)
{
	/* [한국어] kref 에서 되찾을 컨테이너. */
	struct vfio_container *container;
	/* [한국어] kref 멤버 주소에서 그것을 품은 struct vfio_container 의 시작 주소를 계산한다.
	 * 컴파일 시 오프셋 뺄셈이라 런타임 비용이 없다. */
	container = container_of(kref, struct vfio_container, kref);

	/* [한국어] 컨테이너 메모리를 해제한다. **iommu_driver 정리는 여기서 하지 않는다** —
	 * 여기 도달했다는 것은 붙어 있던 group 이 하나도 없다는 뜻이고,
	 * 마지막 group 이 빠질 때 vfio_group_detach_container 가 이미 백엔드 release 와
	 * module_put 을 마쳤기 때문이다. */
	kfree(container);
}

/* [한국어]
 * vfio_container_get - 컨테이너 참조를 하나 올린다
 *
 * @container: 붙들 컨테이너.
 * @return: 없음.
 *
 * 왜 필요한가: group 이 컨테이너에 붙어 있는 동안 컨테이너 메모리가 살아 있어야 한다.
 * 사용자가 컨테이너 fd 를 먼저 close 해도 group 이 남아 있으면 해제되면 안 되기
 * 때문이다. 유일한 호출 지점은 vfio_container_attach_group 이며, **group 하나당 1** 이다.
 *
 * 동작 과정: kref_get 한 번.
 *
 * 실행 컨텍스트: 프로세스 문맥. container->group_lock 쓰기 락 아래.
 *
 * 에러 경로: 없다. kref_get 은 0 에서 올리려 하면 커널이 경고한다.
 *
 * 짝: vfio_group_detach_container 의 vfio_container_put.
 * 빠뜨리면 group 이 아직 붙어 있는데 컨테이너 fd 를 닫는 순간 메모리가 해제되어
 * use-after-free 가 된다.
 *
 * 호출 체인:
 *   vfio_container_attach_group → [vfio_container_get] → kref_get
 */
static void vfio_container_get(struct vfio_container *container)
{
	/* [한국어] 참조를 하나 올린다. 유일한 호출 지점은 vfio_container_attach_group 이며,
	 * "이 group 이 컨테이너를 붙들고 있다" 는 뜻이다. 짝은 vfio_group_detach_container 의
	 * vfio_container_put. 빠뜨리면 group 이 아직 붙어 있는데 컨테이너 fd 를 닫는 순간
	 * 메모리가 해제되어 use-after-free 가 된다. */
	kref_get(&container->kref);
}

/* [한국어]
 * vfio_container_put - 컨테이너 참조를 하나 내리고 마지막이면 해제한다
 *
 * @container: 놓을 컨테이너.
 * @return: 없음.
 *
 * 왜 필요한가: 컨테이너 참조는 두 종류의 주인이 있다 — 컨테이너 fd 자신(kref_init 의 1)과
 * 붙어 있는 각 group(vfio_container_get). 이 함수가 그 둘을 모두 놓는 통로이며,
 * 마지막 하나가 빠질 때 vfio_container_release 를 부른다.
 *
 * 동작 과정: kref_put 한 번. 0 이 되면 소멸자를 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. vfio_group_detach_container 는 **락을 푼 뒤에** 부른다 —
 * 마지막 참조라면 kfree 로 이어지기 때문이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 지점 둘:
 *  - vfio_fops_release : 컨테이너 fd 를 닫을 때, kref_init 의 1 을 되돌린다.
 *  - vfio_group_detach_container : group 이 빠질 때, attach 의 get 을 되돌린다.
 * 어느 쪽이 나중이든 마지막 하나가 메모리를 해제한다.
 *
 * 호출 체인:
 *   vfio_fops_release 또는 vfio_group_detach_container → [vfio_container_put]
 *     → kref_put → vfio_container_release
 */
static void vfio_container_put(struct vfio_container *container)
{
	/* [한국어] 참조를 하나 내리고 0 이면 vfio_container_release 를 부른다. 호출 지점은 둘 —
	 * vfio_fops_release(컨테이너 fd 를 닫을 때, kref_init 의 1 을 되돌림)와
	 * vfio_group_detach_container(group 이 빠질 때, attach 의 get 을 되돌림).
	 * 상류 주석대로 컨테이너/group/device 가 **어떤 순서로 닫혀도** 마지막 하나가
	 * 메모리를 해제하도록 이 구조를 쓴다. */
	kref_put(&container->kref, vfio_container_release);
}

/* [한국어]
 * vfio_device_container_register - 디바이스를 백엔드의 device 목록에 등록한다
 *
 * @device: 첫 fd 가 열리는 디바이스. group->container 가 이미 있어야 한다.
 * @return: 없음. 백엔드가 이 기능을 제공하지 않으면 조용히 아무것도 하지 않는다.
 *
 * 왜 필요한가: type1 백엔드는 DMA 언맵이 일어날 때 그 IOVA 를 핀해 둔 vendor 드라이버에게
 * 알려 줘야 한다(mdev 계열이 게스트 메모리를 붙들고 있을 수 있으므로). 그러려면 어떤
 * 디바이스가 이 컨테이너에 참여 중인지 알아야 하고, 이 함수가 그것을 알린다.
 *
 * 동작 과정: group->container->iommu_driver 를 꺼내고, 그 vtable 이 register_device 를
 * 제공하면 iommu_data 와 함께 호출한다.
 *
 * 실행 컨텍스트: 프로세스 문맥(첫 device open). 호출자 group.c 의
 * vfio_device_group_use_iommu 가 group->group_lock 과 dev_set->lock 을 쥐고 있다.
 *
 * 에러 경로: 없다. 반환값이 void 라 백엔드도 실패를 알릴 수 없다.
 *
 * 백엔드 dispatch: type1 이면 vfio_iommu_type1_register_device.
 * noiommu 는 이 슬롯이 NULL 이라 건너뛴다.
 *
 * 짝: vfio_device_container_unregister. 빠뜨리면 해제된 device 포인터가 백엔드 목록에
 * 남아 use-after-free 가 된다.
 *
 * 호출 체인:
 *   vfio_df_open → vfio_df_device_first_open(vfio_main.c)
 *     → vfio_device_group_use_iommu(group.c) → [vfio_device_container_register]
 *     → ops->register_device → vfio_iommu_type1_register_device
 */
void vfio_device_container_register(struct vfio_device *device)
{
	/* [한국어] 이 디바이스가 속한 group 의 컨테이너에서 백엔드를 꺼낸다.
	 * 호출자(group.c 의 vfio_device_group_use_iommu)가 group->group_lock 을 쥐고 있고
	 * container 가 NULL 이 아님을 이미 확인했다. */
	struct vfio_iommu_driver *iommu_driver =
		device->group->container->iommu_driver;

	/* [한국어] 백엔드가 정해져 있고 그 vtable 이 register_device 를 제공할 때만 부른다.
	 * noiommu 백엔드는 이 슬롯이 NULL 이라 조용히 건너뛴다. */
	if (iommu_driver && iommu_driver->ops->register_device)
		/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_register_device 로 간다.
		 * 그 함수는 디바이스를 컨테이너의 device 목록에 넣어, DMA 언맵이 일어날 때
		 * vendor 에게 알려 줄 대상으로 등록한다. 짝은 vfio_device_container_unregister 다. */
		iommu_driver->ops->register_device(
			device->group->container->iommu_data, device);
}

/* [한국어]
 * vfio_device_container_unregister - 디바이스를 백엔드의 device 목록에서 뺀다
 *
 * @device: 마지막 fd 가 닫히는 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_device_container_register 의 짝이다. 디바이스가 더 이상 이 컨테이너에
 * 참여하지 않으므로 언맵 통지 대상에서 빼야 한다. 빠뜨리면 곧 해제될 device 포인터가
 * 백엔드 목록에 남는다.
 *
 * 동작 과정: register 쪽과 같은 방식으로 백엔드를 꺼내고, unregister_device 슬롯이 있으면
 * 호출한다. 조건이 register 쪽과 정확히 대칭이라 짝이 어긋나지 않는다
 * (vfio_register_iommu_driver 의 WARN_ON 이 두 슬롯이 짝임을 보장한다).
 *
 * 실행 컨텍스트: 프로세스 문맥(마지막 device close). 호출자가 group->group_lock 과
 * dev_set->lock 을 쥐고 있다.
 *
 * 에러 경로: 없다.
 *
 * 백엔드 dispatch: type1 이면 vfio_iommu_type1_unregister_device.
 *
 * 호출 체인:
 *   vfio_df_close → vfio_df_device_last_close(vfio_main.c)
 *     → vfio_device_group_unuse_iommu(group.c) → [vfio_device_container_unregister]
 *     → ops->unregister_device → vfio_iommu_type1_unregister_device
 */
void vfio_device_container_unregister(struct vfio_device *device)
{
	/* [한국어] 같은 방식으로 백엔드를 꺼낸다. */
	struct vfio_iommu_driver *iommu_driver =
		device->group->container->iommu_driver;

	/* [한국어] 짝이 되는 슬롯이 있을 때만 부른다. register 쪽과 같은 조건이라 짝이 어긋나지 않는다. */
	if (iommu_driver && iommu_driver->ops->unregister_device)
		/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_unregister_device.
		 * 언맵 통지 대상 목록에서 이 디바이스를 뺀다. 빠뜨리면 해제된 device 포인터가
		 * 백엔드 목록에 남아 use-after-free 가 된다. */
		iommu_driver->ops->unregister_device(
			device->group->container->iommu_data, device);
}

/* [한국어]
 * vfio_container_ioctl_check_extension - VFIO_CHECK_EXTENSION ioctl 핸들러
 *
 * @container: 컨테이너 fd 의 private_data.
 * @arg: 물어보는 확장 번호(VFIO_TYPE1_IOMMU, VFIO_TYPE1v2_IOMMU, VFIO_DMA_CC_IOMMU,
 *       VFIO_NOIOMMU_IOMMU 등, include/uapi/linux/vfio.h:26~).
 * @return: 0 이면 미지원, 양수면 지원.
 *
 * 왜 필요한가: 사용자 공간이 SET_IOMMU 를 하기 전에 "이 커널이 어떤 IOMMU 인터페이스를
 * 지원하는가" 를 알아내는 표준 절차다. 그리고 SET_IOMMU 이후에는 "이 컨테이너의 현재
 * 백엔드가 그 기능을 갖췄는가" 를 묻는 용도로 바뀐다.
 *
 * 동작 과정(상류 주석이 규칙을 설명한다):
 *  - 백엔드가 아직 없으면 등록된 모든 후보를 순회하며 물어보고 **첫 긍정** 을 답으로 쓴다.
 *    이때 컨테이너에 이미 group 이 있으면 성격이 맞지 않는 후보는 건너뛴다.
 *    각 호출 전후로 try_module_get / module_put 을 짝지어 모듈이 사라지지 않게 한다.
 *  - 백엔드가 이미 정해졌으면 그 백엔드에만, 그리고 **실제 인스턴스와 함께** 묻는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). container->group_lock 을 **읽기 모드**로 잡으므로
 * 여러 스레드가 동시에 조회할 수 있다. 후보 순회 구간에서는 vfio.iommu_drivers_lock 도
 * 함께 잡는다.
 *
 * 에러 경로: 없다. 모르는 확장은 0 이며 그것이 정상적인 답이다.
 *
 * 백엔드 dispatch: 두 곳 모두 ops->ioctl 이며, type1 이면 vfio_iommu_type1_ioctl 의
 * VFIO_CHECK_EXTENSION 갈래, noiommu 면 vfio_noiommu_ioctl 이다.
 * 첫 번째 호출은 iommu_data 로 NULL 을 주는데, 그 갈래가 인스턴스를 만지지 않기 때문이다.
 *
 * 호출 체인:
 *   ioctl(container, VFIO_CHECK_EXTENSION) → vfio_fops_unl_ioctl
 *     → [vfio_container_ioctl_check_extension] → ops->ioctl
 */
static long
vfio_container_ioctl_check_extension(struct vfio_container *container,
				     unsigned long arg)
{
	/* [한국어] 순회 커서 겸 이미 정해진 백엔드를 담을 변수. */
	struct vfio_iommu_driver *driver;
	/* [한국어] 기본 답은 0 — "지원하지 않음". */
	long ret = 0;

	/* [한국어] 읽기 모드로 잡는다. 이 ioctl 은 상태를 바꾸지 않으므로 여러 스레드가 동시에
	 * 조회해도 된다. */
	down_read(&container->group_lock);

	/* [한국어] 이미 SET_IOMMU 가 끝났으면 그 백엔드에만 물어본다. */
	driver = container->iommu_driver;

	/* [한국어] 확장 번호로 갈라야 하지만, 코어가 직접 답하는 확장은 아직 없다. */
	switch (arg) {
		/* No base extensions yet */
	/* [한국어] 모든 확장 질의가 이 갈래로 온다. */
	default:
		/*
		 * If no driver is set, poll all registered drivers for
		 * extensions and return the first positive result.  If
		 * a driver is already set, further queries will be passed
		 * only to that driver.
		 */
		/* [한국어] 아직 백엔드가 정해지지 않았다면 등록된 모든 백엔드에 물어보고 첫 긍정을 답으로 쓴다.
		 * 사용자 공간이 SET_IOMMU 전에 "이 커널이 TYPE1 을 지원하는가" 를 알아보는 표준 절차다. */
		if (!driver) {
			/* [한국어] 백엔드 목록을 순회하는 동안 등록/해제와 직렬화한다. */
			mutex_lock(&vfio.iommu_drivers_lock);
			/* [한국어] 등록된 백엔드를 하나씩 본다. */
			list_for_each_entry(driver, &vfio.iommu_drivers_list,
					    vfio_next) {

				/* [한국어] 이미 group 이 붙어 있다면 컨테이너의 noiommu 성격이 확정된 상태이므로,
				 * 그와 맞지 않는 백엔드는 물어볼 필요도 없다. group 이 하나도 없으면 아직 성격이
				 * 정해지지 않았으므로 모두에게 물어본다. */
				if (!list_empty(&container->group_list) &&
				    !vfio_iommu_driver_allowed(container,
							       driver))
					/* [한국어] 맞지 않는 백엔드는 건너뛴다. */
					continue;
				/* [한국어] 콜백을 부르는 동안 그 모듈이 언로드되지 않게 붙잡는다. 실패는 모듈이 이미
				 * 내려가는 중이라는 뜻이다. */
				if (!try_module_get(driver->ops->owner))
					continue;

				/* [한국어] **백엔드 dispatch**: iommu_data 로 NULL 을 준다 — 아직 인스턴스가 없기 때문이다.
				 * type1 이면 vfio_iommu_type1_ioctl 의 VFIO_CHECK_EXTENSION 갈래로 가고,
				 * 그 갈래는 iommu_data 를 만지지 않고 상수만 답한다. */
				ret = driver->ops->ioctl(NULL,
							 VFIO_CHECK_EXTENSION,
							 arg);
				/* [한국어] 질의가 끝났으니 모듈 참조를 즉시 놓는다. 이 참조는 이 한 번의 호출 동안만 필요하다. */
				module_put(driver->ops->owner);
				/* [한국어] 긍정(1 이상)이 나오면 */
				if (ret > 0)
					/* [한국어] 더 볼 필요 없이 그 값을 답으로 쓴다. */
					break;
			}
			/* [한국어] 임계구역을 닫는다. */
			mutex_unlock(&vfio.iommu_drivers_lock);
		/* [한국어] 백엔드가 이미 정해진 경우. */
		} else
			/* [한국어] **백엔드 dispatch**: 이번에는 진짜 인스턴스(iommu_data)를 준다.
			 * type1 이면 vfio_iommu_type1_ioctl 이 그 인스턴스의 상태(예: 붙어 있는 domain 이
			 * 캐시 일관성을 강제하는가)를 보고 답한다.
			 * 상류 주석대로 한 번 백엔드가 정해지면 이후 질의는 그 백엔드에만 간다. */
			ret = driver->ops->ioctl(container->iommu_data,
						 VFIO_CHECK_EXTENSION, arg);
	}

	/* [한국어] 읽기 락을 푼다. */
	up_read(&container->group_lock);

	/* [한국어] 0(미지원) 또는 양수(지원)가 ioctl 반환값이 된다. */
	return ret;
}

/* [한국어]
 * __vfio_container_attach_groups - 컨테이너에 이미 붙어 있는 모든 group 을 새 백엔드에 결합한다
 *
 * @container: 대상 컨테이너. 호출자가 group_lock 을 쓰기 모드로 잡고 있어야 한다
 *             (상류 주석이 그 요구를 적어 두었다).
 * @driver: 방금 고른 후보 백엔드.
 * @data: 그 백엔드의 open 이 돌려준 인스턴스.
 * @return: 0 성공. 실패면 그 group 의 attach_group 이 낸 오류.
 *
 * 왜 필요한가: VFIO_SET_IOMMU 는 group 이 하나 이상 붙어 있어야만 할 수 있고, 그 시점에
 * 이미 붙어 있는 group 들을 **한꺼번에** 새 백엔드에 결합해야 한다. 하나라도 실패하면
 * 부분적으로 결합된 상태를 남기면 안 되므로 되감기까지 이 함수가 책임진다.
 *
 * 동작 과정: 목록을 앞에서부터 돌며 ops->attach_group 을 부르고, 실패하면 unwind 로 간다.
 * unwind 는 list_for_each_entry_continue_reverse 로 **실패한 항목의 바로 앞** 부터
 * 역방향으로 돌며 detach_group 을 부른다. 실패한 항목 자신은 붙지 않았으므로 되돌릴
 * 대상이 아니고, 그 매크로가 커서를 한 칸 뒤로 옮긴 뒤 시작하므로 경계가 정확히 맞는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(SET_IOMMU ioctl). container->group_lock 쓰기 락과
 * vfio.iommu_drivers_lock 을 모두 쥔 상태.
 *
 * 에러 경로: 위의 unwind 루프. detach_group 은 반환값이 없어 되감기는 실패할 수 없다.
 *
 * 반환값에 관한 관찰: ret 은 -ENODEV 로 초기화되고, 루프를 끝까지 돌면 **마지막 반복의
 * attach_group 반환값**, 즉 0 이 그대로 나간다. 목록이 비어 루프가 한 번도 돌지 않으면
 * -ENODEV 가 나가지만, 유일한 호출자 vfio_ioctl_set_iommu 가 그 전에 list_empty 를
 * 확인해 -EINVAL 로 막으므로 실제로 도달하지 않는다.
 *
 * 백엔드 dispatch: type1 이면 vfio_iommu_type1_attach_group 과
 * vfio_iommu_type1_detach_group.
 *
 * 호출 체인:
 *   vfio_ioctl_set_iommu → [__vfio_container_attach_groups]
 *     → ops->attach_group / ops->detach_group
 */
/* hold write lock on container->group_lock */
static int __vfio_container_attach_groups(struct vfio_container *container,
					  struct vfio_iommu_driver *driver,
					  void *data)
{
	/* [한국어] 순회 커서. 실패 시 되감기 시작점으로도 쓰인다. */
	struct vfio_group *group;
	/* [한국어] 기본값 -ENODEV. 목록이 비어 루프가 한 번도 돌지 않으면 이 값이 그대로 나가지만,
	 * 유일한 호출자 vfio_ioctl_set_iommu 가 그 전에 list_empty 를 확인해 -EINVAL 로
	 * 막으므로 실제로는 도달하지 않는다. */
	int ret = -ENODEV;

	/* [한국어] 이 컨테이너에 이미 붙어 있는 모든 group 을 새 백엔드에 붙인다. */
	list_for_each_entry(group, &container->group_list, container_next) {
		/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_attach_group.
		 * 그 함수가 IOMMU domain 을 만들거나 재사용하고 iommu_attach_group 으로 실제 결합을
		 * 수행하며, group 의 type 에 따라 emulated/noiommu 를 다르게 처리한다. */
		ret = driver->ops->attach_group(data, group->iommu_group,
						group->type);
		/* [한국어] attach_group 이 0 이 아닌 값을 돌려주면 이 group 은 붙지 못한 것이다. */
		if (ret)
			/* [한국어] 하나라도 실패하면 이미 붙인 것들을 모두 되돌려야 한다. */
			goto unwind;
	}

	/* [한국어] 루프를 끝까지 돌았으면 ret 은 **마지막 반복의 반환값**, 즉 0 이다.
	 * 여기서 0 을 새로 넣지 않고 루프 변수를 그대로 쓰는 형태다. */
	return ret;

/* [한국어] 실패 되감기 라벨. 이 시점 group 은 **실패한** 항목을 가리킨다. */
unwind:
	/* [한국어] 현재 커서의 **바로 앞** 항목부터 역방향으로 돈다. 실패한 group 자신은 붙지 않았으므로
	 * 되돌릴 대상이 아니고, 그 앞의 성공한 것들만 떼면 된다. 이 매크로가 커서를 한 칸
	 * 뒤로 옮긴 뒤 시작하므로 그 경계가 정확히 맞는다. */
	list_for_each_entry_continue_reverse(group, &container->group_list,
					     container_next) {
		/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_detach_group.
		 * 반환값이 없어 되감기는 실패할 수 없다. */
		driver->ops->detach_group(data, group->iommu_group);
	}

	/* [한국어] 실패 코드를 그대로 올린다. 호출자는 이 백엔드를 포기하고 다음 후보로 넘어간다. */
	return ret;
}

/* [한국어]
 * vfio_ioctl_set_iommu - VFIO_SET_IOMMU ioctl 핸들러, 백엔드를 고르고 활성화한다
 *
 * @container: 컨테이너 fd 의 private_data.
 * @arg: 사용자가 요청한 IOMMU 타입(VFIO_TYPE1v2_IOMMU 등).
 * @return: 0 성공. -EINVAL(group 이 하나도 없거나 이미 SET_IOMMU 됨),
 *          -ENODEV(맞는 백엔드 없음), 그 밖에 후보의 open 이나 결합이 낸 오류.
 *
 * 왜 필요한가: **컨테이너에 IOVA 주소공간을 실제로 만들어 주는 지점**이다. 이 ioctl 이
 * 성공해야 VFIO_IOMMU_MAP_DMA 같은 명령이 열리고, group.c 의 vfio_group_use_container 가
 * 첫 device open 을 허락한다.
 *
 * 권한 모델(상류 주석이 그대로 설명한다): 컨테이너 노드는 0666 이라 누구나 열 수 있는
 * **비특권 인터페이스**다. 특권은 group 을 붙여야 생긴다 — group 노드의 권한이 곧
 * 디바이스 접근 권한이기 때문이다. 그래서 group 이 하나도 없으면 IOMMU 를 켤 수 없다.
 * unset_iommu 가 따로 없고, 모든 group 을 떼면 자동으로 비특권 상태로 돌아간다
 * (vfio_group_detach_container 의 마지막 정리).
 *
 * 동작 과정:
 *  1. group_lock 을 쓰기 모드로 잡고 두 거절 조건을 확인한다.
 *  2. 등록된 백엔드를 순서대로 후보로 놓고, 성격이 맞지 않으면 건너뛴다.
 *  3. try_module_get 으로 후보 모듈을 붙잡는다.
 *  4. ops->ioctl(NULL, VFIO_CHECK_EXTENSION, arg) 로 그 후보가 요청 타입을 지원하는지
 *     묻는다. 상류 주석대로 SET_IOMMU 의 인자 매직이 CHECK_EXTENSION 의 것과 같기 때문에
 *     같은 콜백을 재사용할 수 있고, 한 백엔드가 여러 인터페이스를 지원할 수도 있다.
 *  5. ops->open(arg) 로 인스턴스를 만든다. **성공값이 NULL 일 수 있어 IS_ERR 로 판정한다.**
 *  6. __vfio_container_attach_groups 로 이미 붙어 있는 group 을 모두 결합한다.
 *  7. 성공하면 iommu_driver 와 iommu_data 를 저장하고 break — **모듈 참조는 놓지 않고
 *     컨테이너가 물려받는다.**
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). container->group_lock 쓰기 락과
 * vfio.iommu_drivers_lock 을 모두 쥔 채 백엔드 콜백을 부른다.
 *
 * 에러 경로: 4/5/6단계 실패는 각각 module_put(과 5, 6단계는 ops->release 도) 을 하고
 * 다음 후보로 continue 한다. 후보가 모두 소진되면 마지막 오류나 초기값 -ENODEV 가 나간다.
 *
 * 백엔드 dispatch 세 곳: ops->ioctl(vfio_iommu_type1_ioctl),
 * ops->open(vfio_iommu_type1_open), ops->release(vfio_iommu_type1_release).
 * noiommu 면 각각 vfio_noiommu_ioctl, vfio_noiommu_open, vfio_noiommu_release 다.
 *
 * 모듈 참조: 7단계에서 유지된 참조의 짝은 vfio_group_detach_container 의 module_put 이다.
 * 빠뜨리면 vfio_iommu_type1 모듈이 영원히 언로드되지 않는다.
 *
 * 호출 체인:
 *   ioctl(container, VFIO_SET_IOMMU) → vfio_fops_unl_ioctl → [vfio_ioctl_set_iommu]
 *     → vfio_iommu_driver_allowed / try_module_get / ops->ioctl / ops->open /
 *       __vfio_container_attach_groups
 */
static long vfio_ioctl_set_iommu(struct vfio_container *container,
				 unsigned long arg)
{
	/* [한국어] 후보 백엔드 순회 커서. */
	struct vfio_iommu_driver *driver;
	/* [한국어] 기본값 -ENODEV — 어떤 백엔드도 맞지 않았을 때의 답. */
	long ret = -ENODEV;

	/* [한국어] 쓰기 모드로 잡는다. iommu_driver 와 iommu_data 를 바꾸고 모든 group 을 붙이는
	 * 구간이라 배타 접근이 필요하다. */
	down_write(&container->group_lock);

	/*
	 * The container is designed to be an unprivileged interface while
	 * the group can be assigned to specific users.  Therefore, only by
	 * adding a group to a container does the user get the privilege of
	 * enabling the iommu, which may allocate finite resources.  There
	 * is no unset_iommu, but by removing all the groups from a container,
	 * the container is deprivileged and returns to an unset state.
	 */
	/* [한국어] 두 가지 거절 조건. group 이 하나도 없으면 아직 **비특권 컨테이너** 라 IOMMU 를
	 * 켤 수 없고(상류 주석이 설명하는 권한 모델의 핵심 — 컨테이너 자체는 누구나 열 수
	 * 있지만 group 을 붙여야 특권이 생긴다), 이미 백엔드가 정해졌으면 두 번 설정할 수 없다.
	 * unset_iommu 가 따로 없고, 모든 group 을 떼면 자동으로 비특권 상태로 돌아간다. */
	if (list_empty(&container->group_list) || container->iommu_driver) {
		/* [한국어] goto 를 쓰지 않고 직접 푼다. */
		up_write(&container->group_lock);
		/* [한국어] -EINVAL. */
		return -EINVAL;
	}

	/* [한국어] 백엔드 목록을 순회하는 동안 등록/해제와 직렬화한다. */
	mutex_lock(&vfio.iommu_drivers_lock);
	/* [한국어] 등록된 백엔드를 하나씩 후보로 놓는다. */
	list_for_each_entry(driver, &vfio.iommu_drivers_list, vfio_next) {
		/* [한국어] 그 백엔드가 open 에서 돌려줄 인스턴스. */
		void *data;

		/* [한국어] 컨테이너의 noiommu 성격과 맞지 않는 백엔드는 건너뛴다. */
		if (!vfio_iommu_driver_allowed(container, driver))
			/* [한국어] 다음 후보로. */
			continue;
		/* [한국어] 후보를 시험하는 동안 그 모듈이 언로드되지 않게 붙잡는다.
		 * **성공하면 이 참조를 놓지 않고 유지한다** — 컨테이너가 그 백엔드를 쓰는 동안
		 * rmmod 를 막아야 하기 때문이다. 짝은 vfio_group_detach_container 의 module_put 과
		 * 아래 세 실패 경로의 module_put 이다. */
		if (!try_module_get(driver->ops->owner))
			/* [한국어] 붙잡지 못했으면 다음 후보로. */
			continue;

		/*
		 * The arg magic for SET_IOMMU is the same as CHECK_EXTENSION,
		 * so test which iommu driver reported support for this
		 * extension and call open on them.  We also pass them the
		 * magic, allowing a single driver to support multiple
		 * interfaces if they'd like.
		 */
		/* [한국어] **백엔드 dispatch**: 이 후보가 사용자가 요청한 타입을 지원하는지 먼저 묻는다.
		 * 상류 주석대로 SET_IOMMU 의 인자 매직이 CHECK_EXTENSION 의 것과 같기 때문에
		 * 같은 콜백을 재사용할 수 있고, 한 백엔드가 여러 인터페이스를 지원할 수도 있다.
		 * type1 이면 vfio_iommu_type1_ioctl 이 TYPE1/TYPE1v2 등에 1 을 답한다.
		 * noiommu 면 vfio_noiommu_ioctl 이 VFIO_NOIOMMU_IOMMU 에만 1 을 답한다. */
		if (driver->ops->ioctl(NULL, VFIO_CHECK_EXTENSION, arg) <= 0) {
			/* [한국어] 지원하지 않으므로 방금 잡은 모듈 참조를 놓고 */
			module_put(driver->ops->owner);
			/* [한국어] 다음 후보로 넘어간다. */
			continue;
		}

		/* [한국어] **백엔드 dispatch**: 인스턴스를 만든다. type1 이면 vfio_iommu_type1_open 이
		 * struct vfio_iommu 를 할당하고 IOVA 리스트와 domain 리스트를 초기화한다.
		 * noiommu 면 vfio_noiommu_open 이 권한만 확인하고 NULL 을 준다 —
		 * 그래서 성공/실패 판정에 NULL 이 아니라 IS_ERR 을 쓴다. */
		data = driver->ops->open(arg);
		/* [한국어] 오류 포인터면 실패다. */
		if (IS_ERR(data)) {
			/* [한국어] 오류 코드를 꺼내 둔다. 다음 후보가 성공하면 이 값은 덮어써진다. */
			ret = PTR_ERR(data);
			/* [한국어] 모듈 참조를 되돌리고 */
			module_put(driver->ops->owner);
			/* [한국어] 다음 후보로. */
			continue;
		}

		/* [한국어] 이미 붙어 있는 group 들을 새 인스턴스에 모두 붙인다. 하나라도 실패하면
		 * 그 안에서 앞의 것들이 모두 되감긴다. */
		ret = __vfio_container_attach_groups(container, driver, data);
		/* [한국어] 결합 실패. */
		if (ret) {
			/* [한국어] **백엔드 dispatch**: 방금 만든 인스턴스를 해제한다. type1 이면
			 * vfio_iommu_type1_release 가 IOVA 리스트와 domain 을 모두 정리한다. */
			driver->ops->release(data);
			/* [한국어] 모듈 참조도 되돌린다. */
			module_put(driver->ops->owner);
			/* [한국어] 다음 후보로. */
			continue;
		}

		/* [한국어] 성공. 이제부터 이 컨테이너의 모든 IOMMU 계열 ioctl 이 이 백엔드로 간다.
		 * 여기 저장된 뒤로는 try_module_get 참조가 컨테이너 소유가 된다. */
		container->iommu_driver = driver;
		/* [한국어] 인스턴스도 함께 저장한다. 둘은 항상 함께 설정되고 함께 지워진다. */
		container->iommu_data = data;
		/* [한국어] 첫 성공에서 멈춘다. ret 에는 __vfio_container_attach_groups 가 준 0 이 들어 있다. */
		break;
	}

	/* [한국어] 백엔드 목록 락을 푼다. */
	mutex_unlock(&vfio.iommu_drivers_lock);
	/* [한국어] 컨테이너 쓰기 락을 푼다. */
	up_write(&container->group_lock);

	/* [한국어] 0 이면 성공. 실패면 마지막으로 시도한 후보의 오류이거나, 후보가 하나도 없었으면
	 * 초기값 -ENODEV 다. */
	return ret;
}

/* [한국어]
 * vfio_fops_unl_ioctl - 컨테이너 fd 의 ioctl 디스패처
 *
 * @filep: 컨테이너 fd 의 struct file.
 * @cmd: ioctl 번호.
 * @arg: 명령별 인자.
 * @return: 각 갈래의 결과. 백엔드가 없는 상태에서 미지원 명령이면 초기값 -EINVAL.
 *
 * 왜 필요한가: 컨테이너 fd 가 받는 명령은 두 부류다. 코어가 직접 답하는 셋
 * (GET_API_VERSION, CHECK_EXTENSION, SET_IOMMU)과, 나머지 전부 — 그것들은 **선택된
 * 백엔드로 그대로 넘어간다**. 상류 주석의 "passthrough all unrecognized ioctls" 가
 * 그 뜻이며, 이 구조 덕분에 VFIO 코어는 VFIO_IOMMU_MAP_DMA 의 인자 형식조차 알 필요가 없다.
 *
 * 동작 과정:
 *  - VFIO_GET_API_VERSION : 상수 VFIO_API_VERSION(0)을 답한다. 사용자 공간이 가장 먼저
 *    부르는 ioctl 이다.
 *  - VFIO_CHECK_EXTENSION : 전용 함수로 넘긴다.
 *  - VFIO_SET_IOMMU : 백엔드 선택 함수로 넘긴다.
 *  - default : 백엔드가 정해져 있으면 ops->ioctl 로 넘긴다. 정해지지 않았으면 초기값
 *    -EINVAL 이 나가 "SET_IOMMU 를 먼저 하라" 는 뜻이 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. default 갈래는 **락을 잡지 않고** iommu_driver 를 읽는다.
 * 그 시점에 백엔드가 바뀌려면 마지막 group 이 떨어져야 하는데, group 이 떨어지려면 그
 * group fd 를 통해 열린 device fd 가 모두 닫혀야 한다.
 *
 * 에러 경로: 각 갈래가 정한 값을 그대로 돌려준다.
 *
 * 백엔드 dispatch: default 갈래의 ops->ioctl 이 **이 파일에서 가장 뜨거운 경로**다.
 * type1 이면 vfio_iommu_type1_ioctl 로 가고, 사용자 공간의 VFIO_IOMMU_MAP_DMA 가 거기서
 * vfio_dma_do_map 으로 이어져 hugepage 를 핀하고 IOVA 에 매핑한다. SPDK 나 DPDK 가
 * NVMe 를 직접 몰 때 PRP/SGL 에 싣는 주소가 곧 그 IOVA 다.
 *
 * 호출 체인:
 *   ioctl(2) → VFS → [vfio_fops_unl_ioctl]
 *     → vfio_container_ioctl_check_extension / vfio_ioctl_set_iommu / ops->ioctl
 */
static long vfio_fops_unl_ioctl(struct file *filep,
				unsigned int cmd, unsigned long arg)
{
	/* [한국어] vfio_fops_open 이 넣어 둔 컨테이너 포인터. */
	struct vfio_container *container = filep->private_data;
	/* [한국어] default 갈래에서 쓸 백엔드. */
	struct vfio_iommu_driver *driver;
	/* [한국어] 그 백엔드의 인스턴스. */
	void *data;
	/* [한국어] 기본 반환값. */
	long ret = -EINVAL;

	/* [한국어] [상류 코드 관찰] vfio_fops_open 은 컨테이너 할당에 실패하면 -ENOMEM 을 돌려주므로
	 * private_data 가 NULL 인 채로 파일이 열리는 경로는 이 파일 안에 없다. 그런데도
	 * 이 방어 검사가 남아 있다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	if (!container)
		/* [한국어] -EINVAL. */
		return ret;

	/* [한국어] 컨테이너 fd 가 직접 처리하는 명령은 셋이고, 나머지는 백엔드로 넘어간다. */
	switch (cmd) {
	/* [한국어] ABI 세대 조회. */
	case VFIO_GET_API_VERSION:
		/* [한국어] include/uapi/linux/vfio.h:19 의 상수 0 을 그대로 답한다. 사용자 공간이 가장 먼저
		 * 부르는 ioctl 이며, 값이 다르면 라이브러리가 아예 진행하지 않는다. */
		ret = VFIO_API_VERSION;
		/* [한국어] 처리 끝. */
		break;
	/* [한국어] 특정 확장을 지원하는지 조회. */
	case VFIO_CHECK_EXTENSION:
		/* [한국어] 백엔드들에게 물어보는 전용 함수로 넘긴다. */
		ret = vfio_container_ioctl_check_extension(container, arg);
		/* [한국어] 처리 끝. */
		break;
	/* [한국어] IOMMU 백엔드 선택 및 활성화. */
	case VFIO_SET_IOMMU:
		/* [한국어] 후보 순회와 인스턴스 생성, group 결합을 담당하는 함수로 넘긴다. */
		ret = vfio_ioctl_set_iommu(container, arg);
		/* [한국어] 처리 끝. */
		break;
	/* [한국어] 그 밖의 모든 명령 — VFIO_IOMMU_MAP_DMA, UNMAP_DMA, GET_INFO, DIRTY_PAGES 등. */
	default:
		/* [한국어] **락 없이** 백엔드를 읽는다. 이 시점에 백엔드가 바뀌려면 마지막 group 이 떨어져야
		 * 하는데, group 이 하나라도 있어야 백엔드가 정해질 수 있고 group 이 떨어지려면
		 * 그 group fd 를 통해 열린 device fd 가 모두 닫혀야 한다. */
		driver = container->iommu_driver;
		/* [한국어] 인스턴스도 함께 떠 둔다. */
		data = container->iommu_data;

		/* [한국어] 백엔드가 정해져 있을 때만 넘긴다. 정해지지 않았으면 초기값 -EINVAL 이 그대로 나가
		 * "SET_IOMMU 를 먼저 하라" 는 뜻이 된다. */
		if (driver) /* passthrough all unrecognized ioctls */
			/* [한국어] **백엔드 dispatch — 이 파일에서 가장 뜨거운 경로**: type1 이면
			 * vfio_iommu_type1_ioctl 로 간다. 사용자 공간의 VFIO_IOMMU_MAP_DMA 가 여기를 지나
			 * vfio_dma_do_map 으로 가서 hugepage 를 핀하고 IOVA 에 매핑한다.
			 * SPDK 나 DPDK 가 NVMe 를 직접 몰 때 PRP/SGL 에 싣는 주소가 곧 그 IOVA 다. */
			ret = driver->ops->ioctl(data, cmd, arg);
	}

	/* [한국어] 각 갈래가 정한 값이 ioctl 반환값이 된다. */
	return ret;
}

/* [한국어]
 * vfio_fops_open - /dev/vfio/vfio 를 열어 빈 컨테이너를 하나 만든다
 *
 * @inode: 열리는 misc 디바이스의 inode(쓰이지 않는다).
 * @filep: 새 struct file. private_data 에 컨테이너를 심는다.
 * @return: 0 성공, -ENOMEM.
 *
 * 왜 필요한가: 컨테이너 객체는 fd 하나당 하나씩 생긴다. 이 함수가 그것을 만들고
 * 초기 상태(group 없음, 백엔드 없음)로 둔다.
 *
 * **권한 검사가 없다는 점이 중요하다.** 노드 권한이 0666 이라 누구나 열 수 있고,
 * 이 시점의 컨테이너는 아무 특권도 없는 빈 껍데기다. 특권은 group 을 붙일 때 생기며,
 * group 노드의 권한이 그 관문이다.
 *
 * 동작 과정: GFP_KERNEL_ACCOUNT 로 할당(여는 프로세스의 memcg 에 과금하므로 fd 를
 * 무한히 열어 커널 메모리를 고갈시키는 공격이 cgroup 한도에 걸린다) → group_list 초기화
 * → rwsem 초기화 → kref 를 1 로 → private_data 대입.
 *
 * 실행 컨텍스트: 프로세스 문맥(open 시스템 콜). 할당이 있어 잠들 수 있다.
 *
 * 에러 경로: 할당 실패면 -ENOMEM 이고 파일은 만들어지지 않는다.
 *
 * refcount: kref_init 의 1 이 **컨테이너 fd 자신의 몫**이며, 짝은 vfio_fops_release 의
 * vfio_container_put 이다. group 이 붙을 때마다 vfio_container_get 이 추가로 올린다.
 *
 * 호출 체인:
 *   open("/dev/vfio/vfio") → VFS → misc 코어 → [vfio_fops_open]
 */
static int vfio_fops_open(struct inode *inode, struct file *filep)
{
	/* [한국어] 새로 만들 컨테이너. */
	struct vfio_container *container;

	/* [한국어] GFP_KERNEL_ACCOUNT 로 할당한다. **이 메모리를 여는 프로세스의 memcg 에 과금**하므로,
	 * 컨테이너 fd 를 무한히 열어 커널 메모리를 고갈시키는 공격이 cgroup 한도에 걸린다.
	 * 컨테이너 노드가 누구나 열 수 있는 비특권 인터페이스라서 이 과금이 중요하다. */
	container = kzalloc_obj(*container, GFP_KERNEL_ACCOUNT);
	/* [한국어] 메모리 부족. */
	if (!container)
		/* [한국어] -ENOMEM. 파일은 만들어지지 않는다. */
		return -ENOMEM;

	/* [한국어] 아직 붙은 group 이 없다 — 비특권 상태. */
	INIT_LIST_HEAD(&container->group_list);
	/* [한국어] group_list 와 백엔드 필드를 지킬 읽기-쓰기 세마포어를 초기화한다. */
	init_rwsem(&container->group_lock);
	/* [한국어] 참조를 1 로 시작한다. 이것이 **컨테이너 fd 자신의 몫**이며,
	 * 짝은 vfio_fops_release 의 vfio_container_put 이다.
	 * group 이 붙을 때마다 vfio_container_get 이 추가로 올린다. */
	kref_init(&container->kref);

	/* [한국어] 이후 ioctl 이 이 fd 에서 컨테이너를 되찾을 수 있게 한다. */
	filep->private_data = container;

	/* [한국어] 성공. 이 시점의 컨테이너는 IOMMU 백엔드도 group 도 없는 빈 껍데기이며,
	 * 아무 특권도 없다. */
	return 0;
}

/* [한국어]
 * vfio_fops_release - 컨테이너 fd 를 닫는다 (메모리는 아직 해제되지 않을 수 있다)
 *
 * @inode: 닫히는 노드의 inode(쓰이지 않는다).
 * @filep: 닫히는 파일.
 * @return: 항상 0.
 *
 * 왜 필요한가: fd 가 닫혔다고 컨테이너가 곧바로 사라져서는 안 된다. group 이 아직
 * 붙어 있을 수 있기 때문이다. 그래서 이 함수는 참조 하나만 놓고, 실제 해제는
 * 마지막 참조가 빠질 때 vfio_container_release 가 한다.
 *
 * 동작 과정: private_data 를 끊고 vfio_container_put 을 한 번 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥(마지막 fput).
 *
 * 에러 경로: 없다.
 *
 * 수명: 상류 주석대로 "컨테이너/group/device 를 어떤 순서로 닫아도 안전" 해야 하고,
 * 그 요구가 이 함수의 형태를 결정했다. 사용자가 컨테이너 fd 를 먼저 닫으면 group 이
 * 쥔 참조가 남아 컨테이너가 살아 있고, 나중에 group 이 detach 될 때 해제된다.
 *
 * 호출 체인:
 *   close(container fd) → VFS 마지막 fput → [vfio_fops_release] → vfio_container_put
 */
static int vfio_fops_release(struct inode *inode, struct file *filep)
{
	/* [한국어] open 때 넣어 둔 컨테이너를 되찾는다. */
	struct vfio_container *container = filep->private_data;

	/* [한국어] 먼저 끊는다. 이 뒤로는 이 파일로 컨테이너에 접근할 수 없다. */
	filep->private_data = NULL;

	/* [한국어] kref_init 의 1 을 되돌린다. **아직 group 이 붙어 있으면 그 group 들이 참조를
	 * 쥐고 있어 0 이 되지 않고**, 마지막 group 이 detach 될 때 비로소 해제된다.
	 * 상류 주석이 말하는 "컨테이너/group/device 가 어떤 순서로 닫혀도 안전" 이 이것이다. */
	vfio_container_put(container);

	/* [한국어] release 콜백은 실패할 수 없어 항상 0. */
	return 0;
}

/* [한국어] /dev/vfio/vfio 노드의 file_operations. 아래 miscdevice 에 꽂힌다.
 * 이 테이블의 **주소 자체**가 신원 증명으로도 쓰인다 — vfio_container_from_file 이
 * file->f_op 를 이것과 비교해 "정말 컨테이너 fd 인가" 를 판정한다.
 * read/write/mmap 이 없다: 컨테이너는 순수 제어 인터페이스이고,
 * 실제 DMA 매핑조차 ioctl 로만 이루어진다. */
static const struct file_operations vfio_fops = {
	/* [한국어] 이 fops 를 소유한 모듈. 파일이 열려 있는 동안 vfio 코어 모듈의 언로드를 막는다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 가 open 시 사용.
	 * 값 범위: THIS_MODULE.
	 * 동기화: 불변. */
	.owner		= THIS_MODULE,
	/* [한국어] 노드를 열 때 불린다. 컨테이너 객체를 만들어 private_data 에 단다.
	 * **권한 검사가 없다** — 상류 주석대로 컨테이너는 비특권 인터페이스이고 특권은
	 * group 을 붙일 때 생긴다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 open 경로.
	 * 값 범위: 이 파일의 vfio_fops_open.
	 * 동기화: 열 때마다 새 객체를 만들므로 공유 상태가 없다. */
	.open		= vfio_fops_open,
	/* [한국어] 마지막 참조가 사라질 때 불린다. kref 를 하나 내릴 뿐이라 group 이 남아 있으면
	 * 메모리는 해제되지 않는다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 마지막 fput 경로.
	 * 값 범위: 이 파일의 vfio_fops_release.
	 * 동기화: kref 가 원자 연산이라 별도 락이 없다. */
	.release	= vfio_fops_release,
	/* [한국어] 컨테이너 fd 의 ioctl 진입점. 세 명령을 직접 처리하고 나머지는 백엔드로 넘긴다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 ioctl 경로.
	 * 값 범위: 이 파일의 vfio_fops_unl_ioctl.
	 * 동기화: 여러 스레드가 동시에 진입할 수 있고, 직렬화는 각 핸들러가
	 * container->group_lock 으로 한다. */
	.unlocked_ioctl	= vfio_fops_unl_ioctl,
	/* [한국어] 32비트 프로세스용 어댑터. 포인터 폭만 맞춰 unlocked_ioctl 로 넘긴다.
	 * VFIO_IOMMU_MAP_DMA 의 인자 구조체는 __u64 필드만 써서 32/64비트 레이아웃이
	 * 같으므로 별도 변환이 필요 없다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 의 compat ioctl 경로.
	 * 값 범위: 커널 공용 compat_ptr_ioctl.
	 * 동기화: 위와 같다. */
	.compat_ioctl	= compat_ptr_ioctl,
};

/* [한국어]
 * vfio_container_from_file - struct file 이 컨테이너 fd 인지 확인하고 컨테이너를 꺼낸다
 *
 * @file: 검사할 파일. 호출자가 참조를 쥐고 있어야 한다.
 * @return: 컨테이너 fd 이면 그 vfio_container, 아니면 NULL. **참조를 잡지 않는다.**
 *
 * 왜 필요한가: group.c 의 VFIO_GROUP_SET_CONTAINER 는 사용자가 준 fd 가 컨테이너인지
 * iommufd 인지 모르는 채로 받는다. 이 함수가 "컨테이너인가" 를 위조 불가능하게 판정하고,
 * NULL 이면 호출자가 다음 후보(iommufd)를 시험한다 — 그래서 오류가 아니라 NULL 이다.
 *
 * 동작 과정: file->f_op 가 이 파일의 vfio_fops 와 같은 주소인지 본다. fops 주소는 커널
 * 내부 심볼이라 사용자 공간이 위조할 수 없다. group.c 의 vfio_group_from_file 과
 * vfio_main.c 의 vfio_device_from_file 도 같은 기법을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다.
 *
 * 에러 경로: 종류가 다르면 NULL. 상류 주석대로 호출자가 fget 으로 파일을 붙잡고 있어
 * vfio_fops_release 와 경합할 수 없으므로 private_data 가 NULL 일 수 없고, WARN_ON 은
 * 그것을 방어적으로 확인만 한다.
 *
 * 호출 체인:
 *   vfio_group_ioctl_set_container(group.c) → [vfio_container_from_file]
 */
struct vfio_container *vfio_container_from_file(struct file *file)
{
	/* [한국어] 돌려줄 컨테이너. */
	struct vfio_container *container;

	/* Sanity check, is this really our fd? */
	/* [한국어] **신원 검사**: 이 파일의 file_operations 주소가 위 vfio_fops 와 같은지 본다.
	 * fops 주소는 커널 내부 심볼이라 사용자 공간이 위조할 수 없다.
	 * group.c 의 vfio_group_from_file 과 vfio_main.c 의 vfio_device_from_file 도 같은 기법을 쓴다. */
	if (file->f_op != &vfio_fops)
		/* [한국어] 컨테이너 fd 가 아니다. 호출자 vfio_group_ioctl_set_container 는 이 NULL 을 보고
		 * 다음 후보(iommufd fd)를 시험한다 — 그래서 오류가 아니라 NULL 이다. */
		return NULL;

	/* [한국어] 종류가 맞으므로 private_data 를 그대로 꺼낸다. */
	container = file->private_data;
	/* [한국어] 상류 주석대로 호출자가 fget 으로 파일을 붙잡고 있으므로 vfio_fops_release 와
	 * 경합할 수 없고, 따라서 private_data 가 NULL 일 수 없다. 그래도 방어적으로 확인한다. */
	WARN_ON(!container); /* fget ensures we don't race vfio_release */
	/* [한국어] 검증된 컨테이너. **참조를 잡지 않는다** — 호출자가 파일 참조를 쥐고 있는 동안만
	 * 유효하다. 실제 참조는 vfio_container_attach_group 이 성공할 때 잡는다. */
	return container;
}

/* [한국어] 컨테이너 노드 /dev/vfio/vfio 를 만드는 misc 디바이스 서술자.
 * group 노드처럼 동적 major 를 쓰지 않는 이유는 컨테이너 노드가 시스템에 **단 하나**
 * 이기 때문이다. vfio_container_init 이 misc_register 로 등록한다. */
static struct miscdevice vfio_dev = {
	/* [한국어] 고정 minor 번호. 사용자 공간과 배포판 udev 규칙이 이 번호를 안다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: misc_register.
	 * 값 범위: include/linux/miscdevice.h 의 VFIO_MINOR(이 트리에 없어 값은 확인 못 함).
	 * 동기화: 불변. */
	.minor = VFIO_MINOR,
	/* [한국어] misc 디바이스 이름. /proc/misc 에 이 이름으로 보인다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: misc 코어.
	 * 값 범위: 문자열 "vfio".
	 * 동기화: 불변. */
	.name = "vfio",
	/* [한국어] 이 노드의 file_operations.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: VFS 가 open 시 file->f_op 에 복사한다 — 그래서
	 * vfio_container_from_file 의 주소 비교가 성립한다.
	 * 값 범위: 이 파일의 vfio_fops.
	 * 동기화: 불변. */
	.fops = &vfio_fops,
	/* [한국어] devtmpfs 가 만들 노드 경로. "vfio/vfio" 라 실제 노드는 /dev/vfio/vfio 가 된다 —
	 * group 노드들과 같은 디렉터리에 놓인다.
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: misc 코어와 devtmpfs.
	 * 값 범위: 문자열 "vfio/vfio".
	 * 동기화: 불변. */
	.nodename = "vfio/vfio",
	/* [한국어] 노드 권한 0666 — **모든 사용자가 읽고 쓸 수 있다**. 컨테이너는 그 자체로 아무
	 * 특권도 주지 않는 빈 껍데기이고(vfio_fops_open 에 권한 검사가 없다), 실제 권한은
	 * group 노드를 열 수 있느냐로 결정되기 때문이다. group 노드는 반대로 기본 권한이
	 * 제한적이고 udev 가 소유자를 바꿔 준다(group.c 의 vfio_devnode).
	 * 설정자: 컴파일 시 고정.
	 * 읽는 자: devtmpfs 가 노드를 만들 때.
	 * 값 범위: S_IRUGO | S_IWUGO = 0666.
	 * 동기화: 불변. */
	.mode = S_IRUGO | S_IWUGO,
};

/* [한국어]
 * vfio_container_attach_group - group 을 컨테이너에 붙인다 (SET_CONTAINER 의 실체)
 *
 * @container: 붙일 대상 컨테이너.
 * @group: 붙일 group. 호출자가 group->group_lock 을 쥐고 있어야 한다.
 * @return: 0 성공. -EPERM(noiommu 인데 RAWIO 없음, 또는 진짜/가짜 group 혼용),
 *          iommu_group_claim_dma_owner 나 백엔드 attach_group 이 낸 오류.
 *
 * 왜 필요한가: **3층 모델에서 group 과 container 를 잇는 유일한 지점**이다. 여기서
 * 세 가지 일이 한꺼번에 일어난다 — (1) 커널로부터 DMA 소유권을 빼앗아 격리를 성립시키고,
 * (2) 이미 백엔드가 정해져 있으면 즉시 결합하며, (3) 컨테이너 참조와 container_users 를
 * 세운다.
 *
 * 동작 과정:
 *  1. noiommu group 이면 CAP_SYS_RAWIO 를 요구한다(group.c 의 노드 open 검사와 별개로
 *     다시 확인하는 이유는, 열린 group fd 를 권한 낮은 프로세스에 물려줄 수 있기 때문이다).
 *  2. group_lock 을 쓰기 모드로 잡는다.
 *  3. 이미 group 이 있으면 noiommu 성격이 같은지 본다 — 진짜 격리와 가짜 격리가
 *     한 IOVA 주소공간에 섞이면 격리 보증이 무의미해진다.
 *  4. 진짜 IOMMU group 이면 iommu_group_claim_dma_owner 로 **DMA 소유권을 가져온다**.
 *     성공하면 group 안 모든 디바이스에서 커널 드라이버가 쫓겨나고 이후 바인딩도 막힌다.
 *     group 에 아직 커널 드라이버가 붙어 있으면 실패한다.
 *  5. 백엔드가 이미 정해져 있으면 ops->attach_group 으로 즉시 결합한다. 아직이면
 *     나중에 SET_IOMMU 가 __vfio_container_attach_groups 로 한꺼번에 붙인다.
 *  6. group->container, container_users = 1, container->noiommu, 목록 추가,
 *     그리고 vfio_container_get.
 *
 * 실행 컨텍스트: 프로세스 문맥(SET_CONTAINER ioctl). 락 순서는
 * group->group_lock → container->group_lock 이며 역순은 이 트리에 없다.
 *
 * 에러 경로: 5단계 실패는 4단계의 소유권을 되돌린 뒤 나간다. 그것을 빠뜨리면 group 이
 * 영원히 잠겨 커널 드라이버도 다른 컨테이너도 그 디바이스를 쓸 수 없게 된다.
 *
 * refcount: 6단계의 vfio_container_get 은 "group 이 컨테이너를 붙들고 있다" 는 표시이며
 * 짝은 vfio_group_detach_container 의 vfio_container_put 이다.
 * container_users = 1 은 **group 자신의 몫**이고, device fd 마다
 * vfio_group_use_container 가 +1 을 더한다.
 *
 * 백엔드 dispatch: ops->attach_group — type1 이면 vfio_iommu_type1_attach_group.
 *
 * 호출 체인:
 *   ioctl(group, VFIO_GROUP_SET_CONTAINER) → vfio_group_ioctl_set_container(group.c)
 *     → [vfio_container_attach_group]
 *     → iommu_group_claim_dma_owner / ops->attach_group / vfio_container_get
 */
int vfio_container_attach_group(struct vfio_container *container,
				struct vfio_group *group)
{
	/* [한국어] 이미 정해진 백엔드가 있으면 그것으로 즉시 결합해야 하므로 꺼내 둘 변수. */
	struct vfio_iommu_driver *driver;
	/* [한국어] 결과 코드. */
	int ret = 0;

	/* [한국어] 호출자 group.c 의 vfio_group_ioctl_set_container 가 group->group_lock 을 쥐고 왔다.
	 * 그 락이 group->container 와 container_users 를 지킨다. */
	lockdep_assert_held(&group->group_lock);

	/* [한국어] noiommu group 을 컨테이너에 붙이려면 CAP_SYS_RAWIO 가 필요하다.
	 * group.c 의 vfio_group_fops_open 에서 이미 한 번 검사했지만, 열려 있는 group fd 를
	 * 권한이 낮은 프로세스에 물려줄 수 있으므로 여기서 다시 확인한다. */
	if (group->type == VFIO_NO_IOMMU && !capable(CAP_SYS_RAWIO))
		/* [한국어] -EPERM. */
		return -EPERM;

	/* [한국어] group_list 와 백엔드 상태를 바꾸는 구간이라 쓰기 모드로 잡는다. */
	down_write(&container->group_lock);

	/* Real groups and fake groups cannot mix */
	/* [한국어] 이미 group 이 하나라도 있으면 컨테이너의 noiommu 성격이 확정돼 있다.
	 * 새 group 의 성격이 그와 다르면 거절한다 — 진짜 격리와 가짜 격리가 한 IOVA
	 * 주소공간에 섞이면 격리 보증 자체가 무의미해진다. */
	if (!list_empty(&container->group_list) &&
	    container->noiommu != (group->type == VFIO_NO_IOMMU)) {
		ret = -EPERM;
		/* [한국어] -EPERM 으로 거절하고 락을 푸는 출구로 간다. */
		goto out_unlock_container;
	}

	/* [한국어] 진짜 IOMMU group 일 때만. */
	if (group->type == VFIO_IOMMU) {
		/* [한국어] **격리의 실체가 되는 호출**: IOMMU 코어에게 이 group 의 DMA 소유권을 달라고 한다.
		 * 성공하면 group 안 모든 디바이스에서 커널 드라이버가 쫓겨나고, 이후 다른 드라이버가
		 * 바인딩되지 못한다. 두 번째 인자 group 은 소유자 토큰이며, 같은 토큰으로만 놓을 수
		 * 있다. group 안에 아직 커널 드라이버가 붙은 디바이스가 있으면 실패한다 —
		 * 이것이 사용자가 GET_STATUS 에서 VIABLE 을 확인해야 하는 이유다.
		 * 구현(drivers/iommu)은 이 트리에 없어 확인 못 함. */
		ret = iommu_group_claim_dma_owner(group->iommu_group, group);
		/* [한국어] DMA 소유권 획득이 실패하면 group 안에 아직 커널 드라이버에 잡힌 디바이스가 있다는 뜻이다. */
		if (ret)
			/* [한국어] 소유권을 못 얻었다. 아무것도 바꾸지 않은 채 나간다. */
			goto out_unlock_container;
	}

	/* [한국어] 컨테이너에 이미 백엔드가 정해져 있는지 본다. */
	driver = container->iommu_driver;
	/* [한국어] 정해져 있다면 지금 붙이는 group 도 즉시 그 백엔드에 결합해야 한다.
	 * 아직 없다면(SET_IOMMU 전) 나중에 __vfio_container_attach_groups 가 한꺼번에 붙인다. */
	if (driver) {
		/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_attach_group 으로 간다.
		 * 그 함수가 기존 domain 에 합류시키거나 새 domain 을 만들고, IOVA 창을 재계산하며,
		 * group->type 에 따라 emulated/noiommu 를 분기 처리한다. */
		ret = driver->ops->attach_group(container->iommu_data,
						group->iommu_group,
						group->type);
		/* [한국어] 백엔드 결합 실패. */
		if (ret) {
			/* [한국어] 진짜 IOMMU group 이었다면 */
			if (group->type == VFIO_IOMMU)
				/* [한국어] 방금 얻은 DMA 소유권을 되돌린다. 이것을 빠뜨리면 group 이 영원히 잠겨
				 * 커널 드라이버도 다른 컨테이너도 그 디바이스를 쓸 수 없게 된다. */
				iommu_group_release_dma_owner(
					group->iommu_group);
			/* [한국어] 락을 푸는 출구로. */
			goto out_unlock_container;
		}
	}

	/* [한국어] group 이 이 컨테이너에 속했음을 기록한다. 이 대입이 곧 group.c 의
	 * vfio_group_has_iommu 가 참이 되는 순간이다. */
	group->container = container;
	/* [한국어] 사용자 수를 1 로 시작한다. 이 1 은 **group 자신의 몫**이며,
	 * device fd 가 열릴 때마다 vfio_group_use_container 가 +1 한다.
	 * group.c 의 UNSET_CONTAINER 가 "1 이 아니면 -EBUSY" 로 거절하는 대상이 이 값이다.
	 * container 대입과 이 대입이 한 쌍이라 vfio_group_has_iommu 의 WARN_ON 불변식이 성립한다. */
	group->container_users = 1;
	/* [한국어] 컨테이너의 noiommu 성격을 이 group 의 성격으로 확정한다. 이후 다른 성격은 거절된다. */
	container->noiommu = (group->type == VFIO_NO_IOMMU);
	/* [한국어] 컨테이너의 group 목록에 넣는다. 이 뒤로 SET_IOMMU 와 CHECK_EXTENSION 이 이 group 을 본다. */
	list_add(&group->container_next, &container->group_list);

	/* Get a reference on the container and mark a user within the group */
	/* [한국어] **컨테이너 참조 +1**. 상류 주석이 밝히듯 group 이 컨테이너를 붙들고 있다는 표시이며,
	 * 짝은 vfio_group_detach_container 의 vfio_container_put 이다.
	 * 이것이 있어서 사용자가 컨테이너 fd 를 먼저 닫아도 group 이 붙어 있는 한 컨테이너
	 * 메모리가 살아 있다. */
	vfio_container_get(container);

/* [한국어] 성공과 실패가 모두 합류하는 출구. */
out_unlock_container:
	/* [한국어] 쓰기 락을 푼다. */
	up_write(&container->group_lock);
	/* [한국어] 0 또는 -EPERM / 백엔드 오류가 group.c 를 거쳐 ioctl 반환값이 된다. */
	return ret;
}

/* [한국어]
 * vfio_group_detach_container - group 을 컨테이너에서 떼고, 마지막이면 백엔드까지 해제한다
 *
 * @group: 떼어낼 group. 호출자가 group->group_lock 을 쥐고 있어야 한다.
 * @return: 없음. **실패할 수 없다** — 되돌릴 곳이 없다.
 *
 * 왜 필요한가: vfio_container_attach_group 의 짝이며, 세 곳에서 불린다 —
 * UNSET_CONTAINER, group fd 해제, 그리고 마지막 디바이스가 뽑힐 때
 * (group.c 의 vfio_device_remove_group). 어느 경로든 이 시점에는 이 group 을 통해 열린
 * device fd 가 하나도 없어야 하고, WARN_ON(container_users != 1) 이 그것을 확인한다.
 *
 * 동작 과정(모두 container->group_lock 쓰기 락 아래):
 *  1. 백엔드가 있으면 ops->detach_group 으로 결합을 끊는다.
 *  2. 진짜 IOMMU group 이면 iommu_group_release_dma_owner 로 소유권을 커널에 돌려준다.
 *     이 뒤로 커널 드라이버가 다시 바인딩될 수 있다.
 *  3. group->container = NULL, container_users = 0, 목록에서 제거.
 *  4. **마지막 group 이었다면** 상류 주석대로 컨테이너를 비특권 상태로 되돌린다 —
 *     ops->release 로 백엔드 인스턴스를 해제하고, module_put 으로 백엔드 모듈 참조를
 *     놓고, iommu_driver 와 iommu_data 를 NULL 로 비운다. 그러면 다시 SET_IOMMU 를
 *     할 수 있다.
 *  5. 락을 푼 **뒤** vfio_container_put 을 부른다. 마지막 참조라면 kfree 로 이어지므로
 *     락 안에서 부르면 안 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. group->group_lock 을 쥔 채 container->group_lock 을
 * 쓰기 모드로 잡는다.
 *
 * 에러 경로: 없다.
 *
 * refcount 세 개가 여기서 정리된다:
 *  - 백엔드 모듈 참조 : vfio_ioctl_set_iommu 의 try_module_get 짝. 빠뜨리면
 *    vfio_iommu_type1 모듈이 영원히 언로드되지 않는다.
 *  - DMA 소유권 : vfio_container_attach_group 의 claim 짝.
 *  - 컨테이너 참조 : 같은 함수의 vfio_container_get 짝. 이것이 마지막이면 컨테이너
 *    메모리가 해제된다 — 사용자가 컨테이너 fd 를 먼저 닫아 둔 경우가 그 상황이다.
 *
 * 백엔드 dispatch 둘: ops->detach_group(vfio_iommu_type1_detach_group)과
 * ops->release(vfio_iommu_type1_release).
 *
 * 호출 체인:
 *   vfio_group_ioctl_unset_container / vfio_group_fops_release /
 *   vfio_device_remove_group (모두 group.c) → [vfio_group_detach_container]
 *     → ops->detach_group / iommu_group_release_dma_owner / ops->release /
 *       module_put / vfio_container_put
 */
void vfio_group_detach_container(struct vfio_group *group)
{
	/* [한국어] 떼어낼 대상 컨테이너를 group 에서 꺼낸다. */
	struct vfio_container *container = group->container;
	/* [한국어] 정해져 있으면 백엔드 콜백을 불러야 하므로 꺼내 둘 변수. */
	struct vfio_iommu_driver *driver;

	/* [한국어] 호출자(group.c 의 세 지점)가 group->group_lock 을 쥐고 왔다. */
	lockdep_assert_held(&group->group_lock);
	/* [한국어] 여기 도달했다는 것은 이 group 을 통해 열린 device fd 가 하나도 없다는 뜻이다.
	 * UNSET_CONTAINER 는 명시적으로 검사하고, vfio_group_fops_release 는 device fd 가
	 * group 파일 참조를 쥐고 있어 자동으로 보장되며, vfio_device_remove_group 은
	 * 마지막 디바이스를 뽑는 중이라 성립한다. */
	WARN_ON(group->container_users != 1);

	/* [한국어] 컨테이너 상태를 바꾸므로 쓰기 모드로 잡는다. */
	down_write(&container->group_lock);

	/* [한국어] 정해진 백엔드가 있는지 본다. */
	driver = container->iommu_driver;
	/* [한국어] SET_IOMMU 를 하지 않은 채 group 만 붙였다 뗄 수도 있으므로 NULL 검사가 필요하다. */
	if (driver)
		/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_detach_group.
		 * 그 함수가 IOMMU domain 에서 group 을 떼고, 그 domain 을 쓰는 group 이 더 없으면
		 * domain 자체를 해제하며 IOVA 창을 재계산한다. 반환값이 없어 실패할 수 없다. */
		driver->ops->detach_group(container->iommu_data,
					  group->iommu_group);

	/* [한국어] 진짜 IOMMU group 이었다면 */
	if (group->type == VFIO_IOMMU)
		/* [한국어] attach 때 얻은 DMA 소유권을 IOMMU 코어에 돌려준다. 이 뒤로 커널 드라이버가
		 * 다시 이 group 의 디바이스에 바인딩될 수 있다. */
		iommu_group_release_dma_owner(group->iommu_group);

	/* [한국어] group 의 컨테이너 연결을 끊는다. vfio_group_has_iommu 가 다시 거짓이 된다. */
	group->container = NULL;
	/* [한국어] 사용자 수를 0 으로 되돌린다. container 대입과 한 쌍이라 불변식이 유지된다. */
	group->container_users = 0;
	/* [한국어] 컨테이너의 group 목록에서 뺀다. */
	list_del(&group->container_next);

	/* Detaching the last group deprivileges a container, remove iommu */
	/* [한국어] 상류 주석대로 마지막 group 이 빠지면 컨테이너는 다시 **비특권** 상태가 되어야 한다.
	 * IOMMU 자원(domain, 핀된 페이지, IOVA 리스트)을 계속 붙들고 있을 근거가 사라지기
	 * 때문이다. 그래서 백엔드를 통째로 해제한다. */
	if (driver && list_empty(&container->group_list)) {
		/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_release 가 모든 DMA 매핑을 풀고
		 * 핀된 페이지를 놓고 domain 을 해제한 뒤 struct vfio_iommu 를 kfree 한다. */
		driver->ops->release(container->iommu_data);
		/* [한국어] vfio_ioctl_set_iommu 의 try_module_get 짝. 이제 그 백엔드 모듈을 rmmod 할 수 있다.
		 * 빠뜨리면 vfio_iommu_type1 모듈이 영원히 언로드되지 않는다. */
		module_put(driver->ops->owner);
		/* [한국어] 백엔드를 지워 컨테이너를 SET_IOMMU 이전 상태로 되돌린다. 다시 SET_IOMMU 할 수 있다. */
		container->iommu_driver = NULL;
		/* [한국어] 인스턴스 포인터도 함께 지운다. 방금 해제한 메모리를 가리키면 안 된다. */
		container->iommu_data = NULL;
	}

	/* [한국어] 쓰기 락을 푼다. 아래 put 은 락 밖에서 해야 한다 — 마지막이면 kfree 로 이어지기 때문이다. */
	up_write(&container->group_lock);

	/* [한국어] **vfio_container_attach_group 의 vfio_container_get 짝.** 이것이 마지막 참조였다면
	 * vfio_container_release 가 불려 컨테이너 메모리가 해제된다. 사용자가 컨테이너 fd 를
	 * 먼저 닫아 둔 경우가 정확히 그 상황이다.
	 * 빠뜨리면 컨테이너가 영원히 남아 새고, 두 번 부르면 use-after-free 가 된다. */
	vfio_container_put(container);
}

/* [한국어]
 * vfio_group_use_container - device fd 하나가 컨테이너 사용을 시작한다고 등록한다
 *
 * @group: 디바이스가 속한 group. 호출자가 group->group_lock 을 쥐고 있어야 하고,
 *         group->container 가 NULL 이 아님도 이미 확인했다.
 * @return: 0 성공. -EINVAL(SET_CONTAINER 는 했지만 SET_IOMMU 를 아직 안 함),
 *          -EPERM(noiommu 인데 CAP_SYS_RAWIO 없음).
 *
 * 왜 필요한가: 첫 device open 에서 두 가지를 동시에 세운다 — container_users 증가와
 * **group 파일 참조 획득**. 후자가 이 파일에서 가장 중요한 한 줄이다.
 *
 * 동작 과정:
 *  1. container->iommu_driver 가 NULL 이면 IOVA 주소공간이 아직 없다는 뜻이라 -EINVAL.
 *     상류 주석이 그 상황을 그대로 설명한다.
 *  2. noiommu 게이트의 마지막 관문 — CAP_SYS_RAWIO.
 *  3. get_file(group->opened_file) 로 group fd 의 참조를 하나 건다.
 *  4. container_users 를 +1.
 *
 * 실행 컨텍스트: 프로세스 문맥(첫 device open). group->group_lock 과 dev_set->lock 을
 * 쥔 상태. container->group_lock 은 잡지 않는다 — iommu_driver 를 읽기만 하고,
 * 그것이 바뀌려면 이 group 이 떨어져야 하는데 그럴 수 없는 상태다.
 *
 * 에러 경로: 두 실패 모두 아무것도 바꾸지 않는다.
 *
 * get_file 이 보장하는 것: device fd 가 하나라도 열려 있으면 group fd 의 참조가 0 이 되지
 * 않으므로 group.c 의 vfio_group_fops_release 가 불리지 않는다. 그 덕분에
 * vfio_group_fops_release 는 "여기 왔다면 열린 device 가 없다" 고 단정하고
 * container_users 검사 없이 detach 할 수 있다. 짝은 vfio_group_unuse_container 의 fput.
 *
 * container_users: attach 때의 1(group 자신의 몫) 위에 device fd 수만큼 쌓인다.
 * group.c 의 UNSET_CONTAINER 가 "1 이 아니면 -EBUSY" 로 거절하는 근거가 이 값이다.
 *
 * 호출 체인:
 *   vfio_df_open → vfio_df_device_first_open(vfio_main.c)
 *     → vfio_device_group_use_iommu(group.c) → [vfio_group_use_container]
 */
int vfio_group_use_container(struct vfio_group *group)
{
	/* [한국어] 호출자(group.c 의 vfio_device_group_use_iommu)가 group->group_lock 을 쥐고 왔고,
	 * container 가 NULL 이 아님도 이미 확인했다. */
	lockdep_assert_held(&group->group_lock);

	/*
	 * The container fd has been assigned with VFIO_GROUP_SET_CONTAINER but
	 * VFIO_SET_IOMMU hasn't been done yet.
	 */
	/* [한국어] SET_CONTAINER 는 했는데 SET_IOMMU 를 아직 안 한 상태다. 그런 컨테이너에는 IOVA
	 * 주소공간이 없으므로 디바이스를 열어 줄 수 없다. 상류 주석이 그대로 설명한다. */
	if (!group->container->iommu_driver)
		/* [한국어] -EINVAL 로 첫 device open 을 실패시킨다. */
		return -EINVAL;

	/* [한국어] noiommu 게이트의 마지막 관문. group fd 를 열 때와 컨테이너에 붙일 때 이미
	 * 확인했지만, device fd 를 실제로 얻는 시점에 한 번 더 본다. */
	if (group->type == VFIO_NO_IOMMU && !capable(CAP_SYS_RAWIO))
		/* [한국어] -EPERM. */
		return -EPERM;

	/* [한국어] **group 파일 참조 +1** — 이 파일에서 가장 중요한 한 줄이다.
	 * device fd 가 하나 열릴 때마다 group fd 의 참조를 하나 건다. 그래서 사용자가
	 * device fd 를 열어 둔 채 group fd 를 close 해도 group.c 의 vfio_group_fops_release 가
	 * 불리지 않는다. 그 덕분에 vfio_group_fops_release 는 "여기 왔다면 열린 device 가
	 * 없다" 고 단정하고 container_users 검사 없이 detach 할 수 있다.
	 * 짝은 vfio_group_unuse_container 의 fput 이다. */
	get_file(group->opened_file);
	/* [한국어] 사용자 수를 하나 늘린다. attach 때의 1(group 자신의 몫) 위에 device fd 수만큼 쌓인다.
	 * group.c 의 UNSET_CONTAINER 가 1 이 아니면 -EBUSY 로 거절하는 근거다. */
	group->container_users++;
	/* [한국어] 성공. 호출자가 이어서 vfio_device_container_register 를 부른다. */
	return 0;
}

/* [한국어]
 * vfio_group_unuse_container - device fd 하나가 컨테이너 사용을 끝냈다고 등록한다
 *
 * @group: 디바이스가 속한 group. 호출자가 group->group_lock 을 쥐고 있어야 한다.
 * @return: 없음. 실패할 수 없다.
 *
 * 왜 필요한가: vfio_group_use_container 의 정확한 짝이다. container_users 를 되돌리고
 * group fd 참조를 놓는다.
 *
 * 동작 과정:
 *  1. container_users 가 2 이상인지 WARN_ON 으로 확인한다. 여기 오기 전에 attach 의 1 과
 *     use 의 +1 이 모두 있어야 하므로 1 이하면 짝이 어긋난 커널 버그다.
 *  2. container_users 를 -1.
 *  3. fput(group->opened_file) 로 use 의 get_file 을 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 문맥(마지막 device close). group->group_lock 과
 * dev_set->lock 을 쥔 상태.
 *
 * 에러 경로: 없다.
 *
 * 주의: 마지막 device fd 였고 사용자가 이미 group fd 를 close 해 두었다면, 이 fput 이
 * group 파일의 최종 참조를 떨어뜨려 group.c 의 vfio_group_fops_release 로 이어질 수 있다.
 * 빠뜨리면 group fd 가 영원히 살아남아 컨테이너와 IOVA 매핑, 핀된 페이지가 모두 샌다.
 *
 * 호출 체인:
 *   vfio_df_close → vfio_df_device_last_close(vfio_main.c)
 *     → vfio_device_group_unuse_iommu(group.c) → [vfio_group_unuse_container]
 */
void vfio_group_unuse_container(struct vfio_group *group)
{
	/* [한국어] use 쪽과 같은 락을 요구한다. 획득과 해제가 같은 락 아래에서 일어나야 짝이 어긋나지 않는다. */
	lockdep_assert_held(&group->group_lock);

	/* [한국어] 여기 오기 전에 반드시 attach 의 1 과 use 의 +1 이 모두 있어야 하므로 2 이상이어야
	 * 한다. 1 이하면 use/unuse 짝이 어긋난 커널 버그다. */
	WARN_ON(group->container_users <= 1);
	/* [한국어] use 의 ++ 를 되돌린다. */
	group->container_users--;
	/* [한국어] use 의 get_file 을 되돌린다. 마지막 device fd 였다면 이 fput 이 group fd 의
	 * 최종 참조를 떨어뜨려 vfio_group_fops_release 를 부를 수도 있다.
	 * 빠뜨리면 group fd 가 영원히 살아남아 컨테이너와 IOVA 매핑, 핀된 페이지가 모두 샌다. */
	fput(group->opened_file);
}

/* [한국어]
 * vfio_device_container_pin_pages - vendor 가 게스트 IOVA 의 페이지를 핀해 달라고 요청한다
 *
 * @device: 요청한 디바이스. group->container 로 백엔드를 찾는다.
 * @iova: 핀할 게스트 IOVA 시작 주소.
 * @npage: 핀할 페이지 수(4KB 단위).
 * @prot: IOMMU_READ / IOMMU_WRITE 보호 플래그.
 * @pages: 결과 struct page 포인터 배열(호출자가 준비한다).
 * @return: 실제로 핀한 페이지 수(양수), 또는 음수 오류 —
 *          -E2BIG(상한 초과), -ENOTTY(백엔드가 지원 안 함), 그 밖에 백엔드 오류.
 *
 * 왜 필요한가: mdev 계열 vendor 드라이버는 게스트가 준 IOVA 를 자기가 직접 읽고 써야
 * 한다(디바이스가 아니라 소프트웨어가 DMA 를 중개하기 때문이다). 그러려면 그 IOVA 에
 * 대응하는 호스트 페이지가 스왑되거나 이동하지 않도록 핀해야 하고, 이 함수가 그 통로다.
 * vfio_main.c 의 vfio_pin_pages 가 container 경로와 iommufd 경로를 가른 뒤 이쪽으로 온다.
 *
 * 동작 과정: npage 상한 검사 → 백엔드와 pin_pages 슬롯 존재 확인 → ops->pin_pages 호출.
 *
 * 실행 컨텍스트: 프로세스 문맥(vendor 의 ioctl 처리 중). **락을 잡지 않는다** —
 * 호출자가 device fd 를 쥐고 있어 백엔드가 바뀔 수 없다.
 *
 * 에러 경로: 상한 초과는 -E2BIG(무한히 큰 요청으로 커널 메모리를 고갈시키는 것을 막는다),
 * 슬롯이 없으면 -ENOTTY. noiommu 백엔드가 정확히 후자다.
 *
 * 백엔드 dispatch: ops->pin_pages — type1 이면 vfio_iommu_type1_pin_pages.
 * 그 함수가 IOVA 를 사용자 가상주소로 되돌린 뒤 GUP 로 핀하고 배열을 채운다.
 *
 * 호출 체인:
 *   vendor(mdev) → vfio_pin_pages(vfio_main.c) → [vfio_device_container_pin_pages]
 *     → ops->pin_pages → vfio_iommu_type1_pin_pages
 */
int vfio_device_container_pin_pages(struct vfio_device *device,
				    dma_addr_t iova, int npage,
				    int prot, struct page **pages)
{
	/* [한국어] 이 디바이스가 속한 group 의 컨테이너. */
	struct vfio_container *container = device->group->container;
	/* [한국어] 백엔드가 요구하는 group 인자. type1 은 이것으로 어느 domain 의 IOVA 인지 판별한다. */
	struct iommu_group *iommu_group = device->group->iommu_group;
	/* [한국어] 정해진 백엔드. */
	struct vfio_iommu_driver *driver = container->iommu_driver;

	/* [한국어] 한 번에 핀할 수 있는 페이지 수를 제한한다. VFIO_PIN_PAGES_MAX_ENTRIES 는
	 * include/linux/vfio.h 에 정의돼 있으며, 사용자/vendor 가 무한히 큰 요청으로 커널
	 * 메모리를 고갈시키는 것을 막는다. */
	if (npage > VFIO_PIN_PAGES_MAX_ENTRIES)
		/* [한국어] -E2BIG. */
		return -E2BIG;

	/* [한국어] 백엔드가 없거나 그 vtable 이 pin_pages 를 제공하지 않으면 이 기능이 없다.
	 * noiommu 백엔드가 정확히 그 경우다(슬롯이 NULL). unlikely 는 정상 경로가 아님을
	 * 분기 예측에 알린다. */
	if (unlikely(!driver || !driver->ops->pin_pages))
		/* [한국어] -ENOTTY — "이 백엔드는 그 기능을 모른다". */
		return -ENOTTY;
	/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_pin_pages 로 간다.
	 * 그 함수가 사용자 IOVA 를 호스트 페이지로 바꿔 GUP 로 핀하고 struct page 배열을
	 * 채워 준다. mdev 계열 vendor 가 게스트 메모리를 직접 만질 때 쓰는 경로다. */
	return driver->ops->pin_pages(container->iommu_data, iommu_group, iova,
				      npage, prot, pages);
}

/* [한국어]
 * vfio_device_container_unpin_pages - 핀했던 페이지를 놓는다
 *
 * @device: 요청한 디바이스.
 * @iova: 놓을 게스트 IOVA 시작 주소.
 * @npage: 페이지 수.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_device_container_pin_pages 의 짝이다. 놓지 않으면 호스트 페이지가
 * 영원히 핀된 채로 남아 메모리가 샌다.
 *
 * 동작 과정: npage 가 유효 범위인지 WARN_ON 으로 확인하고, 백엔드의 unpin_pages 를 부른다.
 * 반환형이 void 라 오류를 알릴 수 없어 상한 위반은 WARN 후 그냥 돌아간다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다.
 *
 * 에러 경로: 인자 검증 실패면 아무것도 하지 않는다.
 *
 * [상류 코드 관찰] 짝인 vfio_device_container_pin_pages 는
 * `!driver || !driver->ops->pin_pages` 를 확인하고 -ENOTTY 를 돌려주는데, 이 함수는
 * container->iommu_driver 와 그 ops->unpin_pages 슬롯을 **NULL 검사 없이** 역참조한다.
 * unpin 은 pin 이 성공한 뒤에만 불릴 수 있고 그 사이 백엔드가 바뀔 수 없다는 전제 위에
 * 서 있는 것으로 보인다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 백엔드 dispatch: ops->unpin_pages — type1 이면 vfio_iommu_type1_unpin_pages.
 *
 * 호출 체인:
 *   vendor(mdev) → vfio_unpin_pages(vfio_main.c) → [vfio_device_container_unpin_pages]
 *     → ops->unpin_pages → vfio_iommu_type1_unpin_pages
 */
void vfio_device_container_unpin_pages(struct vfio_device *device,
				       dma_addr_t iova, int npage)
{
	/* [한국어] 이 디바이스가 속한 group 의 컨테이너. */
	struct vfio_container *container = device->group->container;

	/* [한국어] pin 때와 같은 상한을 확인하고, 0 이하도 거른다. 여기서는 -E2BIG 을 돌려줄 수 없어
	 * (반환형이 void) WARN_ON 으로 알리고 그냥 돌아간다. */
	if (WARN_ON(npage <= 0 || npage > VFIO_PIN_PAGES_MAX_ENTRIES))
		/* [한국어] 잘못된 인자면 아무것도 하지 않는다. */
		return;

	/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_unpin_pages.
	 * [상류 코드 관찰] 짝인 vfio_device_container_pin_pages 는 `!driver ||
	 * !driver->ops->pin_pages` 를 확인하고 -ENOTTY 를 돌려주는데, 이 함수는
	 * iommu_driver 와 unpin_pages 슬롯을 **NULL 검사 없이** 역참조한다.
	 * unpin 은 pin 이 성공한 뒤에만 불릴 수 있고 그 사이에 백엔드가 바뀔 수 없다는 전제
	 * 위에 서 있는 것으로 보인다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	container->iommu_driver->ops->unpin_pages(container->iommu_data, iova,
						  npage);
}

/* [한국어]
 * vfio_device_container_dma_rw - 커널이 게스트 IOVA 를 직접 읽거나 쓴다
 *
 * @device: 요청한 디바이스.
 * @iova: 접근할 게스트 IOVA.
 * @data: 커널 쪽 버퍼.
 * @len: 바이트 수.
 * @write: 참이면 게스트로 쓰기, 거짓이면 게스트에서 읽기.
 * @return: 0 성공, -ENOTTY(백엔드 미지원), 그 밖에 백엔드 오류.
 *
 * 왜 필요한가: 마이그레이션에서 디바이스 상태를 게스트 메모리로 옮기거나, mdev vendor 가
 * 게스트가 준 디스크립터를 읽어야 할 때 쓴다. pin_pages 와 달리 페이지를 붙들지 않고
 * 그 자리에서 복사만 한다.
 *
 * 동작 과정: 백엔드와 dma_rw 슬롯 존재를 확인하고 ops->dma_rw 를 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다 — 호출자가 device fd 를 쥐고 있다.
 *
 * 에러 경로: 슬롯이 없으면 -ENOTTY. noiommu 백엔드가 그 경우다.
 *
 * 백엔드 dispatch: ops->dma_rw — type1 이면 vfio_iommu_type1_dma_rw.
 *
 * 호출 체인:
 *   vendor → vfio_dma_rw(vfio_main.c) → [vfio_device_container_dma_rw]
 *     → ops->dma_rw → vfio_iommu_type1_dma_rw
 */
int vfio_device_container_dma_rw(struct vfio_device *device,
				 dma_addr_t iova, void *data,
				 size_t len, bool write)
{
	/* [한국어] 이 디바이스가 속한 group 의 컨테이너. */
	struct vfio_container *container = device->group->container;
	/* [한국어] 정해진 백엔드. */
	struct vfio_iommu_driver *driver = container->iommu_driver;

	/* [한국어] 백엔드가 없거나 dma_rw 슬롯이 없으면 지원하지 않는다. noiommu 가 그 경우다.
	 * 이쪽은 pin_pages 와 같은 방식으로 방어한다. */
	if (unlikely(!driver || !driver->ops->dma_rw))
		/* [한국어] -ENOTTY. */
		return -ENOTTY;
	/* [한국어] **백엔드 dispatch**: type1 이면 vfio_iommu_type1_dma_rw 로 간다.
	 * 게스트 IOVA 를 커널이 직접 읽고 쓰는 경로이며, 마이그레이션에서 디바이스 상태를
	 * 게스트 메모리로 옮길 때 vendor 가 쓴다. write 가 참이면 게스트로 쓰기, 거짓이면 읽기다. */
	return driver->ops->dma_rw(container->iommu_data, iova, data, len,
				   write);
}

/* [한국어]
 * vfio_container_init - 모듈 적재 시 컨테이너 노드와 noiommu 백엔드를 등록한다
 *
 * @return: 0 성공. misc_register 나 백엔드 등록이 낸 오류.
 *
 * 왜 필요한가: /dev/vfio/vfio 가 존재해야 사용자가 컨테이너를 만들 수 있다. 이 함수가
 * 백엔드 목록을 초기화하고 그 노드를 띄운다. group.c 의 vfio_group_init 이 자기 일보다
 * **먼저** 이것을 부르는 이유는 group 이 SET_CONTAINER 로 컨테이너를 참조하기 때문이다.
 *
 * 동작 과정:
 *  1. iommu_drivers_lock 과 iommu_drivers_list 초기화.
 *  2. misc_register(&vfio_dev) — 고정 minor 를 가진 /dev/vfio/vfio 노드.
 *     group 노드처럼 동적 major 를 쓰지 않는 이유는 컨테이너 노드가 시스템에 단 하나이기
 *     때문이다.
 *  3. CONFIG_VFIO_NOIOMMU 가 켜져 있으면 vfio_noiommu_ops 를 후보 목록에 등록한다.
 *     **등록돼야 사용자가 SET_IOMMU 에 VFIO_NOIOMMU_IOMMU 를 넘길 수 있다.**
 *     등록 자체는 위험하지 않다 — 실제 사용은 모듈 파라미터와 CAP_SYS_RAWIO 가 함께 막는다.
 *
 * 실행 컨텍스트: 모듈 init(프로세스 문맥). 잠들 수 있다.
 *
 * 에러 경로: 2단계 실패는 로그를 남기고 그대로 반환. 3단계 실패는 err_misc 로 가서
 * misc_deregister 로 2단계를 되돌린다.
 *
 * 호출 체인:
 *   module_init → vfio_init(vfio_main.c) → vfio_group_init(group.c)
 *     → [vfio_container_init] → misc_register / vfio_register_iommu_driver
 */
int __init vfio_container_init(void)
{
	/* [한국어] 각 단계의 결과 코드. */
	int ret;

	/* [한국어] 백엔드 목록을 지킬 뮤텍스를 초기화한다. */
	mutex_init(&vfio.iommu_drivers_lock);
	/* [한국어] 백엔드 목록을 빈 상태로 만든다. 이 두 줄이 끝나야 아래 등록이 안전하다. */
	INIT_LIST_HEAD(&vfio.iommu_drivers_list);

	/* [한국어] /dev/vfio/vfio 노드를 만든다. 고정 minor 를 쓰므로 다른 드라이버가 그 번호를
	 * 먼저 잡고 있으면 실패한다. */
	ret = misc_register(&vfio_dev);
	/* [한국어] 등록 실패. */
	if (ret) {
		/* [한국어] 무엇이 실패했는지 커널 로그에 남긴다. */
		pr_err("vfio: misc device register failed\n");
		/* [한국어] 아직 아무것도 등록하지 않았으므로 그냥 반환한다. */
		return ret;
	}

	/* [한국어] Kconfig 로 noiommu 를 뺐다면 이 백엔드를 등록하지 않는다. IS_ENABLED 는 컴파일 시
	 * 상수라 그 구성에서는 아래 두 줄이 통째로 사라지고, vfio_noiommu_ops 도
	 * 쓰이지 않는 정적 객체가 된다. */
	if (IS_ENABLED(CONFIG_VFIO_NOIOMMU)) {
		/* [한국어] noiommu 백엔드를 등록한다. **이것이 등록돼야 사용자가 VFIO_SET_IOMMU 에
		 * VFIO_NOIOMMU_IOMMU 를 넘길 수 있다.** 등록만으로는 아무 위험이 없다 —
		 * 실제 사용은 모듈 파라미터와 CAP_SYS_RAWIO 가 함께 막는다. */
		ret = vfio_register_iommu_driver(&vfio_noiommu_ops);
		/* [한국어] 등록 실패. */
		if (ret)
			/* [한국어] misc 디바이스를 되돌리는 라벨로 간다. */
			goto err_misc;
	}
	/* [한국어] 성공. 이 시점부터 사용자가 /dev/vfio/vfio 를 열 수 있다. */
	return 0;

/* [한국어] noiommu 등록 실패만 여기로 온다. */
err_misc:
	/* [한국어] misc_register 를 되돌린다. */
	misc_deregister(&vfio_dev);
	/* [한국어] 오류를 group.c 의 vfio_group_init 에 올린다. 그쪽이 모듈 적재를 실패시킨다. */
	return ret;
}

/* [한국어]
 * vfio_container_cleanup - vfio_container_init 을 역순으로 되감는다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 모듈 언로드와 vfio_group_init 의 실패 되돌림 두 경우에 불린다.
 * init 이 잡은 것(noiommu 백엔드 등록, misc 디바이스, 뮤텍스)을 역순으로 반납한다.
 *
 * 동작 과정:
 *  1. CONFIG_VFIO_NOIOMMU 가 켜져 있었으면 vfio_unregister_iommu_driver 로 noiommu 를
 *     목록에서 뺀다. init 과 같은 조건이라 짝이 어긋나지 않는다.
 *  2. misc_deregister 로 /dev/vfio/vfio 노드를 없앤다.
 *  3. mutex_destroy 로 백엔드 목록 뮤텍스를 파괴한다. 이 시점에 목록은 비어 있어야
 *     하는데, type1 같은 별도 모듈은 vfio 코어보다 먼저 언로드되며 그때
 *     vfio_unregister_iommu_driver 로 스스로 빠지기 때문이다.
 *
 * 실행 컨텍스트: 모듈 exit 또는 init 실패 경로(프로세스 문맥). 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_group_cleanup(group.c) 또는 vfio_group_init 의 실패 라벨
 *     → [vfio_container_cleanup]
 *     → vfio_unregister_iommu_driver / misc_deregister / mutex_destroy
 */
void vfio_container_cleanup(void)
{
	/* [한국어] 등록했던 경우에만 되돌린다. init 과 같은 조건이라 짝이 어긋나지 않는다. */
	if (IS_ENABLED(CONFIG_VFIO_NOIOMMU))
		/* [한국어] noiommu 백엔드를 목록에서 빼고 그 원소를 해제한다. */
		vfio_unregister_iommu_driver(&vfio_noiommu_ops);
	/* [한국어] /dev/vfio/vfio 노드를 없앤다. init 의 역순이다. */
	misc_deregister(&vfio_dev);
	/* [한국어] 백엔드 목록 뮤텍스를 파괴한다. 이 시점에는 목록이 비어 있어야 한다 —
	 * type1 같은 별도 모듈은 vfio 코어보다 먼저 언로드되며, 그때
	 * vfio_unregister_iommu_driver 로 스스로 빠진다. */
	mutex_destroy(&vfio.iommu_drivers_lock);
}

/* [한국어] 이 모듈이 VFIO_MINOR 번호의 misc 디바이스를 제공한다고 선언한다.
 * modprobe 가 /dev/vfio/vfio 접근 시 이 모듈을 자동 적재할 수 있게 하는 별칭이다. */
MODULE_ALIAS_MISCDEV(VFIO_MINOR);
/* [한국어] devname 별칭. udev/systemd 가 노드 이름 "vfio/vfio" 만 보고 이 모듈을 미리
 * 적재하도록 한다. 위 별칭이 minor 번호 기준이라면 이쪽은 이름 기준이다. */
MODULE_ALIAS("devname:vfio/vfio");
