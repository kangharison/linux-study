/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Purpose:	PCI Express Port Bus Driver's Internal Data Structures
 *
 * Copyright (C) 2004 Intel
 * Copyright (C) Tom Long Nguyen (tom.l.nguyen@intel.com)
 */

/*
 * [한국어 설명] 포트 서비스 버스의 내부 자료구조 정의 (portdrv.h)
 *
 * === 파일의 역할 ===
 * portdrv.c 가 구현하는 "포트 서비스 버스" 의 자료구조와 상수를 정의한다.
 * 이 헤더 하나로 다음이 정해진다.
 *
 *   - 서비스의 종류: PCIE_PORT_SERVICE_{PME,AER,HP,DPC,BWCTRL} 다섯 개와
 *     각각의 SHIFT 값. 마스크는 "이 포트가 제공하는 서비스들" 을 OR 로
 *     모으는 데, SHIFT 는 서비스별 배열의 인덱스로 쓴다.
 *   - struct pcie_device: 서비스 하나를 나타내는 가상 장치. 포트 하나에
 *     대해 서비스 개수만큼 만들어진다.
 *   - struct pcie_port_service_driver: 서비스 드라이버가 자기를 기술하는
 *     구조체. 마지막 필드가 struct device_driver 라 커널 드라이버 모델을
 *     그대로 재사용한다.
 *   - 각 서비스의 초기화 함수 선언. CONFIG_* 에 따라 실제 구현이거나
 *     아무것도 하지 않는 인라인 스텁이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 헤더는 코드가 아니라 계약이다. portdrv.c 가 이 정의대로 가상 장치를
 * 만들고, 각 서비스 드라이버(aer.c, dpc.c, pme.c, bwctrl.c,
 * hotplug/pciehp_core.c)가 이 정의대로 자기를 등록한다.
 *
 *   portdrv.c        : pcie_device 를 만들어 등록
 *   aer.c 등          : pcie_port_service_driver 를 채워 등록
 *   pcie_port_bus_type: 둘의 service 필드와 port_type 을 대조해 짝을 찾는다
 *
 * === 타 모듈과의 연결 ===
 * 포함하는 것: linux/compiler.h 뿐이다. 그만큼 독립적인 헤더다.
 * 포함되는 곳: drivers/pci/pcie/ 아래의 서비스 드라이버들과 portdrv.c,
 *   그리고 drivers/pci/hotplug/pciehp_core.c.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 헤더를 include 하지 않는다. NVMe 는 엔드포인트이고,
 * 이 헤더는 포트(그 위의 브리지)를 다루기 때문이다.
 *
 * 그럼에도 여기 정의된 다섯 서비스가 NVMe 의 동작을 뒷받침한다 —
 * AER 이 오류를 잡아 복구를 시작하고, DPC 가 링크를 격리하고, HP 가
 * 핫스왑을 처리하고, PME 가 절전 복귀 신호를 받고, BWCTRL 이 링크 속도
 * 변화를 알린다. 그 자세한 관계는 각 필드의 주석과 portdrv.c 의 헤더에
 * 적어 두었다.
 *
 * === 주요 함수/구조체 요약 ===
 * PCIE_PORT_SERVICE_* / *_SHIFT : 서비스 종류를 비트와 인덱스로 표현.
 * PCIE_PORT_DEVICE_MAXSERVICES  : 5. 서비스 개수와 맞춰야 한다.
 * PCIE_ANY_PORT                 : 포트 종류를 가리지 않는다는 표시(~0).
 * struct pcie_device            : 서비스 가상 장치. irq / port / service /
 *                                 priv_data / device 다섯 필드.
 * struct pcie_port_service_driver : 서비스 드라이버. probe/remove 와
 *                                 PM 콜백들, slot_reset, 그리고 매칭용
 *                                 port_type / service.
 * to_pcie_device() / to_service_driver() : container_of 로 임베디드
 *                                 구조체에서 바깥 구조체를 되찾는 매크로.
 * set_service_data() / get_service_data() : priv_data 접근 헬퍼.
 * pcie_aer_init() / pcie_hp_init() / pcie_pme_init() / pcie_dpc_init() /
 * pcie_bwctrl_init()            : 각 서비스의 초기화. CONFIG_* 가 꺼져 있으면
 *                                 0 을 돌려주는 빈 인라인으로 대체된다.
 * pcie_port_service_register() / _unregister() : 서비스 드라이버 등록/해제.
 * pcie_port_bus_type            : 이 가상 버스의 bus_type.
 */

#ifndef _PORTDRV_H_
#define _PORTDRV_H_

#include <linux/compiler.h>

/* Service Type */
/* [한국어] 하나의 PCIe 포트(Root Port 또는 스위치 포트)는 여러 종류의 "서비스" 를
 * 동시에 제공한다. 포트 드라이버(portdrv)는 그 서비스마다 별도의 가상 장치
 * (struct pcie_device)를 만들어 각각 전용 드라이버가 붙게 한다 — AER 드라이버,
 * hotplug 드라이버, PME 드라이버가 서로 다른 파일에 있으면서도 같은 포트를
 * 다룰 수 있는 이유다.
 *
 * 아래는 서비스 종류를 비트로 표현한 것이다. _SHIFT 와 마스크를 쌍으로 두는
 * 이유는, 마스크는 "이 포트가 제공하는 서비스들" 을 OR 로 모을 때 쓰고,
 * SHIFT 값은 서비스별 배열의 인덱스(0~4)로 쓰기 때문이다. 두 용도가 다르므로
 * 하나로 합칠 수 없다. */
#define PCIE_PORT_SERVICE_PME_SHIFT	0	/* Power Management Event */
/* [한국어] 하위 장치가 절전 상태에서 깨어나고 싶을 때 보내는 PME 메시지를
 * 이 포트가 받아 인터럽트로 바꿔 준다. D3 에 들어간 NVMe 가 깨어나는 경로다. */
#define PCIE_PORT_SERVICE_PME		(1 << PCIE_PORT_SERVICE_PME_SHIFT)
#define PCIE_PORT_SERVICE_AER_SHIFT	1	/* Advanced Error Reporting */
/* [한국어] 하위에서 올라온 정정 가능/불가능 오류를 수집해 보고한다.
 * NVMe 컨트롤러가 치명적 오류를 만나면 이 서비스가 복구 절차를 시작하고,
 * 결국 NVMe 드라이버의 error_detected/slot_reset 콜백이 불린다. */
#define PCIE_PORT_SERVICE_AER		(1 << PCIE_PORT_SERVICE_AER_SHIFT)
#define PCIE_PORT_SERVICE_HP_SHIFT	2	/* Native Hotplug */
/* [한국어] 슬롯의 Presence Detect 변화를 인터럽트로 받아 장치 추가/제거를
 * 처리한다. "Native" 는 펌웨어(ACPI)가 아니라 커널이 직접 다룬다는 뜻이다.
 * U.2/EDSFF 백플레인의 NVMe 핫스왑이 이 서비스로 동작한다. */
#define PCIE_PORT_SERVICE_HP		(1 << PCIE_PORT_SERVICE_HP_SHIFT)
#define PCIE_PORT_SERVICE_DPC_SHIFT	3	/* Downstream Port Containment */
/* [한국어] 하위에서 치명적 오류가 나면 그 링크를 즉시 끊어 오류가 위로
 * 번지는 것을 막는다. NVMe 가 갑자기 뽑히거나(surprise down) 오동작할 때
 * 시스템 전체가 무너지는 대신 그 드라이브만 격리된다. */
#define PCIE_PORT_SERVICE_DPC		(1 << PCIE_PORT_SERVICE_DPC_SHIFT)
#define PCIE_PORT_SERVICE_BWCTRL_SHIFT	4	/* Bandwidth Controller (notifications) */
/* [한국어] 링크 속도나 폭이 바뀌었을 때 알림을 받는다. 링크가 불안정해
 * Gen4 에서 Gen3 로 떨어지는 경우를 감지하는 데 쓴다. */
#define PCIE_PORT_SERVICE_BWCTRL	(1 << PCIE_PORT_SERVICE_BWCTRL_SHIFT)

/* [한국어] 위 서비스가 다섯 종류이므로 5. 포트 하나가 만들 수 있는
 * struct pcie_device 의 최대 개수이며, portdrv 가 서비스별 배열을 잡을 때
 * 이 값을 쓴다. 서비스를 추가하면 이 값도 함께 늘려야 한다. */
#define PCIE_PORT_DEVICE_MAXSERVICES   5

/* [한국어] DPC 를 커널이 직접 다루는지(true), 펌웨어에게 맡기는지(false).
 * 설정자: pcie/portdrv.c 가 부팅 시 _OSC 협상 결과를 보고 정한다.
 * 읽는 자: pcie/dpc.c 와 err.c 가 복구 절차를 시작할지 판단할 때.
 * 값 범위: true/false. 펌웨어가 DPC 소유권을 넘겨주지 않으면 false 가 되고,
 *   그때는 오류가 나도 커널이 개입하지 않아 NVMe 복구 동작이 달라진다.
 * 동기화: 부팅 중 한 번 정해진 뒤 바뀌지 않으므로 별도 보호가 없다. */
extern bool pcie_ports_dpc_native;

#ifdef CONFIG_PCIEAER
/* [한국어] AER 이 빌드에 포함되면 실제 초기화 함수가 있고, */
int pcie_aer_init(void);
#else
/* [한국어] 없으면 아무것도 하지 않고 성공을 반환하는 인라인이 대신 쓰인다.
 * 이 패턴이 아래 네 서비스에 똑같이 반복된다 — 호출부(portdrv.c 의 초기화
 * 순서)를 #ifdef 로 어지럽히지 않고 헤더에서 흡수하려는 것이다.
 * 0 을 반환하는 것도 의도적이다. "이 서비스는 없다" 가 오류가 아니기 때문이다. */
static inline int pcie_aer_init(void) { return 0; }
#endif

#ifdef CONFIG_HOTPLUG_PCI_PCIE
/* [한국어] 핫플러그 서비스도 같은 패턴. */
int pcie_hp_init(void);
#else
/* [한국어] 꺼져 있으면 무동작 스텁. */
static inline int pcie_hp_init(void) { return 0; }
#endif

#ifdef CONFIG_PCIE_PME
/* [한국어] PME(Power Management Event) 서비스. */
int pcie_pme_init(void);
#else
/* [한국어] 무동작 스텁. */
static inline int pcie_pme_init(void) { return 0; }
#endif

#ifdef CONFIG_PCIE_DPC
/* [한국어] DPC(Downstream Port Containment) 서비스. */
int pcie_dpc_init(void);
#else
/* [한국어] 무동작 스텁. */
static inline int pcie_dpc_init(void) { return 0; }
#endif

/* [한국어] 대역폭 제어만은 #ifdef 로 감싸지 않는다. 조건부 컴파일 없이 언제나
 * 빌드에 들어간다는 뜻이다. */
int pcie_bwctrl_init(void);

/* Port Type */
#define PCIE_ANY_PORT			(~0)

/* [한국어] 포트 서비스 하나를 나타내는 가상 장치.
 * 실제 하드웨어가 아니라 소프트웨어가 만든 장치이며, 포트 하나에 대해
 * 서비스 개수만큼(최대 PCIE_PORT_DEVICE_MAXSERVICES 개) 만들어진다.
 * 이렇게 쪼개 두면 AER·hotplug·PME 드라이버가 서로를 모르는 채로 각자
 * 자기 서비스만 담당할 수 있다 — 커널 드라이버 모델을 그대로 재사용하는
 * 방식이다. */
struct pcie_device {
	int		irq;	    /* Service IRQ/MSI/MSI-X Vector */
	/* [한국어] 이 서비스가 쓸 인터럽트 번호.
	 * 설정자: portdrv 가 pcie_port_device_register() 에서 채운다. 포트가
	 *   MSI/MSI-X 를 쓰면 서비스마다 다른 벡터가 배정될 수 있고, 아니면
	 *   여러 서비스가 같은 INTx 를 공유한다.
	 * 읽는 자: 각 서비스 드라이버의 probe 가 request_irq 에 넘긴다.
	 * 값 범위: 유효한 Linux IRQ 번호. 인터럽트가 없는 서비스면 -1.
	 * 동기화: probe/remove 시점에만 접근하므로 별도 보호가 없다. */

	struct pci_dev *port;	    /* Root/Upstream/Downstream Port */
	/* [한국어] 이 서비스가 붙어 있는 실제 포트 장치.
	 * 설정자: portdrv 가 가상 장치를 만들 때 채운다.
	 * 읽는 자: 서비스 드라이버가 config space 를 읽고 쓸 때. 예컨대 AER
	 *   드라이버는 이 포인터로 AER capability 레지스터에 접근한다.
	 * 값 범위: NULL 이 아닌 유효한 포트. NVMe SSD 가 꽂힌 슬롯의 Root Port
	 *   또는 그 위 스위치 포트가 여기 온다.
	 * 동기화: 가상 장치의 생명주기가 포트보다 짧으므로 항상 유효하다. */

	u32		service;    /* Port service this device represents */
	/* [한국어] 이 가상 장치가 담당하는 서비스 하나를 나타내는 비트.
	 * 설정자: portdrv 가 서비스마다 하나씩 만들며 지정한다.
	 * 읽는 자: 드라이버 매칭 코드. 서비스 드라이버는 자기 service 비트가
	 *   맞는 가상 장치에만 바인딩된다.
	 * 값 범위: PCIE_PORT_SERVICE_{PME,AER,HP,DPC,BWCTRL} 중 정확히 하나.
	 *   여러 비트가 동시에 서지 않는다 — 서비스마다 장치가 따로이기 때문이다.
	 * 동기화: 만들 때 정해지고 바뀌지 않는다. */

	void		*priv_data; /* Service Private Data */
	/* [한국어] 서비스 드라이버가 자유롭게 쓰는 사설 포인터.
	 * 설정자/읽는 자: 해당 서비스 드라이버 자신뿐. 예컨대 hotplug 드라이버는
	 *   여기에 struct controller 를, AER 드라이버는 오류 통계 구조체를 매단다.
	 * 값 범위: 드라이버가 정한다. probe 전에는 NULL.
	 * 동기화: 소유한 드라이버의 규칙을 따른다. */

	struct device	device;     /* Generic Device Interface */
	/* [한국어] 커널 드라이버 모델에 이 가상 장치를 등록하기 위한 임베디드 구조체.
	 * 설정자: portdrv 가 device_register() 로 등록한다.
	 * 읽는 자: 드라이버 코어. 이것이 있어야 sysfs 에 노출되고 probe/remove 가
	 *   정상적으로 불린다.
	 * 값 범위: 구조체 자체가 내장돼 있으므로 container_of 로 바깥의
	 *   pcie_device 를 되찾는다(아래 to_pcie_device 매크로).
	 * 동기화: 참조 카운트로 관리된다 — 마지막 참조가 사라져야 해제된다. */
};
#define to_pcie_device(d) container_of(d, struct pcie_device, device)

/* [한국어]
 * set_service_data - 서비스 드라이버의 사설 데이터를 pcie_device 에 맡긴다
 *
 * @dev: 이 서비스의 pcie_device.
 * @data: 맡길 포인터.
 *
 * 포트 서비스 드라이버(aer.c, dpc.c, pme.c, pciehp_core.c, bwctrl.c)는 자기
 * 상태를 어딘가 매달아 두어야 하는데, pcie_device 구조체를 서비스마다 고칠 수는
 * 없다. priv_data 한 칸을 두고 이 접근자 쌍으로 감싸는 것이 그 해법이다.
 *
 * 실행 컨텍스트: 서비스 probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   서비스 드라이버의 probe → [이 함수]
 */
static inline void set_service_data(struct pcie_device *dev, void *data)
{
	/* [한국어] 서비스 드라이버가 자기 사설 데이터를 이 자리에 맡긴다. */
	dev->priv_data = data;
}

/* [한국어]
 * get_service_data - 맡겨 둔 사설 데이터를 되찾는다
 *
 * @dev: 이 서비스의 pcie_device.
 * @return: set_service_data() 로 맡긴 포인터. 맡긴 적이 없으면 NULL.
 *
 * set_service_data() 의 짝이다. 서비스의 인터럽트 핸들러나 remove 경로가
 * 자기 상태를 되찾을 때 쓴다.
 *
 * 실행 컨텍스트: 어디서든. 필드 읽기뿐이라 잠들지 않으며 인터럽트 문맥에서도
 * 안전하다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   서비스 드라이버의 IRQ 핸들러 / remove → [이 함수]
 */
static inline void *get_service_data(struct pcie_device *dev)
{
	return dev->priv_data;
}

/* [한국어] 포트 서비스 드라이버가 자기 자신을 기술하는 구조체.
 * AER(aer.c), DPC(dpc.c), PME(pme.c), hotplug(hotplug/pciehp_core.c),
 * 대역폭 알림(bwctrl.c) 다섯 드라이버가 각각 하나씩 만들어
 * pcie_port_service_register() 로 등록한다.
 *
 * 마지막 필드가 struct device_driver 인 것이 핵심 설계다. 커널 드라이버
 * 모델은 device_driver 만 알면 되고, portdrv 는 to_service_driver() 로
 * 바깥 구조체를 되찾아 서비스 고유 정보(port_type, service)를 본다.
 * 덕분에 서비스 드라이버들이 일반 드라이버와 똑같은 방식으로 등록·매칭된다. */
struct pcie_port_service_driver {
	const char *name;
	/* [한국어] 드라이버 이름. sysfs 의 /sys/bus/pci_express/drivers/ 아래
	 * 이 이름으로 디렉터리가 생긴다.
	 * 설정자: 각 서비스 드라이버가 정적 초기화로 채운다("aerdrv", "pciehp" 등).
	 * 읽는 자: 드라이버 코어. 값 범위: NULL 이 아닌 상수 문자열. */

	int (*probe)(struct pcie_device *dev);
	/* [한국어] 이 서비스를 제공하는 포트가 발견됐을 때 불린다.
	 * 설정자: 서비스 드라이버. 읽는 자: pcie_port_bus_probe()(portdrv.c:1325)가
	 *   driver->probe 로 이 포인터를 꺼내 부른다.
	 * 반환: 0 이면 바인딩 성공, 음수면 이 포트에서는 서비스를 켜지 않는다.
	 * 실행 컨텍스트: 프로세스 컨텍스트. 여기서 request_irq 를 건다. */

	void (*remove)(struct pcie_device *dev);
	/* [한국어] 서비스 해제. probe 에서 잡은 자원을 되돌린다.
	 * 호출 시점: 포트가 제거되거나 서비스 드라이버 모듈이 내려갈 때. */

	int (*suspend)(struct pcie_device *dev);
	/* [한국어] 시스템 절전 진입 시. 서비스가 만들어 낼 인터럽트를 미리 막는다. */

	int (*resume_noirq)(struct pcie_device *dev);
	/* [한국어] 복귀 도중, 인터럽트를 다시 켜기 직전에 불린다.
	 * 순서가 중요하다 — 인터럽트가 살아나기 전에 하드웨어 상태를 정리해
	 * 두지 않으면, 절전 중 누적된 상태 비트가 깨어나자마자 인터럽트로
	 * 쏟아진다. hotplug 서비스가 이 단계에서 Slot Status 를 정리한다. */

	int (*resume)(struct pcie_device *dev);
	/* [한국어] 복귀 완료 후. 정상 동작을 재개한다. */

	int (*runtime_suspend)(struct pcie_device *dev);
	/* [한국어] 런타임 절전(시스템은 켜져 있는데 이 장치만 재우는 것) 진입. */

	int (*runtime_resume)(struct pcie_device *dev);
	/* [한국어] 런타임 절전에서 깨어날 때. */

	int (*slot_reset)(struct pcie_device *dev);
	/* [한국어] 슬롯 리셋 직후 호출. AER 복구 절차가 링크를 리셋한 뒤,
	 * 각 서비스에게 "하드웨어가 초기화됐으니 설정을 다시 써 넣으라" 고
	 * 알리는 자리다. 앞의 PM 콜백들과 빈 줄로 떨어뜨려 둔 것은 성격이
	 * 다르기 때문이다 — 이쪽은 절전이 아니라 오류 복구 경로다. */

	int port_type;  /* Type of the port this driver can handle */
	/* [한국어] 이 드라이버가 다룰 수 있는 포트 종류.
	 * 설정자: 서비스 드라이버가 정적 초기화로 지정.
	 * 읽는 자: pcie_port_bus_type 의 match 함수.
	 * 값 범위: PCI_EXP_TYPE_ROOT_PORT, PCI_EXP_TYPE_UPSTREAM,
	 *   PCI_EXP_TYPE_DOWNSTREAM, PCI_EXP_TYPE_RC_EC 등. 종류를 가리지 않으면
	 *   PCIE_ANY_PORT(~0). 예컨대 hotplug 는 슬롯이 있는 하류 포트에만
	 *   의미가 있으므로 종류를 좁히고, AER 은 어느 포트에서나 필요하다. */

	u32 service;    /* Port service this device represents */
	/* [한국어] 이 드라이버가 담당하는 서비스 비트.
	 * 값 범위: PCIE_PORT_SERVICE_{PME,AER,HP,DPC,BWCTRL} 중 하나.
	 * struct pcie_device 의 같은 이름 필드와 값이 일치해야 매칭된다 —
	 * 두 필드를 짝지어 보는 것이 pcie_port_bus_match() 가 하는 일 전부다. */

	struct device_driver driver;
	/* [한국어] 커널 드라이버 모델에 등록하기 위한 임베디드 구조체.
	 * 설정자: pcie_port_service_register() 가 bus 를 pcie_port_bus_type 으로,
	 *   probe/remove 를 portdrv 의 래퍼로 채운 뒤 driver_register() 한다.
	 * 읽는 자: 드라이버 코어. 아래 to_service_driver() 로 역산한다. */
};
#define to_service_driver(d) \
	container_of(d, struct pcie_port_service_driver, driver)

/* [한국어] 서비스 드라이버(aer.c, dpc.c, pme.c, pciehp_core.c, bwctrl.c)가 자기
 * 구조체를 등록한다. 이 함수가 bus 를 pcie_port_bus_type 으로 채워
 * 드라이버 코어에 넘긴다. */
int pcie_port_service_register(struct pcie_port_service_driver *new);
/* [한국어] 그 반대. 모듈이 내려갈 때 부른다. */
void pcie_port_service_unregister(struct pcie_port_service_driver *new);

/* [한국어] 포트 서비스 전용 가상 버스. 일반 PCI 버스와 별개의 버스 타입을 두는
 * 이유는, 포트 하나에 여러 서비스 드라이버가 동시에 붙어야 하는데
 * 드라이버 코어는 장치 하나에 드라이버 하나만 허용하기 때문이다.
 * 그래서 서비스마다 pcie_device 를 따로 만들어 이 버스에 올린다. */
extern const struct bus_type pcie_port_bus_type;

/* [한국어] 아래 선언들이 포인터로만 쓰므로 전방 선언으로 충분하다.
 * linux/pci.h 를 여기서 포함하지 않아도 되게 한다. */
struct pci_dev;

#ifdef CONFIG_PCIE_PME
/* [한국어] PME 에서 MSI 를 쓰지 않도록 꺼 두는 전역 플래그.
 * 설정자: 아래 pcie_pme_disable_msi() 를 통해서만 세워진다. 실제로는
 *   MSI 가 신뢰할 수 없다고 알려진 칩셋의 쿼크가 부른다.
 * 읽는 자: pcie_pme_no_msi() 를 통해 pme.c 가 읽는다.
 * 값 범위: 기본 false. 한 번 true 가 되면 되돌리는 경로가 없다 —
 *   꺼야 할 이유가 생겼다면 그 이유가 사라지지 않기 때문이다.
 * 동기화: 부팅 초기에 한 번 세워지고 이후 읽기만 하므로 보호가 없다. */
extern bool pcie_pme_msi_disabled;

/* [한국어]
 * pcie_pme_disable_msi - PME 가 MSI 를 쓰지 않도록 전역 플래그를 세운다
 *
 * PME 인터럽트를 MSI 로 받으면 안 되는 칩셋이 있어, 그런 하드웨어의 쿼크가
 * 이 함수를 부른다. 세우기만 하고 되돌리는 짝이 없는데, 끄기로 판단한 이유가
 * 나중에 사라지지 않기 때문이다.
 *
 * CONFIG_PCIE_PME 가 꺼진 빌드에서는 아래 #else 쪽의 무동작 스텁이 쓰인다.
 * 플래그 자체가 없으므로 세울 것도 없다.
 *
 * 실행 컨텍스트: 부팅 초기의 쿼크 적용 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   칩셋 쿼크 → [이 함수] → pcie_pme_msi_disabled = true
 */
static inline void pcie_pme_disable_msi(void)
{
	/* [한국어] 단방향으로만 세운다. 끄는 함수가 없다는 점이 이 플래그의 성격을 말해 준다. */
	pcie_pme_msi_disabled = true;
}

/* [한국어]
 * pcie_pme_no_msi - PME 가 MSI 를 쓰지 말아야 하는지 묻는다
 *
 * @return: true = MSI 를 쓰지 말 것, false = 써도 됨.
 *
 * 위 함수가 세운 플래그를 읽는다. 전역 변수를 직접 읽지 않고 접근자를 두는
 * 이유는, CONFIG_PCIE_PME 가 꺼진 빌드에서 변수 자체가 없어도 호출부가
 * 그대로 컴파일되게 하려는 것이다.
 *
 * 그 스텁은 언제나 false 를 돌려준다. PME 가 없으면 MSI 를 막을 이유도
 * 없기 때문이며, 반대로 true 를 돌려주면 PME 와 무관한 경로까지 MSI 를
 * 포기하게 된다.
 *
 * 실행 컨텍스트: MSI 설정 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pme.c 의 인터럽트 설정 → [이 함수] → pcie_pme_msi_disabled
 */
static inline bool pcie_pme_no_msi(void)
{
	/* [한국어] 현재 값을 그대로 돌려준다. */
	return pcie_pme_msi_disabled;
}

/* [한국어] PME 인터럽트를 켜고 끈다. pme.c 가 정의한다. */
void pcie_pme_interrupt_enable(struct pci_dev *dev, bool enable);
#else /* !CONFIG_PCIE_PME */
/* [한국어] PME 가 빌드에 없으면 이 플래그를 세울 일도 없으므로 무동작. */
static inline void pcie_pme_disable_msi(void) {}
/* [한국어] 언제나 false — MSI 를 막을 이유가 없다. 여기서 true 를 돌려주면
 * PME 와 무관한 다른 경로까지 MSI 를 포기하게 되므로 반대로 두면 안 된다. */
static inline bool pcie_pme_no_msi(void) { return false; }
/* [한국어] PME 가 없으면 켤 인터럽트도 없다. */
/* [한국어]
 * pcie_pme_interrupt_enable - PME 인터럽트를 켜고 끈다 (스텁)
 *
 * @dev: 대상 포트.
 * @en: true = 켜기, false = 끄기.
 *
 * CONFIG_PCIE_PME 가 꺼진 빌드의 무동작 스텁이다. PME 서비스가 없으면 켤
 * 인터럽트도 없으므로 아무 일도 하지 않는다.
 *
 * CONFIG_PCIE_PME 가 켜져 있으면 같은 이름의 실제 함수를 pme.c 가 정의한다
 * (위 :309 의 선언).
 *
 * 실행 컨텍스트: 호출부를 따른다. 스텁은 아무 제약이 없다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_portdrv 의 절전/복귀 경로 → [이 함수](아무 일도 하지 않는다)
 */
static inline void pcie_pme_interrupt_enable(struct pci_dev *dev, bool en) {}
#endif /* !CONFIG_PCIE_PME */

/* [한국어] 특정 포트에서 특정 서비스의 device 를 찾아 준다. 예를 들어 어떤 포트의
 * AER 서비스 device 가 필요할 때 쓴다. */
struct device *pcie_port_find_device(struct pci_dev *dev, u32 service);

/* [한국어] CXL 관련 선언이 이 타입을 포인터로만 쓰므로 전방 선언으로 충분하다.
 * aer.c 의 내부 타입이라 헤더를 끌어오지 않는 편이 낫다. */
struct aer_err_info;

#ifdef CONFIG_CXL_RAS
/* [한국어] CXL RAS 가 빌드에 있으면 aer.c 와 aer_cxl_rch.c 가 정의한다. */
bool is_aer_internal_error(struct aer_err_info *info);
/* [한국어] RCEC 내부 오류를 CXL 쪽으로 넘기는 진입점(aer_cxl_rch.c). */
void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info);
/* [한국어] CXL 장치가 있는 RCEC 에서만 내부 오류 마스크를 푸는 초기화(aer_cxl_rch.c). */
void cxl_rch_enable_rcec(struct pci_dev *rcec);
#else
/* [한국어] CXL RAS 가 없으면 "내부 오류가 아니다" 로 답한다. 이렇게 해 두면
 * aer.c 의 호출부가 #ifdef 없이 그대로 컴파일되고, CXL 경로는 자연스럽게
 * 죽는다. */
static inline bool is_aer_internal_error(struct aer_err_info *info) { return false; }
/* [한국어]
 * cxl_rch_handle_error - RCEC 내부 오류를 CXL 쪽으로 넘긴다 (스텁)
 *
 * @dev: 오류를 보고한 장치.
 * @info: aer.c 가 채운 오류 정보.
 *
 * CONFIG_CXL_RAS 가 꺼진 빌드의 무동작 스텁이다. aer.c 의
 * handle_error_source() 가 표준 처리 앞에서 이것을 부르는데(:2932),
 * 스텁이면 그 호출이 통째로 사라지고 표준 AER 처리만 남는다.
 *
 * CONFIG_CXL_RAS 가 켜져 있으면 aer_cxl_rch.c 가 실제 구현을 제공한다.
 * 호출부를 #ifdef 로 감싸지 않아도 되게 하는 것이 이 스텁의 목적이다.
 *
 * 실행 컨텍스트: AER 스레드 핸들러. 스텁은 아무 일도 하지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   aer.c 의 handle_error_source() → [이 함수](아무 일도 하지 않는다)
 */
static inline void cxl_rch_handle_error(struct pci_dev *dev, struct aer_err_info *info) { }
/* [한국어]
 * cxl_rch_enable_rcec - CXL 장치가 있는 RCEC 의 내부 오류를 켠다 (스텁)
 *
 * @rcec: AER 서비스가 붙은 포트.
 *
 * CONFIG_CXL_RAS 가 꺼진 빌드의 무동작 스텁이다. AER 서비스 probe 가
 * IRQ 를 얻은 직후 이것을 부르는데(aer.c:3981), 스텁이면 내부 오류 마스크를
 * 건드리지 않아 기본값(마스크됨) 그대로 남는다.
 *
 * 그것이 옳은 동작이다. 내부 오류는 장치마다 의미가 달라 일반적으로 켜지
 * 않으며, CXL 만이 그것을 표준화된 방식으로 쓰기 때문에 예외적으로 켜는
 * 것이기 때문이다. CXL 지원이 없는 커널에서는 켤 이유가 없다.
 *
 * 실행 컨텍스트: AER 서비스 probe. 스텁은 아무 일도 하지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   aer.c 의 AER 서비스 probe(:3981) → [이 함수](아무 일도 하지 않는다)
 */
static inline void cxl_rch_enable_rcec(struct pci_dev *rcec) { }
#endif /* CONFIG_CXL_RAS */
#endif /* _PORTDRV_H_ */
