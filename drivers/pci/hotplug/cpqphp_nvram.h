/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Compaq Hot Plug Controller Driver
 *
 * Copyright (C) 1995,2001 Compaq Computer Corporation
 * Copyright (C) 2001 Greg Kroah-Hartman (greg@kroah.com)
 *
 * All rights reserved.
 *
 * Send feedback to <greg@kroah.com>
 *
 */

/*
 * [한국어 설명] Compaq 핫플러그 컨트롤러의 NVRAM 인터페이스 선언 (cpqphp_nvram.h)
 *
 * === 파일의 역할 ===
 * Compaq(현 HPE) 서버의 PCI 핫플러그 컨트롤러가 슬롯 설정을 NVRAM 에
 * 저장하고 불러오는 함수 세 개를 선언한다. 구조체도 매크로도 없다.
 * 이 헤더의 진짜 역할은 선언이 아니라 **선택 기능의 부재를 감추는 것**이다.
 * CONFIG_HOTPLUG_PCI_COMPAQ_NVRAM 이 꺼져 있으면 세 함수를 아무 일도 하지
 * 않는 static inline 으로 바꿔치기해, 호출부가 #ifdef 없이 그대로 컴파일되게
 * 한다. Makefile 이 그 옵션에 따라 cpqphp_nvram.o 를 빼고 넣는 구조라
 * (Makefile:44), 링크 오류 대신 빈 함수를 쓰는 이 방식이 필요하다.
 * NVRAM 이 하는 일은 부팅 사이에 슬롯 구성을 기억하는 것이다. 시스템
 * 펌웨어(ROM)가 남긴 설정을 읽어 컨트롤러 상태를 복원하고, 종료 시 현재
 * 상태를 되쓴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 핫플러그 계층의 Compaq 전용 구현 중 한 조각이다. 호출 관계는
 * 이렇다: cpqphp_core.c 가 초기화 때 compaq_nvram_init() 을(:723),
 * 종료 때 compaq_nvram_store() 를(:1257) 부르고, cpqphp_pci.c 가 슬롯
 * 구성을 읽을 때 compaq_nvram_load() 를 부른다(:1216).
 * 구현은 cpqphp_nvram.c 에 있다(:412, :421, :636).
 * 실행 컨텍스트는 없다 -- 선언과 빈 인라인 함수만 있는 헤더다.
 *
 * === 타 모듈과의 연결 ===
 * 포함하는 헤더가 하나도 없다. 대신 인자 타입으로 struct controller 를
 * 쓰므로, 이 헤더를 포함하는 쪽이 cpqphp.h 를 먼저 포함해 그 정의를
 * 갖고 있어야 한다 -- 전방 선언조차 없어 포함 순서에 의존한다.
 * 데이터 흐름은 ROM 시작 주소(void __iomem *rom_start)를 통해 흐른다.
 * 호출자가 매핑해 둔 시스템 ROM 창을 넘기면, 구현이 그 안의 NVRAM 영역을
 * 찾아 읽고 쓴다.
 * drivers/nvme 는 이 헤더의 어떤 이름도 참조하지 않는다. 이름의 "NVRAM" 은
 * 비휘발성 RAM 이고 NVMe 와는 무관하다.
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체는 없고 함수 셋뿐이며, 각각 두 벌(빈 인라인 / 실제 선언)로 존재한다.
 *  - compaq_nvram_init(rom_start): NVRAM 영역을 찾아 내부 상태를 준비한다.
 *    반환값이 없어 실패를 알리지 않는다.
 *  - compaq_nvram_load(rom_start, ctrl): 저장된 슬롯 구성을 컨트롤러에
 *    불러온다. 기능이 꺼져 있을 때의 빈 판은 **0(성공)** 을 돌려주므로,
 *    호출자는 '저장된 구성이 없었다' 와 구별하지 못한다.
 *  - compaq_nvram_store(rom_start): 현재 구성을 NVRAM 에 되쓴다. 빈 판은
 *    역시 0 이다.
 */

#ifndef _CPQPHP_NVRAM_H
#define _CPQPHP_NVRAM_H

/* [한국어] 여기서부터 #else 까지가 **기능이 꺼졌을 때**의 빈 구현이다.
 * Makefile:44 가 이 옵션에 따라 cpqphp_nvram.o 를 빌드에서 빼므로,
 * 빈 인라인 함수를 두지 않으면 호출부가 링크 오류를 낸다. 호출부에
 * #ifdef 를 흩뿌리는 대신 이 헤더 한곳에서 흡수하는 흔한 기법이다. */
#ifndef CONFIG_HOTPLUG_PCI_COMPAQ_NVRAM

/* [한국어] 기능이 꺼졌을 때의 compaq_nvram_init -- 아무 일도 하지 않는다.
 * @rom_start: 무시된다.
 * @return: 없음.
 * 본문이 비어 있어 컴파일러가 호출 자체를 없앤다. */
static inline void compaq_nvram_init(void __iomem *rom_start) { }

/* [한국어] 기능이 꺼졌을 때의 compaq_nvram_load.
 * @rom_start, @ctrl: 무시된다.
 * @return: **항상 0(성공)**.
 * 0 을 돌려주는 것이 요점이다 -- 호출자(cpqphp_pci.c:1216)가 실패로 보고
 * 초기화를 접지 않게 하려는 것이다. 다만 그 때문에 '저장된 구성을 실제로
 * 불러왔다' 와 '기능이 꺼져 있어 아무것도 안 했다' 를 호출자가 구별할 수
 * 없다. */
static inline int compaq_nvram_load(void __iomem *rom_start, struct controller *ctrl)
{
	return 0;
}

/* [한국어] 기능이 꺼졌을 때의 compaq_nvram_store.
 * @rom_start: 무시된다.
 * @return: 항상 0(성공).
 * load 와 같은 이유로 성공을 가장한다. */
static inline int compaq_nvram_store(void __iomem *rom_start)
{
	return 0;
}

#else

/* [한국어] 여기서부터가 **기능이 켜졌을 때**의 진짜 선언이다.
 * 구현은 cpqphp_nvram.c 에 있다(:412, :421, :636).
 *
 * compaq_nvram_init - 시스템 ROM 안의 NVRAM 영역을 찾아 내부 상태를 준비한다.
 * @rom_start: 호출자가 매핑해 둔 시스템 ROM 창의 시작 주소.
 * @return: 없음 -- **실패를 알리지 않는다.** 영역을 못 찾아도 조용히
 *          넘어가고, 이후 load 가 저장된 구성을 찾지 못하는 형태로 드러난다.
 * 호출자: cpqphp_core.c:723 (드라이버 초기화). */
void compaq_nvram_init(void __iomem *rom_start);
/* [한국어] compaq_nvram_load - 저장된 슬롯 구성을 컨트롤러에 불러온다.
 * @rom_start: 시스템 ROM 창의 시작 주소.
 * @ctrl: 구성을 채워 넣을 컨트롤러. 이 타입의 정의는 이 헤더에 없으므로
 *        포함하는 쪽이 cpqphp.h 를 먼저 포함해야 한다.
 * @return: 0 성공, 음수 실패.
 * 호출자: cpqphp_pci.c:1216. 부팅 사이에 유지된 슬롯 구성을 복원해,
 * 핫플러그로 바꿔 둔 설정이 재부팅 후에도 남게 한다. */
int compaq_nvram_load(void __iomem *rom_start, struct controller *ctrl);
/* [한국어] compaq_nvram_store - 현재 구성을 NVRAM 에 되쓴다.
 * @rom_start: 시스템 ROM 창의 시작 주소.
 * @return: 0 성공, 음수 실패.
 * 호출자: cpqphp_core.c:1257 (드라이버 종료). load 의 대칭이며, 이 쓰기가
 * 있어야 다음 부팅의 load 가 의미를 갖는다. */
int compaq_nvram_store(void __iomem *rom_start);

/* [한국어] CONFIG_HOTPLUG_PCI_COMPAQ_NVRAM 분기 끝. */
#endif

/* [한국어] _CPQPHP_NVRAM_H 포함 보호 끝. */
#endif

