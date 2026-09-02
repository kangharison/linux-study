// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO: IOMMU DMA mapping support for Type1 IOMMU
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 *
 * We arbitrarily define a Type1 IOMMU as one matching the below code.
 * It could be called the x86 IOMMU as it's designed for AMD-Vi & Intel
 * VT-d, but that makes it harder to re-use as theoretically anyone
 * implementing a similar IOMMU could make use of this.  We expect the
 * IOMMU to support the IOMMU API and have few to no restrictions around
 * the IOVA range that can be mapped.  The Type1 IOMMU is currently
 * optimized for relatively static mappings of a userspace process with
 * userspace pages pinned into memory.  We also assume devices and IOMMU
 * domains are PCI based as the IOMMU API is still centered around a
 * device/bus interface rather than a group interface.
 */

/* [한국어] Type1 IOMMU DMA 매핑 백엔드 (drivers/vfio/vfio_iommu_type1.c)
 *
 * === 파일의 역할 ===
 * 사용자 공간이 자기 주소 공간의 버퍼를 장치가 직접 DMA 할 수 있는 대상으로
 * 바꾸는 일을 전담한다. 구체적으로는 ioctl(VFIO_IOMMU_MAP_DMA) 한 번에 대해
 * (1) 요청된 가상 주소 구간의 페이지를 GUP(get_user_pages) 계열 API 로 핀해
 * 물리 페이지가 스왑/이동/해제되지 않도록 고정하고, (2) 그 페이지들을
 * 사용자가 지정한 IOVA(I/O Virtual Address) 에 오도록 IOMMU 페이지 테이블에
 * 매핑을 설치하며, (3) 핀한 페이지 수를 그 프로세스의 RLIMIT_MEMLOCK 한도에
 * 과금한다. 반대 방향인 ioctl(VFIO_IOMMU_UNMAP_DMA) 는 IOMMU 매핑을 걷어내고
 * 핀을 풀고 과금을 되돌린다. 이 세 가지(핀 / IOMMU 매핑 / 과금)가 언제나
 * 같은 수로 짝을 이루도록 유지하는 것이 이 파일 전체를 관통하는 불변식이다.
 * 그 밖에 라이브 마이그레이션용 dirty page 비트맵, mediated device 를 위한
 * 페이지 단위 pin/unpin 외부 인터페이스, 게스트 IOVA 를 커널이 직접 읽고
 * 쓰는 dma_rw 도 같은 자료구조 위에서 제공한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 저장소는 NVMe I/O 경로를 따라가며 읽도록 만들어져 있고, block/ 의
 * blk-mq 코어와 drivers/nvme, drivers/pci 는 이미 주석이 완료되어 있다.
 * 그 경로는 "커널 안에서" 도는 경로다 — 파일 시스템이 bio 를 만들고,
 * blk-mq 가 request 로 바꿔 nvme_queue_rq() 에 넘기고, drivers/nvme/host/pci.c
 * 의 nvme_map_data()(:1631) 와 nvme_pci_setup_data_prp()(:1375) 가 커널 DMA
 * API 가 돌려준 dma_addr_t 를 NVMe 명령의 PRP(Physical Region Page) 항목에
 * 채워 넣는다. 이 파일은 바로 그 "주소를 얻는 단계" 를 커널 밖으로 꺼낸
 * 것이다. 사용자 공간 NVMe 드라이버(SPDK 등, 이 트리에는 소스가 없다) 는
 * 자기 힙 버퍼를 VFIO_IOMMU_MAP_DMA 로 등록해 IOVA 를 얻고, 그 IOVA 를 직접
 * PRP/SGL 항목에 써 넣은 뒤 도어벨을 울린다. 즉 커널 경로에서 DMA API 가
 * 하던 일(페이지 고정 + IOMMU 매핑 + 주소 반환)을 여기서는 이 파일이
 * ioctl 한 번으로 대신해 주고, 그 뒤의 명령 조립과 제출은 전부 사용자
 * 공간이 한다. GPU 가 스토리지 버퍼로 직접 DMA 하는 경로도 형태가 같아서,
 * GPU 쪽 BAR 를 P2P 대상으로 등록하는 drivers/pci/p2pdma.c 와 이 파일의
 * IOVA 등록이 짝을 이룬다.
 * VFIO 내부에서 보면 이 파일은 legacy container 모델의 "IOMMU backend" 다.
 * /dev/vfio/vfio 를 열고 ioctl(VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU) 를 하면
 * drivers/vfio/container.c:302 이 이 파일의 open 콜백을 부르고, 이후 모든
 * IOMMU 계열 ioctl 이 container.c:353 을 거쳐 vfio_iommu_type1_ioctl() 로
 * 들어온다. 현대 인터페이스인 iommufd 를 쓰면 이 파일은 아예 쓰이지 않고
 * drivers/vfio/iommufd.c 가 대신한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽(호출하는 쪽): drivers/vfio/container.c 가 유일한 진입 경로다.
 * vfio_iommu_driver_ops 라는 vtable(정의는 drivers/vfio/vfio.h:400~449) 을
 * 통해 open(:302) / release(:311, :492) / ioctl(:216, :225, :353) /
 * attach_group(:243, :444) / detach_group(:254, :480) / pin_pages(:544) /
 * unpin_pages(:556) / register_device(:170) / unregister_device(:180) /
 * dma_rw(:569) 가 이 파일의 함수로 디스패치된다. 등록은 모듈 init 에서
 * container.c:91 의 vfio_register_iommu_driver() 를 불러 이루어진다.
 * vendor 드라이버(mdev 류)가 부르는 vfio_pin_pages()/vfio_unpin_pages()/
 * vfio_dma_rw()(drivers/vfio/vfio_main.c) 도 container 를 거쳐 이 파일의
 * pin_pages/unpin_pages/dma_rw 콜백으로 내려온다.
 * 아래쪽(호출당하는 쪽): IOMMU 코어 API(iommu_map / iommu_unmap /
 * iommu_attach_group / iommu_domain_free / iommu_iova_to_phys 등) 와 mm 의
 * 페이지 핀 API(pin_user_pages_remote / unpin_user_pages_dirty_lock /
 * __account_locked_vm). 이 트리는 sparse checkout 이라 drivers/iommu/ 와
 * mm/ 이 없으므로, 이 함수들의 내부 동작은 이 트리에서 확인 못 함 — 아래
 * 주석들은 "무엇을 요구하는가" 와 "반환값을 어떻게 해석하는가" 만 적는다.
 * 옆쪽: 하드웨어 격리의 전제인 IOMMU group 은 PCIe ACS(Access Control
 * Services) 로 결정되는데, ACS 를 켜는 코드는 drivers/pci/pci-driver.c:3618
 * 의 pci_enable_acs() 다. ATS/PASID 같은 장치 측 주소 변환 기능은
 * drivers/pci/ats.c 에 정리되어 있다.
 * 데이터 흐름: 사용자 vaddr -> (핀) struct page -> pfn -> phys ->
 * iommu_map() 으로 IOVA 에 설치 -> 장치가 그 IOVA 로 DMA -> IOMMU 가 phys 로
 * 변환 -> 사용자 버퍼에 데이터 도착.
 *
 * === 주요 함수/구조체 요약 ===
 *  - vfio_dma_do_map() : VFIO_IOMMU_MAP_DMA 의 본체. 정합성 검사 ->
 *    struct vfio_dma 할당 -> rb-tree 등록 -> vfio_pin_map_dma() 로 핀+매핑.
 *  - vfio_dma_do_unmap() : VFIO_IOMMU_UNMAP_DMA 의 본체. 범위가 기존 매핑을
 *    반으로 자르지 않는지 확인한 뒤 vfio_remove_dma() 를 반복 호출.
 *  - vfio_pin_pages_remote() : 이 파일의 심장. 물리적으로 연속인 최대 구간을
 *    한 번에 핀하고, 그만큼만 RLIMIT_MEMLOCK 에 과금한다.
 *  - vfio_unpin_pages_remote() : 위의 짝. 핀을 풀고 과금을 되돌린다.
 *  - vfio_iommu_replay() : 컨테이너에 새 IOMMU domain 이 추가될 때 기존
 *    매핑 전체를 새 domain 에 다시 설치한다.
 *  - vfio_iommu_type1_attach_group() / _detach_group() : IOMMU group 을
 *    컨테이너에 붙이고 떼며, 유효 IOVA 창(aperture)과 예약 구간을 갱신한다.
 *  - struct vfio_iommu : 컨테이너 하나에 대응하는 최상위 객체. domain 목록,
 *    DMA rb-tree, 유효 IOVA 목록, 락 두 개를 가진다.
 *  - struct vfio_dma : MAP_DMA 한 번에 대응하는 매핑 하나. rb-tree 노드,
 *    iova/vaddr/size, 소유 task 와 mm, 과금량(locked_vm) 을 가진다.
 *  - struct vfio_pfn : mdev 가 별도로 핀해 둔 페이지 하나. vfio_dma 안의
 *    두 번째 rb-tree 에 매달린다.
 *
 * === 페이지 핀과 잠금 메모리 과금 모델 ===
 * 핀(pin)과 과금(accounting)은 서로 다른 두 개의 회계 장부이고, 이 파일의
 * 버그는 거의 전부 이 둘이 어긋나는 형태로 나타난다.
 * (1) 핀 장부. 페이지 하나를 핀하면 그 struct page 의 pin 카운트가 1 오른다.
 *     올리는 곳은 vaddr_get_pfns() 안의 pin_user_pages_remote() 뿐이고,
 *     내리는 곳은 put_pfn() 과 put_valid_unreserved_pfns() 뿐이다. 핀을 놓치면
 *     그 물리 페이지는 프로세스가 죽어도 영원히 회수되지 않는다.
 * (2) 과금 장부. "이 프로세스가 잠근 페이지 수" 를 mm 에 더하고 빼며,
 *     RLIMIT_MEMLOCK 한도와 비교한다. 갱신하는 곳은 vfio_lock_acct() 하나뿐이고
 *     그것이 mm_lock_acct() -> __account_locked_vm() 으로 내려간다. 과금을
 *     빼먹으면(누락) 프로세스가 종료된 뒤에도 한도가 소모된 채로 남는다.
 * (3) 두 장부가 어긋나는 지점이 세 군데 있다.
 *     - 예약 pfn(is_invalid_reserved_pfn() 이 참): MMIO 를 mmap 한 구간처럼
 *       struct page 가 없는 메모리다. 핀도 하지 않고 과금도 하지 않는다.
 *       vfio_dma.has_rsvd 가 "이 매핑에 그런 페이지가 섞여 있다" 를 기억한다.
 *     - 이미 mdev 가 외부에서 핀해 둔 페이지: vfio_pfn rb-tree 에 있으면
 *       이미 과금된 것이므로 vpfn_pages() 로 그 수를 세어 과금에서 뺀다.
 *     - IOMMU domain 이 하나도 없는 컨테이너(mdev 전용): 페이지를 미리 핀하지
 *       않으므로 pin_pages 콜백 쪽에서만 과금한다(do_accounting 플래그).
 * (4) 누구의 한도인가. vfio_dma.task 는 MAP_DMA 를 부른 스레드의 group_leader
 *     이고 vfio_dma.mm 은 그 프로세스의 mm 이다. 둘 다 참조를 쥐고 있어서
 *     프로세스가 먼저 죽어도 언핀 시점에 올바른 대상에게 반환할 수 있다.
 *     vfio_dma.lock_cap 은 MAP 시점의 CAP_IPC_LOCK 보유 여부를 굳혀 둔 값으로,
 *     권한이 있으면 한도 검사를 통과시킨다. VFIO_UPDATE_VADDR 로 소유자가 바뀌면
 *     vfio_change_dma_owner() 가 옛 mm 에서 빼고 새 mm 에 더해 장부를 옮긴다.
 *
 * === DMA rb-tree 두 그루 ===
 * 이 파일에는 red-black tree 가 두 그루 있고 역할이 다르다.
 * (1) vfio_iommu.dma_list : 키는 vfio_dma.iova(구간의 시작 주소) 다. 노드가
 *     표현하는 것은 점이 아니라 [iova, iova+size-1] 구간이고, 구간끼리는
 *     절대 겹치지 않는다는 것이 이 트리의 불변식이다. 그래서 검색
 *     vfio_find_dma() 는 "겹치는 노드" 를 찾고, 삽입 vfio_link_dma() 는
 *     반드시 size==0 인 빈 노드만 받는다(WARN_ON(new->size != 0)) — 매핑을
 *     조금씩 키워 나가는 동안 다른 노드와 겹칠 일이 없게 하려는 것이다.
 *     보호하는 락은 vfio_iommu.lock 하나이며, 트리를 읽는 모든 함수가 그
 *     락을 잡은 채로 호출된다. 구간을 반으로 쪼개는 연산은 v2 인터페이스에서
 *     금지되어 있어(언맵 범위가 기존 매핑을 이등분하면 -EINVAL) 노드 분할
 *     코드가 아예 존재하지 않는다.
 * (2) vfio_dma.pfn_list : 키는 vfio_pfn.iova 이고 노드 하나가 정확히 한
 *     페이지를 뜻한다. mdev 가 vfio_pin_pages() 로 따로 핀한 페이지만
 *     들어가며, ref_count 로 중복 핀을 센다. 같은 vfio_iommu.lock 이 보호한다.
 *     이 트리가 비어 있지 않은 vfio_dma 는 곧바로 지울 수 없어서,
 *     vfio_dma_do_unmap() 이 vfio_notify_dma_unmap() 으로 장치에 언핀을
 *     요청하고 again 라벨로 되돌아가 다시 시도한다.
 *
 * === IOVA 와 페이지 크기 산술 ===
 * 이 파일에서 등장하는 정렬/마스크 연산은 세 가지 서로 다른 페이지 크기를
 * 구분하지 못하면 읽을 수 없다.
 *  - PAGE_SIZE : 호스트 CPU 의 페이지 크기. 핀과 struct page 의 단위다.
 *  - iommu->pgsize_bitmap : 컨테이너에 붙은 모든 IOMMU domain 이 공통으로
 *    지원하는 페이지 크기들의 비트맵. 각 비트 위치가 그 크기를 뜻한다.
 *    __ffs() 로 가장 작은 지원 크기를 뽑아 pgsize/pgshift 로 쓴다.
 *    vfio_update_pgsize_bitmap() 이 이 값을 PAGE_SIZE 이상으로 올림한다.
 *  - bitmap 의 페이지 크기 : dirty page 비트맵에서 비트 하나가 뜻하는 크기.
 *    항상 위의 최소 지원 크기와 같아야 한다.
 * 정렬 검사는 거의 항상 (x & (pgsize - 1)) 꼴이다. pgsize 가 2 의 거듭제곱
 * 이므로 pgsize-1 은 하위 비트 마스크가 되고, 그 결과가 0 이 아니면 정렬
 * 위반이다. 구간의 끝은 언제나 "start + size - 1"(닫힌 구간의 마지막 바이트)
 * 로 표현하며, size 를 그대로 더해 얻은 배타적 끝을 쓰지 않는다 — 주소
 * 공간의 최상단을 매핑했을 때 오버플로로 0 이 되는 것을 피하기 위해서다.
 * 그래서 오버플로 검사는 check_add_overflow(iova, size - 1, &iova_end) 형태로
 * 나타난다. */
/* [한국어] compat_ptr 등 32비트 사용자 공간 호환 정의. ioctl 인자를 사용자 공간에서 받는 백엔드라 포함한다. */
#include <linux/compat.h>
/* [한국어] struct device 와 dev_ 계열 로그. iommu_group_for_each_dev() 콜백이 struct device 를 받는다. */
#include <linux/device.h>
/* [한국어] 파일 계층 기본 정의. 이 백엔드는 /dev/vfio/vfio 파일의 ioctl 뒤편에서 동작한다. */
#include <linux/fs.h>
/* [한국어] highmem 접근 헬퍼. pfn_to_page() 계열이 여기 및 mm.h 를 통해 들어온다. */
#include <linux/highmem.h>
/* [한국어] IOMMU 코어 API 의 본체 — iommu_map/iommu_unmap/iommu_attach_group/iommu_domain 과 IOMMU_READ 계열 prot 플래그. 이 파일의 존재 이유다. */
#include <linux/iommu.h>
/* [한국어] module_param_named/MODULE_LICENSE 등 모듈 등록. 이 백엔드는 별도 모듈로 빌드될 수 있다. */
#include <linux/module.h>
/* [한국어] 핵심 메모리 관리 헤더 — pin_user_pages_remote(), unpin_user_pages_dirty_lock(), struct vm_area_struct, PAGE_SIZE/PAGE_SHIFT/PAGE_MASK. */
#include <linux/mm.h>
/* [한국어] kthread_use_mm()/kthread_unuse_mm(). dma_rw 가 커널 스레드 문맥에서 불릴 수 있어 사용자 mm 을 잠시 빌려 쓴다. */
#include <linux/kthread.h>
/* [한국어] red-black tree 구현. 이 파일의 두 그루 트리(dma_list, pfn_list)가 전부 여기에 의존한다. */
#include <linux/rbtree.h>
/* [한국어] 시그널 관련 정의. mmap_write_lock_killable() 이 치명 시그널로 깨어날 수 있는 대기를 쓰기 때문이다. */
#include <linux/sched/signal.h>
/* [한국어] mmget_not_zero()/mmput()/mmgrab()/mmdrop() 등 mm_struct 참조 관리. 매핑 소유 프로세스가 먼저 죽는 경우를 다루려면 필수다. */
#include <linux/sched/mm.h>
/* [한국어] kzalloc/kfree/kvzalloc/kvfree 와 kzalloc_obj 계열 매크로. vfio_dma/vfio_pfn/비트맵 할당에 쓴다. */
#include <linux/slab.h>
/* [한국어] copy_from_user/copy_to_user/access_ok. ioctl 인자와 dirty 비트맵을 사용자 공간과 주고받는다. */
#include <linux/uaccess.h>
/* [한국어] VFIO 외부 ABI — struct vfio_device, vfio_device_ops, vfio_info_cap 헬퍼. 여기서 uapi 의 VFIO_IOMMU_MAP_DMA 계열 정의도 딸려 온다. */
#include <linux/vfio.h>
/* [한국어] 워크큐 정의. 이 파일이 직접 워크를 큐잉하지는 않지만 상류가 포함해 둔 헤더다. */
#include <linux/workqueue.h>
/* [한국어] mm 인라인 헬퍼 모음. 페이지 상태 조회 인라인들이 여기에 있다. */
#include <linux/mm_inline.h>
/* [한국어] check_add_overflow()/check_mul_overflow(). 사용자에게서 받은 iova+size 가 주소 공간을 넘는지 검사하는 데 반드시 필요하다. */
#include <linux/overflow.h>
/* [한국어] VFIO 코어 내부 헤더 — struct vfio_iommu_driver_ops vtable 과 vfio_register_iommu_driver() 선언. 이 파일이 그 vtable 을 채워 등록한다. */
#include "vfio.h"

/* [한국어] 모듈 버전 문자열. 아래 MODULE_VERSION() 에 그대로 실린다. */
#define DRIVER_VERSION  "0.2"
/* [한국어] 모듈 작성자 문자열. MODULE_AUTHOR() 용. */
#define DRIVER_AUTHOR   "Alex Williamson <alex.williamson@redhat.com>"
/* [한국어] 모듈 설명 문자열. MODULE_DESCRIPTION() 용. */
#define DRIVER_DESC     "Type1 IOMMU driver for VFIO"

/* [한국어] 인터럽트 리매핑이 없는 플랫폼에서도 VFIO 사용을 허용할지. 기본 false.
 * 인터럽트 리매핑이 없으면 장치가 임의의 MSI 주소로 써서 호스트 인터럽트를
 * 위조할 수 있으므로 격리가 깨진다. vfio_iommu_type1_attach_group() 이
 * 이 값을 보고 격리되지 않은 MSI 를 가진 group 의 attach 를 거부한다. */
static bool allow_unsafe_interrupts;
/* [한국어] sysfs 로도 노출되는 bool 모듈 파라미터로 등록. S_IRUGO 는 모두 읽기, S_IWUSR 는 소유자 쓰기 권한이다. */
module_param_named(allow_unsafe_interrupts,
		   allow_unsafe_interrupts, bool, S_IRUGO | S_IWUSR);
/* [한국어] modinfo 에 표시될 설명문. */
MODULE_PARM_DESC(allow_unsafe_interrupts,
		 "Enable VFIO IOMMU support for on platforms without interrupt remapping support.");

/* [한국어] IOMMU 대형 페이지 매핑을 끌지. 기본 false.
 * true 면 vfio_pin_pages_remote() 가 한 번에 한 페이지만 핀하고
 * __vfio_batch_init() 이 배치 크기를 1 로 줄인다. 디버깅/재현 용도다. */
static bool disable_hugepages;
/* [한국어] 위 변수를 읽기·쓰기 가능한 bool 모듈 파라미터로 등록. */
module_param_named(disable_hugepages,
		   disable_hugepages, bool, S_IRUGO | S_IWUSR);
/* [한국어] modinfo 설명문. */
MODULE_PARM_DESC(disable_hugepages,
		 "Disable VFIO IOMMU support for IOMMU hugepages.");

/* [한국어] 컨테이너 하나가 만들 수 있는 vfio_dma 개수 상한. 기본 U16_MAX(65535).
 * __read_mostly 는 거의 바뀌지 않는 변수를 한데 모아 캐시 라인 오염을 줄이라는 힌트다.
 * vfio_iommu_type1_open() 이 이 값을 iommu->dma_avail 초기값으로 복사한다.
 * 사용자가 작은 매핑을 무한히 만들어 커널 메모리를 고갈시키는 것을 막는 방어선이다. */
static unsigned int dma_entry_limit __read_mostly = U16_MAX;
/* [한국어] 0644 = 소유자 읽기/쓰기, 나머지 읽기. 부팅 후에도 sysfs 로 조절 가능하다. */
module_param_named(dma_entry_limit, dma_entry_limit, uint, 0644);
/* [한국어] modinfo 설명문. */
MODULE_PARM_DESC(dma_entry_limit,
		 "Maximum number of user DMA mappings per container (65535).");

/* [한국어] 컨테이너(/dev/vfio/vfio 파일 하나)에 대응하는 이 백엔드의 최상위 객체.
 * vfio_iommu_type1_open() 이 만들고 vfio_iommu_type1_release() 가 없앤다.
 * 모든 콜백에 void  iommu_data 로 전달되며 맨 앞에서 이 타입으로 캐스팅된다. */
struct vfio_iommu {
	/* [한국어] 이 컨테이너에 붙어 있는 struct vfio_domain 들의 목록(하드웨어 IOMMU domain).
	 * 설정자: vfio_iommu_type1_attach_group() 이 새 domain 을 list_add 하고,
	 *   _detach_group()/_release() 가 마지막 group 이 빠질 때 list_del 한다.
	 * 읽는 자: vfio_iommu_map()/vfio_unmap_unpin()/vfio_update_pgsize_bitmap() 등
	 *   매핑을 모든 domain 에 반영해야 하는 모든 함수.
	 * 값 범위: 비어 있을 수 있다 — mdev 만 있는 컨테이너는 domain 이 0 개이고,
	 *   그때는 페이지를 미리 핀하지 않는다(vfio_dma_do_map() 의 분기).
	 * 동기화: lock 필드가 보호한다. */
	struct list_head	domain_list;
	/* [한국어] 사용자가 매핑해도 되는 IOVA 구간들의 목록(struct vfio_iova).
	 * 설정자: attach 시 domain geometry(aperture)와 예약 구간을 반영해
	 *   vfio_iommu_aper_resize()/vfio_iommu_resv_exclude() 가 만든 사본을
	 *   vfio_iommu_iova_insert_copy() 가 통째로 갈아 끼운다.
	 * 읽는 자: vfio_iommu_iova_dma_valid() 가 MAP 요청 구간의 유효성 검사에,
	 *   vfio_iommu_iova_build_caps() 가 GET_INFO 응답 작성에 쓴다.
	 * 값 범위: 빈 목록은 '제약 없음'(mdev 전용 컨테이너)을 뜻한다.
	 * 동기화: lock 필드가 보호한다. */
	struct list_head	iova_list;
	/* [한국어] 이 컨테이너의 주 뮤텍스. 아래 dma_list, iova_list, domain_list,
	 * dma_avail, pgsize_bitmap, dirty_page_tracking 등 거의 모든 상태를 보호한다.
	 * 설정자/읽는 자: 모든 ioctl 진입점과 pin/unpin/dma_rw 콜백이 잡는다.
	 * 값 범위: 프로세스 문맥에서만 잡는다 — 안에서 GUP 와 iommu_map 을 부르므로
	 *   잠들 수 있고, 따라서 인터럽트 문맥에서는 절대 진입하지 않는다.
	 * 동기화 주의: vfio_notify_dma_unmap() 은 장치 콜백이 다시 이 락을 잡으러
	 *   들어오기 때문에 일부러 락을 놓았다가 다시 잡는다. */
	struct mutex		lock;
	/* [한국어] struct vfio_dma 들의 red-black tree 루트. 키는 vfio_dma.iova 다.
	 * 설정자: vfio_link_dma() 가 삽입, vfio_unlink_dma() 가 제거.
	 * 읽는 자: vfio_find_dma(), vfio_find_dma_first_node() 와 rb_first/rb_next 로
	 *   전체를 순회하는 replay/dirty-bitmap 계열 함수들.
	 * 값 범위: 각 노드는 [iova, iova+size-1] 구간을 뜻하며 서로 겹치지 않는다.
	 * 동기화: lock 필드가 보호한다. RCU 조회는 쓰지 않는다. */
	struct rb_root		dma_list;
	/* [한국어] 이 컨테이너에 등록된 struct vfio_device 목록(iommu_entry 로 연결).
	 * dma_unmap 콜백을 가진 장치만 들어온다.
	 * 설정자: vfio_iommu_type1_register_device()/_unregister_device().
	 * 읽는 자: vfio_notify_dma_unmap() 이 언맵 직전에 순회하며 통지하고,
	 *   vfio_iommu_type1_pin_pages() 가 '통지 대상이 하나도 없으면 핀 금지' 검사에 쓴다.
	 * 값 범위: 비어 있으면 mdev 가 없다는 뜻.
	 * 동기화: 목록을 순회할 때는 device_list_lock, 비었는지만 볼 때는 lock.
	 *   등록/해제는 두 락을 모두 잡아 두 검사가 서로 모순되지 않게 한다. */
	struct list_head	device_list;
	/* [한국어] device_list 순회 전용 뮤텍스.
	 * 왜 두 번째 락이 필요한가: dma_unmap 콜백이 vfio_unpin_pages() 를 통해
	 *   다시 이 파일로 재진입해 lock 을 잡으려 하므로, 통지 중에는 lock 을
	 *   놓아야 한다. 그래도 목록 자체는 안전해야 하므로 별도 락을 쓴다.
	 * 설정자/읽는 자: register/unregister_device 와 vfio_notify_dma_unmap().
	 * 동기화 순서: 항상 lock -> device_list_lock 순으로 잡는다. */
	struct mutex		device_list_lock;
	/* [한국어] 아직 만들 수 있는 vfio_dma 의 남은 개수.
	 * 설정자: open 시 dma_entry_limit 로 초기화, vfio_dma_do_map() 이 하나 만들 때
	 *   감소, vfio_remove_dma() 가 지울 때 증가.
	 * 읽는 자: vfio_dma_do_map() 의 -ENOSPC 검사와 GET_INFO 의 dma_avail capability.
	 * 값 범위: 0 ~ dma_entry_limit. 0 이면 새 매핑 요청이 -ENOSPC 로 거절된다.
	 * 동기화: lock 이 보호한다. */
	unsigned int		dma_avail;
	/* [한국어] vaddr 가 무효화된 상태(VFIO_DMA_UNMAP_FLAG_VADDR 로 표시된) vfio_dma 의 개수.
	 * 설정자: vfio_dma_do_unmap() 이 무효화할 때 증가, vfio_dma_do_map() 의
	 *   set_vaddr 경로와 vfio_remove_dma() 가 감소.
	 * 읽는 자: pin_pages/dma_rw 콜백이 0 이 아니면 -EBUSY 로 거절하고,
	 *   attach_group 도 0 이 아니면 거부한다 — 유효하지 않은 vaddr 로는
	 *   페이지를 핀할 수 없기 때문이다.
	 * 값 범위: 0 이상. 0 이 정상 상태.
	 * 동기화: lock 이 보호한다. */
	unsigned int		vaddr_invalid_count;
	/* [한국어] 이 컨테이너의 모든 domain 이 공통으로 지원하는 IOMMU 페이지 크기 비트맵.
	 * 비트 n 이 서면 2^n 바이트 페이지를 지원한다는 뜻이다.
	 * 설정자: vfio_update_pgsize_bitmap() 이 domain 들의 비트맵을 AND 해 계산하고,
	 *   PAGE_SIZE 미만 비트는 지운 뒤 PAGE_SIZE 비트를 세운다.
	 * 읽는 자: __ffs() 로 최소 지원 크기를 뽑아 정렬 검사와 dirty 비트맵
	 *   단위로 쓰는 거의 모든 함수.
	 * 값 범위: open 직후에는 PAGE_MASK(= PAGE_SIZE 이상 모든 2의 거듭제곱).
	 * 동기화: lock 이 보호한다. */
	uint64_t		pgsize_bitmap;
	/* [한국어] 자체 dirty page 추적 능력을 보고하지 않는 IOMMU group 의 수.
	 * 설정자: attach 시 IOMMU 기반 group 이 붙으면 증가,
	 *   detach 시와 그 group 이 pin_pages 를 처음 쓸 때 감소.
	 * 읽는 자: update_user_bitmap() 이 0 이 아니면 매핑 전체를 dirty 로 칠한다
	 *   — 누가 어디에 썼는지 알 수 없으니 전부 더럽다고 보수적으로 보고하는 것이다.
	 * 값 범위: 0 이면 모든 group 이 핀 기반 추적을 하므로 정밀 보고가 가능하다.
	 * 동기화: lock 이 보호한다. */
	uint64_t		num_non_pinned_groups;
	/* [한국어] v2 인터페이스(VFIO_TYPE1v2_IOMMU)로 열렸는지.
	 * 설정자: vfio_iommu_type1_open() 이 arg 값에 따라 한 번만 설정.
	 * 읽는 자: vfio_dma_do_unmap() 의 언맵 경계 규칙, pin_pages/dirty_pages 의
	 *   진입 가드(v2 전용 기능).
	 * 값 범위: false 면 구식 v1 — 언맵이 기존 매핑을 잘라도 허용된다.
	 * 동기화: open 이후 불변이라 락 없이 읽어도 안전하다. */
	bool			v2;
	/* [한국어] 라이브 마이그레이션용 dirty page 추적이 켜져 있는지.
	 * 설정자: VFIO_IOMMU_DIRTY_PAGES ioctl 의 START/STOP 분기.
	 * 읽는 자: 새 매핑을 만들 때 비트맵을 함께 할당할지, 핀/쓰기 때 비트를
	 *   세울지 판단하는 곳들.
	 * 값 범위: true 인 동안에는 모든 vfio_dma 가 bitmap 을 갖고 있어야 한다.
	 * 동기화: lock 이 보호한다. */
	bool			dirty_page_tracking;
	/* [한국어] 에뮬레이트(mdev) IOMMU group 목록. 하드웨어 domain 이 없는 group 들이다.
	 * 설정자: attach_group 이 type 이 VFIO_EMULATED_IOMMU 일 때 list_add,
	 *   detach_group/release 가 list_del.
	 * 읽는 자: 마지막 group 이 빠질 때 전체 언맵 여부를 정하는 판단과,
	 *   VFIO_UPDATE_VADDR 기능 가용성 판정(mdev 가 있으면 금지).
	 * 값 범위: 비어 있는 것이 일반적인 PCI 패스스루 상황.
	 * 동기화: lock 이 보호한다. */
	struct list_head	emulated_iommu_groups;
};

/* [한국어] 하드웨어 IOMMU domain 하나를 감싸는 래퍼. 컨테이너 안에서 같은 iommu_ops 와
 * 같은 캐시 일관성 성질을 가진 group 들이 domain 하나를 공유한다.
 * 생성: vfio_iommu_type1_attach_group(), 소멸: 같은 함수의 실패 경로와
 * vfio_iommu_type1_detach_group()/vfio_iommu_type1_release(). */
struct vfio_domain {
	/* [한국어] IOMMU 코어가 만들어 준 실제 domain 객체. 페이지 테이블의 본체다.
	 * 설정자: vfio_iommu_domain_alloc() 콜백이 iommu_paging_domain_alloc() 결과를 저장.
	 * 읽는 자: iommu_map/iommu_unmap/iommu_iova_to_phys 를 부르는 모든 곳과
	 *   geometry(aperture) 조회.
	 * 값 범위: 유효 포인터이거나 ERR_PTR — 할당 직후 IS_ERR 로 검사한다.
	 * 동기화: iommu->lock 아래에서만 다룬다. 내부 구조는 이 트리에서 확인 못 함. */
	struct iommu_domain	*domain;
	/* [한국어] iommu->domain_list 에 매달리기 위한 목록 노드.
	 * 설정자/읽는 자: attach 의 list_add, detach 의 list_del, 그리고 모든 순회.
	 * 값 범위: 목록에 있는 동안만 유효.
	 * 동기화: iommu->lock. */
	struct list_head	next;
	/* [한국어] 이 domain 에 붙어 있는 struct vfio_iommu_group 들의 목록.
	 * 설정자: attach 가 group 을 list_add, detach 가 list_del.
	 * 읽는 자: find_iommu_group() 의 선형 탐색, vfio_release_domain() 의 일괄 해제.
	 * 값 범위: 비면 domain 을 해제해도 된다는 신호다(detach 가 그렇게 처리한다).
	 * 동기화: iommu->lock. */
	struct list_head	group_list;
	/* [한국어] 이 domain 이 no-snoop DMA 를 강제로 캐시 일관 처리할 수 있는지(비트필드 1비트).
	 * 설정자: attach 시 domain->ops 의 enforce_cache_coherency 콜백 결과.
	 * 읽는 자: vfio_domains_have_enforce_cache_coherency() 가 모든 domain 에 대해
	 *   AND 를 취해 VFIO_DMA_CC_IOMMU 확장 지원 여부로 보고한다. 또 domain 병합
	 *   판정에서 성질이 같은 domain 끼리만 합치도록 비교 대상이 된다.
	 * 값 범위: true 면 KVM 이 wbinvd 에뮬레이션을 끄는 최적화를 쓸 수 있다.
	 * 동기화: attach 시 한 번 정해지고 이후 불변. iommu->lock 아래에서 읽는다. */
	bool			enforce_cache_coherency : 1;
};

/* [한국어] MAP_DMA 요청 하나에 대응하는 매핑 하나. 이 파일의 중심 자료구조다.
 * 표현하는 것은 '사용자 vaddr 구간 <-> IOVA 구간' 의 대응과, 그 구간을
 * 위해 핀해 둔 페이지들에 대한 회계 정보다.
 * 생성: vfio_dma_do_map(), 소멸: vfio_remove_dma(). */
struct vfio_dma {
	/* [한국어] iommu->dma_list red-black tree 에 매달리기 위한 노드.
	 * 설정자: vfio_link_dma() 의 rb_link_node/rb_insert_color.
	 * 읽는 자: rb_entry() 로 노드에서 구조체를 복원하는 모든 탐색/순회.
	 * 값 범위: 트리에 있는 동안만 유효. 트리 정렬 키는 아래 iova 다.
	 * 동기화: iommu->lock. */
	struct rb_node		node;
	/* [한국어] 이 매핑의 시작 IOVA — 장치가 DMA 할 때 쓰는 주소. rb-tree 의 정렬 키다.
	 * 설정자: vfio_dma_do_map() 이 사용자가 준 map->iova 를 그대로 저장.
	 * 읽는 자: 트리 탐색, iommu_map/iommu_unmap 의 주소 인자, 비트맵 오프셋 계산.
	 * 값 범위: iommu->pgsize_bitmap 의 최소 크기에 정렬되어 있어야 하며,
	 *   iova_list 가 허용하는 구간 안이어야 한다. 유효 구간은 [iova, iova+size-1].
	 * 동기화: 생성 후 불변. iommu->lock 아래에서 읽는다. */
	dma_addr_t		iova;		/* Device address */
	/* [한국어] 이 매핑에 대응하는 사용자 공간 가상 주소의 시작.
	 * 설정자: vfio_dma_do_map(). VFIO_DMA_MAP_FLAG_VADDR 경로로 나중에 갱신될 수 있다.
	 * 읽는 자: vfio_pin_pages_remote() 가 핀할 주소를 계산할 때, dma_rw 가
	 *   copy_to_user/copy_from_user 대상 주소를 계산할 때.
	 * 값 범위: iova 와 같은 정렬 규칙. iova 와 vaddr 의 차가 두 주소 공간의
	 *   고정 오프셋이 되어 vaddr - dma->vaddr + dma->iova 같은 변환에 쓰인다.
	 * 동기화: iommu->lock. vaddr_invalid 가 true 인 동안에는 읽으면 안 된다. */
	unsigned long		vaddr;		/* Process virtual addr */
	/* [한국어] 이 매핑의 바이트 크기. 트리 구간의 길이이기도 하다.
	 * 설정자: vfio_link_dma() 로 넣을 때는 반드시 0 이고(WARN_ON), 그 뒤
	 *   vfio_pin_map_dma() 가 청크를 핀·매핑할 때마다 조금씩 키운다.
	 *   domain 이 없는 컨테이너에서는 vfio_dma_do_map() 이 곧바로 size 를 채운다.
	 * 읽는 자: 구간 겹침 판정, 언맵 범위 검사, 비트맵 비트 수 계산 등 거의 전부.
	 * 값 범위: 0 보다 크고 페이지 정렬. 0 인 상태는 삽입 직후의 과도 상태뿐이다.
	 * 동기화: iommu->lock. */
	size_t			size;		/* Map size (bytes) */
	/* [한국어] 이 매핑의 접근 권한 — IOMMU_READ, IOMMU_WRITE 의 비트 OR.
	 * 설정자: vfio_dma_do_map() 이 사용자 플래그를 변환해 저장. 장치 관점의 방향이다
	 *   (WRITE 는 장치가 메모리에 쓴다는 뜻).
	 * 읽는 자: iommu_map() 의 prot 인자, 핀할 때 FOLL_WRITE 를 붙일지 판단,
	 *   언핀할 때 페이지를 dirty 로 표시할지 판단(put_pfn 의 prot & IOMMU_WRITE).
	 * 값 범위: 최소한 하나는 서 있어야 한다 — 둘 다 0 이면 MAP 이 거절된다.
	 * 동기화: 생성 후 불변. */
	int			prot;		/* IOMMU_READ/WRITE */
	/* [한국어] 이 매핑이 실제로 IOMMU 페이지 테이블에 설치되어 있는지.
	 * 설정자: vfio_pin_map_dma() 가 매핑을 끝내며 true, vfio_unmap_unpin() 이 false,
	 *   vfio_iommu_replay() 가 재설치를 마치고 true.
	 * 읽는 자: replay 가 '이미 매핑되어 있으니 물리 주소는 IOMMU 에게 물어보면 된다'
	 *   와 '아직이니 지금 핀해야 한다' 를 가르는 데 쓴다. update_user_bitmap() 도
	 *   이 값이 true 일 때만 전체 dirty 처리를 한다.
	 * 값 범위: domain 이 없는 컨테이너에서는 false 로 남는다.
	 * 동기화: iommu->lock. */
	bool			iommu_mapped;
	/* [한국어] MAP_DMA 를 부른 시점에 그 프로세스가 CAP_IPC_LOCK 을 갖고 있었는지를 굳혀 둔 값.
	 * 설정자: vfio_dma_do_map() 의 capable(CAP_IPC_LOCK), 소유자 교체 시
	 *   vfio_change_dma_owner() 가 새 소유자 기준으로 다시 계산.
	 * 읽는 자: vfio_pin_pages_remote() 의 RLIMIT_MEMLOCK 한도 검사와
	 *   mm_lock_acct() 에 넘기는 인자.
	 * 값 범위: true 면 잠금 메모리 한도를 넘겨도 핀이 허용된다.
	 * 왜 굳혀 두는가: 언핀은 프로세스가 죽은 뒤 다른 문맥에서 일어날 수 있어
	 *   그때 capable() 을 다시 부르면 엉뚱한 답이 나오기 때문이다.
	 * 동기화: iommu->lock. */
	bool			lock_cap;	/* capable(CAP_IPC_LOCK) */
	/* [한국어] 이 매핑의 vaddr 가 무효 상태인지(VFIO_DMA_UNMAP_FLAG_VADDR 로 표시됨).
	 * 쓰임새: 프로세스가 fork/exec 등으로 주소 공간을 바꾸는 동안 IOVA 매핑은
	 *   유지한 채 vaddr 만 나중에 새 주소로 갱신하게 해 주는 기능이다.
	 * 설정자: vfio_dma_do_unmap() 의 invalidate_vaddr 경로가 true,
	 *   vfio_dma_do_map() 의 set_vaddr 경로가 false.
	 * 읽는 자: 위 두 함수와, 개수를 세는 iommu->vaddr_invalid_count 의 증감 지점.
	 * 값 범위: true 인 동안 이 매핑은 핀/rw 대상이 될 수 없다.
	 * 동기화: iommu->lock. */
	bool			vaddr_invalid;
	/* [한국어] 이 매핑 안에 struct page 가 없는 예약 pfn(MMIO 등)이 하나라도 섞여 있는지.
	 * 설정자: vfio_pin_pages_remote() 이 out 라벨에서 OR 로 누적한다 — 한 번 서면
	 *   매핑이 사라질 때까지 내려가지 않는다.
	 * 읽는 자: vfio_unpin_pages_remote() 가 true 면 페이지마다 put_pfn() 으로
	 *   하나씩 검사하며 풀고, false 면 구간 전체를 한 번에 푸는 빠른 경로를 쓴다.
	 * 값 범위: false 가 일반적인 일반 RAM 매핑.
	 * 동기화: iommu->lock. */
	bool			has_rsvd;	/* has 1 or more rsvd pfns */
	/* [한국어] 이 매핑의 잠금 메모리를 과금할 대상 task — MAP 시점 스레드의 group_leader.
	 * 설정자: vfio_dma_do_map() 이 get_task_struct() 로 참조를 잡고 저장,
	 *   vfio_change_dma_owner() 가 교체, vfio_remove_dma() 가 put_task_struct().
	 * 읽는 자: mm_lock_acct() 에 넘겨 RLIMIT_MEMLOCK 판정 주체로 쓰고,
	 *   한도 초과 경고 메시지에 comm/pid 를 찍는다.
	 * 값 범위: 항상 유효한 포인터(참조를 쥐고 있다). 스레드가 아니라 프로세스
	 *   대표를 쓰는 이유는 모든 DMA 회계를 한 주체로 모아 추적을 쉽게 하기 위해서다.
	 * 동기화: iommu->lock. */
	struct task_struct	*task;
	/* [한국어] 이 매핑 범위 안에서 mdev 가 따로 핀해 둔 페이지들의 rb-tree 루트.
	 * 키는 vfio_pfn.iova 이고 노드 하나가 정확히 한 페이지를 뜻한다.
	 * 설정자: vfio_link_pfn()/vfio_unlink_pfn().
	 * 읽는 자: vpfn_pages() 가 중복 과금을 피하려고 개수를 세고,
	 *   vfio_dma_do_unmap() 이 비어 있지 않으면 언맵을 미룬다.
	 * 값 범위: 대개 비어 있다(RB_EMPTY_ROOT). 비어 있지 않으면 vfio_remove_dma() 가
	 *   WARN_ON 을 낸다 — 지우기 전에 반드시 비워져 있어야 한다.
	 * 동기화: iommu->lock. */
	struct rb_root		pfn_list;	/* Ex-user pinned pfn list */
	/* [한국어] 라이브 마이그레이션용 dirty page 비트맵. 비트 하나가 최소 지원 페이지 크기 하나.
	 * 설정자: vfio_dma_bitmap_alloc() 이 할당, vfio_dma_bitmap_free() 가 해제,
	 *   bitmap_set/bitmap_clear 계열이 내용 갱신.
	 * 읽는 자: update_user_bitmap() 이 사용자 버퍼로 복사한다.
	 * 값 범위: dirty_page_tracking 이 꺼져 있으면 NULL. 할당 크기는 필요한
	 *   바이트 + sizeof(u64) 로, 인접 매핑과 비트를 이어 붙일 때 쓰는 여유분이다.
	 * 동기화: iommu->lock. */
	unsigned long		*bitmap;
	/* [한국어] 이 매핑의 사용자 주소가 속한 mm_struct.
	 * 설정자: vfio_dma_do_map() 이 mmgrab() 으로 참조를 잡고 저장,
	 *   vfio_change_dma_owner() 가 교체, vfio_remove_dma() 가 mmdrop().
	 * 읽는 자: GUP 를 부를 때의 대상 mm, 과금 대상 mm, dma_rw 가 빌려 쓸 mm.
	 * 값 범위: mmgrab 은 mm_struct 구조체만 살려 둘 뿐 주소 공간이 이미 해체됐을
	 *   수 있으므로, 실제로 접근하기 전에는 mmget_not_zero() 로 승격해야 한다.
	 * 동기화: iommu->lock. 참조 카운트는 mm 코어가 관리한다. */
	struct mm_struct	*mm;
	/* [한국어] 이 매핑이 지금까지 이 mm 앞으로 과금한 잠금 페이지 수의 누계.
	 * 설정자: vfio_lock_acct() 가 성공할 때마다 npage 를 더한다(음수면 뺀다).
	 * 읽는 자: vfio_change_dma_owner() 가 소유자를 바꿀 때 옛 mm 에서 뺄 양과
	 *   새 mm 에 더할 양으로 이 값을 그대로 쓴다.
	 * 값 범위: 0 이상이어야 정상. 핀과 언핀이 짝을 이루면 매핑 소멸 시 0 이 된다.
	 * 동기화: iommu->lock. */
	size_t			locked_vm;
};

/* [한국어] GUP 한 번에 여러 페이지를 받아 오기 위한 임시 버퍼.
 * 왜 필요한가: 페이지를 한 장씩 핀하면 매핑 하나당 GUP 호출이 수십만 번이 된다.
 *   한 페이지 크기의 배열(512~1024 항목)에 몰아 받아 오면 호출 횟수가 그만큼 준다.
 * 수명: 스택에 잡고 vfio_batch_init() 으로 초기화, vfio_batch_fini() 로 정리한다. */
struct vfio_batch {
	/* [한국어] GUP 가 채워 줄 struct page 포인터 배열.
	 * 설정자: __vfio_batch_init() 이 __get_free_page() 로 잡거나, 실패 시
	 *   아래 fallback_page 를 가리키게 한다. 내용은 pin_user_pages_remote() 가 채운다.
	 * 읽는 자: vfio_pin_pages_remote() 가 offset 부터 순서대로 꺼내 쓴다.
	 * 값 범위: 절대 NULL 이 아니다 — 할당 실패 시에도 fallback 이 있다.
	 * 동기화: 스택 지역 객체라 단일 스레드 전용. 락 불필요. */
	struct page		**pages;	/* for pin_user_pages_remote */
	/* [한국어] 페이지 배열 할당이 실패했을 때 쓰는 1개짜리 예비 슬롯.
	 * 설정자/읽는 자: __vfio_batch_init() 이 pages 를 여기로 향하게 하고,
	 *   이후에는 pages[0] 으로만 접근된다.
	 * 값 범위: 이 경로에서는 capacity 가 1 이 되어 한 번에 한 페이지만 핀한다.
	 * 동기화: 불필요(스택 지역). */
	struct page		*fallback_page; /* if pages alloc fails */
	/* [한국어] pages 배열이 담을 수 있는 항목 수.
	 * 설정자: __vfio_batch_init() 이 VFIO_BATCH_MAX_CAPACITY 또는 1 로 설정.
	 * 읽는 자: vaddr_get_pfns() 가 한 번에 요청할 페이지 수의 상한으로 쓰고,
	 *   vfio_batch_fini() 가 이 값으로 '페이지를 할당했었는가' 를 판별한다.
	 * 값 범위: VFIO_BATCH_MAX_CAPACITY 또는 1 둘 중 하나.
	 * 동기화: 불필요. */
	unsigned int		capacity;	/* length of pages array */
	/* [한국어] 배치에 아직 남아 있는(소비되지 않은) 페이지 수.
	 * 설정자: vaddr_get_pfns() 가 GUP 반환값으로 채우고, 소비하는 쪽이 감소시킨다.
	 * 읽는 자: vfio_pin_pages_remote() 의 '배치가 비었으면 다시 채운다' 판정과,
	 *   vfio_batch_unpin() 의 잔여 페이지 반환 루프.
	 * 값 범위: 0 ~ capacity. 0 은 다시 채워야 한다는 뜻이며, GUP 가 페이지를
	 *   못 준 pfnmap 경로에서도 0 으로 남는다.
	 * 동기화: 불필요. */
	unsigned int		size;		/* of batch currently */
	/* [한국어] pages 배열에서 다음에 꺼낼 항목의 인덱스.
	 * 설정자: vaddr_get_pfns() 가 0 으로 리셋, 소비하는 쪽이 소비한 만큼 증가.
	 * 읽는 자: pages[offset] 접근 전부.
	 * 값 범위: offset + size 가 항상 GUP 가 채운 개수와 같도록 유지된다 —
	 *   이 불변식이 깨지면 이미 소비한 페이지를 다시 언핀하거나 놓치게 된다.
	 * 동기화: 불필요. */
	unsigned int		offset;		/* of next entry in pages */
};

/* [한국어] 컨테이너에 붙은 IOMMU group 하나를 감싸는 래퍼.
 * 생성: vfio_iommu_type1_attach_group(), 소멸: _detach_group()/_release()/
 * vfio_release_domain(). domain->group_list 또는 iommu->emulated_iommu_groups
 * 둘 중 하나에만 매달린다. */
struct vfio_iommu_group {
	/* [한국어] IOMMU 코어가 관리하는 실제 group 객체. PCIe ACS 격리 단위와 대응한다
	 * (ACS 를 켜는 코드는 drivers/pci/pci-driver.c:3618 의 pci_enable_acs()).
	 * 설정자: attach 시 인자로 받은 값을 저장.
	 * 읽는 자: iommu_attach_group/iommu_detach_group 호출과 목록 탐색의 비교 키.
	 * 값 범위: 항상 유효 — 참조는 상위 VFIO 코어가 쥐고 있다.
	 * 동기화: iommu->lock 아래에서만 다룬다. */
	struct iommu_group	*iommu_group;
	/* [한국어] domain->group_list 또는 iommu->emulated_iommu_groups 에 매달리는 목록 노드.
	 * 설정자/읽는 자: attach 의 list_add, detach 의 list_del, 모든 순회.
	 * 값 범위: 두 목록 중 정확히 하나에만 들어 있다.
	 * 동기화: iommu->lock. */
	struct list_head	next;
	/* [한국어] 이 group 이 '핀 기반 dirty 추적' 범위 안에 있는지.
	 * 설정자: 에뮬레이트 group 은 attach 시 곧바로 true, IOMMU 기반 group 은
	 *   vfio_iommu_type1_pin_pages() 를 처음 성공했을 때 true 로 승격된다.
	 * 읽는 자: 승격/강등 시 iommu->num_non_pinned_groups 를 조정하는 판단.
	 * 값 범위: false 면 이 group 의 장치가 어디에 DMA 했는지 알 수 없어
	 *   마이그레이션 시 매핑 전체를 dirty 로 보고해야 한다.
	 * 동기화: iommu->lock. */
	bool			pinned_page_dirty_scope;
};

/* [한국어] 사용자가 매핑해도 되는 IOVA 구간 하나. iommu->iova_list 의 원소다.
 * 생성: vfio_iommu_iova_insert(), 소멸: vfio_iommu_iova_free().
 * 목록은 주소 오름차순으로 정렬되어 있고 원소끼리 겹치지 않는다. */
struct vfio_iova {
	/* [한국어] iommu->iova_list(또는 작업용 사본 목록)에 매달리는 목록 노드.
	 * 설정자: vfio_iommu_iova_insert() 의 list_add_tail.
	 * 읽는 자: 모든 순회와 vfio_iommu_resv_exclude() 의 중간 삽입.
	 * 값 범위: 목록에 있는 동안 유효.
	 * 동기화: iommu->lock. 사본 목록은 스택 지역이라 락 없이 다룬다. */
	struct list_head	list;
	/* [한국어] 구간의 시작 주소(포함).
	 * 설정자: vfio_iommu_iova_insert() 인자, vfio_iommu_aper_resize() 의 축소,
	 *   vfio_iommu_aper_expand() 의 확대.
	 * 읽는 자: vfio_iommu_iova_dma_valid() 의 포함 검사, GET_INFO capability 작성.
	 * 값 범위: end 이하.
	 * 동기화: iommu->lock. */
	dma_addr_t		start;
	/* [한국어] 구간의 끝 주소(포함, 즉 마지막 유효 바이트).
	 * 왜 배타적 끝이 아닌가: 주소 공간 최상단까지 덮는 구간에서 end+1 이 0 으로
	 *   넘치는 것을 피하기 위해서다. 이 파일 전체가 같은 규약을 쓴다.
	 * 설정자/읽는 자: start 와 같다.
	 * 값 범위: start 이상.
	 * 동기화: iommu->lock. */
	dma_addr_t		end;
};

/*
 * Guest RAM pinning working set or DMA target
 */
/* [한국어] mdev 가 vfio_pin_pages() 로 따로 핀해 둔 페이지 하나를 기록하는 노드.
 * 왜 필요한가: 이 페이지들은 이미 핀되고 과금되었으므로, 나중에 같은 범위를
 *   vfio_pin_pages_remote() 가 다시 핀할 때 이중 과금하지 않도록 세어 빼야 한다.
 * 생성: vfio_add_to_pfn_list(), 소멸: vfio_remove_from_pfn_list(). */
struct vfio_pfn {
	/* [한국어] dma->pfn_list rb-tree 에 매달리기 위한 노드.
	 * 설정자: vfio_link_pfn() 의 rb_link_node/rb_insert_color.
	 * 읽는 자: rb_entry() 로 구조체를 복원하는 모든 탐색/순회.
	 * 값 범위: 트리에 있는 동안 유효.
	 * 동기화: iommu->lock. */
	struct rb_node		node;
	/* [한국어] 이 페이지가 놓인 IOVA. rb-tree 의 정렬 키다.
	 * 설정자: vfio_add_to_pfn_list() 인자.
	 * 읽는 자: vfio_find_vpfn_range() 의 탐색 키, dirty 비트맵 비트 위치 계산.
	 * 값 범위: 반드시 페이지 정렬. 노드 하나가 정확히 PAGE_SIZE 를 뜻한다.
	 * 동기화: iommu->lock. */
	dma_addr_t		iova;		/* Device address */
	/* [한국어] 이 페이지의 호스트 물리 페이지 프레임 번호.
	 * 설정자: vfio_add_to_pfn_list() 인자(GUP 결과에서 얻은 값).
	 * 읽는 자: 언핀할 때 put_pfn() 의 인자, 예약 페이지인지 판정.
	 * 값 범위: pfn_valid() 를 통과한 값만 저장된다
	 *   (vfio_iommu_type1_pin_pages() 가 미리 검사한다).
	 * 동기화: iommu->lock. */
	unsigned long		pfn;		/* Host pfn */
	/* [한국어] 같은 IOVA 에 대한 중복 핀 횟수.
	 * 설정자: 생성 시 1, vfio_iova_get_vfio_pfn() 이 증가,
	 *   vfio_iova_put_vfio_pfn() 이 감소하고 0 이 되면 노드를 없앤다.
	 * 읽는 자: 위 두 함수.
	 * 값 범위: 1 이상. 0 이 되는 순간 실제 핀이 풀리고 노드가 해제된다.
	 * 동기화: iommu->lock 이 감싸므로 원자 연산이 필요 없다. */
	unsigned int		ref_count;
};

/* [한국어] 빠른 언맵 경로에서 '아직 IOTLB 를 비우지 않아 언핀을 미뤄 둔 구간' 하나.
 * 왜 필요한가: iommu_unmap_fast() 는 페이지 테이블만 지우고 IOTLB 플러시는
 *   나중에 몰아서 한다. 플러시 전에 페이지를 언핀해 재사용시키면 장치가
 *   낡은 IOTLB 항목으로 남의 메모리에 DMA 할 수 있으므로, 플러시가 끝날
 *   때까지 목록에 담아 둔다.
 * 생성: unmap_unpin_fast(), 소멸: vfio_sync_unpin(). */
struct vfio_regions {
	/* [한국어] 호출자 스택의 unmapped_region_list 에 매달리는 목록 노드.
	 * 설정자: unmap_unpin_fast() 의 list_add_tail.
	 * 읽는 자: vfio_sync_unpin() 의 list_for_each_entry_safe.
	 * 값 범위: 목록에 있는 동안 유효.
	 * 동기화: 스택 지역 목록이며 iommu->lock 아래에서만 다룬다. */
	struct list_head list;
	/* [한국어] 언맵된 구간의 시작 IOVA.
	 * 설정자: unmap_unpin_fast().
	 * 읽는 자: vfio_sync_unpin() 이 vfio_unpin_pages_remote() 에 넘겨
	 *   중복 과금 계산(vpfn_pages)의 기준 주소로 쓴다.
	 * 값 범위: 페이지 정렬.
	 * 동기화: 위와 같다. */
	dma_addr_t iova;
	/* [한국어] 언맵된 구간이 가리키던 호스트 물리 주소.
	 * 설정자: unmap_unpin_fast() 가 iommu_iova_to_phys() 결과를 저장.
	 * 읽는 자: vfio_sync_unpin() 이 PAGE_SHIFT 만큼 내려 pfn 으로 바꿔 언핀한다.
	 * 값 범위: 페이지 정렬. 0 이면 매핑이 없다는 뜻이라 여기 들어오지 않는다.
	 * 동기화: 위와 같다. */
	phys_addr_t phys;
	/* [한국어] 실제로 언맵된 바이트 수 — 요청 길이가 아니라 iommu_unmap_fast() 의 반환값이다.
	 * 설정자: unmap_unpin_fast().
	 * 읽는 자: vfio_sync_unpin() 이 PAGE_SHIFT 만큼 내려 언핀할 페이지 수로 쓴다.
	 * 값 범위: 0 보다 큼(0 이면 항목을 만들지 않는다).
	 * 동기화: 위와 같다. */
	size_t len;
};

/* [한국어] 페이지 n 개를 담는 dirty 비트맵의 바이트 크기.
 * n 을 u64 비트 수(64)의 배수로 올림한 뒤 8 로 나눈다. 64비트 워드 단위로
 *   올림하는 이유는 bitmap_shift_left()/bitmap_or() 가 워드 단위로 동작하고,
 *   사용자 버퍼도 u64 배열로 정의되어 있기 때문이다. */
#define DIRTY_BITMAP_BYTES(n)	(ALIGN(n, BITS_PER_TYPE(u64)) / BITS_PER_BYTE)

/*
 * Input argument of number of bits to bitmap_set() is unsigned integer, which
 * further casts to signed integer for unaligned multi-bit operation,
 * __bitmap_set().
 * Then maximum bitmap size supported is 2^31 bits divided by 2^3 bits/byte,
 * that is 2^28 (256 MB) which maps to 2^31 * 2^12 = 2^43 (8TB) on 4K page
 * system.
 */
/* [한국어] dirty 비트맵이 담을 수 있는 최대 페이지 수 = INT_MAX(2^31-1).
 * 위 영어 주석의 설명대로 bitmap_set() 의 비트 수 인자가 결국 부호 있는
 *   정수로 캐스팅되기 때문에 생긴 한계다. */
#define DIRTY_BITMAP_PAGES_MAX	 ((u64)INT_MAX)
/* [한국어] 위 한계를 바이트로 환산한 값(약 256MB). GET_INFO 의 migration capability 가
 *   max_dirty_bitmap_size 로 사용자에게 보고하고, verify_bitmap_size() 가
 *   사용자가 준 버퍼 크기의 상한 검사에 쓴다. */
#define DIRTY_BITMAP_SIZE_MAX	 DIRTY_BITMAP_BYTES(DIRTY_BITMAP_PAGES_MAX)

/* [한국어] put_pfn() 전방 선언 — 아래 vfio_iova_put_vfio_pfn() 이 정의보다 먼저
 *   이 함수를 부르기 때문에 필요하다. */
static int put_pfn(unsigned long pfn, int prot);

/* [한국어] vfio_iommu_find_iommu_group() 전방 선언 — vfio_iommu_type1_pin_pages() 가
 *   파일 뒤쪽에 정의된 이 함수를 앞에서 부른다. */
static struct vfio_iommu_group*
vfio_iommu_find_iommu_group(struct vfio_iommu *iommu,
			    struct iommu_group *iommu_group);

/*
 * This code handles mapping and unmapping of user data buffers
 * into DMA'ble space using the IOMMU
 */

/* [한국어]
 * vfio_find_dma - 주어진 IOVA 구간과 겹치는 vfio_dma 를 rb-tree 에서 찾는다
 *
 * @iommu: 컨테이너 객체. iommu->dma_list 가 탐색 대상 red-black tree 다.
 * @start: 찾으려는 구간의 시작 IOVA.
 * @size: 찾으려는 구간의 바이트 길이. 0 이면 WARN_ON 이 울린다 —
 *        size 0 은 아래 start+size-1 계산을 언더플로시켜 뜻이 달라지기 때문이다.
 * @return: 겹치는 vfio_dma 포인터, 없으면 NULL.
 *
 * 이 트리의 노드는 점이 아니라 [iova, iova+size-1] 닫힌 구간을 뜻하고, 구간들은
 * 서로 겹치지 않는다는 불변식이 있다. 그래서 일반적인 '키 같은 값 찾기' 가 아니라
 * '구간이 겹치는 노드 찾기' 를 해야 한다. 이 함수는 그 겹침 판정을 이진 탐색으로 한다.
 *
 * 동작 단계:
 *  1. 루트에서 시작해 노드를 하나 꺼낸다.
 *  2. 요청 구간의 마지막 바이트(start+size-1)가 노드의 시작보다 작으면 요청이
 *     완전히 왼쪽이므로 rb_left 로 내려간다.
 *  3. 요청의 시작이 노드의 마지막 바이트보다 크면 완전히 오른쪽이므로 rb_right.
 *  4. 둘 다 아니면 반드시 겹치므로 그 노드를 반환한다.
 *  5. 잎까지 내려가 node 가 NULL 이 되면 겹치는 매핑이 없다는 뜻이라 NULL.
 * 구간의 끝을 언제나 '+size-1' 로 계산하는 이유는 주소 공간 최상단을 덮는 매핑에서
 * start+size 가 0 으로 넘치는 것을 피하기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 반드시 iommu->lock 을 쥔 상태여야 한다.
 * 잠들지 않고 메모리도 할당하지 않는 순수 탐색 함수다.
 *
 * caller: vfio_dma_do_map(), vfio_dma_do_unmap(), vfio_iommu_type1_pin_pages(),
 *   vfio_iommu_type1_unpin_pages(), vfio_iova_dirty_bitmap(),
 *   vfio_iommu_aper_conflict(), vfio_iommu_resv_conflict(),
 *   vfio_iommu_type1_dma_rw_chunk().
 * callee: rb_entry() 매크로뿐. 다른 함수를 부르지 않는다.
 * 에러 경로: 없다. 못 찾으면 NULL 을 돌려주고, 그것을 에러로 볼지는 호출자가 정한다.
 *
 * 호출 체인:
 *   vfio_dma_do_map() → [vfio_find_dma] → rb_entry()
 */
static struct vfio_dma *vfio_find_dma(struct vfio_iommu *iommu,
				      dma_addr_t start, size_t size)
{
	struct rb_node *node = iommu->dma_list.rb_node;

	/* [한국어] 크기 0 은 아래 start+size-1 계산을 언더플로시켜 뜻이 달라진다. */
	WARN_ON(!size);

	/* [한국어] 잎에 닿을 때까지 내려간다. */
	while (node) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);

		/* [한국어] 요청의 마지막 바이트가 노드 시작보다 앞이면 요청이 완전히 왼쪽이다. */
		if (start + size - 1 < dma->iova)
			/* [한국어] 왼쪽으로 내려간다. */
			node = node->rb_left;
		/* [한국어] 요청 시작이 노드의 마지막 바이트보다 뒤면 완전히 오른쪽이다. */
		else if (start > dma->iova + dma->size - 1)
			/* [한국어] 오른쪽으로 내려간다. */
			node = node->rb_right;
		/* [한국어] 둘 다 아니면 */
		else
			/* [한국어] 둘 다 아니면 반드시 겹친다. */
			return dma;
	}

	/* [한국어] 잎까지 내려갔으면 겹치는 매핑이 없다. */
	return NULL;
}

/* [한국어]
 * vfio_find_dma_first_node - [start, end] 범위에 걸치는 첫(가장 낮은 IOVA) 노드를 찾는다
 *
 * @iommu: 컨테이너 객체.
 * @start: 범위의 시작 IOVA(포함).
 * @end: 범위의 끝 IOVA(포함). start 보다 작으면 WARN_ON.
 * @return: 범위와 겹치는 노드 중 IOVA 가 가장 작은 rb_node, 없으면 NULL.
 *
 * vfio_find_dma() 는 겹치는 노드 '아무거나' 를 돌려주지만, 여러 매핑을 한 번에
 * 언맵할 때는 가장 낮은 것부터 rb_next() 로 순서대로 훑어야 한다. 그 시작점을
 * 찾는 것이 이 함수다.
 *
 * 동작 단계:
 *  1. 루트에서 내려가면서, 노드의 마지막 바이트(iova+size-1)가 start 이상이면
 *     이 노드는 후보다 — res 에 기억해 두고 더 왼쪽에 더 작은 후보가 있는지
 *     rb_left 로 계속 내려간다.
 *  2. 단 노드의 시작이 이미 start 이하이면 그보다 왼쪽에는 겹치는 노드가 있을 수
 *     없으므로(구간이 겹치지 않는다는 불변식) 곧바로 break 한다.
 *  3. 노드가 start 보다 완전히 왼쪽이면 rb_right 로 내려간다.
 *  4. 마지막으로, 찾은 후보의 시작이 end 보다 크면 요청 범위 밖이므로 NULL 로 바꾼다.
 *     (1) 단계는 'start 이상' 만 보므로 범위의 오른쪽 끝 검사는 여기서 한 번에 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 순수 탐색.
 *
 * caller: vfio_dma_do_unmap() 한 곳뿐이다.
 * callee: rb_entry() 매크로.
 * 에러 경로: 없다. NULL 은 '범위 안에 매핑이 하나도 없다' 는 정상적인 결과다.
 *
 * 호출 체인:
 *   vfio_dma_do_unmap() → [vfio_find_dma_first_node] → rb_entry()
 */
static struct rb_node *vfio_find_dma_first_node(struct vfio_iommu *iommu,
						dma_addr_t start,
						dma_addr_t end)
{
	struct rb_node *res = NULL;
	/* [한국어] 탐색 커서 — 루트에서 시작한다. */
	struct rb_node *node = iommu->dma_list.rb_node;
	/* [한국어] 지금까지 찾은 최선 후보의 구조체. */
	struct vfio_dma *dma_res = NULL;

	/* [한국어] 범위가 거꾸로면 호출자의 버그다. */
	WARN_ON(end < start);

	/* [한국어] 잎에 닿을 때까지 내려간다. */
	while (node) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		struct vfio_dma *dma = rb_entry(node, struct vfio_dma, node);

		/* [한국어] 이 노드의 마지막 바이트가 범위 시작 이상이면 겹칠 가능성이 있는 후보다. */
		if (start <= dma->iova + dma->size - 1) {
			/* [한국어] 후보로 기억해 둔다. */
			res = node;
			/* [한국어] 구조체도 함께 기억한다 — 아래 범위 밖 판정에 쓴다. */
			dma_res = dma;
			/* [한국어] 이 노드의 시작이 이미 범위 시작 이하면 더 왼쪽에는 겹치는 노드가 없다. */
			if (start >= dma->iova)
				break;
			/* [한국어] 더 낮은 후보를 찾아 왼쪽으로 내려간다. */
			node = node->rb_left;
		} else {
			/* [한국어] 노드가 범위보다 완전히 왼쪽이면 오른쪽으로 내려간다. */
			node = node->rb_right;
		}
	}
	/* [한국어] 찾은 후보가 범위 끝보다 뒤에 있으면 범위 밖이다 — 위 루프는 시작 조건만 보았기 때문이다. */
	if (res && dma_res->iova > end)
		/* [한국어] 결과를 무효로 만든다. */
		res = NULL;
	/* [한국어] 찾은 후보 노드를 돌려준다. NULL 이면 범위 안에 매핑이 없다. */
	return res;
}

/* [한국어]
 * vfio_link_dma - 새 vfio_dma 를 컨테이너의 rb-tree 에 끼워 넣는다
 *
 * @iommu: 컨테이너 객체. iommu->dma_list 가 대상 트리다.
 * @new: 삽입할 노드. size 가 반드시 0 이어야 한다.
 * @return: 없음.
 *
 * 왜 size 가 0 이어야 하는가(WARN_ON(new->size != 0) 의 의미): 이 트리의 불변식은
 * '구간이 서로 겹치지 않는다' 이고, 정렬 비교는 iova 만 본다. 크기를 가진 노드를
 * 바로 넣으면 삽입 시점에 겹침을 확인할 방법이 없다. 그래서 vfio_dma_do_map() 은
 * 크기 0 짜리 빈 노드를 먼저 넣고, 그 뒤 vfio_pin_map_dma() 가 청크를 매핑할 때마다
 * size 를 키운다. 겹치지 않는다는 보장은 삽입 전에 vfio_find_dma() 로 이미 확인한다.
 *
 * 동작 단계:
 *  1. link 를 루트 포인터의 주소로 잡고 잎에 닿을 때까지 내려간다.
 *  2. 새 노드의 iova 가 현재 노드 이하이면 왼쪽, 크면 오른쪽 가지 포인터를 따라간다.
 *  3. rb_link_node() 로 부모와 연결 위치를 확정하고, rb_insert_color() 로 red-black
 *     트리의 색 규칙에 맞게 재균형한다. 이 두 호출은 반드시 짝으로 불러야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들지 않는다.
 *
 * caller: vfio_dma_do_map() 한 곳뿐.
 * callee: rb_entry(), rb_link_node(), rb_insert_color().
 * 에러 경로: 없다. 실패할 수 없는 연산이다.
 *
 * 호출 체인:
 *   vfio_dma_do_map() → [vfio_link_dma] → rb_insert_color()
 */
static void vfio_link_dma(struct vfio_iommu *iommu, struct vfio_dma *new)
{
	struct rb_node **link = &iommu->dma_list.rb_node, *parent = NULL;
	/* [한국어] 비교 대상 노드. */
	struct vfio_dma *dma;

	/* [한국어] 삽입 시점에는 반드시 크기 0 이어야 한다 — 크기를 가진 채 넣으면 겹침을 확인할 방법이 없다. */
	WARN_ON(new->size != 0);

	/* [한국어] 잎에 닿을 때까지 내려간다. */
	while (*link) {
		/* [한국어] 현재 자리가 부모 후보다. */
		parent = *link;
		/* [한국어] 노드에서 구조체를 복원한다. */
		dma = rb_entry(parent, struct vfio_dma, node);

		/* [한국어] IOVA 가 작거나 같으면 왼쪽, */
		if (new->iova <= dma->iova)
			/* [한국어] 왼쪽 가지 포인터를 따라간다. */
			link = &(*link)->rb_left;
		/* [한국어] 요청이 노드보다 크면 */
		else
			/* [한국어] 크면 오른쪽 가지로 내려간다. */
			link = &(*link)->rb_right;
	}

	/* [한국어] 부모와 연결 위치를 확정한다. */
	rb_link_node(&new->node, parent, link);
	/* [한국어] red-black 트리의 색 규칙에 맞게 재균형한다. 위 rb_link_node 와 짝이다. */
	rb_insert_color(&new->node, &iommu->dma_list);
}

/* [한국어]
 * vfio_unlink_dma - vfio_dma 를 컨테이너의 rb-tree 에서 뺀다
 *
 * @iommu: 컨테이너 객체.
 * @old: 뺄 노드. 반드시 트리에 들어 있어야 한다.
 * @return: 없음.
 *
 * vfio_link_dma() 의 짝이다. 트리에서 빼기만 할 뿐 메모리를 해제하거나 핀을 풀지는
 * 않는다 — 그 일은 호출자인 vfio_remove_dma() 가 순서대로 처리한다. 삽입과 제거를
 * 한 줄짜리 함수로 감싸 둔 이유는 이 트리를 만지는 지점을 두 곳으로 못박아,
 * 락 규약을 검토하기 쉽게 하려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_remove_dma() 한 곳뿐.
 * callee: rb_erase() — red-black 트리에서 노드를 빼고 재균형한다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_remove_dma() → [vfio_unlink_dma] → rb_erase()
 */
static void vfio_unlink_dma(struct vfio_iommu *iommu, struct vfio_dma *old)
{
	rb_erase(&old->node, &iommu->dma_list);
}


/* [한국어]
 * vfio_dma_bitmap_alloc - 매핑 하나에 대한 dirty page 비트맵을 할당한다
 *
 * @dma: 대상 매핑. dma->size 로 필요한 비트 수를 계산하고 dma->bitmap 에 저장한다.
 * @pgsize: 비트 하나가 뜻하는 바이트 크기. 언제나 컨테이너가 지원하는 최소
 *          IOMMU 페이지 크기다.
 * @return: 0 성공, -EINVAL(범위 초과), -ENOMEM(할당 실패).
 *
 * 라이브 마이그레이션에서 게스트 메모리 중 어디가 변경되었는지 사용자 공간에
 * 알려 주려면 매핑마다 비트맵이 필요하다. 이 함수가 그 저장소를 만든다.
 *
 * 동작 단계:
 *  1. npages = dma->size / pgsize 로 필요한 비트 수를 구한다.
 *  2. DIRTY_BITMAP_PAGES_MAX(INT_MAX)를 넘으면 -EINVAL. 상류 주석대로 bitmap_set() 의
 *     비트 수 인자가 결국 부호 있는 정수가 되기 때문에 생긴 상한이다.
 *  3. DIRTY_BITMAP_BYTES(npages) + sizeof(u64) 만큼 kvzalloc 으로 0 초기화 할당한다.
 *     64비트를 더 얹는 이유는 상류 주석대로 update_user_bitmap() 이
 *     bitmap_shift_left() 로 비트열을 통째로 밀 때 밀려 나갈 자리가 필요해서다.
 *     kvzalloc 을 쓰는 이유는 큰 매핑에서 비트맵이 수 MB 가 될 수 있어 연속
 *     물리 메모리를 요구하지 않는 vmalloc 대체 경로가 필요하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. GFP_KERNEL 할당이라
 * 잠들 수 있다.
 *
 * caller: vfio_dma_bitmap_alloc_all()(추적 시작 시 전체 순회),
 *   vfio_dma_do_map()(추적 중에 새 매핑이 생겼을 때).
 * callee: kvzalloc().
 * 에러 경로: -ENOMEM 이면 호출자가 그때까지 만든 비트맵을 되돌린다
 *   (vfio_dma_bitmap_alloc_all() 의 rb_prev 역순 해제, vfio_dma_do_map() 의
 *   vfio_remove_dma()).
 *
 * 호출 체인:
 *   vfio_iommu_type1_dirty_pages() → vfio_dma_bitmap_alloc_all() →
 *   [vfio_dma_bitmap_alloc] → kvzalloc()
 */
static int vfio_dma_bitmap_alloc(struct vfio_dma *dma, size_t pgsize)
{
	uint64_t npages = dma->size / pgsize;

	/* [한국어] 커널이 한 번에 다룰 수 있는 비트 수를 넘으면 거절한다. */
	if (npages > DIRTY_BITMAP_PAGES_MAX)
		return -EINVAL;

	/*
	 * Allocate extra 64 bits that are used to calculate shift required for
	 * bitmap_shift_left() to manipulate and club unaligned number of pages
	 * in adjacent vfio_dma ranges.
	 */
	/* [한국어] 필요한 바이트에 64비트를 더해 0 초기화 할당한다. */
	dma->bitmap = kvzalloc(DIRTY_BITMAP_BYTES(npages) + sizeof(u64),
			       /* [한국어] kvzalloc 은 연속 물리 메모리를 요구하지 않아 수 MB 짜리 비트맵도 잡을 수 있다. */
			       GFP_KERNEL);
	/* [한국어] 할당 실패. */
	if (!dma->bitmap)
		return -ENOMEM;

	return 0;
}

/* [한국어]
 * vfio_dma_bitmap_free - 매핑의 dirty page 비트맵을 해제한다
 *
 * @dma: 대상 매핑.
 * @return: 없음.
 *
 * vfio_dma_bitmap_alloc() 의 짝. 해제 후 포인터를 NULL 로 만들어 두 번 해제되거나
 * 해제된 메모리를 참조하는 것을 막는다. dma->bitmap 이 이미 NULL 이어도 kvfree(NULL)
 * 은 안전하므로 별도 검사가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_dma_bitmap_free_all()(추적 중지), vfio_remove_dma()(매핑 소멸),
 *   vfio_dma_bitmap_alloc_all() 의 실패 되감기.
 * callee: kvfree().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_remove_dma() → [vfio_dma_bitmap_free] → kvfree()
 */
static void vfio_dma_bitmap_free(struct vfio_dma *dma)
{
	kvfree(dma->bitmap);
	/* [한국어] 포인터를 지워 두 번 해제하거나 해제된 메모리를 참조하는 것을 막는다. */
	dma->bitmap = NULL;
}

/* [한국어]
 * vfio_dma_populate_bitmap - 외부에서 핀된 페이지들을 dirty 로 표시한다
 *
 * @dma: 대상 매핑. dma->pfn_list rb-tree 를 순회한다.
 * @pgsize: 비트 하나가 뜻하는 바이트 크기.
 * @return: 없음.
 *
 * 왜 필요한가: mdev 가 vfio_pin_pages() 로 핀해 둔 페이지는 장치가 언제든 쓸 수 있는
 * 상태다. 마이그레이션 관점에서 그런 페이지는 '지금 더럽다' 고 보아야 안전하다.
 * 그래서 비트맵을 새로 만들거나 비운 직후에는 언제나 이 함수로 핀된 페이지들의
 * 비트를 다시 세워 준다.
 *
 * 동작 단계:
 *  1. pgshift = __ffs(pgsize) — pgsize 가 2 의 거듭제곱이므로 최하위 1 비트의 위치가
 *     곧 시프트 양이다. 나눗셈 대신 시프트를 쓰기 위한 변환이다.
 *  2. rb_first()/rb_next() 로 pfn_list 를 오름차순 순회한다.
 *  3. 각 vfio_pfn 에 대해 (vpfn->iova - dma->iova) >> pgshift 로 매핑 시작 기준
 *     비트 번호를 구하고 bitmap_set(..., 1) 로 한 비트만 세운다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_dma_bitmap_alloc_all()(추적 시작 직후),
 *   vfio_iova_dirty_bitmap()(사용자에게 비트맵을 넘기고 비운 직후).
 * callee: __ffs(), rb_first(), rb_next(), rb_entry(), bitmap_set().
 * 에러 경로: 없다. dma->bitmap 이 NULL 이면 안 되는데, 두 호출자 모두 비트맵이
 *   존재하는 상태에서만 부른다.
 *
 * 호출 체인:
 *   vfio_iova_dirty_bitmap() → [vfio_dma_populate_bitmap] → bitmap_set()
 */
static void vfio_dma_populate_bitmap(struct vfio_dma *dma, size_t pgsize)
{
	struct rb_node *p;
	/* [한국어] 비트 하나가 뜻하는 크기의 시프트 값. pgsize 가 2 의 거듭제곱이라 나눗셈 대신 쓸 수 있다. */
	unsigned long pgshift = __ffs(pgsize);

	/* [한국어] 이 매핑의 외부 핀 기록을 오름차순으로 훑는다. */
	for (p = rb_first(&dma->pfn_list); p; p = rb_next(p)) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		struct vfio_pfn *vpfn = rb_entry(p, struct vfio_pfn, node);

		/* [한국어] 매핑 시작 기준 비트 번호를 구해 그 한 비트만 세운다. */
		bitmap_set(dma->bitmap, (vpfn->iova - dma->iova) >> pgshift, 1);
	}
}

/* [한국어]
 * vfio_iommu_populate_bitmap_full - 컨테이너의 모든 매핑을 통째로 dirty 로 칠한다
 *
 * @iommu: 컨테이너 객체.
 * @return: 없음.
 *
 * 언제 쓰이는가: dirty 추적 능력이 없는 IOMMU group 이 detach 되어
 * iommu->num_non_pinned_groups 가 줄어드는 순간이다. 그 group 이 붙어 있던 동안에는
 * 장치가 어디에 썼는지 알 수 없었으므로, 추적 정밀도가 올라가기 직전에 '그동안의
 * 모든 것은 더럽다' 고 한 번 칠해 두어야 마이그레이션이 데이터를 잃지 않는다.
 *
 * 동작 단계:
 *  1. pgshift 를 컨테이너의 최소 지원 페이지 크기에서 뽑는다.
 *  2. dma_list 트리 전체를 순회하며 각 매핑의 비트맵에 0 번 비트부터
 *     dma->size >> pgshift 개(= 매핑이 담는 페이지 수)만큼 전부 1 을 채운다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_detach_group() 의 detach_group_done 뒤처리 한 곳뿐이며,
 *   dirty_page_tracking 이 켜져 있을 때만 불린다.
 * callee: __ffs(), rb_first(), rb_next(), rb_entry(), bitmap_set().
 * 에러 경로: 없다. 추적이 켜져 있다는 것은 모든 매핑에 비트맵이 있다는 뜻이므로
 *   dma->bitmap 은 NULL 이 아니다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_detach_group() → [vfio_iommu_populate_bitmap_full] → bitmap_set()
 */
static void vfio_iommu_populate_bitmap_full(struct vfio_iommu *iommu)
{
	struct rb_node *n;
	/* [한국어] 비트 하나가 뜻하는 크기의 시프트 값. */
	unsigned long pgshift = __ffs(iommu->pgsize_bitmap);

	/* [한국어] 모든 매핑을 훑으며 */
	for (n = rb_first(&iommu->dma_list); n; n = rb_next(n)) {
		/* [한국어] 노드에서 구조체를 복원해 */
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);

		/* [한국어] 0 번 비트부터 매핑이 담는 페이지 수만큼 전부 1 로 채운다. */
		bitmap_set(dma->bitmap, 0, dma->size >> pgshift);
	}
}

/* [한국어]
 * vfio_dma_bitmap_alloc_all - 컨테이너의 모든 매핑에 dirty 비트맵을 만든다
 *
 * @iommu: 컨테이너 객체.
 * @pgsize: 비트 하나가 뜻하는 바이트 크기(최소 지원 IOMMU 페이지 크기).
 * @return: 0 성공, 실패 시 vfio_dma_bitmap_alloc() 이 준 음수 오류.
 *
 * VFIO_IOMMU_DIRTY_PAGES 의 START 요청을 받았을 때 추적을 시작하기 위한 준비 작업이다.
 * '전부 성공하거나 전부 없던 일이 되거나' 여야 하므로, 중간에 실패하면 그때까지
 * 만든 비트맵을 되감아 해제한다.
 *
 * 동작 단계:
 *  1. dma_list 를 오름차순으로 순회한다.
 *  2. 각 매핑에 vfio_dma_bitmap_alloc() 으로 비트맵을 만든다.
 *  3. 실패하면 rb_prev() 로 지금 노드의 앞쪽 노드들을 역순으로 훑으며
 *     vfio_dma_bitmap_free() 로 되감고 오류를 반환한다. 실패한 노드 자신은
 *     비트맵이 없으니 되감을 것이 없다.
 *  4. 성공하면 곧바로 vfio_dma_populate_bitmap() 으로 이미 핀된 페이지의 비트를 세운다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 안에서 GFP_KERNEL 할당을
 * 하므로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_dirty_pages() 의 START 분기 한 곳뿐.
 * callee: vfio_dma_bitmap_alloc(), vfio_dma_bitmap_free(), vfio_dma_populate_bitmap().
 * 에러 경로: 되감기 후 오류를 그대로 올려보내고, 호출자는 dirty_page_tracking 을
 *   켜지 않은 채 사용자에게 오류를 반환한다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_ioctl() → vfio_iommu_type1_dirty_pages() →
 *   [vfio_dma_bitmap_alloc_all] → vfio_dma_bitmap_alloc()
 */
static int vfio_dma_bitmap_alloc_all(struct vfio_iommu *iommu, size_t pgsize)
{
	struct rb_node *n;

	/* [한국어] 모든 매핑을 오름차순으로 훑는다. */
	for (n = rb_first(&iommu->dma_list); n; n = rb_next(n)) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);
		/* [한국어] 할당 결과. */
		int ret;

		/* [한국어] 이 매핑의 비트맵을 만든다. */
		ret = vfio_dma_bitmap_alloc(dma, pgsize);
		/* [한국어] 실패하면 그때까지 만든 것을 되감아야 한다. */
		if (ret) {
			/* [한국어] 역순 훑기용 커서. */
			struct rb_node *p;

			/* [한국어] 지금 노드의 앞쪽 노드들을 역순으로 훑으며 되감는다. 실패한 노드 자신은 비트맵이 없어 되감을 것이 없다. */
			for (p = rb_prev(n); p; p = rb_prev(p)) {
				/* [한국어] 노드에서 구조체를 복원한다. */
				struct vfio_dma *dma = rb_entry(p,
							struct vfio_dma, node);

				/* [한국어] 되감기 — 앞서 만든 비트맵을 해제한다. */
				vfio_dma_bitmap_free(dma);
			}
			return ret;
		}
		/* [한국어] 만들자마자 이미 핀된 페이지의 비트를 세워 둔다 — 그 페이지들은 언제든 더러워질 수 있다. */
		vfio_dma_populate_bitmap(dma, pgsize);
	}
	return 0;
}

/* [한국어]
 * vfio_dma_bitmap_free_all - 컨테이너의 모든 매핑의 dirty 비트맵을 해제한다
 *
 * @iommu: 컨테이너 객체.
 * @return: 없음.
 *
 * VFIO_IOMMU_DIRTY_PAGES 의 STOP 요청 처리. 추적을 끄면 비트맵은 더 이상 갱신되지
 * 않으므로 곧바로 메모리를 돌려준다.
 *
 * 동작 단계: dma_list 전체를 순회하며 vfio_dma_bitmap_free() 를 부른다.
 * 각 호출이 포인터를 NULL 로 만들어 두므로 이후 실수로 접근해도 즉시 드러난다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_dirty_pages() 의 STOP 분기 한 곳뿐.
 * callee: rb_first(), rb_next(), rb_entry(), vfio_dma_bitmap_free().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_dirty_pages() → [vfio_dma_bitmap_free_all] →
 *   vfio_dma_bitmap_free()
 */
static void vfio_dma_bitmap_free_all(struct vfio_iommu *iommu)
{
	struct rb_node *n;

	/* [한국어] 모든 매핑을 훑으며 */
	for (n = rb_first(&iommu->dma_list); n; n = rb_next(n)) {
		/* [한국어] 노드에서 매핑 구조체를 복원한다. */
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);

		/* [한국어] 이 매핑의 dirty 비트맵을 해제하고 포인터를 NULL 로 만든다. */
		vfio_dma_bitmap_free(dma);
	}
}

/*
 * Helper Functions for host iova-pfn list
 */

/* [한국어]
 * vfio_find_vpfn_range - [iova_start, iova_end) 와 겹치는 vfio_pfn 하나를 찾는다
 *
 * @dma: 대상 매핑. dma->pfn_list rb-tree 를 탐색한다.
 * @iova_start: 범위의 시작(포함).
 * @iova_end: 범위의 끝(배타적). 이 함수만 배타적 끝을 쓴다는 점에 주의한다.
 * @return: 범위 안에 있는 vfio_pfn 하나, 없으면 NULL.
 *
 * 상류 영어 주석은 '범위와 겹치는 가장 높은 vfio_pfn' 이라고 적고 있으나, 실제
 * 코드는 이진 탐색 도중 처음 만나는 겹치는 노드에서 곧바로 반환하므로 '어느 하나'
 * 가 정확한 표현이다. 실제로 호출자 vpfn_pages() 는 반환된 노드에서 rb_prev()/
 * rb_next() 로 양쪽을 모두 훑기 때문에 어느 노드가 오든 결과가 같다.
 *
 * 동작 단계:
 *  1. 루트부터 내려간다.
 *  2. 범위의 배타적 끝이 노드 iova 이하면 노드가 완전히 오른쪽이므로 rb_left.
 *  3. 범위의 시작이 노드 iova 보다 크면 노드가 완전히 왼쪽이므로 rb_right.
 *  4. 둘 다 아니면 iova_start <= vpfn->iova < iova_end 이므로 그 노드를 반환.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 순수 탐색.
 *
 * caller: vfio_find_vpfn()(한 페이지짜리 조회), vpfn_pages()(범위 개수 세기).
 * callee: rb_entry().
 * 에러 경로: 없다. NULL 은 '그 범위에 외부 핀이 없다' 는 정상 결과다.
 *
 * 호출 체인:
 *   vpfn_pages() → [vfio_find_vpfn_range] → rb_entry()
 */
/*
 * Find the highest vfio_pfn that overlapping the range
 * [iova_start, iova_end) in rb tree.
 */
static struct vfio_pfn *vfio_find_vpfn_range(struct vfio_dma *dma,
		dma_addr_t iova_start, dma_addr_t iova_end)
{
	struct vfio_pfn *vpfn;
	/* [한국어] 이 매핑의 외부 핀 트리 루트. */
	struct rb_node *node = dma->pfn_list.rb_node;

	/* [한국어] 잎에 닿을 때까지 내려간다. */
	while (node) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		vpfn = rb_entry(node, struct vfio_pfn, node);

		/* [한국어] 범위의 배타적 끝이 노드 이하면 노드가 완전히 오른쪽이다. */
		if (iova_end <= vpfn->iova)
			/* [한국어] 왼쪽으로 내려간다. */
			node = node->rb_left;
		/* [한국어] 범위 시작이 노드보다 크면 노드가 완전히 왼쪽이다. */
		else if (iova_start > vpfn->iova)
			/* [한국어] 오른쪽으로 내려간다. */
			node = node->rb_right;
		/* [한국어] 둘 다 아니면 범위 안이다. */
		else
			/* [한국어] 둘 다 아니면 iova_start <= vpfn->iova < iova_end 이므로 범위 안이다. */
			return vpfn;
	}
	/* [한국어] 잎까지 내려갔으면 그 범위에 외부 핀이 없다. */
	return NULL;
}

/* [한국어]
 * vfio_find_vpfn - 특정 IOVA 한 페이지에 대한 vfio_pfn 을 찾는다
 *
 * @dma: 대상 매핑.
 * @iova: 찾을 페이지의 IOVA.
 * @return: 해당 vfio_pfn, 없으면 NULL.
 *
 * vfio_find_vpfn_range() 를 [iova, iova+1) 로 부르는 얇은 래퍼다. 폭 1 바이트짜리
 * 범위를 주면 vpfn->iova == iova 인 노드만 걸리므로 정확히 한 페이지 조회가 된다.
 * inline 으로 선언되어 호출 비용이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iova_get_vfio_pfn()(핀 참조 증가), vfio_unpin_page_external()(참조 감소).
 * callee: vfio_find_vpfn_range().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_pin_pages() → vfio_iova_get_vfio_pfn() → [vfio_find_vpfn] →
 *   vfio_find_vpfn_range()
 */
static inline struct vfio_pfn *vfio_find_vpfn(struct vfio_dma *dma, dma_addr_t iova)
{
	return vfio_find_vpfn_range(dma, iova, iova + 1);
}

/* [한국어]
 * vfio_link_pfn - 새 vfio_pfn 을 매핑의 pfn rb-tree 에 끼워 넣는다
 *
 * @dma: 대상 매핑. dma->pfn_list 가 트리 루트다.
 * @new: 삽입할 노드. new->iova 가 정렬 키다.
 * @return: 없음.
 *
 * 이 트리의 노드는 폭이 정확히 한 페이지이므로 vfio_link_dma() 와 달리 크기 0
 * 제약이 없다. 같은 iova 로 두 번 삽입되는 일은 호출자가 미리 vfio_find_vpfn() 으로
 * 막는다 — 이미 있으면 ref_count 만 올린다.
 *
 * 동작 단계:
 *  1. 루트 포인터의 주소부터 시작해 잎에 닿을 때까지 iova 비교로 내려간다.
 *  2. rb_link_node() 로 부모/링크 위치를 확정하고 rb_insert_color() 로 재균형한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_add_to_pfn_list() 한 곳뿐.
 * callee: rb_entry(), rb_link_node(), rb_insert_color().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_pin_pages() → vfio_add_to_pfn_list() → [vfio_link_pfn] →
 *   rb_insert_color()
 */
static void vfio_link_pfn(struct vfio_dma *dma,
			  struct vfio_pfn *new)
{
	struct rb_node **link, *parent = NULL;
	/* [한국어] 비교 대상 노드. */
	struct vfio_pfn *vpfn;

	/* [한국어] 루트 포인터의 주소부터 시작한다. */
	link = &dma->pfn_list.rb_node;
	/* [한국어] 잎에 닿을 때까지 내려간다. */
	while (*link) {
		/* [한국어] 현재 자리가 새 노드의 부모 후보다. */
		parent = *link;
		/* [한국어] 노드에서 구조체를 복원한다. */
		vpfn = rb_entry(parent, struct vfio_pfn, node);

		/* [한국어] IOVA 가 작으면 왼쪽, */
		if (new->iova < vpfn->iova)
			/* [한국어] 왼쪽 가지 포인터를 따라간다. */
			link = &(*link)->rb_left;
		else
			/* [한국어] 크거나 같으면 오른쪽 가지로 내려간다. */
			link = &(*link)->rb_right;
	}

	/* [한국어] 부모와 연결 위치를 확정한다. 아래 rb_insert_color 와 반드시 짝으로 불러야 한다. */
	rb_link_node(&new->node, parent, link);
	/* [한국어] red-black 트리의 색 규칙에 맞게 재균형한다. 위 rb_link_node 와 짝이다. */
	rb_insert_color(&new->node, &dma->pfn_list);
}

/* [한국어]
 * vfio_unlink_pfn - vfio_pfn 을 매핑의 pfn rb-tree 에서 뺀다
 *
 * @dma: 대상 매핑.
 * @old: 뺄 노드.
 * @return: 없음.
 *
 * vfio_link_pfn() 의 짝. 트리에서 빼기만 하고 메모리 해제는 호출자
 * vfio_remove_from_pfn_list() 가 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_remove_from_pfn_list() 한 곳뿐.
 * callee: rb_erase().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iova_put_vfio_pfn() → vfio_remove_from_pfn_list() → [vfio_unlink_pfn] →
 *   rb_erase()
 */
static void vfio_unlink_pfn(struct vfio_dma *dma, struct vfio_pfn *old)
{
	rb_erase(&old->node, &dma->pfn_list);
}

/* [한국어]
 * vfio_add_to_pfn_list - 외부에서 핀한 페이지 하나를 매핑의 pfn 트리에 기록한다
 *
 * @dma: 대상 매핑.
 * @iova: 그 페이지의 IOVA.
 * @pfn: 그 페이지의 호스트 물리 페이지 프레임 번호.
 * @return: 0 성공, -ENOMEM(노드 할당 실패).
 *
 * mdev 가 vfio_pin_pages() 로 페이지를 핀하면, 그 사실을 여기 기록해 두어야
 * (1) 같은 페이지를 또 핀할 때 참조만 올리고, (2) 나중에 같은 범위를 IOMMU 매핑용
 * 으로 핀할 때 이중 과금하지 않으며, (3) 매핑을 지우기 전에 아직 외부 핀이 남아
 * 있는지 알 수 있다.
 *
 * 동작 단계:
 *  1. kzalloc_obj(*vpfn) 로 vfio_pfn 하나를 0 초기화 할당한다.
 *  2. iova/pfn 을 채우고 ref_count 를 1 로 둔다 — 이 최초 참조가 곧 실제 페이지 핀
 *     한 장에 대응한다.
 *  3. vfio_link_pfn() 으로 트리에 넣는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_pin_pages() 한 곳뿐.
 * callee: kzalloc_obj(), vfio_link_pfn().
 * 에러 경로: -ENOMEM 이면 호출자가 방금 핀한 페이지를 put_pfn() 으로 되돌리고,
 *   과금까지 했다면 vfio_lock_acct(dma, -1, true) 로 과금도 되돌린 뒤 pin_unwind 로
 *   간다 — 핀 장부와 과금 장부를 둘 다 되감는 지점이다.
 *
 * 호출 체인:
 *   vfio_pin_pages() → vfio_iommu_type1_pin_pages() → [vfio_add_to_pfn_list] →
 *   vfio_link_pfn()
 */
static int vfio_add_to_pfn_list(struct vfio_dma *dma, dma_addr_t iova,
				unsigned long pfn)
{
	struct vfio_pfn *vpfn;

	/* [한국어] 기록 노드를 0 초기화 할당한다. */
	vpfn = kzalloc_obj(*vpfn);
	/* [한국어] 할당 실패. */
	if (!vpfn)
		return -ENOMEM;

	/* [한국어] 이 페이지가 놓인 IOVA — 트리의 정렬 키다. */
	vpfn->iova = iova;
	/* [한국어] 호스트 물리 페이지 프레임 번호. */
	vpfn->pfn = pfn;
	/* [한국어] 최초 참조 1 — 이것이 실제 페이지 핀 한 장에 대응한다. */
	vpfn->ref_count = 1;
	/* [한국어] 트리에 끼워 넣는다. */
	vfio_link_pfn(dma, vpfn);
	return 0;
}

/* [한국어]
 * vfio_remove_from_pfn_list - vfio_pfn 노드를 트리에서 빼고 해제한다
 *
 * @dma: 대상 매핑.
 * @vpfn: 없앨 노드. 이 시점에 ref_count 는 0 이다.
 * @return: 없음.
 *
 * vfio_add_to_pfn_list() 의 짝. 트리에서 빼는 것과 메모리를 돌려주는 것을 한 곳에
 * 묶어, 둘 중 하나만 하는 실수를 막는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iova_put_vfio_pfn() 한 곳뿐.
 * callee: vfio_unlink_pfn(), kfree().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_unpin_page_external() → vfio_iova_put_vfio_pfn() →
 *   [vfio_remove_from_pfn_list] → kfree()
 */
static void vfio_remove_from_pfn_list(struct vfio_dma *dma,
				      struct vfio_pfn *vpfn)
{
	vfio_unlink_pfn(dma, vpfn);
	/* [한국어] 기록 노드를 해제한다. */
	kfree(vpfn);
}

/* [한국어]
 * vfio_iova_get_vfio_pfn - 이미 기록된 외부 핀에 참조를 하나 더 얹는다
 *
 * @dma: 대상 매핑.
 * @iova: 조회할 페이지의 IOVA.
 * @return: 참조를 올린 vfio_pfn, 그 IOVA 에 기록이 없으면 NULL.
 *
 * mdev 가 같은 페이지를 여러 번 핀할 수 있으므로 참조 계수가 필요하다. 이미 있으면
 * 실제 GUP 를 다시 부르지 않고 계수만 올린다 — 그래야 핀 장부(페이지의 pin 카운트)
 * 와 이 트리의 ref_count 가 어긋나지 않는다.
 *
 * 동작 단계:
 *  1. vfio_find_vpfn() 으로 조회한다.
 *  2. 있으면 ref_count 를 1 올린다. iommu->lock 아래이므로 원자 연산이 필요 없다.
 *  3. 결과를 그대로 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_pin_pages() 한 곳뿐 — NULL 이면 새로 핀하고,
 *   NULL 이 아니면 그 페이지를 그대로 사용자에게 돌려준다.
 * callee: vfio_find_vpfn().
 * 에러 경로: 없다. NULL 은 '아직 핀되지 않았다' 는 정상 결과다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_pin_pages() → [vfio_iova_get_vfio_pfn] → vfio_find_vpfn()
 */
static struct vfio_pfn *vfio_iova_get_vfio_pfn(struct vfio_dma *dma,
					       unsigned long iova)
{
	struct vfio_pfn *vpfn = vfio_find_vpfn(dma, iova);

	/* [한국어] 이미 기록이 있으면 */
	if (vpfn)
		/* [한국어] 참조만 올린다 — GUP 를 다시 부르지 않아야 핀 카운트와 ref_count 가 어긋나지 않는다. */
		vpfn->ref_count++;
	/* [한국어] 찾은 기록을 돌려준다. NULL 이면 아직 핀되지 않았다는 뜻이다. */
	return vpfn;
}

/* [한국어]
 * vfio_iova_put_vfio_pfn - 외부 핀 참조를 하나 놓고, 0 이 되면 실제 핀을 푼다
 *
 * @dma: 대상 매핑. dma->prot 로 페이지를 dirty 로 표시할지 결정한다.
 * @vpfn: 참조를 놓을 노드.
 * @return: 실제로 언핀되어 잠금 메모리 회계에서 빼야 할 페이지 수(0 또는 1).
 *
 * 핀 장부와 과금 장부를 잇는 지점이다. 참조가 남아 있으면 아무 일도 일어나지 않고
 * 0 을 돌려주며, 마지막 참조가 사라질 때만 put_pfn() 으로 실제 핀을 풀고 그 반환값
 * (예약 pfn 이면 0, 일반 페이지면 1)을 그대로 올려보낸다. 호출자는 그 값을 그대로
 * 과금 반환량으로 쓴다 — 예약 pfn 은 애초에 과금하지 않았으므로 되돌릴 것도 없다.
 *
 * 동작 단계:
 *  1. ref_count 를 1 내린다.
 *  2. 0 이 아니면 아직 다른 사용자가 있으므로 0 을 반환한다.
 *  3. 0 이면 put_pfn() 으로 페이지 핀을 풀고, vfio_remove_from_pfn_list() 로
 *     노드를 트리에서 빼고 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_unpin_page_external() 한 곳뿐.
 * callee: put_pfn(), vfio_remove_from_pfn_list().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_unpin_pages() → vfio_iommu_type1_unpin_pages() →
 *   vfio_unpin_page_external() → [vfio_iova_put_vfio_pfn] → put_pfn()
 */
static int vfio_iova_put_vfio_pfn(struct vfio_dma *dma, struct vfio_pfn *vpfn)
{
	int ret = 0;

	/* [한국어] 참조를 하나 내린다. iommu->lock 이 감싸므로 원자 연산이 필요 없다. */
	vpfn->ref_count--;
	/* [한국어] 마지막 참조였으면 */
	if (!vpfn->ref_count) {
		/* [한국어] 실제 핀을 푼다. 반환값(0 또는 1)이 곧 과금에서 뺄 양이다. */
		ret = put_pfn(vpfn->pfn, dma->prot);
		/* [한국어] 기록을 트리에서 빼고 해제한다. */
		vfio_remove_from_pfn_list(dma, vpfn);
	}
	return ret;
}

/* [한국어]
 * mm_lock_acct - 한 mm 의 잠금 메모리 회계를 mmap 쓰기 락 아래에서 갱신한다
 *
 * @task: 회계 주체가 될 task. RLIMIT_MEMLOCK 한도를 이 task 에서 읽는다.
 * @mm: 대상 주소 공간. 잠금 페이지 수가 여기에 누적된다.
 * @lock_cap: 이 주체가 CAP_IPC_LOCK 을 가졌는지. true 면 한도 검사를 통과시킨다.
 * @npage: 더할 페이지 수. 음수면 빼는 것이다.
 * @return: 0 성공, -EINTR 계열(치명 시그널로 락 대기가 깨짐), 또는
 *          -ENOMEM(한도 초과).
 *
 * 이 파일에서 잠금 메모리 회계를 실제로 만지는 유일한 지점이다. mm 코어의 회계
 * 함수는 mmap 쓰기 락 아래에서 불러야 하므로, 그 락 획득/해제를 여기서 감싼다.
 *
 * 동작 단계:
 *  1. mmap_write_lock_killable() 로 mm 의 mmap 락을 쓰기 모드로 잡는다. killable 을
 *     쓰는 이유는 다른 스레드가 오래 잡고 있을 때 프로세스 종료가 막히지 않게
 *     하기 위해서다. 실패하면 회계를 건드리지 않고 그대로 반환한다.
 *  2. __account_locked_vm(mm, abs(npage), npage > 0, task, lock_cap) 을 부른다.
 *     부호를 abs() 로 떼고 방향을 세 번째 인자(true=증가)로 따로 넘기는 것이
 *     그 API 의 규약이다. 증가 방향일 때만 한도 검사가 일어난다.
 *  3. 락을 놓고 결과를 반환한다.
 * mm 쪽 함수의 내부 구현은 이 트리에 mm/ 이 없어 확인 못 함 — 여기서는 '무엇을
 * 요구하고 반환값을 어떻게 해석하는가' 만 기술한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. iommu->lock 을 쥔 채로 불리며, 안에서 mmap 쓰기
 * 락을 잡으므로 잠들 수 있다. 인터럽트 문맥에서는 절대 불리지 않는다.
 *
 * caller: vfio_lock_acct()(일반 경로), vfio_change_dma_owner()(소유자 이전 시
 *   새 mm 에 더하고 옛 mm 에서 빼는 두 번).
 * callee: mmap_write_lock_killable(), __account_locked_vm(), mmap_write_unlock().
 * 에러 경로: 실패하면 회계가 전혀 바뀌지 않는다. 호출자는 그 오류를 그대로
 *   사용자에게 올려보내거나, 핀을 되돌린다.
 *
 * 호출 체인:
 *   vfio_pin_pages_remote() → vfio_lock_acct() → [mm_lock_acct] →
 *   __account_locked_vm()
 */
static int mm_lock_acct(struct task_struct *task, struct mm_struct *mm,
			bool lock_cap, long npage)
{
	int ret = mmap_write_lock_killable(mm);

	/* [한국어] 치명 시그널로 깨어났으면 회계를 건드리지 않고 그대로 반환한다. */
	if (ret)
		return ret;

	/* [한국어] mm 코어의 잠금 메모리 회계를 갱신한다. 부호를 abs 로 떼고 방향을 세 번째
	 * 인자로 따로 넘기는 것이 그 API 의 규약이며, 증가 방향일 때만 RLIMIT_MEMLOCK
	 * 한도 검사가 일어난다. 내부 구현은 mm/ 이 없는 이 트리에서 확인 못 함. */
	ret = __account_locked_vm(mm, abs(npage), npage > 0, task, lock_cap);
	/* [한국어] 회계를 마쳤으니 mmap 쓰기 락을 놓는다. */
	mmap_write_unlock(mm);
	return ret;
}

/* [한국어]
 * vfio_lock_acct - 매핑 단위 잠금 메모리 회계를 갱신한다
 *
 * @dma: 대상 매핑. 회계 주체(task/mm/lock_cap)와 누계(locked_vm)를 여기서 가져온다.
 * @npage: 더할 페이지 수. 음수면 언핀에 따른 반환이다.
 * @async: 매핑을 만든 프로세스가 아닌 문맥에서 부르는가. true 면 mm 이 이미
 *         해체되었을 수 있으므로 mmget_not_zero() 로 살아 있는지 확인한다.
 * @return: 0 성공, -ESRCH(프로세스가 이미 종료됨), 그 밖에 mm_lock_acct() 의 오류.
 *
 * 이 파일에서 dma->locked_vm 을 갱신하는 유일한 함수다. '핀한 페이지 수' 와
 * '과금한 페이지 수' 가 언제나 같아야 하므로, 핀/언핀 코드는 반드시 같은 수로
 * 이 함수를 불러 주어야 한다. 여기서 빠뜨리면 프로세스가 죽은 뒤에도
 * RLIMIT_MEMLOCK 한도가 소모된 채 남는다.
 *
 * 동작 단계:
 *  1. npage 가 0 이면 할 일이 없으므로 곧바로 0 을 반환한다 — 불필요한 mmap 락
 *     획득을 피하는 빠른 경로다.
 *  2. async 면 mmget_not_zero() 로 mm 사용자 참조를 승격한다. dma->mm 은 mmgrab()
 *     으로 구조체만 붙잡아 둔 것이라, 주소 공간이 이미 해체되었으면 이 승격이
 *     실패한다. 그때는 -ESRCH 로 '프로세스가 이미 끝났다' 를 알린다.
 *  3. mm_lock_acct() 로 실제 회계를 갱신한다.
 *  4. 성공했을 때만 dma->locked_vm 에 npage 를 누적한다. 실패했는데 누계를 올리면
 *     나중에 소유자를 옮길 때 있지도 않은 양을 빼게 되므로 순서가 중요하다.
 *  5. async 였으면 mmput() 으로 승격했던 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: 핀/언핀을 하는 거의 모든 곳 — vfio_pin_pages_remote(),
 *   vfio_unpin_pages_remote(), vfio_pin_page_external(), vfio_unpin_page_external(),
 *   vfio_unmap_unpin(), vfio_iommu_unmap_unpin_reaccount(),
 *   vfio_iommu_type1_pin_pages() 의 되감기.
 * callee: mmget_not_zero(), mm_lock_acct(), mmput().
 * 에러 경로: 언핀 경로에서 실패하면 반환값을 무시하는 호출자가 대부분이다 —
 *   이미 페이지는 풀었으므로 되돌릴 수 없고, 남은 것은 과금 누계뿐이기 때문이다.
 *   핀 경로에서 실패하면 호출자가 방금 핀한 페이지들을 되돌린다.
 *
 * 호출 체인:
 *   vfio_dma_do_map() → vfio_pin_map_dma() → vfio_pin_pages_remote() →
 *   [vfio_lock_acct] → mm_lock_acct()
 */
static int vfio_lock_acct(struct vfio_dma *dma, long npage, bool async)
{
	struct mm_struct *mm;
	/* [한국어] 회계 결과. */
	int ret;

	/* [한국어] 0 이면 할 일이 없다. 불필요한 mmap 쓰기 락 획득을 피하는 빠른 경로다. */
	if (!npage)
		return 0;

	/* [한국어] 이 매핑의 소유 주소 공간. */
	mm = dma->mm;
	/* [한국어] 다른 문맥에서 부르는 경우, 주소 공간이 아직 살아 있는지 확인한다. */
	if (async && !mmget_not_zero(mm))
		/* [한국어] 이미 해체되었으면 회계 대상이 없다. */
		return -ESRCH; /* process exited */

	/* [한국어] 실제 회계 갱신. 한도 검사는 증가 방향일 때만 일어난다. */
	ret = mm_lock_acct(dma->task, mm, dma->lock_cap, npage);
	/* [한국어] 성공했을 때만 */
	if (!ret)
		/* [한국어] 누계를 갱신한다. 실패했는데 올리면 나중에 소유자를 옮길 때 없는 양을 빼게 된다. */
		dma->locked_vm += npage;

	/* [한국어] 승격했던 참조를 놓는다. */
	if (async)
		mmput(mm);

	return ret;
}

/* [한국어]
 * is_invalid_reserved_pfn - 이 pfn 이 struct page 로 관리되지 않는 메모리인지 판정한다
 *
 * @pfn: 검사할 호스트 물리 페이지 프레임 번호.
 * @return: true 면 예약/무효 pfn(핀도 과금도 하지 않는다), false 면 일반 페이지.
 *
 * 왜 필요한가: 사용자가 매핑하려는 구간이 다른 장치의 BAR 를 mmap 한 MMIO 영역일
 * 수 있다. 그런 메모리는 struct page 가 없거나 예약 표시가 되어 있어 핀할 대상이
 * 아니고, 프로세스의 잠금 메모리로 세어서도 안 된다. 이 판정 하나가 이 파일의
 * 핀/과금 경로 전체에서 '건너뛸지' 를 가르는 갈림길이다.
 *
 * 동작 단계:
 *  1. pfn_valid() 가 거짓이면 애초에 struct page 가 없는 주소이므로 true.
 *  2. 유효하면 pfn_to_page() 로 struct page 를 얻어 PageReserved() 를 본다.
 * 상류 영어 주석대로, 복합 페이지(huge page)에서는 head 페이지에 예약 비트를 세운
 * 드라이버가 모든 sub page 에도 세워 주어야 이 판정이 일관된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 요구하지 않으며 잠들지 않는다.
 *
 * caller: put_pfn(), vaddr_get_pfns(), vfio_pin_pages_remote(),
 *   vfio_pin_page_external(), vfio_iommu_unmap_unpin_reaccount().
 * callee: pfn_valid(), pfn_to_page(), PageReserved(). 이들의 구현은 mm/ 이 없는
 *   이 트리에서 확인 못 함.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pin_pages_remote() → [is_invalid_reserved_pfn] → pfn_valid()
 */
/*
 * Some mappings aren't backed by a struct page, for example an mmap'd
 * MMIO range for our own or another device.  These use a different
 * pfn conversion and shouldn't be tracked as locked pages.
 * For compound pages, any driver that sets the reserved bit in head
 * page needs to set the reserved bit in all subpages to be safe.
 */
static bool is_invalid_reserved_pfn(unsigned long pfn)
{
	if (pfn_valid(pfn))
		/* [한국어] struct page 가 있으면 예약 비트를 본다. 예약 페이지는 커널이 특별 용도로 잡아 둔 것이라 핀 대상이 아니다. */
		return PageReserved(pfn_to_page(pfn));

	return true;
}

/* [한국어]
 * put_pfn - 페이지 하나의 핀을 푼다(예약 pfn 이면 아무 일도 하지 않는다)
 *
 * @pfn: 놓을 호스트 물리 페이지 프레임 번호.
 * @prot: 이 매핑의 접근 권한. IOMMU_WRITE 가 서 있으면 장치가 이 페이지에 썼을 수
 *        있으므로 dirty 로 표시해야 한다.
 * @return: 실제로 언핀한 페이지 수 — 1(일반 페이지) 또는 0(예약 pfn).
 *
 * 핀 장부를 내리는 두 지점 중 하나다(다른 하나는 구간 단위인
 * put_valid_unreserved_pfns()). 반환값이 곧 '과금에서 빼야 할 양' 이므로,
 * 호출자는 이 값을 그대로 vfio_lock_acct() 에 넘긴다. 예약 pfn 은 핀도 과금도 하지
 * 않았으므로 0 을 돌려 두 장부의 균형을 유지한다.
 *
 * 동작 단계:
 *  1. is_invalid_reserved_pfn() 이 거짓(=일반 페이지)일 때만 실제 작업을 한다.
 *  2. pfn_to_page() 로 struct page 를 얻는다.
 *  3. unpin_user_pages_dirty_lock(&page, 1, prot & IOMMU_WRITE) 로 핀을 푼다.
 *     세 번째 인자가 참이면 페이지를 dirty 로 표시해 파일 백업 페이지의 내용이
 *     디스크로 반영되게 한다. 쓰기 가능하게 매핑한 페이지는 장치가 언제 썼는지 알
 *     수 없으므로 무조건 dirty 로 본다.
 *  4. 1 을 반환한다. 예약 pfn 이면 아무것도 하지 않고 0 을 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. iommu->lock 아래에서 불린다. 내부에서 mm 락을
 * 잡을 수 있어 잠들 수 있다.
 *
 * caller: vfio_iova_put_vfio_pfn(), vfio_batch_unpin(), vfio_pin_pages_remote() 의
 *   되감기, vfio_unpin_pages_remote() 의 예약 pfn 섞인 경로,
 *   vfio_pin_page_external() 의 과금 실패 경로, vfio_iommu_type1_pin_pages() 의
 *   pfn 목록 등록 실패 경로.
 * callee: is_invalid_reserved_pfn(), pfn_to_page(), unpin_user_pages_dirty_lock().
 *   마지막 함수의 내부는 mm/ 이 없는 이 트리에서 확인 못 함.
 * 에러 경로: 없다. 실패할 수 없는 연산이다.
 *
 * 호출 체인:
 *   vfio_unpin_pages_remote() → [put_pfn] → unpin_user_pages_dirty_lock()
 */
static int put_pfn(unsigned long pfn, int prot)
{
	if (!is_invalid_reserved_pfn(pfn)) {
		/* [한국어] pfn 에서 struct page 를 얻는다. 예약 pfn 이 아님이 확인된 뒤이므로 안전하다. */
		struct page *page = pfn_to_page(pfn);

		/* [한국어] 핀을 푼다. 세 번째 인자가 참이면 페이지를 dirty 로 표시한다 —
		 * 장치가 쓸 수 있게 매핑했다면 실제로 썼는지 알 수 없으므로 무조건 더럽다고 본다. */
		unpin_user_pages_dirty_lock(&page, 1, prot & IOMMU_WRITE);
		/* [한국어] 실제로 한 장을 풀었음을 알린다. 호출자는 이 값을 그대로 과금 반환량으로 쓴다. */
		return 1;
	}
	return 0;
}

#define VFIO_BATCH_MAX_CAPACITY (PAGE_SIZE / sizeof(struct page *))

/* [한국어]
 * __vfio_batch_init - GUP 결과를 담을 페이지 배열을 준비한다
 *
 * @batch: 초기화할 배치 객체(대개 호출자 스택에 있다).
 * @single: true 면 한 번에 한 페이지만 다루는 모드로 만든다.
 * @return: 없음.
 *
 * 왜 필요한가: 페이지를 한 장씩 핀하면 GUP 호출 횟수가 매핑 크기에 비례해 폭증한다.
 * 한 페이지 크기의 포인터 배열을 잡아 두면 4KB 페이지 기준 512(64비트) 개를 한 번에
 * 받아 올 수 있어 호출 횟수가 그만큼 준다. 다만 이 배열 할당 자체가 실패할 수 있으므로
 * 구조체 안에 1 개짜리 예비 슬롯을 두어 어떤 경우에도 진행이 가능하게 만든다.
 *
 * 동작 단계:
 *  1. size/offset 을 0 으로 리셋한다 — 배치가 비어 있다는 뜻이다.
 *  2. single 이 참이거나 disable_hugepages 모듈 파라미터가 켜져 있으면 곧바로
 *     fallback 으로 간다. 후자는 대형 페이지 최적화를 끄기 위한 디버깅 스위치다.
 *  3. __get_free_page(GFP_KERNEL) 로 한 페이지를 잡아 포인터 배열로 쓴다.
 *     실패해도 오류를 올리지 않고 fallback 으로 간다.
 *  4. 성공하면 capacity 를 VFIO_BATCH_MAX_CAPACITY(= PAGE_SIZE / 포인터 크기)로 둔다.
 *  5. fallback 라벨: pages 를 구조체 내장 슬롯 하나로 향하게 하고 capacity 를 1 로 둔다.
 *     capacity 값이 곧 vfio_batch_fini() 가 '페이지를 해제해야 하는가' 를 판별하는
 *     표식이기도 하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. iommu->lock 을 쥔 상태에서 불리며 GFP_KERNEL
 * 할당이라 잠들 수 있다.
 *
 * caller: vfio_batch_init(), vfio_batch_init_single().
 * callee: __get_free_page().
 * 에러 경로: 할당 실패는 오류가 아니라 성능 저하로만 처리된다 — 언제나 진행 가능하다.
 *
 * 호출 체인:
 *   vfio_pin_map_dma() → vfio_batch_init() → [__vfio_batch_init] → __get_free_page()
 */
static void __vfio_batch_init(struct vfio_batch *batch, bool single)
{
	batch->size = 0;
	/* [한국어] 배치를 처음부터 소비하도록 위치를 리셋한다. */
	batch->offset = 0;

	/* [한국어] 한 장 모드이거나 대형 페이지 최적화가 꺼져 있으면 배열을 잡을 이유가 없다. */
	if (single || unlikely(disable_hugepages))
		goto fallback;

	/* [한국어] 포인터 배열로 쓸 한 페이지를 잡는다. 4KB 페이지 기준 64비트 포인터 512개가 들어간다. */
	batch->pages = (struct page **) __get_free_page(GFP_KERNEL);
	/* [한국어] 할당 실패는 오류가 아니라 성능 저하로만 처리한다. */
	if (!batch->pages)
		goto fallback;

	/* [한국어] 한 페이지에 담을 수 있는 포인터 수. 이 값이 곧 vfio_batch_fini() 의 해제 판별 표식이 된다. */
	batch->capacity = VFIO_BATCH_MAX_CAPACITY;
	return;

fallback:
	/* [한국어] 구조체 안의 1 개짜리 예비 슬롯을 쓴다 — 할당 없이도 언제나 진행 가능하게 한다. */
	batch->pages = &batch->fallback_page;
	/* [한국어] 한 번에 한 페이지만 다룬다는 표시이자, vfio_batch_fini() 의 해제 판별 표식이다. */
	batch->capacity = 1;
}

/* [한국어]
 * vfio_batch_init - 다중 페이지 모드로 배치를 초기화한다
 *
 * @batch: 초기화할 배치 객체.
 * @return: 없음.
 *
 * __vfio_batch_init(batch, false) 를 부르는 한 줄 래퍼다. 대량으로 페이지를 핀하는
 * 경로(매핑 생성, replay)에서 쓰이며, 가능하면 한 페이지짜리 포인터 배열을 잡아
 * GUP 호출을 몰아친다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_pin_map_dma(), vfio_iommu_replay().
 * callee: __vfio_batch_init().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_dma_do_map() → vfio_pin_map_dma() → [vfio_batch_init] → __vfio_batch_init()
 */
static void vfio_batch_init(struct vfio_batch *batch)
{
	__vfio_batch_init(batch, false);
}

/* [한국어]
 * vfio_batch_init_single - 한 페이지 모드로 배치를 초기화한다
 *
 * @batch: 초기화할 배치 객체.
 * @return: 없음.
 *
 * __vfio_batch_init(batch, true) 를 부르는 한 줄 래퍼다. mdev 의 페이지 단위
 * 핀 경로처럼 애초에 한 장만 다루는 곳에서 쓰며, 쓸데없이 한 페이지를 할당하지
 * 않도록 곧바로 구조체 내장 예비 슬롯을 쓰게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_pin_page_external() 한 곳뿐.
 * callee: __vfio_batch_init().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_pin_pages() → vfio_pin_page_external() →
 *   [vfio_batch_init_single] → __vfio_batch_init()
 */
static void vfio_batch_init_single(struct vfio_batch *batch)
{
	__vfio_batch_init(batch, true);
}

/* [한국어]
 * vfio_batch_unpin - 배치에 남아 소비되지 않은 페이지들의 핀을 되돌린다
 *
 * @batch: 잔여 페이지가 들어 있을 수 있는 배치.
 * @dma: 대상 매핑. dma->prot 로 dirty 표시 여부를 정한다.
 * @return: 없음.
 *
 * 핀 장부 누수를 막는 안전망이다. GUP 는 요청한 수만큼 페이지를 핀해 배치에
 * 채워 놓는데, 호출자가 그중 일부만 쓰고 오류로 빠져나가면 남은 페이지들의 핀이
 * 그대로 새어 나간다. 그런 경로마다 이 함수를 불러 배치를 비운다.
 *
 * 동작 단계:
 *  1. batch->size 가 0 이 될 때까지 반복한다.
 *  2. pages[offset] 의 pfn 을 구해 put_pfn() 으로 핀을 푼다.
 *  3. offset 을 올리고 size 를 내려 다음 항목으로 넘어간다 — 이 두 값의 합이 언제나
 *     GUP 가 채운 총 개수와 같다는 불변식이 유지된다.
 * 주의: 여기서 푸는 페이지들은 아직 과금되지 않은 것들이다. 과금은 소비 시점에
 * vfio_pin_pages_remote() 가 lock_acct 에 누적했다가 마지막에 한 번에 하므로,
 * 여기서 vfio_lock_acct() 를 부를 필요가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_pin_pages_remote() 의 unpin_out 경로, vfio_pin_map_dma() 의 매핑
 *   실패 경로, vfio_iommu_replay() 의 unwind 경로.
 * callee: page_to_pfn(), put_pfn().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pin_map_dma() → [vfio_batch_unpin] → put_pfn()
 */
static void vfio_batch_unpin(struct vfio_batch *batch, struct vfio_dma *dma)
{
	while (batch->size) {
		/* [한국어] 다음에 소비할 페이지의 pfn 을 꺼낸다. */
		unsigned long pfn = page_to_pfn(batch->pages[batch->offset]);

		/* [한국어] 핀을 푼다. 이 페이지들은 아직 과금되지 않았으므로 회계는 건드리지 않는다. */
		put_pfn(pfn, dma->prot);
		/* [한국어] 소비 위치를 전진시키고 */
		batch->offset++;
		/* [한국어] 잔여 수를 줄인다. 두 값의 합이 불변이라 정확히 한 바퀴만 돈다. */
		batch->size--;
	}
}

/* [한국어]
 * vfio_batch_fini - 배치가 잡고 있던 페이지 배열을 해제한다
 *
 * @batch: 정리할 배치.
 * @return: 없음.
 *
 * __vfio_batch_init() 의 짝이다. capacity 가 VFIO_BATCH_MAX_CAPACITY 인 경우에만
 * __get_free_page() 로 잡은 페이지를 돌려준다. fallback 모드(capacity == 1)에서는
 * pages 가 구조체 안을 가리키므로 해제하면 안 되는데, 그 구분을 별도 플래그 없이
 * capacity 값 하나로 표현한 것이다.
 *
 * 주의: 이 함수는 배열 메모리만 돌려줄 뿐 아직 남은 페이지의 핀은 풀지 않는다.
 * 잔여 핀 정리는 vfio_batch_unpin() 의 몫이므로 두 함수는 별개로 불러야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_pin_map_dma(), vfio_pin_page_external(), vfio_iommu_replay()
 *   (성공 경로와 unwind 경로 양쪽).
 * callee: free_page().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pin_map_dma() → [vfio_batch_fini] → free_page()
 */
static void vfio_batch_fini(struct vfio_batch *batch)
{
	if (batch->capacity == VFIO_BATCH_MAX_CAPACITY)
		/* [한국어] 페이지를 할당했던 경우에만 돌려준다. capacity 값이 그 판별 표식이다. */
		free_page((unsigned long)batch->pages);
}

/* [한국어]
 * follow_fault_pfn - VM_PFNMAP 구간의 pfn 을 페이지 테이블에서 직접 읽는다
 *
 * @vma: 대상 가상 메모리 구간. VM_PFNMAP 플래그가 서 있는 것이 전제다.
 * @mm: 그 구간이 속한 주소 공간.
 * @vaddr: 조회할 사용자 가상 주소.
 * @pfn: 결과 pfn 을 받을 곳.
 * @addr_mask: 이 매핑을 덮는 페이지의 크기 마스크를 받을 곳. 대형 페이지면
 *             더 큰 정렬 단위를 알려 준다.
 * @write_fault: 쓰기 접근인가. 참이면 쓰기 가능한 매핑이어야 한다.
 * @return: 0 성공, -EAGAIN(락이 풀렸으니 재시도하라), -EFAULT(쓰기 불가),
 *          그 밖에 폴트 처리 오류.
 *
 * 왜 필요한가: 다른 장치의 BAR 를 mmap 한 구간처럼 struct page 가 없는 메모리는
 * GUP 로 핀할 수 없다. 그런 구간은 페이지 테이블에 물리 주소가 직접 박혀 있으므로,
 * 그 값을 그대로 읽어 IOMMU 에 매핑해야 한다.
 *
 * 동작 단계:
 *  1. follow_pfnmap_start() 로 페이지 테이블을 잠그고 항목을 읽는다.
 *  2. 실패하면 아직 폴트가 나지 않은 것이므로 fixup_user_fault() 로 강제로 폴트를
 *     일으킨다. FAULT_FLAG_REMOTE 는 '현재 프로세스가 아닌 남의 mm 에 대한 폴트'
 *     라는 뜻이고, 쓰기 접근이면 FAULT_FLAG_WRITE 를 더한다.
 *  3. 폴트 처리 중에 mmap 락이 풀렸다면(unlocked) 그 사이 vma 가 바뀌었을 수 있으므로
 *     -EAGAIN 을 반환해 호출자가 vma 조회부터 다시 하게 한다. 이 재시도 규약이
 *     없으면 해제된 vma 를 참조하게 된다.
 *  4. 폴트가 성공했으면 follow_pfnmap_start() 를 다시 시도한다.
 *  5. 쓰기를 원했는데 매핑이 쓰기 불가면 -EFAULT, 아니면 pfn 과 addr_mask 를 채운다.
 *  6. follow_pfnmap_end() 로 페이지 테이블 락을 반드시 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자 vaddr_get_pfns() 가 mmap 읽기 락을 쥔 상태로
 * 부른다. 안에서 폴트를 처리하므로 잠들 수 있다.
 *
 * caller: vaddr_get_pfns() 한 곳뿐.
 * callee: follow_pfnmap_start(), fixup_user_fault(), follow_pfnmap_end().
 *   이들의 내부는 mm/ 이 없는 이 트리에서 확인 못 함.
 * 에러 경로: 모든 오류를 그대로 반환하고, vaddr_get_pfns() 가 -EAGAIN 만 재시도로
 *   해석하고 나머지는 위로 올린다.
 *
 * 호출 체인:
 *   vfio_pin_pages_remote() → vaddr_get_pfns() → [follow_fault_pfn] →
 *   follow_pfnmap_start()
 */
static int follow_fault_pfn(struct vm_area_struct *vma, struct mm_struct *mm,
			    unsigned long vaddr, unsigned long *pfn,
			    unsigned long *addr_mask, bool write_fault)
{
	struct follow_pfnmap_args args = { .vma = vma, .address = vaddr };
	/* [한국어] 조회/폴트 결과. */
	int ret;

	/* [한국어] 페이지 테이블을 잠그고 항목을 읽어 본다. */
	ret = follow_pfnmap_start(&args);
	/* [한국어] 아직 폴트가 나지 않았으면 */
	if (ret) {
		/* [한국어] 폴트 처리 중 mmap 락이 풀렸는지 받을 곳. */
		bool unlocked = false;

		/* [한국어] 강제로 폴트를 일으켜 매핑을 만든다. */
		ret = fixup_user_fault(mm, vaddr,
				       FAULT_FLAG_REMOTE |
				       /* [한국어] 쓰기 접근이면 쓰기 폴트로 처리해 쓰기 가능한 매핑을 얻는다. */
				       (write_fault ?  FAULT_FLAG_WRITE : 0),
				       &unlocked);
		/* [한국어] 락이 풀렸었다면 그 사이 vma 가 바뀌었을 수 있으므로 호출자가 처음부터 다시 하게 한다. */
		if (unlocked)
			return -EAGAIN;

		/* [한국어] 폴트 자체가 실패했으면 그대로 반환한다. */
		if (ret)
			return ret;

		/* [한국어] 폴트가 성공했으니 다시 조회한다. */
		ret = follow_pfnmap_start(&args);
		/* [한국어] 그래도 실패하면 포기한다. */
		if (ret)
			return ret;
	}

	/* [한국어] 쓰기를 원했는데 매핑이 쓰기 불가면 거절한다. */
	if (write_fault && !args.writable) {
		ret = -EFAULT;
	} else {
		*pfn = args.pfn;
		*addr_mask = args.addr_mask;
	}

	/* [한국어] 페이지 테이블 락을 반드시 놓는다. 성공·실패 어느 쪽이든 거친다. */
	follow_pfnmap_end(&args);
	return ret;
}

/* [한국어]
 * vaddr_get_pfns - 사용자 가상 주소에서 pfn 을 얻는다(핀하거나, pfnmap 이면 읽어 온다)
 *
 * @mm: 대상 주소 공간.
 * @vaddr: 시작 사용자 가상 주소.
 * @npages: 얻고 싶은 페이지 수(상한). 실제로는 batch->capacity 로 한 번 더 잘린다.
 * @prot: 이 매핑의 권한. IOMMU_WRITE 가 있으면 쓰기 가능하게 핀한다.
 * @pfn: 첫 페이지의 pfn 을 받을 곳.
 * @batch: 페이지 배열. 성공 시 size/offset 이 갱신된다.
 * @return: 양수면 얻은 페이지 수, 음수면 오류.
 *
 * 이 파일에서 실제로 페이지를 핀하는 유일한 지점이다. 두 종류의 메모리를 모두
 * 다룬다: (a) 일반 RAM — pin_user_pages_remote() 로 핀하고 배치에 담는다.
 * (b) VM_PFNMAP 구간(MMIO 를 mmap 한 것) — 핀이라는 개념이 없으므로 페이지
 * 테이블에서 물리 주소만 읽어 온다. 상류 주석대로 (b) 의 경우 batch 는 채워지지
 * 않고, 반환된 개수만큼 pfn 이 연속이라는 것만 보장된다.
 *
 * 동작 단계:
 *  1. 실제 요청 수를 npages 와 batch->capacity 중 작은 값으로 정한다.
 *  2. 쓰기 권한이면 FOLL_WRITE 를 세운다.
 *  3. mmap 읽기 락을 잡는다. 이 함수를 벗어나기 전에 반드시 놓는다(done 라벨).
 *  4. pin_user_pages_remote(..., FOLL_LONGTERM, ...) 을 부른다. FOLL_LONGTERM 은
 *     '오랫동안 핀해 둘 것' 이라는 표시로, 이동 불가능한 영역(예: CMA)에서
 *     이동 가능한 곳으로 페이지를 옮긴 뒤 핀하게 만든다. 이 플래그가 없으면
 *     장기 핀이 메모리 압축을 영구히 방해한다.
 *  5. 양수를 얻으면 첫 페이지의 pfn 을 돌려주고 배치의 size/offset 을 갱신한 뒤
 *     done 으로 간다.
 *  6. 0 을 얻으면 -EFAULT 로 바꾼다(진행이 전혀 없다는 뜻).
 *  7. 여기까지 왔다면 GUP 가 실패한 것이므로 pfnmap 가능성을 본다.
 *     untagged_addr_remote() 로 아키텍처별 주소 태그(예: 상위 바이트 태깅)를 떼고,
 *     vma_lookup() 으로 vma 를 찾는다.
 *  8. VM_PFNMAP 이면 follow_fault_pfn() 으로 pfn 과 addr_mask 를 얻는다.
 *     -EAGAIN 이면 mmap 락이 풀렸던 것이므로 retry 라벨로 되돌아가 vma 부터 다시 찾는다.
 *  9. 얻은 pfn 이 예약 pfn 이어야만(struct page 로 관리되지 않는 진짜 MMIO)
 *     받아들인다. 그렇지 않으면 -EFAULT — GUP 가 실패한 일반 페이지를 우회로
 *     핀 없이 매핑해 주면 안 되기 때문이다.
 * 10. epfn = (*pfn | (~addr_mask >> PAGE_SHIFT)) + 1 로 이 pfnmap 페이지의 끝
 *     다음 pfn 을 구한다. addr_mask 는 해당 (대형) 페이지의 상위 비트만 남기는
 *     마스크이므로, 그 보수를 PAGE_SHIFT 만큼 내리면 페이지 안의 오프셋 pfn 을
 *     모두 1 로 만든 값이 된다. 그것을 OR 하면 그 페이지의 마지막 pfn 이 되고,
 *     +1 이 끝 다음이다. 반환 개수는 npages 와 (epfn - pfn) 중 작은 값이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. iommu->lock 을 쥔 채 불리고, 안에서 mmap 읽기 락을
 * 잡으며 GUP 가 잠들 수 있다.
 *
 * caller: vfio_pin_pages_remote()(대량 경로), vfio_pin_page_external()(한 장 경로).
 * callee: pin_user_pages_remote(), untagged_addr_remote(), vma_lookup(),
 *   follow_fault_pfn(), is_invalid_reserved_pfn().
 * 에러 경로: 어느 경로로 실패하든 done 을 거쳐 mmap 락을 놓고 음수를 반환한다.
 *   호출자는 그 값을 그대로 사용자에게 올리거나 핀 되감기를 시작한다.
 *
 * 호출 체인:
 *   vfio_pin_map_dma() → vfio_pin_pages_remote() → [vaddr_get_pfns] →
 *   pin_user_pages_remote()
 */
/*
 * Returns the positive number of pfns successfully obtained or a negative
 * error code.  The initial pfn is stored in the pfn arg.  For page-backed
 * pfns, the provided batch is also updated to indicate the filled pages and
 * initial offset.  For VM_PFNMAP pfns, only the returned number of pfns and
 * returned initial pfn are provided; subsequent pfns are contiguous.
 */
static long vaddr_get_pfns(struct mm_struct *mm, unsigned long vaddr,
			   unsigned long npages, int prot, unsigned long *pfn,
			   struct vfio_batch *batch)
{
	unsigned long pin_pages = min_t(unsigned long, npages, batch->capacity);
	/* [한국어] pfnmap 경로에서 조회할 가상 메모리 구간. */
	struct vm_area_struct *vma;
	/* [한국어] GUP 에 넘길 플래그. */
	unsigned int flags = 0;
	/* [한국어] GUP 결과 또는 오류. */
	long ret;

	/* [한국어] 장치가 메모리에 쓸 수 있어야 하면 */
	if (prot & IOMMU_WRITE)
		/* [한국어] 쓰기 가능하게 핀하라고 알린다. 이 플래그가 없으면 COW 페이지가 복사되지 않아 장치가 엉뚱한 사본에 쓴다. */
		flags |= FOLL_WRITE;

	/* [한국어] GUP 와 vma 조회 동안 주소 공간이 바뀌지 않도록 읽기 락을 잡는다. */
	mmap_read_lock(mm);
	/* [한국어] 페이지를 핀한다. FOLL_LONGTERM 은 오래 붙잡아 둘 것이라는 표시로,
	 * 이동 불가 영역의 페이지를 이동 가능한 곳으로 옮긴 뒤 핀하게 만든다.
	 * 이 플래그가 없으면 장기 핀이 메모리 압축을 영구히 방해한다. */
	ret = pin_user_pages_remote(mm, vaddr, pin_pages, flags | FOLL_LONGTERM,
				    batch->pages, NULL);
	/* [한국어] 한 장이라도 핀했으면 */
	if (ret > 0) {
		*pfn = page_to_pfn(batch->pages[0]);
		/* [한국어] GUP 가 채운 개수를 배치에 기록한다. */
		batch->size = ret;
		/* [한국어] 배치를 처음부터 소비하도록 위치를 리셋한다. */
		batch->offset = 0;
		/* [한국어] 성공했으므로 락 해제 지점으로 간다. */
		goto done;
	/* [한국어] 0 은 아무 진전이 없었다는 뜻이다. */
	} else if (!ret) {
		ret = -EFAULT;
	}

	/* [한국어] 아키텍처별 주소 태그를 떼어 실제 매핑 주소로 만든다. vma 조회는 태그가 붙어 있으면 실패한다. */
	vaddr = untagged_addr_remote(mm, vaddr);

retry:
	/* [한국어] 이 주소를 덮는 가상 메모리 구간을 찾는다. */
	vma = vma_lookup(mm, vaddr);

	/* [한국어] vma 가 있고 struct page 가 없는 MMIO 매핑이면 */
	if (vma && vma->vm_flags & VM_PFNMAP) {
		/* [한국어] 이 매핑을 덮는 페이지의 크기 마스크를 받을 곳. */
		unsigned long addr_mask;

		/* [한국어] 페이지 테이블에서 물리 주소를 직접 읽는다. */
		ret = follow_fault_pfn(vma, mm, vaddr, pfn, &addr_mask,
				       prot & IOMMU_WRITE);
		/* [한국어] mmap 락이 풀렸었다면 vma 가 바뀌었을 수 있으므로 조회부터 다시 한다. */
		if (ret == -EAGAIN)
			goto retry;

		/* [한국어] 물리 주소를 얻었으면 */
		if (!ret) {
			/* [한국어] 그것이 예약 pfn 이어야 한다 — GUP 가 실패한 일반 페이지를 핀 없이 매핑해 주면 안 된다. */
			if (is_invalid_reserved_pfn(*pfn)) {
				/* [한국어] 이 pfnmap 페이지의 끝 다음 pfn. */
				unsigned long epfn;

				/* [한국어] addr_mask 는 해당 (대형) 페이지의 상위 비트만 남기는 마스크다. 그 보수를
				 * PAGE_SHIFT 만큼 내리면 페이지 안의 오프셋 pfn 이 모두 1 인 값이 되고,
				 * OR 하면 그 페이지의 마지막 pfn, +1 이 끝 다음 pfn 이다. */
				epfn = (*pfn | (~addr_mask >> PAGE_SHIFT)) + 1;
				/* [한국어] 요청량과 이 페이지에 남은 양 중 작은 쪽만 돌려준다. */
				ret = min_t(long, npages, epfn - *pfn);
			} else {
				/* [한국어] pfnmap 도 아니면 번역할 수 없는 주소다. */
				ret = -EFAULT;
			}
		}
	}
done:
	/* [한국어] 어느 경로로 끝나든 mmap 읽기 락을 반드시 놓는다. */
	mmap_read_unlock(mm);
	return ret;
}


/* [한국어]
 * vpfn_pages - 주어진 IOVA 범위 안에 이미 외부 핀된 페이지가 몇 장인지 센다
 *
 * @dma: 대상 매핑. dma->pfn_list 를 훑는다.
 * @iova_start: 범위 시작 IOVA.
 * @nr_pages: 범위의 페이지 수.
 * @return: 그 범위 안에 있는 vfio_pfn 노드 개수(= 이미 핀·과금된 페이지 수).
 *
 * 이 함수는 과금 장부의 이중 계상을 막는 장치다. mdev 가 vfio_pin_pages() 로 미리
 * 핀해 둔 페이지는 그때 이미 잠금 메모리로 세어졌다. 나중에 같은 범위를 IOMMU
 * 매핑용으로 다시 핀할 때 그 수만큼 빼지 않으면 사용자는 같은 페이지에 두 번
 * 과금당한다. 반대로 언핀할 때도 그만큼 덜 돌려주어야 균형이 맞는다.
 *
 * 동작 단계:
 *  1. 범위의 배타적 끝을 iova_start + (nr_pages << PAGE_SHIFT) 로 구한다.
 *  2. vfio_find_vpfn_range() 로 범위 안의 노드 하나를 찾는다. 없으면
 *     (likely 로 표시된 흔한 경우) 0 을 반환한다 — mdev 가 없는 시스템에서는
 *     언제나 이 경로다.
 *  3. 찾았으면 개수를 1 로 시작해, 그 노드에서 rb_prev() 로 왼쪽으로 가며
 *     iova 가 범위 시작보다 작아질 때까지 센다.
 *  4. 같은 노드에서 rb_next() 로 오른쪽으로 가며 iova 가 범위 끝 이상이 될 때까지 센다.
 *     트리가 iova 오름차순이므로 양방향 선형 훑기로 범위 전체를 정확히 덮는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 순수 계산이다.
 *
 * caller: vfio_pin_pages_remote()(과금할 양에서 뺄 수),
 *   vfio_unpin_pages_remote()(되돌릴 양에서 뺄 수).
 * callee: vfio_find_vpfn_range(), rb_prev(), rb_next(), rb_entry().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pin_pages_remote() → [vpfn_pages] → vfio_find_vpfn_range()
 */
static long vpfn_pages(struct vfio_dma *dma,
		dma_addr_t iova_start, long nr_pages)
{
	dma_addr_t iova_end = iova_start + (nr_pages << PAGE_SHIFT);
	/* [한국어] 범위 안의 노드 하나를 찾는다. */
	struct vfio_pfn *top = vfio_find_vpfn_range(dma, iova_start, iova_end);
	/* [한국어] 찾았으므로 최소 1 개다. */
	long ret = 1;
	/* [한국어] 좌우 훑기용 임시 포인터. */
	struct vfio_pfn *vpfn;
	/* [한국어] 왼쪽 방향 커서. */
	struct rb_node *prev;
	/* [한국어] 오른쪽 방향 커서. */
	struct rb_node *next;

	/* [한국어] 범위 안에 외부 핀이 없는 것이 흔한 경우다 — mdev 가 없는 시스템에서는 언제나 이 경로다. */
	if (likely(!top))
		return 0;

	/* [한국어] 찾은 노드에서 좌우 양쪽으로 뻗어 나간다. */
	prev = next = &top->node;

	/* [한국어] 왼쪽(더 낮은 IOVA)으로 훑는다. */
	while ((prev = rb_prev(prev))) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		vpfn = rb_entry(prev, struct vfio_pfn, node);
		/* [한국어] 범위 시작보다 낮아지면 더 볼 것이 없다 — 트리가 오름차순이기 때문이다. */
		if (vpfn->iova < iova_start)
			break;
		/* [한국어] 범위 안이므로 하나 센다. */
		ret++;
	}

	/* [한국어] 오른쪽(더 높은 IOVA)으로 훑는다. */
	while ((next = rb_next(next))) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		vpfn = rb_entry(next, struct vfio_pfn, node);
		/* [한국어] 범위 끝 이상이면 종료한다. */
		if (vpfn->iova >= iova_end)
			break;
		/* [한국어] 범위 안이므로 하나 센다. */
		ret++;
	}

	return ret;
}

/* [한국어]
 * vfio_pin_pages_remote - 물리적으로 연속인 최대 구간을 핀하고 그만큼 과금한다
 *
 * @dma: 대상 매핑. vaddr/iova 오프셋, prot, 과금 주체를 여기서 가져온다.
 * @vaddr: 핀을 시작할 사용자 가상 주소.
 * @npage: 핀하고 싶은 최대 페이지 수.
 * @pfn_base: 첫 페이지의 pfn 을 받을 곳. 호출자는 이 값으로 iommu_map() 을 부른다.
 * @limit: RLIMIT_MEMLOCK 한도를 페이지 수로 환산한 값.
 * @batch: 재사용할 GUP 배치. 이전 호출의 잔여 페이지가 남아 있을 수 있다.
 * @return: 양수면 핀한 페이지 수, 음수면 오류.
 *
 * 이 파일의 심장이다. IOMMU 는 물리적으로 연속인 구간만 한 번에 매핑할 수 있으므로,
 * '첫 페이지부터 시작해 pfn 이 계속 이어지고 예약 여부도 같은 동안' 만 핀해서
 * 그 길이를 돌려준다. 호출자는 그 길이만큼 iommu_map() 을 부르고, 남은 부분에 대해
 * 다시 이 함수를 부르는 식으로 매핑을 완성한다.
 *
 * 동작 단계:
 *  1. current->mm 이 없으면(커널 스레드) -ENODEV. 이 경로는 사용자 요청으로만 온다.
 *  2. 배치에 이전 호출의 잔여가 있으면 그 첫 페이지를 시작점으로 삼는다.
 *     없으면 pfn_base 를 0 으로 두어 '아직 시작점이 정해지지 않음' 을 표시한다.
 *  3. disable_hugepages 가 켜져 있으면 npage 를 1 로 줄인다.
 *  4. 바깥 루프: 아직 요청량이 남아 있는 동안 반복한다.
 *     4-1. 배치가 비었으면 cond_resched() 로 CPU 를 양보한 뒤 vaddr_get_pfns() 로
 *          다시 채운다. 큰 매핑에서 이 루프가 길어져 다른 태스크를 굶기지 않게
 *          하려는 것이다.
 *     4-2. 아직 시작점이 없으면 이번에 얻은 pfn 을 시작점으로 삼고 예약 여부를 기록한다.
 *     4-3. 배치가 여전히 비어 있다면 pfnmap 경로다(struct page 가 없다). 이때는
 *          pfn 이 시작점에서 pinned 만큼 떨어진 곳에 정확히 이어져야 하고 예약
 *          pfn 이어야 한다. 아니면 out 으로 빠져 지금까지의 길이만 확정한다.
 *          맞으면 반환된 개수만큼 진행시키고 다음 바퀴로 간다.
 *     4-4. 안쪽 루프: 배치에 담긴 페이지들을 소비한다.
 *          - pfn 이 연속이 아니거나 예약 여부가 달라지면 out 으로 나간다. 여기서
 *            끊어야 iommu_map() 한 번에 연속 구간만 들어간다.
 *          - num_pages_contiguous() 로 배치 안에서 물리적으로 이어지는 페이지 수를
 *            한 번에 구한다.
 *          - 예약 pfn 이 아니면 그 수에서 vpfn_pages() 가 센 '이미 외부 핀된 수' 를
 *            빼 실제 과금할 양(acct_pages)을 구한다.
 *          - 과금할 양이 있으면 CAP_IPC_LOCK 이 없는 한 mm->locked_vm + 누적 +
 *            이번 양이 limit 을 넘는지 검사한다. 넘으면 경고를 찍고 -ENOMEM 으로
 *            unpin_out 으로 간다. limit 은 페이지 수이므로 로그에는 PAGE_SHIFT 만큼
 *            올려 바이트로 찍는다.
 *          - 진행 상태(pinned/npage/vaddr/iova/batch offset·size)를 한꺼번에 갱신한다.
 *          - 배치가 비면 안쪽 루프를 벗어나 다시 채우러 간다. 아니면 다음 pfn 을
 *            미리 읽어 둔다(상류 주석이 설명하는 '루프 끝에서 다음 pfn 을 가져오는'
 *            구조다 — 첫 바퀴의 pfn 은 pfnmap 을 위해 vaddr_get_pfns() 가 이미 준다).
 *  5. out 라벨: 이 매핑에 예약 pfn 이 섞였다는 사실을 dma->has_rsvd 에 OR 로 기록하고,
 *     누적된 과금량을 vfio_lock_acct() 로 한 번에 반영한다. 여러 번 나누어 과금하지
 *     않는 이유는 mmap 쓰기 락 획득 횟수를 줄이기 위해서다.
 *  6. unpin_out 라벨: ret 가 음수면 지금까지 핀한 것(연속이므로 pfn_base 부터 순번)을
 *     put_pfn() 으로 모두 풀고, 배치에 남은 것도 vfio_batch_unpin() 으로 푼 뒤 오류를
 *     반환한다. 예약 pfn 구간은 애초에 핀하지 않았으므로 !rsvd 일 때만 푼다.
 *  7. 정상이면 핀한 페이지 수를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. GUP 와 회계에서 잠들 수 있다.
 * 사용자 요청 경로에서만 불리므로 current 가 곧 매핑 소유 프로세스다.
 *
 * caller: vfio_pin_map_dma()(새 매핑), vfio_iommu_replay()(새 domain 에 재설치).
 * callee: vaddr_get_pfns(), is_invalid_reserved_pfn(), num_pages_contiguous(),
 *   vpfn_pages(), vfio_lock_acct(), put_pfn(), vfio_batch_unpin(), cond_resched().
 * 에러 경로: 위 6 번. 핀 장부와 과금 장부를 모두 원상 복구한 뒤 음수를 반환하며,
 *   호출자는 매핑 전체를 없앤다.
 *
 * 호출 체인:
 *   vfio_dma_do_map() → vfio_pin_map_dma() → [vfio_pin_pages_remote] →
 *   vaddr_get_pfns()
 */
/*
 * Attempt to pin pages.  We really don't want to track all the pfns and
 * the iommu can only map chunks of consecutive pfns anyway, so get the
 * first page and all consecutive pages with the same locking.
 */
static long vfio_pin_pages_remote(struct vfio_dma *dma, unsigned long vaddr,
				  unsigned long npage, unsigned long *pfn_base,
				  unsigned long limit, struct vfio_batch *batch)
{
	unsigned long pfn;
	/* [한국어] 핀 대상 주소 공간 — 이 경로는 사용자 요청 문맥이므로 current 가 곧 소유자다. */
	struct mm_struct *mm = current->mm;
	/* [한국어] 결과 코드, 지금까지 핀한 수, 아직 반영하지 않은 과금 누적량. */
	long ret, pinned = 0, lock_acct = 0;
	/* [한국어] 이 구간이 예약 pfn 인가. 첫 페이지를 얻는 순간 정해진다. */
	bool rsvd;
	/* [한국어] 이 vaddr 에 대응하는 IOVA. 두 주소의 차가 매핑 내내 일정하다는 성질을 쓴다. */
	dma_addr_t iova = vaddr - dma->vaddr + dma->iova;

	/* This code path is only user initiated */
	/* [한국어] 커널 스레드에는 사용자 주소 공간이 없다. 이 경로는 사용자 요청으로만 온다. */
	if (!mm)
		/* [한국어] 장치가 없다는 오류로 거절한다. */
		return -ENODEV;

	/* [한국어] 이전 호출이 남긴 페이지가 배치에 있으면 */
	if (batch->size) {
		/* Leftover pages in batch from an earlier call. */
		*pfn_base = page_to_pfn(batch->pages[batch->offset]);
		/* [한국어] 그 페이지를 이번 연속 구간의 시작점으로 삼는다. */
		pfn = *pfn_base;
		/* [한국어] 잔여 페이지의 예약 여부를 기억해 둔다 — 아래 루프의 연속성 판정 기준이다. */
		rsvd = is_invalid_reserved_pfn(*pfn_base);
	} else {
		*pfn_base = 0;
	}

	/* [한국어] 대형 페이지 최적화를 끄는 디버깅 스위치가 켜져 있으면 */
	if (unlikely(disable_hugepages))
		/* [한국어] 한 번에 한 페이지만 다룬다. */
		npage = 1;

	/* [한국어] 요청량이 남아 있는 동안 반복한다. */
	while (npage) {
		if (!batch->size) {
			/*
			 * Large mappings may take a while to repeatedly refill
			 * the batch, so conditionally relinquish the CPU when
			 * needed to avoid stalls.
			 */
			/* [한국어] 큰 매핑에서 배치를 반복해 채우는 동안 다른 태스크를 굶기지 않도록 CPU 를 양보한다. */
			cond_resched();

			/* Empty batch, so refill it. */
			/* [한국어] 배치를 다시 채운다. */
			ret = vaddr_get_pfns(mm, vaddr, npage, dma->prot,
					     /* [한국어] 첫 pfn 과 배치를 받는다. */
					     &pfn, batch);
			/* [한국어] GUP 가 실패했으면 지금까지 핀한 것을 되돌려야 한다. */
			if (ret < 0)
				goto unpin_out;

			/* [한국어] 아직 시작점이 정해지지 않았으면 */
			if (!*pfn_base) {
				*pfn_base = pfn;
				/* [한국어] 이 구간이 예약 pfn 인지 여기서 확정한다. 이후 연속성 판정의 기준이 된다. */
				rsvd = is_invalid_reserved_pfn(*pfn_base);
			}

			/* Handle pfnmap */
			/* [한국어] GUP 가 페이지를 주지 않았다면 struct page 가 없는 pfnmap 구간이다. */
			if (!batch->size) {
				/* [한국어] pfnmap 경로 — 물리 주소가 시작점에서 pinned 만큼 정확히 떨어져 있고
				 * 예약 pfn 이어야 같은 구간으로 이어 붙일 수 있다. 아니면 여기까지를 확정한다. */
				if (pfn != *pfn_base + pinned || !rsvd)
					goto out;

				/* [한국어] 얻은 만큼 진행 수를 올리고 */
				pinned += ret;
				/* [한국어] 남은 요청량을 줄이고 */
				npage -= ret;
				/* [한국어] 사용자 주소를 전진시키고 */
				vaddr += (PAGE_SIZE * ret);
				/* [한국어] IOVA 도 같은 만큼 전진시킨다. */
				iova += (PAGE_SIZE * ret);
				continue;
			}
		}

		/*
		 * pfn is preset for the first iteration of this inner loop
		 * due to the fact that vaddr_get_pfns() needs to provide the
		 * initial pfn for pfnmaps.  Therefore to reduce redundancy,
		 * the next pfn is fetched at the end of the loop.
		 * A PageReserved() page could still qualify as page backed
		 * and rsvd here, and therefore continues to use the batch.
		 */
		/* [한국어] 배치에 담긴 페이지들을 연속인 동안 소비한다. */
		while (true) {
			/* [한국어] 이번 바퀴에 이어 붙일 페이지 수와, 그중 실제로 과금할 수. */
			long nr_pages, acct_pages = 0;

			/* [한국어] 물리 주소가 끊겼거나 예약 여부가 달라지면 여기까지가 한 연속 구간이다. */
			if (pfn != *pfn_base + pinned ||
			    rsvd != is_invalid_reserved_pfn(pfn))
				goto out;

			/*
			 * Using GUP with the FOLL_LONGTERM in
			 * vaddr_get_pfns() will not return invalid
			 * or reserved pages.
			 */
			/* [한국어] 배치 안에서 물리적으로 이어지는 페이지 수를 한 번에 구한다. */
			nr_pages = num_pages_contiguous(
					/* [한국어] 다음에 소비할 위치부터 */
					&batch->pages[batch->offset],
					/* [한국어] 남아 있는 개수까지를 대상으로 본다. */
					batch->size);
			/* [한국어] 예약 pfn 이 아니면 과금 대상이다. */
			if (!rsvd) {
				/* [한국어] 일단 전부를 과금 후보로 잡고 */
				acct_pages = nr_pages;
				/* [한국어] 이미 외부 핀으로 과금된 페이지 수를 뺀다 — 이중 계상을 막는 지점이다. */
				acct_pages -= vpfn_pages(dma, iova, nr_pages);
			}

			/*
			 * Reserved pages aren't counted against the user,
			 * externally pinned pages are already counted against
			 * the user.
			 */
			/* [한국어] 실제로 과금할 페이지가 있으면 한도를 확인해야 한다. */
			if (acct_pages) {
				/* [한국어] CAP_IPC_LOCK 이 없으면 한도를 검사한다. */
				if (!dma->lock_cap &&
				    mm->locked_vm + lock_acct + acct_pages > limit) {
					/* [한국어] 한도를 넘으면 경고를 남긴다. limit 은 페이지 수이므로 로그에는 바이트로 환산해 찍는다. */
					pr_warn("%s: RLIMIT_MEMLOCK (%ld) exceeded\n",
						__func__, limit << PAGE_SHIFT);
					/* [한국어] 한도 초과는 메모리 부족으로 보고한다. */
					ret = -ENOMEM;
					/* [한국어] 지금까지 핀한 것을 되돌리러 간다. */
					goto unpin_out;
				}
				/* [한국어] 과금 누적량에 더한다. 실제 반영은 루프가 끝난 뒤 한 번에 한다. */
				lock_acct += acct_pages;
			}

			/* [한국어] 핀한 총 수를 올리고 */
			pinned += nr_pages;
			/* [한국어] 남은 요청량을 줄이고 */
			npage -= nr_pages;
			/* [한국어] 사용자 주소를 전진시키고 */
			vaddr += PAGE_SIZE * nr_pages;
			/* [한국어] IOVA 도 전진시키고 */
			iova += PAGE_SIZE * nr_pages;
			/* [한국어] 배치의 소비 위치를 전진시키고 */
			batch->offset += nr_pages;
			/* [한국어] 배치의 잔여 수를 줄인다. offset+size 의 합이 불변이어야 잔여 정리가 정확해진다. */
			batch->size -= nr_pages;

			/* [한국어] 배치를 다 썼으면 바깥 루프로 나가 다시 채운다. */
			if (!batch->size)
				break;

			/* [한국어] 아직 남았으면 다음 pfn 을 미리 읽어 둔다 — 루프 머리의 연속성 판정에 쓰인다. */
			pfn = page_to_pfn(batch->pages[batch->offset]);
		}
	}

out:
	/* [한국어] 예약 pfn 이 섞였다는 사실을 매핑에 누적 기록한다. 한 번 서면 매핑이 사라질 때까지 내려가지 않는다. */
	dma->has_rsvd |= rsvd;
	/* [한국어] 누적된 과금량을 한 번에 반영한다. 여러 번 나누지 않는 것은 mmap 쓰기 락 획득 횟수를 줄이기 위해서다. */
	ret = vfio_lock_acct(dma, lock_acct, false);

unpin_out:
	/* [한국어] 오류로 빠져나온 경우에만 되감는다. */
	if (ret < 0) {
		/* [한국어] 핀한 것이 있고 예약 구간이 아니면 — 예약 pfn 은 애초에 핀하지 않았다. */
		if (pinned && !rsvd) {
			/* [한국어] 연속 구간이므로 시작 pfn 부터 순번으로 훑으며 */
			for (pfn = *pfn_base ; pinned ; pfn++, pinned--)
				/* [한국어] 한 장씩 핀을 푼다. */
				put_pfn(pfn, dma->prot);
		}
		/* [한국어] 배치에 남은 잔여 핀도 되돌린다. */
		vfio_batch_unpin(batch, dma);

		return ret;
	}

	/* [한국어] 핀한 페이지 수를 돌려준다. 호출자는 이만큼을 iommu_map 에 넘긴다. */
	return pinned;
}

/* [한국어]
 * put_valid_unreserved_pfns - 연속된 일반 페이지 구간의 핀을 한 번에 푼다
 *
 * @start_pfn: 구간의 첫 pfn.
 * @npage: 페이지 수.
 * @prot: 매핑 권한. IOMMU_WRITE 가 서 있으면 dirty 로 표시한다.
 * @return: 없음.
 *
 * put_pfn() 을 npage 번 부르는 대신, 구간 단위 API 한 번으로 끝내는 빠른 경로다.
 * 전제는 '이 구간에 예약 pfn 이 하나도 없다' 는 것이며, 그 판단은 호출자가
 * dma->has_rsvd 로 한다. 예약 pfn 이 섞여 있으면 이 함수를 쓸 수 없다 —
 * struct page 가 없는 pfn 에 pfn_to_page() 를 부르게 되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_unpin_pages_remote() 한 곳뿐.
 * callee: pfn_to_page(), unpin_user_page_range_dirty_lock(). 후자의 내부는 mm/ 이
 *   없는 이 트리에서 확인 못 함.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_unmap_unpin() → vfio_unpin_pages_remote() → [put_valid_unreserved_pfns] →
 *   unpin_user_page_range_dirty_lock()
 */
static inline void put_valid_unreserved_pfns(unsigned long start_pfn,
		unsigned long npage, int prot)
{
	unpin_user_page_range_dirty_lock(pfn_to_page(start_pfn), npage,
					 /* [한국어] 쓰기 가능하게 매핑했던 구간이면 페이지를 dirty 로 표시한다. */
					 prot & IOMMU_WRITE);
}

/* [한국어]
 * vfio_unpin_pages_remote - 연속 구간의 핀을 풀고 과금을 되돌린다
 *
 * @dma: 대상 매핑. prot 와 has_rsvd 로 어떤 언핀 경로를 쓸지 정한다.
 * @iova: 구간의 시작 IOVA. 외부 핀 개수를 세는 기준이다.
 * @pfn: 구간의 첫 pfn.
 * @npage: 페이지 수.
 * @do_accounting: true 면 이 함수 안에서 과금을 되돌린다. false 면 호출자가
 *                 반환값을 모아 나중에 한 번에 되돌린다.
 * @return: 실제로 언핀한 페이지 수.
 *
 * vfio_pin_pages_remote() 의 짝이다. 두 장부를 동시에 되감는다: 핀 장부는 페이지
 * 핀을 풀어서, 과금 장부는 vfio_lock_acct() 에 음수를 넘겨서.
 *
 * 동작 단계:
 *  1. 먼저 vpfn_pages() 로 이 구간에서 '외부 핀으로 이미 과금되어 있는' 페이지 수를
 *     센다(locked). 이 페이지들은 언핀해도 과금이 그대로 남아야 한다 — 외부 핀
 *     참조가 살아 있기 때문이다.
 *  2. dma->has_rsvd 가 참이면 예약 pfn 이 섞였을 수 있으므로 페이지마다 put_pfn() 을
 *     부르고, 실제로 풀린 것(반환 1)만 unlocked 로 센다.
 *  3. 아니면 put_valid_unreserved_pfns() 로 구간 전체를 한 번에 풀고 unlocked 를
 *     npage 로 둔다.
 *  4. do_accounting 이면 vfio_lock_acct(dma, locked - unlocked, true) 를 부른다.
 *     부호에 주의한다: unlocked 만큼 돌려주어야 하므로 -unlocked 이고, 그중 외부
 *     핀으로 남아 있어야 할 locked 만큼은 다시 더해 주므로 합이 locked - unlocked
 *     (대개 음수)가 된다. async=true 인 이유는 이 경로가 컨테이너 해제처럼 매핑
 *     소유 프로세스가 이미 죽은 뒤에도 불릴 수 있기 때문이다.
 *  5. 실제로 언핀한 페이지 수를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_sync_unpin(), unmap_unpin_slow(), vfio_pin_map_dma() 의 매핑 실패
 *   되감기, vfio_iommu_replay() 의 unwind.
 * callee: vpfn_pages(), put_pfn(), put_valid_unreserved_pfns(), vfio_lock_acct().
 * 에러 경로: 없다. 실패할 수 없는 정리 연산이다.
 *
 * 호출 체인:
 *   vfio_dma_do_unmap() → vfio_remove_dma() → vfio_unmap_unpin() →
 *   unmap_unpin_slow() → [vfio_unpin_pages_remote] → put_pfn()
 */
static long vfio_unpin_pages_remote(struct vfio_dma *dma, dma_addr_t iova,
				    unsigned long pfn, unsigned long npage,
				    bool do_accounting)
{
	long unlocked = 0, locked = vpfn_pages(dma, iova, npage);

	/* [한국어] 이 매핑에 예약 pfn 이 섞여 있었으면 페이지마다 검사해야 한다. */
	if (dma->has_rsvd) {
		/* [한국어] 반복 인덱스. */
		unsigned long i;

		/* [한국어] 한 장씩 훑으며 */
		for (i = 0; i < npage; i++)
			/* [한국어] 핀을 푼다. 예약 pfn 이면 0 이 돌아와 세지 않는다. */
			if (put_pfn(pfn++, dma->prot))
				/* [한국어] 실제로 풀린 것만 센다. */
				unlocked++;
	} else {
		/* [한국어] 예약 pfn 이 없으면 구간 전체를 한 번에 푸는 빠른 경로를 쓴다. */
		put_valid_unreserved_pfns(pfn, npage, dma->prot);
		/* [한국어] 전부 일반 페이지이므로 요청한 수가 곧 푼 수다. */
		unlocked = npage;
	}
	/* [한국어] 과금 처리를 이 함수가 맡았으면 */
	if (do_accounting)
		/* [한국어] 푼 것에서 외부 핀으로 남아야 할 것을 뺀 차액만 반영한다. 외부 핀 페이지는
		 * 참조가 살아 있어 계속 잠겨 있어야 하므로 과금을 되돌리면 안 된다. */
		vfio_lock_acct(dma, locked - unlocked, true);

	/* [한국어] 언핀한 페이지 수를 돌려준다. */
	return unlocked;
}

/* [한국어]
 * vfio_pin_page_external - mdev 를 위해 페이지 한 장을 핀하고 과금한다
 *
 * @dma: 대상 매핑. dma->mm 이 GUP 대상 주소 공간이다.
 * @vaddr: 핀할 사용자 가상 주소.
 * @pfn_base: 얻은 pfn 을 받을 곳.
 * @do_accounting: 이 컨테이너에 IOMMU domain 이 없어 여기서 과금해야 하는가.
 * @return: 0 성공, -ENODEV(주소 공간이 이미 해체됨), 그 밖에 GUP/과금 오류.
 *
 * vfio_pin_pages_remote() 와 갈라지는 점: (a) 언제나 한 장만 다룬다,
 * (b) current->mm 이 아니라 dma->mm 을 대상으로 한다. mdev 의 핀 요청은 매핑을 만든
 * 프로세스가 아닌 문맥(예: 다른 vCPU 스레드)에서 올 수 있기 때문이다.
 *
 * 동작 단계:
 *  1. mmget_not_zero() 로 dma->mm 을 사용 가능한 상태로 승격한다. 실패하면
 *     프로세스가 이미 끝난 것이므로 -ENODEV.
 *  2. vfio_batch_init_single() 로 한 장짜리 배치를 준비한다.
 *  3. vaddr_get_pfns(mm, vaddr, 1, ...) 로 한 장을 핀한다. 정확히 1 이 아니면 실패다.
 *  4. 성공하면 ret 를 0 으로 되돌린다 — 이 함수의 반환 규약은 개수가 아니라 0/오류다.
 *  5. do_accounting 이고 예약 pfn 이 아니면 vfio_lock_acct(dma, 1, false) 로 한 장을
 *     과금한다. async=false 인 이유는 방금 mmget 으로 mm 을 붙잡아 두었기 때문이다.
 *     과금이 실패하면 방금 핀한 페이지를 put_pfn() 으로 곧바로 되돌린다 — 두 장부가
 *     어긋나지 않게 하는 지점이다. -ENOMEM 이면 어느 태스크가 한도를 넘겼는지
 *     comm/pid/한도와 함께 경고를 남긴다.
 *  6. out 라벨에서 배치 배열을 해제하고 mmput() 으로 mm 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. GUP 와 회계에서 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_pin_pages() 한 곳뿐.
 * callee: mmget_not_zero(), vfio_batch_init_single(), vaddr_get_pfns(),
 *   is_invalid_reserved_pfn(), vfio_lock_acct(), put_pfn(), vfio_batch_fini(), mmput().
 * 에러 경로: 모든 실패가 out 을 거쳐 배치와 mm 참조를 정리한 뒤 음수를 반환하고,
 *   호출자는 pin_unwind 로 가서 앞서 핀한 페이지들을 되돌린다.
 *
 * 호출 체인:
 *   vfio_pin_pages() → vfio_iommu_type1_pin_pages() → [vfio_pin_page_external] →
 *   vaddr_get_pfns()
 */
static int vfio_pin_page_external(struct vfio_dma *dma, unsigned long vaddr,
				  unsigned long *pfn_base, bool do_accounting)
{
	struct vfio_batch batch;
	/* [한국어] GUP 대상이 될 주소 공간. */
	struct mm_struct *mm;
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] 매핑을 만든 프로세스의 주소 공간 — current 가 아니다. 핀 요청은 다른 스레드에서 올 수 있다. */
	mm = dma->mm;
	/* [한국어] 구조체만 붙잡아 둔 상태를 실제 사용 가능 상태로 승격한다. 실패하면 프로세스가 이미 끝났다. */
	if (!mmget_not_zero(mm))
		return -ENODEV;

	/* [한국어] 한 장만 다루므로 페이지 배열을 따로 할당하지 않는 모드로 초기화한다. */
	vfio_batch_init_single(&batch);

	/* [한국어] 정확히 한 장을 핀한다. */
	ret = vaddr_get_pfns(mm, vaddr, 1, dma->prot, pfn_base, &batch);
	/* [한국어] 1 이 아니면 실패다(음수 오류이거나 예상 밖의 개수). */
	if (ret != 1)
		goto out;

	/* [한국어] 이 함수의 반환 규약은 개수가 아니라 0/오류이므로 성공을 0 으로 바꾼다. */
	ret = 0;

	/* [한국어] 과금해야 하고 예약 pfn 이 아니면 — 예약 pfn 은 애초에 핀도 과금도 하지 않는다. */
	if (do_accounting && !is_invalid_reserved_pfn(*pfn_base)) {
		/* [한국어] 한 장을 과금한다. 방금 mm 을 붙잡아 두었으므로 async 는 false 다. */
		ret = vfio_lock_acct(dma, 1, false);
		/* [한국어] 한도를 넘어 실패하면 */
		if (ret) {
			/* [한국어] 방금 핀한 페이지를 곧바로 되돌린다 — 두 장부가 어긋나지 않게 하는 지점이다. */
			put_pfn(*pfn_base, dma->prot);
			/* [한국어] 한도 초과였으면 */
			if (ret == -ENOMEM)
				/* [한국어] 어느 태스크가 어떤 한도를 넘겼는지 남긴다. */
				pr_warn("%s: Task %s (%d) RLIMIT_MEMLOCK "
					"(%ld) exceeded\n", __func__,
					dma->task->comm, task_pid_nr(dma->task),
					task_rlimit(dma->task, RLIMIT_MEMLOCK));
		}
	}

out:
	/* [한국어] 배치 배열을 해제한다. */
	vfio_batch_fini(&batch);
	/* [한국어] 승격했던 주소 공간 참조를 놓는다. */
	mmput(mm);
	return ret;
}

/* [한국어]
 * vfio_unpin_page_external - mdev 가 핀했던 페이지 한 장의 참조를 놓는다
 *
 * @dma: 대상 매핑.
 * @iova: 놓을 페이지의 IOVA.
 * @do_accounting: 여기서 과금을 되돌려야 하는가.
 * @return: 실제로 언핀된 페이지 수(0 또는 1).
 *
 * vfio_pin_page_external() 의 짝이다. 실제 핀 해제는 참조가 0 이 될 때만 일어나므로,
 * 반환값이 0 이면 아직 다른 사용자가 있어 과금도 그대로 두어야 한다.
 *
 * 동작 단계:
 *  1. vfio_find_vpfn() 으로 그 IOVA 의 기록을 찾는다. 없으면 핀된 적이 없으므로 0.
 *  2. vfio_iova_put_vfio_pfn() 으로 참조를 놓는다. 0 이 되면 실제 언핀이 일어나고 1 을
 *     돌려준다.
 *  3. do_accounting 이면 그 수만큼 과금을 되돌린다(-unlocked). async=true 인 이유는
 *     컨테이너 해제 등 소유 프로세스가 이미 죽은 문맥에서도 불릴 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_unpin_pages()(정상 경로),
 *   vfio_iommu_type1_pin_pages() 의 pin_unwind 되감기.
 * callee: vfio_find_vpfn(), vfio_iova_put_vfio_pfn(), vfio_lock_acct().
 * 에러 경로: 없다. 기록이 없는 IOVA 는 조용히 0 을 반환한다.
 *
 * 호출 체인:
 *   vfio_unpin_pages() → vfio_iommu_type1_unpin_pages() →
 *   [vfio_unpin_page_external] → vfio_iova_put_vfio_pfn()
 */
static int vfio_unpin_page_external(struct vfio_dma *dma, dma_addr_t iova,
				    bool do_accounting)
{
	int unlocked;
	/* [한국어] 그 IOVA 에 대한 외부 핀 기록을 찾는다. */
	struct vfio_pfn *vpfn = vfio_find_vpfn(dma, iova);

	/* [한국어] 기록이 없으면 핀된 적이 없으므로 되돌릴 것도 없다. */
	if (!vpfn)
		return 0;

	/* [한국어] 참조를 하나 놓는다. 0 이 되면 실제 핀이 풀리고 1 이 돌아온다. */
	unlocked = vfio_iova_put_vfio_pfn(dma, vpfn);

	/* [한국어] 이 컨테이너에서 과금까지 했다면 */
	if (do_accounting)
		/* [한국어] 실제로 풀린 만큼만 되돌린다. async=true 는 소유 프로세스가 이미 죽었을 수 있어서다. */
		vfio_lock_acct(dma, -unlocked, true);

	/* [한국어] 실제로 언핀한 페이지 수를 돌려준다. 호출자가 과금 총합에 쓴다. */
	return unlocked;
}

/* [한국어]
 * vfio_iommu_type1_pin_pages - mdev 를 위해 IOVA 범위의 페이지들을 핀해 준다
 *
 * @iommu_data: 컨테이너 객체(void 포인터로 넘어온 struct vfio_iommu).
 * @iommu_group: 요청한 장치가 속한 IOMMU group. dirty 추적 범위 승격에 쓴다.
 * @user_iova: 핀할 범위의 시작 IOVA.
 * @npage: 핀할 페이지 수(4KB 단위).
 * @prot: 요청 권한. 매핑의 권한에 포함되어야 한다.
 * @pages: 결과 struct page 포인터들을 받을 배열. 호출자가 준비한다.
 * @return: 실제로 핀한 페이지 수(양수), 또는 음수 오류.
 *
 * vfio_iommu_driver_ops 의 pin_pages 콜백 구현이다. 에뮬레이트(mdev) 장치는 하드웨어
 * IOMMU 를 통해 DMA 하지 않고 호스트가 대신 메모리를 만지므로, 게스트 IOVA 에
 * 해당하는 호스트 페이지를 직접 얻어야 한다. 이 함수가 그것을 제공한다.
 *
 * 동작 단계:
 *  1. 인자 정합성 검사(NULL, npage <= 0)와 v2 전용 검사(-EACCES).
 *  2. npage * PAGE_SIZE 와 user_iova + 크기 - 1 을 오버플로 검사와 함께 계산한다.
 *     (iova_end/iova_size 는 오버플로 여부만 보기 위한 것이고 이후 쓰이지 않는다.)
 *  3. iommu->lock 을 잡는다. 이후 모든 트리 조작이 이 락 아래에 있다.
 *  4. vaddr 무효화가 진행 중이면(-EBUSY) 거절한다. 무효 vaddr 로는 핀할 수 없다.
 *  5. device_list 가 비어 있으면 -EINVAL. 언맵 통지를 받을 장치가 없으면 나중에
 *     강제로 언핀시킬 방법이 없어 핀을 허용해서는 안 된다.
 *  6. do_accounting 을 '이 컨테이너에 IOMMU domain 이 하나도 없는가' 로 정한다.
 *     domain 이 있으면 그 범위의 페이지는 이미 매핑 시점에 핀·과금되었으므로
 *     여기서 또 과금하면 이중 계상이 된다.
 *  7. 페이지마다 반복한다.
 *     - iova 를 계산하고 vfio_find_dma() 로 그 페이지를 덮는 매핑을 찾는다. 없으면 -EINVAL.
 *     - 매핑의 권한이 요청 권한을 모두 포함하지 않으면 -EPERM.
 *     - 이미 외부 핀 기록이 있으면 참조만 올리고 그 페이지를 그대로 돌려준다.
 *     - 없으면 remote_vaddr = dma->vaddr + (iova - dma->iova) 로 사용자 주소를 구해
 *       vfio_pin_page_external() 로 새로 핀한다. 두 주소 공간의 차가 매핑 내내
 *       일정하다는 성질을 이용한 변환이다.
 *     - pfn_valid() 로 struct page 가 있는 메모리인지 확인한다. mdev 는 struct page 를
 *       필요로 하므로 MMIO 는 받아들일 수 없다.
 *     - vfio_add_to_pfn_list() 로 기록한다. 실패하면 방금 핀한 페이지를 되돌리고
 *       과금도 되돌린 뒤 pin_unwind 로 간다.
 *     - dirty 추적 중이면 이 페이지의 비트를 세운다. mdev 가 핀한 페이지는 언제든
 *       더러워질 수 있으므로 보수적으로 표시한다.
 *  8. 전부 성공하면 이 group 의 dirty 추적 범위를 승격한다 — 이 group 이 핀 인터페이스를
 *     쓴다는 것이 확인되었으므로 num_non_pinned_groups 를 하나 줄여, 마이그레이션 시
 *     매핑 전체를 dirty 로 칠하지 않아도 되게 만든다.
 *  9. pin_unwind 라벨: 실패한 인덱스의 출력 슬롯을 NULL 로 만들고, 그 앞까지 성공한
 *     페이지들을 vfio_unpin_page_external() 로 되돌리며 슬롯도 NULL 로 지운다.
 * 10. pin_done 라벨에서 락을 놓고 결과를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 커널 안의 vendor 드라이버가 호출하며, 이 함수가
 * iommu->lock 을 직접 잡는다. GUP 로 잠들 수 있다.
 *
 * caller: drivers/vfio/container.c:544 의 vfio_device_container_pin_pages() 가
 *   ops->pin_pages 로 부른다. 그 위는 vfio_main.c 의 vfio_pin_pages().
 * callee: vfio_find_dma(), vfio_iova_get_vfio_pfn(), vfio_pin_page_external(),
 *   vfio_add_to_pfn_list(), put_pfn(), vfio_lock_acct(),
 *   vfio_iommu_find_iommu_group(), vfio_unpin_page_external().
 * 에러 경로: 위 9 번의 되감기. 이미 존재하던 외부 핀에 참조만 올린 경우도
 *   vfio_unpin_page_external() 이 참조를 내리므로 정확히 상쇄된다.
 *
 * 호출 체인:
 *   vfio_pin_pages() → vfio_device_container_pin_pages() →
 *   [vfio_iommu_type1_pin_pages] → vfio_pin_page_external()
 */
static int vfio_iommu_type1_pin_pages(void *iommu_data,
				      struct iommu_group *iommu_group,
				      dma_addr_t user_iova,
				      int npage, int prot,
				      struct page **pages)
{
	struct vfio_iommu *iommu = iommu_data;
	/* [한국어] dirty 추적 범위를 승격할 group 래퍼. */
	struct vfio_iommu_group *group;
	/* [한국어] 진행 인덱스, 되감기 인덱스, 결과 코드. */
	int i, j, ret;
	/* [한국어] 핀할 사용자 가상 주소. */
	unsigned long remote_vaddr;
	/* [한국어] 이 페이지를 덮는 매핑. */
	struct vfio_dma *dma;
	/* [한국어] 이 컨테이너에서 과금까지 해야 하는가. */
	bool do_accounting;
	/* [한국어] 범위 오버플로 검사용 임시값. */
	dma_addr_t iova_end;
	/* [한국어] 요청 범위의 바이트 크기. */
	size_t iova_size;

	/* [한국어] 인자 정합성 검사. */
	if (!iommu || !pages || npage <= 0)
		return -EINVAL;

	/* Supported for v2 version only */
	/* [한국어] v1 컨테이너면 */
	if (!iommu->v2)
		/* [한국어] v2 전용 기능이다. */
		return -EACCES;

	/* [한국어] npage * PAGE_SIZE 와 그 끝 주소가 넘치지 않는지 확인한다. 두 임시값은 오버플로 판정에만 쓰인다. */
	if (check_mul_overflow(npage, PAGE_SIZE, &iova_size) ||
	    check_add_overflow(user_iova, iova_size - 1, &iova_end))
		return -EOVERFLOW;

	/* [한국어] 매핑 트리와 pfn 트리를 만지므로 락이 필요하다. */
	mutex_lock(&iommu->lock);

	/* [한국어] vaddr 무효화가 진행 중이면 사용자 주소가 유효하지 않아 핀할 수 없다. */
	if (WARN_ONCE(iommu->vaddr_invalid_count,
		      "vfio_pin_pages not allowed with VFIO_UPDATE_VADDR\n")) {
		ret = -EBUSY;
		/* [한국어] 무효 vaddr 로는 핀할 수 없다. */
		goto pin_done;
	}

	/* Fail if no dma_umap notifier is registered */
	/* [한국어] 언맵 통지를 받을 장치가 하나도 등록되어 있지 않으면 */
	if (list_empty(&iommu->device_list)) {
		/* [한국어] 인자 오류로 보고한다. */
		ret = -EINVAL;
		/* [한국어] 통지 대상 장치가 없으면 나중에 강제 언핀시킬 방법이 없다. */
		goto pin_done;
	}

	/*
	 * If iommu capable domain exist in the container then all pages are
	 * already pinned and accounted. Accounting should be done if there is no
	 * iommu capable domain in the container.
	 */
	/* [한국어] domain 이 없으면 매핑 시점에 핀·과금이 없었으므로 여기서 과금해야 한다. */
	do_accounting = list_empty(&iommu->domain_list);

	/* [한국어] 요청한 페이지 수만큼 반복한다. */
	for (i = 0; i < npage; i++) {
		/* [한국어] 얻은 물리 페이지 프레임 번호. */
		unsigned long phys_pfn;
		/* [한국어] 이번 페이지의 IOVA. */
		dma_addr_t iova;
		/* [한국어] 이미 핀되어 있는지 조회한 결과. */
		struct vfio_pfn *vpfn;

		/* [한국어] 요청 시작에서 i 페이지 떨어진 IOVA. */
		iova = user_iova + PAGE_SIZE * i;
		/* [한국어] 그 한 페이지를 덮는 매핑을 찾는다. */
		dma = vfio_find_dma(iommu, iova, PAGE_SIZE);
		/* [한국어] 매핑이 없으면 번역할 수 없는 주소다. */
		if (!dma) {
			ret = -EINVAL;
			/* [한국어] 권한 부족 — 되감기로. */
			goto pin_unwind;
		}

		/* [한국어] 매핑의 권한이 요청 권한을 모두 포함하지 않으면 거절한다. */
		if ((dma->prot & prot) != prot) {
			ret = -EPERM;
			/* [한국어] 핀 실패 — 되감기로. */
			goto pin_unwind;
		}

		/* [한국어] 이미 외부 핀 기록이 있으면 참조만 올린다. */
		vpfn = vfio_iova_get_vfio_pfn(dma, iova);
		/* [한국어] 있으면 */
		if (vpfn) {
			/* [한국어] 그 페이지를 그대로 돌려주고 다음으로 넘어간다 — GUP 를 다시 부르지 않는다. */
			pages[i] = pfn_to_page(vpfn->pfn);
			continue;
		}

		/* [한국어] 매핑 안의 오프셋이 IOVA 와 vaddr 양쪽에서 같다는 성질로 사용자 주소를 구한다. */
		remote_vaddr = dma->vaddr + (iova - dma->iova);
		/* [한국어] 그 주소의 페이지 한 장을 새로 핀한다. */
		ret = vfio_pin_page_external(dma, remote_vaddr, &phys_pfn,
					     do_accounting);
		/* [한국어] 핀에 실패했으면 되감기로 간다. */
		if (ret)
			goto pin_unwind;

		/* [한국어] mdev 는 struct page 를 필요로 하므로 MMIO 같은 예약 pfn 은 받아들일 수 없다. */
		if (!pfn_valid(phys_pfn)) {
			ret = -EINVAL;
			/* [한국어] struct page 가 없는 메모리는 mdev 가 쓸 수 없다 — 되감기로. */
			goto pin_unwind;
		}

		/* [한국어] 핀한 사실을 pfn 트리에 기록한다. */
		ret = vfio_add_to_pfn_list(dma, iova, phys_pfn);
		/* [한국어] 기록에 실패하면 */
		if (ret) {
			/* [한국어] 방금 핀한 페이지를 되돌리고, 실제로 풀렸고 과금까지 했다면 */
			if (put_pfn(phys_pfn, dma->prot) && do_accounting)
				/* [한국어] 과금도 한 장만큼 되돌린다 — 두 장부를 함께 되감는 지점이다. */
				vfio_lock_acct(dma, -1, true);
			/* [한국어] pfn 트리 기록 실패 — 되감기로. */
			goto pin_unwind;
		}

		/* [한국어] 핀한 페이지를 사용자에게 돌려준다. */
		pages[i] = pfn_to_page(phys_pfn);

		/* [한국어] dirty 추적 중이면 */
		if (iommu->dirty_page_tracking) {
			/* [한국어] 비트 하나가 뜻하는 크기의 시프트 값. */
			unsigned long pgshift = __ffs(iommu->pgsize_bitmap);

			/*
			 * Bitmap populated with the smallest supported page
			 * size
			 */
			/* [한국어] 이 페이지의 비트를 세운다. */
			bitmap_set(dma->bitmap,
				   /* [한국어] 매핑 시작 기준 비트 번호를 계산한다. */
				   (iova - dma->iova) >> pgshift, 1);
		}
	}
	/* [한국어] 성공한 개수를 반환값으로 삼는다. 이 콜백은 개수를 돌려주는 규약이다. */
	ret = i;

	/* [한국어] 이 group 의 dirty 추적 범위를 승격한다. */
	group = vfio_iommu_find_iommu_group(iommu, iommu_group);
	/* [한국어] 아직 승격되지 않았으면 */
	if (!group->pinned_page_dirty_scope) {
		/* [한국어] 이 group 은 핀 인터페이스를 쓴다고 표시하고 */
		group->pinned_page_dirty_scope = true;
		/* [한국어] 추적 못 하는 group 수를 하나 줄여 마이그레이션 보고를 정밀하게 만든다. */
		iommu->num_non_pinned_groups--;
	}

	/* [한국어] 성공했으므로 공통 출구로 간다. */
	goto pin_done;

pin_unwind:
	/* [한국어] 실패한 인덱스의 출력 슬롯을 지운다 — 호출자가 유효한 페이지로 오인하지 않게 한다. */
	pages[i] = NULL;
	/* [한국어] 실패한 인덱스 앞까지 되돌린다. */
	for (j = 0; j < i; j++) {
		/* [한국어] 되돌릴 페이지의 IOVA. */
		dma_addr_t iova;

		/* [한국어] j 번째 페이지의 IOVA. */
		iova = user_iova + PAGE_SIZE * j;
		/* [한국어] 그 매핑을 다시 찾는다. */
		dma = vfio_find_dma(iommu, iova, PAGE_SIZE);
		/* [한국어] 참조를 하나 놓는다 — 새로 핀했든 참조만 올렸든 정확히 상쇄된다. */
		vfio_unpin_page_external(dma, iova, do_accounting);
		/* [한국어] 출력 슬롯도 지운다. */
		pages[j] = NULL;
	}
pin_done:
	/* [한국어] 락을 놓는다. 성공·실패 경로가 모두 여기로 모인다. */
	mutex_unlock(&iommu->lock);
	return ret;
}

/* [한국어]
 * vfio_iommu_type1_unpin_pages - mdev 가 핀했던 IOVA 범위의 페이지들을 놓는다
 *
 * @iommu_data: 컨테이너 객체.
 * @user_iova: 놓을 범위의 시작 IOVA.
 * @npage: 페이지 수.
 * @return: 없음 — 실패해도 알릴 방법이 없으므로 WARN_ON 으로만 드러낸다.
 *
 * vfio_iommu_driver_ops 의 unpin_pages 콜백 구현이며 pin_pages 의 짝이다. 반환형이
 * void 인 것은 이 API 의 규약이 '핀한 만큼 반드시 놓는다' 이기 때문이다 — 놓을 것이
 * 없다면 그것은 호출자의 버그이므로 경고만 남긴다.
 *
 * 동작 단계:
 *  1. v2 가 아니거나 npage <= 0 이면 WARN_ON 을 내고 그대로 돌아간다.
 *  2. 범위 계산의 오버플로도 WARN_ON 대상이다.
 *  3. iommu->lock 을 잡는다.
 *  4. do_accounting 을 pin 때와 같은 기준(domain 이 없는가)으로 정한다. 두 함수가
 *     같은 기준을 써야 과금이 정확히 상쇄된다.
 *  5. 페이지마다 vfio_find_dma() 로 매핑을 찾아 vfio_unpin_page_external() 을 부른다.
 *     매핑을 못 찾으면 break 한다 — 이미 언맵된 것이므로 더 볼 필요가 없다.
 *  6. 락을 놓고, 요청한 만큼 처리하지 못했으면 WARN_ON 으로 알린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * caller: drivers/vfio/container.c:556 의 vfio_device_container_unpin_pages().
 *   그 위는 vfio_main.c 의 vfio_unpin_pages().
 * callee: vfio_find_dma(), vfio_unpin_page_external().
 * 에러 경로: 오류를 반환할 수 없으므로 WARN_ON 으로 개발자에게 알리는 것이 전부다.
 *
 * 호출 체인:
 *   vfio_unpin_pages() → vfio_device_container_unpin_pages() →
 *   [vfio_iommu_type1_unpin_pages] → vfio_unpin_page_external()
 */
static void vfio_iommu_type1_unpin_pages(void *iommu_data,
					 dma_addr_t user_iova, int npage)
{
	struct vfio_iommu *iommu = iommu_data;
	/* [한국어] 이 컨테이너에서 과금까지 되돌려야 하는가. */
	bool do_accounting;
	/* [한국어] 범위 오버플로 검사용 임시값. */
	dma_addr_t iova_end;
	/* [한국어] 요청 범위의 바이트 크기. */
	size_t iova_size;
	/* [한국어] 처리한 페이지 수. */
	int i;

	/* Supported for v2 version only */
	/* [한국어] 이 콜백은 v2 전용이다. v1 이면 호출자의 버그다. */
	if (WARN_ON(!iommu->v2))
		return;

	/* [한국어] 0 이하 개수는 호출자의 버그다. */
	if (WARN_ON(npage <= 0))
		return;

	/* [한국어] 범위 계산이 넘치는 것도 호출자의 버그다. */
	if (WARN_ON(check_mul_overflow(npage, PAGE_SIZE, &iova_size) ||
		    check_add_overflow(user_iova, iova_size - 1, &iova_end)))
		return;

	/* [한국어] 매핑 트리를 만지므로 락이 필요하다. */
	mutex_lock(&iommu->lock);

	/* [한국어] 핀할 때와 같은 기준으로 정해야 과금이 정확히 상쇄된다. */
	do_accounting = list_empty(&iommu->domain_list);
	/* [한국어] 페이지마다 처리한다. */
	for (i = 0; i < npage; i++) {
		/* [한국어] 이번 페이지의 IOVA. */
		dma_addr_t iova = user_iova + PAGE_SIZE * i;
		/* [한국어] 그 페이지를 덮는 매핑. */
		struct vfio_dma *dma;

		/* [한국어] 매핑을 찾는다. */
		dma = vfio_find_dma(iommu, iova, PAGE_SIZE);
		/* [한국어] 이미 언맵되었으면 더 볼 것이 없다. */
		if (!dma)
			break;

		/* [한국어] 참조를 하나 놓는다. 0 이 되면 실제 핀이 풀린다. */
		vfio_unpin_page_external(dma, iova, do_accounting);
	}

	/* [한국어] 락을 놓는다. */
	mutex_unlock(&iommu->lock);

	/* [한국어] 요청한 수만큼 처리하지 못했으면 호출자가 이미 언맵된 범위를 놓으려 한 것이다. */
	WARN_ON(i != npage);
}

/* [한국어]
 * vfio_sync_unpin - IOTLB 를 비운 뒤, 미뤄 두었던 구간들의 핀을 푼다
 *
 * @dma: 대상 매핑.
 * @domain: IOTLB 를 비울 IOMMU domain.
 * @regions: unmap_unpin_fast() 가 쌓아 둔 struct vfio_regions 목록.
 * @iotlb_gather: 어느 범위를 비워야 하는지 모아 둔 정보.
 * @return: 실제로 언핀한 페이지 수의 합.
 *
 * 왜 이 순서여야 하는가: iommu_unmap_fast() 는 페이지 테이블만 지우고 IOTLB(장치
 * 측 주소 변환 캐시)는 비우지 않는다. 그 상태에서 페이지를 언핀해 커널이 다른
 * 용도로 재사용하면, 장치가 아직 남아 있는 IOTLB 항목으로 그 페이지에 DMA 할 수
 * 있다. 그래서 반드시 IOTLB 플러시를 먼저 끝내고 나서 언핀해야 한다.
 *
 * 동작 단계:
 *  1. iommu_iotlb_sync() 로 모아 둔 범위의 IOTLB 를 실제로 비운다.
 *  2. 목록의 각 구간에 대해 vfio_unpin_pages_remote() 를 do_accounting=false 로
 *     부른다 — 과금은 호출자가 총합을 모아 마지막에 한 번에 되돌린다.
 *  3. 목록에서 빼고 항목을 해제한다.
 *  4. cond_resched() 로 CPU 를 양보한다. 큰 매핑 해제가 다른 태스크를 굶기지 않게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: unmap_unpin_fast()(목록이 한계에 닿았거나 실패했을 때),
 *   vfio_unmap_unpin()(루프가 끝난 뒤 남은 것 처리).
 * callee: iommu_iotlb_sync(), vfio_unpin_pages_remote(), kfree(), cond_resched().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_unmap_unpin() → [vfio_sync_unpin] → vfio_unpin_pages_remote()
 */
static long vfio_sync_unpin(struct vfio_dma *dma, struct vfio_domain *domain,
			    struct list_head *regions,
			    struct iommu_iotlb_gather *iotlb_gather)
{
	long unlocked = 0;
	/* [한국어] 대기 목록 안전 순회용 커서 쌍 — 순회 도중 항목을 해제한다. */
	struct vfio_regions *entry, *next;

	/* [한국어] 모아 둔 범위의 IOTLB 를 실제로 비운다. 이 호출이 끝나야 페이지를 재사용시켜도 안전하다. */
	iommu_iotlb_sync(domain->domain, iotlb_gather);

	/* [한국어] 대기하던 구간들을 하나씩 처리한다. */
	list_for_each_entry_safe(entry, next, regions, list) {
		/* [한국어] 이제 IOTLB 가 깨끗하므로 핀을 풀어도 된다. 과금은 호출자가 총합으로 처리하도록 끈다. */
		unlocked += vfio_unpin_pages_remote(dma,
						    entry->iova,
						    entry->phys >> PAGE_SHIFT,
						    entry->len >> PAGE_SHIFT,
						    false);
		/* [한국어] 대기 목록에서 뺀다. */
		list_del(&entry->list);
		/* [한국어] 기록 항목을 해제한다. */
		kfree(entry);
	}

	/* [한국어] 다음 구간 처리 전에 CPU 를 양보한다. */
	cond_resched();

	/* [한국어] 언핀한 총 페이지 수를 호출자에게 돌려준다. */
	return unlocked;
}

/*
 * Generally, VFIO needs to unpin remote pages after each IOTLB flush.
 * Therefore, when using IOTLB flush sync interface, VFIO need to keep track
 * of these regions (currently using a list).
 *
 * This value specifies maximum number of regions for each IOTLB flush sync.
 */
#define VFIO_IOMMU_TLB_SYNC_MAX		512

/* [한국어]
 * unmap_unpin_fast - IOTLB 플러시를 미루는 빠른 언맵을 시도한다
 *
 * @domain: 대상 IOMMU domain.
 * @dma: 대상 매핑.
 * @iova: 언맵할 구간의 시작 IOVA.
 * @len: 언맵할 길이.
 * @phys: 그 구간이 가리키던 호스트 물리 주소.
 * @unlocked: 언핀한 페이지 수를 누적할 곳(호출자 변수).
 * @unmapped_list: 플러시 대기 구간 목록.
 * @unmapped_cnt: 그 목록의 항목 수(호출자 변수).
 * @iotlb_gather: 플러시 범위 누적 구조체.
 * @return: 실제로 언맵된 바이트 수. 0 이면 호출자가 느린 경로로 재시도해야 한다.
 *
 * IOTLB 플러시는 비싸다. 구간마다 플러시하는 대신 여러 구간을 모아 한 번에
 * 비우면 큰 매핑 해제가 훨씬 빨라진다. 다만 플러시 전에는 언핀할 수 없으므로,
 * '무엇을 언핀해야 하는지' 를 목록에 적어 두어야 한다.
 *
 * 동작 단계:
 *  1. 기록용 vfio_regions 항목을 할당한다. 실패하면 아래 검사에서 unmapped 가 0 인
 *     채로 남아 호출자가 느린 경로를 쓰게 된다 — 할당 실패가 곧바로 오류가 되지
 *     않고 성능 저하로만 처리되는 구조다.
 *  2. 할당에 성공했으면 iommu_unmap_fast() 로 페이지 테이블만 지운다.
 *  3. 반환이 0 이면(아무것도 못 지웠으면) 항목을 해제한다.
 *  4. 아니면 iova/phys/실제 언맵 길이를 채워 목록 끝에 달고 개수를 올린다.
 *     요청한 len 이 아니라 반환된 unmapped 를 적는 것이 중요하다 — IOMMU 가 요청보다
 *     적게 지울 수 있고, 언핀 범위는 실제로 지운 만큼이어야 한다.
 *  5. 목록이 VFIO_IOMMU_TLB_SYNC_MAX(512)에 닿았거나 이번에 아무것도 못 지웠으면
 *     vfio_sync_unpin() 으로 지금까지 모은 것을 플러시하고 언핀한다. 목록 길이를
 *     제한하는 이유는 미뤄 둔 항목이 무한히 쌓여 메모리를 먹는 것을 막기 위해서다.
 *  6. 실제 언맵된 바이트 수를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_unmap_unpin() 한 곳뿐.
 * callee: kzalloc_obj(), iommu_unmap_fast(), kfree(), vfio_sync_unpin().
 * 에러 경로: 0 을 반환하는 것이 곧 '느린 경로로 가라' 는 신호다. 오류 코드를 쓰지 않는다.
 *
 * 호출 체인:
 *   vfio_remove_dma() → vfio_unmap_unpin() → [unmap_unpin_fast] → iommu_unmap_fast()
 */
static size_t unmap_unpin_fast(struct vfio_domain *domain,
			       struct vfio_dma *dma, dma_addr_t iova,
			       size_t len, phys_addr_t phys, long *unlocked,
			       struct list_head *unmapped_list,
			       int *unmapped_cnt,
			       struct iommu_iotlb_gather *iotlb_gather)
{
	size_t unmapped = 0;
	/* [한국어] 플러시 대기 목록에 넣을 기록 항목을 할당한다. 실패해도 오류가 아니라 느린 경로로 넘어가는 신호가 된다. */
	struct vfio_regions *entry = kzalloc_obj(*entry);

	/* [한국어] 할당에 성공했을 때만 빠른 언맵을 시도한다. */
	if (entry) {
		/* [한국어] 페이지 테이블만 지우고 IOTLB 플러시는 미룬다. 플러시 범위는 iotlb_gather 에 누적된다. */
		unmapped = iommu_unmap_fast(domain->domain, iova, len,
					    iotlb_gather);

		/* [한국어] 아무것도 못 지웠으면 */
		if (!unmapped) {
			kfree(entry);
		} else {
			/* [한국어] 언핀할 때 필요한 시작 IOVA 를 적어 둔다. */
			entry->iova = iova;
			/* [한국어] 그 구간의 물리 주소도 적어 둔다. */
			entry->phys = phys;
			/* [한국어] 요청 길이가 아니라 실제로 지워진 길이를 적는다 — 언핀 범위가 이 값이어야 한다. */
			entry->len  = unmapped;
			/* [한국어] 대기 목록 끝에 단다. */
			list_add_tail(&entry->list, unmapped_list);

			/* [한국어] 대기 항목 수를 올린다. 한계에 닿으면 아래에서 플러시가 일어난다. */
			(*unmapped_cnt)++;
		}
	}

	/*
	 * Sync if the number of fast-unmap regions hits the limit
	 * or in case of errors.
	 */
	/* [한국어] 대기 항목이 한계에 닿았거나 이번에 아무것도 못 지웠으면 지금 플러시한다. 한계를 두는 것은 대기 목록이 무한히 쌓이는 것을 막기 위해서다. */
	if (*unmapped_cnt >= VFIO_IOMMU_TLB_SYNC_MAX || !unmapped) {
		*unlocked += vfio_sync_unpin(dma, domain, unmapped_list,
					     /* [한국어] 누적된 플러시 범위를 넘긴다. */
					     iotlb_gather);
		*unmapped_cnt = 0;
	}

	/* [한국어] 실제 언맵된 바이트 수를 돌려준다. 0 이면 호출자가 느린 경로로 재시도한다. */
	return unmapped;
}

/* [한국어]
 * unmap_unpin_slow - 구간마다 즉시 플러시하는 느린 언맵을 한다
 *
 * @domain: 대상 IOMMU domain.
 * @dma: 대상 매핑.
 * @iova: 언맵할 구간의 시작 IOVA.
 * @len: 언맵할 길이.
 * @phys: 그 구간이 가리키던 호스트 물리 주소.
 * @unlocked: 언핀한 페이지 수를 누적할 곳.
 * @return: 실제로 언맵된 바이트 수.
 *
 * unmap_unpin_fast() 가 실패했을 때의 대체 경로다. iommu_unmap() 은 반환 전에
 * IOTLB 플러시까지 끝내 주므로, 돌아온 직후 곧바로 언핀해도 안전하다. 대기 목록도
 * 필요 없어 메모리 할당 실패의 영향을 받지 않는다.
 *
 * 동작 단계:
 *  1. iommu_unmap() 으로 페이지 테이블을 지우고 IOTLB 까지 동기적으로 비운다.
 *  2. 실제로 지워진 만큼만 vfio_unpin_pages_remote() 로 언핀한다.
 *     do_accounting=false 이므로 과금 반환은 호출자가 모아서 한다.
 *  3. cond_resched() 로 CPU 를 양보한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_unmap_unpin() 한 곳뿐.
 * callee: iommu_unmap(), vfio_unpin_pages_remote(), cond_resched().
 * 에러 경로: 0 을 반환하면 호출자가 WARN_ON 을 내고 루프를 중단한다 —
 *   진행이 없으면 무한 루프가 되기 때문이다.
 *
 * 호출 체인:
 *   vfio_remove_dma() → vfio_unmap_unpin() → [unmap_unpin_slow] → iommu_unmap()
 */
static size_t unmap_unpin_slow(struct vfio_domain *domain,
			       struct vfio_dma *dma, dma_addr_t iova,
			       size_t len, phys_addr_t phys,
			       long *unlocked)
{
	size_t unmapped = iommu_unmap(domain->domain, iova, len);

	/* [한국어] 실제로 지워진 만큼만 언핀한다. iommu_unmap 은 반환 전에 IOTLB 플러시까지 끝내므로 곧바로 풀어도 안전하다. */
	if (unmapped) {
		*unlocked += vfio_unpin_pages_remote(dma, iova,
						     /* [한국어] 물리 주소를 pfn 으로 내리고 */
						     phys >> PAGE_SHIFT,
						     /* [한국어] 길이도 페이지 수로 내려 넘긴다. */
						     unmapped >> PAGE_SHIFT,
						     /* [한국어] 과금은 호출자가 총합으로 처리하므로 여기서는 끈다. */
						     false);
		/* [한국어] 큰 매핑 해제가 다른 태스크를 굶기지 않도록 CPU 를 양보한다. */
		cond_resched();
	}
	/* [한국어] 실제 언맵된 바이트 수를 돌려준다. 0 이면 호출자가 진행 불가로 판단한다. */
	return unmapped;
}

/* [한국어]
 * vfio_unmap_unpin - 매핑 하나를 모든 domain 에서 걷어내고 페이지 핀을 푼다
 *
 * @iommu: 컨테이너 객체.
 * @dma: 걷어낼 매핑.
 * @do_accounting: true 면 이 함수가 과금을 되돌리고 0 을 반환한다. false 면
 *                 언핀한 페이지 수를 반환해 호출자가 나중에 처리하게 한다.
 * @return: do_accounting 이 false 일 때 언핀한 페이지 수, true 면 0.
 *
 * 핀을 풀려면 그 페이지의 물리 주소를 알아야 하는데, 이 파일은 pfn 목록을 따로
 * 보관하지 않는다. 상류 주석이 설명하듯 물리 주소 추적을 IOMMU 페이지 테이블에
 * 맡기고 있기 때문이다. 그래서 domain 하나를 '조회용' 으로 남겨 두고 나머지
 * domain 은 먼저 전부 언맵한 뒤, 남은 domain 에 iommu_iova_to_phys() 로 물어보면서
 * 언맵과 언핀을 함께 진행한다.
 *
 * 동작 단계:
 *  1. 크기가 0 이거나 domain 이 하나도 없으면 할 일이 없다.
 *  2. 첫 domain 을 조회용으로 정하고, 두 번째부터는 곧바로 iommu_unmap() 으로 지운다.
 *     이렇게 해야 페이지를 언핀하는 시점에 어느 domain 에도 번역이 남지 않는다.
 *  3. iotlb_gather 를 초기화하고 매핑 전체를 pos 로 훑는다.
 *     - iommu_iova_to_phys() 로 현재 위치의 물리 주소를 얻는다. 0 이면 WARN_ON 을
 *       내고 한 페이지 건너뛴다 — 있어야 할 번역이 없다는 뜻이다.
 *     - 물리적으로 이어지는 최대 길이를 찾는다. iommu_unmap() 호출 한 번마다 하드웨어
 *       캐시 플러시가 따를 수 있으므로 호출 횟수를 줄이는 것이 이득이다.
 *     - 먼저 unmap_unpin_fast() 를 시도하고, 0 이면 unmap_unpin_slow() 로 재시도한다.
 *       둘 다 0 이면 진행이 없으므로 WARN_ON 을 내고 루프를 벗어난다.
 *     - 실제 언맵된 만큼 pos 를 전진시킨다.
 *  4. iommu_mapped 를 false 로 내린다.
 *  5. 대기 목록에 남은 것이 있으면 vfio_sync_unpin() 으로 마무리한다.
 *  6. do_accounting 이면 모아 둔 언핀 수만큼 과금을 되돌리고 0 을 반환한다.
 *     async=true 인 이유는 컨테이너 해제 시 소유 프로세스가 이미 죽었을 수 있어서다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_remove_dma()(do_accounting=true),
 *   vfio_iommu_unmap_unpin_reaccount()(false — 외부 핀 수를 따로 세어 합산한다).
 * callee: iommu_unmap(), iommu_iotlb_gather_init(), iommu_iova_to_phys(),
 *   unmap_unpin_fast(), unmap_unpin_slow(), vfio_sync_unpin(), vfio_lock_acct(),
 *   cond_resched().
 * 에러 경로: 오류를 반환하지 않는다. 이상은 WARN_ON 으로만 드러낸다.
 *
 * 호출 체인:
 *   vfio_dma_do_unmap() → vfio_remove_dma() → [vfio_unmap_unpin] → unmap_unpin_fast()
 */
static long vfio_unmap_unpin(struct vfio_iommu *iommu, struct vfio_dma *dma,
			     bool do_accounting)
{
	struct vfio_domain *domain, *d;
	/* [한국어] 플러시 대기 구간 목록. 스택에 만든다. */
	LIST_HEAD(unmapped_region_list);
	/* [한국어] IOTLB 플러시 범위를 모아 두는 구조체. */
	struct iommu_iotlb_gather iotlb_gather;
	/* [한국어] 플러시 대기 중인 구간 수. */
	int unmapped_region_cnt = 0;
	/* [한국어] 지금까지 언핀한 페이지 수의 합. */
	long unlocked = 0;
	/* [한국어] 매핑 안에서의 진행 위치. */
	size_t pos = 0;

	/* [한국어] 크기 0 매핑은 걷어낼 것이 없다. */
	if (!dma->size)
		return 0;

	/* [한국어] domain 이 하나도 없으면 IOMMU 에 설치된 것이 없다. */
	if (list_empty(&iommu->domain_list))
		return 0;

	/*
	 * We use the IOMMU to track the physical addresses, otherwise we'd
	 * need a much more complicated tracking system.  Unfortunately that
	 * means we need to use one of the iommu domains to figure out the
	 * pfns to unpin.  The rest need to be unmapped in advance so we have
	 * no iommu translations remaining when the pages are unpinned.
	 */
	/* [한국어] 첫 domain 을 물리 주소 조회용으로 남긴다. */
	domain = d = list_first_entry(&iommu->domain_list,
				      /* [한국어] 목록의 첫 원소를 구조체로 복원한다. */
				      struct vfio_domain, next);

	/* [한국어] 두 번째 domain 부터는 곧바로 전부 언맵한다 — 첫 domain 만 물리 주소 조회용으로 남긴다. */
	list_for_each_entry_continue(d, &iommu->domain_list, next) {
		/* [한국어] 번역을 통째로 걷어낸다. 이래야 페이지를 언핀할 때 어느 domain 에도 번역이 남지 않는다. */
		iommu_unmap(d->domain, dma->iova, dma->size);
		/* [한국어] domain 이 많으면 이 루프가 길어질 수 있으므로 CPU 를 양보한다. */
		cond_resched();
	}

	/* [한국어] 플러시 범위 누적 구조체를 초기화한다. */
	iommu_iotlb_gather_init(&iotlb_gather);
	/* [한국어] 조회용 domain 을 보며 매핑 전체를 훑는다. */
	while (pos < dma->size) {
		/* [한국어] 이번에 실제로 언맵된 길이와, 언맵을 시도할 길이. */
		size_t unmapped, len;
		/* [한국어] 이 위치의 물리 주소와 연속 탐색용 임시값. */
		phys_addr_t phys, next;
		/* [한국어] 이번에 처리할 IOVA. */
		dma_addr_t iova = dma->iova + pos;

		/* [한국어] 물리 주소를 IOMMU 에게 묻는다 — 이 파일은 pfn 목록을 따로 보관하지 않는다. */
		phys = iommu_iova_to_phys(domain->domain, iova);
		/* [한국어] 있어야 할 번역이 없으면 경고하고 */
		if (WARN_ON(!phys)) {
			/* [한국어] 한 페이지 건너뛴다. */
			pos += PAGE_SIZE;
			continue;
		}

		/*
		 * To optimize for fewer iommu_unmap() calls, each of which
		 * may require hardware cache flushing, try to find the
		 * largest contiguous physical memory chunk to unmap.
		 */
		/* [한국어] 물리적으로 이어지는 최대 길이를 찾는다 — iommu_unmap 호출마다 하드웨어 캐시 플러시가 따를 수 있어 호출 횟수를 줄이는 것이 이득이다. */
		for (len = PAGE_SIZE; pos + len < dma->size; len += PAGE_SIZE) {
			/* [한국어] 다음 위치의 물리 주소를 얻어 */
			next = iommu_iova_to_phys(domain->domain, iova + len);
			/* [한국어] 연속이 끊기면 거기까지가 한 번에 언맵할 구간이다. */
			if (next != phys + len)
				break;
		}

		/*
		 * First, try to use fast unmap/unpin. In case of failure,
		 * switch to slow unmap/unpin path.
		 */
		/* [한국어] 먼저 IOTLB 플러시를 미루는 빠른 경로를 시도한다. */
		unmapped = unmap_unpin_fast(domain, dma, iova, len, phys,
					    /* [한국어] 언핀 수 누적 변수와 플러시 대기 목록을 넘기고 */
					    &unlocked, &unmapped_region_list,
					    /* [한국어] 대기 항목 수도 넘긴다. */
					    &unmapped_region_cnt,
					    /* [한국어] 플러시 범위 누적 구조체를 넘긴다. */
					    &iotlb_gather);
		/* [한국어] 빠른 경로가 아무것도 못 지웠으면 */
		if (!unmapped) {
			/* [한국어] 구간마다 즉시 플러시하는 느린 경로로 재시도한다. */
			unmapped = unmap_unpin_slow(domain, dma, iova, len,
						    phys, &unlocked);
			/* [한국어] 그것마저 0 이면 진행이 없어 무한 루프가 되므로 중단한다. */
			if (WARN_ON(!unmapped))
				break;
		}

		/* [한국어] 실제 언맵된 만큼 전진한다 — 요청 길이가 아니라 반환값을 쓰는 것이 중요하다. */
		pos += unmapped;
	}

	/* [한국어] 이제 이 매핑은 어느 domain 에도 설치되어 있지 않다. */
	dma->iommu_mapped = false;

	/* [한국어] 플러시 대기 구간이 남아 있으면 */
	if (unmapped_region_cnt) {
		/* [한국어] 마지막으로 IOTLB 를 비우고 그 구간들을 언핀한다. */
		unlocked += vfio_sync_unpin(dma, domain, &unmapped_region_list,
					    &iotlb_gather);
	}

	/* [한국어] 과금 처리를 이 함수가 맡았으면 */
	if (do_accounting) {
		/* [한국어] 푼 만큼 되돌린다. async=true 인 이유는 소유 프로세스가 이미 죽었을 수 있어서다. */
		vfio_lock_acct(dma, -unlocked, true);
		return 0;
	}
	/* [한국어] 언핀한 페이지 수를 호출자에게 돌려준다. 호출자가 과금을 모아서 처리한다. */
	return unlocked;
}

/* [한국어]
 * vfio_remove_dma - 매핑 하나를 완전히 없앤다(언맵·언핀·과금 반환·해제)
 *
 * @iommu: 컨테이너 객체.
 * @dma: 없앨 매핑.
 * @return: 없음.
 *
 * vfio_dma_do_map() 이 만든 모든 것을 정확히 역순으로 되돌리는 함수다. 여기서
 * 하나라도 빠뜨리면 그것이 곧 누수다.
 *
 * 동작 단계:
 *  1. WARN_ON(!RB_EMPTY_ROOT(&dma->pfn_list)) — 외부 핀이 남아 있으면 안 된다.
 *     남아 있는데도 지우면 mdev 가 나중에 언핀하려 할 때 이미 없는 매핑을 찾게 된다.
 *     호출자 vfio_dma_do_unmap() 이 미리 vfio_notify_dma_unmap() 으로 비워 둔다.
 *  2. vfio_unmap_unpin(do_accounting=true) — IOMMU 매핑을 걷고, 핀을 풀고,
 *     잠금 메모리 과금을 되돌린다. 세 장부가 여기서 한꺼번에 정리된다.
 *  3. vfio_unlink_dma() — rb-tree 에서 뺀다.
 *  4. put_task_struct() — vfio_dma_do_map() 의 get_task_struct() 짝.
 *  5. mmdrop() — vfio_dma_do_map() 의 mmgrab() 짝.
 *  6. vfio_dma_bitmap_free() — dirty 비트맵 해제.
 *  7. vaddr_invalid 였다면 컨테이너의 무효 카운트를 줄인다. 이걸 빠뜨리면 카운트가
 *     영원히 0 으로 돌아오지 않아 이후 모든 핀/rw 요청이 -EBUSY 가 된다.
 *  8. 구조체를 해제하고 dma_avail 을 하나 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_dma_do_unmap()(사용자 요청), vfio_pin_map_dma() 와 vfio_dma_do_map()
 *   의 실패 되감기, vfio_iommu_unmap_unpin_all()(컨테이너 정리).
 * callee: vfio_unmap_unpin(), vfio_unlink_dma(), put_task_struct(), mmdrop(),
 *   vfio_dma_bitmap_free(), kfree().
 * 에러 경로: 없다. 실패할 수 없는 정리 경로다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_unmap_dma() → vfio_dma_do_unmap() → [vfio_remove_dma] →
 *   vfio_unmap_unpin()
 */
static void vfio_remove_dma(struct vfio_iommu *iommu, struct vfio_dma *dma)
{
	WARN_ON(!RB_EMPTY_ROOT(&dma->pfn_list));
	/* [한국어] IOMMU 매핑을 걷고, 핀을 풀고, 잠금 메모리 과금을 되돌린다. 세 장부가 여기서 한꺼번에 정리된다. */
	vfio_unmap_unpin(iommu, dma, true);
	/* [한국어] rb-tree 에서 뺀다. */
	vfio_unlink_dma(iommu, dma);
	/* [한국어] 회계 주체 task 참조를 놓는다. vfio_dma_do_map() 의 get_task_struct 짝이다. */
	put_task_struct(dma->task);
	/* [한국어] 주소 공간 구조체 참조를 놓는다. vfio_dma_do_map() 의 mmgrab 짝이다. */
	mmdrop(dma->mm);
	/* [한국어] dirty 비트맵을 해제한다. */
	vfio_dma_bitmap_free(dma);
	/* [한국어] 무효 표시가 있었으면 */
	if (dma->vaddr_invalid)
		/* [한국어] 컨테이너 카운터도 되돌린다. 빠뜨리면 0 으로 돌아오지 않아 이후 모든 핀/rw 가 -EBUSY 가 된다. */
		iommu->vaddr_invalid_count--;
	/* [한국어] 매핑 객체를 해제한다. */
	kfree(dma);
	/* [한국어] 매핑 슬롯을 하나 돌려준다. vfio_dma_do_map() 의 감소와 짝이다. */
	iommu->dma_avail++;
}

/* [한국어]
 * vfio_update_pgsize_bitmap - 컨테이너가 지원하는 IOMMU 페이지 크기 집합을 다시 계산한다
 *
 * @iommu: 컨테이너 객체.
 * @return: 없음.
 *
 * 컨테이너에 여러 IOMMU domain 이 붙을 수 있고, 각 domain 이 지원하는 페이지 크기가
 * 다를 수 있다. 사용자에게 약속할 수 있는 것은 그 교집합뿐이므로 domain 이 추가되거나
 * 제거될 때마다 다시 계산한다.
 *
 * 동작 단계:
 *  1. ULONG_MAX(모든 비트 1)로 시작해 모든 domain 의 pgsize_bitmap 을 AND 한다 —
 *     교집합을 구하는 표준 관용구다.
 *  2. 상류 주석이 설명하듯, IOMMU 가 PAGE_SIZE 보다 작은 페이지를 지원하더라도
 *     사용자에게는 숨긴다. 핀은 PAGE_SIZE 단위로만 할 수 있고, 사용자에게
 *     sub-page 정렬을 허용하면 핀 단위와 매핑 단위가 어긋나기 때문이다.
 *     그래서 PAGE_MASK 로 하위 비트를 지운 뒤 PAGE_SIZE 비트를 다시 세운다.
 *     IOMMU 드라이버는 내부적으로 더 작은 크기를 써도 무방하다.
 * 이 값의 __ffs() 가 이 파일 전체에서 정렬 검사와 dirty 비트맵 단위로 쓰이는
 * '최소 지원 페이지 크기' 다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_attach_group()(새 domain 추가 후),
 *   vfio_iommu_type1_detach_group()(domain 제거 후).
 * callee: 없다. 목록 순회와 비트 연산뿐이다.
 * 에러 경로: 없다. domain 이 하나도 없으면 ULONG_MAX 가 남았다가 PAGE_MASK 로
 *   다듬어지는데, 그 상태에서는 매핑도 만들어지지 않으므로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_update_pgsize_bitmap]
 */
static void vfio_update_pgsize_bitmap(struct vfio_iommu *iommu)
{
	struct vfio_domain *domain;

	/* [한국어] 모든 비트를 1 로 시작한다 — 교집합을 구하는 표준 관용구다. */
	iommu->pgsize_bitmap = ULONG_MAX;

	/* [한국어] 모든 domain 의 지원 크기와 */
	list_for_each_entry(domain, &iommu->domain_list, next)
		/* [한국어] AND 를 취해 공통으로 지원하는 것만 남긴다. */
		iommu->pgsize_bitmap &= domain->domain->pgsize_bitmap;

	/*
	 * In case the IOMMU supports page sizes smaller than PAGE_SIZE
	 * we pretend PAGE_SIZE is supported and hide sub-PAGE_SIZE sizes.
	 * That way the user will be able to map/unmap buffers whose size/
	 * start address is aligned with PAGE_SIZE. Pinning code uses that
	 * granularity while iommu driver can use the sub-PAGE_SIZE size
	 * to map the buffer.
	 */
	/* [한국어] PAGE_SIZE 보다 작은 크기를 지원한다는 비트가 남아 있으면 다듬어야 한다. */
	if (iommu->pgsize_bitmap & ~PAGE_MASK) {
		/* [한국어] PAGE_SIZE 미만 비트를 지운다. 핀은 PAGE_SIZE 단위로만 할 수 있어 사용자에게 더 작은 정렬을 허용하면 안 된다. */
		iommu->pgsize_bitmap &= PAGE_MASK;
		/* [한국어] PAGE_SIZE 비트를 다시 세운다. IOMMU 드라이버는 내부적으로 더 작은 크기를 써도 무방하다. */
		iommu->pgsize_bitmap |= PAGE_SIZE;
	}
}

/* [한국어]
 * update_user_bitmap - 매핑 하나의 dirty 비트맵을 사용자 버퍼의 올바른 비트 위치로 복사한다
 *
 * @bitmap: 사용자 공간의 u64 배열 시작 주소.
 * @iommu: 컨테이너 객체. dirty 추적 정밀도 판단에 쓴다.
 * @dma: 복사할 매핑.
 * @base_iova: 사용자가 요청한 전체 범위의 시작 IOVA. 비트 오프셋의 기준점이다.
 * @pgsize: 비트 하나가 뜻하는 바이트 크기.
 * @return: 0 성공, -EFAULT(사용자 버퍼 접근 실패).
 *
 * 사용자는 여러 매핑을 덮는 넓은 범위 하나에 대해 비트맵을 요청할 수 있다. 그러면
 * 각 매핑의 비트열을 전체 범위 기준의 정확한 비트 위치에 얹어야 하는데, 그 위치가
 * 64비트 워드 경계에 맞지 않을 수 있다. 이 함수가 그 자리 맞추기를 한다.
 *
 * 동작 단계와 산술:
 *  1. pgshift = __ffs(pgsize) — 나눗셈을 시프트로 바꾸기 위한 값.
 *  2. nbits = dma->size >> pgshift — 이 매핑이 차지하는 비트 수.
 *  3. bit_offset = (dma->iova - base_iova) >> pgshift — 전체 범위 시작부터 이 매핑까지의
 *     비트 거리.
 *  4. copy_offset = bit_offset / BITS_PER_LONG — 그 비트가 몇 번째 워드에 있는지.
 *     사용자 버퍼는 u64 배열이므로 이 값이 곧 배열 인덱스다.
 *  5. shift = bit_offset % BITS_PER_LONG — 워드 안에서의 비트 어긋남.
 *  6. num_non_pinned_groups 가 0 이 아니고 이 매핑이 IOMMU 에 설치되어 있으면,
 *     dirty 를 보고할 수 없는 장치가 있다는 뜻이므로 매핑 전체를 dirty 로 칠한다.
 *     데이터를 잃느니 과하게 보고하는 쪽을 택하는 보수적 선택이다.
 *  7. shift 가 0 이 아니면 자리 맞추기가 필요하다.
 *     - bitmap_shift_left() 로 비트열을 shift 만큼 왼쪽으로 민다. 길이를 nbits+shift 로
 *       주는 이유는 밀려 나갈 자리가 필요하기 때문이고, 그 여유분이 바로
 *       vfio_dma_bitmap_alloc() 이 추가로 잡아 둔 64비트다.
 *     - 그 워드에는 앞선 매핑이 이미 쓴 비트가 들어 있을 수 있으므로,
 *       copy_from_user() 로 그 한 워드를 읽어 와 bitmap_or() 로 하위 shift 비트를
 *       합친다. 이렇게 해야 앞 매핑의 결과를 덮어쓰지 않는다.
 *  8. copy_to_user() 로 nbits+shift 비트를 담는 바이트 수만큼 사용자 버퍼에 쓴다.
 *     바이트 수는 DIRTY_BITMAP_BYTES() 가 64비트 단위로 올림해 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 사용자 메모리 접근이 있어
 * 페이지 폴트로 잠들 수 있다.
 *
 * caller: vfio_iova_dirty_bitmap()(GET_BITMAP ioctl),
 *   vfio_dma_do_unmap()(언맵과 동시에 비트맵을 가져가는 플래그가 켜졌을 때).
 * callee: __ffs(), bitmap_set(), bitmap_shift_left(), copy_from_user(), bitmap_or(),
 *   copy_to_user().
 * 에러 경로: 사용자 버퍼 접근 실패 시 -EFAULT. 이 함수는 dma->bitmap 을 밀어 놓은
 *   상태로 반환하지만, 호출자가 곧 오류를 사용자에게 올리고 정상 경로에서는
 *   비트맵을 다시 비우고 채우므로 문제가 되지 않는다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_dirty_pages() → vfio_iova_dirty_bitmap() → [update_user_bitmap] →
 *   copy_to_user()
 */
static int update_user_bitmap(u64 __user *bitmap, struct vfio_iommu *iommu,
			      struct vfio_dma *dma, dma_addr_t base_iova,
			      size_t pgsize)
{
	unsigned long pgshift = __ffs(pgsize);
	/* [한국어] 이 매핑이 차지하는 비트 수 = 매핑 크기를 페이지 크기로 나눈 값. */
	unsigned long nbits = dma->size >> pgshift;
	/* [한국어] 요청 범위 시작부터 이 매핑까지의 비트 거리. */
	unsigned long bit_offset = (dma->iova - base_iova) >> pgshift;
	/* [한국어] 그 비트가 몇 번째 워드에 있는지. 사용자 버퍼가 u64 배열이므로 곧 배열 인덱스다. */
	unsigned long copy_offset = bit_offset / BITS_PER_LONG;
	/* [한국어] 워드 안에서의 비트 어긋남. 0 이 아니면 자리 맞추기가 필요하다. */
	unsigned long shift = bit_offset % BITS_PER_LONG;
	/* [한국어] 사용자 버퍼에서 읽어 올 한 워드분의 기존 비트. */
	unsigned long leftover;

	/*
	 * mark all pages dirty if any IOMMU capable device is not able
	 * to report dirty pages and all pages are pinned and mapped.
	 */
	/* [한국어] 추적 못 하는 장치가 있고 이 매핑이 IOMMU 에 설치되어 있으면 전부 dirty 로 보아야 안전하다. */
	if (iommu->num_non_pinned_groups && dma->iommu_mapped)
		/* [한국어] 추적 못 하는 장치가 있고 이 매핑이 IOMMU 에 설치되어 있으면, 어디에 썼는지 알 수 없으므로 전부 dirty 로 칠한다. */
		bitmap_set(dma->bitmap, 0, nbits);

	/* [한국어] 워드 경계에 맞지 않으면 */
	if (shift) {
		/* [한국어] 비트열을 shift 만큼 왼쪽으로 민다. 길이를 nbits+shift 로 주는 것은 밀려 나갈 자리를 포함하기 위해서다. */
		bitmap_shift_left(dma->bitmap, dma->bitmap, shift,
				  nbits + shift);

		/* [한국어] 앞선 매핑이 이미 쓴 비트를 잃지 않도록 그 워드를 읽어 온다. */
		if (copy_from_user(&leftover,
				   (void __user *)(bitmap + copy_offset),
				   sizeof(leftover)))
			return -EFAULT;

		/* [한국어] 읽어 온 하위 shift 비트를 합친다. */
		bitmap_or(dma->bitmap, dma->bitmap, &leftover, shift);
	}

	/* [한국어] 정렬된 비트열을 사용자 버퍼에 쓴다. 바이트 수는 64비트 단위로 올림한 값이다. */
	if (copy_to_user((void __user *)(bitmap + copy_offset), dma->bitmap,
			 DIRTY_BITMAP_BYTES(nbits + shift)))
		return -EFAULT;

	return 0;
}

/* [한국어]
 * vfio_iova_dirty_bitmap - 요청 범위의 dirty 비트맵을 사용자에게 넘기고 초기화한다
 *
 * @bitmap: 사용자 공간 u64 배열.
 * @iommu: 컨테이너 객체.
 * @iova: 요청 범위의 시작 IOVA.
 * @iova_end: 요청 범위의 끝 IOVA(포함).
 * @pgsize: 비트 하나가 뜻하는 크기.
 * @return: 0 성공, -EINVAL(범위가 매핑을 이등분함), -EFAULT(복사 실패).
 *
 * 마이그레이션 도중 주기적으로 불려, 지난번 이후 더러워진 페이지를 보고하고
 * 장부를 리셋한다. 리셋까지 한 번에 하는 이유는 '읽고 지우기' 사이에 새 쓰기가
 * 끼어들어 유실되는 창을 최소화하기 위해서다.
 *
 * 동작 단계:
 *  1. 상류 주석이 설명하는 경계 규칙을 검사한다. 요청 범위의 시작이 어떤 매핑의
 *     한가운데를 가리키면 안 되고(dma->iova != iova 면 -EINVAL), 끝도 마찬가지다.
 *     여러 매핑을 한 번에 덮는 것은 허용되지만 반으로 자르는 것은 금지다.
 *     vfio_find_dma(iommu, iova, 1) 처럼 크기 1 로 부르는 것은 '이 주소를 덮는
 *     매핑이 있는가' 를 묻는 관용구다.
 *  2. dma_list 를 처음부터 순회하며 요청 범위 안의 매핑만 처리한다.
 *     시작보다 앞이면 건너뛰고, 끝을 넘어서면 중단한다.
 *  3. 각 매핑에 update_user_bitmap() 으로 비트를 사용자 버퍼에 얹는다.
 *  4. 상류 주석대로 비트맵을 통째로 비운 뒤 vfio_dma_populate_bitmap() 으로 아직
 *     외부 핀이 살아 있는 페이지의 비트만 다시 세운다. 그 페이지들은 언제든 다시
 *     더러워질 수 있으므로 계속 dirty 로 보고해야 하고, 반대로 언핀된 페이지와
 *     dma_rw 로 이미 보고한 페이지는 여기서 지워진다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 사용자 복사로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_dirty_pages() 의 GET_BITMAP 분기 한 곳뿐.
 * callee: vfio_find_dma(), update_user_bitmap(), bitmap_clear(),
 *   vfio_dma_populate_bitmap().
 * 에러 경로: 중간에 실패하면 그때까지 처리한 매핑은 이미 비워졌으므로 부분적으로
 *   소비된 상태가 된다. 사용자는 오류를 받으면 마이그레이션을 중단해야 한다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_ioctl() → vfio_iommu_type1_dirty_pages() →
 *   [vfio_iova_dirty_bitmap] → update_user_bitmap()
 */
static int vfio_iova_dirty_bitmap(u64 __user *bitmap, struct vfio_iommu *iommu,
				  dma_addr_t iova, dma_addr_t iova_end, size_t pgsize)
{
	struct vfio_dma *dma;
	/* [한국어] dma_list 순회용 커서. */
	struct rb_node *n;
	/* [한국어] 비트 하나가 뜻하는 크기의 시프트 값. */
	unsigned long pgshift = __ffs(pgsize);
	/* [한국어] 복사 결과. */
	int ret;

	/*
	 * GET_BITMAP request must fully cover vfio_dma mappings.  Multiple
	 * vfio_dma mappings may be clubbed by specifying large ranges, but
	 * there must not be any previous mappings bisected by the range.
	 * An error will be returned if these conditions are not met.
	 */
	/* [한국어] 요청 시작 주소를 덮는 매핑이 있는지 본다. 크기 1 은 '이 주소를 포함하는가' 를 묻는 관용구다. */
	dma = vfio_find_dma(iommu, iova, 1);
	/* [한국어] 요청 시작이 어떤 매핑의 한가운데를 가리키면 안 된다. */
	if (dma && dma->iova != iova)
		return -EINVAL;

	/* [한국어] 요청 끝도 마찬가지로 검사한다. */
	dma = vfio_find_dma(iommu, iova_end, 1);
	/* [한국어] 매핑의 끝과 정확히 맞지 않으면 이등분이므로 거절한다. */
	if (dma && dma->iova + dma->size - 1 != iova_end)
		return -EINVAL;

	/* [한국어] 트리 전체를 오름차순으로 훑는다. */
	for (n = rb_first(&iommu->dma_list); n; n = rb_next(n)) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);

		/* [한국어] 요청 범위보다 앞에 있는 매핑은 건너뛴다. */
		if (dma->iova < iova)
			continue;

		/* [한국어] 요청 범위를 넘어섰으면 종료한다. */
		if (dma->iova > iova_end)
			break;

		/* [한국어] 이 매핑의 비트를 사용자 버퍼의 올바른 자리에 얹는다. */
		ret = update_user_bitmap(bitmap, iommu, dma, iova, pgsize);
		/* [한국어] 복사에 실패하면 그대로 반환한다. */
		if (ret)
			return ret;

		/*
		 * Re-populate bitmap to include all pinned pages which are
		 * considered as dirty but exclude pages which are unpinned and
		 * pages which are marked dirty by vfio_dma_rw()
		 */
		/* [한국어] 사용자에게 넘긴 비트는 모두 지운다 — 다음 주기에는 새로 더러워진 것만 보고해야 한다. */
		bitmap_clear(dma->bitmap, 0, dma->size >> pgshift);
		/* [한국어] 비운 뒤, 아직 외부 핀이 살아 있는 페이지의 비트만 다시 세운다 — 그 페이지들은 언제든 다시 더러워질 수 있다. */
		vfio_dma_populate_bitmap(dma, pgsize);
	}
	return 0;
}

/* [한국어]
 * verify_bitmap_size - 사용자가 준 비트맵 버퍼 크기가 요청 페이지 수에 맞는지 검사한다
 *
 * @npages: 보고할 페이지 수(= 필요한 비트 수).
 * @bitmap_size: 사용자가 제공한 버퍼의 바이트 크기.
 * @return: 0 이면 적법, -EINVAL 이면 거절.
 *
 * 사용자가 실제 필요보다 작은 버퍼를 주면 커널이 그 너머를 덮어써 메모리 손상이
 * 일어난다. 그것을 막는 입구 검사다.
 *
 * 검사 항목:
 *  - npages 가 0 이면 뜻이 없다.
 *  - bitmap_size 가 0 이면 쓸 곳이 없다.
 *  - DIRTY_BITMAP_SIZE_MAX 를 넘으면 거절한다. 커널이 한 번에 다룰 수 있는 상한이다.
 *  - DIRTY_BITMAP_BYTES(npages) 보다 작으면 필요한 만큼을 담지 못하므로 거절한다.
 *    더 큰 것은 허용한다 — 사용자가 여유 있게 잡는 것은 안전하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락과 무관한 순수 계산이다.
 *
 * caller: vfio_iommu_type1_unmap_dma(), vfio_iommu_type1_dirty_pages() 의
 *   GET_BITMAP 분기. 둘 다 사용자 인자를 막 복사해 온 직후에 부른다.
 * callee: 없다.
 * 에러 경로: -EINVAL 을 그대로 사용자에게 반환한다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_dirty_pages() → [verify_bitmap_size]
 */
static int verify_bitmap_size(uint64_t npages, uint64_t bitmap_size)
{
	if (!npages || !bitmap_size || (bitmap_size > DIRTY_BITMAP_SIZE_MAX) ||
	    /* [한국어] 필요한 비트를 담을 수 없을 만큼 작으면 커널이 사용자 버퍼 너머를 덮어쓰게 된다. */
	    (bitmap_size < DIRTY_BITMAP_BYTES(npages)))
		/* [한국어] 거절한다. */
		return -EINVAL;

	return 0;
}

/* [한국어]
 * vfio_notify_dma_unmap - 언맵 직전에 등록된 장치들에게 언핀을 요청한다
 *
 * @iommu: 컨테이너 객체.
 * @dma: 곧 언맵될 매핑.
 * @return: 없음.
 *
 * mdev 가 페이지를 핀해 둔 채로 매핑을 지우면 그 페이지는 영원히 새어 나간다.
 * 그래서 지우기 전에 등록된 장치들에게 '이 IOVA 범위를 놓아라' 라고 통지하고,
 * 장치가 vfio_unpin_pages() 로 돌려주기를 기다린다.
 *
 * 락 곡예가 이 함수의 핵심이다:
 *  1. device_list 가 비어 있으면 통지할 곳이 없으므로 곧바로 돌아간다. 이 검사는
 *     iommu->lock 아래에서 하므로 device_list_lock 을 잡지 않아도 안전하다 —
 *     등록/해제가 두 락을 모두 잡기 때문에 가능한 최적화다.
 *  2. device_list_lock 을 먼저 잡고 iommu->lock 을 놓는다. 순서가 중요하다.
 *     상류 주석대로 장치의 dma_unmap 콜백이 vfio_unpin_pages() 를 통해 다시 이
 *     파일로 들어와 iommu->lock 을 잡으려 하므로, 그 전에 놓아야 데드락이 없다.
 *     그동안 목록 자체는 device_list_lock 이 지킨다.
 *  3. 각 장치의 ops->dma_unmap(device, iova, size) 를 부른다.
 *  4. device_list_lock 을 놓고 iommu->lock 을 다시 잡는다.
 * 락을 놓았다 잡는 사이에 컨테이너 상태가 바뀔 수 있으므로, 호출자
 * vfio_dma_do_unmap() 은 이 함수가 돌아오면 again 라벨로 되돌아가 매핑 탐색부터
 * 전부 다시 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 진입 시 iommu->lock 을 쥐고 있어야 하고, 반환 시에도
 * 쥔 상태다. 장치 콜백 안에서 잠들 수 있다.
 *
 * caller: vfio_dma_do_unmap() 한 곳뿐 — pfn_list 가 비어 있지 않을 때만 부른다.
 * callee: 각 장치의 vfio_device_ops.dma_unmap 콜백. 그 구현은 vendor 드라이버에 있다.
 * 에러 경로: 없다. 장치가 놓아 주지 않으면 호출자의 재시도 카운터가 10 을 넘어
 *   BUG_ON 으로 멈춘다 — 그 상태로 진행하면 메모리 손상이 나기 때문이다.
 *
 * 호출 체인:
 *   vfio_dma_do_unmap() → [vfio_notify_dma_unmap] → device->ops->dma_unmap()
 */
/*
 * Notify VFIO drivers using vfio_register_emulated_iommu_dev() to invalidate
 * and unmap iovas within the range we're about to unmap. Drivers MUST unpin
 * pages in response to an invalidation.
 */
static void vfio_notify_dma_unmap(struct vfio_iommu *iommu,
				  struct vfio_dma *dma)
{
	struct vfio_device *device;

	/* [한국어] 통지할 장치가 없으면 곧바로 돌아간다. 이 검사는 iommu->lock 아래라 device_list_lock 없이도 안전하다. */
	if (list_empty(&iommu->device_list))
		return;

	/*
	 * The device is expected to call vfio_unpin_pages() for any IOVA it has
	 * pinned within the range. Since vfio_unpin_pages() will eventually
	 * call back down to this code and try to obtain the iommu->lock we must
	 * drop it.
	 */
	/* [한국어] 목록 락을 먼저 잡는다. 순서는 언제나 주 락 -> 목록 락이다. */
	mutex_lock(&iommu->device_list_lock);
	/* [한국어] 주 락을 놓는다. 장치 콜백이 vfio_unpin_pages() 를 통해 다시 이 파일로 들어와
	 * 같은 락을 잡으려 하므로, 놓지 않으면 데드락이 난다. 그동안 목록 자체는
	 * device_list_lock 이 지킨다. */
	mutex_unlock(&iommu->lock);

	/* [한국어] 등록된 모든 장치에 */
	list_for_each_entry(device, &iommu->device_list, iommu_entry)
		/* [한국어] 이 IOVA 범위를 놓으라고 통지한다. 장치는 응답으로 vfio_unpin_pages() 를 불러야 한다. */
		device->ops->dma_unmap(device, dma->iova, dma->size);

	/* [한국어] 목록 락을 놓는다. */
	mutex_unlock(&iommu->device_list_lock);
	/* [한국어] 통지가 끝났으니 주 락을 다시 잡는다. 호출자는 이후 상태를 다시 확인해야 한다. */
	mutex_lock(&iommu->lock);
}

/* [한국어]
 * vfio_dma_do_unmap - VFIO_IOMMU_UNMAP_DMA 의 본체. 요청 범위의 매핑들을 없앤다
 *
 * @iommu: 컨테이너 객체.
 * @unmap: 사용자에게서 복사해 온 요청. 반환 시 size 필드에 실제 언맵량을 적어 준다.
 * @bitmap: dirty 비트맵을 함께 가져가는 경우의 사용자 버퍼 정보.
 * @return: 0 성공, -EINVAL(정렬/경계 위반), -EOVERFLOW, -EBUSY.
 *
 * MAP 의 역연산이다. 세 가지 변형을 한 함수가 처리한다:
 *  (a) 일반 언맵 — 범위 안의 매핑들을 지운다.
 *  (b) UNMAP_ALL — 컨테이너의 모든 매핑을 지운다.
 *  (c) VADDR 무효화 — 매핑은 남기고 사용자 주소만 무효 표시한다. 프로세스가
 *      주소 공간을 재구성하는 동안 IOVA 를 유지하기 위한 기능이다.
 *
 * 동작 단계:
 *  1. iommu->lock 을 잡는다.
 *  2. VADDR 무효화는 mdev 가 있으면 금지한다(-EBUSY). mdev 는 vaddr 없이 핀/rw 를
 *     할 수 없기 때문이다.
 *  3. size_t/dma_addr_t 로 좁혀 담은 값이 원래 __u64 값과 같은지 확인한다 —
 *     32비트 커널에서 64비트 인자가 잘려 들어오는 것을 잡아내는 검사다.
 *  4. 최소 지원 페이지 크기를 pgshift/pgsize 로 구하고, iova 가 그에 정렬되어
 *     있는지 (iova & (pgsize - 1)) 로 본다.
 *  5. UNMAP_ALL 이면 iova 와 size 가 0 이어야 하고, 범위 끝을 주소 공간 최대값으로
 *     둔다. 아니면 size 가 0 이 아니고 정렬되어 있어야 하며, iova+size-1 이
 *     넘치지 않는지 check_add_overflow() 로 확인한다.
 *  6. dirty 비트맵을 함께 요청했다면 추적이 켜져 있고 비트맵 페이지 크기가 최소
 *     지원 크기와 같아야 한다.
 *  7. WARN_ON((pgsize - 1) & PAGE_MASK) — 최소 IOMMU 페이지 크기가 PAGE_SIZE 보다
 *     작으면 안 된다는 불변식 확인이다. vfio_update_pgsize_bitmap() 이 그것을 보장한다.
 *  8. again 라벨(재시도 지점): v2 이고 UNMAP_ALL 이 아니면 상류 주석이 길게 설명하는
 *     경계 규칙을 적용한다 — 범위의 시작과 끝이 기존 매핑을 반으로 자르면 -EINVAL.
 *     v1 호환 모드에서는 이 검사를 하지 않는 대신 아래 루프에서 매핑 중간부터
 *     시작하는 요청을 break 로 무시한다.
 *  9. vfio_find_dma_first_node() 로 범위의 첫 노드를 찾아 rb_next() 로 훑는다.
 *     - VADDR 무효화 모드: 이미 무효인 매핑을 또 무효화하려 하면 오류다. 그때는
 *       이번 호출에서 무효화한 것들을 first_n 부터 되돌린 뒤 -EINVAL 로 중단한다.
 *       정상이면 표시하고 카운터를 올린 뒤 다음으로 넘어간다.
 *     - 외부 핀이 남아 있으면(pfn_list 가 비지 않음) vfio_notify_dma_unmap() 으로
 *       장치에 언핀을 요청하고 again 으로 되돌아간다. 같은 매핑에서 10 번 넘게
 *       실패하면 BUG_ON — 장치가 규약을 지키지 않는다는 뜻이며 그대로 진행하면
 *       메모리 손상이 나므로 여기서 멈추는 편이 낫다.
 *     - dirty 비트맵을 함께 요청했으면 update_user_bitmap() 으로 넘긴다.
 *     - 크기를 누적하고, rb_next() 로 다음 노드를 먼저 잡아 둔 뒤 vfio_remove_dma()
 *       로 현재 노드를 없앤다. 순서가 중요하다 — 지운 노드에서 rb_next() 를 부를 수 없다.
 * 10. unlock 라벨에서 락을 놓고, 실제 언맵량을 unmap->size 에 적어 사용자에게 알린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. ioctl 경로에서만 불린다. 락을 잠시 놓았다 잡는
 * 구간(vfio_notify_dma_unmap)이 있으므로 재진입 가능한 구조로 짜여 있다.
 *
 * caller: vfio_iommu_type1_unmap_dma() 한 곳뿐.
 * callee: vfio_find_dma(), vfio_find_dma_first_node(), vfio_notify_dma_unmap(),
 *   update_user_bitmap(), vfio_remove_dma().
 * 에러 경로: 대부분 unlock 라벨로 점프해 -EINVAL 을 반환한다. 오류가 나도 unmap->size
 *   에는 그때까지 실제로 언맵한 양이 담긴다.
 *
 * 호출 체인:
 *   container.c:353 ops->ioctl → vfio_iommu_type1_ioctl() →
 *   vfio_iommu_type1_unmap_dma() → [vfio_dma_do_unmap] → vfio_remove_dma()
 */
static int vfio_dma_do_unmap(struct vfio_iommu *iommu,
			     struct vfio_iommu_type1_dma_unmap *unmap,
			     struct vfio_bitmap *bitmap)
{
	struct vfio_dma *dma, *dma_last = NULL;
	/* [한국어] 실제로 언맵한 총 바이트 수와, 최소 지원 IOMMU 페이지 크기. */
	size_t unmapped = 0, pgsize;
	/* [한국어] 기본 반환값은 -EINVAL — 아래 검사들이 그냥 unlock 으로 점프하면 이 값이 나간다. 재시도 카운터도 함께 선언한다. */
	int ret = -EINVAL, retries = 0;
	/* [한국어] 최소 페이지 크기의 시프트 값. */
	unsigned long pgshift;
	/* [한국어] 사용자가 준 시작 IOVA 를 dma_addr_t 로 좁혀 담는다. */
	dma_addr_t iova = unmap->iova;
	/* [한국어] 그 범위의 마지막 바이트 주소. */
	dma_addr_t iova_end;
	/* [한국어] 사용자가 준 길이를 size_t 로 좁혀 담는다. */
	size_t size = unmap->size;
	/* [한국어] 컨테이너의 모든 매핑을 지우는 변형인가. */
	bool unmap_all = unmap->flags & VFIO_DMA_UNMAP_FLAG_ALL;
	/* [한국어] 매핑은 남기고 사용자 주소만 무효화하는 변형인가. */
	bool invalidate_vaddr = unmap->flags & VFIO_DMA_UNMAP_FLAG_VADDR;
	/* [한국어] 순회 커서와, 범위의 첫 노드(무효화 되감기의 기준점). */
	struct rb_node *n, *first_n;

	/* [한국어] 매핑 트리를 바꾸므로 락이 필요하다. */
	mutex_lock(&iommu->lock);

	/* Cannot update vaddr if mdev is present. */
	/* [한국어] vaddr 무효화를 요청했는데 mdev 가 붙어 있으면 */
	if (invalidate_vaddr && !list_empty(&iommu->emulated_iommu_groups)) {
		/* [한국어] 장치가 사용 중이라는 뜻의 오류. */
		ret = -EBUSY;
		/* [한국어] mdev 는 vaddr 없이 핀/rw 를 안전하게 할 수 없으므로 거절한다. */
		goto unlock;
	}

	/* [한국어] 좁혀 담는 과정에서 값이 잘렸으면 32비트 커널에서 인자가 손실된 것이다. */
	if (iova != unmap->iova || size != unmap->size) {
		ret = -EOVERFLOW;
		/* [한국어] 값이 잘렸으면 오버플로다. */
		goto unlock;
	}

	/* [한국어] 최소 지원 페이지 크기의 시프트. */
	pgshift = __ffs(iommu->pgsize_bitmap);
	/* [한국어] 그 크기 자체. 2 의 거듭제곱이다. */
	pgsize = (size_t)1 << pgshift;

	/* [한국어] 시작 주소가 그 크기에 정렬되어 있어야 한다. pgsize-1 은 하위 비트 마스크다. */
	if (iova & (pgsize - 1))
		goto unlock;

	/* [한국어] 전체 언맵 변형이면 */
	if (unmap_all) {
		/* [한국어] iova 와 size 는 반드시 0 이어야 한다 — 범위를 지정하는 것과 모순되기 때문이다. */
		if (iova || size)
			goto unlock;
		/* [한국어] 범위 끝을 주소 공간의 최대값으로 둔다. 모든 매핑이 이 범위 안에 들어온다. */
		iova_end = ~(dma_addr_t)0;
	} else {
		/* [한국어] 일반 언맵이면 길이가 0 이 아니고 정렬되어 있어야 한다. */
		if (!size || size & (pgsize - 1))
			goto unlock;

		/* [한국어] 닫힌 구간의 끝을 구하면서 주소 공간을 넘는지 확인한다. */
		if (check_add_overflow(iova, size - 1, &iova_end)) {
			ret = -EOVERFLOW;
			/* [한국어] 정렬/오버플로 검사 실패 — 기본값 -EINVAL 이 나간다. */
			goto unlock;
		}
	}

	/* When dirty tracking is enabled, allow only min supported pgsize */
	/* [한국어] dirty 비트맵을 함께 회수하는 변형이면 */
	if ((unmap->flags & VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP) &&
	    (!iommu->dirty_page_tracking || (bitmap->pgsize != pgsize))) {
		/* [한국어] 추적이 꺼져 있거나 비트맵 페이지 크기가 다르면 비트맵을 만들어 줄 수 없다. */
		goto unlock;
	}

	/* [한국어] 최소 IOMMU 페이지 크기가 PAGE_SIZE 보다 작으면 안 된다는 불변식 확인. */
	WARN_ON((pgsize - 1) & PAGE_MASK);
again:
	/*
	 * vfio-iommu-type1 (v1) - User mappings were coalesced together to
	 * avoid tracking individual mappings.  This means that the granularity
	 * of the original mapping was lost and the user was allowed to attempt
	 * to unmap any range.  Depending on the contiguousness of physical
	 * memory and page sizes supported by the IOMMU, arbitrary unmaps may
	 * or may not have worked.  We only guaranteed unmap granularity
	 * matching the original mapping; even though it was untracked here,
	 * the original mappings are reflected in IOMMU mappings.  This
	 * resulted in a couple unusual behaviors.  First, if a range is not
	 * able to be unmapped, ex. a set of 4k pages that was mapped as a
	 * 2M hugepage into the IOMMU, the unmap ioctl returns success but with
	 * a zero sized unmap.  Also, if an unmap request overlaps the first
	 * address of a hugepage, the IOMMU will unmap the entire hugepage.
	 * This also returns success and the returned unmap size reflects the
	 * actual size unmapped.
	 *
	 * We attempt to maintain compatibility with this "v1" interface, but
	 * we take control out of the hands of the IOMMU.  Therefore, an unmap
	 * request offset from the beginning of the original mapping will
	 * return success with zero sized unmap.  And an unmap request covering
	 * the first iova of mapping will unmap the entire range.
	 *
	 * The v2 version of this interface intends to be more deterministic.
	 * Unmap requests must fully cover previous mappings.  Multiple
	 * mappings may still be unmaped by specifying large ranges, but there
	 * must not be any previous mappings bisected by the range.  An error
	 * will be returned if these conditions are not met.  The v2 interface
	 * will only return success and a size of zero if there were no
	 * mappings within the range.
	 */
	/* [한국어] v2 이고 전체 언맵이 아니면 경계 규칙을 적용한다. */
	if (iommu->v2 && !unmap_all) {
		/* [한국어] v2 경계 규칙 — 시작 주소를 덮는 매핑이 있는지 본다. */
		dma = vfio_find_dma(iommu, iova, 1);
		/* [한국어] 있는데 그 매핑의 시작이 아니면 요청이 매핑을 반으로 자르는 것이다. */
		if (dma && dma->iova != iova)
			goto unlock;

		/* [한국어] 끝 주소도 마찬가지로 검사한다. */
		dma = vfio_find_dma(iommu, iova_end, 1);
		/* [한국어] 매핑의 끝과 정확히 일치하지 않으면 역시 이등분이다. */
		if (dma && dma->iova + dma->size - 1 != iova_end)
			goto unlock;
	}

	/* [한국어] 여기까지 왔으면 검증을 통과한 것이다. 기본값 -EINVAL 을 0 으로 바꾼다. */
	ret = 0;
	/* [한국어] 범위와 겹치는 노드 중 가장 낮은 것을 찾는다. first_n 은 무효화 되감기의 기준점으로 보관한다. */
	n = first_n = vfio_find_dma_first_node(iommu, iova, iova_end);

	/* [한국어] 범위를 벗어날 때까지 오름차순으로 훑는다. */
	while (n) {
		/* [한국어] 노드에서 구조체를 복원한다. */
		dma = rb_entry(n, struct vfio_dma, node);
		/* [한국어] 범위 끝을 넘어섰으면 종료한다. */
		if (dma->iova > iova_end)
			break;

		/* [한국어] v1 호환 모드에서는 매핑 중간부터 시작하는 요청을 조용히 무시한다 — 상류 주석이 설명하는 옛 동작이다. */
		if (!iommu->v2 && iova > dma->iova)
			break;

		/* [한국어] vaddr 무효화 변형. */
		if (invalidate_vaddr) {
			/* [한국어] 이미 무효인 매핑을 또 무효화하려 하면 오류다. */
			if (dma->vaddr_invalid) {
				/* [한국어] 되감기 종료 지점을 기억한다. */
				struct rb_node *last_n = n;

				/* [한국어] 이번 호출에서 무효화한 것들을 처음부터 되돌린다. */
				for (n = first_n; n != last_n; n = rb_next(n)) {
					/* [한국어] 노드에서 구조체를 복원해 */
					dma = rb_entry(n,
						       struct vfio_dma, node);
					/* [한국어] 무효 표시를 내리고 */
					dma->vaddr_invalid = false;
					/* [한국어] 컨테이너 카운터도 되돌린다. */
					iommu->vaddr_invalid_count--;
				}
				/* [한국어] 장치가 아직 놓지 않은 상태에서 되돌렸으므로 인자 오류로 보고한다. */
				ret = -EINVAL;
				/* [한국어] 실제로 언맵한 것은 없으므로 사용자에게 0 을 보고한다. */
				unmapped = 0;
				break;
			}
			/* [한국어] 무효 표시를 세운다. 이제 이 매핑의 vaddr 는 읽으면 안 된다. */
			dma->vaddr_invalid = true;
			/* [한국어] 컨테이너 카운터를 올린다. 0 이 아닌 동안 핀/rw/attach 가 모두 막힌다. */
			iommu->vaddr_invalid_count++;
			/* [한국어] 무효화한 크기를 누적한다. */
			unmapped += dma->size;
			/* [한국어] 다음 노드로 넘어간다 — 매핑을 지우지 않으므로 그냥 전진한다. */
			n = rb_next(n);
			continue;
		}

		/* [한국어] 외부 핀이 남아 있으면 지금 지울 수 없다. */
		if (!RB_EMPTY_ROOT(&dma->pfn_list)) {
			/* [한국어] 같은 매핑에서 또 막혔으면 */
			if (dma_last == dma) {
				/* [한국어] 10 번을 넘으면 장치가 언핀 규약을 어긴 것이다. 그대로 진행하면 메모리 손상이 나므로 여기서 멈춘다. */
				BUG_ON(++retries > 10);
			} else {
				/* [한국어] 다른 매핑이면 재시도 카운터를 이 매핑 기준으로 새로 잡는다. */
				dma_last = dma;
				/* [한국어] 카운터를 초기화한다. */
				retries = 0;
			}

			/* [한국어] 등록된 장치들에 언핀을 요청한다. 이 안에서 락을 놓았다 잡으므로 상태가 바뀔 수 있다. */
			vfio_notify_dma_unmap(iommu, dma);
			/* [한국어] 락을 놓았다 잡는 사이 상태가 바뀌었을 수 있으므로 매핑 탐색부터 전부 다시 한다. */
			goto again;
		}

		/* [한국어] dirty 비트맵을 함께 회수하는 변형이면 */
		if (unmap->flags & VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP) {
			/* [한국어] 이 매핑의 비트를 사용자 버퍼에 얹는다. */
			ret = update_user_bitmap(bitmap->data, iommu, dma,
						 iova, pgsize);
			/* [한국어] 복사에 실패하면 더 진행하지 않는다. */
			if (ret)
				break;
		}

		/* [한국어] 언맵할 크기를 누적한다. */
		unmapped += dma->size;
		/* [한국어] 다음 노드를 먼저 잡아 둔다 — 지운 노드에서는 rb_next 를 부를 수 없다. */
		n = rb_next(n);
		/* [한국어] 매핑을 완전히 없앤다(언맵·언핀·과금 반환·해제). */
		vfio_remove_dma(iommu, dma);
	}

unlock:
	/* [한국어] 락을 놓는다. 모든 경로가 여기로 모인다. */
	mutex_unlock(&iommu->lock);

	/* Report how much was unmapped */
	/* [한국어] 실제로 언맵(또는 무효화)한 총 바이트 수를 사용자에게 알려 준다. 오류가 나도 그때까지의 양이 담긴다. */
	unmap->size = unmapped;

	return ret;
}

/* [한국어]
 * vfio_iommu_map - 물리 연속 구간 하나를 컨테이너의 모든 domain 에 매핑한다
 *
 * @iommu: 컨테이너 객체.
 * @iova: 매핑할 IOVA 시작.
 * @pfn: 대응하는 호스트 물리 페이지 프레임 번호.
 * @npage: 페이지 수.
 * @prot: 접근 권한(IOMMU_READ/IOMMU_WRITE).
 * @return: 0 성공, 실패 시 iommu_map() 이 준 음수 오류.
 *
 * 한 컨테이너에 여러 IOMMU domain 이 있으면 모든 domain 에서 같은 IOVA 가 같은 물리
 * 주소로 번역되어야 한다. 그래야 어느 장치가 DMA 하든 같은 버퍼에 닿는다. 이 함수는
 * 그 '전부 아니면 전무' 를 보장한다.
 *
 * 동작 단계:
 *  1. 모든 domain 에 iommu_map() 을 부른다. 물리 주소는 pfn 을 PAGE_SHIFT 만큼 올려
 *     만들고, 길이도 페이지 수를 올려 바이트로 바꾼다.
 *  2. prot 에 IOMMU_CACHE 를 항상 더한다 — 장치의 DMA 가 CPU 캐시와 일관되게
 *     처리되도록 요청하는 속성이다.
 *  3. GFP_KERNEL_ACCOUNT 를 쓴다. IOMMU 페이지 테이블 자체를 위한 커널 메모리를
 *     요청한 cgroup 앞으로 과금해, 사용자가 매핑을 남발해 커널 메모리를 고갈시키는
 *     것을 막는다.
 *  4. 각 domain 사이에 cond_resched() 로 CPU 를 양보한다.
 *  5. 실패하면 unwind 라벨에서 list_for_each_entry_continue_reverse() 로 지금까지
 *     성공한 domain 들을 역순으로 훑으며 iommu_unmap() 한다. 실패한 domain 자신은
 *     매핑되지 않았으므로 되돌릴 것이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 페이지 테이블 할당으로
 * 잠들 수 있다.
 *
 * caller: vfio_pin_map_dma() 한 곳뿐.
 * callee: iommu_map(), iommu_unmap(), cond_resched(). 이들의 내부는 drivers/iommu/ 가
 *   없는 이 트리에서 확인 못 함.
 * 에러 경로: 위 5 번의 되감기 후 오류를 반환한다. 페이지 핀은 이 함수의 책임이
 *   아니므로 호출자가 따로 되돌린다.
 *
 * 호출 체인:
 *   vfio_dma_do_map() → vfio_pin_map_dma() → [vfio_iommu_map] → iommu_map()
 */
static int vfio_iommu_map(struct vfio_iommu *iommu, dma_addr_t iova,
			  unsigned long pfn, long npage, int prot)
{
	struct vfio_domain *d;
	/* [한국어] 설치 결과. */
	int ret;

	/* [한국어] 컨테이너의 모든 domain 에 같은 번역을 설치한다. */
	list_for_each_entry(d, &iommu->domain_list, next) {
		/* [한국어] pfn 을 물리 주소로 올려 iova 에 설치한다. IOMMU_CACHE 로 캐시 일관 처리를 요청하고, 페이지 테이블 메모리는 요청자 cgroup 앞으로 과금한다. */
		ret = iommu_map(d->domain, iova, (phys_addr_t)pfn << PAGE_SHIFT,
				npage << PAGE_SHIFT, prot | IOMMU_CACHE,
				GFP_KERNEL_ACCOUNT);
		/* [한국어] 하나라도 실패하면 전부 되돌린다 — 모든 domain 이 같은 번역을 가져야 하기 때문이다. */
		if (ret)
			goto unwind;

		/* [한국어] domain 이 많으면 이 루프가 길어질 수 있으므로 CPU 를 양보한다. */
		cond_resched();
	}

	return 0;

unwind:
	/* [한국어] 지금까지 성공한 domain 들을 역순으로 훑는다. 실패한 domain 자신은 설치되지 않았다. */
	list_for_each_entry_continue_reverse(d, &iommu->domain_list, next) {
		/* [한국어] 지금까지 성공한 domain 들을 역순으로 훑으며 설치를 취소한다. */
		iommu_unmap(d->domain, iova, npage << PAGE_SHIFT);
		/* [한국어] 되감기 도중에도 CPU 를 양보한다. */
		cond_resched();
	}

	return ret;
}

/* [한국어]
 * vfio_pin_map_dma - 매핑 하나를 청크 단위로 핀하고 IOMMU 에 설치한다
 *
 * @iommu: 컨테이너 객체.
 * @dma: 대상 매핑. 삽입 직후라 size 는 0 이고, 이 함수가 조금씩 키운다.
 * @map_size: 매핑해야 할 총 바이트 수.
 * @return: 0 성공, 음수면 오류(이때 매핑은 이미 제거되어 있다).
 *
 * 물리 메모리는 조각나 있으므로 사용자 버퍼 전체가 물리적으로 연속일 리 없다.
 * 그래서 '연속인 만큼 핀 -> 그만큼 매핑' 을 반복해 전체를 덮는다. dma->size 를
 * 진행 표시로 쓰기 때문에, 중간에 실패해도 어디까지 되돌려야 하는지가 자동으로
 * 기록된다.
 *
 * 동작 단계:
 *  1. limit 을 rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT 로 구한다 — 바이트 한도를 페이지
 *     수로 바꾼 것이다. current 의 한도를 쓰는 이유는 이 경로가 사용자 요청 문맥에서만
 *     실행되기 때문이다.
 *  2. vfio_batch_init() 으로 GUP 배치를 준비한다.
 *  3. 남은 크기가 있는 동안 반복한다.
 *     - vfio_pin_pages_remote(dma, vaddr + dma->size, ...) — 진행량을 dma->size 로
 *       읽는다. 이 값은 직전 바퀴에서 매핑에 성공한 만큼만 올라가 있으므로 언제나
 *       '다음에 핀할 주소' 를 정확히 가리킨다.
 *     - 0 이하가 오면 오류다. 0 은 있을 수 없으므로 WARN_ON 을 낸다.
 *     - vfio_iommu_map(iova + dma->size, ...) 로 방금 핀한 만큼을 모든 domain 에 설치한다.
 *     - 매핑에 실패하면 방금 핀한 것을 vfio_unpin_pages_remote(do_accounting=true) 로
 *       되돌리고, 배치에 남은 것도 vfio_batch_unpin() 으로 되돌린 뒤 루프를 벗어난다.
 *       이 두 번의 되감기가 짝을 이루어야 핀 장부가 맞는다.
 *     - 성공하면 남은 크기를 줄이고 dma->size 를 키운다.
 *  4. 배치 배열을 해제하고 iommu_mapped 를 true 로 세운다.
 *  5. 오류가 있었으면 vfio_remove_dma() 로 매핑 전체를 없앤다. 이때 dma->size 가
 *     성공한 만큼만 올라가 있으므로 정확히 그만큼만 언맵·언핀된다.
 *     iommu_mapped 를 실패 시에도 true 로 세워 두는 이유가 여기 있다 — 그래야
 *     vfio_remove_dma() 가 부분 매핑을 제대로 걷어낸다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. GUP 와 페이지 테이블 할당으로
 * 잠들 수 있다.
 *
 * caller: vfio_dma_do_map() 한 곳뿐.
 * callee: vfio_batch_init(), vfio_pin_pages_remote(), vfio_iommu_map(),
 *   vfio_unpin_pages_remote(), vfio_batch_unpin(), vfio_batch_fini(), vfio_remove_dma().
 * 에러 경로: 위 5 번. 호출자는 dma 포인터를 더 이상 쓰지 않는다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_map_dma() → vfio_dma_do_map() → [vfio_pin_map_dma] →
 *   vfio_pin_pages_remote()
 */
static int vfio_pin_map_dma(struct vfio_iommu *iommu, struct vfio_dma *dma,
			    size_t map_size)
{
	dma_addr_t iova = dma->iova;
	/* [한국어] 매핑의 사용자 주소 시작. */
	unsigned long vaddr = dma->vaddr;
	/* [한국어] GUP 배치. 스택에 잡는다. */
	struct vfio_batch batch;
	/* [한국어] 아직 매핑하지 못한 남은 바이트 수. */
	size_t size = map_size;
	/* [한국어] 이번 바퀴에 핀한 페이지 수. */
	long npage;
	/* [한국어] 첫 pfn 과, RLIMIT_MEMLOCK 한도를 페이지 수로 환산한 값. */
	unsigned long pfn, limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
	/* [한국어] 결과 코드. */
	int ret = 0;

	/* [한국어] GUP 배치를 준비한다. */
	vfio_batch_init(&batch);

	/* [한국어] 남은 크기가 0 이 될 때까지 청크 단위로 반복한다. */
	while (size) {
		/* Pin a contiguous chunk of memory */
		/* [한국어] 다음에 핀할 주소는 vaddr + 지금까지 매핑한 크기다. */
		npage = vfio_pin_pages_remote(dma, vaddr + dma->size,
					      size >> PAGE_SHIFT, &pfn, limit,
					      &batch);
		/* [한국어] 0 이하면 오류다. */
		if (npage <= 0) {
			/* [한국어] 0 은 있을 수 없다 — 진행이 없으면 무한 루프가 된다. */
			WARN_ON(!npage);
			/* [한국어] 음수 오류를 int 로 좁혀 담는다. */
			ret = (int)npage;
			break;
		}

		/* Map it! */
		/* [한국어] 방금 핀한 구간을 모든 domain 에 설치한다. 매핑 진행 오프셋으로 dma->size 를 쓴다. */
		ret = vfio_iommu_map(iommu, iova + dma->size, pfn, npage,
				     dma->prot);
		/* [한국어] IOMMU 설치에 실패했으면 */
		if (ret) {
			/* [한국어] 방금 핀한 만큼 되돌린다. do_accounting 이 true 라 과금도 함께 되돌아간다. */
			vfio_unpin_pages_remote(dma, iova + dma->size, pfn,
						npage, true);
			/* [한국어] 배치에 남은 잔여 핀도 되돌린다. 이 둘이 짝을 이루어야 핀 장부가 맞는다. */
			vfio_batch_unpin(&batch, dma);
			break;
		}

		/* [한국어] 설치한 만큼 남은 양을 줄이고 */
		size -= npage << PAGE_SHIFT;
		/* [한국어] 매핑 크기를 키운다. 이 값이 곧 다음 바퀴의 진행 오프셋이자 실패 시 되돌릴 범위다. */
		dma->size += npage << PAGE_SHIFT;
	}

	/* [한국어] 배치 배열을 해제한다. 잔여 핀 정리는 위에서 이미 끝났다. */
	vfio_batch_fini(&batch);
	/* [한국어] 실패했더라도 true 로 둔다 — 그래야 아래 vfio_remove_dma() 가 부분 매핑을 제대로 걷어낸다. */
	dma->iommu_mapped = true;

	/* [한국어] 오류가 있었으면 */
	if (ret)
		/* [한국어] 매핑을 통째로 없앤다. dma->size 가 성공한 만큼만 올라가 있으므로 정확히 그만큼만 정리된다. */
		vfio_remove_dma(iommu, dma);

	return ret;
}

/* [한국어]
 * vfio_iommu_iova_dma_valid - 요청 구간이 매핑 가능한 IOVA 창 안에 있는지 확인한다
 *
 * @iommu: 컨테이너 객체.
 * @start: 요청 구간의 시작.
 * @end: 요청 구간의 끝(포함).
 * @return: true 면 매핑해도 된다, false 면 거절해야 한다.
 *
 * IOMMU 하드웨어가 다룰 수 있는 주소 범위(aperture)는 유한하고, 그중에서도
 * MSI 도어벨 같은 예약 구간은 비켜 가야 한다. iommu->iova_list 가 그 결과로 남은
 * '써도 되는 구간들' 이고, 이 함수는 요청이 그중 하나에 통째로 들어가는지 본다.
 *
 * 동작 단계:
 *  1. 목록을 훑으며 start >= node->start && end <= node->end 인 노드가 있으면 true.
 *     구간을 쪼개 여러 노드에 걸치는 것은 허용하지 않는다 — 그 사이에 예약 구간이
 *     끼어 있다는 뜻이기 때문이다.
 *  2. 하나도 없으면, 상류 주석대로 목록이 비어 있는지 확인한다. mdev 만 있는
 *     컨테이너는 하드웨어 IOMMU 가 없어 목록이 비고, 그때는 제약이 없으므로 true 다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_dma_do_map() 한 곳뿐.
 * callee: 없다. 목록 순회뿐이다.
 * 에러 경로: false 를 반환하면 호출자가 -EINVAL 을 사용자에게 돌려준다.
 *
 * 호출 체인:
 *   vfio_dma_do_map() → [vfio_iommu_iova_dma_valid]
 */
/*
 * Check dma map request is within a valid iova range
 */
static bool vfio_iommu_iova_dma_valid(struct vfio_iommu *iommu,
				      dma_addr_t start, dma_addr_t end)
{
	struct list_head *iova = &iommu->iova_list;
	/* [한국어] 목록 순회용 커서. */
	struct vfio_iova *node;

	/* [한국어] 허용 구간들을 훑는다. */
	list_for_each_entry(node, iova, list) {
		/* [한국어] 요청이 한 구간 안에 통째로 들어가야 한다 — 여러 구간에 걸치면 사이에 예약 구간이 끼어 있다는 뜻이다. */
		if (start >= node->start && end <= node->end)
			return true;
	}

	/*
	 * Check for list_empty() as well since a container with
	 * a single mdev device will have an empty list.
	 */
	/* [한국어] 구간 목록이 비어 있으면 제약이 없다는 뜻이므로 어떤 IOVA 든 허용한다. */
	return list_empty(iova);
}

/* [한국어]
 * vfio_change_dma_owner - 매핑의 잠금 메모리 회계 주체를 현재 프로세스로 옮긴다
 *
 * @dma: 소유자를 바꿀 매핑.
 * @return: 0 성공, 음수면 새 소유자의 한도 초과 등 회계 실패.
 *
 * VFIO_UPDATE_VADDR 기능은 매핑을 유지한 채 사용자 주소만 다시 지정하게 해 준다.
 * 그 새 주소는 다른 프로세스의 것일 수 있다(예: fork 후 자식이 이어받는 경우).
 * 그러면 잠금 메모리 과금도 함께 옮겨야 한다 — 옮기지 않으면 옛 프로세스는 죽은
 * 뒤에도 한도가 묶여 있고, 새 프로세스는 실제로 잠근 메모리를 회계에 반영하지 않게 된다.
 *
 * 동작 단계:
 *  1. mm 이 같으면 옮길 것이 없다.
 *  2. 새 소유자의 CAP_IPC_LOCK 보유 여부를 지금 확인한다.
 *  3. 먼저 새 mm 에 dma->locked_vm 만큼 더한다. 이때 한도 검사가 일어나므로,
 *     새 소유자가 한도를 넘으면 여기서 실패하고 옛 회계는 그대로 남는다.
 *     순서가 '더하기 먼저' 인 이유가 이것이다 — 빼기부터 하면 실패 시 옛 소유자에게
 *     되돌려 주어야 하는데 그 사이 mm 이 사라졌을 수 있다.
 *  4. 옛 mm 을 mmget_not_zero() 로 승격할 수 있으면(아직 살아 있으면) 같은 양을 뺀다.
 *     이미 죽었으면 뺄 필요가 없다 — 프로세스 종료 시 회계가 함께 사라지기 때문이다.
 *  5. task 가 달라졌으면 옛 참조를 놓고 새 group_leader 참조를 잡는다.
 *  6. mmdrop() 으로 옛 mm 참조를 놓고, 새 mm 을 mmgrab() 으로 붙잡는다.
 *  7. lock_cap 을 새 값으로 갱신한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 회계에서 mmap 쓰기 락을
 * 잡으므로 잠들 수 있다.
 *
 * caller: vfio_dma_do_map() 의 set_vaddr 분기 한 곳뿐.
 * callee: capable(), mm_lock_acct(), mmget_not_zero(), mmput(), put_task_struct(),
 *   get_task_struct(), mmdrop(), mmgrab().
 * 에러 경로: 3 번에서 실패하면 아무것도 바꾸지 않고 오류를 반환한다. 호출자는
 *   vaddr 를 갱신하지 않고 사용자에게 오류를 올린다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_map_dma() → vfio_dma_do_map() → [vfio_change_dma_owner] →
 *   mm_lock_acct()
 */
static int vfio_change_dma_owner(struct vfio_dma *dma)
{
	struct task_struct *task = current->group_leader;
	/* [한국어] 새 소유자가 될 주소 공간. */
	struct mm_struct *mm = current->mm;
	/* [한국어] 옮겨야 할 과금량 — 이 매핑이 지금까지 잠근 페이지 수다. */
	long npage = dma->locked_vm;
	/* [한국어] 새 소유자의 CAP_IPC_LOCK 보유 여부. */
	bool lock_cap;
	/* [한국어] 회계 결과. */
	int ret;

	/* [한국어] 주소 공간이 그대로면 옮길 것이 없다. */
	if (mm == dma->mm)
		return 0;

	/* [한국어] 새 소유자의 권한을 지금 확인해 굳혀 둔다. */
	lock_cap = capable(CAP_IPC_LOCK);
	/* [한국어] 먼저 새 mm 에 더한다 — 여기서 한도 검사가 일어나므로, 실패해도 옛 회계가 온전히 남는다. */
	ret = mm_lock_acct(task, mm, lock_cap, npage);
	/* [한국어] 새 소유자가 한도를 넘으면 아무것도 바꾸지 않고 실패한다. */
	if (ret)
		return ret;

	/* [한국어] 옛 주소 공간이 아직 살아 있으면 */
	if (mmget_not_zero(dma->mm)) {
		/* [한국어] 같은 양을 뺀다. 이미 죽었으면 종료 시 회계가 함께 사라졌으므로 뺄 필요가 없다. */
		mm_lock_acct(dma->task, dma->mm, dma->lock_cap, -npage);
		/* [한국어] 위 mmget_not_zero 로 승격했던 참조를 놓는다. */
		mmput(dma->mm);
	}

	/* [한국어] 회계 주체 task 도 달라졌으면 */
	if (dma->task != task) {
		put_task_struct(dma->task);
		/* [한국어] 새 프로세스 대표의 참조를 잡는다. */
		dma->task = get_task_struct(task);
	}
	/* [한국어] 옛 주소 공간 구조체 참조를 놓는다. */
	mmdrop(dma->mm);
	/* [한국어] 새 주소 공간을 기록하고 */
	dma->mm = mm;
	/* [한국어] 새 주소 공간 구조체 참조를 잡는다. */
	mmgrab(dma->mm);
	/* [한국어] 권한 스냅숏도 갱신한다. */
	dma->lock_cap = lock_cap;
	return 0;
}

/* [한국어]
 * vfio_dma_do_map - VFIO_IOMMU_MAP_DMA 의 본체. 사용자 버퍼를 IOVA 에 매핑한다
 *
 * @iommu: 컨테이너 객체.
 * @map: 사용자에게서 복사해 온 요청(iova, vaddr, size, flags).
 * @return: 0 성공, -EINVAL/-EOVERFLOW/-EEXIST/-ENOENT/-ENOSPC/-ENOMEM 등.
 *
 * 이 파일이 존재하는 이유 그 자체인 함수다. 사용자 공간 드라이버가 자기 힙 버퍼를
 * 장치가 DMA 할 수 있는 주소로 만들려면 이 ioctl 을 부른다. 반환 후에는 사용자가
 * 지정한 iova 로 장치가 그 버퍼에 직접 읽고 쓸 수 있다. 커널 안의 NVMe 경로에서
 * DMA API 가 하던 일(페이지 고정 + IOMMU 매핑 + 주소 확정)을 여기서 한 번에 한다.
 *
 * 두 가지 모드를 처리한다:
 *  (a) 일반 매핑 — READ/WRITE 플래그가 있고 VADDR 플래그는 없다.
 *  (b) vaddr 갱신 — VADDR 플래그만 있다. 기존 매핑의 사용자 주소만 새로 지정한다.
 *  두 모드를 동시에 요구하거나 둘 다 아니면 -EINVAL 이다.
 *
 * 동작 단계:
 *  1. __u64 인자를 size_t/dma_addr_t/unsigned long 으로 좁혀 담고, 값이 보존되었는지
 *     확인한다(32비트 커널 방어).
 *  2. size 가 0 이면 거절하고, iova+size-1 과 vaddr+size-1 의 오버플로를 확인한다.
 *     끝을 -1 로 계산하는 규약은 이 파일 전체에서 동일하다.
 *  3. 사용자 플래그를 IOMMU_READ/IOMMU_WRITE 로 변환한다. 상류 주석대로 방향은
 *     장치 관점이다 — WRITE 는 장치가 메모리에 쓴다는 뜻이다.
 *  4. iommu->lock 을 잡는다.
 *  5. 최소 지원 페이지 크기를 구하고, size/iova/vaddr 셋을 한꺼번에 OR 해서
 *     (pgsize - 1) 로 정렬을 검사한다. 셋 중 하나라도 어긋나면 결과가 0 이 아니다.
 *  6. 겹치는 매핑을 찾는다.
 *     - VADDR 모드: 매핑이 없으면 -ENOENT. 있어도 무효 표시가 아니거나 시작/크기가
 *       정확히 일치하지 않으면 -EINVAL — 부분 갱신은 허용하지 않는다. 통과하면
 *       vfio_change_dma_owner() 로 회계를 옮기고 vaddr 를 갱신한 뒤 무효 카운트를 줄인다.
 *     - 일반 모드: 이미 겹치는 매핑이 있으면 -EEXIST. 이 트리의 겹침 금지 불변식을
 *       지키는 지점이다.
 *  7. dma_avail 이 0 이면 -ENOSPC. 사용자가 매핑을 남발해 커널 메모리를 고갈시키는
 *     것을 막는다.
 *  8. vfio_iommu_iova_dma_valid() 로 요청 구간이 허용된 IOVA 창 안인지 확인한다.
 *  9. vfio_dma 를 0 초기화 할당하고 dma_avail 을 하나 소비한다.
 * 10. iova/vaddr/prot 를 채운다.
 * 11. 상류 주석이 설명하는 소유자 고정: group_leader 의 task 참조와 mm 참조를 잡고,
 *     CAP_IPC_LOCK 여부를 지금 굳혀 둔다. 핀은 mdev 경로를 통해 나중에 비동기로도
 *     일어날 수 있어, 그때 current 를 보면 엉뚱한 주체가 되기 때문이다.
 * 12. pfn_list 를 빈 트리로 초기화한다.
 * 13. 상류 주석대로 크기 0 인 빈 노드를 먼저 트리에 넣는다. 겹침 검사는 이미 끝났고,
 *     이후 매핑이 진행되는 만큼 size 를 키운다.
 * 14. domain 이 하나도 없으면(mdev 전용 컨테이너) 핀도 매핑도 하지 않고 size 만
 *     채운다. 실제 핀은 나중에 mdev 가 pin_pages 를 부를 때 일어난다.
 *     domain 이 있으면 vfio_pin_map_dma() 로 핀+매핑을 진행한다.
 * 15. dirty 추적이 켜져 있으면 이 매핑의 비트맵도 만든다. 실패하면 매핑을 통째로
 *     없앤다 — 비트맵 없는 매핑이 추적 중에 존재하면 이후 코드가 NULL 을 참조한다.
 * 16. out_unlock 에서 락을 놓고 결과를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. ioctl 경로에서만 불린다. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_map_dma() 한 곳뿐.
 * callee: vfio_find_dma(), vfio_change_dma_owner(), vfio_iommu_iova_dma_valid(),
 *   kzalloc_obj(), get_task_struct(), capable(), mmgrab(), vfio_link_dma(),
 *   vfio_pin_map_dma(), vfio_dma_bitmap_alloc(), vfio_remove_dma().
 * 에러 경로: 14 번 이후의 실패는 vfio_pin_map_dma()/vfio_remove_dma() 가 매핑을
 *   완전히 되돌린다. 그 이전 실패는 아직 할당한 것이 없거나 kfree 만 남는다.
 *
 * 호출 체인:
 *   container.c:353 ops->ioctl → vfio_iommu_type1_ioctl() →
 *   vfio_iommu_type1_map_dma() → [vfio_dma_do_map] → vfio_pin_map_dma()
 */
static int vfio_dma_do_map(struct vfio_iommu *iommu,
			   struct vfio_iommu_type1_dma_map *map)
{
	bool set_vaddr = map->flags & VFIO_DMA_MAP_FLAG_VADDR;
	/* [한국어] 사용자가 요청한 IOVA 를 dma_addr_t 로 좁혀 담는다. */
	dma_addr_t iova = map->iova;
	/* [한국어] 그 구간의 마지막 바이트 주소. */
	dma_addr_t iova_end;
	/* [한국어] 사용자 버퍼의 시작 가상 주소. */
	unsigned long vaddr = map->vaddr;
	/* [한국어] 그 버퍼의 마지막 바이트 주소. */
	unsigned long vaddr_end;
	/* [한국어] 매핑 길이. */
	size_t size = map->size;
	/* [한국어] 반환값과, 사용자 플래그에서 변환할 IOMMU 권한 비트. */
	int ret = 0, prot = 0;
	/* [한국어] 컨테이너가 강제하는 최소 IOMMU 페이지 크기. */
	size_t pgsize;
	/* [한국어] 새로 만들거나 찾아낼 매핑. */
	struct vfio_dma *dma;

	/* Verify that none of our __u64 fields overflow */
	/* [한국어] __u64 인자를 좁은 타입에 담았을 때 값이 보존되었는지 확인한다. */
	if (map->size != size || map->vaddr != vaddr || map->iova != iova)
		/* [한국어] 값이 잘렸으면 32비트 커널에서 64비트 인자가 손실된 것이다. */
		return -EOVERFLOW;

	/* [한국어] 길이 0 매핑은 뜻이 없다. */
	if (!size)
		return -EINVAL;

	/* [한국어] IOVA 쪽 닫힌 구간 끝을 구하면서 오버플로를 확인하고 */
	if (check_add_overflow(iova, size - 1, &iova_end) ||
	    check_add_overflow(vaddr, size - 1, &vaddr_end))
		return -EOVERFLOW;

	/* READ/WRITE from device perspective */
	/* [한국어] 사용자가 쓰기 권한을 요청했으면 */
	if (map->flags & VFIO_DMA_MAP_FLAG_WRITE)
		/* [한국어] 장치가 메모리에 쓸 수 있게 한다. */
		prot |= IOMMU_WRITE;
	/* [한국어] 사용자가 읽기 권한을 요청했으면 */
	if (map->flags & VFIO_DMA_MAP_FLAG_READ)
		/* [한국어] 장치가 메모리를 읽을 수 있게 한다. */
		prot |= IOMMU_READ;

	/* [한국어] 권한 지정과 vaddr 갱신은 서로 배타적이다. 둘 다 주었거나 둘 다 주지 않았으면
	 * 무엇을 하려는지 알 수 없으므로 거절한다. */
	if ((prot && set_vaddr) || (!prot && !set_vaddr))
		return -EINVAL;

	/* [한국어] 컨테이너 상태와 매핑 트리를 바꾸므로 락이 필요하다. */
	mutex_lock(&iommu->lock);

	/* [한국어] 컨테이너의 최소 지원 IOMMU 페이지 크기. __ffs 로 최하위 1 비트 위치를 찾아 2 의 거듭제곱으로 만든다. */
	pgsize = (size_t)1 << __ffs(iommu->pgsize_bitmap);

	/* [한국어] 그 크기가 PAGE_SIZE 보다 작으면 안 된다 — vfio_update_pgsize_bitmap() 이 보장하는 불변식이다. */
	WARN_ON((pgsize - 1) & PAGE_MASK);

	/* [한국어] 길이·IOVA·vaddr 셋을 OR 해 한 번에 정렬을 검사한다. 하나라도 어긋나면 결과가 0 이 아니다. */
	if ((size | iova | vaddr) & (pgsize - 1)) {
		ret = -EINVAL;
		/* [한국어] 정렬 위반. */
		goto out_unlock;
	}

	/* [한국어] 요청 구간과 겹치는 매핑이 이미 있는지 본다. */
	dma = vfio_find_dma(iommu, iova, size);
	/* [한국어] vaddr 갱신 모드. */
	if (set_vaddr) {
		/* [한국어] 갱신할 매핑이 없으면 */
		if (!dma) {
			ret = -ENOENT;
		/* [한국어] 무효 표시가 아니거나 시작/크기가 정확히 일치하지 않으면 거절한다 — 부분 갱신은 지원하지 않는다. */
		} else if (!dma->vaddr_invalid || dma->iova != iova ||
			   dma->size != size) {
			ret = -EINVAL;
		} else {
			/* [한국어] 잠금 메모리 회계를 새 소유자 앞으로 옮긴다. */
			ret = vfio_change_dma_owner(dma);
			/* [한국어] 새 소유자가 한도를 넘으면 여기서 실패하고 매핑은 그대로 남는다. */
			if (ret)
				goto out_unlock;
			/* [한국어] 새 사용자 주소를 기록한다. */
			dma->vaddr = vaddr;
			/* [한국어] 무효 표시를 내린다. */
			dma->vaddr_invalid = false;
			/* [한국어] 컨테이너의 무효 카운트도 줄인다. 이걸 빠뜨리면 이후 모든 핀/rw 가 -EBUSY 가 된다. */
			iommu->vaddr_invalid_count--;
		}
		/* [한국어] vaddr 갱신 경로의 공통 출구 — 성공이든 실패든 여기서 락을 놓는다. */
		goto out_unlock;
	/* [한국어] 일반 매핑인데 이미 겹치는 매핑이 있으면 겹침 금지 불변식을 어긴다. */
	} else if (dma) {
		ret = -EEXIST;
		/* [한국어] 이미 겹치는 매핑이 있음. */
		goto out_unlock;
	}

	/* [한국어] 매핑 슬롯이 다 떨어졌으면 커널 메모리 고갈을 막기 위해 거절한다. */
	if (!iommu->dma_avail) {
		ret = -ENOSPC;
		/* [한국어] 매핑 슬롯 소진. */
		goto out_unlock;
	}

	/* [한국어] 요청 구간이 허용된 IOVA 창 안인지 확인한다. */
	if (!vfio_iommu_iova_dma_valid(iommu, iova, iova_end)) {
		ret = -EINVAL;
		/* [한국어] 매핑 객체 할당 실패. */
		goto out_unlock;
	}

	/* [한국어] 매핑 객체를 0 초기화 할당한다. */
	dma = kzalloc_obj(*dma);
	/* [한국어] 할당 실패. */
	if (!dma) {
		ret = -ENOMEM;
		/* [한국어] IOVA 창 밖이면 매핑할 수 없다. */
		goto out_unlock;
	}

	/* [한국어] 슬롯 하나를 소비한다. vfio_remove_dma() 가 돌려준다. */
	iommu->dma_avail--;
	/* [한국어] 장치가 쓸 주소. */
	dma->iova = iova;
	/* [한국어] 대응하는 사용자 주소. 두 주소의 차가 매핑 내내 일정하다. */
	dma->vaddr = vaddr;
	/* [한국어] 접근 권한. */
	dma->prot = prot;

	/*
	 * We need to be able to both add to a task's locked memory and test
	 * against the locked memory limit and we need to be able to do both
	 * outside of this call path as pinning can be asynchronous via the
	 * external interfaces for mdev devices.  RLIMIT_MEMLOCK requires a
	 * task_struct. Save the group_leader so that all DMA tracking uses
	 * the same task, to make debugging easier.  VM locked pages requires
	 * an mm_struct, so grab the mm in case the task dies.
	 */
	/* [한국어] 프로세스 대표 task 의 참조를 잡는다. vfio_remove_dma() 의 put_task_struct 와 짝이다. */
	get_task_struct(current->group_leader);
	/* [한국어] 회계 주체를 스레드가 아니라 프로세스 대표로 굳혀 둔다. */
	dma->task = current->group_leader;
	/* [한국어] CAP_IPC_LOCK 보유 여부도 지금 굳혀 둔다 — 언핀은 프로세스가 죽은 뒤에도 일어날 수 있다. */
	dma->lock_cap = capable(CAP_IPC_LOCK);
	/* [한국어] 대상 주소 공간을 기록한다. */
	dma->mm = current->mm;
	/* [한국어] 주소 공간 구조체 참조를 잡는다. 프로세스가 먼저 죽어도 회계 대상이 유지된다. */
	mmgrab(dma->mm);

	/* [한국어] 외부 핀 기록용 트리를 빈 상태로 시작한다. */
	dma->pfn_list = RB_ROOT;

	/* Insert zero-sized and grow as we map chunks of it */
	/* [한국어] 크기 0 인 빈 노드를 먼저 트리에 넣는다. 겹침 검사는 이미 끝났고, 매핑이 진행되는 만큼 크기가 자란다. */
	vfio_link_dma(iommu, dma);

	/* Don't pin and map if container doesn't contain IOMMU capable domain*/
	/* [한국어] domain 이 없는 컨테이너(mdev 전용)면 */
	if (list_empty(&iommu->domain_list))
		/* [한국어] domain 이 없으면 지금 핀하지 않으므로 크기만 채워 둔다. 실제 핀은 mdev 가 pin_pages 를 부를 때 일어난다. */
		dma->size = size;
	/* [한국어] domain 이 있으면 실제로 핀하고 매핑해야 한다. */
	else
		/* [한국어] domain 이 있으면 청크 단위로 핀하고 IOMMU 에 설치한다. */
		ret = vfio_pin_map_dma(iommu, dma, size);

	/* [한국어] 매핑에 성공했고 dirty 추적 중이면 이 매핑의 비트맵도 만들어야 한다. */
	if (!ret && iommu->dirty_page_tracking) {
		/* [한국어] 비트맵을 할당한다. */
		ret = vfio_dma_bitmap_alloc(dma, pgsize);
		/* [한국어] 실패하면 */
		if (ret)
			/* [한국어] 매핑을 통째로 없앤다 — 추적 중에 비트맵 없는 매핑이 남으면 이후 코드가 NULL 을 참조한다. */
			vfio_remove_dma(iommu, dma);
	}

out_unlock:
	/* [한국어] 락을 놓는다. 모든 경로가 여기로 모인다. */
	mutex_unlock(&iommu->lock);
	return ret;
}

/* [한국어]
 * vfio_iommu_replay - 기존 매핑 전체를 새로 추가된 IOMMU domain 에 다시 설치한다
 *
 * @iommu: 컨테이너 객체.
 * @domain: 방금 추가된, 아직 매핑이 하나도 없는 domain.
 * @return: 0 성공, 음수면 오류(이때 부분 설치는 모두 되돌려진다).
 *
 * 컨테이너에 이미 매핑이 있는 상태에서 새 장치 group 이 붙으면, 그 장치도 같은
 * IOVA 로 같은 버퍼에 닿아야 한다. 그래서 dma_list 전체를 훑어 새 domain 에 똑같이
 * 설치한다. 게스트가 실행 중인 상태에서 장치를 추가(hotplug)할 때 쓰이는 경로다.
 *
 * 두 가지 상황을 나누어 다룬다:
 *  (a) 이미 다른 domain 에 매핑되어 있는 vfio_dma — 페이지는 이미 핀되어 있으므로
 *      iommu_iova_to_phys() 로 기존 domain 에서 물리 주소를 읽어 그대로 새 domain 에
 *      설치한다. 새로 핀하지 않으므로 과금도 늘지 않는다.
 *  (b) 아직 매핑되지 않은 vfio_dma(domain 이 없던 mdev 전용 컨테이너에 처음
 *      하드웨어 domain 이 붙는 경우) — 지금 핀해야 하므로 vfio_pin_pages_remote() 를
 *      부른다. 이때는 핀과 과금이 함께 늘어난다.
 *
 * 동작 단계:
 *  1. 기존 domain 이 있으면 첫 번째를 조회용(d)으로 잡는다. 상류 주석대로 어느
 *     것이든 상관없다 — 모든 domain 이 같은 번역을 갖고 있기 때문이다.
 *  2. GUP 배치를 준비하고 dma_list 를 오름차순 순회한다.
 *  3. 각 매핑 안에서 pos 를 0 부터 size 까지 전진시킨다.
 *     - (a) 경우: 조회용 domain 이 없으면 WARN_ON 후 -EINVAL(매핑되어 있는데 domain 이
 *       없다는 모순). 물리 주소를 읽어 오고, 0 이면 WARN_ON 후 한 페이지 건너뛴다.
 *       그 다음 물리적으로 이어지는 동안 size 를 키워 한 번에 설치할 길이를 늘린다.
 *     - (b) 경우: 남은 길이만큼 vfio_pin_pages_remote() 로 핀하고, 얻은 pfn 과 길이를
 *       쓴다. 0 이하면 오류다.
 *     - iommu_map() 으로 새 domain 에 설치한다. 실패하면, 방금 핀한 경우에만
 *       핀을 되돌린 뒤(이미 핀되어 있던 (a) 는 되돌리면 안 된다) unwind 로 간다.
 *     - 성공한 만큼 pos 를 전진시킨다.
 *  4. 상류 주석대로, 모든 매핑이 설치된 뒤에야 iommu_mapped 를 true 로 세운다.
 *     루프 도중에 세우면 실패 시 unwind 가 (a)/(b) 를 잘못 구분하게 된다.
 *  5. unwind 라벨: 실패한 노드부터 rb_prev() 로 거슬러 올라가며 되돌린다.
 *     - 이미 매핑되어 있던 매핑은 새 domain 에서 언맵만 하면 된다(핀은 남겨 둔다).
 *     - 이번에 핀한 매핑은 물리 주소를 새 domain 에서 읽어 언맵하고 핀도 푼다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. GUP 와 페이지 테이블
 * 할당으로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_attach_group() 한 곳뿐.
 * callee: vfio_batch_init(), iommu_iova_to_phys(), vfio_pin_pages_remote(),
 *   iommu_map(), iommu_unmap(), vfio_unpin_pages_remote(), vfio_batch_unpin(),
 *   vfio_batch_fini().
 * 에러 경로: unwind 후 오류를 반환하고, 호출자는 group 을 detach 하고 domain 을
 *   해제한다.
 *
 * [상류 코드 관찰] unwind 라벨 안쪽 while 루프(pos < dma->size)는 iommu_unmap() 과
 * vfio_unpin_pages_remote() 를 부른 뒤 pos 를 size 만큼 전진시키지 않는다. 그래도
 * 무한 루프가 되지 않는 이유는, 방금 언맵해서 같은 iova 의 iommu_iova_to_phys() 가
 * 0 을 돌려주고 그 경로가 pos 를 PAGE_SIZE 씩 올리기 때문이다. 결과적으로 되감기가
 * 페이지 단위로 다시 훑는 형태가 되어 느리다. 원본(1f0e418bb6)에서 확인했으며
 * 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_replay] → iommu_map()
 */
static int vfio_iommu_replay(struct vfio_iommu *iommu,
			     struct vfio_domain *domain)
{
	struct vfio_batch batch;
	/* [한국어] 물리 주소 조회용으로 쓸 기존 domain. 없을 수도 있다. */
	struct vfio_domain *d = NULL;
	/* [한국어] dma_list 순회용 커서. */
	struct rb_node *n;
	/* [한국어] RLIMIT_MEMLOCK 한도를 페이지 수로 환산한 값. */
	unsigned long limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;
	/* [한국어] 결과 코드. */
	int ret;

	/* Arbitrarily pick the first domain in the list for lookups */
	/* [한국어] 기존 domain 이 하나라도 있으면 */
	if (!list_empty(&iommu->domain_list))
		/* [한국어] 첫 domain 을 조회용으로 잡는다. 모든 domain 이 같은 번역을 갖고 있으므로 어느 것이든 된다. */
		d = list_first_entry(&iommu->domain_list,
				     struct vfio_domain, next);

	/* [한국어] GUP 배치를 준비한다. 대량으로 핀할 수 있으므로 다중 페이지 모드다. */
	vfio_batch_init(&batch);

	/* [한국어] 가장 낮은 IOVA 의 매핑부터 시작한다. */
	n = rb_first(&iommu->dma_list);

	/* [한국어] 모든 매핑을 오름차순으로 훑는다. */
	for (; n; n = rb_next(n)) {
		/* [한국어] 현재 매핑. */
		struct vfio_dma *dma;
		/* [한국어] 이 매핑 안에서의 진행 위치. */
		size_t pos = 0;

		/* [한국어] 노드에서 구조체를 복원한다. */
		dma = rb_entry(n, struct vfio_dma, node);

		/* [한국어] 매핑 전체를 덮을 때까지 반복한다. */
		while (pos < dma->size) {
			/* [한국어] 이번에 처리할 IOVA. */
			dma_addr_t iova = dma->iova + pos;
			/* [한국어] 그에 대응하는 호스트 물리 주소. */
			phys_addr_t phys;
			/* [한국어] 한 번에 설치할 길이. */
			size_t size;

			/* [한국어] 이미 다른 domain 에 매핑되어 있으면 페이지는 이미 핀되어 있다. */
			if (dma->iommu_mapped) {
				/* [한국어] 연속 길이 탐색용 임시 물리 주소. */
				phys_addr_t p;
				/* [한국어] 연속 길이 탐색용 임시 IOVA. */
				dma_addr_t i;

				/* [한국어] 매핑되어 있다는데 조회할 domain 이 없으면 모순이다. */
				if (WARN_ON(!d)) { /* mapped w/o a domain?! */
					ret = -EINVAL;
					/* [한국어] 매핑되어 있다는데 조회할 domain 이 없는 모순 — 되감기로 간다. */
					goto unwind;
				}

				/* [한국어] 기존 domain 에서 물리 주소를 얻는다 — 이 파일은 pfn 을 따로 보관하지 않는다. */
				phys = iommu_iova_to_phys(d->domain, iova);

				/* [한국어] 있어야 할 번역이 없으면 경고하고 한 페이지 건너뛴다. */
				if (WARN_ON(!phys)) {
					/* [한국어] 진행 위치만 한 페이지 올린다. */
					pos += PAGE_SIZE;
					continue;
				}

				/* [한국어] 최소 한 페이지는 확보되었다. */
				size = PAGE_SIZE;
				/* [한국어] 다음 페이지의 예상 물리 주소. */
				p = phys + size;
				/* [한국어] 다음 페이지의 IOVA. */
				i = iova + size;
				/* [한국어] 매핑 끝을 넘지 않는 동안, 물리 주소가 계속 이어지면 */
				while (pos + size < dma->size &&
				       p == iommu_iova_to_phys(d->domain, i)) {
					/* [한국어] 설치 길이를 한 페이지 늘리고 */
					size += PAGE_SIZE;
					/* [한국어] 예상 물리 주소도 한 페이지 전진시킨다. */
					p += PAGE_SIZE;
					/* [한국어] 조회할 IOVA 도 마찬가지. */
					i += PAGE_SIZE;
				}
			} else {
				/* [한국어] 얻은 pfn. */
				unsigned long pfn;
				/* [한국어] 핀할 사용자 가상 주소. */
				unsigned long vaddr = dma->vaddr + pos;
				/* [한국어] 이 매핑에 남은 바이트 수. */
				size_t n = dma->size - pos;
				/* [한국어] 핀에 성공한 페이지 수. */
				long npage;

				/* [한국어] 아직 매핑된 적이 없으므로 지금 핀해야 한다. 핀과 과금이 함께 늘어난다. */
				npage = vfio_pin_pages_remote(dma, vaddr,
							      n >> PAGE_SHIFT,
							      &pfn, limit,
							      &batch);
				/* [한국어] 0 이하면 오류다. */
				if (npage <= 0) {
					/* [한국어] 0 은 있을 수 없다. */
					WARN_ON(!npage);
					/* [한국어] 음수 오류를 int 로 좁혀 반환값으로 삼는다. */
					ret = (int)npage;
					/* [한국어] 핀 실패 — 되감기로 간다. */
					goto unwind;
				}

				/* [한국어] pfn 을 물리 주소로 환산한다. */
				phys = pfn << PAGE_SHIFT;
				/* [한국어] 핀한 페이지 수를 바이트 길이로 환산한다. */
				size = npage << PAGE_SHIFT;
			}

			/* [한국어] 새 domain 에 설치한다. IOMMU_CACHE 를 더해 캐시 일관 처리를 요청한다. */
			ret = iommu_map(domain->domain, iova, phys, size,
					dma->prot | IOMMU_CACHE,
					GFP_KERNEL_ACCOUNT);
			/* [한국어] 설치 실패. */
			if (ret) {
				/* [한국어] 이번에 새로 핀한 경우에만 핀을 되돌린다 — 원래 핀되어 있던 것은 남겨야 한다. */
				if (!dma->iommu_mapped) {
					/* [한국어] 방금 핀한 만큼 되돌린다. */
					vfio_unpin_pages_remote(dma, iova,
							phys >> PAGE_SHIFT,
							size >> PAGE_SHIFT,
							true);
					/* [한국어] 배치에 남은 잔여 핀도 되돌린다. */
					vfio_batch_unpin(&batch, dma);
				}
				/* [한국어] 설치 실패 — 되감기로 간다. */
				goto unwind;
			}

			/* [한국어] 설치한 만큼 진행 위치를 전진시킨다. */
			pos += size;
		}
	}

	/* All dmas are now mapped, defer to second tree walk for unwind */
	/* [한국어] 모든 매핑이 설치되었으니 이제 표시를 세우러 다시 훑는다. */
	for (n = rb_first(&iommu->dma_list); n; n = rb_next(n)) {
		/* [한국어] 모든 매핑이 설치된 뒤에야 노드에서 구조체를 복원해 */
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);

		/* [한국어] 매핑됨 표시를 세운다. 루프 도중에 세우면 unwind 가 (a)/(b) 를 잘못 구분한다. */
		dma->iommu_mapped = true;
	}

	/* [한국어] 성공 경로의 배치 배열 해제. */
	vfio_batch_fini(&batch);
	return 0;

unwind:
	/* [한국어] 실패한 노드부터 rb_prev 로 거슬러 올라가며 되돌린다. */
	for (; n; n = rb_prev(n)) {
		/* [한국어] 되감을 매핑. */
		struct vfio_dma *dma = rb_entry(n, struct vfio_dma, node);
		/* [한국어] 이 매핑 안에서의 진행 위치. */
		size_t pos = 0;

		/* [한국어] 원래 매핑되어 있던 것이면 새 domain 에서 언맵만 하면 된다 — 핀은 남겨야 한다. */
		if (dma->iommu_mapped) {
			/* [한국어] 새 domain 의 번역만 걷어낸다. */
			iommu_unmap(domain->domain, dma->iova, dma->size);
			continue;
		}

		/* [한국어] 이번에 핀한 매핑은 언맵과 언핀을 모두 해야 한다. */
		while (pos < dma->size) {
			/* [한국어] 이번에 처리할 IOVA. */
			dma_addr_t iova = dma->iova + pos;
			/* [한국어] 그 물리 주소와 연속 탐색용 임시값. */
			phys_addr_t phys, p;
			/* [한국어] 한 번에 되돌릴 길이. */
			size_t size;
			/* [한국어] 연속 탐색용 임시 IOVA. */
			dma_addr_t i;

			/* [한국어] 새 domain 에서 물리 주소를 얻는다. */
			phys = iommu_iova_to_phys(domain->domain, iova);
			/* [한국어] 번역이 없으면 이미 되돌렸거나 설치되지 않은 부분이다. */
			if (!phys) {
				/* [한국어] 한 페이지 건너뛴다. */
				pos += PAGE_SIZE;
				continue;
			}

			/* [한국어] 최소 한 페이지. */
			size = PAGE_SIZE;
			/* [한국어] 다음 페이지의 예상 물리 주소. */
			p = phys + size;
			/* [한국어] 다음 페이지의 IOVA. */
			i = iova + size;
			/* [한국어] 매핑 끝을 넘지 않는 동안 물리 주소가 이어지면 */
			while (pos + size < dma->size &&
			       p == iommu_iova_to_phys(domain->domain, i)) {
				/* [한국어] 되돌릴 길이를 늘리고 */
				size += PAGE_SIZE;
				/* [한국어] 예상 물리 주소를 전진시키고 */
				p += PAGE_SIZE;
				/* [한국어] 조회 IOVA 도 전진시킨다. */
				i += PAGE_SIZE;
			}

			/* [한국어] 번역을 걷어낸다. */
			iommu_unmap(domain->domain, iova, size);
			/* [한국어] 핀을 풀고 과금도 되돌린다. */
			vfio_unpin_pages_remote(dma, iova, phys >> PAGE_SHIFT,
						size >> PAGE_SHIFT, true);
		}
	}

	/* [한국어] 되감기 경로에서도 배치 배열을 반드시 해제한다. */
	vfio_batch_fini(&batch);
	return ret;
}

/* [한국어]
 * find_iommu_group - 한 domain 의 group 목록에서 특정 iommu_group 을 찾는다
 *
 * @domain: 검색할 domain.
 * @iommu_group: 찾을 IOMMU group 포인터. 포인터 값 자체를 키로 비교한다.
 * @return: 찾은 vfio_iommu_group 래퍼, 없으면 NULL.
 *
 * group 수는 보통 한 자릿수라 선형 탐색으로 충분하다. 포인터 비교로 동일성을
 * 판정하는 이유는 iommu_group 객체가 IOMMU 코어에서 유일하게 관리되기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_find_iommu_group()(컨테이너 전체 검색),
 *   vfio_iommu_type1_detach_group()(어느 domain 에 있는지 찾기),
 *   vfio_iommu_type1_group_iommu_domain()(group 이 붙은 domain 조회).
 * callee: 없다.
 * 에러 경로: 없다. NULL 은 정상 결과다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_detach_group() → [find_iommu_group]
 */
static struct vfio_iommu_group *find_iommu_group(struct vfio_domain *domain,
						 struct iommu_group *iommu_group)
{
	struct vfio_iommu_group *g;

	/* [한국어] 이 domain 에 붙은 group 들을 선형 탐색한다. 개수가 작아 이진 탐색이 필요 없다. */
	list_for_each_entry(g, &domain->group_list, next) {
		/* [한국어] iommu_group 객체는 IOMMU 코어에서 유일하게 관리되므로 포인터 비교로 충분하다. */
		if (g->iommu_group == iommu_group)
			/* [한국어] 찾은 래퍼를 반환한다. */
			return g;
	}

	/* [한국어] 이 domain 에는 없다. */
	return NULL;
}

/* [한국어]
 * vfio_iommu_find_iommu_group - 컨테이너 전체에서 특정 iommu_group 을 찾는다
 *
 * @iommu: 컨테이너 객체.
 * @iommu_group: 찾을 IOMMU group.
 * @return: 찾은 vfio_iommu_group 래퍼, 없으면 NULL.
 *
 * group 은 두 곳 중 하나에 있다: 하드웨어 domain 에 붙어 있거나(domain_list 아래
 * group_list), 에뮬레이트 group 목록에 있거나. 이 함수는 두 곳을 모두 본다.
 *
 * 동작 단계:
 *  1. 모든 domain 을 돌며 find_iommu_group() 으로 찾는다.
 *  2. 없으면 emulated_iommu_groups 를 직접 훑는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_attach_group()(중복 attach 방지),
 *   vfio_iommu_type1_pin_pages()(dirty 추적 범위 승격).
 * callee: find_iommu_group().
 * 에러 경로: 없다.
 *
 * [상류 코드 관찰] 이 함수는 어느 목록에도 없으면 NULL 을 반환하는데,
 * vfio_iommu_type1_pin_pages() 는 반환값을 검사하지 않고 곧바로
 * group->pinned_page_dirty_scope 를 참조한다. 실제로는 핀에 성공한 장치의 group 이
 * 반드시 컨테이너에 붙어 있으므로 NULL 이 오지 않는다는 전제로 보인다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_find_iommu_group] →
 *   find_iommu_group()
 */
static struct vfio_iommu_group*
vfio_iommu_find_iommu_group(struct vfio_iommu *iommu,
			    struct iommu_group *iommu_group)
{
	struct vfio_iommu_group *group;
	/* [한국어] domain 목록 순회용 커서. */
	struct vfio_domain *domain;

	/* [한국어] 먼저 하드웨어 domain 들을 훑는다. */
	list_for_each_entry(domain, &iommu->domain_list, next) {
		/* [한국어] 이 domain 의 group 목록에서 찾는다. */
		group = find_iommu_group(domain, iommu_group);
		/* [한국어] 찾았으면 */
		if (group)
			/* [한국어] 그 래퍼를 반환한다. */
			return group;
	}

	/* [한국어] 하드웨어 쪽에 없으면 에뮬레이트 group 목록을 훑는다. */
	list_for_each_entry(group, &iommu->emulated_iommu_groups, next)
		/* [한국어] 포인터 값으로 동일성을 판정한다. */
		if (group->iommu_group == iommu_group)
			/* [한국어] 찾은 래퍼를 반환한다. */
			return group;
	/* [한국어] 두 목록 어디에도 없으면 이 컨테이너에 붙어 있지 않은 group 이다. */
	return NULL;
}

/* [한국어]
 * vfio_iommu_has_sw_msi - 예약 구간 목록에 소프트웨어 관리 MSI 창이 있는지 본다
 *
 * @group_resv_regions: IOMMU 코어가 알려 준 이 group 의 예약 구간 목록.
 * @base: 소프트웨어 MSI 창의 시작 주소를 받을 곳.
 * @return: true 면 소프트웨어 MSI 창이 있고 base 가 채워졌다.
 *
 * 배경: ARM 계열처럼 MSI 도어벨 주소가 IOMMU 를 거쳐 가는 플랫폼에서는, 장치가
 * 인터럽트를 쏘려면 도어벨 물리 주소가 IOVA 공간 어딘가에 매핑되어 있어야 한다.
 * IOMMU 코어가 그 창을 소프트웨어로 잡아 주는 방식이 IOMMU_RESV_SW_MSI 이고,
 * 하드웨어가 고정 창을 갖는 방식이 IOMMU_RESV_MSI 다.
 *
 * 동작 단계:
 *  1. 목록을 훑는다.
 *  2. 상류 주석대로 진짜 하드웨어 MSI 구간이 하나라도 있으면 소프트웨어 창은
 *     필요 없으므로 false 로 확정하고 곧바로 중단한다.
 *  3. 소프트웨어 MSI 구간을 만나면 시작 주소를 기록하고 true 로 둔다. 다만 뒤에
 *     하드웨어 구간이 나오면 2 번이 뒤집으므로 순서에 무관하게 하드웨어가 우선한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_attach_group() 한 곳뿐. true 이면 나중에
 *   iommu_get_msi_cookie() 로 그 창을 IOMMU 코어에 등록한다.
 * callee: 없다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_has_sw_msi]
 */
static bool vfio_iommu_has_sw_msi(struct list_head *group_resv_regions,
				  phys_addr_t *base)
{
	struct iommu_resv_region *region;
	/* [한국어] 기본값은 '소프트웨어 MSI 창 없음'. */
	bool ret = false;

	/* [한국어] 이 group 의 예약 구간을 훑는다. */
	list_for_each_entry(region, group_resv_regions, list) {
		/*
		 * The presence of any 'real' MSI regions should take
		 * precedence over the software-managed one if the
		 * IOMMU driver happens to advertise both types.
		 */
		/* [한국어] 하드웨어가 고정으로 잡아 둔 MSI 구간이 있으면 */
		if (region->type == IOMMU_RESV_MSI) {
			/* [한국어] 하드웨어 창이 있으면 소프트웨어 창은 필요 없다. */
			ret = false;
			break;
		}

		/* [한국어] 소프트웨어가 관리하는 MSI 창이면 */
		if (region->type == IOMMU_RESV_SW_MSI) {
			*base = region->start;
			/* [한국어] 소프트웨어 MSI 창을 찾았다고 기록한다. 뒤에 하드웨어 구간이 나오면 위에서 뒤집힌다. */
			ret = true;
		}
	}

	return ret;
}

/* [한국어]
 * vfio_iommu_iova_insert - IOVA 구간 하나를 목록의 지정 위치에 끼워 넣는다
 *
 * @head: 끼워 넣을 위치. 목록의 머리일 수도 있고, 특정 노드의 list 필드일 수도 있다.
 * @start: 구간 시작(포함).
 * @end: 구간 끝(포함).
 * @return: 0 성공, -ENOMEM.
 *
 * 상류 주석이 설명하듯 이 목록은 처음에 domain geometry 하나로 시작해, 새 domain 이
 * 붙을 때 좁혀지고 예약 구간을 빼면서 여러 조각으로 나뉜다. head 에 목록 머리가
 * 아니라 기존 노드의 list 를 넘기면 그 노드 '앞' 에 삽입되는데, 그 성질을
 * vfio_iommu_resv_exclude() 가 정렬을 유지하며 쪼개는 데 이용한다.
 *
 * 동작 단계:
 *  1. vfio_iova 를 할당한다(0 초기화 없는 kmalloc_obj — 세 필드를 모두 채우므로 충분하다).
 *  2. list 를 자기 자신으로 초기화하고 start/end 를 채운다.
 *  3. list_add_tail(&region->list, head) — head 가 목록 머리면 맨 뒤에, 노드의 list 면
 *     그 노드 바로 앞에 들어간다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. GFP_KERNEL 할당이라 잠들 수 있다.
 *
 * caller: vfio_iommu_aper_resize()(빈 목록 초기화), vfio_iommu_resv_exclude()(쪼개기),
 *   vfio_iommu_iova_get_copy()(사본 만들기).
 * callee: kmalloc_obj(), INIT_LIST_HEAD(), list_add_tail().
 * 에러 경로: -ENOMEM 을 올리면 호출자가 지금까지 만든 사본을 vfio_iommu_iova_free()
 *   로 정리하고, 원래 목록은 건드리지 않은 채로 남긴다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → vfio_iommu_iova_get_copy() →
 *   [vfio_iommu_iova_insert] → kmalloc_obj()
 */
/*
 * This is a helper function to insert an address range to iova list.
 * The list is initially created with a single entry corresponding to
 * the IOMMU domain geometry to which the device group is attached.
 * The list aperture gets modified when a new domain is added to the
 * container if the new aperture doesn't conflict with the current one
 * or with any existing dma mappings. The list is also modified to
 * exclude any reserved regions associated with the device group.
 */
static int vfio_iommu_iova_insert(struct list_head *head,
				  dma_addr_t start, dma_addr_t end)
{
	struct vfio_iova *region;

	/* [한국어] 구간 노드를 할당한다. 세 필드를 모두 채우므로 0 초기화가 필요 없다. */
	region = kmalloc_obj(*region);
	/* [한국어] 할당 실패. */
	if (!region)
		return -ENOMEM;

	/* [한국어] 노드를 목록에 넣기 전에 자기 자신을 가리키도록 초기화한다. */
	INIT_LIST_HEAD(&region->list);
	/* [한국어] 구간의 시작. */
	region->start = start;
	/* [한국어] 구간의 끝(포함). */
	region->end = end;

	/* [한국어] head 가 목록 머리면 맨 뒤에, 기존 노드의 list 면 그 노드 바로 앞에 들어간다. */
	list_add_tail(&region->list, head);
	return 0;
}

/* [한국어]
 * vfio_iommu_aper_conflict - 새 domain 의 주소 창이 기존 상태와 충돌하는지 본다
 *
 * @iommu: 컨테이너 객체.
 * @start: 새 domain 이 다룰 수 있는 주소의 시작.
 * @end: 그 끝.
 * @return: true 면 충돌 — 이 group 을 붙일 수 없다.
 *
 * 새 domain 이 붙으면 컨테이너의 유효 IOVA 창은 기존 창과 새 창의 교집합으로
 * 좁아진다. 그런데 이미 그 바깥에 매핑이 존재한다면, 새 장치는 그 주소를 번역할 수
 * 없으므로 컨테이너의 약속(모든 장치가 같은 IOVA 로 같은 곳에 닿는다)이 깨진다.
 *
 * 동작 단계:
 *  1. 목록이 비어 있으면(mdev 전용 컨테이너) 제약이 없으므로 충돌 없음.
 *  2. 두 창이 아예 겹치지 않으면(start > 기존 끝 또는 end < 기존 시작) 교집합이
 *     공집합이므로 충돌이다.
 *  3. 새 창의 시작이 기존 시작보다 뒤라면, 그 사이 구간에 매핑이 있는지
 *     vfio_find_dma() 로 확인한다. 있으면 충돌이다.
 *  4. 새 창의 끝이 기존 끝보다 앞이라면, 그 뒤 구간도 같은 방식으로 확인한다.
 *     end + 1 을 시작으로, 길이를 last->end - end 로 주어 배타적 경계를 맞춘다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_attach_group() 한 곳뿐.
 * callee: vfio_find_dma().
 * 에러 경로: true 를 반환하면 호출자가 -EINVAL 로 attach 를 거절한다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_aper_conflict] → vfio_find_dma()
 */
/*
 * Check the new iommu aperture conflicts with existing aper or with any
 * existing dma mappings.
 */
static bool vfio_iommu_aper_conflict(struct vfio_iommu *iommu,
				     dma_addr_t start, dma_addr_t end)
{
	struct vfio_iova *first, *last;
	/* [한국어] 컨테이너의 현재 유효 IOVA 목록. */
	struct list_head *iova = &iommu->iova_list;

	/* [한국어] 목록이 비어 있으면(mdev 전용) 제약이 없으므로 충돌도 없다. */
	if (list_empty(iova))
		return false;

	/* Disjoint sets, return conflict */
	/* [한국어] 목록의 첫 구간 — 정렬되어 있으므로 이것이 현재 창의 시작이다. */
	first = list_first_entry(iova, struct vfio_iova, list);
	/* [한국어] 목록의 마지막 구간 — 정렬되어 있으므로 이것이 현재 창의 끝이다. */
	last = list_last_entry(iova, struct vfio_iova, list);
	/* [한국어] 두 창이 아예 겹치지 않으면 교집합이 공집합이라 붙일 수 없다. */
	if (start > last->end || end < first->start)
		return true;

	/* Check for any existing dma mappings below the new start */
	/* [한국어] 새 창의 시작이 기존 시작보다 뒤면 그 앞 구간을 검사해야 한다. */
	if (start > first->start) {
		/* [한국어] 새 창이 시작하기 전 구간에 이미 매핑이 있으면, 새 장치는 그 주소를 번역할 수 없다. */
		if (vfio_find_dma(iommu, first->start, start - first->start))
			return true;
	}

	/* Check for any existing dma mappings beyond the new end */
	/* [한국어] 새 창의 끝이 기존 끝보다 앞이면 그 뒤 구간을 검사해야 한다. */
	if (end < last->end) {
		/* [한국어] 새 창이 끝난 뒤 구간도 같은 이유로 검사한다. end+1 부터 기존 끝까지의 길이다. */
		if (vfio_find_dma(iommu, end + 1, last->end - end))
			return true;
	}

	return false;
}

/* [한국어]
 * vfio_iommu_aper_resize - IOVA 목록을 새 주소 창 [start, end] 로 잘라 맞춘다
 *
 * @iova: 대상 목록(대개 원본이 아니라 작업용 사본).
 * @start: 새 창의 시작.
 * @end: 새 창의 끝.
 * @return: 0 성공, -ENOMEM(빈 목록에 첫 구간을 만들 때만 가능).
 *
 * 상류 주석대로 이 함수는 충돌이 없다고 확인된 뒤에만 불린다. 목록 전체를
 * [start, end] 안으로 가두는 것이 하는 일이다.
 *
 * 동작 단계:
 *  1. 목록이 비었으면 [start, end] 하나짜리 목록을 만든다.
 *  2. 앞쪽 정리: 목록을 앞에서부터 훑는다.
 *     - start 가 노드 시작보다 앞이면 더 자를 것이 없으므로 중단.
 *     - start 가 노드 안에 있으면 그 노드의 시작을 start 로 올리고 중단.
 *     - 그 밖(노드 전체가 start 보다 앞)이면 노드를 통째로 지운다.
 *  3. 뒤쪽 정리: 다시 앞에서부터 훑는다.
 *     - end 가 노드 끝보다 뒤면 이 노드는 그대로 두고 continue.
 *     - end 가 노드 안에 있으면 노드의 끝을 end 로 내리고 continue.
 *     - 그 밖(노드 전체가 end 보다 뒤)이면 노드를 지운다.
 *     두 루프 모두 list_for_each_entry_safe 를 쓰는 이유는 순회 도중 노드를
 *     지우기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_attach_group()(새 domain 의 창 반영),
 *   vfio_iommu_resv_refresh()(detach 후 목록 재구성).
 * callee: vfio_iommu_iova_insert(), list_del(), kfree().
 * 에러 경로: -ENOMEM 이면 호출자가 사본을 버리고 원본 목록을 그대로 둔다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_aper_resize] →
 *   vfio_iommu_iova_insert()
 */
/*
 * Resize iommu iova aperture window. This is called only if the new
 * aperture has no conflict with existing aperture and dma mappings.
 */
static int vfio_iommu_aper_resize(struct list_head *iova,
				  dma_addr_t start, dma_addr_t end)
{
	struct vfio_iova *node, *next;

	/* [한국어] 목록이 비어 있으면(첫 domain) 통째로 하나 만들면 된다. */
	if (list_empty(iova))
		/* [한국어] 새 창 하나짜리 목록을 만든다. */
		return vfio_iommu_iova_insert(iova, start, end);

	/* Adjust iova list start */
	/* [한국어] 앞에서부터 훑으며 앞쪽을 정리한다. 순회 도중 지우므로 안전 순회를 쓴다. */
	list_for_each_entry_safe(node, next, iova, list) {
		/* [한국어] 새 시작이 이 노드보다 앞이면 앞쪽에 자를 것이 없다. */
		if (start < node->start)
			break;
		/* [한국어] 새 시작이 이 노드 안이면 */
		if (start >= node->start && start < node->end) {
			/* [한국어] 이 노드의 시작만 끌어올리고 끝낸다. */
			node->start = start;
			break;
		}
		/* Delete nodes before new start */
		/* [한국어] 새 시작보다 완전히 앞에 있는 노드는 창 밖이므로 지운다. */
		list_del(&node->list);
		/* [한국어] 노드를 해제한다. */
		kfree(node);
	}

	/* Adjust iova list end */
	/* [한국어] 다시 앞에서부터 훑으며 뒤쪽을 정리한다. */
	list_for_each_entry_safe(node, next, iova, list) {
		/* [한국어] 새 끝이 이 노드 끝보다 뒤면 이 노드는 온전히 남는다. */
		if (end > node->end)
			continue;
		/* [한국어] 새 끝이 이 노드 안이면 */
		if (end > node->start && end <= node->end) {
			/* [한국어] 이 노드의 끝만 끌어내린다. break 가 아니라 continue 인 것은 뒤에 더 지울 노드가 있을 수 있어서다. */
			node->end = end;
			continue;
		}
		/* Delete nodes after new end */
		/* [한국어] 새 끝보다 완전히 뒤에 있는 노드는 창 밖이므로 지운다. */
		list_del(&node->list);
		/* [한국어] 노드를 해제한다. */
		kfree(node);
	}

	return 0;
}

/* [한국어]
 * vfio_iommu_resv_conflict - 예약 구간이 기존 매핑과 겹치는지 본다
 *
 * @iommu: 컨테이너 객체.
 * @resv_regions: 붙이려는 group 의 예약 구간 목록.
 * @return: true 면 충돌 — 이 group 을 붙일 수 없다.
 *
 * 예약 구간은 그 group 의 장치가 특별한 용도로 쓰는 주소다(MSI 도어벨, 펌웨어가
 * 잡아 둔 직접 매핑 등). 그 주소에 이미 사용자 매핑이 있으면, 장치를 붙이는 순간
 * 사용자 데이터와 하드웨어 용도가 같은 주소를 두고 다투게 된다.
 *
 * 동작 단계:
 *  1. 각 예약 구간을 본다.
 *  2. IOMMU_RESV_DIRECT_RELAXABLE 은 건너뛴다 — '가능하면 직접 매핑하되 안 되면
 *     포기해도 되는' 완화된 종류라 충돌로 보지 않는다.
 *  3. 나머지는 vfio_find_dma(iommu, region->start, region->length) 로 겹치는 매핑이
 *     있는지 확인한다. 있으면 충돌이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_attach_group() 한 곳뿐.
 * callee: vfio_find_dma().
 * 에러 경로: true 면 호출자가 -EINVAL 로 거절한다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_resv_conflict] → vfio_find_dma()
 */
/*
 * Check reserved region conflicts with existing dma mappings
 */
static bool vfio_iommu_resv_conflict(struct vfio_iommu *iommu,
				     struct list_head *resv_regions)
{
	struct iommu_resv_region *region;

	/* Check for conflict with existing dma mappings */
	/* [한국어] 이 group 의 예약 구간을 하나씩 검사한다. */
	list_for_each_entry(region, resv_regions, list) {
		/* [한국어] 완화된 예약 구간은 겹쳐도 되므로 검사 대상이 아니다. */
		if (region->type == IOMMU_RESV_DIRECT_RELAXABLE)
			continue;

		/* [한국어] 이 예약 구간을 덮는 사용자 매핑이 이미 있으면 충돌이다. */
		if (vfio_find_dma(iommu, region->start, region->length))
			return true;
	}

	return false;
}

/* [한국어]
 * vfio_iommu_resv_exclude - 유효 IOVA 목록에서 예약 구간들을 도려낸다
 *
 * @iova: 대상 목록(작업용 사본).
 * @resv_regions: 도려낼 예약 구간 목록.
 * @return: 0 성공, -ENOMEM, -EINVAL(도려내고 나니 남은 구간이 없음).
 *
 * vfio_iommu_resv_conflict() 가 '겹치는 매핑이 없다' 를 확인한 뒤, 실제로 그 구간을
 * 사용자가 쓰지 못하도록 목록에서 빼는 것이 이 함수다. 구간 한가운데를 도려내면
 * 하나가 둘로 쪼개지므로 목록이 길어질 수 있다.
 *
 * 동작 단계:
 *  1. 각 예약 구간의 시작과 끝(포함)을 구한다.
 *  2. IOMMU_RESV_DIRECT_RELAXABLE 은 건너뛴다.
 *  3. 유효 구간 목록을 훑으며 겹치는 노드마다:
 *     - 전혀 겹치지 않으면 건너뛴다.
 *     - 상류 주석대로, 예약 구간 앞에 남는 부분이 있으면 [n->start, start-1] 을
 *       현재 노드 '앞' 에 삽입한다(&n->list 를 head 로 넘기면 그 앞에 들어간다).
 *     - 뒤에 남는 부분이 있으면 [end+1, n->end] 도 같은 자리에 삽입한다.
 *     - 그런 다음 원래 노드를 지운다. 결과적으로 목록의 정렬 순서가 유지된다.
 *     - 삽입에 실패하면 곧바로 오류를 반환한다.
 *  4. 다 도려내고 남은 구간이 하나도 없으면 -EINVAL — 사용자가 쓸 수 있는 주소가
 *     전혀 없는 컨테이너는 의미가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_attach_group(), vfio_iommu_resv_refresh().
 * callee: vfio_iommu_iova_insert(), list_del(), kfree().
 * 에러 경로: 호출자가 사본을 버리고 원본 목록을 그대로 둔다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_resv_exclude] →
 *   vfio_iommu_iova_insert()
 */
/*
 * Check iova region overlap with  reserved regions and
 * exclude them from the iommu iova range
 */
static int vfio_iommu_resv_exclude(struct list_head *iova,
				   struct list_head *resv_regions)
{
	struct iommu_resv_region *resv;
	/* [한국어] IOVA 목록 안전 순회용 커서 쌍 — 순회 도중 노드를 지운다. */
	struct vfio_iova *n, *next;

	/* [한국어] 각 예약 구간을 하나씩 도려낸다. */
	list_for_each_entry(resv, resv_regions, list) {
		/* [한국어] 이 예약 구간의 시작과 끝(포함). */
		phys_addr_t start, end;

		/* [한국어] 완화된 종류는 겹쳐도 무방하므로 도려내지 않는다. */
		if (resv->type == IOMMU_RESV_DIRECT_RELAXABLE)
			continue;

		/* [한국어] 예약 구간의 시작. */
		start = resv->start;
		/* [한국어] 끝은 길이에서 1 을 빼 닫힌 구간의 마지막 바이트로 만든다. */
		end = resv->start + resv->length - 1;

		/* [한국어] 유효 IOVA 목록을 훑으며 겹치는 노드를 찾는다. */
		list_for_each_entry_safe(n, next, iova, list) {
			/* [한국어] 삽입 결과. */
			int ret = 0;

			/* No overlap */
			/* [한국어] 예약 구간과 전혀 겹치지 않으면 도려낼 것이 없다. */
			if (start > n->end || end < n->start)
				continue;
			/*
			 * Insert a new node if current node overlaps with the
			 * reserve region to exclude that from valid iova range.
			 * Note that, new node is inserted before the current
			 * node and finally the current node is deleted keeping
			 * the list updated and sorted.
			 */
			/* [한국어] 예약 구간 앞에 남는 부분이 있으면 */
			if (start > n->start)
				/* [한국어] 예약 구간 앞에 남는 부분이 있으면 그 조각을 현재 노드 앞에 끼워 넣는다. */
				ret = vfio_iommu_iova_insert(&n->list, n->start,
							     start - 1);
			/* [한국어] 뒤에도 남는 부분이 있으면 그 조각도 같은 자리에 끼워 넣는다. */
			if (!ret && end < n->end)
				/* [한국어] 끝 다음 주소부터 원래 노드의 끝까지. */
				ret = vfio_iommu_iova_insert(&n->list, end + 1,
							     n->end);
			/* [한국어] 조각을 만들지 못했으면 목록이 일관되지 않은 상태이므로 곧바로 실패한다. */
			if (ret)
				return ret;

			/* [한국어] 쪼갠 조각들을 앞에 넣었으므로 원래 노드는 지운다 — 결과적으로 정렬이 유지된다. */
			list_del(&n->list);
			/* [한국어] 원래 노드를 해제한다. */
			kfree(n);
		}
	}

	/* [한국어] 다 도려내고 남은 구간이 하나도 없으면 사용자가 쓸 주소가 없다는 뜻이다. */
	if (list_empty(iova))
		return -EINVAL;

	return 0;
}

/* [한국어]
 * vfio_iommu_resv_free - IOMMU 코어가 만들어 준 예약 구간 목록을 해제한다
 *
 * @resv_regions: 해제할 목록.
 * @return: 없음.
 *
 * iommu_get_group_resv_regions() 가 채워 준 목록의 각 항목은 힙에 있으므로 다 쓰고
 * 나면 돌려주어야 한다. 목록에서 빼고 해제하는 것을 한 곳에 묶어 두었다.
 *
 * 실행 컨텍스트: 프로세스 문맥. attach 의 성공 경로에서는 iommu->lock 을 놓은 뒤에,
 * 실패 경로에서는 락을 쥔 채로 불린다 — 목록이 스택 지역이라 어느 쪽이든 안전하다.
 *
 * caller: vfio_iommu_type1_attach_group()(성공/실패 양쪽),
 *   vfio_iommu_resv_refresh() 의 done 라벨.
 * callee: list_del(), kfree().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_resv_free] → kfree()
 */
static void vfio_iommu_resv_free(struct list_head *resv_regions)
{
	struct iommu_resv_region *n, *next;

	/* [한국어] 안전 순회 — 지우면서 훑기 때문에 다음 노드를 미리 잡아 두는 형태가 필요하다. */
	list_for_each_entry_safe(n, next, resv_regions, list) {
		list_del(&n->list);
		/* [한국어] IOMMU 코어가 힙에 만들어 준 예약 구간 항목을 해제한다. */
		kfree(n);
	}
}

/* [한국어]
 * vfio_iommu_iova_free - vfio_iova 목록의 모든 노드를 해제한다
 *
 * @iova: 해제할 목록(원본 iommu->iova_list 일 수도, 작업용 사본일 수도 있다).
 * @return: 없음.
 *
 * 목록을 갈아 끼우는 vfio_iommu_iova_insert_copy() 가 옛 목록을 버릴 때, 그리고
 * 사본 작업이 실패해 되돌릴 때 쓰인다. list_for_each_entry_safe 를 쓰는 이유는
 * 순회 도중 노드를 해제하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_iova_get_copy() 의 실패 되감기, vfio_iommu_iova_insert_copy(),
 *   vfio_iommu_resv_refresh()(재구성 전 비우기), vfio_iommu_type1_attach_group() 과
 *   _detach_group() 의 실패 경로, vfio_iommu_type1_release().
 * callee: list_del(), kfree().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_release() → [vfio_iommu_iova_free] → kfree()
 */
static void vfio_iommu_iova_free(struct list_head *iova)
{
	struct vfio_iova *n, *next;

	/* [한국어] 안전 순회로 모든 vfio_iova 노드를 지운다. */
	list_for_each_entry_safe(n, next, iova, list) {
		list_del(&n->list);
		/* [한국어] vfio_iova 노드를 해제한다. */
		kfree(n);
	}
}

/* [한국어]
 * vfio_iommu_iova_get_copy - 유효 IOVA 목록의 깊은 사본을 만든다
 *
 * @iommu: 컨테이너 객체.
 * @iova_copy: 사본을 담을 빈 목록(호출자 스택의 LIST_HEAD).
 * @return: 0 성공, -ENOMEM.
 *
 * 상류 주석이 설명하듯, attach/detach 는 IOVA 목록을 여러 단계에 걸쳐 고치는데
 * 중간에 실패할 수 있다. 원본을 직접 고치면 실패 시 되돌릴 방법이 없으므로,
 * 사본에서 작업하고 전부 성공했을 때만 통째로 갈아 끼운다.
 *
 * 동작 단계:
 *  1. 원본 목록을 순서대로 훑으며 각 구간을 vfio_iommu_iova_insert() 로 사본 끝에
 *     붙인다. list_add_tail 이므로 정렬 순서가 그대로 유지된다.
 *  2. 중간에 실패하면 out_free 에서 지금까지 만든 사본을 모두 해제하고 오류를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_attach_group(), vfio_iommu_type1_detach_group().
 * callee: vfio_iommu_iova_insert(), vfio_iommu_iova_free().
 * 에러 경로: -ENOMEM. attach 는 그 값을 그대로 사용자에게 올리고,
 *   detach 는 반환값을 보지 않는다 — 사본이 비면 뒤 단계가 알아서 원본을 그대로 둔다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_iova_get_copy] →
 *   vfio_iommu_iova_insert()
 */
static int vfio_iommu_iova_get_copy(struct vfio_iommu *iommu,
				    struct list_head *iova_copy)
{
	struct list_head *iova = &iommu->iova_list;
	/* [한국어] 원본 목록 순회용 커서. */
	struct vfio_iova *n;
	/* [한국어] 삽입 결과. */
	int ret;

	/* [한국어] 원본을 앞에서부터 훑는다. */
	list_for_each_entry(n, iova, list) {
		/* [한국어] 각 구간을 사본 끝에 붙인다. list_add_tail 이라 정렬 순서가 유지된다. */
		ret = vfio_iommu_iova_insert(iova_copy, n->start, n->end);
		/* [한국어] 하나라도 실패하면 사본 전체를 버린다. */
		if (ret)
			goto out_free;
	}

	return 0;

out_free:
	/* [한국어] 실패했으므로 지금까지 만든 사본을 모두 버린다. */
	vfio_iommu_iova_free(iova_copy);
	return ret;
}

/* [한국어]
 * vfio_iommu_iova_insert_copy - 작업이 끝난 사본을 원본 IOVA 목록으로 갈아 끼운다
 *
 * @iommu: 컨테이너 객체.
 * @iova_copy: 완성된 사본 목록. 이 호출 후 비게 된다.
 * @return: 없음.
 *
 * '전부 성공했을 때만 반영한다' 는 규약의 마지막 단계다. 원본을 통째로 버리고
 * 사본을 그 자리에 옮겨 붙이므로 중간 상태가 노출되지 않는다.
 *
 * 동작 단계:
 *  1. vfio_iommu_iova_free() 로 원본 목록의 노드를 모두 해제한다.
 *  2. list_splice_tail() 로 사본의 노드들을 원본 머리 뒤에 통째로 옮긴다.
 *     노드를 복사하는 것이 아니라 링크만 옮기므로 추가 할당이 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_attach_group() 의 done 라벨,
 *   vfio_iommu_type1_detach_group() 의 갱신 성공 경로.
 * callee: vfio_iommu_iova_free(), list_splice_tail().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → [vfio_iommu_iova_insert_copy] →
 *   list_splice_tail()
 */
static void vfio_iommu_iova_insert_copy(struct vfio_iommu *iommu,
					struct list_head *iova_copy)
{
	struct list_head *iova = &iommu->iova_list;

	/* [한국어] 원본 목록의 노드를 모두 해제한다. */
	vfio_iommu_iova_free(iova);

	/* [한국어] 사본의 노드들을 원본 머리 뒤로 통째로 옮긴다. 링크만 옮기므로 추가 할당이 없다. */
	list_splice_tail(iova_copy, iova);
}

/* [한국어]
 * vfio_iommu_domain_alloc - group 의 대표 장치 하나로 IOMMU domain 을 만든다
 *
 * @dev: iommu_group_for_each_dev() 가 넘겨 주는 group 안의 장치.
 * @data: 결과를 담을 struct iommu_domain 포인터의 주소.
 * @return: 언제나 1 — 순회를 여기서 멈추라는 뜻이다.
 *
 * 왜 콜백 형태인가: IOMMU domain 을 만들려면 대표 장치가 하나 필요한데, group 에서
 * 장치를 꺼내는 안전한 방법이 iommu_group_for_each_dev() 순회뿐이다. 상류 주석대로
 * 직접 목록을 뒤지면 경쟁 조건이 생긴다. 그래서 순회를 쓰되 첫 장치에서 곧바로
 * 1 을 반환해 멈춘다 — 이 API 는 0 이 아닌 값을 받으면 순회를 중단한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 안에서 domain 할당이
 * 일어나므로 잠들 수 있다.
 *
 * caller: iommu_group_for_each_dev() 가 콜백으로 부른다. 유일한 등록 지점은
 *   vfio_iommu_type1_attach_group() 이다.
 * callee: iommu_paging_domain_alloc(). 내부는 drivers/iommu/ 가 없는 이 트리에서
 *   확인 못 함. 실패 시 ERR_PTR 을 돌려주므로 호출자가 IS_ERR 로 검사한다.
 * 에러 경로: 오류를 반환하지 않고, 실패한 ERR_PTR 을 그대로 data 에 담아 둔다.
 *   판정은 호출자의 몫이다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_attach_group() → iommu_group_for_each_dev() →
 *   [vfio_iommu_domain_alloc] → iommu_paging_domain_alloc()
 */
static int vfio_iommu_domain_alloc(struct device *dev, void *data)
{
	struct iommu_domain **domain = data;

	*domain = iommu_paging_domain_alloc(dev);
	/* [한국어] 0 이 아닌 값을 돌려주면 순회가 멈춘다 — 첫 장치 하나만 필요하기 때문이다. */
	return 1; /* Don't iterate */
}

/* [한국어]
 * vfio_iommu_type1_attach_group - IOMMU group 을 컨테이너에 붙인다
 *
 * @iommu_data: 컨테이너 객체.
 * @iommu_group: 붙일 IOMMU group.
 * @type: VFIO_IOMMU(하드웨어) / VFIO_EMULATED_IOMMU(mdev) / VFIO_NO_IOMMU 구분.
 * @return: 0 성공, -EBUSY/-EINVAL/-ENOMEM/-EPERM 등.
 *
 * vfio_iommu_driver_ops 의 attach_group 콜백이다. 사용자가 group fd 를 컨테이너에
 * 넣을 때 불린다. 하는 일은 (1) IOMMU domain 확보와 group attach,
 * (2) 유효 IOVA 창 재계산, (3) 기존 매핑을 새 domain 에 replay, (4) 인터럽트 격리
 * 확인이다. 이 중 하나라도 실패하면 전부 되돌린다.
 *
 * 동작 단계:
 *  1. iommu->lock 을 잡는다. vaddr 무효화가 진행 중이면 -EBUSY — attach 가 replay 로
 *     페이지를 핀할 수 있는데 무효 vaddr 로는 핀할 수 없기 때문이다.
 *  2. 이미 붙어 있는 group 이면 -EINVAL.
 *  3. 래퍼 vfio_iommu_group 을 할당한다.
 *  4. 에뮬레이트 group 이면 하드웨어 domain 이 필요 없다. 목록에 넣고, 상류 주석대로
 *     dirty 추적 범위를 곧바로 true 로 둔 뒤 성공 반환한다 — mdev 는 핀 인터페이스를
 *     통해서만 메모리를 만지므로 추적이 정확하다.
 *  5. vfio_domain 래퍼를 할당하고, iommu_group_for_each_dev() 로 대표 장치를 얻어
 *     IOMMU domain 을 만든다. ERR_PTR 이면 그 오류를 그대로 쓴다.
 *  6. iommu_attach_group() 으로 group 을 domain 에 붙인다. 이 시점부터 그 group 의
 *     모든 장치의 DMA 가 이 domain 의 페이지 테이블을 거친다.
 *  7. domain 의 geometry(다룰 수 있는 주소 창)가 기존 상태와 충돌하는지 확인한다.
 *  8. iommu_get_group_resv_regions() 로 예약 구간을 받아 기존 매핑과 충돌하는지 본다.
 *  9. 상류 주석대로 IOVA 목록의 사본을 뜬 뒤, 사본 위에서 창을 좁히고(aper_resize)
 *     예약 구간을 도려낸다(resv_exclude).
 * 10. 소프트웨어 MSI 창이 필요한지 판정해 둔다.
 * 11. domain->group_list 를 초기화하고 group 을 넣는다.
 * 12. 인터럽트 격리 검사: allow_unsafe_interrupts 가 꺼져 있는데
 *     iommu_group_has_isolated_msi() 가 거짓이면 -EPERM 이다. 격리되지 않은 MSI 를
 *     허용하면 장치가 임의의 도어벨 주소로 써서 호스트 인터럽트를 위조할 수 있어,
 *     사용자 공간 드라이버에게 장치를 넘기는 전제 자체가 무너진다.
 * 13. 상류 주석대로 no-snoop 차단 능력을 확인해 기록한다. 이 값은 KVM 이
 *     wbinvd 에뮬레이션을 끄는 최적화의 근거가 된다.
 * 14. 상류 주석대로 이미 있는 domain 중 성질이 같은 것(같은 iommu_ops, 같은 캐시
 *     일관성)을 찾아 병합을 시도한다. 새 domain 에서 떼고 기존 domain 에 붙여 보고,
 *     성공하면 새로 만든 domain 을 버리고 done 으로 간다. 실패하면 원래 domain 에
 *     다시 붙이고, 그것마저 실패하면 out_domain 으로 간다.
 *     domain 수를 줄이면 매핑 한 번에 갱신할 페이지 테이블 수가 줄어 유리하다.
 * 15. 병합하지 못했으면 새 domain 이므로 vfio_iommu_replay() 로 기존 매핑 전체를
 *     설치한다.
 * 16. 소프트웨어 MSI 창이 필요하면 iommu_get_msi_cookie() 로 등록한다.
 *     -ENODEV 는 그 기능이 없는 구성이라는 뜻이라 무시한다.
 * 17. domain 을 목록에 넣고 페이지 크기 비트맵을 다시 계산한다.
 * 18. done 라벨: 완성된 IOVA 사본을 원본 자리에 갈아 끼운다.
 * 19. 상류 주석대로 하드웨어 group 은 직접 메모리를 더럽힐 수 있으므로
 *     num_non_pinned_groups 를 하나 올려 dirty 추적 정밀도를 낮춘다. 나중에 이
 *     group 이 핀 인터페이스를 쓰면 다시 내려간다.
 * 20. 락을 놓고 예약 구간 목록을 해제한 뒤 0 을 반환한다.
 * 21. 실패 라벨들은 out_detach -> out_domain -> out_free_domain -> out_free_group ->
 *     out_unlock 순으로 이어져, 진행한 만큼만 정확히 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. ioctl 경로에서 불리며 이 함수가 락을 직접 잡는다.
 * 할당과 replay 로 잠들 수 있다.
 *
 * caller: drivers/vfio/container.c:243 과 :444 에서 ops->attach_group 으로 불린다.
 * callee: vfio_iommu_find_iommu_group(), iommu_group_for_each_dev(),
 *   iommu_attach_group(), vfio_iommu_aper_conflict(), iommu_get_group_resv_regions(),
 *   vfio_iommu_resv_conflict(), vfio_iommu_iova_get_copy(), vfio_iommu_aper_resize(),
 *   vfio_iommu_resv_exclude(), vfio_iommu_has_sw_msi(),
 *   iommu_group_has_isolated_msi(), vfio_iommu_replay(), iommu_get_msi_cookie(),
 *   vfio_update_pgsize_bitmap(), vfio_iommu_iova_insert_copy().
 * 에러 경로: 위 21 번. IOVA 목록은 사본에서만 작업했으므로 실패해도 원본이 온전하다.
 *
 * 호출 체인:
 *   vfio_container_attach_group() → [vfio_iommu_type1_attach_group] →
 *   vfio_iommu_replay()
 */
static int vfio_iommu_type1_attach_group(void *iommu_data,
		struct iommu_group *iommu_group, enum vfio_group_type type)
{
	struct vfio_iommu *iommu = iommu_data;
	/* [한국어] 새로 만들 group 래퍼. */
	struct vfio_iommu_group *group;
	/* [한국어] 새로 만들 domain 래퍼와 기존 domain 순회용 커서. */
	struct vfio_domain *domain, *d;
	/* [한국어] 소프트웨어 MSI 창이 필요한지. */
	bool resv_msi;
	/* [한국어] 그 창의 시작 물리 주소. */
	phys_addr_t resv_msi_base = 0;
	/* [한국어] domain 이 다룰 수 있는 주소 범위. */
	struct iommu_domain_geometry *geo;
	/* [한국어] IOVA 목록의 작업용 사본. 스택에 만든다. */
	LIST_HEAD(iova_copy);
	/* [한국어] 이 group 의 예약 구간을 받을 목록. 스택에 만든다. */
	LIST_HEAD(group_resv_regions);
	/* [한국어] 기본 오류를 -EBUSY 로 두어, 아래 첫 검사가 실패하면 그 값이 나가게 한다. */
	int ret = -EBUSY;

	/* [한국어] 컨테이너 상태 전체를 바꾸므로 락이 필요하다. */
	mutex_lock(&iommu->lock);

	/* Attach could require pinning, so disallow while vaddr is invalid. */
	/* [한국어] vaddr 무효화가 진행 중이면 */
	if (iommu->vaddr_invalid_count)
		/* [한국어] 무효 vaddr 로는 replay 가 페이지를 핀할 수 없으므로 거절한다. */
		goto out_unlock;

	/* Check for duplicates */
	/* [한국어] 다음 실패는 인자 오류다 — goto 로 뛰기 전에 반환값을 미리 정해 두는 관용구다. */
	ret = -EINVAL;
	/* [한국어] 이미 이 컨테이너에 붙어 있는 group 이면 중복 attach 다. */
	if (vfio_iommu_find_iommu_group(iommu, iommu_group))
		goto out_unlock;

	/* [한국어] 다음 실패는 메모리 부족이다. */
	ret = -ENOMEM;
	/* [한국어] group 래퍼를 0 초기화 할당한다. */
	group = kzalloc_obj(*group);
	/* [한국어] 할당 실패면 위에서 설정한 -ENOMEM 이 나간다. */
	if (!group)
		goto out_unlock;
	/* [한국어] 래퍼가 감쌀 실제 IOMMU group 을 기록한다. */
	group->iommu_group = iommu_group;

	/* [한국어] 에뮬레이트(mdev) group 은 하드웨어 domain 이 필요 없다. */
	if (type == VFIO_EMULATED_IOMMU) {
		list_add(&group->next, &iommu->emulated_iommu_groups);
		/*
		 * An emulated IOMMU group cannot dirty memory directly, it can
		 * only use interfaces that provide dirty tracking.
		 * The iommu scope can only be promoted with the addition of a
		 * dirty tracking group.
		 */
		/* [한국어] 에뮬레이트 장치는 핀 인터페이스로만 메모리를 만지므로 추적이 처음부터 정확하다. */
		group->pinned_page_dirty_scope = true;
		/* [한국어] 성공 반환. */
		ret = 0;
		/* [한국어] 에뮬레이트 group 은 하드웨어 domain 없이 여기서 끝난다. */
		goto out_unlock;
	}

	/* [한국어] 이후 실패는 메모리 부족이다. */
	ret = -ENOMEM;
	/* [한국어] domain 래퍼를 0 초기화 할당한다. */
	domain = kzalloc_obj(*domain);
	/* [한국어] 실패하면 group 래퍼만 해제하고 나간다. */
	if (!domain)
		goto out_free_group;

	/*
	 * Going via the iommu_group iterator avoids races, and trivially gives
	 * us a representative device for the IOMMU API call. We don't actually
	 * want to iterate beyond the first device (if any).
	 */
	/* [한국어] group 안의 대표 장치 하나로 domain 을 만든다. 콜백이 첫 장치에서 1 을 반환해 순회를 멈춘다. */
	iommu_group_for_each_dev(iommu_group, &domain->domain,
				 vfio_iommu_domain_alloc);
	/* [한국어] domain 할당은 오류를 ERR_PTR 로 표현하므로 IS_ERR 로 검사한다. */
	if (IS_ERR(domain->domain)) {
		/* [한국어] 그 오류 코드를 꺼내 반환값으로 삼는다. */
		ret = PTR_ERR(domain->domain);
		/* [한국어] domain 을 만들지 못했으므로 래퍼만 해제하면 된다. */
		goto out_free_domain;
	}

	/* [한국어] group 을 domain 에 붙인다. 이 순간부터 그 group 의 장치 DMA 가 이 페이지 테이블을 거친다. */
	ret = iommu_attach_group(domain->domain, group->iommu_group);
	/* [한국어] 실패하면 domain 만 해제하면 된다(붙지 않았으므로 detach 는 불필요). */
	if (ret)
		goto out_domain;

	/* Get aperture info */
	/* [한국어] 이 domain 이 다룰 수 있는 주소 범위. */
	geo = &domain->domain->geometry;
	/* [한국어] 새 domain 의 주소 창이 기존 창이나 기존 매핑과 충돌하는지 본다. */
	if (vfio_iommu_aper_conflict(iommu, geo->aperture_start,
				     geo->aperture_end)) {
		ret = -EINVAL;
		/* [한국어] 주소 창이 충돌하면 붙일 수 없다. */
		goto out_detach;
	}

	/* [한국어] 이 group 의 예약 구간(MSI 도어벨, 펌웨어 직접 매핑 등)을 받아 온다. */
	ret = iommu_get_group_resv_regions(iommu_group, &group_resv_regions);
	/* [한국어] 받아 오지 못하면 안전을 위해 attach 를 포기한다. */
	if (ret)
		goto out_detach;

	/* [한국어] 예약 구간이 기존 사용자 매핑과 겹치면 붙일 수 없다. */
	if (vfio_iommu_resv_conflict(iommu, &group_resv_regions)) {
		ret = -EINVAL;
		/* [한국어] 예약 구간이 기존 매핑과 겹치면 붙일 수 없다. */
		goto out_detach;
	}

	/*
	 * We don't want to work on the original iova list as the list
	 * gets modified and in case of failure we have to retain the
	 * original list. Get a copy here.
	 */
	/* [한국어] 원본을 직접 고치지 않도록 사본을 뜬다 — 중간에 실패해도 원본이 온전해야 하기 때문이다. */
	ret = vfio_iommu_iova_get_copy(iommu, &iova_copy);
	/* [한국어] 사본 뜨기 실패. */
	if (ret)
		goto out_detach;

	/* [한국어] 사본을 새 domain 의 주소 창 안으로 좁힌다. */
	ret = vfio_iommu_aper_resize(&iova_copy, geo->aperture_start,
				     geo->aperture_end);
	/* [한국어] 실패하면 사본을 버리고 원본을 유지한다. */
	if (ret)
		goto out_detach;

	/* [한국어] 사본에서 예약 구간들을 도려낸다. */
	ret = vfio_iommu_resv_exclude(&iova_copy, &group_resv_regions);
	/* [한국어] 도려내고 남는 구간이 없으면 여기서 실패한다. */
	if (ret)
		goto out_detach;

	/* [한국어] 소프트웨어 MSI 창이 필요한지 판정하고 그 시작 주소를 받아 둔다. */
	resv_msi = vfio_iommu_has_sw_msi(&group_resv_regions, &resv_msi_base);

	/* [한국어] 새 domain 의 group 목록을 빈 상태로 초기화한다. */
	INIT_LIST_HEAD(&domain->group_list);
	/* [한국어] group 을 새 domain 의 목록에 넣는다. */
	list_add(&group->next, &domain->group_list);

	/* [한국어] 인터럽트 리매핑 검사 — 모듈 파라미터로 우회하지 않았고 */
	if (!allow_unsafe_interrupts &&
	    !iommu_group_has_isolated_msi(iommu_group)) {
		/* [한국어] MSI 가 격리되지 않았다면 장치가 임의의 도어벨 주소로 써서 호스트 인터럽트를
		 * 위조할 수 있다. 사용자 공간에 장치를 넘기는 전제가 무너지므로 -EPERM 이다. */
		pr_warn("%s: No interrupt remapping support.  Use the module param \"allow_unsafe_interrupts\" to enable VFIO IOMMU support on this platform\n",
		       __func__);
		/* [한국어] 권한 부족으로 거절한다. */
		ret = -EPERM;
		/* [한국어] 격리되지 않은 MSI 는 인터럽트 위조를 허용하므로 붙일 수 없다. */
		goto out_detach;
	}

	/*
	 * If the IOMMU can block non-coherent operations (ie PCIe TLPs with
	 * no-snoop set) then VFIO always turns this feature on because on Intel
	 * platforms it optimizes KVM to disable wbinvd emulation.
	 */
	/* [한국어] domain 이 no-snoop 차단 능력을 광고하면 */
	if (domain->domain->ops->enforce_cache_coherency)
		/* [한국어] domain 이 no-snoop 트랜잭션을 차단할 수 있는지 물어 기록해 둔다. */
		domain->enforce_cache_coherency =
			domain->domain->ops->enforce_cache_coherency(
				domain->domain);

	/*
	 * Try to match an existing compatible domain.  We don't want to
	 * preclude an IOMMU driver supporting multiple bus_types and being
	 * able to include different bus_types in the same IOMMU domain, so
	 * we test whether the domains use the same iommu_ops rather than
	 * testing if they're on the same bus_type.
	 */
	/* [한국어] 기존 domain 들을 훑으며 병합할 수 있는 것을 찾는다. domain 수가 줄면 매핑마다 갱신할 페이지 테이블 수도 준다. */
	list_for_each_entry(d, &iommu->domain_list, next) {
		/* [한국어] iommu_ops 가 같고 캐시 일관성 성질도 같은 기존 domain 을 찾는다. */
		if (d->domain->ops == domain->domain->ops &&
		    d->enforce_cache_coherency ==
			    domain->enforce_cache_coherency) {
			/* [한국어] 병합을 시도하려면 먼저 새 domain 에서 떼야 한다. */
			iommu_detach_group(domain->domain, group->iommu_group);
			/* [한국어] 기존 domain 에 붙여 본다. */
			if (!iommu_attach_group(d->domain,
						group->iommu_group)) {
				/* [한국어] 성공했으면 group 을 기존 domain 의 목록으로 옮긴다. */
				list_add(&group->next, &d->group_list);
				/* [한국어] 쓰지 않게 된 새 domain 을 해제한다. */
				iommu_domain_free(domain->domain);
				/* [한국어] 래퍼도 해제한다. */
				kfree(domain);
				/* [한국어] 병합에 성공했으니 새로 만든 domain 을 쓰지 않고 공통 마무리로 간다. */
				goto done;
			}

			/* [한국어] 병합에 실패했으니 원래 domain 에 다시 붙인다. */
			ret = iommu_attach_group(domain->domain,
						 group->iommu_group);
			/* [한국어] 그것마저 실패하면 되돌릴 수 없다 — domain 을 해제하고 나간다. */
			if (ret)
				goto out_domain;
		}
	}

	/* replay mappings on new domains */
	/* [한국어] 새 domain 이므로 기존 매핑 전체를 여기에 다시 설치한다. */
	ret = vfio_iommu_replay(iommu, domain);
	/* [한국어] replay 실패면 group 을 떼고 전부 되돌린다. */
	if (ret)
		goto out_detach;

	/* [한국어] 소프트웨어 MSI 창이 필요하면 */
	if (resv_msi) {
		/* [한국어] IOMMU 코어에 그 창을 등록해 MSI 도어벨이 IOVA 공간에 나타나게 한다. */
		ret = iommu_get_msi_cookie(domain->domain, resv_msi_base);
		/* [한국어] -ENODEV 는 이 구성에 그 기능이 없다는 뜻이라 무시한다. 다른 오류는 실패다. */
		if (ret && ret != -ENODEV)
			goto out_detach;
	}

	/* [한국어] 완성된 domain 을 컨테이너 목록에 넣는다. */
	list_add(&domain->next, &iommu->domain_list);
	/* [한국어] 새 domain 이 추가되었으니 공통 지원 페이지 크기를 다시 계산한다. */
	vfio_update_pgsize_bitmap(iommu);
done:
	/* Delete the old one and insert new iova list */
	/* [한국어] 완성된 사본을 원본 자리에 통째로 갈아 끼운다. */
	vfio_iommu_iova_insert_copy(iommu, &iova_copy);

	/*
	 * An iommu backed group can dirty memory directly and therefore
	 * demotes the iommu scope until it declares itself dirty tracking
	 * capable via the page pinning interface.
	 */
	/* [한국어] 하드웨어 group 은 직접 메모리를 더럽힐 수 있으므로 추적 정밀도를 한 단계 낮춘다. 나중에 핀 인터페이스를 쓰면 되돌아간다. */
	iommu->num_non_pinned_groups++;
	/* [한국어] 락을 놓는다. */
	mutex_unlock(&iommu->lock);
	/* [한국어] 예약 구간 목록을 해제한다. 락을 놓은 뒤에 하는 것은 스택 지역 목록이라 안전하기 때문이다. */
	vfio_iommu_resv_free(&group_resv_regions);

	return 0;

out_detach:
	/* [한국어] attach 까지 성공했던 경우에만 여기로 온다 — 먼저 하드웨어에서 뗀다. */
	iommu_detach_group(domain->domain, group->iommu_group);
out_domain:
	/* [한국어] 하드웨어 domain 을 해제한다. */
	iommu_domain_free(domain->domain);
	/* [한국어] 작업용 IOVA 사본을 버린다 — 원본 목록은 손대지 않았으므로 그대로 유효하다. */
	vfio_iommu_iova_free(&iova_copy);
	/* [한국어] 예약 구간 목록을 해제한다. */
	vfio_iommu_resv_free(&group_resv_regions);
out_free_domain:
	/* [한국어] domain 래퍼를 해제한다. */
	kfree(domain);
out_free_group:
	/* [한국어] group 래퍼를 해제한다. */
	kfree(group);
out_unlock:
	/* [한국어] 락을 놓고 오류를 반환한다. 모든 실패 경로가 여기로 모인다. */
	mutex_unlock(&iommu->lock);
	return ret;
}

/* [한국어]
 * vfio_iommu_unmap_unpin_all - 컨테이너의 모든 매핑을 없앤다
 *
 * @iommu: 컨테이너 객체.
 * @return: 없음.
 *
 * 컨테이너를 닫거나 마지막 group 이 빠져 아무도 이 매핑을 쓸 수 없게 되었을 때
 * 전부 정리한다. 여기서 빠뜨리면 핀과 과금이 그대로 새어 나간다.
 *
 * 동작 단계: rb_first() 가 NULL 을 돌려줄 때까지 루트를 반복해서 지운다.
 * vfio_remove_dma() 안에서 트리 구조가 바뀌므로 순회 포인터를 들고 다니지 않고
 * 매번 처음부터 다시 꺼내는 방식을 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태(release 경로에서는 이미
 * 아무도 접근하지 않는 상태다). 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_detach_group()(마지막 group 이 빠질 때),
 *   vfio_iommu_type1_release()(컨테이너 해제).
 * callee: rb_first(), rb_entry(), vfio_remove_dma().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_release() → [vfio_iommu_unmap_unpin_all] → vfio_remove_dma()
 */
static void vfio_iommu_unmap_unpin_all(struct vfio_iommu *iommu)
{
	struct rb_node *node;

	/* [한국어] 트리가 빌 때까지 루트를 반복해 지운다. 제거가 트리 구조를 바꾸므로 매번 처음부터 다시 꺼낸다. */
	while ((node = rb_first(&iommu->dma_list)))
		vfio_remove_dma(iommu, rb_entry(node, struct vfio_dma, node));
}

/* [한국어]
 * vfio_iommu_unmap_unpin_reaccount - 하드웨어 domain 이 사라질 때 과금 기준을 다시 잡는다
 *
 * @iommu: 컨테이너 객체.
 * @return: 없음.
 *
 * 언제 불리는가: 마지막 하드웨어 domain 이 detach 되었지만 에뮬레이트 group 은 남아
 * 있는 경우다. 매핑 자체는 살아 있어야 하지만(mdev 가 계속 쓴다) IOMMU 매핑과 그를
 * 위한 핀은 더 이상 필요 없다. 그런데 mdev 가 따로 핀해 둔 페이지는 남겨야 하므로,
 * '전부 언핀' 이 아니라 '차액만 반영' 이 필요하다.
 *
 * 동작 단계: 각 매핑에 대해
 *  1. vfio_unmap_unpin(do_accounting=false) 로 IOMMU 매핑을 걷고 핀을 푼다. 과금은
 *     이 함수가 직접 처리할 것이므로 do_accounting 을 끄고 언핀 수만 받는다.
 *  2. pfn_list 를 훑어 아직 살아 있는 외부 핀 중 예약 pfn 이 아닌 것의 수를 센다.
 *     이들은 계속 잠겨 있어야 하므로 과금이 남아야 한다.
 *  3. vfio_lock_acct(dma, locked - unlocked, true) 로 차액만 반영한다. 대개 음수가
 *     되어 과금이 줄어든다. 이 계산이 vfio_unpin_pages_remote() 안의 계산과 같은
 *     형태인 것은 우연이 아니라, 두 곳 모두 '푼 것에서 남은 것을 뺀다' 는 같은
 *     규칙을 쓰기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_detach_group() 한 곳뿐.
 * callee: vfio_unmap_unpin(), is_invalid_reserved_pfn(), vfio_lock_acct().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_detach_group() → [vfio_iommu_unmap_unpin_reaccount] →
 *   vfio_unmap_unpin()
 */
static void vfio_iommu_unmap_unpin_reaccount(struct vfio_iommu *iommu)
{
	struct rb_node *n, *p;

	/* [한국어] 첫 매핑부터 시작한다. */
	n = rb_first(&iommu->dma_list);
	/* [한국어] 모든 매핑을 훑는다. */
	for (; n; n = rb_next(n)) {
		/* [한국어] 현재 매핑. */
		struct vfio_dma *dma;
		/* [한국어] 남겨야 할 과금량과 이번에 푼 페이지 수. */
		long locked = 0, unlocked = 0;

		/* [한국어] 노드에서 구조체를 복원한다. */
		dma = rb_entry(n, struct vfio_dma, node);
		/* [한국어] IOMMU 매핑을 걷고 핀을 푼다. 과금은 여기서 직접 처리하므로 do_accounting 을 끈다. */
		unlocked += vfio_unmap_unpin(iommu, dma, false);
		/* [한국어] 이 매핑에 남아 있는 외부 핀들을 훑기 시작한다. */
		p = rb_first(&dma->pfn_list);
		/* [한국어] pfn 트리를 순회한다. */
		for (; p; p = rb_next(p)) {
			/* [한국어] 노드에서 vfio_pfn 을 복원한다. */
			struct vfio_pfn *vpfn = rb_entry(p, struct vfio_pfn,
							 node);

			/* [한국어] 예약 pfn 은 애초에 과금되지 않았으므로 세지 않는다. */
			if (!is_invalid_reserved_pfn(vpfn->pfn))
				/* [한국어] 계속 잠겨 있어야 할 페이지를 하나 센다. */
				locked++;
		}
		/* [한국어] 푼 것에서 남길 것을 뺀 차액만 회계에 반영한다. 대개 음수가 되어 과금이 줄어든다. */
		vfio_lock_acct(dma, locked - unlocked, true);
	}
}

/* [한국어]
 * vfio_iommu_aper_expand - domain 이 제거된 뒤 IOVA 창을 다시 넓힌다
 *
 * @iommu: 컨테이너 객체.
 * @iova_copy: 작업용 IOVA 사본 목록.
 * @return: 없음.
 *
 * 상류 주석대로, 방금 제거된 domain 이 창을 가장 좁게 만든 장본인이었을 수 있다.
 * 그러면 남은 domain 들의 교집합은 더 넓어진다. 이 함수가 그 새 경계를 계산해
 * 사본의 처음/마지막 노드에 반영한다.
 *
 * 동작 단계:
 *  1. 사본이 비어 있으면(사본 뜨기가 실패했으면) 아무것도 하지 않는다.
 *  2. start 를 0, end 를 최대값으로 시작해 남은 모든 domain 의 geometry 로 좁힌다 —
 *     시작은 최대값을, 끝은 최소값을 취하는 표준 교집합 계산이다.
 *  3. 사본의 첫 노드 시작과 마지막 노드 끝을 그 값으로 바꾼다. 가운데 노드들은
 *     예약 구간 때문에 쪼개진 것이므로 건드리지 않는다.
 *  4. 주석대로 새 창은 같거나 더 넓으므로, 기존 노드를 넘어 확장해도 다른 노드와
 *     충돌하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_detach_group() 한 곳뿐 — domain 을 해제한 직후.
 * callee: 없다. 목록 순회와 비교뿐이다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_detach_group() → [vfio_iommu_aper_expand]
 */
/*
 * Called when a domain is removed in detach. It is possible that
 * the removed domain decided the iova aperture window. Modify the
 * iova aperture with the smallest window among existing domains.
 */
static void vfio_iommu_aper_expand(struct vfio_iommu *iommu,
				   struct list_head *iova_copy)
{
	struct vfio_domain *domain;
	/* [한국어] 사본의 처음/마지막 노드를 가리킬 임시 포인터. */
	struct vfio_iova *node;
	/* [한국어] 교집합의 시작 — 최대값을 취해야 하므로 0 에서 시작한다. */
	dma_addr_t start = 0;
	/* [한국어] 교집합의 끝 — 최소값을 취해야 하므로 최대값에서 시작한다. */
	dma_addr_t end = (dma_addr_t)~0;

	/* [한국어] 사본 뜨기가 실패했으면 고칠 대상이 없다. */
	if (list_empty(iova_copy))
		return;

	/* [한국어] 남아 있는 모든 domain 의 주소 창을 본다. */
	list_for_each_entry(domain, &iommu->domain_list, next) {
		/* [한국어] 이 domain 이 다룰 수 있는 주소 범위. */
		struct iommu_domain_geometry *geo = &domain->domain->geometry;

		/* [한국어] 더 늦게 시작하는 domain 이 있으면 교집합의 시작이 그만큼 뒤로 밀린다. */
		if (geo->aperture_start > start)
			/* [한국어] 시작을 갱신한다. */
			start = geo->aperture_start;
		/* [한국어] 더 일찍 끝나는 domain 이 있으면 교집합의 끝이 그만큼 앞으로 당겨진다. */
		if (geo->aperture_end < end)
			/* [한국어] 끝을 갱신한다. */
			end = geo->aperture_end;
	}

	/* Modify aperture limits. The new aper is either same or bigger */
	/* [한국어] 사본의 첫 노드를 잡는다. */
	node = list_first_entry(iova_copy, struct vfio_iova, list);
	/* [한국어] 첫 노드의 시작을 새 창의 시작으로 넓힌다. */
	node->start = start;
	/* [한국어] 마지막 노드를 잡는다. */
	node = list_last_entry(iova_copy, struct vfio_iova, list);
	/* [한국어] 마지막 노드의 끝을 새 창의 끝으로 넓힌다. */
	node->end = end;
}

/* [한국어]
 * vfio_iommu_resv_refresh - 남은 group 들 기준으로 유효 IOVA 목록을 다시 만든다
 *
 * @iommu: 컨테이너 객체.
 * @iova_copy: 작업용 IOVA 사본. 이 함수가 통째로 다시 만든다.
 * @return: 0 성공, -EINVAL(사본이 비었거나 남는 구간이 없음), -ENOMEM.
 *
 * 상류 주석대로, group 이 빠지면 그 group 때문에 도려냈던 예약 구간이 다시 쓸 수
 * 있게 될 수 있다. 그런데 예약 구간은 여러 group 이 공유할 수도 있어서 '그 group
 * 것만 되돌리기' 가 불가능하다. 그래서 남은 group 전체의 예약 구간을 다시 모아
 * 목록을 처음부터 다시 만든다.
 *
 * 동작 단계:
 *  1. 사본이 비어 있으면 -EINVAL — 되돌릴 기준이 없다.
 *  2. 남아 있는 모든 domain 의 모든 group 에서 예약 구간을 모은다.
 *     같은 목록에 계속 덧붙이므로 중복이 있을 수 있으나, 도려내기 연산은 멱등이라
 *     문제가 되지 않는다.
 *  3. 사본의 처음/마지막 노드에서 현재 창의 경계를 읽어 둔다(vfio_iommu_aper_expand()
 *     가 이미 넓혀 놓은 값이다).
 *  4. 사본을 통째로 비우고, 그 경계로 구간 하나를 다시 만든 뒤
 *  5. 모은 예약 구간들을 도려낸다.
 *  6. done 라벨에서 예약 구간 목록을 해제하고 결과를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_detach_group() 한 곳뿐.
 * callee: iommu_get_group_resv_regions(), vfio_iommu_iova_free(),
 *   vfio_iommu_aper_resize(), vfio_iommu_resv_exclude(), vfio_iommu_resv_free().
 * 에러 경로: 실패하면 호출자가 사본을 버리고 원본 IOVA 목록을 그대로 둔다 —
 *   약간 보수적이지만(실제보다 좁은 창) 안전한 상태다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_detach_group() → [vfio_iommu_resv_refresh] →
 *   vfio_iommu_resv_exclude()
 */
/*
 * Called when a group is detached. The reserved regions for that
 * group can be part of valid iova now. But since reserved regions
 * may be duplicated among groups, populate the iova valid regions
 * list again.
 */
static int vfio_iommu_resv_refresh(struct vfio_iommu *iommu,
				   struct list_head *iova_copy)
{
	struct vfio_domain *d;
	/* [한국어] group 목록 순회용 커서. */
	struct vfio_iommu_group *g;
	/* [한국어] IOVA 사본의 처음/마지막 노드를 가리킬 임시 포인터. */
	struct vfio_iova *node;
	/* [한국어] 재구성에 쓸 창의 경계. */
	dma_addr_t start, end;
	/* [한국어] 남은 group 전체의 예약 구간을 모을 목록. 스택에 만든다. */
	LIST_HEAD(resv_regions);
	/* [한국어] 결과 코드. */
	int ret;

	/* [한국어] 사본이 비어 있으면 기준으로 삼을 창이 없다. */
	if (list_empty(iova_copy))
		return -EINVAL;

	/* [한국어] 남아 있는 모든 domain 을 훑는다. */
	list_for_each_entry(d, &iommu->domain_list, next) {
		/* [한국어] 각 domain 에 붙어 있는 모든 group 을 훑는다. */
		list_for_each_entry(g, &d->group_list, next) {
			/* [한국어] 그 group 의 예약 구간을 같은 목록에 계속 덧붙인다. 중복이 생겨도 도려내기는 멱등이라 무방하다. */
			ret = iommu_get_group_resv_regions(g->iommu_group,
							   &resv_regions);
			/* [한국어] 실패하면 지금까지 모은 것을 done 에서 해제한다. */
			if (ret)
				goto done;
		}
	}

	/* [한국어] 사본의 첫 노드에서 현재 창의 시작을 읽는다. */
	node = list_first_entry(iova_copy, struct vfio_iova, list);
	/* [한국어] 그 값을 기억한다. */
	start = node->start;
	/* [한국어] 마지막 노드에서 창의 끝을 읽는다. */
	node = list_last_entry(iova_copy, struct vfio_iova, list);
	/* [한국어] 그 값도 기억한다. 두 값이 vfio_iommu_aper_expand() 가 이미 넓혀 둔 새 창이다. */
	end = node->end;

	/* purge the iova list and create new one */
	/* [한국어] 사본을 통째로 비운다 — 아래에서 처음부터 다시 만든다. */
	vfio_iommu_iova_free(iova_copy);

	/* [한국어] 비운 사본에 그 창 하나짜리 구간을 다시 만든다. */
	ret = vfio_iommu_aper_resize(iova_copy, start, end);
	/* [한국어] 실패하면 done 으로. */
	if (ret)
		goto done;

	/* Exclude current reserved regions from iova ranges */
	/* [한국어] 새로 만든 창에서 예약 구간들을 도려낸다. */
	ret = vfio_iommu_resv_exclude(iova_copy, &resv_regions);
done:
	/* [한국어] 모아 둔 예약 구간 목록을 해제한다. 성공·실패 어느 쪽이든 반드시 거친다. */
	vfio_iommu_resv_free(&resv_regions);
	return ret;
}

/* [한국어]
 * vfio_iommu_type1_detach_group - IOMMU group 을 컨테이너에서 뗀다
 *
 * @iommu_data: 컨테이너 객체.
 * @iommu_group: 뗄 group.
 * @return: 없음 — 이 연산은 실패할 수 없다.
 *
 * vfio_iommu_driver_ops 의 detach_group 콜백이며 attach 의 역연산이다. 어려운 점은
 * '언제 매핑까지 없애야 하는가' 의 판정이다. 매핑은 컨테이너의 자산이므로 아직
 * 쓸 수 있는 장치가 남아 있으면 유지해야 하고, 아무도 없으면 정리해야 한다.
 *
 * 동작 단계:
 *  1. iommu->lock 을 잡는다.
 *  2. 먼저 에뮬레이트 group 목록에서 찾는다. 있으면 dirty 범위 갱신 여부를 기억하고
 *     목록에서 빼 해제한 뒤, 에뮬레이트 group 도 domain 도 모두 없어졌으면 매핑을
 *     전부 없앤다. 이때 device_list 는 비어 있어야 한다(WARN_ON) — 장치가 남아
 *     있는데 매핑을 지우면 그 장치가 언핀할 대상을 잃는다.
 *  3. 하드웨어 group 이면, 상류 주석대로 IOVA 목록의 사본을 먼저 뜬다. 갱신에
 *     실패해도 원본을 그대로 두기 위해서다.
 *  4. 모든 domain 을 돌며 이 group 을 가진 domain 을 찾는다.
 *     - iommu_detach_group() 으로 하드웨어에서 뗀다. 이 순간부터 그 장치의 DMA 는
 *       더 이상 이 페이지 테이블을 쓰지 않는다.
 *     - 래퍼를 목록에서 빼고 해제한다.
 *     - 상류 주석이 설명하는 세 갈래 판정: domain 의 group 목록이 비면 그 domain 은
 *       쓸모가 없다. 그것이 마지막 domain 이고 에뮬레이트 group 도 없으면 매핑을
 *       전부 없애고, 에뮬레이트 group 이 남았으면 과금 기준만 다시 잡는다
 *       (vfio_iommu_unmap_unpin_reaccount).
 *     - domain 을 해제하고 목록에서 빼고, IOVA 창을 넓히고 페이지 크기 비트맵을
 *       다시 계산한다.
 *     - 찾았으면 break — group 은 한 domain 에만 속한다.
 *  5. vfio_iommu_resv_refresh() 로 사본을 재구성한다. 성공하면 갈아 끼우고,
 *     실패하면 사본을 버려 원본을 유지한다.
 *  6. detach_group_done 라벨: 상류 주석대로 dirty 추적 능력이 없던 group 이 빠지면
 *     추적 정밀도가 올라갈 수 있으므로 num_non_pinned_groups 를 줄인다. 추적 중이면
 *     그 직전까지의 상태를 잃지 않도록 모든 매핑을 dirty 로 한 번 칠한다.
 *  7. 락을 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 함수가 락을 직접 잡는다. 잠들 수 있다.
 *
 * caller: drivers/vfio/container.c:254 와 :480 에서 ops->detach_group 으로 불린다.
 * callee: vfio_iommu_unmap_unpin_all(), vfio_iommu_iova_get_copy(),
 *   find_iommu_group(), iommu_detach_group(), vfio_iommu_unmap_unpin_reaccount(),
 *   iommu_domain_free(), vfio_iommu_aper_expand(), vfio_update_pgsize_bitmap(),
 *   vfio_iommu_resv_refresh(), vfio_iommu_iova_insert_copy(), vfio_iommu_iova_free(),
 *   vfio_iommu_populate_bitmap_full().
 * 에러 경로: 반환값이 없으므로 부분 실패는 보수적인 상태(좁은 IOVA 창, 전부 dirty)
 *   로 흡수한다.
 *
 * 호출 체인:
 *   vfio_container_detach_group 경로 → [vfio_iommu_type1_detach_group] →
 *   iommu_detach_group()
 */
static void vfio_iommu_type1_detach_group(void *iommu_data,
					  struct iommu_group *iommu_group)
{
	struct vfio_iommu *iommu = iommu_data;
	/* [한국어] domain 목록 순회용 커서. */
	struct vfio_domain *domain;
	/* [한국어] 찾아낸 group 래퍼. */
	struct vfio_iommu_group *group;
	/* [한국어] 이 group 이 dirty 추적을 못 하던 group 이었는지 — 나중에 카운터를 줄일지 판단한다. */
	bool update_dirty_scope = false;
	/* [한국어] IOVA 목록의 작업용 사본. 스택에 빈 목록으로 만든다. */
	LIST_HEAD(iova_copy);

	/* [한국어] 컨테이너 상태 전체를 바꾸므로 락이 필요하다. */
	mutex_lock(&iommu->lock);
	/* [한국어] 먼저 에뮬레이트 group 목록에서 찾는다. */
	list_for_each_entry(group, &iommu->emulated_iommu_groups, next) {
		/* [한국어] 다른 group 이면 건너뛴다. */
		if (group->iommu_group != iommu_group)
			continue;
		/* [한국어] 추적 범위가 아니었던 group 이면 제거 후 정밀도가 올라간다. */
		update_dirty_scope = !group->pinned_page_dirty_scope;
		/* [한국어] 에뮬레이트 group 목록에서 뺀다. */
		list_del(&group->next);
		/* [한국어] 래퍼를 해제한다. */
		kfree(group);

		/* [한국어] 에뮬레이트 group 도 하드웨어 domain 도 모두 없어졌는지 본다. */
		if (list_empty(&iommu->emulated_iommu_groups) &&
		    list_empty(&iommu->domain_list)) {
			/* [한국어] 그런데 장치가 아직 등록되어 있으면 언핀할 주체가 남아 있다는 모순이다. */
			WARN_ON(!list_empty(&iommu->device_list));
			/* [한국어] 쓸 주체가 아무도 없으므로 모든 매핑을 없앤다. */
			vfio_iommu_unmap_unpin_all(iommu);
		}
		/* [한국어] 에뮬레이트 group 처리가 끝났으므로 공통 뒤처리로 간다. */
		goto detach_group_done;
	}

	/*
	 * Get a copy of iova list. This will be used to update
	 * and to replace the current one later. Please note that
	 * we will leave the original list as it is if update fails.
	 */
	/* [한국어] 반환값을 보지 않는다 — 사본 뜨기가 실패하면 빈 목록이 남고, 아래 단계들이
	 * 빈 목록을 보고 원본을 그대로 두는 방향으로 동작한다. */
	vfio_iommu_iova_get_copy(iommu, &iova_copy);

	/* [한국어] 하드웨어 group 이면 어느 domain 에 있는지 찾는다. */
	list_for_each_entry(domain, &iommu->domain_list, next) {
		/* [한국어] 이 domain 의 group 목록에서 찾는다. */
		group = find_iommu_group(domain, iommu_group);
		/* [한국어] 이 domain 에 없으면 다음 domain 을 본다. */
		if (!group)
			continue;

		/* [한국어] 하드웨어에서 뗀다. 이 순간부터 그 장치의 DMA 는 번역되지 않는다. */
		iommu_detach_group(domain->domain, group->iommu_group);
		/* [한국어] 추적 범위 갱신 여부를 기억해 둔다. */
		update_dirty_scope = !group->pinned_page_dirty_scope;
		/* [한국어] domain 의 group 목록에서 뺀다. */
		list_del(&group->next);
		/* [한국어] 래퍼를 해제한다. */
		kfree(group);
		/*
		 * Group ownership provides privilege, if the group list is
		 * empty, the domain goes away. If it's the last domain with
		 * iommu and external domain doesn't exist, then all the
		 * mappings go away too. If it's the last domain with iommu and
		 * external domain exist, update accounting
		 */
		/* [한국어] 이 domain 에 남은 group 이 없으면 domain 자체가 쓸모없어진다. */
		if (list_empty(&domain->group_list)) {
			/* [한국어] 이 domain 이 컨테이너의 마지막 domain 이었는가. */
			if (list_is_singular(&iommu->domain_list)) {
				/* [한국어] 에뮬레이트 group 도 없으면 이 매핑을 쓸 주체가 아무도 남지 않는다. */
				if (list_empty(&iommu->emulated_iommu_groups)) {
					/* [한국어] 그런데 장치가 등록되어 있으면 모순이다. */
					WARN_ON(!list_empty(
						&iommu->device_list));
					/* [한국어] 아무도 이 매핑을 쓸 수 없으므로 전부 없앤다. */
					vfio_iommu_unmap_unpin_all(iommu);
				} else {
					/* [한국어] 에뮬레이트 group 이 남았으면 매핑은 유지하고 과금 기준만 다시 잡는다. */
					vfio_iommu_unmap_unpin_reaccount(iommu);
				}
			}
			/* [한국어] 하드웨어 domain 을 해제한다. 이 시점에 붙어 있는 group 은 없다. */
			iommu_domain_free(domain->domain);
			/* [한국어] 컨테이너 목록에서 뺀다. */
			list_del(&domain->next);
			/* [한국어] domain 래퍼를 해제한다. */
			kfree(domain);
			/* [한국어] domain 이 사라졌으니 IOVA 창이 더 넓어질 수 있다. */
			vfio_iommu_aper_expand(iommu, &iova_copy);
			/* [한국어] domain 이 줄었으니 공통 지원 페이지 크기도 다시 계산한다. */
			vfio_update_pgsize_bitmap(iommu);
		}
		break;
	}

	/* [한국어] 남은 group 기준으로 IOVA 목록을 재구성해 본다. */
	if (!vfio_iommu_resv_refresh(iommu, &iova_copy))
		/* [한국어] 성공했으면 원본과 갈아 끼운다. */
		vfio_iommu_iova_insert_copy(iommu, &iova_copy);
	/* [한국어] 재구성 실패. */
	else
		/* [한국어] 재구성에 실패했으면 사본을 버려 원본 목록을 그대로 유지한다 — 실제보다 좁은 창이지만 안전하다. */
		vfio_iommu_iova_free(&iova_copy);

detach_group_done:
	/*
	 * Removal of a group without dirty tracking may allow the iommu scope
	 * to be promoted.
	 */
	/* [한국어] 추적 못 하던 group 이 빠졌으면 정밀도를 되돌린다. */
	if (update_dirty_scope) {
		/* [한국어] 추적 못 하던 group 이 빠졌으므로 정밀도가 한 단계 올라간다. */
		iommu->num_non_pinned_groups--;
		/* [한국어] 추적 중이었다면 그 group 이 붙어 있던 동안의 변경을 잃지 않도록 전부 dirty 로 칠한다. */
		if (iommu->dirty_page_tracking)
			vfio_iommu_populate_bitmap_full(iommu);
	}
	/* [한국어] 락을 놓는다. */
	mutex_unlock(&iommu->lock);
}

/* [한국어]
 * vfio_iommu_type1_open - 컨테이너 하나에 대한 type1 백엔드 인스턴스를 만든다
 *
 * @arg: 사용자가 ioctl(VFIO_SET_IOMMU) 에 넘긴 IOMMU 종류 상수.
 * @return: 컨테이너 객체 포인터, 실패 시 ERR_PTR(-ENOMEM/-EINVAL).
 *
 * vfio_iommu_driver_ops 의 open 콜백이다. 사용자가 /dev/vfio/vfio 를 열고
 * VFIO_SET_IOMMU 로 type1 을 고르면 이 함수가 불려 백엔드 상태를 만든다. 반환한
 * 포인터가 이후 모든 콜백의 iommu_data 인자가 된다.
 *
 * 동작 단계:
 *  1. vfio_iommu 를 0 초기화 할당한다.
 *  2. arg 로 인터페이스 세대를 정한다.
 *     - VFIO_TYPE1_IOMMU: 구식 v1. v2 플래그를 세우지 않는다.
 *     - VFIO_TYPE1v2_IOMMU 와 예약된 nesting 상수: v2. 언맵 경계 규칙이 엄격해지고
 *       pin_pages/dirty_pages 같은 v2 전용 기능이 열린다.
 *     - 그 밖: -EINVAL.
 *  3. 목록들과 뮤텍스 두 개를 초기화하고, 빈 rb-tree 를 설정한다.
 *  4. dma_avail 을 모듈 파라미터 dma_entry_limit 로 초기화한다.
 *  5. pgsize_bitmap 을 PAGE_MASK 로 시작한다 — 아직 domain 이 없으므로 'PAGE_SIZE
 *     이상의 모든 2 의 거듭제곱' 이라는 가장 느슨한 값이다. 첫 domain 이 붙으면
 *     vfio_update_pgsize_bitmap() 이 실제 값으로 좁힌다.
 *
 * 실행 컨텍스트: 프로세스 문맥, ioctl 경로. 컨테이너 락은 아직 존재하지 않으므로
 * 잡지 않는다. 할당으로 잠들 수 있다.
 *
 * caller: drivers/vfio/container.c:302 에서 ops->open 으로 불린다.
 * callee: kzalloc_obj(), INIT_LIST_HEAD(), mutex_init().
 * 에러 경로: -EINVAL 이면 할당한 구조체를 곧바로 해제하고 ERR_PTR 을 반환한다.
 *
 * 호출 체인:
 *   ioctl(VFIO_SET_IOMMU) → container.c:302 ops->open →
 *   [vfio_iommu_type1_open]
 */
static void *vfio_iommu_type1_open(unsigned long arg)
{
	struct vfio_iommu *iommu;

	/* [한국어] 컨테이너 객체를 0 초기화 할당한다. 이후 모든 콜백이 이 포인터를 iommu_data 로 받는다. */
	iommu = kzalloc_obj(*iommu);
	/* [한국어] 할당 실패는 ERR_PTR 로 표현한다 — 반환형이 포인터이기 때문이다. */
	if (!iommu)
		return ERR_PTR(-ENOMEM);

	/* [한국어] 사용자가 SET_IOMMU 에 넘긴 상수로 인터페이스 세대를 정한다. */
	switch (arg) {
	/* [한국어] 구식 v1 — 언맵 경계 규칙이 느슨하고 v2 전용 기능이 닫힌다. */
	case VFIO_TYPE1_IOMMU:
		break;
	/* [한국어] 예약된 nesting 상수와 v2 는 같은 취급 — v2 플래그를 세운다. */
	case __VFIO_RESERVED_TYPE1_NESTING_IOMMU:
	case VFIO_TYPE1v2_IOMMU:
		iommu->v2 = true;
		break;
	default:
		/* [한국어] 실패하므로 방금 할당한 것을 곧바로 돌려준다. */
		kfree(iommu);
		/* [한국어] 모르는 IOMMU 종류는 이 백엔드가 다룰 수 없다. */
		return ERR_PTR(-EINVAL);
	}

	/* [한국어] domain 목록을 빈 상태로 시작한다. */
	INIT_LIST_HEAD(&iommu->domain_list);
	/* [한국어] 유효 IOVA 목록을 빈 상태로 시작한다 — 빈 목록은 '제약 없음' 을 뜻한다. */
	INIT_LIST_HEAD(&iommu->iova_list);
	/* [한국어] DMA 매핑 rb-tree 를 빈 상태로 시작한다. */
	iommu->dma_list = RB_ROOT;
	/* [한국어] 만들 수 있는 매핑 수를 모듈 파라미터 값으로 초기화한다. */
	iommu->dma_avail = dma_entry_limit;
	/* [한국어] 컨테이너의 주 뮤텍스를 초기화한다. */
	mutex_init(&iommu->lock);
	/* [한국어] 그 목록 전용 락. 통지 중 재진입을 허용하기 위해 주 락과 분리되어 있다. */
	mutex_init(&iommu->device_list_lock);
	/* [한국어] 언맵 통지 대상 장치 목록을 빈 상태로 시작한다. */
	INIT_LIST_HEAD(&iommu->device_list);
	/* [한국어] 아직 domain 이 없으므로 가장 느슨한 값으로 시작한다. PAGE_MASK 는 상위 비트가
	 * 모두 1 이라 'PAGE_SIZE 이상의 모든 2 의 거듭제곱' 을 뜻한다.
	 * 첫 domain 이 붙으면 vfio_update_pgsize_bitmap() 이 실제 값으로 좁힌다. */
	iommu->pgsize_bitmap = PAGE_MASK;
	/* [한국어] 에뮬레이트 group 목록을 빈 상태로 시작한다. */
	INIT_LIST_HEAD(&iommu->emulated_iommu_groups);

	/* [한국어] 완성된 컨테이너 객체를 반환한다. 이후 모든 콜백이 이 포인터를 iommu_data 로 받는다. */
	return iommu;
}

/* [한국어]
 * vfio_release_domain - domain 에 붙은 group 을 모두 떼고 domain 을 해제한다
 *
 * @domain: 해제할 domain.
 * @return: 없음.
 *
 * 컨테이너를 닫을 때 쓰는 일괄 정리 함수다. detach_group 과 달리 IOVA 목록이나
 * dirty 추적 상태를 갱신하지 않는데, 컨테이너 전체가 사라지는 중이라 그 상태가
 * 더 이상 의미가 없기 때문이다.
 *
 * 동작 단계:
 *  1. group_list 를 안전 순회하며 각 group 을 iommu_detach_group() 으로 떼고,
 *     목록에서 빼고 래퍼를 해제한다.
 *  2. iommu_domain_free() 로 domain 을 해제한다. 이 시점에 붙어 있는 group 이
 *     남아 있으면 안 되므로 순서가 중요하다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 컨테이너 해제 경로라 다른 참조자가 없다. 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_release() 한 곳뿐.
 * callee: iommu_detach_group(), iommu_domain_free(), list_del(), kfree().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_release() → [vfio_release_domain] → iommu_domain_free()
 */
static void vfio_release_domain(struct vfio_domain *domain)
{
	struct vfio_iommu_group *group, *group_tmp;

	/* [한국어] 이 domain 에 붙은 group 을 안전 순회로 하나씩 뗀다. */
	list_for_each_entry_safe(group, group_tmp,
				 &domain->group_list, next) {
		/* [한국어] 하드웨어에서 group 을 뗀다. 이 순간부터 그 장치의 DMA 는 이 페이지 테이블을 쓰지 않는다. */
		iommu_detach_group(domain->domain, group->iommu_group);
		/* [한국어] 목록에서 뺀다. */
		list_del(&group->next);
		/* [한국어] 래퍼를 해제한다. */
		kfree(group);
	}

	/* [한국어] 붙어 있던 group 을 모두 뗀 뒤에야 domain 을 해제할 수 있다. */
	iommu_domain_free(domain->domain);
}

/* [한국어]
 * vfio_iommu_type1_release - 컨테이너의 type1 백엔드 상태를 전부 해제한다
 *
 * @iommu_data: 컨테이너 객체.
 * @return: 없음.
 *
 * vfio_iommu_driver_ops 의 release 콜백이며 open 의 짝이다. 이 시점에는 이미 모든
 * group 이 detach 되고 모든 fd 가 닫혀 있어 다른 참조자가 없으므로 락을 잡지 않는다.
 * 순서가 중요하다 — 매핑을 먼저 없애야 그 안의 핀이 풀리고, 그 다음에 domain 을
 * 없애야 언맵이 정상적으로 이루어진다.
 *
 * 동작 단계:
 *  1. 남아 있는 에뮬레이트 group 래퍼들을 해제한다.
 *  2. vfio_iommu_unmap_unpin_all() 로 모든 매핑을 없앤다 — 핀과 과금이 여기서
 *     마지막으로 정리된다. 이 호출이 domain 해제보다 앞이어야 하는 이유는
 *     vfio_unmap_unpin() 이 물리 주소를 domain 에게 물어보기 때문이다.
 *  3. 모든 domain 을 vfio_release_domain() 으로 정리하고 래퍼를 해제한다.
 *  4. IOVA 목록을 해제한다.
 *  5. 컨테이너 구조체 자체를 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 컨테이너 fd 가 닫히는 경로다. 잠들 수 있다.
 *
 * caller: drivers/vfio/container.c:311(SET_IOMMU 실패 되감기) 과 :492(컨테이너 해제)
 *   에서 ops->release 로 불린다.
 * callee: vfio_iommu_unmap_unpin_all(), vfio_release_domain(),
 *   vfio_iommu_iova_free(), kfree().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   close(container fd) → container.c:492 ops->release →
 *   [vfio_iommu_type1_release] → vfio_iommu_unmap_unpin_all()
 */
static void vfio_iommu_type1_release(void *iommu_data)
{
	struct vfio_iommu *iommu = iommu_data;
	/* [한국어] domain 목록 안전 순회용 커서 쌍. */
	struct vfio_domain *domain, *domain_tmp;
	/* [한국어] 에뮬레이트 group 목록 안전 순회용 커서 쌍. */
	struct vfio_iommu_group *group, *next_group;

	/* [한국어] 남아 있는 에뮬레이트 group 래퍼를 모두 해제한다. 하드웨어 자원은 없으므로 메모리만 돌려주면 된다. */
	list_for_each_entry_safe(group, next_group,
			&iommu->emulated_iommu_groups, next) {
		list_del(&group->next);
		/* [한국어] 에뮬레이트 group 래퍼를 해제한다. 하드웨어 자원이 없어 메모리만 돌려주면 된다. */
		kfree(group);
	}

	/* [한국어] 모든 매핑을 없앤다 — 핀과 과금이 여기서 마지막으로 정리된다. domain 해제보다 앞이어야 언맵이 물리 주소를 조회할 수 있다. */
	vfio_iommu_unmap_unpin_all(iommu);

	/* [한국어] 모든 domain 을 정리한다. 매핑 제거가 이보다 먼저여야 언맵이 정상적으로 이루어진다. */
	list_for_each_entry_safe(domain, domain_tmp,
				 &iommu->domain_list, next) {
		vfio_release_domain(domain);
		/* [한국어] 목록에서 뺀다. */
		list_del(&domain->next);
		/* [한국어] domain 래퍼를 해제한다. */
		kfree(domain);
	}

	/* [한국어] 유효 IOVA 목록을 해제한다. */
	vfio_iommu_iova_free(&iommu->iova_list);

	/* [한국어] 컨테이너 객체 자체를 해제한다. 이 시점에 다른 참조자는 없다. */
	kfree(iommu);
}

/* [한국어]
 * vfio_domains_have_enforce_cache_coherency - 모든 domain 이 no-snoop 을 차단하는지 본다
 *
 * @iommu: 컨테이너 객체.
 * @return: 1 이면 모든 domain 이 차단 능력을 갖췄다, 0 이면 하나라도 없다.
 *
 * 배경: PCIe 트랜잭션은 'no-snoop' 비트를 세워 CPU 캐시 일관성 프로토콜을 우회할 수
 * 있다. 그러면 장치가 쓴 데이터가 CPU 캐시와 어긋난다. IOMMU 가 그 비트를 무시하도록
 * 강제할 수 있으면 가상화 계층이 캐시 무효화 명령을 에뮬레이트하지 않아도 되어
 * 게스트 성능이 크게 좋아진다. 사용자는 VFIO_DMA_CC_IOMMU 확장 질의로 이 값을 본다.
 *
 * 동작 단계: 락을 잡고 모든 domain 을 훑어 하나라도 차단 능력이 없으면 0 으로
 * 확정하고 중단한다. domain 이 하나도 없으면 1 이 반환되는데, 매핑도 없는 상태라
 * 의미가 없는 값이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 함수가 직접 iommu->lock 을 잡고 놓는다.
 *
 * caller: vfio_iommu_type1_check_extension() 의 VFIO_DMA_CC_IOMMU 분기.
 * callee: 없다. 목록 순회뿐이다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ioctl(VFIO_CHECK_EXTENSION) → vfio_iommu_type1_check_extension() →
 *   [vfio_domains_have_enforce_cache_coherency]
 */
static int vfio_domains_have_enforce_cache_coherency(struct vfio_iommu *iommu)
{
	struct vfio_domain *domain;
	/* [한국어] 낙관적으로 1 로 시작해, 반례가 있으면 0 으로 내린다. */
	int ret = 1;

	/* [한국어] domain 목록을 훑으므로 락이 필요하다. */
	mutex_lock(&iommu->lock);
	/* [한국어] 모든 domain 을 훑는다. */
	list_for_each_entry(domain, &iommu->domain_list, next) {
		/* [한국어] 하나라도 차단 능력이 없으면 컨테이너 전체로는 보장할 수 없다. */
		if (!(domain->enforce_cache_coherency)) {
			ret = 0;
			break;
		}
	}
	/* [한국어] 락을 놓는다. */
	mutex_unlock(&iommu->lock);

	return ret;
}

/* [한국어]
 * vfio_iommu_has_emulated - 컨테이너에 에뮬레이트(mdev) group 이 있는지 본다
 *
 * @iommu: 컨테이너 객체.
 * @return: true 면 mdev 가 하나 이상 붙어 있다.
 *
 * 한 줄 검사를 함수로 뺀 이유는 iommu->lock 을 잡아야 하기 때문이다. 호출자는
 * 락을 잡지 않은 상태에서 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 함수가 직접 락을 잡고 놓는다.
 *
 * caller: vfio_iommu_type1_check_extension() 의 VFIO_UPDATE_VADDR 분기 한 곳뿐.
 *   상류 주석대로 mdev 가 있으면 vaddr 갱신 기능을 아예 없는 것으로 보고한다 —
 *   vaddr 가 무효인 동안 mdev 는 핀/rw 를 안전하게 할 수 없기 때문이다.
 * callee: 없다.
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   ioctl(VFIO_CHECK_EXTENSION) → vfio_iommu_type1_check_extension() →
 *   [vfio_iommu_has_emulated]
 */
static bool vfio_iommu_has_emulated(struct vfio_iommu *iommu)
{
	bool ret;

	/* [한국어] 에뮬레이트 group 목록을 읽으므로 락이 필요하다. */
	mutex_lock(&iommu->lock);
	/* [한국어] 에뮬레이트 group 목록이 비어 있지 않으면 mdev 가 있다는 뜻이다. */
	ret = !list_empty(&iommu->emulated_iommu_groups);
	/* [한국어] 락을 놓는다. */
	mutex_unlock(&iommu->lock);
	return ret;
}

/* [한국어]
 * vfio_iommu_type1_check_extension - 이 백엔드가 지원하는 확장 기능을 보고한다
 *
 * @iommu: 컨테이너 객체. SET_IOMMU 이전에는 NULL 일 수 있다.
 * @arg: 질의할 확장 상수.
 * @return: 0 이 아니면 지원, 0 이면 미지원.
 *
 * VFIO_CHECK_EXTENSION ioctl 의 처리다. 사용자 공간 라이브러리가 어떤 인터페이스를
 * 쓸지 결정하기 위해 컨테이너를 만들기 전후로 여러 번 부른다.
 *
 * 분기별 의미:
 *  - VFIO_TYPE1_IOMMU / VFIO_TYPE1v2_IOMMU: 이 백엔드가 그 인터페이스 자체다. 항상 1.
 *  - VFIO_UNMAP_ALL: iova/size 를 0 으로 주어 전체 언맵하는 기능. 항상 1.
 *  - VFIO_UPDATE_VADDR: 상류 주석대로 mdev 가 있으면 0 이다. iommu 가 NULL 이면
 *     (SET_IOMMU 전 질의) 그것만으로 0 이 된다.
 *  - VFIO_DMA_CC_IOMMU: 모든 domain 이 no-snoop 을 차단할 수 있어야 1.
 *  - 그 밖: 0.
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 함수 자체는 락을 잡지 않고, 필요한 하위 함수가
 * 각자 잡는다. 그래서 iommu 가 NULL 일 수 있는 경로를 안전하게 처리한다.
 *
 * caller: vfio_iommu_type1_ioctl() 의 VFIO_CHECK_EXTENSION 분기. 그 위는
 *   container.c:216, :225, :297, :353.
 * callee: vfio_iommu_has_emulated(), vfio_domains_have_enforce_cache_coherency().
 * 에러 경로: 없다. 모르는 확장은 0(미지원)이다.
 *
 * 호출 체인:
 *   container.c:297 ops->ioctl → vfio_iommu_type1_ioctl() →
 *   [vfio_iommu_type1_check_extension]
 */
static int vfio_iommu_type1_check_extension(struct vfio_iommu *iommu,
					    unsigned long arg)
{
	switch (arg) {
	/* [한국어] type1 v1 과 v2, 전체 언맵은 이 백엔드가 언제나 지원한다. */
	case VFIO_TYPE1_IOMMU:
	case VFIO_TYPE1v2_IOMMU:
	case VFIO_UNMAP_ALL:
		return 1;
	/* [한국어] 매핑을 유지한 채 사용자 주소만 갱신하는 기능 질의. */
	case VFIO_UPDATE_VADDR:
		/*
		 * Disable this feature if mdevs are present.  They cannot
		 * safely pin/unpin/rw while vaddrs are being updated.
		 */
		/* [한국어] SET_IOMMU 전 질의(iommu 가 NULL)이거나 mdev 가 있으면 이 기능을 없는 것으로 보고한다. */
		return iommu && !vfio_iommu_has_emulated(iommu);
	/* [한국어] no-snoop 차단(캐시 일관성 강제) 지원 여부 질의. */
	case VFIO_DMA_CC_IOMMU:
		if (!iommu)
			return 0;
		/* [한국어] 모든 domain 이 그 능력을 갖췄을 때만 1 을 보고한다. */
		return vfio_domains_have_enforce_cache_coherency(iommu);
	default:
		return 0;
	}
}

/* [한국어]
 * vfio_iommu_iova_add_cap - 유효 IOVA 구간 목록을 capability 체인에 추가한다
 *
 * @caps: capability 체인 버퍼 관리 구조체.
 * @cap_iovas: 채워 넣을 구간 배열이 담긴 임시 버퍼.
 * @size: 그 구조체의 전체 바이트 크기(가변 길이 배열 포함).
 * @return: 0 성공, 음수면 vfio_info_cap_add() 의 오류.
 *
 * VFIO_IOMMU_GET_INFO 응답은 고정 헤더 뒤에 'capability chain' 이라는 가변 목록을
 * 붙여 확장한다. 이 함수는 그 체인에 IOVA 범위 capability 한 개를 붙인다.
 *
 * 동작 단계:
 *  1. vfio_info_cap_add() 로 체인 버퍼를 size 만큼 늘리고 헤더를 채운다.
 *     id 는 VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE, 버전은 1 이다.
 *  2. 반환된 헤더 포인터에서 container_of() 로 바깥 구조체 주소를 얻는다.
 *     헤더가 구조체의 첫 멤버라 오프셋 계산이 필요하다.
 *  3. 구간 개수와 배열 내용을 memcpy 로 옮긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태(호출자가 잡고 있다).
 * 버퍼 재할당으로 잠들 수 있다.
 *
 * caller: vfio_iommu_iova_build_caps() 한 곳뿐.
 * callee: vfio_info_cap_add()(drivers/vfio/vfio_main.c 에 구현), container_of(), memcpy().
 * 에러 경로: vfio_info_cap_add() 는 -ENOMEM 시 체인 버퍼를 스스로 해제하고 NULL 로
 *   만들어 주므로, 여기서는 오류를 그대로 올리기만 하면 된다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_get_info() → vfio_iommu_iova_build_caps() →
 *   [vfio_iommu_iova_add_cap] → vfio_info_cap_add()
 */
static int vfio_iommu_iova_add_cap(struct vfio_info_cap *caps,
		 struct vfio_iommu_type1_info_cap_iova_range *cap_iovas,
		 size_t size)
{
	struct vfio_info_cap_header *header;
	/* [한국어] 체인에서 얻은 헤더로부터 복원할 바깥 구조체 포인터. */
	struct vfio_iommu_type1_info_cap_iova_range *iova_cap;

	/* [한국어] 체인 버퍼를 size 만큼 늘리고 헤더(id, version)를 채워 그 주소를 돌려받는다. */
	header = vfio_info_cap_add(caps, size,
				   VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE, 1);
	/* [한국어] -ENOMEM 이면 ERR_PTR 로 온다. 이때 체인 버퍼는 그 함수가 이미 해제해 두었다. */
	if (IS_ERR(header))
		return PTR_ERR(header);

	/* [한국어] 헤더가 구조체의 첫 멤버이므로 container_of 로 바깥 구조체 주소를 되찾는다. */
	iova_cap = container_of(header,
				struct vfio_iommu_type1_info_cap_iova_range,
				header);
	/* [한국어] 구간 개수를 옮긴다. */
	iova_cap->nr_iovas = cap_iovas->nr_iovas;
	/* [한국어] 구간 배열 전체를 옮긴다. 크기는 개수 곱하기 항목 크기다. */
	memcpy(iova_cap->iova_ranges, cap_iovas->iova_ranges,
	       cap_iovas->nr_iovas * sizeof(*cap_iovas->iova_ranges));
	return 0;
}

/* [한국어]
 * vfio_iommu_iova_build_caps - 유효 IOVA 목록을 사용자에게 보고할 형태로 만든다
 *
 * @iommu: 컨테이너 객체.
 * @caps: capability 체인.
 * @return: 0 성공, -ENOMEM.
 *
 * 사용자 공간 드라이버는 어느 IOVA 를 써도 되는지 알아야 매핑 주소를 고를 수 있다.
 * 이 함수가 iommu->iova_list 를 uapi 구조체 배열로 옮겨 담는다.
 *
 * 동작 단계:
 *  1. 구간 개수를 센다.
 *  2. 상류 주석대로 개수가 0 이면 조용히 0 을 반환한다 — mdev 만 있는 컨테이너는
 *     목록이 비어 있고, 그것은 오류가 아니라 '제약 없음' 이다.
 *  3. struct_size() 로 가변 길이 배열을 포함한 전체 크기를 안전하게 계산한다.
 *     곱셈 오버플로를 막아 주는 매크로다.
 *  4. 임시 버퍼를 할당하고 개수와 각 구간의 start/end 를 채운다.
 *  5. vfio_iommu_iova_add_cap() 으로 체인에 붙이고 임시 버퍼를 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 할당으로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_get_info() 한 곳뿐.
 * callee: struct_size(), kzalloc(), vfio_iommu_iova_add_cap(), kfree().
 * 에러 경로: -ENOMEM 을 호출자에게 올린다.
 *
 * [상류 코드 관찰] 4 번의 kzalloc 이 실패해 -ENOMEM 으로 반환하는 경로에서는
 * caps->buf 를 건드리지 않는다. 그런데 호출자 vfio_iommu_type1_get_info() 는
 * 앞선 migration/dma_avail capability 로 이미 채워진 caps.buf 를 해제하지 않고
 * 곧바로 return ret 로 빠져나가므로, 이 좁은 실패 경로에서 그 버퍼가 새어 나간다.
 * 같은 함수 안의 다른 실패 경로(vfio_info_cap_add() 의 -ENOMEM)는 그 함수가 스스로
 * 버퍼를 해제하므로 문제가 없다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_get_info() → [vfio_iommu_iova_build_caps] →
 *   vfio_iommu_iova_add_cap()
 */
static int vfio_iommu_iova_build_caps(struct vfio_iommu *iommu,
				      struct vfio_info_cap *caps)
{
	struct vfio_iommu_type1_info_cap_iova_range *cap_iovas;
	/* [한국어] iova_list 순회용 커서. */
	struct vfio_iova *iova;
	/* [한국어] 가변 길이 구조체의 전체 바이트 크기. */
	size_t size;
	/* [한국어] 구간 개수와 채워 넣을 인덱스, 결과 코드. */
	int iovas = 0, i = 0, ret;

	/* [한국어] 먼저 구간이 몇 개인지 센다 — 가변 배열 크기를 정하려면 개수가 먼저 필요하다. */
	list_for_each_entry(iova, &iommu->iova_list, list)
		/* [한국어] 하나씩 센다. */
		iovas++;

	/* [한국어] 구간이 하나도 없으면(mdev 전용 컨테이너) capability 를 만들지 않는다. */
	if (!iovas) {
		/*
		 * Return 0 as a container with a single mdev device
		 * will have an empty list
		 */
		return 0;
	}

	/* [한국어] 헤더 + 구간 배열의 전체 크기를 곱셈 오버플로 없이 계산한다. */
	size = struct_size(cap_iovas, iova_ranges, iovas);

	/* [한국어] 임시 버퍼를 0 초기화 할당한다. 사용자에게 그대로 복사되므로 패딩까지 0 이어야 정보가 새지 않는다. */
	cap_iovas = kzalloc(size, GFP_KERNEL);
	/* [한국어] 할당 실패면 capability 를 만들 수 없다. */
	if (!cap_iovas)
		return -ENOMEM;

	/* [한국어] 구간 개수를 채운다. */
	cap_iovas->nr_iovas = iovas;

	/* [한국어] 다시 순회하며 각 구간을 배열에 옮겨 담는다. */
	list_for_each_entry(iova, &iommu->iova_list, list) {
		/* [한국어] 구간의 시작 주소. */
		cap_iovas->iova_ranges[i].start = iova->start;
		/* [한국어] 구간의 끝 주소(포함). */
		cap_iovas->iova_ranges[i].end = iova->end;
		/* [한국어] 다음 배열 칸으로 이동. */
		i++;
	}

	/* [한국어] 완성된 구조체를 capability 체인에 붙인다. */
	ret = vfio_iommu_iova_add_cap(caps, cap_iovas, size);

	/* [한국어] 임시 버퍼를 해제한다. 내용은 이미 체인 버퍼로 복사되었다. */
	kfree(cap_iovas);
	return ret;
}

/* [한국어]
 * vfio_iommu_migration_build_caps - 마이그레이션 관련 능력을 capability 체인에 붙인다
 *
 * @iommu: 컨테이너 객체.
 * @caps: capability 체인.
 * @return: 0 성공, 음수면 오류.
 *
 * 사용자 공간(대개 VMM)이 dirty page 추적을 쓰려면 비트맵의 페이지 단위와 한 번에
 * 요청할 수 있는 최대 크기를 알아야 한다. 그것을 알려 주는 capability 다.
 *
 * 동작 단계:
 *  1. 스택에 구조체를 0 초기화해 잡는다.
 *  2. 헤더의 id 와 버전을 채운다.
 *  3. flags 는 0 — 현재 정의된 추가 플래그가 없다.
 *  4. pgsize_bitmap 에는 최소 지원 페이지 크기 하나만 담는다. 비트맵은 그 단위로만
 *     다루므로 여러 크기를 광고하지 않는다.
 *  5. max_dirty_bitmap_size 는 DIRTY_BITMAP_SIZE_MAX.
 *  6. vfio_info_add_capability() 로 체인에 통째로 복사해 붙인다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태. 버퍼 재할당으로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_get_info() 한 곳뿐이며, 세 capability 중 가장 먼저 불린다.
 * callee: __ffs(), vfio_info_add_capability().
 * 에러 경로: -ENOMEM 이면 그 함수가 체인 버퍼를 스스로 정리한다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_get_info() → [vfio_iommu_migration_build_caps] →
 *   vfio_info_add_capability()
 */
static int vfio_iommu_migration_build_caps(struct vfio_iommu *iommu,
					   struct vfio_info_cap *caps)
{
	struct vfio_iommu_type1_info_cap_migration cap_mig = {};

	/* [한국어] 마이그레이션 capability 식별자. */
	cap_mig.header.id = VFIO_IOMMU_TYPE1_INFO_CAP_MIGRATION;
	/* [한국어] 이 구조체의 버전. */
	cap_mig.header.version = 1;

	/* [한국어] 현재 정의된 추가 플래그가 없다. */
	cap_mig.flags = 0;
	/* support minimum pgsize */
	/* [한국어] 최소 지원 페이지 크기 하나만 광고한다. 비트맵은 언제나 그 단위이므로 여러 크기를 알릴 이유가 없다. */
	cap_mig.pgsize_bitmap = (size_t)1 << __ffs(iommu->pgsize_bitmap);
	/* [한국어] 커널이 한 번에 다룰 수 있는 dirty 비트맵의 최대 바이트 수. */
	cap_mig.max_dirty_bitmap_size = DIRTY_BITMAP_SIZE_MAX;

	/* [한국어] 완성된 구조체를 체인 버퍼에 통째로 복사해 붙인다. */
	return vfio_info_add_capability(caps, &cap_mig.header, sizeof(cap_mig));
}

/* [한국어]
 * vfio_iommu_dma_avail_build_caps - 남은 매핑 슬롯 수를 capability 체인에 붙인다
 *
 * @iommu: 컨테이너 객체.
 * @caps: capability 체인.
 * @return: 0 성공, 음수면 오류.
 *
 * 사용자 공간이 매핑을 몇 개나 더 만들 수 있는지 미리 알 수 있게 해 준다.
 * dma_entry_limit 로 정해진 상한에 가까워지면 매핑을 합치는 등의 대응을 할 수 있다.
 *
 * 동작 단계: 스택 구조체에 id/버전과 현재 dma_avail 값을 채워
 * vfio_info_add_capability() 로 붙인다. 이 구조체는 가변 부분이 없어
 * migration capability 와 달리 0 초기화 없이 전 필드를 직접 채운다.
 *
 * 실행 컨텍스트: 프로세스 문맥, iommu->lock 을 쥔 상태.
 *
 * caller: vfio_iommu_type1_get_info() 한 곳뿐.
 * callee: vfio_info_add_capability().
 * 에러 경로: -ENOMEM 이면 그 함수가 체인 버퍼를 스스로 정리한다.
 *
 * 호출 체인:
 *   vfio_iommu_type1_get_info() → [vfio_iommu_dma_avail_build_caps] →
 *   vfio_info_add_capability()
 */
static int vfio_iommu_dma_avail_build_caps(struct vfio_iommu *iommu,
					   struct vfio_info_cap *caps)
{
	struct vfio_iommu_type1_info_dma_avail cap_dma_avail;

	/* [한국어] capability 식별자 — 사용자가 이 id 로 체인에서 찾아낸다. */
	cap_dma_avail.header.id = VFIO_IOMMU_TYPE1_INFO_DMA_AVAIL;
	/* [한국어] 이 capability 구조체의 버전. 필드가 늘어나면 올라간다. */
	cap_dma_avail.header.version = 1;

	/* [한국어] 지금 남은 매핑 슬롯 수를 그대로 알려 준다. */
	cap_dma_avail.avail = iommu->dma_avail;

	/* [한국어] 체인 버퍼에 통째로 복사해 붙인다. */
	return vfio_info_add_capability(caps, &cap_dma_avail.header,
					sizeof(cap_dma_avail));
}

/* [한국어]
 * vfio_iommu_type1_get_info - VFIO_IOMMU_GET_INFO 를 처리해 컨테이너 능력을 보고한다
 *
 * @iommu: 컨테이너 객체.
 * @arg: 사용자 공간의 struct vfio_iommu_type1_info 주소.
 * @return: 0 성공, -EFAULT/-EINVAL/-ENOMEM.
 *
 * 사용자 공간 드라이버가 매핑을 시작하기 전에 반드시 부르는 질의다. 여기서
 * '어떤 페이지 크기로 정렬해야 하는가' 와 '어느 IOVA 를 써도 되는가' 를 얻는다.
 *
 * argsz 규약: 사용자가 구조체 크기를 argsz 에 적어 보내고, 커널이 필요한 크기를
 * 같은 필드에 적어 돌려주는 확장 가능한 ABI 다. 버전이 다른 사용자/커널이 섞여도
 * 동작하게 하는 표준 VFIO 관용구다.
 *
 * 동작 단계:
 *  1. minsz 를 iova_pgsizes 필드 끝까지로 잡고 그만큼만 사용자에게서 복사한다.
 *  2. 사용자가 적은 argsz 가 minsz 보다 작으면 -EINVAL.
 *  3. 실제로 되돌려 줄 크기를 argsz 와 구조체 크기 중 작은 값으로 정한다.
 *  4. 락을 잡고 PGSIZES 플래그와 페이지 크기 비트맵을 채운다.
 *  5. migration -> dma_avail -> iova 순으로 capability 세 개를 만든다. 앞이 실패하면
 *     뒤는 건너뛴다.
 *  6. 락을 놓는다. 오류가 있으면 그대로 반환한다.
 *  7. capability 가 하나라도 있으면 CAPS 플래그를 세운다.
 *     - 사용자 버퍼가 부족하면 필요한 크기만 argsz 에 적어 돌려준다. 사용자는
 *       버퍼를 키워 다시 부른다.
 *     - 충분하면 vfio_info_cap_shift() 로 체인 내부의 상대 오프셋을 사용자 버퍼
 *       기준으로 보정한 뒤, 구조체 뒤에 체인을 복사하고 cap_offset 을 알려 준다.
 *  8. 마지막으로 고정 부분을 사용자에게 복사한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, ioctl 경로. 사용자 메모리 접근과 할당으로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_ioctl() 의 VFIO_IOMMU_GET_INFO 분기.
 * callee: copy_from_user(), vfio_iommu_migration_build_caps(),
 *   vfio_iommu_dma_avail_build_caps(), vfio_iommu_iova_build_caps(),
 *   vfio_info_cap_shift(), copy_to_user(), kfree().
 * 에러 경로: 6 번의 return ret 경로에서 caps.buf 정리가 빠질 수 있다 —
 *   vfio_iommu_iova_build_caps() 의 주석에 그 조건을 적어 두었다.
 *
 * 호출 체인:
 *   container.c:353 ops->ioctl → vfio_iommu_type1_ioctl() →
 *   [vfio_iommu_type1_get_info] → vfio_iommu_iova_build_caps()
 */
static int vfio_iommu_type1_get_info(struct vfio_iommu *iommu,
				     unsigned long arg)
{
	struct vfio_iommu_type1_info info = {};
	/* [한국어] 사용자와 주고받을 고정부의 크기. */
	unsigned long minsz;
	/* [한국어] capability 체인 버퍼 관리 구조체. 처음에는 비어 있다. */
	struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
	/* [한국어] capability 생성 결과. */
	int ret;

	/* [한국어] iova_pgsizes 필드 끝까지가 모든 버전 공통의 고정부다. */
	minsz = offsetofend(struct vfio_iommu_type1_info, iova_pgsizes);

	/* [한국어] 사용자가 적어 보낸 argsz 를 포함한 고정부를 복사해 온다. */
	if (copy_from_user(&info, (void __user *)arg, minsz))
		return -EFAULT;

	/* [한국어] argsz 가 고정부보다 작으면 대화가 성립하지 않는다. */
	if (info.argsz < minsz)
		return -EINVAL;

	/* [한국어] 실제로 되돌려 줄 크기 — 사용자 버퍼와 커널 구조체 중 작은 쪽이다. 버전이 다른 양쪽이 안전하게 통신하게 해 준다. */
	minsz = min_t(size_t, info.argsz, sizeof(info));

	/* [한국어] 컨테이너 상태를 읽어 capability 를 만드는 동안 락이 필요하다. */
	mutex_lock(&iommu->lock);
	/* [한국어] 페이지 크기 비트맵을 보고한다는 표시. */
	info.flags = VFIO_IOMMU_INFO_PGSIZES;

	/* [한국어] 컨테이너가 지원하는 IOMMU 페이지 크기 집합. 사용자는 이 값으로 매핑 정렬을 정한다. */
	info.iova_pgsizes = iommu->pgsize_bitmap;

	/* [한국어] 마이그레이션 capability 를 먼저 붙인다. */
	ret = vfio_iommu_migration_build_caps(iommu, &caps);

	/* [한국어] 앞이 성공했을 때만 다음을 시도한다. */
	if (!ret)
		/* [한국어] 남은 매핑 슬롯 수 capability. */
		ret = vfio_iommu_dma_avail_build_caps(iommu, &caps);

	/* [한국어] 역시 앞이 성공했을 때만. */
	if (!ret)
		/* [한국어] 유효 IOVA 범위 capability. */
		ret = vfio_iommu_iova_build_caps(iommu, &caps);

	/* [한국어] 락을 놓는다. 이후 사용자 메모리 접근은 락 밖에서 한다 — 페이지 폴트로 오래 잠들 수 있기 때문이다. */
	mutex_unlock(&iommu->lock);

	/* [한국어] capability 생성에 실패했으면 그대로 반환한다. */
	if (ret)
		return ret;

	/* [한국어] 붙은 capability 가 하나라도 있으면 체인을 사용자에게 넘겨야 한다. */
	if (caps.size) {
		/* [한국어] capability 체인이 있다는 표시를 플래그에 세운다. */
		info.flags |= VFIO_IOMMU_INFO_CAPS;

		/* [한국어] 사용자 버퍼가 고정부 + 체인을 담기에 부족한 경우. */
		if (info.argsz < sizeof(info) + caps.size) {
			/* [한국어] 필요한 크기만 알려 준다. 사용자는 버퍼를 키워 다시 부른다. */
			info.argsz = sizeof(info) + caps.size;
		} else {
			/* [한국어] 체인 내부의 상대 오프셋을 사용자 버퍼 기준으로 보정한다. 체인은 버퍼 머리 기준으로 만들어져 있다. */
			vfio_info_cap_shift(&caps, sizeof(info));
			/* [한국어] 고정부 바로 뒤에 체인을 복사한다. */
			if (copy_to_user((void __user *)arg +
					sizeof(info), caps.buf,
					caps.size)) {
				kfree(caps.buf);
				/* [한국어] 체인 복사에 실패했으면 버퍼를 해제하고 -EFAULT. */
				return -EFAULT;
			}
			/* [한국어] 체인이 시작되는 오프셋을 알려 준다. */
			info.cap_offset = sizeof(info);
		}

		/* [한국어] 체인 버퍼를 해제한다. 사용자에게 복사가 끝났으므로 더 필요 없다. */
		kfree(caps.buf);
	}

	/* [한국어] 마지막으로 고정부를 사용자에게 복사한다. 실패하면 -EFAULT. */
	return copy_to_user((void __user *)arg, &info, minsz) ?
			-EFAULT : 0;
}

/* [한국어]
 * vfio_iommu_type1_map_dma - VFIO_IOMMU_MAP_DMA ioctl 인자를 검증하고 본체로 넘긴다
 *
 * @iommu: 컨테이너 객체.
 * @arg: 사용자 공간의 struct vfio_iommu_type1_dma_map 주소.
 * @return: 0 성공, -EFAULT(복사 실패), -EINVAL(argsz/플래그 위반), 그 밖에
 *          vfio_dma_do_map() 의 오류.
 *
 * 사용자 인자를 커널 스택으로 안전하게 들여오는 얇은 껍질이다. 실제 작업은
 * vfio_dma_do_map() 이 한다. 껍질을 분리해 두면 인자 검증과 매핑 논리를 따로
 * 읽을 수 있다.
 *
 * 동작 단계:
 *  1. 허용 플래그 마스크를 READ/WRITE/VADDR 로 정한다.
 *  2. minsz 를 size 필드 끝까지로 잡고 그만큼 복사한다.
 *  3. argsz 가 minsz 보다 작거나 모르는 플래그가 섞였으면 -EINVAL. 모르는 플래그를
 *     거절하는 것은 미래에 플래그가 추가될 때 옛 커널이 조용히 무시하지 않게
 *     하려는 ABI 규약이다.
 *  4. vfio_dma_do_map() 에 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥, ioctl 경로. 락은 잡지 않는다 —
 * vfio_dma_do_map() 이 직접 잡는다. 사용자 복사로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_ioctl().
 * callee: copy_from_user(), vfio_dma_do_map().
 * 에러 경로: 오류를 그대로 사용자에게 올린다.
 *
 * 호출 체인:
 *   ioctl(VFIO_IOMMU_MAP_DMA) → container.c:353 ops->ioctl →
 *   vfio_iommu_type1_ioctl() → [vfio_iommu_type1_map_dma] → vfio_dma_do_map()
 */
static int vfio_iommu_type1_map_dma(struct vfio_iommu *iommu,
				    unsigned long arg)
{
	struct vfio_iommu_type1_dma_map map;
	/* [한국어] 고정부 크기. */
	unsigned long minsz;
	/* [한국어] 허용 플래그 마스크 — 읽기/쓰기 권한과 vaddr 갱신. */
	uint32_t mask = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE |
			VFIO_DMA_MAP_FLAG_VADDR;

	/* [한국어] size 필드 끝까지가 모든 버전 공통의 고정부다. */
	minsz = offsetofend(struct vfio_iommu_type1_dma_map, size);

	/* [한국어] 사용자 인자를 커널 스택으로 복사해 온다. 이후 검증은 커널 사본을 대상으로 한다. */
	if (copy_from_user(&map, (void __user *)arg, minsz))
		return -EFAULT;

	/* [한국어] argsz 가 부족하거나 모르는 플래그가 섞였으면 거절한다. 모르는 플래그를 조용히 무시하지 않는 것이 VFIO ABI 규약이다. */
	if (map.argsz < minsz || map.flags & ~mask)
		return -EINVAL;

	/* [한국어] 검증이 끝난 커널 사본을 본체에 넘긴다. 락은 본체가 잡는다. */
	return vfio_dma_do_map(iommu, &map);
}

/* [한국어]
 * vfio_iommu_type1_unmap_dma - VFIO_IOMMU_UNMAP_DMA ioctl 인자를 검증하고 본체로 넘긴다
 *
 * @iommu: 컨테이너 객체.
 * @arg: 사용자 공간의 struct vfio_iommu_type1_dma_unmap 주소.
 * @return: 0 성공, -EFAULT/-EINVAL, 그 밖에 vfio_dma_do_unmap() 의 오류.
 *
 * MAP 쪽 껍질보다 하는 일이 많다. dirty 비트맵을 함께 가져가는 변형이 있어
 * 추가 구조체를 더 읽어 오고 그 크기를 검증해야 하기 때문이다.
 *
 * 동작 단계:
 *  1. 허용 플래그를 GET_DIRTY_BITMAP / VADDR / ALL 로 정한다.
 *  2. 고정부를 복사하고 argsz/플래그를 검증한다.
 *  3. GET_DIRTY_BITMAP 은 ALL 이나 VADDR 과 함께 쓸 수 없다 — 전체 언맵이나 주소
 *     무효화는 비트맵 범위를 특정할 수 없기 때문이다.
 *  4. GET_DIRTY_BITMAP 이면
 *     - argsz 가 고정부 + struct vfio_bitmap 을 담을 만큼 커야 한다.
 *     - 고정부 뒤에서 vfio_bitmap 을 복사해 온다.
 *     - access_ok() 로 사용자 비트맵 버퍼가 접근 가능한 주소 범위인지 미리 확인한다.
 *       실제 복사는 나중에 락 안에서 일어나므로 여기서 걸러 두는 편이 낫다.
 *     - 페이지 수를 bitmap.pgsize 로 나눠 verify_bitmap_size() 로 버퍼 크기를 검증한다.
 *  5. vfio_dma_do_unmap() 을 부른다.
 *  6. 성공하면 실제 언맵된 크기가 담긴 고정부를 사용자에게 되돌려 준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, ioctl 경로. 락은 본체가 잡는다.
 *
 * caller: vfio_iommu_type1_ioctl().
 * callee: copy_from_user(), access_ok(), __ffs(), verify_bitmap_size(),
 *   vfio_dma_do_unmap(), copy_to_user().
 * 에러 경로: 오류를 그대로 올린다. 마지막 copy_to_user 가 실패하면 -EFAULT 인데,
 *   이때 언맵은 이미 끝난 상태라 사용자는 실제 언맵량을 알 수 없다.
 *
 * 호출 체인:
 *   ioctl(VFIO_IOMMU_UNMAP_DMA) → vfio_iommu_type1_ioctl() →
 *   [vfio_iommu_type1_unmap_dma] → vfio_dma_do_unmap()
 */
static int vfio_iommu_type1_unmap_dma(struct vfio_iommu *iommu,
				      unsigned long arg)
{
	struct vfio_iommu_type1_dma_unmap unmap;
	/* [한국어] dirty 비트맵 인자. 사용하지 않는 경로에서도 0 으로 초기화해 두어야 본체가 안전하게 참조한다. */
	struct vfio_bitmap bitmap = { 0 };
	/* [한국어] 허용 플래그 마스크 — 비트맵 회수 / vaddr 무효화 / 전체 언맵. */
	uint32_t mask = VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP |
			VFIO_DMA_UNMAP_FLAG_VADDR |
			VFIO_DMA_UNMAP_FLAG_ALL;
	/* [한국어] 고정부 크기. */
	unsigned long minsz;
	/* [한국어] 본체 호출 결과. */
	int ret;

	/* [한국어] size 필드 끝까지가 고정부다. */
	minsz = offsetofend(struct vfio_iommu_type1_dma_unmap, size);

	/* [한국어] 고정부를 복사해 온다. */
	if (copy_from_user(&unmap, (void __user *)arg, minsz))
		return -EFAULT;

	/* [한국어] argsz 가 부족하거나 모르는 플래그가 섞였으면 거절한다. */
	if (unmap.argsz < minsz || unmap.flags & ~mask)
		return -EINVAL;

	/* [한국어] 비트맵 회수는 전체 언맵이나 vaddr 무효화와 함께 쓸 수 없다 —
	 * 그 둘은 비트맵을 담을 IOVA 범위를 특정할 수 없기 때문이다. */
	if ((unmap.flags & VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP) &&
	    (unmap.flags & (VFIO_DMA_UNMAP_FLAG_ALL |
			    VFIO_DMA_UNMAP_FLAG_VADDR)))
		return -EINVAL;

	/* [한국어] 비트맵을 함께 회수하는 변형이면 추가 인자를 읽어야 한다. */
	if (unmap.flags & VFIO_DMA_UNMAP_FLAG_GET_DIRTY_BITMAP) {
		/* [한국어] 비트맵 페이지 크기의 시프트 값. */
		unsigned long pgshift;

		/* [한국어] 고정부 뒤에 vfio_bitmap 을 담을 만큼 argsz 가 커야 한다. */
		if (unmap.argsz < (minsz + sizeof(bitmap)))
			return -EINVAL;

		/* [한국어] 고정부 바로 뒤에서 비트맵 서술자를 복사해 온다. */
		if (copy_from_user(&bitmap,
				   (void __user *)(arg + minsz),
				   sizeof(bitmap)))
			return -EFAULT;

		/* [한국어] 사용자 비트맵 버퍼의 접근 가능성을 미리 확인한다. */
		if (!access_ok((void __user *)bitmap.data, bitmap.size))
			return -EINVAL;

		/* [한국어] 사용자가 지정한 비트맵 페이지 크기의 시프트. */
		pgshift = __ffs(bitmap.pgsize);
		/* [한국어] 언맵 범위의 페이지 수에 비해 버퍼가 충분한지 검증한다. */
		ret = verify_bitmap_size(unmap.size >> pgshift,
					 bitmap.size);
		/* [한국어] 본체가 실패했으면 사용자에게 되돌려 줄 내용이 없다. */
		if (ret)
			return ret;
	}

	/* [한국어] 검증이 끝났으니 본체에 넘긴다. 락은 본체가 잡는다. */
	ret = vfio_dma_do_unmap(iommu, &unmap, &bitmap);
	/* [한국어] 실패하면 사용자에게 되돌려 줄 것이 없다. */
	if (ret)
		return ret;

	/* [한국어] 실제 언맵된 크기가 담긴 고정부를 사용자에게 돌려준다. */
	return copy_to_user((void __user *)arg, &unmap, minsz) ?
			-EFAULT : 0;
}

/* [한국어]
 * vfio_iommu_type1_dirty_pages - VFIO_IOMMU_DIRTY_PAGES ioctl 을 처리한다
 *
 * @iommu: 컨테이너 객체.
 * @arg: 사용자 공간의 struct vfio_iommu_type1_dirty_bitmap 주소.
 * @return: 0 성공, -EACCES(v1), -EFAULT, -EINVAL, -EOVERFLOW, -ENOMEM.
 *
 * 라이브 마이그레이션의 dirty page 추적을 제어한다. 세 가지 동작이 플래그로
 * 구분되며 한 번에 하나만 쓸 수 있다.
 *
 * 동작 단계:
 *  1. v2 전용이므로 v1 이면 -EACCES.
 *  2. flags 필드 끝까지를 복사하고 argsz/플래그 마스크를 검증한다.
 *  3. __ffs(dirty.flags) != __fls(dirty.flags) 로 '세워진 비트가 정확히 하나인가' 를
 *     확인한다. 최하위 1 비트와 최상위 1 비트의 위치가 같다는 것은 비트가 하나뿐이라는
 *     뜻이다.
 *  4. START: 락을 잡고, 아직 켜져 있지 않으면 모든 매핑에 비트맵을 만든 뒤 플래그를
 *     켠다. 이미 켜져 있으면 조용히 성공한다(멱등).
 *  5. STOP: 락을 잡고, 켜져 있으면 플래그를 내리고 모든 비트맵을 해제한다.
 *  6. GET_BITMAP: 고정부 뒤에 오는 struct vfio_iommu_type1_dirty_bitmap_get 을 읽어
 *     - 크기가 충분한지, __u64 값이 좁은 타입에 그대로 담기는지 확인하고
 *     - size 가 0 이 아니고 iova+size-1 이 넘치지 않는지 확인하고
 *     - access_ok() 로 사용자 버퍼 접근 가능성을 확인하고
 *     - verify_bitmap_size() 로 버퍼 크기를 검증한다.
 *     그런 다음 락을 잡고 비트맵 페이지 크기가 최소 지원 크기와 정확히 같은지,
 *     iova 와 size 가 그 크기에 정렬되어 있는지 본다. 정렬 검사는 모두
 *     (x & (iommu_pgsize - 1)) 관용구다. 마지막으로 추적이 켜져 있으면
 *     vfio_iova_dirty_bitmap() 으로 실제 보고를 하고, 꺼져 있으면 -EINVAL 이다.
 *  7. 어느 플래그도 아니면 -EINVAL.
 *
 * 실행 컨텍스트: 프로세스 문맥, ioctl 경로. 이 함수가 분기마다 직접 락을 잡고 놓는다.
 * 사용자 복사와 할당으로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_ioctl().
 * callee: copy_from_user(), __ffs(), __fls(), vfio_dma_bitmap_alloc_all(),
 *   vfio_dma_bitmap_free_all(), access_ok(), verify_bitmap_size(),
 *   vfio_iova_dirty_bitmap().
 * 에러 경로: GET_BITMAP 의 락 안쪽 실패는 out_unlock 라벨로 모여 락을 놓고 반환한다.
 *
 * 호출 체인:
 *   ioctl(VFIO_IOMMU_DIRTY_PAGES) → vfio_iommu_type1_ioctl() →
 *   [vfio_iommu_type1_dirty_pages] → vfio_iova_dirty_bitmap()
 */
static int vfio_iommu_type1_dirty_pages(struct vfio_iommu *iommu,
					unsigned long arg)
{
	struct vfio_iommu_type1_dirty_bitmap dirty;
	/* [한국어] 허용 플래그 마스크 — START / STOP / GET_BITMAP 셋뿐이다. */
	uint32_t mask = VFIO_IOMMU_DIRTY_PAGES_FLAG_START |
			VFIO_IOMMU_DIRTY_PAGES_FLAG_STOP |
			VFIO_IOMMU_DIRTY_PAGES_FLAG_GET_BITMAP;
	/* [한국어] 사용자와 주고받을 고정부의 크기. */
	unsigned long minsz;
	/* [한국어] 반환값. 분기마다 갱신된다. */
	int ret = 0;

	/* [한국어] dirty 추적은 v2 전용 기능이다. v1 컨테이너면 -EACCES. */
	if (!iommu->v2)
		return -EACCES;

	/* [한국어] flags 필드 끝까지가 모든 버전이 공통으로 갖는 고정부다. */
	minsz = offsetofend(struct vfio_iommu_type1_dirty_bitmap, flags);

	/* [한국어] 사용자에게서 고정부만 먼저 복사해 온다. */
	if (copy_from_user(&dirty, (void __user *)arg, minsz))
		return -EFAULT;

	/* [한국어] argsz 가 고정부보다 작거나 모르는 플래그가 섞였으면 거절한다. */
	if (dirty.argsz < minsz || dirty.flags & ~mask)
		return -EINVAL;

	/* only one flag should be set at a time */
	/* [한국어] 최하위 1 비트와 최상위 1 비트의 위치가 같다는 것은 세워진 비트가 정확히 하나뿐이라는 뜻이다. */
	if (__ffs(dirty.flags) != __fls(dirty.flags))
		/* [한국어] 두 개 이상 세워졌거나 하나도 없으면 무엇을 하려는지 알 수 없다. */
		return -EINVAL;

	/* [한국어] START 분기 — 추적을 시작한다. */
	if (dirty.flags & VFIO_IOMMU_DIRTY_PAGES_FLAG_START) {
		/* [한국어] 비트맵 한 비트가 뜻할 바이트 크기. */
		size_t pgsize;

		/* [한국어] 매핑 트리 전체를 순회하며 비트맵을 만들므로 락이 필요하다. */
		mutex_lock(&iommu->lock);
		/* [한국어] 컨테이너가 지원하는 최소 IOMMU 페이지 크기. 비트맵은 언제나 이 단위다. */
		pgsize = 1 << __ffs(iommu->pgsize_bitmap);
		/* [한국어] 이미 켜져 있으면 아무것도 하지 않는다 — 이 ioctl 은 멱등이다. */
		if (!iommu->dirty_page_tracking) {
			/* [한국어] 모든 매핑에 비트맵을 만든다. 하나라도 실패하면 전부 되감긴다. */
			ret = vfio_dma_bitmap_alloc_all(iommu, pgsize);
			/* [한국어] 전부 성공했을 때만 플래그를 켠다. 비트맵 없는 매핑이 남으면 안 되기 때문이다. */
			if (!ret)
				iommu->dirty_page_tracking = true;
		}
		/* [한국어] 락을 놓는다. */
		mutex_unlock(&iommu->lock);
		return ret;
	/* [한국어] STOP 분기 — 추적을 멈춘다. */
	} else if (dirty.flags & VFIO_IOMMU_DIRTY_PAGES_FLAG_STOP) {
		mutex_lock(&iommu->lock);
		/* [한국어] 켜져 있을 때만 정리한다. */
		if (iommu->dirty_page_tracking) {
			/* [한국어] 먼저 플래그를 내려 새 비트 기록을 막고, 그 다음 비트맵을 해제한다. */
			iommu->dirty_page_tracking = false;
			/* [한국어] 플래그를 내린 뒤에 해제해야 그 사이 다른 경로가 해제된 비트맵을 만지지 않는다. */
			vfio_dma_bitmap_free_all(iommu);
		}
		/* [한국어] 락을 놓는다. */
		mutex_unlock(&iommu->lock);
		return 0;
	/* [한국어] GET_BITMAP 분기 — 지금까지 쌓인 dirty 정보를 사용자에게 넘긴다. */
	} else if (dirty.flags & VFIO_IOMMU_DIRTY_PAGES_FLAG_GET_BITMAP) {
		/* [한국어] 고정부 뒤에 오는 가변 인자 구조체. */
		struct vfio_iommu_type1_dirty_bitmap_get range;
		/* [한국어] 사용자가 지정한 비트맵 페이지 크기의 시프트 값. */
		unsigned long pgshift;
		/* [한국어] 사용자가 고정부 뒤에 붙여 보낸 데이터의 크기. */
		size_t data_size = dirty.argsz - minsz;
		/* [한국어] 요청 길이와, 컨테이너가 강제하는 최소 IOMMU 페이지 크기. */
		size_t size, iommu_pgsize;
		/* [한국어] 요청 범위의 시작 IOVA 와 마지막 바이트 주소. */
		dma_addr_t iova, iova_end;

		/* [한국어] 가변부가 없거나 필요한 구조체보다 작으면 거절한다. */
		if (!data_size || data_size < sizeof(range))
			return -EINVAL;

		/* [한국어] 고정부 바로 뒤에서 범위 지정 구조체를 복사해 온다. */
		if (copy_from_user(&range, (void __user *)(arg + minsz),
				   sizeof(range)))
			return -EFAULT;

		/* [한국어] __u64 인자를 dma_addr_t 로 좁혀 담는다. */
		iova = range.iova;
		/* [한국어] 크기도 size_t 로 좁혀 담는다. */
		size = range.size;

		/* [한국어] 좁히는 과정에서 값이 잘렸는지 확인한다(32비트 커널 방어). */
		if (iova != range.iova || size != range.size)
			return -EOVERFLOW;

		/* [한국어] 0 바이트 범위는 뜻이 없다. */
		if (!size)
			return -EINVAL;

		/* [한국어] 닫힌 구간의 끝을 구하면서 주소 공간을 넘는지 확인한다. */
		if (check_add_overflow(iova, size - 1, &iova_end))
			return -EOVERFLOW;

		/* [한국어] 사용자 비트맵 버퍼가 접근 가능한 주소 범위인지 미리 확인한다. 실제 복사는 락 안에서 일어난다. */
		if (!access_ok((void __user *)range.bitmap.data,
			       range.bitmap.size))
			return -EINVAL;

		/* [한국어] 사용자가 지정한 비트맵 페이지 크기의 시프트 값. */
		pgshift = __ffs(range.bitmap.pgsize);
		/* [한국어] 필요한 비트 수에 비해 버퍼가 충분히 큰지 검증한다. */
		ret = verify_bitmap_size(size >> pgshift,
					 range.bitmap.size);
		/* [한국어] 버퍼 크기 검증 실패. */
		if (ret)
			return ret;

		/* [한국어] 여기서부터는 컨테이너 상태를 읽으므로 락이 필요하다. */
		mutex_lock(&iommu->lock);

		/* [한국어] 컨테이너가 강제하는 최소 IOMMU 페이지 크기. */
		iommu_pgsize = (size_t)1 << __ffs(iommu->pgsize_bitmap);

		/* allow only smallest supported pgsize */
		/* [한국어] 비트맵 페이지 크기는 컨테이너의 최소 지원 크기와 정확히 같아야 한다 — 그래야 비트와 페이지가 1:1 로 대응한다. */
		if (range.bitmap.pgsize != iommu_pgsize) {
			/* [한국어] 사용자가 지정한 비트맵 페이지 크기가 커널이 강제하는 값과 다르다. */
			ret = -EINVAL;
			/* [한국어] 락을 놓고 나가기 위해 공통 출구로 점프한다. */
			goto out_unlock;
		}
		/* [한국어] 시작 주소가 그 크기에 정렬되어 있어야 비트 경계와 맞는다. */
		if (iova & (iommu_pgsize - 1)) {
			ret = -EINVAL;
			/* [한국어] 시작 주소 정렬 위반. */
			goto out_unlock;
		}
		/* [한국어] 길이도 마찬가지로 정렬되어 있어야 한다. */
		if (size & (iommu_pgsize - 1)) {
			ret = -EINVAL;
			/* [한국어] 길이 정렬 위반. */
			goto out_unlock;
		}

		/* [한국어] 추적이 켜져 있을 때만 보고할 것이 있다. */
		if (iommu->dirty_page_tracking)
			/* [한국어] 실제 보고와 비트맵 리셋을 한 번에 수행한다. */
			ret = vfio_iova_dirty_bitmap(range.bitmap.data,
						     iommu, iova, iova_end,
						     range.bitmap.pgsize);
		/* [한국어] 추적이 꺼져 있는 경우. */
		else
			/* [한국어] 추적이 꺼져 있으면 보고할 비트맵 자체가 없다. */
			ret = -EINVAL;
out_unlock:
		/* [한국어] 락을 놓는다. 위 세 정렬 검사와 보고가 모두 이 락 아래에 있었다. */
		mutex_unlock(&iommu->lock);

		return ret;
	}

	/* [한국어] 세 플래그 중 어느 것도 아니면 처리할 동작이 없다. */
	return -EINVAL;
}

/* [한국어]
 * vfio_iommu_type1_ioctl - 이 백엔드의 ioctl 디스패처
 *
 * @iommu_data: 컨테이너 객체. VFIO_CHECK_EXTENSION 은 NULL 로도 불릴 수 있다.
 * @cmd: ioctl 명령 번호.
 * @arg: 사용자 공간 인자 주소.
 * @return: 각 처리 함수의 반환값, 모르는 명령이면 -ENOTTY.
 *
 * vfio_iommu_driver_ops 의 ioctl 콜백이다. container.c 가 컨테이너 fd 의 ioctl 중
 * IOMMU 백엔드가 다룰 것들을 이리로 넘긴다. 이 파일이 사용자 공간에 노출하는 API
 * 전체가 이 switch 문 다섯 줄로 요약된다.
 *
 * 명령별 의미:
 *  - VFIO_CHECK_EXTENSION: 기능 질의.
 *  - VFIO_IOMMU_GET_INFO: 페이지 크기와 유효 IOVA 범위 보고.
 *  - VFIO_IOMMU_MAP_DMA: 버퍼를 핀하고 IOMMU 에 매핑 — 이 파일의 핵심 진입점이다.
 *  - VFIO_IOMMU_UNMAP_DMA: 매핑 해제와 언핀.
 *  - VFIO_IOMMU_DIRTY_PAGES: 마이그레이션용 dirty 추적 제어.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락은 각 처리 함수가 필요에 따라 잡는다.
 *
 * caller: drivers/vfio/container.c:216, :225, :297, :353 에서 ops->ioctl 로 불린다.
 *   :216 과 :297 은 컨테이너가 만들어지기 전 백엔드 후보를 고르는 질의라
 *   iommu_data 가 NULL 이다.
 * callee: 위 다섯 처리 함수.
 * 에러 경로: -ENOTTY 는 '이 백엔드가 모르는 명령' 이라는 뜻이며, container.c 가
 *   다른 처리 경로로 넘어가는 신호로도 쓰인다.
 *
 * 호출 체인:
 *   container.c:353 ops->ioctl → [vfio_iommu_type1_ioctl] →
 *   vfio_iommu_type1_map_dma()
 */
static long vfio_iommu_type1_ioctl(void *iommu_data,
				   unsigned int cmd, unsigned long arg)
{
	struct vfio_iommu *iommu = iommu_data;

	/* [한국어] 명령 번호로 처리 함수를 고른다. 이 다섯 가지가 이 파일이 사용자 공간에 노출하는 API 전부다. */
	switch (cmd) {
	/* [한국어] 기능 질의. iommu 가 NULL 로 올 수 있는 유일한 명령이다. */
	case VFIO_CHECK_EXTENSION:
		return vfio_iommu_type1_check_extension(iommu, arg);
	/* [한국어] 페이지 크기 비트맵과 유효 IOVA 범위 보고. */
	case VFIO_IOMMU_GET_INFO:
		return vfio_iommu_type1_get_info(iommu, arg);
	/* [한국어] 사용자 버퍼를 핀하고 IOMMU 에 매핑 — 이 파일의 핵심 진입점. */
	case VFIO_IOMMU_MAP_DMA:
		return vfio_iommu_type1_map_dma(iommu, arg);
	/* [한국어] 매핑 해제와 언핀. */
	case VFIO_IOMMU_UNMAP_DMA:
		return vfio_iommu_type1_unmap_dma(iommu, arg);
	/* [한국어] 마이그레이션용 dirty page 추적 제어. */
	case VFIO_IOMMU_DIRTY_PAGES:
		return vfio_iommu_type1_dirty_pages(iommu, arg);
	default:
		/* [한국어] 이 백엔드가 모르는 명령이라는 뜻이다. container.c 가 다른 처리 경로를 찾는 신호로도 쓰인다. */
		return -ENOTTY;
	}
}

/* [한국어]
 * vfio_iommu_type1_register_device - 언맵 통지를 받을 장치를 컨테이너에 등록한다
 *
 * @iommu_data: 컨테이너 객체.
 * @vdev: 등록할 vfio_device.
 * @return: 없음.
 *
 * vfio_iommu_driver_ops 의 register_device 콜백이다. dma_unmap 콜백을 가진 장치만
 * 등록한다 — 통지를 받을 수 없는 장치는 목록에 있어 봐야 의미가 없기 때문이다.
 *
 * 락 두 개를 모두 잡는 이유는 상류 주석이 설명한다: list_empty() 검사는
 * iommu->lock 아래에서 하고 목록 순회는 device_list_lock 아래에서 하는데, 등록/해제가
 * 두 락을 모두 잡아 주면 두 검사가 서로 모순되는 순간이 없다. 덕분에
 * vfio_notify_dma_unmap() 과 vfio_iommu_type1_pin_pages() 의 빠른 경로에서
 * device_list_lock 을 생략할 수 있다.
 *
 * 동작 단계: dma_unmap 이 없으면 반환. 있으면 iommu->lock -> device_list_lock 순으로
 * 잡고 목록에 넣은 뒤 역순으로 놓는다. 락 순서는 vfio_notify_dma_unmap() 과 같다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 장치가 컨테이너에 열릴 때 불린다.
 *
 * caller: drivers/vfio/container.c:170 의 vfio_device_container_register().
 * callee: mutex_lock(), list_add(), mutex_unlock().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_device_container_register() → [vfio_iommu_type1_register_device]
 */
static void vfio_iommu_type1_register_device(void *iommu_data,
					     struct vfio_device *vdev)
{
	struct vfio_iommu *iommu = iommu_data;

	/* [한국어] dma_unmap 콜백이 없는 장치는 언맵 통지를 받을 수 없으므로 목록에 넣지 않는다. 등록 때와 같은 조건이어야 list_del 이 안전하다. */
	if (!vdev->ops->dma_unmap)
		return;

	/*
	 * list_empty(&iommu->device_list) is tested under the iommu->lock while
	 * iteration for dma_unmap must be done under the device_list_lock.
	 * Holding both locks here allows avoiding the device_list_lock in
	 * several fast paths. See vfio_notify_dma_unmap()
	 */
	/* [한국어] 두 락을 모두 잡는 이유는 상류 주석이 설명한다 — list_empty 검사는 이 락 아래에서,
	 * 순회는 device_list_lock 아래에서 하므로 등록/해제가 양쪽을 모두 잡아야
	 * 두 검사가 모순되지 않는다. */
	mutex_lock(&iommu->lock);
	/* [한국어] 목록 자체를 보호하는 락. 통지 순회는 이 락만으로 돈다. */
	mutex_lock(&iommu->device_list_lock);
	/* [한국어] 목록에 넣는다. 두 락을 모두 쥔 상태이므로 list_empty 검사와 순회 어느 쪽에서 보아도 일관된다. */
	list_add(&vdev->iommu_entry, &iommu->device_list);
	/* [한국어] 목록 락을 먼저 놓는다. */
	mutex_unlock(&iommu->device_list_lock);
	/* [한국어] 바깥 락을 놓는다. */
	mutex_unlock(&iommu->lock);
}

/* [한국어]
 * vfio_iommu_type1_unregister_device - 장치를 컨테이너의 통지 목록에서 뺀다
 *
 * @iommu_data: 컨테이너 객체.
 * @vdev: 뺄 vfio_device.
 * @return: 없음.
 *
 * register 의 짝이다. 같은 조건(dma_unmap 콜백 보유)과 같은 락 순서를 쓴다.
 * 목록에 넣지 않은 장치를 빼려 하면 list_del 이 잘못된 포인터를 만지므로,
 * 등록 때와 정확히 같은 조건으로 걸러야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 장치가 컨테이너에서 닫힐 때 불린다.
 *
 * caller: drivers/vfio/container.c:180 의 vfio_device_container_unregister().
 * callee: mutex_lock(), list_del(), mutex_unlock().
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_device_container_unregister() → [vfio_iommu_type1_unregister_device]
 */
static void vfio_iommu_type1_unregister_device(void *iommu_data,
					       struct vfio_device *vdev)
{
	struct vfio_iommu *iommu = iommu_data;

	/* [한국어] 등록 때와 같은 조건. 넣지 않은 노드를 빼면 목록이 깨진다. */
	if (!vdev->ops->dma_unmap)
		return;

	/* [한국어] 등록 때와 같은 순서로 두 락을 잡는다 — 순서가 다르면 데드락이 난다. */
	mutex_lock(&iommu->lock);
	/* [한국어] 목록 자체를 보호하는 락. */
	mutex_lock(&iommu->device_list_lock);
	/* [한국어] 목록에서 뺀다. */
	list_del(&vdev->iommu_entry);
	/* [한국어] 목록 락을 먼저 놓는다. */
	mutex_unlock(&iommu->device_list_lock);
	/* [한국어] 바깥 락을 놓는다. 잡을 때의 역순이다. */
	mutex_unlock(&iommu->lock);
}

/* [한국어]
 * vfio_iommu_type1_dma_rw_chunk - 매핑 하나 안에서 IOVA 를 사용자 버퍼로 읽고 쓴다
 *
 * @iommu: 컨테이너 객체.
 * @user_iova: 접근할 IOVA.
 * @data: 커널 쪽 버퍼.
 * @count: 요청 바이트 수.
 * @write: true 면 커널 -> 사용자 방향(장치 메모리에 쓰기).
 * @copied: 실제로 옮긴 바이트 수를 받을 곳.
 * @return: 0 성공, -EINVAL(매핑 없음), -EPERM(권한/문맥 불일치), -EFAULT(복사 실패).
 *
 * 왜 필요한가: vendor 드라이버가 마이그레이션 상태를 저장·복원하거나 게스트가
 * 쓴 명령을 해석할 때, 게스트 IOVA 로 표현된 메모리를 커널이 직접 읽고 써야 한다.
 * 그런데 그 메모리는 다른 프로세스의 사용자 공간에 있으므로 그 주소 공간을
 * 빌려 써야 한다.
 *
 * 동작 단계:
 *  1. vfio_find_dma() 로 그 IOVA 를 덮는 매핑을 찾는다.
 *  2. 방향에 맞는 권한이 있는지 본다. 쓰기면 IOMMU_WRITE 가 있어야 하고,
 *     어느 방향이든 IOMMU_READ 는 있어야 한다.
 *  3. mmget_not_zero() 로 소유 프로세스의 주소 공간을 승격한다. 죽었으면 -EPERM.
 *  4. 커널 스레드면(current->mm 이 NULL) kthread_use_mm() 으로 그 주소 공간을 빌린다.
 *     사용자 스레드인데 자기 mm 이 아니면 접근할 수 없으므로 out 으로 빠진다.
 *  5. 매핑 시작부터의 오프셋을 구하고, 요청량이 매핑 끝을 넘으면 잘라낸다. 그래서
 *     호출자는 count 가 여러 매핑에 걸치면 여러 번 불러야 한다.
 *  6. 사용자 가상 주소를 dma->vaddr + offset 으로 계산해 복사한다. copy_to_user()/
 *     copy_from_user() 는 실패 시 '못 옮긴 바이트 수' 를 돌려주므로, 0 이 아니면
 *     copied 를 0 으로 두어 부분 성공을 실패로 처리한다.
 *  7. 쓰기였고 dirty 추적 중이면 건드린 범위의 비트를 세운다. 비트 개수는
 *     끝 페이지 번호에서 시작 페이지 번호를 빼고 1 을 더해 구한다 — 시작과 끝이
 *     같은 페이지여도 1 이 되도록 하는 표준 계산이다.
 *  8. 빌린 mm 을 kthread_unuse_mm() 으로 돌려주고, out 에서 mmput() 한다.
 *  9. 한 바이트도 못 옮겼으면 -EFAULT.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 커널 스레드 문맥. 호출자가 iommu->lock 을 쥔
 * 상태다. 사용자 메모리 접근으로 잠들 수 있다.
 *
 * caller: vfio_iommu_type1_dma_rw() 한 곳뿐.
 * callee: vfio_find_dma(), mmget_not_zero(), kthread_use_mm(), copy_to_user(),
 *   copy_from_user(), bitmap_set(), kthread_unuse_mm(), mmput().
 * 에러 경로: 4 번의 out 점프는 kthread_unuse_mm() 을 건너뛰는데, 그 경로는
 *   kthread 가 아니어서 애초에 빌리지 않았으므로 짝이 맞는다.
 *
 * 호출 체인:
 *   vfio_dma_rw() → vfio_device_container_dma_rw() → vfio_iommu_type1_dma_rw() →
 *   [vfio_iommu_type1_dma_rw_chunk] → copy_to_user()
 */
static int vfio_iommu_type1_dma_rw_chunk(struct vfio_iommu *iommu,
					 dma_addr_t user_iova, void *data,
					 size_t count, bool write,
					 size_t *copied)
{
	struct mm_struct *mm;
	/* [한국어] 복사 대상이 될 사용자 공간 가상 주소. */
	unsigned long vaddr;
	/* [한국어] 이 IOVA 를 덮는 매핑. */
	struct vfio_dma *dma;
	/* [한국어] 현재 문맥이 커널 스레드인가. current->mm 이 NULL 이면 자기 주소 공간이
	 * 없다는 뜻이므로 남의 mm 을 빌려 써야 한다. */
	bool kthread = current->mm == NULL;
	/* [한국어] 매핑 시작으로부터의 바이트 오프셋. */
	size_t offset;

	*copied = 0;

	/* [한국어] 이 IOVA 한 바이트를 덮는 매핑을 찾는다. 크기 1 은 '이 주소를 포함하는가' 를 묻는 관용구다. */
	dma = vfio_find_dma(iommu, user_iova, 1);
	/* [한국어] 매핑이 없으면 번역할 수 없는 주소다. */
	if (!dma)
		return -EINVAL;

	/* [한국어] 방향에 맞는 권한 검사. 쓰기면 IOMMU_WRITE 가 있어야 하고,
	 * 어느 방향이든 IOMMU_READ 는 있어야 한다 — 읽기 권한도 없는 매핑을
	 * 커널이 대신 읽어 주면 사용자가 정한 접근 정책을 우회하는 셈이 된다. */
	if ((write && !(dma->prot & IOMMU_WRITE)) ||
			!(dma->prot & IOMMU_READ))
		return -EPERM;

	/* [한국어] 이 매핑의 소유 주소 공간. */
	mm = dma->mm;
	/* [한국어] mmgrab 으로 붙잡아 둔 구조체를 실제 사용 가능 상태로 승격한다. 실패하면 프로세스가 이미 끝난 것이다. */
	if (!mmget_not_zero(mm))
		return -EPERM;

	/* [한국어] 커널 스레드면 그 주소 공간을 빌려 쓴다. 이후 copy_to_user 계열이 이 mm 을 대상으로 동작한다. */
	if (kthread)
		kthread_use_mm(mm);
	/* [한국어] 사용자 스레드인데 자기 mm 이 아니면 접근할 수 없다. out 으로 빠져 -EFAULT 가 된다. */
	else if (current->mm != mm)
		goto out;

	/* [한국어] 매핑 시작으로부터의 오프셋. IOVA 오프셋과 vaddr 오프셋이 같다는 성질을 이용한다. */
	offset = user_iova - dma->iova;

	/* [한국어] 요청이 매핑 끝을 넘으면 잘라낸다. */
	if (count > dma->size - offset)
		/* [한국어] 이번에 옮길 수 있는 최대치로 줄인다. 남은 부분은 호출자가 다음 매핑에서 처리한다. */
		count = dma->size - offset;

	/* [한국어] 실제 복사 대상 사용자 가상 주소. */
	vaddr = dma->vaddr + offset;

	/* [한국어] 쓰기 방향 — 커널 버퍼의 내용을 사용자(게스트) 메모리에 쓴다. */
	if (write) {
		*copied = copy_to_user((void __user *)vaddr, data,
					 count) ? 0 : count;
		/* [한국어] 실제로 옮겼고 dirty 추적 중이면, 방금 쓴 범위의 비트를 세워야 한다.
		 * 이 경로는 IOMMU 를 거치지 않는 커널의 직접 쓰기라 하드웨어가 알려 주지 않는다. */
		if (*copied && iommu->dirty_page_tracking) {
			unsigned long pgshift = __ffs(iommu->pgsize_bitmap);
			/*
			 * Bitmap populated with the smallest supported page
			 * size
			 */
			/* [한국어] 쓴 범위의 비트를 세운다. */
			bitmap_set(dma->bitmap, offset >> pgshift,
				   /* [한국어] 끝 페이지 번호에서 */
				   ((offset + *copied - 1) >> pgshift) -
				   /* [한국어] 시작 페이지 번호를 빼고 1 을 더해 비트 개수를 구한다. 같은 페이지 안이어도 1 이 되도록 하는 계산이다. */
				   (offset >> pgshift) + 1);
		}
	/* [한국어] 읽기 방향 — 사용자 버퍼에서 커널 버퍼로 옮긴다. */
	} else
		*copied = copy_from_user(data, (void __user *)vaddr,
					   count) ? 0 : count;
	/* [한국어] 빌렸던 주소 공간을 돌려준다. 빌리지 않았으면 건드리지 않는다. */
	if (kthread)
		kthread_unuse_mm(mm);
out:
	/* [한국어] 승격했던 주소 공간 참조를 놓는다. 위 mmget_not_zero 의 짝이다. */
	mmput(mm);
	/* [한국어] 한 바이트도 못 옮겼으면 실패다. 부분 성공을 인정하지 않는 것이 이 API 의 규약이다. */
	return *copied ? 0 : -EFAULT;
}

/* [한국어]
 * vfio_iommu_type1_dma_rw - IOVA 로 지정된 게스트 메모리를 커널이 읽고 쓴다
 *
 * @iommu_data: 컨테이너 객체.
 * @user_iova: 시작 IOVA.
 * @data: 커널 버퍼.
 * @count: 바이트 수.
 * @write: 방향.
 * @return: 0 성공, -EBUSY(vaddr 무효화 중), 그 밖에 청크 함수의 오류.
 *
 * vfio_iommu_driver_ops 의 dma_rw 콜백이다. 요청이 여러 매핑에 걸칠 수 있으므로
 * 매핑 경계마다 잘라 반복 호출한다.
 *
 * 동작 단계:
 *  1. 락을 잡는다.
 *  2. vaddr 무효화가 진행 중이면 -EBUSY — 무효 vaddr 로는 사용자 메모리에 접근할 수
 *     없다. WARN_ONCE 로 규약을 어긴 호출자를 한 번만 알린다.
 *  3. 남은 양이 있는 동안 vfio_iommu_type1_dma_rw_chunk() 를 부르고, 옮긴 만큼
 *     카운터/포인터/IOVA 를 전진시킨다. 청크 함수가 매핑 경계에서 잘라 주므로
 *     이 루프가 자연히 매핑을 넘나든다.
 *  4. 오류가 나면 즉시 중단한다. 이때까지 옮긴 분량은 호출자에게 알리지 않는다 —
 *     이 API 의 반환은 전부 성공/실패뿐이다.
 *  5. 락을 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥 또는 커널 스레드 문맥. 이 함수가 락을 직접 잡는다.
 * 잠들 수 있다.
 *
 * caller: drivers/vfio/container.c:569 의 vfio_device_container_dma_rw().
 *   그 위는 vfio_main.c 의 vfio_dma_rw().
 * callee: vfio_iommu_type1_dma_rw_chunk().
 * 에러 경로: out 라벨에서 락을 놓고 오류를 반환한다.
 *
 * 호출 체인:
 *   vfio_dma_rw() → vfio_device_container_dma_rw() → [vfio_iommu_type1_dma_rw] →
 *   vfio_iommu_type1_dma_rw_chunk()
 */
static int vfio_iommu_type1_dma_rw(void *iommu_data, dma_addr_t user_iova,
				   void *data, size_t count, bool write)
{
	struct vfio_iommu *iommu = iommu_data;
	/* [한국어] 누적 오류 코드. 한 청크라도 실패하면 여기 담겨 반환된다. */
	int ret = 0;
	/* [한국어] 이번 청크에서 실제로 옮긴 바이트 수를 받을 곳. */
	size_t done;

	/* [한국어] 매핑 트리를 보호한다. 청크 함수는 이 락을 쥔 상태로 불린다. */
	mutex_lock(&iommu->lock);

	/* [한국어] vaddr 무효화가 진행 중이면 사용자 주소가 유효하지 않아 접근할 수 없다.
	 * WARN_ONCE 로 규약을 어긴 호출자를 한 번만 알리고 -EBUSY 로 거절한다. */
	if (WARN_ONCE(iommu->vaddr_invalid_count,
		      "vfio_dma_rw not allowed with VFIO_UPDATE_VADDR\n")) {
		ret = -EBUSY;
		/* [한국어] 무효 상태에서는 사용자 메모리에 접근할 수 없으므로 out 으로 빠져 락을 놓는다. */
		goto out;
	}

	/* [한국어] 요청량이 남아 있는 동안 매핑 경계마다 잘라 반복한다. */
	while (count > 0) {
		/* [한국어] 한 매핑 안에서 가능한 만큼 옮긴다. 청크 함수가 매핑 끝에서 잘라 준다. */
		ret = vfio_iommu_type1_dma_rw_chunk(iommu, user_iova, data,
						    count, write, &done);
		/* [한국어] 청크가 실패하면 즉시 중단한다. 이미 옮긴 분량은 호출자에게 알리지 않는다. */
		if (ret)
			break;

		/* [한국어] 옮긴 만큼 남은 요청량을 줄인다. */
		count -= done;
		/* [한국어] 커널 버퍼 포인터를 옮긴 만큼 전진시킨다. */
		data += done;
		/* [한국어] IOVA 도 같은 만큼 전진시켜 다음 매핑으로 넘어간다. */
		user_iova += done;
	}

out:
	/* [한국어] 락을 놓는다. */
	mutex_unlock(&iommu->lock);
	return ret;
}

/* [한국어]
 * vfio_iommu_type1_group_iommu_domain - group 이 붙어 있는 IOMMU domain 을 돌려준다
 *
 * @iommu_data: 컨테이너 객체.
 * @iommu_group: 조회할 group.
 * @return: 해당 iommu_domain 포인터, 못 찾으면 ERR_PTR(-ENODEV),
 *          인자가 잘못되면 ERR_PTR(-EINVAL).
 *
 * vfio_iommu_driver_ops 의 group_iommu_domain 콜백이다. vfio.h:446 의 주석대로
 * 바깥 계층이 domain 을 직접 참조해야 할 때 쓰는 통로다. 이 트리에서는 이 콜백을
 * 부르는 코드를 찾지 못했다 — 호출자는 sparse checkout 에 포함되지 않은 쪽에 있다.
 *
 * 동작 단계:
 *  1. 인자가 NULL 이면 -EINVAL 을 ERR_PTR 로 감싸 반환한다.
 *  2. 락을 잡고 모든 domain 을 훑으며 find_iommu_group() 으로 이 group 을 가진
 *     domain 을 찾는다.
 *  3. 찾으면 그 domain 포인터를, 못 찾으면 초기값인 ERR_PTR(-ENODEV) 를 반환한다.
 * 반환된 포인터의 수명은 컨테이너가 유지되는 동안만 보장된다 — 락을 놓은 뒤에는
 * detach 로 사라질 수 있으므로 호출자가 별도 보장을 마련해야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 함수가 직접 락을 잡고 놓는다.
 *
 * caller: drivers/vfio/vfio.h:446 에 선언된 ops->group_iommu_domain 을 통해서만
 *   불린다. 이 트리 안에는 호출 지점이 없다.
 * callee: find_iommu_group().
 * 에러 경로: ERR_PTR 로 오류를 표현하므로 호출자는 IS_ERR 로 검사해야 한다.
 *
 * 호출 체인:
 *   ops->group_iommu_domain → [vfio_iommu_type1_group_iommu_domain] →
 *   find_iommu_group()
 */
static struct iommu_domain *
vfio_iommu_type1_group_iommu_domain(void *iommu_data,
				    struct iommu_group *iommu_group)
{
	struct iommu_domain *domain = ERR_PTR(-ENODEV);
	/* [한국어] void 인자를 컨테이너 객체로 되돌린다. */
	struct vfio_iommu *iommu = iommu_data;
	/* [한국어] domain 목록 순회용 커서. */
	struct vfio_domain *d;

	/* [한국어] 인자가 하나라도 NULL 이면 조회할 수 없다. 오류를 포인터로 표현하므로 ERR_PTR 로 감싼다. */
	if (!iommu || !iommu_group)
		return ERR_PTR(-EINVAL);

	/* [한국어] 컨테이너 상태와 매핑 트리를 보호한다. 아래 청크 루프 전체가 이 락 아래에 있다. */
	mutex_lock(&iommu->lock);
	/* [한국어] 컨테이너의 모든 domain 을 훑는다. */
	list_for_each_entry(d, &iommu->domain_list, next) {
		/* [한국어] 이 domain 의 group 목록에 찾는 group 이 있는지 본다. */
		if (find_iommu_group(d, iommu_group)) {
			/* [한국어] 찾았으면 그 domain 의 실제 iommu_domain 포인터를 결과로 삼는다. */
			domain = d->domain;
			break;
		}
	}
	/* [한국어] 락을 놓는다. 이 지점 이후 컨테이너 상태는 다른 스레드가 바꿀 수 있다. */
	mutex_unlock(&iommu->lock);

	/* [한국어] 찾았으면 domain 포인터, 못 찾았으면 초기값 ERR_PTR(-ENODEV) 를 반환한다. */
	return domain;
}

/* [한국어] 이 파일이 VFIO 코어에 넘기는 vtable. 이 구조체 하나가 곧 type1 백엔드의
 * 공개 인터페이스 전부다. 정의는 drivers/vfio/vfio.h:400 부근에 있고,
 * container.c 가 각 콜백을 ops-> 로 간접 호출한다. */
static const struct vfio_iommu_driver_ops vfio_iommu_driver_ops_type1 = {
	/* [한국어] 백엔드 이름 — 진단 로그와 식별용. */
	.name			= "vfio-iommu-type1",
	/* [한국어] 이 vtable 을 소유한 모듈. 컨테이너가 살아 있는 동안 모듈 제거를 막는 참조로 쓰인다. */
	.owner			= THIS_MODULE,
	/* [한국어] SET_IOMMU 시 백엔드 인스턴스 생성 — container.c:302 에서 불린다. */
	.open			= vfio_iommu_type1_open,
	/* [한국어] 컨테이너 해제 시 정리 — container.c:311 과 :492. */
	.release		= vfio_iommu_type1_release,
	/* [한국어] IOMMU 계열 ioctl 디스패처 — container.c:216, :225, :297, :353. */
	.ioctl			= vfio_iommu_type1_ioctl,
	/* [한국어] group 을 컨테이너에 붙이기 — container.c:243 과 :444. */
	.attach_group		= vfio_iommu_type1_attach_group,
	/* [한국어] group 을 떼기 — container.c:254 와 :480. */
	.detach_group		= vfio_iommu_type1_detach_group,
	/* [한국어] mdev 용 페이지 핀 — container.c:544. */
	.pin_pages		= vfio_iommu_type1_pin_pages,
	/* [한국어] 그 짝 — container.c:556. */
	.unpin_pages		= vfio_iommu_type1_unpin_pages,
	/* [한국어] 언맵 통지 대상 장치 등록 — container.c:170. */
	.register_device	= vfio_iommu_type1_register_device,
	/* [한국어] 그 짝 — container.c:180. */
	.unregister_device	= vfio_iommu_type1_unregister_device,
	/* [한국어] 게스트 IOVA 를 커널이 직접 읽고 쓰기 — container.c:569. */
	.dma_rw			= vfio_iommu_type1_dma_rw,
	/* [한국어] group 이 붙은 IOMMU domain 조회. 이 트리 안에는 호출 지점이 없다. */
	.group_iommu_domain	= vfio_iommu_type1_group_iommu_domain,
};

/* [한국어]
 * vfio_iommu_type1_init - 모듈 적재 시 type1 백엔드를 VFIO 코어에 등록한다
 *
 * @return: 0 성공, 음수면 등록 실패.
 *
 * 이 파일 전체가 하나의 vtable(vfio_iommu_driver_ops_type1)로 요약되고, 이 함수가
 * 그것을 코어에 넘긴다. 등록되면 사용자가 ioctl(VFIO_SET_IOMMU, VFIO_TYPE1v2_IOMMU)
 * 를 했을 때 container.c 가 이 vtable 을 골라 open 콜백을 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 모듈 적재 경로. module_init 으로 등록된다.
 *
 * caller: 커널의 모듈 초기화 기구(module_init).
 * callee: vfio_register_iommu_driver()(drivers/vfio/container.c:91 에 구현).
 * 에러 경로: 실패하면 모듈 적재 자체가 실패한다.
 *
 * 호출 체인:
 *   module_init → [vfio_iommu_type1_init] → vfio_register_iommu_driver()
 */
static int __init vfio_iommu_type1_init(void)
{
	/* [한국어] 이 파일의 vtable 을 VFIO 코어에 등록한다(container.c:91). 실패하면 모듈 적재가 실패한다. */
	return vfio_register_iommu_driver(&vfio_iommu_driver_ops_type1);
}

/* [한국어]
 * vfio_iommu_type1_cleanup - 모듈 제거 시 백엔드 등록을 취소한다
 *
 * @return: 없음.
 *
 * init 의 짝이다. 등록을 취소하면 이후 SET_IOMMU 요청은 이 백엔드를 고를 수 없다.
 * 이미 열려 있는 컨테이너는 모듈 참조(ops->owner = THIS_MODULE)가 잡혀 있어
 * 모듈 제거 자체가 막히므로, 이 시점에는 살아 있는 컨테이너가 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, 모듈 제거 경로. module_exit 으로 등록된다.
 *
 * caller: 커널의 모듈 종료 기구(module_exit).
 * callee: vfio_unregister_iommu_driver()(drivers/vfio/container.c:123 에 구현).
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   module_exit → [vfio_iommu_type1_cleanup] → vfio_unregister_iommu_driver()
 */
static void __exit vfio_iommu_type1_cleanup(void)
{
	/* [한국어] 등록을 취소한다. 이후 SET_IOMMU 는 이 백엔드를 고를 수 없다. */
	vfio_unregister_iommu_driver(&vfio_iommu_driver_ops_type1);
}

/* [한국어] 모듈 적재 시 위 init 함수를 부르도록 등록한다. */
module_init(vfio_iommu_type1_init);
/* [한국어] 모듈 제거 시 위 cleanup 함수를 부르도록 등록한다. */
module_exit(vfio_iommu_type1_cleanup);

/* [한국어] modinfo 의 version 필드 — 위에서 정의한 DRIVER_VERSION 문자열. */
MODULE_VERSION(DRIVER_VERSION);
/* [한국어] 라이선스 선언. GPL 전용 커널 심볼(EXPORT_SYMBOL_GPL)을 쓰려면 필수다. */
MODULE_LICENSE("GPL v2");
/* [한국어] modinfo 의 author 필드. */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] modinfo 의 description 필드. */
MODULE_DESCRIPTION(DRIVER_DESC);
