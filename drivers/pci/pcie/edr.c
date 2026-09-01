// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Error Disconnect Recover support
 * Author: Kuppuswamy Sathyanarayanan <sathyanarayanan.kuppuswamy@linux.intel.com>
 *
 * Copyright (C) 2020 Intel Corp.
 */

/*
 * [한국어 설명] 펌웨어가 DPC 를 소유한 경우의 오류 복구 (edr.c)
 *
 * === 파일의 역할 ===
 * DPC(Downstream Port Containment)로 링크가 끊긴 뒤의 복구는 보통 커널이
 * 직접 한다(pcie/dpc.c). 그런데 펌웨어가 DPC 소유권을 커널에게 넘기지 않는
 * 플랫폼이 있다. 그 경우 커널은 DPC 를 제어할 수 없고, 대신 펌웨어가 ACPI
 * 알림으로 사건을 알려 준다. 그 알림을 받아 복구를 진행하는 것이
 * EDR(Error Disconnect Recover)이며 이 파일의 구현이다.
 * 절차가 펌웨어와의 협업이라 독특하다.
 *   1) 하위에서 치명적 오류 → 펌웨어가 DPC 로 링크를 격리한다.
 *   2) 펌웨어가 ACPI Notify(0x0F, ACPI_NOTIFY_DISCONNECT_RECOVER)를 보낸다.
 *   3) [이 파일] edr_handle_event() 가 그것을 받는다.
 *   4) _DSM(리비전 5, 함수 0x0D = EDR_PORT_LOCATE_DSM)으로 "어느 포트에서
 *      일어난 일인가" 를 묻는다. 알림을 받은 장치와 사건이 난 포트가 다를 수
 *      있고, 그것은 펌웨어만 알기 때문이다.
 *   5) 커널이 pcie_do_recovery() 로 표준 복구 절차를 진행한다.
 *      하위 드라이버의 error_detected / slot_reset / resume 콜백이 여기서 불린다.
 *   6) 결과를 _OST(ACPI_NOTIFY_DISCONNECT_RECOVER)로 펌웨어에 보고한다.
 *      성공(0x80)을 보고해야 펌웨어가 DPC 트리거를 풀어 링크를 되살린다.
 * 6번이 이 파일의 존재 이유다. 커널이 DPC 상태를 직접 지울 수 없으므로,
 * 복구 결과를 펌웨어에게 보고하고 펌웨어가 마무리하게 해야 한다.
 * 펌웨어와 주고받는 수단이 두 종류라는 점에 주의할 만하다. _DSM 은 "무언가를
 * 해 달라" 는 요청(DPC 를 켜라, 포트를 알려 달라)이고, _OST 는 "이렇게
 * 되었다" 는 통보(복구 성공/실패)다. 결과 보고에 _DSM 을 쓰지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 등록: pci-acpi.c 의 pci_acpi_setup() 이 PCI 장치와 ACPI 장치를 이을 때
 *         → [이 파일] pci_acpi_add_edr_notifier()
 *            → 알림 핸들러를 먼저 달고 → acpi_enable_dpc() 로 DPC 를 켠다.
 *              순서가 반대면 알림을 받을 수 없는 사이에 사건이 날 수 있다.
 * 발생: 펌웨어의 ACPI Notify(0x0F)
 *         → [이 파일] edr_handle_event()
 *            → acpi_dpc_port_get() 으로 문제의 포트를 알아낸다
 *            → DPC 기능 유무와 트리거 비트를 확인한다
 *            → dpc_process_error()[dpc.c:840] 로 어떤 오류였는지 남기고
 *            → pci_aer_raw_clear_status()[aer.c:998] 로 AER 상태를 지우고
 *            → pcie_do_recovery()[err.c] 로 표준 복구
 *            → acpi_send_edr_status() 로 결과 보고
 * 해제: pci_acpi_cleanup() → [이 파일] pci_acpi_remove_edr_notifier()
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 이 파일에는 워크큐가 없고,
 * edr_handle_event() 가 알림 문맥에서 복구까지 동기적으로 끝낸다. ACPI 평가와
 * 복구 과정 모두 잠들 수 있는 자리다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: drivers/acpi 의 알림 전달(이 스파스 체크아웃에는 없다), 그리고
 * pci-acpi.c 가 이 파일의 두 진입점을 부른다.
 * 아래쪽: pcie/err.c 의 pcie_do_recovery(), pcie/dpc.c 의 dpc_process_error()
 * (:840)와 dpc_reset_link()(:553), pcie/aer.c 의 pci_aer_raw_clear_status()
 * (:998), pci.c 의 pcie_clear_device_status()(:4413).
 * 커널이 DPC 를 소유하지 않아도 상태 레지스터를 읽고 AER 비트를 지우는 것은
 * 가능하다는 전제 위에 이 조합이 성립한다.
 * 옆쪽: pcie/dpc.c — 소유권에 따라 둘 중 하나만 동작한다. dpc.c 의 probe 가
 * pcie_aer_is_native() 등으로 소유권을 확인하고, 커널이 소유하지 않으면
 * 물러나 이 파일 쪽 경로가 쓰인다.
 * 펌웨어 인터페이스: pci_acpi_dsm_guid 아래의 함수 번호 셋 —
 * EDR_PORT_DPC_ENABLE_DSM(0x0C, 리비전 6)과 EDR_PORT_LOCATE_DSM(0x0D,
 * 리비전 5). 두 기능이 PCI 펌웨어 규격의 다른 절(4.6.12 와 4.6.13)에서
 * 정의되어 리비전이 다르다.
 * 공유 상태: 없다. 전역 변수도 static 변수도 두지 않으며, 상태는 알림
 * 핸들러에 넘겨 둔 pci_dev 포인터 하나뿐이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - edr_handle_event()           : 몸통. 알림 종류 확인 → 포트 조회 →
 *   DPC 기능·트리거 검사 → 복구 → 결과 보고. estate 초기값이
 *   PCI_ERS_RESULT_DISCONNECT 라, 검사에 걸려 건너뛰면 자동으로 실패가
 *   보고된다.
 * - acpi_dpc_port_get()          : _DSM 으로 격리가 난 포트를 알아낸다.
 *   답이 셋으로 갈린다 — 기능이 없거나 평가가 실패하면 자기 자신(규격이
 *   그렇게 정한다), 답의 형식이 깨졌으면 NULL(신뢰할 수 없는 답으로 엉뚱한
 *   포트를 복구하느니 물러난다), 정상이면 그 BDF 의 장치. 세 경로 모두
 *   참조를 올려 반환하며 호출자가 put 한다.
 * - acpi_send_edr_status()       : _OST 로 결과를 보고한다. 상위 16비트가
 *   오류가 난 장치의 BDF, 하위 16비트가 상태 코드. pdev 로 평가하면서
 *   edev 의 BDF 를 싣는 비대칭이 "알림 받은 장치 ≠ 사건이 난 포트" 를
 *   그대로 반영한다.
 * - acpi_enable_dpc()            : 설치 시 한 번, _DSM 으로 DPC 를 켜 달라고
 *   요청한다. 선택 사항이라 "없으면 성공" 이고, 답이 이상할 때만 -EIO 다.
 *   형식 위반에는 FW_BUG 를, 단순 거부에는 붙이지 않는 구분이 있다.
 * - pci_acpi_add_edr_notifier() / pci_acpi_remove_edr_notifier() : 바깥으로
 *   열린 두 진입점. 설치는 "핸들러 먼저, DPC 나중" 순서이며 DPC 켜기가
 *   실패하면 핸들러를 되감는다. 해제는 핸들러만 떼고 DPC 를 끄지는 않는다.
 * - 구조체 정의는 없다. 상수 넷(_DSM 함수 번호 둘, _OST 상태 코드 둘)이
 *   이 파일이 정의하는 전부다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 관계는 dpc.c 와 같다 — NVMe SSD 에서 치명적 오류가 나거나 예고 없이
 * 뽑혔을 때 링크가 격리되고, 그 복구 과정에서 NVMe 가 등록한
 * error_detected / slot_reset / resume 콜백이 pcie_do_recovery() 안에서
 * 불린다. 차이는 그 절차를 커널이 주도하느냐(dpc.c) 펌웨어와
 * 협업하느냐(이 파일)뿐이고, NVMe 쪽에서 보이는 콜백 순서는 동일하다.
 * 실무적으로는 어느 쪽이 동작하는지가 플랫폼에 달려 있다. dev_fmt 접두사가
 * 그 구별을 도와준다 — dmesg 에 "DPC: " 로 시작하는 줄이 보이면 dpc.c 가,
 * "EDR: " 로 시작하면 이 파일이 처리한 것이다.
 */

/* [한국어] 이 파일에서 나가는 모든 pci_ 계열 메시지 앞에 "EDR: " 를 붙인다.
 * dmesg 에서 dpc.c 가 처리한 사건("DPC: ...")과 이 파일이 처리한 사건을
 * 구별하는 표식이 된다. */
#define dev_fmt(fmt) "EDR: " fmt

/* [한국어] struct pci_dev, PCI_DEVID(), PCI_BUS_NUM(), pci_get_domain_bus_and_slot(),
 * 그리고 PCI_EXP_DPC_STATUS 계열 레지스터 상수. */
#include <linux/pci.h>
/* [한국어] ACPI_COMPANION(), pci_acpi_dsm_guid, acpi_check_dsm(), acpi_evaluate_dsm(),
 * acpi_evaluate_ost(), ACPI_NOTIFY_DISCONNECT_RECOVER — 이 파일이 펌웨어와
 * 주고받는 모든 수단이 여기서 온다. */
#include <linux/pci-acpi.h>

/* [한국어] pcie_do_recovery() 선언. 표준 복구 절차의 진입점이다. */
#include "portdrv.h"
/* [한국어] dpc_process_error() / dpc_reset_link() / pci_aer_raw_clear_status() /
 * pcie_clear_device_status() 등 PCI 코어 내부 선언. */
#include "../pci.h"

/* [한국어] DPC 를 켜 달라고 펌웨어에 요청하는 _DSM 함수 번호. 리비전 6 에 속한다.
 * 이름과 달리 이 상수는 알림 핸들러를 설치할 때 한 번 쓰이고,
 * 오류가 났을 때 쓰이는 것이 아니다. */
#define EDR_PORT_DPC_ENABLE_DSM		0x0C
/* [한국어] 어느 포트에서 격리가 일어났는지 묻는 _DSM 함수 번호. 리비전 5 에 속한다.
 * 위 상수와 리비전이 다르다는 점이 눈에 띄는데, 두 기능이 PCI 펌웨어
 * 규격의 서로 다른 절에서 정의되었기 때문이다(각각 4.6.12 와 4.6.13). */
#define EDR_PORT_LOCATE_DSM		0x0D
/* [한국어] 복구에 성공했음을 알리는 _OST 상태 코드. */
#define EDR_OST_SUCCESS			0x80
/* [한국어] 복구에 실패했음을 알리는 _OST 상태 코드. 이 두 값은 _DSM 이 아니라
 * _OST 로 보낸다 — 아래 acpi_send_edr_status() 를 보라. */
#define EDR_OST_FAILED			0x81
/*
 * _DSM wrapper function to enable/disable DPC
 * @pdev   : PCI device structure
 *
 * returns 0 on success or errno on failure.
 */
/* [한국어]
 * acpi_enable_dpc - 펌웨어에 DPC 를 켜 달라고 _DSM 으로 요청한다
 *
 * @pdev: 대상 루트 포트 또는 다운스트림 포트.
 * @return: 0 = 성공(또는 이 기능이 없어 건너뜀), -EIO = 펌웨어가 거부하거나
 *   형식이 어긋남.
 *
 * EDR 이 성립하려면 두 가지가 갖춰져야 한다. 커널이 알림을 받을 준비가 되어
 * 있어야 하고, 펌웨어가 DPC 를 실제로 켜 두어야 한다. 이 함수는 그 둘째다.
 *
 * "없으면 성공" 이 두 번 나온다. 함수 안의 영어 주석이 밝히듯 PCI 펌웨어
 * 규격 3.3 절 4.6.12 에서 이 _DSM 은 선택 사항이라, 지원하지 않는 플랫폼을
 * 오류로 취급하면 안 되기 때문이다. acpi_check_dsm() 이 없다고 답할 때와
 * acpi_evaluate_dsm() 이 NULL 을 줄 때 모두 0 을 반환한다.
 *
 * 반면 답을 하기는 했는데 그 답이 이상하면 -EIO 다. 정수가 아니면 FW_BUG 를
 * 붙여 펌웨어 규격 위반임을 표시하고, 정수인데 1 이 아니면 붙이지 않는다 —
 * 후자는 형식은 맞고 결과만 거부이므로 규격 위반이 아니기 때문이다.
 * 같은 오류 코드에 로그 성격을 달리하는 이 구분이 이 함수의 세밀한 지점이다.
 *
 * 두 검사 모두 status 를 덮어쓰고 조기 반환하지 않는데, ACPI_FREE 를 한 곳에서
 * 처리하기 위해서다.
 *
 * 실행 컨텍스트: 알림 핸들러 설치 경로. 프로세스 컨텍스트이며 ACPI 평가가
 * 잠들 수 있다.
 *
 * 에러 경로: -EIO 를 받은 호출자는 방금 단 알림 핸들러를 도로 뗀다.
 *
 * 호출 체인:
 *   pci_acpi_add_edr_notifier() → [이 함수]
 *     → acpi_check_dsm() → acpi_evaluate_dsm(rev 6, fn 0x0C) → ACPI_FREE()
 */
static int acpi_enable_dpc(struct pci_dev *pdev)
{
	/* [한국어] 이 PCI 장치에 대응하는 ACPI 장치를 얻는다. 펌웨어와 대화하려면
	 * ACPI 네임스페이스 핸들이 필요하기 때문이다. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	/* [한국어] obj 는 _DSM 의 반환값, argv4 와 req 는 인자를 담을 그릇이다. */
	union acpi_object *obj, argv4, req;
	/* [한국어] 반환할 상태. 기본값이 0(성공)이다. */
	int status = 0;

	/*
	 * Per PCI Firmware r3.3, sec 4.6.12, EDR_PORT_DPC_ENABLE_DSM is
	 * optional. Return success if it's not implemented.
	 */
	/* [한국어] 위 영어 주석대로 이 _DSM 은 선택 사항이다. 지원 여부를 먼저 물어
	 * 없으면 오류가 아니라 성공으로 친다. */
	if (!acpi_check_dsm(adev->handle, &pci_acpi_dsm_guid, 6,
			    1ULL << EDR_PORT_DPC_ENABLE_DSM))
		/* [한국어] 구현하지 않은 플랫폼이면 그냥 성공. */
		return 0;

	/* [한국어] 인자는 정수 하나다. */
	req.type = ACPI_TYPE_INTEGER;
	/* [한국어] 값 1 = "DPC 를 켜라". */
	req.integer.value = 1;

	/* [한국어] _DSM 의 네 번째 인자는 언제나 패키지여야 한다는 것이 ACPI 규약이다. */
	argv4.type = ACPI_TYPE_PACKAGE;
	/* [한국어] 원소 하나짜리 패키지. */
	argv4.package.count = 1;
	/* [한국어] 그 원소가 위에서 만든 정수다. */
	argv4.package.elements = &req;

	/* [한국어] _DSM 을 평가한다. 리비전 6, 함수 0x0C 로 DPC 활성화를 요청한다. */
	obj = acpi_evaluate_dsm(adev->handle, &pci_acpi_dsm_guid, 6,
				EDR_PORT_DPC_ENABLE_DSM, &argv4);
	/* [한국어] 평가 자체가 실패하면(메서드가 없거나 오류), */
	if (!obj)
		/* [한국어] 성공으로 친다. 위와 같은 이유 — 이 기능은 선택 사항이다. */
		return 0;

	/* [한국어] 반환값이 정수가 아니면 펌웨어 버그다. */
	if (obj->type != ACPI_TYPE_INTEGER) {
		/* [한국어] FW_BUG 접두사를 붙여 로그에 남긴다. 커널이 아니라 펌웨어 쪽 문제임을
		 * 표시하는 관례다. */
		pci_err(pdev, FW_BUG "Enable DPC _DSM returned non integer\n");
		/* [한국어] 입출력 오류로 표시한다. */
		status = -EIO;
	}

	/* [한국어] 정수이긴 한데 1 이 아니면 펌웨어가 켜기를 거부한 것이다. */
	if (obj->integer.value != 1) {
		/* [한국어] 이쪽은 FW_BUG 를 붙이지 않는다. 형식은 맞고 결과만 실패이므로
		 * 규격 위반이 아니기 때문이다. */
		pci_err(pdev, "Enable DPC _DSM failed to enable DPC\n");
		/* [한국어] 같은 오류 코드. */
		status = -EIO;
	}

	/* [한국어] ACPI 가 할당한 객체를 반환한다. 두 검사 모두 통과했든 아니든
	 * 반드시 여기서 놓아야 누수가 없다. */
	ACPI_FREE(obj);

	/* [한국어] 성공이면 0, 위 두 검사 중 하나라도 걸렸으면 -EIO. */
	return status;
}
/*
 * _DSM wrapper function to locate DPC port
 * @pdev   : Device which received EDR event
 *
 * Returns pci_dev or NULL.  Caller is responsible for dropping a reference
 * on the returned pci_dev with pci_dev_put().
 */
/* [한국어]
 * acpi_dpc_port_get - 격리가 실제로 일어난 포트를 펌웨어에 물어 알아낸다
 *
 * @pdev: EDR 알림을 받은 장치.
 * @return: 격리를 겪은 포트(참조가 올라간 상태). NULL 이면 펌웨어의 답을
 *   신뢰할 수 없다는 뜻이다.
 *
 * 알림을 받은 장치와 사건이 일어난 포트가 다를 수 있다는 것이 EDR 의
 * 까다로운 점이다. 알림은 부모 쪽으로 올라올 수 있고, 어느 포트였는지는
 * 펌웨어만 안다. 그래서 물어봐야 한다.
 *
 * 세 갈래의 답이 있고 각각 다르게 처리한다.
 * 첫째, _DSM 이 아예 없거나 평가가 실패하면 pdev 자신을 반환한다. 함수 안의
 * 영어 주석대로 PCI 펌웨어 규격 3.3 절 4.6.13 이 그때는 알림을 받은 포트가
 * 곧 그 포트라고 정하고 있기 때문이다.
 * 둘째, 답이 정수가 아니거나 31번 비트로 실패를 표시하면 NULL 이다. 이때는
 * pdev 로 대체하지 않는데, 펌웨어가 답을 하려다 실패한 것이라 엉뚱한 포트를
 * 복구하느니 아무것도 하지 않는 편이 안전하기 때문이다.
 * 셋째, 정상이면 하위 16비트의 BDF 로 장치를 찾는다.
 *
 * 세 경로 모두 참조를 올려 반환한다는 규약이 일치한다 — pci_dev_get() 이든
 * pci_get_domain_bus_and_slot() 이든 마찬가지다. 위 영어 주석대로 호출자가
 * pci_dev_put() 할 책임을 진다.
 *
 * 실행 컨텍스트: ACPI 알림 문맥. 프로세스 컨텍스트이며 ACPI 평가가 잠들 수 있다.
 *
 * 에러 경로: NULL 을 받은 호출자는 _OST 도 보내지 않고 물러난다.
 * 어느 포트에 대한 결과인지 말할 수 없기 때문이다.
 *
 * 호출 체인:
 *   edr_handle_event() → [이 함수]
 *     → acpi_check_dsm() → acpi_evaluate_dsm(rev 5, fn 0x0D)
 *     → pci_get_domain_bus_and_slot()
 */
static struct pci_dev *acpi_dpc_port_get(struct pci_dev *pdev)
{
	/* [한국어] ACPI 장치 핸들. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	/* [한국어] _DSM 반환값. */
	union acpi_object *obj;
	/* [한국어] 펌웨어가 알려 줄 BDF 를 담을 곳. */
	u16 port;

	/*
	 * If EDR_PORT_LOCATE_DSM is not implemented under the target of
	 * EDR, the target is the port that experienced the containment
	 * event (PCI Firmware r3.3, sec 4.6.13).
	 */
	/* [한국어] 이 _DSM 도 선택 사항이다. 위 영어 주석이 그때의 규칙을 밝힌다 —
	 * 구현되어 있지 않으면 알림을 받은 그 포트가 곧 격리를 겪은 포트다. */
	if (!acpi_check_dsm(adev->handle, &pci_acpi_dsm_guid, 5,
			    1ULL << EDR_PORT_LOCATE_DSM))
		/* [한국어] 그러므로 자기 자신을 반환한다. 참조를 올려서 주는 것이 이 함수의 규약이라
		 * (위 영어 주석대로 호출자가 pci_dev_put 해야 한다) 여기서도 get 을 쓴다. */
		return pci_dev_get(pdev);

	/* [한국어] 리비전 5, 함수 0x0D 로 포트 위치를 묻는다. */
	obj = acpi_evaluate_dsm(adev->handle, &pci_acpi_dsm_guid, 5,
				EDR_PORT_LOCATE_DSM, NULL);
	/* [한국어] 평가가 실패하면, */
	if (!obj)
		/* [한국어] 위와 같은 규칙으로 자기 자신을 반환한다. */
		return pci_dev_get(pdev);

	/* [한국어] 정수가 아니면 펌웨어 버그다. */
	if (obj->type != ACPI_TYPE_INTEGER) {
		/* [한국어] 객체를 먼저 놓는다. 아래 로그보다 앞에 두어 어느 경로로 나가든
		 * 누수가 없게 한다. */
		ACPI_FREE(obj);
		/* [한국어] FW_BUG 로 기록하고, */
		pci_err(pdev, FW_BUG "Locate Port _DSM returned non integer\n");
		/* [한국어] NULL 을 반환한다 — 자기 자신으로 대체하지 않는다. 형식이 깨졌다는 것은
		 * 펌웨어의 답을 신뢰할 수 없다는 뜻이라, 엉뚱한 포트를 복구하느니
		 * 아무것도 하지 않는 편이 낫기 때문이다. */
		return NULL;
	}
	/*
	 * Bit 31 represents the success/failure of the operation. If bit
	 * 31 is set, the operation failed.
	 */
	/* [한국어] 위 영어 주석대로 31번 비트가 서 있으면 펌웨어가 조회에 실패한 것이다. */
	if (obj->integer.value & BIT(31)) {
		/* [한국어] 객체를 놓고, */
		ACPI_FREE(obj);
		/* [한국어] 기록한 뒤, */
		pci_err(pdev, "Locate Port _DSM failed\n");
		/* [한국어] 역시 NULL. 여기서도 자기 자신으로 대체하지 않는 이유는 같다. */
		return NULL;
	}

	/*
	 * Firmware returns DPC port BDF details in following format:
	 *	15:8 = bus
	 *	 7:3 = device
	 *	 2:0 = function
	 */
	/* [한국어] 하위 16비트가 BDF 다. 위 영어 주석의 표대로 15:8 이 버스,
	 * 7:3 이 장치, 2:0 이 기능 번호다. */
	port = obj->integer.value;

	/* [한국어] BDF 를 꺼냈으니 객체를 놓는다. */
	ACPI_FREE(obj);

	/* [한국어] 같은 도메인 안에서 그 BDF 의 장치를 찾는다. PCI_BUS_NUM(port) 이
	 * 상위 8비트를, port & 0xff 가 devfn(장치+기능)을 뽑아낸다.
	 * 이 함수도 참조를 올려 반환하므로 위 두 경로와 규약이 일치한다. */
	return pci_get_domain_bus_and_slot(pci_domain_nr(pdev->bus),
					   PCI_BUS_NUM(port), port & 0xff);
}
/*
 * _OST wrapper function to let firmware know the status of EDR event
 * @pdev   : Device used to send _OST
 * @edev   : Device which experienced EDR event
 * @status : Status of EDR event
 */
/* [한국어]
 * acpi_send_edr_status - 복구 결과를 _OST 로 펌웨어에 보고한다
 *
 * @pdev: _OST 를 평가할 장치. 알림을 받은 쪽이다.
 * @edev: 실제로 오류를 겪은 장치. 위와 다를 수 있다.
 * @status: EDR_OST_SUCCESS(0x80) 또는 EDR_OST_FAILED(0x81).
 * @return: 0 = 평가 성공, -EINVAL = ACPI 평가 실패.
 *
 * 이 파일 전체가 존재하는 이유가 이 한 번의 보고다. 커널이 DPC 를 소유하지
 * 않으므로 트리거 비트를 직접 지울 수 없고, 링크를 되살리는 것은 펌웨어의
 * 몫이다. 펌웨어는 이 보고를 받고서야 움직인다. 성공을 보고하면 링크를
 * 되살리고, 실패를 보고하면 그대로 둔다.
 *
 * _DSM 이 아니라 _OST 를 쓰는 것이 이 함수의 특징이다. _DSM 이 "무언가를
 * 해 달라" 는 요청이라면 _OST(OSPM Status Indication)는 "이렇게 되었다" 는
 * 통보다. 알림 종류로 EDR 과 같은 ACPI_NOTIFY_DISCONNECT_RECOVER(0x0F)를
 * 넘겨, 어떤 사건에 대한 답인지 밝힌다.
 *
 * 값의 구성은 상위 16비트가 edev 의 BDF, 하위 16비트가 상태 코드다.
 * pdev 로 평가하면서 edev 의 BDF 를 실어 보내는 이 비대칭이, 알림을 받은
 * 장치와 사건이 일어난 포트가 다를 수 있다는 사정을 그대로 반영한다.
 *
 * [상류 코드 관찰] 두 호출자 모두 이 함수의 반환값을 확인하지 않는다.
 * 보고가 실패해도 할 수 있는 일이 없기 때문으로 보인다.
 *
 * 실행 컨텍스트: ACPI 알림 문맥. 프로세스 컨텍스트.
 *
 * 에러 경로: -EINVAL 을 반환하지만 아무도 보지 않는다.
 *
 * 호출 체인:
 *   edr_handle_event() → [이 함수] → acpi_evaluate_ost()
 */
static int acpi_send_edr_status(struct pci_dev *pdev, struct pci_dev *edev,
				u16 status)
{
	/* [한국어] _OST 를 평가할 ACPI 장치. 알림을 받은 쪽(pdev)이지 오류가 난 쪽(edev)이
	 * 아니라는 점이 중요하다 — 둘은 다를 수 있다. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	/* [한국어] _OST 에 넘길 32비트 값. */
	u32 ost_status;

	/* [한국어] 어느 장치에 어떤 상태를 보고하는지 디버그 로그에 남긴다. */
	pci_dbg(pdev, "Status for %s: %#x\n", pci_name(edev), status);

	/* [한국어] 상위 16비트에 오류가 난 장치의 BDF 를 넣는다. 펌웨어는 이 값을 보고
	 * 어느 포트의 결과인지 안다. */
	ost_status = PCI_DEVID(edev->bus->number, edev->devfn) << 16;
	/* [한국어] 하위 16비트가 상태 코드(0x80 성공 / 0x81 실패)다. */
	ost_status |= status;

	/* [한국어] _OST(OSPM Status Indication)를 평가한다. _DSM 이 "무언가를 해 달라" 는
	 * 요청이라면 _OST 는 "이렇게 되었다" 는 통보다. 그래서 결과 보고에는
	 * _OST 를 쓴다. 알림 종류로 EDR 과 같은 0x0F 를 넘겨 어떤 사건에 대한
	 * 답인지 밝힌다. */
	status = acpi_evaluate_ost(adev->handle, ACPI_NOTIFY_DISCONNECT_RECOVER,
				   ost_status, NULL);
	/* [한국어] 평가가 실패하면, */
	if (ACPI_FAILURE(status))
		/* [한국어] 잘못된 인자로 보고한다. 다만 이 함수의 반환값을 호출자가 확인하지 않으므로
		 * 실제로는 로그만 남는 셈이다. */
		return -EINVAL;

	/* [한국어] 성공. */
	return 0;
}
/* [한국어]
 * edr_handle_event - EDR 알림을 받아 복구를 진행하고 결과를 보고한다
 *
 * @handle: 알림이 온 ACPI 핸들. 실제로는 쓰지 않는다.
 * @event: 알림 종류. ACPI_NOTIFY_DISCONNECT_RECOVER(0x0F)만 처리한다.
 * @data: 핸들러를 설치할 때 넘겨 둔 pci_dev.
 *
 * 이 파일의 몸통이다. 펌웨어가 DPC 로 링크를 격리한 뒤 보내는 알림을 받아,
 * 표준 복구 절차를 돌리고, 결과를 다시 펌웨어에 보고한다.
 *
 * 절차는 다섯 단계다.
 * 1. 우리 알림인지 확인한다. 같은 핸들에 다른 시스템 알림도 오기 때문이다.
 * 2. acpi_dpc_port_get() 으로 어느 포트였는지 알아낸다. 알림을 받은 장치와
 *    다를 수 있어 물어봐야 한다.
 * 3. 두 가지를 검사한다 — 그 포트에 DPC 기능이 있는가, 그리고 트리거 비트가
 *    실제로 서 있는가. 하나라도 어긋나면 복구를 건너뛰고 결과 보고로 간다.
 *    estate 초기값이 PCI_ERS_RESULT_DISCONNECT 인 것이 이 두 경로를 위한 것으로,
 *    건너뛰면 자동으로 실패가 보고된다.
 * 4. dpc_process_error() 로 어떤 오류였는지 로그를 남기고, AER 상태를 지운 뒤
 *    pcie_do_recovery() 를 부른다. 함수 안의 영어 주석대로 언제나
 *    pci_channel_io_frozen 으로 부르는데, DPC 를 촉발한 것이 ERR_FATAL 이든
 *    ERR_NONFATAL 이든 링크가 이미 내려가 있어 구분이 의미가 없기 때문이다.
 *    NVMe 를 비롯한 하위 드라이버의 error_detected / slot_reset / resume
 *    콜백이 이 안에서 불린다.
 * 5. 성공이면 장치 상태를 지우고 _OST 로 성공을, 실패면 실패를 보고한다.
 *    상태 비트를 성공 경로에서만 지우는 것은 실패했을 때 그 비트가 진단에
 *    필요하기 때문이다.
 *
 * 이 파일에는 워크큐가 없다. 알림 문맥에서 복구까지 동기적으로 끝낸다.
 *
 * 참조 관리는 한 곳에 모여 있다. acpi_dpc_port_get() 이 올린 참조를 함수의
 * 유일한 정상 출구에서 놓으며, send_ost 로 건너뛴 두 경로도 그 자리를 지난다.
 * edev 가 NULL 인 경로만 그 앞에서 돌아가는데, 그때는 놓을 참조가 없다.
 *
 * 실행 컨텍스트: ACPI 알림 핸들러. 프로세스 컨텍스트이며, 복구 과정에서
 * 잠들 수 있다.
 *
 * 에러 경로: 포트를 찾지 못하면 _OST 도 보내지 않고 물러난다. 그 밖의 실패는
 * 모두 EDR_OST_FAILED 로 보고되어 펌웨어가 링크를 되살리지 않는다.
 *
 * 호출 체인:
 *   펌웨어 ACPI Notify(0x0F) → ACPI 코어 → [이 함수]
 *     → acpi_dpc_port_get() → dpc_process_error() [dpc.c:840]
 *     → pci_aer_raw_clear_status() [aer.c:998]
 *     → pcie_do_recovery(..., dpc_reset_link [dpc.c:553]) [err.c]
 *     → pcie_clear_device_status() [pci.c:4413]
 *     → acpi_send_edr_status() → pci_dev_put()
 */
static void edr_handle_event(acpi_handle handle, u32 event, void *data)
{
	/* [한국어] data 는 핸들러를 설치할 때 넘겨 둔 pci_dev 다. edev 는 아래에서
	 * 실제로 격리를 겪은 포트를 담는다. 둘이 다를 수 있다는 것이 이 함수의 전제다. */
	struct pci_dev *pdev = data, *edev;
	/* [한국어] 복구 결과의 초기값을 실패(연결 끊김)로 둔다. 아래 send_ost 라벨로
	 * 건너뛰는 두 경로가 이 초기값을 그대로 쓴다. */
	pci_ers_result_t estate = PCI_ERS_RESULT_DISCONNECT;
	/* [한국어] DPC 상태 레지스터를 읽어 담을 곳. */
	u16 status;

	/* [한국어] 우리가 기다리는 알림이 아니면, */
	if (event != ACPI_NOTIFY_DISCONNECT_RECOVER)
		/* [한국어] 아무 일도 하지 않는다. 같은 핸들에 다른 종류의 시스템 알림도 오기 때문이다. */
		return;

	/*
	 * pdev is a Root Port or Downstream Port that is still present and
	 * has triggered a containment event, e.g., DPC, so its child
	 * devices have been disconnected (ACPI r6.5, sec 5.6.6).
	 */
	/* [한국어] 위 영어 주석이 pdev 의 성격을 밝힌다 — 아직 살아 있는 루트 포트나
	 * 다운스트림 포트이고, 그 아래 장치들이 끊긴 상태다. */
	pci_info(pdev, "EDR event received\n");

	/*
	 * Locate the port that experienced the containment event.  pdev
	 * may be that port or a parent of it (PCI Firmware r3.3, sec
	 * 4.6.13).
	 */
	/* [한국어] 어느 포트에서 일어난 일인지 펌웨어에 묻는다. 참조를 올려 받으므로
	 * 이 함수의 모든 출구에서 놓아야 한다. */
	edev = acpi_dpc_port_get(pdev);
	/* [한국어] 펌웨어가 답하지 못했으면, */
	if (!edev) {
		/* [한국어] 기록하고, */
		pci_err(pdev, "Firmware failed to locate DPC port\n");
		/* [한국어] 돌아간다. 여기서는 edev 가 NULL 이라 놓을 참조가 없고, _OST 도 보내지
		 * 않는다 — 어느 포트에 대한 결과인지 말할 수 없기 때문이다. */
		return;
	}

	/* [한국어] 찾아낸 포트를 디버그 로그에 남긴다. */
	pci_dbg(pdev, "Reported EDR dev: %s\n", pci_name(edev));
	/* If port does not support DPC, just send the OST */
	/* [한국어] 그 포트에 DPC 기능이 없으면, */
	if (!edev->dpc_cap) {
		/* [한국어] 펌웨어가 엉뚱한 포트를 지목한 것이므로 FW_BUG 로 기록하고, */
		pci_err(edev, FW_BUG "This device doesn't support DPC\n");
		/* [한국어] 복구를 건너뛰고 결과 보고로 간다. estate 는 초기값(실패)인 채다. */
		goto send_ost;
	}
	/* Check if there is a valid DPC trigger */
	/* [한국어] DPC 상태 레지스터를 읽는다. 커널이 DPC 를 소유하지 않아도 레지스터를
	 * 읽는 것은 가능하다. */
	pci_read_config_word(edev, edev->dpc_cap + PCI_EXP_DPC_STATUS, &status);
	/* [한국어] 트리거 비트가 서 있지 않으면 실제로 격리가 일어나지 않았다는 뜻이다. */
	if (!(status & PCI_EXP_DPC_STATUS_TRIGGER)) {
		/* [한국어] 기록하고, */
		pci_err(edev, "Invalid DPC trigger %#010x\n", status);
		/* [한국어] 역시 복구를 건너뛴다. */
		goto send_ost;
	}

	/* [한국어] DPC 상태를 읽어 어떤 오류였는지 로그로 남긴다. dpc.c 와 공유하는
	 * 함수로, 커널이 DPC 를 소유하지 않는 이 경로에서도 읽기는 할 수 있다. */
	dpc_process_error(edev);
	/* [한국어] AER 상태 비트를 원시적으로 지운다. 표준 AER 경로를 거치지 않고 직접
	 * 지우는 것은, 여기서는 커널이 AER 소유자가 아닐 수 있기 때문이다. */
	pci_aer_raw_clear_status(edev);
	/*
	 * Irrespective of whether the DPC event is triggered by ERR_FATAL
	 * or ERR_NONFATAL, since the link is already down, use the FATAL
	 * error recovery path for both cases.
	 */
	/* [한국어] 표준 복구 절차. 위 영어 주석이 pci_channel_io_frozen 을 고른 이유를
	 * 밝힌다 — DPC 를 촉발한 것이 ERR_FATAL 이든 ERR_NONFATAL 이든 링크가
	 * 이미 내려가 있으므로, 둘 다 치명적 경로로 처리한다. 이 안에서 하위
	 * 드라이버들의 error_detected / slot_reset / resume 콜백이 불린다. */
	estate = pcie_do_recovery(edev, pci_channel_io_frozen, dpc_reset_link);
send_ost:

	/*
	 * If recovery is successful, send _OST(0xF, BDF << 16 | 0x80)
	 * to firmware. If not successful, send _OST(0xF, BDF << 16 | 0x81).
	 */
	/* [한국어] 복구에 성공했으면, */
	if (estate == PCI_ERS_RESULT_RECOVERED) {
		/* [한국어] 기록하고, */
		pci_dbg(edev, "DPC port successfully recovered\n");
		/* [한국어] 장치 상태 레지스터의 오류 비트를 지운다. 복구 성공 경로에서만 지우는
		 * 이유는, 실패했다면 그 비트가 진단에 필요하기 때문이다. */
		pcie_clear_device_status(edev);
		/* [한국어] 펌웨어에 성공을 보고한다. 이 보고가 이 파일의 존재 이유다 —
		 * 커널이 DPC 트리거를 직접 지울 수 없으므로, 펌웨어가 이 보고를 받고
		 * 링크를 되살려 준다. */
		acpi_send_edr_status(pdev, edev, EDR_OST_SUCCESS);
	} else {
		/* [한국어] 실패했으면 기록하고, */
		pci_dbg(edev, "DPC port recovery failed\n");
		/* [한국어] 실패를 보고한다. 그러면 펌웨어는 링크를 되살리지 않는다. */
		acpi_send_edr_status(pdev, edev, EDR_OST_FAILED);
	}

	/* [한국어] acpi_dpc_port_get() 이 올린 참조를 놓는다. 함수의 유일한 정상 출구라
	 * 이 한 줄이 send_ost 로 건너뛴 두 경로까지 함께 정리한다. */
	pci_dev_put(edev);
}

/* [한국어]
 * pci_acpi_add_edr_notifier - 이 장치에 EDR 알림 핸들러를 달고 DPC 를 켠다
 *
 * @pdev: 루트 포트 또는 다운스트림 포트.
 *
 * 이 파일의 바깥 진입점 중 하나다. pci-acpi.c 가 PCI 장치와 ACPI 장치를
 * 이을 때 불린다.
 *
 * 순서가 이 함수의 핵심이다. 알림 핸들러를 **먼저** 달고 그 다음에 DPC 를
 * 켠다. 반대로 하면 DPC 는 켜졌는데 알림을 받을 준비가 안 된 짧은 창이
 * 생기고, 그 사이에 사건이 일어나면 아무도 복구하지 않는다.
 *
 * 그 순서 때문에 되감기가 필요해진다. acpi_enable_dpc() 가 실패하면 방금 단
 * 핸들러를 도로 뗀다 — 켜지지도 않은 기능의 핸들러를 남겨 둘 이유가 없다.
 *
 * ACPI 노드가 없으면 조용히 물러난다. 오류가 아니라 이 플랫폼에서는 EDR 을
 * 쓰지 않는다는 뜻이라, pci_dbg 수준으로만 남긴다.
 *
 * 반환값이 없다는 점에 주의할 만하다. 실패해도 호출자에게 알릴 방법이 없고,
 * 그래도 되는 이유는 EDR 이 없으면 그냥 이 복구 경로가 동작하지 않을 뿐
 * 시스템이 못 쓰게 되지는 않기 때문이다.
 *
 * 실행 컨텍스트: PCI-ACPI 바인딩 경로. 프로세스 컨텍스트이며 ACPI 평가가
 * 잠들 수 있다.
 *
 * 에러 경로: 두 실패 지점 모두 로그를 남기고 돌아간다. 둘째 지점만
 * 되감기(핸들러 제거)를 한다.
 *
 * 호출 체인:
 *   pci_acpi_setup() [pci-acpi.c] → [이 함수]
 *     → acpi_install_notify_handler() → acpi_enable_dpc()
 *     → (실패 시) acpi_remove_notify_handler()
 */
void pci_acpi_add_edr_notifier(struct pci_dev *pdev)
{
	/* [한국어] ACPI 장치를 얻는다. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);
	/* [한국어] ACPI 호출 결과. */
	acpi_status status;

	/* [한국어] 대응하는 ACPI 노드가 없으면, */
	if (!adev) {
		/* [한국어] 조용히 건너뛴다. 오류가 아니라 이 플랫폼에서는 EDR 이 쓰이지 않는다는
		 * 뜻이므로 pci_dbg 수준으로만 남긴다. */
		pci_dbg(pdev, "No valid ACPI node, skipping EDR init\n");
		/* [한국어] 돌아간다. */
		return;
	}

	/* [한국어] 시스템 알림 핸들러를 단다. 이 시점부터 펌웨어가 보내는
	 * ACPI Notify 가 edr_handle_event() 로 들어온다. pdev 를 문맥으로 넘겨
	 * 핸들러가 어느 장치의 알림인지 알 수 있게 한다. */
	status = acpi_install_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY,
					     edr_handle_event, pdev);
	/* [한국어] 설치가 실패하면, */
	if (ACPI_FAILURE(status)) {
		/* [한국어] 기록하고, */
		pci_err(pdev, "Failed to install notify handler\n");
		/* [한국어] 돌아간다. DPC 활성화를 시도하지 않는 것이 중요하다 — 받을 수 없는
		 * 알림을 켜 두면 오류가 나도 아무도 처리하지 않는다. */
		return;
	}

	/* [한국어] 이제 펌웨어에 DPC 를 켜 달라고 요청한다. 순서가 이렇게 된 이유는,
	 * DPC 를 먼저 켜면 알림 핸들러가 없는 사이에 사건이 일어날 수 있기
	 * 때문이다. 핸들러부터 걸고 그 다음에 켠다. */
	if (acpi_enable_dpc(pdev))
		/* [한국어] 켜기에 실패하면 방금 단 핸들러를 도로 뗀다. 켜지지도 않은 기능의
		 * 핸들러를 남겨 둘 이유가 없다. */
		acpi_remove_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY,
					   edr_handle_event);
	else
		/* [한국어] 성공했으면 그 사실만 디버그 로그에 남긴다. */
		pci_dbg(pdev, "Notify handler installed\n");
}
/* [한국어]
 * pci_acpi_remove_edr_notifier - 달아 두었던 EDR 알림 핸들러를 뗀다
 *
 * @pdev: 정리할 장치.
 *
 * pci_acpi_add_edr_notifier() 의 짝이다. PCI 장치와 ACPI 장치의 연결이
 * 끊길 때 불린다.
 *
 * 설치 쪽과 대칭이 완전하지는 않다. 핸들러는 떼지만 "DPC 를 꺼 달라" 는
 * 요청은 하지 않는다. 장치가 사라지는 마당에 펌웨어 쪽 상태를 되돌릴 실익이
 * 없기 때문이다.
 *
 * ACPI 노드 유무 검사는 설치 쪽과 짝을 이룬다. 노드가 없었다면 애초에
 * 핸들러를 달지 않았으므로 뗄 것도 없다.
 *
 * 실행 컨텍스트: PCI-ACPI 해제 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. acpi_remove_notify_handler() 의 결과를 확인하지 않는데,
 * 실패해도 할 수 있는 일이 없기 때문이다.
 *
 * 호출 체인:
 *   pci_acpi_cleanup() [pci-acpi.c] → [이 함수] → acpi_remove_notify_handler()
 */
void pci_acpi_remove_edr_notifier(struct pci_dev *pdev)
{
	/* [한국어] ACPI 장치를 얻는다. */
	struct acpi_device *adev = ACPI_COMPANION(&pdev->dev);

	/* [한국어] 애초에 핸들러를 달지 않았으므로, */
	if (!adev)
		/* [한국어] 할 일이 없다. 설치 쪽의 같은 검사와 짝을 이룬다. */
		return;

	/* [한국어] 핸들러를 뗀다. DPC 를 꺼 달라는 요청은 하지 않는데, 장치가 사라지는
	 * 마당에 펌웨어 쪽 상태를 되돌릴 필요가 없기 때문이다. */
	acpi_remove_notify_handler(adev->handle, ACPI_SYSTEM_NOTIFY,
				   edr_handle_event);
	/* [한국어] 제거 사실을 남긴다. */
	pci_dbg(pdev, "Notify handler removed\n");
}