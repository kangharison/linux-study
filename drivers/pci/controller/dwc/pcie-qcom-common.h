/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

/*
 * [한국어 설명] 퀄컴 PCIe RC/EP 가 공유하는 두 함수의 선언 (pcie-qcom-common.h)
 *
 * === 파일의 역할 ===
 * 20줄짜리 헤더다. pcie-qcom-common.c 가 구현한 함수 둘을 선언하는 것이
 * 전부이며, 그 둘을 pcie-qcom.c(호스트)와 pcie-qcom-ep.c(엔드포인트)가
 * 함께 쓴다.
 *
 * 이렇게 작은 헤더가 따로 있는 이유는 링크의 물리 계층 설정이 방향과
 * 무관하기 때문이다. 이 컨트롤러가 호스트로 동작하든 엔드포인트로
 * 동작하든, 배선을 타고 가는 신호를 어떻게 보정할지는 같은 문제다.
 * 그래서 그 부분만 뽑아 공유한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * pcie-qcom.c (RC 모드) ─┐
 *                        ├─> [이 헤더] ─> pcie-qcom-common.c 의 구현
 * pcie-qcom-ep.c (EP 모드)┘                 ─> pcie-designware.c 의 DBI 접근
 *
 * 두 드라이버 모두 링크 트레이닝이 시작되기 직전에 이 함수들을 부른다.
 * probe 가 아니라 그 자리인 이유는, 이 설정이 트레이닝 과정에서 쓰이므로
 * 트레이닝이 시작될 때 이미 하드웨어에 들어가 있어야 하기 때문이다.
 * RC 는 qcom_pcie_start_link() 에서, EP 는 호스트가 PERST# 를 떼는
 * qcom_pcie_perst_deassert() 에서 부른다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더를 include 하는 곳: pcie-qcom.c, pcie-qcom-ep.c, 그리고
 *   구현 파일인 pcie-qcom-common.c 자신.
 * 이 헤더가 의존하는 것: struct dw_pcie 의 전방 선언뿐이다. 실제 정의는
 *   pcie-designware.h 에 있지만, 여기서는 포인터로만 쓰므로 전방 선언으로
 *   충분하고 그 덕에 헤더 의존이 가벼워진다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 헤더를 include 하지 않고 여기 선언된 함수도 부르지
 * 않는다(전수 확인). 관계는 간접적이다 — 퀄컴 플랫폼에서 NVMe 가 붙는
 * 링크의 신호 품질을 이 두 함수가 좌우한다.
 *
 * === 주요 함수/구조체 요약 ===
 * qcom_pcie_common_set_equalization()       : Gen3 이상에서 필요한 송신
 *                          왜곡 보정 preset 을 설정한다.
 * qcom_pcie_common_set_16gt_lane_margining(): Gen4 의 레인 마진 진단 기능을
 *                          켠다. 링크에 여유가 얼마나 있는지 잴 수 있다.
 * struct dw_pcie (전방 선언) : DesignWare 코어의 상태 구조체. 두 함수가
 *                          이것을 통해 DBI 레지스터에 접근한다.
 */

/* [한국어] 헤더 중복 포함 방지 가드. 이 헤더를 여러 .c 가 include 하고
 * 서로를 다시 include 할 수 있으므로, 두 번째부터는 내용을 건너뛰게 한다. */
#ifndef _PCIE_QCOM_COMMON_H
#define _PCIE_QCOM_COMMON_H

/* [한국어] struct dw_pcie 의 전방 선언.
 * 정의는 pcie-designware.h 에 있지만 여기서는 포인터로만 쓰므로 그 헤더를
 * include 할 필요가 없다. 헤더끼리의 의존을 줄이는 흔한 기법이다. */
struct dw_pcie;

/* [한국어]
 * qcom_pcie_common_set_equalization - 송신 이퀄라이제이션 preset 설정
 *
 * @pci: DesignWare 코어 상태. 이 안의 DBI 베이스로 레지스터에 쓴다.
 * @return: 없음. 실패해도 링크는 낮은 속도로나마 올라갈 수 있으므로
 *   호출자가 되돌릴 것이 없다.
 *
 * Gen3(8GT/s) 이상에서는 기판 배선이 신호를 뭉개므로, 송신 쪽이 미리
 * 반대 방향으로 왜곡을 넣어 보내야 수신 쪽에서 원래 파형에 가깝게 읽힌다.
 * 그 보정의 시작점이 되는 preset 값을 하드웨어에 넣는 것이 이 함수다.
 * 이후 링크 트레이닝 중에 양쪽이 실제로 주고받으며 값을 다듬는다.
 *
 * 호출 체인 (실제 호출 위치 확인):
 *   RC 모드: qcom_pcie_start_link() [pcie-qcom.c:324]
 *   EP 모드: qcom_pcie_perst_deassert() [pcie-qcom-ep.c:532]
 *     → [이 함수] → dw_pcie_write_dbi() [pcie-designware.c]
 *
 * 두 호출 위치가 probe 가 아니라는 점이 중요하다. 링크 트레이닝을
 * 시작하기 직전이어야 하기 때문이다. RC 는 링크를 올리기 시작하는
 * 그 자리에서, EP 는 호스트가 PERST# 를 떼어 링크가 시작될 때 설정한다.
 */
void qcom_pcie_common_set_equalization(struct dw_pcie *pci);

/* [한국어]
 * qcom_pcie_common_set_16gt_lane_margining - Gen4 레인 마진 기능 활성화
 *
 * @pci: DesignWare 코어 상태.
 * @return: 없음.
 *
 * 레인 마진(lane margining)은 진단 기능이다. 수신 쪽에서 판정 시점이나
 * 전압 기준을 일부러 조금씩 밀어 보며 어디까지 데이터를 제대로 받는지
 * 재는 것으로, 링크에 여유가 얼마나 있는지 알 수 있다.
 *
 * 16GT/s(Gen4)부터 규격에 들어온 기능이라 함수 이름에 16gt 가 붙었다.
 * 링크가 간헐적으로 불안정할 때, 배선 품질 문제인지 다른 원인인지
 * 좁히는 데 쓴다.
 *
 * 호출 체인 (실제 호출 위치 확인):
 *   RC 모드: qcom_pcie_start_link() [pcie-qcom.c:327]
 *   EP 모드: qcom_pcie_perst_deassert() [pcie-qcom-ep.c:535]
 *     → [이 함수] → dw_pcie_write_dbi()
 *
 * 바로 위 이퀄라이제이션 설정 직후에 불린다.
 */
void qcom_pcie_common_set_16gt_lane_margining(struct dw_pcie *pci);

/* [한국어] 위 include 가드의 끝. */
#endif
