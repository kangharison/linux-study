// SPDX-License-Identifier: GPL-2.0-only
/*
 * VFIO PCI interrupt handling
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * Derived from original vfio:
 * Copyright 2010 Cisco Systems, Inc.  All rights reserved.
 * Author: Tom Lyon, pugs@cisco.com
 */

/* [한국어] VFIO PCI 인터럽트 설정 계층 — INTx / MSI / MSI-X 를 사용자 공간의 eventfd 로
 * 중계하고, 사용자 소유 디바이스가 호스트를 인터럽트 폭풍으로 몰아넣지 못하게
 * 막는 곳 (drivers/vfio/pci/vfio_pci_intrs.c)
 * 
 * 이 블록에서 다른 파일을 가리킬 때만 줄 번호를 적었다. 이 파일 자신은 주석이
 * 붙으며 줄 번호가 계속 밀리므로 함수 이름으로만 가리킨다. 같은 디렉터리의
 * vfio_pci_core.c 도 지금 함께 주석 작업 중이라 함수 이름으로만 가리킨다.
 * 
 * === 파일의 역할 ===
 * 사용자 공간 드라이버는 인터럽트를 직접 받을 수 없다. 인터럽트는 CPU 의
 * 특권 자원이고, 인터럽트 벡터와 IRQ 번호는 호스트 커널이 소유한다. 그래서
 * VFIO 는 "커널이 인터럽트를 받아 eventfd 하나를 신호한다" 는 중계 구조를
 * 쓴다. 이 파일이 그 중계의 전부다 — IRQ 를 등록하고, 하드웨어 인터럽트
 * 핸들러에서 eventfd 를 때리고, 사용자가 ioctl 로 요구한 마스킹을 실제
 * 하드웨어나 IRQ 칩 조작으로 번역한다.
 * 
 * 세 가지 인터럽트 방식을 모두 다루는데 성격이 크게 다르다.
 *  - INTx: 레벨 트리거 공유 라인이다. 사용자가 인터럽트를 처리하기 전까지
 *    라인이 계속 어서션 상태로 남으므로, **핸들러가 곧바로 마스크하지 않으면
 *    호스트가 같은 인터럽트를 무한히 다시 받는다.** 이 파일에서 마스킹 코드가
 *    가장 복잡한 이유가 그것이다. 사용자가 eventfd 를 읽고 처리한 뒤 다시
 *    unmask 를 요청해야 다음 인터럽트가 들어온다.
 *  - MSI: 메시지 시그널 방식이라 에지 트리거처럼 동작한다. 라인이 계속 서
 *    있지 않으므로 마스킹이 필요 없고, 핸들러는 eventfd 를 때리기만 한다.
 *  - MSI-X: MSI 와 같지만 벡터마다 독립된 주소/데이터를 가지며, 이 트리에서는
 *    동적 벡터 할당까지 지원한다.
 * 
 * 또 하나의 축이 있다. INTx/MSI/MSI-X 는 **동시에 켤 수 없다**. vdev->irq_type
 * 하나가 현재 모드를 담고, 모드 전환은 반드시 "전부 끄고 새로 켜기" 로만
 * 이뤄진다. is_irq_none 검사가 그 배타성을 지킨다.
 * 
 * 마지막으로 이 파일은 인터럽트가 아닌 두 개의 알림 채널도 다룬다.
 * VFIO_PCI_ERR_IRQ_INDEX 는 AER 오류를, VFIO_PCI_REQ_IRQ_INDEX 는 "호스트가
 * 이 디바이스를 돌려받고 싶다" 는 요청을 사용자에게 알린다. 둘 다 하드웨어
 * 인터럽트가 아니라 커널 내부 사건이며, RCU 로 보호되는 eventfd 슬롯 하나씩만
 * 쓴다.
 * 
 * === 전체 아키텍처에서의 위치 ===
 * 설정 경로(사용자 → 커널)와 전달 경로(하드웨어 → 사용자)가 정반대 방향으로
 * 흐른다.
 * 
 *   [설정 경로 — 전부 프로세스 문맥, igate mutex 아래]
 *     사용자 ioctl(VFIO_DEVICE_SET_IRQS)
 *       -> drivers/vfio/vfio_main.c 의 vfio_device_fops_unl_ioctl
 *       -> vfio_pci_core.c 의 vfio_pci_core_ioctl
 *          -> vfio_pci_ioctl_set_irqs — 인자를 검증하고 **igate 를 잡은 뒤**
 *             이 파일을 부른다
 *       -> [이 파일] vfio_pci_set_irqs_ioctl — index 와 action 으로 8갈래 디스패치
 *       -> vfio_intx_enable / vfio_msi_enable / vfio_msi_set_vector_signal 등
 *       -> request_irq, drivers/pci/msi/api.c:519 의 pci_alloc_irq_vectors,
 *          drivers/pci/msi/api.c:317 의 pci_msix_alloc_irq_at
 * 
 *   [전달 경로 — 하드 인터럽트 문맥에서 시작]
 *     디바이스가 인터럽트를 올림
 *       -> 커널 IRQ 코어 -> [이 파일] vfio_intx_handler 또는 vfio_msihandler
 *          (둘 다 threadfn 없이 등록돼 **하드 인터럽트 문맥**에서 돈다)
 *       -> INTx 면 irqlock 아래에서 먼저 마스크하고
 *       -> eventfd_signal — 사용자 공간의 poll/read 를 깨운다
 *       -> 사용자 드라이버가 디바이스를 처리하고, INTx 면 다시 ioctl 로 unmask
 * 
 *   [보조 경로 — 사용자가 unmask 를 eventfd 로 등록한 경우]
 *     사용자가 unmask eventfd 에 write
 *       -> eventfd wait queue wakeup(atomic 문맥)
 *       -> drivers/vfio/virqfd.c:179 의 virqfd_wakeup
 *       -> handler 콜백 = [이 파일] vfio_pci_intx_unmask_handler (atomic)
 *       -> 0 이 아니면 워커 큐잉 -> thread 콜백 = [이 파일]
 *          vfio_send_intx_eventfd (프로세스 문맥)
 *     이 두 단계 분리는 virqfd 가 강제하는 것이다. atomic 단계에서
 *     eventfd_signal 을 부르면 같은 eventfd 의 wait queue 락에서 자기 자신과
 *     교착하기 때문에, 신호가 필요하면 양수를 반환해 워커로 넘긴다.
 * 
 * === 타 모듈과의 연결 ===
 *  - drivers/vfio/pci/vfio_pci_priv.h
 *      : 이 파일이 노출하는 vfio_pci_intx_mask, vfio_pci_intx_unmask,
 *        vfio_pci_set_irqs_ioctl 의 선언처. 이 파일이 부르는
 *        vfio_pci_eventfd_replace_locked, vfio_pci_memory_lock_and_enable,
 *        vfio_pci_memory_unlock_and_restore 의 선언도 여기.
 *  - include/linux/vfio_pci_core.h
 *      : struct vfio_pci_core_device 정의(98줄). 이 파일이 쓰는 필드는
 *        irqlock(107줄), igate(108줄), ctx xarray(109줄), irq_type(110줄),
 *        msi_qmax(113줄), has_dyn_msix / pci_2_3 / virq_disabled / nointx
 *        (118~126줄의 비트필드), err_trigger 와 req_trigger(133~134줄).
 *        struct vfio_pci_eventfd 도 여기(33~36줄) — eventfd_ctx 하나와
 *        rcu_head 하나로 된 RCU 교체용 껍데기다.
 *  - drivers/vfio/pci/vfio_pci_core.c  (함께 주석 작업 중)
 *      : 유일한 ioctl 호출자이자 igate 를 잡아 주는 곳
 *        (vfio_pci_ioctl_set_irqs). vfio_pci_core_disable 이 디바이스를 닫을 때
 *        vfio_pci_set_irqs_ioctl 을 DATA_NONE + TRIGGER 로 불러 현재 모드를
 *        통째로 끈다. 런타임 PM 진입/복귀에서 vfio_pci_intx_mask 와
 *        vfio_pci_intx_unmask 를 부르고, vfio_pci_eventfd_replace_locked 의
 *        구현도 그 파일에 있다.
 *  - drivers/vfio/pci/vfio_pci_config.c  (같은 작업으로 함께 주석)
 *      : 사용자가 COMMAND 레지스터의 INTx Disable 비트를 쓰면 그 파일의
 *        vfio_basic_config_write 가 vdev->virq_disabled 를 갱신하고 이 파일의
 *        vfio_pci_intx_mask / vfio_pci_intx_unmask 를 부른다. 반대로 이 파일은
 *        그 필드를 읽어 eventfd 전달 여부와 초기 마스크 상태를 정한다.
 *        그 파일의 vfio_msi_config_write 는 사용자가 config 로 MSI 를 켜지
 *        못하게 막으며, 그 근거가 이 파일의 vdev->irq_type 이다.
 *  - drivers/vfio/virqfd.c  (이미 주석 완료)
 *      : vfio_virqfd_enable(337줄), vfio_virqfd_disable(435줄),
 *        vfio_virqfd_flush_thread(474줄). 사용자가 "마스크 해제를 eventfd 로
 *        하고 싶다" 고 할 때 쓰는 2단계 콜백 기구. atomic handler 와 process
 *        thread 의 분리 규약은 그 파일 179줄 위의 주석에 상세히 적혀 있다.
 *  - drivers/pci/msi/  (이미 주석 완료)
 *      : pci_alloc_irq_vectors(api.c:519), pci_free_irq_vectors(api.c:857),
 *        pci_irq_vector(api.c:705), pci_msix_alloc_irq_at(api.c:317),
 *        pci_write_msi_msg(msi.c:812). 이 파일은 벡터 할당과 메시지 기록을
 *        전부 그 계층에 맡기고, 자기는 IRQ 번호와 eventfd 를 잇는 일만 한다.
 *        MSI/MSI-X 비활성화 시 DisINTx 가 pci_intx_for_msi(msi.c:848)에 의해
 *        지워진다는 사실이 vfio_msi_disable 의 마지막 보정 근거다.
 *  - drivers/pci/pci.c 와 drivers/pci/irq.c  (이미 주석 완료)
 *      : pci_intx(pci.c:8201) — COMMAND 의 INTx Disable 비트 조작.
 *        pci_check_and_mask_intx(irq.c:696)와
 *        pci_check_and_unmask_intx(irq.c:734) — 공유 라인에서 "정말 이
 *        디바이스가 인터럽트를 올렸는지" 확인하며 원자적으로 마스크/해제하는
 *        PCI 2.3 전용 연산.
 * 
 * === 주요 함수/구조체 요약 ===
 *  - struct vfio_pci_irq_ctx    : 벡터 하나의 상태. eventfd, 두 개의 virqfd
 *                                 슬롯, IRQ 이름, 마스크 여부, irq bypass
 *                                 producer. vdev->ctx xarray 에 인덱스별로 담긴다.
 *  - is_intx / irq_is / is_irq_none
 *                               : 현재 모드 판정 3종. 모드 배타성의 문지기다.
 *  - __vfio_pci_intx_mask / _unmask_handler
 *                               : irqlock 아래에서 INTx 물리 마스킹을 조작하는
 *                                 핵심 두 함수. 세 방향(인터럽트, ioctl,
 *                                 config 공간)에서 불린다.
 *  - vfio_intx_handler          : 하드 인터럽트 핸들러. 먼저 마스크하고 나서
 *                                 eventfd 를 때린다.
 *  - vfio_intx_enable / _disable / _set_signal
 *                               : INTx 수명 관리 3종.
 *  - vfio_msihandler            : MSI/MSI-X 하드 인터럽트 핸들러. eventfd 만 때린다.
 *  - vfio_msi_enable / _disable : 벡터 묶음 할당과 해제.
 *  - vfio_msi_set_vector_signal : 벡터 하나와 eventfd 를 잇거나 끊는다. 이
 *                                 파일에서 가장 긴 함수이며 IRQ 등록,
 *                                 메시지 재기록, irq bypass 등록을 모두 한다.
 *  - vfio_pci_set_irqs_ioctl    : ioctl 디스패처. index x action 조합으로
 *                                 핸들러를 고른다.
 * 
 * === 실행 문맥과 락 지도 ===
 * 이 파일을 읽을 때 가장 헷갈리는 것이 "이 코드가 어디서 도는가" 다. 네 가지
 * 문맥이 섞여 있다.
 * 
 *  (1) 하드 인터럽트 문맥 — vfio_intx_handler, vfio_msihandler.
 *      request_irq 에 threadfn 을 주지 않았으므로 커널 IRQ 코어가 인터럽트를
 *      받은 CPU 에서 직접 부른다. 잠들 수 없고, mutex 를 잡을 수 없다.
 *      그래서 여기서 쓰는 락은 spin_lock_irqsave(irqlock) 뿐이다.
 *  (2) eventfd wait queue 문맥(atomic) — vfio_pci_intx_unmask_handler 가
 *      virqfd 의 handler 로 불릴 때. eventfd 의 wait queue 락을 쥔 상태라
 *      잠들 수 없고, 특히 **같은 eventfd 를 signal 하면 교착한다.** 그래서
 *      이 함수는 신호가 필요하면 1 을 반환만 하고 실제 signal 은 워커로 넘긴다.
 *  (3) 워커(프로세스 문맥) — vfio_send_intx_eventfd 가 virqfd 의 thread 로
 *      불릴 때. schedule_work 로 큐잉된 뒤 시스템 워크큐에서 돈다.
 *  (4) ioctl(프로세스 문맥) — 나머지 전부.
 * 
 * 락은 두 개다.
 *  - vdev->igate (mutex)
 *      **모드 전환을 덮는 락이다.** vdev->irq_type 을 읽고 쓰는 모든 코드가 이
 *      락 아래에 있어야 하며, ioctl 경로에서는 vfio_pci_core.c 의
 *      vfio_pci_ioctl_set_irqs 가 이 파일을 부르기 전에 잡아 준다. 그래서 이
 *      파일의 vfio_pci_set_ 계열 함수들은 스스로 잡지 않는다. 반대로
 *      vfio_pci_intx_mask 와 vfio_pci_intx_unmask 는 config 공간 경로와 런타임
 *      PM 경로에서 igate 없이 불려 오므로 자기가 직접 잡는다. 그 짝인
 *      __vfio_pci_intx_mask 와 __vfio_pci_intx_unmask 는 lockdep_assert_held 로
 *      호출자가 이미 잡았음을 확인한다.
 *      INTx 를 끄고 MSI 를 켜는 것 같은 전환이 request_irq/free_irq 와 겹치지
 *      않는 이유가 바로 이 락이다. vfio_intx_enable 의 상류 주석도 "IRQ 핸들러가
 *      등록돼 있는 동안 irq_type 이 안정적이어야 하므로 request_irq 전에
 *      설정한다" 고 그 규약을 명시한다.
 *  - vdev->irqlock (spinlock, irqsave)
 *      **하드 인터럽트 핸들러와 공유하는 상태를 덮는 락이다.** 구체적으로는
 *      ctx->masked 와 물리 마스킹 조작(pci_intx, disable_irq_nosync 등)이
 *      대상이다. 하드 인터럽트 문맥에서 잡으므로 반드시 irqsave 판을 쓴다.
 *      이 락 안에서는 잠들 수 없어 eventfd_signal 도 락 밖에서 한다.
 * 
 *  메모리 배리어 성격의 장치 하나:
 *  - ctx->trigger 는 WRITE_ONCE 로 쓰고 READ_ONCE 로 읽는다. 하드 인터럽트
 *    핸들러가 언제든 이 포인터를 읽을 수 있는데 ioctl 이 그것을 교체하기
 *    때문이다. 교체 뒤에는 synchronize_irq 로 진행 중인 핸들러가 끝나기를
 *    기다린 뒤에야 옛 eventfd 의 참조를 놓는다(vfio_intx_set_signal). */
/* [한국어] dev_info 와 struct device. irq bypass producer 등록이 실패했을 때
 * 남기는 로그 한 줄이 이 헤더를 쓴다. */
#include <linux/device.h>
/* [한국어] request_irq, free_irq, synchronize_irq, disable_irq_nosync, enable_irq,
 * irqreturn_t 와 IRQ_HANDLED / IRQ_NONE, IRQF_SHARED 와 IRQF_NO_AUTOEN.
 * 이 파일의 핵심 헤더다 — 커널 IRQ 코어와 주고받는 모든 것이 여기서 온다.
 * sparse checkout 이라 헤더 원문은 이 트리에 없다. */
#include <linux/interrupt.h>
/* [한국어] struct eventfd_ctx, eventfd_ctx_fdget, eventfd_ctx_put, eventfd_signal.
 * 사용자 공간으로 인터럽트를 중계하는 유일한 수단이 eventfd 라, 이 파일의
 * 목적 자체가 이 헤더에 달려 있다. */
#include <linux/eventfd.h>
/* [한국어] struct msi_msg, struct msi_map, get_cached_msi_msg.
 * MSI-X 벡터를 다시 살릴 때 커널이 캐시해 둔 메시지를 꺼내 쓰는 데 필요하다. */
#include <linux/msi.h>
/* [한국어] struct pci_dev, pci_name, pci_intx, pci_check_and_mask_intx,
 * pci_check_and_unmask_intx, pci_alloc_irq_vectors, pci_free_irq_vectors,
 * pci_irq_vector, pci_msix_alloc_irq_at, pci_write_msi_msg, IRQ_NOTCONNECTED.
 * 인터럽트 배선의 PCI 쪽 절반이 전부 여기서 온다. */
#include <linux/pci.h>
/* [한국어] 파일 디스크립터 계층. 사용자가 넘긴 fd 를 eventfd 컨텍스트로 바꾸는
 * 경로가 이 헤더의 정의에 기댄다. */
#include <linux/file.h>
/* [한국어] VFIO 외부 ABI. VFIO_PCI_INTX_IRQ_INDEX 같은 IRQ 인덱스 상수와
 * VFIO_IRQ_SET_DATA_NONE / _BOOL / _EVENTFD, VFIO_IRQ_SET_ACTION_MASK /
 * _UNMASK / _TRIGGER 플래그가 여기서 온다. 이 파일의 ioctl 디스패처가
 * 바로 그 상수들 위에 세워져 있다. */
#include <linux/vfio.h>
/* [한국어] [상류 코드 관찰] 이 헤더가 주는 이름(wait_queue_head_t, wake_up 계열)을
 * 직접 쓰는 곳을 이 파일에서 찾지 못했다. eventfd 와 virqfd 가 내부적으로
 * wait queue 를 쓰지만 그 타입이 이 파일에 드러나지는 않는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
#include <linux/wait.h>
/* [한국어] kzalloc_obj, kfree, kasprintf. 벡터별 컨텍스트와 IRQ 이름 문자열
 * 할당에 쓴다. */
#include <linux/slab.h>

/* [한국어] drivers/vfio/pci 내부 전용 헤더. 이 파일이 노출하는 vfio_pci_intx_mask,
 * vfio_pci_intx_unmask, vfio_pci_set_irqs_ioctl 의 선언처이자, 이 파일이
 * 부르는 vfio_pci_eventfd_replace_locked, vfio_pci_memory_lock_and_enable,
 * vfio_pci_memory_unlock_and_restore 의 선언처다.
 * include/linux/vfio_pci_core.h 도 이 헤더를 통해 딸려 온다. */
#include "vfio_pci_priv.h"

/* [한국어] 벡터 하나의 상태를 담는 구조체. INTx 는 벡터가 하나뿐이라 인덱스 0 에
 * 하나만 있고, MSI/MSI-X 는 벡터마다 하나씩 vdev->ctx xarray 에 담긴다.
 * 이 구조체가 이 파일의 중심 자료구조이며, 사용자 공간의 eventfd 와 커널의
 * IRQ 번호를 잇는 매듭이다. 이 파일 안에서만 쓰이는 비공개 타입이라
 * 헤더가 아니라 여기 정의돼 있다. */
struct vfio_pci_irq_ctx {
	/* [한국어] 자신이 속한 디바이스로 돌아가는 역포인터.
	 * 설정자: vfio_intx_enable 이 컨텍스트를 만든 직후 채운다. MSI 경로는
	 * 이 필드를 채우지 않는다 — MSI 핸들러가 디바이스를 필요로 하지 않기
	 * 때문이다.
	 * 읽는 자: vfio_intx_handler 하나뿐이다. **하드 인터럽트 문맥에서 xarray
	 * 조회 없이 디바이스를 찾기 위해** 존재하는 필드다.
	 * 값 범위: INTx 컨텍스트에서는 유효 포인터, MSI/MSI-X 컨텍스트에서는 NULL.
	 * 동기화: 생성 시 한 번 쓰고 그 뒤로는 읽기 전용이라 락이 필요 없다. */
	struct vfio_pci_core_device	*vdev;
	/* [한국어] 인터럽트를 전달할 사용자 eventfd. 이 파일의 존재 이유가 담긴 필드다.
	 * 설정자: INTx 는 vfio_intx_enable 과 vfio_intx_set_signal(WRITE_ONCE),
	 * MSI/MSI-X 는 vfio_msi_set_vector_signal 이 마지막 단계에서 채운다.
	 * 읽는 자: vfio_send_intx_eventfd 가 READ_ONCE 로 읽고, MSI 는 이 값을
	 * request_irq 의 dev_id 로 넘겨 핸들러가 인자로 직접 받는다.
	 * vfio_pci_set_msi_trigger 의 루프백 경로도 직접 읽는다.
	 * 값 범위: 유효한 eventfd 참조이거나 NULL. NULL 이 정상인 경우는 INTx 를
	 * fd -1 로 켠 때뿐이며, MSI 는 fd 가 음수면 컨텍스트 자체를 만들지 않는다.
	 * 동기화: 하드 인터럽트 핸들러와 ioctl 이 함께 만지므로 WRITE_ONCE 와
	 * READ_ONCE 쌍을 쓴다. 옛 값을 놓기 전에는 synchronize_irq 와
	 * vfio_virqfd_flush_thread 로 모든 사용자가 빠져나가기를 기다린다.
	 * 참조 소유권은 이 필드에 있으며, 컨텍스트를 없앨 때 eventfd_ctx_put 한다. */
	struct eventfd_ctx		*trigger;
	/* [한국어] "이 fd 에 쓰면 INTx 마스크를 풀어라" 는 자동화 슬롯.
	 * 설정자: vfio_pci_set_intx_unmask 의 EVENTFD 경로가 vfio_virqfd_enable 로
	 * 채우고, 같은 함수와 vfio_intx_disable, vfio_msi_disable 이
	 * vfio_virqfd_disable 로 비운다.
	 * 읽는 자: drivers/vfio/virqfd.c 의 wakeup 과 shutdown 경로가 슬롯 주소를
	 * 들고 있으며, vfio_intx_set_signal 이 flush 대상으로 쓴다.
	 * 값 범위: 등록된 virqfd 포인터이거나 NULL. 사용자가 eventfd 를 닫으면
	 * virqfd 쪽이 스스로 NULL 로 만든다.
	 * 동기화: virqfd.c 의 파일 전역 spinlock 이 이 슬롯의 갱신을 직렬화한다.
	 * 이 파일은 그 슬롯의 주소만 넘기고 직접 만지지 않는다.
	 * 이 자동화가 있으면 사용자가 인터럽트를 처리한 뒤 ioctl 왕복 없이
	 * eventfd 한 번 쓰기로 다음 인터럽트를 받을 수 있다. */
	struct virqfd			*unmask;
	/* [한국어] "이 fd 에 쓰면 INTx 를 마스크하라" 는 자동화 슬롯.
	 * 설정자: 지금은 아무도 채우지 않는다. vfio_pci_set_intx_mask 의 EVENTFD
	 * 경로가 상류 코드에서 -ENOTTY 로 남아 있기 때문이다.
	 * 읽는 자: vfio_intx_disable 과 vfio_msi_disable 이 정리 대상으로 삼는다.
	 * 언제나 NULL 이라 vfio_virqfd_disable 이 사실상 워크큐 flush 만 한다.
	 * 값 범위: 현재는 NULL 뿐.
	 * 동기화: unmask 와 같다.
	 * 필드가 미리 자리를 잡고 있는 것은 상류의 "XXX implement me" 가 채워질
	 * 자리를 남겨 둔 것이다. */
	struct virqfd			*mask;
	/* [한국어] /proc/interrupts 에 보일 IRQ 이름 문자열.
	 * 설정자: vfio_intx_enable 은 "vfio-intx(도메인:버스:장치.기능)" 을,
	 * vfio_msi_set_vector_signal 은 "vfio-msi[벡터](주소)" 또는
	 * "vfio-msix[벡터](주소)" 를 kasprintf 로 만든다.
	 * 읽는 자: request_irq 에 넘긴 뒤 커널 IRQ 코어가 소유권 없이 참조만 한다.
	 * 그래서 IRQ 가 등록된 동안 이 문자열이 살아 있어야 하고, free_irq 뒤에야
	 * 해제할 수 있다.
	 * 값 범위: 유효한 힙 문자열. 할당 실패면 컨텍스트 생성 자체가 실패한다.
	 * 동기화: 생성 시 한 번 쓰고 해제까지 바뀌지 않는다. */
	char				*name;
	/* [한국어] INTx 라인이 지금 마스크돼 있는지.
	 * 설정자: vfio_intx_enable 이 virq_disabled 로 초기화하고, 그 뒤로는
	 * __vfio_pci_intx_mask, vfio_pci_intx_unmask_handler, vfio_intx_handler
	 * 세 곳이 irqlock 아래에서 갱신한다.
	 * 읽는 자: 같은 세 함수.
	 * 값 범위: true 면 라인이 막혀 있어 인터럽트가 오지 않는다.
	 * 동기화: **하드 인터럽트 핸들러와 ioctl 이 함께 만지는 유일한 필드이며,
	 * 그래서 vdev->irqlock spinlock 이 존재한다.** irqsave 판으로 잡아야
	 * 하드 인터럽트 문맥과 프로세스 문맥이 같은 CPU 에서 교착하지 않는다.
	 * MSI/MSI-X 컨텍스트에서는 쓰이지 않는다 — 마스킹 개념이 없기 때문이다. */
	bool				masked;
	/* [한국어] KVM 이 이 인터럽트를 게스트에 직통으로 잇게 해 주는 등록 구조체.
	 * 설정자와 읽는 자: vfio_msi_set_vector_signal 이 등록하고 해제한다.
	 * INTx 경로는 쓰지 않는다 — 마스킹이 필요해 커널을 거쳐야 하기 때문이다.
	 * 값 범위: irq bypass 코어가 관리하는 불투명 구조체. 등록이 실패해도
	 * 치명적이지 않다 — 인터럽트가 커널을 거쳐 eventfd 로 도는 평범한 경로로
	 * 돌아갈 뿐이다.
	 * 동기화: irq bypass 코어가 자기 뮤텍스로 직렬화한다. 이 파일은 등록과
	 * 해제 순서만 지키면 되며, 반드시 free_irq 보다 먼저 해제해야 한다. */
	struct irq_bypass_producer	producer;
};

/* [한국어]
 * irq_is - 이 디바이스의 현재 인터럽트 모드가 주어진 종류인지 본다
 *
 * @vdev: 대상 디바이스.
 * @type: 비교할 VFIO IRQ 인덱스(INTx / MSI / MSI-X / ERR / REQ 중 하나).
 * @return: 같으면 true.
 *
 * 왜 필요한가: INTx, MSI, MSI-X 는 동시에 켤 수 없다. vdev->irq_type 하나가
 * 현재 모드를 담으며, 모든 설정 함수가 "지금 모드가 무엇인가" 를 먼저 확인해야
 * 한다. MSI 와 MSI-X 를 같은 코드가 처리하므로 is_intx 처럼 종류를 못박은
 * 판정 대신 인자로 받는 판정도 필요하다.
 *
 * 동작 과정: 필드 하나를 비교해 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl). 호출자가 igate 를 쥐고 있어야
 * irq_type 이 안정적이다 — 이 함수 자체는 락을 잡지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_set_msi_trigger → [irq_is]
 */
static bool irq_is(struct vfio_pci_core_device *vdev, int type)
{
	/* [한국어] 현재 모드를 인자와 비교한다. igate 를 쥔 호출자만 이 값을 신뢰할 수 있다. */
	return vdev->irq_type == type;
}

/* [한국어]
 * is_intx - 이 디바이스가 지금 INTx 모드인지 본다
 *
 * @vdev: 대상 디바이스.
 * @return: INTx 모드면 true.
 *
 * 왜 필요한가: INTx 전용 코드가 여기저기 흩어져 있고, 그중 일부는 INTx 가
 * 아닌 상태에서도 불린다. 특히 config 공간의 INTx Disable 비트 처리는
 * 현재 모드가 MSI 여도 들어오므로(사용자가 COMMAND 를 쓰는 것을 막을 수 없다)
 * 모드 확인이 필수다.
 *
 * 동작 과정: irq_type 을 INTx 인덱스와 비교한다.
 *
 * 실행 컨텍스트: 프로세스 문맥과 **하드 인터럽트 문맥 양쪽**에서 불린다 —
 * vfio_send_intx_eventfd 가 vfio_intx_handler 안에서 이것을 쓴다. 단순 정수
 * 비교라 어느 문맥에서도 안전하다. 다만 락 없이 읽으므로 값이 그 순간의
 * 스냅숏일 뿐이며, 모드 전환과 겹치면 옛 값을 볼 수 있다. 그 경합은
 * igate 아래에서 free_irq 와 synchronize_irq 로 닫힌다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_send_intx_eventfd / __vfio_pci_intx_mask /
 *   vfio_pci_intx_unmask_handler / vfio_pci_set_intx_unmask /
 *   vfio_pci_set_intx_mask / vfio_pci_set_intx_trigger → [is_intx]
 */
static bool is_intx(struct vfio_pci_core_device *vdev)
{
	/* [한국어] INTx 인덱스와 비교한다. 하드 인터럽트 문맥에서도 불리므로 락을 잡지
	 * 않으며, 그 대신 모드 전환 시 free_irq 와 synchronize_irq 가 경합을 닫는다. */
	return vdev->irq_type == VFIO_PCI_INTX_IRQ_INDEX;
}

/* [한국어]
 * is_irq_none - 이 디바이스가 아직 어떤 인터럽트 모드도 켜지 않았는지 본다
 *
 * @vdev: 대상 디바이스.
 * @return: INTx/MSI/MSI-X 중 어느 것도 아니면 true.
 *
 * 왜 필요한가: **세 모드의 배타성을 지키는 문지기다.** 새 모드를 켜려는
 * 함수(vfio_intx_enable, vfio_msi_enable)는 반드시 이 함수로 "지금 비어
 * 있는가" 를 확인하고, 아니면 -EINVAL 로 거절한다. 사용자가 INTx 를 켠 채로
 * MSI 를 켜려 하면 IRQ 등록이 겹쳐 하드웨어와 커널 상태가 어긋난다.
 * ERR 과 REQ 인덱스는 하드웨어 인터럽트가 아니라 여기서 세지 않는다 —
 * 그 둘은 언제든 독립적으로 켤 수 있다.
 *
 * 동작 과정: irq_type 이 세 하드웨어 모드 중 어느 것도 아니면 true.
 * 아무 모드도 없을 때 irq_type 에 들어 있는 값은 VFIO_PCI_NUM_IRQS 이며,
 * vfio_intx_disable 과 vfio_msi_disable 이 그 값으로 되돌린다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl), igate 아래.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_intx_enable / vfio_msi_enable / vfio_pci_set_intx_trigger /
 *   vfio_pci_set_msi_trigger → [is_irq_none]
 */
static bool is_irq_none(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 세 하드웨어 모드 중 어느 것도 아닌지 본다. ERR 과 REQ 는 하드웨어
	 * 인터럽트가 아니라 여기 넣지 않는다 — 그 둘은 다른 모드와 동시에 켤 수 있다.
	 * 아무 모드도 없을 때 irq_type 에 들어 있는 값은 VFIO_PCI_NUM_IRQS 이며,
	 * vfio_intx_disable 과 vfio_msi_disable 이 그 값을 심는다. */
	return !(vdev->irq_type == VFIO_PCI_INTX_IRQ_INDEX ||
		 vdev->irq_type == VFIO_PCI_MSI_IRQ_INDEX ||
		 vdev->irq_type == VFIO_PCI_MSIX_IRQ_INDEX);
}

/* [한국어]
 * vfio_irq_ctx_get - 벡터 번호로 인터럽트 컨텍스트를 찾는다
 *
 * @vdev: 대상 디바이스.
 * @index: 벡터 번호. INTx 는 언제나 0, MSI/MSI-X 는 0부터 시작하는 벡터 인덱스.
 * @return: 있으면 컨텍스트 포인터, 없으면 NULL.
 *
 * 왜 필요한가: 벡터마다 eventfd 와 마스크 상태가 따로 있고, 벡터 수가
 * 디바이스마다 다르며 MSI-X 에서는 동적으로 늘어날 수도 있다. 그래서 고정
 * 배열 대신 xarray 로 성기게 담는다. 이 함수가 그 조회창이다.
 *
 * 동작 과정: xarray 에서 인덱스로 조회한다. xa_load 는 RCU 로 보호되는
 * 무락 조회라 별도 락이 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥(ioctl)과 **하드 인터럽트 문맥이 아닌 곳**에서
 * 불린다. 하드 인터럽트 핸들러는 이 조회를 쓰지 않고 request_irq 에 넘긴
 * dev_id 로 컨텍스트를 직접 받는다 — 인터럽트마다 xarray 를 뒤지지 않기
 * 위한 설계다.
 *
 * 에러 경로: 없다. NULL 반환을 호출자가 판단한다.
 *
 * 호출 체인:
 *   __vfio_pci_intx_mask / __vfio_pci_intx_unmask / vfio_intx_set_signal /
 *   vfio_intx_disable / vfio_msi_set_vector_signal / vfio_pci_set_intx_unmask /
 *   vfio_pci_set_intx_trigger / vfio_pci_set_msi_trigger
 *     → [vfio_irq_ctx_get] → xa_load
 */
static
struct vfio_pci_irq_ctx *vfio_irq_ctx_get(struct vfio_pci_core_device *vdev,
					  unsigned long index)
{
	/* [한국어] xarray 조회. RCU 로 보호되는 무락 읽기라 호출자가 락을 잡지 않아도 된다.
	 * 없는 인덱스면 NULL 이 나온다. */
	return xa_load(&vdev->ctx, index);
}

/* [한국어]
 * vfio_irq_ctx_free - 인터럽트 컨텍스트를 xarray 에서 빼고 해제한다
 *
 * @vdev: 대상 디바이스.
 * @ctx: 해제할 컨텍스트.
 * @index: 그 컨텍스트가 담긴 벡터 번호.
 * @return: 없다.
 *
 * 왜 필요한가: xarray 에서 지우는 것과 메모리를 놓는 것을 한 쌍으로 묶어,
 * 지웠는데 해제하지 않거나 그 반대인 실수를 막는다. 순서가 중요한데,
 * 먼저 xarray 에서 지워야 다른 코드가 해제된 포인터를 조회할 수 없다.
 *
 * 동작 과정: xa_erase 로 슬롯을 비우고 구조체를 해제한다. ctx 안의
 * name 과 trigger 는 이 함수가 책임지지 않으므로 호출자가 미리 정리해야 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. xa_erase 는 내부 락을 잡는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_intx_enable(실패 롤백) / vfio_intx_disable /
 *   vfio_msi_set_vector_signal → [vfio_irq_ctx_free] → xa_erase, kfree
 */
static void vfio_irq_ctx_free(struct vfio_pci_core_device *vdev,
			      struct vfio_pci_irq_ctx *ctx, unsigned long index)
{
	/* [한국어] 먼저 슬롯을 비운다. 순서가 중요하다 — 해제한 뒤에 지우면 그 사이
	 * 다른 코드가 해제된 포인터를 조회할 수 있다. */
	xa_erase(&vdev->ctx, index);
	/* [한국어] 구조체를 해제한다. ctx->name 과 ctx->trigger 는 호출자가 미리 정리해
	 * 두었어야 한다. */
	kfree(ctx);
}

/* [한국어]
 * vfio_irq_ctx_alloc - 벡터 하나의 인터럽트 컨텍스트를 만들어 xarray 에 등록한다
 *
 * @vdev: 대상 디바이스.
 * @index: 벡터 번호.
 * @return: 성공하면 0 초기화된 컨텍스트 포인터, 실패하면 NULL.
 *          호출자는 NULL 을 -ENOMEM 으로 번역한다.
 *
 * 왜 필요한가: 컨텍스트 할당과 xarray 등록이 한 쌍이어야 하고, 등록 실패
 * 시 할당한 메모리를 반드시 되돌려야 한다. 그 쌍을 한 함수로 묶는다.
 *
 * 동작 과정:
 *  1. 0 초기화로 구조체를 만든다. masked 가 false 로, 모든 포인터가 NULL 로
 *     시작한다는 뜻이며 호출자가 그 초기 상태에 의존한다.
 *  2. xa_insert 로 등록한다. xa_store 가 아니라 insert 를 쓰는 것이 중요한데,
 *     insert 는 그 슬롯이 이미 차 있으면 실패한다 — 같은 벡터에 컨텍스트를
 *     두 벌 만들어 하나를 잃어버리는 사고를 막는다.
 *  3. 실패하면 방금 만든 구조체를 해제하고 NULL 을 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. GFP_KERNEL_ACCOUNT 라 잠들 수
 * 있고, 이 메모리가 요청 프로세스의 memcg 에 달린다 — 컨테이너가 벡터를
 * 무한정 만들어 호스트 메모리를 고갈시키지 못하게 하는 장치다.
 *
 * 에러 경로: 두 실패 모두 NULL 반환이며 부분 상태를 남기지 않는다.
 *
 * 호출 체인:
 *   vfio_intx_enable / vfio_msi_set_vector_signal
 *     → [vfio_irq_ctx_alloc] → kzalloc_obj, xa_insert
 */
static struct vfio_pci_irq_ctx *
vfio_irq_ctx_alloc(struct vfio_pci_core_device *vdev, unsigned long index)
{
	/* [한국어] 만들 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;
	/* [한국어] xarray 등록 결과. */
	int ret;

	/* [한국어] 0 초기화 할당. masked 가 false 로, 모든 포인터가 NULL 로 시작한다는
	 * 사실에 호출자들이 의존한다. ACCOUNT 는 이 메모리를 요청 프로세스의
	 * memcg 에 달아, 컨테이너가 벡터를 무한정 만들어 호스트 메모리를 고갈시키지
	 * 못하게 한다. */
	ctx = kzalloc_obj(*ctx, GFP_KERNEL_ACCOUNT);
	if (!ctx)
		return NULL;

	/* [한국어] xarray 에 등록한다. store 가 아니라 insert 를 쓰는 것이 중요한데,
	 * insert 는 슬롯이 이미 차 있으면 -EBUSY 로 실패한다 — 같은 벡터에
	 * 컨텍스트를 두 벌 만들어 하나를 잃어버리는 사고를 막는다. */
	ret = xa_insert(&vdev->ctx, index, ctx, GFP_KERNEL_ACCOUNT);
	/* [한국어] 등록 실패. 방금 만든 것을 되돌린다. */
	if (ret) {
		/* [한국어] 할당 해제. */
		kfree(ctx);
		return NULL;
	}

	/* [한국어] 등록까지 성공한 컨텍스트. */
	return ctx;
}

/* [한국어]
 * vfio_send_intx_eventfd - INTx 인터럽트를 사용자 공간 eventfd 로 전달한다
 *
 * @opaque: struct vfio_pci_core_device 포인터. virqfd 의 thread 콜백
 *          시그니처를 맞추느라 void 포인터로 받는다.
 * @data: struct vfio_pci_irq_ctx 포인터. 같은 이유로 void 포인터다.
 * @return: 없다.
 *
 * 왜 필요한가: 인터럽트를 사용자에게 알리는 유일한 수단이다. 세 곳에서
 * 불리는데 문맥이 모두 다르다는 점이 이 함수의 핵심이다.
 *  (1) vfio_intx_handler 안 — **하드 인터럽트 문맥**.
 *  (2) __vfio_pci_intx_unmask 안 — 프로세스 문맥(ioctl).
 *  (3) virqfd 의 thread 콜백으로 — 워커(프로세스 문맥).
 * 어느 문맥에서도 안전해야 하므로 잠들지 않고 락도 잡지 않는다.
 * eventfd_signal 자체가 atomic 문맥에서 호출 가능하도록 설계돼 있어 성립한다.
 *
 * 동작 과정:
 *  1. 지금도 INTx 모드이고 가상 INTx Disable 이 서 있지 않은지 확인한다.
 *     virq_disabled 는 vfio_pci_config.c 의 vfio_basic_config_write 가 사용자의
 *     COMMAND 쓰기를 보고 갱신하는 필드다. 사용자가 소프트웨어적으로 INTx 를
 *     막아 둔 상태라면 인터럽트를 전달해서는 안 된다. 두 조건을 likely 로
 *     감싼 것은 정상 경로가 압도적으로 흔하기 때문이다.
 *  2. ctx->trigger 를 READ_ONCE 로 읽는다. **이 한 줄이 이 함수의 동시성
 *     안전을 지탱한다.** ioctl 경로가 WRITE_ONCE 로 이 포인터를 언제든 교체할
 *     수 있어, 컴파일러가 값을 두 번 읽어 서로 다른 결과를 얻는 일을 막아야
 *     한다.
 *  3. 포인터가 살아 있으면 eventfd 카운터를 올려 사용자를 깨운다. NULL 검사가
 *     필요한 이유는 사용자가 fd 를 -1 로 주어 트리거 없이 INTx 를 켤 수 있기
 *     때문이다(vfio_pci_set_intx_trigger 의 fd < 0 경로).
 *
 * 실행 컨텍스트: 위 세 가지. 잠들지 않고 락을 잡지 않으며 어느 문맥에서도
 * 안전하다.
 *
 * 에러 경로: 없다. 조건이 맞지 않으면 조용히 아무것도 하지 않는다.
 *
 * 호출 체인:
 *   vfio_intx_handler(하드 인터럽트) / __vfio_pci_intx_unmask(ioctl) /
 *   drivers/vfio/virqfd.c:275 의 virqfd_inject(워커)
 *     → [vfio_send_intx_eventfd] → eventfd_signal
 */
/*
 * INTx
 */
static void vfio_send_intx_eventfd(void *opaque, void *data)
{
	/* [한국어] virqfd 콜백 시그니처에 맞추느라 void 포인터로 받은 것을 되돌린다. */
	struct vfio_pci_core_device *vdev = opaque;

	/* [한국어] 두 가지를 확인한다. (a) 아직 INTx 모드인가 — 이 함수가 워커에서
	 * 늦게 실행될 수 있어 그 사이 모드가 바뀌었을 수 있다. (b) 사용자가 config
	 * 공간의 INTx Disable 을 세워 두지 않았는가 — virq_disabled 는
	 * vfio_pci_config.c 의 vfio_basic_config_write 가 갱신하는 공유 상태이며,
	 * 서 있으면 사용자가 소프트웨어적으로 인터럽트를 막아 둔 것이다.
	 * 정상 경로가 압도적으로 흔해 likely 로 표시한다. */
	if (likely(is_intx(vdev) && !vdev->virq_disabled)) {
		/* [한국어] 역시 void 포인터를 되돌린다. */
		struct vfio_pci_irq_ctx *ctx = data;
		/* [한국어] **이 파일의 동시성 안전을 지탱하는 한 줄.** ioctl 경로가 WRITE_ONCE 로
		 * 이 포인터를 언제든 교체할 수 있어, 컴파일러가 값을 두 번 읽어 서로 다른
		 * 결과를 얻거나 검사와 사용 사이에 다시 읽는 일을 막아야 한다.
		 * 교체 측은 이 함수가 빠져나가기를 synchronize_irq 와
		 * vfio_virqfd_flush_thread 로 기다린 뒤에야 옛 참조를 놓는다. */
		struct eventfd_ctx *trigger = READ_ONCE(ctx->trigger);

		/* [한국어] NULL 일 수 있다. 사용자가 fd 를 -1 로 주어 "전달할 곳 없는 INTx" 를
		 * 켤 수 있기 때문이다(vfio_pci_set_intx_trigger 의 fd < 0 경로). */
		if (likely(trigger))
			/* [한국어] eventfd 카운터를 올려 사용자 공간의 poll/read 를 깨운다. atomic 문맥에서
			 * 호출 가능하도록 설계돼 있어, 하드 인터럽트 문맥에서 불려도 안전하다. */
			eventfd_signal(trigger);
	}
}

/* [한국어]
 * __vfio_pci_intx_mask - INTx 라인을 실제로 마스크한다(igate 를 이미 쥔 호출자용)
 *
 * @vdev: 대상 디바이스.
 * @return: 이번 호출로 마스크 상태가 실제로 바뀌었으면 true. 이미 마스크돼
 *          있었거나 INTx 모드가 아니면 false. 상류 주석이 그 의미를 명시한다.
 *          호출자 중 vfio_pci_core.c 의 런타임 PM 진입 경로가 이 반환값으로
 *          "복귀할 때 unmask 해야 하는가" 를 기억한다 — 사용자가 이미
 *          마스크해 둔 것을 커널이 멋대로 풀면 안 되기 때문이다.
 *
 * 왜 필요한가: INTx 는 레벨 트리거 공유 라인이라, 인터럽트를 사용자가
 * 처리하기 전까지 계속 어서션 상태다. 마스크하지 않으면 호스트가 같은
 * 인터럽트를 무한히 다시 받아 사실상 정지한다. 그 마스킹의 실행부다.
 *
 * 마스킹 방법이 두 가지인 것이 이 함수의 핵심이다.
 *  - PCI 2.3 이상(pci_2_3 가 참): COMMAND 의 INTx Disable 비트로 디바이스
 *    자신을 침묵시킨다. 라인이 공유돼도 이 함수만 조용해지므로 다른
 *    디바이스의 인터럽트를 막지 않는다.
 *  - PCI 2.3 미만: 그런 비트가 없다. 어쩔 수 없이 IRQ 칩 수준에서 라인
 *    전체를 끈다. 그래서 이런 디바이스는 IRQ 를 독점해야 하고,
 *    vfio_intx_enable 이 IRQF_SHARED 없이 등록한다.
 *
 * 동작 과정:
 *  1. lockdep 으로 호출자가 igate 를 쥐었는지 확인한다.
 *  2. irqlock 을 irqsave 로 잡는다. 하드 인터럽트 핸들러와 ctx->masked 를
 *     공유하므로 인터럽트를 끈 채로 잡아야 같은 CPU 에서 교착하지 않는다.
 *  3. 상류 주석이 밝히듯 이 함수는 INTx 모드가 아닐 때도 불린다. 마스킹
 *     요청이 인터럽트, ioctl, config 공간의 INTx Disable 세 방향에서 오는데
 *     마지막 것은 모드와 무관하게 들어오기 때문이다. 그 경우 물리 비트만
 *     가상 비트를 따라가게 하고 끝낸다.
 *  4. 벡터 0 의 컨텍스트를 얻는다. INTx 는 벡터가 하나뿐이라 인덱스가 언제나 0.
 *  5. 아직 마스크되지 않았으면 위 두 방법 중 하나로 마스크한다. 상류 주석이
 *     check_and_mask 판을 쓰지 않는 이유를 밝힌다 — 그 판은 "인터럽트가 실제로
 *     올라와 있을 때만" 마스크하는데, 여기서는 조건 없이 반드시 마스크해야 한다.
 *  6. 상태를 기록하고 변경 사실을 반환한다.
 *
 * 실행 컨텍스트: 프로세스 문맥. igate 를 쥔 상태로 들어와야 하고, 안에서
 * irqlock 을 잡는다. 락 안에서 잠들지 않는다.
 *
 * 에러 경로: 컨텍스트가 없으면 WARN 을 남기고 아무것도 하지 않는다.
 *
 * 호출 체인:
 *   vfio_pci_intx_mask / vfio_pci_set_intx_mask
 *     → [__vfio_pci_intx_mask]
 *     → vfio_irq_ctx_get, pci_intx (drivers/pci/pci.c:8201),
 *       disable_irq_nosync
 */
/* Returns true if the INTx vfio_pci_irq_ctx.masked value is changed. */
static bool __vfio_pci_intx_mask(struct vfio_pci_core_device *vdev)
{
	/* [한국어] INTx Disable 비트를 조작할 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 벡터 0 의 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;
	/* [한국어] spin_lock_irqsave 가 저장할 인터럽트 상태. */
	unsigned long flags;
	/* [한국어] 이번 호출로 상태가 실제로 바뀌었는지. 런타임 PM 진입 경로가 이 값으로
	 * "복귀할 때 풀어야 하는가" 를 기억하므로, 이미 마스크돼 있었으면 false 를
	 * 돌려주어 사용자가 막아 둔 것을 커널이 멋대로 풀지 않게 한다. */
	bool masked_changed = false;

	/* [한국어] 호출자가 igate 를 쥐고 들어와야 한다. 이 함수는 잡지 않는다 —
	 * igate 를 잡는 판이 vfio_pci_intx_mask 로 따로 있다. */
	lockdep_assert_held(&vdev->igate);

	/* [한국어] ctx->masked 와 물리 마스킹 조작을 하드 인터럽트 핸들러로부터 지킨다.
	 * irqsave 판이어야 하는 이유는 vfio_intx_handler 가 같은 락을 하드 인터럽트
	 * 문맥에서 잡기 때문이다 — 인터럽트를 켠 채로 잡았다가 그 CPU 에 인터럽트가
	 * 들어오면 자기 자신과 교착한다. */
	spin_lock_irqsave(&vdev->irqlock, flags);

	/*
	 * Masking can come from interrupt, ioctl, or config space
	 * via INTx disable.  The latter means this can get called
	 * even when not using intx delivery.  In this case, just
	 * try to have the physical bit follow the virtual bit.
	 */
	/* [한국어] 상류 주석이 이 경로를 설명한다. 마스킹 요청은 인터럽트, ioctl,
	 * config 공간의 INTx Disable 세 방향에서 오는데 마지막 것은 현재 모드와
	 * 무관하게 들어온다. 사용자가 MSI 를 쓰면서 COMMAND 를 만질 수 있기 때문이다. */
	if (unlikely(!is_intx(vdev))) {
		/* [한국어] INTx 모드가 아니어도 물리 비트만은 가상 비트를 따라가게 한다.
		 * 사용자가 나중에 INTx 로 돌아왔을 때 상태가 일관되도록. */
		if (vdev->pci_2_3)
			/* [한국어] COMMAND 의 INTx Disable 비트를 세워 이 함수를 침묵시킨다
			 * (drivers/pci/pci.c:8201). 인자 0 은 "INTx 를 끈다" 는 뜻이다. */
			pci_intx(pdev, 0);
		goto out_unlock;
	}

	/* [한국어] INTx 는 벡터가 하나뿐이라 인덱스가 언제나 0 이다. */
	ctx = vfio_irq_ctx_get(vdev, 0);
	/* [한국어] INTx 모드인데 컨텍스트가 없으면 프로그래밍 오류다. 경고를 한 번만
	 * 남기고 아무것도 하지 않는다. */
	if (WARN_ON_ONCE(!ctx))
		goto out_unlock;

	/* [한국어] 이미 마스크돼 있으면 할 일이 없다. 이 검사가 masked_changed 를 결정한다. */
	if (!ctx->masked) {
		/*
		 * Can't use check_and_mask here because we always want to
		 * mask, not just when something is pending.
		 */
		/* [한국어] PCI 2.3 이상은 디바이스 자신의 INTx Disable 비트로 마스크한다.
		 * 라인을 공유하는 다른 디바이스는 영향을 받지 않는다. */
		if (vdev->pci_2_3)
			/* [한국어] 상류 주석이 pci_check_and_mask_intx 판을 쓰지 않는 이유를 밝힌다 —
			 * 그 판은 인터럽트가 실제로 걸려 있을 때만 마스크하는데, 여기서는 조건 없이
			 * 반드시 마스크해야 한다. */
			pci_intx(pdev, 0);
		else
			/* [한국어] PCI 2.3 미만은 INTx Disable 비트가 없다. IRQ 칩 수준에서 라인 전체를
			 * 끄는 수밖에 없고, 그래서 이런 디바이스는 IRQ 를 독점해야 한다.
			 * nosync 판을 쓰는 이유는 지금 irqlock 을 쥐고 있어 진행 중인 핸들러를
			 * 기다리면 교착하기 때문이다 — 그 핸들러도 같은 락을 원한다. */
			disable_irq_nosync(pdev->irq);

		/* [한국어] 상태를 기록한다. */
		ctx->masked = true;
		/* [한국어] 이번 호출이 상태를 바꿨음을 알린다. */
		masked_changed = true;
	}

out_unlock:
	/* [한국어] 락과 인터럽트 상태 복원. */
	spin_unlock_irqrestore(&vdev->irqlock, flags);
	/* [한국어] 호출자가 복귀 시 풀지 말지 판단하는 근거. */
	return masked_changed;
}

/* [한국어]
 * vfio_pci_intx_mask - igate 를 직접 잡고 INTx 를 마스크한다(외부 호출자용)
 *
 * @vdev: 대상 디바이스.
 * @return: 마스크 상태가 실제로 바뀌었으면 true.
 *
 * 왜 필요한가: 같은 일을 하는 함수가 두 벌인 이유는 락 소유권이 다르기
 * 때문이다. ioctl 경로는 vfio_pci_core.c 의 vfio_pci_ioctl_set_irqs 가 이미
 * igate 를 잡아 주므로 밑줄 두 개짜리 판을 부르고, 그 밖의 경로 —
 * vfio_pci_config.c 의 COMMAND 쓰기 처리와 vfio_pci_core.c 의 런타임 PM 진입 —
 * 는 락을 쥐고 있지 않으므로 이 판을 통해 들어온다.
 *
 * 동작 과정: igate 를 잡고 실행부를 부른 뒤 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. mutex 를 잡으므로 잠들 수 있고, 따라서
 * 인터럽트 문맥에서 부를 수 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_config.c 의 vfio_basic_config_write(사용자가 INTx Disable 을 쓸 때),
 *   vfio_pci_core.c 의 런타임 PM 서스펜드
 *     → [vfio_pci_intx_mask] → __vfio_pci_intx_mask
 */
bool vfio_pci_intx_mask(struct vfio_pci_core_device *vdev)
{
	/* [한국어] 실행부의 반환값을 담을 곳. */
	bool mask_changed;

	/* [한국어] 이 진입점은 igate 를 쥐지 않은 경로(config 공간 쓰기, 런타임 PM)를
	 * 위한 것이라 직접 잡는다. mutex 라 잠들 수 있고, 따라서 인터럽트 문맥에서
	 * 이 함수를 부를 수 없다. */
	mutex_lock(&vdev->igate);
	/* [한국어] 실행부를 부른다. */
	mask_changed = __vfio_pci_intx_mask(vdev);
	/* [한국어] 락 해제. */
	mutex_unlock(&vdev->igate);

	/* [한국어] 상태 변경 여부를 그대로 전달한다. */
	return mask_changed;
}

/* [한국어]
 * vfio_pci_intx_unmask_handler - INTx 마스크를 푼다. atomic 문맥에서도 안전하도록 신호를 직접 보내지 않는다
 *
 * @opaque: struct vfio_pci_core_device 포인터. virqfd handler 시그니처를
 *          맞추느라 void 포인터다.
 * @data: struct vfio_pci_irq_ctx 포인터. 같은 이유.
 * @return: 0 이면 할 일이 끝났다. 양수(1)면 **호출자가 eventfd 신호를 대신
 *          보내야 한다**. 상류 주석이 그 규약과 이유를 명시한다.
 *
 * 왜 그런 반환 규약인가: 이 함수는 두 가지 문맥에서 불린다.
 *  (1) __vfio_pci_intx_unmask 안 — 프로세스 문맥. 양수면 곧바로
 *      vfio_send_intx_eventfd 를 부른다.
 *  (2) virqfd 의 handler 콜백으로 — **eventfd wait queue 락을 쥔 atomic
 *      문맥**(drivers/vfio/virqfd.c:179 의 virqfd_wakeup 안). 여기서
 *      eventfd_signal 을 부르면 같은 wait queue 락에서 자기 자신과 교착한다.
 *      그래서 양수만 반환하고, virqfd 가 그 값을 보고 워커를 큐잉해
 *      vfio_send_intx_eventfd 를 프로세스 문맥에서 부른다.
 *
 * 왜 마스크 해제가 신호를 낳는가: 상류 주석이 그 최적화를 설명한다.
 * 마스크를 푸는 순간 아직 처리되지 않은 인터럽트가 걸려 있으면 라인이 즉시
 * 다시 어서션되어 하드 인터럽트가 한 번 더 돈다. 그 비용을 아끼려고,
 * pci_check_and_unmask_intx 가 "펜딩이 있어 풀지 못했다" 고 알려 주면
 * 마스크를 유지한 채 사용자에게 인터럽트를 한 번 더 보내 준다.
 *
 * 동작 과정:
 *  1. irqlock 을 irqsave 로 잡는다.
 *  2. 상류 주석대로 마스크 해제 요청도 ioctl 과 config 공간 양쪽에서 오므로
 *     INTx 모드가 아닐 수 있다. 그 경우 물리 비트만 가상 비트를 따라가게 한다.
 *  3. 마스크돼 있고 사용자가 가상 INTx Disable 을 세워 두지 않았을 때만
 *     실제로 푼다. virq_disabled 가 서 있으면 사용자가 소프트웨어적으로 막아
 *     둔 것이라 풀어서는 안 된다.
 *  4. PCI 2.3 이상이면 pci_check_and_unmask_intx 로 원자적으로 확인하며 푼다.
 *     그 함수가 false 를 돌려주면 펜딩이 있어 풀지 못했다는 뜻이므로 1 을
 *     반환해 상위 단계가 신호를 보내게 한다.
 *     PCI 2.3 미만이면 IRQ 칩 수준에서 라인을 다시 켠다.
 *  5. masked 를 갱신한다. ret 이 양수면 여전히 마스크 상태로 남는다.
 *
 * 실행 컨텍스트: 위 두 가지. 어느 쪽이든 잠들지 않는다. irqlock 만 쓴다.
 *
 * 에러 경로: 없다. ctx 가 NULL 이면 2단계에서 걸러지지 않는 한 역참조하는데,
 * 그런 상황은 INTx 모드가 켜져 있으면서 벡터 0 컨텍스트가 없을 때뿐이라
 * 실제로 생기지 않는다.
 *
 * 호출 체인:
 *   __vfio_pci_intx_unmask(프로세스 문맥) 또는
 *   drivers/vfio/virqfd.c:179 의 virqfd_wakeup(atomic)
 *     → [vfio_pci_intx_unmask_handler]
 *     → pci_intx, pci_check_and_unmask_intx (drivers/pci/irq.c:734), enable_irq
 */
/*
 * If this is triggered by an eventfd, we can't call eventfd_signal
 * or else we'll deadlock on the eventfd wait queue.  Return >0 when
 * a signal is necessary, which can then be handled via a work queue
 * or directly depending on the caller.
 */
static int vfio_pci_intx_unmask_handler(void *opaque, void *data)
{
	/* [한국어] virqfd handler 시그니처에 맞춘 void 포인터를 되돌린다. */
	struct vfio_pci_core_device *vdev = opaque;
	/* [한국어] INTx Disable 비트를 만질 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 벡터 0 의 컨텍스트. 호출자가 미리 찾아 넘겨 준다. */
	struct vfio_pci_irq_ctx *ctx = data;
	/* [한국어] irqsave 저장용. */
	unsigned long flags;
	/* [한국어] 0 이면 할 일 끝, 양수면 호출자가 eventfd 신호를 보내야 한다는 뜻이다. */
	int ret = 0;

	/* [한국어] ctx->masked 를 하드 인터럽트 핸들러로부터 지킨다. */
	spin_lock_irqsave(&vdev->irqlock, flags);

	/*
	 * Unmasking comes from ioctl or config, so again, have the
	 * physical bit follow the virtual even when not using INTx.
	 */
	/* [한국어] 상류 주석대로 마스크 해제도 ioctl 과 config 공간 양쪽에서 오므로
	 * INTx 모드가 아닐 수 있다. */
	if (unlikely(!is_intx(vdev))) {
		/* [한국어] 모드가 아니어도 물리 비트가 가상 비트를 따라가게 한다. */
		if (vdev->pci_2_3)
			/* [한국어] COMMAND 의 INTx Disable 비트를 지워 인터럽트를 다시 허용한다.
			 * 인자 1 은 "INTx 를 켠다" 는 뜻이다. */
			pci_intx(pdev, 1);
		goto out_unlock;
	}

	/* [한국어] 마스크돼 있고, 사용자가 config 공간에서 소프트웨어적으로 막아 두지도
	 * 않았을 때만 실제로 푼다. virq_disabled 가 서 있는데 풀면 사용자 의도를
	 * 어기는 것이다. */
	if (ctx->masked && !vdev->virq_disabled) {
		/*
		 * A pending interrupt here would immediately trigger,
		 * but we can avoid that overhead by just re-sending
		 * the interrupt to the user.
		 */
		/* [한국어] PCI 2.3 이상 경로. */
		if (vdev->pci_2_3) {
			/* [한국어] 상태 확인과 마스크 해제를 원자적으로 함께 한다
			 * (drivers/pci/irq.c:734). false 를 돌려주면 아직 처리되지 않은 인터럽트가
			 * 걸려 있어 풀지 못했다는 뜻이다. */
			if (!pci_check_and_unmask_intx(pdev))
				/* [한국어] 상류 주석이 이 최적화를 설명한다 — 여기서 마스크를 풀면 걸려 있던
				 * 인터럽트가 즉시 다시 올라와 하드 인터럽트가 한 번 더 돈다. 그 비용을
				 * 아끼려고, 마스크는 유지한 채 사용자에게 인터럽트를 한 번 더 보내 준다.
				 * 양수 반환이 그 요청이며, 신호를 여기서 직접 보내지 않는 이유는 이 함수가
				 * eventfd wait queue 락을 쥔 atomic 문맥에서 불릴 수 있어 같은 eventfd 를
				 * 신호하면 교착하기 때문이다. */
				ret = 1;
		} else
			/* [한국어] PCI 2.3 미만은 IRQ 칩 수준에서 라인을 다시 켠다. 이 경로에는 펜딩
			 * 확인 수단이 없어 ret 이 0 으로 남고, 걸려 있던 인터럽트는 실제 하드
			 * 인터럽트로 다시 들어온다. */
			enable_irq(pdev->irq);

		/* [한국어] ret 이 양수면 풀지 못했으므로 여전히 마스크 상태다. 0 이면 풀렸다. */
		ctx->masked = (ret > 0);
	}

out_unlock:
	/* [한국어] 락과 인터럽트 상태 복원. */
	spin_unlock_irqrestore(&vdev->irqlock, flags);

	/* [한국어] 0 이면 끝, 양수면 호출자가 신호를 보내야 한다. */
	return ret;
}

/* [한국어]
 * __vfio_pci_intx_unmask - INTx 마스크를 풀고, 필요하면 eventfd 신호까지 보낸다(igate 를 이미 쥔 호출자용)
 *
 * @vdev: 대상 디바이스.
 * @return: 없다.
 *
 * 왜 필요한가: 마스크 해제의 프로세스 문맥판이다. atomic 제약이 없으므로
 * "신호를 보내야 한다" 는 반환값을 받으면 그 자리에서 바로 보낸다.
 *
 * 동작 과정:
 *  1. 벡터 0 의 컨텍스트를 얻는다.
 *  2. lockdep 으로 호출자가 igate 를 쥐었는지 확인한다.
 *  3. 실행부를 부르고, 양수를 받으면 eventfd 를 신호한다.
 *
 * [상류 코드 관찰] 1단계 뒤에 ctx 가 NULL 인지 확인하지 않는다. 짝이 되는
 * __vfio_pci_intx_mask 는 같은 조회 뒤에 WARN_ON_ONCE(!ctx) 로 걸러 내고,
 * 같은 파일의 vfio_intx_set_signal 과 vfio_intx_disable 도 그렇게 한다.
 * 네 함수 중 이 함수만 검사가 없다. 실제로는 아래 실행부가 INTx 모드가
 * 아닐 때 ctx 를 만지기 전에 빠져나가고, INTx 모드일 때는 벡터 0 컨텍스트가
 * 반드시 존재하므로 문제가 되지 않는다.
 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다.
 *
 * 실행 컨텍스트: 프로세스 문맥. igate 를 쥔 상태로 들어와야 한다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_intx_unmask / vfio_pci_set_intx_unmask
 *     → [__vfio_pci_intx_unmask]
 *     → vfio_irq_ctx_get, vfio_pci_intx_unmask_handler, vfio_send_intx_eventfd
 */
static void __vfio_pci_intx_unmask(struct vfio_pci_core_device *vdev)
{
	/* [한국어] [상류 코드 관찰] 조회 결과가 NULL 인지 확인하지 않는다. 짝이 되는
	 * __vfio_pci_intx_mask 는 같은 조회 뒤에 WARN_ON_ONCE(!ctx) 로 걸러 내고,
	 * 같은 파일의 vfio_intx_set_signal 과 vfio_intx_disable 도 그렇게 한다.
	 * 네 함수 중 이 함수만 검사가 없다. 실제로는 아래 실행부가 INTx 모드가
	 * 아닐 때 ctx 를 만지기 전에 빠져나가고, INTx 모드일 때는 벡터 0 컨텍스트가
	 * 반드시 존재하므로 문제가 되지 않는다.
	 * 원본(1f0e418bb6)에서 확인했으며 코드는 고치지 않았다. */
	struct vfio_pci_irq_ctx *ctx = vfio_irq_ctx_get(vdev, 0);

	/* [한국어] 호출자가 igate 를 쥐고 들어와야 한다. */
	lockdep_assert_held(&vdev->igate);

	/* [한국어] 실행부를 부른다. 양수면 "펜딩이 있어 풀지 못했으니 신호를 대신 보내
	 * 달라" 는 요청이다. */
	if (vfio_pci_intx_unmask_handler(vdev, ctx) > 0)
		/* [한국어] 여기는 프로세스 문맥이라 eventfd 를 그 자리에서 바로 신호해도 안전하다.
		 * virqfd 경로였다면 이 호출이 워커로 미뤄진다. */
		vfio_send_intx_eventfd(vdev, ctx);
}

/* [한국어]
 * vfio_pci_intx_unmask - igate 를 직접 잡고 INTx 마스크를 푼다(외부 호출자용)
 *
 * @vdev: 대상 디바이스.
 * @return: 없다.
 *
 * 왜 필요한가: vfio_pci_intx_mask 와 같은 이유다. igate 를 쥐지 않은 경로 —
 * vfio_pci_config.c 의 COMMAND 쓰기 처리와 vfio_pci_core.c 의 런타임 PM
 * 복귀 — 를 위한 진입점이다.
 *
 * 동작 과정: igate 를 잡고 실행부를 부른 뒤 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥. mutex 를 잡으므로 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_config.c 의 vfio_basic_config_write,
 *   vfio_pci_core.c 의 런타임 PM 리줌
 *     → [vfio_pci_intx_unmask] → __vfio_pci_intx_unmask
 */
void vfio_pci_intx_unmask(struct vfio_pci_core_device *vdev)
{
	/* [한국어] igate 를 쥐지 않은 경로(config 공간 쓰기, 런타임 PM 복귀)를 위한
	 * 진입점이라 직접 잡는다. */
	mutex_lock(&vdev->igate);
	/* [한국어] 실행부를 부른다. */
	__vfio_pci_intx_unmask(vdev);
	/* [한국어] 락 해제. */
	mutex_unlock(&vdev->igate);
}

/* [한국어]
 * vfio_intx_handler - INTx 하드 인터럽트 핸들러. 라인을 먼저 막고 사용자에게 알린다
 *
 * @irq: 발생한 리눅스 IRQ 번호. 이 함수는 쓰지 않는다 — 어차피 컨텍스트로
 *       대상을 특정할 수 있다.
 * @dev_id: request_irq 에 넘긴 값, 즉 이 벡터의 vfio_pci_irq_ctx 포인터.
 *          xarray 를 뒤지지 않고 곧바로 컨텍스트를 얻기 위한 설계다.
 * @return: IRQ_HANDLED 면 이 디바이스의 인터럽트였다는 뜻이고, IRQ_NONE 이면
 *          "내 것이 아니다" 라는 뜻이다. 공유 라인에서 커널이 이 값으로
 *          핸들러 사슬을 계속 돌지 판단하며, 아무도 HANDLED 를 내지 않는
 *          인터럽트가 반복되면 커널이 그 라인을 꺼 버린다.
 *
 * 왜 필요한가: **INTx 는 레벨 트리거라, 이 핸들러가 라인을 막지 않으면
 * 인터럽트가 즉시 다시 올라와 호스트가 정지한다.** 사용자 공간은 하드
 * 인터럽트 문맥에서 아무것도 할 수 없으므로, 커널이 대신 라인을 막고
 * eventfd 로 알린 뒤 사용자가 처리를 마치고 unmask 를 요청할 때까지 기다린다.
 * 이것이 VFIO INTx 중계의 핵심 계약이다.
 *
 * 동작 과정:
 *  1. dev_id 에서 컨텍스트를, 컨텍스트에서 디바이스를 꺼낸다. ctx->vdev 라는
 *     역포인터가 있는 이유가 바로 이 자리다 — 하드 인터럽트 문맥에서 xarray
 *     조회를 피하기 위한 것이다.
 *  2. irqlock 을 irqsave 로 잡는다. 이미 인터럽트가 꺼진 문맥이지만, 다른
 *     CPU 의 ioctl 경로와 ctx->masked 를 공유하므로 락 자체는 필요하다.
 *  3. PCI 2.3 미만이면 INTx Disable 비트가 없다. IRQ 칩 수준에서 라인을 끄고
 *     무조건 HANDLED 로 본다 — 이런 디바이스는 IRQ 를 독점하므로 다른
 *     디바이스와 혼동될 일이 없다.
 *  4. PCI 2.3 이상이면 라인이 공유될 수 있다. 아직 마스크되지 않았고
 *     pci_check_and_mask_intx 가 "이 디바이스가 정말 인터럽트를 올렸고
 *     마스크했다" 고 답할 때만 내 것이다. 그 함수가 상태 확인과 마스킹을
 *     원자적으로 함께 해 주는 것이 중요하다 — 따로 하면 그 사이에 인터럽트가
 *     사라져 남의 인터럽트를 가로챌 수 있다.
 *     상류 주석의 "may be shared" 가 masked 검사의 이유를 밝힌다. 이미
 *     마스크된 상태에서 라인이 올라왔다면 그것은 라인을 공유하는 다른
 *     디바이스의 인터럽트다.
 *  5. 락을 놓고, 내 인터럽트였으면 eventfd 를 신호한다. **락 밖에서 신호하는
 *     것이 중요하다** — eventfd 신호는 wait queue 락을 잡으므로 irqlock 안에서
 *     하면 락 순서가 뒤엉킬 수 있다.
 *
 * 실행 컨텍스트: **하드 인터럽트 문맥.** request_irq 에 threadfn 을 주지
 * 않았으므로 커널 IRQ 코어가 인터럽트를 받은 CPU 에서 직접 부른다. 잠들 수
 * 없고 mutex 를 잡을 수 없다.
 *
 * 에러 경로: 없다. 판정 결과가 곧 반환값이다.
 *
 * 호출 체인:
 *   커널 IRQ 코어 → [vfio_intx_handler]
 *     → disable_irq_nosync, pci_check_and_mask_intx (drivers/pci/irq.c:696),
 *       vfio_send_intx_eventfd
 */
static irqreturn_t vfio_intx_handler(int irq, void *dev_id)
{
	/* [한국어] request_irq 에 넘긴 값이 그대로 돌아온다. 하드 인터럽트 문맥에서
	 * xarray 를 뒤지지 않으려고 컨텍스트 포인터 자체를 dev_id 로 준 것이다. */
	struct vfio_pci_irq_ctx *ctx = dev_id;
	/* [한국어] 컨텍스트의 역포인터로 디바이스를 얻는다. 이 필드가 존재하는 유일한 이유다. */
	struct vfio_pci_core_device *vdev = ctx->vdev;
	/* [한국어] irqsave 저장용. */
	unsigned long flags;
	/* [한국어] 기본값은 "내 인터럽트가 아니다". 공유 라인에서 남의 인터럽트를 가로채지
	 * 않으려면 확실할 때만 HANDLED 로 바꿔야 한다. */
	int ret = IRQ_NONE;

	/* [한국어] ctx->masked 를 다른 CPU 의 ioctl 경로로부터 지킨다. 이미 인터럽트가
	 * 꺼진 문맥이지만 락 자체는 필요하다. */
	spin_lock_irqsave(&vdev->irqlock, flags);

	/* [한국어] PCI 2.3 미만이면 INTx Disable 비트가 없다. 이런 디바이스는 IRQ 를
	 * 독점하므로 라인이 올라왔다면 반드시 내 인터럽트다. */
	if (!vdev->pci_2_3) {
		/* [한국어] IRQ 칩 수준에서 라인을 끈다. **레벨 트리거이므로 여기서 끄지 않으면
		 * 인터럽트가 즉시 다시 올라와 호스트가 정지한다.** nosync 판이어야 하는
		 * 이유는 지금 irqlock 을 쥐고 있어 진행 중인 핸들러(= 자기 자신)를
		 * 기다리면 교착하기 때문이다. */
		disable_irq_nosync(vdev->pdev->irq);
		/* [한국어] 마스크 상태를 기록한다. */
		ctx->masked = true;
		/* [한국어] 내 인터럽트로 확정. */
		ret = IRQ_HANDLED;
	/* [한국어] PCI 2.3 이상이면 라인이 공유될 수 있다. 상류 주석의 "may be shared" 가
	 * masked 검사의 이유다 — 이미 마스크된 상태에서 라인이 올라왔다면 그것은
	 * 같은 라인을 쓰는 다른 디바이스의 인터럽트다. */
	} else if (!ctx->masked &&  /* may be shared */
		   /* [한국어] "이 디바이스가 정말 인터럽트를 올렸는가" 를 확인하고 그렇다면
		    * 원자적으로 마스크한다(drivers/pci/irq.c:696). 확인과 마스킹을 따로 하면
		    * 그 사이 인터럽트가 사라져 남의 것을 가로챌 수 있어, 한 연산이어야 한다. */
		   pci_check_and_mask_intx(vdev->pdev)) {
		/* [한국어] 마스크 상태를 기록한다. */
		ctx->masked = true;
		/* [한국어] 내 인터럽트로 확정. */
		ret = IRQ_HANDLED;
	}

	/* [한국어] **신호를 보내기 전에 락을 놓는다.** eventfd 신호는 wait queue 락을
	 * 잡으므로 irqlock 안에서 하면 락 순서가 뒤엉킬 수 있다. */
	spin_unlock_irqrestore(&vdev->irqlock, flags);

	/* [한국어] 내 인터럽트였을 때만 사용자에게 알린다. */
	if (ret == IRQ_HANDLED)
		/* [한국어] eventfd 를 신호해 사용자 공간을 깨운다. 이제 사용자가 디바이스를
		 * 처리하고 ioctl 이나 unmask eventfd 로 마스크를 풀어야 다음 인터럽트가 온다. */
		vfio_send_intx_eventfd(vdev, ctx);

	/* [한국어] 커널 IRQ 코어가 이 값으로 핸들러 사슬을 계속 돌지 판단한다. 아무도
	 * HANDLED 를 내지 않는 인터럽트가 반복되면 커널이 그 라인을 꺼 버린다. */
	return ret;
}

/* [한국어]
 * vfio_intx_enable - INTx 모드를 켠다. 컨텍스트를 만들고 IRQ 를 등록한다
 *
 * @vdev: 대상 디바이스.
 * @trigger: 인터럽트를 전달할 eventfd 컨텍스트. NULL 일 수 있다 — 사용자가
 *           fd 를 -1 로 주면 "인터럽트는 켜되 아직 받을 곳은 없다" 는 뜻이 된다.
 *           성공하면 이 참조의 소유권이 ctx->trigger 로 넘어간다.
 * @return: 0 성공. -EINVAL 이면 이미 다른 모드가 켜져 있고, -ENODEV 면 이
 *          디바이스에 쓸 수 있는 IRQ 가 없으며, -ENOMEM 이나 request_irq 의
 *          오류가 그대로 나올 수도 있다. 실패 시 trigger 의 참조는 호출자가
 *          되돌린다.
 *
 * 왜 필요한가: INTx 중계를 성립시키는 준비 전부를 한다. 컨텍스트 할당,
 * 초기 마스크 상태 결정, IRQ 등록이 그것이다. 순서가 까다로워 상류 주석이
 * 긴 설명을 달아 두었다.
 *
 * 동작 과정:
 *  1. 다른 모드가 켜져 있으면 거절한다. 세 모드는 배타적이다.
 *  2. IRQ 번호가 없거나 연결되지 않았으면 -ENODEV.
 *  3. /proc/interrupts 에 보일 이름을 만든다.
 *  4. 벡터 0 의 컨텍스트를 만든다.
 *  5. 이름, eventfd, 디바이스 역포인터를 채운다. 역포인터는 하드 인터럽트
 *     핸들러가 xarray 없이 디바이스를 찾기 위한 것이다.
 *  6. 초기 마스크 상태를 virq_disabled 로 정한다. 상류 주석이 이 대목을
 *     자세히 설명한다 — 사용자가 config 공간의 INTx Disable 을 미리 세워
 *     두었을 수 있고, 그 상태를 이어받아야 한다. 이 시점에는 igate 가 경합을
 *     막아 주고 IRQ 핸들러와 irqfd 가 아직 살아 있지 않아 masked 가 안정적이다.
 *     활성화 이후에는 config 의 그 비트를 바꾸는 것이 곧 INTx 마스킹이 되고,
 *     masked 는 irqlock 이 지킨다.
 *  7. PCI 2.3 이상이면 물리 INTx Disable 비트를 masked 의 반대로 세우고
 *     IRQF_SHARED 로 등록한다. 상류 주석대로 DisINTx 를 지원하는 디바이스는
 *     현재 마스크 상태를 그 물리 비트에 그대로 반영하며, 그 비트는 IRQ 설정
 *     과정에서 건드려지지 않는다. 공유가 가능한 이유는 이 함수만 조용히
 *     시킬 수 있기 때문이다.
 *     PCI 2.3 미만이면 마스킹을 IRQ 칩에서 해야 하므로 라인을 독점해야 하고,
 *     처음부터 마스크 상태라면 IRQF_NO_AUTOEN 으로 등록해 request_irq 가
 *     자동으로 라인을 켜지 않게 한다.
 *  8. **request_irq 보다 먼저 irq_type 을 설정한다.** 상류 주석이 그 이유를
 *     못 박는다 — IRQ 핸들러가 등록돼 있는 동안 irq_type 이 안정적이어야
 *     하고, 등록 직후 인터럽트가 곧바로 들어올 수 있으므로 그때 이미
 *     is_intx 가 참이어야 한다.
 *  9. PCI 2.3 미만이면 IRQ_DISABLE_UNLAZY 플래그를 세운다. 커널의 기본
 *     동작은 disable_irq 요청을 미뤄 두었다가 실제 인터럽트가 올 때 끄는
 *     것인데(lazy disable), 레벨 트리거 라인에서는 그 사이 인터럽트가
 *     반복되므로 즉시 끄도록 강제해야 한다.
 * 10. 핸들러를 등록한다. 이 순간부터 인터럽트가 들어올 수 있다.
 * 11. 실패하면 8~9단계를 되돌리고 컨텍스트를 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. 할당과 request_irq 에서 잠들 수 있다.
 *
 * 에러 경로: 롤백에서 irq_type 을 "모드 없음" 으로 되돌리고, 상태 플래그를
 * 지우고, 이름과 컨텍스트를 해제한다. trigger 의 참조는 건드리지 않고
 * 호출자(vfio_pci_set_intx_trigger)가 되돌린다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → vfio_pci_set_intx_trigger → [vfio_intx_enable]
 *     → vfio_irq_ctx_alloc, pci_intx (drivers/pci/pci.c:8201),
 *       irq_set_status_flags, request_irq
 */
static int vfio_intx_enable(struct vfio_pci_core_device *vdev,
			    struct eventfd_ctx *trigger)
{
	/* [한국어] IRQ 번호와 INTx 비트의 출처. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 만들 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;
	/* [한국어] request_irq 에 넘길 플래그. */
	unsigned long irqflags;
	/* [한국어] /proc/interrupts 에 보일 이름. */
	char *name;
	/* [한국어] 하부 호출 결과. */
	int ret;

	/* [한국어] 다른 모드가 이미 켜져 있다. 세 모드는 배타적이라 거절한다. */
	if (!is_irq_none(vdev))
		return -EINVAL;

	/* [한국어] IRQ 번호가 없거나 "연결되지 않음" 표시다. INTx 를 쓸 수 없다.
	 * vfio_pci_config.c 의 vfio_config_init 도 같은 조건에서 Interrupt Pin 을
	 * 0 으로 만들어 사용자에게 INTx 가 없는 것처럼 보이게 한다. */
	if (!pdev->irq || pdev->irq == IRQ_NOTCONNECTED)
		return -ENODEV;

	/* [한국어] "vfio-intx(0000:03:00.0)" 형태의 이름을 만든다. request_irq 가 이
	 * 포인터를 소유권 없이 참조하므로 IRQ 가 등록된 동안 살아 있어야 한다. */
	name = kasprintf(GFP_KERNEL_ACCOUNT, "vfio-intx(%s)", pci_name(pdev));
	if (!name)
		return -ENOMEM;

	/* [한국어] INTx 는 벡터가 하나뿐이라 인덱스 0. */
	ctx = vfio_irq_ctx_alloc(vdev, 0);
	if (!ctx) {
		/* [한국어] 컨텍스트 생성 실패. 방금 만든 이름을 되돌린다. */
		kfree(name);
		return -ENOMEM;
	}

	/* [한국어] 이름 소유권을 컨텍스트로 넘긴다. */
	ctx->name = name;
	/* [한국어] eventfd 참조의 소유권도 넘긴다. NULL 일 수 있다 — 사용자가 fd 를 -1 로
	 * 주면 "전달할 곳 없는 INTx" 가 된다. 여기서는 아직 IRQ 가 등록되지 않아
	 * 핸들러가 이 필드를 볼 수 없으므로 WRITE_ONCE 가 필요 없다. */
	ctx->trigger = trigger;
	/* [한국어] 하드 인터럽트 핸들러가 xarray 없이 디바이스를 찾기 위한 역포인터. */
	ctx->vdev = vdev;

	/*
	 * Fill the initial masked state based on virq_disabled.  After
	 * enable, changing the DisINTx bit in vconfig directly changes INTx
	 * masking.  igate prevents races during setup, once running masked
	 * is protected via irqlock.
	 *
	 * Devices supporting DisINTx also reflect the current mask state in
	 * the physical DisINTx bit, which is not affected during IRQ setup.
	 *
	 * Devices without DisINTx support require an exclusive interrupt.
	 * IRQ masking is performed at the IRQ chip.  Again, igate protects
	 * against races during setup and IRQ handlers and irqfds are not
	 * yet active, therefore masked is stable and can be used to
	 * conditionally auto-enable the IRQ.
	 *
	 * irq_type must be stable while the IRQ handler is registered,
	 * therefore it must be set before request_irq().
	 */
	/* [한국어] 초기 마스크 상태를 사용자의 가상 INTx Disable 값으로 정한다.
	 * 상류 주석이 그 정당성을 밝힌다 — 이 시점에는 igate 가 경합을 막고
	 * IRQ 핸들러와 irqfd 가 아직 살아 있지 않아 masked 가 안정적이다.
	 * 활성화 이후에는 config 공간의 그 비트를 바꾸는 것이 곧 INTx 마스킹이 되며
	 * masked 는 irqlock 이 지킨다. */
	ctx->masked = vdev->virq_disabled;
	/* [한국어] DisINTx 를 지원하는 디바이스. */
	if (vdev->pci_2_3) {
		/* [한국어] 물리 INTx Disable 비트를 마스크 상태의 반대로 세운다. 상류 주석대로
		 * DisINTx 를 지원하는 디바이스는 현재 마스크 상태를 그 물리 비트에 그대로
		 * 반영하며, 그 비트는 이후 IRQ 설정 과정에서 건드려지지 않는다. */
		pci_intx(pdev, !ctx->masked);
		/* [한국어] 공유 라인으로 등록한다. 이 함수만 조용히 시킬 수 있으므로 다른
		 * 디바이스와 라인을 나눠 써도 된다. */
		irqflags = IRQF_SHARED;
	} else {
		/* [한국어] DisINTx 가 없는 디바이스는 라인을 독점해야 해서 IRQF_SHARED 를 주지
		 * 않는다. 상류 주석대로 마스킹이 IRQ 칩에서 이뤄지기 때문이다.
		 * 처음부터 마스크 상태라면 IRQF_NO_AUTOEN 으로 등록해, request_irq 가
		 * 자동으로 라인을 켜지 않게 한다 — 그러지 않으면 등록 즉시 막아 두었어야 할
		 * 인터럽트가 들어온다. */
		irqflags = ctx->masked ? IRQF_NO_AUTOEN : 0;
	}

	/* [한국어] **request_irq 보다 먼저 모드를 기록한다.** 상류 주석이 그 이유를
	 * 못 박는다 — IRQ 핸들러가 등록돼 있는 동안 irq_type 이 안정적이어야 하고,
	 * 등록 직후 인터럽트가 곧바로 들어올 수 있으므로 그때 이미 is_intx 가
	 * 참이어야 한다. 그러지 않으면 첫 인터럽트가 vfio_send_intx_eventfd 의
	 * is_intx 검사에 걸려 조용히 버려진다. */
	vdev->irq_type = VFIO_PCI_INTX_IRQ_INDEX;

	/* [한국어] DisINTx 가 없는 디바이스에만 필요한 보정. */
	if (!vdev->pci_2_3)
		/* [한국어] 커널의 기본 동작은 disable_irq 요청을 미뤄 두었다가 실제 인터럽트가
		 * 올 때 끄는 것이다(lazy disable). 레벨 트리거 라인에서는 그 사이 인터럽트가
		 * 반복되므로 즉시 끄도록 강제해야 한다. */
		irq_set_status_flags(pdev->irq, IRQ_DISABLE_UNLAZY);

	/* [한국어] 핸들러를 등록한다. threadfn 을 주지 않으므로 vfio_intx_handler 는
	 * **하드 인터럽트 문맥**에서 돈다. dev_id 로 컨텍스트를 넘겨 핸들러가
	 * 조회 없이 상태에 닿게 한다. 이 호출이 성공하는 순간부터 인터럽트가
	 * 들어올 수 있다. */
	ret = request_irq(pdev->irq, vfio_intx_handler,
			  irqflags, ctx->name, ctx);
	/* [한국어] 등록 실패. 위에서 바꾼 것을 전부 되돌린다. */
	if (ret) {
		/* [한국어] 세워 두었던 IRQ 상태 플래그가 있으면 */
		if (!vdev->pci_2_3)
			/* [한국어] 지운다. 이 라인을 다른 드라이버가 쓰게 될 수 있어 원상 복구가 필요하다. */
			irq_clear_status_flags(pdev->irq, IRQ_DISABLE_UNLAZY);
		/* [한국어] 모드를 "없음" 으로 되돌린다. 이 값이 is_irq_none 의 판정 기준이다. */
		vdev->irq_type = VFIO_PCI_NUM_IRQS;
		/* [한국어] 이름 해제. */
		kfree(name);
		/* [한국어] 컨텍스트를 xarray 에서 빼고 해제한다. ctx->trigger 는 건드리지 않으며,
		 * 그 참조는 호출자 vfio_pci_set_intx_trigger 가 되돌린다. */
		vfio_irq_ctx_free(vdev, ctx, 0);
		return ret;
	}

	/* [한국어] INTx 활성화 완료. */
	return 0;
}

/* [한국어]
 * vfio_intx_set_signal - 이미 켜진 INTx 의 eventfd 를 다른 것으로 갈아 끼운다
 *
 * @vdev: 대상 디바이스.
 * @trigger: 새 eventfd 컨텍스트. NULL 이면 전달을 끊는다는 뜻이다.
 *           성공하면 참조의 소유권이 ctx 로 넘어간다.
 * @return: 0 성공, 컨텍스트가 없으면 -EINVAL(WARN 동반).
 *
 * 왜 필요한가: 사용자가 INTx 를 끄고 다시 켜지 않고도 인터럽트를 받을 fd 를
 * 바꾸고 싶을 수 있다. 문제는 하드 인터럽트 핸들러가 언제든 옛 포인터를
 * 읽는 중일 수 있다는 것이다. 옛 eventfd 의 참조를 성급히 놓으면 핸들러가
 * 해제된 객체를 신호하게 된다. 그 경합을 닫는 것이 이 함수의 전부다.
 *
 * 동작 과정:
 *  1. 벡터 0 의 컨텍스트를 얻는다. 없으면 프로그래밍 오류이므로 WARN.
 *  2. 옛 포인터를 기억한다.
 *  3. WRITE_ONCE 로 새 포인터를 심는다. vfio_send_intx_eventfd 의 READ_ONCE 와
 *     짝을 이뤄, 컴파일러가 이 저장을 쪼개거나 미루지 못하게 한다.
 *  4. 옛 것이 있었다면 세 단계로 조용히 정리한다.
 *     a. synchronize_irq 로 **진행 중인 하드 인터럽트 핸들러가 끝나기를
 *        기다린다.** 이 호출 이후에는 어떤 CPU 도 옛 포인터를 들고 있지 않다.
 *     b. vfio_virqfd_flush_thread 로 unmask virqfd 가 큐잉해 둔 워커까지
 *        끝나기를 기다린다. 그 워커도 vfio_send_intx_eventfd 를 통해 옛
 *        포인터를 쓸 수 있기 때문이다(drivers/vfio/virqfd.c:474).
 *     c. 이제 안전하게 옛 eventfd 의 참조를 놓는다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. synchronize_irq 와
 * flush_workqueue 에서 잠들 수 있어 mutex 문맥이 필수다.
 *
 * 에러 경로: 컨텍스트가 없으면 -EINVAL. 그 밖에는 실패할 수 없다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → vfio_pci_set_intx_trigger
 *     → [vfio_intx_set_signal]
 *     → synchronize_irq, vfio_virqfd_flush_thread (drivers/vfio/virqfd.c:474),
 *       eventfd_ctx_put
 */
static int vfio_intx_set_signal(struct vfio_pci_core_device *vdev,
				struct eventfd_ctx *trigger)
{
	/* [한국어] synchronize_irq 에 넘길 IRQ 번호의 출처. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 벡터 0 의 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;
	/* [한국어] 교체 전의 eventfd. 나중에 참조를 놓는다. */
	struct eventfd_ctx *old;

	/* [한국어] INTx 는 인덱스 0. */
	ctx = vfio_irq_ctx_get(vdev, 0);
	/* [한국어] INTx 가 켜져 있는데 컨텍스트가 없으면 프로그래밍 오류다. */
	if (WARN_ON_ONCE(!ctx))
		return -EINVAL;

	/* [한국어] 옛 포인터를 기억해 둔다. */
	old = ctx->trigger;

	/* [한국어] 새 포인터를 심는다. vfio_send_intx_eventfd 의 READ_ONCE 와 짝을 이뤄,
	 * 컴파일러가 이 저장을 쪼개거나 미루지 못하게 한다. 이 시점부터 새로
	 * 들어오는 인터럽트는 새 eventfd 로 간다. */
	WRITE_ONCE(ctx->trigger, trigger);

	/* Releasing an old ctx requires synchronizing in-flight users */
	/* [한국어] 옛 것이 있었으면 참조를 놓아야 하는데, 그 전에 아무도 그것을 쓰고
	 * 있지 않음을 보장해야 한다. */
	if (old) {
		/* [한국어] **진행 중인 하드 인터럽트 핸들러가 끝나기를 기다린다.** 다른 CPU 가
		 * 지금 vfio_intx_handler 안에서 옛 포인터를 들고 eventfd 를 신호하려는
		 * 중일 수 있다. 이 호출이 돌아온 뒤에는 어떤 CPU 도 옛 포인터를 들고 있지
		 * 않다. */
		synchronize_irq(pdev->irq);
		/* [한국어] unmask virqfd 가 큐잉해 둔 워커까지 끝나기를 기다린다. 그 워커도
		 * vfio_send_intx_eventfd 를 통해 옛 포인터를 쓸 수 있기 때문이다
		 * (drivers/vfio/virqfd.c:474). 슬롯은 살려 두고 진행 중인 작업만 비운다. */
		vfio_virqfd_flush_thread(&ctx->unmask);
		/* [한국어] 이제 안전하게 옛 eventfd 의 참조를 놓는다. 이것이 마지막 참조였다면
		 * 여기서 해제된다. */
		eventfd_ctx_put(old);
	}

	/* [한국어] 교체 완료. */
	return 0;
}

/* [한국어]
 * vfio_intx_disable - INTx 모드를 끄고 모든 자원을 되돌린다
 *
 * @vdev: 대상 디바이스.
 * @return: 없다. 실패할 수 있는 단계가 없다.
 *
 * 왜 필요한가: vfio_intx_enable 의 정확한 역순이다. 해제 순서가 중요한데,
 * 인터럽트를 만들어 내는 주체를 먼저 끊고 나서 그 인터럽트가 쓰던 자원을
 * 놓아야 한다.
 *
 * 동작 과정:
 *  1. 벡터 0 컨텍스트를 얻는다. 없으면 WARN 을 남기지만 아래 정리는 건너뛰고
 *     마지막의 irq_type 되돌리기는 그대로 수행한다.
 *  2. 두 개의 virqfd(마스크용과 마스크 해제용)를 먼저 해제한다. 이들이
 *     콜백을 통해 아래에서 해제할 컨텍스트를 만질 수 있으므로 가장 먼저
 *     끊어야 한다. vfio_virqfd_disable 은 워크큐를 flush 해 진행 중인 콜백이
 *     끝날 때까지 기다린다(drivers/vfio/virqfd.c:435).
 *  3. IRQ 를 해제한다. free_irq 는 진행 중인 핸들러가 끝나기를 기다리므로,
 *     이 호출이 끝난 뒤에는 vfio_intx_handler 가 돌지 않는다.
 *  4. PCI 2.3 미만이었다면 세워 둔 IRQ 상태 플래그를 지운다. 이 라인을
 *     다른 드라이버가 쓰게 될 수 있어 원상 복구가 필요하다.
 *  5. eventfd 참조를 놓고 이름과 컨텍스트를 해제한다. 3단계가 끝난 뒤이므로
 *     누구도 이 포인터들을 보고 있지 않다.
 *  6. irq_type 을 "모드 없음" 으로 되돌려 다음 모드를 켤 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. free_irq 와 워크큐 flush 에서
 * 잠들 수 있다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → vfio_pci_set_intx_trigger
 *     (또는 vfio_pci_core.c 의 vfio_pci_core_disable 이 같은 ioctl 진입점을
 *      통해) → [vfio_intx_disable]
 *     → vfio_virqfd_disable (drivers/vfio/virqfd.c:435), free_irq,
 *       irq_clear_status_flags, eventfd_ctx_put, vfio_irq_ctx_free
 */
static void vfio_intx_disable(struct vfio_pci_core_device *vdev)
{
	/* [한국어] free_irq 에 넘길 IRQ 번호의 출처. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 벡터 0 의 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;

	/* [한국어] INTx 는 인덱스 0. */
	ctx = vfio_irq_ctx_get(vdev, 0);
	/* [한국어] 없으면 프로그래밍 오류라 경고하되, 아래 정리는 조건부로 건너뛰고
	 * 마지막의 모드 되돌리기는 그대로 수행한다. */
	WARN_ON_ONCE(!ctx);
	/* [한국어] 컨텍스트가 있을 때만 자원을 정리한다. */
	if (ctx) {
		/* [한국어] **가장 먼저 virqfd 를 끊는다.** 이들이 콜백을 통해 아래에서 해제할
		 * 컨텍스트를 만질 수 있기 때문이다. 이 함수는 워크큐를 flush 해 진행 중인
		 * 콜백이 끝날 때까지 기다린다(drivers/vfio/virqfd.c:435). */
		vfio_virqfd_disable(&ctx->unmask);
		/* [한국어] 마스크용 슬롯도 정리한다. 지금은 아무도 채우지 않아 항상 NULL 이지만,
		 * 그래도 워크큐 flush 효과는 남는다. */
		vfio_virqfd_disable(&ctx->mask);
		/* [한국어] IRQ 를 해제한다. free_irq 는 진행 중인 핸들러가 끝나기를 기다리므로,
		 * 이 호출이 돌아온 뒤에는 vfio_intx_handler 가 돌지 않는다. 두 번째 인자가
		 * 등록 때 준 dev_id 와 같아야 공유 라인에서 올바른 핸들러를 골라낸다. */
		free_irq(pdev->irq, ctx);
		/* [한국어] 등록 때 IRQ 상태 플래그를 세웠던 경우에만 */
		if (!vdev->pci_2_3)
			/* [한국어] 지운다. 이 라인을 다른 드라이버가 쓰게 될 수 있어 원상 복구가 필요하다. */
			irq_clear_status_flags(pdev->irq, IRQ_DISABLE_UNLAZY);
		/* [한국어] fd -1 로 켠 경우 NULL 일 수 있다. */
		if (ctx->trigger)
			/* [한국어] eventfd 참조를 놓는다. free_irq 가 끝난 뒤라 누구도 이 포인터를
			 * 보고 있지 않다. */
			eventfd_ctx_put(ctx->trigger);
		/* [한국어] IRQ 이름 해제. free_irq 뒤여야 하는데, 커널 IRQ 코어가 등록된 동안
		 * 이 문자열을 참조하기 때문이다. */
		kfree(ctx->name);
		/* [한국어] 컨텍스트를 xarray 에서 빼고 해제한다. */
		vfio_irq_ctx_free(vdev, ctx, 0);
	}
	/* [한국어] 모드를 "없음" 으로 되돌려 다음 모드를 켤 수 있게 한다. 컨텍스트가
	 * 없었더라도 이 줄은 실행된다. */
	vdev->irq_type = VFIO_PCI_NUM_IRQS;
}

/* [한국어]
 * vfio_msihandler - MSI/MSI-X 하드 인터럽트 핸들러. eventfd 를 때리기만 한다
 *
 * @irq: 발생한 리눅스 IRQ 번호. 쓰지 않는다.
 * @arg: request_irq 에 넘긴 값, 즉 이 벡터의 eventfd 컨텍스트 포인터.
 *       INTx 판이 vfio_pci_irq_ctx 를 넘기는 것과 달리 여기서는 eventfd 를
 *       곧바로 넘긴다 — 이 핸들러가 그것 말고는 아무것도 필요로 하지 않기
 *       때문이다.
 * @return: 언제나 IRQ_HANDLED.
 *
 * 왜 이렇게 짧은가: MSI 와 MSI-X 는 메시지 시그널 방식이라 에지 트리거처럼
 * 동작한다. 인터럽트가 "계속 걸려 있는" 상태가 없으므로 마스킹이 필요 없고,
 * 벡터마다 IRQ 번호가 전용이라 공유 판정도 필요 없다. INTx 핸들러가 락을
 * 잡고 마스킹하고 소유권을 판정해야 했던 것과 극명하게 대비된다.
 * 언제나 HANDLED 를 반환할 수 있는 것도 벡터가 전용이기 때문이다.
 *
 * 동작 과정: 인자를 eventfd 로 해석해 신호한다.
 *
 * 실행 컨텍스트: **하드 인터럽트 문맥.** threadfn 없이 등록됐다.
 * eventfd_signal 은 atomic 문맥에서 호출 가능하다.
 *
 * 에러 경로: 없다. trigger 가 NULL 일 수 없는데, MSI 경로는 fd 가 음수면
 * 컨텍스트 자체를 만들지 않기 때문이다(vfio_msi_set_vector_signal).
 *
 * 호출 체인:
 *   커널 IRQ 코어 → [vfio_msihandler] → eventfd_signal
 */
/*
 * MSI/MSI-X
 */
static irqreturn_t vfio_msihandler(int irq, void *arg)
{
	/* [한국어] request_irq 에 넘긴 값이 eventfd 그 자체다. INTx 판이 컨텍스트 구조체를
	 * 넘기는 것과 달리, 이 핸들러는 eventfd 말고 아무것도 필요로 하지 않는다. */
	struct eventfd_ctx *trigger = arg;

	/* [한국어] 사용자 공간을 깨운다. MSI 는 에지 트리거처럼 동작해 마스킹이 필요
	 * 없으므로 이 한 줄이 전부다. */
	eventfd_signal(trigger);
	/* [한국어] 언제나 처리했다고 답한다. 벡터마다 IRQ 번호가 전용이라 공유 판정이
	 * 필요 없다 — 이 IRQ 가 울렸다면 반드시 이 벡터의 것이다. */
	return IRQ_HANDLED;
}

/* [한국어]
 * vfio_msi_enable - MSI 또는 MSI-X 벡터를 한꺼번에 할당해 모드를 켠다
 *
 * @vdev: 대상 디바이스.
 * @nvec: 필요한 벡터 개수.
 * @msix: true 면 MSI-X, false 면 MSI.
 * @return: 0 성공. -EINVAL 이면 이미 다른 모드가 켜져 있다. 요청한 만큼
 *          할당하지 못하면 **할당 가능한 개수를 양수로** 돌려주는데,
 *          상류 주석이 그 규약을 밝힌다. 호출자
 *          vfio_pci_set_msi_trigger 는 그 값을 그대로 사용자에게 전달해
 *          사용자가 다시 시도할 수 있게 한다.
 *
 * 왜 필요한가: MSI 벡터 할당은 PCI 코어와 IRQ 도메인이 하는 일이라 이 파일이
 * 직접 할 수 없다. 이 함수는 그 API 를 부르고, 부작용을 정리하고, 사용자에게
 * 보여 줄 벡터 수 표현을 계산한다.
 *
 * 왜 메모리 락으로 감싸는가: 벡터 할당 과정에서 PCI 코어가 MSI-X 테이블에
 * 접근한다. 그 테이블은 BAR 안의 MMIO 라, 메모리 디코딩이 꺼져 있으면 접근이
 * 실패한다. vfio_pci_memory_lock_and_enable 은 memory_lock 을 쓰기 모드로
 * 잡고 필요하면 COMMAND 의 메모리 활성 비트를 임시로 켜 주며, 짝이 되는
 * 함수가 원래 값을 되돌린다. 락을 잡는 동안 사용자의 BAR 접근이 멈추므로
 * 그 사이 테이블이 사용자에 의해 바뀌지 않는다.
 *
 * 동작 과정:
 *  1. 다른 모드가 켜져 있으면 거절한다.
 *  2. 메모리 락을 잡고 벡터를 할당한다. 최소 1개, 최대 nvec 개를 요청한다.
 *  3. nvec 보다 적게 받았으면 받은 것을 도로 반납하고 그 개수를 반환한다.
 *     상류 주석의 "전부 얻지 못하면 지원 가능한 개수를 반환한다" 가 이 대목이다.
 *  4. 모드를 기록한다.
 *  5. MSI 라면 사용자에게 보여 줄 최대 벡터 수 필드를 계산한다. 그 필드는
 *     벡터 수의 밑 2 로그(2의 거듭제곱 지수)라, nvec 를 두 배로 만든 뒤
 *     1 을 빼고 최상위 비트 위치를 구해 1 을 빼면 올림 로그가 나온다.
 *     이 값을 vfio_pci_config.c 의 MSI capability 처리가 읽어 사용자에게
 *     보고하고, 사용자가 그보다 많은 벡터를 요구하면 깎는다.
 *     MSI-X 는 벡터 수를 config 공간이 아니라 테이블 크기로 알리므로 이
 *     계산이 필요 없다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. 벡터 할당에서 잠들 수 있고
 * memory_lock 을 쓰기 모드로 잡는다.
 *
 * 에러 경로: 부족한 할당은 되돌리고 개수를 반환한다. 실패해도 irq_type 은
 * 건드리지 않으므로 디바이스는 "모드 없음" 상태로 남는다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → vfio_pci_set_msi_trigger → [vfio_msi_enable]
 *     → vfio_pci_memory_lock_and_enable,
 *       pci_alloc_irq_vectors (drivers/pci/msi/api.c:519),
 *       pci_free_irq_vectors (drivers/pci/msi/api.c:857),
 *       vfio_pci_memory_unlock_and_restore
 */
static int vfio_msi_enable(struct vfio_pci_core_device *vdev, int nvec, bool msix)
{
	/* [한국어] 벡터를 할당할 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] PCI 코어에 어느 방식으로 할당할지 알리는 플래그. 이 한 줄이 MSI 와
	 * MSI-X 를 가르는 유일한 지점이며, 나머지 코드는 공용이다. */
	unsigned int flag = msix ? PCI_IRQ_MSIX : PCI_IRQ_MSI;
	/* [한국어] 할당 결과 또는 실제 할당된 개수. */
	int ret;
	/* [한국어] 메모리 락 진입 시 저장해 둘 COMMAND 원본 값. */
	u16 cmd;

	/* [한국어] 다른 모드가 켜져 있으면 거절한다. */
	if (!is_irq_none(vdev))
		return -EINVAL;

	/* return the number of supported vectors if we can't get all: */
	/* [한국어] memory_lock 을 쓰기 모드로 잡고, 필요하면 COMMAND 의 메모리 활성
	 * 비트를 임시로 켠다. MSI-X 테이블이 BAR 안의 MMIO 라 메모리 디코딩이
	 * 꺼져 있으면 벡터 설정이 실패하기 때문이다. 원본 COMMAND 값을 돌려받아
	 * 나중에 되돌린다. 락을 쥐는 동안 사용자의 BAR 접근이 멈춰, 그 사이
	 * 테이블이 사용자에 의해 바뀌지 않는다. */
	cmd = vfio_pci_memory_lock_and_enable(vdev);
	/* [한국어] 벡터를 할당한다(drivers/pci/msi/api.c:519). 최소 1개, 최대 nvec 개를
	 * 요청하므로 부족하면 실패 대신 적은 개수를 돌려준다. */
	ret = pci_alloc_irq_vectors(pdev, 1, nvec, flag);
	/* [한국어] 요청한 만큼 받지 못했다. */
	if (ret < nvec) {
		/* [한국어] 일부라도 받았으면 되돌려야 한다. 음수 오류였다면 반납할 것이 없다. */
		if (ret > 0)
			/* [한국어] 부분 할당을 반납한다(drivers/pci/msi/api.c:857). */
			pci_free_irq_vectors(pdev);
		/* [한국어] COMMAND 를 원래대로 되돌리고 락을 푼다. */
		vfio_pci_memory_unlock_and_restore(vdev, cmd);
		/* [한국어] 상류 주석대로 "전부 얻지 못하면 지원 가능한 개수를 반환한다".
		 * 호출자가 이 양수를 그대로 사용자에게 전달해 다시 시도할 수 있게 한다. */
		return ret;
	}
	/* [한국어] 할당 성공 경로에서도 COMMAND 를 되돌리고 락을 푼다. */
	vfio_pci_memory_unlock_and_restore(vdev, cmd);

	/* [한국어] 모드를 기록한다. 이 시점부터 is_irq_none 이 거짓이 되어 다른 모드를
	 * 켤 수 없다. 아직 IRQ 핸들러가 등록되지 않았지만 igate 가 이 구간 전체를
	 * 덮으므로 중간 상태가 밖에서 보이지 않는다. */
	vdev->irq_type = msix ? VFIO_PCI_MSIX_IRQ_INDEX :
				VFIO_PCI_MSI_IRQ_INDEX;

	/* [한국어] MSI 만 config 공간에 벡터 수를 알린다. MSI-X 는 테이블 크기로 알리므로
	 * 이 계산이 필요 없다. */
	if (!msix) {
		/*
		 * Compute the virtual hardware field for max msi vectors -
		 * it is the log base 2 of the number of vectors.
		 */
		/* [한국어] 상류 주석대로 "벡터 수의 밑 2 로그" 를 구한다. MSI 의 Multiple Message
		 * 필드가 지수 형식이기 때문이다. nvec 를 두 배로 만든 뒤 1 을 빼고 최상위
		 * 비트 위치(fls)를 구해 1 을 빼면 올림 로그가 나온다 — 예컨대 nvec 가 3 이면
		 * fls(5) = 3 이므로 결과는 2 이고, 2의 2제곱인 4 개까지 쓸 수 있다고 알린다.
		 * 이 값을 vfio_pci_config.c 의 MSI 처리가 읽어 사용자에게 보고하고,
		 * 사용자가 그보다 많은 벡터를 요구하면 거기서 깎는다. */
		vdev->msi_qmax = fls(nvec * 2 - 1) - 1;
	}

	/* [한국어] 모드 활성화 완료. 실제 배선은 호출자가 이어서 한다. */
	return 0;
}

/* [한국어]
 * vfio_msi_alloc_irq - 벡터 하나의 리눅스 IRQ 번호를 얻거나, 없으면 동적으로 만든다
 *
 * @vdev: 대상 디바이스.
 * @vector: 벡터 번호.
 * @msix: MSI-X 인지 여부.
 * @return: 양수면 리눅스 IRQ 번호. 음수면 오류(-EINVAL 또는 동적 할당 실패).
 *
 * 왜 필요한가: vfio_msi_enable 이 미리 할당해 둔 벡터는 IRQ 번호를 곧바로
 * 얻을 수 있다. 그런데 MSI-X 는 나중에 벡터를 더 요구할 수 있고, 최신
 * 플랫폼은 그 동적 확장을 지원한다. 그 두 경우를 한 함수로 감춘다.
 *
 * 상류 주석이 해제 함수가 없는 이유를 밝힌다 — 한 번 할당한 인터럽트는
 * 그대로 두어 일종의 캐시로 삼고, 뒤이은 할당이 그것을 다시 쓴다. 실제
 * 해제는 MSI/MSI-X 를 끌 때 pci_free_irq_vectors 가 한꺼번에 한다.
 * 사용자가 eventfd 를 붙였다 뗐다 반복해도 벡터 할당과 해제가 되풀이되지
 * 않게 하는 최적화다.
 *
 * 동작 과정:
 *  1. 이미 할당된 IRQ 번호를 물어본다.
 *  2. 0 이 나오면 있을 수 없는 값이라 WARN 후 오류. IRQ 0 은 유효한 번호가
 *     아니다.
 *  3. 양수면 그대로 쓴다. MSI-X 가 아니거나 동적 확장을 지원하지 않는
 *     디바이스라면 음수 오류라도 그대로 돌려준다 — 더 시도할 방법이 없다.
 *  4. MSI-X 이면서 동적 확장을 지원하면 메모리 락을 잡고 벡터 하나를
 *     새로 할당한다. 메모리 락이 필요한 이유는 MSI-X 테이블이 BAR 안의
 *     MMIO 이기 때문이다.
 *  5. 결과 구조체에서 오류 코드나 IRQ 번호를 꺼내 돌려준다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. 할당에서 잠들 수 있고
 * memory_lock 을 쓰기 모드로 잡는다.
 *
 * 에러 경로: 음수를 그대로 위로 올린다. 호출자가 컨텍스트를 만들기 전에
 * 부르므로 되돌릴 것이 없다.
 *
 * 호출 체인:
 *   vfio_msi_set_vector_signal → [vfio_msi_alloc_irq]
 *     → pci_irq_vector (drivers/pci/msi/api.c:705),
 *       vfio_pci_memory_lock_and_enable,
 *       pci_msix_alloc_irq_at (drivers/pci/msi/api.c:317),
 *       vfio_pci_memory_unlock_and_restore
 */
/*
 * vfio_msi_alloc_irq() returns the Linux IRQ number of an MSI or MSI-X device
 * interrupt vector. If a Linux IRQ number is not available then a new
 * interrupt is allocated if dynamic MSI-X is supported.
 *
 * Where is vfio_msi_free_irq()? Allocated interrupts are maintained,
 * essentially forming a cache that subsequent allocations can draw from.
 * Interrupts are freed using pci_free_irq_vectors() when MSI/MSI-X is
 * disabled.
 */
static int vfio_msi_alloc_irq(struct vfio_pci_core_device *vdev,
			      unsigned int vector, bool msix)
{
	/* [한국어] 벡터를 조회하거나 할당할 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 동적 할당 결과. 인덱스(또는 오류)와 virq 를 담는다. */
	struct msi_map map;
	/* [한국어] 리눅스 IRQ 번호. */
	int irq;
	/* [한국어] 메모리 락 진입 시 저장할 COMMAND 원본. */
	u16 cmd;

	/* [한국어] 이미 할당된 벡터의 IRQ 번호를 조회한다(drivers/pci/msi/api.c:705).
	 * 아직 없으면 음수 오류가 나온다. */
	irq = pci_irq_vector(pdev, vector);
	/* [한국어] IRQ 0 은 유효한 번호가 아니다. 이 값이 나왔다면 PCI 코어 쪽에 문제가
	 * 있다는 뜻이라 경고하고 오류로 처리한다. */
	if (WARN_ON_ONCE(irq == 0))
		return -EINVAL;
	/* [한국어] 세 가지 중 하나면 여기서 끝난다 — 이미 IRQ 가 있거나(양수 그대로 반환),
	 * MSI 라 동적 확장 개념이 없거나, 이 디바이스가 동적 MSI-X 를 지원하지
	 * 않는 경우다. 뒤 두 경우에는 음수 오류가 그대로 나간다.
	 * has_dyn_msix 는 vfio_pci_core.c 의 vfio_pci_core_enable 이
	 * pci_msix_can_alloc_dyn 으로 확인해 채워 둔 값이다. */
	if (irq > 0 || !msix || !vdev->has_dyn_msix)
		return irq;

	/* [한국어] MSI-X 테이블이 BAR 안의 MMIO 라 메모리 디코딩이 켜져 있어야 한다. */
	cmd = vfio_pci_memory_lock_and_enable(vdev);
	/* [한국어] 지정한 인덱스에 벡터 하나를 새로 할당한다
	 * (drivers/pci/msi/api.c:317). 세 번째 인자가 NULL 이면 affinity 를
	 * 지정하지 않고 커널 기본값을 쓴다는 뜻이다. */
	map = pci_msix_alloc_irq_at(pdev, vector, NULL);
	/* [한국어] COMMAND 를 되돌리고 락을 푼다. */
	vfio_pci_memory_unlock_and_restore(vdev, cmd);

	/* [한국어] 실패하면 index 에 음수 오류가 담기고, 성공하면 virq 에 리눅스 IRQ
	 * 번호가 담긴다. 상류 주석이 밝히듯 여기서 만든 벡터를 되돌리는 함수는
	 * 없다 — 한 번 만든 것은 캐시처럼 남겨 두었다가 MSI-X 를 끌 때
	 * pci_free_irq_vectors 로 한꺼번에 반납한다. */
	return map.index < 0 ? map.index : map.virq;
}

/* [한국어]
 * vfio_msi_set_vector_signal - 벡터 하나에 eventfd 를 붙이거나 뗀다
 *
 * @vdev: 대상 디바이스.
 * @vector: 벡터 번호.
 * @fd: 붙일 eventfd 의 파일 디스크립터. 음수면 이 벡터의 전달을 끊으라는 뜻이다.
 * @msix: MSI-X 인지 여부.
 * @return: 0 성공. -ENOMEM, eventfd 조회 실패, request_irq 실패가 음수로 나온다.
 *
 * 왜 필요한가: 이 파일에서 가장 긴 함수이며 MSI/MSI-X 중계의 실제 배선을
 * 전부 한다. 벡터와 eventfd 의 결합은 언제든 바뀔 수 있으므로 "기존 것을
 * 완전히 끊고 새로 잇는" 구조를 취한다.
 *
 * 동작 과정:
 *  1. 이 벡터에 이미 컨텍스트가 있으면 통째로 해체한다.
 *     a. irq bypass producer 를 먼저 해제한다. KVM 이 이 eventfd 를 게스트
 *        인터럽트에 직접 연결해 두었을 수 있어, IRQ 를 놓기 전에 그 결합을
 *        끊어야 한다.
 *     b. IRQ 번호를 얻어 두고 메모리 락 아래에서 free_irq 한다. free_irq 가
 *        MSI-X 마스크 비트를 만질 수 있어 메모리 디코딩이 켜져 있어야 한다.
 *     c. 상류 주석대로 벡터 자체는 반납하지 않는다. MSI-X 를 끌 때 한꺼번에
 *        반납된다.
 *     d. 이름과 eventfd 참조를 놓고 컨텍스트를 해제한다.
 *  2. fd 가 음수였다면 여기서 끝이다. "끊기만 하라" 는 요청이었다.
 *  3. 위에서 컨텍스트가 없어 IRQ 번호를 못 얻었다면 지금 얻는다. 필요하면
 *     동적으로 새 벡터를 만든다.
 *  4. 새 컨텍스트를 만든다.
 *  5. /proc/interrupts 에 보일 이름을 만든다. MSI 와 MSI-X 를 구별하고
 *     벡터 번호와 디바이스 이름을 넣는다.
 *  6. fd 로 eventfd 컨텍스트를 얻는다. 이 참조는 성공 시 ctx 가 소유한다.
 *  7. 메모리 락을 잡고, MSI-X 라면 **디바이스의 메시지 데이터를 다시 쓴다.**
 *     상류 주석이 그 이유를 밝힌다 — 이 벡터가 전에 할당됐던 것이면 그
 *     사이에 뒷문 리셋 등으로 테이블 내용이 지워졌거나 망가졌을 수 있다.
 *     커널이 캐시해 둔 메시지를 그대로 다시 기록해 되살린다.
 *     이 두 함수가 커널의 MSI 메시지 소유권을 보여 준다 — 사용자가 config
 *     공간에 쓴 주소/데이터는 vfio_pci_config.c 가 가짜로 붙잡아 두고,
 *     하드웨어에 실제로 들어가는 값은 언제나 커널 irq 도메인의 것이다.
 *  8. 핸들러를 등록한다. dev_id 로 eventfd 를 그대로 넘겨, 하드 인터럽트
 *     핸들러가 다른 조회 없이 신호할 수 있게 한다.
 *  9. irq bypass producer 를 등록한다. KVM 이 이 eventfd 를 게스트에
 *     직통으로 잇게 해 주는 선택적 최적화라, 실패해도 정보 로그만 남기고
 *     진행한다 — 중계가 커널을 거쳐 도는 것뿐이라 기능은 온전하다.
 * 10. 마지막에 trigger 를 심는다. 이 대입이 끝나야 이 벡터가 완전히 살아난다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. free_irq, request_irq, eventfd
 * 조회에서 잠들 수 있고 memory_lock 을 쓰기 모드로 여러 번 잡는다.
 *
 * 에러 경로: 세 라벨이 역순으로 정리한다 — eventfd 참조 반납, 이름 해제,
 * 컨텍스트 해제. 1단계에서 이미 해체한 옛 결합은 되살리지 않으므로,
 * 중간에 실패하면 이 벡터는 "아무것도 붙어 있지 않은" 상태가 된다.
 * 호출자 vfio_msi_set_block 이 그 상태를 전제로 나머지 벡터도 정리한다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → vfio_pci_set_msi_trigger → vfio_msi_set_block
 *     → [vfio_msi_set_vector_signal]
 *     → irq_bypass_unregister_producer, pci_irq_vector, free_irq,
 *       vfio_irq_ctx_alloc, eventfd_ctx_fdget, get_cached_msi_msg,
 *       pci_write_msi_msg (drivers/pci/msi/msi.c:812), request_irq,
 *       irq_bypass_register_producer
 */
static int vfio_msi_set_vector_signal(struct vfio_pci_core_device *vdev,
				      unsigned int vector, int fd, bool msix)
{
	/* [한국어] IRQ 조회와 이름 생성에 쓸 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 이 벡터의 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;
	/* [한국어] 새로 붙일 eventfd. */
	struct eventfd_ctx *trigger;
	/* [한국어] IRQ 번호를 -EINVAL 로 시작한다. 아래에서 이 값이 그대로면 "아직 IRQ
	 * 번호를 모른다" 는 표시로 쓰인다 — 기존 컨텍스트가 없었다는 뜻이다. */
	int irq = -EINVAL, ret;
	/* [한국어] 메모리 락 진입 시 저장할 COMMAND 원본. */
	u16 cmd;

	/* [한국어] 기존 결합이 있는지 본다. */
	ctx = vfio_irq_ctx_get(vdev, vector);

	/* [한국어] 있으면 통째로 해체한 뒤 새로 잇는다. */
	if (ctx) {
		/* [한국어] **IRQ 를 놓기 전에 먼저 KVM 직통 결합을 끊는다.** KVM 이 이 eventfd 를
		 * 게스트 인터럽트에 직접 이어 두었을 수 있어, 순서를 바꾸면 KVM 이 사라진
		 * IRQ 를 참조하게 된다. */
		irq_bypass_unregister_producer(&ctx->producer);
		/* [한국어] free_irq 에 넘길 IRQ 번호를 얻는다. 동시에 아래에서 "이미 번호를
		 * 안다" 는 표시가 되어 재할당을 건너뛰게 한다. */
		irq = pci_irq_vector(pdev, vector);
		/* [한국어] free_irq 가 MSI-X 마스크 비트를 만질 수 있어 메모리 디코딩이 켜져
		 * 있어야 한다. */
		cmd = vfio_pci_memory_lock_and_enable(vdev);
		/* [한국어] 핸들러를 해제한다. 두 번째 인자가 등록 때 준 dev_id 와 같아야 하는데,
		 * MSI 는 eventfd 포인터를 dev_id 로 썼으므로 여기서도 그것을 넘긴다.
		 * free_irq 는 진행 중인 핸들러가 끝나기를 기다리므로, 이 뒤에는
		 * vfio_msihandler 가 이 eventfd 를 만지지 않는다. */
		free_irq(irq, ctx->trigger);
		/* [한국어] COMMAND 를 되돌리고 락을 푼다. */
		vfio_pci_memory_unlock_and_restore(vdev, cmd);
		/* Interrupt stays allocated, will be freed at MSI-X disable. */
		/* [한국어] IRQ 이름 해제. free_irq 뒤여야 커널 IRQ 코어가 참조하지 않는다. */
		kfree(ctx->name);
		/* [한국어] 옛 eventfd 참조를 놓는다. free_irq 가 끝난 뒤라 안전하다. */
		eventfd_ctx_put(ctx->trigger);
		/* [한국어] 컨텍스트를 xarray 에서 빼고 해제한다. 이 시점에서 이 벡터는 아무것도
		 * 붙어 있지 않은 상태다. */
		vfio_irq_ctx_free(vdev, ctx, vector);
	}

	/* [한국어] "끊기만 하라" 는 요청이었으면 여기서 끝이다. 상류 주석대로 벡터 자체는
	 * 남아 있고 MSI-X 를 끌 때 반납된다. */
	if (fd < 0)
		return 0;

	/* [한국어] 기존 컨텍스트가 없어 IRQ 번호를 얻지 못했다. 지금 얻어야 한다. */
	if (irq == -EINVAL) {
		/* Interrupt stays allocated, will be freed at MSI-X disable. */
		/* [한국어] 이미 할당된 벡터면 그 번호를, 없으면 동적 MSI-X 로 새로 만든다. */
		irq = vfio_msi_alloc_irq(vdev, vector, msix);
		if (irq < 0)
			return irq;
	}

	/* [한국어] 새 컨텍스트를 만든다. */
	ctx = vfio_irq_ctx_alloc(vdev, vector);
	if (!ctx)
		return -ENOMEM;

	/* [한국어] "vfio-msix[3](0000:03:00.0)" 같은 이름을 만든다. MSI 와 MSI-X 를
	 * "x" 한 글자로 구별하고 벡터 번호와 디바이스 주소를 넣어
	 * /proc/interrupts 에서 구분되게 한다. */
	ctx->name = kasprintf(GFP_KERNEL_ACCOUNT, "vfio-msi%s[%d](%s)",
			      msix ? "x" : "", vector, pci_name(pdev));
	/* [한국어] 이름 할당 실패. */
	if (!ctx->name) {
		/* [한국어] 메모리 부족으로 보고한다. */
		ret = -ENOMEM;
		goto out_free_ctx;
	}

	/* [한국어] 사용자가 준 fd 를 커널측 eventfd 컨텍스트로 바꾼다. 참조 하나를
	 * 얻으며, 성공 시 그 소유권이 ctx->trigger 로 넘어간다. */
	trigger = eventfd_ctx_fdget(fd);
	/* [한국어] fd 가 eventfd 가 아니거나 닫혔으면 오류 포인터가 온다. */
	if (IS_ERR(trigger)) {
		/* [한국어] 오류 포인터를 errno 로 바꾼다. */
		ret = PTR_ERR(trigger);
		goto out_free_name;
	}

	/*
	 * If the vector was previously allocated, refresh the on-device
	 * message data before enabling in case it had been cleared or
	 * corrupted (e.g. due to backdoor resets) since writing.
	 */
	/* [한국어] 아래 메시지 기록과 request_irq 가 MSI-X 테이블을 만지므로 메모리
	 * 디코딩이 켜져 있어야 한다. */
	cmd = vfio_pci_memory_lock_and_enable(vdev);
	/* [한국어] MSI-X 만 필요한 보정이다. */
	if (msix) {
		/* [한국어] 커널이 캐시해 둔 메시지를 담을 그릇. */
		struct msi_msg msg;

		/* [한국어] 커널 irq 도메인이 이 벡터에 대해 기록해 둔 주소/데이터를 꺼낸다. */
		get_cached_msi_msg(irq, &msg);
		/* [한국어] **같은 값을 디바이스 테이블에 다시 쓴다**(drivers/pci/msi/msi.c:812).
		 * 상류 주석이 이유를 밝힌다 — 이 벡터가 전에 할당됐던 것이면 그 사이
		 * 뒷문 리셋 등으로 테이블 내용이 지워지거나 망가졌을 수 있다.
		 * 이 두 줄이 MSI 메시지의 소유권을 그대로 보여 준다. 사용자가 config
		 * 공간에 쓴 주소/데이터는 vfio_pci_config.c 가 가짜로 붙잡아 두고,
		 * 하드웨어에 실제로 들어가는 값은 언제나 커널 irq 도메인의 것이다.
		 * 이 분리가 없으면 사용자가 임의의 호스트 인터럽트를 위조할 수 있다. */
		pci_write_msi_msg(irq, &msg);
	}

	/* [한국어] 핸들러를 등록한다. 플래그가 0 인 것은 MSI 벡터가 전용이라 공유가
	 * 필요 없기 때문이다. dev_id 로 eventfd 를 그대로 넘겨, 하드 인터럽트
	 * 핸들러가 다른 조회 없이 신호할 수 있게 한다. */
	ret = request_irq(irq, vfio_msihandler, 0, ctx->name, trigger);
	/* [한국어] COMMAND 를 되돌리고 락을 푼다. 등록 성공 여부를 보기 전에 락부터
	 * 푸는 것이 중요하다 — 실패 경로에서도 락이 남지 않는다. */
	vfio_pci_memory_unlock_and_restore(vdev, cmd);
	if (ret)
		goto out_put_eventfd_ctx;

	/* [한국어] KVM 이 이 인터럽트를 게스트에 직통으로 잇게 해 주는 선택적 등록이다.
	 * 성공하면 게스트 인터럽트가 호스트 커널을 거의 거치지 않는다. */
	ret = irq_bypass_register_producer(&ctx->producer, trigger, irq);
	/* [한국어] 실패해도 치명적이지 않다. 인터럽트가 커널을 거쳐 eventfd 로 도는
	 * 평범한 경로로 돌아갈 뿐이라 기능은 온전하다. */
	if (unlikely(ret)) {
		/* [한국어] 정보 수준 로그만 남기고 계속 진행한다. 여기서 오류를 반환하면
		 * 정상 동작할 수 있는 설정을 실패시키게 된다. */
		dev_info(&pdev->dev,
		"irq bypass producer (eventfd %p) registration fails: %d\n",
		trigger, ret);
	}
	/* [한국어] **마지막에 심는다.** 이 대입이 끝나야 이 벡터가 완전히 살아난 것으로
	 * 본다. 다만 MSI 핸들러는 이 필드를 읽지 않고 dev_id 로 받은 포인터를
	 * 쓰므로, 하드 인터럽트 문맥과의 경합을 위한 READ_ONCE/WRITE_ONCE 는
	 * 필요하지 않다. 이 필드는 해제 경로와 루프백 신호 경로가 쓴다. */
	ctx->trigger = trigger;

	/* [한국어] 벡터 배선 완료. */
	return 0;

out_put_eventfd_ctx:
	/* [한국어] request_irq 실패 경로. 방금 얻은 eventfd 참조를 되돌린다. */
	eventfd_ctx_put(trigger);
out_free_name:
	/* [한국어] 이름 해제. */
	kfree(ctx->name);
out_free_ctx:
	/* [한국어] 컨텍스트를 되돌린다. 이 벡터는 아무것도 붙어 있지 않은 상태로 남으며,
	 * 호출자 vfio_msi_set_block 이 그 상태를 전제로 나머지도 정리한다. */
	vfio_irq_ctx_free(vdev, ctx, vector);
	/* [한국어] 실패 원인을 위로 올린다. */
	return ret;
}

/* [한국어]
 * vfio_msi_set_block - 연속된 벡터 구간에 eventfd 배열을 한꺼번에 붙인다
 *
 * @vdev: 대상 디바이스.
 * @start: 시작 벡터 번호.
 * @count: 벡터 개수.
 * @fds: fd 배열. NULL 이면 전부 -1 로 간주해 해당 구간을 끊는다.
 * @msix: MSI-X 인지 여부.
 * @return: 0 성공, 아니면 첫 실패의 오류 코드.
 *
 * 왜 필요한가: 사용자 ioctl 은 한 번에 여러 벡터를 설정한다. 그 반복과,
 * 도중에 실패했을 때의 롤백을 담당한다.
 *
 * 동작 과정:
 *  1. i 는 fds 배열 인덱스, j 는 실제 벡터 번호로 나란히 전진한다. 두
 *     인덱스가 다른 이유는 사용자가 벡터 3번부터 5개를 설정하는 식으로
 *     요청할 수 있기 때문이다. 오류가 나면 조건식의 !ret 이 거짓이 되어
 *     루프가 끝난다.
 *  2. fds 가 NULL 이면 -1 을 넘겨 그 벡터를 끊는다.
 *  3. 실패했으면 start 부터 j 직전까지 되돌린다. 루프를 빠져나올 때 j 가
 *     이미 한 번 더 증가했으므로 이 구간에는 실패한 벡터 자신도 포함되는데,
 *     그 벡터는 이미 아무것도 붙어 있지 않은 상태라 끊기 요청이 컨텍스트를
 *     찾지 못하고 조용히 성공한다. 즉 과잉 롤백이 무해하다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래.
 *
 * 에러 경로: 위 3단계의 부분 롤백. 이미 성공한 벡터를 모두 끊어 "이 요청
 * 전체가 없던 일" 이 되게 한다. 다만 vfio_msi_enable 로 할당한 벡터 자체는
 * 남으며, 그것은 호출자가 vfio_msi_disable 로 되돌린다.
 *
 * 호출 체인:
 *   vfio_pci_set_msi_trigger → [vfio_msi_set_block]
 *     → vfio_msi_set_vector_signal
 */
static int vfio_msi_set_block(struct vfio_pci_core_device *vdev, unsigned start,
			      unsigned count, int32_t *fds, bool msix)
{
	/* [한국어] i 는 fds 배열 안의 인덱스, j 는 실제 벡터 번호. 사용자가 "벡터 3번부터
	 * 5개" 처럼 요청할 수 있어 두 인덱스가 다르다. */
	unsigned int i, j;
	/* [한국어] 첫 실패의 오류 코드. */
	int ret = 0;

	/* [한국어] 두 인덱스를 나란히 전진시킨다. 조건식의 !ret 덕분에 한 번 실패하면
	 * 루프가 곧바로 끝난다. 다만 증감식은 그 전에 한 번 더 실행되므로,
	 * 빠져나올 때 j 는 실패한 벡터의 다음을 가리킨다. */
	for (i = 0, j = start; i < count && !ret; i++, j++) {
		/* [한국어] fds 가 NULL 이면 "이 구간을 전부 끊어라" 는 뜻이므로 -1 을 넘긴다. */
		int fd = fds ? fds[i] : -1;
		/* [한국어] 벡터 하나를 배선하거나 끊는다. */
		ret = vfio_msi_set_vector_signal(vdev, j, fd, msix);
	}

	/* [한국어] 도중에 실패했다. */
	if (ret) {
		/* [한국어] start 부터 j 직전까지 되돌린다. 위에서 본 대로 j 가 이미 한 번 더
		 * 증가했으므로 이 구간에 실패한 벡터 자신도 포함되는데, 그 벡터는 이미
		 * 아무것도 붙어 있지 않은 상태라 끊기 요청이 컨텍스트를 찾지 못하고
		 * 조용히 성공한다 — 과잉 롤백이 무해하다. */
		for (i = start; i < j; i++)
			/* [한국어] 이미 성공한 벡터의 결합을 끊어 "이 요청 전체가 없던 일" 이 되게 한다. */
			vfio_msi_set_vector_signal(vdev, i, -1, msix);
	}

	/* [한국어] 첫 실패의 오류 코드, 또는 0. */
	return ret;
}

/* [한국어]
 * vfio_msi_disable - MSI 또는 MSI-X 모드를 끄고 모든 벡터를 반납한다
 *
 * @vdev: 대상 디바이스.
 * @msix: 끄려는 것이 MSI-X 인지 여부.
 * @return: 없다.
 *
 * 왜 필요한가: vfio_msi_enable 과 vfio_msi_set_vector_signal 이 만든 모든
 * 것을 되돌린다. 순서가 중요하다 — 각 벡터의 IRQ 등록을 먼저 끊고 나서
 * 벡터 묶음 자체를 반납해야 한다.
 *
 * 동작 과정:
 *  1. xarray 에 남은 모든 컨텍스트를 순회하며, 벡터마다 두 virqfd 를 해제하고
 *     eventfd 결합을 끊는다. 끊기 호출이 그 컨텍스트를 xarray 에서 지우고
 *     해제하는데, xarray 순회는 진행 중 삭제를 견디도록 설계돼 있다.
 *  2. 메모리 락 아래에서 벡터 묶음을 통째로 반납한다. MSI-X 테이블을 만지므로
 *     메모리 디코딩이 켜져 있어야 한다.
 *  3. 상류 주석이 마지막 보정의 근거를 밝힌다 — MSI 와 MSI-X 를 끄는 두
 *     경로 모두 내부에서 DisINTx 비트를 지운다(drivers/pci/msi/msi.c:848 의
 *     pci_intx_for_msi). MSI 를 쓰던 동안 INTx 를 막아 두었던 것을 풀어
 *     주는 정상 동작이지만, INTx 자체가 고장 나 커널이 영구히 막아 둔
 *     디바이스에는 재앙이다. 그런 디바이스는 여기서 다시 막아 준다.
 *  4. 모드를 "없음" 으로 되돌려 다음 모드를 켤 수 있게 한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. free_irq 와 워크큐 flush,
 * 벡터 반납에서 잠들 수 있고 memory_lock 을 쓰기 모드로 잡는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → vfio_pci_set_msi_trigger → [vfio_msi_disable]
 *     → vfio_virqfd_disable (drivers/vfio/virqfd.c:435),
 *       vfio_msi_set_vector_signal, vfio_pci_memory_lock_and_enable,
 *       pci_free_irq_vectors (drivers/pci/msi/api.c:857),
 *       vfio_pci_memory_unlock_and_restore, pci_intx (drivers/pci/pci.c:8201)
 */
static void vfio_msi_disable(struct vfio_pci_core_device *vdev, bool msix)
{
	/* [한국어] 벡터를 반납할 PCI 함수. */
	struct pci_dev *pdev = vdev->pdev;
	/* [한국어] 순회 중 현재 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;
	/* [한국어] 순회 중 현재 벡터 번호. */
	unsigned long i;
	/* [한국어] 메모리 락 진입 시 저장할 COMMAND 원본. */
	u16 cmd;

	/* [한국어] 살아 있는 모든 벡터 컨텍스트를 순회한다. 본문에서 컨텍스트를
	 * xarray 에서 지우는데, xarray 순회는 진행 중 삭제를 견디도록 설계돼 있다. */
	xa_for_each(&vdev->ctx, i, ctx) {
		/* [한국어] virqfd 를 먼저 끊는다. 콜백이 아래에서 해제할 컨텍스트를 만질 수
		 * 있기 때문이다. MSI 경로에서 이 슬롯이 채워지는 일은 없지만
		 * (vfio_pci_set_irqs_ioctl 이 MSI 의 UNMASK 액션을 지원하지 않는다)
		 * INTx 와 같은 정리 절차를 밟는다. */
		vfio_virqfd_disable(&ctx->unmask);
		/* [한국어] 마스크용 슬롯도 정리한다. */
		vfio_virqfd_disable(&ctx->mask);
		/* [한국어] 이 벡터의 IRQ 등록과 eventfd 결합을 끊고 컨텍스트를 해제한다. */
		vfio_msi_set_vector_signal(vdev, i, -1, msix);
	}

	/* [한국어] 벡터 반납이 MSI-X 테이블을 만지므로 메모리 디코딩이 켜져 있어야 한다. */
	cmd = vfio_pci_memory_lock_and_enable(vdev);
	/* [한국어] 벡터 묶음을 통째로 반납한다(drivers/pci/msi/api.c:857). 동적으로
	 * 추가된 MSI-X 벡터도 여기서 함께 사라진다 —
	 * vfio_msi_alloc_irq 의 상류 주석이 예고한 일괄 반납 지점이다. */
	pci_free_irq_vectors(pdev);
	/* [한국어] COMMAND 를 되돌리고 락을 푼다. */
	vfio_pci_memory_unlock_and_restore(vdev, cmd);

	/*
	 * Both disable paths above use pci_intx_for_msi() to clear DisINTx
	 * via their shutdown paths.  Restore for NoINTx devices.
	 */
	/* [한국어] INTx 가 고장 나 커널이 영구히 막아 둔 디바이스인지 본다. */
	if (vdev->nointx)
		/* [한국어] 상류 주석이 근거를 밝힌다 — MSI 와 MSI-X 를 끄는 두 경로 모두
		 * 내부에서 DisINTx 비트를 지운다(drivers/pci/msi/msi.c:848 의
		 * pci_intx_for_msi). MSI 를 쓰는 동안 막아 두었던 INTx 를 풀어 주는 정상
		 * 동작이지만, INTx 자체가 고장 난 디바이스에는 재앙이다. 그래서 여기서
		 * 다시 막는다. 인자 0 이 "INTx 를 끈다" 는 뜻이다. */
		pci_intx(pdev, 0);

	/* [한국어] 모드를 "없음" 으로 되돌려 다음 모드를 켤 수 있게 한다. */
	vdev->irq_type = VFIO_PCI_NUM_IRQS;
}

/* [한국어]
 * vfio_pci_set_intx_unmask - INTx UNMASK 액션 ioctl 핸들러
 *
 * @vdev: 대상 디바이스.
 * @index: IRQ 인덱스. 디스패처가 이미 INTx 로 확인했으므로 쓰지 않는다.
 * @start: 시작 벡터. INTx 는 벡터가 하나뿐이라 0 이어야 한다.
 * @count: 벡터 개수. 1 이어야 한다.
 * @flags: 데이터 형식(NONE / BOOL / EVENTFD)과 액션 종류 비트.
 * @data: 사용자에게서 복사해 온 데이터. 형식에 따라 해석이 다르다.
 * @return: 0 성공, -EINVAL 이면 인자가 잘못됐다. EVENTFD 경로는
 *          vfio_virqfd_enable 의 오류를 그대로 올린다.
 *
 * 왜 필요한가: 사용자가 INTx 를 처리한 뒤 다음 인터럽트를 받으려면 마스크를
 * 풀어야 한다. 그 요청을 세 가지 방식으로 받을 수 있다.
 *  - DATA_NONE: "지금 풀어라". 가장 단순하다.
 *  - DATA_BOOL: 바이트 하나가 0 이 아닐 때만 푼다. 여러 인덱스를 한 ioctl 로
 *    다룰 때의 선택 수단이다.
 *  - DATA_EVENTFD: **fd 를 등록해 두고 사용자가 그 fd 에 쓰면 자동으로 풀린다.**
 *    ioctl 왕복 없이 인터럽트 재무장을 할 수 있어 지연이 크게 준다. 이것이
 *    virqfd 기구가 존재하는 이유다.
 *
 * 동작 과정:
 *  1. INTx 모드이고 벡터 지정이 (0, 1)인지 확인한다.
 *  2. NONE 이면 곧바로 푼다.
 *  3. BOOL 이면 값을 보고 참일 때만 푼다.
 *  4. EVENTFD 이면 fd 가 유효할 때 virqfd 를 등록한다. handler 로
 *     vfio_pci_intx_unmask_handler 를, thread 로 vfio_send_intx_eventfd 를
 *     준다. 그 2단계 구조가 필요한 이유는 handler 가 atomic 문맥에서 돌기
 *     때문이다. fd 가 음수면 등록해 둔 것을 해제한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래(호출 전에 vfio_pci_core.c 가 잡는다).
 *
 * 에러 경로: 인자 오류는 -EINVAL. virqfd 등록 실패는 그대로 전달된다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → [vfio_pci_set_intx_unmask]
 *     → __vfio_pci_intx_unmask, vfio_irq_ctx_get,
 *       vfio_virqfd_enable (drivers/vfio/virqfd.c:337),
 *       vfio_virqfd_disable (drivers/vfio/virqfd.c:435)
 */
/*
 * IOCTL support
 */
static int vfio_pci_set_intx_unmask(struct vfio_pci_core_device *vdev,
				    unsigned index, unsigned start,
				    unsigned count, uint32_t flags, void *data)
{
	/* [한국어] INTx 모드여야 하고, 벡터 지정이 정확히 (0, 1)이어야 한다. INTx 는
	 * 벡터가 하나뿐이므로 그 밖의 조합은 의미가 없다. */
	if (!is_intx(vdev) || start != 0 || count != 1)
		return -EINVAL;

	/* [한국어] 데이터 없이 "지금 풀어라" 는 요청. */
	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		/* [한국어] igate 는 vfio_pci_core.c 가 이미 잡아 주었으므로 밑줄 두 개짜리 판을
		 * 직접 부른다. */
		__vfio_pci_intx_unmask(vdev);
	/* [한국어] 바이트 하나로 풀지 말지 고르는 형식. */
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		/* [한국어] 사용자 데이터의 첫 바이트를 읽는다. 이 버퍼는 호출자가 memdup_user 로
		 * 복사해 온 커널 메모리라 여기서 다시 검증할 필요가 없다. */
		uint8_t unmask = *(uint8_t *)data;
		/* [한국어] 0 이 아닐 때만 푼다. */
		if (unmask)
			/* [한국어] 마스크를 푼다. */
			__vfio_pci_intx_unmask(vdev);
	/* [한국어] **fd 를 등록해 두고 사용자가 그 fd 에 쓰면 자동으로 풀리게 하는 형식.**
	 * ioctl 왕복 없이 인터럽트를 재무장할 수 있어 지연이 크게 준다. */
	} else if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		/* [한국어] virqfd 슬롯이 이 컨텍스트 안에 있으므로 먼저 얻는다. */
		struct vfio_pci_irq_ctx *ctx = vfio_irq_ctx_get(vdev, 0);
		/* [한국어] 사용자가 준 fd. */
		int32_t fd = *(int32_t *)data;

		/* [한국어] INTx 모드인데 컨텍스트가 없으면 프로그래밍 오류다. */
		if (WARN_ON_ONCE(!ctx))
			return -EINVAL;
		/* [한국어] 유효한 fd 면 등록한다. */
		if (fd >= 0)
			/* [한국어] virqfd 를 등록한다(drivers/vfio/virqfd.c:337). handler 로
			 * vfio_pci_intx_unmask_handler 를, thread 로 vfio_send_intx_eventfd 를 준다.
			 * 그 2단계 구조가 필요한 이유는 handler 가 eventfd wait queue 락을 쥔
			 * atomic 문맥에서 돌기 때문이다 — 거기서 같은 eventfd 를 신호하면 교착한다.
			 * data 로 ctx 를 넘겨 두 콜백이 컨텍스트에 닿게 하고, 슬롯 주소로
			 * &ctx->unmask 를 준다. */
			return vfio_virqfd_enable((void *) vdev,
						  vfio_pci_intx_unmask_handler,
						  vfio_send_intx_eventfd, ctx,
						  &ctx->unmask, fd);

		/* [한국어] fd 가 음수면 등록해 둔 것을 해제한다. 워크큐를 flush 해 진행 중인
		 * 콜백이 끝날 때까지 기다린다(drivers/vfio/virqfd.c:435). */
		vfio_virqfd_disable(&ctx->unmask);
	}

	return 0;
}

/* [한국어]
 * vfio_pci_set_intx_mask - INTx MASK 액션 ioctl 핸들러
 *
 * @vdev: 대상 디바이스.
 * @index: 쓰지 않는다(디스패처가 이미 확인).
 * @start: 0 이어야 한다.
 * @count: 1 이어야 한다.
 * @flags: 데이터 형식과 액션 비트.
 * @data: 형식에 따른 사용자 데이터.
 * @return: 0 성공, -EINVAL 이면 인자 오류, -ENOTTY 면 미구현 형식.
 *
 * 왜 필요한가: 사용자가 명시적으로 INTx 를 잠재우고 싶을 때 쓴다. 마스크
 * 해제와 대칭이지만 한 가지가 빠져 있다 — eventfd 로 마스킹하는 기능이
 * 구현돼 있지 않고, 상류 코드가 "XXX implement me" 주석과 함께 -ENOTTY 를
 * 돌려준다. 마스킹은 인터럽트 처리 직후 커널이 자동으로 해 주므로 사용자가
 * 비동기로 요청할 실익이 적어 미뤄진 것으로 보인다.
 *
 * 동작 과정:
 *  1. INTx 모드이고 벡터 지정이 (0, 1)인지 확인한다.
 *  2. NONE 이면 곧바로 마스크한다.
 *  3. BOOL 이면 값이 참일 때만 마스크한다.
 *  4. EVENTFD 는 미구현이라 -ENOTTY.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래.
 *
 * 에러 경로: 인자 오류 -EINVAL, 미구현 형식 -ENOTTY.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → [vfio_pci_set_intx_mask]
 *     → __vfio_pci_intx_mask
 */
static int vfio_pci_set_intx_mask(struct vfio_pci_core_device *vdev,
				  unsigned index, unsigned start,
				  unsigned count, uint32_t flags, void *data)
{
	/* [한국어] 마스크 해제와 같은 인자 검증. */
	if (!is_intx(vdev) || start != 0 || count != 1)
		return -EINVAL;

	/* [한국어] "지금 마스크하라" 는 요청. */
	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		/* [한국어] igate 는 이미 잡혀 있으므로 실행부를 직접 부른다. */
		__vfio_pci_intx_mask(vdev);
	/* [한국어] 바이트로 고르는 형식. */
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		/* [한국어] 사용자 데이터의 첫 바이트. */
		uint8_t mask = *(uint8_t *)data;
		/* [한국어] 0 이 아닐 때만 마스크한다. */
		if (mask)
			/* [한국어] 마스크한다. */
			__vfio_pci_intx_mask(vdev);
	/* [한국어] eventfd 로 마스킹하는 형식은 구현돼 있지 않다. */
	} else if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		/* [한국어] 상류 코드가 미구현임을 주석으로 밝히고 -ENOTTY 를 돌려준다.
		 * 마스킹은 인터럽트 처리 직후 커널이 자동으로 해 주므로 사용자가 비동기로
		 * 요청할 실익이 적어 미뤄진 것으로 보인다. ctx->mask 슬롯이 구조체에
		 * 자리만 잡고 언제나 NULL 인 이유가 이것이다. */
		return -ENOTTY; /* XXX implement me */
	}

	return 0;
}

/* [한국어]
 * vfio_pci_set_intx_trigger - INTx TRIGGER 액션 ioctl 핸들러. INTx 를 켜고 끄고 강제 발생시킨다
 *
 * @vdev: 대상 디바이스.
 * @index: 쓰지 않는다.
 * @start: 0 이어야 한다(끄기 요청은 예외).
 * @count: 1 이어야 한다. 0 이면 "끄기" 를 뜻한다.
 * @flags: 데이터 형식과 액션 비트.
 * @data: EVENTFD 면 fd 하나, BOOL 이면 바이트 하나.
 * @return: 0 성공, -EINVAL 이면 인자 오류나 모드 충돌, 그 밖에 하부 오류.
 *
 * 왜 필요한가: TRIGGER 액션은 세 가지 전혀 다른 일을 한 진입점에서 처리한다.
 *  (1) count 가 0 이고 DATA_NONE 이면 INTx 를 통째로 끈다. 이것이 VFIO 의
 *      "인터럽트 끄기" 관례이며, vfio_pci_core.c 의 vfio_pci_core_disable 이
 *      디바이스를 닫을 때 이 조합으로 현재 모드를 정리한다.
 *  (2) DATA_EVENTFD 면 eventfd 를 붙인다. 아직 꺼져 있으면 켜면서 붙이고,
 *      이미 켜져 있으면 갈아 끼운다.
 *  (3) DATA_NONE 이나 DATA_BOOL 이면 **인터럽트를 소프트웨어로 흉내 낸다.**
 *      하드웨어가 인터럽트를 올리지 않아도 사용자에게 신호를 보내 준다.
 *      사용자 공간 드라이버 테스트에 쓰는 루프백이다.
 *
 * 동작 과정:
 *  1. 끄기 요청이면 즉시 끄고 끝낸다.
 *  2. 현재 INTx 이거나 아무 모드도 아닐 때만 진행한다. MSI 가 켜져 있는데
 *     INTx 를 만지려 하면 거절이다. 벡터 지정도 (0, 1)이어야 한다.
 *  3. EVENTFD 경로: fd 가 0 이상이면 eventfd 참조를 얻는다. 음수면 NULL 인
 *     채로 진행해 "전달 없는 INTx" 를 만든다.
 *     이미 INTx 면 갈아 끼우고, 아니면 새로 켠다. 실패하면 방금 얻은 eventfd
 *     참조를 되돌린다 — 성공한 경우에만 소유권이 넘어가기 때문이다.
 *  4. 여기까지 왔는데 INTx 가 아니면(예: 아무 모드도 없는데 NONE 으로
 *     트리거를 요청) 거절한다.
 *  5. NONE 이면 그냥 신호를 보내고, BOOL 이면 값이 참일 때만 보낸다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래.
 *
 * 에러 경로: 인자 오류와 모드 충돌은 -EINVAL. eventfd 조회 실패는
 * PTR_ERR 로 변환된다. 켜기/갈아 끼우기 실패 시 eventfd 참조를 되돌린다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → [vfio_pci_set_intx_trigger]
 *     → vfio_intx_disable, eventfd_ctx_fdget, vfio_intx_set_signal,
 *       vfio_intx_enable, vfio_send_intx_eventfd, vfio_irq_ctx_get
 */
static int vfio_pci_set_intx_trigger(struct vfio_pci_core_device *vdev,
				     unsigned index, unsigned start,
				     unsigned count, uint32_t flags, void *data)
{
	/* [한국어] count 가 0 이고 데이터가 없으면 "INTx 를 통째로 꺼라" 는 뜻이다.
	 * VFIO 인터럽트 ABI 의 관례이며, vfio_pci_core.c 의 vfio_pci_core_disable 이
	 * 디바이스를 닫을 때 바로 이 조합으로 현재 모드를 정리한다. */
	if (is_intx(vdev) && !count && (flags & VFIO_IRQ_SET_DATA_NONE)) {
		/* [한국어] 모든 자원을 되돌리고 모드를 비운다. */
		vfio_intx_disable(vdev);
		return 0;
	}

	/* [한국어] 현재 INTx 이거나 아무 모드도 아닐 때만 진행한다. MSI 가 켜져 있는데
	 * INTx 를 만지려 하면 모드 배타성 위반이라 거절한다. 벡터 지정도 (0, 1)이어야
	 * 한다. */
	if (!(is_intx(vdev) || is_irq_none(vdev)) || start != 0 || count != 1)
		return -EINVAL;

	/* [한국어] eventfd 를 붙이거나 갈아 끼우는 경로. */
	if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		/* [한국어] NULL 로 시작한다. fd 가 음수면 이 값 그대로 넘겨 "전달할 곳 없는
		 * INTx" 를 만든다. */
		struct eventfd_ctx *trigger = NULL;
		/* [한국어] 사용자가 준 fd. */
		int32_t fd = *(int32_t *)data;
		/* [한국어] 하부 호출 결과. */
		int ret;

		/* [한국어] 유효한 fd 면 커널측 참조를 얻는다. */
		if (fd >= 0) {
			/* [한국어] 참조 하나를 얻는다. 성공 경로에서는 이 소유권이 컨텍스트로 넘어간다. */
			trigger = eventfd_ctx_fdget(fd);
			/* [한국어] fd 가 eventfd 가 아니거나 닫혔다. */
			if (IS_ERR(trigger))
				/* [한국어] 오류 포인터를 errno 로 바꿔 반환한다. */
				return PTR_ERR(trigger);
		}

		/* [한국어] 이미 켜져 있으면 eventfd 만 갈아 끼운다. */
		if (is_intx(vdev))
			/* [한국어] 진행 중인 핸들러와 워커가 옛 포인터를 놓을 때까지 기다린 뒤 교체한다. */
			ret = vfio_intx_set_signal(vdev, trigger);
		else
			/* [한국어] 아직 꺼져 있으면 컨텍스트를 만들고 IRQ 를 등록하며 켠다. */
			ret = vfio_intx_enable(vdev, trigger);

		/* [한국어] 실패했고 참조를 얻었었다면 되돌려야 한다. 성공한 경우에만 소유권이
		 * 넘어가는 규약이라 여기서 균형을 맞춘다. */
		if (ret && trigger)
			/* [한국어] 참조 반납. */
			eventfd_ctx_put(trigger);

		/* [한국어] 결과를 그대로 전달한다. */
		return ret;
	}

	/* [한국어] 여기부터는 소프트웨어 루프백 경로다. INTx 가 켜져 있지 않으면 신호를
	 * 보낼 곳이 없어 거절한다 — 예컨대 아무 모드도 없는데 NONE 으로 트리거를
	 * 요청한 경우다. */
	if (!is_intx(vdev))
		return -EINVAL;

	/* [한국어] 조건 없이 인터럽트를 흉내 낸다. 하드웨어가 인터럽트를 올리지 않아도
	 * 사용자에게 신호를 보내 준다 — 사용자 공간 드라이버 테스트용이다. */
	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		/* [한국어] 벡터 0 의 컨텍스트를 즉석에서 찾아 신호한다. INTx 가 켜져 있음을
		 * 바로 위에서 확인했으므로 컨텍스트가 반드시 존재한다. */
		vfio_send_intx_eventfd(vdev, vfio_irq_ctx_get(vdev, 0));
	/* [한국어] 바이트 하나로 보낼지 고르는 형식. */
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		/* [한국어] 사용자 데이터의 첫 바이트. 위쪽 EVENTFD 경로의 동명 변수와 이름이
		 * 같지만 블록 스코프가 달라 서로 무관하다. */
		uint8_t trigger = *(uint8_t *)data;
		/* [한국어] 0 이 아닐 때만 보낸다. */
		if (trigger)
			/* [한국어] 신호를 보낸다. */
			vfio_send_intx_eventfd(vdev, vfio_irq_ctx_get(vdev, 0));
	}
	return 0;
}

/* [한국어]
 * vfio_pci_set_msi_trigger - MSI/MSI-X TRIGGER 액션 ioctl 핸들러. 벡터를 켜고 끄고 강제 발생시킨다
 *
 * @vdev: 대상 디바이스.
 * @index: MSI 인덱스인지 MSI-X 인덱스인지. 이 값으로 두 모드를 구별한다.
 * @start: 시작 벡터 번호.
 * @count: 벡터 개수. 0 이면 "끄기".
 * @flags: 데이터 형식과 액션 비트.
 * @data: EVENTFD 면 fd 배열, BOOL 이면 바이트 배열.
 * @return: 0 성공, -EINVAL 이면 모드 충돌, 그 밖에 하부 오류.
 *          벡터가 부족하면 vfio_msi_enable 이 낸 **양수 개수**가 그대로
 *          올라가 사용자에게 전달된다.
 *
 * 왜 필요한가: INTx 판과 같은 세 가지 일을 벡터 여러 개에 대해 한다.
 * MSI 와 MSI-X 를 같은 코드로 처리하며 index 로만 구별한다.
 *
 * 동작 과정:
 *  1. index 로 MSI-X 여부를 정한다.
 *  2. 현재 그 모드이고 count 가 0 이며 DATA_NONE 이면 통째로 끈다.
 *  3. 현재 그 모드이거나 아무 모드도 아닐 때만 진행한다.
 *  4. EVENTFD 경로:
 *     - 이미 그 모드면 벡터 구간만 다시 배선한다.
 *     - 아니면 먼저 start + count 개의 벡터를 할당해 모드를 켜고, 이어서
 *       배선한다. 배선이 실패하면 방금 켠 모드를 통째로 되돌린다 —
 *       부분적으로 켜진 상태를 남기지 않기 위해서다.
 *       할당 요청 개수가 count 가 아니라 start + count 인 이유는, MSI 벡터가
 *       0번부터 연속으로 할당되기 때문이다. 사용자가 3번부터 2개를 원해도
 *       하드웨어에는 0~4번 다섯 개가 필요하다.
 *  5. 여기까지 왔는데 그 모드가 아니면 거절한다.
 *  6. NONE 이나 BOOL 이면 지정 구간의 각 벡터에 대해 소프트웨어로 신호를
 *     보낸다. 컨텍스트가 없는 벡터는 건너뛴다 — 사용자가 아직 eventfd 를
 *     붙이지 않은 벡터일 수 있다. BOOL 은 배열 인덱스를 start 기준 상대
 *     위치로 계산한다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래.
 *
 * 에러 경로: 모드 충돌은 -EINVAL. 벡터 할당 실패는 개수나 오류로 전달된다.
 * 배선 실패 시 모드 전체를 되돌리므로 중간 상태가 남지 않는다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → [vfio_pci_set_msi_trigger]
 *     → vfio_msi_disable, vfio_msi_set_block, vfio_msi_enable,
 *       vfio_irq_ctx_get, eventfd_signal
 */
static int vfio_pci_set_msi_trigger(struct vfio_pci_core_device *vdev,
				    unsigned index, unsigned start,
				    unsigned count, uint32_t flags, void *data)
{
	/* [한국어] 루프백 경로에서 쓸 벡터별 컨텍스트. */
	struct vfio_pci_irq_ctx *ctx;
	/* [한국어] 루프백 경로의 벡터 번호. */
	unsigned int i;
	/* [한국어] MSI 와 MSI-X 를 인덱스로 구별한다. 아래 코드는 이 한 값으로만 갈린다. */
	bool msix = (index == VFIO_PCI_MSIX_IRQ_INDEX);

	/* [한국어] count 가 0 이고 데이터가 없으면 "이 모드를 통째로 꺼라" 는 뜻이다.
	 * INTx 판과 같은 관례다. */
	if (irq_is(vdev, index) && !count && (flags & VFIO_IRQ_SET_DATA_NONE)) {
		/* [한국어] 벡터를 전부 반납하고 모드를 비운다. */
		vfio_msi_disable(vdev, msix);
		return 0;
	}

	/* [한국어] 이미 그 모드이거나 아무 모드도 아닐 때만 진행한다. INTx 가 켜져 있거나
	 * MSI 가 켜진 채로 MSI-X 를 요구하면 거절이다. */
	if (!(irq_is(vdev, index) || is_irq_none(vdev)))
		return -EINVAL;

	/* [한국어] fd 배열로 벡터들을 배선하는 경로. */
	if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		/* [한국어] 사용자 데이터를 fd 배열로 해석한다. 길이 검증은 호출자
		 * (vfio_pci_core.c 의 vfio_pci_ioctl_set_irqs)가 이미 마쳤다. */
		int32_t *fds = data;
		/* [한국어] 하부 호출 결과. */
		int ret;

		/* [한국어] 이미 그 모드면 벡터 묶음은 그대로 두고 배선만 다시 한다. */
		if (vdev->irq_type == index)
			/* [한국어] 지정 구간의 벡터들을 다시 잇는다. */
			return vfio_msi_set_block(vdev, start, count,
						  fds, msix);

		/* [한국어] 아직 꺼져 있으면 먼저 벡터를 할당한다. 요청 개수가 count 가 아니라
		 * start + count 인 이유는 MSI 벡터가 0번부터 연속으로 할당되기 때문이다 —
		 * 사용자가 3번부터 2개를 원해도 하드웨어에는 0~4번 다섯 개가 필요하다. */
		ret = vfio_msi_enable(vdev, start + count, msix);
		if (ret)
			return ret;

		/* [한국어] 모드를 켠 뒤 지정 구간을 배선한다. */
		ret = vfio_msi_set_block(vdev, start, count, fds, msix);
		/* [한국어] 배선이 실패했다. */
		if (ret)
			/* [한국어] 방금 켠 모드를 통째로 되돌린다. 부분적으로 켜진 상태를 남기지 않기
			 * 위해서다 — 그런 상태로 두면 사용자가 다시 시도할 때 모드 충돌로 거절된다. */
			vfio_msi_disable(vdev, msix);

		return ret;
	}

	/* [한국어] 여기부터는 소프트웨어 루프백 경로다. 그 모드가 아니면 신호를 보낼
	 * 곳이 없다. */
	if (!irq_is(vdev, index))
		return -EINVAL;

	/* [한국어] 지정 구간의 벡터들을 순회한다. */
	for (i = start; i < start + count; i++) {
		/* [한국어] 이 벡터의 컨텍스트를 얻는다. */
		ctx = vfio_irq_ctx_get(vdev, i);
		/* [한국어] 아직 eventfd 를 붙이지 않은 벡터일 수 있다. 오류가 아니라 건너뛴다. */
		if (!ctx)
			continue;
		/* [한국어] 조건 없이 모든 지정 벡터에 신호를 보낸다. */
		if (flags & VFIO_IRQ_SET_DATA_NONE) {
			/* [한국어] 인터럽트를 흉내 낸다. MSI 경로에서는 컨텍스트가 있으면 trigger 가
			 * 반드시 채워져 있다 — fd 가 음수면 vfio_msi_set_vector_signal 이 컨텍스트
			 * 자체를 만들지 않기 때문에, INTx 쪽과 달리 NULL 검사가 필요 없다. */
			eventfd_signal(ctx->trigger);
		/* [한국어] 바이트 배열로 벡터마다 보낼지 고르는 형식. */
		} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
			/* [한국어] 사용자 데이터를 바이트 배열로 해석한다. */
			uint8_t *bools = data;
			/* [한국어] 배열 인덱스를 start 기준 상대 위치로 계산한다. i 는 절대 벡터 번호라
			 * 그대로 쓰면 배열 밖을 읽는다. */
			if (bools[i - start])
				/* [한국어] 참이면 신호를 보낸다. */
				eventfd_signal(ctx->trigger);
		}
	}
	return 0;
}

/* [한국어]
 * vfio_pci_set_ctx_trigger_single - eventfd 슬롯 하나(ERR 또는 REQ)를 설정하거나 신호한다
 *
 * @vdev: 대상 디바이스.
 * @peventfd: 대상 슬롯의 주소. vdev->err_trigger 또는 vdev->req_trigger 이며,
 *            둘 다 RCU 로 보호되는 포인터라 __rcu 표시가 붙어 있다.
 * @count: 0 이면 "해제", 1 이면 "설정하거나 신호".
 * @flags: 데이터 형식(NONE / BOOL / EVENTFD).
 * @data: 형식에 따른 사용자 데이터.
 * @return: 0 성공, -EINVAL 이면 인자나 상태가 맞지 않는다, 그 밖에 하부 오류.
 *
 * 왜 필요한가: ERR 과 REQ 는 하드웨어 인터럽트가 아니라 커널 내부 사건의
 * 알림 채널이다. ERR 은 AER 오류를, REQ 는 "호스트가 이 디바이스를 돌려받고
 * 싶다" 는 요청을 사용자에게 알린다. 벡터도 마스킹도 없고 eventfd 하나씩만
 * 있으면 되므로, 앞의 복잡한 컨텍스트 기구를 쓰지 않고 이 짧은 공용 함수를
 * 두 인덱스가 나눠 쓴다.
 *
 * 왜 RCU 인가: 이 두 eventfd 는 인터럽트 문맥이나 다른 스레드가 igate 를
 * 잡지 않고 신호해야 할 수 있다(AER 복구 콜백, 호스트 드라이버 언바인드
 * 요청 등). 그래서 읽는 쪽은 rcu_read_lock 아래에서 포인터를 보고, 쓰는
 * 쪽은 igate 를 잡고 교체한 뒤 유예 기간이 지나서야 옛 것을 놓는다.
 * 그 교체 함수가 vfio_pci_core.c 의 vfio_pci_eventfd_replace_locked 다.
 * 이 함수 안에서 rcu_dereference_protected 에 lockdep_is_held(&vdev->igate)
 * 를 주는 것은 "지금은 쓰기 측 락을 쥐고 있으니 RCU 읽기 보호가 없어도
 * 된다" 를 lockdep 에 알리는 표시다.
 *
 * 동작 과정: 상류 주석이 밝히듯 NONE 과 BOOL 은 루프백 테스트용이다.
 *  - DATA_NONE: 슬롯을 꺼내 본다. 비어 있으면 -EINVAL. count 가 1 이면
 *    신호를 보내고, 0 이면 슬롯을 비운다(교체 함수에 NULL 을 준다).
 *  - DATA_BOOL: count 가 0 이면 인자 오류. 값이 참이면 슬롯이 있을 때 신호한다.
 *    NONE 과 달리 슬롯이 비어 있어도 오류가 아니다.
 *  - DATA_EVENTFD: count 가 0 이면 인자 오류. fd 가 -1 이면 슬롯을 비우고,
 *    0 이상이면 새 eventfd 를 얻어 교체한다. 교체가 실패하면 방금 얻은
 *    참조를 되돌린다.
 *  - 어느 형식도 아니면 -EINVAL.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래. 교체 함수가 할당을 하므로
 * 잠들 수 있다.
 *
 * 에러 경로: 위 각 갈래의 -EINVAL 과, eventfd 조회 실패의 PTR_ERR,
 * 교체 실패 시의 참조 반납.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → vfio_pci_set_err_trigger 또는
 *   vfio_pci_set_req_trigger → [vfio_pci_set_ctx_trigger_single]
 *     → rcu_dereference_protected, eventfd_signal, eventfd_ctx_fdget,
 *       vfio_pci_eventfd_replace_locked (vfio_pci_core.c),
 *       eventfd_ctx_put
 */
static int vfio_pci_set_ctx_trigger_single(struct vfio_pci_core_device *vdev,
					   struct vfio_pci_eventfd __rcu **peventfd,
					   unsigned int count, uint32_t flags,
					   void *data)
{
	/* DATA_NONE/DATA_BOOL enables loopback testing */
	/* [한국어] 상류 주석이 밝히듯 NONE 과 BOOL 은 루프백 테스트용이다. 사용자가
	 * 스스로 알림을 만들어 자기 처리 경로를 시험할 수 있게 해 준다. */
	if (flags & VFIO_IRQ_SET_DATA_NONE) {
		/* [한국어] 슬롯에 담긴 RCU 껍데기. eventfd 참조와 rcu_head 를 함께 들고 있어,
		 * 교체할 때 유예 기간이 지난 뒤 안전하게 해제할 수 있다
		 * (include/linux/vfio_pci_core.h:33). */
		struct vfio_pci_eventfd *eventfd;

		/* [한국어] RCU 로 보호되는 포인터를 읽는다. protected 판을 쓰면서
		 * lockdep_is_held(&vdev->igate) 를 주는 것은 "지금은 쓰기 측 락을 쥐고
		 * 있으니 RCU 읽기 보호가 없어도 된다" 를 lockdep 에 알리는 표시다.
		 * igate 는 호출자인 vfio_pci_core.c 가 잡아 주었다. */
		eventfd = rcu_dereference_protected(*peventfd,
						lockdep_is_held(&vdev->igate));

		/* [한국어] 슬롯이 비어 있으면 신호를 보낼 곳도, 비울 것도 없다. */
		if (!eventfd)
			return -EINVAL;

		/* [한국어] count 가 1 이면 신호를 보내라는 뜻이다. */
		if (count) {
			/* [한국어] 루프백 알림을 보낸다. */
			eventfd_signal(eventfd->ctx);
			/* [한국어] 신호 전송 완료. */
			return 0;
		}

		/* [한국어] count 가 0 이면 슬롯을 비우라는 뜻이다. NULL 로 교체하면 옛 껍데기가
		 * RCU 유예 기간 뒤에 해제되고 eventfd 참조도 그때 반납된다. 그 구현은
		 * vfio_pci_core.c 에 있다. */
		return vfio_pci_eventfd_replace_locked(vdev, peventfd, NULL);
	/* [한국어] 바이트 하나로 보낼지 고르는 형식. */
	} else if (flags & VFIO_IRQ_SET_DATA_BOOL) {
		/* [한국어] 사용자 데이터의 첫 바이트를 담을 곳. */
		uint8_t trigger;

		/* [한국어] BOOL 형식은 데이터가 있어야 의미가 있다. count 가 0 이면 인자 오류다. */
		if (!count)
			return -EINVAL;

		/* [한국어] 바이트를 읽는다. */
		trigger = *(uint8_t *)data;

		/* [한국어] 0 이 아닐 때만 신호를 보낸다. */
		if (trigger) {
			/* [한국어] 여기서도 igate 보호 아래 슬롯을 읽는다. */
			struct vfio_pci_eventfd *eventfd =
					rcu_dereference_protected(*peventfd,
					lockdep_is_held(&vdev->igate));

			/* [한국어] NONE 형식과 달리 슬롯이 비어 있어도 오류가 아니다. 조용히 넘어간다. */
			if (eventfd)
				/* [한국어] 루프백 알림을 보낸다. */
				eventfd_signal(eventfd->ctx);
		}

		/* [한국어] BOOL 경로 완료. */
		return 0;
	/* [한국어] 실제로 알림을 받을 fd 를 등록하는 형식. */
	} else if (flags & VFIO_IRQ_SET_DATA_EVENTFD) {
		/* [한국어] 사용자가 준 fd. */
		int32_t fd;

		/* [한국어] EVENTFD 형식도 데이터가 있어야 한다. */
		if (!count)
			return -EINVAL;

		/* [한국어] fd 를 읽는다. */
		fd = *(int32_t *)data;
		/* [한국어] -1 은 "등록을 해제하라" 는 관례다. 그 밖의 음수는 아래 조건에도
		 * 걸리지 않아 함수 끝의 -EINVAL 로 떨어진다. */
		if (fd == -1) {
			/* [한국어] 슬롯을 비운다. */
			return vfio_pci_eventfd_replace_locked(vdev,
							       peventfd, NULL);
		/* [한국어] 유효한 fd 면 등록한다. */
		} else if (fd >= 0) {
			/* [한국어] 커널측 eventfd 참조. */
			struct eventfd_ctx *efdctx;
			/* [한국어] 교체 결과. */
			int ret;

			/* [한국어] 참조 하나를 얻는다. */
			efdctx = eventfd_ctx_fdget(fd);
			/* [한국어] fd 가 eventfd 가 아니거나 닫혔다. */
			if (IS_ERR(efdctx))
				/* [한국어] 오류 포인터를 errno 로 바꾼다. */
				return PTR_ERR(efdctx);

			/* [한국어] 새 껍데기를 만들어 슬롯에 심고, 옛 것은 RCU 유예 뒤 해제되게 한다. */
			ret = vfio_pci_eventfd_replace_locked(vdev,
							      peventfd, efdctx);
			/* [한국어] 교체 실패(대개 껍데기 할당 실패). */
			if (ret)
				/* [한국어] 방금 얻은 참조를 되돌린다. 성공한 경우에만 소유권이 껍데기로 넘어간다. */
				eventfd_ctx_put(efdctx);

			/* [한국어] 교체 결과를 전달한다. */
			return ret;
		}
	}

	/* [한국어] 어느 데이터 형식에도 해당하지 않거나, fd 가 -1 도 아니고 0 이상도
	 * 아닌 값이었다. 알 수 없는 요청이므로 거절한다. */
	return -EINVAL;
}

/* [한국어]
 * vfio_pci_set_err_trigger - ERR 인덱스 TRIGGER 액션 ioctl 핸들러
 *
 * @vdev: 대상 디바이스.
 * @index: 반드시 ERR 인덱스여야 한다. 디스패처가 이미 확인했지만 한 번 더 본다.
 * @start: 0 이어야 한다.
 * @count: 1 이하여야 한다. 채널이 하나뿐이라 여러 개를 요구할 수 없다.
 * @flags: 데이터 형식.
 * @data: 형식에 따른 사용자 데이터.
 * @return: 0 성공, -EINVAL 이면 인자 오류, 그 밖에 공용 함수의 오류.
 *
 * 왜 필요한가: AER 오류 알림 채널을 설정한다. 이 eventfd 가 붙어 있으면
 * PCI 오류 복구 콜백이 사용자에게 "네 디바이스에 오류가 났다" 고 알릴 수
 * 있고, 사용자 드라이버가 게스트에 그 사실을 전파할 수 있다. 디스패처가
 * PCIe 디바이스일 때만 이 핸들러를 고르는데, AER 자체가 PCIe 기능이기
 * 때문이다.
 *
 * 동작 과정: 인자를 검증하고 err_trigger 슬롯을 대상으로 공용 함수를 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래.
 *
 * 에러 경로: 인자 오류는 -EINVAL, 나머지는 공용 함수가 낸 값 그대로.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → [vfio_pci_set_err_trigger]
 *     → vfio_pci_set_ctx_trigger_single
 */
static int vfio_pci_set_err_trigger(struct vfio_pci_core_device *vdev,
				    unsigned index, unsigned start,
				    unsigned count, uint32_t flags, void *data)
{
	/* [한국어] 디스패처가 이미 인덱스를 확인했지만 한 번 더 본다. 채널이 하나뿐이라
	 * start 는 0, count 는 0 또는 1 이어야 한다. */
	if (index != VFIO_PCI_ERR_IRQ_INDEX || start != 0 || count > 1)
		return -EINVAL;

	/* [한국어] AER 오류 알림 슬롯을 대상으로 공용 함수를 부른다. 이 eventfd 가 붙어
	 * 있으면 PCI 오류 복구 경로가 사용자에게 "네 디바이스에 오류가 났다" 고
	 * 알릴 수 있다. */
	return vfio_pci_set_ctx_trigger_single(vdev, &vdev->err_trigger,
					       count, flags, data);
}

/* [한국어]
 * vfio_pci_set_req_trigger - REQ 인덱스 TRIGGER 액션 ioctl 핸들러
 *
 * @vdev: 대상 디바이스.
 * @index: 반드시 REQ 인덱스여야 한다.
 * @start: 0 이어야 한다.
 * @count: 1 이하여야 한다.
 * @flags: 데이터 형식.
 * @data: 형식에 따른 사용자 데이터.
 * @return: 0 성공, -EINVAL 이면 인자 오류, 그 밖에 공용 함수의 오류.
 *
 * 왜 필요한가: "디바이스를 돌려 달라" 는 요청 채널이다. 호스트 관리자가
 * 이 PCI 함수를 vfio-pci 에서 떼어 내려 할 때, 커널은 사용자 프로세스를
 * 강제로 죽이는 대신 이 eventfd 를 신호해 자발적인 반납을 요청한다.
 * ERR 과 달리 PCIe 여부를 따지지 않는데, 반납 요청은 버스 종류와 무관하기
 * 때문이다.
 *
 * 동작 과정: 인자를 검증하고 req_trigger 슬롯을 대상으로 공용 함수를 부른다.
 *
 * 실행 컨텍스트: 프로세스 문맥, igate 아래.
 *
 * 에러 경로: ERR 판과 같다.
 *
 * 호출 체인:
 *   vfio_pci_set_irqs_ioctl → [vfio_pci_set_req_trigger]
 *     → vfio_pci_set_ctx_trigger_single
 */
static int vfio_pci_set_req_trigger(struct vfio_pci_core_device *vdev,
				    unsigned index, unsigned start,
				    unsigned count, uint32_t flags, void *data)
{
	/* [한국어] ERR 판과 같은 인자 검증. */
	if (index != VFIO_PCI_REQ_IRQ_INDEX || start != 0 || count > 1)
		return -EINVAL;

	/* [한국어] "디바이스를 돌려 달라" 는 요청 슬롯을 대상으로 공용 함수를 부른다.
	 * 호스트 관리자가 이 PCI 함수를 vfio-pci 에서 떼어 내려 할 때, 커널이
	 * 사용자 프로세스를 강제로 죽이는 대신 이 eventfd 를 신호해 자발적인
	 * 반납을 요청한다. */
	return vfio_pci_set_ctx_trigger_single(vdev, &vdev->req_trigger,
					       count, flags, data);
}

/* [한국어]
 * vfio_pci_set_irqs_ioctl - VFIO_DEVICE_SET_IRQS 의 디스패처. 인덱스와 액션으로 핸들러를 고른다
 *
 * @vdev: 대상 디바이스.
 * @flags: ioctl 헤더의 플래그. 액션 종류(MASK / UNMASK / TRIGGER)와 데이터
 *         형식(NONE / BOOL / EVENTFD)이 함께 들어 있다.
 * @index: IRQ 인덱스(INTx / MSI / MSI-X / ERR / REQ).
 * @start: 시작 벡터 번호.
 * @count: 벡터 개수.
 * @data: 사용자에게서 복사해 온 데이터 버퍼. 호출자가 memdup_user 로 만들고
 *        돌아간 뒤 해제한다.
 * @return: 고른 핸들러의 반환값. 조합이 지원되지 않으면 -ENOTTY.
 *
 * 왜 필요한가: VFIO 인터럽트 ABI 는 (인덱스 x 액션) 2차원 표다. 그 표를
 * 중첩 switch 로 펼쳐 함수 포인터 하나를 고른다. 인덱스별로 지원하는 액션이
 * 다르다는 점이 표의 핵심이다.
 *  - INTx: MASK, UNMASK, TRIGGER 셋 다.
 *  - MSI 와 MSI-X: TRIGGER 만. MASK 와 UNMASK 는 case 는 있지만 아무것도
 *    고르지 않아 -ENOTTY 가 되고, 상류 주석이 "XXX Need masking support
 *    exported" 라며 그 이유를 밝힌다 — 커널 IRQ 코어가 벡터별 마스킹을
 *    드라이버에 노출하지 않기 때문이다. 다만 MSI 의 마스크 레지스터는
 *    vfio_pci_config.c 의 MSI 권한 표에서 하드웨어 직통 쓰기로 열려 있어,
 *    사용자는 config 공간을 통해 직접 마스킹할 수 있다.
 *  - ERR: TRIGGER 만, 그것도 PCIe 디바이스일 때만. AER 이 PCIe 기능이기
 *    때문이다.
 *  - REQ: TRIGGER 만.
 *
 * 동작 과정:
 *  1. 함수 포인터를 NULL 로 시작한다. 표에서 짝을 찾지 못하면 그대로 남는다.
 *  2. 인덱스로 바깥 switch, 액션 비트로 안쪽 switch 를 돈다.
 *  3. 아무것도 고르지 못했으면 -ENOTTY 로 "이 조합은 지원하지 않는다" 를
 *     알린다. **모든 슬롯 사용 전에 이 검사가 한 번 있어 NULL 호출이 없다.**
 *  4. 고른 핸들러를 부른다. 인자 순서가 헷갈리기 쉬운데, 이 함수는
 *     (flags, index, start, count) 순으로 받고 핸들러에는
 *     (index, start, count, flags) 순으로 넘긴다.
 *
 * 실행 컨텍스트: 프로세스 문맥. **호출자인 vfio_pci_core.c 의
 * vfio_pci_ioctl_set_irqs 가 igate 를 잡은 뒤 부른다.** 그래서 이 함수와
 * 아래 모든 핸들러는 락을 직접 잡지 않으며, irq_type 을 안심하고 읽고 쓴다.
 * vfio_pci_core.c 의 vfio_pci_core_disable 도 같은 진입점을 통해 들어온다.
 *
 * 에러 경로: 지원하지 않는 조합은 -ENOTTY, 나머지는 핸들러의 반환값 그대로.
 *
 * 호출 체인:
 *   사용자 ioctl(VFIO_DEVICE_SET_IRQS) → vfio_pci_core.c 의
 *   vfio_pci_core_ioctl → vfio_pci_ioctl_set_irqs (igate 획득)
 *     → [vfio_pci_set_irqs_ioctl]
 *     → vfio_pci_set_intx_mask / _intx_unmask / _intx_trigger /
 *       _msi_trigger / _err_trigger / _req_trigger
 */
int vfio_pci_set_irqs_ioctl(struct vfio_pci_core_device *vdev, uint32_t flags,
			    unsigned index, unsigned start, unsigned count,
			    void *data)
{
	/* [한국어] 고를 핸들러를 담을 함수 포인터. NULL 로 시작해, 표에서 짝을 찾지
	 * 못하면 그대로 남아 아래에서 -ENOTTY 가 된다. */
	int (*func)(struct vfio_pci_core_device *vdev, unsigned index,
		    unsigned start, unsigned count, uint32_t flags,
		    void *data) = NULL;

	/* [한국어] 바깥 switch — IRQ 인덱스로 가른다. */
	switch (index) {
	/* [한국어] INTx 는 세 액션을 모두 지원하는 유일한 인덱스다. */
	case VFIO_PCI_INTX_IRQ_INDEX:
		/* [한국어] 안쪽 switch — 액션 종류 비트만 남겨 가른다. 같은 flags 에 데이터 형식
		 * 비트도 들어 있으므로 마스크로 걸러야 한다. */
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		/* [한국어] INTx 마스크 요청. */
		case VFIO_IRQ_SET_ACTION_MASK:
			/* [한국어] 마스크 핸들러를 고른다. */
			func = vfio_pci_set_intx_mask;
			break;
		/* [한국어] INTx 마스크 해제 요청. */
		case VFIO_IRQ_SET_ACTION_UNMASK:
			/* [한국어] 마스크 해제 핸들러를 고른다. */
			func = vfio_pci_set_intx_unmask;
			break;
		/* [한국어] INTx 켜기/끄기/강제 발생 요청. */
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			/* [한국어] 트리거 핸들러를 고른다. */
			func = vfio_pci_set_intx_trigger;
			break;
		}
		break;
	/* [한국어] MSI 와 */
	case VFIO_PCI_MSI_IRQ_INDEX:
	/* [한국어] MSI-X 는 같은 핸들러를 쓴다. 두 모드의 차이는 그 핸들러 안에서
	 * index 로 판별한다. */
	case VFIO_PCI_MSIX_IRQ_INDEX:
		/* [한국어] 액션으로 가른다. */
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		/* [한국어] MSI 계열의 마스크 요청. 아래 UNMASK 와 함께 case 만 있고 핸들러를
		 * 고르지 않아 -ENOTTY 로 떨어진다. */
		case VFIO_IRQ_SET_ACTION_MASK:
		/* [한국어] MSI 계열의 마스크 해제 요청. 상류 주석의 "XXX Need masking support
		 * exported" 가 이유를 밝힌다 — 커널 IRQ 코어가 벡터별 마스킹을 드라이버에
		 * 노출하지 않는다. 다만 MSI 의 마스크 레지스터는 vfio_pci_config.c 의 MSI
		 * 권한 표에서 하드웨어 직통 쓰기로 열려 있어, 사용자는 config 공간을 통해
		 * 직접 마스킹할 수 있다. */
		case VFIO_IRQ_SET_ACTION_UNMASK:
			/* XXX Need masking support exported */
			break;
		/* [한국어] MSI 계열이 지원하는 유일한 액션. */
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			/* [한국어] MSI/MSI-X 트리거 핸들러를 고른다. */
			func = vfio_pci_set_msi_trigger;
			break;
		}
		break;
	/* [한국어] AER 오류 알림 채널. */
	case VFIO_PCI_ERR_IRQ_INDEX:
		/* [한국어] 액션으로 가른다. */
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		/* [한국어] 트리거만 지원한다. */
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			/* [한국어] PCIe 디바이스일 때만 이 채널을 연다. AER 자체가 PCIe 기능이라
			 * 전통 PCI 디바이스에는 오류 보고 구조가 없다. */
			if (pci_is_pcie(vdev->pdev))
				/* [한국어] ERR 핸들러를 고른다. */
				func = vfio_pci_set_err_trigger;
			break;
		}
		break;
	/* [한국어] 디바이스 반납 요청 채널. */
	case VFIO_PCI_REQ_IRQ_INDEX:
		/* [한국어] 액션으로 가른다. */
		switch (flags & VFIO_IRQ_SET_ACTION_TYPE_MASK) {
		/* [한국어] 트리거만 지원한다. ERR 과 달리 PCIe 여부를 따지지 않는데, 반납 요청은
		 * 버스 종류와 무관하기 때문이다. */
		case VFIO_IRQ_SET_ACTION_TRIGGER:
			/* [한국어] REQ 핸들러를 고른다. */
			func = vfio_pci_set_req_trigger;
			break;
		}
		break;
	}

	/* [한국어] **모든 슬롯 사용 전에 이 검사가 한 번 있어 NULL 호출이 없다.**
	 * 표에서 짝을 찾지 못한 (인덱스, 액션) 조합이 여기로 온다. */
	if (!func)
		/* [한국어] "이 조합은 지원하지 않는다". 사용자가 능력 질의 없이 시도했을 때
		 * 받는 표준 응답이다. */
		return -ENOTTY;

	/* [한국어] 고른 핸들러를 부른다. 인자 순서가 헷갈리기 쉬운데, 이 함수는
	 * (flags, index, start, count) 순으로 받고 핸들러에는
	 * (index, start, count, flags) 순으로 넘긴다. */
	return func(vdev, index, start, count, flags, data);
}
