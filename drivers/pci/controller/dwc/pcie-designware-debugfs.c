// SPDX-License-Identifier: GPL-2.0
/*
 * Synopsys DesignWare PCIe controller debugfs driver
 *
 * Copyright (C) 2025 Samsung Electronics Co., Ltd.
 *		 http://www.samsung.com
 *
 * Author: Shradha Todi <shradha.t@samsung.com>
 */

/*
 * [한국어 설명] DesignWare PCIe 컨트롤러의 RAS DES / LTSSM / PTM 진단 인터페이스 (pcie-designware-debugfs.c)
 *
 * === 파일의 역할 ===
 * DesignWare PCIe IP 안에 들어 있는 진단 하드웨어를 debugfs 파일로 꺼내 보여 주는
 * 파일이다. 핵심은 RAS DES 라는 Synopsys 벤더 확장인데, 이는 Reliability,
 * Availability, Serviceability 를 위한 Debug, Error injection, Statistics 블록을
 * 뜻한다. 이 블록에는 (1) 레인 검출/RX 유효 같은 물리 계층 상태를 들여다보는 창,
 * (2) 링크에 일부러 오류를 심어 상대편의 복구 동작을 시험하는 오류 주입기,
 * (3) bad TLP, replay timeout, ASPM 진입 횟수 같은 것을 세는 이벤트 카운터 수십 종이
 * 들어 있다. 이 파일은 그 셋을 각각 rasdes_debug / rasdes_err_inj /
 * rasdes_event_counter 디렉토리로 노출한다. 여기에 더해 링크 트레이닝 상태 기계
 * (LTSSM)의 현재 상태를 문자열로 보여 주는 ltssm_status 파일과, PCIe 정밀 시각 측정
 * (PTM)의 타임스탬프를 PCI 코어의 공용 PTM debugfs 계층에 연결하는 콜백 묶음을 둔다.
 * 정상 동작에 필요한 코드는 하나도 없다 — 전부 진단용이며, 이 파일 자체가
 * CONFIG_PCIE_DW_DEBUGFS 로 빌드에서 통째로 빠질 수 있다(Makefile 이
 * obj-$(CONFIG_PCIE_DW_DEBUGFS) 로 걸어 두었고, 그 옵션은 DEBUG_FS 와
 * PCIE_DW_HOST/PCIE_DW_EP 중 하나에 의존한다). 빠지면 pcie-designware.h 의
 * 빈 인라인 함수가 대신 쓰여 호출부를 고칠 필요가 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DWC 드라이버 묶음에서 이 파일은 곁가지다. 본류인 pcie-designware.c(공용 하부),
 * pcie-designware-host.c(RC 모드), pcie-designware-ep.c(EP 모드) 중 뒤의 둘이
 * 초기화를 마치면서 dwc_pcie_debugfs_init(pci, mode) 을 부르는 것이 유일한 진입점이다.
 * RC 쪽은 DW_PCIE_RC_TYPE 을, EP 쪽은 DW_PCIE_EP_TYPE 을 넘기며, 그 값이 pci->mode 에
 * 저장되어 아래 PTM 가시성 콜백들이 "이 모드에서 의미 있는 파일만 만들라" 고 답하는
 * 근거가 된다. 해제는 반대로 dwc_pcie_debugfs_deinit() 이며 host.c 의 remove 와
 * ep.c 의 dw_pcie_ep_cleanup() 이 부른다.
 * 실행 컨텍스트는 두 갈래다. init/deinit 은 드라이버 probe/remove 의 프로세스
 * 컨텍스트에서 한 번 돈다. 반면 파일 읽기/쓰기 핸들러들은 사용자가 debugfs 파일에
 * cat 이나 echo 를 할 때마다 그 사용자 프로세스의 컨텍스트에서 불린다 — 즉 언제
 * 몇 개가 동시에 들어올지 알 수 없고, 그래서 아래 이벤트 카운터 경로에 뮤텍스가 있다.
 *
 * === 타 모듈과의 연결 ===
 * - 위쪽(호출자): pcie-designware-host.c 와 pcie-designware-ep.c 의 초기화/해제 경로.
 *   그리고 debugfs 를 통해 사용자 공간이 직접 이 파일의 read/write 핸들러를 부른다.
 * - 아래쪽(피호출자): pcie-designware.c 의 dw_pcie_find_rasdes_capability()(RAS DES
 *   벤더 확장의 위치를 찾는다), dw_pcie_find_ptm_capability(), dw_pcie_get_ltssm() 과
 *   dw_pcie_ltssm_status_string(), 그리고 DBI 읽기/쓰기 접근자.
 * - 옆쪽: drivers/pci/pcie/ptm.c 의 pcie_ptm_create_debugfs()/destroy_debugfs().
 *   이쪽이 PTM 파일들의 뼈대를 만들고, 이 파일은 struct pcie_ptm_ops 로 "레지스터를
 *   어떻게 읽고 쓰는지" 만 제공한다. 어떤 파일을 만들지는 그 구조체의 *_visible
 *   콜백이 결정하므로, 모드에 맞지 않는 파일은 아예 생기지 않는다.
 * - 공유 상태: struct dw_pcie 의 debugfs(디렉토리와 rasdes_info 포인터), mode,
 *   ptm_vsec_offset, ptm_debugfs 네 필드. 이 파일이 그 필드들의 유일한 설정자다.
 * - 데이터 흐름: 사용자 공간의 read → DBI 로 RAS DES 레지스터 읽기 → 문자열로 포맷 →
 *   simple_read_from_buffer 로 복사. write 는 반대로 사용자 문자열 파싱 →
 *   레지스터의 해당 필드만 갈아 끼우기.
 *
 * === 주요 함수/구조체 요약 ===
 * - dwc_pcie_debugfs_init() / _deinit(): 이 파일의 유일한 외부 진입점. 컨트롤러마다
 *   dwc_pcie_<디바이스명> 디렉토리를 만들고 그 아래에 세 갈래를 편다.
 * - dwc_pcie_rasdes_debugfs_init(): RAS DES 세 디렉토리와 그 안의 파일 수십 개를 만든다.
 *   RAS DES 가 없는 SoC 에서는 조용히 0 을 돌려주는 것이 중요한 설계 결정이다.
 * - err_inj_write(): 오류 주입. 그룹에 따라 인자 개수가 다른 것이 이 함수의 복잡도다.
 * - set_event_number() 와 counter_* 핸들러들: 이벤트 카운터는 "그림자 레지스터" 구조라,
 *   먼저 보고 싶은 이벤트 번호를 선택 레지스터에 쓴 다음에야 값을 읽을 수 있다.
 *   그 두 단계 사이에 다른 프로세스가 끼어들면 엉뚱한 카운터를 읽게 되므로
 *   reg_event_lock 뮤텍스로 묶는다.
 * - err_inj_list[] / event_list[]: 주입 가능한 오류 30종과 셀 수 있는 이벤트 80여 종의
 *   이름·그룹·번호 표. debugfs 파일 이름이 곧 이 표의 name 이다.
 * - dw_pcie_ptm_ops: PTM 타임스탬프 읽기 콜백 11개와 가시성 콜백 8개.
 */

/* [한국어] debugfs_create_dir()/debugfs_create_file()/debugfs_remove_recursive() 와
 * simple_open, simple_read_from_buffer. 이 파일의 존재 이유가 debugfs 이므로 필수다. */
#include <linux/debugfs.h>

/* [한국어] struct dw_pcie 와 그 안의 debugfs/mode/ptm_vsec_offset 필드, DBI 접근자,
 * dw_pcie_find_rasdes_capability(), dw_pcie_get_ltssm() 선언이 전부 여기서 온다. */
#include "pcie-designware.h"

/* [한국어] 아래 오프셋들은 모두 RAS DES 벤더 확장 블록의 시작(rinfo->ras_cap_offset)에서
 * 잰 상대 위치다. 절대 주소가 아닌 이유는 벤더 확장 capability 의 위치가 IP 설정에 따라
 * 달라져 런타임에 찾아야 하기 때문이다(dw_pcie_find_rasdes_capability). */

/* [한국어] Silicon Debug 그룹의 L1 레인 상태 레지스터. 아래 세 필드를 담고 있다. */
#define SD_STATUS_L1LANE_REG		0xb0
/* [한국어] 선택된 레인의 PIPE RX 유효 신호. PHY 가 그 레인에서 유효한 심볼을 받고 있는지. */
#define PIPE_RXVALID			BIT(18)
/* [한국어] 선택된 레인에서 상대편 리시버가 검출됐는지. 링크가 안 붙을 때 어느 레인이
 * 죽었는지 짚는 데 쓴다. */
#define PIPE_DETECT_LANE		BIT(17)
/* [한국어] 위 두 상태가 '몇 번 레인의 것인지' 를 고르는 필드(비트 3:0). 상태 비트가
 * 레인마다 따로 있는 것이 아니라, 이 선택자를 바꿔 가며 한 창으로 들여다보는 구조다.
 * lane_detect_write()/rx_valid_write() 가 쓰는 값이 바로 이 필드다. */
#define LANE_SELECT			GENMASK(3, 0)

/* [한국어] 오류 주입 그룹 0 의 제어 레지스터. 그룹 n 은 0x34 + 4*n 에 있다 —
 * err_inj_write() 의 (0x4 * err_group) 계산이 여기서 나온다. */
#define ERR_INJ0_OFF			0x34
/* [한국어] 시퀀스 번호나 크레딧 값을 얼마나 틀리게 만들지(비트 28:16). 부호 있는
 * 값이라 -4095~4095 범위이며, 그룹 1(시퀀스 번호)과 그룹 4(플로우 컨트롤 크레딧)에만 쓰인다. */
#define EINJ_VAL_DIFF			GENMASK(28, 16)
/* [한국어] 어느 가상 채널(Virtual Channel)의 크레딧을 망가뜨릴지(비트 14:12).
 * 그룹 4 에서만 의미가 있다. */
#define EINJ_VC_NUM			GENMASK(14, 12)
/* [한국어] 아래 EINJn_TYPE 필드들이 공통으로 시작하는 비트 자리(8). 그룹마다 필드 폭이
 * 달라 마스크는 따로지만 시작 위치는 같아서, err_inj_write() 가 값을 8비트 올린 뒤
 * 그룹별 마스크로 자른다. */
#define EINJ_TYPE_SHIFT			8
/* [한국어] 그룹 0(CRC 계열 오류)의 종류 필드. 9가지를 구분해야 해 4비트가 필요하다. */
#define EINJ0_TYPE			GENMASK(11, 8)
/* [한국어] 그룹 1(시퀀스 번호 오류)의 종류 필드. TLP 와 ACK/NAK 두 가지뿐이라 1비트. */
#define EINJ1_TYPE			BIT(8)
/* [한국어] 그룹 2(DLLP 오류)의 종류 필드. 세 가지라 2비트. */
#define EINJ2_TYPE			GENMASK(9, 8)
/* [한국어] 그룹 3(심볼/오더드셋 오류)의 종류 필드. 여덟 가지라 3비트. */
#define EINJ3_TYPE			GENMASK(10, 8)
/* [한국어] 그룹 4(플로우 컨트롤 크레딧 오류)의 종류 필드. 그룹 3 과 같은 3비트 폭이다. */
#define EINJ4_TYPE			GENMASK(10, 8)
/* [한국어] 그룹 5(TLP 중복/무효화)의 종류 필드. 두 가지라 1비트. */
#define EINJ5_TYPE			BIT(8)
/* [한국어] 오류를 몇 번 심을지(비트 7:0). 사용자가 debugfs 에 쓴 첫 숫자가 여기 들어간다.
 * 하드웨어가 이 횟수만큼 오류를 낸 뒤 스스로 멈춘다. */
#define EINJ_COUNT			GENMASK(7, 0)

/* [한국어] 오류 주입 활성화 레지스터. 비트 n 을 세우면 그룹 n 의 주입이 시작된다.
 * err_inj_write() 마지막의 (0x1 << err_group) 이 그것이다. */
#define ERR_INJ_ENABLE_REG		0x30

/* [한국어] 이벤트 카운터 값 레지스터. 아래 CTRL 로 '어느 이벤트를 볼지' 를 고른 뒤
 * 이 한 자리를 읽으면 그 이벤트의 누적 횟수가 나온다 — 카운터마다 레지스터가 있는 것이
 * 아니라 창 하나를 돌려 쓰는 그림자 구조다. */
#define RAS_DES_EVENT_COUNTER_DATA_REG	0xc

/* [한국어] 이벤트 카운터 제어 레지스터. '어느 이벤트를 볼지' 선택과 켜기/끄기가 모두 여기 있다. */
#define RAS_DES_EVENT_COUNTER_CTRL_REG	0x8
/* [한국어] 볼 이벤트의 그룹 번호(비트 27:24). event_list[] 의 group_no 가 들어간다. */
#define EVENT_COUNTER_GROUP_SELECT	GENMASK(27, 24)
/* [한국어] 그룹 안에서의 이벤트 번호(비트 23:16). event_list[] 의 event_no 가 들어간다.
 * 이 둘이 짝을 이뤄 하나의 카운터를 지목한다. */
#define EVENT_COUNTER_EVENT_SELECT	GENMASK(23, 16)
/* [한국어] 레인별로 따로 세는 이벤트일 때 어느 레인을 볼지(비트 11:8).
 * 그룹 0(물리 계층)과 그룹 4(엘라스틱 버퍼)만 레인 개념이 있어, 초기화 코드가
 * 그 두 그룹에만 lane_select 파일을 만든다. */
#define EVENT_COUNTER_LANE_SELECT	GENMASK(11, 8)
/* [한국어] 이 카운터가 현재 켜져 있는지(비트 7). 쓰기가 아니라 읽기 전용 상태다.
 * 켜기를 시도한 뒤 이 비트를 되읽어 보는 것이 '이 플랫폼이 그 이벤트를 지원하는가' 를
 * 알아내는 유일한 방법이다 — counter_enable_write() 가 그 확인을 한다. */
#define EVENT_COUNTER_STATUS		BIT(7)
/* [한국어] 카운터 켜기/끄기 명령 필드(비트 4:2). 상태(STATUS)와 명령이 별개 필드라는 점이
 * 중요한데, 명령을 써도 하드웨어가 받아들이지 않으면 상태는 바뀌지 않는다. */
#define EVENT_COUNTER_ENABLE		GENMASK(4, 2)
/* [한국어] 선택된 이벤트 하나만 켜라는 명령 값. */
#define PER_EVENT_ON			0x3
/* [한국어] 선택된 이벤트 하나만 꺼라는 명령 값. */
#define PER_EVENT_OFF			0x1

/* [한국어] 이 파일이 쓰는 모든 문자열 버퍼의 크기. 상태 문자열과 디렉토리 이름 양쪽에
 * 같은 값을 쓴다. 스택에 잡는 버퍼라 크게 두지 않는다. */
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
/* [한국어] 한 컨트롤러의 모든 RAS DES debugfs 파일이 공유하는 정보.
 * dwc_pcie_rasdes_debugfs_init() 이 devm 으로 하나 만들어 pci->debugfs->rasdes_info 에
 * 걸어 두고, 이후 모든 핸들러가 그 포인터로 되찾는다. */
struct dwc_pcie_rasdes_info {
	u32 ras_cap_offset;
	/* [한국어] RAS DES 벤더 확장 capability 블록이 설정 공간의 어디에서 시작하는지.
	 * 이 파일의 모든 레지스터 오프셋은 여기에 더해져 절대 위치가 된다.
	 * 설정자: dwc_pcie_rasdes_debugfs_init() 이 dw_pcie_find_rasdes_capability() 결과로 한 번 채운다.
	 * 읽는 자: 이 파일의 모든 read/write 핸들러와 set_event_number().
	 * 값 범위: 0 이 아닌 설정 공간 오프셋. 0 이면 RAS DES 가 없다는 뜻이라 애초에
	 *   이 구조체가 만들어지지 않는다.
	 * 동기화: 초기화 때 한 번 쓰이고 이후로는 읽기 전용이라 잠금이 필요 없다. */

	struct mutex reg_event_lock;
	/* [한국어] 이벤트 카운터의 '선택 후 접근' 두 단계를 원자적으로 묶는 뮤텍스.
	 * 카운터가 그림자 구조라서 필요하다 — set_event_number() 로 볼 이벤트를 고른 뒤
	 * 값을 읽기까지 사이에 다른 프로세스가 다른 이벤트를 고르면 엉뚱한 값을 읽게 된다.
	 * 설정자: dwc_pcie_rasdes_debugfs_init() 의 mutex_init,
	 *   dwc_pcie_rasdes_debugfs_deinit() 의 mutex_destroy.
	 * 읽는 자: counter_enable_read/write, counter_lane_read/write, counter_value_read
	 *   다섯 핸들러가 잡고 푼다.
	 * 값 범위: 일반 뮤텍스. 사용자 프로세스 컨텍스트에서만 잡히므로 잠들어도 무방하다.
	 * 동기화: 오류 주입(err_inj_write)과 레인 선택(lane_detect_write)은 이 잠금을
	 *   쓰지 않는다. 그쪽은 읽고-고치고-쓰기 한 번으로 끝나 두 단계로 나뉘지 않기 때문이다. */
};

/**
 * struct dwc_pcie_rasdes_priv - Stores file specific private data information
 * @pci: Reference to the dw_pcie structure
 * @idx: Index of specific file related information in array of structs
 *
 * All debugfs files will have this struct as its private data.
 */
/* [한국어] debugfs 파일 하나하나가 자기 것으로 들고 있는 private 데이터.
 * 파일 개수만큼(오류 30개 + 이벤트 80여 개) devm 으로 따로 할당된다. */
struct dwc_pcie_rasdes_priv {
	struct dw_pcie *pci;
	/* [한국어] 이 파일이 속한 컨트롤러. 핸들러는 file->private_data 에서 이 구조체를
	 * 꺼내고, 다시 여기서 pci 를 얻어 DBI 접근과 rasdes_info 접근을 한다.
	 * 설정자: dwc_pcie_rasdes_debugfs_init() 의 파일 생성 루프.
	 * 읽는 자: err_inj_write, counter_* 핸들러 전부.
	 * 값 범위: 유효한 dw_pcie 포인터. NULL 이 될 수 없다.
	 * 동기화: 생성 후 불변이라 잠금이 필요 없다. */

	int idx;
	/* [한국어] 이 파일이 err_inj_list[] 또는 event_list[] 의 몇 번째 항목인지.
	 * 파일 이름과 실제 하드웨어 그룹/번호를 잇는 유일한 연결 고리다 — 핸들러는
	 * 파일 이름을 보지 않고 이 인덱스로 표를 되짚어 어떤 오류/이벤트인지 안다.
	 * 설정자: 파일 생성 루프의 반복 변수 i.
	 * 읽는 자: err_inj_write() 의 err_inj_list[pdata->idx],
	 *   set_event_number() 의 event_list[pdata->idx].
	 * 값 범위: 해당 표의 0 ~ ARRAY_SIZE-1.
	 * 동기화: 생성 후 불변. */
};

/**
 * struct dwc_pcie_err_inj - Store details about each error injection
 *			     supported by DWC RAS DES
 * @name: Name of the error that can be injected
 * @err_inj_group: Group number to which the error belongs. The value
 *		   can range from 0 to 5
 * @err_inj_type: Each group can have multiple types of error
 */
/* [한국어] 주입 가능한 오류 한 종류의 정의. 아래 err_inj_list[] 의 원소 타입이다. */
struct dwc_pcie_err_inj {
	const char *name;
	/* [한국어] debugfs 에 만들어질 파일 이름이자 사람이 읽는 오류 이름.
	 * 예: "tx_lcrc" 는 보내는 TLP 의 링크 CRC 를 망가뜨린다는 뜻.
	 * 설정자: 아래 표의 초기화식(컴파일 타임 상수).
	 * 읽는 자: dwc_pcie_rasdes_debugfs_init() 의 debugfs_create_file().
	 * 값 범위: 표에 적힌 30개 문자열. 정적 문자열이라 해제되지 않는다.
	 * 동기화: const 상수라 불필요. */

	u32 err_inj_group;
	/* [한국어] 이 오류가 속한 그룹 번호(0~5). 그룹이 곧 어느 제어 레지스터를 쓸지를
	 * 정한다 — ERR_INJ0_OFF + 4*group. 또한 그룹에 따라 사용자가 넘겨야 하는 인자
	 * 개수가 달라진다(그룹 1 은 2개, 그룹 4 는 3개, 나머지는 1개).
	 * 설정자: 표의 초기화식.
	 * 읽는 자: err_inj_write() 가 레지스터 오프셋 계산, 타입 마스크 선택,
	 *   인자 파싱 분기, 활성화 비트 위치 계산에 모두 쓴다.
	 * 값 범위: 0~5. err_inj_type_mask[] 의 인덱스이기도 하다.
	 * 동기화: const 상수라 불필요. */

	u32 err_inj_type;
	/* [한국어] 그룹 안에서 이 오류가 몇 번인지. 그룹과 짝을 이뤄 하나의 오류를 지목한다.
	 * 설정자: 표의 초기화식.
	 * 읽는 자: err_inj_write() 가 EINJ_TYPE_SHIFT 만큼 올린 뒤 그룹별 마스크로 잘라 쓴다.
	 * 값 범위: 그룹마다 다르다. 그룹 0 은 0x0~0xb 로 중간에 빈 번호가 있고,
	 *   그룹 5 는 0x0~0x1 뿐이다. 그래서 마스크 폭도 그룹마다 다르다.
	 * 동기화: const 상수라 불필요. */
};

/* [한국어] 주입 가능한 오류 30종의 표. 각 항목이 rasdes_err_inj/ 아래 파일 하나가 된다.
 * 크게 보면 그룹 0 은 CRC 계열(보내는 쪽과 받는 쪽 모두), 그룹 1 은 시퀀스 번호,
 * 그룹 2 는 DLLP(데이터 링크 계층 패킷), 그룹 3 은 심볼과 오더드셋, 그룹 4 는
 * 플로우 컨트롤 크레딧, 그룹 5 는 TLP 무효화/중복이다. 상대편 장치가 이런 오류를
 * 만났을 때 제대로 재전송하고 복구하는지를 시험하는 용도다. */
static const struct dwc_pcie_err_inj err_inj_list[] = {
	{"tx_lcrc", 0x0, 0x0},
	{"b16_crc_dllp", 0x0, 0x1},
	{"b16_crc_upd_fc", 0x0, 0x2},
	{"tx_ecrc", 0x0, 0x3},
	{"fcrc_tlp", 0x0, 0x4},
	{"parity_tsos", 0x0, 0x5},
	{"parity_skpos", 0x0, 0x6},
	{"rx_lcrc", 0x0, 0x8},
	{"rx_ecrc", 0x0, 0xb},
	{"tlp_err_seq", 0x1, 0x0},
	{"ack_nak_dllp_seq", 0x1, 0x1},
	{"ack_nak_dllp", 0x2, 0x0},
	{"upd_fc_dllp", 0x2, 0x1},
	{"nak_dllp", 0x2, 0x2},
	{"inv_sync_hdr_sym", 0x3, 0x0},
	{"com_pad_ts1", 0x3, 0x1},
	{"com_pad_ts2", 0x3, 0x2},
	{"com_fts", 0x3, 0x3},
	{"com_idl", 0x3, 0x4},
	{"end_edb", 0x3, 0x5},
	{"stp_sdp", 0x3, 0x6},
	{"com_skp", 0x3, 0x7},
	{"posted_tlp_hdr", 0x4, 0x0},
	{"non_post_tlp_hdr", 0x4, 0x1},
	{"cmpl_tlp_hdr", 0x4, 0x2},
	{"posted_tlp_data", 0x4, 0x4},
	{"non_post_tlp_data", 0x4, 0x5},
	{"cmpl_tlp_data", 0x4, 0x6},
	{"duplicate_tlp", 0x5, 0x0},
	{"nullified_tlp", 0x5, 0x1},
};

/* [한국어] 그룹 번호를 그 그룹의 타입 필드 마스크로 바꾸는 표. 배열 인덱스가 곧
 * 그룹 번호이므로 err_inj_type_mask[err_group] 한 번의 조회로 끝난다.
 * 그룹마다 오류 종류의 개수가 달라 필드 폭이 1~4비트로 제각각이라 이 표가 필요하다. */
static const u32 err_inj_type_mask[] = {
	EINJ0_TYPE,
	EINJ1_TYPE,
	EINJ2_TYPE,
	EINJ3_TYPE,
	EINJ4_TYPE,
	EINJ5_TYPE,
};

/**
 * struct dwc_pcie_event_counter - Store details about each event counter
 *				   supported in DWC RAS DES
 * @name: Name of the error counter
 * @group_no: Group number that the event belongs to. The value can range
 *	      from 0 to 7
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
/* [한국어] 셀 수 있는 이벤트 한 종류의 정의. 아래 event_list[] 의 원소 타입이다. */
struct dwc_pcie_event_counter {
	const char *name;
	/* [한국어] debugfs 에 만들어질 디렉토리 이름. 오류 주입과 달리 이벤트는 파일이
	 * 아니라 디렉토리가 되고, 그 안에 counter_value/counter_enable(과 일부는 lane_select)이 놓인다.
	 * 설정자: 아래 표의 초기화식.
	 * 읽는 자: dwc_pcie_rasdes_debugfs_init() 의 debugfs_create_dir().
	 * 값 범위: 표에 적힌 80여 개 문자열. "l1.1_entry" 처럼 점이 들어간 이름도 있다.
	 * 동기화: const 상수라 불필요. */

	u32 group_no;
	/* [한국어] 이 이벤트가 속한 그룹 번호(0~7). 그룹이 이벤트의 성격을 나눈다 —
	 * 0 은 물리 계층 오류, 1 은 링크 트레이닝/복구, 2 는 데이터 링크 계층 재전송,
	 * 3 은 트랜잭션 계층 오류, 4 는 엘라스틱 버퍼 SKP 조정, 5 는 전력 상태 전이,
	 * 6 은 DLLP 개수, 7 은 TLP 종류별 개수.
	 * 설정자: 표의 초기화식.
	 * 읽는 자: set_event_number() 가 GROUP_SELECT 필드에 넣는다. 초기화 코드도
	 *   이 값이 0 이나 4 인지 보고 lane_select 파일을 만들지 결정한다 —
	 *   그 두 그룹만 레인별로 따로 세기 때문이다.
	 * 값 범위: 0~7.
	 * 동기화: const 상수라 불필요. */

	u32 event_no;
	/* [한국어] 그룹 안에서의 이벤트 번호. 그룹과 짝을 이뤄 하나의 카운터를 지목한다.
	 * 설정자: 표의 초기화식.
	 * 읽는 자: set_event_number() 가 EVENT_SELECT 필드에 넣는다.
	 * 값 범위: 그룹마다 다르다(원문 kernel-doc 이 그룹별 범위를 나열한다).
	 *   그룹 1 이 5 부터 시작하고 중간에 0xb 가 빠져 있는 것처럼 연속이 아닌 그룹도 있다.
	 * 동기화: const 상수라 불필요. */
};

/* [한국어] 셀 수 있는 이벤트 80여 종의 표. 각 항목이 rasdes_event_counter/ 아래
 * 디렉토리 하나가 되고, 그 안의 counter_value 를 읽으면 그 이벤트가 몇 번 일어났는지 나온다.
 * 링크 품질 진단에 실질적으로 쓰이는 것들이 여기 있다 — bad_tlp/lcrc_err/retry_tlp 로
 * 재전송이 얼마나 일어나는지, replay_timeout 으로 상대가 응답을 놓치는지,
 * l1_entry/speed_change 로 전력 관리가 어떻게 동작하는지, 그룹 7 의 tx/rx_memory_read 등으로
 * 실제 트래픽 구성이 어떤지를 본다. */
static const struct dwc_pcie_event_counter event_list[] = {
	{"ebuf_overflow", 0x0, 0x0},
	{"ebuf_underrun", 0x0, 0x1},
	{"decode_err", 0x0, 0x2},
	{"running_disparity_err", 0x0, 0x3},
	{"skp_os_parity_err", 0x0, 0x4},
	{"sync_header_err", 0x0, 0x5},
	{"rx_valid_deassertion", 0x0, 0x6},
	{"ctl_skp_os_parity_err", 0x0, 0x7},
	{"retimer_parity_err_1st", 0x0, 0x8},
	{"retimer_parity_err_2nd", 0x0, 0x9},
	{"margin_crc_parity_err", 0x0, 0xA},
	{"detect_ei_infer", 0x1, 0x5},
	{"receiver_err", 0x1, 0x6},
	{"rx_recovery_req", 0x1, 0x7},
	{"n_fts_timeout", 0x1, 0x8},
	{"framing_err", 0x1, 0x9},
	{"deskew_err", 0x1, 0xa},
	{"framing_err_in_l0", 0x1, 0xc},
	{"deskew_uncompleted_err", 0x1, 0xd},
	{"bad_tlp", 0x2, 0x0},
	{"lcrc_err", 0x2, 0x1},
	{"bad_dllp", 0x2, 0x2},
	{"replay_num_rollover", 0x2, 0x3},
	{"replay_timeout", 0x2, 0x4},
	{"rx_nak_dllp", 0x2, 0x5},
	{"tx_nak_dllp", 0x2, 0x6},
	{"retry_tlp", 0x2, 0x7},
	{"fc_timeout", 0x3, 0x0},
	{"poisoned_tlp", 0x3, 0x1},
	{"ecrc_error", 0x3, 0x2},
	{"unsupported_request", 0x3, 0x3},
	{"completer_abort", 0x3, 0x4},
	{"completion_timeout", 0x3, 0x5},
	{"ebuf_skp_add", 0x4, 0x0},
	{"ebuf_skp_del", 0x4, 0x1},
	{"l0_to_recovery_entry", 0x5, 0x0},
	{"l1_to_recovery_entry", 0x5, 0x1},
	{"tx_l0s_entry", 0x5, 0x2},
	{"rx_l0s_entry", 0x5, 0x3},
	{"aspm_l1_reject", 0x5, 0x4},
	{"l1_entry", 0x5, 0x5},
	{"l1_cpm", 0x5, 0x6},
	{"l1.1_entry", 0x5, 0x7},
	{"l1.2_entry", 0x5, 0x8},
	{"l1_short_duration", 0x5, 0x9},
	{"l1.2_abort", 0x5, 0xa},
	{"l2_entry", 0x5, 0xb},
	{"speed_change", 0x5, 0xc},
	{"link_width_change", 0x5, 0xd},
	{"tx_ack_dllp", 0x6, 0x0},
	{"tx_update_fc_dllp", 0x6, 0x1},
	{"rx_ack_dllp", 0x6, 0x2},
	{"rx_update_fc_dllp", 0x6, 0x3},
	{"rx_nullified_tlp", 0x6, 0x4},
	{"tx_nullified_tlp", 0x6, 0x5},
	{"rx_duplicate_tlp", 0x6, 0x6},
	{"tx_memory_write", 0x7, 0x0},
	{"tx_memory_read", 0x7, 0x1},
	{"tx_configuration_write", 0x7, 0x2},
	{"tx_configuration_read", 0x7, 0x3},
	{"tx_io_write", 0x7, 0x4},
	{"tx_io_read", 0x7, 0x5},
	{"tx_completion_without_data", 0x7, 0x6},
	{"tx_completion_w_data", 0x7, 0x7},
	{"tx_message_tlp_pcie_vc_only", 0x7, 0x8},
	{"tx_atomic", 0x7, 0x9},
	{"tx_tlp_with_prefix", 0x7, 0xa},
	{"rx_memory_write", 0x7, 0xb},
	{"rx_memory_read", 0x7, 0xc},
	{"rx_configuration_write", 0x7, 0xd},
	{"rx_configuration_read", 0x7, 0xe},
	{"rx_io_write", 0x7, 0xf},
	{"rx_io_read", 0x7, 0x10},
	{"rx_completion_without_data", 0x7, 0x11},
	{"rx_completion_w_data", 0x7, 0x12},
	{"rx_message_tlp_pcie_vc_only", 0x7, 0x13},
	{"rx_atomic", 0x7, 0x14},
	{"rx_tlp_with_prefix", 0x7, 0x15},
	{"tx_ccix_tlp", 0x7, 0x16},
	{"rx_ccix_tlp", 0x7, 0x17},
	{"tx_deferrable_memory_write_tlp", 0x7, 0x18},
	{"rx_deferrable_memory_write_tlp", 0x7, 0x19},
};

/* [한국어]
 * lane_detect_read - 선택된 레인에서 상대편 리시버가 검출됐는지 보여 준다
 *
 * @file: 열린 debugfs 파일. private_data 에 struct dw_pcie 가 들어 있다(simple_open 이 넣어 준다).
 * @buf: 사용자 공간 버퍼.
 * @count: 사용자가 요청한 바이트 수.
 * @ppos: 파일 오프셋. 부분 읽기를 이어서 하도록 simple_read_from_buffer 가 갱신한다.
 * @return: 복사한 바이트 수. 오프셋이 끝을 넘으면 0.
 *
 * 링크가 붙지 않을 때 "전기적으로 상대가 거기 있기는 한가" 를 확인하는 가장 낮은
 * 층위의 진단이다. PHY 의 PIPE 인터페이스가 내는 검출 신호를 그대로 보여 준다.
 * 어느 레인의 것인지는 같은 레지스터의 LANE_SELECT 필드가 정하며, 그 값은
 * lane_detect_write() 로 미리 골라 둔다 — 즉 이 파일은 읽기와 쓰기의 의미가 다르다.
 * 읽으면 '검출됐는가', 쓰면 '몇 번 레인을 볼 것인가' 다.
 *
 * 결과를 스택 버퍼에 문자열로 만든 뒤 simple_read_from_buffer 로 넘기는 것은
 * debugfs 읽기 핸들러의 관용적인 형태다. 그 함수가 오프셋 처리와
 * copy_to_user 를 대신해 준다.
 *
 * 실행 컨텍스트: 사용자가 이 파일을 read 할 때 그 프로세스 컨텍스트.
 * 동시에 여러 번 불릴 수 있지만 잠금이 없다 — 레지스터 읽기 한 번이라 두 단계로
 * 나뉘지 않기 때문이다. 다만 다른 프로세스가 그 사이 LANE_SELECT 를 바꾸면
 * 다른 레인의 값을 볼 수 있다.
 *
 * 호출 체인:
 *   사용자 공간 read() → VFS → dbg_lane_detect_fops.read → [이 함수]
 *     → dw_pcie_readl_dbi(), simple_read_from_buffer()
 */
static ssize_t lane_detect_read(struct file *file, char __user *buf,
				size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;
	/* [한국어] RAS DES 블록의 시작 오프셋을 얻는다. 아래 레지스터 주소가 전부 여기 기준이다 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] 결과 문자열을 담을 스택 버퍼 */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];
	/* [한국어] 만들어진 문자열의 길이. simple_read_from_buffer 에 넘길 유효 바이트 수다 */
	ssize_t pos;
	/* [한국어] 레지스터에서 읽은 원시 값 */
	u32 val;

	/* [한국어] Silicon Debug 의 L1 레인 상태 레지스터를 읽는다. capability 시작 + 0xb0 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG);
	/* [한국어] 레인 검출 비트만 뽑는다. 어느 레인의 것인지는 같은 레지스터의 LANE_SELECT 가 정한다 */
	val = FIELD_GET(PIPE_DETECT_LANE, val);
	/* [한국어] 검출됨 */
	if (val)
		/* [한국어] scnprintf 는 잘림이 나도 실제로 쓴 길이만 돌려주므로 오버플로 계산 실수가 없다 */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Lane Detected\n");
	else
		/* [한국어] 미검출 — 상대편 리시버가 그 레인에 없거나 전기적으로 죽어 있다는 뜻 */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Lane Undetected\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
}

/* [한국어]
 * lane_detect_write - 앞으로 들여다볼 레인 번호를 고른다
 *
 * @file: 열린 debugfs 파일. private_data 는 struct dw_pcie.
 * @buf: 사용자가 쓴 문자열.
 * @count: 그 길이.
 * @ppos: 파일 오프셋. 쓰이지 않는다.
 * @return: 성공하면 count(전부 소비했다는 뜻), 파싱 실패면 음수 오류.
 *
 * 이름과 달리 무언가를 검출하는 것이 아니라 선택자를 바꾼다. RAS DES 의 레인 상태는
 * 레인마다 레지스터가 있는 것이 아니라 창 하나를 LANE_SELECT 로 돌려 쓰는 구조라,
 * 보고 싶은 레인을 먼저 여기에 써야 한다.
 *
 * kstrtou32_from_user 는 사용자 버퍼에서 바로 숫자를 파싱한다(base 0 이므로
 * 0x 접두사도 받는다). 읽고-고치고-쓰기로 LANE_SELECT 필드만 갈아 끼우는 것은
 * 같은 레지스터의 상태 비트들을 뭉개지 않기 위해서다 — 다만 그 비트들은 읽기 전용이라
 * 실제로는 무해하다.
 *
 * rx_valid_write() 가 이 함수를 그대로 되부른다. 두 파일이 같은 레지스터의 같은
 * 선택자를 공유하기 때문이다.
 *
 * 실행 컨텍스트: 사용자 write 의 프로세스 컨텍스트. 잠금 없음 — 읽고-고치고-쓰기가
 * 원자적이지 않아 동시 쓰기가 겹치면 한쪽이 지워질 수 있지만, 진단용이라 감수한다.
 *
 * 호출 체인:
 *   사용자 공간 write() → VFS → dbg_lane_detect_fops.write / rx_valid_write()
 *     → [이 함수] → kstrtou32_from_user(), dw_pcie_readl_dbi()/writel_dbi()
 */
static ssize_t lane_detect_write(struct file *file, const char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;
	/* [한국어] RAS DES 블록 시작 오프셋 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] lane 은 사용자가 고른 레인 번호, val 은 읽고 고칠 레지스터 값 */
	u32 lane, val;
	/* [한국어] 파싱 결과 */
	int ret;

	/* [한국어] 사용자 버퍼에서 바로 숫자를 파싱한다. base 0 이라 0x 접두사도 받는다 */
	ret = kstrtou32_from_user(buf, count, 0, &lane);
	/* [한국어] 숫자가 아니면 그 오류를 그대로 사용자에게 돌려준다 */
	if (ret)
		return ret;

	/* [한국어] 읽고-고치고-쓰기의 '읽기'. 같은 레지스터의 다른 필드를 지키기 위함 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG);
	/* [한국어] 레인 선택 필드만 비운다 */
	val &= ~(LANE_SELECT);
	/* [한국어] 거기에 사용자가 고른 번호를 끼워 넣는다 */
	val |= FIELD_PREP(LANE_SELECT, lane);
	/* [한국어] 되쓴다. 이 순간부터 lane_detect/rx_valid 읽기가 이 레인을 비춘다 */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG, val);

	return count;
}

/* [한국어]
 * rx_valid_read - 선택된 레인의 PIPE RX 유효 신호를 보여 준다
 *
 * @file: 열린 debugfs 파일. private_data 는 struct dw_pcie.
 * @buf: 사용자 공간 버퍼.
 * @count: 요청 바이트 수.
 * @ppos: 파일 오프셋.
 * @return: 복사한 바이트 수.
 *
 * lane_detect_read() 와 같은 레지스터의 다른 비트를 본다. 레인 검출이 "상대가
 * 물리적으로 있는가" 라면, 이쪽은 "그 레인에서 유효한 심볼이 실제로 들어오고 있는가" 다.
 * 검출은 되는데 RX 가 유효하지 않다면 이퀄라이제이션이나 신호 품질 쪽을 의심하게 된다.
 *
 * 보는 레인은 마찬가지로 LANE_SELECT 가 정하며, rx_valid_write() 로 고른다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트. 잠금 없음.
 *
 * 호출 체인:
 *   사용자 공간 read() → dbg_rx_valid_fops.read → [이 함수]
 *     → dw_pcie_readl_dbi(), simple_read_from_buffer()
 */
static ssize_t rx_valid_read(struct file *file, char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct dw_pcie *pci = file->private_data;
	/* [한국어] RAS DES 블록 시작 오프셋 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] 결과 문자열 버퍼 */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];
	/* [한국어] 문자열 길이 */
	ssize_t pos;
	/* [한국어] 레지스터 원시 값 */
	u32 val;

	/* [한국어] lane_detect_read 와 같은 레지스터를 읽는다. 보는 비트만 다르다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + SD_STATUS_L1LANE_REG);
	/* [한국어] PIPE RX 유효 비트만 뽑는다 */
	val = FIELD_GET(PIPE_RXVALID, val);
	/* [한국어] 유효한 심볼이 들어오고 있음 */
	if (val)
		/* [한국어] 정상 상태 */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "RX Valid\n");
	else
		/* [한국어] 검출은 되는데 RX 가 무효라면 신호 품질이나 이퀄라이제이션 쪽을 의심한다 */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "RX Invalid\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
}

/* [한국어]
 * rx_valid_write - 레인 선택을 lane_detect_write 에 그대로 넘긴다
 *
 * @file: 열린 debugfs 파일.
 * @buf: 사용자가 쓴 문자열.
 * @count: 그 길이.
 * @ppos: 파일 오프셋.
 * @return: lane_detect_write() 의 반환값.
 *
 * 두 파일이 같은 레지스터의 같은 LANE_SELECT 필드를 공유하므로 구현을 나눌 이유가 없다.
 * 그런데도 별도 함수를 두는 것은 DWC_DEBUGFS_FOPS() 매크로가 <이름>_read 와
 * <이름>_write 를 토큰 결합으로 만들어 내기 때문이다 — 이름이 맞아야 매크로가 성립한다.
 * 즉 이 한 줄짜리 래퍼는 매크로의 이름 규약을 만족시키려고 존재한다.
 *
 * 부작용으로 rx_valid 에 레인을 쓰면 lane_detect 가 보는 레인도 같이 바뀐다.
 * 하드웨어가 선택자를 하나만 두었으니 피할 수 없는 성질이다.
 *
 * 실행 컨텍스트: 사용자 write 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 write() → dbg_rx_valid_fops.write → [이 함수] → lane_detect_write()
 */
static ssize_t rx_valid_write(struct file *file, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	return lane_detect_write(file, buf, count, ppos);
}

/* [한국어]
 * err_inj_write - 링크에 특정 오류를 지정한 횟수만큼 심는다
 *
 * @file: 열린 debugfs 파일. private_data 는 struct dwc_pcie_rasdes_priv 이고,
 *        그 안의 idx 가 err_inj_list[] 의 몇 번째 오류인지를 가리킨다.
 * @buf: 사용자가 쓴 문자열. 그룹에 따라 숫자 1~3개.
 * @count: 그 길이.
 * @ppos: 파일 오프셋. 쓰이지 않는다.
 * @return: 성공하면 count, 파싱/범위 오류면 -EINVAL 이나 kstrtou32 의 오류.
 *
 * 오류 주입은 상대편 장치와 링크 복구 논리를 시험하는 수단이다. 예컨대 tx_lcrc 에
 * 10 을 쓰면 다음 10개의 TLP 에 잘못된 링크 CRC 가 실려 나가고, 상대는 그것을
 * 버리고 재전송을 요구해야 정상이다.
 *
 * 이 함수의 복잡도는 그룹마다 필요한 인자가 다르다는 데서 온다.
 * 그룹 4(플로우 컨트롤 크레딧)는 "횟수, 값 차이, VC 번호" 세 개,
 * 그룹 1(시퀀스 번호)은 "횟수, 값 차이" 두 개, 나머지는 "횟수" 하나다.
 * 값 차이는 부호 있는 -4095~4095 로, 13비트 필드에 담기는 범위다.
 *
 * 레지스터 조립은 세 단계다. 먼저 그 그룹의 제어 레지스터를 읽어 타입과 횟수
 * 필드를 비우고, 표에서 가져온 오류 종류를 EINJ_TYPE_SHIFT 만큼 올려 그룹별
 * 마스크로 자른 뒤 넣고, 횟수를 넣는다. 그룹에 따라 값 차이와 VC 번호를 더 채운 다음
 * 되쓰고, 마지막으로 활성화 레지스터의 그 그룹 비트를 세워 주입을 시작한다.
 *
 * 실행 컨텍스트: 사용자 write 의 프로세스 컨텍스트. memdup_user_nul 이 잠들 수 있다.
 * 에러 경로: 파싱 실패 시 kern_buf 를 반드시 kfree 하고 나가야 한다 — 그래서 각
 * 실패 지점마다 kfree 가 반복된다.
 *
 * 호출 체인:
 *   사용자 공간 write() → dwc_pcie_err_inj_ops.write → [이 함수]
 *     → memdup_user_nul(), sscanf()/kstrtou32(), dw_pcie_readl_dbi()/writel_dbi()
 */
static ssize_t err_inj_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;
	/* [한국어] private 데이터에서 컨트롤러를 꺼낸다 */
	struct dw_pcie *pci = pdata->pci;
	/* [한국어] RAS DES 블록 시작 오프셋 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] counter 는 주입 횟수, vc_num 은 가상 채널 번호, err_group 은 그룹, type_mask 는 그룹별 타입 필드 마스크 */
	u32 val, counter, vc_num, err_group, type_mask;
	/* [한국어] 값 차이. 부호 있는 값이라 int 이고, 안 쓰는 그룹을 위해 0 으로 초기화한다 */
	int val_diff = 0;
	/* [한국어] 사용자 문자열을 커널 쪽으로 복사해 담을 버퍼 */
	char *kern_buf;

	/* [한국어] 이 파일이 어떤 오류인지는 idx 로 표를 되짚어 알아낸다. 파일 이름을 파싱하지 않는다 */
	err_group = err_inj_list[pdata->idx].err_inj_group;
	/* [한국어] 그룹 번호가 곧 마스크 표의 인덱스다. 그룹마다 타입 필드 폭이 1~4비트로 달라 이 표가 필요하다 */
	type_mask = err_inj_type_mask[err_group];

	/* [한국어] 사용자 버퍼를 복사하며 끝에 NUL 을 붙여 준다. 아래 sscanf 가 문자열을 요구하기 때문 */
	kern_buf = memdup_user_nul(buf, count);
	/* [한국어] 복사 실패는 오류 포인터로 온다 */
	if (IS_ERR(kern_buf))
		return PTR_ERR(kern_buf);

	/* [한국어] 그룹 4(플로우 컨트롤 크레딧)는 인자가 세 개다 — 횟수, 값 차이, VC 번호 */
	if (err_group == 4) {
		/* [한국어] %u %d %u 로 파싱. 가운데만 %d 인 것은 값 차이가 부호 있는 값이기 때문 */
		val = sscanf(kern_buf, "%u %d %u", &counter, &val_diff, &vc_num);
		/* [한국어] 세 개를 다 받았는지, 값 차이가 13비트 부호 범위(-4095~4095) 안인지 함께 검사 */
		if ((val != 3) || (val_diff < -4095 || val_diff > 4095)) {
			kfree(kern_buf);
			return -EINVAL;
		}
	/* [한국어] 그룹 1(시퀀스 번호)은 인자가 두 개다 — 횟수, 값 차이. VC 개념이 없다 */
	} else if (err_group == 1) {
		/* [한국어] %u %d 로 파싱 */
		val = sscanf(kern_buf, "%u %d", &counter, &val_diff);
		/* [한국어] 두 개를 다 받았는지와 범위를 검사 */
		if ((val != 2) || (val_diff < -4095 || val_diff > 4095)) {
			kfree(kern_buf);
			return -EINVAL;
		}
	} else {
		/* [한국어] 나머지 그룹은 횟수 하나뿐이라 sscanf 없이 정수 변환으로 충분하다 */
		val = kstrtou32(kern_buf, 0, &counter);
		/* [한국어] 변환 실패 */
		if (val) {
			kfree(kern_buf);
			/* [한국어] kstrtou32 의 오류 코드를 그대로 돌려준다 */
			return val;
		}
	}

	/* [한국어] 그룹 n 의 제어 레지스터를 읽는다. 그룹당 4바이트씩 떨어져 있어 0x34 + 4*n 이다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + ERR_INJ0_OFF + (0x4 * err_group));
	/* [한국어] 타입과 횟수 필드를 비운다. 나머지 비트는 아래에서 그룹에 따라 손본다 */
	val &= ~(type_mask | EINJ_COUNT);
	/* [한국어] 표의 오류 종류를 공통 시작 자리(비트 8)로 올린 뒤 그룹별 마스크로 잘라 넣는다.
	 * FIELD_PREP 를 못 쓰는 것은 마스크가 컴파일 타임 상수가 아니라 변수여서다 */
	val |= ((err_inj_list[pdata->idx].err_inj_type << EINJ_TYPE_SHIFT) & type_mask);
	/* [한국어] 몇 번 심을지. 하드웨어가 이 횟수만큼 오류를 낸 뒤 스스로 멈춘다 */
	val |= FIELD_PREP(EINJ_COUNT, counter);

	/* [한국어] 값 차이를 쓰는 두 그룹 */
	if (err_group == 1 || err_group == 4) {
		/* [한국어] 값 차이 필드를 비우고 */
		val &= ~(EINJ_VAL_DIFF);
		/* [한국어] 부호 있는 값을 그대로 넣는다. 13비트 필드라 음수는 2의 보수로 실린다 */
		val |= FIELD_PREP(EINJ_VAL_DIFF, val_diff);
	}
	/* [한국어] VC 번호는 그룹 4 만 쓴다 */
	if (err_group == 4) {
		/* [한국어] VC 필드를 비우고 */
		val &= ~(EINJ_VC_NUM);
		/* [한국어] 사용자가 고른 가상 채널 번호를 넣는다 */
		val |= FIELD_PREP(EINJ_VC_NUM, vc_num);
	}

	/* [한국어] 조립한 값을 그룹 n 의 제어 레지스터에 쓴다. 아직 주입은 시작되지 않는다 */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + ERR_INJ0_OFF + (0x4 * err_group), val);
	/* [한국어] 활성화 레지스터의 그룹 n 비트를 세워 주입을 시작한다. 설정과 시작을 나눈 구조라
	 * 제어 레지스터를 다 채운 뒤에 이 한 줄로 방아쇠를 당긴다 */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + ERR_INJ_ENABLE_REG, (0x1 << err_group));

	kfree(kern_buf);
	return count;
}

/* [한국어]
 * set_event_number - 이벤트 카운터 창이 어느 카운터를 비추게 할지 고른다
 *
 * @pdata: 이 debugfs 파일의 private 데이터. idx 로 event_list[] 를 되짚는다.
 * @pci: 대상 컨트롤러.
 * @rinfo: RAS DES 공통 정보. capability 오프셋을 얻는다.
 * @return: 없음.
 *
 * 이 파일 전체에서 가장 중요한 함수다. RAS DES 의 이벤트 카운터는 80여 종이지만
 * 레지스터는 제어 하나와 데이터 하나뿐이다 — 그림자(shadow) 구조로, 제어
 * 레지스터에 "그룹 몇, 이벤트 몇" 을 써서 창을 돌린 뒤에야 데이터 레지스터가 그
 * 카운터의 값을 비춘다. 따라서 카운터를 읽거나 조작하는 모든 경로가 반드시
 * 이 함수를 먼저 불러야 한다.
 *
 * ENABLE 필드까지 함께 지우는 것이 중요하다. 그러지 않으면 창을 돌리는 이 쓰기가
 * 직전에 남아 있던 켜기/끄기 명령을 새로 선택한 카운터에 다시 적용해 버린다.
 *
 * 이 함수 자체는 잠금을 잡지 않는다. 대신 호출자 다섯 곳이 모두
 * rinfo->reg_event_lock 을 잡은 채 부르고, 창을 돌린 뒤의 접근까지 그 잠금 안에서 끝낸다.
 * 잠금을 이 함수 안에 두면 "선택" 과 "접근" 이 갈라져 보호가 깨지므로 밖에 두는 것이다.
 *
 * 실행 컨텍스트: 사용자 read/write 의 프로세스 컨텍스트, reg_event_lock 을 쥔 상태.
 *
 * 호출 체인:
 *   counter_enable_read/write, counter_lane_read/write, counter_value_read
 *     → [이 함수] → dw_pcie_readl_dbi(), dw_pcie_writel_dbi()
 */
static void set_event_number(struct dwc_pcie_rasdes_priv *pdata,
			     struct dw_pcie *pci, struct dwc_pcie_rasdes_info *rinfo)
{
	u32 val;

	/* [한국어] 이벤트 카운터 제어 레지스터를 읽는다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
	/* [한국어] 켜기/끄기 명령 필드를 지운다. 그러지 않으면 창을 돌리는 이 쓰기가
	 * 직전 명령을 새로 선택한 카운터에 다시 적용해 버린다 */
	val &= ~EVENT_COUNTER_ENABLE;
	/* [한국어] 그룹과 이벤트 선택 필드를 비운다 */
	val &= ~(EVENT_COUNTER_GROUP_SELECT | EVENT_COUNTER_EVENT_SELECT);
	/* [한국어] 이 파일이 담당하는 이벤트의 그룹 번호를 넣는다 */
	val |= FIELD_PREP(EVENT_COUNTER_GROUP_SELECT, event_list[pdata->idx].group_no);
	/* [한국어] 같은 이벤트의 번호를 넣는다. 이 둘이 짝을 이뤄 카운터 하나를 지목한다 */
	val |= FIELD_PREP(EVENT_COUNTER_EVENT_SELECT, event_list[pdata->idx].event_no);
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG, val);
}

/* [한국어]
 * counter_enable_read - 이 이벤트 카운터가 켜져 있는지 보여 준다
 *
 * @file: 열린 debugfs 파일. private_data 는 dwc_pcie_rasdes_priv.
 * @buf: 사용자 공간 버퍼.
 * @count: 요청 바이트 수.
 * @ppos: 파일 오프셋.
 * @return: 복사한 바이트 수.
 *
 * 창을 이 이벤트로 돌린 뒤 제어 레지스터의 STATUS 비트를 읽는다. STATUS 는 명령
 * 필드(ENABLE)와 별개인 읽기 전용 상태라, "켜라고 썼는데 실제로 켜졌는가" 를
 * 확인할 수 있는 유일한 통로다.
 *
 * 선택과 읽기 두 단계를 reg_event_lock 으로 묶는 것이 이 함수의 핵심이다.
 * 잠금 없이 하면 그 사이 다른 프로세스가 다른 이벤트를 선택해 엉뚱한 카운터의
 * 상태를 읽게 된다. 잠금은 레지스터 읽기까지만 잡고, 문자열 포맷과 사용자 복사는
 * 잠금 밖에서 한다 — 복사 중 페이지 폴트로 잠들 수 있어 임계 구역을 짧게 두는 것이다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   사용자 공간 read() → dwc_pcie_counter_enable_ops.read → [이 함수]
 *     → set_event_number(), dw_pcie_readl_dbi(), simple_read_from_buffer()
 */
static ssize_t counter_enable_read(struct file *file, char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;
	/* [한국어] private 데이터에서 컨트롤러를 꺼낸다 */
	struct dw_pcie *pci = pdata->pci;
	/* [한국어] RAS DES 블록 시작 오프셋과 뮤텍스가 여기 있다 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] 결과 문자열 버퍼 */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];
	/* [한국어] 문자열 길이 */
	ssize_t pos;
	/* [한국어] 레지스터 원시 값 */
	u32 val;

	mutex_lock(&rinfo->reg_event_lock);
	/* [한국어] 창을 이 이벤트로 돌린다. 잠금 안에서 해야 다른 프로세스가 끼어들지 못한다 */
	set_event_number(pdata, pci, rinfo);
	/* [한국어] 돌린 창으로 제어 레지스터를 읽는다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
	mutex_unlock(&rinfo->reg_event_lock);
	/* [한국어] 켜짐 상태 비트를 뽑는다. 문자열 포맷은 잠금 밖에서 — 임계 구역을 짧게 두려는 것 */
	val = FIELD_GET(EVENT_COUNTER_STATUS, val);
	/* [한국어] 켜져 있음 */
	if (val)
		/* [한국어] 이 카운터가 이벤트를 세고 있다는 뜻 */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Counter Enabled\n");
	else
		/* [한국어] 꺼져 있거나 이 플랫폼이 지원하지 않는 이벤트 */
		pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Counter Disabled\n");

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
}

/* [한국어]
 * counter_enable_write - 이 이벤트 카운터를 켜거나 끈다
 *
 * @file: 열린 debugfs 파일. private_data 는 dwc_pcie_rasdes_priv.
 * @buf: 사용자가 쓴 숫자. 0 이 아니면 켜기, 0 이면 끄기.
 * @count: 그 길이.
 * @ppos: 파일 오프셋. 쓰이지 않는다.
 * @return: 성공하면 count, 파싱 실패면 그 오류, 켜기가 먹히지 않으면 -EOPNOTSUPP.
 *
 * 창을 이 이벤트로 돌린 뒤 ENABLE 필드에 PER_EVENT_ON/OFF 명령을 쓴다.
 * "PER_EVENT" 라는 이름대로 선택된 이벤트 하나에만 적용되는 명령이다.
 *
 * 켜기일 때 상태를 되읽어 확인하는 것이 이 함수의 요점이고, 원문 주석도 그것을
 * 강조한다. RAS DES 에 정의된 이벤트라고 해서 모든 IP 구성이 그것을 실제로 세는
 * 것은 아니다. 지원하지 않는 이벤트는 켜라고 써도 STATUS 가 서지 않으므로,
 * 되읽어 보고 -EOPNOTSUPP 를 돌려 주어 사용자가 "이 플랫폼에서는 안 된다" 를 알게 한다.
 *
 * 실행 컨텍스트: 사용자 write 의 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다.
 * 에러 경로: 미지원으로 빠져나갈 때에도 반드시 뮤텍스를 풀어야 한다 —
 * 그래서 그 분기 안에 mutex_unlock 이 따로 들어 있다.
 *
 * 호출 체인:
 *   사용자 공간 write() → dwc_pcie_counter_enable_ops.write → [이 함수]
 *     → kstrtou32_from_user(), set_event_number(), DBI 읽기/쓰기
 */
static ssize_t counter_enable_write(struct file *file, const char __user *buf,
				    size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;
	/* [한국어] private 데이터에서 컨트롤러를 꺼낸다 */
	struct dw_pcie *pci = pdata->pci;
	/* [한국어] RAS DES 블록 시작 오프셋과 뮤텍스 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] val 은 조립할 레지스터 값, enable 은 사용자가 쓴 켜기/끄기 */
	u32 val, enable;
	/* [한국어] 파싱 결과 */
	int ret;

	/* [한국어] 사용자가 쓴 숫자를 읽는다. 0 이면 끄기, 그 외는 켜기 */
	ret = kstrtou32_from_user(buf, count, 0, &enable);
	/* [한국어] 숫자가 아니면 그 오류를 돌려준다 */
	if (ret)
		return ret;

	mutex_lock(&rinfo->reg_event_lock);
	/* [한국어] 창을 이 이벤트로 돌린다 */
	set_event_number(pdata, pci, rinfo);
	/* [한국어] 제어 레지스터를 읽는다. set_event_number 가 ENABLE 을 지워 둔 상태다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
	/* [한국어] 켜기 요청 */
	if (enable)
		/* [한국어] 선택된 이벤트 하나만 켜라는 명령 */
		val |= FIELD_PREP(EVENT_COUNTER_ENABLE, PER_EVENT_ON);
	else
		/* [한국어] 선택된 이벤트 하나만 꺼라는 명령 */
		val |= FIELD_PREP(EVENT_COUNTER_ENABLE, PER_EVENT_OFF);

	/* [한국어] 명령을 쓴다. 명령 필드와 상태 필드가 별개라 이 쓰기가 곧 성공을 뜻하지는 않는다 */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG, val);

	/*
	 * While enabling the counter, always read the status back to check if
	 * it is enabled or not. Return error if it is not enabled to let the
	 * users know that the counter is not supported on the platform.
	 */
	if (enable) {
		/* [한국어] 켜졌는지 확인하려고 되읽는다 */
		val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset +
					RAS_DES_EVENT_COUNTER_CTRL_REG);
		/* [한국어] 상태 비트가 서지 않았다면 이 IP 구성이 그 이벤트를 세지 않는다는 뜻이다 */
		if (!FIELD_GET(EVENT_COUNTER_STATUS, val)) {
			mutex_unlock(&rinfo->reg_event_lock);
			return -EOPNOTSUPP;
		}
	}

	mutex_unlock(&rinfo->reg_event_lock);

	return count;
}

/* [한국어]
 * counter_lane_read - 이 카운터가 현재 몇 번 레인을 세고 있는지 보여 준다
 *
 * @file: 열린 debugfs 파일. private_data 는 dwc_pcie_rasdes_priv.
 * @buf: 사용자 공간 버퍼.
 * @count: 요청 바이트 수.
 * @ppos: 파일 오프셋.
 * @return: 복사한 바이트 수.
 *
 * 이 파일은 모든 이벤트에 생기지 않는다. 초기화 코드가 그룹 0(물리 계층 오류)과
 * 그룹 4(엘라스틱 버퍼 SKP 조정)에만 lane_select 를 만드는데, 그 두 그룹만
 * 레인별로 따로 세기 때문이다. 나머지 그룹의 이벤트는 링크 전체 단위라
 * 레인을 고를 필요가 없다.
 *
 * 앞의 lane_detect 계열이 쓰는 LANE_SELECT 와는 다른 레지스터의 다른 필드다.
 * 그쪽은 Silicon Debug 의 물리 상태 창이고, 이쪽은 이벤트 카운터의 레인 선택이다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트. reg_event_lock 을 잡는다.
 *
 * 호출 체인:
 *   사용자 공간 read() → dwc_pcie_counter_lane_ops.read → [이 함수]
 *     → set_event_number(), dw_pcie_readl_dbi(), simple_read_from_buffer()
 */
static ssize_t counter_lane_read(struct file *file, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;
	/* [한국어] private 데이터에서 컨트롤러를 꺼낸다 */
	struct dw_pcie *pci = pdata->pci;
	/* [한국어] RAS DES 블록 시작 오프셋과 뮤텍스 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] 결과 문자열 버퍼 */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];
	/* [한국어] 문자열 길이 */
	ssize_t pos;
	/* [한국어] 레지스터 원시 값 */
	u32 val;

	mutex_lock(&rinfo->reg_event_lock);
	/* [한국어] 창을 이 이벤트로 돌린다 */
	set_event_number(pdata, pci, rinfo);
	/* [한국어] 제어 레지스터를 읽는다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
	mutex_unlock(&rinfo->reg_event_lock);
	/* [한국어] 이벤트 카운터의 레인 선택 필드를 뽑는다. 앞의 lane_detect 가 쓰는 것과는 다른 레지스터의 다른 필드다 */
	val = FIELD_GET(EVENT_COUNTER_LANE_SELECT, val);
	/* [한국어] 레인 번호를 문자열로 만든다 */
	pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Lane: %d\n", val);

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
}

/* [한국어]
 * counter_lane_write - 이 카운터가 셀 레인 번호를 바꾼다
 *
 * @file: 열린 debugfs 파일. private_data 는 dwc_pcie_rasdes_priv.
 * @buf: 사용자가 쓴 레인 번호.
 * @count: 그 길이.
 * @ppos: 파일 오프셋. 쓰이지 않는다.
 * @return: 성공하면 count, 파싱 실패면 그 오류.
 *
 * 창을 이 이벤트로 돌린 뒤 LANE_SELECT 필드만 갈아 끼운다. 읽고-고치고-쓰기가
 * 필요한 이유는 같은 레지스터에 그룹/이벤트 선택과 켜기 상태가 함께 있어서다 —
 * 통째로 쓰면 방금 set_event_number 가 고른 선택이 지워진다.
 *
 * 선택부터 쓰기까지 전 구간이 reg_event_lock 안에 있다. 여기서는 읽기 경로보다
 * 임계 구역이 긴데, 창 돌리기·읽기·수정·쓰기 네 단계가 모두 한 덩어리여야 하기 때문이다.
 *
 * 실행 컨텍스트: 사용자 write 의 프로세스 컨텍스트. 뮤텍스를 잡으므로 잠들 수 있다.
 *
 * 호출 체인:
 *   사용자 공간 write() → dwc_pcie_counter_lane_ops.write → [이 함수]
 *     → kstrtou32_from_user(), set_event_number(), DBI 읽기/쓰기
 */
static ssize_t counter_lane_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;
	/* [한국어] private 데이터에서 컨트롤러를 꺼낸다 */
	struct dw_pcie *pci = pdata->pci;
	/* [한국어] RAS DES 블록 시작 오프셋과 뮤텍스 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] val 은 고칠 레지스터 값, lane 은 사용자가 고른 레인 */
	u32 val, lane;
	/* [한국어] 파싱 결과 */
	int ret;

	/* [한국어] 사용자가 쓴 레인 번호를 읽는다 */
	ret = kstrtou32_from_user(buf, count, 0, &lane);
	/* [한국어] 숫자가 아니면 그 오류를 돌려준다 */
	if (ret)
		return ret;

	mutex_lock(&rinfo->reg_event_lock);
	/* [한국어] 창을 이 이벤트로 돌린다 */
	set_event_number(pdata, pci, rinfo);
	/* [한국어] 제어 레지스터를 읽는다. 방금 돌린 선택을 보존하려면 통째로 쓰지 말고 읽고 고쳐야 한다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG);
	/* [한국어] 레인 선택 필드만 비운다 */
	val &= ~(EVENT_COUNTER_LANE_SELECT);
	/* [한국어] 사용자가 고른 번호를 끼워 넣는다 */
	val |= FIELD_PREP(EVENT_COUNTER_LANE_SELECT, lane);
	/* [한국어] 되쓴다. 창 돌리기부터 여기까지가 한 덩어리라 전 구간이 잠금 안에 있다 */
	dw_pcie_writel_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_CTRL_REG, val);
	mutex_unlock(&rinfo->reg_event_lock);

	return count;
}

/* [한국어]
 * counter_value_read - 이 이벤트가 지금까지 몇 번 일어났는지 읽는다
 *
 * @file: 열린 debugfs 파일. private_data 는 dwc_pcie_rasdes_priv.
 * @buf: 사용자 공간 버퍼.
 * @count: 요청 바이트 수.
 * @ppos: 파일 오프셋.
 * @return: 복사한 바이트 수.
 *
 * RAS DES 통계의 실질적인 목적지다. 창을 이 이벤트로 돌린 뒤 데이터 레지스터를
 * 한 번 읽으면 그 카운터의 누적값이 나온다. 카운터가 켜져 있지 않으면 값이 늘지
 * 않으므로, 보통 counter_enable 에 1 을 쓰고 부하를 준 다음 여기를 읽는 순서로 쓴다.
 *
 * 파일 모드가 0444(읽기 전용)인 것은 카운터 값을 소프트웨어가 임의로 쓸 수 없기
 * 때문이다. 초기화는 카운터를 껐다 켜는 것으로 한다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트. reg_event_lock 을 잡는다.
 *
 * 호출 체인:
 *   사용자 공간 read() → dwc_pcie_counter_value_ops.read → [이 함수]
 *     → set_event_number(), dw_pcie_readl_dbi(), simple_read_from_buffer()
 */
static ssize_t counter_value_read(struct file *file, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct dwc_pcie_rasdes_priv *pdata = file->private_data;
	/* [한국어] private 데이터에서 컨트롤러를 꺼낸다 */
	struct dw_pcie *pci = pdata->pci;
	/* [한국어] RAS DES 블록 시작 오프셋과 뮤텍스 */
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;
	/* [한국어] 결과 문자열 버퍼 */
	char debugfs_buf[DWC_DEBUGFS_BUF_MAX];
	/* [한국어] 문자열 길이 */
	ssize_t pos;
	/* [한국어] 읽은 카운터 값 */
	u32 val;

	mutex_lock(&rinfo->reg_event_lock);
	/* [한국어] 창을 이 이벤트로 돌린다 */
	set_event_number(pdata, pci, rinfo);
	/* [한국어] 데이터 레지스터를 읽는다. 창이 돌아간 뒤라야 이 자리에 원하는 카운터의 값이 비친다 */
	val = dw_pcie_readl_dbi(pci, rinfo->ras_cap_offset + RAS_DES_EVENT_COUNTER_DATA_REG);
	mutex_unlock(&rinfo->reg_event_lock);
	/* [한국어] 누적 횟수를 문자열로 만든다 */
	pos = scnprintf(debugfs_buf, DWC_DEBUGFS_BUF_MAX, "Counter value: %d\n", val);

	return simple_read_from_buffer(buf, count, ppos, debugfs_buf, pos);
}

/* [한국어]
 * ltssm_status_show - 링크 트레이닝 상태 기계의 현재 상태를 문자열로 찍는다
 *
 * @s: seq_file 출력 스트림. private 에 struct dw_pcie 가 들어 있다.
 * @v: seq_file 반복자. 단일 항목이라 쓰지 않는다.
 * @return: 항상 0.
 *
 * LTSSM(Link Training and Status State Machine)은 PCIe 링크가 Detect → Polling →
 * Configuration → L0 순으로 올라가는 상태 기계다. 링크가 붙지 않을 때 어느 단계에서
 * 멈췄는지가 곧 원인의 실마리이므로, 이 한 줄이 링크 디버깅의 첫 관문이 된다.
 *
 * 상태 값을 어디서 읽는지는 SoC 마다 다르다 — 표준 레지스터에 있는 경우도 있고
 * 벤더 전용 레지스터에 있는 경우도 있어, dw_pcie_get_ltssm() 이 글루의 훅과
 * 기본 구현 사이를 가른다. 그 값을 사람이 읽는 이름으로 바꾸는 것이
 * dw_pcie_ltssm_status_string() 이다. 숫자도 함께 찍는 것은 문자열 표에 없는
 * 값이 나올 때를 대비한 것이다.
 *
 * 다른 파일들과 달리 seq_file 방식을 쓰는 것은 출력이 한 줄로 끝나 고정 버퍼를
 * 굴릴 이유가 없어서다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트. 잠금 없음 — 읽기 한 번이다.
 *
 * 호출 체인:
 *   사용자 공간 read() → seq_read() → [이 함수]
 *     → dw_pcie_get_ltssm(), dw_pcie_ltssm_status_string()
 */
static int ltssm_status_show(struct seq_file *s, void *v)
{
	struct dw_pcie *pci = s->private;
	/* [한국어] LTSSM 상태를 담을 열거형 변수 */
	enum dw_pcie_ltssm val;

	/* [한국어] 현재 상태를 읽는다. 어느 레지스터에서 읽을지는 SoC 마다 달라 이 함수가 글루 훅과 기본 구현을 가른다 */
	val = dw_pcie_get_ltssm(pci);
	/* [한국어] 이름과 숫자를 함께 찍는다. 숫자도 내는 것은 문자열 표에 없는 값이 나올 때를 대비한 것이다 */
	seq_printf(s, "%s (0x%02x)\n", dw_pcie_ltssm_status_string(val), val);

	return 0;
}

/* [한국어]
 * ltssm_status_open - ltssm_status 파일을 seq_file 로 연다
 *
 * @inode: 이 debugfs 파일의 inode. i_private 에 debugfs_create_file 이 넣어 둔
 *         struct dw_pcie 포인터가 들어 있다.
 * @file: 열릴 파일 객체.
 * @return: single_open() 의 결과. 0 성공, -ENOMEM 등 실패.
 *
 * single_open() 은 "항목이 하나뿐인 seq_file" 을 만드는 표준 헬퍼다. 세 번째 인자로
 * 넘긴 i_private 가 show 함수의 s->private 로 전달되므로, 이 한 줄이 곧
 * "이 파일이 어느 컨트롤러의 것인지" 를 show 함수에 전달하는 통로다.
 *
 * 다른 파일들이 쓰는 simple_open 은 inode->i_private 를 file->private_data 로
 * 그대로 옮기는데, seq_file 은 그 자리에 자기 상태를 두어야 해서 이 전용 open 이 필요하다.
 *
 * 실행 컨텍스트: 사용자 open 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 open() → dwc_pcie_ltssm_status_ops.open → [이 함수] → single_open()
 */
static int ltssm_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, ltssm_status_show, inode->i_private);
}

/* [한국어] rasdes_debug 디렉토리에 파일 하나를 만드는 편의 매크로.
 * #name 으로 이름을 문자열화하고, name ## _fops 로 그 파일의 연산 테이블 이름을
 * 만들어 낸다. 즉 dwc_debugfs_create(lane_detect) 한 줄이
 * debugfs_create_file("lane_detect", 0644, rasdes_debug, pci, &dbg_lane_detect_fops)
 * 로 펼쳐진다. 이 매크로가 rasdes_debug 와 pci 라는 이름을 그대로 참조하므로,
 * 호출부(dwc_pcie_rasdes_debugfs_init)에 그 이름의 지역 변수가 반드시 있어야 한다.
 * 모드 0644 는 읽기와 쓰기를 모두 허용한다는 뜻으로, 이 두 파일이 읽기는 상태 조회,
 * 쓰기는 레인 선택이라는 서로 다른 의미를 갖기 때문이다. */
#define dwc_debugfs_create(name)			\
debugfs_create_file(#name, 0644, rasdes_debug, pci,	\
			&dbg_ ## name ## _fops)

/* [한국어] 읽기와 쓰기를 모두 갖는 debugfs 파일의 연산 테이블을 찍어 내는 매크로.
 * 토큰 결합으로 <이름>_read 와 <이름>_write 함수를 찾아 꽂으므로, 이 매크로를 쓰려면
 * 그 두 함수가 정확히 그 이름으로 존재해야 한다 — rx_valid_write() 가 하는 일이
 * lane_detect_write() 를 되부르는 것뿐인데도 따로 정의된 이유가 이것이다.
 * simple_open 은 inode->i_private 를 file->private_data 로 옮기기만 하는 표준 헬퍼로,
 * 그 덕에 핸들러들이 file->private_data 에서 곧바로 struct dw_pcie 를 꺼낼 수 있다. */
#define DWC_DEBUGFS_FOPS(name)					\
static const struct file_operations dbg_ ## name ## _fops = {	\
	.open = simple_open,				\
	.read = name ## _read,				\
	.write = name ## _write				\
}

/* [한국어] 레인 검출 파일의 연산 테이블(dbg_lane_detect_fops)을 만든다. */
DWC_DEBUGFS_FOPS(lane_detect);
/* [한국어] RX 유효 파일의 연산 테이블(dbg_rx_valid_fops)을 만든다. */
DWC_DEBUGFS_FOPS(rx_valid);

/* [한국어] 오류 주입 파일 30개가 공유하는 연산 테이블.
 * read 가 없는 것이 특징인데, 주입은 명령일 뿐 되읽을 상태가 없기 때문이다.
 * 그래서 파일 모드도 0200(쓰기 전용)으로 만든다. 파일마다 다른 것은 연산 테이블이
 * 아니라 private 데이터의 idx 값이고, 그것으로 어떤 오류인지가 갈린다. */
static const struct file_operations dwc_pcie_err_inj_ops = {
	/* [한국어] i_private(파일별 dwc_pcie_rasdes_priv)를 file->private_data 로 옮긴다. */
	.open = simple_open,
	/* [한국어] 사용자가 쓴 숫자를 파싱해 해당 오류를 그 횟수만큼 심는다. */
	.write = err_inj_write,
};

/* [한국어] 이벤트 디렉토리마다 생기는 counter_enable 파일의 연산 테이블.
 * 읽기와 쓰기가 모두 있고 의미도 대칭이다 — 읽으면 켜졌는지, 쓰면 켜거나 끈다.
 * DWC_DEBUGFS_FOPS 매크로를 쓰지 않는 것은 함수 이름이 counter_enable_* 이라
 * 매크로 인자로 넘길 이름과 파일 이름이 어긋나서다. */
static const struct file_operations dwc_pcie_counter_enable_ops = {
	/* [한국어] i_private(파일별 dwc_pcie_rasdes_priv)를 file->private_data 로 옮긴다. */
	.open = simple_open,
	/* [한국어] STATUS 비트를 읽어 "Counter Enabled/Disabled" 를 찍는다. */
	.read = counter_enable_read,
	/* [한국어] ENABLE 명령을 쓰고, 켜기일 때는 실제로 켜졌는지 되읽어 확인한다. */
	.write = counter_enable_write,
};

/* [한국어] lane_select 파일의 연산 테이블. 레인 개념이 있는 그룹 0 과 4 의
 * 이벤트에만 이 파일이 생기므로, 이 테이블도 그 이벤트들만 쓴다. */
static const struct file_operations dwc_pcie_counter_lane_ops = {
	/* [한국어] i_private(파일별 dwc_pcie_rasdes_priv)를 file->private_data 로 옮긴다. */
	.open = simple_open,
	/* [한국어] 현재 어느 레인을 세고 있는지 찍는다. */
	.read = counter_lane_read,
	/* [한국어] 셀 레인 번호를 바꾼다. */
	.write = counter_lane_write,
};

/* [한국어] counter_value 파일의 연산 테이블. write 가 없다 — 카운터 값은
 * 하드웨어가 세는 것이라 소프트웨어가 임의로 쓸 수 없기 때문이며,
 * 파일 모드도 0444(읽기 전용)로 만든다. */
static const struct file_operations dwc_pcie_counter_value_ops = {
	/* [한국어] i_private(파일별 dwc_pcie_rasdes_priv)를 file->private_data 로 옮긴다. */
	.open = simple_open,
	/* [한국어] 창을 이 이벤트로 돌린 뒤 데이터 레지스터의 누적값을 찍는다. */
	.read = counter_value_read,
};

/* [한국어] ltssm_status 파일의 연산 테이블. 이 파일만 seq_file 방식을 쓴다 —
 * 출력이 한 줄뿐이라 고정 버퍼를 굴리는 것보다 single_open 이 간단하기 때문이다. */
static const struct file_operations dwc_pcie_ltssm_status_ops = {
	/* [한국어] simple_open 이 아니라 전용 open. seq_file 은 private_data 자리에
	 * 자기 상태를 두어야 해서, single_open 으로 그 연결을 만들어야 한다. */
	.open = ltssm_status_open,
	/* [한국어] seq_file 코어가 제공하는 표준 읽기. 내부적으로 ltssm_status_show 를 부른다. */
	.read = seq_read,
};

/* [한국어]
 * dwc_pcie_rasdes_debugfs_deinit - RAS DES 쪽에서 명시적으로 정리할 것을 되돌린다
 *
 * @pci: 대상 컨트롤러.
 * @return: 없음.
 *
 * 하는 일이 뮤텍스 파괴 하나뿐인데, 그것이 전부인 이유가 이 함수의 설명이다.
 * rasdes_info 와 파일별 priv 는 devm 으로 잡아 디바이스 수명에 묶여 있고, debugfs
 * 파일과 디렉토리는 상위의 debugfs_remove_recursive() 가 한꺼번에 걷는다.
 * 뮤텍스만은 devm 이 알지 못하는 자원이라 손으로 파괴해야 한다(디버그 커널에서
 * 파괴하지 않은 뮤텍스는 경고를 낸다).
 *
 * 두 곳에서 불린다. 정상 해제 경로인 dwc_pcie_debugfs_deinit(), 그리고
 * dwc_pcie_rasdes_debugfs_init() 이 중간에 실패했을 때의 되감기 경로다.
 * 후자에서도 안전한 것은 mutex_init 이 파일 생성 루프보다 먼저 끝나기 때문이다.
 *
 * 실행 컨텍스트: probe 실패 또는 remove 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dwc_pcie_debugfs_deinit() / dwc_pcie_rasdes_debugfs_init() 의 err_deinit
 *     → [이 함수] → mutex_destroy()
 */
static void dwc_pcie_rasdes_debugfs_deinit(struct dw_pcie *pci)
{
	struct dwc_pcie_rasdes_info *rinfo = pci->debugfs->rasdes_info;

	mutex_destroy(&rinfo->reg_event_lock);
}

/* [한국어]
 * dwc_pcie_rasdes_debugfs_init - RAS DES 세 갈래 디렉토리와 그 안의 파일들을 만든다
 *
 * @pci: 대상 컨트롤러.
 * @dir: 이 컨트롤러의 debugfs 최상위 디렉토리. 아래에 세 갈래를 편다.
 * @return: 0 성공(RAS DES 가 없어 아무것도 안 만든 경우도 0), -ENOMEM 이면 할당 실패.
 *
 * 이 파일의 뼈대를 세우는 함수다. RAS DES 라는 이름 자체가 세 갈래를 뜻하므로
 * 디렉토리도 그대로 셋이다 — rasdes_debug(물리 상태 들여다보기),
 * rasdes_err_inj(오류 주입), rasdes_event_counter(통계 카운터).
 *
 * 첫 분기가 가장 중요한 설계 결정이다. RAS DES 는 필수가 아닌 벤더 확장이라
 * 없는 SoC 가 있는데, 그때 오류를 돌려주면 원문 주석대로 멀쩡히 동작하던
 * 플랫폼의 초기화가 깨진다. 그래서 없으면 디버그 로그만 남기고 0 을 돌려준다 —
 * "진단 기능이 없다" 는 것은 오류가 아니라는 판단이다.
 *
 * 파일 구조는 갈래마다 다르다. 디버그 두 파일은 pci 자체를 private 로 쓴다(공유 정보만
 * 필요하다). 오류 주입은 항목마다 파일 하나이고 0200(쓰기 전용)인데, 읽을 것이
 * 없기 때문이다. 이벤트 카운터는 항목마다 디렉토리를 만들고 그 안에
 * counter_value(0444)와 counter_enable(0644)을 두며, 레인 개념이 있는
 * 그룹 0 과 4 에만 lane_select 를 추가한다.
 *
 * 뒤의 두 갈래는 파일마다 priv 구조체를 따로 할당한다. 그 안의 idx 가 표의 몇 번째
 * 항목인지를 담아, 핸들러가 파일 이름을 파싱하지 않고도 자기가 어떤 오류/이벤트인지 안다.
 *
 * 실행 컨텍스트: 드라이버 초기화의 프로세스 컨텍스트. devm 할당으로 잠들 수 있다.
 * 에러 경로: priv 할당이 실패하면 err_deinit 으로 가서 뮤텍스를 파괴하고 오류를 올린다.
 * 이미 만든 debugfs 파일들은 여기서 걷지 않는데, 상위 dwc_pcie_debugfs_init() 이
 * 오류를 로그만 남기고 계속 진행하며 최종 해제는 debugfs_remove_recursive 가 맡기 때문이다.
 *
 * 호출 체인:
 *   dwc_pcie_debugfs_init() → [이 함수]
 *     → dw_pcie_find_rasdes_capability(), debugfs_create_dir/file(), devm_kzalloc()
 */
static int dwc_pcie_rasdes_debugfs_init(struct dw_pcie *pci, struct dentry *dir)
{
	struct dentry *rasdes_debug, *rasdes_err_inj;
	/* [한국어] rasdes_event_counter 는 이벤트 최상위 디렉토리, rasdes_events 는 이벤트 하나의 디렉토리 */
	struct dentry *rasdes_event_counter, *rasdes_events;
	/* [한국어] 컨트롤러 단위 공유 정보. capability 오프셋과 뮤텍스를 담는다 */
	struct dwc_pcie_rasdes_info *rasdes_info;
	/* [한국어] 파일 하나마다 새로 잡을 private 데이터 */
	struct dwc_pcie_rasdes_priv *priv_tmp;
	/* [한국어] devm 할당의 주인이자 로그를 남길 디바이스 */
	struct device *dev = pci->dev;
	/* [한국어] ras_cap 은 찾은 capability 오프셋, i 는 순회 인덱스, ret 는 오류 코드 */
	int ras_cap, i, ret;

	/*
	 * If a given SoC has no RAS DES capability, the following call is
	 * bound to return an error, breaking some existing platforms. So,
	 * return 0 here, as this is not necessarily an error.
	 */
	ras_cap = dw_pcie_find_rasdes_capability(pci);
	/* [한국어] RAS DES 는 필수가 아닌 벤더 확장이라 없는 SoC 가 있다 */
	if (!ras_cap) {
		/* [한국어] 오류가 아니므로 dev_err 가 아니라 dev_dbg 로만 남긴다 */
		dev_dbg(dev, "no RAS DES capability available\n");
		return 0;
	}

	/* [한국어] 컨트롤러 단위 공유 정보를 잡는다. devm 이라 디바이스 수명에 묶인다 */
	rasdes_info = devm_kzalloc(dev, sizeof(*rasdes_info), GFP_KERNEL);
	/* [한국어] 메모리 부족 — 이것 없이는 어떤 파일도 동작할 수 없다 */
	if (!rasdes_info)
		return -ENOMEM;

	/* Create subdirectories for Debug, Error Injection, Statistics. */
	rasdes_debug = debugfs_create_dir("rasdes_debug", dir);
	/* [한국어] 오류 주입 디렉토리 */
	rasdes_err_inj = debugfs_create_dir("rasdes_err_inj", dir);
	/* [한국어] 이벤트 카운터 디렉토리. 세 이름이 곧 RAS DES 의 D, E, S 세 갈래다 */
	rasdes_event_counter = debugfs_create_dir("rasdes_event_counter", dir);

	mutex_init(&rasdes_info->reg_event_lock);
	/* [한국어] 찾은 capability 오프셋을 기록. 이후 모든 레지스터 접근의 기준점이 된다 */
	rasdes_info->ras_cap_offset = ras_cap;
	/* [한국어] 컨트롤러 구조체에 걸어 둔다. 핸들러들이 pci->debugfs->rasdes_info 로 되찾는 경로 */
	pci->debugfs->rasdes_info = rasdes_info;

	/* Create debugfs files for Debug subdirectory. */
	dwc_debugfs_create(lane_detect);
	dwc_debugfs_create(rx_valid);

	/* Create debugfs files for Error Injection subdirectory. */
	for (i = 0; i < ARRAY_SIZE(err_inj_list); i++) {
		/* [한국어] 파일마다 private 데이터를 따로 잡는다. idx 가 파일마다 달라야 하기 때문 */
		priv_tmp = devm_kzalloc(dev, sizeof(*priv_tmp), GFP_KERNEL);
		/* [한국어] 메모리 부족 */
		if (!priv_tmp) {
			ret = -ENOMEM;
			goto err_deinit;
		}

		/* [한국어] 이 파일이 표의 몇 번째 오류인지. 핸들러가 파일 이름 대신 이 값으로 표를 되짚는다 */
		priv_tmp->idx = i;
		/* [한국어] 어느 컨트롤러의 파일인지 */
		priv_tmp->pci = pci;
		/* [한국어] 모드 0200 은 쓰기 전용. 주입은 명령일 뿐 되읽을 상태가 없다 */
		debugfs_create_file(err_inj_list[i].name, 0200, rasdes_err_inj, priv_tmp,
				    &dwc_pcie_err_inj_ops);
	}

	/* Create debugfs files for Statistical Counter subdirectory. */
	for (i = 0; i < ARRAY_SIZE(event_list); i++) {
		/* [한국어] 이벤트도 파일(정확히는 디렉토리)마다 private 데이터를 따로 잡는다 */
		priv_tmp = devm_kzalloc(dev, sizeof(*priv_tmp), GFP_KERNEL);
		/* [한국어] 메모리 부족 */
		if (!priv_tmp) {
			ret = -ENOMEM;
			goto err_deinit;
		}

		/* [한국어] 이 디렉토리가 표의 몇 번째 이벤트인지 */
		priv_tmp->idx = i;
		/* [한국어] 어느 컨트롤러의 것인지 */
		priv_tmp->pci = pci;
		/* [한국어] 이벤트 이름으로 디렉토리를 만든다. 오류 주입과 달리 파일이 아니라 디렉토리인 것은
		 * 한 이벤트에 값/켜기/레인 여러 파일이 딸리기 때문이다 */
		rasdes_events = debugfs_create_dir(event_list[i].name, rasdes_event_counter);
		/* [한국어] 그룹 0(물리 계층)과 그룹 4(엘라스틱 버퍼)만 레인별로 따로 센다 */
		if (event_list[i].group_no == 0 || event_list[i].group_no == 4) {
			/* [한국어] 그래서 그 두 그룹에만 레인 선택 파일을 만든다. 0644 로 읽기와 쓰기 모두 허용 */
			debugfs_create_file("lane_select", 0644, rasdes_events,
					    priv_tmp, &dwc_pcie_counter_lane_ops);
		}
		/* [한국어] 누적값 파일. 0444 인 것은 하드웨어가 세는 값이라 쓸 수 없기 때문 */
		debugfs_create_file("counter_value", 0444, rasdes_events, priv_tmp,
				    &dwc_pcie_counter_value_ops);
		/* [한국어] 켜기/끄기 파일. 0644 로 상태 조회와 명령 모두 허용 */
		debugfs_create_file("counter_enable", 0644, rasdes_events, priv_tmp,
				    &dwc_pcie_counter_enable_ops);
	}

	return 0;

err_deinit:
	dwc_pcie_rasdes_debugfs_deinit(pci);
	return ret;
}

/* [한국어]
 * dwc_pcie_ltssm_debugfs_init - ltssm_status 파일 하나를 만든다
 *
 * @pci: 대상 컨트롤러. 파일의 private 데이터가 된다.
 * @dir: 이 컨트롤러의 debugfs 최상위 디렉토리.
 * @return: 없음.
 *
 * 파일 하나짜리라 함수로 뺄 이유가 없어 보이지만, 상위 init 을 "세 갈래를 각각
 * 편다" 는 구조로 읽히게 하려는 분리다. RAS DES 와 달리 LTSSM 상태는 벤더 확장이
 * 아니라 어느 DWC IP 에나 있으므로 존재 검사 없이 무조건 만든다.
 *
 * 0444(읽기 전용)인 것은 LTSSM 상태가 하드웨어가 정하는 것이지 소프트웨어가
 * 쓸 수 있는 값이 아니기 때문이다. private 로 pci 를 넘기면 그것이
 * inode->i_private → single_open → s->private 순으로 흘러 show 함수에 닿는다.
 *
 * 실행 컨텍스트: 드라이버 초기화의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dwc_pcie_debugfs_init() → [이 함수] → debugfs_create_file()
 */
static void dwc_pcie_ltssm_debugfs_init(struct dw_pcie *pci, struct dentry *dir)
{
	debugfs_create_file("ltssm_status", 0444, dir, pci,
			    &dwc_pcie_ltssm_status_ops);
}

/* [한국어]
 * dw_pcie_ptm_check_capability - PTM 확장이 있는지 확인하고 그 위치를 기억한다
 *
 * @drvdata: PTM 계층이 되돌려 주는 우리 쪽 데이터. 실제로는 struct dw_pcie 다.
 * @return: 찾은 오프셋. 0 이면 PTM 미지원.
 *
 * PCI 코어의 PTM debugfs 계층이 파일을 만들기 전에 가장 먼저 부르는 콜백이다.
 * 0 이 나오면 그쪽이 아무 파일도 만들지 않고 NULL 을 돌려주므로, 이 한 함수가
 * PTM 관련 debugfs 전체의 관문이 된다.
 *
 * 찾은 오프셋을 반환만 하지 않고 pci->ptm_vsec_offset 에 저장하는 것이 요점이다.
 * 아래의 모든 PTM 읽기/쓰기 콜백이 그 필드를 기준으로 레지스터 주소를 만든다.
 * 즉 이 함수가 그 필드의 유일한 설정자다.
 *
 * 인자가 void * 인 것은 PTM 계층이 여러 종류의 컨트롤러를 다루기 때문이고,
 * 그래서 첫 줄에서 struct dw_pcie 로 되돌린다. 아래 콜백들이 모두 같은 형태다.
 *
 * 실행 컨텍스트: 드라이버 초기화의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dwc_pcie_debugfs_init() → pcie_ptm_create_debugfs() → [이 함수]
 *     → dw_pcie_find_ptm_capability()
 */
static int dw_pcie_ptm_check_capability(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	/* [한국어] 찾은 오프셋을 반환만 하지 않고 구조체에 저장한다. 아래 모든 PTM 콜백이 이 필드를
	 * 기준으로 레지스터 주소를 만들므로, 이 한 줄이 그 필드의 유일한 설정자다 */
	pci->ptm_vsec_offset = dw_pcie_find_ptm_capability(pci);

	return pci->ptm_vsec_offset;
}

/* [한국어]
 * dw_pcie_ptm_context_update_write - PTM 시각 문맥 갱신 방식을 자동/수동으로 바꾼다
 *
 * @drvdata: struct dw_pcie.
 * @mode: PCIE_PTM_CONTEXT_UPDATE_AUTO 또는 _MANUAL.
 * @return: 0 성공, -EINVAL 이면 알 수 없는 모드.
 *
 * PTM 은 엔드포인트가 루트 쪽 마스터 시계와 자기 시계의 차이를 주기적으로 재는
 * 구조다. 그 재측정을 하드웨어가 알아서 반복할지(AUTO), 아니면 소프트웨어가
 * 요청할 때 한 번씩 할지(MANUAL)를 이 함수가 정한다.
 *
 * 두 비트가 한 레지스터에 있고 의미가 다르다. AUTO 는 지속 상태를 나타내는
 * 비트라 세우면 계속 켜져 있고, MANUAL 은 그 비트를 내린 뒤 START 비트를 세워
 * 한 번의 갱신을 촉발한다. START 는 하드웨어가 처리하면 스스로 내려가는
 * self-clearing 비트라, 아래 read 쪽에서 그것을 되읽어 판별하지 못하는 이유가 된다.
 *
 * 이 파일이 이 콜백을 EP 모드에서만 노출한다(dw_pcie_ptm_context_update_visible).
 * 시각 문맥을 갱신 요청하는 쪽이 요청자, 즉 엔드포인트이기 때문이다.
 *
 * 실행 컨텍스트: 사용자가 PTM debugfs 파일에 쓸 때의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 write() → ptm.c 의 context_update_fops → [이 함수] → DBI 읽기/쓰기
 */
static int dw_pcie_ptm_context_update_write(void *drvdata, u8 mode)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 읽고 고칠 레지스터 값 */
	u32 val;

	/* [한국어] 자동 갱신 요청 */
	if (mode == PCIE_PTM_CONTEXT_UPDATE_AUTO) {
		/* [한국어] 응답자 요청 제어 레지스터를 읽는다 */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		/* [한국어] 자동 갱신 비트를 세운다. 지속 상태를 나타내는 비트라 한 번 세우면 계속 켜져 있다 */
		val |= PTM_REQ_AUTO_UPDATE_ENABLED;
		/* [한국어] 되쓴다. 이후 하드웨어가 알아서 시각 문맥을 갱신한다 */
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	/* [한국어] 수동 갱신 요청 */
	} else if (mode == PCIE_PTM_CONTEXT_UPDATE_MANUAL) {
		/* [한국어] 같은 레지스터를 읽는다 */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		/* [한국어] 자동 비트를 내린다 — 수동으로 바꾸려면 자동을 먼저 꺼야 한다 */
		val &= ~PTM_REQ_AUTO_UPDATE_ENABLED;
		/* [한국어] 시작 비트를 세워 한 번의 갱신을 촉발한다. self-clearing 이라 처리되면 스스로 내려간다 */
		val |= PTM_REQ_START_UPDATE;
		/* [한국어] 되쓴다. 두 비트 변경을 한 번의 쓰기로 함께 반영한다 */
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else {
		return -EINVAL;
	}

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_context_update_read - 현재 갱신 방식이 자동인지 수동인지 답한다
 *
 * @drvdata: struct dw_pcie.
 * @mode: 결과를 담아 돌려줄 곳.
 * @return: 항상 0.
 *
 * AUTO 비트가 서 있으면 자동, 아니면 수동이라고 답한다. 수동임을 직접 확인하지
 * 않고 "자동이 아니면 수동" 으로 단정하는 것이 이 함수의 유일한 판단이고,
 * 원문 주석이 그 근거를 밝힌다 — START 비트는 self-clearing 이라 평소에는 0 이므로,
 * 그것을 읽어서는 수동 모드인지 알 수 없다.
 *
 * 즉 하드웨어에는 "수동 모드" 라는 지속 상태가 없고 AUTO 의 부재로만 표현된다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 context_update_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_context_update_read(void *drvdata, u8 *mode)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 읽은 레지스터 값 */
	u32 val;

	/* [한국어] 응답자 요청 제어 레지스터를 읽는다 */
	val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
	if (FIELD_GET(PTM_REQ_AUTO_UPDATE_ENABLED, val))
		*mode = PCIE_PTM_CONTEXT_UPDATE_AUTO;
	else
		/*
		 * PTM_REQ_START_UPDATE is a self clearing register bit. So if
		 * PTM_REQ_AUTO_UPDATE_ENABLED is not set, then it implies that
		 * manual update is used.
		 */
		*mode = PCIE_PTM_CONTEXT_UPDATE_MANUAL;

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_context_valid_write - 응답자로서 시각 문맥이 유효한지 표시한다
 *
 * @drvdata: struct dw_pcie.
 * @valid: true 면 유효 표시, false 면 무효화.
 * @return: 항상 0.
 *
 * PTM 대화에서 루트 컴플렉스는 응답자(responder)다. 응답자가 이 비트를 세워야
 * 엔드포인트의 PTM 요청에 유효한 타임스탬프로 답하겠다는 뜻이 되고, 내리면
 * 시각 정보가 신뢰할 수 없는 상태임을 알린다. 그래서 이 콜백은
 * dw_pcie_ptm_context_valid_visible() 에 의해 RC 모드에서만 노출된다.
 *
 * true/false 두 갈래가 읽기-수정-쓰기를 통째로 반복하는 형태인데, 비트를
 * 세우느냐 내리느냐만 다르다. 같은 레지스터에 위쪽 함수가 다루는 AUTO/START
 * 비트도 있어, 통째로 쓰지 않고 읽고 고치는 방식이어야 한다.
 *
 * 실행 컨텍스트: 사용자 write 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 write() → ptm.c 의 context_valid_fops → [이 함수] → DBI 읽기/쓰기
 */
static int dw_pcie_ptm_context_valid_write(void *drvdata, bool valid)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 읽고 고칠 레지스터 값 */
	u32 val;

	/* [한국어] 유효 표시를 세우라는 요청 */
	if (valid) {
		/* [한국어] 응답자 요청 제어 레지스터를 읽는다 */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		/* [한국어] 유효 비트를 세운다. 이제 엔드포인트의 PTM 요청에 유효한 시각으로 답한다 */
		val |= PTM_RES_CCONTEXT_VALID;
		/* [한국어] 되쓴다 */
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	} else {
		/* [한국어] 무효화 요청 — 같은 레지스터를 읽는다 */
		val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
		/* [한국어] 유효 비트를 내린다. 시각 정보를 신뢰할 수 없는 상태임을 알린다 */
		val &= ~PTM_RES_CCONTEXT_VALID;
		/* [한국어] 되쓴다 */
		dw_pcie_writel_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL, val);
	}

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_context_valid_read - 시각 문맥 유효 비트의 현재 값을 답한다
 *
 * @drvdata: struct dw_pcie.
 * @valid: 결과를 담아 돌려줄 곳.
 * @return: 항상 0.
 *
 * write 쪽의 짝이다. FIELD_GET 이 마스크 위치로 내린 값을 !! 로 0/1 로 정규화해
 * bool 에 담는다 — 비트가 최하위 자리라 실제로는 이미 0/1 이지만, 마스크가 바뀌어도
 * 안전하도록 쓰는 관용적인 형태다.
 *
 * RC 모드에서만 노출된다는 점은 write 쪽과 같다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 context_valid_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_context_valid_read(void *drvdata, bool *valid)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 읽은 레지스터 값 */
	u32 val;

	val = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_RES_REQ_CTRL);
	*valid = !!FIELD_GET(PTM_RES_CCONTEXT_VALID, val);

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_local_clock_read - 이 컨트롤러의 로컬 PTM 시계를 64비트로 읽는다
 *
 * @drvdata: struct dw_pcie.
 * @clock: 읽은 시각을 담아 돌려줄 곳(나노초 단위 카운터).
 * @return: 항상 0.
 *
 * 64비트 값을 32비트 레지스터 두 개로 나눠 읽어야 하는데, 그 사이에 시계가
 * 계속 흐른다는 것이 문제다. 상위를 읽고 하위를 읽는 사이에 하위가 넘쳐 상위가
 * 증가하면, 옛 상위와 새 하위를 붙여 실제보다 한참 과거의 값이 나온다.
 *
 * do-while 이 그것을 막는다. 상위·하위를 읽은 뒤 상위를 다시 읽어 처음과 같은지
 * 확인하고, 다르면 넘침이 일어난 것이므로 통째로 다시 읽는다. 이것은 하드웨어
 * 카운터를 lock 없이 안전하게 읽는 표준 기법이며, 아래 다섯 함수가 모두 같은 형태다.
 *
 * dw_pcie_ptm_local_clock_visible() 이 늘 true 를 돌려주므로 이 파일은 RC/EP
 * 어느 모드에서나 만들어진다 — 로컬 시계는 양쪽 모두에 의미가 있다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트. 잠금 없음(위 재시도 기법으로 대체).
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 local_clock_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_local_clock_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 64비트 시계의 상위/하위 절반 */
	u32 msb, lsb;

	do {
		/* [한국어] 상위 절반을 먼저 읽는다 */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB);
		/* [한국어] 이어서 하위 절반을 읽는다. 그 사이에도 시계는 계속 흐른다 */
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_LOCAL_MSB));

	*clock = ((u64) msb) << 32 | lsb;

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_master_clock_read - 상대편(마스터) PTM 시계 값을 읽는다
 *
 * @drvdata: struct dw_pcie.
 * @clock: 읽은 시각을 담아 돌려줄 곳.
 * @return: 항상 0.
 *
 * PTM 대화가 성공하면 엔드포인트는 루트 쪽 시계를 자기 레지스터에 복사해 갖는다.
 * 이 함수가 그 값을 읽는다. 로컬 시계와 이 값의 차이가 곧 두 장치의 시간 편차이고,
 * PTM 이 존재하는 이유다.
 *
 * dw_pcie_ptm_master_clock_visible() 이 EP 모드에서만 true 라 이 파일은
 * 엔드포인트에만 생긴다 — 마스터 시계를 받아 오는 쪽이 엔드포인트이기 때문이다.
 *
 * 읽기 방식은 로컬 시계와 같은 do-while 재시도다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 master_clock_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_master_clock_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 64비트 시계의 상위/하위 절반 */
	u32 msb, lsb;

	do {
		/* [한국어] 마스터 시계의 상위 절반 */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB);
		/* [한국어] 마스터 시계의 하위 절반 */
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_MASTER_MSB));

	*clock = ((u64) msb) << 32 | lsb;

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_t1_read - PTM 대화의 t1 타임스탬프를 읽는다
 *
 * @drvdata: struct dw_pcie.
 * @clock: 읽은 시각을 담아 돌려줄 곳.
 * @return: 항상 0.
 *
 * PTM 은 네 개의 타임스탬프로 왕복 지연과 시각 편차를 계산한다 — 엔드포인트가
 * 요청을 보낸 시각 t1, 루트가 그것을 받은 시각 t2, 루트가 응답을 보낸 시각 t3,
 * 엔드포인트가 응답을 받은 시각 t4. t1 과 t4 는 엔드포인트 쪽 시계로,
 * t2 와 t3 는 루트 쪽 시계로 찍힌다.
 *
 * 하드웨어는 t1 과 t2 를 같은 레지스터 쌍(PTM_T1_T2_LSB/MSB)에 둔다. 한 장치에서
 * 둘 다 유효할 일이 없기 때문이다 — 엔드포인트면 t1 이, 루트면 t2 가 그 자리에 있다.
 * 그래서 이 함수와 dw_pcie_ptm_t2_read() 의 본문이 완전히 같고, 구분은 오직
 * 가시성 콜백이 한다: t1 파일은 EP 모드에만, t2 파일은 RC 모드에만 생긴다.
 *
 * 읽기는 다른 시계와 같은 do-while 재시도 기법이다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 t1_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_t1_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 64비트 타임스탬프의 상위/하위 절반 */
	u32 msb, lsb;

	do {
		/* [한국어] t1/t2 공용 레지스터의 상위 절반. EP 에서 이 자리는 t1 이다 */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		/* [한국어] 같은 쌍의 하위 절반 */
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	*clock = ((u64) msb) << 32 | lsb;

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_t2_read - PTM 대화의 t2 타임스탬프를 읽는다
 *
 * @drvdata: struct dw_pcie.
 * @clock: 읽은 시각을 담아 돌려줄 곳.
 * @return: 항상 0.
 *
 * t2 는 루트 컴플렉스가 엔드포인트의 PTM 요청을 받은 시각이다. 위 t1 과 같은
 * 레지스터 쌍을 읽는데, 그 자리의 값이 t1 인지 t2 인지는 이 장치가 어느 쪽이냐로
 * 정해지기 때문이다. 따라서 이 함수의 본문은 t1 쪽과 한 글자도 다르지 않고,
 * 존재 이유는 PTM 계층이 이름별로 콜백을 요구하는 데 있다.
 *
 * dw_pcie_ptm_t2_visible() 이 RC 모드에서만 true 라, 이 파일은 루트에만 생긴다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 t2_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_t2_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 64비트 타임스탬프의 상위/하위 절반 */
	u32 msb, lsb;

	do {
		/* [한국어] t1/t2 공용 레지스터의 상위 절반. RC 에서 이 자리는 t2 다 */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB);
		/* [한국어] 같은 쌍의 하위 절반 */
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T1_T2_MSB));

	*clock = ((u64) msb) << 32 | lsb;

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_t3_read - PTM 대화의 t3 타임스탬프를 읽는다
 *
 * @drvdata: struct dw_pcie.
 * @clock: 읽은 시각을 담아 돌려줄 곳.
 * @return: 항상 0.
 *
 * t3 는 루트가 PTM 응답을 내보낸 시각이다. t1/t2 가 한 쌍을 공유했듯 t3 와 t4 도
 * PTM_T3_T4_LSB/MSB 한 쌍을 공유하며, 어느 쪽 값인지는 장치의 역할이 정한다.
 * 그래서 이 함수와 t4 쪽 함수의 본문도 동일하다.
 *
 * dw_pcie_ptm_t3_visible() 이 RC 모드에서만 true 라, 이 파일은 루트에만 생긴다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 t3_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_t3_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 64비트 타임스탬프의 상위/하위 절반 */
	u32 msb, lsb;

	do {
		/* [한국어] t3/t4 공용 레지스터의 상위 절반. RC 에서 이 자리는 t3 다 */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		/* [한국어] 같은 쌍의 하위 절반 */
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	*clock = ((u64) msb) << 32 | lsb;

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_t4_read - PTM 대화의 t4 타임스탬프를 읽는다
 *
 * @drvdata: struct dw_pcie.
 * @clock: 읽은 시각을 담아 돌려줄 곳.
 * @return: 항상 0.
 *
 * t4 는 엔드포인트가 PTM 응답을 받은 시각이다. t1 과 t4 를 알면 왕복 시간이,
 * 거기에 t2 와 t3 를 더하면 루트 쪽 처리 시간을 뺀 순수 전파 지연이 나오고,
 * 그것으로 두 시계의 편차를 보정한다.
 *
 * t3 와 같은 레지스터 쌍을 읽으며, dw_pcie_ptm_t4_visible() 이 EP 모드에서만
 * true 라 이 파일은 엔드포인트에만 생긴다.
 *
 * 실행 컨텍스트: 사용자 read 의 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   사용자 공간 read() → ptm.c 의 t4_fops → [이 함수] → dw_pcie_readl_dbi()
 */
static int dw_pcie_ptm_t4_read(void *drvdata, u64 *clock)
{
	struct dw_pcie *pci = drvdata;
	/* [한국어] 64비트 타임스탬프의 상위/하위 절반 */
	u32 msb, lsb;

	do {
		/* [한국어] t3/t4 공용 레지스터의 상위 절반. EP 에서 이 자리는 t4 다 */
		msb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB);
		/* [한국어] 같은 쌍의 하위 절반 */
		lsb = dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_LSB);
	} while (msb != dw_pcie_readl_dbi(pci, pci->ptm_vsec_offset + PTM_T3_T4_MSB));

	*clock = ((u64) msb) << 32 | lsb;

	return 0;
}

/* [한국어]
 * dw_pcie_ptm_context_update_visible - context_update 파일을 만들지 결정한다
 *
 * @drvdata: struct dw_pcie.
 * @return: EP 모드면 true, 아니면 false.
 *
 * PTM 계층은 파일을 만들기 전에 이런 가시성 콜백을 물어, false 면 그 파일을 아예
 * 만들지 않는다. 덕분에 모드에 맞지 않는 파일이 보이지 않고, 사용자가 의미 없는
 * 값을 읽고 혼란스러워하는 일이 없다.
 *
 * 시각 문맥 갱신은 요청자인 엔드포인트가 하는 일이므로 EP 모드에서만 노출한다.
 * 판단 근거인 pci->mode 는 dwc_pcie_debugfs_init() 이 호출자에게서 받아 저장한 값이다.
 *
 * 실행 컨텍스트: 초기화 중 pcie_ptm_create_debugfs() 안, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() 의 pcie_ptm_create_debugfs_file 매크로 → [이 함수]
 */
static bool dw_pcie_ptm_context_update_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return pci->mode == DW_PCIE_EP_TYPE;
}

/* [한국어]
 * dw_pcie_ptm_context_valid_visible - context_valid 파일을 만들지 결정한다
 *
 * @drvdata: struct dw_pcie.
 * @return: RC 모드면 true, 아니면 false.
 *
 * 시각 문맥의 유효 표시는 응답자인 루트 컴플렉스가 하는 일이라 RC 모드에서만
 * 노출한다. 바로 위 context_update 와 정확히 반대 조건인 것이 그 때문이다.
 *
 * 실행 컨텍스트: 초기화 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() → [이 함수]
 */
static bool dw_pcie_ptm_context_valid_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return pci->mode == DW_PCIE_RC_TYPE;
}

/* [한국어]
 * dw_pcie_ptm_local_clock_visible - local_clock 파일은 언제나 만든다
 *
 * @drvdata: struct dw_pcie. 쓰이지 않는다.
 * @return: 언제나 true.
 *
 * 로컬 PTM 시계는 요청자든 응답자든 자기 시계이므로 양쪽 모두에 의미가 있다.
 * 그래서 모드를 보지 않고 무조건 노출한다 — 원문 주석도 그 한 줄을 밝힌다.
 * drvdata 를 받지만 쓰지 않는 것은 PTM 계층이 모든 가시성 콜백에 같은 서명을
 * 요구하기 때문이다.
 *
 * 실행 컨텍스트: 초기화 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() → [이 함수]
 */
static bool dw_pcie_ptm_local_clock_visible(void *drvdata)
{
	/* PTM local clock is always visible */
	return true;
}

/* [한국어]
 * dw_pcie_ptm_master_clock_visible - master_clock 파일을 만들지 결정한다
 *
 * @drvdata: struct dw_pcie.
 * @return: EP 모드면 true.
 *
 * 마스터 시계는 상대편(루트)의 시계를 복사해 온 값이므로, 그것을 받아 오는 쪽인
 * 엔드포인트에만 존재한다. 루트 자신에게는 "마스터 시계" 가 곧 로컬 시계라
 * 따로 노출할 이유가 없다.
 *
 * 실행 컨텍스트: 초기화 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() → [이 함수]
 */
static bool dw_pcie_ptm_master_clock_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return pci->mode == DW_PCIE_EP_TYPE;
}

/* [한국어]
 * dw_pcie_ptm_t1_visible - t1 파일을 만들지 결정한다
 *
 * @drvdata: struct dw_pcie.
 * @return: EP 모드면 true.
 *
 * t1 은 엔드포인트가 PTM 요청을 보낸 시각이므로 엔드포인트에만 있다.
 * 하드웨어가 t1 과 t2 를 같은 레지스터 쌍에 두기 때문에, 그 자리를 t1 로 볼지
 * t2 로 볼지를 실질적으로 가르는 것이 이 콜백과 t2 쪽 콜백의 조건이다.
 *
 * 실행 컨텍스트: 초기화 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() → [이 함수]
 */
static bool dw_pcie_ptm_t1_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return pci->mode == DW_PCIE_EP_TYPE;
}

/* [한국어]
 * dw_pcie_ptm_t2_visible - t2 파일을 만들지 결정한다
 *
 * @drvdata: struct dw_pcie.
 * @return: RC 모드면 true.
 *
 * t2 는 루트가 요청을 받은 시각이라 루트에만 있다. t1 과 레지스터를 공유하므로
 * 이 조건이 t1 쪽과 배타적이어야 두 파일이 한 장치에 동시에 생기지 않는다.
 *
 * 실행 컨텍스트: 초기화 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() → [이 함수]
 */
static bool dw_pcie_ptm_t2_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return pci->mode == DW_PCIE_RC_TYPE;
}

/* [한국어]
 * dw_pcie_ptm_t3_visible - t3 파일을 만들지 결정한다
 *
 * @drvdata: struct dw_pcie.
 * @return: RC 모드면 true.
 *
 * t3 는 루트가 응답을 내보낸 시각이라 루트에만 있다. t4 와 레지스터 쌍을
 * 공유하며, 그 배타성을 이 콜백과 t4 쪽 콜백이 보장한다.
 *
 * 실행 컨텍스트: 초기화 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() → [이 함수]
 */
static bool dw_pcie_ptm_t3_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return pci->mode == DW_PCIE_RC_TYPE;
}

/* [한국어]
 * dw_pcie_ptm_t4_visible - t4 파일을 만들지 결정한다
 *
 * @drvdata: struct dw_pcie.
 * @return: EP 모드면 true.
 *
 * t4 는 엔드포인트가 응답을 받은 시각이라 엔드포인트에만 있다. 결과적으로
 * EP 에는 t1 과 t4 가, RC 에는 t2 와 t3 가 생겨, 각 장치가 자기 시계로 찍은
 * 두 시각만 보게 된다.
 *
 * 실행 컨텍스트: 초기화 중, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   pcie_ptm_create_debugfs() → [이 함수]
 */
static bool dw_pcie_ptm_t4_visible(void *drvdata)
{
	struct dw_pcie *pci = drvdata;

	return pci->mode == DW_PCIE_EP_TYPE;
}

/* [한국어] PCI 코어의 PTM debugfs 계층에 넘기는 콜백 묶음.
 * 두 종류가 섞여 있다 — 앞쪽 11개는 "레지스터를 어떻게 읽고 쓰는가"(동작),
 * 뒤쪽 8개는 "이 모드에서 그 파일을 만들 것인가"(가시성)다. 코어는 파일을 만들기 전에
 * 가시성 콜백을 먼저 물어 false 면 그 파일을 아예 만들지 않으므로, RC 에는
 * context_valid/t2/t3 가, EP 에는 context_update/master_clock/t1/t4 가 생긴다.
 * local_clock 만 양쪽 모두에 생긴다. */
static const struct pcie_ptm_ops dw_pcie_ptm_ops = {
	/* [한국어] PTM 확장의 존재 확인이자 그 오프셋을 pci->ptm_vsec_offset 에 기억하는 관문.
	 * 코어가 가장 먼저 부르며, 0 이 나오면 아무 파일도 만들지 않는다. */
	.check_capability = dw_pcie_ptm_check_capability,
	/* [한국어] 갱신 방식을 자동/수동으로 바꾼다. EP 에만 노출된다 */
	.context_update_write = dw_pcie_ptm_context_update_write,
	/* [한국어] 현재 갱신 방식을 답한다. AUTO 비트의 부재로 수동을 판별한다 */
	.context_update_read = dw_pcie_ptm_context_update_read,
	/* [한국어] 응답자로서 시각 문맥의 유효 표시를 세우거나 내린다. RC 에만 노출된다 */
	.context_valid_write = dw_pcie_ptm_context_valid_write,
	/* [한국어] 그 유효 비트의 현재 값을 답한다 */
	.context_valid_read = dw_pcie_ptm_context_valid_read,
	/* [한국어] 이 장치의 로컬 PTM 시계. 양쪽 모드 모두에 노출된다 */
	.local_clock_read = dw_pcie_ptm_local_clock_read,
	/* [한국어] 상대편에게서 받아 온 마스터 시계. EP 에만 노출된다 */
	.master_clock_read = dw_pcie_ptm_master_clock_read,
	/* [한국어] 엔드포인트가 요청을 보낸 시각. t2 와 레지스터를 공유한다 */
	.t1_read = dw_pcie_ptm_t1_read,
	/* [한국어] 루트가 요청을 받은 시각. t1 과 같은 레지스터를 읽는다 */
	.t2_read = dw_pcie_ptm_t2_read,
	/* [한국어] 루트가 응답을 보낸 시각. t4 와 레지스터를 공유한다 */
	.t3_read = dw_pcie_ptm_t3_read,
	/* [한국어] 엔드포인트가 응답을 받은 시각. t3 와 같은 레지스터를 읽는다 */
	.t4_read = dw_pcie_ptm_t4_read,
	/* [한국어] context_update 파일을 만들지 — EP 에서만 참 */
	.context_update_visible = dw_pcie_ptm_context_update_visible,
	/* [한국어] context_valid 파일을 만들지 — RC 에서만 참 */
	.context_valid_visible = dw_pcie_ptm_context_valid_visible,
	/* [한국어] local_clock 파일을 만들지 — 언제나 참 */
	.local_clock_visible = dw_pcie_ptm_local_clock_visible,
	/* [한국어] master_clock 파일을 만들지 — EP 에서만 참 */
	.master_clock_visible = dw_pcie_ptm_master_clock_visible,
	/* [한국어] t1 파일을 만들지 — EP 에서만 참. t2 와 배타적이어야 레지스터 공유가 성립한다 */
	.t1_visible = dw_pcie_ptm_t1_visible,
	/* [한국어] t2 파일을 만들지 — RC 에서만 참 */
	.t2_visible = dw_pcie_ptm_t2_visible,
	/* [한국어] t3 파일을 만들지 — RC 에서만 참 */
	.t3_visible = dw_pcie_ptm_t3_visible,
	/* [한국어] t4 파일을 만들지 — EP 에서만 참 */
	.t4_visible = dw_pcie_ptm_t4_visible,
};

/* [한국어]
 * dwc_pcie_debugfs_deinit - 이 컨트롤러의 debugfs 를 전부 걷는다
 *
 * @pci: 대상 컨트롤러.
 * @return: 없음.
 *
 * init 의 역이며 순서가 반대다. PTM 쪽을 먼저 파괴하는데, 그것은 PCI 코어가
 * 별도의 최상위 디렉토리(pcie_ptm_<이름>)에 만든 것이라 우리 디렉토리를
 * 재귀 삭제해도 걷히지 않기 때문이다. 그다음 RAS DES 의 뮤텍스를 파괴하고,
 * 마지막에 우리 디렉토리를 통째로 재귀 삭제한다.
 *
 * 재귀 삭제가 파일 하나하나를 지우는 코드를 대신하므로, 앞의 init 이 만든
 * 수십 개 파일에 대한 개별 해제 코드가 이 파일에 없는 것이다.
 *
 * pci->debugfs 가 NULL 이면 곧바로 돌아간다. init 이 devm_kzalloc 실패로
 * 일찍 빠져나갔거나 아예 불리지 않은 경우를 막는 방어다.
 *
 * 실행 컨텍스트: 드라이버 remove 또는 EP 의 cleanup 경로, 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   dw_pcie_host_deinit() / dw_pcie_ep_cleanup() → [이 함수]
 *     → pcie_ptm_destroy_debugfs(), dwc_pcie_rasdes_debugfs_deinit(),
 *       debugfs_remove_recursive()
 */
void dwc_pcie_debugfs_deinit(struct dw_pcie *pci)
{
	if (!pci->debugfs)
		return;

	pcie_ptm_destroy_debugfs(pci->ptm_debugfs);
	dwc_pcie_rasdes_debugfs_deinit(pci);
	debugfs_remove_recursive(pci->debugfs->debug_dir);
}

/* [한국어]
 * dwc_pcie_debugfs_init - 이 컨트롤러의 debugfs 계층 전체를 만든다
 *
 * @pci: 대상 컨트롤러.
 * @mode: DW_PCIE_RC_TYPE 또는 DW_PCIE_EP_TYPE. 호출자가 자기 역할을 알려 준다.
 * @return: 없음(진단 기능이라 실패해도 드라이버를 멈추지 않는다).
 *
 * 이 파일의 유일한 외부 진입점이다. 컨트롤러마다 dwc_pcie_<디바이스명> 디렉토리를
 * debugfs 최상위에 만들고, 그 아래에 RAS DES 세 갈래와 ltssm_status 를 편다.
 * 디렉토리 이름에 디바이스 이름을 붙이는 것은 한 시스템에 DWC 컨트롤러가
 * 여러 개 있을 수 있어서다.
 *
 * 오류 처리 방침이 특징적이다. RAS DES 초기화가 실패해도 로그만 남기고 계속
 * 진행하며, 반환형 자체가 void 다. debugfs 는 진단 편의 기능이므로 그것이
 * 안 된다고 PCIe 링크를 못 쓰게 만들 이유가 없다는 판단이다. 유일하게
 * 곧바로 돌아가는 경우는 debugfs 상태 구조체 할당 실패인데, 그것 없이는
 * 이후 단계가 성립하지 않기 때문이다.
 *
 * pci->mode 를 저장하는 위치가 중요하다. 바로 다음 줄의
 * pcie_ptm_create_debugfs() 가 가시성 콜백들을 부르고, 그것들이 이 필드를 보고
 * 어떤 PTM 파일을 만들지 정하기 때문에 반드시 그 전에 채워져 있어야 한다.
 *
 * 실행 컨텍스트: 드라이버 초기화의 프로세스 컨텍스트. devm 할당으로 잠들 수 있다.
 *
 * 호출 체인:
 *   dw_pcie_host_init()(RC) / dw_pcie_ep_init_registers()(EP) → [이 함수]
 *     → debugfs_create_dir(), dwc_pcie_rasdes_debugfs_init(),
 *       dwc_pcie_ltssm_debugfs_init(), pcie_ptm_create_debugfs()
 */
void dwc_pcie_debugfs_init(struct dw_pcie *pci, enum dw_pcie_device_mode mode)
{
	char dirname[DWC_DEBUGFS_BUF_MAX];
	/* [한국어] devm 할당의 주인이자 디렉토리 이름에 쓸 이름의 출처 */
	struct device *dev = pci->dev;
	/* [한국어] 만들 debugfs 상태 구조체 */
	struct debugfs_info *debugfs;
	/* [한국어] 만든 최상위 디렉토리 */
	struct dentry *dir;
	/* [한국어] RAS DES 초기화 결과 */
	int err;

	/* Create main directory for each platform driver. */
	snprintf(dirname, DWC_DEBUGFS_BUF_MAX, "dwc_pcie_%s", dev_name(dev));
	/* [한국어] debugfs 최상위(부모 NULL)에 만든다. 반환값을 검사하지 않는 것은 debugfs API 가
	 * 실패해도 오류 포인터를 그대로 넘겨 이후 호출이 무해하게 실패하도록 설계돼 있기 때문 */
	dir = debugfs_create_dir(dirname, NULL);
	/* [한국어] 디렉토리와 rasdes_info 포인터를 담을 구조체를 잡는다 */
	debugfs = devm_kzalloc(dev, sizeof(*debugfs), GFP_KERNEL);
	/* [한국어] 이것이 없으면 이후 단계가 성립하지 않으므로 여기서만 곧바로 돌아간다 */
	if (!debugfs)
		return;

	/* [한국어] 최상위 디렉토리를 기록. deinit 이 이것을 재귀 삭제한다 */
	debugfs->debug_dir = dir;
	/* [한국어] 컨트롤러에 걸어 둔다. 이 순간부터 핸들러들이 pci->debugfs 로 접근할 수 있다 */
	pci->debugfs = debugfs;
	/* [한국어] RAS DES 세 갈래를 편다. 그 함수는 RAS DES 가 없으면 조용히 0 을 돌려준다 */
	err = dwc_pcie_rasdes_debugfs_init(pci, dir);
	/* [한국어] 할당 실패 등 진짜 오류일 때 */
	if (err)
		/* [한국어] 로그만 남기고 계속 진행한다 — 진단 기능이 안 된다고 링크를 못 쓰게 할 이유가 없다 */
		dev_err(dev, "failed to initialize RAS DES debugfs, err=%d\n",
			err);

	/* [한국어] ltssm_status 파일 하나를 만든다. 이쪽은 벤더 확장이 아니라 늘 있으므로 검사가 없다 */
	dwc_pcie_ltssm_debugfs_init(pci, dir);

	/* [한국어] 호출자가 알려 준 역할을 기록. 바로 다음 줄의 가시성 콜백들이 이 값을 보므로
	 * 반드시 그 전에 채워져 있어야 한다 */
	pci->mode = mode;
	/* [한국어] PCI 코어의 PTM debugfs 계층에 우리 콜백을 넘긴다. 그쪽이 별도 최상위 디렉토리를
	 * 만들므로 deinit 에서 따로 파괴해야 한다 */
	pci->ptm_debugfs = pcie_ptm_create_debugfs(pci->dev, pci,
						   &dw_pcie_ptm_ops);
}
