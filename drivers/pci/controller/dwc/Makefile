# SPDX-License-Identifier: GPL-2.0
#
# ============================================================================
# [한국어 설명] Synopsys DesignWare PCIe 컨트롤러 빌드 (controller/dwc/Makefile)
#
# === 파일의 역할 ===
# Synopsys DesignWare(DWC) PCIe IP를 채용한 호스트/엔드포인트 드라이버를
# 빌드한다. DWC는 상용 SoC에서 가장 널리 쓰이는 PCIe IP라 이 디렉토리가 PCI
# 트리에서 가장 붐비는 곳이며, 공통 코어 5개 위에 30개 넘는 SoC 결합
# 드라이버가 얹히는 구조다. 각 벤더는 클럭/리셋/PHY/SerDes 배선만 다르고
# 링크 트레이닝과 iATU(주소 변환) 로직은 공통 코어가 담당한다.
#
# === 전체 아키텍처에서의 위치 ===
# controller/Makefile -> [이 파일]
# 계층: pcie-designware.o (공통 레지스터/iATU)
#         + pcie-designware-host.o (루트 콤플렉스) 또는 -ep.o (엔드포인트)
#         + <SoC>.o (전원/클럭/PHY 결합)
# NVMe 관점에서 이 계층은 "링크를 올리는 층"이다. iATU가 CPU 물리 주소를 PCI
# 버스 주소로 옮겨 주어야 NVMe BAR0 접근과 DMA가 성립한다.
#
# === 타 모듈과의 연결 ===
# - controller/dwc/Kconfig: 여기 모든 CONFIG 심볼의 정의처.
# - drivers/pci/probe.c: 이 드라이버가 등록한 host bridge에서 열거를 시작.
# - drivers/phy/: PHY 드라이버가 SerDes를 켜 줘야 링크 트레이닝이 가능하다.
#
# === 주요 함수/구조체 요약 ===
# - pcie-designware.o     : dw_pcie 코어. iATU 프로그래밍과 링크 상태 판정.
# - pcie-designware-host.o: dw_pcie_host_init. MSI 도메인과 config 접근 설정.
# - pcie-designware-ep.o  : dw_pcie_ep_init. 엔드포인트 BAR/헤더 노출.
# - 나머지                : SoC별 결합 드라이버(전원, 클럭, PHY, 리셋 순서).
# ============================================================================
obj-$(CONFIG_PCIE_DW) += pcie-designware.o	# [한국어] DWC 공통 코어. iATU 주소 변환 창과 링크 상태 판정 등 모든 파생이 공유하는 뼈대
obj-$(CONFIG_PCIE_DW_DEBUGFS) += pcie-designware-debugfs.o	# [한국어] debugfs 노출. 링크 속도/폭과 오류 카운터를 런타임에 들여다볼 때
obj-$(CONFIG_PCIE_DW_HOST) += pcie-designware-host.o	# [한국어] 루트 콤플렉스 모드. NVMe가 하위에 열거되려면 이쪽이 필요하다
obj-$(CONFIG_PCIE_DW_EP) += pcie-designware-ep.o	# [한국어] 엔드포인트 모드. 리눅스가 장치측으로 동작하는 반대 방향
obj-$(CONFIG_PCIE_DW_PLAT) += pcie-designware-plat.o	# [한국어] 범용 DT 결합. 벤더 고유 초기화가 없는 보드용 최소 래퍼
obj-$(CONFIG_PCIE_AMD_MDB) += pcie-amd-mdb.o	# [한국어] AMD MDB(Versal) 결합
obj-$(CONFIG_PCIE_ANDES_QILAI) += pcie-andes-qilai.o	# [한국어] Andes Qilai RISC-V SoC 결합
obj-$(CONFIG_PCIE_ESWIN) += pcie-eswin.o	# [한국어] ESWIN EIC7700 SoC 결합
obj-$(CONFIG_PCI_DRA7XX) += pci-dra7xx.o	# [한국어] TI DRA7xx 결합. 자동차용 SoC
obj-$(CONFIG_PCI_EXYNOS) += pci-exynos.o	# [한국어] Samsung Exynos 결합
obj-$(CONFIG_PCIE_FU740) += pcie-fu740.o	# [한국어] SiFive FU740(HiFive Unmatched) 결합. RISC-V 보드에서 NVMe를 올리는 대표 경로
obj-$(CONFIG_PCI_IMX6) += pci-imx6.o	# [한국어] NXP i.MX6/7/8 결합. 임베디드에서 NVMe를 붙일 때 흔히 쓰인다
obj-$(CONFIG_PCIE_NXP_S32G) += pcie-nxp-s32g.o	# [한국어] NXP S32G 차량용 네트워킹 SoC 결합
obj-$(CONFIG_PCIE_SPEAR13XX) += pcie-spear13xx.o	# [한국어] ST SPEAr13xx 결합
# ARM32 platforms use hook_fault_code() and cannot support loadable module.
obj-$(CONFIG_PCI_KEYSTONE) += pci-keystone.o	# [한국어] TI Keystone 결합(내장 전용). 위 주석대로 ARM32는 hook_fault_code() 때문에 모듈이 될 수 없다
# On non-ARM32 platforms, loadable module can be supported.
obj-$(CONFIG_PCI_KEYSTONE_TRISTATE) += pci-keystone.o	# [한국어] 같은 소스를 모듈로 빌드하는 경로. ARM32가 아닌 플랫폼에서만 선택 가능
obj-$(CONFIG_PCI_LAYERSCAPE) += pci-layerscape.o	# [한국어] NXP Layerscape 호스트 결합
obj-$(CONFIG_PCI_LAYERSCAPE_EP) += pci-layerscape-ep.o	# [한국어] NXP Layerscape 엔드포인트 결합
obj-$(CONFIG_PCIE_QCOM_COMMON) += pcie-qcom-common.o	# [한국어] Qualcomm 호스트/EP가 공유하는 PHY 및 클럭 시퀀스
obj-$(CONFIG_PCIE_QCOM) += pcie-qcom.o	# [한국어] Qualcomm 호스트 결합. 모바일/노트북 SoC에서 NVMe 경로
obj-$(CONFIG_PCIE_QCOM_EP) += pcie-qcom-ep.o	# [한국어] Qualcomm 엔드포인트 결합
obj-$(CONFIG_PCIE_ARMADA_8K) += pcie-armada8k.o	# [한국어] Marvell Armada 8K 결합
obj-$(CONFIG_PCIE_ARTPEC6) += pcie-artpec6.o	# [한국어] Axis ARTPEC-6 결합
obj-$(CONFIG_PCIE_ROCKCHIP_DW) += pcie-dw-rockchip.o	# [한국어] Rockchip의 DWC 기반 세대(RK3568/3588). 구형 자체 IP용 pcie-rockchip.c와는 별개다
obj-$(CONFIG_PCIE_SOPHGO_DW) += pcie-sophgo.o	# [한국어] Sophgo SG2044 결합
obj-$(CONFIG_PCIE_INTEL_GW) += pcie-intel-gw.o	# [한국어] Intel Gateway(LGM) SoC 결합
obj-$(CONFIG_PCIE_KEEMBAY) += pcie-keembay.o	# [한국어] Intel Keem Bay 결합
obj-$(CONFIG_PCIE_KIRIN) += pcie-kirin.o	# [한국어] HiSilicon Kirin 결합
obj-$(CONFIG_PCIE_HISI_STB) += pcie-histb.o	# [한국어] HiSilicon STB(셋톱박스) 결합
obj-$(CONFIG_PCI_MESON) += pci-meson.o	# [한국어] Amlogic Meson 결합
obj-$(CONFIG_PCIE_TEGRA194) += pcie-tegra194.o	# [한국어] NVIDIA Tegra194 결합. DT 기반 경로
obj-$(CONFIG_PCIE_UNIPHIER) += pcie-uniphier.o	# [한국어] Socionext UniPhier 호스트 결합
obj-$(CONFIG_PCIE_UNIPHIER_EP) += pcie-uniphier-ep.o	# [한국어] Socionext UniPhier 엔드포인트 결합
obj-$(CONFIG_PCIE_VISCONTI_HOST) += pcie-visconti.o	# [한국어] Toshiba Visconti5 결합
obj-$(CONFIG_PCIE_RCAR_GEN4) += pcie-rcar-gen4.o	# [한국어] Renesas R-Car Gen4 결합. Gen2/3의 자체 IP와 달리 Gen4는 DWC 기반이라 여기 있다
obj-$(CONFIG_PCIE_SPACEMIT_K1) += pcie-spacemit-k1.o	# [한국어] SpacemiT K1(RISC-V) 결합
obj-$(CONFIG_PCIE_STM32_HOST) += pcie-stm32.o	# [한국어] ST STM32MP25 호스트 결합
obj-$(CONFIG_PCIE_STM32_EP) += pcie-stm32-ep.o	# [한국어] ST STM32MP25 엔드포인트 결합

# The following drivers are for devices that use the generic ACPI
# pci_root.c driver but don't support standard ECAM config access.
# They contain MCFG quirks to replace the generic ECAM accessors with
# device-specific ones that are shared with the DT driver.

# The ACPI driver is generic and should not require driver-specific
# config options to be enabled, so we always build these drivers on
# ARM64 and use internal ifdefs to only build the pieces we need
# depending on whether ACPI, the DT driver, or both are enabled.

obj-$(CONFIG_PCIE_AL) += pcie-al.o	# [한국어] Amazon Annapurna Labs 결합. DT 경로용 일반 빌드
obj-$(CONFIG_PCI_HISI) += pcie-hisi.o	# [한국어] HiSilicon 서버 결합. DT 경로용 일반 빌드

ifdef CONFIG_ACPI	# [한국어] ACPI 부팅 경로에서만 아래 강제 빌드가 의미를 갖는다
ifdef CONFIG_PCI_QUIRKS	# [한국어] quirk 인프라가 있어야 MCFG 접근자 교체가 성립한다
obj-$(CONFIG_ARM64) += pcie-al.o	# [한국어] ACPI 경로용 강제 빌드. 위 주석대로 ACPI 드라이버는 벤더 CONFIG에 의존해선 안 되므로 ARM64에서 항상 넣고 내부 ifdef로 필요한 부분만 남긴다
obj-$(CONFIG_ARM64) += pcie-hisi.o	# [한국어] HiSilicon의 MCFG quirk를 ACPI 경로에 제공. 표준 ECAM으로 config에 접근할 수 없는 하드웨어 보정
obj-$(CONFIG_ARM64) += pcie-tegra194-acpi.o	# [한국어] Tegra194의 ACPI 전용 접근자. DT 드라이버(pcie-tegra194.o)와 quirk를 공유
endif	# [한국어] 위 ifdef 블록 종료
endif	# [한국어] 위 ifdef 블록 종료
