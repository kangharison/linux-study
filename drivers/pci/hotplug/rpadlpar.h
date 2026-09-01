/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Interface for Dynamic Logical Partitioning of I/O Slots on
 * RPA-compliant PPC64 platform.
 *
 * John Rose <johnrose@austin.ibm.com>
 * October 2003
 *
 * Copyright (C) 2003 IBM.
 */

/*
 * [한국어 설명] RPA DLPAR I/O 슬롯 인터페이스 선언 (rpadlpar.h)
 *
 * === 파일의 역할 ===
 * IBM RPA(RS/6000 Platform Architecture) 규격을 따르는 PPC64 시스템에서
 * I/O 슬롯을 동적 논리 분할(DLPAR, Dynamic Logical Partitioning)로 붙였다
 * 떼는 인터페이스를 선언한 헤더다. 함수 원형 네 개가 전부이고, 구조체도
 * 매크로도 실행 코드도 없다.
 * DLPAR 은 일반 PCI 핫플러그와 목적이 다르다. 물리적으로 카드를 꽂고 빼는
 * 것이 아니라, **하이퍼바이저가 논리 파티션들 사이에서 I/O 슬롯의 소유권을
 * 옮기는** 것이다. 그래서 슬롯을 가리키는 이름도 버스:장치 번호가 아니라
 * 펌웨어가 부여한 DRC(Dynamic Reconfiguration Connector) 이름 문자열이다.
 * 선언과 구현을 나눈 이유는 이 인터페이스의 소비자가 둘이기 때문이다 --
 * 구현은 rpadlpar_core.c 에, 사용자 공간 통로(sysfs)는
 * rpadlpar_sysfs.c 에 있어 두 파일이 이 헤더를 공유한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 핫플러그 계층의 플랫폼별 구현 중 하나다. 위로는 sysfs 를 통해
 * 관리자나 하이퍼바이저 도구가 슬롯 추가/제거를 지시하고, 아래로는 RTAS
 * (Run-Time Abstraction Services)를 거쳐 펌웨어와 대화한다.
 * 같은 디렉터리의 rpaphp_* 파일들이 '이미 꽂힌 슬롯의 핫플러그' 를 다루는
 * 반면, 이 rpadlpar_* 쪽은 '슬롯 자체를 파티션에 넣고 빼는' 상위 동작을
 * 맡는다. 실행 컨텍스트는 없다 -- 선언만 있는 헤더다.
 *
 * === 타 모듈과의 연결 ===
 * 포함하는 헤더가 하나도 없다는 점이 특징이다. 선언에 쓰이는 타입이
 * int / void / char * 뿐이라 다른 헤더가 필요 없다.
 * 데이터 흐름: 사용자가 sysfs 에 DRC 이름을 쓰면 rpadlpar_sysfs.c 가 그
 * 문자열을 받아 dlpar_add_slot() 또는 dlpar_remove_slot() 에 넘기고,
 * rpadlpar_core.c 가 그 이름으로 장치 트리 노드를 찾아 PCI 열거나 제거를
 * 진행한다. 즉 이 헤더는 문자열 하나를 주고받는 좁은 접점이다.
 * drivers/nvme 는 이 헤더의 어떤 이름도 참조하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * 구조체는 없고 함수 원형 네 개뿐이다.
 *  - dlpar_sysfs_init() / dlpar_sysfs_exit(): sysfs 항목을 만들고 없앤다.
 *    모듈 적재·제거 시 한 번씩 불린다.
 *  - dlpar_add_slot(char *drc_name): 그 이름의 슬롯을 이 파티션에 추가한다.
 *  - dlpar_remove_slot(char *drc_name): 반대로 떼어 낸다.
 * 네 함수 모두 이 헤더에는 선언만 있고 구현은 rpadlpar_core.c 에 있다.
 */

#ifndef _RPADLPAR_IO_H_
#define _RPADLPAR_IO_H_

/* [한국어] sysfs 인터페이스를 만든다.
 * @return: 0 성공, 음수 실패.
 * 모듈 적재 시 rpadlpar_core.c 가 부르며, 이것이 성공해야 사용자 공간이
 * DRC 이름을 써 넣을 통로가 생긴다. 구현은 rpadlpar_sysfs.c 에 있다. */
int dlpar_sysfs_init(void);

/* [한국어] 위에서 만든 sysfs 인터페이스를 걷어낸다.
 * @return: 없음.
 * 모듈 제거 시 불린다. 반환값이 없는 것은 되감기 경로라 실패를 전할 곳이
 * 없기 때문이다. */
void dlpar_sysfs_exit(void);

/* [한국어] DRC 이름으로 지정한 I/O 슬롯을 이 논리 파티션에 추가한다.
 * @drc_name: 펌웨어가 부여한 Dynamic Reconfiguration Connector 이름.
 *            버스:장치 번호가 아니라 문자열인 것이 DLPAR 의 특징이다 --
 *            슬롯의 소유권을 파티션 사이에서 옮기는 동작이라, 물리 위치가
 *            아니라 펌웨어가 아는 논리 이름으로 지목해야 한다.
 * @return: 0 성공, 음수는 이름을 찾지 못했거나 추가에 실패.
 * 구현은 rpadlpar_core.c 에 있고, 그쪽이 장치 트리에서 노드를 찾아 PCI
 * 열거를 진행한다. 호출자는 rpadlpar_sysfs.c 의 sysfs 쓰기 경로다. */
int dlpar_add_slot(char *drc_name);

/* [한국어] 그 슬롯을 이 파티션에서 떼어 낸다.
 * @drc_name: 위와 같은 DRC 이름.
 * @return: 0 성공, 음수 실패.
 * add 의 대칭이며, PCI 장치를 제거한 뒤 슬롯 소유권을 펌웨어에 되돌린다. */
int dlpar_remove_slot(char *drc_name);

#endif
