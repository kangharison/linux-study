/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common library for PCI host controller drivers
 *
 * Copyright (C) 2014 ARM Limited
 *
 * Author: Will Deacon <will.deacon@arm.com>
 */

/*
 * [한국어 설명] ECAM 기반 호스트 컨트롤러 공용 라이브러리의 선언 (pci-host-common.h)
 *
 * === 파일의 역할 ===
 * ECAM(Enhanced Configuration Access Mechanism)만으로 설정공간에 접근하는
 * 호스트 컨트롤러 드라이버들이 공유하는 함수 원형을 선언한다. 구현은
 * pci-host-common.c 에 있고, 이 헤더에는 전방 선언 하나와 원형 네 개뿐이다.
 * 이런 라이브러리가 필요한 이유: ECAM 컨트롤러는 대부분 "DT 에서 창을 읽어
 * pci_ecam_create() 로 매핑하고 pci_host_probe() 를 부른다" 는 똑같은
 * 절차를 밟는다. 그 공통부를 한곳에 모아 두면 SoC 별 드라이버는 ops 표와
 * 매칭 표만 제공하면 된다 -- pci-host-generic.c 에 probe 함수가 아예 없는
 * 것이 그 결과다.
 * 진입 단계를 세 겹으로 나눈 것이 이 API 의 요점이다. 완전 자동인
 * pci_host_common_probe(), 브리지를 직접 만들고 넘기는
 * pci_host_common_init(), ECAM 창만 만드는 pci_host_common_ecam_create()
 * 순으로 통제 범위가 넓어진다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 컨트롤러 드라이버 계층의 공용 라이브러리 단이다. 위로는 SoC 별
 * 드라이버가 이 함수들을 부르고, 아래로는 PCI 코어의 pci_ecam_create() 와
 * pci_host_probe() 로 이어진다.
 * 이 트리의 소비자는 셋이다: pci-host-generic.c(가장 단순한 형태로
 * pci_host_common_probe 를 그대로 .probe 에 건다), pcie-apple.c(브리지를
 * 자기가 만들어 pci_host_common_init 을 부른다, :875),
 * pcie-qcom.c(ECAM 창만 필요해 pci_host_common_ecam_create 를 부른다, :1887).
 * 실행 컨텍스트는 없다 -- 선언만 있는 헤더다.
 *
 * === 타 모듈과의 연결 ===
 * 포함하는 헤더가 하나도 없다. 대신 struct pci_ecam_ops 를 **전방 선언**
 * 만으로 다룬다 -- 포인터로만 쓰이므로 완전한 정의가 필요 없고, 그 덕에
 * 이 헤더를 포함하는 쪽이 <linux/pci-ecam.h> 를 먼저 포함할 의무가 없어진다.
 * 다만 platform_device / pci_host_bridge / pci_config_window 는 전방 선언도
 * 없이 쓰이므로, 포함하는 쪽이 그 정의를 이미 갖고 있어야 한다.
 * 데이터 흐름: SoC 드라이버가 pci_ecam_ops 를 넘기면 라이브러리가 DT 의
 * 자원을 읽어 pci_config_window 를 만들고, 그것을 브리지의 sysdata 로 걸어
 * 열거를 시작한다.
 * drivers/nvme 는 이 헤더의 어떤 이름도 참조하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체 정의는 없다(pci_ecam_ops 는 전방 선언뿐). 함수 원형 넷:
 *  - pci_host_common_probe(): 브리지 할당부터 열거까지 전부 대신 해 준다.
 *    of_match 의 .data 에서 ops 를 꺼내 쓰므로, 드라이버는 표만 제공하면 된다.
 *  - pci_host_common_init(): 브리지를 이미 만들어 둔 드라이버용. ops 를
 *    인자로 직접 받는다.
 *  - pci_host_common_remove(): 위 둘의 짝. 버스를 걷어낸다.
 *  - pci_host_common_ecam_create(): ECAM 창(pci_config_window)만 만들어
 *    돌려준다. 열거는 호출자가 알아서 한다.
 */

#ifndef _PCI_HOST_COMMON_H
#define _PCI_HOST_COMMON_H

/* [한국어] struct pci_ecam_ops 의 **전방 선언**. 아래 원형들이 이 타입을
 * 포인터로만 쓰므로 완전한 정의가 필요 없다. 그 덕에 이 헤더를 포함하는
 * 쪽이 <linux/pci-ecam.h> 를 먼저 포함하지 않아도 컴파일된다 -- 헤더 간
 * 포함 순서 의존을 줄이는 흔한 기법이다. */
struct pci_ecam_ops;

/* [한국어] ECAM 호스트 컨트롤러의 프로브를 통째로 대신한다.
 * @pdev: 매칭된 플랫폼 디바이스.
 * @return: 0 성공, 음수 실패.
 * 브리지 할당 → DT 자원 파싱 → ECAM 창 생성 → 열거까지 모두 처리한다.
 * ops 는 of_device_get_match_data() 로 매칭 표의 .data 에서 꺼내므로,
 * 드라이버는 매칭 표만 제공하면 된다 -- pci-host-generic.c 가 이 함수를
 * .probe 에 그대로 걸어 자체 probe 함수가 아예 없는 이유다. */
int pci_host_common_probe(struct platform_device *pdev);
/* [한국어] 브리지를 이미 만들어 둔 드라이버용 진입점.
 * @pdev: 플랫폼 디바이스.
 * @bridge: 호출자가 할당한 호스트 브리지. 사설 데이터를 브리지 뒤에 함께
 *          잡아 두는 드라이버가 이 경로를 쓴다.
 * @ops: 쓸 ECAM 연산 표. probe 판과 달리 매칭 표에서 꺼내지 않고 인자로
 *       직접 받는다 -- 드라이버가 조건에 따라 다른 ops 를 고를 수 있다.
 * @return: 0 성공, 음수 실패.
 * 이 트리에서는 pcie-apple.c:875 가 쓴다. */
int pci_host_common_init(struct platform_device *pdev,
			 struct pci_host_bridge *bridge,
			 const struct pci_ecam_ops *ops);
/* [한국어] 위 두 진입점의 짝. 버스를 멈추고 걷어낸다.
 * @pdev: 제거되는 플랫폼 디바이스.
 * @return: 없음 -- 제거 경로라 실패를 전할 곳이 없다. */
void pci_host_common_remove(struct platform_device *pdev);

/* [한국어] ECAM 창만 만들어 돌려준다. 열거는 호출자가 알아서 한다.
 * @dev: 자원의 주인 장치.
 * @bridge: 윈도 목록을 가진 호스트 브리지. 여기서 버스 범위를 읽는다.
 * @ops: ECAM 연산 표.
 * @return: 만들어진 pci_config_window, 또는 ERR_PTR 오류.
 * 세 진입점 중 통제 범위가 가장 넓은 것이다 -- 설정공간 접근 방식만
 * 빌려 쓰고 나머지는 직접 하려는 드라이버를 위한 것으로,
 * pcie-qcom.c:1887 이 이 경로를 쓴다. pci_host_common_init() 자신도
 * 내부적으로 이 함수를 부른다(pci-host-common.c:67). */
struct pci_config_window *pci_host_common_ecam_create(struct device *dev,
	struct pci_host_bridge *bridge, const struct pci_ecam_ops *ops);
#endif
