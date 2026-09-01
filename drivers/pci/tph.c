// SPDX-License-Identifier: GPL-2.0
/*
 * TPH (TLP Processing Hints) support
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *     Eric Van Tassell <Eric.VanTassell@amd.com>
 *     Wei Huang <wei.huang2@amd.com>
 */

/*
 * [한국어 설명] 장치가 "이 데이터를 어디에 두라" 고 힌트를 주는 기능 (tph.c)
 *
 * === 파일의 역할 ===
 * TPH(TLP Processing Hints)는 장치가 DMA 를 하면서 "이 데이터는 곧
 * 어느 CPU 가 쓸 것" 이라는 힌트를 함께 보내는 기능이다. 그러면
 * Root Complex 가 그 데이터를 해당 CPU 의 캐시(또는 가까운 메모리
 * 컨트롤러)에 미리 넣어 둘 수 있다.
 *
 * 힌트는 두 부분이다.
 *   PH (Processing Hint, 2비트) — 데이터의 성격. 곧 읽힐 것인가,
 *     쓰기만 될 것인가, 양방향인가, 시간 지역성이 있는가.
 *   ST (Steering Tag, 8~16비트) — 목적지. 어느 CPU/캐시인지를 나타내는
 *     플랫폼 고유 값이다. 커널이 그 값을 알아내 장치에 알려 줘야 한다.
 *
 * ST 값을 얻는 방법이 이 파일의 어려운 부분이다. 플랫폼마다 다르고,
 * ACPI 의 _DSM 이나 아키텍처 고유 인터페이스로 물어봐야 한다. 그리고
 * 장치는 그 값을 어디에 저장할지도 두 가지다 —
 * MSI-X 테이블 항목 안(ST Table Location = MSI-X)이거나,
 * TPH capability 안의 전용 테이블이거나.
 *
 * 왜 유용한가. NVMe 같은 고성능 장치는 완료 큐 엔트리를 쓰고 바로
 * 인터럽트를 올린다. 그 인터럽트를 받은 CPU 가 그 엔트리를 읽는데,
 * 데이터가 그 CPU 의 캐시에 없으면 메모리까지 다녀와야 한다. TPH 로
 * 미리 그 캐시에 넣어 두면 그 지연이 사라진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거: pci_init_capabilities() [probe.c:6389] 안에서 probe.c:6446
 *         -> [이 파일] pci_tph_init()
 *            -> TPH capability 오프셋을 dev->tph_cap 에 캐시하고,
 *               suspend/resume 용 저장 버퍼를 ST 표 크기만큼 미리 잡는다
 *
 * 사용(드라이버가 부르는 순서):
 *         -> [이 파일] pcie_enable_tph(pdev, mode)
 *            ST 모드를 고르고 요청 종류(TPH / Extended TPH)를 정한다
 *         -> [이 파일] pcie_tph_get_cpu_st(pdev, mem_type, cpu, &tag)
 *            그 CPU 에 해당하는 ST 값을 펌웨어에 묻는다(ACPI _DSM rev 7 func 0xF)
 *         -> [이 파일] pcie_tph_set_st_entry(pdev, index, tag)
 *            ST 값을 MSI-X 테이블 또는 TPH capability 안의 ST 표에 기록한다
 *         -> [이 파일] pcie_disable_tph(pdev) 로 끝낸다
 *
 * 전원 관리: pci_save_state() 경로가 pci_save_tph_state()(pci.c:3346)를,
 *   pci_restore_state() 경로가 pci_restore_tph_state()(pci.c:3540)를 부른다.
 *   Control 레지스터와 ST 표 전체가 저장 대상이다.
 *
 * 전역 차단: 부팅 인자 "pci=notph" 가 pci_no_tph()(pci.c:14036)를 불러
 *   pcie_enable_tph() 가 언제나 실패하게 만든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 평가와 MSI-X 테이블 접근이 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 이 파일이 EXPORT 하는 여섯 함수(pcie_enable_tph, pcie_disable_tph,
 *   pcie_tph_get_cpu_st, pcie_tph_set_st_entry, pcie_tph_get_st_table_loc,
 *   pcie_tph_get_st_table_size)를 쓰는 드라이버들. 이 스파스 체크아웃
 *   안에는 그 호출자가 하나도 없다(전수 grep) — 트리 밖 드라이버가 쓴다.
 *   커널 내부용 넷(pci_tph_init, pci_no_tph, pci_save_tph_state,
 *   pci_restore_tph_state)의 호출자는 위 "전체 아키텍처" 절에 적은 대로
 *   probe.c 와 pci.c 다. 원형은 drivers/pci/pci.h:2425-2428 에 있고,
 *   CONFIG 가 꺼지면 같은 헤더 2430-2433 의 빈 인라인 스텁이 들어간다.
 * 아래쪽: drivers/pci/msi/msi.c:2476 의 pci_msix_write_tph_tag() —
 *   ST 값을 MSI-X 테이블 항목에 써 넣는다.
 *   ACPI 의 acpi_check_dsm() / acpi_evaluate_dsm() 과 acpi_get_cpu_uid().
 *   그 구현은 drivers/acpi 에 있는데 이 스파스 체크아웃에는 그 디렉터리가
 *   없어(acpi_get_cpu_uid 는 이 트리에서 이 파일 밖 등장이 0건),
 *   호출 이후의 동작은 코드로 추적하지 못했다.
 *   pci_acpi_dsm_guid 는 이 트리의 pci-label.c 와 pcie/edr.c 도 함께 쓰는
 *   공용 GUID 다 — 같은 GUID 에 rev 와 func 만 달리해 여러 기능을 태운다.
 *   pcie_capability_read_dword(), pci_find_ext_capability(),
 *   pci_add_ext_cap_save_buffer(), pci_find_saved_ext_cap().
 * 공유 상태: struct pci_dev 의 tph_cap(capability 오프셋),
 *   tph_mode(고른 ST 모드), tph_req_type(TPH / Extended TPH),
 *   tph_enabled(켜져 있는가). 네 필드 모두 이 파일 밖에서 건드리는 코드가
 *   이 트리에 없다(전수 grep). struct pci_dev 의 선언은
 *   include/linux/pci.h 에 있고 이 트리에 없어, 네 필드의 존재는 이 파일의
 *   사용례로만 확인했다.
 *
 * 이 파일이 쓰는 PCI_TPH_* 와 PCI_EXP_DEVCAP2_TPH_COMP_MASK,
 * PCI_EXT_CAP_ID_TPH 상수의 실제 값은 확인하지 못했다 — 정의가 있어야 할
 * include/linux/pci_regs.h 가 이 스파스 체크아웃에 없다. 아래 주석들은
 * 값 대신 "코드가 그 상수를 어떻게 쓰는가"(FIELD_GET/FIELD_PREP 로 다루는
 * 필드 마스크인지, 단일 비트인지, 구조체 크기 상수인지)로 설명한다.
 *
 * === 주요 함수/구조체 요약 ===
 * 외부에 공개(EXPORT_SYMBOL)되는 여섯:
 * pcie_enable_tph()          : ST 모드를 골라 TPH 를 켠다. 장치가 그 모드를
 *                              지원하는지 확인하고, 장치와 Root Port 의
 *                              능력 중 작은 쪽으로 요청 종류를 정한다.
 * pcie_disable_tph()         : Control 레지스터를 0 으로 만들고 캐시를 지운다.
 * pcie_tph_get_cpu_st()      : 특정 CPU 에 대응하는 ST 값을 ACPI _DSM 으로 묻는다.
 * pcie_tph_set_st_entry()    : ST 값을 장치에 기록한다. 쓰는 동안 TPH 를
 *                              잠시 껐다 켜는 것이 요점이다.
 * pcie_tph_get_st_table_loc(): ST 표가 어디에 있는가(없음 / capability 안 /
 *                              MSI-X 표 안).
 * pcie_tph_get_st_table_size(): capability 안에 있을 때 그 표의 항목 수.
 *
 * 커널 내부용 넷:
 * pci_tph_init()             : 열거 때 capability 를 찾고 저장 버퍼를 잡는다.
 * pci_no_tph()               : "pci=notph" 로 TPH 를 전역으로 끈다.
 * pci_save_tph_state() / pci_restore_tph_state()
 *                            : Control 레지스터와 ST 표 전체를 저장하고 복원한다.
 *
 * 파일 내부 헬퍼:
 * tph_invoke_dsm()           : _DSM(rev 7, func 0xF)을 평가해 st_info 를 받는다.
 * tph_extract_tag()          : 그 응답에서 메모리 종류와 요청 종류에 맞는
 *                              태그(8비트 또는 16비트)를 골라낸다.
 * set_ctrl_reg_req_en()      : Control 의 Requester Enable 필드만 갈아 끼운다.
 * get_st_modes()             : 장치가 지원하는 ST 모드 세 비트를 뽑는다.
 * get_rp_completer_type()    : Root Port 가 완료자로서 지원하는 TPH 종류.
 * write_tag_to_st_table()    : capability 안의 ST 표에 태그 하나를 쓴다.
 *
 * union st_info              : _DSM 이 돌려주는 64비트 응답의 비트 배치.
 *                              휘발성/영속 메모리 각각에 대해 8비트 ST 와
 *                              16비트 확장 ST, 그리고 그 유효 비트를 담는다.
 * pci_tph_disabled           : "pci=notph" 로 세워지는 전역 차단 플래그.
 *
 * (기존 요약에는 pcie_tph_init() / tph_write_tag_to_msix() /
 *  pcie_tph_intr_vec_supported() 세 이름이 올라 있었으나 그런 함수는 이 트리
 *  어디에도 없다 — 각각의 유일한 등장이 그 요약 줄 자체였다. 실제 이름은
 *  pci_tph_init() 이고, MSI-X 쪽 쓰기는 이 파일이 아니라
 *  drivers/pci/msi/msi.c:2476 의 pci_msix_write_tph_tag() 이며, 모드 지원
 *  여부는 별도 함수가 아니라 pcie_enable_tph() 안의
 *  get_st_modes() 비트 검사로 처리한다.)
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 쓰지 않는다(전수 확인).
 *
 * 기술적으로는 잘 맞는 조합이다. NVMe 는 큐마다 MSI-X 벡터를 갖고 그
 * 벡터가 특정 CPU 에 묶여 있으므로(irq affinity), 그 큐의 완료 엔트리를
 * 그 CPU 캐시로 보내면 이득이 크다. 그리고 ST 값을 MSI-X 테이블에
 * 저장하는 방식이 그 구조와 자연스럽게 맞는다.
 *
 * 그럼에도 NVMe 가 쓰지 않는 이유는 코드에서 확인할 수 없다. 플랫폼
 * 지원이 아직 제한적이라는 것이 일반적인 설명이지만, 이 트리의
 * 정보만으로는 확인할 수 없다.
 *
 * (기존 주석은 NVMe 가 "큐-CPU affinity 설정과 함께 TPH 를 활용한다" 는
 *  취지로 적었으나, drivers/nvme/ 에 TPH 관련 호출은 0건이다.)
 */

/* [한국어] struct pci_dev, pci_read_config_dword/word, pci_write_config_dword/word,
 * pci_find_ext_capability, pcie_find_root_port, pcie_capability_read_dword,
 * pci_pcie_type, pci_dbg 등 PCI 코어 API */
#include <linux/pci.h>
/* [한국어] pci_acpi_dsm_guid — PCI 용 공용 _DSM GUID. 이 트리의 pci-label.c 와
 * pcie/edr.c 도 같은 GUID 를 rev/func 만 달리해 함께 쓴다 */
#include <linux/pci-acpi.h>
/* [한국어] pci_msix_write_tph_tag() — ST 표가 MSI-X 안에 있는 장치에서 태그를
 * 써 넣는 경로. 정의는 drivers/pci/msi/msi.c:2476 */
#include <linux/msi.h>
/* [한국어] FIELD_GET / FIELD_PREP — 마스크 상수 하나로 비트 필드를 뽑고 넣는 매크로.
 * 시프트 값을 손으로 적지 않아도 되므로, 이 파일이 PCI_TPH_*_MASK 의
 * 실제 값을 몰라도 의미가 통하는 이유가 된다 */
#include <linux/bitfield.h>
/* [한국어] enum tph_mem_type(TPH_MEM_TYPE_VM / _PM)와 이 파일이 공개하는 API 의 선언.
 * 이 스파스 체크아웃에는 그 헤더 파일이 없어 내용을 직접 확인하지는 못했다 */
#include <linux/pci-tph.h>

/* [한국어] PCI 코어 내부 헤더. pci_tph_init(), pci_no_tph(), pci_save_tph_state(),
 * pci_restore_tph_state() 의 원형과 pci_add_ext_cap_save_buffer(),
 * pci_find_saved_ext_cap() 이 여기 있다 */
#include "pci.h"

/* System-wide TPH disabled */
/* [한국어] 부팅 인자 "pci=notph" 로 TPH 를 전역 차단했는지.
 * 설정자: pci_no_tph()(pci.c:14036 에서 불린다).
 * 읽는 자: pcie_enable_tph() 하나뿐 — 참이면 곧바로 -EINVAL 이다.
 * 값 범위: false 가 기본. 한 번 true 가 되면 되돌아가지 않는다.
 * 동기화: 부팅 초기에 한 번 쓰고 이후 읽기 전용이라 락이 없다.
 * 주의: pci_tph_init() 은 이 값을 보지 않는다 — capability 탐색과 저장
 *   버퍼 할당은 차단 중에도 그대로 돈다 */
static bool pci_tph_disabled;

/* [한국어] 이 블록 안의 세 가지(union st_info, tph_extract_tag, tph_invoke_dsm)는
 * ACPI _DSM 응답을 다루는 코드다. ACPI 가 없는 커널에서는 ST 값을 얻을
 * 경로가 아예 없으므로 통째로 뺀다. 그 경우 pcie_tph_get_cpu_st() 의
 * 본문도 -ENODEV 한 줄로 바뀐다(그 함수 안의 #else 갈래) */
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
union st_info {
	/* [한국어] 익명 구조체와 u64 를 겹쳐 두어, 펌웨어가 준 64비트 한 덩어리를
	 * 비트필드로 해석할 수도 있고(struct 쪽) 통째로 대입할 수도 있게(value 쪽)
	 * 한다. tph_invoke_dsm() 이 value 에 한 번에 넣고 tph_extract_tag() 가
	 * 필드로 읽는 것이 그 쓰임이다 */
	struct {
		/* [한국어] 휘발성 메모리용 8비트 ST 가 유효한가(위 원문 주석 그대로).
		 * 설정자: 펌웨어(_DSM 응답).
		 * 읽는 자: tph_extract_tag() 의 TPH_ONLY + VM 갈래.
		 * 값 범위: 0 이면 그 조합을 지원하지 않는다는 뜻이라 태그 0 이 반환된다.
		 * 동기화: 스택 지역 변수라 공유되지 않는다 */
		u64 vm_st_valid : 1;
		/* [한국어] 휘발성 메모리용 16비트 확장 ST 가 유효한가.
		 * 읽는 자: tph_extract_tag() 의 EXT_TPH + VM 갈래 */
		u64 vm_xst_valid : 1;
		/* [한국어] PH(Processing Hint)를 무시했고 앞으로도 무시할 것인가(1) 아니면
		 * 공급해야 하는가(0). 위 원문 주석이 그 의미를 적고 있다.
		 * 이 파일의 어느 코드도 이 필드를 읽지 않는다 — 비트 배치를 맞추기 위해
		 * 존재하며, 그 배치가 맞아야 아래 vm_st 의 오프셋이 옳다 */
		u64 vm_ph_ignore : 1;
		/* [한국어] 예약 5비트. 이름 그대로 쓰이지 않지만, 이 자리를 비워 두어야
		 * 뒤따르는 필드들이 스펙이 정한 비트 위치에 오게 된다 */
		u64 rsvd1 : 5;
		/* [한국어] 휘발성 메모리용 8비트 ST 값.
		 * 읽는 자: tph_extract_tag() — vm_st_valid 가 참일 때만 돌려준다 */
		u64 vm_st : 8;
		/* [한국어] 휘발성 메모리용 16비트 확장 ST 값.
		 * 읽는 자: tph_extract_tag() — vm_xst_valid 가 참일 때만 돌려준다.
		 * 확장 TPH 를 장치와 Root Port 가 모두 지원할 때 쓰인다 */
		u64 vm_xst : 16;
		/* [한국어] 영속 메모리용 8비트 ST 가 유효한가. 위 vm_ 계열과 대칭이며,
		 * 64비트의 상위 절반이 영속 메모리 쪽에 배정되어 있다 */
		u64 pm_st_valid : 1;
		/* [한국어] 영속 메모리용 16비트 확장 ST 가 유효한가 */
		u64 pm_xst_valid : 1;
		/* [한국어] 영속 메모리 쪽 PH 무시 여부. vm_ph_ignore 와 마찬가지로 이 파일에서
		 * 읽지 않으며 비트 배치를 맞추는 역할이다 */
		u64 pm_ph_ignore : 1;
		/* [한국어] 예약 5비트(상위 절반 쪽) */
		u64 rsvd2 : 5;
		/* [한국어] 영속 메모리용 8비트 ST 값.
		 * 읽는 자: tph_extract_tag() 의 TPH_ONLY + PM 갈래 */
		u64 pm_st : 8;
		/* [한국어] 영속 메모리용 16비트 확장 ST 값.
		 * 읽는 자: tph_extract_tag() 의 EXT_TPH + PM 갈래 */
		u64 pm_xst : 16;
	};
	/* [한국어] 같은 64비트를 통째로 다루는 창.
	 * 설정자: tph_invoke_dsm() 이 _DSM 응답 버퍼의 앞 8바이트를 여기에 넣는다.
	 * 읽는 자: 위 비트필드들을 통해 간접적으로.
	 * 왜 필요한가: 펌웨어 응답을 필드 하나씩 옮기지 않고 한 번의 대입으로
	 *   받아들이기 위해서다 */
	u64 value;
};
/* [한국어]
 * tph_extract_tag - _DSM 응답에서 알맞은 Steering Tag 를 골라낸다
 *
 * @mem_type: 대상 메모리 종류(TPH_MEM_TYPE_VM 휘발성 / TPH_MEM_TYPE_PM 영속)
 * @req_type: 이 장치가 낼 수 있는 요청 종류(PCI_TPH_REQ_TPH_ONLY 또는 _EXT_TPH)
 * @info: tph_invoke_dsm() 이 받아 온 64비트 응답
 * @return: 골라낸 태그. 유효한 값이 없으면 0
 *
 * _DSM 은 네 가지 태그를 한 번에 돌려준다 — (휘발성, 영속) x (8비트, 16비트).
 * 그중 어느 것을 쓸지는 두 축으로 정해진다.
 *   req_type  — 장치와 Root Port 가 확장 TPH 를 둘 다 지원하면 16비트
 *               확장 태그를, 아니면 8비트 태그를 쓴다.
 *   mem_type  — 이 DMA 가 향하는 메모리가 보통 RAM 인지 영속 메모리인지.
 *
 * 각 태그에는 짝이 되는 valid 비트가 있다. 펌웨어가 그 조합을 지원하지
 * 않으면 valid 가 0 이고, 그때는 0 을 돌려준다. 0 을 "태그 없음" 으로
 * 쓰는 셈인데, 호출자(pcie_tph_get_cpu_st)는 그것을 오류로 보지 않고
 * 그대로 전달한다 — 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * default 갈래는 req_type 이 두 값 중 어느 것도 아닐 때다. 그런 장치에는
 * 태그를 줄 수 없으므로 0 이다.
 *
 * CONFIG_ACPI 안에서만 컴파일된다. _DSM 응답을 해석하는 것이 유일한
 * 용도이기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 비트 선택이다.
 *
 * 호출 체인:
 *   pcie_tph_get_cpu_st() → [이 함수]
 */
static u16 tph_extract_tag(enum tph_mem_type mem_type, u8 req_type,
			   union st_info *info)
{
	switch (req_type) {
	/* [한국어] 8비트 태그를 쓰는 경우 — 장치나 Root Port 중 한쪽이라도 확장 TPH 를
	 * 지원하지 않으면 여기로 온다 */
	case PCI_TPH_REQ_TPH_ONLY: /* 8-bit tag */
		switch (mem_type) {
		/* [한국어] 보통 RAM 을 향하는 DMA */
		case TPH_MEM_TYPE_VM:
			if (info->vm_st_valid)
				/* [한국어] 유효할 때만 8비트 ST 를 돌려준다. 유효하지 않으면 break 로 빠져
				 * 아래 공통 return 0 에 닿는다 */
				return info->vm_st;
			break;
		/* [한국어] 영속 메모리를 향하는 DMA */
		case TPH_MEM_TYPE_PM:
			if (info->pm_st_valid)
				/* [한국어] 영속 메모리용 8비트 ST */
				return info->pm_st;
			break;
		}
		break;
	/* [한국어] 16비트 확장 태그를 쓰는 경우 — 양쪽 다 확장 TPH 를 지원할 때다 */
	case PCI_TPH_REQ_EXT_TPH: /* 16-bit tag */
		switch (mem_type) {
		/* [한국어] 보통 RAM 쪽 확장 태그 */
		case TPH_MEM_TYPE_VM:
			if (info->vm_xst_valid)
				/* [한국어] 유효할 때만 돌려준다 */
				return info->vm_xst;
			break;
		/* [한국어] 영속 메모리 쪽 확장 태그 */
		case TPH_MEM_TYPE_PM:
			if (info->pm_xst_valid)
				/* [한국어] 유효할 때만 돌려준다 */
				return info->pm_xst;
			break;
		}
		break;
	default:
		return 0;
	}
	return 0;
}

/* [한국어] 이 _DSM 안에서 캐시 지역성 TPH 질의를 가리키는 함수 번호.
 * union st_info 위의 원문 주석이 밝힌 rev=0x7, func=0xF 중 뒤쪽이다.
 * 두 곳에서 쓰인다 — acpi_check_dsm() 에 BIT() 으로 묶어 "이 함수가 있는가"
 * 를 묻고, acpi_evaluate_dsm() 에 그대로 넘겨 실제로 부른다 */
#define TPH_ST_DSM_FUNC_INDEX	0xF
/* [한국어]
 * tph_invoke_dsm - 캐시 지역성 질의 _DSM 을 평가해 ST 정보를 받는다
 *
 * @handle: Root Port 브리지의 ACPI 핸들
 * @cpu_uid: 대상 CPU 의 ACPI UID
 * @st_out: 응답 64비트를 담을 곳
 * @return: AE_OK = 성공, AE_ERROR = _DSM 이 없거나 평가 실패 또는 타입 불일치
 *
 * 이 파일에서 플랫폼에 직접 묻는 유일한 지점이다. 위 union st_info 에
 * 붙은 원문 주석이 근거를 밝힌다 — PCI Firmware Spec 의 승인된 ECN 에
 * 정의된 rev 7, func 0xF "_DSM to Query Cache Locality TPH Features" 다.
 *
 * 먼저 acpi_check_dsm() 으로 그 함수가 구현되어 있는지 확인한다. 없으면
 * 평가를 시도하지도 않고 AE_ERROR 다.
 *
 * 인자 셋을 조립한다(코드 주석이 각각의 뜻을 적고 있다).
 *   arg3[0] DWORD — feature ID. 0 이 프로세서 캐시 ST 질의다.
 *   arg3[1] DWORD — 대상 CPU 의 UID. 이것이 "어느 CPU 의 캐시인가" 를 정한다.
 *   arg3[2] QWORD — properties. 전부 0.
 * 셋을 패키지 하나(in_obj)로 묶어 넘긴다.
 *
 * 응답은 반드시 버퍼 타입이어야 한다. acpi_evaluate_dsm()(타입을 강제하지
 * 않는 판)을 쓰므로 타입 검사를 손으로 한다. 통과하면 버퍼의 처음 8바이트를
 * u64 로 읽어 st_out 에 넣는다.
 *
 * 두 실패 경로와 성공 경로 모두 ACPI_FREE 를 부른다 — 반환된 객체의
 * 해제는 호출자 책임이다.
 *
 * 핸들이 장치 자신이 아니라 Root Port 브리지의 것인 점이 중요하다.
 * ST 는 "Root Complex 가 데이터를 어디에 둘지" 의 문제라 그 경로를 아는
 * 쪽이 Root Port 이기 때문이다(pcie_tph_get_cpu_st 가 그 핸들을 구해 넘긴다).
 *
 * CONFIG_ACPI 안에서만 컴파일된다. acpi_check_dsm/acpi_evaluate_dsm 의
 * 구현은 drivers/acpi 에 있는데 이 스파스 체크아웃에는 그 디렉터리가 없어,
 * 호출 이후의 동작은 코드로 추적하지 못했다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 평가가 잠들 수 있다.
 *
 * 호출 체인:
 *   pcie_tph_get_cpu_st() → [이 함수] → acpi_check_dsm() → acpi_evaluate_dsm()
 */
static acpi_status tph_invoke_dsm(acpi_handle handle, u32 cpu_uid,
				  union st_info *st_out)
{
	union acpi_object arg3[3], in_obj, *out_obj;
	/* [한국어] 이 Root Port 의 펌웨어에 rev 7 의 0xF 함수가 구현되어 있는지 먼저 묻는다.
	 * 없으면 평가를 시도하지도 않는다 */
	if (!acpi_check_dsm(handle, &pci_acpi_dsm_guid, 7,
			    BIT(TPH_ST_DSM_FUNC_INDEX)))
		return AE_ERROR;
	/* DWORD: feature ID (0 for processor cache ST query) */
	arg3[0].integer.type = ACPI_TYPE_INTEGER;
	/* [한국어] feature ID. 0 이 프로세서 캐시 ST 질의를 뜻한다(바로 위 원문 주석) */
	arg3[0].integer.value = 0;

	/* DWORD: target UID */
	arg3[1].integer.type = ACPI_TYPE_INTEGER;
	/* [한국어] 어느 CPU 의 캐시를 원하는지. 커널 CPU 번호가 아니라 ACPI UID 이며,
	 * 호출자가 acpi_get_cpu_uid() 로 옮겨 넘긴다 */
	arg3[1].integer.value = cpu_uid;

	/* QWORD: properties, all 0's */
	arg3[2].integer.type = ACPI_TYPE_INTEGER;
	/* [한국어] properties. 위 원문 주석대로 전부 0 이다 — 추가로 요구할 속성이 없다 */
	arg3[2].integer.value = 0;

	/* [한국어] 세 인자를 패키지 하나로 묶는다. _DSM 의 네 번째 인자는 언제나
	 * 패키지 형태이기 때문이다 */
	in_obj.type = ACPI_TYPE_PACKAGE;
	/* [한국어] 원소 개수는 배열 크기에서 자동으로 얻는다. 인자를 늘려도 이 줄은
	 * 고칠 필요가 없다 */
	in_obj.package.count = ARRAY_SIZE(arg3);
	/* [한국어] 위에서 채운 배열을 원소로 지정한다 */
	in_obj.package.elements = arg3;

	/* [한국어] _DSM 을 평가한다. _typed 판이 아니라 일반 판이라 타입 강제가 없고,
	 * 그래서 아래에서 타입을 손으로 확인한다 */
	out_obj = acpi_evaluate_dsm(handle, &pci_acpi_dsm_guid, 7,
				    TPH_ST_DSM_FUNC_INDEX, &in_obj);
	/* [한국어] 평가 실패. 메서드가 없거나 펌웨어가 오류를 냈다 */
	if (!out_obj)
		return AE_ERROR;

	/* [한국어] 응답이 버퍼가 아니면 해석할 수 없다. 이 경우에도 ACPI_FREE 를 잊지 않는다 */
	if (out_obj->type != ACPI_TYPE_BUFFER) {
		ACPI_FREE(out_obj);
		return AE_ERROR;
	}

	/* [한국어] 버퍼의 앞 8바이트를 u64 로 읽어 통째로 넣는다. 그 뒤 union 의
	 * 비트필드들이 같은 값을 필드 단위로 보여 준다.
	 * 버퍼 길이를 확인하지 않고 8바이트를 읽는데, 코드는 고치지 않고
	 * 이 관찰만 적어 둔다 */
	st_out->value = *((u64 *)(out_obj->buffer.pointer));

	ACPI_FREE(out_obj);

	return AE_OK;
}
#endif

/* Update the TPH Requester Enable field of TPH Control Register */
/* [한국어]
 * set_ctrl_reg_req_en - TPH Control 의 Requester Enable 필드만 갈아 끼운다
 *
 * @pdev: 대상 장치.  @req_type: 새 요청 종류(PCI_TPH_REQ_DISABLE / _TPH_ONLY / _EXT_TPH)
 * @return: 없음
 *
 * 읽고-고치고-쓰기의 전형이다. PCI_TPH_CTRL_REQ_EN_MASK 로 그 필드만 지우고
 * FIELD_PREP 으로 새 값을 그 자리에 맞춰 넣는다. ST Mode Select 같은 다른
 * 필드는 그대로 보존된다.
 *
 * FIELD_PREP(mask, value) 는 value 를 mask 의 최하위 비트 위치까지 밀어
 * 올려 mask 와 AND 한다. 시프트 값을 손으로 적지 않아도 되게 하는
 * 매크로이며, 마스크 상수 하나가 위치와 폭을 모두 담고 있다는 뜻이다.
 * 그래서 PCI_TPH_CTRL_REQ_EN_MASK 의 실제 값을 몰라도 이 코드의 의미는
 * 분명하다 — 다만 그 값 자체는 include/linux/pci_regs.h 가 이 트리에
 * 없어 확인하지 못했다.
 *
 * 용도가 둘이다. pcie_tph_set_st_entry() 가 ST 를 쓰기 직전에
 * PCI_TPH_REQ_DISABLE 로 껐다가, 쓴 뒤 원래 요청 종류로 되돌린다.
 *
 * config 접근의 실패를 확인하지 않는다. 코드는 고치지 않고 이 사실만 적어 둔다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_tph_set_st_entry() → [이 함수] → pci_read_config_dword()
 */
static void set_ctrl_reg_req_en(struct pci_dev *pdev, u8 req_type)
{
	u32 reg;

	/* [한국어] 현재 Control 값을 읽는다. 읽고-고치고-쓰기라 ST Mode Select 같은
	 * 다른 필드가 보존된다 */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg);

	/* [한국어] Requester Enable 필드만 지운다 */
	reg &= ~PCI_TPH_CTRL_REQ_EN_MASK;
	/* [한국어] 새 값을 그 자리로 밀어 넣는다. FIELD_PREP 이 시프트를 대신해 주므로
	 * 마스크 상수 하나가 위치와 폭을 모두 담는다 */
	reg |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, req_type);

	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg);
}

/* [한국어]
 * get_st_modes - 이 장치가 지원하는 ST 모드 세 비트를 뽑는다
 *
 * @pdev: 대상 장치.  @return: 지원 모드 비트들(NS / IV / DS)만 남긴 값
 *
 * TPH Capability 레지스터를 읽어 세 비트만 남긴다.
 *   PCI_TPH_CAP_ST_NS — No ST Mode. ST 를 쓰지 않고 PH 만 보낸다.
 *   PCI_TPH_CAP_ST_IV — Interrupt Vector Mode. MSI-X 벡터 번호가 곧
 *                       ST 표의 인덱스가 된다.
 *   PCI_TPH_CAP_ST_DS — Device Specific Mode. 장치가 알아서 인덱스를 고른다.
 *
 * pcie_enable_tph() 가 이 값을 (1 << mode) 와 AND 해서 요청한 모드가
 * 지원되는지 확인한다. 그 비교가 성립하려면 세 상수가 각각 모드 번호
 * 자리의 단일 비트여야 하는데, 그 값은 include/linux/pci_regs.h 가 이
 * 트리에 없어 확인하지 못했다 — 코드의 사용 방식이 그런 배치를 전제한다는
 * 것만 알 수 있다.
 *
 * 반환형이 u8 인데 reg 는 u32 다. 세 비트가 하위 8비트 안에 있다는
 * 전제이며, 역시 헤더가 없어 값으로는 확인하지 못했다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_enable_tph() → [이 함수] → pci_read_config_dword()
 */
static u8 get_st_modes(struct pci_dev *pdev)
{
	u32 reg;

	/* [한국어] TPH Capability 레지스터를 읽는다 */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);
	/* [한국어] ST 모드 세 비트만 남긴다 — No ST / Interrupt Vector / Device Specific.
	 * 나머지 능력 비트(확장 TPH 지원, 표 위치, 표 크기 등)는 여기서 걸러진다 */
	reg &= PCI_TPH_CAP_ST_NS | PCI_TPH_CAP_ST_IV | PCI_TPH_CAP_ST_DS;

	/* [한국어] u32 를 u8 로 좁혀 돌려준다. 세 비트가 하위 8비트 안에 있다는 전제이며,
	 * 그 값은 include/linux/pci_regs.h 가 이 트리에 없어 확인하지 못했다 */
	return reg;
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
/* [한국어]
 * pcie_tph_get_st_table_loc - ST 표가 어디에 있는지 돌려준다
 *
 * @pdev: 질의할 장치
 * @return: 위 원문 kernel-doc 이 적은 세 값 — 없음 / TPH 확장 capability 안 /
 *          MSI-X 표 안
 *
 * TPH Capability 의 ST Table Location 필드를 FIELD_GET 으로 뽑는다.
 * FIELD_GET 은 마스크 위치만큼 오른쪽으로 밀어 값을 정규화하므로,
 * 돌려주는 것은 "필드 값" 이지 원래 자리의 비트가 아니다.
 *
 * 그 점이 호출자들에게 함정이 된다. PCI_TPH_LOC_CAP / PCI_TPH_LOC_MSIX
 * 상수는 원래 자리에 있는 형태라, 비교하려면 FIELD_PREP 으로 도로 밀어
 * 올려야 한다. pcie_tph_get_st_table_size() 와 pcie_tph_set_st_entry()
 * 둘 다 그렇게 하고 있고, 각각 "Convert loc to match with PCI_TPH_LOC_*"
 * 라는 원문 주석을 달아 두었다. 코드는 고치지 않고 이 비대칭만 적어 둔다.
 *
 * EXPORT_SYMBOL 이지만 이 스파스 체크아웃 안에는 외부 호출자가 없다 —
 * 같은 파일의 두 함수만 부른다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_tph_get_st_table_size() / pcie_tph_set_st_entry() → [이 함수]
 */
u32 pcie_tph_get_st_table_loc(struct pci_dev *pdev)
{
	u32 reg;

	/* [한국어] TPH Capability 레지스터를 읽는다 */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);

	/* [한국어] ST Table Location 필드를 뽑는다. FIELD_GET 이 마스크 위치만큼 오른쪽으로
	 * 밀어 값을 정규화하므로, 돌려주는 것은 "필드 값" 이지 원래 자리의 비트가
	 * 아니다 — 그래서 호출자들이 PCI_TPH_LOC_* 와 비교하기 전에
	 * FIELD_PREP 으로 도로 밀어 올린다 */
	return FIELD_GET(PCI_TPH_CAP_LOC_MASK, reg);
}
EXPORT_SYMBOL(pcie_tph_get_st_table_loc);

/*
 * Return the size of ST table. If ST table is not in TPH Requester Extended
 * Capability space, return 0. Otherwise return the ST Table Size + 1.
 */
/* [한국어]
 * pcie_tph_get_st_table_size - capability 안 ST 표의 항목 수를 돌려준다
 *
 * @pdev: 질의할 장치
 * @return: 항목 수. 표가 capability 안에 없으면 0
 *
 * 위 원문 주석이 규약을 밝힌다 — ST 표가 TPH 확장 capability 공간에 있지
 * 않으면 0 을, 있으면 "ST Table Size 필드 + 1" 을 돌려준다.
 *
 * +1 인 이유는 스펙이 그 필드를 0 부터 세기 때문이다(0 이 항목 하나를 뜻한다).
 * 크기 0 인 표는 의미가 없으므로 한 칸을 아끼는 흔한 인코딩이다.
 *
 * 먼저 위치를 확인하는 것이 중요하다. MSI-X 표에 있는 장치에서 이
 * 필드를 읽으면 의미 없는 값이 나오고, 그것을 크기로 믿으면
 * write_tag_to_st_table() 이 엉뚱한 config 오프셋에 쓰게 된다.
 *
 * loc 를 FIELD_PREP 으로 도로 밀어 올린 뒤 PCI_TPH_LOC_CAP 과 비교하는
 * 부분은 pcie_tph_get_st_table_loc() 이 값을 정규화해 돌려주기 때문이다.
 * 코드 주석이 그 사정을 적고 있다.
 *
 * 세 곳에서 쓰인다 — write_tag_to_st_table() 의 범위 검사,
 * pci_save_tph_state() / pci_restore_tph_state() 의 반복 횟수,
 * 그리고 pci_tph_init() 의 저장 버퍼 크기 계산.
 *
 * EXPORT_SYMBOL 이지만 이 스파스 체크아웃 안에는 외부 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   write_tag_to_st_table() / pci_save_tph_state() / pci_restore_tph_state() /
 *   pci_tph_init() → [이 함수] → pcie_tph_get_st_table_loc()
 */
u16 pcie_tph_get_st_table_size(struct pci_dev *pdev)
{
	u32 reg;
	/* [한국어] 위 함수가 정규화해 돌려준 위치 값 */
	u32 loc;

	/* Check ST table location first */
	loc = pcie_tph_get_st_table_loc(pdev);

	/* Convert loc to match with PCI_TPH_LOC_* defined in pci_regs.h */
	loc = FIELD_PREP(PCI_TPH_CAP_LOC_MASK, loc);
	/* [한국어] capability 안에 있지 않으면 이 함수가 셀 수 있는 표가 아니다.
	 * MSI-X 표의 크기는 MSI-X 쪽이 관리한다 */
	if (loc != PCI_TPH_LOC_CAP)
		return 0;

	/* [한국어] 표 크기 필드를 읽기 위해 Capability 를 다시 읽는다 */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);

	/* [한국어] +1 하는 이유는 스펙이 이 필드를 0 부터 세기 때문이다 —
	 * 0 이 항목 하나를 뜻한다. 크기 0 인 표가 의미 없으므로 한 칸을 아끼는
	 * 흔한 인코딩이다 */
	return FIELD_GET(PCI_TPH_CAP_ST_MASK, reg) + 1;
}
EXPORT_SYMBOL(pcie_tph_get_st_table_size);

/* Return device's Root Port completer capability */
/* [한국어]
 * get_rp_completer_type - Root Port 가 완료자로서 지원하는 TPH 종류를 읽는다
 *
 * @pdev: 기준이 되는 장치(이 장치의 Root Port 를 찾는다)
 * @return: Root Port 의 TPH Completer Supported 필드 값. 못 찾으면 0
 *
 * TPH 는 양쪽이 맞아야 성립한다. 장치가 확장 TPH 요청을 낼 수 있어도
 * Root Port 가 그것을 완료자로서 받지 못하면 소용이 없다. 그래서
 * pcie_enable_tph() 가 장치 능력과 이 값 중 작은 쪽을 최종 요청 종류로
 * 삼는다.
 *
 * 값은 Root Port 의 PCIe Capability 안 Device Capabilities 2 레지스터에
 * 있는 TPH Completer Supported 필드다. 장치 자신이 아니라 Root Port 를
 * 읽는다는 점이 요점이다.
 *
 * 실패를 모두 0 으로 뭉뚱그린다 — Root Port 를 못 찾았을 때도, config
 * 읽기가 실패했을 때도 0 이다. 0 이 PCI_TPH_REQ_DISABLE 과 같은 값이면
 * 호출자의 min() 결과가 DISABLE 이 되어 pcie_enable_tph() 가 -EINVAL 로
 * 끝나는데, 그 상수의 실제 값은 include/linux/pci_regs.h 가 이 트리에
 * 없어 확인하지 못했다. 코드의 흐름이 그런 관계를 전제한다는 것만
 * 알 수 있다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_enable_tph() → [이 함수] → pcie_find_root_port() → pcie_capability_read_dword()
 */
static u8 get_rp_completer_type(struct pci_dev *pdev)
{
	struct pci_dev *rp;
	/* [한국어] Root Port 의 Device Capabilities 2 값 */
	u32 reg;
	/* [한국어] 읽기 결과 */
	int ret;

	/* [한국어] 이 장치의 Root Port 를 찾는다. TPH 는 요청자와 완료자가 모두 지원해야
	 * 성립하므로 상대편 능력을 봐야 한다 */
	rp = pcie_find_root_port(pdev);
	/* [한국어] Root Port 가 없으면(RC Integrated Endpoint 등) 0 으로 답한다 */
	if (!rp)
		return 0;

	/* [한국어] 장치 자신이 아니라 Root Port 의 PCIe Capability 를 읽는다 */
	ret = pcie_capability_read_dword(rp, PCI_EXP_DEVCAP2, &reg);
	/* [한국어] 읽기 실패도 0 으로 뭉뚱그린다 */
	if (ret)
		return 0;

	/* [한국어] TPH Completer Supported 필드를 뽑는다. 호출자가 이것과 장치 능력 중
	 * 작은 쪽을 최종 요청 종류로 삼는다 */
	return FIELD_GET(PCI_EXP_DEVCAP2_TPH_COMP_MASK, reg);
}

/* Write tag to ST table - Return 0 if OK, otherwise -errno */
/* [한국어]
 * write_tag_to_st_table - capability 안의 ST 표에 태그 하나를 쓴다
 *
 * @pdev: 대상 장치.  @index: 표 안의 항목 번호.  @tag: 쓸 Steering Tag
 * @return: 0 = 성공, -ENXIO = 인덱스가 표 범위를 벗어남, 그 밖에는 config 쓰기 오류
 *
 * ST 표가 TPH 확장 capability 안에 있는 경우의 쓰기 경로다.
 * MSI-X 표에 있는 장치는 이 함수가 아니라
 * drivers/pci/msi/msi.c:2476 의 pci_msix_write_tph_tag() 로 간다.
 *
 * 오프셋 계산이 이 함수의 전부다.
 *   tph_cap + PCI_TPH_BASE_SIZEOF + index * sizeof(u16)
 * PCI_TPH_BASE_SIZEOF 는 capability 헤더 부분의 크기이고, ST 표는 그
 * 바로 뒤에 이어진다. 항목 하나가 16비트라 index 에 2 를 곱한다.
 * 그 상수의 실제 값은 include/linux/pci_regs.h 가 이 트리에 없어
 * 확인하지 못했다 — 코드가 그것을 "표가 시작하는 상대 오프셋" 으로
 * 쓴다는 것만 알 수 있다.
 *
 * 범위 검사를 먼저 하는 이유가 중요하다. 이것이 없으면 큰 index 로
 * capability 바깥의 config 공간을 덮어쓰게 된다. 표가 capability 안에
 * 없는 장치에서는 크기가 0 이라 어떤 index 도 통과하지 못한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 TPH 를 잠시 꺼 둔 상태다.
 *
 * 호출 체인:
 *   pcie_tph_set_st_entry() → [이 함수] → pcie_tph_get_st_table_size()
 */
static int write_tag_to_st_table(struct pci_dev *pdev, int index, u16 tag)
{
	int st_table_size;
	/* [한국어] 쓸 config 오프셋 */
	int offset;

	/* Check if index is out of bound */
	st_table_size = pcie_tph_get_st_table_size(pdev);
	/* [한국어] 범위 검사. 이것이 없으면 큰 index 로 capability 바깥의 config 공간을
	 * 덮어쓴다. 표가 capability 안에 없는 장치에서는 크기가 0 이라
	 * 어떤 index 도 통과하지 못한다 */
	if (index >= st_table_size)
		return -ENXIO;

	/* [한국어] capability 시작 + 헤더 크기 + index*2. ST 표는 헤더 바로 뒤에 이어지고
	 * 항목 하나가 16비트다. PCI_TPH_BASE_SIZEOF 의 실제 값은
	 * include/linux/pci_regs.h 가 이 트리에 없어 확인하지 못했다 —
	 * 코드가 그것을 "표가 시작하는 상대 오프셋" 으로 쓴다는 것만 알 수 있다 */
	offset = pdev->tph_cap + PCI_TPH_BASE_SIZEOF + index * sizeof(u16);

	/* [한국어] 16비트 태그를 쓴다 */
	return pci_write_config_word(pdev, offset, tag);
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
/* [한국어]
 * pcie_tph_get_cpu_st - 특정 CPU 에 대응하는 Steering Tag 를 얻는다
 *
 * @pdev: PCI 장치.  @mem_type: 대상 메모리 종류(휘발성 / 영속)
 * @cpu: 대상 CPU 번호.  @tag: 얻은 태그를 담을 곳
 * @return: 0 = 성공, -ENODEV = ACPI 미지원 또는 Root Port 없음,
 *          -EINVAL = _DSM 평가 실패, 그 밖에는 acpi_get_cpu_uid() 의 오류
 *
 * 드라이버가 "이 큐의 완료는 3번 CPU 가 처리하니 그 캐시로 보내 달라" 를
 * 표현하려면 먼저 3번 CPU 에 해당하는 ST 값을 알아야 한다. 그 값은
 * 플랫폼 고유라 커널이 계산할 수 없고 펌웨어에 물어야 한다. 이 함수가
 * 그 질의다.
 *
 * 절차:
 *   1) 커널의 CPU 번호를 ACPI UID 로 옮긴다. 두 체계가 다르기 때문이다.
 *   2) 이 장치의 Root Port 를 찾고, 그 버스의 브리지에 달린 ACPI 핸들을
 *      얻는다. 장치 자신이 아니라 Root Port 쪽에 묻는 이유는 ST 가
 *      "Root Complex 가 데이터를 어디에 둘지" 의 문제이기 때문이다.
 *      rp, rp->bus, rp->bus->bridge 를 차례로 확인하는 것은 어느 하나라도
 *      없으면 핸들을 얻을 수 없어서다.
 *   3) _DSM 을 평가한다. 실패하면 태그를 0 으로 지우고 -EINVAL. 여기서만
     *tag 를 명시적으로 0 으로 만드는데, 실패 시 호출자가 쓰레기 값을
 *      쓰지 않게 하려는 것이다.
 *   4) tph_extract_tag() 로 요청 종류와 메모리 종류에 맞는 태그를 고른다.
 *      pdev->tph_req_type 을 쓰므로 pcie_enable_tph() 가 먼저 불려
 *      그 값이 정해져 있어야 한다.
 *
 * CONFIG_ACPI 가 꺼져 있으면 함수 본문이 통째로 -ENODEV 한 줄이다.
 * 이 파일에는 ACPI 말고 ST 값을 얻는 다른 경로가 없다.
 *
 * EXPORT_SYMBOL 이지만 이 스파스 체크아웃 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. ACPI 평가가 잠들 수 있다.
 *
 * 호출 체인:
 *   (트리 밖 드라이버) → [이 함수]
 *     → acpi_get_cpu_uid() → pcie_find_root_port() → tph_invoke_dsm()
 *     → tph_extract_tag()
 */
int pcie_tph_get_cpu_st(struct pci_dev *pdev, enum tph_mem_type mem_type,
			unsigned int cpu, u16 *tag)
{
/* [한국어] 본문 전체가 ACPI 에 의존한다. 이 파일에는 ST 값을 얻는 다른 경로가
 * 없으므로, ACPI 가 없는 커널에서는 아래 #else 갈래가 -ENODEV 한 줄로
 * 대신한다. 지역 변수 선언까지 이 안에 있는 이유도 그것들이 전부
 * ACPI 타입이거나 ACPI 호출에만 쓰이기 때문이다 */
#ifdef CONFIG_ACPI
	/* [한국어] 이 장치의 Root Port */
	struct pci_dev *rp;
	/* [한국어] 그 Root Port 브리지의 ACPI 핸들 */
	acpi_handle rp_acpi_handle;
	/* [한국어] _DSM 응답을 받을 곳 */
	union st_info info;
	/* [한국어] 커널 CPU 번호에 대응하는 ACPI UID */
	u32 cpu_uid;
	/* [한국어] 반환값 */
	int ret;

	/* [한국어] 커널의 CPU 번호와 ACPI 의 UID 는 서로 다른 체계라 변환이 필요하다.
	 * acpi_get_cpu_uid() 의 정의는 drivers/acpi 에 있는데 이 스파스
	 * 체크아웃에는 그 디렉터리가 없어, 이 트리에서는 이 파일 밖 등장이 0건이다 */
	ret = acpi_get_cpu_uid(cpu, &cpu_uid);
	if (ret != 0)
		return ret;

	/* [한국어] Root Port 를 찾는다 */
	rp = pcie_find_root_port(pdev);
	/* [한국어] 핸들에 닿으려면 셋이 다 있어야 한다. 어느 하나라도 없으면
	 * ACPI_HANDLE 을 부를 수 없다 */
	if (!rp || !rp->bus || !rp->bus->bridge)
		return -ENODEV;

	/* [한국어] 장치 자신이 아니라 Root Port 버스의 브리지에 묻는다. ST 는
	 * "Root Complex 가 데이터를 어디에 둘지" 의 문제라 그 경로를 아는 쪽이
	 * Root Port 이기 때문이다 */
	rp_acpi_handle = ACPI_HANDLE(rp->bus->bridge);

	if (tph_invoke_dsm(rp_acpi_handle, cpu_uid, &info) != AE_OK) {
		*tag = 0;
		return -EINVAL;
	}

	*tag = tph_extract_tag(mem_type, pdev->tph_req_type, &info);

	/* [한국어] 얻은 태그를 디버그 로그로 남긴다. 메모리 종류를 문자열로 풀어
	 * 어느 조합의 태그인지 바로 보이게 한다 */
	pci_dbg(pdev, "get steering tag: mem_type=%s, cpu=%d, tag=%#04x\n",
		(mem_type == TPH_MEM_TYPE_VM) ? "volatile" : "persistent",
		cpu, *tag);

	return 0;
/* [한국어] CONFIG_ACPI 가 없는 커널의 구현 */
#else
	return -ENODEV;
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
/* [한국어]
 * pcie_tph_set_st_entry - Steering Tag 를 장치의 ST 표에 기록한다
 *
 * @pdev: PCI 장치.  @index: ST 표 항목 번호.  @tag: 쓸 Steering Tag
 * @return: 0 = 성공, -EINVAL = TPH 미지원/미활성 또는 표 위치가 이상함,
 *          그 밖에는 쓰기 실패값
 *
 * 위 원문 kernel-doc 이 요지를 밝힌다 — ST 표의 실제 위치를 알아내
 * MSI-X 표든 TPH capability 공간이든 알맞은 곳에 태그를 써 넣는다.
 *
 * 이 함수에서 가장 중요한 것은 쓰기 전후로 TPH 를 껐다 켜는 부분이다.
 * 코드 안의 원문 주석이 PCIe r6.2 sec 6.17.3 "ST Modes of Operation" 의
 * 경고를 인용한다 — ST 를 갱신하는 동안 TPH 가 켜져 있으면 불안정할 수
 * 있다. 그래서 set_ctrl_reg_req_en(PCI_TPH_REQ_DISABLE) 로 먼저 끄고,
 * 성공하면 원래 요청 종류로 되돌린다.
 *
 * 실패했을 때는 되돌리지 않고 pcie_disable_tph() 로 아예 꺼 버린다.
 * ST 표가 어중간한 상태인 채 TPH 가 도는 것보다 끄는 편이 안전하다는
 * 판단이다.
 *
 * 앞의 세 검사에 각각 이유가 있다.
 *   tph_cap 없음   — TPH capability 자체가 없는 장치.
 *   tph_enabled 0  — pcie_enable_tph() 를 아직 부르지 않았다. tph_req_type
 *                    이 정해지지 않아 되돌릴 값도 없다.
 *   NS 모드        — No ST Mode 에서는 ST 를 아예 쓰지 않으므로 할 일이
 *                    없다. 오류가 아니라 0 으로 성공 처리한다.
 *
 * 위치 판정에서 loc 를 FIELD_PREP 으로 도로 밀어 올리는 것은
 * pcie_tph_get_st_table_loc() 이 값을 정규화해 돌려주기 때문이다.
 *
 * EXPORT_SYMBOL 이지만 이 스파스 체크아웃 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (트리 밖 드라이버) → [이 함수]
 *     → set_ctrl_reg_req_en() → pci_msix_write_tph_tag() 또는
 *       write_tag_to_st_table() → set_ctrl_reg_req_en()
 */
int pcie_tph_set_st_entry(struct pci_dev *pdev, unsigned int index, u16 tag)
{
	u32 loc;
	/* [한국어] 쓰기 결과. 0 으로 초기화해 두어 default 갈래를 제외한 경로에서
	 * 초기화되지 않은 채 읽히는 일이 없게 한다 */
	int err = 0;

	/* [한국어] TPH capability 자체가 없는 장치 */
	if (!pdev->tph_cap)
		return -EINVAL;

	/* [한국어] pcie_enable_tph() 를 아직 부르지 않았다. tph_req_type 이 정해지지
	 * 않아 아래에서 되돌릴 값도 없다 */
	if (!pdev->tph_enabled)
		return -EINVAL;

	/* No need to write tag if device is in "No ST Mode" */
	if (pdev->tph_mode == PCI_TPH_ST_NS_MODE)
		return 0;

	/*
	 * Disable TPH before updating ST to avoid potential instability as
	 * cautioned in PCIe r6.2, sec 6.17.3, "ST Modes of Operation"
	 */
	set_ctrl_reg_req_en(pdev, PCI_TPH_REQ_DISABLE);

	loc = pcie_tph_get_st_table_loc(pdev);
	/* Convert loc to match with PCI_TPH_LOC_* */
	loc = FIELD_PREP(PCI_TPH_CAP_LOC_MASK, loc);

	/* [한국어] ST 표가 어디 있느냐에 따라 쓰는 경로가 갈린다 */
	switch (loc) {
	/* [한국어] MSI-X 표 안에 있는 경우 */
	case PCI_TPH_LOC_MSIX:
		err = pci_msix_write_tph_tag(pdev, index, tag);
		break;
	/* [한국어] TPH capability 공간 안에 있는 경우 */
	case PCI_TPH_LOC_CAP:
		err = write_tag_to_st_table(pdev, index, tag);
		break;
	default:
		err = -EINVAL;
	}

	/* [한국어] 쓰기가 실패했다 */
	if (err) {
		pcie_disable_tph(pdev);
		return err;
	}

	/* [한국어] 성공했으니 원래 요청 종류로 되돌려 TPH 를 다시 켠다.
	 * 이 줄과 위의 DISABLE 쓰기가 한 쌍이다 */
	set_ctrl_reg_req_en(pdev, pdev->tph_req_type);

	/* [한국어] 어느 표의 몇 번째에 무슨 태그를 썼는지 디버그 로그로 남긴다 */
	pci_dbg(pdev, "set steering tag: %s table, index=%d, tag=%#04x\n",
		(loc == PCI_TPH_LOC_MSIX) ? "MSI-X" : "ST", index, tag);

	return 0;
}
EXPORT_SYMBOL(pcie_tph_set_st_entry);

/**
 * pcie_disable_tph - Turn off TPH support for device
 * @pdev: PCI device
 *
 * Return: none
 */
/* [한국어]
 * pcie_disable_tph - 이 장치의 TPH 를 끈다
 *
 * @pdev: PCI 장치.  @return: 없음
 *
 * Control 레지스터를 통째로 0 으로 만든다. 읽고-고치고-쓰기가 아니라
 * 그냥 0 을 쓰는 이유는 이 레지스터의 모든 필드(ST Mode Select 와
 * Requester Enable)를 다 끄는 것이 목적이기 때문이다.
 *
 * 그 다음 캐시 세 필드를 0 으로 되돌린다. tph_enabled 를 0 으로 만드는
 * 것이 특히 중요하다 — 그래야 pcie_enable_tph() 가 -EBUSY 없이 다시
 * 켤 수 있고, pcie_tph_set_st_entry() 가 꺼진 장치에 쓰지 않는다.
 *
 * 앞의 두 검사는 "끌 것이 없으면 config 를 건드리지 않는다" 는 방어다.
 * 특히 tph_cap 이 0 인 장치에서 쓰면 config 오프셋 0 근처, 즉 Vendor ID
 * 자리를 건드리게 된다.
 *
 * pcie_tph_set_st_entry() 의 실패 경로에서도 불린다.
 *
 * EXPORT_SYMBOL 이지만 이 스파스 체크아웃 안에는 외부 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (트리 밖 드라이버) 또는 pcie_tph_set_st_entry()의 실패 경로 → [이 함수]
 */
void pcie_disable_tph(struct pci_dev *pdev)
{
	if (!pdev->tph_cap)
		return;

	/* [한국어] 켜져 있지 않으면 끌 것이 없다. tph_cap 검사와 함께, 꺼진 장치의
	 * config 를 건드리지 않게 하는 방어다 */
	if (!pdev->tph_enabled)
		return;

	/* [한국어] Control 을 통째로 0 으로 만든다. 읽고-고치고-쓰기가 아닌 이유는
	 * 이 레지스터의 모든 필드를 다 끄는 것이 목적이기 때문이다 */
	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, 0);

	/* [한국어] 캐시된 모드를 지운다 */
	pdev->tph_mode = 0;
	/* [한국어] 캐시된 요청 종류를 지운다 */
	pdev->tph_req_type = 0;
	/* [한국어] 마지막에 활성 표시를 지운다. 이것이 0 이 되어야 pcie_enable_tph() 가
	 * -EBUSY 없이 다시 켤 수 있다 */
	pdev->tph_enabled = 0;
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
/* [한국어]
 * pcie_enable_tph - ST 모드를 골라 이 장치의 TPH 를 켠다
 *
 * @pdev: PCI 장치.  @mode: 켤 ST 모드(위 원문 kernel-doc 이 세 값을 나열한다)
 * @return: 0 = 성공, -EINVAL = 전역 차단/미지원/모드 불일치/요청 종류 없음,
 *          -EBUSY = 이미 켜져 있음
 *
 * 위 원문 kernel-doc 이 두 가지를 한다고 밝힌다 — 장치가 그 모드를
 * 실제로 지원하는지 확인하고, 장치의 요청자 능력과 Root Port 의 완료자
 * 능력을 함께 보아 어떤 종류의 요청(TPH / 확장 TPH)을 낼 수 있는지 정한다.
 *
 * 절차:
 *   1) "pci=notph" 로 전역 차단되었는지 본다. 코드 주석이 그 인자를 명시한다.
 *   2) capability 유무와 중복 활성화를 거른다. 이미 켜져 있으면 -EBUSY 로
 *      알린다 — 조용히 덮어쓰면 앞서 설정한 ST 표가 무의미해진다.
 *   3) mode 를 마스크로 잘라 낸(sanitize) 뒤, get_st_modes() 가 돌려준
 *      지원 비트와 (1 << mode) 를 AND 해 실제 지원 여부를 확인한다.
 *      그 비교는 세 모드 상수가 모드 번호 자리의 단일 비트라는 배치를
 *      전제한다(그 값은 이 트리에 헤더가 없어 확인하지 못했다).
 *   4) 요청 종류를 정한다. 먼저 장치의 Capability 에서 확장 TPH 지원
 *      여부를 보고, 이어 Root Port 쪽 완료자 능력과 min() 을 취한다.
 *      "작은 쪽" 을 고르는 것이 요점이다 — 양쪽이 다 지원해야 성립하므로,
 *      상수 값이 능력 순서대로 커지도록 정의되어 있음을 전제한 관용구다.
 *      RC Integrated Endpoint(PCI_EXP_TYPE_RC_END)는 위에 Root Port 가
 *      없으므로 이 단계를 건너뛴다.
 *      결과가 PCI_TPH_REQ_DISABLE 이면 실제로 낼 수 있는 요청이 없다는
 *      뜻이라 -EINVAL 이다.
 *   5) Control 레지스터에 모드와 요청 종류를 각각 FIELD_PREP 으로 채워
 *      한 번에 쓴다. 읽고-고치고-쓰기라 다른 비트는 보존된다.
 *   6) tph_enabled 를 세운다. 이 대입이 마지막인 것이 중요하다 —
 *      하드웨어 설정이 끝난 뒤에만 "켜졌다" 로 보이게 한다.
 *
 * 4)의 config 읽기 실패를 확인하지 않는다. 코드는 고치지 않고 사실만 적어 둔다.
 *
 * EXPORT_SYMBOL 이지만 이 스파스 체크아웃 안에는 호출자가 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   (트리 밖 드라이버) → [이 함수]
 *     → get_st_modes() → get_rp_completer_type() → pci_write_config_dword()
 */
int pcie_enable_tph(struct pci_dev *pdev, int mode)
{
	u32 reg;
	/* [한국어] 장치가 지원하는 ST 모드 비트들 */
	u8 dev_modes;
	/* [한국어] Root Port 가 완료자로서 지원하는 요청 종류 */
	u8 rp_req_type;

	/* Honor "notph" kernel parameter */
	if (pci_tph_disabled)
		return -EINVAL;

	/* [한국어] TPH capability 가 없는 장치 */
	if (!pdev->tph_cap)
		return -EINVAL;

	/* [한국어] 이미 켜져 있으면 -EBUSY 로 알린다. 조용히 덮어쓰면 앞서 설정해 둔
	 * ST 표가 무의미해지기 때문이다 */
	if (pdev->tph_enabled)
		return -EBUSY;

	/* Sanitize and check ST mode compatibility */
	mode &= PCI_TPH_CTRL_MODE_SEL_MASK;
	/* [한국어] 장치가 지원하는 모드 비트를 읽어 온다 */
	dev_modes = get_st_modes(pdev);
	/* [한국어] 요청한 모드가 그 안에 있는지 본다. (1 << mode) 와 AND 하는 것은
	 * 세 모드 상수가 모드 번호 자리의 단일 비트라는 배치를 전제한 것이며,
	 * 그 값은 이 트리에 헤더가 없어 확인하지 못했다 */
	if (!((1 << mode) & dev_modes))
		return -EINVAL;

	/* [한국어] 모드를 캐시한다. 아래 Control 쓰기와 pcie_tph_set_st_entry() 의
	 * NS 모드 검사가 이 값을 본다 */
	pdev->tph_mode = mode;

	/* Get req_type supported by device and its Root Port */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CAP, &reg);
	/* [한국어] 장치가 확장 TPH 요청을 낼 수 있는가 */
	if (FIELD_GET(PCI_TPH_CAP_EXT_TPH, reg))
		/* [한국어] 낼 수 있으면 16비트 확장 태그를 쓰는 종류로 시작한다 */
		pdev->tph_req_type = PCI_TPH_REQ_EXT_TPH;
	else
		/* [한국어] 아니면 8비트 태그만 쓰는 종류 */
		pdev->tph_req_type = PCI_TPH_REQ_TPH_ONLY;

	/* Check if the device is behind a Root Port */
	if (pci_pcie_type(pdev) != PCI_EXP_TYPE_RC_END) {
		/* [한국어] Root Complex 통합 엔드포인트가 아니면 위에 Root Port 가 있으므로
		 * 그쪽 완료자 능력도 본다 */
		rp_req_type = get_rp_completer_type(pdev);

		/* Final req_type is the smallest value of two */
		pdev->tph_req_type = min(pdev->tph_req_type, rp_req_type);
	}

	/* [한국어] 장치와 Root Port 능력을 맞춰 본 결과 낼 수 있는 요청이 없다는 뜻이다.
	 * get_rp_completer_type() 이 실패로 0 을 돌려준 경우도 여기로 온다 */
	if (pdev->tph_req_type == PCI_TPH_REQ_DISABLE)
		return -EINVAL;

	/* Write them into TPH control register */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, &reg);

	/* [한국어] ST Mode Select 필드를 지우고 */
	reg &= ~PCI_TPH_CTRL_MODE_SEL_MASK;
	/* [한국어] 고른 모드를 그 자리에 넣는다 */
	reg |= FIELD_PREP(PCI_TPH_CTRL_MODE_SEL_MASK, pdev->tph_mode);

	/* [한국어] Requester Enable 필드를 지우고 */
	reg &= ~PCI_TPH_CTRL_REQ_EN_MASK;
	/* [한국어] 정한 요청 종류를 그 자리에 넣는다. 두 필드를 한 번의 쓰기로 반영한다 */
	reg |= FIELD_PREP(PCI_TPH_CTRL_REQ_EN_MASK, pdev->tph_req_type);

	/* [한국어] 완성된 값을 쓴다. 이 순간 TPH 가 실제로 켜진다 */
	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, reg);

	/* [한국어] 하드웨어 설정이 끝난 뒤에만 "켜졌다" 로 보이게 한다. 이 대입이
	 * 마지막인 이유이며, 다른 함수들이 이 값을 보고 동작 여부를 가른다 */
	pdev->tph_enabled = 1;

	return 0;
}
EXPORT_SYMBOL(pcie_enable_tph);

/* [한국어]
 * pci_restore_tph_state - resume 후 TPH 설정과 ST 표를 되돌린다
 *
 * @pdev: 대상 PCI 장치.  @return: 없음
 *
 * 확인한 유일한 호출자는 drivers/pci/pci.c:3540 의 pci_restore_state()
 * 경로다.
 *
 * 저장 쪽(pci_save_tph_state)의 정확한 거울상이며, 버퍼 배치도 같다 —
 * 맨 앞 u32 하나가 Control 레지스터이고, 그 뒤로 u16 항목들이 ST 표다.
 * cap 포인터를 u32 로 한 칸 전진시킨 뒤 u16 포인터로 캐스팅하는 것이
 * 그 배치를 표현한다.
 *
 * ST 표까지 복원해야 하는 이유가 이 함수의 존재 이유다. 리셋이나 D3
 * 복귀 후 config space 는 기본값으로 돌아가는데, ST 표는 드라이버가
 * pcie_tph_set_st_entry() 로 하나씩 채워 넣은 값이라 커널이 되살려 주지
 * 않으면 힌트가 엉뚱한 곳을 가리키게 된다.
 *
 * 항목 수를 저장 때와 똑같이 pcie_tph_get_st_table_size() 로 다시 구한다.
 * 그 값이 하드웨어에서 오는 것이라 리셋 전후로 달라지지 않는다는 전제다.
 * ST 표가 MSI-X 안에 있으면 그 크기가 0 이라 루프가 돌지 않는다 —
 * 그쪽 표는 MSI-X 복원 경로가 맡는다.
 *
 * 세 검사(tph_cap / tph_enabled / save_state)를 모두 통과해야 진행한다.
 * tph_enabled 를 보는 이유는, 꺼져 있던 장치의 Control 을 되살려 놓으면
 * 드라이버가 모르는 사이에 TPH 가 켜지기 때문이다.
 *
 * 실행 컨텍스트: PM/리셋 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_restore_state() [pci.c:3540] → [이 함수]
 *     → pci_find_saved_ext_cap() → pcie_tph_get_st_table_size()
 */
void pci_restore_tph_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *save_state;
	/* [한국어] num_entries = ST 표 항목 수, i = 반복자, offset = 표 안의 상대 오프셋 */
	int num_entries, i, offset;
	/* [한국어] 저장 버퍼 안의 ST 표 부분을 가리킬 포인터 */
	u16 *st_entry;
	/* [한국어] 저장 버퍼의 시작. u32 인 이유는 첫 칸이 Control 레지스터라서다 */
	u32 *cap;

	/* [한국어] TPH capability 가 없으면 복원할 것도 없다 */
	if (!pdev->tph_cap)
		return;

	/* [한국어] 꺼져 있던 장치의 Control 을 되살리면 드라이버가 모르는 사이에 TPH 가
	 * 켜진다. 그것을 막는 검사다 */
	if (!pdev->tph_enabled)
		return;

	/* [한국어] pci_tph_init() 이 잡아 둔 저장 슬롯을 찾는다 */
	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_TPH);
	/* [한국어] 슬롯이 없으면 저장된 값도 없다. 저장 쪽도 같은 검사로 걸러지므로
	 * 짝이 맞는다 */
	if (!save_state)
		return;

	/* Restore control register and all ST entries */
	cap = &save_state->cap.data[0];
	/* [한국어] 첫 칸의 Control 값을 되돌리고 포인터를 한 칸 전진시킨다 */
	pci_write_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, *cap++);
	/* [한국어] 그 다음 위치부터가 ST 표다. u32 포인터를 u16 로 다시 보는 것이
	 * 그 배치를 표현한다 — 저장 쪽과 한 글자도 어긋나면 안 된다 */
	st_entry = (u16 *)cap;
	/* [한국어] 표는 capability 헤더 바로 뒤에서 시작한다 */
	offset = PCI_TPH_BASE_SIZEOF;
	/* [한국어] 항목 수는 저장 때와 똑같이 하드웨어에서 다시 구한다. 그 값이
	 * 리셋 전후로 달라지지 않는다는 전제다 */
	num_entries = pcie_tph_get_st_table_size(pdev);
	/* [한국어] 항목마다 하나씩 되돌린다. 표가 MSI-X 안에 있으면 크기가 0 이라
	 * 루프가 돌지 않는다 — 그쪽은 MSI-X 복원 경로가 맡는다 */
	for (i = 0; i < num_entries; i++) {
		pci_write_config_word(pdev, pdev->tph_cap + offset,
			      *st_entry++);
		offset += sizeof(u16);
	}
}

/* [한국어]
 * pci_save_tph_state - suspend 전에 TPH 설정과 ST 표를 저장한다
 *
 * @pdev: 대상 PCI 장치.  @return: 없음
 *
 * 확인한 유일한 호출자는 drivers/pci/pci.c:3346 의 pci_save_state()
 * 경로다.
 *
 * 버퍼 배치는 pci_tph_init() 이 잡아 둔 크기와 정확히 맞아야 한다 —
 * sizeof(u32) + 항목수 * sizeof(u16). 앞의 u32 가 Control 레지스터,
 * 뒤가 ST 표다.
 *
 * 복원 쪽과 순서·타입이 한 글자도 어긋나면 안 된다. cap 을 u32 로 한 칸
 * 전진시킨 뒤 그 위치를 u16 포인터로 다시 보는 부분이 특히 그렇다.
 *
 * Status 성격의 값은 저장하지 않는다. Control 과 ST 표만이 "커널이 정한
 * 값" 이고 나머지는 하드웨어가 알려 주는 값이라 되살릴 성질이 아니다.
 *
 * save_state 가 없으면 조용히 돌아선다. pci_tph_init() 에서 버퍼 할당이
 * 실패한 경우인데, 그때는 복원 쪽도 같은 검사로 걸러지므로 짝이 맞는다.
 *
 * 실행 컨텍스트: PM/리셋 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_save_state() [pci.c:3346] → [이 함수]
 *     → pci_find_saved_ext_cap() → pcie_tph_get_st_table_size()
 */
void pci_save_tph_state(struct pci_dev *pdev)
{
	struct pci_cap_saved_state *save_state;
	/* [한국어] 복원 쪽과 같은 지역 변수들 */
	int num_entries, i, offset;
	/* [한국어] 저장 버퍼 안의 ST 표 부분 */
	u16 *st_entry;
	/* [한국어] 저장 버퍼의 시작 */
	u32 *cap;

	/* [한국어] TPH capability 가 없으면 저장할 것도 없다 */
	if (!pdev->tph_cap)
		return;

	/* [한국어] 꺼져 있는 장치는 저장할 설정이 없다. 복원 쪽과 같은 검사라
	 * 저장하지 않은 것을 복원하려 드는 일이 없다 */
	if (!pdev->tph_enabled)
		return;

	/* [한국어] pci_tph_init() 이 잡아 둔 저장 슬롯을 찾는다 */
	save_state = pci_find_saved_ext_cap(pdev, PCI_EXT_CAP_ID_TPH);
	/* [한국어] 슬롯이 없으면(할당 실패) 조용히 돌아선다 */
	if (!save_state)
		return;

	/* Save control register */
	cap = &save_state->cap.data[0];
	/* [한국어] 첫 칸에 Control 값을 저장하고 포인터를 한 칸 전진시킨다 */
	pci_read_config_dword(pdev, pdev->tph_cap + PCI_TPH_CTRL, cap++);

	/* Save all ST entries in extended capability structure */
	st_entry = (u16 *)cap;
	/* [한국어] 표는 capability 헤더 바로 뒤에서 시작한다 */
	offset = PCI_TPH_BASE_SIZEOF;
	/* [한국어] 항목 수를 하드웨어에서 구한다 */
	num_entries = pcie_tph_get_st_table_size(pdev);
	for (i = 0; i < num_entries; i++) {
		/* [한국어] 항목을 하나씩 읽어 버퍼에 담는다 */
		pci_read_config_word(pdev, pdev->tph_cap + offset,
			     st_entry++);
		/* [한국어] 다음 항목으로. 항목 하나가 16비트다 */
		offset += sizeof(u16);
	}
}

/* [한국어]
 * pci_no_tph - TPH 를 커널 전역으로 끈다
 *
 * @return: 없음
 *
 * 확인한 유일한 호출자는 drivers/pci/pci.c:14036 의 pci_setup() 안
 * strncmp(str, "notph", 5) 분기다 — 즉 실제 부팅 인자는 "pci=notph" 다.
 *
 * 전역 플래그 하나를 세우고 알림을 찍는 것이 전부다. 그 값을 보는 곳은
 * pcie_enable_tph() 하나뿐이고, 거기서 곧바로 -EINVAL 이 되어 어떤
 * 드라이버도 TPH 를 켜지 못하게 된다.
 *
 * 이미 켜져 있던 장치를 끄지는 않는다. 부팅 인자 파싱이 어떤 드라이버가
 * probe 되기 전에 일어나므로 그럴 대상이 없기 때문이다.
 *
 * pci_tph_init() 은 이 플래그를 보지 않는다. capability 를 찾고 저장
 * 버퍼를 잡는 일은 TPH 를 켜는 것과 무관해서, 차단 중에도 그대로 돈다.
 * 코드는 고치지 않고 이 관찰만 적어 둔다.
 *
 * 실행 컨텍스트: 부팅 파라미터 파싱, 단일 스레드. 그래서 락이 없다.
 *
 * 호출 체인:
 *   pci_setup()["pci=notph"] [pci.c:14036] → [이 함수]
 */
void pci_no_tph(void)
{
	pci_tph_disabled = true;

	pr_info("PCIe TPH is disabled\n");
}

/* [한국어]
 * pci_tph_init - 열거 때 TPH capability 를 찾고 저장 버퍼를 잡는다
 *
 * @pdev: 갓 열거된 PCI 장치.  @return: 없음
 *
 * 확인한 유일한 호출자는 drivers/pci/probe.c:6446 의
 * pci_init_capabilities() 다 — 커널이 인식하는 모든 PCI 장치가 한 번씩
 * 지나간다.
 *
 * 두 가지만 한다.
 *   1) TPH 확장 capability 오프셋을 찾아 pdev->tph_cap 에 캐시한다.
 *      없으면 여기서 끝 — 이 파일의 다른 함수들이 전부 이 필드가 0 인지
 *      먼저 보고 물러난다.
 *   2) suspend/resume 용 저장 버퍼를 미리 잡는다. 크기는
 *      Control 레지스터 u32 하나에 ST 표 항목(u16)들을 더한 값이다.
 *      ST 표가 MSI-X 안에 있으면 항목 수가 0 이라 u32 하나만 잡힌다.
 *
 * 여기서 TPH 를 켜지는 않는다. 어느 ST 모드를 쓸지는 드라이버가 정할
 * 일이고, ST 값을 채우기 전에 켜 두면 힌트가 엉뚱한 곳을 가리킨다.
 * pcie_enable_tph() 가 그 결정을 드라이버에게 맡기는 이유다.
 *
 * pci_add_ext_cap_save_buffer() 의 실패를 확인하지 않는다. 실패하면
 * 저장/복원 쪽이 버퍼를 못 찾아 조용히 넘어가므로 TPH 자체는 동작한다.
 *
 * 짝이 되는 해제 함수가 이 파일에 없다. 저장 버퍼는 PCI 코어가 장치
 * 해제 때 함께 반납한다.
 *
 * 실행 컨텍스트: 버스 열거 경로의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pci_init_capabilities() [probe.c:6446] → [이 함수]
 *     → pci_find_ext_capability() → pcie_tph_get_st_table_size()
 *     → pci_add_ext_cap_save_buffer()
 */
void pci_tph_init(struct pci_dev *pdev)
{
	int num_entries;
	/* [한국어] 저장 버퍼에 필요한 바이트 수 */
	u32 save_size;

	/* [한국어] TPH 확장 capability 오프셋을 찾아 캐시한다. 이 파일의 다른 함수들이
	 * 전부 이 값이 0 인지 먼저 보고 물러난다 */
	pdev->tph_cap = pci_find_ext_capability(pdev, PCI_EXT_CAP_ID_TPH);
	/* [한국어] TPH 를 지원하지 않는 장치. 여기서 끝 */
	if (!pdev->tph_cap)
		return;

	/* [한국어] ST 표가 capability 안에 있으면 그 항목 수, MSI-X 안에 있으면 0 */
	num_entries = pcie_tph_get_st_table_size(pdev);
	/* [한국어] Control 레지스터 u32 하나에 ST 표 항목(u16)들을 더한 크기.
	 * 저장/복원 두 함수가 이 배치를 그대로 전제한다 */
	save_size = sizeof(u32) + num_entries * sizeof(u16);
	/* [한국어] suspend/resume 용 버퍼를 미리 잡는다. 실패를 확인하지 않는데,
	 * 실패하면 저장/복원 쪽이 버퍼를 못 찾아 조용히 넘어가므로
	 * TPH 자체는 그대로 동작한다 */
	pci_add_ext_cap_save_buffer(pdev, PCI_EXT_CAP_ID_TPH, save_size);
}
