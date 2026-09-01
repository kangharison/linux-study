// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Express Downstream Port Containment services driver
 * Author: Keith Busch <keith.busch@intel.com>
 *
 * Copyright (C) 2016 Intel Corp.
 */

/*
 * [한국어 설명] 고장 난 하위 장치를 링크째 격리하는 방어선 (dpc.c)
 *
 * === 파일의 역할 ===
 * DPC(Downstream Port Containment)는 하류 포트가 자기 아래에서 치명적
 * 오류를 감지했을 때, 그 링크를 하드웨어적으로 즉시 끊어 버리는 기능이다.
 * 소프트웨어가 개입하기 전에 하드웨어가 먼저 차단하는 것이 핵심이다.
 *
 * 왜 필요한가. 고장 난 장치는 잘못된 트랜잭션을 계속 쏟아 낼 수 있다.
 * 그것이 위로 올라가면 Root Complex 가 감당하지 못해 시스템 전체가
 * 멈추거나, 잘못된 데이터가 메모리에 써진다. 특히 surprise removal
 * (장치를 예고 없이 뽑는 것)은 링크가 끊기는 순간 진행 중이던 모든
 * 트랜잭션이 완료되지 못해 Completion Timeout 이 쏟아진다.
 * DPC 는 그 순간 링크를 Off 로 만들어 사태를 그 포트에서 끝낸다.
 *
 * 격리 후의 처리가 이 파일의 나머지다.
 *   1) 인터럽트로 알린다(DPC Interrupt).
 *   2) 무엇 때문에 트리거됐는지 읽는다(DPC Status 의 Trigger Reason —
 *      Unmasked Uncorrectable Error, ERR_NONFATAL, ERR_FATAL, RP PIO 등).
 *   3) RP PIO 오류라면 상세 로그를 남긴다. Root Port 가 하류로 보낸
 *      Programmed I/O 가 실패한 경우이며, 어느 TLP 였는지까지 기록된다.
 *   4) pcie_do_recovery() 로 복구 절차를 넘긴다.
 *   5) 복구의 리셋 단계에서 dpc_reset_link() 가 불려 링크를 다시 살린다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록:  portdrv 가 DPC 서비스를 가진 포트마다 이 드라이버를 바인딩
 *          -> dpc_probe() -> IRQ 등록, DPC Control 의 Enable 비트 설정
 *
 * 발생:  하위에서 치명적 오류
 *          -> 하드웨어가 링크를 Off (여기까지는 소프트웨어 개입 없음)
 *          -> DPC 인터럽트 -> [이 파일] dpc_irq() -> dpc_handler()(스레드)
 *             -> dpc_process_error() 로 원인을 읽고 로그
 *             -> pcie_do_recovery() [err.c] 로 복구 절차 시작
 *                -> 리셋 단계에서 [이 파일] dpc_reset_link() 가 불린다
 *
 * 실행 컨텍스트: dpc_irq() 는 하드 IRQ(원인만 읽고 스레드를 깨운다),
 * dpc_handler() 는 스레드 문맥(로그와 복구 절차, 잠들 수 있다).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/portdrv.c 가 이 드라이버를 서비스로 등록한다. 어떤 포트에
 *   DPC 서비스를 붙일지는 portdrv.c:373 이 pcie_ports_dpc_native 와
 *   AER 서비스 보유 여부로 정한다.
 *   probe.c 는 열거 때 pci_dpc_init() 을 불러 capability 를 미리 찾아 둔다.
 *   pci.c:3538 의 pci_restore_state() 경로가 pci_restore_dpc_state() 를
 *   부르고, 저장 쪽은 pci.c:3083 근처의 pci_save_state() 경로다.
 * 아래쪽: pcie/err.c 의 pcie_do_recovery().
 *   pcie/aer.c 의 aer_get_device_error_info() / aer_print_error() /
 *   pci_aer_clear_nonfatal_status() / pci_aer_clear_fatal_status() /
 *   pci_aer_raw_clear_status() — 같은 AER capability 레지스터를 읽으므로
 *   출력과 정리 코드를 그대로 재사용한다.
 *   pcie/tlp.c 의 dpc_tlp_log_len()(tlp.c:97), pcie_read_tlp_log(),
 *   pcie_print_tlp_log() — RP PIO 의 TLP 헤더 로그를 읽고 찍는다.
 *   pci.c 의 pcie_wait_for_link(), pci_bridge_wait_for_secondary_bus().
 * 옆쪽: pcie/edr.c — 펌웨어가 DPC 를 소유한 경우, 커널이 직접 다루지 않고
 *   ACPI 알림을 통해 처리한다. 그 경로가 edr.c 다. 소유권 판정에 쓰이는
 *   host->native_dpc 는 probe.c:1770 에서 기본값 1 로 세워지며, 그것을
 *   0 으로 낮추는 _OSC 협상 코드는 이 스파스 체크아웃에 없다
 *   (drivers/acpi 가 통째로 빠져 있다).
 *   hotplug — pci_dpc_recovered() 가 그쪽에 "이 Link Down/Up 은 DPC 가
 *   만든 것이니 무시하라" 고 알려 준다(위 원문 kernel-doc 참조).
 * 공유 상태: struct pci_dev 의 dpc_cap(capability 오프셋),
 *   dpc_rp_extensions / dpc_rp_log_size(RP PIO 로그 관련 능력),
 *   priv_flags 의 PCI_DPC_RECOVERING / PCI_DPC_RECOVERED 두 비트
 *   (drivers/pci/pci.h:1893-1894 에 정의).
 *   struct pci_dev 의 선언은 include/linux/pci.h 에 있고 이 트리에 없어,
 *   그 필드들의 존재는 이 파일의 사용례로만 확인했다.
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_dpc_init()       : __init. DPC 포트 서비스 드라이버를 등록한다.
 * dpc_probe()           : 포트에 DPC 서비스를 붙인다. IRQ 를 등록하고
 *                         dpc_enable() 로 Control 의 Enable 과 인터럽트를 켠다.
 * dpc_remove()/dpc_suspend()/dpc_resume()
 *                       : 각각 dpc_disable() / dpc_disable() / dpc_enable().
 * dpc_enable()/dpc_disable()
 *                       : DPC Control 의 트리거와 인터럽트 비트를 켜고 끈다.
 *                         enable 쪽은 먼저 묵은 Interrupt Status 를 지운다.
 * pci_dpc_init()        : 열거 때 DPC capability 오프셋을 찾고, RP 확장이
 *                         있으면 RP PIO 로그 크기를 계산해 둔다.
 * dpc_irq()             : 하드 IRQ 핸들러. DPC Status 를 읽어 자기 인터럽트인지
 *                         확인하고, 트리거까지 섰으면 스레드를 깨운다.
 * dpc_handler()         : 스레드 핸들러. surprise removal 이면 조용히 정리하고,
 *                         아니면 오류를 기록한 뒤 복구를 시작한다.
 * dpc_is_surprise_removal() / dpc_handle_surprise_removal() / 
 * pci_clear_surpdn_errors()
 *                       : 예고 없는 장치 제거를 알아보고, 그때 딸려 오는
 *                         오류 비트들을 오류로 취급하지 않고 지운다.
 * dpc_process_error()   : Trigger Reason 을 해석해 로그를 남긴다.
 * dpc_get_aer_uncorrect_severity() : AER 상태에서 이 오류가 fatal 인지
 *                         non-fatal 인지 판정해 aer_err_info 를 채운다.
 * dpc_process_rp_pio_error() : RP PIO 오류의 상세 로그(어느 TLP 였는지).
 * dpc_reset_link()      : 복구의 리셋 단계에서 불린다. 링크가 내려가기를
 *                         기다린 뒤 DPC Trigger Status 를 지워 포트를 DPC 에서
 *                         빠져나오게 하고, 하위 버스가 다시 서기를 기다린다.
 * dpc_wait_rp_inactive() : Root Port 가 아직 바쁜지(RP Busy) 최대 1초 기다린다.
 * pci_dpc_recovered()   : DPC 복구가 끝났는지 알려 준다. 핫플러그 드라이버가
 *                         DPC 가 만든 Link Down/Up 을 무시하는 데 쓴다.
 * dpc_completed()       : 그 대기의 조건 — 트리거가 내려갔고 복구 중도 아닌가.
 * pci_save_dpc_state() / pci_restore_dpc_state()
 *                       : DPC Control 레지스터를 suspend 전에 저장하고
 *                         resume 후 되돌린다.
 *
 * rp_pio_error_string[] : RP PIO Status 의 비트 번호를 사람이 읽을 이름으로
 *                         바꾸는 표. 인덱스가 곧 비트 번호다.
 * dpc_completed_waitqueue : dpc_reset_link() 와 dpc_handle_surprise_removal()
 *                         이 끝날 때 깨우는 대기열. 기다리는 쪽은
 *                         pci_dpc_recovered() 다.
 * dpcdriver             : portdrv 에 등록하는 포트 서비스 드라이버 서술자.
 *
 * (기존 요약에는 dpc_has_rp_pio_error() 가 올라 있었으나 그런 이름의 함수는
 *  이 트리 어디에도 없다 — 이 파일에도, 다른 어디에도 정의가 없다. 삭제했다.
 *  RP PIO 로그를 읽을지 판정하는 실제 코드는 dpc_process_rp_pio_error() 안의
 *  dpc_rp_log_size 검사다.)
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 하지만 DPC 가 NVMe 에 미치는 영향은 크다.
 *
 * U.2/EDSFF 백플레인에서 NVMe 드라이브를 예고 없이 뽑는 경우를 보자.
 * DPC 가 없으면 링크가 끊긴 뒤에도 진행 중이던 DMA 와 완료 대기가 남아
 * Completion Timeout 이 연쇄적으로 발생하고, 그 오류가 위로 번진다.
 * DPC 가 있으면 하드웨어가 즉시 링크를 Off 로 만들어 그 포트에서 끝내고,
 * 커널은 깔끔하게 nvme_error_detected() -> 제거 절차로 넘어간다.
 *
 * 복구 흐름에서 이 파일과 NVMe 가 만나는 지점은 간접적이다 —
 * dpc_handler() 가 pcie_do_recovery() 를 부르고, 그것이 NVMe 가 등록한
 * nvme_error_detected / nvme_slot_reset / nvme_error_resume 을 차례로 부른다.
 *
 * nvme_err_handler(drivers/nvme/host/pci.c:5157-5163)에 등록된 콜백은
 * 모두 다섯이다 — 위 셋에 reset_prepare(nvme_reset_prepare)와
 * reset_done(nvme_reset_done)이 더 있다. 뒤의 둘은 FLR 경로에서 불리는
 * 것이고, 이 파일이 시작한 복구가 부르는 것은 앞의 셋이다.
 *
 * (기존 주석은 복구 콜백 이름을 "nvme_resume" 이라고 적었으나, NVMe 가
 *  err_handler 의 .resume 에 등록한 함수는 nvme_error_resume 이다.
 *  nvme_resume 은 전원 관리(dev_pm_ops)의 복귀 콜백으로 전혀 다른 함수다.
 *  또 "DPC 가 P2PDMA/SR-IOV/MSI-X/ATS/ReBAR 과 밀접하게 연관된다" 는
 *  서술은 이 파일의 코드와 근거를 찾을 수 없어 삭제했다.)
 */

/* [한국어] 이 파일의 dev_ 계열 printk 앞에 "DPC: " 를 붙인다.
 * aer.c 와 달리 pr_fmt 는 정의하지 않는데, 이 파일이 장치 없는 pr_ 계열
 * 출력을 쓰지 않기 때문이다 */
#define dev_fmt(fmt) "DPC: " fmt

/* [한국어] struct aer_capability_regs 등 AER 관련 타입. DPC 는 원인을 알아내려고
 * AER 레지스터를 함께 읽으므로 이 헤더가 필요하다 */
#include <linux/aer.h>
/* [한국어] FIELD_GET — 마스크 상수 하나로 비트 필드를 뽑아내는 매크로.
 * RP PIO 의 First Error Pointer 와 로그 크기 필드를 읽는 데 쓴다 */
#include <linux/bitfield.h>
/* [한국어] msleep — dpc_wait_rp_inactive() 가 RP Busy 를 10밀리초 간격으로 폴링한다 */
#include <linux/delay.h>
/* [한국어] irqreturn_t, IRQ_NONE/IRQ_HANDLED/IRQ_WAKE_THREAD,
 * devm_request_threaded_irq — 이 파일의 두 IRQ 핸들러가 여기에 기댄다 */
#include <linux/interrupt.h>
/* [한국어] __init — 맨 아래 pcie_dpc_init() 을 부팅 전용 섹션에 넣는다 */
#include <linux/init.h>
/* [한국어] struct pci_dev, pci_read_config_ 계열, pcie_capability_ 계열,
 * pci_domain_nr, PCI_SLOT/PCI_FUNC 등 PCI 코어 API 전부 */
#include <linux/pci.h>

/* [한국어] 포트 서비스 계층 헤더. struct pcie_device, pcie_port_service_driver,
 * pcie_port_service_register(), pcie_ports_dpc_native 가 여기서 온다 */
#include "portdrv.h"
/* [한국어] PCI 코어 내부 헤더. struct aer_err_info, aer_get_device_error_info(),
 * aer_print_error(), pci_aer_clear_ 계열, dpc_tlp_log_len(),
 * pcie_wait_for_link(), PCI_DPC_RECOVERING/RECOVERED 비트 번호가 여기 있다 */
#include "../pci.h"

/* [한국어] DPC Control 의 트리거 활성 비트 두 개(FATAL, NONFATAL)를 묶은 마스크.
 * dpc_enable() 이 새 설정을 쓰기 전에 이 묶음으로 기존 값을 먼저 지운다 —
 * 어느 쪽이 켜져 있었든 상관없이 원하는 상태로 만들기 위해서다 */
#define PCI_EXP_DPC_CTL_EN_MASK	(PCI_EXP_DPC_CTL_EN_FATAL | \
				 PCI_EXP_DPC_CTL_EN_NONFATAL)

/* [한국어] RP PIO Status 의 비트 번호를 사람이 읽을 이름으로 바꾸는 표.
 * 인덱스가 곧 비트 번호이고, NULL 자리는 스펙상 예약된 비트다.
 * 
 * 배열을 읽으면 규칙이 보인다 — 0~2 가 Configuration Request,
 * 8~10 이 I/O Request, 16~18 이 Memory Request 이고, 각 묶음 안에서
 * UR 완료 / CA 완료 / 완료 타임아웃 순이다. 즉 "어떤 종류의 요청이
 * 어떤 식으로 실패했는가" 를 8비트 간격으로 배치한 것이다.
 * 
 * 읽는 자: dpc_process_rp_pio_error() 하나뿐.
 * 동기화: const 정적 데이터라 보호가 필요 없다 */
static const char * const rp_pio_error_string[] = {
	"Configuration Request received UR Completion",	 /* Bit Position 0  */
	"Configuration Request received CA Completion",	 /* Bit Position 1  */
	"Configuration Request Completion Timeout",	 /* Bit Position 2  */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"I/O Request received UR Completion",		 /* Bit Position 8  */
	"I/O Request received CA Completion",		 /* Bit Position 9  */
	"I/O Request Completion Timeout",		 /* Bit Position 10 */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	"Memory Request received UR Completion",	 /* Bit Position 16 */
	"Memory Request received CA Completion",	 /* Bit Position 17 */
	"Memory Request Completion Timeout",		 /* Bit Position 18 */
};

/* [한국어]
 * pci_save_dpc_state - suspend 전에 DPC Control 레지스터를 저장한다
 *
 * @dev: 대상 PCI 장치.  @return: 없음
 *
 * 리셋이나 D3 복귀 후 config space 는 기본값으로 돌아간다. DPC Control 의
 * Enable 과 Interrupt Enable 은 커널이 정한 값이므로, 그대로 두면 복귀 후
 * DPC 가 꺼진 채로 남는다. 그래서 pci_save_state() 경로가 이 함수를 부른다
 * (drivers/pci/pci.c:3083 근처의 주석이 그 목록을 적고 있다).
 *
 * 저장 대상은 u16 하나 — PCI_EXP_DPC_CTL 뿐이다. 상태(Status)는 저장하지
 * 않는다. 복원할 성질의 값이 아니기 때문이다.
 *
 * 저장 공간은 dpc_probe() 가 pci_add_ext_cap_save_buffer(..., sizeof(u16))
 * 로 미리 잡아 둔 것이다. 그것이 없으면(DPC 서비스가 붙지 않은 장치이거나
 * 할당이 실패했으면) 조용히 돌아선다.
 *
 * 맨 앞의 pci_is_pcie() 검사가 있고 dev->dpc_cap 검사는 없다. 저장 버퍼가
 * dpc_probe() 에서만 만들어지고 그때는 이미 dpc_cap 이 유효하므로,
 * save_state 가 있다는 것이 곧 dpc_cap 이 유효하다는 뜻이 된다.
 *
 * 실행 컨텍스트: PM/리셋 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_save_state() [pci.c] → [이 함수] → pci_find_saved_ext_cap()
 */
void pci_save_dpc_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;
	/* [한국어] 저장 버퍼 안을 가리킬 포인터. DPC 는 u16 하나만 저장하므로
	 * aer.c 처럼 여러 칸을 전진할 일이 없다 */
	u16 *cap;

	/* [한국어] PCIe 장치가 아니면 DPC capability 자체가 있을 수 없다 */
	if (!pci_is_pcie(dev))
		return;

	/* [한국어] PCI_EXT_CAP_ID_DPC 용으로 예약된 저장 슬롯을 찾는다.
	 * 그 슬롯은 dpc_probe() 가 pci_add_ext_cap_save_buffer() 로 만들어 둔 것이다 */
	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);
	/* [한국어] 슬롯이 없으면 이 장치에는 DPC 서비스가 붙지 않았거나 할당이 실패한 것이다.
	 * 조용히 돌아선다 — 복원 쪽도 같은 검사로 걸러진다 */
	if (!save_state)
		return;

	/* [한국어] 버퍼 첫 칸을 u16 로 본다. cap.data[] 는 u32 배열이지만 DPC Control 이
	 * 16비트라 그렇게 캐스팅한다 */
	cap = (u16 *)&save_state->cap.data[0];
	pci_read_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL, cap);
}

/* [한국어]
 * pci_restore_dpc_state - resume 후 DPC Control 레지스터를 되돌린다
 *
 * @dev: 대상 PCI 장치.  @return: 없음
 *
 * pci_save_dpc_state() 의 정확한 거울상이다. 같은 자리를 읽기 대신 쓰기로
 * 되돌린다.
 *
 * 확인한 호출자는 drivers/pci/pci.c:3538 의 pci_restore_state() 경로다.
 * 그 근처에서 pci_restore_aer_state() 도 함께 불린다 — 리셋 뒤 오류 관련
 * 설정을 한꺼번에 되살리는 자리다.
 *
 * 실행 컨텍스트: PM/리셋 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_restore_state() [pci.c:3538] → [이 함수] → pci_write_config_word()
 */
void pci_restore_dpc_state(struct pci_dev *dev)
{
	struct pci_cap_saved_state *save_state;
	/* [한국어] 저장 때와 같은 포인터 */
	u16 *cap;

	/* [한국어] PCIe 장치가 아니면 복원할 것이 없다 */
	if (!pci_is_pcie(dev))
		return;

	/* [한국어] 저장해 둔 슬롯을 찾는다 */
	save_state = pci_find_saved_ext_cap(dev, PCI_EXT_CAP_ID_DPC);
	/* [한국어] 슬롯이 없으면 저장된 값도 없다 */
	if (!save_state)
		return;

	/* [한국어] 버퍼 첫 칸을 u16 로 본다 */
	cap = (u16 *)&save_state->cap.data[0];
	/* [한국어] DPC Control 을 되돌린다. 이것으로 Enable 과 Interrupt Enable 이
	 * 리셋 전 상태로 복귀한다 */
	pci_write_config_word(dev, dev->dpc_cap + PCI_EXP_DPC_CTL, *cap);
}

/* [한국어] DPC 처리가 끝나기를 기다리는 프로세스들의 대기열.
 * 깨우는 쪽: dpc_reset_link() 와 dpc_handle_surprise_removal() 이
 *   일을 마칠 때 wake_up_all() 한다. 두 경로 모두 out 라벨에서 깨우므로
 *   실패로 끝나도 기다리던 쪽이 4초 타임아웃을 다 쓰지 않는다.
 * 기다리는 쪽: pci_dpc_recovered() 의 wait_event_timeout() 하나뿐.
 * 왜 전역인가: 포트마다 두지 않고 하나만 두었다. 깨어난 쪽이
 *   dpc_completed(pdev) 로 자기 포트의 조건을 다시 확인하므로,
 *   다른 포트의 이벤트에 깨어나도 그냥 다시 잠들 뿐이다. DPC 가
 *   드문 사건이라 이 단순함을 택한 것으로 보인다.
 * 동기화: 대기열 자체가 내부 스핀락을 갖는다 */
static DECLARE_WAIT_QUEUE_HEAD(dpc_completed_waitqueue);

/* [한국어] CONFIG_HOTPLUG_PCI_PCIE 가 켜졌을 때만 dpc_completed() 와
 * pci_dpc_recovered() 를 넣는다. 이 두 함수의 유일한 목적이 핫플러그
 * 드라이버에게 "이 Link Down/Up 은 DPC 가 만든 것이니 무시하라" 를
 * 알려 주는 것이라, 핫플러그가 없으면 물어볼 사람도 없기 때문이다.
 * 꺼져 있으면 drivers/pci/pci.h 의 인라인 스텁이 false 를 돌려준다.
 * 
 * 다만 위 dpc_completed_waitqueue 와 그것을 깨우는 wake_up_all() 은
 * 이 블록 밖에 있어 언제나 컴파일된다 — 기다리는 사람이 없어도
 * 깨우는 쪽은 그대로 돈다. */
#ifdef CONFIG_HOTPLUG_PCI_PCIE
/* [한국어]
 * dpc_completed - DPC 처리가 끝났는가(대기 조건)
 *
 * @pdev: 대상 포트.  @return: true 면 더 기다릴 필요가 없다
 *
 * pci_dpc_recovered() 의 wait_event_timeout() 조건으로만 쓰인다.
 * "끝났다" 를 두 가지로 확인한다.
 *
 *   1) DPC Status 의 Trigger 비트가 내려갔는가. 아직 서 있으면 포트가
 *      여전히 DPC 상태이므로 끝나지 않았다.
 *      PCI_POSSIBLE_ERROR(status) 를 함께 보는 이유가 중요하다 — 장치가
 *      사라지면 config 읽기가 전부 1(0xffff)로 돌아오고, 그러면 Trigger
 *      비트도 1 로 보인다. 그 경우를 "아직 트리거 중" 으로 오해하면
 *      4초를 헛되이 기다리게 되므로, 읽기 자체가 실패한 것으로 보고
 *      끝난 것으로 처리한다.
 *   2) PCI_DPC_RECOVERING 비트가 내려갔는가. dpc_reset_link() 가 일을
 *      시작할 때 세우고 끝날 때 지우는 표시다. 1)만 보면 Trigger 를 지운
 *      직후 아직 하위 버스를 기다리는 중인데 끝났다고 판단하게 된다.
 *
 * CONFIG_HOTPLUG_PCI_PCIE 안에서만 컴파일된다. 이 판정을 필요로 하는 것이
 * 핫플러그 드라이버뿐이기 때문이다.
 *
 * 실행 컨텍스트: 대기 조건 평가. wait_event_timeout 안에서 불리므로
 * 잠들면 안 되는데, config 읽기는 잠들지 않는다.
 *
 * 호출 체인:
 *   pci_dpc_recovered() → wait_event_timeout → [이 함수]
 */
static bool dpc_completed(struct pci_dev *pdev)
{
	u16 status;

	/* [한국어] DPC Status 를 읽는다. 대기 조건이라 자주 불리지만 config 읽기 한 번이라
	 * 비용이 크지 않다 */
	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_STATUS, &status);
	/* [한국어] PCI_POSSIBLE_ERROR 를 함께 보는 것이 요점이다. 장치가 사라지면 config
	 * 읽기가 전부 1(0xffff)로 돌아와 Trigger 비트도 1 로 보인다.
	 * 그 경우를 "아직 트리거 중" 으로 오해하면 4초를 헛되이 기다리게 되므로,
	 * 읽기 자체가 실패한 것으로 보고 끝난 것으로 처리한다 */
	if ((!PCI_POSSIBLE_ERROR(status)) && (status & PCI_EXP_DPC_STATUS_TRIGGER))
		return false;

	/* [한국어] Trigger 가 내려갔어도 dpc_reset_link() 가 아직 하위 버스를 기다리는
	 * 중일 수 있다. 그 사이를 표시하는 비트라, 이것까지 내려가야 정말 끝이다 */
	if (test_bit(PCI_DPC_RECOVERING, &pdev->priv_flags))
		return false;

	return true;
}

/**
 * pci_dpc_recovered - whether DPC triggered and has recovered successfully
 * @pdev: PCI device
 *
 * Return true if DPC was triggered for @pdev and has recovered successfully.
 * Wait for recovery if it hasn't completed yet.  Called from the PCIe hotplug
 * driver to recognize and ignore Link Down/Up events caused by DPC.
 */
/* [한국어]
 * pci_dpc_recovered - DPC 가 걸렸다가 복구되었는지 알려 준다
 *
 * @pdev: 대상 포트
 * @return: true 면 이 포트에서 DPC 가 걸렸고 복구까지 성공했다
 *
 * 위 원문 kernel-doc 이 용도를 밝힌다 — 핫플러그 드라이버가 DPC 때문에
 * 생긴 Link Down/Up 이벤트를 알아보고 무시하는 데 쓴다. 그 구분이 없으면
 * DPC 복구 중의 링크 흔들림을 "장치가 뽑혔다 다시 꽂혔다" 로 오해해
 * 장치를 제거해 버린다.
 *
 * 확인한 유일한 호출자는 drivers/pci/hotplug/pciehp_hpc.c:1624 다.
 * 그 줄에서 pci_hp_spurious_link_change() 와 논리합으로 묶여
 * "이 링크 변화가 우리가 아는 원인 때문인가" 를 판정한다.
 *
 * 세 단계다.
 *   1) dpc_cap 이 없으면 이 포트는 DPC 자체가 없다.
 *   2) 원문 주석대로, 펌웨어가 DPC 를 소유하고 EDR 도 꺼져 있으면 커널이
 *      DPC 진행 상황을 알 길이 없다. 그 경우 동기화를 지원하지 않는다고
 *      보고 false 를 돌려준다.
 *   3) 그 밖에는 dpc_completed() 가 참이 될 때까지 기다린 뒤,
 *      PCI_DPC_RECOVERED 비트를 test_and_clear 로 읽어 돌려준다.
 *      읽으면서 지우는 이유는 이 정보가 한 번만 소비되어야 하기 때문이다 —
 *      한 번의 DPC 이벤트를 두 번 "복구됨" 으로 보고하면 안 된다.
 *
 * 4초 타임아웃의 근거도 원문이 밝힌다 — 스펙이 시한을 정하지 않았지만
 * 보고된 사례들이 4초 안에 끝났다는 것이다. dpc_wait_rp_inactive() 가
 * 실패해 복구가 영영 끝나지 않는 경우를 대비한 상한이다.
 *
 * CONFIG_HOTPLUG_PCI_PCIE 안에서만 컴파일된다. 꺼져 있으면
 * drivers/pci/pci.h 의 인라인 스텁이 false 를 돌려준다.
 *
 * 실행 컨텍스트: 핫플러그 처리 스레드. 최대 4초 잠든다.
 *
 * 호출 체인:
 *   (pciehp 링크 이벤트 처리) → [이 함수] → wait_event_timeout → dpc_completed()
 */
bool pci_dpc_recovered(struct pci_dev *pdev)
{
	struct pci_host_bridge *host;

	/* [한국어] DPC capability 가 없는 포트에서는 DPC 가 걸릴 일이 없다 */
	if (!pdev->dpc_cap)
		return false;

	/*
	 * Synchronization between hotplug and DPC is not supported
	 * if DPC is owned by firmware and EDR is not enabled.
	 */
	host = pci_find_host_bridge(pdev->bus);
	/* [한국어] 바로 위 원문 주석대로, 펌웨어가 DPC 를 소유하고 EDR 도 꺼져 있으면
	 * 커널이 DPC 진행 상황을 알 방법이 없다. 그 경우 동기화를 지원하지
	 * 않는다고 보고 물러난다.
	 * native_dpc 는 drivers/pci/probe.c:1770 에서 기본값 1 로 세워지고,
	 * 그것을 0 으로 낮추는 _OSC 협상 코드는 이 스파스 체크아웃에 없다 */
	if (!host->native_dpc && !IS_ENABLED(CONFIG_PCIE_EDR))
		return false;

	/*
	 * Need a timeout in case DPC never completes due to failure of
	 * dpc_wait_rp_inactive().  The spec doesn't mandate a time limit,
	 * but reports indicate that DPC completes within 4 seconds.
	 */
	wait_event_timeout(dpc_completed_waitqueue, dpc_completed(pdev),
			   msecs_to_jiffies(4000));

	/* [한국어] 읽으면서 지운다. 이 정보는 한 번만 소비되어야 하기 때문이다 —
	 * 한 번의 DPC 이벤트를 두 번 "복구됨" 으로 보고하면 핫플러그 쪽이
	 * 진짜 제거 이벤트까지 무시하게 된다 */
	return test_and_clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
}
#endif /* CONFIG_HOTPLUG_PCI_PCIE */

/* [한국어]
 * dpc_wait_rp_inactive - Root Port 가 바쁜 상태에서 벗어나기를 기다린다
 *
 * @pdev: 대상 포트.  @return: 0 = 한가해졌다, -EBUSY = 1초 안에 안 끝났다
 *
 * DPC Status 의 RP Busy 비트는 "이 Root Port 가 아직 DPC 관련 내부 처리를
 * 하고 있다" 는 뜻이다. 그 상태에서 Trigger Status 를 지워 DPC 를 빠져나가려
 * 하면 동작이 정의되지 않으므로, 먼저 한가해지기를 기다린다.
 *
 * 10밀리초씩 자면서 최대 1초(HZ) 폴링한다. 인터럽트를 쓰지 않고 폴링하는
 * 이유는 이 비트가 인터럽트를 만들지 않기 때문이고, msleep 을 쓰는 것은
 * 이미 스레드 문맥이라 잠들어도 되기 때문이다.
 *
 * 시한을 넘기면 경고를 남기고 -EBUSY 를 돌려준다. 그러면 호출자
 * (dpc_reset_link)는 복구를 포기하고 PCI_ERS_RESULT_DISCONNECT 로 넘어간다 —
 * 장치를 되살리지 못했으니 떼어 내라는 뜻이다.
 *
 * RP 확장(dpc_rp_extensions)이 있는 포트에서만 의미가 있어, 두 호출자
 * 모두 그 플래그를 먼저 확인하고 부른다.
 *
 * 실행 컨텍스트: 스레드 문맥. msleep 으로 잠든다.
 *
 * 호출 체인:
 *   dpc_reset_link() / dpc_handle_surprise_removal() → [이 함수] → msleep()
 */
static int dpc_wait_rp_inactive(struct pci_dev *pdev)
{
	unsigned long timeout = jiffies + HZ;
	/* [한국어] cap = DPC capability 오프셋(반복해 쓰므로 지역 변수에 담는다),
	 * status = 폴링할 DPC Status 값 */
	u16 cap = pdev->dpc_cap, status;

	/* [한국어] 먼저 한 번 읽어 본다. 대개 이미 한가해 루프를 돌지 않는다 */
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);
	/* [한국어] RP Busy 가 서 있고 시한(1초)을 넘기지 않았으면 계속 기다린다.
	 * time_after 를 쓰는 이유는 jiffies 가 넘침(wrap)할 때도 비교가
	 * 올바르게 되도록 하기 위해서다 */
	while (status & PCI_EXP_DPC_RP_BUSY &&
					!time_after(jiffies, timeout)) {
		msleep(10);
		/* [한국어] 다시 읽는다. 이 비트는 인터럽트를 만들지 않아 폴링 외에 방법이 없다 */
		pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);
	}
	/* [한국어] 시한을 넘겼는데도 여전히 바쁘다 */
	if (status & PCI_EXP_DPC_RP_BUSY) {
		/* [한국어] 경고를 남긴다. 호출자는 이것을 받아 복구를 포기한다 */
		pci_warn(pdev, "root port still busy\n");
		return -EBUSY;
	}
	return 0;
}

/* [한국어]
 * dpc_reset_link - DPC 로 끊긴 링크를 되살린다(복구의 리셋 단계)
 *
 * @pdev: DPC 가 걸린 포트
 * @return: PCI_ERS_RESULT_RECOVERED = 링크가 다시 섰다,
 *          PCI_ERS_RESULT_DISCONNECT = 실패. 호출자가 장치를 포기한다
 *
 * dpc_handler()(dpc.c:1094)와 drivers/pci/pcie/edr.c:260 이 이 함수의
 * 주소를 pcie_do_recovery() 에 넘기고, 복구 절차의 "링크를 리셋하라"
 * 단계에서 불린다. 두 번째 호출자가 EDR — 펌웨어가 DPC 를 소유한 경우의
 * 경로이며, 링크를 되살리는 절차 자체는 같으므로 이 함수를 그대로 쓴다.
 *
 * 다른 리셋 콜백과 결정적으로 다른 점을 코드 안의 원문 주석이 밝힌다 —
 * DPC 는 하드웨어가 이미 링크를 내려 두었으므로 여기서 리셋을 걸 필요가
 * 없다. 이 함수가 하는 일은 "리셋" 이 아니라 "DPC 상태에서 빠져나오기" 다.
 *
 * 순서:
 *   1) PCI_DPC_RECOVERING 을 세운다. 이 사이에 pci_dpc_recovered() 가
 *      "끝났다" 고 오판하지 않게 하는 표시다.
 *   2) 링크가 실제로 내려가기를 기다린다. 1초 안에 안 내려가면 정보만
 *      남기고 계속 진행한다 — 실패로 처리하지 않는 이유는 아래 단계들이
 *      여전히 성공할 수 있기 때문이다.
 *   3) RP 확장이 있으면 RP Busy 가 내려가기를 기다린다. 실패하면
 *      PCI_DPC_RECOVERED 를 지우고 DISCONNECT 로 빠진다.
 *   4) DPC Trigger Status 에 1 을 써서 지운다(RW1C). 이것이 포트를 DPC
 *      에서 빠져나오게 하는 실제 동작이며, 이때부터 링크가 다시 훈련된다.
 *   5) pci_bridge_wait_for_secondary_bus() 로 하위 버스가 서기를 기다린다.
 *      성공하면 PCI_DPC_RECOVERED 를 세우고 RECOVERED 를, 실패하면 지우고
 *      DISCONNECT 를 돌려준다.
 *
 * out 라벨에서는 어느 경로로 왔든 PCI_DPC_RECOVERING 을 지우고 대기열을
 * 깨운다. 그래야 pci_dpc_recovered() 에서 기다리던 핫플러그 쪽이 4초
 * 타임아웃을 다 쓰지 않고 곧바로 진행한다.
 *
 * 실행 컨텍스트: 복구 스레드. 링크 대기로 여러 초 잠들 수 있다.
 *
 * 호출 체인:
 *   dpc_handler() → pcie_do_recovery() [err.c] → [이 함수]
 *     → pcie_wait_for_link() → dpc_wait_rp_inactive()
 *     → pci_bridge_wait_for_secondary_bus() → wake_up_all()
 */
pci_ers_result_t dpc_reset_link(struct pci_dev *pdev)
{
	pci_ers_result_t ret;
	/* [한국어] DPC capability 오프셋. 아래 Trigger Status 쓰기에 쓴다 */
	u16 cap;

	/* [한국어] 복구가 시작되었음을 표시한다. 이 사이에 pci_dpc_recovered() 가
	 * "끝났다" 고 오판하지 않게 하는 것이 목적이다 */
	set_bit(PCI_DPC_RECOVERING, &pdev->priv_flags);

	/*
	 * DPC disables the Link automatically in hardware, so it has
	 * already been reset by the time we get here.
	 */
	cap = pdev->dpc_cap;

	/*
	 * Wait until the Link is inactive, then clear DPC Trigger Status
	 * to allow the Port to leave DPC.
	 */
	if (!pcie_wait_for_link(pdev, false))
		/* [한국어] 1초 안에 안 내려갔다. 정보만 남기고 계속 진행한다 — 실패로 처리하지
		 * 않는 이유는 아래 단계들이 여전히 성공할 수 있기 때문이다 */
		pci_info(pdev, "Data Link Layer Link Active not cleared in 1000 msec\n");

	/* [한국어] RP 확장이 있는 포트라면 Root Port 가 한가해지기를 기다린다.
	 * 바쁜 상태에서 Trigger Status 를 지우면 동작이 정의되지 않는다 */
	if (pdev->dpc_rp_extensions && dpc_wait_rp_inactive(pdev)) {
		/* [한국어] 복구 실패로 표시한다. 핫플러그 쪽이 이 Link Down 을 진짜 제거로 처리한다 */
		clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
		ret = PCI_ERS_RESULT_DISCONNECT;
		goto out;
	}

	/* [한국어] Trigger Status 에 1 을 써서 지운다(RW1C). 이것이 포트를 DPC 상태에서
	 * 빠져나오게 하는 실제 동작이며, 이때부터 링크가 다시 훈련된다 */
	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_TRIGGER);

	/* [한국어] 하위 버스가 다시 서기를 기다린다. "DPC" 는 로그에 찍힐 문맥 문자열이다.
	 * PCIe 규격이 정한 대기 시간을 이 함수가 관리한다 */
	if (pci_bridge_wait_for_secondary_bus(pdev, "DPC")) {
		/* [한국어] 링크가 돌아오지 않았다. 복구 실패로 표시한다 */
		clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
		ret = PCI_ERS_RESULT_DISCONNECT;
	} else {
		/* [한국어] 링크가 돌아왔다. 핫플러그 쪽이 이 Link Down/Up 을 무시하도록 표시한다 */
		set_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
		ret = PCI_ERS_RESULT_RECOVERED;
	}
out:
	clear_bit(PCI_DPC_RECOVERING, &pdev->priv_flags);
	wake_up_all(&dpc_completed_waitqueue);
	return ret;
}

/* [한국어]
 * dpc_process_rp_pio_error - RP PIO 오류의 상세를 읽어 로그로 남긴다
 *
 * @pdev: DPC 가 걸린 포트.  @return: 없음
 *
 * RP PIO(Root Port Programmed I/O) 오류는 "Root Port 가 하류로 보낸 읽기/
 * 쓰기 요청이 실패했다" 는 뜻이다. CPU 가 장치 레지스터를 읽으려 했는데
 * UR/CA 완료가 돌아오거나 아예 완료가 오지 않은 경우이며, 어떤 종류의
 * 요청이었는지까지 하드웨어가 기록해 준다.
 *
 * 찍는 것이 네 묶음이다.
 *   1) RP PIO Status 와 Mask — 어떤 오류가 났고 어떤 것이 보고 대상인가.
 *   2) Severity / SysError / Exception 세 레지스터. 각각 이 오류를 fatal 로
 *      볼지, System Error 를 낼지, 예외로 처리할지를 정하는 설정값이다.
 *   3) 비트별 이름. status & ~mask 로 실제 보고된 것만 골라
 *      rp_pio_error_string[] 로 옮긴다. 인덱스가 곧 비트 번호이고,
 *      First Error Pointer 와 같은 비트에는 " (First)" 를 붙인다.
 *      그 포인터는 DPC Status 안에 있어 따로 읽는다.
 *   4) TLP 헤더 로그와 ImpSpec 로그. 문제의 요청이 어느 주소를 향했는지까지
 *      알 수 있는 가장 구체적인 정보다.
 *
 * 로그 읽기가 두 단계 조건부인 점이 중요하다.
 *   - dpc_rp_log_size 가 표준 헤더 개수보다 작으면 헤더 로그 자체가 없다.
 *   - 그보다 하나 더 커야 ImpSpec 로그까지 있다.
 * 어느 쪽이든 clear_status 로 건너뛴다. 크기를 확인하지 않고 읽으면
 * 존재하지 않는 레지스터를 읽게 된다.
 *
 * 마지막에 RP PIO Status 를 읽은 값 그대로 되써서 지운다(RW1C).
 *
 * 실행 컨텍스트: 스레드 문맥(dpc_handler). printk 를 여러 번 한다.
 *
 * 호출 체인:
 *   dpc_process_error() → [이 함수]
 *     → pcie_read_tlp_log() → pcie_print_tlp_log() [tlp.c]
 */
static void dpc_process_rp_pio_error(struct pci_dev *pdev)
{
	u16 cap = pdev->dpc_cap, dpc_status, first_error;
	/* [한국어] status/mask = RP PIO 오류 상태와 보고 마스크,
	 * sev/syserr/exc = 이 오류를 fatal 로 볼지, System Error 를 낼지,
	 * 예외로 처리할지를 정하는 설정 레지스터 셋,
	 * log = ImpSpec(구현 정의) 로그 */
	u32 status, mask, sev, syserr, exc, log;
	/* [한국어] TLP 헤더 로그를 받을 구조체. 어느 요청이 실패했는지가 여기 담긴다 */
	struct pcie_tlp_log tlp_log;
	/* [한국어] 비트 번호를 훑을 인덱스 */
	int i;

	/* [한국어] 어떤 RP PIO 오류가 났는가 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, &status);
	/* [한국어] 그중 어떤 것이 보고 대상인가 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_MASK, &mask);
	/* [한국어] 두 값을 원본 그대로 찍는다. 커널이 이름을 모르는 비트가 있어도
	 * 이 줄에는 남으므로 나중에 스펙과 대조할 수 있다 */
	pci_err(pdev, "rp_pio_status: %#010x, rp_pio_mask: %#010x\n",
		status, mask);

	/* [한국어] 이 오류를 fatal 로 볼지 정하는 설정 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SEVERITY, &sev);
	/* [한국어] System Error 를 낼지 정하는 설정 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_SYSERROR, &syserr);
	/* [한국어] 예외로 처리할지 정하는 설정 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_EXCEPTION, &exc);
	/* [한국어] 세 설정값을 한 줄에 찍는다. 왜 이 오류가 이런 심각도로 다뤄졌는지를
	 * 사후에 확인할 수 있게 하려는 것이다 */
	pci_err(pdev, "RP PIO severity=%#010x, syserror=%#010x, exception=%#010x\n",
		sev, syserr, exc);

	/* Get First Error Pointer */
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &dpc_status);
	/* [한국어] First Error Pointer 는 RP PIO 쪽이 아니라 DPC Status 안에 들어 있다.
	 * 그래서 위에서 따로 읽은 dpc_status 에서 뽑는다. 여러 비트가 동시에
	 * 섰을 때 맨 처음 난 오류를 가리킨다 */
	first_error = FIELD_GET(PCI_EXP_DPC_RP_PIO_FEP, dpc_status);

	/* [한국어] 이름 표 길이만큼 훑는다. 32비트 전체가 아니라 표 크기(19)만 도는데,
	 * 그 뒤 비트에는 정의된 이름이 없기 때문이다 */
	for (i = 0; i < ARRAY_SIZE(rp_pio_error_string); i++) {
		/* [한국어] 마스크되지 않은, 즉 실제로 보고된 비트만 찍는다 */
		if ((status & ~mask) & (1 << i))
			/* [한국어] "[ 2] Configuration Request Completion Timeout (First)" 같은 한 줄.
			 * 표의 그 자리가 NULL 이면 %s 에 NULL 이 들어가는데, 커널 printk 는
			 * 그것을 "(null)" 로 찍는다 — 코드는 고치지 않고 이 관찰만 적어 둔다 */
			pci_err(pdev, "[%2d] %s%s\n", i, rp_pio_error_string[i],
				first_error == i ? " (First)" : "");
	}

	/* [한국어] 로그 크기가 표준 헤더 개수보다 작으면 헤더 로그 자체가 없다.
	 * 크기를 확인하지 않고 읽으면 존재하지 않는 레지스터를 건드리게 된다 */
	if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG)
		goto clear_status;
	/* [한국어] Header Log 와 TLP Prefix Log 를 읽어 tlp_log 에 채운다.
	 * 길이는 dpc_tlp_log_len()(drivers/pci/pcie/tlp.c:97)이 계산하고,
	 * 하위 버스의 flit_mode 로 Gen6 FLIT 형식인지 알려 준다 */
	pcie_read_tlp_log(pdev, cap + PCI_EXP_DPC_RP_PIO_HEADER_LOG,
			  cap + PCI_EXP_DPC_RP_PIO_TLPPREFIX_LOG,
			  dpc_tlp_log_len(pdev),
			  pdev->subordinate->flit_mode,
			  &tlp_log);
	/* [한국어] 사람이 읽는 형태로 찍는다. aer.c 가 쓰는 것과 같은 함수라
	 * 두 경로의 TLP 출력이 같은 모양이 된다 */
	pcie_print_tlp_log(pdev, &tlp_log, KERN_ERR, dev_fmt(""));

	/* [한국어] ImpSpec 로그는 표준 헤더보다 한 칸 더 있을 때만 존재한다 */
	if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG + 1)
		goto clear_status;
	/* [한국어] 구현 정의 로그. 벤더가 자유롭게 쓰는 자리라 커널이 해석하지 않는다 */
	pci_read_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_IMPSPEC_LOG, &log);
	/* [한국어] 값만 그대로 찍는다. 벤더 문서와 대조할 사람을 위한 정보다 */
	pci_err(pdev, "RP PIO ImpSpec Log %#010x\n", log);

 clear_status:
	pci_write_config_dword(pdev, cap + PCI_EXP_DPC_RP_PIO_STATUS, status);
}

/* [한국어]
 * dpc_get_aer_uncorrect_severity - AER 상태를 읽어 심각도를 판정하고 info 를 채운다
 *
 * @dev: DPC 가 걸린 포트.  @info: 채울 오류 정보(호출자가 0 초기화해 넘긴다)
 * @return: 1 = 보고할 오류가 있다, 0 = 없다
 *
 * DPC 는 "치명적 오류가 났다" 는 사실만 알려 줄 뿐 무슨 오류인지는
 * AER 레지스터에 있다. 그래서 이 함수가 AER 쪽을 읽어 aer_err_info 를
 * 조립하고, 그것을 aer.c 의 출력 함수들에 그대로 넘길 수 있게 만든다.
 *
 * 판정은 aer.c 의 pci_aer_clear_fatal_status() 와 같은 원리다.
 *   status &= ~mask   로 마스크되지 않은 오류만 남기고, 하나도 없으면 0.
 *   status &= sev     로 severity 가 1 인 비트만 남긴다. 남으면 AER_FATAL,
 *                     남지 않으면 AER_NONFATAL.
 * "어느 오류가 fatal 인가" 는 장치 고정이 아니라 Severity 레지스터가
 * 정한다는 점이 여기서도 그대로 적용된다.
 *
 * 뒤이어 aer_err_info 의 나머지를 채운다. level 은 KERN_ERR, dev[0] 에
 * 이 포트 자신을, error_dev_num 은 1, ratelimit_print[0] 은 1 이다.
 * 장치를 하나만 넣는 이유는 DPC 가 이미 "이 포트에서 났다" 를 알려 주었기
 * 때문이고, ratelimit 을 강제로 1 로 두는 이유는 DPC 이벤트가 드물어
 * 눌러야 할 이유가 없기 때문이다.
 *
 * info->status 는 여기서 채우지 않는다. 그것은 호출자가 이어서 부르는
 * aer_get_device_error_info() 가 채운다 — 이 함수는 심각도만 정한다.
 *
 * 실행 컨텍스트: 스레드 문맥(dpc_handler).
 *
 * 호출 체인:
 *   dpc_process_error() → [이 함수] → pci_read_config_dword()
 */
static int dpc_get_aer_uncorrect_severity(struct pci_dev *dev,
					  struct aer_err_info *info)
{
	int pos = dev->aer_cap;
	/* [한국어] status = 오류 상태, mask = 보고 마스크, sev = 심각도 설정 */
	u32 status, mask, sev;

	/* [한국어] 이 포트의 AER Uncorrectable Status */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_STATUS, &status);
	/* [한국어] 그 마스크 */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_MASK, &mask);
	/* [한국어] 마스크되지 않은, 즉 실제로 보고된 오류만 남긴다 */
	status &= ~mask;
	/* [한국어] 보고할 오류가 하나도 없으면 0 을 돌려준다. 호출자는 출력을 건너뛴다 */
	if (!status)
		return 0;

	/* [한국어] 심각도 설정을 읽는다. 어느 오류가 fatal 인가는 장치 고정이 아니라
	 * 이 레지스터가 정한다 */
	pci_read_config_dword(dev, pos + PCI_ERR_UNCOR_SEVER, &sev);
	/* [한국어] severity 가 1 인 비트만 남긴다 */
	status &= sev;
	/* [한국어] 하나라도 남았으면 */
	if (status)
		/* [한국어] fatal 로 본다 */
		info->severity = AER_FATAL;
	else
		/* [한국어] 남지 않았으면 nonfatal 이다 */
		info->severity = AER_NONFATAL;

	/* [한국어] DPC 로 격리된 사건이라 언제나 오류 수준으로 찍는다 */
	info->level = KERN_ERR;

	/* [한국어] 오류를 낸 장치로 이 포트 자신을 넣는다. DPC 가 이미 "이 포트에서
	 * 났다" 를 알려 주었으므로 aer.c 처럼 하위 트리를 뒤질 필요가 없다 */
	info->dev[0] = dev;
	/* [한국어] 그래서 장치는 하나뿐이다 */
	info->error_dev_num = 1;
	/* [한국어] 레이트리밋을 강제로 켠다. DPC 이벤트는 드물어 눌러야 할 이유가 없고,
	 * 격리가 일어났는데 로그가 눌려 보이지 않으면 원인을 알 수 없게 된다 */
	info->ratelimit_print[0] = 1;

	return 1;
}

/* [한국어]
 * dpc_process_error - DPC 가 걸린 이유를 해석해 로그로 남긴다
 *
 * @pdev: DPC 가 걸린 포트.  @return: 없음
 *
 * DPC Status 의 Trigger Reason 필드를 읽어 갈래를 나눈다. 이 파일이
 * dmesg 에 남기는 "containment event" 줄들이 전부 여기서 나온다.
 *
 *   UNCOR (마스크되지 않은 uncorrectable 오류)
 *       이 포트 자신의 AER 레지스터에 원인이 남아 있다.
 *       dpc_get_aer_uncorrect_severity() 로 심각도를 정하고,
 *       aer_get_device_error_info() 로 상태를 읽은 뒤
 *       aer_print_error() 로 aer.c 와 같은 서식으로 찍는다. 그 다음
 *       nonfatal/fatal 상태를 모두 지운다. 세 함수를 그대로 빌려 쓰므로
 *       DPC 로그와 AER 로그가 같은 모양으로 나온다.
 *
 *   NFE / FE (하류에서 ERR_NONFATAL 또는 ERR_FATAL 메시지를 받았다)
 *       원인 장치의 Requester ID 가 DPC Source ID 레지스터에 있다.
 *       그것을 읽어 BDF 로 풀어 찍는다. 이 경우 상세 상태는 그 하류
 *       장치에 있으므로 여기서 더 읽지 않는다.
 *
 *   IN_EXT (확장 트리거 사유)
 *       한 단계 더 들어가 RP PIO 오류인지, 소프트웨어가 일부러 건 것인지,
 *       예약값인지 가른다. RP PIO 이고 RP 확장이 있으면
 *       dpc_process_rp_pio_error() 로 상세 로그까지 남긴다.
 *
 * 확인한 호출자는 둘이다 — 같은 파일의 dpc_handler()(dpc.c:1091)와
 * drivers/pci/pcie/edr.c:253. static 이 아니고 drivers/pci/pci.h:2173 에
 * 선언이 있는 이유가 그 두 번째 호출자다. EDR 은 펌웨어가 DPC 를 소유한
 * 경우의 경로인데, 원인을 읽고 찍는 일은 똑같으므로 이 함수를 그대로
 * 재사용한다. 그 바로 다음 줄(edr.c:260)에서 dpc_reset_link() 까지
 * 같은 방식으로 빌려 쓴다.
 *
 * 실행 컨텍스트: 스레드 문맥(dpc_handler). printk 를 여러 번 한다.
 *
 * 호출 체인:
 *   dpc_handler() → [이 함수]
 *     → dpc_get_aer_uncorrect_severity() → aer_get_device_error_info()
 *     → aer_print_error() → dpc_process_rp_pio_error()
 */
void dpc_process_error(struct pci_dev *pdev)
{
	u16 cap = pdev->dpc_cap, status, source, reason, ext_reason;
	/* [한국어] = {} 로 0 초기화한다. 아래 두 함수가 필드를 나눠 채우므로
	 * 초기값이 쓰레기면 안 된다 */
	struct aer_err_info info = {};

	/* [한국어] DPC Status 를 읽는다. 트리거 사유와 First Error Pointer 가 여기 있다 */
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);

	/* [한국어] Trigger Reason 필드만 뽑는다 */
	reason = status & PCI_EXP_DPC_STATUS_TRIGGER_RSN;

	/* [한국어] 사유마다 알아낼 수 있는 정보가 다르다 */
	switch (reason) {
	/* [한국어] 마스크되지 않은 uncorrectable 오류. 원인이 이 포트 자신의 AER 에 있다 */
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_UNCOR:
		pci_warn(pdev, "containment event, status:%#06x: unmasked uncorrectable error detected\n",
			 status);
		/* [한국어] 심각도를 정하고(dpc_get_aer_uncorrect_severity), 상태를 읽는다
		 * (aer_get_device_error_info). 둘 다 성공했을 때만 출력한다 */
		if (dpc_get_aer_uncorrect_severity(pdev, &info) &&
		    aer_get_device_error_info(&info, 0)) {
			/* [한국어] aer.c 의 출력 함수를 그대로 쓴다. 그래서 DPC 로그와 AER 로그가
			 * 같은 서식으로 나온다 */
			aer_print_error(&info, 0);
			pci_aer_clear_nonfatal_status(pdev);
			pci_aer_clear_fatal_status(pdev);
		}
		break;
	/* [한국어] 하류에서 ERR_NONFATAL 메시지를 받았다 */
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_NFE:
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_FE:
		pci_read_config_word(pdev, cap + PCI_EXP_DPC_SOURCE_ID,
			     &source);
		/* [한국어] 원인 장치의 BDF 를 풀어 찍는다. 상세 상태는 그 하류 장치에 있으므로
		 * 여기서 더 읽지 않는다. 도메인 번호는 이 포트의 것을 쓴다 —
		 * Source ID 에는 도메인이 들어 있지 않기 때문이다 */
		pci_warn(pdev, "containment event, status:%#06x, %s received from %04x:%02x:%02x.%d\n",
			 status,
			 (reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_FE) ?
				"ERR_FATAL" : "ERR_NONFATAL",
			 pci_domain_nr(pdev->bus), PCI_BUS_NUM(source),
			 PCI_SLOT(source), PCI_FUNC(source));
		break;
	/* [한국어] 확장 트리거 사유. 한 단계 더 들어가야 무엇인지 알 수 있다 */
	case PCI_EXP_DPC_STATUS_TRIGGER_RSN_IN_EXT:
		ext_reason = status & PCI_EXP_DPC_STATUS_TRIGGER_RSN_EXT;
		/* [한국어] RP PIO 오류인지, 소프트웨어가 일부러 건 것인지, 예약값인지를
		 * 삼항 연산자 사슬로 골라 찍는다 */
		pci_warn(pdev, "containment event, status:%#06x: %s detected\n",
			 status,
			 (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_RP_PIO) ?
			 "RP PIO error" :
			 (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_SW_TRIGGER) ?
			 "software trigger" :
			 "reserved error");
		/* show RP PIO error detail information */
		if (ext_reason == PCI_EXP_DPC_STATUS_TRIGGER_RSN_RP_PIO &&
		    pdev->dpc_rp_extensions)
			dpc_process_rp_pio_error(pdev);
		break;
	}
}

/* [한국어]
 * pci_clear_surpdn_errors - surprise removal 이 남긴 오류 비트들을 지운다
 *
 * @pdev: 장치가 뽑힌 포트.  @return: 없음
 *
 * 장치를 예고 없이 뽑으면 오류 비트가 여러 레지스터에 흩어져 선다.
 * 그것들은 진짜 고장이 아니라 제거의 부작용이므로, 오류로 처리하지 않고
 * 조용히 지운다.
 *
 * 지우는 곳이 셋이다.
 *   1) RP 확장이 있으면 RP PIO Status 전체(~0 을 써서 모든 비트를 RW1C).
 *   2) PCI Status 레지스터에 0xffff. 바로 위 원문 영어 주석이 근거를
 *      밝힌다 — 실제로 Surprise Down 오류가 이 레지스터의 오류 비트들도
 *      함께 세우는 것이 관찰되었다는 것이다.
 *   3) PCIe Device Status 의 Fatal Error Detected 비트. 같은 관찰의
 *      연장이다.
 *
 * 모두 RW1C 라 1 을 쓰는 것이 지우는 동작이다. 두 번째에서 0xffff 를
 * 통째로 쓰는 것은 "선 것만 지워지고 안 선 것에는 영향이 없다" 는
 * RW1C 의 성질에 기댄 것이다.
 *
 * 실행 컨텍스트: 스레드 문맥(dpc_handler).
 *
 * 호출 체인:
 *   dpc_handle_surprise_removal() → [이 함수] → pci_write_config_ 계열
 */
static void pci_clear_surpdn_errors(struct pci_dev *pdev)
{
	if (pdev->dpc_rp_extensions)
		/* [한국어] RP PIO Status 전체를 지운다. ~0 을 쓰는 것은 RW1C 라 "선 것만
		 * 지워지고 안 선 것에는 영향이 없다" 는 성질에 기댄 것이다 */
		pci_write_config_dword(pdev, pdev->dpc_cap +
				       PCI_EXP_DPC_RP_PIO_STATUS, ~0);

	/*
	 * In practice, Surprise Down errors have been observed to also set
	 * error bits in the Status Register as well as the Fatal Error
	 * Detected bit in the Device Status Register.
	 */
	pci_write_config_word(pdev, PCI_STATUS, 0xffff);

	pcie_capability_write_word(pdev, PCI_EXP_DEVSTA, PCI_EXP_DEVSTA_FED);
}

/* [한국어]
 * dpc_handle_surprise_removal - 예고 없는 장치 제거를 조용히 정리한다
 *
 * @pdev: 장치가 뽑힌 포트.  @return: 없음
 *
 * dpc_handler() 가 dpc_is_surprise_removal() 로 판정한 경우에만 온다.
 * 복구 절차(pcie_do_recovery)를 아예 밟지 않는 것이 요점이다 — 되살릴
 * 장치가 없기 때문이다. 대신 포트를 DPC 상태에서 빼내고 흔적을 지운다.
 *
 *   1) 링크가 내려가기를 기다린다. 1초 안에 안 내려가면 정보만 남기고
 *      out 으로 빠진다. 링크가 살아 있다면 정말 뽑힌 것이 아닐 수 있다.
 *   2) RP 확장이 있으면 RP Busy 가 내려가기를 기다린다. 실패하면 out.
 *   3) AER 상태를 통째로 지우고(raw 판이라 소유권을 따지지 않는다),
 *      pci_clear_surpdn_errors() 로 나머지 흩어진 비트도 지운다.
 *   4) DPC Trigger Status 를 지워 포트를 DPC 에서 빠져나오게 한다.
 *
 * out 라벨에서 어느 경로로 왔든 PCI_DPC_RECOVERED 를 지우고 대기열을
 * 깨운다. 지우는 이유가 중요하다 — 핫플러그 드라이버가
 * pci_dpc_recovered() 로 물었을 때 false 를 받아야 이 Link Down 을
 * "DPC 가 만든 무시할 이벤트" 가 아니라 "진짜 장치 제거" 로 처리한다.
 *
 * 실행 컨텍스트: 스레드 문맥(dpc_handler). 링크 대기로 잠든다.
 *
 * 호출 체인:
 *   dpc_handler() → [이 함수]
 *     → pcie_wait_for_link() → pci_aer_raw_clear_status()
 *     → pci_clear_surpdn_errors() → wake_up_all()
 */
static void dpc_handle_surprise_removal(struct pci_dev *pdev)
{
	if (!pcie_wait_for_link(pdev, false)) {
		/* [한국어] 링크가 1초 안에 안 내려갔다. 정말 뽑힌 것이 아닐 수 있으므로
		 * 정리를 진행하지 않고 out 으로 빠진다 */
		pci_info(pdev, "Data Link Layer Link Active not cleared in 1000 msec\n");
		goto out;
	}

	/* [한국어] RP 확장이 있으면 Root Port 가 한가해지기를 기다린다. 실패하면 out */
	if (pdev->dpc_rp_extensions && dpc_wait_rp_inactive(pdev))
		goto out;

	pci_aer_raw_clear_status(pdev);
	pci_clear_surpdn_errors(pdev);

	/* [한국어] Trigger Status 를 지워 포트를 DPC 상태에서 빼낸다.
	 * 복구 절차 없이 여기서 바로 지우는 이유는 되살릴 장치가 없기 때문이다 */
	pci_write_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_TRIGGER);

out:
	clear_bit(PCI_DPC_RECOVERED, &pdev->priv_flags);
	wake_up_all(&dpc_completed_waitqueue);
}

/* [한국어]
 * dpc_is_surprise_removal - 이 DPC 이벤트가 장치 제거 때문인가
 *
 * @pdev: DPC 가 걸린 포트.  @return: true 면 예고 없는 제거다
 *
 * 세 조건을 차례로 본다.
 *   1) 이 포트가 핫플러그 브리지인가. 아니면 장치를 뽑을 수 없으므로
 *      제거일 리 없다. 가장 싼 검사를 앞에 두었다.
 *   2) AER Uncorrectable Status 를 읽는다. 읽기 자체가 실패하면
 *      (pci_read_config_word 가 0 이 아닌 값을 돌려주면) 판정할 수 없으므로
 *      false 로 본다.
 *   3) 그 안의 Surprise Down Error 비트가 서 있는가.
 *
 * 이 판정이 참이면 dpc_handler() 는 복구 절차를 건너뛴다. 그 근거를
 * dpc_handler() 안의 원문 주석이 PCIe r6.0 6.7.6 을 들어 밝힌다 —
 * 비동기 제거에서 오류가 나는 것은 예상된 부작용이며 소프트웨어가
 * 무시해야 한다는 것이다.
 *
 * 주의할 점: dev->aer_cap 이 0 인 장치에서도 그대로 읽는다. 그 경우
 * config 오프셋 0 근처의 Vendor ID 를 읽게 되므로 판정이 무의미해질 수
 * 있으나, 코드는 고치지 않고 이 관찰만 적어 둔다. 실제로는 DPC 서비스가
 * 붙는 포트가 대부분 AER 도 함께 갖는다(dpc_probe 가 pcie_aer_is_native()
 * 를 먼저 확인한다).
 *
 * 실행 컨텍스트: 스레드 문맥(dpc_handler 진입 직후).
 *
 * 호출 체인:
 *   dpc_handler() → [이 함수] → pci_read_config_word()
 */
static bool dpc_is_surprise_removal(struct pci_dev *pdev)
{
	u16 status;

	/* [한국어] 핫플러그 브리지가 아니면 장치를 뽑을 수 없다. 가장 싼 검사를 앞에 둔다 */
	if (!pdev->is_hotplug_bridge)
		return false;

	/* [한국어] AER Uncorrectable Status 를 읽는다. 읽기 자체가 실패하면
	 * (0 이 아닌 값을 돌려주면) 판정할 수 없으므로 아래에서 false 로 본다.
	 * 주의: dev->aer_cap 이 0 이면 config 오프셋 0 근처를 읽게 되는데,
	 * 코드는 고치지 않고 이 관찰만 적어 둔다. 실제로는 dpc_probe() 가
	 * pcie_aer_is_native() 를 먼저 확인하므로 대부분 유효하다 */
	if (pci_read_config_word(pdev, pdev->aer_cap + PCI_ERR_UNCOR_STATUS,
				 &status))
		return false;

	return status & PCI_ERR_UNC_SURPDN;
}

/* [한국어]
 * dpc_handler - DPC 인터럽트의 스레드 핸들러(bottom half)
 *
 * @irq: IRQ 번호(쓰지 않는다).  @context: devm_request_threaded_irq 에 넘긴 pci_dev
 * @return: 항상 IRQ_HANDLED
 *
 * dpc_irq() 가 IRQ_WAKE_THREAD 를 돌려주면 커널이 이 함수를 스레드 문맥에서
 * 부른다. 여기서부터는 잠들어도 되므로 printk 와 링크 대기, 리셋을
 * 마음껏 할 수 있다.
 *
 * 갈래가 둘이다.
 *   1) 예고 없는 장치 제거. 원문 주석이 PCIe r6.0 6.7.6 을 근거로,
 *      이때의 오류는 예상된 부작용이니 무시해야 한다고 못 박는다.
 *      dpc_handle_surprise_removal() 로 조용히 정리하고 끝낸다.
 *      복구 절차를 밟지 않는 이유는 되살릴 장치가 없기 때문이다.
 *   2) 그 밖 — 진짜 오류. dpc_process_error() 로 원인을 기록한 뒤
 *      pcie_do_recovery() 로 복구를 시작한다.
 *
 * pci_dev_get / pci_dev_put 이 2)를 감싼다. 복구 도중 이 포트가 사라지면
 * 안 되기 때문이다. 1)에는 그것이 없는데, 코드는 고치지 않고 이 차이만
 * 적어 둔다.
 *
 * pci_channel_io_frozen 을 고정으로 넘기는 이유를 코드 안의 원문 주석이
 * 밝힌다 — dpc_enable() 이 DPC 를 ERR_FATAL 에만 반응하도록 설정하므로,
 * 여기 오는 사건은 언제나 치명적이다. 그래서 심각도를 따로 판정하지 않는다.
 * 세 번째 인자 dpc_reset_link 가 복구의 리셋 단계에서 불릴 콜백이다.
 *
 * 실행 컨텍스트: 커널 IRQ 스레드. 잠들 수 있다.
 *
 * 호출 체인:
 *   (IRQ 스레드) → [이 함수]
 *     → dpc_is_surprise_removal() → dpc_process_error()
 *     → pcie_do_recovery() [err.c] → nvme_error_detected() 등
 */
static irqreturn_t dpc_handler(int irq, void *context)
{
	struct pci_dev *pdev = context;

	/*
	 * According to PCIe r6.0 sec 6.7.6, errors are an expected side effect
	 * of async removal and should be ignored by software.
	 */
	if (dpc_is_surprise_removal(pdev)) {
		dpc_handle_surprise_removal(pdev);
		return IRQ_HANDLED;
	}

	pci_dev_get(pdev);
	dpc_process_error(pdev);

	/* We configure DPC so it only triggers on ERR_FATAL */
	pcie_do_recovery(pdev, pci_channel_io_frozen, dpc_reset_link);

	pci_dev_put(pdev);
	return IRQ_HANDLED;
}

/* [한국어]
 * dpc_irq - DPC 인터럽트의 하드 IRQ 핸들러(top half)
 *
 * @irq: IRQ 번호(쓰지 않는다).  @context: devm_request_threaded_irq 에 넘긴 pci_dev
 * @return: IRQ_NONE = 내 인터럽트가 아니다,
 *          IRQ_WAKE_THREAD = 트리거까지 섰으니 스레드를 깨워라,
 *          IRQ_HANDLED = 인터럽트만 있었고 트리거는 없었다
 *
 * 하드 IRQ 문맥이라 오래 머물 수 없다. 읽고, 지우고, 판정하는 것이 전부다.
 *
 *   1) DPC Status 를 읽는다.
 *   2) Interrupt 비트가 서 있지 않으면 다른 장치의 인터럽트다 — IRQF_SHARED
 *      로 등록하므로 이 판정이 필요하다. PCI_POSSIBLE_ERROR(status) 를 함께
 *      보는 이유는 장치가 사라져 config 읽기가 전부 1 로 돌아온 경우를
 *      "인터럽트가 왔다" 로 오해하지 않기 위해서다.
 *   3) Interrupt 비트를 RW1C 로 지운다. 이것을 해야 다음 인터럽트를 받을 수
 *      있고, 같은 인터럽트가 되풀이되지 않는다.
 *   4) Trigger 비트까지 서 있으면 실제로 격리가 일어난 것이므로 스레드를
 *      깨운다. 서 있지 않으면 인터럽트만 있었던 것이라 IRQ_HANDLED 로 끝낸다.
 *
 * 3)과 4)의 순서에 주의한다. Interrupt 비트만 지우고 Trigger 비트는 그대로
 * 둔다 — Trigger 는 dpc_reset_link() 가 복구를 마칠 때 지우는 것이고,
 * 그때까지 서 있어야 dpc_completed() 가 "아직 진행 중" 을 알아본다.
 *
 * 실행 컨텍스트: 하드 IRQ. 잠들 수 없다.
 *
 * 호출 체인:
 *   (하드웨어 인터럽트) → [이 함수] → (IRQ_WAKE_THREAD) → dpc_handler()
 */
static irqreturn_t dpc_irq(int irq, void *context)
{
	struct pci_dev *pdev = context;
	/* [한국어] cap = DPC capability 오프셋, status = DPC Status 값 */
	u16 cap = pdev->dpc_cap, status;

	/* [한국어] 하드 IRQ 문맥이지만 config 읽기는 피할 수 없다 —
	 * 이 인터럽트가 내 것인지 가리려면 상태를 봐야 한다 */
	pci_read_config_word(pdev, cap + PCI_EXP_DPC_STATUS, &status);

	/* [한국어] Interrupt 비트가 없으면 다른 장치의 인터럽트다(IRQF_SHARED).
	 * PCI_POSSIBLE_ERROR 를 함께 보는 이유는 장치가 사라져 읽기가 전부 1 로
	 * 돌아온 경우를 "인터럽트가 왔다" 로 오해하지 않기 위해서다 */
	if (!(status & PCI_EXP_DPC_STATUS_INTERRUPT) || PCI_POSSIBLE_ERROR(status))
		return IRQ_NONE;

	/* [한국어] Interrupt 비트만 RW1C 로 지운다. Trigger 비트는 그대로 둔다 —
	 * 그것은 dpc_reset_link() 가 복구를 마칠 때 지우는 것이고, 그때까지
	 * 서 있어야 dpc_completed() 가 "아직 진행 중" 을 알아본다 */
	pci_write_config_word(pdev, cap + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_INTERRUPT);
	/* [한국어] 트리거까지 섰으면 실제로 격리가 일어난 것이다. 스레드를 깨운다.
	 * 서 있지 않으면 인터럽트만 있었던 것이라 아래에서 IRQ_HANDLED 로 끝낸다 */
	if (status & PCI_EXP_DPC_STATUS_TRIGGER)
		return IRQ_WAKE_THREAD;
	return IRQ_HANDLED;
}

/* [한국어]
 * pci_dpc_init - 열거 때 DPC capability 를 찾아 기록해 둔다
 *
 * @pdev: 갓 열거된 PCI 장치.  @return: 없음
 *
 * drivers/pci/probe.c 의 pci_init_capabilities() 경로에서 불린다
 * (probe.c:6381 의 주석이 pci_aer_init, pci_iov_init 과 나란히 적고 있다).
 * DPC 서비스가 붙기 훨씬 전, 장치를 처음 인식할 때 실행된다.
 *
 *   1) DPC Extended Capability 오프셋을 찾아 dev->dpc_cap 에 넣는다.
 *      없으면 여기서 끝 — 이 장치는 DPC 를 지원하지 않는다.
 *   2) DPC Capability 레지스터를 읽어 RP Extensions 지원 여부를 본다.
 *      없으면 역시 여기서 끝난다. RP PIO 관련 레지스터가 전부 그 확장에
 *      딸려 있기 때문이다.
 *   3) dpc_rp_extensions 를 세우고 RP PIO 로그 크기를 계산한다.
 *
 * 3)의 계산이 이 함수에서 가장 까다롭다.
 *   - 원문 주석대로 dpc_rp_log_size 가 이미 0 이 아니면 건드리지 않는다.
 *     장치나 펌웨어가 잘못된 값을 보고하는 경우 quirk 가 미리 채워 두기
 *     때문이다(drivers/pci/quirks.c:12540 이 그 예다).
 *   - 기본 크기는 Capability 의 LOG_SIZE 필드에서 온다.
 *   - FLIT 모드(Gen6 의 새 패킷 포맷)면 LOG_SIZE4 필드를 4비트 왼쪽으로
 *     밀어 더한다. FLIT 에서는 로그가 더 길어질 수 있어 상위 비트를 별도
 *     필드에 담아 두었기 때문이다. FLIT 여부는 PCIe Capability 의 FLAGS 에서 읽는다.
 *   - 계산 결과가 표준 헤더 개수보다 작거나 최대치+1 보다 크면 하드웨어가
 *     이상한 값을 보고한 것이므로 경고를 남기고 0 으로 되돌린다. 0 이면
 *     dpc_process_rp_pio_error() 가 로그 읽기를 통째로 건너뛴다 —
 *     잘못된 크기로 읽어 존재하지 않는 레지스터를 건드리는 것보다 안전하다.
 *
 * 실행 컨텍스트: 버스 열거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_init_capabilities() [probe.c] → [이 함수] → pci_find_ext_capability()
 */
void pci_dpc_init(struct pci_dev *pdev)
{
	u16 cap;

	/* [한국어] config space 의 확장 capability 사슬을 훑어 DPC 오프셋을 찾는다.
	 * 이후 이 파일의 모든 DPC 레지스터 접근이 "dpc_cap + 오프셋" 형태를 쓴다 */
	pdev->dpc_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_DPC);
	/* [한국어] DPC 를 지원하지 않는 장치다. 여기서 끝 */
	if (!pdev->dpc_cap)
		return;

	/* [한국어] DPC Capability 레지스터를 읽는다. RP 확장 지원 여부가 여기 있다 */
	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_CAP, &cap);
	/* [한국어] RP Extensions 가 없으면 여기서 끝난다. RP PIO 관련 레지스터가
	 * 전부 그 확장에 딸려 있어, 없으면 로그 크기를 계산할 대상도 없다 */
	if (!(cap & PCI_EXP_DPC_CAP_RP_EXT))
		return;

	/* [한국어] 이 플래그가 서야 dpc_reset_link() 와 dpc_process_rp_pio_error() 가
	 * RP PIO 관련 처리를 한다 */
	pdev->dpc_rp_extensions = true;

	/* Quirks may set dpc_rp_log_size if device or firmware is buggy */
	if (!pdev->dpc_rp_log_size) {
		/* [한국어] PCIe Capability 의 FLAGS 값. FLIT 모드 여부를 여기서 읽는다 */
		u16 flags;
		/* [한국어] 읽기 결과 */
		int ret;

		/* [한국어] PCIe Capability 를 읽는다. DPC capability 가 아니라 PCIe 쪽이다 —
		 * FLIT 모드 표시가 거기 있기 때문이다 */
		ret = pcie_capability_read_word(pdev, PCI_EXP_FLAGS, &flags);
		/* [한국어] 읽기에 실패하면 로그 크기를 정할 수 없으므로 0 인 채로 둔다.
		 * 그러면 dpc_process_rp_pio_error() 가 로그 읽기를 통째로 건너뛴다 */
		if (ret)
			return;

		/* [한국어] 기본 크기는 DPC Capability 의 LOG_SIZE 필드에서 온다 */
		pdev->dpc_rp_log_size =
				FIELD_GET(PCI_EXP_DPC_RP_PIO_LOG_SIZE, cap);
		/* [한국어] FLIT 모드(Gen6 의 새 패킷 포맷)면 */
		if (FIELD_GET(PCI_EXP_FLAGS_FLIT, flags))
			/* [한국어] LOG_SIZE4 필드를 4비트 왼쪽으로 밀어 더한다. FLIT 에서는 로그가
			 * 더 길어질 수 있어 상위 비트를 별도 필드에 담아 두었기 때문이다 */
			pdev->dpc_rp_log_size += FIELD_GET(PCI_EXP_DPC_RP_PIO_LOG_SIZE4,
						   cap) << 4;

		/* [한국어] 계산 결과가 표준 헤더 개수보다 작거나 최대치+1 보다 크면
		 * 하드웨어가 이상한 값을 보고한 것이다 */
		if (pdev->dpc_rp_log_size < PCIE_STD_NUM_TLP_HEADERLOG ||
		    pdev->dpc_rp_log_size > PCIE_STD_MAX_TLP_HEADERLOG + 1) {
			/* [한국어] 무엇이 잘못되었는지 남긴다 */
			pci_err(pdev, "RP PIO log size %u is invalid\n",
				pdev->dpc_rp_log_size);
			/* [한국어] 0 으로 되돌려 로그 읽기를 아예 하지 않게 한다.
			 * 잘못된 크기로 읽어 존재하지 않는 레지스터를 건드리는 것보다 안전하다 */
			pdev->dpc_rp_log_size = 0;
		}
	}
}

/* [한국어]
 * dpc_enable - DPC 트리거와 인터럽트를 켠다
 *
 * @dev: DPC 서비스의 struct pcie_device.  @return: 없음
 *
 * 순서가 이 함수의 요점이다. 코드 안의 원문 주석이 이유를 밝힌다 —
 * 먼저 DPC Interrupt Status 를 지우지 않고 Interrupt Enable 을 켜면,
 * 예전에 남아 있던 이벤트 때문에 곧바로 가짜 인터럽트가 들어온다.
 *
 * 그 다음 Control 레지스터를 읽고-고치고-쓴다.
 *   ctl &= ~PCI_EXP_DPC_CTL_EN_MASK  로 기존 트리거 설정(FATAL/NONFATAL)을
 *       먼저 모두 지우고,
 *   ctl |= EN_FATAL | INT_EN         으로 ERR_FATAL 트리거와 인터럽트만 켠다.
 * NONFATAL 을 켜지 않는 선택이 dpc_handler() 가 심각도를 따지지 않고
 * 언제나 pci_channel_io_frozen 을 쓰는 근거가 된다.
 *
 * 읽고-고치고-쓰기라 Control 의 다른 비트(소프트웨어 트리거 등)는
 * 그대로 보존된다.
 *
 * dpc_probe() 와 dpc_resume() 이 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dpc_probe() / dpc_resume() → [이 함수] → pci_write_config_word()
 */
static void dpc_enable(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	/* [한국어] DPC capability 오프셋 */
	int dpc = pdev->dpc_cap;
	/* [한국어] Control 레지스터 값 */
	u16 ctl;

	/*
	 * Clear DPC Interrupt Status so we don't get an interrupt for an
	 * old event when setting DPC Interrupt Enable.
	 */
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_STATUS,
			      PCI_EXP_DPC_STATUS_INTERRUPT);

	/* [한국어] 현재 Control 을 읽는다. 읽고-고치고-쓰기라 다른 비트(소프트웨어 트리거
	 * 설정 등)는 그대로 보존된다 */
	pci_read_config_word(pdev, dpc + PCI_EXP_DPC_CTL, &ctl);
	/* [한국어] 기존 트리거 설정(FATAL/NONFATAL)을 먼저 모두 지운다.
	 * 어느 쪽이 켜져 있었든 상관없이 원하는 상태로 만들기 위해서다 */
	ctl &= ~PCI_EXP_DPC_CTL_EN_MASK;
	/* [한국어] ERR_FATAL 트리거와 인터럽트만 켠다. NONFATAL 을 켜지 않는 이 선택이
	 * dpc_handler() 가 심각도를 따지지 않고 언제나 pci_channel_io_frozen 을
	 * 쓰는 근거가 된다 */
	ctl |= PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN;
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_CTL, ctl);
}

/* [한국어]
 * dpc_disable - DPC 트리거와 인터럽트를 끈다
 *
 * @dev: DPC 서비스의 struct pcie_device.  @return: 없음
 *
 * dpc_enable() 이 켠 두 비트(EN_FATAL, INT_EN)를 AND-NOT 으로 지운다.
 * enable 쪽과 달리 Interrupt Status 를 건드리지 않는데, 끄는 시점에는
 * 지워 둘 이유가 없기 때문이다(다음에 켤 때 dpc_enable 이 지운다).
 *
 * EN_MASK 대신 EN_FATAL 만 지우는 점이 enable 쪽과 짝이 맞는다 —
 * 이 드라이버는 애초에 NONFATAL 을 켜지 않는다.
 *
 * dpc_remove() 와 dpc_suspend() 가 부른다. 서비스를 떼거나 절전에 들어갈 때
 * DPC 가 계속 살아 있으면, 그 과정의 링크 흔들림을 격리 사유로 오인해
 * 포트를 끊어 버릴 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dpc_remove() / dpc_suspend() → [이 함수] → pci_write_config_word()
 */
static void dpc_disable(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	/* [한국어] DPC capability 오프셋 */
	int dpc = pdev->dpc_cap;
	/* [한국어] Control 레지스터 값 */
	u16 ctl;

	/* Disable DPC triggering and DPC interrupts */
	pci_read_config_word(pdev, dpc + PCI_EXP_DPC_CTL, &ctl);
	/* [한국어] enable 이 켠 두 비트를 지운다. EN_MASK 가 아니라 EN_FATAL 만 지우는
	 * 것이 enable 쪽과 짝이 맞는다 — 이 드라이버는 NONFATAL 을 켜지 않는다 */
	ctl &= ~(PCI_EXP_DPC_CTL_EN_FATAL | PCI_EXP_DPC_CTL_INT_EN);
	/* [한국어] 되쓴다. 이제 격리도 인터럽트도 일어나지 않는다 */
	pci_write_config_word(pdev, dpc + PCI_EXP_DPC_CTL, ctl);
}

/* [한국어] 능력 비트 하나를 '+' 또는 '-' 한 글자로 바꾸는 매크로.
 * 바로 아래 dpc_probe() 의 능력 로그 한 줄에서만 쓰인다.
 * "RPExt+ PoisonedTLP- SwTrigger+" 처럼 lspci 와 비슷한 표기를 만들어,
 * 어떤 기능이 있는지 한눈에 보이게 한다. 인자를 괄호로 감싼 것은
 * 매크로 확장 시 연산자 우선순위가 뒤바뀌는 것을 막는 관용구다 */
#define FLAG(x, y) (((x) & (y)) ? '+' : '-')

/* [한국어]
 * dpc_probe - 포트에 DPC 서비스를 붙인다
 *
 * @dev: portdrv 가 만든 DPC 서비스용 struct pcie_device
 * @return: 0 = 성공, -ENOTSUPP = 커널이 DPC 를 소유하지 않음,
 *          그 밖에는 IRQ 등록 실패값
 *
 * 이 파일의 시작점이다. portdrv 가 PCIE_PORT_SERVICE_DPC 를 가진 포트마다
 * 부른다(그 판정은 drivers/pci/pcie/portdrv.c:373 이 한다).
 *
 *   1) 소유권 확인. pcie_aer_is_native() 도 아니고 부팅 인자
 *      pcie_ports_dpc_native 도 아니면 -ENOTSUPP 로 물러난다. AER 소유권을
 *      함께 보는 이유는, DPC 처리가 AER 레지스터를 읽고 지우는 데 전적으로
 *      의존하기 때문이다. 펌웨어가 AER 을 쥔 채 커널이 DPC 만 다루면
 *      둘이 같은 레지스터를 두고 부딪친다.
 *   2) 하드 IRQ(dpc_irq)와 스레드 핸들러(dpc_handler)를 한 쌍으로 등록한다.
 *      IRQF_SHARED 인 이유는 포트 서비스들(AER/PME/hotplug/DPC)이 같은
 *      MSI 벡터를 나눠 쓸 수 있기 때문이고, 그래서 dpc_irq() 가 맨 앞에서
 *      "내 인터럽트인가" 를 확인한다.
 *   3) dpc_enable() 로 트리거와 인터럽트를 켠다.
 *   4) 능력을 사람이 읽는 형태로 한 줄 찍는다. FLAG 매크로가 각 비트를
 *      '+' 또는 '-' 로 바꾼다.
 *   5) suspend/resume 용 저장 버퍼를 u16 하나 크기로 잡는다.
 *      pci_save_dpc_state() 가 그것을 쓴다.
 *
 * 반환값이 status 인데 이 시점에서는 언제나 0 이다. 0 이 아니면 이미
 * 2)에서 돌아갔기 때문이다. 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * pci_add_ext_cap_save_buffer() 의 실패를 확인하지 않는다. 실패하면
 * pci_save_dpc_state() 가 버퍼를 못 찾아 조용히 넘어가므로, DPC 자체는
 * 계속 동작한다.
 *
 * 실행 컨텍스트: 드라이버 바인드 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv 바인드) → [이 함수]
 *     → devm_request_threaded_irq() → dpc_enable()
 */
static int dpc_probe(struct pcie_device *dev)
{
	struct pci_dev *pdev = dev->port;
	/* [한국어] devm 할당과 IRQ 등록의 기준이 되는 device. 서비스가 떨어지면
	 * devm 이 알아서 해제한다 */
	struct device *device = &dev->device;
	/* [한국어] IRQ 등록 결과이자 이 함수의 반환값 */
	int status;
	/* [한국어] DPC Capability 값. 아래 능력 로그를 찍는 데만 쓴다 */
	u16 cap;

	/* [한국어] AER 소유권을 함께 보는 이유는, DPC 처리가 AER 레지스터를 읽고 지우는
	 * 데 전적으로 의존하기 때문이다. 펌웨어가 AER 을 쥔 채 커널이 DPC 만
	 * 다루면 둘이 같은 레지스터를 두고 부딪친다.
	 * pcie_ports_dpc_native 는 그 판정을 무시하고 강제로 가져가는 부팅 인자다 */
	if (!pcie_aer_is_native(pdev) && !pcie_ports_dpc_native)
		return -ENOTSUPP;

	/* [한국어] 하드 IRQ(dpc_irq)와 스레드 핸들러(dpc_handler)를 한 쌍으로 등록한다.
	 * IRQF_SHARED 인 이유는 포트 서비스들이 같은 MSI 벡터를 나눠 쓸 수 있기
	 * 때문이고, 그래서 dpc_irq() 가 맨 앞에서 "내 인터럽트인가" 를 확인한다 */
	status = devm_request_threaded_irq(device, dev->irq, dpc_irq,
					   dpc_handler, IRQF_SHARED,
					   "pcie-dpc", pdev);
	/* [한국어] 등록 실패 */
	if (status) {
		/* [한국어] 어느 IRQ 였는지 남긴다 */
		pci_warn(pdev, "request IRQ%d failed: %d\n", dev->irq,
			 status);
		/* [한국어] 실패를 그대로 올린다. devm 이 정리한다 */
		return status;
	}

	/* [한국어] 능력 로그에 쓸 값을 읽는다 */
	pci_read_config_word(pdev, pdev->dpc_cap + PCI_EXP_DPC_CAP, &cap);
	dpc_enable(dev);

	/* [한국어] 성공 로그 */
	pci_info(pdev, "enabled with IRQ %d\n", dev->irq);
	/* [한국어] 이 포트의 DPC 능력을 사람이 읽는 형태로 한 줄 찍는다.
	 * FLAG 매크로가 각 비트를 '+' 또는 '-' 로 바꾼다. RP PIO Log 는
	 * pci_dpc_init() 이 계산해 둔 크기다 */
	pci_info(pdev, "error containment capabilities: Int Msg #%d, RPExt%c PoisonedTLP%c SwTrigger%c RP PIO Log %d, DL_ActiveErr%c\n",
		 cap & PCI_EXP_DPC_IRQ, FLAG(cap, PCI_EXP_DPC_CAP_RP_EXT),
		 FLAG(cap, PCI_EXP_DPC_CAP_POISONED_TLP),
		 FLAG(cap, PCI_EXP_DPC_CAP_SW_TRIGGER), pdev->dpc_rp_log_size,
		 FLAG(cap, PCI_EXP_DPC_CAP_DL_ACTIVE));

	/* [한국어] suspend/resume 용 저장 버퍼를 u16 하나 크기로 잡는다.
	 * pci_save_dpc_state() 가 그것을 쓴다. 실패를 확인하지 않는데,
	 * 실패하면 저장 쪽이 버퍼를 못 찾아 조용히 넘어가므로 DPC 자체는
	 * 계속 동작한다 */
	pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_DPC, sizeof(u16));
	return status;
}

/* [한국어]
 * dpc_suspend - 절전 진입 시 DPC 를 끈다
 *
 * @dev: DPC 서비스의 struct pcie_device.  @return: 항상 0
 *
 * 절전 과정에서 링크가 내려가는 것은 정상인데, DPC 가 켜져 있으면 그것을
 * 격리 사유로 오인해 포트를 끊어 버릴 수 있다. 그래서 미리 끈다.
 *
 * Control 레지스터 값 자체의 보존은 이 함수가 아니라
 * pci_save_dpc_state() 가 pci_save_state() 경로에서 따로 처리한다 —
 * 역할이 나뉘어 있다.
 *
 * 짝이 되는 dpc_resume() 이 다시 켠다.
 *
 * 실행 컨텍스트: PM 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv PM) → [이 함수] → dpc_disable()
 */
static int dpc_suspend(struct pcie_device *dev)
{
	dpc_disable(dev);
	return 0;
}

/* [한국어]
 * dpc_resume - 절전에서 깨어난 뒤 DPC 를 다시 켠다
 *
 * @dev: DPC 서비스의 struct pcie_device.  @return: 항상 0
 *
 * dpc_enable() 을 그대로 부른다. 그 함수가 켜기 전에 묵은 Interrupt Status
 * 를 지우므로, 절전/복귀 중에 남은 이벤트 때문에 깨어나자마자 가짜
 * 인터럽트가 들어오는 일이 없다. dpc_probe() 와 같은 함수를 쓰는 것이
 * 자연스러운 이유가 여기 있다.
 *
 * 실행 컨텍스트: PM 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv PM) → [이 함수] → dpc_enable()
 */
static int dpc_resume(struct pcie_device *dev)
{
	dpc_enable(dev);
	return 0;
}

/* [한국어]
 * dpc_remove - DPC 서비스가 떨어질 때 정리한다
 *
 * @dev: DPC 서비스의 struct pcie_device.  @return: 없음
 *
 * dpc_disable() 한 줄이 전부다. 나머지는 손댈 것이 없다 — IRQ 는
 * devm_request_threaded_irq 로 잡았으므로 devm 이 알아서 해제한다.
 * 그 해제가 IRQ 를 free 하기 전에 인터럽트를 꺼 두어야 하므로,
 * 이 함수의 존재 이유가 바로 그 순서 보장이다.
 *
 * 실행 컨텍스트: 드라이버 언바인드 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (portdrv 언바인드) → [이 함수] → dpc_disable()
 */
static void dpc_remove(struct pcie_device *dev)
{
	dpc_disable(dev);
}

/* [한국어] portdrv 에 등록할 서비스 드라이버 서술자.
 * 이 구조체가 이 파일과 포트 버스 계층을 잇는 유일한 접점이다 */
static struct pcie_port_service_driver dpcdriver = {
	/* [한국어] sysfs 와 로그에 보이는 서비스 이름 */
	.name		= "dpc",
	.port_type	= PCIE_ANY_PORT,
	.service	= PCIE_PORT_SERVICE_DPC,
	.probe		= dpc_probe,
	.suspend	= dpc_suspend,
	.resume		= dpc_resume,
	.remove		= dpc_remove,
};

/* [한국어]
 * pcie_dpc_init - DPC 포트 서비스 드라이버를 등록한다(모듈 초기화)
 *
 * @return: pcie_port_service_register() 의 반환값(0 = 성공)
 *
 * __init 이며, 아래 dpcdriver 를 portdrv 에 등록한다. 등록되면 이후
 * 조건에 맞는 포트마다 dpc_probe() 가 불린다.
 *
 * aer.c 의 pcie_aer_init() 과 달리 사전 검사가 없다. AER 쪽은
 * pci_aer_available() 로 "pci=noaer" 와 MSI 유무를 먼저 보지만, DPC 에는
 * 그런 전역 스위치가 없고 소유권 판정은 dpc_probe() 가 포트마다 한다.
 *
 * 선언은 drivers/pci/pcie/portdrv.h:142 에 있고, CONFIG 가 꺼져 있으면
 * 같은 헤더 144행의 인라인 스텁이 0 을 돌려준다. 호출자는 portdrv 의
 * 초기화 경로다(portdrv.h:63 의 주석이 pcie_aer_init/pcie_hp_init/
 * pcie_pme_init 과 나란히 불리는 것으로 적고 있다).
 *
 * 실행 컨텍스트: 부팅 시 초기화, 단일 스레드.
 *
 * 호출 체인:
 *   (portdrv 초기화) → [이 함수] → pcie_port_service_register()
 */
int __init pcie_dpc_init(void)
{
	return pcie_port_service_register(&dpcdriver);
}
