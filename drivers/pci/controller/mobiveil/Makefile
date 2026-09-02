# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] Mobiveil 계열 PCIe 컨트롤러 빌드 (controller/mobiveil/Makefile)
#
# === 파일의 역할 ===
# Mobiveil GPEX IP 기반 호스트 브리지를 빌드한다. 공통 레지스터 접근 계층
# (pcie-mobiveil.o)과 루트 콤플렉스 동작(host), 그리고 플랫폼 결합
# (plat, layerscape-gen4)을 세 층으로 나눠 둔 것이 특징이다.
#
# === 전체 아키텍처에서의 위치 ===
# controller/Makefile -> [이 파일]
# NXP Layerscape 계열 네트워킹 SoC에서 NVMe를 붙일 때 이 경로가 링크를
# 담당한다. 링크가 올라오기 전에는 drivers/pci/probe.c가 아무 장치도
# 발견하지 못하므로, NVMe 열거의 선행 조건이다.
#
# === 타 모듈과의 연결 ===
# - controller/mobiveil/Kconfig: 네 CONFIG 심볼 정의처.
# - drivers/pci/setup-bus.c: 여기서 신고한 버스 범위 안에서 BAR를 배치한다.
#
# === 주요 함수/구조체 요약 ===
# - pcie-mobiveil.o          : GPEX 레지스터 접근과 아웃바운드/인바운드 윈도우 공통 코드.
# - pcie-mobiveil-host.o     : 루트 콤플렉스 초기화와 INTx/MSI 처리.
# - pcie-mobiveil-plat.o     : 범용 DT 바인딩 래퍼.
# - pcie-layerscape-gen4.o   : NXP Layerscape Gen4 전용 결합.
# ============================================================================
obj-$(CONFIG_PCIE_MOBIVEIL) += pcie-mobiveil.o	# [한국어] GPEX 공통 계층. 윈도우 프로그래밍과 레지스터 접근 헬퍼
obj-$(CONFIG_PCIE_MOBIVEIL_HOST) += pcie-mobiveil-host.o	# [한국어] 루트 콤플렉스 동작. 링크 업 대기와 인터럽트 도메인 생성
obj-$(CONFIG_PCIE_MOBIVEIL_PLAT) += pcie-mobiveil-plat.o	# [한국어] 범용 device tree 바인딩. 벤더 고유 처리가 없는 보드용
obj-$(CONFIG_PCIE_LAYERSCAPE_GEN4) += pcie-layerscape-gen4.o	# [한국어] NXP Layerscape Gen4 결합. 해당 SoC에서 NVMe가 올라오는 실제 경로
