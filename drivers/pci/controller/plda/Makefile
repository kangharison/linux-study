# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] PLDA 계열 PCIe 호스트 컨트롤러 빌드 (controller/plda/Makefile)
#
# === 파일의 역할 ===
# PLDA(현 Rambus) XpressRICH IP를 기반으로 하는 호스트 브리지 드라이버들을
# 빌드한다. 공통 로직을 pcie-plda-host.o에 모으고, 각 SoC 벤더가 그 위에
# 자사 클럭/리셋/인터럽트 배선만 덧입히는 구조다.
#
# === 전체 아키텍처에서의 위치 ===
# controller/Makefile -> [이 파일] -> plda 공통 + Microchip/StarFive 파생
# 이 계층이 링크를 올려야 하위에 꽂힌 NVMe가 비로소 열거된다. RISC-V 개발
# 보드(StarFive VisionFive 등)에서 NVMe를 쓰는 경로가 정확히 여기다.
#
# === 타 모듈과의 연결 ===
# - drivers/pci/controller/plda/Kconfig: 세 CONFIG 심볼의 정의처.
# - drivers/pci/probe.c: 이 드라이버가 등록한 pci_host_bridge를 받아 버스를 연다.
# - drivers/pci/ecam.c: config space 접근이 ECAM 규약을 따를 때 함께 쓰인다.
#
# === 주요 함수/구조체 요약 ===
# - pcie-plda-host.o    : PLDA 공통 호스트 로직(윈도우 설정, INTx/MSI 도메인).
# - pcie-microchip-host.o: PolarFire SoC용 파생.
# - pcie-starfive.o     : StarFive JH7110용 파생.
# ============================================================================
obj-$(CONFIG_PCIE_PLDA_HOST) += pcie-plda-host.o	# [한국어] PLDA 공통 호스트 계층. 아웃바운드 윈도우와 인터럽트 도메인 등 파생 드라이버가 공유하는 뼈대
obj-$(CONFIG_PCIE_MICROCHIP_HOST) += pcie-microchip-host.o	# [한국어] Microchip PolarFire SoC 루트 포트. FPGA SoC에 붙은 NVMe의 진입 경로
obj-$(CONFIG_PCIE_STARFIVE_HOST) += pcie-starfive.o	# [한국어] StarFive JH7110 루트 포트. RISC-V 보드에서 M.2 NVMe를 올릴 때 필요
