// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO PCI config space virtualization
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */

/*
 * This code handles reading and writing of PCI configuration registers.
 * This is hairy because we want to allow a lot of flexibility to the
 * user driver, but cannot trust it with all of the config fields.
 * Tables determine which fields can be read and written, as well as
 * which fields are 'virtualized' - special actions and translations to
 * make it appear to the user that he has control, when in fact things
 * must be negotiated with the underlying OS.
 */

/* [한국어] VFIO PCI config 공간 중재 계층 — 사용자 공간 드라이버가 PCI 함수의 설정
 * 레지스터에 무엇을 할 수 있는지를 바이트 단위로 판정하는 보안 경계
 * (drivers/vfio/pci/vfio_pci_config.c)
 * 
 * 이 블록에서 다른 파일을 가리킬 때만 줄 번호를 적었다. 이 파일 자신은 주석이
 * 붙으며 줄 번호가 계속 밀리므로 함수 이름으로만 가리킨다. 같은 디렉터리의
 * vfio_pci_core.c 도 지금 함께 주석 작업 중이라 함수 이름으로만 가리킨다.
 * 
 * === 파일의 역할 ===
 * 이 파일 하나가 "사용자 공간 드라이버가 PCI 함수의 config space 에 무엇을 할 수
 * 있는가" 를 전부 결정한다. VFIO 는 디바이스를 통째로 사용자 공간에 넘기지만
 * config space 만은 그대로 넘길 수 없다. BAR 를 다시 쓰면 디바이스의 MMIO 창을
 * 호스트가 모르는 물리 주소로 옮길 수 있고, MSI/MSI-X 의 메시지 주소를 바꾸면
 * IOMMU 를 우회해 호스트 인터럽트를 위조할 수 있으며, Bus Master 비트 하나로
 * DMA 격리의 전제를 흔들 수 있다. 그래서 이 파일은 config space 를 바이트 단위로
 * 세 갈래로 나눈다 — 하드웨어로 그냥 통과시킬 바이트, 커널이 들고 있는 그림자
 * 복사본(vconfig)에서 지어내 보여 줄 바이트, 그리고 쓰기를 조용히 버릴 바이트다.
 * 세 갈래의 정확한 정의는 아래 "통과 / 에뮬레이션 / 거부 3분류" 절에 있다.
 * 
 * 판정의 근거는 두 벌의 비트맵이다. struct perm_bits 의 virt 는 "이 비트는
 * 하드웨어가 아니라 vconfig 에서 읽고 쓴다", write 는 "이 비트는 사용자가 쓸 수
 * 있다" 를 비트 하나 단위로 담는다. 이 두 비트맵은 capability 종류마다 한 벌씩
 * 있고(cap_perms 와 ecap_perms), 시스템의 모든 vfio-pci 디바이스가 그 한 벌을
 * 공유한다 — 그래서 init_pci_cap_ 계열 초기화 함수들이 __init 으로 표시돼 모듈
 * 적재 시 한 번만 돈다. 디바이스마다 달라지는 것은 MSI capability 뿐이라
 * (64비트 주소 지원 여부와 per-vector masking 지원 여부에 따라 길이와 필드
 * 배치가 달라진다) msi_perm 만 vdev 별로 따로 만든다.
 * 
 * 세 번째 자료구조가 pci_config_map 이다. 디바이스마다 pdev->cfg_size 바이트짜리
 * 배열을 두고 각 바이트에 "이 오프셋은 어느 capability 소속인가" 를 적어 둔다.
 * 접근이 들어오면 이 지도에서 capability ID 를 꺼내 해당 perm_bits 를 고르고,
 * vfio_find_cap_start 로 그 capability 의 시작점을 되짚어 capability 안에서의
 * 오프셋을 계산한 뒤 readfn/writefn 을 부른다. 지도에 아무 capability 도 없는
 * 빈틈은 PCI_CAP_ID_INVALID(0xFF) 로 남겨 raw 통과시키고, 커널이 감춘 확장
 * capability 자리는 PCI_CAP_ID_INVALID_VIRT(0xFE) 로 표시해 전부 에뮬레이션한다.
 * 두 상수는 drivers/vfio/pci/vfio_pci_priv.h 에 정의돼 있다.
 * 
 * 끝으로 이 파일은 capability chain 자체를 다시 엮는다. 커널이 사용자에게
 * 보이고 싶지 않은 capability(핫플러그, CompactPCI, 루트 포트 전용 등)는 앞
 * capability 의 next 포인터를 vconfig 안에서 고쳐 통째로 건너뛰게 만든다. 그래서
 * 사용자가 lspci 로 보는 capability 목록은 하드웨어의 목록이 아니라 이 파일이
 * 재작성한 목록이다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간(QEMU, DPDK, SPDK 같은 것들)이 VFIO device fd 에 pread/pwrite 를
 * 하면 그 오프셋의 상위 비트가 "어느 region 인가" 를 가리킨다
 * (include/linux/vfio_pci_core.h:23~26 의 오프셋 인코딩 매크로). region 번호가
 * config region 이면 아래 경로로 이 파일에 도착한다.
 * 
 *   사용자 pread/pwrite(device fd)
 *     -> drivers/vfio/vfio_main.c 의 vfio_device_fops_read / _write
 *        (df->access_granted 게이트를 통과해야 여기까지 온다)
 *     -> vfio_pci_core_read / vfio_pci_core_write  (vfio_pci_core.c)
 *     -> vfio_pci_rw  — 런타임 PM 으로 디바이스를 D0 로 붙잡고 region 분기
 *     -> [이 파일] vfio_pci_config_rw
 *     -> [이 파일] vfio_pci_config_rw_single  — 접근을 정렬된 조각으로 자른다
 *     -> perm->readfn / perm->writefn  — 아래 표가 고른 콜백
 *     -> drivers/pci/access.c:707 과 :769 가 찍어낸 pci_user_read_config_ 계열 /
 *        pci_user_write_config_ 계열 -> pci_lock -> 버스 config 접근자 -> 하드웨어
 * 
 * 반대편에는 초기화 경로가 있다. 모듈 적재 시 vfio_pci_core.c 의
 * vfio_pci_core_init 이 이 파일의 vfio_pci_init_perm_bits 를 불러 공유 표를
 * 만들고, 디바이스가 처음 열릴 때 vfio_pci_core_enable 이 vfio_config_init 를
 * 불러 그 디바이스의 pci_config_map 과 vconfig 를 만든다. 닫힐 때는
 * vfio_pci_core_disable 이 vfio_config_free 를 부른다.
 * 
 * 실행 컨텍스트는 전부 호스트 커널의 프로세스 문맥이다. 이 파일에는 인터럽트
 * 문맥에서 도는 코드가 없다. copy_to_user 와 copy_from_user 로 사용자 페이지를
 * 만지고, pci_user_read_config_ 계열은 config 접근이 차단된 동안 잠들 수 있으며
 * (drivers/pci/access.c:653 의 pci_wait_cfg), memory_lock rw_semaphore 를 쓰기
 * 모드로 잡는 자리도 여럿이다.
 * 
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/pci/vfio_pci_priv.h
 *      : 이 파일이 노출하는 6개 함수(vfio_pci_init_perm_bits,
 *        vfio_pci_uninit_perm_bits, vfio_config_init, vfio_config_free,
 *        vfio_pci_config_rw, vfio_pci_config_rw_single)의 선언처이자,
 *        PCI_CAP_ID_INVALID 와 PCI_CAP_ID_INVALID_VIRT 두 특수 ID 의 정의처.
 *        이 파일이 부르는 vfio_pci_zap_and_down_write_memory_lock,
 *        vfio_pci_set_power_state, vfio_pci_dma_buf_move, vfio_pci_intx_mask,
 *        vfio_pci_intx_unmask 의 선언도 여기에 있다.
 *  - include/linux/vfio_pci_core.h
 *      : struct vfio_pci_core_device 의 정의처(98줄). 이 파일이 쓰는 필드는
 *        pci_config_map(104줄), vconfig(105줄), msi_perm(106줄),
 *        msi_qmax(113줄), rbar(117줄), 그리고 118~128줄의 비트필드 중
 *        extended_caps, bardirty, nointx 다. VFIO_PCI_OFFSET_MASK(26줄)도 여기.
 *  - drivers/pci/access.c  (이미 주석 완료)
 *      : pci_user_read_config_byte/word/dword 와 pci_user_write_config_ 계열의
 *        실제 구현. 707줄과 769줄의 매크로가 찍어내고 796~801줄에서 인스턴스화된다.
 *        일반 pci_read_config_ 계열과 달리 이 판은 pci_cfg_access_lock 으로
 *        차단된 동안 -EBUSY 를 내지 않고 pci_wait_cfg(653줄)에서 잠들어 기다린다.
 *        사용자를 대신하는 접근이므로 실패보다 대기가 옳다는 판단이다.
 *  - drivers/pci/pci.c  (이미 주석 완료)
 *      : pci_try_reset_function(10347줄) — 가상화된 FLR 비트가 켜졌을 때 실제
 *        리셋을 대신 수행. pcie_set_readrq(11719줄)와 pcie_get_mps(11816줄) —
 *        MRRS 쓰기를 하드웨어 MPS 아래로 못 내려가게 막는 데 쓴다.
 *  - drivers/pci/vpd.c  (이미 주석 완료)
 *      : pci_read_vpd(1304줄)와 pci_write_vpd(1416줄). VPD capability 는
 *        주소/데이터 레지스터 쌍으로 된 간접 접근 창인데, 그 프로토콜을 커널이
 *        대신 수행하고 결과만 vconfig 에 채워 준다.
 *  - drivers/pci/pci-driver.c  (이미 주석 완료)
 *      : pci_match_id(347줄) — VF 의 엉터리 INTx pin 을 이미 아는 하드웨어
 *        버그 목록과 대조할 때 쓴다.
 *  - drivers/vfio/pci/vfio_pci_intrs.c  (같은 작업으로 함께 주석)
 *      : vfio_pci_intx_mask 와 vfio_pci_intx_unmask 의 구현처. 사용자가 COMMAND
 *        레지스터의 INTx Disable 비트를 쓰면 이 파일의 vfio_basic_config_write 가
 *        그 두 함수를 불러 실제 인터럽트 라인을 여닫는다. 반대로 그 파일은
 *        vdev->virq_disabled 를 읽어 eventfd 전달 여부를 정한다 — 이 필드가 두
 *        파일이 공유하는 상태다.
 *  - drivers/pci/msi/  (이미 주석 완료)
 *      : 이 파일은 MSI capability 를 "표" 로만 다루고 실제 벡터 할당은 하지
 *        않는다. 사용자가 MSI Enable 비트를 켜도 이 파일은 vconfig 에만 반영하고
 *        (vfio_msi_config_write 의 irq_type 검사), 진짜 활성화는
 *        vfio_pci_intrs.c 가 drivers/pci/msi/api.c:519 의 pci_alloc_irq_vectors 를
 *        부를 때 일어난다. 즉 MSI 는 config 쓰기가 아니라 ioctl 로만 켜진다.
 *  - drivers/vfio/pci/ism/main.c:205, virtio/legacy_io.c, nvgrace-gpu/main.c
 *      : 이 파일이 EXPORT_SYMBOL_GPL 로 내보낸 vfio_pci_config_rw_single 과
 *        vfio_pci_core_range_intersect_range 의 소비자들. 변종 드라이버가 config
 *        공간의 일부만 자기 방식으로 가로챌 때 쓴다.
 * 
 * === 주요 함수/구조체 요약 ===
 *  - struct perm_bits            : 이 파일의 심장. virt/write 두 비트맵과
 *                                  readfn/writefn 두 콜백. capability 하나가
 *                                  한 벌씩 갖는다.
 *  - cap_perms / ecap_perms      : 표준 capability 와 확장 capability 용 표.
 *                                  전 디바이스 공유이며 기본값은 "읽기는
 *                                  하드웨어 직통, 쓰기는 전면 거부" 다.
 *  - unassigned_perms / virt_perms / direct_ro_perms
 *                                : 지도에 capability 가 없는 자리, 감춘
 *                                  capability 자리, 정체 모를 확장 capability
 *                                  자리에 각각 쓰이는 세 개의 특수 표.
 *  - vfio_default_config_read/_write
 *                                : virt 비트맵을 마스크로 삼아 하드웨어 값과
 *                                  vconfig 값을 비트 단위로 섞는 기본 엔진.
 *                                  나머지 readfn/writefn 은 대개 이것을 감싼다.
 *  - alloc_perm_bits / p_setb / p_setw / p_setd
 *                                : 표를 채우는 도구. 기본 상태는 "전부 읽기 가능,
 *                                  아무것도 쓰기 불가, 아무것도 가상화 안 함" 이고
 *                                  p_set 계열이 예외를 하나씩 뚫는다.
 *  - vfio_bar_fixup / vfio_bar_restore / vfio_need_bar_restore
 *                                : BAR 사이징 흉내와, 뒷문 리셋으로 BAR 가
 *                                  날아갔을 때의 복구.
 *  - vfio_cap_init / vfio_ecap_init
 *                                : 하드웨어의 capability chain 을 순회하며
 *                                  pci_config_map 을 칠하고 vconfig 를 채우며,
 *                                  감출 capability 는 next 포인터를 고쳐 건너뛴다.
 *  - vfio_config_init / vfio_config_free
 *                                : 디바이스별 지도와 그림자 복사본의 생성/소멸.
 *  - vfio_pci_config_rw_single   : 접근 한 조각을 처리하는 디스패처. 지도에서
 *                                  표를 고르고 오프셋을 계산해 콜백을 부른다.
 *  - vfio_pci_config_rw          : 사용자 요청 전체를 조각으로 나눠 위 함수를
 *                                  반복 호출하는 바깥 루프.
 * 
 * === 통과 / 에뮬레이션 / 거부 3분류 ===
 * 이 파일이 하는 모든 판정은 결국 아래 세 가지 중 하나다. 어떤 바이트가 어디에
 * 속하는지는 (지도가 고른 perm_bits, virt 비트, write 비트) 세 값의 조합으로
 * 정해진다.
 * 
 *  (1) 통과(pass-through) — virt 비트가 0 인 곳.
 *      읽기: vfio_default_config_read 가 하드웨어에서 실제로 읽어 그대로 준다.
 *      쓰기: write 비트도 1 이면 vfio_default_config_write 가 read-modify-write
 *            로 하드웨어에 그 비트만 반영한다.
 *      안전한 이유: 그 비트가 하드웨어에서 무슨 짓을 하든 디바이스 자기 자신의
 *      동작 범위를 벗어나지 못하기 때문이다. 디바이스는 이미 IOMMU 도메인 안에
 *      갇혀 있으므로, 자기 자신을 망가뜨리는 것은 사용자의 자유다.
 *      대표 예: Cache Line Size, Latency Timer, BIST, PCI-X Command,
 *      AER 의 상태/마스크/심각도 레지스터, Power Budgeting 의 데이터 선택자.
 * 
 *  (2) 에뮬레이션(virtualized) — virt 비트가 1 인 곳.
 *      읽기: vconfig 의 값을 준다. 하드웨어는 쳐다보지도 않는다.
 *      쓰기: write 비트도 1 이면 vconfig 에만 적는다. 하드웨어에는 닿지 않는다.
 *      안전한 이유: 사용자는 자기가 쓴 값을 그대로 되읽으므로 드라이버가
 *      정상 동작한다고 믿지만, 실제 하드웨어 상태는 커널이 결정한다. 필요하면
 *      writefn 이 그 값을 "번역" 해서 커널 API 로 대신 수행한다.
 *      대표 예: BAR 6개와 ROM BAR 전부(사이징만 흉내 내고 실제 재배치 금지),
 *      capability list 포인터와 각 capability 의 next 포인터(chain 재작성),
 *      INTx Disable 비트(실제 마스킹은 vfio_pci_intrs.c 가 대행),
 *      Interrupt Pin(INTx 를 숨기기 위해), VF 의 Vendor/Device ID,
 *      PM 의 PME 관련 비트, PCIe DEVCTL 의 FLR 비트, VPD 주소/데이터 레지스터.
 * 
 *  (3) 거부(refused) — write 비트가 0 인 곳.
 *      쓰기가 조용히 버려진다. 오류를 내지 않고 count 를 그대로 반환하므로
 *      사용자 쪽에서는 성공한 것처럼 보인다. 오류를 내면 게스트 드라이버가
 *      엉뚱한 곳에서 죽기 때문에 일부러 조용히 삼킨다.
 *      안전한 이유: 그 비트를 사용자가 바꾸면 호스트나 다른 디바이스에 영향을
 *      주기 때문이다.
 *      대표 예: Vendor/Device ID 같은 읽기 전용 필드, PCIe DEVCTL 의 Phantom
 *      Function 비트(IOMMU 가 보는 requester ID 를 흐트러뜨린다), DEVCTL2 의
 *      ARI 비트(probe 시점에 커널이 정한다), Max Payload Size(시스템 전체를
 *      봐야 정할 수 있다), 그리고 표에 아무 예외도 뚫려 있지 않은 모든 곳.
 *      기본값이 바로 이 (3)이라는 점이 이 파일의 설계 원칙이다 — 명시적으로
 *      허용한 것만 허용된다.
 * 
 *  (예외) raw 접근 — 지도에 capability 가 없는 빈틈(unassigned_perms)과
 *      Vendor-Specific capability 는 virt/write 비트맵 없이 읽기도 쓰기도 그대로
 *      통과한다. be2net 처럼 capability 사이 빈 공간에 자기 레지스터를 숨겨 둔
 *      디바이스가 있어 어쩔 수 없다. 상류 주석이 적어 두었듯 이 경우는 MMIO 나
 *      I/O 포트와 마찬가지로 하드웨어 격리를 믿는 수밖에 없다. */
/* [한국어] loff_t 타입과 파일 계층 기본 정의. 이 파일의 두 진입점이 파일 오프셋을
 * loff_t 로 받으므로 필요하다. */
#include <linux/fs.h>
/* [한국어] PCI 코어 전체 — struct pci_dev, pci_read_config_ 계열과 pci_user_ 계열,
 * pci_resource_start/len/flags, pci_try_reset_function, pcie_set_readrq,
 * pci_read_vpd, pci_match_id, 그리고 PCI_ 로 시작하는 모든 레지스터 오프셋과
 * 비트 상수. 이 파일이 가장 크게 의존하는 헤더다. 다만 이 트리는 sparse
 * checkout 이라 헤더 원문(include/linux/pci.h 와 그것이 끌어오는
 * uapi/linux/pci_regs.h)은 없어 상수 값 자체는 확인 못 함. */
#include <linux/pci.h>
/* [한국어] copy_to_user 와 copy_from_user. vfio_pci_config_rw_single 이 사용자
 * 버퍼와 주고받는 유일한 수단이다. */
#include <linux/uaccess.h>
/* [한국어] VFIO 외부 ABI. 이 파일은 struct vfio_device 를 직접 다루지는 않지만,
 * vfio_pci_core.h 가 이 헤더 위에 얹혀 있어 타입 사슬을 완성하는 데 필요하다. */
#include <linux/vfio.h>
/* [한국어] kmalloc, kzalloc, kfree, kmalloc_obj. 권한 비트맵과 디바이스별 지도/
 * 그림자 복사본 할당에 쓴다. */
#include <linux/slab.h>

/* [한국어] drivers/vfio/pci 내부 전용 헤더. 이 파일이 노출하는 6개 함수의 선언,
 * PCI_CAP_ID_INVALID 와 PCI_CAP_ID_INVALID_VIRT 두 특수 ID, 그리고 이 파일이
 * 부르는 vfio_pci_zap_and_down_write_memory_lock, vfio_pci_set_power_state,
 * vfio_pci_dma_buf_move, vfio_pci_intx_mask, vfio_pci_intx_unmask 의 선언이
 * 모두 여기 있다. include/linux/vfio_pci_core.h 도 이 헤더를 통해 딸려 온다. */
#include "vfio_pci_priv.h"

/* Fake capability ID for standard config space */
/* [한국어] 표준 config 헤더(0~63바이트)를 하나의 가짜 capability 로 취급하기 위한 ID.
 * 진짜 capability ID 중에는 0 이 없어(스펙상 0 은 NULL capability) 이 값을
 * 빌려 쓸 수 있다. 덕분에 지도(pci_config_map)와 권한 표(cap_perms) 양쪽에서
 * 표준 헤더를 다른 capability 와 똑같은 방식으로 다룰 수 있다. 대가로,
 * 하드웨어 chain 에서 진짜 ID 0 을 만나면 구별할 수 없어 감춰 버린다
 * (vfio_cap_init 의 상류 주석이 그 사실을 밝힌다). */
#define PCI_CAP_ID_BASIC	0

/* [한국어] 이 오프셋이 BAR 6개나 ROM BAR 안에 드는지 판정하는 매크로.
 * BAR0 부터 BAR5 까지는 4바이트씩 연속이라 마지막 BAR 의 끝(+4)까지를 한 구간으로
 * 보고, ROM BAR 는 따로 떨어져 있어 두 번째 구간으로 본다.
 * 쓰는 곳은 vfio_basic_config_read(사이징 계산을 돌릴지 판단)와
 * vfio_basic_config_write(bardirty 를 켤지 판단) 두 곳뿐이며, 둘 다 표준 헤더
 * 안이라 pos 와 offset 이 같다는 전제가 성립한다.
 * 인자를 괄호로 감싸지 않았지만 두 호출자가 모두 단순 변수를 넘겨 문제가 없다. */
#define is_bar(offset)	\
	((offset >= PCI_BASE_ADDRESS_0 && offset < PCI_BASE_ADDRESS_5 + 4) || \
	 (offset >= PCI_ROM_ADDRESS && offset < PCI_ROM_ADDRESS + 4))

/*
 * Lengths of PCI Config Capabilities
 *   0: Removed from the user visible capability list
 *   FF: Variable length
 */
/* [한국어] 표준 capability ID 별 길이표.
 * 설정자: 컴파일 시 상수. 런타임에 바뀌지 않는다(const).
 * 읽는 자: vfio_cap_init 이 chain 을 순회하며 각 capability 의 길이를 여기서 찾고,
 * 그 값이 0xFF 면 vfio_cap_len 으로 실측한다.
 * 값 범위: 0 = 사용자에게 보이는 capability 목록에서 제거(감춤),
 * 0xFF = 가변 길이라 하드웨어를 읽어 봐야 함, 그 밖에는 바이트 길이.
 * 표에 아예 없는 ID 는 배열 초기화 규칙에 따라 0 이 되어 자동으로 감춰진다.
 * 동기화: 읽기 전용이라 필요 없다. */
static const u8 pci_cap_length[PCI_CAP_ID_MAX + 1] = {
	/* [한국어] 0번 슬롯은 위에서 정의한 가짜 ID 로, 표준 config 헤더 64바이트를 뜻한다.
	 * 이 항목은 vfio_cap_init 의 chain 순회에서는 쓰이지 않고
	 * (chain 에 ID 0 이 나오면 감춰 버린다) 표의 자리를 채워 두는 역할이다. */
	[PCI_CAP_ID_BASIC]	= PCI_STD_HEADER_SIZEOF, /* pci config header */
	/* [한국어] Power Management. 고정 길이이며 전용 권한 표와 writefn 을 갖는다
	 * (init_pci_cap_pm_perm, vfio_pm_config_write). */
	[PCI_CAP_ID_PM]		= PCI_PM_SIZEOF,
	/* [한국어] AGP. 고정 길이이고 전용 표가 없어 기본값(읽기 직통 + 쓰기 거부)이 적용된다.
	 * 사용자에게 보이기는 하되 아무것도 바꿀 수 없다. */
	[PCI_CAP_ID_AGP]	= PCI_AGP_SIZEOF,
	/* [한국어] Vital Product Data. 고정 길이이며 주소/데이터 레지스터를 통째로 가상화해
	 * 커널이 간접 접근 프로토콜을 대행한다(init_pci_cap_vpd_perm). */
	[PCI_CAP_ID_VPD]	= PCI_CAP_VPD_SIZEOF,
	/* [한국어] Slot Identification. 길이 0 = 감춤. 브리지 전용 capability 인데 VFIO 는
	 * 엔드포인트 함수만 넘기므로 사용자에게 보일 이유가 없다. */
	[PCI_CAP_ID_SLOTID]	= 0,		/* bridge - don't care */
	/* [한국어] MSI. 0xFF = 가변. 64비트 주소 지원과 per-vector masking 지원 여부에 따라
	 * 네 가지 길이가 나오며, vfio_msi_cap_len 이 실측하면서 디바이스 전용 권한
	 * 표까지 함께 만든다. */
	[PCI_CAP_ID_MSI]	= 0xFF,		/* 10, 14, 20, or 24 */
	/* [한국어] CompactPCI Hot Swap. 감춤. 핫스왑 제어를 사용자에게 주면 호스트가 모르는
	 * 사이 슬롯 상태가 바뀔 수 있고, 아직 지원 계획도 없다. */
	[PCI_CAP_ID_CHSWP]	= 0,		/* cpci - not yet */
	/* [한국어] PCI-X. 0xFF = 가변. 명령 레지스터의 버전 필드로 v0(8바이트)와 v2(24바이트)가
	 * 갈리며 vfio_cap_len 이 판별한다. */
	[PCI_CAP_ID_PCIX]	= 0xFF,		/* 8 or 24 */
	/* [한국어] HyperTransport. 0xFF = 가변. 세 번째 바이트의 비트로 짧은 형식과 긴 형식이
	 * 갈린다. 전용 표가 없어 읽기 전용으로 노출된다. */
	[PCI_CAP_ID_HT]		= 0xFF,		/* hypertransport */
	/* [한국어] Vendor-Specific. 0xFF = 가변이며 길이 필드가 capability 안에 들어 있다.
	 * 내용이 벤더 정의라 커널이 의미를 알 수 없어, vfio_pci_init_perm_bits 가
	 * 이 슬롯의 writefn 만 raw 쓰기로 바꿔 통째로 통과시킨다. */
	[PCI_CAP_ID_VNDR]	= 0xFF,		/* variable */
	/* [한국어] Debug Port. 감춤. 디버그 포트는 플랫폼 펌웨어와 커널 콘솔이 쓰는 자원이라
	 * 사용자에게 넘기면 호스트 디버깅 경로를 가로챌 수 있다. */
	[PCI_CAP_ID_DBG]	= 0,		/* debug - don't care */
	/* [한국어] CompactPCI Central Resource Control. 감춤. 위 CHSWP 와 같은 이유다. */
	[PCI_CAP_ID_CCRC]	= 0,		/* cpci - not yet */
	/* [한국어] Standard Hot-Plug Controller. 감춤. 핫플러그 컨트롤러를 사용자가 조작하면
	 * 호스트가 모르는 사이 슬롯 전원과 링크 상태가 바뀐다. */
	[PCI_CAP_ID_SHPC]	= 0,		/* hotswap - not yet */
	/* [한국어] Bridge Subsystem Vendor ID. 감춤. 브리지 전용이라 엔드포인트에는 없다. */
	[PCI_CAP_ID_SSVID]	= 0,		/* bridge - don't care */
	/* [한국어] AGP 8x. 감춤. 지원 계획이 없다. */
	[PCI_CAP_ID_AGP3]	= 0,		/* AGP8x - not yet */
	/* [한국어] Secure Device. 감춤. 보안 관련 제어를 검증 없이 넘길 수 없다. */
	[PCI_CAP_ID_SECDEV]	= 0,		/* secure device not yet */
	/* [한국어] PCI Express. 0xFF = 가변. capability 버전과 디바이스 타입(특히 링크 없는
	 * Root Complex Integrated Endpoint 인지)에 따라 길이가 갈린다. 전용 표와
	 * writefn 이 FLR 과 MRRS 를 처리한다. */
	[PCI_CAP_ID_EXP]	= 0xFF,		/* 20 or 44 */
	/* [한국어] MSI-X. 고정 길이(12바이트)다. 여기에 전용 권한 표가 없다는 점이 중요한데,
	 * MSI-X 는 config 공간에 제어 레지스터만 있고 실제 벡터 테이블은 BAR 안의
	 * MMIO 에 있기 때문이다. 그 테이블 보호는 config 계층이 아니라 BAR 매핑
	 * 계층(vfio_pci_core.c 와 vfio_pci_rdwr.c)이 맡는다. 그래서 이 capability 는
	 * 기본값대로 읽기 직통 + 쓰기 거부로 노출된다. */
	[PCI_CAP_ID_MSIX]	= PCI_CAP_MSIX_SIZEOF,
	/* [한국어] Serial ATA. 0xFF = 가변. 인덱스/데이터 쌍이 config 공간에 인라인인지에
	 * 따라 길이가 갈린다. */
	[PCI_CAP_ID_SATA]	= 0xFF,
	/* [한국어] Advanced Features. 고정 길이. PCIe 가 아닌 전통 PCI 디바이스에 FLR 을
	 * 얹어 주는 capability 이며 전용 writefn 이 그 FLR 을 대행한다. */
	[PCI_CAP_ID_AF]		= PCI_CAP_AF_SIZEOF,
};

/*
 * Lengths of PCIe/PCI-X Extended Config Capabilities
 *   0: Removed or masked from the user visible capability list
 *   FF: Variable length
 */
/* [한국어] 확장(PCIe/PCI-X) capability ID 별 길이표.
 * 설정자: 컴파일 시 상수(const).
 * 읽는 자: vfio_ecap_init 이 확장 chain 을 순회하며 참조하고, 0xFF 면
 * vfio_ext_cap_len 으로 실측한다.
 * 값 범위: 0 = 목록에서 제거하거나 가려 버림, 0xFF = 가변, 그 밖에는 바이트 길이.
 * u8 이 아니라 u16 인 이유는 확장 capability 가 표준보다 길 수 있기 때문이다.
 * 동기화: 읽기 전용. */
static const u16 pci_ext_cap_length[PCI_EXT_CAP_ID_MAX + 1] = {
	/* [한국어] AER. 길이를 Root Error Command 레지스터의 오프셋으로 잘라 두었다.
	 * 그 뒤는 루트 포트 전용 레지스터라 엔드포인트를 넘겨받는 사용자에게 보일
	 * 이유가 없고, 표 밖이라 자동으로 쓰기가 거부된다. */
	[PCI_EXT_CAP_ID_ERR]	=	PCI_ERR_ROOT_COMMAND,
	/* [한국어] Virtual Channel. 가변 — 확장 VC 개수와 중재 테이블 크기에 달렸다.
	 * vfio_vc_cap_len 이 계산한다. */
	[PCI_EXT_CAP_ID_VC]	=	0xFF,
	/* [한국어] Device Serial Number. 고정 길이이고 순수 읽기 전용 정보라 그대로 노출한다. */
	[PCI_EXT_CAP_ID_DSN]	=	PCI_EXT_CAP_DSN_SIZEOF,
	/* [한국어] Power Budgeting. 고정 길이. 전용 표가 데이터 선택자에만 쓰기를 허용한다. */
	[PCI_EXT_CAP_ID_PWR]	=	PCI_EXT_CAP_PWR_SIZEOF,
	/* [한국어] Root Complex Link Declaration. 감춤 — 루트 컴플렉스 전용이라 엔드포인트에는
	 * 의미가 없다. */
	[PCI_EXT_CAP_ID_RCLD]	=	0,	/* root only - don't care */
	/* [한국어] Root Complex Internal Link Control. 감춤 — 같은 이유. */
	[PCI_EXT_CAP_ID_RCILC]	=	0,	/* root only - don't care */
	/* [한국어] Root Complex Event Collector. 감춤 — 같은 이유. */
	[PCI_EXT_CAP_ID_RCEC]	=	0,	/* root only - don't care */
	/* [한국어] Multi-Function Virtual Channel. 가변이며 VC 와 같은 계산식을 쓴다. */
	[PCI_EXT_CAP_ID_MFVC]	=	0xFF,
	/* [한국어] VC 의 두 번째 ID 배정(스펙 개정 과정에서 생긴 별칭). 상류 주석대로 VC 와
	 * 완전히 같은 구조라 같은 길이 계산을 쓴다. */
	[PCI_EXT_CAP_ID_VC9]	=	0xFF,	/* same as CAP_ID_VC */
	/* [한국어] Root Complex Register Block. 감춤 — 루트 전용. */
	[PCI_EXT_CAP_ID_RCRB]	=	0,	/* root only - don't care */
	/* [한국어] 확장 Vendor-Specific. 가변이며 헤더에 길이 필드가 있다. 표준판과 마찬가지로
	 * writefn 만 raw 쓰기로 바뀌어 통째로 통과한다. */
	[PCI_EXT_CAP_ID_VNDR]	=	0xFF,
	/* [한국어] Configuration Access Correlation. 감춤 — 스펙에서 폐기된 항목이다. */
	[PCI_EXT_CAP_ID_CAC]	=	0,	/* obsolete */
	/* [한국어] Access Control Services. 가변 — egress control 벡터 유무에 달렸다.
	 * ACS 는 IOMMU 격리의 전제(peer-to-peer 트래픽 차단)를 세우는 기능이라 커널이
	 * 소유해야 하지만, 전용 표가 없어 기본값대로 읽기 전용으로만 노출된다.
	 * 사용자는 볼 수 있고 바꿀 수는 없다. */
	[PCI_EXT_CAP_ID_ACS]	=	0xFF,
	/* [한국어] Alternative Routing-ID Interpretation. 고정 길이. 활성 비트 자체는 PCIe
	 * capability 의 DEVCTL2 에 있고 그쪽에서 쓰기가 막혀 있다. */
	[PCI_EXT_CAP_ID_ARI]	=	PCI_EXT_CAP_ARI_SIZEOF,
	/* [한국어] Address Translation Services. 고정 길이. 읽기 전용으로만 노출된다 —
	 * ATS 를 사용자가 켜면 디바이스가 자기 캐시로 IOVA 를 번역해 IOMMU 를
	 * 우회하게 되므로, 활성화는 커널만 할 수 있어야 한다. */
	[PCI_EXT_CAP_ID_ATS]	=	PCI_EXT_CAP_ATS_SIZEOF,
	/* [한국어] SR-IOV. 고정 길이. 읽기 전용으로만 노출된다. VF 생성/삭제와 VF BAR 배치가
	 * 전부 이 capability 에 있어 사용자가 쓰면 호스트가 모르는 VF 가 생긴다. */
	[PCI_EXT_CAP_ID_SRIOV]	=	PCI_EXT_CAP_SRIOV_SIZEOF,
	/* [한국어] Multi-Root IOV. 감춤 — 지원 계획이 없다. */
	[PCI_EXT_CAP_ID_MRIOV]	=	0,	/* not yet */
	/* [한국어] Multicast. 고정 길이(엔드포인트판). 읽기 전용. */
	[PCI_EXT_CAP_ID_MCAST]	=	PCI_EXT_CAP_MCAST_ENDPOINT_SIZEOF,
	/* [한국어] Page Request Interface. 고정 길이. 읽기 전용 — PRI 는 IOMMU 와 짝을 이루는
	 * 기능이라 커널이 소유한다. */
	[PCI_EXT_CAP_ID_PRI]	=	PCI_EXT_CAP_PRI_SIZEOF,
	/* [한국어] AMD 고유 확장. 감춤 — 의미를 모르는 벤더 확장이다. */
	[PCI_EXT_CAP_ID_AMD_XXX] =	0,	/* not yet */
	/* [한국어] Resizable BAR. 가변 — 제어 대상 BAR 개수만큼 항목이 붙는다.
	 * 읽기 전용으로만 노출된다. 사용자가 BAR 크기를 바꾸면 호스트가 배정해 둔
	 * 자원 창과 어긋난다. */
	[PCI_EXT_CAP_ID_REBAR]	=	0xFF,
	/* [한국어] Dynamic Power Allocation. 가변 — substate 개수에 달렸다. */
	[PCI_EXT_CAP_ID_DPA]	=	0xFF,
	/* [한국어] TLP Processing Hints. 가변 — steering tag 테이블이 config 공간에 있는
	 * 경우에만 길어진다. */
	[PCI_EXT_CAP_ID_TPH]	=	0xFF,
	/* [한국어] Latency Tolerance Reporting. 고정 길이. 읽기 전용. */
	[PCI_EXT_CAP_ID_LTR]	=	PCI_EXT_CAP_LTR_SIZEOF,
	/* [한국어] Secondary PCI Express. 감춤 — 링크 속도 재협상 제어가 들어 있어 사용자에게
	 * 줄 수 없고, 지원 계획도 없다. */
	[PCI_EXT_CAP_ID_SECPCI]	=	0,	/* not yet */
	/* [한국어] Protocol Multiplexing. 감춤 — 지원 계획이 없다. */
	[PCI_EXT_CAP_ID_PMUX]	=	0,	/* not yet */
	/* [한국어] Process Address Space ID. 감춤 — PASID 는 IOMMU 의 주소 공간 분리와 직결돼
	 * 커널이 소유해야 한다. */
	[PCI_EXT_CAP_ID_PASID]	=	0,	/* not yet */
	/* [한국어] Designated Vendor-Specific. 가변이며 헤더에 길이 필드가 있다. 확장
	 * Vendor-Specific 과 마찬가지로 writefn 만 raw 쓰기로 바뀐다. */
	[PCI_EXT_CAP_ID_DVSEC]	=	0xFF,
};

/*
 * Read/Write Permission Bits - one bit for each bit in capability
 * Any field can be read if it exists, but what is read depends on
 * whether the field is 'virtualized', or just pass through to the
 * hardware.  Any virtualized field is also virtualized for writes.
 * Writes are only permitted if they have a 1 bit here.
 */
/* [한국어] 이 파일의 심장. capability 하나에 대한 "무엇을 읽고 무엇을 쓸 수 있는가" 를
 * 담는다. 시스템 전역으로 공유되는 것이 원칙이고(cap_perms, ecap_perms),
 * MSI 만 디바이스마다 다른 모양이라 vdev->msi_perm 으로 따로 둔다.
 * 상류 주석이 규칙을 요약한다 — 존재하는 필드는 무엇이든 읽을 수 있되 그 값이
 * 하드웨어에서 오는지 vconfig 에서 오는지는 virt 가 정하고, 가상화된 필드는
 * 쓰기도 가상화되며, 쓰기는 write 에 1 이 선 비트만 허용된다. */
struct perm_bits {
	/* [한국어] 가상화 비트맵. 비트가 1 인 자리는 하드웨어가 아니라 vconfig 에서 읽고 쓴다.
	 * 설정자: alloc_perm_bits 가 0 으로 만들고(= 가상화 없음이 기본),
	 * init_pci_cap_ 계열이 p_setb/p_setw/p_setd 로 예외를 뚫는다.
	 * 읽는 자: vfio_default_config_read 와 vfio_default_config_write 가 마스크로 쓴다.
	 * 값 범위: capability 길이만큼(dword 로 올림)의 바이트 배열. NULL 이면 비트맵
	 * 없는 표(unassigned_perms, virt_perms, direct_ro_perms)이며, 그런 표의
	 * readfn/writefn 은 비트맵을 참조하지 않는 구현이어야 한다.
	 * 동기화: 초기화 뒤로는 읽기 전용이라 락이 없다. 공유 표는 모듈 적재 시,
	 * msi_perm 은 디바이스 첫 열기 시 한 번만 채워진다. */
	u8	*virt;		/* read/write virtual data, not hw */
	/* [한국어] 쓰기 허용 비트맵. 비트가 1 인 자리만 사용자 쓰기가 반영된다.
	 * 설정자: alloc_perm_bits 가 0 으로 만들고(= 전면 거부가 기본),
	 * init_pci_cap_ 계열이 예외를 뚫는다.
	 * 읽는 자: vfio_default_config_write 가 가장 먼저 이 마스크를 본다. 통째로
	 * 0 이면 아무 일도 하지 않고 성공으로 보고한다.
	 * 값 범위: virt 와 같은 크기의 바이트 배열. NULL 이면 비트맵 없는 표다.
	 * 동기화: virt 와 같다. */
	u8	*write;		/* writeable bits */
	/* [한국어] 읽기 콜백. NULL 이면 읽기가 아예 수행되지 않고 사용자에게 0 이 간다.
	 * 설정자: alloc_perm_bits 가 vfio_default_config_read 를 기본으로 심고,
	 * cap_perms/ecap_perms 의 지정 초기화가 vfio_direct_config_read 를 기본으로
	 * 두며, 개별 init 함수와 파일 정적 표 정의가 필요에 따라 덮어쓴다.
	 * 읽는 자: vfio_pci_config_rw_single 이 딱 한 곳에서 호출한다.
	 * 값 범위: 이 파일의 여덟 후보 중 하나 — vfio_default_config_read,
	 * vfio_direct_config_read, vfio_raw_config_read, vfio_virt_config_read,
	 * vfio_basic_config_read, vfio_msi_config_read.
	 * 동기화: 초기화 뒤 읽기 전용. */
	int	(*readfn)(struct vfio_pci_core_device *vdev, int pos, int count,
			  struct perm_bits *perm, int offset, __le32 *val);
	/* [한국어] 쓰기 콜백. NULL 이면 쓰기가 조용히 무시되며 사용자에게는 성공으로 보인다 —
	 * 읽기 전용 capability 를 표현하는 방법이 바로 이 NULL 이다.
	 * 설정자: alloc_perm_bits 가 vfio_default_config_write 를 심고, 개별 init
	 * 함수와 vfio_pci_init_perm_bits 가 필요에 따라 덮어쓴다. cap_perms/ecap_perms
	 * 의 기본값은 NULL 이라 대부분의 capability 가 자동으로 읽기 전용이 된다.
	 * 읽는 자: vfio_pci_config_rw_single 한 곳.
	 * 값 범위: NULL 이거나 vfio_default_config_write, vfio_raw_config_write,
	 * vfio_virt_config_write, vfio_basic_config_write, vfio_pm_config_write,
	 * vfio_vpd_config_write, vfio_exp_config_write, vfio_af_config_write,
	 * vfio_msi_config_write 중 하나.
	 * 동기화: 초기화 뒤 읽기 전용. */
	int	(*writefn)(struct vfio_pci_core_device *vdev, int pos, int count,
			   struct perm_bits *perm, int offset, __le32 val);
};

/* [한국어] "이 필드는 가상화하지 않는다" 를 뜻하는 p_set 계열 인자. 값이 0 이라
 * 비트맵을 건드리지 않는 것과 같지만, 표를 읽는 사람이 "여기는 하드웨어
 * 직통" 이라고 명시적으로 읽게 해 준다. */
#define	NO_VIRT		0
/* [한국어] "이 필드 전체를 가상화한다". 32비트 전부 1 이며, p_setb 와 p_setw 에
 * 넘길 때는 호출자가 (u8) 또는 (u16) 으로 잘라 쓴다. */
#define	ALL_VIRT	0xFFFFFFFFU
/* [한국어] "사용자 쓰기를 전면 거부한다". 이 파일의 기본 상태와 같은 값이지만
 * 표에서 의도를 드러내기 위해 명시적으로 적는다. */
#define	NO_WRITE	0
/* [한국어] "이 필드 전체에 사용자 쓰기를 허용한다". 가상화 여부와 조합해
 * (ALL_VIRT, ALL_WRITE)면 완전 에뮬레이션, (NO_VIRT, ALL_WRITE)면 하드웨어
 * 직통 쓰기가 된다. */
#define	ALL_WRITE	0xFFFFFFFFU

/* [한국어]
 * vfio_user_config_read - 폭(1/2/4바이트)에 맞는 PCI config 읽기 함수를 골라 부른다
 *
 * @pdev: 읽을 대상 PCI 함수. vdev->pdev 에서 온다.
 * @offset: config space 안의 바이트 오프셋(0 ~ pdev->cfg_size-1).
 * @val: 결과를 리틀엔디언으로 담아 줄 곳. 항상 __le32 한 칸에 담고,
 *       count 가 1 이나 2 면 상위 바이트는 0 으로 남는다.
 * @count: 1, 2, 4 중 하나. 호출자가 이미 정렬과 크기를 맞춰 놓았다.
 * @return: 0 이면 성공. 그 밖에는 PCIBIOS_ 계열 오류 코드(양수)이거나,
 *          count 가 1/2/4 가 아니면 초기값 -EINVAL 이 그대로 나간다.
 *          호출자(vfio_default_config_read 등)는 0 이 아니면 즉시 되돌아간다.
 *
 * 왜 필요한가: PCI config 접근 API 는 폭마다 함수가 따로 있어서
 * (byte/word/dword) 폭을 런타임 값으로 다루려면 매번 switch 를 써야 한다.
 * 이 파일은 그 switch 를 한 곳에 모아 두고, 읽은 값을 항상 __le32 한 칸에
 * 정규화해 준다. 뒤이어 virt 비트맵과 비트 연산으로 섞으려면 폭이 달라도 같은
 * 32비트 그릇에 담겨 있어야 하기 때문이다.
 *
 * 동작 과정:
 *  1. ret 을 -EINVAL 로, tmp_val 을 0 으로 초기화한다. count 가 이상하면
 *     switch 의 어느 가지에도 걸리지 않고 이 값이 그대로 결과가 된다.
 *  2. count 에 따라 pci_user_read_config_byte / _word / _dword 중 하나를 부른다.
 *  3. 1바이트/2바이트로 읽었으면 좁은 임시 변수에서 u32 로 넓힌다 —
 *     상위 바이트는 자동으로 0 이 된다.
 *  4. cpu_to_le32 로 리틀엔디언 표현으로 바꿔 *val 에 담는다. PCI config
 *     공간은 스펙상 리틀엔디언이고, 이 파일의 virt/write 비트맵도 그 순서로
 *     저장돼 있어 두 값을 memcpy 로 겹쳐 쓰려면 표현이 같아야 한다.
 *
 * pci_user_read_config_ 계열을 쓰는 이유: 일반 pci_read_config_ 계열과 달리
 * 이 판은 pci_cfg_access_lock 으로 config 접근이 차단된 동안 오류를 내지 않고
 * 잠들어 기다린다(drivers/pci/access.c:707 의 매크로와 :653 의 pci_wait_cfg).
 * 사용자를 대신하는 접근이므로 실패시키는 것보다 기다리는 편이 옳다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 위 이유로 잠들 수 있으므로 스핀락 안에서
 * 불러서는 안 된다.
 *
 * 에러 경로: 오류 코드를 그대로 위로 올린다. 다만 *val 은 이미 (0일 수도 있는)
 * 값으로 덮어써진 뒤이므로, 호출자는 반드시 반환값을 먼저 확인해야 한다.
 *
 * 호출 체인:
 *   vfio_default_config_read / vfio_default_config_write /
 *   vfio_direct_config_read / vfio_raw_config_read
 *     → [vfio_user_config_read]
 *     → pci_user_read_config_byte / _word / _dword (drivers/pci/access.c:796~798)
 */
static int vfio_user_config_read(struct pci_dev *pdev, int offset,
				 __le32 *val, int count)
{
	/* [한국어] count 가 1/2/4 가 아니면 switch 의 어느 가지에도 걸리지 않는다. 그때
	 * 그대로 반환될 값을 미리 넣어 둔다 — 호출자의 프로그래밍 오류를 조용히
	 * 성공으로 만들지 않기 위해서다. */
	int ret = -EINVAL;
	/* [한국어] 읽기 실패 시에도 *val 에 쓰레기가 들어가지 않도록 0 으로 시작한다. */
	u32 tmp_val = 0;

	/* [한국어] 폭에 따라 PCI 접근자를 고른다. PCI config 접근은 1/2/4바이트만 정의돼
	 * 있고 각각 함수가 따로 있다. */
	switch (count) {
	/* [한국어] 1바이트 읽기. */
	case 1:
	{
		/* [한국어] 접근자가 u8 포인터를 요구하므로 좁은 임시 변수를 둔다. 블록 스코프로
		 * 감싼 이유는 case 안에서 변수를 선언하기 위해서다. */
		u8 tmp;
		/* [한국어] 사용자를 대신하는 1바이트 config 읽기. 일반 판과 달리 config 접근이
		 * 차단된 동안 오류를 내지 않고 잠들어 기다린다(drivers/pci/access.c:796). */
		ret = pci_user_read_config_byte(pdev, offset, &tmp);
		/* [한국어] u8 을 u32 로 넓힌다. 상위 24비트는 0 으로 채워진다. */
		tmp_val = tmp;
		break;
	}
	/* [한국어] 2바이트 읽기. */
	case 2:
	{
		/* [한국어] 접근자가 u16 포인터를 요구한다. */
		u16 tmp;
		/* [한국어] 2바이트 config 읽기(drivers/pci/access.c:797). */
		ret = pci_user_read_config_word(pdev, offset, &tmp);
		/* [한국어] u16 을 u32 로 넓힌다. 상위 16비트는 0. */
		tmp_val = tmp;
		break;
	}
	/* [한국어] 4바이트 읽기. 목적지가 이미 u32 라 임시 변수가 필요 없다. */
	case 4:
		/* [한국어] 4바이트 config 읽기(drivers/pci/access.c:798). */
		ret = pci_user_read_config_dword(pdev, offset, &tmp_val);
		break;
	}

	/* [한국어] CPU 표현을 리틀엔디언으로 바꿔 담는다. 이 파일은 하드웨어 값과 vconfig
	 * 값과 비트맵을 memcpy 와 비트 연산으로 섞으므로, 세 가지가 모두 PCI 의
	 * 리틀엔디언 순서로 통일돼 있어야 한다. 오류 상황에서도 이 대입은 실행되므로
	 * 호출자가 반환값을 먼저 봐야 한다. */
	*val = cpu_to_le32(tmp_val);

	/* [한국어] 0 이면 성공. 그 밖에는 PCIBIOS 계열 오류이거나 초기값 -EINVAL 이다. */
	return ret;
}

/* [한국어]
 * vfio_user_config_write - 폭(1/2/4바이트)에 맞는 PCI config 쓰기 함수를 골라 부른다
 *
 * @pdev: 쓸 대상 PCI 함수.
 * @offset: config space 안의 바이트 오프셋.
 * @val: 쓸 값. 리틀엔디언 __le32 로 들어오며, 하위 count 바이트만 쓰인다.
 * @count: 1, 2, 4 중 하나.
 * @return: 0 이면 성공. PCIBIOS_ 계열 오류 코드거나, count 가 1/2/4 가 아니면
 *          초기값 -EINVAL. 호출자는 0 이 아니면 즉시 오류로 되돌아간다.
 *
 * 왜 필요한가: 위 vfio_user_config_read 와 정확히 대칭이다. 폭별 API 분기를
 * 한 곳에 모으고, 이 파일 안에서만 쓰는 __le32 정규 표현을 CPU 바이트 순서로
 * 되돌려 준다.
 *
 * 동작 과정:
 *  1. le32_to_cpu 로 CPU 표현 u32 를 얻는다. 하드웨어에 쓸 값은 CPU 표현으로
 *     넘겨야 한다 — 리틀엔디언 변환은 PCI 접근자 안쪽에서 다시 해 준다.
 *  2. count 에 따라 pci_user_write_config_byte / _word / _dword 를 부른다.
 *     좁은 폭으로 쓸 때는 tmp_val 의 상위 비트가 그냥 잘린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 읽기와 마찬가지로 잠들 수 있다.
 *
 * 에러 경로: 오류를 그대로 위로 올린다. 부분 쓰기가 이미 하드웨어에 반영됐을
 * 수 있으나(4바이트 요청은 접근자 한 번으로 끝나므로 실제로는 전부 아니면
 * 전무다) 이 계층에서 되돌릴 방법은 없다.
 *
 * 호출 체인:
 *   vfio_default_config_write / vfio_raw_config_write
 *     → [vfio_user_config_write]
 *     → pci_user_write_config_byte / _word / _dword (drivers/pci/access.c:799~801)
 */
static int vfio_user_config_write(struct pci_dev *pdev, int offset,
				  __le32 val, int count)
{
	/* [한국어] count 가 1/2/4 가 아닐 때 반환될 값. 읽기 쪽과 같은 방어다. */
	int ret = -EINVAL;
	/* [한국어] 리틀엔디언 정규 표현을 CPU 표현으로 되돌린다. PCI 쓰기 접근자는 CPU
	 * 표현을 받아 내부에서 다시 리틀엔디언으로 바꿔 버스에 내보낸다. */
	u32 tmp_val = le32_to_cpu(val);

	/* [한국어] 폭에 따라 접근자를 고른다. */
	switch (count) {
	/* [한국어] 1바이트 쓰기. u32 인자가 u8 파라미터로 좁혀지며 상위 비트가 잘린다. */
	case 1:
		/* [한국어] 1바이트 config 쓰기(drivers/pci/access.c:799). */
		ret = pci_user_write_config_byte(pdev, offset, tmp_val);
		break;
	/* [한국어] 2바이트 쓰기. */
	case 2:
		/* [한국어] 2바이트 config 쓰기(drivers/pci/access.c:800). */
		ret = pci_user_write_config_word(pdev, offset, tmp_val);
		break;
	/* [한국어] 4바이트 쓰기. */
	case 4:
		/* [한국어] 4바이트 config 쓰기(drivers/pci/access.c:801). */
		ret = pci_user_write_config_dword(pdev, offset, tmp_val);
		break;
	}

	/* [한국어] 0 이면 성공, 아니면 오류나 초기값 -EINVAL. */
	return ret;
}

/* [한국어]
 * vfio_default_config_read - virt 비트맵을 마스크 삼아 vconfig 값과 하드웨어 값을 비트 단위로 섞는다
 *
 * @vdev: 대상 디바이스. vconfig(그림자 복사본)와 pdev 를 여기서 꺼낸다.
 * @pos: config space 안의 절대 오프셋. vconfig 인덱스로 그대로 쓰인다.
 * @count: 이번에 읽을 바이트 수(1/2/4).
 * @perm: 이 오프셋을 관장하는 권한 표. virt 비트맵을 여기서 꺼낸다.
 * @offset: capability 시작점으로부터의 상대 오프셋. perm->virt 의 인덱스다.
 *          pos 와 offset 이 다른 이유는 표가 capability 단위로 만들어지기
 *          때문이다 — 표는 "이 capability 의 몇 번째 바이트" 로 색인된다.
 * @val: 결과를 담을 곳.
 * @return: 성공하면 읽은 바이트 수(count). 하드웨어 읽기가 실패하면 그 오류.
 *          호출자는 이 값을 그대로 사용자에게 돌려줄 바이트 수로 쓴다.
 *
 * 왜 필요한가: 이 파일의 3분류 중 (1)통과와 (2)에뮬레이션이 한 dword 안에
 * 섞여 있을 수 있다. 예컨대 COMMAND 레지스터는 INTx Disable 비트 하나만
 * 가상화되고 나머지는 하드웨어 직통이다. 그래서 "비트별로 출처가 다른" 값을
 * 만들어 낼 엔진이 필요하다. 이 함수가 그 엔진이며, 이 파일의 거의 모든
 * readfn 이 마지막에 이것을 부른다.
 *
 * 동작 과정:
 *  1. vconfig 에서 count 바이트를 그대로 val 로 복사한다 — 일단 전부
 *     에뮬레이션 값으로 채워 둔다.
 *  2. perm->virt 에서 같은 폭만큼 가상화 마스크를 꺼낸다. 이 마스크는 비트가
 *     1 인 자리가 "vconfig 에서 읽어야 할 비트" 다.
 *  3. "이번 폭 전체가 가상화인가" 를 판별한다. count*8 비트짜리 전부 1 인
 *     값과 virt 가 같으면 하드웨어를 볼 필요가 없으므로 그대로 반환한다.
 *     이 비교를 cpu_to_le32 로 감싸는 이유는 virt 가 리틀엔디언으로 저장돼
 *     있기 때문이다 — 빅엔디언 호스트에서도 옳게 돌아가야 한다.
 *  4. 하나라도 통과 비트가 있으면 하드웨어에서 같은 폭을 실제로 읽는다.
 *  5. (하드웨어 값 & ~virt) | (vconfig 값 & virt) 로 비트별로 골라 합친다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 4단계에서 잠들 수 있다. vconfig 는 이 디바이스
 * 전용 버퍼이고 config 접근은 vfio_pci_core.c 쪽에서 열린 fd 단위로 직렬화되지
 * 않으므로, 같은 dword 를 두 스레드가 동시에 읽으면 서로의 중간 상태를 볼 수
 * 있다. 다만 그 결과는 사용자 자신의 데이터일 뿐이라 호스트 안전에는 영향이 없다.
 *
 * 에러 경로: 하드웨어 읽기 실패 시 그 오류를 그대로 반환한다. 이때 *val 에는
 * 1단계에서 넣은 vconfig 값이 남아 있지만, 호출자가 음수를 보고 버린다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->readfn(= vfio_basic_config_read /
 *     vfio_msi_config_read / 또는 alloc_perm_bits 가 심어 둔 기본값)
 *     → [vfio_default_config_read] → vfio_user_config_read
 */
static int vfio_default_config_read(struct vfio_pci_core_device *vdev, int pos,
				    int count, struct perm_bits *perm,
				    int offset, __le32 *val)
{
	/* [한국어] 이 폭에 해당하는 가상화 마스크를 담을 그릇. count 가 1 이나 2 면 상위
	 * 바이트가 0 으로 남아, 그 자리는 자동으로 "가상화 아님" 이 된다. */
	__le32 virt = 0;

	/* [한국어] 일단 그림자 복사본의 값으로 결과를 채운다. 아래에서 통과 비트만
	 * 하드웨어 값으로 덮어쓸 것이다. */
	memcpy(val, vdev->vconfig + pos, count);

	/* [한국어] 권한 표에서 이 위치의 가상화 마스크를 꺼낸다. 표의 색인이 pos 가 아니라
	 * offset 인 것에 주의 — 표는 capability 단위로 만들어져 "이 capability 의
	 * 몇 번째 바이트" 로 색인된다. */
	memcpy(&virt, perm->virt + offset, count);

	/* Any non-virtualized bits? */
	/* [한국어] "이번 폭이 통째로 가상화인가" 를 판별한다. ~0U 를 오른쪽으로 밀어
	 * count*8 비트만 1 인 값을 만들고(count 가 4 면 32-32=0 시프트라 정의되지
	 * 않은 동작 없이 0xFFFFFFFF 가 된다) 마스크와 비교한다. 다르면 통과 비트가
	 * 하나라도 있다는 뜻이므로 하드웨어를 읽어야 한다. 같으면 이미 채워 둔
	 * vconfig 값이 곧 정답이라 하드웨어 접근을 통째로 아낀다.
	 * 비교 대상을 cpu_to_le32 로 감싸는 이유는 virt 가 리틀엔디언으로 저장돼
	 * 있기 때문이다 — 빅엔디언 호스트에서도 옳아야 한다. */
	if (cpu_to_le32(~0U >> (32 - (count * 8))) != virt) {
		/* [한국어] 하드웨어 접근에 쓸 PCI 함수 핸들. */
		struct pci_dev *pdev = vdev->pdev;
		/* [한국어] 하드웨어에서 읽어 올 값. 실패해도 쓰레기가 섞이지 않도록 0 으로 시작. */
		__le32 phys_val = 0;
		/* [한국어] 하드웨어 읽기 결과 코드. */
		int ret;

		/* [한국어] 실제 하드웨어 config 를 읽는다. 여기서 잠들 수 있다. */
		ret = vfio_user_config_read(pdev, pos, &phys_val, count);
		if (ret)
			return ret;

		/* [한국어] 비트별로 출처를 고른다. 가상화 마스크가 0 인 비트는 하드웨어 값,
		 * 1 인 비트는 앞서 채워 둔 vconfig 값을 쓴다. 이 한 줄이 3분류의 (1)통과와
		 * (2)에뮬레이션을 한 dword 안에서 섞어 내는 지점이다. */
		*val = (phys_val & ~virt) | (*val & virt);
	}

	/* [한국어] 읽은 바이트 수를 돌려준다. 이 파일의 read/write 콜백은 모두 "처리한
	 * 바이트 수" 를 양수로, 오류를 음수로 반환하는 규약을 따른다. */
	return count;
}

/* [한국어]
 * vfio_default_config_write - write 비트맵으로 쓰기를 걸러내고, virt 여부에 따라 vconfig 와 하드웨어로 나눠 반영한다
 *
 * @vdev: 대상 디바이스.
 * @pos: config space 안의 절대 오프셋(vconfig 인덱스).
 * @count: 쓸 바이트 수(1/2/4).
 * @perm: 권한 표. write 와 virt 두 비트맵을 여기서 꺼낸다.
 * @offset: capability 안에서의 상대 오프셋(비트맵 인덱스).
 * @val: 사용자가 쓴 값(리틀엔디언).
 * @return: 성공하면 count. 하드웨어 읽기나 쓰기가 실패하면 그 오류.
 *
 * 왜 필요한가: 3분류를 실제로 집행하는 함수다. 한 번의 쓰기에 대해
 *  - write 비트가 0 인 비트는 그냥 버리고(거부),
 *  - write 이면서 virt 인 비트는 vconfig 에만 넣고(에뮬레이션),
 *  - write 이면서 virt 가 아닌 비트는 하드웨어에 넣는다(통과).
 * 세 갈래를 한 함수 안에서 비트 마스크만으로 갈라내는 것이 핵심이다.
 *
 * 동작 과정:
 *  1. perm->write 에서 쓰기 허용 마스크를 꺼낸다.
 *  2. 마스크가 통째로 0 이면 쓸 것이 없다. 오류가 아니라 count 를 돌려주어
 *     "성공했다" 고 보고한다 — 읽기 전용 레지스터에 쓴 게스트 드라이버가
 *     오류를 보고 죽는 것을 막기 위한 의도적인 조용한 무시다.
 *  3. perm->virt 에서 가상화 마스크를 꺼낸다.
 *  4. (write & virt) 가 있으면 vconfig 의 해당 비트만 지우고 사용자 값의
 *     같은 비트를 얹는다. read-modify-write 를 memcpy 로 꺼내 계산하고 다시
 *     memcpy 로 넣는 이유는 count 가 1/2/4 로 변하기 때문이다.
 *  5. (write & ~virt) 가 있으면 하드웨어를 먼저 읽어 현재 값을 얻고, 그
 *     비트만 갈아끼운 뒤 하드웨어에 되쓴다. 하드웨어 read-modify-write 를
 *     하는 이유는 허용되지 않은 비트를 사용자 값으로 덮어쓰면 안 되기
 *     때문이다 — 예컨대 COMMAND 의 상위 비트를 건드리면 안 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 5단계의 읽기와 쓰기 사이에는 아무 락도 없어서,
 * 같은 레지스터에 대한 동시 쓰기는 서로를 잃을 수 있다. 그러나 이 창은 사용자
 * 자신의 디바이스에만 영향을 주며, 호스트가 지키는 값(BAR, MSI 주소 등)은
 * 애초에 virt 로 분류돼 하드웨어에 닿지 않는다.
 *
 * 에러 경로: 4단계는 실패할 수 없다(메모리 복사뿐). 5단계에서 읽기나 쓰기가
 * 실패하면 그 오류를 반환하는데, 이때 4단계의 vconfig 갱신은 이미 끝난 상태라
 * 되돌아가지 않는다. 사용자 눈에는 "vconfig 는 바뀌었는데 오류" 로 보인다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(= vfio_basic_config_write /
 *     vfio_pm_config_write / vfio_vpd_config_write / vfio_exp_config_write /
 *     vfio_af_config_write / vfio_msi_config_write / 또는 기본값)
 *     → [vfio_default_config_write] → vfio_user_config_read + vfio_user_config_write
 */
static int vfio_default_config_write(struct vfio_pci_core_device *vdev, int pos,
				     int count, struct perm_bits *perm,
				     int offset, __le32 val)
{
	/* [한국어] 가상화 마스크와 쓰기 허용 마스크를 담을 두 그릇. 좁은 폭이면 상위
	 * 바이트가 0 으로 남아 "가상화 아님 + 쓰기 불가" 로 해석된다. */
	__le32 virt = 0, write = 0;

	/* [한국어] 권한 표에서 쓰기 허용 마스크를 꺼낸다. 3분류의 (3)거부 판정이 여기서
	 * 시작된다. */
	memcpy(&write, perm->write + offset, count);

	/* [한국어] 허용된 비트가 하나도 없다. 읽기 전용 레지스터에 쓴 경우다. */
	if (!write)
		return count; /* drop, no writable bits */

	/* [한국어] 가상화 마스크를 꺼낸다. 이제 write 와 virt 의 조합으로 세 갈래가 갈린다. */
	memcpy(&virt, perm->virt + offset, count);

	/* Virtualized and writable bits go to vconfig */
	/* [한국어] 쓰기 허용이면서 가상화된 비트가 있으면 그 비트는 vconfig 로 간다.
	 * 하드웨어에는 절대 닿지 않는다 — BAR 나 MSI 주소가 바로 이 갈래다. */
	if (write & virt) {
		/* [한국어] vconfig 의 현재 값을 꺼내 읽기-수정-쓰기 할 그릇. */
		__le32 virt_val = 0;

		/* [한국어] 그림자 복사본의 현재 값을 꺼낸다. 허용되지 않은 비트를 보존하려면
		 * 통째로 덮어쓰는 대신 현재 값에서 시작해야 한다. */
		memcpy(&virt_val, vdev->vconfig + pos, count);

		/* [한국어] 갱신할 비트만 0 으로 비운다. */
		virt_val &= ~(write & virt);
		/* [한국어] 비운 자리에 사용자 값의 같은 비트를 얹는다. 나머지 비트는 그대로 남는다. */
		virt_val |= (val & (write & virt));

		/* [한국어] 완성된 값을 그림자 복사본에 되쓴다. */
		memcpy(vdev->vconfig + pos, &virt_val, count);
	}

	/* Non-virtualized and writable bits go to hardware */
	/* [한국어] 쓰기 허용이면서 가상화되지 않은 비트가 있으면 그 비트는 하드웨어로
	 * 간다. 3분류의 (1)통과 갈래다. */
	if (write & ~virt) {
		/* [한국어] 하드웨어 접근용 핸들. */
		struct pci_dev *pdev = vdev->pdev;
		/* [한국어] 하드웨어의 현재 값을 담을 그릇. */
		__le32 phys_val = 0;
		/* [한국어] 하드웨어 접근 결과 코드. */
		int ret;

		/* [한국어] 하드웨어를 먼저 읽는다. 허용되지 않은 비트를 사용자 값으로 덮어쓰면
		 * 안 되므로 반드시 읽기-수정-쓰기여야 한다. 예컨대 COMMAND 를 통째로 쓰면
		 * 사용자가 건드릴 수 없어야 할 상위 비트까지 바뀐다. */
		ret = vfio_user_config_read(pdev, pos, &phys_val, count);
		if (ret)
			return ret;

		/* [한국어] 하드웨어 값에서 갱신할 비트만 비운다. */
		phys_val &= ~(write & ~virt);
		/* [한국어] 비운 자리에 사용자 값의 같은 비트를 얹는다. */
		phys_val |= (val & (write & ~virt));

		/* [한국어] 완성된 값을 실제 하드웨어에 쓴다. 여기서 잠들 수 있다. 읽기와 쓰기
		 * 사이에 락이 없어 같은 레지스터에 대한 동시 쓰기는 서로를 잃을 수 있으나,
		 * 호스트가 지키는 값은 애초에 가상화돼 이 경로로 오지 않는다. */
		ret = vfio_user_config_write(pdev, pos, phys_val, count);
		if (ret)
			return ret;
	}

	/* [한국어] 쓴 바이트 수를 돌려준다. */
	return count;
}

/* [한국어]
 * vfio_direct_config_read - 하드웨어에서 그대로 읽되 capability 헤더의 next 포인터만 vconfig 값으로 바꿔치기한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 읽을 바이트 수.
 * @perm: 권한 표. 이 함수는 비트맵을 쓰지 않으므로 실제로는 참조하지 않는다.
 *        readfn 시그니처를 맞추기 위한 인자다.
 * @offset: capability 안에서의 상대 오프셋. 헤더 영역인지 판별하는 데 쓴다.
 * @val: 결과를 담을 곳.
 * @return: 성공하면 count, 하드웨어 읽기 실패면 그 오류.
 *
 * 왜 필요한가: cap_perms 와 ecap_perms 의 기본값이 바로 이 함수다. 커널이
 * 특별히 다룰 이유가 없는 capability 는 "읽기는 하드웨어 직통, 쓰기는 전면
 * 거부" 로 두는데, 그때도 딱 하나는 손봐야 한다 — capability chain 의 next
 * 포인터다. 이 파일이 감춘 capability 를 건너뛰도록 chain 을 다시 엮어 놓았기
 * 때문에, next 포인터만은 반드시 vconfig 의 재작성된 값을 보여 줘야 한다.
 * 그러지 않으면 사용자가 감춘 capability 를 다시 찾아낸다.
 *
 * 동작 과정:
 *  1. 하드웨어에서 count 바이트를 실제로 읽는다.
 *  2. pos 가 확장 config 영역(256바이트 이상)이면 확장 capability 헤더를
 *     다룬다. 확장 헤더는 첫 dword 안에 ID, 버전, next 오프셋이 모두 들어
 *     있으므로, offset 이 4 미만이면 그 부분 전체를 vconfig 값으로 덮는다.
 *  3. pos 가 표준 헤더(64바이트) 뒤의 표준 capability 영역이면 바이트 배치가
 *     다르다. 표준 capability 는 0번 바이트가 ID, 1번 바이트가 next 다.
 *       - offset 이 ID 자리이고 2바이트 이상 읽는 중이면, ID 와 next 를
 *         함께 읽는 셈이므로 두 바이트를 vconfig 에서 가져온다.
 *         min 으로 폭을 제한해 필요 이상 덮지 않는다.
 *       - offset 이 next 자리면 그 1바이트만 vconfig 에서 가져온다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 1단계에서 잠들 수 있다.
 *
 * 에러 경로: 하드웨어 읽기 실패면 덮어쓰기 없이 즉시 오류를 반환한다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->readfn(cap_perms/ecap_perms 의 기본값,
 *     또는 direct_ro_perms) → [vfio_direct_config_read] → vfio_user_config_read
 */
/* Allow direct read from hardware, except for capability next pointer */
static int vfio_direct_config_read(struct vfio_pci_core_device *vdev, int pos,
				   int count, struct perm_bits *perm,
				   int offset, __le32 *val)
{
	/* [한국어] 하드웨어 읽기 결과 코드. */
	int ret;

	/* [한국어] 하드웨어 값을 그대로 읽는다. 이 표에는 virt 비트맵이 없으므로 섞을
	 * 것도 없다. */
	ret = vfio_user_config_read(vdev->pdev, pos, val, count);
	if (ret)
		return ret;

	/* [한국어] 확장 config 영역(256바이트 이상)이면 확장 capability 헤더 규칙을 쓴다.
	 * 확장 헤더는 ID, 버전, next 오프셋이 첫 dword 안에 함께 들어 있다. */
	if (pos >= PCI_CFG_SPACE_SIZE) { /* Extended cap header mangling */
		/* [한국어] 이번 읽기가 그 첫 dword 안이면 chain 재작성 결과를 보여 줘야 한다. */
		if (offset < 4)
			/* [한국어] 헤더 부분을 vconfig 값으로 통째로 갈아끼운다. vfio_ecap_init 이 감춘
			 * capability 를 건너뛰도록 next 를 고쳐 두었고, 껍데기 자리라면 ID 와 버전도
			 * 0 으로 지워 두었다. */
			memcpy(val, vdev->vconfig + pos, count);
	/* [한국어] 표준 헤더(64바이트) 뒤의 표준 capability 영역. 여기는 바이트 배치가
	 * 달라 ID 1바이트 + next 1바이트 구조다. */
	} else if (pos >= PCI_STD_HEADER_SIZEOF) { /* Std cap mangling */
		/* [한국어] capability 의 첫 바이트(ID)부터 2바이트 이상 읽는 중이면 next 포인터도
		 * 함께 읽는 셈이다. */
		if (offset == PCI_CAP_LIST_ID && count > 1)
			/* [한국어] ID 와 next 두 바이트를 vconfig 에서 가져온다. min 으로 폭을 제한해
			 * 그 뒤의 capability 내용까지 덮지 않게 한다 — 내용은 하드웨어 값이 맞다. */
			memcpy(val, vdev->vconfig + pos,
			       min(PCI_CAP_FLAGS, count));
		/* [한국어] next 포인터 바이트만 읽는 경우. */
		else if (offset == PCI_CAP_LIST_NEXT)
			/* [한국어] 그 1바이트만 재작성된 값으로 바꿔치기한다. 이 한 줄이 없으면 사용자가
			 * 감춘 capability 를 chain 을 따라가며 다시 찾아낸다. */
			memcpy(val, vdev->vconfig + pos, 1);
	}

	/* [한국어] 읽은 바이트 수. */
	return count;
}

/* [한국어]
 * vfio_raw_config_write - 아무 검사 없이 사용자 값을 하드웨어에 그대로 쓴다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 쓸 바이트 수.
 * @perm: 쓰지 않는다(시그니처 맞춤용).
 * @offset: 쓰지 않는다(시그니처 맞춤용).
 * @val: 사용자가 쓴 값.
 * @return: 성공하면 count, 실패하면 하드웨어 오류.
 *
 * 왜 필요한가: 두 곳에서 쓰인다. (a) unassigned_perms — capability 지도에
 * 아무것도 없는 빈틈. be2net 처럼 capability 사이 공간에 진짜 레지스터를
 * 숨겨 둔 디바이스가 있어 막을 수 없다. (b) Vendor-Specific capability
 * (표준과 확장 양쪽)와 DVSEC — 내용이 벤더 정의라 커널이 의미를 알 수 없다.
 * 두 경우 모두 상류 주석이 밝히듯 "MMIO 나 I/O 포트와 마찬가지로 하드웨어
 * 격리를 믿는 수밖에" 없다. 이 디바이스는 이미 IOMMU 도메인에 갇혀 있으므로,
 * 자기 자신을 어떻게 망가뜨리든 호스트에는 닿지 않는다는 것이 근거다.
 *
 * 동작 과정: 폭에 맞는 config 쓰기를 한 번 하고 count 를 반환한다. vconfig 도
 * 비트맵도 건드리지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 에러 경로: 하드웨어 오류를 그대로 위로 올린다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(unassigned_perms 또는
 *     vfio_pci_init_perm_bits 가 VNDR/DVSEC 슬롯에 심어 둔 값)
 *     → [vfio_raw_config_write] → vfio_user_config_write
 */
/* Raw access skips any kind of virtualization */
static int vfio_raw_config_write(struct vfio_pci_core_device *vdev, int pos,
				 int count, struct perm_bits *perm,
				 int offset, __le32 val)
{
	/* [한국어] 하드웨어 쓰기 결과 코드. */
	int ret;

	/* [한국어] 검사 없이 사용자 값을 그대로 하드웨어에 쓴다. capability 사이 빈틈과
	 * Vendor-Specific 영역에서만 쓰이며, 그 안전 근거는 IOMMU 격리다. */
	ret = vfio_user_config_write(vdev->pdev, pos, val, count);
	if (ret)
		return ret;

	/* [한국어] 쓴 바이트 수. */
	return count;
}

/* [한국어]
 * vfio_raw_config_read - 아무 가공 없이 하드웨어 값을 그대로 읽어 준다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 읽을 바이트 수.
 * @perm: 쓰지 않는다(시그니처 맞춤용).
 * @offset: 쓰지 않는다(시그니처 맞춤용).
 * @val: 결과를 담을 곳.
 * @return: 성공하면 count, 실패하면 하드웨어 오류.
 *
 * 왜 필요한가: vfio_raw_config_write 의 짝이다. unassigned_perms 의 readfn 이
 * 이것이다. vfio_direct_config_read 와 다른 점은 capability 헤더 바꿔치기를
 * 전혀 하지 않는다는 것 — 애초에 capability 가 아닌 빈틈이므로 헤더가 없다.
 *
 * 동작 과정: 폭에 맞는 config 읽기를 한 번 하고 count 를 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 *
 * 에러 경로: 하드웨어 오류를 그대로 반환한다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->readfn(unassigned_perms)
 *     → [vfio_raw_config_read] → vfio_user_config_read
 */
static int vfio_raw_config_read(struct vfio_pci_core_device *vdev, int pos,
				int count, struct perm_bits *perm,
				int offset, __le32 *val)
{
	/* [한국어] 하드웨어 읽기 결과 코드. */
	int ret;

	/* [한국어] 가공 없이 하드웨어 값을 읽는다. capability 헤더 바꿔치기도 하지 않는데,
	 * 애초에 capability 가 아닌 빈틈이라 헤더가 없기 때문이다. */
	ret = vfio_user_config_read(vdev->pdev, pos, val, count);
	if (ret)
		return ret;

	/* [한국어] 읽은 바이트 수. */
	return count;
}

/* [한국어]
 * vfio_virt_config_write - 하드웨어를 완전히 무시하고 vconfig 에만 기록한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋(vconfig 인덱스).
 * @count: 쓸 바이트 수.
 * @perm: 쓰지 않는다(시그니처 맞춤용).
 * @offset: 쓰지 않는다(시그니처 맞춤용).
 * @val: 사용자가 쓴 값.
 * @return: 항상 count. 실패할 수 없다.
 *
 * 왜 필요한가: virt_perms 의 writefn 이며, pci_config_map 이
 * PCI_CAP_ID_INVALID_VIRT(0xFE) 로 칠해진 자리에 쓰인다. 지금 그 값이 칠해지는
 * 곳은 vfio_ecap_init 이 확장 capability chain 의 맨 앞에 있는 정체 모를
 * capability 를 감출 때 만드는 자리다. 그 자리는 chain 의 닻 역할만 하고
 * 내용은 통째로 지어낸 것이므로, 하드웨어와 아무 관계가 없어야 한다.
 *
 * 동작 과정: 사용자 값을 vconfig 에 memcpy 하고 끝. 하드웨어 접근이 없어
 * 잠들지도, 실패하지도 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 메모리 복사뿐이라 어디서 불려도 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(virt_perms)
 *     → [vfio_virt_config_write]
 */
/* Virt access uses only virtualization */
static int vfio_virt_config_write(struct vfio_pci_core_device *vdev, int pos,
				  int count, struct perm_bits *perm,
				  int offset, __le32 val)
{
	/* [한국어] 하드웨어를 완전히 건너뛰고 그림자 복사본에만 기록한다. 이 자리는
	 * vfio_ecap_init 이 만든 껍데기라 대응하는 하드웨어 레지스터가 의미를 갖지
	 * 않는다. */
	memcpy(vdev->vconfig + pos, &val, count);
	/* [한국어] 항상 성공. 실패할 수 있는 연산이 없다. */
	return count;
}

/* [한국어]
 * vfio_virt_config_read - 하드웨어를 보지 않고 vconfig 값만 돌려준다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋(vconfig 인덱스).
 * @count: 읽을 바이트 수.
 * @perm: 쓰지 않는다(시그니처 맞춤용).
 * @offset: 쓰지 않는다(시그니처 맞춤용).
 * @val: 결과를 담을 곳.
 * @return: 항상 count.
 *
 * 왜 필요한가: vfio_virt_config_write 의 짝. 완전 가상 영역이므로 읽기도
 * 그림자 복사본에서만 온다. 사용자는 자기가 쓴 값을 그대로 되읽는다.
 *
 * 동작 과정: vconfig 에서 count 바이트를 val 로 memcpy 하고 끝.
 *
 * 실행 컨텍스트: 프로세스 문맥. 실패도 대기도 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->readfn(virt_perms)
 *     → [vfio_virt_config_read]
 */
static int vfio_virt_config_read(struct vfio_pci_core_device *vdev, int pos,
				 int count, struct perm_bits *perm,
				 int offset, __le32 *val)
{
	/* [한국어] 그림자 복사본에서만 읽는다. 사용자는 자기가 쓴 값을 그대로 되읽는다. */
	memcpy(val, vdev->vconfig + pos, count);
	/* [한국어] 항상 성공. */
	return count;
}

/* [한국어] "정체 모를 확장 capability" 전용 표.
 * 설정자: 컴파일 시 지정 초기화. 런타임에 바뀌지 않는다.
 * 읽는 자: vfio_pci_config_rw_single 이 확장 영역에서 표 범위를 넘는 ID 를
 * 만났을 때 이 표를 쓴다. 그런 ID 는 vfio_ecap_init 이 chain 첫머리의 모르는
 * capability 를 감출 때 지도에 남긴다.
 * 값 범위: readfn 만 채워지고 writefn 은 NULL 이므로 읽기 전용이다. virt/write
 * 비트맵도 없어 vfio_direct_config_read 만 쓸 수 있다.
 * 동기화: 읽기 전용이라 필요 없다. */
static struct perm_bits direct_ro_perms = {
	/* [한국어] 읽기는 하드웨어 직통이되 확장 capability 헤더의 첫 dword 만 vconfig 로
	 * 바꿔치기한다. 이 슬롯이 존재하는 이유가 바로 그 바꿔치기다 — 감춘
	 * capability 를 건너뛰는 next 값을 사용자에게 보여 줘야 한다. */
	.readfn = vfio_direct_config_read,
};

/* Default capability regions to read-only, no-virtualization */
/* [한국어] 표준 capability ID 별 권한 표 배열.
 * 설정자: 아래 지정 초기화로 전 슬롯이 같은 기본값을 받고, 모듈 적재 시
 * vfio_pci_init_perm_bits 가 여덟 슬롯을 실제 표로 채우거나 writefn 만 바꾼다.
 * 읽는 자: vfio_pci_config_rw_single 이 지도에서 얻은 ID 로 색인한다.
 * 값 범위: 슬롯 수는 표준 capability ID 최댓값 + 1. 채워지지 않은 슬롯은
 * readfn 만 있는 읽기 전용 표다.
 * 동기화: 초기화 뒤로는 읽기 전용이며 모든 디바이스가 공유한다. 이것이
 * 디바이스마다 표를 만들지 않는 메모리 절약의 핵심이다. */
static struct perm_bits cap_perms[PCI_CAP_ID_MAX + 1] = {
	/* [한국어] GCC 의 범위 지정 초기화로 전 슬롯을 한 번에 채운다. 기본값의 의미는
	 * 상류 주석대로 "읽기 전용, 가상화 없음" 이다 — readfn 만 있고 writefn 이
	 * NULL 이라 쓰기는 조용히 무시되며, virt/write 비트맵이 없어 비트 단위 판정도
	 * 없다. 이 기본값 덕분에 커널이 모르는 capability 는 자동으로 안전해진다. */
	[0 ... PCI_CAP_ID_MAX] = { .readfn = vfio_direct_config_read }
};
/* [한국어] 확장 capability ID 별 권한 표 배열. 구성과 규칙은 cap_perms 와 같고
 * 슬롯 수만 확장 ID 최댓값 기준이다.
 * 설정자: 지정 초기화 + vfio_pci_init_perm_bits 가 AER, Power Budgeting,
 * 확장 Vendor-Specific, DVSEC 네 슬롯을 손본다.
 * 읽는 자: vfio_pci_config_rw_single.
 * 동기화: 초기화 뒤 읽기 전용, 전 디바이스 공유. */
static struct perm_bits ecap_perms[PCI_EXT_CAP_ID_MAX + 1] = {
	/* [한국어] 확장 capability 도 기본은 읽기 전용이다. vfio_direct_config_read 를 쓰는
	 * 것이 특히 중요한데, 확장 헤더의 첫 dword 에 next 오프셋이 들어 있어
	 * chain 재작성 결과를 반드시 보여 줘야 하기 때문이다. */
	[0 ... PCI_EXT_CAP_ID_MAX] = { .readfn = vfio_direct_config_read }
};
/*
 * Default unassigned regions to raw read-write access.  Some devices
 * require this to function as they hide registers between the gaps in
 * config space (be2net).  Like MMIO and I/O port registers, we have
 * to trust the hardware isolation.
 */
/* [한국어] capability 지도에 아무것도 없는 빈틈 전용 표.
 * 설정자: 컴파일 시 지정 초기화.
 * 읽는 자: vfio_pci_config_rw_single 이 지도에서 PCI_CAP_ID_INVALID 를 만났을 때.
 * 값 범위: readfn 과 writefn 이 모두 raw 판이라 읽기도 쓰기도 하드웨어로
 * 그대로 간다. 비트맵은 없다.
 * 동기화: 읽기 전용.
 * 상류 주석이 이 선택의 근거를 밝힌다 — be2net 같은 디바이스가 capability
 * 사이 빈 공간에 진짜 레지스터를 숨겨 두기 때문에 막을 수 없고, MMIO 나
 * I/O 포트와 마찬가지로 하드웨어 격리를 믿는 수밖에 없다. */
static struct perm_bits unassigned_perms = {
	/* [한국어] 빈틈 읽기는 가공 없이 하드웨어 값 그대로. capability 헤더가 아니므로
	 * 바꿔치기할 것도 없다. */
	.readfn = vfio_raw_config_read,
	/* [한국어] 빈틈 쓰기도 검사 없이 하드웨어로 간다. 이 파일에서 3분류의 예외에
	 * 해당하는 두 자리 중 하나다. */
	.writefn = vfio_raw_config_write
};

/* [한국어] 감춘 확장 capability 의 껍데기 자리 전용 표.
 * 설정자: 컴파일 시 지정 초기화.
 * 읽는 자: vfio_pci_config_rw_single 이 지도에서 PCI_CAP_ID_INVALID_VIRT 를
 * 만났을 때. 그 값을 지도에 칠하는 코드는 이 트리에 없고, 지금은
 * vfio_ecap_init 이 껍데기 자리를 만들 때 실제 ecap ID 로 칠한 뒤 vconfig
 * 쪽만 지우는 방식을 쓴다. 이 표는 그런 자리를 명시적으로 표시하고 싶을 때를
 * 위한 준비물이다.
 * 값 범위: 읽기도 쓰기도 vconfig 에서만 이뤄진다. 하드웨어와 완전히 절연된다.
 * 동기화: 읽기 전용. */
static struct perm_bits virt_perms = {
	/* [한국어] 읽기는 그림자 복사본에서만. */
	.readfn = vfio_virt_config_read,
	/* [한국어] 쓰기도 그림자 복사본에만. 사용자는 자기 값을 되읽지만 하드웨어는
	 * 전혀 움직이지 않는다. */
	.writefn = vfio_virt_config_write
};

/* [한국어]
 * free_perm_bits - 권한 표의 두 비트맵을 해제하고 포인터를 비운다
 *
 * @perm: 해제할 권한 표. 공유 표(cap_perms/ecap_perms 의 원소)일 수도 있고
 *        디바이스별 msi_perm 일 수도 있다. 구조체 자체는 해제하지 않는다.
 * @return: 없다.
 *
 * 왜 필요한가: alloc_perm_bits 가 kzalloc 두 번으로 만든 virt/write 비트맵을
 * 되돌린다. 포인터를 NULL 로 비우는 것이 중요한데, 이 함수는 alloc_perm_bits
 * 의 실패 롤백에서도 불리기 때문이다 — 둘 중 하나만 성공한 상태에서 불려도
 * kfree(NULL) 은 무해하고, 이후 다시 시도하거나 그냥 두어도 이중 해제가
 * 생기지 않는다.
 *
 * 동작 과정: virt 와 write 를 kfree 하고 둘 다 NULL 로 만든다. readfn 과
 * writefn 은 건드리지 않는다 — 공유 표는 모듈 수명 내내 살아 있고 다시
 * 초기화될 수 있기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 모듈 언로드 시점(vfio_pci_uninit_perm_bits),
 * 초기화 실패 롤백(alloc_perm_bits, vfio_pci_init_perm_bits),
 * 디바이스 닫기(vfio_config_free) 세 갈래에서 불린다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   alloc_perm_bits / vfio_pci_uninit_perm_bits / vfio_config_free
 *     → [free_perm_bits] → kfree
 */
static void free_perm_bits(struct perm_bits *perm)
{
	/* [한국어] 가상화 비트맵 해제. NULL 이어도 안전하다 — alloc_perm_bits 의 부분 실패
	 * 롤백에서 그런 상태로 불린다. */
	kfree(perm->virt);
	/* [한국어] 쓰기 허용 비트맵 해제. 같은 이유로 NULL 안전. */
	kfree(perm->write);
	/* [한국어] 포인터를 비워 이중 해제와 해제 후 사용을 막는다. 공유 표는 모듈 수명
	 * 내내 살아 있고 초기화 실패 뒤 다시 정리될 수 있어 이 초기화가 중요하다. */
	perm->virt = NULL;
	/* [한국어] 같은 이유로 비운다. */
	perm->write = NULL;
}

/* [한국어]
 * alloc_perm_bits - 권한 표의 두 비트맵을 0 으로 채워 만들고 기본 콜백을 심는다
 *
 * @perm: 채울 권한 표.
 * @size: 이 capability 의 바이트 길이. dword 로 올림된다.
 * @return: 0 성공, -ENOMEM 이면 둘 중 하나라도 할당 실패(이미 롤백됨).
 *          호출자인 init_pci_cap_ 계열은 이 값을 그대로 위로 올린다.
 *
 * 왜 필요한가: 이 파일의 안전 기본값을 정하는 함수다. 두 비트맵을 kzalloc 으로
 * 0 초기화한다는 것은 곧 "모두 읽기 가능, 아무것도 쓰기 불가, 아무것도 가상화
 * 안 함" 이라는 뜻이다. 즉 명시적으로 뚫지 않은 모든 비트는 자동으로 3분류의
 * (3)거부가 된다. 이후 p_setb/p_setw/p_setd 가 예외를 하나씩 뚫는다.
 *
 * 크기를 dword 로 올림하는 이유(상류 주석에 근거가 있다): 표준 config 헤더도,
 * 표준 capability 도(next 포인터의 하위 2비트가 예약이라 4바이트 정렬),
 * 확장 capability 도 모두 dword 정렬이다. 그래서 표를 dword 단위로 만들어
 * 두면 읽기/쓰기가 capability 경계를 살짝 넘어도 비트맵 밖으로 나가지 않는다.
 *
 * 동작 과정:
 *  1. size 를 4의 배수로 올린다.
 *  2. virt 와 write 를 각각 kzalloc 한다.
 *  3. 둘 중 하나라도 실패하면 free_perm_bits 로 되돌리고 -ENOMEM.
 *  4. readfn/writefn 에 기본 엔진(vfio_default_config_read/_write)을 심는다.
 *     호출자가 필요하면 이후에 덮어쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GFP_KERNEL 이라 잠들 수 있다. 대부분 모듈
 * 적재 시점(__init)이지만 init_pci_cap_msi_perm 을 통해 디바이스 열기
 * 시점에도 불린다.
 *
 * 에러 경로: 부분 할당 상태를 free_perm_bits 가 정리하므로 호출자는 아무것도
 * 되돌릴 필요가 없다.
 *
 * 호출 체인:
 *   init_pci_cap_basic_perm / _pm_perm / _vpd_perm / _pcix_perm / _exp_perm /
 *   _af_perm / init_pci_ext_cap_err_perm / _pwr_perm / init_pci_cap_msi_perm
 *     → [alloc_perm_bits] → kzalloc, free_perm_bits
 */
static int alloc_perm_bits(struct perm_bits *perm, int size)
{
	/*
	 * Round up all permission bits to the next dword, this lets us
	 * ignore whether a read/write exceeds the defined capability
	 * structure.  We can do this because:
	 *  - Standard config space is already dword aligned
	 *  - Capabilities are all dword aligned (bits 0:1 of next reserved)
	 *  - Express capabilities defined as dword aligned
	 */
	/* [한국어] 길이를 dword 로 올린다. 상류 주석이 근거 세 가지를 든다 — 표준 config
	 * 헤더가 이미 dword 정렬이고, 표준 capability 도 next 포인터의 하위 2비트가
	 * 예약이라 dword 정렬이며, 확장 capability 도 dword 정렬이다. 덕분에 접근이
	 * capability 경계를 살짝 넘어도 비트맵 밖을 읽지 않는다. */
	size = round_up(size, 4);

	/*
	 * Zero state is
	 * - All Readable, None Writeable, None Virtualized
	 */
	/* [한국어] 0 초기화 할당이 곧 "가상화 없음" 기본값이다. 명시적으로 뚫지 않은
	 * 비트는 전부 하드웨어 직통이 된다. */
	perm->virt = kzalloc(size, GFP_KERNEL);
	/* [한국어] 0 초기화가 곧 "쓰기 전면 거부" 기본값이다. 이 파일의 안전 설계가
	 * 여기서 시작된다 — 허용은 예외이고 거부가 기본이다. */
	perm->write = kzalloc(size, GFP_KERNEL);
	/* [한국어] 둘 중 하나라도 실패하면 부분 상태를 남기지 않는다. */
	if (!perm->virt || !perm->write) {
		/* [한국어] 성공한 쪽만 해제하고 두 포인터를 NULL 로 만든다. 호출자는 아무것도
		 * 되돌릴 필요가 없다. */
		free_perm_bits(perm);
		return -ENOMEM;
	}

	/* [한국어] 기본 읽기 엔진을 심는다. 비트맵을 쓰는 표이므로 direct 판이 아니라
	 * default 판이어야 한다. 호출자가 필요하면 곧바로 덮어쓴다. */
	perm->readfn = vfio_default_config_read;
	/* [한국어] 기본 쓰기 엔진을 심는다. 여기서 writefn 이 NULL 이 아니게 되므로,
	 * 이 함수를 거친 표는 "쓰기 요청이 콜백까지 도달하되 write 비트맵이 0 이면
	 * 조용히 무시" 하는 동작을 하게 된다. */
	perm->writefn = vfio_default_config_write;

	/* [한국어] 두 비트맵 준비 완료. */
	return 0;
}

/* [한국어]
 * p_setb - 권한 표의 한 바이트에 virt/write 비트를 기록한다
 *
 * @p: 대상 권한 표.
 * @off: capability 안에서의 바이트 오프셋.
 * @virt: 이 바이트에서 가상화할 비트들(1 = vconfig 에서 읽고 쓴다).
 * @write: 이 바이트에서 사용자에게 쓰기를 허용할 비트들(1 = 허용).
 * @return: 없다.
 *
 * 왜 필요한가: 표를 채우는 세 도구(p_setb/p_setw/p_setd) 중 1바이트짜리다.
 * 1바이트는 엔디언 변환이 필요 없으므로 그냥 대입한다.
 *
 * 동작 과정: virt 비트맵과 write 비트맵의 같은 오프셋에 각각 대입한다.
 * 읽기-수정-쓰기가 아니라 통째 대입이므로, 같은 오프셋에 두 번 호출하면
 * 나중 값이 앞 값을 지운다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 표가 아직 아무에게도 노출되지 않은
 * 초기화 시점에만 불리므로 동기화가 없다.
 *
 * 에러 경로: 없다. off 범위 검사도 없으니 호출자가 alloc_perm_bits 에 준
 * size 안이어야 한다 — 그 size 가 dword 로 올림돼 있어 여유가 있다.
 *
 * 호출 체인:
 *   init_pci_cap_ 계열 / init_pci_ext_cap_ 계열 → [p_setb]
 */
/*
 * Helper functions for filling in permission tables
 */
static inline void p_setb(struct perm_bits *p, int off, u8 virt, u8 write)
{
	/* [한국어] 1바이트라 엔디언 변환이 필요 없다. 통째 대입이므로 같은 자리에 두 번
	 * 호출하면 앞 설정이 지워진다. */
	p->virt[off] = virt;
	/* [한국어] 쓰기 허용 비트도 같은 방식으로 대입한다. */
	p->write[off] = write;
}

/* [한국어]
 * p_setw - 권한 표의 2바이트에 virt/write 비트를 리틀엔디언으로 기록한다
 *
 * @p: 대상 권한 표.
 * @off: capability 안에서의 바이트 오프셋. 2바이트 정렬이어야 한다.
 * @virt: 가상화 비트 16개. 호출자는 CPU 표현으로 준다.
 * @write: 쓰기 허용 비트 16개. 역시 CPU 표현.
 * @return: 없다.
 *
 * 왜 필요한가: PCI config space 는 스펙상 리틀엔디언이고, 이 파일의 비트맵도
 * 그 순서로 저장돼 하드웨어 값과 memcpy 로 겹쳐 쓴다. 그래서 호출자가 쓰는
 * PCI_COMMAND_INTX_DISABLE 같은 CPU 표현 상수를 반드시 리틀엔디언으로
 * 변환해 넣어야 한다. 상류 주석이 "Handle endian-ness" 라고 적어 둔 이유다.
 * 빅엔디언 호스트에서 이 변환이 없으면 마스크가 엉뚱한 바이트에 걸린다.
 *
 * 동작 과정: 두 비트맵의 off 위치를 __le16 포인터로 보고 cpu_to_le16 한 값을
 * 대입한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 초기화 시점 전용이라 동기화가 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   init_pci_cap_ 계열 → [p_setw]
 */
/* Handle endian-ness - pci and tables are little-endian */
static inline void p_setw(struct perm_bits *p, int off, u16 virt, u16 write)
{
	/* [한국어] 비트맵의 해당 위치를 __le16 로 보고 리틀엔디언으로 변환해 넣는다.
	 * 호출자는 PCI_COMMAND_INTX_DISABLE 같은 CPU 표현 상수를 넘기므로 변환이
	 * 반드시 필요하다. 이 비트맵은 나중에 하드웨어 값과 memcpy 로 겹쳐지므로
	 * 바이트 순서가 PCI 와 같아야 한다. */
	*(__le16 *)(&p->virt[off]) = cpu_to_le16(virt);
	/* [한국어] 쓰기 허용 마스크도 같은 변환을 거친다. */
	*(__le16 *)(&p->write[off]) = cpu_to_le16(write);
}

/* [한국어]
 * p_setd - 권한 표의 4바이트에 virt/write 비트를 리틀엔디언으로 기록한다
 *
 * @p: 대상 권한 표.
 * @off: capability 안에서의 바이트 오프셋. 4바이트 정렬이어야 한다.
 * @virt: 가상화 비트 32개(CPU 표현).
 * @write: 쓰기 허용 비트 32개(CPU 표현).
 * @return: 없다.
 *
 * 왜 필요한가: p_setw 와 같은 이유의 4바이트판이다. BAR 하나, AER 의 상태/
 * 마스크 레지스터, PM 제어 레지스터처럼 dword 단위로 다뤄야 하는 필드에 쓴다.
 *
 * 동작 과정: 두 비트맵의 off 위치를 __le32 포인터로 보고 cpu_to_le32 값을
 * 대입한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 초기화 시점 전용.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   init_pci_cap_ 계열 / init_pci_ext_cap_ 계열 → [p_setd]
 */
/* Handle endian-ness - pci and tables are little-endian */
static inline void p_setd(struct perm_bits *p, int off, u32 virt, u32 write)
{
	/* [한국어] 4바이트판. BAR 하나, AER 상태/마스크 레지스터, PM 제어 레지스터처럼
	 * dword 단위로 다루는 필드에 쓴다. */
	*(__le32 *)(&p->virt[off]) = cpu_to_le32(virt);
	/* [한국어] 쓰기 허용 마스크의 4바이트판. */
	*(__le32 *)(&p->write[off]) = cpu_to_le32(write);
}

/* [한국어]
 * __vfio_pci_memory_enabled - 지금 이 디바이스의 MMIO 창을 사용자에게 열어 줘도 되는지 판정한다
 *
 * @vdev: 대상 디바이스. vconfig 의 COMMAND 레지스터 사본과 pdev 를 본다.
 * @return: true 면 MMIO 접근을 허용해도 된다. false 면 막아야 한다.
 *          호출자들은 false 일 때 BAR 매핑을 걷어내거나 페이지 폴트를
 *          SIGBUS 로 돌린다.
 *
 * 왜 필요한가: 사용자가 COMMAND 의 Memory Space Enable 을 끄거나 디바이스를
 * D3 로 내려놓으면, 그 상태에서 BAR 에 접근하는 것은 정의되지 않은 동작이고
 * 플랫폼에 따라 머신 체크로 호스트를 죽일 수 있다. 그래서 VFIO 는 그 두
 * 조건이 깨지는 순간 사용자 매핑을 무효화하고, 다시 접근하려 하면 이
 * 함수로 판정해 막는다. 이 파일 밖에서도 쓰이므로 EXPORT_SYMBOL_GPL 로 나간다.
 *
 * 동작 과정:
 *  1. vconfig 의 COMMAND 사본을 리틀엔디언에서 CPU 표현으로 읽는다.
 *     하드웨어가 아니라 vconfig 를 보는 이유는, 사용자가 "켰다" 고 믿는
 *     상태가 곧 사용자에게 허용해야 할 상태이기 때문이다.
 *  2. 전원 상태가 D3hot 이상이면 무조건 false. D3 에서는 메모리 영역에
 *     접근할 수 없다.
 *  3. no_command_memory 가 켜진 디바이스(SR-IOV VF 등)는 COMMAND 에 Memory
 *     Space Enable 비트가 아예 없다. VF 의 메모리 디코딩은 PF 의 SR-IOV
 *     Control 레지스터 MSE 비트가 일괄 제어하므로(drivers/pci/iov.c:883 에서
 *     이 플래그를 세운다) 가상 비트로 막을 이유가 없다 — 그래서 통과시킨다.
 *  4. 그 밖에는 vconfig 의 Memory Space Enable 비트를 그대로 따른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 상류 주석대로 호출자가 memory_lock 을 쥔
 * 상태여야 한다 — 이 함수 자체는 락을 잡지 않고 검사만 한다. 이 파일 안에서는
 * vfio_basic_config_write, vfio_lock_and_set_power_state, vfio_exp_config_write,
 * vfio_af_config_write 가 모두 memory_lock 을 쓰기 모드로 쥔 채 부른다.
 *
 * 에러 경로: 없다. 판정만 한다.
 *
 * 호출 체인:
 *   vfio_basic_config_write / vfio_lock_and_set_power_state /
 *   vfio_exp_config_write / vfio_af_config_write (이 파일),
 *   그리고 vfio_pci_core.c 의 PM/리셋/폴트 경로 → [__vfio_pci_memory_enabled]
 */
/* Caller should hold memory_lock semaphore */
bool __vfio_pci_memory_enabled(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 전원 상태를 볼 PCI 함수 핸들. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 그림자 복사본의 COMMAND 값을 CPU 표현으로 읽는다. 하드웨어가 아니라
	 * vconfig 를 보는 것이 핵심이다 — 사용자가 "켰다" 고 믿는 상태가 곧 사용자
	 * 매핑을 허용해야 할 상태이기 때문이다. 하드웨어를 보면 뒷문 리셋 직후
	 * 어긋난 값을 보게 된다. */
	u16 cmd = le16_to_cpu(*(__le16 *)&vdev->vconfig[PCI_COMMAND]);

	/*
	 * Memory region cannot be accessed if device power state is D3.
	 *
	 * SR-IOV VF memory enable is handled by the MSE bit in the
	 * PF SR-IOV capability, there's therefore no need to trigger
	 * faults based on the virtual value.
	 */
	/* [한국어] 세 조건의 결합. (a) 전원이 D3hot 미만이어야 한다 — D3 에서는 메모리
	 * 영역 접근이 정의되지 않고 플랫폼에 따라 머신 체크가 난다.
	 * (b) no_command_memory 디바이스는 Memory Space Enable 비트가 물리적으로
	 * 없으므로 무조건 허용한다. 상류 주석대로 VF 의 메모리 활성화는 PF 의
	 * SR-IOV capability MSE 비트가 담당하니 가상 값으로 폴트를 낼 이유가 없다
	 * (그 플래그를 세우는 곳은 drivers/pci/iov.c:883).
	 * (c) 그 밖에는 vconfig 의 Memory Space Enable 비트를 따른다. */
	return pdev->current_state < PCI_D3hot &&
	       (pdev->no_command_memory || (cmd & PCI_COMMAND_MEMORY));
}
EXPORT_SYMBOL_GPL(__vfio_pci_memory_enabled);

/* [한국어]
 * vfio_bar_restore - FLR 이나 뒷문 리셋으로 날아간 진짜 BAR 값을 다시 써 넣는다
 *
 * @vdev: 대상 디바이스. rbar 배열(BAR 6개 + ROM BAR 의 원본 값)을 쓴다.
 * @return: 없다.
 *
 * 왜 필요한가: 사용자 공간 드라이버가 디바이스를 리셋하면 하드웨어의 BAR 가
 * 0 으로 돌아간다. VFIO 는 BAR 를 전부 가상화하므로 사용자는 자기가 쓴 값을
 * 계속 보지만, 실제 하드웨어에는 아무 주소도 없어 MMIO 가 죽는다. 게다가
 * VFIO 는 리셋 명령을 전부 가로채지도 못한다 — 벤더 고유의 "뒷문" 리셋
 * (디바이스 레지스터에 매직 값을 쓰는 방식 등)은 알아챌 수 없다. 그래서
 * COMMAND 쓰기 때마다 vfio_need_bar_restore 로 BAR 가 사라졌는지 확인하고,
 * 사라졌으면 이 함수로 호스트가 배정했던 원래 주소를 되살린다.
 *
 * 동작 과정:
 *  1. VF 는 그냥 돌아간다. VF 의 BAR 는 PF 의 SR-IOV capability 가 정하는
 *     것이라 VF 자신의 config space 에 쓸 수 있는 BAR 가 없다.
 *  2. 복구가 일어났다는 사실을 pci_info 로 남긴다. 정상 상황이 아니므로
 *     관리자가 볼 수 있어야 한다.
 *  3. BAR0 부터 BAR5 까지 4바이트씩 전진하며 rbar[0..5] 를 되쓴다. 루프가
 *     i 를 4씩 늘리는 동안 rbar 포인터도 한 칸씩 전진한다.
 *  4. 루프를 빠져나온 시점의 rbar 는 이미 여섯 번 전진해 rbar[6] 을 가리키고
 *     있다. 그 값이 ROM BAR 의 원본이므로 그대로 ROM 주소에 쓴다. 포인터
 *     산술이 곧 인덱스 역할을 하는 자리다.
 *  5. nointx 디바이스라면(INTx 가 고장 나 커널이 영구히 막아 둔 디바이스)
 *     리셋으로 풀려 버린 INTx Disable 비트를 다시 세운다. 이걸 빠뜨리면
 *     고장 난 INTx 라인이 호스트를 인터럽트 폭풍으로 몰아넣을 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자가 memory_lock 을 쓰기 모드로 쥐고 있다.
 * pci_user_ 계열이라 잠들 수 있다.
 *
 * 에러 경로: 반환값을 보지 않는다. 복구 자체가 이미 비정상 상황에 대한
 * 최선의 시도이고, 여기서 실패해도 더 할 수 있는 일이 없다.
 *
 * 호출 체인:
 *   vfio_basic_config_write → [vfio_bar_restore]
 *     → pci_user_write_config_dword / pci_user_read_config_word /
 *       pci_user_write_config_word (drivers/pci/access.c:796~801)
 */
/*
 * Restore the *real* BARs after we detect a FLR or backdoor reset.
 * (backdoor = some device specific technique that we didn't catch)
 */
static void vfio_bar_restore(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 복구 대상 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 기억해 둔 원본 BAR 값 7개(BAR0~5 와 ROM BAR)의 시작 포인터. 아래 루프가
	 * 이 포인터를 전진시키며 쓰고, 루프가 끝난 뒤의 위치가 곧 ROM BAR 항목이 된다. */
	u32 *rbar = vdev->rbar;
	/* [한국어] nointx 디바이스의 COMMAND 값을 잠시 담을 곳. */
	u16 cmd;
	/* [한국어] config 오프셋을 세는 루프 변수. */
	int i;

	/* [한국어] VF 는 자기 BAR 를 config 공간에 갖지 않는다. VF BAR 는 PF 의 SR-IOV
	 * capability 가 정하므로 여기서 되쓸 것이 없다. */
	if (pdev->is_virtfn)
		return;

	/* [한국어] 정상 상황이 아니므로 로그를 남긴다. 뒷문 리셋이 실제로 일어났다는
	 * 유일한 흔적이다. */
	pci_info(pdev, "%s: reset recovery - restoring BARs\n", __func__);

	/* [한국어] BAR0 부터 BAR5 까지 4바이트씩 전진한다. config 오프셋 i 와 배열
	 * 포인터 rbar 가 나란히 움직인다. */
	for (i = PCI_BASE_ADDRESS_0; i <= PCI_BASE_ADDRESS_5; i += 4, rbar++)
		/* [한국어] 기억해 둔 원본 주소를 하드웨어에 되쓴다. 반환값을 보지 않는데,
		 * 여기서 실패해도 더 할 수 있는 일이 없기 때문이다. */
		pci_user_write_config_dword(pdev, i, *rbar);

	/* [한국어] 루프를 빠져나온 rbar 는 이미 여섯 번 전진해 일곱 번째 항목(ROM BAR
	 * 원본)을 가리킨다. 포인터 산술이 곧 인덱스 역할을 하는 자리다. */
	pci_user_write_config_dword(pdev, PCI_ROM_ADDRESS, *rbar);

	/* [한국어] INTx 가 고장 나 커널이 영구히 막아 둔 디바이스라면, 리셋으로 풀려
	 * 버린 INTx Disable 비트를 다시 세워야 한다. */
	if (vdev->nointx) {
		/* [한국어] 현재 COMMAND 를 읽어 다른 비트를 보존한다. */
		pci_user_read_config_word(pdev, PCI_COMMAND, &cmd);
		/* [한국어] INTx Disable 비트를 세운다. 이 비트를 되살리지 않으면 고장 난 인터럽트
		 * 라인이 호스트를 인터럽트 폭풍으로 몰아넣을 수 있다. */
		cmd |= PCI_COMMAND_INTX_DISABLE;
		/* [한국어] 되쓴다. */
		pci_user_write_config_word(pdev, PCI_COMMAND, cmd);
	}
}

/* [한국어]
 * vfio_generate_bar_flags - BAR 의 하위 플래그 비트(공간 종류, 64비트 여부, prefetch)를 커널 자원 플래그에서 만들어 낸다
 *
 * @pdev: 대상 PCI 함수.
 * @bar: 자원 인덱스(PCI_STD_RESOURCES 기준의 절대 인덱스).
 * @return: BAR 하위 비트만 세운 리틀엔디언 dword. 주소 비트는 전부 0 이다.
 *
 * 왜 필요한가: BAR 는 상위 비트가 주소이고 하위 몇 비트가 속성 플래그다.
 * VFIO 는 주소를 가상화하지만 속성만은 진짜여야 한다 — 사용자 드라이버가
 * "이 BAR 는 64비트 메모리인가 I/O 포트인가" 를 보고 매핑 방식을 정하기
 * 때문이다. 하드웨어를 다시 읽는 대신 커널이 이미 파싱해 둔
 * pci_resource_flags 에서 되만드는 이유는, 하드웨어의 BAR 가 리셋 등으로
 * 날아가 있어도 옳은 값을 낼 수 있기 때문이다.
 *
 * 동작 과정:
 *  1. 커널이 열거 시점에 채워 둔 자원 플래그를 가져온다.
 *  2. I/O 자원이면 I/O 공간 표시 비트 하나만 세워 즉시 반환한다. I/O BAR 에는
 *     64비트나 prefetch 개념이 없다.
 *  3. 메모리면 메모리 공간 표시로 시작해, prefetchable 이면 그 비트를,
 *     64비트 자원이면 타입 비트를 더한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 메모리 읽기뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_bar_fixup → [vfio_generate_bar_flags] → pci_resource_flags
 */
static __le32 vfio_generate_bar_flags(struct pci_dev *pdev, int bar)
{
	/* [한국어] 커널이 열거 시점에 파싱해 둔 자원 속성. 하드웨어 BAR 를 다시 읽지 않는
	 * 이유는, 리셋으로 BAR 가 날아가 있어도 이 값은 그대로이기 때문이다. */
	unsigned long flags = pci_resource_flags(pdev, bar);
	/* [한국어] 메모리 BAR 의 플래그를 조립할 그릇. */
	u32 val;

	/* [한국어] I/O 포트 자원이면 공간 종류 비트 하나만 세우고 끝이다. I/O BAR 에는
	 * 64비트나 prefetch 개념이 없다. */
	if (flags & IORESOURCE_IO)
		return cpu_to_le32(PCI_BASE_ADDRESS_SPACE_IO);

	/* [한국어] 메모리 공간 표시로 시작한다. 이 상수 값은 0 이지만 의도를 드러내려고
	 * 명시적으로 적는다. */
	val = PCI_BASE_ADDRESS_SPACE_MEMORY;

	/* [한국어] prefetchable 메모리면 그 사실을 알려야 한다. 사용자 드라이버가 쓰기
	 * 결합 매핑을 쓸지 정하는 근거가 된다. */
	if (flags & IORESOURCE_PREFETCH)
		/* [한국어] prefetch 비트를 세운다. */
		val |= PCI_BASE_ADDRESS_MEM_PREFETCH;

	/* [한국어] 64비트 BAR 면 다음 dword 가 주소 상위 절반이라는 사실을 사용자에게
	 * 알려야 한다. */
	if (flags & IORESOURCE_MEM_64)
		/* [한국어] 64비트 타입 비트를 세운다. vfio_bar_fixup 이 이 비트를 되읽어 다음
		 * dword 도 함께 다듬을지 판단한다. */
		val |= PCI_BASE_ADDRESS_MEM_TYPE_64;

	/* [한국어] vconfig 에 얹을 값이므로 리틀엔디언으로 바꿔 돌려준다. */
	return cpu_to_le32(val);
}

/* [한국어]
 * vfio_bar_fixup - 가상 BAR 값을 하드웨어 능력에 맞게 다듬어 BAR 사이징을 흉내 낸다
 *
 * @vdev: 대상 디바이스. vconfig 의 BAR 영역을 제자리에서 고친다.
 * @return: 없다.
 *
 * 왜 필요한가: PCI 드라이버가 BAR 크기를 알아내는 방법은 "BAR 에 전부 1 을
 * 쓰고 되읽어, 0 으로 남은 하위 비트 수를 세는 것" 이다. VFIO 는 진짜 BAR 에
 * 쓰게 둘 수 없으므로(그러면 디바이스의 MMIO 창이 호스트 메모리 위로 옮겨
 * 갈 수 있다) 이 프로토콜을 소프트웨어로 흉내 내야 한다. 사용자의 쓰기는
 * vconfig 에만 들어가고, 읽기 직전에 이 함수가 "크기 마스크" 를 적용해
 * 하드웨어가 답할 법한 값으로 다듬는다. 그 결과 사용자는 진짜 크기를 얻지만
 * 디바이스는 움직이지 않는다.
 *
 * 동작 과정:
 *  1. bardirty 가 꺼져 있으면 아무도 BAR 를 건드리지 않은 것이므로 그냥
 *     돌아간다. 이 플래그는 vfio_basic_config_write 가 BAR 영역 쓰기를
 *     감지할 때 켜고, vfio_config_init 이 처음에 켜 둔다.
 *  2. vconfig 의 BAR0 부터 표준 BAR 6개를 순회한다.
 *  3. 호스트가 이 BAR 에 아무 자원도 배정하지 않았으면 통째로 0 으로 만든다.
 *     상류 주석대로 "호스트가 매핑하지 않았다 = 사용자에게는 미구현" 이다.
 *  4. 자원 길이에서 크기 마스크를 만든다. 길이가 2의 거듭제곱이므로
 *     길이-1 의 보수가 곧 "주소로 유효한 상위 비트" 마스크가 된다.
 *  5. 사용자가 쓴 값에 그 마스크를 씌워 하위 비트를 0 으로 만들고,
 *     vfio_generate_bar_flags 가 만든 속성 비트를 얹는다. 이것이 곧 실제
 *     하드웨어가 답할 값이다.
 *  6. 그 BAR 가 64비트 타입이면 다음 dword 가 주소 상위 절반이다. 포인터를
 *     한 칸 더 전진시켜 마스크의 상위 32비트를 씌우고, 루프 변수도 하나 더
 *     올려 그 칸을 다시 처리하지 않게 한다.
 *  7. ROM BAR 도 같은 방식으로 다듬는다. 다만 ROM BAR 는 비트 0 이 주소가
 *     아니라 Enable 비트라 마스크에 그 비트를 살려 둔다. 자원이 없으면
 *     pdev->rom 과 romlen 으로 섀도 ROM 크기를 대신 쓰고, 그것도 없으면 0.
 *     상류 주석이 밝히듯 REGION_INFO 가 크기 0 을 보고했더라도 여기서는
 *     실제 BAR 크기를 답한다.
 *  8. bardirty 를 내려 다음 읽기에서 이 계산을 건너뛰게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. vconfig 와 커널 자원 정보만 만지므로 잠들지
 * 않는다. bardirty 를 락 없이 읽고 쓰지만, 동시 접근 시 최악의 결과는 계산을
 * 한 번 더 하거나 한 번 덜 하는 것뿐이고 그 대상은 사용자 자신의 그림자
 * 복사본이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_basic_config_read → [vfio_bar_fixup] → vfio_generate_bar_flags
 */
/*
 * Pretend we're hardware and tweak the values of the *virtual* PCI BARs
 * to reflect the hardware capabilities.  This implements BAR sizing.
 */
static void vfio_bar_fixup(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 자원 정보를 볼 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] BAR 인덱스 루프 변수. 64비트 BAR 를 만나면 본문에서 한 번 더 오른다. */
	int i;
	/* [한국어] vconfig 안의 BAR 를 가리키는 커서. */
	__le32 *vbar;
	/* [한국어] 크기 마스크. 64비트 BAR 의 상위 절반까지 담아야 해서 64비트다. */
	u64 mask;

	/* [한국어] 아무도 BAR 를 건드리지 않았으면 이미 다듬어진 값이 그대로 유효하다.
	 * 매번 계산하지 않기 위한 캐시 플래그다. */
	if (!vdev->bardirty)
		return;

	/* [한국어] 그림자 복사본의 BAR0 위치에서 시작한다. */
	vbar = (__le32 *)&vdev->vconfig[PCI_BASE_ADDRESS_0];

	/* [한국어] 표준 BAR 6개를 순회한다. */
	for (i = 0; i < PCI_STD_NUM_BARS; i++, vbar++) {
		/* [한국어] 커널의 자원 배열 인덱스로 변환한다. pci_dev 의 resource 배열에는
		 * 표준 BAR 앞에 다른 항목이 있어 기준점을 더해야 한다. */
		int bar = i + PCI_STD_RESOURCES;

		/* [한국어] 호스트가 이 BAR 에 주소를 배정하지 않았다. */
		if (!pci_resource_start(pdev, bar)) {
			/* [한국어] 통째로 0 으로 만들어 "구현되지 않은 BAR" 로 보이게 한다. 상류 주석이
			 * 그 해석을 명시한다 — 호스트가 매핑하지 못한 것은 사용자에게도 없는 것이다. */
			*vbar = 0; /* Unmapped by host = unimplemented to user */
			continue;
		}

		/* [한국어] 크기 마스크를 만든다. BAR 크기는 언제나 2의 거듭제곱이라 (크기-1)의
		 * 보수가 곧 "주소로 유효한 상위 비트" 가 된다. 이 마스크를 씌우면 하위
		 * 비트가 0 이 되어, 사용자가 전부 1 을 쓰고 되읽었을 때 크기를 계산할 수 있다. */
		mask = ~(pci_resource_len(pdev, bar) - 1);

		/* [한국어] 사용자가 쓴 값에 크기 마스크를 씌운다. 이것이 곧 진짜 하드웨어가
		 * 답했을 값이다. */
		*vbar &= cpu_to_le32((u32)mask);
		/* [한국어] 하위 속성 비트를 얹는다. 주소는 가짜지만 속성은 진짜여야 사용자
		 * 드라이버가 올바른 매핑 방식을 고른다. */
		*vbar |= vfio_generate_bar_flags(pdev, bar);

		/* [한국어] 방금 얹은 속성으로 64비트 BAR 인지 판정한다. */
		if (*vbar & cpu_to_le32(PCI_BASE_ADDRESS_MEM_TYPE_64)) {
			/* [한국어] 다음 dword 가 주소의 상위 절반이다. */
			vbar++;
			/* [한국어] 마스크의 상위 32비트를 씌운다. 대개 전부 1 이라 사용자 값이 그대로
			 * 남지만, 크기가 4GiB 를 넘는 BAR 에서는 여기서도 하위 비트가 잘린다. */
			*vbar &= cpu_to_le32((u32)(mask >> 32));
			/* [한국어] 루프 변수도 하나 더 올린다. 상위 절반 dword 를 독립된 BAR 로 다시
			 * 처리하지 않기 위해서다. 루프 증감식의 vbar++ 와 짝이 맞는다. */
			i++;
		}
	}

	/* [한국어] 커서를 ROM BAR 로 옮긴다. */
	vbar = (__le32 *)&vdev->vconfig[PCI_ROM_ADDRESS];

	/*
	 * NB. REGION_INFO will have reported zero size if we weren't able
	 * to read the ROM, but we still return the actual BAR size here if
	 * it exists (or the shadow ROM space).
	 */
	/* [한국어] 호스트가 ROM BAR 에 자원을 배정했다면 그 크기를 쓴다. */
	if (pci_resource_start(pdev, PCI_ROM_RESOURCE)) {
		/* [한국어] ROM 크기로 마스크를 만든다. */
		mask = ~(pci_resource_len(pdev, PCI_ROM_RESOURCE) - 1);
		/* [한국어] ROM BAR 의 비트 0 은 주소가 아니라 Enable 비트다. 마스크에 그 비트를
		 * 살려 두어 사용자가 쓴 활성화 상태가 지워지지 않게 한다. */
		mask |= PCI_ROM_ADDRESS_ENABLE;
		/* [한국어] 마스크를 씌운다. ROM BAR 에는 vfio_generate_bar_flags 로 얹을 속성이 없다. */
		*vbar &= cpu_to_le32((u32)mask);
	/* [한국어] 자원은 없지만 커널이 섀도 ROM(펌웨어가 복사해 둔 옵션 ROM 사본)을
	 * 들고 있는 경우다. */
	} else if (pdev->rom && pdev->romlen) {
		/* [한국어] 섀도 ROM 길이는 2의 거듭제곱이 아닐 수 있어 먼저 올림한 뒤 마스크를
		 * 만든다. BAR 사이징 프로토콜 자체가 2의 거듭제곱만 표현할 수 있기 때문이다. */
		mask = ~(roundup_pow_of_two(pdev->romlen) - 1);
		/* [한국어] 여기서도 Enable 비트를 마스크에 살린다. */
		mask |= PCI_ROM_ADDRESS_ENABLE;
		/* [한국어] 섀도 ROM 크기로 다듬는다. 상류 주석이 밝히듯, REGION_INFO 가 ROM 을
		 * 읽지 못해 크기 0 을 보고했더라도 여기서는 실제 BAR 크기를 답한다. */
		*vbar &= cpu_to_le32((u32)mask);
	} else {
		/* [한국어] ROM 이 아예 없으면 미구현으로 보이게 0 을 둔다. */
		*vbar = 0;
	}

	/* [한국어] 다듬기 완료 표시. 다음 사용자 쓰기가 다시 켤 때까지 이 계산을 건너뛴다. */
	vdev->bardirty = false;
}

/* [한국어]
 * vfio_basic_config_read - 표준 PCI 헤더(0~63바이트) 읽기 콜백. BAR 사이징과 가상 메모리 활성 비트를 얹는다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋. 표준 헤더 영역이므로 offset 과 값이 같다.
 * @count: 읽을 바이트 수.
 * @perm: 표준 헤더용 권한 표(cap_perms 의 0번 슬롯).
 * @offset: capability 시작점 기준 오프셋. 표준 헤더는 시작점이 0 이라 pos 와 같다.
 * @val: 결과를 담을 곳.
 * @return: 읽은 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: 표준 헤더에는 두 가지 특별 처리가 필요하다. (a) BAR 를 읽기
 * 직전에 사이징 계산을 반영해야 하고, (b) Memory Space Enable 비트가 없는
 * 디바이스(VF 등)에는 가상 비트를 얹어 줘야 한다. 나머지는 기본 엔진이 한다.
 *
 * 동작 과정:
 *  1. 읽는 위치가 BAR 나 ROM BAR 영역이면 vfio_bar_fixup 으로 vconfig 의
 *     BAR 값을 먼저 다듬는다. 상류 주석이 "표준 헤더에서는 pos 와 offset 이
 *     같다" 고 밝혀, is_bar 에 offset 을 넘겨도 되는 근거를 준다.
 *  2. 기본 엔진으로 vconfig 와 하드웨어를 섞어 읽는다.
 *  3. COMMAND 레지스터를 읽는 중이고 이 디바이스에 Memory Space Enable 비트가
 *     물리적으로 없다면(no_command_memory), vconfig 쪽의 그 비트를 결과에
 *     얹어 준다. 그러지 않으면 사용자가 방금 켠 비트가 되읽을 때 0 으로
 *     보여 드라이버가 혼란에 빠진다. 실제 메모리 디코딩은 PF 의 SR-IOV
 *     Control 이 담당하므로 이 비트는 순수한 겉치레다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 2단계에서 잠들 수 있다.
 *
 * 에러 경로: 기본 엔진이 음수를 반환하면 그대로 흘러 나간다. 다만 3단계의
 * 조건문은 count 의 부호를 보지 않으므로, 오류 상황에서도 *val 을 한 번 더
 * 건드릴 수 있다 — 호출자가 음수를 보고 값을 버리므로 결과에는 영향이 없다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->readfn(= 이 함수)
 *     → [vfio_basic_config_read] → vfio_bar_fixup, vfio_default_config_read
 */
static int vfio_basic_config_read(struct vfio_pci_core_device *vdev, int pos,
				  int count, struct perm_bits *perm,
				  int offset, __le32 *val)
{
	/* [한국어] 읽는 위치가 BAR 나 ROM BAR 영역인지 본다. 상류 주석이 표준 헤더에서는
	 * pos 와 offset 이 같다는 사실을 밝혀, 절대 오프셋 기준의 is_bar 에 offset 을
	 * 넘겨도 되는 근거를 준다. */
	if (is_bar(offset)) /* pos == offset for basic config */
		/* [한국어] 읽기 직전에 vconfig 의 BAR 값을 크기 마스크로 다듬는다. 이 한 번의
		 * 호출이 BAR 사이징 프로토콜 전체를 흉내 낸다. */
		vfio_bar_fixup(vdev);

	/* [한국어] 기본 엔진으로 vconfig 와 하드웨어를 비트별로 섞어 읽는다. BAR 는 표에서
	 * 전면 가상화라 방금 다듬은 vconfig 값이 그대로 나간다. */
	count = vfio_default_config_read(vdev, pos, count, perm, offset, val);

	/* Mask in virtual memory enable */
	/* [한국어] COMMAND 를 읽는 중이고 이 디바이스에 Memory Space Enable 비트가
	 * 물리적으로 없는 경우(VF 등)다. */
	if (offset == PCI_COMMAND && vdev->pdev->no_command_memory) {
		/* [한국어] 그림자 복사본의 COMMAND 값을 꺼낸다. 사용자가 켠 가상 비트가 여기 있다. */
		u16 cmd = le16_to_cpu(*(__le16 *)&vdev->vconfig[PCI_COMMAND]);
		/* [한국어] 방금 읽은 결과를 CPU 표현으로 바꾼다. */
		u32 tmp_val = le32_to_cpu(*val);

		/* [한국어] 가상 Memory Space Enable 비트를 결과에 얹는다. 이 비트는 표에서
		 * 가상화 대상이 아니라 기본 엔진이 하드웨어 값(항상 0)을 주기 때문에,
		 * 얹어 주지 않으면 사용자가 방금 켠 비트가 0 으로 되읽혀 드라이버가
		 * 활성화 실패로 판단한다. */
		tmp_val |= cmd & PCI_COMMAND_MEMORY;
		/* [한국어] 리틀엔디언으로 되돌려 결과에 담는다. */
		*val = cpu_to_le32(tmp_val);
	}

	/* [한국어] 기본 엔진이 돌려준 바이트 수(또는 오류)를 그대로 위로 올린다. */
	return count;
}

/* [한국어]
 * vfio_need_bar_restore - 하드웨어의 BAR 가 우리가 기억하는 값과 달라졌는지 검사한다
 *
 * @vdev: 대상 디바이스. rbar[0..5] 에 원본 값이 들어 있다.
 * @return: true 면 하나라도 어긋났으니 복구가 필요하다. false 면 그대로다.
 *
 * 왜 필요한가: 벤더 고유의 뒷문 리셋은 VFIO 가 가로챌 수 없다. 그래서
 * "리셋 명령을 봤는가" 가 아니라 "결과가 달라졌는가" 로 판정한다. 이 함수가
 * 그 결과 검사다.
 *
 * 동작 과정:
 *  1. BAR0 부터 BAR5 까지 순회한다. i 는 rbar 인덱스, pos 는 config 오프셋이며
 *     둘이 나란히 전진한다.
 *  2. rbar 에 기억해 둔 값이 0 이 아닌 BAR 만 본다. 0 이면 애초에 호스트가
 *     주소를 배정하지 않은 BAR 라 비교할 기준이 없다.
 *  3. 하드웨어에서 실제 값을 읽는다. 읽기 자체가 실패하거나 값이 다르면
 *     즉시 true — 읽기 실패도 "디바이스가 이상해졌다" 는 신호로 본다.
 *  4. 여섯 개가 모두 일치하면 false.
 *
 * 실행 컨텍스트: 프로세스 문맥. COMMAND 쓰기를 처리하는 도중, memory_lock 을
 * 쓰기 모드로 쥔 채 불린다. pci_user_ 계열이라 잠들 수 있다.
 *
 * 에러 경로: 읽기 오류를 위로 올리지 않고 "복구 필요" 로 해석한다. 복구는
 * 멱등하므로(같은 값을 다시 쓰는 것뿐) 과잉 복구는 무해하다.
 *
 * 호출 체인:
 *   vfio_basic_config_write → [vfio_need_bar_restore]
 *     → pci_user_read_config_dword
 */
/* Test whether BARs match the value we think they should contain */
static bool vfio_need_bar_restore(struct vfio_pci_core_device *vdev)
{
	/* [한국어] i 는 rbar 배열 인덱스, pos 는 config 오프셋. 두 값이 나란히 전진한다. */
	int i = 0, pos = PCI_BASE_ADDRESS_0, ret;
	/* [한국어] 하드웨어에서 읽은 현재 BAR 값을 담을 곳. */
	u32 bar;

	/* [한국어] 표준 BAR 6개만 검사한다. ROM BAR 는 활성화 비트 때문에 값이 정당하게
	 * 달라질 수 있어 판정 기준으로 삼지 않는다. */
	for (; pos <= PCI_BASE_ADDRESS_5; i++, pos += 4) {
		/* [한국어] 호스트가 주소를 배정한 BAR 만 비교한다. 기억해 둔 값이 0 이면 비교할
		 * 기준이 없다. */
		if (vdev->rbar[i]) {
			/* [한국어] 하드웨어의 현재 값을 읽는다. */
			ret = pci_user_read_config_dword(vdev->pdev, pos, &bar);
			/* [한국어] 읽기 실패도, 값 불일치도 모두 "디바이스가 리셋됐다" 는 신호로 본다.
			 * 복구는 같은 값을 다시 쓰는 멱등 연산이라 과잉 복구가 무해하기 때문에
			 * 보수적으로 판정해도 된다. */
			if (ret || vdev->rbar[i] != bar)
				return true;
		}
	}

	/* [한국어] 여섯 개가 모두 그대로다. 복구할 필요가 없다. */
	return false;
}

/* [한국어]
 * vfio_basic_config_write - 표준 PCI 헤더 쓰기 콜백. COMMAND 레지스터의 부작용을 전부 여기서 처리한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋(표준 헤더라 offset 과 같다).
 * @count: 쓸 바이트 수.
 * @perm: 표준 헤더 권한 표.
 * @offset: capability 기준 오프셋(= pos).
 * @val: 사용자가 쓴 값.
 * @return: 쓴 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: 표준 헤더에서 부작용이 있는 레지스터는 COMMAND 하나뿐인데,
 * 그 하나가 이 파일에서 가장 위험한 자리다. Memory/IO Space Enable 을 끄면
 * 사용자가 매핑해 둔 BAR 를 즉시 걷어내야 하고(안 그러면 비활성 BAR 에
 * 접근해 머신 체크가 날 수 있다), 켜기 전에는 뒷문 리셋으로 BAR 가 날아가지
 * 않았는지 확인해야 하며, INTx Disable 비트는 실제 인터럽트 마스킹으로
 * 번역해야 한다. 그 세 가지를 순서대로 한다.
 *
 * 동작 과정:
 *  1. vconfig 의 COMMAND 사본 주소를 미리 잡아 둔다. 아래 여러 곳에서 쓴다.
 *  2. COMMAND 를 쓰는 중이면:
 *     a. 하드웨어의 현재 COMMAND 를 읽는다.
 *     b. 물리/가상/새 값 세 벌에 대해 IO 와 MEM 활성 비트를 각각 뽑는다.
 *     c. 새 값이 MEM 을 끄는 것이면 vfio_pci_zap_and_down_write_memory_lock 으로
 *        사용자 매핑을 전부 걷어내고 memory_lock 을 쓰기 모드로 잡는다.
 *        dma-buf 로 내보낸 영역도 revoke 한다. 켜는 쪽이면 매핑을 걷어낼
 *        필요가 없으므로 락만 잡는다. 어느 쪽이든 이 지점 이후로는 락을
 *        쥔 상태이므로 아래 모든 경로가 반드시 풀어 줘야 한다.
 *     d. 상류 주석이 설명하는 뒷문 리셋 감지: "사용자가 켜려 하고, 우리도
 *        이미 켜져 있다고 믿는데, 하드웨어는 꺼져 있다" 면 그 사이에 무언가
 *        디바이스를 리셋한 것이다. 또는 BAR 값 자체가 어긋났어도 마찬가지다.
 *        그러면 BAR 를 먼저 되살린 뒤 활성화를 허용한다. no_command_memory
 *        디바이스는 MEM 비트가 물리적으로 없어 이 판정에서 제외한다.
 *  3. 기본 엔진으로 실제 쓰기를 수행한다.
 *  4. 실패했으면 COMMAND 경로에서 잡았던 락을 반드시 풀고 오류를 반환한다.
 *  5. 성공했고 COMMAND 였다면, 다음번 뒷문 리셋 판정을 위해 MEM/IO 활성
 *     비트를 vconfig 에 보존한다. 이 두 비트는 표에서 가상화 대상이 아니라
 *     기본 엔진이 vconfig 에 남겨 주지 않으므로 여기서 직접 넣어야 한다.
 *     그 뒤 메모리가 다시 켜졌으면 dma-buf 를 복구하고 락을 푼다.
 *  6. COMMAND 의 두 바이트 중 어느 쪽이든 건드렸으면 INTx Disable 가상 비트를
 *     확인한다. 사용자가 그 비트를 세웠고 아직 우리가 마스크하지 않았다면
 *     vfio_pci_intx_mask 로 실제 라인을 막고, 반대면 푼다. virq_disabled 는
 *     이 파일과 vfio_pci_intrs.c 가 공유하는 상태로, 그 파일의
 *     vfio_send_intx_eventfd 가 이 값을 보고 eventfd 전달을 억제한다.
 *  7. BAR 영역을 건드렸으면 bardirty 를 켜서 다음 읽기에 사이징을 다시
 *     계산하게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. memory_lock(rw_semaphore)을 쓰기 모드로
 * 잡는 구간이 있고, vfio_pci_zap_and_down_write_memory_lock 은 페이지 테이블
 * 정리를 동반해 오래 걸릴 수 있다. INTx 처리는 vfio_pci_intrs.c 의 igate
 * mutex 를 다시 잡는다.
 *
 * 에러 경로: 3단계 실패 시 4단계가 락을 푼다. 2a 단계 실패는 락을 잡기 전이라
 * 그냥 반환해도 안전하다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(= 이 함수)
 *     → [vfio_basic_config_write]
 *     → vfio_pci_zap_and_down_write_memory_lock / vfio_pci_dma_buf_move /
 *       vfio_need_bar_restore / vfio_bar_restore / vfio_default_config_write /
 *       __vfio_pci_memory_enabled / vfio_pci_intx_mask / vfio_pci_intx_unmask
 */
static int vfio_basic_config_write(struct vfio_pci_core_device *vdev, int pos,
				   int count, struct perm_bits *perm,
				   int offset, __le32 val)
{
	/* [한국어] 하드웨어 접근용 핸들. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 그림자 복사본의 COMMAND 위치. 아래 여러 곳에서 읽고 쓴다. */
	__le16 *virt_cmd;
	/* [한국어] 사용자가 이번에 쓴 COMMAND 값. COMMAND 가 아닌 오프셋이면 0 으로 남고
	 * 아래 보존 단계에서도 쓰이지 않는다. */
	u16 new_cmd = 0;
	/* [한국어] 하드웨어 읽기 결과 코드. */
	int ret;

	/* [한국어] COMMAND 사본 주소를 미리 잡는다. INTx 처리까지 이 포인터를 계속 쓴다. */
	virt_cmd = (__le16 *)&vdev->vconfig[PCI_COMMAND];

	/* [한국어] COMMAND 레지스터 자체를 (2바이트 이상으로) 쓰는 경우. 이 파일에서 가장
	 * 많은 부작용이 달린 자리다. */
	if (offset == PCI_COMMAND) {
		/* [한국어] 물리/가상/새 값 세 벌에 대한 메모리와 I/O 활성 비트. 뒷문 리셋 판정에
		 * 세 값이 모두 필요하다. */
		bool phys_mem, virt_mem, new_mem, phys_io, virt_io, new_io;
		/* [한국어] 하드웨어의 현재 COMMAND 값. */
		u16 phys_cmd;

		/* [한국어] 하드웨어의 현재 상태를 읽는다. 아직 락을 잡기 전이라 실패해도 그냥
		 * 반환하면 된다. */
		ret = pci_user_read_config_word(pdev, PCI_COMMAND, &phys_cmd);
		if (ret)
			return ret;

		/* [한국어] 사용자가 쓴 값을 CPU 표현으로. 32비트에서 16비트로 좁혀지지만
		 * 관심 있는 비트(I/O, Memory, INTx Disable)가 모두 하위 16비트에 있어 문제없다. */
		new_cmd = le32_to_cpu(val);

		/* [한국어] 하드웨어의 I/O 공간 활성 여부. !! 로 0/1 로 정규화한다. */
		phys_io = !!(phys_cmd & PCI_COMMAND_IO);
		/* [한국어] 우리가 "켜져 있다" 고 믿는 상태(그림자 복사본). */
		virt_io = !!(le16_to_cpu(*virt_cmd) & PCI_COMMAND_IO);
		/* [한국어] 사용자가 요청한 상태. */
		new_io = !!(new_cmd & PCI_COMMAND_IO);

		/* [한국어] 메모리 공간에 대해 같은 세 값을 뽑는다. */
		phys_mem = !!(phys_cmd & PCI_COMMAND_MEMORY);
		/* [한국어] 그림자 복사본 기준 메모리 활성 여부. */
		virt_mem = !!(le16_to_cpu(*virt_cmd) & PCI_COMMAND_MEMORY);
		/* [한국어] 사용자 요청 기준. */
		new_mem = !!(new_cmd & PCI_COMMAND_MEMORY);

		/* [한국어] 사용자가 메모리 디코딩을 끄려 한다. 그 순간부터 BAR 접근이 정의되지
		 * 않으므로, 사용자가 mmap 해 둔 매핑을 먼저 전부 걷어내야 한다. */
		if (!new_mem) {
			/* [한국어] BAR 영역의 사용자 페이지 테이블 항목을 지우고 memory_lock 을 쓰기
			 * 모드로 잡는다. 락을 쥔 채로 지우므로, 이후 새 폴트는 락 뒤에서 대기하다가
			 * __vfio_pci_memory_enabled 판정에 걸려 SIGBUS 가 된다. */
			vfio_pci_zap_and_down_write_memory_lock(vdev);
			/* [한국어] 이 BAR 를 dma-buf 로 다른 드라이버에 내보냈다면 그 매핑도 revoke 한다.
			 * peer-to-peer DMA 대상이 사라지기 때문이다. */
			vfio_pci_dma_buf_move(vdev, true);
		} else {
			/* [한국어] 켜는 방향이면 기존 매핑이 계속 유효하므로 걷어낼 필요가 없다. 락만
			 * 잡아 아래 판정과 갱신을 원자적으로 만든다. */
			down_write(&vdev->memory_lock);
		}

		/*
		 * If the user is writing mem/io enable (new_mem/io) and we
		 * think it's already enabled (virt_mem/io), but the hardware
		 * shows it disabled (phys_mem/io, then the device has
		 * undergone some kind of backdoor reset and needs to be
		 * restored before we allow it to enable the bars.
		 * SR-IOV devices will trigger this - for mem enable let's
		 * catch this now and for io enable it will be caught later
		 */
		/* [한국어] 상류 주석이 설명하는 뒷문 리셋 감지의 첫 조건. "사용자가 켜려 하고
		 * (new_mem), 우리는 이미 켜져 있다고 믿는데(virt_mem), 하드웨어는 꺼져
		 * 있다(!phys_mem)" 면 그 사이 무언가가 디바이스를 리셋한 것이다.
		 * no_command_memory 디바이스는 이 비트가 물리적으로 없어 판정에서 제외한다.
		 * 두 번째 조건은 I/O 공간에 대한 같은 논리이고, 세 번째는 BAR 값 자체를
		 * 직접 비교하는 vfio_need_bar_restore 다. 상류 주석대로 SR-IOV 디바이스가
		 * 이 경로를 실제로 밟는다. */
		if ((new_mem && virt_mem && !phys_mem &&
		     !pdev->no_command_memory) ||
		    (new_io && virt_io && !phys_io) ||
		    vfio_need_bar_restore(vdev))
			/* [한국어] 활성화를 허용하기 전에 원래 BAR 주소를 되살린다. 이걸 빼먹으면 사용자가
			 * 0 번지에 놓인 BAR 를 켜게 된다. */
			vfio_bar_restore(vdev);
	}

	/* [한국어] 실제 쓰기. 표에 따라 INTx Disable 비트는 vconfig 로, 나머지 허용 비트는
	 * 하드웨어로 간다. */
	count = vfio_default_config_write(vdev, pos, count, perm, offset, val);
	/* [한국어] 쓰기 실패. COMMAND 경로였다면 락을 쥔 채로 있으므로 반드시 풀어야 한다. */
	if (count < 0) {
		/* [한국어] 락을 잡은 것은 COMMAND 경로뿐이다. */
		if (offset == PCI_COMMAND)
			/* [한국어] 락 해제 후 오류 전파. */
			up_write(&vdev->memory_lock);
		/* [한국어] 쓴 바이트 수. */
		return count;
	}

	/*
	 * Save current memory/io enable bits in vconfig to allow for
	 * the test above next time.
	 */
	/* [한국어] 쓰기가 성공한 COMMAND 경로. 남은 세 가지 뒷정리를 한다. */
	if (offset == PCI_COMMAND) {
		/* [한국어] 다음번 뒷문 리셋 판정의 기준이 될 두 비트. */
		u16 mask = PCI_COMMAND_MEMORY | PCI_COMMAND_IO;

		/* [한국어] 그림자 복사본에서 두 비트를 비운다. */
		*virt_cmd &= cpu_to_le16(~mask);
		/* [한국어] 사용자가 요청한 값으로 채운다. 상류 주석이 밝히듯 이 보존이 있어야
		 * 다음번에 virt_mem/virt_io 를 비교할 수 있다. 이 두 비트는 표에서 가상화
		 * 대상이 아니라 기본 엔진이 vconfig 에 남겨 주지 않기 때문에 직접 넣는다. */
		*virt_cmd |= cpu_to_le16(new_cmd & mask);

		/* [한국어] 메모리 접근이 다시 유효해졌으면 revoke 했던 dma-buf 를 되살린다. */
		if (__vfio_pci_memory_enabled(vdev))
			/* [한국어] dma-buf 매핑 복구. */
			vfio_pci_dma_buf_move(vdev, false);
		/* [한국어] 락 해제. 이 시점부터 사용자 폴트가 새 상태로 판정된다. */
		up_write(&vdev->memory_lock);
	}

	/* Emulate INTx disable */
	/* [한국어] COMMAND 의 두 바이트 중 어느 쪽이든 건드렸으면 INTx Disable 비트를
	 * 확인해야 한다. 위의 == 비교와 달리 1바이트 쓰기도 잡아낸다. */
	if (offset >= PCI_COMMAND && offset <= PCI_COMMAND + 1) {
		/* [한국어] 가상 INTx Disable 비트의 현재 값. */
		bool virt_intx_disable;

		/* [한국어] 그림자 복사본에서 INTx Disable 비트를 읽는다. 이 비트는 표에서
		 * 가상화 대상이라 사용자 값이 그대로 여기 들어 있다. */
		virt_intx_disable = !!(le16_to_cpu(*virt_cmd) &
				       PCI_COMMAND_INTX_DISABLE);

		/* [한국어] 사용자가 방금 INTx 를 끄려 했고 아직 우리가 막지 않은 상태다. */
		if (virt_intx_disable && !vdev->virq_disabled) {
			/* [한국어] 이 파일과 vfio_pci_intrs.c 가 공유하는 상태를 갱신한다. 그 파일의
			 * vfio_send_intx_eventfd 가 이 값을 보고 eventfd 전달을 억제하고,
			 * vfio_intx_enable 은 초기 마스크 상태를 이 값으로 정한다. */
			vdev->virq_disabled = true;
			/* [한국어] 실제 인터럽트 라인을 막는다. igate mutex 를 잡고 irqlock 아래에서
			 * 하드웨어 또는 IRQ 칩 수준의 마스킹을 수행한다. */
			vfio_pci_intx_mask(vdev);
		/* [한국어] 반대로 사용자가 INTx 를 다시 켰고 우리가 막아 둔 상태다. */
		} else if (!virt_intx_disable && vdev->virq_disabled) {
			/* [한국어] 공유 상태를 먼저 되돌린다. */
			vdev->virq_disabled = false;
			/* [한국어] 실제 라인을 다시 연다. 이 순서가 중요한데, unmask 경로가
			 * virq_disabled 를 다시 읽어 판단하기 때문이다. */
			vfio_pci_intx_unmask(vdev);
		}
	}

	/* [한국어] BAR 영역을 건드렸으면 다음 읽기에서 사이징을 다시 계산해야 한다. */
	if (is_bar(offset))
		/* [한국어] 캐시 무효화 표시. */
		vdev->bardirty = true;

	return count;
}

/* [한국어]
 * init_pci_cap_basic_perm - 표준 PCI 헤더 64바이트의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(cap_perms 의 0번 슬롯).
 * @return: 0 성공, -ENOMEM 이면 비트맵 할당 실패.
 *
 * 왜 필요한가: 이 파일에서 가장 중요한 표다. 표준 헤더에는 BAR, COMMAND,
 * capability 목록 포인터, 인터럽트 핀처럼 호스트 안전에 직결되는 필드가
 * 모두 모여 있다. 아래에서 레지스터별로 왜 그 선택이 안전한지 밝힌다.
 * 표시하지 않은 나머지 바이트는 전부 기본값 그대로 — 읽기는 하드웨어 통과,
 * 쓰기는 거부다.
 *
 * 동작 과정: 64바이트짜리 비트맵을 만들고, 전용 readfn/writefn 을 심은 뒤
 * p_set 계열로 예외를 하나씩 뚫는다.
 *
 * 실행 컨텍스트: 모듈 적재 시점. __init 으로 표시돼 초기화가 끝나면 코드가
 * 버려진다. 프로세스 문맥이며 잠들 수 있다.
 *
 * 에러 경로: 할당 실패면 -ENOMEM 만 올린다. 호출자 vfio_pci_init_perm_bits 가
 * 전체를 되돌린다.
 *
 * 호출 체인:
 *   vfio_pci_core.c 의 모듈 init → vfio_pci_init_perm_bits
 *     → [init_pci_cap_basic_perm] → alloc_perm_bits, p_setb/p_setw/p_setd
 */
/* Permissions for the Basic PCI Header */
static int __init init_pci_cap_basic_perm(struct perm_bits *perm)
{
	/* [한국어] 표준 헤더 64바이트짜리 비트맵 두 벌을 만든다. 0 초기화이므로 이 시점의
	 * 표는 "전부 읽기 가능, 아무것도 쓰기 불가, 아무것도 가상화 안 함" 이다. */
	if (alloc_perm_bits(perm, PCI_STD_HEADER_SIZEOF))
		return -ENOMEM;

	/* [한국어] 기본 엔진 대신 표준 헤더 전용 읽기 콜백을 심는다. BAR 사이징과
	 * 가상 Memory Space Enable 비트 처리가 그 안에 있다. */
	perm->readfn = vfio_basic_config_read;
	/* [한국어] 표준 헤더 전용 쓰기 콜백. COMMAND 레지스터의 모든 부작용이 그 안에 있다. */
	perm->writefn = vfio_basic_config_write;

	/* Virtualized for SR-IOV functions, which just have FFFF */
	/* [한국어] [Vendor ID / 오프셋 0x00, 2바이트] 읽기: 에뮬레이션(vconfig). 쓰기: 거부.
	 * 왜 안전한가 — 원래 읽기 전용 필드라 쓰기를 막는 것이 스펙과 일치한다.
	 * 굳이 가상화까지 하는 이유는 상류 주석대로 SR-IOV VF 때문이다. VF 의
	 * config 공간은 이 두 필드를 전부 1(0xFFFF)로 읽히게 하는 경우가 있어
	 * 사용자가 디바이스를 식별할 수 없다. vfio_config_init 이 VF 일 때 커널이
	 * 아는 진짜 값을 vconfig 에 채워 넣고, 이 가상화 덕분에 그 값이 사용자에게
	 * 보인다. */
	p_setw(perm, PCI_VENDOR_ID, (u16)ALL_VIRT, NO_WRITE);
	/* [한국어] [Device ID / 오프셋 0x02, 2바이트] Vendor ID 와 완전히 같은 이유로
	 * 에뮬레이션 + 쓰기 거부다. */
	p_setw(perm, PCI_DEVICE_ID, (u16)ALL_VIRT, NO_WRITE);

	/*
	 * Virtualize INTx disable, we use it internally for interrupt
	 * control and can emulate it for non-PCI 2.3 devices.
	 */
	/* [한국어] [Command / 오프셋 0x04, 2바이트] 이 파일에서 가장 중요한 한 줄.
	 * 가상화 마스크는 INTx Disable 비트 하나뿐이고, 쓰기는 16비트 전부 허용이다.
	 * 즉 I/O Space Enable, Memory Space Enable, Bus Master, Parity Error Response,
	 * SERR Enable 같은 나머지 비트는 전부 (1)통과 — 사용자 쓰기가 하드웨어에
	 * 그대로 반영된다.
	 * 왜 그것이 안전한가 — 이 비트들은 모두 디바이스 자신의 동작 범위 안에서만
	 * 효력을 갖는다. Bus Master 를 켜 봐야 그 DMA 는 IOMMU 도메인 안에 갇혀 있고,
	 * 메모리 디코딩을 켜 봐야 호스트가 배정한 BAR 창 안에서만 응답한다. 반대로
	 * 끄는 쪽은 위험할 수 있는데(사용자 매핑이 살아 있는 채로 디코딩이 꺼진다)
	 * 그래서 vfio_basic_config_write 가 쓰기를 가로채 매핑을 먼저 걷어낸다.
	 * INTx Disable 만 가상화하는 이유는 상류 주석이 밝힌다 — VFIO 가 이 비트를
	 * 내부 인터럽트 제어에 쓰고 있어서(사용자가 INTx 를 마스크하는 표준 수단),
	 * 그 값을 커널이 읽을 수 있어야 하고, PCI 2.3 이전 디바이스처럼 이 비트가
	 * 없는 하드웨어에서도 소프트웨어로 흉내 낼 수 있어야 하기 때문이다. */
	p_setw(perm, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE, (u16)ALL_WRITE);

	/* Virtualize capability list, we might want to skip/disable */
	/* [한국어] [Status / 오프셋 0x06, 2바이트] Capability List 비트만 에뮬레이션,
	 * 쓰기는 전면 거부.
	 * 왜 그 비트만 가상화하는가 — 상류 주석대로 chain 을 건너뛰거나 통째로
	 * 비활성화할 수 있어야 하기 때문이다. vfio_cap_init 이 capability 를 하나도
	 * 채우지 못하면 이 비트를 vconfig 에서 지워 "목록 없음" 으로 보이게 한다.
	 * 쓰기 거부인 이유 — Status 의 나머지 비트는 대부분 W1C 오류 상태 비트라
	 * 쓰기를 허용해도 무해할 것 같지만, 마스터 중단이나 시스템 오류 같은 호스트
	 * 차원의 오류 기록을 사용자가 임의로 지우게 둘 이유가 없다. */
	p_setw(perm, PCI_STATUS, PCI_STATUS_CAP_LIST, NO_WRITE);

	/* No harm to write */
	/* [한국어] [Cache Line Size / 오프셋 0x0C, 1바이트] 통과 + 쓰기 허용.
	 * 상류 주석이 "쓴다고 해가 없다" 고 요약한다. 이 값은 디바이스가 Memory
	 * Write and Invalidate 트랜잭션을 쓸 때의 캐시 라인 길이일 뿐이라, 잘못
	 * 설정해도 그 디바이스의 성능만 나빠진다. */
	p_setb(perm, PCI_CACHE_LINE_SIZE, NO_VIRT, (u8)ALL_WRITE);
	/* [한국어] [Latency Timer / 오프셋 0x0D, 1바이트] 통과 + 쓰기 허용.
	 * 전통 PCI 버스에서 이 함수가 버스를 붙잡을 수 있는 최대 시간이다. PCIe 에는
	 * 의미가 없고, 전통 PCI 에서도 최악의 결과가 같은 버스의 지연 증가라
	 * 호스트 안전 문제는 아니다. */
	p_setb(perm, PCI_LATENCY_TIMER, NO_VIRT, (u8)ALL_WRITE);
	/* [한국어] [BIST / 오프셋 0x0F, 1바이트] 통과 + 쓰기 허용.
	 * Built-In Self Test 를 시작시키는 레지스터다. 자기 진단은 그 디바이스
	 * 안에서 끝나므로 사용자가 돌려도 된다. */
	p_setb(perm, PCI_BIST, NO_VIRT, (u8)ALL_WRITE);

	/* Virtualize all bars, can't touch the real ones */
	/* [한국어] [BAR0 / 오프셋 0x10, 4바이트] 완전 에뮬레이션 + 쓰기 허용.
	 * 상류 주석의 "진짜 BAR 는 건드릴 수 없다" 가 이 파일의 핵심 보안 결정 중
	 * 하나다. 사용자가 BAR 에 임의의 물리 주소를 쓰면 디바이스의 MMIO 응답 창이
	 * 호스트 커널 메모리나 다른 디바이스의 레지스터 위로 옮겨 갈 수 있다.
	 * IOMMU 는 디바이스가 내는 DMA 를 걸러 주지만, CPU 가 그 주소로 보내는
	 * MMIO 요청은 걸러 주지 않는다.
	 * 그래서 쓰기는 전부 허용하되(사용자 드라이버가 BAR 사이징 프로토콜을
	 * 정상적으로 수행해야 한다) 그 값은 vconfig 에만 들어가고, 읽을 때
	 * vfio_bar_fixup 이 크기 마스크와 속성 비트를 얹어 "진짜 하드웨어가 답했을
	 * 값" 을 만들어 준다. 사용자는 크기를 정확히 알아내지만 디바이스는 움직이지
	 * 않는다. */
	p_setd(perm, PCI_BASE_ADDRESS_0, ALL_VIRT, ALL_WRITE);
	/* [한국어] [BAR1 / 오프셋 0x14] BAR0 과 같은 이유로 완전 에뮬레이션. 64비트 BAR 의
	 * 상위 절반일 수도 있고 독립된 32비트 BAR 일 수도 있는데, 어느 쪽이든
	 * vfio_bar_fixup 이 타입 비트를 보고 알맞게 다듬는다. */
	p_setd(perm, PCI_BASE_ADDRESS_1, ALL_VIRT, ALL_WRITE);
	/* [한국어] [BAR2 / 오프셋 0x18] 같은 이유. */
	p_setd(perm, PCI_BASE_ADDRESS_2, ALL_VIRT, ALL_WRITE);
	/* [한국어] [BAR3 / 오프셋 0x1C] 같은 이유. */
	p_setd(perm, PCI_BASE_ADDRESS_3, ALL_VIRT, ALL_WRITE);
	/* [한국어] [BAR4 / 오프셋 0x20] 같은 이유. */
	p_setd(perm, PCI_BASE_ADDRESS_4, ALL_VIRT, ALL_WRITE);
	/* [한국어] [BAR5 / 오프셋 0x24] 같은 이유. 여섯 BAR 를 하나도 빠짐없이 가상화해야
	 * 의미가 있다 — 하나라도 통과시키면 그 창으로 공격이 성립한다. */
	p_setd(perm, PCI_BASE_ADDRESS_5, ALL_VIRT, ALL_WRITE);
	/* [한국어] [Expansion ROM BAR / 오프셋 0x30, 4바이트] 같은 이유로 완전 에뮬레이션.
	 * ROM BAR 는 비트 0 이 Enable 비트라는 점만 다르고, vfio_bar_fixup 이 그
	 * 비트를 마스크에 살려 사용자 값이 보존되게 한다. */
	p_setd(perm, PCI_ROM_ADDRESS, ALL_VIRT, ALL_WRITE);

	/* Allow us to adjust capability chain */
	/* [한국어] [Capabilities Pointer / 오프셋 0x34, 1바이트] 에뮬레이션 + 쓰기 거부.
	 * 상류 주석의 "capability chain 을 조정할 수 있게" 가 그 이유다. 이 바이트가
	 * chain 의 머리이므로, 첫 capability 를 감춰야 할 때 vfio_cap_init 이 여기를
	 * 고쳐 목록의 시작점을 옮긴다. 쓰기를 막는 이유는 사용자가 chain 의 머리를
	 * 바꾸면 지도(pci_config_map)와 사용자가 믿는 구조가 어긋나기 때문이다. */
	p_setb(perm, PCI_CAPABILITY_LIST, (u8)ALL_VIRT, NO_WRITE);

	/* Sometimes used by sw, just virtualize */
	/* [한국어] [Interrupt Line / 오프셋 0x3C, 1바이트] 에뮬레이션 + 쓰기 허용.
	 * 이 바이트는 하드웨어적 기능이 없는 순수 소프트웨어 낙서장이다(BIOS 가
	 * IRQ 번호를 적어 두면 OS 가 읽는 관례). 상류 주석대로 "소프트웨어가 가끔
	 * 쓰니 그냥 가상화" 하면 된다 — 호스트의 진짜 IRQ 번호를 사용자에게
	 * 알려 줄 이유도 없고, 사용자가 무엇을 적든 하드웨어는 무관심하다. */
	p_setb(perm, PCI_INTERRUPT_LINE, (u8)ALL_VIRT, (u8)ALL_WRITE);

	/* Virtualize interrupt pin to allow hiding INTx */
	/* [한국어] [Interrupt Pin / 오프셋 0x3D, 1바이트] 에뮬레이션 + 쓰기 거부.
	 * 상류 주석의 "INTx 를 숨길 수 있도록" 이 이유다. VFIO 는 INTx 를 아예
	 * 쓰지 못하게 해야 할 때가 많다 — 설정으로 껐거나(CONFIG_VFIO_PCI_INTX 가
	 * 없거나 nointx), 디바이스의 INTx 가 고장 났거나, IRQ 가 배정되지 않은
	 * 경우다. 그때 vfio_config_init 이 vconfig 의 이 바이트를 0 으로 만들면
	 * 사용자에게는 "INTx 핀이 없는 디바이스" 로 보인다. VF 도 SR-IOV 스펙상
	 * 이 레지스터가 0 이어야 해서 같은 방법으로 바로잡는다.
	 * 쓰기를 막는 이유는 이 값이 vfio_pci_intrs.c 의 INTx 벡터 개수 판정 기준이
	 * 되기 때문이다 — 사용자가 마음대로 바꾸면 커널이 없는 핀을 있다고 믿는다. */
	p_setb(perm, PCI_INTERRUPT_PIN, (u8)ALL_VIRT, (u8)NO_WRITE);

	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * vfio_lock_and_set_power_state - 전원 상태 전이를 락과 매핑 무효화로 감싸 안전하게 수행한다
 *
 * @vdev: 대상 디바이스.
 * @state: 옮겨 갈 전원 상태(D0/D1/D2/D3hot).
 * @return: 없다. 전이 실패는 무시된다.
 *
 * 왜 필요한가: D3 로 내려가면 BAR 접근이 정의되지 않는다. 그런데 사용자는
 * 이미 BAR 를 mmap 해 두었을 수 있다. 그 매핑을 그대로 두면 사용자 프로세스가
 * 전원이 꺼진 디바이스에 MMIO 를 찍어 플랫폼에 따라 호스트를 죽일 수 있다.
 * 그래서 D3 이상으로 갈 때는 매핑을 먼저 걷어내고 페이지 테이블 항목을
 * 지운 뒤에 전이해야 한다. 상류 주석이 "전원 관련 변수를 보호하는 락을 전부
 * 잡은 뒤 vfio_pci_set_power_state 를 부른다" 고 밝힌 그 함수다.
 *
 * 동작 과정:
 *  1. 목표가 D3hot 이상이면 매핑을 걷어내고 memory_lock 을 쓰기 모드로 잡고,
 *     dma-buf 로 내보낸 영역도 revoke 한다. D0~D2 로 가는 것이면 접근이
 *     계속 유효하므로 락만 잡는다.
 *  2. 실제 전원 전이를 수행한다.
 *  3. 전이 뒤에도 메모리 접근이 허용되는 상태라면 dma-buf 를 되살린다.
 *  4. 락을 푼다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 사용자의 config 쓰기 처리 도중에 불린다.
 * memory_lock 을 쓰기 모드로 잡으므로 그 순간 모든 BAR 접근과 폴트가 멈춘다.
 *
 * 에러 경로: vfio_pci_set_power_state 의 반환값을 보지 않는다. 전이가 실패해도
 * 락과 매핑 상태는 일관되게 정리되므로 안전 측면의 문제는 없다.
 *
 * 호출 체인:
 *   vfio_pm_config_write → [vfio_lock_and_set_power_state]
 *     → vfio_pci_zap_and_down_write_memory_lock / vfio_pci_dma_buf_move /
 *       vfio_pci_set_power_state / __vfio_pci_memory_enabled
 */
/*
 * It takes all the required locks to protect the access of power related
 * variables and then invokes vfio_pci_set_power_state().
 */
static void vfio_lock_and_set_power_state(struct vfio_pci_core_device *vdev,
					  pci_power_t state)
{
	/* [한국어] D3hot 이상으로 내려가면 BAR 접근이 정의되지 않는다. 사용자가 이미
	 * mmap 해 둔 매핑을 먼저 무효화해야 한다. */
	if (state >= PCI_D3hot) {
		/* [한국어] BAR 매핑을 걷어내고 memory_lock 을 쓰기 모드로 잡는다. */
		vfio_pci_zap_and_down_write_memory_lock(vdev);
		/* [한국어] dma-buf 로 내보낸 영역도 revoke 한다. */
		vfio_pci_dma_buf_move(vdev, true);
	} else {
		/* [한국어] D0~D2 로 가는 경우는 접근이 계속 유효하므로 매핑을 그대로 두고 락만
		 * 잡는다. 그래도 락은 필요하다 — 전원 상태 변경과 폴트 처리가 겹치면
		 * 안 되기 때문이다. */
		down_write(&vdev->memory_lock);
	}

	/* [한국어] 실제 전원 전이. NoSoftRst- 디바이스의 상태 저장/복원까지 그 안에서
	 * 처리된다. 반환값을 보지 않는 이유는, 실패해도 아래 락/매핑 정리는
	 * 똑같이 필요하고 사용자에게 알릴 통로도 없기 때문이다. */
	vfio_pci_set_power_state(vdev, state);
	/* [한국어] 전이 뒤에도 메모리 접근이 허용되는 상태인지 다시 판정한다. */
	if (__vfio_pci_memory_enabled(vdev))
		/* [한국어] 유효하면 dma-buf 를 되살린다. */
		vfio_pci_dma_buf_move(vdev, false);
	/* [한국어] 락 해제. 이 시점부터 새 폴트가 새 전원 상태로 판정된다. */
	up_write(&vdev->memory_lock);
}

/* [한국어]
 * vfio_pm_config_write - Power Management capability 쓰기 콜백. 전원 상태 요청을 커널 API 로 번역한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 쓸 바이트 수.
 * @perm: PM capability 권한 표.
 * @offset: PM capability 시작점 기준 오프셋.
 * @val: 사용자가 쓴 값.
 * @return: 쓴 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: 상류 주석이 밝히듯 전원 관리는 "함수 단위" 로 정의돼 있어
 * 사용자가 자기 함수의 전원을 바꾸는 것 자체는 허용해도 된다. 다만 커널도
 * 그 사실을 알아야 한다 — 커널은 pdev->current_state 로 전원 상태를 추적하고
 * 그 값에 따라 config 접근과 MMIO 접근의 허용 여부를 정하기 때문이다. 그래서
 * 상태 비트는 쓰기 금지로 두고(표에서 STATE 마스크를 write 에서 제외한다),
 * 쓰기를 가로채 커널 API 로 같은 일을 대신 수행한다.
 *
 * 동작 과정:
 *  1. 기본 엔진으로 먼저 쓴다. 표 덕분에 상태 비트는 하드웨어에 닿지 않고,
 *     PME 관련 비트는 vconfig 에만 들어간다.
 *  2. 실패했으면 그대로 반환.
 *  3. 쓴 위치가 PM Control 레지스터면 사용자가 요청한 전원 상태 코드를
 *     꺼낸다. 마스크가 2비트라 0~3 네 값만 나오고, 그래서 switch 에 default
 *     가 없어도 state 가 반드시 초기화된다. 다만 그 마스크 상수의 정의는
 *     이 트리에 pci_regs.h 가 없어 확인 못 함 — 코드 구조상 네 가지를 모두
 *     적어 두었다는 사실만 확인했다.
 *  4. vfio_lock_and_set_power_state 로 락과 매핑 정리를 곁들여 전이한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 4단계에서 memory_lock 을 쓰기 모드로 잡는다.
 *
 * 에러 경로: 1단계 실패면 전원 전이를 시도조차 하지 않는다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(= 이 함수)
 *     → [vfio_pm_config_write]
 *     → vfio_default_config_write, vfio_lock_and_set_power_state
 */
static int vfio_pm_config_write(struct vfio_pci_core_device *vdev, int pos,
				int count, struct perm_bits *perm,
				int offset, __le32 val)
{
	/* [한국어] 기본 엔진으로 먼저 쓴다. 표 덕분에 전원 상태 비트는 하드웨어에 닿지
	 * 않고 PME 관련 비트는 vconfig 에만 들어간다. */
	count = vfio_default_config_write(vdev, pos, count, perm, offset, val);
	if (count < 0)
		return count;

	/* [한국어] 쓴 위치가 PM Control 레지스터일 때만 전원 전이를 검토한다. */
	if (offset == PCI_PM_CTRL) {
		/* [한국어] 번역된 커널 전원 상태 값. 아래 switch 가 반드시 채운다. */
		pci_power_t state;

		/* [한국어] 사용자가 요청한 전원 상태 코드를 뽑는다. 상태 필드는 2비트라 0~3 네
		 * 값만 나오고, 그래서 default 가 없어도 state 가 초기화되지 않은 채로
		 * 쓰이는 일이 없다. 다만 이 마스크 상수의 정의는 이 트리에 pci_regs.h 가
		 * 없어 확인 못 함 — 네 가지 case 를 모두 적어 두었다는 코드 구조로만
		 * 그 폭을 짐작한다. */
		switch (le32_to_cpu(val) & PCI_PM_CTRL_STATE_MASK) {
		/* [한국어] 코드 0 = D0, 완전 동작 상태. */
		case 0:
			/* [한국어] 커널 표현으로 번역. */
			state = PCI_D0;
			break;
		/* [한국어] 코드 1 = D1, 얕은 절전. */
		case 1:
			/* [한국어] 커널 표현으로 번역. */
			state = PCI_D1;
			break;
		/* [한국어] 코드 2 = D2, 더 깊은 절전. */
		case 2:
			/* [한국어] 커널 표현으로 번역. */
			state = PCI_D2;
			break;
		/* [한국어] 코드 3 = D3hot, config 접근만 살아 있는 절전. */
		case 3:
			/* [한국어] 커널 표현으로 번역. 이 값부터는 BAR 접근이 금지되므로 아래 호출이
			 * 매핑을 걷어낸다. */
			state = PCI_D3hot;
			break;
		}

		/* [한국어] 락과 매핑 정리를 곁들여 실제 전이를 수행한다. 사용자가 하드웨어에
		 * 직접 쓰지 못하게 막아 두고 커널이 같은 일을 대신하는 구조다. */
		vfio_lock_and_set_power_state(vdev, state);
	}

	/* [한국어] 쓴 바이트 수. */
	return count;
}

/* [한국어]
 * init_pci_cap_pm_perm - Power Management capability 의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(cap_perms 의 PM 슬롯).
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: PM capability 에서 사용자에게 내주면 안 되는 것은 두 가지다.
 * (a) 전원 상태 비트 — 커널이 추적해야 하므로 가로채 대신 수행한다.
 * (b) PME(Power Management Event) 관련 비트 — 게스트는 PME 를 처리할 수 없고
 *     실제 PME 는 호스트가 받아 상태를 지우기 때문에, 사용자에게는 "PME 를
 *     지원하지 않는 디바이스" 로 보이게 감춘다. 상류 주석이 그 이유를 적어
 *     두었고, vconfig 쪽 실제 비트 지우기는 vfio_update_pm_vconfig_bytes 가 한다.
 *
 * 동작 과정: 표를 만들고 전용 writefn 을 심은 뒤 세 자리를 뚫는다.
 *
 * 실행 컨텍스트: 모듈 적재 시점(__init). 프로세스 문맥.
 *
 * 에러 경로: -ENOMEM 만 올린다.
 *
 * 호출 체인:
 *   vfio_pci_init_perm_bits → [init_pci_cap_pm_perm]
 *     → alloc_perm_bits, p_setb, p_setw, p_setd
 */
/* Permissions for the Power Management capability */
static int __init init_pci_cap_pm_perm(struct perm_bits *perm)
{
	/* [한국어] PM capability 는 고정 길이라 길이표에서 바로 꺼내 쓴다. */
	if (alloc_perm_bits(perm, pci_cap_length[PCI_CAP_ID_PM]))
		return -ENOMEM;

	/* [한국어] 전용 쓰기 콜백. 전원 상태 요청을 커널 API 로 번역한다. readfn 은
	 * alloc_perm_bits 가 심은 기본 엔진 그대로다. */
	perm->writefn = vfio_pm_config_write;

	/*
	 * We always virtualize the next field so we can remove
	 * capabilities from the chain if we want to.
	 */
	/* [한국어] [Next Pointer / capability 내 오프셋 0x01, 1바이트] 에뮬레이션 + 쓰기 거부.
	 * 상류 주석대로 "필요하면 chain 에서 capability 를 빼낼 수 있도록 next 는
	 * 언제나 가상화한다". 이 한 줄이 모든 capability 표에 똑같이 등장하며,
	 * 그것이 이 파일의 chain 재작성을 가능하게 하는 공통 장치다.
	 * 쓰기를 막는 이유는 사용자가 chain 구조를 바꾸면 지도와 어긋나기 때문이다. */
	p_setb(perm, PCI_CAP_LIST_NEXT, (u8)ALL_VIRT, NO_WRITE);

	/*
	 * The guests can't process PME events. If any PME event will be
	 * generated, then it will be mostly handled in the host and the
	 * host will clear the PME_STATUS. So virtualize PME_Support bits.
	 * The vconfig bits will be cleared during device capability
	 * initialization.
	 */
	/* [한국어] [PM Capabilities / capability 내 오프셋 0x02, 2바이트] PME_Support 필드만
	 * 에뮬레이션, 쓰기는 전면 거부.
	 * 왜 그 필드만 가상화하는가 — 상류 주석이 이유를 밝힌다. 게스트는 PME
	 * (Power Management Event)를 처리할 수 없다. PME 가 실제로 발생하면 대부분
	 * 호스트가 받아 처리하고 호스트가 PME_Status 를 지운다. 그러니 사용자에게는
	 * "PME 를 지원하지 않는 디바이스" 로 보이는 편이 일관적이다. 실제로 그 값을
	 * 0 으로 만드는 것은 vfio_update_pm_vconfig_bytes 이며, 상류 주석이 그 사실을
	 * "vconfig 비트는 디바이스 capability 초기화 중에 지워진다" 로 예고한다.
	 * 나머지 비트(버전, D1/D2 지원 여부 등)는 통과라 하드웨어 값이 그대로 보인다. */
	p_setw(perm, PCI_PM_PMC, PCI_PM_CAP_PME_MASK, NO_WRITE);

	/*
	 * Power management is defined *per function*, so we can let
	 * the user change power state, but we trap and initiate the
	 * change ourselves, so the state bits are read-only.
	 *
	 * The guest can't process PME from D3cold so virtualize PME_Status
	 * and PME_En bits. The vconfig bits will be cleared during device
	 * capability initialization.
	 */
	/* [한국어] [PM Control/Status / capability 내 오프셋 0x04, 4바이트] 이 표에서 가장
	 * 복잡한 한 줄. 가상화 마스크는 PME_En 과 PME_Status 두 비트이고, 쓰기
	 * 허용은 "그 두 비트와 전원 상태 필드를 뺀 나머지 전부" 다.
	 * - 전원 상태 필드(하위 2비트): 쓰기 거부 + 가상화 안 함. 상류 주석대로
	 *   전원 관리는 함수 단위라 사용자가 바꾸는 것 자체는 허용해도 되지만,
	 *   커널이 pdev->current_state 로 상태를 추적해야 하므로 하드웨어 직통
	 *   쓰기는 막고 writefn 이 가로채 커널 API 로 대신 수행한다. 그래서
	 *   "상태 비트는 읽기 전용" 이다.
	 * - PME_En 과 PME_Status: 에뮬레이션. D3cold 에서 온 PME 를 게스트가 처리할
	 *   수 없으므로 사용자에게는 늘 꺼져 있는 것처럼 보여야 한다.
	 * - 나머지(데이터 선택자, 데이터 스케일 등): 통과 + 쓰기 허용. 그 함수
	 *   자신의 전력 데이터 보고 방식일 뿐이라 안전하다. */
	p_setd(perm, PCI_PM_CTRL,
	       PCI_PM_CTRL_PME_ENABLE | PCI_PM_CTRL_PME_STATUS,
	       ~(PCI_PM_CTRL_PME_ENABLE | PCI_PM_CTRL_PME_STATUS |
		 PCI_PM_CTRL_STATE_MASK));

	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * vfio_vpd_config_write - VPD capability 쓰기 콜백. 간접 접근 프로토콜을 커널이 대신 수행한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 쓸 바이트 수.
 * @perm: VPD capability 권한 표.
 * @offset: VPD capability 시작점 기준 오프셋.
 * @val: 사용자가 쓴 값.
 * @return: 쓴 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: VPD(Vital Product Data)는 주소 레지스터와 데이터 레지스터
 * 한 쌍으로 된 간접 접근 창이다. 주소를 쓰고 플래그 비트를 토글하면
 * 하드웨어가 완료 신호로 그 비트를 되돌리는 핸드셰이크 구조인데, 이 대기
 * 루프를 사용자가 직접 돌게 두면 하드웨어를 오래 붙잡거나 잘못된 순서로
 * 접근해 문제를 일으킬 수 있다. 게다가 커널에는 이미 검증된 VPD 접근
 * 구현이 있다(drivers/pci/vpd.c). 그래서 주소와 데이터 레지스터를 통째로
 * 가상화하고, 사용자가 핸드셰이크를 시작하는 순간 커널 API 로 대행한다.
 *
 * 동작 과정:
 *  1. 이 capability 안의 주소/데이터 레지스터 위치를 vconfig 에서 계산한다.
 *     pos - offset 이 capability 의 시작 절대 오프셋이므로, 거기에 각
 *     레지스터의 상대 오프셋을 더한다.
 *  2. 기본 엔진으로 먼저 vconfig 에 반영한다(두 레지스터 모두 전면 가상화라
 *     하드웨어에는 닿지 않는다).
 *  3. 이번 쓰기가 주소 레지스터의 상위 바이트를 포함하지 않으면 할 일이 없다.
 *     플래그 비트가 그 바이트에 있기 때문이다. 상류 주석이 그 판정 근거를
 *     적어 두었다.
 *  4. vconfig 의 주소 값을 읽어 플래그 비트를 본다.
 *     - 플래그가 1 이면 쓰기 요청이다. 데이터 레지스터의 4바이트를 커널
 *       VPD 쓰기 API 로 실제 하드웨어에 내려보낸다.
 *     - 0 이면 읽기 요청이다. 커널 VPD 읽기 API 로 4바이트를 가져와
 *       데이터 레지스터의 vconfig 값을 채운다.
 *     어느 쪽이든 실패하면 플래그를 토글하지 않고 그냥 반환한다.
 *  5. 성공했으면 플래그 비트를 XOR 로 뒤집어 완료를 알린다. 상류 주석이
 *     밝히듯, 실패 시 토글하지 않는 것 자체가 사용자 드라이버의 타임아웃을
 *     유도하는 오류 통지 방식이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 커널 VPD 접근은 하드웨어 핸드셰이크를
 * 기다리므로 상당 시간 잠들 수 있다.
 *
 * 에러 경로: 위 4단계의 실패는 오류 코드가 아니라 "플래그 미토글" 로 표현된다.
 * 쓰기 자체는 성공했으므로 count 를 반환한다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(= 이 함수)
 *     → [vfio_vpd_config_write]
 *     → vfio_default_config_write, pci_write_vpd (drivers/pci/vpd.c:1416),
 *       pci_read_vpd (drivers/pci/vpd.c:1304)
 */
static int vfio_vpd_config_write(struct vfio_pci_core_device *vdev, int pos,
				 int count, struct perm_bits *perm,
				 int offset, __le32 val)
{
	/* [한국어] 커널 VPD API 에 넘길 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 그림자 복사본 안의 VPD 주소 레지스터 위치. pos - offset 이 이 capability 의
	 * 시작 절대 오프셋이라는 점이 계산의 핵심이다 — offset 은 capability 안에서의
	 * 상대 위치이므로 빼면 시작점이 나온다. */
	__le16 *paddr = (__le16 *)(vdev->vconfig + pos - offset + PCI_VPD_ADDR);
	/* [한국어] 같은 방식으로 데이터 레지스터 위치를 구한다. */
	__le32 *pdata = (__le32 *)(vdev->vconfig + pos - offset + PCI_VPD_DATA);
	/* [한국어] 사용자가 요청한 VPD 주소와 완료 플래그. */
	u16 addr;
	/* [한국어] 실제로 주고받을 4바이트 데이터. */
	u32 data;

	/*
	 * Write through to emulation.  If the write includes the upper byte
	 * of PCI_VPD_ADDR, then the PCI_VPD_ADDR_F bit is written and we
	 * have work to do.
	 */
	/* [한국어] 먼저 vconfig 에 반영한다. 두 레지스터 모두 전면 가상화라 하드웨어에는
	 * 닿지 않는다. */
	count = vfio_default_config_write(vdev, pos, count, perm, offset, val);
	/* [한국어] 세 가지 중 하나면 여기서 끝난다 — 쓰기 실패, 이번 쓰기가 주소 레지스터
	 * 뒤에서 시작했거나, 주소 레지스터의 상위 바이트에 닿기 전에 끝난 경우다.
	 * 상류 주석대로 완료 플래그가 그 상위 바이트에 있어서, 그 바이트를 건드리지
	 * 않았다면 핸드셰이크가 시작되지 않은 것이다. */
	if (count < 0 || offset > PCI_VPD_ADDR + 1 ||
	    offset + count <= PCI_VPD_ADDR + 1)
		return count;

	/* [한국어] 방금 사용자가 쓴 주소와 플래그를 그림자 복사본에서 읽는다. */
	addr = le16_to_cpu(*paddr);

	/* [한국어] 플래그 비트가 1 이면 쓰기 요청이다(PCI 스펙의 VPD 프로토콜 규약). */
	if (addr & PCI_VPD_ADDR_F) {
		/* [한국어] 사용자가 데이터 레지스터에 넣어 둔 값을 꺼낸다. */
		data = le32_to_cpu(*pdata);
		/* [한국어] 플래그 비트를 지운 순수 주소로 커널 VPD 쓰기를 수행한다
		 * (drivers/pci/vpd.c:1416). 4바이트를 다 쓰지 못했으면 실패로 본다.
		 * 실패 시 플래그를 토글하지 않고 그냥 반환해 사용자 드라이버의 타임아웃을
		 * 유도한다. */
		if (pci_write_vpd(pdev, addr & ~PCI_VPD_ADDR_F, 4, &data) != 4)
			return count;
	} else {
		/* [한국어] 읽기 요청 경로. 읽기 실패 시 쓰레기가 남지 않도록 0 으로 시작한다. */
		data = 0;
		/* [한국어] 커널 VPD 읽기(drivers/pci/vpd.c:1304). 읽기 요청에서는 플래그가 0 이라
		 * 주소를 마스크할 필요가 없다. */
		if (pci_read_vpd(pdev, addr, 4, &data) < 0)
			return count;
		/* [한국어] 가져온 값을 그림자 복사본의 데이터 레지스터에 채운다. 사용자는 이
		 * 값을 읽어 간다. */
		*pdata = cpu_to_le32(data);
	}

	/*
	 * Toggle PCI_VPD_ADDR_F in the emulated PCI_VPD_ADDR register to
	 * signal completion.  If an error occurs above, we assume that not
	 * toggling this bit will induce a driver timeout.
	 */
	/* [한국어] 완료 플래그를 뒤집는다. 읽기 요청이었으면 0 에서 1 로, 쓰기 요청이었으면
	 * 1 에서 0 으로 바뀌어 스펙의 완료 신호가 된다. */
	addr ^= PCI_VPD_ADDR_F;
	/* [한국어] 뒤집은 값을 그림자 복사본에 되쓴다. 사용자가 이 비트를 폴링하다가
	 * 완료를 확인한다. */
	*paddr = cpu_to_le16(addr);

	/* [한국어] 쓴 바이트 수. */
	return count;
}

/* [한국어]
 * init_pci_cap_vpd_perm - VPD capability 의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(cap_perms 의 VPD 슬롯).
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: VPD 는 주소/데이터 두 레지스터를 통째로 가상화해야 커널이
 * 간접 접근 프로토콜을 대행할 수 있다. 상류 주석이 그 의도를 적어 두었다.
 *
 * 동작 과정: 표를 만들고 전용 writefn 을 심은 뒤 next 포인터와 두 레지스터를
 * 설정한다.
 *
 * 실행 컨텍스트: 모듈 적재 시점(__init).
 *
 * 에러 경로: -ENOMEM 만 올린다.
 *
 * 호출 체인:
 *   vfio_pci_init_perm_bits → [init_pci_cap_vpd_perm]
 *     → alloc_perm_bits, p_setb, p_setw, p_setd
 */
/* Permissions for Vital Product Data capability */
static int __init init_pci_cap_vpd_perm(struct perm_bits *perm)
{
	/* [한국어] VPD capability 는 고정 길이(주소 2바이트 + 데이터 4바이트 + 헤더)다. */
	if (alloc_perm_bits(perm, pci_cap_length[PCI_CAP_ID_VPD]))
		return -ENOMEM;

	/* [한국어] 전용 쓰기 콜백. 간접 접근 핸드셰이크를 커널 VPD API 로 대행한다. */
	perm->writefn = vfio_vpd_config_write;

	/*
	 * We always virtualize the next field so we can remove
	 * capabilities from the chain if we want to.
	 */
	/* [한국어] [Next Pointer] 모든 capability 표 공통 — chain 재작성을 위해 에뮬레이션,
	 * 사용자 쓰기는 거부. */
	p_setb(perm, PCI_CAP_LIST_NEXT, (u8)ALL_VIRT, NO_WRITE);

	/*
	 * Both the address and data registers are virtualized to
	 * enable access through the pci_vpd_read/write functions
	 */
	/* [한국어] [VPD Address / capability 내 오프셋 0x02, 2바이트] 완전 에뮬레이션 +
	 * 쓰기 허용.
	 * 왜 안전한가 — 상류 주석대로 두 레지스터를 모두 가상화해야 커널의
	 * pci_read_vpd/pci_write_vpd 경로로 접근을 돌릴 수 있다. 사용자가 주소와
	 * 완료 플래그를 자유롭게 쓰되 하드웨어에는 닿지 않고, writefn 이 그 요청을
	 * 읽어 커널 API 로 대신 수행한 뒤 플래그를 뒤집어 완료를 알린다. 이렇게
	 * 하면 사용자가 하드웨어 핸드셰이크 루프를 직접 돌지 않으므로 디바이스를
	 * 오래 붙잡거나 잘못된 순서로 접근할 수 없다. */
	p_setw(perm, PCI_VPD_ADDR, (u16)ALL_VIRT, (u16)ALL_WRITE);
	/* [한국어] [VPD Data / capability 내 오프셋 0x04, 4바이트] 완전 에뮬레이션 +
	 * 쓰기 허용. 주소 레지스터와 짝을 이루며, 읽기 요청이면 커널이 가져온
	 * 값이 여기 채워지고 쓰기 요청이면 여기 있는 값이 하드웨어로 내려간다. */
	p_setd(perm, PCI_VPD_DATA, ALL_VIRT, ALL_WRITE);

	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * init_pci_cap_pcix_perm - PCI-X capability 의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(cap_perms 의 PCI-X 슬롯).
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: PCI-X 는 PCIe 이전 세대의 고속 병렬 버스다. 여기서 사용자가
 * 건드릴 수 있는 것은 명령 레지스터와 ECC 제어/상태 레지스터인데, 둘 다
 * 그 함수 자신의 버스 동작에만 영향을 주므로 하드웨어 직통 쓰기를 허용해도
 * 호스트에 위험이 없다. 별도 readfn/writefn 이 없어 기본 엔진만 쓴다.
 *
 * 동작 과정: 최대 크기(v2)로 표를 만들고 세 자리를 뚫는다. 상류 주석대로
 * v0 디바이스는 앞 8바이트만 쓰지만, 표는 크게 잡아 두어도 무해하다.
 *
 * 실행 컨텍스트: 모듈 적재 시점(__init).
 *
 * 에러 경로: -ENOMEM 만 올린다.
 *
 * 호출 체인:
 *   vfio_pci_init_perm_bits → [init_pci_cap_pcix_perm]
 *     → alloc_perm_bits, p_setb, p_setw, p_setd
 */
/* Permissions for PCI-X capability */
static int __init init_pci_cap_pcix_perm(struct perm_bits *perm)
{
	/* Alloc 24, but only 8 are used in v0 */
	/* [한국어] 상류 주석대로 24바이트(v2)를 잡아 두지만 v0 디바이스는 앞 8바이트만
	 * 쓴다. 크게 잡아 두면 어느 버전이든 표 밖으로 나가지 않는다. */
	if (alloc_perm_bits(perm, PCI_CAP_PCIX_SIZEOF_V2))
		return -ENOMEM;

	/* [한국어] [Next Pointer] chain 재작성용 공통 항목. */
	p_setb(perm, PCI_CAP_LIST_NEXT, (u8)ALL_VIRT, NO_WRITE);

	/* [한국어] [PCI-X Command / capability 내 오프셋 0x02, 2바이트] 통과 + 쓰기 허용.
	 * 왜 안전한가 — 이 레지스터는 이 함수가 발행하는 트랜잭션의 최대 크기와
	 * 미완료 분할 트랜잭션 개수, 릴랙스드 오더링 사용 여부를 정한다. 전부 그
	 * 함수 자신의 버스 동작에만 영향을 주고, 잘못 설정해도 그 디바이스의 성능이
	 * 나빠질 뿐이다. 전용 writefn 이 없어 기본 엔진이 하드웨어에 그대로 반영한다. */
	p_setw(perm, PCI_X_CMD, NO_VIRT, (u16)ALL_WRITE);
	/* [한국어] [PCI-X ECC Control and Status / capability 내 오프셋 0x04, 4바이트]
	 * 통과 + 쓰기 허용. ECC 오류 주입과 상태 지우기가 들어 있는데, 이 역시
	 * 그 함수의 버스 인터페이스 안에서 끝난다. AER 의 상태 레지스터를 열어 주는
	 * 것과 같은 판단이다. */
	p_setd(perm, PCI_X_ECC_CSR, NO_VIRT, ALL_WRITE);
	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * vfio_exp_config_write - PCI Express capability 쓰기 콜백. 가상 FLR 비트와 MRRS 하한을 처리한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 쓸 바이트 수.
 * @perm: PCIe capability 권한 표.
 * @offset: PCIe capability 시작점 기준 오프셋.
 * @val: 사용자가 쓴 값.
 * @return: 쓴 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: PCIe Device Control 레지스터에는 두 개의 특수 필드가 있다.
 * (a) Initiate Function Level Reset — 사용자가 이 비트를 세우면 디바이스가
 *     리셋되는데, VFIO 는 리셋을 자기가 관리해야 한다(리셋 전에 사용자 매핑을
 *     걷어내고 dma-buf 를 revoke 해야 하며, 리셋 후 상태 복구도 필요하다).
 *     그래서 비트를 가상화해 하드웨어에 닿지 않게 하고, 여기서 커널 리셋
 *     API 로 대신 수행한다.
 * (b) Max Read Request Size — 사용자가 시스템 전체를 볼 수 없어 지나치게 낮은
 *     값을 고를 수 있다. 상류 주석대로 그러면 TLP 하나에 담기는 데이터가
 *     줄어 성능이 떨어진다. 그래서 쓰기는 통과시키되 물리 MPS 값을 하한으로
 *     깔아 준다.
 *
 * 동작 과정:
 *  1. 이 capability 안의 Device Control 레지스터 위치를 계산하고, 쓰기 전의
 *     MRRS 필드 값을 기억해 둔다.
 *  2. 기본 엔진으로 쓴다. 표 덕분에 FLR/MPS/MRRS 는 vconfig 에만 들어가고,
 *     Phantom Function 비트는 아예 쓰기가 막혀 있다.
 *  3. vconfig 에 FLR 비트가 서 있으면:
 *     a. 먼저 그 비트를 지운다. 스펙상 이 비트는 항상 0 으로 읽혀야 하므로,
 *        리셋 성공 여부와 무관하게 지우는 것이 옳다.
 *     b. 하드웨어의 Device Capabilities 를 읽어 FLR 지원 여부를 확인한다.
 *     c. 지원하면 사용자 매핑을 전부 걷어내고 memory_lock 을 쓰기 모드로 잡은
 *        뒤 dma-buf 를 revoke 하고 커널 리셋을 시도한다. 상류 주석이 밝히듯
 *        커널 리셋 API 가 반드시 PCIe FLR 을 쓴다는 보장은 없다 — 이 계층에는
 *        그만한 세밀함이 없다.
 *     d. 리셋 뒤 메모리가 다시 유효하면 dma-buf 를 되살리고 락을 푼다.
 *  4. MRRS 필드가 이번 쓰기로 바뀌었으면 새 값을 바이트 수로 환산한다.
 *     이 필드는 3비트 지수이고 128바이트에서 시작하므로 128 을 그만큼 왼쪽으로
 *     민다. 그 값을 물리 MPS 와 비교해 큰 쪽을 골라 하드웨어에 실제로 적용한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 3c 에서 memory_lock 을 쓰기 모드로 잡고 실제
 * 디바이스 리셋을 수행하므로 상당히 오래 걸릴 수 있다.
 *
 * 에러 경로: 2단계 실패면 즉시 반환한다. 3b 의 읽기 실패는 "FLR 미지원" 과
 * 같이 취급해 리셋을 건너뛴다 — 비트는 이미 3a 에서 지워졌다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(= 이 함수)
 *     → [vfio_exp_config_write]
 *     → vfio_default_config_write, pci_user_read_config_dword,
 *       vfio_pci_zap_and_down_write_memory_lock, vfio_pci_dma_buf_move,
 *       pci_try_reset_function (drivers/pci/pci.c:10347),
 *       __vfio_pci_memory_enabled, pcie_get_mps (drivers/pci/pci.c:11816),
 *       pcie_set_readrq (drivers/pci/pci.c:11719)
 */
static int vfio_exp_config_write(struct vfio_pci_core_device *vdev, int pos,
				 int count, struct perm_bits *perm,
				 int offset, __le32 val)
{
	/* [한국어] 그림자 복사본 안의 PCIe Device Control 레지스터 위치. pos - offset 이
	 * capability 시작 절대 오프셋이므로 거기에 Device Control 의 상대 오프셋을
	 * 더한다. */
	__le16 *ctrl = (__le16 *)(vdev->vconfig + pos -
				  offset + PCI_EXP_DEVCTL);
	/* [한국어] 쓰기 전의 Max Read Request Size 필드를 기억해 둔다. 아래에서 값이
	 * 실제로 바뀌었는지 비교해, 바뀐 경우에만 하드웨어에 반영한다. */
	int readrq = le16_to_cpu(*ctrl) & PCI_EXP_DEVCTL_READRQ;

	/* [한국어] 기본 엔진으로 쓴다. 표에 따라 FLR, MPS, MRRS 는 vconfig 로 가고
	 * Phantom Function 비트는 쓰기 자체가 막힌다. */
	count = vfio_default_config_write(vdev, pos, count, perm, offset, val);
	if (count < 0)
		return count;

	/*
	 * The FLR bit is virtualized, if set and the device supports PCIe
	 * FLR, issue a reset_function.  Regardless, clear the bit, the spec
	 * requires it to be always read as zero.  NB, reset_function might
	 * not use a PCIe FLR, we don't have that level of granularity.
	 */
	/* [한국어] 사용자가 Function Level Reset 을 요청했다. 이 비트는 표에서 가상화
	 * 대상이라 하드웨어에는 닿지 않았고 vconfig 에만 서 있다. */
	if (*ctrl & cpu_to_le16(PCI_EXP_DEVCTL_BCR_FLR)) {
		/* [한국어] Device Capabilities 레지스터 값을 담을 곳. */
		u32 cap;
		/* [한국어] config 읽기 결과 코드. */
		int ret;

		/* [한국어] 먼저 비트를 지운다. 상류 주석대로 스펙이 이 비트를 항상 0 으로 읽히도록
		 * 요구하므로, 리셋 수행 여부와 무관하게 지우는 것이 옳다. */
		*ctrl &= ~cpu_to_le16(PCI_EXP_DEVCTL_BCR_FLR);

		/* [한국어] 하드웨어의 Device Capabilities 를 읽어 FLR 지원 여부를 확인한다.
		 * 사용자가 지원하지 않는 디바이스에 FLR 을 요청할 수도 있다. */
		ret = pci_user_read_config_dword(vdev->pdev,
						 pos - offset + PCI_EXP_DEVCAP,
						 &cap);

		/* [한국어] 읽기가 성공했고 FLR 을 지원하는 경우에만 실제로 리셋한다. */
		if (!ret && (cap & PCI_EXP_DEVCAP_FLR)) {
			/* [한국어] 리셋 전에 사용자 BAR 매핑을 전부 걷어내고 memory_lock 을 쓰기 모드로
			 * 잡는다. 리셋 중에 사용자가 MMIO 를 찍으면 무엇이 벌어질지 알 수 없다. */
			vfio_pci_zap_and_down_write_memory_lock(vdev);
			/* [한국어] dma-buf 로 내보낸 영역도 revoke 한다. */
			vfio_pci_dma_buf_move(vdev, true);
			/* [한국어] 커널 리셋 API 로 실제 리셋을 수행한다(drivers/pci/pci.c:10347).
			 * 상류 주석이 밝히듯 이 API 가 반드시 PCIe FLR 을 쓴다는 보장은 없다 —
			 * 이 계층에는 리셋 방식을 고를 만큼의 세밀함이 없다. 반환값을 보지 않는데,
			 * 사용자에게 리셋 실패를 알릴 통로가 이 config 쓰기 경로에는 없기 때문이다. */
			pci_try_reset_function(vdev->pdev);
			/* [한국어] 리셋 뒤에도 메모리 접근이 유효한 상태인지 다시 판정한다. */
			if (__vfio_pci_memory_enabled(vdev))
				/* [한국어] 유효하면 dma-buf 를 되살린다. */
				vfio_pci_dma_buf_move(vdev, false);
			/* [한국어] 락 해제. */
			up_write(&vdev->memory_lock);
		}
	}

	/*
	 * MPS is virtualized to the user, writes do not change the physical
	 * register since determining a proper MPS value requires a system wide
	 * device view.  The MRRS is largely independent of MPS, but since the
	 * user does not have that system-wide view, they might set a safe, but
	 * inefficiently low value.  Here we allow writes through to hardware,
	 * but we set the floor to the physical device MPS setting, so that
	 * we can at least use full TLPs, as defined by the MPS value.
	 *
	 * NB, if any devices actually depend on an artificially low MRRS
	 * setting, this will need to be revisited, perhaps with a quirk
	 * though pcie_set_readrq().
	 */
	/* [한국어] MRRS 필드가 이번 쓰기로 실제로 바뀌었는지 비교한다. 바뀌지 않았으면
	 * 하드웨어를 건드릴 이유가 없다. */
	if (readrq != (le16_to_cpu(*ctrl) & PCI_EXP_DEVCTL_READRQ)) {
		/* [한국어] 3비트 지수 필드를 바이트 수로 환산한다. 128바이트가 기준값이고 필드
		 * 값만큼 두 배씩 커지므로, 필드를 12비트만큼 오른쪽으로 밀어 지수를 얻은 뒤
		 * 128 을 그만큼 왼쪽으로 민다. */
		readrq = 128 <<
			((le16_to_cpu(*ctrl) & PCI_EXP_DEVCTL_READRQ) >> 12);
		/* [한국어] 물리 MPS 를 하한으로 깐다. 상류 주석이 근거를 밝힌다 — 사용자는 시스템
		 * 전체를 볼 수 없어 안전하지만 비효율적으로 낮은 MRRS 를 고를 수 있다.
		 * 최소한 MPS 만큼은 읽어야 TLP 하나를 꽉 채울 수 있다. 상류 주석은 인위적으로
		 * 낮은 MRRS 에 의존하는 디바이스가 실제로 있다면 이 결정을 재검토해야 한다는
		 * 단서도 함께 남긴다. */
		readrq = max(readrq, pcie_get_mps(vdev->pdev));

		/* [한국어] 커널 API 로 실제 MRRS 를 설정한다(drivers/pci/pci.c:11719). MRRS 만은
		 * 가상화가 아니라 통과인데, 상류 주석대로 MPS 와 달리 시스템 전체 합의가
		 * 필요 없어 사용자가 정해도 되는 값이기 때문이다. */
		pcie_set_readrq(vdev->pdev, readrq);
	}

	/* [한국어] 쓴 바이트 수. */
	return count;
}

/* [한국어]
 * init_pci_cap_exp_perm - PCI Express capability 의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(cap_perms 의 PCIe 슬롯).
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: PCIe capability 는 링크 제어, 슬롯 제어, 오류 보고 등 많은
 * 필드를 담지만 엔드포인트 사용자가 실제로 필요한 것은 Device Control 뿐이다.
 * 상류 주석이 세 가지 금지 이유를 밝힌다 — Phantom Function 은 IOMMU 가 보는
 * requester ID 를 흐트러뜨리고, MPS 는 다른 물리 디바이스와의 통신을 깨뜨리며,
 * DEVCTL2 의 ARI 비트는 probe 시점에 커널이 이미 정해 둔 값이다.
 *
 * 동작 과정: 가장 큰 크기(v2 엔드포인트)로 표를 만들고 전용 writefn 을 심은 뒤
 * next 포인터와 두 제어 레지스터를 설정한다.
 *
 * 실행 컨텍스트: 모듈 적재 시점(__init).
 *
 * 에러 경로: -ENOMEM 만 올린다.
 *
 * 호출 체인:
 *   vfio_pci_init_perm_bits → [init_pci_cap_exp_perm]
 *     → alloc_perm_bits, p_setb, p_setw
 */
/* Permissions for PCI Express capability */
static int __init init_pci_cap_exp_perm(struct perm_bits *perm)
{
	/* Alloc largest of possible sizes */
	/* [한국어] 상류 주석대로 가능한 크기 중 가장 큰 것으로 잡는다. 이 디바이스가 v1
	 * 이거나 링크 없는 엔드포인트라 더 짧더라도, 크게 잡아 두면 어느 경우든
	 * 표 밖으로 나가지 않는다. */
	if (alloc_perm_bits(perm, PCI_CAP_EXP_ENDPOINT_SIZEOF_V2))
		return -ENOMEM;

	/* [한국어] 전용 쓰기 콜백. FLR 대행과 MRRS 하한 처리가 그 안에 있다. */
	perm->writefn = vfio_exp_config_write;

	/* [한국어] [Next Pointer] chain 재작성용 공통 항목 — 에뮬레이션 + 쓰기 거부. */
	p_setb(perm, PCI_CAP_LIST_NEXT, (u8)ALL_VIRT, NO_WRITE);

	/*
	 * Allow writes to device control fields, except devctl_phantom,
	 * which could confuse IOMMU, MPS, which can break communication
	 * with other physical devices, and the ARI bit in devctl2, which
	 * is set at probe time.  FLR and MRRS get virtualized via our
	 * writefn.
	 */
	/* [한국어] [Device Control / capability 내 오프셋 0x08, 2바이트] 이 표의 핵심.
	 * 가상화 마스크는 FLR 비트, Max Payload Size 필드, Max Read Request Size
	 * 필드 세 가지이고, 쓰기 허용은 "Phantom Function 비트를 뺀 나머지 전부" 다.
	 * 레지스터별 근거는 상류 주석이 나열한다.
	 * - Phantom Function Enable: 쓰기 거부. 이 비트를 켜면 디바이스가 사용하지
	 *   않는 function 번호를 태그 공간 확장에 쓰는데, 그러면 IOMMU 가 보는
	 *   requester ID 가 여러 개로 흩어져 격리 도메인 매핑이 흐트러진다.
	 *   **이 파일에서 IOMMU 격리와 가장 직접적으로 얽힌 거부 항목이다.**
	 * - Max Payload Size: 에뮬레이션. 올바른 MPS 는 경로 위의 모든 디바이스가
	 *   합의해야 정할 수 있어서 사용자가 혼자 정하면 다른 물리 디바이스와의
	 *   통신이 깨진다. 그래서 값은 vconfig 에만 두고 하드웨어는 커널이 정한
	 *   값을 유지한다.
	 * - FLR: 에뮬레이션. writefn 이 가로채 커널 리셋 API 로 대행한다.
	 * - MRRS: 에뮬레이션 + writefn 이 하한을 깐 뒤 하드웨어에 반영. 가상화해
	 *   두는 이유는 사용자가 쓴 값을 그대로 되읽게 하기 위해서이고, 실제
	 *   적용은 pcie_set_readrq 가 한다.
	 * - 나머지(오류 보고 활성 비트들, Relaxed Ordering, No Snoop, Extended Tag,
	 *   Aux Power PM 등): 통과 + 쓰기 허용. 그 함수 자신의 트랜잭션 속성이라
	 *   호스트에 영향이 없다. */
	p_setw(perm, PCI_EXP_DEVCTL,
	       PCI_EXP_DEVCTL_BCR_FLR | PCI_EXP_DEVCTL_PAYLOAD |
	       PCI_EXP_DEVCTL_READRQ, ~PCI_EXP_DEVCTL_PHANTOM);
	/* [한국어] [Device Control 2 / capability 내 오프셋 0x28, 2바이트] 가상화 없음,
	 * ARI Forwarding Enable 비트만 빼고 쓰기 허용.
	 * 왜 ARI 만 막는가 — 상류 주석대로 그 비트는 probe 시점에 커널이 정한다.
	 * ARI(Alternative Routing-ID Interpretation)는 device/function 번호 해석
	 * 방식을 바꾸므로, 켜고 끄면 이 함수의 requester ID 자체가 달라져 IOMMU
	 * 매핑과 어긋난다.
	 * 나머지(Completion Timeout 값과 비활성화, AtomicOp 요청 활성, LTR 활성,
	 * OBFF 등)는 통과다. 그 함수 자신의 트랜잭션 정책이라 안전하다. */
	p_setw(perm, PCI_EXP_DEVCTL2, NO_VIRT, ~PCI_EXP_DEVCTL2_ARI);
	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * vfio_af_config_write - Advanced Features capability 쓰기 콜백. 가상 FLR 비트를 처리한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 쓸 바이트 수.
 * @perm: AF capability 권한 표.
 * @offset: AF capability 시작점 기준 오프셋.
 * @val: 사용자가 쓴 값.
 * @return: 쓴 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: Advanced Features 는 PCIe 가 아닌 전통 PCI 디바이스에 FLR 과
 * Transactions Pending 을 얹어 주는 작은 capability 다. 여기의 FLR 비트도
 * vfio_exp_config_write 의 PCIe FLR 과 똑같은 이유로 가로채야 한다 — VFIO 가
 * 리셋 전후로 매핑과 dma-buf 를 정리해야 하기 때문이다.
 *
 * 동작 과정:
 *  1. 이 capability 안의 Control 레지스터 위치를 계산한다. AF 의 제어
 *     레지스터는 1바이트라 u8 포인터로 다룬다.
 *  2. 기본 엔진으로 쓴다. 표에서 FLR 비트만 가상화 + 쓰기 허용이므로,
 *     사용자 값은 vconfig 에만 들어간다.
 *  3. vconfig 에 FLR 비트가 서 있으면:
 *     a. 스펙상 항상 0 으로 읽혀야 하므로 먼저 지운다.
 *     b. AF Capabilities 바이트를 읽어 FLR 지원과 Transactions Pending 지원을
 *        모두 확인한다. TP 까지 요구하는 이유는, 진행 중인 트랜잭션이 끝났는지
 *        확인할 수 없으면 안전하게 리셋할 수 없기 때문이다.
 *     c. 둘 다 지원하면 매핑 정리 + dma-buf revoke + 커널 리셋 + 복구 순으로
 *        PCIe 쪽과 동일한 절차를 밟는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 3c 에서 memory_lock 을 쓰기 모드로 잡는다.
 *
 * 에러 경로: 2단계 실패면 즉시 반환. 3b 읽기 실패는 미지원과 동일하게 처리된다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(= 이 함수)
 *     → [vfio_af_config_write]
 *     → vfio_default_config_write, pci_user_read_config_byte,
 *       vfio_pci_zap_and_down_write_memory_lock, vfio_pci_dma_buf_move,
 *       pci_try_reset_function, __vfio_pci_memory_enabled
 */
static int vfio_af_config_write(struct vfio_pci_core_device *vdev, int pos,
				int count, struct perm_bits *perm,
				int offset, __le32 val)
{
	/* [한국어] 그림자 복사본 안의 AF Control 레지스터 위치. AF 의 제어 레지스터는
	 * 1바이트라 u8 포인터로 다룬다 — 엔디언 변환이 필요 없다. */
	u8 *ctrl = vdev->vconfig + pos - offset + PCI_AF_CTRL;

	/* [한국어] 기본 엔진으로 쓴다. 표에서 FLR 비트만 가상화 + 쓰기 허용이라 사용자
	 * 값은 vconfig 에만 들어간다. */
	count = vfio_default_config_write(vdev, pos, count, perm, offset, val);
	if (count < 0)
		return count;

	/*
	 * The FLR bit is virtualized, if set and the device supports AF
	 * FLR, issue a reset_function.  Regardless, clear the bit, the spec
	 * requires it to be always read as zero.  NB, reset_function might
	 * not use an AF FLR, we don't have that level of granularity.
	 */
	/* [한국어] 사용자가 AF FLR 을 요청했다. */
	if (*ctrl & PCI_AF_CTRL_FLR) {
		/* [한국어] AF Capabilities 바이트를 담을 곳. */
		u8 cap;
		/* [한국어] config 읽기 결과 코드. */
		int ret;

		/* [한국어] 스펙상 항상 0 으로 읽혀야 하므로 먼저 지운다. PCIe FLR 쪽과 같은 규약이다. */
		*ctrl &= ~PCI_AF_CTRL_FLR;

		/* [한국어] AF Capabilities 를 읽어 지원 여부를 확인한다. */
		ret = pci_user_read_config_byte(vdev->pdev,
						pos - offset + PCI_AF_CAP,
						&cap);

		/* [한국어] FLR 지원과 Transactions Pending 지원을 모두 요구한다. TP 까지 필요한
		 * 이유는, 진행 중인 트랜잭션이 모두 끝났는지 확인할 수 없으면 안전하게
		 * 리셋할 수 없기 때문이다. 커널 리셋 API 도 그 비트를 폴링해 대기한다. */
		if (!ret && (cap & PCI_AF_CAP_FLR) && (cap & PCI_AF_CAP_TP)) {
			/* [한국어] 리셋 전 사용자 매핑 무효화 + memory_lock 쓰기 모드 획득. */
			vfio_pci_zap_and_down_write_memory_lock(vdev);
			/* [한국어] dma-buf revoke. */
			vfio_pci_dma_buf_move(vdev, true);
			/* [한국어] 커널 리셋 수행. 상류 주석대로 이 API 가 실제로 AF FLR 을 쓴다는 보장은
			 * 없다 — 방식을 고를 세밀함이 없다. */
			pci_try_reset_function(vdev->pdev);
			/* [한국어] 리셋 뒤 접근 가능 여부 재판정. */
			if (__vfio_pci_memory_enabled(vdev))
				/* [한국어] dma-buf 복구. */
				vfio_pci_dma_buf_move(vdev, false);
			/* [한국어] 락 해제. */
			up_write(&vdev->memory_lock);
		}
	}

	/* [한국어] 쓴 바이트 수. */
	return count;
}

/* [한국어]
 * init_pci_cap_af_perm - Advanced Features capability 의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(cap_perms 의 AF 슬롯).
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: AF capability 에서 사용자에게 열어 줄 것은 FLR 비트 하나뿐이고,
 * 그것도 하드웨어가 아니라 vconfig 로 받아 writefn 이 대신 수행한다.
 *
 * 동작 과정: 표를 만들고 전용 writefn 을 심은 뒤 두 자리를 뚫는다.
 *
 * 실행 컨텍스트: 모듈 적재 시점(__init).
 *
 * 에러 경로: -ENOMEM 만 올린다.
 *
 * 호출 체인:
 *   vfio_pci_init_perm_bits → [init_pci_cap_af_perm]
 *     → alloc_perm_bits, p_setb
 */
/* Permissions for Advanced Function capability */
static int __init init_pci_cap_af_perm(struct perm_bits *perm)
{
	/* [한국어] AF capability 는 고정 길이라 길이표에서 꺼내 쓴다. */
	if (alloc_perm_bits(perm, pci_cap_length[PCI_CAP_ID_AF]))
		return -ENOMEM;

	/* [한국어] 전용 쓰기 콜백. FLR 대행이 그 안에 있다. */
	perm->writefn = vfio_af_config_write;

	/* [한국어] [Next Pointer] chain 재작성용 공통 항목. */
	p_setb(perm, PCI_CAP_LIST_NEXT, (u8)ALL_VIRT, NO_WRITE);
	/* [한국어] [AF Control / capability 내 오프셋 0x04, 1바이트] FLR 비트만 가상화하고
	 * 같은 비트만 쓰기 허용한다.
	 * 왜 안전한가 — 사용자가 쓴 FLR 비트는 vconfig 에만 머물고 하드웨어에는
	 * 닿지 않는다. writefn 이 그 요청을 읽어 커널 리셋 API 로 대행하므로,
	 * 리셋 전후로 사용자 매핑 무효화와 dma-buf revoke, 상태 복구가 반드시
	 * 수행된다. 이 레지스터의 나머지 비트는 표에 없어 자동으로 쓰기 거부다. */
	p_setb(perm, PCI_AF_CTRL, PCI_AF_CTRL_FLR, PCI_AF_CTRL_FLR);
	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * init_pci_ext_cap_err_perm - AER(Advanced Error Reporting) 확장 capability 의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(ecap_perms 의 AER 슬롯).
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: AER 은 사용자 드라이버가 자기 디바이스의 오류를 보고 지우게
 * 해 줘야 실용적이다. 오류 상태 레지스터는 W1C(1 을 쓰면 지워짐) 방식이라
 * 쓰기를 허용해도 다른 값이 되지 않고, 마스크와 심각도 레지스터는 이 함수
 * 자신의 오류 보고 방식만 바꿀 뿐 호스트나 다른 디바이스에 영향을 주지
 * 않는다. 그래서 정의된 오류 비트에 한해 하드웨어 직통 쓰기를 허용한다.
 * 정의되지 않은 예약 비트를 마스크에서 빼는 이유는, 예약 비트에 쓰면
 * 하드웨어가 어떻게 반응할지 알 수 없기 때문이다.
 *
 * 동작 과정:
 *  1. 표를 만든다. 크기는 Root Error Command 직전까지 — 그 뒤의 루트 포트
 *     전용 레지스터는 아예 표 밖이라 자동으로 거부된다.
 *  2. 첫 dword 를 통째로 가상화한다. 상류 주석대로 확장 capability 의 첫
 *     dword 에는 next 포인터가 들어 있어 chain 재작성을 위해 반드시
 *     가상화해야 한다.
 *  3. Uncorrectable 오류 17종의 비트를 모아 마스크를 만들고 상태/마스크/
 *     심각도 세 레지스터에 쓰기를 허용한다.
 *  4. Correctable 오류 8종으로 같은 일을 상태/마스크 두 레지스터에 한다.
 *  5. ECRC 생성/검사 활성 비트 두 개만 AER Capabilities 레지스터에서 허용한다.
 *
 * 실행 컨텍스트: 모듈 적재 시점(__init).
 *
 * 에러 경로: -ENOMEM 만 올린다.
 *
 * 호출 체인:
 *   vfio_pci_init_perm_bits → [init_pci_ext_cap_err_perm]
 *     → alloc_perm_bits, p_setd
 */
/* Permissions for Advanced Error Reporting extended capability */
static int __init init_pci_ext_cap_err_perm(struct perm_bits *perm)
{
	/* [한국어] 쓰기를 허용할 오류 비트를 모아 담을 임시 마스크. 세 묶음(비정정
	 * 오류, 정정 가능 오류, ECRC)에 차례로 재사용된다. */
	u32 mask;

	/* [한국어] 길이표의 값이 Root Error Command 오프셋이라, 그 뒤의 루트 포트 전용
	 * 레지스터는 표 밖으로 밀려나 자동으로 쓰기 거부가 된다. */
	if (alloc_perm_bits(perm, pci_ext_cap_length[PCI_EXT_CAP_ID_ERR]))
		return -ENOMEM;

	/*
	 * Virtualize the first dword of all express capabilities
	 * because it includes the next pointer.  This lets us later
	 * remove capabilities from the chain if we need to.
	 */
	/* [한국어] [확장 capability 헤더 / capability 내 오프셋 0x00, 4바이트]
	 * 완전 에뮬레이션 + 쓰기 거부.
	 * 상류 주석대로 확장 capability 의 첫 dword 에는 next 포인터가 들어 있어
	 * chain 재작성을 위해 통째로 가상화해야 한다. 표준 capability 에서 next
	 * 1바이트만 가상화하던 것과 대비된다 — 확장 헤더는 ID, 버전, next 가 한
	 * dword 에 뭉쳐 있어 쪼갤 수 없다. */
	p_setd(perm, 0, ALL_VIRT, NO_WRITE);

	/* Writable bits mask */
	/* [한국어] 비정정(Uncorrectable) 오류 17종의 비트를 모은다. 이 마스크가
	 * 상태/마스크/심각도 세 레지스터에 그대로 쓰인다. 정의된 비트만 골라 넣는
	 * 이유는, 예약 비트에 쓰면 하드웨어가 어떻게 반응할지 모르기 때문이다.
	 * 아래 각 줄의 영어 주석이 오류의 종류를 밝힌다. */
	mask =	PCI_ERR_UNC_UND |		/* Undefined */
		PCI_ERR_UNC_DLP |		/* Data Link Protocol */
		PCI_ERR_UNC_SURPDN |		/* Surprise Down */
		PCI_ERR_UNC_POISON_TLP |	/* Poisoned TLP */
		PCI_ERR_UNC_FCP |		/* Flow Control Protocol */
		PCI_ERR_UNC_COMP_TIME |		/* Completion Timeout */
		PCI_ERR_UNC_COMP_ABORT |	/* Completer Abort */
		PCI_ERR_UNC_UNX_COMP |		/* Unexpected Completion */
		PCI_ERR_UNC_RX_OVER |		/* Receiver Overflow */
		PCI_ERR_UNC_MALF_TLP |		/* Malformed TLP */
		PCI_ERR_UNC_ECRC |		/* ECRC Error Status */
		PCI_ERR_UNC_UNSUP |		/* Unsupported Request */
		PCI_ERR_UNC_ACSV |		/* ACS Violation */
		PCI_ERR_UNC_INTN |		/* internal error */
		PCI_ERR_UNC_MCBTLP |		/* MC blocked TLP */
		PCI_ERR_UNC_ATOMEG |		/* Atomic egress blocked */
		PCI_ERR_UNC_TLPPRE;		/* TLP prefix blocked */
	/* [한국어] [Uncorrectable Error Status / 오프셋 0x04, 4바이트] 통과 + 정의된 오류
	 * 비트만 쓰기 허용.
	 * 왜 안전한가 — 이 레지스터는 W1C 방식이라 1 을 쓰면 해당 오류 기록이
	 * 지워질 뿐 다른 값이 되지 않는다. 사용자 드라이버가 자기 디바이스의 오류를
	 * 확인하고 지우는 것은 정상적인 드라이버 동작이며, 그 결과가 다른 디바이스나
	 * 호스트에 미치지 않는다. */
	p_setd(perm, PCI_ERR_UNCOR_STATUS, NO_VIRT, mask);
	/* [한국어] [Uncorrectable Error Mask / 오프셋 0x08, 4바이트] 통과 + 쓰기 허용.
	 * 어떤 오류를 보고할지 고르는 레지스터다. 사용자가 전부 마스크해 버리면
	 * 자기 오류를 못 보게 될 뿐이고, 반대로 전부 열어도 그 함수의 오류만
	 * 올라온다. */
	p_setd(perm, PCI_ERR_UNCOR_MASK, NO_VIRT, mask);
	/* [한국어] [Uncorrectable Error Severity / 오프셋 0x0C, 4바이트] 통과 + 쓰기 허용.
	 * 각 오류를 치명(fatal)으로 볼지 비치명(non-fatal)으로 볼지 정한다.
	 * 심각도를 올리면 링크 하향까지 갈 수 있지만 그 대상은 이 함수의 링크이고,
	 * 어차피 이 디바이스는 사용자 소유다. */
	p_setd(perm, PCI_ERR_UNCOR_SEVER, NO_VIRT, mask);

	/* [한국어] 정정 가능(Correctable) 오류 8종의 비트를 모은다. 위와 같은 이유로
	 * 정의된 비트만 고른다. */
	mask =	PCI_ERR_COR_RCVR |		/* Receiver Error Status */
		PCI_ERR_COR_BAD_TLP |		/* Bad TLP Status */
		PCI_ERR_COR_BAD_DLLP |		/* Bad DLLP Status */
		PCI_ERR_COR_REP_ROLL |		/* REPLAY_NUM Rollover */
		PCI_ERR_COR_REP_TIMER |		/* Replay Timer Timeout */
		PCI_ERR_COR_ADV_NFAT |		/* Advisory Non-Fatal */
		PCI_ERR_COR_INTERNAL |		/* Corrected Internal */
		PCI_ERR_COR_LOG_OVER;		/* Header Log Overflow */
	/* [한국어] [Correctable Error Status / 오프셋 0x10, 4바이트] 통과 + 쓰기 허용.
	 * 비정정 상태 레지스터와 같은 W1C 논리다. */
	p_setd(perm, PCI_ERR_COR_STATUS, NO_VIRT, mask);
	/* [한국어] [Correctable Error Mask / 오프셋 0x14, 4바이트] 통과 + 쓰기 허용.
	 * 정정 가능 오류의 보고 여부만 고른다. */
	p_setd(perm, PCI_ERR_COR_MASK, NO_VIRT, mask);

	/* [한국어] ECRC(End-to-End CRC) 생성과 검사 활성 비트 두 개만 모은다. */
	mask =	PCI_ERR_CAP_ECRC_GENE |		/* ECRC Generation Enable */
		PCI_ERR_CAP_ECRC_CHKE;		/* ECRC Check Enable */
	/* [한국어] [AER Capabilities and Control / 오프셋 0x18, 4바이트] 통과 + ECRC 두
	 * 비트만 쓰기 허용.
	 * 왜 그 둘만인가 — 이 레지스터의 나머지는 First Error Pointer 같은 읽기
	 * 전용 정보이거나 멀티 헤더 기록 관련 상태다. ECRC 는 TLP 끝에 CRC 를 붙여
	 * 경로 전체의 무결성을 검사하는 기능이라 켜고 끄는 것이 이 함수의 트랜잭션
	 * 품질 선택일 뿐이다. */
	p_setd(perm, PCI_ERR_CAP, NO_VIRT, mask);
	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * init_pci_ext_cap_pwr_perm - Power Budgeting 확장 capability 의 권한 표를 만든다
 *
 * @perm: 채울 권한 표(ecap_perms 의 Power Budgeting 슬롯).
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: Power Budgeting 은 "몇 번째 전력 항목을 보고 싶다" 를 데이터
 * 선택자 레지스터에 쓰고 데이터 레지스터에서 읽는 구조다. 선택자를 쓰는 것은
 * 어떤 항목을 보여 줄지 고르는 것일 뿐이라 안전하고, 정작 정보 자체는 여전히
 * 읽기 전용이다. 상류 주석이 그 판단을 그대로 적어 두었다.
 *
 * 동작 과정: 표를 만들고 첫 dword(next 포인터 포함)를 가상화한 뒤,
 * 데이터 선택자 바이트에만 쓰기를 허용한다.
 *
 * 실행 컨텍스트: 모듈 적재 시점(__init).
 *
 * 에러 경로: -ENOMEM 만 올린다.
 *
 * 호출 체인:
 *   vfio_pci_init_perm_bits → [init_pci_ext_cap_pwr_perm]
 *     → alloc_perm_bits, p_setd, p_setb
 */
/* Permissions for Power Budgeting extended capability */
static int __init init_pci_ext_cap_pwr_perm(struct perm_bits *perm)
{
	/* [한국어] Power Budgeting 은 고정 길이라 길이표에서 꺼내 쓴다. */
	if (alloc_perm_bits(perm, pci_ext_cap_length[PCI_EXT_CAP_ID_PWR]))
		return -ENOMEM;

	/* [한국어] [확장 capability 헤더 / 오프셋 0x00, 4바이트] AER 과 같은 이유로 첫
	 * dword 를 통째로 가상화하고 쓰기는 거부한다 — next 포인터가 이 안에 있다. */
	p_setd(perm, 0, ALL_VIRT, NO_WRITE);

	/* Writing the data selector is OK, the info is still read-only */
	/* [한국어] [Data Select / 오프셋 0x04, 1바이트] 통과 + 쓰기 허용.
	 * 상류 주석이 근거를 요약한다 — "데이터 선택자를 쓰는 것은 괜찮다, 정보
	 * 자체는 여전히 읽기 전용이다". 이 바이트는 "몇 번째 전력 항목을 보여
	 * 달라" 는 인덱스일 뿐이고, 그 결과가 나오는 Data 레지스터는 표에 없어
	 * 자동으로 쓰기 거부다. */
	p_setb(perm, PCI_PWR_DATA, NO_VIRT, (u8)ALL_WRITE);
	/* [한국어] 표 완성. */
	return 0;
}

/* [한국어]
 * vfio_pci_uninit_perm_bits - 모듈 전역 공유 권한 표를 모두 해제한다
 *
 * @return: 없다.
 *
 * 왜 필요한가: vfio_pci_init_perm_bits 가 kzalloc 으로 만든 비트맵들을
 * 되돌린다. 모듈 언로드 경로와 초기화 실패 롤백 양쪽에서 불리므로,
 * 할당되지 않은 슬롯에 대해서도 안전해야 한다 — free_perm_bits 가
 * kfree(NULL) 을 견디고 포인터를 NULL 로 비우므로 두 번 불려도 무해하다.
 *
 * 동작 과정: 명시적으로 초기화했던 8개 슬롯(표준 헤더, PM, VPD, PCI-X,
 * PCIe, AF, AER, Power Budgeting)만 해제한다. 나머지 슬롯은 readfn 포인터만
 * 가지고 비트맵이 없으므로 해제할 것이 없다. VNDR 과 DVSEC 슬롯도 writefn 만
 * 바꿔 놓은 것이라 마찬가지다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 모듈 언로드 시점 또는 초기화 실패 직후.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_core.c 의 모듈 exit, 그리고 vfio_pci_init_perm_bits 의 실패 롤백
 *     → [vfio_pci_uninit_perm_bits] → free_perm_bits
 */
/*
 * Initialize the shared permission tables
 */
void vfio_pci_uninit_perm_bits(void)
{
	/* [한국어] 표준 헤더 표 해제. 나머지와 한 줄 띄어 둔 것은 이것만 가짜 ID 를
	 * 쓰는 특별한 슬롯이기 때문이다. */
	free_perm_bits(&cap_perms[PCI_CAP_ID_BASIC]);

	/* [한국어] PM 표 해제. */
	free_perm_bits(&cap_perms[PCI_CAP_ID_PM]);
	/* [한국어] VPD 표 해제. */
	free_perm_bits(&cap_perms[PCI_CAP_ID_VPD]);
	/* [한국어] PCI-X 표 해제. */
	free_perm_bits(&cap_perms[PCI_CAP_ID_PCIX]);
	/* [한국어] PCIe 표 해제. */
	free_perm_bits(&cap_perms[PCI_CAP_ID_EXP]);
	/* [한국어] AF 표 해제. */
	free_perm_bits(&cap_perms[PCI_CAP_ID_AF]);

	/* [한국어] AER 표 해제. */
	free_perm_bits(&ecap_perms[PCI_EXT_CAP_ID_ERR]);
	/* [한국어] Power Budgeting 표 해제. 비트맵을 가진 슬롯은 이 여덟 개뿐이고,
	 * VNDR/DVSEC 슬롯은 writefn 만 바뀐 것이라 해제할 것이 없다. */
	free_perm_bits(&ecap_perms[PCI_EXT_CAP_ID_PWR]);
}

/* [한국어]
 * vfio_pci_init_perm_bits - 모듈 적재 시 모든 디바이스가 공유할 권한 표를 만든다
 *
 * @return: 0 이면 전부 성공. 실패하면 -ENOMEM(이미 전부 롤백된 상태).
 *          호출자인 vfio_pci_core.c 의 모듈 init 이 그대로 반환해 모듈 적재를
 *          실패시킨다.
 *
 * 왜 필요한가: 상류 주석이 vfio_config_init 위에서 밝히듯, 권한 표를 디바이스마다
 * 만들면 디바이스 하나당 cfg_size 바이트짜리 virt/write 버퍼가 두 벌씩 더
 * 필요하다. 표는 디바이스와 무관하게 같으므로(MSI 만 예외) 모듈 전역으로 한 벌만
 * 만들어 공유한다.
 *
 * 동작 과정:
 *  1. 표준 헤더 표를 만든다.
 *  2. PM, VPD, PCI-X, PCIe, AF 표를 차례로 만든다. 사이에 Vendor-Specific
 *     슬롯의 writefn 만 raw 쓰기로 바꾼다 — 벤더 정의 영역이라 커널이 의미를
 *     알 수 없어 통째로 통과시키는 것 외에 방법이 없다. readfn 은 기본값
 *     (vfio_direct_config_read)을 그대로 둬 next 포인터만 가상화된다.
 *  3. 확장 capability 쪽으로 AER 과 Power Budgeting 표를 만들고, 확장
 *     Vendor-Specific 과 DVSEC 슬롯의 writefn 도 raw 쓰기로 바꾼다.
 *  4. 하나라도 실패했으면 전부 해제한다.
 *
 * 반환값을 OR 로 누적하는 이유: 위 초기화 함수들이 0 또는 -ENOMEM 두 값만
 * 반환하기 때문에, OR 로 모아도 결과가 -ENOMEM 아니면 0 이다. 서로 다른 음수
 * 오류가 섞이면 OR 결과가 무의미해지겠지만 그런 경우가 없다.
 *
 * 실행 컨텍스트: 모듈 적재 시점. __init 이며 프로세스 문맥, 잠들 수 있다.
 *
 * 에러 경로: 4단계의 일괄 해제가 부분 성공 상태를 정리한다.
 *
 * 호출 체인:
 *   vfio_pci_core.c 의 모듈 init → [vfio_pci_init_perm_bits]
 *     → init_pci_cap_basic_perm / _pm_perm / _vpd_perm / _pcix_perm /
 *       _exp_perm / _af_perm / init_pci_ext_cap_err_perm / _pwr_perm,
 *       vfio_pci_uninit_perm_bits
 */
int __init vfio_pci_init_perm_bits(void)
{
	/* [한국어] 모든 초기화 결과를 OR 로 누적할 변수. */
	int ret;

	/* Basic config space */
	/* [한국어] 표준 config 헤더 표. 첫 대입이라 = 를 쓴다. */
	ret = init_pci_cap_basic_perm(&cap_perms[PCI_CAP_ID_BASIC]);

	/* Capabilities */
	/* [한국어] Power Management 표. 이하 모두 OR 누적인데, 이 함수들이 0 아니면
	 * -ENOMEM 두 값만 반환하기 때문에 OR 로 모아도 의미가 유지된다. */
	ret |= init_pci_cap_pm_perm(&cap_perms[PCI_CAP_ID_PM]);
	/* [한국어] VPD 표. */
	ret |= init_pci_cap_vpd_perm(&cap_perms[PCI_CAP_ID_VPD]);
	/* [한국어] PCI-X 표. */
	ret |= init_pci_cap_pcix_perm(&cap_perms[PCI_CAP_ID_PCIX]);
	/* [한국어] [Vendor-Specific capability] 표를 만들지 않고 writefn 만 raw 쓰기로
	 * 바꾼다. 그러면 비트맵 없이 읽기는 vfio_direct_config_read(기본값)로
	 * next 포인터만 가상화되고, 쓰기는 검사 없이 하드웨어로 간다.
	 * 왜 그렇게 하는가 — 벤더 정의 영역이라 커널이 어느 비트가 무엇인지 알 수
	 * 없다. 통째로 막으면 정상 드라이버가 동작하지 못하고, 표를 만들려 해도
	 * 기준이 없다. 3분류의 예외인 raw 통과가 적용되는 두 자리 중 하나다. */
	cap_perms[PCI_CAP_ID_VNDR].writefn = vfio_raw_config_write;
	/* [한국어] PCIe 표. */
	ret |= init_pci_cap_exp_perm(&cap_perms[PCI_CAP_ID_EXP]);
	/* [한국어] Advanced Features 표. 여기까지가 표준 capability. */
	ret |= init_pci_cap_af_perm(&cap_perms[PCI_CAP_ID_AF]);

	/* Extended capabilities */
	/* [한국어] AER 표. 여기서부터 확장 capability. */
	ret |= init_pci_ext_cap_err_perm(&ecap_perms[PCI_EXT_CAP_ID_ERR]);
	/* [한국어] Power Budgeting 표. */
	ret |= init_pci_ext_cap_pwr_perm(&ecap_perms[PCI_EXT_CAP_ID_PWR]);
	/* [한국어] [확장 Vendor-Specific] 표준판과 같은 이유로 raw 쓰기. */
	ecap_perms[PCI_EXT_CAP_ID_VNDR].writefn = vfio_raw_config_write;
	/* [한국어] [DVSEC] Designated Vendor-Specific 도 벤더 정의라 raw 쓰기.
	 * CXL 같은 최신 규격이 DVSEC 위에 얹혀 있어 실제로 쓰이는 경로다. */
	ecap_perms[PCI_EXT_CAP_ID_DVSEC].writefn = vfio_raw_config_write;

	/* [한국어] 하나라도 실패했으면 부분 성공 상태를 남기지 않는다. */
	if (ret)
		/* [한국어] 이미 만든 표를 전부 해제한다. free_perm_bits 가 NULL 안전하므로
		 * 만들어지지 않은 슬롯에 대해서도 문제없다. */
		vfio_pci_uninit_perm_bits();

	/* [한국어] 0 이면 모듈 적재 계속, -ENOMEM 이면 모듈 적재 실패. */
	return ret;
}

/* [한국어]
 * vfio_find_cap_start - 어떤 오프셋이 속한 capability 의 시작 오프셋을 지도에서 되짚는다
 *
 * @vdev: 대상 디바이스. pci_config_map 을 읽는다.
 * @pos: 알아보려는 절대 오프셋.
 * @return: 그 capability 가 시작하는 절대 오프셋. 표준 헤더 안이면 0.
 *
 * 왜 필요한가: perm_bits 의 virt/write 비트맵은 capability 단위로 만들어져
 * "이 capability 의 몇 번째 바이트" 로 색인된다. 그런데 사용자가 주는 것은
 * config space 안의 절대 오프셋이다. 둘을 잇기 위해 capability 의 시작점을
 * 알아야 하고, 그것을 지도에서 왼쪽으로 훑어 찾는 것이 이 함수다.
 *
 * 동작 과정:
 *  1. 확장 config 영역(256바이트 이상)인지에 따라 훑기를 멈출 하한을 정한다.
 *     표준 영역이면 표준 헤더 끝(64), 확장 영역이면 표준 영역 끝(256)이다.
 *     이 하한이 없으면 표준 capability 를 찾다가 헤더 영역으로 넘어가거나,
 *     확장 capability 를 찾다가 표준 영역으로 넘어갈 수 있다.
 *  2. 지도에서 이 위치의 capability ID 를 꺼낸다.
 *  3. 표준 헤더 소속이면 시작점은 언제나 0 이다.
 *  4. 아니면 왼쪽으로 한 바이트씩 가며 같은 ID 가 이어지는 동안 후퇴한다.
 *     ID 가 달라지거나 하한에 닿으면 그 자리가 시작점이다.
 *
 * 상류 주석의 "XXX Can we have to abutting capabilities of the same type?"
 * (오타로 to/two 가 바뀐 채 남아 있다)은 이 방식의 한계를 지적한다 — 같은
 * 종류의 capability 두 개가 맞붙어 있으면 둘을 하나로 오인한다. 실제로는
 * 같은 ID 의 capability 가 인접하는 경우가 없어 문제되지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 배열 훑기뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다. 지도가 올바르게 칠해져 있다는 전제로만 동작한다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single / vfio_msi_config_read / vfio_msi_config_write
 *     → [vfio_find_cap_start]
 */
static int vfio_find_cap_start(struct vfio_pci_core_device *vdev, int pos)
{
	/* [한국어] 이 위치에 칠해진 capability ID. */
	u8 cap;
	/* [한국어] 왼쪽으로 훑을 때 멈출 하한. 확장 영역이면 확장 영역의 시작(256),
	 * 표준 영역이면 표준 헤더의 끝(64)이다. 이 하한이 없으면 표준 capability 를
	 * 되짚다가 헤더 영역으로 넘어가거나, 확장 capability 를 되짚다가 표준
	 * 영역으로 넘어간다. */
	int base = (pos >= PCI_CFG_SPACE_SIZE) ? PCI_CFG_SPACE_SIZE :
						 PCI_STD_HEADER_SIZEOF;
	/* [한국어] 지도에서 이 바이트의 소속 capability 를 꺼낸다. */
	cap = vdev->pci_config_map[pos];

	/* [한국어] 표준 config 헤더에 붙인 가짜 ID 다. 헤더의 시작은 언제나 0 이므로
	 * 훑을 필요가 없다. */
	if (cap == PCI_CAP_ID_BASIC)
		return 0;

	/* XXX Can we have to abutting capabilities of the same type? */
	/* [한국어] 왼쪽으로 한 바이트씩 가며 같은 ID 가 이어지는 동안 후퇴한다.
	 * 바로 위의 상류 주석은 이 방식의 한계를 지적한다 — 같은 종류의 capability
	 * 두 개가 맞붙어 있으면 둘을 하나로 오인한다. 실제 하드웨어에서 그런 배치가
	 * 없어 문제되지 않는다. */
	while (pos - 1 >= base && vdev->pci_config_map[pos - 1] == cap)
		pos--;

	/* [한국어] ID 가 달라지거나 하한에 닿은 자리가 capability 시작점이다. */
	return pos;
}

/* [한국어]
 * vfio_msi_config_read - MSI capability 읽기 콜백. 사용자에게 보일 최대 벡터 수를 갱신한 뒤 읽는다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 읽을 바이트 수.
 * @perm: 이 디바이스 전용 MSI 권한 표(vdev->msi_perm).
 * @offset: MSI capability 시작점 기준 오프셋.
 * @val: 결과를 담을 곳.
 * @return: 읽은 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: MSI Message Control 레지스터의 Multiple Message Capable 필드는
 * "이 디바이스가 최대 몇 개의 MSI 벡터를 쓸 수 있는가" 를 지수로 알린다.
 * VFIO 에서는 그 값이 하드웨어 능력이 아니라 커널이 실제로 배정해 준 벡터
 * 수(vdev->msi_qmax)여야 한다. 그래서 사용자가 그 레지스터를 읽기 직전에
 * vconfig 의 해당 필드를 msi_qmax 로 갱신하려 한다.
 *
 * 동작 과정:
 *  1. 이번 읽기 범위가 Message Control 레지스터를 포함하는지 판정한다.
 *  2. vfio_find_cap_start 로 이 MSI capability 의 시작 절대 오프셋을 얻는다.
 *  3. vconfig 의 해당 위치를 __le16 로 보고 Multiple Message Capable 필드를
 *     지운 뒤 msi_qmax 를 1비트 왼쪽으로 밀어 넣는다. 그 필드가 비트 1~3 에
 *     있기 때문에 1비트 시프트다.
 *  4. 기본 엔진으로 실제 읽기를 수행한다. 표에서 Message Control 의 하위
 *     바이트가 전면 가상화이므로 3단계에서 넣은 값이 그대로 사용자에게 간다.
 *
 * [상류 코드 관찰] 3단계의 포인터가 vdev->vconfig[start] 다. 짝이 되는
 * vfio_msi_config_write 는 같은 레지스터를 가리키는 데
 * vdev->vconfig[start + PCI_MSI_FLAGS] 를 쓴다. 두 식이 같은 곳을 가리키려면
 * PCI_MSI_FLAGS 가 0 이어야 하는데, 그 상수는 Message Control 레지스터의
 * capability 내 오프셋이므로 0 이 아니다(이 트리에는 그 정의가 있는
 * pci_regs.h 가 없어 값 자체는 확인 못 함). 즉 읽기 경로의 갱신은 Message
 * Control 이 아니라 capability 헤더 바이트(ID 와 next 포인터)에 떨어진다.
 * 다행히 capability ID 바이트는 표에서 가상화 대상이 아니라 읽을 때
 * 하드웨어 값이 쓰이고, next 포인터가 있는 상위 바이트는 이 연산이 건드리는
 * 하위 8비트 밖이라, 결과적으로 사용자에게 보이는 값이 깨지지는 않는다.
 * 대신 의도했던 msi_qmax 반영이 일어나지 않는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 4단계에서 잠들 수 있다.
 *
 * 에러 경로: 기본 엔진의 오류를 그대로 올린다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->readfn(= 이 함수)
 *     → [vfio_msi_config_read] → vfio_find_cap_start, vfio_default_config_read
 */
static int vfio_msi_config_read(struct vfio_pci_core_device *vdev, int pos,
				int count, struct perm_bits *perm,
				int offset, __le32 *val)
{
	/* Update max available queue size from msi_qmax */
	/* [한국어] 이번 읽기 범위가 Message Control 레지스터에 걸치는지 판정한다. */
	if (offset <= PCI_MSI_FLAGS && offset + count >= PCI_MSI_FLAGS) {
		/* [한국어] 갱신할 위치를 가리킬 포인터. */
		__le16 *flags;
		/* [한국어] 이 MSI capability 의 시작 절대 오프셋. */
		int start;

		/* [한국어] 지도를 되짚어 capability 시작점을 얻는다. MSI capability 는 디바이스마다
		 * 다른 오프셋에 있어 매번 찾아야 한다. */
		start = vfio_find_cap_start(vdev, pos);

		/* [한국어] [상류 코드 관찰] 짝이 되는 vfio_msi_config_write 는 같은 레지스터를
		 * 가리키는 데 vconfig[start + PCI_MSI_FLAGS] 를 쓴다. 두 식이 같은 곳을
		 * 가리키려면 PCI_MSI_FLAGS 가 0 이어야 하는데, 그 상수는 Message Control
		 * 레지스터의 capability 내 오프셋이라 0 이 아니다(정의가 있는 pci_regs.h 는
		 * 이 트리에 없어 값 자체는 확인 못 함). 즉 아래 두 줄의 갱신은 Message
		 * Control 이 아니라 capability 헤더 바이트에 떨어진다. 헤더의 ID 바이트는
		 * 표에서 가상화 대상이 아니어서 읽을 때 하드웨어 값이 쓰이고, next 포인터가
		 * 있는 상위 바이트는 이 연산이 건드리는 하위 8비트 밖이라 사용자에게 보이는
		 * 값이 깨지지는 않는다. 다만 의도한 msi_qmax 반영이 일어나지 않는다.
		 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		flags = (__le16 *)&vdev->vconfig[start];

		/* [한국어] Multiple Message Capable 필드를 비운다. 이 필드는 "이 디바이스가 최대
		 * 몇 개의 MSI 벡터를 쓸 수 있는가" 를 2의 지수로 알린다. */
		*flags &= cpu_to_le16(~PCI_MSI_FLAGS_QMASK);
		/* [한국어] 커널이 실제로 배정한 벡터 수의 지수를 넣는다. 필드가 비트 1~3 에 있어
		 * 1비트 왼쪽으로 민다. 하드웨어 능력이 아니라 커널이 준 값을 보여 주려는
		 * 것이 이 갱신의 목적이다. */
		*flags |= cpu_to_le16(vdev->msi_qmax << 1);
	}

	/* [한국어] 기본 엔진으로 실제 읽기를 수행한다. 표에서 Message Control 하위 바이트가
	 * 전면 가상화라 vconfig 값이 그대로 사용자에게 간다. */
	return vfio_default_config_read(vdev, pos, count, perm, offset, val);
}

/* [한국어]
 * vfio_msi_config_write - MSI capability 쓰기 콜백. 활성 비트와 벡터 수를 검증해 하드웨어에 반영한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 절대 오프셋.
 * @count: 쓸 바이트 수.
 * @perm: 이 디바이스 전용 MSI 권한 표.
 * @offset: MSI capability 시작점 기준 오프셋.
 * @val: 사용자가 쓴 값.
 * @return: 쓴 바이트 수, 또는 하부 오류.
 *
 * 왜 필요한가: MSI 는 config 쓰기만으로 켜지면 안 된다. 실제 벡터 할당과
 * IRQ 등록은 ioctl 경로(vfio_pci_intrs.c 의 vfio_msi_enable)가 하고, 커널의
 * irq 도메인이 메시지 주소/데이터를 직접 채운다. 그래서 사용자가 Message
 * Control 에 쓴 값 중 Enable 비트는 ioctl 로 실제 활성화된 경우에만 살려
 * 두고, 요청한 벡터 수도 커널이 배정한 상한을 넘지 못하게 깎는다. 그렇게
 * 검증한 값만 하드웨어에 반영한다.
 *
 * 동작 과정:
 *  1. 기본 엔진으로 먼저 쓴다. 표에서 Message Control 하위 바이트와 주소/
 *     데이터 필드가 전면 가상화이므로 사용자 값은 vconfig 에만 들어간다.
 *  2. 이번 쓰기가 Message Control 을 포함하지 않으면 할 일이 없다.
 *  3. capability 시작점을 찾아 vconfig 의 Message Control 값을 꺼낸다.
 *  4. 현재 이 디바이스의 IRQ 모드가 MSI 가 아니면 Enable 비트를 강제로
 *     지운다. 상류 주석의 "MSI is enabled via ioctl" 이 그 뜻이다.
 *  5. 요청한 벡터 수(Multiple Message Enable 필드, 비트 4~6)가 커널이 배정한
 *     msi_qmax 를 넘으면 msi_qmax 로 깎는다.
 *  6. 검증된 값을 vconfig 에 되쓰고, 같은 값을 하드웨어에도 실제로 쓴다.
 *     주소/데이터와 달리 Message Control 만은 하드웨어에 반영해야 하는데,
 *     Enable 비트가 실제로 MSI 전송을 켜는 비트이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 6단계에서 잠들 수 있다.
 *
 * 에러 경로: 1단계 실패면 즉시 반환. 6단계의 하드웨어 쓰기가 실패하면
 * vconfig 는 이미 갱신된 채로 오류가 나간다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → perm->writefn(= 이 함수)
 *     → [vfio_msi_config_write]
 *     → vfio_default_config_write, vfio_find_cap_start,
 *       pci_user_write_config_word
 */
static int vfio_msi_config_write(struct vfio_pci_core_device *vdev, int pos,
				 int count, struct perm_bits *perm,
				 int offset, __le32 val)
{
	/* [한국어] 기본 엔진으로 먼저 쓴다. 표에서 Message Control 하위 바이트와 주소/
	 * 데이터 필드가 전면 가상화라 사용자 값은 vconfig 에만 들어간다. */
	count = vfio_default_config_write(vdev, pos, count, perm, offset, val);
	if (count < 0)
		return count;

	/* Fixup and write configured queue size and enable to hardware */
	/* [한국어] 이번 쓰기가 Message Control 에 걸칠 때만 검증과 하드웨어 반영을 한다. */
	if (offset <= PCI_MSI_FLAGS && offset + count >= PCI_MSI_FLAGS) {
		/* [한국어] 그림자 복사본의 Message Control 위치. */
		__le16 *pflags;
		/* [한국어] 검증할 CPU 표현 값. */
		u16 flags;
		/* [한국어] capability 시작 오프셋과 하드웨어 쓰기 결과 코드. */
		int start, ret;

		/* [한국어] capability 시작점을 되짚는다. */
		start = vfio_find_cap_start(vdev, pos);

		/* [한국어] Message Control 레지스터의 정확한 위치. 읽기 경로와 달리 여기서는
		 * capability 내 오프셋을 제대로 더한다. */
		pflags = (__le16 *)&vdev->vconfig[start + PCI_MSI_FLAGS];

		/* [한국어] 사용자가 방금 쓴 값을 CPU 표현으로 꺼낸다. */
		flags = le16_to_cpu(*pflags);

		/* MSI is enabled via ioctl */
		/* [한국어] 이 디바이스의 현재 IRQ 모드가 MSI 가 아니다. 상류 주석의 "MSI is
		 * enabled via ioctl" 이 이 검사의 이유다 — MSI 활성화는 config 쓰기가 아니라
		 * vfio_pci_intrs.c 의 ioctl 경로가 벡터를 할당하고 IRQ 를 등록해야 성립한다. */
		if  (vdev->irq_type != VFIO_PCI_MSI_IRQ_INDEX)
			/* [한국어] Enable 비트를 강제로 지운다. 그러지 않으면 커널이 벡터를 준비하지 않은
			 * 채로 하드웨어가 MSI 를 쏘게 되고, 그 메시지 주소/데이터는 사용자가 쓴
			 * 가짜 값이라 어디로 갈지 알 수 없다. **MSI 경로 보안의 핵심 검사다.** */
			flags &= ~PCI_MSI_FLAGS_ENABLE;

		/* Check queue size */
		/* [한국어] 사용자가 요청한 벡터 수(Multiple Message Enable 필드, 비트 4~6)가 커널이
		 * 배정한 상한을 넘는지 본다. */
		if ((flags & PCI_MSI_FLAGS_QSIZE) >> 4 > vdev->msi_qmax) {
			/* [한국어] 필드를 비우고 */
			flags &= ~PCI_MSI_FLAGS_QSIZE;
			/* [한국어] 커널이 배정한 상한으로 깎아 넣는다. 하드웨어가 실제로 할당된 것보다
			 * 많은 벡터를 쏘면 등록되지 않은 IRQ 가 발생한다. */
			flags |= vdev->msi_qmax << 4;
		}

		/* Write back to virt and to hardware */
		/* [한국어] 검증된 값을 그림자 복사본에 되쓴다. 사용자가 되읽으면 자기가 쓴 값이
		 * 아니라 검증된 값을 본다. */
		*pflags = cpu_to_le16(flags);
		/* [한국어] 검증된 값을 하드웨어에도 실제로 쓴다. 주소/데이터와 달리 Message
		 * Control 만은 반영해야 하는데, Enable 비트가 실제로 MSI 전송을 켜는 비트라
		 * 커널이 벡터를 준비한 뒤에는 하드웨어에 반영돼야 인터럽트가 나오기
		 * 때문이다. */
		ret = pci_user_write_config_word(vdev->pdev,
						 start + PCI_MSI_FLAGS,
						 flags);
		/* [한국어] 하드웨어 쓰기가 실패했다. 이 시점에서 그림자 복사본은 이미 갱신된
		 * 뒤라 사용자 눈에는 "값은 바뀌었는데 오류" 로 보인다. 되돌리지 않는 이유는
		 * 어느 쪽 값이 옳은지 알 수 없기 때문이다. */
		if (ret)
			return ret;
	}

	/* [한국어] 쓴 바이트 수. */
	return count;
}

/* [한국어]
 * init_pci_cap_msi_perm - 이 디바이스의 MSI capability 모양에 맞춘 권한 표를 만든다
 *
 * @perm: 채울 권한 표(vdev->msi_perm 이 가리키는 것).
 * @len: 이 디바이스의 MSI capability 실제 길이(10/14/20/24바이트).
 * @flags: 하드웨어에서 읽은 Message Control 값. 64비트 주소 지원과 per-vector
 *         masking 지원 여부를 이 값에서 판별한다.
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 왜 필요한가: 상류 주석이 밝히듯 MSI 는 "디바이스마다 다르게 판정" 해야 하는
 * 유일한 capability 다. 64비트 주소를 지원하면 데이터 레지스터가 4바이트 뒤로
 * 밀리고, per-vector masking 을 지원하면 마스크/대기 레지스터가 더 붙는다.
 * 같은 표를 공유할 수 없으므로 디바이스마다 만든다. 그래서 이 함수만은
 * __init 이 아니다 — 상류 주석이 "Don't add __init" 이라고 못 박아 두었다.
 *
 * 어떤 비트를 왜 그렇게 두는가:
 *  - Message Control 하위 바이트: 전면 가상화 + 전면 쓰기 허용. 사용자가
 *    Enable 과 벡터 수를 자유롭게 쓰되 실제 반영은 writefn 이 검증한 뒤에만
 *    한다. 상위 바이트는 스펙상 예약이라 손대지 않는다(자동으로 거부).
 *  - 메시지 주소(하위/상위)와 데이터: 전면 가상화 + 전면 쓰기 허용.
 *    **이것이 이 파일에서 가장 중요한 보안 결정 중 하나다.** MSI 는 결국
 *    "이 주소에 이 데이터를 DMA 로 쓰라" 는 지시이고, 그 주소는 CPU 의
 *    인터럽트 리매핑 창을 가리킨다. 사용자가 하드웨어에 직접 쓰게 두면
 *    임의의 호스트 인터럽트를 위조할 수 있다. 그래서 사용자 값은 vconfig 에
 *    가둬 두고, 진짜 하드웨어 값은 커널 irq 도메인이 채운다
 *    (drivers/pci/msi/msi.c:762 의 __pci_write_msi_msg 계열).
 *  - 마스크/대기 비트 레지스터(per-vector masking 지원 시): 가상화 없이
 *    쓰기 허용. 벡터별 마스킹은 그 디바이스 자신의 인터럽트를 잠재우는
 *    일이라 호스트에 위험하지 않다. 오히려 하드웨어에 직접 닿아야 실효가 있다.
 *
 * 동작 과정: len 크기로 표를 만들고, 전용 readfn/writefn 을 심은 뒤 next
 * 포인터와 위 필드들을 flags 에 따라 두 갈래(64비트/32비트)로 설정한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스가 처음 열려 vfio_config_init 이 돌
 * 때 딱 한 번 불린다. 잠들 수 있다.
 *
 * 에러 경로: -ENOMEM 만 올린다. 호출자 vfio_msi_cap_len 이 구조체를 해제한다.
 *
 * 호출 체인:
 *   vfio_config_init → vfio_cap_init → vfio_cap_len → vfio_msi_cap_len
 *     → [init_pci_cap_msi_perm] → alloc_perm_bits, p_setb, p_setw, p_setd
 */
/*
 * MSI determination is per-device, so this routine gets used beyond
 * initialization time. Don't add __init
 */
static int init_pci_cap_msi_perm(struct perm_bits *perm, int len, u16 flags)
{
	/* [한국어] 디바이스마다 다른 실제 길이로 표를 만든다. 이 함수만 __init 이 아니며,
	 * 상류 주석이 그 이유를 "MSI 판정은 디바이스별이라 초기화 시점 이후에도
	 * 쓰인다" 로 못 박아 두었다. */
	if (alloc_perm_bits(perm, len))
		return -ENOMEM;

	/* [한국어] 전용 읽기 콜백. 사용자에게 보일 최대 벡터 수를 갱신한다. */
	perm->readfn = vfio_msi_config_read;
	/* [한국어] 전용 쓰기 콜백. Enable 비트와 벡터 수를 검증한다. */
	perm->writefn = vfio_msi_config_write;

	/* [한국어] [Next Pointer] chain 재작성용 공통 항목. */
	p_setb(perm, PCI_CAP_LIST_NEXT, (u8)ALL_VIRT, NO_WRITE);

	/*
	 * The upper byte of the control register is reserved,
	 * just setup the lower byte.
	 */
	/* [한국어] [Message Control 하위 바이트 / capability 내 오프셋 0x02, 1바이트]
	 * 완전 에뮬레이션 + 쓰기 허용.
	 * 상류 주석대로 이 레지스터의 상위 바이트는 예약이라 표에 넣지 않는다
	 * (자동으로 쓰기 거부). 하위 바이트에는 Enable, Multiple Message Capable,
	 * Multiple Message Enable, 64비트 주소 지원, per-vector masking 지원이 모여
	 * 있다.
	 * 왜 안전한가 — 사용자가 무엇을 쓰든 vconfig 에만 들어가고, writefn 이
	 * IRQ 모드와 벡터 수 상한을 검증한 뒤에야 하드웨어에 반영된다. */
	p_setb(perm, PCI_MSI_FLAGS, (u8)ALL_VIRT, (u8)ALL_WRITE);
	/* [한국어] [Message Address 하위 32비트 / capability 내 오프셋 0x04, 4바이트]
	 * 완전 에뮬레이션 + 쓰기 허용.
	 * **이 파일에서 가장 중요한 보안 결정 중 하나다.** MSI 는 결국 "이 주소에
	 * 이 데이터를 써라" 는 DMA 이고, 그 주소는 CPU 의 인터럽트 리매핑 창을
	 * 가리킨다. 사용자가 이 값을 하드웨어에 직접 쓸 수 있다면 임의의 호스트
	 * 인터럽트 벡터를 위조할 수 있다 — IOMMU 가 DMA 를 걸러 주더라도 인터럽트
	 * 리매핑이 없는 플랫폼에서는 그대로 통과한다.
	 * 그래서 쓰기는 전면 허용하되(게스트 드라이버가 정상적으로 값을 쓰고
	 * 되읽어야 하므로) 그 값은 vconfig 에 갇히고, 진짜 하드웨어 값은 커널의
	 * irq 도메인이 채운다(drivers/pci/msi/msi.c:812 의 pci_write_msi_msg 와
	 * 그것이 부르는 __pci_write_msi_msg). */
	p_setd(perm, PCI_MSI_ADDRESS_LO, ALL_VIRT, ALL_WRITE);
	/* [한국어] 64비트 주소를 지원하는 디바이스는 주소 상위 32비트가 더 있고, 그만큼
	 * 데이터와 마스크 레지스터의 위치가 밀린다. */
	if (flags & PCI_MSI_FLAGS_64BIT) {
		/* [한국어] [Message Address 상위 32비트 / 오프셋 0x08, 4바이트] 하위 절반과 같은
		 * 이유로 완전 에뮬레이션. */
		p_setd(perm, PCI_MSI_ADDRESS_HI, ALL_VIRT, ALL_WRITE);
		/* [한국어] [Message Data (64비트 배치) / 오프셋 0x0C, 2바이트] 완전 에뮬레이션 +
		 * 쓰기 허용. 주소와 짝을 이루는 값으로, 어느 인터럽트 벡터를 발생시킬지
		 * 결정한다. 주소와 같은 이유로 하드웨어에 닿아서는 안 된다. */
		p_setw(perm, PCI_MSI_DATA_64, (u16)ALL_VIRT, (u16)ALL_WRITE);
		/* [한국어] per-vector masking 을 지원하면 마스크/대기 비트 레지스터가 더 붙는다. */
		if (flags & PCI_MSI_FLAGS_MASKBIT) {
			/* [한국어] [Mask Bits (64비트 배치) / 오프셋 0x10, 4바이트] 통과 + 쓰기 허용.
			 * 왜 여기만 가상화하지 않는가 — 벡터별 마스킹은 그 디바이스 자신의
			 * 인터럽트를 잠재우는 일이라 호스트에 위험하지 않다. 오히려 하드웨어에
			 * 직접 닿아야 실효가 있다. 주소/데이터는 "어디로 쏘는가" 라 위험하고,
			 * 마스크는 "쏠지 말지" 라 안전하다는 대비가 이 표의 설계다. */
			p_setd(perm, PCI_MSI_MASK_64, NO_VIRT, ALL_WRITE);
			/* [한국어] [Pending Bits (64비트 배치) / 오프셋 0x14, 4바이트] 통과 + 쓰기 허용.
			 * 스펙상 읽기 전용 상태 레지스터라 쓰기가 무시되지만, 표에서 굳이 막지
			 * 않아도 하드웨어가 알아서 무시한다. */
			p_setd(perm, PCI_MSI_PENDING_64, NO_VIRT, ALL_WRITE);
		}
	} else {
		/* [한국어] [Message Data (32비트 배치) / 오프셋 0x08, 2바이트] 32비트 주소만
		 * 지원하는 디바이스에서는 데이터가 4바이트 앞으로 당겨진다. 가상화 근거는
		 * 64비트 판과 같다. */
		p_setw(perm, PCI_MSI_DATA_32, (u16)ALL_VIRT, (u16)ALL_WRITE);
		/* [한국어] 32비트 배치에서도 per-vector masking 지원 여부에 따라 두 레지스터가 붙는다. */
		if (flags & PCI_MSI_FLAGS_MASKBIT) {
			/* [한국어] [Mask Bits (32비트 배치) / 오프셋 0x0C, 4바이트] 통과 + 쓰기 허용. */
			p_setd(perm, PCI_MSI_MASK_32, NO_VIRT, ALL_WRITE);
			/* [한국어] [Pending Bits (32비트 배치) / 오프셋 0x10, 4바이트] 통과 + 쓰기 허용. */
			p_setd(perm, PCI_MSI_PENDING_32, NO_VIRT, ALL_WRITE);
		}
	}
	/* [한국어] 디바이스별 MSI 표 완성. */
	return 0;
}

/* [한국어]
 * vfio_msi_cap_len - MSI capability 의 실제 길이를 재고, 이 디바이스의 첫 호출이면 MSI 권한 표까지 만든다
 *
 * @vdev: 대상 디바이스.
 * @pos: MSI capability 의 시작 절대 오프셋.
 * @return: 성공하면 capability 길이(10/14/20/24). 음수면 오류
 *          (config 읽기 실패를 변환한 값이거나 -ENOMEM).
 *
 * 왜 필요한가: MSI capability 의 길이는 고정이 아니라 Message Control 의 두
 * 비트에 달려 있다. 지도(pci_config_map)를 정확한 길이만큼 칠하려면 먼저
 * 그 길이를 알아야 한다. 그리고 같은 정보(64비트 여부, 마스킹 지원 여부)가
 * 권한 표 모양도 결정하므로, 길이를 재는 김에 표까지 만든다.
 *
 * 동작 과정:
 *  1. 하드웨어에서 Message Control 을 읽는다.
 *  2. 최소 길이 10 에서 시작해, 64비트 주소를 지원하면 4바이트를,
 *     per-vector masking 을 지원하면 마스크/대기 레지스터 몫으로 10바이트를
 *     더한다. 그래서 가능한 길이는 10, 14, 20, 24 네 가지다 — pci_cap_length
 *     표에 달린 상류 주석의 "10, 14, 20, or 24" 가 이 값들이다.
 *  3. 이 디바이스에 이미 표가 있으면(같은 디바이스를 다시 열었거나, 지도
 *     칠하기가 두 번 불린 경우) 길이만 돌려준다.
 *  4. 없으면 구조체를 할당하고 init_pci_cap_msi_perm 으로 채운다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로. 잠들 수 있다.
 *
 * 에러 경로: 1단계 실패는 PCIBIOS 코드를 errno 로 바꿔 반환한다. 4단계에서
 * 표 초기화가 실패하면 구조체를 해제하고 오류를 올리는데, 이때
 * vdev->msi_perm 은 이미 대입된 뒤라 해제된 포인터가 남는다. 다만 그 뒤
 * 호출 사슬이 곧바로 vfio_config_init 의 실패 경로로 빠지고, 그 경로에서는
 * msi_perm 을 다시 보지 않는다.
 *
 * 호출 체인:
 *   vfio_cap_init → vfio_cap_len → [vfio_msi_cap_len]
 *     → pci_read_config_word, init_pci_cap_msi_perm
 */
/* Determine MSI CAP field length; initialize msi_perms on 1st call per vdev */
static int vfio_msi_cap_len(struct vfio_pci_core_device *vdev, u8 pos)
{
	/* [한국어] Message Control 을 읽을 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 계산한 길이와 config 읽기 결과 코드. */
	int len, ret;
	/* [한국어] 하드웨어의 Message Control 값. 길이와 표 모양을 함께 결정한다. */
	u16 flags;

	/* [한국어] capability 시작점에서 Message Control 오프셋만큼 떨어진 곳을 읽는다.
	 * 여기서는 사용자를 대신하는 접근이 아니라 커널 자신의 초기화이므로
	 * pci_user_ 판이 아닌 일반 판을 쓴다. */
	ret = pci_read_config_word(pdev, pos + PCI_MSI_FLAGS, &flags);
	if (ret)
		return pcibios_err_to_errno(ret);

	/* [한국어] 최소 구성은 헤더 2바이트 + Message Control 2바이트 + 32비트 주소 4바이트
	 * + 데이터 2바이트 = 10바이트다. */
	len = 10; /* Minimum size */
	/* [한국어] 64비트 주소를 지원하면 주소 상위 32비트가 더 붙는다. */
	if (flags & PCI_MSI_FLAGS_64BIT)
		/* [한국어] 4바이트 증가. */
		len += 4;
	/* [한국어] per-vector masking 을 지원하면 마스크와 대기 비트 레지스터가 붙는다. */
	if (flags & PCI_MSI_FLAGS_MASKBIT)
		/* [한국어] 마스크 4 + 대기 4 에 정렬 여유 2 를 더한 10바이트 증가. 이 두 조합으로
		 * 길이표 주석의 "10, 14, 20, or 24" 네 값이 나온다. */
		len += 10;

	/* [한국어] 이 디바이스의 MSI 표가 이미 있으면 길이만 돌려준다. 같은 디바이스를
	 * 다시 열거나 지도 칠하기가 두 번 불리는 경우를 위한 방어다. */
	if (vdev->msi_perm)
		return len;

	/* [한국어] 표 구조체를 할당한다. 내용은 곧바로 alloc_perm_bits 가 채우므로 0
	 * 초기화가 필요 없다. ACCOUNT 는 이 메모리를 요청 프로세스의 memcg 에
	 * 달아 컨테이너 격리를 지키게 한다. */
	vdev->msi_perm = kmalloc_obj(struct perm_bits, GFP_KERNEL_ACCOUNT);
	if (!vdev->msi_perm)
		return -ENOMEM;

	/* [한국어] 방금 잰 길이와 하드웨어 플래그로 이 디바이스 전용 표를 채운다. */
	ret = init_pci_cap_msi_perm(vdev->msi_perm, len, flags);
	/* [한국어] 표 초기화 실패. 구조체를 되돌린다. */
	if (ret) {
		/* [한국어] 구조체를 해제한다. vdev->msi_perm 포인터 자체는 NULL 로 비우지 않지만,
		 * 호출 사슬이 곧바로 vfio_config_init 의 실패 경로로 빠지고 그 경로는
		 * msi_perm 을 다시 보지 않는다. */
		kfree(vdev->msi_perm);
		return ret;
	}

	/* [한국어] 계산한 capability 길이. 호출자가 지도를 이만큼 칠한다. */
	return len;
}

/* [한국어]
 * vfio_vc_cap_len - Virtual Channel 계열 확장 capability 의 가변 길이를 계산한다
 *
 * @vdev: 대상 디바이스.
 * @pos: 이 capability 의 시작 절대 오프셋.
 * @return: 성공하면 바이트 길이. 음수면 config 읽기 오류를 변환한 값.
 *
 * 왜 필요한가: VC capability 는 확장 VC 개수와 중재(arbitration) 테이블
 * 크기에 따라 길이가 달라진다. 지도를 정확히 칠하려면 실제 길이가 필요하다.
 * 같은 계산이 VC, VC9, MFVC 세 capability 에 그대로 쓰인다.
 *
 * 동작 과정:
 *  1. 기본 길이에서 시작한다.
 *  2. Port VC Capability 1 에서 확장 VC 개수를 읽는다.
 *  3. Port VC Capability 2 에서 중재 테이블의 위상(phase) 수를 읽는다.
 *     128/64/32 위상 중 지원하는 가장 큰 것을 고르고, 아무것도 없으면 0.
 *  4. 위상당 4비트이므로 위상 수에 4 를 곱해 중재 테이블의 비트 수를 얻는다.
 *  5. VC 하나당 고정 크기 블록이 (1 + 확장 VC 개수) 개 붙는다.
 *  6. 중재 테이블이 있으면 그 시작이 16바이트 정렬이므로 길이를 올림하고
 *     테이블 크기(비트 수 / 8)를 더한다.
 *
 * 상류 주석이 밝히듯 포트 중재 테이블은 루트/스위치 전용이고 함수 중재
 * 테이블은 함수 0 전용이라, 엔드포인트를 넘겨받는 VFIO 에서는 사용자가
 * 이것들을 쓸 일이 없다. 어차피 쓰기를 허용하지 않으므로 크기가 정확한지
 * 자체는 중요하지 않고, 지도를 덮을 범위만 맞으면 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로. 잠들 수 있다.
 *
 * 에러 경로: 두 번의 config 읽기 중 어느 하나라도 실패하면 오류를 올린다.
 * 호출자 vfio_ext_cap_len 이 그대로 전달하고, vfio_ecap_init 이 초기화를
 * 중단한다.
 *
 * 호출 체인:
 *   vfio_ecap_init → vfio_ext_cap_len → [vfio_vc_cap_len]
 *     → pci_read_config_dword
 */
/* Determine extended capability length for VC (2 & 9) and MFVC */
static int vfio_vc_cap_len(struct vfio_pci_core_device *vdev, u16 pos)
{
	/* [한국어] capability 를 읽을 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 읽은 dword 를 담을 임시 변수. 두 번 재사용된다. */
	u32 tmp;
	/* [한국어] 결과 코드, 확장 VC 개수, 중재 테이블 위상 수, 중재 테이블 비트 수. */
	int ret, evcc, phases, vc_arb;
	/* [한국어] 헤더와 포트 VC capability/control/status 를 포함한 고정 부분의 길이. */
	int len = PCI_CAP_VC_BASE_SIZEOF;

	/* [한국어] Port VC Capability 1 을 읽는다. */
	ret = pci_read_config_dword(pdev, pos + PCI_VC_PORT_CAP1, &tmp);
	if (ret)
		return pcibios_err_to_errno(ret);

	/* [한국어] 확장 VC 개수를 뽑는다. 0 이면 VC0 하나만 있다는 뜻이라 아래에서 (1+0)개
	 * 블록을 센다. */
	evcc = tmp & PCI_VC_CAP1_EVCC; /* extended vc count */
	/* [한국어] Port VC Capability 2 를 읽는다. 중재 테이블 형식이 여기 있다. */
	ret = pci_read_config_dword(pdev, pos + PCI_VC_PORT_CAP2, &tmp);
	if (ret)
		return pcibios_err_to_errno(ret);

	/* [한국어] 지원하는 가장 큰 중재 테이블 형식부터 확인한다. 여러 형식을 동시에
	 * 지원할 수 있으므로 큰 쪽을 골라야 테이블 전체를 덮는다. */
	if (tmp & PCI_VC_CAP2_128_PHASE)
		/* [한국어] 128 위상 테이블. */
		phases = 128;
	/* [한국어] 다음으로 64 위상. */
	else if (tmp & PCI_VC_CAP2_64_PHASE)
		/* [한국어] 64 위상 테이블. */
		phases = 64;
	/* [한국어] 다음으로 32 위상. */
	else if (tmp & PCI_VC_CAP2_32_PHASE)
		/* [한국어] 32 위상 테이블. */
		phases = 32;
	else
		/* [한국어] 중재 테이블이 없는 구성. */
		phases = 0;

	/* [한국어] 위상 하나당 4비트를 차지하므로 테이블 전체의 비트 수를 얻는다. */
	vc_arb = phases * 4;

	/*
	 * Port arbitration tables are root & switch only;
	 * function arbitration tables are function 0 only.
	 * In either case, we'll never let user write them so
	 * we don't care how big they are
	 */
	/* [한국어] VC 하나당 고정 크기 자원 블록이 붙는다. VC0 을 포함해 (1 + 확장 VC
	 * 개수) 개다. */
	len += (1 + evcc) * PCI_CAP_VC_PER_VC_SIZEOF;
	/* [한국어] 중재 테이블이 있으면 그 크기까지 더해야 지도를 정확히 칠할 수 있다. */
	if (vc_arb) {
		/* [한국어] 스펙상 중재 테이블은 16바이트 정렬 위치에서 시작한다. */
		len = round_up(len, 16);
		/* [한국어] 비트 수를 바이트 수로 바꿔 더한다. */
		len += vc_arb / 8;
	}
	/* [한국어] 계산한 총 길이. 바로 위 상류 주석대로 이 테이블들은 루트/스위치 또는
	 * 함수 0 전용이고 어차피 쓰기를 허용하지 않으므로, 지도를 덮을 범위만
	 * 맞으면 충분하다. */
	return len;
}

/* [한국어]
 * vfio_cap_len - 가변 길이 표준 capability 의 실제 길이를 하드웨어를 읽어 알아낸다
 *
 * @vdev: 대상 디바이스.
 * @cap: capability ID. pci_cap_length 표에서 0xFF(가변)로 표시된 것만 온다.
 * @pos: 이 capability 의 시작 절대 오프셋.
 * @return: 성공하면 바이트 길이. 음수면 오류. 아는 방법이 없는 ID 면
 *          경고를 남기고 0 을 반환하는데, 호출자는 길이 0 을 "이 capability
 *          를 감춘다" 로 해석한다.
 *
 * 왜 필요한가: 대부분의 표준 capability 는 길이가 고정이라 pci_cap_length
 * 표에 상수로 적어 두면 되지만, 여섯 종류는 하드웨어를 읽어 봐야 안다.
 * 지도를 잘못된 길이로 칠하면 인접 capability 를 침범하거나 빈틈을 남긴다.
 *
 * 각 갈래:
 *  - MSI: 별도 함수에 위임한다. 길이 계산과 표 생성을 함께 해야 하기 때문이다.
 *  - PCI-X: 명령 레지스터의 버전 필드로 v0(8바이트)와 v2(24바이트)를 가른다.
 *    v2 인 김에, config 공간이 256바이트보다 크면 첫 확장 capability 헤더를
 *    읽어 확장 capability 존재 여부를 vdev->extended_caps 에 기록한다.
 *    이 플래그가 나중에 vfio_ecap_init 을 돌릴지 말지를 정한다.
 *  - Vendor-Specific: 스펙이 "길이가 next 필드 다음 바이트에 있다" 고 정해
 *    두었으므로 그 바이트를 그대로 길이로 쓴다.
 *  - PCIe: 여기서도 확장 capability 존재 여부를 확인한다. 길이는 capability
 *    버전과 디바이스 타입으로 갈린다. Root Complex Integrated Endpoint 는
 *    링크가 없어 링크 관련 레지스터가 빠지므로 짧다(v1 은 0xc, v2 는 0x2c).
 *  - HyperTransport: 세 번째 바이트의 특정 비트로 짧은 형식과 긴 형식을 가른다.
 *  - SATA: 인덱스 데이터 쌍이 config 공간에 인라인으로 있는지에 따라 길이가
 *    달라진다.
 *  - 그 밖에: 경고를 남기고 0. 길이를 모르는 capability 를 지도에 칠했다가
 *    이웃을 덮는 것보다 감추는 편이 안전하다는 판단이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로. 잠들 수 있다.
 *
 * 에러 경로: config 읽기 실패는 PCIBIOS 코드를 errno 로 바꿔 올린다.
 * 호출자 vfio_cap_init 이 음수를 보면 초기화 전체를 중단한다. 다만 PCI-X 와
 * PCIe 갈래에서 확장 capability 존재를 확인하는 읽기만은 반환값을 보지 않는데,
 * 실패하면 dword 가 미정의 값이라 extended_caps 판정이 흔들릴 수 있다.
 *
 * 호출 체인:
 *   vfio_config_init → vfio_cap_init → [vfio_cap_len]
 *     → vfio_msi_cap_len, pci_read_config_byte / _word / _dword,
 *       pcie_caps_reg, pci_pcie_type
 */
static int vfio_cap_len(struct vfio_pci_core_device *vdev, u8 cap, u8 pos)
{
	/* [한국어] 하드웨어를 읽을 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 4바이트 읽기용 임시 변수. */
	u32 dword;
	/* [한국어] 2바이트 읽기용. */
	u16 word;
	/* [한국어] 1바이트 읽기용. */
	u8 byte;
	/* [한국어] config 읽기 결과 코드. */
	int ret;

	/* [한국어] 가변 길이 capability 만 여기로 들어온다. */
	switch (cap) {
	/* [한국어] MSI 는 길이 계산과 표 생성을 함께 해야 한다. */
	case PCI_CAP_ID_MSI:
		/* [한국어] 전용 함수에 위임한다. */
		return vfio_msi_cap_len(vdev, pos);
	/* [한국어] PCI-X 는 버전에 따라 길이가 갈린다. */
	case PCI_CAP_ID_PCIX:
		/* [한국어] 명령 레지스터에 버전 필드가 들어 있다. */
		ret = pci_read_config_word(pdev, pos + PCI_X_CMD, &word);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] 버전이 0 이 아니면 v2 이상이다. */
		if (PCI_X_CMD_VERSION(word)) {
			/* [한국어] config 공간이 256바이트보다 크면 확장 capability 가 있을 수 있다.
			 * PCI-X v2 디바이스도 확장 config 공간을 가질 수 있어 여기서 확인한다. */
			if (pdev->cfg_size > PCI_CFG_SPACE_SIZE) {
				/* Test for extended capabilities */
				/* [한국어] 확장 영역의 첫 dword 를 읽어 본다. 반환값을 보지 않아서, 읽기가
				 * 실패하면 dword 가 미정의 값인 채로 아래 판정에 쓰인다. */
				pci_read_config_dword(pdev, PCI_CFG_SPACE_SIZE,
						      &dword);
				/* [한국어] 첫 헤더가 0 이 아니면 확장 capability 가 있다는 뜻이다. 이 플래그가
				 * 나중에 vfio_ecap_init 을 돌릴지 말지를 정한다. */
				vdev->extended_caps = (dword != 0);
			}
			/* [한국어] v2 는 24바이트. */
			return PCI_CAP_PCIX_SIZEOF_V2;
		} else
			/* [한국어] v0 는 8바이트. */
			return PCI_CAP_PCIX_SIZEOF_V0;
	/* [한국어] Vendor-Specific 은 길이 필드가 안에 있다. */
	case PCI_CAP_ID_VNDR:
		/* length follows next field */
		/* [한국어] 스펙이 next 포인터 다음 바이트를 길이로 정해 두었다. 상류 주석의
		 * "length follows next field" 가 그 뜻이다. */
		ret = pci_read_config_byte(pdev, pos + PCI_CAP_FLAGS, &byte);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] 그 바이트가 곧 전체 길이다. */
		return byte;
	/* [한국어] PCIe 는 버전과 디바이스 타입에 따라 갈린다. */
	case PCI_CAP_ID_EXP:
		/* [한국어] PCIe 디바이스에서도 확장 capability 존재를 확인한다. 대부분의 현대
		 * 디바이스가 이 경로로 extended_caps 를 세운다. */
		if (pdev->cfg_size > PCI_CFG_SPACE_SIZE) {
			/* Test for extended capabilities */
			/* [한국어] 확장 영역 첫 dword. 여기서도 반환값을 보지 않는다. */
			pci_read_config_dword(pdev, PCI_CFG_SPACE_SIZE, &dword);
			/* [한국어] 확장 capability 존재 여부를 기록한다. */
			vdev->extended_caps = (dword != 0);
		}

		/* length based on version and type */
		/* [한국어] PCIe capability 구조 버전이 1 이면 옛 배치다. */
		if ((pcie_caps_reg(pdev) & PCI_EXP_FLAGS_VERS) == 1) {
			/* [한국어] Root Complex Integrated Endpoint 는 링크가 없어 링크 관련 레지스터가
			 * 빠진다. */
			if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_END)
				/* [한국어] 상류 주석대로 모든 디바이스에 공통인 앞부분만 있다 — 12바이트. */
				return 0xc; /* "All Devices" only, no link */
			/* [한국어] 링크가 있는 v1 엔드포인트의 전체 길이. */
			return PCI_CAP_EXP_ENDPOINT_SIZEOF_V1;
		} else {
			/* [한국어] v2 배치에서도 링크 없는 엔드포인트를 따로 다룬다. */
			if (pci_pcie_type(pdev) == PCI_EXP_TYPE_RC_END)
				/* [한국어] 링크 관련 레지스터를 뺀 44바이트. 길이표 주석의 "20 or 44" 가 이
				 * 값들을 가리킨다. */
				return 0x2c; /* No link */
			/* [한국어] 링크가 있는 v2 엔드포인트의 전체 길이. */
			return PCI_CAP_EXP_ENDPOINT_SIZEOF_V2;
		}
	/* [한국어] HyperTransport 는 형식 비트로 길이가 갈린다. */
	case PCI_CAP_ID_HT:
		/* [한국어] capability 시작에서 3바이트 뒤에 형식 비트가 있다. 이 오프셋에 이름
		 * 붙은 상수가 없어 숫자로 적혀 있다. */
		ret = pci_read_config_byte(pdev, pos + 3, &byte);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] 3비트 명령 형식이면 짧은 형식, 아니면 긴 형식이다. */
		return (byte & HT_3BIT_CAP_MASK) ?
			HT_CAP_SIZEOF_SHORT : HT_CAP_SIZEOF_LONG;
	/* [한국어] SATA 는 인덱스/데이터 위치에 따라 갈린다. */
	case PCI_CAP_ID_SATA:
		/* [한국어] 레지스터 위치 지정자를 읽는다. */
		ret = pci_read_config_byte(pdev, pos + PCI_SATA_REGS, &byte);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] 위치 필드만 남긴다. */
		byte &= PCI_SATA_REGS_MASK;
		/* [한국어] 인덱스/데이터 쌍이 config 공간 안에 인라인으로 있는 형식이다. */
		if (byte == PCI_SATA_REGS_INLINE)
			/* [한국어] 인라인이면 그만큼 길다. */
			return PCI_SATA_SIZEOF_LONG;
		else
			/* [한국어] BAR 안에 있으면 config 공간 쪽은 짧다. */
			return PCI_SATA_SIZEOF_SHORT;
	default:
		/* [한국어] 길이를 알 방법이 없는 capability 다. 경고를 남겨 나중에 표를 보강할
		 * 근거로 삼는다. */
		pci_warn(pdev, "%s: unknown length for PCI cap %#x@%#x\n",
			 __func__, cap, pos);
	}

	/* [한국어] 길이 0 은 호출자에게 "이 capability 를 감춘다" 는 뜻이다. 길이를
	 * 모르는 것을 지도에 칠했다가 이웃 capability 를 덮는 것보다 안전하다. */
	return 0;
}

/* [한국어]
 * vfio_ext_cap_len - 가변 길이 확장 capability 의 실제 길이를 하드웨어를 읽어 알아낸다
 *
 * @vdev: 대상 디바이스.
 * @ecap: 확장 capability ID.
 * @epos: 이 capability 의 시작 절대 오프셋(256 이상).
 * @return: 성공하면 바이트 길이. 음수면 오류. 모르는 ID 면 경고 후 0
 *          (= 감춘다).
 *
 * 왜 필요한가: vfio_cap_len 의 확장 capability 판이다. pci_ext_cap_length 표에
 * 0xFF 로 표시된 것들이 여기로 온다.
 *
 * 각 갈래:
 *  - 확장 Vendor-Specific: 헤더에 길이 필드가 있어 그대로 뽑는다.
 *  - VC / VC9 / MFVC: 세 가지 모두 같은 계산이라 vfio_vc_cap_len 에 위임한다.
 *  - ACS(Access Control Services): egress control 을 지원하면 egress 제어
 *    벡터가 뒤에 붙는다. 벡터 크기 필드가 0 이면 256비트를 뜻하고, 아니면
 *    그 값을 32비트 단위로 올림한다. 기본 8바이트에 벡터 바이트 수를 더한다.
 *  - Resizable BAR: 제어 레지스터의 BAR 개수 필드를 읽어 항목당 8바이트씩
 *    더한다.
 *  - DPA(Dynamic Power Allocation): substate 개수만큼 바이트가 붙는다.
 *    필드가 0 기반이라 1 을 더한다.
 *  - TPH(TLP Processing Hints): steering tag 테이블이 config 공간 안에 있는
 *    경우에만 테이블 크기를 더한다. 항목당 2바이트이고 개수 필드가 0 기반이라
 *    끝에 2 를 더 붙인다.
 *  - DVSEC: 헤더에 길이 필드가 있어 그대로 뽑는다.
 *  - 그 밖에: 경고 후 0.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로. 잠들 수 있다.
 *
 * 에러 경로: 모든 config 읽기가 반환값을 검사하고 실패하면 errno 로 바꿔
 * 올린다. 호출자 vfio_ecap_init 이 초기화를 중단한다.
 *
 * 호출 체인:
 *   vfio_config_init → vfio_ecap_init → [vfio_ext_cap_len]
 *     → pci_read_config_byte / _dword, vfio_vc_cap_len
 */
static int vfio_ext_cap_len(struct vfio_pci_core_device *vdev, u16 ecap, u16 epos)
{
	/* [한국어] 하드웨어를 읽을 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 1바이트 읽기용 임시 변수. */
	u8 byte;
	/* [한국어] 4바이트 읽기용. */
	u32 dword;
	/* [한국어] config 읽기 결과 코드. */
	int ret;

	/* [한국어] 가변 길이 확장 capability 만 여기로 온다. */
	switch (ecap) {
	/* [한국어] 확장 Vendor-Specific. */
	case PCI_EXT_CAP_ID_VNDR:
		/* [한국어] 벤더 헤더에 길이 필드가 들어 있다. */
		ret = pci_read_config_dword(pdev, epos + PCI_VNDR_HEADER,
					    &dword);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] 그 필드를 뽑아 길이로 쓴다. */
		return PCI_VNDR_HEADER_LEN(dword);
	/* [한국어] Virtual Channel. */
	case PCI_EXT_CAP_ID_VC:
	/* [한국어] VC 의 별칭 ID. */
	case PCI_EXT_CAP_ID_VC9:
	/* [한국어] Multi-Function VC. 세 가지 모두 같은 구조라 한 함수로 처리한다. */
	case PCI_EXT_CAP_ID_MFVC:
		/* [한국어] 공통 계산 함수에 위임. */
		return vfio_vc_cap_len(vdev, epos);
	/* [한국어] Access Control Services. */
	case PCI_EXT_CAP_ID_ACS:
		/* [한국어] ACS 능력 비트를 읽는다. */
		ret = pci_read_config_byte(pdev, epos + PCI_ACS_CAP, &byte);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] Egress Control 을 지원하면 뒤에 egress 제어 벡터가 붙는다. */
		if (byte & PCI_ACS_EC) {
			/* [한국어] 벡터의 비트 수. */
			int bits;

			/* [한국어] 벡터 크기 필드를 읽는다. */
			ret = pci_read_config_byte(pdev,
						   epos + PCI_ACS_EGRESS_BITS,
						   &byte);
			if (ret)
				return pcibios_err_to_errno(ret);

			/* [한국어] 크기 필드가 0 이면 스펙상 256비트를 뜻한다. 0 이 아니면 그 값을
			 * 32비트 단위로 올림한다 — 벡터가 dword 경계에 맞춰 놓이기 때문이다. */
			bits = byte ? round_up(byte, 32) : 256;
			/* [한국어] 헤더 + 능력/제어 레지스터 8바이트에 벡터 바이트 수를 더한다. */
			return 8 + (bits / 8);
		}
		/* [한국어] egress control 이 없으면 8바이트로 끝난다. */
		return 8;

	/* [한국어] Resizable BAR. */
	case PCI_EXT_CAP_ID_REBAR:
		/* [한국어] 제어 레지스터에 몇 개의 BAR 를 다루는지 들어 있다. */
		ret = pci_read_config_byte(pdev, epos + PCI_REBAR_CTRL, &byte);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] BAR 개수 필드만 남기고 */
		byte &= PCI_REBAR_CTRL_NBAR_MASK;
		/* [한국어] 제자리로 민다. */
		byte >>= PCI_REBAR_CTRL_NBAR_SHIFT;

		/* [한국어] 헤더 4바이트에 BAR 하나당 능력/제어 8바이트씩 더한다. */
		return 4 + (byte * 8);
	/* [한국어] Dynamic Power Allocation. */
	case PCI_EXT_CAP_ID_DPA:
		/* [한국어] substate 개수 필드를 읽는다. */
		ret = pci_read_config_byte(pdev, epos + PCI_DPA_CAP, &byte);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] 개수 필드만 남긴다. */
		byte &= PCI_DPA_CAP_SUBSTATE_MASK;
		/* [한국어] 고정 부분에 substate 당 1바이트씩 더한다. 필드가 0 기반이라 1 을
		 * 더해야 실제 개수가 된다. */
		return PCI_DPA_BASE_SIZEOF + byte + 1;
	/* [한국어] TLP Processing Hints. */
	case PCI_EXT_CAP_ID_TPH:
		/* [한국어] 능력 레지스터에 steering tag 테이블의 위치와 크기가 들어 있다. */
		ret = pci_read_config_dword(pdev, epos + PCI_TPH_CAP, &dword);
		if (ret)
			return pcibios_err_to_errno(ret);

		/* [한국어] 테이블이 이 capability 안에 있는 경우에만 길이가 늘어난다. MMIO 나
		 * 다른 곳에 있으면 config 공간 쪽은 고정 크기다. */
		if ((dword & PCI_TPH_CAP_LOC_MASK) == PCI_TPH_LOC_CAP) {
			/* [한국어] steering tag 개수. */
			int sts;

			/* [한국어] 개수 필드만 남기고 */
			sts = dword & PCI_TPH_CAP_ST_MASK;
			/* [한국어] 제자리로 민다. */
			sts >>= PCI_TPH_CAP_ST_SHIFT;
			/* [한국어] 고정 부분에 태그당 2바이트를 더하고, 개수 필드가 0 기반이라 한 항목
			 * 분(2바이트)을 더 붙인다. */
			return PCI_TPH_BASE_SIZEOF + (sts * 2) + 2;
		}
		/* [한국어] 테이블이 밖에 있으면 고정 크기. */
		return PCI_TPH_BASE_SIZEOF;
	/* [한국어] Designated Vendor-Specific. */
	case PCI_EXT_CAP_ID_DVSEC:
		/* [한국어] 첫 헤더에 길이 필드가 들어 있다. */
		ret = pci_read_config_dword(pdev, epos + PCI_DVSEC_HEADER1, &dword);
		if (ret)
			return pcibios_err_to_errno(ret);
		/* [한국어] 그 필드를 뽑아 길이로 쓴다. */
		return PCI_DVSEC_HEADER1_LEN(dword);
	default:
		/* [한국어] 길이를 알 수 없는 확장 capability. 경고를 남긴다. */
		pci_warn(pdev, "%s: unknown length for PCI ecap %#x@%#x\n",
			 __func__, ecap, epos);
	}

	/* [한국어] 길이 0 = 감춤. */
	return 0;
}

/* [한국어]
 * vfio_update_pm_vconfig_bytes - PM capability 를 vconfig 에 채운 직후 PME 관련 비트를 지운다
 *
 * @vdev: 대상 디바이스.
 * @offset: PM capability 의 시작 절대 오프셋.
 * @return: 없다.
 *
 * 왜 필요한가: init_pci_cap_pm_perm 이 PME 관련 비트를 가상화 대상으로
 * 잡아 두었지만, 가상화만으로는 부족하다. vconfig 는 vfio_fill_vconfig_bytes
 * 가 하드웨어 값을 그대로 복사해 만들기 때문에, 가상화된 비트에도 하드웨어의
 * 초기값이 그대로 들어 있다. 그 상태로 두면 사용자에게 "이 디바이스는 PME 를
 * 지원한다" 고 보인다. 게스트는 PME 를 처리할 수 없고 실제 PME 는 호스트가
 * 받아 상태를 지우므로, 지원하지 않는 것처럼 보여야 옳다. 상류 주석이
 * init_pci_cap_pm_perm 에서 "vconfig 비트는 디바이스 capability 초기화 중에
 * 지워진다" 고 예고한 그 지점이 바로 여기다.
 *
 * 동작 과정: PM Capabilities 레지스터에서 PME_Support 필드를 지우고,
 * PM Control 레지스터에서 PME_En 과 PME_Status 를 지운다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로. 메모리 조작뿐이라
 * 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_config_init → vfio_cap_init → [vfio_update_pm_vconfig_bytes]
 */
static void vfio_update_pm_vconfig_bytes(struct vfio_pci_core_device *vdev,
					 int offset)
{
	/* [한국어] 그림자 복사본의 PM Capabilities 레지스터 위치. 여기에 PME_Support 가 있다. */
	__le16 *pmc = (__le16 *)&vdev->vconfig[offset + PCI_PM_PMC];
	/* [한국어] 그림자 복사본의 PM Control/Status 위치. PME_En 과 PME_Status 가 있다. */
	__le16 *ctrl = (__le16 *)&vdev->vconfig[offset + PCI_PM_CTRL];

	/* Clear vconfig PME_Support, PME_Status, and PME_En bits */
	/* [한국어] PME_Support 필드를 통째로 지운다. 이 필드는 표에서 가상화 대상이라
	 * 사용자는 vconfig 값을 읽으며, 그 결과 "PME 를 어느 전원 상태에서도
	 * 지원하지 않는 디바이스" 로 보인다. init_pci_cap_pm_perm 의 상류 주석이
	 * 예고한 "vconfig 비트는 디바이스 capability 초기화 중에 지워진다" 가
	 * 바로 이 줄이다. */
	*pmc &= ~cpu_to_le16(PCI_PM_CAP_PME_MASK);
	/* [한국어] PME_En 과 PME_Status 도 지운다. 게스트는 D3cold 에서 온 PME 를 처리할
	 * 수 없고 실제 처리는 호스트가 하므로, 사용자에게는 늘 꺼져 있는 것처럼
	 * 보여야 일관적이다. */
	*ctrl &= ~cpu_to_le16(PCI_PM_CTRL_PME_ENABLE | PCI_PM_CTRL_PME_STATUS);
}

/* [한국어]
 * vfio_fill_vconfig_bytes - 하드웨어 config 공간의 한 구간을 vconfig 그림자 복사본에 그대로 읽어 담는다
 *
 * @vdev: 대상 디바이스.
 * @offset: 복사를 시작할 절대 오프셋.
 * @size: 복사할 바이트 수.
 * @return: 0 성공. 아니면 PCIBIOS 계열 config 읽기 오류(양수).
 *
 * 왜 필요한가: 가상화된 비트의 초기값은 하드웨어의 현재 값이어야 한다.
 * 그래야 사용자가 아직 아무것도 쓰지 않은 상태에서 읽었을 때 진짜 디바이스와
 * 같은 값을 본다. 지도를 칠할 때마다 그 구간을 이 함수로 채운다.
 *
 * 동작 과정: 상류 주석이 밝히듯 "모든 필드가 dword 접근을 지원한다" 는 가정
 * 아래 가능한 한 큰 폭으로 읽는다. pci_save_state 도 같은 가정을 하며 잘
 * 동작한다는 것이 근거다.
 *  1. 남은 크기가 4 이상이고 오프셋이 4바이트 정렬이면 dword 로 읽는다.
 *  2. 아니면 2 이상이고 2바이트 정렬이면 word 로.
 *  3. 그것도 아니면 바이트 하나씩.
 *  4. 읽은 폭만큼 오프셋을 전진시키고 남은 크기를 줄인다.
 * 읽은 값은 cpu_to_le 계열로 리틀엔디언으로 바꿔 담는다. vconfig 는 PCI
 * config 공간의 바이트 순서를 그대로 흉내 내야 하기 때문이다. 바이트 단위
 * 읽기만은 변환이 필요 없어 목적지를 직접 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로. 잠들 수 있다.
 *
 * 에러 경로: 어느 한 번의 읽기라도 실패하면 즉시 반환한다. 그때까지 채운
 * 부분은 그대로 남지만, 호출자가 초기화 전체를 중단하고 vconfig 를 해제한다.
 *
 * 호출 체인:
 *   vfio_config_init / vfio_cap_init / vfio_ecap_init
 *     → [vfio_fill_vconfig_bytes] → pci_read_config_dword / _word / _byte
 */
static int vfio_fill_vconfig_bytes(struct vfio_pci_core_device *vdev,
				   int offset, int size)
{
	/* [한국어] 하드웨어를 읽을 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 읽기 결과. 크기가 0 이면 루프를 한 번도 돌지 않으므로 초기값이 그대로
	 * 반환된다. */
	int ret = 0;

	/*
	 * We try to read physical config space in the largest chunks
	 * we can, assuming that all of the fields support dword access.
	 * pci_save_state() makes this same assumption and seems to do ok.
	 */
	/* [한국어] 남은 바이트가 없어질 때까지 반복한다. */
	while (size) {
		/* [한국어] 이번 반복에서 채운 바이트 수. */
		int filled;

		/* [한국어] 가능하면 dword 로 읽는다. 상류 주석이 근거를 밝힌다 — 모든 필드가
		 * dword 접근을 지원한다고 가정하며, 커널의 pci_save_state 도 같은 가정을
		 * 하고 잘 동작한다. */
		if (size >= 4 && !(offset % 4)) {
			/* [한국어] 그림자 복사본의 목적지를 __le32 로 본다. */
			__le32 *dwordp = (__le32 *)&vdev->vconfig[offset];
			/* [한국어] CPU 표현으로 읽어 올 임시 변수. */
			u32 dword;

			/* [한국어] 하드웨어에서 4바이트를 읽는다. */
			ret = pci_read_config_dword(pdev, offset, &dword);
			if (ret)
				return ret;
			/* [한국어] 리틀엔디언으로 바꿔 담는다. vconfig 는 PCI config 공간의 바이트 순서를
			 * 그대로 흉내 내야 하기 때문이다. */
			*dwordp = cpu_to_le32(dword);
			/* [한국어] 4바이트 처리 완료. */
			filled = 4;
		/* [한국어] dword 가 안 되면 2바이트 정렬로 시도한다. */
		} else if (size >= 2 && !(offset % 2)) {
			/* [한국어] 목적지를 __le16 로. */
			__le16 *wordp = (__le16 *)&vdev->vconfig[offset];
			/* [한국어] CPU 표현 임시 변수. */
			u16 word;

			/* [한국어] 2바이트 읽기. */
			ret = pci_read_config_word(pdev, offset, &word);
			if (ret)
				return ret;
			/* [한국어] 리틀엔디언 변환 후 저장. */
			*wordp = cpu_to_le16(word);
			/* [한국어] 2바이트 처리 완료. */
			filled = 2;
		} else {
			/* [한국어] 마지막 수단은 바이트 단위다. 1바이트는 엔디언 개념이 없어 목적지를
			 * 접근자에 직접 넘길 수 있다. */
			u8 *byte = &vdev->vconfig[offset];
			/* [한국어] 하드웨어 값을 그림자 복사본에 바로 읽어 넣는다. */
			ret = pci_read_config_byte(pdev, offset, byte);
			if (ret)
				return ret;
			/* [한국어] 1바이트 처리 완료. */
			filled = 1;
		}

		/* [한국어] 다음 위치로 전진한다. */
		offset += filled;
		/* [한국어] 남은 크기를 줄인다. */
		size -= filled;
	}

	/* [한국어] 마지막 읽기의 결과. 0 이면 전 구간 성공이다. */
	return ret;
}

/* [한국어]
 * vfio_cap_init - 표준 capability chain 을 순회하며 지도를 칠하고 vconfig 를 채우며 감출 것은 chain 에서 빼낸다
 *
 * @vdev: 대상 디바이스.
 * @return: 0 성공(capability 가 아예 없는 경우 포함). 아니면 config 읽기
 *          오류나 vfio_cap_len 이 낸 음수 오류.
 *
 * 왜 필요한가: 이 파일의 판정 체계 전체가 pci_config_map 위에서 돈다. 그
 * 지도를 만드는 것이 이 함수다. 동시에 사용자에게 보일 capability 목록을
 * 여기서 재작성한다 — 커널이 다루고 싶지 않은 capability 는 앞 capability 의
 * next 포인터를 vconfig 안에서 고쳐 통째로 건너뛰게 만든다.
 *
 * 동작 과정:
 *  1. STATUS 레지스터의 Capability List 비트를 확인한다. 없으면 chain 자체가
 *     없으므로 성공으로 끝낸다.
 *  2. Capability List 포인터에서 첫 capability 오프셋을 읽는다.
 *  3. prev 를 vconfig 의 Capability List 바이트로 둔다. 첫 capability 를
 *     감춰야 할 경우 여기를 고쳐 목록의 시작점을 옮긴다.
 *  4. 루프 횟수를 제한한다. capability 는 dword 정렬이라 표준 영역
 *     (64~255바이트)에 들어갈 수 있는 최대 개수가 정해져 있다. 이 상한이
 *     없으면 고장 난 하드웨어의 순환 chain 에 커널이 갇힌다.
 *  5. 각 capability 마다:
 *     a. ID 와 next 포인터를 읽는다.
 *     b. ID 가 0 이면 NULL capability 다. 상류 주석대로 이 파일이 표준 헤더에
 *        쓰는 가짜 ID(PCI_CAP_ID_BASIC = 0)와 충돌하므로 감춘 것으로 취급한다.
 *        ID 가 표 범위를 넘어도 마찬가지다.
 *     c. 표에서 길이를 얻는다. 0xFF 면 가변이라 vfio_cap_len 으로 실측한다.
 *     d. 길이가 0 이면 감출 capability 다. prev 가 가리키는 vconfig 바이트에
 *        next 값을 써서 이 capability 를 목록에서 빼내고 다음으로 넘어간다.
 *        지도는 칠하지 않으므로 그 영역은 PCI_CAP_ID_INVALID 인 채로 남아
 *        raw 접근이 된다 — 즉 사용자가 오프셋을 알고 직접 읽으면 여전히
 *        보인다. 목록에서만 사라질 뿐이다.
 *     e. 이 구간이 이미 다른 capability 로 칠해져 있으면 하드웨어의 chain 이
 *        겹친다는 뜻이라 경고를 남긴다. 덮어쓰기는 그대로 진행한다.
 *     f. 컴파일 시 검사로, 실제 capability ID 의 최댓값이 특수 ID 와 겹치지
 *        않음을 확인한다. 겹치면 지도의 특수 표시가 진짜 capability 와
 *        구분되지 않는다.
 *     g. 지도를 이 capability ID 로 길이만큼 칠하고, vconfig 에 하드웨어
 *        값을 채운다.
 *     h. PM capability 라면 PME 관련 비트를 지운다.
 *     i. prev 를 이 capability 의 next 포인터 자리로 옮기고 다음으로 간다.
 *  6. 하나도 채우지 못했으면 vconfig 의 STATUS 에서 Capability List 비트를
 *     지운다. 목록이 비었는데 "목록 있음" 이라고 말하면 사용자 드라이버가
 *     빈 목록을 순회하게 된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로에서 한 번만 돈다.
 * 아직 사용자에게 노출되지 않은 상태라 동기화가 필요 없다.
 *
 * 에러 경로: 어느 단계든 오류가 나면 그대로 반환하고 vfio_config_init 이
 * 지도와 vconfig 를 통째로 해제한다.
 *
 * 호출 체인:
 *   vfio_pci_core_enable → vfio_config_init → [vfio_cap_init]
 *     → pci_read_config_word / _byte, vfio_cap_len,
 *       vfio_fill_vconfig_bytes, vfio_update_pm_vconfig_bytes
 */
static int vfio_cap_init(struct vfio_pci_core_device *vdev)
{
	/* [한국어] capability chain 을 읽을 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 칠할 지도. vfio_config_init 이 이미 표준 헤더 영역만 칠하고 나머지는
	 * PCI_CAP_ID_INVALID 로 채워 둔 상태다. */
	u8 *map = vdev->pci_config_map;
	/* [한국어] STATUS 레지스터 값. capability 목록 유무를 여기서 본다. */
	u16 status;
	/* [한국어] pos 는 현재 capability 오프셋, prev 는 "이 capability 를 감출 때 고쳐야
	 * 할 vconfig 안의 next 바이트 주소", cap 은 현재 capability ID 다. */
	u8 pos, *prev, cap;
	/* [한국어] 순환 chain 방어용 반복 상한, 결과 코드, 실제로 채운 capability 개수. */
	int loops, ret, caps = 0;

	/* Any capabilities? */
	/* [한국어] 하드웨어의 STATUS 를 읽는다. */
	ret = pci_read_config_word(pdev, PCI_STATUS, &status);
	if (ret)
		return ret;

	/* [한국어] Capability List 비트가 없으면 chain 자체가 없다. */
	if (!(status & PCI_STATUS_CAP_LIST))
		/* [한국어] 성공으로 끝낸다. vconfig 의 STATUS 사본에도 그 비트가 없으므로 사용자
		 * 쪽 표시가 이미 일관적이다. */
		return 0; /* Done */

	/* [한국어] chain 의 첫 capability 오프셋을 읽는다. */
	ret = pci_read_config_byte(pdev, PCI_CAPABILITY_LIST, &pos);
	if (ret)
		return ret;

	/* Mark the previous position in case we want to skip a capability */
	/* [한국어] chain 재작성의 시작점. 첫 capability 를 감춰야 하면 이 바이트를 고쳐
	 * 목록의 머리를 다음 것으로 옮긴다. 이 바이트는 표에서 가상화 대상이라
	 * 사용자가 읽으면 여기 값이 그대로 나간다. */
	prev = &vdev->vconfig[PCI_CAPABILITY_LIST];

	/* We can bound our loop, capabilities are dword aligned */
	/* [한국어] 반복 상한. 상류 주석대로 capability 는 dword 정렬이므로 표준 영역
	 * (64~255바이트)에 들어갈 수 있는 최대 개수가 정해진다. 이 상한이 없으면
	 * 고장 나거나 악의적인 하드웨어의 순환 chain 에 커널이 무한 루프로 갇힌다. */
	loops = (PCI_CFG_SPACE_SIZE - PCI_STD_HEADER_SIZEOF) / PCI_CAP_SIZEOF;
	/* [한국어] 오프셋 0 은 chain 의 끝이다. 상한도 함께 소모한다. */
	while (pos && loops--) {
		/* [한국어] 다음 capability 의 오프셋. */
		u8 next;
		/* [한국어] 겹침 검사용 인덱스와 이 capability 의 길이. 0 으로 시작해 "감춤" 이
		 * 기본값이 되게 한다. */
		int i, len = 0;

		/* [한국어] capability ID 바이트를 읽는다. */
		ret = pci_read_config_byte(pdev, pos, &cap);
		if (ret)
			return ret;

		/* [한국어] next 포인터 바이트를 읽는다. 감추더라도 이 값이 있어야 다음으로 넘어간다. */
		ret = pci_read_config_byte(pdev,
					   pos + PCI_CAP_LIST_NEXT, &next);
		if (ret)
			return ret;

		/*
		 * ID 0 is a NULL capability, conflicting with our fake
		 * PCI_CAP_ID_BASIC.  As it has no content, consider it
		 * hidden for now.
		 */
		/* [한국어] ID 가 0 이거나 표 범위를 넘으면 길이를 0 으로 둬 감춘다. 상류 주석이
		 * ID 0 을 감추는 이유를 밝힌다 — 이 파일이 표준 헤더에 붙인 가짜 ID 와
		 * 충돌하는데, NULL capability 는 내용이 없어 감춰도 잃을 것이 없다. */
		if (cap && cap <= PCI_CAP_ID_MAX) {
			/* [한국어] 길이표에서 찾는다. */
			len = pci_cap_length[cap];
			/* [한국어] 가변 길이 표시. 하드웨어를 읽어 실측해야 한다. */
			if (len == 0xFF) { /* Variable length */
				/* [한국어] 실측한다. 음수면 오류다. */
				len = vfio_cap_len(vdev, cap, pos);
				if (len < 0)
					return len;
			}
		}

		/* [한국어] 길이가 0 이면 감출 capability 다. 표에서 0 이었거나, vfio_cap_len 이
		 * 길이를 알 수 없어 0 을 돌려준 경우다. */
		if (!len) {
			/* [한국어] 어떤 capability 를 감췄는지 디버그 로그로 남긴다. */
			pci_dbg(pdev, "%s: hiding cap %#x@%#x\n", __func__,
				cap, pos);
			/* [한국어] **chain 재작성의 실체.** 앞 capability 의 next 바이트(또는 chain 의
			 * 머리)를 이 capability 의 next 값으로 덮어써서, 사용자가 목록을 따라갈 때
			 * 이 항목을 아예 건너뛰게 만든다. 지도는 칠하지 않으므로 이 영역은
			 * PCI_CAP_ID_INVALID 로 남아 raw 접근이 된다 — 오프셋을 직접 아는 사용자는
			 * 여전히 읽고 쓸 수 있고, 목록에서만 사라진다. */
			*prev = next;
			/* [한국어] 다음 capability 로. */
			pos = next;
			continue;
		}

		/* Sanity check, do we overlap other capabilities? */
		/* [한국어] 겹침 검사. 길이만큼 지도를 훑는다. */
		for (i = 0; i < len; i++) {
			/* [한국어] 아직 아무도 칠하지 않은 자리가 정상이다. */
			if (likely(map[pos + i] == PCI_CAP_ID_INVALID))
				continue;

			/* [한국어] 이미 칠해져 있으면 하드웨어의 chain 이 겹친다는 뜻이다. 경고만 남기고
			 * 덮어쓰기는 그대로 진행한다 — 어느 쪽이 옳은지 판단할 근거가 없고,
			 * 지도가 비어 있는 것보다는 낫기 때문이다. */
			pci_warn(pdev, "%s: PCI config conflict @%#x, was cap %#x now cap %#x\n",
				 __func__, pos + i, map[pos + i], cap);
		}

		/* [한국어] 컴파일 시 검사. 진짜 capability ID 의 최댓값이 특수 표시값(0xFE, 0xFF)에
		 * 닿으면 지도에서 "감춘 자리" 와 "진짜 capability" 를 구별할 수 없게 된다. */
		BUILD_BUG_ON(PCI_CAP_ID_MAX >= PCI_CAP_ID_INVALID_VIRT);

		/* [한국어] 지도를 이 capability ID 로 길이만큼 칠한다. 이 한 줄이 앞으로 이
		 * 영역의 모든 접근이 어느 권한 표를 쓸지 결정한다. */
		memset(map + pos, cap, len);
		/* [한국어] 그림자 복사본에 하드웨어의 현재 값을 채운다. 가상화된 비트의 초기값이
		 * 진짜 값이어야 사용자가 처음 읽을 때 정상적인 디바이스로 보인다. */
		ret = vfio_fill_vconfig_bytes(vdev, pos, len);
		if (ret)
			return ret;

		/* [한국어] PM capability 는 채운 직후 PME 관련 비트를 지워야 한다. */
		if (cap == PCI_CAP_ID_PM)
			/* [한국어] 게스트가 처리할 수 없는 PME 기능을 숨긴다. */
			vfio_update_pm_vconfig_bytes(vdev, pos);

		/* [한국어] 다음 반복에서 감출 일이 생기면 고칠 자리를 이 capability 의 next
		 * 바이트로 옮긴다. 이 바이트는 표에서 가상화 대상이라 사용자가 읽으면
		 * 여기 값이 나간다. */
		prev = &vdev->vconfig[pos + PCI_CAP_LIST_NEXT];
		/* [한국어] 다음 capability 로 이동. */
		pos = next;
		/* [한국어] 실제로 채운 capability 개수를 센다. */
		caps++;
	}

	/* If we didn't fill any capabilities, clear the status flag */
	/* [한국어] 하나도 채우지 못했다. 하드웨어에는 chain 이 있었지만 전부 감췄거나
	 * 길이를 알 수 없었던 경우다. */
	if (!caps) {
		/* [한국어] 그림자 복사본의 STATUS 위치. */
		__le16 *vstatus = (__le16 *)&vdev->vconfig[PCI_STATUS];
		/* [한국어] Capability List 비트를 지운다. 그 비트는 표에서 가상화 대상이라 이
		 * 값이 사용자에게 그대로 보인다. 목록이 비었는데 "목록 있음" 이라고 하면
		 * 사용자 드라이버가 빈 chain 을 순회하다 혼란에 빠진다. */
		*vstatus &= ~cpu_to_le16(PCI_STATUS_CAP_LIST);
	}

	/* [한국어] 지도 칠하기 완료. */
	return 0;
}

/* [한국어]
 * vfio_ecap_init - 확장 capability chain 을 순회하며 지도를 칠하고 감출 것은 chain 에서 빼낸다
 *
 * @vdev: 대상 디바이스.
 * @return: 0 성공(확장 capability 가 없는 경우 포함). 아니면 config 읽기
 *          오류나 vfio_ext_cap_len 이 낸 음수 오류.
 *
 * 왜 필요한가: vfio_cap_init 의 확장 capability 판이다. 다만 확장 chain 은
 * 구조가 달라 감추는 방법이 두 가지로 갈린다. 표준 capability 는 next 가
 * 별도 바이트라 앞 capability 의 그 바이트만 고치면 되지만, 확장 capability 는
 * ID/버전/next 가 첫 dword 에 함께 들어 있고, chain 의 시작점이 고정
 * 오프셋(256)이라 "첫 항목을 건너뛴다" 는 개념이 없다.
 *
 * 동작 과정:
 *  1. vfio_cap_len 이 확인해 둔 extended_caps 플래그가 꺼져 있으면 할 일이 없다.
 *  2. 확장 영역의 고정 시작 오프셋에서 출발한다.
 *  3. 루프 상한을 확장 영역 크기로 정한다. 순환 chain 방어다.
 *  4. 각 확장 capability 마다:
 *     a. 첫 dword 를 읽어 ID 를 뽑는다.
 *     b. 표에서 길이를 얻고, 가변이면 vfio_ext_cap_len 으로 실측한다.
 *     c. 길이가 0(감출 대상)이고 앞에 다른 capability 가 있었다면, 앞
 *        capability 의 첫 dword 안 next 필드만 새 값으로 갈아끼워 건너뛴다.
 *        마스크가 상위 12비트 자리를 지우고 새 오프셋을 그 자리로 민다.
 *     d. 감출 대상인데 앞이 없다면(= chain 의 첫 항목) 건너뛸 방법이 없다.
 *        대신 자리를 지키는 껍데기를 남긴다. 지도를 그 자리만큼 칠하고
 *        hidden 표시를 세워, 아래에서 ID 와 버전을 0 으로 만들고 next 만
 *        살린다. 상류 주석이 밝히듯 이 자리는 direct readfn 이 자동으로
 *        가상화해 준다.
 *     e. 겹침 검사와 경고는 표준판과 같다.
 *     f. 컴파일 시 검사로 확장 ID 최댓값이 특수 ID 와 겹치지 않음을 확인한다.
 *        상류 주석은 확장 ID 가 2바이트인데도 1바이트 지도를 쓰는 이유를
 *        설명한다 — 아직 0xFE 근처까지 간 ID 가 없고, 가면 지도를 2바이트로
 *        바꿔야 한다.
 *     g. 지도를 칠하고 vconfig 를 채운다.
 *     h. 껍데기 자리면 ID 와 버전을 지워 next 만 남기고, 진짜 capability 면
 *        개수를 센다.
 *     i. prev 를 이 capability 의 첫 dword 로 두고 next 오프셋으로 이동한다.
 *  5. 진짜 확장 capability 가 하나도 없었으면 확장 영역의 첫 dword 를 통째로
 *     0 으로 만든다. 상류 주석대로 스펙은 확장 capability 가 없을 때 ID/버전/
 *     next 를 모두 0 으로 두라고 하며, 사용자가 next 까지 따라와 주기를 기대한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 열기 경로에서 한 번.
 *
 * 에러 경로: 오류를 그대로 올리면 vfio_config_init 이 전부 해제한다.
 *
 * 호출 체인:
 *   vfio_pci_core_enable → vfio_config_init → [vfio_ecap_init]
 *     → pci_read_config_dword, vfio_ext_cap_len, vfio_fill_vconfig_bytes
 */
static int vfio_ecap_init(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 확장 chain 을 읽을 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 칠할 지도. */
	u8 *map = vdev->pci_config_map;
	/* [한국어] 현재 확장 capability 의 오프셋. 확장 영역은 256 이상이라 u8 로는 담을 수
	 * 없어 u16 이다. */
	u16 epos;
	/* [한국어] 앞 확장 capability 의 첫 dword 주소. NULL 이면 아직 첫 항목이라는 뜻이고,
	 * 그때는 감출 방법이 달라진다. */
	__le32 *prev = NULL;
	/* [한국어] 반복 상한, 결과 코드, 실제로 채운 확장 capability 개수. */
	int loops, ret, ecaps = 0;

	/* [한국어] vfio_cap_len 이 PCI-X 나 PCIe capability 를 만났을 때 확인해 둔 플래그다.
	 * 확장 capability 가 없으면 할 일이 없다. */
	if (!vdev->extended_caps)
		return 0;

	/* [한국어] 확장 chain 의 시작은 언제나 고정 오프셋 256 이다. 표준 chain 과 달리
	 * "머리 포인터" 가 따로 없다 — 이 차이가 첫 항목을 감추기 어렵게 만든다. */
	epos = PCI_CFG_SPACE_SIZE;

	/* [한국어] 확장 영역 크기 기준의 반복 상한. 순환 chain 방어다. */
	loops = (pdev->cfg_size - PCI_CFG_SPACE_SIZE) / PCI_CAP_SIZEOF;

	/* [한국어] 오프셋이 확장 영역 아래로 내려가면 chain 의 끝이다. next 가 0 이면
	 * epos 가 0 이 되어 이 조건에서 빠져나온다. */
	while (loops-- && epos >= PCI_CFG_SPACE_SIZE) {
		/* [한국어] 확장 capability 의 첫 dword. ID, 버전, next 가 함께 들어 있다. */
		u32 header;
		/* [한국어] 이 capability 의 ID. */
		u16 ecap;
		/* [한국어] 겹침 검사 인덱스와 길이. 0 이 기본이라 모르는 것은 자동으로 감춰진다. */
		int i, len = 0;
		/* [한국어] "이 자리는 chain 을 잇기 위한 껍데기" 표시. 아래에서 ID 와 버전을
		 * 지울지 판단하는 데 쓴다. */
		bool hidden = false;

		/* [한국어] 확장 헤더를 통째로 읽는다. */
		ret = pci_read_config_dword(pdev, epos, &header);
		if (ret)
			return ret;

		/* [한국어] 헤더에서 ID 필드를 뽑는다. */
		ecap = PCI_EXT_CAP_ID(header);

		/* [한국어] 표 범위 안이면 길이를 찾는다. */
		if (ecap <= PCI_EXT_CAP_ID_MAX) {
			/* [한국어] 길이표 조회. */
			len = pci_ext_cap_length[ecap];
			/* [한국어] 가변이면 실측한다. */
			if (len == 0xFF) {
				/* [한국어] 하드웨어를 읽어 잰다. */
				len = vfio_ext_cap_len(vdev, ecap, epos);
				if (len < 0)
					return len;
			}
		}

		if (!len) {
			/* [한국어] 감출 확장 capability 를 디버그 로그로 남긴다. */
			pci_dbg(pdev, "%s: hiding ecap %#x@%#x\n",
				__func__, ecap, epos);

			/* If not the first in the chain, we can skip over it */
			/* [한국어] chain 의 첫 항목이 아니면 앞 항목의 next 를 고쳐 건너뛸 수 있다. */
			if (prev) {
				/* [한국어] 이 capability 의 next 오프셋을 뽑아 epos 와 val 에 동시에 대입한다.
				 * epos 갱신으로 다음 반복이 그 자리로 가고, val 은 아래에서 앞 항목의
				 * next 필드에 심을 값이 된다. */
				u32 val = epos = PCI_EXT_CAP_NEXT(header);
				/* [한국어] 앞 항목 헤더의 next 필드만 비운다. 확장 헤더에서 next 오프셋은 상위
				 * 12비트(비트 20~31)에 있고, 오프셋이 dword 정렬이라 하위 2비트가 늘 0 이다.
				 * 그래서 마스크 0xffc 를 20비트 왼쪽으로 밀면 정확히 그 필드만 덮는다.
				 * ID(비트 0~15)와 버전(비트 16~19)은 건드리지 않는다. */
				*prev &= cpu_to_le32(~(0xffcU << 20));
				/* [한국어] 건너뛸 오프셋을 그 자리에 넣는다. 이 두 줄이 확장 chain 재작성의
				 * 실체다 — 표준 chain 이 next 바이트 하나를 통째로 갈아끼우는 것과 달리,
				 * 여기서는 한 dword 안의 비트 필드만 골라 고친다. */
				*prev |= cpu_to_le32(val << 20);
				continue;
			}

			/*
			 * Otherwise, fill in a placeholder, the direct
			 * readfn will virtualize this automatically
			 */
			/* [한국어] chain 의 첫 항목이라 건너뛸 수 없다. 대신 헤더 크기만큼 자리를 지키는
			 * 껍데기를 남긴다. 상류 주석대로 direct readfn 이 이 자리를 자동으로
			 * 가상화해 준다. */
			len = PCI_CAP_SIZEOF;
			/* [한국어] 아래에서 ID 와 버전을 지워 "정체 없는 자리" 로 만들라는 표시. */
			hidden = true;
		}

		/* [한국어] 겹침 검사. */
		for (i = 0; i < len; i++) {
			/* [한국어] 아직 칠해지지 않은 자리가 정상이다. */
			if (likely(map[epos + i] == PCI_CAP_ID_INVALID))
				continue;

			/* [한국어] 겹치면 경고만 남기고 덮어쓴다. */
			pci_warn(pdev, "%s: PCI config conflict @%#x, was ecap %#x now ecap %#x\n",
				 __func__, epos + i, map[epos + i], ecap);
		}

		/*
		 * Even though ecap is 2 bytes, we're currently a long way
		 * from exceeding 1 byte capabilities.  If we ever make it
		 * up to 0xFE we'll need to up this to a two-byte, byte map.
		 */
		/* [한국어] 컴파일 시 검사. 상류 주석이 그 배경을 설명한다 — 확장 ID 는 2바이트인데
		 * 지도는 1바이트짜리라, 실제 ID 가 0xFE 근처까지 커지면 지도를 2바이트로
		 * 바꿔야 한다. 아직은 그럴 일이 없다. */
		BUILD_BUG_ON(PCI_EXT_CAP_ID_MAX >= PCI_CAP_ID_INVALID_VIRT);

		/* [한국어] 지도를 이 확장 capability ID 로 칠한다. 껍데기 자리도 실제 ID 로 칠하는데,
		 * 그 ID 가 표 범위를 넘으면 vfio_pci_config_rw_single 이 direct_ro_perms 로
		 * 갈아탄다. */
		memset(map + epos, ecap, len);
		/* [한국어] 그림자 복사본에 하드웨어 값을 채운다. */
		ret = vfio_fill_vconfig_bytes(vdev, epos, len);
		if (ret)
			return ret;

		/*
		 * If we're just using this capability to anchor the list,
		 * hide the real ID.  Only count real ecaps.  XXX PCI spec
		 * indicates to use cap id = 0, version = 0, next = 0 if
		 * ecaps are absent, hope users check all the way to next.
		 */
		/* [한국어] 껍데기 자리면 정체를 지운다. */
		if (hidden)
			/* [한국어] 헤더에서 next 필드만 남기고 ID 와 버전을 0 으로 만든다. 마스크가
			 * 0xffc 를 20비트 민 값이라 next 만 살아남는다. 상류 주석대로 이렇게 하면
			 * "진짜 ID 는 감추고 목록의 닻 역할만 하는 자리" 가 된다. */
			*(__le32 *)&vdev->vconfig[epos] &=
				cpu_to_le32((0xffcU << 20));
		else
			/* [한국어] 진짜로 사용자에게 보이는 확장 capability 만 센다. 껍데기는 세지 않는다. */
			ecaps++;

		/* [한국어] 다음 반복에서 감출 일이 생기면 고칠 자리를 이 항목의 첫 dword 로 옮긴다. */
		prev = (__le32 *)&vdev->vconfig[epos];
		/* [한국어] 하드웨어 헤더에서 다음 오프셋을 읽어 이동한다. vconfig 를 고쳤더라도
		 * 순회는 하드웨어의 진짜 chain 을 따라간다. */
		epos = PCI_EXT_CAP_NEXT(header);
	}

	/* [한국어] 진짜 확장 capability 가 하나도 없었다. */
	if (!ecaps)
		/* [한국어] 확장 영역의 첫 dword 를 통째로 0 으로 만든다. 상류 주석이 근거와 한계를
		 * 함께 밝힌다 — 스펙은 확장 capability 가 없을 때 ID/버전/next 를 모두 0 으로
		 * 두라고 하며, 사용자가 next 까지 확인해 주기를 기대한다. */
		*(u32 *)&vdev->vconfig[PCI_CFG_SPACE_SIZE] = 0;

	/* [한국어] 확장 지도 칠하기 완료. */
	return 0;
}

/*
 * Nag about hardware bugs, hopefully to have vendors fix them, but at least
 * to collect a list of dependencies for the VF INTx pin quirk below.
 */
/* [한국어] SR-IOV 스펙을 어기고 VF 에 0 이 아닌 Interrupt Pin 을 보고하는 것으로
 * 이미 알려진 하드웨어 목록.
 * 설정자: 컴파일 시 상수.
 * 읽는 자: vfio_config_init 이 pci_match_id(drivers/pci/pci-driver.c:347)로
 * 대조해, 목록에 없는 새 위반이면 경고를 남긴다.
 * 값 범위: 벤더/디바이스 ID 쌍의 배열이며 빈 항목으로 끝난다.
 * 동기화: 읽기 전용.
 * 상류 주석이 목적을 밝힌다 — 벤더가 고치도록 시끄럽게 알리되, 이미 아는
 * 것에 대해서는 조용히 넘어가 로그를 어지럽히지 않는다. */
static const struct pci_device_id known_bogus_vf_intx_pin[] = {
	/* [한국어] Intel 의 특정 디바이스 하나가 이 버그를 갖고 있다. 이 항목이 있으면
	 * 그 디바이스에 대해서는 경고를 내지 않는다. */
	{ PCI_DEVICE(PCI_VENDOR_ID_INTEL, 0x270c) },
	/* [한국어] 배열의 끝 표시. pci_match_id 는 벤더 ID 가 0 인 항목을 만나면 순회를
	 * 멈춘다. */
	{}
};

/* [한국어]
 * vfio_config_init - 이 디바이스의 config 지도와 그림자 복사본을 만들고 초기값을 채운다
 *
 * @vdev: 대상 디바이스. 성공하면 pci_config_map 과 vconfig 가 채워진다.
 * @return: 0 성공. 실패면 errno(음수). 호출자는 디바이스 열기를 중단한다.
 *
 * 왜 필요한가: 이 파일의 나머지 전부가 이 두 버퍼 위에서 돈다. 지도가
 * "어느 표를 쓸 것인가" 를, vconfig 가 "가상화된 비트의 현재 값" 을 담는다.
 * 상류 주석이 설계 근거를 밝힌다 — 권한 표를 디바이스마다 두면 메모리가
 * 낭비되고, vconfig 없이 필요한 영역마다 작은 버퍼를 두면 그 포인터 배열이
 * 결국 비슷한 크기가 된다.
 *
 * 동작 과정:
 *  1. 지도와 vconfig 를 각각 cfg_size 바이트로 할당한다. 상류 주석이
 *     "capability 는 dword 정렬이지만 길이에는 제약이 없어 capability 사이
 *     빈틈은 바이트 단위여야 한다" 며 바이트 지도를 쓰는 이유를 밝힌다.
 *     GFP_KERNEL_ACCOUNT 라 이 메모리가 요청한 프로세스의 memcg 에 달린다.
 *  2. 지도의 앞 64바이트를 표준 헤더 표시로, 나머지를 PCI_CAP_ID_INVALID 로
 *     칠한다. 즉 초기 상태에서 헤더 밖은 전부 "capability 없음 = raw 접근" 이다.
 *  3. 표준 헤더 64바이트를 하드웨어에서 vconfig 로 복사한다.
 *  4. bardirty 를 켜 첫 BAR 읽기에서 사이징 계산이 돌게 한다.
 *  5. BAR 6개와 ROM BAR 의 초기값을 rbar 에 기억한다. 뒷문 리셋 감지와
 *     복구의 기준값이다. 상류 주석의 XXX 는 이 수동 저장 대신 커널의 상태
 *     저장/복원 API 를 쓸 수 있지 않겠냐는 미해결 질문이다.
 *  6. VF 라면:
 *     - Vendor/Device ID 를 커널이 아는 값으로 채운다. VF 의 config 공간은
 *       이 두 필드를 전부 1 로 읽히게 하는 경우가 있어(표에서 이 두 필드를
 *       가상화한 이유가 이것이다) 사용자가 디바이스를 식별할 수 없다.
 *     - Interrupt Pin 을 0 으로 만든다. SR-IOV 스펙 1.1 의 3.4.1.18 이
 *       VF 에는 이 레지스터가 적용되지 않으며 값 0 의 읽기 전용이어야 한다고
 *       정한다. 상류 주석이 그 조항을 직접 인용하며, 하드웨어가 그것을
 *       어겨도 사용자가 알아챌 방법이 없으니 커널이 대신 바로잡는다고 밝힌다.
 *       다만 이미 알려진 하드웨어 버그 목록에 없는 새 위반이면 경고를 남겨
 *       벤더가 고치도록 유도한다.
 *  7. Memory Space Enable 비트가 없는 디바이스는 vconfig 쪽 비트를 켜 둔다.
 *     상류 주석대로 PF 와 일관되게 보이도록 하기 위해서다.
 *  8. INTx 를 쓸 수 없는 상황이면(설정으로 꺼졌거나, 고장 나 막았거나,
 *     IRQ 가 배정되지 않았거나, 연결되지 않았거나) Interrupt Pin 을 0 으로
 *     만들어 사용자에게 INTx 가 없는 것처럼 보이게 한다. 이 값은
 *     vfio_pci_intrs.c 가 INTx 벡터 개수를 셀 때도 기준이 된다.
 *  9. 표준 capability chain 과 확장 capability chain 을 순회해 지도를
 *     완성한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 첫 열기 경로. 잠들 수 있다.
 * 아직 사용자에게 노출되기 전이라 락이 없다.
 *
 * 에러 경로: out 라벨에서 지도와 vconfig 를 해제하고 두 포인터를 NULL 로
 * 비운 뒤 오류를 errno 로 바꿔 반환한다.
 *
 * [상류 코드 관찰] out 경로는 vdev->msi_perm 을 해제하지 않는다. 9단계의
 * vfio_cap_init 이 MSI capability 를 만나면 vfio_msi_cap_len 이
 * vdev->msi_perm 을 할당하는데, 그 뒤 같은 chain 의 다른 capability 에서
 * 오류가 나면 이 경로로 빠진다. msi_perm 을 해제하는 곳은 vfio_config_free
 * 하나뿐이고(이 파일), 그것은 vfio_pci_core.c 의 vfio_pci_core_disable 에서만
 * 불린다. 그런데 vfio_config_init 이 실패하면 vfio_pci_core_enable 은
 * vfio_pci_core_disable 을 거치지 않고 자기 실패 경로로 되돌아가며,
 * vfio_pci_core_release_dev 도 msi_perm 을 보지 않는다. 그래서 이 경로로
 * 빠진 msi_perm 은 다음번 성공적인 열기/닫기 한 쌍이 있을 때까지 남는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_pci_core_enable → [vfio_config_init]
 *     → kmalloc, vfio_fill_vconfig_bytes, pci_match_id
 *       (drivers/pci/pci-driver.c:347), vfio_cap_init, vfio_ecap_init
 */
/*
 * For each device we allocate a pci_config_map that indicates the
 * capability occupying each dword and thus the struct perm_bits we
 * use for read and write.  We also allocate a virtualized config
 * space which tracks reads and writes to bits that we emulate for
 * the user.  Initial values filled from device.
 *
 * Using shared struct perm_bits between all vfio-pci devices saves
 * us from allocating cfg_size buffers for virt and write for every
 * device.  We could remove vconfig and allocate individual buffers
 * for each area requiring emulated bits, but the array of pointers
 * would be comparable in size (at least for standard config space).
 */
int vfio_config_init(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 초기값을 읽어 올 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 지도와 그림자 복사본을 담을 지역 포인터. */
	u8 *map, *vconfig;
	/* [한국어] 하부 호출의 결과 코드. */
	int ret;

	/*
	 * Config space, caps and ecaps are all dword aligned, so we could
	 * use one byte per dword to record the type.  However, there are
	 * no requirements on the length of a capability, so the gap between
	 * capabilities needs byte granularity.
	 */
	/* [한국어] config 공간과 같은 크기의 지도를 만든다. 상류 주석이 바이트 단위인
	 * 이유를 밝힌다 — capability 자체는 dword 정렬이라 dword 당 1바이트면
	 * 충분할 것 같지만, capability 의 길이에는 제약이 없어 capability 사이
	 * 빈틈이 바이트 단위로 생긴다. ACCOUNT 는 이 메모리를 요청 프로세스의
	 * memcg 에 단다. */
	map = kmalloc(pdev->cfg_size, GFP_KERNEL_ACCOUNT);
	if (!map)
		return -ENOMEM;

	/* [한국어] 같은 크기의 그림자 복사본. 아래에서 필요한 영역만 하드웨어 값으로
	 * 채우고, 나머지는 초기화되지 않은 채 남지만 지도가 칠해지지 않은 자리는
	 * raw 접근이라 vconfig 를 읽지 않는다. */
	vconfig = kmalloc(pdev->cfg_size, GFP_KERNEL_ACCOUNT);
	if (!vconfig) {
		/* [한국어] vconfig 할당 실패. 앞서 만든 지도를 되돌린다. 아직 vdev 에 붙이기
		 * 전이라 포인터 정리가 필요 없다. */
		kfree(map);
		return -ENOMEM;
	}

	/* [한국어] 디바이스에 지도를 건다. 이 시점부터 실패 경로는 out 라벨을 거쳐야 한다. */
	vdev->pci_config_map = map;
	/* [한국어] 그림자 복사본도 건다. */
	vdev->vconfig = vconfig;

	/* [한국어] 앞 64바이트를 표준 헤더용 가짜 ID 로 칠한다. 이제 그 영역의 접근은
	 * cap_perms 의 0번 슬롯을 쓴다. */
	memset(map, PCI_CAP_ID_BASIC, PCI_STD_HEADER_SIZEOF);
	/* [한국어] 나머지는 전부 "capability 없음" 으로 칠한다. 즉 초기 상태에서 헤더 밖은
	 * 모두 raw 접근이며, capability 를 발견할 때마다 그 자리만 덮어써 나간다. */
	memset(map + PCI_STD_HEADER_SIZEOF, PCI_CAP_ID_INVALID,
	       pdev->cfg_size - PCI_STD_HEADER_SIZEOF);

	/* [한국어] 표준 헤더 64바이트를 하드웨어에서 그대로 복사한다. 가상화된 필드
	 * (BAR, COMMAND 등)의 초기값이 진짜 값이어야 한다. */
	ret = vfio_fill_vconfig_bytes(vdev, 0, PCI_STD_HEADER_SIZEOF);
	if (ret)
		goto out;

	/* [한국어] 첫 BAR 읽기에서 vfio_bar_fixup 이 반드시 돌게 한다. 지금 vconfig 의
	 * BAR 값은 하드웨어의 진짜 주소라, 사용자에게 보이기 전에 크기 마스크를
	 * 씌워야 한다. */
	vdev->bardirty = true;

	/*
	 * XXX can we just pci_load_saved_state/pci_restore_state?
	 * may need to rebuild vconfig after that
	 */

	/* For restore after reset */
	/* [한국어] BAR0 의 원본 값을 기억한다. 뒷문 리셋 감지(vfio_need_bar_restore)와
	 * 복구(vfio_bar_restore)의 기준값이다. 바로 위 상류 주석의 XXX 는 이 수동
	 * 저장 대신 커널의 상태 저장/복원 API 를 쓸 수 있지 않겠느냐는 미해결
	 * 질문이며, 그러려면 그 뒤 vconfig 를 다시 만들어야 한다는 단서도 달려 있다. */
	vdev->rbar[0] = le32_to_cpu(*(__le32 *)&vconfig[PCI_BASE_ADDRESS_0]);
	/* [한국어] BAR1 의 원본 값. */
	vdev->rbar[1] = le32_to_cpu(*(__le32 *)&vconfig[PCI_BASE_ADDRESS_1]);
	/* [한국어] BAR2 의 원본 값. */
	vdev->rbar[2] = le32_to_cpu(*(__le32 *)&vconfig[PCI_BASE_ADDRESS_2]);
	/* [한국어] BAR3 의 원본 값. */
	vdev->rbar[3] = le32_to_cpu(*(__le32 *)&vconfig[PCI_BASE_ADDRESS_3]);
	/* [한국어] BAR4 의 원본 값. */
	vdev->rbar[4] = le32_to_cpu(*(__le32 *)&vconfig[PCI_BASE_ADDRESS_4]);
	/* [한국어] BAR5 의 원본 값. */
	vdev->rbar[5] = le32_to_cpu(*(__le32 *)&vconfig[PCI_BASE_ADDRESS_5]);
	/* [한국어] 일곱 번째 칸은 ROM BAR 다. vfio_bar_restore 가 루프를 빠져나온 뒤의
	 * 포인터로 이 칸에 닿는다. */
	vdev->rbar[6] = le32_to_cpu(*(__le32 *)&vconfig[PCI_ROM_ADDRESS]);

	/* [한국어] SR-IOV VF 전용 보정 두 가지. */
	if (pdev->is_virtfn) {
		/* [한국어] VF 의 config 공간은 Vendor ID 를 전부 1 로 읽히게 하는 경우가 있다.
		 * 커널이 아는 진짜 값을 그림자 복사본에 넣는다. 표에서 이 필드를 가상화해
		 * 둔 덕분에 이 값이 사용자에게 그대로 보인다. */
		*(__le16 *)&vconfig[PCI_VENDOR_ID] = cpu_to_le16(pdev->vendor);
		/* [한국어] Device ID 도 같은 이유로 커널이 아는 값으로 채운다. */
		*(__le16 *)&vconfig[PCI_DEVICE_ID] = cpu_to_le16(pdev->device);

		/*
		 * Per SR-IOV spec rev 1.1, 3.4.1.18 the interrupt pin register
		 * does not apply to VFs and VFs must implement this register
		 * as read-only with value zero.  Userspace is not readily able
		 * to identify whether a device is a VF and thus that the pin
		 * definition on the device is bogus should it violate this
		 * requirement.  We already virtualize the pin register for
		 * other purposes, so we simply need to replace the bogus value
		 * and consider VFs when we determine INTx IRQ count.
		 */
		/* [한국어] VF 인데 Interrupt Pin 이 0 이 아니다. 상류 주석이 인용하는 SR-IOV 스펙
		 * 1.1 의 3.4.1.18 은 이 레지스터가 VF 에 적용되지 않으며 값 0 의 읽기
		 * 전용이어야 한다고 정한다. 이미 알려진 위반 목록에 없으면 새로운 하드웨어
		 * 버그이므로 알린다. */
		if (vconfig[PCI_INTERRUPT_PIN] &&
		    !pci_match_id(known_bogus_vf_intx_pin, pdev))
			/* [한국어] 벤더가 고치도록 경고를 남긴다. 상류 주석대로 사용자 공간은 이 디바이스가
			 * VF 인지 쉽게 알 수 없어 스스로 판단할 수 없으니 커널이 대신 알려야 한다. */
			pci_warn(pdev,
				 "Hardware bug: VF reports bogus INTx pin %d\n",
				 vconfig[PCI_INTERRUPT_PIN]);

		/* [한국어] 어느 쪽이든 0 으로 만든다. 스펙을 지킨 VF 라면 이미 0 이라 이 대입이
		 * 불필요하지만(상류 주석의 "Gratuitous"), 어긴 VF 를 바로잡으려면 무조건
		 * 써야 한다. 이 값은 vfio_pci_intrs.c 가 INTx 벡터 개수를 셀 때의 기준도 된다. */
		vconfig[PCI_INTERRUPT_PIN] = 0; /* Gratuitous for good VFs */
	}
	/* [한국어] COMMAND 에 Memory Space Enable 비트가 물리적으로 없는 디바이스. */
	if (pdev->no_command_memory) {
		/*
		 * VFs and devices that set pdev->no_command_memory do not
		 * implement the memory enable bit of the COMMAND register
		 * therefore we'll not have it set in our initial copy of
		 * config space after pci_enable_device().  For consistency
		 * with PFs, set the virtual enable bit here.
		 */
		/* [한국어] 상류 주석대로 PF 와 일관되게 보이도록 가상 활성 비트를 켜 둔다.
		 * pci_enable_device 뒤에도 하드웨어에는 이 비트가 서지 않으므로 복사본에
		 * 직접 넣어야 한다. 이 값을 __vfio_pci_memory_enabled 와
		 * vfio_basic_config_read 가 함께 읽는다. */
		*(__le16 *)&vconfig[PCI_COMMAND] |=
					cpu_to_le16(PCI_COMMAND_MEMORY);
	}

	/* [한국어] INTx 를 쓸 수 없는 네 가지 상황을 한꺼번에 본다 — 커널 빌드에서
	 * INTx 지원이 빠졌거나, 이 디바이스의 INTx 가 고장 나 커널이 막았거나,
	 * IRQ 번호가 배정되지 않았거나, 연결되지 않은 IRQ 로 표시된 경우다. */
	if (!IS_ENABLED(CONFIG_VFIO_PCI_INTX) || vdev->nointx ||
	    !vdev->pdev->irq || vdev->pdev->irq == IRQ_NOTCONNECTED)
		/* [한국어] Interrupt Pin 을 0 으로 만들어 사용자에게 "INTx 핀이 없는 디바이스" 로
		 * 보이게 한다. 이 필드가 표에서 가상화 대상인 주된 이유가 바로 이것이다. */
		vconfig[PCI_INTERRUPT_PIN] = 0;

	/* [한국어] 표준 capability chain 을 순회해 지도를 칠하고 chain 을 재작성한다. */
	ret = vfio_cap_init(vdev);
	if (ret)
		goto out;

	/* [한국어] 확장 capability chain 에 대해 같은 일을 한다. extended_caps 플래그는
	 * 바로 위 vfio_cap_init 안에서 세워진다. */
	ret = vfio_ecap_init(vdev);
	if (ret)
		goto out;

	/* [한국어] 지도와 그림자 복사본 준비 완료. */
	return 0;

out:
	/* [한국어] 실패 경로. 지도를 해제한다. 이 라벨에 도달하는 경로는 표준 헤더 채우기
	 * 실패, capability 초기화 실패, 확장 capability 초기화 실패 세 가지다. */
	kfree(map);
	/* [한국어] 포인터를 비워 해제 후 사용을 막는다. */
	vdev->pci_config_map = NULL;
	/* [한국어] 그림자 복사본도 해제한다. */
	kfree(vconfig);
	/* [한국어] 포인터를 비운다. */
	vdev->vconfig = NULL;
	/* [한국어] PCIBIOS 계열 양수 오류를 커널 errno 로 바꾼다. 이미 음수 errno 인
	 * 값(예: vfio_msi_cap_len 이 낸 -ENOMEM)은 그대로 통과한다. */
	return pcibios_err_to_errno(ret);
}

/* [한국어]
 * vfio_config_free - 디바이스의 config 지도, 그림자 복사본, MSI 권한 표를 해제한다
 *
 * @vdev: 대상 디바이스.
 * @return: 없다.
 *
 * 왜 필요한가: vfio_config_init 과 vfio_msi_cap_len 이 만든 디바이스별
 * 자원을 되돌린다. 포인터를 전부 NULL 로 비우므로 두 번 불려도 안전하다 —
 * 디바이스를 닫았다 다시 여는 경로에서 실제로 그럴 수 있다.
 *
 * 동작 과정: vconfig, 지도, 그리고 msi_perm 이 있으면 그 안의 두 비트맵과
 * 구조체 자체까지 해제하고 모든 포인터를 NULL 로 만든다. 공유 표
 * (cap_perms/ecap_perms)는 건드리지 않는다 — 다른 디바이스가 쓰고 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스 닫기 경로.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_core_disable → [vfio_config_free] → kfree, free_perm_bits
 */
void vfio_config_free(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 그림자 복사본 해제. */
	kfree(vdev->vconfig);
	/* [한국어] 포인터를 비운다. 디바이스를 닫았다 다시 여는 경로가 있어 이 정리가
	 * 중요하다. */
	vdev->vconfig = NULL;
	/* [한국어] 지도 해제. */
	kfree(vdev->pci_config_map);
	/* [한국어] 포인터를 비운다. */
	vdev->pci_config_map = NULL;
	/* [한국어] MSI capability 를 가진 디바이스만 이 표를 갖는다. */
	if (vdev->msi_perm) {
		/* [한국어] 표 안의 virt/write 두 비트맵을 해제한다. */
		free_perm_bits(vdev->msi_perm);
		/* [한국어] 표 구조체 자체를 해제한다. 공유 표(cap_perms/ecap_perms)는 건드리지
		 * 않는다 — 다른 디바이스가 쓰고 있다. */
		kfree(vdev->msi_perm);
		/* [한국어] 포인터를 비운다. 이 줄이 vfio_msi_cap_len 의 "이미 있으면 재사용"
		 * 검사와 짝을 이뤄, 다음 열기에서 새 표를 만들게 한다. */
		vdev->msi_perm = NULL;
	}
}

/* [한국어]
 * vfio_pci_cap_remaining_dword - 이 위치부터 같은 capability 가 이어지는 바이트 수를 dword 경계까지 센다
 *
 * @vdev: 대상 디바이스. pci_config_map 을 읽는다.
 * @pos: 세기 시작할 절대 오프셋.
 * @return: 1 이상의 바이트 수. pos 자신을 포함한다.
 *
 * 왜 필요한가: 접근 하나가 두 capability 에 걸치면 어느 표를 써야 할지
 * 정할 수 없다. 또 dword 경계를 넘는 접근은 PCI config 접근자로 한 번에
 * 할 수 없다. 그래서 사용자의 요청을 "한 capability 안에 있고 dword 경계를
 * 넘지 않는" 조각으로 자르는데, 그 조각의 최대 길이를 이 함수가 알려 준다.
 *
 * 동작 과정: pos 의 capability ID 를 기준으로 삼고, 다음 바이트부터
 * (a) dword 경계에 닿거나 (b) capability ID 가 달라질 때까지 센다. 두
 * 조건이 루프의 조건식에 함께 들어 있어 먼저 걸리는 쪽에서 멈춘다.
 * 루프 본문이 비어 있는 것은 의도된 것으로, 상류 코드가 nop 주석으로
 * 밝혀 두었다.
 *
 * 경계 안전: pos 는 항상 cfg_size 미만이고, dword 경계 조건 때문에
 * pos + i 가 다음 dword 시작을 넘지 못한다. cfg_size 가 256 또는 4096 으로
 * dword 정렬이라 지도 밖을 읽지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 배열 훑기뿐이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw_single → [vfio_pci_cap_remaining_dword]
 */
/*
 * Find the remaining number of bytes in a dword that match the given
 * position.  Stop at either the end of the capability or the dword boundary.
 */
static size_t vfio_pci_cap_remaining_dword(struct vfio_pci_core_device *vdev,
					   loff_t pos)
{
	/* [한국어] 기준이 될 capability ID. 이 ID 가 이어지는 동안만 같은 조각이다. */
	u8 cap = vdev->pci_config_map[pos];
	/* [한국어] 센 바이트 수. pos 자신을 포함하므로 1 에서 시작한다. */
	size_t i;

	/* [한국어] 두 조건이 함께 걸려 있다. (a) dword 경계에 닿으면 멈춘다 — PCI config
	 * 접근자가 dword 를 넘는 접근을 한 번에 못 한다. (b) capability ID 가
	 * 달라지면 멈춘다 — 그 자리부터는 다른 권한 표를 써야 한다.
	 * 경계 안전: pos 가 cfg_size 미만이고 dword 조건 때문에 pos + i 가 다음
	 * dword 시작을 넘지 못하며, cfg_size 는 256 이나 4096 으로 dword 정렬이라
	 * 지도 밖을 읽지 않는다. */
	for (i = 1; (pos + i) % 4 && vdev->pci_config_map[pos + i] == cap; i++)
		/* nop */;

	/* [한국어] 이 위치에서 안전하게 처리할 수 있는 최대 바이트 수. 호출자가 여기서
	 * 다시 정렬에 맞는 폭으로 줄인다. */
	return i;
}

/* [한국어]
 * vfio_pci_config_rw_single - config 접근 한 조각을 처리하는 디스패처. 지도에서 표를 골라 콜백을 부른다
 *
 * @vdev: 대상 디바이스.
 * @buf: 사용자 공간 버퍼. 읽기면 여기로 쓰고, 쓰기면 여기서 읽는다.
 * @count: 사용자가 요청한 남은 바이트 수. 이 함수가 더 작게 자를 수 있다.
 * @ppos: 접근 시작 오프셋. region 비트가 이미 제거된 config 공간 안의
 *        오프셋이어야 한다. 이 함수는 값을 전진시키지 않는다 — 상류 주석대로
 *        호출자가 반환값만큼 더한다.
 * @iswrite: true 면 쓰기, false 면 읽기.
 * @return: 실제로 처리한 바이트 수(1/2/4), 또는 음수 errno.
 *
 * 왜 필요한가: 이 파일의 모든 판정이 여기서 한자리에 모인다. 접근을 안전한
 * 조각으로 자르고, 지도에서 표를 고르고, capability 안에서의 오프셋을
 * 계산하고, 사용자 버퍼와의 복사를 하고, 콜백을 부른다. 이 파일 밖에서도
 * 쓸 수 있게 EXPORT_SYMBOL_GPL 로 나가며, s390 의 ism 드라이버가
 * MIO 명령을 피하려고 이 함수를 직접 부른다(drivers/vfio/pci/ism/main.c:205).
 *
 * 동작 과정:
 *  1. 범위 검사. 음수이거나 config 공간 밖이거나 끝이 넘치면 -EFAULT.
 *     **이 검사가 지도와 vconfig 배열 접근의 유일한 경계 보장이다.**
 *  2. 접근을 자른다. 먼저 vfio_pci_cap_remaining_dword 로 "같은 capability
 *     안에서 dword 경계까지" 로 제한하고, 그 안에서 정렬에 맞는 가장 큰
 *     폭(4 → 2 → 1)을 고른다. 상류 주석이 "정렬된 조각으로, capability 를
 *     넘지 않게" 라는 의도를 밝힌다.
 *  3. 지도에서 이 위치의 capability ID 를 꺼내 표를 고른다.
 *     - PCI_CAP_ID_INVALID: capability 가 없는 빈틈. raw 통과 표를 쓰고
 *       capability 시작점을 자기 자신으로 둔다(오프셋 계산이 무의미하므로).
 *     - PCI_CAP_ID_INVALID_VIRT: vfio_ecap_init 이 남긴 껍데기 자리.
 *       전면 가상 표를 쓴다.
 *     - 확장 영역이면 ecap_perms 에서 고른다. 다만 상류 주석이 밝히듯,
 *       확장 chain 첫머리의 정체 모를 capability 를 감출 때 지도에 표 범위를
 *       넘는 ID 가 들어갈 수 있어 그 경우 읽기 전용 direct 표로 대체한다.
 *     - 표준 영역이면 cap_perms 에서 고르되 MSI 만은 디바이스 전용 표로
 *       바꾼다. 표준 헤더가 아니면 capability 시작점을 되짚는다.
 *  4. 두 개의 정합성 검사를 한다. 표준 헤더가 아닌데 시작점이 0 이면
 *     지도가 잘못 칠해진 것이고, 시작점이 접근 위치보다 뒤면 되짚기가
 *     잘못된 것이다.
 *  5. capability 안에서의 오프셋을 구한다. 이 값이 virt/write 비트맵의
 *     인덱스가 된다.
 *  6. 쓰기면 writefn 이 없을 때 조용히 성공으로 처리하고(읽기 전용 표),
 *     있으면 사용자 버퍼를 커널로 복사한 뒤 부른다.
 *     읽기면 readfn 이 있을 때만 부르고, 결과를 사용자 버퍼로 복사한다.
 *     readfn 이 없으면 val 의 초기값 0 이 그대로 사용자에게 간다.
 *
 * [상류 코드 관찰] 3단계의 표준 영역 갈래는 cap_id 가 표 범위를 넘을 때
 * 경고만 남기고 그대로 cap_perms 를 색인한다. 바로 위의 확장 영역 갈래는
 * 같은 상황에서 direct_ro_perms 로 갈아타 배열 밖 접근을 피한다. 표준 쪽에서
 * 그런 ID 가 지도에 들어가는 경로는 vfio_cap_init 이 막고 있어(범위를 넘는
 * ID 는 감춰 버린다) 실제로는 도달하지 않는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 사용자 메모리를 만지고 하부 콜백이 잠들 수
 * 있다. 이 함수 자체는 락을 잡지 않는다 — 필요한 락은 각 writefn 이 잡는다.
 *
 * 에러 경로: 범위 밖이면 -EFAULT, 사용자 복사 실패도 -EFAULT, 콜백 오류는
 * 그대로 전달. 읽기에서 콜백이 실패하면 사용자 버퍼를 건드리지 않고 반환한다.
 *
 * 호출 체인:
 *   vfio_pci_config_rw (이 파일) 또는 ism_vfio_pci_config_rw
 *     → [vfio_pci_config_rw_single]
 *     → vfio_pci_cap_remaining_dword, vfio_find_cap_start,
 *       copy_from_user / copy_to_user, perm->writefn / perm->readfn
 */
ssize_t vfio_pci_config_rw_single(struct vfio_pci_core_device *vdev,
				  char __user *buf, size_t count, loff_t *ppos,
				  bool iswrite)
{
	/* [한국어] 경계 검사에 쓸 cfg_size 의 출처. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 이 접근을 관장할 권한 표. 아래에서 고른다. */
	struct perm_bits *perm;
	/* [한국어] 읽기면 결과를, 쓰기면 사용자 값을 담는 4바이트 그릇. 0 으로 시작해,
	 * readfn 이 없는 표에서 읽을 때 사용자에게 0 이 가게 한다. */
	__le32 val = 0;
	/* [한국어] 이 capability 의 시작 절대 오프셋과, 그로부터의 상대 오프셋. */
	int cap_start = 0, offset;
	/* [한국어] 지도에서 꺼낸 capability ID. */
	u8 cap_id;
	/* [한국어] 반환할 바이트 수 또는 오류. */
	ssize_t ret;

	/* [한국어] **이 검사가 지도와 그림자 복사본 배열 접근의 유일한 경계 보장이다.**
	 * 음수 오프셋, config 공간 밖 시작, 끝이 넘치는 요청을 모두 막는다. 이
	 * 세 가지 중 하나라도 뚫리면 커널 힙을 읽거나 쓰게 된다. */
	if (*ppos < 0 || *ppos >= pdev->cfg_size ||
	    *ppos + count > pdev->cfg_size)
		return -EFAULT;

	/*
	 * Chop accesses into aligned chunks containing no more than a
	 * single capability.  Caller increments to the next chunk.
	 */
	/* [한국어] 상류 주석이 밝히는 조각 자르기 1단계. 같은 capability 안에서 dword
	 * 경계까지로 제한한다. */
	count = min(count, vfio_pci_cap_remaining_dword(vdev, *ppos));
	/* [한국어] 조각 자르기 2단계. 4바이트 정렬이고 4바이트 이상 남았으면 dword 접근. */
	if (count >= 4 && !(*ppos % 4))
		/* [한국어] dword 폭. */
		count = 4;
	/* [한국어] 다음으로 2바이트 정렬을 본다. */
	else if (count >= 2 && !(*ppos % 2))
		/* [한국어] word 폭. */
		count = 2;
	else
		/* [한국어] 그 밖에는 바이트 단위. 정렬되지 않은 오프셋이나 남은 크기가 작을 때다. */
		count = 1;

	/* [한국어] 기본 반환값을 정해 둔다. writefn 이 없는 표에 쓰는 경우 이 값이 그대로
	 * 나가 "성공했다" 고 보고한다. */
	ret = count;

	/* [한국어] 지도에서 이 위치의 소속을 꺼낸다. 아래 분기 전체가 이 값으로 갈린다. */
	cap_id = vdev->pci_config_map[*ppos];

	/* [한국어] 어떤 capability 에도 속하지 않는 빈틈이다. */
	if (cap_id == PCI_CAP_ID_INVALID) {
		/* [한국어] raw 통과 표를 쓴다. 읽기도 쓰기도 하드웨어로 그대로 간다. */
		perm = &unassigned_perms;
		/* [한국어] 시작점을 자기 자신으로 둔다. 비트맵이 없어 상대 오프셋이 의미가 없고,
		 * 아래 정합성 검사(cap_start 가 0 이 아니어야 한다)를 통과시키기 위한 값이다. */
		cap_start = *ppos;
	/* [한국어] vfio_ecap_init 이 남긴 껍데기 자리 표시다. */
	} else if (cap_id == PCI_CAP_ID_INVALID_VIRT) {
		/* [한국어] 전면 가상 표. 읽기도 쓰기도 그림자 복사본에서만 이뤄진다. */
		perm = &virt_perms;
		/* [한국어] 마찬가지로 시작점을 자기 자신으로. */
		cap_start = *ppos;
	} else {
		/* [한국어] 확장 config 영역이면 ecap_perms 를 본다. */
		if (*ppos >= PCI_CFG_SPACE_SIZE) {
			/*
			 * We can get a cap_id that exceeds PCI_EXT_CAP_ID_MAX
			 * if we're hiding an unknown capability at the start
			 * of the extended capability list.  Use default, ro
			 * access, which will virtualize the id and next values.
			 */
			/* [한국어] 상류 주석이 이 경우를 설명한다 — 확장 chain 첫머리의 모르는
			 * capability 를 감출 때 지도에 표 범위를 넘는 ID 가 남을 수 있다. */
			if (cap_id > PCI_EXT_CAP_ID_MAX)
				/* [한국어] 읽기 전용 direct 표로 갈아탄다. 배열 밖 접근을 피하면서, ID 와 next 를
				 * 가상화해 감춘 상태를 유지한다. */
				perm = &direct_ro_perms;
			else
				/* [한국어] 정상 범위면 확장 capability 표에서 고른다. */
				perm = &ecap_perms[cap_id];

			/* [한국어] 지도를 되짚어 이 확장 capability 의 시작점을 찾는다. */
			cap_start = vfio_find_cap_start(vdev, *ppos);
		} else {
			/* [한국어] [상류 코드 관찰] 표 범위를 넘는 표준 capability ID 를 경고만 하고
			 * 그대로 다음 줄에서 배열을 색인한다. 바로 위의 확장 영역 갈래는 같은
			 * 상황에서 direct_ro_perms 로 갈아타 배열 밖 접근을 피하는 것과 대비된다.
			 * 실제로는 vfio_cap_init 이 범위를 넘는 ID 를 감춰 버려 지도에 남지 않으므로
			 * 이 경로에 도달하지 않는다.
			 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
			WARN_ON(cap_id > PCI_CAP_ID_MAX);

			/* [한국어] 표준 capability 표에서 고른다. 표준 헤더(가짜 ID 0)도 이 배열의 0번
			 * 슬롯이라 같은 방식으로 처리된다. */
			perm = &cap_perms[cap_id];

			/* [한국어] MSI 만은 디바이스마다 표가 다르다. */
			if (cap_id == PCI_CAP_ID_MSI)
				/* [한국어] 이 디바이스 전용 표로 바꾼다. vfio_msi_cap_len 이 만들어 둔 것이다. */
				perm = vdev->msi_perm;

			/* [한국어] 표준 헤더가 아니면 capability 시작점을 되짚어야 상대 오프셋을 구할 수
			 * 있다. 표준 헤더는 시작점이 0 이라 초기값 그대로 두면 된다. */
			if (cap_id > PCI_CAP_ID_BASIC)
				/* [한국어] 지도를 되짚어 시작점을 찾는다. */
				cap_start = vfio_find_cap_start(vdev, *ppos);
		}
	}

	/* [한국어] 정합성 검사 1. 표준 헤더가 아닌데 시작점이 0 이면 지도가 잘못 칠해졌거나
	 * 되짚기가 실패한 것이다. */
	WARN_ON(!cap_start && cap_id != PCI_CAP_ID_BASIC);
	/* [한국어] 정합성 검사 2. 시작점이 접근 위치보다 뒤에 있을 수 없다. 이 검사가
	 * 아래 뺄셈이 음수가 되는 것을 잡아낸다. */
	WARN_ON(cap_start > *ppos);

	/* [한국어] capability 안에서의 상대 오프셋. 이 값이 virt/write 비트맵의 인덱스가
	 * 되고, 각 콜백이 "이 capability 안 어느 레지스터인가" 를 판별하는 기준이 된다. */
	offset = *ppos - cap_start;

	/* [한국어] 쓰기 경로. */
	if (iswrite) {
		/* [한국어] 쓰기 콜백이 없는 표는 읽기 전용이다. 오류를 내지 않고 미리 정해 둔
		 * count 를 반환해 조용히 성공으로 보고한다 — 게스트 드라이버가 읽기 전용
		 * 레지스터에 썼다고 죽지 않게 하려는 의도적 선택이다. */
		if (!perm->writefn)
			return ret;

		/* [한국어] 사용자 버퍼에서 값을 가져온다. 잘못된 포인터면 -EFAULT. */
		if (copy_from_user(&val, buf, count))
			return -EFAULT;

		/* [한국어] 고른 표의 쓰기 콜백을 부른다. 이 한 줄이 3분류 집행의 진입점이다. */
		ret = perm->writefn(vdev, *ppos, count, perm, offset, val);
	} else {
		/* [한국어] 읽기 콜백이 있으면 부른다. 없으면 val 의 초기값 0 이 그대로 나간다. */
		if (perm->readfn) {
			/* [한국어] 고른 표의 읽기 콜백. 하드웨어와 vconfig 를 섞는 일이 그 안에서 벌어진다. */
			ret = perm->readfn(vdev, *ppos, count,
					   perm, offset, &val);
			if (ret < 0)
				return ret;
		}

		/* [한국어] 결과를 사용자 버퍼로 복사한다. */
		if (copy_to_user(buf, &val, count))
			return -EFAULT;
	}

	/* [한국어] 처리한 바이트 수. 호출자가 이만큼 오프셋과 버퍼를 전진시킨다. */
	return ret;
}
EXPORT_SYMBOL_GPL(vfio_pci_config_rw_single);

/* [한국어]
 * vfio_pci_config_rw - 사용자의 config 읽기/쓰기 요청 전체를 조각으로 나눠 반복 처리한다
 *
 * @vdev: 대상 디바이스.
 * @buf: 사용자 공간 버퍼.
 * @count: 요청한 바이트 수.
 * @ppos: VFIO region 오프셋. 상위 비트에 region 번호가 실려 있다.
 * @iswrite: true 면 쓰기.
 * @return: 처리한 바이트 수. 도중에 실패하면 그 오류(이미 처리한 바이트가
 *          있어도 오류를 우선한다).
 *
 * 왜 필요한가: 사용자는 한 번의 pread/pwrite 로 config 공간 전체를 읽으려
 * 할 수 있다(lspci 가 실제로 그렇게 한다). 그런데 실제 처리는 정렬된
 * 1/2/4바이트 조각 단위로만 가능하다. 그 반복을 담당하는 바깥 루프다.
 *
 * 동작 과정:
 *  1. region 번호가 실린 상위 비트를 마스크로 걷어내 config 공간 안의
 *     순수 오프셋만 남긴다. 이 마스크의 정의는
 *     include/linux/vfio_pci_core.h:26 에 있다.
 *  2. 남은 바이트가 없어질 때까지 vfio_pci_config_rw_single 을 부른다.
 *     반환된 바이트 수만큼 남은 개수를 줄이고 버퍼와 오프셋을 전진시킨다.
 *     조각 자르기는 안쪽 함수가 하므로 여기서는 결과만 따라간다.
 *  3. 다 끝나면 호출자의 파일 오프셋을 처리한 만큼 전진시킨다. 루프 안에서
 *     쓰는 pos 는 지역 복사본이므로, 도중에 실패하면 호출자의 오프셋은
 *     전혀 움직이지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. vfio_pci_core.c 의 vfio_pci_rw 가 런타임
 * PM 으로 디바이스를 D0 에 붙잡아 둔 상태에서 부른다.
 *
 * 에러 경로: 조각 하나라도 실패하면 즉시 그 오류를 반환한다. 그때까지
 * 처리된 바이트는 이미 하드웨어나 vconfig 에 반영됐지만 호출자에게는
 * 알리지 않는다 — 부분 성공을 표현할 방법이 없기 때문이다.
 *
 * 호출 체인:
 *   vfio_pci_core_read / vfio_pci_core_write → vfio_pci_rw
 *     → [vfio_pci_config_rw] → vfio_pci_config_rw_single
 */
ssize_t vfio_pci_config_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			   size_t count, loff_t *ppos, bool iswrite)
{
	/* [한국어] 지금까지 처리한 총 바이트 수. */
	size_t done = 0;
	/* [한국어] 조각 처리 결과. */
	int ret = 0;
	/* [한국어] 호출자의 오프셋을 지역 복사본으로 가져온다. 도중에 실패하면 호출자의
	 * 값이 전혀 움직이지 않게 하려는 것이다. */
	loff_t pos = *ppos;

	/* [한국어] VFIO region 번호가 실린 상위 비트를 걷어내 config 공간 안의 순수
	 * 오프셋만 남긴다. 마스크의 정의는 include/linux/vfio_pci_core.h:26 에 있고,
	 * region 번호를 오프셋 상위 24비트에 싣는 인코딩은 같은 헤더 23~25줄이다. */
	pos &= VFIO_PCI_OFFSET_MASK;

	/* [한국어] 요청한 바이트를 다 처리할 때까지 반복한다. */
	while (count) {
		/* [한국어] 조각 하나를 처리한다. 조각 자르기는 안쪽 함수가 하므로 여기서는 남은
		 * 개수만 넘긴다. */
		ret = vfio_pci_config_rw_single(vdev, buf, count, &pos, iswrite);
		if (ret < 0)
			return ret;

		/* [한국어] 처리한 만큼 남은 개수를 줄인다. */
		count -= ret;
		/* [한국어] 총 처리량에 더한다. */
		done += ret;
		/* [한국어] 사용자 버퍼 커서를 전진시킨다. */
		buf += ret;
		/* [한국어] 오프셋을 전진시킨다. 안쪽 함수는 ppos 를 건드리지 않으므로 여기서
		 * 직접 올려 줘야 한다. */
		pos += ret;
	}

	/* [한국어] 전부 성공했을 때만 호출자의 파일 오프셋을 갱신한다. 실패 경로는
	 * 위에서 곧장 반환하므로 이 줄에 닿지 않는다. */
	*ppos += done;

	/* [한국어] 처리한 총 바이트 수. */
	return done;
}

/* [한국어]
 * vfio_pci_core_range_intersect_range - 사용자 버퍼 구간과 레지스터 구간이 겹치는 부분을 계산한다
 *
 * @buf_start: 사용자 요청의 시작 오프셋.
 * @buf_cnt: 사용자 요청의 바이트 수.
 * @reg_start: 관심 있는 레지스터의 시작 오프셋.
 * @reg_cnt: 그 레지스터의 바이트 수.
 * @buf_offset: 겹치는 부분이 사용자 버퍼 안에서 시작하는 위치(출력).
 * @intersect_count: 겹치는 바이트 수(출력).
 * @register_offset: 겹치는 부분이 레지스터 안에서 시작하는 위치(출력).
 * @return: 겹치면 true 이고 세 출력이 채워진다. 겹치지 않으면 false 이고
 *          출력은 건드리지 않는다.
 *
 * 왜 필요한가: 이 파일 자신은 쓰지 않는 순수 헬퍼이지만, 변종 vfio-pci
 * 드라이버들이 config 공간의 특정 레지스터만 자기 값으로 바꿔치기할 때
 * 반드시 필요하다. 사용자의 한 번의 접근이 레지스터 경계에 걸쳐 있을 수
 * 있어서, "이번 요청 중 어느 부분이 이 레지스터에 해당하는가" 를 계산해야
 * 한다. 그래서 EXPORT_SYMBOL_GPL 로 내보내며, 실제 소비자는
 * drivers/vfio/pci/virtio/legacy_io.c 와 nvgrace-gpu/main.c 다.
 *
 * 동작 과정: 두 경우로 나눈다.
 *  1. 버퍼가 레지스터보다 앞에서 시작해 레지스터 시작점을 덮는 경우:
 *     겹침은 레지스터의 처음부터이고, 버퍼 안 위치는 두 시작점의 차이다.
 *     길이는 레지스터 전체와 "버퍼 끝까지" 중 짧은 쪽이다.
 *  2. 버퍼가 레지스터 안쪽에서 시작하는 경우: 겹침은 버퍼의 처음부터이고,
 *     레지스터 안 위치는 두 시작점의 차이다. 길이는 버퍼 전체와 "레지스터
 *     끝까지" 중 짧은 쪽이다.
 *  3. 어느 쪽도 아니면 겹치지 않는다.
 *
 * 실행 컨텍스트: 정수 연산뿐이라 어떤 문맥에서도 안전하다. 실제로는 변종
 * 드라이버의 config 읽기/쓰기 콜백에서, 즉 프로세스 문맥에서 불린다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   변종 드라이버의 config rw 콜백(virtio/legacy_io.c:143 등)
 *     → [vfio_pci_core_range_intersect_range]
 */
/**
 * vfio_pci_core_range_intersect_range() - Determine overlap between a buffer
 *					   and register offset ranges.
 * @buf_start:		start offset of the buffer
 * @buf_cnt:		number of buffer bytes
 * @reg_start:		start register offset
 * @reg_cnt:		number of register bytes
 * @buf_offset:	start offset of overlap in the buffer
 * @intersect_count:	number of overlapping bytes
 * @register_offset:	start offset of overlap in register
 *
 * Returns: true if there is overlap, false if not.
 * The overlap start and size is returned through function args.
 */
bool vfio_pci_core_range_intersect_range(loff_t buf_start, size_t buf_cnt,
					 loff_t reg_start, size_t reg_cnt,
					 loff_t *buf_offset,
					 size_t *intersect_count,
					 size_t *register_offset)
{
	/* [한국어] 경우 1 — 버퍼가 레지스터보다 앞(또는 같은 곳)에서 시작하고 레지스터
	 * 시작점을 덮는다. */
	if (buf_start <= reg_start &&
	    buf_start + buf_cnt > reg_start) {
		/* [한국어] 겹침이 버퍼 안에서 시작하는 위치는 두 시작점의 차이다. */
		*buf_offset = reg_start - buf_start;
		/* [한국어] 겹치는 길이는 레지스터 전체 길이와 "버퍼 끝까지의 거리" 중 짧은 쪽이다.
		 * 버퍼가 레지스터를 다 덮지 못할 수 있다. */
		*intersect_count = min_t(size_t, reg_cnt,
					 buf_start + buf_cnt - reg_start);
		/* [한국어] 레지스터 쪽에서는 처음부터 겹친다. */
		*register_offset = 0;
		/* [한국어] 겹침 있음. */
		return true;
	}

	/* [한국어] 경우 2 — 버퍼가 레지스터 안쪽에서 시작한다. */
	if (buf_start > reg_start &&
	    buf_start < reg_start + reg_cnt) {
		/* [한국어] 버퍼 쪽에서는 처음부터 겹친다. */
		*buf_offset = 0;
		/* [한국어] 겹치는 길이는 버퍼 전체 길이와 "레지스터 끝까지의 거리" 중 짧은 쪽이다. */
		*intersect_count = min_t(size_t, buf_cnt,
					 reg_start + reg_cnt - buf_start);
		/* [한국어] 겹침이 레지스터 안에서 시작하는 위치는 두 시작점의 차이다. */
		*register_offset = buf_start - reg_start;
		/* [한국어] 겹침 있음. */
		return true;
	}

	/* [한국어] 두 경우 어디에도 해당하지 않으면 겹치지 않는다. 이때 세 출력 인자는
	 * 건드리지 않으므로 호출자는 반드시 반환값을 먼저 봐야 한다. */
	return false;
}
EXPORT_SYMBOL_GPL(vfio_pci_core_range_intersect_range);
