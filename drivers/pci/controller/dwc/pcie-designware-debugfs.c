// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare PCIe controller debugfs driver
 *
 * Copyright (C) 2025 Samsung Electronics Co., Ltd.
 *		 http://www.samsung.com
 *
 * Author: Shradha Todi <shradha.t@samsung.com>
 */

#include <linux/debugfs.h> /* PCI/NVMe: debugfs 헤더; NVMe 열거/바인딩 시 사용하는
			    *  DWC RC 아래에 연결된 NVMe SSD의 PCIe link 상태를
			    *  런타임에 관찰/제어하기 위한 진입점 */

#include "pcie-designware.h" /* PCI/NVMe: DesignWare PCIe host/EP 공용 구조체 및
			      *  DBI(Database) 접근 API 포함; NVMe SSD가 RC 뒤에
			      *  매핑될 때 사용하는 dw_pcie 객체 정의 */

/* PCI/NVMe: RAS DES(Data Exchange) capability 내 Status/Debug 레지스터 오프셋.
 *  NVMe SSD의 PCIe link 물리 레인(Lane) 감지 상태를 확인할 때 사용. */
#define SD_STATUS_L1LANE_REG		0xb0
/* PCI/NVMe: PIPE RX valid 상태 비트; NVMe 장치로부터의 물리 계층 수신 신호
 *  유효성을 나타냄. RX Invalid 시 NVMe TLP 수신 장애 원인이 될 수 있음. */
#define PIPE_RXVALID			BIT(18)
/* PCI/NVMe: Lane detect 상태 비트; NVMe SSD가 연결된 특정 lane의 감지 여부. */
#define PIPE_DETECT_LANE		BIT(17)
/* PCI/NVMe: 감시할 물리 lane 번호 선택 필드(0~15); NVMe PCIe lane 품질
 *  디버깅 시 대상 lane을 지정. */
#define LANE_SELECT			GENMASK(3, 0)

/* PCI/NVMe: Error Injection 레지스터 그룹 기준 오프셋; AER(Advanced Error
 *  Reporting) 이벤트를 소프트웨어로 주입해 NVMe controller의 오류 복구 동작을
 *  검증할 때 사용. */
#define ERR_INJ0_OFF			0x34
/* PCI/NVMe: Error Injection group 4에서 사용하는 값 차이(value difference)
 *  필드; NVMe TLP 데이터/헤더 주입 시 변조폭 지정. */
#define EINJ_VAL_DIFF			GENMASK(28, 16)
/* PCI/NVMe: Error Injection group 4의 Virtual Channel 선택 필드; NVMe가
 *  사용하는 VC(일반적으로 VC0) 이외 VC 오류 주입 가능. */
#define EINJ_VC_NUM			GENMASK(14, 12)
/* PCI/NVMe: Error Injection type 필드의 비트 시프트 값; 그룹별 type 위치
 *  통일을 위해 사용. */
#define EINJ_TYPE_SHIFT			8
/* PCI/NVMe: Group 0 error injection type 비트마스크(11:8); LCRC/ECRC 등
 *  NVMe TLP 무결성 오류 모델링. */
#define EINJ0_TYPE			GENMASK(11, 8)
/* PCI/NVMe: Group 1 error injection type 비트마스크(8); ACK/NAK DLLP 오류. */
#define EINJ1_TYPE			BIT(8)
/* PCI/NVMe: Group 2 error injection type 비트마스크(9:8); DLLP 관련 오류. */
#define EINJ2_TYPE			GENMASK(9, 8)
/* PCI/NVMe: Group 3 error injection type 비트마스크(10:8); 물리 계층
 *  символ/정렬 오류. */
#define EINJ3_TYPE			GENMASK(10, 8)
/* PCI/NVMe: Group 4 error injection type 비트마스크(10:8); TLP 헤더/데이터
 *  오류. */
#define EINJ4_TYPE			GENMASK(10, 8)
/* PCI/NVMe: Group 5 error injection type 비트마스크(8); 중복/무효화 TLP. */
#define EINJ5_TYPE			BIT(8)
/* PCI/NVMe: Error Injection 반복 횟수 필드(7:0); NVMe I/O 동작 중 특정
 *  PCIe 오류를 몇 회 발생시킬지 지정. */
#define EINJ_COUNT			GENMASK(7, 0)

/* PCI/NVMe: Error Injection enable 레지스터 오프셋; 그룹별 오류 주입을
 *  실제로 활성화. */
#define ERR_INJ_ENABLE_REG		0x30

/* PCI/NVMe: RAS DES Event Counter 데이터 레지스터 오프셋; NVMe link 품질
 *  및 성능 지표(재전송, DLLP 수, ASPM 전환 횟수 등)를 읽음. */
#define RAS_DES_EVENT_COUNTER_DATA_REG	0xc

/* PCI/NVMe: RAS DES Event Counter 제어 레지스터 오프셋; 측정할 이벤트
 *  그룹/이벤트/lane을 선택하고 카운터를 on/off. */
#define RAS_DES_EVENT_COUNTER_CTRL_REG	0x8
/* PCI/NVMe: Event Counter 그룹 선택 필드(27:24); NVMe PCIe 물리/데이터
 *  링크 계층 이벤트 분류. */
#define EVENT_COUNTER_GROUP_SELECT	GENMASK(27, 24)
/* PCI/NVMe: Event Counter 이벤트 선택 필드(23:16); 그룹 내 특정 이벤트
 *  (예: bad_tlp, replay_timeout) 선택. */
#define EVENT_COUNTER_EVENT_SELECT	GENMASK(23, 16)
/* PCI/NVMe: Event Counter lane 선택 필드(11:8); NVMe SSD가 사용하는
 *  특정 lane의 이벤트 측정. */
#define EVENT_COUNTER_LANE_SELECT	GENMASK(11, 8)
/* PCI/NVMe: Counter enable 상태 비트(7); 카운터가 실제 동작 중인지 확인. */
#define EVENT_COUNTER_STATUS		BIT(7)
/* PCI/NVMe: Counter enable 제어 필드(4:2); PER_EVENT_ON/PER_EVENT_OFF
 *  값으로 측정 시작/정지. */
#define EVENT_COUNTER_ENABLE		GENMASK(4, 2)
/* PCI/NVMe: Event Counter 측정 활성화 값; NVMe link 이벤트 누적 시작. */
#define PER_EVENT_ON			0x3
/* PCI/NVMe: Event Counter 측정 비활성화 값; 누적 중지. */
#define PER_EVENT_OFF			0x1

/* PCI/NVMe: debugfs read/write 시 사용자 공간에 복사할 버퍼 최대 크기;
 *  한 줄 상태 문자열 출력용. */
#define DWC_DEBUGFS_BUF_MAX		128

/**
 * struct dwc_pcie_rasdes_info - Stores controller common information
 * @ras_cap_offset: RAS DES vendor specific extended capability offset
 * @reg_event_lock: Mutex used for RAS DES shadow event registers
 *
 * Any parameter constant to all files of the debugfs hierarchy for a single
 * controller will be stored in this struct. It is allocated and assigned to
 * controller specific struct dw_pcie during initialization.
 */
struct dwc_pcie_rasdes_info {
	u32 ras_cap_offset;	/* NVMe: DWC RC의 PCIe configuration space에서
				 *  RAS DES extended capability 시작 오프셋;
				 *  NVMe SSD가 RC 아래로 열거될 때 이 capability를
				 *  통해 AER 이벤트/카운터에 접근. */
	struct mutex reg_event_lock;	/* NVMe: event counter 제어/데이터
					 *  레지스터 접근 시 동기화; NVMe I/O
					 *  중 concurrent debugfs 접근으로 인한
					 *  레지스터 shadow 상태 손상 방지. */
};

/**
 * struct dwc_pcie_rasdes_priv - Stores file specific private data information
 * @pci: Reference to the dw_pcie structure
 * @idx: Index of specific file related information in array of structs
 *
 * All debugfs files will have this struct as its private data.
 */
struct dwc_pcie_rasdes_priv {
	struct dw_pcie *pci;	/* NVMe: 이 debugfs 파일이 속한 DWC PCIe host
				 *  controller; NVMe SSD의 parent bus를
				 *  생성하는 RC 객체. */
	int idx;		/* NVMe: err_inj_list[] 또는 event_list[]에서
				 *  이 파일이 대표하는 오류/이벤트 인덱스. */
};

/**
 * struct dwc_pcie_err_inj - Store details about each error injection
 *			     supported by DWC RAS DES
 * @name: Name of the error that can be injected
 * @err_inj_group: Group number to which the error belongs. The value
 *		   can range from 0 to 5
 * @err_inj_type: Each group can have multiple types of error
 */
struct dwc_pcie_err_inj {
	const char *name;	/* NVMe: 주입 가능한 PCIe 오류 이름; NVMe host
				 *  driver의 AER 핸들러가 기대하는 오류 유형과
				 *  매핑됨(예: ecrc_error, completion_timeout). */
	u32 err_inj_group;	/* NVMe: 오류가 속한 DWC RAS DES 그룹 번호(0~5);
				 *  레지스터 오프셋 계산에 사용. */
	u32 err_inj_type;	/* NVMe: 그룹 내 구체적 오류 유형; TLP/DLLP/물리
				 *  계층 오류를 NVMe traffic에 맞게 선택. */
};

/* PCI/NVMe: DWC RAS DES가 지원하는 PCIe error injection 목록.
 *  NVMe SSD와의 PCIe link에서 LCRC/ECRC/DLLP/TLP 등 다양한 계층의 오류를
 *  의도적으로 발생시켜, drivers/nvme/host/pci.c의 AER 복구 경로와 연동
 *  검증할 수 있음. */
static const struct dwc_pcie_err_inj err_inj_list[] = {
	{"tx_lcrc", 0x0, 0x0},		/* NVMe: 송신 LCRC 오류; NVMe 쓰기
					 *  TLP 송신 시 CRC 오류 유발. */
	{"b16_crc_dllp", 0x0, 0x1},	/* NVMe: 16GT/s DLLP CRC 오류; Gen4 이상
					 *  NVMe SSD link에서 데이터 링크 계층
					 *  무결성 문제 시뮬레이션. */
	{"b16_crc_upd_fc", 0x0, 0x2},	/* NVMe: UpdateFC DLLP CRC 오류; Flow
					 *  Control 손상 시 NVMe DMA 성능 저하
					 *  모델링. */
	{"tx_ecrc", 0x0, 0x3},		/* NVMe: 송신 ECRC 오류; End-to-End
					 *  데이터 무결성 검증 실패 유발. */
	{"fcrc_tlp", 0x0, 0x4},		/* NVMe: TLP FCRC 오류; NVMe Admin/I/O
					 *  queue doorbell/PRP 전송 오류. */
	{"parity_tsos", 0x0, 0x5},	/* NVMe: TS OS parity 오류; 물리 계층
					 *  training 단계 문제. */
	{"parity_skpos", 0x0, 0x6},	/* NVMe: SKP OS parity 오류; clock
					 *  compensation 시 오류. */
	{"rx_lcrc", 0x0, 0x8},		/* NVMe: 수신 LCRC 오류; NVMe SSD로부터
					 *  수신한 TLP의 CRC 불일치. */
	{"rx_ecrc", 0x0, 0xb},		/* NVMe: 수신 ECRC 오류; NVMe completion
					 *  TLP의 end-to-end CRC 실패. */
	{"tlp_err_seq", 0x1, 0x0},	/* NVMe: TLP sequence number 오류;
					 *  replay buffer 관련 NVMe I/O 지연/재전송. */
	{"ack_nak_dllp_seq", 0x1, 0x1},	/* NVMe: ACK/NAK DLLP sequence 오류;
					 *  데이터 링크 계층 재전송 메커니즘
					 *  검증. */
	{"ack_nak_dllp", 0x2, 0x0},	/* NVMe: ACK/NAK DLLP 송신 오류. */
	{"upd_fc_dllp", 0x2, 0x1},	/* NVMe: UpdateFC DLLP 송신 오류; NVMe
					 *  queue credits 관리 이상. */
	{"nak_dllp", 0x2, 0x2},		/* NVMe: NAK DLLP 오류; 수신 TLP 거부. */
	{"inv_sync_hdr_sym", 0x3, 0x0},	/* NVMe: 물리 계층 sync header symbol
					 *  오류; lane deskew 실패. */
	{"com_pad_ts1", 0x3, 0x1},	/* NVMe: TS1 ordered set COM/PAD 오류;
					 *  link training 협상 실패. */
	{"com_pad_ts2", 0x3, 0x2},	/* NVMe: TS2 ordered set COM/PAD 오류. */
	{"com_fts", 0x3, 0x3},		/* NVMe: FTS ordered set 오류; L0s
					 *  exit 시 NVMe DMA 지연 가능. */
	{"com_idl", 0x3, 0x4},		/* NVMe: IDLE ordered set 오류; ASPM
					 *  power management 상태 전환 이상. */
	{"end_edb", 0x3, 0x5},		/* NVMe: END/EDB symbol 오류; TLP
					 *  framing 경계 문제. */
	{"stp_sdp", 0x3, 0x6},		/* NVMe: STP/SDP symbol 오류; TLP/DLLP
					 *  시작 구분자 손상. */
	{"com_skp", 0x3, 0x7},		/* NVMe: SKP ordered set 오류; lane
					 *  clock 차이 보정 실패. */
	{"posted_tlp_hdr", 0x4, 0x0},	/* NVMe: Posted TLP 헤더 오류; NVMe
					 *  메모리 쓰기(MWr) 요청 헤더 변조. */
	{"non_post_tlp_hdr", 0x4, 0x1},	/* NVMe: Non-posted TLP 헤더 오류; NVMe
					 *  메모리 읽기(MRd) 또는 구성 읽기/쓰기
					 *  요청 헤더 변조. */
	{"cmpl_tlp_hdr", 0x4, 0x2},	/* NVMe: Completion TLP 헤더 오류; NVMe
					 *  SSD의 completion 응답 헤더 손상. */
	{"posted_tlp_data", 0x4, 0x4},	/* NVMe: Posted TLP 데이터 페이로드
					 *  오류; NVMe 쓰기 데이터 무결성 훼손. */
	{"non_post_tlp_data", 0x4, 0x5},/* NVMe: Non-posted TLP 데이터 오류. */
	{"cmpl_tlp_data", 0x4, 0x6},	/* NVMe: Completion TLP 데이터 오류;
					 *  NVMe read completion 데이터 손상. */
	{"duplicate_tlp", 0x5, 0x0},	/* NVMe: 중복 TLP 수신; replay
					 *  메커니즘 검증. */
	{"nullified_tlp", 0x5, 0x1},	/* NVMe: 무효화된 TLP; ECRC 실패 등으로
					 *  처리된 TLP. */
};

/* PCI/NVMe: 그룹별 error injection type 필드 위치/폭; 레지스터에 type값을
 *  안전하게 채우기 위한 마스크. */
static const u32 err_inj_type_mask[] = {
	EINJ0_TYPE,	/* NVMe: group 0 type mask (bits 11:8). */
	EINJ1_TYPE,	/* NVMe: group 1 type mask (bit 8). */
	EINJ2_TYPE,	/* NVMe: group 2 type mask (bits 9:8). */
	EINJ3_TYPE,	/* NVMe: group 3 type mask (bits 10:8). */
	EINJ4_TYPE,	/* NVMe: group 4 type mask (bits 10:8). */
	EINJ5_TYPE,	/* NVMe: group 5 type mask (bit 8). */
};

/**
 * struct dwc_pcie_event_counter - Store details about each event counter
 *				   supported in DWC RAS DES
 * @name: Name of the error counter
 * @group_no: Group number that the event belongs to. The value can range
 *	    from 0 to 7
 * @event_no: Event number of the particular event. The value ranges are:
 *		Group 0: 0 - 10
 *		Group 1: 5 - 13
 *		Group 2: 0 - 7
 *		Group 3: 0 - 5
 *		Group 4: 0 - 1
 *		Group 5: 0 - 13
 *		Group 6: 0 - 6
 *		Group 7: 0 - 25
 */
struct dwc_pcie_event_counter {
	const char *name;	/* NVMe: 측정 가능한 PCIe link/transaction
				 *  이벤트 이름; NVMe 성능/안정성 디버깅에
				 *  사용. */
	u32 group_no;		/* NVMe: 이벤트 그룹 번호(0~7); 제어 레지스터의
				 *  EVENT_COUNTER_GROUP_SELECT 필드 값. */
	u32 event_no;		/* NVMe: 그룹 내 이벤트 번호; 제어 레지스터의
				 *  EVENT_COUNTER_EVENT_SELECT 필드 값. */
};

/* PCI/NVMe: DWC RAS DES Event Counter 목록.
 *  NVMe SSD와의 PCIe link에서 발생하는 물리/데이터/트랜잭션 계층 이벤트를
 *  카운트하여, NVMe I/O 성능 저하(link 재전송, FC timeout, completion
 *  timeout 등)의 근본 원인을 분석. */
static const struct dwc_pcie_event_counter event_list[] = {
	{"ebuf_overflow", 0x0, 0x0},	/* NVMe: elastic buffer overflow; lane
					 *  clock 차이로 인한 데이터 손실. */
	{"ebuf_underrun", 0x0, 0x1},	/* NVMe: elastic buffer underrun. */
	{"decode_err", 0x0, 0x2},	/* NVMe: 8b/10b 또는 128b/130b decode
					 *  오류; NVMe TLP/DLLP 비트 오류. */
	{"running_disparity_err", 0x0, 0x3},/* NVMe: running disparity 오류;
					 *  물리 계층 신호 불일치. */
	{"skp_os_parity_err", 0x0, 0x4},/* NVMe: SKP ordered set parity 오류. */
	{"sync_header_err", 0x0, 0x5},	/* NVMe: sync header 오류; block
					 *  alignment 실패. */
	{"rx_valid_deassertion", 0x0, 0x6},/* NVMe: RX valid 비트 해제; NVMe
					 *  SSD로부터 수신 신호 끊김 징후. */
	{"ctl_skp_os_parity_err", 0x0, 0x7},/* NVMe: control SKP OS parity
					 *  오류. */
	{"retimer_parity_err_1st", 0x0, 0x8},/* NVMe: 1st retimer parity
					 *  오류; 신호 재생 장치 문제. */
	{"retimer_parity_err_2nd", 0x0, 0x9},/* NVMe: 2nd retimer parity
					 *  오류. */
	{"margin_crc_parity_err", 0x0, 0xA},/* NVMe: margin CRC parity 오류. */
	{"detect_ei_infer", 0x1, 0x5},	/* NVMe: Electrical Idle inference
					 *  감지; link 상태 변화. */
	{"receiver_err", 0x1, 0x6},	/* NVMe: receiver 오류; NVMe 장치
					 *  수신기 문제. */
	{"rx_recovery_req", 0x1, 0x7},	/* NVMe: RX recovery 요청; link
					 *  retrain으로 NVMe DMA 지연. */
	{"n_fts_timeout", 0x1, 0x8},	/* NVMe: N_FTS timeout; L0s exit
					 *  실패로 NVMe 성능 저하. */
	{"framing_err", 0x1, 0x9},	/* NVMe: TLP/DLLP framing 오류. */
	{"deskew_err", 0x1, 0xa},	/* NVMe: multi-lane deskew 오류; NVMe
					 *  PCIe x4/x8 등 wide link에서 문제. */
	{"framing_err_in_l0", 0x1, 0xc},	/* NVMe: L0 상태에서 framing
					 *  오류; 정상 동작 중 오류. */
	{"deskew_uncompleted_err", 0x1, 0xd},/* NVMe: deskew 완료 실패. */
	{"bad_tlp", 0x2, 0x0},		/* NVMe: 잘못된 TLP 수신; NVMe SSD가
					 *  본 TLP LCRC/LENGTH 문제. */
	{"lcrc_err", 0x2, 0x1},		/* NVMe: LCRC 오류 발생 횟수. */
	{"bad_dllp", 0x2, 0x2},		/* NVMe: 잘못된 DLLP 수신. */
	{"replay_num_rollover", 0x2, 0x3},/* NVMe: replay number rollover;
					 *  데이터 링크 계층 재전송 한계 초과로
					 *  NVMe I/O 중단 가능. */
	{"replay_timeout", 0x2, 0x4},	/* NVMe: replay timeout; NVMe 요청에
					 *  대한 ACK 수신 실패. */
	{"rx_nak_dllp", 0x2, 0x5},	/* NVMe: 수신 NAK DLLP; 상대(NVMe
					 *  SSD)가 TLP 거부. */
	{"tx_nak_dllp", 0x2, 0x6},	/* NVMe: 송신 NAK DLLP; RC가 NVMe SSD
					 *  TLP 거부. */
	{"retry_tlp", 0x2, 0x7},	/* NVMe: 재시도 TLP 횟수; NVMe 링크
					 *  재전송 빈도 측정. */
	{"fc_timeout", 0x3, 0x0},	/* NVMe: Flow Control timeout; NVMe
					 *  queue credit 획득 지연. */
	{"poisoned_tlp", 0x3, 0x1},	/* NVMe: poisoned TLP; NVMe SSD/RC가
					 *  오류 표시한 TLP. */
	{"ecrc_error", 0x3, 0x2},	/* NVMe: ECRC 오류 횟수; NVMe 데이터
					 *  무결성. */
	{"unsupported_request", 0x3, 0x3},/* NVMe: UR(Unsupported Request); 잘못된
					 *  NVMe register 접근 시. */
	{"completer_abort", 0x3, 0x4},	/* NVMe: Completer Abort; NVMe SSD가
					 *  요청 완료 거부. */
	{"completion_timeout", 0x3, 0x5},	/* NVMe: Completion timeout;
					 *  NVMe host가 SSD 응답 대기 초과. */
	{"ebuf_skp_add", 0x4, 0x0},	/* NVMe: elastic buffer SKP 추가. */
	{"ebuf_skp_del", 0x4, 0x1},	/* NVMe: elastic buffer SKP 삭제. */
	{"l0_to_recovery_entry", 0x5, 0x0},/* NVMe: L0 -> Recovery 전환; link
					 *  retrain으로 NVMe DMA 일시 중단. */
	{"l1_to_recovery_entry", 0x5, 0x1},/* NVMe: L1 -> Recovery 전환. */
	{"tx_l0s_entry", 0x5, 0x2},	/* NVMe: 송신 L0s 진입; NVMe PCIe ASPM
					 *  전력 절약. */
	{"rx_l0s_entry", 0x5, 0x3},	/* NVMe: 수신 L0s 진입. */
	{"aspm_l1_reject", 0x5, 0x4},	/* NVMe: ASPM L1 진입 거부; NVMe SSD
					 *  ASPM 협상 문제. */
	{"l1_entry", 0x5, 0x5},		/* NVMe: ASPM L1 진입 횟수. */
	{"l1_cpm", 0x5, 0x6},		/* NVMe: L1 Clock PM 관련 이벤트. */
	{"l1.1_entry", 0x5, 0x7},	/* NVMe: ASPM L1.1 진입; deeper power
					 *  saving 상태. */
	{"l1.2_entry", 0x5, 0x8},	/* NVMe: ASPM L1.2 진입; exit latency가
					 *  NVMe I/O 지연에 영향. */
	{"l1_short_duration", 0x5, 0x9},/* NVMe: 짧은 L1 체류; 전력 대비
					 *  성능 영향 분석. */
	{"l1.2_abort", 0x5, 0xa},	/* NVMe: L1.2 진입 중단; NVMe 요청으로
					 *  인한 깨어남. */
	{"l2_entry", 0x5, 0xb},		/* NVMe: L2(Link-off) 진입; NVMe
					 *  hotplug/removal 시나리오. */
	{"speed_change", 0x5, 0xc},	/* NVMe: PCIe link speed 변경; Gen
					 *  down-grade로 NVMe 대역폭 감소. */
	{"link_width_change", 0x5, 0xd},/* NVMe: PCIe link width 변경; lane
					 *  감소로 NVMe 성능 저하. */
	{"tx_ack_dllp", 0x6, 0x0},	/* NVMe: 송신 ACK DLLP 횟수. */
	{"tx_update_fc_dllp", 0x6, 0x1},/* NVMe: 송신 UpdateFC DLLP 횟수;
					 *  NVMe queue credit 업데이트. */
	{"rx_ack_dllp", 0x6, 0x2},	/* NVMe: 수신 ACK DLLP 횟수. */
	{"rx_update_fc_dllp", 0x6, 0x3},/* NVMe: 수신 UpdateFC DLLP 횟수. */
	{"rx_nullified_tlp", 0x6, 0x4},/* NVMe: 수신 무효화 TLP. */
	{"tx_nullified_tlp", 0x6, 0x5},/* NVMe: 송신 무효화 TLP. */
	{"rx_duplicate_tlp", 0x6, 0x6},/* NVMe: 수신 중복 TLP. */
	{"tx_memory_write", 0x7, 0x0},	/* NVMe: 송신 Memory Write TLP; NVMe
					 *  host -> SSD 쓰기 요청. */
	{"tx_memory_read", 0x7, 0x1},	/* NVMe: 송신 Memory Read TLP. */
	{"tx_configuration_write", 0x7, 0x2},/* NVMe: 송신 Config Write TLP;
					 *  NVMe SSD BAR/CSR 설정. */
	{"tx_configuration_read", 0x7, 0x3},/* NVMe: 송신 Config Read TLP. */
	{"tx_io_write", 0x7, 0x4},	/* NVMe: 송신 I/O Write TLP. */
	{"tx_io_read", 0x7, 0x5},	/* NVMe: 송신 I/O Read TLP. */
	{"tx_completion_without_data", 0x7, 0x6},/* NVMe: 송신 data-less
					 *  completion; NVMe Admin 명령 완료. */
	{"tx_completion_w_data", 0x7, 0x7},/* NVMe: 송신 data completion. */
	{"tx_message_tlp_pcie_vc_only", 0x7, 0x8},/* NVMe: Message TLP
					 *  송신; PME/AER 등. */
	{"tx_atomic", 0x7, 0x9},	/* NVMe: Atomic TLP 송신. */
	{"tx_tlp_with_prefix", 0x7, 0xa},/* NVMe: prefix가 포함된 TLP 송신. */
	{"rx_memory_write", 0x7, 0xb},	/* NVMe: 수신 Memory Write TLP; NVMe
					 *  SSD -> host DMA 쓰기. */
	{"rx_memory_read", 0x7, 0xc},	/* NVMe: 수신 Memory Read TLP. */
	{"rx_configuration_write", 0x7, 0xd},/* NVMe: 수신 Config Write TLP. */
	{"rx_configuration_read", 0x7, 0xe},/* NVMe: 수신 Config Read TLP. */
	{"rx_io_write", 0x7, 0xf},	/* NVMe: 수신 I/O Write TLP. */
	{"rx_io_read", 0x7, 0x10},	/* NVMe: 수신 I/O Read TLP. */
	{"rx_completion_without_data", 0x7, 0x11},/* NVMe: 수신 data-less
					 *  completion; NVMe queue doorbell
					 *  응답 등. */
	{"rx_completion_w_data", 0x7, 0x12},/* NVMe: 수신 data completion;
					 *  NVMe read completion 데이터. */
	{"rx_message_tlp_pcie_vc_only", 0x7, 0x13},/* NVMe: 수신 Message TLP;
					 *  AER/PME/MSI/MSI-X 등. */
	{"rx_atomic", 0x7, 0x14},	/* NVMe: 수신 Atomic TLP. */
	{"rx_tlp_with_prefix", 0x7, 0x15},/* NVMe: prefix가 포함된 TLP 수신. */
	{"tx_ccix_tlp", 0x7, 0x16},	/* NVMe: CCIX TLP 송신. */
	{"rx_ccix_tlp", 0x7, 0x17},	/* NVMe: CCIX TLP 수신. */
	{"tx_deferrable_memory_write_tlp", 0x7, 0x18},/* NVMe: Deferrable
					 *  Memory Write TLP 송신. */
	{"rx_deferrable_memory_write_tlp", 0x7, 0x19},/* NVMe: Deferrable
					 *  Memory Write TLP 수신. */
};

/* PCI/NVMe: debugfs 'lane_detect' 파일 읽기 콜백.
 *  사용자가 cat lane_detect를 실행하면, 현재 선택된 lane에 대해 NVMe SSD의
 *  PCIe 물리 lane 감지 여부를 반환. */
static ssize_t lane_detect_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;	/* NVMe: 이 debugfs 파일이
							 *  속한 DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability 공통
							 *  정보. */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];	/* NVMe: 사용자 공간으로 복사할
						 *  상태 문자열 버퍼. */
	ssize_t pos;	/* NVMe: 출력 문자열 길이. */
	u32 val;	/* NVMe: DBI에서 읽은 레지스터 값. */

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG);
					/* NVMe: RAS DES Status/Debug 레지스터
					 *  읽기; lane 상태 획득. */
	val = FIELD_GET(PIPE_DETECT_LANE, val);
					/* NVMe: PIPE_DETECT_LANE 비트 추출. */
	if (val)			/* NVMe: lane이 감지되면 NVMe SSD가 해당
					 *  물리 lane에서 link training을
					 *  마쳤음을 의미. */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Lane Detected\n");
	else				/* NVMe: lane 미감지; NVMe 장치 미연결,
					 *  link down, 또는 하드웨어 결함. */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Lane Undetected\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
					/* NVMe: 사용자 버퍼로 상태 문자열 복사. */
}

/* PCI/NVMe: debugfs 'lane_detect' 파일 쓰기 콜백.
 *  사용자가 lane 번호를 쓰면 NVMe SSD link 모니터링 대상 lane을 변경. */
static ssize_t lane_detect_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability 정보. */
	u32 lane, val;	/* NVMe: 사용자 입력 lane 번호와 레지스터 값. */
	int ret;	/* NVMe: kstrtou32_from_user 반환값. */

	ret = kstrtou32_from_user(buf, count, 0, &lane);
					/* NVMe: 사용자 입력을 u32 lane 번호로
					 *  변환. */
	if (ret)			/* NVMe: 변환 실패 시 errno 즉시 반환. */
		return ret;

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG);
					/* NVMe: 현재 lane 상태 레지스터 읽기. */
	val &= ~(LANE_SELECT);		/* NVMe: 기존 lane 선택 필드 클리어. */
	val |= FIELD_PREP(LANE_SELECT, lane);
					/* NVMe: 새 lane 번호 설정. */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG, val);
					/* NVMe: 설정값을 DBI에 기록; 이후
					 *  lane_detect_read는 지정 lane을
					 *  모니터링. */

	return count;			/* NVMe: 쓰기 성공 시 입력 바이트 수 반환. */
}

/* PCI/NVMe: debugfs 'rx_valid' 파일 읽기 콜백.
 *  현재 선택된 lane의 PIPE RX valid 상태를 반환; RX Invalid는 NVMe SSD
 *  TLP/DLLP 수신 실패의 직접적 원인. */
static ssize_t rx_valid_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability 정보. */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];	/* NVMe: 출력 버퍼. */
	ssize_t pos;	/* NVMe: 출력 길이. */
	u32 val;	/* NVMe: 레지스터 값. */

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG);
					/* NVMe: status 레지스터 읽기. */
	val = FIELD_GET(PIPE_RXVALID, val);	/* NVMe: PIPE_RXVALID 비트 추출. */
	if (val)			/* NVMe: RX valid; NVMe SSD로부터의 물리
					 *  계층 신호 정상. */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "RX Valid\n");
	else				/* NVMe: RX invalid; lane 품질 문제로
					 *  NVMe data corruption/link down 가능. */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "RX Invalid\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
					/* NVMe: 사용자 공간으로 복사. */
}

/* PCI/NVMe: debugfs 'rx_valid' 파일 쓰기 콜백.
 *  lane_detect_write와 동일; rx_valid 측정 대상 lane을 변경. */
static ssize_t rx_valid_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	return lane_detect_write(file, buf, count, ppos);
					/* NVMe: lane 선택 레지스터 공유로
					 *  동일 함수 재사용. */
}

/* PCI/NVMe: debugfs error injection 파일 쓰기 콜백.
 *  사용자가 특정 오류 파일에 파라미터를 쓰면, DWC RAS DES 하드웨어에 PCIe
 *  오류를 주입하여 NVMe SSD의 AER 복구 경로를 테스트. */
static ssize_t err_inj_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;	/* NVMe: 파일
							 *  private data; 어떤
							 *  오류인지 idx 포함. */
	struct dw_pcie *pci = pdata->pci;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability. */
	u32 val, counter, vc_num, err_group, type_mask;
					/* NVMe: 레지스터 값/주입 횟수/VC/그룹/
					 *  type mask. */
	int val_diff = 0;		/* NVMe: group 1/4에서 사용하는 값 차이;
					 *  기본 0. */
	char *kern_buf;			/* NVMe: 사용자 입력 커널 버퍼. */

	err_group = err_inj_list[pdata->idx].err_inj_group;
					/* NVMe: 현재 파일에 해당하는 오류 그룹
					 *  결정. */
	type_mask = err_inj_type_mask[err_group];
					/* NVMe: 그룹별 type 필드 마스크 선택. */

	kern_buf = memdup_user_nul(buf, count);
					/* NVMe: 사용자 입력을 null-terminated
					 *  커널 버퍼로 복사. */
	if (IS_ERR(kern_buf))		/* NVMe: 복사 실패 시 errno 반환. */
		return PTR_ERR(kern_buf);

	if (err_group == 4) {		/* NVMe: group 4는 TLP 헤더/데이터 오류로
					 *  counter, val_diff, vc_num 3개
					 *  인자 필요. */
		val = sscanf(kern_buf, "%u %d %u", &counter, &val_diff, &vc_num);
		if ((val != 3) || (val_diff < -4095 || val_diff > 4095)) {
			/* NVMe: 인자 부족 또는 val_diff 범위 초과 시
			 *  입력 거부; 잘못된 NVMe TLP 변조 방지. */
			kfree(kern_buf);
			return -EINVAL;
		}
	} else if (err_group == 1) {	/* NVMe: group 1은 sequence 관련 오류로
					 *  counter, val_diff 2개 인자 필요. */
		val = sscanf(kern_buf, "%u %d", &counter, &val_diff);
		if ((val != 2) || (val_diff < -4095 || val_diff > 4095)) {
			kfree(kern_buf);
			return -EINVAL;
		}
	} else {			/* NVMe: 나머지 그룹은 counter만 필요. */
		val = kstrtou32(kern_buf, 0, &counter);
		if (val) {		/* NVMe: 변환 실패 시 에러 처리. */
			kfree(kern_buf);
			return val;
		}
	}

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + ERR_INJ0_OFF + (0x4 * err_group));
					/* NVMe: 해당 그룹의 error injection
					 *  레지스터 읽기. */
	val &= ~(type_mask | EINJ_COUNT);
					/* NVMe: 기존 type과 counter 필드 클리어. */
	val |= ((err_inj_list[pdata->idx].err_inj_type << EINJ_TYPE_SHIFT) & type_mask);
					/* NVMe: 주입할 오류 type 설정. */
	val |= FIELD_PREP(EINJ_COUNT, counter);
					/* NVMe: 주입 반복 횟수 설정. */

	if (err_group == 1 || err_group == 4) {
		/* NVMe: group 1/4에만 val_diff 필드 적용. */
		val &= ~(EINJ_VAL_DIFF);
		val |= FIELD_PREP(EINJ_VAL_DIFF, val_diff);
	}
	if (err_group == 4) {
		/* NVMe: group 4에만 VC 번호 필드 적용; NVMe는 보통 VC0. */
		val &= ~(EINJ_VC_NUM);
		val |= FIELD_PREP(EINJ_VC_NUM, vc_num);
	}

	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + ERR_INJ0_OFF + (0x4 * err_group), val);
					/* NVMe: 오류 주입 파라미터 기록. */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + ERR_INJ_ENABLE_REG, (0x1 << err_group));
					/* NVMe: 해당 그룹의 error injection
					 *  활성화; 이후 NVMe PCIe transaction에
					 *  오류가 삽입되어 AER 핸들러 동작. */

	kfree(kern_buf);		/* NVMe: 사용자 입력 버퍼 해제. */
	return count;			/* NVMe: 쓰기 성공. */
}

/* PCI/NVMe: event counter 측정 대상을 선택하는 공통 헬퍼.
 *  NVMe link 품질 모니터링 시 counter 그룹/이벤트/lane을 제어 레지스터에
 *  설정. */
static void set_event_number(struct dwc_pcie_rasdes_priv *pdata,
			     struct dw_pcie *pci, struct dwc_pcie_rasdes_info *rinfo)
{
	u32 val;	/* NVMe: 제어 레지스터 값. */

	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
					/* NVMe: event counter 제어 레지스터 읽기. */
	val &= ~EVENT_COUNTER_ENABLE;	/* NVMe: counter enable 필드 일단 클리어;
					 *  뒤에서 다시 설정. */
	val &= ~(EVENT_COUNTER_GROUP_SELECT | EVENT_COUNTER_EVENT_SELECT);
					/* NVMe: 기존 그룹/이벤트 선택 클리어. */
	val |= FIELD_PREP(EVENT_COUNTER_GROUP_SELECT, event_list[pdata->idx].group_no);
					/* NVMe: 측정할 이벤트 그룹 설정. */
	val |= FIELD_PREP(EVENT_COUNTER_EVENT_SELECT, event_list[pdata->idx].event_no);
					/* NVMe: 측정할 구체적 이벤트 설정. */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG, val);
					/* NVMe: 제어 레지스터 기록. */
}

/* PCI/NVMe: debugfs 'counter_enable' 파일 읽기 콜백.
 *  NVMe PCIe link 이벤트 카운터가 현재 측정 중인지 상태 반환. */
static ssize_t counter_enable_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;	/* NVMe: 이벤트
							 *  private data. */
	struct dw_pcie *pci = pdata->pci;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability. */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];	/* NVMe: 출력 버퍼. */
	ssize_t pos;	/* NVMe: 출력 길이. */
	u32 val;	/* NVMe: 레지스터 값. */

	mutex_lock(&rinfo->reg_event_lock);
					/* NVMe: event counter 제어/데이터 레지스터
					 *  접근 동기화 시작. */
	set_event_number(pdata, pci, rinfo);	/* NVMe: 측정 대상 이벤트 선택. */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
					/* NVMe: 제어 레지스터 읽기. */
	mutex_unlock(&rinfo->reg_event_lock);
					/* NVMe: 동기화 종료. */
	val = FIELD_GET(EVENT_COUNTER_STATUS, val);
					/* NVMe: counter enable 상태 비트 추출. */
	if (val)			/* NVMe: 카운터 활성화 중; NVMe link 이벤트
					 *  누적 중. */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Counter Enabled\n");
	else				/* NVMe: 카운터 비활성화. */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Counter Disabled\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
					/* NVMe: 사용자 공간으로 상태 복사. */
}

/* PCI/NVMe: debugfs 'counter_enable' 파일 쓰기 콜백.
 *  NVMe PCIe link 이벤트 카운터를 활성화/비활성화. */
static ssize_t counter_enable_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;	/* NVMe: 이벤트
							 *  private data. */
	struct dw_pcie *pci = pdata->pci;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability. */
	u32 val, enable;	/* NVMe: 레지스터 값과 사용자 enable 입력. */
	int ret;	/* NVMe: kstrtou32_from_user 반환값. */

	ret = kstrtou32_from_user(buf, count, 0, &enable);
					/* NVMe: 사용자 입력을 u32로 변환
					 *  (0=disable, 1=enable). */
	if (ret)			/* NVMe: 변환 실패 시 errno 반환. */
		return ret;

	mutex_lock(&rinfo->reg_event_lock);
					/* NVMe: 레지스터 접근 보호 시작. */
	set_event_number(pdata, pci, rinfo);	/* NVMe: 측정 대상 이벤트 선택. */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
					/* NVMe: 제어 레지스터 읽기. */
	if (enable)			/* NVMe: enable 시 counter ON 값 설정. */
		val |= FIELD_PREP(EVENT_COUNTER_ENABLE, PER_EVENT_ON);
	else				/* NVMe: disable 시 counter OFF 값 설정. */
		val |= FIELD_PREP(EVENT_COUNTER_ENABLE, PER_EVENT_OFF);

	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG, val);
					/* NVMe: enable/disable 기록. */

	/*
	 * While enabling the counter, always read the status back to check if
	 * it is enabled or not. Return error if it is not enabled to let the
	 * users know that the counter is not supported on the platform.
	 */
	if (enable) {
		/* NVMe: enable 요청 시 하드웨어가 실제로 활성화되었는지
		 *  상태 비트로 확인; 미지원 플랫폼에서는 -EOPNOTSUPP
		 *  반환. */
		val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset +
					RAS_DES_EVENT_COUNTER_CTRL_REG);
		if (!FIELD_GET(EVENT_COUNTER_STATUS, val)) {
			mutex_unlock(&rinfo->reg_event_lock);
			return -EOPNOTSUPP;
		}
	}

	mutex_unlock(&rinfo->reg_event_lock);
					/* NVMe: 레지스터 접근 보호 종료. */

	return count;			/* NVMe: 쓰기 성공. */
}

/* PCI/NVMe: debugfs 'lane_select' 파일 읽기 콜백.
 *  NVMe PCIe event counter가 현재 측정 중인 lane 번호를 반환. */
static ssize_t counter_lane_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;	/* NVMe: 이벤트
							 *  private data. */
	struct dw_pcie *pci = pdata->pci;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability. */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];	/* NVMe: 출력 버퍼. */
	ssize_t pos;	/* NVMe: 출력 길이. */
	u32 val;	/* NVMe: 레지스터 값. */

	mutex_lock(&rinfo->reg_event_lock);
					/* NVMe: 제어 레지스터 보호 시작. */
	set_event_number(pdata, pci, rinfo);	/* NVMe: 측정 대상 이벤트 선택. */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
					/* NVMe: 제어 레지스터 읽기. */
	mutex_unlock(&rinfo->reg_event_lock);
					/* NVMe: 제어 레지스터 보호 종료. */
	val = FIELD_GET(EVENT_COUNTER_LANE_SELECT, val);
					/* NVMe: lane 선택 필드 추출. */
	pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Lane: %d\n", val);
					/* NVMe: lane 번호 문자열 생성. */

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
					/* NVMe: 사용자 공간으로 복사. */
}

/* PCI/NVMe: debugfs 'lane_select' 파일 쓰기 콜백.
 *  NVMe PCIe event counter의 측정 대상 lane을 변경. */
static ssize_t counter_lane_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;	/* NVMe: 이벤트
							 *  private data. */
	struct dw_pcie *pci = pdata->pci;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability. */
	u32 val, lane;	/* NVMe: 레지스터 값과 사용자 입력 lane. */
	int ret;	/* NVMe: 변환 결과. */

	ret = kstrtou32_from_user(buf, count, 0, &lane);
					/* NVMe: 사용자 입력 lane 번호 변환. */
	if (ret)			/* NVMe: 변환 실패 시 errno 반환. */
		return ret;

	mutex_lock(&rinfo->reg_event_lock);
					/* NVMe: 제어 레지스터 보호 시작. */
	set_event_number(pdata, pci, rinfo);	/* NVMe: 측정 대상 이벤트 선택. */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
					/* NVMe: 제어 레지스터 읽기. */
	val &= ~(EVENT_COUNTER_LANE_SELECT);	/* NVMe: 기존 lane 선택 필드
					 *  클리어. */
	val |= FIELD_PREP(EVENT_COUNTER_LANE_SELECT, lane);
					/* NVMe: 새 lane 설정. */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG, val);
					/* NVMe: lane 설정 기록. */
	mutex_unlock(&rinfo->reg_event_lock);
					/* NVMe: 제어 레지스터 보호 종료. */

	return count;			/* NVMe: 쓰기 성공. */
}

/* PCI/NVMe: debugfs 'counter_value' 파일 읽기 콜백.
 *  현재 선택된 NVMe PCIe link 이벤트의 누적 카운터 값을 반환. */
static ssize_t counter_value_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;	/* NVMe: 이벤트
							 *  private data. */
	struct dw_pcie *pci = pdata->pci;	/* NVMe: DWC PCIe host. */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;	/* NVMe:
							 *  RAS DES capability. */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];	/* NVMe: 출력 버퍼. */
	ssize_t pos;	/* NVMe: 출력 길이. */
	u32 val;	/* NVMe: 카운터 데이터 값. */

	mutex_lock(&rinfo->reg_event_lock);
					/* NVMe: 제어/데이터 레지스터 보호 시작. */
	set_event_number(pdata, pci, rinfo);	/* NVMe: 읽을 카운터 이벤트 선택. */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_DATA_REG);
					/* NVMe: event counter 데이터 레지스터 읽기;
					 *  NVMe link 이벤트 발생 횟수. */
	mutex_unlock(&rinfo->reg_event_lock);
					/* NVMe: 레지스터 보호 종료. */
	pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Counter value: %d\n", val);
					/* NVMe: 카운터 값 문자열 생성. */

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
					/* NVMe: 사용자 공간으로 복사. */
}

/* PCI/NVMe: debugfs 'ltssm_status' 파일의 seq_file show 콜백.
 *  현재 PCIe Link Training and Status State Machine(LTSSM) 상태를 출력;
 *  NVMe SSD가 link up(L0)인지, recovery/training 중인지 확인. */
static int ltssm_status_show(struct seq_file *s, void *v)
{
	struct dw_pcie *pci = s->private;	/* NVMe: DWC PCIe host. */
	enum dw_pcie_ltssm val;		/* NVMe: LTSSM 상태 열거형. */

	val = dw_pcie_get_ltssm(pci);	/* NVMe: LTSSM 상태 레지스터 읽기; NVMe
					 *  SSD와의 link 상태 진단. */
	seq_printf(s, "%s (0x%02x)\n", dw_pcie_ltssm_status_string(val), val);
					/* NVMe: 상태 문자열과 원시 값 출력;
					 *  예: L0(link up), Recovery, Detect
					 *  등. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: debugfs 'ltssm_status' 파일 open 콜백.
 *  seq_file을 생성하고 private 데이터로 dw_pcie 객체를 연결. */
static int ltssm_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, ltssm_status_show, inode->i_private);
					/* NVMe: single-entry seq_file 생성; NVMe
					 *  link 상태 출력 준비. */
}

/* PCI/NVMe: RAS DES debugfs 파일 생성 매크로.
 *  이름 문자열과 read/write fops를 연결하여 디버그 파일을 만듦. */
#define dwc_debugfs_create(name)			\
	debugfs_create_file(#name, 0644, rasdes_debug, pci,	\
				&dbg_ ## name ## _fops)

/* PCI/NVMe: read/write가 모두 있는 RAS DES debugfs 파일 연산 구조체를
 *  자동 생성하는 매크로. */
#define DWC_DEBUGFS_FOPS(name)					\
static const struct file_operations dbg_ ## name ## _fops = {	\
	.open = simple_open,				\
	.read = name ## _read,				\
	.write = name ## _write				\
}

DWC_DEBUGFS_FOPS(lane_detect);	/* NVMe: lane_detect 파일용 fops 생성. */
DWC_DEBUGFS_FOPS(rx_valid);	/* NVMe: rx_valid 파일용 fops 생성. */

/* PCI/NVMe: error injection debugfs 파일 연산 구조체.
 *  쓰기 전용(0200)으로 사용자가 오류를 주입. */
static const struct file_operations dwc_pcie_err_inj_ops = {
	.open = simple_open,	/* NVMe: private data 그대로 사용. */
	.write = err_inj_write,	/* NVMe: 오류 주입 쓰기 콜백. */
};

/* PCI/NVMe: event counter enable 파일 연산 구조체. */
static const struct file_operations dwc_pcie_counter_enable_ops = {
	.open = simple_open,	/* NVMe: private data 그대로 사용. */
	.read = counter_enable_read,	/* NVMe: enable 상태 읽기. */
	.write = counter_enable_write,	/* NVMe: enable/disable 쓰기. */
};

/* PCI/NVMe: event counter lane_select 파일 연산 구조체. */
static const struct file_operations dwc_pcie_counter_lane_ops = {
	.open = simple_open,	/* NVMe: private data 그대로 사용. */
	.read = counter_lane_read,	/* NVMe: 현재 lane 읽기. */
	.write = counter_lane_write,	/* NVMe: lane 설정 쓰기. */
};

/* PCI/NVMe: event counter value 파일 연산 구조체. */
static const struct file_operations dwc_pcie_counter_value_ops = {
	.open = simple_open,	/* NVMe: private data 그대로 사용. */
	.read = counter_value_read,	/* NVMe: 카운터 값 읽기. */
};

/* PCI/NVMe: LTSSM status 파일 연산 구조체. */
static const struct file_operations dwc_pcie_ltssm_status_ops = {
	.open = ltssm_status_open,	/* NVMe: seq_file open. */
	.read = seq_read,	/* NVMe: seq_file 기본 read. */
};

/* PCI/NVMe: RAS DES debugfs 해제 함수.
 *  드라이버 제거 시 mutex를 파괴; debugfs 디렉터리는 상위에서
 *  debugfs_remove_recursive로 제거. */
static void dwc_pcie_rasdes_debugfs_deinit(struct dw_pcie *pci)
{
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
					/* NVMe: RAS DES 공통 정보 획득. */

	mutex_destroy(&rinfo->reg_event_lock);
					/* NVMe: event counter 레지스터 보호용
					 *  mutex 파괴. */
}

/* PCI/NVMe: RAS DES debugfs 초기화 함수.
 *  NVMe PCIe link 디버깅을 위한 debugfs 트리(rasdes_debug,
 *  rasdes_err_inj, rasdes_event_counter)를 생성. */
static int dwc_pcie_rasdes_debugfs_init(struct dw_pcie *pci, struct dentry *dir)
{
	struct dentry *rasdes_debug, *rasdes_err_inj;	/* NVMe: debug/error
						 *  injection 디렉터리. */
	struct dentry *rasdes_event_counter, *rasdes_events;	/* NVMe: event
						 *  counter 디렉터리. */
	struct dwc_pcie_rasdes_info *rasdes_info;	/* NVMe: RAS DES 공통
						 *  정보 구조체. */
	struct dwc_pcie_rasdes_priv *priv_tmp;	/* NVMe: 개별 파일 private
						 *  데이터 임시 포인터. */
	struct device *dev = pci->dev;	/* NVMe: DWC PCIe host device; 메모리
					 *  할당 및 로그 출력용. */
	int ras_cap, i, ret;	/* NVMe: RAS capability 오프셋, 루프 변수,
					 *  반환값. */

	/*
	 * If a given SoC has no RAS DES capability, the following call is
	 * bound to return an error, breaking some existing platforms. So,
	 * return 0 here, as this is not necessarily an error.
	 */
	ras_cap = dw_pcie_find_rasdes_capability(pci);
					/* NVMe: DWC RC의 PCIe configuration space에서
					 *  RAS DES vendor-specific extended
					 *  capability 검색; NVMe AER/오류 모니터링
					 *  지원 여부 확인. */
	if (!ras_cap) {			/* NVMe: capability가 없으면 debugfs를
					 *  만들지 않고 정상 종료; 일부 SoC에서는
					 *  미지원. */
		dev_dbg(dev, "no RAS DES capability available\n");
		return 0;
	}

	rasdes_info = devm_kzalloc(dev, sizeof(*rasdes_info), GFP_KERNEL);
					/* NVMe: RAS DES 공통 정보 구조체 할당. */
	if (!rasdes_info)		/* NVMe: 메모리 부족 시 -ENOMEM 반환. */
		return -ENOMEM;

	/* Create subdirectories for Debug, Error Injection, Statistics. */
	rasdes_debug = debugfs_create_dir("rasdes_debug", dir);
					/* NVMe: Debug(lane_detect, rx_valid)용
					 *  디렉터리. */
	rasdes_err_inj = debugfs_create_dir("rasdes_err_inj", dir);
					/* NVMe: Error Injection용 디렉터리; NVMe
					 *  AER 복구 테스트 진입점. */
	rasdes_event_counter = debugfs_create_dir("rasdes_event_counter", dir);
					/* NVMe: Event Counter용 디렉터리; NVMe
					 *  link 품질/성능 모니터링 진입점. */

	mutex_init(&rasdes_info->reg_event_lock);	/* NVMe: event counter
						 *  레지스터 동시 접근 방지. */
	rasdes_info->ras_cap_offset = ras_cap;	/* NVMe: capability 오프셋 저장. */
	pci->debugfs->rasdes_info = rasdes_info;	/* NVMe: dw_pcie에 연결. */

	/* Create debugfs files for Debug subdirectory. */
	dwc_debugfs_create(lane_detect);	/* NVMe: lane_detect 파일 생성. */
	dwc_debugfs_create(rx_valid);	/* NVMe: rx_valid 파일 생성. */

	/* Create debugfs files for Error Injection subdirectory. */
	for (i = 0; i < ARRAY_SIZE(err_inj_list); i++) {
		/* NVMe: 모든 error injection 항목마다 debugfs 파일
		 *  생성. */
		priv_tmp = devm_kzalloc(dev, sizeof(*priv_tmp), GFP_KERNEL);
					/* NVMe: 개별 파일 private data 할당. */
		if (!priv_tmp) {	/* NVMe: 할당 실패 시 초기화 중단. */
			ret = -ENOMEM;
			goto err_deinit;
		}

		priv_tmp->idx = i;	/* NVMe: err_inj_list 인덱스 저장. */
		priv_tmp->pci = pci;	/* NVMe: DWC PCIe host 연결. */
		debugfs_create_file(err_inj_list[i].name, 0200, rasdes_err_inj, priv_tmp,
				    &dwc_pcie_err_inj_ops);
					/* NVMe: 쓰기 전용 오류 주입 파일 생성;
					 *  NVMe PCIe AER 이벤트 시뮬레이션. */
	}

	/* Create debugfs files for Statistical Counter subdirectory. */
	for (i = 0; i < ARRAY_SIZE(event_list); i++) {
		/* NVMe: 모든 event counter 항목마다 서브디렉터리 및
		 *  파일 생성. */
		priv_tmp = devm_kzalloc(dev, sizeof(*priv_tmp), GFP_KERNEL);
					/* NVMe: 개별 이벤트 private data 할당. */
		if (!priv_tmp) {	/* NVMe: 할당 실패 시 초기화 중단. */
			ret = -ENOMEM;
			goto err_deinit;
		}

		priv_tmp->idx = i;	/* NVMe: event_list 인덱스 저장. */
		priv_tmp->pci = pci;	/* NVMe: DWC PCIe host 연결. */
		rasdes_events = debugfs_create_dir(event_list[i].name, rasdes_event_counter);
					/* NVMe: 이벤트별 서브디렉터리 생성. */
		if (event_list[i].group_no == 0 || event_list[i].group_no == 4) {
			/* NVMe: group 0/4 이벤트는 per-lane 측정
			 *  가능하므로 lane_select 파일 추가. */
			debugfs_create_file("lane_select", 0644, rasdes_events,
					    priv_tmp, &dwc_pcie_counter_lane_ops);
		}
		debugfs_create_file("counter_value", 0444, rasdes_events, priv_tmp,
				    &dwc_pcie_counter_value_ops);
					/* NVMe: 카운터 값 읽기 파일 생성. */
		debugfs_create_file("counter_enable", 0644, rasdes_events, priv_tmp,
				    &dwc_pcie_counter_enable_ops);
					/* NVMe: 카운터 enable/disable 파일 생성. */
	}

	return 0;			/* NVMe: RAS DES debugfs 초기화 성공. */

err_deinit:
	dwc_pcie_rasdes_debugfs_deinit(pci);
					/* NVMe: 실패 시 mutex만 파괴; 디렉터리는
					 *  상위 debugfs_remove_recursive로 정리. */
	return ret;			/* NVMe: 에러 코드 반환. */
}

/* PCI/NVMe: LTSSM status debugfs 파일 초기화 함수.
 *  NVMe SSD의 PCIe link 상태(L0/Recovery 등)를 확인할 수 있는 파일 생성. */
static void dwc_pcie_ltssm_debugfs_init(struct dw_pcie *pci, struct dentry *dir)
{
	debugfs_create_file("ltssm_status", 0444, dir, pci,
			    &dwc_pcie_ltssm_status_ops);
					/* NVMe: 읽기 전용 LTSSM 상태 파일 생성. */
}

/* PCI/NVMe: PTM( Precision Time Measurement) capability 확인 콜백.
 *  NVMe SSD와의 정밀 시간 동기화에 필요한 PTM vendor-specific capability
 *  오프셋을 찾아 dw_pcie에 저장. */
static int dw_pcie_ptm_check_capability(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	pci->ptm_vsec_offset = dw_pcie_find_ptm_capability(pci);
					/* NVMe: PTM VSEC capability 오프셋 검색. */

	return pci->ptm_vsec_offset;	/* NVMe: 0이면 PTM 미지원; 양수면
					 *  capability 오프셋. */
}

/* PCI/NVMe: PTM context update 모드 쓰기 콜백.
 *  NVMe host(RC) 또는 NVMe device(EP)의 PTM context 갱신 방식을 제어. */
static int dw_pcie_ptm_context_update_write(void *drvdata, u8 mode)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 val;	/* NVMe: PTM 제어 레지스터 값. */

	if (mode == PCIE_PTM_CONTEXT_UPDATE_AUTO) {
		/* NVMe: 자동 갱신 모드; NVMe PTM 타임스탬프가 주기적
		 *  또는 이벤트에 의해 갱신. */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val |= PTM_REQ_AUTO_UPDATE_ENABLED;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else if (mode == PCIE_PTM_CONTEXT_UPDATE_MANUAL) {
		/* NVMe: 수동 갱신 모드; 소프트웨어가 START_UPDATE 비트를
		 *  써서 NVMe PTM context를 한 번 갱신. */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val &= ~PTM_REQ_AUTO_UPDATE_ENABLED;
		val |= PTM_REQ_START_UPDATE;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else {
		return -EINVAL;		/* NVMe: 잘못된 모드. */
	}

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM context update 모드 읽기 콜백.
 *  현재 PTM context가 자동 갱신 중인지 수동 갱신 중인지 확인. */
static int dw_pcie_ptm_context_update_read(void *drvdata, u8 *mode)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 val;	/* NVMe: PTM 제어 레지스터 값. */

	val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
	if (FIELD_GET(PTM_REQ_AUTO_UPDATE_ENABLED, val))
		/* NVMe: auto update 비트가 set이면 자동 갱신. */
		*mode = PCIE_PTM_CONTEXT_UPDATE_AUTO;
	else
		/*
		 * PTM_REQ_START_UPDATE is a self clearing register bit. So if
		 * PTM_REQ_AUTO_UPDATE_ENABLED is not set, then it implies that
		 * manual update is used.
		 */
		/* NVMe: auto update가 꺼져 있으면 수동 갱신으로 간주. */
		*mode = PCIE_PTM_CONTEXT_UPDATE_MANUAL;

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM context valid 비트 쓰기 콜백.
 *  NVMe PTM 측정값이 유효함을 표시하거나 무효화. */
static int dw_pcie_ptm_context_valid_write(void *drvdata, bool valid)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 val;	/* NVMe: PTM 제어 레지스터 값. */

	if (valid) {
		/* NVMe: PTM context valid 설정; NVMe 시간 동기화 값
		 *  신뢰 가능 표시. */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val |= PTM_RES_CCONTEXT_VALID;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else {
		/* NVMe: PTM context valid 클리어; 값을 신뢰하지 않음. */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		val &= ~PTM_RES_CCONTEXT_VALID;
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	}

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM context valid 비트 읽기 콜백.
 *  현재 PTM context가 유효한지 확인. */
static int dw_pcie_ptm_context_valid_read(void *drvdata, bool *valid)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 val;	/* NVMe: PTM 제어 레지스터 값. */

	val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
	*valid = !!FIELD_GET(PTM_RES_CCONTEXT_VALID, val);
					/* NVMe: valid 비트를 bool로 변환. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM local clock 값 읽기 콜백.
 *  64-bit local timebase를 MSB/LSB 두 번 읽어 일관성 있게 반환;
 *  NVMe host 내 타임스탬프 획득. */
static int dw_pcie_ptm_local_clock_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 msb, lsb;	/* NVMe: 64-bit 시계의 상위/하위 32비트. */

	do {
		/* NVMe: MSB가 읽는 도중 바뀌지 않았는지 확인하기 위해
		 *  반복 읽기; 64-bit atomic read 보장. */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB));

	*clock = ((u64) msb) << 32 | lsb;
					/* NVMe: 64-bit local clock 조합. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM master clock 값 읽기 콜백.
 *  NVMe PCIe root의 master time 값을 읽음; EP 모드에서 PTM slave가
 *  master 시간을 참조할 때 사용. */
static int dw_pcie_ptm_master_clock_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 msb, lsb;	/* NVMe: 64-bit 시계 상위/하위. */

	do {
		/* NVMe: 64-bit atomic read 보장을 위한 MSB 재확인 루프. */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB));

	*clock = ((u64) msb) << 32 | lsb;
					/* NVMe: 64-bit master clock 조합. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM T1(timestamp) 읽기 콜백.
 *  PTM 메시지 교환에서 t1 시점; NVMe EP가 PTM Request를 보낸 시각. */
static int dw_pcie_ptm_t1_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 msb, lsb;	/* NVMe: 64-bit timestamp 상위/하위. */

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	*clock = ((u64) msb) << 32 | lsb;
					/* NVMe: T1 timestamp 조합. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM T2(timestamp) 읽기 콜백.
 *  PTM 메시지 교환에서 t2 시점; NVMe RC가 PTM Request를 수신한 시각. */
static int dw_pcie_ptm_t2_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 msb, lsb;	/* NVMe: 64-bit timestamp 상위/하위. */

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	*clock = ((u64) msb) << 32 | lsb;
					/* NVMe: T2 timestamp 조합. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM T3(timestamp) 읽기 콜백.
 *  PTM 메시지 교환에서 t3 시점; NVMe RC가 PTM Response를 보낸 시각. */
static int dw_pcie_ptm_t3_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 msb, lsb;	/* NVMe: 64-bit timestamp 상위/하위. */

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	*clock = ((u64) msb) << 32 | lsb;
					/* NVMe: T3 timestamp 조합. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM T4(timestamp) 읽기 콜백.
 *  PTM 메시지 교환에서 t4 시점; NVMe EP가 PTM Response를 수신한 시각. */
static int dw_pcie_ptm_t4_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */
	u32 msb, lsb;	/* NVMe: 64-bit timestamp 상위/하위. */

	do {
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	*clock = ((u64) msb) << 32 | lsb;
					/* NVMe: T4 timestamp 조합. */

	return 0;			/* NVMe: 성공. */
}

/* PCI/NVMe: PTM context update 파일의 EP 가시성 콜백.
 *  context update 제어는 EP 모드(NVMe device)에서만 의미 있음. */
static bool dw_pcie_ptm_context_update_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	return pci->mode == DW_PCIE_EP_TYPE;	/* NVMe: EP 모드일 때만 표시. */
}

/* PCI/NVMe: PTM context valid 파일의 RC 가시성 콜백.
 *  context valid 제어는 RC 모드(NVMe host)에서만 의미 있음. */
static bool dw_pcie_ptm_context_valid_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	return pci->mode == DW_PCIE_RC_TYPE;	/* NVMe: RC 모드일 때만 표시. */
}

/* PCI/NVMe: PTM local clock 파일은 host/EP 모두 항상 표시. */
static bool dw_pcie_ptm_local_clock_visible(void *drvdata)
{
	/* PTM local clock is always visible */
	return true;
}

/* PCI/NVMe: PTM master clock 파일의 EP 가시성 콜백.
 *  master clock 읽기는 EP 모드(NVMe device)에서만 의미 있음. */
static bool dw_pcie_ptm_master_clock_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	return pci->mode == DW_PCIE_EP_TYPE;	/* NVMe: EP 모드일 때만 표시. */
}

/* PCI/NVMe: PTM T1 파일의 EP 가시성 콜백.
 *  T1은 EP가 PTM Request를 보낸 시각이므로 EP 모드에서만 표시. */
static bool dw_pcie_ptm_t1_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	return pci->mode == DW_PCIE_EP_TYPE;	/* NVMe: EP 모드일 때만 표시. */
}

/* PCI/NVMe: PTM T2 파일의 RC 가시성 콜백.
 *  T2는 RC가 PTM Request를 받은 시각이므로 RC 모드에서만 표시. */
static bool dw_pcie_ptm_t2_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	return pci->mode == DW_PCIE_RC_TYPE;	/* NVMe: RC 모드일 때만 표시. */
}

/* PCI/NVMe: PTM T3 파일의 RC 가시성 콜백.
 *  T3는 RC가 PTM Response를 보낸 시각이므로 RC 모드에서만 표시. */
static bool dw_pcie_ptm_t3_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	return pci->mode == DW_PCIE_RC_TYPE;	/* NVMe: RC 모드일 때만 표시. */
}

/* PCI/NVMe: PTM T4 파일의 EP 가시성 콜백.
 *  T4는 EP가 PTM Response를 받은 시각이므로 EP 모드에서만 표시. */
static bool dw_pcie_ptm_t4_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;	/* NVMe: DWC PCIe host/EP 객체. */

	return pci->mode == DW_PCIE_EP_TYPE;	/* NVMe: EP 모드일 때만 표시. */
}

/* PCI/NVMe: DesignWare PCIe PTM debugfs 동작 구조체.
 *  NVMe host(RC)와 NVMe device(EP) 양쪽의 PTM 시간 동기화 관련
 *  capability 확인/제어/읽기/가시성 함수를 등록. */
static const struct pcie_ptm_ops dw_pcie_ptm_ops = {
	.check_capability = dw_pcie_ptm_check_capability,
	.context_update_write = dw_pcie_ptm_context_update_write,
	.context_update_read = dw_pcie_ptm_context_update_read,
	.context_valid_write = dw_pcie_ptm_context_valid_write,
	.context_valid_read = dw_pcie_ptm_context_valid_read,
	.local_clock_read = dw_pcie_ptm_local_clock_read,
	.master_clock_read = dw_pcie_ptm_master_clock_read,
	.t1_read = dw_pcie_ptm_t1_read,
	.t2_read = dw_pcie_ptm_t2_read,
	.t3_read = dw_pcie_ptm_t3_read,
	.t4_read = dw_pcie_ptm_t4_read,
	.context_update_visible = dw_pcie_ptm_context_update_visible,
	.context_valid_visible = dw_pcie_ptm_context_valid_visible,
	.local_clock_visible = dw_pcie_ptm_local_clock_visible,
	.master_clock_visible = dw_pcie_ptm_master_clock_visible,
	.t1_visible = dw_pcie_ptm_t1_visible,
	.t2_visible = dw_pcie_ptm_t2_visible,
	.t3_visible = dw_pcie_ptm_t3_visible,
	.t4_visible = dw_pcie_ptm_t4_visible,
};

/* PCI/NVMe: DesignWare PCIe debugfs 전체 해제 함수.
 *  드라이버 제거 시 PTM/RAS DES/LTSSM debugfs를 정리. */
void dwc_pcie_debugfs_deinit(struct dw_pcie *pci)
{
	if (!pci->debugfs)		/* NVMe: debugfs가 초기화되지 않았으면
					 *  아무것도 하지 않음. */
		return;

	pcie_ptm_destroy_debugfs(pci->ptm_debugfs);
					/* NVMe: PTM debugfs 제거; NVMe 시간
					 *  동기화 관련 파일 정리. */
	dwc_pcie_rasdes_debugfs_deinit(pci);
					/* NVMe: RAS DES debugfs 내부 mutex 파괴. */
	debugfs_remove_recursive(pci->debugfs->debug_dir);
					/* NVMe: 이 DWC PCIe host의 전체 debugfs
					 *  디렉터리 재귀 제거. */
}

/* PCI/NVMe: DesignWare PCIe debugfs 전체 초기화 함수.
 *  NVMe PCIe host controller 아래에 debugfs 진입점을 생성하여, NVMe SSD의
 *  PCIe link 상태, 오류 주입, 이벤트 카운터, PTM, LTSSM 등을
 *  런타임에 디버깅. */
void dwc_pcie_debugfs_init(struct dw_pcie *pci, enum dw_pcie_device_mode mode)
{
	char dirname[DWC_DEBUGFS_BUF_MAX];	/* NVMe: debugfs 최상위 디렉터리
						 *  이름 버퍼. */
	struct device *dev = pci->dev;	/* NVMe: host device. */
	struct debugfs_info *debugfs;	/* NVMe: DWC PCIe debugfs 정보. */
	struct dentry *dir;	/* NVMe: 최상위 debugfs 디렉터리. */
	int err;		/* NVMe: RAS DES 초기화 결과. */

	/* Create main directory for each platform driver. */
	snprintf(dirname, DWC_DEBUGFS_BUF_MAX, "dwc_pcie_%s", dev_name(dev));
					/* NVMe: 디렉터리 이름 생성;
					 *  예: dwc_pcie_pci0000:00. */
	dir = debugfs_create_dir(dirname, NULL);
					/* NVMe: debugfs 최상위 디렉터리 생성. */
	debugfs = devm_kzalloc(dev, sizeof(*debugfs), GFP_KERNEL);
					/* NVMe: debugfs 정보 구조체 할당. */
	if (!debugfs)			/* NVMe: 메모리 부족 시 조용히 반환;
					 *  debugfs는 필수 기능이 아님. */
		return;

	debugfs->debug_dir = dir;	/* NVMe: 최상위 디렉터리 저장. */
	pci->debugfs = debugfs;		/* NVMe: dw_pcie에 debugfs 정보 연결. */
	err = dwc_pcie_rasdes_debugfs_init(pci, dir);
					/* NVMe: RAS DES debugfs 초기화; NVMe
					 *  AER/오류/이벤트 모니터링 진입점. */
	if (err)			/* NVMe: 초기화 실패 시 에러 로그 출력. */
		dev_err(dev, "failed to initialize RAS DES debugfs, err=%d\n",
			err);

	dwc_pcie_ltssm_debugfs_init(pci, dir);
					/* NVMe: LTSSM 상태 파일 생성; NVMe PCIe
					 *  link up/down 상태 확인. */

	pci->mode = mode;		/* NVMe: RC/EP 모드 저장; PTM 가시성 및
					 *  동작 결정. */
	pci->ptm_debugfs = pcie_ptm_create_debugfs(pci->dev, pci,
					   &dw_pcie_ptm_ops);
					/* NVMe: PTM debugfs 생성; NVMe host/device
					 *  간 정밀 시간 동기화 디버깅. */
}
