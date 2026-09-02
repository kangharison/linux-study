// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO PCI I/O Port & MMIO access
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */
/* [한국어 설명] VFIO PCI 의 read(2)/write(2) 본체 — 장치 BAR 와 ROM 과 레거시 VGA 를
 * 바이트 단위로 중계하고, 중계 도중 특정 구간을 도려내는 곳
 * (drivers/vfio/pci/vfio_pci_rdwr.c)
 *
 * 인용 규칙: 이 파일 자신은 주석이 붙으며 줄 번호가 계속 밀리므로 함수 이름으로만
 * 가리킨다. 형제 파일 vfio_pci_config.c 와 vfio_pci_intrs.c 는 지금 다른 작업자가
 * 주석을 붙이고 있어 줄 번호가 유동적이라 역시 함수 이름으로만 인용한다. 반대로
 * drivers/pci 전체와 drivers/vfio 의 코어(vfio_main.c, virqfd.c, vfio_pci_core.c)는
 * 이미 주석 완료 상태이므로 실제 줄 번호로 인용한다. 이 트리는 sparse checkout 이라
 * drivers/iommu 가 통째로 없고 include/linux 에는 여섯 개 헤더(blkdev.h, blk-mq.h,
 * iommufd.h, nvme.h, vfio.h, vfio_pci_core.h)만 있다. 다만 include/uapi/linux/vfio.h 는
 * 있어 사용자 ABI 상수는 그쪽에서 확인할 수 있다. 그래서 pci.h, io.h, vgaarb.h,
 * uaccess.h 안의 선언과 인라인 정의는 "이 트리에서 확인 못 함" 으로 적었다.
 *
 * === 파일의 역할 ===
 * 사용자 공간이 장치 레지스터를 만지는 두 통로 중 **중재되는 쪽**의 실체다.
 * vfio_pci_core.c 의 파일 상단 주석이 이미 정리해 둔 대로, mmap(2) 는 BAR 의 물리
 * 페이지를 VM_PFNMAP 으로 그대로 사용자 주소 공간에 걸어 주는 직통 경로이고,
 * read(2)/write(2) 는 vfio_pci_bar_rw 를 거치며 MSI-X 테이블 구간이 제외된다. 이
 * 파일은 그 후자의 구현이다. 그 사실을 새로 발견한 것이 아니라, **그 제외가
 * 정확히 어떤 산술로 이루어지는지**를 여기서 채워 넣는다.
 *
 * 구체적으로 네 덩어리다.
 *  (1) **폭별 MMIO 접근 래퍼** — VFIO_IOWRITE 와 VFIO_IOREAD 매크로가
 *      vfio_pci_core_iowrite8/16/32/64 와 vfio_pci_core_ioread8/16/32/64 여덟 개를
 *      찍어낸다. 각각은 memory_lock 을 read 로 잡고 PCI_COMMAND 의 Memory Space
 *      Enable 이 켜져 있는지 확인한 뒤에야 실제 ioread/iowrite 를 부른다. 꺼져
 *      있으면 -EIO 다. 이 여덟 개는 EXPORT_SYMBOL_GPL 로 내보내져 vendor 변종
 *      드라이버(mlx5, hisilicon, nvgrace-gpu 등)도 같은 잠금 규약을 쓴다.
 *  (2) **바이트 펌프** — vfio_pci_core_do_io_rw 가 요청 하나를 훑으면서, 가능한
 *      가장 넓은 자연 정렬 접근으로 쪼개 (1) 의 래퍼에 넘긴다. 동시에
 *      [x_start, x_end) 라는 **제외 구간**을 받아, 그 안에서는 쓰기를 버리고 읽기를
 *      0xFF 로 채운다.
 *  (3) **region 별 준비 작업** — vfio_pci_bar_rw 가 파일 오프셋에서 BAR 번호와
 *      BAR 안 위치를 뽑고, BAR 를 매핑하고(vfio_pci_core_setup_barmap), 제외 구간을
 *      MSI-X 테이블 또는 ROM 이미지 뒤쪽 여백으로 정한다. vfio_pci_vga_rw 는 BAR 가
 *      아닌 레거시 VGA 주소 세 구간을 같은 펌프로 흘려보낸다.
 *  (4) **ioeventfd** — 사용자가 "이 eventfd 가 울리면 이 BAR 오프셋에 이 상수를
 *      써라" 를 미리 등록해 두는 지름길. VMM 이 게스트의 특정 레지스터 쓰기를
 *      가로챘을 때 유저스페이스로 돌아오지 않고 커널 안에서 끝내기 위한 것이다.
 *
 * 반대로 이 파일이 **하지 않는** 일도 분명하다. 어떤 region 인지 고르는 일은
 * vfio_pci_core.c 의 vfio_pci_rw 가 하고(vfio_pci_core.c:4841), config 공간의
 * 바이트 단위 권한표는 vfio_pci_config.c 가 하고, 인터럽트 벡터 할당은
 * vfio_pci_intrs.c 가 한다. 이 파일은 **주소 산술과 제외 구간과 접근 폭**만 쥔다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 사용자 공간에서 시작해 하드웨어까지 내려가는 사슬은 이렇다.
 *
 *   1. 사용자가 device fd 에 read(2)/write(2) 를 건다. 파일 오프셋의 상위 비트가
 *      region 번호이고 하위 40비트가 region 안의 위치다
 *      (include/linux/vfio_pci_core.h 의 VFIO_PCI_OFFSET_SHIFT 는 40).
 *   2. drivers/vfio/vfio_main.c 의 파일 오퍼레이션이 vendor 의 read/write 콜백을
 *      부르고, vfio_pci.c 의 vfio_pci_ops 표가 그것을 vfio_pci_core_read /
 *      vfio_pci_core_write 로 이어 준다.
 *   3. vfio_pci_core.c 의 vfio_pci_rw(4841줄)가 region 번호로 갈린다.
 *        - config region     → vfio_pci_config.c 의 vfio_pci_config_rw
 *        - BAR0~5, ROM BAR   → [이 파일] vfio_pci_bar_rw
 *        - VGA region        → [이 파일] vfio_pci_vga_rw
 *        - vendor 추가 region → 그 vendor 의 regops
 *   4. [이 파일] vfio_pci_bar_rw 가 BAR 를 매핑하고 제외 구간을 정한 뒤
 *      vfio_pci_core_do_io_rw 를 부른다.
 *   5. vfio_pci_core_do_io_rw 가 vfio_pci_iordwr8/16/32/64 를 부르고, 그것이
 *      vfio_pci_core_ioread/iowrite 를 부르고, 그것이 마침내 ioread32 같은
 *      아키텍처 원시 접근을 부른다. MMIO 면 CPU 의 load/store 가, I/O 포트면
 *      x86 의 in/out 명령이 나간다.
 *
 * 실행 컨텍스트는 대부분 **호스트 커널의 프로세스 문맥**이다. 시스템 콜 안이라
 * 잠들 수 있고, copy_to_user / copy_from_user 로 사용자 페이지를 만지며,
 * ioremap 과 pci_iomap 이 페이지 테이블을 잡는다. 예외가 둘 있다.
 *  - vfio_pci_ioeventfd_handler 는 eventfd 의 대기열 깨우기에서 불린다. 스핀락을
 *    쥔 원자적 문맥일 수 있어 down_read_trylock 을 쓰고, 경합하면 1 을 돌려
 *    워커에게 넘긴다.
 *  - vfio_pci_ioeventfd_thread 는 그 워커(workqueue)에서 불린다. 여기서는 잠들
 *    수 있으므로 down_read 로 정직하게 기다린다.
 *
 * NVMe 를 공부하는 독자를 위한 위치 감각: SPDK 가 NVMe 컨트롤러를 붙잡고
 * SQ/CQ 도어벨을 두드리는 **빠른 경로는 이 파일을 지나지 않는다**. 그쪽은 BAR0 을
 * mmap 해 두고 CPU store 로 곧장 때린다. 이 파일이 쓰이는 것은 (a) 매핑하기 전에
 * CAP/CC/CSTS 같은 컨트롤러 레지스터를 한두 번 읽고 쓸 때, (b) VMM 이 게스트의
 * 레지스터 접근을 일부러 트랩해 중재하고 싶을 때, (c) 페이지보다 작거나 MSI-X
 * 테이블과 한 페이지에 섞여 mmap 이 허용되지 않는 BAR 를 만질 때다.
 *
 * === 타 모듈과의 연결 ===
 *  - include/linux/vfio_pci_core.h
 *      : 이 파일이 다루는 상태의 정의. struct vfio_pci_core_device(98~145줄)의
 *        barmap 배열(102줄), msix_bar/msix_size/msix_offset(114~116줄),
 *        ioeventfds_nr(132줄), ioeventfds_lock(137줄), ioeventfds_list(138줄),
 *        memory_lock(143줄)이 모두 여기 선언돼 있다. 접근 폭을 제한하는
 *        enum vfio_pci_io_width(147~152줄)도 여기다. 그리고 205~225줄의
 *        VFIO_IOWRITE_DECLARATION / VFIO_IOREAD_DECLARATION 매크로가 이 파일의
 *        VFIO_IOWRITE / VFIO_IOREAD 가 정의하는 함수들의 prototype 을 만든다 —
 *        정의와 선언이 서로 다른 매크로로 짝지어져 있다는 점을 유의해야 한다.
 *  - drivers/vfio/pci/vfio_pci_priv.h
 *      : 형제 파일 사이의 내부 ABI. VFIO_PCI_IOEVENTFD_MAX(12줄, 값 1000),
 *        struct vfio_pci_ioeventfd(14~24줄), 그리고 이 파일이 정의하는
 *        vfio_pci_bar_rw(44줄), vfio_pci_vga_rw(48줄, CONFIG 가 꺼지면 47~57줄의
 *        stub 이 -EINVAL 을 돌려준다), vfio_pci_ioeventfd(59줄)의 prototype 이 여기 있다.
 *  - drivers/vfio/pci/vfio_pci_core.c (주석 완료) — 이 파일의 유일한 상위.
 *        vfio_pci_rw(4841줄)가 vfio_pci_bar_rw 와 vfio_pci_vga_rw 를 부르고,
 *        vfio_pci_ioctl_ioeventfd(4511줄)가 인자를 검증한 뒤 vfio_pci_ioeventfd 를
 *        부르고, vfio_pci_core_mmap(5561줄)이 mmap 하기 전에
 *        vfio_pci_core_setup_barmap 을 부른다. 제외 구간의 근거가 되는 세 필드는
 *        vfio_pci_core_enable(1948줄)이 MSI-X capability 에서 뽑아 채운다 —
 *        MSI-X 가 없는 장치는 msix_bar 에 0xFF 를 넣어 어떤 BAR 번호와도 맞지 않게
 *        만든다(vfio_pci_core.c:2092). 정리는 vfio_pci_core_disable(2199줄)이
 *        하며, 거기서 barmap 을 pci_iounmap 하고 ioeventfds_list 를 비운다.
 *  - drivers/vfio/pci/vfio_pci_config.c
 *      : __vfio_pci_memory_enabled 가 여기 있다. 이 파일의 여덟 개 접근 래퍼가
 *        매번 그것을 불러 PCI_COMMAND 의 Memory Space Enable 을 확인한다.
 *        (그 파일은 현재 다른 작업자가 주석 중이라 줄 번호를 적지 않는다.)
 *  - drivers/vfio/virqfd.c (주석 완료)
 *      : ioeventfd 가 올라타는 2단계 콜백 기계. vfio_virqfd_enable(337줄)에
 *        handler 와 thread 를 함께 넘기면, eventfd 가 울릴 때 먼저 원자적 문맥에서
 *        handler 가 돌고, handler 가 0 이 아닌 값을 돌려주면 워커에서 thread 가
 *        돈다. 해제는 vfio_virqfd_disable(435줄).
 *  - drivers/pci (전부 주석 완료) — 이 파일이 직접 부르는 것들:
 *        pci.c:7343 pci_request_selected_regions — BAR 를 이 드라이버 이름으로 예약,
 *        pci.c:7258 pci_release_selected_regions — 그 예약 해제,
 *        iomap.c:308 pci_iomap — BAR 를 MMIO 든 I/O 포트든 같은 __iomem 토큰으로
 *          매핑(그래서 이 파일이 두 종류를 한 코드로 다룰 수 있다),
 *        rom.c:382 pci_map_rom — ROM 디코딩을 켜고 매핑하며 **실제 이미지 크기**를
 *          출력 인자로 돌려준다(이 파일은 그 값을 제외 구간의 시작으로 쓴다),
 *        rom.c:475 pci_unmap_rom — 그 짝,
 *        vgaarb.c:1542 vga_get — vga_get_interruptible 의 실체(세 번째 인자가 1),
 *        vgaarb.c:1792 vga_put — 그 짝.
 *  - 데이터 흐름: 사용자 버퍼 ↔ 이 파일의 스택 위 u8/u16/u32/u64 한 칸 ↔
 *        __iomem 포인터 ↔ 장치 레지스터. **중간 버퍼가 없다.** 한 번에 최대
 *        8바이트만 옮기고 바로 사용자 공간과 주고받으므로, 큰 요청은 루프가 된다.
 *
 * === 주요 함수/구조체 요약 ===
 *  - VFIO_IOWRITE(size) / VFIO_IOREAD(size)
 *      : 폭별 접근 래퍼 여덟 개를 찍어내는 매크로. 본체는 "필요하면 memory_lock 을
 *        read 로 잡고, 메모리 디코드가 켜져 있는지 보고, 아니면 -EIO" 뿐이다.
 *        EXPORT_SYMBOL_GPL 이 매크로 안에 들어 있어 인스턴스마다 내보내진다.
 *  - VFIO_IORDWR(size)
 *      : 위 래퍼와 사용자 버퍼를 잇는 static 접착제 네 개. 쓰기면
 *        copy_from_user 뒤 iowrite, 읽기면 ioread 뒤 copy_to_user 를 하고,
 *        옮긴 바이트 수를 filled 로 돌려준다.
 *  - vfio_pci_core_do_io_rw
 *      : **이 파일의 심장**. 제외 구간을 피해 가며 가장 넓은 자연 정렬 접근으로
 *        요청을 쪼갠다. 아래 전용 절에 산술을 통째로 적었다.
 *  - vfio_pci_core_setup_barmap
 *      : BAR 하나를 예약하고 매핑해 vdev->barmap 에 캐시한다. 이미 있으면 즉시
 *        성공. read/write 와 mmap 과 ioeventfd 세 경로가 모두 이 함수를 거친다.
 *  - vfio_pci_bar_rw
 *      : BAR/ROM region 의 진입점. 오프셋 분해, 길이 자르기, ROM 의 두 가지 출처
 *        처리, MSI-X 제외 구간 설정, 그리고 사후 정리.
 *  - vfio_pci_vga_rw
 *      : 레거시 VGA 주소 세 구간(0xA0000~0xBFFFF 메모리, 0x3B0~0x3BB 포트,
 *        0x3C0~0x3DF 포트)의 진입점. VGA 중재자에게 소유권을 빌린 뒤 같은 펌프를
 *        쓴다. 제외 구간은 비어 있고 최대 폭은 4바이트다.
 *  - vfio_pci_ioeventfd_do_write / _handler / _thread
 *      : 등록된 상수 쓰기를 실제로 수행하는 세 함수. handler 는 원자적 문맥용,
 *        thread 는 잠들 수 있는 문맥용이다.
 *  - vfio_pci_ioeventfd
 *      : 등록/해제 진입점. 중복 검사, 개수 상한, MSI-X 우회 차단.
 *  - struct vfio_pci_ioeventfd (정의는 vfio_pci_priv.h:14~24)
 *      : 이 파일이 채우고 읽는 유일한 구조체. 이 파일 안에 정의된 구조체나 enum 은
 *        하나도 없다 — 전부 두 헤더에서 온다.
 *
 * === 제외 구간(excluded range)의 산술 — 이 파일의 핵심 ===
 * vfio_pci_core_do_io_rw 는 [x_start, x_end) 라는 반열린 구간을 받는다. 그 구간은
 * "이 region 안에 있지만 사용자에게 진짜 내용을 보여 주지 않는 곳" 이다. 규칙은
 * 딱 두 줄이다 — **쓰기는 조용히 버리고, 읽기는 0xFF 로 채운다.** 원본 영어
 * 주석이 do_io_rw 바로 위에서 그렇게 선언하고 있다.
 *
 * 구간이 정해지는 자리는 세 곳뿐이다.
 *  (a) **MSI-X 테이블** — vfio_pci_bar_rw 에서 `bar == vdev->msix_bar` 일 때
 *      x_start = vdev->msix_offset, x_end = msix_offset + msix_size. 세 값은
 *      vfio_pci_core_enable(vfio_pci_core.c:1948) 이 MSI-X capability 의
 *      Table Offset/BIR 레지스터에서 뽑아 둔 것이다. BIR 하위 3비트가 BAR 번호,
 *      나머지가 오프셋, Table Size 필드 +1 에 16을 곱한 것이 크기다.
 *  (b) **ROM 이미지 뒤 여백** — ROM BAR 는 창(window)이 실제 이미지보다 훨씬 클 수
 *      있다. pci_map_rom(drivers/pci/rom.c:382)이 출력 인자로 돌려주는 진짜 크기를
 *      x_start 로, 창 크기를 x_end 로 잡는다. 원본 영어 주석이 "큰 ROM BAR 를
 *      채우는 것이 훨씬 빨라진다" 고 적은 이유는, 여백을 진짜 MMIO 읽기로 훑으면
 *      바이트마다 PCIe 왕복이 생기지만 제외 구간으로 만들면 그냥 메모리 채우기가
 *      되기 때문이다.
 *  (c) **없음** — 그 밖의 모든 경우 x_start = x_end = 0 이다. off 는 음수가 될 수
 *      없으므로 `off < x_start` 는 항상 거짓, `off >= x_end` 는 항상 참이 되어
 *      제외가 사라진다. 즉 "구간 없음" 을 따로 표현하는 플래그가 필요 없다.
 *
 * **경계를 걸치는(straddling) 접근은 절대 일어나지 않는다.** 이유는 fillable 의
 * 정의에 있다. off 가 구간 앞에 있으면 fillable 은 `x_start - off` 이하로 잘리고,
 * 실제 접근 폭은 언제나 fillable 이하로 고른다. 그래서 하나의 ioread/iowrite 가
 * x_start 를 넘어설 수 없다. 사용자가 경계를 가로지르는 read(2) 를 한 번 걸면
 * 커널은 그것을 세 토막으로 나눈다 —
 *   (i) off 부터 x_start 직전까지: 진짜 MMIO 접근. 다만 남은 바이트 수가 줄어들면서
 *       폭이 8 → 4 → 2 → 1 로 **자동으로 좁아진다**.
 *   (ii) x_start 부터 min(x_end, 요청 끝)까지: 쓰기는 버려지고 읽기는 0xFF.
 *   (iii) x_end 부터: 다시 진짜 접근.
 * 세 토막의 바이트 수는 모두 done 에 더해지므로 **사용자에게는 요청한 길이가 전부
 * 처리된 것처럼 보인다.** 오류 코드가 아니라 0xFF 라는 값으로 통보된다는 점이
 * 중요하다. 실제 PCI 버스에서도 응답 없는 읽기는 마스터 어보트로 all-ones 를
 * 돌려주므로, 0xFF 채우기는 "그 자리에 아무것도 없다" 는 관례적인 표현이다.
 *
 * === 접근 폭(width)이 결정되는 규칙 ===
 * 사용자가 요청한 길이는 접근 폭이 **아니다**. do_io_rw 는 반복마다 세 조건을 모두
 * 만족하는 가장 넓은 폭을 고른다.
 *   - fillable 이 그 폭 이상일 것 (제외 구간이나 요청 끝을 넘지 않도록)
 *   - off 가 그 폭으로 나누어떨어질 것 (`!(off % 8)` 같은 자연 정렬 조건)
 *   - max_width 가 그 폭 이상일 것 (호출자가 건 상한)
 * 정렬을 region 오프셋으로 판단해도 되는 이유는, 실제 주소가 `io + off` 이고
 * io 는 pci_iomap 이나 ioremap 이 돌려준 최소 페이지 정렬 주소이기 때문이다.
 * 그래서 off 의 하위 3비트가 곧 물리 주소의 하위 3비트다.
 *
 * 실용적인 귀결이 셋 있다.
 *  - 오프셋 4 에서 8바이트를 읽으면 8로 나누어떨어지지 않으므로 **4바이트 두 번**이
 *    된다. 즉 read(2) 는 요청 길이만큼의 단일 버스 트랜잭션을 보장하지 않는다.
 *    64비트 레지스터를 반드시 한 번에 읽어야 한다면 오프셋이 8정렬이어야 한다.
 *  - 정렬되지 않은 시작 오프셋은 1바이트씩 시작해 점점 넓어진다. 예를 들어
 *    오프셋 3 에서 16바이트를 읽으면 1, 4, 8, 2, 1 순으로 갈린다.
 *  - max_width 는 두 곳에서만 낮춰 잡는다. ROM BAR 는
 *    VFIO_PCI_IO_WIDTH_4(원본 주석이 밝히듯 Intel X710 같은 장치가 ROM 을 qword 로
 *    읽으면 PCI AER 오류가 뜬다), 레거시 VGA 도 VFIO_PCI_IO_WIDTH_4 다. 그 밖의
 *    BAR 는 VFIO_PCI_IO_WIDTH_8 로 시작한다.
 *
 * === ROM BAR 의 두 가지 출처 ===
 * ROM region 은 다른 BAR 와 달리 두 갈래다.
 *  - **하드웨어 ROM BAR 가 주소를 배정받은 경우** (pci_resource_start 가 0 이 아님):
 *    pci_map_rom 이 ROM 디코딩을 켜고 창을 매핑하며 실제 이미지 크기를 알려 준다.
 *    끝나면 pci_unmap_rom 으로 디코딩을 원래대로 되돌린다. 이 매핑은 barmap 에
 *    캐시하지 않고 **요청마다 만들고 부순다** — ROM 디코딩을 계속 켜 두면 그 창이
 *    다른 BAR 와 주소를 다투기 때문이다.
 *  - **펌웨어가 시스템 메모리에 떠 둔 사본** (pdev->rom 과 pdev->romlen):
 *    일부 장치는 초기화 뒤에 ROM 을 읽을 수 없게 되므로 펌웨어가 미리 복사해
 *    둔다(drivers/pci/rom.c:30~34 의 설명 참조). 이때는 그냥 ioremap 으로 그
 *    물리 주소를 매핑하고, region 크기는 romlen 을 2의 거듭제곱으로 올림한
 *    값으로 알린다. 실제 내용은 romlen 까지뿐이므로 x_start 를 romlen 으로 두어
 *    나머지를 제외 구간으로 만든다. 끝나면 iounmap 이다.
 *  - 둘 다 아니면 -EINVAL 이다. ROM 이 없는 장치의 ROM region 을 읽으려 한 것이다.
 *
 * === ioeventfd 의 2단계 콜백 ===
 * ioeventfd 는 "eventfd 가 울리면 미리 정해 둔 상수를 미리 정해 둔 BAR 오프셋에
 * 쓴다" 는 커널 안의 지름길이다. KVM 이 게스트의 MMIO 쓰기를 트랩해 eventfd 를
 * 울리면, 유저스페이스 VMM 으로 나가지 않고 커널 안에서 장치 레지스터 쓰기가
 * 끝난다. 등록은 VFIO_DEVICE_IOEVENTFD ioctl 이고, 그것을 검증하는 쪽이
 * vfio_pci_core.c 의 vfio_pci_ioctl_ioeventfd(4511줄)다.
 *
 * 두 단계로 나뉜 이유는 잠금 때문이다. eventfd 깨우기는 원자적 문맥일 수 있어
 * memory_lock 을 기다릴 수 없다. 그래서 handler 가 down_read_trylock 을 먼저
 * 시도하고, 성공하면 그 자리에서 쓰기를 끝내고, 실패하면 1 을 돌려 virqfd 가
 * 워커에서 thread 를 부르게 한다. thread 는 프로세스 문맥이므로 down_read 로
 * 정직하게 기다린다. 즉 **같은 쓰기가 두 함수 중 하나에서만 수행되며, 어느 쪽이든
 * memory_lock 아래에서 수행된다.**
 *
 * 보안상 중요한 제약이 하나 있다. MSI-X 테이블이 든 BAR 에서 테이블 구간과
 * 조금이라도 겹치는 ioeventfd 는 등록할 수 없다. 그렇지 않으면 read/write 로는
 * 막아 둔 MSI-X 테이블 쓰기를 ioeventfd 로 우회할 수 있게 된다. 원본 영어 주석이
 * "Disallow ioeventfds working around MSI-X table writes" 라고 그 의도를 밝힌다. */

/* [한국어] 파일 계층의 기본 타입과 관례를 가져온다. 이 파일의 진입점 세 개
 * (vfio_pci_bar_rw, vfio_pci_vga_rw, vfio_pci_ioeventfd)가 모두 read(2)/write(2)
 * 규약의 loff_t 오프셋을 다루기 때문이다. 이 트리는 sparse checkout 이라
 * include/linux/fs.h 가 없어 정확히 어떤 선언이 여기서 오는지는 확인 못 함. */
#include <linux/fs.h>
/* [한국어] PCI 계층. struct pci_dev, pci_resource_start/pci_resource_len,
 * PCI_ROM_RESOURCE 같은 자원 번호, 그리고 이 파일이 부르는
 * pci_request_selected_regions(drivers/pci/pci.c:7343),
 * pci_release_selected_regions(pci.c:7258), pci_iomap(iomap.c:308),
 * pci_map_rom(rom.c:382), pci_unmap_rom(rom.c:475)의 prototype 이 여기 있다.
 * include/linux/pci.h 는 이 트리에 없지만 정의부인 drivers/pci 는 전부 있다. */
#include <linux/pci.h>
/* [한국어] copy_to_user 와 copy_from_user. 이 파일은 커널 버퍼를 따로 잡지 않고
 * 스택 위의 u8/u16/u32/u64 한 칸을 사용자 공간과 직접 주고받으므로, 이 둘이
 * 사용자 포인터 검증과 페이지 폴트 처리를 대신해 준다. */
#include <linux/uaccess.h>
/* [한국어] ioread8/16/32/64 와 iowrite 계열, ioremap/iounmap, ioport_map/ioport_unmap.
 * 이 파일의 존재 이유인 실제 장치 접근 원시 연산이 전부 여기서 온다. */
#include <linux/io.h>
/* [한국어] VFIO 공용 정의. 이 파일이 직접 쓰는 것은 ioeventfd 가 올라타는
 * virqfd 기계다 — include/linux/vfio.h:412 의 struct virqfd,
 * 426줄의 vfio_virqfd_enable, 429줄의 vfio_virqfd_disable. */
#include <linux/vfio.h>
/* [한국어] VGA 중재자(arbiter). vfio_pci_vga_rw 가 쓰는 VGA_RSRC_LEGACY_MEM /
 * VGA_RSRC_LEGACY_IO 상수와 vga_get_interruptible / vga_put 이 여기서 온다.
 * vga_get_interruptible 은 vgaarb.h 안의 인라인 껍데기이고 실체는
 * drivers/pci/vgaarb.c:1542 의 vga_get 에 세 번째 인자를 1 로 넘기는 것이다
 * (그 사실은 vgaarb.c:1510 의 주석이 밝히고 있다). vgaarb.h 자체는 이 트리에 없다. */
#include <linux/vgaarb.h>
/* [한국어] 64비트 MMIO 접근을 32비트 두 번(하위 먼저, 상위 나중)으로 흉내 내는
 * 대체 정의. 32비트 아키텍처처럼 ioread64/iowrite64 를 원자적으로 제공하지 못하는
 * 플랫폼에서도 아래 VFIO_IOWRITE(64) / VFIO_IOREAD(64) 인스턴스가 컴파일되게 한다.
 * lo-hi 순서를 고른 것은 대부분의 장치 레지스터가 하위 워드를 쓴 뒤 상위 워드를
 * 써야 동작을 시작하기 때문이다. 이 헤더는 이 트리에 없어 내용은 확인 못 함. */
#include <linux/io-64-nonatomic-lo-hi.h>

/* [한국어] 형제 파일 사이의 내부 헤더. 이 파일이 정의하는 vfio_pci_bar_rw(44줄),
 * vfio_pci_vga_rw(48줄), vfio_pci_ioeventfd(59줄)의 prototype 과, 이 파일이 쓰는
 * struct vfio_pci_ioeventfd(14~24줄), VFIO_PCI_IOEVENTFD_MAX(12줄, 1000),
 * __vfio_pci_memory_enabled 선언이 여기 있다. 이 헤더가 다시
 * include/linux/vfio_pci_core.h 를 포함하므로 struct vfio_pci_core_device 와
 * enum vfio_pci_io_width 도 함께 딸려 온다. */
#include "vfio_pci_priv.h"

/* [한국어] 엔디안 분기. ioread32 계열은 "레지스터를 리틀 엔디안으로 읽어 CPU
 * 엔디안으로 바꾼다" 는 의미이므로, 빅 엔디안 CPU 에서 그대로 쓰면 사용자가 준
 * 바이트열이 뒤집힌다. VFIO 는 사용자에게 **장치가 본 바이트 순서 그대로**를
 * 보여 줘야 하므로, 빅 엔디안에서는 다시 뒤집는 be 변종을 골라 두 번의 뒤집기가
 * 서로 상쇄되게 한다. 8비트에는 순서가 없으므로 이 분기 밖에 있다. */
#ifdef __LITTLE_ENDIAN
/* [한국어] 리틀 엔디안: 변환 없는 그대로의 64비트 읽기. */
#define vfio_ioread64	ioread64
/* [한국어] 리틀 엔디안: 변환 없는 그대로의 64비트 쓰기. */
#define vfio_iowrite64	iowrite64
/* [한국어] 리틀 엔디안: 변환 없는 그대로의 32비트 읽기. */
#define vfio_ioread32	ioread32
/* [한국어] 리틀 엔디안: 변환 없는 그대로의 32비트 쓰기. */
#define vfio_iowrite32	iowrite32
/* [한국어] 리틀 엔디안: 변환 없는 그대로의 16비트 읽기. */
#define vfio_ioread16	ioread16
/* [한국어] 리틀 엔디안: 변환 없는 그대로의 16비트 쓰기. */
#define vfio_iowrite16	iowrite16
/* [한국어] 빅 엔디안 CPU. 아래 여섯 개는 be 변종이라 한 번 더 바이트를 뒤집어,
 * ioread 가 이미 한 뒤집기를 되돌린다. 결과적으로 장치 메모리의 바이트열이
 * 사용자 버퍼에 그대로 복사된다. */
#else
/* [한국어] 빅 엔디안: 뒤집기를 상쇄하는 64비트 읽기. */
#define vfio_ioread64	ioread64be
/* [한국어] 빅 엔디안: 뒤집기를 상쇄하는 64비트 쓰기. */
#define vfio_iowrite64	iowrite64be
/* [한국어] 빅 엔디안: 뒤집기를 상쇄하는 32비트 읽기. */
#define vfio_ioread32	ioread32be
/* [한국어] 빅 엔디안: 뒤집기를 상쇄하는 32비트 쓰기. */
#define vfio_iowrite32	iowrite32be
/* [한국어] 빅 엔디안: 뒤집기를 상쇄하는 16비트 읽기. */
#define vfio_ioread16	ioread16be
/* [한국어] 빅 엔디안: 뒤집기를 상쇄하는 16비트 쓰기. */
#define vfio_iowrite16	iowrite16be
/* [한국어] 엔디안 분기 끝. */
#endif
/* [한국어] 8비트에는 바이트 순서가 없으므로 두 엔디안이 같은 정의를 쓴다.
 * 그래서 이 두 줄만 #ifdef 밖에 있다. */
#define vfio_ioread8	ioread8
/* [한국어] 8비트 쓰기. 위와 같은 이유로 분기 밖. */
#define vfio_iowrite8	iowrite8

/* [한국어] VFIO_IOWRITE(size) - 폭별 장치 쓰기 래퍼 네 개를 찍어내는 매크로
 *
 * @size: 8, 16, 32, 64 중 하나. 함수 이름과 값 타입(u8/u16/u32/u64)에 동시에 심긴다.
 *
 * 펼쳐진 함수 vfio_pci_core_iowrite##size 의 규약:
 *   @vdev: 대상 vfio-pci 디바이스. memory_lock 과 vconfig 그림자를 여기서 얻는다.
 *   @test_mem: 참이면 memory_lock 을 read 로 잡고 PCI_COMMAND 의 Memory Space
 *     Enable 을 확인한다. MMIO BAR 접근이면 참, I/O 포트 BAR 나 레거시 VGA 면 거짓.
 *   @val: 장치에 쓸 값. 이미 사용자 공간에서 복사된 커널 쪽 값이다.
 *   @io: 최종 __iomem 주소. 호출자가 오프셋을 이미 더해 두었다.
 *   @return: 0 = 성공, -EIO = 메모리 디코드가 꺼져 있어 접근을 거부.
 *
 * 왜 필요한가: BAR 접근은 두 가지 경쟁과 싸워야 한다. 하나는 사용자가
 * PCI_COMMAND 의 Memory Space Enable 을 끄는 것이고, 다른 하나는 리셋 경로가
 * BAR 매핑을 회수하는 것이다. 둘 다 vfio_pci_core.c 가 memory_lock 을 write 로
 * 잡고 수행하므로, 이 함수가 read 로 잡고 있는 동안에는 일어날 수 없다. 그
 * 잠금과 검사를 폭마다 손으로 네 번 쓰는 대신 매크로 하나로 찍어낸다.
 *
 * 동작 단계: (1) test_mem 이면 down_read 로 memory_lock 확보. (2) 메모리 디코드
 * 확인, 꺼져 있으면 잠금을 놓고 -EIO. (3) 엔디안 분기에서 고른 iowrite 로 실제
 * 쓰기. (4) 잡았으면 up_read. (5) 0 반환.
 *
 * 실행 컨텍스트: 프로세스 문맥(read/write 시스템 콜 안) 또는 워크큐
 * (vfio_pci_ioeventfd_thread). down_read 가 잠들 수 있으므로 원자적 문맥에서
 * 부르면 안 된다 - 원자적 문맥용 경로는 vfio_pci_ioeventfd_handler 가 따로
 * down_read_trylock 을 쓴다.
 *
 * 호출자: 같은 파일의 vfio_pci_iordwr##size 와 vfio_pci_ioeventfd_do_write,
 * 그리고 EXPORT_SYMBOL_GPL 로 공개돼 있어 vendor 변종 드라이버도 부른다.
 * 호출 대상: __vfio_pci_memory_enabled(vfio_pci_config.c), down_read/up_read,
 * 그리고 아키텍처의 iowrite8/16/32/64 또는 그 be 변종.
 *
 * 에러 경로: 유일한 실패는 -EIO 뿐이고, 그 경우 잠금은 확실히 풀린다.
 *
 * 호출 체인:
 *   vfio_pci_core_do_io_rw -> vfio_pci_iordwr32 -> [vfio_pci_core_iowrite32]
 *     -> __vfio_pci_memory_enabled -> iowrite32 */
#define VFIO_IOWRITE(size)	/* [한국어] size 를 토큰 붙이기로 이름에 심어 폭별 함수를 찍어내는 틀. 아래 네 번 인스턴스화된다 */ \
int vfio_pci_core_iowrite##size(struct vfio_pci_core_device *vdev,	/* [한국어] 함수 이름 생성: ## 는 전처리기 토큰 붙이기라 VFIO_IOWRITE(32) 이면 vfio_pci_core_iowrite32 가 된다. 반환형 int 는 0(성공) 또는 -EIO(메모리 디코드 꺼짐) */ \
			bool test_mem, u##size val, void __iomem *io)	/* [한국어] test_mem 은 이 접근이 memory_lock 검사를 받아야 하는지(MMIO BAR 이면 참, I/O 포트나 레거시 VGA 면 거짓). u##size 는 u8/u16/u32/u64 로 펼쳐진다. io 는 이미 오프셋이 더해진 최종 __iomem 주소 */ \
{	/* [한국어] 함수 본문 시작 */ \
	if (test_mem) {	/* [한국어] MMIO 접근일 때만 잠금과 검사를 한다. I/O 포트는 PCI_COMMAND 의 Memory Space Enable 과 무관하므로 건너뛴다 */ \
		down_read(&vdev->memory_lock);	/* [한국어] memory_lock 을 read 로 잡는다. write 로 잡는 쪽은 리셋과 메모리 디코드 해제 경로(vfio_pci_core.c 의 vfio_pci_zap_and_down_write_memory_lock)이므로, 이 read 잠금이 그 사이에 접근이 끼어드는 것을 막는다 */ \
		if (!__vfio_pci_memory_enabled(vdev)) {	/* [한국어] PCI_COMMAND 의 Memory Space Enable 비트를 vconfig 그림자에서 확인한다. 정의는 형제 파일 vfio_pci_config.c 의 __vfio_pci_memory_enabled */ \
			up_read(&vdev->memory_lock);	/* [한국어] 실패 반환 전에 반드시 잠금을 놓는다. 이 한 줄이 빠지면 이후 모든 리셋이 영원히 막힌다 */ \
			return -EIO;	/* [한국어] 디코딩이 꺼진 BAR 를 건드리면 호스트가 마스터 어보트를 맞을 수 있으므로 접근 자체를 거부한다. 이 -EIO 가 사용자에게 그대로 올라간다 */ \
		}	/* [한국어] 메모리 디코드 검사 블록 끝 */ \
	}	/* [한국어] test_mem 블록 끝. 여기 도달했으면 잠금을 쥔 상태이거나 애초에 잠글 필요가 없는 경우다 */ \
	/* [한국어] 빈 줄이지만 매크로 안이라 백슬래시가 필요하다 */ \
	vfio_iowrite##size(val, io);	/* [한국어] 실제 장치 쓰기. vfio_iowrite32 등으로 펼쳐지며, 엔디안 분기에서 고른 정의가 여기 들어온다. 반환값이 없으므로 하드웨어가 받았는지는 알 수 없다 - PCI 쓰기는 posted 라 완료를 기다리지 않는다 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
	if (test_mem)	/* [한국어] 잠갔던 경우에만 놓는다 */ \
		up_read(&vdev->memory_lock);	/* [한국어] read 잠금 해제. 위 down_read 와 정확히 짝을 이룬다 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
	return 0;	/* [한국어] 여기까지 왔으면 성공. 실제로 값이 장치에 도달했는지는 검증하지 않는다 */ \
}	/* [한국어] 함수 본문 끝 */ \
EXPORT_SYMBOL_GPL(vfio_pci_core_iowrite##size);	/* [한국어] 인스턴스마다 심벌을 내보낸다. mlx5, hisilicon, nvgrace-gpu 같은 vendor 변종 드라이버가 자기 region 접근에서 같은 잠금 규약을 쓰게 하기 위해서다. 매크로 안에 EXPORT 가 들어 있으므로 네 번 펼쳐져 네 개의 심벌이 나간다 */

VFIO_IOWRITE(8)	/* [한국어] u8 판 생성: vfio_pci_core_iowrite8 */
VFIO_IOWRITE(16)	/* [한국어] u16 판 생성: vfio_pci_core_iowrite16 */
VFIO_IOWRITE(32)	/* [한국어] u32 판 생성: vfio_pci_core_iowrite32 */
VFIO_IOWRITE(64)	/* [한국어] u64 판 생성: vfio_pci_core_iowrite64. include/linux/vfio_pci_core.h:213 의 선언은 iowrite64 가 매크로로 정의돼 있을 때만 나오지만, 정의인 이 줄은 조건 없이 펼쳐진다. io-64-nonatomic-lo-hi.h 가 어떤 플랫폼에서든 iowrite64 를 제공하기 때문이다 */

/* [한국어] VFIO_IOREAD(size) - 폭별 장치 읽기 래퍼 네 개를 찍어내는 매크로
 *
 * @size: 8, 16, 32, 64 중 하나.
 *
 * 펼쳐진 함수 vfio_pci_core_ioread##size 의 규약:
 *   @vdev: 대상 vfio-pci 디바이스.
 *   @test_mem: 위 쓰기 판과 같은 의미.
 *   @val: 출력 인자. 읽은 값이 여기 담긴다. 실패하면 손대지 않는다.
 *   @io: 최종 __iomem 주소.
 *   @return: 0 = 성공, -EIO = 메모리 디코드 꺼짐.
 *
 * 왜 필요한가: 쓰기 판과 완전히 같은 이유다. 읽기도 디코딩이 꺼진 BAR 를
 * 건드리면 호스트가 마스터 어보트를 맞을 수 있어 같은 보호가 필요하다.
 * 쓰기와 읽기를 따로 만든 것은 값이 인자로 들어오느냐 포인터로 나가느냐만
 * 다르기 때문이며, 그 한 줄 차이 때문에 매크로를 두 벌 둔다.
 *
 * 동작 단계: (1) test_mem 이면 down_read. (2) 메모리 디코드 확인, 꺼져 있으면
 * 잠금 해제 후 -EIO. (3) ioread 로 실제 읽기, 값을 val 에 저장. (4) up_read.
 * (5) 0 반환.
 *
 * 주의: 이 함수는 제외 구간을 모른다. MSI-X 테이블을 0xFF 로 채우는 일은
 * 윗 계층인 vfio_pci_core_do_io_rw 의 몫이고, 이 함수까지 내려온 주소는 이미
 * "진짜로 읽어도 되는 곳" 으로 판정이 끝난 주소다.
 *
 * 실행 컨텍스트: 프로세스 문맥. 잠들 수 있다.
 * 호출자: vfio_pci_iordwr##size, 그리고 vendor 변종 드라이버.
 * 호출 대상: __vfio_pci_memory_enabled(vfio_pci_config.c), down_read/up_read,
 * 아키텍처의 ioread8/16/32/64 또는 be 변종.
 *
 * 에러 경로: -EIO 하나뿐이며 잠금은 확실히 풀린다.
 *
 * 호출 체인:
 *   vfio_pci_core_do_io_rw -> vfio_pci_iordwr32 -> [vfio_pci_core_ioread32]
 *     -> __vfio_pci_memory_enabled -> ioread32 */
#define VFIO_IOREAD(size)	/* [한국어] 읽기 판을 찍어내는 틀. 쓰기 판과 구조가 대칭이고 값 전달 방향만 반대다 */ \
int vfio_pci_core_ioread##size(struct vfio_pci_core_device *vdev,	/* [한국어] vfio_pci_core_ioread8/16/32/64 생성 */ \
			bool test_mem, u##size *val, void __iomem *io)	/* [한국어] 쓰기 판과 다른 유일한 자리: 값이 인자가 아니라 출력 포인터 val 로 나간다 */ \
{	/* [한국어] 함수 본문 시작 */ \
	if (test_mem) {	/* [한국어] MMIO 일 때만 잠금과 검사 */ \
		down_read(&vdev->memory_lock);	/* [한국어] memory_lock 을 read 로 잡는다 */ \
		if (!__vfio_pci_memory_enabled(vdev)) {	/* [한국어] 메모리 디코드가 켜져 있는지 확인 */ \
			up_read(&vdev->memory_lock);	/* [한국어] 실패 반환 전 잠금 해제 */ \
			return -EIO;	/* [한국어] 꺼져 있으면 읽지 않고 -EIO. 값을 0xFF 로 채우지 않는다는 점에 주의 - 그 채우기는 위쪽 vfio_pci_core_do_io_rw 의 제외 구간 처리이지 이 계층의 일이 아니다 */ \
		}	/* [한국어] 검사 블록 끝 */ \
	}	/* [한국어] test_mem 블록 끝 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
	*val = vfio_ioread##size(io);	/* [한국어] 실제 장치 읽기. 반환값을 출력 인자에 담는다. 응답 없는 주소를 읽으면 하드웨어가 all-ones 를 돌려주므로 오류 없이 0xFF.. 가 담길 수 있다 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
	if (test_mem)	/* [한국어] 잠갔던 경우에만 놓는다 */ \
		up_read(&vdev->memory_lock);	/* [한국어] read 잠금 해제 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
	return 0;	/* [한국어] 성공 */ \
}	/* [한국어] 함수 본문 끝 */ \
EXPORT_SYMBOL_GPL(vfio_pci_core_ioread##size);	/* [한국어] 인스턴스마다 심벌 공개. 쓰기 판과 같은 이유다 */

VFIO_IOREAD(8)	/* [한국어] u8 판 생성: vfio_pci_core_ioread8 */
VFIO_IOREAD(16)	/* [한국어] u16 판 생성: vfio_pci_core_ioread16 */
VFIO_IOREAD(32)	/* [한국어] u32 판 생성: vfio_pci_core_ioread32 */
VFIO_IOREAD(64)	/* [한국어] u64 판 생성: vfio_pci_core_ioread64 */

/* [한국어] VFIO_IORDWR(size) - 사용자 버퍼와 장치 사이를 한 번에 폭만큼 옮기는
 * 접착제 네 개를 찍어내는 매크로
 *
 * @size: 8, 16, 32, 64 중 하나.
 *
 * 펼쳐진 함수 vfio_pci_iordwr##size 의 규약:
 *   @vdev: 대상 디바이스. 아래 래퍼에 그대로 전달된다.
 *   @iswrite: 참이면 사용자 -> 장치, 거짓이면 장치 -> 사용자.
 *   @test_mem: memory_lock 검사 여부. 그대로 아래 래퍼로 전달.
 *   @io: region 의 시작 __iomem 주소(오프셋 더하기 전).
 *   @buf: 사용자 버퍼의 현재 위치. 호출자가 진행에 맞춰 전진시켜 온 값.
 *   @off: region 시작으로부터의 바이트 오프셋. 호출자가 접근 폭으로 정렬해 둔다.
 *   @filled: 출력 인자. 성공하면 sizeof(값) 이 담긴다. 실패하면 손대지 않는다.
 *   @return: 0 = 성공, -EFAULT = 사용자 포인터 오류, -EIO = 메모리 디코드 꺼짐.
 *
 * 왜 필요한가: 위 두 매크로는 커널 값 한 칸과 장치 사이만 다룬다. 그 앞뒤로
 * copy_from_user / copy_to_user 를 붙이고 옮긴 바이트 수를 보고하는 일을 폭마다
 * 네 벌 쓰지 않으려고 이 세 번째 매크로가 있다. 결과적으로 상위의
 * vfio_pci_core_do_io_rw 는 폭만 고르면 되고 방향과 복사는 신경 쓰지 않는다.
 *
 * 동작 단계 - 쓰기: copy_from_user 로 값을 받고, iowrite 로 장치에 쓴다.
 * 동작 단계 - 읽기: ioread 로 값을 받고, copy_to_user 로 사용자에게 준다.
 * 두 경우 모두 마지막에 filled 에 폭을 기록하고 0 을 돌려준다.
 *
 * 중요한 순서 차이: 쓰기는 사용자 복사가 **먼저**라 실패해도 장치를 건드리지
 * 않지만, 읽기는 장치 접근이 **먼저**라 copy_to_user 가 실패하면 이미 일어난
 * 장치 읽기를 되돌릴 수 없다. 읽기에 부작용이 있는 레지스터라면 그 부작용은
 * 남는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. copy_to_user / copy_from_user 가 페이지 폴트를
 * 낼 수 있으므로 반드시 잠들 수 있는 문맥이어야 한다. 그래서 ioeventfd 의
 * 원자적 경로는 이 함수를 쓰지 않고 vfio_pci_ioeventfd_do_write 가 래퍼를 직접
 * 부른다.
 * 호출자: vfio_pci_core_do_io_rw 하나뿐이다(static 이므로 파일 밖에서 부를 수 없다).
 * 호출 대상: copy_from_user, copy_to_user, vfio_pci_core_iowrite##size,
 * vfio_pci_core_ioread##size.
 *
 * 에러 경로: 세 지점(copy_from_user, 래퍼, copy_to_user) 어디서든 음수를 그대로
 * 위로 올리며, filled 를 갱신하지 않아 호출자가 진행하지 않게 만든다.
 *
 * 호출 체인:
 *   vfio_pci_bar_rw -> vfio_pci_core_do_io_rw -> [vfio_pci_iordwr32]
 *     -> copy_from_user / copy_to_user
 *     -> vfio_pci_core_iowrite32 / vfio_pci_core_ioread32 */
#define VFIO_IORDWR(size)	/* [한국어] 위 두 매크로가 만든 접근 래퍼와 사용자 버퍼를 잇는 접착제를 찍어내는 틀. 이쪽은 EXPORT 하지 않고 static 이다 - 이 파일 안에서만 쓰인다 */ \
static int vfio_pci_iordwr##size(struct vfio_pci_core_device *vdev,	/* [한국어] vfio_pci_iordwr8/16/32/64 생성. static 이므로 심벌이 밖으로 나가지 않는다 */ \
				bool iswrite, bool test_mem,	/* [한국어] iswrite 가 방향을 정한다(참이면 사용자 -> 장치). test_mem 은 그대로 아래 래퍼에 전달되어 memory_lock 검사 여부를 결정한다 */ \
				void __iomem *io, char __user *buf,	/* [한국어] io 는 region 의 시작 __iomem 주소이고 buf 는 사용자 버퍼의 현재 위치다. 두 포인터 모두 호출자가 진행에 맞춰 갱신해 준다 */ \
				loff_t off, size_t *filled)	/* [한국어] off 는 region 시작으로부터의 바이트 오프셋. filled 는 출력 인자로, 이번 호출이 옮긴 바이트 수를 호출자에게 알린다 - 호출자는 그 값으로 off/buf/count 를 전진시킨다 */ \
{	/* [한국어] 함수 본문 시작 */ \
	u##size val;	/* [한국어] 장치와 주고받을 값 한 칸. u8/u16/u32/u64 로 펼쳐지며, 이것이 이 파일에서 데이터가 머무는 유일한 중간 저장소다 */ \
	int ret;	/* [한국어] 아래 래퍼의 반환값(0 또는 -EIO)을 받을 자리 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
	if (iswrite) {	/* [한국어] 쓰기 방향. 사용자 버퍼에서 먼저 값을 가져와야 한다 */ \
		if (copy_from_user(&val, buf, sizeof(val)))	/* [한국어] 사용자 버퍼에서 sizeof(val) 바이트를 커널 스택으로 복사한다. 길이가 접근 폭과 정확히 같으므로 호출자가 폭만큼의 바이트가 남아 있음을 이미 보장한 상태여야 한다 */ \
			return -EFAULT;	/* [한국어] 사용자 포인터가 잘못됐거나 매핑되지 않았다. 장치는 아직 건드리지 않았으므로 부작용 없이 빠진다 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
		ret = vfio_pci_core_iowrite##size(vdev, test_mem,	/* [한국어] 이제야 장치에 쓴다. 여기서 memory_lock 을 잡고 메모리 디코드를 확인한다 */ \
						  val, io + off);	/* [한국어] 주소 계산: io + off. off 가 이미 접근 폭으로 정렬돼 있음을 호출자가 보장한다 */ \
		if (ret)	/* [한국어] 메모리 디코드가 꺼져 있었으면 -EIO */ \
			return ret;	/* [한국어] 오류를 그대로 위로 올린다. filled 는 건드리지 않았으므로 호출자는 이번 회차에 아무것도 옮기지 않은 것으로 본다 */ \
	} else {	/* [한국어] 읽기 방향. 장치에서 먼저 값을 가져온다 */ \
		ret = vfio_pci_core_ioread##size(vdev, test_mem,	/* [한국어] 장치 읽기. 실패하면 사용자 버퍼는 손대지 않는다 */ \
						 &val, io + off);	/* [한국어] 출력 인자로 val 을 받고, 주소는 마찬가지로 io + off */ \
		if (ret)	/* [한국어] 메모리 디코드 꺼짐 검사 */ \
			return ret;	/* [한국어] -EIO 를 그대로 올린다 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
		if (copy_to_user(buf, &val, sizeof(val)))	/* [한국어] 읽어 온 값을 사용자 버퍼로 내보낸다. 여기서 실패하면 장치 읽기는 이미 일어난 뒤다 - 읽기에 부작용이 있는 레지스터(read-to-clear 등)라면 그 부작용은 되돌릴 수 없다 */ \
			return -EFAULT;	/* [한국어] 사용자 포인터 오류 */ \
	}	/* [한국어] 방향 분기 끝 */ \
	/* [한국어] 빈 줄(매크로 계속) */ \
	*filled = sizeof(val);	/* [한국어] 이번 회차에 옮긴 바이트 수는 언제나 접근 폭과 같다. 부분 전송이라는 개념이 없다 - 성공이면 폭 전체, 실패면 0 이다 */ \
	return 0;	/* [한국어] 성공 */ \
}	/* [한국어] 매크로 본문 끝. [상류 코드 관찰] 이 닫는 중괄호 뒤에도 줄 잇기 백슬래시가 남아 있어, 매크로 본문이 바로 다음의 빈 줄까지 삼킨다. 형제인 VFIO_IOWRITE / VFIO_IOREAD 는 EXPORT_SYMBOL_GPL 줄에서 백슬래시 없이 끝난다. 컴파일 결과에는 영향이 없다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다 */ \

VFIO_IORDWR(8)	/* [한국어] u8 판 생성: vfio_pci_iordwr8 */
VFIO_IORDWR(16)	/* [한국어] u16 판 생성: vfio_pci_iordwr16 */
VFIO_IORDWR(32)	/* [한국어] u32 판 생성: vfio_pci_iordwr32 */
VFIO_IORDWR(64)	/* [한국어] u64 판 생성: vfio_pci_iordwr64 */

/* [한국어] vfio_pci_core_do_io_rw - 제외 구간을 피해 가며 요청 하나를 가장 넓은 자연
 * 정렬 접근들로 쪼개 옮기는 바이트 펌프
 *
 * @vdev: 대상 vfio-pci 디바이스. 아래 래퍼에 그대로 전달된다.
 * @test_mem: 참이면 접근마다 memory_lock 을 read 로 잡고 메모리 디코드를
 *   확인한다. 호출자가 자원 종류를 보고 정한다 - MMIO BAR 는 참,
 *   I/O 포트 BAR 와 레거시 VGA 는 거짓.
 * @io: region 의 시작 __iomem 주소. pci_iomap 이나 ioremap 이나 ioport_map 이
 *   돌려준 값이라 최소 페이지 정렬이 보장된다.
 * @buf: 사용자 버퍼. 진행하면서 앞으로 밀린다.
 * @off: region 시작으로부터의 바이트 오프셋. 진행하면서 커진다.
 * @count: 남은 바이트 수. 진행하면서 줄어든다.
 * @x_start: 제외 구간의 시작 오프셋(포함).
 * @x_end: 제외 구간의 끝 오프셋(불포함). x_start 와 x_end 가 모두 0 이면
 *   제외 구간이 없다는 뜻이 된다 - off 는 음수가 될 수 없으므로 첫 조건이
 *   항상 거짓이고 둘째 조건이 항상 참이 되기 때문이다.
 * @iswrite: 참이면 사용자 -> 장치.
 * @max_width: 이 region 에서 허용되는 최대 접근 폭(바이트). ROM 과 VGA 는 4,
 *   그 밖의 BAR 는 8. include/linux/vfio_pci_core.h:147~152 의 열거형이다.
 * @return: 0 이상이면 처리한 바이트 수(제외 구간에서 0xFF 로 채운 것도 포함),
 *   음수면 errno. 호출자는 0 이상일 때만 파일 위치를 전진시킨다.
 *
 * 왜 필요한가: 사용자는 임의의 오프셋에서 임의의 길이를 요청한다. 그런데
 * 장치 레지스터는 정렬된 폭 단위로만 접근해야 하고, 그 사이에 절대 진짜로
 * 읽거나 써서는 안 되는 구멍(MSI-X 테이블, ROM 이미지 뒤 여백)이 있다. 이
 * 함수가 그 둘을 동시에 만족시키는 유일한 지점이다.
 *
 * 동작 단계(회차마다 반복):
 *  (1) 지금 위치 off 가 제외 구간의 앞인지 뒤인지 안인지 판정해 fillable 을
 *      정한다. 앞이면 경계까지의 거리로 잘리고, 뒤면 남은 전부, 안이면 0 이다.
 *  (2) fillable 이 0 이 아니면, 폭 <= fillable 이고 off 가 그 폭으로 정렬돼
 *      있고 max_width 가 허용하는 가장 넓은 폭을 골라 vfio_pci_iordwr 계열을
 *      부른다. 8 -> 4 -> 2 -> 1 순으로 내려간다.
 *  (3) fillable 이 0 이면 제외 구간 안이다. 이번 덩어리 길이를 구간 끝(또는
 *      요청 끝)까지로 잡고, 쓰기면 아무 일도 하지 않고, 읽기면 0xFF 로 채운다.
 *  (4) 처리한 바이트 수만큼 count/done/off/buf 를 한꺼번에 전진시킨다.
 *
 * **경계를 걸치는 접근이 없다는 보장**은 (1)의 min 과 (2)의 폭 <= fillable
 * 조건에서 나온다. 하나의 ioread/iowrite 가 x_start 를 넘을 수 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥. copy_to_user / copy_from_user 가 페이지 폴트를
 * 낼 수 있고 down_read 가 잠들 수 있다. 원자적 문맥에서 부르면 안 된다.
 * 호출자: 같은 파일의 vfio_pci_bar_rw 와 vfio_pci_vga_rw, 그리고
 * EXPORT_SYMBOL_GPL 로 공개돼 있어 vendor 변종 드라이버의 region 접근 콜백도
 * 같은 규칙을 재사용한다.
 * 호출 대상: vfio_pci_iordwr8/16/32/64(이 파일의 매크로가 만든 것), copy_to_user.
 *
 * 에러 경로: 하위 호출이 음수를 돌려주면 그 자리에서 그대로 반환한다.
 * [상류 코드 관찰] 그때 이미 처리한 done 바이트는 버려진다. 즉 앞부분이
 * 실제로 장치에 나간 뒤에도 사용자에게는 오류만 보이고 몇 바이트가 나갔는지
 * 알 방법이 없으며, 호출자 vfio_pci_bar_rw 도 done 이 음수라 파일 위치를
 * 전진시키지 않는다. 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 호출 체인:
 *   vfio_pci_core.c 의 vfio_pci_rw(4841줄)
 *     -> vfio_pci_bar_rw -> [vfio_pci_core_do_io_rw]
 *     -> vfio_pci_iordwr64/32/16/8 -> vfio_pci_core_ioread/iowrite -> ioread/iowrite
 *
 * 바로 아래의 상류 영어 주석이 같은 계약을 한 문단으로 밝히고 있다 - 제외
 * 구간은 쓰기를 버리고 읽기를 -1(모든 비트가 1, 즉 0xFF 반복)로 채우며,
 * 그것이 MSI-X 벡터 테이블과 ROM BAR 의 남는 공간을 위한 것이라는 설명이다. */
/*
 * Read or write from an __iomem region (MMIO or I/O port) with an excluded
 * range which is inaccessible.  The excluded range drops writes and fills
 * reads with -1.  This is intended for handling MSI-X vector tables and
 * leftover space for ROM BARs.
 */
/* [한국어] region 하나에 대한 read/write 를 실제로 수행하는 진입점. 반환형 ssize_t 는 바이트 수와 음수 errno 를 겸한다. */
ssize_t vfio_pci_core_do_io_rw(struct vfio_pci_core_device *vdev, bool test_mem,
			       void __iomem *io, char __user *buf,
			       loff_t off, size_t count, size_t x_start,
			       size_t x_end, bool iswrite,
			       enum vfio_pci_io_width max_width)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 지금까지 처리한 누적 바이트 수. 성공 시 반환값이며, 제외 구간에서 0xFF 로 채운 바이트도 여기 포함된다 - 사용자에게는 요청한 길이가 전부 처리된 것처럼 보인다. */
	ssize_t done = 0;
	/* [한국어] 하위 호출의 반환값을 받을 자리. 0 이 아니면 즉시 반환한다. */
	int ret;

	/* [한국어] 남은 바이트가 0 이 될 때까지 반복한다. 회차마다 한 번의 장치 접근 또는 한 덩어리의 채우기가 일어난다. */
	while (count) {
		/* [한국어] fillable - 지금 위치에서 **진짜로 장치에 접근할 수 있는** 연속 바이트 수.
		 * filled - 이번 회차가 실제로 처리한 바이트 수. 접근 경로에서는 접근 폭과
		 * 같고, 제외 구간 경로에서는 구멍의 남은 길이다. 두 값이 다른 이유는
		 * 제외 구간에서는 한 번에 여러 바이트를 뭉텅이로 처리하기 때문이다. */
		size_t fillable, filled;

		/* [한국어] 지금 위치가 제외 구간보다 앞인가. x_start 가 0 이면(제외 없음) off 는 음수가 될 수 없으므로 이 조건은 결코 참이 되지 않는다. */
		if (off < x_start)
			/* [한국어] 경계까지의 거리와 남은 요청 길이 중 작은 쪽. **이 한 줄이 경계 걸침을
			 * 막는 전부다.** off < x_start 이므로 x_start - off 는 양수이고, size_t 로
			 * 캐스팅해도 감싸 돌지 않는다. 이렇게 잘라 두면 아래에서 고르는 접근 폭이
			 * fillable 이하로 제한되어 어떤 단일 접근도 x_start 를 넘어설 수 없다. */
			fillable = min(count, (size_t)(x_start - off));
		/* [한국어] 지금 위치가 제외 구간을 이미 지났는가. 제외가 없는 경우(x_end 가 0)에도 이 가지가 잡혀 아래 fillable = count 로 간다. */
		else if (off >= x_end)
			/* [한국어] 구멍 뒤쪽에는 더 이상 막을 것이 없으므로 남은 전부를 접근 가능하다고 본다. */
			fillable = count;
		/* [한국어] 남은 경우는 x_start <= off < x_end, 즉 구멍 한가운데다. */
		else
			/* [한국어] 0 으로 두면 아래 네 개의 폭 조건이 모두 거짓이 되어 마지막 else 의 채우기 경로로 떨어진다. */
			fillable = 0;

		/* [한국어] 8바이트 접근 조건 세 가지: 경계까지 8바이트 이상 남았고, off 가 8정렬이고, 이 region 이 qword 접근을 허용하는가. off 의 정렬로 판단해도 되는 이유는 io 가 최소 페이지 정렬이라 실제 주소의 하위 3비트가 off 의 하위 3비트와 같기 때문이다. */
		if (fillable >= 8 && !(off % 8) && max_width >= 8) {
			/* [한국어] 8바이트 한 번. filled 에 8 이 담긴다. */
			ret = vfio_pci_iordwr64(vdev, iswrite, test_mem,
						io, buf, off, &filled);
			/* [한국어] 복사 오류(-EFAULT)나 메모리 디코드 꺼짐(-EIO). */
			if (ret)
				/* [한국어] 지금까지의 done 을 버리고 오류만 돌려준다(위 함수 블록의 상류 코드 관찰 참조). */
				return ret;

		/* [한국어] 4바이트 조건. 8바이트가 안 되는 이유는 셋 중 하나 - 경계나 요청 끝이 가까움, off 가 8정렬이 아님, region 이 qword 를 금지함(ROM, VGA). */
		} else if (fillable >= 4 && !(off % 4) && max_width >= 4) {
			/* [한국어] 4바이트 한 번. filled 에 4. */
			ret = vfio_pci_iordwr32(vdev, iswrite, test_mem,
						io, buf, off, &filled);
			/* [한국어] 오류 검사. */
			if (ret)
				/* [한국어] 즉시 반환. */
				return ret;

		/* [한국어] 2바이트 조건. 여기까지 내려왔다면 off 가 4정렬이 아니거나 남은 길이가 4 미만이다. */
		} else if (fillable >= 2 && !(off % 2) && max_width >= 2) {
			/* [한국어] 2바이트 한 번. filled 에 2. */
			ret = vfio_pci_iordwr16(vdev, iswrite, test_mem,
						io, buf, off, &filled);
			/* [한국어] 오류 검사. */
			if (ret)
				/* [한국어] 즉시 반환. */
				return ret;

		/* [한국어] 여기까지 왔으면 폭을 더 줄일 수 없다. fillable 이 0 만 아니면 1바이트씩이라도 옮긴다. 정렬되지 않은 시작 오프셋이 여기로 들어와 한 바이트를 옮기고 나면 다음 회차에서는 정렬이 맞아 폭이 다시 넓어진다. */
		} else if (fillable) {
			/* [한국어] 1바이트 한 번. filled 에 1. */
			ret = vfio_pci_iordwr8(vdev, iswrite, test_mem,
					       io, buf, off, &filled);
			/* [한국어] 오류 검사. */
			if (ret)
				/* [한국어] 즉시 반환. */
				return ret;

		/* [한국어] fillable 이 0 - 제외 구간 한가운데다. 여기서는 장치를 전혀 건드리지 않는다. */
		} else {
			/* [한국어] 아래 영어 주석이 규칙을 한 줄로 요약한다 - 읽기는 -1 로 채우고 쓰기는 버린다. */
			/* Fill reads with -1, drop writes */
			/* [한국어] 이번 덩어리 길이는 구멍의 끝까지, 또는 요청이 먼저 끝나면 거기까지다.
			 * 여기서는 off < x_end 가 보장되므로 x_end - off 는 양수다. 접근 경로와 달리
			 * 폭 단위로 쪼개지 않고 한 번에 처리하는 이유는, 장치를 건드리지 않으므로
			 * 정렬을 지킬 이유가 없기 때문이다. */
			filled = min(count, (size_t)(x_end - off));
			/* [한국어] 쓰기이면 이 블록을 통째로 건너뛴다 - **그것이 곧 '쓰기를 버린다' 는 구현이다.** 오류를 돌려주지 않고 조용히 성공한 척한다. 사용자가 MSI-X 테이블에 쓴 값은 아무 데도 가지 않는다. */
			if (!iswrite) {
				/* [한국어] 채울 값. 실제 PCI 버스에서 응답 없는 읽기가 마스터 어보트로 돌려주는 all-ones 와 같은 값이라, 사용자 입장에서는 '그 자리에 아무것도 없다' 는 관례적 신호로 읽힌다. */
				u8 val = 0xFF;
				/* [한국어] 채우기 루프의 인덱스. */
				size_t i;

				/* [한국어] filled 바이트를 채운다. */
				for (i = 0; i < filled; i++)
					/* [한국어] 한 번에 한 바이트씩 copy_to_user 를 부른다. memset 한 임시 버퍼를
					 * 한꺼번에 복사하는 대신 이렇게 하는 것은 임시 버퍼를 잡지 않기 위해서다.
					 * 대신 4KB MSI-X 테이블 전체를 읽으면 copy_to_user 가 4096번 불린다.
					 * 그래도 진짜 MMIO 읽기(한 번에 마이크로초 단위의 PCIe 왕복)보다는 훨씬
					 * 빠르며, 그 속도 차이가 ROM 여백을 제외 구간으로 만드는 이유이기도 하다. */
					if (copy_to_user(buf + i, &val, 1))
						/* [한국어] 사용자 포인터 오류. 이때도 done 은 버려진다. */
						return -EFAULT;
			/* [한국어] 읽기 채우기 블록 끝. */
			}
		/* [한국어] 제외 구간 처리 끝. */
		}

		/* [한국어] 남은 요청 길이를 줄인다. filled 는 접근 경로에서는 폭, 채우기 경로에서는 덩어리 길이다. */
		count -= filled;
		/* [한국어] 누적 처리량을 늘린다. 이 값이 반환값이 된다. */
		done += filled;
		/* [한국어] region 안 위치를 전진시킨다. 다음 회차의 제외 구간 판정과 정렬 판정이 이 값으로 다시 이루어진다. */
		off += filled;
		/* [한국어] 사용자 버퍼 포인터도 같은 만큼 전진시킨다. 제외 구간을 지나온 경우에도 전진하므로, 구멍 뒤의 진짜 데이터가 사용자 버퍼의 올바른 자리에 놓인다. */
		buf += filled;
	/* [한국어] 루프 끝. count 가 0 이 되면 빠져나온다. filled 가 0 이 될 수 있는 경우가 없으므로 무한 루프는 생기지 않는다 - 접근 경로는 최소 1바이트, 채우기 경로는 off < x_end 이므로 최소 1바이트를 처리한다. */
	}

	/* [한국어] 요청한 만큼 전부 처리했다. 호출자는 이 값만큼 파일 위치를 전진시킨다. */
	return done;
}
/* [한국어] vendor 변종 드라이버가 자기 region 의 read/write 를 구현할 때 이 펌프를 그대로 재사용할 수 있게 공개한다. nvgrace-gpu 처럼 BAR 를 다르게 노출하는 드라이버가 이것을 쓴다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_do_io_rw);

/* [한국어] vfio_pci_core_setup_barmap - BAR 하나를 예약하고 매핑해 vdev->barmap 에 캐시한다
 *
 * @vdev: 대상 vfio-pci 디바이스. 결과가 vdev->barmap[bar] 에 남는다.
 * @bar: BAR 번호 0~5. ROM(6)은 이 함수를 쓰지 않는다 - ROM 은 요청마다
 *   매핑하고 부수기 때문이다.
 * @return: 0 = 성공(이미 매핑돼 있던 경우 포함),
 *   pci_request_selected_regions 의 오류(보통 -EBUSY, 다른 드라이버가 그 자원을
 *   이미 잡고 있음), 또는 -ENOMEM(주소 공간 매핑 실패).
 *
 * 왜 필요한가: BAR 접근 경로가 셋(read/write, mmap, ioeventfd)인데 셋 다 같은
 * 매핑을 원한다. 매번 ioremap 하면 낭비이고, 미리 전부 매핑하면 쓰지도 않을
 * BAR 의 주소 공간을 잡아먹는다. 그래서 **처음 필요해진 순간에 한 번만**
 * 매핑하고 vdev->barmap 배열(include/linux/vfio_pci_core.h:102)에 캐시한다.
 *
 * 동작 단계: (1) 이미 캐시돼 있으면 즉시 0. (2) pci_request_selected_regions 로
 * 그 BAR 자원을 "vfio" 라는 이름으로 예약한다 - /proc/iomem 에 그 이름이
 * 찍히고, 다른 드라이버가 같은 자원을 잡지 못하게 된다. (3) pci_iomap 으로
 * 매핑한다. maxlen 을 0 으로 주어 BAR 전체를 매핑한다. (4) 실패하면 (2)의
 * 예약을 되돌리고 -ENOMEM. (5) 성공하면 캐시에 넣는다.
 *
 * pci_iomap(drivers/pci/iomap.c:308)이 중요한 이유: 그 함수가 IORESOURCE_MEM 이면
 * ioremap 을, IORESOURCE_IO 이면 ioport_map 을 골라 **둘 다 같은 __iomem 토큰**을
 * 돌려준다. 그래서 이 파일 위쪽의 접근 코드가 MMIO 경로와 I/O 포트 경로를
 * 따로 쓰지 않아도 된다. 두 경로의 차이는 오직 test_mem 인자 하나 -
 * memory_lock 과 Memory Space Enable 검사를 하느냐 마느냐뿐이다.
 *
 * 해제는 이 함수가 하지 않는다. vfio_pci_core.c 의 vfio_pci_core_disable(2199줄)이
 * 장치를 닫을 때 barmap 전체를 pci_iounmap 하고 예약도 함께 놓는다. 즉 이
 * 함수의 짝은 이 파일이 아니라 코어에 있다.
 *
 * 실행 컨텍스트: 프로세스 문맥. ioremap 이 페이지 테이블을 잡으므로 잠들 수 있다.
 * 호출자: 같은 파일의 vfio_pci_bar_rw 와 vfio_pci_ioeventfd,
 * vfio_pci_core.c 의 vfio_pci_core_mmap(5561줄), 그리고 EXPORT_SYMBOL_GPL 로
 * 공개돼 있어 vendor 변종 드라이버도 부른다.
 * 호출 대상: pci_request_selected_regions(drivers/pci/pci.c:7343),
 * pci_iomap(drivers/pci/iomap.c:308),
 * pci_release_selected_regions(drivers/pci/pci.c:7258).
 *
 * 에러 경로: 예약 실패는 그대로 위로 올리고, 매핑 실패는 예약을 되돌린 뒤
 * -ENOMEM 을 올린다. 두 경우 모두 barmap 은 건드리지 않아 다음 호출이 다시
 * 시도할 수 있다.
 *
 * 호출 체인:
 *   vfio_pci_bar_rw / vfio_pci_ioeventfd / vfio_pci_core_mmap
 *     -> [vfio_pci_core_setup_barmap] -> pci_request_selected_regions -> pci_iomap */
int vfio_pci_core_setup_barmap(struct vfio_pci_core_device *vdev, int bar)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] vfio 디바이스에서 실제 PCI 함수를 꺼낸다. 아래 세 개의 PCI API 가 모두 이것을 요구한다. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] pci_request_selected_regions 의 반환값을 받을 자리. */
	int ret;
	/* [한국어] pci_iomap 의 결과. 성공해야만 barmap 에 들어간다. */
	void __iomem *io;

	/* [한국어] 이미 매핑해 둔 BAR 인가. 이 캐시가 있어서 read/write 를 아무리 많이 걸어도 매핑은 한 번뿐이다. */
	if (vdev->barmap[bar])
		/* [한국어] 이미 있으면 할 일이 없다. */
		return 0;

	/* [한국어] 1 << bar 는 BAR 번호를 비트마스크로 바꾼 것이다. 이 API 가 여러 BAR 를
	 * 한 번에 예약할 수 있게 마스크를 받기 때문이며, 여기서는 한 비트만 세운다.
	 * "vfio" 는 자원 소유자 이름으로 /proc/iomem 과 /proc/ioports 에 그대로 찍힌다 -
	 * 어떤 장치가 사용자 공간에 넘어가 있는지 사람이 눈으로 확인할 수 있는 표시다. */
	ret = pci_request_selected_regions(pdev, 1 << bar, "vfio");
	/* [한국어] 예약 실패. 보통 -EBUSY 로, 커널 드라이버가 아직 그 자원을 붙잡고 있다는 뜻이다. */
	if (ret)
		/* [한국어] 그대로 위로 올린다. 아직 아무것도 잡지 않았으므로 되돌릴 것이 없다. */
		return ret;

	/* [한국어] BAR 를 커널 주소 공간에 매핑한다. 세 번째 인자 maxlen 이 0 이면 BAR
	 * 전체를 매핑하라는 뜻이다(drivers/pci/iomap.c:308 과 그 아래 pci_iomap_range).
	 * 이 한 줄이 MMIO 와 I/O 포트의 차이를 흡수한다 - 메모리 자원이면 ioremap 을,
	 * I/O 자원이면 ioport_map 을 골라 같은 모양의 __iomem 포인터를 돌려준다. */
	io = pci_iomap(pdev, bar, 0);
	/* [한국어] 매핑 실패. 32비트에서 큰 BAR 를 vmalloc 공간에 못 넣는 경우가 대표적이다. */
	if (!io) {
		/* [한국어] 앞서 성공한 예약을 되돌린다. **이 한 줄이 있어야 실패한 뒤에도 자원이
		 * 새지 않는다.** 반대 방향의 짝(성공 경로의 해제)은 이 파일이 아니라
		 * vfio_pci_core.c 의 vfio_pci_core_disable(2199줄)에 있다. */
		pci_release_selected_regions(pdev, 1 << bar);
		/* [한국어] 매핑 실패는 언제나 -ENOMEM 으로 보고한다. */
		return -ENOMEM;
	/* [한국어] 매핑 실패 처리 끝. */
	}

	/* [한국어] 캐시에 넣는다. 이후의 모든 접근은 위쪽 if 에서 곧바로 이 값을 쓴다. 이 배열을 비우는 쪽도 코어의 disable 이다. */
	vdev->barmap[bar] = io;

	/* [한국어] 성공. */
	return 0;
}
/* [한국어] vendor 변종 드라이버가 자기 BAR 를 매핑할 때 같은 캐시를 쓰도록 공개한다. */
EXPORT_SYMBOL_GPL(vfio_pci_core_setup_barmap);

/* [한국어] vfio_pci_bar_rw - BAR 와 ROM region 의 read(2)/write(2) 진입점
 *
 * @vdev: 대상 vfio-pci 디바이스.
 * @buf: 사용자 버퍼.
 * @count: 사용자가 요청한 바이트 수. 이 함수 안에서 region 끝까지로 잘린다.
 * @ppos: 입출력 겸용 파일 위치. 상위 비트에 region 번호가, 하위 40비트에
 *   region 안 오프셋이 들어 있다. 성공하면 처리한 만큼 전진시킨다.
 * @iswrite: 참이면 write(2), 거짓이면 read(2).
 * @return: 0 이상이면 처리한 바이트 수, 음수면 errno
 *   (-EINVAL = 그런 BAR/ROM 이 없거나 오프셋이 범위 밖, -ENOMEM = 매핑 실패,
 *   그 밖에는 하위 경로가 올린 값).
 *
 * 왜 필요한가: 이 함수가 하는 일은 **제외 구간을 정하고 매핑을 준비하는 것**
 * 뿐이다. 실제 바이트 이동은 vfio_pci_core_do_io_rw 가 한다. 그런데 그
 * "준비" 가 이 서브시스템의 보안 경계 그 자체다 - 여기서 MSI-X 테이블 구간을
 * 도려내지 않으면 사용자가 read/write 로 MSI-X 벡터의 주소 필드를 직접 고쳐
 * 장치가 임의의 호스트 메모리로 인터럽트를 쏘게 만들 수 있다.
 *
 * 동작 단계:
 *  (1) ppos 를 오프셋과 region 번호로 쪼갠다.
 *  (2) region 의 끝 end 를 정한다. 하드웨어 BAR 가 주소를 받았으면 그 길이,
 *      ROM 인데 펌웨어 사본만 있으면 romlen 을 2의 거듭제곱으로 올림한 값,
 *      둘 다 아니면 -EINVAL.
 *  (3) 오프셋이 끝을 넘으면 -EINVAL, 아니면 count 를 끝까지로 자른다.
 *  (4) ROM 이면 두 출처 중 하나로 매핑하고 제외 구간을 이미지 뒤 여백으로
 *      잡고 최대 폭을 4바이트로 낮춘다. 아니면 barmap 을 확보한다.
 *  (5) 그 BAR 가 MSI-X 테이블을 품고 있으면 제외 구간을 테이블로 **덮어쓴다**.
 *      ROM(6)과 MSI-X BAR(0~5)는 겹칠 수 없으므로 충돌하지 않는다.
 *  (6) 펌프를 돌리고, 성공한 만큼 파일 위치를 전진시키고, ROM 매핑을 되돌린다.
 *
 * test_mem 인자로 `res->flags & IORESOURCE_MEM` 을 넘기는 것이 (6)의 핵심이다 -
 * 메모리 자원이면 접근마다 memory_lock 과 Memory Space Enable 검사를 받고,
 * I/O 포트 자원이면 그 검사가 생략된다. I/O 포트 디코딩은 PCI_COMMAND 의 다른
 * 비트(I/O Space Enable)가 관장하며 이 파일은 그것을 보지 않는다.
 *
 * 실행 컨텍스트: 프로세스 문맥(read/write 시스템 콜). 잠들 수 있다.
 * 호출자: vfio_pci_core.c 의 vfio_pci_rw(4841줄). 그 함수가 region 번호를 보고
 * BAR0~5 와 ROM 을 이쪽으로 보낸다.
 * 호출 대상: pci_resource_start / pci_resource_len, roundup_pow_of_two,
 * pci_map_rom(drivers/pci/rom.c:382), ioremap, vfio_pci_core_setup_barmap,
 * vfio_pci_core_do_io_rw, pci_unmap_rom(drivers/pci/rom.c:475), iounmap.
 *
 * 에러 경로: (2)와 (3)의 -EINVAL 은 아무것도 잡기 전이라 그냥 반환한다.
 * ROM 매핑 실패의 -ENOMEM 도 마찬가지다. barmap 확보 실패만 done 에 담아
 * out 라벨로 가는데, 그 라벨이 하는 일도 결국 반환뿐이다.
 *
 * 호출 체인:
 *   사용자의 read(2)/write(2) -> vfio_main.c 의 파일 오퍼레이션
 *     -> vfio_pci_core_read/_write -> vfio_pci_rw(vfio_pci_core.c:4841)
 *     -> [vfio_pci_bar_rw] -> vfio_pci_core_do_io_rw */
ssize_t vfio_pci_bar_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			size_t count, loff_t *ppos, bool iswrite)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 실제 PCI 함수. 자원 조회와 ROM 매핑에 쓰인다. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 파일 오프셋에서 region 안 위치만 남긴다. VFIO_PCI_OFFSET_MASK 는
	 * include/linux/vfio_pci_core.h 에 있으며 하위 40비트를 남기는 마스크다.
	 * 상위 비트는 region 번호이므로 여기서 지워야 BAR 안 오프셋이 된다. */
	loff_t pos = *ppos & VFIO_PCI_OFFSET_MASK;
	/* [한국어] 같은 파일 오프셋의 상위 비트를 40비트 오른쪽으로 밀어 region 번호를 얻는다. 0~5 는 BAR, 6 은 ROM(PCI_ROM_RESOURCE)이다. */
	int bar = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	/* [한국어] 제외 구간의 초기값. 둘 다 0 이면 '제외 없음' 을 뜻한다 - do_io_rw 의 판정식이 그렇게 동작한다. */
	size_t x_start = 0, x_end = 0;
	/* [한국어] 이 region 의 길이(끝 오프셋). 아래 세 갈래 중 하나로 정해진다. */
	resource_size_t end;
	/* [한국어] 펌프에 넘길 매핑된 시작 주소. */
	void __iomem *io;
	/* [한국어] 이 BAR 의 자원 서술자. 아래에서 IORESOURCE_MEM 비트만 보고 test_mem 을
	 * 정한다. pdev 지역 변수 대신 vdev->pdev 를 다시 따라가는 것은 원본 그대로의
	 * 표기이며 가리키는 대상은 같다. */
	struct resource *res = &vdev->pdev->resource[bar];
	/* [한국어] 펌프의 반환값. 바이트 수 또는 음수 errno. */
	ssize_t done;
	/* [한국어] 기본 최대 접근 폭은 8바이트. 아래 ROM 분기에서만 4바이트로 낮춘다. */
	enum vfio_pci_io_width max_width = VFIO_PCI_IO_WIDTH_8;

	/* [한국어] 하드웨어 BAR(또는 ROM BAR)가 실제로 주소를 배정받았는가. 배정 전이면 시작 주소가 0 이다. */
	if (pci_resource_start(pdev, bar))
		/* [한국어] 배정돼 있으면 그 자원의 길이가 곧 region 크기다. */
		end = pci_resource_len(pdev, bar);
	/* [한국어] 배정돼 있지 않은데 ROM region 이고, 펌웨어가 시스템 메모리에 떠 둔
	 * 사본이 있는 경우. 일부 장치는 초기화 뒤 ROM 을 읽을 수 없게 되므로 펌웨어가
	 * 미리 복사해 둔다(drivers/pci/rom.c:30~34 의 설명). pdev->rom 이 그 사본의
	 * 물리 주소, pdev->romlen 이 길이다. */
	else if (bar == PCI_ROM_RESOURCE && pdev->rom && pdev->romlen)
		/* [한국어] 사본 길이를 2의 거듭제곱으로 올림해 region 크기로 삼는다. 사용자에게
		 * 보이는 region 크기는 진짜 BAR 처럼 2의 거듭제곱이어야 하기 때문이다.
		 * 올림하면서 생긴 여백은 아래에서 제외 구간이 되어 0xFF 로 채워진다. */
		end = roundup_pow_of_two(pdev->romlen);
	/* [한국어] 그 밖의 경우 - ROM 도 없고 BAR 도 배정되지 않았다. */
	else
		/* [한국어] 읽을 것이 없으므로 -EINVAL. 사용자가 존재하지 않는 region 을 건드린 것이다. */
		return -EINVAL;

	/* [한국어] 시작 오프셋이 이미 region 끝을 넘었는가. */
	if (pos >= end)
		/* [한국어] 끝을 넘은 곳에서 시작하는 요청은 잘라 낼 것도 없으므로 오류다. 0 바이트 반환이 아니라 -EINVAL 인 점에 유의. */
		return -EINVAL;

	/* [한국어] 요청 길이를 region 끝까지로 자른다. 이 한 줄 덕분에 아래 펌프는 범위
	 * 검사를 전혀 하지 않아도 된다 - off + count 가 region 을 벗어날 수 없다.
	 * end - pos 는 위에서 pos < end 를 확인했으므로 양수다. */
	count = min(count, (size_t)(end - pos));

	/* [한국어] ROM region 인가. ROM 은 다른 BAR 와 매핑 방식도 다르고 제외 구간도 다르다. */
	if (bar == PCI_ROM_RESOURCE) {
		/* [한국어] 아래 영어 주석대로, ROM 이미지는 BAR 창보다 작을 수 있다. 그래서 제외
		 * 구간을 **진짜 ROM 이 끝나는 곳**부터 시작하게 만든다. 그러면 큰 ROM BAR 를
		 * 읽을 때 여백을 진짜 MMIO 로 훑지 않고 메모리 채우기로 끝낼 수 있어 훨씬
		 * 빠르다. */
		/*
		 * The ROM can fill less space than the BAR, so we start the
		 * excluded range at the end of the actual ROM.  This makes
		 * filling large ROM BARs much faster.
		 */
		/* [한국어] ROM BAR 가 주소를 배정받은 경우 - 진짜 하드웨어 ROM 을 읽는다. */
		if (pci_resource_start(pdev, bar)) {
			/* [한국어] ROM 디코딩을 켜고 창을 매핑한다. 두 번째 인자가 출력 인자여서,
			 * pci_map_rom(drivers/pci/rom.c:382)이 이미지 사슬을 훑어 알아낸 **실제 ROM
			 * 크기**가 x_start 에 담긴다. 즉 이 한 줄이 매핑과 제외 구간 시작을 동시에
			 * 정한다. 매핑을 barmap 에 캐시하지 않고 요청마다 만들고 부수는 이유는, ROM
			 * 디코딩을 계속 켜 두면 그 창이 다른 BAR 와 주소 공간을 다투기 때문이다. */
			io = pci_map_rom(pdev, &x_start);
		/* [한국어] BAR 는 배정되지 않았지만 펌웨어 사본이 있는 경우. */
		} else {
			/* [한국어] 사본의 물리 주소를 그대로 매핑한다. 장치가 아니라 시스템 메모리를 읽는 것이므로 ROM 디코딩을 켤 필요가 없다. */
			io = ioremap(pdev->rom, pdev->romlen);
			/* [한국어] 사본의 길이가 곧 진짜 내용의 끝이다. 그 뒤(2의 거듭제곱으로 올림하며 생긴 여백)는 제외 구간이 된다. */
			x_start = pdev->romlen;
		/* [한국어] ROM 출처 분기 끝. 두 갈래 모두 io 와 x_start 를 채웠다. */
		}
		/* [한국어] 두 경로 어느 쪽이든 매핑에 실패했는가. */
		if (!io)
			/* [한국어] -ENOMEM. 아직 아무것도 잡지 않았으므로 out 라벨로 갈 필요 없이 바로 반환한다. */
			return -ENOMEM;
		/* [한국어] 제외 구간의 끝은 region 의 끝이다. 즉 [실제 ROM 크기, region 크기) 전체가 구멍이 된다. */
		x_end = end;

		/* [한국어] 아래 영어 주석대로, Intel X710 같은 일부 장치는 ROM BAR 를 qword(8바이트)로
		 * 읽으면 PCI AER 오류를 일으킨다. qword 접근이 도입되기 전 오랫동안 4바이트
		 * 접근이 안정적으로 동작했으므로, 장치를 가리지 않고 ROM 은 4바이트로 제한한다. */
		/*
		 * Certain devices (e.g. Intel X710) don't support qword
		 * access to the ROM bar. Otherwise PCI AER errors might be
		 * triggered.
		 *
		 * Disable qword access to the ROM bar universally, which
		 * worked reliably for years before qword access is enabled.
		 */
		/* [한국어] 최대 폭을 4바이트로 낮춘다. 이 값이 펌프의 8바이트 분기를 통째로 막는다. */
		max_width = VFIO_PCI_IO_WIDTH_4;
	/* [한국어] ROM 이 아닌 일반 BAR(0~5). */
	} else {
		/* [한국어] BAR 를 예약하고 매핑한다. 이미 돼 있으면 즉시 성공하므로 반복 호출 비용이 없다. */
		int ret = vfio_pci_core_setup_barmap(vdev, bar);
		/* [한국어] 예약이나 매핑 실패. */
		if (ret) {
			/* [한국어] 오류 코드를 반환값 자리에 담는다. 아래 out 라벨이 이 값을 그대로 돌려준다. */
			done = ret;
			/* [한국어] 정리 구간으로 간다. 여기서는 ROM 매핑이 없으므로 out 이 하는 일은 반환뿐이다. */
			goto out;
		}

		/* [한국어] 캐시된 매핑을 꺼내 쓴다. ROM 과 달리 이 매핑은 장치를 닫을 때까지 살아 있다. */
		io = vdev->barmap[bar];
	/* [한국어] region 종류 분기 끝. 두 갈래 모두 io 를 채웠다. */
	}

	/* [한국어] 이 BAR 가 MSI-X 테이블을 품고 있는가. **여기가 이 파일 전체에서 가장
	 * 중요한 세 줄이다.** msix_bar 는 vfio_pci_core_enable(vfio_pci_core.c:1948)이
	 * MSI-X capability 의 Table Offset/BIR 레지스터 하위 3비트에서 뽑아 둔 값이고,
	 * MSI-X 가 없는 장치에서는 0xFF 라 어떤 BAR 번호와도 맞지 않는다
	 * (vfio_pci_core.c:2092). ROM 은 6번이므로 위 ROM 분기가 정한 제외 구간을
	 * 이 조건이 덮어쓸 일도 없다. */
	if (bar == vdev->msix_bar) {
		/* [한국어] 제외 구간의 시작 = MSI-X 테이블의 BAR 안 오프셋. Table Offset 레지스터의 상위 비트에서 온 값이라 이미 8바이트 정렬돼 있다. */
		x_start = vdev->msix_offset;
		/* [한국어] 제외 구간의 끝 = 테이블 시작 + 테이블 크기. 크기는 Table Size 필드
		 * +1 에 항목 하나의 크기 16바이트를 곱한 값이다(vfio_pci_core.c:2080).
		 * 이 구간 안에서 사용자의 쓰기는 버려지고 읽기는 0xFF 로 채워진다.
		 * **mmap 경로는 이 구간을 가리지 않는다** - 그 비대칭이 이 서브시스템의
		 * 알려진 성질이며 vfio_pci_core.c 의 파일 상단 주석이 그 배경을 설명한다. */
		x_end = vdev->msix_offset + vdev->msix_size;
	/* [한국어] MSI-X 제외 구간 설정 끝. */
	}

	/* [한국어] 이제 준비가 끝났으므로 펌프를 돌린다. 두 번째 인자가 test_mem 이며,
	 * 자원이 메모리 공간이면 참이 되어 접근마다 memory_lock 을 read 로 잡고
	 * PCI_COMMAND 의 Memory Space Enable 을 확인한다. I/O 포트 BAR 는 거짓이
	 * 되어 그 검사를 건너뛴다 - I/O 공간은 다른 비트가 관장하기 때문이다. */
	done = vfio_pci_core_do_io_rw(vdev, res->flags & IORESOURCE_MEM, io, buf, pos,
				      count, x_start, x_end, iswrite, max_width);

	/* [한국어] 펌프가 성공했는가(0 이상). */
	if (done >= 0)
		/* [한국어] 처리한 만큼 파일 위치를 전진시킨다. 오류였다면 전진시키지 않아
		 * 사용자가 같은 자리에서 다시 시도할 수 있다. 제외 구간에서 0xFF 로 채운
		 * 바이트도 done 에 포함돼 있으므로 위치는 요청한 만큼 전진한다. */
		*ppos += done;

	/* [한국어] ROM 이었으면 매핑을 되돌려야 한다. 일반 BAR 의 매핑은 캐시에 남겨 두므로 여기서 손대지 않는다. */
	if (bar == PCI_ROM_RESOURCE) {
		/* [한국어] 위에서 어느 갈래로 매핑했는지 같은 조건으로 다시 판단한다. */
		if (pci_resource_start(pdev, bar))
			/* [한국어] ROM 디코딩을 끄고 매핑을 푼다. pci_map_rom 이 켜기 전에 이미 켜져 있었다면 그대로 두는 판단도 그 함수가 한다(drivers/pci/rom.c:475). */
			pci_unmap_rom(pdev, io);
		/* [한국어] 펌웨어 사본이었던 경우. */
		else
			/* [한국어] 평범한 iounmap. 디코딩을 건드린 적이 없으므로 되돌릴 것도 없다. */
			iounmap(io);
	/* [한국어] ROM 정리 끝. */
	}

/* [한국어] 정리 지점. 실제로는 barmap 확보 실패만 여기로 점프해 오며, 그 경우 정리할 매핑이 없으므로 아래 반환만 남는다. */
out:
	/* [한국어] 바이트 수 또는 음수 errno 를 그대로 돌려준다. */
	return done;
}

/* [한국어] 레거시 VGA 지원은 컴파일 옵션이다. 꺼져 있으면 이 함수 자체가 사라지고,
 * vfio_pci_priv.h:47~57 의 static inline stub 이 대신 -EINVAL 을 돌려준다.
 * 그래서 호출자인 vfio_pci_core.c 의 vfio_pci_rw 는 #ifdef 없이 부를 수 있다. */
#ifdef CONFIG_VFIO_PCI_VGA
/* [한국어] vfio_pci_vga_rw - 레거시 VGA 주소 세 구간의 read(2)/write(2) 진입점
 *
 * @vdev: 대상 vfio-pci 디바이스. has_vga 플래그와 pdev 를 여기서 얻는다.
 * @buf: 사용자 버퍼.
 * @count: 요청 바이트 수. 각 구간의 끝까지로 잘린다.
 * @ppos: 파일 위치. 하위 40비트가 **레거시 물리 주소 그 자체**다.
 * @iswrite: 참이면 write(2).
 * @return: 0 이상이면 처리한 바이트 수, -EINVAL(VGA 아님/주소 밖),
 *   -ENOMEM(매핑 실패), 또는 vga_get_interruptible 이 올린 값
 *   (시그널로 깨면 -ERESTARTSYS).
 *
 * 왜 필요한가: VGA 호환 장치는 BAR 와 무관하게 x86 의 고정된 레거시 주소에서도
 * 응답한다 - 0xA0000~0xBFFFF 의 프레임버퍼 창과 0x3B0~0x3BB, 0x3C0~0x3DF 의
 * I/O 포트다. 게스트에서 비디오 BIOS 를 돌리려면 그 주소들에 닿아야 하므로
 * VFIO 가 별도 region 으로 노출한다. BAR 가 아니므로 barmap 캐시를 쓸 수 없고,
 * 여러 VGA 카드가 같은 주소를 두고 다투므로 중재자에게 소유권을 빌려야 한다는
 * 점이 vfio_pci_bar_rw 와의 결정적인 차이다.
 *
 * 동작 단계:
 *  (1) 이 장치가 VGA region 을 노출하는지 확인한다(has_vga).
 *  (2) 주소가 0xBFFFF 를 넘으면 거절한다.
 *  (3) 세 구간 중 어디인지 switch 로 가른다. 각 구간마다 요청 길이를 구간
 *      끝까지로 자르고, 그 구간 전체를 매핑하고, 구간 시작으로부터의 오프셋을
 *      구하고, 중재자에게 요구할 자원 종류와 해제 방법을 정한다. 어느 구간에도
 *      속하지 않으면(예: 0x3BC~0x3BF 나 0x3E0~0x9FFFF) -EINVAL.
 *  (4) VGA 중재자에게서 그 자원을 얻는다. 다른 카드가 쓰고 있으면 잠들어
 *      기다린다(drivers/pci/vgaarb.c:1542 의 vga_get).
 *  (5) 펌프를 돌린다. 제외 구간은 비어 있고(0, 0) 최대 폭은 4바이트,
 *      test_mem 은 거짓이다.
 *  (6) 중재자에게 자원을 돌려주고, 매핑을 풀고, 파일 위치를 전진시킨다.
 *
 * **제외 구간이 없다**는 점이 vfio_pci_bar_rw 와 크게 다르다. 레거시 VGA 에는
 * MSI-X 테이블이 있을 수 없으므로 가릴 것이 없다. x_start 와 x_end 를 둘 다
 * 0 으로 넘기는 것이 "제외 없음" 의 관용 표현이다.
 *
 * 실행 컨텍스트: 프로세스 문맥. ioremap 과 vga_get 이 모두 잠들 수 있다.
 * 호출자: vfio_pci_core.c 의 vfio_pci_rw(4841줄)가 VGA region 인덱스를 보고
 * 이쪽으로 보낸다.
 * 호출 대상: ioremap / ioport_map, vga_get_interruptible(실체는
 * drivers/pci/vgaarb.c:1542 의 vga_get 에 interruptible=1),
 * vfio_pci_core_do_io_rw, vga_put(drivers/pci/vgaarb.c:1792),
 * iounmap / ioport_unmap.
 *
 * 에러 경로: (1)(2)(3)의 -EINVAL 은 아무것도 잡기 전이다. 매핑 실패의 -ENOMEM
 * 도 마찬가지다. 중재자 획득 실패만 매핑을 되돌린 뒤 반환한다. 성공 경로에서는
 * 중재자 반환과 매핑 해제가 항상 짝을 이룬다.
 *
 * 호출 체인:
 *   사용자의 read(2)/write(2) -> vfio_pci_rw(vfio_pci_core.c:4841)
 *     -> [vfio_pci_vga_rw] -> vga_get_interruptible
 *     -> vfio_pci_core_do_io_rw -> vga_put */
ssize_t vfio_pci_vga_rw(struct vfio_pci_core_device *vdev, char __user *buf,
			       size_t count, loff_t *ppos, bool iswrite)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] vga_get_interruptible 의 반환값을 받을 자리. */
	int ret;
	/* [한국어] off 는 고른 구간 안에서의 오프셋, pos 는 파일 위치에서 뽑은 레거시 주소다. 상위 비트의 region 번호를 마스크로 지운다. */
	loff_t off, pos = *ppos & VFIO_PCI_OFFSET_MASK;
	/* [한국어] 매핑 결과. NULL 로 시작해 switch 의 각 갈래가 채운다. */
	void __iomem *iomem = NULL;
	/* [한국어] 중재자에게 요구할 자원 종류 - 프레임버퍼 창이면 VGA_RSRC_LEGACY_MEM, 포트면 VGA_RSRC_LEGACY_IO. */
	unsigned int rsrc;
	/* [한국어] 해제 방법을 기억해 둔다. 참이면 ioport_unmap, 거짓이면 iounmap 을 써야 한다. */
	bool is_ioport;
	/* [한국어] 펌프의 반환값. */
	ssize_t done;

	/* [한국어] 이 장치가 VGA region 을 노출하는가. vfio_pci_core_enable(vfio_pci_core.c:1948)이 VGA 클래스 장치이면서 disable_vga 모듈 파라미터가 꺼져 있을 때만 이 플래그를 세운다. */
	if (!vdev->has_vga)
		/* [한국어] VGA 가 아니면 이 region 자체가 없다. */
		return -EINVAL;

	/* [한국어] 레거시 VGA 주소의 상한. 0xBFFFF 는 프레임버퍼 창의 마지막 바이트다. ul 접미사는 32비트에서도 상수가 잘리지 않게 한다. */
	if (pos > 0xbfffful)
		/* [한국어] 그 위는 VGA 와 무관한 주소다. */
		return -EINVAL;

	/* [한국어] u32 로 캐스팅해 switch 한다. case 범위가 정수 상수라 loff_t 그대로는
	 * 비교 폭이 어긋날 수 있는데, 바로 위에서 0xBFFFF 이하임을 확인했으므로
	 * 32비트로 줄여도 값이 보존된다. `case A ... B` 는 GCC 확장인 범위 case 다. */
	switch ((u32)pos) {
	/* [한국어] 레거시 VGA 프레임버퍼 창 128KiB. 실모드 비디오 BIOS 와 텍스트 모드가 쓰는 그 주소다. */
	case 0xa0000 ... 0xbffff:
		/* [한국어] 요청을 창의 끝(0xC0000, 불포함)까지로 자른다. 이 한 줄이 있어 아래 펌프는 창을 넘어가지 않는다. */
		count = min(count, (size_t)(0xc0000 - pos));
		/* [한국어] 창 **전체**를 매핑한다. 시작 주소가 pos 가 아니라 0xA0000 인 점이
		 * 중요하다 - 그래서 아래에서 off 를 따로 계산해 펌프에 넘긴다. 길이 계산
		 * 0xBFFFF - 0xA0000 + 1 은 양 끝을 포함하는 크기 0x20000(128KiB)이다. */
		iomem = ioremap(0xa0000, 0xbffff - 0xa0000 + 1);
		/* [한국어] 창 시작으로부터의 오프셋. 펌프는 io + off 로 실제 주소를 만든다. */
		off = pos - 0xa0000;
		/* [한국어] 중재자에게 요구할 자원은 레거시 메모리 창이다. */
		rsrc = VGA_RSRC_LEGACY_MEM;
		/* [한국어] 메모리 매핑이므로 해제는 iounmap 이다. */
		is_ioport = false;
		/* [한국어] 구간 판정 끝. */
		break;
	/* [한국어] MDA(단색 어댑터) 호환 CRTC 포트 12개. VGA 카드가 단색 모드를 흉내 낼 때 쓰는 주소다. */
	case 0x3b0 ... 0x3bb:
		/* [한국어] 요청을 0x3BC(불포함)까지로 자른다. 0x3BC 부터는 병렬 포트 영역이라 VGA 가 아니다. */
		count = min(count, (size_t)(0x3bc - pos));
		/* [한국어] 포트 구간 전체를 매핑한다. ioport_map 은 I/O 포트 번호를 ioread/iowrite 가 쓸 수 있는 __iomem 토큰으로 바꿔 준다 - x86 에서는 in/out 명령으로 번역된다. 길이 0x3BB - 0x3B0 + 1 = 12. */
		iomem = ioport_map(0x3b0, 0x3bb - 0x3b0 + 1);
		/* [한국어] 구간 시작으로부터의 오프셋. */
		off = pos - 0x3b0;
		/* [한국어] 중재자에게 요구할 자원은 레거시 I/O 다. */
		rsrc = VGA_RSRC_LEGACY_IO;
		/* [한국어] 포트 매핑이므로 해제는 ioport_unmap 이다. */
		is_ioport = true;
		/* [한국어] 구간 판정 끝. */
		break;
	/* [한국어] VGA 본체의 I/O 포트 32개. 순차 제어기, 그래픽 제어기, 속성 제어기, DAC 팔레트가 모두 여기 있다. */
	case 0x3c0 ... 0x3df:
		/* [한국어] 요청을 0x3E0(불포함)까지로 자른다. */
		count = min(count, (size_t)(0x3e0 - pos));
		/* [한국어] 포트 구간 전체를 매핑한다. 길이 0x3DF - 0x3C0 + 1 = 32. */
		iomem = ioport_map(0x3c0, 0x3df - 0x3c0 + 1);
		/* [한국어] 구간 시작으로부터의 오프셋. */
		off = pos - 0x3c0;
		/* [한국어] 레거시 I/O 자원. */
		rsrc = VGA_RSRC_LEGACY_IO;
		/* [한국어] ioport_unmap 으로 풀어야 한다. */
		is_ioport = true;
		/* [한국어] 구간 판정 끝. */
		break;
	/* [한국어] 세 구간 어디에도 속하지 않는 주소. 0x0~0x3AF, 0x3BC~0x3BF, 0x3E0~0x9FFFF 가 여기로 온다. */
	default:
		/* [한국어] 구멍은 0xFF 로 채우지 않고 아예 오류로 거절한다. 이 점이 BAR 경로의 제외 구간과 다르다 - 여기서는 애초에 매핑할 자원이 없기 때문이다. */
		return -EINVAL;
	/* [한국어] 구간 선택 끝. 여기까지 왔다면 iomem, off, rsrc, is_ioport 가 모두 채워져 있다. */
	}

	/* [한국어] 매핑에 실패했는가. NULL 초기화 덕분에 어느 갈래에서 실패했든 한 곳에서 걸린다. */
	if (!iomem)
		/* [한국어] -ENOMEM. 아직 중재자를 잡지 않았으므로 되돌릴 것이 없다. */
		return -ENOMEM;

	/* [한국어] VGA 중재자에게서 그 자원의 소유권을 얻는다. 여러 VGA 카드가 같은
	 * 레거시 주소에 응답하므로, 한 번에 한 카드만 디코딩하도록 중재자가 브리지의
	 * VGA 포워딩 비트를 조정한다. interruptible 판이라 다른 카드가 쓰는 동안
	 * 잠들어 기다리다 시그널을 받으면 -ERESTARTSYS 로 빠진다(실체는
	 * drivers/pci/vgaarb.c:1542 의 vga_get). */
	ret = vga_get_interruptible(vdev->pdev, rsrc);
	/* [한국어] 중재자 획득 실패. */
	if (ret) {
		/* [한국어] 매핑을 되돌린다. 삼항 연산자를 문장으로 쓰는 드문 표기인데, 두 함수가
		 * 모두 void 를 돌려주므로 문법적으로 문제가 없다. is_ioport 를 기억해 둔
		 * 이유가 바로 이 한 줄이다 - ioport_map 으로 만든 토큰을 iounmap 으로 풀면
		 * 안 되기 때문이다. */
		is_ioport ? ioport_unmap(iomem) : iounmap(iomem);
		/* [한국어] 중재자의 오류 코드를 그대로 올린다. */
		return ret;
	/* [한국어] 중재자 실패 처리 끝. */
	}

	/* [한국어] 아래 영어 주석대로, VGA MMIO 는 BAR 가 아닌 레거시 자원이라 probing 이
	 * 허용되는 것을 전제로 하며, 따라서 PCI_COMMAND 의 memory enable 비트와의
	 * 관계를 지금은 신경 쓰지 않는다. 그것이 아래 호출에서 test_mem 을 거짓으로
	 * 넘기는 근거다. */
	/*
	 * VGA MMIO is a legacy, non-BAR resource that hopefully allows
	 * probing, so we don't currently worry about access in relation
	 * to the memory enable bit in the command register.
	 */
	/* [한국어] 펌프를 돌린다. 두 번째 인자 test_mem 이 **거짓**이라 접근마다
	 * memory_lock 을 잡지도, Memory Space Enable 을 확인하지도 않는다. 위 영어
	 * 주석이 그 이유를 밝힌다. */
	done = vfio_pci_core_do_io_rw(vdev, false, iomem, buf, off, count,
				      /* [한국어] x_start 와 x_end 가 둘 다 0 - **제외 구간 없음**이다. off 는 음수가 될
				       * 수 없으므로 do_io_rw 안에서 `off < x_start` 는 항상 거짓이고 `off >= x_end` 는
				       * 항상 참이 되어, 요청 전체가 진짜 접근 경로로 흐른다. 최대 폭은 4바이트로
				       * 제한한다 - 레거시 VGA 는 8바이트 접근을 상정하지 않는다. */
				      0, 0, iswrite, VFIO_PCI_IO_WIDTH_4);

	/* [한국어] 중재자에게 자원을 돌려준다. 펌프의 성공 여부와 무관하게 반드시 짝을 맞춰야 하므로 오류 검사보다 먼저 있다. */
	vga_put(vdev->pdev, rsrc);

	/* [한국어] 매핑도 되돌린다. 위 오류 경로와 같은 삼항 표기다. BAR 와 달리 매핑을 캐시하지 않으므로 요청마다 만들고 부순다. */
	is_ioport ? ioport_unmap(iomem) : iounmap(iomem);

	/* [한국어] 펌프가 성공했는가. */
	if (done >= 0)
		/* [한국어] 처리한 만큼 파일 위치를 전진시킨다. */
		*ppos += done;

	/* [한국어] 바이트 수 또는 음수 errno. */
	return done;
}
/* [한국어] CONFIG_VFIO_PCI_VGA 분기 끝. */
#endif

/* [한국어] vfio_pci_ioeventfd_do_write - 등록해 둔 상수 쓰기를 실제로 수행한다
 *
 * @ioeventfd: 등록 시 채워 둔 서술자. 폭(count), 값(data), 최종 주소(addr),
 *   디바이스(vdev)가 모두 여기 들어 있다.
 * @test_mem: 아래 접근 래퍼에 그대로 넘길 값. 참이면 래퍼가 memory_lock 을
 *   read 로 잡고 메모리 디코드를 확인한다. **호출자가 이미 잠금을 쥐고 있으면
 *   반드시 거짓을 넘겨야 한다** - 그러지 않으면 같은 rwsem 을 두 번 잡는다.
 * @return: 없음. 쓰기 실패(-EIO)는 삼켜지고 사용자에게 보고되지 않는다.
 *   eventfd 를 울린 쪽에게 돌려줄 통로 자체가 없기 때문이다.
 *
 * 왜 필요한가: 등록된 쓰기를 수행하는 코드가 두 문맥(원자적 handler 와
 * 잠들 수 있는 thread)에서 똑같이 필요하다. 그 공통 부분을 여기 모아 두고,
 * 두 호출자가 test_mem 만 다르게 넘긴다.
 *
 * 동작: 등록 시 정한 폭에 따라 네 개의 접근 래퍼 중 하나를 고른다. 주소
 * 계산도 값 변환도 없다 - addr 은 등록 시점에 barmap 기준으로 이미 완성해 둔
 * 최종 __iomem 주소이고, data 는 u64 로 저장돼 있다가 각 래퍼의 인자 타입으로
 * 암묵 변환되며 상위 비트가 잘린다.
 *
 * 실행 컨텍스트: 두 가지다. handler 를 통하면 원자적 문맥일 수 있고(그래서
 * test_mem 을 거짓으로 넘겨 잠금을 다시 잡지 않는다), thread 를 통하면
 * 워크큐의 프로세스 문맥이다.
 * 호출자: vfio_pci_ioeventfd_handler, vfio_pci_ioeventfd_thread.
 * 호출 대상: vfio_pci_core_iowrite8/16/32/64 (이 파일 위쪽의 매크로 생성물).
 *
 * 에러 경로: 없다. 반환값이 void 라 래퍼의 -EIO 는 그대로 버려진다.
 *
 * 호출 체인:
 *   eventfd 쓰기 -> virqfd_wakeup(drivers/vfio/virqfd.c)
 *     -> vfio_pci_ioeventfd_handler 또는 vfio_pci_ioeventfd_thread
 *     -> [vfio_pci_ioeventfd_do_write] -> vfio_pci_core_iowrite32 */
static void vfio_pci_ioeventfd_do_write(struct vfio_pci_ioeventfd *ioeventfd,
					bool test_mem)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 등록 시 정해진 접근 폭으로 분기한다. 값은 1, 2, 4 중 하나다 - 등록 함수가 8 을 거절하기 때문이다. */
	switch (ioeventfd->count) {
	/* [한국어] 1바이트 쓰기. */
	case 1:
		/* [한국어] u8 판 래퍼. data 의 하위 8비트만 나간다. */
		vfio_pci_core_iowrite8(ioeventfd->vdev, test_mem,
				       ioeventfd->data, ioeventfd->addr);
		/* [한국어] 분기 종료. */
		break;
	/* [한국어] 2바이트 쓰기. */
	case 2:
		/* [한국어] u16 판 래퍼. data 의 하위 16비트. */
		vfio_pci_core_iowrite16(ioeventfd->vdev, test_mem,
					ioeventfd->data, ioeventfd->addr);
		/* [한국어] 분기 종료. */
		break;
	/* [한국어] 4바이트 쓰기. 가장 흔한 경우다 - 대부분의 장치 레지스터가 32비트다. */
	case 4:
		/* [한국어] u32 판 래퍼. data 의 하위 32비트. */
		vfio_pci_core_iowrite32(ioeventfd->vdev, test_mem,
					ioeventfd->data, ioeventfd->addr);
		/* [한국어] 분기 종료. */
		break;
	/* [한국어] 8바이트 쓰기.
	 * [상류 코드 관찰] **이 가지에는 도달할 수 없다.** struct vfio_pci_ioeventfd 를
	 * 만드는 유일한 곳인 아래 vfio_pci_ioeventfd 가 `if (count == 8) return -EINVAL`
	 * 로 8바이트 등록을 거절하므로, count 가 8 인 서술자는 존재할 수 없다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	case 8:
		/* [한국어] u64 판 래퍼. */
		vfio_pci_core_iowrite64(ioeventfd->vdev, test_mem,
					ioeventfd->data, ioeventfd->addr);
		/* [한국어] 분기 종료. */
		break;
	/* [한국어] 폭 분기 끝. 어느 case 에도 맞지 않으면 아무 일도 하지 않는다. */
	}
}

/* [한국어] vfio_pci_ioeventfd_handler - eventfd 깨우기의 **원자적 문맥** 1단계 콜백
 *
 * @opaque: 등록 시 vfio_virqfd_enable 에 넘긴 값. 여기서는
 *   struct vfio_pci_ioeventfd 포인터다.
 * @unused: virqfd 의 data 인자. 등록 때 NULL 을 넘겼으므로 언제나 NULL 이며
 *   이름 그대로 쓰이지 않는다.
 * @return: 0 = 처리 완료(2단계 thread 를 부르지 말라),
 *   1 = 지금은 못 하겠으니 thread 로 넘겨 달라.
 *   이 규약은 drivers/vfio/virqfd.c 의 virqfd_wakeup(189~191줄)이 정의한다 -
 *   handler 가 0 이 아닌 값을 돌려줄 때만 워커를 큐잉한다.
 *
 * 왜 필요한가: eventfd 를 울리는 쪽(보통 KVM 의 MMIO 트랩)은 스핀락을 쥔
 * 원자적 문맥일 수 있다. 그런 곳에서 memory_lock 을 down_read 로 잡으면
 * 잠들 수 있어 시스템이 멈춘다. 그래서 이 콜백은 **잠들지 않는 시도**만 하고,
 * 경합하면 잠들 수 있는 문맥으로 일을 미룬다. 그 결과 흔한 경우(아무도
 * memory_lock 을 write 로 잡고 있지 않은 경우)에는 워커를 깨우는 비용조차
 * 들지 않고 그 자리에서 장치 쓰기가 끝난다.
 *
 * 동작 단계: (1) 이 등록이 memory_lock 검사를 요구하면(MMIO BAR),
 * down_read_trylock 을 시도한다. (2) 실패하면 1 을 돌려 thread 에게 넘긴다.
 * (3) 성공했으면 메모리 디코드를 확인하고, 꺼져 있으면 잠금을 놓고 0 을
 * 돌려 **아무 일도 하지 않은 채 끝낸다** - 그 경우 thread 도 부르지 않는다.
 * (4) 쓰기를 수행하고 잠금을 놓고 0 을 돌려준다.
 *
 * 실행 컨텍스트: **원자적 문맥일 수 있다.** 잠들 수 있는 함수를 부르면 안 된다.
 * 호출자: drivers/vfio/virqfd.c 의 virqfd_wakeup(189~191줄).
 * 호출 대상: down_read_trylock, __vfio_pci_memory_enabled(vfio_pci_config.c),
 * vfio_pci_ioeventfd_do_write, 그리고 up_read 를 부른다.
 *
 * 에러 경로: 오류를 보고할 통로가 없다. 메모리 디코드가 꺼진 경우는 조용히
 * 아무 일도 하지 않는 것으로 처리된다.
 *
 * 호출 체인:
 *   KVM 등이 eventfd 에 쓴다 -> virqfd_wakeup(drivers/vfio/virqfd.c)
 *     -> [vfio_pci_ioeventfd_handler] -> vfio_pci_ioeventfd_do_write
 *     (경합 시) -> schedule_work -> vfio_pci_ioeventfd_thread */
static int vfio_pci_ioeventfd_handler(void *opaque, void *unused)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] void 포인터로 받은 등록 서술자를 제 타입으로 되돌린다. virqfd 는 내용을 모르고 그대로 전달만 한다. */
	struct vfio_pci_ioeventfd *ioeventfd = opaque;
	/* [한국어] 잠금을 잡을 대상 디바이스. 서술자가 등록 시점에 물어 둔 것이다. */
	struct vfio_pci_core_device *vdev = ioeventfd->vdev;

	/* [한국어] 이 등록이 MMIO BAR 를 향하는가. I/O 포트라면 잠금도 검사도 필요 없으므로 통째로 건너뛴다. */
	if (ioeventfd->test_mem) {
		/* [한국어] **trylock 이어야 한다.** 여기가 원자적 문맥일 수 있어 잠들 수 없기
		 * 때문이다. 경합 상대는 리셋이나 메모리 디코드 해제 경로가 memory_lock 을
		 * write 로 잡은 경우다(vfio_pci_core.c 의 vfio_pci_zap_and_down_write_memory_lock). */
		if (!down_read_trylock(&vdev->memory_lock))
			/* [한국어] 1 을 돌려주면 virqfd 가 워커를 큐잉해 thread 판이 다시 시도한다. 오른쪽 영어 주석이 그 의도를 그대로 밝힌다. */
			return 1; /* Lock contended, use thread */
		/* [한국어] PCI_COMMAND 의 Memory Space Enable 확인. 꺼진 BAR 에 쓰면 마스터 어보트가 날 수 있다. */
		if (!__vfio_pci_memory_enabled(vdev)) {
			/* [한국어] 0 을 돌려주기 전에 잠금을 놓는다. */
			up_read(&vdev->memory_lock);
			/* [한국어] 0 을 돌려주므로 thread 도 불리지 않는다. 즉 이 이벤트는 조용히 버려진다 - 사용자에게 알릴 통로가 없기 때문이다. */
			return 0;
		/* [한국어] 메모리 디코드 검사 블록 끝. */
		}
	/* [한국어] 잠금 처리 블록 끝. 여기 도달했으면 잠금을 쥐고 있거나 애초에 잠글 필요가 없다. */
	}

	/* [한국어] **두 번째 인자가 거짓**인 것이 핵심이다. 이미 위에서 잠금을 잡고 검사도 마쳤으므로, 래퍼가 같은 rwsem 을 다시 잡으려 하면(잠들 수 있는 down_read 로) 원자적 문맥에서 문제가 된다. */
	vfio_pci_ioeventfd_do_write(ioeventfd, false);

	/* [한국어] 잠갔던 경우에만 놓는다. */
	if (ioeventfd->test_mem)
		/* [한국어] read 잠금 해제. 위 down_read_trylock 과 짝이다. */
		up_read(&vdev->memory_lock);

	/* [한국어] 0 = 처리 완료. thread 를 부르지 말라는 뜻이다. */
	return 0;
}

/* [한국어] vfio_pci_ioeventfd_thread - eventfd 깨우기의 **프로세스 문맥** 2단계 콜백
 *
 * @opaque: 등록 서술자(struct vfio_pci_ioeventfd 포인터).
 * @unused: virqfd 의 data 인자. 등록 때 NULL 을 넘겼다.
 * @return: 없음.
 *
 * 왜 필요한가: 1단계 handler 가 down_read_trylock 에 실패했을 때 그 쓰기를
 * 버리지 않고 마무리하기 위해서다. 여기는 워크큐라 잠들 수 있으므로
 * memory_lock 을 정직하게 기다릴 수 있다.
 *
 * 동작: do_write 에 **서술자의 test_mem 을 그대로** 넘긴다. handler 가
 * 거짓을 넘긴 것과 정확히 반대인데, 여기서는 아직 아무 잠금도 쥐고 있지
 * 않으므로 래퍼가 직접 down_read 로 잡아야 하기 때문이다. 그 한 인자가
 * 두 콜백의 유일한 차이다.
 *
 * 실행 컨텍스트: 워크큐(프로세스 문맥). 잠들 수 있다.
 * 호출자: drivers/vfio/virqfd.c 의 virqfd_inject 워커(278~279줄).
 * 호출 대상: vfio_pci_ioeventfd_do_write.
 *
 * 에러 경로: 없다. 여기서도 실패는 조용히 버려진다.
 *
 * 호출 체인:
 *   vfio_pci_ioeventfd_handler 가 1 을 반환
 *     -> schedule_work -> virqfd_inject(drivers/vfio/virqfd.c:278)
 *     -> [vfio_pci_ioeventfd_thread] -> vfio_pci_ioeventfd_do_write
 *     -> vfio_pci_core_iowrite32 (여기서 down_read 로 memory_lock 확보) */
static void vfio_pci_ioeventfd_thread(void *opaque, void *unused)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] void 포인터를 제 타입으로 되돌린다. */
	struct vfio_pci_ioeventfd *ioeventfd = opaque;

	/* [한국어] handler 판과 달리 서술자의 test_mem 을 그대로 넘긴다. 잠금을 아직 잡지 않았으므로 래퍼가 잡아야 한다. */
	vfio_pci_ioeventfd_do_write(ioeventfd, ioeventfd->test_mem);
}

/* [한국어] vfio_pci_ioeventfd - BAR 오프셋에 대한 ioeventfd 를 등록하거나 해제한다
 *
 * @vdev: 대상 vfio-pci 디바이스. 등록 목록과 그 뮤텍스가 여기 있다.
 * @offset: 사용자가 준 파일 오프셋. 상위 비트가 region 번호, 하위 40비트가
 *   BAR 안 위치다.
 * @data: eventfd 가 울릴 때마다 그 자리에 쓸 상수 값.
 * @count: 접근 폭(바이트). 1, 2, 4 만 허용된다. 코어의
 *   vfio_pci_ioctl_ioeventfd(vfio_pci_core.c:4511)가 hweight8 로 2의 거듭제곱임을
 *   이미 확인했으므로 여기 들어오는 값은 1/2/4/8 중 하나다.
 * @fd: 연결할 eventfd 번호. -1 이면 같은 (오프셋, BAR, 값, 폭) 조합의 기존
 *   등록을 **해제**하라는 뜻이다.
 * @return: 0 = 성공, -EINVAL = 인자가 규칙을 어김, -EEXIST = 같은 조합이 이미
 *   등록됨, -ENODEV = 해제하려는 등록이 없음, -ENOSPC = 개수 상한 초과,
 *   -ENOMEM = 할당 실패, 그 밖에 vfio_virqfd_enable 이 올린 값.
 *
 * 왜 필요한가: VMM 이 게스트의 특정 레지스터 쓰기를 트랩할 때, 그 쓰기를
 * 처리하러 매번 유저스페이스로 나갔다 오면 비싸다. 값이 늘 같은 상수라면
 * "이 eventfd 가 울리면 이 자리에 이 값을 써라" 를 커널에 미리 등록해 두고
 * KVM 이 그 eventfd 를 울리게 하면, 나가는 일 없이 커널 안에서 끝난다.
 * virtio 의 kick 레지스터나 NVMe 의 도어벨처럼 값이 정해진 쓰기가 대표적인
 * 용례다.
 *
 * 동작 단계:
 *  (1) BAR0~5 만 허용한다. ROM 과 VGA 와 config 는 대상이 아니다.
 *  (2) 접근이 BAR 길이를 벗어나지 않는지 본다.
 *  (3) **MSI-X 테이블과 조금이라도 겹치면 거절한다.** 이것이 없으면 read/write
 *      경로에서 막아 둔 MSI-X 테이블 쓰기를 ioeventfd 로 우회할 수 있다.
 *  (4) 8바이트 폭을 거절한다.
 *  (5) BAR 매핑을 확보한다(주소를 미리 완성해 두기 위해서다).
 *  (6) 뮤텍스를 잡고 목록에서 같은 조합을 찾는다. 있으면 fd 가 -1 일 때만
 *      해제하고, 아니면 -EEXIST.
 *  (7) 없으면 새로 만들어 등록한다. 개수 상한과 할당 실패를 검사한 뒤
 *      virqfd 에 handler/thread 한 쌍을 걸고 목록에 넣는다.
 *
 * **read/write 와의 결정적 차이**: 제외 구간을 걸치는 접근을 read/write 는
 * 세 토막으로 나눠 처리하지만, ioeventfd 는 겹치기만 해도 통째로 거절한다.
 * 쓰기를 "조용히 버리는" 동작을 이 경로에서는 흉내 낼 수 없기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 문맥(VFIO_DEVICE_IOEVENTFD ioctl 안). 잠들 수 있다.
 * 잠금 순서는 ioeventfds_lock 뮤텍스 하나뿐이며, 그 안에서 vfio_virqfd_enable 을
 * 부른다.
 * 호출자: vfio_pci_core.c 의 vfio_pci_ioctl_ioeventfd(4511줄). 그 함수가 폭이
 * 2의 거듭제곱인지, fd 가 -1 이상인지를 먼저 검사한다.
 * 호출 대상: pci_resource_len, vfio_pci_core_setup_barmap,
 * vfio_virqfd_disable(drivers/vfio/virqfd.c:435),
 * vfio_virqfd_enable(drivers/vfio/virqfd.c:337), kzalloc_obj, kfree,
 * mutex_lock/mutex_unlock, list_add/list_del.
 *
 * 에러 경로: (1)~(4)는 잠금 전이라 그냥 반환한다. (5)의 실패도 마찬가지다.
 * (6) 이후는 모두 out_unlock 로 모여 뮤텍스를 놓고 나간다. 새로 만든 서술자가
 * virqfd 등록에 실패하면 그 자리에서 kfree 하며, 목록에는 아직 넣기 전이라
 * 남는 흔적이 없다.
 *
 * 호출 체인:
 *   사용자의 VFIO_DEVICE_IOEVENTFD ioctl
 *     -> vfio_pci_core_ioctl -> vfio_pci_ioctl_ioeventfd(vfio_pci_core.c:4511)
 *     -> [vfio_pci_ioeventfd] -> vfio_pci_core_setup_barmap
 *     -> vfio_virqfd_enable(drivers/vfio/virqfd.c:337) */
int vfio_pci_ioeventfd(struct vfio_pci_core_device *vdev, loff_t offset,
		       uint64_t data, int count, int fd)
/* [한국어] 함수 본문 시작. */
{
	/* [한국어] 실제 PCI 함수. pci_resource_len 과 자원 플래그 조회에 쓰인다. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 파일 오프셋에서 BAR 안 위치만 남긴다. 상위 비트의 region 번호를 지운다. */
	loff_t pos = offset & VFIO_PCI_OFFSET_MASK;
	/* [한국어] ret 는 반환값 누적 자리, bar 는 오프셋 상위 비트에서 뽑은 region 번호다. 두 변수를 한 줄에 선언한 것은 원본 그대로의 표기다. */
	int ret, bar = VFIO_PCI_OFFSET_TO_INDEX(offset);
	/* [한국어] 목록 순회와 새 등록 양쪽에서 쓰이는 서술자 포인터. 순회에서는 기존 항목을 가리키고, 그 뒤에는 새로 할당한 것을 가리킨다. */
	struct vfio_pci_ioeventfd *ioeventfd;

	/* [한국어] 아래 영어 주석대로 ioeventfd 는 BAR 만 대상으로 한다. */
	/* Only support ioeventfds into BARs */
	/* [한국어] VFIO_PCI_BAR5_REGION_INDEX 는 5 다(include/uapi/linux/vfio.h:620~639 의 region 인덱스 열거형). 즉 0~5 만 통과하고 ROM(6), config(7), VGA(8) 이상은 거절한다. bar 는 음이 아닌 파일 오프셋에서 왔으므로 음수가 될 수 없어 아래쪽 경계 검사가 없다. */
	if (bar > VFIO_PCI_BAR5_REGION_INDEX)
		/* [한국어] BAR 가 아닌 region 은 -EINVAL. */
		return -EINVAL;

	/* [한국어] 접근 끝이 BAR 길이를 넘는가. pos 는 loff_t, count 는 int, 자원 길이는
	 * 부호 없는 resource_size_t 라 비교가 부호 없는 쪽으로 승격되지만 pos 가
	 * 음수일 수 없어 문제되지 않는다. 존재하지 않는 BAR 는 길이가 0 이므로 이
	 * 검사 하나로 함께 걸러진다. */
	if (pos + count > pci_resource_len(pdev, bar))
		/* [한국어] 범위 밖이면 -EINVAL. */
		return -EINVAL;

	/* [한국어] 아래 영어 주석이 이 검사의 목적을 그대로 밝힌다 - ioeventfd 로 MSI-X 테이블 쓰기를 우회하는 것을 막는다. */
	/* Disallow ioeventfds working around MSI-X table writes */
	/* [한국어] **이 파일에서 두 번째로 중요한 검사다.** MSI-X 테이블이 든 BAR 인가.
	 * msix_bar 가 0xFF 인 장치(MSI-X 없음)는 여기서 걸리지 않는다. */
	if (bar == vdev->msix_bar &&
	    /* [한국어] 겹치지 않음의 조건은 둘 중 하나 - 접근이 테이블 시작 전에 완전히
	     * 끝나거나(pos + count <= msix_offset), 테이블 끝 이후에서 시작하거나
	     * (pos >= msix_offset + msix_size). 그 논리합을 부정했으므로 **조금이라도
	     * 겹치면 참**이 되어 아래에서 거절된다. */
	    !(pos + count <= vdev->msix_offset ||
	      /* [한국어] 테이블의 끝. read/write 경로의 x_end 와 같은 값이다. */
	      pos >= vdev->msix_offset + vdev->msix_size))
		/* [한국어] -EINVAL. read/write 는 걸치는 접근을 세 토막으로 나누어 겹치는 부분만
		 * 버리지만, ioeventfd 에는 그런 부분 처리가 없으므로 통째로 거절한다.
		 * 등록을 허용해 두고 실행 시점에 버리는 방식이 아니라 **등록 자체를 막는**
		 * 것이 이 방어의 요점이다. */
		return -EINVAL;

	/* [한국어] 8바이트 폭은 지원하지 않는다. 위쪽 vfio_pci_ioeventfd_do_write 의
	 * `case 8` 이 도달 불가능한 코드가 되는 원인이 바로 이 두 줄이다. */
	if (count == 8)
		/* [한국어] -EINVAL. */
		return -EINVAL;

	/* [한국어] BAR 매핑을 확보한다. 아직 매핑되지 않았다면 여기서 예약과 ioremap 이
	 * 일어난다. 등록 시점에 매핑을 확정해 두어야 아래에서 최종 주소를 미리
	 * 계산할 수 있고, 그래야 콜백이 원자적 문맥에서도 주소 계산 없이 곧장 쓸 수
	 * 있다. */
	ret = vfio_pci_core_setup_barmap(vdev, bar);
	/* [한국어] 예약이나 매핑 실패. */
	if (ret)
		/* [한국어] 그대로 올린다. 아직 뮤텍스를 잡지 않았다. */
		return ret;

	/* [한국어] 등록 목록을 보호하는 뮤텍스. 여기서부터 out_unlock 까지가 임계 구역이다.
	 * 콜백 쪽(handler/thread)은 이 뮤텍스를 잡지 않는다 - 콜백은 목록을 훑지 않고
	 * 자기 서술자만 보기 때문이며, 서술자의 수명은 vfio_virqfd_disable 이
	 * 보장한다. */
	mutex_lock(&vdev->ioeventfds_lock);

	/* [한국어] 이미 같은 조합이 등록돼 있는지 훑는다. 목록은 vdev 하나당 하나이고 상한이 1000 이라 선형 탐색으로 충분하다. */
	list_for_each_entry(ioeventfd, &vdev->ioeventfds_list, next) {
		/* [한국어] 같은 조합의 판정 기준 - 위치와 BAR 가 같고, */
		if (ioeventfd->pos == pos && ioeventfd->bar == bar &&
		    /* [한국어] 값과 폭까지 같아야 한다. **fd 는 비교하지 않는다.** 그래서 해제할 때 원래 어떤 fd 였는지 몰라도 된다. */
		    ioeventfd->data == data && ioeventfd->count == count) {
			/* [한국어] fd 가 -1 이면 해제 요청이다. */
			if (fd == -1) {
				/* [한국어] virqfd 등록을 먼저 끊는다. 이 함수가 워커가 끝날 때까지 기다려 주므로,
				 * 반환한 뒤에는 콜백이 이 서술자를 더 이상 만지지 않는다는 것이 보장된다.
				 * 아래 kfree 가 안전한 이유가 바로 이것이다(drivers/vfio/virqfd.c:435). */
				vfio_virqfd_disable(&ioeventfd->virqfd);
				/* [한국어] 목록에서 떼어 낸다. */
				list_del(&ioeventfd->next);
				/* [한국어] 등록 개수를 줄인다. 이 값이 상한 검사의 기준이다. */
				vdev->ioeventfds_nr--;
				/* [한국어] 서술자를 해제한다. virqfd 를 먼저 끊었으므로 use-after-free 가 생기지 않는다. */
				kfree(ioeventfd);
				/* [한국어] 해제 성공. */
				ret = 0;
			/* [한국어] fd 가 -1 이 아닌데 같은 조합이 이미 있는 경우. */
			} else
				/* [한국어] 덮어쓰기를 허용하지 않는다. 사용자는 먼저 -1 로 지우고 다시 등록해야 한다. */
				ret = -EEXIST;

			/* [한국어] 두 경우 모두 목록 순회를 그만두고 뮤텍스 해제 지점으로 간다. */
			goto out_unlock;
		/* [한국어] 조합 일치 블록 끝. */
		}
	/* [한국어] 목록 순회 끝. 여기 도달했으면 같은 조합이 없다는 뜻이다. */
	}

	/* [한국어] 같은 조합이 없는데 해제 요청(fd 가 음수)이었는가. */
	if (fd < 0) {
		/* [한국어] 지울 것이 없다. -EEXIST 의 반대편 오류다. */
		ret = -ENODEV;
		/* [한국어] 뮤텍스 해제 지점으로. */
		goto out_unlock;
	/* [한국어] 해제 요청 처리 끝. */
	}

	/* [한국어] 등록 개수 상한. VFIO_PCI_IOEVENTFD_MAX 는 vfio_pci_priv.h:12 에서 1000 으로
	 * 정해져 있고, 그 줄의 영어 주석이 "임의로 정한 상한" 이라고 밝힌다. 사용자가
	 * 등록을 무한히 늘려 커널 메모리를 소진하는 것을 막는다. */
	if (vdev->ioeventfds_nr >= VFIO_PCI_IOEVENTFD_MAX) {
		/* [한국어] -ENOSPC. */
		ret = -ENOSPC;
		/* [한국어] 뮤텍스 해제 지점으로. */
		goto out_unlock;
	/* [한국어] 상한 검사 끝. */
	}

	/* [한국어] 서술자를 0 초기화해 할당한다. GFP_KERNEL_ACCOUNT 는 이 할당을 호출한
	 * 프로세스의 memcg 예산에 청구하라는 뜻이다 - 사용자가 등록을 잔뜩 만들어도
	 * 그 비용이 자기 cgroup 에 잡히므로 시스템 전체를 굶기지 못한다. */
	ioeventfd = kzalloc_obj(*ioeventfd, GFP_KERNEL_ACCOUNT);
	/* [한국어] 할당 실패. */
	if (!ioeventfd) {
		/* [한국어] -ENOMEM. */
		ret = -ENOMEM;
		/* [한국어] 뮤텍스 해제 지점으로. 아직 목록에 넣지 않았으므로 정리할 것이 없다. */
		goto out_unlock;
	/* [한국어] 할당 검사 끝. */
	}

	/* [한국어] 콜백이 잠금을 잡을 때 쓸 디바이스를 물어 둔다. */
	ioeventfd->vdev = vdev;
	/* [한국어] **최종 __iomem 주소를 지금 완성해 둔다.** barmap 은 위에서 확보를
	 * 보장했다. 콜백 시점에는 이 주소를 그대로 쓰기만 하면 되므로 원자적 문맥에서
	 * 주소 계산이나 조회가 필요 없다. */
	ioeventfd->addr = vdev->barmap[bar] + pos;
	/* [한국어] 울릴 때마다 쓸 상수 값. u64 로 보관했다가 폭에 맞는 래퍼가 잘라 쓴다. */
	ioeventfd->data = data;
	/* [한국어] BAR 안 위치. 중복 검사의 열쇠 중 하나다. */
	ioeventfd->pos = pos;
	/* [한국어] BAR 번호. 역시 중복 검사의 열쇠다. */
	ioeventfd->bar = bar;
	/* [한국어] 접근 폭. do_write 의 switch 가 이 값으로 갈린다. */
	ioeventfd->count = count;
	/* [한국어] 이 BAR 가 메모리 자원이면 콜백이 memory_lock 과 메모리 디코드를 검사해야
	 * 한다. 비트마스크를 bool 에 대입하므로 IORESOURCE_MEM 비트가 서 있으면 참이
	 * 된다. I/O 포트 BAR 이면 거짓이 되어 콜백이 잠금 없이 곧장 쓴다. */
	ioeventfd->test_mem = vdev->pdev->resource[bar].flags & IORESOURCE_MEM;

	/* [한국어] virqfd 에 등록한다. 첫 인자가 콜백에 그대로 전달될 opaque 이고,
	 * 둘째가 원자적 문맥 1단계 handler 다. */
	ret = vfio_virqfd_enable(ioeventfd, vfio_pci_ioeventfd_handler,
				 /* [한국어] 셋째가 프로세스 문맥 2단계 thread, 넷째는 콜백의 data 인자인데 여기서는 필요 없어 NULL 이다. */
				 vfio_pci_ioeventfd_thread, NULL,
				 /* [한국어] 다섯째는 virqfd 핸들을 저장할 자리(해제할 때 이 주소가 필요하다), 여섯째가 사용자가 준 eventfd 번호다. */
				 &ioeventfd->virqfd, fd);
	/* [한국어] 등록 실패 - fd 가 유효한 eventfd 가 아니거나 내부 할당이 실패했다. */
	if (ret) {
		/* [한국어] 아직 목록에 넣지 않았으므로 서술자만 해제하면 된다. */
		kfree(ioeventfd);
		/* [한국어] 뮤텍스 해제 지점으로. */
		goto out_unlock;
	/* [한국어] virqfd 등록 실패 처리 끝. */
	}

	/* [한국어] 이제 목록에 넣는다. **virqfd 등록이 성공한 뒤에 넣는 순서가 중요하다** -
	 * 반대로 하면 등록 실패 시 목록에서 다시 빼야 한다. list_add 는 머리에
	 * 넣으므로 최근 등록이 먼저 탐색된다. */
	list_add(&ioeventfd->next, &vdev->ioeventfds_list);
	/* [한국어] 개수를 늘린다. 상한 검사의 기준값이다. */
	vdev->ioeventfds_nr++;

/* [한국어] 뮤텍스 해제 지점. 성공과 다섯 가지 실패가 모두 여기로 모인다. */
out_unlock:
	/* [한국어] 임계 구역 끝. */
	mutex_unlock(&vdev->ioeventfds_lock);

	/* [한국어] 0 또는 위에서 정한 오류 코드. ret 는 setup_barmap 이나 vfio_virqfd_enable 이 반드시 한 번은 대입하므로 초기화되지 않은 채 읽히는 경로가 없다. */
	return ret;
}
