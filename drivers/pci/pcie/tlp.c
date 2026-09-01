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

#include <linux/aer.h>
#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "../pci.h"

/**
 * aer_tlp_log_len - Calculate AER Capability TLP Header/Prefix Log length
 * @dev: PCIe device
 * @aercc: AER Capabilities and Control register value
 *
 * Return: TLP Header/Prefix Log length
 */
unsigned int aer_tlp_log_len(struct pci_dev *dev, u32 aercc)
{
	if (aercc & PCI_ERR_CAP_TLP_LOG_FLIT)
		/* [한국어] FLIT 모드에서는 로그 길이가 고정이 아니라 AER Capabilities and Control
		 * 레지스터의 TLP_LOG_SIZE 필드에 실려 온다. FIELD_GET 이 그 필드를
		 * 마스크·시프트해 순수한 DWORD 개수로 정규화해 준다. */
		return FIELD_GET(PCI_ERR_CAP_TLP_LOG_SIZE, aercc);

	/* [한국어] 비 FLIT(=기존) 모드에서는 헤더 로그가 항상 4 DWORD 로 고정이고, */
	return PCIE_STD_NUM_TLP_HEADERLOG +
	       /* [한국어] End-to-End 프리픽스 로그가 존재한다고 표시되어 있으면 그 최대 개수만큼을 더한다.
	        * eetlp_prefix_max 는 열거 시점에 장치 능력에서 읽어 pci_dev 에 저장해 둔 값이다.
	        * 즉 이 함수는 "레지스터를 몇 DWORD 읽어야 하는가"를 답하는 계산기다. */
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
unsigned int dpc_tlp_log_len(struct pci_dev *dev)
{
	/* Remove ImpSpec Log register from the count */
	if (dev->dpc_rp_log_size >= PCIE_STD_NUM_TLP_HEADERLOG + 1)
		/* [한국어] 위 영어 주석대로 마지막 한 칸(ImpSpec Log 레지스터)은 TLP 로그가 아니므로 뺀다.
		 * dpc_rp_log_size 는 DPC capability 에서 읽은 전체 로그 레지스터 개수라
		 * 그 안에 구현 정의 레지스터가 섞여 있다. */
		return dev->dpc_rp_log_size - 1;

	/* [한국어] 5 칸 미만이면 ImpSpec Log 자체가 없는 구현이므로 그대로 돌려준다.
	 * 즉 이 조건문은 "뺄 것이 있을 때만 뺀다"는 방어다. */
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
int pcie_read_tlp_log(struct pci_dev *dev, int where, int where2,
		      unsigned int tlp_len, bool flit, struct pcie_tlp_log *log)
{
	/* [한국어] 레지스터 순회 인덱스. */
	unsigned int i;
	/* [한국어] off: 이번에 읽을 config 오프셋. ret: config 읽기 결과. */
	int off, ret;

	/* [한국어] 요청 길이가 구조체 배열보다 크면, */
	if (tlp_len > ARRAY_SIZE(log->dw))
		/* [한국어] 배열 크기로 잘라 넘침을 막는다. 하드웨어가 보고한 길이를 그대로 믿지 않는
		 * 방어이며, 잘린다는 사실을 호출자에게 알리지는 않는다. */
		tlp_len = ARRAY_SIZE(log->dw);

	/* [한국어] 구조체 전체를 0 으로 지운다. 아래 루프가 tlp_len 칸만 채우므로, 나머지 칸이
	 * 이전 호출의 값을 물고 있으면 출력 루프가 쓰레기를 찍게 된다.
	 * 특히 prefix[] 를 0 으로 만들어야 아래 출력에서 "0 이면 끝" 판정이 성립한다. */
	memset(log, 0, sizeof(*log));

	/* [한국어] 필요한 DWORD 개수만큼 config 레지스터를 하나씩 읽는다. */
	for (i = 0; i < tlp_len; i++) {
		/* [한국어] 앞의 4 칸(PCIE_STD_NUM_TLP_HEADERLOG)은 헤더 로그다. */
		if (i < PCIE_STD_NUM_TLP_HEADERLOG)
			/* [한국어] 헤더 로그 시작 오프셋에서 4바이트씩 나아간다. */
			off = where + i * 4;
		else
			/* [한국어] 그 뒤는 프리픽스 로그이며 레지스터 블록이 아예 다른 곳(where2)에 있다.
			 * 그래서 인덱스에서 헤더 몫 4 를 빼 프리픽스 블록 안의 상대 위치를 구한다.
			 * FLIT 모드에서는 뒤쪽도 헤더의 연장이지만, 레지스터 배치가 같아 같은 계산이 통한다. */
			off = where2 + (i - PCIE_STD_NUM_TLP_HEADERLOG) * 4;

		/* [한국어] config space 에서 32비트를 읽어 배열에 담는다. */
		ret = pci_read_config_dword(dev, off, &log->dw[i]);
		/* [한국어] 읽기 실패 검사. */
		if (ret)
			/* [한국어] PCIBIOS_* 코드를 커널 errno 로 변환해 돌려준다. 두 오류 체계가 다르므로
			 * 이 변환이 없으면 호출자가 값을 잘못 해석한다. */
			return pcibios_err_to_errno(ret);
	}

	/*
	 * Hard-code non-Flit mode to 4 DWORDs, for now. The exact length
	 * can only be known if the TLP is parsed.
	 */
	log->header_len = flit ? tlp_len : 4;
	/* [한국어] FLIT 모드였는지 기록해 둔다. 출력 함수가 이 값으로 레이아웃을 고른다. */
	log->flit = flit;

	return 0;
}

#define EE_PREFIX_STR " E-E Prefixes:"

/**
 * pcie_print_tlp_log - Print TLP Header / Prefix Log contents
 * @dev: PCIe device
 * @log: TLP Log structure
 * @level: Printk log level
 * @pfx: String prefix
 *
 * Prints TLP Header and Prefix Log information held by @log.
 */
void pcie_print_tlp_log(const struct pci_dev *dev,
			const struct pcie_tlp_log *log, const char *level,
			const char *pfx)
{
	/* EE_PREFIX_STR fits the extended DW space needed for the Flit mode */
	char buf[11 * PCIE_STD_MAX_TLP_HEADERLOG + 1];
	/* [한국어] 출력 루프의 인덱스. */
	unsigned int i;
	/* [한국어] 지금까지 buf 에 쓴 길이. scnprintf 반환값을 누적한다. */
	int len;

	/* [한국어] 헤더 로그 앞 4 DWORD 를 먼저 찍는다. %#010x 는 "0x" 접두사를 포함해
	 * 정확히 10 글자를 만드는 서식이라, 출력이 열 맞춰 정렬된다.
	 * scnprintf 는 snprintf 와 달리 "실제로 쓴 길이"를 돌려주므로(잘렸을 때
	 * 쓰려던 길이가 아니라) 누적 계산이 버퍼 밖으로 튀지 않는다. */
	len = scnprintf(buf, sizeof(buf), "%#010x %#010x %#010x %#010x",
			log->dw[0], log->dw[1], log->dw[2], log->dw[3]);

	/* [한국어] FLIT 모드면 뒤쪽 DWORD 도 헤더의 연장이다. */
	if (log->flit) {
		/* [한국어] 헤더 길이만큼 이어서 찍는다. 프리픽스 개념이 없다. */
		for (i = PCIE_STD_NUM_TLP_HEADERLOG; i < log->header_len; i++) {
			/* [한국어] 남은 공간(sizeof(buf) - len)만 넘겨 넘침을 막는다. */
			len += scnprintf(buf + len, sizeof(buf) - len,
					 " %#010x", log->dw[i]);
		}
	} else {
		/* [한국어] 비 FLIT 모드에서는 뒤쪽이 End-to-End 프리픽스다.
		 * prefix[] 는 dw[] 와 공용체로 겹쳐 있어 dw[4] 이후를 다른 이름으로 보는 것이다
		 * (struct pcie_tlp_log 의 정의는 <linux/aer.h> 에 있고 이 스파스 체크아웃에는
		 * 없다 — 배포판 헤더에서 union { dw[]; struct { _do_not_use[4]; prefix[]; } } 임을 확인했다).
		 * 첫 프리픽스가 0 이면 프리픽스가 아예 없다는 뜻이라 제목도 찍지 않는다. */
		if (log->prefix[0])
			/* [한국어] 프리픽스 구간이 시작됨을 알리는 고정 문자열을 붙인다. */
			len += scnprintf(buf + len, sizeof(buf) - len,
					 EE_PREFIX_STR);
		/* [한국어] 프리픽스를 최대 개수만큼 순회한다. */
		for (i = 0; i < ARRAY_SIZE(log->prefix); i++) {
			/* [한국어] 0 을 만나면 거기가 끝이다 — 위 memset 이 뒷칸을 0 으로 만들어 두었기에
			 * 성립하는 판정이다. */
			if (!log->prefix[i])
				break;
			/* [한국어] 프리픽스 값을 이어 찍는다. */
			len += scnprintf(buf + len, sizeof(buf) - len,
					 " %#010x", log->prefix[i]);
		}
	}

	/* [한국어] 완성된 한 줄을 호출자가 지정한 로그 레벨로 출력한다. FLIT 모드였으면
	 * 제목에 " (Flit)" 를 덧붙여 뒤쪽 값들이 프리픽스가 아니라 헤더 연장임을 알린다. */
	dev_printk(level, &dev->dev, "%sTLP Header%s: %s\n", pfx,
		log->flit ? " (Flit)" : "", buf);
}
