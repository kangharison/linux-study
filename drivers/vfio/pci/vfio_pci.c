// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2021, NVIDIA CORPORATION & AFFILIATES. All rights reserved
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */
/* [한국어 설명] vfio-pci 범용 드라이버 — PCI 함수 하나를 붙잡아 vfio 코어에
 * 넘겨 주는 얇은 껍데기 (drivers/vfio/pci/vfio_pci.c)
 *
 * 인용 규칙: 이 파일 자신은 주석이 붙으며 줄 번호가 밀리므로 함수 이름으로만
 * 가리킨다. 형제 파일 vfio_pci_config.c 와 vfio_pci_intrs.c 는 다른 작업자가
 * 동시에 주석 중이라 역시 함수 이름으로만 인용한다. drivers/pci 전체와
 * drivers/vfio 의 코어(vfio_main.c, iommufd.c, vfio_pci_core.c)는 주석 완료
 * 상태라 실제 줄 번호로 인용한다. 이 트리는 sparse checkout 이라
 * drivers/iommu 가 없고 include/linux 에는 여섯 개 헤더뿐이라,
 * pci.h / pci_ids.h / module.h 안의 매크로와 상수 정의는 "이 트리에서 확인
 * 못 함" 으로 적었다.
 *
 * === 파일의 역할 ===
 * vfio-pci 라는 이름의 **커널 모듈이자 PCI 드라이버**를 만든다. 이 파일이
 * 직접 하는 일은 놀랄 만큼 적다. 장치를 다루는 코드는 사실상 한 줄도 없고,
 * 다섯 가지 껍데기 일만 한다.
 *  (1) **모듈 파라미터 여섯 개**를 받아 그중 셋을 코어의 전역 변수로 옮긴다
 *      (vfio_pci_core_set_params). 나머지 셋은 이 파일 안에서만 쓰인다.
 *  (2) **denylist** — 알려진 하드웨어 결함 때문에 사용자 공간에 넘기면 위험한
 *      장치를 probe 단계에서 거절한다. 인텔 QAT 여섯 종과 DSA/IAX 두 종이다.
 *  (3) **vtable 두 벌** — struct vfio_device_ops 와 struct vfio_pci_device_ops.
 *      스무 개 남짓한 슬롯 중 열아홉 개가 코어 함수를 그대로 가리키고, 이
 *      파일이 자기 함수를 넣은 슬롯은 open_device 하나뿐이다.
 *  (4) **probe / remove / sriov_configure** — PCI 드라이버 모델의 세 콜백.
 *      probe 는 denylist 검사, 구조체 할당, drvdata 저장, 코어 등록 네 단계뿐이다.
 *  (5) **id_table 과 동적 ID** — 정적 표는 "driver_override 로 지목했을 때만
 *      유효" 한 항목 하나뿐이라 이 드라이버는 **아무것도 자동으로 붙잡지
 *      않는다**. 붙이는 방법은 driver_override, sysfs 의 new_id, 그리고
 *      모듈 파라미터 ids 세 가지다.
 *
 * 반대로 이 파일이 **하지 않는** 일이 훨씬 많다. 장치를 켜고 리셋하고
 * config 그림자를 만드는 것도, region 지도를 그리는 것도, read/write/mmap 도,
 * 인터럽트 설정도, hot reset 과 AER 도 전부 vfio_pci_core.c 가 한다. 이 파일은
 * 그 코어에 "이 PCI 함수를 맡아 달라" 고 말하는 창구다. 같은 창구를 다르게
 * 만든 것이 mlx5, hisilicon, pds, qat, nvgrace-gpu, virtio, xe, ism 변종
 * 드라이버들이며, 그들도 같은 코어를 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * NVMe 컨트롤러를 SPDK 에 넘길 때 실제로 일어나는 일을 따라가면 이 파일이
 * 어디에 있는지 그대로 드러난다.
 *
 *   1. 사용자가 커널 nvme 드라이버를 unbind 한다. drivers/nvme/host/pci.c 의
 *      nvme_remove 가 컨트롤러를 내린다.
 *   2. 사용자가 /sys/bus/pci/devices/<주소>/driver_override 에 "vfio-pci" 를
 *      쓴다. 그 sysfs 속성의 동작은 drivers/pci/pci-driver.c:362~404 에 주석돼
 *      있다.
 *   3. 사용자가 bind 를 쓰면 드라이버 코어가 pci_bus_match 를 거쳐
 *      pci_match_device(drivers/pci/pci-driver.c:430)를 부른다. 거기서
 *      device_match_driver_override 가 양수를 돌려주고, 이 파일의 정적
 *      id_table 항목이 override_only 라 그때만 인정된다
 *      (pci-driver.c:455~468).
 *   4. drivers/pci/pci-driver.c 의 pci_device_probe → local_pci_probe(757줄)가
 *      [이 파일] vfio_pci_probe 를 부른다.
 *   5. vfio_pci_probe → vfio_alloc_device(include/linux/vfio.h:327 의 매크로,
 *      실체는 drivers/vfio/vfio_main.c:921 의 _vfio_alloc_device)
 *      → 그 안에서 vtable 의 init 슬롯, 즉
 *      vfio_pci_core_init_dev(vfio_pci_core.c:6525)가 불린다.
 *   6. vfio_pci_core_register_device(vfio_pci_core.c:6701)가 device set 을
 *      배정하고 마이그레이션 vtable 완비성을 검사한 뒤
 *      vfio_register_group_dev(vfio_main.c:1350)로 등록한다.
 *      이때 /dev/vfio/<group> 아래에 장치가 보이기 시작한다.
 *   7. 사용자 공간이 device fd 를 열면 vfio_main.c 가 vtable 의 open_device
 *      슬롯, 즉 [이 파일] vfio_pci_open_device 를 부른다. 그 함수가
 *      vfio_pci_core_enable(1948줄) → (인텔 IGD 면 IGD region 등록) →
 *      vfio_pci_core_finish_enable(2536줄) 순서로 코어를 부른다.
 *   8. 그 뒤 SPDK 가 BAR0 을 mmap 하고 NVMe 도어벨을 직접 두드린다. 그
 *      순간부터 이 파일도 코어도 실행 경로에 없다.
 *
 * 실행 컨텍스트는 전부 **호스트 커널의 프로세스 문맥**이다. probe/remove 는
 * 드라이버 코어의 바인딩 경로에서, sriov_configure 는 sysfs 쓰기에서,
 * init/cleanup 은 모듈 적재/해제에서 불린다. 인터럽트 문맥이나 원자적 문맥에서
 * 도는 코드가 이 파일에는 하나도 없다.
 *
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/pci/vfio_pci_core.c (주석 완료) — 이 파일의 알맹이 전부.
 *      vtable 이 가리키는 것들: vfio_pci_core_init_dev(6525줄),
 *      vfio_pci_core_release_dev(6611줄), vfio_pci_core_close_device(2460줄),
 *      vfio_pci_core_ioctl(4590줄), vfio_pci_ioctl_get_region_info(3529줄),
 *      vfio_pci_core_ioctl_feature(4754줄), vfio_pci_core_read(4958줄),
 *      vfio_pci_core_write(5007줄), vfio_pci_core_mmap(5561줄),
 *      vfio_pci_core_request(5733줄), vfio_pci_core_match(6008줄),
 *      vfio_pci_core_match_token_uuid(5823줄),
 *      vfio_pci_core_get_dmabuf_phys(vfio_pci_dmabuf.c:172).
 *      그 밖에 vfio_pci_core_set_params(7852줄),
 *      vfio_pci_core_register_device(6701줄),
 *      vfio_pci_core_unregister_device(6907줄),
 *      vfio_pci_core_sriov_configure(7059줄), 그리고 err_handler 슬롯이
 *      가리키는 vfio_pci_core_err_handlers 표(7172줄).
 *  - drivers/vfio/vfio_main.c (주석 완료)
 *      : vfio_alloc_device 의 실체 _vfio_alloc_device(921줄)와 그 짝인
 *      vfio_put_device(include/linux/vfio.h 의 인라인). vtable 을 소비하는
 *      쪽도 이 파일이다.
 *  - drivers/vfio/iommufd.c (주석 완료)
 *      : vtable 의 여섯 개 iommufd 슬롯이 가리키는 함수들.
 *      vfio_iommufd_physical_bind(318줄), _attach_ioas(373줄),
 *      _pasid_attach_ioas(431줄)과 각각의 짝. "physical" 판은 진짜 PCI 함수를
 *      IOMMU 에 직접 붙이는 판이며, 에뮬레이션 장치용 "emulated" 판과 구별된다.
 *  - drivers/vfio/pci/vfio_pci_igd.c
 *      : vfio_pci_is_intel_display 와 vfio_pci_igd_init. 이 파일의
 *      open_device 가 코어의 enable 과 finish_enable 사이에 끼워 넣는 유일한
 *      추가 작업이다. CONFIG_VFIO_PCI_IGD 가 꺼지면 vfio_pci_priv.h 의 stub 이
 *      각각 false 와 -ENODEV 를 돌려준다.
 *  - drivers/pci (전부 주석 완료)
 *      : pci_add_dynid(pci-driver.c:242) — ids 파라미터가 부르는 동적 ID 추가.
 *      그 함수가 목록에 넣은 뒤 driver_attach 를 불러 **즉시 재대조**를
 *      일으키므로, 모듈 적재만으로 장치가 붙는다.
 *      pci_unregister_driver(pci-driver.c:3182), __pci_register_driver(3130줄),
 *      pci_match_device(430줄), local_pci_probe(757줄).
 *      driver_managed_dma 플래그를 소비하는 곳은 pci-driver.c:3623 과 3655 다.
 *  - 데이터 흐름: 이 파일을 지나는 데이터는 사실상 없다. 오가는 것은
 *      **포인터와 소유권**뿐이다 — pci_dev 하나가 들어와
 *      struct vfio_pci_core_device 로 감싸이고, 그 주소가 dev_get_drvdata 로
 *      다시 꺼내진다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - vfio_pci_dev_in_denylist 와 vfio_pci_is_denylisted 두 함수
 *      : 위험 장치 판정 두 겹. 앞이 순수한 표 조회이고, 뒤가 그 결과에
 *        disable_denylist 파라미터와 경고 메시지를 얹는다.
 *  - vfio_pci_open_device
 *      : 이 파일이 vtable 에 넣은 **유일한 자기 함수**. 코어의 enable 과
 *        finish_enable 사이에 인텔 IGD region 등록을 끼워 넣는다.
 *  - vfio_pci_ops (struct vfio_device_ops)
 *      : 사용자 공간에서 보이는 모든 동작의 진입표. 스무 개 슬롯.
 *  - vfio_pci_dev_ops (struct vfio_pci_device_ops)
 *      : PCI 계층 안에서만 쓰는 작은 표. 지금은 dma-buf 물리 주소 조회
 *        슬롯 하나뿐이다.
 *  - vfio_pci_probe / vfio_pci_remove
 *      : 바인딩과 해제. 네 단계와 두 단계.
 *  - vfio_pci_sriov_configure
 *      : SR-IOV VF 개수 설정. enable_sriov 파라미터로 문을 여닫는 게이트일 뿐,
 *        실제 일은 코어가 한다.
 *  - vfio_pci_table (struct pci_device_id 배열)
 *      : 항목 하나. PCI_ANY_ID 두 개에 "override 전용" 표시가 붙어 있다.
 *  - vfio_pci_driver (struct pci_driver)
 *      : 드라이버 코어에 등록하는 서술자.
 *  - vfio_pci_fill_ids
 *      : ids 모듈 파라미터 문자열을 파싱해 동적 ID 로 추가한다.
 *  - vfio_pci_init / vfio_pci_cleanup
 *      : 모듈 진입점과 퇴장점.
 *
 * === id_table 이 "override 전용" 이라는 뜻 ===
 * vfio_pci_table 의 유일한 항목은 PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_ANY_ID,
 * PCI_ANY_ID) 다. 그 매크로는 vendor 와 device 를 와일드카드로 두면서 항목의
 * override_only 필드를 세운다(매크로 자체는 include/linux/pci.h 에 있고 이
 * 트리에는 없다). 그 필드를 실제로 해석하는 곳은
 * drivers/pci/pci-driver.c:462 다 — override_only 인 항목은 사용자가
 * driver_override 로 이 드라이버를 지목했을 때(ret > 0)만 인정된다.
 *
 * 그래서 이 드라이버는 **부팅 중에 아무 장치도 낚아채지 않는다**. 모든
 * PCI 장치와 형식상 맞지만, 사용자가 명시적으로 지목하지 않으면 그 항목이
 * 무효이기 때문이다. 그렇지 않았다면 vfio-pci 가 모든 장치를 먼저 붙잡아
 * 시스템이 부팅되지 않았을 것이다.
 *
 * 반면 **동적 ID 는 override_only 검사를 받지 않는다.** pci_match_device 가
 * 정적 표보다 dynids 목록을 먼저 훑고, 거기서 맞으면 그대로 돌려주기
 * 때문이다(pci-driver.c:443~454). 그래서 sysfs 의 new_id 나 이 파일의 ids
 * 모듈 파라미터로 넣은 ID 는 **자동 바인딩을 일으킨다**. 두 경로의 이 차이가
 * vfio_pci_fill_ids 가 존재하는 이유다.
 *
 * === probe 가 하는 일과 코어가 하는 일의 경계 ===
 * vfio_pci_probe 가 하는 일은 정확히 네 가지다.
 *   (1) denylist 검사 — 이 파일만의 정책이다. 코어는 denylist 를 모른다.
 *   (2) vfio_alloc_device — struct vfio_pci_core_device 를 통째로 할당하고
 *       vtable 의 init 슬롯을 부른다. 즉 이 한 줄 안에서 이미
 *       vfio_pci_core_init_dev 가 돌아 mutex 와 목록들이 초기화된다.
 *   (3) dev_set_drvdata 와 pci_ops 대입 — remove 와 sriov_configure 가
 *       나중에 이 포인터를 되찾을 수 있게 하는 연결 고리다.
 *   (4) vfio_pci_core_register_device.
 *
 * 그리고 **코어가 하는 일**은 (4) 안에서 벌어진다 — 물리 함수인지 검사,
 * SR-IOV PF/VF 관계 파악, device set(함께 리셋되는 함수들의 묶음) 배정,
 * 마이그레이션 vtable 의 슬롯 단위 완비성 검사, 그리고
 * vfio_register_group_dev 호출. 그 어느 것도 이 파일에는 없다.
 *
 * remove 는 더 짧다 — 코어의 unregister 를 부르고 참조를 놓는다. 장치를
 * 실제로 끄고 되돌리는 일(vfio_pci_core_disable)은 사용자가 fd 를 닫을 때
 * 이미 일어났거나, unregister 안에서 코어가 처리한다.
 *
 * === 모듈 파라미터 여섯 개 ===
 * 셋은 코어로 넘어가고 셋은 여기 남는다.
 *  - **코어로 넘어가는 것**(vfio_pci_core_set_params, vfio_pci_core.c:7852):
 *      nointxmask(PCI 2.3 방식 INTx 마스킹을 쓰지 않음),
 *      disable_vga(레거시 VGA region 을 노출하지 않음),
 *      disable_idle_d3(놀고 있는 장치를 D3hot 으로 재우지 않음).
 *  - **여기 남는 것**:
 *      ids(적재 시점에 동적 ID 로 추가할 목록. __initdata 라 적재 후 버려진다),
 *      enable_sriov(SR-IOV 설정을 허용할지),
 *      disable_denylist(위험 장치 목록을 무시할지).
 * 권한 비트도 제각각이다 — ids 는 0(sysfs 파일 자체가 안 생김),
 * disable_denylist 는 0444(읽기 전용, 적재 후 바꿀 수 없음),
 * enable_sriov 는 0644, 나머지는 S_IRUGO 또는 S_IRUGO|S_IWUSR 이다.
 * 적재 후에도 바꿀 수 있는 값(nointxmask, disable_idle_d3, enable_sriov)은
 * 이미 열려 있는 장치에는 소급 적용되지 않는다 — 코어가 그 값을 읽는 시점이
 * 장치를 여는 순간이기 때문이다.
 *
 * === denylist 가 막는 것 ===
 * 인텔 QAT(QuickAssist) 가속기 세 세대의 PF/VF 여섯 개와 Sapphire Rapids 의
 * DSA/IAX 가속기 둘이 목록에 있다. 이 장치들은 알려진 errata 때문에 신뢰할 수
 * 없는 사용자에게 넘기면 시스템 안정성이나 보안을 해칠 수 있다는 것이
 * disable_denylist 파라미터 설명문의 요지다. 목록에 걸리면 probe 가 -EINVAL 로
 * 실패하므로 바인딩 자체가 되지 않는다. disable_denylist 를 켜면 경고만 찍고
 * 통과시킨다. 이 정책은 오직 이 파일에만 있으며, vendor 변종 드라이버나
 * 코어는 관여하지 않는다. */

/* [한국어] pr_warn / pr_info 의 출력 앞에 모듈 이름과 콜론을 붙이는 관례. 이 파일의
 * 메시지는 장치가 아직 없는 시점(모듈 적재, ID 파싱)에도 나오므로 pci_warn
 * 같은 장치 기준 매크로를 쓸 수 없고, 그래서 접두사를 직접 붙인다.
 * KBUILD_MODNAME 은 빌드 시스템이 정의하는 모듈 이름으로 여기서는 "vfio-pci" 다.
 * 반드시 어떤 헤더보다 **먼저** 정의해야 printk 계열이 이 정의를 보게 된다. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* [한국어] struct device 와 dev_set_drvdata / dev_get_drvdata. probe 가 저장하고 remove 와 sriov_configure 가 되찾는 그 포인터 통로다. */
#include <linux/device.h>
/* [한국어] eventfd 관련 선언. 이 파일이 직접 부르지는 않지만 vtable 이 가리키는 코어 함수들이 eventfd 를 다루므로 타입 정의가 필요하다. */
#include <linux/eventfd.h>
/* [한국어] 파일 서술자 관련 선언. 같은 이유로 딸려 오는 헤더다. */
#include <linux/file.h>
/* [한국어] 인터럽트 관련 선언. 역시 vtable 이 가리키는 코어 경로의 타입 때문이다. */
#include <linux/interrupt.h>
/* [한국어] IOMMU 관련 선언. iommufd 슬롯 여섯 개의 타입에 필요하다. 다만 IOMMU
 * 드라이버 본체(drivers/iommu)는 이 트리에 없으므로 내부 동작은 확인 못 함. */
#include <linux/iommu.h>
/* [한국어] module_param 계열 매크로, MODULE_LICENSE / MODULE_AUTHOR / MODULE_DESCRIPTION, module_init / module_exit. 이 파일이 모듈로서 존재하는 데 필요한 전부가 여기서 온다. */
#include <linux/module.h>
/* [한국어] 뮤텍스 선언. 코어 구조체가 물고 있는 잠금들의 타입 때문에 필요하다. */
#include <linux/mutex.h>
/* [한국어] 알림 체인(notifier). 코어가 PCI 버스 알림을 받는 데 쓰는 타입이다. */
#include <linux/notifier.h>
/* [한국어] 런타임 전원 관리. 코어의 저전력 기능이 쓰는 선언이다. */
#include <linux/pm_runtime.h>
/* [한국어] 동적 할당. 이 파일이 직접 kmalloc 을 부르지는 않지만 코어 경로가 쓴다. */
#include <linux/slab.h>
/* [한국어] 기본 정수 타입 별칭(u8, u16, bool 등). */
#include <linux/types.h>
/* [한국어] 사용자 공간 접근 헬퍼. 이 파일은 직접 쓰지 않고 vtable 대상이 쓴다. */
#include <linux/uaccess.h>

/* [한국어] 형제 파일 사이의 내부 헤더. 여기서 vfio_pci_is_intel_display 와
 * vfio_pci_igd_init 의 선언(또는 CONFIG_VFIO_PCI_IGD 가 꺼졌을 때의 stub)이
 * 온다. 그 stub 덕분에 아래 vfio_pci_open_device 가 #ifdef 없이 깔끔하게
 * 쓰인다. 이 헤더가 다시 include/linux/vfio_pci_core.h 를 포함하므로
 * struct vfio_pci_core_device 와 코어 함수 prototype 도 함께 딸려 온다. */
#include "vfio_pci_priv.h"

/* [한국어] MODULE_AUTHOR 에 넣을 문자열. 상수를 따로 두는 것은 커널 모듈의 오래된 관례다. */
#define DRIVER_AUTHOR   "Alex Williamson <alex.williamson@redhat.com>"
/* [한국어] MODULE_DESCRIPTION 에 넣을 문자열. "meta-driver" 라는 표현이 이 파일의
 * 성격을 정확히 말한다 - 장치를 실제로 운전하지 않고, 운전할 권한을 사용자
 * 공간에 넘겨 주는 드라이버다. */
#define DRIVER_DESC     "VFIO PCI - User Level meta-driver"

/* [한국어] 모듈 적재 시점에 동적 ID 로 추가할 장치 목록 문자열.
 * 설정자: 부트 파라미터나 modprobe 인자로 사용자가 준다.
 * 읽는 자: 아래 vfio_pci_fill_ids 하나뿐이며, 그것도 모듈 초기화 때 한 번이다.
 * 값 범위: 최대 1023자 + 종료 문자. "vendor:device[:subvendor[:subdevice
 * [:class[:class_mask]]]]" 형식을 쉼표로 여러 개 이어 붙인 것.
 * 동기화: __initdata 라 초기화 코드 구간에 놓이며, 모듈 적재가 끝나면 그
 * 메모리가 통째로 회수된다. 그래서 적재 후에는 절대 참조할 수 없고,
 * 아래 module_param_string 의 권한을 0 으로 준 것도 sysfs 파일을 만들면
 * 회수된 메모리를 읽게 되기 때문이다. */
static char ids[1024] __initdata;
/* [한국어] ids 배열을 문자열 파라미터로 등록한다. 마지막 인자 권한이 **0** 이라
 * /sys/module/vfio_pci/parameters 아래에 파일이 생기지 않는다. 위에서 말한
 * __initdata 회수 때문이며, 적재 후에 읽거나 쓸 수 없다는 뜻이다. */
module_param_string(ids, ids, sizeof(ids), 0);
/* [한국어] 사용자에게 보이는 설명문. 형식과 여러 개를 쉼표로 잇는다는 규칙을 밝힌다. */
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the vfio driver, format is \"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\" and multiple comma separated entries can be specified");

/* [한국어] PCI 2.3 방식 INTx 마스킹을 쓰지 않게 하는 스위치.
 * 설정자: 모듈 파라미터. 적재 후에도 쓸 수 있다(S_IWUSR).
 * 읽는 자: 아래 vfio_pci_init 이 vfio_pci_core_set_params 로 코어에 넘긴다.
 * 코어에서는 vfio_pci_core.c:471 의 같은 이름 전역 변수가 된다.
 * 값 범위: true/false. 기본 false(마스킹을 시도한다).
 * 동기화: 없다. 적재 시점에 한 번 코어로 옮겨지므로, 적재 후에 값을 바꿔도
 * 이미 코어에 복사된 값은 바뀌지 않는다. */
static bool nointxmask;
/* [한국어] named 판을 쓰는 것은 sysfs 이름과 변수 이름을 명시적으로 맞추기
 * 위해서다(여기서는 둘이 같다). S_IRUGO 는 모두 읽기 가능, S_IWUSR 은 소유자
 * 쓰기 가능이라 적재 후에도 값을 바꿀 수 있다. */
module_param_named(nointxmask, nointxmask, bool, S_IRUGO | S_IWUSR);
/* [한국어] 설명문 등록. */
MODULE_PARM_DESC(nointxmask,
		  /* [한국어] 특정 장치에서 문제가 해결되면 lspci 출력을 메일링 리스트에 보고해 broken_intx_masking 플래그로 자동 처리되게 하라는 안내다. */
		  "Disable support for PCI 2.3 style INTx masking.  If this resolves problems for specific devices, report lspci -vvvxxx to linux-pci@vger.kernel.org so the device can be fixed automatically via the broken_intx_masking flag.");

/* [한국어] 레거시 VGA 지원이 컴파일에 포함될 때만 이 파라미터가 존재한다. */
#ifdef CONFIG_VFIO_PCI_VGA
/* [한국어] 레거시 VGA region 노출을 끄는 스위치.
 * 설정자: 모듈 파라미터.
 * 읽는 자: vfio_pci_init 이 코어로 넘긴다. CONFIG 가 꺼져 있으면 그 변수 자체가
 * 없으므로 init 이 상수 true 를 대신 넘긴다.
 * 값 범위: true/false. 기본 false(VGA region 을 노출한다).
 * 동기화: 없다. S_IRUGO 뿐이라 적재 후 변경도 불가능하다. */
static bool disable_vga;
/* [한국어] 읽기 전용 파라미터. VGA 노출 여부를 장치가 열린 뒤에 바꾸면 region 지도가 어긋나므로 쓰기를 막아 둔 것으로 보인다. */
module_param(disable_vga, bool, S_IRUGO);
/* [한국어] 설명문 등록. */
MODULE_PARM_DESC(disable_vga, "Disable VGA resource access through vfio-pci");
/* [한국어] CONFIG_VFIO_PCI_VGA 분기 끝. */
#endif

/* [한국어] 놀고 있는 장치를 D3hot 저전력 상태로 재우는 동작을 끄는 스위치.
 * 설정자: 모듈 파라미터. 적재 후에도 쓸 수 있다.
 * 읽는 자: vfio_pci_init 이 코어로 넘긴다. 코어에서는 장치를 닫을 때
 * D3hot 으로 내릴지 판단하는 데 쓰인다.
 * 값 범위: true/false. 기본 false(재운다).
 * 동기화: 없다. 코어로 한 번 복사된다. */
static bool disable_idle_d3;
/* [한국어] 적재 후에도 소유자가 바꿀 수 있는 권한이지만, 이미 코어에 복사된 값에는 영향이 없다. */
module_param(disable_idle_d3, bool, S_IRUGO | S_IWUSR);
/* [한국어] 설명문 등록. */
MODULE_PARM_DESC(disable_idle_d3,
		 /* [한국어] 쓰이지 않는 유휴 장치에 PCI D3 저전력 상태를 쓰지 않게 한다는 설명이다. */
		 "Disable using the PCI D3 low power state for idle, unused devices");

/* [한국어] SR-IOV 설정을 허용하는 스위치. **이 변수만 #ifdef 밖에 있다.**
 * 설정자: CONFIG_PCI_IOV 가 켜져 있을 때만 모듈 파라미터로 설정할 수 있다.
 * 읽는 자: 아래 vfio_pci_sriov_configure 하나뿐. 거짓이면 -ENOENT 를 돌려준다.
 * 값 범위: true/false. 기본 false(SR-IOV 설정을 거부한다).
 * 동기화: 없다. 매 호출마다 새로 읽으므로 적재 후 변경이 즉시 반영된다 -
 * 코어로 복사되는 위 셋과 다른 점이다.
 * 변수만 밖에 둔 이유: CONFIG_PCI_IOV 가 꺼져 있어도 아래
 * vfio_pci_sriov_configure 가 컴파일되어야 하고, 그 함수가 이 변수를 읽기
 * 때문이다. 그 경우 값이 언제나 false 라 SR-IOV 설정은 항상 -ENOENT 가 된다. */
static bool enable_sriov;
/* [한국어] SR-IOV 자체가 컴파일에 포함될 때만 파라미터를 노출한다. */
#ifdef CONFIG_PCI_IOV
/* [한국어] 0644 = 모두 읽기, 소유자 쓰기. 적재 후에 켜고 끌 수 있다. */
module_param(enable_sriov, bool, 0644);
/* [한국어] 설명문. PF 에서 SR-IOV 를 켜려면 대개 유저스페이스 PF 드라이버의
 * 지원이 필요하며, 그 지원 없이 VF 를 만들면 VF 나 PF 가 제대로 동작하지
 * 않을 수 있다는 경고다. 기본값이 false 인 이유가 이 경고에 있다. */
MODULE_PARM_DESC(enable_sriov, "Enable support for SR-IOV configuration.  Enabling SR-IOV on a PF typically requires support of the userspace PF driver, enabling VFs without such support may result in non-functional VFs or PF.");
/* [한국어] CONFIG_PCI_IOV 분기 끝. */
#endif

/* [한국어] denylist 를 무시하게 하는 스위치.
 * 설정자: 모듈 파라미터. **적재 시점에만** 정할 수 있다.
 * 읽는 자: 아래 vfio_pci_is_denylisted 와 vfio_pci_init.
 * 값 범위: true/false. 기본 false(목록을 지킨다).
 * 동기화: 없다. 0444 라 적재 후 변경이 불가능하므로 경쟁 자체가 없다. */
static bool disable_denylist;
/* [한국어] 0444 = **읽기 전용**. 다른 파라미터와 달리 쓰기 권한이 전혀 없다.
 * 안전 장치를 런타임에 몰래 해제하지 못하게 하려는 것이다 - 끄려면 모듈을
 * 내렸다가 이 값을 주고 다시 올려야 하고, 그 흔적이 남는다. */
module_param(disable_denylist, bool, 0444);
/* [한국어] 설명문. denylist 를 끄면 알려진 errata 가 있는 장치에도 바인딩할 수
 * 있게 되며, 신뢰할 수 없는 사용자가 그 장치를 만지면 시스템 안정성이나
 * 보안을 해칠 수 있다는 경고다. */
MODULE_PARM_DESC(disable_denylist, "Disable use of device denylist. Disabling the denylist allows binding to devices with known errata that may lead to exploitable stability or security issues when accessed by untrusted users.");

/* [한국어] vfio_pci_dev_in_denylist - 이 장치가 위험 목록에 있는지 표를 조회한다
 *
 * @pdev: 검사할 PCI 함수. vendor 와 device 필드만 본다.
 * @return: true = 목록에 있음, false = 없음.
 *
 * 왜 필요한가: 일부 가속기는 알려진 하드웨어 결함 때문에 신뢰할 수 없는
 * 사용자에게 넘기면 시스템 안정성이나 보안을 해칠 수 있다. 그런 장치를
 * vfio-pci 에 바인딩하지 못하게 막는 정책이 필요하고, 그 판정의 순수한
 * 부분(표 조회)만 이 함수가 맡는다. 정책 판단(모듈 파라미터로 무시할지,
 * 경고를 찍을지)은 바로 아래 vfio_pci_is_denylisted 가 얹는다.
 *
 * 동작: vendor 로 한 번, device 로 한 번, 두 겹 switch 다. 인텔이 아니면
 * 바깥 switch 를 그냥 빠져나가 마지막 return 으로 간다. 인텔이면 여덟 개의
 * device ID 와 대조한다 - QAT(QuickAssist) 세 세대의 PF/VF 여섯 개와
 * Sapphire Rapids 의 DSA/IAX 둘이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(probe 경로). 잠금 없이 읽기만 한다.
 * 호출자: vfio_pci_is_denylisted 하나뿐.
 * 호출 대상: 없다. 순수한 값 비교뿐이다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_probe -> vfio_pci_is_denylisted -> [vfio_pci_dev_in_denylist] */
static bool vfio_pci_dev_in_denylist(struct pci_dev *pdev)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 제조사 ID 로 먼저 가른다. 목록에 오른 장치가 전부 인텔이라 바깥 switch 에 case 가 하나뿐이다. */
	switch (pdev->vendor) {
	/* [한국어] 인텔. 상수 정의는 include/linux/pci_ids.h 에 있고 이 트리에는 그 헤더가 없어 값 자체는 확인 못 함. */
	case PCI_VENDOR_ID_INTEL:
		/* [한국어] 인텔 장치들 중 어느 것인지 가른다. */
		switch (pdev->device) {
		/* [한국어] QAT C3xxx 가속기 PF. */
		case PCI_DEVICE_ID_INTEL_QAT_C3XXX:
		/* [한국어] 그 VF. PF 와 VF 를 둘 다 막는다 - VF 만 넘겨도 같은 errata 에 노출되기 때문이다. */
		case PCI_DEVICE_ID_INTEL_QAT_C3XXX_VF:
		/* [한국어] QAT C62x 가속기 PF. */
		case PCI_DEVICE_ID_INTEL_QAT_C62X:
		/* [한국어] 그 VF. */
		case PCI_DEVICE_ID_INTEL_QAT_C62X_VF:
		/* [한국어] QAT DH895xCC 가속기 PF. */
		case PCI_DEVICE_ID_INTEL_QAT_DH895XCC:
		/* [한국어] 그 VF. */
		case PCI_DEVICE_ID_INTEL_QAT_DH895XCC_VF:
		/* [한국어] Sapphire Rapids 의 DSA(Data Streaming Accelerator) 0번. */
		case PCI_DEVICE_ID_INTEL_DSA_SPR0:
		/* [한국어] 같은 세대의 IAX(In-memory Analytics Accelerator) 0번. */
		case PCI_DEVICE_ID_INTEL_IAX_SPR0:
			/* [한국어] 여덟 개 중 하나에 걸리면 목록에 있다고 보고한다. */
			return true;
		/* [한국어] 그 밖의 인텔 장치. */
		default:
			/* [한국어] 인텔이어도 목록에 없으면 허용이다. */
			return false;
		/* [한국어] 안쪽 switch 끝. 두 갈래 모두 return 하므로 여기로 흘러나오는 경로는 없다. */
		}
	/* [한국어] 바깥 switch 끝. 인텔이 아닌 벤더는 case 에 걸리지 않아 그대로 아래로 나온다. */
	}

	/* [한국어] 인텔이 아닌 모든 장치는 허용이다. */
	return false;
}

/* [한국어] vfio_pci_is_denylisted - 위험 목록 판정에 파라미터와 경고를 얹는다
 *
 * @pdev: 검사할 PCI 함수.
 * @return: true = 바인딩을 거부해야 함, false = 진행해도 됨.
 *
 * 왜 필요한가: 표 조회만으로는 정책이 완성되지 않는다. 관리자가 위험을
 * 알고도 쓰겠다고 하면(disable_denylist) 허용해야 하고, 어느 쪽이든 왜
 * 그렇게 됐는지 로그에 남아야 한다. 그 두 가지를 여기서 얹는다.
 *
 * 동작 단계: (1) 표에 없으면 조용히 false. **목록에 없는 장치는 로그를 전혀
 * 남기지 않는다** - 대부분의 장치가 여기에 해당하므로 로그가 조용하다.
 * (2) 목록에 있는데 파라미터로 목록을 껐으면 경고를 찍고 false(허용).
 * (3) 목록에 있고 파라미터도 기본값이면 경고를 찍고 true(거부).
 * (2)와 (3)의 메시지가 다르다는 점이 중요하다 - 관리자가 로그만 보고
 * "막혔다" 와 "위험을 무릅쓰고 통과시켰다" 를 구별할 수 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥(probe 경로).
 * 호출자: vfio_pci_probe 하나뿐.
 * 호출 대상: vfio_pci_dev_in_denylist, pci_warn.
 *
 * 에러 경로: 없다. 판정만 하고 오류를 만들지 않는다. 실제 -EINVAL 은
 * 호출자가 만든다.
 *
 * 호출 체인:
 *   드라이버 코어의 바인딩 -> vfio_pci_probe -> [vfio_pci_is_denylisted]
 *     -> vfio_pci_dev_in_denylist */
static bool vfio_pci_is_denylisted(struct pci_dev *pdev)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 표에 없으면. */
	if (!vfio_pci_dev_in_denylist(pdev))
		/* [한국어] 조용히 허용한다. 로그도 남기지 않는다. */
		return false;

	/* [한국어] 관리자가 목록을 끄겠다고 명시했는가. 이 파라미터는 0444 라 적재 후에
	 * 바꿀 수 없으므로, 여기서 읽는 값은 모듈이 올라올 때 정해진 그대로다. */
	if (disable_denylist) {
		/* [한국어] 장치 기준 경고를 찍는다. pci_warn 은 메시지 앞에 PCI 주소와 드라이버 이름을 자동으로 붙여 준다. */
		pci_warn(pdev,
			 /* [한국어] 목록이 꺼져 있어 이 장치를 허용한다는 내용. */
			 "device denylist disabled - allowing device %04x:%04x.\n",
			 /* [한국어] vendor 와 device ID 를 네 자리 16진수로 찍는다. */
			 pdev->vendor, pdev->device);
		/* [한국어] 허용한다. 경고는 남았으므로 나중에 문제가 생겼을 때 추적할 수 있다. */
		return false;
	/* [한국어] 파라미터 분기 끝. */
	}

	/* [한국어] 여기까지 왔으면 목록에 있고 파라미터도 기본값이다. 거부한다는 사실을 남긴다. */
	pci_warn(pdev, "%04x:%04x exists in vfio-pci device denylist, driver probing disallowed.\n",
		 /* [한국어] 역시 vendor 와 device ID 를 함께 찍어 어떤 장치가 막혔는지 알 수 있게 한다. */
		 pdev->vendor, pdev->device);

	/* [한국어] 거부를 보고한다. 호출자가 이 값을 보고 -EINVAL 로 probe 를 실패시킨다. */
	return true;
}

/* [한국어] vfio_pci_open_device - vtable 의 open_device 슬롯. **이 파일이 vtable 에
 * 넣은 유일한 자기 함수**다
 *
 * @core_vdev: vfio 코어가 넘겨 주는 추상 디바이스. 이것을 감싼
 *   struct vfio_pci_core_device 를 container_of 로 복원해 쓴다.
 * @return: 0 = 성공, 그 밖에는 vfio_pci_core_enable 이나 vfio_pci_igd_init 이
 *   올린 음수 errno.
 *
 * 왜 필요한가: 장치를 여는 일 자체는 코어가 다 한다. 그런데 코어의
 * enable 과 finish_enable 사이에는 **vendor 가 추가 region 을 등록할 수 있는
 * 틈**이 있다. finish_enable 이 그때까지 등록된 region 을 기준으로 mmap 가능
 * 여부를 확정하기 때문이다. 인텔 통합 그래픽(IGD)은 OpRegion 과 호스트
 * 브리지 config 같은 추가 region 이 있어야 게스트에서 동작하므로, 그 틈에
 * IGD region 등록을 끼워 넣어야 한다. 그 한 가지 때문에 이 함수가 존재한다.
 * 그래서 다른 열아홉 개 슬롯은 코어 함수를 그대로 가리킬 수 있다.
 *
 * 동작 단계: (1) 코어의 enable 로 전원, 초기 리셋, config 그림자,
 * MSI-X 위치 파악까지 마친다. (2) 인텔 디스플레이 장치이면 IGD region 을
 * 등록한다. -ENODEV 는 "이 장치에는 그런 region 이 없다" 는 정상적인 답이라
 * 무시하고 넘어간다. 그 밖의 오류는 **enable 을 되돌리고** 실패로 처리한다.
 * (3) finish_enable 로 region 목록을 확정한다.
 *
 * 되돌리기 순서가 중요하다 - (2)에서 실패하면 (1)이 잡은 것을 반드시
 * vfio_pci_core_disable 로 놓아야 한다. 그 한 줄이 빠지면 장치가 켜진 채
 * 사용자 없이 남는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(device fd 를 처음 여는 경로). 잠들 수 있다.
 * 호출자: drivers/vfio/vfio_main.c 의 vfio_df_open 이 vtable 의 이 슬롯을
 * 부른다.
 * 호출 대상: vfio_pci_core_enable(vfio_pci_core.c:1948),
 * vfio_pci_is_intel_display / vfio_pci_igd_init(vfio_pci_igd.c, CONFIG 가
 * 꺼지면 vfio_pci_priv.h 의 stub 이 각각 false 와 -ENODEV),
 * vfio_pci_core_disable(vfio_pci_core.c:2199),
 * vfio_pci_core_finish_enable(vfio_pci_core.c:2536), pci_warn.
 *
 * 에러 경로: enable 실패는 그대로 반환(되돌릴 것이 없다). IGD 실패는
 * disable 로 되감은 뒤 반환.
 *
 * 호출 체인:
 *   사용자가 device fd 를 연다 -> vfio_main.c 의 vfio_df_open
 *     -> vtable 의 open_device -> [vfio_pci_open_device]
 *     -> vfio_pci_core_enable -> vfio_pci_igd_init
 *     -> vfio_pci_core_finish_enable */
static int vfio_pci_open_device(struct vfio_device *core_vdev)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 추상 디바이스에서 PCI 판 구조체를 복원한다. struct vfio_device 가
	 * struct vfio_pci_core_device 의 **첫 멤버**라는 배치를 이용한 관용 표현이며,
	 * 그 배치는 include/linux/vfio_pci_core.h:98~99 에 있다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	/* [한국어] IGD 판정과 경고 메시지에 쓸 실제 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 하위 호출의 반환값을 받을 자리. */
	int ret;

	/* [한국어] 장치를 실제로 켠다. 전원, 버스 마스터 해제, 초기 리셋, pci_saved_state 저장, config 그림자 생성, MSI-X 테이블 위치 파악이 모두 이 한 줄 안에서 일어난다. */
	ret = vfio_pci_core_enable(vdev);
	/* [한국어] 실패했는가. */
	if (ret)
		/* [한국어] 아직 아무것도 잡지 않은 상태이므로 그대로 올린다. */
		return ret;

	/* [한국어] 인텔 통합 그래픽인가. 아니면 이 블록을 통째로 건너뛰므로, 일반 장치에는 아무 추가 작업도 없다. */
	if (vfio_pci_is_intel_display(pdev)) {
		/* [한국어] IGD 전용 추가 region(OpRegion, 호스트 브리지 config 등)을 등록한다. finish_enable 보다 **먼저** 해야 그 region 들이 지도에 반영된다. */
		ret = vfio_pci_igd_init(vdev);
		/* [한국어] -ENODEV 는 "이 장치에는 그런 region 이 없다" 는 정상적인 답이므로
		 * 실패로 보지 않는다. 그 밖의 오류만 실패로 처리한다. */
		if (ret && ret != -ENODEV) {
			/* [한국어] 관리자가 원인을 알 수 있게 남긴다. */
			pci_warn(pdev, "Failed to setup Intel IGD regions\n");
			/* [한국어] **enable 이 잡은 것을 전부 되돌린다.** 이 한 줄이 없으면 장치가 켜진 채
			 * 소유자 없이 남는다. 이 파일에서 코어의 disable 을 부르는 유일한 자리다. */
			vfio_pci_core_disable(vdev);
			/* [한국어] IGD 오류를 그대로 올린다. */
			return ret;
		/* [한국어] IGD 오류 처리 끝. */
		}
	/* [한국어] IGD 블록 끝. */
	}

	/* [한국어] region 등록이 끝났으므로 지도를 확정한다. 여기서 BAR 마다 mmap 가능
	 * 여부(bar_mmap_supported)가 정해진다. 반환값이 없다 - 실패할 여지가 없는
	 * 후처리이기 때문이다. */
	vfio_pci_core_finish_enable(vdev);

	/* [한국어] 성공. */
	return 0;
}

/* [한국어] vfio_pci_ops - 사용자 공간에서 보이는 모든 동작의 진입표
 * (struct vfio_device_ops, 정의는 include/linux/vfio.h:116~146)
 *
 * 설정자: 컴파일 시점 상수다. const 이므로 런타임에 바뀌지 않는다.
 * 읽는 자: drivers/vfio/vfio_main.c 가 전부다. 파일 오퍼레이션(read/write/
 * mmap/ioctl)과 수명 콜백(init/release/open_device/close_device)이 이 표를
 * 거쳐 코어로 간다.
 * 값 범위: 스무 개 슬롯 중 열아홉 개가 다른 파일의 함수를 가리키고,
 * open_device 하나만 이 파일의 함수다.
 * 동기화: 없다. 읽기 전용 표다.
 *
 * 이 표가 이 파일의 존재 이유를 가장 잘 보여 준다 - vfio-pci 드라이버가
 * "하는 일" 은 사실상 이 표를 채워 코어에 건네는 것뿐이다. mlx5 나
 * nvgrace-gpu 같은 변종 드라이버는 이 표의 몇 슬롯을 자기 함수로 바꿔
 * 끼워 다른 동작을 만든다. */
static const struct vfio_device_ops vfio_pci_ops = {
	/* [한국어] 드라이버 이름 문자열.
	 * 설정자: 여기 상수로.
	 * 읽는 자: vfio_pci_core.c:6169 가 SR-IOV VF 를 가로챌 때 그 VF 의
	 * driver_override 에 이 이름을 심고, 6409줄이
	 * aperture_remove_conflicting_pci_devices 에 넘기고, 6722줄이 등록 시점에
	 * 이 슬롯이 비어 있지 않은지 WARN_ON 으로 확인한다.
	 * 값 범위: 널이 아닌 문자열. 여기서는 "vfio-pci" 이며, 이는 sysfs 의
	 * driver_override 에 쓰는 이름과 정확히 같아야 한다.
	 * 동기화: 없다. */
	.name		= "vfio-pci",
	/* [한국어] 장치 구조체를 처음 만들 때 불리는 초기화 콜백.
	 * 설정자: 여기.
	 * 읽는 자: drivers/vfio/vfio_main.c:921 의 _vfio_alloc_device 가 할당 직후
	 * 이 슬롯을 부른다. 즉 아래 probe 의 vfio_alloc_device 한 줄 안에서 실행된다.
	 * 값 범위: vfio_pci_core_init_dev(vfio_pci_core.c:6525). 뮤텍스와 목록들과
	 * rw_semaphore 를 초기화한다.
	 * 동기화: 아직 다른 스레드가 이 장치를 볼 수 없는 시점이라 잠금이 필요 없다. */
	.init		= vfio_pci_core_init_dev,
	/* [한국어] 장치 구조체가 마지막 참조를 잃었을 때 불리는 해제 콜백.
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 참조 해제 경로(vfio_put_device 의 뒷단).
	 * 값 범위: vfio_pci_core_release_dev(vfio_pci_core.c:6611). init 이 만든
	 * 것들을 되돌린다.
	 * 동기화: 마지막 참조가 사라진 뒤라 경쟁이 없다. */
	.release	= vfio_pci_core_release_dev,
	/* [한국어] 사용자가 device fd 를 열 때 불리는 콜백. **이 표에서 유일하게 이 파일의
	 * 함수를 가리키는 슬롯이다.**
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 vfio_df_open.
	 * 값 범위: 바로 위의 vfio_pci_open_device. 코어의 enable 과 finish_enable
	 * 사이에 인텔 IGD region 등록을 끼워 넣는다.
	 * 동기화: vfio_main.c 가 device 잠금 아래에서 부른다. */
	.open_device	= vfio_pci_open_device,
	/* [한국어] 마지막 사용자가 fd 를 닫을 때 불리는 콜백.
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 vfio_df_close.
	 * 값 범위: vfio_pci_core_close_device(vfio_pci_core.c:2460). ioeventfd 목록을
	 * 비우고, 장치를 disable 하고, 필요하면 리셋한다. open_device 와 달리 이
	 * 파일이 끼워 넣을 것이 없어 코어를 그대로 가리킨다.
	 * 동기화: vfio_main.c 가 device 잠금 아래에서 부른다. */
	.close_device	= vfio_pci_core_close_device,
	/* [한국어] VFIO_DEVICE_* 계열 ioctl 의 진입점.
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 ioctl 파일 오퍼레이션.
	 * 값 범위: vfio_pci_core_ioctl(vfio_pci_core.c:4590). GET_INFO,
	 * GET_IRQ_INFO, GET_PCI_HOT_RESET_INFO, IOEVENTFD, PCI_HOT_RESET, RESET,
	 * SET_IRQS 일곱 개를 디스패치한다.
	 * 동기화: 각 하위 처리기가 필요한 잠금을 스스로 잡는다. */
	.ioctl		= vfio_pci_core_ioctl,
	/* [한국어] region 정보 조회에 붙일 capability 를 채우는 콜백.
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 GET_REGION_INFO 처리기.
	 * 값 범위: vfio_pci_ioctl_get_region_info(vfio_pci_core.c:3529).
	 * **무엇이 mmap 가능한지가 정해지는 지점**이며, MSI-X 테이블이 든 BAR 에
	 * VFIO_REGION_INFO_CAP_MSIX_MAPPABLE 을 붙일지도 여기서 정한다.
	 * 동기화: 읽기만 한다. */
	.get_region_info_caps = vfio_pci_ioctl_get_region_info,
	/* [한국어] VFIO_DEVICE_FEATURE ioctl 의 진입점(저전력, 마이그레이션, dma-buf 등).
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 feature 처리기.
	 * 값 범위: vfio_pci_core_ioctl_feature(vfio_pci_core.c:4754).
	 * 동기화: 기능별로 다르다. 코어가 처리한다. */
	.device_feature = vfio_pci_core_ioctl_feature,
	/* [한국어] read(2) 콜백.
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 파일 오퍼레이션.
	 * 값 범위: vfio_pci_core_read(vfio_pci_core.c:4958). 그것이 vfio_pci_rw 로
	 * 가고, region 번호에 따라 config 는 vfio_pci_config.c 로, BAR 와 ROM 은
	 * vfio_pci_rdwr.c 의 vfio_pci_bar_rw 로 갈린다.
	 * 동기화: 아래 계층이 memory_lock 을 read 로 잡는다. */
	.read		= vfio_pci_core_read,
	/* [한국어] write(2) 콜백.
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 파일 오퍼레이션.
	 * 값 범위: vfio_pci_core_write(vfio_pci_core.c:5007). read 와 같은 경로를
	 * iswrite=true 로 탄다.
	 * 동기화: read 와 같다. */
	.write		= vfio_pci_core_write,
	/* [한국어] mmap(2) 콜백. **read/write 와 달리 여기서 만든 매핑은 중재 없이
	 * 하드웨어에 직통한다.**
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 의 파일 오퍼레이션.
	 * 값 범위: vfio_pci_core_mmap(vfio_pci_core.c:5561). VM_PFNMAP 으로 BAR 의
	 * 물리 페이지를 그대로 걸고, 실제 PFN 삽입은 폴트 때 한다.
	 * 동기화: 폴트 경로가 memory_lock 을 read 로 잡는다. */
	.mmap		= vfio_pci_core_mmap,
	/* [한국어] 커널이 사용자에게 "장치를 놓아 달라" 고 요청할 때 불리는 콜백.
	 * 설정자: 여기.
	 * 읽는 자: vfio_main.c 가 드라이버 언바인드 등으로 장치를 회수해야 할 때.
	 * 값 범위: vfio_pci_core_request(vfio_pci_core.c:5733). req_trigger
	 * eventfd 를 울려 사용자에게 알린다.
	 * 동기화: 코어가 igate 를 잡는다. */
	.request	= vfio_pci_core_request,
	/* [한국어] 1세대 group fd 경로에서 이름으로 장치를 찾을 때 쓰는 콜백.
	 * 설정자: 여기.
	 * 읽는 자: drivers/vfio/group.c 의 vfio_device_get_from_name.
	 * 값 범위: vfio_pci_core_match(vfio_pci_core.c:6008). PCI 주소 문자열을
	 * 비교하고, SR-IOV 의 vf_token 도 이 문법으로 받는다.
	 * 동기화: 코어가 vf_token 잠금을 잡는다. */
	.match		= vfio_pci_core_match,
	/* [한국어] 2세대 cdev 경로에서 vf_token UUID 를 확인할 때 쓰는 콜백.
	 * 설정자: 여기.
	 * 읽는 자: drivers/vfio/device_cdev.c 의 vfio_df_check_token.
	 * 값 범위: vfio_pci_core_match_token_uuid(vfio_pci_core.c:5823). 1세대와
	 * 같은 신뢰 협약을 다른 문법으로 받는다.
	 * 동기화: 코어가 vf_token 잠금을 잡는다. */
	.match_token_uuid = vfio_pci_core_match_token_uuid,
	/* [한국어] 이 장치를 iommufd 컨텍스트에 묶는 콜백.
	 * 설정자: 여기.
	 * 읽는 자: drivers/vfio/iommufd.c 의 vfio_iommufd_bind.
	 * 값 범위: vfio_iommufd_physical_bind(drivers/vfio/iommufd.c:318).
	 * "physical" 판은 진짜 PCI 함수를 IOMMU 에 직접 붙이는 판이며, 소프트웨어
	 * 에뮬레이션 장치가 쓰는 "emulated" 판과 구별된다.
	 * 동기화: iommufd 쪽이 잡는다. */
	.bind_iommufd	= vfio_iommufd_physical_bind,
	/* [한국어] 그 짝. 묶음을 푼다.
	 * 설정자: 여기.
	 * 읽는 자: iommufd.c 의 해제 경로.
	 * 값 범위: vfio_iommufd_physical_unbind(drivers/vfio/iommufd.c:342).
	 * 동기화: iommufd 쪽이 잡는다. */
	.unbind_iommufd	= vfio_iommufd_physical_unbind,
	/* [한국어] 이 장치를 IOAS(사용자가 소유한 주소 공간)에 붙이는 콜백. **장치의
	 * DMA 가 그 순간부터 사용자의 IOVA 로 번역된다.**
	 * 설정자: 여기.
	 * 읽는 자: iommufd.c 의 attach 경로.
	 * 값 범위: vfio_iommufd_physical_attach_ioas(drivers/vfio/iommufd.c:373).
	 * 동기화: iommufd 쪽이 잡는다. */
	.attach_ioas	= vfio_iommufd_physical_attach_ioas,
	/* [한국어] 그 짝. IOAS 에서 뗀다.
	 * 설정자: 여기.
	 * 읽는 자: iommufd.c 의 detach 경로.
	 * 값 범위: vfio_iommufd_physical_detach_ioas(drivers/vfio/iommufd.c:402).
	 * 동기화: iommufd 쪽이 잡는다. */
	.detach_ioas	= vfio_iommufd_physical_detach_ioas,
	/* [한국어] PASID 단위로 IOAS 에 붙이는 콜백. 한 장치가 여러 주소 공간을 동시에
	 * 쓰는 경우(SVA 계열)를 위한 것이다.
	 * 설정자: 여기.
	 * 읽는 자: iommufd.c 의 PASID attach 경로.
	 * 값 범위: vfio_iommufd_physical_pasid_attach_ioas(drivers/vfio/iommufd.c:431).
	 * 동기화: iommufd 쪽이 잡는다. */
	.pasid_attach_ioas	= vfio_iommufd_physical_pasid_attach_ioas,
	/* [한국어] 그 짝.
	 * 설정자: 여기.
	 * 읽는 자: iommufd.c 의 PASID detach 경로.
	 * 값 범위: vfio_iommufd_physical_pasid_detach_ioas(drivers/vfio/iommufd.c:461).
	 * 동기화: iommufd 쪽이 잡는다.
	 * 표 끝. dma_unmap 슬롯은 비워 두었는데, 그것은 사용자 페이지를 핀해 두는
	 * 에뮬레이션 장치가 매핑 해제 통보를 받으려고 채우는 슬롯이라 진짜 PCI
	 * 함수에는 필요가 없다. */
	.pasid_detach_ioas	= vfio_iommufd_physical_pasid_detach_ioas,
};

/* [한국어] vfio_pci_dev_ops - PCI 계층 안에서만 쓰는 작은 vtable
 * (struct vfio_pci_device_ops, 정의는 include/linux/vfio_pci_core.h:60~67)
 *
 * 설정자: 컴파일 시점 상수. 아래 probe 가 vdev->pci_ops 에 이 표의 주소를
 * 넣는다.
 * 읽는 자: drivers/vfio/pci/vfio_pci_dmabuf.c:232 가 이 포인터와 슬롯이
 * 모두 비어 있지 않은지 확인한 뒤, 275줄에서 슬롯을 부른다.
 * 값 범위: 슬롯 하나뿐이다.
 * 동기화: 없다. 읽기 전용 표다.
 *
 * 위 vfio_pci_ops 와 달리 이 표는 **사용자 공간에 보이지 않는다**. vfio 코어가
 * 아니라 vfio-pci 안쪽에서만 쓰이는 확장점이며, 변종 드라이버가 BAR 의 물리
 * 주소를 다르게 계산해야 할 때 여기를 갈아 끼운다. */
static const struct vfio_pci_device_ops vfio_pci_dev_ops = {
	/* [한국어] dma-buf 로 내줄 메모리의 물리 주소 범위를 알아내는 콜백.
	 * 설정자: 여기.
	 * 읽는 자: vfio_pci_dmabuf.c:275. 그 직전 232줄에서 pci_ops 와 이 슬롯이
	 * 모두 채워져 있는지 확인하므로, 비워 두어도 안전하다.
	 * 값 범위: vfio_pci_core_get_dmabuf_phys(vfio_pci_dmabuf.c:172). 평범한 PCI
	 * 함수의 BAR 물리 주소를 그대로 돌려주는 기본 구현이다.
	 * 동기화: 코어가 필요한 잠금을 잡는다.
	 * 이 슬롯이 있는 덕분에 BAR 를 P2P DMA 대상으로 다른 장치에 내줄 수 있고,
	 * NVMe 관점에서는 GPU 가 SSD 의 BAR 에 직접 DMA 하는 구성이 여기에 해당한다. */
	.get_dmabuf_phys = vfio_pci_core_get_dmabuf_phys,
};

/* [한국어] vfio_pci_probe - PCI 함수 하나를 붙잡아 vfio 코어에 넘긴다
 *
 * @pdev: 드라이버 코어가 이 드라이버에 배정한 PCI 함수.
 * @id: 대조에 성공한 id_table 항목(또는 driver_override 경로에서는
 *   drivers/pci/pci-driver.c:362 의 만능 더미 pci_device_id_any 의 주소).
 *   **이 함수는 그 값을 쓰지 않는다** - 정적 표가 와일드카드 하나뿐이고
 *   아래 vfio_pci_fill_ids 도 driver_data 를 0 으로 넣기 때문에 실어 보낼
 *   정보가 없다. 인자로 받는 것은 PCI 드라이버 probe 의 규약이기 때문이다.
 * @return: 0 = 성공, -EINVAL = denylist 에 걸림, 그 밖에는 할당이나 코어
 *   등록이 올린 음수 errno.
 *
 * 왜 필요한가: PCI 드라이버 모델의 바인딩 진입점이다. 다만 이 함수가 직접
 * 장치를 만지지는 않는다 - 껍데기를 씌워 코어에 넘기는 네 단계뿐이다.
 *
 * 동작 단계:
 *  (1) denylist 검사. 이 파일만의 정책이며 코어는 모른다.
 *  (2) vfio_alloc_device 로 struct vfio_pci_core_device 를 통째로 할당한다.
 *      그 매크로(include/linux/vfio.h:327)가 vfio_main.c:921 의
 *      _vfio_alloc_device 를 부르고, **그 안에서 vtable 의 init 슬롯 즉
 *      vfio_pci_core_init_dev 가 이미 실행된다.** 그래서 이 줄 하나로
 *      뮤텍스와 목록 초기화까지 끝난다.
 *  (3) drvdata 에 주소를 심고 pci_ops 표를 물린다. remove 와
 *      sriov_configure 가 나중에 이 포인터로 구조체를 되찾는다.
 *  (4) vfio_pci_core_register_device 로 코어에 등록한다. device set 배정,
 *      마이그레이션 vtable 완비성 검사, vfio_register_group_dev 가 모두
 *      그 안에서 일어난다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 드라이버 코어의 바인딩 경로
 * (drivers/pci/pci-driver.c:757 의 local_pci_probe)에서 불린다. 잠들 수 있다.
 * 호출자: drivers/pci/pci-driver.c 의 pci_device_probe -> local_pci_probe.
 * 호출 대상: vfio_pci_is_denylisted(이 파일),
 * _vfio_alloc_device(drivers/vfio/vfio_main.c:921),
 * vfio_pci_core_register_device(vfio_pci_core.c:6701),
 * vfio_put_device(include/linux/vfio.h 의 인라인).
 *
 * 에러 경로: denylist 는 할당 전이라 그냥 반환한다. 할당 실패는 포인터에
 * 실린 오류를 꺼내 반환한다. 등록 실패만 out_put_vdev 로 가서 참조를 놓는다.
 * [상류 코드 관찰] 그 되감기 경로는 (3)에서 심은 drvdata 를 지우지 않아,
 * probe 가 실패한 뒤에도 pdev 의 drvdata 가 해제될 구조체를 가리킨 채 남는다.
 * 실제로 문제가 되려면 바인딩되지 않은 장치에서 remove 나 sriov_configure 가
 * 불려야 하는데 그런 경로는 없고, 드라이버 코어가 probe 실패 시 drvdata 를
 * 지우는지는 이 트리에 drivers/base 가 없어 확인 못 함.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   사용자의 sysfs bind -> pci_bus_match -> pci_match_device(pci-driver.c:430)
 *     -> pci_device_probe -> local_pci_probe(pci-driver.c:757)
 *     -> [vfio_pci_probe] -> _vfio_alloc_device -> vfio_pci_core_init_dev
 *     -> vfio_pci_core_register_device -> vfio_register_group_dev */
static int vfio_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 할당해 만들 vfio-pci 디바이스 구조체. */
	struct vfio_pci_core_device *vdev;
	/* [한국어] 코어 등록의 반환값. */
	int ret;

	/* [한국어] 위험 목록 판정. 여기가 이 파일의 유일한 정책 결정 지점이다. */
	if (vfio_pci_is_denylisted(pdev))
		/* [한국어] -EINVAL 로 바인딩을 거부한다. 아무것도 할당하지 않았으므로 되돌릴 것이 없다. */
		return -EINVAL;

	/* [한국어] struct vfio_pci_core_device 를 할당한다. 첫 두 인자가 "이 타입의 vdev
	 * 멤버가 struct vfio_device 다" 라는 정보이고, 셋째가 소유 device, 넷째가
	 * 위에서 만든 vtable 이다. 매크로의 실체는 include/linux/vfio.h:327 이며
	 * vfio_main.c:921 의 _vfio_alloc_device 를 부른다. */
	vdev = vfio_alloc_device(vfio_pci_core_device, vdev, &pdev->dev,
				 /* [한국어] vtable 을 건네는 순간 코어가 init 슬롯을 불러 구조체를 초기화한다. */
				 &vfio_pci_ops);
	/* [한국어] 할당 실패는 NULL 이 아니라 오류 포인터로 온다. */
	if (IS_ERR(vdev))
		/* [한국어] 포인터에 실린 errno 를 꺼내 반환한다. */
		return PTR_ERR(vdev);

	/* [한국어] 구조체 주소를 pci_dev 에 매어 둔다. **이 한 줄이 remove 와
	 * sriov_configure 가 나중에 구조체를 되찾는 유일한 통로다.** */
	dev_set_drvdata(&pdev->dev, vdev);
	/* [한국어] PCI 계층 안쪽 vtable 을 물린다. vfio_pci_dmabuf.c:232 가 이 포인터를 검사하므로, 비워 두면 dma-buf 기능이 조용히 꺼진다. */
	vdev->pci_ops = &vfio_pci_dev_ops;
	/* [한국어] 코어에 등록한다. 이 호출이 성공하는 순간 /dev/vfio 아래에서 사용자에게 보이기 시작한다. */
	ret = vfio_pci_core_register_device(vdev);
	/* [한국어] 등록 실패. */
	if (ret)
		/* [한국어] 참조를 놓는 되감기 경로로 간다. */
		goto out_put_vdev;
	/* [한국어] 성공. 되감기 코드를 건너뛰려고 여기서 바로 반환한다. */
	return 0;

/* [한국어] 등록 실패 되감기 지점. */
out_put_vdev:
	/* [한국어] 할당 때 얻은 참조를 놓는다. 마지막 참조라면 vtable 의 release 슬롯
	 * (vfio_pci_core_release_dev)이 불려 init 이 만든 것들을 되돌리고 구조체가
	 * 해제된다. */
	vfio_put_device(&vdev->vdev);
	/* [한국어] 등록 실패의 errno 를 그대로 올린다. 드라이버 코어가 이 값을 보고 바인딩을 실패로 처리한다. */
	return ret;
}

/* [한국어] vfio_pci_remove - 바인딩을 끊고 구조체를 놓는다
 *
 * @pdev: 언바인드되는 PCI 함수.
 * @return: 없음. PCI 드라이버 모델의 remove 는 실패할 수 없다.
 *
 * 왜 필요한가: probe 가 만든 두 가지(코어 등록, 구조체 참조)를 역순으로
 * 되돌리기 위해서다. 그 이상은 없다 - 장치를 실제로 끄고 리셋하는 일은
 * 사용자가 fd 를 닫을 때 이미 일어났거나, 아래 unregister 안에서 코어가
 * 사용자를 쫓아내며 처리한다.
 *
 * 동작 단계: (1) drvdata 로 구조체를 되찾는다. (2) 코어에서 등록을 해제한다 -
 * 그 안에서 아직 열려 있는 사용자가 있으면 request 콜백으로 놓아 달라고
 * 요청하고 기다린다. (3) probe 가 얻은 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(sysfs unbind 또는 장치 제거 경로). 잠들 수 있다.
 * 호출자: drivers/pci/pci-driver.c 의 pci_device_remove.
 * 호출 대상: dev_get_drvdata,
 * vfio_pci_core_unregister_device(vfio_pci_core.c:6907),
 * vfio_put_device(include/linux/vfio.h 의 인라인).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자의 sysfs unbind -> pci_device_remove -> [vfio_pci_remove]
 *     -> vfio_pci_core_unregister_device -> vfio_put_device */
static void vfio_pci_remove(struct pci_dev *pdev)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] probe 가 심어 둔 포인터를 되찾는다. probe 가 성공했어야만 이 콜백이 불리므로 NULL 검사가 없다. */
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);

	/* [한국어] 코어에서 등록을 해제한다. 사용자가 아직 열고 있다면 여기서 회수 절차가 돈다. */
	vfio_pci_core_unregister_device(vdev);
	/* [한국어] probe 가 얻은 참조를 놓는다. 마지막 참조면 release 슬롯이 불리고 구조체가 사라진다. */
	vfio_put_device(&vdev->vdev);
}

/* [한국어] vfio_pci_sriov_configure - SR-IOV VF 개수 설정 요청을 걸러 코어로 넘긴다
 *
 * @pdev: SR-IOV PF. 이 드라이버에 바인딩돼 있어야 한다.
 * @nr_virtfn: 만들 VF 개수. 0 이면 전부 없앤다.
 * @return: 0 이상이면 성공(대개 만든 개수), -ENOENT = 모듈 파라미터로
 *   기능이 꺼져 있음, 그 밖에는 코어가 올린 음수 errno.
 *
 * 왜 필요한가: PF 를 사용자 공간이 몰고 있는 상태에서 VF 를 만드는 것은
 * 위험할 수 있다. 유저스페이스 PF 드라이버의 협조 없이 VF 를 켜면 VF 도
 * PF 도 제대로 동작하지 않을 수 있다는 것이 enable_sriov 파라미터 설명문의
 * 경고다. 그래서 기본값을 꺼짐으로 두고, 이 함수가 **문지기 역할만** 한다.
 *
 * 동작 단계: (1) drvdata 로 구조체를 되찾는다. (2) 파라미터가 꺼져 있으면
 * -ENOENT. (3) 켜져 있으면 코어에 그대로 넘긴다.
 *
 * 파라미터를 **매 호출마다 다시 읽는다**는 점이 코어로 복사되는 다른 셋과
 * 다르다. 그래서 sysfs 로 enable_sriov 를 바꾸면 즉시 반영된다.
 * CONFIG_PCI_IOV 가 꺼진 커널에서는 module_param 등록이 사라져 변수 값이
 * 언제나 false 이므로, 이 함수는 항상 -ENOENT 를 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. sysfs 의 sriov_numvfs 쓰기에서 시작해
 * drivers/pci/iov.c 가 드라이버의 이 슬롯을 부른다(iov.c:33 과 1129 의 설명).
 * 호출자: drivers/pci/iov.c 의 sriov_numvfs_store 경로.
 * 호출 대상: dev_get_drvdata,
 * vfio_pci_core_sriov_configure(vfio_pci_core.c:7059).
 *
 * 에러 경로: -ENOENT 는 여기서 만들고, 나머지는 코어가 만든다.
 *
 * 호출 체인:
 *   sysfs 의 sriov_numvfs 쓰기 -> drivers/pci/iov.c
 *     -> [vfio_pci_sriov_configure] -> vfio_pci_core_sriov_configure */
static int vfio_pci_sriov_configure(struct pci_dev *pdev, int nr_virtfn)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] probe 가 심어 둔 구조체를 되찾는다. */
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);

	/* [한국어] 기능이 꺼져 있는가. 기본값이 꺼짐이므로 관리자가 명시적으로 켜야 한다. */
	if (!enable_sriov)
		/* [한국어] -ENOENT 를 고른 것은 "이 드라이버는 SR-IOV 설정을 제공하지 않는다" 는
		 * 뜻에 가깝기 때문이다. -EPERM 이나 -EINVAL 이 아니라는 점이 sysfs 사용자에게
		 * "권한 문제도 인자 문제도 아니다" 를 알려 준다. */
		return -ENOENT;

	/* [한국어] 실제 일은 코어가 한다. 코어가 PF/VF 신뢰 관계와 vf_token 을 챙긴 뒤 pci_enable_sriov 나 pci_disable_sriov 를 부른다. */
	return vfio_pci_core_sriov_configure(vdev, nr_virtfn);
}

/* [한국어] vfio_pci_table - 이 드라이버의 정적 ID 표
 * (struct pci_device_id 배열)
 *
 * 설정자: 컴파일 시점 상수.
 * 읽는 자: drivers/pci/pci-driver.c:455 의 pci_match_device 가 훑는다.
 * 값 범위: 유효 항목 하나와 끝 표시 하나.
 * 동기화: 없다. 읽기 전용이다.
 *
 * **이 표가 이 드라이버의 바인딩 정책 전부다.** 항목이 모든 장치와 형식상
 * 맞지만 override 전용 표시가 붙어 있어, 사용자가 driver_override 로 이
 * 드라이버를 지목했을 때만 인정된다. 그래서 vfio-pci 는 부팅 중에 아무
 * 장치도 낚아채지 않는다. */
static const struct pci_device_id vfio_pci_table[] = {
	/* [한국어] 유일한 유효 항목.
	 * 설정자: 여기.
	 * 읽는 자: pci_match_device(drivers/pci/pci-driver.c:430).
	 * 값 범위: vendor 와 device 가 모두 PCI_ANY_ID(와일드카드)이고, 매크로가
	 * 항목의 override_only 필드를 세운다. 그 필드를 해석하는 곳이
	 * drivers/pci/pci-driver.c:462 이며, 거기서 driver_override 로 지목된
	 * 경우(ret > 0)에만 이 항목을 인정한다. 매크로 자체의 정의는
	 * include/linux/pci.h 에 있고 이 트리에는 그 헤더가 없어 확인 못 함.
	 * 동기화: 없다.
	 * 오른쪽 영어 주석의 "match all by default" 는 **형식상** 모두와 맞는다는
	 * 뜻이지 실제로 모두에 바인딩된다는 뜻이 아니다 - override 표시가 그것을
	 * 막는다. */
	{ PCI_DRIVER_OVERRIDE_DEVICE_VFIO(PCI_ANY_ID, PCI_ANY_ID) }, /* match all by default */
	/* [한국어] 표의 끝 표시. 모든 필드가 0 인 빈 항목이다.
	 * 설정자: 여기.
	 * 읽는 자: pci_match_id 의 순회 종료 조건(drivers/pci/pci-driver.c:351 이
	 * vendor, subvendor, class_mask 가 모두 0 인지로 판단한다).
	 * 값 범위: 전부 0.
	 * 동기화: 없다. */
	{}
};

/* [한국어] 이 표를 모듈 메타데이터에 심는다. depmod 가 modules.alias 를 만들 때
 * 쓰고, udev 가 그것을 보고 자동 적재를 결정한다. 다만 항목이 override
 * 전용이라 실제 자동 적재로 이어지지는 않으며, 표를 노출하는 형식적 선언에
 * 가깝다. */
MODULE_DEVICE_TABLE(pci, vfio_pci_table);

/* [한국어] vfio_pci_driver - PCI 드라이버 코어에 등록하는 서술자
 * (struct pci_driver)
 *
 * 설정자: 컴파일 시점 상수이지만 const 가 아니다 - 드라이버 코어가 내부
 * 필드(dynids 목록, embedded struct device_driver)를 런타임에 갱신하기
 * 때문이다.
 * 읽는 자: drivers/pci/pci-driver.c 의 바인딩/해제 경로 전체와,
 * 이 파일의 vfio_pci_fill_ids(pci_add_dynid 의 대상), vfio_pci_init,
 * vfio_pci_cleanup.
 * 값 범위: 아래 일곱 슬롯.
 * 동기화: dynids 목록만 자체 spinlock 으로 보호된다
 * (drivers/pci/pci-driver.c:262 과 443). */
static struct pci_driver vfio_pci_driver = {
	/* [한국어] 드라이버 이름.
	 * 설정자: 여기.
	 * 읽는 자: 드라이버 코어. /sys/bus/pci/drivers/vfio-pci 라는 경로가 이
	 * 문자열에서 나오고, driver_override 에 써야 하는 이름도 이것이다.
	 * 값 범위: "vfio-pci". 위 vfio_pci_ops 의 name 슬롯과 같은 값이어야 한다 -
	 * 코어가 SR-IOV VF 를 가로챌 때 그 값을 driver_override 에 심기 때문이다
	 * (vfio_pci_core.c:6169).
	 * 동기화: 없다. */
	.name			= "vfio-pci",
	/* [한국어] 정적 ID 표의 주소.
	 * 설정자: 여기.
	 * 읽는 자: pci_match_device(drivers/pci/pci-driver.c:455).
	 * 값 범위: 위 vfio_pci_table. override 전용 항목 하나뿐이다.
	 * 동기화: 없다. */
	.id_table		= vfio_pci_table,
	/* [한국어] 바인딩 콜백.
	 * 설정자: 여기.
	 * 읽는 자: drivers/pci/pci-driver.c:757 의 local_pci_probe.
	 * 값 범위: 위 vfio_pci_probe.
	 * 동기화: 드라이버 코어가 device 잠금 아래에서 부른다. */
	.probe			= vfio_pci_probe,
	/* [한국어] 언바인드 콜백.
	 * 설정자: 여기.
	 * 읽는 자: pci_device_remove.
	 * 값 범위: 위 vfio_pci_remove.
	 * 동기화: 드라이버 코어가 device 잠금 아래에서 부른다. */
	.remove			= vfio_pci_remove,
	/* [한국어] SR-IOV VF 개수 설정 콜백.
	 * 설정자: 여기.
	 * 읽는 자: drivers/pci/iov.c 의 sysfs 경로(iov.c:1129 의 설명).
	 * 값 범위: 위 vfio_pci_sriov_configure. 이 슬롯이 비어 있으면 sysfs 의
	 * sriov_numvfs 쓰기 자체가 거부되므로, 기능을 꺼 두고 싶어도 함수는 두고
	 * 그 안에서 -ENOENT 를 돌려주는 방식을 택했다.
	 * 동기화: 코어가 잡는다. */
	.sriov_configure	= vfio_pci_sriov_configure,
	/* [한국어] PCI 오류 복구 콜백 표의 주소.
	 * 설정자: 여기.
	 * 읽는 자: PCI AER 복구 경로.
	 * 값 범위: vfio_pci_core_err_handlers(vfio_pci_core.c:7172). 이 파일이
	 * 가리키기만 하고 표 자체는 코어가 정의한다. 그 표는 복구를 직접 하지 않고
	 * err_trigger eventfd 로 사용자에게 넘긴다 - 즉 **정책이 사용자 공간에 있다.**
	 * 동기화: 코어가 잡는다. */
	.err_handler		= &vfio_pci_core_err_handlers,
	/* [한국어] 이 드라이버가 DMA 를 스스로 관리한다는 선언.
	 * 설정자: 여기.
	 * 읽는 자: drivers/pci/pci-driver.c:3623 과 3655.
	 * 값 범위: true. 이 값이 참이면 드라이버 코어가 이 장치를 기본 IOMMU
	 * 도메인에 자동으로 붙이지 않는다 - vfio 가 사용자 소유의 IOAS 에 직접
	 * 붙일 것이기 때문이다. 거짓으로 두면 코어가 먼저 붙여 버려 vfio 가 원하는
	 * 주소 공간을 만들 수 없다.
	 * 동기화: 없다.
	 * 표 끝. shutdown, suspend, resume 슬롯은 비워 두었다 - 장치를 실제로 몰고
	 * 있는 것은 사용자 공간이므로 커널이 전원 전이를 대신 처리할 수 없다. */
	.driver_managed_dma	= true,
};

/* [한국어] vfio_pci_fill_ids - ids 모듈 파라미터 문자열을 파싱해 동적 ID 로 추가한다
 *
 * @return: 없음. 실패는 전부 경고 메시지로만 보고한다.
 *
 * 왜 필요한가: 이 드라이버의 정적 표는 override 전용이라 아무것도 자동으로
 * 붙잡지 않는다. 그런데 **동적 ID 는 override 검사를 받지 않는다** -
 * pci_match_device 가 정적 표보다 dynids 목록을 먼저 훑고 거기서 맞으면
 * 그대로 인정하기 때문이다(drivers/pci/pci-driver.c:443~454). 그래서 부팅
 * 시점에 특정 장치를 vfio-pci 에 자동으로 붙이고 싶으면 sysfs 를 손대는
 * 대신 이 파라미터를 쓰면 된다. 초기 램디스크에서 sysfs 스크립트를 돌리기
 * 어려운 환경을 위한 통로다.
 *
 * 동작 단계: (1) 빈 문자열이면 즉시 반환. (2) strsep 으로 쉼표마다 잘라
 * 가며 반복한다. (3) 각 토막을 sscanf 로 최대 여섯 개의 16진수 필드로
 * 읽는다. (4) 최소 vendor 와 device 둘은 있어야 하며, 없으면 경고만 찍고
 * 그 토막을 건너뛴다. (5) pci_add_dynid 로 목록에 추가한다. 그 함수가
 * 목록에 넣은 뒤 driver_attach 를 불러 **등록된 모든 PCI 장치를 즉시 다시
 * 대조**하므로, 이 줄에서 곧바로 probe 가 돌 수 있다.
 *
 * 실행 컨텍스트: 모듈 초기화(프로세스 문맥). __init 이라 초기화가 끝나면
 * 코드 자체가 회수된다. 그 표시가 있어야 __initdata 인 ids 배열을 참조해도
 * 섹션 불일치 경고가 나지 않는다.
 * 호출자: vfio_pci_init 하나뿐. **pci_register_driver 뒤에** 불린다는 순서가
 * 중요하다 - pci_add_dynid 안의 driver_attach 는 드라이버가 이미 등록돼
 * 있어야 의미가 있다(drivers/pci/pci-driver.c:242 의 주석이 그 조건을 명시).
 * 호출 대상: strsep, strlen, sscanf,
 * pci_add_dynid(drivers/pci/pci-driver.c:242), pr_warn, pr_info.
 *
 * 에러 경로: 파싱 실패도 추가 실패도 경고만 남기고 다음 토막으로 넘어간다.
 * 반환값이 void 라 호출자는 결과를 알 수 없고, 모듈 적재는 어쨌든 성공한다.
 *
 * 호출 체인:
 *   modprobe vfio-pci ids=... -> vfio_pci_init -> [vfio_pci_fill_ids]
 *     -> pci_add_dynid(pci-driver.c:242) -> driver_attach -> vfio_pci_probe */
static void __init vfio_pci_fill_ids(void)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] p 는 strsep 이 갱신할 순회 커서, id 는 잘라 낸 한 토막이다. */
	char *p, *id;
	/* [한국어] pci_add_dynid 의 반환값. */
	int rc;

	/* [한국어] 아래 영어 주석대로, 파라미터가 실제로 주어지지 않은 경우다. */
	/* no ids passed actually */
	/* [한국어] 첫 바이트가 널이면 빈 문자열이다. 배열이 __initdata 로 0 초기화돼 있으므로 파라미터가 없으면 항상 이 조건이 참이다. */
	if (ids[0] == '\0')
		/* [한국어] 할 일이 없다. */
		return;

	/* [한국어] 아래 영어 주석대로, 모듈 파라미터로 지정된 ID 들을 추가한다. */
	/* add ids specified in the module parameter */
	/* [한국어] 커서를 문자열 처음에 놓는다. 아래 strsep 이 이 배열을 **제자리에서 변형**해 쉼표를 널로 바꾼다. __initdata 라 어차피 곧 버려질 메모리이므로 파괴적 파싱이 문제되지 않는다. */
	p = ids;
	/* [한국어] 쉼표를 구분자로 한 토막씩 떼어 낸다. strsep 은 strtok 과 달리 연속된
	 * 구분자를 빈 토막으로 돌려주고 커서를 인자로 받아 재진입이 안전하다.
	 * 더 이상 토막이 없으면 NULL 을 돌려줘 루프가 끝난다. */
	while ((id = strsep(&p, ","))) {
		/* [한국어] 한 토막이 담을 여섯 개의 값. vendor 와 device 는 초기값이 없다 -
		 * sscanf 가 최소 둘은 채웠는지 아래에서 확인하기 때문이다. subvendor 와 */
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			/* [한국어] subdevice 는 PCI_ANY_ID(와일드카드)로, class 와 class_mask 는 0 으로
			 * 시작한다. 사용자가 두 개만 적으면 나머지는 이 초기값 그대로 쓰인다.
			 * class_mask 가 0 이면 클래스 비교에서 아무 비트도 보지 않으므로
			 * (drivers/pci/pci-driver.c:215 의 설명대로 mask 비트가 1 인 자리만 비교한다)
			 * 클래스 조건이 사실상 없는 것과 같다 - 즉 class 만 적고 class_mask 를
			 * 생략하면 그 class 값은 아무 효과가 없다. */
			subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
		/* [한국어] sscanf 가 실제로 채운 필드 수. */
		int fields;

		/* [한국어] 빈 토막(쉼표가 연달아 있거나 문자열이 쉼표로 끝난 경우). */
		if (!strlen(id))
			/* [한국어] 조용히 건너뛴다. 경고도 찍지 않는다 - 사람이 쓴 목록에 흔한 실수라 시끄러울 이유가 없다. */
			continue;

		/* [한국어] 콜론으로 구분된 16진수를 최대 여섯 개까지 읽는다. sscanf 는 채운
		 * 개수를 돌려주므로, 사용자가 앞에서부터 몇 개를 적었는지 그 값으로 알 수
		 * 있다. %x 라 0x 접두사 없이 적는다. */
		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				/* [한국어] 앞의 넷은 vendor:device:subvendor:subdevice, */
				&vendor, &device, &subvendor, &subdevice,
				/* [한국어] 뒤의 둘은 class 와 class_mask 다. */
				&class, &class_mask);

		/* [한국어] 최소한 vendor 와 device 둘은 있어야 의미 있는 ID 가 된다. */
		if (fields < 2) {
			/* [한국어] 무엇이 잘못됐는지 알 수 있게 원문 토막을 그대로 찍는다. */
			pr_warn("invalid id string \"%s\"\n", id);
			/* [한국어] 그 토막만 버리고 다음으로 넘어간다. 나머지 목록은 계속 처리한다. */
			continue;
		/* [한국어] 파싱 실패 처리 끝. */
		}

		/* [한국어] 동적 ID 목록에 추가한다. 첫 인자가 이 파일의 드라이버 서술자이므로,
		 * 추가된 ID 는 이 드라이버의 dynids 목록에 매달린다. */
		rc = pci_add_dynid(&vfio_pci_driver, vendor, device,
				   /* [한국어] 마지막 인자 0 은 driver_data 다 - probe 에 실어 보낼 사설 값이 없다는
				    * 뜻이며, 그래서 vfio_pci_probe 의 id 인자가 쓰이지 않는다.
				    * 이 호출 안에서 driver_attach 가 돌아 곧바로 probe 가 일어날 수 있다. */
				   subvendor, subdevice, class, class_mask, 0);
		/* [한국어] 추가 실패(대개 -ENOMEM). */
		if (rc)
			/* [한국어] 어떤 ID 를 못 넣었는지와 errno 를 함께 남긴다. */
			pr_warn("failed to add dynamic id [%04x:%04x[%04x:%04x]] class %#08x/%08x (%d)\n",
				/* [한국어] 네 개의 ID 필드와 */
				vendor, device, subvendor, subdevice,
				/* [한국어] 클래스 조건, 그리고 실패 코드를 찍는다. */
				class, class_mask, rc);
		/* [한국어] 추가 성공. */
		else
			/* [한국어] 어떤 ID 를 넣었는지 정보 수준으로 남긴다. 경고가 아니라 info 인 것은 정상 동작이기 때문이다. */
			pr_info("add [%04x:%04x[%04x:%04x]] class %#08x/%08x\n",
				/* [한국어] 같은 네 필드와 */
				vendor, device, subvendor, subdevice,
				/* [한국어] 클래스 조건을 찍는다. */
				class, class_mask);
	/* [한국어] 다음 토막으로. */
	}
}

/* [한국어] vfio_pci_init - 모듈 진입점. 파라미터를 코어에 넘기고 드라이버를 등록한다
 *
 * @return: 0 = 성공, 그 밖에는 pci_register_driver 가 올린 음수 errno.
 *   음수를 돌려주면 모듈 적재가 실패한다.
 *
 * 왜 필요한가: 세 가지를 순서대로 해야 하기 때문이다. (1) 코어가 장치를
 * 열 때 참조할 전역 설정을 먼저 심어 두고, (2) 드라이버를 등록해 바인딩이
 * 가능한 상태로 만들고, (3) 그 다음에야 동적 ID 를 넣어 실제 바인딩을
 * 일으킨다. **순서를 바꾸면 안 된다** - 등록 전에 동적 ID 를 넣으면
 * driver_attach 가 할 일을 찾지 못하고, 파라미터를 나중에 넘기면 그 사이에
 * 열린 장치가 잘못된 설정으로 동작한다.
 *
 * 동작 단계: (1) VGA 파라미터의 유효 값을 정한다. (2) 세 파라미터를 코어의
 * 전역 변수로 옮긴다. (3) 드라이버를 등록한다. (4) 동적 ID 를 추가한다.
 * (5) denylist 를 껐다면 그 사실을 경고로 남긴다.
 *
 * VGA 처리가 특이하다 - is_disable_vga 를 **true 로 초기화**해 두고
 * CONFIG_VFIO_PCI_VGA 가 켜진 경우에만 파라미터 값으로 덮어쓴다. 즉 VGA
 * 지원이 컴파일에서 빠진 커널에서는 언제나 "VGA 비활성" 으로 코어에
 * 전달된다. disable_vga 변수 자체가 그 경우 존재하지 않으므로 이렇게 하지
 * 않으면 컴파일되지 않는다.
 *
 * 실행 컨텍스트: 모듈 적재(프로세스 문맥). __init 이라 끝나면 회수된다.
 * 호출자: 커널의 모듈 적재 경로(아래 module_init 이 등록한다).
 * 호출 대상: vfio_pci_core_set_params(vfio_pci_core.c:7852),
 * pci_register_driver(실체는 drivers/pci/pci-driver.c:3130 의
 * __pci_register_driver), vfio_pci_fill_ids(이 파일), pr_warn.
 *
 * 에러 경로: 드라이버 등록 실패만 적재 실패로 이어진다. 그때는 이미 코어에
 * 심어 둔 파라미터를 되돌리지 않지만, 드라이버가 없으므로 그 값을 읽는
 * 경로도 없다. 동적 ID 추가 실패는 경고뿐이며 적재는 성공한다.
 *
 * 호출 체인:
 *   modprobe vfio-pci -> [vfio_pci_init] -> vfio_pci_core_set_params
 *     -> pci_register_driver -> vfio_pci_fill_ids -> pci_add_dynid */
static int __init vfio_pci_init(void)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 드라이버 등록의 반환값. */
	int ret;
	/* [한국어] 코어에 넘길 VGA 설정. **참으로 초기화한다** - VGA 지원이 컴파일에서
	 * 빠진 커널에서는 이 값이 그대로 쓰여 VGA region 이 노출되지 않는다. */
	bool is_disable_vga = true;

/* [한국어] VGA 지원이 컴파일에 포함된 경우에만, */
#ifdef CONFIG_VFIO_PCI_VGA
	/* [한국어] 모듈 파라미터의 실제 값으로 덮어쓴다. disable_vga 변수 자체가 이 분기 안에서만 존재하므로 이 대입도 여기 있어야 한다. */
	is_disable_vga = disable_vga;
/* [한국어] VGA 분기 끝. */
#endif

	/* [한국어] 세 파라미터를 코어의 전역 변수로 옮긴다. **여기가 이 파일과 코어가
	 * 설정을 주고받는 유일한 지점이다.** 이 호출 뒤로는 이 파일의
	 * nointxmask/disable_vga/disable_idle_d3 을 아무도 읽지 않는다. */
	vfio_pci_core_set_params(nointxmask, is_disable_vga, disable_idle_d3);

	/* [한국어] 아래 영어 주석대로, 드라이버를 등록하면서 이미 있는 장치들을 훑는다. */
	/* Register and scan for devices */
	/* [한국어] 드라이버를 PCI 버스에 등록한다. 매크로라 실제로는
	 * __pci_register_driver(drivers/pci/pci-driver.c:3130)에 모듈 포인터와 이름이
	 * 함께 넘어간다. 등록 즉시 드라이버 코어가 모든 PCI 장치를 이 드라이버의
	 * id_table 과 대조하지만, 그 표가 override 전용이라 **이 시점에는 아무것도
	 * 붙지 않는다.** */
	ret = pci_register_driver(&vfio_pci_driver);
	/* [한국어] 등록 실패. */
	if (ret)
		/* [한국어] 그대로 올려 모듈 적재를 실패시킨다. */
		return ret;

	/* [한국어] 이제야 동적 ID 를 넣는다. 등록이 끝난 뒤여야 그 안의 driver_attach 가
	 * 실제로 장치를 훑을 수 있다. **모듈 적재만으로 장치가 붙는 경로가 이
	 * 한 줄이다.** */
	vfio_pci_fill_ids();

	/* [한국어] 관리자가 denylist 를 껐는가. */
	if (disable_denylist)
		/* [한국어] 껐다면 부팅 로그에 한 번 남긴다. 장치별 경고는 probe 때 따로 나가지만,
		 * 이 메시지는 "이 시스템은 목록을 무시하도록 설정돼 있다" 는 사실 자체를
		 * 장치와 무관하게 알린다. */
		pr_warn("device denylist disabled.\n");

	/* [한국어] 적재 성공. */
	return 0;
}
/* [한국어] 위 함수를 모듈 진입점으로 등록한다. 내장 빌드일 때는 부팅 중 initcall 로, 모듈일 때는 insmod 때 불린다. */
module_init(vfio_pci_init);

/* [한국어] vfio_pci_cleanup - 모듈 퇴장점. 드라이버 등록을 해제한다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 모듈을 내릴 때 드라이버를 버스에서 떼어 내야 한다.
 * 그 한 줄이 전부다.
 *
 * 동작: pci_unregister_driver 가 이 드라이버에 붙어 있는 모든 장치에 대해
 * remove 콜백(vfio_pci_remove)을 부르고, 동적 ID 목록도 함께 해제한다
 * (drivers/pci/pci-driver.c:3182 와 그 안의 pci_free_dynids).
 * 즉 vfio_pci_fill_ids 가 넣은 ID 들의 정리를 이 파일이 직접 하지 않아도
 * 되는 이유가 거기 있다.
 *
 * 실행 컨텍스트: 모듈 해제(프로세스 문맥). __exit 이라 내장 빌드에서는
 * 코드가 아예 링크되지 않는다.
 * 호출자: 커널의 모듈 해제 경로(아래 module_exit 이 등록한다).
 * 호출 대상: pci_unregister_driver(drivers/pci/pci-driver.c:3182).
 *
 * 에러 경로: 없다. 아직 사용자가 장치를 열고 있으면 참조 계수 때문에
 * 모듈 해제 자체가 먼저 거부된다.
 *
 * 호출 체인:
 *   rmmod vfio-pci -> [vfio_pci_cleanup] -> pci_unregister_driver
 *     -> vfio_pci_remove (붙어 있던 장치마다) -> pci_free_dynids */
static void __exit vfio_pci_cleanup(void)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 버스에서 드라이버를 뗀다. 붙어 있던 장치마다 remove 가 돌고, 동적 ID 목록도 여기서 해제된다. */
	pci_unregister_driver(&vfio_pci_driver);
}
/* [한국어] 위 함수를 모듈 퇴장점으로 등록한다. */
module_exit(vfio_pci_cleanup);

/* [한국어] 모듈 라이선스 선언. "GPL v2" 라야 EXPORT_SYMBOL_GPL 로 공개된 심벌
 * (이 드라이버가 쓰는 vfio_pci_core_* 계열 전부)에 링크할 수 있다. 파일 첫
 * 줄의 SPDX 표기와 짝을 이룬다. */
MODULE_LICENSE("GPL v2");
/* [한국어] 위에서 정의한 저자 문자열을 모듈 메타데이터에 심는다. modinfo 로 볼 수 있다. */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] 설명 문자열을 심는다. 커널은 이 매크로가 없으면 빌드 경고를 낸다. */
MODULE_DESCRIPTION(DRIVER_DESC);
