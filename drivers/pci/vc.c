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
/* [한국어]
 * pci_vc_save_restore_dwords - 연속된 dword 들을 버퍼와 config 사이에 오간다
 *
 * @dev: 대상 장치.
 * @pos: config 공간의 시작 오프셋.
 * @buf: 저장할 버퍼 또는 복원할 값이 든 버퍼.
 * @dwords: 옮길 dword 개수.
 * @save: true = config → 버퍼, false = 버퍼 → config.
 *
 * VC 상태 저장·복원의 가장 낮은 벽돌이다. 방향만 다르고 나머지가 같은 두
 * 루프를 하나로 합친 것으로, save 플래그가 그 방향을 정한다.
 *
 * 이 파일 전체가 그 방식을 쓴다 — pci_vc_do_save_buffer() 도 같은 플래그로
 * 저장과 복원을 겸한다. 저장과 복원이 **정확히 같은 순서로 같은 자리** 를
 * 훑어야 하므로, 두 함수로 나누면 한쪽만 고쳐 어긋날 위험이 생긴다.
 *
 * 옮기는 대상은 중재 표(arbitration table)다. 그 크기가 장치마다 달라
 * 개수를 인자로 받는다.
 *
 * 실행 컨텍스트: 절전 진입·복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. config 접근 실패를 확인하지 않는다.
 *
 * 호출 체인:
 *   pci_vc_do_save_buffer() → [이 함수]
 *     → pci_read_config_dword() / pci_write_config_dword()
 */
static void pci_vc_save_restore_dwords(struct pci_dev *dev, int pos,
				       u32 *buf, int dwords, bool save)
{
	int i;

	/* [한국어] dword 단위로 순회한다. buf 도 함께 전진하므로 인덱스와 포인터가 나란히 움직인다. */
	for (i = 0; i < dwords; i++, buf++) {
		/* [한국어] 저장 방향이면, */
		if (save)
			/* [한국어] config 에서 읽어 버퍼에 담는다. i * 4 는 dword 인덱스를 바이트 오프셋으로
			 * 바꾸는 것이다. */
			pci_read_config_dword(dev, pos + (i * 4), buf);
		else
			/* [한국어] 복원 방향이면 버퍼의 값을 config 에 쓴다. 두 방향이 같은 루프를 쓰므로
			 * 저장과 복원의 순서가 어긋날 수 없다 — 이 파일 전체를 관통하는 설계다. */
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
/* [한국어]
 * pci_vc_load_arb_table - VC 중재 표를 하드웨어에 적재시킨다
 *
 * @dev: 대상 장치.
 * @pos: VC capability 의 위치.
 *
 * 중재 표를 config 공간에 써 넣는 것만으로는 반영되지 않는다. Load VC
 * Arbitration Table 비트를 세워 "이제 읽어 가라" 고 알려야 하고, 하드웨어가
 * 다 읽으면 그 비트가 스스로 내려간다.
 *
 * 그래서 세우고 기다리는 두 단계다. 기다림이 필요한 이유는 적재가 끝나기
 * 전에 다음 설정을 하면 표가 반쯤 반영된 상태가 될 수 있기 때문이다.
 *
 * 읽기-수정-쓰기로 그 비트만 세우는 것도 요점이다. 같은 레지스터의 다른
 * 필드가 VC 중재 방식을 정하고 있어 통째로 쓸 수 없다.
 *
 * 시간이 다 되면 오류 기록만 남기고 돌아간다. 복원 경로에서 불리는 함수라
 * 여기서 실패해도 되돌릴 곳이 없다 — 남길 수 있는 것이 기록뿐이다.
 *
 * 실행 컨텍스트: 절전 복귀. 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 반환값이 없어 실패를 알리지 못하고 로그만 남는다.
 *
 * 호출 체인:
 *   pci_vc_enable() → [이 함수] → pci_wait_for_pending()
 */
static void pci_vc_load_arb_table(struct pci_dev *dev, int pos)
{
	/* [한국어] Port VC Control 레지스터 값. */
	u16 ctrl;

	/* [한국어] 현재 값을 읽는다. 다른 비트를 보존해야 하므로 읽기-수정-쓰기다. */
	pci_read_config_word(dev, pos + PCI_VC_PORT_CTRL, &ctrl);
	/* [한국어] Load VC Arbitration Table 비트를 세워 되쓴다. 이 쓰기가 하드웨어에
	 * "방금 써 둔 표를 실제로 적용하라" 고 지시하는 방아쇠다. */
	pci_write_config_word(dev, pos + PCI_VC_PORT_CTRL,
			      ctrl | PCI_VC_PORT_CTRL_LOAD_TABLE);
	/* [한국어] Status 의 Table 비트가 내려갈 때까지 기다린다. 위 영어 주석대로 그 비트가
	 * 내려갔다는 것은 하드웨어가 표를 중재 로직에 걸어 넣었다는 뜻이다. */
	if (pci_wait_for_pending(dev, pos + PCI_VC_PORT_STATUS,
				 PCI_VC_PORT_STATUS_TABLE))
		return;

	/* [한국어] 시간 안에 내려가지 않으면 기록만 남긴다. 반환값이 없어 호출자에게 알릴
	 * 방법이 없고, 복원 중이라 중단할 수도 없기 때문이다. */
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
/* [한국어]
 * pci_vc_load_port_arb_table - 한 VC 자원의 포트 중재 표를 적재시킨다
 *
 * @dev: 대상 장치.
 * @pos: VC capability 의 위치.
 * @res: VC 자원 번호.
 *
 * pci_vc_load_arb_table() 과 구조가 같고 대상이 다르다. 그쪽은 VC 들 사이의
 * 중재를, 이쪽은 한 VC 안에서 포트들 사이의 중재를 다룬다.
 *
 * 두 중재가 나뉘어 있는 이유는 계층이 다르기 때문이다 — 먼저 어느 VC 에
 * 차례를 줄지 정하고, 그 안에서 어느 포트의 트래픽을 보낼지 정한다.
 *
 * 오프셋 계산에 res 가 곱해지는 것이 그래서다. VC 자원마다 제어·상태
 * 레지스터 한 벌씩이 일정한 간격으로 늘어서 있다.
 *
 * 실행 컨텍스트: 절전 복귀. 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 반환값이 없어 실패를 알리지 못하고 로그만 남는다.
 *
 * 호출 체인:
 *   pci_vc_enable() → [이 함수] → pci_wait_for_pending()
 */
static void pci_vc_load_port_arb_table(struct pci_dev *dev, int pos, int res)
{
	/* [한국어] VC 자원 하나의 제어·상태 레지스터 위치. */
	int ctrl_pos, status_pos;
	/* [한국어] 제어 레지스터 값. */
	u32 ctrl;

	/* [한국어] VC 자원 배열에서 res 번째의 제어 레지스터 오프셋을 계산한다.
	 * PCI_CAP_VC_PER_VC_SIZEOF 가 자원 하나가 차지하는 바이트 수다. */
	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	/* [한국어] 같은 방식으로 상태 레지스터 오프셋. */
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	/* [한국어] 현재 값을 읽고, */
	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	/* [한국어] Load Port Arbitration Table 비트를 세워 되쓴다. 위 VC 중재 표와 같은 구조이며,
	 * 다른 점은 이 표가 VC 마다 따로 있어 res 로 지목해야 한다는 것뿐이다. */
	pci_write_config_dword(dev, ctrl_pos,
			       ctrl | PCI_VC_RES_CTRL_LOAD_TABLE);

	/* [한국어] 표가 걸릴 때까지 기다린다. */
	if (pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_TABLE))
		return;

	/* [한국어] 실패하면 어느 VC 였는지와 함께 기록만 남긴다. */
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
/* [한국어]
 * pci_vc_enable - 링크 양끝에서 한 VC 자원을 함께 켠다
 *
 * @dev: 대상 장치.
 * @pos: VC capability 의 위치.
 * @res: 켤 VC 자원 번호.
 *
 * 이 파일에서 가장 까다로운 함수이며, 그 이유가 하나다 — **VC 는 링크
 * 양끝이 함께 켜야 성립한다**. 한쪽만 켜면 그 VC 로 보낸 트래픽을 반대편이
 * 받지 못한다.
 *
 * 그래서 순서가 정해져 있다. 양쪽을 다 켠 **뒤** 에 협상이 끝나기를 기다린다.
 * 한쪽을 켜고 그쪽 협상을 기다렸다가 다른 쪽을 켜면, 첫 협상이 영영 끝나지
 * 않는다 — 상대가 아직 켜지지 않았기 때문이다.
 *
 * 상대편을 찾는 과정도 만만치 않다. 링크 반대편 장치를 찾고, 그쪽의 VC
 * capability 를 찾고, 그 안에서 **같은 VC ID** 를 가진 자원을 찾아야 한다.
 * 자원 번호가 아니라 ID 로 맞추는 것이 요점으로, 같은 VC 가 양쪽에서 서로
 * 다른 번호를 가질 수 있다.
 *
 * 상대를 찾지 못하면 한쪽만 켠다. 그것이 옳은 동작인 경우가 있는데,
 * 루트 포트 아래에 장치가 없는 등의 상황이다.
 *
 * 협상이 끝나지 않아도 기록만 남긴다. 복원 경로라 되돌릴 곳이 없다.
 *
 * 실행 컨텍스트: 절전 복귀. 대기가 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 반환값이 없다. 상대 없음도, 협상 실패도 로그로만 남는다.
 *
 * 호출 체인:
 *   pci_vc_do_save_buffer(복원) → [이 함수]
 *     → pci_vc_load_port_arb_table() → pci_wait_for_pending()
 */
static void pci_vc_enable(struct pci_dev *dev, int pos, int res)
{
	/* [한국어] 두 끝의 제어·상태 위치, VC ID, 상대편 capability 위치, 그 포트의 VC 개수,
	 * 순회 인덱스, 상대편 제어·상태 위치. */
	int ctrl_pos, status_pos, id, pos2, evcc, i, ctrl_pos2, status_pos2;
	/* [한국어] 제어 값, capability 헤더, 상대편 capability 레지스터, 상대편 제어 값. */
	u32 ctrl, header, cap1, ctrl2;
	/* [한국어] 링크 반대편 장치. 찾지 못하면 NULL 로 남고, 그 경우 한쪽만 켠다. */
	struct pci_dev *link = NULL;

	/* Enable VCs from the downstream device */
	if (!pci_is_pcie(dev) || !pcie_downstream_port(dev))
		return;

	/* [한국어] 이 VC 자원의 제어 레지스터 위치. */
	ctrl_pos = pos + PCI_VC_RES_CTRL + (res * PCI_CAP_VC_PER_VC_SIZEOF);
	/* [한국어] 상태 레지스터 위치. */
	status_pos = pos + PCI_VC_RES_STATUS + (res * PCI_CAP_VC_PER_VC_SIZEOF);

	/* [한국어] 현재 제어 값을 읽고, */
	pci_read_config_dword(dev, ctrl_pos, &ctrl);
	/* [한국어] 그 안의 VC ID 를 꺼낸다. 링크 양끝에서 같은 VC 를 가리키는 것은 배열의
	 * 인덱스가 아니라 이 ID 다 — 아래 상대편 탐색이 인덱스가 아니라 ID 로
	 * 짝을 찾는 이유가 그것이다. */
	id = ctrl & PCI_VC_RES_CTRL_ID;

	/* [한국어] 어떤 종류의 VC capability 인지 알기 위해 헤더를 읽는다. */
	pci_read_config_dword(dev, pos, &header);

	/* If there is no opposite end of the link, skip to enable */
	if (PCI_EXT_CAP_ID(header) == PCI_EXT_CAP_ID_VC9 ||
	    pci_is_root_bus(dev->bus))
		goto enable;

	/* [한국어] 상류 포트(dev->bus->self)에서 같은 VC capability 를 찾는다. */
	pos2 = pci_find_ext_capability(dev->bus->self, PCI_EXT_CAP_ID_VC);
	/* [한국어] 없으면 짝지을 상대가 없으므로, */
	if (!pos2)
		goto enable;

	/* [한국어] 그 포트의 Port VC Capability 1 을 읽고, */
	pci_read_config_dword(dev->bus->self, pos2 + PCI_VC_PORT_CAP1, &cap1);
	/* [한국어] Extended VC Count 를 꺼낸다. 아래 순회의 상한이 된다. */
	evcc = cap1 & PCI_VC_CAP1_EVCC;

	/* VC0 is hardwired enabled, so we can start with 1 */
	for (i = 1; i < evcc + 1; i++) {
		/* [한국어] 상대편에서 i 번째 자원의 제어 레지스터 위치. */
		ctrl_pos2 = pos2 + PCI_VC_RES_CTRL +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		/* [한국어] 상태 레지스터 위치. 두 값 모두 루프 밖에서도 쓰이므로 지역 변수가
		 * 루프 바깥에 선언되어 있다 — break 로 빠져나온 뒤의 값이 그대로 쓰인다. */
		status_pos2 = pos2 + PCI_VC_RES_STATUS +
				(i * PCI_CAP_VC_PER_VC_SIZEOF);
		/* [한국어] 상대편의 제어 값을 읽어, */
		pci_read_config_dword(dev->bus->self, ctrl_pos2, &ctrl2);
		/* [한국어] VC ID 가 우리 것과 같으면 짝을 찾은 것이다. */
		if ((ctrl2 & PCI_VC_RES_CTRL_ID) == id) {
			/* [한국어] 그 장치를 기록하고, */
			link = dev->bus->self;
			break;
		}
	}

	/* [한국어] 짝을 찾지 못했으면 상대를 건드릴 수 없으므로, */
	if (!link)
		goto enable;

	/* Disable if enabled */
	if (ctrl2 & PCI_VC_RES_CTRL_ENABLE) {
		/* [한국어] 이미 켜져 있으면 먼저 끈다. 위 영어 주석대로 양끝을 다시 협상시키려면
		 * 한 번 내렸다 올려야 하기 때문이다. */
		ctrl2 &= ~PCI_VC_RES_CTRL_ENABLE;
		/* [한국어] 그 값을 쓴다. */
		pci_write_config_dword(link, ctrl_pos2, ctrl2);
	}

	/* Enable on both ends */
	ctrl2 |= PCI_VC_RES_CTRL_ENABLE;
	/* [한국어] 상대편을 켠다. 우리 쪽보다 먼저 켜는 순서가 중요하다 — 아래에서 우리
	 * 쪽을 켜는 순간 협상이 시작되므로, 그때 상대가 이미 준비되어 있어야 한다. */
	pci_write_config_dword(link, ctrl_pos2, ctrl2);
enable:
	ctrl |= PCI_VC_RES_CTRL_ENABLE;
	/* [한국어] 우리 쪽을 켠다. 이 쓰기로 협상이 시작된다. */
	pci_write_config_dword(dev, ctrl_pos, ctrl);

	/* [한국어] 협상 진행 비트가 내려갈 때까지 기다린다. 내려가지 않으면, */
	if (!pci_wait_for_pending(dev, status_pos, PCI_VC_RES_STATUS_NEGO))
		/* [한국어] 멈춰 있다고 기록한다. */
		pci_err(dev, "VC%d negotiation stuck pending\n", id);

	/* [한국어] 상대가 있었다면 그쪽도 확인한다. */
	if (link && !pci_wait_for_pending(link, status_pos2,
					  PCI_VC_RES_STATUS_NEGO))
		/* [한국어] 역시 기록만 남긴다. */
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
/* [한국어]
 * pci_vc_do_save_buffer - VC 상태를 버퍼에 담거나 버퍼에서 되돌린다, 또는 크기만 잰다
 *
 * @dev: 대상 장치.
 * @pos: VC capability 의 위치.
 * @save_state: 저장 버퍼. NULL 이면 크기만 계산한다.
 * @save: true = 저장, false = 복원.
 * @return: 필요한 버퍼 크기(바이트).
 *
 * 한 함수가 **세 가지 일** 을 겸한다. 저장, 복원, 그리고 크기 계산이다.
 *
 * 셋을 합친 이유가 이 함수의 존재 이유다. VC 상태의 크기는 고정이 아니라
 * capability 레지스터가 알려 주는 VC 개수와 중재 표 크기로 정해진다. 저장할
 * 자리를 순회하는 코드와 크기를 세는 코드가 따로 있으면 반드시 어긋나고,
 * 어긋나면 버퍼를 넘겨 쓰게 된다. 하나로 합치면 그 어긋남이 원천적으로
 * 불가능해진다.
 *
 * 버퍼가 NULL 이면 쓰기 없이 길이만 누적하는 방식으로 그 셋을 구현한다.
 *
 * 맨 앞의 크기 검사가 그 설계를 한 번 더 지킨다 — 미리 할당해 둔 버퍼 크기와
 * 지금 계산한 크기를 비교하고, 다르면 쓰지 않고 물러난다. 할당 시점과 저장
 * 시점 사이에 장치 상태가 달라졌다면 그럴 수 있다.
 *
 * 복원 경로에서만 pci_vc_enable() 을 부른다. 값을 되쓰는 것만으로는 VC 가
 * 켜지지 않고, 링크 양끝의 협상이 따로 필요하기 때문이다.
 *
 * 실행 컨텍스트: 열거(크기 계산)와 절전 진입·복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 크기 불일치는 -ENOMEM 이며 그때 버퍼는 건드리지 않는다.
 *
 * 호출 체인:
 *   pci_save_vc_state() / pci_restore_vc_state() / pci_allocate_vc_save_buffers()
 *     → [이 함수] → pci_vc_save_restore_dwords() → pci_vc_enable()
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
		/* [한국어] 크기를 다시 계산해 저장해 둔 버퍼 크기와 비교한다. 어긋났다는 것은
		 * 할당 시점과 지금 사이에 하드웨어가 보고하는 구성이 달라졌다는 뜻이라,
		 * 그대로 진행하면 버퍼를 넘어 쓰게 된다. */
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
		/* [한국어] 저장 방향이면, */
		if (save)
			/* [한국어] Port VC Control 을 버퍼 앞부분에 담는다. 위 영어 주석이 이것을 **가장 먼저**
			 * 다루는 이유를 밝힌다 — 이 레지스터의 VC Arbitration Select 필드는 저우선순위
			 * VC 가 둘 이상 동작 중이면 바꿀 수 없는데, 리셋 직후에는 VC0 만 켜져 있어
			 * 지금이 유일하게 안전한 시점이기 때문이다. */
			pci_read_config_word(dev, pos + PCI_VC_PORT_CTRL,
					     (u16 *)buf);
		else
			pci_write_config_word(dev, pos + PCI_VC_PORT_CTRL,
					      *(u16 *)buf);
		buf += 4;
	}
	/* [한국어] 2바이트만 썼지만 4바이트를 소비한다. 버퍼를 dword 정렬로 유지해
	 * 아래 pci_vc_save_restore_dwords() 가 u32 포인터로 다룰 수 있게 하려는 것이다. */
	len += 4;

	/*
	 * If we have any Low Priority VCs and a VC Arbitration Table Offset
	 * in Port VC Capability Register 2 then save/restore it next.
	 */
	if (lpevcc) {
		/* [한국어] Port VC Capability 2. */
		u32 cap2;
		/* [한국어] VC 중재 표의 오프셋. */
		int vcarb_offset;

		/* [한국어] 두 번째 capability 레지스터를 읽는다. */
		pci_read_config_dword(dev, pos + PCI_VC_PORT_CAP2, &cap2);
		/* [한국어] 오프셋 필드에 16 을 곱한다. 규격이 이 필드를 16바이트 단위로 정의하기
		 * 때문이다. */
		vcarb_offset = FIELD_GET(PCI_VC_CAP2_ARB_OFF, cap2) * 16;

		/* [한국어] 오프셋이 0 이면 표가 없다는 뜻이므로 있을 때만 진행한다. */
		if (vcarb_offset) {
			/* [한국어] 표의 크기와 단계 수. */
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
				/* [한국어] 32단계. 여기까지 오면 셋 중 가장 작은 값이다. */
				vcarb_phases = 32;

			/* Fixed 4 bits per phase per lpevcc (plus VC0) */
			size = ((lpevcc + 1) * vcarb_phases * 4) / 8;

			/* [한국어] 크기가 0 이 아니고 버퍼가 주어졌을 때만 실제로 옮긴다.
			 * 크기 계산만 하는 호출(buf 가 NULL)에서는 건너뛴다. */
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
					/* [한국어] 복원 방향이면 표를 쓴 것만으로는 부족하다. 위 영어 주석대로 하드웨어에
					 * 다시 읽어 들이라고 지시해야 실제로 적용된다. */
					pci_vc_load_arb_table(dev, pos);

				/* [한국어] 버퍼를 옮긴 만큼 전진시킨다. */
				buf += size;
			}
			/* [한국어] 크기 누적. buf 가 NULL 이어도 이 줄은 실행되므로, 같은 함수로 크기만
			 * 재는 것이 가능해진다. */
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
		/* [한국어] 이 VC 자원의 capability 레지스터. */
		u32 cap;
		/* [한국어] 포트 중재 표의 오프셋. */
		int parb_offset;

		/* [한국어] i 번째 자원의 capability 를 읽는다. */
		pci_read_config_dword(dev, pos + PCI_VC_RES_CAP +
				      (i * PCI_CAP_VC_PER_VC_SIZEOF), &cap);
		/* [한국어] 역시 16바이트 단위 오프셋. */
		parb_offset = FIELD_GET(PCI_VC_RES_CAP_ARB_OFF, cap) * 16;
		/* [한국어] 표가 있을 때만. */
		if (parb_offset) {
			/* [한국어] 크기와 단계 수. */
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

			/* [한국어] 항목 하나의 비트 수와 단계 수를 곱하고 8 로 나눠 바이트 크기를 얻는다.
			 * 항목 크기(parb_size)는 포트 전체에 공통이고 단계 수만 VC 마다 다르다 —
			 * 위 영어 주석이 그 분담을 설명한다. */
			size = (parb_size * parb_phases) / 8;

			/* [한국어] 버퍼가 있을 때만 옮긴다. */
			if (size && buf) {
				pci_vc_save_restore_dwords(dev,
							   pos + parb_offset,
							   (u32 *)buf,
							   size / 4, save);
				/* [한국어] 버퍼 전진. */
				buf += size;
			}
			/* [한국어] 크기 누적. */
			len += size;
		}

		/* VC Resource Control Register */
		if (buf) {
			/* [한국어] 이 자원의 제어 레지스터 위치. */
			int ctrl_pos = pos + PCI_VC_RES_CTRL +
					(i * PCI_CAP_VC_PER_VC_SIZEOF);
			/* [한국어] 저장 방향이면, */
			if (save)
				/* [한국어] 그대로 읽어 담는다. */
				pci_read_config_dword(dev, ctrl_pos,
						      (u32 *)buf);
			else {
				u32 tmp, ctrl = *(u32 *)buf;
				/*
				 * For an FLR case, the VC config may remain.
				 * Preserve enable bit, restore the rest.
				 */
				pci_read_config_dword(dev, ctrl_pos, &tmp);
				/* [한국어] 현재 하드웨어 값에서 활성화 비트만 남기고, */
				tmp &= PCI_VC_RES_CTRL_ENABLE;
				/* [한국어] 저장해 둔 값에서는 활성화 비트를 뺀 나머지를 얹는다. 위 영어 주석대로
				 * FLR(Function Level Reset) 뒤에는 VC 설정이 남아 있을 수 있으므로,
				 * 현재의 활성화 상태를 보존한 채 나머지 설정만 되돌린다. */
				tmp |= ctrl & ~PCI_VC_RES_CTRL_ENABLE;
				pci_write_config_dword(dev, ctrl_pos, tmp);
				/* Load port arbitration table if used */
				if (ctrl & PCI_VC_RES_CTRL_ARB_SELECT)
					pci_vc_load_port_arb_table(dev, pos, i);
				/* Re-enable if needed */
				if ((ctrl ^ tmp) & PCI_VC_RES_CTRL_ENABLE)
					/* [한국어] 저장된 값과 현재 값의 활성화 비트가 다르면, 즉 켜져 있어야 하는데
					 * 꺼져 있으면 링크 양끝을 맞춰 켠다. 위에서 활성화 비트를 일부러 건드리지
					 * 않았기 때문에 이 단계가 따로 필요하다. */
					pci_vc_enable(dev, pos, i);
			}
			/* [한국어] 버퍼 전진. 제어 레지스터가 4바이트다. */
			buf += 4;
		}
		/* [한국어] 크기 누적. */
		len += 4;
	}

	/* [한국어] 버퍼가 있었으면 저장·복원을 한 것이므로 0(성공), 없었으면 계산한 크기를
	 * 돌려준다. 한 함수가 세 가지 일(크기 계산, 저장, 복원)을 하는 것은 위
	 * 영어 주석대로 세 경로의 순서가 어긋나지 않게 하기 위함이다. */
	return buf ? 0 : len;
}

/* [한국어] VC 계열 capability 세 종류를 이름과 함께 묶은 표. 아래 세 함수가 모두
 * 이 표를 돌며 같은 일을 반복한다. */
static struct {
	/* [한국어] 확장 capability ID.
	 * 설정자: 아래 초기화 목록에서 한 번 정해지고 바뀌지 않는다.
	 * 읽는 자: pci_find_ext_capability() 와 pci_find_saved_ext_cap() 의 인자.
	 * 값 범위: MFVC, VC, VC9 세 가지.
	 * 동기화: 사실상 상수라 보호가 필요 없다. */
	u16 id;
	/* [한국어] 진단 메시지에 쓸 이름.
	 * 설정자: 위와 같다.
	 * 읽는 자: pci_err() 의 인자로만 쓰인다.
	 * 값 범위: "MFVC" / "VC" / "VC9".
	 * 동기화: 상수 문자열이라 필요 없다. */
	const char *name;
/* [한국어] 세 종류를 나열한다. MFVC(Multi-Function VC)가 먼저 오고, 표준 VC,
 * 그리고 VC9(VC 의 다른 capability ID 판)가 뒤따른다. 같은 장치가
 * 여럿을 동시에 가질 수 있으므로 세 함수 모두 표 전체를 돈다. */
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
/* [한국어]
 * pci_save_vc_state - 세 VC capability 의 상태를 모두 저장한다
 *
 * @dev: 대상 장치.
 * @return: 0 = 성공, 또는 음수 오류.
 *
 * 절전 진입 시 불린다. D3 에서 config 공간이 비워지므로 미리 담아 두어야 한다.
 *
 * vc_caps 표를 도는 것이 이 함수의 뼈대다. VC 계열 capability 가 셋(MFVC, VC,
 * VC9)이고 셋의 구조가 같아, 표로 두고 같은 코드를 세 번 돌린다.
 *
 * 버퍼가 없으면 오류로 답하는 것이 중요하다. pci_allocate_vc_save_buffers()
 * 가 열거 시점에 미리 잡아 두는데, 그것이 빠졌거나 실패했다는 뜻이다.
 * 그대로 진행하면 복원할 것이 없는 채로 절전에 들어가 복귀 후 VC 가 죽는다.
 *
 * 실행 컨텍스트: 절전 진입. 프로세스 컨텍스트.
 *
 * 에러 경로: 버퍼 부재는 -ENOMEM 이며 어느 capability 였는지 로그로 남는다.
 * 아래 저장이 실패하면 그 오류를 올려보낸다.
 *
 * 호출 체인:
 *   pci_save_state() → [이 함수]
 *     → pci_find_ext_capability() → pci_find_saved_ext_cap()
 *     → pci_vc_do_save_buffer(save=true)
 */
int pci_save_vc_state(struct pci_dev *dev)
{
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] 세 capability 를 차례로 확인한다. */
	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		/* [한국어] capability 위치와 결과. */
		int pos, ret;
		/* [한국어] 미리 할당해 둔 저장 버퍼. */
		struct pci_cap_saved_state *save_state;

		/* [한국어] 이 장치에 그 capability 가 있는지 찾는다. */
		pos = pci_find_ext_capability(dev, vc_caps[i].id);
		/* [한국어] 없으면, */
		if (!pos)
			continue;

		/* [한국어] pci_allocate_vc_save_buffers() 가 미리 잡아 둔 버퍼를 찾는다. */
		save_state = pci_find_saved_ext_cap(dev, vc_caps[i].id);
		/* [한국어] 버퍼가 없으면 할당 단계가 빠졌거나 실패했다는 뜻이므로, */
		if (!save_state) {
			/* [한국어] 어느 capability 였는지 남기고, */
			pci_err(dev, "%s buffer not found in %s\n",
				vc_caps[i].name, __func__);
			return -ENOMEM;
		}

		/* [한국어] 실제 저장. save 인자를 true 로 준다. */
		ret = pci_vc_do_save_buffer(dev, pos, save_state, true);
		/* [한국어] 실패하면(버퍼 크기 불일치), */
		if (ret) {
			/* [한국어] 기록하고, */
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
/* [한국어]
 * pci_restore_vc_state - 저장해 둔 VC 상태를 되돌린다
 *
 * @dev: 대상 장치.
 *
 * pci_save_vc_state() 의 짝이며 절전 복귀 시 불린다.
 *
 * 버퍼가 없어도 조용히 건너뛰는 것이 저장 쪽과 다르다. 저장 쪽은 버퍼 부재를
 * 오류로 알리는데, 여기서는 그때 이미 알렸으므로 다시 알릴 필요가 없다.
 * 복원할 것이 없으면 그저 할 일이 없는 것이다.
 *
 * 되쓰기만으로 끝나지 않고 아래에서 pci_vc_enable() 까지 부른다. VC 는 링크
 * 양끝의 협상이 있어야 실제로 살아나기 때문이다.
 *
 * 실행 컨텍스트: 절전 복귀. 프로세스 컨텍스트.
 *
 * 에러 경로: 반환값이 없다. 복원 실패를 알릴 방법이 없다.
 *
 * 호출 체인:
 *   pci_restore_state() → [이 함수]
 *     → pci_vc_do_save_buffer(save=false) → pci_vc_enable()
 */
void pci_restore_vc_state(struct pci_dev *dev)
{
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] 세 capability 를 차례로. */
	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		/* [한국어] capability 위치. */
		int pos;
		/* [한국어] 저장 버퍼. */
		struct pci_cap_saved_state *save_state;

		/* [한국어] 위치를 찾고, */
		pos = pci_find_ext_capability(dev, vc_caps[i].id);
		/* [한국어] 버퍼를 찾는다. */
		save_state = pci_find_saved_ext_cap(dev, vc_caps[i].id);
		/* [한국어] 둘 중 하나라도 없으면 복원할 수 없으므로 건너뛴다. 저장 쪽이 버퍼 부재를
		 * 오류로 다루는 것과 달리 여기서는 조용히 넘어간다 — 복원 실패로 장치를
		 * 쓸 수 없게 만드느니 기본 설정으로 두는 편이 낫기 때문이다. */
		if (!save_state || !pos)
			continue;

		/* [한국어] 복원. save 인자를 false 로 준다. 반환값을 보지 않는데, 실패해도
		 * 되돌릴 방법이 없기 때문이다. */
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
/* [한국어]
 * pci_allocate_vc_save_buffers - 저장에 쓸 버퍼를 미리 잡아 둔다
 *
 * @dev: 대상 장치.
 *
 * 열거 시점에 불린다. 절전 진입 경로에서는 할당이 실패해도 되돌릴 방법이
 * 마땅치 않으므로, 여유 있을 때 미리 잡는 것이다.
 *
 * 크기를 pci_vc_do_save_buffer() 에 NULL 을 넘겨 얻는다. 실제로 저장할 때
 * 훑는 것과 똑같은 코드가 세어 준 크기라, 나중에 모자랄 수 없다.
 *
 * 할당이 실패해도 기록만 남기고 계속한다. 나머지 capability 는 여전히
 * 잡아 둘 수 있고, 실패한 것은 저장 시점에 다시 걸러진다.
 *
 * 실행 컨텍스트: 열거. 할당이 있어 프로세스 컨텍스트여야 한다.
 *
 * 에러 경로: 할당 실패는 로그로만 남으며, 그 결과가 pci_save_vc_state() 의
 * -ENOMEM 으로 나타난다.
 *
 * 호출 체인:
 *   pci_init_capabilities() → [이 함수]
 *     → pci_vc_do_save_buffer(NULL) → pci_add_ext_cap_save_buffer()
 */
void pci_allocate_vc_save_buffers(struct pci_dev *dev)
{
	/* [한국어] 표 순회 인덱스. */
	int i;

	/* [한국어] 세 capability 를 차례로. */
	for (i = 0; i < ARRAY_SIZE(vc_caps); i++) {
		/* [한국어] 위치를 찾는다. */
		int len, pos = pci_find_ext_capability(dev, vc_caps[i].id);

		/* [한국어] 없으면, */
		if (!pos)
			continue;

		/* [한국어] save_state 를 NULL 로 주어 **크기만** 계산하게 한다. 같은 함수로 크기를
		 * 재기 때문에 실제 저장 때와 크기가 어긋날 수 없다. */
		len = pci_vc_do_save_buffer(dev, pos, NULL, false);
		/* [한국어] 그 크기만큼 버퍼를 미리 잡는다. 실패하면, */
		if (pci_add_ext_cap_save_buffer(dev, vc_caps[i].id, len))
			/* [한국어] 기록만 남긴다. 반환값이 없어 호출자에게 알릴 방법이 없고, 이 단계가
			 * 실패해도 절전 시 저장만 못 할 뿐 장치는 정상 동작하기 때문이다. */
			pci_err(dev, "unable to preallocate %s save buffer\n",
				vc_caps[i].name);
	}
}
