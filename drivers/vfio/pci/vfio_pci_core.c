// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */

/* [한국어] [한국어 설명] VFIO PCI 백엔드 — 진짜 PCI 함수 하나를 사용자 공간이 직접 모는
 * 디바이스로 바꾸는 곳 (drivers/vfio/pci/vfio_pci_core.c)
 * 
 * 이 블록에서 다른 파일을 가리킬 때만 줄 번호를 적는다. 이 파일 자신은 주석이
 * 붙으며 줄 번호가 계속 밀리므로 함수 이름으로만 가리킨다. 이 트리는 sparse
 * checkout 이라 drivers/iommu 와 include/linux 의 대부분이 없다. 그래서 IOMMU
 * 내부와 대부분의 커널 헤더 선언은 "이 트리에서 확인 못 함" 으로 적었다.
 * 반대로 drivers/pci 전체(240개 파일)와 drivers/nvme, block 은 이미 주석 완료
 * 상태이므로 실제 줄 번호로 인용한다.
 * 
 * === 파일의 역할 ===
 * vfio-pci 계열 드라이버(vfio_pci.c 의 범용 드라이버와 mlx5, hisilicon, pds,
 * qat, nvgrace-gpu, virtio, xe, ism 의 변종들)가 **공유하는 PCI 백엔드 본체**다.
 * drivers/vfio/vfio_main.c 가 정의한 struct vfio_device 라는 추상 디바이스에
 * "이것은 PCI 함수다" 라는 실체를 채워 넣는 층이며, 구체적으로 다섯 가지 일을
 * 한다.
 * 
 *  (1) **디바이스 여닫기와 하드웨어 소유권** — vfio_pci_core_enable 이
 *      pci_enable_device 로 장치를 켜고, 리셋으로 초기 상태를 만들고,
 *      config 공간 그림자(vconfig)를 세우고, MSI-X 테이블의 위치를 알아 둔다.
 *      vfio_pci_core_disable 이 그 전부를 역순으로 되돌린다.
 *  (2) **region 모델** — 사용자에게 보이는 파일 오프셋 공간을 나눈다.
 *      config, BAR0~5, ROM, VGA, 그리고 vendor 드라이버가 덧붙인 추가 region.
 *      오프셋의 상위 비트가 region 번호이고 하위 40비트가 region 안의 위치다
 *      (include/linux/vfio_pci_core.h 의 VFIO_PCI_OFFSET_SHIFT 는 40).
 *  (3) **접근 중재** — read/write 는 vfio_pci_rw 가 region 별로 갈라
 *      config 는 vfio_pci_config.c 의 바이트 단위 권한표로, BAR 는
 *      vfio_pci_rdwr.c 의 제외 구간 규칙으로 넘긴다. mmap 은
 *      vfio_pci_core_mmap 이 BAR 물리 페이지를 **그대로** 사용자 주소 공간에
 *      노출한다. 무엇이 걸러지고 무엇이 통과하는지는 아래 전용 절에 정리했다.
 *  (4) **리셋과 오류 복구** — 함수 리셋, 슬롯/버스 리셋(hot reset), 마지막
 *      close 에서의 자동 리셋, AER 오류의 eventfd 중계. 리셋 경로마다 어떤
 *      상태를 가정하고 무엇을 복원하는지도 아래 전용 절에 정리했다.
 *  (5) **부가 정책** — SR-IOV PF/VF 사이의 vf_token 신뢰 협약, 런타임 PM
 *      진입/이탈(저전력 기능), VGA 중재자 참여, 그리고 vendor 가 등록한
 *      마이그레이션 vtable 의 **슬롯 단위 완비성 검사**.
 * 
 * 반대로 이 파일이 **하지 않는** 일도 분명하다. config 공간의 실제 권한표와
 * capability chain 재조립은 vfio_pci_config.c 가, BAR 의 실제 바이트 전송은
 * vfio_pci_rdwr.c 가, 인터럽트 벡터의 실제 할당과 request_irq 는
 * vfio_pci_intrs.c 가, dma-buf 노출은 vfio_pci_dmabuf.c 가 한다. 이 파일은
 * **정책과 수명과 잠금**을 쥐고 그 셋을 부른다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 이 저장소는 NVMe I/O 경로를 따라 읽어 왔다. block 의 blk-mq 코어,
 * drivers/nvme 의 커널 NVMe 드라이버, drivers/pci 의 PCI 코어가 모두 주석
 * 완료 상태다. 이 파일은 **그 경로 전체를 우회하는 쪽**에서 PCI 계층과 맞닿는
 * 지점이다. SPDK 가 NVMe 컨트롤러를 붙잡을 때 실제로 일어나는 일을 따라가면
 * 이 파일의 위치가 그대로 드러난다.
 * 
 *   1. 사용자가 /sys/bus/pci/devices/<주소>/driver_override 에 "vfio-pci" 를
 *      쓴다. 그 sysfs 속성이 바인딩을 어떻게 강제하는지는 이 트리의
 *      drivers/pci/pci-driver.c:362~404 와 435~437 에 이미 주석돼 있다.
 *   2. 커널 nvme 드라이버를 unbind 하면 drivers/nvme/host/pci.c 의
 *      nvme_remove 가 컨트롤러를 내리고, vfio-pci 가 probe 된다.
 *   3. vfio_pci.c 의 probe → [이 파일] vfio_pci_core_register_device
 *      → drivers/vfio/vfio_main.c 의 vfio_register_group_dev 로 등록.
 *      이때 vfio_assign_device_set 으로 "함께 리셋되는 함수들" 묶음을 정한다.
 *   4. 사용자 공간이 device fd 를 열면 vfio_main.c 의 vfio_df_open 이
 *      vendor 의 open_device 를 부르고, 그것이 [이 파일]
 *      vfio_pci_core_enable → vfio_pci_core_finish_enable 로 이어진다.
 *   5. 사용자 공간이 VFIO_DEVICE_GET_REGION_INFO 로 BAR 목록을 받고
 *      ([이 파일] vfio_pci_ioctl_get_region_info), BAR0 을 mmap 한다
 *      ([이 파일] vfio_pci_core_mmap).
 *   6. 그 뒤로 사용자 공간은 NVMe 컨트롤러 레지스터(CAP, CC, CSTS)와
 *      **SQ/CQ 도어벨을 직접 store 명령으로 쓴다**. 커널 nvme 드라이버도
 *      blk-mq 도 이 경로에는 없다. 이 파일도 그 순간에는 실행되지 않는다 —
 *      페이지 폴트가 이미 끝났다면 CPU 가 MMIO 를 바로 때린다.
 *   7. 장치가 DMA 로 찍는 주소는 물리 주소가 아니라 IOVA 이고, 그 변환표는
 *      사용자가 소유한 IOAS 뿐이다. 그 결합을 만드는 쪽이
 *      drivers/vfio/vfio_iommu_type1.c(1세대 container)와 iommufd(2세대)다.
 *      이 파일은 그 결합을 **직접 만들지 않고**, 결합이 성립하기 전에는
 *      vfio_main.c 의 df->access_granted 게이트가 ioctl 을 막아 준다.
 * 
 * 실행 컨텍스트는 거의 전부 **호스트 커널 프로세스 문맥**이다. mutex 와
 * rw_semaphore 를 잡고 잠들 수 있고, copy_to_user 와 copy_from_user 로 사용자
 * 페이지를 만진다. 예외가 셋 있다.
 *  - vfio_pci_core_aer_err_detected 는 PCI AER 복구 워커에서 불린다.
 *  - vfio_pci_mmap_huge_fault 계열은 **페이지 폴트 문맥**이라 mmap_lock 을
 *    쥔 채 들어오며, 여기서 잠들거나 memory_lock 을 write 로 잡으면 안 된다.
 *  - vfio_pci_bus_notifier 는 PCI 버스 알림 체인에서 불린다.
 * 
 * === 타 모듈과의 연결 ===
 *  - include/linux/vfio_pci_core.h
 *      : 이 파일이 구현하는 **vendor 대상 ABI**. struct vfio_pci_core_device
 *        (200줄대), struct vfio_pci_regops, struct vfio_pci_region,
 *        struct vfio_pci_eventfd, VFIO_PCI_OFFSET_SHIFT 계열 매크로,
 *        is_aligned_for_order(227줄)가 모두 여기 있다. 이 파일이 정의하는
 *        구조체는 그래서 셋뿐이다 — dummy_resource, vf_token, fill_info,
 *        group_info, walk_info 처럼 **이 파일 안에서만 쓰는 것들**이다.
 *  - drivers/vfio/pci/vfio_pci_priv.h
 *      : 형제 파일 사이의 내부 ABI. vfio_config_init, vfio_pci_bar_rw,
 *        vfio_pci_set_irqs_ioctl, vfio_pci_zdev_open_device 등의 prototype 과
 *        CONFIG 별 stub. 이 파일이 #ifdef 없이 s390 zPCI, IGD, dma-buf 분기를
 *        쓸 수 있는 이유가 이 헤더의 stub 이다.
 *  - drivers/vfio/vfio_main.c (주석 완료)
 *      : 이 파일의 상위. vfio_register_group_dev, vfio_assign_device_set,
 *        vfio_check_feature, vfio_info_add_capability, vfio_info_cap_shift,
 *        vfio_set_irqs_validate_and_prepare, vfio_device_set_open_count,
 *        vfio_find_device_in_devset, vfio_device_cdev_opened 를 여기서 가져다
 *        쓴다. 그 파일의 주석은 **마이그레이션 vtable 의 슬롯 단위 완비성
 *        검사가 코어가 아니라 이 파일에 있다**고 적어 두었다 — 그 검사가 바로
 *        vfio_pci_core_register_device 안의 mig_ops 와 log_ops 검사다.
 *        코어는 포인터가 있는지만 보고, 슬롯 셋이 다 찼는지는 여기서 본다.
 *  - drivers/vfio/group.c, drivers/vfio/device_cdev.c
 *      : 1세대 group fd 와 2세대 cdev 경로. group.c 의
 *        vfio_device_get_from_name 이 이 파일의 vfio_pci_core_match 를 부르고,
 *        device_cdev.c 의 vfio_df_check_token 이
 *        vfio_pci_core_match_token_uuid 를 부른다. 두 경로가 vf_token 을
 *        각각 다른 문법으로 받는다는 점이 vfio_pci_core_match 주석에 있다.
 *  - drivers/vfio/vfio_iommu_type1.c (주석 완료), iommufd.c
 *      : DMA 핀과 IOVA 매핑. 이 파일은 hot reset 소유권 판정에서만 iommufd 를
 *        건드리며(vfio_iommufd_get_dev_id), 매핑 자체는 만지지 않는다.
 *  - drivers/vfio/pci/vfio_pci_config.c
 *      : config 공간 중재의 실체. vfio_config_init 이 vconfig 그림자와
 *        pci_config_map 을 만들고, vfio_pci_config_rw 가 바이트 단위 권한표로
 *        읽기/쓰기를 나눈다. 이 파일은 그것을 **켜고 끄고 부를 뿐**이다.
 *  - drivers/vfio/pci/vfio_pci_rdwr.c
 *      : BAR/ROM/VGA 의 실제 바이트 전송. vfio_pci_core_setup_barmap(201줄),
 *        vfio_pci_bar_rw(226줄), vfio_pci_core_do_io_rw(133줄대),
 *        vfio_pci_ioeventfd(431줄).
 *  - drivers/vfio/pci/vfio_pci_intrs.c
 *      : INTx/MSI/MSI-X/ERR/REQ 다섯 종류 인터럽트의 실체.
 *        vfio_pci_set_irqs_ioctl(825줄)이 이 파일의 ioctl 에서 불린다.
 *  - drivers/vfio/pci/vfio_pci_dmabuf.c, vfio_pci_igd.c, vfio_pci_zdev.c
 *      : 선택 기능. dma-buf 로 BAR 를 P2P 대상으로 내주기(221, 334, 379줄),
 *        인텔 통합 그래픽 전용 region(163, 385줄의 regops 표),
 *        s390 zPCI 정보 capability(116줄).
 *  - drivers/pci (전부 주석 완료) — 이 파일이 직접 부르는 것들:
 *        pci.c:4126 pci_enable_device, pci.c:4314 pci_disable_device,
 *        pci.c:7923 pci_clear_master, pci.c:8201 pci_intx,
 *        pci.c:2971 pci_set_power_state, pci.c:3324 pci_save_state,
 *        pci.c:3530 pci_restore_state, pci.c:3606 pci_store_saved_state,
 *        pci.c:3728 pci_load_and_free_saved_state,
 *        pci.c:10347 pci_try_reset_function,
 *        pci.c:10016 __pci_reset_function_locked, pci.c:11418 pci_reset_bus,
 *        pci.c:11012 pci_probe_reset_slot, pci.c:11378 pci_probe_reset_bus,
 *        pci.c:9727 pci_dev_trylock, pci.c:379 pci_bus_max_busnr,
 *        pci.c:6998 pci_enable_atomic_ops_to_root,
 *        pci.c:7258 pci_release_selected_regions,
 *        search.c:746 pci_get_class, bus.c:1190 pci_walk_bus,
 *        rom.c:382 pci_map_rom, rom.c:475 pci_unmap_rom,
 *        iomap.c:417 pci_iounmap, iov.c:2458 pci_enable_sriov,
 *        iov.c:2491 pci_disable_sriov, iov.c:2524 pci_num_vf,
 *        p2pdma.c:809 pcim_p2pdma_init, pci-driver.c:3239 pci_dev_driver,
 *        vgaarb.c:2812 vga_client_register, vgaarb.c:2745
 *        vga_set_legacy_decoding.
 *        drivers/pci/pci.c:11418 의 pci_reset_bus 주석은 "VFIO 가 장치를
 *        게스트에게 넘기기 전후로 부르는 것이 주된 용례" 라고 적고 있는데,
 *        그 호출자가 바로 이 파일의 vfio_pci_dev_set_hot_reset 와
 *        vfio_pci_dev_set_try_reset 다.
 * 
 * === 주요 함수/구조체 요약 ===
 *  - vfio_pci_core_enable / _disable / _finish_enable
 *      : 디바이스 수명의 몸통. enable 이 잡는 것(전원, 버스 마스터 해제,
 *        초기 리셋, pci_saved_state, vconfig, msix_bar/offset/size, VGA)을
 *        disable 이 정확히 역순으로 놓는다. finish_enable 은 vendor 가
 *        region 을 다 등록한 뒤 BAR 의 mmap 가능 여부를 확정하는 후처리다.
 *  - vfio_pci_core_ioctl
 *      : 일곱 개 ioctl 의 디스패처. GET_INFO, GET_IRQ_INFO,
 *        GET_PCI_HOT_RESET_INFO, IOEVENTFD, PCI_HOT_RESET, RESET, SET_IRQS.
 *  - vfio_pci_ioctl_get_region_info
 *      : 사용자에게 region 지도를 그려 주는 함수. 이 파일에서 **무엇이
 *        mmap 가능한지가 정해지는 지점**이다.
 *  - vfio_pci_rw / vfio_pci_core_read / _write
 *      : read(2)/write(2) 경로. region 번호로 갈라 config 는 권한표로,
 *        BAR 는 제외 구간 규칙으로 보낸다.
 *  - vfio_pci_core_mmap 과 vfio_pci_mmap_huge_fault
 *      : mmap(2) 경로. mmap 시점에는 아무것도 매핑하지 않고 vm_ops 만 걸며,
 *        실제 PFN 삽입은 폴트 때 memory_lock 을 read 로 잡고 한다.
 *  - vfio_pci_zap_bars / vfio_pci_zap_and_down_write_memory_lock
 *      : **BAR 매핑 회수**. 리셋이나 메모리 디코드 해제 직전에 사용자
 *        페이지 테이블 항목을 모두 걷어낸다. 이 파일의 안전 장치 중 하나.
 *  - vfio_pci_dev_set_hot_reset / _try_reset / _resettable
 *      : 슬롯/버스 리셋 삼총사. 소유권 판정, 잠금 획득, 실패 시 되감기.
 *  - vfio_pci_core_match / _match_token_uuid
 *      : SR-IOV PF 와 VF 사이의 신뢰를 UUID 하나로 표현하는 협약.
 *  - vfio_pci_core_register_device / _unregister_device
 *      : probe/remove 에서 불리는 등록의 시작과 끝. 마이그레이션 vtable
 *        완비성 검사와 device set 배정이 여기 있다.
 *  - struct vfio_pci_dummy_resource
 *      : 페이지보다 작은 BAR 가 페이지의 나머지를 남에게 뺏기지 않도록
 *        잡아 두는 자리표.
 *  - struct vfio_pci_vf_token
 *      : PF 하나가 들고 있는 UUID 와 그것을 쓰는 VF 사용자 수.
 *  - struct vfio_pci_fill_info / _group_info / _walk_info
 *      : hot reset 관련 버스 순회에서만 쓰는 임시 전달 상자 셋.
 * 
 * === 무엇이 중재되고 무엇이 그대로 통과하는가 (이 서브시스템의 보안 경계) ===
 * 사용자 공간이 장치에 닿는 통로는 셋뿐이다 — config 접근, BAR 접근, 인터럽트
 * 설정. 셋의 통과 정도가 완전히 다르다.
 * 
 *  [1] **config 공간 — 전부 중재된다. 직통 경로가 없다.**
 *      사용자의 read/write 는 반드시 vfio_pci_config_rw 를 거치고, 거기서
 *      바이트마다 두 개의 마스크를 본다(vfio_pci_config.c:118~121 의
 *      NO_VIRT/ALL_VIRT/NO_WRITE/ALL_WRITE).
 *        - virt 비트: 읽으면 하드웨어가 아니라 vdev->vconfig 그림자에서 온다.
 *        - write 비트: 쓰기가 허용된 비트. write 이면서 virt 인 비트는
 *          vconfig 에만 쓰이고, write 이면서 virt 가 아닌 비트만
 *          pci_user_write_config 로 **진짜 하드웨어에 나간다**
 *          (vfio_pci_config.c:201~245 의 vfio_default_config_write).
 *        - 둘 다 아닌 비트: 쓰기는 조용히 버려지고, 읽기는 하드웨어 값이다.
 *      실제 표의 예(vfio_pci_config.c 의 init_pci_cap_basic_perm 등):
 *        - Vendor/Device ID(670~671줄): 전부 virt, 쓰기 금지.
 *        - COMMAND(677줄): INTx Disable 비트만 virt. 나머지 비트(Memory
 *          Space Enable, Bus Master Enable 등)는 **진짜로 하드웨어에 나간다**.
 *        - STATUS(680줄): Capability List 비트만 virt, 전 구간 쓰기 금지.
 *        - BAR0~5 와 ROM BAR(688~694줄): **전부 virt 이면서 전부 쓰기 가능**.
 *          사용자는 BAR 를 마음대로 재배치하는 것처럼 보이지만, 그 값은
 *          vconfig 에만 남고 하드웨어 BAR 는 호스트가 정한 그대로다. 이것이
 *          게스트가 자기 주소 공간을 자유롭게 짜면서도 호스트의 자원 배치를
 *          깨뜨리지 못하는 이유다.
 *        - Capability List 포인터(697줄): 전부 virt, 쓰기 금지. capability
 *          체인 자체가 재조립되어 사용자에게는 다른 목록이 보인다.
 *        - Interrupt Pin(703줄): 전부 virt, 쓰기 금지. 이 파일의
 *          vfio_pci_get_irq_count 가 INTx 개수를 vconfig 의 이 바이트로
 *          판단한다 — 즉 nointx 장치는 여기를 0 으로 만들어 INTx 자체가
 *          없는 것처럼 보이게 한다.
 *        - MSI/MSI-X 의 주소와 데이터 필드(1229~1242줄): virt + 쓰기 가능.
 *          사용자가 쓴 MSI 주소는 하드웨어에 가지 않는다. 진짜 벡터는 커널이
 *          drivers/pci/msi 를 통해 프로그램한다.
 *        - AF FLR 비트(1017줄): virt 이자 쓰기 가능 — 사용자의 FLR 요청은
 *          vfio_af_config_write 가 가로채 커널의 리셋 경로로 바꾼다.
 *      결론: **config 로는 장치를 재배치하거나 남의 메모리로 인터럽트를
 *      쏘게 만들 수 없다.** 반면 Memory/BusMaster Enable 같은 실제 동작
 *      비트는 통과하며, 그 통과가 memory_lock 과 짝지어 관리된다.
 * 
 *  [2] **BAR — 통로에 따라 다르다.**
 *      - mmap(2): **하드웨어 직통**이다. vfio_pci_core_mmap 이 VM_PFNMAP 로
 *        BAR 의 물리 페이지를 사용자 주소 공간에 그대로 건다. 중재는 매핑을
 *        **허용할지 말지**에서만 일어난다 — bar_mmap_supported 가 참인 BAR
 *        (IORESOURCE_MEM 이고, 크기가 0 이 아니고, non_mappable_bars 가 아니고,
 *        페이지 크기 이상이거나 페이지 시작에 정렬된 것)만 되고, ROM 과 VGA
 *        와 그 이상의 인덱스는 거부한다. 일단 매핑되면 CPU store 가 곧장
 *        장치 레지스터를 때리며 커널은 개입하지 않는다. **SPDK 가 NVMe
 *        도어벨을 쓰는 경로가 정확히 이것이다.**
 *      - read(2)/write(2): vfio_pci_bar_rw 를 거치며 **MSI-X 테이블 구간은
 *        제외된다**. vdev->msix_offset 부터 msix_size 바이트는
 *        vfio_pci_core_do_io_rw 의 제외 구간이 되어 쓰기는 버려지고 읽기는
 *        0xFF 로 채워진다(drivers/vfio/pci/vfio_pci_rdwr.c:126~133 의 설명과
 *        175~186 의 구현). ROM BAR 의 실제 ROM 뒤쪽 남는 공간도 같은 방식으로
 *        제외되고, ROM 은 qword 접근이 금지된다.
 *      - 여기서 주의할 비대칭: **read/write 는 MSI-X 테이블을 가리지만 mmap 은
 *        가리지 않는다.** 이 파일의 msix_mmappable_cap 이 붙이는
 *        VFIO_REGION_INFO_CAP_MSIX_MAPPABLE 이 바로 "이 BAR 는 MSI-X 테이블을
 *        품고 있지만 그래도 mmap 해도 된다" 는 통보다. 그렇게 바뀔 수 있었던
 *        이유는 인터럽트 재매핑 하드웨어가 있으면 장치가 임의 주소로 MSI 를
 *        쏘아도 막히기 때문인데, 그 판단 자체는 이 트리에 없는
 *        drivers/iommu 쪽이라 여기서는 확인 못 함.
 *      - 두 통로 모두 memory_lock 아래에 있다. PCI_COMMAND 의 Memory Space
 *        Enable 이 꺼져 있으면 read/write 는 -EIO 를 받고 폴트는 SIGBUS 가
 *        된다(vfio_pci_vmf_insert_pfn). 이것이 없으면 디코드가 꺼진 BAR 를
 *        건드려 호스트가 마스터 어보트를 맞을 수 있다.
 * 
 *  [3] **인터럽트 — 전부 중재된다. 사용자는 벡터를 만들 수 없다.**
 *      사용자가 할 수 있는 것은 VFIO_DEVICE_SET_IRQS 로 "몇 번 벡터를
 *      이 eventfd 에 연결해 달라" 고 요청하는 것뿐이다. 자세한 경로는 아래
 *      인터럽트 절에 적었다.
 * 
 * === 리셋과 오류 복구 — 네 개의 진입점 ===
 *  [A] **열 때의 리셋** (vfio_pci_core_enable)
 *      pci_try_reset_function 으로 이전 사용자의 잔재를 지운다. 가정: 아직
 *      사용자 매핑이 없다(fd 는 열렸지만 mmap 전). -EAGAIN(디바이스 락 실패)
 *      이면 열기 자체를 포기하고, 다른 실패는 reset_works 를 거짓으로 만들고
 *      계속 간다 — 리셋 못 하는 장치도 쓸 수는 있게 해 준다. 리셋 **직후**에
 *      pci_save_state 와 pci_store_saved_state 로 "깨끗한 상태" 를 떠 둔다.
 *      이 스냅숏이 닫을 때 복원의 기준이다.
 *  [B] **VFIO_DEVICE_RESET ioctl** (vfio_pci_ioctl_reset)
 *      사용자가 명시적으로 요청하는 함수 단위 리셋. 순서가 중요하다.
 *      zap_and_down_write_memory_lock 으로 **먼저 사용자 BAR 매핑을 전부
 *      회수하고 memory_lock 을 write 로 잡은 뒤** D0 로 올리고,
 *      dma-buf 를 revoke 하고, pci_try_reset_function 을 부른다. 열린 fd 는
 *      그대로 살아 있고, 사용자의 다음 접근은 폴트를 다시 맞아 재매핑된다.
 *      vconfig 그림자는 이 경로에서 손대지 않는다.
 *  [C] **VFIO_DEVICE_PCI_HOT_RESET ioctl** (vfio_pci_dev_set_hot_reset)
 *      슬롯이나 버스 전체를 리셋한다. 가정이 가장 무겁다 — 그 범위의
 *      **모든 PCI 함수가 같은 dev_set 안에 있고, 모두 호출자 소유**여야 한다.
 *      소유 증명은 두 가지 문법이다. 1세대는 group fd 배열을 넘겨
 *      vfio_dev_in_groups 로 확인하고, 2세대(cdev)는 길이 0 배열을 넘겨
 *      같은 iommufd_ctx 에 묶여 있는지로 확인한다. 그 다음 dev_set 의 모든
 *      디바이스에 대해 memory_lock 을 **trylock** 으로 잡고 BAR 를 zap 한다.
 *      하나라도 경합하면 -EBUSY 로 물러서며, 이미 잡은 것들을 역순으로 되감는
 *      코드가 err_undo 다. 여러 디바이스의 락을 겹쳐 잡는 유일한 곳이라
 *      교착을 피하려고 trylock 과 되감기를 쓴다.
 *  [D] **닫을 때의 리셋** (vfio_pci_core_disable 과 _dev_set_try_reset)
 *      저장해 둔 상태를 되돌리고, 가능하면 리셋까지 한다. 여기서는
 *      pci_try_reset_function 대신 pci_dev_trylock + __pci_reset_function_locked
 *      를 쓰는데, "try" 계열이 내부에서 상태를 다시 저장해 방금 복원한
 *      설정을 덮어쓰기 때문이다(원본 영어 주석이 그렇게 적고 있다).
 *      상위 브리지까지 trylock 하는 이유는 리셋이 브리지 링크를 건드리기
 *      때문이다. 여기서 리셋에 실패하면 needs_reset 을 남겨 두고,
 *      vfio_pci_dev_set_try_reset 이 **묶음 전체가 놀고 있을 때** 버스 리셋을
 *      대신 시도한다.
 *  [E] **AER 오류** (vfio_pci_core_aer_err_detected)
 *      PCI 코어의 오류 복구 콜백. 이 파일은 복구를 하지 않고 err_trigger
 *      eventfd 를 울려 사용자에게 넘긴 뒤 PCI_ERS_RESULT_CAN_RECOVER 를
 *      돌려준다. 즉 **정책은 사용자 공간에 있다**.
 * 
 *  어느 경로든 **열린 fd 는 닫히지 않는다**. 리셋이 사용자에게 보이는 방식은
 *  (i) BAR 매핑이 사라졌다가 다음 접근에서 다시 생기는 것, (ii) 필요하면
 *  req_trigger/err_trigger eventfd 로 통보받는 것, 이 둘뿐이다.
 * 
 * === 인터럽트 설정 — 이 파일과 drivers/pci/msi 의 접점 ===
 * 이 파일이 직접 하는 인터럽트 일은 셋뿐이다.
 *  - **개수 세기**: vfio_pci_get_irq_count 가 다섯 종류의 최대 개수를 센다.
 *    INTx 는 vconfig 의 Interrupt Pin 바이트로, MSI 는 MSI Flags 의
 *    Multiple Message Capable 필드(2^n)로, MSI-X 는 MSI-X Flags 의 Table Size
 *    필드(+1)로 센다. 값을 **하드웨어 config 에서 직접 읽는다**는 점이
 *    중요하다 — 그림자가 아니라 실물을 본다.
 *  - **위치 알아내기**: vfio_pci_core_enable 이 MSI-X capability 에서
 *    Table BIR(어느 BAR 인지)과 Table Offset 을 뽑아 msix_bar 와 msix_offset
 *    에 넣고, Table Size 로 msix_size 를 계산한다. 이 셋이 위에서 말한
 *    read/write 제외 구간의 근거다. 같은 자리에서 pci_msix_can_alloc_dyn
 *    (drivers/pci/msi/api.c:287)으로 **동적 벡터 추가가 가능한지**를 물어
 *    has_dyn_msix 에 저장한다. 그 값이 GET_IRQ_INFO 에서
 *    VFIO_IRQ_INFO_NORESIZE 플래그의 유무로 사용자에게 전달된다.
 *  - **문 열어 주기**: vfio_pci_ioctl_set_irqs 가 인자를 검증하고
 *    igate mutex 를 잡은 뒤 vfio_pci_set_irqs_ioctl(vfio_pci_intrs.c:825)로
 *    넘긴다. 실제 할당은 거기서 일어난다 —
 *    pci_alloc_irq_vectors(drivers/pci/msi/api.c:519)로 벡터를 잡고
 *    (vfio_pci_intrs.c:393), 동적 추가는 pci_msix_alloc_irq_at
 *    (drivers/pci/msi/api.c:317)로 하고(vfio_pci_intrs.c:441),
 *    pci_irq_vector 로 얻은 Linux IRQ 번호에 request_irq 로 핸들러를 건다
 *    (vfio_pci_intrs.c:434, 460, 510). 해제는 pci_free_irq_vectors
 *    (vfio_pci_intrs.c:567). INTx 는 drivers/pci/pci.c:8201 의 pci_intx 로
 *    PCI_COMMAND 의 Interrupt Disable 비트를 직접 토글한다
 *    (vfio_pci_intrs.c:121, 181, 299, 575).
 *  즉 **사용자는 벡터를 만들지 못하고 eventfd 에 연결만 요청한다**. 어느
 *  주소로 MSI 를 쏠지는 커널이 정하고, 그 값은 사용자가 config 로 읽어도
 *  그림자 값만 보인다. 이것이 인터럽트 쪽 보안 경계다. */
/* [한국어] pr_debug 와 pci_info 계열이 찍는 모든 커널 로그 줄 앞에 모듈 이름과 콜론을
 * 붙인다. KBUILD_MODNAME 은 Makefile 이 정의하는 모듈 이름이라 이 파일이 어느
 * 모듈로 빌드되든(vfio-pci-core) 로그만 보고 출처를 알 수 있다. 반드시 모든
 * #include 보다 위에 있어야 한다 — 헤더가 먼저 pr_fmt 를 정의해 버리면 이
 * 정의가 재정의 경고를 내기 때문이다. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* [한국어] aperture_remove_conflicting_pci_devices — 이 PCI 장치의 프레임버퍼 영역을
 * 쓰고 있던 기존 콘솔/fbdev 드라이버를 밀어낸다. VGA 장치를 사용자 공간에
 * 넘기기 전에 커널이 그 BAR 를 계속 쓰고 있으면 안 되므로
 * vfio_pci_vga_init 이 이 함수 하나 때문에 이 헤더를 필요로 한다. */
#include <linux/aperture.h>
/* [한국어] struct device 와 dev_get_drvdata, device_lock_assert, device_set_driver_override,
 * bus_register_notifier, dev_dbg_ratelimited. 이 파일은 pci_dev 뿐 아니라 그
 * 안의 struct device 를 직접 다루는 지점이 많다 — 런타임 PM 은 device 단위이고,
 * SR-IOV VF 가로채기는 버스 알림 체인을 쓰기 때문이다. */
#include <linux/device.h>
/* [한국어] eventfd_ctx, eventfd_ctx_fdget, eventfd_ctx_put, eventfd_signal.
 * VFIO 가 사용자 공간에 사건을 알리는 유일한 수단이 eventfd 다. 이 파일에서는
 * err_trigger(AER 오류), req_trigger(디바이스 반납 요청),
 * pm_wake_eventfd_ctx(저전력에서 깨어남) 셋에 쓴다. */
#include <linux/eventfd.h>
/* [한국어] fget 와 fput — hot reset 에서 사용자가 넘긴 group fd 배열을 struct file 로
 * 바꾸고, 리셋이 끝날 때까지 그 참조를 붙잡아 둔다. 리셋 도중에 group 이
 * 사라지면 소유권 판정이 무의미해지므로 참조를 반드시 쥔 채로 진행한다. */
#include <linux/file.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(request_irq, free_irq, irqreturn_t,
 * IRQF_ 계열, tasklet)을 이 파일에서 하나도 쓰지 않는다. 실제 인터럽트 등록은
 * 전부 drivers/vfio/pci/vfio_pci_intrs.c 에서 하며 이 파일은 그 함수를 부를 뿐이다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/interrupt.h>
/* [한국어] iommu_group_get, iommu_group_id, iommu_group_put. hot reset 정보 조회에서
 * 1세대(group) 문법일 때 각 PCI 함수가 속한 IOMMU 그룹 번호를 사용자에게
 * 알려 주는 데 쓴다. IOMMU 그룹이 없는 장치는 격리가 보장되지 않으므로
 * 그 자리에서 -EPERM 으로 거절한다. 그룹 자체의 내부 구현은 drivers/iommu
 * 아래에 있는데 이 트리에는 없어 확인 못 함. */
#include <linux/iommu.h>
/* [한국어] MODULE_LICENSE, MODULE_AUTHOR, MODULE_DESCRIPTION, module_init, module_exit,
 * EXPORT_SYMBOL_GPL. 이 파일은 vfio-pci-core 라는 **독립 모듈**로 빌드되어
 * 자기 init/exit 를 갖고, 23개의 심볼을 vendor 드라이버에게 내보낸다. */
#include <linux/module.h>
/* [한국어] DEFINE_MUTEX, mutex_init, mutex_lock, mutex_unlock, mutex_destroy.
 * 이 파일의 잠금은 네 종류다 — 전역 sriov_pfs_mutex, 디바이스별 igate(인터럽트
 * 설정), ioeventfds_lock, 그리고 vf_token->lock. */
#include <linux/mutex.h>
/* [한국어] struct notifier_block 과 bus_register_notifier/bus_unregister_notifier 가 쓰는
 * 알림 체인 타입. SR-IOV PF 가 자기 VF 들이 나타나는 순간을 가로채
 * driver_override 를 심어 두기 위해 필요하다. */
#include <linux/notifier.h>
/* [한국어] PCI 코어 전체의 공개 API. struct pci_dev 부터 pci_enable_device,
 * pci_save_state, pci_reset_bus, PCI_COMMAND 계열 레지스터 상수까지 이 파일이
 * 쓰는 PCI 이름의 대부분이 여기서 온다. 이 트리는 sparse checkout 이라
 * include/linux/pci.h 자체는 없지만, 구현은 drivers/pci 아래에 전부 있고
 * 이미 주석 완료 상태다. */
#include <linux/pci.h>
/* [한국어] pm_runtime_resume_and_get, pm_runtime_put, pm_runtime_put_noidle,
 * pm_runtime_get_noresume, pm_runtime_allow, pm_runtime_forbid, pm_runtime_resume,
 * SET_RUNTIME_PM_OPS. 사용자가 없을 때 장치를 D3 로 재우는 정책 전체가 이
 * API 위에 서 있다. */
#include <linux/pm_runtime.h>
/* [한국어] kzalloc_obj, kzalloc_objs, krealloc, kfree. 이 파일의 할당은 대부분
 * GFP_KERNEL_ACCOUNT 를 써서 **사용자 요청으로 생긴 커널 메모리를 그 사용자의
 * cgroup 에 청구**한다 — 사용자가 ioctl 로 커널 메모리를 무한정 쓰게 두지
 * 않으려는 장치다. */
#include <linux/slab.h>
/* [한국어] u8, u16, u32, size_t, loff_t, bool 같은 기본 타입. 다른 헤더가 대부분
 * 전이적으로 끌어오지만, 이 파일이 이 타입들을 직접 쓰므로 명시적으로 포함해
 * 헤더 정리 순서가 바뀌어도 깨지지 않게 한다. */
#include <linux/types.h>
/* [한국어] copy_from_user 와 copy_to_user. 이 파일의 모든 ioctl 핸들러가 사용자
 * 메모리를 오직 이 두 함수로만 만진다. 사용자 포인터를 역참조하는 다른 경로는
 * 없다 — 있다면 그것이 곧 취약점이다. */
#include <linux/uaccess.h>
/* [한국어] VGA_RSRC_NORMAL_IO 계열 상수와 vga_client_register, vga_client_unregister,
 * vga_set_legacy_decoding. 레거시 VGA 자원(0x3B0~0x3DF I/O 포트,
 * 0xA0000~0xBFFFF 메모리)은 여러 그래픽 카드가 공유하므로 중재자가 필요하다.
 * 구현은 drivers/pci/vgaarb.c 에 있고 주석 완료 상태다(2745, 2812줄). */
#include <linux/vgaarb.h>
/* [한국어] array_index_nospec — Spectre v1(경계 검사 우회) 방어. 사용자가 준 region
 * 인덱스를 배열 첨자로 쓰기 직전에 이 매크로를 통과시켜, 분기 예측이 틀려도
 * 투기 실행이 배열 밖을 읽지 못하게 마스크를 씌운다. 이 파일에서는
 * vfio_pci_ioctl_get_region_info 의 vendor region 조회 한 곳에 쓴다. */
#include <linux/nospec.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(mmget, mmput, mmgrab, get_task_mm,
 * might_alloc)을 이 파일에서 하나도 쓰지 않는다. 사용자 주소 공간을 다루는
 * 지점은 vfio_pci_zap_bars 의 unmap_mapping_range 와 폴트 핸들러뿐인데 그것들은
 * 다른 헤더에서 온다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/sched/mm.h>
/* [한국어] struct iommufd_ctx 와 vfio_iommufd_device_ictx, vfio_iommufd_get_dev_id 가
 * 쓰는 타입. 2세대(cdev) hot reset 에서 "이 디바이스가 호출자와 같은 iommufd
 * 컨텍스트에 묶여 있는가" 를 판정하는 데 필요하다. iommufd 내부 구현은
 * drivers/iommu 아래라 이 트리에서 확인 못 함. */
#include <linux/iommufd.h>
/* [한국어] pcim_p2pdma_init — 이 PCI 함수의 BAR 를 peer-to-peer DMA 의 **제공자**로
 * 등록한다. 그래야 다른 장치가 이 장치의 BAR 로 직접 DMA 할 수 있고,
 * dma-buf 로 BAR 를 내주는 기능이 성립한다. 구현은 이 트리의
 * drivers/pci/p2pdma.c:809 에 있으며 주석 완료 상태다. */
#include <linux/pci-p2pdma.h>
/* [한국어] EEH(Enhanced Error Handling)는 IBM POWER 계열의 PCI 오류 격리 기능이다.
 * 그 아키텍처에서만 존재하는 헤더라 조건부로 포함한다. 아래 두 곳
 * (vfio_pci_core_finish_enable 의 eeh_dev_open, vfio_pci_core_close_device 의
 * eeh_dev_release)도 같은 조건으로 감싸여 있다. */
#if IS_ENABLED(CONFIG_EEH)
/* [한국어] eeh_dev_open 과 eeh_dev_release — 사용자 공간이 장치를 소유하는 동안
 * EEH 복구를 커널이 마음대로 하지 않도록 표시한다. */
#include <asm/eeh.h>
#endif

/* [한국어] 형제 파일(config/intrs/rdwr/igd/zdev/dmabuf)과 공유하는 내부 헤더.
 * vfio_config_init, vfio_pci_bar_rw, vfio_pci_set_irqs_ioctl 같은 prototype 과
 * CONFIG_VFIO_PCI_VGA, CONFIG_VFIO_PCI_IGD, CONFIG_VFIO_PCI_ZDEV_KVM,
 * CONFIG_VFIO_PCI_DMABUF 가 꺼졌을 때의 stub 이 들어 있다. 그 stub 덕분에
 * 이 파일 본문에는 #ifdef 가 EEH 두 곳 말고는 없다. */
#include "vfio_pci_priv.h"

/* [한국어] 아래 MODULE_AUTHOR 에 들어갈 원저자 표기. 코드에서 쓰이는 곳은 그 한 군데뿐. */
#define DRIVER_AUTHOR   "Alex Williamson <alex.williamson@redhat.com>"
/* [한국어] modinfo 로 보이는 모듈 설명. "core driver" 라는 표현대로 이 모듈은 스스로
 * 어떤 PCI 장치와도 매칭되지 않는다 — vfio_pci.c 같은 실제 드라이버가 이
 * 모듈의 심볼을 가져다 쓸 뿐이다. */
#define DRIVER_DESC "core driver for VFIO based PCI devices"

/* [한국어] INTx 마스킹을 아예 쓰지 않게 하는 전역 스위치.
 * 설정자: vfio_pci_core_set_params 를 통해 vfio_pci.c 의 모듈 파라미터
 * nointxmask 가 그대로 전달된다(drivers/vfio/pci/vfio_pci.c:265).
 * 읽는 자: vfio_pci_core_enable 한 곳뿐. 거짓이면 pci_intx_mask_supported 로
 * 장치가 PCI 2.3 스타일 INTx 마스킹을 지원하는지 조사해 pci_2_3 에 넣는다.
 * 값 범위: false 가 기본. true 면 INTx 를 쓰는 장치는 인터럽트 선을 공유하지
 * 못하고 **전용 인터럽트를 요구**하게 된다.
 * 동기화: 모듈 파라미터라 사실상 부팅 시 한 번만 쓰이고 이후 읽기 전용이다. */
static bool nointxmask;
/* [한국어] 레거시 VGA region 지원을 끄는 전역 스위치.
 * 설정자: vfio_pci_core_set_params 경유(vfio_pci.c:265).
 * 읽는 자: vfio_vga_disabled 를 통해 vfio_pci_set_decode 와
 * vfio_pci_core_enable 이 읽는다.
 * 값 범위: false 면 VGA region 노출, true 면 has_vga 를 세우지 않아
 * VFIO_PCI_VGA_REGION_INDEX 조회가 -EINVAL 이 된다.
 * 동기화: 위와 같다. */
static bool disable_vga;
/* [한국어] 사용자가 없을 때 장치를 D3hot 으로 재우는 절전 동작을 끄는 스위치.
 * 설정자: vfio_pci_core_set_params 경유(vfio_pci.c:265).
 * 읽는 자: 이 파일 여섯 곳 — enable/disable 의 pm_runtime 짝, 등록/해제의
 * pm_runtime 짝, 그리고 dev_set 리셋 전후의 usage count 조작.
 * 값 범위: false(기본)면 사용 중이 아닐 때 D3 로 내려간다. true 면 항상 D0.
 * 동기화: 위와 같다. 다만 이 값이 pm_runtime 의 usage count 증감을 **짝으로**
 * 결정하므로, 실행 중에 값이 바뀌면 카운트가 어긋난다 — 그래서 부팅 시
 * 한 번만 설정하는 것이 전제다. */
static bool disable_idle_d3;

/* [한국어]
 * vfio_pci_eventfd_rcu_free - 교체돼 물러난 eventfd 래퍼를 유예 기간 뒤에 해제한다
 *
 * @rcu: struct vfio_pci_eventfd 안에 박혀 있는 rcu_head. container_of 로 바깥
 *       구조체를 복원한다.
 * @return: 없음.
 *
 * 왜 필요한가: err_trigger 와 req_trigger 는 RCU 로 보호되는 포인터다. 읽는 쪽
 * (vfio_pci_core_request, vfio_pci_core_aer_err_detected)은 rcu_read_lock 만 잡고
 * 포인터를 따라가므로, 쓰는 쪽이 곧바로 kfree 하면 읽는 쪽이 해제된 메모리를
 * 만진다. 그래서 교체는 즉시 하되 **해제는 모든 읽기가 끝난 뒤로 미룬다**.
 * 그 "뒤" 를 알려 주는 것이 RCU 유예 기간이고, 이 함수가 그 시점의 콜백이다.
 *
 * 동작 과정:
 *  1. rcu_head 주소에서 container_of 로 vfio_pci_eventfd 본체를 되찾는다.
 *  2. eventfd_ctx_put 으로 eventfd 컨텍스트 참조를 놓는다. 이 참조는 사용자가
 *     ioctl 로 fd 를 등록할 때 eventfd_ctx_fdget 으로 얻은 것이다.
 *  3. 래퍼 자체를 kfree 한다.
 *
 * 실행 컨텍스트: **RCU 콜백 문맥**이다. softirq 또는 rcuc 커널 스레드에서
 * 불리며 프로세스 문맥이 아니다. 잠들 수 없고, 사용자 메모리를 만질 수 없다.
 * 여기서 하는 일이 참조 해제와 kfree 둘뿐인 것은 그 제약 때문이다.
 *
 * 호출자: 이 파일에서 직접 부르는 곳은 없다. call_rcu 에 등록되어 RCU 코어가
 * 부른다. 등록 지점은 vfio_pci_eventfd_replace_locked 한 곳뿐이다.
 * 호출 대상: eventfd_ctx_put, kfree.
 *
 * 에러 경로: 없다. 이 함수는 실패할 수 없다.
 *
 * 호출 체인:
 *   vfio_pci_eventfd_replace_locked → call_rcu → (RCU 유예 기간)
 *     → [vfio_pci_eventfd_rcu_free] → eventfd_ctx_put / kfree
 */
static void vfio_pci_eventfd_rcu_free(struct rcu_head *rcu)
{
	/* [한국어] rcu_head 주소에서 바깥 구조체를 되찾는다. rcu_head 는 구조체 안에 박혀
	 * 있으므로 그 오프셋만큼 빼면 본체 주소가 나온다 — container_of 가 하는 일이다. */
	struct vfio_pci_eventfd *eventfd =
		container_of(rcu, struct vfio_pci_eventfd, rcu);

	/* [한국어] 사용자가 fd 를 등록할 때 eventfd_ctx_fdget 이 얻었던 참조를 놓는다.
	 * 이 참조가 0 이 되어야 eventfd 객체가 해제될 수 있다. */
	eventfd_ctx_put(eventfd->ctx);
	/* [한국어] 래퍼 자체를 해제한다. 이 시점에는 RCU 유예 기간이 지나 어떤 독자도
	 * 이 포인터를 들고 있지 않음이 보장된다. */
	kfree(eventfd);
}

/* [한국어]
 * vfio_pci_eventfd_replace_locked - RCU 로 보호되는 eventfd 슬롯을 안전하게 갈아 끼운다
 *
 * @vdev:      대상 디바이스. igate mutex 의 소유자이기도 하다.
 * @peventfd:  갈아 끼울 슬롯의 **주소**. 실제로는 &vdev->err_trigger 또는
 *             &vdev->req_trigger 둘 중 하나가 온다.
 * @ctx:       새로 걸 eventfd 컨텍스트. NULL 이면 "슬롯을 비운다" 는 뜻이다.
 *             호출자가 eventfd_ctx_fdget 으로 참조를 이미 얻어 둔 상태여야 하며,
 *             성공하면 그 참조의 소유권이 이 함수(정확히는 새 래퍼)로 넘어간다.
 * @return: 0 성공. -ENOMEM 이면 래퍼 할당 실패이며, 이때 슬롯은 **바뀌지 않고**
 *          @ctx 의 참조도 호출자에게 그대로 남는다 —
 *          drivers/vfio/pci/vfio_pci_intrs.c:791 의 호출자가 실패 시
 *          eventfd_ctx_put 을 하는 이유가 이것이다.
 *
 * 왜 필요한가: eventfd 를 담는 슬롯은 두 종류의 문맥에서 접근된다. 쓰는 쪽은
 * 사용자 ioctl(프로세스 문맥, igate 를 쥔다)이고, 읽는 쪽은 AER 복구 워커나
 * vfio 코어의 요청 알림(igate 를 쥐지 않는다)이다. 읽는 쪽이 락을 잡지 못하는
 * 이유는 그쪽이 잠들면 안 되는 경로이기 때문이다. 그래서 읽기는 RCU 로,
 * 쓰기는 igate 로 직렬화하고, 해제만 유예한다. 그 세 가지를 한 함수에 모았다.
 *
 * 동작 과정:
 *  1. lockdep_assert_held 로 igate 가 잡혀 있는지 확인한다(디버그 빌드 전용).
 *  2. ctx 가 있으면 래퍼를 GFP_KERNEL_ACCOUNT 로 할당해 ctx 를 담는다.
 *     포인터 하나를 위해 구조체를 따로 두는 이유는 rcu_head 를 담을 자리가
 *     필요하기 때문이다 — eventfd_ctx 는 커널 공용 타입이라 손댈 수 없다.
 *  3. rcu_replace_pointer 로 새 값을 넣고 옛 값을 받는다. 이 매크로는
 *     rcu_assign_pointer 의 메모리 배리어와 "락을 쥐고 있다" 는 lockdep 증명을
 *     한 번에 처리한다.
 *  4. 옛 값이 있으면 call_rcu 로 해제를 예약한다.
 *
 * 실행 컨텍스트: 프로세스 문맥이며 **호출 전에 vdev->igate 를 반드시 잡아야
 * 한다**. 함수 이름 끝의 _locked 가 그 계약을 나타낸다. kzalloc 이 잠들 수
 * 있으므로 spinlock 아래에서는 부를 수 없다.
 *
 * 호출자: 같은 파일의 vfio_pci_core_close_device 가 두 슬롯을 모두 NULL 로
 * 비우고, drivers/vfio/pci/vfio_pci_intrs.c 의 vfio_pci_set_ctx_trigger_single
 * (754, 781, 791줄)이 사용자의 VFIO_DEVICE_SET_IRQS 요청을 받아 부른다.
 * 호출 대상: kzalloc_obj, rcu_replace_pointer, call_rcu.
 *
 * 에러 경로: 할당 실패만이 유일한 실패다. 그 경우 아무것도 바꾸지 않고
 * -ENOMEM 을 돌려준다 — 이 "전부 아니면 전무" 성질 덕분에 호출자가 되돌릴
 * 상태가 없다.
 *
 * 호출 체인:
 *   vfio_pci_core_close_device 또는
 *   vfio_pci_intrs.c:vfio_pci_set_ctx_trigger_single
 *     → [vfio_pci_eventfd_replace_locked]
 *       → kzalloc_obj / rcu_replace_pointer / call_rcu(vfio_pci_eventfd_rcu_free)
 */
int vfio_pci_eventfd_replace_locked(struct vfio_pci_core_device *vdev,
				    struct vfio_pci_eventfd __rcu **peventfd,
				    struct eventfd_ctx *ctx)
{
	/* [한국어] 새로 만들 래퍼. ctx 가 NULL 이면 끝까지 NULL 로 남아 슬롯을 비우는 뜻이 된다. */
	struct vfio_pci_eventfd *new = NULL;
	/* [한국어] 슬롯에 들어 있던 옛 래퍼를 받아 둘 자리. 유예 해제 대상이다. */
	struct vfio_pci_eventfd *old;

	/* [한국어] 호출자가 igate 를 쥐고 있어야 한다는 계약을 디버그 빌드에서 확인한다.
	 * 이 락이 없으면 두 스레드가 동시에 슬롯을 갈아 끼워 옛 값을 잃는다. */
	lockdep_assert_held(&vdev->igate);

	/* [한국어] ctx 가 있으면 새 래퍼가 필요하고, NULL 이면 슬롯을 비우는 것이라 필요 없다. */
	if (ctx) {
		/* [한국어] GFP_KERNEL_ACCOUNT — 사용자의 ioctl 로 생긴 커널 메모리이므로 그 사용자의
		 * memcg 에 청구한다. 사용자가 커널 메모리를 무한정 쓰지 못하게 하는 장치다. */
		new = kzalloc_obj(*new, GFP_KERNEL_ACCOUNT);
		/* [한국어] 할당 실패. 아직 슬롯을 건드리지 않았으므로 그냥 반환해도 상태가 온전하다. */
		if (!new)
			/* [한국어] -ENOMEM. 호출자는 자기가 쥔 eventfd 참조를 스스로 놓는다. */
			return -ENOMEM;

		/* [한국어] eventfd 컨텍스트 참조의 소유권이 이 래퍼로 넘어온다. */
		new->ctx = ctx;
	}

	/* [한국어] 슬롯을 새 값으로 원자적으로 바꾸고 옛 값을 받는다. 이 매크로가
	 * rcu_assign_pointer 의 저장 배리어(래퍼 내용이 포인터 공개보다 먼저 보이게)를
	 * 포함하므로, RCU 독자가 반쯤 채워진 래퍼를 보는 일이 없다. */
	old = rcu_replace_pointer(*peventfd, new,
				  /* [한국어] 락을 쥐고 있다는 증명. lockdep 이 켜진 빌드에서 이 조건이 거짓이면 경고한다. */
				  lockdep_is_held(&vdev->igate));
	/* [한국어] 옛 값이 없었다면(처음 등록) 해제할 것도 없다. */
	if (old)
		/* [한국어] 지금 kfree 하면 rcu_read_lock 구간의 독자가 해제된 메모리를 만진다.
		 * 유예 기간이 지난 뒤 vfio_pci_eventfd_rcu_free 가 해제하도록 예약한다. */
		call_rcu(&old->rcu, vfio_pci_eventfd_rcu_free);

	/* [한국어] 슬롯 교체는 실패할 수 없으므로 여기까지 왔으면 항상 성공이다. */
	return 0;
}

/* [한국어] SR-IOV 를 켠 적이 있는 PF 들의 전역 목록과 그것을 지키는 mutex.
 * 왜 전역이어야 하는가: VF 가 probe 될 때 자기 PF 의 vfio_pci_core_device 를
 * 찾아야 하는데, VF 쪽에서 PF 쪽 객체로 가는 포인터가 PCI 코어에는 없다.
 * 그래서 PF 가 SR-IOV 를 켤 때 자신을 이 목록에 등록해 두고,
 * vfio_pci_vf_init 이 pci_physfn 으로 얻은 pdev 를 열쇠 삼아 목록을 뒤진다. */
/* List of PF's that vfio_pci_core_sriov_configure() has been called on */
/* [한국어] 목록을 지키는 전역 mutex. 잡는 곳은 셋 — vfio_pci_core_sriov_configure 의
 * 등록/해제와 vfio_pci_vf_init 의 조회. spinlock 이 아니라 mutex 인 이유는
 * 이 구간이 짧지 않고 프로세스 문맥에서만 불리기 때문이다. */
static DEFINE_MUTEX(vfio_pci_sriov_pfs_mutex);
/* [한국어] 목록의 머리. 원소는 struct vfio_pci_core_device 의 sriov_pfs_item 필드다.
 * 비어 있는 항목(list_del_init 로 초기화된 상태)인지로 "이 PF 가 이미
 * SR-IOV 를 켰는가" 를 판정하는 것이 vfio_pci_core_sriov_configure 의 핵심
 * 가드다. */
static LIST_HEAD(vfio_pci_sriov_pfs);

/* [한국어] 페이지보다 작은 BAR 때문에 생기는 구멍을 막는 자리표.
 * 왜 필요한가: mmap 의 최소 단위는 페이지인데, PCI BAR 는 16바이트짜리도 있다.
 * 그런 BAR 를 사용자에게 mmap 으로 내주면 같은 페이지의 나머지 바이트까지
 * 노출되고, 그 나머지에 나중에 hot-add 된 다른 장치의 BAR 가 배정되면 사용자가
 * 남의 장치 레지스터를 만지게 된다. 그래서 나머지 구간을 미리 커널 자원으로
 * 예약해 아무도 못 가져가게 한다. 생성은 vfio_pci_probe_mmaps, 해제는
 * vfio_pci_core_disable. */
struct vfio_pci_dummy_resource {
	/* [한국어] 예약할 구간을 나타내는 리소스 서술자.
	 * 설정자: vfio_pci_probe_mmaps 가 name 을 "vfio sub-page reserved" 로,
	 * start 를 진짜 BAR 의 끝 다음 바이트로, end 를 그 페이지의 마지막 바이트로,
	 * flags 를 원래 BAR 의 flags 그대로 채운 뒤 request_resource 로 등록한다.
	 * 읽는 자: 커널 자원 트리(iomem_resource) 자체. /proc/iomem 에도 보인다.
	 * 값 범위: 항상 진짜 BAR 뒤쪽의 페이지 나머지. 크기는 1바이트 이상
	 * PAGE_SIZE 미만.
	 * 동기화: 자원 트리 등록/해제는 request_resource 와 release_resource 가
	 * 내부 락으로 처리한다. 이 구조체 자체는 열고 닫는 동안 한 스레드만 만진다. */
	struct resource		resource;
	/* [한국어] 이 자리표가 어느 BAR 를 위한 것인지 나타내는 번호(PCI_STD_RESOURCES 기준의
	 * 절대 인덱스, 보통 0~5).
	 * 설정자: vfio_pci_probe_mmaps 가 dummy_res->index = bar 로 채운다.
	 * 읽는 자: [상류 코드 관찰] 이 필드를 읽는 코드를 drivers/vfio 전체에서 찾지
	 * 못했다. 해제는 리스트 순회로만 하므로 인덱스가 필요 없다. 디버깅용으로
	 * 남아 있는 것으로 보인다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
	 * 값 범위: 0 이상 PCI_STD_NUM_BARS 미만.
	 * 동기화: 위와 같다. */
	int			index;
	/* [한국어] 디바이스별 자리표 목록(vdev->dummy_resources_list)에 매다는 고리.
	 * 설정자: vfio_pci_probe_mmaps 의 list_add.
	 * 읽는 자: vfio_pci_core_disable 의 list_for_each_entry_safe 가 순회하며
	 * release_resource 후 kfree 한다.
	 * 값 범위: 리스트 노드. 디바이스당 0개에서 최대 6개(BAR 수)까지.
	 * 동기화: 이 리스트는 디바이스를 여는 동안에만 만들어지고 닫을 때만 지워지며,
	 * 그 두 시점은 vfio_main.c 의 open_count 0↔1 전이라 겹치지 않는다.
	 * 그래서 별도 락이 없다. */
	struct list_head	res_next;
};

/* [한국어] SR-IOV PF 하나가 들고 있는 "이 PF 와 VF 들은 서로를 믿는다" 는 증표.
 * 왜 필요한가: PF 드라이버는 언제든 VF 를 리셋하거나 VF 를 지나가는 데이터를
 * 볼 수 있다. 그러므로 PF 를 사용자 공간에 넘긴 상태에서 VF 를 다른 사용자에게
 * 주면, VF 사용자는 자기도 모르게 낯선 사용자를 신뢰하게 된다. VFIO 는 그
 * 신뢰를 UUID 하나를 공유하는 것으로 **명시적 opt-in** 시킨다.
 * 생성은 vfio_pci_vf_init(PF 일 때만), 소멸은 vfio_pci_vf_uninit. */
struct vfio_pci_vf_token {
	/* [한국어] 아래 두 필드를 함께 지키는 mutex.
	 * 설정자: vfio_pci_vf_init 의 mutex_init, vfio_pci_vf_uninit 의 mutex_destroy.
	 * 읽는 자: vfio_pci_core_feature_token(사용자가 토큰을 새로 설정),
	 * vfio_pci_core_match_token_uuid(토큰 대조), 그리고 VF 쪽의
	 * vfio_pci_core_finish_enable 과 vfio_pci_core_close_device 의 users 증감.
	 * 값 범위: 표준 mutex.
	 * 동기화: PF 의 토큰을 VF 스레드가 읽고 PF 스레드가 쓰므로 반드시 필요하다.
	 * 주의할 점은 **VF 가 PF 의 락을 잡는다**는 것 — 락 소유자는 PF 객체이지만
	 * 잡는 쪽은 VF 의 open/close 경로다. */
	struct mutex		lock;
	/* [한국어] 공유 비밀 UUID.
	 * 설정자: vfio_pci_vf_init 이 uuid_gen 으로 무작위 값을 만들어 시작하고,
	 * 사용자가 VFIO_DEVICE_FEATURE_PCI_VF_TOKEN 으로 덮어쓰거나
	 * (vfio_pci_core_feature_token), VF 사용자가 없을 때 PF 를 여는 사용자가
	 * 제시한 값으로 설정된다(vfio_pci_core_match_token_uuid).
	 * 읽는 자: vfio_pci_core_match_token_uuid 의 uuid_equal 대조.
	 * 값 범위: 임의의 128비트 값. 초기값은 무작위라, 사용자가 명시적으로 설정하기
	 * 전에는 VF 를 여는 것이 사실상 불가능하다.
	 * 동기화: 위 lock. GET 은 지원하지 않는다 — 이전 사용자의 토큰이 새 사용자에게
	 * 새는 것을 막기 위해서다(vfio_pci_core_feature_token 의 영어 주석). */
	uuid_t			uuid;
	/* [한국어] 지금 이 PF 의 VF 를 열어 둔 사용자 수.
	 * 설정자: VF 쪽 vfio_pci_core_finish_enable 이 ++, vfio_pci_core_close_device
	 * 가 -- 한다. 즉 **PF 의 필드를 VF 가 갱신한다**.
	 * 읽는 자: vfio_pci_core_match_token_uuid 가 "VF 사용자가 있는가" 로 PF 를 여는
	 * 사용자에게 토큰을 요구할지 정할 때. vfio_pci_vf_uninit 의 WARN_ON.
	 * 값 범위: 0 이상. 0 이면 PF 를 여는 사용자가 토큰을 **설정**할 수 있고,
	 * 1 이상이면 기존 토큰을 **맞혀야** 한다.
	 * 동기화: 위 lock 아래에서만 증감한다. */
	int			users;
};

/* [한국어]
 * vfio_vga_disabled - 레거시 VGA 지원이 꺼져 있는지 한 줄로 판정한다
 *
 * @return: true 면 VGA 지원을 하지 않는다. false 면 disable_vga 모듈 파라미터가
 *          거짓이므로 VGA region 을 노출한다.
 *
 * 왜 필요한가: VGA 지원 여부는 두 가지에 달려 있다 — 컴파일 시
 * CONFIG_VFIO_PCI_VGA 가 켜졌는지, 그리고 실행 시 disable_vga 파라미터가
 * 어떤지. 두 조건을 호출자마다 되풀이하지 않으려고 한 곳에 모았다.
 * CONFIG 가 꺼져 있으면 상수 true 를 돌려주므로 컴파일러가 호출 측 분기를
 * 아예 죽은 코드로 지운다.
 *
 * 동작 과정: #ifdef 로 갈라, 켜져 있으면 전역 disable_vga 를 그대로,
 * 꺼져 있으면 무조건 true 를 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. inline 이라 호출 비용이 없고 락도 잡지 않는다.
 *
 * 호출자: vfio_pci_set_decode(VGA 중재자 콜백)와 vfio_pci_core_enable
 * (has_vga 를 세울지 결정) 두 곳.
 * 호출 대상: 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_set_decode 또는 vfio_pci_core_enable → [vfio_vga_disabled]
 */
static inline bool vfio_vga_disabled(void)
{
/* [한국어] 빌드 시점에 VGA 지원을 골라낸다. 꺼져 있으면 아래 상수 true 가 남아
 * 호출 측 분기가 컴파일러에 의해 죽은 코드로 제거된다. */
#ifdef CONFIG_VFIO_PCI_VGA
	/* [한국어] 실행 시 스위치를 그대로 돌려준다. 관리자가 vfio-pci 의 disable_vga
	 * 모듈 파라미터로 바꾼 값이다. */
	return disable_vga;
/* [한국어] CONFIG_VFIO_PCI_VGA 가 꺼진 빌드. */
#else
	/* [한국어] VGA region 을 아예 제공하지 않는 빌드이므로 항상 "비활성" 이다. */
	return true;
#endif
}

/* [한국어]
 * vfio_pci_set_decode - VGA 중재자에게 "이 장치가 어떤 자원을 디코드하는가" 를 알린다
 *
 * @pdev:       질문 대상 PCI 장치.
 * @single_vga: 이 장치가 시스템에서 유일한 VGA 장치라고 중재자가 판단했는지.
 * @return: VGA_RSRC_ 계열 비트의 OR. 중재자는 이 값으로 어느 장치가 레거시
 *          VGA 자원을 놓고 다투는지 계산한다.
 *
 * 왜 필요한가: 레거시 VGA 자원(0x3B0~0x3DF I/O 포트와 0xA0000~0xBFFFF 메모리)은
 * 주소가 고정이라 여러 그래픽 카드가 동시에 디코드할 수 없다. 그래서
 * drivers/pci/vgaarb.c 의 중재자가 순서를 정하는데, 그러려면 각 드라이버가
 * 자기 장치의 디코드 범위를 신고해야 한다. VFIO 는 장치 내부를 모르므로
 * 정직하게 신고할 수가 없다 — 그래서 **안전한 쪽으로만** 답한다.
 *
 * 동작 과정(원본 영어 주석이 설명하는 논리를 그대로):
 *  1. 유일한 VGA 장치이거나, VGA 지원이 켜져 있거나, 루트 버스에 직결된
 *     장치이면 아무것도 감출 수 없으므로 네 비트를 모두 신고한다.
 *  2. 그렇지 않으면(다른 VGA 장치가 있고, VFIO 의 VGA 지원이 꺼졌고,
 *     브리지 아래에 있으면) 일단 normal 두 비트만 신고한다. 사용자는 어차피
 *     레거시 자원에 닿을 수 없고, 브리지에서 라우팅을 끊을 수 있기 때문이다.
 *  3. 다만 **같은 브리지 아래에 다른 VGA 장치가 또 있으면** 그 안전 가정이
 *     깨진다. 그래서 pci_get_class 로 시스템의 모든 VGA 장치를 훑어, 같은
 *     도메인의 pdev 버스 번호와 max_busnr 사이에 다른 VGA 장치가 있으면
 *     legacy 두 비트를 도로 켠다.
 *
 * 실행 컨텍스트: 프로세스 문맥. VGA 중재자가 콜백으로 부르며, 등록 시점
 * (vfio_pci_vga_init)에도 한 번 직접 부른다. 루프 안에서 pci_get_class 가
 * 참조를 잡아 주므로 순회 중 장치가 사라지지 않는다.
 *
 * 호출자: drivers/pci/vgaarb.c 의 중재자(vga_client_register 로 등록됨,
 * vgaarb.c:2812)와 이 파일의 vfio_pci_vga_init.
 * 호출 대상: vfio_vga_disabled, pci_is_root_bus, pci_bus_max_busnr
 * (drivers/pci/pci.c:379), pci_get_class(drivers/pci/search.c:746),
 * pci_domain_nr, pci_dev_put.
 *
 * 에러 경로: 실패가 없다. 판단이 어려우면 "더 많이 디코드한다" 는 쪽으로
 * 기울어 안전을 택한다.
 *
 * 호출 체인:
 *   vgaarb.c 중재자 또는 vfio_pci_vga_init → [vfio_pci_set_decode]
 *     → pci_bus_max_busnr / pci_get_class / pci_dev_put
 */
/*
 * Our VGA arbiter participation is limited since we don't know anything
 * about the device itself.  However, if the device is the only VGA device
 * downstream of a bridge and VFIO VGA support is disabled, then we can
 * safely return legacy VGA IO and memory as not decoded since the user
 * has no way to get to it and routing can be disabled externally at the
 * bridge.
 */
static unsigned int vfio_pci_set_decode(struct pci_dev *pdev, bool single_vga)
{
	/* [한국어] pci_get_class 순회의 커서. NULL 로 시작하면 처음부터 훑는다.
	 * 매 반복에서 pci_get_class 가 이전 항목의 참조를 놓고 다음 항목의 참조를 잡는다. */
	struct pci_dev *tmp = NULL;
	/* [한국어] 이 장치가 속한 버스 하위의 가장 큰 버스 번호. 즉 브리지 아래 범위의 끝이다. */
	unsigned char max_busnr;
	/* [한국어] 중재자에게 신고할 비트 모음. 아래에서 조건에 따라 legacy 비트가 더해진다. */
	unsigned int decodes;

	/* [한국어] 감출 수 없는 세 경우다. 유일한 VGA 면 감출 이유가 없고, VFIO 의 VGA
	 * 지원이 켜져 있으면 사용자가 실제로 레거시 자원에 접근하며, 루트 버스
	 * 직결이면 라우팅을 끊어 줄 브리지가 없다. */
	if (single_vga || !vfio_vga_disabled() || pci_is_root_bus(pdev->bus))
		/* [한국어] 네 비트를 모두 신고한다 — normal I/O, normal 메모리, legacy I/O, legacy 메모리. */
		return VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM |
		       /* [한국어] 레거시 두 비트. 0x3B0~0x3DF 포트와 0xA0000~0xBFFFF 메모리를 뜻한다. */
		       VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;

	/* [한국어] 이 장치가 달린 버스 아래 계층의 마지막 버스 번호를 구한다.
	 * 구현은 drivers/pci/pci.c:379 에 있고 주석 완료 상태다. */
	max_busnr = pci_bus_max_busnr(pdev->bus);
	/* [한국어] 일단 normal 두 비트만 신고하는 것으로 시작한다. 레거시 자원은
	 * "이 장치가 디코드하지 않는다" 고 답하는 셈이라 중재 부담이 줄어든다. */
	decodes = VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM;

	/* [한국어] 시스템의 모든 VGA 클래스 장치를 훑는다. 클래스 코드를 8비트 왼쪽으로
	 * 미는 이유는 pci_get_class 가 24비트 클래스(base/sub/progif)를 받는데
	 * PCI_CLASS_DISPLAY_VGA 는 상위 16비트만 담고 있기 때문이다.
	 * progif 자리 8비트를 0 으로 채워 넣는 것이 이 시프트다. */
	while ((tmp = pci_get_class(PCI_CLASS_DISPLAY_VGA << 8, tmp)) != NULL) {
		/* [한국어] 자기 자신은 비교 대상이 아니다. */
		if (tmp == pdev ||
		    /* [한국어] 다른 PCI 도메인(세그먼트)의 장치는 레거시 자원을 공유하지 않는다. */
		    pci_domain_nr(tmp->bus) != pci_domain_nr(pdev->bus) ||
		    /* [한국어] 루트 버스 직결 장치는 이 브리지 아래가 아니므로 무관하다. */
		    pci_is_root_bus(tmp->bus))
			/* [한국어] 위 셋 중 하나면 다음 장치로 넘어간다. 이때 tmp 의 참조는
			 * 다음 pci_get_class 호출이 대신 놓아 준다. */
			continue;

		/* [한국어] 이 장치의 버스 번호가 pdev 의 버스 번호 이상이고 */
		if (tmp->bus->number >= pdev->bus->number &&
		    /* [한국어] 브리지 아래 범위의 끝 이하이면 — 같은 브리지 아래에 있다는 뜻이다. */
		    tmp->bus->number <= max_busnr) {
			/* [한국어] 루프를 벗어나기 전에 참조를 직접 놓는다. break 하면 다음
			 * pci_get_class 호출이 없어 아무도 대신 놓아 주지 않기 때문이다. */
			pci_dev_put(tmp);
			/* [한국어] 같은 브리지 아래에 다른 VGA 장치가 있으므로 레거시 자원을 감추는
			 * 가정이 깨진다. 두 비트를 도로 켜서 중재자가 정상적으로 중재하게 한다. */
			decodes |= VGA_RSRC_LEGACY_IO | VGA_RSRC_LEGACY_MEM;
			/* [한국어] 하나만 찾으면 결론이 나므로 더 볼 필요가 없다. */
			break;
		}
	}

	/* [한국어] 완성된 디코드 비트를 중재자에게 돌려준다. */
	return decodes;
}

/* [한국어]
 * vfio_pci_probe_mmaps - BAR 여섯 개가 각각 mmap 가능한지 판정해 표를 채운다
 *
 * @vdev: 대상 디바이스. 결과를 vdev->bar_mmap_supported 배열에 남기고,
 *        필요하면 vdev->dummy_resources_list 에 자리표를 매단다.
 * @return: 없음. 실패는 "그 BAR 는 mmap 불가" 로 흡수된다.
 *
 * 왜 필요한가: **이 파일에서 BAR mmap 정책이 정해지는 곳이다.** 사용자에게
 * BAR 물리 페이지를 그대로 열어 주려면 그 페이지에 다른 장치의 자원이 섞여
 * 있지 않아야 한다. PCI BAR 는 페이지보다 작을 수 있으므로 그 판정을 BAR 마다
 * 따로 해야 하고, 그 결과가 GET_REGION_INFO 의 VFIO_REGION_INFO_FLAG_MMAP
 * 플래그와 vfio_pci_core_mmap 의 허용 여부를 동시에 결정한다.
 *
 * 동작 과정(BAR 0~5 각각에 대해):
 *  1. pdev->non_mappable_bars 가 서 있으면 무조건 불가. 이 플래그의 의미는
 *     drivers/pci/pci-sysfs.c:3496~3537 과 drivers/pci/proc.c:702~746 에 이미
 *     주석돼 있다 — 그쪽에서도 같은 이유로 mmap 을 막는다.
 *  2. IORESOURCE_MEM 이 아니면(즉 I/O 포트 BAR 이면) 불가. I/O 포트는 애초에
 *     메모리 매핑 대상이 아니다.
 *  3. 크기가 0 이면 불가. 원본 주석이 말하듯 정상적으로는 생기지 않아야 할
 *     상태지만 방어한다.
 *  4. 크기가 PAGE_SIZE 이상이면 가능. 가장 흔한 경우다.
 *  5. 크기는 작지만 **페이지 경계에서 시작**하면, 그 페이지의 나머지를
 *     dummy resource 로 예약해 아무도 못 쓰게 만든 뒤 가능으로 표시한다.
 *     예약에 실패하면 불가로 떨어진다.
 *  6. 페이지 정렬조차 안 되어 있으면 불가. 원본 주석대로 게스트가 같은
 *     페이지 오프셋에 BAR 를 놓는다는 보장이 없고, 사용자가 페이지 안의
 *     위치를 알 방법도 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. vfio_pci_core_finish_enable 에서만 불리며,
 * 그 시점은 사용자가 아직 아무것도 mmap 하지 않은 때다. kzalloc 이 잠들 수
 * 있으므로 락을 쥐지 않고 부른다.
 *
 * 호출자: vfio_pci_core_finish_enable.
 * 호출 대상: resource_size, kzalloc_obj, request_resource, kfree, list_add.
 *
 * 에러 경로: 할당 실패도 자원 예약 실패도 모두 goto no_mmap 으로 모여
 * "그 BAR 는 mmap 불가" 로 끝난다. 디바이스 열기 자체는 실패하지 않는다 —
 * mmap 이 안 되면 사용자는 read/write 로 접근하면 되기 때문이다.
 *
 * 호출 체인:
 *   vendor open_device → vfio_pci_core_finish_enable → [vfio_pci_probe_mmaps]
 *     → request_resource
 */
static void vfio_pci_probe_mmaps(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 지금 보고 있는 BAR 의 리소스 서술자를 가리킬 임시 포인터. */
	struct resource *res;
	/* [한국어] BAR 인덱스 0~5 를 도는 루프 변수. */
	int i;
	/* [한국어] 페이지의 나머지를 예약할 때 만드는 자리표. */
	struct vfio_pci_dummy_resource *dummy_res;

	/* [한국어] 표준 BAR 여섯 개만 본다. ROM BAR 와 SR-IOV BAR 는 대상이 아니다. */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		/* [한국어] pci_dev->resource 배열에서 BAR 가 시작하는 오프셋을 더해 절대 인덱스를 만든다.
		 * PCI_STD_RESOURCES 는 그 배열에서 표준 BAR 가 시작하는 자리다. */
		int bar = i + PCI_STD_RESOURCES;

		/* [한국어] 그 BAR 의 리소스 서술자. 호스트가 실제로 배정한 물리 주소 범위가 여기 있다. */
		res = &vdev->pdev->resource[bar];

		/* [한국어] PCI 코어가 "이 장치의 BAR 는 사용자에게 매핑하면 안 된다" 고 표시한 경우다.
		 * 같은 플래그를 drivers/pci/pci-sysfs.c:3537 과 drivers/pci/proc.c:746 도
		 * 같은 이유로 검사한다. */
		if (vdev->pdev->non_mappable_bars)
			/* [한국어] mmap 불가로 표시하러 간다. */
			goto no_mmap;

		/* [한국어] I/O 포트 BAR 는 메모리 주소 공간에 없으므로 mmap 대상이 아니다.
		 * 사용자는 read/write 로만 접근한다. */
		if (!(res->flags & IORESOURCE_MEM))
			/* [한국어] mmap 불가. */
			goto no_mmap;

		/*
		 * The PCI core shouldn't set up a resource with a
		 * type but zero size. But there may be bugs that
		 * cause us to do that.
		 */
		/* [한국어] 크기 0 인 자원. 원본 주석대로 정상적으로는 없어야 하지만 방어한다. */
		if (!resource_size(res))
			/* [한국어] mmap 불가. */
			goto no_mmap;

		/* [한국어] 페이지 크기 이상이면 이 BAR 만으로 페이지가 채워지므로 남의 자원이
		 * 섞일 여지가 없다. 가장 흔한 경우다. */
		if (resource_size(res) >= PAGE_SIZE) {
			/* [한국어] mmap 허용으로 표시한다. 이 값이 GET_REGION_INFO 의 MMAP 플래그와
			 * vfio_pci_core_mmap 의 허용 여부를 동시에 결정한다. */
			vdev->bar_mmap_supported[bar] = true;
			/* [한국어] 다음 BAR 로. */
			continue;
		}

		/* [한국어] PAGE_MASK 를 뒤집으면 페이지 안 오프셋 비트만 남는다. 그것이 0 이면
		 * BAR 가 페이지 경계에서 시작한다는 뜻이다. 페이지 앞쪽은 이 BAR 가
		 * 독점하므로, 뒤쪽만 막으면 페이지 전체를 안전하게 내줄 수 있다. */
		if (!(res->start & ~PAGE_MASK)) {
			/*
			 * Add a dummy resource to reserve the remainder
			 * of the exclusive page in case that hot-add
			 * device's bar is assigned into it.
			 */
			/* [한국어] 페이지의 나머지를 예약할 자리표를 만든다. 사용자 요청으로 생긴
			 * 커널 메모리이므로 GFP_KERNEL_ACCOUNT 로 memcg 에 청구한다. */
			dummy_res = kzalloc_obj(*dummy_res, GFP_KERNEL_ACCOUNT);
			/* [한국어] 할당 실패면 예약을 못 하므로 안전하게 mmap 불가로 떨어진다. */
			if (dummy_res == NULL)
				/* [한국어] mmap 불가. */
				goto no_mmap;

			/* [한국어] /proc/iomem 에 보일 이름. 이 예약이 무엇인지 관리자가 알 수 있게 한다. */
			dummy_res->resource.name = "vfio sub-page reserved";
			/* [한국어] 예약 구간의 시작은 진짜 BAR 가 끝난 바로 다음 바이트다. */
			dummy_res->resource.start = res->end + 1;
			/* [한국어] 끝은 그 페이지의 마지막 바이트. BAR 가 페이지 시작에 정렬돼 있으므로
			 * res->start 에 PAGE_SIZE-1 을 더하면 정확히 그 페이지의 끝이다. */
			dummy_res->resource.end = res->start + PAGE_SIZE - 1;
			/* [한국어] 원래 BAR 와 같은 플래그를 쓴다. 자원 트리는 종류가 맞아야 부모 아래
			 * 들어갈 수 있다. */
			dummy_res->resource.flags = res->flags;
			/* [한국어] 부모 자원(대개 이 BAR 가 속한 브리지 창) 아래에 예약을 등록한다.
			 * 성공하면 그 구간은 다른 장치에게 배정되지 않는다. */
			if (request_resource(res->parent,
						&dummy_res->resource)) {
				/* [한국어] 등록 실패 — 이미 누가 그 구간을 쓰고 있다는 뜻이다. 자리표를 버린다. */
				kfree(dummy_res);
				/* [한국어] 예약하지 못했으므로 mmap 불가. */
				goto no_mmap;
			}
			/* [한국어] 어느 BAR 를 위한 예약인지 기록한다(이 값을 읽는 코드는 없다). */
			dummy_res->index = bar;
			/* [한국어] 디바이스별 목록에 매단다. vfio_pci_core_disable 이 이 목록을 순회하며
			 * 예약을 풀고 해제한다. */
			list_add(&dummy_res->res_next,
					&vdev->dummy_resources_list);
			/* [한국어] 페이지의 나머지를 막았으므로 이제 안전하게 mmap 을 허용한다. */
			vdev->bar_mmap_supported[bar] = true;
			/* [한국어] 다음 BAR 로. */
			continue;
		}
		/*
		 * Here we don't handle the case when the BAR is not page
		 * aligned because we can't expect the BAR will be
		 * assigned into the same location in a page in guest
		 * when we passthrough the BAR. And it's hard to access
		 * this BAR in userspace because we have no way to get
		 * the BAR's location in a page.
		 */
no_mmap:
		/* [한국어] 위 어느 조건도 만족하지 못했으니 이 BAR 는 mmap 할 수 없다.
		 * 사용자는 read/write 로만 접근하게 된다. */
		vdev->bar_mmap_supported[bar] = false;
	}
}

/* [한국어] 아래 두 함수가 이 파일 뒤쪽(리셋 절)에 정의돼 있어 미리 선언해 둔다.
 * struct vfio_pci_group_info 도 실제 정의는 뒤에 있으므로 여기서는 태그만
 * 선언해 포인터 타입을 쓸 수 있게 한다(불완전 타입 선언). */
struct vfio_pci_group_info;
/* [한국어] vfio_pci_core_disable 이 마지막에 부르는 "묶음 전체가 놀고 있으면 버스
 * 리셋을 시도한다" 함수. 정의는 이 파일 끝부분에 있다. */
static void vfio_pci_dev_set_try_reset(struct vfio_device_set *dev_set);
/* [한국어] VFIO_DEVICE_PCI_HOT_RESET ioctl 의 실체. 정의는 이 파일 끝부분에 있으며,
 * groups(1세대 소유 증명)와 iommufd_ctx(2세대 소유 증명) 중 하나만 채워
 * 부른다. */
static int vfio_pci_dev_set_hot_reset(struct vfio_device_set *dev_set,
				      struct vfio_pci_group_info *groups,
				      struct iommufd_ctx *iommufd_ctx);

/* [한국어]
 * vfio_pci_nointx - INTx 를 아예 없는 것처럼 감춰야 하는 장치인지 판정한다
 *
 * @pdev: 검사할 PCI 장치.
 * @return: true 면 이 장치의 INTx 를 감춘다. false 면 정상 처리.
 *
 * 왜 필요한가: 원본 영어 주석이 배경을 설명한다. PCI 2.3 식 INTx 마스킹은
 * 두 가지 능력을 함께 요구한다 — PCI_COMMAND 의 Interrupt Disable 비트로
 * 신호를 끌 수 있어야 하고, PCI_STATUS 의 Interrupt Status 비트로 지금 이
 * 장치가 INTx 를 올렸는지 알 수 있어야 한다. 뒤쪽이 없으면 인터럽트 선을
 * 공유할 수 없어 전용 인터럽트를 요구하게 되는데, VFIO 는 Disable 비트는
 * 제어할 수 있으므로 **차라리 INTx 자체가 없는 척** 하는 편이 낫다. 그러면
 * 사용자는 MSI/MSI-X 만 쓰고, 호스트는 DisINTx 를 계속 세워 둔다.
 *
 * 동작 과정: 벤더가 인텔인 경우에만 장치 ID 를 본다. i40e 계열
 * (XL710/X710/XXV710 10/20/25/40GbE NIC)의 ID 대역과 X550(0x1563)이 대상이다.
 * 그 밖의 모든 장치는 false.
 *
 * 실행 컨텍스트: 프로세스 문맥. vfio_pci_core_enable 에서 한 번만 불린다.
 *
 * 호출자: vfio_pci_core_enable.
 * 호출 대상: 없다. pdev 의 두 필드만 본다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vendor open_device → vfio_pci_core_enable → [vfio_pci_nointx]
 */
/*
 * INTx masking requires the ability to disable INTx signaling via PCI_COMMAND
 * _and_ the ability detect when the device is asserting INTx via PCI_STATUS.
 * If a device implements the former but not the latter we would typically
 * expect broken_intx_masking be set and require an exclusive interrupt.
 * However since we do have control of the device's ability to assert INTx,
 * we can instead pretend that the device does not implement INTx, virtualizing
 * the pin register to report zero and maintaining DisINTx set on the host.
 */
static bool vfio_pci_nointx(struct pci_dev *pdev)
{
	/* [한국어] 벤더 ID 로 먼저 가른다. 인텔 말고는 이 목록에 없다. */
	switch (pdev->vendor) {
	/* [한국어] 인텔 장치만 세부 ID 를 검사한다. */
	case PCI_VENDOR_ID_INTEL:
		/* [한국어] 장치 ID 별 분기. 아래 값들은 PCI Device ID 이며 스펙이 아니라
		 * 실제 하드웨어의 결함 목록이다. */
		switch (pdev->device) {
		/* All i40e (XL710/X710/XXV710) 10/20/25/40GbE NICs */
		/* [한국어] i40e 계열 첫 항목. */
		case 0x1572:
		/* [한국어] i40e 계열. */
		case 0x1574:
		/* [한국어] GCC 의 case 범위 확장. 0x1580 부터 0x1581 까지를 한 줄로 적는다. */
		case 0x1580 ... 0x1581:
		/* [한국어] 0x1583~0x158b 구간의 i40e 변종들. */
		case 0x1583 ... 0x158b:
		/* [한국어] 0x37d0~0x37d2 구간의 i40e 변종들. */
		case 0x37d0 ... 0x37d2:
		/* X550 */
		/* [한국어] X550 10GbE 컨트롤러. */
		case 0x1563:
			/* [한국어] 이 장치들은 PCI_STATUS 의 Interrupt Status 를 제대로 보고하지 않으므로
			 * INTx 자체를 감춘다. */
			return true;
		/* [한국어] 같은 벤더의 다른 장치들. */
		default:
			/* [한국어] 정상 처리한다. */
			return false;
		}
	}

	/* [한국어] 인텔이 아닌 모든 벤더는 정상 처리한다. */
	return false;
}

/* [한국어]
 * vfio_pci_probe_power_state - 이 장치가 D3 에서 깨어날 때 소프트 리셋을 하는지 조사한다
 *
 * @vdev: 대상 디바이스. 결과를 vdev->needs_pm_restore 에 남긴다.
 * @return: 없음.
 *
 * 왜 필요한가: PCI 전원 관리 스펙의 PMCSR 레지스터에는 No_Soft_Reset 비트가
 * 있다. 이 비트가 **0** 이면(즉 NoSoftRst-) 장치는 D3hot 에서 D0 로 돌아올 때
 * 내부 상태를 초기화한다. 그런 장치는 전원 전이 전후로 config 공간을 직접
 * 저장/복원해 주지 않으면 사용자가 설정해 둔 값이 조용히 날아간다.
 * 그 저장본이 vdev->pm_save 이고, 이 함수가 그것이 필요한지를 미리 판정한다.
 *
 * 동작 과정:
 *  1. pdev->pm_cap 이 0 이면(PM capability 자체가 없으면) 아무것도 하지 않는다.
 *     needs_pm_restore 는 0 으로 남는다 — 전원 전이를 할 수 없는 장치이므로
 *     복원할 일도 없다.
 *  2. PM capability 오프셋 + PCI_PM_CTRL 에서 PMCSR 워드를 읽는다.
 *  3. PCI_PM_CTRL_NO_SOFT_RESET 비트를 뒤집어 needs_pm_restore 에 넣는다.
 *     즉 **비트가 없을 때 복원이 필요하다**.
 *
 * 실행 컨텍스트: 프로세스 문맥. vfio_pci_core_register_device 에서 probe 중에
 * 한 번만 불린다. 이때 config 접근은 중재를 거치지 않는 **커널 자신의 접근**
 * 이다 — 사용자 경로가 아니다.
 *
 * 호출자: vfio_pci_core_register_device.
 * 호출 대상: pci_read_config_word(drivers/pci/access.c 의 config 접근기).
 *
 * 에러 경로: pci_read_config_word 의 실패를 검사하지 않는다. 실패하면 pmcsr 이
 * 불정값이 되지만, 최악의 경우 불필요한 저장/복원을 하는 정도라 안전 쪽으로
 * 기운다.
 *
 * 호출 체인:
 *   vfio_pci.c probe → vfio_pci_core_register_device
 *     → [vfio_pci_probe_power_state] → pci_read_config_word
 */
static void vfio_pci_probe_power_state(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터를 지역에 둔다. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] Power Management Control/Status 레지스터 값을 담을 임시 변수. */
	u16 pmcsr;

	/* [한국어] PM capability 자체가 없는 장치. 전원 전이를 할 수 없으니 복원할 일도 없다. */
	if (!pdev->pm_cap)
		/* [한국어] needs_pm_restore 는 0 으로 남는다. */
		return;

	/* [한국어] PM capability 시작 오프셋에 PCI_PM_CTRL(=4)을 더한 자리가 PMCSR 이다.
	 * PCI 전원 관리 스펙이 정한 배치이며, 이 접근은 커널 자신의 config 접근이라
	 * 사용자 중재 경로를 거치지 않는다. */
	pci_read_config_word(pdev, pdev->pm_cap + PCI_PM_CTRL, &pmcsr);

	/* [한국어] No_Soft_Reset 비트가 **없을 때** 복원이 필요하다. 그 비트가 0 이면
	 * D3hot 에서 D0 로 돌아올 때 장치가 내부 상태를 잃기 때문이다. */
	vdev->needs_pm_restore = !(pmcsr & PCI_PM_CTRL_NO_SOFT_RESET);
}

/* [한국어]
 * vfio_pci_set_power_state - NoSoftRst- 장치를 위해 저장/복원을 덧댄 전원 전이 래퍼
 *
 * @vdev:  대상 디바이스.
 * @state: 옮겨 갈 전원 상태(PCI_D0 부터 PCI_D3cold 까지).
 * @return: 0 성공. -EBUSY 는 VF 가 살아 있는 PF 를 D0 아래로 내리려 한 경우.
 *          그 밖에는 pci_set_power_state 의 반환값을 그대로 전달한다.
 *
 * 왜 필요한가: 원본 영어 주석이 셋을 말한다. (1) D3->D0 에서 소프트 리셋을
 * 하는 장치는 config 를 잃으므로 직접 저장/복원해야 한다. (2) 그 저장본을
 * vdev->pm_save 에 따로 두는 이유는 PM capability 에뮬레이션이 그것을 쓰기
 * 때문이고, (3) pci_dev 내부의 saved state 와 분리하는 이유는 다른 리셋 경로가
 * 그것을 덮어쓰기 때문이다. 즉 **VFIO 만의 전용 스냅숏 슬롯**이다.
 *
 * 동작 과정:
 *  1. VF 가 하나라도 활성인 PF 를 D0 위로(더 깊은 절전으로) 보내려 하면
 *     -EBUSY. PF 의 전원은 항상 VF 보다 높아야 한다.
 *  2. needs_pm_restore 인 장치에 한해, D3hot 이상으로 내려가는 전이 직전에
 *     저장 표시를, D3hot 이상에서 D0 로 올라오는 전이에 복원 표시를 세운다.
 *  3. 실제 전이는 pci_set_power_state(drivers/pci/pci.c:2971)가 한다.
 *  4. 성공했고 저장이 필요했다면 — 다만 quirk 때문에 D3 진입이 무시됐을 수도
 *     있으므로 **실제로 D3hot 이상에 있는지 다시 확인한 뒤** —
 *     옛 pm_save 를 kfree 하고 새로 pci_store_saved_state 한다.
 *     옛것을 먼저 지우는 이유는 원본 주석대로 드라이버 개입 없이 D0 로
 *     돌아가는 경로가 있어 pm_save 가 남아 있을 수 있기 때문이다(메모리 누수 방지).
 *  5. 복원이 필요했다면 pci_load_and_free_saved_state 로 pm_save 를 pci_dev 에
 *     되돌린 뒤 pci_restore_state 로 하드웨어에 쓴다. 이름대로 이 함수가
 *     pm_save 를 해제하고 NULL 로 만든다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자에 따라 memory_lock 을 write 로 쥔 채
 * 불리기도 하고(runtime_suspend, sriov_configure) 아니기도 하다
 * (register_device, disable). 잠들 수 있다.
 *
 * 호출자: vfio_pci_core_enable 경로가 아니라 vfio_pci_core_disable,
 * vfio_pci_ioctl_reset, vfio_pci_core_runtime_suspend,
 * vfio_pci_core_register_device, vfio_pci_core_sriov_configure,
 * vfio_pci_dev_set_hot_reset 여섯 곳. 그리고
 * drivers/vfio/pci/vfio_pci_config.c 의 PM capability 에뮬레이션도 부른다.
 * 호출 대상: pci_num_vf(drivers/pci/iov.c:2524), pci_save_state(pci.c:3324),
 * pci_set_power_state(pci.c:2971), pci_store_saved_state(pci.c:3606),
 * pci_load_and_free_saved_state(pci.c:3728), pci_restore_state(pci.c:3530).
 *
 * 에러 경로: 전이 자체가 실패하면 저장도 복원도 하지 않고 그대로 반환한다.
 * pci_store_saved_state 가 NULL 을 돌려줘도 검사하지 않는데, 그 경우 뒤이은
 * pci_load_and_free_saved_state 가 NULL 을 받아 실패를 반환하고 복원을
 * 건너뛰므로 크래시로 이어지지는 않는다.
 *
 * 호출 체인:
 *   vfio_pci_core_disable / _ioctl_reset / _runtime_suspend / _register_device /
 *   _sriov_configure / vfio_pci_dev_set_hot_reset / vfio_pci_config.c 의 PM 쓰기
 *     → [vfio_pci_set_power_state] → pci_set_power_state
 */
/*
 * pci_set_power_state() wrapper handling devices which perform a soft reset on
 * D3->D0 transition.  Save state prior to D0/1/2->D3, stash it on the vdev,
 * restore when returned to D0.  Saved separately from pci_saved_state for use
 * by PM capability emulation and separately from pci_dev internal saved state
 * to avoid it being overwritten and consumed around other resets.
 */
int vfio_pci_set_power_state(struct vfio_pci_core_device *vdev, pci_power_t state)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 아래에서 정할 두 표시. 기본은 둘 다 거짓이라 needs_pm_restore 가 아닌
	 * 장치는 저장도 복원도 하지 않는다. */
	bool needs_restore = false, needs_save = false;
	/* [한국어] pci_set_power_state 의 반환값. */
	int ret;

	/* Prevent changing power state for PFs with VFs enabled */
	/* [한국어] VF 가 살아 있는 PF 를 더 깊은 절전으로 보내려는 요청은 막는다.
	 * PF 의 전원은 항상 VF 보다 높아야 하기 때문이다. */
	if (pci_num_vf(pdev) && state > PCI_D0)
		/* [한국어] -EBUSY. 사용자에게 "지금은 안 된다" 를 알린다. */
		return -EBUSY;

	/* [한국어] 소프트 리셋을 하는 장치에 한해 아래 판정을 한다. */
	if (vdev->needs_pm_restore) {
		/* [한국어] D3hot 미만에서 D3hot 이상으로 내려가는 전이. 이때 상태를 잃을 수 있다. */
		if (pdev->current_state < PCI_D3hot && state >= PCI_D3hot) {
			/* [한국어] PCI 코어의 내부 saved state 를 갱신해 둔다. 바로 아래에서
			 * pci_store_saved_state 로 그것을 복제해 vdev 로 떠 갈 것이다. */
			pci_save_state(pdev);
			/* [한국어] 전이 성공 후 스냅숏을 뜨라는 표시. */
			needs_save = true;
		}

		/* [한국어] D3hot 이상에서 D0 로 올라오는 전이. 이때 잃어버린 상태를 되돌려야 한다. */
		if (pdev->current_state >= PCI_D3hot && state <= PCI_D0)
			/* [한국어] 전이 성공 후 복원하라는 표시. */
			needs_restore = true;
	}

	/* [한국어] 실제 전원 전이. 구현은 drivers/pci/pci.c:2971 에 있고 주석 완료 상태다. */
	ret = pci_set_power_state(pdev, state);

	/* [한국어] 전이에 실패했으면 저장도 복원도 의미가 없다. */
	if (!ret) {
		/* D3 might be unsupported via quirk, skip unless in D3 */
		/* [한국어] 저장하라는 표시가 있고, **실제로도** D3hot 이상에 도달했을 때만 뜬다.
		 * quirk 때문에 D3 진입이 무시되는 장치가 있어 결과를 다시 확인한다. */
		if (needs_save && pdev->current_state >= PCI_D3hot) {
			/*
			 * The current PCI state will be saved locally in
			 * 'pm_save' during the D3hot transition. When the
			 * device state is changed to D0 again with the current
			 * function, then pci_store_saved_state() will restore
			 * the state and will free the memory pointed by
			 * 'pm_save'. There are few cases where the PCI power
			 * state can be changed to D0 without the involvement
			 * of the driver. For these cases, free the earlier
			 * allocated memory first before overwriting 'pm_save'
			 * to prevent the memory leak.
			 */
			/* [한국어] 이전 스냅숏이 남아 있으면 먼저 해제한다. 원본 주석대로 드라이버 개입
			 * 없이 D0 로 돌아가는 경로가 있어 pm_save 가 살아 있을 수 있고,
			 * 그대로 덮어쓰면 그 메모리가 샌다. */
			kfree(vdev->pm_save);
			/* [한국어] PCI 코어의 saved state 를 복제해 VFIO 전용 슬롯에 보관한다.
			 * pci_dev 내부의 것과 분리하는 이유는 다른 리셋 경로가 그것을 덮어쓰기 때문이다. */
			vdev->pm_save = pci_store_saved_state(pdev);
		/* [한국어] 저장이 아니라 복원해야 하는 전이였다면. */
		} else if (needs_restore) {
			/* [한국어] 보관해 둔 스냅숏을 PCI 코어의 saved state 로 되돌린다. 이름대로 이
			 * 함수가 pm_save 를 해제하고 NULL 로 만든다(drivers/pci/pci.c:3728). */
			pci_load_and_free_saved_state(pdev, &vdev->pm_save);
			/* [한국어] 그 상태를 실제 하드웨어 config 에 쓴다(drivers/pci/pci.c:3530). */
			pci_restore_state(pdev);
		}
	}

	/* [한국어] 전원 전이의 결과를 그대로 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_runtime_pm_entry - 사용자의 요청으로 장치를 저전력 상태로 들여보낸다
 *
 * @vdev:   대상 디바이스.
 * @efdctx: 깨어날 때 울릴 eventfd. NULL 이면 알림 없이 그냥 잔다.
 *          참조는 호출자가 이미 얻어 두었고, 성공하면 소유권이 vdev 로 넘어간다.
 * @return: 0 성공. -EINVAL 이면 이미 저전력 진입 상태라 중복 요청이다.
 *
 * 왜 필요한가: VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY 기능의 실체다. 사용자
 * 공간(주로 QEMU)이 게스트의 절전 요청을 호스트로 전달하는 통로이며, 호스트
 * 런타임 PM 이 장치를 실제로 D3 로 내리게 만드는 방법은 **usage count 를 하나
 * 떨어뜨리는 것**이다.
 *
 * 동작 과정:
 *  1. vfio_pci_zap_and_down_write_memory_lock — 사용자 BAR 매핑을 전부 걷어내고
 *     memory_lock 을 write 로 잡는다. 잠든 장치의 MMIO 를 사용자가 만지면
 *     호스트가 죽을 수 있으므로 **매핑 회수가 먼저**다.
 *  2. vfio_pci_dma_buf_move(vdev, true) — 이 BAR 를 P2P 대상으로 빌려 준
 *     dma-buf 도 함께 무효화한다. 다른 장치가 잠든 BAR 로 DMA 하면 같은 문제다.
 *  3. 이미 진입 상태면 락을 풀고 -EINVAL.
 *  4. 상태 플래그와 eventfd 를 세우고 pm_runtime_put_noidle 로 usage count 만
 *     하나 내린다. _noidle 인 이유는 여기서 곧장 잠재우지 않고, ioctl 이
 *     반환하면서 상위 래퍼가 pm_runtime_put 을 한 번 더 해 그때 잠들게
 *     하려는 것이다(vfio_pci_core_pm_entry 의 영어 주석).
 *
 * 실행 컨텍스트: 프로세스 문맥. **함수가 끝날 때 memory_lock 을 반드시 놓고
 * 나간다** — 성공 경로든 -EINVAL 경로든 up_write 가 있다.
 *
 * 호출자: vfio_pci_core_pm_entry(알림 없음)와
 * vfio_pci_core_pm_entry_with_wakeup(알림 있음).
 * 호출 대상: vfio_pci_zap_and_down_write_memory_lock, vfio_pci_dma_buf_move
 * (drivers/vfio/pci/vfio_pci_dmabuf.c:334), pm_runtime_put_noidle.
 *
 * 에러 경로: 중복 진입만이 실패다. 그 경우 eventfd 참조는 건드리지 않으므로
 * 호출자(vfio_pci_core_pm_entry_with_wakeup)가 eventfd_ctx_put 으로 되돌린다.
 *
 * 호출 체인:
 *   사용자 VFIO_DEVICE_FEATURE ioctl → vfio_main.c 의 feature 디스패처
 *     → vfio_pci_core_ioctl_feature → vfio_pci_core_pm_entry(_with_wakeup)
 *     → [vfio_pci_runtime_pm_entry] → pm_runtime_put_noidle
 */
static int vfio_pci_runtime_pm_entry(struct vfio_pci_core_device *vdev,
				     struct eventfd_ctx *efdctx)
{
	/*
	 * The vdev power related flags are protected with 'memory_lock'
	 * semaphore.
	 */
	/* [한국어] 사용자 BAR 매핑을 전부 걷어내고 memory_lock 을 write 로 잡는다.
	 * 잠들 장치의 MMIO 를 사용자가 만지지 못하게 하는 것이 먼저다. */
	vfio_pci_zap_and_down_write_memory_lock(vdev);
	/* [한국어] 이 BAR 를 P2P DMA 대상으로 빌려 준 dma-buf 도 무효화한다(revoked=true).
	 * 다른 장치가 잠든 BAR 로 DMA 하면 같은 문제가 생긴다. */
	vfio_pci_dma_buf_move(vdev, true);

	/* [한국어] 이미 저전력 진입 상태인데 또 요청이 왔다. 중복이다. */
	if (vdev->pm_runtime_engaged) {
		/* [한국어] 락을 반드시 풀고 나간다. 이 함수는 성공 경로든 실패 경로든 락을 놓는다. */
		up_write(&vdev->memory_lock);
		/* [한국어] -EINVAL 로 중복 요청임을 알린다. */
		return -EINVAL;
	}

	/* [한국어] 저전력 진입 상태를 표시한다. 이 플래그가 vfio_pci_vmf_insert_pfn 의
	 * 게이트에도 쓰여, 진입 중에는 BAR 폴트가 SIGBUS 가 된다. */
	vdev->pm_runtime_engaged = true;
	/* [한국어] 깨어날 때 울릴 eventfd 를 보관한다. NULL 이면 알림 없이 잔다.
	 * 참조 소유권이 여기서 vdev 로 넘어온다. */
	vdev->pm_wake_eventfd_ctx = efdctx;
	/* [한국어] 런타임 PM usage count 를 하나 내린다. _noidle 이라 곧장 잠재우지는 않고,
	 * ioctl 이 반환하며 상위 래퍼가 한 번 더 내릴 때 실제로 잠든다. */
	pm_runtime_put_noidle(&vdev->pdev->dev);
	/* [한국어] 락을 놓는다. 이후로는 사용자의 BAR 접근이 다시 폴트를 맞고,
	 * pm_runtime_engaged 때문에 SIGBUS 를 받는다. */
	up_write(&vdev->memory_lock);

	/* [한국어] 진입 성공. */
	return 0;
}

/* [한국어]
 * vfio_pci_core_pm_entry - VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY 의 얇은 껍데기
 *
 * @vdev:  대상 디바이스.
 * @flags: 사용자가 준 VFIO_DEVICE_FEATURE 플래그. SET/GET/PROBE 구분이 들어 있다.
 * @arg:   기능별 인자 영역. 이 기능은 인자가 없으므로 쓰지 않는다.
 * @argsz: 사용자가 신고한 인자 크기.
 * @return: 0 성공. vfio_check_feature 가 1 이 아닌 값을 돌려주면 그대로 전달한다
 *          (PROBE 요청이면 0, 잘못된 조합이면 음수 errno).
 *
 * 왜 필요한가: VFIO_DEVICE_FEATURE 는 하나의 ioctl 안에 여러 기능을 담는
 * 확장 통로다. 각 기능은 "내가 지원하는 방향(SET/GET)과 인자 크기" 를 코어의
 * vfio_check_feature 에 신고해야 하며, 그 신고를 대신해 주는 껍데기가 필요하다.
 *
 * 동작 과정:
 *  1. vfio_check_feature(include/linux/vfio.h:271 의 인라인)로 SET 방향만 지원하고
 *     인자 크기는 0 임을 검증한다. 반환값 1 만이 "실제로 수행하라" 는 뜻이다.
 *  2. 인자가 없으므로 eventfd 없이 vfio_pci_runtime_pm_entry 를 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 상위 ioctl 래퍼가 이미 런타임 PM 으로 장치를
 * D0 에 붙잡아 둔 상태다 — 그래서 여기서 usage count 를 하나 내려도 즉시
 * 잠들지 않고, ioctl 이 끝나며 래퍼가 한 번 더 내릴 때 잠든다.
 *
 * 호출자: vfio_pci_core_ioctl_feature.
 * 호출 대상: vfio_check_feature, vfio_pci_runtime_pm_entry.
 *
 * 에러 경로: 검증 실패 시 그대로 전달. 부작용이 없다.
 *
 * 호출 체인:
 *   vfio_pci_core_ioctl_feature → [vfio_pci_core_pm_entry]
 *     → vfio_check_feature / vfio_pci_runtime_pm_entry
 */
static int vfio_pci_core_pm_entry(struct vfio_pci_core_device *vdev, u32 flags,
				  void __user *arg, size_t argsz)
{
	/* [한국어] vfio_check_feature 의 결과를 받을 변수. */
	int ret;

	/* [한국어] SET 방향만 지원하고 인자 크기는 0 임을 신고해 검증받는다.
	 * 마지막 인자 0 이 "이 기능은 인자가 없다" 는 뜻이다. */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET, 0);
	/* [한국어] 1 이 아니면 실제 수행이 아니다 — PROBE 요청이면 0, 잘못된 조합이면 음수다. */
	if (ret != 1)
		/* [한국어] 그 값을 그대로 사용자에게 돌려준다. */
		return ret;

	/*
	 * Inside vfio_pci_runtime_pm_entry(), only the runtime PM usage count
	 * will be decremented. The pm_runtime_put() will be invoked again
	 * while returning from the ioctl and then the device can go into
	 * runtime suspended state.
	 */
	/* [한국어] 알림 eventfd 없이 진입한다. */
	return vfio_pci_runtime_pm_entry(vdev, NULL);
}

/* [한국어]
 * vfio_pci_core_pm_entry_with_wakeup - 저전력 진입 + 깨어남 알림 eventfd 등록
 *
 * @vdev:  대상 디바이스.
 * @flags: VFIO_DEVICE_FEATURE 플래그.
 * @arg:   사용자 공간의 struct vfio_device_low_power_entry_with_wakeup 포인터.
 *         wakeup_eventfd 필드 하나를 담고 있다.
 * @argsz: 사용자가 신고한 인자 크기. 구조체 크기 이상이어야 한다.
 * @return: 0 성공. -EFAULT(사용자 메모리 오류), -EINVAL(음수 fd 또는 중복 진입),
 *          eventfd_ctx_fdget 의 오류(잘못된 fd), 그 밖은 검증 함수의 반환값.
 *
 * 왜 필요한가: 게스트가 잠든 장치를 하드웨어가 깨우는 일(PME 등)이 있을 수
 * 있다. 그때 사용자 공간에 알려 줄 통로가 필요하고, 그것이 eventfd 다.
 * 알림이 필요 없는 사용자를 위해 알림 없는 판(vfio_pci_core_pm_entry)이
 * 따로 있다.
 *
 * 동작 과정:
 *  1. vfio_check_feature 로 SET 방향과 구조체 크기를 검증한다.
 *  2. copy_from_user 로 구조체를 커널로 복사한다. 사용자 포인터를 직접
 *     역참조하지 않는 것이 규칙이다.
 *  3. 음수 fd 는 -EINVAL. eventfd_ctx_fdget 으로 fd 를 컨텍스트로 바꾸며
 *     참조를 하나 얻는다.
 *  4. vfio_pci_runtime_pm_entry 에 넘긴다. 성공하면 참조 소유권이 vdev 로
 *     넘어가고, 실패하면 여기서 eventfd_ctx_put 으로 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 문맥. copy_from_user 때문에 잠들 수 있다.
 *
 * 호출자: vfio_pci_core_ioctl_feature.
 * 호출 대상: vfio_check_feature, copy_from_user, eventfd_ctx_fdget,
 * vfio_pci_runtime_pm_entry, eventfd_ctx_put.
 *
 * 에러 경로: 세 단계 모두 얻은 자원이 없거나(1~3) 얻은 참조를 정확히
 * 되돌린다(4). 참조 누수가 없다.
 *
 * 호출 체인:
 *   vfio_pci_core_ioctl_feature → [vfio_pci_core_pm_entry_with_wakeup]
 *     → eventfd_ctx_fdget / vfio_pci_runtime_pm_entry
 */
static int vfio_pci_core_pm_entry_with_wakeup(
	struct vfio_pci_core_device *vdev, u32 flags,
	struct vfio_device_low_power_entry_with_wakeup __user *arg,
	size_t argsz)
{
	/* [한국어] 사용자 구조체를 담을 커널 쪽 사본. 사용자 포인터를 직접 역참조하지 않는다. */
	struct vfio_device_low_power_entry_with_wakeup entry;
	/* [한국어] fd 를 변환해 얻을 eventfd 컨텍스트. */
	struct eventfd_ctx *efdctx;
	/* [한국어] 반환값 임시 변수. */
	int ret;

	/* [한국어] SET 방향과 구조체 크기를 신고해 검증받는다. */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET,
				 /* [한국어] 이 기능은 wakeup_eventfd 필드 하나를 담은 구조체를 인자로 받는다. */
				 sizeof(entry));
	/* [한국어] 1 이 아니면 실제 수행이 아니다. */
	if (ret != 1)
		/* [한국어] 그대로 전달한다. */
		return ret;

	/* [한국어] 사용자 메모리에서 구조체를 복사한다. 실패는 잘못된 포인터를 뜻한다. */
	if (copy_from_user(&entry, arg, sizeof(entry)))
		/* [한국어] -EFAULT. */
		return -EFAULT;

	/* [한국어] 음수 fd 는 의미가 없다. -1 도 여기서는 허용하지 않는다 — 이 기능은
	 * 알림을 요구하는 진입이므로 유효한 eventfd 가 반드시 있어야 한다. */
	if (entry.wakeup_eventfd < 0)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] fd 를 eventfd 컨텍스트로 바꾸며 참조를 하나 얻는다.
	 * eventfd 가 아닌 fd 를 주면 여기서 오류 포인터가 나온다. */
	efdctx = eventfd_ctx_fdget(entry.wakeup_eventfd);
	/* [한국어] 오류 포인터 여부를 확인한다. */
	if (IS_ERR(efdctx))
		/* [한국어] 포인터에 인코딩된 errno 를 꺼내 돌려준다. */
		return PTR_ERR(efdctx);

	/* [한국어] 얻은 컨텍스트를 들고 진입한다. 성공하면 소유권이 vdev 로 넘어간다. */
	ret = vfio_pci_runtime_pm_entry(vdev, efdctx);
	/* [한국어] 실패했다면 소유권이 넘어가지 않았다. */
	if (ret)
		/* [한국어] 우리가 얻은 참조를 되돌려 놓는다. 이 한 줄이 참조 누수를 막는다. */
		eventfd_ctx_put(efdctx);

	/* [한국어] 진입 결과를 그대로 돌려준다. */
	return ret;
}

/* [한국어]
 * __vfio_pci_runtime_pm_exit - 저전력 진입 상태를 되돌린다(락은 호출자가 쥔다)
 *
 * @vdev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: 저전력 이탈은 두 문맥에서 일어난다 — 사용자가 명시적으로
 * LOW_POWER_EXIT 를 요청할 때와, 하드웨어가 장치를 깨워 런타임 resume 콜백이
 * 돌 때. 두 문맥의 락 잡는 방식이 달라서, 락을 밖에 두고 알맹이만 뽑았다.
 * 이름 앞의 밑줄 둘이 "락은 네가 책임져라" 는 관례적 표시다.
 *
 * 동작 과정:
 *  1. 진입 상태가 아니면 아무것도 하지 않는다 — **중복 호출에 안전**하다.
 *     vfio_pci_core_pm_exit 의 영어 주석이 말하는 "redundant call 보호" 가 이것이다.
 *  2. 플래그를 내리고 pm_runtime_get_noresume 으로 진입 때 내렸던 usage count 를
 *     되돌린다. _noresume 인 이유는 이 함수가 이미 D0 인 상태에서 불리거나
 *     (resume 콜백 안) 호출자가 따로 resume 을 시키기 때문이다.
 *  3. 등록된 wake eventfd 가 있으면 참조를 놓고 슬롯을 비운다.
 *
 * 실행 컨텍스트: 프로세스 문맥이며 **호출자가 memory_lock 을 write 로 쥐고
 * 있어야 한다**. 이 파일에서 pm 관련 플래그를 지키는 락이 memory_lock 이다.
 *
 * 호출자: vfio_pci_runtime_pm_exit(래퍼)와 vfio_pci_core_runtime_resume
 * (하드웨어가 깨운 경우).
 * 호출 대상: pm_runtime_get_noresume, eventfd_ctx_put.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_runtime_pm_exit 또는 vfio_pci_core_runtime_resume
 *     → [__vfio_pci_runtime_pm_exit] → pm_runtime_get_noresume
 */
static void __vfio_pci_runtime_pm_exit(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 진입 상태가 아니면 아무것도 하지 않는다. 이 검사가 중복 호출을 흡수한다 —
	 * 사용자의 LOW_POWER_EXIT 와 하드웨어 resume 이 겹쳐도 안전하다. */
	if (vdev->pm_runtime_engaged) {
		/* [한국어] 먼저 플래그를 내려 다음 호출이 중복으로 걸러지게 한다. */
		vdev->pm_runtime_engaged = false;
		/* [한국어] 진입 때 내렸던 usage count 를 되돌린다. _noresume 인 이유는 이미 D0 이거나
		 * 호출자가 따로 resume 을 시키기 때문에 여기서 깨울 필요가 없어서다. */
		pm_runtime_get_noresume(&vdev->pdev->dev);

		/* [한국어] 알림 eventfd 를 등록해 둔 진입이었다면. */
		if (vdev->pm_wake_eventfd_ctx) {
			/* [한국어] 진입 때 넘겨받은 참조를 놓는다. */
			eventfd_ctx_put(vdev->pm_wake_eventfd_ctx);
			/* [한국어] 슬롯을 비워 다음 진입이 깨끗하게 시작하도록 한다. */
			vdev->pm_wake_eventfd_ctx = NULL;
		}
	}
}

/* [한국어]
 * vfio_pci_runtime_pm_exit - memory_lock 을 직접 잡고 저전력 상태를 되돌린다
 *
 * @vdev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: __vfio_pci_runtime_pm_exit 은 락을 밖에 맡긴다. 락을 스스로
 * 잡아야 하는 두 호출자(사용자의 LOW_POWER_EXIT, 그리고 디바이스 닫기)를 위해
 * 락 + 알맹이 + dma-buf 복구를 한 덩어리로 묶은 판이 이것이다.
 *
 * 동작 과정:
 *  1. memory_lock 을 write 로 잡는다. 원본 주석대로 pm 관련 플래그를 지키는
 *     락이 memory_lock 이다.
 *  2. 알맹이를 실행한다(플래그 내리기, usage count 되돌리기, eventfd 놓기).
 *  3. **메모리 디코드가 실제로 켜져 있을 때만** dma-buf 를 되살린다.
 *     진입 때 무조건 revoke 했던 것과 대칭이 아닌 이유는, 사용자가 그 사이
 *     PCI_COMMAND 의 Memory Space Enable 을 꺼 두었을 수도 있기 때문이다.
 *     그 상태에서 되살리면 디코드가 꺼진 BAR 로 P2P DMA 가 날아간다.
 *  4. 락을 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **자기가 락을 잡고 자기가 놓는다.** 그래서
 * 락을 이미 쥔 곳(runtime_resume 콜백)에서는 이 함수 대신 밑줄 두 개짜리를
 * 직접 부른다.
 *
 * 호출자: vfio_pci_core_pm_exit(사용자 요청)과 vfio_pci_core_disable(닫기).
 * 호출 대상: __vfio_pci_runtime_pm_exit, __vfio_pci_memory_enabled
 * (drivers/vfio/pci/vfio_pci_config.c:404), vfio_pci_dma_buf_move.
 *
 * 에러 경로: 없다. 실패할 수 있는 일을 하지 않는다.
 *
 * 호출 체인:
 *   vfio_pci_core_pm_exit 또는 vfio_pci_core_disable
 *     → [vfio_pci_runtime_pm_exit] → __vfio_pci_runtime_pm_exit
 */
static void vfio_pci_runtime_pm_exit(struct vfio_pci_core_device *vdev)
{
	/*
	 * The vdev power related flags are protected with 'memory_lock'
	 * semaphore.
	 */
	/* [한국어] pm 관련 플래그를 지키는 락이 memory_lock 이다. write 로 잡아 사용자의
	 * BAR 접근과 폴트를 이 구간 동안 모두 막는다. */
	down_write(&vdev->memory_lock);
	/* [한국어] 플래그를 내리고 usage count 를 되돌리고 eventfd 를 놓는다. */
	__vfio_pci_runtime_pm_exit(vdev);
	/* [한국어] PCI_COMMAND 의 Memory Space Enable 이 실제로 켜져 있는지 본다.
	 * 사용자가 그 사이 꺼 두었을 수도 있기 때문이다(vfio_pci_config.c:404). */
	if (__vfio_pci_memory_enabled(vdev))
		/* [한국어] 디코드가 켜져 있을 때만 dma-buf 를 되살린다(revoked=false).
		 * 꺼진 BAR 로 P2P DMA 가 날아가면 안 되므로 진입 때와 비대칭이다. */
		vfio_pci_dma_buf_move(vdev, false);
	/* [한국어] 락을 놓는다. 이후 사용자의 BAR 접근이 다시 정상 동작한다. */
	up_write(&vdev->memory_lock);
}

/* [한국어]
 * vfio_pci_core_pm_exit - VFIO_DEVICE_FEATURE_LOW_POWER_EXIT 의 얇은 껍데기
 *
 * @vdev:  대상 디바이스.
 * @flags: VFIO_DEVICE_FEATURE 플래그.
 * @arg:   인자 없음.
 * @argsz: 사용자가 신고한 인자 크기(0 이어야 한다).
 * @return: 0 성공. 검증 실패면 vfio_check_feature 의 값을 그대로 전달.
 *
 * 왜 필요한가: 진입과 짝을 이루는 이탈 통로다. 원본 영어 주석이 중요한 사실을
 * 하나 말한다 — **이 함수가 불릴 때 장치는 이미 D0 에 있다**. ioctl 을 감싸는
 * 런타임 PM 래퍼(drivers/vfio/vfio_main.c 의 ioctl 디스패처)가 진입 전에
 * 장치를 깨우기 때문이다. 그리고 하드웨어가 깨운 경우라면
 * vfio_pci_core_runtime_resume 이 이미 eventfd 를 울리고 이탈까지 마쳤다.
 * 그러므로 여기서의 호출은 대개 **중복**이고, 그 중복은
 * pm_runtime_engaged 검사가 흡수한다.
 *
 * 동작 과정: 검증 후 vfio_pci_runtime_pm_exit 한 줄. 항상 0 을 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출자: vfio_pci_core_ioctl_feature.
 * 호출 대상: vfio_check_feature, vfio_pci_runtime_pm_exit.
 *
 * 에러 경로: 검증 실패만 전달한다.
 *
 * 호출 체인:
 *   vfio_pci_core_ioctl_feature → [vfio_pci_core_pm_exit]
 *     → vfio_pci_runtime_pm_exit
 */
static int vfio_pci_core_pm_exit(struct vfio_pci_core_device *vdev, u32 flags,
				 void __user *arg, size_t argsz)
{
	/* [한국어] 검증 결과를 받을 변수. */
	int ret;

	/* [한국어] SET 방향, 인자 없음으로 신고해 검증받는다. */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET, 0);
	/* [한국어] 1 이 아니면 실제 수행이 아니다. */
	if (ret != 1)
		/* [한국어] 그대로 전달한다. */
		return ret;

	/*
	 * The device is always in the active state here due to pm wrappers
	 * around ioctls. If the device had entered a low power state and
	 * pm_wake_eventfd_ctx is valid, vfio_pci_core_runtime_resume() has
	 * already signaled the eventfd and exited low power mode itself.
	 * pm_runtime_engaged protects the redundant call here.
	 */
	/* [한국어] 중복 호출은 pm_runtime_engaged 검사가 흡수하므로 조건 없이 부른다. */
	vfio_pci_runtime_pm_exit(vdev);
	/* [한국어] 이 기능은 실패할 수 없다. */
	return 0;
}

/* [한국어] 런타임 PM 자체가 빌드에서 빠지면 아래 두 콜백도 필요 없다.
 * SET_RUNTIME_PM_OPS 매크로가 그때는 아무 슬롯도 채우지 않으므로
 * 미사용 함수 경고를 피하려면 정의도 함께 빠져야 한다. */
#ifdef CONFIG_PM
/* [한국어]
 * vfio_pci_core_runtime_suspend - 런타임 PM 이 장치를 재우기 직전에 불리는 콜백
 *
 * @dev: struct device. dev_get_drvdata 로 vfio_pci_core_device 를 되찾는다
 *       — vendor 드라이버가 probe 에서 drvdata 를 그렇게 설정하도록
 *       vfio_pci_core_register_device 가 WARN 으로 강제한다.
 * @return: 항상 0. 실패하지 않는다.
 *
 * 왜 필요한가: PCI 드라이버 코어의 런타임 PM 이 장치를 저전력으로 내리기 전에,
 * VFIO 만 아는 두 가지를 정리해야 한다. (1) 사용자가 PCI_PM_CTRL 에뮬레이션을
 * 통해 이미 장치를 D3hot 으로 내려 두었을 수 있는데, 그 상태에서 코어가 또
 * 전이를 하면 NoSoftRst- 장치의 pm_save 복원 타이밍이 어긋난다. 그래서
 * **먼저 D0 로 올려 놓고** 코어에게 넘긴다. (2) INTx 는 레벨 트리거라 잠든
 * 장치가 선을 계속 눌러 두면 호스트가 인터럽트 폭풍을 맞는다.
 *
 * 동작 과정:
 *  1. memory_lock 을 write 로 잡고 vfio_pci_set_power_state(PCI_D0). 락을 잡는
 *     이유는 사용자의 config 쓰기가 같은 전원 상태를 건드리기 때문이다.
 *  2. 지금 INTx 를 쓰고 있다면 vfio_pci_intx_mask 로 마스크한다. 그 함수는
 *     **실제로 마스크가 바뀌었을 때만 true** 를 돌려주므로, 사용자가 이미
 *     마스크해 둔 경우에는 pm_intx_masked 가 거짓이 되어 resume 때 함부로
 *     풀지 않는다. 원본 영어 주석이 그 대칭을 설명한다.
 *
 * 실행 컨텍스트: 런타임 PM 코어의 작업 큐 또는 put 을 호출한 스레드.
 * 프로세스 문맥이며 잠들 수 있다.
 *
 * 호출자: PCI 드라이버 코어의 런타임 PM(아래 vfio_pci_core_pm_ops 에
 * SET_RUNTIME_PM_OPS 로 등록된다).
 * 호출 대상: dev_get_drvdata, vfio_pci_set_power_state, vfio_pci_intx_mask
 * (drivers/vfio/pci/vfio_pci_intrs.c:148).
 *
 * 에러 경로: 없다. 0 을 돌려주어 코어가 계속 진행하게 한다.
 *
 * 호출 체인:
 *   런타임 PM 코어 → [vfio_pci_core_runtime_suspend]
 *     → vfio_pci_set_power_state / vfio_pci_intx_mask
 */
static int vfio_pci_core_runtime_suspend(struct device *dev)
{
	/* [한국어] PCI 드라이버 코어는 struct device 만 넘겨준다. drvdata 로 vdev 를 되찾는데,
	 * 그 계약은 vfio_pci_core_register_device 의 첫 WARN 이 강제한다. */
	struct vfio_pci_core_device *vdev = dev_get_drvdata(dev);

	/* [한국어] 전원 상태를 만지므로 memory_lock 을 write 로 잡는다.
	 * 사용자의 PCI_PM_CTRL 쓰기(config 중재 경로)와 경합하기 때문이다. */
	down_write(&vdev->memory_lock);
	/*
	 * The user can move the device into D3hot state before invoking
	 * power management IOCTL. Move the device into D0 state here and then
	 * the pci-driver core runtime PM suspend function will move the device
	 * into the low power state. Also, for the devices which have
	 * NoSoftRst-, it will help in restoring the original state
	 * (saved locally in 'vdev->pm_save').
	 */
	/* [한국어] 코어에 넘기기 전에 먼저 D0 로 올린다. 사용자가 에뮬레이션을 통해
	 * 이미 D3hot 으로 내려 두었을 수 있고, 그 상태에서 코어가 또 전이하면
	 * NoSoftRst- 장치의 pm_save 복원 시점이 어긋난다. */
	vfio_pci_set_power_state(vdev, PCI_D0);
	/* [한국어] 락을 놓는다. 아래 INTx 조작은 자체 스핀락을 쓰므로 이 락이 필요 없다. */
	up_write(&vdev->memory_lock);

	/*
	 * If INTx is enabled, then mask INTx before going into the runtime
	 * suspended state and unmask the same in the runtime resume.
	 * If INTx has already been masked by the user, then
	 * vfio_pci_intx_mask() will return false and in that case, INTx
	 * should not be unmasked in the runtime resume.
	 */
	/* [한국어] 지금 사용 중인 인터럽트 종류가 INTx 인지 먼저 본다. MSI/MSI-X 는
	 * 메시지 기반이라 잠든 장치가 선을 눌러 두는 문제가 없다. */
	vdev->pm_intx_masked = ((vdev->irq_type == VFIO_PCI_INTX_IRQ_INDEX) &&
				/* [한국어] 실제로 마스크를 바꿨을 때만 true 를 돌려준다(vfio_pci_intrs.c:148).
				 * 사용자가 이미 마스크해 둔 경우에는 false 가 되어, resume 때
				 * 남의 마스크를 함부로 풀지 않는다. 두 조건의 AND 가 그 대칭을 만든다. */
				vfio_pci_intx_mask(vdev));

	/* [한국어] 런타임 PM 코어에게 계속 진행하라고 알린다. */
	return 0;
}

/* [한국어]
 * vfio_pci_core_runtime_resume - 장치가 깨어난 직후에 불리는 콜백
 *
 * @dev: struct device. drvdata 에서 vdev 를 되찾는다.
 * @return: 항상 0.
 *
 * 왜 필요한가: 장치를 깨운 주체가 사용자가 아닐 수 있다. 하드웨어 PME 나
 * 호스트 쪽 접근이 깨웠다면, 저전력 진입을 요청했던 사용자에게 **깨어났다는
 * 사실을 알려야** 한다. 그 알림 수단이 진입 때 등록해 둔 eventfd 다.
 *
 * 동작 과정:
 *  1. memory_lock 을 write 로 잡는다.
 *  2. wake eventfd 가 걸려 있으면 = 사용자가 알림을 요청한 저전력 진입
 *     상태였다는 뜻이다. eventfd 를 울리고 진입 상태를 되돌린다.
 *     (밑줄 두 개짜리를 쓰는 이유는 락을 이미 쥐고 있기 때문이다.)
 *  3. 락을 놓는다.
 *  4. suspend 때 우리가 INTx 를 마스크했다면 여기서 푼다. 사용자가 마스크한
 *     것은 건드리지 않는다 — pm_intx_masked 가 그 구분을 담고 있다.
 *
 * 실행 컨텍스트: 런타임 PM 코어의 resume 경로. 프로세스 문맥.
 * INTx 언마스크를 락 밖에서 하는 이유는 그 경로가 자체 스핀락(irqlock)을
 * 쓰기 때문이다.
 *
 * 호출자: PCI 드라이버 코어의 런타임 PM.
 * 호출 대상: dev_get_drvdata, eventfd_signal, __vfio_pci_runtime_pm_exit,
 * vfio_pci_intx_unmask(drivers/vfio/pci/vfio_pci_intrs.c:216).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   런타임 PM 코어 → [vfio_pci_core_runtime_resume]
 *     → eventfd_signal / __vfio_pci_runtime_pm_exit / vfio_pci_intx_unmask
 */
static int vfio_pci_core_runtime_resume(struct device *dev)
{
	/* [한국어] drvdata 에서 vdev 를 되찾는다. */
	struct vfio_pci_core_device *vdev = dev_get_drvdata(dev);

	/*
	 * Resume with a pm_wake_eventfd_ctx signals the eventfd and exit
	 * low power mode.
	 */
	/* [한국어] pm 관련 플래그를 만지므로 write 로 잡는다. */
	down_write(&vdev->memory_lock);
	/* [한국어] 알림 eventfd 가 걸려 있다 = 사용자가 알림을 요청한 저전력 진입 상태였다.
	 * 하드웨어나 호스트가 깨운 경우이므로 사용자에게 알려야 한다. */
	if (vdev->pm_wake_eventfd_ctx) {
		/* [한국어] eventfd 를 울려 사용자를 깨운다. 이 함수는 잠들지 않으므로 락 안에서 안전하다. */
		eventfd_signal(vdev->pm_wake_eventfd_ctx);
		/* [한국어] 락을 이미 쥐고 있으므로 밑줄 두 개짜리 알맹이를 직접 부른다. */
		__vfio_pci_runtime_pm_exit(vdev);
	}
	/* [한국어] 락을 놓는다. */
	up_write(&vdev->memory_lock);

	/* [한국어] suspend 때 **우리가** 마스크한 경우에만 참이다. 사용자가 마스크한 것은
	 * 건드리지 않는다. */
	if (vdev->pm_intx_masked)
		/* [한국어] INTx 를 다시 열어 인터럽트가 흐르게 한다(vfio_pci_intrs.c:216). */
		vfio_pci_intx_unmask(vdev);

	/* [한국어] 런타임 PM 코어에게 성공을 알린다. */
	return 0;
}
/* [한국어] CONFIG_PM 블록의 끝. */
#endif /* CONFIG_PM */

/*
 * The pci-driver core runtime PM routines always save the device state
 * before going into suspended state. If the device is going into low power
 * state with only with runtime PM ops, then no explicit handling is needed
 * for the devices which have NoSoftRst-.
 */
/* [한국어] 이 드라이버가 쓸 전원 관리 콜백 표. vfio_pci_core_register_device 가
 * dev->driver->pm 에 이것을 꽂는다 — 드라이버 구조체에 미리 박아 두지 않고
 * 등록 시점에 심는 것이 특이한 점이다.
 * 설정자: 이 파일의 정적 초기화.
 * 읽는 자: PCI 드라이버 코어의 런타임 PM.
 * 값 범위: runtime_suspend 와 runtime_resume 만 채워지고 나머지는 NULL.
 * 동기화: const 라 읽기 전용이다. */
static const struct dev_pm_ops vfio_pci_core_pm_ops = {
	/* [한국어] SET_RUNTIME_PM_OPS 는 CONFIG_PM 이 켜졌을 때만 세 슬롯을 채운다.
	 * 첫 인자가 suspend 콜백.
	 * 설정자와 읽는 자: 위와 같다.
	 * 값 범위: 이 파일의 함수 포인터.
	 * 동기화: 없음. */
	SET_RUNTIME_PM_OPS(vfio_pci_core_runtime_suspend,
			   /* [한국어] 두 번째 인자가 resume 콜백.
			    * 설정자와 읽는 자: 위와 같다.
			    * 값 범위: 이 파일의 함수 포인터.
			    * 동기화: 없음. */
			   vfio_pci_core_runtime_resume,
			   /* [한국어] 세 번째 인자는 runtime_idle 콜백인데 NULL 이다. 유휴 판정은 코어의
			    * 기본 동작(usage count 가 0 이면 잠재움)에 맡긴다는 뜻이다.
			    * 설정자와 읽는 자: 위와 같다.
			    * 값 범위: NULL 고정.
			    * 동기화: 없음. */
			   NULL)
};

/* [한국어]
 * vfio_pci_core_enable - 장치를 사용자에게 넘길 수 있는 상태로 만든다(열기의 몸통)
 *
 * @vdev: 대상 디바이스.
 * @return: 0 성공. 음수 errno 면 열기 자체가 실패한다.
 *
 * 왜 필요한가: **하드웨어 소유권이 커널에서 사용자로 넘어가는 지점**이다.
 * 여기서 하는 일곱 가지가 뒤이은 모든 접근의 전제가 된다 — 전원을 올리고,
 * 장치를 켜고, 이전 사용자의 잔재를 리셋으로 지우고, "깨끗한 상태" 스냅숏을
 * 뜨고, INTx 정책을 정하고, config 그림자를 만들고, MSI-X 테이블 위치를
 * 알아 둔다. 마지막 둘이 없으면 config 중재도 BAR 제외 구간도 성립하지 않는다.
 *
 * 동작 과정:
 *  1. 절전이 켜져 있으면 pm_runtime_resume_and_get 으로 장치를 D0 에 붙잡는다.
 *     이 참조는 vfio_pci_core_disable 이 놓는다.
 *  2. pci_clear_master — 원본 주석대로 **저장할 초기 상태에 Bus Master 가
 *     들어가지 않게** 먼저 끈다. 사용자가 명시적으로 켜기 전에는 장치가
 *     DMA 를 못 하게 하는 것이 안전 기본값이다.
 *  3. pci_enable_device 로 장치를 켠다.
 *  4. pci_try_reset_function 으로 이전 사용자의 상태를 지운다.
 *     **-EAGAIN(디바이스 락 획득 실패)이면 열기 전체를 포기**한다. 그 밖의
 *     실패는 reset_works 를 거짓으로 두고 계속 간다 — 리셋을 못 하는 장치도
 *     쓸 수는 있게 해 주되, 그 사실을 사용자에게 알린다
 *     (GET_INFO 의 VFIO_DEVICE_FLAGS_RESET 플래그).
 *  5. 리셋 **직후** 상태를 pci_save_state + pci_store_saved_state 로 뜬다.
 *     이 스냅숏이 닫을 때 복원의 기준이다. 실패해도 치명적이지 않아 로그만 남긴다.
 *  6. INTx 정책: nointxmask 파라미터가 꺼져 있으면, 알려진 고장 장치인지 보고
 *     (vfio_pci_nointx) 맞으면 INTx 를 감춘 채 DisINTx 를 세워 둔다.
 *     아니면 pci_intx_mask_supported 로 PCI 2.3 마스킹 가능 여부를 조사한다.
 *  7. 마스킹이 가능한데 지금 DisINTx 가 서 있으면 지운다. 마스킹은 소프트웨어가
 *     필요할 때 다시 세우면 되므로, 시작 상태는 열어 둔다.
 *  8. s390 zPCI 정보 준비(비 s390 에서는 stub 이라 0).
 *  9. **vfio_config_init** — 여기서 vconfig 그림자와 pci_config_map 이 만들어진다.
 *     이 호출 이후부터 사용자의 config 접근이 의미를 갖는다.
 * 10. MSI-X capability 가 있으면 Table BIR/Offset/Size 를 뽑아
 *     msix_bar, msix_offset, msix_size 에 넣고, pci_msix_can_alloc_dyn
 *     (drivers/pci/msi/api.c:287)으로 동적 벡터 추가 가능 여부를 저장한다.
 *     이 셋이 vfio_pci_bar_rw 의 제외 구간과 GET_IRQ_INFO 의 NORESIZE 플래그의
 *     근거다. 없으면 msix_bar 를 0xFF 로 두어 **어떤 BAR 인덱스와도 겹치지
 *     않게** 만든다.
 * 11. VGA 장치이고 VGA 지원이 켜져 있으면 has_vga 를 세운다.
 *
 * 실행 컨텍스트: 프로세스 문맥. vendor 의 open_device 콜백 안이며,
 * vfio_main.c 의 dev_set->lock 을 쥔 상태로 불린다(첫 open 에서만 불린다).
 *
 * 호출자: vendor 드라이버의 open_device — drivers/vfio/pci/vfio_pci.c:110,
 * drivers/vfio/pci/xe/main.c:155, drivers/vfio/pci/ism/main.c:60 등.
 * 호출 대상: pm_runtime_resume_and_get, pci_clear_master(pci.c:7923),
 * pci_enable_device(pci.c:4126), pci_try_reset_function(pci.c:10347),
 * pci_save_state(pci.c:3324), pci_store_saved_state(pci.c:3606),
 * pci_intx(pci.c:8201), pci_intx_mask_supported, pci_read_config_word,
 * pci_write_config_word, vfio_pci_zdev_open_device, vfio_config_init
 * (vfio_pci_config.c:1749), pci_msix_can_alloc_dyn(drivers/pci/msi/api.c:287).
 *
 * 에러 경로: 네 개의 되감기 라벨이 계단식으로 쌓여 있다.
 * out_free_zdev(zdev 정리) → out_free_state(스냅숏 해제) →
 * out_disable_device(장치 끄기) → out_power(런타임 PM 참조 놓기).
 * 얻은 순서의 정확한 역순이라 어느 지점에서 실패해도 새는 자원이 없다.
 *
 * 호출 체인:
 *   사용자 open(2) → vfio_main.c:vfio_df_open → vendor open_device
 *     → [vfio_pci_core_enable] → pci_enable_device / pci_try_reset_function /
 *       vfio_config_init
 */
int vfio_pci_core_enable(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 각 단계의 반환값. 되감기 라벨로 뛸 때 그대로 반환값이 된다. */
	int ret;
	/* [한국어] PCI_COMMAND 를 읽어 둘 임시 변수. */
	u16 cmd;
	/* [한국어] MSI-X capability 의 config 공간 오프셋. */
	u8 msix_pos;

	/* [한국어] 절전이 켜져 있으면 장치가 D3 에 잠들어 있을 수 있다. */
	if (!disable_idle_d3) {
		/* [한국어] 장치를 깨우고 usage count 를 하나 올려 열려 있는 동안 D0 에 붙잡는다.
		 * 이 참조는 vfio_pci_core_disable 이 놓는다. */
		ret = pm_runtime_resume_and_get(&pdev->dev);
		/* [한국어] 깨우기 실패. */
		if (ret < 0)
			/* [한국어] 아직 아무것도 잡지 않았으므로 그냥 반환한다. */
			return ret;
	}

	/* Don't allow our initial saved state to include busmaster */
	/* [한국어] Bus Master 를 끈다. 아래에서 뜨는 스냅숏에 그 비트가 들어가지 않게 해,
	 * 사용자가 명시적으로 켜기 전에는 장치가 DMA 를 못 하게 만든다.
	 * 구현은 drivers/pci/pci.c:7923. */
	pci_clear_master(pdev);

	/* [한국어] 장치를 켠다 — BAR 자원을 확보하고 I/O/메모리 디코드를 준비한다
	 * (drivers/pci/pci.c:4126). */
	ret = pci_enable_device(pdev);
	/* [한국어] 켜기 실패. */
	if (ret)
		/* [한국어] 런타임 PM 참조만 되돌리면 된다. */
		goto out_power;

	/* If reset fails because of the device lock, fail this path entirely */
	/* [한국어] 이전 사용자의 잔재를 지우기 위한 초기 리셋(drivers/pci/pci.c:10347). */
	ret = pci_try_reset_function(pdev);
	/* [한국어] -EAGAIN 은 디바이스 락을 얻지 못했다는 뜻이다. 다른 스레드가 이 장치를
	 * 만지는 중이므로 열기 전체를 포기한다 — 원본 영어 주석이 명시한 정책이다. */
	if (ret == -EAGAIN)
		/* [한국어] 장치를 끄고 전원 참조를 놓는다. */
		goto out_disable_device;

	/* [한국어] 그 밖의 실패는 치명적이지 않다. 리셋 가능 여부만 기록해 두고 계속 간다.
	 * 이 값이 GET_INFO 의 VFIO_DEVICE_FLAGS_RESET 플래그와 VFIO_DEVICE_RESET
	 * ioctl 의 허용 여부를 결정한다. */
	vdev->reset_works = !ret;
	/* [한국어] 리셋 직후의 깨끗한 상태를 PCI 코어의 saved state 로 뜬다(pci.c:3324). */
	pci_save_state(pdev);
	/* [한국어] 그것을 복제해 VFIO 전용 슬롯에 보관한다(pci.c:3606). 닫을 때 이 스냅숏을
	 * 되돌린다. pci_dev 내부의 것과 따로 두는 이유는 리셋 경로들이 그것을
	 * 덮어쓰기 때문이다. */
	vdev->pci_saved_state = pci_store_saved_state(pdev);
	/* [한국어] 복제 실패(메모리 부족). 치명적이지 않다 — 닫을 때 복원을 못 할 뿐이다. */
	if (!vdev->pci_saved_state)
		/* [한국어] 디버그 로그만 남기고 계속 진행한다. */
		pci_dbg(pdev, "%s: Couldn't store saved state\n", __func__);

	/* [한국어] nointxmask 파라미터가 꺼져 있는 것이 일반적이므로 likely 로 표시해
	 * 분기 예측을 돕는다. 켜져 있으면 INTx 마스킹 조사를 통째로 건너뛴다. */
	if (likely(!nointxmask)) {
		/* [한국어] PCI_STATUS 의 Interrupt Status 를 제대로 보고하지 않는 알려진 장치인가. */
		if (vfio_pci_nointx(pdev)) {
			/* [한국어] 감춘다는 사실을 관리자에게 알린다. */
			pci_info(pdev, "Masking broken INTx support\n");
			/* [한국어] 이 플래그를 보고 vfio_pci_config.c 가 vconfig 의 Interrupt Pin 을 0 으로
			 * 만들어 사용자에게 INTx 가 없는 것처럼 보이게 한다. */
			vdev->nointx = true;
			/* [한국어] 호스트 쪽에서 INTx 를 끈 채로 둔다 — 두 번째 인자 0 이 DisINTx 를 세운다
			 * (drivers/pci/pci.c:8201). */
			pci_intx(pdev, 0);
		/* [한국어] 정상 장치. */
		} else
			/* [한국어] PCI 2.3 식 INTx 마스킹(Interrupt Disable 로 끄고 Interrupt Status 로 확인)을
			 * 지원하는지 조사한다. 지원하면 인터럽트 선을 다른 장치와 공유할 수 있다. */
			vdev->pci_2_3 = pci_intx_mask_supported(pdev);
	}

	/* [한국어] 현재 명령 레지스터를 읽는다. */
	pci_read_config_word(pdev, PCI_COMMAND, &cmd);
	/* [한국어] 마스킹이 가능한 장치인데 지금 DisINTx 가 서 있으면. */
	if (vdev->pci_2_3 && (cmd & PCI_COMMAND_INTX_DISABLE)) {
		/* [한국어] 그 비트를 지운다. 마스킹은 필요할 때 소프트웨어가 다시 세우면 되므로
		 * 시작 상태는 열어 둔다. */
		cmd &= ~PCI_COMMAND_INTX_DISABLE;
		/* [한국어] 지운 값을 하드웨어에 쓴다. 이 접근도 커널 자신의 것이라 사용자 중재
		 * 경로를 거치지 않는다. */
		pci_write_config_word(pdev, PCI_COMMAND, cmd);
	}

	/* [한국어] s390 zPCI 정보 준비. 그 아키텍처가 아니면 vfio_pci_priv.h 의 stub 이 0 을
	 * 돌려준다. */
	ret = vfio_pci_zdev_open_device(vdev);
	/* [한국어] 준비 실패. */
	if (ret)
		/* [한국어] 스냅숏부터 되감는다. */
		goto out_free_state;

	/* [한국어] **config 공간 그림자(vconfig)와 바이트별 권한 지도(pci_config_map)를 만든다**
	 * (vfio_pci_config.c:1749). 이 호출 이후부터 사용자의 config 접근이
	 * 중재를 거쳐 의미를 갖는다. */
	ret = vfio_config_init(vdev);
	/* [한국어] 그림자 생성 실패. */
	if (ret)
		/* [한국어] zdev 부터 되감는다. */
		goto out_free_zdev;

	/* [한국어] MSI-X capability 의 오프셋. PCI 코어가 열거 시점에 찾아 둔 값이다. */
	msix_pos = pdev->msix_cap;
	/* [한국어] MSI-X 를 지원하는 장치인가. */
	if (msix_pos) {
		/* [한국어] MSI-X Message Control 레지스터 값. */
		u16 flags;
		/* [한국어] MSI-X Table Offset/BIR 레지스터 값. */
		u32 table;

		/* [한국어] Message Control 을 읽는다. 하위 11비트가 Table Size(0 기준)다. */
		pci_read_config_word(pdev, msix_pos + PCI_MSIX_FLAGS, &flags);
		/* [한국어] Table Offset/BIR 을 읽는다. 하위 3비트가 BIR(어느 BAR 인지),
		 * 나머지 상위 비트가 그 BAR 안에서의 오프셋이다. */
		pci_read_config_dword(pdev, msix_pos + PCI_MSIX_TABLE, &table);

		/* [한국어] 하위 3비트를 떼어 MSI-X 테이블이 든 BAR 번호를 얻는다. */
		vdev->msix_bar = table & PCI_MSIX_TABLE_BIR;
		/* [한국어] 상위 비트를 마스크해 BAR 안에서의 바이트 오프셋을 얻는다. 이 값은
		 * 이미 8바이트 정렬돼 있으므로 시프트가 필요 없다. */
		vdev->msix_offset = table & PCI_MSIX_TABLE_OFFSET;
		/* [한국어] Table Size 는 0 기준이므로 1 을 더하고, MSI-X 항목 하나가 16바이트이므로
		 * 곱한다. 결과가 테이블의 바이트 길이다.
		 * **이 셋(bar/offset/size)이 vfio_pci_rdwr.c 의 read/write 제외 구간을 정한다.** */
		vdev->msix_size = ((flags & PCI_MSIX_FLAGS_QSIZE) + 1) * 16;
		/* [한국어] MSI-X 를 켠 뒤에도 벡터를 더 할당할 수 있는 플랫폼인지 물어 둔다
		 * (drivers/pci/msi/api.c:287). 이 값이 GET_IRQ_INFO 에서
		 * VFIO_IRQ_INFO_NORESIZE 플래그의 유무로 사용자에게 전달된다. */
		vdev->has_dyn_msix = pci_msix_can_alloc_dyn(pdev);
	/* [한국어] MSI-X capability 가 없는 장치. */
	} else {
		/* [한국어] 어떤 BAR 인덱스(0~5)와도 겹치지 않는 값을 넣어, 아래 read/write 경로의
		 * `bar == vdev->msix_bar` 비교가 절대 참이 되지 않게 한다.
		 * [상류 코드 관찰] 이 분기는 msix_offset 과 msix_size 를 되돌리지 않는다.
		 * msix_bar 가 0xFF 라 그 둘을 읽는 조건이 성립하지 않으므로 결과에는
		 * 영향이 없다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		vdev->msix_bar = 0xFF;
		/* [한국어] 동적 벡터 추가도 당연히 불가능하다. */
		vdev->has_dyn_msix = false;
	}

	/* [한국어] VGA 지원이 켜져 있고 이 장치가 VGA 클래스이면. */
	if (!vfio_vga_disabled() && vfio_pci_is_vga(pdev))
		/* [한국어] VGA region 을 노출한다. 이 플래그가 없으면 GET_REGION_INFO 의
		 * VGA 인덱스 조회가 -EINVAL 이 된다. */
		vdev->has_vga = true;


	/* [한국어] 여기까지 왔으면 장치는 사용자에게 넘길 준비가 끝났다. 남은 것은
	 * vendor 의 region 등록과 vfio_pci_core_finish_enable 이다. */
	return 0;

/* [한국어] config 그림자 생성이 실패했을 때 들어온다. */
out_free_zdev:
	/* [한국어] zdev 준비를 되돌린다. */
	vfio_pci_zdev_close_device(vdev);
/* [한국어] zdev 준비가 실패했을 때 들어온다. */
out_free_state:
	/* [한국어] 스냅숏 복제본을 해제한다. */
	kfree(vdev->pci_saved_state);
	/* [한국어] 다음 열기에서 해제된 포인터를 쓰지 않도록 비운다. */
	vdev->pci_saved_state = NULL;
/* [한국어] 장치 켜기나 초기 리셋이 실패했을 때 들어온다. */
out_disable_device:
	/* [한국어] 장치를 끈다(drivers/pci/pci.c:4314). */
	pci_disable_device(pdev);
/* [한국어] 런타임 PM 참조를 얻은 뒤의 모든 실패가 여기로 모인다. */
out_power:
	/* [한국어] 참조를 얻었던 조건과 같아야 계수가 맞는다. */
	if (!disable_idle_d3)
		/* [한국어] 장치를 다시 잠들 수 있게 놓아 준다. */
		pm_runtime_put(&pdev->dev);
	/* [한국어] 실패 원인을 그대로 vendor 의 open_device 에 돌려준다. */
	return ret;
}
/* [한국어] vendor 드라이버가 open_device 에서 부를 수 있도록 내보낸다.
 * 이 파일의 모든 EXPORT 는 _GPL 판이다(총 23개). */
EXPORT_SYMBOL_GPL(vfio_pci_core_enable);

/* [한국어]
 * vfio_pci_core_disable - 열기가 잡은 모든 것을 역순으로 놓고 장치를 리셋해 돌려준다
 *
 * @vdev: 대상 디바이스.
 * @return: 없음. 실패해도 계속 진행한다 — 닫기는 중단할 수 없다.
 *
 * 왜 필요한가: **하드웨어 소유권이 사용자에서 커널로 돌아오는 지점**이다.
 * 다음 사용자(또는 호스트 드라이버)에게 장치를 넘기기 전에, 사용자가 남긴
 * 설정과 진행 중인 DMA 와 인터럽트를 전부 지워야 한다. 그러지 않으면 열린
 * 채로 남은 DMA 가 새 IOVA 매핑을 타고 엉뚱한 메모리를 덮어쓸 수 있다.
 *
 * 동작 과정(순서 자체가 안전의 근거다):
 *  1. 저전력 상태를 되돌리고 pm_runtime_resume 으로 장치를 깨운다. 잠든 장치는
 *     config 도 MMIO 도 응답하지 않으므로 아래 모든 단계의 전제다.
 *  2. vfio_pci_set_power_state(PCI_D0) — 원본 주석대로 아래에서 쓸
 *     __pci_reset_function_locked 가 내부적으로 pci_pm_reset 을 쓸 수 있는데
 *     그것은 D0 가 아니면 실패한다.
 *  3. **pci_clear_master** — 더 이상의 DMA 를 끊는다. 이것이 가장 먼저 와야
 *     하는 실질적 정리다.
 *  4. 모든 인터럽트를 해제한다(vfio_pci_set_irqs_ioctl 에 DATA_NONE +
 *     ACTION_TRIGGER 로 현재 irq_type 을 통째로 끈다).
 *  5. ioeventfd 목록을 비운다. 디바이스가 닫히는 중이라 경합이 없어
 *     ioeventfds_lock 을 잡지 않는다(원본 영어 주석).
 *  6. vendor 가 등록한 추가 region 들의 release 콜백을 부르고 배열을 해제한다.
 *     region 배열 포인터를 NULL 로 되돌리는 이유는 다음 열기에서 krealloc 이
 *     해제된 포인터를 받지 않게 하려는 것이다(원본 주석).
 *  7. vfio_config_free — vconfig 그림자를 없앤다. 이 이후 사용자의 config
 *     접근은 의미가 없다.
 *  8. BAR 매핑 해제: barmap 에 남은 iomap 을 풀고 요청했던 region 을 놓는다.
 *  9. dummy resource 들을 자원 트리에서 빼고 해제한다.
 * 10. needs_reset 을 세운다. 아래에서 리셋에 성공하면 다시 내린다.
 * 11. 저장해 둔 스냅숏을 되돌린다. 실패하면(스냅숏이 없거나 형식이 안 맞으면)
 *     리셋조차 못 하는 장치는 그대로 포기하고, 리셋 가능한 장치는 현재 상태를
 *     다시 저장해 두었다가 리셋 뒤 복원한다.
 * 12. PCI_COMMAND 에 INTX_DISABLE 만 써서 INTx 와 MSI 를 끈다. 원본 주석대로
 *     pci_reset_function 에서 가져온 관용구이며, 리셋 도중의 헛 인터럽트를 막는다.
 * 13. 리셋: **pci_try_reset_function 이 아니라** pci_dev_trylock +
 *     __pci_reset_function_locked 를 쓴다. 원본 주석이 이유를 밝힌다 —
 *     "try" 계열은 내부에서 상태를 다시 저장해 방금 복원한 설정을 덮어쓴다.
 *     상위 브리지까지 trylock 하는 이유는 리셋이 링크를 건드리기 때문이고,
 *     trylock 인 이유는 교착을 피하려는 것이다.
 * 14. 상태를 하드웨어에 쓰고 장치를 끈다.
 * 15. vfio_pci_dev_set_try_reset 으로 묶음 전체 리셋을 시도한다. 이 함수에서
 *     리셋에 실패했더라도, 같은 슬롯/버스의 모든 디바이스가 놀고 있다면
 *     버스 리셋으로 확실히 지울 수 있기 때문이다.
 * 16. 열기에서 얻은 런타임 PM 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥이며 **dev_set->lock 을 쥔 채** 불린다
 * (맨 앞의 lockdep_assert_held 가 그 계약을 못 박는다). needs_reset 을
 * 묶음 단위로 다루기 때문에 그 락이 필요하다.
 *
 * 호출자: vfio_pci_core_close_device, 그리고 vendor 의 open_device 실패
 * 되감기(drivers/vfio/pci/vfio_pci.c:118).
 * 호출 대상: 위 각 단계의 PCI 코어 함수들과 형제 파일의 정리 함수들.
 *
 * 에러 경로: 이 함수는 실패를 보고하지 않는다. 각 단계는 실패해도 로그를
 * 남기거나 조용히 건너뛰고 다음으로 간다 — 닫기를 중간에 멈추면 자원이
 * 영구히 새기 때문이다.
 *
 * 호출 체인:
 *   사용자 close(2) → vfio_main.c:vfio_df_close → vendor close_device
 *     → vfio_pci_core_close_device → [vfio_pci_core_disable]
 *       → __pci_reset_function_locked / pci_restore_state / pci_disable_device
 */
void vfio_pci_core_disable(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 리셋 때 함께 잠글 상위 브리지. */
	struct pci_dev *bridge;
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] dummy resource 목록을 안전하게 순회하며 지우기 위한 짝. */
	struct vfio_pci_dummy_resource *dummy_res, *tmp;
	/* [한국어] ioeventfd 목록을 안전하게 순회하며 지우기 위한 짝. */
	struct vfio_pci_ioeventfd *ioeventfd, *ioeventfd_tmp;
	/* [한국어] BAR 순회용 변수 둘. */
	int i, bar;

	/* For needs_reset */
	/* [한국어] needs_reset 은 묶음 단위로 다루므로 dev_set->lock 아래에서만 만져야 한다.
	 * 호출자가 그 락을 쥐고 있음을 디버그 빌드에서 확인한다. */
	lockdep_assert_held(&vdev->vdev.dev_set->lock);

	/*
	 * This function can be invoked while the power state is non-D0.
	 * This non-D0 power state can be with or without runtime PM.
	 * vfio_pci_runtime_pm_exit() will internally increment the usage
	 * count corresponding to pm_runtime_put() called during low power
	 * feature entry and then pm_runtime_resume() will wake up the device,
	 * if the device has already gone into the suspended state. Otherwise,
	 * the vfio_pci_set_power_state() will change the device power state
	 * to D0.
	 */
	/* [한국어] 저전력 진입 상태를 되돌린다. 진입 때 내렸던 usage count 가 여기서 올라간다. */
	vfio_pci_runtime_pm_exit(vdev);
	/* [한국어] 장치가 이미 잠들었다면 깨운다. 잠든 장치는 config 도 MMIO 도 응답하지
	 * 않으므로 아래 모든 단계의 전제다. */
	pm_runtime_resume(&pdev->dev);

	/*
	 * This function calls __pci_reset_function_locked() which internally
	 * can use pci_pm_reset() for the function reset. pci_pm_reset() will
	 * fail if the power state is non-D0. Also, for the devices which
	 * have NoSoftRst-, the reset function can cause the PCI config space
	 * reset without restoring the original state (saved locally in
	 * 'vdev->pm_save').
	 */
	/* [한국어] 전원을 D0 로 확실히 맞춘다. 아래에서 쓸 __pci_reset_function_locked 가
	 * 내부적으로 pci_pm_reset 을 쓸 수 있는데 그것은 D0 가 아니면 실패한다. */
	vfio_pci_set_power_state(vdev, PCI_D0);

	/* Stop the device from further DMA */
	/* [한국어] **더 이상의 DMA 를 끊는다.** 사용자가 남긴 진행 중 DMA 가 다음 사용자의
	 * IOVA 매핑을 타고 엉뚱한 메모리를 덮어쓰지 못하게 하는 핵심 한 줄이다
	 * (drivers/pci/pci.c:7923). */
	pci_clear_master(pdev);

	/* [한국어] 현재 설정된 인터럽트를 통째로 끈다. DATA_NONE 은 데이터 배열이 없다는 뜻,
	 * ACTION_TRIGGER 는 트리거(eventfd) 설정을 대상으로 한다는 뜻이다. */
	vfio_pci_set_irqs_ioctl(vdev, VFIO_IRQ_SET_DATA_NONE |
				/* [한국어] 이 조합으로 count=0 을 주면 "그 종류를 전부 해제하라" 가 된다. */
				VFIO_IRQ_SET_ACTION_TRIGGER,
				/* [한국어] 지금 쓰고 있는 종류(irq_type)에 대해 start=0, count=0, data=NULL.
				 * 아직 아무것도 설정하지 않았다면 irq_type 은 VFIO_PCI_NUM_IRQS 이고
				 * vfio_pci_intrs.c 가 그것을 걸러 낸다. */
				vdev->irq_type, 0, 0, NULL);

	/* Device closed, don't need mutex here */
	/* [한국어] ioeventfd 목록을 지우며 순회한다. _safe 판을 쓰는 이유는 순회 중에
	 * 현재 노드를 해제하기 때문이다. */
	list_for_each_entry_safe(ioeventfd, ioeventfd_tmp,
				 &vdev->ioeventfds_list, next) {
		/* [한국어] 이 ioeventfd 에 걸린 virqfd 작업을 멈추고 정리한다. */
		vfio_virqfd_disable(&ioeventfd->virqfd);
		/* [한국어] 목록에서 뺀다. */
		list_del(&ioeventfd->next);
		/* [한국어] 노드를 해제한다. */
		kfree(ioeventfd);
	}
	/* [한국어] 등록 개수를 0 으로 되돌린다. 이 값이 VFIO_PCI_IOEVENTFD_MAX 상한 검사에 쓰인다. */
	vdev->ioeventfds_nr = 0;

	/* [한국어] 가상 INTx 비활성 상태를 초기값으로 되돌린다. 사용자가 vconfig 를 통해
	 * DisINTx 를 세워 둔 상태로 닫았을 수 있다. */
	vdev->virq_disabled = false;

	/* [한국어] vendor 가 등록한 추가 region 을 차례로 정리한다. */
	for (i = 0; i < vdev->num_regions; i++)
		/* [한국어] 각 region 의 release 콜백. **NULL 검사 없이 부른다** — rw 와 release 는
		 * regops 의 필수 슬롯이라는 규약이 이 한 줄로 표현돼 있다.
		 * 이 트리의 두 regops 표(vfio_pci_igd.c:163, 385)는 모두 채운다. */
		vdev->region[i].ops->release(vdev, &vdev->region[i]);

	/* [한국어] 개수를 0 으로 되돌려 다음 열기가 처음부터 등록하게 한다. */
	vdev->num_regions = 0;
	/* [한국어] region 배열 자체를 해제한다. */
	kfree(vdev->region);
	/* [한국어] 다음 열기에서 vfio_pci_core_register_dev_region 의 krealloc 이 해제된
	 * 포인터를 받지 않도록 비운다. 원본 주석이 그 이유를 그대로 적고 있다. */
	vdev->region = NULL; /* don't krealloc a freed pointer */

	/* [한국어] vconfig 그림자와 권한 지도를 해제한다(vfio_pci_config.c:1854).
	 * 이 뒤로 사용자의 config 접근은 의미가 없다. */
	vfio_config_free(vdev);

	/* [한국어] 표준 BAR 여섯 개를 훑는다. */
	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		/* [한국어] resource 배열에서의 절대 인덱스로 바꾼다. */
		bar = i + PCI_STD_RESOURCES;
		/* [한국어] 매핑한 적 없는 BAR 는 건너뛴다. */
		if (!vdev->barmap[bar])
			/* [한국어] 다음 BAR 로. */
			continue;
		/* [한국어] 커널 가상 주소 매핑을 해제한다(drivers/pci/iomap.c:417). */
		pci_iounmap(pdev, vdev->barmap[bar]);
		/* [한국어] 그 BAR 의 자원 요청을 놓아 다른 드라이버가 가져갈 수 있게 한다.
		 * 비트마스크로 BAR 번호를 지정하므로 1 을 그 번호만큼 왼쪽으로 민다
		 * (drivers/pci/pci.c:7258). */
		pci_release_selected_regions(pdev, 1 << bar);
		/* [한국어] 슬롯을 비워 다음 열기가 처음부터 매핑하게 한다. */
		vdev->barmap[bar] = NULL;
	}

	/* [한국어] sub-page BAR 를 위해 예약해 둔 자리표들을 정리한다. */
	list_for_each_entry_safe(dummy_res, tmp,
				 &vdev->dummy_resources_list, res_next) {
		/* [한국어] 목록에서 뺀다. */
		list_del(&dummy_res->res_next);
		/* [한국어] 자원 트리에서 예약을 푼다. 이제 그 구간을 다른 장치가 쓸 수 있다. */
		release_resource(&dummy_res->resource);
		/* [한국어] 자리표를 해제한다. */
		kfree(dummy_res);
	}

	/* [한국어] 일단 "더럽다" 로 표시한다. 아래에서 리셋에 성공하면 다시 내린다.
	 * 실패하면 이 표시가 남아 vfio_pci_dev_set_try_reset 이 버스 리셋을 시도한다. */
	vdev->needs_reset = true;

	/* [한국어] s390 zPCI 정리. 다른 아키텍처에서는 stub 이라 아무 일도 하지 않는다. */
	vfio_pci_zdev_close_device(vdev);

	/*
	 * If we have saved state, restore it.  If we can reset the device,
	 * even better.  Resetting with current state seems better than
	 * nothing, but saving and restoring current state without reset
	 * is just busy work.
	 */
	/* [한국어] 열기 때 떠 둔 스냅숏을 PCI 코어의 saved state 로 되돌린다.
	 * 0 이 아닌 값은 실패를 뜻한다(스냅숏이 없거나 형식이 맞지 않음). */
	if (pci_load_and_free_saved_state(pdev, &vdev->pci_saved_state)) {
		/* [한국어] 복원에 실패했음을 알린다. */
		pci_info(pdev, "%s: Couldn't reload saved state\n", __func__);

		/* [한국어] 리셋조차 못 하는 장치라면 지금 상태로 쓸 수 있는 방법이 없다. */
		if (!vdev->reset_works)
			/* [한국어] 복원도 리셋도 건너뛰고 장치를 끄러 간다. */
			goto out;

		/* [한국어] 리셋은 가능하니, 원본 주석대로 "현재 상태로라도 리셋하는 편이
		 * 아무것도 안 하는 것보다 낫다". 지금 상태를 저장해 두었다가 리셋 뒤 복원한다. */
		pci_save_state(pdev);
	}

	/*
	 * Disable INTx and MSI, presumably to avoid spurious interrupts
	 * during reset.  Stolen from pci_reset_function()
	 */
	/* [한국어] 명령 레지스터에 INTX_DISABLE 하나만 쓴다 — 나머지 비트가 모두 0 이 되므로
	 * 메모리/IO 디코드와 버스 마스터도 함께 꺼진다. 리셋 도중의 헛 인터럽트와
	 * 접근을 막는 관용구이며 원본 주석대로 pci_reset_function 에서 가져왔다. */
	pci_write_config_word(pdev, PCI_COMMAND, PCI_COMMAND_INTX_DISABLE);

	/*
	 * Try to get the locks ourselves to prevent a deadlock. The
	 * success of this is dependent on being able to lock the device,
	 * which is not always possible.
	 * We can not use the "try" reset interface here, which will
	 * overwrite the previously restored configuration information.
	 */
	/* [한국어] 리셋 가능한 장치일 때만 시도한다. */
	if (vdev->reset_works) {
		/* [한국어] 이 장치의 상위 브리지. 루트 포트 아래 직결이면 그 루트 포트다. */
		bridge = pci_upstream_bridge(pdev);
		/* [한국어] 브리지가 있는데 잠글 수 없으면 지금 리셋하지 않는다. 리셋이 브리지
		 * 링크를 건드리므로 그쪽도 잠가야 안전하다. trylock 이라 잠들지 않는다. */
		if (bridge && !pci_dev_trylock(bridge))
			/* [한국어] 상태만 복원하고 나간다. needs_reset 은 참으로 남는다. */
			goto out_restore_state;
		/* [한국어] 장치 자신을 잠근다. 실패하면 리셋을 건너뛴다 — 그래도 needs_reset 이
		 * 남아 나중에 버스 리셋으로 처리될 수 있다. */
		if (pci_dev_trylock(pdev)) {
			/* [한국어] 함수 리셋을 수행한다. "try" 판이 아니라 이 판을 쓰는 이유는 원본 주석대로
			 * "try" 계열이 내부에서 상태를 다시 저장해 방금 복원한 설정을 덮어쓰기
			 * 때문이다(drivers/pci/pci.c:10016). */
			if (!__pci_reset_function_locked(pdev))
				/* [한국어] 리셋에 성공했으니 더 이상 더럽지 않다. */
				vdev->needs_reset = false;
			/* [한국어] 장치 잠금을 푼다. */
			pci_dev_unlock(pdev);
		}
		/* [한국어] 브리지를 잠갔다면. */
		if (bridge)
			/* [한국어] 브리지 잠금도 푼다. 잠근 역순으로 푼다. */
			pci_dev_unlock(bridge);
	}

/* [한국어] 브리지 잠금 실패로 리셋을 건너뛴 경우가 여기로 온다. */
out_restore_state:
	/* [한국어] 저장해 둔 상태를 실제 하드웨어 config 에 쓴다(drivers/pci/pci.c:3530). */
	pci_restore_state(pdev);
/* [한국어] 상태 복원조차 못 한 경우가 여기로 온다. */
out:
	/* [한국어] 장치를 끈다(drivers/pci/pci.c:4314). */
	pci_disable_device(pdev);

	/* [한국어] 함수 리셋이 실패했더라도, 같은 슬롯/버스의 모든 디바이스가 놀고 있다면
	 * 버스 리셋으로 확실히 지울 수 있다. 그 마지막 기회를 시도한다. */
	vfio_pci_dev_set_try_reset(vdev->vdev.dev_set);

	/* Put the pm-runtime usage counter acquired during enable */
	/* [한국어] 열기에서 참조를 얻었던 조건과 같아야 계수가 맞는다. */
	if (!disable_idle_d3)
		/* [한국어] 런타임 PM 참조를 놓는다. 이제 장치는 다시 D3 로 잠들 수 있다. */
		pm_runtime_put(&pdev->dev);
}
/* [한국어] vendor 드라이버가 close_device 나 open_device 실패 되감기에서 쓸 수 있게
 * 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_disable);

/* [한국어]
 * vfio_pci_core_close_device - vfio_device_ops 의 close_device 슬롯 구현
 *
 * @core_vdev: 코어 쪽 struct vfio_device. container_of 로 PCI 쪽 객체를 되찾는다.
 * @return: 없음.
 *
 * 왜 필요한가: 코어(vfio_main.c 의 vfio_df_close)는 **마지막 fd 가 닫힐 때만**
 * 이 콜백을 부른다. 즉 여기가 "이 장치를 쓰는 사람이 아무도 없다" 가 확정되는
 * 지점이며, PF/VF 신뢰 관계와 EEH 소유권과 하드웨어 정리를 한 번에 끝낸다.
 *
 * 동작 과정:
 *  1. 이 장치가 VF 이고 그 PF 도 vfio-pci 라면, PF 의 vf_token->users 를 하나
 *     줄인다. 0 이 되면 PF 를 여는 사용자가 토큰을 새로 설정할 수 있게 된다.
 *     WARN_ON 은 열기와 닫기의 짝이 어긋난 경우를 잡는다.
 *  2. EEH 가 있는 아키텍처에서는 eeh_dev_release 로 소유권을 커널에 돌려준다.
 *  3. vfio_pci_core_disable 로 하드웨어를 정리한다.
 *  4. dma-buf 로 내준 BAR 를 모두 정리한다.
 *  5. igate 를 잡고 err_trigger 와 req_trigger 두 eventfd 슬롯을 비운다.
 *     이 시점에는 인터럽트가 이미 다 풀렸지만, RCU 독자가 아직 남아 있을 수
 *     있으므로 kfree 가 아니라 replace_locked 를 거쳐 유예 해제를 쓴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock 을 쥔 상태다(그래서 3단계의
 * lockdep_assert_held 가 통과한다).
 *
 * 호출자: vfio_main.c 의 vfio_df_close 가 device->ops->close_device 로 부른다.
 * vendor 가 자기 정리를 덧붙일 때는 자기 close_device 안에서 이것을 부른다
 * (drivers/vfio/pci/xe/main.c:174).
 * 호출 대상: vfio_pci_core_disable, vfio_pci_dma_buf_cleanup
 * (vfio_pci_dmabuf.c:379), vfio_pci_eventfd_replace_locked.
 *
 * 에러 경로: 없다. 5단계의 replace_locked 는 ctx 가 NULL 이라 할당을 하지
 * 않으므로 실패할 수 없고, 그래서 반환값을 보지 않는다.
 *
 * 호출 체인:
 *   사용자 close(2) → vfio_main.c:vfio_df_close
 *     → [vfio_pci_core_close_device] → vfio_pci_core_disable
 */
void vfio_pci_core_close_device(struct vfio_device *core_vdev)
{
	/* [한국어] 코어 쪽 struct vfio_device 는 PCI 확장 구조체의 첫 필드다.
	 * container_of 로 그 오프셋을 빼면 PCI 객체가 나온다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	/* [한국어] 이 장치가 VF 이고 그 PF 도 vfio-pci 로 잡혀 있을 때만 참이다.
	 * vfio_pci_vf_init 이 채워 둔 포인터다. */
	if (vdev->sriov_pf_core_dev) {
		/* [한국어] PF 의 토큰 락을 잡는다. **VF 를 닫는 스레드가 PF 객체의 락을 잡는다**. */
		mutex_lock(&vdev->sriov_pf_core_dev->vf_token->lock);
		/* [한국어] 열기와 닫기의 짝이 맞으면 users 가 0 일 수 없다. 0 이면 수명 관리가
		 * 깨진 것이므로 경고한다. */
		WARN_ON(!vdev->sriov_pf_core_dev->vf_token->users);
		/* [한국어] VF 사용자 수를 하나 줄인다. 0 이 되면 PF 를 여는 사용자가 토큰을
		 * 새로 설정할 수 있게 된다. */
		vdev->sriov_pf_core_dev->vf_token->users--;
		/* [한국어] PF 의 토큰 락을 놓는다. */
		mutex_unlock(&vdev->sriov_pf_core_dev->vf_token->lock);
	}
/* [한국어] EEH 는 IBM POWER 계열에만 있는 PCI 오류 격리 기능이다. */
#if IS_ENABLED(CONFIG_EEH)
	/* [한국어] 사용자가 장치를 놓았음을 EEH 계층에 알려, 커널이 다시 복구를 주관하게 한다. */
	eeh_dev_release(vdev->pdev);
#endif
	/* [한국어] 하드웨어를 정리하고 리셋한다. 이 함수 안에서 dev_set->lock 을 요구하는데,
	 * 호출자(vfio_main.c 의 vfio_df_close)가 이미 쥐고 있다. */
	vfio_pci_core_disable(vdev);

	/* [한국어] BAR 를 dma-buf 로 내주었던 것들을 모두 정리한다(vfio_pci_dmabuf.c:379). */
	vfio_pci_dma_buf_cleanup(vdev);

	/* [한국어] eventfd 슬롯 교체는 igate 아래에서만 허용된다. */
	mutex_lock(&vdev->igate);
	/* [한국어] AER 통보 채널을 비운다. ctx 가 NULL 이라 할당이 없어 실패할 수 없으므로
	 * 반환값을 보지 않는다. */
	vfio_pci_eventfd_replace_locked(vdev, &vdev->err_trigger, NULL);
	/* [한국어] 디바이스 반납 요청 채널을 비운다. 이 뒤로 vfio_pci_core_request 는
	 * "등록된 채널 없음" 경로로 간다. */
	vfio_pci_eventfd_replace_locked(vdev, &vdev->req_trigger, NULL);
	/* [한국어] 락을 놓는다. 이제 이 디바이스에는 아무 사용자도 없다. */
	mutex_unlock(&vdev->igate);
}
/* [한국어] vendor 가 자기 close_device 안에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_close_device);

/* [한국어]
 * vfio_pci_core_finish_enable - 열기의 마지막 마무리(BAR mmap 표 확정과 소유권 표시)
 *
 * @vdev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_pci_core_enable 과 이 함수 사이에 vendor 드라이버가 자기
 * 추가 region 을 등록하고 자기 초기화를 한다. mmap 가능 여부 판정은 그 뒤에
 * 와야 하므로 열기를 두 조각으로 나눈 것이다. vendor 의 open_device 는
 * "core_enable → 내 일 → core_finish_enable" 순으로 부르는 것이 규약이다
 * (drivers/vfio/pci/vfio_pci.c:110~123 이 그 표준 형태다).
 *
 * 동작 과정:
 *  1. vfio_pci_probe_mmaps 로 BAR 여섯 개의 mmap 가능 여부를 확정한다.
 *  2. EEH 아키텍처에서 eeh_dev_open 으로 "사용자가 소유 중" 을 표시한다.
 *  3. 이 장치가 VF 이고 PF 도 vfio-pci 라면 PF 의 vf_token->users 를 늘린다.
 *     이 순간부터 그 PF 를 열려는 사용자는 토큰을 맞혀야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. vendor 의 open_device 안이며 dev_set->lock 을
 * 쥔 상태다.
 *
 * 호출자: vendor 의 open_device — vfio_pci.c:123, xe/main.c:161, ism/main.c:64 등.
 * 호출 대상: vfio_pci_probe_mmaps, eeh_dev_open.
 *
 * 에러 경로: 없다. probe_mmaps 의 실패는 "그 BAR 는 mmap 불가" 로 흡수된다.
 *
 * 호출 체인:
 *   vendor open_device → [vfio_pci_core_finish_enable] → vfio_pci_probe_mmaps
 */
void vfio_pci_core_finish_enable(struct vfio_pci_core_device *vdev)
{
	/* [한국어] BAR 여섯 개의 mmap 가능 여부를 확정한다. vendor 가 region 을 다 등록한
	 * 뒤여야 하므로 열기가 두 조각으로 나뉘어 있다. */
	vfio_pci_probe_mmaps(vdev);
/* [한국어] EEH 아키텍처 전용. */
#if IS_ENABLED(CONFIG_EEH)
	/* [한국어] 사용자가 장치를 소유했음을 EEH 계층에 알려, 커널이 임의로 복구하지
	 * 않게 한다. */
	eeh_dev_open(vdev->pdev);
#endif

	/* [한국어] 이 장치가 VF 이고 그 PF 도 vfio-pci 일 때만. */
	if (vdev->sriov_pf_core_dev) {
		/* [한국어] PF 의 토큰 락을 잡는다. */
		mutex_lock(&vdev->sriov_pf_core_dev->vf_token->lock);
		/* [한국어] VF 사용자 수를 하나 늘린다. **이 순간부터 그 PF 를 열려는 사용자는
		 * 토큰을 맞혀야 한다.** */
		vdev->sriov_pf_core_dev->vf_token->users++;
		/* [한국어] PF 의 토큰 락을 놓는다. */
		mutex_unlock(&vdev->sriov_pf_core_dev->vf_token->lock);
	}
}
/* [한국어] vendor 가 자기 open_device 마지막에 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_finish_enable);

/* [한국어]
 * vfio_pci_get_irq_count - 다섯 종류 인터럽트 각각의 최대 개수를 센다
 *
 * @vdev:     대상 디바이스.
 * @irq_type: VFIO_PCI_INTX_IRQ_INDEX 부터 VFIO_PCI_REQ_IRQ_INDEX 까지의 인덱스.
 * @return: 그 종류로 쓸 수 있는 최대 벡터 수. 지원하지 않으면 0.
 *
 * 왜 필요한가: 사용자에게 "몇 개까지 요청할 수 있는가" 를 알려 주는 것이자,
 * 사용자가 요청한 개수가 범위 안인지 검증하는 기준이다. 두 용도가 같은
 * 숫자를 써야 하므로 한 함수로 모았다.
 *
 * 동작 과정(종류별로):
 *  - INTx: **vconfig 그림자**의 Interrupt Pin 바이트가 0 이 아니면 1, 0 이면 0.
 *    그림자를 보는 이유는 vfio_pci_nointx 로 감춘 장치가 여기서 0 으로
 *    보여야 하기 때문이다 — 감추기의 효과가 바로 여기서 나타난다.
 *  - MSI: 하드웨어 MSI capability 의 Message Control 에서 Multiple Message
 *    Capable 필드를 뽑아 2 의 거듭제곱으로 바꾼다. PCI 스펙상 그 필드는
 *    log2 개수를 담으므로 (flags & QMASK) >> 1 만큼 왼쪽 시프트한다.
 *  - MSI-X: MSI-X Message Control 의 Table Size 필드 + 1. 스펙이 0 기준
 *    개수를 쓰기 때문이다.
 *  - ERR: PCIe 장치일 때만 1(AER 통보 채널 하나).
 *  - REQ: 항상 1(디바이스 반납 요청 채널 하나). PCI 종류와 무관하다.
 *
 * **MSI 와 MSI-X 는 그림자가 아니라 하드웨어 config 를 직접 읽는다.**
 * 장치가 실제로 제공하는 능력은 사용자가 바꿀 수 없어야 하기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다 — 읽는 값이 장치 고정
 * 능력이거나(MSI/MSI-X) 열기 이후 변하지 않는 그림자 바이트(INTx)다.
 *
 * 호출자: vfio_pci_ioctl_get_irq_info(사용자에게 보고)와
 * vfio_pci_ioctl_set_irqs(요청 검증의 상한).
 * 호출 대상: pci_read_config_word.
 *
 * 에러 경로: config 읽기 실패를 검사하지 않는다. 실패하면 flags 가 불정값이
 * 되지만, 그 결과는 잘못된 개수 보고이지 메모리 안전 문제는 아니다.
 *
 * 호출 체인:
 *   vfio_pci_core_ioctl → vfio_pci_ioctl_get_irq_info 또는 _set_irqs
 *     → [vfio_pci_get_irq_count] → pci_read_config_word
 */
static int vfio_pci_get_irq_count(struct vfio_pci_core_device *vdev, int irq_type)
{
	/* [한국어] INTx 개수 질의. */
	if (irq_type == VFIO_PCI_INTX_IRQ_INDEX) {
		/* [한국어] **하드웨어가 아니라 vconfig 그림자**의 Interrupt Pin 바이트를 본다.
		 * vfio_pci_nointx 로 감춘 장치는 그 그림자가 0 이라 여기서 0 이 나온다 —
		 * 감추기의 효과가 정확히 이 한 줄에서 나타난다. 0 이 아니면 INTx 는
		 * 선 하나이므로 개수는 언제나 1 이다. */
		return vdev->vconfig[PCI_INTERRUPT_PIN] ? 1 : 0;
	/* [한국어] MSI 개수 질의. */
	} else if (irq_type == VFIO_PCI_MSI_IRQ_INDEX) {
		/* [한국어] MSI capability 의 config 오프셋. */
		u8 pos;
		/* [한국어] Message Control 레지스터 값. */
		u16 flags;

		/* [한국어] PCI 코어가 열거 시점에 찾아 둔 MSI capability 오프셋. */
		pos = vdev->pdev->msi_cap;
		/* [한국어] MSI capability 가 없으면 아래 return 0 으로 떨어진다. */
		if (pos) {
			/* [한국어] **그림자가 아니라 하드웨어 config** 를 읽는다. 장치가 실제로 제공하는
			 * 능력은 사용자가 바꿀 수 없어야 하기 때문이다. */
			pci_read_config_word(vdev->pdev,
					     /* [한국어] capability 시작에서 PCI_MSI_FLAGS(=2) 만큼 떨어진 자리가 Message Control 이다. */
					     pos + PCI_MSI_FLAGS, &flags);
			/* [한국어] Multiple Message Capable 필드는 비트 1~3 에 있고 log2 개수를 담는다.
			 * 그래서 1비트 오른쪽으로 밀어 지수를 얻고 2 의 거듭제곱으로 되돌린다.
			 * 값 0 이면 1개, 5 면 32개다(PCI 스펙의 MSI 최대치). */
			return 1 << ((flags & PCI_MSI_FLAGS_QMASK) >> 1);
		}
	/* [한국어] MSI-X 개수 질의. */
	} else if (irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
		/* [한국어] MSI-X capability 의 config 오프셋. */
		u8 pos;
		/* [한국어] Message Control 레지스터 값. */
		u16 flags;

		/* [한국어] PCI 코어가 찾아 둔 MSI-X capability 오프셋. */
		pos = vdev->pdev->msix_cap;
		/* [한국어] MSI-X capability 가 없으면 아래 return 0 으로 떨어진다. */
		if (pos) {
			/* [한국어] 여기도 하드웨어 config 를 직접 읽는다. */
			pci_read_config_word(vdev->pdev,
					     /* [한국어] capability 시작에서 PCI_MSIX_FLAGS(=2) 만큼 떨어진 자리. */
					     pos + PCI_MSIX_FLAGS, &flags);

			/* [한국어] Table Size 필드(하위 11비트)는 0 기준이므로 1 을 더한다.
			 * 최대 2048개까지 표현된다. */
			return (flags & PCI_MSIX_FLAGS_QSIZE) + 1;
		}
	/* [한국어] AER 오류 통보 채널 질의. */
	} else if (irq_type == VFIO_PCI_ERR_IRQ_INDEX) {
		/* [한국어] PCIe 장치만 AER 를 갖는다. 레거시 PCI 는 그 개념이 없다. */
		if (pci_is_pcie(vdev->pdev))
			/* [한국어] 통보 채널은 하나뿐이다. */
			return 1;
	/* [한국어] 디바이스 반납 요청 채널 질의. */
	} else if (irq_type == VFIO_PCI_REQ_IRQ_INDEX) {
		/* [한국어] PCI 종류와 무관하게 항상 하나다. */
		return 1;
	}

	/* [한국어] 지원하지 않는 종류이거나 capability 가 없는 경우. */
	return 0;
}

/* [한국어]
 * vfio_pci_count_devs - 버스 순회 콜백: 만난 장치 수를 하나 늘린다
 *
 * @pdev: 순회가 지금 방문한 PCI 장치. 이 콜백은 쓰지 않는다.
 * @data: int 를 가리키는 포인터. 호출자의 카운터다.
 * @return: 항상 0 — pci_walk_bus 는 콜백이 0 이 아닌 값을 돌려주면 순회를
 *          멈추므로, 0 은 "끝까지 계속하라" 는 뜻이다.
 *
 * 왜 필요한가: hot reset 관련 두 ioctl 이 모두 "영향 받는 장치가 몇 개인가" 를
 * 먼저 알아야 한다 — 하나는 사용자 버퍼 크기를 정하려고, 다른 하나는
 * 사용자가 준 그룹 배열 길이의 상한을 검사하려고. 그 세기 자체를
 * pci_walk_bus 에 맡기기 위한 최소 콜백이다.
 *
 * 동작 과정: 한 줄이다. void 포인터를 int 포인터로 보고 후위 증가시킨다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_walk_bus(drivers/pci/bus.c:1190)의
 * 콜백으로 불리며, 그 함수는 pci_bus_sem 을 읽기로 잡은 채 순회한다.
 *
 * 호출자: vfio_pci_walk_wrapper 를 거쳐 pci_walk_bus.
 * 직접 등록하는 곳은 vfio_pci_ioctl_get_pci_hot_reset_info,
 * vfio_pci_ioctl_pci_hot_reset_groups, 그리고 간접적으로
 * vfio_pci_dev_set_resettable 이 아니라 앞의 둘이다.
 * 호출 대상: 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_for_each_slot_or_bus → pci_walk_bus → vfio_pci_walk_wrapper
 *     → [vfio_pci_count_devs]
 */
static int vfio_pci_count_devs(struct pci_dev *pdev, void *data)
{
	/* [한국어] void 포인터를 int 포인터로 보고 후위 증가시킨다. 호출자가 스택에 둔
	 * 카운터를 가리킨다. */
	(*(int *)data)++;
	/* [한국어] 0 은 "순회를 계속하라" 는 뜻이다. pci_walk_bus 는 0 이 아닌 값을
	 * 받으면 그 자리에서 멈춘다. */
	return 0;
}

/* [한국어] vfio_pci_fill_devs 콜백이 버스를 순회하는 동안 들고 다니는 상태 상자.
 * 왜 필요한가: pci_walk_bus 의 콜백은 인자를 void 포인터 하나만 받는다.
 * 사용자 버퍼, 지금까지 채운 개수, 상한, 그리고 플래그를 함께 넘겨야 하므로
 * 한 구조체에 묶는다. VFIO_DEVICE_GET_PCI_HOT_RESET_INFO 에서만 쓴다. */
struct vfio_pci_fill_info {
	/* [한국어] 이 조회를 요청한 디바이스 자신(코어 쪽 객체).
	 * 설정자: vfio_pci_ioctl_get_pci_hot_reset_info 가 &vdev->vdev 로 채운다.
	 * 읽는 자: vfio_pci_fill_devs 가 vfio_iommufd_device_ictx 로 호출자의 iommufd
	 * 컨텍스트를 얻고, dev_set 을 꺼내는 데 쓴다.
	 * 값 범위: 항상 유효 포인터.
	 * 동기화: 순회 동안 참조만 하며 바꾸지 않는다. */
	struct vfio_device *vdev;
	/* [한국어] 사용자에게 돌려줄 배열의 커널 쪽 임시 버퍼.
	 * 설정자: vfio_pci_ioctl_get_pci_hot_reset_info 가 kzalloc_objs 로 할당해 건다.
	 * 읽는 자: vfio_pci_fill_devs 가 count 번째 원소를 채운다. 다 채운 뒤
	 * 호출자가 copy_to_user 로 옮기고 kfree 한다.
	 * 값 범위: count 개의 struct vfio_pci_dependent_device.
	 * 동기화: 순회는 dev_set->lock 아래에서 단일 스레드로 돈다. */
	struct vfio_pci_dependent_device *devices;
	/* [한국어] 위 배열의 크기(원소 수). 미리 vfio_pci_count_devs 로 센 값이다.
	 * 설정자: 호출자가 fill.nr_devices = count 로 채운다.
	 * 읽는 자: vfio_pci_fill_devs 가 매 원소마다 상한 검사에 쓴다.
	 * 값 범위: 1 이상. 0 이면 호출자가 WARN 후 -ERANGE 로 끝낸다.
	 * 동기화: 순회 중 변하지 않는다. */
	int nr_devices;
	/* [한국어] 지금까지 채운 원소 수이자 다음에 채울 첨자.
	 * 설정자와 읽는 자: vfio_pci_fill_devs 가 후위 증가로 쓴다.
	 * 값 범위: 0 에서 nr_devices 까지. 세는 시점과 채우는 시점 사이에 장치가
	 * 늘어나 이 값이 nr_devices 에 닿으면 -EAGAIN 으로 물러선다 —
	 * **토폴로지가 그 사이에 바뀌었다**는 뜻이다.
	 * 동기화: 순회 중 단일 스레드. */
	u32 count;
	/* [한국어] 사용자에게 돌려줄 헤더 플래그.
	 * 설정자: 호출자가 cdev 로 열린 디바이스일 때
	 * VFIO_PCI_HOT_RESET_FLAG_DEV_ID 와 그 OWNED 짝을 세워 시작한다.
	 * vfio_pci_fill_devs 는 소유하지 않은 장치를 하나라도 만나면 OWNED 비트를
	 * 지운다.
	 * 읽는 자: 순회가 끝난 뒤 호출자가 hdr.flags 에 복사해 사용자에게 보낸다.
	 * 값 범위: 두 비트의 조합. DEV_ID 가 없으면 1세대 문법(그룹 번호 보고)이다.
	 * 동기화: 순회 중 단일 스레드. */
	u32 flags;
};

/* [한국어]
 * vfio_pci_fill_devs - 버스 순회 콜백: 영향 받는 장치 하나를 사용자 배열에 적는다
 *
 * @pdev: 지금 방문한 PCI 장치.
 * @data: struct vfio_pci_fill_info 포인터.
 * @return: 0 이면 계속. -EAGAIN 은 세는 사이 장치가 늘어난 경우,
 *          -EPERM 은 IOMMU 그룹이 없는 장치(격리 불가)라 리셋을 허용할 수 없는
 *          경우. 0 이 아니면 pci_walk_bus 가 그 자리에서 순회를 멈춘다.
 *
 * 왜 필요한가: VFIO_DEVICE_GET_PCI_HOT_RESET_INFO 는 사용자에게 "이 장치를
 * hot reset 하면 함께 리셋되는 함수들이 이것들이다" 를 알려 준다. 사용자는
 * 그 목록을 보고 자기가 전부 소유하고 있는지 판단해 실제 리셋 요청을 만든다.
 * **소유 증명의 문법이 두 세대로 갈리므로 보고 형식도 둘이다.**
 *
 * 동작 과정:
 *  1. 이미 nr_devices 만큼 채웠는데 또 불렸다면 토폴로지가 그 사이에 바뀐
 *     것이므로 -EAGAIN. 사용자는 다시 시도하면 된다.
 *  2. 세그먼트(도메인)/버스/devfn 을 적는다. 이 셋이 PCI 주소다.
 *  3. **2세대(cdev) 문법** — 호출자가 iommufd 로 열려 있으면:
 *     dev_set 에서 그 pci_dev 에 대응하는 vfio_device 를 찾는다. 없으면
 *     NOT_OWNED. 있으면 vfio_iommufd_get_dev_id 로 호출자의 iommufd 안에서의
 *     device id 를 얻어, 양수면 그 id 를, -ENOENT 면 "소유했지만 id 는 없음"
 *     (OWNED)을, 그 밖이면 NOT_OWNED 를 적는다. 하나라도 NOT_OWNED 가 나오면
 *     헤더의 OWNED 플래그를 지워 **사용자에게 "전부는 못 가졌다" 를 알린다**.
 *     원본 주석대로 hot reset 은 영향 범위의 모든 장치가 dev_set 안에 있어야
 *     성립한다.
 *  4. **1세대(group) 문법** — 그렇지 않으면 IOMMU 그룹 번호를 적는다.
 *     그룹이 없으면 격리가 보장되지 않는 장치이므로 -EPERM 으로 즉시 중단한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **dev_set->lock 을 쥔 채** 순회한다
 * (호출자가 순회 전후로 잡고 푼다). dev_set 안을 뒤지기 때문이다.
 *
 * 호출자: vfio_pci_walk_wrapper 를 거쳐 pci_walk_bus.
 * 호출 대상: pci_domain_nr, vfio_iommufd_device_ictx,
 * vfio_find_device_in_devset(vfio_main.c), vfio_iommufd_get_dev_id,
 * iommu_group_get, iommu_group_id, iommu_group_put.
 *
 * 에러 경로: 두 오류 모두 순회를 즉시 멈추고 호출자에게 전달되어 ioctl 이
 * 실패한다. 이미 채운 배열은 호출자가 kfree 한다.
 *
 * 호출 체인:
 *   vfio_pci_ioctl_get_pci_hot_reset_info → vfio_pci_for_each_slot_or_bus
 *     → pci_walk_bus → vfio_pci_walk_wrapper → [vfio_pci_fill_devs]
 */
static int vfio_pci_fill_devs(struct pci_dev *pdev, void *data)
{
	/* [한국어] 이번에 채울 사용자 배열의 한 칸을 가리킬 포인터. */
	struct vfio_pci_dependent_device *info;
	/* [한국어] 호출자가 넘긴 상태 상자. */
	struct vfio_pci_fill_info *fill = data;

	/* The topology changed since we counted devices */
	/* [한국어] 세는 시점과 채우는 시점 사이에 장치가 늘어났다는 뜻이다.
	 * 두 순회가 서로 다른 락 구간에 있어 생기는 경합이다. */
	if (fill->count >= fill->nr_devices)
		/* [한국어] -EAGAIN 으로 순회를 멈춘다. 사용자는 다시 조회하면 된다. */
		return -EAGAIN;

	/* [한국어] 다음 빈 칸을 잡고 카운터를 하나 올린다. */
	info = &fill->devices[fill->count++];
	/* [한국어] PCI 세그먼트(도메인) 번호. 큰 시스템에서는 버스 번호만으로 장치를
	 * 식별할 수 없어 도메인이 함께 필요하다. */
	info->segment = pci_domain_nr(pdev->bus);
	/* [한국어] 버스 번호. */
	info->bus = pdev->bus->number;
	/* [한국어] 장치/함수 번호가 합쳐진 바이트. 상위 5비트가 device, 하위 3비트가 function. */
	info->devfn = pdev->devfn;

	/* [한국어] 2세대(cdev) 문법인가. 호출자가 cdev 로 열린 디바이스일 때만 이 플래그를
	 * 세워 둔다. */
	if (fill->flags & VFIO_PCI_HOT_RESET_FLAG_DEV_ID) {
		/* [한국어] 호출자가 묶여 있는 iommufd 컨텍스트. 소유 판정의 기준이다. */
		struct iommufd_ctx *iommufd = vfio_iommufd_device_ictx(fill->vdev);
		/* [한국어] 이 디바이스가 속한 리셋 묶음. */
		struct vfio_device_set *dev_set = fill->vdev->dev_set;
		/* [한국어] 이 pci_dev 에 대응하는 vfio_device 를 담을 변수. */
		struct vfio_device *vdev;

		/*
		 * hot-reset requires all affected devices be represented in
		 * the dev_set.
		 */
		/* [한국어] 묶음 안에서 이 PCI 함수에 해당하는 vfio_device 를 찾는다.
		 * 원본 주석대로 hot reset 은 영향 범위의 모든 장치가 dev_set 안에 있어야
		 * 성립한다(vfio_main.c 의 헬퍼). */
		vdev = vfio_find_device_in_devset(dev_set, &pdev->dev);
		/* [한국어] 묶음 밖의 장치 — VFIO 가 관리하지 않는 함수다. */
		if (!vdev) {
			/* [한국어] "소유하지 않음" 을 사용자에게 알린다. */
			info->devid = VFIO_PCI_DEVID_NOT_OWNED;
		/* [한국어] 묶음 안에 있는 장치. */
		} else {
			/* [한국어] 호출자의 iommufd 안에서 이 디바이스의 id 를 묻는다. */
			int id = vfio_iommufd_get_dev_id(vdev, iommufd);

			/* [한국어] 양수면 그 iommufd 에 정식으로 바인딩된 디바이스다. */
			if (id > 0)
				/* [한국어] 그 id 를 그대로 알려 준다. 사용자는 이 값으로 자기 소유임을 확인한다. */
				info->devid = id;
			/* [한국어] -ENOENT 는 아직 어떤 iommufd 에도 바인딩되지 않았다는 뜻이다. */
			else if (id == -ENOENT)
				/* [한국어] 그래도 소유로 센다. 그 iommu_group 이 이미 이 iommufd 에 소유돼 있어
				 * 다른 iommufd 가 가져갈 수 없기 때문이다(vfio_pci_dev_set_hot_reset 의
				 * 영어 주석이 그 논리를 설명한다). */
				info->devid = VFIO_PCI_DEVID_OWNED;
			/* [한국어] 그 밖의 오류는 소유하지 않은 것으로 본다. */
			else
				/* [한국어] "소유하지 않음" 을 알린다. */
				info->devid = VFIO_PCI_DEVID_NOT_OWNED;
		}
		/* If devid is VFIO_PCI_DEVID_NOT_OWNED, clear owned flag. */
		/* [한국어] 하나라도 소유하지 않은 장치가 있으면. */
		if (info->devid == VFIO_PCI_DEVID_NOT_OWNED)
			/* [한국어] 헤더의 OWNED 플래그를 지운다. 사용자는 이 플래그가 없으면
			 * "지금 상태로는 hot reset 이 거부된다" 를 알 수 있다. */
			fill->flags &= ~VFIO_PCI_HOT_RESET_FLAG_DEV_ID_OWNED;
	/* [한국어] 1세대(group) 문법. */
	} else {
		/* [한국어] 이 장치가 속한 IOMMU 그룹. */
		struct iommu_group *iommu_group;

		/* [한국어] IOMMU 그룹 참조를 얻는다. 구현은 drivers/iommu 아래라 이 트리에서 확인 못 함. */
		iommu_group = iommu_group_get(&pdev->dev);
		/* [한국어] 그룹이 없다 = IOMMU 로 격리되지 않은 장치다. */
		if (!iommu_group)
			/* [한국어] 격리되지 않은 장치는 소유를 증명할 방법이 없으므로 -EPERM 으로
			 * 순회를 멈춘다. */
			return -EPERM; /* Cannot reset non-isolated devices */

		/* [한국어] 그룹 번호를 사용자에게 알린다. 사용자는 그 번호의 group fd 를 열어
		 * 소유를 증명하게 된다. */
		info->group_id = iommu_group_id(iommu_group);
		/* [한국어] 얻은 참조를 놓는다. */
		iommu_group_put(iommu_group);
	}

	/* [한국어] 이 장치는 정상 처리했으니 순회를 계속한다. */
	return 0;
}

/* [한국어] 1세대(group fd) hot reset 에서 "이 그룹들을 내가 소유한다" 는 증거 묶음.
 * 왜 필요한가: 슬롯/버스 리셋은 범위 안의 모든 함수를 함께 리셋하므로, 호출자가
 * 그 전부를 소유하고 있음을 증명해야 남의 장치를 리셋하는 사고를 막는다.
 * 사용자가 넘긴 group fd 배열을 struct file 배열로 바꾼 것이며, 판정은
 * vfio_dev_in_groups 가 vfio_file_has_dev 로 한 줄씩 확인한다. */
struct vfio_pci_group_info {
	/* [한국어] files 배열의 원소 수.
	 * 설정자: vfio_pci_ioctl_pci_hot_reset_groups 가 info.count = array_count.
	 * 읽는 자: vfio_dev_in_groups 의 순회 상한.
	 * 값 범위: 1 이상, 영향 받는 PCI 함수 개수 이하(그룹 하나에 장치가 여럿일 수
	 * 있으므로 장치 수가 그룹 수의 상한이다).
	 * 동기화: 리셋 한 번 동안만 존재하는 스택 객체라 락이 없다. */
	int count;
	/* [한국어] group fd 를 fget 으로 붙잡아 둔 struct file 포인터 배열.
	 * 설정자: vfio_pci_ioctl_pci_hot_reset_groups 가 fget 으로 채우고 끝나면
	 * fput 으로 모두 놓는다.
	 * 읽는 자: vfio_dev_in_groups 가 vfio_file_has_dev 에 하나씩 넘긴다.
	 * 값 범위: 모두 vfio_file_is_group 검사를 통과한 group fd. 아니면 -EINVAL.
	 * 동기화: 참조를 쥐고 있는 동안 group 이 사라지지 않는 것이 이 배열의 목적이다. */
	struct file **files;
};

/* [한국어]
 * vfio_pci_dev_below_slot - 이 장치가 지정한 슬롯 아래에 있는지 위로 거슬러 확인한다
 *
 * @pdev: 검사할 장치.
 * @slot: 기준 슬롯.
 * @return: true 면 그 슬롯 아래(자신 포함), false 면 아니다.
 *
 * 왜 필요한가: 슬롯 리셋의 영향 범위를 정확히 가려내야 한다. pci_walk_bus 는
 * 버스 아래 전부를 훑으므로, 슬롯 단위로 좁히려면 각 장치가 그 슬롯에
 * 속하는지 따로 판정해야 한다.
 *
 * 동작 과정: 장치에서 시작해 pdev->bus->self(그 버스를 만든 상위 브리지)를
 * 타고 루트 쪽으로 거슬러 올라간다. 어느 단계에서 그 장치의 버스가 슬롯의
 * 버스와 같아지면, 그 장치의 슬롯이 기준 슬롯과 같은지로 답한다.
 * 루트까지 올라가도 못 만나면(pdev 가 NULL 이 되면) false.
 * 슬롯은 "같은 물리 커넥터에 꽂힌 함수들" 이라는 개념이므로, 슬롯의 버스에
 * 닿는 순간이 비교 시점이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_walk_bus 콜백 안에서 불리므로 순회 락
 * 아래다. 브리지 사슬을 따라가지만 참조를 잡지 않는데, 순회 중에는 위상이
 * 고정돼 있다는 전제 위에 있다.
 *
 * 호출자: vfio_pci_walk_wrapper 하나뿐.
 * 호출 대상: 없다. 자료구조 순회만 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_walk_bus → vfio_pci_walk_wrapper → [vfio_pci_dev_below_slot]
 */
static bool vfio_pci_dev_below_slot(struct pci_dev *pdev, struct pci_slot *slot)
{
	/* [한국어] 장치에서 시작해 pdev->bus->self(그 버스를 만든 상위 브리지)를 타고
	 * 루트 쪽으로 거슬러 올라간다. 루트 버스에 닿으면 self 가 NULL 이라 끝난다. */
	for (; pdev; pdev = pdev->bus->self)
		/* [한국어] 지금 단계의 버스가 슬롯의 버스와 같아지는 순간이 비교 시점이다.
		 * 슬롯은 "같은 커넥터에 꽂힌 함수들" 이라 하나의 버스에 속한다. */
		if (pdev->bus == slot->bus)
			/* [한국어] 그 단계의 장치가 바로 그 슬롯에 속하는지로 답한다. */
			return (pdev->slot == slot);
	/* [한국어] 루트까지 올라가도 슬롯의 버스를 못 만났다 — 다른 갈래다. */
	return false;
}

/* [한국어] pci_walk_bus 콜백에 "슬롯 필터" 를 덧씌우기 위한 어댑터 상자.
 * 왜 필요한가: PCI 코어의 pci_walk_bus(drivers/pci/bus.c:1190)는 버스 아래 모든
 * 장치를 훑을 뿐 슬롯 단위로 걸러 주지 않는다. VFIO 는 슬롯 리셋이 가능하면
 * 슬롯 범위만 보고 싶으므로, 진짜 콜백을 이 상자에 넣고 래퍼를 대신 등록한다. */
struct vfio_pci_walk_info {
	/* [한국어] 실제로 부를 콜백. vfio_pci_count_devs, vfio_pci_fill_devs,
	 * vfio_pci_is_device_in_set 셋 중 하나가 들어온다.
	 * 설정자: vfio_pci_for_each_slot_or_bus 가 초기화자로 채운다.
	 * 읽는 자: vfio_pci_walk_wrapper 가 필터를 통과한 장치에 대해 호출한다.
	 * 값 범위: NULL 이 아닌 함수 포인터(세 호출자 모두 항상 채운다).
	 * 동기화: 스택 객체라 락이 없다. */
	int (*fn)(struct pci_dev *pdev, void *data);
	/* [한국어] 위 콜백에 그대로 넘길 사용자 데이터. count 포인터, fill_info,
	 * dev_set 중 하나다.
	 * 설정자와 읽는 자: 위와 같다.
	 * 값 범위: 콜백이 해석할 임의 포인터.
	 * 동기화: 위와 같다. */
	void *data;
	/* [한국어] 순회의 기준이 되는 장치. 슬롯 필터는 이 장치의 슬롯을 기준으로 한다.
	 * 설정자: vfio_pci_for_each_slot_or_bus 의 초기화자.
	 * 읽는 자: vfio_pci_walk_wrapper 가 walk->pdev->slot 을 꺼내
	 * vfio_pci_dev_below_slot 에 넘긴다.
	 * 값 범위: 항상 유효한 pci_dev.
	 * 동기화: 위와 같다. */
	struct pci_dev *pdev;
	/* [한국어] 슬롯 필터를 켤지 여부.
	 * 설정자: 호출자가 pci_probe_reset_slot 결과로 정한 값을 넘긴다.
	 * 읽는 자: vfio_pci_walk_wrapper 의 첫 조건.
	 * 값 범위: true 면 같은 슬롯 아래 장치만, false 면 버스 아래 전부.
	 * 동기화: 위와 같다. */
	bool slot;
	/* [한국어] 콜백이 돌려준 마지막 값을 담아 두는 자리.
	 * 설정자: vfio_pci_walk_wrapper 가 매번 덮어쓴다.
	 * 읽는 자: 순회가 끝난 뒤 vfio_pci_for_each_slot_or_bus 가 이 값을 반환한다.
	 * 값 범위: 0 이면 성공. 0 이 아니면 pci_walk_bus 가 그 자리에서 순회를
	 * 멈추므로(콜백의 반환값이 0 이 아니면 중단) **첫 실패 값이 그대로 남는다**.
	 * 동기화: 위와 같다. */
	int ret;
};

/* [한국어]
 * vfio_pci_walk_wrapper - pci_walk_bus 콜백에 슬롯 필터를 덧씌우는 어댑터
 *
 * @pdev: 지금 방문한 장치.
 * @data: struct vfio_pci_walk_info 포인터.
 * @return: 진짜 콜백이 마지막으로 돌려준 값. 0 이 아니면 pci_walk_bus 가
 *          순회를 멈춘다.
 *
 * 왜 필요한가: PCI 코어의 pci_walk_bus 는 버스 아래 전부를 훑을 뿐 슬롯 단위로
 * 좁히지 못한다. VFIO 는 슬롯 리셋이 가능하면 슬롯 범위만 보고 싶으므로,
 * 필터를 여기서 적용하고 진짜 콜백은 통과한 장치에 대해서만 부른다.
 *
 * 동작 과정:
 *  1. 슬롯 필터가 꺼져 있거나(버스 전체 대상), 이 장치가 기준 슬롯 아래에
 *     있으면 진짜 콜백을 부르고 결과를 walk->ret 에 남긴다.
 *  2. 필터에 걸린 장치는 콜백을 부르지 않고 **walk->ret 도 갱신하지 않는다** —
 *     직전 값이 그대로 남는다. 초기값이 0 이므로 걸러진 장치는 순회를 멈추지
 *     않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. pci_walk_bus 안이라 PCI 버스 세마포어 아래다.
 *
 * 호출자: pci_walk_bus(drivers/pci/bus.c:1190).
 * 호출 대상: vfio_pci_dev_below_slot 과 walk->fn(세 콜백 중 하나).
 *
 * 에러 경로: 콜백의 오류를 그대로 흘려보내 순회를 멈추게 한다.
 *
 * 호출 체인:
 *   vfio_pci_for_each_slot_or_bus → pci_walk_bus → [vfio_pci_walk_wrapper]
 *     → vfio_pci_count_devs / vfio_pci_fill_devs / vfio_pci_is_device_in_set
 */
static int vfio_pci_walk_wrapper(struct pci_dev *pdev, void *data)
{
	/* [한국어] 호출자가 스택에 꾸린 어댑터 상자. */
	struct vfio_pci_walk_info *walk = data;

	/* [한국어] 슬롯 필터가 꺼져 있으면 모두 통과. 켜져 있으면 기준 장치의 슬롯 아래인지 본다. */
	if (!walk->slot || vfio_pci_dev_below_slot(pdev, walk->pdev->slot))
		/* [한국어] 통과한 장치에만 진짜 콜백을 부르고 결과를 상자에 남긴다.
		 * 걸러진 장치는 이 줄을 지나지 않으므로 walk->ret 이 그대로 유지된다. */
		walk->ret = walk->fn(pdev, walk->data);

	/* [한국어] 0 이 아니면 pci_walk_bus 가 순회를 멈춘다. 걸러진 장치 때문에
	 * 직전 결과가 그대로 반환되지만, 초기값이 0 이고 실패는 즉시 멈추므로
	 * 의도한 대로 동작한다. */
	return walk->ret;
}

/* [한국어]
 * vfio_pci_for_each_slot_or_bus - 슬롯 또는 버스 범위의 모든 PCI 함수에 콜백을 돌린다
 *
 * @pdev: 기준 장치. 이 장치의 버스를 순회 시작점으로 삼는다.
 * @fn:   각 장치에 적용할 콜백.
 * @data: 콜백에 그대로 넘길 데이터.
 * @slot: true 면 기준 장치의 슬롯 아래만, false 면 버스 아래 전부.
 * @return: 콜백이 돌려준 마지막(사실상 첫 실패) 값. 전부 성공하면 0.
 *
 * 왜 필요한가: hot reset 의 모든 판단이 "영향 범위 안의 모든 PCI 함수" 를
 * 대상으로 한다. 그 범위를 슬롯/버스 둘 중 하나로 고르고, 어댑터 상자를
 * 꾸리고, pci_walk_bus 를 부르는 세 단계를 한 함수로 감쌌다.
 *
 * 동작 과정: 스택에 walk_info 를 꾸리고 pci_walk_bus 를 부른 뒤 walk.ret 반환.
 * 순회 시작점을 pdev->bus 로 삼는 이유는, 슬롯 필터를 쓰더라도 슬롯은 항상
 * 어떤 버스 아래에 있으므로 그 버스부터 훑으면 빠뜨리지 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자에 따라 dev_set->lock 을 쥔 채 불리기도
 * 한다(vfio_pci_fill_devs 를 쓸 때).
 *
 * 호출자: vfio_pci_ioctl_get_pci_hot_reset_info(두 번 — 세기와 채우기),
 * vfio_pci_ioctl_pci_hot_reset_groups(세기), vfio_pci_dev_set_resettable
 * (집합 소속 검사).
 * 호출 대상: pci_walk_bus(drivers/pci/bus.c:1190).
 *
 * 에러 경로: 콜백의 오류를 그대로 전달한다.
 *
 * 호출 체인:
 *   hot reset 관련 함수 → [vfio_pci_for_each_slot_or_bus] → pci_walk_bus
 *     → vfio_pci_walk_wrapper → 콜백
 */
static int vfio_pci_for_each_slot_or_bus(struct pci_dev *pdev,
					 int (*fn)(struct pci_dev *,
						   void *data), void *data,
					 bool slot)
{
	/* [한국어] 스택에 어댑터 상자를 꾸린다. 지정 초기화자로 다섯 필드를 모두 채운다. */
	struct vfio_pci_walk_info walk = {
		/* [한국어] ret 을 명시적으로 0 으로 두는 것이 중요하다 — 걸러진 장치만 있을 때
		 * 이 값이 그대로 반환되기 때문이다. */
		.fn = fn, .data = data, .pdev = pdev, .slot = slot, .ret = 0,
	};

	/* [한국어] 기준 장치가 속한 버스부터 아래로 훑는다. 슬롯 필터를 쓰더라도 슬롯은
	 * 어떤 버스 아래에 있으므로 이 시작점이면 빠뜨리지 않는다
	 * (drivers/pci/bus.c:1190). */
	pci_walk_bus(pdev->bus, vfio_pci_walk_wrapper, &walk);

	/* [한국어] 콜백이 남긴 마지막 값을 돌려준다. */
	return walk.ret;
}

/* [한국어]
 * msix_mmappable_cap - "이 BAR 는 MSI-X 테이블을 품고 있지만 mmap 해도 된다" 를 알린다
 *
 * @vdev: 대상 디바이스(이 함수에서는 쓰지 않는다).
 * @caps: 사용자에게 돌려줄 capability 사슬. 여기에 항목 하나를 덧붙인다.
 * @return: 0 성공. -ENOMEM 이면 사슬 확장 실패.
 *
 * 왜 필요한가: 옛 VFIO 는 MSI-X 테이블이 든 BAR 의 mmap 을 아예 막거나 그
 * 페이지만 잘라냈다. 사용자가 테이블을 직접 쓰면 장치가 임의 주소로 MSI 를
 * 쏠 수 있었기 때문이다. 인터럽트 재매핑 하드웨어가 그 공격을 막아 주게 된
 * 뒤로는 굳이 막을 이유가 없어졌고, 대신 "이 커널은 막지 않는다" 를 사용자에게
 * 알려 주어야 한다. 그 통보가 VFIO_REGION_INFO_CAP_MSIX_MAPPABLE 이다.
 * **이 파일에서 read/write 와 mmap 의 중재 수준이 갈리는 지점**이기도 하다 —
 * read/write 는 여전히 테이블 구간을 가리지만(vfio_pci_rdwr.c 의 제외 구간)
 * mmap 은 가리지 않는다. 재매핑 하드웨어의 존재 판정 자체는 이 트리에 없는
 * drivers/iommu 쪽이라 여기서는 확인 못 함.
 *
 * 동작 과정: 헤더만 있는(본문 없는) capability 항목을 스택에 만들어
 * vfio_info_add_capability 에 넘긴다. id 와 version 만으로 의미가 완결되는
 * "깃발" 형 capability 다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GET_REGION_INFO ioctl 처리 중.
 *
 * 호출자: vfio_pci_ioctl_get_region_info 의 BAR 분기 — 그 BAR 가 mmap 가능하고
 * 동시에 msix_bar 일 때만.
 * 호출 대상: vfio_info_add_capability(drivers/vfio/vfio_main.c).
 *
 * 에러 경로: 사슬 확장 실패를 그대로 올려 보내 ioctl 을 실패시킨다.
 *
 * 호출 체인:
 *   vfio_pci_core_ioctl → vfio_main.c 의 region info 처리
 *     → vfio_pci_ioctl_get_region_info → [msix_mmappable_cap]
 *       → vfio_info_add_capability
 */
static int msix_mmappable_cap(struct vfio_pci_core_device *vdev,
			      struct vfio_info_cap *caps)
{
	/* [한국어] 본문 없이 헤더만 있는 capability 항목. id 와 version 만으로 의미가
	 * 완결되는 "깃발" 형이다. */
	struct vfio_info_cap_header header = {
		/* [한국어] "이 BAR 는 MSI-X 테이블을 품고 있어도 mmap 할 수 있다" 를 뜻하는 id. */
		.id = VFIO_REGION_INFO_CAP_MSIX_MAPPABLE,
		/* [한국어] 이 capability 의 형식 버전. */
		.version = 1
	};

	/* [한국어] 사슬에 항목을 덧붙인다. 내부에서 krealloc 으로 버퍼를 늘리므로
	 * 실패하면 -ENOMEM 이 나온다(vfio_main.c 의 헬퍼). */
	return vfio_info_add_capability(caps, &header, sizeof(header));
}

/* [한국어]
 * vfio_pci_core_register_dev_region - vendor 드라이버가 자기만의 region 을 하나 덧붙인다
 *
 * @vdev:    대상 디바이스.
 * @type:    region 대분류(VFIO_REGION_TYPE_ 계열).
 * @subtype: 소분류. 둘이 합쳐 사용자에게 "이 region 이 무엇인지" 를 알린다.
 * @ops:     이 region 의 rw/release/mmap/add_capability 콜백 표.
 * @size:    region 크기(바이트).
 * @flags:   VFIO_REGION_INFO_FLAG_ 계열. READ/WRITE/MMAP 허용 여부.
 * @data:    vendor 가 쓸 임의 포인터. ops 콜백이 region->data 로 받는다.
 * @return: 0 성공. -ENOMEM 이면 배열 확장 실패이며 기존 region 들은 그대로다.
 *
 * 왜 필요한가: PCI 표준 region(config, BAR0~5, ROM, VGA)만으로는 표현할 수
 * 없는 것이 있다. 인텔 통합 그래픽의 OpRegion(vfio_pci_igd.c:260, 405, 425),
 * 마이그레이션 상태 채널, 장치별 진단 창 같은 것들이다. 그것들을 같은 파일
 * 오프셋 공간에 **VFIO_PCI_NUM_REGIONS 뒤로 이어 붙이는** 통로가 이 함수다.
 *
 * 동작 과정:
 *  1. 배열을 하나 늘려 krealloc 한다. 실패하면 원래 배열은 유지되므로 그냥
 *     -ENOMEM 만 돌려주면 된다 — krealloc 의 계약이 그렇다.
 *  2. 새 칸에 일곱 개 필드를 채운다.
 *  3. num_regions 를 늘린다. 이 값이 사용자에게 보이는 region 개수와
 *     오프셋 해석의 상한을 동시에 정한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. vendor 의 open_device 안, 즉
 * vfio_pci_core_enable 과 vfio_pci_core_finish_enable 사이에서 불린다.
 * 그 구간에는 사용자가 아직 아무 ioctl 도 못 보내므로 락이 필요 없다.
 *
 * 호출자: vendor 드라이버 — drivers/vfio/pci/vfio_pci_igd.c:260, 405, 425 등.
 * 호출 대상: krealloc.
 *
 * 에러 경로: 할당 실패 하나뿐이며 부작용이 없다. vendor 는 대개 이 실패를
 * open_device 실패로 올려 보낸다.
 *
 * 주의: 여기서 등록한 region 은 vfio_pci_core_disable 이 ops->release 를
 * **NULL 검사 없이** 부른다. 반면 mmap 과 add_capability 슬롯은 호출 전에
 * NULL 검사를 한다. 즉 rw 와 release 는 필수, mmap 과 add_capability 는
 * 선택이라는 규약이 코드의 비대칭으로 표현돼 있다. 이 트리의 두 regops 표
 * (vfio_pci_igd.c:163, 385)는 모두 rw 와 release 를 채운다.
 *
 * 호출 체인:
 *   vendor open_device → [vfio_pci_core_register_dev_region] → krealloc
 */
int vfio_pci_core_register_dev_region(struct vfio_pci_core_device *vdev,
				      unsigned int type, unsigned int subtype,
				      const struct vfio_pci_regops *ops,
				      size_t size, u32 flags, void *data)
{
	/* [한국어] 확장된 배열을 받을 임시 포인터. 실패 시 원본을 잃지 않으려고 따로 받는다. */
	struct vfio_pci_region *region;

	/* [한국어] 배열을 한 칸 늘린다. krealloc 은 실패하면 NULL 을 돌려주고 **원본은
	 * 그대로 둔다** — 그래서 아래에서 그냥 -ENOMEM 만 돌려주면 안전하다. */
	region = krealloc(vdev->region,
			  /* [한국어] 현재 개수 + 1 칸. */
			  (vdev->num_regions + 1) * sizeof(*region),
			  /* [한국어] 사용자 요청으로 생긴 커널 메모리이므로 memcg 에 청구한다. */
			  GFP_KERNEL_ACCOUNT);
	/* [한국어] 확장 실패. */
	if (!region)
		/* [한국어] -ENOMEM. vendor 는 대개 이것을 open_device 실패로 올린다. */
		return -ENOMEM;

	/* [한국어] 확장된 배열을 디바이스에 건다. */
	vdev->region = region;
	/* [한국어] region 대분류. GET_REGION_INFO 의 capability 로 사용자에게 전달된다. */
	vdev->region[vdev->num_regions].type = type;
	/* [한국어] 소분류. type 과 함께 "이 region 이 무엇인지" 를 사용자에게 알린다. */
	vdev->region[vdev->num_regions].subtype = subtype;
	/* [한국어] 이 region 의 콜백 표. rw 와 release 는 필수, mmap 과 add_capability 는 선택이다. */
	vdev->region[vdev->num_regions].ops = ops;
	/* [한국어] region 크기. 사용자가 GET_REGION_INFO 로 받는 값이다. */
	vdev->region[vdev->num_regions].size = size;
	/* [한국어] READ/WRITE/MMAP 허용 플래그. */
	vdev->region[vdev->num_regions].flags = flags;
	/* [한국어] vendor 전용 데이터. 콜백이 region->data 로 받는다. */
	vdev->region[vdev->num_regions].data = data;

	/* [한국어] 개수를 늘린다. 이 값이 사용자에게 보이는 region 개수와 오프셋 해석의
	 * 상한을 동시에 정한다. */
	vdev->num_regions++;

	/* [한국어] 등록 성공. */
	return 0;
}
/* [한국어] vendor 드라이버가 자기 region 을 붙일 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_register_dev_region);

/* [한국어]
 * vfio_pci_info_atomic_cap - PCIe AtomicOp 완료 능력을 조사해 사용자에게 알린다
 *
 * @vdev: 대상 디바이스.
 * @caps: 사용자에게 돌려줄 capability 사슬.
 * @return: 0 성공. -ENODEV 면 지원 폭이 하나도 없어 알릴 것이 없다.
 *          그 밖의 음수는 사슬 확장 실패.
 *
 * 왜 필요한가: PCIe AtomicOp 는 장치가 호스트 메모리에 원자적 갱신(FetchAdd,
 * Swap, CAS)을 요청하는 기능이다. GPU 나 스마트 NIC 이 호스트와 큐를 공유할 때
 * 쓴다. 게스트에게 장치를 넘기려면 **그 경로가 루트 컴플렉스까지 실제로 열려
 * 있는지** 알려 줘야 한다 — 중간 스위치가 지원하지 않으면 요청이 막힌다.
 *
 * 동작 과정:
 *  1. VF 라면 pci_physfn 으로 PF 를 얻는다. AtomicOp 능력은 함수가 아니라
 *     물리 장치와 그 위쪽 경로의 성질이기 때문이다.
 *  2. Device Capabilities 2 레지스터에서 32/64/128비트 완료 지원 비트를 읽는다.
 *  3. 각 폭마다 pci_enable_atomic_ops_to_root(drivers/pci/pci.c:6998)로
 *     **루트까지 실제로 활성화**를 시도한다. 성공한 폭만 사용자에게 알린다.
 *     즉 이 함수는 조회가 아니라 조회 겸 설정이다.
 *  4. 하나도 못 켰으면 -ENODEV. 호출자는 그 값을 "알릴 것이 없다" 로 보고
 *     조용히 넘어간다.
 *
 * 실행 컨텍스트: 프로세스 문맥. GET_INFO ioctl 처리 중.
 *
 * 호출자: vfio_pci_ioctl_get_info.
 * 호출 대상: pci_physfn, pcie_capability_read_dword,
 * pci_enable_atomic_ops_to_root, vfio_info_add_capability.
 *
 * 에러 경로: -ENODEV 는 정상 흐름의 일부다. 그 밖의 오류는 ioctl 실패로
 * 이어진다.
 *
 * 호출 체인:
 *   vfio_pci_core_ioctl → vfio_pci_ioctl_get_info → [vfio_pci_info_atomic_cap]
 *     → pci_enable_atomic_ops_to_root / vfio_info_add_capability
 */
static int vfio_pci_info_atomic_cap(struct vfio_pci_core_device *vdev,
				    struct vfio_info_cap *caps)
{
	/* [한국어] 사용자에게 돌려줄 capability 항목. 헤더에 이어 flags 필드가 있다. */
	struct vfio_device_info_cap_pci_atomic_comp cap = {
		/* [한국어] "이 장치는 PCIe AtomicOp 완료를 지원한다" 를 뜻하는 id. */
		.header.id = VFIO_DEVICE_INFO_CAP_PCI_ATOMIC_COMP,
		/* [한국어] 이 capability 의 형식 버전. */
		.header.version = 1
	};
	/* [한국어] VF 라면 PF 를 얻는다. AtomicOp 능력은 함수 단위가 아니라 물리 장치와
	 * 그 위쪽 경로의 성질이기 때문이다. */
	struct pci_dev *pdev = pci_physfn(vdev->pdev);
	/* [한국어] Device Capabilities 2 레지스터 값을 담을 변수. */
	u32 devcap2;

	/* [한국어] PCIe capability 안의 Device Capabilities 2 를 읽는다. 이 헬퍼는
	 * capability 오프셋 계산과 PCIe 여부 확인을 함께 해 준다. */
	pcie_capability_read_dword(pdev, PCI_EXP_DEVCAP2, &devcap2);

	/* [한국어] 32비트 AtomicOp 완료를 지원한다고 신고하는가. */
	if ((devcap2 & PCI_EXP_DEVCAP2_ATOMIC_COMP32) &&
	    /* [한국어] 루트 컴플렉스까지의 경로를 실제로 열어 본다. 0 이 성공이므로 ! 로 뒤집는다.
	     * 중간 스위치가 막고 있으면 실패한다(drivers/pci/pci.c:6998). */
	    !pci_enable_atomic_ops_to_root(pdev, PCI_EXP_DEVCAP2_ATOMIC_COMP32))
		/* [한국어] 실제로 열린 폭만 사용자에게 알린다. */
		cap.flags |= VFIO_PCI_ATOMIC_COMP32;

	/* [한국어] 64비트 폭. */
	if ((devcap2 & PCI_EXP_DEVCAP2_ATOMIC_COMP64) &&
	    /* [한국어] 같은 방식으로 실제 활성화를 시도한다. */
	    !pci_enable_atomic_ops_to_root(pdev, PCI_EXP_DEVCAP2_ATOMIC_COMP64))
		/* [한국어] 성공한 폭을 기록한다. */
		cap.flags |= VFIO_PCI_ATOMIC_COMP64;

	/* [한국어] 128비트 폭(CAS 전용). */
	if ((devcap2 & PCI_EXP_DEVCAP2_ATOMIC_COMP128) &&
	    /* [한국어] 같은 방식으로 시도한다. */
	    !pci_enable_atomic_ops_to_root(pdev,
					   PCI_EXP_DEVCAP2_ATOMIC_COMP128))
		/* [한국어] 성공한 폭을 기록한다. */
		cap.flags |= VFIO_PCI_ATOMIC_COMP128;

	/* [한국어] 하나도 열지 못했다. */
	if (!cap.flags)
		/* [한국어] -ENODEV — 호출자는 이 값을 "알릴 것이 없다" 로 보고 조용히 넘어간다. */
		return -ENODEV;

	/* [한국어] flags 를 담은 항목을 사슬에 붙인다. 헤더 주소를 넘기지만 크기는 구조체
	 * 전체이므로 flags 까지 함께 복사된다. */
	return vfio_info_add_capability(caps, &cap.header, sizeof(cap));
}

/* [한국어]
 * vfio_pci_ioctl_get_info - VFIO_DEVICE_GET_INFO: 디바이스의 전체 윤곽을 보고한다
 *
 * @vdev: 대상 디바이스.
 * @arg:  사용자 공간의 struct vfio_device_info 포인터. 입출력 겸용이다.
 * @return: 0 성공. -EFAULT(사용자 메모리), -EINVAL(argsz 부족), 그 밖에
 *          capability 조립 실패의 errno.
 *
 * 왜 필요한가: 사용자 공간이 가장 먼저 부르는 ioctl 이다. 여기서 받은
 * num_regions 와 num_irqs 로 이후 GET_REGION_INFO 와 GET_IRQ_INFO 를 몇 번
 * 부를지 정한다. 즉 **탐색의 출발점**이다.
 *
 * 동작 과정:
 *  1. 최소 크기(num_irqs 필드까지)만 먼저 복사해 argsz 를 검증한다.
 *     VFIO 의 모든 ioctl 이 이 "argsz 자기 신고" 관례를 쓴다 — 구조체가
 *     커져도 옛 사용자 공간이 계속 동작하게 하는 장치다.
 *  2. flags 에 VFIO_DEVICE_FLAGS_PCI 를 세우고, 리셋이 되는 장치면
 *     VFIO_DEVICE_FLAGS_RESET 도 더한다. reset_works 는 열기 때
 *     pci_try_reset_function 결과로 정해졌다.
 *  3. region 수는 표준 개수 + vendor 추가분, IRQ 수는 고정 다섯 종류.
 *  4. s390 zPCI capability 와 AtomicOp capability 를 조립한다. 둘 다
 *     -ENODEV(해당 없음)는 정상으로 넘어간다.
 *  5. capability 가 있으면 사용자 버퍼 뒤쪽에 이어 붙인다. 버퍼가 모자라면
 *     필요한 크기만 argsz 에 적어 돌려주어, 사용자가 다시 부르게 한다.
 *     vfio_info_cap_shift 는 사슬 안의 next 오프셋을 사용자 버퍼 기준으로
 *     옮겨 준다.
 *  6. 헤더를 사용자에게 복사한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다.
 *
 * 호출자: vfio_pci_core_ioctl.
 * 호출 대상: copy_from_user, copy_to_user, vfio_pci_info_zdev_add_caps
 * (drivers/vfio/pci/vfio_pci_zdev.c:116), vfio_pci_info_atomic_cap,
 * vfio_info_cap_shift, kfree.
 *
 * 에러 경로: [상류 코드 관찰] 4단계의 두 실패 경로는 caps.buf 를 해제하지 않고
 * 곧장 return ret 한다. zdev capability 는 여러 항목을 차례로 붙이므로
 * (vfio_pci_zdev.c:125~140 이 zpci_base_cap → zpci_group_cap → zpci_util_cap →
 * zpci_pfip_cap 순으로 붙인다) 앞 항목이 성공해 버퍼가 할당된 뒤 뒤 항목이
 * 실패할 수 있고, 그 경우 그 버퍼가 샌다. 아래 5단계의 copy_to_user 실패
 * 경로는 kfree 를 하고 나가므로 두 경로의 처리가 서로 다르다.
 * CONFIG_VFIO_PCI_ZDEV_KVM 이 켜진 s390 에서만 닿는 경로다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_GET_INFO) → vfio_main.c ioctl 디스패처
 *     → vfio_pci_core_ioctl → [vfio_pci_ioctl_get_info]
 */
static int vfio_pci_ioctl_get_info(struct vfio_pci_core_device *vdev,
				   struct vfio_device_info __user *arg)
{
	/* [한국어] 이 ioctl 이 요구하는 최소 구조체 크기. num_irqs 필드의 끝까지다.
	 * 뒤에 필드가 더 생겨도 옛 사용자 공간이 동작하게 하는 관례다. */
	unsigned long minsz = offsetofend(struct vfio_device_info, num_irqs);
	/* [한국어] 커널 쪽 사본. 0 으로 초기화해 안 채운 필드가 쓰레기 값이 되지 않게 한다. */
	struct vfio_device_info info = {};
	/* [한국어] capability 사슬을 담을 상자. 처음에는 비어 있다. */
	struct vfio_info_cap caps = { .buf = NULL, .size = 0 };
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] 최소 크기만 먼저 복사해 argsz 를 확인한다. */
	if (copy_from_user(&info, arg, minsz))
		/* [한국어] -EFAULT — 잘못된 사용자 포인터. */
		return -EFAULT;

	/* [한국어] 사용자가 신고한 크기가 최소보다 작으면 구조체를 해석할 수 없다. */
	if (info.argsz < minsz)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] 이제부터 minsz 는 "사용자에게 실제로 돌려줄 바이트 수" 다. 사용자가
	 * 신고한 크기와 커널 구조체 크기 중 작은 쪽 — 사용자 버퍼를 넘지 않으면서
	 * 커널이 아는 만큼만 준다. */
	minsz = min_t(size_t, info.argsz, sizeof(info));

	/* [한국어] 이 디바이스는 PCI 함수다. */
	info.flags = VFIO_DEVICE_FLAGS_PCI;

	/* [한국어] 리셋이 되는 장치인가. 열기 때 pci_try_reset_function 결과로 정해졌다. */
	if (vdev->reset_works)
		/* [한국어] VFIO_DEVICE_RESET ioctl 을 쓸 수 있다고 알린다. */
		info.flags |= VFIO_DEVICE_FLAGS_RESET;

	/* [한국어] 표준 region(config, BAR0~5, ROM, VGA) 개수에 vendor 추가분을 더한다.
	 * 사용자는 이 수만큼 GET_REGION_INFO 를 돌려 지도를 그린다. */
	info.num_regions = VFIO_PCI_NUM_REGIONS + vdev->num_regions;
	/* [한국어] 인터럽트 종류는 다섯으로 고정이다(INTx, MSI, MSI-X, ERR, REQ). */
	info.num_irqs = VFIO_PCI_NUM_IRQS;

	/* [한국어] s390 zPCI 전용 정보를 사슬에 붙인다. 다른 아키텍처에서는 stub 이
	 * -ENODEV 를 돌려준다(vfio_pci_zdev.c:116). */
	ret = vfio_pci_info_zdev_add_caps(vdev, &caps);
	/* [한국어] -ENODEV 는 "해당 없음" 이라 정상이고, 그 밖의 오류만 실패로 본다. */
	if (ret && ret != -ENODEV) {
		/* [한국어] 관리자에게 알린다. */
		pci_warn(vdev->pdev,
			 "Failed to setup zPCI info capabilities\n");
		/* [한국어] [상류 코드 관찰] 이 경로는 caps.buf 를 해제하지 않는다.
		 * vfio_pci_zdev.c:125~140 은 zpci_base_cap → zpci_group_cap →
		 * zpci_util_cap → zpci_pfip_cap 순으로 항목을 차례로 붙이므로, 앞 항목이
		 * 버퍼를 할당한 뒤 뒤 항목이 실패하면 그 버퍼가 샌다. 아래 3376줄의
		 * copy_to_user 실패 경로는 kfree 를 하고 나간다 — 두 경로의 처리가 다르다.
		 * CONFIG_VFIO_PCI_ZDEV_KVM 이 켜진 s390 에서만 닿는다.
		 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		return ret;
	}

	/* [한국어] AtomicOp 능력을 사슬에 붙인다. */
	ret = vfio_pci_info_atomic_cap(vdev, &caps);
	/* [한국어] 여기도 -ENODEV 는 정상이다. */
	if (ret && ret != -ENODEV) {
		/* [한국어] 관리자에게 알린다. */
		pci_warn(vdev->pdev,
			 "Failed to setup AtomicOps info capability\n");
		/* [한국어] 이 경로도 위와 같은 이유로 caps.buf 를 해제하지 않는다. 앞 단계에서
		 * zdev 항목이 붙어 버퍼가 이미 할당됐을 수 있다. */
		return ret;
	}

	/* [한국어] 붙은 capability 가 하나라도 있으면. */
	if (caps.size) {
		/* [한국어] "뒤에 capability 사슬이 따라온다" 를 사용자에게 알린다. */
		info.flags |= VFIO_DEVICE_FLAGS_CAPS;
		/* [한국어] 사용자 버퍼가 구조체 + 사슬을 담을 만큼 크지 않으면. */
		if (info.argsz < sizeof(info) + caps.size) {
			/* [한국어] 필요한 크기를 argsz 에 적어 돌려준다. 사용자는 버퍼를 키워 다시 부른다.
			 * 이때 cap_offset 은 0 으로 남아 "아직 사슬을 못 받았다" 를 뜻한다. */
			info.argsz = sizeof(info) + caps.size;
		/* [한국어] 버퍼가 충분한 경우. */
		} else {
			/* [한국어] 사슬 안의 next 오프셋들을 사용자 버퍼 기준으로 옮긴다. 커널에서는
			 * 버퍼 시작이 0 이지만 사용자에게는 구조체 뒤부터이므로 그만큼 더한다. */
			vfio_info_cap_shift(&caps, sizeof(info));
			/* [한국어] 구조체 바로 뒤(arg + 1 은 포인터 산술이라 구조체 하나만큼 뒤)에 사슬을 쓴다. */
			if (copy_to_user(arg + 1, caps.buf, caps.size)) {
				/* [한국어] 복사 실패. **여기서는 버퍼를 해제하고 나간다.** */
				kfree(caps.buf);
				/* [한국어] -EFAULT. */
				return -EFAULT;
			}
			/* [한국어] 사슬이 시작되는 오프셋을 알린다. 구조체 크기와 같다. */
			info.cap_offset = sizeof(*arg);
		}

		/* [한국어] 성공 경로에서도 커널 쪽 임시 버퍼는 해제한다. */
		kfree(caps.buf);
	}

	/* [한국어] 헤더를 사용자에게 쓴다. 복사에 실패하면 -EFAULT, 아니면 0. */
	return copy_to_user(arg, &info, minsz) ? -EFAULT : 0;
}

/* [한국어]
 * vfio_pci_ioctl_get_region_info - region 하나의 크기/오프셋/접근 권한을 보고한다
 *
 * @core_vdev: 코어 쪽 디바이스. container_of 로 PCI 객체를 되찾는다.
 * @info:      코어가 이미 사용자에게서 복사해 온 요청/응답 구조체.
 *             index 는 입력, offset/size/flags 는 출력이다.
 * @caps:      이 region 에 붙일 capability 사슬(출력).
 * @return: 0 성공. -EINVAL 은 없는 region 이거나 VGA 가 꺼진 경우.
 *          그 밖은 capability 조립 실패.
 *
 * 왜 필요한가: **이 파일에서 "무엇이 mmap 가능한가" 가 사용자에게 확정되는
 * 지점**이다. 사용자 공간은 여기서 받은 offset 을 mmap 의 오프셋으로,
 * size 를 길이로 쓰고, flags 의 MMAP 비트가 없으면 read/write 로만 접근한다.
 *
 * region 별 처리:
 *  - CONFIG: 크기는 pdev->cfg_size(레거시 256, PCIe 4096). READ|WRITE 만.
 *    **MMAP 비트가 없다** — config 는 반드시 중재를 거쳐야 하므로 매핑을
 *    내주지 않는다. 이 한 줄이 config 보안 경계의 절반이다.
 *  - BAR0~5: 크기는 실제 BAR 길이. 길이가 0 이면 flags 도 0(없는 BAR).
 *    READ|WRITE 는 항상, MMAP 은 bar_mmap_supported 일 때만 붙인다.
 *    그 BAR 가 MSI-X 테이블을 품고 있으면 msix_mmappable_cap 도 덧붙인다.
 *  - ROM: 실제로 읽을 수 있는 ROM 이 있을 때만 READ 를 붙인다. 확인 방법이
 *    특이한데, ROM 을 읽으려면 메모리 디코드가 켜져 있어야 하므로
 *    vfio_pci_memory_lock_and_enable 로 **임시로 켜고** pci_map_rom 을
 *    시도한 뒤 원래 PCI_COMMAND 값을 되돌린다. 보고하는 크기는 ROM 내용의
 *    크기가 아니라 BAR 크기다(사용자 주소 공간 배치를 위해).
 *    섀도 ROM(pdev->rom/romlen)만 있는 경우는 2의 거듭제곱으로 올림해 보고한다.
 *    **쓰기 비트도 mmap 비트도 없다.**
 *  - VGA: has_vga 가 아니면 -EINVAL. 크기는 0xc0000 고정(레거시 VGA 창).
 *    READ|WRITE 만, mmap 없음.
 *  - 그 밖(vendor region): 인덱스 범위를 검사하고 array_index_nospec 으로
 *    투기 실행 방어를 건 뒤, 등록 때 받은 size/flags 를 그대로 보고하고
 *    type/subtype capability 를 붙인다. vendor 가 add_capability 슬롯을
 *    채웠으면 그것도 부른다 — **여기서는 NULL 검사를 한다**.
 *
 * 실행 컨텍스트: 프로세스 문맥. ROM 분기에서만 memory_lock 을 write 로
 * 잠깐 잡았다 놓는다.
 *
 * 호출자: 코어의 GET_REGION_INFO 처리가 ops->get_region_info_caps 로 부른다
 * (vfio_pci.c:135 가 이 함수를 그 슬롯에 등록한다).
 * 호출 대상: pci_resource_len, vfio_pci_memory_lock_and_enable, pci_map_rom
 * (drivers/pci/rom.c:382), pci_unmap_rom(rom.c:475),
 * vfio_pci_memory_unlock_and_restore, msix_mmappable_cap,
 * vfio_info_add_capability, array_index_nospec.
 *
 * 에러 경로: 각 분기의 오류를 그대로 올려 보낸다. ROM 분기는 실패해도
 * PCI_COMMAND 를 반드시 되돌린다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_GET_REGION_INFO) → vfio_main.c 의 공통 처리
 *     → ops->get_region_info_caps = [vfio_pci_ioctl_get_region_info]
 */
int vfio_pci_ioctl_get_region_info(struct vfio_device *core_vdev,
				   struct vfio_region_info *info,
				   struct vfio_info_cap *caps)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] vendor region 첨자와 각 단계의 반환값. */
	int i, ret;

	/* [한국어] 사용자가 물은 region 번호로 나눈다. */
	switch (info->index) {
	/* [한국어] config 공간 region. */
	case VFIO_PCI_CONFIG_REGION_INDEX:
		/* [한국어] 이 region 의 파일 오프셋 시작. region 번호를 40비트 왼쪽으로 밀어 만든다.
		 * 사용자는 이 값을 lseek 이나 pread/pwrite 의 오프셋으로 쓴다. */
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		/* [한국어] 레거시 PCI 는 256바이트, PCIe 는 확장 config 까지 4096바이트다.
		 * PCI 코어가 열거 시점에 정해 둔 값이다. */
		info->size = pdev->cfg_size;
		/* [한국어] 읽기와 쓰기만 허용한다. */
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      /* [한국어] **MMAP 비트를 붙이지 않는다.** config 는 반드시 바이트 단위 권한표를
			       * 거쳐야 하므로 매핑을 내주지 않는다. 이 한 줄이 config 보안 경계의 절반이다. */
			      VFIO_REGION_INFO_FLAG_WRITE;
		break;
	/* [한국어] BAR0 부터 BAR5 까지를 한 case 로 묶는다(GCC 범위 확장). */
	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		/* [한국어] 그 BAR 의 파일 오프셋 시작. */
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		/* [한국어] **호스트가 실제로 배정한** BAR 길이. 사용자가 vconfig 에 써 둔 가짜 값이
		 * 아니다. */
		info->size = pci_resource_len(pdev, info->index);
		/* [한국어] 길이가 0 이면 그 BAR 는 구현되지 않았다. */
		if (!info->size) {
			/* [한국어] 아무 접근도 허용하지 않는다. */
			info->flags = 0;
			/* [한국어] 이 case 를 끝낸다. */
			break;
		}

		/* [한국어] 읽기와 쓰기는 항상 허용한다. read/write 경로는 MSI-X 테이블 구간을
		 * 제외하고 전달한다. */
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      VFIO_REGION_INFO_FLAG_WRITE;
		/* [한국어] vfio_pci_probe_mmaps 가 이미 판정해 둔 결과를 본다. */
		if (vdev->bar_mmap_supported[info->index]) {
			/* [한국어] **mmap 을 허용한다.** 이 비트가 붙은 BAR 만 vfio_pci_core_mmap 을 통과한다. */
			info->flags |= VFIO_REGION_INFO_FLAG_MMAP;
			/* [한국어] 그 BAR 가 MSI-X 테이블을 품고 있으면. */
			if (info->index == vdev->msix_bar) {
				/* [한국어] "테이블이 있어도 mmap 해도 된다" 는 capability 를 붙여 사용자에게 알린다.
				 * read/write 는 그 구간을 가리는데 mmap 은 가리지 않는 비대칭을 명시하는 통보다. */
				ret = msix_mmappable_cap(vdev, caps);
				/* [한국어] 사슬 확장 실패. */
				if (ret)
					/* [한국어] 그대로 올려 보낸다. */
					return ret;
			}
		}

		break;
	/* [한국어] 확장 ROM region. 지역 변수를 두려고 블록을 연다. */
	case VFIO_PCI_ROM_REGION_INDEX: {
		/* [한국어] pci_map_rom 이 돌려줄 커널 가상 주소. */
		void __iomem *io;
		/* [한국어] 그 ROM 의 실제 내용 크기(보고에는 쓰지 않는다). */
		size_t size;
		/* [한국어] 메모리 디코드를 임시로 켜기 전의 PCI_COMMAND 값. */
		u16 cmd;

		/* [한국어] ROM region 의 파일 오프셋 시작. */
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		/* [한국어] 기본은 접근 불가. */
		info->flags = 0;
		/* [한국어] 기본은 크기 0. */
		info->size = 0;

		/* [한국어] ROM BAR 가 실제로 배정돼 있는가. 없으면 아래 섀도 ROM 경로로 간다. */
		if (pci_resource_start(pdev, PCI_ROM_RESOURCE)) {
			/*
			 * Check ROM content is valid. Need to enable memory
			 * decode for ROM access in pci_map_rom().
			 */
			/* [한국어] **ROM 을 읽으려면 메모리 디코드가 켜져 있어야 한다.** 사용자가 꺼 두었을
			 * 수 있으므로 임시로 켜고 원래 값을 받아 둔다. 이 호출이 memory_lock 을
			 * write 로 잡으므로 그 동안 사용자의 BAR 접근이 모두 막힌다. */
			cmd = vfio_pci_memory_lock_and_enable(vdev);
			/* [한국어] ROM 을 매핑하고 유효성(0x55AA 서명 등)을 확인한다
			 * (drivers/pci/rom.c:382). 내용이 유효하지 않으면 NULL 이 나온다. */
			io = pci_map_rom(pdev, &size);
			/* [한국어] 유효한 ROM 이 있으면. */
			if (io) {
				/* [한국어] **읽기만** 허용한다. 쓰기도 mmap 도 주지 않는다. */
				info->flags = VFIO_REGION_INFO_FLAG_READ;
				/* Report the BAR size, not the ROM size. */
				/* [한국어] 원본 주석대로 ROM 내용 크기가 아니라 **BAR 크기**를 보고한다.
				 * 사용자 주소 공간에서 그 region 이 차지하는 자리는 BAR 크기이기 때문이다.
				 * 실제 ROM 뒤쪽 빈 공간은 read 에서 0xFF 로 채워진다. */
				info->size = pci_resource_len(pdev,
							      PCI_ROM_RESOURCE);
				/* [한국어] 매핑을 해제한다(drivers/pci/rom.c:475). */
				pci_unmap_rom(pdev, io);
			}
			/* [한국어] PCI_COMMAND 를 원래대로 되돌리고 memory_lock 을 놓는다.
			 * 사용자가 보는 디코드 상태가 이 조회 때문에 달라지면 안 된다. */
			vfio_pci_memory_unlock_and_restore(vdev, cmd);
		/* [한국어] BAR 는 없지만 펌웨어가 남긴 섀도 ROM 사본이 있는 경우. */
		} else if (pdev->rom && pdev->romlen) {
			/* [한국어] 읽기만 허용한다. */
			info->flags = VFIO_REGION_INFO_FLAG_READ;
			/* Report BAR size as power of two. */
			/* [한국어] 원본 주석대로 2 의 거듭제곱으로 올림해 보고한다. BAR 크기는 언제나
			 * 2 의 거듭제곱이므로 사용자에게 그렇게 보이는 편이 자연스럽다. */
			info->size = roundup_pow_of_two(pdev->romlen);
		}

		/* [한국어] ROM case 를 끝낸다. */
		break;
	}
	/* [한국어] 레거시 VGA region. */
	case VFIO_PCI_VGA_REGION_INDEX:
		/* [한국어] VGA 지원이 꺼졌거나 VGA 장치가 아니면. */
		if (!vdev->has_vga)
			/* [한국어] -EINVAL — 그런 region 은 없다. */
			return -EINVAL;

		/* [한국어] VGA region 의 파일 오프셋 시작. */
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		/* [한국어] 0xc0000 은 레거시 VGA 창의 크기다. 0xA0000~0xBFFFF 의 프레임버퍼와
		 * I/O 포트 창을 한 오프셋 공간에 겹쳐 담는 관례적 크기다. */
		info->size = 0xc0000;
		/* [한국어] 읽기와 쓰기만 허용한다. */
		info->flags = VFIO_REGION_INFO_FLAG_READ |
			      /* [한국어] **mmap 비트가 없다.** 레거시 VGA 접근은 I/O 포트를 섞어 쓰므로
			       * 단순 메모리 매핑으로 표현할 수 없다. */
			      VFIO_REGION_INFO_FLAG_WRITE;

		/* [한국어] VGA case 를 끝낸다. */
		break;
	/* [한국어] vendor 가 등록한 추가 region. 지역 변수를 두려고 블록을 연다. */
	default: {
		/* [한국어] 이 region 의 type/subtype 을 알릴 capability 항목. */
		struct vfio_region_info_cap_type cap_type = {
			/* [한국어] "이 region 에는 type/subtype 이 있다" 를 뜻하는 id. */
			.header.id = VFIO_REGION_INFO_CAP_TYPE,
			/* [한국어] 이 capability 의 형식 버전. */
			.header.version = 1
		};

		/* [한국어] 사용자가 준 인덱스가 범위를 넘는가. */
		if (info->index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
			/* [한국어] -EINVAL. */
			return -EINVAL;
		/* [한국어] **Spectre v1 방어.** 위 경계 검사의 분기 예측이 틀려도 투기 실행이
		 * 배열 밖을 읽지 못하도록, 범위를 넘으면 0 이 되는 마스크를 씌운다.
		 * 사용자가 제어하는 값을 배열 첨자로 쓰기 직전에 반드시 통과시킨다. */
		info->index = array_index_nospec(
			info->index, VFIO_PCI_NUM_REGIONS + vdev->num_regions);

		/* [한국어] 표준 region 개수를 빼서 vendor 배열 안의 첨자로 바꾼다. */
		i = info->index - VFIO_PCI_NUM_REGIONS;

		/* [한국어] 그 region 의 파일 오프셋 시작. */
		info->offset = VFIO_PCI_INDEX_TO_OFFSET(info->index);
		/* [한국어] 등록 때 받은 크기를 그대로 보고한다. */
		info->size = vdev->region[i].size;
		/* [한국어] 등록 때 받은 플래그를 그대로 보고한다. mmap 허용 여부도 vendor 가 정한다. */
		info->flags = vdev->region[i].flags;

		/* [한국어] 대분류를 capability 에 담는다. */
		cap_type.type = vdev->region[i].type;
		/* [한국어] 소분류를 담는다. 사용자는 이 둘로 region 의 정체를 안다. */
		cap_type.subtype = vdev->region[i].subtype;

		/* [한국어] type capability 를 사슬에 붙인다. */
		ret = vfio_info_add_capability(caps, &cap_type.header,
					       sizeof(cap_type));
		/* [한국어] 사슬 확장 실패. */
		if (ret)
			/* [한국어] 그대로 올려 보낸다. */
			return ret;

		/* [한국어] **여기서는 슬롯 NULL 검사를 한다.** add_capability 는 선택 슬롯이라는
		 * 규약이 이 한 줄에 담겨 있다(vfio_pci_core_disable 의 release 호출과 대비). */
		if (vdev->region[i].ops->add_capability) {
			/* [한국어] vendor 가 자기만의 capability 를 더 붙일 기회를 준다. */
			ret = vdev->region[i].ops->add_capability(
				vdev, &vdev->region[i], caps);
			/* [한국어] vendor 쪽 실패. */
			if (ret)
				/* [한국어] 그대로 올려 보낸다. */
				return ret;
		}
	}
	}
	/* [한국어] 여기까지 오면 info 와 caps 가 채워졌다. 코어가 사용자에게 복사한다. */
	return 0;
}
/* [한국어] vendor 가 자기 get_region_info_caps 안에서 부를 수 있게 내보낸다
 * (drivers/vfio/pci/virtio/legacy_io.c:291 이 그렇게 쓴다). */
EXPORT_SYMBOL_GPL(vfio_pci_ioctl_get_region_info);

/* [한국어]
 * vfio_pci_ioctl_get_irq_info - VFIO_DEVICE_GET_IRQ_INFO: 인터럽트 종류별 능력을 보고한다
 *
 * @vdev: 대상 디바이스.
 * @arg:  사용자 공간의 struct vfio_irq_info 포인터.
 * @return: 0 성공. -EFAULT, 또는 -EINVAL(argsz 부족, 인덱스 범위 초과,
 *          PCIe 가 아닌데 ERR 을 물음).
 *
 * 왜 필요한가: 사용자는 이 정보로 어떤 인터럽트 방식을 쓸지 고른다. 특히
 * count(최대 벡터 수)와 세 플래그가 핵심이다.
 *
 * 동작 과정:
 *  1. 최소 크기 복사 후 argsz 와 인덱스 범위를 검사한다.
 *  2. 종류별 유효성: INTx/MSI/MSI-X 와 REQ 는 항상 유효. ERR 은 PCIe 일 때만
 *     유효하고, 아니면 fallthrough 로 default 에 떨어져 -EINVAL 이 된다.
 *  3. 모든 종류에 VFIO_IRQ_INFO_EVENTFD 를 세운다 — VFIO 의 인터럽트 전달
 *     수단은 언제나 eventfd 하나뿐이다.
 *  4. count 는 vfio_pci_get_irq_count 가 센다.
 *  5. INTx 에는 MASKABLE 과 AUTOMASKED 를 붙인다. 레벨 트리거라 핸들러가
 *     자동으로 마스크하고 사용자가 처리 후 언마스크해야 하기 때문이다.
 *  6. 나머지 종류에는 원칙적으로 NORESIZE 를 붙이되, **MSI-X 이면서
 *     has_dyn_msix 인 경우만 예외**로 붙이지 않는다. 즉 "벡터 수를 나중에
 *     늘릴 수 있는가" 를 여기서 알린다. 그 능력의 근거는 열기 때 물어 둔
 *     pci_msix_can_alloc_dyn(drivers/pci/msi/api.c:287)이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡지 않는다.
 *
 * 호출자: vfio_pci_core_ioctl.
 * 호출 대상: copy_from_user, vfio_pci_get_irq_count, copy_to_user.
 *
 * 에러 경로: 모두 즉시 반환이며 부작용이 없다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_GET_IRQ_INFO) → vfio_pci_core_ioctl
 *     → [vfio_pci_ioctl_get_irq_info]
 */
static int vfio_pci_ioctl_get_irq_info(struct vfio_pci_core_device *vdev,
				       struct vfio_irq_info __user *arg)
{
	/* [한국어] 이 ioctl 이 요구하는 최소 구조체 크기(count 필드의 끝까지). */
	unsigned long minsz = offsetofend(struct vfio_irq_info, count);
	/* [한국어] 커널 쪽 사본. */
	struct vfio_irq_info info;

	/* [한국어] 최소 크기만 복사한다. */
	if (copy_from_user(&info, arg, minsz))
		/* [한국어] -EFAULT. */
		return -EFAULT;

	/* [한국어] 신고 크기가 부족하거나 인덱스가 다섯 종류를 벗어나면 거절한다. */
	if (info.argsz < minsz || info.index >= VFIO_PCI_NUM_IRQS)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] 종류별 유효성 검사. */
	switch (info.index) {
	/* [한국어] INTx, MSI, MSI-X 세 인덱스가 연속이라 범위로 묶는다. */
	case VFIO_PCI_INTX_IRQ_INDEX ... VFIO_PCI_MSIX_IRQ_INDEX:
	/* [한국어] REQ 는 모든 장치가 갖는다. */
	case VFIO_PCI_REQ_IRQ_INDEX:
		/* [한국어] 유효하므로 아래로 진행한다. */
		break;
	/* [한국어] AER 통보 채널. */
	case VFIO_PCI_ERR_IRQ_INDEX:
		/* [한국어] PCIe 장치만 AER 를 갖는다. */
		if (pci_is_pcie(vdev->pdev))
			/* [한국어] 유효하므로 아래로 진행한다. */
			break;
		/* [한국어] PCIe 가 아니면 default 로 흘러내린다. fallthrough 로 의도적임을
		 * 컴파일러에 알려 경고를 막는다. */
		fallthrough;
	/* [한국어] 위 어느 것도 아닌 인덱스(사실상 도달 불가 — 위에서 범위를 이미 검사했다). */
	default:
		/* [한국어] -EINVAL. */
		return -EINVAL;
	}

	/* [한국어] 모든 종류가 eventfd 로 전달된다. VFIO 에는 다른 전달 수단이 없다. */
	info.flags = VFIO_IRQ_INFO_EVENTFD;

	/* [한국어] 그 종류의 최대 벡터 수를 센다. */
	info.count = vfio_pci_get_irq_count(vdev, info.index);

	/* [한국어] INTx 인 경우. */
	if (info.index == VFIO_PCI_INTX_IRQ_INDEX)
		/* [한국어] MASKABLE — 사용자가 마스크/언마스크를 요청할 수 있다. */
		info.flags |=
			/* [한국어] AUTOMASKED — INTx 는 레벨 트리거라 커널 핸들러가 인터럽트를 받으면
			 * 자동으로 마스크하고, 사용자가 처리 후 언마스크해야 다시 흐른다.
			 * 그 규약을 사용자에게 알린다. */
			(VFIO_IRQ_INFO_MASKABLE | VFIO_IRQ_INFO_AUTOMASKED);
	/* [한국어] MSI-X 가 아니거나, MSI-X 라도 동적 추가가 불가능한 경우. */
	else if (info.index != VFIO_PCI_MSIX_IRQ_INDEX || !vdev->has_dyn_msix)
		/* [한국어] NORESIZE — "한 번 정한 벡터 수를 나중에 바꿀 수 없다". 뒤집어 말하면
		 * MSI-X 이면서 has_dyn_msix 인 경우에만 이 비트가 없어, 사용자가 벡터를
		 * 나중에 더 요청할 수 있음을 알린다(근거는 pci_msix_can_alloc_dyn). */
		info.flags |= VFIO_IRQ_INFO_NORESIZE;

	/* [한국어] 결과를 사용자에게 복사한다. */
	return copy_to_user(arg, &info, minsz) ? -EFAULT : 0;
}

/* [한국어]
 * vfio_pci_ioctl_set_irqs - VFIO_DEVICE_SET_IRQS: 인터럽트를 eventfd 에 연결하거나 끊는다
 *
 * @vdev: 대상 디바이스.
 * @arg:  사용자 공간의 struct vfio_irq_set 포인터. 뒤에 가변 길이 데이터가 따른다.
 * @return: 0 성공. -EFAULT(사용자 메모리), memdup_user 의 -ENOMEM,
 *          검증 함수의 오류, 또는 실제 설정 함수의 오류.
 *
 * 왜 필요한가: **사용자가 인터럽트에 관해 할 수 있는 유일한 조작**이다.
 * 사용자는 벡터를 만들 수 없고, "몇 번 벡터를 이 eventfd 에 걸어 달라" 거나
 * "마스크/언마스크 해 달라" 고 요청할 뿐이다. 실제 벡터 할당과 request_irq 는
 * 전부 커널이 한다(drivers/vfio/pci/vfio_pci_intrs.c).
 *
 * 동작 과정:
 *  1. 헤더를 복사한다.
 *  2. 그 종류의 최대 개수를 vfio_pci_get_irq_count 로 구한다.
 *  3. vfio_set_irqs_validate_and_prepare(drivers/vfio/vfio_main.c)에 넘겨
 *     flags 조합, start/count 범위, 그리고 뒤따르는 데이터 크기를 검증한다.
 *     이 검증이 사용자 입력을 신뢰 가능한 값으로 바꾸는 지점이다.
 *  4. 데이터가 있으면 memdup_user 로 커널에 복사한다. 배열 원소는 요청에 따라
 *     eventfd 번호(int32) 또는 마스크 바이트다.
 *  5. **igate mutex** 를 잡고 vfio_pci_set_irqs_ioctl(vfio_pci_intrs.c:825)에
 *     넘긴다. 이 락이 인터럽트 설정 전체를 직렬화한다 — 두 스레드가 동시에
 *     MSI 를 켜고 끄면 벡터 상태가 깨진다.
 *  6. 락을 풀고 임시 버퍼를 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. igate 를 잡으므로 잠들 수 있다.
 *
 * 호출자: vfio_pci_core_ioctl.
 * 호출 대상: copy_from_user, vfio_pci_get_irq_count,
 * vfio_set_irqs_validate_and_prepare, memdup_user,
 * vfio_pci_set_irqs_ioctl(vfio_pci_intrs.c:825), kfree.
 *
 * 에러 경로: 4단계까지의 실패는 자원을 잡기 전이라 그냥 반환한다. 5단계
 * 이후는 성공/실패에 관계없이 락 해제와 kfree 를 반드시 지난다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_SET_IRQS) → vfio_pci_core_ioctl
 *     → [vfio_pci_ioctl_set_irqs] → vfio_pci_intrs.c:vfio_pci_set_irqs_ioctl
 *       → pci_alloc_irq_vectors(drivers/pci/msi/api.c:519) / request_irq
 */
static int vfio_pci_ioctl_set_irqs(struct vfio_pci_core_device *vdev,
				   struct vfio_irq_set __user *arg)
{
	/* [한국어] 이 ioctl 이 요구하는 최소 구조체 크기(count 필드의 끝까지).
	 * 뒤에는 가변 길이 data 배열이 따른다. */
	unsigned long minsz = offsetofend(struct vfio_irq_set, count);
	/* [한국어] 커널 쪽 헤더 사본. */
	struct vfio_irq_set hdr;
	/* [한국어] 뒤따르는 가변 데이터의 커널 쪽 사본. 없을 수도 있다. */
	u8 *data = NULL;
	/* [한국어] 그 종류의 최대 개수와 반환값. */
	int max, ret = 0;
	/* [한국어] 가변 데이터의 바이트 수. 검증 함수가 채워 준다. */
	size_t data_size = 0;

	/* [한국어] 헤더만 먼저 복사한다. */
	if (copy_from_user(&hdr, arg, minsz))
		/* [한국어] -EFAULT. */
		return -EFAULT;

	/* [한국어] 요청한 종류의 최대 벡터 수. 아래 검증의 상한이 된다. */
	max = vfio_pci_get_irq_count(vdev, hdr.index);

	/* [한국어] flags 조합, index/start/count 범위, 그리고 뒤따르는 데이터 크기를 검증한다.
	 * **사용자 입력을 신뢰 가능한 값으로 바꾸는 지점**이며, 구현은 코어에 있다. */
	ret = vfio_set_irqs_validate_and_prepare(&hdr, max, VFIO_PCI_NUM_IRQS,
						 /* [한국어] 검증이 계산한 데이터 크기를 받는다. */
						 &data_size);
	/* [한국어] 검증 실패. */
	if (ret)
		/* [한국어] 그대로 사용자에게 돌려준다. */
		return ret;

	/* [한국어] 데이터가 따라오는 요청인가. DATA_EVENTFD 면 int32 배열,
	 * DATA_BOOL 이면 바이트 배열이다. */
	if (data_size) {
		/* [한국어] 사용자 데이터를 커널로 복사한다. memdup_user 는 할당과 복사를 함께 하고
		 * 실패하면 오류 포인터를 돌려준다. arg->data 는 헤더 뒤의 가변 부분이다. */
		data = memdup_user(&arg->data, data_size);
		/* [한국어] 할당 또는 복사 실패. */
		if (IS_ERR(data))
			/* [한국어] 포인터에 인코딩된 errno 를 꺼내 돌려준다. */
			return PTR_ERR(data);
	}

	/* [한국어] **인터럽트 설정 전체를 직렬화하는 락.** 두 스레드가 동시에 MSI 를 켜고
	 * 끄면 벡터 상태가 깨진다. */
	mutex_lock(&vdev->igate);

	/* [한국어] 실제 설정은 형제 파일이 한다(vfio_pci_intrs.c:825). 거기서
	 * pci_alloc_irq_vectors 와 request_irq 로 진짜 벡터가 만들어진다. */
	ret = vfio_pci_set_irqs_ioctl(vdev, hdr.flags, hdr.index, hdr.start,
				      /* [한국어] 사용자가 요청한 범위와 데이터를 그대로 넘긴다. 이미 검증을 거쳤다. */
				      hdr.count, data);

	/* [한국어] 락을 놓는다. */
	mutex_unlock(&vdev->igate);
	/* [한국어] 임시 데이터 버퍼를 해제한다. NULL 이어도 안전하다. */
	kfree(data);

	/* [한국어] 설정 결과를 사용자에게 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_ioctl_reset - VFIO_DEVICE_RESET: 이 함수 하나만 리셋한다
 *
 * @vdev: 대상 디바이스.
 * @arg:  쓰지 않는다(이 ioctl 은 인자가 없다).
 * @return: 0 성공. -EINVAL 이면 리셋을 지원하지 않는 장치.
 *          그 밖은 pci_try_reset_function 의 반환값.
 *
 * 왜 필요한가: 게스트가 장치를 리셋하고 싶을 때(드라이버 재초기화, 게스트
 * 재부팅) 그 요청을 호스트로 전달하는 통로다. **순서가 곧 안전이다.**
 *
 * 동작 과정:
 *  1. reset_works 가 아니면 -EINVAL. 이 값은 열기 때 정해졌다.
 *  2. **vfio_pci_zap_and_down_write_memory_lock** — 사용자의 BAR 매핑을 전부
 *     걷어내고 memory_lock 을 write 로 잡는다. 리셋 중인 장치의 MMIO 를
 *     사용자가 만지면 호스트가 마스터 어보트를 맞거나 정의되지 않은 값을
 *     읽는다. 매핑 회수가 먼저이고 그 다음이 리셋이라는 순서가 핵심이다.
 *  3. D0 로 올린다. 원본 주석대로 pci_try_reset_function 은 비 D0 에서
 *     불려도 내부적으로 D0 로 올리는데, 그러면 NoSoftRst- 장치의 pm_save
 *     복원이 건너뛰어진다. 그래서 VFIO 가 먼저 올려 자기 복원 로직을 태운다.
 *  4. dma-buf 를 revoke 한다 — 다른 장치가 이 BAR 로 P2P DMA 를 하고 있을 수
 *     있다.
 *  5. pci_try_reset_function(drivers/pci/pci.c:10347)으로 리셋한다.
 *  6. 메모리 디코드가 켜져 있으면 dma-buf 를 되살린다. 리셋으로 PCI_COMMAND
 *     가 초기화되어 꺼졌을 수도 있으므로 조건부다.
 *  7. 락을 놓는다. **열린 fd 는 그대로 살아 있고**, 사용자의 다음 BAR 접근은
 *     페이지 폴트를 맞아 vfio_pci_mmap_huge_fault 가 다시 매핑해 준다.
 *     vconfig 그림자는 이 경로에서 손대지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. memory_lock 을 write 로 쥔 구간이 길다 —
 * 그 동안 사용자의 모든 BAR read/write 와 폴트가 막힌다. 그것이 의도다.
 *
 * 호출자: vfio_pci_core_ioctl.
 * 호출 대상: vfio_pci_zap_and_down_write_memory_lock, vfio_pci_set_power_state,
 * vfio_pci_dma_buf_move, pci_try_reset_function, __vfio_pci_memory_enabled.
 *
 * 에러 경로: 리셋이 실패해도 락과 dma-buf 상태는 정상으로 되돌린다.
 * 사용자는 오류 코드를 받고 장치는 리셋 전 상태로 남는다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_RESET) → vfio_pci_core_ioctl
 *     → [vfio_pci_ioctl_reset] → pci_try_reset_function(drivers/pci/pci.c:10347)
 */
static int vfio_pci_ioctl_reset(struct vfio_pci_core_device *vdev,
				void __user *arg)
{
	/* [한국어] 리셋 결과. */
	int ret;

	/* [한국어] 열기 때 리셋이 안 되는 장치로 판정됐다. */
	if (!vdev->reset_works)
		/* [한국어] -EINVAL. 사용자는 GET_INFO 의 RESET 플래그로 미리 알 수 있다. */
		return -EINVAL;

	/* [한국어] **사용자 BAR 매핑을 전부 걷어내고 memory_lock 을 write 로 잡는다.**
	 * 리셋 중인 장치의 MMIO 를 사용자가 만지면 호스트가 마스터 어보트를
	 * 맞거나 정의되지 않은 값을 읽는다. 매핑 회수가 리셋보다 먼저라는
	 * 이 순서가 안전의 핵심이다. */
	vfio_pci_zap_and_down_write_memory_lock(vdev);

	/*
	 * This function can be invoked while the power state is non-D0. If
	 * pci_try_reset_function() has been called while the power state is
	 * non-D0, then pci_try_reset_function() will internally set the power
	 * state to D0 without vfio driver involvement. For the devices which
	 * have NoSoftRst-, the reset function can cause the PCI config space
	 * reset without restoring the original state (saved locally in
	 * 'vdev->pm_save').
	 */
	/* [한국어] 먼저 D0 로 올린다. 원본 주석대로 pci_try_reset_function 이 스스로
	 * D0 로 올리면 NoSoftRst- 장치의 pm_save 복원이 건너뛰어진다. */
	vfio_pci_set_power_state(vdev, PCI_D0);

	/* [한국어] 이 BAR 를 P2P DMA 대상으로 빌려 준 dma-buf 를 무효화한다(revoked=true).
	 * 리셋 중인 BAR 로 다른 장치가 DMA 하면 안 된다. */
	vfio_pci_dma_buf_move(vdev, true);
	/* [한국어] 함수 단위 리셋을 시도한다(drivers/pci/pci.c:10347). */
	ret = pci_try_reset_function(vdev->pdev);
	/* [한국어] 리셋으로 PCI_COMMAND 가 초기화되어 메모리 디코드가 꺼졌을 수 있으므로
	 * 확인한다(vfio_pci_config.c:404). */
	if (__vfio_pci_memory_enabled(vdev))
		/* [한국어] 켜져 있을 때만 dma-buf 를 되살린다. */
		vfio_pci_dma_buf_move(vdev, false);
	/* [한국어] 락을 놓는다. **열린 fd 는 그대로 살아 있고**, 사용자의 다음 BAR 접근은
	 * 폴트를 맞아 vfio_pci_mmap_huge_fault 가 다시 매핑해 준다. */
	up_write(&vdev->memory_lock);

	/* [한국어] 리셋 결과를 사용자에게 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_ioctl_get_pci_hot_reset_info - hot reset 의 영향 범위를 사용자에게 알린다
 *
 * @vdev: 대상 디바이스.
 * @arg:  사용자 공간의 struct vfio_pci_hot_reset_info 포인터. 뒤에 가변 길이
 *        devices 배열이 따른다.
 * @return: 0 성공. -EFAULT, -EINVAL(argsz 부족), -ENODEV(슬롯도 버스도 리셋
 *          불가), -ERANGE(장치를 하나도 못 셈), -ENOSPC(버퍼 부족 — 이때
 *          hdr.count 에 필요한 개수를 담아 돌려준다), -ENOMEM, -EAGAIN, -EPERM.
 *
 * 왜 필요한가: 슬롯/버스 리셋은 범위 안의 모든 함수를 함께 리셋한다. 사용자가
 * 그 사실을 모르고 리셋하면 남의 장치를 망가뜨린다. 그래서 실제 리셋 요청 전에
 * **"함께 죽는 장치들이 이것들이다"** 를 반드시 조회하게 만든다.
 *
 * 동작 과정:
 *  1. 헤더 복사와 argsz 검증. flags 는 출력 전용이므로 0 으로 초기화한다.
 *  2. 리셋 범위 결정: pci_probe_reset_slot 이 0(가능)이면 슬롯 범위,
 *     아니면 버스 범위. 버스 리셋도 불가능하면 -ENODEV — 이 장치는 hot reset
 *     자체가 안 된다.
 *  3. vfio_pci_count_devs 로 범위 안 장치 수를 센다. 0 이면 자기 자신도
 *     못 셌다는 뜻이라 WARN 후 -ERANGE.
 *  4. 사용자 버퍼가 그만큼을 못 담으면 개수만 알려 주고 -ENOSPC. 사용자는
 *     버퍼를 키워 다시 부른다.
 *  5. 커널 임시 배열을 잡고, cdev 로 열린 디바이스면 2세대(devid) 문법
 *     플래그를 세운다.
 *  6. **dev_set->lock 을 잡고** vfio_pci_fill_devs 로 채운다. 그 콜백이
 *     dev_set 안을 뒤지기 때문이다.
 *  7. 배열과 헤더를 사용자에게 복사한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock 을 순회 동안만 짧게 잡는다.
 *
 * 호출자: vfio_pci_core_ioctl.
 * 호출 대상: pci_probe_reset_slot(drivers/pci/pci.c:11012),
 * pci_probe_reset_bus(pci.c:11378) 와 vfio_pci_for_each_slot_or_bus,
 * vfio_device_cdev_opened(drivers/vfio/vfio_main.c), copy_to_user, kfree.
 *
 * 에러 경로: header 와 out 두 라벨이 겹쳐 있다. -ENOSPC 는 header 로 뛰어
 * 헤더만 돌려주고, 그 밖의 실패는 out 으로 가 배열을 해제한다. devices 가
 * NULL 인 채로 out 에 닿아도 kfree(NULL) 은 안전하다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_GET_PCI_HOT_RESET_INFO) → vfio_pci_core_ioctl
 *     → [vfio_pci_ioctl_get_pci_hot_reset_info] → vfio_pci_for_each_slot_or_bus
 *       → vfio_pci_fill_devs
 */
static int vfio_pci_ioctl_get_pci_hot_reset_info(
	struct vfio_pci_core_device *vdev,
	struct vfio_pci_hot_reset_info __user *arg)
{
	/* [한국어] 이 ioctl 이 요구하는 최소 구조체 크기(count 필드의 끝까지).
	 * 뒤에는 가변 길이 devices 배열이 따른다. */
	unsigned long minsz =
		offsetofend(struct vfio_pci_hot_reset_info, count);
	/* [한국어] 커널 쪽 임시 배열. NULL 로 시작해 out 라벨의 kfree 가 안전하게 한다. */
	struct vfio_pci_dependent_device *devices = NULL;
	/* [한국어] 커널 쪽 헤더 사본. */
	struct vfio_pci_hot_reset_info hdr;
	/* [한국어] 순회 콜백에 넘길 상태 상자. 0 초기화로 시작한다. */
	struct vfio_pci_fill_info fill = {};
	/* [한국어] 슬롯 범위인지 버스 범위인지. */
	bool slot = false;
	/* [한국어] 반환값과 장치 수. */
	int ret, count = 0;

	/* [한국어] 헤더만 먼저 복사한다. */
	if (copy_from_user(&hdr, arg, minsz))
		/* [한국어] -EFAULT. */
		return -EFAULT;

	/* [한국어] 신고 크기가 부족하면 구조체를 해석할 수 없다. */
	if (hdr.argsz < minsz)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] flags 는 출력 전용이므로 사용자가 무엇을 넣었든 0 으로 시작한다. */
	hdr.flags = 0;

	/* Can we do a slot or bus reset or neither? */
	/* [한국어] 슬롯 리셋이 가능한지 조사한다. 0 이 "가능" 이므로 ! 로 뒤집는다
	 * (drivers/pci/pci.c:11012). */
	if (!pci_probe_reset_slot(vdev->pdev->slot))
		/* [한국어] 슬롯 범위로 좁힌다 — 영향 범위가 작은 쪽을 먼저 쓴다. */
		slot = true;
	/* [한국어] 슬롯이 안 되면 버스 리셋을 조사한다. 0 이 아니면 그것도 안 된다
	 * (drivers/pci/pci.c:11378). */
	else if (pci_probe_reset_bus(vdev->pdev->bus))
		/* [한국어] -ENODEV — 이 장치는 hot reset 자체가 불가능하다. */
		return -ENODEV;

	/* [한국어] 범위 안의 장치 수를 센다. */
	ret = vfio_pci_for_each_slot_or_bus(vdev->pdev, vfio_pci_count_devs,
					    /* [한국어] 카운터 주소를 콜백 데이터로 넘긴다. */
					    &count, slot);
	/* [한국어] 순회 중 오류. */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. */
		return ret;

	/* [한국어] 자기 자신도 못 셌다면 순회 자체가 잘못된 것이다. 원본 주석대로
	 * 최소 하나는 나와야 한다. */
	if (WARN_ON(!count)) /* Should always be at least one */
		/* [한국어] -ERANGE. */
		return -ERANGE;

	/* [한국어] 사용자 버퍼가 그 개수를 담을 수 있는가. argsz 에서 헤더 크기를 빼고
	 * 원소 크기로 나눈 값이 담을 수 있는 최대 개수다. */
	if (count > (hdr.argsz - sizeof(hdr)) / sizeof(*devices)) {
		/* [한국어] 필요한 개수를 알려 준다. */
		hdr.count = count;
		/* [한국어] -ENOSPC — 사용자는 버퍼를 키워 다시 부른다. */
		ret = -ENOSPC;
		/* [한국어] 배열은 아직 할당하지 않았으므로 헤더만 돌려주러 간다. */
		goto header;
	}

	/* [한국어] 커널 쪽 임시 배열을 0 초기화로 잡는다. */
	devices = kzalloc_objs(*devices, count);
	/* [한국어] 할당 실패. */
	if (!devices)
		/* [한국어] -ENOMEM. */
		return -ENOMEM;

	/* [한국어] 콜백이 채울 배열을 건다. */
	fill.devices = devices;
	/* [한국어] 상한을 알려 준다. 콜백이 이 값을 넘으면 -EAGAIN 을 낸다. */
	fill.nr_devices = count;
	/* [한국어] 콜백이 iommufd 컨텍스트와 dev_set 을 꺼낼 출발점. */
	fill.vdev = &vdev->vdev;

	/* [한국어] 이 디바이스가 2세대(cdev) 경로로 열렸는가. */
	if (vfio_device_cdev_opened(&vdev->vdev))
		/* [한국어] DEV_ID — "장치 목록에 iommufd device id 를 담아 보고한다" 는 문법 표시. */
		fill.flags |= VFIO_PCI_HOT_RESET_FLAG_DEV_ID |
			     /* [한국어] OWNED — 일단 전부 소유했다고 두고, 콜백이 아니면 지운다. */
			     VFIO_PCI_HOT_RESET_FLAG_DEV_ID_OWNED;

	/* [한국어] 콜백이 dev_set 안을 뒤지므로 그 락을 잡아야 한다. */
	mutex_lock(&vdev->vdev.dev_set->lock);
	/* [한국어] 범위 안의 각 장치를 사용자 배열에 채운다. */
	ret = vfio_pci_for_each_slot_or_bus(vdev->pdev, vfio_pci_fill_devs,
					    /* [한국어] 상태 상자를 콜백 데이터로 넘긴다. */
					    &fill, slot);
	/* [한국어] 채우기가 끝났으므로 락을 놓는다. */
	mutex_unlock(&vdev->vdev.dev_set->lock);
	/* [한국어] 순회 중 오류(-EAGAIN 또는 -EPERM). */
	if (ret)
		/* [한국어] 배열을 해제하고 나간다. */
		goto out;

	/* [한국어] 채운 배열을 사용자 버퍼의 헤더 뒤쪽으로 복사한다. */
	if (copy_to_user(arg->devices, devices,
			 /* [한국어] 실제로 채운 개수(fill.count)만큼만 복사한다. */
			 sizeof(*devices) * fill.count)) {
		/* [한국어] -EFAULT. */
		ret = -EFAULT;
		/* [한국어] 배열을 해제하고 나간다. */
		goto out;
	}

	/* [한국어] 실제로 채운 개수를 헤더에 담는다. */
	hdr.count = fill.count;
	/* [한국어] 콜백이 갱신한 플래그(OWNED 가 지워졌을 수 있다)를 헤더에 담는다. */
	hdr.flags = fill.flags;

/* [한국어] -ENOSPC 경로가 여기로 뛰어 들어온다. 그때 hdr.count 에는 필요한 개수가
 * 들어 있고 devices 는 NULL 이다. */
header:
	/* [한국어] 헤더를 사용자에게 복사한다. */
	if (copy_to_user(arg, &hdr, minsz))
		/* [한국어] 복사 실패면 그 오류로 덮어쓴다. */
		ret = -EFAULT;
/* [한국어] 배열을 할당한 뒤의 모든 실패가 여기로 모인다. */
out:
	/* [한국어] 임시 배열을 해제한다. NULL 이어도 안전하므로 -ENOSPC 경로도 통과할 수 있다. */
	kfree(devices);
	/* [한국어] 성공이면 0, 아니면 마지막 오류를 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_ioctl_pci_hot_reset_groups - 1세대(group fd) 문법으로 hot reset 소유권을 증명받는다
 *
 * @vdev:        대상 디바이스.
 * @array_count: 사용자가 넘긴 group fd 개수.
 * @slot:        슬롯 범위인지 버스 범위인지.
 * @arg:         사용자 공간의 struct vfio_pci_hot_reset 포인터.
 *               뒤에 int32 group_fds 배열이 따른다.
 * @return: 0 성공. -EINVAL(개수 초과 또는 group fd 가 아님), -ENOMEM,
 *          -EFAULT, -EBADF(닫힌 fd), 그 밖은 실제 리셋의 반환값.
 *
 * 왜 필요한가: 1세대 ABI 에서 소유권의 단위는 group 이다. 사용자는 자기가 연
 * group fd 들을 증거로 제출하고, 커널은 영향 범위의 모든 디바이스가 그 group
 * 들 안에 있는지 확인한다. **fd 를 붙잡아 두는 것 자체가 증명의 일부**다 —
 * 리셋 도중에 group 이 닫히면 증명이 무의미해지기 때문이다.
 *
 * 동작 과정:
 *  1. 원본 주석대로, 사용자가 터무니없이 큰 배열을 주지 못하게 상한을 먼저
 *     계산한다. 그룹 하나에 장치가 여럿일 수 있으므로 **장치 수가 그룹 수의
 *     상한**이다.
 *  2. fd 배열과 file 포인터 배열을 잡는다. 둘 중 하나만 실패해도 둘 다 해제한다.
 *  3. 사용자에게서 fd 배열을 복사한다.
 *  4. 각 fd 를 fget 으로 struct file 로 바꾸고, vfio_file_is_group 으로
 *     **정말 vfio group fd 인지** 확인한다. 아무 fd 나 넘겨 검사를 통과시키는
 *     것을 막는 게이트다.
 *  5. fd 배열은 더 이상 필요 없으므로 해제한다.
 *  6. 증거 묶음을 꾸려 vfio_pci_dev_set_hot_reset 에 넘긴다.
 *  7. hot_reset_release 에서 잡은 file 참조를 역순으로 모두 놓는다.
 *     file_idx 를 먼저 감소시키는 이유는, 정상 종료 시 file_idx 가
 *     array_count 이므로 마지막 유효 첨자로 맞추기 위해서다. 4단계에서
 *     실패했다면 file_idx 는 실패한 칸을 가리키므로 감소 후 그 앞칸부터
 *     놓게 되어 정확히 맞는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. fget/fput 과 copy_from_user 때문에 잠들 수 있다.
 *
 * 호출자: vfio_pci_ioctl_pci_hot_reset(count 가 0 이 아닐 때).
 * 호출 대상: vfio_pci_for_each_slot_or_bus, kzalloc_objs, copy_from_user,
 * fget, vfio_file_is_group(drivers/vfio/vfio_main.c), fput,
 * vfio_pci_dev_set_hot_reset.
 *
 * 에러 경로: 4단계에서 실패한 fd 는 그 자리에서 fput 하고 break 하므로,
 * 되감기 루프가 그것을 다시 놓지 않는다. 배열 두 개는 어느 경로에서도
 * 정확히 한 번씩 해제된다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_PCI_HOT_RESET, count>0)
 *     → vfio_pci_ioctl_pci_hot_reset → [vfio_pci_ioctl_pci_hot_reset_groups]
 *       → vfio_pci_dev_set_hot_reset → pci_reset_bus(drivers/pci/pci.c:11418)
 */
static int
vfio_pci_ioctl_pci_hot_reset_groups(struct vfio_pci_core_device *vdev,
				    u32 array_count, bool slot,
				    struct vfio_pci_hot_reset __user *arg)
{
	/* [한국어] 사용자에게서 받을 group fd 배열의 커널 사본. */
	int32_t *group_fds;
	/* [한국어] 그 fd 들을 변환한 struct file 포인터 배열. */
	struct file **files;
	/* [한국어] 리셋 함수에 넘길 증거 묶음. */
	struct vfio_pci_group_info info;
	/* [한국어] 루프 첨자, 범위 안 장치 수, 반환값. */
	int file_idx, count = 0, ret = 0;

	/*
	 * We can't let userspace give us an arbitrarily large buffer to copy,
	 * so verify how many we think there could be.  Note groups can have
	 * multiple devices so one group per device is the max.
	 */
	/* [한국어] 원본 주석대로, 사용자가 터무니없이 큰 배열을 주지 못하게 상한을 먼저
	 * 계산한다. */
	ret = vfio_pci_for_each_slot_or_bus(vdev->pdev, vfio_pci_count_devs,
					    /* [한국어] 카운터 주소를 콜백 데이터로 넘긴다. */
					    &count, slot);
	/* [한국어] 순회 중 오류. */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. */
		return ret;

	/* [한국어] 그룹 하나에 장치가 여럿일 수 있으므로 **장치 수가 그룹 수의 상한**이다.
	 * 그보다 많이 주면 명백히 잘못된 요청이다. */
	if (array_count > count)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] fd 배열을 잡는다. */
	group_fds = kzalloc_objs(*group_fds, array_count);
	/* [한국어] file 포인터 배열을 잡는다. */
	files = kzalloc_objs(*files, array_count);
	/* [한국어] 둘 중 하나라도 실패하면. */
	if (!group_fds || !files) {
		/* [한국어] 둘 다 해제한다. kfree(NULL) 은 안전하다. */
		kfree(group_fds);
		/* [한국어] 둘 다 해제한다. */
		kfree(files);
		/* [한국어] -ENOMEM. */
		return -ENOMEM;
	}

	/* [한국어] 사용자에게서 fd 배열을 복사한다. arg->group_fds 가 헤더 뒤의 가변 부분이다. */
	if (copy_from_user(group_fds, arg->group_fds,
			   /* [한국어] 신고한 개수만큼만 읽는다. 위에서 개수 상한을 이미 검증했다. */
			   array_count * sizeof(*group_fds))) {
		/* [한국어] 복사 실패 — 두 배열을 모두 해제한다. */
		kfree(group_fds);
		/* [한국어] 둘 다 해제한다. */
		kfree(files);
		/* [한국어] -EFAULT. */
		return -EFAULT;
	}

	/*
	 * Get the group file for each fd to ensure the group is held across
	 * the reset
	 */
	/* [한국어] 각 fd 를 struct file 로 바꾼다. */
	for (file_idx = 0; file_idx < array_count; file_idx++) {
		/* [한국어] fd 참조를 얻는다. **리셋이 끝날 때까지 group 이 사라지지 않게 붙잡는 것이
		 * 증명의 일부다.** */
		struct file *file = fget(group_fds[file_idx]);

		/* [한국어] 닫힌 fd 이거나 잘못된 번호다. */
		if (!file) {
			/* [한국어] -EBADF. */
			ret = -EBADF;
			/* [한국어] 루프를 멈춘다. file_idx 는 실패한 칸을 가리킨 채 남는다. */
			break;
		}

		/* Ensure the FD is a vfio group FD.*/
		/* [한국어] 아무 fd 나 넘겨 검사를 통과시키지 못하게, 정말 vfio group fd 인지 확인한다. */
		if (!vfio_file_is_group(file)) {
			/* [한국어] 우리가 얻은 참조를 그 자리에서 놓는다. 그래서 아래 되감기 루프가
			 * 이 칸을 다시 놓지 않는다. */
			fput(file);
			/* [한국어] -EINVAL. */
			ret = -EINVAL;
			/* [한국어] 루프를 멈춘다. */
			break;
		}

		/* [한국어] 검증을 통과한 file 을 배열에 넣는다. */
		files[file_idx] = file;
	}

	/* [한국어] fd 번호 배열은 더 이상 필요 없다. */
	kfree(group_fds);

	/* release reference to groups on error */
	/* [한국어] 위 루프에서 실패했다면. */
	if (ret)
		/* [한국어] 얻은 참조를 되감으러 간다. */
		goto hot_reset_release;

	/* [한국어] 증거 묶음에 개수를 담는다. */
	info.count = array_count;
	/* [한국어] 증거 묶음에 file 배열을 담는다. */
	info.files = files;

	/* [한국어] 실제 리셋. 세 번째 인자 NULL 이 "1세대 문법" 을 뜻한다. */
	ret = vfio_pci_dev_set_hot_reset(vdev->vdev.dev_set, &info, NULL);

/* [한국어] 성공이든 실패든 여기를 지나 참조를 모두 놓는다. */
hot_reset_release:
	/* [한국어] 먼저 하나 줄인다. 정상 종료면 file_idx 가 array_count 이므로 마지막
	 * 유효 첨자로 맞춰지고, 루프에서 break 했다면 실패한 칸의 앞칸부터
	 * 놓게 되어 정확히 맞는다. */
	for (file_idx--; file_idx >= 0; file_idx--)
		/* [한국어] 얻은 참조를 놓는다. */
		fput(files[file_idx]);

	/* [한국어] 배열 자체를 해제한다. */
	kfree(files);
	/* [한국어] 리셋 결과를 사용자에게 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_ioctl_pci_hot_reset - VFIO_DEVICE_PCI_HOT_RESET: 슬롯/버스 전체를 리셋한다
 *
 * @vdev: 대상 디바이스.
 * @arg:  사용자 공간의 struct vfio_pci_hot_reset 포인터.
 * @return: 0 성공. -EFAULT, -EINVAL(argsz 부족, flags 가 0 이 아님, 또는 열린
 *          방식과 배열 길이가 어긋남), -ENODEV(슬롯도 버스도 리셋 불가),
 *          그 밖은 실제 리셋의 반환값.
 *
 * 왜 필요한가: 함수 단위 리셋(FLR)이 통하지 않거나 여러 함수를 한꺼번에
 * 초기화해야 할 때 쓴다. 영향 범위가 넓은 만큼 소유권 증명이 필수이고,
 * 그 증명 문법이 세대에 따라 갈린다. 이 함수는 **어느 문법인지 가려내는
 * 분기점**이다.
 *
 * 동작 과정:
 *  1. 헤더 복사, argsz 검증, flags 는 0 이어야 한다(미래 확장을 위해 예약됨).
 *  2. **문법 판정**: `!!hdr.count == vfio_device_cdev_opened(...)` 라는 한 줄이
 *     핵심이다. cdev 로 열린 디바이스는 길이 0 배열을 줘야 하고(iommufd 로
 *     소유를 증명), group 으로 열린 디바이스는 길이가 1 이상이어야 한다
 *     (group fd 로 증명). 두 조건이 같으면(둘 다 참이거나 둘 다 거짓) 문법이
 *     어긋난 것이므로 -EINVAL.
 *  3. 리셋 범위 결정: 슬롯이 되면 슬롯, 아니면 버스. 둘 다 안 되면 -ENODEV.
 *  4. count 가 있으면 1세대 경로로, 없으면 2세대 경로로 간다. 2세대는
 *     호출자의 iommufd 컨텍스트를 그대로 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출자: vfio_pci_core_ioctl.
 * 호출 대상: copy_from_user, vfio_device_cdev_opened, pci_probe_reset_slot,
 * pci_probe_reset_bus, vfio_pci_ioctl_pci_hot_reset_groups,
 * vfio_pci_dev_set_hot_reset, vfio_iommufd_device_ictx.
 *
 * 에러 경로: 모두 즉시 반환이며 부작용이 없다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_PCI_HOT_RESET) → vfio_pci_core_ioctl
 *     → [vfio_pci_ioctl_pci_hot_reset]
 *       → vfio_pci_ioctl_pci_hot_reset_groups(1세대) 또는
 *         vfio_pci_dev_set_hot_reset(2세대)
 */
static int vfio_pci_ioctl_pci_hot_reset(struct vfio_pci_core_device *vdev,
					struct vfio_pci_hot_reset __user *arg)
{
	/* [한국어] 이 ioctl 이 요구하는 최소 구조체 크기(count 필드의 끝까지). */
	unsigned long minsz = offsetofend(struct vfio_pci_hot_reset, count);
	/* [한국어] 커널 쪽 헤더 사본. */
	struct vfio_pci_hot_reset hdr;
	/* [한국어] 슬롯 범위인지 버스 범위인지. */
	bool slot = false;

	/* [한국어] 헤더만 먼저 복사한다. */
	if (copy_from_user(&hdr, arg, minsz))
		/* [한국어] -EFAULT. */
		return -EFAULT;

	/* [한국어] 신고 크기가 부족하거나, 예약된 flags 에 아무 비트나 세워 보냈으면 거절한다.
	 * flags 를 0 으로 강제하는 것은 미래에 새 비트를 도입할 여지를 남기는 관례다. */
	if (hdr.argsz < minsz || hdr.flags)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* zero-length array is only for cdev opened devices */
	/* [한국어] **문법 판정.** cdev 로 열린 디바이스는 길이 0 배열(iommufd 로 증명),
	 * group 으로 열린 디바이스는 길이 1 이상(group fd 로 증명)이어야 한다.
	 * 두 조건이 같으면 문법이 어긋난 것이다. !! 로 count 를 불리언으로 정규화해
	 * cdev 여부와 직접 비교한다. */
	if (!!hdr.count == vfio_device_cdev_opened(&vdev->vdev))
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* Can we do a slot or bus reset or neither? */
	/* [한국어] 슬롯 리셋이 가능한지 조사한다. */
	if (!pci_probe_reset_slot(vdev->pdev->slot))
		/* [한국어] 슬롯 범위로 좁힌다. */
		slot = true;
	/* [한국어] 슬롯이 안 되면 버스 리셋을 조사한다. */
	else if (pci_probe_reset_bus(vdev->pdev->bus))
		/* [한국어] -ENODEV — hot reset 자체가 불가능한 장치다. */
		return -ENODEV;

	/* [한국어] group fd 배열이 있으면 1세대 경로. */
	if (hdr.count)
		/* [한국어] fd 를 파일로 바꾸고 붙잡은 뒤 실제 리셋으로 들어간다. */
		return vfio_pci_ioctl_pci_hot_reset_groups(vdev, hdr.count, slot, arg);

	/* [한국어] 2세대 경로. groups 는 NULL 이고, */
	return vfio_pci_dev_set_hot_reset(vdev->vdev.dev_set, NULL,
					  /* [한국어] 호출자가 묶여 있는 iommufd 컨텍스트를 소유 증명의 기준으로 넘긴다. */
					  vfio_iommufd_device_ictx(&vdev->vdev));
}

/* [한국어]
 * vfio_pci_ioctl_ioeventfd - VFIO_DEVICE_IOEVENTFD: MMIO 쓰기를 커널에서 대신 처리하도록 등록한다
 *
 * @vdev: 대상 디바이스.
 * @arg:  사용자 공간의 struct vfio_device_ioeventfd 포인터.
 * @return: 0 성공. -EFAULT, -EINVAL(argsz 부족, 알 수 없는 플래그, 크기가
 *          2의 거듭제곱이 아님, fd 가 -1 미만), 그 밖은 실제 등록 함수의 값.
 *
 * 왜 필요한가: 게스트가 특정 BAR 오프셋에 특정 값을 쓰는 일이 아주 잦을 때
 * (예: NVMe 도어벨, virtio 알림), 그 쓰기마다 사용자 공간(QEMU)까지 나가면
 * 비싸다. ioeventfd 는 "이 오프셋에 이 값이 쓰이면 커널이 대신 MMIO 를 하고
 * eventfd 를 울려라" 를 등록해, VM exit 한 번을 아낀다.
 *
 * 동작 과정:
 *  1. 헤더 복사와 argsz 검증.
 *  2. 플래그에 크기 마스크 말고 다른 비트가 있으면 -EINVAL.
 *  3. count 는 접근 폭(1/2/4/8 바이트)이다. hweight8 로 **비트가 정확히 하나**
 *     인지 검사해 2의 거듭제곱만 허용한다. fd 는 -1(해제) 이상이어야 한다.
 *  4. vfio_pci_ioeventfd(drivers/vfio/pci/vfio_pci_rdwr.c:431)에 넘긴다.
 *     거기서 오프셋 검증, BAR 매핑 확보, virqfd 등록, 그리고 개수 상한
 *     (vfio_pci_priv.h:12 의 VFIO_PCI_IOEVENTFD_MAX, 1000)을 처리한다.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출자: vfio_pci_core_ioctl.
 * 호출 대상: copy_from_user, hweight8, vfio_pci_ioeventfd.
 *
 * 에러 경로: 검증 실패는 즉시 반환. 실제 등록의 오류는 그대로 전달한다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_IOEVENTFD) → vfio_pci_core_ioctl
 *     → [vfio_pci_ioctl_ioeventfd] → vfio_pci_rdwr.c:vfio_pci_ioeventfd
 */
static int vfio_pci_ioctl_ioeventfd(struct vfio_pci_core_device *vdev,
				    struct vfio_device_ioeventfd __user *arg)
{
	/* [한국어] 이 ioctl 이 요구하는 최소 구조체 크기(fd 필드의 끝까지). */
	unsigned long minsz = offsetofend(struct vfio_device_ioeventfd, fd);
	/* [한국어] 커널 쪽 사본. */
	struct vfio_device_ioeventfd ioeventfd;
	/* [한국어] 접근 폭(바이트). */
	int count;

	/* [한국어] 구조체를 복사한다. */
	if (copy_from_user(&ioeventfd, arg, minsz))
		/* [한국어] -EFAULT. */
		return -EFAULT;

	/* [한국어] 신고 크기가 부족하면 해석할 수 없다. */
	if (ioeventfd.argsz < minsz)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] 크기 마스크 말고 다른 비트가 서 있으면 알 수 없는 요청이다. */
	if (ioeventfd.flags & ~VFIO_DEVICE_IOEVENTFD_SIZE_MASK)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] flags 의 크기 필드가 곧 접근 폭이다. 1, 2, 4, 8 중 하나여야 한다. */
	count = ioeventfd.flags & VFIO_DEVICE_IOEVENTFD_SIZE_MASK;

	/* [한국어] hweight8 은 세워진 비트 수를 센다. 정확히 하나여야 2 의 거듭제곱이다.
	 * fd 는 -1(등록 해제) 이상이어야 한다. */
	if (hweight8(count) != 1 || ioeventfd.fd < -1)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] 실제 등록은 형제 파일이 한다(vfio_pci_rdwr.c:431). 거기서 오프셋 검증,
	 * BAR 매핑 확보, virqfd 등록, 그리고 개수 상한
	 * (vfio_pci_priv.h:12 의 VFIO_PCI_IOEVENTFD_MAX, 1000)을 처리한다. */
	return vfio_pci_ioeventfd(vdev, ioeventfd.offset, ioeventfd.data, count,
				  /* [한국어] -1 이면 그 오프셋의 등록을 해제하라는 뜻이다. */
				  ioeventfd.fd);
}

/* [한국어]
 * vfio_pci_core_ioctl - vfio_device_ops 의 ioctl 슬롯: PCI 전용 ioctl 일곱 개를 나눈다
 *
 * @core_vdev: 코어 쪽 디바이스.
 * @cmd:       ioctl 번호.
 * @arg:       사용자 인자(포인터를 정수로 담은 것).
 * @return: 각 처리 함수의 반환값. 모르는 명령이면 -ENOTTY.
 *
 * 왜 필요한가: VFIO 의 ioctl 은 두 층으로 나뉜다. 코어(vfio_main.c 의
 * vfio_device_fops_unl_ioctl)가 BIND_IOMMUFD, ATTACH_IOAS, FEATURE,
 * GET_REGION_INFO 같은 **모든 디바이스에 공통인 것**을 처리하고, 나머지를
 * vendor 의 ops->ioctl 로 넘긴다. 이 함수가 그 넘겨받는 자리이며,
 * "PCI 라서 존재하는" 명령들만 여기 모인다.
 *
 * 동작 과정: container_of 로 PCI 객체를 되찾고 switch 로 나눈다.
 *  - GET_INFO / GET_IRQ_INFO: 능력 보고.
 *  - GET_PCI_HOT_RESET_INFO / PCI_HOT_RESET: 슬롯/버스 리셋 조회와 실행.
 *  - IOEVENTFD: MMIO 쓰기 가속 등록.
 *  - RESET: 함수 단위 리셋.
 *  - SET_IRQS: 인터럽트 연결.
 * default 가 -ENOTTY 인 것이 중요하다 — 코어가 이 값을 보고 "이 vendor 는
 * 이 명령을 모른다" 로 해석한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **상위 래퍼가 이미 런타임 PM 으로 장치를
 * D0 에 붙잡아 둔 상태**다(vfio_main.c 의 ioctl 디스패처). 그래서 각 처리
 * 함수가 전원 상태를 다시 확인하지 않아도 된다.
 *
 * 호출자: vfio_main.c 의 ioctl 디스패처가 device->ops->ioctl 로 부른다.
 * vendor 가 자기 ioctl 을 덧붙일 때는 자기 함수에서 이것을 default 로 부른다.
 * 호출 대상: 위 일곱 처리 함수.
 *
 * 에러 경로: 각 처리 함수가 자기 오류를 돌려준다.
 *
 * 호출 체인:
 *   사용자 ioctl(2) → vfio_main.c:vfio_device_fops_unl_ioctl
 *     → ops->ioctl = [vfio_pci_core_ioctl] → 일곱 처리 함수
 */
long vfio_pci_core_ioctl(struct vfio_device *core_vdev, unsigned int cmd,
			 unsigned long arg)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	/* [한국어] 정수로 전달된 인자를 사용자 포인터로 되돌린다. __user 표시가 sparse 에게
	 * "이 포인터는 역참조하지 말고 copy_from_user 로만 다뤄라" 를 알린다. */
	void __user *uarg = (void __user *)arg;

	/* [한국어] ioctl 번호로 나눈다. */
	switch (cmd) {
	/* [한국어] 디바이스 능력 조회. */
	case VFIO_DEVICE_GET_INFO:
		/* [한국어] region 개수, IRQ 개수, 리셋 가능 여부를 보고한다. */
		return vfio_pci_ioctl_get_info(vdev, uarg);
	/* [한국어] 인터럽트 종류별 능력 조회. */
	case VFIO_DEVICE_GET_IRQ_INFO:
		/* [한국어] 최대 벡터 수와 마스킹/크기 조절 가능 여부를 보고한다. */
		return vfio_pci_ioctl_get_irq_info(vdev, uarg);
	/* [한국어] hot reset 영향 범위 조회. */
	case VFIO_DEVICE_GET_PCI_HOT_RESET_INFO:
		/* [한국어] 함께 리셋되는 PCI 함수 목록을 보고한다. */
		return vfio_pci_ioctl_get_pci_hot_reset_info(vdev, uarg);
	/* [한국어] MMIO 쓰기 가속 등록. */
	case VFIO_DEVICE_IOEVENTFD:
		/* [한국어] 특정 오프셋의 특정 값 쓰기를 커널이 대신 처리하게 한다. */
		return vfio_pci_ioctl_ioeventfd(vdev, uarg);
	/* [한국어] 슬롯/버스 리셋 실행. */
	case VFIO_DEVICE_PCI_HOT_RESET:
		/* [한국어] 소유권을 증명받은 뒤 pci_reset_bus 로 간다. */
		return vfio_pci_ioctl_pci_hot_reset(vdev, uarg);
	/* [한국어] 함수 단위 리셋 실행. */
	case VFIO_DEVICE_RESET:
		/* [한국어] BAR 매핑을 걷어내고 pci_try_reset_function 으로 간다. */
		return vfio_pci_ioctl_reset(vdev, uarg);
	/* [한국어] 인터럽트 연결/해제. */
	case VFIO_DEVICE_SET_IRQS:
		/* [한국어] eventfd 를 벡터에 건다. */
		return vfio_pci_ioctl_set_irqs(vdev, uarg);
	/* [한국어] 이 vendor 가 모르는 ioctl. */
	default:
		/* [한국어] -ENOTTY 는 코어에게 "이 명령은 여기서 처리하지 않는다" 를 뜻한다. */
		return -ENOTTY;
	}
}
/* [한국어] vendor 가 자기 ioctl 의 default 에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_ioctl);

/* [한국어]
 * vfio_pci_core_feature_token - VFIO_DEVICE_FEATURE_PCI_VF_TOKEN: PF 의 VF 토큰을 설정한다
 *
 * @vdev:  대상 디바이스(반드시 SR-IOV PF 여야 한다).
 * @flags: VFIO_DEVICE_FEATURE 플래그.
 * @arg:   사용자 공간의 uuid_t 포인터.
 * @argsz: 사용자가 신고한 인자 크기.
 * @return: 0 성공. -ENOTTY 면 이 디바이스는 PF 가 아니라 토큰이 없다.
 *          -EFAULT 는 사용자 메모리 오류. 그 밖은 검증 함수의 값.
 *
 * 왜 필요한가: PF 를 여는 사용자가 자기 VF 사용자들과 공유할 비밀을 정하는
 * 통로다. 이 값을 알아야만 VF 를 열 수 있으므로, PF 사용자가 VF 사용자를
 * 고르는 셈이다.
 *
 * **GET 을 지원하지 않는 것이 설계의 핵심**이다. 원본 영어 주석이 이유를
 * 밝힌다 — GET 을 허용하면 새 사용자가 이전 사용자의 토큰을 읽어 낼 수 있다.
 * 그래서 vfio_check_feature 에 SET 만 신고한다.
 *
 * 동작 과정:
 *  1. vf_token 이 없으면(PF 가 아니면) -ENOTTY.
 *  2. SET 방향과 uuid 크기를 검증한다.
 *  3. 사용자에게서 UUID 를 복사한다.
 *  4. vf_token->lock 아래에서 uuid_copy 로 덮어쓴다.
 * **users 가 0 이 아니어도 덮어쓴다** — 이미 열려 있는 VF 는 영향을 받지 않고,
 * 앞으로 열 VF 만 새 토큰을 요구받는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. mutex 를 잡으므로 잠들 수 있다.
 *
 * 호출자: vfio_pci_core_ioctl_feature.
 * 호출 대상: vfio_check_feature, copy_from_user, uuid_copy.
 *
 * 에러 경로: 모두 락을 잡기 전이거나 락 안에서 실패할 수 없는 연산이다.
 *
 * 호출 체인:
 *   사용자 VFIO_DEVICE_FEATURE ioctl → vfio_main.c 의 feature 디스패처
 *     → vfio_pci_core_ioctl_feature → [vfio_pci_core_feature_token]
 */
static int vfio_pci_core_feature_token(struct vfio_pci_core_device *vdev,
				       u32 flags, uuid_t __user *arg,
				       size_t argsz)
{
	/* [한국어] 사용자에게서 받을 UUID 의 커널 사본. */
	uuid_t uuid;
	/* [한국어] 검증 결과. */
	int ret;

	/* [한국어] vf_token 이 없다 = 이 디바이스는 SR-IOV PF 가 아니다. */
	if (!vdev->vf_token)
		/* [한국어] -ENOTTY — 그런 기능이 없는 디바이스다. */
		return -ENOTTY;
	/*
	 * We do not support GET of the VF Token UUID as this could
	 * expose the token of the previous device user.
	 */
	/* [한국어] **SET 만 신고한다.** GET 을 지원하지 않는 것이 설계의 핵심이며,
	 * 원본 영어 주석이 이유를 밝힌다 — 이전 사용자의 토큰이 새 사용자에게
	 * 새는 것을 막기 위해서다. */
	ret = vfio_check_feature(flags, argsz, VFIO_DEVICE_FEATURE_SET,
				 /* [한국어] 인자 크기는 UUID 하나. */
				 sizeof(uuid));
	/* [한국어] 1 이 아니면 실제 수행이 아니다. */
	if (ret != 1)
		/* [한국어] 그대로 전달한다. */
		return ret;

	/* [한국어] 사용자에게서 UUID 를 복사한다. */
	if (copy_from_user(&uuid, arg, sizeof(uuid)))
		/* [한국어] -EFAULT. */
		return -EFAULT;

	/* [한국어] 토큰을 지키는 락을 잡는다. */
	mutex_lock(&vdev->vf_token->lock);
	/* [한국어] **users 가 0 이 아니어도 덮어쓴다.** 이미 열려 있는 VF 는 영향을 받지
	 * 않고, 앞으로 열 VF 만 새 토큰을 요구받는다. */
	uuid_copy(&vdev->vf_token->uuid, &uuid);
	/* [한국어] 락을 놓는다. */
	mutex_unlock(&vdev->vf_token->lock);
	/* [한국어] 설정은 실패할 수 없다. */
	return 0;
}

/* [한국어]
 * vfio_pci_core_ioctl_feature - vfio_device_ops 의 device_feature 슬롯: PCI 전용 기능 다섯 개를 나눈다
 *
 * @device: 코어 쪽 디바이스.
 * @flags:  VFIO_DEVICE_FEATURE 플래그. 하위 16비트가 기능 번호다.
 * @arg:    기능별 인자 영역의 사용자 포인터.
 * @argsz:  사용자가 신고한 인자 크기.
 * @return: 각 처리 함수의 값. 모르는 기능이면 -ENOTTY.
 *
 * 왜 필요한가: VFIO_DEVICE_FEATURE 는 ioctl 번호를 늘리지 않고 기능을 추가하는
 * 확장 통로다. 코어가 마이그레이션과 DMA 로깅 같은 공통 기능을 먼저 처리하고,
 * 남은 것을 vendor 의 이 슬롯으로 넘긴다.
 *
 * 동작 과정: 기능 번호로 switch 한다.
 *  - LOW_POWER_ENTRY / _WITH_WAKEUP / LOW_POWER_EXIT: 런타임 PM 제어.
 *  - PCI_VF_TOKEN: SR-IOV 신뢰 토큰 설정.
 *  - DMA_BUF: BAR 를 dma-buf 로 내주기
 *    (drivers/vfio/pci/vfio_pci_dmabuf.c:221. CONFIG_VFIO_PCI_DMABUF 가
 *    꺼져 있으면 vfio_pci_priv.h 의 stub 이 -ENOTTY 를 돌려준다).
 * default 가 -ENOTTY 라 코어는 "이 vendor 는 그 기능이 없다" 로 해석한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 상위에서 런타임 PM 으로 D0 를 확보한 상태다.
 *
 * 호출자: vfio_main.c 의 feature 디스패처가 device->ops->device_feature 로
 * 부른다. vendor 가 자기 기능을 덧붙일 때는 자기 함수의 default 에서
 * 이것을 부른다.
 * 호출 대상: 위 다섯 처리 함수.
 *
 * 에러 경로: 각 처리 함수가 자기 오류를 돌려준다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_FEATURE) → vfio_main.c 의 feature 디스패처
 *     → ops->device_feature = [vfio_pci_core_ioctl_feature]
 */
int vfio_pci_core_ioctl_feature(struct vfio_device *device, u32 flags,
				void __user *arg, size_t argsz)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(device, struct vfio_pci_core_device, vdev);

	/* [한국어] flags 의 하위 16비트가 기능 번호다. 상위 비트는 SET/GET/PROBE 방향이라
	 * 마스크로 걸러 낸다. */
	switch (flags & VFIO_DEVICE_FEATURE_MASK) {
	/* [한국어] 저전력 진입(알림 없음). */
	case VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY:
		/* [한국어] usage count 를 내려 장치를 잠들게 한다. */
		return vfio_pci_core_pm_entry(vdev, flags, arg, argsz);
	/* [한국어] 저전력 진입(깨어남 알림 있음). */
	case VFIO_DEVICE_FEATURE_LOW_POWER_ENTRY_WITH_WAKEUP:
		/* [한국어] eventfd 를 함께 등록한다. */
		return vfio_pci_core_pm_entry_with_wakeup(vdev, flags,
							  arg, argsz);
	/* [한국어] 저전력 이탈. */
	case VFIO_DEVICE_FEATURE_LOW_POWER_EXIT:
		/* [한국어] usage count 를 되돌리고 eventfd 를 놓는다. */
		return vfio_pci_core_pm_exit(vdev, flags, arg, argsz);
	/* [한국어] SR-IOV 신뢰 토큰 설정. */
	case VFIO_DEVICE_FEATURE_PCI_VF_TOKEN:
		/* [한국어] PF 의 UUID 를 사용자가 정한 값으로 바꾼다. */
		return vfio_pci_core_feature_token(vdev, flags, arg, argsz);
	/* [한국어] BAR 를 dma-buf 로 내주기. */
	case VFIO_DEVICE_FEATURE_DMA_BUF:
		/* [한국어] 구현은 vfio_pci_dmabuf.c:221. CONFIG_VFIO_PCI_DMABUF 가 꺼져 있으면
		 * vfio_pci_priv.h 의 stub 이 -ENOTTY 를 돌려준다. */
		return vfio_pci_core_feature_dma_buf(vdev, flags, arg, argsz);
	/* [한국어] 이 vendor 가 모르는 기능. */
	default:
		/* [한국어] -ENOTTY 는 코어에게 "이 기능은 여기서 처리하지 않는다" 를 뜻한다. */
		return -ENOTTY;
	}
}
/* [한국어] vendor 가 자기 device_feature 의 default 에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_ioctl_feature);

/* [한국어]
 * vfio_pci_rw - read(2)/write(2) 의 공통 몸통: 오프셋의 region 번호로 갈라 보낸다
 *
 * @vdev:    대상 디바이스.
 * @buf:     사용자 버퍼.
 * @count:   바이트 수.
 * @ppos:    파일 오프셋 포인터. 상위 비트가 region 번호, 하위 40비트가
 *           region 안의 위치다(VFIO_PCI_OFFSET_SHIFT 는 40).
 * @iswrite: 쓰기면 true.
 * @return: 처리한 바이트 수, 또는 음수 errno.
 *          -EINVAL(범위 밖 region 또는 ROM 쓰기), -EIO(장치를 깨우지 못함).
 *
 * 왜 필요한가: **파일 오프셋 하나로 여러 자원을 표현하는 VFIO 의 핵심 관례가
 * 여기서 해석된다.** 사용자는 fd 하나에 lseek 과 read/write 만으로 config 도
 * BAR 도 ROM 도 만진다. 그 오프셋을 region 번호로 되돌려 각각의 중재기로
 * 보내는 것이 이 함수다.
 *
 * 동작 과정:
 *  1. 오프셋 상위 비트에서 region 번호를 뽑아 범위를 검사한다.
 *  2. **pm_runtime_resume_and_get 으로 장치를 깨운다.** 잠든 장치의 config 나
 *     MMIO 를 만지면 안 되기 때문이다. 실패하면 -EIO(속도 제한 로그와 함께).
 *     이 참조는 함수 끝에서 반드시 놓는다.
 *  3. region 별로 갈라 보낸다.
 *     - CONFIG: vfio_pci_config_rw — **바이트 단위 권한표 중재**를 거친다.
 *     - ROM: 쓰기는 -EINVAL 로 거절하고 읽기만 BAR 경로로 보낸다.
 *     - BAR0~5: vfio_pci_bar_rw — MSI-X 테이블 구간이 제외된다.
 *     - VGA: vfio_pci_vga_rw(CONFIG_VFIO_PCI_VGA 가 꺼지면 stub 이 -EINVAL).
 *     - 그 밖: vendor region 의 ops->rw. **여기서는 ops 나 rw 의 NULL 을
 *       검사하지 않는다** — 등록 시점에 항상 채워지는 것이 규약이다.
 *  4. 런타임 PM 참조를 놓고 결과를 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 이 함수 자체는 락을 잡지 않는다.
 * memory_lock 은 아래쪽(vfio_pci_core_ioread/iowrite 매크로)에서 read 로 잡는다.
 *
 * 호출자: vfio_pci_core_read 와 vfio_pci_core_write.
 * 호출 대상: pm_runtime_resume_and_get, vfio_pci_config_rw
 * (vfio_pci_config.c:1976), vfio_pci_bar_rw(vfio_pci_rdwr.c:226),
 * vfio_pci_vga_rw(vfio_pci_rdwr.c:309), vendor 의 ops->rw, pm_runtime_put.
 *
 * 에러 경로: 2단계 실패는 참조를 얻지 못한 상태라 그냥 반환한다. 그 이후의
 * 모든 경로는 pm_runtime_put 을 지난다.
 *
 * 호출 체인:
 *   사용자 read(2)/write(2) → vfio_main.c:vfio_device_fops_read/write
 *     → ops->read/write → vfio_pci_core_read/_write → [vfio_pci_rw]
 */
static ssize_t vfio_pci_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			   size_t count, loff_t *ppos, bool iswrite)
{
	/* [한국어] 파일 오프셋의 상위 비트(40비트 시프트)에서 region 번호를 뽑는다.
	 * **VFIO 가 fd 하나로 여러 자원을 표현하는 관례가 여기서 해석된다.** */
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	/* [한국어] 각 region 처리기의 반환값. */
	int ret;

	/* [한국어] 표준 region + vendor 추가분을 넘는 번호는 존재하지 않는다. */
	if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] **장치를 깨워 D0 에 붙잡는다.** 잠든 장치의 config 나 MMIO 를 만지면 안 된다. */
	ret = pm_runtime_resume_and_get(&vdev->pdev->dev);
	/* [한국어] 깨우기 실패. */
	if (ret) {
		/* [한국어] 속도 제한 로그를 남긴다. 이 경로는 사용자가 반복 호출할 수 있어
		 * 제한 없이 찍으면 로그가 폭주한다. */
		pci_info_ratelimited(vdev->pdev, "runtime resume failed %d\n",
				     ret);
		/* [한국어] -EIO 로 통일해 돌려준다. 사용자에게는 "장치가 응답하지 않는다" 는 뜻이다. */
		return -EIO;
	}

	/* [한국어] region 번호로 갈라 각각의 중재기로 보낸다. */
	switch (index) {
	/* [한국어] config 공간. */
	case VFIO_PCI_CONFIG_REGION_INDEX:
		/* [한국어] **바이트 단위 권한표 중재**를 거친다(vfio_pci_config.c:1976).
		 * virt 비트는 vconfig 그림자에서, write 비트 중 virt 가 아닌 것만
		 * 실제 하드웨어로 나간다. */
		ret = vfio_pci_config_rw(vdev, buf, count, ppos, iswrite);
		/* [한국어] config case 끝. */
		break;

	/* [한국어] 확장 ROM. */
	case VFIO_PCI_ROM_REGION_INDEX:
		/* [한국어] ROM 에 쓰기는 허용하지 않는다. */
		if (iswrite)
			/* [한국어] -EINVAL. GET_REGION_INFO 가 READ 플래그만 준 것과 일관된다. */
			ret = -EINVAL;
		/* [한국어] 읽기 요청. */
		else
			/* [한국어] BAR 경로로 보내되 iswrite 를 강제로 false 로 준다.
			 * 거기서 pci_map_rom 으로 매핑하고, 실제 ROM 뒤쪽은 0xFF 로 채운다. */
			ret = vfio_pci_bar_rw(vdev, buf, count, ppos, false);
		/* [한국어] ROM case 끝. */
		break;

	/* [한국어] BAR0~5 를 한 case 로 묶는다. */
	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		/* [한국어] **MSI-X 테이블 구간이 제외된 채** 전달된다(vfio_pci_rdwr.c:226).
		 * 그 구간은 쓰기가 버려지고 읽기가 0xFF 로 채워진다. */
		ret = vfio_pci_bar_rw(vdev, buf, count, ppos, iswrite);
		/* [한국어] BAR case 끝. */
		break;

	/* [한국어] 레거시 VGA 창. */
	case VFIO_PCI_VGA_REGION_INDEX:
		/* [한국어] CONFIG_VFIO_PCI_VGA 가 꺼져 있으면 vfio_pci_priv.h 의 stub 이 -EINVAL 을
		 * 돌려준다(vfio_pci_rdwr.c:309). */
		ret = vfio_pci_vga_rw(vdev, buf, count, ppos, iswrite);
		/* [한국어] VGA case 끝. */
		break;

	/* [한국어] vendor 가 등록한 추가 region. */
	default:
		/* [한국어] 표준 개수를 빼서 vendor 배열 안의 첨자로 바꾼다. 위에서 상한을 이미
		 * 검사했으므로 배열 밖으로 나가지 않는다. */
		index -= VFIO_PCI_NUM_REGIONS;
		/* [한국어] **여기서는 ops 나 rw 의 NULL 을 검사하지 않는다** — rw 는 regops 의 필수
		 * 슬롯이라는 규약이 이 한 줄에 담겨 있다. */
		ret = vdev->region[index].ops->rw(vdev, buf,
						   /* [한국어] 사용자 버퍼와 오프셋을 그대로 넘긴다. 오프셋 해석은 vendor 몫이다. */
						   count, ppos, iswrite);
		/* [한국어] vendor case 끝. */
		break;
	}

	/* [한국어] 런타임 PM 참조를 놓는다. 어떤 경로로 왔든 반드시 지난다. */
	pm_runtime_put(&vdev->pdev->dev);
	/* [한국어] 처리한 바이트 수 또는 오류를 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_core_read - vfio_device_ops 의 read 슬롯
 *
 * @core_vdev: 코어 쪽 디바이스.
 * @buf:       사용자 버퍼.
 * @count:     바이트 수.
 * @ppos:      파일 오프셋 포인터.
 * @return: 읽은 바이트 수 또는 음수 errno. count 가 0 이면 0.
 *
 * 왜 필요한가: 코어의 file_operations 는 struct vfio_device 만 알고 PCI 를
 * 모른다. container_of 로 PCI 객체를 되찾아 공통 몸통으로 넘기는 얇은 껍데기가
 * 필요하다. 길이 0 을 미리 걸러 내는 것도 여기서 한다 — 그래야 아래에서
 * region 번호 계산이 무의미해지는 경우를 신경 쓰지 않아도 된다.
 *
 * 동작 과정: 두 줄이다. 0 길이 조기 반환, 그리고 iswrite=false 로 vfio_pci_rw.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출자: vfio_main.c 의 파일 연산이 device->ops->read 로 부른다
 * (vfio_pci.c:137 이 이 함수를 그 슬롯에 등록한다).
 * drivers/vfio/pci/virtio/legacy_io.c:139, 226 처럼 vendor 가 자기 read 안에서
 * 이것을 부르기도 한다.
 * 호출 대상: vfio_pci_rw.
 *
 * 에러 경로: vfio_pci_rw 의 오류를 그대로 전달한다.
 *
 * 호출 체인:
 *   사용자 read(2) → vfio_main.c 의 파일 연산 → ops->read = [vfio_pci_core_read]
 *     → vfio_pci_rw
 */
ssize_t vfio_pci_core_read(struct vfio_device *core_vdev, char __user *buf,
		size_t count, loff_t *ppos)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	/* [한국어] 길이 0 은 아무 일도 하지 않는다. 여기서 걸러 두면 아래 오프셋 해석이
	 * 무의미해지는 경우를 신경 쓰지 않아도 된다. */
	if (!count)
		/* [한국어] read(2) 의 관례대로 0 을 돌려준다. */
		return 0;

	/* [한국어] iswrite=false 로 공통 몸통에 넘긴다. */
	return vfio_pci_rw(vdev, buf, count, ppos, false);
}
/* [한국어] vendor 가 자기 read 안에서 부를 수 있게 내보낸다
 * (drivers/vfio/pci/virtio/legacy_io.c:139, 226 이 그렇게 쓴다). */
EXPORT_SYMBOL_GPL(vfio_pci_core_read);

/* [한국어]
 * vfio_pci_core_write - vfio_device_ops 의 write 슬롯
 *
 * @core_vdev: 코어 쪽 디바이스.
 * @buf:       사용자 버퍼(const 로 받는다).
 * @count:     바이트 수.
 * @ppos:      파일 오프셋 포인터.
 * @return: 쓴 바이트 수 또는 음수 errno. count 가 0 이면 0.
 *
 * 왜 필요한가: read 쪽과 같은 이유의 껍데기다. 다만 const 한정자를 떼는
 * 캐스트가 하나 들어간다 — vfio_pci_rw 가 읽기와 쓰기를 한 함수로 처리하려고
 * 버퍼를 비 const 로 받기 때문이다. 실제로 쓰기 경로에서 그 버퍼에 쓰는 일은
 * 없다.
 *
 * 동작 과정: 0 길이 조기 반환, 그리고 iswrite=true 로 vfio_pci_rw.
 *
 * 실행 컨텍스트: 프로세스 문맥.
 *
 * 호출자: vfio_main.c 의 파일 연산이 device->ops->write 로 부른다
 * (vfio_pci.c:138). vendor 가 감싸 쓰기도 한다
 * (drivers/vfio/pci/virtio/legacy_io.c:260, 280).
 * 호출 대상: vfio_pci_rw.
 *
 * 에러 경로: vfio_pci_rw 의 오류를 그대로 전달한다.
 *
 * 호출 체인:
 *   사용자 write(2) → vfio_main.c 의 파일 연산 → ops->write
 *     = [vfio_pci_core_write] → vfio_pci_rw
 */
ssize_t vfio_pci_core_write(struct vfio_device *core_vdev, const char __user *buf,
		size_t count, loff_t *ppos)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	/* [한국어] 길이 0 은 아무 일도 하지 않는다. */
	if (!count)
		/* [한국어] write(2) 의 관례대로 0 을 돌려준다. */
		return 0;

	/* [한국어] const 를 떼는 캐스트. vfio_pci_rw 가 읽기와 쓰기를 한 함수로 처리하려고
	 * 버퍼를 비 const 로 받기 때문이며, 쓰기 경로에서 그 버퍼에 쓰지는 않는다. */
	return vfio_pci_rw(vdev, (char __user *)buf, count, ppos, true);
}
/* [한국어] vendor 가 자기 write 안에서 부를 수 있게 내보낸다
 * (drivers/vfio/pci/virtio/legacy_io.c:260, 280). */
EXPORT_SYMBOL_GPL(vfio_pci_core_write);

/* [한국어]
 * vfio_pci_zap_bars - 사용자가 mmap 해 둔 BAR 매핑을 전부 걷어낸다
 *
 * @vdev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: **이 파일에서 가장 중요한 안전 장치다.** 리셋 직전이나 메모리
 * 디코드를 끄기 직전에, 사용자 프로세스의 페이지 테이블에 남아 있는 BAR
 * 매핑을 지워야 한다. 지우지 않으면 리셋 중인(또는 디코드가 꺼진) 장치의
 * MMIO 에 CPU 가 접근해 호스트가 마스터 어보트나 정의되지 않은 데이터를 만난다.
 * 사용자 공간은 fd 를 계속 들고 있어도 되고, 다음 접근은 페이지 폴트를 맞아
 * vfio_pci_mmap_huge_fault 로 들어온다. 그 폴트 핸들러가 memory_lock 을 read 로
 * 잡으려다 막히거나 SIGBUS 를 돌려주므로, **회수와 재매핑 금지가 한 쌍**으로
 * 동작한다.
 *
 * 동작 과정:
 *  1. BAR 영역이 차지하는 파일 오프셋 구간을 계산한다. BAR0 의 시작부터
 *     ROM region 의 시작 직전까지가 BAR0~5 여섯 개의 구간이다.
 *  2. unmap_mapping_range 로 그 구간을 가리키는 모든 매핑을 없앤다.
 *     마지막 인자 true 는 "even cows" — 이 파일에 대한 모든 매핑을 강제로
 *     끊으라는 뜻이다.
 *  3. 대상 주소 공간은 core_vdev->inode->i_mapping 이다. VFIO 코어가
 *     디바이스마다 pseudo-fs 의 inode 를 하나씩 두는 이유가 바로 이것 —
 *     여러 프로세스가 같은 디바이스를 mmap 해도 한 번에 걷어낼 수 있게
 *     공통 address_space 를 만들어 둔 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출자는 memory_lock 을 write 로 쥐고 있거나
 * (zap_and_down_write_memory_lock 안) 곧 쥔다. unmap_mapping_range 는 각
 * 프로세스의 페이지 테이블 락을 내부에서 잡는다.
 *
 * 호출자: vfio_pci_zap_and_down_write_memory_lock 과
 * vfio_pci_dev_set_hot_reset(각 디바이스마다).
 * 호출 대상: unmap_mapping_range.
 *
 * 에러 경로: 없다. 실패할 수 없다.
 *
 * 호출 체인:
 *   vfio_pci_ioctl_reset / _runtime_pm_entry / _dev_set_hot_reset
 *     → [vfio_pci_zap_bars] → unmap_mapping_range
 */
static void vfio_pci_zap_bars(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 코어 객체를 가리킨다. inode 는 코어가 pseudo-fs 에 만들어 둔 것이다. */
	struct vfio_device *core_vdev = &vdev->vdev;
	/* [한국어] BAR0 region 이 시작하는 파일 오프셋. */
	loff_t start = VFIO_PCI_INDEX_TO_OFFSET(VFIO_PCI_BAR0_REGION_INDEX);
	/* [한국어] ROM region 이 시작하는 파일 오프셋. 즉 BAR5 의 끝 다음이다. */
	loff_t end = VFIO_PCI_INDEX_TO_OFFSET(VFIO_PCI_ROM_REGION_INDEX);
	/* [한국어] 그 사이가 BAR0~5 여섯 개가 차지하는 오프셋 구간이다. */
	loff_t len = end - start;

	/* [한국어] **그 구간을 가리키는 모든 사용자 매핑을 없앤다.** 마지막 인자 true 는
	 * "even cows" — 이 파일에 대한 모든 매핑을 강제로 끊으라는 뜻이다.
	 * 대상이 core_vdev->inode->i_mapping 인 것이 핵심으로, 여러 프로세스가
	 * 같은 디바이스를 mmap 해도 한 번에 걷어낼 수 있게 코어가 디바이스마다
	 * inode 를 하나씩 두었다. */
	unmap_mapping_range(core_vdev->inode->i_mapping, start, len, true);
}

/* [한국어]
 * vfio_pci_zap_and_down_write_memory_lock - 매핑 회수와 쓰기 락 획득을 한 동작으로 묶는다
 *
 * @vdev: 대상 디바이스.
 * @return: 없음. 반환 시점에 memory_lock 은 **write 로 잡힌 상태**다 —
 *          호출자가 반드시 up_write 로 풀어야 한다.
 *
 * 왜 필요한가: 순서가 중요하다. 락을 먼저 잡고 매핑을 걷어내야, 걷어내는
 * 동안 다른 CPU 의 폴트 핸들러가 새 매핑을 만들지 못한다. 폴트 핸들러는
 * memory_lock 을 read 로 잡으므로 write 를 쥔 동안 들어오지 못한다. 그
 * 순서를 호출자마다 되풀이하지 않으려고 한 함수로 묶었고, 이름이 그 순서를
 * 그대로 말한다(down_write 가 뒤에 적혀 있지만 코드는 락이 먼저다 —
 * 이름은 "zap 하고 나면 write 락을 쥔 상태" 라는 결과를 말한다).
 *
 * 동작 과정: down_write 로 memory_lock 을 잡고 vfio_pci_zap_bars 를 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **락을 잡은 채 반환하는 함수**이므로
 * 호출자와 짝을 이뤄야 한다. 세 호출자 모두 자기 함수 안에서 up_write 한다.
 *
 * 호출자: vfio_pci_runtime_pm_entry, vfio_pci_ioctl_reset, 그리고
 * drivers/vfio/pci/vfio_pci_config.c 의 메모리 디코드 비활성화 처리.
 * 호출 대상: down_write, vfio_pci_zap_bars.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_ioctl_reset / vfio_pci_runtime_pm_entry / vfio_pci_config.c
 *     → [vfio_pci_zap_and_down_write_memory_lock] → vfio_pci_zap_bars
 */
void vfio_pci_zap_and_down_write_memory_lock(struct vfio_pci_core_device *vdev)
{
	/* [한국어] **락을 먼저 잡는다.** 그래야 걷어내는 동안 다른 CPU 의 폴트 핸들러가
	 * 새 매핑을 만들지 못한다. 폴트 핸들러는 memory_lock 을 read 로 잡으므로
	 * write 를 쥔 동안 들어오지 못한다. */
	down_write(&vdev->memory_lock);
	/* [한국어] 그 다음에 매핑을 걷어낸다. 함수는 락을 쥔 채 반환하며, 호출자가 푼다. */
	vfio_pci_zap_bars(vdev);
}

/* [한국어]
 * vfio_pci_memory_lock_and_enable - 메모리 디코드를 임시로 켜고 원래 값을 돌려준다
 *
 * @vdev: 대상 디바이스.
 * @return: 켜기 **전**의 PCI_COMMAND 값. 호출자가 이 값을 그대로 들고 있다가
 *          vfio_pci_memory_unlock_and_restore 에 넘겨 되돌린다.
 *
 * 왜 필요한가: 커널이 BAR 나 ROM 을 잠깐 읽어야 할 때가 있다(ROM 유효성 검사,
 * IGD OpRegion 초기화 등). 그러려면 PCI_COMMAND 의 Memory Space Enable 이
 * 켜져 있어야 하는데, 사용자가 그것을 꺼 두었을 수 있다. 그렇다고 켠 채로
 * 두면 사용자가 기대한 상태와 달라진다. 그래서 **켜고, 쓰고, 정확히 원래대로
 * 되돌리는** 짝을 만든다.
 *
 * 동작 과정:
 *  1. memory_lock 을 write 로 잡는다. 이 구간 동안 사용자의 BAR 접근과
 *     폴트가 모두 막힌다 — 커널이 디코드를 만지는 동안 사용자가 끼어들면
 *     안 되기 때문이다.
 *  2. 현재 PCI_COMMAND 를 읽어 둔다. 이것이 반환값이다.
 *  3. Memory Space Enable 이 꺼져 있으면 켠다. 이미 켜져 있으면 쓰지 않는다 —
 *     불필요한 config 쓰기를 피한다.
 * **이 경로의 config 접근은 사용자 경로가 아니라 커널 자신의 접근이라
 * 바이트 단위 권한표를 거치지 않는다.** 중재는 사용자가 보내는 요청에만
 * 적용된다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 락을 잡은 채 반환한다.
 *
 * 호출자: vfio_pci_ioctl_get_region_info 의 ROM 분기,
 * drivers/vfio/pci/vfio_pci_igd.c 의 OpRegion 초기화, vfio_pci_rdwr.c 등.
 * 호출 대상: down_write, pci_read_config_word, pci_write_config_word.
 *
 * 에러 경로: config 접근 실패를 검사하지 않는다. 실패해도 락 상태는 정상이며,
 * 뒤이은 작업이 실패로 드러난다.
 *
 * 호출 체인:
 *   vfio_pci_ioctl_get_region_info(ROM) 또는 vfio_pci_igd.c
 *     → [vfio_pci_memory_lock_and_enable] → pci_write_config_word
 */
u16 vfio_pci_memory_lock_and_enable(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 켜기 전의 PCI_COMMAND 값을 담아 호출자에게 돌려줄 변수. */
	u16 cmd;

	/* [한국어] 이 구간 동안 사용자의 BAR 접근과 폴트를 모두 막는다. 커널이 디코드를
	 * 만지는 동안 사용자가 끼어들면 안 되기 때문이다. */
	down_write(&vdev->memory_lock);
	/* [한국어] 현재 명령 레지스터를 읽는다. **커널 자신의 config 접근이라 사용자
	 * 중재 경로(권한표)를 거치지 않는다.** */
	pci_read_config_word(vdev->pdev, PCI_COMMAND, &cmd);
	/* [한국어] 메모리 디코드가 이미 켜져 있으면 건드리지 않는다 — 불필요한 config
	 * 쓰기를 피한다. */
	if (!(cmd & PCI_COMMAND_MEMORY))
		/* [한국어] 꺼져 있으면 Memory Space Enable 비트만 더해 켠다. */
		pci_write_config_word(vdev->pdev, PCI_COMMAND,
				      /* [한국어] 다른 비트는 그대로 두고 이 비트만 세운다. */
				      cmd | PCI_COMMAND_MEMORY);

	/* [한국어] 켜기 전 값을 돌려준다. 호출자가 이 값을 그대로 복원 함수에 넘긴다. */
	return cmd;
}

/* [한국어]
 * vfio_pci_memory_unlock_and_restore - PCI_COMMAND 를 되돌리고 memory_lock 을 푼다
 *
 * @vdev: 대상 디바이스.
 * @cmd:  vfio_pci_memory_lock_and_enable 이 돌려준 원래 PCI_COMMAND 값.
 * @return: 없음.
 *
 * 왜 필요한가: 위 함수의 정확한 짝이다. 사용자가 보는 PCI_COMMAND 상태가
 * 커널의 일시적 조작 때문에 달라지면 안 된다.
 *
 * 동작 과정: 받은 값을 그대로 PCI_COMMAND 에 쓰고 up_write 로 락을 푼다.
 * **조건 없이 쓴다**는 점이 짝과 다르다 — 켤 때는 이미 켜져 있으면 건너뛰지만,
 * 되돌릴 때는 그 사이 다른 비트가 바뀌었을 수도 있으므로 통째로 복원한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 호출 시점에 memory_lock 이 write 로 잡혀
 * 있어야 한다.
 *
 * 호출자: vfio_pci_memory_lock_and_enable 을 부른 모든 곳.
 * 호출 대상: pci_write_config_word, up_write.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_ioctl_get_region_info(ROM) 또는 vfio_pci_igd.c
 *     → [vfio_pci_memory_unlock_and_restore]
 */
void vfio_pci_memory_unlock_and_restore(struct vfio_pci_core_device *vdev, u16 cmd)
{
	/* [한국어] 받은 값을 통째로 되돌린다. **조건 없이 쓴다** — 그 사이 다른 비트가
	 * 바뀌었을 수도 있으므로 스냅숏 전체를 복원한다. */
	pci_write_config_word(vdev->pdev, PCI_COMMAND, cmd);
	/* [한국어] 락을 놓는다. 이제 사용자의 BAR 접근이 다시 정상 동작한다. */
	up_write(&vdev->memory_lock);
}

/* [한국어]
 * vma_to_pfn - VMA 의 시작 지점이 가리키는 BAR 물리 페이지 번호를 계산한다
 *
 * @vma: 폴트가 난 VMA. vm_private_data 에 vdev 가, vm_pgoff 에 VFIO 파일
 *       오프셋(페이지 단위)이 들어 있다.
 * @return: 이 VMA 의 첫 페이지에 해당하는 물리 페이지 프레임 번호(PFN).
 *
 * 왜 필요한가: mmap 시점에는 아무것도 매핑하지 않고 vm_ops 만 걸어 두므로,
 * 폴트가 났을 때 "이 주소가 어느 BAR 의 어느 물리 페이지인가" 를 다시
 * 계산해야 한다. VFIO 의 파일 오프셋 인코딩을 되풀어 그 답을 내는 함수다.
 *
 * 동작 과정:
 *  1. vm_private_data 에서 vdev 를 꺼낸다. vfio_pci_core_mmap 이 심어 둔 값이다.
 *  2. vm_pgoff 의 상위 비트에서 region 번호(= BAR 번호)를 뽑는다.
 *     vm_pgoff 는 페이지 단위이므로 시프트 폭이 VFIO_PCI_OFFSET_SHIFT 에서
 *     PAGE_SHIFT 를 뺀 값이다.
 *  3. 같은 시프트 폭의 마스크로 하위 비트, 즉 BAR 안에서의 페이지 오프셋을 뽑는다.
 *  4. BAR 의 물리 시작 주소를 페이지 번호로 바꾸고 오프셋을 더한다.
 * **여기서 pci_resource_start 를 쓴다는 것이 곧 "사용자가 vconfig 에 써 둔
 * 가짜 BAR 값이 아니라 호스트가 실제로 배정한 물리 주소" 를 매핑한다는 뜻**이다.
 * 사용자가 config 로 BAR 를 옮겨도 매핑되는 물리 페이지는 바뀌지 않는다.
 *
 * 실행 컨텍스트: **페이지 폴트 문맥**. mmap_lock 을 쥔 채 들어오며 잠들면 안
 * 된다. 계산만 하므로 문제없다.
 *
 * 호출자: vfio_pci_mmap_huge_fault 하나뿐.
 * 호출 대상: pci_resource_start.
 *
 * 에러 경로: 없다. 인덱스 검증은 mmap 시점에 이미 끝났다.
 *
 * 호출 체인:
 *   페이지 폴트 → vfio_pci_mmap_huge_fault → [vma_to_pfn]
 */
static unsigned long vma_to_pfn(struct vm_area_struct *vma)
{
	/* [한국어] vfio_pci_core_mmap 이 심어 둔 디바이스 포인터. */
	struct vfio_pci_core_device *vdev = vma->vm_private_data;
	/* [한국어] vm_pgoff 는 **페이지 단위** 파일 오프셋이므로, 바이트 기준 시프트 폭
	 * (40)에서 PAGE_SHIFT 를 뺀 만큼만 밀어야 region 번호가 나온다. */
	int index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);
	/* [한국어] BAR 안에서의 페이지 오프셋. */
	u64 pgoff;

	/* [한국어] 같은 시프트 폭의 마스크로 하위 비트만 남긴다. */
	pgoff = vma->vm_pgoff &
		/* [한국어] 1 을 그만큼 밀고 1 을 빼면 하위 비트가 모두 1 인 마스크가 된다. */
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);

	/* [한국어] **호스트가 실제로 배정한 BAR 물리 시작 주소**를 페이지 번호로 바꾸고
	 * BAR 안 오프셋을 더한다. 사용자가 vconfig 에 써 둔 가짜 BAR 값이 아니라
	 * 실물 주소를 쓴다는 것이 중요하다 — 사용자가 config 로 BAR 를 옮겨도
	 * 매핑되는 물리 페이지는 바뀌지 않는다. */
	return (pci_resource_start(vdev->pdev, index) >> PAGE_SHIFT) + pgoff;
}

/* [한국어]
 * vfio_pci_vmf_insert_pfn - 폴트 지점에 BAR 물리 페이지를 꽂는다(게이트 포함)
 *
 * @vdev:  대상 디바이스.
 * @vmf:   폴트 정보. vma 와 폴트 주소가 들어 있다.
 * @pfn:   꽂을 물리 페이지 번호.
 * @order: 매핑 단위의 차수. 0 이면 일반 페이지, PMD_ORDER 나 PUD_ORDER 면
 *         거대 매핑.
 * @return: VM_FAULT_ 계열 결과. NOPAGE 계열이면 성공,
 *          VM_FAULT_SIGBUS 면 접근 거부, VM_FAULT_FALLBACK 이면 더 작은
 *          단위로 다시 시도하라는 뜻이다.
 *
 * 왜 필요한가: **BAR 접근의 마지막 관문이다.** 여기서 두 가지를 확인한 뒤에만
 * 물리 페이지를 사용자 주소 공간에 연결한다.
 *  (1) 장치가 저전력 진입 상태가 아닐 것 — 잠든 장치의 MMIO 는 응답하지 않는다.
 *  (2) PCI_COMMAND 의 Memory Space Enable 이 켜져 있을 것 — 디코드가 꺼진
 *      BAR 에 접근하면 마스터 어보트가 난다.
 * 둘 중 하나라도 아니면 SIGBUS 를 돌려 **프로세스에 신호를 보낸다**. 커널이
 * 죽는 대신 사용자 프로세스가 죽는다는 것이 이 설계의 요점이다.
 * vendor 드라이버(nvgrace-gpu 등)도 자기 폴트 핸들러에서 이 관문을 쓰라고
 * EXPORT 되어 있다.
 *
 * 동작 과정:
 *  1. lockdep_assert_held_read — **memory_lock 을 read 로 쥔 채** 불려야 한다.
 *     그래야 검사와 삽입 사이에 디코드가 꺼지는 경합이 없다.
 *  2. 두 게이트를 확인하고 실패면 SIGBUS.
 *  3. order 가 0 이면 일반 vmf_insert_pfn.
 *  4. PMD/PUD 거대 매핑은 아키텍처가 PFNMAP 거대 페이지를 지원할 때만 시도한다.
 *     지원하지 않으면 FALLBACK 을 돌려 상위가 더 작은 단위로 다시 오게 한다.
 *     두 번째 인자 false 는 "쓰기 가능으로 만들지 여부" 가 아니라 write 폴트
 *     여부 인자로, 여기서는 폴트 종류와 무관하게 매핑을 건다.
 *
 * 실행 컨텍스트: **페이지 폴트 문맥**. mmap_lock 과 memory_lock(read)을
 * 쥔 상태다. 잠들 수 없다.
 *
 * 호출자: 이 파일의 vfio_pci_mmap_huge_fault, 그리고
 * drivers/vfio/pci/nvgrace-gpu/main.c:315 부근의 vendor 폴트 핸들러.
 * 호출 대상: __vfio_pci_memory_enabled(vfio_pci_config.c:404),
 * vmf_insert_pfn, vmf_insert_pfn_pmd, vmf_insert_pfn_pud.
 *
 * 에러 경로: SIGBUS 와 FALLBACK 두 가지로 표현되며, 둘 다 정상적인 폴트
 * 처리 결과다. 예외를 던지거나 락을 놓지 않는다.
 *
 * 호출 체인:
 *   페이지 폴트 → vfio_pci_mmap_huge_fault → [vfio_pci_vmf_insert_pfn]
 *     → vmf_insert_pfn
 */
vm_fault_t vfio_pci_vmf_insert_pfn(struct vfio_pci_core_device *vdev,
				   struct vm_fault *vmf,
				   unsigned long pfn,
				   unsigned int order)
{
	/* [한국어] **memory_lock 을 read 로 쥔 채** 불려야 한다는 계약을 디버그 빌드에서
	 * 확인한다. 그래야 아래 두 게이트를 확인한 순간과 실제로 매핑을 꽂는
	 * 순간 사이에 디코드가 꺼지는 경합이 없다. */
	lockdep_assert_held_read(&vdev->memory_lock);

	/* [한국어] 두 게이트. 저전력 진입 중이면 장치가 MMIO 에 응답하지 않고, 메모리
	 * 디코드가 꺼져 있으면 접근이 마스터 어보트를 일으킨다. */
	if (vdev->pm_runtime_engaged || !__vfio_pci_memory_enabled(vdev))
		/* [한국어] **커널이 죽는 대신 사용자 프로세스에 SIGBUS 를 보낸다.** 이것이
		 * 이 설계의 요점이다 — 사용자가 잘못 접근해도 호스트는 살아남는다. */
		return VM_FAULT_SIGBUS;

	/* [한국어] 차수 0 = 일반 페이지. */
	if (!order)
		/* [한국어] 폴트 주소 한 페이지에 PFN 을 꽂는다. struct page 가 없는 MMIO 이므로
		 * _pfn 계열을 쓴다. */
		return vmf_insert_pfn(vmf->vma, vmf->address, pfn);

	/* [한국어] PMD 단위 거대 매핑은 아키텍처가 PFNMAP 거대 페이지를 지원할 때만 가능하다.
	 * IS_ENABLED 는 컴파일 시 상수라 미지원 아키텍처에서는 분기가 통째로 사라진다. */
	if (IS_ENABLED(CONFIG_ARCH_SUPPORTS_PMD_PFNMAP) && order == PMD_ORDER)
		/* [한국어] 2MB 단위로 한 번에 꽂는다. 세 번째 인자는 쓰기 폴트 여부이며 false 다. */
		return vmf_insert_pfn_pmd(vmf, pfn, false);

	/* [한국어] PUD 단위(보통 1GB) 거대 매핑. */
	if (IS_ENABLED(CONFIG_ARCH_SUPPORTS_PUD_PFNMAP) && order == PUD_ORDER)
		/* [한국어] 1GB 단위로 한 번에 꽂는다. */
		return vmf_insert_pfn_pud(vmf, pfn, false);

	/* [한국어] 요청한 차수를 지원하지 않는다. 상위가 더 작은 단위로 다시 부르게 한다. */
	return VM_FAULT_FALLBACK;
}
/* [한국어] vendor 가 자기 폴트 핸들러에서 이 게이트를 쓸 수 있게 내보낸다
 * (drivers/vfio/pci/nvgrace-gpu/main.c:315 부근이 그렇게 쓴다). */
EXPORT_SYMBOL_GPL(vfio_pci_vmf_insert_pfn);

/* [한국어]
 * vfio_pci_mmap_huge_fault - BAR 매핑의 폴트 핸들러(일반/거대 페이지 공용)
 *
 * @vmf:   폴트 정보.
 * @order: 이번에 시도할 매핑 차수. 0 이면 일반 페이지.
 * @return: VM_FAULT_ 계열 결과.
 *
 * 왜 필요한가: **BAR 는 mmap 시점이 아니라 폴트 시점에 매핑된다.** 그렇게
 * 미루는 이유가 둘이다. (1) 리셋이나 저전력 진입에서 매핑을 통째로 걷어낼 수
 * 있어야 하는데(vfio_pci_zap_bars), 미리 다 꽂아 두면 걷어낸 뒤 다시 꽂을
 * 방법이 없다. (2) 걷어낸 뒤에는 재매핑을 **거부할 수 있어야** 하는데,
 * 그 거부를 폴트 시점에 memory_lock 으로 판정한다. 즉 지연 매핑이 곧
 * 철회 가능한 매핑이다.
 *
 * 동작 과정:
 *  1. 이번 차수의 매핑이 걸릴 정렬된 시작 주소를 구한다.
 *  2. VMA 시작으로부터의 페이지 오프셋을 더해 이 주소의 PFN 을 계산한다.
 *  3. is_aligned_for_order(include/linux/vfio_pci_core.h:227)로 그 차수가
 *     실제로 가능한지 본다 — 주소가 VMA 안에 들어오고, 끝도 VMA 를 넘지
 *     않고, PFN 이 그 차수에 정렬돼 있어야 한다. 하나라도 아니면 초기값인
 *     FALLBACK 이 그대로 반환되어 상위가 더 작은 단위로 다시 부른다.
 *  4. 가능하면 scoped_guard 로 **memory_lock 을 read 로 잡고** 삽입한다.
 *     scoped_guard 는 블록을 벗어날 때 자동으로 락을 놓는 매크로라, 여러
 *     반환 경로에서 락을 흘릴 위험이 없다.
 *  5. 결과를 속도 제한 디버그 로그로 남긴다.
 *
 * 실행 컨텍스트: **페이지 폴트 문맥**. mmap_lock 을 쥐고 들어온다.
 * memory_lock 을 read 로만 잡는 것이 중요하다 — write 로 잡으면 리셋
 * 경로와 교착한다.
 *
 * 호출자: 커널 메모리 관리자. 아래 vfio_pci_mmap_ops 의 .huge_fault 에
 * 직접 등록되고, .fault 는 order=0 으로 감싼 vfio_pci_mmap_page_fault 다.
 * 호출 대상: vma_to_pfn, is_aligned_for_order, vfio_pci_vmf_insert_pfn.
 *
 * 에러 경로: 정렬 실패는 FALLBACK, 게이트 실패는 SIGBUS 로 나간다.
 * 어느 쪽이든 락은 scoped_guard 가 정확히 놓는다.
 *
 * 호출 체인:
 *   사용자의 BAR 접근 → 페이지 폴트 → mm 코어 → vm_ops->huge_fault
 *     = [vfio_pci_mmap_huge_fault] → vfio_pci_vmf_insert_pfn
 */
static vm_fault_t vfio_pci_mmap_huge_fault(struct vm_fault *vmf,
					   unsigned int order)
{
	/* [한국어] 폴트가 난 VMA. */
	struct vm_area_struct *vma = vmf->vma;
	/* [한국어] mmap 때 심어 둔 디바이스 포인터. */
	struct vfio_pci_core_device *vdev = vma->vm_private_data;
	/* [한국어] 이번 차수의 매핑이 시작될 정렬된 주소. PAGE_SIZE 를 차수만큼 밀어
	 * 그 크기의 마스크를 만들고 하위 비트를 지운다. */
	unsigned long addr = vmf->address & ~((PAGE_SIZE << order) - 1);
	/* [한국어] VMA 시작으로부터 그 주소까지의 페이지 수. */
	unsigned long pgoff = (addr - vma->vm_start) >> PAGE_SHIFT;
	/* [한국어] VMA 첫 페이지의 PFN 에 그 페이지 수를 더해 이 주소의 PFN 을 얻는다. */
	unsigned long pfn = vma_to_pfn(vma) + pgoff;
	/* [한국어] 기본값을 FALLBACK 으로 둔다. 아래 정렬 검사에 걸리면 이 값이 그대로
	 * 반환되어 상위가 더 작은 단위로 다시 부른다. */
	vm_fault_t ret = VM_FAULT_FALLBACK;

	/* [한국어] 이 차수가 실제로 가능한지 본다 — 시작 주소가 VMA 안이고, 끝도 VMA 를
	 * 넘지 않고, PFN 이 그 차수에 정렬돼 있어야 한다
	 * (include/linux/vfio_pci_core.h:227). */
	if (is_aligned_for_order(vma, addr, pfn, order)) {
		/* [한국어] **memory_lock 을 read 로 잡는다.** write 로 잡으면 리셋 경로와 교착한다.
		 * scoped_guard 는 블록을 벗어날 때 자동으로 락을 놓으므로 여러 반환
		 * 경로에서 락을 흘릴 위험이 없다. */
		scoped_guard(rwsem_read, &vdev->memory_lock)
			/* [한국어] 게이트를 통과하면 물리 페이지를 꽂는다. */
			ret = vfio_pci_vmf_insert_pfn(vdev, vmf, pfn, order);
	}

	/* [한국어] 결과를 속도 제한 디버그 로그로 남긴다. 폴트는 매우 잦으므로 제한이 필수다. */
	dev_dbg_ratelimited(&vdev->pdev->dev,
			   /* [한국어] 차수, BAR 번호, 페이지 오프셋, 결과 코드를 찍는다. */
			   "%s(,order = %d) BAR %ld page offset 0x%lx: 0x%x\n",
			    /* [한국어] 함수 이름과 차수. */
			    __func__, order,
			    /* [한국어] vm_pgoff 의 상위 비트에서 */
			    vma->vm_pgoff >>
				/* [한국어] region 번호(= BAR 번호)를 뽑아 찍는다. */
				(VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT),
			    /* [한국어] BAR 안 페이지 오프셋과 vm_fault_t 결과 코드. */
			    pgoff, (unsigned int)ret);

	/* [한국어] 폴트 결과를 mm 코어에 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_mmap_page_fault - 일반 페이지 폴트를 차수 0 으로 바꿔 넘기는 한 줄 어댑터
 *
 * @vmf: 폴트 정보.
 * @return: vfio_pci_mmap_huge_fault 의 결과.
 *
 * 왜 필요한가: vm_operations_struct 의 .fault 슬롯은 인자가 vmf 하나뿐이고
 * .huge_fault 는 order 를 더 받는다. 두 슬롯을 같은 구현으로 처리하려고
 * 시그니처만 맞춰 주는 어댑터가 필요하다. 차수 0 은 일반 페이지를 뜻한다.
 *
 * 동작 과정: order=0 으로 그대로 전달한다.
 *
 * 실행 컨텍스트: **페이지 폴트 문맥**.
 *
 * 호출자: 커널 메모리 관리자가 vm_ops->fault 로 부른다.
 * 호출 대상: vfio_pci_mmap_huge_fault.
 *
 * 에러 경로: 아래 함수의 결과를 그대로 전달한다.
 *
 * 호출 체인:
 *   페이지 폴트 → mm 코어 → vm_ops->fault = [vfio_pci_mmap_page_fault]
 *     → vfio_pci_mmap_huge_fault
 */
static vm_fault_t vfio_pci_mmap_page_fault(struct vm_fault *vmf)
{
	/* [한국어] 차수 0(일반 페이지)으로 그대로 넘긴다. */
	return vfio_pci_mmap_huge_fault(vmf, 0);
}

/* [한국어] BAR 매핑의 vm_operations. vfio_pci_core_mmap 이 vma->vm_ops 에 건다.
 * 설정자: 이 파일의 정적 초기화.
 * 읽는 자: 커널 메모리 관리자의 폴트 처리 경로.
 * 값 범위: fault 는 항상, huge_fault 는 아키텍처 지원 시에만 채워진다.
 * 동기화: const 라 읽기 전용이다. */
static const struct vm_operations_struct vfio_pci_mmap_ops = {
	/* [한국어] 일반 페이지 폴트 핸들러.
	 * 설정자: 정적 초기화.
	 * 읽는 자: mm 코어가 페이지 폴트마다 부른다.
	 * 값 범위: 차수 0 어댑터 고정.
	 * 동기화: mm 코어가 mmap_lock 을 쥔 채 부른다. */
	.fault = vfio_pci_mmap_page_fault,
/* [한국어] 거대 PFNMAP 을 지원하는 아키텍처에서만 huge_fault 슬롯이 의미가 있다. */
#ifdef CONFIG_ARCH_SUPPORTS_HUGE_PFNMAP
	/* [한국어] PMD/PUD 단위 폴트 핸들러. 같은 구현을 차수만 달리해 쓴다.
	 * 설정자: 정적 초기화.
	 * 읽는 자: mm 코어가 거대 매핑을 시도할 때.
	 * 값 범위: 아키텍처가 지원할 때만 채워지고, 아니면 슬롯 자체가 없다.
	 * 동기화: 위와 같다. */
	.huge_fault = vfio_pci_mmap_huge_fault,
#endif
};

/* [한국어]
 * vfio_pci_core_mmap - vfio_device_ops 의 mmap 슬롯: BAR 를 사용자 주소 공간에 걸 준비를 한다
 *
 * @core_vdev: 코어 쪽 디바이스.
 * @vma:       사용자가 요청한 매핑 구간. vm_pgoff 에 VFIO 파일 오프셋이 들어 있다.
 * @return: 0 성공(실제 매핑은 아직 없다). -EINVAL 이면 거절,
 *          그 밖은 barmap 확보 실패의 errno.
 *
 * 왜 필요한가: **BAR mmap 정책이 최종 집행되는 지점이자, 이 서브시스템에서
 * 하드웨어가 사용자에게 직통으로 열리는 유일한 통로다.** 여기를 통과하면
 * 그 뒤로는 CPU store 명령이 곧장 장치 레지스터를 때린다 — 커널이 개입하지
 * 않는다. SPDK 가 NVMe 도어벨을 쓰는 경로가 정확히 이것이다.
 *
 * 거절 규칙(차례로):
 *  1. region 번호가 범위를 넘으면 거절.
 *  2. VMA 가 뒤집혀 있으면 거절.
 *  3. **VM_SHARED 가 아니면 거절** — 사설(private) 매핑은 COW 를 의미하는데
 *     MMIO 에 COW 는 성립하지 않는다.
 *  4. vendor region 이면 그쪽 ops->mmap 에 넘긴다. 단 ops 와 mmap 슬롯이
 *     모두 있고 flags 에 MMAP 이 서 있을 때만 — **여기서는 슬롯 NULL 검사를
 *     한다**(vfio_pci_core_disable 의 release 호출과 대비된다).
 *  5. ROM 이상의 표준 region(ROM, VGA)은 거절. 그것들은 read/write 전용이다.
 *  6. bar_mmap_supported 가 아니면 거절. 그 판정은 vfio_pci_probe_mmaps 가
 *     이미 내려 두었다.
 *  7. 요청 구간이 BAR 크기를 넘으면 거절.
 *
 * 통과 후 하는 일:
 *  8. vfio_pci_core_setup_barmap 으로 region 을 요청한다. 원본 주석이 말하듯
 *     mmap 자체는 barmap 을 쓰지 않지만, **자원 요청(request_region)을 해 두어야
 *     다른 드라이버가 같은 BAR 를 가져가지 못한다.** barmap 이 그 요청 상태를
 *     추적한다.
 *  9. vm_private_data 에 vdev 를 심는다. 폴트 핸들러가 이것으로 vdev 를 찾는다.
 * 10. 페이지 보호 속성을 noncached 로, 그리고 decrypted 로 바꾼다.
 *     후자는 SEV/TDX 같은 메모리 암호화 환경에서 MMIO 는 암호화하지 않아야
 *     하기 때문이다.
 * 11. vm_flags 를 미리 확정한다. 원본 영어 주석이 VM_ALLOW_ANY_UNCACHED 의
 *     배경을 길게 설명한다 — ARM64 에서 KVM 2단계 매핑이 Normal-NC 를 쓸 수
 *     있게 해 주는 플래그이며, 그것이 안전한지는 플랫폼이 보장해야 한다.
 *     VM_IO 와 VM_PFNMAP 은 "이 매핑에는 struct page 가 없다" 를 뜻하고,
 *     VM_DONTEXPAND 와 VM_DONTDUMP 는 확장과 코어 덤프를 막는다.
 * 12. vm_ops 를 걸고 0 을 반환한다. **실제 매핑은 아직 하나도 없다.**
 *
 * 실행 컨텍스트: 프로세스 문맥. mmap_lock 을 write 로 쥔 상태로 들어온다.
 * memory_lock 은 여기서 잡지 않는다 — 매핑을 실제로 만들지 않기 때문이다.
 *
 * 호출자: vfio_main.c 의 파일 연산이 device->ops->mmap 으로 부른다
 * (vfio_pci.c:139).
 * 호출 대상: vfio_pci_core_setup_barmap(vfio_pci_rdwr.c:201),
 * pgprot_noncached, pgprot_decrypted, vm_flags_set.
 *
 * 에러 경로: 8단계 이전의 거절은 부작용이 없다. 8단계가 실패하면 그 오류를
 * 그대로 올려 보내며, 이미 성공한 barmap 은 vfio_pci_core_disable 이 정리한다.
 *
 * 호출 체인:
 *   사용자 mmap(2) → vfio_main.c 의 파일 연산 → ops->mmap
 *     = [vfio_pci_core_mmap] → vfio_pci_core_setup_barmap
 *   (그 뒤 첫 접근에서) 페이지 폴트 → vfio_pci_mmap_page_fault
 */
int vfio_pci_core_mmap(struct vfio_device *core_vdev, struct vm_area_struct *vma)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 사용자가 매핑하려는 region 번호. */
	unsigned int index;
	/* [한국어] BAR 의 페이지 정렬 길이, 요청 길이, BAR 안 페이지 오프셋, 요청 시작 위치. */
	u64 phys_len, req_len, pgoff, req_start;
	/* [한국어] barmap 확보의 반환값. */
	int ret;

	/* [한국어] vm_pgoff 는 페이지 단위이므로 시프트 폭에서 PAGE_SHIFT 를 뺀다. */
	index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);

	/* [한국어] 존재하지 않는 region 번호. */
	if (index >= VFIO_PCI_NUM_REGIONS + vdev->num_regions)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] VMA 가 뒤집혀 있으면 길이 계산이 음수가 된다. */
	if (vma->vm_end < vma->vm_start)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] **사설(private) 매핑은 거절한다.** 사설 매핑은 쓰기 시 복사(COW)를
	 * 의미하는데 MMIO 에 COW 는 성립하지 않는다. */
	if ((vma->vm_flags & VM_SHARED) == 0)
		/* [한국어] -EINVAL. */
		return -EINVAL;
	/* [한국어] vendor 가 등록한 추가 region 이면. */
	if (index >= VFIO_PCI_NUM_REGIONS) {
		/* [한국어] vendor 배열 안의 첨자로 바꾼다. */
		int regnum = index - VFIO_PCI_NUM_REGIONS;
		/* [한국어] 그 region 서술자. */
		struct vfio_pci_region *region = vdev->region + regnum;

		/* [한국어] **여기서는 ops 와 mmap 슬롯의 NULL 을 모두 검사한다** — mmap 은 regops 의
		 * 선택 슬롯이라는 규약이 이 한 줄에 담겨 있다. */
		if (region->ops && region->ops->mmap &&
		    /* [한국어] 등록 때 MMAP 플래그를 세운 region 만 허용한다. 사용자에게 보고한 것과
		     * 실제 허용이 일치해야 한다. */
		    (region->flags & VFIO_REGION_INFO_FLAG_MMAP))
			/* [한국어] vendor 에게 넘긴다. 그쪽이 자기 방식으로 매핑을 만든다. */
			return region->ops->mmap(vdev, region, vma);
		/* [한국어] 조건에 맞지 않으면 거절한다. */
		return -EINVAL;
	}
	/* [한국어] ROM 과 VGA 는 mmap 대상이 아니다. ROM 은 디코드를 켜야 읽히고, VGA 는
	 * I/O 포트가 섞여 있어 단순 메모리 매핑으로 표현할 수 없다. */
	if (index >= VFIO_PCI_ROM_REGION_INDEX)
		/* [한국어] -EINVAL. GET_REGION_INFO 가 그 둘에 MMAP 플래그를 주지 않은 것과 일관된다. */
		return -EINVAL;
	/* [한국어] vfio_pci_probe_mmaps 가 판정해 둔 결과를 확인한다. 페이지보다 작고
	 * 정렬도 안 된 BAR, I/O 포트 BAR, non_mappable_bars 장치가 여기서 걸린다. */
	if (!vdev->bar_mmap_supported[index])
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] BAR 의 실제 길이를 페이지 크기로 올림한다. 매핑 단위가 페이지이므로
	 * 비교 기준도 페이지 단위여야 한다. */
	phys_len = PAGE_ALIGN(pci_resource_len(pdev, index));
	/* [한국어] 사용자가 요청한 매핑 길이. */
	req_len = vma->vm_end - vma->vm_start;
	/* [한국어] vm_pgoff 의 하위 비트에서 */
	pgoff = vma->vm_pgoff &
		/* [한국어] BAR 안에서의 페이지 오프셋을 뽑는다. */
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);
	/* [한국어] 그것을 바이트 오프셋으로 바꾼다. */
	req_start = pgoff << PAGE_SHIFT;

	/* [한국어] 요청 구간이 BAR 를 넘어가면 남의 물리 주소를 매핑하게 된다.
	 * **이 검사가 없으면 사용자가 임의 물리 메모리를 얻는다.** */
	if (req_start + req_len > phys_len)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/*
	 * Even though we don't make use of the barmap for the mmap,
	 * we need to request the region and the barmap tracks that.
	 */
	/* [한국어] 원본 주석대로 mmap 자체는 barmap 을 쓰지 않지만, **자원 요청을 해 두어야
	 * 다른 드라이버가 같은 BAR 를 가져가지 못한다.** barmap 이 그 요청 상태를
	 * 추적한다(vfio_pci_rdwr.c:201). */
	ret = vfio_pci_core_setup_barmap(vdev, index);
	/* [한국어] 요청 실패(이미 남이 쓰고 있음 또는 메모리 부족). */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. */
		return ret;

	/* [한국어] 폴트 핸들러가 vdev 를 찾을 수 있도록 심어 둔다. */
	vma->vm_private_data = vdev;
	/* [한국어] **MMIO 는 캐시하면 안 된다.** 캐시된 쓰기는 순서와 시점이 보장되지 않아
	 * 장치 레지스터 프로토콜을 깨뜨린다. */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	/* [한국어] 메모리 암호화 환경(SEV, TDX)에서 MMIO 는 암호화하지 않는다.
	 * 장치는 게스트의 암호화 키를 모르기 때문이다. */
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);

	/*
	 * Set vm_flags now, they should not be changed in the fault handler.
	 * We want the same flags and page protection (decrypted above) as
	 * io_remap_pfn_range() would set.
	 *
	 * VM_ALLOW_ANY_UNCACHED: The VMA flag is implemented for ARM64,
	 * allowing KVM stage 2 device mapping attributes to use Normal-NC
	 * rather than DEVICE_nGnRE, which allows guest mappings
	 * supporting write-combining attributes (WC). ARM does not
	 * architecturally guarantee this is safe, and indeed some MMIO
	 * regions like the GICv2 VCPU interface can trigger uncontained
	 * faults if Normal-NC is used.
	 *
	 * To safely use VFIO in KVM the platform must guarantee full
	 * safety in the guest where no action taken against a MMIO
	 * mapping can trigger an uncontained failure. The assumption is
	 * that most VFIO PCI platforms support this for both mapping types,
	 * at least in common flows, based on some expectations of how
	 * PCI IP is integrated. Hence VM_ALLOW_ANY_UNCACHED is set in
	 * the VMA flags.
	 */
	/* [한국어] **여기서 vm_flags 를 확정한다.** 폴트 핸들러에서는 바꾸면 안 된다는 것이
	 * 원본 주석의 요지다. VM_IO 와 VM_PFNMAP 은 "이 매핑에는 struct page 가
	 * 없다" 를 뜻해 페이지 참조 계수 조작을 막는다. */
	vm_flags_set(vma, VM_ALLOW_ANY_UNCACHED | VM_IO | VM_PFNMAP |
			/* [한국어] VM_DONTEXPAND 는 mremap 확장을, VM_DONTDUMP 는 코어 덤프 포함을 막는다.
			 * MMIO 를 덤프하면 장치 상태를 바꿔 버릴 수 있다. */
			VM_DONTEXPAND | VM_DONTDUMP);
	/* [한국어] **vm_ops 만 걸고 끝낸다. 실제 매핑은 아직 하나도 없다.**
	 * 첫 접근에서 폴트가 나고 그때 memory_lock 을 read 로 잡고 꽂는다.
	 * 이 지연이 곧 철회 가능성(vfio_pci_zap_bars)을 만든다. */
	vma->vm_ops = &vfio_pci_mmap_ops;

	/* [한국어] mmap 준비 완료. */
	return 0;
}
/* [한국어] vendor 가 자기 mmap 안에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_mmap);

/* [한국어]
 * vfio_pci_core_request - vfio_device_ops 의 request 슬롯: "장치를 놓아 달라" 를 사용자에게 전한다
 *
 * @core_vdev: 코어 쪽 디바이스.
 * @count:     이번이 몇 번째 요청인지. 0 부터 시작해 코어가 늘려 가며 부른다.
 * @return: 없음.
 *
 * 왜 필요한가: 호스트가 장치를 되찾아야 할 때가 있다 — 드라이버 언바인드,
 * 장치 제거, 모듈 언로드. 그런데 사용자 공간이 fd 를 들고 있으면 커널은
 * 강제로 뺏을 수 없다(뺏으면 사용자 매핑이 유령이 된다). 그래서 **정중히
 * 요청하고 기다린다**. 그 요청 통로가 req_trigger eventfd 다.
 *
 * 동작 과정:
 *  1. rcu_read_lock 아래에서 req_trigger 를 읽는다. 등록/해제는 igate 아래에서
 *     일어나지만 이 경로는 igate 를 잡지 않으므로 RCU 로만 보호한다.
 *  2. eventfd 가 있으면 열 번에 한 번씩 로그를 남기고 eventfd 를 울린다.
 *     매번 로그를 남기면 코어가 반복 호출하는 동안 로그가 폭주한다.
 *  3. eventfd 가 없고 **첫 호출이면** 경고를 남긴다. 알릴 방법이 없으니
 *     사용자가 스스로 놓을 때까지 커널이 막힌다는 뜻이다. 첫 호출에만
 *     경고하는 이유도 로그 폭주 방지다.
 *
 * 실행 컨텍스트: 프로세스 문맥이지만 **rcu_read_lock 구간 안에서 eventfd 를
 * 울린다** — 그래서 잠들 수 없다. eventfd_signal 은 잠들지 않는 함수다.
 *
 * 호출자: drivers/vfio/vfio_main.c 의 vfio_unregister_group_dev 가 사용자가
 * fd 를 놓기를 기다리며 반복해서 부른다.
 * 호출 대상: rcu_dereference, eventfd_signal.
 *
 * 에러 경로: 없다. 알릴 수 없으면 경고만 남긴다.
 *
 * 호출 체인:
 *   드라이버 언바인드 → vfio_main.c:vfio_unregister_group_dev
 *     → ops->request = [vfio_pci_core_request] → eventfd_signal
 */
void vfio_pci_core_request(struct vfio_device *core_vdev, unsigned int count)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	/* [한국어] 로그를 찍을 때 쓸 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] req_trigger 슬롯에서 읽어 올 래퍼. */
	struct vfio_pci_eventfd *eventfd;

	/* [한국어] **igate 를 잡지 않고 RCU 로만 보호한다.** 이 경로는 잠들 수 없는 곳에서
	 * 불릴 수 있고, 슬롯 교체 쪽이 유예 해제를 쓰므로 이것으로 충분하다. */
	rcu_read_lock();
	/* [한국어] 슬롯을 읽는다. rcu_dereference 가 의존 순서 배리어를 넣어, 포인터를
	 * 따라간 뒤 읽는 내용이 제대로 보이게 한다. */
	eventfd = rcu_dereference(vdev->req_trigger);
	/* [한국어] 등록된 알림 채널이 있으면. */
	if (eventfd) {
		/* [한국어] 열 번에 한 번만 로그를 남긴다. 코어가 반복해서 부르므로 매번 찍으면
		 * 로그가 폭주한다. */
		if (!(count % 10))
			/* [한국어] 관리자에게 "사용자에게 반납을 요청 중" 임을 알린다. */
			pci_notice_ratelimited(pdev,
				"Relaying device request to user (#%u)\n",
				count);
		/* [한국어] eventfd 를 울려 사용자에게 반납을 요청한다. 이 함수는 잠들지 않으므로
		 * RCU 구간 안에서 안전하다. */
		eventfd_signal(eventfd->ctx);
	/* [한국어] 채널이 없고 **첫 호출**인 경우. */
	} else if (count == 0) {
		/* [한국어] 알릴 방법이 없으니 사용자가 스스로 놓을 때까지 커널이 막힌다는 사실을
		 * 경고한다. 첫 호출에만 찍는 이유도 로그 폭주 방지다. */
		pci_warn(pdev,
			"No device request channel registered, blocked until released by user\n");
	}
	/* [한국어] RCU 구간을 닫는다. */
	rcu_read_unlock();
}
/* [한국어] vendor 가 자기 request 안에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_request);

/* [한국어]
 * vfio_pci_core_match_token_uuid - SR-IOV PF/VF 사이의 신뢰 협약을 UUID 로 검사한다
 *
 * @core_vdev: 열려는 디바이스(PF 일 수도 VF 일 수도 있다).
 * @uuid:      사용자가 제시한 토큰. 제시하지 않았으면 NULL.
 * @return: 0 이면 열어도 좋다. -EACCES 는 토큰이 없거나 틀림,
 *          -EINVAL 은 토큰을 줄 자리가 아닌데 준 경우.
 *
 * 왜 필요한가: 원본 영어 주석이 배경을 자세히 설명한다. PF 와 VF 사이에는
 * 어느 정도의 신뢰가 항상 존재한다 — 최소한 PF 가 SR-IOV capability 를
 * 쥐고 있어 리셋으로 VF 를 방해할 수 있고, 흔히는 VF 를 지나가는 데이터에
 * 접근하거나 서비스를 거부할 수도 있다. 그러므로 PF 를 사용자 공간에 넘긴
 * 상태에서 VF 를 **다른** 사용자에게 주면, VF 사용자가 모르는 사이에 낯선
 * 사용자를 신뢰하게 된다. VFIO 는 그 신뢰를 공유 UUID 로 명시적 opt-in 하게
 * 만든다. 사용자 공간에서는 장치 이름 뒤에 붙여 표현한다:
 * 예: "0000:04:10.0 vf_token=bd8d9d2b-5a5f-4f5a-a211-f591514ba1f3" 형태
 *
 * 세 갈래 판정:
 *  [VF 를 여는 경우]
 *    - PF 가 vfio-pci 가 아니면(sriov_pf_core_dev 가 NULL) 신뢰 문제가 없다.
 *      토큰을 안 줬으면 통과, 줬으면 줄 자리가 아니므로 -EINVAL.
 *    - PF 도 vfio-pci 인데 토큰을 안 줬으면 -EACCES.
 *    - PF 의 토큰과 대조해 다르면 -EACCES.
 *  [PF 를 여는 경우(vf_token 이 있음)]
 *    - 쓰는 VF 사용자가 있으면(users != 0) 토큰을 **맞혀야** 한다.
 *      없으면 -EACCES, 틀리면 -EACCES.
 *    - 쓰는 VF 사용자가 없고 토큰을 줬으면 그 값으로 **설정**한다.
 *      즉 PF 를 먼저 여는 쪽이 토큰의 주인이 된다.
 *  [PF 도 VF 도 아닌 경우]
 *    - 토큰을 줬으면 -EINVAL(줄 자리가 아니다), 안 줬으면 통과.
 *
 * 실행 컨텍스트: 프로세스 문맥. PF 의 vf_token->lock 을 짧게 잡는다.
 * **VF 를 여는 스레드가 PF 객체의 락을 잡는다**는 점이 특이하다.
 *
 * 호출자: 두 세대의 열기 경로 — 1세대는 이 파일의 vfio_pci_core_match 가
 * 파싱한 UUID 를 들고 ops 를 거쳐 다시 이 함수로 오고, 2세대는
 * drivers/vfio/device_cdev.c 의 vfio_df_check_token 이 직접 부른다.
 * 호출 대상: uuid_equal, uuid_copy, pci_info_ratelimited.
 *
 * 에러 경로: 모든 실패가 속도 제한 로그를 남긴다. 락을 잡은 채 반환하는
 * 경로는 없다.
 *
 * 호출 체인:
 *   1세대: group open → group.c:vfio_device_get_from_name → ops->match
 *          = vfio_pci_core_match → ops->match_token_uuid
 *          = [vfio_pci_core_match_token_uuid]
 *   2세대: cdev BIND_IOMMUFD → device_cdev.c:vfio_df_check_token
 *          → ops->match_token_uuid = [vfio_pci_core_match_token_uuid]
 */
int vfio_pci_core_match_token_uuid(struct vfio_device *core_vdev,
				   const uuid_t *uuid)

{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	/*
	 * There's always some degree of trust or collaboration between SR-IOV
	 * PF and VFs, even if just that the PF hosts the SR-IOV capability and
	 * can disrupt VFs with a reset, but often the PF has more explicit
	 * access to deny service to the VF or access data passed through the
	 * VF.  We therefore require an opt-in via a shared VF token (UUID) to
	 * represent this trust.  This both prevents that a VF driver might
	 * assume the PF driver is a trusted, in-kernel driver, and also that
	 * a PF driver might be replaced with a rogue driver, unknown to in-use
	 * VF drivers.
	 *
	 * Therefore when presented with a VF, if the PF is a vfio device and
	 * it is bound to the vfio-pci driver, the user needs to provide a VF
	 * token to access the device, in the form of appending a vf_token to
	 * the device name, for example:
	 *
	 * "0000:04:10.0 vf_token=bd8d9d2b-5a5f-4f5a-a211-f591514ba1f3"
	 *
	 * When presented with a PF which has VFs in use, the user must also
	 * provide the current VF token to prove collaboration with existing
	 * VF users.  If VFs are not in use, the VF token provided for the PF
	 * device will act to set the VF token.
	 *
	 * If the VF token is provided but unused, an error is generated.
	 */
	/* [한국어] 이 디바이스가 VF 인 경우. */
	if (vdev->pdev->is_virtfn) {
		/* [한국어] vfio_pci_vf_init 이 찾아 둔 PF 객체. NULL 이면 PF 가 vfio-pci 가 아니다. */
		struct vfio_pci_core_device *pf_vdev = vdev->sriov_pf_core_dev;
		/* [한국어] 토큰 대조 결과. */
		bool match;

		/* [한국어] PF 가 vfio-pci 가 아니다 = PF 는 호스트 드라이버가 쥐고 있다.
		 * 그 경우 신뢰 협약이 성립하지 않으므로 토큰도 필요 없다. */
		if (!pf_vdev) {
			/* [한국어] 토큰을 주지 않았으면 정상이다. */
			if (!uuid)
				/* [한국어] 통과. 이 VF 는 토큰 없이 열 수 있다. */
				return 0; /* PF is not vfio-pci, no VF token */

			/* [한국어] 토큰을 줬는데 줄 자리가 아니다 — 사용자가 상황을 오해한 것이다. */
			pci_info_ratelimited(vdev->pdev,
				"VF token incorrectly provided, PF not bound to vfio-pci\n");
			/* [한국어] -EINVAL. 조용히 무시하지 않고 오류로 알려 준다. */
			return -EINVAL;
		}

		/* [한국어] PF 도 vfio-pci 인데 토큰을 주지 않았다. */
		if (!uuid) {
			/* [한국어] 무엇이 필요한지 알린다. */
			pci_info_ratelimited(vdev->pdev,
				"VF token required to access device\n");
			/* [한국어] -EACCES. */
			return -EACCES;
		}

		/* [한국어] PF 의 토큰 락을 잡는다. **VF 를 여는 스레드가 PF 객체의 락을 잡는다.** */
		mutex_lock(&pf_vdev->vf_token->lock);
		/* [한국어] UUID 를 상수 시간 비교가 아닌 단순 비교로 대조한다. */
		match = uuid_equal(uuid, &pf_vdev->vf_token->uuid);
		/* [한국어] 락을 놓는다. 결과는 지역 변수에 남아 있다. */
		mutex_unlock(&pf_vdev->vf_token->lock);

		/* [한국어] 토큰이 다르다. */
		if (!match) {
			/* [한국어] 무엇이 틀렸는지 알린다. */
			pci_info_ratelimited(vdev->pdev,
				"Incorrect VF token provided for device\n");
			/* [한국어] -EACCES. */
			return -EACCES;
		}
	/* [한국어] VF 가 아니면서 vf_token 이 있다 = SR-IOV PF 인 경우. */
	} else if (vdev->vf_token) {
		/* [한국어] 토큰 락을 잡는다. 아래 users 검사와 uuid 조작을 한 임계 구역에 묶는다. */
		mutex_lock(&vdev->vf_token->lock);
		/* [한국어] 지금 이 PF 의 VF 를 쓰는 사용자가 있는가. */
		if (vdev->vf_token->users) {
			/* [한국어] 있는데 토큰을 주지 않았다 — 기존 VF 사용자와의 협업을 증명하지 못했다. */
			if (!uuid) {
				/* [한국어] 반환 전에 락을 반드시 놓는다. */
				mutex_unlock(&vdev->vf_token->lock);
				/* [한국어] 무엇이 필요한지 알린다. */
				pci_info_ratelimited(vdev->pdev,
					"VF token required to access device\n");
				/* [한국어] -EACCES. */
				return -EACCES;
			}

			/* [한국어] 제시한 토큰이 현재 값과 다르다. */
			if (!uuid_equal(uuid, &vdev->vf_token->uuid)) {
				/* [한국어] 반환 전에 락을 놓는다. */
				mutex_unlock(&vdev->vf_token->lock);
				/* [한국어] 무엇이 틀렸는지 알린다. */
				pci_info_ratelimited(vdev->pdev,
					"Incorrect VF token provided for device\n");
				/* [한국어] -EACCES. */
				return -EACCES;
			}
		/* [한국어] VF 사용자가 없고 토큰을 제시했다면. */
		} else if (uuid) {
			/* [한국어] **그 값으로 토큰을 설정한다.** PF 를 먼저 여는 쪽이 토큰의 주인이 된다.
			 * VF 사용자가 없을 때만 가능하므로 기존 사용자를 배신하지 못한다. */
			uuid_copy(&vdev->vf_token->uuid, uuid);
		}

		/* [한국어] 정상 경로에서 락을 놓는다. */
		mutex_unlock(&vdev->vf_token->lock);
	/* [한국어] PF 도 VF 도 아닌데 토큰을 줬다. */
	} else if (uuid) {
		/* [한국어] 줄 자리가 아님을 알린다. */
		pci_info_ratelimited(vdev->pdev,
			"VF token incorrectly provided, not a PF or VF\n");
		/* [한국어] -EINVAL. */
		return -EINVAL;
	}

	/* [한국어] 모든 검사를 통과했다. 이 디바이스를 열어도 좋다. */
	return 0;
}
/* [한국어] 두 세대의 열기 경로가 ops->match_token_uuid 로 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_match_token_uuid);

/* [한국어] 1세대 문법에서 장치 이름 뒤에 붙이는 인자 접두사.
 * "0000:04:10.0 vf_token=<UUID>" 형태로 쓰인다. 아래 파서가 이 문자열의
 * 길이만큼 건너뛰어 UUID 를 읽는다. */
#define VF_TOKEN_ARG "vf_token="

/* [한국어]
 * vfio_pci_core_match - vfio_device_ops 의 match 슬롯: 이름과 vf_token 인자를 파싱한다
 *
 * @core_vdev: 후보 디바이스.
 * @buf:       사용자가 준 문자열. "0000:04:10.0" 또는
 *             "0000:04:10.0 vf_token=<UUID>" 형태다.
 * @return: 1 이면 이 디바이스가 맞다. 0 이면 다른 디바이스다.
 *          음수면 이름은 맞지만 인자가 잘못됐거나 토큰 검사에 실패했다.
 *
 * 왜 필요한가: 1세대(group) ABI 의 VFIO_GROUP_GET_DEVICE_FD 는 장치를
 * **문자열 이름으로** 지목한다. 그 문자열에 vf_token 을 얹어 보내는 것이
 * 1세대의 토큰 전달 문법이다(2세대는 BIND_IOMMUFD 구조체의 필드로 보낸다).
 * 그러므로 이름 비교와 인자 파싱을 vendor 가 직접 해야 하고, 그 구현이
 * 이 함수다.
 *
 * 동작 과정:
 *  1. PCI 주소 문자열이 앞부분과 일치하지 않으면 0(다른 장치).
 *  2. 이름 뒤에 더 있으면 반드시 공백으로 이어져야 한다. 아니면 0 —
 *     "0000:04:10.0x" 같은 다른 장치 이름을 잘못 매칭하지 않기 위해서다.
 *  3. 남은 문자열을 공백으로 끊어 가며 읽는다. "vf_token=" 으로 시작하는
 *     항목 하나만 허용하고, 그 뒤 문자열이 UUID 길이 이상인지 확인한 뒤
 *     uuid_parse 로 파싱한다. 알 수 없거나 중복된 옵션은 -EINVAL.
 *  4. 파싱한 토큰(또는 NULL)을 들고 ops->match_token_uuid 를 부른다.
 *  5. 검사를 통과하면 1(일치)을 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. group->device_lock 을 쥔 채 불린다
 * (group.c 의 vfio_device_get_from_name 이 목록을 순회하는 중이다).
 *
 * 호출자: drivers/vfio/group.c 의 vfio_device_get_from_name 이
 * it->ops->match 로 부른다.
 * 호출 대상: pci_name, strncmp, uuid_parse, core_vdev->ops->match_token_uuid.
 *
 * 에러 경로: 파싱 오류와 토큰 검사 실패 모두 음수로 나가며, group.c 는
 * 그것을 곧바로 사용자에게 돌려준다(순회를 멈춘다).
 *
 * [상류 코드 관찰] 4단계는 ops->match_token_uuid 를 **NULL 검사 없이** 부른다.
 * include/linux/vfio.h:109 는 그 슬롯을 "Optional" 이라고 명시하고,
 * drivers/vfio/device_cdev.c 의 vfio_df_check_token 은 부르기 전에 NULL 을
 * 확인한다. 즉 같은 슬롯을 두 호출자가 다르게 취급한다. 다만 이 트리에서
 * .match 에 이 함수를 꽂은 ops 표는 열세 개인데(vfio_pci.c:141,
 * ism/main.c:347, virtio/main.c:97/119/141, xe/main.c:537,
 * nvgrace-gpu/main.c:946/971, pds/vfio_dev.c:204, qat/main.c:612,
 * mlx5/main.c:1399, hisilicon/hisi_acc_vfio_pci.c:1627/1648) 전부
 * .match_token_uuid 도 함께 채우므로 실제로 NULL 이 되는 경로는 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   사용자 VFIO_GROUP_GET_DEVICE_FD → group.c:vfio_device_get_from_name
 *     → ops->match = [vfio_pci_core_match] → ops->match_token_uuid
 */
int vfio_pci_core_match(struct vfio_device *core_vdev, char *buf)
{
	/* [한국어] 코어 객체에서 PCI 객체를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	/* [한국어] vf_token 인자를 만났는지 표시. 중복 지정을 막는 데도 쓰인다. */
	bool vf_token = false;
	/* [한국어] 파싱한 UUID 를 담을 자리. */
	uuid_t uuid;
	/* [한국어] 파싱과 검사의 반환값. */
	int ret;

	/* [한국어] PCI 주소 문자열("0000:04:10.0")이 앞부분과 일치하는지 본다.
	 * pci_name 은 pci_dev 의 이름 문자열을 돌려준다. */
	if (strncmp(pci_name(vdev->pdev), buf, strlen(pci_name(vdev->pdev))))
		/* [한국어] 다른 장치다. group.c 의 순회가 다음 후보로 넘어간다. */
		return 0; /* No match */

	/* [한국어] 이름 뒤에 더 있으면 인자를 파싱해야 한다. */
	if (strlen(buf) > strlen(pci_name(vdev->pdev))) {
		/* [한국어] 이름 길이만큼 건너뛴다. */
		buf += strlen(pci_name(vdev->pdev));

		/* [한국어] 이름 바로 뒤가 공백이 아니면 사실 다른 장치 이름이다.
		 * 예컨대 "0000:04:10.0" 로 시작하는 더 긴 이름을 잘못 매칭하지 않게 한다. */
		if (*buf != ' ')
			/* [한국어] 다른 장치로 본다. */
			return 0; /* No match: non-whitespace after name */

		/* [한국어] 남은 문자열을 끝까지 읽는다. */
		while (*buf) {
			/* [한국어] 공백은 인자 구분자다. */
			if (*buf == ' ') {
				/* [한국어] 건너뛴다. */
				buf++;
				/* [한국어] 다음 문자로. */
				continue;
			/* [한국어] 공백 처리 끝. */
			}

			/* [한국어] 아직 토큰을 못 봤고 이 인자가 "vf_token=" 으로 시작하는가.
			 * vf_token 검사가 앞에 있어 **같은 인자를 두 번 주면 아래 else 로 떨어져
			 * -EINVAL 이 된다.** */
			if (!vf_token && !strncmp(buf, VF_TOKEN_ARG,
						  /* [한국어] 접두사 길이만큼 비교한다. */
						  strlen(VF_TOKEN_ARG))) {
				/* [한국어] 접두사를 건너뛰어 UUID 문자열 앞에 선다. */
				buf += strlen(VF_TOKEN_ARG);

				/* [한국어] 남은 길이가 UUID 표준 표기(36자)보다 짧으면 파싱할 수 없다. */
				if (strlen(buf) < UUID_STRING_LEN)
					/* [한국어] -EINVAL. */
					return -EINVAL;

				/* [한국어] "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" 형식을 128비트 값으로 바꾼다. */
				ret = uuid_parse(buf, &uuid);
				/* [한국어] 형식 오류. */
				if (ret)
					/* [한국어] 그대로 올려 보낸다. */
					return ret;

				/* [한국어] 토큰을 받았음을 표시한다. 이후 같은 인자가 또 오면 else 로 간다. */
				vf_token = true;
				/* [한국어] UUID 문자열 길이만큼 건너뛴다. */
				buf += UUID_STRING_LEN;
			/* [한국어] 알 수 없거나 중복된 인자. */
			} else {
				/* Unknown/duplicate option */
				/* [한국어] -EINVAL. 조용히 무시하지 않는다 — 사용자가 의도한 설정이 적용되지
				 * 않은 채 장치가 열리면 더 위험하다. */
				return -EINVAL;
			}
		}
	}

	/* [한국어] 파싱한 토큰(없으면 NULL)을 들고 소유 검사로 넘어간다.
	 * [상류 코드 관찰] 이 슬롯의 NULL 을 검사하지 않는다.
	 * include/linux/vfio.h:109 는 match_token_uuid 를 "Optional" 로 명시하고
	 * drivers/vfio/device_cdev.c 의 vfio_df_check_token 은 부르기 전에 NULL 을
	 * 확인한다. 다만 이 트리에서 .match 에 이 함수를 꽂은 열세 개의 ops 표가
	 * 모두 .match_token_uuid 도 함께 채우므로 실제로 NULL 이 되는 경로는 없다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	ret = core_vdev->ops->match_token_uuid(core_vdev,
					       /* [한국어] 토큰을 받았으면 그 주소를, 아니면 NULL 을 넘긴다. */
					       vf_token ? &uuid : NULL);
	/* [한국어] 소유 검사 실패. */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. group.c 는 이 값을 사용자에게 돌려주고 순회를 멈춘다. */
		return ret;

	/* [한국어] 이름도 맞고 토큰도 맞다. 1 은 "이 디바이스다" 를 뜻한다. */
	return 1; /* Match */
}
/* [한국어] vendor 가 자기 match 안에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_match);

/* [한국어]
 * vfio_pci_bus_notifier - PCI 버스 알림을 받아 새로 나타나는 VF 를 가로챈다
 *
 * @nb:     알림 블록. container_of 로 vdev 를 되찾는다.
 * @action: 알림 종류. BUS_NOTIFY_ADD_DEVICE 와 BUS_NOTIFY_BOUND_DRIVER 만 본다.
 * @data:   알림 대상 struct device.
 * @return: 항상 0(NOTIFY_DONE 과 같은 값). 다른 알림 수신자를 막지 않는다.
 *
 * 왜 필요한가: PF 를 vfio-pci 로 열어 둔 사용자가 SR-IOV 를 켜면 VF 들이
 * 새로 생긴다. 그 VF 가 호스트의 일반 드라이버에 붙어 버리면, PF 사용자와
 * 호스트 드라이버가 같은 하드웨어를 나눠 쓰게 되어 위험하다. 그래서
 * **VF 가 등록되는 바로 그 순간에 driver_override 를 심어** 같은 vfio 드라이버로
 * 가게 만든다. driver_override 가 바인딩을 어떻게 강제하는지는 이 트리의
 * drivers/pci/pci-driver.c:362~404 와 435~437 에 이미 주석돼 있다.
 *
 * 동작 과정:
 *  1. ADD_DEVICE 알림이고, 그 장치가 VF 이고, 그 VF 의 PF 가 나 자신이면
 *     device_set_driver_override 로 내 드라이버 이름을 심는다.
 *     WARN_ON 으로 감싼 이유는 실패하면 격리가 깨지기 때문이다.
 *  2. BOUND_DRIVER 알림에서, 그 VF 가 나의 VF 인데 **나와 다른 드라이버에
 *     붙었다면** 경고를 남긴다. 1단계가 무슨 이유로든 통하지 않은 경우를
 *     잡는 사후 감시다.
 *
 * 실행 컨텍스트: **PCI 버스 알림 체인 문맥**. 장치 등록/바인딩 경로 안이므로
 * device_lock 이 잡혀 있을 수 있고, 여기서 무거운 일을 하면 안 된다.
 *
 * 호출자: 커널의 버스 알림 체인. 등록은 vfio_pci_vf_init 의
 * bus_register_notifier, 해제는 vfio_pci_vf_uninit.
 * 호출 대상: pci_physfn, device_set_driver_override, pci_dev_driver
 * (drivers/pci/pci-driver.c:3239).
 *
 * 에러 경로: 실패해도 0 을 돌려주어 알림 체인을 막지 않는다. 문제는
 * WARN 과 경고 로그로만 알린다.
 *
 * 호출 체인:
 *   PCI 코어의 장치 등록 → 버스 알림 체인 → [vfio_pci_bus_notifier]
 *     → device_set_driver_override
 */
static int vfio_pci_bus_notifier(struct notifier_block *nb,
				 unsigned long action, void *data)
{
	/* [한국어] notifier_block 이 vdev 안에 박혀 있으므로 container_of 로 본체를 되찾는다. */
	struct vfio_pci_core_device *vdev = container_of(nb,
						    struct vfio_pci_core_device, nb);
	/* [한국어] 알림 대상 장치. */
	struct device *dev = data;
	/* [한국어] 그것을 pci_dev 로 본다. 이 알림 체인은 PCI 버스 전용이라 안전하다. */
	struct pci_dev *pdev = to_pci_dev(dev);
	/* [한국어] 그 장치가 VF 라면 그 PF 를, 아니면 자기 자신을 돌려준다. */
	struct pci_dev *physfn = pci_physfn(pdev);

	/* [한국어] 새 장치가 등록되는 알림이고 */
	if (action == BUS_NOTIFY_ADD_DEVICE &&
	    /* [한국어] 그 장치가 VF 이며 그 PF 가 바로 나 자신인 경우. */
	    pdev->is_virtfn && physfn == vdev->pdev) {
		/* [한국어] 가로챘음을 관리자에게 알린다. */
		pci_info(vdev->pdev, "Captured SR-IOV VF %s driver_override\n",
			 pci_name(pdev));
		/* [한국어] **driver_override 에 내 드라이버 이름을 심는다.** 이 순간 이후 PCI 코어는
		 * 그 VF 를 이 드라이버에만 붙인다 — 그 매칭 규칙은 이 트리의
		 * drivers/pci/pci-driver.c:362~404 와 435~437 에 주석돼 있다.
		 * 실패하면 격리가 깨지므로 WARN 으로 드러낸다. */
		WARN_ON(device_set_driver_override(&pdev->dev,
						   /* [한국어] 심을 이름은 vendor 가 ops->name 에 적어 둔 값이다.
						    * vfio_pci_core_register_device 가 그것이 있는지 WARN 으로 강제한다. */
						   vdev->vdev.ops->name));
	/* [한국어] 드라이버 바인딩이 끝났다는 알림이고 */
	} else if (action == BUS_NOTIFY_BOUND_DRIVER &&
		   /* [한국어] 그 장치가 나의 VF 인 경우. */
		   pdev->is_virtfn && physfn == vdev->pdev) {
		/* [한국어] 그 VF 에 실제로 붙은 드라이버. */
		struct pci_driver *drv = pci_dev_driver(pdev);

		/* [한국어] 붙긴 붙었는데 나와 다른 드라이버라면 위 가로채기가 통하지 않은 것이다. */
		if (drv && drv != pci_dev_driver(vdev->pdev))
			/* [한국어] 사후 감시 경고를 남긴다. 격리가 깨진 상태이므로 관리자가 알아야 한다. */
			pci_warn(vdev->pdev,
				 "VF %s bound to driver %s while PF bound to driver %s\n",
				 pci_name(pdev), drv->name,
				 pci_dev_driver(vdev->pdev)->name);
	}

	/* [한국어] 0 은 NOTIFY_DONE 과 같은 값으로, 다른 알림 수신자를 막지 않는다. */
	return 0;
}

/* [한국어]
 * vfio_pci_vf_init - PF 라면 토큰과 알림을 준비하고, VF 라면 자기 PF 를 찾아 둔다
 *
 * @vdev: 대상 디바이스.
 * @return: 0 성공. -ENOMEM 이면 토큰 할당 실패, 그 밖은 알림 등록 실패.
 *
 * 왜 필요한가: SR-IOV 신뢰 모델을 세우는 준비 단계다. 하는 일이 PF 와 VF 에서
 * 완전히 다르다.
 *
 * 동작 과정:
 *  [VF 인 경우]
 *    전역 목록에서 자기 PF 의 vfio_pci_core_device 를 찾아 sriov_pf_core_dev 에
 *    저장한다. 못 찾으면 NULL 로 남고, 그것은 "PF 가 vfio-pci 가 아니다" 를
 *    뜻하며 토큰 검사가 면제된다. 원본 주석이 수명을 보장한다 — 이 VF 가
 *    vfio_pci_core_sriov_configure 로 만들어졌다면 pci_disable_sriov 의 잠금
 *    덕분에 이 VF 드라이버가 제거되기 전까지 PF 객체가 사라지지 않는다.
 *  [PF 도 VF 도 아닌 경우]
 *    아무것도 하지 않고 0. vf_token 이 NULL 로 남는다.
 *  [PF 인 경우]
 *    1. vf_token 을 할당하고 mutex 를 초기화한 뒤 무작위 UUID 로 채운다.
 *       무작위로 시작하는 것이 중요하다 — 사용자가 명시적으로 설정하기 전에는
 *       아무도 VF 를 열 수 없다.
 *    2. 버스 알림을 등록해 앞으로 생길 VF 를 가로챌 준비를 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. probe 중이며 device_lock 이 잡혀 있다.
 * 전역 sriov_pfs_mutex 를 짧게 잡는다.
 *
 * 호출자: vfio_pci_core_register_device.
 * 호출 대상: pci_physfn, kzalloc_obj, mutex_init, uuid_gen,
 * bus_register_notifier.
 *
 * 에러 경로: 알림 등록이 실패하면 방금 할당한 토큰을 해제하고 오류를
 * 돌려준다. [상류 코드 관찰] 그 경로는 kfree 만 하고 mutex_destroy 는 하지
 * 않는다 — 정상 해제 경로인 vfio_pci_vf_uninit 은 mutex_destroy 를 부른다.
 * lockdep 이 켜진 빌드에서 초기화된 락을 해제 없이 버리는 형태다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_pci.c probe → vfio_pci_core_register_device → [vfio_pci_vf_init]
 *     → bus_register_notifier
 */
static int vfio_pci_vf_init(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 전역 PF 목록을 순회할 커서. */
	struct vfio_pci_core_device *cur;
	/* [한국어] 이 VF 의 PF 에 해당하는 pci_dev. */
	struct pci_dev *physfn;
	/* [한국어] 알림 등록의 반환값. */
	int ret;

	/* [한국어] 이 디바이스가 VF 인 경우. */
	if (pdev->is_virtfn) {
		/*
		 * If this VF was created by our vfio_pci_core_sriov_configure()
		 * then we can find the PF vfio_pci_core_device now, and due to
		 * the locking in pci_disable_sriov() it cannot change until
		 * this VF device driver is removed.
		 */
		/* [한국어] 이 VF 의 물리 함수(PF)를 얻는다. 이것이 전역 목록을 뒤질 열쇠다. */
		physfn = pci_physfn(vdev->pdev);
		/* [한국어] 전역 PF 목록을 지키는 mutex 를 잡는다. */
		mutex_lock(&vfio_pci_sriov_pfs_mutex);
		/* [한국어] SR-IOV 를 켠 적이 있는 vfio-pci PF 들을 훑는다. */
		list_for_each_entry(cur, &vfio_pci_sriov_pfs, sriov_pfs_item) {
			/* [한국어] 그중 내 PF 와 같은 pci_dev 를 쓰는 항목을 찾으면. */
			if (cur->pdev == physfn) {
				/* [한국어] 그 PF 객체로 가는 포인터를 저장한다. 원본 주석대로 pci_disable_sriov 의
				 * 잠금 덕분에 이 VF 드라이버가 제거되기 전까지 그 객체는 사라지지 않는다. */
				vdev->sriov_pf_core_dev = cur;
				/* [한국어] 하나 찾으면 끝이다. */
				break;
			}
		}
		/* [한국어] 락을 놓는다. 못 찾았으면 sriov_pf_core_dev 가 NULL 로 남고, 그것은
		 * "PF 가 vfio-pci 가 아니다" 를 뜻해 토큰 검사가 면제된다. */
		mutex_unlock(&vfio_pci_sriov_pfs_mutex);
		/* [한국어] VF 쪽 준비 끝. */
		return 0;
	}

	/* Not a SRIOV PF */
	/* [한국어] PF 도 VF 도 아닌 평범한 함수. */
	if (!pdev->is_physfn)
		/* [한국어] 할 일이 없다. vf_token 은 NULL 로 남는다. */
		return 0;

	/* [한국어] PF 전용 토큰 객체를 만든다. 이 할당에는 GFP_KERNEL_ACCOUNT 를 쓰지 않는데,
	 * probe 경로라 사용자 요청이 아니기 때문이다. */
	vdev->vf_token = kzalloc_obj(*vdev->vf_token);
	/* [한국어] 할당 실패. */
	if (!vdev->vf_token)
		/* [한국어] -ENOMEM. probe 가 실패한다. */
		return -ENOMEM;

	/* [한국어] 토큰을 지킬 mutex 를 초기화한다. */
	mutex_init(&vdev->vf_token->lock);
	/* [한국어] **무작위 UUID 로 시작한다.** 사용자가 명시적으로 설정하기 전에는
	 * 아무도 맞힐 수 없으므로 VF 를 여는 것이 사실상 불가능하다 —
	 * 안전 기본값이다. */
	uuid_gen(&vdev->vf_token->uuid);

	/* [한국어] 버스 알림이 오면 부를 함수를 건다. */
	vdev->nb.notifier_call = vfio_pci_bus_notifier;
	/* [한국어] PCI 버스 알림 체인에 등록한다. 이 뒤로 새 VF 가 나타나면
	 * vfio_pci_bus_notifier 가 가로챈다. */
	ret = bus_register_notifier(&pci_bus_type, &vdev->nb);
	/* [한국어] 등록 실패. */
	if (ret) {
		/* [한국어] 방금 만든 토큰을 해제한다.
		 * [상류 코드 관찰] 이 경로는 위에서 초기화한 mutex 를 mutex_destroy 하지
		 * 않고 버린다. 정상 해제 경로인 vfio_pci_vf_uninit 은 mutex_destroy 를
		 * 부른다. lockdep 이 켜진 빌드에서 초기화된 락을 해제 없이 버리는 형태다.
		 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		kfree(vdev->vf_token);
		/* [한국어] 그대로 올려 보낸다. */
		return ret;
	}
	/* [한국어] PF 쪽 준비 끝. */
	return 0;
}

/* [한국어]
 * vfio_pci_vf_uninit - PF 쪽 토큰과 알림 등록을 되돌린다
 *
 * @vdev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_pci_vf_init 의 PF 분기와 정확히 짝이다. VF 분기는 포인터를
 * 하나 저장했을 뿐이라 되돌릴 것이 없다.
 *
 * 동작 과정:
 *  1. vf_token 이 없으면(PF 가 아니면) 즉시 반환. 이 검사 하나가 "PF 인가" 의
 *     판정이자 이 함수 전체의 게이트다.
 *  2. 버스 알림을 해제한다. 이 뒤로는 새 VF 를 가로채지 않는다.
 *  3. WARN_ON(users) — 아직 VF 를 쓰는 사용자가 있는데 PF 를 내리는 것은
 *     수명 관리가 깨진 것이므로 경고한다.
 *  4. mutex 를 파괴하고 토큰을 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. remove 경로 또는 register 의 실패 되감기.
 *
 * 호출자: vfio_pci_core_unregister_device 와
 * vfio_pci_core_register_device 의 out_vf 되감기.
 * 호출 대상: bus_unregister_notifier, mutex_destroy, kfree.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci.c remove → vfio_pci_core_unregister_device → [vfio_pci_vf_uninit]
 */
static void vfio_pci_vf_uninit(struct vfio_pci_core_device *vdev)
{
	/* [한국어] vf_token 이 없다 = PF 가 아니다. 이 검사 하나가 이 함수 전체의 게이트다. */
	if (!vdev->vf_token)
		/* [한국어] 되돌릴 것이 없다. */
		return;

	/* [한국어] 알림 체인에서 뺀다. 이 뒤로는 새 VF 를 가로채지 않는다. */
	bus_unregister_notifier(&pci_bus_type, &vdev->nb);
	/* [한국어] 아직 VF 를 쓰는 사용자가 있는데 PF 를 내리는 것은 수명 관리가 깨진
	 * 상태이므로 경고한다. */
	WARN_ON(vdev->vf_token->users);
	/* [한국어] 락을 파괴한다. lockdep 에게 이 락의 수명이 끝났음을 알린다. */
	mutex_destroy(&vdev->vf_token->lock);
	/* [한국어] 토큰 객체를 해제한다. */
	kfree(vdev->vf_token);
}

/* [한국어]
 * vfio_pci_vga_init - VGA 장치라면 기존 콘솔을 밀어내고 중재자에 등록한다
 *
 * @vdev: 대상 디바이스.
 * @return: 0 성공(또는 VGA 장치가 아니어서 할 일 없음). 음수면 실패.
 *
 * 왜 필요한가: 그래픽 카드를 사용자 공간에 넘기려면 두 가지를 정리해야 한다.
 * (1) 커널의 프레임버퍼/콘솔 드라이버가 그 BAR 를 계속 쓰고 있으면 안 된다.
 * (2) 레거시 VGA 자원은 여러 카드가 공유하므로 중재자에게 이 카드의 디코드
 *     범위를 신고해야 한다.
 *
 * 동작 과정:
 *  1. VGA 클래스가 아니면 아무것도 하지 않는다(vfio_pci_is_vga 는
 *     vfio_pci_priv.h 의 인라인으로 클래스 코드 상위 16비트를 본다).
 *  2. aperture_remove_conflicting_pci_devices 로 이 카드의 프레임버퍼를
 *     쓰던 드라이버를 제거한다.
 *  3. vga_client_register(drivers/pci/vgaarb.c:2812)로 중재자에 콜백을 등록한다.
 *  4. vga_set_legacy_decoding(vgaarb.c:2745)으로 지금의 디코드 상태를 신고한다.
 *     single_vga 를 false 로 주는 이유는, 등록 시점에는 이 카드가 유일한
 *     VGA 인지 알 수 없으므로 **더 보수적인 쪽**으로 계산하기 위해서다.
 *
 * 실행 컨텍스트: 프로세스 문맥. probe 중이다.
 *
 * 호출자: vfio_pci_core_register_device.
 * 호출 대상: vfio_pci_is_vga, aperture_remove_conflicting_pci_devices,
 * vga_client_register, vfio_pci_set_decode, vga_set_legacy_decoding.
 *
 * 에러 경로: 2단계나 3단계가 실패하면 그대로 오류를 돌려준다. 그 시점에는
 * 등록된 것이 없으므로 되감을 것이 없다 — 그래서 호출자의 되감기 라벨
 * out_vf 는 VGA 정리를 하지 않는다.
 *
 * 호출 체인:
 *   vfio_pci.c probe → vfio_pci_core_register_device → [vfio_pci_vga_init]
 *     → vga_client_register
 */
static int vfio_pci_vga_init(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] 클래스 코드 상위 16비트가 VGA 인지 본다(vfio_pci_priv.h 의 인라인). */
	if (!vfio_pci_is_vga(pdev))
		/* [한국어] VGA 장치가 아니면 할 일이 없다. */
		return 0;

	/* [한국어] **이 카드의 프레임버퍼를 쓰던 커널 드라이버를 밀어낸다.** 사용자에게
	 * 넘기기 전에 커널이 그 BAR 를 놓아야 한다. 두 번째 인자는 로그에 찍힐
	 * 새 소유자 이름이다. */
	ret = aperture_remove_conflicting_pci_devices(pdev, vdev->vdev.ops->name);
	/* [한국어] 밀어내기 실패(그 드라이버가 제거를 거부). */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. */
		return ret;

	/* [한국어] VGA 중재자에 이 파일의 디코드 신고 콜백을 등록한다
	 * (drivers/pci/vgaarb.c:2812). */
	ret = vga_client_register(pdev, vfio_pci_set_decode);
	/* [한국어] 등록 실패. */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. 이 시점에는 등록된 것이 없으므로 되감을 것이 없다. */
		return ret;
	/* [한국어] 지금의 디코드 상태를 중재자에게 신고한다. single_vga 를 false 로 주는
	 * 이유는 등록 시점에는 이 카드가 유일한 VGA 인지 알 수 없어 **더 보수적인
	 * 쪽**으로 계산하기 위해서다(drivers/pci/vgaarb.c:2745). */
	vga_set_legacy_decoding(pdev, vfio_pci_set_decode(pdev, false));
	/* [한국어] VGA 준비 끝. */
	return 0;
}

/* [한국어]
 * vfio_pci_vga_uninit - VGA 중재자 등록을 해제하고 디코드 신고를 원상 복구한다
 *
 * @vdev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: vfio_pci_vga_init 의 짝이다. 이 드라이버가 물러난 뒤에도
 * 중재자가 죽은 콜백을 들고 있으면 안 되고, 디코드 신고도 "이 카드는 아무것도
 * 감추지 않는다" 로 되돌려야 다음 드라이버가 정상적으로 중재에 참여한다.
 *
 * 동작 과정:
 *  1. VGA 클래스가 아니면 즉시 반환.
 *  2. vga_client_unregister 로 콜백을 뗀다.
 *  3. 네 비트(normal I/O, normal MEM, legacy I/O, legacy MEM)를 모두 신고해
 *     가장 보수적인 상태로 되돌린다. 즉 "이 카드가 전부 디코드한다" 고
 *     알려 중재자가 안전하게 판단하게 한다.
 * 비대칭에 주의: init 이 했던 aperture_remove_conflicting_pci_devices 는
 * 되돌리지 않는다. 밀어낸 프레임버퍼 드라이버를 다시 붙이는 것은 이 계층의
 * 일이 아니기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. remove 경로.
 *
 * 호출자: vfio_pci_core_unregister_device.
 * 호출 대상: vfio_pci_is_vga, vga_client_unregister, vga_set_legacy_decoding.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci.c remove → vfio_pci_core_unregister_device
 *     → [vfio_pci_vga_uninit] → vga_client_unregister
 */
static void vfio_pci_vga_uninit(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;

	/* [한국어] VGA 장치가 아니면 등록한 적도 없다. */
	if (!vfio_pci_is_vga(pdev))
		/* [한국어] 할 일이 없다. */
		return;
	/* [한국어] 중재자에서 콜백을 뗀다. 죽은 콜백을 남기면 중재자가 해제된 객체를 부른다. */
	vga_client_unregister(pdev);
	/* [한국어] 네 비트를 모두 신고해 "이 카드가 전부 디코드한다" 는 가장 보수적인
	 * 상태로 되돌린다. 다음 드라이버가 정상적으로 중재에 참여할 수 있게 한다. */
	vga_set_legacy_decoding(pdev, VGA_RSRC_NORMAL_IO | VGA_RSRC_NORMAL_MEM |
					      /* [한국어] 레거시 I/O 비트. */
					      VGA_RSRC_LEGACY_IO |
					      /* [한국어] 레거시 메모리 비트. **init 이 했던 aperture 밀어내기는 되돌리지 않는다** —
					       * 밀어낸 프레임버퍼 드라이버를 다시 붙이는 것은 이 계층의 일이 아니다. */
					      VGA_RSRC_LEGACY_MEM);
}

/* [한국어]
 * vfio_pci_core_init_dev - vfio_device_ops 의 init 슬롯: 구조체 필드를 초기 상태로 세운다
 *
 * @core_vdev: 코어가 막 할당한 struct vfio_device. 뒤에 PCI 확장부가 붙어 있다.
 * @return: 0 성공. 음수면 p2pdma 초기화 실패.
 *
 * 왜 필요한가: 코어(vfio_main.c 의 _vfio_alloc_device)가 메모리를 잡고 공통
 * 필드를 채운 뒤, vendor 에게 "네 부분을 초기화하라" 고 부르는 자리다.
 * 여기서 세워지는 것들은 **디바이스가 열리기 전부터 유효해야 하는** 것들이다 —
 * 락, 빈 목록, xarray. 열기에서 채우는 하드웨어 상태와는 층이 다르다.
 *
 * 동작 과정:
 *  1. core_vdev->dev 를 pci_dev 로 변환해 저장한다. 이 포인터가 이 파일의
 *     거의 모든 함수의 출발점이다.
 *  2. irq_type 을 VFIO_PCI_NUM_IRQS 로 둔다. 유효한 인덱스 범위를 벗어난
 *     값이라 **"아직 아무 인터럽트도 설정하지 않았다"** 를 뜻하는 표식이다.
 *  3. igate(인터럽트 설정 직렬화), irqlock(인터럽트 문맥과 공유하는 스핀락),
 *     ioeventfds_lock 을 초기화한다.
 *  4. 네 개의 리스트 머리를 초기화한다 — dummy resource, ioeventfd,
 *     SR-IOV PF 목록의 노드, dma-buf 목록.
 *  5. pcim_p2pdma_init(drivers/pci/p2pdma.c:809)으로 이 장치의 BAR 를 P2P DMA
 *     제공자로 등록한다. -EOPNOTSUPP 는 그 플랫폼이 P2P 를 지원하지 않는다는
 *     뜻이라 정상으로 넘어가고, 다른 오류는 초기화 실패로 올린다.
 *  6. memory_lock rwsem 과 인터럽트 컨텍스트 xarray 를 초기화한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. probe 경로이며 아직 아무도 이 객체를 볼 수 없다.
 *
 * 호출자: vfio_main.c 의 vfio_init_device 가 ops->init 으로 부른다
 * (vfio_pci.c:130 이 이 함수를 그 슬롯에 등록한다). vendor 가 자기 초기화를
 * 덧붙일 때는 자기 init 안에서 이것을 먼저 부른다
 * (drivers/vfio/pci/ism/main.c:319, xe/main.c:512).
 * 호출 대상: to_pci_dev, mutex_init, spin_lock_init, INIT_LIST_HEAD,
 * pcim_p2pdma_init, init_rwsem, xa_init.
 *
 * 에러 경로: 5단계 실패만이 유일한 실패다. **그 시점까지 초기화한 mutex 들을
 * 파괴하지 않고 반환한다** — 코어가 뒤이어 ops->release 를 부르지 않고
 * 그냥 해제하는 경로다. 실제로 문제가 되지는 않지만, 정상 경로의
 * vfio_pci_core_release_dev 가 mutex_destroy 를 부르는 것과 대비된다.
 *
 * 호출 체인:
 *   vfio_pci.c probe → vfio_main.c:_vfio_alloc_device → vfio_init_device
 *     → ops->init = [vfio_pci_core_init_dev] → pcim_p2pdma_init
 */
int vfio_pci_core_init_dev(struct vfio_device *core_vdev)
{
	/* [한국어] 코어가 막 할당한 객체에서 PCI 확장부를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);
	/* [한국어] p2pdma 초기화의 반환값. */
	int ret;

	/* [한국어] core_vdev->dev(struct device)를 pci_dev 로 되돌려 저장한다.
	 * **이 포인터가 이 파일 거의 모든 함수의 출발점이다.** */
	vdev->pdev = to_pci_dev(core_vdev->dev);
	/* [한국어] 유효한 인덱스 범위(0~4)를 벗어난 값을 넣어 "아직 아무 인터럽트도
	 * 설정하지 않았다" 를 표시한다. vfio_pci_core_disable 이 이 값으로
	 * 해제 요청을 보내면 vfio_pci_intrs.c 가 걸러 낸다. */
	vdev->irq_type = VFIO_PCI_NUM_IRQS;
	/* [한국어] 인터럽트 설정을 직렬화하는 mutex. */
	mutex_init(&vdev->igate);
	/* [한국어] 인터럽트 핸들러와 공유하는 스핀락. 인터럽트 문맥에서도 잡히므로
	 * mutex 가 아니라 spinlock 이다. */
	spin_lock_init(&vdev->irqlock);
	/* [한국어] ioeventfd 목록을 지키는 mutex. */
	mutex_init(&vdev->ioeventfds_lock);
	/* [한국어] sub-page BAR 자리표 목록. */
	INIT_LIST_HEAD(&vdev->dummy_resources_list);
	/* [한국어] ioeventfd 목록. */
	INIT_LIST_HEAD(&vdev->ioeventfds_list);
	/* [한국어] 전역 SR-IOV PF 목록에 매달릴 노드. 비어 있는 상태로 시작하는 것이
	 * "아직 SR-IOV 를 켜지 않았다" 를 뜻한다. */
	INIT_LIST_HEAD(&vdev->sriov_pfs_item);
	/* [한국어] 이 장치의 BAR 를 peer-to-peer DMA 제공자로 등록한다
	 * (drivers/pci/p2pdma.c:809). 그래야 dma-buf 로 BAR 를 내줄 수 있다. */
	ret = pcim_p2pdma_init(vdev->pdev);
	/* [한국어] -EOPNOTSUPP 는 그 플랫폼이 P2P 를 지원하지 않는다는 뜻이라 정상이다.
	 * 그 밖의 오류만 초기화 실패로 본다. */
	if (ret && ret != -EOPNOTSUPP)
		/* [한국어] 그대로 올려 보낸다. [상류 코드 관찰] 이 경로는 위에서 초기화한 두
		 * mutex 를 파괴하지 않는다 — 코어는 ops->init 이 실패하면 ops->release 를
		 * 부르지 않고 객체를 해제한다. 정상 경로의 vfio_pci_core_release_dev 는
		 * mutex_destroy 를 부르므로 두 경로의 처리가 다르다.
		 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
		return ret;
	/* [한국어] dma-buf 목록. */
	INIT_LIST_HEAD(&vdev->dmabufs);
	/* [한국어] **BAR 접근과 리셋을 가르는 핵심 락.** 읽기는 폴트와 MMIO 접근이,
	 * 쓰기는 리셋과 저전력 진입이 잡는다. */
	init_rwsem(&vdev->memory_lock);
	/* [한국어] 인터럽트 컨텍스트를 벡터 번호로 색인하는 xarray. */
	xa_init(&vdev->ctx);

	/* [한국어] 초기화 완료. 이제 코어가 이 객체를 등록 절차에 넘긴다. */
	return 0;
}
/* [한국어] vendor 가 자기 init 안에서 먼저 부를 수 있게 내보낸다
 * (drivers/vfio/pci/ism/main.c:319, xe/main.c:512). */
EXPORT_SYMBOL_GPL(vfio_pci_core_init_dev);

/* [한국어]
 * vfio_pci_core_release_dev - vfio_device_ops 의 release 슬롯: 마지막 참조가 사라질 때 정리한다
 *
 * @core_vdev: 해제되기 직전의 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: struct vfio_device 는 refcount 로 관리되고, 마지막 참조가
 * 사라질 때 코어가 이 콜백을 부른 뒤 메모리를 해제한다
 * (vfio_main.c:852~855). 그 시점에 남아 있을 수 있는 것들을 여기서 정리한다.
 *
 * 동작 과정:
 *  1. igate 와 ioeventfds_lock 을 파괴한다. lockdep 이 켜진 빌드에서 락의
 *     수명을 정확히 알리는 절차다. irqlock 은 spinlock 이라 파괴 절차가 없다.
 *  2. region 배열을 해제한다. 정상 경로라면 vfio_pci_core_disable 이 이미
 *     NULL 로 만들었지만, 열린 적 없이 사라지는 경우를 위해 남겨 둔다.
 *  3. pm_save 를 해제한다. 저전력 전이 도중 남았을 수 있는 스냅숏이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 디바이스가 이미 등록 해제된 뒤이며
 * 아무도 이 객체를 참조하지 않는다.
 *
 * 호출자: vfio_main.c 의 vfio_device_release 가 ops->release 로 부른다
 * (vfio_pci.c:131). vendor 가 감싸기도 한다(ism/main.c:332, xe/main.c:521).
 * 호출 대상: mutex_destroy, kfree.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   마지막 put_device → vfio_main.c:vfio_device_release
 *     → ops->release = [vfio_pci_core_release_dev]
 */
void vfio_pci_core_release_dev(struct vfio_device *core_vdev)
{
	/* [한국어] 해제되기 직전의 객체에서 PCI 확장부를 되찾는다. */
	struct vfio_pci_core_device *vdev =
		container_of(core_vdev, struct vfio_pci_core_device, vdev);

	/* [한국어] 인터럽트 설정 락을 파괴한다. lockdep 에게 수명이 끝났음을 알린다. */
	mutex_destroy(&vdev->igate);
	/* [한국어] ioeventfd 락을 파괴한다. irqlock 은 spinlock 이라 파괴 절차가 없다. */
	mutex_destroy(&vdev->ioeventfds_lock);
	/* [한국어] vendor region 배열을 해제한다. 정상 경로라면 vfio_pci_core_disable 이
	 * 이미 NULL 로 만들었지만, 열린 적 없이 사라지는 경우를 위해 남겨 둔다. */
	kfree(vdev->region);
	/* [한국어] 저전력 전이 도중 남았을 수 있는 config 스냅숏을 해제한다. */
	kfree(vdev->pm_save);
}
/* [한국어] vendor 가 자기 release 안에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_release_dev);

/* [한국어]
 * vfio_pci_core_register_device - probe 의 몸통: 검사, device set 배정, 등록
 *
 * @vdev: vendor 가 이미 할당하고 init 을 마친 디바이스.
 * @return: 0 성공. -EINVAL(계약 위반 또는 브리지), -EBUSY(VF 가 살아 있는 PF),
 *          그 밖은 각 단계의 오류.
 *
 * 왜 필요한가: **하드웨어가 VFIO 세계에 들어오는 관문**이다. 여기서 통과하지
 * 못하면 그 PCI 함수는 사용자에게 노출되지 않는다. 검사가 여섯 겹이고,
 * 각각이 뒤에서 어떤 사고를 막는지가 분명하다.
 *
 * 검사 단계:
 *  1. drvdata 가 vdev 를 가리키는지. 이 파일의 여러 콜백
 *     (vfio_pci_core_runtime_suspend/resume, vfio_pci_core_aer_err_detected)이
 *     dev_get_drvdata 로 vdev 를 되찾으므로, 그 계약이 깨지면 엉뚱한
 *     포인터를 역참조하게 된다. WARN 으로 vendor 의 버그를 드러낸다.
 *  2. ops->name 이 있는지. SR-IOV VF 를 가로챌 때 driver_override 에 심을
 *     이름이라 없으면 격리가 성립하지 않는다.
 *  3. 헤더 타입이 일반(엔드포인트)인지. **브리지는 받지 않는다** — 브리지를
 *     사용자에게 넘기면 그 아래 모든 장치의 라우팅을 사용자가 쥐게 된다.
 *  4. **마이그레이션 vtable 의 슬롯 단위 완비성 검사.**
 *     drivers/vfio/vfio_main.c 의 주석이 적어 둔 대로, 코어는 mig_ops 포인터가
 *     있는지만 보고 개별 슬롯은 보지 않는다. 그 슬롯 셋(get_state, set_state,
 *     get_data_size)이 모두 채워졌는지, 그리고 migration_flags 에 최소한
 *     STOP_COPY 가 선언됐는지를 **여기서** 확인한다. 하나라도 비면 -EINVAL 로
 *     등록 자체를 거부한다. 코어가 나중에 NULL 슬롯을 부르는 사고를 이
 *     한 번의 검사로 막는 구조다.
 *  5. 같은 방식으로 log_ops 세 슬롯(log_start, log_stop, log_read_and_clear)의
 *     완비성도 본다. 이쪽은 flags 검사가 없다.
 *  6. VF 가 이미 살아 있는 PF 는 -EBUSY. 원본 영어 주석이 이유를 셋 든다 —
 *     이미 있는 VF 를 가로챌 수 없고, VF 사용자를 추적할 수 없으며, 여기서
 *     SR-IOV 를 끄면 VF 제거가 시작되어 그 VF 를 쓰는 vfio-pci 가 막힐 수 있다.
 *
 * 등록 단계:
 *  7. **device set 배정** — 리셋 범위에 따라 셋 중 하나를 고른다.
 *     루트 버스 직결이거나 VF 면 자기 자신(혼자 리셋됨),
 *     슬롯 리셋이 되면 pdev->slot, 아니면 pdev->bus. 원본 주석대로
 *     슬롯 리셋조차 안 되면 버스 전체가 한 묶음이 되어야 버스 리셋을 쓸 수 있다.
 *     이 선택이 이후 모든 hot reset 판정의 기준이 된다.
 *  8. SR-IOV 준비(vfio_pci_vf_init)와 VGA 준비(vfio_pci_vga_init).
 *  9. 전원 관리 준비: NoSoftRst 여부를 조사하고, 원본 주석대로
 *     **일단 D0 로 한 번 전이시킨다**. pci-core 는 부팅 직후와 드라이버 제거
 *     직후에 전원 상태를 "알 수 없음" 으로 두는데, 그 상태에서 허용되는
 *     유일한 전이가 D0 이기 때문이다. 아직 pci_enable_device 를 부를 때가
 *     아니므로 직접 전이시킨다.
 * 10. dev->driver->pm 에 이 파일의 런타임 PM 콜백 표를 꽂고 런타임 PM 을
 *     허용한다. 그리고 절전이 켜져 있으면 참조를 하나 놓아 즉시 잠들 수 있게 한다.
 * 11. vfio_register_group_dev 로 코어에 등록한다. **이 줄이 끝나는 순간부터
 *     사용자가 이 장치를 열 수 있다.**
 *
 * 실행 컨텍스트: 프로세스 문맥. PCI probe 안이라 device_lock 이 잡혀 있다.
 *
 * 호출자: vendor 의 probe — drivers/vfio/pci/vfio_pci.c:170, ism/main.c:368 등.
 * 호출 대상: pci_num_vf, pci_is_root_bus, pci_probe_reset_slot,
 * vfio_assign_device_set(vfio_main.c), vfio_pci_vf_init, vfio_pci_vga_init,
 * vfio_pci_probe_power_state, vfio_pci_set_power_state, pm_runtime_allow,
 * vfio_register_group_dev(vfio_main.c).
 *
 * 에러 경로: 두 개의 되감기 라벨. out_power 는 런타임 PM 참조를 되찾고
 * 금지 상태로 되돌린 뒤 out_vf 로 이어져 SR-IOV 준비를 되돌린다.
 * **VGA 준비(8단계 뒤쪽)는 되감지 않는다** — out_vf 는 vfio_pci_vga_uninit 을
 * 부르지 않는다. VGA 등록 자체가 실패하면 되감을 것이 없고, 그 뒤 단계에서
 * 실패한 경우에도 그렇다. 정상 해제는 vfio_pci_core_unregister_device 가 한다.
 * device set 배정(7단계)도 되감지 않는데, 그것은 코어가 디바이스 해제 때
 * 자동으로 정리한다.
 *
 * 호출 체인:
 *   사용자가 driver_override 에 vfio-pci 를 씀
 *     → drivers/pci/pci-driver.c:362~404 의 매칭 → vfio_pci.c probe
 *     → [vfio_pci_core_register_device] → vfio_main.c:vfio_register_group_dev
 */
int vfio_pci_core_register_device(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 런타임 PM 조작에 쓸 struct device 포인터. */
	struct device *dev = &pdev->dev;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* Drivers must set the vfio_pci_core_device to their drvdata */
	/* [한국어] **drvdata 계약 검사.** 이 파일의 runtime_suspend/resume 과
	 * aer_err_detected 가 dev_get_drvdata 로 vdev 를 되찾으므로, 이 계약이
	 * 깨지면 엉뚱한 포인터를 역참조하게 된다. vendor 의 버그이므로 WARN 으로
	 * 드러낸다. */
	if (WARN_ON(vdev != dev_get_drvdata(dev)))
		/* [한국어] -EINVAL. 등록을 거부한다. */
		return -EINVAL;

	/* Drivers must set a name.  Required for sequestering SR-IOV VFs */
	/* [한국어] ops->name 이 없으면 SR-IOV VF 를 가로챌 때 driver_override 에 심을
	 * 이름이 없다 — 격리가 성립하지 않는다. */
	if (WARN_ON(!vdev->vdev.ops->name))
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] **브리지는 받지 않는다.** 브리지를 사용자에게 넘기면 그 아래 모든
	 * 장치의 라우팅을 사용자가 쥐게 된다. 일반(엔드포인트) 헤더만 허용한다. */
	if (pdev->hdr_type != PCI_HEADER_TYPE_NORMAL)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] vendor 가 마이그레이션을 지원한다고 선언했으면. */
	if (vdev->vdev.mig_ops) {
		/* [한국어] **마이그레이션 vtable 의 슬롯 단위 완비성 검사.**
		 * drivers/vfio/vfio_main.c 의 주석이 적어 둔 대로 코어는 mig_ops 포인터가
		 * 있는지만 보고 개별 슬롯은 보지 않는다. 그 검사가 여기 있다. */
		if (!(vdev->vdev.mig_ops->migration_get_state &&
		      /* [한국어] 상태를 바꾸는 슬롯. */
		      vdev->vdev.mig_ops->migration_set_state &&
		      /* [한국어] 남은 데이터 크기를 묻는 슬롯. 셋 중 하나라도 비면 거부한다. */
		      vdev->vdev.mig_ops->migration_get_data_size) ||
		    /* [한국어] 그리고 migration_flags 에 최소한 STOP_COPY 가 선언돼 있어야 한다.
		     * 마이그레이션의 가장 기본 모드조차 지원하지 않으면 의미가 없다. */
		    !(vdev->vdev.migration_flags & VFIO_MIGRATION_STOP_COPY))
			/* [한국어] -EINVAL. **등록 자체를 거부해 코어가 나중에 NULL 슬롯을 부르는 사고를
			 * 한 번의 검사로 막는다.** */
			return -EINVAL;
	}

	/* [한국어] DMA 더티 로깅을 지원한다고 선언했으면 세 슬롯이 모두 있어야 한다.
	 * 이쪽은 flags 검사가 없다. */
	if (vdev->vdev.log_ops && !(vdev->vdev.log_ops->log_start &&
	    /* [한국어] 로깅 정지 슬롯. */
	    vdev->vdev.log_ops->log_stop &&
	    /* [한국어] 더티 비트맵을 읽고 지우는 슬롯. */
	    vdev->vdev.log_ops->log_read_and_clear))
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/*
	 * Prevent binding to PFs with VFs enabled, the VFs might be in use
	 * by the host or other users.  We cannot capture the VFs if they
	 * already exist, nor can we track VF users.  Disabling SR-IOV here
	 * would initiate removing the VFs, which would unbind the driver,
	 * which is prone to blocking if that VF is also in use by vfio-pci.
	 * Just reject these PFs and let the user sort it out.
	 */
	/* [한국어] VF 가 이미 살아 있는 PF 인가. */
	if (pci_num_vf(pdev)) {
		/* [한국어] 왜 거부하는지 관리자에게 알린다. */
		pci_warn(pdev, "Cannot bind to PF with SR-IOV enabled\n");
		/* [한국어] -EBUSY. 사용자가 먼저 VF 를 정리하게 한다. */
		return -EBUSY;
	}

	/* [한국어] **device set 배정 — 리셋 범위를 정한다.**
	 * 루트 버스에 직결된 장치나 VF 는 함께 리셋될 상대가 없다. */
	if (pci_is_root_bus(pdev->bus) || pdev->is_virtfn) {
		/* [한국어] 자기 자신을 set_id 로 주어 혼자만의 묶음을 만든다. */
		ret = vfio_assign_device_set(&vdev->vdev, vdev);
	/* [한국어] 슬롯 리셋이 가능하면. */
	} else if (!pci_probe_reset_slot(pdev->slot)) {
		/* [한국어] 같은 슬롯의 함수들이 한 묶음이 된다. pdev->slot 포인터가 set_id 다. */
		ret = vfio_assign_device_set(&vdev->vdev, pdev->slot);
	/* [한국어] 슬롯 리셋도 안 되면. */
	} else {
		/*
		 * If there is no slot reset support for this device, the whole
		 * bus needs to be grouped together to support bus-wide resets.
		 */
		/* [한국어] 버스 전체가 한 묶음이 된다. 원본 주석대로 버스 리셋을 쓰려면 그 범위
		 * 전체를 함께 관리해야 하기 때문이다. */
		ret = vfio_assign_device_set(&vdev->vdev, pdev->bus);
	}

	/* [한국어] 묶음 배정 실패. */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. 배정 실패는 되감을 것이 없다. */
		return ret;
	/* [한국어] SR-IOV 준비 — PF 면 토큰과 알림을, VF 면 PF 포인터를 마련한다. */
	ret = vfio_pci_vf_init(vdev);
	/* [한국어] 준비 실패. */
	if (ret)
		/* [한국어] 그대로 올려 보낸다. */
		return ret;
	/* [한국어] VGA 준비 — 프레임버퍼 드라이버를 밀어내고 중재자에 등록한다. */
	ret = vfio_pci_vga_init(vdev);
	/* [한국어] 준비 실패. */
	if (ret)
		/* [한국어] SR-IOV 준비를 되감는다. */
		goto out_vf;

	/* [한국어] NoSoftRst 여부를 조사해 needs_pm_restore 를 정한다. */
	vfio_pci_probe_power_state(vdev);

	/*
	 * pci-core sets the device power state to an unknown value at
	 * bootup and after being removed from a driver.  The only
	 * transition it allows from this unknown state is to D0, which
	 * typically happens when a driver calls pci_enable_device().
	 * We're not ready to enable the device yet, but we do want to
	 * be able to get to D3.  Therefore first do a D0 transition
	 * before enabling runtime PM.
	 */
	/* [한국어] 원본 주석대로 **일단 D0 로 한 번 전이시킨다.** pci-core 는 부팅 직후와
	 * 드라이버 제거 직후에 전원 상태를 "알 수 없음" 으로 두는데, 그 상태에서
	 * 허용되는 유일한 전이가 D0 다. 아직 pci_enable_device 를 부를 때가 아니라
	 * 직접 전이시킨다. */
	vfio_pci_set_power_state(vdev, PCI_D0);

	/* [한국어] **런타임 PM 콜백 표를 드라이버 구조체에 심는다.** 드라이버에 미리 박아
	 * 두지 않고 등록 시점에 꽂는 것이 이 파일의 특징으로, vfio-pci 계열
	 * 드라이버들이 각자 pm 필드를 채우지 않아도 되게 해 준다. */
	dev->driver->pm = &vfio_pci_core_pm_ops;
	/* [한국어] 이 장치에 런타임 PM 을 허용한다. sysfs 의 power/control 이 auto 가 된다. */
	pm_runtime_allow(dev);
	/* [한국어] 절전이 켜져 있으면. */
	if (!disable_idle_d3)
		/* [한국어] 참조를 하나 놓아 사용자가 없을 때 즉시 D3 로 잠들 수 있게 한다.
		 * 이 참조는 vfio_pci_core_unregister_device 가 되찾는다. */
		pm_runtime_put(dev);

	/* [한국어] **코어에 등록한다. 이 줄이 끝나는 순간부터 사용자가 이 장치를 열 수 있다.** */
	ret = vfio_register_group_dev(&vdev->vdev);
	/* [한국어] 등록 실패. */
	if (ret)
		/* [한국어] 런타임 PM 부터 되감는다. */
		goto out_power;
	/* [한국어] 등록 성공. */
	return 0;

/* [한국어] 코어 등록이 실패했을 때 들어온다. */
out_power:
	/* [한국어] 위에서 참조를 놓았던 조건과 같아야 계수가 맞는다. */
	if (!disable_idle_d3)
		/* [한국어] 놓았던 참조를 되찾는다. */
		pm_runtime_get_noresume(dev);

	/* [한국어] 런타임 PM 을 금지 상태로 되돌린다. */
	pm_runtime_forbid(dev);
/* [한국어] VGA 준비 이후의 실패가 여기로 모인다. */
out_vf:
	/* [한국어] SR-IOV 준비를 되돌린다. **VGA 준비는 되감지 않는다** — 정상 해제는
	 * vfio_pci_core_unregister_device 의 vfio_pci_vga_uninit 이 한다.
	 * device set 배정도 되감지 않으며, 코어가 디바이스 해제 때 정리한다. */
	vfio_pci_vf_uninit(vdev);
	/* [한국어] 실패 원인을 vendor 의 probe 에 돌려준다. */
	return ret;
}
/* [한국어] vendor 가 자기 probe 에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_register_device);

/* [한국어]
 * vfio_pci_core_unregister_device - remove 의 몸통: 등록을 역순으로 푼다
 *
 * @vdev: 대상 디바이스.
 * @return: 없음.
 *
 * 왜 필요한가: 등록의 정확한 역순이며, 중간에 **사용자가 fd 를 놓을 때까지
 * 무한정 기다리는** 지점을 포함한다.
 *
 * 동작 과정:
 *  1. vfio_pci_core_sriov_configure(vdev, 0) 으로 SR-IOV 를 끈다. VF 를
 *     남겨 둔 채 PF 드라이버를 내리면 VF 가 고아가 된다.
 *  2. vfio_unregister_group_dev — 코어에서 뺀다. **이 호출이 사용자가 fd 를
 *     모두 놓을 때까지 잠들며 기다린다.** 그 기다리는 동안 코어가 반복해서
 *     ops->request(= vfio_pci_core_request)를 불러 사용자에게 반납을 요청한다.
 *     이 파일 전체에서 무한정 잠들 수 있는 유일한 지점이다.
 *  3. SR-IOV 준비와 VGA 준비를 되돌린다.
 *  4. 런타임 PM 을 등록 때와 반대로 돌린다 — 절전이 켜져 있었다면 참조를
 *     되찾고, 런타임 PM 을 금지 상태로 만든다. 이 순서라야 참조 계수가 맞는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. PCI remove 안이라 device_lock 이 잡혀 있다.
 * 2단계에서 잠들 수 있다.
 *
 * 호출자: vendor 의 remove — drivers/vfio/pci/vfio_pci.c:184, ism/main.c:384 등.
 * 호출 대상: vfio_pci_core_sriov_configure, vfio_unregister_group_dev
 * (vfio_main.c), vfio_pci_vf_uninit, vfio_pci_vga_uninit,
 * pm_runtime_get_noresume, pm_runtime_forbid.
 *
 * 에러 경로: 없다. remove 는 실패할 수 없다.
 *
 * 호출 체인:
 *   드라이버 언바인드 → vfio_pci.c remove → [vfio_pci_core_unregister_device]
 *     → vfio_main.c:vfio_unregister_group_dev(사용자가 놓을 때까지 대기)
 */
void vfio_pci_core_unregister_device(struct vfio_pci_core_device *vdev)
{
	/* [한국어] SR-IOV 를 끈다. VF 를 남겨 둔 채 PF 드라이버를 내리면 VF 가 고아가 된다.
	 * 두 번째 인자 0 이 "모두 없애라" 는 뜻이다. */
	vfio_pci_core_sriov_configure(vdev, 0);

	/* [한국어] **코어에서 뺀다. 이 호출이 사용자가 fd 를 모두 놓을 때까지 잠들며
	 * 기다린다.** 기다리는 동안 코어가 반복해서 ops->request 를 불러
	 * 사용자에게 반납을 요청한다. 이 파일에서 무한정 잠들 수 있는 유일한 지점이다. */
	vfio_unregister_group_dev(&vdev->vdev);

	/* [한국어] SR-IOV 토큰과 버스 알림 등록을 되돌린다. */
	vfio_pci_vf_uninit(vdev);
	/* [한국어] VGA 중재자 등록을 되돌린다. */
	vfio_pci_vga_uninit(vdev);

	/* [한국어] 등록 때 참조를 놓았던 조건과 같아야 계수가 맞는다. */
	if (!disable_idle_d3)
		/* [한국어] 그때 놓았던 참조를 되찾는다. */
		pm_runtime_get_noresume(&vdev->pdev->dev);

	/* [한국어] 런타임 PM 을 금지 상태로 되돌린다. 등록 때의 pm_runtime_allow 와 짝이다. */
	pm_runtime_forbid(&vdev->pdev->dev);
}
/* [한국어] vendor 가 자기 remove 에서 부를 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_unregister_device);

/* [한국어]
 * vfio_pci_core_aer_err_detected - PCI AER 오류를 사용자 공간에 중계한다
 *
 * @pdev:  오류가 난 장치.
 * @state: 채널 상태(정상/frozen/perm_failure). 이 함수는 보지 않는다.
 * @return: 항상 PCI_ERS_RESULT_CAN_RECOVER — "복구 가능하니 계속 진행하라".
 *
 * 왜 필요한가: **정책이 커널이 아니라 사용자 공간에 있다는 것을 보여 주는
 * 함수다.** 일반 드라이버라면 여기서 DMA 를 멈추고 큐를 비우고 복구를
 * 준비하겠지만, 장치를 실제로 모는 것은 사용자 공간이므로 커널은 알릴 뿐이다.
 * 사용자(주로 QEMU)는 이 eventfd 를 받아 게스트에게 AER 를 주입하거나
 * 장치를 리셋한다.
 *
 * 동작 과정:
 *  1. drvdata 에서 vdev 를 되찾는다.
 *  2. rcu_read_lock 아래에서 err_trigger 를 읽어 있으면 울린다.
 *     등록된 eventfd 가 없으면 조용히 넘어간다.
 *  3. CAN_RECOVER 를 돌려준다. 이 값은 PCI 오류 복구 상태 기계에게
 *     "이 드라이버는 계속할 수 있다" 를 뜻하며, 그래야 복구 절차가 진행된다.
 *
 * 실행 컨텍스트: **PCI AER 복구 경로**. 프로세스 문맥이지만 오류 복구
 * 워크큐 안이며, rcu_read_lock 구간에서는 잠들 수 없다.
 *
 * 호출자: PCI 코어의 오류 복구가 pci_driver->err_handler->error_detected 로
 * 부른다. 아래 vfio_pci_core_err_handlers 가 그 표이며, vendor 는 그 표를
 * 통째로 쓰거나(vfio_pci.c:211) 슬롯만 빌려 쓴다(xe/main.c:145).
 * 호출 대상: dev_get_drvdata, rcu_dereference, eventfd_signal.
 *
 * 에러 경로: 없다.
 *
 * [상류 코드 관찰] dev_get_drvdata 의 결과를 NULL 검사 없이 곧바로
 * vdev->err_trigger 로 역참조한다. 실제로는 vfio_pci_core_register_device 의
 * 첫 검사가 "drvdata 는 반드시 vdev 여야 한다" 를 WARN 으로 강제하고, 그
 * 등록이 끝난 뒤에야 err_handler 가 붙은 드라이버로서 알림을 받으므로 NULL 이
 * 되는 경로를 이 트리에서 찾지 못했다. 다만 검사 자체는 없다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   하드웨어 AER 오류 → PCI 코어 오류 복구
 *     → err_handler->error_detected = [vfio_pci_core_aer_err_detected]
 *     → eventfd_signal → 사용자 공간
 */
pci_ers_result_t vfio_pci_core_aer_err_detected(struct pci_dev *pdev,
						pci_channel_state_t state)
{
	/* [한국어] drvdata 에서 vdev 를 되찾는다.
	 * [상류 코드 관찰] NULL 검사 없이 곧바로 아래에서 역참조한다. 실제로는
	 * vfio_pci_core_register_device 의 첫 검사가 "drvdata 는 반드시 vdev" 를
	 * WARN 으로 강제하고, 등록이 끝난 뒤에야 err_handler 가 붙은 드라이버로서
	 * 알림을 받으므로 NULL 이 되는 경로를 이 트리에서 찾지 못했다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	struct vfio_pci_core_device *vdev = dev_get_drvdata(&pdev->dev);
	/* [한국어] err_trigger 슬롯에서 읽어 올 래퍼. */
	struct vfio_pci_eventfd *eventfd;

	/* [한국어] igate 를 잡지 않고 RCU 로만 보호한다. 오류 복구 경로는 잠들 수 없다. */
	rcu_read_lock();
	/* [한국어] AER 통보 채널을 읽는다. */
	eventfd = rcu_dereference(vdev->err_trigger);
	/* [한국어] 등록된 채널이 있으면. */
	if (eventfd)
		/* [한국어] eventfd 를 울려 사용자에게 넘긴다. **이 파일은 복구를 하지 않는다 —
		 * 정책이 사용자 공간에 있다.** */
		eventfd_signal(eventfd->ctx);
	/* [한국어] RCU 구간을 닫는다. */
	rcu_read_unlock();

	/* [한국어] PCI 오류 복구 상태 기계에게 "이 드라이버는 계속할 수 있다" 를 알린다.
	 * 이 값이라야 복구 절차가 진행된다. */
	return PCI_ERS_RESULT_CAN_RECOVER;
}
/* [한국어] vendor 가 자기 err_handler 에 이 함수를 꽂을 수 있게 내보낸다
 * (drivers/vfio/pci/xe/main.c:145 가 그렇게 쓴다). */
EXPORT_SYMBOL_GPL(vfio_pci_core_aer_err_detected);

/* [한국어]
 * vfio_pci_core_sriov_configure - sysfs 의 sriov_numvfs 쓰기를 받아 VF 를 켜고 끈다
 *
 * @vdev:      PF 디바이스.
 * @nr_virtfn: 만들 VF 수. 0 이면 모두 없앤다.
 * @return: 켤 때는 성공 시 nr_virtfn(sysfs 규약이 그렇다), 끌 때는 0.
 *          -EINVAL 이면 이미 켜져 있다. 그 밖은 각 단계의 오류.
 *
 * 왜 필요한가: 사용자가 PF 를 vfio-pci 로 열어 둔 채 VF 를 만들 수 있게 해
 * 준다. VF 들은 vfio_pci_bus_notifier 가 가로채 같은 드라이버로 몰아 준다.
 * 전역 목록에 자신을 등록하는 것이 여기서 일어나며, 그 등록이 VF 쪽
 * vfio_pci_vf_init 이 PF 를 찾는 근거가 된다.
 *
 * 동작 과정(켜기):
 *  1. 전역 mutex 를 잡고 **이미 목록에 있으면 -EINVAL**. 원본 주석대로
 *     목록에 넣은 스레드만 pci_enable_sriov 를 부를 수 있고, 한 번 켠 뒤에는
 *     pci_disable_sriov 를 거치지 않고 다시 켤 수 없다. 목록 소속 여부가
 *     그 상태 기계의 유일한 표시다.
 *  2. 목록에 넣고 락을 푼다.
 *  3. 런타임 PM 참조를 얻어 PF 를 깨운다. 원본 주석대로 **PF 의 전원은 항상
 *     VF 보다 높아야** 하기 때문이다.
 *  4. memory_lock 을 write 로 잡고 D0 로 올린 뒤 pci_enable_sriov 를 부른다.
 *     락을 잡는 이유도 원본 주석에 있다 — 이 함수는 언제든 불릴 수 있고,
 *     사용자의 PCI_PM_CTRL 쓰기(config 중재를 통한 전원 조작)와 경합할 수 있다.
 *  5. 성공하면 nr_virtfn 을 돌려준다.
 *
 * 동작 과정(끄기):
 *  6. VF 가 있으면 pci_disable_sriov 로 없애고 켤 때 얻은 런타임 PM 참조를 놓는다.
 *  7. out_del 로 내려가 목록에서 자신을 빼고 락을 푼다.
 *     list_del_init 을 쓰는 이유는 노드를 "빈 상태" 로 만들어 1단계의
 *     list_empty 검사가 다시 성립하게 하려는 것이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 맨 앞의 device_lock_assert 가 말하듯
 * **PCI 코어가 device_lock 을 쥔 채** 부른다(sysfs 쓰기 경로).
 *
 * 호출자: vendor 의 pci_driver.sriov_configure — vfio_pci.c:195,
 * 그리고 이 파일의 vfio_pci_core_unregister_device(0 으로).
 * 호출 대상: pci_num_vf(drivers/pci/iov.c:2524),
 * pci_enable_sriov(iov.c:2458), pci_disable_sriov(iov.c:2491),
 * pm_runtime_resume_and_get, vfio_pci_set_power_state.
 *
 * 에러 경로: 3단계와 4단계의 실패는 out_del 로 가 목록에서 자신을 빼
 * 다시 시도할 수 있게 만든다. 4단계 실패는 런타임 PM 참조도 함께 놓는다.
 * 1단계 실패만 out_unlock 으로 가 목록을 건드리지 않는다 — 이미 남의 항목이
 * 들어 있기 때문이다.
 *
 * 호출 체인:
 *   sysfs sriov_numvfs 쓰기 → PCI 코어 → pci_driver->sriov_configure
 *     → vfio_pci.c:195 → [vfio_pci_core_sriov_configure] → pci_enable_sriov
 */
int vfio_pci_core_sriov_configure(struct vfio_pci_core_device *vdev,
				  int nr_virtfn)
{
	/* [한국어] 자주 쓰는 pci_dev 포인터. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 반환값. 0 으로 시작해 끄기 경로에서 그대로 반환된다. */
	int ret = 0;

	/* [한국어] **PCI 코어가 device_lock 을 쥔 채 부른다**는 계약을 확인한다.
	 * sysfs 의 sriov_numvfs 쓰기 경로가 그 락을 잡는다. */
	device_lock_assert(&pdev->dev);

	/* [한국어] VF 를 켜는 요청. */
	if (nr_virtfn) {
		/* [한국어] 전역 PF 목록의 락을 잡는다. */
		mutex_lock(&vfio_pci_sriov_pfs_mutex);
		/*
		 * The thread that adds the vdev to the list is the only thread
		 * that gets to call pci_enable_sriov() and we will only allow
		 * it to be called once without going through
		 * pci_disable_sriov()
		 */
		/* [한국어] 이미 목록에 있으면 이 PF 는 SR-IOV 를 켠 상태다. 원본 주석대로
		 * 목록에 넣은 스레드만 pci_enable_sriov 를 부를 수 있고, 한 번 켠 뒤에는
		 * pci_disable_sriov 를 거치지 않고 다시 켤 수 없다.
		 * **목록 소속 여부가 그 상태 기계의 유일한 표시다.** */
		if (!list_empty(&vdev->sriov_pfs_item)) {
			/* [한국어] -EINVAL. */
			ret = -EINVAL;
			/* [한국어] 목록을 건드리지 않고 락만 놓는다 — 그 항목은 남의 것이 아니라
			 * 내 것이지만 지금 지우면 안 되기 때문이다. */
			goto out_unlock;
		}
		/* [한국어] 목록에 자신을 추가한다. 이 등록이 VF 쪽 vfio_pci_vf_init 이
		 * PF 를 찾는 근거가 된다. */
		list_add_tail(&vdev->sriov_pfs_item, &vfio_pci_sriov_pfs);
		/* [한국어] 목록 조작이 끝났으므로 락을 놓는다. 아래는 오래 걸릴 수 있는 작업이다. */
		mutex_unlock(&vfio_pci_sriov_pfs_mutex);

		/*
		 * The PF power state should always be higher than the VF power
		 * state. The PF can be in low power state either with runtime
		 * power management (when there is no user) or PCI_PM_CTRL
		 * register write by the user. If PF is in the low power state,
		 * then change the power state to D0 first before enabling
		 * SR-IOV. Also, this function can be called at any time, and
		 * userspace PCI_PM_CTRL write can race against this code path,
		 * so protect the same with 'memory_lock'.
		 */
		/* [한국어] **PF 를 깨워 D0 에 붙잡는다.** 원본 주석대로 PF 의 전원은 항상 VF 보다
		 * 높아야 한다. 이 참조는 SR-IOV 를 끌 때 놓는다. */
		ret = pm_runtime_resume_and_get(&pdev->dev);
		/* [한국어] 깨우기 실패. */
		if (ret)
			/* [한국어] 목록에서 자신을 빼고 나간다. */
			goto out_del;

		/* [한국어] **memory_lock 을 write 로 잡는다.** 원본 주석대로 사용자의 PCI_PM_CTRL
		 * 쓰기(config 중재를 통한 전원 조작)가 이 경로와 경합할 수 있다. */
		down_write(&vdev->memory_lock);
		/* [한국어] 런타임 PM 으로 깨웠더라도 사용자가 에뮬레이션으로 D3 에 두었을 수 있으니
		 * 확실히 D0 로 맞춘다. */
		vfio_pci_set_power_state(vdev, PCI_D0);
		/* [한국어] VF 들을 만든다(drivers/pci/iov.c:2458). 이 호출 안에서 VF 장치들이
		 * 등록되고, 그때마다 vfio_pci_bus_notifier 가 driver_override 를 심는다. */
		ret = pci_enable_sriov(pdev, nr_virtfn);
		/* [한국어] 락을 놓는다. */
		up_write(&vdev->memory_lock);
		/* [한국어] SR-IOV 켜기 실패. */
		if (ret) {
			/* [한국어] 위에서 얻은 런타임 PM 참조를 놓는다. */
			pm_runtime_put(&pdev->dev);
			/* [한국어] 목록에서 자신을 빼고 나간다. */
			goto out_del;
		}
		/* [한국어] **sysfs 규약대로 만든 VF 수를 돌려준다.** 0 이 아닌 값이 성공을 뜻한다. */
		return nr_virtfn;
	}

	/* [한국어] 끄기 요청. 실제로 VF 가 있을 때만 할 일이 있다. */
	if (pci_num_vf(pdev)) {
		/* [한국어] VF 들을 없앤다(drivers/pci/iov.c:2491). 그 과정에서 각 VF 드라이버가
		 * 제거되며, 그 VF 를 쓰던 사용자가 있으면 여기서 막힐 수 있다. */
		pci_disable_sriov(pdev);
		/* [한국어] 켤 때 얻은 런타임 PM 참조를 놓는다. 이제 PF 도 다시 잠들 수 있다. */
		pm_runtime_put(&pdev->dev);
	}

/* [한국어] 켜기 실패와 정상 끄기가 모두 여기로 온다. */
out_del:
	/* [한국어] 전역 목록의 락을 잡는다. */
	mutex_lock(&vfio_pci_sriov_pfs_mutex);
	/* [한국어] 목록에서 자신을 뺀다. _init 판을 쓰는 이유는 노드를 "빈 상태" 로 만들어
	 * 위 list_empty 검사가 다시 성립하게 하려는 것이다. */
	list_del_init(&vdev->sriov_pfs_item);
/* [한국어] 중복 켜기 실패가 여기로 온다 — 목록은 건드리지 않는다. */
out_unlock:
	/* [한국어] 락을 놓는다. */
	mutex_unlock(&vfio_pci_sriov_pfs_mutex);
	/* [한국어] 끄기 성공이면 0, 켜기 실패면 그 오류. */
	return ret;
}
/* [한국어] vendor 가 자기 sriov_configure 에서 부를 수 있게 내보낸다
 * (drivers/vfio/pci/vfio_pci.c:195). */
EXPORT_SYMBOL_GPL(vfio_pci_core_sriov_configure);

/* [한국어] PCI 오류 복구 콜백 표. vendor 가 pci_driver.err_handler 에 통째로 꽂는다
 * (drivers/vfio/pci/vfio_pci.c:211, ism/main.c:400).
 * 설정자: 이 파일의 정적 초기화.
 * 읽는 자: PCI 코어의 AER 복구 상태 기계.
 * 값 범위: error_detected 하나만 채워지고 mmio_enabled, slot_reset,
 * resume 은 NULL 이다 — 복구 정책이 사용자 공간에 있으므로 커널이 할 일이 없다.
 * 동기화: const 라 읽기 전용이다. */
const struct pci_error_handlers vfio_pci_core_err_handlers = {
	/* [한국어] 오류가 감지됐을 때 부를 콜백.
	 * 설정자: 정적 초기화.
	 * 읽는 자: PCI 코어.
	 * 값 범위: 이 파일의 함수 포인터 고정.
	 * 동기화: 없음. */
	.error_detected = vfio_pci_core_aer_err_detected,
};
/* [한국어] vendor 드라이버가 자기 pci_driver 에 이 표를 걸 수 있게 내보낸다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_err_handlers);

/* [한국어]
 * vfio_dev_in_groups - 이 디바이스가 사용자가 제출한 group 들 안에 있는지 확인한다
 *
 * @vdev:   확인할 디바이스.
 * @groups: 사용자가 제출한 group file 배열. NULL 일 수 있다.
 * @return: true 면 그 group 들 중 하나가 이 디바이스를 담고 있다.
 *
 * 왜 필요한가: 1세대 hot reset 소유권 판정의 알맹이다. 슬롯/버스 리셋 범위의
 * **모든** 디바이스에 대해 이 함수가 참이어야 리셋을 허용한다.
 *
 * 동작 과정:
 *  1. groups 가 NULL 이면 false. 2세대(iommufd) 경로에서 실수로 이 함수가
 *     불려도 "소유하지 않음" 으로 안전하게 떨어지게 하는 방어다.
 *  2. 각 group file 에 대해 vfio_file_has_dev 를 물어 하나라도 참이면 true.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock 을 쥔 채 불린다.
 *
 * 호출자: vfio_pci_dev_set_hot_reset 의 소유권 판정 루프.
 * 호출 대상: vfio_file_has_dev(drivers/vfio/group.c 에 구현).
 *
 * 에러 경로: 없다. 판정만 한다.
 *
 * 호출 체인:
 *   vfio_pci_ioctl_pci_hot_reset_groups → vfio_pci_dev_set_hot_reset
 *     → [vfio_dev_in_groups] → vfio_file_has_dev
 */
static bool vfio_dev_in_groups(struct vfio_device *vdev,
			       struct vfio_pci_group_info *groups)
{
	/* [한국어] 그룹 배열 순회 첨자. */
	unsigned int i;

	/* [한국어] 2세대(iommufd) 경로에서 실수로 이 함수가 불려도 "소유하지 않음" 으로
	 * 안전하게 떨어지게 하는 방어다. */
	if (!groups)
		/* [한국어] 소유하지 않은 것으로 본다. */
		return false;

	/* [한국어] 사용자가 제출한 group file 들을 훑는다. */
	for (i = 0; i < groups->count; i++)
		/* [한국어] 그 group 이 이 디바이스를 담고 있는지 묻는다(drivers/vfio/group.c 구현). */
		if (vfio_file_has_dev(groups->files[i], vdev))
			/* [한국어] 하나라도 담고 있으면 소유가 증명된다. */
			return true;
	/* [한국어] 어느 group 에도 없으면 소유하지 않은 것이다. */
	return false;
}

/* [한국어]
 * vfio_pci_is_device_in_set - 버스 순회 콜백: 이 PCI 함수가 dev_set 안에 있는가
 *
 * @pdev: 지금 방문한 장치.
 * @data: struct vfio_device_set 포인터.
 * @return: 0 이면 안에 있다(순회 계속). -ENODEV 면 없다(순회 중단).
 *
 * 왜 필요한가: 리셋 범위 안에 **VFIO 가 관리하지 않는 PCI 함수가 하나라도
 * 있으면 리셋해서는 안 된다.** 그 함수는 호스트 드라이버가 쓰고 있을 수 있고,
 * 리셋 사실을 통보받지 못한다. 이 콜백이 그 검사를 한 장치씩 수행한다.
 *
 * 동작 과정: vfio_find_device_in_devset 으로 찾아 있으면 0, 없으면 -ENODEV.
 * 반환값의 극성이 뒤집혀 있다는 점에 주의 — pci_walk_bus 는 0 이 아닌 값을
 * "중단" 으로 보므로, **없을 때 순회가 멈추고 그 값이 호출자에게 전달된다**.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock 을 쥔 채 순회한다.
 *
 * 호출자: vfio_pci_walk_wrapper 를 거쳐 pci_walk_bus.
 * 등록하는 곳은 vfio_pci_dev_set_resettable.
 * 호출 대상: vfio_find_device_in_devset(drivers/vfio/vfio_main.c).
 *
 * 에러 경로: -ENODEV 하나뿐이며, 그것이 곧 판정 결과다.
 *
 * 호출 체인:
 *   vfio_pci_dev_set_resettable → vfio_pci_for_each_slot_or_bus
 *     → pci_walk_bus → vfio_pci_walk_wrapper → [vfio_pci_is_device_in_set]
 */
static int vfio_pci_is_device_in_set(struct pci_dev *pdev, void *data)
{
	/* [한국어] 순회 콜백이 받은 데이터는 검사 기준이 되는 device set 이다. */
	struct vfio_device_set *dev_set = data;

	/* [한국어] **반환값의 극성이 뒤집혀 있다.** 묶음 안에 있으면 0(계속),
	 * 없으면 -ENODEV(중단)이다. pci_walk_bus 가 0 이 아닌 값을 중단으로
	 * 보므로, "밖에 있는 장치를 하나 발견한 순간" 순회가 멈추고 그 값이
	 * 호출자에게 전달된다. */
	return vfio_find_device_in_devset(dev_set, &pdev->dev) ? 0 : -ENODEV;
}

/* [한국어]
 * vfio_pci_dev_set_resettable - 이 묶음을 통째로 리셋해도 되는지 판정하고 기준 장치를 돌려준다
 *
 * @dev_set: 검사할 device set.
 * @return: 리셋해도 되면 pci_reset_bus 에 넘길 pci_dev, 아니면 NULL.
 *
 * 왜 필요한가: 원본 영어 주석이 핵심을 말한다. vfio 코어는 일부 장치가
 * pci-stub 이나 pcieport 에 묶여 있어도 group 을 "쓸 만하다" 고 보고
 * vfio_device 를 만들어 준다. 하지만 리셋은 다르다 — **리셋 범위의 모든 PCI
 * 장치가 우리 dev_set 안에 있어야** 그것들이 자리를 지키고, 그 장치를 모는
 * 모든 드라이버가 리셋에 협조할 수 있다.
 *
 * 동작 과정:
 *  1. dev_set->lock 이 잡혀 있는지 확인한다(디버그 빌드).
 *  2. 묶음의 첫 디바이스의 pci_dev 를 기준으로 삼는다. 원본 주석대로
 *     **정의상 묶음의 모든 장치는 같은 리셋 범위를 공유**하므로 어느 것을
 *     골라도 pci_probe_reset_ 계열과 pci_reset_bus 의 결과가 같다.
 *  3. 슬롯 리셋도 버스 리셋도 안 되면 NULL.
 *  4. 리셋 범위(슬롯이 되면 슬롯, 아니면 버스)의 모든 PCI 함수를 훑어
 *     하나라도 dev_set 밖에 있으면 NULL.
 *  5. 통과하면 기준 pci_dev 를 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **dev_set->lock 을 쥔 채** 불려야 한다.
 *
 * 호출자: vfio_pci_dev_set_hot_reset(사용자 요청)과
 * vfio_pci_dev_set_try_reset(닫기 때 자동).
 * 호출 대상: pci_probe_reset_slot(drivers/pci/pci.c:11012),
 * pci_probe_reset_bus(pci.c:11378) 와 vfio_pci_for_each_slot_or_bus,
 * 그리고 콜백 vfio_pci_is_device_in_set 을 쓴다.
 *
 * 에러 경로: 실패는 모두 NULL 로 표현된다. 호출자가 -EINVAL 로 바꾸거나
 * 조용히 포기한다.
 *
 * 호출 체인:
 *   vfio_pci_dev_set_hot_reset 또는 _try_reset
 *     → [vfio_pci_dev_set_resettable] → vfio_pci_for_each_slot_or_bus
 */
/*
 * vfio-core considers a group to be viable and will create a vfio_device even
 * if some devices are bound to drivers like pci-stub or pcieport. Here we
 * require all PCI devices to be inside our dev_set since that ensures they stay
 * put and that every driver controlling the device can co-ordinate with the
 * device reset.
 *
 * Returns the pci_dev to pass to pci_reset_bus() if every PCI device to be
 * reset is inside the dev_set, and pci_reset_bus() can succeed. NULL otherwise.
 */
static struct pci_dev *
vfio_pci_dev_set_resettable(struct vfio_device_set *dev_set)
{
	/* [한국어] 리셋의 기준이 될 장치. */
	struct pci_dev *pdev;

	/* [한국어] **dev_set->lock 을 쥔 채 불려야 한다.** 아래에서 묶음의 리스트를 읽기
	 * 때문이다. */
	lockdep_assert_held(&dev_set->lock);

	/*
	 * By definition all PCI devices in the dev_set share the same PCI
	 * reset, so any pci_dev will have the same outcomes for
	 * pci_probe_reset_*() and pci_reset_bus().
	 */
	/* [한국어] 묶음의 첫 디바이스를 꺼낸다. 원본 주석대로 정의상 묶음의 모든 장치는
	 * 같은 리셋 범위를 공유하므로 어느 것을 골라도 결과가 같다. */
	pdev = list_first_entry(&dev_set->device_list,
				/* [한국어] 리스트 노드에서 바깥 구조체를 되찾는 타입 정보. */
				struct vfio_pci_core_device,
				/* [한국어] 그 디바이스의 pci_dev 를 기준으로 삼는다. */
				vdev.dev_set_list)->pdev;

	/* pci_reset_bus() is supported */
	/* [한국어] 슬롯 리셋도 버스 리셋도 불가능하면(둘 다 0 이 아닌 값을 돌려주면)
	 * 이 묶음은 hot reset 을 할 수 없다. */
	if (pci_probe_reset_slot(pdev->slot) && pci_probe_reset_bus(pdev->bus))
		/* [한국어] NULL 로 "불가능" 을 알린다. */
		return NULL;

	/* [한국어] 리셋 범위의 모든 PCI 함수가 이 묶음 안에 있는지 검사한다.
	 * 하나라도 밖에 있으면 콜백이 -ENODEV 를 내 순회가 멈추고 그 값이 돌아온다. */
	if (vfio_pci_for_each_slot_or_bus(pdev, vfio_pci_is_device_in_set,
					  /* [한국어] 묶음 자체를 콜백 데이터로 넘긴다. */
					  dev_set,
					  /* [한국어] 슬롯 리셋이 가능하면 슬롯 범위로, 아니면 버스 범위로 훑는다.
					   * 0 이 "가능" 이므로 ! 로 뒤집어 slot 인자를 만든다. */
					  !pci_probe_reset_slot(pdev->slot)))
		/* [한국어] 밖에 있는 장치가 있으면 리셋할 수 없다. */
		return NULL;
	/* [한국어] 통과. 이 pci_dev 를 pci_reset_bus 에 넘기면 된다. */
	return pdev;
}

/* [한국어]
 * vfio_pci_dev_set_pm_runtime_get - 묶음의 모든 디바이스를 D0 로 깨우고 붙잡는다
 *
 * @dev_set: 대상 묶음.
 * @return: 0 성공. 음수면 도중에 실패했고, **그때까지 얻은 참조는 모두
 *          되돌려 놓은 상태**다.
 *
 * 왜 필요한가: 리셋 전에 묶음의 모든 장치가 깨어 있어야 한다. 잠든 장치는
 * 리셋 신호에 제대로 반응하지 않고, 리셋 후 상태 복원도 어긋난다. 여러
 * 장치의 참조를 순서대로 얻는 일이라 **부분 실패의 되감기**가 필요하고,
 * 그것이 이 함수의 절반을 차지한다.
 *
 * 동작 과정:
 *  1. 목록을 앞에서부터 훑으며 pm_runtime_resume_and_get 을 부른다.
 *  2. 하나라도 실패하면 unwind 로 뛴다.
 *  3. unwind 는 list_for_each_entry_continue_reverse 로 **실패한 항목의
 *     바로 앞부터 거꾸로** 훑으며 참조를 놓는다. 실패한 항목 자신은
 *     resume_and_get 이 내부에서 이미 정리했으므로 건드리지 않는다.
 *     이 매크로가 그 "직전 항목부터" 라는 의미를 정확히 구현한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock 을 쥔 채 불린다.
 * pm_runtime_resume_and_get 은 잠들 수 있다.
 *
 * 호출자: vfio_pci_dev_set_hot_reset 과 vfio_pci_dev_set_try_reset.
 * 두 호출자 모두 리셋이 끝나면 짝이 되는 pm_runtime_put 루프를 돈다.
 * 호출 대상: pm_runtime_resume_and_get.
 *
 * 에러 경로: 위 3단계의 되감기가 전부다. 참조 계수가 새지 않는다.
 *
 * 호출 체인:
 *   vfio_pci_dev_set_hot_reset / _try_reset
 *     → [vfio_pci_dev_set_pm_runtime_get] → pm_runtime_resume_and_get
 */
static int vfio_pci_dev_set_pm_runtime_get(struct vfio_device_set *dev_set)
{
	/* [한국어] 묶음을 순회할 커서. unwind 라벨이 이 값을 이어받으므로 루프 밖에 선언된다. */
	struct vfio_pci_core_device *cur;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] 묶음의 모든 디바이스를 앞에서부터 훑는다. */
	list_for_each_entry(cur, &dev_set->device_list, vdev.dev_set_list) {
		/* [한국어] 장치를 깨우고 usage count 를 하나 올린다. 잠든 장치는 리셋 신호에
		 * 제대로 반응하지 않는다. */
		ret = pm_runtime_resume_and_get(&cur->pdev->dev);
		/* [한국어] 하나라도 실패하면. */
		if (ret)
			/* [한국어] 지금까지 얻은 참조를 되돌리러 간다. */
			goto unwind;
	}

	/* [한국어] 전부 성공. 호출자가 리셋 뒤에 짝이 되는 put 루프를 돈다. */
	return 0;

/* [한국어] 부분 실패 되감기. */
unwind:
	/* [한국어] **실패한 항목의 바로 앞부터 거꾸로** 훑는다. 실패한 항목 자신은
	 * pm_runtime_resume_and_get 이 내부에서 이미 정리했으므로 건드리지 않는다 —
	 * 이 매크로가 정확히 그 의미를 구현한다. */
	list_for_each_entry_continue_reverse(cur, &dev_set->device_list,
					     vdev.dev_set_list)
		/* [한국어] 얻었던 참조를 놓는다. */
		pm_runtime_put(&cur->pdev->dev);

	/* [한국어] 실패 원인을 그대로 돌려준다. 참조 계수는 원상 복구된 상태다. */
	return ret;
}

/* [한국어]
 * vfio_pci_dev_set_hot_reset - 슬롯/버스 리셋의 실체: 소유권 확인, 매핑 회수, 리셋, 되감기
 *
 * @dev_set:     리셋할 묶음.
 * @groups:      1세대 소유 증거(group file 배열). 2세대면 NULL.
 * @iommufd_ctx: 2세대 소유 증거(호출자의 iommufd 컨텍스트). 1세대면 NULL.
 * @return: 0 성공. -EINVAL(리셋 불가한 묶음 또는 소유하지 않은 장치가 있음),
 *          -EBUSY(다른 스레드가 memory_lock 을 쥐고 있음),
 *          그 밖은 pci_reset_bus 의 반환값.
 *
 * 왜 필요한가: **이 파일에서 가장 조심스러운 함수다.** 여러 디바이스의 락을
 * 겹쳐 잡는 유일한 곳이고, 잘못하면 교착하거나 리셋 중인 장치를 사용자가
 * 만지게 된다. 그래서 세 가지 장치를 쓴다 — trylock, 실패 시 역순 되감기,
 * 그리고 리셋 전 BAR 매핑 회수.
 *
 * 동작 과정:
 *  1. dev_set->lock 을 잡는다. 이 락이 묶음 전체의 직렬화 지점이다.
 *  2. vfio_pci_dev_set_resettable 로 리셋 가능 여부와 기준 장치를 얻는다.
 *  3. 묶음의 모든 장치를 D0 로 깨우고 붙잡는다. 원본 주석대로 일부가
 *     런타임 서스펜드 상태일 수 있기 때문이다.
 *  4. 묶음의 각 디바이스에 대해 차례로:
 *     a. **소유권 판정.** 원본 영어 주석이 두 문법을 설명한다.
 *        2세대(iommufd_ctx 가 있음)면 vfio_iommufd_get_dev_id 가 양수이거나
 *        -ENOENT 면 소유로 본다. -ENOENT 를 소유로 세는 이유도 주석에 있다 —
 *        아직 어떤 iommufd 에도 바인딩되지 않은 장치는, 그 iommu_group 이
 *        이미 이 iommufd 에 소유돼 있으므로 다른 iommufd 가 가져갈 수 없기
 *        때문이다. 1세대면 vfio_dev_in_groups 로 본다.
 *        소유하지 않은 장치가 하나라도 있으면 -EINVAL 로 루프를 깬다.
 *     b. **memory_lock 을 write 로 trylock.** 원본 주석대로 여러 디바이스의
 *        락을 잡는 일은 교착과 폭주에 취약하므로, 경합하면 즉시 -EBUSY 로
 *        물러서고 되감는다. 잠들며 기다리지 않는 것이 핵심이다.
 *     c. dma-buf 를 revoke 하고 BAR 매핑을 걷어낸다. **이 순서로 모든
 *        디바이스를 잠근 뒤에야 리셋이 시작된다.**
 *  5. 루프가 정상 종료했는지(list_entry_is_head 로) 확인한다. 중간에 break
 *     했다면 vdev 는 실패한 항목을 가리키므로, 그 **직전 항목**부터 되감아야
 *     한다 — list_prev_entry 로 한 칸 물린 뒤 err_undo 로 간다.
 *  6. 정상이면 묶음 전체를 D0 로 올린다. 원본 주석대로 pci_reset_bus 가
 *     내부에서 D0 로 올리기는 하지만 그러면 NoSoftRst- 장치의 pm_save 복원이
 *     건너뛰어진다.
 *  7. pci_reset_bus(drivers/pci/pci.c:11418) — 슬롯 리셋이 가능하면 슬롯을,
 *     아니면 버스를 리셋한다. 그 함수의 주석이 "VFIO 가 장치를 게스트에게
 *     넘기기 전후로 부르는 것이 주된 용례" 라고 적은 그 호출자가 여기다.
 *  8. 되감기 위치를 마지막 항목으로 맞춘다.
 *  9. err_undo: 역순으로 훑으며 **아직 열려 있고 메모리 디코드가 켜진**
 *     디바이스만 dma-buf 를 되살리고, memory_lock 을 푼다. 열림 여부를 보는
 *     이유는 리셋 도중 닫힌 디바이스의 dma-buf 를 되살릴 필요가 없기 때문이다.
 * 10. 3단계에서 얻은 런타임 PM 참조를 모두 놓고 락을 푼다.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock 과 여러 개의 memory_lock 을
 * 동시에 쥔다. 잠들 수 있지만, memory_lock 은 trylock 이라 그 획득에서는
 * 잠들지 않는다.
 *
 * 호출자: vfio_pci_ioctl_pci_hot_reset_groups(1세대)와
 * vfio_pci_ioctl_pci_hot_reset(2세대).
 * 호출 대상: vfio_pci_dev_set_resettable, vfio_pci_dev_set_pm_runtime_get,
 * vfio_iommufd_get_dev_id, vfio_dev_in_groups, vfio_pci_dma_buf_move,
 * vfio_pci_zap_bars, vfio_pci_set_power_state, pci_reset_bus,
 * __vfio_pci_memory_enabled.
 *
 * 에러 경로: 어느 지점에서 실패하든 err_undo 와 err_unlock 을 지나
 * 잡은 락과 참조를 정확히 되돌린다. 실패해도 디바이스는 리셋 전 상태로
 * 남고 사용자 fd 는 살아 있다 — 다만 4c 에서 걷어낸 BAR 매핑은 돌아오지
 * 않고, 사용자의 다음 접근이 폴트로 다시 만든다.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_PCI_HOT_RESET)
 *     → vfio_pci_ioctl_pci_hot_reset(_groups)
 *     → [vfio_pci_dev_set_hot_reset] → pci_reset_bus(drivers/pci/pci.c:11418)
 */
static int vfio_pci_dev_set_hot_reset(struct vfio_device_set *dev_set,
				      struct vfio_pci_group_info *groups,
				      struct iommufd_ctx *iommufd_ctx)
{
	/* [한국어] 묶음을 순회할 커서. 되감기 라벨들이 이 값을 이어받는다. */
	struct vfio_pci_core_device *vdev;
	/* [한국어] 리셋의 기준이 될 장치. */
	struct pci_dev *pdev;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/* [한국어] **묶음 전체의 직렬화 지점.** 두 스레드가 동시에 hot reset 을 하면
	 * 락 획득 순서가 엇갈려 교착한다. */
	mutex_lock(&dev_set->lock);

	/* [한국어] 리셋 가능 여부와 기준 장치를 얻는다. */
	pdev = vfio_pci_dev_set_resettable(dev_set);
	/* [한국어] 불가능한 묶음. */
	if (!pdev) {
		/* [한국어] -EINVAL. */
		ret = -EINVAL;
		/* [한국어] 락만 놓고 나간다. */
		goto err_unlock;
	}

	/*
	 * Some of the devices in the dev_set can be in the runtime suspended
	 * state. Increment the usage count for all the devices in the dev_set
	 * before reset and decrement the same after reset.
	 */
	/* [한국어] 묶음의 모든 장치를 깨워 붙잡는다. 원본 주석대로 일부가 런타임 서스펜드
	 * 상태일 수 있다. */
	ret = vfio_pci_dev_set_pm_runtime_get(dev_set);
	/* [한국어] 깨우기 실패(이미 되감긴 상태). */
	if (ret)
		/* [한국어] 락만 놓고 나간다. */
		goto err_unlock;

	/* [한국어] 묶음의 각 디바이스에 대해 소유권 확인과 잠금을 차례로 수행한다. */
	list_for_each_entry(vdev, &dev_set->device_list, vdev.dev_set_list) {
		/* [한국어] 이 디바이스를 호출자가 소유하는가. */
		bool owned;

		/*
		 * Test whether all the affected devices can be reset by the
		 * user.
		 *
		 * If called from a group opened device and the user provides
		 * a set of groups, all the devices in the dev_set should be
		 * contained by the set of groups provided by the user.
		 *
		 * If called from a cdev opened device and the user provides
		 * a zero-length array, all the devices in the dev_set must
		 * be bound to the same iommufd_ctx as the input iommufd_ctx.
		 * If there is any device that has not been bound to any
		 * iommufd_ctx yet, check if its iommu_group has any device
		 * bound to the input iommufd_ctx.  Such devices can be
		 * considered owned by the input iommufd_ctx as the device
		 * cannot be owned by another iommufd_ctx when its iommu_group
		 * is owned.
		 *
		 * Otherwise, reset is not allowed.
		 */
		/* [한국어] 2세대(cdev) 문법 — iommufd 컨텍스트로 판정한다. */
		if (iommufd_ctx) {
			/* [한국어] 호출자의 iommufd 안에서 이 디바이스의 id 를 묻는다. */
			int devid = vfio_iommufd_get_dev_id(&vdev->vdev,
							    iommufd_ctx);

			/* [한국어] 양수면 정식 바인딩, -ENOENT 면 아직 어떤 iommufd 에도 바인딩되지
			 * 않은 상태다. 원본 주석대로 후자도 소유로 세는데, 그 iommu_group 이
			 * 이미 이 iommufd 에 소유돼 있어 다른 iommufd 가 가져갈 수 없기 때문이다. */
			owned = (devid > 0 || devid == -ENOENT);
		/* [한국어] 1세대(group) 문법. */
		} else {
			/* [한국어] 사용자가 제출한 group file 들 안에 이 디바이스가 있는지 본다. */
			owned = vfio_dev_in_groups(&vdev->vdev, groups);
		}

		/* [한국어] 소유하지 않은 장치를 만났다. */
		if (!owned) {
			/* [한국어] -EINVAL. */
			ret = -EINVAL;
			/* [한국어] 루프를 멈춘다. vdev 는 실패한 항목을 가리킨 채 남는다. */
			break;
		}

		/*
		 * Take the memory write lock for each device and zap BAR
		 * mappings to prevent the user accessing the device while in
		 * reset.  Locking multiple devices is prone to deadlock,
		 * runaway and unwind if we hit contention.
		 */
		/* [한국어] **memory_lock 을 write 로 trylock 한다.** 원본 주석대로 여러 디바이스의
		 * 락을 잡는 일은 교착과 폭주에 취약하므로 잠들며 기다리지 않는다. */
		if (!down_write_trylock(&vdev->memory_lock)) {
			/* [한국어] -EBUSY. 사용자는 잠시 뒤 다시 시도하면 된다. */
			ret = -EBUSY;
			/* [한국어] 루프를 멈춘다. */
			break;
		}

		/* [한국어] 이 BAR 를 빌려 준 dma-buf 를 무효화한다(revoked=true). */
		vfio_pci_dma_buf_move(vdev, true);
		/* [한국어] **사용자 BAR 매핑을 걷어낸다.** 이 순서로 묶음의 모든 디바이스를 잠근
		 * 뒤에야 리셋이 시작된다. */
		vfio_pci_zap_bars(vdev);
	}

	/* [한국어] 루프가 정상 종료했는가. 정상 종료면 vdev 는 리스트 헤드를 가리킨다
	 * (list_for_each_entry 의 종료 조건). 아니면 break 로 나온 것이다. */
	if (!list_entry_is_head(vdev,
				&dev_set->device_list, vdev.dev_set_list)) {
		/* [한국어] 실패한 항목은 락을 잡지 못했으므로 **직전 항목**부터 되감아야 한다. */
		vdev = list_prev_entry(vdev, vdev.dev_set_list);
		/* [한국어] 잡은 락들을 역순으로 푼다. */
		goto err_undo;
	}

	/*
	 * The pci_reset_bus() will reset all the devices in the bus.
	 * The power state can be non-D0 for some of the devices in the bus.
	 * For these devices, the pci_reset_bus() will internally set
	 * the power state to D0 without vfio driver involvement.
	 * For the devices which have NoSoftRst-, the reset function can
	 * cause the PCI config space reset without restoring the original
	 * state (saved locally in 'vdev->pm_save').
	 */
	/* [한국어] 묶음 전체를 D0 로 올린다. 원본 주석대로 pci_reset_bus 가 스스로 올리면
	 * NoSoftRst- 장치의 pm_save 복원이 건너뛰어진다. */
	list_for_each_entry(vdev, &dev_set->device_list, vdev.dev_set_list)
		/* [한국어] 각 디바이스의 전원을 D0 로 맞춘다. */
		vfio_pci_set_power_state(vdev, PCI_D0);

	/* [한국어] **슬롯 또는 버스 리셋.** drivers/pci/pci.c:11418 의 주석이 "VFIO 가 장치를
	 * 게스트에게 넘기기 전후로 부르는 것이 주된 용례" 라고 적은 그 호출자가 여기다.
	 * 그 함수는 슬롯 리셋이 가능하면 슬롯을, 아니면 버스를 리셋한다. */
	ret = pci_reset_bus(pdev);

	/* [한국어] 정상 경로의 되감기 시작점은 마지막 항목이다 — 전부 잠갔기 때문이다. */
	vdev = list_last_entry(&dev_set->device_list,
			       /* [한국어] 리스트 노드에서 바깥 구조체를 되찾는 타입 정보. */
			       struct vfio_pci_core_device, vdev.dev_set_list);

/* [한국어] 정상 경로와 실패 경로가 여기서 만난다. 이 시점에 vdev 는 "락을 잡은
 * 마지막 항목" 을 가리킨다. */
err_undo:
	/* [한국어] 그 항목부터 앞쪽으로 거꾸로 훑는다. */
	list_for_each_entry_from_reverse(vdev, &dev_set->device_list,
					 vdev.dev_set_list) {
		/* [한국어] **아직 열려 있고** 메모리 디코드가 켜진 디바이스만 dma-buf 를 되살린다.
		 * 리셋 도중 닫힌 디바이스는 되살릴 필요가 없고, 리셋으로 PCI_COMMAND 가
		 * 초기화되어 디코드가 꺼졌을 수도 있다. */
		if (vdev->vdev.open_count && __vfio_pci_memory_enabled(vdev))
			/* [한국어] dma-buf 를 다시 유효화한다. */
			vfio_pci_dma_buf_move(vdev, false);
		/* [한국어] 그 디바이스의 memory_lock 을 푼다. */
		up_write(&vdev->memory_lock);
	}

	/* [한국어] 묶음의 모든 디바이스를 다시 훑으며 */
	list_for_each_entry(vdev, &dev_set->device_list, vdev.dev_set_list)
		/* [한국어] 리셋 전에 얻은 런타임 PM 참조를 놓는다. */
		pm_runtime_put(&vdev->pdev->dev);

/* [한국어] 모든 실패 경로와 정상 경로가 여기로 모인다. */
err_unlock:
	/* [한국어] 묶음 락을 놓는다. */
	mutex_unlock(&dev_set->lock);
	/* [한국어] 리셋 결과를 사용자에게 돌려준다. */
	return ret;
}

/* [한국어]
 * vfio_pci_dev_set_needs_reset - 묶음이 "놀고 있으면서 더러운" 상태인지 판정한다
 *
 * @dev_set: 검사할 묶음.
 * @return: true 면 리셋을 시도할 값어치가 있다.
 *
 * 왜 필요한가: 닫을 때의 자동 리셋은 **아무도 쓰고 있지 않을 때만** 해도
 * 된다. 다른 디바이스가 열려 있는데 버스 리셋을 하면 그 사용자의 장치가
 * 갑자기 초기화된다. 그리고 아무도 더럽히지 않았다면 리셋할 이유도 없다.
 * 두 조건을 한 함수로 묶었다.
 *
 * 동작 과정:
 *  1. 묶음 안에서 열린 VFIO 디바이스 수가 1 보다 크면 false. 1 인 이유는
 *     **지금 닫는 중인 자기 자신이 아직 세어지기 때문**이다. 즉 "나 말고는
 *     아무도 없다" 가 조건이다.
 *  2. 묶음의 모든 디바이스의 needs_reset 을 OR 로 모은다. 하나라도 더러우면
 *     참이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. dev_set->lock 을 쥔 채 불린다
 * (vfio_pci_core_disable 이 그 락을 쥐고 있고, 그 계약이 그 함수의
 * lockdep_assert_held 로 못 박혀 있다).
 *
 * 호출자: vfio_pci_dev_set_try_reset.
 * 호출 대상: vfio_device_set_open_count(drivers/vfio/vfio_main.c).
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_core_disable → vfio_pci_dev_set_try_reset
 *     → [vfio_pci_dev_set_needs_reset]
 */
static bool vfio_pci_dev_set_needs_reset(struct vfio_device_set *dev_set)
{
	/* [한국어] 묶음을 순회할 커서. */
	struct vfio_pci_core_device *cur;
	/* [한국어] 누적 결과. 하나라도 더러우면 참이 된다. */
	bool needs_reset = false;

	/* No other VFIO device in the set can be open. */
	/* [한국어] **"나 말고는 아무도 없다" 가 조건이다.** 지금 닫는 중인 자기 자신이
	 * 아직 세어지므로 1 이 기준이다. 1 보다 크면 다른 사용자가 있다는 뜻이고,
	 * 그 상태에서 버스 리셋을 하면 남의 장치가 갑자기 초기화된다. */
	if (vfio_device_set_open_count(dev_set) > 1)
		/* [한국어] 리셋하지 않는다. */
		return false;

	/* [한국어] 묶음의 모든 디바이스를 훑으며 */
	list_for_each_entry(cur, &dev_set->device_list, vdev.dev_set_list)
		/* [한국어] needs_reset 을 OR 로 모은다. 하나라도 더러우면 리셋할 값어치가 있다. */
		needs_reset |= cur->needs_reset;
	/* [한국어] 판정 결과를 돌려준다. */
	return needs_reset;
}

/* [한국어]
 * vfio_pci_dev_set_try_reset - 조건이 맞으면 묶음 전체를 버스/슬롯 리셋한다(닫기 때의 마지막 보루)
 *
 * @dev_set: 대상 묶음.
 * @return: 없음. 실패해도 알리지 않는다 — 최선 노력(best effort)이다.
 *
 * 왜 필요한가: 원본 영어 주석이 세 조건을 나열한다. 슬롯/버스 리셋이 가능하고,
 * 영향 받는 장치가 모두 놀고 있고, 그중 하나라도 needs_reset 으로 더럽혀져
 * 있으면(예: FLR 을 지원하지 않아 닫을 때 리셋하지 못한 장치) 시도한다.
 * 함수 단위 리셋이 실패해 사용자의 흔적이 남은 장치를 다음 사용자에게
 * 넘기지 않으려는 안전망이다.
 *
 * 동작 과정:
 *  1. 조건 판정. 아니면 조용히 반환.
 *  2. 리셋 가능 여부와 기준 장치 확인. 아니면 조용히 반환.
 *  3. 절전이 켜져 있으면 묶음 전체를 깨워 붙잡는다. **절전이 꺼져 있으면
 *     이 단계를 건너뛴다** — disable_idle_d3 가 참이면 장치들이 애초에
 *     D0 에 머물기 때문이다. 그래서 아래 put 도 같은 조건으로 감싼다.
 *  4. pci_reset_bus 를 부른다.
 *  5. 리셋에 성공했으면 묶음 전체의 needs_reset 을 내리고, 3단계에서 얻은
 *     참조를 조건에 맞춰 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **dev_set->lock 을 쥔 채** 불린다
 * (호출자인 vfio_pci_core_disable 이 그 계약을 지킨다).
 * vfio_pci_dev_set_hot_reset 과 달리 여기서는 memory_lock 을 잡지 않는데,
 * "아무도 열고 있지 않다" 가 이미 확인됐으므로 걷어낼 매핑도 경합할 상대도
 * 없기 때문이다.
 *
 * 호출자: vfio_pci_core_disable 의 마지막 부분.
 * 호출 대상: vfio_pci_dev_set_needs_reset, vfio_pci_dev_set_resettable,
 * vfio_pci_dev_set_pm_runtime_get, pci_reset_bus, pm_runtime_put.
 *
 * 에러 경로: 모든 실패가 조용한 조기 반환이다. 리셋에 실패하면 needs_reset 이
 * 그대로 남아 다음 기회에 다시 시도된다.
 *
 * 호출 체인:
 *   사용자 close(2) → vfio_pci_core_close_device → vfio_pci_core_disable
 *     → [vfio_pci_dev_set_try_reset] → pci_reset_bus
 */
/*
 * If a bus or slot reset is available for the provided dev_set and:
 *  - All of the devices affected by that bus or slot reset are unused
 *  - At least one of the affected devices is marked dirty via
 *    needs_reset (such as by lack of FLR support)
 * Then attempt to perform that bus or slot reset.
 */
static void vfio_pci_dev_set_try_reset(struct vfio_device_set *dev_set)
{
	/* [한국어] 묶음을 순회할 커서. */
	struct vfio_pci_core_device *cur;
	/* [한국어] 리셋의 기준이 될 장치. */
	struct pci_dev *pdev;
	/* [한국어] 리셋에 성공했는지 표시. */
	bool reset_done = false;

	/* [한국어] "놀고 있으면서 더럽다" 는 조건을 확인한다. */
	if (!vfio_pci_dev_set_needs_reset(dev_set))
		/* [한국어] 아니면 조용히 물러선다. */
		return;

	/* [한국어] 리셋 가능 여부와 기준 장치를 얻는다. */
	pdev = vfio_pci_dev_set_resettable(dev_set);
	/* [한국어] 불가능한 묶음. */
	if (!pdev)
		/* [한국어] 조용히 물러선다. */
		return;

	/*
	 * Some of the devices in the bus can be in the runtime suspended
	 * state. Increment the usage count for all the devices in the dev_set
	 * before reset and decrement the same after reset.
	 */
	/* [한국어] 절전이 켜져 있을 때만 깨워 붙잡는다. 꺼져 있으면 장치들이 애초에
	 * D0 에 머물기 때문이다. 깨우기에 실패하면 리셋을 포기한다 —
	 * 단축 평가 덕분에 절전이 꺼져 있으면 두 번째 항이 아예 실행되지 않는다. */
	if (!disable_idle_d3 && vfio_pci_dev_set_pm_runtime_get(dev_set))
		/* [한국어] 조용히 물러선다. needs_reset 은 그대로 남아 다음 기회에 다시 시도된다. */
		return;

	/* [한국어] **슬롯 또는 버스 리셋.** 여기서는 memory_lock 을 잡지 않는데,
	 * "아무도 열고 있지 않다" 가 이미 확인됐으므로 걷어낼 매핑도 경합할
	 * 상대도 없기 때문이다. */
	if (!pci_reset_bus(pdev))
		/* [한국어] 성공을 기록한다. */
		reset_done = true;

	/* [한국어] 묶음의 모든 디바이스를 훑으며 뒷정리한다. */
	list_for_each_entry(cur, &dev_set->device_list, vdev.dev_set_list) {
		/* [한국어] 리셋에 성공했으면. */
		if (reset_done)
			/* [한국어] 더 이상 더럽지 않다. 다음 사용자에게 깨끗한 장치를 넘길 수 있다. */
			cur->needs_reset = false;

		/* [한국어] 위에서 참조를 얻었던 조건과 같아야 계수가 맞는다. */
		if (!disable_idle_d3)
			/* [한국어] 얻었던 참조를 놓는다. */
			pm_runtime_put(&cur->pdev->dev);
	}
}

/* [한국어]
 * vfio_pci_core_set_params - 실제 드라이버의 모듈 파라미터를 코어의 전역 변수로 옮긴다
 *
 * @is_nointxmask:   INTx 마스킹을 쓰지 않을지.
 * @is_disable_vga:  레거시 VGA region 을 감출지.
 * @is_disable_idle_d3: 유휴 시 D3 절전을 끌지.
 * @return: 없음.
 *
 * 왜 필요한가: 모듈 파라미터는 **실제 드라이버 모듈**(vfio-pci)에 선언돼
 * 있는데, 그 값을 읽어 쓰는 코드는 이 코어 모듈에 있다. 코어에 직접
 * module_param 을 두면 파라미터 이름이 코어 모듈 아래로 가서 사용자 인터페이스가
 * 달라지므로, 값만 이 함수로 넘겨받는다.
 *
 * 동작 과정: 세 전역 변수에 그대로 대입한다. 락이 없다 — 모듈 초기화 시점에
 * 한 번만 불리는 것이 전제다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 실제 드라이버의 module_init 안이며,
 * 아직 어떤 디바이스도 등록되지 않은 상태다.
 *
 * 호출자: drivers/vfio/pci/vfio_pci.c:265 의 모듈 초기화.
 * 호출 대상: 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio-pci 모듈 로드 → vfio_pci.c 의 init → [vfio_pci_core_set_params]
 */
void vfio_pci_core_set_params(bool is_nointxmask, bool is_disable_vga,
			      bool is_disable_idle_d3)
{
	/* [한국어] INTx 마스킹 조사 여부. */
	nointxmask = is_nointxmask;
	/* [한국어] 레거시 VGA region 노출 여부. */
	disable_vga = is_disable_vga;
	/* [한국어] 유휴 시 D3 절전 여부. 락이 없는 이유는 모듈 초기화 시점에 한 번만
	 * 불리는 것이 전제이기 때문이다. */
	disable_idle_d3 = is_disable_idle_d3;
}
/* [한국어] 실제 드라이버(vfio-pci 등)가 자기 module_init 에서 부를 수 있게 내보낸다
 * (drivers/vfio/pci/vfio_pci.c:265). */
EXPORT_SYMBOL_GPL(vfio_pci_core_set_params);

/* [한국어]
 * vfio_pci_core_cleanup - 이 모듈이 내려갈 때 공유 권한표를 해제한다
 *
 * @return: 없음.
 *
 * 왜 필요한가: 아래 vfio_pci_core_init 이 잡은 유일한 전역 자원을 되돌린다.
 * 그 자원이 config 공간 권한표(perm_bits)이며, 모든 디바이스가 공유한다.
 *
 * 동작 과정: vfio_pci_uninit_perm_bits(drivers/vfio/pci/vfio_pci_config.c:1091)
 * 한 줄. 그 함수가 capability 별 권한표 배열들을 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. module_exit 경로이며, 이 시점에는 이 코어를
 * 쓰는 드라이버가 모두 언로드된 뒤다(모듈 의존성이 그것을 보장한다).
 *
 * 호출자: 아래 module_exit 로 등록되어 커널 모듈 코어가 부른다.
 * 호출 대상: vfio_pci_uninit_perm_bits.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   rmmod vfio-pci-core → module_exit → [vfio_pci_core_cleanup]
 *     → vfio_pci_config.c:vfio_pci_uninit_perm_bits
 */
static void vfio_pci_core_cleanup(void)
{
	/* [한국어] config 공간 권한표를 해제한다(vfio_pci_config.c:1091).
	 * 이 모듈이 잡은 유일한 전역 자원이다. */
	vfio_pci_uninit_perm_bits();
}

/* [한국어]
 * vfio_pci_core_init - 이 모듈이 올라올 때 config 공간 권한표를 만든다
 *
 * @return: 0 성공. -ENOMEM 이면 권한표 할당 실패이며 모듈이 올라오지 않는다.
 *
 * 왜 필요한가: **config 중재의 기반을 세우는 자리다.** 어느 바이트의 어느
 * 비트가 가상화되고 쓰기 가능한지는 capability 종류마다 정해져 있고,
 * 디바이스마다 달라지지 않는다. 그래서 디바이스별로 만들지 않고 모듈 전역에
 * 한 벌만 만들어 공유한다. 원본 영어 주석이 그 사실을 그대로 말한다.
 *
 * 동작 과정: vfio_pci_init_perm_bits(drivers/vfio/pci/vfio_pci_config.c:1105)
 * 한 줄. 그 함수가 기본 헤더, PM, VPD, PCI-X, PCIe, AF, AER 등 capability 별
 * 권한표를 만들어 둔다. 실제 표의 내용은 그 파일의 670줄 이후에 있고,
 * 이 파일 상단 블록의 "무엇이 중재되고 무엇이 그대로 통과하는가" 절에
 * 주요 항목을 정리해 두었다.
 *
 * 실행 컨텍스트: 프로세스 문맥. module_init 이며 __init 섹션에 놓여
 * 초기화가 끝나면 코드가 해제된다.
 *
 * 호출자: 아래 module_init 로 등록되어 커널 모듈 코어가 부른다.
 * 호출 대상: vfio_pci_init_perm_bits.
 *
 * 에러 경로: 실패하면 모듈 적재가 실패하고, 이 코어에 의존하는 모든
 * vfio-pci 계열 드라이버도 올라오지 못한다.
 *
 * 호출 체인:
 *   modprobe vfio-pci-core → module_init → [vfio_pci_core_init]
 *     → vfio_pci_config.c:vfio_pci_init_perm_bits
 */
static int __init vfio_pci_core_init(void)
{
	/* Allocate shared config space permission data used by all devices */
	/* [한국어] **config 중재의 기반인 권한표를 만든다**(vfio_pci_config.c:1105).
	 * 실패하면 모듈 적재가 실패한다. */
	return vfio_pci_init_perm_bits();
}

/* [한국어] 모듈 적재 시 위 초기화 함수를 부르도록 등록한다. */
module_init(vfio_pci_core_init);
/* [한국어] 모듈 해제 시 정리 함수를 부르도록 등록한다. */
module_exit(vfio_pci_core_cleanup);

/* [한국어] 라이선스 선언. 이 문자열이 GPL 계열이라야 이 파일의 EXPORT_SYMBOL_GPL
 * 심볼들을 다른 모듈이 쓸 수 있고, 커널이 taint 되지 않는다. */
MODULE_LICENSE("GPL v2");
/* [한국어] modinfo 에 보일 저자. 위 DRIVER_AUTHOR 매크로의 유일한 사용처다. */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] modinfo 에 보일 설명. 위 DRIVER_DESC 매크로의 유일한 사용처다. */
MODULE_DESCRIPTION(DRIVER_DESC);
