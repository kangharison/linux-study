// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Virtual Channel support
 *
 * Copyright (C) 2013 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */

/*
 * [NVMe 관점 요약]
 * 이 파일은 PCI Express(PCIe) Virtual Channel(VC) 기능을 저장/복원하고
 * 선택적으로 활성화하는 PCI 핵심 서브시스템 코드다. NVMe SSD는 x4/x8/x16
 * 같은 PCIe 링크를 통해 호스트와 통신하며, PCIe 스위치/루트포트에 VC
 * (가상 채널) 기능이 있으면 이 파일이 해당 레지스터 상태를 다룬다.
 *
 * NVMe PCIe 호스트 드라이버(drivers/nvme/host/pci.c) 입장에서 볼 때
 * 이 코드는 직접 호출되지 않는다. 대신 PCI 코어가 nvme_pci_dev
 * (struct pci_dev)를 대상으로 suspend/resume/FLR(Function Level Reset)
 * 시 VC 상태를 저장하고 복원할 때 사용한다. NVMe 장치가 D3hot->D0
 * 전환, 런타임 복귀, 또는 시스템 Suspend/Resume 과정에서 VC 레지스터
 * (VC Arbitration Table, Port Arbitration Table, VC Resource Control)가
 * 올바르게 복원되지 않으면 PCIe QoS/아비트레이션 설정이 깨지고
 * DMA 지연/성능 저하/링크 오류가 발생할 수 있으므로 NVMe 입장에서도
 * 간접적으로 중요하다.
 *
 * 주요 호출 경로(NVMe 입장):
 *   nvme_probe() -> pci_enable_device() -> PCI core
 *        -> pci_allocate_vc_save_buffers()  (초기화 시 VC 버퍼 할당)
 *   nvme_suspend() / nvme_resume() -> PCI core
 *        -> pci_save_vc_state() / pci_restore_vc_state()
 *        -> pci_vc_do_save_buffer()         (VC 레지스터 save/restore)
 *   nvme_reset_work() -> pci_reset_function() / FLR
 *        -> pci_restore_vc_state()           (리셋 후 VC 상태 복원)
 */

/* Linux 커널 비트필드 헬퍼 포함: FIELD_GET 등 사용 */
#include <linux/bitfield.h>
/* 디바이스 모델(struct device) 관련 헤더 포함 */
#include <linux/device.h>
/* 커널 기본 매크로/함수 포함 */
#include <linux/kernel.h>
/* 모듈 관련 매크로 포함 */
#include <linux/module.h>
/* PCI 핵심 자료구조 및 함수 선언 포함 (struct pci_dev 등) */
#include <linux/pci.h>
/* PCIe 레지스터 오프셋/비트 정의 포함 (PCI_VC_* 등) */
#include <linux/pci_regs.h>
/* u8/u16/u32 등 고정폭 정수 타입 포함 */
#include <linux/types.h>

/* PCI 서브시스템 내부 헤더 (pci.h) 포함: pci_find_saved_ext_cap 등 */
#include "pci.h"

/**
 * pci_vc_save_restore_dwords - Save or restore a series of dwords
 * @dev: device
 * @pos: starting config space position
 * @buf: buffer to save to or restore from
 * @dwords: number of dwords to save/restore
 * @save: whether to save or restore
 */
/* NVMe: PCIe 설정 공간의 연속된 32비트 값을 저장(save=true) 하거나
 * 복원(save=false) 하는 보조 함수. NVMe 장치의 VC/Port Arbitration Table
 * 등 대용량 레지스터 블록을 save/restore 할 때 사용된다. */
static void pci_vc_save_restore_dwords(struct pci_dev *dev, int pos,
				       u32 *buf, int dwords, bool save)
{
	/* 반복 인덱스 변수 i 선언 */
	int i;

	/* dwords 개수만큼 반복하면서 buf 포인터도 함께 전진 */
	for (i = 0; i < dwords; i++, buf++) {
		/* save 플래그가 참이면 설정 공간에서 버퍼로 읽어옴 */
		if (save)
			/* pos + i*4 오프셋에서 32비트 읽기, NVMe 장치의
			 * VC Arbitration Table 일부를 저장 */
			pci_read_config_dword(dev, pos + (i * 4), buf);
		else
			/* save 플래그가 거짓이면 버퍼 값을 설정 공간에 씀,
			 * resume/FLR 후 NVMe 링크 파트너 측 VC 상태 복원 */
			pci_write_config_dword(dev, pos + (i * 4), *buf);
	}
}

/**
 * pci_vc_load_arb_table - load and wait for VC arbitration table
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 *
 * Set Load VC Arbitration Table bit requesting hardware to apply the VC
 * Arbitration Table (previously loaded).  When the VC Arbitration Table
 * Status clears, hardware has latched the table into VC arbitration logic.
 */
/* NVMe: VC Arbitration Table(여러 VC 간 트래픽 우선순위표)을 하드웨어에
 * 다시 로드하고 완료를 기다리는 함수. NVMe SSD가 Low Priority VC를 사용하는
 * 환경에서 resume 후 QoS 정책이 복원되는 데 필요하다. */
static void pci_vc_load_arb_table(struct pci_dev *dev, int pos)
{
	/* 16비트 Port VC Control 레지스터 값을 담을 변수 */
	u16 ctrl;

	/* 현재 Port VC Control 레지스터 값을 읽어옴 */
	pci_read_config_word(dev, pos + PCI_VC_PORT_CTRL, &ctrl);
	/* Load VC Arbitration Table 비트를 설정하여 하드웨어에 테이블
	 * 적용을 요청, NVMe 링크의 VC 스케줄링 정책 갱신 */
	pci_write_config_word(dev, pos + PCI_VC_PORT_CTRL,
			      ctrl | PCI_VC_PORT_CTRL_LOAD_TABLE);
	/* VC Arbitration Table Status 비트가 클리어될 때까지 대기,
	 * 하드웨어가 테이블을 래치하면 반환 */
	if (pci_wait_for_pending(dev, pos + PCI_VC_PORT_STATUS,
				 PCI_VC_PORT_STATUS_TABLE))
		return;

	/* 타임아웃 시 NVMe 장치라도 dmesg에 에러 기록 */
	pci_err(dev, "VC arbitration table failed to load\n");
}

/**
 * pci_vc_load_port_arb_table - Load and wait for VC port arbitration table
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 * @res: VC resource number, ie. VCn (0-7)
 *
 * Set Load Port Arbitration Table bit requesting hardware to apply the Port
 * Arbitration Table (previously loaded).  When the Port Arbitration Table
 * Status clears, hardware has latched the table into port arbitration logic.
 */
/* NVMe: 특정 VCn(0~7)에 속한 Port Arbitration Table을 하드웨어에 로드.
 * Port Arbitration Table은 같은 VC 내에서 여러 다운스트림 포트/장치
 * (NVMe 포함) 간 링크 대역폭 분배 규칙을 정의한다. */
static void pci_vc_load_port_arb_table(struct pci_dev *dev, int pos, int res)
{
	/* VC Resource Control/Status 오프셋 계산용 변수 */
	int ctrl_pos, status_pos;
	/* 32비트 VC Resource Control 값 저장 변수 */
	u32 ctrl;

	/* 해당 VCn 리소스의 Control 레지스터 오프셋 계산,
	 * NVMe 장치가 속한 VCn 위치 */
	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	/* 해당 VCn 리소스의 Status 레지스터 오프셋 계산 */
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	/* VC Resource Control 현재 값 읽기 */
	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	/* Load Port Arbitration Table 비트 설정 후 쓰기 */
	pci_write_config_dword(dev, ctrl_pos,
			       ctrl | PCI_VC_RES_CTRL_LOAD_TABLE);

	/* Port Arbitration Table Status 비트가 클리어될 때까지 대기 */
	if (pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_TABLE))
		return;

	/* 대기 실패 시 에러 로그, NVMe DMA latency에 영향 가능 */
	pci_err(dev, "VC%d port arbitration table failed to load\n", res);
}

/**
 * pci_vc_enable - Enable virtual channel
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 * @res: VC res number, ie. VCn (0-7)
 *
 * A VC is enabled by setting the enable bit in matching resource control
 * registers on both sides of a link.  We therefore need to find the opposite
 * end of the link.  To keep this simple we enable from the downstream device.
 * RC devices do not have an upstream device, nor does it seem that VC9 do
 * (spec is unclear).  Once we find the upstream device, match the VC ID to
 * get the correct resource, disable and enable on both ends.
 */
/* NVMe: 특정 VCn을 링크 양단에서 enable 한다. NVMe SSD가 다운스트림
 * 장치일 때 upstream 포트(스위치 업스트림/루트포트)의 동일 VC ID를 찾아
 * 양쪽을 enable해야 VC가 실제로 동작한다. */
static void pci_vc_enable(struct pci_dev *dev, int pos, int res)
{
	/* 여러 오프셋/인덱스/id/상대편 capability 위치 변수들 */
	int ctrl_pos, status_pos, id, pos2, evcc, i, ctrl_pos2, status_pos2;
	/* 32비트 Control/capability/header 값 변수들 */
	u32 ctrl, header, cap1, ctrl2;
	/* 링크 반대편(upstream) pci_dev 포인터, 초기 NULL */
	struct pci_dev *link = NULL;

	/* Enable VCs from the downstream device */
	/* NVMe: 다운스트림 포트가 아니면 VC enable 책임이 없으므로 종료.
	 * NVMe endpoint는 다운스트림이 아니라 보통 이 함수에서 빠른 return */
	if (!pci_is_pcie(dev) || !pcie_downstream_port(dev))
		return;

	/* 현재 포트의 VCn Control/Status 오프셋 계산 */
	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	/* 현재 VCn Resource Control 레지스터 읽기 */
	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	/* 하위 8비트에서 VC ID 추출, 상대편과 매칭할 때 사용 */
	id = ctrl & PCI_VC_RES_CTRL_ID;

	/* VC capability 헤더(Extended Capability ID/버전 등) 읽기 */
	pci_read_config_dword(dev, pos, &header);

	/* If there is no opposite end of the link, skip to enable */
	/* NVMe: VC9 확장 capability이거나 루트 버스에 붙은 장치면 upstream
	 * 이 없으므로 바로 enable 레이블로 점프 */
	if (PCI_EXT_CAP_ID(header) == PCI_EXT_CAP_ID_VC9 ||
	    pci_is_root_bus(dev->bus))
		goto enable;

	/* NVMe: 현재 다운스트림 포트의 상위 버스 컨트롤러(업스트림 포트)에서
	 * 일반 VC(PCI_EXT_CAP_ID_VC) 확장 capability 위치 검색 */
	pos2 = pci_find_ext_capability(dev->bus->self, PCI_EXT_CAP_ID_VC);
	/* 없으면 상대편과 매칭 불가, 현재 측만 enable 시도 */
	if (!pos2)
		goto enable;

	/* 상대편 포트의 Port VC Capability Register 1 읽기 */
	pci_read_config_dword(dev->bus->self, pos2 + PCI_VC_PORT_CAP1, &cap1);
	/* Extended VC Count 추출: VC0 제외 추가 VC 개수 */
	evcc = cap1 & PCI_VC_CAP1_EVCC;

	/* VC0 is hardwired enabled, so we can start with 1 */
	/* NVMe: VC0은 하드웨어 고정 enable이므로 1번 VC부터 상대편에서
	 * 같은 VC ID를 갖는 리소스를 찾음 */
	for (i = 1; i < evcc + 1; i++) {
		/* 상대편 VCi Control/Status 오프셋 계산 */
		ctrl_pos2 = pos2 + PCI_VC_RES_CTRL +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		status_pos2 = pos2 + PCI_VC_RES_STATUS +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		/* 상대편 VCi Control 값 읽기 */
		pci_read_config_dword(dev->bus->self, ctrl_pos2, &ctrl2);
		/* 상대편 VC ID가 현재 VC ID와 일치하면 link 포인터 저장 후 중단 */
		if ((ctrl2 & PCI_VC_RES_CTRL_ID) == id) {
			link = dev->bus->self;
			break;
		}
	}

	/* 일치하는 상대편 VC를 찾지 못하면 현재 측만 enable */
	if (!link)
		goto enable;

	/* Disable if enabled */
	/* NVMe: 상대편이 이미 enable되어 있으면 일단 disable 후 재enable,
	 * clean negotiation을 위해 필요 */
	if (ctrl2 & PCI_VC_RES_CTRL_ENABLE) {
		/* enable 비트 클리어 */
		ctrl2 &= ~PCI_VC_RES_CTRL_ENABLE;
		/* 상대편 Control 레지스터에 씀 */
		pci_write_config_dword(link, ctrl_pos2, ctrl2);
	}

	/* Enable on both ends */
	/* NVMe: 상대편 VC enable 비트 설정 후 쓰기 */
	ctrl2 |= PCI_VC_RES_CTRL_ENABLE;
	pci_write_config_dword(link, ctrl_pos2, ctrl2);
enable:
	/* NVMe: 현재 포트(다운스트림) VCn enable 비트 설정 후 쓰기 */
	ctrl |= PCI_VC_RES_CTRL_ENABLE;
	pci_write_config_dword(dev, ctrl_pos, ctrl);

	/* 현재 포트의 VC Negotiation Pending Status가 클리어될 때까지 대기,
	 * 하드웨어 간 VC 협상 완료 확인 */
	if (!pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_NEGO))
		pci_err(dev, "VC%d negotiation stuck pending\n", id);

	/* 상대편이 있으면 상대편 negotiation pending도 대기 */
	if (link && !pci_wait_for_pending(link, status_pos2,
					  PCI_VC_RES_STATUS_NEGO))
		pci_err(link, "VC%d negotiation stuck pending\n", id);
}

/**
 * pci_vc_do_save_buffer - Size, save, or restore VC state
 * @dev: device
 * @pos: starting position of VC capability (VC/VC9/MFVC)
 * @save_state: buffer for save/restore
 * @save: if provided a buffer, this indicates what to do with it
 *
 * Walking Virtual Channel config space to size, save, or restore it
 * is complicated, so we do it all from one function to reduce code and
 * guarantee ordering matches in the buffer.  When called with NULL
 * @save_state, return the size of the necessary save buffer.  When called
 * with a non-NULL @save_state, @save determines whether we save to the
 * buffer or restore from it.
 */
/* NVMe: VC capability 전체(Control, Arbitration Tables)를 한 번에
 * 크기 측정(NULL save_state)하거나 저장(true)/복원(false)하는 핵심 함수.
 * NVMe 장치의 suspend/resume/FLR 시 VC 상태 무결성을 책임진다. */
static int pci_vc_do_save_buffer(struct pci_dev *dev, int pos,
				 struct pci_cap_saved_state *save_state,
				 bool save)
{
	/* Port VC Capability 1 값 저장 */
	u32 cap1;
	/* Extended VC Count, Low Priority EVCC, Port Arbitration entry size */
	char evcc, lpevcc, parb_size;
	/* 반복 인덱스, 누적 버퍼 길이 */
	int i, len = 0;
	/* save_state가 있으면 cap.data를 u8 버퍼로 사용, 없으면 NULL */
	u8 *buf = save_state ? (u8 *)save_state->cap.data : NULL;

	/* Sanity check buffer size for save/restore */
	/* NVMe: 버퍼가 주어졌으면 미리 할당된 크기가 실제 필요 크기와
	 * 일치하는지 검증, 불일치 시 save/restore 진행 불가 */
	if (buf && save_state->cap.size !=
	    pci_vc_do_save_buffer(dev, pos, NULL, save)) {
		/* 크기 불일치 에러 로그 출력, NVMe 장치명과 capability 오프셋 포함 */
		pci_err(dev, "VC save buffer size does not match @0x%x\n", pos);
		/* 메모리 부족으로 간주하고 음수 반환 */
		return -ENOMEM;
	}

	/* Port VC Capability Register 1 읽기 */
	pci_read_config_dword(dev, pos + PCI_VC_PORT_CAP1, &cap1);
	/* Extended VC Count (not counting VC0) */
	/* NVMe: VC0 제외한 추가 VC 개수, 이 값+1이 전체 VC 개수 */
	evcc = cap1 & PCI_VC_CAP1_EVCC;
	/* Low Priority Extended VC Count (not counting VC0) */
	/* NVMe: Low Priority VC 개수, 0보다 크면 VC Arbitration Table 존재 가능 */
	lpevcc = FIELD_GET(PCI_VC_CAP1_LPEVCC, cap1);
	/* Port Arbitration Table Entry Size (bits) */
	/* NVMe: Port Arbitration Table의 한 phase당 비트 수,
	 * 1 << ARB_SIZE 로 계산 (예: 1, 2, 4, 8 비트 등) */
	parb_size = 1 << FIELD_GET(PCI_VC_CAP1_ARB_SIZE, cap1);

	/*
	 * Port VC Control Register contains VC Arbitration Select, which
	 * cannot be modified when more than one LPVC is in operation.  We
	 * therefore save/restore it first, as only VC0 should be enabled
	 * after device reset.
	 */
	/* NVMe: 버퍼가 있을 때 Port VC Control 레지스터를 먼저 다룬다.
	 * reset 직후 VC0만 enabled 상태이므로 안전하게 변경 가능하다. */
	if (buf) {
		/* save 모드면 설정 공간에서 버퍼로 16비트 읽어옴 */
		if (save)
			pci_read_config_word(dev, pos + PCI_VC_PORT_CTRL,
					     (u16 *)buf);
		else
			/* restore 모드면 버퍼 값을 설정 공간에 16비트 씀,
			 * NVMe 장치의 VC Arbitration Select 정책 복원 */
			pci_write_config_word(dev, pos + PCI_VC_PORT_CTRL,
					      *(u16 *)buf);
		/* 4바이트만큼 버퍼 포인터 전진 (32비트 정렬을 위해 4바이트 차지) */
		buf += 4;
	}
	/* Port VC Control 부분이 차지하는 버퍼 길이 4바이트 추가 */
	len += 4;

	/*
	 * If we have any Low Priority VCs and a VC Arbitration Table Offset
	 * in Port VC Capability Register 2 then save/restore it next.
	 */
	/* NVMe: Low Priority VC가 존재하면 VC Arbitration Table을 다음에 처리 */
	if (lpevcc) {
		/* Port VC Capability Register 2 값 */
		u32 cap2;
		/* VC Arbitration Table 오프셋 (바이트 단위) */
		int vcarb_offset;

		/* Port VC Capability Register 2 읽기 */
		pci_read_config_dword(dev, pos + PCI_VC_PORT_CAP2, &cap2);
		/* ARB_OFF 필드를 16바이트 단위로 환산해 오프셋 계산 */
		vcarb_offset = FIELD_GET(PCI_VC_CAP2_ARB_OFF, cap2) * 16;

		/* 오프셋이 0이 아니면 VC Arbitration Table이 존재 */
		if (vcarb_offset) {
			/* 테이블 크기, phase 수 변수 */
			int size, vcarb_phases = 0;

			/* 가능한 phase 수 중 하나를 비트 검사로 결정 */
			if (cap2 & PCI_VC_CAP2_128_PHASE)
				vcarb_phases = 128;
			else if (cap2 & PCI_VC_CAP2_64_PHASE)
				vcarb_phases = 64;
			else if (cap2 & PCI_VC_CAP2_32_PHASE)
				vcarb_phases = 32;

			/* Fixed 4 bits per phase per lpevcc (plus VC0) */
			/* NVMe: phase당 (lpevcc+1)개 VC에 각각 4비트 할당,
			 * 총 바이트 수 = 개수 * phase * 4 / 8 */
			size = ((lpevcc + 1) * vcarb_phases * 4) / 8;

			/* 크기가 있고 버퍼가 주어졌을 때만 save/restore 수행 */
			if (size && buf) {
				/* 연속된 dword 단위로 테이블 save/restore */
				pci_vc_save_restore_dwords(dev,
							   pos + vcarb_offset,
							   (u32 *)buf,
							   size / 4, save);
				/*
				 * On restore, we need to signal hardware to
				 * re-load the VC Arbitration Table.
				 */
				/* NVMe: 복원 시에는 하드웨어에 테이블 재적용 요청 */
				if (!save)
					pci_vc_load_arb_table(dev, pos);

				/* 버퍼를 테이블 크기만큼 전진 */
				buf += size;
			}
			/* 누적 버퍼 길이에 VC Arbitration Table 크기 추가 */
			len += size;
		}
	}

	/*
	 * In addition to each VC Resource Control Register, we may have a
	 * Port Arbitration Table attached to each VC.  The Port Arbitration Table
	 * Offset in each VC Resource Capability Register tells us if
	 * it exists.  The entry size is global from the Port VC Capability
	 * Register1 above.  The number of phases is determined per VC.
	 */
	/* NVMe: 각 VC(0부터 evcc까지)에 대해 Port Arbitration Table과
	 * VC Resource Control Register를 save/restore 한다. */
	for (i = 0; i < evcc + 1; i++) {
		/* VC Resource Capability 레지스터 값 */
		u32 cap;
		/* Port Arbitration Table 오프셋 */
		int parb_offset;

		/* VCi Resource Capability 레지스터 읽기 */
		pci_read_config_dword(dev, pos + PCI_VC_RES_CAP +
				      (i * PCI_CAP_VC_PER_VC_SIZEOF), &cap);
		/* Port Arbitration Table Offset 필드를 16바이트 단위로 변환 */
		parb_offset = FIELD_GET(PCI_VC_RES_CAP_ARB_OFF, cap) * 16;
		/* 오프셋이 0이 아니면 Port Arbitration Table 존재 */
		if (parb_offset) {
			/* 테이블 크기와 phase 수 변수 */
			int size, parb_phases = 0;

			/* Resource Capability 비트에 따라 phase 수 결정 */
			if (cap & PCI_VC_RES_CAP_256_PHASE)
				parb_phases = 256;
			else if (cap & (PCI_VC_RES_CAP_128_PHASE |
					PCI_VC_RES_CAP_128_PHASE_TB))
				parb_phases = 128;
			else if (cap & PCI_VC_RES_CAP_64_PHASE)
				parb_phases = 64;
			else if (cap & PCI_VC_RES_CAP_32_PHASE)
				parb_phases = 32;

			/* entry size * phase 수 / 8 로 테이블 바이트 크기 계산 */
			size = (parb_size * parb_phases) / 8;

			/* 크기와 버퍼가 유효하면 save/restore 실행 */
			if (size && buf) {
				/* Port Arbitration Table dword 단위 save/restore */
				pci_vc_save_restore_dwords(dev,
							   pos + parb_offset,
							   (u32 *)buf,
							   size / 4, save);
				/* 버퍼 포인터 전진 */
				buf += size;
			}
			/* 누적 길이에 Port Arbitration Table 크기 추가 */
			len += size;
		}

		/* VC Resource Control Register */
		/* NVMe: 각 VC의 Resource Control Register save/restore */
		if (buf) {
			/* VCi Resource Control 오프셋 계산 */
			int ctrl_pos = pos + PCI_VC_RES_CTRL +
					(i * PCI_CAP_VC_PER_VC_SIZEOF);
			/* save 모드면 Control 레지스터를 버퍼에 저장 */
			if (save)
				pci_read_config_dword(dev, ctrl_pos,
						      (u32 *)buf);
			else {
				/* restore 모드면 FLR 후에도 enable 비트는 보존 */
				u32 tmp, ctrl = *(u32 *)buf;
				/*
				 * For an FLR case, the VC config may remain.
				 * Preserve enable bit, restore the rest.
				 */
				/* 현재 enable 비트만 읽어옴 */
				pci_read_config_dword(dev, ctrl_pos, &tmp);
				/* enable 비트만 남기고 나머지는 0으로 마스크 */
				tmp &= PCI_VC_RES_CTRL_ENABLE;
				/* 저장된 값에서 enable 비트를 제외한 나머지 병합 */
				tmp |= ctrl & ~PCI_VC_RES_CTRL_ENABLE;
				/* 복원된 Control 레지스터 쓰기 */
				pci_write_config_dword(dev, ctrl_pos, tmp);
				/* Load port arbitration table if used */
				/* ARB_SELECT가 설정되어 있으면 Port Arbitration Table 로드 */
				if (ctrl & PCI_VC_RES_CTRL_ARB_SELECT)
					pci_vc_load_port_arb_table(dev, pos, i);
				/* Re-enable if needed */
				/* 저장된 enable 비트와 현재 enable 비트가 다륾면
				 * VC를 다시 enable하여 negotiation 수행 */
				if ((ctrl ^ tmp) & PCI_VC_RES_CTRL_ENABLE)
					pci_vc_enable(dev, pos, i);
			}
			/* Control Register 4바이트만큼 버퍼 전진 */
			buf += 4;
		}
		/* VC Resource Control Register가 차지하는 4바이트 추가 */
		len += 4;
	}

	/* NVMe: 버퍼가 NULL이면 필요한 크기(len) 반환, 아니면 성공 0 반환 */
	return buf ? 0 : len;
}

/* NVMe: 이 파일에서 다루는 세 가지 VC capability 종류를 정의하는 테이블 */
static struct {
	/* 확장 capability ID (MFVC/VC/VC9) */
	u16 id;
	/* 사람이 읽을 수 있는 이름 */
	const char *name;
} vc_caps[] = { { PCI_EXT_CAP_ID_MFVC, "MFVC" },
		{ PCI_EXT_CAP_ID_VC, "VC" },
		{ PCI_EXT_CAP_ID_VC9, "VC9" } };

/**
 * pci_save_vc_state - Save VC state to pre-allocate save buffer
 * @dev: device
 *
 * For each type of VC capability, VC/VC9/MFVC, find the capability and
 * save it to the pre-allocated save buffer.
 */
/* NVMe: NVMe 장치의 PCI dev에 대해 MFVC/VC/VC9 capability를 모두 찾아
 * 미리 할당된 save buffer에 현재 VC 상태를 저장한다. 시스템 suspend 직전
 * 또는 NVMe reset 전에 PCI core가 호출한다. */
int pci_save_vc_state(struct pci_dev *dev)
{
	/* vc_caps 테이블 인덱스 */
	int i;

	/* 세 종류의 VC capability를 순회 */
	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		/* capability 위치, 함수 반환값 */
		int pos, ret;
		/* 미리 할당된 save buffer 구조체 포인터 */
		struct pci_cap_saved_state *save_state;

		/* 현재 장치에서 해당 VC capability 위치 검색 */
		pos = pci_find_ext_capability(dev, vc_caps[i].id);
		/* capability가 없으면 다음 종류로 건다 */
		if (!pos)
			continue;

		/* 해당 capability에 대한 저장 버퍼 검색 */
		save_state = pci_find_saved_ext_cap(dev, vc_caps[i].id);
		/* 버퍼가 없으면 초기화 시 pci_allocate_vc_save_buffers()가
		 * 호출되지 않은 것이므로 에러 반환 */
		if (!save_state) {
			pci_err(dev, "%s buffer not found in %s\n",
				vc_caps[i].name, __func__);
			return -ENOMEM;
		}

		/* 실제 VC 상태를 save buffer에 저장 */
		ret = pci_vc_do_save_buffer(dev, pos, save_state, true);
		/* 저장 실패 시 에러 로그와 함께 반환 */
		if (ret) {
			pci_err(dev, "%s save unsuccessful %s\n",
				vc_caps[i].name, __func__);
			return ret;
		}
	}

	/* 모든 VC capability 저장 성공 */
	return 0;
}

/**
 * pci_restore_vc_state - Restore VC state from save buffer
 * @dev: device
 *
 * For each type of VC capability, VC/VC9/MFVC, find the capability and
 * restore it from the previously saved buffer.
 */
/* NVMe: NVMe 장치 resume/reset 후 PCI core가 호출하여 저장했던 VC 상태를
 * PCIe 설정 공간에 복원한다. 복원 중 Port Arbitration Table과 VC enable
 * negotiation도 다시 수행된다. */
void pci_restore_vc_state(struct pci_dev *dev)
{
	/* vc_caps 테이블 인덱스 */
	int i;

	/* 세 종류 VC capability를 순회하며 복원 */
	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		/* capability 위치 */
		int pos;
		/* 저장 버퍼 구조체 포인터 */
		struct pci_cap_saved_state *save_state;

		/* 해당 VC capability 위치 검색 */
		pos = pci_find_ext_capability(dev, vc_caps[i].id);
		/* 저장 버퍼 검색 */
		save_state = pci_find_saved_ext_cap(dev, vc_caps[i].id);
		/* 둘 중 하나라도 없으면 복원 건다 */
		if (!save_state || !pos)
			continue;

		/* VC 상태 복원 수행 (save=false) */
		pci_vc_do_save_buffer(dev, pos, save_state, false);
	}
}

/**
 * pci_allocate_vc_save_buffers - Allocate save buffers for VC caps
 * @dev: device
 *
 * For each type of VC capability, VC/VC9/MFVC, find the capability, size
 * it, and allocate a buffer for save/restore.
 */
/* NVMe: NVMe 장치 초기화(pci_enable_device 메모리 등) 시 PCI core가
 * 호출하여 VC save/restore에 필요한 버퍼를 미리 할당한다. 이 버퍼는
 * suspend/resume/FLR 시 VC 레지스터를 안전하게 보존하는 데 쓰인다. */
void pci_allocate_vc_save_buffers(struct pci_dev *dev)
{
	/* vc_caps 테이블 인덱스 */
	int i;

	/* 세 종류 VC capability에 대해 반복 */
	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		/* 필요 버퍼 길이, capability 위치를 한 줄에 선언 및 초기화 */
		int len, pos = pci_find_ext_capability(dev, vc_caps[i].id);

		/* capability가 없으면 할당할 필요 없음 */
		if (!pos)
			continue;

		/* NULL 버퍼로 크기만 측정 */
		len = pci_vc_do_save_buffer(dev, pos, NULL, false);
		/* 측정한 크기로 확장 capability 저장 버퍼 할당 시도 */
		if (pci_add_ext_cap_save_buffer(dev, vc_caps[i].id, len))
			/* 할당 실패 시 에러 로그, NVMe 장치에서도 dmesg 확인 가능 */
			pci_err(dev, "unable to preallocate %s save buffer\n",
				vc_caps[i].name);
	}
}
