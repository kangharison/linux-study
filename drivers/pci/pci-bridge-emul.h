/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __PCI_BRIDGE_EMUL_H__
#define __PCI_BRIDGE_EMUL_H__

#include <linux/kernel.h>		/* PCI/NVMe: 기본 커널 타입/매크로 포함; NVMe PCIe 호스트가 bridge 레지스터 접근 시 사용 */

/* PCI configuration space of a PCI-to-PCI bridge. */
struct pci_bridge_emul_conf {
	__le16 vendor;			/* NVMe: bridge의 Vendor ID; NVMe SSD 열거 시 상위 bridge 식별에 사용 */
	__le16 device;			/* NVMe: bridge의 Device ID; NVMe 컨트롤러가 연결된 bridge 모델 확인 */
	__le16 command;			/* NVMe: bus master/memory space/io enable; NVMe DMA/BAR 접근 허용 플래그 */
	__le16 status;			/* NVMe: bridge 상태/에러 비트; AER(Advanced Error Reporting) 연관 상태 반영 */
	__le32 class_revision;		/* NVMe: bridge 클래스/리비전; NVMe 장치 class(0x010802)와 구분 */
	u8 cache_line_size;		/* NVMe: cache line 크기; NVMe DMA 성능/정렬 관련 */
	u8 latency_timer;		/* NVMe: PCI latency timer; 레거시 타이밍 설정 */
	u8 header_type;			/* NVMe: header type=1(bridge); NVMe 장치(type=0)와 구분 */
	u8 bist;			/* NVMe: BIST 레지스터; 일반적으로 0 */
	__le32 bar[2];			/* NVMe: bridge 자체 BAR; NVMe 컨트롤러 BAR(64-bit BAR0/1)와 별개 */
	u8 primary_bus;			/* NVMe: 상위 버스 번호; NVMe SSD가 속한 PCIe 계층의 루트 근처 버스 */
	u8 secondary_bus;		/* NVMe: 하위 버스 번호; NVMe SSD가 직접 연결될 수 있는 하위 버스 */
	u8 subordinate_bus;		/* NVMe: 최하위 버스 번호; bridge 뒤 NVMe 다중 장치 열거 범위 */
	u8 secondary_latency_timer;	/* NVMe: 하위 버스 latency timer */
	u8 iobase;			/* NVMe: IO 공간 하위 범위; NVMe(주로 MMIO)와 관련 낮음 */
	u8 iolimit;			/* NVMe: IO 공간 상위 범위; NVMe BAR는 메모리 매핑 사용 */
	__le16 secondary_status;	/* NVMe: 하위 버스 상태; 하위 NVMe 장치 에러 전파 시 갱신 */
	__le16 membase;			/* NVMe: 메모리 공간 하위 범위; NVMe BAR가 매핑될 32-bit 메모리 윈도우 */
	__le16 memlimit;		/* NVMe: 메모리 공간 상위 범위; NVMe BAR 매핑 범위의 상위 한계 */
	__le16 pref_mem_base;		/* NVMe: prefetchable 메모리 하위 범위; NVMe 64-bit BAR 매핑 윈도우 */
	__le16 pref_mem_limit;		/* NVMe: prefetchable 메모리 상위 범위; NVMe 64-bit BAR 매핑 상위 한계 */
	__le32 prefbaseupper;		/* NVMe: prefetchable 베이스 상위 32비트; 64-bit NVMe BAR 주소 윈도우 */
	__le32 preflimitupper;		/* NVMe: prefetchable 리미트 상위 32비트; 64-bit NVMe BAR 상위 주소 한계 */
	__le16 iobaseupper;		/* NVMe: IO 베이스 상위 16비트; NVMe MMIO와 직접 관련 없음 */
	__le16 iolimitupper;		/* NVMe: IO 리미트 상위 16비트; NVMe MMIO와 직접 관련 없음 */
	u8 capabilities_pointer;	/* NVMe: capability 연결 리스트 시작; MSI-X/PCIE/AER capability 탐색 시작점 */
	u8 reserve[3];			/* NVMe: 4바이트 정렬용 예약 필드 */
	__le32 romaddr;			/* NVMe: expansion ROM 주소; NVMe option ROM(드문 경우) */
	u8 intline;			/* NVMe: legacy INTx 라인; NVMe는 MSI-X 선호, 레거시 인터럽트 평가 시 참조 */
	u8 intpin;			/* NVMe: legacy INTx 핀; NVMe MSI-X 초기화 전 INTx 설정 가능 */
	__le16 bridgectrl;		/* NVMe: bridge 제어; SERR enable 등 NVMe AER/오류 처리와 연결 */
};

/* PCI configuration space of the PCIe capabilities */
struct pci_bridge_emul_pcie_conf {
	u8 cap_id;			/* NVMe: PCIe capability ID(0x10); NVMe 호스트가 capability 식별 */
	u8 next;			/* NVMe: 다음 capability 포인터; MSI-X/AER 등 다음 capability 탐색 */
	__le16 cap;			/* NVMe: PCIe capability 레지스터; capability 버전/포트 타입 등 */
	__le32 devcap;			/* NVMe: device capabilities; NVMe 컨트롤러 기능 규격(페이로드, ext tag 등) */
	__le16 devctl;			/* NVMe: device control; max payload size, relaxed ordering 등 NVMe 성능/정확성 제어 */
	__le16 devsta;			/* NVMe: device status; correctable/uncorrectable 에러 상태(AER 연동) */
	__le32 lnkcap;			/* NVMe: link capabilities; NVMe SSD 최대 link speed/width/ASPM 지원 */
	__le16 lnkctl;			/* NVMe: link control; ASPM L0s/L1 enable, link retrain; NVMe 전력/성능 관리 핵심 */
	__le16 lnksta;			/* NVMe: link status; 현재 NVMe 연결 speed/width/링크 에러 상태 */
	__le32 slotcap;			/* NVMe: slot capabilities; NVMe 핫플러그/Attention 버튼 지원 */
	__le16 slotctl;			/* NVMe: slot control; NVMe 핫플러그/PME enable 등 제어 */
	__le16 slotsta;			/* NVMe: slot status; 핫플러그 이벤트(프레젠스 변경) 상태 */
	__le16 rootctl;			/* NVMe: root control; PME/CRS 등 NVMe 전원 이벤트 제어 */
	__le16 rootcap;			/* NVMe: root capability; root port 기능 */
	__le32 rootsta;			/* NVMe: root status; PME 상태 등 NVMe 장치喚醒 이벤트 추적 */
	__le32 devcap2;			/* NVMe: device capabilities 2; CTOP, OBFF 등 추가 기능 */
	__le16 devctl2;			/* NVMe: device control 2; completion timeout 등 NVMe 동작 설정 */
	__le16 devsta2;			/* NVMe: device status 2; device 측 추가 상태 */
	__le32 lnkcap2;			/* NVMe: link capabilities 2; 지원 link speed 벡터(Gen3/4/5) */
	__le16 lnkctl2;			/* NVMe: link control 2; target link speed; NVMe Gen 전환 제어 */
	__le16 lnksta2;			/* NVMe: link status 2; 현재 link equalization/규격 상태 */
	__le32 slotcap2;		/* NVMe: slot capabilities 2; 추가 핫플러그/전원 기능 */
	__le16 slotctl2;		/* NVMe: slot control 2; slot 추가 제어 */
	__le16 slotsta2;		/* NVMe: slot status 2; slot 추가 상태 */
};

struct pci_bridge_emul;			/* NVMe: bridge 에뮬레이션 핸들 전방 선언; NVMe 호스트가 가상 bridge 조작 시 사용 */

typedef enum { PCI_BRIDGE_EMUL_HANDLED,	/* NVMe: 에뮬레이션 콜백이 read를 처리함; NVMe config read 경로 */
	       PCI_BRIDGE_EMUL_NOT_HANDLED } pci_bridge_emul_read_status_t;
					/* NVMe: 공통 에뮬레이션 코드가 메모리 복사본에서 읽어야 함 */

struct pci_bridge_emul_ops {
	/*
	 * Called when reading from the regular PCI bridge
	 * configuration space. Return PCI_BRIDGE_EMUL_HANDLED when the
	 * operation has handled the read operation and filled in the
	 * *value, or PCI_BRIDGE_EMUL_NOT_HANDLED when the read should
	 * be emulated by the common code by reading from the
	 * in-memory copy of the configuration space.
	 */
	pci_bridge_emul_read_status_t (*read_base)(struct pci_bridge_emul *bridge,
						   int reg, u32 *value);
					/* NVMe: bridge 기본 config space read 콜백; NVMe probe 시 bridge command/status/BAR 등 조회 경로 */

	/*
	 * Same as ->read_base(), except it is for reading from the
	 * PCIe capability configuration space.
	 */
	pci_bridge_emul_read_status_t (*read_pcie)(struct pci_bridge_emul *bridge,
						   int reg, u32 *value);
					/* NVMe: PCIe capability read 콜백; NVMe link status/ASPM/AER capability 읽기 경로 */

	/*
	 * Same as ->read_base(), except it is for reading from the
	 * PCIe extended capability configuration space.
	 */
	pci_bridge_emul_read_status_t (*read_ext)(struct pci_bridge_emul *bridge,
						  int reg, u32 *value);
					/* NVMe: PCIe extended capability read 콜백; NVMe AER/ACS/SR-IOV ext capability 읽기 경로 */

	/*
	 * Called when writing to the regular PCI bridge configuration
	 * space. old is the current value, new is the new value being
	 * written, and mask indicates which parts of the value are
	 * being changed.
	 */
	void (*write_base)(struct pci_bridge_emul *bridge, int reg,
			   u32 old, u32 new, u32 mask);
					/* NVMe: bridge 기본 config space write 콜백; NVMe DMA를 위한 bus master enable, BAR 할당/해제 반영 */

	/*
	 * Same as ->write_base(), except it is for writing from the
	 * PCIe capability configuration space.
	 */
	void (*write_pcie)(struct pci_bridge_emul *bridge, int reg,
			   u32 old, u32 new, u32 mask);
					/* NVMe: PCIe capability write 콜백; NVMe ASPM lnkctl 변경, link retrain 등 처리 */

	/*
	 * Same as ->write_base(), except it is for writing from the
	 * PCIe extended capability configuration space.
	 */
	void (*write_ext)(struct pci_bridge_emul *bridge, int reg,
			  u32 old, u32 new, u32 mask);
					/* NVMe: PCIe extended capability write 콜백; NVMe AER 마스크/심각도 레지스터 변경 처리 */
};

struct pci_bridge_reg_behavior;		/* NVMe: 레지스터별 RW/예약 동작 테이블 전방 선언; config 접근 보호 정책 */

struct pci_bridge_emul {
	struct pci_bridge_emul_conf conf;	/* NVMe: 에뮬레이션할 PCI bridge config space; NVMe 상위 bridge 속성 */
	struct pci_bridge_emul_pcie_conf pcie_conf;	/* NVMe: PCIe capability 공간; NVMe link/ASPM/AER 관련 */
	const struct pci_bridge_emul_ops *ops;	/* NVMe: read/write 콜백 테이블; NVMe config 접근을 에뮬/가상화 */
	struct pci_bridge_reg_behavior *pci_regs_behavior;	/* NVMe: PCI 레지스터 동작 배열; config read/write 필터링 */
	struct pci_bridge_reg_behavior *pcie_cap_regs_behavior;	/* NVMe: PCIe capability 동작 배열; capability 접근 필터링 */
	void *data;				/* NVMe: bridge 드라이버 private 데이터; NVMe 호스트가 context 저장 */
	u8 pcie_start;				/* NVMe: PCIe capability 오프셋; config 탐색 시 capability 시작점 */
	u8 ssid_start;				/* NVMe: subsystem ID capability 오프셋; NVMe bridge SSID 위치 */
	bool has_pcie;				/* NVMe: PCIe capability 존재 여부; NVMe Gen3/4/5 기능 노출 결정 */
	u16 subsystem_vendor_id;		/* NVMe: 서브시스템 vendor; NVMe bridge/플랫폼 식별 */
	u16 subsystem_id;			/* NVMe: 서브시스템 device; NVMe bridge/플랫폼 식별 */
};

enum {
	/*
	 * PCI bridge does not support forwarding of prefetchable memory
	 * requests between primary and secondary buses.
	 */
	PCI_BRIDGE_EMUL_NO_PREFMEM_FORWARD = BIT(0),
					/* NVMe: prefetchable 메모리 전달 불가; 64-bit NVMe BAR가 bridge 뒤로 매핑 불가능할 때 사용 */

	/*
	 * PCI bridge does not support forwarding of IO requests between
	 * primary and secondary buses.
	 */
	PCI_BRIDGE_EMUL_NO_IO_FORWARD = BIT(1),
					/* NVMe: IO 공간 전달 불가; NVMe MMIO BAR에는 영향 없음, 레거시 IO 제외 */
};

int pci_bridge_emul_init(struct pci_bridge_emul *bridge,
			 unsigned int flags);	/* NVMe: bridge 에뮬레이션 초기화; NVMe 상위 bridge 가상화 준비, flags로 윈도우 제한 */
void pci_bridge_emul_cleanup(struct pci_bridge_emul *bridge);
					/* NVMe: bridge 에뮬레이션 정리; NVMe unbind/핫플러그 시 리소스 해제 */

int pci_bridge_emul_conf_read(struct pci_bridge_emul *bridge, int where,
			      int size, u32 *value);
					/* NVMe: emulated config space read; NVMe 열거/probe 시 bridge 레지스터 읽기 */
int pci_bridge_emul_conf_write(struct pci_bridge_emul *bridge, int where,
			       int size, u32 value);
					/* NVMe: emulated config space write; NVMe BAR 할당/ASPM/전원 관리 레지스터 쓰기 */

#endif /* __PCI_BRIDGE_EMUL_H__ */
