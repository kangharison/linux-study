// SPDX-License-Identifier: GPL-2.0
/*
 * PCIe TLP Log handling
 *
 * Copyright (C) 2024 Intel Corporation
 */
/*
 * [한국어 설명] 오류를 낸 TLP 의 헤더를 읽고 출력하는 보조 모듈 (tlp.c)
 *
 * === 파일의 역할 ===
 * PCIe 에서 오류가 나면 하드웨어가 "그때 문제가 된 패킷의 헤더" 를 로그
 * 레지스터에 남긴다. TLP(Transaction Layer Packet) 헤더는 4바이트씩 4칸,
 * 즉 16바이트이며, 그 안에 트랜잭션의 종류(읽기/쓰기/완료), 목표 주소,
 * 요청자 ID, 태그가 들어 있다.
 *
 * 이 파일은 그 로그를 읽어(pcie_read_tlp_log) 구조체에 담고, 사람이 읽을
 * 형태로 출력한다(pcie_print_tlp_log). 하는 일은 그것뿐이지만, 오류를
 * 진단할 때 가장 결정적인 정보가 여기서 나온다 — 어느 주소로 가던
 * 어떤 트랜잭션이 실패했는지가 곧 원인의 실마리이기 때문이다.
 *
 * 로그의 종류가 둘이다.
 *   Header Log         - 항상 있다. TLP 헤더 16바이트.
 *   TLP Prefix Log     - 선택. End-to-End TLP Prefix 를 쓰는 시스템에서
 *                        추가로 남는다. 최대 4개의 prefix.
 * 어느 것이 있고 몇 개인지는 AER capability 의 능력 비트가 알려 준다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 오류 발생 -> aer.c 의 aer_print_error() 또는 dpc.c 의 오류 처리
 *   -> [이 파일] pcie_read_tlp_log()  : 로그 레지스터를 읽어 구조체로
 *   -> [이 파일] pcie_print_tlp_log() : 그 구조체를 dmesg 로
 *
 * 실행 컨텍스트: 오류 처리 경로에서 불린다. aer.c 쪽은 스레드 문맥,
 * dpc.c 쪽도 스레드 핸들러다. config 읽기가 있으므로 잠들 수 있는 곳이어야 한다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pcie/aer.c, pcie/dpc.c — 두 오류 경로가 모두 이 파일을 쓴다.
 * 아래쪽: access.c 의 config 접근.
 * 공유 상태: struct pcie_tlp_log — 읽어 온 헤더 4워드와 prefix 들을 담는
 *   값 구조체다. 전역 상태는 없다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 *
 * 그러나 NVMe 문제를 추적할 때 이 파일의 출력이 매우 유용하다.
 * dmesg 에 나오는 다음과 같은 줄이 이 파일의 결과다:
 *
 *   nvme 0000:01:00.0: AER:   TLP Header: 40000001 0000000f fedc0000 00000000
 *
 * 첫 워드의 상위 비트가 트랜잭션 종류(0x40 = Memory Write)를 나타내고,
 * 세 번째 워드가 목표 주소다. 그 주소가 NVMe 의 BAR 범위인지, 호스트
 * 메모리인지, 아니면 전혀 엉뚱한 곳인지를 보면 원인이 좁혀진다.
 * 예컨대 매핑 해제된 DMA 버퍼의 주소가 찍혀 있으면 use-after-free 이고,
 * 0 번지 근처면 NULL 포인터가 DMA 주소로 넘어간 것이다.
 *
 * === 주요 함수/구조체 요약 ===
 * pcie_read_tlp_log()   : AER 또는 DPC capability 의 로그 레지스터에서
 *                         헤더와 prefix 를 읽어 struct pcie_tlp_log 에 담는다.
 *                         읽을 워드 수는 호출자가 능력 비트를 보고 정해 넘긴다.
 * pcie_print_tlp_log()  : 그 구조체를 형식에 맞춰 출력한다. prefix 가 있으면
 *                         함께 찍는다.
 * struct pcie_tlp_log   : 헤더 4워드(dw[4])와 prefix 배열, 그리고 prefix 가
 *                         유효한지 표시하는 필드로 이뤄진 값 구조체.
 */

#include <linux/aer.h>		/* PCI/NVMe: AER(Advanced Error Reporting) 캡ability 정의; NVMe 장치의 PCIe 오류 보고에 사용 */
#include <linux/array_size.h>	/* PCI/NVMe: ARRAY_SIZE() 매크로: TLP 로그 DWORD 배열 경계 확인 */
#include <linux/bitfield.h>	/* PCI/NVMe: FIELD_GET()로 비트필드 추출; AER Cap/Control 레지스터 파싱 */
#include <linux/pci.h>		/* PCI/NVMe: PCI/PCIe 핵심 구조체 및 함수; NVMe PCIe 호스트 드라이버와 동일 PCI 서브시스템 */
#include <linux/string.h>	/* PCI/NVMe: memset(), scnprintf() 등 문자열/메모리 함수 */

#include "../pci.h"		/* PCI/NVMe: PCI 코어 낮은 수준 함수 및 pcibios_err_to_errno() 선언 */

/**
 * aer_tlp_log_len - Calculate AER Capability TLP Header/Prefix Log length
 * @dev: PCIe device
 * @aercc: AER Capabilities and Control register value
 *
 * Return: TLP Header/Prefix Log length
 */
/* PCI/NVMe: AER TLP 로그 길이 계산; NVMe 장치에서 UR/CA 등 PCIe 오류 발생 시 헤더 로그 크기 결정 */
unsigned int aer_tlp_log_len(struct pci_dev *dev, u32 aercc)
{
	/* NVMe: Flit 모드 TLP 로그 지원 여부 확인; PCIe 6.x 이상 고속 링크에서 NVMe SSD 오류 디버깅 시 사용 */
	if (aercc & PCI_ERR_CAP_TLP_LOG_FLIT)
		return FIELD_GET(PCI_ERR_CAP_TLP_LOG_SIZE, aercc);

	/* NVMe: 기본 TLP 헤더 4 DWORD + End-to-End Prefix 최대 개수; NVMe 쓰기/읽기 TLP 오류 시 추가 정보 기록 */
	return PCIE_STD_NUM_TLP_HEADERLOG +
	       ((aercc & PCI_ERR_CAP_PREFIX_LOG_PRESENT) ?
		dev->eetlp_prefix_max : 0);
}

#ifdef CONFIG_PCIE_DPC
/**
 * dpc_tlp_log_len - Calculate DPC RP PIO TLP Header/Prefix Log length
 * @dev: PCIe device
 *
 * Return: TLP Header/Prefix Log length
 */
/* PCI/NVMe: DPC(Downstream Port Containment) TLP 로그 길이; NVMe SSD가 연결된 다운스트림 포트 오류 격리에 활용 */
unsigned int dpc_tlp_log_len(struct pci_dev *dev)
{
	/* Remove ImpSpec Log register from the count */
	/* NVMe: ImpSpec 로그 레지스터를 제외하고 실제 TLP 헤더/프리픽스 개수만 계산 */
	if (dev->dpc_rp_log_size >= PCIE_STD_NUM_TLP_HEADERLOG + 1)
		return dev->dpc_rp_log_size - 1;

	/* NVMe: DPC 로그 크기가 작으면 그대로 반환; 핫플러그 또는 AER 복구 시 NVMe SSD 상태 판별에 참고 */
	return dev->dpc_rp_log_size;
}
#endif

/**
 * pcie_read_tlp_log - read TLP Header Log
 * @dev: PCIe device
 * @where: PCI Config offset of TLP Header Log
 * @where2: PCI Config offset of TLP Prefix Log
 * @tlp_len: TLP Log length (Header Log + TLP Prefix Log in DWORDs)
 * @flit: TLP Logged in Flit mode
 * @log: TLP Log structure to fill
 *
 * Fill @log from TLP Header Log registers, e.g., AER or DPC.
 *
 * Return: 0 on success and filled TLP Log structure, <0 on error.
 */
/* PCI/NVMe: PCIe TLP 헤더/프리픽스 로그를 PCI config 공간에서 읽어 구조체에 저장; NVMe 장치의 PCIe 오류 TLP 분석 시 핵심 */
int pcie_read_tlp_log(struct pci_dev *dev, int where, int where2,
		      unsigned int tlp_len, bool flit, struct pcie_tlp_log *log)
{
	unsigned int i;		/* NVMe: 읽을 DWORD 인덱스 */
	int off, ret;		/* NVMe: config 오프셋(off)과 pci_read_config_dword() 반환값(ret) */

	/* NVMe: 버퍼 초과 방지; AER/DPC가 과도한 길이를 보고필 때 NVMe 드라이버도 안전하게 자름 */
	if (tlp_len > ARRAY_SIZE(log->dw))
		tlp_len = ARRAY_SIZE(log->dw);

	memset(log, 0, sizeof(*log));	/* NVMe: pcie_tlp_log 구조체 0으로 초기화; 출력/파싱 시 유효하지 않은 필드 방지 */

	/* NVMe: 각 DWORD를 순회하며 AER Header Log 또는 TLP Prefix Log 레지스터에서 읽음 */
	for (i = 0; i < tlp_len; i++) {
		/* NVMe: 처음 4 DWORD는 표준 TLP 헤더 로그; NVMe 명령/완료 TLP의 Fmt/Type, 요청자ID 등 포함 */
		if (i < PCIE_STD_NUM_TLP_HEADERLOG)
			off = where + i * 4;
		else
			off = where2 + (i - PCIE_STD_NUM_TLP_HEADERLOG) * 4;

		/* NVMe: PCI config dword 읽기; NVMe host/pci.c에서 BAR/캡ability 읽을 때와 동일 메커니즘 */
		ret = pci_read_config_dword(dev, off, &log->dw[i]);
		/* NVMe: config 읽기 실패 시 errno 변환; NVMe probe나 AER 복구 경로에서 오류 전파 */
		if (ret)
			return pcibios_err_to_errno(ret);
	}

	/*
	 * Hard-code non-Flit mode to 4 DWORDs, for now. The exact length
	 * can only be known if the TLP is parsed.
	 */
	/* NVMe: Flit 모드가 아니면 헤더 길이를 4 DWORD로 고정; NVMe DMA/메모리 매핑 관련 TLP는 파싱 후 실제 길이 확정 */
	log->header_len = flit ? tlp_len : 4;
	log->flit = flit;		/* NVMe: Flit 모드 여부 기록; PCIe 6.x 플릿 모드 NVMe 링크 오류 디버깅에 필요 */

	return 0;			/* NVMe: TLP 로그 읽기 성공; 호출자가 dmesg 또는 AER 핸들러에서 활용 */
}

#define EE_PREFIX_STR " E-E Prefixes:"
/* PCI/NVMe: End-to-End TLP Prefix 출력용 문자열; NVMe SSD가 E2E 프리픽스를 포함한 TLP 오류 시 표시 */

/**
 * pcie_print_tlp_log - Print TLP Header / Prefix Log contents
 * @dev: PCIe device
 * @log: TLP Log structure
 * @level: Printk log level
 * @pfx: String prefix
 *
 * Prints TLP Header and Prefix Log information held by @log.
 */
/* PCI/NVMe: 캡처된 TLP 헤더/프리픽스를 커널 로그에 출력; NVMe 장치 PCIe 오류 시 dmesg에 기록하여 디버깅 */
void pcie_print_tlp_log(const struct pci_dev *dev,
			const struct pcie_tlp_log *log, const char *level,
			const char *pfx)
{
	/* EE_PREFIX_STR fits the extended DW space needed for the Flit mode */
	/* NVMe: Flit 모드 최대 헤더 로그를 담을 수 있도록 버퍼 크기 산정; EE_PREFIX_STR도 고려 */
	char buf[11 * PCIE_STD_MAX_TLP_HEADERLOG + 1];
	unsigned int i;		/* NVMe: 출력 루프 인덱스 */
	int len;		/* NVMe: 버퍼에 기록된 문자열 길이 */

	/* NVMe: TLP 헤더 첫 4 DWORD를 16진수로 포맷; NVMe 요청/완료 TLP의 주소/길이/상태 필드 확인 가능 */
	len = scnprintf(buf, sizeof(buf), "%#010x %#010x %#010x %#010x",
			log->dw[0], log->dw[1], log->dw[2], log->dw[3]);

	/* NVMe: Flit 모드인지 여부에 따라 추가 DWORD 또는 E-E Prefix를 버퍼에 덧붙임 */
	if (log->flit) {
		/* NVMe: 표준 헤더 이후 추가 Flit DWORD들을 순회 출력; NVMe 고속 링크 오류 시 상세 TLP 기록 */
		for (i = PCIE_STD_NUM_TLP_HEADERLOG; i < log->header_len; i++) {
			len += scnprintf(buf + len, sizeof(buf) - len,
					 " %#010x", log->dw[i]);
		}
	} else {
		/* NVMe: E-E Prefix가 있으면 구분 문자열 추가; NVMe IOMMU/DMA 관련 TLP에서 확장 정보 표시 */
		if (log->prefix[0])
			len += scnprintf(buf + len, sizeof(buf) - len,
					 EE_PREFIX_STR);
		/* NVMe: 저장된 E-E Prefix DWORD를 모두 출력; NVMe MSI-X 또는 PASID 관련 TLP 오류 추적 */
		for (i = 0; i < ARRAY_SIZE(log->prefix); i++) {
			/* NVMe: 0인 prefix는 더 이상 유효한 prefix 없음을 의미; 루프 종료 */
			if (!log->prefix[i])
				break;
			len += scnprintf(buf + len, sizeof(buf) - len,
					 " %#010x", log->prefix[i]);
		}
	}

	/* NVMe: 최종 TLP 헤더 문자열을 지정 로그 레벨으로 출력; NVMe SSD AER/DPC 이벤트 분석에 사용 */
	dev_printk(level, &dev->dev, "%sTLP Header%s: %s\n", pfx,
		log->flit ? " (Flit)" : "", buf);
}
