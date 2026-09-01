// SPDX-License-Identifier: GPL-2.0+
/*
 * cpcihp_zt5550.c
 *
 * Intel/Ziatech ZT5550 CompactPCI Host Controller driver
 *
 * Copyright 2002 SOMA Networks, Inc.
 * Copyright 2001 Intel San Luis Obispo
 * Copyright 2000,2001 MontaVista Software Inc.
 *
 * Send feedback to <scottm@somanetworks.com>
 */

/* [한국어] module_init/exit, MODULE_ 계열 매크로. */
/*
 * [한국어 설명] Ziatech ZT5550 전용 CompactPCI 핫스왑 호스트 컨트롤러 드라이버 (cpcihp_zt5550.c)
 *
 * === 파일의 역할 ===
 * Intel/Ziatech ZT5550 보드에 들어간 CompactPCI 핫스왑 호스트 컨트롤러를
 * 다룬다. CompactPCI 핫플러그 코어(cpci_hotplug_core.c)가 요구하는 콜백을
 * 채워 주는 것이 전부이며, 그중 필수인 것은 "#ENUM 신호가 지금 서 있는가"
 * 하나다.
 * 같은 디렉터리의 cpcihp_generic.c 와 나란히 놓고 보면 성격이 뚜렷해진다.
 * 그쪽은 **어떤 보드에서도** 쓸 수 있는 범용 드라이버라 포트 주소, 비트
 * 위치, 브리지 위치, 슬롯 범위를 전부 모듈 파라미터로 받는다. 이쪽은 특정
 * 보드 전용이라 그 값들을 알고 있다 — ENUM_PORT 와 ENUM_MASK 는 헤더 상수,
 * 브리지는 DEC 21154 라는 칩 ID 로 찾고, 슬롯 범위는 0x0a~0x0f 로 박혀 있다.
 * 그 대신 범용 판이 하지 못하는 일을 한다. 호스트 컨트롤러 칩의 CSR 창을
 * 직접 매핑해 인터럽트를 다루므로, #ENUM 을 폴링하지 않고 인터럽트로 받을
 * 수 있다. poll 모듈 파라미터가 두 방식을 가르며, 그 선택이 별도 플래그가
 * 아니라 **ops 표에 IRQ 콜백을 채우느냐 마느냐** 로 코어에 전달된다.
 * 레지스터 접근이 두 가지라는 점도 이 하드웨어의 특징이다. 호스트 제어·
 * 오류·직렬 인터럽트는 인덱스와 데이터 두 레지스터를 거치는 **간접 접근**
 * 이고, 인터럽트 상태와 마스크는 **직접 접근** 이다. 좁은 CSR 창으로 많은
 * 레지스터를 다루려는 설계다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * CompactPCI 핫플러그는 세 층이며 이 파일은 맨 아래다.
 *   cpci_hotplug_core.c   슬롯 자료구조, sysfs, 폴링 스레드 또는 IRQ 처리
 *   cpci_hotplug_pci.c    카드의 HS_CSR 조작과 슬롯 열거·해제
 *   [이 파일]             보드의 #ENUM 상태와 인터럽트 제어
 * cpcihp_generic.c 와 형제 관계이며, 둘 중 하나만 로드된다.
 * 흐름:
 *   module_init → zt5550_init() → #ENUM 포트 확보 → pci_register_driver()
 *     → (매치되면) zt5550_hc_init_one()
 *        → zt5550_hc_config() 로 CSR 매핑과 인터럽트 끄기
 *        → poll 여부로 ops 표 구성
 *        → cpci_hp_register_controller() → cpci_hp_register_bus(0x0a~0x0f)
 *        → cpci_hp_start()
 *   이후 코어가 query_enum() 을 폴링하거나, IRQ 가 오면 check_irq() 로
 *   자기 것인지 확인하고 처리 중에는 disable_irq()/enable_irq() 로 여닫는다.
 * 실행 컨텍스트는 셋이다. 모듈 초기화와 probe/remove 는 프로세스 컨텍스트,
 * query_enum 과 check_irq 는 폴링 스레드나 인터럽트 경로에서 불려 잠들 수
 * 없고, enable/disable_irq 는 코어의 사정을 따른다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: cpci_hotplug.h 의 struct cpci_hp_controller 와 그 ops 규약,
 * cpci_hp_register_controller() / _register_bus() / _start() / 그 짝.
 * 옆쪽: cpcihp_zt5550.h 의 CSR 오프셋(CSR_HCINDEX, CSR_HCDATA, CSR_INTSTAT,
 * CSR_INTMASK)과 마스크 비트(HC_INT_MASK_REG, ALL_INDEXED_INTS_MASK,
 * ALL_DIRECT_INTS_MASK, ENUM_INT_MASK), 그리고 ENUM_PORT / ENUM_MASK.
 * 아래쪽: PCI 코어(pci_enable_device, pci_resource_start/len, pci_get_device,
 * pci_register_driver), I/O 포트(inb_p, request_region), MMIO(ioremap,
 * readb/writeb).
 * 공유 상태: 파일 전역 아홉 개가 사실상 이 드라이버의 상태 전부다.
 * 파라미터 둘(debug, poll), 코어에 넘길 서술자 둘(zt5550_hpc_ops,
 * zt5550_hpc), 장치·버스 셋(bus0_dev, bus0, hc_dev), 그리고 매핑된
 * 레지스터 주소 다섯. 보드에 호스트 컨트롤러가 하나뿐이라는 전제 위의
 * 설계이며, zt5550_hc_config() 의 첫 검사가 그 전제를 강제한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - zt5550_hc_config(): 하드웨어 준비 전부. 장치 켜기 → CSR 구간 확보 →
 *   매핑 → 레지스터 주소 캐시 → 인터럽트 모두 끄기. 간접 접근(인덱스에
 *   번호를 쓰고 데이터에 값을 쓴다)과 직접 접근이 나란히 나오는 곳이다.
 * - zt5550_hc_cleanup(): 그 역순. hc_dev 검사가 "설정된 적이 있는가" 를 묻는다.
 * - zt5550_hc_query_enum(): 코어가 요구하는 필수 콜백. cpcihp_generic.c 의
 *   같은 함수와 형태가 같고 상수의 출처만 다르다.
 * - zt5550_hc_check_irq(): 공유 인터럽트에서 자기 것을 가려낸다. dev_id 와
 *   상태 레지스터를 두 겹으로 확인한다.
 * - zt5550_hc_enable_irq() / _disable_irq(): 마스크에서 ENUM 비트를 지우거나
 *   세운다. 마스크는 1 이 차단이므로 켜기가 지우기다. 읽기-수정-쓰기를
 *   쓰는 이유는 같은 레지스터에 다른 인터럽트의 마스크가 함께 있기 때문이다.
 * - zt5550_hc_init_one(): probe. poll 파라미터가 ops 표를 가르고, 그 유무가
 *   곧 모드 선택으로 코어에 전달된다.
 * - zt5550_hc_remove_one(): 인자를 쓰지 않는다 — 모든 상태가 전역에 있다.
 * - zt5550_init(): 포트를 **먼저** 확보하고 드라이버를 등록한다. 등록하는
 *   순간 probe 가 불려 그 포트를 읽을 수 있기 때문이다.
 *
 * === 상류 코드 관찰 ===
 * 코드는 고치지 않고 사실만 기록한다.
 * - zt5550_hc_cleanup() 과 config 의 두 오류 경로가 hc_dev 를 NULL 로
 *   되돌리지 않는다. 그 전역이 남아 있으면 다음 config 가 첫 줄의 중복
 *   검사에 걸려 "too many host controller devices" 로 거절한다.
 * - zt5550_hc_init_one() 의 되감기가 완전히 라벨로 모이지 않았다. 시작 실패
 *   경로만 cpci_hp_unregister_bus() 를 그 자리에서 직접 부르는데, 아래
 *   라벨이 그것을 하지 않기 때문이다.
 * - poll 파라미터가 0644 로 실행 중에도 쓸 수 있게 되어 있으나 probe 때
 *   한 번만 읽히므로, 로드된 뒤에 바꿔도 효과가 없다.
 * - warn 매크로가 정의만 되고 쓰이지 않는다.
 *
 * === NVMe 관점 ===
 * 접점이 없다. ZT5550 은 2000년대 초 통신 장비용 CompactPCI 보드이고,
 * 그 슬롯에 NVMe SSD 가 꽂힐 일은 없다.
 * 다만 보드 전용 드라이버와 범용 드라이버의 대비 — 같은 코어 규약을 두고,
 * 한쪽은 모든 것을 파라미터로 받고 다른 쪽은 알고 있는 값을 박아 둔다 —
 * 는 지금도 그대로 반복되는 형태다. NVMe 쪽에서도 nvme/host/pci.c 가
 * 표준 규격만 믿고 동작하는 반면, 특정 컨트롤러의 결함은 quirk 표로
 * 따로 다루는 것이 같은 분업이다.
 */

#include <linux/module.h>
/* [한국어] module_param. */
#include <linux/moduleparam.h>
/* [한국어] __init / __exit 섹션 표시. */
#include <linux/init.h>
/* [한국어] -EBUSY / -ENODEV / -ENOMEM. */
#include <linux/errno.h>
/* [한국어] pci_enable_device(), pci_resource_start/len(), pci_get_device(),
 * pci_register_driver(). */
#include <linux/pci.h>
/* [한국어] 이 파일은 직접 쓰지 않지만 상류가 남겨 둔 include 다. */
#include <linux/interrupt.h>
/* [한국어] 옆의 영어 주석대로 IRQF_SHARED 때문이다. */
#include <linux/signal.h>	/* IRQF_SHARED */
/* [한국어] struct cpci_hp_controller 와 그 ops, cpci_hp_register_controller() 등
 * CompactPCI 핫플러그 코어의 규약. */
#include "cpci_hotplug.h"
/* [한국어] 이 보드의 CSR 오프셋과 인터럽트 마스크 비트 정의. */
#include "cpcihp_zt5550.h"

/* [한국어] 모듈 정보 문자열. */
#define DRIVER_VERSION	"0.2"
/* [한국어] 작성자. */
#define DRIVER_AUTHOR	"Scott Murray <scottm@somanetworks.com>"
/* [한국어] 설명. */
#define DRIVER_DESC	"ZT5550 CompactPCI Hot Plug Driver"

/* [한국어] 로그 접두사. cpcihp_generic.c 가 모듈 여부로 갈랐던 것과 달리 상수로 고정한다. */
#define MY_NAME	"cpcihp_zt5550"

/* [한국어] debug 파라미터가 켜졌을 때만 찍는다. */
#define dbg(format, arg...)					\
	do {							\
		if (debug)					\
			printk(KERN_DEBUG "%s: " format "\n",	\
				MY_NAME, ## arg);		\
	} while (0)
/* [한국어] 오류 로그. 이 파일도 dev_err 를 쓸 수 없는데, 로그를 내는 시점에
 * struct device 를 들고 있지 않은 경우가 있기 때문이다. */
#define err(format, arg...) printk(KERN_ERR "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 정보 로그. */
#define info(format, arg...) printk(KERN_INFO "%s: " format "\n", MY_NAME, ## arg)
/* [한국어] 경고 로그. 이 파일에서는 쓰이지 않는다. */
#define warn(format, arg...) printk(KERN_WARNING "%s: " format "\n", MY_NAME, ## arg)

/* local variables */
/* [한국어] 디버그 로그를 켤지.
 * 설정자: 모듈 파라미터(0644, 실행 중에도 바꿀 수 있다).
 * 읽는 자: 위 dbg 매크로.  값 범위: true/false.
 * 동기화: 단순 읽기라 보호가 없다. */
static bool debug;
/* [한국어] 인터럽트 대신 폴링으로 #ENUM 을 감시할지.
 * 설정자: 모듈 파라미터.  읽는 자: zt5550_hc_init_one() 이 ops 표를
 *   채울 때 한 번.
 * 값 범위: false = 인터럽트, true = 폴링. 인터럽트가 신뢰할 수 없는
 *   보드나 공유 IRQ 문제가 있을 때의 대비책이다.
 * 동기화: probe 때 한 번 읽고 이후 보지 않는다 — 실행 중에 바꿔도
 *   이미 등록된 컨트롤러에는 반영되지 않는다. */
static bool poll;
/* [한국어] 핫플러그 코어에 넘길 콜백 표. 폴링 모드면 query_enum 하나만,
 *   인터럽트 모드면 IRQ 관련 셋을 더 채운다.
 * 설정자/읽는 자: zt5550_hc_init_one() 과 코어.
 * 동기화: 전역이지만 컨트롤러가 하나뿐이라는 전제다. */
static struct cpci_hp_controller_ops zt5550_hpc_ops;
/* [한국어] 컨트롤러 서술자. 위 ops 를 가리키며 IRQ 정보도 담는다.
 * 설정자: zt5550_hc_init_one().  읽는 자: 코어와 check_irq.
 * 동기화: 위와 같다. */
static struct cpci_hp_controller zt5550_hpc;

/* Primary cPCI bus bridge device */
/* [한국어] cPCI 버스의 브리지 장치. 옆의 영어 주석대로 1차 버스 쪽이다.
 * 설정자: zt5550_hc_init_one() 의 pci_get_device().
 * 읽는 자: 그 자리에서 subordinate 만 꺼내고 참조를 바로 놓는다.
 * 값 범위: 유효한 pci_dev 포인터.
 * 동기화: probe 안에서만 쓰이므로 사실상 지역 변수다. */
static struct pci_dev *bus0_dev;
/* [한국어] 그 브리지 아래의 버스. 핫스왑 슬롯들이 여기 매달린다.
 * 설정자: zt5550_hc_init_one().  읽는 자: 등록·해제 경로.
 * 값 범위: 유효한 pci_bus 포인터.
 * 동기화: probe 에서 정해지고 remove 까지 유지된다. */
static struct pci_bus *bus0;

/* Host controller device */
/* [한국어] 이 호스트 컨트롤러 장치. 이 포인터가 NULL 인지 아닌지가 "이미 설정됨" 의
 *   표시로도 쓰인다.
 * 설정자: zt5550_hc_config().  읽는 자: cleanup 과 IRQ 함수들.
 * 값 범위: 유효한 pci_dev 포인터 또는 NULL.
 * 동기화: config 가 이 값을 보고 두 번째 컨트롤러를 거절한다.
 * [상류 코드 관찰] cleanup 이 이 포인터를 NULL 로 되돌리지 않아,
 *   모듈을 뺐다 다시 넣으면 config 가 "이미 있다" 고 판단한다. */
static struct pci_dev *hc_dev;

/* Host controller register addresses */
/* [한국어] 매핑된 CSR 창의 시작.
 * 설정자: zt5550_hc_config().  읽는 자: 아래 IRQ 함수들과 cleanup.
 * 값 범위: 유효한 __iomem 포인터.
 * 동기화: 보드에 호스트 컨트롤러가 하나뿐이라는 전제 위의 전역이다.
 *   config 가 그 전제를 hc_dev 검사로 강제한다. */
static void __iomem *hc_registers;
/* [한국어] 호스트 컨트롤러 간접 접근의 인덱스 레지스터.
 *   인덱스를 쓰고 데이터를 쓰는 두 단계 방식이라, 좁은 창으로 많은
 *   레지스터를 다룰 수 있다.
 * 설정자: zt5550_hc_config().  읽는 자: 아래 IRQ 함수들과 cleanup.
 * 값 범위: 유효한 __iomem 포인터.
 * 동기화: 보드에 호스트 컨트롤러가 하나뿐이라는 전제 위의 전역이다.
 *   config 가 그 전제를 hc_dev 검사로 강제한다. */
static void __iomem *csr_hc_index;
/* [한국어] 그 간접 접근의 데이터 레지스터.
 * 설정자: zt5550_hc_config().  읽는 자: 아래 IRQ 함수들과 cleanup.
 * 값 범위: 유효한 __iomem 포인터.
 * 동기화: 보드에 호스트 컨트롤러가 하나뿐이라는 전제 위의 전역이다.
 *   config 가 그 전제를 hc_dev 검사로 강제한다. */
static void __iomem *csr_hc_data;
/* [한국어] 직접 접근 인터럽트 상태 레지스터.
 * 설정자: zt5550_hc_config().  읽는 자: 아래 IRQ 함수들과 cleanup.
 * 값 범위: 유효한 __iomem 포인터.
 * 동기화: 보드에 호스트 컨트롤러가 하나뿐이라는 전제 위의 전역이다.
 *   config 가 그 전제를 hc_dev 검사로 강제한다. */
static void __iomem *csr_int_status;
/* [한국어] 직접 접근 인터럽트 마스크 레지스터. 상태와 마스크는 간접이 아니라
 *   직접 접근이라는 점이 위 두 개와 다르다.
 * 설정자: zt5550_hc_config().  읽는 자: 아래 IRQ 함수들과 cleanup.
 * 값 범위: 유효한 __iomem 포인터.
 * 동기화: 보드에 호스트 컨트롤러가 하나뿐이라는 전제 위의 전역이다.
 *   config 가 그 전제를 hc_dev 검사로 강제한다. */
static void __iomem *csr_int_mask;


/* [한국어]
 * zt5550_hc_config - 호스트 컨트롤러의 CSR 창을 확보·매핑하고 인터럽트를 모두 끈다
 *
 * @pdev: 매치된 호스트 컨트롤러 장치.
 * @return: 0 = 성공, -EBUSY = 이미 하나 있음, -ENOMEM / -ENODEV = 자원 실패.
 *
 * 하드웨어 준비의 전부다. 장치를 켜고, BAR 1 의 CSR 구간을 독점 확보해
 * 매핑하고, 자주 쓰는 레지스터 주소를 미리 계산해 두고, 인터럽트를 모두 끈다.
 *
 * 첫 줄의 중복 검사가 눈에 띈다. 함수 안의 영어 주석대로 이 보드에 호스트
 * 컨트롤러 칩이 둘일 수는 없으므로, 두 번째가 오면 오류로 다룬다.
 * hc_dev 전역이 그 판정 근거이자 "이미 설정됨" 의 표시를 겸한다.
 *
 * 인터럽트를 끄는 두 대목이 이 하드웨어의 레지스터 접근 방식을 보여 준다.
 * 호스트 제어·오류·직렬 인터럽트는 **간접 접근** 이라 인덱스에 레지스터
 * 번호를 쓰고 데이터에 값을 쓰는 두 단계가 필요하고, 타이머와 ENUM 은
 * **직접 접근** 이라 한 줄로 끝난다. 좁은 CSR 창으로 많은 레지스터를 다루려는
 * 설계다.
 *
 * 모두 꺼 둔 채 초기화를 마치는 것이 의도적이다. 나중에 코어가
 * zt5550_hc_enable_irq() 로 ENUM 만 다시 연다.
 *
 * 실행 컨텍스트: PCI probe. 프로세스 컨텍스트이며 잠들 수 있다.
 *
 * 에러 경로: 두 라벨이 계단을 이룬다. 매핑 실패는 구간 반환부터,
 * 구간 확보 실패는 장치 끄기부터 되돌린다.
 * [상류 코드 관찰] 두 경로 모두 hc_dev 를 NULL 로 되돌리지 않아, 실패 뒤
 * 다시 시도하면 첫 줄의 중복 검사에 걸린다.
 *
 * 호출 체인:
 *   zt5550_hc_init_one() → [이 함수] → pci_enable_device()
 *     → request_mem_region() → ioremap() → writeb()
 */
static int zt5550_hc_config(struct pci_dev *pdev)
{
	/* [한국어] 결과. */
	int ret;

	/* Since we know that no boards exist with two HC chips, treat it as an error */
	/* [한국어] 옆의 영어 주석대로 이 보드에 호스트 컨트롤러가 둘일 수는 없으므로, */
	if (hc_dev) {
		/* [한국어] 두 번째가 오면 오류로 기록하고, */
		err("too many host controller devices?");
		/* [한국어] 장치 사용 중으로 거절한다. */
		return -EBUSY;
	}

	/* [한국어] 장치를 켠다. BAR 이 배정되고 config 접근이 가능해진다. */
	ret = pci_enable_device(pdev);
	/* [한국어] 실패하면, */
	if (ret) {
		/* [한국어] 어느 장치였는지 남기고, */
		err("cannot enable %s\n", pci_name(pdev));
		return ret;
	}

	/* [한국어] 전역에 기록한다. 이 대입이 "이미 설정됨" 의 표시가 된다. */
	hc_dev = pdev;
	/* [한국어] 장치 주소를 디버그 로그에 남긴다. */
	dbg("hc_dev = %p", hc_dev);
	/* [한국어] BAR 1 의 시작 주소. */
	dbg("pci resource start %llx", (unsigned long long)pci_resource_start(hc_dev, 1));
	/* [한국어] 그 길이. 이 보드의 CSR 이 BAR 1 에 있다. */
	dbg("pci resource len %llx", (unsigned long long)pci_resource_len(hc_dev, 1));

	/* [한국어] 그 구간을 독점 확보한다. 다른 드라이버가 같은 CSR 을 건드리지 못하게
	 * 하는 것이 목적이다. */
	if (!request_mem_region(pci_resource_start(hc_dev, 1),
				pci_resource_len(hc_dev, 1), MY_NAME)) {
		/* [한국어] 실패하면 기록하고, */
		err("cannot reserve MMIO region");
		/* [한국어] 메모리 부족으로 표시한 뒤, */
		ret = -ENOMEM;
		/* [한국어] 장치를 다시 끄러 간다. */
		goto exit_disable_device;
	}

	/* [한국어] 확보한 구간을 매핑한다. */
	hc_registers =
	    ioremap(pci_resource_start(hc_dev, 1), pci_resource_len(hc_dev, 1));
	/* [한국어] 실패하면, */
	if (!hc_registers) {
		/* [한국어] 어느 구간이었는지 길이와 주소를 함께 남기고, */
		err("cannot remap MMIO region %llx @ %llx",
			(unsigned long long)pci_resource_len(hc_dev, 1),
			(unsigned long long)pci_resource_start(hc_dev, 1));
		/* [한국어] 장치 없음으로 표시한 뒤, */
		ret = -ENODEV;
		/* [한국어] 구간 반환부터 되돌리러 간다. */
		goto exit_release_region;
	}

	/* [한국어] 간접 접근 인덱스 레지스터의 주소를 계산해 둔다. 매번 오프셋을 더하지
	 * 않으려는 캐시다. */
	csr_hc_index = hc_registers + CSR_HCINDEX;
	/* [한국어] 간접 접근 데이터 레지스터. */
	csr_hc_data = hc_registers + CSR_HCDATA;
	/* [한국어] 직접 접근 상태 레지스터. */
	csr_int_status = hc_registers + CSR_INTSTAT;
	/* [한국어] 직접 접근 마스크 레지스터. */
	csr_int_mask = hc_registers + CSR_INTMASK;

	/*
	 * Disable host control, fault and serial interrupts
	 */
	/* [한국어] 위 영어 주석대로 호스트 제어·오류·직렬 인터럽트를 끈다는 사실을 남기고, */
	dbg("disabling host control, fault and serial interrupts");
	/* [한국어] 간접 접근의 첫 단계 — 인덱스에 대상 레지스터 번호를 쓴다. */
	writeb((u8) HC_INT_MASK_REG, csr_hc_index);
	/* [한국어] 둘째 단계 — 데이터에 마스크 값을 쓴다. 이 두 줄이 한 쌍으로 간접
	 * 레지스터 하나를 쓰는 방법이다. */
	writeb((u8) ALL_INDEXED_INTS_MASK, csr_hc_data);
	/* [한국어] 완료를 남긴다. */
	dbg("disabled host control, fault and serial interrupts");

	/*
	 * Disable timer0, timer1 and ENUM interrupts
	 */
	/* [한국어] 위 영어 주석대로 타이머와 ENUM 인터럽트를 끈다는 사실을 남기고, */
	dbg("disabling timer0, timer1 and ENUM interrupts");
	/* [한국어] 이쪽은 직접 접근이라 한 줄로 끝난다. 인터럽트를 모두 꺼 둔 채 초기화를
	 * 마치고, 나중에 코어가 zt5550_hc_enable_irq() 로 ENUM 만 다시 연다. */
	writeb((u8) ALL_DIRECT_INTS_MASK, csr_int_mask);
	/* [한국어] 완료를 남긴다. */
	dbg("disabled timer0, timer1 and ENUM interrupts");
	/* [한국어] 성공. */
	return 0;

/* [한국어] 매핑 실패 경로. */
exit_release_region:
	/* [한국어] 확보한 구간을 놓는다. */
	release_mem_region(pci_resource_start(hc_dev, 1),
			   pci_resource_len(hc_dev, 1));
/* [한국어] 구간 확보 실패 경로. 위에서 흘러내려 온다. */
exit_disable_device:
	/* [한국어] 장치를 끈다. */
	pci_disable_device(hc_dev);
	/* [한국어] 원래 오류를 올려보낸다.
	 * [상류 코드 관찰] 두 되감기 경로 모두 hc_dev 를 NULL 로 되돌리지 않는다.
	 *   이 함수가 실패하면 그 전역이 남아 있어, 다음 시도가 "too many host
	 *   controller devices" 로 거절된다. */
	return ret;
}

/* [한국어]
 * zt5550_hc_cleanup - config 가 잡은 것을 모두 되돌린다
 *
 * @return: 0 = 성공, -ENODEV = 설정된 적이 없음.
 *
 * zt5550_hc_config() 의 짝이며 정확한 역순이다 — 매핑 해제, 구간 반환,
 * 장치 끄기.
 *
 * hc_dev 검사가 "설정된 적이 있는가" 를 묻는다. init 이 실패한 뒤에도 불리는
 * 경로가 있어 필요한 방어다.
 *
 * [상류 코드 관찰] hc_dev 를 NULL 로 되돌리지 않는다. 모듈을 뺐다 다시 넣으면
 * 그 전역이 남아 있어 config 가 "too many host controller devices" 로
 * 거절한다. 같은 파일의 다른 전역들도 마찬가지다.
 *
 * 실행 컨텍스트: PCI remove 또는 probe 실패 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: -ENODEV 뿐이며, 호출자들이 그것을 확인하지 않는다.
 *
 * 호출 체인:
 *   zt5550_hc_init_one() 의 오류 경로 / zt5550_hc_remove_one() → [이 함수]
 *     → iounmap() → release_mem_region() → pci_disable_device()
 */
static int zt5550_hc_cleanup(void)
{
	/* [한국어] 설정된 적이 없으면 정리할 것도 없다. */
	if (!hc_dev)
		return -ENODEV;

	/* [한국어] 매핑을 푼다. */
	iounmap(hc_registers);
	/* [한국어] 구간을 놓는다. */
	release_mem_region(pci_resource_start(hc_dev, 1),
			   pci_resource_len(hc_dev, 1));
	/* [한국어] 장치를 끈다. config 의 역순이다. */
	pci_disable_device(hc_dev);
	/* [한국어] 성공.
	 * [상류 코드 관찰] hc_dev 를 NULL 로 되돌리지 않아, 모듈 재삽입 시
	 *   config 의 중복 검사에 걸린다. */
	return 0;
}

/* [한국어]
 * zt5550_hc_query_enum - #ENUM 신호가 서 있는지 I/O 포트에서 읽어 답한다
 *
 * @return: 1 = 서 있음(슬롯 상태가 바뀜), 0 = 아님.
 *
 * 이 드라이버가 CompactPCI 핫플러그 코어에 반드시 제공해야 하는 콜백이다.
 * 폴링 모드든 인터럽트 모드든 코어는 결국 이 함수로 상태를 확인한다.
 *
 * cpcihp_generic.c 의 같은 함수와 형태가 완전히 같고 상수의 출처만 다르다.
 * 그쪽은 포트 주소와 비트 위치를 모듈 파라미터로 받는 반면, 이쪽은 특정 보드
 * 전용이라 헤더의 상수(ENUM_PORT, ENUM_MASK)로 고정되어 있다. 범용 드라이버와
 * 보드 전용 드라이버의 차이가 이 한 함수에 압축되어 있다.
 *
 * 실행 컨텍스트: 코어의 폴링 스레드 또는 인터럽트 처리. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c → controller->ops->query_enum → [이 함수] → inb_p()
 */
static int zt5550_hc_query_enum(void)
{
	/* [한국어] 포트에서 읽은 값. */
	u8 value;

	/* [한국어] #ENUM 포트를 읽는다. cpcihp_generic.c 와 달리 포트 주소가 모듈 파라미터가
	 * 아니라 헤더의 상수(ENUM_PORT)로 고정되어 있다 — 이 드라이버는 특정
	 * 보드 전용이므로 위치를 알고 있기 때문이다. */
	value = inb_p(ENUM_PORT);
	/* [한국어] 마스크와 비교해 0/1 로 만든다. cpcihp_generic.c 의 같은 함수와 형태가
	 * 완전히 같고, 상수의 출처만 다르다. */
	return ((value & ENUM_MASK) == ENUM_MASK);
}

/* [한국어]
 * zt5550_hc_check_irq - 이 인터럽트가 우리 것인지 판별한다
 *
 * @dev_id: 인터럽트와 함께 온 식별자.
 * @return: 1 = 우리 인터럽트, 0 = 아님.
 *
 * IRQF_SHARED 로 등록하므로 남의 인터럽트에도 이 판정이 불린다.
 *
 * 두 겹으로 확인한다. 먼저 dev_id 가 우리가 등록한 것과 같은지 보고,
 * 그 다음 상태 레지스터가 0 이 아닌지 본다. 앞의 검사만으로는 부족한데,
 * 같은 IRQ 선을 공유하는 다른 장치가 인터럽트를 냈을 때도 코어가
 * 우리 dev_id 로 이 함수를 부를 수 있기 때문이다.
 *
 * 폴링 모드에서는 이 콜백이 채워지지 않아 코어가 부르지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 처리 경로. 잠들지 않는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c 의 IRQ 핸들러 → controller->ops->check_irq
 *     → [이 함수] → readb()
 */
static int zt5550_hc_check_irq(void *dev_id)
{
	/* [한국어] 결과. */
	int ret;
	/* [한국어] 읽은 상태 값. */
	u8 reg;

	/* [한국어] 기본값은 "내 인터럽트가 아니다". */
	ret = 0;
	/* [한국어] 코어가 넘긴 dev_id 가 우리가 등록한 것과 같을 때만 확인한다.
	 * 공유 인터럽트라 남의 것도 이 함수를 지나기 때문이다. */
	if (dev_id == zt5550_hpc.dev_id) {
		/* [한국어] 상태 레지스터를 읽어, */
		reg = readb(csr_int_status);
		/* [한국어] 0 이 아니면, */
		if (reg)
			/* [한국어] 우리 인터럽트라고 답한다. */
			ret = 1;
	}
	/* [한국어] 결과를 돌려준다. */
	return ret;
}

/* [한국어]
 * zt5550_hc_enable_irq - ENUM 인터럽트 마스크를 푼다
 *
 * @return: 0 = 성공, -ENODEV = 하드웨어가 설정되지 않음.
 *
 * 마스크 레지스터에서 ENUM 비트를 **지우는** 것이 곧 허용이다. 마스크의
 * 관례상 1 이 차단이므로, 켜는 동작이 비트를 지우는 형태가 된다.
 *
 * 읽기-수정-쓰기를 쓰는 이유는 같은 레지스터에 타이머 등 다른 인터럽트의
 * 마스크가 함께 있기 때문이다. config 가 초기화 때 모두 세워 두었으므로,
 * 여기서 통째로 쓰면 그것들이 열린다.
 *
 * 실행 컨텍스트: 코어의 인터럽트 준비. 프로세스 컨텍스트.
 *
 * 에러 경로: -ENODEV 뿐이다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c → controller->ops->enable_irq → [이 함수]
 *     → readb() → writeb()
 */
static int zt5550_hc_enable_irq(void)
{
	/* [한국어] 읽고 고칠 마스크 값. */
	u8 reg;

	/* [한국어] 설정되지 않았으면, */
	if (hc_dev == NULL)
		return -ENODEV;

	/* [한국어] 현재 마스크를 읽어, */
	reg = readb(csr_int_mask);
	/* [한국어] ENUM 비트만 지운다. 마스크에서 지우는 것이 곧 허용이다. */
	reg = reg & ~ENUM_INT_MASK;
	/* [한국어] 되쓴다. 읽기-수정-쓰기라 다른 인터럽트의 마스크 상태를 보존한다. */
	writeb(reg, csr_int_mask);
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * zt5550_hc_disable_irq - ENUM 인터럽트 마스크를 세운다
 *
 * @return: 0 = 성공, -ENODEV = 하드웨어가 설정되지 않음.
 *
 * zt5550_hc_enable_irq() 와 완전히 대칭이며, 비트를 지우는 대신 세운다.
 *
 * 코어가 이벤트를 처리하는 동안 같은 인터럽트가 다시 들어오지 않게 막는 데
 * 쓴다. 처리가 끝나면 enable 쪽이 다시 연다.
 *
 * 실행 컨텍스트: 코어의 인터럽트 처리. 프로세스 컨텍스트일 수도 인터럽트
 * 문맥일 수도 있으나, 이 함수 자체는 잠들지 않는다.
 *
 * 에러 경로: -ENODEV 뿐이다.
 *
 * 호출 체인:
 *   cpci_hotplug_core.c → controller->ops->disable_irq → [이 함수]
 *     → readb() → writeb()
 */
static int zt5550_hc_disable_irq(void)
{
	/* [한국어] 읽고 고칠 마스크 값. */
	u8 reg;

	/* [한국어] 설정되지 않았으면, */
	if (hc_dev == NULL)
		return -ENODEV;

	/* [한국어] 현재 마스크를 읽어, */
	reg = readb(csr_int_mask);
	/* [한국어] ENUM 비트를 세운다. 세우는 것이 곧 차단이다. */
	reg = reg | ENUM_INT_MASK;
	/* [한국어] 되쓴다. 켜기 쪽과 완전히 대칭이다. */
	writeb(reg, csr_int_mask);
	/* [한국어] 성공. */
	return 0;
}

/* [한국어]
 * zt5550_hc_init_one - 하드웨어를 설정하고 핫플러그 코어에 등록한다
 *
 * @pdev: 매치된 호스트 컨트롤러 장치.
 * @ent: 매치된 ID 표 항목. 쓰지 않는다.
 * @return: 0 = 성공, 각 단계의 오류.
 *
 * PCI 드라이버의 probe 다. 네 단계를 밟는다 — 하드웨어 설정, ops 표 구성,
 * 컨트롤러 등록, 버스 등록과 시작.
 *
 * poll 파라미터가 ops 표를 가르는 것이 이 함수의 요점이다. query_enum 은
 * 어느 모드든 필요하지만, IRQ 관련 세 콜백은 인터럽트 모드에서만 채운다.
 * 폴링 모드에서는 그 셋이 NULL 로 남고, 코어가 그것을 보고 폴링 스레드를
 * 띄운다. 즉 모드 선택이 별도 플래그가 아니라 **콜백의 유무** 로 전달된다.
 *
 * 버스를 찾는 방법도 cpcihp_generic.c 와 다르다. 그쪽은 브리지의 버스·슬롯을
 * 모듈 파라미터로 받는 반면, 이쪽은 DEC 21154 라는 특정 브리지 칩을 ID 로
 * 찾는다. 슬롯 범위도 0x0a~0x0f 로 고정인데, 이 보드의 물리 슬롯 번호가
 * 정해져 있기 때문이다.
 *
 * 되감기가 두 라벨로 계단을 이루지만 완전하지는 않다. 시작 실패 경로만
 * cpci_hp_unregister_bus() 를 그 자리에서 직접 부르는데, 아래 라벨이 그것을
 * 하지 않기 때문이다.
 *
 * 실행 컨텍스트: PCI probe. 프로세스 컨텍스트.
 *
 * 에러 경로: 위 두 라벨. 어느 경로든 zt5550_hc_cleanup() 을 지난다.
 *
 * 호출 체인:
 *   pci_register_driver() → PCI 코어 매치 → [이 함수]
 *     → zt5550_hc_config() → cpci_hp_register_controller()
 *     → pci_get_device(DEC 21154) → cpci_hp_register_bus() → cpci_hp_start()
 */
static int zt5550_hc_init_one(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	/* [한국어] 각 단계의 결과. */
	int status;

	/* [한국어] 하드웨어를 설정한다. */
	status = zt5550_hc_config(pdev);
	/* [한국어] 실패하면, */
	if (status != 0)
		return status;

	/* [한국어] 돌아왔음을 디버그 로그에 남긴다. */
	dbg("returned from zt5550_hc_config");

	/* [한국어] 컨트롤러 서술자를 0 으로 지운다. static 이라 이미 0 이지만 모듈 재삽입을
	 * 고려한 것이다. */
	memset(&zt5550_hpc, 0, sizeof(struct cpci_hp_controller));
	/* [한국어] 어느 모드든 #ENUM 확인 콜백은 필요하다. */
	zt5550_hpc_ops.query_enum = zt5550_hc_query_enum;
	/* [한국어] 서술자에 콜백 표를 매단다. */
	zt5550_hpc.ops = &zt5550_hpc_ops;
	/* [한국어] 인터럽트 모드면, */
	if (!poll) {
		/* [한국어] 이 장치의 IRQ 를 쓰고, */
		zt5550_hpc.irq = hc_dev->irq;
		/* [한국어] 공유로 등록하게 하고, */
		zt5550_hpc.irq_flags = IRQF_SHARED;
		/* [한국어] 핸들러가 자기 것인지 판별할 표식을 넘긴다. */
		zt5550_hpc.dev_id = hc_dev;

		/* [한국어] IRQ 를 켜는 콜백과, */
		zt5550_hpc_ops.enable_irq = zt5550_hc_enable_irq;
		/* [한국어] 끄는 콜백, */
		zt5550_hpc_ops.disable_irq = zt5550_hc_disable_irq;
		/* [한국어] 그리고 자기 인터럽트인지 확인하는 콜백을 채운다. 폴링 모드에서는
		 * 이 셋이 NULL 로 남아 코어가 폴링 스레드를 띄운다. */
		zt5550_hpc_ops.check_irq = zt5550_hc_check_irq;
	} else {
		/* [한국어] 폴링 모드임을 알린다. 사용자가 파라미터로 고른 것이므로 정보로 남긴다. */
		info("using ENUM# polling mode");
	}

	/* [한국어] 코어에 컨트롤러를 등록한다. */
	status = cpci_hp_register_controller(&zt5550_hpc);
	/* [한국어] 실패하면, */
	if (status != 0) {
		/* [한국어] 기록하고, */
		err("could not register cPCI hotplug controller");
		/* [한국어] 하드웨어 정리로 간다. */
		goto init_hc_error;
	}
	/* [한국어] 등록 성공을 남긴다. */
	dbg("registered controller");

	/* Look for first device matching cPCI bus's bridge vendor and device IDs */
	/* [한국어] 옆의 영어 주석대로 cPCI 버스 브리지를 벤더·장치 ID 로 찾는다.
	 * DEC 21154 는 이 보드가 쓰는 PCI-PCI 브리지 칩이다. */
	bus0_dev = pci_get_device(PCI_VENDOR_ID_DEC,
				  PCI_DEVICE_ID_DEC_21154, NULL);
	/* [한국어] 없으면, */
	if (!bus0_dev) {
		/* [한국어] 장치 없음으로 표시하고, */
		status = -ENODEV;
		/* [한국어] 컨트롤러 등록을 되돌리러 간다. */
		goto init_register_error;
	}
	/* [한국어] 그 브리지의 세컨더리 버스가 핫스왑 슬롯들이 매달린 곳이다. */
	bus0 = bus0_dev->subordinate;
	/* [한국어] 버스 포인터만 남기고 장치 참조는 놓는다. 브리지가 살아 있는 한 버스도
	 * 유지된다는 전제다. */
	pci_dev_put(bus0_dev);

	/* [한국어] 슬롯 범위를 코어에 알린다. 0x0a~0x0f 로 고정인데, 이 보드의 물리 슬롯
	 * 번호가 정해져 있기 때문이다 — cpcihp_generic.c 가 모듈 파라미터로 받는
	 * 것과 대비된다. */
	status = cpci_hp_register_bus(bus0, 0x0a, 0x0f);
	/* [한국어] 실패하면, */
	if (status != 0) {
		/* [한국어] 기록하고, */
		err("could not register cPCI hotplug bus");
		/* [한국어] 컨트롤러 등록을 되돌리러 간다. */
		goto init_register_error;
	}
	/* [한국어] 버스 등록 성공을 남긴다. */
	dbg("registered bus");

	/* [한국어] 폴링 스레드나 인터럽트 처리를 시작한다. */
	status = cpci_hp_start();
	/* [한국어] 실패하면, */
	if (status != 0) {
		/* [한국어] 기록하고, */
		err("could not started cPCI hotplug system");
		/* [한국어] 버스 등록을 여기서 직접 되돌린 뒤,
		 * [상류 코드 관찰] 아래 라벨이 버스 해제를 하지 않으므로 이 자리에서
		 *   직접 부른다. 라벨 하나로 통일하지 않은 탓에 되감기가 두 곳에 흩어진다. */
		cpci_hp_unregister_bus(bus0);
		/* [한국어] 컨트롤러 등록 되돌리기로 간다. */
		goto init_register_error;
	}
	/* [한국어] 시작 성공을 남긴다. */
	dbg("started cpci hp system");

	/* [한국어] 모든 준비가 끝났다. */
	return 0;
/* [한국어] 등록 실패 경로. */
init_register_error:
	/* [한국어] 컨트롤러 등록을 되돌린다. */
	cpci_hp_unregister_controller(&zt5550_hpc);
/* [한국어] 하드웨어 설정 실패 경로. 위에서 흘러내려 온다. */
init_hc_error:
	/* [한국어] 어느 단계에서 실패했는지 남기고, */
	err("status = %d", status);
	/* [한국어] 하드웨어를 정리한다. */
	zt5550_hc_cleanup();
	/* [한국어] 원래 오류를 올려보낸다. */
	return status;

}

/* [한국어]
 * zt5550_hc_remove_one - 등록한 것을 모두 되돌린다
 *
 * @pdev: 제거되는 장치. 쓰지 않는다 — 모든 상태가 전역에 있기 때문이다.
 *
 * zt5550_hc_init_one() 의 정확한 역순이다. 폴링·인터럽트 처리를 먼저 멈추고,
 * 버스와 컨트롤러 등록을 해제한 뒤, 하드웨어를 정리한다.
 *
 * 멈추기를 가장 먼저 하는 것이 중요하다. 아래에서 자료구조를 해제하는 동안
 * 폴링 스레드가 query_enum() 을 부르면 이미 해제된 것을 밟는다.
 *
 * 인자를 쓰지 않는다는 점이 이 드라이버의 전역 의존을 드러낸다. 정식 PCI
 * 드라이버 형태를 갖추고 있으면서도 상태는 모두 파일 전역에 있다.
 *
 * 실행 컨텍스트: PCI remove. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 각 해제 함수의 결과를 확인하지 않는다.
 *
 * 호출 체인:
 *   pci_unregister_driver() → PCI 코어 → [이 함수]
 *     → cpci_hp_stop() → cpci_hp_unregister_bus()
 *     → cpci_hp_unregister_controller() → zt5550_hc_cleanup()
 */
static void zt5550_hc_remove_one(struct pci_dev *pdev)
{
	/* [한국어] 먼저 폴링·인터럽트 처리를 멈춘다. 아래 해제 중에 콜백이 불리면 안 된다. */
	cpci_hp_stop();
	/* [한국어] 슬롯 등록을 해제하고, */
	cpci_hp_unregister_bus(bus0);
	/* [한국어] 컨트롤러 등록을 해제한 뒤, */
	cpci_hp_unregister_controller(&zt5550_hpc);
	/* [한국어] 하드웨어를 정리한다. init 의 정확한 역순이다. */
	zt5550_hc_cleanup();
}


static const struct pci_device_id zt5550_hc_pci_tbl[] = {
	/* [한국어] Ziatech ZT5550 호스트 컨트롤러의 벤더·장치 ID. 서브시스템은 아무거나. */
	{ PCI_VENDOR_ID_ZIATECH, PCI_DEVICE_ID_ZIATECH_5550_HC, PCI_ANY_ID, PCI_ANY_ID, },
	/* [한국어] 배열 끝. */
	{ 0, }
};
/* [한국어] 모듈 자동 로딩용 별칭을 만든다. */
MODULE_DEVICE_TABLE(pci, zt5550_hc_pci_tbl);

static struct pci_driver zt5550_hc_driver = {
	/* [한국어] sysfs 와 로그에 보일 이름. */
	.name		= "zt5550_hc",
	/* [한국어] 위 ID 표. */
	.id_table	= zt5550_hc_pci_tbl,
	/* [한국어] probe. */
	.probe		= zt5550_hc_init_one,
	/* [한국어] remove. cpcihp_generic.c 가 module_init 에서 직접 하드웨어를 잡는 것과
	 * 달리 이쪽은 정식 PCI 드라이버라, 장치가 발견될 때 커널이 불러 준다. */
	.remove		= zt5550_hc_remove_one,
};

/* [한국어]
 * zt5550_init - #ENUM 포트를 확보하고 PCI 드라이버를 등록한다
 *
 * @return: 0 = 성공, -EBUSY = 포트가 이미 쓰이는 중, 그 밖에 등록 오류.
 *
 * 모듈 진입점이다. 두 단계뿐이지만 순서에 이유가 있다.
 *
 * 포트를 **먼저** 확보한다. pci_register_driver() 를 부르는 순간 매치되는
 * 장치가 있으면 곧바로 probe 가 불리고, 그 안에서 query_enum 이 이 포트를
 * 읽을 수 있기 때문이다. 등록을 먼저 하면 확보되지 않은 포트를 읽는 창이 생긴다.
 *
 * 등록이 실패하면 확보했던 포트를 놓는다. 같은 디렉터리의
 * cpcihp_generic.c 가 실패 경로에서 이것을 빠뜨려 재삽입 시 -EBUSY 가 나는
 * 것과 대비된다 — 이쪽은 제대로 되돌린다.
 *
 * 실행 컨텍스트: 모듈 초기화. __init.
 *
 * 에러 경로: 두 실패 지점 모두 되돌리기가 갖춰져 있다.
 *
 * 호출 체인:
 *   module_init → [이 함수] → request_region() → pci_register_driver()
 */
static int __init zt5550_init(void)
{
	/* [한국어] 확보한 I/O 영역. */
	struct resource *r;
	/* [한국어] 결과. */
	int rc;

	/* [한국어] 버전과 함께 시작을 알린다. */
	info(DRIVER_DESC " version: " DRIVER_VERSION);
	/* [한국어] #ENUM 포트 한 바이트를 독점 확보한다. PCI 드라이버 등록보다 **먼저**
	 * 하는데, 등록하는 순간 probe 가 불려 그 포트를 읽을 수 있기 때문이다. */
	r = request_region(ENUM_PORT, 1, "#ENUM hotswap signal register");
	/* [한국어] 이미 누가 쓰고 있으면, */
	if (!r)
		return -EBUSY;

	/* [한국어] PCI 드라이버를 등록한다. 이 호출 안에서 매치되는 장치가 있으면
	 * 곧바로 probe 가 불린다. */
	rc = pci_register_driver(&zt5550_hc_driver);
	/* [한국어] 실패하면, */
	if (rc < 0)
		/* [한국어] 확보했던 포트를 놓는다. cpcihp_generic.c 가 실패 경로에서 이것을
		 * 빠뜨린 것과 대비된다 — 이쪽은 제대로 되돌린다. */
		release_region(ENUM_PORT, 1);
	/* [한국어] 결과를 돌려준다. */
	return rc;
}

/* [한국어]
 * zt5550_exit - 드라이버를 해제하고 포트를 놓는다
 *
 * 모듈 종료점이며 init 의 정확한 역순이다.
 *
 * pci_unregister_driver() 가 안에서 zt5550_hc_remove_one() 을 불러 하드웨어
 * 정리까지 끝내 준다. 그 뒤에 포트를 놓는 순서라, 정리 도중 query_enum 이
 * 불리더라도 포트가 아직 확보된 상태다.
 *
 * 실행 컨텍스트: 모듈 해제. __exit.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   module_exit → [이 함수] → pci_unregister_driver() → release_region()
 */
static void __exit
zt5550_exit(void)
{
	/* [한국어] 드라이버를 해제한다. 이 호출이 remove 를 부른다. */
	pci_unregister_driver(&zt5550_hc_driver);
	/* [한국어] 포트를 놓는다. init 의 역순이다. */
	release_region(ENUM_PORT, 1);
}

/* [한국어] 모듈 진입점. */
module_init(zt5550_init);
/* [한국어] 모듈 종료점. */
module_exit(zt5550_exit);

/* [한국어] 작성자. */
MODULE_AUTHOR(DRIVER_AUTHOR);
/* [한국어] 설명. */
MODULE_DESCRIPTION(DRIVER_DESC);
/* [한국어] 라이선스. */
MODULE_LICENSE("GPL");
/* [한국어] 실행 중에도 바꿀 수 있게 쓰기 권한을 준다. */
module_param(debug, bool, 0644);
/* [한국어] 설명 문자열. */
MODULE_PARM_DESC(debug, "Debugging mode enabled or not");
/* [한국어] 폴링 모드도 같은 권한이지만, */
module_param(poll, bool, 0644);
/* [한국어] 실제로는 probe 때 한 번만 읽히므로 실행 중에 바꿔도 효과가 없다. */
MODULE_PARM_DESC(poll, "#ENUM polling mode enabled or not");
