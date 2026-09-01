// SPDX-License-Identifier: GPL-2.0-only
/* Copyright(c) 2023 AMD Corporation. All rights reserved. */

/*
 * [한국어 설명] RCEC 내부 오류를 CXL.mem 장치 드라이버로 넘기는 다리 (aer_cxl_rch.c)
 *
 * === 파일의 역할 ===
 * CXL(Compute Express Link)의 RCH(Restricted CXL Host) 구성에서, 다운스트림
 * 포트의 오류를 CXL.mem 장치 드라이버까지 전달하는 161줄짜리 파일이다.
 * 왜 별도의 경로가 필요한가. RCH 에서는 CXL 장치가 Root Complex 에 통합된
 * 엔드포인트로 나타나고, 그 다운스트림 포트에서 난 오류가 표준 AER 경로로
 * 올라오지 않는다. 대신 RCEC(Root Complex Event Collector)의 **내부 오류**
 * 로 보고된다 — 원문 주석이 밝히는 그대로다. 표준 AER 코드는 내부 오류를
 * 장치 고유의 것으로 보아 손대지 않으므로, CXL 을 위한 해석이 따로 필요하다.
 * 이 파일이 하는 일은 두 갈래다. 하나는 오류가 났을 때 그것이 RCEC 내부
 * 오류인지 보고, 맞으면 그 RCEC 아래에서 CXL 메모리 장치를 찾아 그 장치
 * 드라이버의 err_handler 콜백을 부르는 것이다(cxl_rch_handle_error).
 * 다른 하나는 그보다 앞선 준비 작업으로, AER 서비스가 포트에 붙을 때
 * 그 RCEC 아래에 CXL 장치가 실제로 있는지 확인하고 있을 때만 내부 오류
 * 마스크를 푸는 것이다(cxl_rch_enable_rcec). 내부 오류는 보통 너무
 * 장치 고유해서 일반적으로 켜지 않지만, CXL 에서는 프로토콜 오류를 나르는
 * 방식이 표준화되어 있어 예외로 켠다(aer.c:2777 부근의 영어 주석).
 * 이 파일에는 전역 변수도, 등록 콜백 표도, 구조체 정의도 없다. 함수 일곱
 * 개가 전부이며 그중 둘만 바깥으로 열려 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 파일은 pcie/aer.c 의 두 지점에만 끼어든다.
 * (1) 오류 처리 경로: aer_process_err_devices() → handle_error_source()
 * (aer.c:2932) 가 표준 처리 pci_aer_handle_error() 보다 **먼저**
 * cxl_rch_handle_error() 를 부른다. 순서가 이렇게 된 이유는, 이 함수가
 * RCEC 내부 오류가 아니면 아무것도 하지 않고 즉시 돌아가기 때문이다 —
 * 표준 처리를 가로채지 않고 그 앞에 조용히 서 있는 구조다.
 * (2) 초기화 경로: AER 서비스 probe 가 IRQ 를 얻은 직후
 * cxl_rch_enable_rcec() 를 부른다(aer.c:3981). aer_enable_rootport() 바로
 * 앞이라, 오류 보고가 켜지기 전에 마스크가 풀린다.
 * 빌드도 조건부다. Makefile 이 CONFIG_CXL_RAS 일 때만 이 파일을 넣고,
 * 꺼져 있으면 portdrv.h:324~326 의 빈 static inline 스텁이 대신 쓰인다.
 * 즉 CXL 을 쓰지 않는 커널에서는 이 코드가 통째로 사라진다.
 * 실행 컨텍스트는 둘 다 프로세스 컨텍스트다. 오류 처리 쪽은 AER 의 스레드
 * 핸들러 안이고, 초기화 쪽은 probe 안이다. cxl_rch_handle_error_iter() 가
 * guard(device) 로 장치 락을 잡으므로 잠들 수 있는 자리여야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/aer.c 만이 이 파일의 두 진입점을 부른다. 원형은
 * pcie/portdrv.h:320~322 에 있으며, CONFIG_CXL_RAS 가 꺼지면 같은 자리의
 * 스텁으로 대체된다.
 * 옆쪽(aer.c 에서 빌려 쓰는 것): is_aer_internal_error() 는 correctable 이면
 * PCI_ERR_COR_INTERNAL, uncorrectable 이면 PCI_ERR_UNC_INTN 비트를 본다
 * (aer.c:2803). pci_aer_unmask_internal_errors() 는 그 두 비트를 각각의
 * 마스크 레지스터에서 지운다(aer.c:2749) — 같은 "내부 오류" 개념이 두
 * 레지스터에서 다른 비트를 쓴다는 점이 두 함수에 공통으로 나타난다.
 * 그 함수는 cxl_core 모듈에도 EXPORT 되어 있다(aer.c:2776).
 * 옆쪽(pci.h): pcie_walk_rcec() 은 "이 RCEC 가 담당하는 RCiEP 들" 을 훑는
 * 순회자다(pci.h:2186 의 설명). 이 파일은 그것을 두 번 쓰는데, 한 번은
 * 오류를 나르려고, 한 번은 CXL 장치가 있는지 세어 보려고 쓴다.
 * 옆쪽(pci.h / drivers/pci): pcie_ports_native 와 host->native_aer 가
 * AER 소유권을 나타낸다. 펌웨어가 AER 을 쥐고 있으면 OS 가 손대면 안 되므로,
 * 오류를 넘기기 전에 반드시 확인한다.
 * 아래쪽: CXL 장치 드라이버가 등록한 struct pci_error_handlers 의
 * cor_error_detected / error_detected 콜백. 이 파일이 따로 콜백을 등록받는
 * 창구를 두지 않는다는 점이 중요하다 — 평범한 PCI 드라이버 오류 콜백을
 * 그대로 쓴다.
 * 데이터 흐름: 하드웨어 오류 → AER 인터럽트 → aer.c 가 aer_err_info 를 채움
 * → handle_error_source() → [이 파일] 이 RCEC 내부 오류인지 판별 →
 * pcie_walk_rcec() 로 CXL 장치를 찾음 → 그 드라이버의 err_handler 호출.
 * 공유 상태: 없다. 전역 변수도 static 변수도 두지 않으며, 상태는 모두
 * 인자로 받은 pci_dev 와 aer_err_info 안에 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - cxl_rch_handle_error(): 오류 경로의 진입점. 두 조건(RCEC 타입 + 내부
 *   오류)을 모두 만족할 때만 순회를 시작하고, 아니면 조용히 돌아간다.
 * - cxl_rch_handle_error_iter(): 순회 콜백. CXL 메모리 장치이고 AER 소유권이
 *   OS 에 있을 때만, 그 드라이버의 err_handler 를 심각도에 맞게 부른다.
 *   AER_NONFATAL 은 pci_channel_io_normal 로, AER_FATAL 은
 *   pci_channel_io_frozen 으로 옮겨 전한다.
 * - cxl_rch_enable_rcec(): 초기화 경로의 진입점. CXL 장치가 실제로 있을
 *   때만 내부 오류 마스크를 푼다. 없는데 풀면 장치 고유 내부 오류가
 *   쏟아지기 때문이다.
 * - handles_cxl_errors() / handles_cxl_error_iter(): 위 판정을 위한 순회.
 *   iter 가 0 이 아닌 값을 반환하면 순회가 끝나므로, 하나만 찾으면 즉시
 *   멈춘다.
 * - is_cxl_mem_dev(): 함수 0 인지와 클래스 코드가 0x502 인지 두 가지로
 *   판정한다. 근거는 CXL 3.0 스펙 8.1.3 과 8.1.12.1(원문 주석).
 * - cxl_error_is_native(): AER 을 OS 가 소유하는지 확인한다. 펌웨어가 쥐고
 *   있으면 오류를 건드리면 안 된다.
 * - 구조체 정의는 없다. 이 파일은 남의 구조체(pci_dev, aer_err_info,
 *   pci_error_handlers)만 읽는다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. 이 파일이 다루는 것은 CXL.mem 장치이며, 판정 조건 자체가
 * 클래스 코드 0x502(CXL 메모리)라 NVMe SSD 는 is_cxl_mem_dev() 에서
 * 걸러진다. NVMe 의 AER 오류는 표준 경로 —
 * handle_error_source() → pci_aer_handle_error() → err.c 의 pcie_do_recovery()
 * → 드라이버의 error_detected — 로 처리되며 이 파일을 지나지 않는다.
 * 다만 구조는 같다. 이 파일이 CXL 장치 드라이버의 err_handler 를 부르는
 * 방식은 NVMe 드라이버가 오류를 받는 방식과 동일한 pci_error_handlers
 * 규약이다. 다른 것은 그 콜백에 이르는 경로뿐이다.
 */

/* [한국어] struct pci_dev, PCI_DEVFN(), pci_find_host_bridge(), 그리고
 * struct pci_error_handlers 규약. */
#include <linux/pci.h>
/* [한국어] struct aer_err_info 와 AER_CORRECTABLE / AER_NONFATAL / AER_FATAL 등급. */
#include <linux/aer.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/bitfield.h>
/* [한국어] pcie_walk_rcec() 과 pcie_ports_native 등 PCI 코어 내부 선언. */
#include "../pci.h"
/* [한국어] is_aer_internal_error() / pci_aer_unmask_internal_errors() 와,
 * 이 파일이 정의하는 두 진입점의 원형(:320~322). CONFIG_CXL_RAS 가 꺼지면
 * 같은 자리의 빈 스텁이 쓰인다. */
#include "portdrv.h"

/* [한국어]
 * is_cxl_mem_dev - 이 PCI 장치가 CXL 메모리 장치인지 판정한다
 *
 * @dev: 검사할 장치.
 * @return: true = CXL 메모리 장치, false = 아님.
 *
 * 두 가지만 본다. 함수 번호가 0 인가, 그리고 클래스 코드가 0x502 인가.
 * 근거는 함수 안의 원문 주석에 있다. CXL 3.0 스펙 8.1.3 에 따르면 장치 전체의
 * CXL 기능은 Device 0, Function 0 의 DVSEC 이 제어하므로 다른 함수는 볼 필요가
 * 없고, 8.1.12.1 에 따르면 CXL 메모리 장치는 클래스 코드 0x502 를 반드시 갖는다.
 *
 * 클래스 코드를 8비트 오른쪽으로 미는 것은 리비전(프로그 인터페이스) 바이트를
 * 떼어 내기 위함이다. class 필드는 [베이스 클래스][서브클래스][프로그 인터페이스]
 * 세 바이트로 되어 있어, 상위 두 바이트만 남겨야 0x502 와 비교할 수 있다.
 *
 * 이 판정이 NVMe 를 걸러 내는 지점이기도 하다. NVMe SSD 는 클래스 코드가
 * 0x108(대용량 저장장치 / NVM / NVMe)이라 여기서 false 가 된다.
 *
 * 실행 컨텍스트: 두 순회 콜백 안. 잠들지 않는다.
 *
 * 에러 경로: 없다. 판정 결과가 곧 반환값이다.
 *
 * 호출 체인:
 *   cxl_rch_handle_error_iter() / handles_cxl_error_iter() → [이 함수]
 */
static bool is_cxl_mem_dev(struct pci_dev *dev)
{
	/*
	 * The capability, status, and control fields in Device 0,
	 * Function 0 DVSEC control the CXL functionality of the
	 * entire device (CXL 3.0, 8.1.3).
	 */
	/* [한국어] 원문 주석대로 장치 전체의 CXL 기능은 Device 0, Function 0 의 DVSEC 이
	 * 제어하므로, 그 외의 함수는 볼 필요가 없다. */
	if (dev->devfn != PCI_DEVFN(0, 0))
		/* [한국어] 함수 0 이 아니면 CXL 메모리 장치가 아니다. */
		return false;

	/*
	 * CXL Memory Devices must have the 502h class code set (CXL
	 * 3.0, 8.1.12.1).
	 */
	/* [한국어] 클래스 코드 상위 16비트가 0x502(CXL 메모리)인지 본다. >> 8 은
	 * 리비전 바이트를 떼어 내는 것으로, class 필드가 [클래스][서브클래스][프로그
	 * 인터페이스] 구성이라 상위 두 바이트만 남긴다. */
	if ((dev->class >> 8) != PCI_CLASS_MEMORY_CXL)
		/* [한국어] CXL 메모리 클래스가 아니면 대상이 아니다. */
		return false;

	/* [한국어] 두 조건을 모두 통과하면 CXL 메모리 장치다. */
	return true;
}

/* [한국어]
 * cxl_error_is_native - 이 장치의 AER 오류를 OS 가 처리해도 되는지 확인한다
 *
 * @dev: 검사할 장치.
 * @return: true = OS 가 AER 을 소유함, false = 펌웨어가 쥐고 있음.
 *
 * PCIe 의 AER 은 OS 와 펌웨어 중 한쪽만 소유한다. 펌웨어가 쥐고 있는데 OS 가
 * 오류 레지스터를 건드리면 양쪽이 같은 상태를 서로 다르게 해석하게 되므로,
 * 오류를 CXL 드라이버에 넘기기 전에 반드시 확인해야 한다.
 *
 * 두 조건 중 하나만 맞으면 된다. pcie_ports_native 는 부팅 인자로 OS 소유를
 * 강제한 경우이고, host->native_aer 는 펌웨어가 _OSC 협상으로 소유권을
 * 넘겨 준 경우다.
 *
 * 실행 컨텍스트: 두 순회 콜백 안. pci_find_host_bridge() 는 버스를 거슬러
 * 올라가는 포인터 추적뿐이라 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cxl_rch_handle_error_iter() / handles_cxl_error_iter() → [이 함수]
 *     → pci_find_host_bridge()
 */
static bool cxl_error_is_native(struct pci_dev *dev)
{
	/* [한국어] 이 장치가 매달린 호스트 브리지를 찾는다. AER 소유권 정보가 거기 있다. */
	struct pci_host_bridge *host = pci_find_host_bridge(dev->bus);

	/* [한국어] AER 을 OS 가 소유하는지 판정한다. 부팅 인자로 강제되었거나
	 * (pcie_ports_native) 펌웨어가 _OSC 로 넘겨 주었으면(host->native_aer)
	 * OS 가 오류를 처리해도 된다. 펌웨어가 쥐고 있으면 손대면 안 되므로,
	 * 오류를 CXL 드라이버에 넘기기 전에 반드시 확인한다. */
	return (pcie_ports_native || host->native_aer);
}

/* [한국어]
 * cxl_rch_handle_error_iter - RCEC 아래 CXL 장치를 찾아 오류를 그 드라이버에 전한다
 *
 * @dev: 순회 중 만난 장치.
 * @data: struct aer_err_info 포인터(void 로 넘어온다).
 * @return: 언제나 0 — 순회를 끝까지 진행시킨다.
 *
 * pcie_walk_rcec() 의 콜백이다. 이 파일의 실제 일이 여기서 일어난다.
 *
 * 앞부분은 걸러 내기다. CXL 메모리 장치가 아니거나 AER 소유권이 OS 에 없으면
 * 그냥 넘어간다.
 *
 * guard(device) 가 중요하다. 바로 다음 줄에서 dev->driver 를 읽고 그
 * err_handler 콜백을 부르는데, 그 사이에 드라이버가 언바인드되면 이미 해제된
 * 메모리를 호출하게 된다. 장치 락이 그것을 막고, guard() 는 함수를 어느
 * 경로로 빠져나가든 락을 자동으로 푼다.
 *
 * 뒷부분은 등급 번역이다. AER 의 심각도를 PCI 코어의 채널 상태로 옮긴다.
 * correctable 은 전용 콜백을 부르고 채널 상태를 넘기지 않는다 — 하드웨어가
 * 이미 복구한 오류라 통신이 끊긴 것이 아니기 때문이다. AER_NONFATAL 은
 * pci_channel_io_normal(채널 살아 있음, 복구 시도 가능), AER_FATAL 은
 * pci_channel_io_frozen(접근 불가, 진행 중인 I/O 를 포기해야 함)으로 옮긴다.
 *
 * 언제나 0 을 반환하는 것은 의도된 것이다. 한 RCEC 아래에 CXL 장치가 여럿
 * 있을 수 있고 모두에게 알려야 하므로, 중간에 멈추면 안 된다. 존재 여부만
 * 확인하는 handles_cxl_error_iter() 가 찾는 즉시 멈추는 것과 정반대다.
 *
 * 실행 컨텍스트: AER 스레드 핸들러 안. 장치 락을 잡으므로 잠들 수 있는 자리다.
 *
 * 에러 경로: 없다. 넘길 드라이버가 없으면 조용히 건너뛴다.
 *
 * 호출 체인:
 *   cxl_rch_handle_error() → pcie_walk_rcec() → [이 함수]
 *     → is_cxl_mem_dev() → cxl_error_is_native()
 *     → err_handler->cor_error_detected() 또는 error_detected()
 */
static int cxl_rch_handle_error_iter(struct pci_dev *dev, void *data)
{
	/* [한국어] 순회자가 넘겨 준 오류 정보. void 포인터를 원래 타입으로 되돌린다. */
	struct aer_err_info *info = (struct aer_err_info *)data;
	/* [한국어] 이 장치 드라이버가 등록한 오류 콜백 표. */
	const struct pci_error_handlers *err_handler;

	/* [한국어] CXL 메모리 장치가 아니거나 AER 소유권이 OS 에 없으면, */
	if (!is_cxl_mem_dev(dev) || !cxl_error_is_native(dev))
		/* [한국어] 이 장치는 건너뛴다. 0 을 반환하면 순회가 계속된다. */
		return 0;

	/* [한국어] 장치 락을 잡는다. 아래에서 dev->driver 를 읽고 그 콜백을 부르는데,
	 * 그 사이에 드라이버가 언바인드되면 해제된 메모리를 부르게 된다.
	 * guard() 는 스코프를 벗어날 때 자동으로 락을 푸는 정리 매크로다. */
	guard(device)(&dev->dev);

	/* [한국어] 드라이버가 붙어 있으면 그 오류 콜백 표를, 아니면 NULL 을 얻는다.
	 * 이 읽기가 위 락 안에 있어야 하는 이유가 바로 앞 줄에 있다. */
	err_handler = dev->driver ? dev->driver->err_handler : NULL;
	/* [한국어] 콜백 표가 없으면, */
	if (!err_handler)
		/* [한국어] 넘길 곳이 없으므로 건너뛴다. */
		return 0;

	/* [한국어] 수정 가능한 오류면, */
	if (info->severity == AER_CORRECTABLE) {
		/* [한국어] 그 전용 콜백이 있을 때만, */
		if (err_handler->cor_error_detected)
			/* [한국어] 부른다. correctable 오류는 채널 상태 인자가 없다 — 통신이 끊긴 것이
			 * 아니라 하드웨어가 이미 복구한 오류이기 때문이다. */
			err_handler->cor_error_detected(dev);
	/* [한국어] 수정 불가능한 오류이고 error_detected 콜백이 있으면, */
	} else if (err_handler->error_detected) {
		/* [한국어] 치명적이지 않은 등급이면, */
		if (info->severity == AER_NONFATAL)
			/* [한국어] 채널이 살아 있다고 알린다. 드라이버는 복구를 시도할 수 있다. */
			err_handler->error_detected(dev, pci_channel_io_normal);
		/* [한국어] 치명적 등급이면, */
		else if (info->severity == AER_FATAL)
			/* [한국어] 채널이 얼어붙었다고 알린다. 이 상태에서는 장치에 접근할 수 없으므로
			 * 드라이버가 진행 중인 I/O 를 포기해야 한다. AER 등급을 PCI 코어의
			 * 채널 상태로 번역하는 것이 이 분기의 역할이다. */
			err_handler->error_detected(dev, pci_channel_io_frozen);
	}
	/* [한국어] 언제나 0 을 반환해 순회를 끝까지 진행한다. 한 RCEC 아래에 CXL 장치가
	 * 여럿 있을 수 있고, 모두에게 알려야 하기 때문이다. */
	return 0;
}

/* [한국어]
 * cxl_rch_handle_error - RCEC 내부 오류를 CXL 경로로 넘길지 판단한다
 *
 * @dev: 오류를 보고한 장치.
 * @info: aer.c 가 채운 오류 정보.
 *
 * 오류 경로에서 이 파일의 진입점이다. aer.c 의 handle_error_source() 가
 * 표준 처리 pci_aer_handle_error() 보다 **먼저** 이것을 부른다(aer.c:2932).
 *
 * 먼저 불려도 표준 처리를 가로채지 않는다. 두 조건 — 이 장치가 RCEC 인가,
 * 그리고 이번 오류가 내부 오류인가 — 을 모두 만족하지 않으면 아무 일도 하지
 * 않고 돌아가기 때문이다. 조용히 앞에 서 있다가 자기 몫일 때만 움직인다.
 *
 * 두 조건의 의미는 함수 안의 원문 주석에 있다. RCEC 의 내부 오류가 곧 RCH
 * 다운스트림 포트에서 난 AER 오류를 뜻하며, 그 오류는 CXL.mem 장치 드라이버가
 * 해석해야 한다. 표준 AER 코드는 내부 오류를 장치 고유의 것으로 보아 손대지
 * 않으므로 이 다리가 필요하다.
 *
 * is_aer_internal_error() 는 심각도에 따라 다른 비트를 본다 — correctable 이면
 * PCI_ERR_COR_INTERNAL, uncorrectable 이면 PCI_ERR_UNC_INTN(aer.c:2803).
 * 같은 개념이 두 레지스터에서 다른 자리를 쓰기 때문이다.
 *
 * CONFIG_CXL_RAS 가 꺼져 있으면 이 함수 대신 portdrv.h:325 의 빈 스텁이 쓰여,
 * 호출 자체가 사라진다.
 *
 * 실행 컨텍스트: AER 스레드 핸들러. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값도 없어 실패를 보고할 방법 자체가 없다.
 *
 * 호출 체인:
 *   AER 인터럽트 → aer_process_err_devices() → handle_error_source()(aer.c:2932)
 *     → [이 함수] → is_aer_internal_error() → pcie_walk_rcec()
 *     → cxl_rch_handle_error_iter()
 */
void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info)
{
	/*
	 * Internal errors of an RCEC indicate an AER error in an
	 * RCH's downstream port. Check and handle them in the CXL.mem
	 * device driver.
	 */
	/* [한국어] 두 조건을 함께 본다. 하나는 이 장치가 RCEC 인가, */
	if (pci_pcie_type(dev) == PCI_EXP_TYPE_RC_EC &&
	    /* [한국어] 다른 하나는 이번 오류가 내부 오류인가. 원문 주석이 밝히듯 RCEC 의
	     * 내부 오류가 곧 RCH 다운스트림 포트의 AER 오류를 뜻한다. */
	    is_aer_internal_error(info))
		/* [한국어] 이 RCEC 가 담당하는 RCiEP 들을 훑으며 CXL 장치를 찾아 오류를 전한다.
		 * 두 조건 중 하나라도 어긋나면 이 줄에 오지 않고, 함수는 아무 일도
		 * 하지 않은 채 돌아간다 — 표준 AER 처리를 가로채지 않는 설계다. */
		pcie_walk_rcec(dev, cxl_rch_handle_error_iter, info);
}

/* [한국어]
 * handles_cxl_error_iter - RCEC 아래에 CXL 장치가 있는지 표시한다
 *
 * @dev: 순회 중 만난 장치.
 * @data: bool 포인터(void 로 넘어온다). 찾았으면 true 로 바뀐다.
 * @return: 찾았으면 0 이 아닌 값 — 순회를 즉시 끝낸다.
 *
 * cxl_rch_handle_error_iter() 와 같은 걸러 내기를 쓰지만 목적이 정반대다.
 * 그쪽은 모든 CXL 장치에 오류를 전해야 해서 끝까지 돌지만, 이쪽은 "하나라도
 * 있는가" 만 알면 되므로 찾는 즉시 멈춘다. 옆의 원문 주석이 그 규약을 밝힌다 —
 * 0 이 아닌 값을 반환하면 순회가 끝난다.
 *
 * 이미 찾은 뒤에는 다시 계산하지 않는다. 사실 찾은 순간 순회가 끝나므로
 * 그 검사가 실행될 일은 없지만, 결과를 덮어쓰지 않는다는 뜻을 코드로 남긴 것이다.
 *
 * 실행 컨텍스트: AER 서비스 probe 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   handles_cxl_errors() → pcie_walk_rcec() → [이 함수]
 *     → is_cxl_mem_dev() → cxl_error_is_native()
 */
static int handles_cxl_error_iter(struct pci_dev *dev, void *data)
{
	/* [한국어] 순회자가 넘겨 준 결과 저장 위치. 여기서는 bool 포인터다. */
	bool *handles_cxl = data;

	/* [한국어] 아직 찾지 못했으면, */
	if (!*handles_cxl)
		/* [한국어] 이 장치가 조건에 맞는지 확인해 기록한다. 이미 찾았으면 다시 계산하지
		 * 않는다. */
		*handles_cxl = is_cxl_mem_dev(dev) && cxl_error_is_native(dev);

	/* Non-zero terminates iteration */
	/* [한국어] 옆의 원문 주석대로 0 이 아닌 값을 반환하면 순회가 끝난다. 즉 하나라도
	 * 찾으면 즉시 멈춘다 — 오류를 나르는 iter 가 끝까지 도는 것과 정반대다.
	 * 여기서는 존재 여부만 알면 되기 때문이다. */
	return *handles_cxl;
}

/* [한국어]
 * handles_cxl_errors - 이 RCEC 가 CXL 오류를 나를 일이 있는지 확인한다
 *
 * @rcec: 검사할 Root Complex Event Collector.
 * @return: true = 아래에 CXL 메모리 장치가 있음, false = 없거나 대상이 아님.
 *
 * cxl_rch_enable_rcec() 이 내부 오류 마스크를 풀지 말지 정하는 근거를 만든다.
 *
 * 세 겹의 조건이다. 이 장치가 RCEC 여야 하고, AER 이 네이티브여야 하며,
 * 그 아래에 CXL 메모리 장치가 실제로 있어야 한다. 앞의 둘이 어긋나면 순회를
 * 아예 시작하지 않아 초기값 false 가 그대로 반환된다.
 *
 * cxl_rch_handle_error() 가 오류 경로에서 하는 판정과 구조가 같다 —
 * RCEC 인지 먼저 보고, 그 다음 조건을 확인한 뒤에야 순회한다. 다만 이쪽은
 * 오류가 아니라 존재를 묻는다.
 *
 * 실행 컨텍스트: AER 서비스 probe 안. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cxl_rch_enable_rcec() → [이 함수] → pcie_aer_is_native()
 *     → pcie_walk_rcec() → handles_cxl_error_iter()
 */
static bool handles_cxl_errors(struct pci_dev *rcec)
{
	/* [한국어] 찾았는지 여부. 순회자에게 주소를 넘겨 채우게 한다. */
	bool handles_cxl = false;

	/* [한국어] 이 장치가 RCEC 이고, */
	if (pci_pcie_type(rcec) == PCI_EXP_TYPE_RC_EC &&
	    /* [한국어] AER 이 네이티브(OS 소유)일 때만 확인할 가치가 있다. */
	    pcie_aer_is_native(rcec))
		/* [한국어] 아래 RCiEP 들을 훑으며 CXL 장치가 하나라도 있는지 본다. */
		pcie_walk_rcec(rcec, handles_cxl_error_iter, &handles_cxl);

	/* [한국어] 결과를 반환한다. 조건을 만족하지 못했으면 초기값 false 그대로다. */
	return handles_cxl;
}

/* [한국어]
 * cxl_rch_enable_rcec - CXL 장치가 있는 RCEC 에서만 내부 오류 보고를 켠다
 *
 * @rcec: AER 서비스가 붙은 포트.
 *
 * 초기화 경로에서 이 파일의 진입점이다. AER 서비스 probe 가 IRQ 를 얻은 직후,
 * aer_enable_rootport() 로 오류 보고를 켜기 바로 전에 불린다(aer.c:3981).
 * 순서가 그렇게 잡힌 덕분에 마스크가 먼저 풀리고 보고가 나중에 켜진다.
 *
 * 핵심은 조건부라는 점이다. 내부 오류는 보통 켜지 않는다 — 장치마다 의미가
 * 너무 달라 일반적으로 해석할 수 없기 때문이다(aer.c:2777 부근의 원문 주석).
 * CXL 만 예외인 이유는 그쪽에서는 내부 오류가 프로토콜 오류를 나르는 방식으로
 * 표준화되어 있어서다. 그래서 CXL 장치가 실제로 아래에 있을 때만 푼다.
 * 없는데 풀면 해석할 수 없는 장치 고유 오류가 쏟아진다.
 *
 * pci_aer_unmask_internal_errors() 는 두 마스크 레지스터에서 각각
 * PCI_ERR_UNC_INTN 과 PCI_ERR_COR_INTERNAL 을 지운다(aer.c:2749).
 * 같은 함수가 cxl_core 모듈에도 EXPORT 되어 있다(aer.c:2776).
 *
 * 평소에 켜지 않는 설정이라 pci_info 로 흔적을 남긴다.
 *
 * 실행 컨텍스트: AER 서비스 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값이 없어 실패를 보고할 방법이 없고, config 쓰기가
 * 실패해도 알 수 없다.
 *
 * 호출 체인:
 *   AER 서비스 probe(aer.c:3981) → [이 함수] → handles_cxl_errors()
 *     → pci_aer_unmask_internal_errors() → pci_info()
 */
void cxl_rch_enable_rcec(struct pci_dev *rcec)
{
	/* [한국어] 이 RCEC 아래에 CXL 장치가 없으면, */
	if (!handles_cxl_errors(rcec))
		/* [한국어] 아무것도 하지 않는다. 없는데 내부 오류를 켜면 장치 고유의 내부 오류가
		 * 쏟아지기 때문이다 — 내부 오류를 일반적으로 켜지 않는 이유가 그것이다. */
		return;

	/* [한국어] 내부 오류 마스크를 푼다. uncorrectable 쪽 PCI_ERR_UNC_INTN 과
	 * correctable 쪽 PCI_ERR_COR_INTERNAL 두 비트를 각각의 마스크 레지스터에서
	 * 지운다(aer.c:2749). 이 시점부터 RCEC 내부 오류가 보고되기 시작한다. */
	pci_aer_unmask_internal_errors(rcec);
	/* [한국어] 켰다는 사실을 로그에 남긴다. 평소에 켜지 않는 설정이라 흔적을 남길 값어치가
	 * 있다. */
	pci_info(rcec, "CXL: Internal errors unmasked");
}
