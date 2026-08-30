// SPDX-License-Identifier: GPL-2.0-only
/*
 * Tracepoints for PCI system
 *
 * Copyright (C) 2025 Alibaba Corporation
 */

#include <linux/pci.h> /* PCI/NVMe: NVMe PCIe 호스트 드라이버(drivers/nvme/host/pci.c)가 사용하는 pci_dev, pci_bus, pci_read/write_config_*, BAR/MSI-X/ASPM API를 선언; tracepoint 헤더가 이 구조체들을 TP_ARGS로 쓸 수 있도록 한다. */

#define CREATE_TRACE_POINTS /* PCI/NVMe: 이 컴파일 단위에서 TRACE_EVENT 매크로가 tracepoint 함수, probe, static key, 등록/해제 코드를 한 번만 생성; NVMe probe, DMA, MSI-X, AER, 핫플러그 경로에서 emit되는 tracepoint가 여기서 실체화된다. */
#include <trace/events/pci.h> /* PCI/NVMe: PCI core tracepoint 선언 (pci_cfg_read/write, power mgmt, ASPM 등); NVMe host가 pci_read/write_config_* 호출 시 방출되어 BAR, capability, PM 디버깅에 사용. */
#include <trace/events/pci_controller.h> /* PCI/NVMe: PCI host controller/PCIe root port tracepoint 선언 (link event, AER, hotplug, IOMMU, SR-IOV, virtualization); NVMe SSD 연결 root port의 link down, surprise removal, AER error 추적에 활용. */
