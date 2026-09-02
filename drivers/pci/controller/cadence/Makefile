# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] Cadence PCIe 컨트롤러 빌드 (controller/cadence/Makefile)
#
# === 파일의 역할 ===
# Cadence PCIe IP를 쓰는 호스트/엔드포인트 드라이버를 빌드한다. 공통 코어를
# 세 개의 합성 모듈(-mod)로 묶고, 각 SoC 결합 드라이버가 그 위에 얹히는
# 구조다. HPA(High Performance Architecture) 변형 레지스터 맵을 같은 모듈
# 안에 함께 넣어 런타임에 분기한다.
#
# === 전체 아키텍처에서의 위치 ===
# controller/Makefile -> [이 파일]
# TI J721E 같은 자동차/산업용 SoC에서 NVMe를 붙이는 경로가 여기다. 이 계층이
# 링크를 올린 뒤에야 drivers/pci/probe.c가 하위 장치를 열거한다.
#
# === 타 모듈과의 연결 ===
# - controller/cadence/Kconfig: 여기 CONFIG 심볼 정의처.
# - drivers/pci/endpoint/: EP 모드일 때 pcie-cadence-ep가 EPC로 등록된다.
#
# === 주요 함수/구조체 요약 ===
# - pcie-cadence.o          : 공통 레지스터 접근과 윈도우 설정.
# - pcie-cadence-hpa.o      : HPA 레지스터 맵 변형 처리.
# - pcie-cadence-host*.o    : 루트 콤플렉스 동작(공통/일반/HPA).
# - pcie-cadence-ep.o       : 엔드포인트 모드 구현.
# ============================================================================
pcie-cadence-mod-y := pcie-cadence-hpa.o pcie-cadence.o	# [한국어] 공통 코어 합성 모듈. 구형 맵과 HPA 맵을 한 모듈에 담아 런타임 분기
pcie-cadence-host-mod-y := pcie-cadence-host-common.o pcie-cadence-host.o pcie-cadence-host-hpa.o	# [한국어] 루트 콤플렉스 합성 모듈. 공통 로직 + 일반/HPA 두 변형
pcie-cadence-ep-mod-y := pcie-cadence-ep.o	# [한국어] 엔드포인트 모드 합성 모듈

obj-$(CONFIG_PCIE_CADENCE) = pcie-cadence-mod.o	# [한국어] 코어 등록. `=`(대입)이라 앞선 값을 덮어쓰는데, 이 변수의 첫 지정이라 결과는 `+=`와 같다
obj-$(CONFIG_PCIE_CADENCE_HOST) += pcie-cadence-host-mod.o	# [한국어] 호스트 모드. NVMe가 하위에 열거되려면 이쪽이 필요하다
obj-$(CONFIG_PCIE_CADENCE_EP) += pcie-cadence-ep-mod.o	# [한국어] 엔드포인트 모드. 리눅스가 장치측으로 동작하는 반대 방향
obj-$(CONFIG_PCIE_CADENCE_PLAT) += pcie-cadence-plat.o	# [한국어] 범용 DT 결합. 벤더 고유 초기화가 없는 보드용
obj-$(CONFIG_PCI_J721E) += pci-j721e.o	# [한국어] TI J721E/AM64 결합. SerDes와 리셋 배선이 SoC 고유라 별도 드라이버가 필요
obj-$(CONFIG_PCIE_SG2042_HOST) += pcie-sg2042.o	# [한국어] Sophgo SG2042(RISC-V 64코어) 호스트 결합
obj-$(CONFIG_PCI_SKY1_HOST) += pci-sky1.o	# [한국어] Cixin Sky1 호스트 결합
