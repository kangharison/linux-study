// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * [한국어 설명] TC9563 PCIe 스위치의 전원과 초기 설정 (pwrctrl/pci-pwrctrl-tc9563.c)
 *
 * === 파일의 역할 ===
 * TC9563 은 PCIe 스위치 칩이다. 상류 포트 하나를 받아 하류 포트 여럿으로
 * 나눠 주며, 그 하류에 NVMe SSD 같은 엔드포인트가 붙는다.
 *
 * 이 칩은 전원을 넣는 것만으로는 동작하지 않는다. I2C 로 접속해 내부
 * 레지스터를 초기화해야 하고, 그 설정에 클럭 소스 선택, 하류 포트의
 * 링크 파라미터, ASPM 과 L1 substates 동작이 포함된다. 이 파일이 그 절차를
 * 담당한다.
 *
 * pwrctrl 프레임워크 위에 얹혀 있다는 점이 구조의 핵심이다 — 전원을
 * 넣고 초기화를 마친 뒤 pwrctrl 코어에 "준비됐다" 고 알리면, 그제서야
 * PCI 열거가 시작되어 스위치와 그 아래 장치들이 발견된다. 순서가 반대면
 * 초기화되지 않은 스위치를 열거하게 되어 하류가 보이지 않는다.
 *
 * I2C 를 쓴다는 점도 눈여겨볼 만하다. PCIe 링크가 아직 살아 있지 않은
 * 시점에 칩을 설정해야 하므로, PCI config space 로는 접근할 수 없다.
 * 그래서 완전히 다른 버스인 I2C 를 사이드밴드로 쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * DT 에 이 스위치가 기술되어 있으면
 *   -> pwrctrl/core.c 가 platform device 를 만들고
 *      -> [이 파일] probe
 *         -> 레귤레이터/클럭/리셋 제어로 칩에 전원 인가
 *         -> I2C 로 내부 레지스터 초기화
 *         -> pwrctrl 코어에 준비 완료 통보
 *            -> 버스 재스캔 -> 스위치와 하류 장치 열거
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(probe). I2C 전송과 지연 대기가 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 플랫폼 드라이버 코어, pwrctrl/core.c 의 인프라.
 * 아래쪽: I2C 서브시스템, regulator / clk / reset 프레임워크.
 * 공유 상태: 이 드라이버의 사설 구조체와 struct pci_pwrctrl.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버와 직접 관련이 없다(전수 확인).
 *
 * 다만 이 스위치의 하류에 NVMe 가 붙는 구성이라면, 이 파일이 설정하는
 * 하류 포트 파라미터가 그 NVMe 의 링크 속도와 ASPM 동작을 좌우한다.
 * 스위치가 낮은 속도로 설정되면 아무리 빠른 SSD 를 꽂아도 그 속도로
 * 협상된다(pci.c 의 pcie_bandwidth_available 주석 참고 — 경로에서 가장
 * 좁은 구간이 전체를 결정한다).
 *
 * === 주요 함수/구조체 요약 ===
 * (초기화 절차가 길어 단계별로 함수가 나뉘어 있다. 각 함수의 주석 참고.)
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pci.h>
#include <linux/pci-pwrctrl.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/unaligned.h>

#include "../pci.h"

#define TC9563_GPIO_CONFIG		0x801208
#define TC9563_RESET_GPIO		0x801210

#define TC9563_PORT_L0S_DELAY		0x82496c
#define TC9563_PORT_L1_DELAY		0x824970

#define TC9563_EMBEDDED_ETH_DELAY	0x8200d8
#define TC9563_ETH_L1_DELAY_MASK	GENMASK(27, 18)
#define TC9563_ETH_L1_DELAY_VALUE(x)	FIELD_PREP(TC9563_ETH_L1_DELAY_MASK, x)
#define TC9563_ETH_L0S_DELAY_MASK	GENMASK(17, 13)
#define TC9563_ETH_L0S_DELAY_VALUE(x)	FIELD_PREP(TC9563_ETH_L0S_DELAY_MASK, x)

#define TC9563_NFTS_2_5_GT		0x824978
#define TC9563_NFTS_5_GT		0x82497c

#define TC9563_PORT_LANE_ACCESS_ENABLE	0x828000

#define TC9563_PHY_RATE_CHANGE_OVERRIDE	0x828040
#define TC9563_PHY_RATE_CHANGE		0x828050

#define TC9563_TX_MARGIN		0x828234

#define TC9563_DFE_ENABLE		0x828a04
#define TC9563_DFE_EQ0_MODE		0x828a08
#define TC9563_DFE_EQ1_MODE		0x828a0c
#define TC9563_DFE_EQ2_MODE		0x828a14
#define TC9563_DFE_PD_MASK		0x828254

#define TC9563_PORT_SELECT		0x82c02c
#define TC9563_PORT_ACCESS_ENABLE	0x82c030

#define TC9563_POWER_CONTROL		0x82b09c
#define TC9563_POWER_CONTROL_OVREN	0x82b2c8

#define TC9563_GPIO_MASK		0xfffffff3
#define TC9563_GPIO_DEASSERT_BITS	0xc  /* Clear to deassert GPIO */

#define TC9563_TX_MARGIN_MIN_UA		400000

/*
 * From TC9563 PORSYS rev 0.2, figure 1.1 POR boot sequence
 * wait for 10ms for the internal osc frequency to stabilize.
 */
#define TC9563_OSC_STAB_DELAY_US	(10 * USEC_PER_MSEC)

#define TC9563_L0S_L1_DELAY_UNIT_NS	256  /* Each unit represents 256 ns */

struct tc9563_pwrctrl_reg_setting {
	/* [한국어] 쓸 레지스터의 24비트 주소.
	 * 설정자: 이 파일의 정적 설정 표들이 초기화로 채운다.
	 * 읽는 자: tc9563_pwrctrl_i2c_bulk_write() 가 순서대로 꺼내 쓴다.
	 * 값 범위: 파일 앞머리의 TC9563_ 매크로들이 정의한 주소, 또는 표에 직접 적힌 값.
	 * 동기화: const 정적 배열이거나 함수 지역 배열이라 공유되지 않는다. */
	unsigned int offset;
	/* [한국어] 그 주소에 쓸 값.
	 * 설정자: 위와 같다.
	 * 읽는 자: 위와 같다.
	 * 값 범위: 대부분 데이터시트가 정한 매직 값이라 이 트리에서 의미를 확인 못 함.
	 * 동기화: 위와 같다. */
	unsigned int val;
/* [한국어] 레지스터 하나에 값을 쓰는 설정 한 줄. 이 파일의 설정이 대부분 이 표의 나열이다. */
};

enum tc9563_pwrctrl_ports {
	/* [한국어] 업스트림 포트 — 호스트 쪽으로 향하는 포트다. 아래 열거 순서가
	 * 디바이스 트리 노드의 배치와 정확히 맞아야 한다(probe 가 그 순서로 훑는다). */
	TC9563_USP,
	TC9563_DSP1,
	TC9563_DSP2,
	TC9563_DSP3,
	TC9563_ETHERNET,
	TC9563_MAX
};

struct tc9563_pwrctrl_cfg {
	/* [한국어] L0s 진입 지연(나노초).
	 * 설정자: tc9563_pwrctrl_parse_device_dt() 가 aspm-l0s-entry-delay-ns 에서 읽는다.
	 * 읽는 자: tc9563_pwrctrl_set_l0s_l1_entry_delay().
	 * 값 범위: 0 이면 설정하지 않음. 256ns 미만도 단위 변환에서 0 이 되어 무시된다.
	 * 동기화: probe 후 불변. */
	u32 l0s_delay;
	/* [한국어] L1 진입 지연(나노초).
	 * 설정자·읽는 자: 위와 같고 속성 이름만 aspm-l1-entry-delay-ns 다.
	 * 값 범위: 위와 같다.
	 * 동기화: probe 후 불변. */
	u32 l1_delay;
	/* [한국어] 송신 신호 진폭(마이크로암페어).
	 * 설정자: 디바이스 트리의 toshiba,tx-amplitude-microvolt 에서 읽는다.
	 * 읽는 자: tc9563_pwrctrl_set_tx_amplitude().
	 * 값 범위: 400000 미만이면 설정하지 않는다 — 그 값이 레지스터의 0 에 해당한다.
	 * 속성 이름은 microvolt 인데 코드가 다루는 단위는 마이크로암페어다.
	 * 동기화: probe 후 불변. */
	u32 tx_amp;
	/* [한국어] GEN1·GEN2 각각의 N_FTS 값(옆의 상류 주석).
	 * 설정자: 디바이스 트리의 n-fts 배열에서 읽는다.
	 * 읽는 자: tc9563_pwrctrl_set_nfts().
	 * 값 범위: [0] 이 0 이면 둘 다 적용하지 않는다 — GEN2 만 지정한 경우도 그 검사에 걸린다.
	 * 동기화: probe 후 불변. */
	u8 nfts[2]; /* GEN1 & GEN2 */
	/* [한국어] 이 포트의 DFE 를 꺼야 하는지.
	 * 설정자: 디바이스 트리에 toshiba,no-dfe-support 가 있으면 참.
	 * 읽는 자: tc9563_pwrctrl_disable_dfe() 가 맨 앞에서 확인한다.
	 * 값 범위: 참이면 끈다. 짧고 깨끗한 배선에서는 DFE 가 오히려 링크를 흔들 수 있다.
	 * 동기화: probe 후 불변. */
	bool disable_dfe;
	/* [한국어] 이 포트를 아예 꺼야 하는지.
	 * 설정자: 노드가 사용 불가로 표시돼 있으면 참.
	 * 읽는 자: tc9563_pwrctrl_disable_port().
	 * 값 범위: 참이면 그 포트의 전원 시퀀스를 쓴다.
	 * 동기화: probe 후 불변. */
	bool disable_port;
/* [한국어] 포트 하나의 설정 묶음. 디바이스 트리에서 읽어 전원 켜기 때 하드웨어에 넣는다. */
};

#define TC9563_PWRCTL_MAX_SUPPLY	6

static const char *const tc9563_supply_names[TC9563_PWRCTL_MAX_SUPPLY] = {
	/* [한국어] 코어 전압. */
	"vddc",
	/* [한국어] 1.8V 계열. 아래 이름들의 **순서가 곧 supplies 배열의 순서** 가 되므로
	 * 바꾸면 전압과 이름의 짝이 어긋난다. */
	"vdd18",
	"vdd09",
	"vddio1",
	"vddio2",
	"vddio18",
};

struct tc9563_pwrctrl {
	/* [한국어] pwrctrl 코어가 다루는 부분. **맨 앞에 두어** container_of 로 바깥을 되찾는다.
	 * 설정자: pci_pwrctrl_init() 이 초기화하고 probe 가 콜백 둘을 채운다.
	 * 읽는 자: pwrctrl 코어가 전원 켜기·끄기 때 이 문맥을 콜백에 넘긴다.
	 * 값 범위: 코어가 정의한 구조체.
	 * 동기화: 코어가 관리한다. */
	struct pci_pwrctrl pwrctrl;
	/* [한국어] 여섯 개의 전압 공급.
	 * 설정자: probe 가 이름을 채우고 devm_regulator_bulk_get() 이 나머지를 채운다.
	 * 읽는 자: 전원 켜기·끄기가 통째로 켜고 끈다.
	 * 값 범위: 배열 순서가 tc9563_supply_names 의 순서와 같다.
	 * 동기화: probe 후 불변이며, 켜고 끄는 것은 regulator 코어가 지킨다. */
	struct regulator_bulk_data supplies[TC9563_PWRCTL_MAX_SUPPLY];
	/* [한국어] 포트별 설정. 색인이 enum tc9563_pwrctrl_ports 다.
	 * 설정자: tc9563_pwrctrl_parse_device_dt() 가 포트마다 채운다.
	 * 읽는 자: 전원 켜기의 설정 함수들.
	 * 값 범위: 읽지 않은 필드는 0 으로 남고, 각 설정 함수가 그 0 을 '설정하지 않음' 으로 읽는다.
	 * 동기화: probe 후 불변. */
	struct tc9563_pwrctrl_cfg cfg[TC9563_MAX];
	/* [한국어] 드라이버가 직접 잡은 칩 리셋 GPIO.
	 * 설정자: probe 의 devm_gpiod_get(). 초기값이 HIGH — 즉 리셋이 걸린 상태다.
	 * 읽는 자: 전원 켜기가 0 을 써서 풀고, 끄기가 1 을 써서 건다.
	 * 값 범위: 유효한 GPIO 서술자.
	 * 동기화: probe 후 불변.
	 * 스위치가 **내보내는** GPIO(I2C 로 조작하는 것)와는 다른 것이다. */
	struct gpio_desc *reset_gpio;
	/* [한국어] I2C 어댑터. 참조를 들고 있다.
	 * 설정자: probe 의 of_find_i2c_adapter_by_node() — 참조를 올려 준다.
	 * 읽는 자: 클라이언트를 만들 때, 그리고 remove 가 참조를 놓을 때.
	 * 값 범위: 유효한 어댑터 포인터.
	 * 동기화: probe 후 불변. **devres 가 아니라 remove 가 직접 놓는다.** */
	struct i2c_adapter *adapter;
	/* [한국어] 이 스위치를 가리키는 I2C 클라이언트.
	 * 설정자: probe 의 i2c_new_dummy_device() — 드라이버 없이 주소만 차지하는 클라이언트다.
	 * 읽는 자: 이 파일의 모든 레지스터 접근.
	 * 값 범위: 유효한 클라이언트 포인터.
	 * 동기화: probe 후 불변. remove 가 직접 없앤다. */
	struct i2c_client *client;
/* [한국어] 이 드라이버의 상태 전부. */
};

/*
 * downstream port power off sequence, hardcoding the address
 * as we don't know register names for these register offsets.
 */
static const struct tc9563_pwrctrl_reg_setting common_pwroff_seq[] = {
	{0x82900c, 0x1},
	/* [한국어] 이하 전원 차단 시퀀스의 값들은 데이터시트가 정한 것으로,
	 * 각 주소가 무엇인지는 이 트리에서 확인 못 함. 순서가 의미를 갖는다. */
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

static const struct tc9563_pwrctrl_reg_setting dsp1_pwroff_seq[] = {
	/* [한국어] DSP1 접근을 연다 — 0x2 가 DSP1 의 비트다. */
	{TC9563_PORT_ACCESS_ENABLE, 0x2},
	/* [한국어] 레인 둘 다 접근 대상으로 연다. */
	{TC9563_PORT_LANE_ACCESS_ENABLE, 0x3},
	{TC9563_POWER_CONTROL, 0x014f4804},
	{TC9563_POWER_CONTROL_OVREN, 0x1},
	{TC9563_PORT_ACCESS_ENABLE, 0x4},
};

static const struct tc9563_pwrctrl_reg_setting dsp2_pwroff_seq[] = {
	/* [한국어] DSP2 접근을 연다 — 0x8 이 DSP2 의 비트다. 0x4 자리가 비어 있는데
	 * 그것이 무엇인지는 이 트리에서 확인 못 함. */
	{TC9563_PORT_ACCESS_ENABLE, 0x8},
	/* [한국어] DSP2 는 레인 하나만 연다. */
	{TC9563_PORT_LANE_ACCESS_ENABLE, 0x1},
	{TC9563_POWER_CONTROL, 0x014f4804},
	{TC9563_POWER_CONTROL_OVREN, 0x1},
	{TC9563_PORT_ACCESS_ENABLE, 0x8},
};

/*
 * Since all transfers are initiated by the probe, no locks are necessary,
 * as there are no concurrent calls.
 */
/* [한국어]
 * tc9563_pwrctrl_i2c_write - 스위치의 레지스터 하나에 I2C 로 쓴다
 *
 * @client: I2C 클라이언트.
 * @reg_addr: 24비트 레지스터 주소.
 * @reg_val: 쓸 32비트 값.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 파일의 모든 하드웨어 접근이 이 함수를 거친다. TC9563 은 PCIe 스위치인데,
 * 설정 레지스터를 PCIe 가 아니라 **I2C 로** 다룬다 — 링크가 서기 전에
 * 설정해야 하는 값들이라 PCIe 로는 닿을 수 없기 때문이다.
 *
 * 메시지가 7바이트 한 덩어리다. 주소 3바이트와 값 4바이트가 이어 붙으며,
 * 그 사이에 재시작이 없다.
 *
 * 엔디언이 주소와 값에서 서로 다르다 — 주소는 빅엔디언 24비트, 값은
 * 리틀엔디언 32비트다. 칩이 정한 형식이며, 그래서 두 종류의 put_unaligned
 * 헬퍼를 쓴다.
 *
 * i2c_transfer() 는 성공한 메시지 **개수** 를 돌려주므로, 1 이 아니면 실패다.
 * 그 값을 그대로 올려보내면 호출자가 양수를 성공으로 오인하므로 0 으로 바꾼다.
 *
 * 실행 컨텍스트: 전원 켜기·끄기 경로. I2C 전송이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 전송 실패의 음수 오류를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   이 파일의 설정 함수 전부 → [이 함수]
 *     → put_unaligned_be24() → put_unaligned_le32() → i2c_transfer()
 */
static int tc9563_pwrctrl_i2c_write(struct i2c_client *client,
				    u32 reg_addr, u32 reg_val)
{
	struct i2c_msg msg;
	u8 msg_buf[7];
	/* [한국어] i2c_transfer() 의 결과 — 성공한 메시지 개수다. */
	int ret;

	msg.addr = client->addr;
	/* [한국어] 주소 3바이트 + 값 4바이트. */
	msg.len = 7;
	/* [한국어] 0 은 쓰기 방향이다. */
	msg.flags = 0;

	/* Big Endian for reg addr */
	put_unaligned_be24(reg_addr, &msg_buf[0]);

	/* Little Endian for reg val */
	put_unaligned_le32(reg_val, &msg_buf[3]);

	msg.buf = msg_buf;
	/* [한국어] 메시지 하나를 보낸다. */
	ret = i2c_transfer(client->adapter, &msg, 1);
	/* [한국어] 성공한 개수가 1 이 아니면 실패다. 그대로 올려보내면 호출자가 양수를
	 * 성공으로 오인하므로 0 으로 바꾼다. */
	return ret == 1 ? 0 : ret;
}

/* [한국어]
 * tc9563_pwrctrl_i2c_read - 스위치의 레지스터 하나를 I2C 로 읽는다
 *
 * @client: I2C 클라이언트.
 * @reg_addr: 24비트 레지스터 주소.
 * @reg_val: 읽은 값을 담을 자리.
 * @return: 0 = 성공, -EIO 또는 음수 오류.
 *
 * 쓰기와 달리 메시지가 둘이다 — 주소를 쓰는 메시지와 값을 읽는 메시지.
 * 그 둘 사이에 I2C 재시작이 들어가며, i2c_transfer() 에 배열로 한 번에
 * 넘겨야 그 사이에 다른 주체가 버스를 가져가지 않는다.
 *
 * 두 번째 메시지에 I2C_M_RD 를 세우는 것이 읽기 방향 표시다.
 *
 * 읽은 4바이트를 리틀엔디언으로 해석한다. 쓰기 쪽의 값 형식과 같다.
 *
 * 반환값 처리가 세 갈래다 — 2 면 성공, 1 이면 주소는 갔는데 읽기가 실패한
 * 것이라 -EIO, 그 밖은 전송 자체의 오류다. 1 을 따로 다루는 이유는
 * i2c_transfer() 가 그때 양수를 돌려주기 때문이다.
 *
 * 이 파일에서 읽기가 필요한 곳은 한 군데뿐이다 — 이더넷 포트의 지연 값을
 * 읽기-수정-쓰기로 갱신하는 자리다.
 *
 * 실행 컨텍스트: 전원 켜기 경로. I2C 전송이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 부분 성공은 -EIO, 그 밖은 전송 오류를 올려보낸다.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_set_l0s_l1_entry_delay() → [이 함수]
 *     → put_unaligned_be24() → i2c_transfer() → get_unaligned_le32()
 */
static int tc9563_pwrctrl_i2c_read(struct i2c_client *client,
				   u32 reg_addr, u32 *reg_val)
{
	struct i2c_msg msg[2];
	u8 wr_data[3];
	/* [한국어] 읽어 올 4바이트를 담을 자리. */
	u32 rd_data;
	/* [한국어] 전송 결과. */
	int ret;

	msg[0].addr = client->addr;
	/* [한국어] 주소 3바이트만 보낸다. */
	msg[0].len = 3;
	/* [한국어] 0 은 쓰기 방향 — 주소를 알리는 단계다. */
	msg[0].flags = 0;

	/* Big Endian for reg addr */
	put_unaligned_be24(reg_addr, &wr_data[0]);

	msg[0].buf = wr_data;
/* [한국어] 주소 버퍼를 붙인다. */

	msg[1].addr = client->addr;
	/* [한국어] 값 4바이트를 받는다. */
	msg[1].len = 4;
	/* [한국어] 읽기 방향 표시. 이 플래그가 두 메시지 사이에 I2C 재시작을 만든다. */
	msg[1].flags = I2C_M_RD;

	msg[1].buf = (u8 *)&rd_data;
/* [한국어] 두 메시지를 **한 번에** 넘긴다. 그래야 그 사이에 다른 주체가 버스를 가져가지 않는다. */

	ret = i2c_transfer(client->adapter, &msg[0], 2);
	/* [한국어] 둘 다 성공했으면, */
	if (ret == 2) {
		*reg_val = get_unaligned_le32(&rd_data);
		return 0;
	}

	/* If only one message successfully completed, return -EIO */
	return ret == 1 ? -EIO : ret;
}

/* [한국어]
 * tc9563_pwrctrl_i2c_bulk_write - 레지스터 설정 표를 순서대로 쓴다
 *
 * @client: I2C 클라이언트.
 * @seq: 오프셋·값 쌍의 배열.
 * @len: 그 개수.
 * @return: 0 = 성공, 첫 실패의 오류.
 *
 * 이 파일의 설정이 대부분 "이 순서로 이 값들을 써라" 형태라, 그것을 표로
 * 두고 이 함수가 한 줄씩 쓴다.
 *
 * 순서가 의미를 갖는다. 예를 들어 포트 접근을 먼저 열고 그 다음에 그 포트의
 * 레지스터를 쓰는 식이라, 표의 순서를 바꾸면 엉뚱한 포트에 쓰게 된다.
 *
 * 하나라도 실패하면 즉시 멈춘다. 되감기가 없는데, 절반만 적용된 설정을
 * 되돌리는 방법이 없기 때문이다 — 호출자는 전원을 끄는 것으로 대응한다.
 *
 * 실행 컨텍스트: 전원 켜기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 첫 실패의 오류를 올려보내며, 그때까지 쓴 것은 그대로 남는다.
 *
 * 호출 체인:
 *   이 파일의 설정 함수들 → [이 함수] → tc9563_pwrctrl_i2c_write()
 */
static int tc9563_pwrctrl_i2c_bulk_write(struct i2c_client *client,
				const struct tc9563_pwrctrl_reg_setting *seq,
				int len)
{
	int ret, i;

	for (i = 0; i < len; i++) {
		/* [한국어] 표의 한 줄을 쓴다. */
		ret = tc9563_pwrctrl_i2c_write(client, seq[i].offset, seq[i].val);
		/* [한국어] 하나라도 실패하면, */
		if (ret)
			/* [한국어] 즉시 멈춘다. 절반만 적용된 설정을 되돌릴 방법이 없어,
			 * 호출자가 전원을 끄는 것으로 대응한다. */
			return ret;
	}

	return 0;
}

/* [한국어]
 * tc9563_pwrctrl_disable_port - 쓰지 않는 다운스트림 포트의 전원을 끊는다
 *
 * @tc9563: 드라이버 상태.
 * @port: 대상 포트.
 * @return: 0 = 성공(또는 끌 필요 없음), 음수 오류.
 *
 * 디바이스 트리에서 사용 불가로 표시된 포트를 끈다. 쓰지 않는 포트를 켜 두면
 * 전력만 쓰고, 그 포트의 PHY 가 잡음을 낼 수도 있다.
 *
 * 표가 두 벌인 것이 이 함수의 구조다 — 포트마다 다른 부분(dsp1 또는 dsp2)을
 * 먼저 쓰고, 공통 부분을 뒤에 쓴다.
 *
 * DSP3 과 이더넷은 이 갈림에 없다. DSP1 이 아닌 모든 포트가 dsp2 쪽 표로
 * 가므로, DSP3 이나 이더넷에 대해 이 함수가 불리면 dsp2 의 표가 쓰인다.
 * 실제로 그 두 포트에 disable_port 가 서는지는 디바이스 트리에 달렸다.
 *
 * 레지스터 값들의 의미는 데이터시트에 있고 이 트리에서는 확인 못 함.
 *
 * 실행 컨텍스트: 전원 켜기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 쓰기 실패를 그대로 올려보내며, 호출자가 전원을 끈다.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_power_on() → [이 함수] → tc9563_pwrctrl_i2c_bulk_write()
 */
static int tc9563_pwrctrl_disable_port(struct tc9563_pwrctrl *tc9563,
				       enum tc9563_pwrctrl_ports port)
{
	struct tc9563_pwrctrl_cfg *cfg = &tc9563->cfg[port];
	const struct tc9563_pwrctrl_reg_setting *seq;
	/* [한국어] 결과와 표 길이. */
	int ret, len;

	if (!cfg->disable_port)
		/* [한국어] 끌 포트가 아니면 할 일이 없다. */
		return 0;

	if (port == TC9563_DSP1) {
		/* [한국어] DSP1 전용 시퀀스. */
		seq = dsp1_pwroff_seq;
		/* [한국어] 그 길이. */
		len = ARRAY_SIZE(dsp1_pwroff_seq);
	/* [한국어] 그 밖의 포트면 — */
	} else {
		seq = dsp2_pwroff_seq;
		/* [한국어] DSP2 쪽 표를 쓴다. DSP3 과 이더넷도 이 갈래로 오는데,
		 * 그 두 포트에 disable_port 가 서는지는 디바이스 트리에 달렸다. */
		len = ARRAY_SIZE(dsp2_pwroff_seq);
	}

	ret = tc9563_pwrctrl_i2c_bulk_write(tc9563->client, seq, len);
	/* [한국어] 포트별 시퀀스가 실패하면, */
	if (ret)
		/* [한국어] 공통 시퀀스는 쓰지 않고 물러난다. */
		return ret;

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, common_pwroff_seq,
					     /* [한국어] 포트별 다음에 공통을 쓴다. 이 순서도 데이터시트가 정한 것이다. */
					     ARRAY_SIZE(common_pwroff_seq));
}

/* [한국어]
 * tc9563_pwrctrl_set_l0s_l1_entry_delay - ASPM 절전 진입 지연을 설정한다
 *
 * @tc9563: 드라이버 상태.
 * @port: 대상 포트.
 * @is_l1: L1 인지 L0s 인지.
 * @ns: 원하는 지연(나노초).
 * @return: 0 = 성공(또는 설정할 필요 없음), 음수 오류.
 *
 * ASPM 은 링크가 놀 때 자동으로 절전 상태로 내려가는 기능이고, 이 지연이
 * "얼마나 논 뒤에 내려갈지" 를 정한다. 너무 짧으면 곧바로 다시 깨느라
 * 오히려 손해고, 너무 길면 절전 효과가 없다.
 *
 * 단위 변환이 필요하다. 레지스터는 256ns 단위로 세므로 나노초를 그것으로
 * 나눈다. 256ns 미만이면 0 단위가 되어 뜻이 없으므로 설정하지 않고 물러난다.
 *
 * 이더넷 포트만 다른 경로를 탄다. 내장 이더넷은 L0s 와 L1 지연이 **한
 * 레지스터의 서로 다른 필드** 에 들어 있어, 읽기-수정-쓰기로 한쪽만 갈아야
 * 한다. 다른 포트들은 지연마다 레지스터가 따로 있어 그냥 쓰면 된다.
 *
 * 일반 포트 경로에서 포트 선택을 먼저 쓰는 것이 요점이다. 지연 레지스터가
 * 포트마다 있는 것이 아니라 하나를 공유하고, 어느 포트에 적용할지는 선택
 * 레지스터가 정한다.
 *
 * 실행 컨텍스트: 전원 켜기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 읽기·쓰기 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_power_on() → [이 함수]
 *     → tc9563_pwrctrl_i2c_read() → tc9563_pwrctrl_i2c_write()
 */
static int tc9563_pwrctrl_set_l0s_l1_entry_delay(struct tc9563_pwrctrl *tc9563,
						 enum tc9563_pwrctrl_ports port,
						 bool is_l1, u32 ns)
{
	u32 rd_val, units;
	int ret;

	if (ns < TC9563_L0S_L1_DELAY_UNIT_NS)
		/* [한국어] 256ns 미만이면 단위가 0 이 되어 뜻이 없으므로 설정하지 않는다. */
		return 0;

	/* convert to units of 256ns */
	units = ns / TC9563_L0S_L1_DELAY_UNIT_NS;

	if (port == TC9563_ETHERNET) {
		/* [한국어] 이더넷은 두 지연이 한 레지스터에 있어 먼저 읽어야 한다. */
		ret = tc9563_pwrctrl_i2c_read(tc9563->client,
					      /* [한국어] 그 레지스터 주소. */
					      TC9563_EMBEDDED_ETH_DELAY,
					      &rd_val);
		if (ret)
			/* [한국어] 읽지 못하면 갱신할 수 없다. */
			return ret;

		if (is_l1)
			/* [한국어] L1 이면 그 필드만 갈아 끼운다. */
			rd_val = u32_replace_bits(rd_val, units,
						  /* [한국어] L1 지연이 차지하는 비트 범위. */
						  TC9563_ETH_L1_DELAY_MASK);
		else
			rd_val = u32_replace_bits(rd_val, units,
						  /* [한국어] L0s 면 그쪽 필드를 갈아 끼운다. 나머지 비트는 읽은 값 그대로 남는다. */
						  TC9563_ETH_L0S_DELAY_MASK);

		return tc9563_pwrctrl_i2c_write(tc9563->client,
						/* [한국어] 갱신한 값을 되쓴다. */
						TC9563_EMBEDDED_ETH_DELAY,
						rd_val);
	}

	ret = tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_PORT_SELECT,
				       /* [한국어] 일반 포트는 지연 레지스터를 공유하므로, 어느 포트에 적용할지를 먼저 정한다.
				        * 포트 번호가 곧 비트 위치다. */
				       BIT(port));
	if (ret)
		/* [한국어] 포트 선택이 실패하면 지연을 쓸 수 없다. */
		return ret;

	return tc9563_pwrctrl_i2c_write(tc9563->client,
					/* [한국어] L1 과 L0s 는 레지스터가 따로 있다 — 이더넷과 달리 읽기-수정-쓰기가 필요 없다. */
					is_l1 ? TC9563_PORT_L1_DELAY : TC9563_PORT_L0S_DELAY,
					units);
}

/* [한국어]
 * tc9563_pwrctrl_set_tx_amplitude - 송신 신호 진폭을 설정한다
 *
 * @tc9563: 드라이버 상태.
 * @port: 대상 포트.
 * @return: 0 = 성공(또는 설정할 필요 없음), -EINVAL 또는 음수 오류.
 *
 * 기판 배선의 길이와 품질에 따라 필요한 송신 세기가 다르다. 보드 설계자가
 * 디바이스 트리에 적어 둔 값을 여기서 하드웨어에 넣는다.
 *
 * 단위 변환이 두 단계다. 최솟값을 빼고 3125 로 나누는데, 이는 레지스터가
 * 400000µA 를 0 으로 삼아 3125µA 단위로 세기 때문이다. 최솟값 미만이면
 * 설정하지 않고 물러난다.
 *
 * 포트 접근 값이 비트마스크다 — USP 가 0x1, DSP1 이 0x2, DSP2 가 0x8.
 * DSP1 과 DSP2 사이에 0x4 가 비어 있는데, 그 자리가 무엇인지는 이 트리에서
 * 확인 못 함.
 *
 * DSP3 과 이더넷은 -EINVAL 이다. 그 두 포트에는 이 설정이 없다는 뜻이며,
 * 호출자가 모든 포트에 대해 이 함수를 부르므로 그 둘에서 오류가 나올 수
 * 있다 — 다만 디바이스 트리에 tx_amp 가 없으면 최솟값 검사에서 먼저 걸러진다.
 *
 * 설정 표를 함수 중간에 선언하는 것이 눈에 띈다. port_access 가 위 switch 로
 * 정해진 뒤라야 표를 만들 수 있기 때문이다.
 *
 * 실행 컨텍스트: 전원 켜기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 지원하지 않는 포트는 -EINVAL, 쓰기 실패는 그 오류.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_power_on() → [이 함수] → tc9563_pwrctrl_i2c_bulk_write()
 */
static int tc9563_pwrctrl_set_tx_amplitude(struct tc9563_pwrctrl *tc9563,
					   enum tc9563_pwrctrl_ports port)
{
	u32 amp = tc9563->cfg[port].tx_amp;
	int port_access;

	if (amp < TC9563_TX_MARGIN_MIN_UA)
		/* [한국어] 최솟값 미만이면 설정하지 않는다. 그 값이 레지스터의 0 에 해당한다. */
		return 0;

	/* txmargin = (Amp(uV) - 400000) / 3125 */
	amp = (amp - TC9563_TX_MARGIN_MIN_UA) / 3125;

	switch (port) {
	/* [한국어] 업스트림 포트면, */
	case TC9563_USP:
		/* [한국어] 접근 비트 0x1. */
		port_access = 0x1;
		break;
	case TC9563_DSP1:
		/* [한국어] DSP1 은 0x2. */
		port_access = 0x2;
		break;
	case TC9563_DSP2:
		/* [한국어] DSP2 는 0x8. */
		port_access = 0x8;
		break;
	default:
		return -EINVAL;
	}

	struct tc9563_pwrctrl_reg_setting tx_amp_seq[] = {
		/* [한국어] 어느 포트에 쓸지 먼저 연다. */
		{TC9563_PORT_ACCESS_ENABLE, port_access},
		/* [한국어] 레인 둘 다 대상으로 한다. */
		{TC9563_PORT_LANE_ACCESS_ENABLE, 0x3},
		{TC9563_TX_MARGIN, amp},
	};

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, tx_amp_seq,
					     /* [한국어] 세 줄을 순서대로 쓴다 — 접근을 연 뒤라야 진폭 레지스터가 그 포트에 닿는다. */
					     ARRAY_SIZE(tx_amp_seq));
}

/* [한국어]
 * tc9563_pwrctrl_disable_dfe - 수신 등화기(DFE)를 끈다
 *
 * @tc9563: 드라이버 상태.
 * @port: 대상 포트.
 * @return: 0 = 성공(또는 끌 필요 없음), -EINVAL 또는 음수 오류.
 *
 * DFE(Decision Feedback Equalizer)는 배선을 지나며 뭉개진 신호를 되살리는
 * 회로다. 긴 배선에서는 필요하지만 짧고 깨끗한 배선에서는 오히려 링크를
 * 불안정하게 만들 수 있어, 보드에 따라 끄는 선택지를 둔 것이다.
 *
 * 표가 열한 줄인데 그 끝 넷이 한 덩어리다 — 레이트 변경 오버라이드를 켜고,
 * 레이트를 썼다가 0 으로 되돌리고, 오버라이드를 끈다. 그 왕복이 PHY 에
 * "설정이 바뀌었으니 다시 훈련하라" 를 알리는 방법으로 보이며, 데이터시트
 * 없이는 그 이상 확인 못 함.
 *
 * 포트마다 세 값이 갈린다 — 접근 마스크, 레인 마스크, 레이트 값. USP 만
 * 레이트가 다르고 DSP2 만 레인이 하나인데, 그 차이의 근거는 이 트리에서
 * 확인 못 함.
 *
 * 여기서도 DSP3 과 이더넷은 -EINVAL 이다.
 *
 * 실행 컨텍스트: 전원 켜기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 지원하지 않는 포트는 -EINVAL, 쓰기 실패는 그 오류.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_power_on() → [이 함수] → tc9563_pwrctrl_i2c_bulk_write()
 */
static int tc9563_pwrctrl_disable_dfe(struct tc9563_pwrctrl *tc9563,
				      enum tc9563_pwrctrl_ports port)
{
	struct tc9563_pwrctrl_cfg *cfg = &tc9563->cfg[port];
	int port_access, lane_access = 0x3;
	/* [한국어] PHY 레이트 변경 값. 아래 switch 에서 USP 만 다른 값으로 덮인다. */
	u32 phy_rate = 0x21;

	if (!cfg->disable_dfe)
		/* [한국어] 끌 필요가 없으면 할 일이 없다. */
		return 0;

	switch (port) {
	/* [한국어] 업스트림 포트는, */
	case TC9563_USP:
		/* [한국어] 레이트 값이 다르다. 그 차이의 근거는 이 트리에서 확인 못 함. */
		phy_rate = 0x1;
		port_access = 0x1;
		/* [한국어] 레인은 기본값 0x3 을 그대로 쓴다. */
		break;
	case TC9563_DSP1:
		/* [한국어] DSP1 은 접근 비트만 다르고 나머지는 기본값이다. */
		port_access = 0x2;
		break;
	case TC9563_DSP2:
		/* [한국어] DSP2 는 접근 비트가 0x8 이고, */
		port_access = 0x8;
		lane_access = 0x1;
		/* [한국어] 레인도 하나뿐이다. */
		break;
	default:
		return -EINVAL;
	}

	struct tc9563_pwrctrl_reg_setting disable_dfe_seq[] = {
		/* [한국어] 어느 포트에 쓸지 연다. */
		{TC9563_PORT_ACCESS_ENABLE, port_access},
		/* [한국어] 그 포트의 레인 범위를 연다. */
		{TC9563_PORT_LANE_ACCESS_ENABLE, lane_access},
		{TC9563_DFE_ENABLE, 0x0},
		{TC9563_DFE_EQ0_MODE, 0x411},
		{TC9563_DFE_EQ1_MODE, 0x11},
		{TC9563_DFE_EQ2_MODE, 0x11},
		{TC9563_DFE_PD_MASK, 0x7},
		{TC9563_PHY_RATE_CHANGE_OVERRIDE, 0x10},
		{TC9563_PHY_RATE_CHANGE, phy_rate},
		{TC9563_PHY_RATE_CHANGE, 0x0},
		{TC9563_PHY_RATE_CHANGE_OVERRIDE, 0x0},
	};

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, disable_dfe_seq,
					     /* [한국어] 열한 줄을 순서대로 쓴다. 끝 넷이 한 덩어리로,
					      * PHY 에 '설정이 바뀌었으니 다시 훈련하라' 를 알리는 왕복으로 보인다. */
					     ARRAY_SIZE(disable_dfe_seq));
}

/* [한국어]
 * tc9563_pwrctrl_set_nfts - 절전 복귀에 보낼 훈련 시퀀스 개수를 설정한다
 *
 * @tc9563: 드라이버 상태.
 * @port: 대상 포트.
 * @return: 0 = 성공(또는 설정할 필요 없음), 음수 오류.
 *
 * N_FTS(Number of Fast Training Sequences)는 L0s 에서 깨어날 때 링크를
 * 다시 맞추기 위해 보내는 훈련 패턴의 개수다. 적으면 빨리 깨어나지만 링크가
 * 안정되지 않을 수 있고, 많으면 안전하지만 느리다.
 *
 * GEN1(2.5GT/s)과 GEN2(5GT/s)에 각각 값이 있다. 속도가 다르면 필요한 훈련
 * 길이도 달라지기 때문이다.
 *
 * 첫 값이 0 이면 설정하지 않는다. 디바이스 트리에 n-fts 가 없었다는 뜻이다.
 * GEN2 값만 있고 GEN1 이 0 인 경우도 이 검사에 걸려 둘 다 적용되지 않는다.
 *
 * 여기서도 포트 선택을 먼저 쓴다 — 지연 설정과 같은 구조다.
 *
 * 설정 표를 함수 앞머리에서 초기화하는 것이 위 두 함수와 다르다. 값이
 * cfg 에서 바로 오므로 switch 를 기다릴 필요가 없다.
 *
 * 실행 컨텍스트: 전원 켜기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 쓰기 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_power_on() → [이 함수]
 *     → tc9563_pwrctrl_i2c_write() → tc9563_pwrctrl_i2c_bulk_write()
 */
static int tc9563_pwrctrl_set_nfts(struct tc9563_pwrctrl *tc9563,
				   enum tc9563_pwrctrl_ports port)
{
	u8 *nfts = tc9563->cfg[port].nfts;
	struct tc9563_pwrctrl_reg_setting nfts_seq[] = {
		/* [한국어] GEN1(2.5GT/s)용 값. */
		{TC9563_NFTS_2_5_GT, nfts[0]},
		/* [한국어] GEN2(5GT/s)용 값. 속도가 다르면 필요한 훈련 길이도 달라진다. */
		{TC9563_NFTS_5_GT, nfts[1]},
	};
	int ret;

	if (!nfts[0])
		/* [한국어] GEN1 값이 0 이면 디바이스 트리에 n-fts 가 없었다는 뜻이다.
		 * GEN2 만 지정한 경우도 여기서 함께 걸러진다. */
		return 0;

	ret =  tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_PORT_SELECT,
					/* [한국어] 지연 설정과 같은 구조 — N_FTS 레지스터도 포트마다 있는 것이 아니라 공유한다. */
					BIT(port));
	if (ret)
		/* [한국어] 포트 선택이 실패하면 값을 쓸 수 없다. */
		return ret;

	return tc9563_pwrctrl_i2c_bulk_write(tc9563->client, nfts_seq,
					     /* [한국어] 두 값을 순서대로 쓴다. */
					     ARRAY_SIZE(nfts_seq));
}

/* [한국어]
 * tc9563_pwrctrl_assert_deassert_reset - 스위치가 제어하는 하위 리셋 신호를 걸거나 푼다
 *
 * @tc9563: 드라이버 상태.
 * @deassert: 참이면 리셋을 푼다.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 리셋은 드라이버가 직접 잡은 GPIO(reset_gpio)가 아니라, **스위치 칩이
 * 내보내는** GPIO 다. 그래서 I2C 로 그 칩의 GPIO 레지스터를 써서 조작한다.
 *
 * 두 레지스터를 순서대로 쓴다. 먼저 GPIO 설정 레지스터에 마스크를 써서
 * 그 핀들을 출력으로 만들고, 그 다음 값 레지스터에 실제 값을 쓴다.
 *
 * 전원 켜기 경로에서 이 함수가 **두 번** 불린다 — 처음에 걸고, 모든 설정이
 * 끝난 뒤에 푼다. 그 사이에 하위 장치가 깨어나면 아직 설정되지 않은 링크로
 * 동작하게 되므로, 설정이 끝날 때까지 붙잡아 두는 것이다.
 *
 * 옆의 상류 주석이 밝히듯 값 비트를 **지우는** 것이 assert 이고 세우는 것이
 * deassert 다.
 *
 * 실행 컨텍스트: 전원 켜기 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 쓰기 실패를 그대로 올려보낸다.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_power_on() → [이 함수] → tc9563_pwrctrl_i2c_write()
 */
static int tc9563_pwrctrl_assert_deassert_reset(struct tc9563_pwrctrl *tc9563,
						bool deassert)
{
	int ret, val;

	ret = tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_GPIO_CONFIG,
				       /* [한국어] 먼저 GPIO 설정 레지스터에 마스크를 써서 그 핀들을 출력으로 만든다. */
				       TC9563_GPIO_MASK);
	if (ret)
		/* [한국어] 설정에 실패하면 값을 써도 소용없다. */
		return ret;

	val = deassert ? TC9563_GPIO_DEASSERT_BITS : 0;

	return tc9563_pwrctrl_i2c_write(tc9563->client, TC9563_RESET_GPIO, val);
}

/* [한국어]
 * tc9563_pwrctrl_parse_device_dt - 한 포트의 디바이스 트리 설정을 읽어 둔다
 *
 * @tc9563: 드라이버 상태.
 * @node: 그 포트의 노드.
 * @port: 포트 번호.
 * @return: 0 = 성공, 음수 오류.
 *
 * 전원을 켜기 전에 모든 포트의 설정을 미리 읽어 둔다. 전원 켜기 경로에서
 * 읽으면 그 안에서 디바이스 트리 접근이 섞여 흐름이 흐려지기 때문이다.
 *
 * 노드가 사용 불가로 표시돼 있으면 disable_port 만 세우고 나머지는 읽지
 * 않는다. 끌 포트의 설정을 읽어 봐야 쓸 일이 없다.
 *
 * -EINVAL 을 오류로 다루지 않는 것이 이 함수의 관용이다. of_property_read
 * 계열이 "그 속성이 없다" 를 그 값으로 알리는데, 여기서는 모든 속성이
 * 선택 사항이라 없는 것이 정상이다. 그래서 -EINVAL 만 걸러 내고 나머지
 * 오류 — 속성이 있는데 형식이 잘못된 경우 — 만 올려보낸다.
 *
 * 읽지 않은 필드는 0 으로 남고, 각 설정 함수가 그 0 을 "설정하지 않음" 으로
 * 해석한다. tx_amp 의 최솟값 검사와 nfts[0] 검사가 그 자리다.
 *
 * 실행 컨텍스트: probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 속성 형식 오류를 올려보내며, 속성 부재는 오류가 아니다.
 *
 * 호출 체인:
 *   tc9563_pwrctrl_probe() → [이 함수]
 *     → of_device_is_available() → of_property_read_u32()
 *     → of_property_read_u8_array() → of_property_read_bool()
 */
static int tc9563_pwrctrl_parse_device_dt(struct tc9563_pwrctrl *tc9563,
					  struct device_node *node,
					  enum tc9563_pwrctrl_ports port)
{
	/* [한국어] 이 포트의 설정을 담을 자리. */
	struct tc9563_pwrctrl_cfg *cfg = &tc9563->cfg[port];
	/* [한국어] 속성 읽기의 결과. */
	int ret;

	/* Disable port if the status of the port is disabled. */
	if (!of_device_is_available(node)) {
		cfg->disable_port = true;
		/* [한국어] 노드가 사용 불가로 표시돼 있으면 끌 포트로 기록하고 나머지는 읽지 않는다 —
		 * 끌 포트의 설정을 읽어 봐야 쓸 일이 없다. */
		return 0;
	}

	ret = of_property_read_u32(node, "aspm-l0s-entry-delay-ns", &cfg->l0s_delay);
	/* [한국어] -EINVAL 은 '그 속성이 없다' 는 뜻이라 오류가 아니다. 이 파일의 속성이
	 * 모두 선택 사항이므로 그 값만 걸러 내고 나머지 — 속성이 있는데 형식이
	 * 잘못된 경우 — 만 올려보낸다. */
	if (ret && ret != -EINVAL)
		/* [한국어] 형식 오류면 물러난다. */
		return ret;

	ret = of_property_read_u32(node, "aspm-l1-entry-delay-ns", &cfg->l1_delay);
	/* [한국어] 같은 규약이 이하 모든 속성에 적용된다. */
	if (ret && ret != -EINVAL)
		/* [한국어] 형식 오류면 물러난다. */
		return ret;

	ret = of_property_read_u32(node, "toshiba,tx-amplitude-microvolt", &cfg->tx_amp);
	/* [한국어] 같은 규약. */
	if (ret && ret != -EINVAL)
		/* [한국어] 형식 오류면 물러난다. */
		return ret;

	ret = of_property_read_u8_array(node, "n-fts", cfg->nfts, ARRAY_SIZE(cfg->nfts));
	/* [한국어] 배열 속성도 같은 규약이다. */
	if (ret && ret != -EINVAL)
		/* [한국어] 형식 오류면 물러난다. */
		return ret;

	cfg->disable_dfe = of_property_read_bool(node, "toshiba,no-dfe-support");

	return 0;
}

/* [한국어]
 * tc9563_pwrctrl_power_off - 스위치의 전원을 끊는다
 *
 * @pwrctrl: 코어가 준 pwrctrl 문맥.
 * @return: 언제나 0.
 *
 * 순서가 이 함수의 전부다 — 리셋을 먼저 걸고 그 다음 전원을 끊는다.
 *
 * 그 순서여야 하는 이유는 전원이 내려가는 동안 칩이 불안정한 신호를 내보내지
 * 않게 하기 위해서다. 리셋을 걸어 두면 그 사이에 하위 장치가 잘못된 신호를
 * 보지 않는다.
 *
 * 여기서 쓰는 것은 드라이버가 잡은 GPIO 이지 스위치가 내보내는 GPIO 가 아니다.
 * I2C 로 조작하는 그쪽은 전원이 이미 없으면 접근할 수 없다.
 *
 * 언제나 0 을 돌려준다. 끄기가 실패해도 호출자가 할 수 있는 일이 없다.
 *
 * 전원 켜기의 되감기 경로에서도 이 함수가 쓰인다 — 켜다가 실패하면 여기로
 * 와 지금까지 켠 것을 되돌린다.
 *
 * 실행 컨텍스트: pwrctrl 코어의 전원 끄기, 또는 켜기의 되감기.
 * 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pwrctrl 코어 / tc9563_pwrctrl_power_on() 의 되감기 → [이 함수]
 *     → gpiod_set_value() → regulator_bulk_disable()
 */
static int tc9563_pwrctrl_power_off(struct pci_pwrctrl *pwrctrl)
{
	struct tc9563_pwrctrl *tc9563 = container_of(pwrctrl,
						    struct tc9563_pwrctrl, pwrctrl);

	/* [한국어] 리셋을 **먼저** 건다. 전원이 내려가는 동안 칩이 불안정한 신호를 내보내
	 * 하위 장치가 그것을 보지 않게 하려는 것이다. */
	gpiod_set_value(tc9563->reset_gpio, 1);

	/* [한국어] 그 다음 여섯 전압을 통째로 끊는다. */
	regulator_bulk_disable(ARRAY_SIZE(tc9563->supplies), tc9563->supplies);

	return 0;
}

/* [한국어]
 * tc9563_pwrctrl_power_on - 전원을 넣고 모든 포트를 설정한다
 *
 * @pwrctrl: 코어가 준 pwrctrl 문맥.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 본체다. PCI 코어가 이 스위치를 스캔하기 **전에** 불려,
 * 링크가 서기 전에 끝나야 하는 설정을 모두 마친다.
 *
 * 전체 순서가 이렇다.
 * 1. 전압을 넣는다.
 * 2. 드라이버 GPIO 로 칩 리셋을 푼다.
 * 3. 발진기가 안정될 때까지 10ms 기다린다. 이 대기를 건너뛰면 이어지는
 *    I2C 접근이 실패한다.
 * 4. 스위치가 내보내는 하위 리셋을 **건다**. 설정이 끝날 때까지 하위 장치가
 *    깨어나지 못하게 붙잡는 것이다.
 * 5. 포트마다 다섯 가지를 설정한다 — 포트 끄기, L0s 지연, L1 지연,
 *    송신 진폭, N_FTS, DFE 끄기.
 * 6. 하위 리셋을 **푼다**. 이제 하위 장치들이 설정이 끝난 링크로 깨어난다.
 *
 * 4번과 6번이 이 함수의 뼈대이며, 그 사이가 설정 구간이다.
 *
 * 루프가 모든 포트를 도는데, 설정 함수들이 DSP3 과 이더넷에 대해 -EINVAL 을
 * 돌려줄 수 있다. 실제로는 그 포트들에 해당 설정이 디바이스 트리에 없어
 * 각 함수의 앞선 검사에서 먼저 걸러진다 — 즉 이 구조가 성립하는 것은
 * 디바이스 트리가 그렇게 쓰여 있다는 전제 위에서다.
 *
 * 실패하면 모두 전원을 끄는 한 경로로 모인다. 절반만 설정된 스위치를
 * 되돌릴 방법이 없어, 처음부터 다시 하는 것이 유일한 대응이다.
 *
 * 실행 컨텍스트: pwrctrl 코어의 전원 켜기. 대기와 I2C 가 있어 프로세스
 * 컨텍스트여야 하며 오래 걸린다.
 *
 * 에러 경로: 어느 단계에서 실패하든 goto 로 전원을 끄고 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   pwrctrl 코어 → [이 함수]
 *     → regulator_bulk_enable() → tc9563_pwrctrl_assert_deassert_reset()
 *     → tc9563_pwrctrl_disable_port() → ..._set_l0s_l1_entry_delay()
 *     → ..._set_tx_amplitude() → ..._set_nfts() → ..._disable_dfe()
 */
static int tc9563_pwrctrl_power_on(struct pci_pwrctrl *pwrctrl)
{
	struct tc9563_pwrctrl *tc9563 = container_of(pwrctrl,
						    struct tc9563_pwrctrl, pwrctrl);
	struct device *dev = tc9563->pwrctrl.dev;
	/* [한국어] 루프 안에서 그때그때 가리킬 포트 설정. */
	struct tc9563_pwrctrl_cfg *cfg;
	/* [한국어] 결과와 포트 색인. */
	int ret, i;

	ret = regulator_bulk_enable(ARRAY_SIZE(tc9563->supplies),
				    /* [한국어] 여섯 전압을 통째로 켠다. */
				    tc9563->supplies);
	if (ret < 0)
		/* [한국어] 전압이 없으면 아무것도 할 수 없다. */
		return dev_err_probe(dev, ret, "cannot enable regulators\n");

	gpiod_set_value(tc9563->reset_gpio, 0);

	fsleep(TC9563_OSC_STAB_DELAY_US);

	ret = tc9563_pwrctrl_assert_deassert_reset(tc9563, false);
	/* [한국어] 하위 리셋을 걸지 못했으면, */
	if (ret)
		/* [한국어] 전원을 끄는 경로로 간다. 설정 구간에 들어가면 안 되는데,
		 * 하위 장치가 아직 설정되지 않은 링크로 깨어날 수 있기 때문이다. */
		goto power_off;

	for (i = 0; i < TC9563_MAX; i++) {
		/* [한국어] 이 포트의 설정을 가리킨다. */
		cfg = &tc9563->cfg[i];
		/* [한국어] 쓰지 않는 포트면 끈다. */
		ret = tc9563_pwrctrl_disable_port(tc9563, i);
		/* [한국어] 실패하면, */
		if (ret) {
			/* [한국어] 무엇이 실패했는지 남기고, */
			dev_err(dev, "Disabling port failed\n");
			/* [한국어] 전원을 끈다. */
			goto power_off;
		}

		ret = tc9563_pwrctrl_set_l0s_l1_entry_delay(tc9563, i, false, cfg->l0s_delay);
		/* [한국어] L0s 지연 설정이 실패하면, */
		if (ret) {
			/* [한국어] 그 사실을 남기고, */
			dev_err(dev, "Setting L0s entry delay failed\n");
			/* [한국어] 전원을 끈다. */
			goto power_off;
		}

		ret = tc9563_pwrctrl_set_l0s_l1_entry_delay(tc9563, i, true, cfg->l1_delay);
		/* [한국어] L1 지연 설정이 실패하면, */
		if (ret) {
			/* [한국어] 그 사실을 남기고, */
			dev_err(dev, "Setting L1 entry delay failed\n");
			/* [한국어] 전원을 끈다. */
			goto power_off;
		}

		ret = tc9563_pwrctrl_set_tx_amplitude(tc9563, i);
		/* [한국어] 송신 진폭 설정이 실패하면, */
		if (ret) {
			/* [한국어] 그 사실을 남기고, */
			dev_err(dev, "Setting Tx amplitude failed\n");
			/* [한국어] 전원을 끈다. */
			goto power_off;
		}

		ret = tc9563_pwrctrl_set_nfts(tc9563, i);
		/* [한국어] N_FTS 설정이 실패하면, */
		if (ret) {
			/* [한국어] 그 사실을 남기고, */
			dev_err(dev, "Setting N_FTS failed\n");
			/* [한국어] 전원을 끈다. */
			goto power_off;
		}

		ret = tc9563_pwrctrl_disable_dfe(tc9563, i);
		/* [한국어] DFE 끄기가 실패하면, */
		if (ret) {
			/* [한국어] 그 사실을 남기고, */
			dev_err(dev, "Disabling DFE failed\n");
			/* [한국어] 전원을 끈다. */
			goto power_off;
		}
	}

	ret = tc9563_pwrctrl_assert_deassert_reset(tc9563, true);
	/* [한국어] 하위 리셋을 푸는 데 성공했으면, */
	if (!ret)
		/* [한국어] 여기서 끝이다. 이제 하위 장치들이 설정이 끝난 링크로 깨어난다. */
		return 0;

power_off:
	tc9563_pwrctrl_power_off(&tc9563->pwrctrl);
	return ret;
}

/* [한국어]
 * tc9563_pwrctrl_probe - I2C 통로와 전원 자원을 확보하고 pwrctrl 로 등록한다
 *
 * @pdev: 플랫폼 장치.
 * @return: 0 = 성공, 음수 오류.
 *
 * 이 드라이버의 진입점이다.
 *
 * I2C 통로를 얻는 과정이 앞부분의 대부분이다. 디바이스 트리의 i2c-parent
 * 속성이 어댑터 phandle 과 주소를 함께 담고 있어, 그 둘을 각각 꺼낸다.
 * 어댑터를 찾지 못하면 -EPROBE_DEFER 로 물러나는데, I2C 컨트롤러가 아직
 * probe 되지 않았을 수 있기 때문이다.
 *
 * i2c_new_dummy_device() 를 쓰는 것이 요점이다. 이 스위치는 자기 I2C
 * 드라이버를 갖지 않고 이 파일이 직접 다루므로, 드라이버 없이 주소만
 * 차지하는 클라이언트를 만든다.
 *
 * 그 다음은 전압 여섯 개와 리셋 GPIO 다. 전압 이름들이 파일 앞머리의
 * 배열에 있고, 그 배열 순서가 곧 supplies 배열의 순서가 된다.
 *
 * 디바이스 트리 순회가 세 겹이다 — 최상위 노드가 USP, 그 자식들이 DSP1~3,
 * 그리고 DSP3 의 자식이 이더넷이다. port 를 하나씩 올리며 도는 방식이라
 * **노드의 배치가 enum 의 순서와 정확히 맞아야** 한다.
 *
 * 정리 경로가 devres 와 수동이 섞여 있다. 전압과 GPIO 는 devres 가 맡지만
 * I2C 클라이언트와 어댑터 참조는 직접 놓아야 해서, goto 라벨이 그 둘만 다룬다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 각 단계의 실패를 dev_err_probe 로 기록하고, I2C 자원을 놓는
 * 라벨로 뛴다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → of_find_i2c_adapter_by_node() → i2c_new_dummy_device()
 *     → devm_regulator_bulk_get() → devm_gpiod_get()
 *     → pci_pwrctrl_init() → tc9563_pwrctrl_parse_device_dt()
 */
static int tc9563_pwrctrl_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	/* [한국어] 포트 색인. 아래에서 트리를 훑으며 하나씩 올린다. */
	enum tc9563_pwrctrl_ports port;
	/* [한국어] 이 드라이버의 상태. */
	struct tc9563_pwrctrl *tc9563;
	/* [한국어] I2C 어댑터를 가리키는 노드. */
	struct device_node *i2c_node;
	/* [한국어] 결과와 I2C 주소. */
	int ret, addr;

	tc9563 = devm_kzalloc(dev, sizeof(*tc9563), GFP_KERNEL);
	/* [한국어] 상태 구조를 잡지 못하면, */
	if (!tc9563)
		/* [한국어] 메모리 부족으로 물러난다. */
		return -ENOMEM;

	ret = of_property_read_u32_index(node, "i2c-parent", 1, &addr);
	/* [한국어] i2c-parent 의 두 번째 값이 I2C 주소다. 첫 번째는 어댑터 phandle 이다. */
	if (ret)
		/* [한국어] 속성이 없으면 이 스위치에 닿을 방법이 없다. */
		return dev_err_probe(dev, ret, "Failed to read i2c-parent property\n");

	i2c_node = of_parse_phandle(dev->of_node, "i2c-parent", 0);
	/* [한국어] 그 노드에 해당하는 어댑터를 찾는다 — 참조를 올려 준다. */
	tc9563->adapter = of_find_i2c_adapter_by_node(i2c_node);
	/* [한국어] 노드 참조는 더 필요 없으므로 놓는다. */
	of_node_put(i2c_node);
	if (!tc9563->adapter)
		/* [한국어] 어댑터를 못 찾으면 -EPROBE_DEFER 로 물러난다. I2C 컨트롤러가 아직
		 * probe 되지 않았을 수 있어, 나중에 다시 시도하면 성공할 수 있기 때문이다. */
		return dev_err_probe(dev, -EPROBE_DEFER, "Failed to find I2C adapter\n");

	tc9563->client = i2c_new_dummy_device(tc9563->adapter, addr);
	/* [한국어] 클라이언트 생성이 실패하면, */
	if (IS_ERR(tc9563->client)) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "Failed to create I2C client\n");
		/* [한국어] 방금 올린 어댑터 참조를 놓는다 — 여기는 아직 goto 라벨을 쓸 수 없다.
		 * 라벨이 클라이언트를 없애려 들기 때문이다. */
		put_device(&tc9563->adapter->dev);
		return PTR_ERR(tc9563->client);
	/* [한국어] 오류 포인터에서 코드를 꺼내 올려보낸다. */
	}

	for (int i = 0; i < ARRAY_SIZE(tc9563_supply_names); i++)
		/* [한국어] 이름 배열의 순서가 그대로 supplies 배열의 순서가 된다. */
		tc9563->supplies[i].supply = tc9563_supply_names[i];

	ret = devm_regulator_bulk_get(dev, TC9563_PWRCTL_MAX_SUPPLY,
				      /* [한국어] 여섯 전압을 한 번에 얻는다. */
				      tc9563->supplies);
	if (ret) {
		/* [한국어] 얻지 못하면 그 사실을 남기고, */
		dev_err_probe(dev, ret, "failed to get supply regulator\n");
		/* [한국어] I2C 자원을 놓는 경로로 간다. */
		goto remove_i2c;
	}

	tc9563->reset_gpio = devm_gpiod_get(dev, "resx", GPIOD_OUT_HIGH);
	/* [한국어] 리셋 GPIO 를 얻지 못하면, */
	if (IS_ERR(tc9563->reset_gpio)) {
		/* [한국어] 그 사실을 남기고, */
		ret = dev_err_probe(dev, PTR_ERR(tc9563->reset_gpio), "failed to get resx GPIO\n");
		/* [한국어] I2C 자원을 놓는다. */
		goto remove_i2c;
	}

	pci_pwrctrl_init(&tc9563->pwrctrl, dev);

	port = TC9563_USP;
	/* [한국어] 최상위 노드가 업스트림 포트의 설정을 담는다. */
	ret = tc9563_pwrctrl_parse_device_dt(tc9563, node, port);
	/* [한국어] 읽지 못하면, */
	if (ret) {
		/* [한국어] 그 사실을 남기고, */
		dev_err(dev, "failed to parse device tree properties: %d\n", ret);
		/* [한국어] I2C 자원을 놓는다. */
		goto remove_i2c;
	}

	/*
	 * Downstream ports are always children of the upstream port.
	 * The first node represents DSP1, the second node represents DSP2,
	 * and so on.
	 */
	for_each_child_of_node_scoped(node, child) {
		port++;
		/* [한국어] 그 자식 노드가 DSP1, DSP2, DSP3 순이다. */
		ret = tc9563_pwrctrl_parse_device_dt(tc9563, child, port);
		/* [한국어] 하나라도 실패하면, */
		if (ret)
			/* [한국어] 순회를 멈춘다. _scoped 판이라 노드 참조는 자동으로 놓인다. */
			break;
		/* Embedded ethernet device are under DSP3 */
		if (port == TC9563_DSP3) {
			for_each_child_of_node_scoped(child, child1) {
				/* [한국어] 이더넷 포트로 넘어간다. */
				port++;
				/* [한국어] 그 노드의 설정을 읽는다. */
				ret = tc9563_pwrctrl_parse_device_dt(tc9563,
								/* [한국어] DSP3 의 자식이 내장 이더넷이라는 배치를 그대로 따른다. */
								child1, port);
				if (ret)
					/* [한국어] 실패하면 안쪽 순회를 멈춘다. */
					break;
			}
		}
	}
	if (ret) {
		/* [한국어] 어느 단계든 실패했으면 그 사실을 남기고, */
		dev_err(dev, "failed to parse device tree properties: %d\n", ret);
		/* [한국어] I2C 자원을 놓는다. */
		goto remove_i2c;
	}

	tc9563->pwrctrl.power_on = tc9563_pwrctrl_power_on;
	/* [한국어] 끄기 콜백도 채운다. 이 둘이 pwrctrl 코어와 이 드라이버의 접점 전부다. */
	tc9563->pwrctrl.power_off = tc9563_pwrctrl_power_off;

	ret = devm_pci_pwrctrl_device_set_ready(dev, &tc9563->pwrctrl);
	/* [한국어] pwrctrl 로 등록하지 못하면, */
	if (ret)
		/* [한국어] 전원을 끄는 경로로 간다 — 이미 켜져 있을 수 있기 때문이다. */
		goto power_off;

	return 0;

power_off:
	tc9563_pwrctrl_power_off(&tc9563->pwrctrl);
remove_i2c:
	i2c_unregister_device(tc9563->client);
	put_device(&tc9563->adapter->dev);
	return ret;
}

/* [한국어]
 * tc9563_pwrctrl_remove - I2C 클라이언트와 어댑터 참조를 놓는다
 *
 * @pdev: 플랫폼 장치.
 *
 * probe 가 수동으로 잡은 둘만 되돌린다. 전압과 GPIO 는 devres 가 자동으로
 * 놓으므로 여기 없다.
 *
 * 순서가 정해져 있다 — 클라이언트를 먼저 없애고 어댑터 참조를 나중에 놓는다.
 * 클라이언트가 어댑터에 매달려 있어, 어댑터를 먼저 놓으면 그것이 해제된 뒤
 * 클라이언트를 없애려 들 수 있다.
 *
 * pwrctrl 등록 해제는 여기 없다. devm 판으로 등록되어 devres 가 맡는다.
 *
 * 실행 컨텍스트: 플랫폼 드라이버 remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   플랫폼 드라이버 코어 → [이 함수]
 *     → i2c_unregister_device() → put_device()
 */
static void tc9563_pwrctrl_remove(struct platform_device *pdev)
{
	struct pci_pwrctrl *pwrctrl = dev_get_drvdata(&pdev->dev);
	struct tc9563_pwrctrl *tc9563 = container_of(pwrctrl,
					/* [한국어] pwrctrl 이 구조체 맨 앞에 있어 이 변환이 성립한다. */
					struct tc9563_pwrctrl, pwrctrl);

	tc9563_pwrctrl_power_off(&tc9563->pwrctrl);
	i2c_unregister_device(tc9563->client);
	put_device(&tc9563->adapter->dev);
}

static const struct of_device_id tc9563_pwrctrl_of_match[] = {
	/* [한국어] 이 스위치의 PCI 벤더·장치 ID 로 만든 compatible 문자열이다. */
	{ .compatible = "pci1179,0623"},
	/* [한국어] 표의 끝 표시. */
	{ }
};
MODULE_DEVICE_TABLE(of, tc9563_pwrctrl_of_match);

static struct platform_driver tc9563_pwrctrl_driver = {
	/* [한국어] 플랫폼 드라이버로 등록된다 — PCI 드라이버가 아닌 것이 요점으로,
	 * PCI 코어가 이 스위치를 스캔하기 전에 전원을 넣어야 하기 때문이다. */
	.driver = {
		/* [한국어] sysfs 에 나올 이름. */
		.name = "pwrctrl-tc9563",
		/* [한국어] 디바이스 트리로 매칭한다. */
		.of_match_table = tc9563_pwrctrl_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = tc9563_pwrctrl_probe,
	.remove = tc9563_pwrctrl_remove,
};
module_platform_driver(tc9563_pwrctrl_driver);

MODULE_AUTHOR("Krishna chaitanya chundru <quic_krichai@quicinc.com>");
MODULE_DESCRIPTION("TC956x power control driver");
MODULE_LICENSE("GPL");
