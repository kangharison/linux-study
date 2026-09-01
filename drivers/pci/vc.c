// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Virtual Channel support
 *
 * Copyright (C) 2013 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 */

/*
 * [한국어 설명] PCIe Virtual Channel 설정의 저장과 복원 (vc.c)
 *
 * === 파일의 역할 ===
 * PCIe 는 하나의 물리 링크 위에 여러 개의 논리 채널(Virtual Channel)을
 * 둘 수 있다. 채널마다 별도의 버퍼와 중재 규칙을 가지므로, 지연에 민감한
 * 트래픽과 대역폭만 필요한 트래픽을 섞이지 않게 나눌 수 있다.
 * 어떤 트래픽이 어느 채널로 갈지는 TC(Traffic Class) 번호로 정하고,
 * TC 와 VC 의 대응을 각 포트가 자기 레지스터에 들고 있다.
 *
 * 이 파일이 하는 일은 그 설정을 저장하고 복원하는 것뿐이다. 설정 자체를
 * 만들지는 않는다 — 그것은 펌웨어나 플랫폼이 부팅 시 정한다. 커널은
 * 전원이 끊기거나 리셋이 걸려 그 설정이 날아갔을 때 되돌려 놓는 역할만
 * 맡는다.
 *
 * 저장할 것이 세 종류다.
 *   VC Resource Control - 각 VC 의 활성 여부, TC/VC 매핑, 중재 방식.
 *   VC Arbitration Table - VC 들 사이의 중재 가중치 표.
 *   Port Arbitration Table - 한 VC 안에서 여러 포트의 중재 가중치 표.
 * 뒤의 두 표는 크기가 가변이고(VC 개수와 중재 방식에 따라 다르다),
 * 존재하지 않을 수도 있다. 그래서 크기를 먼저 계산해 버퍼를 잡는
 * pci_vc_do_save_buffer() 의 구조가 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 버퍼 준비: pci_save_state() 의 준비 단계
 *              -> [이 파일] pci_allocate_vc_save_buffers()
 *                 크기를 계산해 저장 버퍼를 미리 잡아 둔다. 저장 시점에
 *                 할당하면 실패할 수 있으므로 미리 하는 것이다.
 * 저장:      pci_save_state() -> [이 파일] pci_save_vc_state()
 * 복원:      pci_restore_state() -> [이 파일] pci_restore_vc_state()
 *
 * 세 함수 모두 pci_vc_do_save_buffer() 를 부르고, save 인자로 방향을
 * 가른다. 크기 계산과 저장과 복원이 같은 순회 로직을 공유해야 어긋나지
 * 않기 때문이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. config 접근이 있고, 중재 표를 다시
 * 로드한 뒤 하드웨어가 반영하기를 기다리는 구간이 있다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: pci.c 의 pci_save_state() / pci_restore_state() 만이 이 파일을 부른다.
 * 아래쪽: access.c 의 config 접근 함수.
 * 공유 상태: struct pci_dev 의 save_state 목록에 매달리는
 *   struct pci_cap_saved_data. VC capability 는 확장 capability 이고,
 *   VC / VC9 / MFVC 세 가지 ID 가 각각 따로 저장된다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 그리고 대부분의 NVMe SSD 는 VC capability 자체를 갖지 않는다 —
 * 엔드포인트에서 VC 를 여러 개 두는 것은 드물고, 보통 VC0 하나만 쓴다.
 * 그 경우 이 파일의 함수들은 capability 를 찾지 못해 곧바로 돌아간다.
 *
 * 의미가 있는 것은 NVMe 가 꽂힌 경로의 스위치나 루트 포트 쪽이다.
 * 여러 종류의 트래픽이 한 링크를 공유하는 환경에서 VC 설정이 복원되지
 * 않으면 중재 규칙이 기본값으로 돌아가, 의도했던 QoS 가 사라진다.
 *
 * (기존 주석은 호출 경로로 "nvme_reset_work() -> pci_reset_function()" 을
 *  적었으나, drivers/nvme/ 에 pci_reset_function() 호출은 0건이다.
 *  NVMe 컨트롤러 리셋은 NVMe 스펙의 CC.EN 절차로 직접 수행하고,
 *  PCI 리셋이 필요한 경우에도 pcie_reset_flr() 을 직접 부른다.
 *  또 "nvme_probe() -> pci_enable_device() -> pci_allocate_vc_save_buffers()"
 *  라고 적었으나 그 함수를 부르는 것은 pci_enable_device 가 아니라
 *  pci_save_state 의 준비 경로다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_vc_do_save_buffer() : 이 파일의 심장. save 인자에 따라 크기 계산,
 *                           저장, 복원 셋 중 하나를 수행한다. 세 동작이
 *                           같은 순회 코드를 공유해 서로 어긋나지 않는다.
 * pci_vc_save_restore_dwords() : dword 배열을 config space 와 버퍼 사이에
 *                           양방향으로 옮긴다. 중재 표 처리에 쓴다.
 * pci_vc_load_arb_table() : 중재 표를 하드웨어에 다시 로드하고, Table
 *                           Status 비트가 내려가기를 기다린다.
 * pci_vc_load_port_arb_table() : 위와 같되 포트 중재 표용.
 * pci_vc_enable()         : VC 를 활성화하고 Negotiation Pending 이
 *                           풀리기를 기다린다. 링크 양쪽이 합의해야 하므로
 *                           시간이 걸린다.
 * pci_save_vc_state()     : 저장 진입점.
 * pci_restore_vc_state()  : 복원 진입점.
 * pci_allocate_vc_save_buffers() : 저장 버퍼를 미리 잡아 둔다.
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>
#include <linux/types.h>

#include "pci.h"

/**
 * pci_vc_save_restore_dwords - Save or restore a series of dwords
 * @dev: device
 * @pos: starting config space position
 * @buf: buffer to save to or restore from
 * @dwords: number of dwords to save/restore
 * @save: whether to save or restore
 */
static void pci_vc_save_restore_dwords(struct pci_dev *dev, int pos,
				       u32 *buf, int dwords, bool save)
{
	int i;

	for (i = 0; i < dwords; i++, buf++) {
		if (save)
			pci_read_config_dword(dev, pos + (i * 4), buf);
		else
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
static void pci_vc_load_arb_table(struct pci_dev *dev, int pos)
{
	u16 ctrl;

	pci_read_config_word(dev, pos + PCI_VC_PORT_CTRL, &ctrl);
	pci_write_config_word(dev, pos + PCI_VC_PORT_CTRL,
			      ctrl | PCI_VC_PORT_CTRL_LOAD_TABLE);
	if (pci_wait_for_pending(dev, pos + PCI_VC_PORT_STATUS,
				 PCI_VC_PORT_STATUS_TABLE))
		return;

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
static void pci_vc_load_port_arb_table(struct pci_dev *dev, int pos, int res)
{
	int ctrl_pos, status_pos;
	u32 ctrl;

	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	pci_write_config_dword(dev, ctrl_pos,
			       ctrl | PCI_VC_RES_CTRL_LOAD_TABLE);

	if (pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_TABLE))
		return;

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
static void pci_vc_enable(struct pci_dev *dev, int pos, int res)
{
	int ctrl_pos, status_pos, id, pos2, evcc, i, ctrl_pos2, status_pos2;
	u32 ctrl, header, cap1, ctrl2;
	struct pci_dev *link = NULL;

	/* Enable VCs from the downstream device */
	if (!pci_is_pcie(dev) || !pcie_downstream_port(dev))
		return;

	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	id = ctrl & PCI_VC_RES_CTRL_ID;

	pci_read_config_dword(dev, pos, &header);

	/* If there is no opposite end of the link, skip to enable */
	if (PCI_EXT_CAP_ID(header) == PCI_EXT_CAP_ID_VC9 ||
	    pci_is_root_bus(dev->bus))
		goto enable;

	pos2 = pci_find_ext_capability(dev->bus->self, PCI_EXT_CAP_ID_VC);
	if (!pos2)
		goto enable;

	pci_read_config_dword(dev->bus->self, pos2 + PCI_VC_PORT_CAP1, &cap1);
	evcc = cap1 & PCI_VC_CAP1_EVCC;

	/* VC0 is hardwired enabled, so we can start with 1 */
	for (i = 1; i < evcc + 1; i++) {
		ctrl_pos2 = pos2 + PCI_VC_RES_CTRL +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		status_pos2 = pos2 + PCI_VC_RES_STATUS +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		pci_read_config_dword(dev->bus->self, ctrl_pos2, &ctrl2);
		if ((ctrl2 & PCI_VC_RES_CTRL_ID) == id) {
			link = dev->bus->self;
			break;
		}
	}

	if (!link)
		goto enable;

	/* Disable if enabled */
	if (ctrl2 & PCI_VC_RES_CTRL_ENABLE) {
		ctrl2 &= ~PCI_VC_RES_CTRL_ENABLE;
		pci_write_config_dword(link, ctrl_pos2, ctrl2);
	}

	/* Enable on both ends */
	ctrl2 |= PCI_VC_RES_CTRL_ENABLE;
	pci_write_config_dword(link, ctrl_pos2, ctrl2);
enable:
	ctrl |= PCI_VC_RES_CTRL_ENABLE;
	pci_write_config_dword(dev, ctrl_pos, ctrl);

	if (!pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_NEGO))
		pci_err(dev, "VC%d negotiation stuck pending\n", id);

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
static int pci_vc_do_save_buffer(struct pci_dev *dev, int pos,
				 struct pci_cap_saved_state *save_state,
				 bool save)
{
	u32 cap1;
	/* [한국어] Port VC Capability Register 1 에서 뽑아낼 세 값을 담는 지역 변수들.
	 * evcc: Extended VC Count — VC0 을 제외하고 이 포트가 추가로 지원하는 VC 개수.
	 *   아래에서 cap1 & PCI_VC_CAP1_EVCC 로 얻으며, 반복문의 상한이 되어
	 *   저장/복원할 VC Resource 레지스터 쌍의 개수를 결정한다.
	 * lpevcc: Low Priority Extended VC Count — 그중 저우선순위 VC 개수.
	 *   VC Arbitration Table 의 존재 여부와 크기를 판단하는 데 쓰인다.
	 * parb_size: Port Arbitration Table Entry Size(비트 단위).
	 *   레지스터에는 지수로 저장되어 있어 1 << 값 으로 실제 비트 수를 얻는다.
	 * 세 값 모두 char 로 선언되어 있지만 의미는 부호 없는 작은 정수다.
	 * (이 자리에 있던 영어 한 줄 주석은 상류 원본에 없던 것이라 걷어냈다.
	 *  원본의 영어 주석 세 줄은 각 값을 실제로 계산하는 곳에 그대로 남아 있다.) */
	char evcc, lpevcc, parb_size;
	int i, len = 0;
	u8 *buf = save_state ? (u8 *)save_state->cap.data : NULL;

	/* Sanity check buffer size for save/restore */
	if (buf && save_state->cap.size !=
	    pci_vc_do_save_buffer(dev, pos, NULL, save)) {
		pci_err(dev, "VC save buffer size does not match @0x%x\n", pos);
		return -ENOMEM;
	}

	pci_read_config_dword(dev, pos + PCI_VC_PORT_CAP1, &cap1);
	/* Extended VC Count (not counting VC0) */
	evcc = cap1 & PCI_VC_CAP1_EVCC;
	/* Low Priority Extended VC Count (not counting VC0) */
	lpevcc = FIELD_GET(PCI_VC_CAP1_LPEVCC, cap1);
	/* Port Arbitration Table Entry Size (bits) */
	parb_size = 1 << FIELD_GET(PCI_VC_CAP1_ARB_SIZE, cap1);

	/*
	 * Port VC Control Register contains VC Arbitration Select, which
	 * cannot be modified when more than one LPVC is in operation.  We
	 * therefore save/restore it first, as only VC0 should be enabled
	 * after device reset.
	 */
	if (buf) {
		if (save)
			pci_read_config_word(dev, pos + PCI_VC_PORT_CTRL,
					     (u16 *)buf);
		else
			pci_write_config_word(dev, pos + PCI_VC_PORT_CTRL,
					      *(u16 *)buf);
		buf += 4;
	}
	len += 4;

	/*
	 * If we have any Low Priority VCs and a VC Arbitration Table Offset
	 * in Port VC Capability Register 2 then save/restore it next.
	 */
	if (lpevcc) {
		u32 cap2;
		int vcarb_offset;

		pci_read_config_dword(dev, pos + PCI_VC_PORT_CAP2, &cap2);
		vcarb_offset = FIELD_GET(PCI_VC_CAP2_ARB_OFF, cap2) * 16;

		if (vcarb_offset) {
			int size, vcarb_phases = 0;

			if (cap2 & PCI_VC_CAP2_128_PHASE)
				vcarb_phases = 128;
			/* [한국어] 아래로 갈수록 작은 단계 수. 여러 비트가 동시에
			 * 서 있으면 가장 큰 것이 이긴다 — else if 사슬이 위에서부터
			 * 검사하므로 순서 자체가 우선순위다.
			 * 단계(phase) 수는 중재 표의 항목 개수를 결정하고,
			 * 그것이 곧 저장해야 할 버퍼 크기가 된다. */
			else if (cap2 & PCI_VC_CAP2_64_PHASE)
				vcarb_phases = 64;
			else if (cap2 & PCI_VC_CAP2_32_PHASE)
				vcarb_phases = 32;

			/* Fixed 4 bits per phase per lpevcc (plus VC0) */
			size = ((lpevcc + 1) * vcarb_phases * 4) / 8;

			if (size && buf) {
				pci_vc_save_restore_dwords(dev,
							   pos + vcarb_offset,
							   (u32 *)buf,
							   size / 4, save);
				/*
				 * On restore, we need to signal hardware to
				 * re-load the VC Arbitration Table.
				 */
				if (!save)
					pci_vc_load_arb_table(dev, pos);

				buf += size;
			}
			len += size;
		}
	}

	/*
	 * In addition to each VC Resource Control Register, we may have a
	 * Port Arbitration Table attached to each VC.  The Port Arbitration
	 * Table Offset in each VC Resource Capability Register tells us if
	 * it exists.  The entry size is global from the Port VC Capability
	 * Register1 above.  The number of phases is determined per VC.
	 */
	for (i = 0; i < evcc + 1; i++) {
		u32 cap;
		int parb_offset;

		pci_read_config_dword(dev, pos + PCI_VC_RES_CAP +
				      (i * PCI_CAP_VC_PER_VC_SIZEOF), &cap);
		parb_offset = FIELD_GET(PCI_VC_RES_CAP_ARB_OFF, cap) * 16;
		if (parb_offset) {
			int size, parb_phases = 0;

			if (cap & PCI_VC_RES_CAP_256_PHASE)
				parb_phases = 256;
			/* [한국어] 포트 중재 표의 단계 수. VC 중재와 달리 128 단계에
			 * 두 종류(일반과 Time-Based)가 있어 둘을 OR 로 함께 본다.
			 * 마찬가지로 큰 것부터 검사해 가장 큰 값이 이긴다. */
			else if (cap & (PCI_VC_RES_CAP_128_PHASE |
					PCI_VC_RES_CAP_128_PHASE_TB))
				parb_phases = 128;
			/* [한국어] 64단계. 128 이 없을 때만 여기 온다 */
			else if (cap & PCI_VC_RES_CAP_64_PHASE)
				parb_phases = 64;
			/* [한국어] 32단계. 가장 작은 선택지 */
			else if (cap & PCI_VC_RES_CAP_32_PHASE)
				parb_phases = 32;

			size = (parb_size * parb_phases) / 8;

			if (size && buf) {
				pci_vc_save_restore_dwords(dev,
							   pos + parb_offset,
							   (u32 *)buf,
							   size / 4, save);
				buf += size;
			}
			len += size;
		}

		/* VC Resource Control Register */
		if (buf) {
			int ctrl_pos = pos + PCI_VC_RES_CTRL +
					(i * PCI_CAP_VC_PER_VC_SIZEOF);
			if (save)
				pci_read_config_dword(dev, ctrl_pos,
						      (u32 *)buf);
			else {
				u32 tmp, ctrl = *(u32 *)buf;
				/*
				 * For an FLR case, the VC config may remain.
				 * Preserve enable bit, restore the rest.
				 */
				pci_read_config_dword(dev, ctrl_pos, &tmp);
				tmp &= PCI_VC_RES_CTRL_ENABLE;
				tmp |= ctrl & ~PCI_VC_RES_CTRL_ENABLE;
				pci_write_config_dword(dev, ctrl_pos, tmp);
				/* Load port arbitration table if used */
				if (ctrl & PCI_VC_RES_CTRL_ARB_SELECT)
					pci_vc_load_port_arb_table(dev, pos, i);
				/* Re-enable if needed */
				if ((ctrl ^ tmp) & PCI_VC_RES_CTRL_ENABLE)
					pci_vc_enable(dev, pos, i);
			}
			buf += 4;
		}
		len += 4;
	}

	return buf ? 0 : len;
}

static struct {
	u16 id;
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
int pci_save_vc_state(struct pci_dev *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		int pos, ret;
		struct pci_cap_saved_state *save_state;

		pos = pci_find_ext_capability(dev, vc_caps[i].id);
		if (!pos)
			continue;

		save_state = pci_find_saved_ext_cap(dev, vc_caps[i].id);
		if (!save_state) {
			pci_err(dev, "%s buffer not found in %s\n",
				vc_caps[i].name, __func__);
			return -ENOMEM;
		}

		ret = pci_vc_do_save_buffer(dev, pos, save_state, true);
		if (ret) {
			pci_err(dev, "%s save unsuccessful %s\n",
				vc_caps[i].name, __func__);
			return ret;
		}
	}

	return 0;
}

/**
 * pci_restore_vc_state - Restore VC state from save buffer
 * @dev: device
 *
 * For each type of VC capability, VC/VC9/MFVC, find the capability and
 * restore it from the previously saved buffer.
 */
void pci_restore_vc_state(struct pci_dev *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		int pos;
		struct pci_cap_saved_state *save_state;

		pos = pci_find_ext_capability(dev, vc_caps[i].id);
		save_state = pci_find_saved_ext_cap(dev, vc_caps[i].id);
		if (!save_state || !pos)
			continue;

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
void pci_allocate_vc_save_buffers(struct pci_dev *dev)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		int len, pos = pci_find_ext_capability(dev, vc_caps[i].id);

		if (!pos)
			continue;

		len = pci_vc_do_save_buffer(dev, pos, NULL, false);
		if (pci_add_ext_cap_save_buffer(dev, vc_caps[i].id, len))
			pci_err(dev, "unable to preallocate %s save buffer\n",
				vc_caps[i].name);
	}
}
