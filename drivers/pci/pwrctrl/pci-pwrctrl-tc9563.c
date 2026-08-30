// SPDX-License-Identifier: GPL-2.0-only
/* PCI/NVMe: 이 드라이버는 TC9563 PCIe 스위치/리타이머의 전원/리셋/물리 레이어를 제어하며,
 *           NVMe SSD가 연결되는 다운스트림 포트(DSP)의 전기적 안정성과 ASPM/L1ss 동작을 좌우한다.
 */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/array_size.h>	/* NVMe: ARRAY_SIZE() 매크로, 설정 시퀀스 순회에 사용 */
#include <linux/bitfield.h>	/* NVMe: FIELD_PREP() 등 비트 필드 조작, PCIe 레지스터 설정에 사용 */
#include <linux/bits.h>		/* NVMe: GENMASK/BIT 매크로, 포트 선택 및 레인 마스크 작성에 사용 */
#include <linux/delay.h>	/* NVMe: fsleep(), PCIe 리타이머 안정화 대기에 필요 */
#include <linux/device.h>	/* NVMe: dev_err_probe(), NVMe 열거 전 전원 레일 활성화 단계에서 사용 */
#include <linux/gpio/consumer.h>	/* NVMe: gpiod_set_value(), PCIe 리타이머 리셋 핀 제어 */
#include <linux/i2c.h>		/* NVMe: I2C를 통해 TC9563 낶부 PCIe 물리 레지스터에 접근 */
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>		/* NVMe: DT에서 NVMe 연결 포트별 ASPM 지연/전원 설정을 읽어옴 */
#include <linux/of_platform.h>	/* NVMe: DT 노드 순회, 다운스트림 NVMe 슬롯 설정 파싱 */
#include <linux/pci.h>		/* NVMe: pci_pwrctrl_* 인프라, NVMe 장치의 PCIe 열거 전 전원 제어 */
#include <linux/pci-pwrctrl.h>	/* NVMe: PCI 전원 제어 코어 콜백 구조체 정의 */
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>	/* NVMe: regulator_bulk_enable(), NVMe 장치 전원 레일 인가 */
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>	/* NVMe: put_unaligned_be24/le32, I2C 레지스터 프로토콜 처리 */

#include "../pci.h"		/* NVMe: PCI 코어 낶부 정의, 전원 제어 프레임워크와 연결 */

#define TC9563_GPIO_CONFIG		0x801208
				/* NVMe: TC9563 GPIO 기능 선택 레지스터, 리셋 핀을 PCIe PERST#처럼 동작시키는 데 사용 */
#define TC9563_RESET_GPIO		0x801210
				/* NVMe: TC9563 리셋 출력 GPIO 레지스터, NVMe SSD의 PERST# 핀 제어와 유사한 역할 */

#define TC9563_PORT_L0S_DELAY		0x82496c
				/* NVMe: PCIe ASPM L0s 진입 지연 레지스터, NVMe 링크의 L0s 수면/절전 타이밍 조정 */
#define TC9563_PORT_L1_DELAY		0x824970
				/* NVMe: PCIe ASPM L1 진입 지연 레지스터, NVMe idle 절전(L1) 성능/안정성 튜닝 */

#define TC9563_EMBEDDED_ETH_DELAY	0x8200d8
				/* NVMe: TC9563 내장 이더넷 포트의 ASPM 지연 레지스터, NVMe가 아닌 다른 EP지만 동일 PCIe fabric 공유 */
#define TC9563_ETH_L1_DELAY_MASK	GENMASK(27, 18)
				/* NVMe: 이더넷 L1 지연 필드 마스크 */
#define TC9563_ETH_L1_DELAY_VALUE(x)	FIELD_PREP(TC9563_ETH_L1_DELAY_MASK, x)
				/* NVMe: 이더넷 L1 지연값 패킹 매크로 */
#define TC9563_ETH_L0S_DELAY_MASK	GENMASK(17, 13)
				/* NVMe: 이더넷 L0s 지연 필드 마스크 */
#define TC9563_ETH_L0S_DELAY_VALUE(x)	FIELD_PREP(TC9563_ETH_L0S_DELAY_MASK, x)
				/* NVMe: 이더넷 L0s 지연값 패킹 매크로 */

#define TC9563_NFTS_2_5_GT		0x824978
				/* NVMe: PCIe Gen1(2.5GT/s)에서 N_FTS 값 설정, NVMe 링크 재훈련 시 빠른 심볼 잠금에 영향 */
#define TC9563_NFTS_5_GT		0x82497c
				/* NVMe: PCIe Gen2(5GT/s)에서 N_FTS 값 설정, NVMe SSD와의 링크 복구 시간 튜닝 */

#define TC9563_PORT_LANE_ACCESS_ENABLE	0x828000
				/* NVMe: 포트/레인 접근 활성화 레지스터, 특정 PCIe 포트의 물리 레이어를 프로그래밍할 때 사용 */

#define TC9563_PHY_RATE_CHANGE_OVERRIDE	0x828040
				/* NVMe: PHY 속도 변경 오버라이드 레지스터, Gen 변경 시 NVMe 링크 안정성 확보 */
#define TC9563_PHY_RATE_CHANGE		0x828050
				/* NVMe: PHY 속도 변경 트리거 레지스터, NVMe SSD의 링크 속도 협상에 대응 */

#define TC9563_TX_MARGIN		0x828234
				/* NVMe: TX 진폭 마진 레지스터, NVMe SSD로의 송신 신호 강도 조정 */

#define TC9563_DFE_ENABLE		0x828a04
				/* NVMe: Decision Feedback Equalizer 활성화 레지스터, NVMe 신호 무결성 개선 */
#define TC9563_DFE_EQ0_MODE		0x828a08
				/* NVMe: DFE EQ0 모드 레지스터, NVMe PCIe 레인의 이퀄라이저 설정 */
#define TC9563_DFE_EQ1_MODE		0x828a0c
				/* NVMe: DFE EQ1 모드 레지스터 */
#define TC9563_DFE_EQ2_MODE		0x828a14
				/* NVMe: DFE EQ2 모드 레지스터 */
#define TC9563_DFE_PD_MASK		0x828254
				/* NVMe: DFE 전원 다운 마스크 레지스터, DFE 사용 안 함 시 전원 차단 */

#define TC9563_PORT_SELECT		0x82c02c
				/* NVMe: L0s/L1 지연/N_FTS 설정 시 접근할 PCIe 포트 선택 */
#define TC9563_PORT_ACCESS_ENABLE	0x82c030
				/* NVMe: 특정 포트의 설정 공간 접근을 활성화, USP/DSP별로 값이 다름 */

#define TC9563_POWER_CONTROL		0x82b09c
				/* NVMe: 포트별 전원 제어 레지스터, NVMe 장치가 연결된 DSP 전원 차단 시 사용 */
#define TC9563_POWER_CONTROL_OVREN	0x82b2c8
				/* NVMe: 전원 제어 오버라이드 레지스터, DSP 강제 파워오프 시퀀스에 필요 */

#define TC9563_GPIO_MASK		0xfffffff3
				/* NVMe: GPIO 설정 시 불필요한 비트를 보존하기 위한 마스크 */
#define TC9563_GPIO_DEASSERT_BITS	0xc  /* Clear to deassert GPIO */
				/* NVMe: 리셋 디어서트를 위해 해당 비트를 클리어(0)로 설정, NVMe PERST# 해제와 동일한 의미 */

#define TC9563_TX_MARGIN_MIN_UA		400000
				/* NVMe: TX 마진 최소 진폭(µA), 400mA 이하이면 NVMe 링크 튜닝을 생략 */

/*
 * From TC9563 PORSYS rev 0.2, figure 1.1 POR boot sequence
 * wait for 10ms for the internal osc frequency to stabilize.
 */
#define TC9563_OSC_STAB_DELAY_US	(10 * USEC_PER_MSEC)
				/* NVMe: TC9563 낶부 발진기 안정화 대기, NVMe SSD 리셋 후 안정적인 PCIe 클록 공급을 위해 필요 */

#define TC9563_L0S_L1_DELAY_UNIT_NS	256  /* Each unit represents 256 ns */
				/* NVMe: ASPM L0s/L1 지연값 단위, NVMe idle 절전 전환 타이밍을 256ns 단위로 조정 */

/* NVMe: TC9563의 레지스터 오프셋과 쓸 값을 쌍으로 묶은 설정 항목, I2C 블록 쓰기에 사용 */
struct tc9563_pwrctrl_reg_setting {
	unsigned int offset;	/* NVMe: I2C로 접근하는 TC9563 낶부 레지스터 오프셋, PCIe 물리/링크 설정 */
	unsigned int val;	/* NVMe: 해당 레지스터에 쓸 값, NVMe 링크 튜닝 파라미터 */
};

/* NVMe: TC9563의 논리 포트 식별자, 각 포트는 PCIe fabric상의 Upstream/Downstream 포트에 대응 */
enum tc9563_pwrctrl_ports {
	TC9563_USP,		/* NVMe: 업스트림 포트, RC와 TC9563를 연결 */
	TC9563_DSP1,		/* NVMe: 다운스트림 포트 1, NVMe SSD가 연결될 수 있는 슬롯 */
	TC9563_DSP2,		/* NVMe: 다운스트림 포트 2, 두 번째 NVMe SSD 슬롯 */
	TC9563_DSP3,		/* NVMe: 다운스트림 포트 3, 추가 NVMe 또는 내장 EP 연결 */
	TC9563_ETHERNET,	/* NVMe: 내장 이더넷 EP, NVMe와 동일 PCIe fabric 사용 */
	TC9563_MAX		/* NVMe: 포트 개수 상한, cfg 배열 인덱스 범위 */
};

/* NVMe: DT에서 읽어온 포트별 설정, NVMe SSD가 연결된 DSP의 물리/절전 특성을 결정 */
struct tc9563_pwrctrl_cfg {
	u32 l0s_delay;		/* NVMe: ASPM L0s 진입 지연(ns), NVMe idle 대역폭/절전 균형에 영향 */
	u32 l1_delay;		/* NVMe: ASPM L1 진입 지연(ns), NVMe L1 절전 진입 속도 조정 */
	u32 tx_amp;		/* NVMe: 송신 진폭(µV), NVMe SSD와의 전기적 호환성 튜닝 */
	u8 nfts[2]; /* GEN1 & GEN2 */
				/* NVMe: Gen1/Gen2 N_FTS, NVMe 링크 재훈련(recovery) 속도에 영향 */
	bool disable_dfe;	/* NVMe: DFE 비활성화 여부, NVMe 레인의 이퀄라이저 동작 변경 */
	bool disable_port;	/* NVMe: DT에서 disabled된 포트, NVMe SSD가 이 포트에 물리적으로 없음을 표시 */
};

#define TC9563_PWRCTL_MAX_SUPPLY	6
				/* NVMe: TC9563를 구동하는 전원 레일 수, NVMe SSD 활성화 전 모두 안정화되어야 함 */

/* NVMe: TC9563가 요구하는 전원 레일 이름, NVMe SSD 전원 순서와 동일하게 안정적인 레일 인가가 필요 */
static const char *const tc9563_supply_names[TC9563_PWRCTL_MAX_SUPPLY] = {
	"vddc",
	"vdd18",
	"vdd09",
	"vddio1",
	"vddio2",
	"vddio18",
};

/* NVMe: TC9563 전원 제어 드라이버 인스턴스, pci_pwrctrl 코어에 등록되어 NVMe 열거 전/후 전원을 관리 */
struct tc9563_pwrctrl {
	struct pci_pwrctrl pwrctrl;	/* NVMe: PCI 전원 제어 코어 구조체, power_on/off 콜백 등록 대상 */
	struct regulator_bulk_data supplies[TC9563_PWRCTL_MAX_SUPPLY];
					/* NVMe: 레귤레이터 일괄 제어 데이터, NVMe 장치 전원 안정화를 위한 레일 */
	struct tc9563_pwrctrl_cfg cfg[TC9563_MAX];
					/* NVMe: 포트별 설정, NVMe 연결 포트의 물리/절전 튜닝값 */
	struct gpio_desc *reset_gpio;	/* NVMe: TC9563 하드웨어 리셋 GPIO, NVMe PERST#와 같은 역할 */
	struct i2c_adapter *adapter;	/* NVMe: TC9563와 통신할 I2C 어댑터, PCIe 레지스터 백도어 접근 */
	struct i2c_client *client;	/* NVMe: I2C 더미 클라이언트, TC9563의 레지스터에 읽기/쓰기 */
};

/*
 * downstream port power off sequence, hardcoding the address
 * as we don't know register names for these register offsets.
 */
/* NVMe: DSP(다운스트림 포트) 강제 파워오프 시퀀스, NVMe SSD가 연결된 포트를 안전하게 끌 때 사용 */
static const struct tc9563_pwrctrl_reg_setting common_pwroff_seq[] = {
	{0x82900c, 0x1},
	{0x829010, 0x1},
	{0x829018, 0x0},
	{0x829020, 0x1},
	{0x82902c, 0x1},
	{0x829030, 0x1},
	{0x82903c, 0x1},
	{0x829058, 0x0},
	{0x82905c, 0x1},
	{0x829060, 0x1},
	{0x8290cc, 0x1},
	{0x8290d0, 0x1},
	{0x8290d8, 0x1},
	{0x8290e0, 0x1},
	{0x8290e8, 0x1},
	{0x8290ec, 0x1},
	{0x8290f4, 0x1},
	{0x82910c, 0x1},
	{0x829110, 0x1},
	{0x829114, 0x1},
};

/* NVMe: DSP1(포트1, NVMe 슬롯 후보) 전용 파워오프 시퀀스 */
static const struct tc9563_pwrctrl_reg_setting dsp1_pwroff_seq[] = {
	{TC9563_PORT_ACCESS_ENABLE, 0x2},
				/* NVMe: DSP1 설정 공간 접근 활성화 */
	{TC9563_PORT_LANE_ACCESS_ENABLE, 0x3},
				/* NVMe: DSP1 레인 접근 활성화, NVMe SSD와의 물리 레인 제어 */
	{TC9563_POWER_CONTROL, 0x014f4804},
				/* NVMe: DSP1 전원 제어 값, NVMe 링크 파워 상태 전환 */
	{TC9563_POWER_CONTROL_OVREN, 0x1},
				/* NVMe: 전원 제어 오버라이드 활성화, DSP1 강제 파워오프 */
	{TC9563_PORT_ACCESS_ENABLE, 0x4},
				/* NVMe: 다음 포트 접근 전환 */
};

/* NVMe: DSP2(포트2, 두 번째 NVMe 슬롯 후보) 전용 파워오프 시퀀스 */
static const struct tc9563_pwrctrl_reg_setting dsp2_pwroff_seq[] = {
	{TC9563_PORT_ACCESS_ENABLE, 0x8},
				/* NVMe: DSP2 설정 공간 접근 활성화 */
	{TC9563_PORT_LANE_ACCESS_ENABLE, 0x1},
				/* NVMe: DSP2 레인 접근 활성화, NVMe SSD와의 레인 선택 */
	{TC9563_POWER_CONTROL, 0x014f4804},
				/* NVMe: DSP2 전원 제어 값, NVMe 링크 전원 차단 */
	{TC9563_POWER_CONTROL_OVREN, 0x1},
				/* NVMe: 전원 제어 오버라이드 활성화, DSP2 강제 파워오프 */
	{TC9563_PORT_ACCESS_ENABLE, 0x8},
				/* NVMe: DSP2 접근 상태 유지/정리 */
};

/*
 * Since all transfers are initiated by the probe, no locks are necessary,
 * as there are no concurrent calls.
 */
/* NVMe: TC9563 낶부 레지스터에 4바이트 값을 I2C로 쓰는 함수, NVMe 열거 전 물리 설정에 사용 */
static int tc9563_pwrctrl_i2c_write(struct i2c_client *client,
				    u32 reg_addr, u32 reg_val)
{
	struct i2c_msg msg;	/* NVMe: 단일 I2C 쓰기 메시지 */
	u8 msg_buf[7];		/* NVMe: 3바이트 레지스터 주소 + 4바이트 값 버퍼 */
	int ret;

	msg.addr = client->addr;	/* NVMe: TC9563 I2C 슬레이브 주소 */
	msg.len = 7;			/* NVMe: 주소 3바이트 + 값 4바이트 */
	msg.flags = 0;			/* NVMe: 쓰기 전송 플래그 */

	/* Big Endian for reg addr */
	put_unaligned_be24(reg_addr, &msg_buf[0]);
					/* NVMe: 레지스터 주소를 빅엔디안 24비트로 변환, TC9563 레지스터 맵 프로토콜 */

	/* Little Endian for reg val */
	put_unaligned_le32(reg_val, &msg_buf[3]);
					/* NVMe: 쓸 데이터를 리틀엔디안 32비트로 변환 */

	msg.buf = msg_buf;		/* NVMe: I2C 메시지 버퍼 연결 */
	ret = i2c_transfer(client->adapter, &msg, 1);
					/* NVMe: I2C 버스로 TC9563에 PCIe 설정 레지스터 쓰기 */
	return ret == 1 ? 0 : ret;
					/* NVMe: 1개 메시지 성공 시 0, 실패 시 I2C 에러 코드 */
}

/* NVMe: TC9563 낶부 레지스터에서 4바이트 값을 I2C로 읽는 함수, NVMe 관련 설정 확인/수정 시 사용 */
static int tc9563_pwrctrl_i2c_read(struct i2c_client *client,
				   u32 reg_addr, u32 *reg_val)
{
	struct i2c_msg msg[2];	/* NVMe: 쓰기(주소) + 읽기(데이터) 2단계 메시지 */
	u8 wr_data[3];		/* NVMe: 읽을 레지스터 주소 버퍼 */
	u32 rd_data;		/* NVMe: I2C로부터 읽은 원시 데이터 */
	int ret;

	msg[0].addr = client->addr;	/* NVMe: TC9563 I2C 주소 */
	msg[0].len = 3;			/* NVMe: 24비트 레지스터 주소 길이 */
	msg[0].flags = 0;		/* NVMe: 쓰기 플래그 */

	/* Big Endian for reg addr */
	put_unaligned_be24(reg_addr, &wr_data[0]);
					/* NVMe: 읽을 레지스터 주소를 빅엔디안으로 변환 */

	msg[0].buf = wr_data;		/* NVMe: 쓰기 메시지 버퍼 */

	msg[1].addr = client->addr;	/* NVMe: 동일 TC9563 슬레이브 */
	msg[1].len = 4;			/* NVMe: 32비트 레지스터 값 길이 */
	msg[1].flags = I2C_M_RD;	/* NVMe: 읽기 플래그 */

	msg[1].buf = (u8 *)&rd_data;	/* NVMe: 읽기 메시지 버퍼, 리틀엔디안 데이터 수신 */

	ret = i2c_transfer(client->adapter, &msg[0], 2);
					/* NVMe: 쓰기+읽기 2단계 I2C 트랜잭션 수행 */
	if (ret == 2) {			/* NVMe: 두 메시지 모두 성공 */
		*reg_val = get_unaligned_le32(&rd_data);
					/* NVMe: 리틀엔디안 32비트 값을 CPU 순서로 변환하여 NVMe 설정 확인 */
		return 0;		/* NVMe: 정상 읽기 완료 */
	}

	/* If only one message successfully completed, return -EIO */
	return ret == 1 ? -EIO : ret;
					/* NVMe: 부분 전송 시 I/O 에러, 그 외에는 I2C 에러코드 전달 */
}

/* NVMe: 여러 TC9563 레지스터 설정을 연속으로 I2C에 쓰는 함수, NVMe 포트 초기화/파워오프 시퀀스에 사용 */
static int tc9563_pwrctrl_i2c_bulk_write(struct i2c_client *client,
				const struct tc9563_pwrctrl_reg_setting *seq,
				int len)
{
	int ret, i;		/* NVMe: 반환값과 루프 인덱스 */

	for (i = 0; i < len; i++) {	/* NVMe: 시퀀스의 모든 레지스터 설정 순회, NVMe 물리/전원 상태 단계별 전환 */
		ret = tc9563_pwrctrl_i2c_write(client, seq[i].offset, seq[i].val);
					/* NVMe: 시퀀스의 i번째 PCIe 레지스터 쓰기 */
		if (ret)		/* NVMe: 하나라도 실패하면 NVMe 포트 설정이 불완전해질 수 있음 */
			return ret;	/* NVMe: 즉시 중단하고 에러 전파 */
	}

	return 0;			/* NVMe: 전체 시퀀스 성공 */
}

/* NVMe: DT에서 비활성화된 DSP(NVMe 슬롯 후보)의 전원을 끄는 함수, NVMe가 연결되지 않은 포트 전력 절약 */
static int tc9563_pwrctrl_disable_port(struct tc9563_pwrctrl *tc9563,
				       enum tc9563_pwrctrl_ports port)
{
	struct tc9563_pwrctrl_cfg *cfg = &tc9563->cfg[port];
					/* NVMe: 대상 포트 설정, NVMe 사용 여부 포함 */
	const struct tc9563_pwrctrl_reg_setting *seq;
					/* NVMe: 사용할 파워오프 시퀀스 포인터 */
	int ret, len;			/* NVMe: 반환값과 시퀀스 길이 */

	if (!cfg->disable_port)		/* NVMe: 포트가 활성화되어 있으면(NVMe 사용 가능) 파워오프 생략 */
		return 0;

	if (port == TC9563_DSP1) {	/* NVMe: DSP1(NVMe 슬롯 1) 파워오프 시퀀스 선택 */
		seq = dsp1_pwroff_seq;
		len = ARRAY_SIZE(dsp1_pwroff_seq);
	} else {			/* NVMe: DSP2(NVMe 슬롯 2) 파워오프 시퀀스 선택 */
		seq = dsp2_pwroff_seq;
		len = ARRAY_SIZE(dsp2_pwroff_seq);
	}

	ret = tc9563_pwrctrl_i2c_bulk_write(tc9563->client, seq, len);
					/* NVMe: 포트별 파워오프 시퀀스 수행, NVMe 링크 전원 안전 차단 */
	if (ret)
		return ret;		/* NVMe: 포트 파워오프 실패 시 에러 전파, NVMe 장치 손상 방지를 위해 중단 */

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, common_pwroff_seq,
					     ARRAY_SIZE(common_pwroff_seq));
					/* NVMe: 공통 파워오프 시퀀스 수행, DSP 후 NVMe fabric 공유 회로 정리 */
}

/* NVMe: ASPM L0s/L1 진입 지연을 설정, NVMe SSD의 idle 절전 동작과 링크 복구 지연 튜닝 */
static int tc9563_pwrctrl_set_l0s_l1_entry_delay(struct tc9563_pwrctrl *tc9563,
						 enum tc9563_pwrctrl_ports port,
						 bool is_l1, u32 ns)
{
	u32 rd_val, units;		/* NVMe: 읽은 레지스터 값과 256ns 단위 변환값 */
	int ret;

	if (ns < TC9563_L0S_L1_DELAY_UNIT_NS)
		return 0;		/* NVMe: 지연이 단위 미만이면 NVMe 링크 튜닝 불필요 */

	/* convert to units of 256ns */
	units = ns / TC9563_L0S_L1_DELAY_UNIT_NS;
					/* NVMe: ns를 TC9563 레지스터 단위로 변환, NVMe ASPM 지연 정량화 */

	if (port == TC9563_ETHERNET) {	/* NVMe: 내장 이더넷 EP는 별도 레지스터 사용, NVMe와 동일 fabric이므로 영향 가능 */
		ret = tc9563_pwrctrl_i2c_read(tc9563->client,
					      TC9563_EMBEDDED_ETH_DELAY,
					      &rd_val);
					/* NVMe: 이더넷 ASPM 지연 레지스터 읽기 */
		if (ret)
			return ret;

		if (is_l1)
			rd_val = u32_replace_bits(rd_val, units,
						  TC9563_ETH_L1_DELAY_MASK);
					/* NVMe: 이더넷 L1 지연값 갱신, NVMe와 공유된 fabric의 전력 상태에 영향 */
		else
			rd_val = u32_replace_bits(rd_val, units,
						  TC9563_ETH_L0S_DELAY_MASK);
					/* NVMe: 이더넷 L0s 지연값 갱신 */

		return tc9563_pwrctrl_i2c_write(tc9563->client,
						TC9563_EMBEDDED_ETH_DELAY,
						rd_val);
					/* NVMe: 갱신된 이더넷 ASPM 지연값 쓰기 */
	}

	ret = tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_PORT_SELECT,
				       BIT(port));
					/* NVMe: L0s/L1 지연을 설정할 PCIe 포트(USP/DSP1/DSP2) 선택, NVMe 슬롯 후보 포함 */
	if (ret)
		return ret;

	return tc9563_pwrctrl_i2c_write(tc9563->client,
					is_l1 ? TC9563_PORT_L1_DELAY : TC9563_PORT_L0S_DELAY,
					units);
					/* NVMe: 선택된 포트의 L0s 또는 L1 지연값 쓰기, NVMe ASPM 타이밍 확정 */
}

/* NVMe: 특정 포트의 PCIe 송신 진폭(TX Margin)을 설정, NVMe SSD와의 신호 무결성/호환성 향상 */
static int tc9563_pwrctrl_set_tx_amplitude(struct tc9563_pwrctrl *tc9563,
					   enum tc9563_pwrctrl_ports port)
{
	u32 amp = tc9563->cfg[port].tx_amp;	/* NVMe: DT에서 읽은 목표 송신 진폭(µV) */
	int port_access;			/* NVMe: 포트 접근 활성화값, USP/DSP1/DSP2가 다름 */

	if (amp < TC9563_TX_MARGIN_MIN_UA)
		return 0;			/* NVMe: 최소값 미만이면 NVMe 링크 튜닝 생략 */

	/* txmargin = (Amp(uV) - 400000) / 3125 */
	amp = (amp - TC9563_TX_MARGIN_MIN_UA) / 3125;
						/* NVMe: TX Margin 레지스터 인코딩 공식 적용, NVMe SSD 맞춤 송신 세기 변환 */

	switch (port) {				/* NVMe: 포트별로 TC9563의 접근 코드가 다름 */
	case TC9563_USP:
		port_access = 0x1;		/* NVMe: 업스트림 포트(RC 측) 접근, NVMe 루트 컴플렉스 연결 */
		break;
	case TC9563_DSP1:
		port_access = 0x2;		/* NVMe: 다운스트림 포트1, NVMe SSD 슬롯 1 접근 */
		break;
	case TC9563_DSP2:
		port_access = 0x8;		/* NVMe: 다운스트림 포트2, NVMe SSD 슬롯 2 접근 */
		break;
	default:
		return -EINVAL;			/* NVMe: 이더넷 등 TX 진폭 미지원 포트 */
	}

	struct tc9563_pwrctrl_reg_setting tx_amp_seq[] = {
		{TC9563_PORT_ACCESS_ENABLE, port_access},
						/* NVMe: 대상 포트 설정 공간 접근 활성화 */
		{TC9563_PORT_LANE_ACCESS_ENABLE, 0x3},
						/* NVMe: 레인 0/1 접근, NVMe x1/x2 레인에 대응 */
		{TC9563_TX_MARGIN, amp},
						/* NVMe: 계산된 TX Margin 값 기록, NVMe SSD 송신 세기 조정 */
	};

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, tx_amp_seq,
					     ARRAY_SIZE(tx_amp_seq));
						/* NVMe: TX 진폭 설정 시퀀스를 TC9563에 일괄 쓰기 */
}

/* NVMe: DFE(Decision Feedback Equalizer)를 비활성화, 특정 NVMe SSD/케이블 환경에서 신호 문제 회피 */
static int tc9563_pwrctrl_disable_dfe(struct tc9563_pwrctrl *tc9563,
				      enum tc9563_pwrctrl_ports port)
{
	struct tc9563_pwrctrl_cfg *cfg = &tc9563->cfg[port];	/* NVMe: 대상 포트 설정 */
	int port_access, lane_access = 0x3;			/* NVMe: 포트 접근값과 기본 레인 접근(레인 0,1) */
	u32 phy_rate = 0x21;					/* NVMe: PHY 속도 변경 기본값, Gen 변경 시 NVMe 링크 안정화 */

	if (!cfg->disable_dfe)					/* NVMe: DFE 비활성화가 필요 없으면(NVMe 정상 동작) 생략 */
		return 0;

	switch (port) {						/* NVMe: 포트별 DFE 비활성화 시퀀스 파라미터 */
	case TC9563_USP:
		phy_rate = 0x1;					/* NVMe: USP용 PHY 속도 값 */
		port_access = 0x1;				/* NVMe: 업스트림 포트 접근, NVMe RC 연결 */
		break;
	case TC9563_DSP1:
		port_access = 0x2;				/* NVMe: DSP1, NVMe SSD 슬롯 1 */
		break;
	case TC9563_DSP2:
		port_access = 0x8;				/* NVMe: DSP2, NVMe SSD 슬롯 2 */
		lane_access = 0x1;				/* NVMe: DSP2는 레인 0만 접근, NVMe x1 연결 가정 */
		break;
	default:
		return -EINVAL;					/* NVMe: DFE 비활성화 대상이 아닌 포트 */
	}

	struct tc9563_pwrctrl_reg_setting disable_dfe_seq[] = {
		{TC9563_PORT_ACCESS_ENABLE, port_access},
							/* NVMe: 대상 포트 접근 활성화 */
		{TC9563_PORT_LANE_ACCESS_ENABLE, lane_access},
							/* NVMe: 대상 레인 접근 활성화, NVMe 연결 레인 선택 */
		{TC9563_DFE_ENABLE, 0x0},
							/* NVMe: DFE 기능 끄기, NVMe 레인의 이퀄라이저 동작 변경 */
		{TC9563_DFE_EQ0_MODE, 0x411},
							/* NVMe: DFE EQ0 모드 고정값, NVMe 신호 특성에 맞춤 */
		{TC9563_DFE_EQ1_MODE, 0x11},
							/* NVMe: DFE EQ1 모드 고정값 */
		{TC9563_DFE_EQ2_MODE, 0x11},
							/* NVMe: DFE EQ2 모드 고정값 */
		{TC9563_DFE_PD_MASK, 0x7},
							/* NVMe: DFE 블록 전원 다운 마스크, NVMe 링크 전력 절감 */
		{TC9563_PHY_RATE_CHANGE_OVERRIDE, 0x10},
							/* NVMe: PHY 속도 변경 오버라이드, NVMe Gen 협상 안정화 */
		{TC9563_PHY_RATE_CHANGE, phy_rate},
							/* NVMe: 포트별 PHY 속도 트리거 값, NVMe 링크 속도 변경 적용 */
		{TC9563_PHY_RATE_CHANGE, 0x0},
							/* NVMe: PHY 속도 변경 트리거 클리어, NVMe 링크 상태 안정화 */
		{TC9563_PHY_RATE_CHANGE_OVERRIDE, 0x0},
							/* NVMe: 오버라이드 해제, NVMe 정상 링크 동작 복원 */
	};

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, disable_dfe_seq,
					     ARRAY_SIZE(disable_dfe_seq));
							/* NVMe: DFE 비활성화 시퀀스 일괄 수행 */
}

/* NVMe: N_FTS(Number of Fast Training Sequences)를 설정, NVMe 링크 재훈련(recovery) 시간과 안정성 조절 */
static int tc9563_pwrctrl_set_nfts(struct tc9563_pwrctrl *tc9563,
				   enum tc9563_pwrctrl_ports port)
{
	u8 *nfts = tc9563->cfg[port].nfts;	/* NVMe: Gen1/Gen2 N_FTS 값 배열 포인터 */
	struct tc9563_pwrctrl_reg_setting nfts_seq[] = {
		{TC9563_NFTS_2_5_GT, nfts[0]},	/* NVMe: Gen1(2.5GT/s) N_FTS, NVMe SSD와의 L0s/L1 recovery에 사용 */
		{TC9563_NFTS_5_GT, nfts[1]},	/* NVMe: Gen2(5GT/s) N_FTS, NVMe SSD와의 Gen2 recovery에 사용 */
	};
	int ret;

	if (!nfts[0])				/* NVMe: N_FTS 값이 0이면 NVMe 링크 튜닝 생략 */
		return 0;

	ret =  tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_PORT_SELECT,
					BIT(port));
						/* NVMe: N_FTS를 설정할 포트 선택, NVMe 슬롯 후보 포함 */
	if (ret)
		return ret;

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, nfts_seq,
					     ARRAY_SIZE(nfts_seq));
						/* NVMe: Gen1/Gen2 N_FTS 값을 TC9563에 쓰기, NVMe SSD fast recovery 튜닝 */
}

/* NVMe: TC9563의 GPIO 기반 리셋을 어서트/디어서트, NVMe SSD의 PERST# 제어와 동일한 열거/재열거 효과 */
static int tc9563_pwrctrl_assert_deassert_reset(struct tc9563_pwrctrl *tc9563,
						bool deassert)
{
	int ret, val;

	ret = tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_GPIO_CONFIG,
				       TC9563_GPIO_MASK);
					/* NVMe: GPIO 설정 레지스터 초기화, 리셋 핀을 NVMe PERST#처럼 출력으로 구성 */
	if (ret)
		return ret;

	val = deassert ? TC9563_GPIO_DEASSERT_BITS : 0;
					/* NVMe: 디어서트 시 클리어 비트 설정, 어서트 시 0으로 NVMe 리셋 유지 */

	return tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_RESET_GPIO, val);
					/* NVMe: TC9563 RESET GPIO 출력을 통해 NVMe 연결 포트의 리셋 상태 제어 */
}

/* NVMe: 장치 트리에서 한 포트(NVMe 슬롯 후보 포함)의 전원/물리/ASPM 설정을 파싱 */
static int tc9563_pwrctrl_parse_device_dt(struct tc9563_pwrctrl *tc9563,
					  struct device_node *node,
					  enum tc9563_pwrctrl_ports port)
{
	struct tc9563_pwrctrl_cfg *cfg = &tc9563->cfg[port];	/* NVMe: 파싱 결과를 저장할 포트 설정 */
	int ret;

	/* Disable port if the status of the port is disabled. */
	if (!of_device_is_available(node)) {	/* NVMe: DT status="disabled" 포트는 NVMe가 연결되지 않은 것으로 처리 */
		cfg->disable_port = true;
		return 0;
	}

	ret = of_property_read_u32(node, "aspm-l0s-entry-delay-ns", &cfg->l0s_delay);
						/* NVMe: L0s 진입 지연(ns) 읽기, NVMe idle 절전 타이밍 */
	if (ret && ret != -EINVAL)
		return ret;				/* NVMe: -EINVAL 외의 에러는 치명적, NVMe 포트 설정 실패로 간주 */

	ret = of_property_read_u32(node, "aspm-l1-entry-delay-ns", &cfg->l1_delay);
						/* NVMe: L1 진입 지연(ns) 읽기, NVMe L1 절전 타이밍 */
	if (ret && ret != -EINVAL)
		return ret;

	ret = of_property_read_u32(node, "toshiba,tx-amplitude-microvolt", &cfg->tx_amp);
						/* NVMe: 송신 진폭(µV) 읽기, NVMe SSD 전기적 튜닝 */
	if (ret && ret != -EINVAL)
		return ret;

	ret = of_property_read_u8_array(node, "n-fts", cfg->nfts, ARRAY_SIZE(cfg->nfts));
						/* NVMe: Gen1/Gen2 N_FTS 배열 읽기, NVMe 링크 recovery 튜닝 */
	if (ret && ret != -EINVAL)
		return ret;

	cfg->disable_dfe = of_property_read_bool(node, "toshiba,no-dfe-support");
						/* NVMe: DFE 비활성화 플래그, 특정 NVMe 환경에서만 설정 */

	return 0;
						/* NVMe: 포트별 NVMe 연결 설정 파싱 완료 */
}

/* NVMe: PCI 전원 제어 코어에서 호출하는 power_off 콜백, NVMe 장치 제거/슬립 시 전원 차단 */
static int tc9563_pwrctrl_power_off(struct pci_pwrctrl *pwrctrl)
{
	struct tc9563_pwrctrl *tc9563 = container_of(pwrctrl,
						    struct tc9563_pwrctrl, pwrctrl);
						/* NVMe: pci_pwrctrl에서 tc9563 구조체 역참조 */

	gpiod_set_value(tc9563->reset_gpio, 1);
						/* NVMe: TC9563 리셋 어서트, NVMe SSD가 연결된 PCIe 포트를 리셋 상태로 */

	regulator_bulk_disable(ARRAY_SIZE(tc9563->supplies), tc9563->supplies);
						/* NVMe: TC9563 전원 레일 일괄 차단, NVMe 엔드포인트 전력 공급 중단 */

	return 0;
}

/* NVMe: PCI 전원 제어 코어에서 호출하는 power_on 콜백, NVMe 장치 열거 전 TC9563 및 포트 초기화 수행 */
static int tc9563_pwrctrl_power_on(struct pci_pwrctrl *pwrctrl)
{
	struct tc9563_pwrctrl *tc9563 = container_of(pwrctrl,
						    struct tc9563_pwrctrl, pwrctrl);
						/* NVMe: pci_pwrctrl에서 드라이버 인스턴스 역참조 */
	struct device *dev = tc9563->pwrctrl.dev;	/* NVMe: PCI/ACPI platform 장치, NVMe 열거 전 초기화 대상 */
	struct tc9563_pwrctrl_cfg *cfg;			/* NVMe: 현재 포트 설정 포인터 */
	int ret, i;					/* NVMe: 반환값과 포트 루프 인덱스 */

	ret = regulator_bulk_enable(ARRAY_SIZE(tc9563->supplies),
				    tc9563->supplies);
						/* NVMe: TC9563 전원 레일 일괄 활성화, NVMe SSD 구동 전 필수 */
	if (ret < 0)
		return dev_err_probe(dev, ret, "cannot enable regulators\n");
						/* NVMe: 레일 활성화 실패 시 NVMe 열거가 불가능하므로 probe 지연/실패 */

	gpiod_set_value(tc9563->reset_gpio, 0);
						/* NVMe: TC9563 하드웨어 리셋 해제, PCIe 클록/링크 가동 시작 */

	fsleep(TC9563_OSC_STAB_DELAY_US);
						/* NVMe: TC9563 낶부 발진기 안정화 대기, NVMe SSD와의 안정적인 PCIe 링크 형성을 위해 필요 */

	ret = tc9563_pwrctrl_assert_deassert_reset(tc9563, false);
						/* NVMe: TC9563 출력 리셋 어서트, NVMe SSD를 리셋 상태로 유지 */
	if (ret)
		goto power_off;				/* NVMe: 리셋 어서트 실패 시 전원 차단 후 에러 */

	for (i = 0; i < TC9563_MAX; i++) {	/* NVMe: 모든 PCIe 포트(USP/DSP1/DSP2/이더넷) 설정 순회 */
		cfg = &tc9563->cfg[i];			/* NVMe: i번째 포트 설정, NVMe SSD 슬롯 포함 */
		ret = tc9563_pwrctrl_disable_port(tc9563, i);
						/* NVMe: DT에서 disabled된 포트 전원 차단, NVMe 미연결 슬롯 전력 절약 */
		if (ret) {
			dev_err(dev, "Disabling port failed\n");
			goto power_off;			/* NVMe: 포트 비활성화 실패 시 NVMe 초기화 중단 및 전원 차단 */
		}

		ret = tc9563_pwrctrl_set_l0s_l1_entry_delay(tc9563, i, false, cfg->l0s_delay);
						/* NVMe: i번째 포트 L0s 지연 설정, NVMe idle 절전/복구 타이밍 튜닝 */
		if (ret) {
			dev_err(dev, "Setting L0s entry delay failed\n");
			goto power_off;			/* NVMe: L0s 설정 실패 시 NVMe ASPM 동작이 불안정해질 수 있음 */
		}

		ret = tc9563_pwrctrl_set_l0s_l1_entry_delay(tc9563, i, true, cfg->l1_delay);
						/* NVMe: i번째 포트 L1 지연 설정, NVMe L1 절전 진입/복구 타이밍 튜닝 */
		if (ret) {
			dev_err(dev, "Setting L1 entry delay failed\n");
			goto power_off;			/* NVMe: L1 설정 실패 시 NVMe 절전/성능에 영향 */
		}

		ret = tc9563_pwrctrl_set_tx_amplitude(tc9563, i);
						/* NVMe: i번째 포트 TX 진폭 설정, NVMe SSD 신호 강도 튜닝 */
		if (ret) {
			dev_err(dev, "Setting Tx amplitude failed\n");
			goto power_off;			/* NVMe: TX 진폭 설정 실패 시 NVMe 링크 훈련/안정성 문제 발생 가능 */
		}

		ret = tc9563_pwrctrl_set_nfts(tc9563, i);
						/* NVMe: i번째 포트 N_FTS 설정, NVMe 링크 fast recovery 튜닝 */
		if (ret) {
			dev_err(dev, "Setting N_FTS failed\n");
			goto power_off;			/* NVMe: N_FTS 설정 실패 시 NVMe 링크 재훈련 시간이 비정상적 */
		}

		ret = tc9563_pwrctrl_disable_dfe(tc9563, i);
						/* NVMe: i번째 포트 DFE 설정, 특정 NVMe 환경에서만 비활성화 */
		if (ret) {
			dev_err(dev, "Disabling DFE failed\n");
			goto power_off;			/* NVMe: DFE 설정 실패 시 NVMe 레인 이퀄라이저가 의도와 다름 */
		}
	}

	ret = tc9563_pwrctrl_assert_deassert_reset(tc9563, true);
						/* NVMe: 모든 설정 완료 후 TC9563 출력 리셋 디어서트, NVMe SSD가 PCIe enumeration에 응답 가능 */
	if (!ret)
		return 0;			/* NVMe: power_on 성공, 이후 PCI 버스 스캔에서 NVMe 장치 발견 가능 */

power_off:
	tc9563_pwrctrl_power_off(&tc9563->pwrctrl);
	return ret;				/* NVMe: 초기화 실패 시 TC9563 전원 차단, NVMe 열거 방지 */
}

/* NVMe: platform 드라이버 probe, TC9563와 I2C/전원/GPIO 연결 후 PCI 전원 제어 프레임워크에 등록 */
static int tc9563_pwrctrl_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;	/* NVMe: DT 루트 노드, NVMe 슬롯 설정 포함 */
	struct device *dev = &pdev->dev;		/* NVMe: platform 장치, 전원/핫플러그 컨텍스트 */
	enum tc9563_pwrctrl_ports port;			/* NVMe: 현재 파싱 중인 포트 식별자 */
	struct tc9563_pwrctrl *tc9563;			/* NVMe: 드라이버 인스턴스 */
	struct device_node *i2c_node;			/* NVMe: I2C 어댑터 phandle 노드 */
	int ret, addr;					/* NVMe: 반환값과 I2C 슬레이브 주소 */

	tc9563 = devm_kzalloc(dev, sizeof(*tc9563), GFP_KERNEL);
							/* NVMe: 드라이버 상태 메모리 할당, NVMe 포트별 설정 포함 */
	if (!tc9563)
		return -ENOMEM;					/* NVMe: 메모리 부족 시 NVMe 전원 제어 등록 불가 */

	ret = of_property_read_u32_index(node, "i2c-parent", 1, &addr);
							/* NVMe: "i2c-parent" 두 번째 셀에서 TC9563 I2C 주소 읽기, NVMe 설정 백도어 접근 */
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read i2c-parent property\n");

	i2c_node = of_parse_phandle(dev->of_node, "i2c-parent", 0);
							/* NVMe: I2C 어댑터 phandle 파싱, TC9563에 접근할 버스 */
	tc9563->adapter = of_find_i2c_adapter_by_node(i2c_node);
							/* NVMe: phandle로 I2C 어댑터 검색, TC9563와 통신할 버스 확보 */
	of_node_put(i2c_node);
							/* NVMe: 참조 카운트 감소, 메모리 누수 방지 */
	if (!tc9563->adapter)
		return dev_err_probe(dev, -EPROBE_DEFER, "Failed to find I2C adapter\n");
							/* NVMe: I2C 어댑터 미준비 시 deferred probe, NVMe 열거 지연 */

	tc9563->client = i2c_new_dummy_device(tc9563->adapter, addr);
							/* NVMe: TC9563 I2C 주소로 더미 클라이언트 생성, PCIe 레지스터 R/W용 */
	if (IS_ERR(tc9563->client)) {
		dev_err(dev, "Failed to create I2C client\n");
		put_device(&tc9563->adapter->dev);
							/* NVMe: I2C 클라이언트 생성 실패 시 어댑터 참조 해제 */
		return PTR_ERR(tc9563->client);
	}

	for (int i = 0; i < ARRAY_SIZE(tc9563_supply_names); i++)
							/* NVMe: TC9563 전원 레일 이름을 bulk_data에 채움, NVMe 장치 전원 준비 */
		tc9563->supplies[i].supply = tc9563_supply_names[i];

	ret = devm_regulator_bulk_get(dev, TC9563_PWRCTL_MAX_SUPPLY,
				      tc9563->supplies);
							/* NVMe: 커널에서 전원 레일 일괄 조회, NVMe SSD 구동 전 레일 확보 */
	if (ret) {
		dev_err_probe(dev, ret, "failed to get supply regulator\n");
		goto remove_i2c;			/* NVMe: 레일 조회 실패 시 I2C 정리 후 에러 */
	}

	tc9563->reset_gpio = devm_gpiod_get(dev, "resx", GPIOD_OUT_HIGH);
							/* NVMe: TC9563 리셋 GPIO 확보, 기본 HIGH(어서트), NVMe SSD는 리셋 상태로 시작 */
	if (IS_ERR(tc9563->reset_gpio)) {
		ret = dev_err_probe(dev, PTR_ERR(tc9563->reset_gpio), "failed to get resx GPIO\n");
		goto remove_i2c;			/* NVMe: GPIO 확보 실패 시 NVMe PERST# 제어 불가 */
	}

	pci_pwrctrl_init(&tc9563->pwrctrl, dev);
							/* NVMe: PCI 전원 제어 구조체 초기화, NVMe 장치의 전원 생명주기와 연결 */

	port = TC9563_USP;					/* NVMe: 업스트림 포트부터 DT 파싱 시작, NVMe RC 연결 설정 */
	ret = tc9563_pwrctrl_parse_device_dt(tc9563, node, port);
							/* NVMe: 루트 노드에서 USP 설정 파싱 */
	if (ret) {
		dev_err(dev, "failed to parse device tree properties: %d\n", ret);
		goto remove_i2c;			/* NVMe: USP 설정 파싱 실패 시 NVMe 초기화 불가 */
	}

	/*
	 * Downstream ports are always children of the upstream port.
	 * The first node represents DSP1, the second node represents DSP2,
	 * and so on.
	 */
	for_each_child_of_node_scoped(node, child) {	/* NVMe: USP 아래 자식 노드 순회, NVMe SSD 슬롯(DSP1/2) 설정 파싱 */
		port++;					/* NVMe: 다음 다운스트림 포트로 이동 */
		ret = tc9563_pwrctrl_parse_device_dt(tc9563, child, port);
							/* NVMe: DSP 노드에서 NVMe 연결 포트 설정 파싱 */
		if (ret)
			break;				/* NVMe: 파싱 에러 시 즉시 중단, 잘못된 NVMe 포트 설정 방지 */
		/* Embedded ethernet device are under DSP3 */
		if (port == TC9563_DSP3) {		/* NVMe: DSP3 아래 내장 이더넷이 있는 경우, NVMe와 동일 fabric의 추가 EP */
			for_each_child_of_node_scoped(child, child1) {
				port++;
				ret = tc9563_pwrctrl_parse_device_dt(tc9563,
								child1, port);
							/* NVMe: 이더넷 포트 설정 파싱, NVMe fabric의 다른 EP 전원/ASPM 설정 */
				if (ret)
					break;		/* NVMe: 이더넷 설정 파싱 실패 시 NVMe 초기화도 중단 */
			}
		}
	}
	if (ret) {
		dev_err(dev, "failed to parse device tree properties: %d\n", ret);
		goto remove_i2c;			/* NVMe: DT 파싱 실패 시 NVMe 장치 등록 전 정리 */
	}

	tc9563->pwrctrl.power_on = tc9563_pwrctrl_power_on;
							/* NVMe: power_on 콜백 등록, NVMe 장치 열거 직전 커널이 호출 */
	tc9563->pwrctrl.power_off = tc9563_pwrctrl_power_off;
							/* NVMe: power_off 콜백 등록, NVMe 장치 제거/슬립 시 커널이 호출 */

	ret = devm_pci_pwrctrl_device_set_ready(dev, &tc9563->pwrctrl);
							/* NVMe: PCI 전원 제어 프레임워크에 등록, NVMe PCIe 호스트가 이제 TC9563 전원 관리 가능 */
	if (ret)
		goto power_off;				/* NVMe: 등록 실패 시 TC9563 전원 차단 */

	return 0;					/* NVMe: probe 성공, NVMe 장치가 이후 PCIe enumeration에 참여 가능 */

power_off:
	tc9563_pwrctrl_power_off(&tc9563->pwrctrl);
							/* NVMe: probe 실패 시 TC9563 하드웨어 전원 차단, NVMe 링크 비활성 */
remove_i2c:
	i2c_unregister_device(tc9563->client);
							/* NVMe: I2C 더미 클라이언트 제거, NVMe 설정 백도어 해제 */
	put_device(&tc9563->adapter->dev);
							/* NVMe: I2C 어댑터 참조 해제 */
	return ret;
}

/* NVMe: platform 드라이버 remove, TC9563 전원 차단 및 I2C/GPIO 자원 정리, NVMe 장치 종료 */
static void tc9563_pwrctrl_remove(struct platform_device *pdev)
{
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(&pdev->dev);
							/* NVMe: platform drvdata에서 pci_pwrctrl 획득 */
	struct tc9563_pwrctrl *tc9563 = container_of(pwrctrl,
					struct tc9563_pwrctrl, pwrctrl);
							/* NVMe: pci_pwrctrl에서 tc9563 인스턴스 역참조 */

	tc9563_pwrctrl_power_off(&tc9563->pwrctrl);
							/* NVMe: TC9563 및 연결된 NVMe 포트 전원 차단 */
	i2c_unregister_device(tc9563->client);
							/* NVMe: I2C 더미 클라이언트 해제, NVMe 레지스터 접근 중단 */
	put_device(&tc9563->adapter->dev);
							/* NVMe: I2C 어댑터 참조 해제 */
}

/* NVMe: DT 호환성 테이블, "pci1179,0623"은 TC9563 PCIe 리타이머/스위치, NVMe 호스트와 PCIe fabric 연결 */
static const struct of_device_id tc9563_pwrctrl_of_match[] = {
	{ .compatible = "pci1179,0623"},
	{ }
};
MODULE_DEVICE_TABLE(of, tc9563_pwrctrl_of_match);

/* NVMe: platform_driver 정의, probe_type 비동기로 NVMe 장치 열거 지연 최소화 */
static struct platform_driver tc9563_pwrctrl_driver = {
	.driver = {
		.name = "pwrctrl-tc9563",
							/* NVMe: 드라이버 이름 */
		.of_match_table = tc9563_pwrctrl_of_match,
							/* NVMe: DT 매칭 테이블, NVMe 시스템에서 TC9563 인식 */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
							/* NVMe: 비동기 probe, NVMe SSD 열거 시 병렬 초기화로 부팅 시간 단축 */
	},
	.probe = tc9563_pwrctrl_probe,
							/* NVMe: probe 함수, TC9563 전원 제어 등록 */
	.remove = tc9563_pwrctrl_remove,
							/* NVMe: remove 함수, TC9563 전원 제거 */
};
module_platform_driver(tc9563_pwrctrl_driver);

MODULE_AUTHOR("Krishna chaitanya chundru <quic_krichai@quicinc.com>");
MODULE_DESCRIPTION("TC956x power control driver");
MODULE_LICENSE("GPL");
