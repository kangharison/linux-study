// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */

/*
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/tph.c)은 PCIe TLP Processing Hints(TPH)를 관리한다.
 * TPH는 엔드포인트(예: NVMe SSD)가 Root Complex(RC)에 메모리 트랜잭션
 * 처리 방식에 대한 힌트(Processing Hint, PH)와 Steering Tag(ST)를 전달해
 * 캐시/메모리 근접성을 최적화하는 PCIe 기능이다.
 *
 * NVMe SSD 관련 의미:
 *   - NVMe 드라이브는 여러 큐와 MSI-X 인터럽트를 사용하며, 큐-CPU affinity
 *     설정 시 TPH를 이용해 데이터/완료큐가 특정 CPU/NUMA 노드의 캐시로
 *     스티어링되도록 요청할 수 있다.
 *   - TPH를 통해 DMA read/write 트랜잭션의 지연(latency)과 대역폭을 개선할
 *     수 있어 고성능 NVMe 워크로드에 직접적인 영향을 준다.
 *   - NVMe 장치가 D3cold, PCIe surprise down, FLR, suspend-resume 등으로
 *     상태를 잃은 뒤 복귀할 때 TPH 설정값(Control register, ST table)을
 *     복원해야 큐-CPU affinity 성능이 유지된다.
 *
 * NVMe 드라이버가 간접/직접 사용하는 호출 경로:
 *   - 장치 탐색 단계: pci_tph_init() -> pci_find_ext_capability()
 *     (NVMe pdev->tph_cap 등록, drivers/pci/probe.c에서 자동 호출)
 *   - NVMe 큐 초기화: pcie_enable_tph() -> pcie_tph_set_st_entry() 로
 *     큐/인터럽트 벡터별 ST 구성(현재 NVMe 본문은 직접 호출하지 않으나,
 *     NVMe 성능 최적화를 위해 활용 가능한 표준 인터페이스)
 *   - 장치 재초기화/오류 복구: pci_restore_tph_state() -> pcie_enable_tph()
 *     (PCIe AER, PME, D3cold wakeup, runtime resume 시 복원)
 *   - 전역 비활성화: pci_no_tph() ("notph" 커널 파라미터)
 *
 * TPH 상태는 struct pci_dev의 tph_cap, tph_enabled, tph_mode,
 * tph_req_type 필드로 관리되며, NVMe pdev에 포함된다.
 * ===================================================================
 */

#include <linux/pci.h>           /* NVMe: PCIe 장치 구조체와 config space 접근 API */
#include <linux/pci-acpi.h>      /* NVMe: ACPI _DSM 기반 ST 획득에 필요 */
#include <linux/msi.h>           /* NVMe: MSI-X table에 ST entry를 기록할 때 사용 */
#include <linux/bitfield.h>      /* NVMe: PCIe TPH capability 필드 추출/조합 */
#include <linux/pci-tph.h>       /* NVMe: TPH 관련 enum/define 노출 */

#include "pci.h"                 /* NVMe: 낮은 레벨 PCI/PCIe helper 함수들 */

/* System-wide TPH disabled */
/* NVMe: 커널 파라미터 "notph"로 전역 TPH 사용을 끌 때 설정되는 플래그 */
static bool pci_tph_disabled;

#ifdef CONFIG_ACPI
/*
 * The st_info struct defines the Steering Tag (ST) info returned by the
 * firmware PCI ACPI _DSM method (rev=0x7, func=0xF, "_DSM to Query Cache
 * Locality TPH Features"), as specified in the approved ECN for PCI Firmware
 * Spec and available at https://members.pcisig.com/wg/PCI-SIG/document/15470.
 *
 * @vm_st_valid:  8-bit ST for volatile memory is valid
 * @vm_xst_valid: 16-bit extended ST for volatile memory is valid
 * @vm_ph_ignore: 1 => PH was and will be ignored, 0 => PH should be supplied
 * @vm_st:        8-bit ST for volatile mem
 * @vm_xst:       16-bit extended ST for volatile mem
 * @pm_st_valid:  8-bit ST for persistent memory is valid
 * @pm_xst_valid: 16-bit extended ST for persistent memory is valid
 * @pm_ph_ignore: 1 => PH was and will be ignored, 0 => PH should be supplied
 * @pm_st:        8-bit ST for persistent mem
 * @pm_xst:       16-bit extended ST for persistent mem
 */
/* NVMe: ACPI _DSM으로부터 받은 volatile/persistent 메모리별 ST 정보를
 * 담는 64bit 공용체. NVMe SSD가 DMA할 메모리 대상(volatile DRAM 등)에
 * 대한 캐시 근접성 힌트를 운영체제가 아닌 firmware/ACPI에서 제공할 때
 * 사용된다.
 */
union st_info {
	struct {
		u64 vm_st_valid : 1;     /* NVMe: volatile 메모리용 8bit ST 유효 비트 */
		u64 vm_xst_valid : 1;    /* NVMe: volatile 메모리용 16bit 확장 ST 유효 비트 */
		u64 vm_ph_ignore : 1;    /* NVMe: volatile 메모리에서 PH 무시 여부(0이면 PH 제공 필요) */
		u64 rsvd1 : 5;           /* NVMe: ACPI 규격 예약 필드 */
		u64 vm_st : 8;           /* NVMe: volatile 메모리용 8bit Steering Tag */
		u64 vm_xst : 16;         /* NVMe: volatile 메모리용 16bit 확장 ST */
		u64 pm_st_valid : 1;     /* NVMe: persistent 메모리용 8bit ST 유효 비트 */
		u64 pm_xst_valid : 1;    /* NVMe: persistent 메모리용 16bit 확장 ST 유효 비트 */
		u64 pm_ph_ignore : 1;    /* NVMe: persistent 메모리에서 PH 무시 여부 */
		u64 rsvd2 : 5;           /* NVMe: ACPI 규격 예약 필드 */
		u64 pm_st : 8;           /* NVMe: persistent 메모리용 8bit Steering Tag */
		u64 pm_xst : 16;         /* NVMe: persistent 메모리용 16bit 확장 ST */
	};
	u64 value;                     /* NVMe: _DSM 반환 64bit 원시값 전체 접근용 */
};

/*
 * tph_extract_tag:
 *   ACPI _DSM에서 받은 st_info에서 메모리 타입과 TPH 요청 타입에 맞는
 *   Steering Tag를 추출한다. NVMe 큐-CPU affinity 설정 시 특정 CPU에
 *   대응하는 ST 값을 얻는 데 사용된다.
 */
static u16 tph_extract_tag(enum tph_mem_type mem_type, u8 req_type,
			   union st_info *info)
{
	switch (req_type) {            /* NVMe: 장치가 지원하는 TPH 요청 형식(8bit/16bit)에 따라 분기 */
	case PCI_TPH_REQ_TPH_ONLY: /* 8-bit tag */
		switch (mem_type) {    /* NVMe: 대상 메모리 타입(volatile/persistent)에 따라 분기 */
		case TPH_MEM_TYPE_VM:
			if (info->vm_st_valid)      /* NVMe: volatile ST가 유효한지 확인 */
				return info->vm_st; /* NVMe: NVMe DMA 대상 volatile 메모리의 8bit ST 반환 */
			break;                      /* NVMe: ST가 유효하지 않으면 다음 case 탐색 종료 */
		case TPH_MEM_TYPE_PM:
			if (info->pm_st_valid)      /* NVMe: persistent ST가 유효한지 확인 */
				return info->pm_st; /* NVMe: persistent 메모리용 8bit ST 반환 */
			break;                      /* NVMe: ST가 유효하지 않으면 분기 종료 */
		}
		break;                          /* NVMe: 8bit TPH 요청 처리 완료, 상위 switch 종료 */
	case PCI_TPH_REQ_EXT_TPH: /* 16-bit tag */
		switch (mem_type) {    /* NVMe: 확장 TPH(16bit)용 ST 선택 */
		case TPH_MEM_TYPE_VM:
			if (info->vm_xst_valid)     /* NVMe: volatile 확장 ST 유효성 검사 */
				return info->vm_xst;/* NVMe: volatile 메모리용 16bit ST 반환 */
			break;                      /* NVMe: 유효하지 않으면 분기 종료 */
		case TPH_MEM_TYPE_PM:
			if (info->pm_xst_valid)     /* NVMe: persistent 확장 ST 유효성 검사 */
				return info->pm_xst;/* NVMe: persistent 메모리용 16bit ST 반환 */
			break;                      /* NVMe: 유효하지 않으면 분기 종료 */
		}
		break;                          /* NVMe: 16bit TPH 요청 처리 완료 */
	default:
		return 0;                       /* NVMe: 알 수 없는 req_type이면 ST 0 반환(힌트 없음) */
	}

	return 0;                              /* NVMe: 유효한 ST를 찾지 못하면 0 반환 */
}

#define TPH_ST_DSM_FUNC_INDEX	0xF        /* NVMe: ACPI _DSM 함수 인덱스 0xF(Cache Locality TPH Features) */
/*
 * tph_invoke_dsm:
 *   Root Port의 ACPI _DSM을 호출해 특정 CPU(UID)에 연결된 ST 정보를
 *   가져온다. NVMe 드라이버가 큐를 특정 CPU에 바인딩할 때, 해당 CPU로
 *   향하는 DMA 트랜잭션의 캐시 근접성 힌트를 firmware에서 얻는 통로다.
 */
static acpi_status tph_invoke_dsm(acpi_handle handle, u32 cpu_uid,
				  union st_info *st_out)
{
	union acpi_object arg3[3], in_obj, *out_obj; /* NVMe: _DSM 인자/반환 객체 */

	if (!acpi_check_dsm(handle, &pci_acpi_dsm_guid, 7,     /* NVMe: rev 7 _DSM에서 함수 0xF를 지원하는지 확인 */
			    BIT(TPH_ST_DSM_FUNC_INDEX)))      /* NVMe: TPH ST 조회 비트 검사 */
		return AE_ERROR;                        /* NVMe: 지원하지 않으면 ACPI 오류 반환 */

	/* DWORD: feature ID (0 for processor cache ST query) */
	arg3[0].integer.type = ACPI_TYPE_INTEGER;       /* NVMe: _DSM 인자0 타입을 정수로 설정 */
	arg3[0].integer.value = 0;                      /* NVMe: feature ID 0은 프로세서 캐시 ST 조회 */

	/* DWORD: target UID */
	arg3[1].integer.type = ACPI_TYPE_INTEGER;       /* NVMe: _DSM 인자1 타입을 정수로 설정 */
	arg3[1].integer.value = cpu_uid;                /* NVMe: ST를 조회할 대상 CPU의 ACPI UID */

	/* QWORD: properties, all 0's */
	arg3[2].integer.type = ACPI_TYPE_INTEGER;       /* NVMe: _DSM 인자2 타입을 정수로 설정 */
	arg3[2].integer.value = 0;                      /* NVMe: 현재 규격에서 추가 속성은 0으로 예약 */

	in_obj.type = ACPI_TYPE_PACKAGE;                /* NVMe: _DSM 입력 객체를 패키지로 구성 */
	in_obj.package.count = ARRAY_SIZE(arg3);        /* NVMe: 패키지 내 인자 개수(3개) */
	in_obj.package.elements = arg3;                 /* NVMe: 패키지 원소 배열 연결 */

	out_obj = acpi_evaluate_dsm(handle, &pci_acpi_dsm_guid, 7,  /* NVMe: ACPI _DSM 실제 평가 */
				    TPH_ST_DSM_FUNC_INDEX, &in_obj);
	if (!out_obj)                                   /* NVMe: _DSM 평가 결과가 없으면 */
		return AE_ERROR;                        /* NVMe: ACPI 오류 반환 */

	if (out_obj->type != ACPI_TYPE_BUFFER) {        /* NVMe: 반환값이 8바이트 버퍼가 아니면 */
		ACPI_FREE(out_obj);                     /* NVMe: 반환 객체 메모리 해제 */
		return AE_ERROR;                        /* NVMe: 형식 오류 반환 */
	}

	st_out->value = *((u64 *)(out_obj->buffer.pointer)); /* NVMe: 버퍼에서 64bit ST 정보를 공용체로 복사 */

	ACPI_FREE(out_obj);                             /* NVMe: ACPI 반환 객체 메모리 해제 */

	return AE_OK;                                   /* NVMe: ST 획득 성공 */
}
#endif

/* Update the TPH Requester Enable field of TPH Control Register */
/*
 * set_ctrl_reg_req_en:
 *   TPH Control Register의 Requester Enable 필드를 갱신한다. NVMe 장치의
 *   TPH 요청 기능을 켜거나 끄거나, 8bit/16bit 요청 형식을 선택할 때 사용.
 */
static void set_ctrl_reg_req_en(struct pci_dev *pdev, u8 req_type)
{
	u32 reg;                                        /* NVMe: TPH Control 레지스터 값 */

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg); /* NVMe: NVMe pdev의 TPH Control 레지스터 읽기 */

	reg &= ~PCI_TPH_CTRL_REQ_EN_MASK;               /* NVMe: 기존 Request Enable 필드 클리어 */
	reg |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, req_type); /* NVMe: 새로운 요청 형식(비활성/8bit/16bit) 기록 */

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg); /* NVMe: 갱신된 값을 NVMe pdev config space에 기록 */
}

/*
 * get_st_modes:
 *   NVMe 장치가 지원하는 ST mode(Interrupt Vector, Device Specific,
 *   No ST) 비트를 TPH Capability 레지스터에서 읽어온다. pcie_enable_tph()
 *   에서 요청 mode가 지원되는지 검증할 때 사용.
 */
static u8 get_st_modes(struct pci_dev *pdev)
{
	u32 reg;                                        /* NVMe: TPH Capability 레지스터 값 */

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg); /* NVMe: NVMe pdev의 TPH Capability 읽기 */
	reg &= PCI_TPH_CAP_ST_NS | PCI_TPH_CAP_ST_IV | PCI_TPH_CAP_ST_DS; /* NVMe: ST mode 지원 비트만 마스크 */

	return reg;                                     /* NVMe: 지원되는 ST mode 비트 반환 */
}

/**
 * pcie_tph_get_st_table_loc - Return the device's ST table location
 * @pdev: PCI device to query
 *
 * Return:
 *  PCI_TPH_LOC_NONE - Not present
 *  PCI_TPH_LOC_CAP  - Located in the TPH Requester Extended Capability
 *  PCI_TPH_LOC_MSIX - Located in the MSI-X Table
 */
/*
 * pcie_tph_get_st_table_loc:
 *   NVMe 장치의 Steering Tag table이 위치한 곳(TPH capability 공간 또는
 *   MSI-X table)을 반환한다. NVMe가 MSI-X 벡터별로 ST를 설정할지, 아니면
 *   장치 고유 table을 사용할지 결정하는 데 필요.
 */
u32 pcie_tph_get_st_table_loc(struct pci_dev *pdev)
{
	u32 reg;                                        /* NVMe: TPH Capability 레지스터 값 */

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg); /* NVMe: NVMe pdev의 TPH Capability 읽기 */

	return FIELD_GET(PCI_TPH_CAP_LOC_MASK, reg);    /* NVMe: LOC(Location) 필드 추출하여 반환 */
}
EXPORT_SYMBOL(pcie_tph_get_st_table_loc);

/*
 * Return the size of ST table. If ST table is not in TPH Requester Extended
 * Capability space, return 0. Otherwise return the ST Table Size + 1.
 */
/*
 * pcie_tph_get_st_table_size:
 *   TPH capability 공간 내 ST table의 항목 개수를 반환한다. NVMe 큐/벡터
 *   개수에 맞춰 ST entry를 설정할 때 경계를 확인하는 데 사용.
 */
u16 pcie_tph_get_st_table_size(struct pci_dev *pdev)
{
	u32 reg;                                        /* NVMe: TPH Capability 레지스터 값 */
	u32 loc;                                        /* NVMe: ST table 위치 */

	/* Check ST table location first */
	loc = pcie_tph_get_st_table_loc(pdev);          /* NVMe: ST table 위치 조회 */

	/* Convert loc to match with PCI_TPH_LOC_* defined in pci_regs.h */
	loc = FIELD_PREP(PCI_TPH_CAP_LOC_MASK, loc);    /* NVMe: LOC 필드 값을 pci_regs.h의 PCI_TPH_LOC_* 매크로와 동일한 형식으로 변환 */
	if (loc != PCI_TPH_LOC_CAP)                     /* NVMe: ST table이 TPH capability 공간에 없으면 */
		return 0;                               /* NVMe: 크기 0 반환(MSI-X table 사용 등) */

	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg); /* NVMe: TPH Capability 다시 읽기 */

	return FIELD_GET(PCI_TPH_CAP_ST_MASK, reg) + 1; /* NVMe: ST table size 필드 + 1이 실제 항목 수 */
}
EXPORT_SYMBOL(pcie_tph_get_st_table_size);

/* Return device's Root Port completer capability */
/*
 * get_rp_completer_type:
 *   NVMe 장치 뒤의 Root Port가 TPH completer로서 어떤 요청 형식(8bit/
 *   16bit)을 지원하는지 조회한다. device와 Root Port capability의 교집합을
 *   통해 실제 사용 가능한 req_type을 결정.
 */
static u8 get_rp_completer_type(struct pci_dev *pdev)
{
	struct pci_dev *rp;                             /* NVMe: NVMe 장치가 연결된 Root Port */
	u32 reg;                                        /* NVMe: Root Port DEVCAP2 레지스터 값 */
	int ret;                                        /* NVMe: 레지스터 읽기 결과 */

	rp = pcie_find_root_port(pdev);                 /* NVMe: NVMe pdev의 상위 Root Port 찾기 */
	if (!rp)                                        /* NVMe: Root Port를 찾지 못하면 */
		return 0;                               /* NVMe: TPH completer capability 없음 */

	ret = pcie_capability_read_dword(rp, PCI_EXP_DEVCAP2, &reg); /* NVMe: Root Port의 Device Capability 2 읽기 */
	if (ret)                                        /* NVMe: 읽기에 실패하면 */
		return 0;                               /* NVMe: capability 정보를 얻을 수 없음 */

	return FIELD_GET(PCI_EXP_DEVCAP2_TPH_COMP_MASK, reg); /* NVMe: Root Port TPH Completer 필드 반환 */
}

/* Write tag to ST table - Return 0 if OK, otherwise -errno */
/*
 * write_tag_to_st_table:
 *   TPH Extended Capability 공간 내 ST table의 특정 인덱스에 Steering Tag를
 *   기록한다. NVMe가 특정 큐/인터럽트에 해당하는 ST 값을 하드웨어에
 *   반영할 때 사용.
 */
static int write_tag_to_st_table(struct pci_dev *pdev, int index, u16 tag)
{
	int st_table_size;                              /* NVMe: ST table 총 항목 수 */
	int offset;                                     /* NVMe: config space 내 기록할 오프셋 */

	/* Check if index is out of bound */
	st_table_size = pcie_tph_get_st_table_size(pdev); /* NVMe: ST table 크기 확인 */
	if (index >= st_table_size)                     /* NVMe: 요청 인덱스가 table 범위를 벗어나면 */
		return -ENXIO;                          /* NVMe: 잘못된 인덱스 오류 반환 */

	offset = pdev->tph_cap + PCI_TPH_BASE_SIZEOF + index * sizeof(u16); /* NVMe: 기본 capability 크기 + 인덱스 * 2byte로 config space 오프셋 계산 */

	return pci_write_config_word(pdev, offset, tag); /* NVMe: 계산된 오프셋에 16bit ST 기록 */
}

/**
 * pcie_tph_get_cpu_st() - Retrieve Steering Tag for a target memory associated
 * with a specific CPU
 * @pdev: PCI device
 * @mem_type: target memory type (volatile or persistent RAM)
 * @cpu: associated CPU id
 * @tag: Steering Tag to be returned
 *
 * Return the Steering Tag for a target memory that is associated with a
 * specific CPU as indicated by cpu.
 *
 * Return: 0 if success, otherwise negative value (-errno)
 */
/*
 * pcie_tph_get_cpu_st:
 *   지정한 CPU와 메모리 타입에 대해 ACPI _DSM으로부터 Steering Tag를
 *   획득한다. NVMe 드라이버가 큐를 특정 CPU에 affinity할 때, 해당 CPU로
 *   향하는 DMA 트랜잭션에 사용할 ST 값을 얻는 NVMe-직접 관련 인터페이스.
 */
int pcie_tph_get_cpu_st(struct pci_dev *pdev, enum tph_mem_type mem_type,
			unsigned int cpu, u16 *tag)
{
#ifdef CONFIG_ACPI
	struct pci_dev *rp;                             /* NVMe: NVMe 장치의 Root Port */
	acpi_handle rp_acpi_handle;                     /* NVMe: Root Port bridge의 ACPI 핸들 */
	union st_info info;                             /* NVMe: _DSM 반환 ST 정보 */
	u32 cpu_uid;                                    /* NVMe: ACPI에서 사용하는 CPU UID */
	int ret;                                        /* NVMe: 함수 반환값 */

	ret = acpi_get_cpu_uid(cpu, &cpu_uid);          /* NVMe: linux CPU 번호를 ACPI UID로 변환 */
	if (ret != 0)                                   /* NVMe: UID 변환 실패 시 */
		return ret;                             /* NVMe: 오류 코드 반환 */

	rp = pcie_find_root_port(pdev);                 /* NVMe: NVMe pdev의 Root Port 획득(_DSM은 RP에 연결) */
	if (!rp || !rp->bus || !rp->bus->bridge)        /* NVMe: RP, RP bus, bridge 중 하나라도 없으면 */
		return -ENODEV;                         /* NVMe: ACPI 핸들을 얻을 수 없음 */

	rp_acpi_handle = ACPI_HANDLE(rp->bus->bridge);  /* NVMe: Root Port bridge에서 ACPI 핸들 추출 */

	if (tph_invoke_dsm(rp_acpi_handle, cpu_uid, &info) != AE_OK) { /* NVMe: ACPI _DSM으로 ST 정보 획득 시도 */
		*tag = 0;                               /* NVMe: 획득 실패 시 tag를 0으로 설정 */
		return -EINVAL;                         /* NVMe: ST 획득 실패 오류 반환 */
	}

	*tag = tph_extract_tag(mem_type, pdev->tph_req_type, &info); /* NVMe: 메모리 타입과 요청 형식에 맞는 ST 추출 */

	pci_dbg(pdev, "get steering tag: mem_type=%s, cpu=%d, tag=%#04x\n",
		(mem_type == TPH_MEM_TYPE_VM) ? "volatile" : "persistent",
		cpu, *tag);                             /* NVMe: dmesg에 ST 획득 로그 기록(NVMe 디버깅 시 유용) */

	return 0;                                       /* NVMe: ST 획득 성공 */
#else
	return -ENODEV;                                 /* NVMe: ACPI 미지원 시 ST 획득 불가 */
#endif
}
EXPORT_SYMBOL(pcie_tph_get_cpu_st);

/**
 * pcie_tph_set_st_entry() - Set Steering Tag in the ST table entry
 * @pdev: PCI device
 * @index: ST table entry index
 * @tag: Steering Tag to be written
 *
 * Figure out the proper location of ST table, either in the MSI-X table or
 * in the TPH Extended Capability space, and write the Steering Tag into
 * the ST entry pointed by index.
 *
 * Return: 0 if success, otherwise negative value (-errno)
 */
/*
 * pcie_tph_set_st_entry:
 *   NVMe 장치의 ST table(MSI-X table 또는 TPH capability 공간) 내 특정
 *   인덱스에 Steering Tag를 기록한다. NVMe 큐/MSI-X 벡터와 CPU/NUMA 노드
 *   간 스티어링 정책을 하드웨어에 반영하는 핵심 함수.
 */
int pcie_tph_set_st_entry(struct pci_dev *pdev, unsigned int index, u16 tag)
{
	u32 loc;                                        /* NVMe: ST table 위치 */
	int err = 0;                                    /* NVMe: 오류 코드 초기화 */

	if (!pdev->tph_cap)                             /* NVMe: NVMe 장치가 TPH capability를 갖지 않으면 */
		return -EINVAL;                         /* NVMe: TPH 설정 불가 */

	if (!pdev->tph_enabled)                         /* NVMe: TPH가 아직 활성화되지 않은 상태면 */
		return -EINVAL;                         /* NVMe: 먼저 pcie_enable_tph()를 호출해야 함 */

	/* No need to write tag if device is in "No ST Mode" */
	if (pdev->tph_mode == PCI_TPH_ST_NS_MODE)       /* NVMe: No ST Mode에서는 ST table을 사용하지 않음 */
		return 0;                               /* NVMe: 기록할 필요 없이 성공 처리 */

	/*
	 * Disable TPH before updating ST to avoid potential instability as
	 * cautioned in PCIe r6.2, sec 6.17.3, "ST Modes of Operation"
	 */
	set_ctrl_reg_req_en(pdev, PCI_TPH_REQ_DISABLE); /* NVMe: ST 갱신 중 TLP 힌트 불일치를 막기 위해 TPH 요청 일시 비활성화 */

	loc = pcie_tph_get_st_table_loc(pdev);          /* NVMe: ST table 위치 다시 확인 */
	/* Convert loc to match with PCI_TPH_LOC_* */
	loc = FIELD_PREP(PCI_TPH_CAP_LOC_MASK, loc);    /* NVMe: LOC 필드 값을 PCI_TPH_LOC_* 상수와 일치시킴 */

	switch (loc) {                                  /* NVMe: ST table 위치에 따라 기록 경로 선택 */
	case PCI_TPH_LOC_MSIX:
		err = pci_msix_write_tph_tag(pdev, index, tag); /* NVMe: MSI-X table의 해당 entry에 ST 기록(NVMe MSI-X 벡터별 ST) */
		break;                                      /* NVMe: MSI-X 경로 처리 완료 */
	case PCI_TPH_LOC_CAP:
		err = write_tag_to_st_table(pdev, index, tag); /* NVMe: TPH Extended Capability 공간의 ST table에 기록 */
		break;                                      /* NVMe: capability 경로 처리 완료 */
	default:
		err = -EINVAL;                              /* NVMe: 알 수 없는 위치이면 오류 */
	}

	if (err) {                                      /* NVMe: ST 기록에 실패하면 */
		pcie_disable_tph(pdev);                 /* NVMe: 안전을 위해 TPH를 완전히 비활성화 */
		return err;                             /* NVMe: 오류 코드 반환 */
	}

	set_ctrl_reg_req_en(pdev, pdev->tph_req_type);  /* NVMe: ST 갱신 완료 후 TPH 요청을 다시 활성화(8bit/16bit) */

	pci_dbg(pdev, "set steering tag: %s table, index=%d, tag=%#04x\n",
		(loc == PCI_TPH_LOC_MSIX) ? "MSI-X" : "ST", index, tag); /* NVMe: NVMe dmesg에 ST 설정 로그 기록 */

	return 0;                                       /* NVMe: ST entry 설정 성공 */
}
EXPORT_SYMBOL(pcie_tph_set_st_entry);

/**
 * pcie_disable_tph - Turn off TPH support for device
 * @pdev: PCI device
 *
 * Return: none
 */
/*
 * pcie_disable_tph:
 *   NVMe 장치의 TPH를 완전히 끈다. NVMe 드라이버가 큐 초기화 실패, quirk,
 *   오류 복구, 또는 드라이버 unload 시 TPH 관련 리소스를 정리할 때 호출.
 */
void pcie_disable_tph(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)                             /* NVMe: TPH capability가 없는 NVMe 장치면 */
		return;                                 /* NVMe: 아무 작업도 하지 않음 */

	if (!pdev->tph_enabled)                         /* NVMe: 이미 비활성화된 상태면 */
		return;                                 /* NVMe: 중복 처리 방지 */

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, 0); /* NVMe: TPH Control Register를 0으로 클리어(TPH 요청 끔) */

	pdev->tph_mode = 0;                             /* NVMe: 저장된 ST mode 초기화 */
	pdev->tph_req_type = 0;                         /* NVMe: 저장된 요청 형식 초기화 */
	pdev->tph_enabled = 0;                          /* NVMe: TPH 활성화 플래그 해제 */
}
EXPORT_SYMBOL(pcie_disable_tph);

/**
 * pcie_enable_tph - Enable TPH support for device using a specific ST mode
 * @pdev: PCI device
 * @mode: ST mode to enable. Current supported modes include:
 *
 *   - PCI_TPH_ST_NS_MODE: NO ST Mode
 *   - PCI_TPH_ST_IV_MODE: Interrupt Vector Mode
 *   - PCI_TPH_ST_DS_MODE: Device Specific Mode
 *
 * Check whether the mode is actually supported by the device before enabling
 * and return an error if not. Additionally determine what types of requests,
 * TPH or extended TPH, can be issued by the device based on its TPH requester
 * capability and the Root Port's completer capability.
 *
 * Return: 0 on success, otherwise negative value (-errno)
 */
/*
 * pcie_enable_tph:
 *   NVMe 장치의 TPH를 지정한 ST mode로 활성화한다. NVMe 큐-CPU affinity
 *   설정 전에 호출되어, NVMe endpoint와 Root Port가 모두 지원하는 요청
 *   형식(8bit/16bit TPH)과 mode를 결정.
 */
int pcie_enable_tph(struct pci_dev *pdev, int mode)
{
	u32 reg;                                        /* NVMe: TPH Control/Capability 레지스터 값 */
	u8 dev_modes;                                   /* NVMe: NVMe 장치가 지원하는 ST mode */
	u8 rp_req_type;                                 /* NVMe: Root Port가 지원하는 TPH completer 형식 */

	/* Honor "notph" kernel parameter */
	if (pci_tph_disabled)                           /* NVMe: "notph" 커널 파라미터로 전역 비활성화된 경우 */
		return -EINVAL;                         /* NVMe: TPH 활성화 거부 */

	if (!pdev->tph_cap)                             /* NVMe: NVMe pdev에 TPH capability가 없으면 */
		return -EINVAL;                         /* NVMe: TPH 활성화 불가 */

	if (pdev->tph_enabled)                          /* NVMe: 이미 활성화되어 있으면 */
		return -EBUSY;                          /* NVMe: 중복 활성화 방지 */

	/* Sanitize and check ST mode compatibility */
	mode &= PCI_TPH_CTRL_MODE_SEL_MASK;             /* NVMe: 상위 비트를 제거해 유효한 mode 값만 남김 */
	dev_modes = get_st_modes(pdev);                 /* NVMe: NVMe 장치가 지원하는 ST mode 획득 */
	if (!((1 << mode) & dev_modes))                 /* NVMe: 요청 mode가 장치 지원 비트에 없으면 */
		return -EINVAL;                         /* NVMe: 지원하지 않는 mode 오류 */

	pdev->tph_mode = mode;                          /* NVMe: 사용할 ST mode를 NVMe pdev에 저장 */

	/* Get req_type supported by device and its Root Port */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg); /* NVMe: TPH Capability 레지스터 읽기 */
	if (FIELD_GET(PCI_TPH_CAP_EXT_TPH, reg))        /* NVMe: 장치가 16bit Extended TPH를 지원하면 */
		pdev->tph_req_type = PCI_TPH_REQ_EXT_TPH; /* NVMe: 확장 TPH 요청 형식 선택 */
	else                                            /* NVMe: 16bit 확장 TPH 미지원 시 */
		pdev->tph_req_type = PCI_TPH_REQ_TPH_ONLY; /* NVMe: 기본 8bit TPH 요청 형식 선택 */

	/* Check if the device is behind a Root Port */
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_END) { /* NVMe: Root Complex integrated endpoint가 아닌 일반 endpoint면 */
		rp_req_type = get_rp_completer_type(pdev); /* NVMe: Root Port completer capability 조회 */

		/* Final req_type is the smallest value of two */
		pdev->tph_req_type = min(pdev->tph_req_type, rp_req_type); /* NVMe: endpoint와 Root Port 중 능력이 낮은 쪽으로 제한 */
	}

	if (pdev->tph_req_type == PCI_TPH_REQ_DISABLE)  /* NVMe: 양쪽 모두 TPH를 지원하지 않으면 */
		return -EINVAL;                         /* NVMe: 활성화할 수 없음 */

	/* Write them into TPH control register */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg); /* NVMe: TPH Control 레지스터 읽기 */

	reg &= ~PCI_TPH_CTRL_MODE_SEL_MASK;             /* NVMe: 기존 mode 선택 필드 클리어 */
	reg |= FIELD_PREP(PCI_TPH_CTRL_MODE_SEL_MASK, pdev->tph_mode); /* NVMe: 선택한 ST mode 기록 */

	reg &= ~PCI_TPH_CTRL_REQ_EN_MASK;               /* NVMe: 기존 Request Enable 필드 클리어 */
	reg |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, pdev->tph_req_type); /* NVMe: 선택한 요청 형식 기록 */

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg); /* NVMe: NVMe pdev에 TPH Control 레지스터 기록 */

	pdev->tph_enabled = 1;                          /* NVMe: TPH 활성화 플래그 설정 */

	return 0;                                       /* NVMe: TPH 활성화 성공 */
}
EXPORT_SYMBOL(pcie_enable_tph);

/*
 * pci_restore_tph_state:
 *   NVMe 장치의 TPH 상태(Control register + ST table)를 이전에 저장한
 *   값으로 복원한다. PCIe AER, PME, D3cold wakeup, runtime resume, suspend
 *   resume 등 NVMe가 재초기화될 때 큐-CPU affinity 성능을 유지하기 위해
 *   PCI core가 호출한다.
 */
void pci_restore_tph_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *save_state;         /* NVMe: 저장된 TPH capability 상태 버퍼 */
	int num_entries, i, offset;                     /* NVMe: ST table 항목 수, 루프 변수, 오프셋 */
	u16 *st_entry;                                  /* NVMe: ST table 항목을 가리키는 포인터 */
	u32 *cap;                                       /* NVMe: 저장된 Control register를 가리키는 포인터 */

	if (!pdev->tph_cap)                             /* NVMe: TPH capability가 없으면 */
		return;                                 /* NVMe: 복원할 것 없음 */

	if (!pdev->tph_enabled)                         /* NVMe: TPH가 활성화되지 않은 상태면 */
		return;                                 /* NVMe: 복원할 필요 없음 */

	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_TPH); /* NVMe: 저장된 TPH 확장 capability 상태 검색 */
	if (!save_state)                                /* NVMe: 저장된 상태가 없으면 */
		return;                                 /* NVMe: 복원 불가 */

	/* Restore control register and all ST entries */
	cap = &save_state->cap.data[0];                 /* NVMe: 저장 버퍼의 첫 번째 u32가 Control register */
	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, *cap++); /* NVMe: Control register 복원 후 포인터 증가 */
	st_entry = (u16 *)cap;                          /* NVMe: 이후 u16 배열이 ST table 항목들 */
	offset = PCI_TPH_BASE_SIZEOF;                   /* NVMe: ST table은 capability 기본 크기 직후 시작 */
	num_entries = pcie_tph_get_st_table_size(pdev); /* NVMe: 복원할 ST 항목 개수 산출 */
	for (i = 0; i < num_entries; i++) {             /* NVMe: 모든 ST table 항목을 순회하며 복원 */
		pci_write_config_word(pdev, pdev->tph_cap + offset, /* NVMe: 해당 오프셋에 저장된 ST 기록 */
			      *st_entry++);
		offset += sizeof(u16);                      /* NVMe: 다음 16bit ST 항목 위치로 이동 */
	}
}

/*
 * pci_save_tph_state:
 *   NVMe 장치의 TPH 상태를 suspend, D3cold 진입, 또는 PCIe reset 전에
 *   저장한다. NVMe가 다시 켜질 때 pci_restore_tph_state()로 복원되어
 *   큐-CPU affinity 및 TPH 스티어링 설정이 유지된다.
 */
void pci_save_tph_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *save_state;         /* NVMe: 저장용 TPH capability 상태 버퍼 */
	int num_entries, i, offset;                     /* NVMe: ST table 항목 수, 루프 변수, 오프셋 */
	u16 *st_entry;                                  /* NVMe: ST table 항목 저장 포인터 */
	u32 *cap;                                       /* NVMe: Control register 저장 포인터 */

	if (!pdev->tph_cap)                             /* NVMe: TPH capability가 없으면 */
		return;                                 /* NVMe: 저장할 것 없음 */

	if (!pdev->tph_enabled)                         /* NVMe: TPH가 비활성화 상태면 */
		return;                                 /* NVMe: 저장할 필요 없음 */

	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_TPH); /* NVMe: TPH 확장 capability 저장 버퍼 찾기 */
	if (!save_state)                                /* NVMe: 저장 버퍼가 없으면 */
		return;                                 /* NVMe: 저장 불가 */

	/* Save control register */
	cap = &save_state->cap.data[0];                 /* NVMe: 저장 버퍼의 첫 u32 위치 */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, cap++); /* NVMe: Control register 읽어 저장 후 포인터 증가 */

	/* Save all ST entries in extended capability structure */
	st_entry = (u16 *)cap;                          /* NVMe: Control register 다음에 ST table 항목 저장 */
	offset = PCI_TPH_BASE_SIZEOF;                   /* NVMe: ST table 시작 오프셋 */
	num_entries = pcie_tph_get_st_table_size(pdev); /* NVMe: 저장할 ST 항목 개수 */
	for (i = 0; i < num_entries; i++) {             /* NVMe: 모든 ST 항목을 순회하며 저장 */
		pci_read_config_word(pdev, pdev->tph_cap + offset, /* NVMe: 현재 ST entry 값 읽기 */
			     st_entry++);
		offset += sizeof(u16);                      /* NVMe: 다음 16bit ST 항목 위치로 이동 */
	}
}

/*
 * pci_no_tph:
 *   "notph" 커널 파라미터가 지정된 경우 시스템 전체의 TPH를 비활성화한다.
 *   NVMe 장치도 TPH 요청을 하지 않게 되어, TPH 관련 호환성/안정성 문제를
 *   회피할 수 있다(성능은 저하될 수 있음).
 */
void pci_no_tph(void)
{
	pci_tph_disabled = true;                        /* NVMe: 전역 TPH 비활성화 플래그 설정 */

	pr_info("PCIe TPH is disabled\n");              /* NVMe: 커널 로그에 TPH 비활성화 메시지 출력 */
}

/*
 * pci_tph_init:
 *   NVMe 장치 탐색(probe) 단계에서 TPH 확장 capability를 찾고, 이후
 *   save/restore를 위한 버퍼를 미리 할당한다. NVMe pdev 구조체의
 *   tph_cap 필드가 설정되며, 이후 NVMe 드라이버가 TPH를 활용할 수
 *   있는 기반이 마련된다.
 */
void pci_tph_init(struct pci_dev *pdev)
{
	int num_entries;                                /* NVMe: ST table 항목 개수 */
	u32 save_size;                                  /* NVMe: 저장 버퍼 크기(Control register + ST table) */

	pdev->tph_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_TPH); /* NVMe: NVMe pdev config space에서 TPH 확장 capability 위치 검색 */
	if (!pdev->tph_cap)                             /* NVMe: TPH capability를 찾지 못하면 */
		return;                                 /* NVMe: 초기화할 것 없음 */

	num_entries = pcie_tph_get_st_table_size(pdev); /* NVMe: TPH capability에 기록된 ST table 크기 획득 */
	save_size = sizeof(u32) + num_entries * sizeof(u16); /* NVMe: Control register 4byte + ST entry당 2byte 크기 계산 */
	pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_TPH, save_size); /* NVMe: suspend/reset 복원용 저장 버퍼 등록 */
}
