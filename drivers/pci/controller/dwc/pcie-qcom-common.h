/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */ /* PCI/NVMe: Qualcomm DWC PCIe 컨트롤러 공통 헤더;
      *      NVMe PCIe host(pci.c)가 루트 컴플렉스 초기화/링크 튜닝 시
      *      사용하는 물리/데이터링크 계층 인터페이스를 정의함 */

#ifndef _PCIE_QCOM_COMMON_H /* NVMe: 헤더 중복 포함 방지 */
#define _PCIE_QCOM_COMMON_H /* PCI/NVMe: Qcom DWC PCIe 공통 API 가드 */

struct dw_pcie; /* NVMe: DesignWare PCIe 코어 구조체 전방 선언;
                 *      NVMe 장치가 연결된 루트 컴플렉스의 공통 상태/레지스터 컨테이너 */

void qcom_pcie_common_set_equalization(struct dw_pcie *pci); /* PCI/NVMe: PCIe 이퀄라이제이션 설정;
                                                              *      Gen3/Gen4/Gen5 NVMe SSD 열거 전 링크 품질/속도 협상에 영향 */

void qcom_pcie_common_set_16gt_lane_margining(struct dw_pcie *pci); /* PCI/NVMe: 16GT/s(Gen4) 레인 마진링 설정;
                                                                     *      고속 NVMe SSD의 신호 무결성 평가 및 ASPM/성능 안정성에 기여 */

#endif /* PCI/NVMe: Qcom 공통 헤더 가드 종료 */
