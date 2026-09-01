// SPDX-License-Identifier: GPL-2.0
/*
 * Root Complex Event Collector Support
 *
 * Authors:
 *  Sean V Kelley <sean.v.kelley@intel.com>
 *  Qiuxu Zhuo <qiuxu.zhuo@intel.com>
 *
 * Copyright (C) 2020 Intel Corp.
 */

/*
 * [한국어 설명] 링크 없는 내장 엔드포인트를 대신해 이벤트를 모아 주는 장치 (rcec.c)
 *
 * === 파일의 역할 ===
 * RCiEP(Root Complex Integrated Endpoint)는 Root Complex 안에 통합된
 * 엔드포인트다. 칩 내부에 있어 바깥으로 나가는 PCIe 링크가 없다.
 * 그래서 문제가 생긴다 — 오류 보고(AER)와 전원 이벤트(PME)는 상류 포트로
 * 메시지를 보내는 방식인데, 보낼 링크가 없기 때문이다.
 *
 * RCEC(Root Complex Event Collector)가 그 해법이다. RCiEP 들을 대신해
 * 이벤트를 모아 보고해 주는 전용 function 이며, 어느 RCiEP 들을 담당하는지를
 * 자기 capability(RCEC Endpoint Association)에 비트맵으로 들고 있다.
 *
 * 이 파일은 그 연결 관계를 다룬다.
 *   - RCEC 를 발견하면 담당 RCiEP 목록을 읽어 캐시한다(rcec_ea).
 *   - "이 RCiEP 를 담당하는 RCEC 는 무엇인가" 를 찾아 준다.
 *   - RCEC 에 이벤트가 오면 담당 RCiEP 들을 훑으며 콜백을 적용한다.
 *
 * 연결 관계가 비트맵이라는 점이 특이하다. RCEC 의 Association Bitmap 은
 * 같은 버스의 어느 장치 번호가 자기 담당인지를 32비트로 표시하고, 여러
 * 버스에 걸치면 Next Bus / Last Bus 필드로 범위를 넓힌다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거: probe.c 가 RCEC 를 발견
 *         -> [이 파일] pci_rcec_init() — Association capability 를 읽어 캐시
 *
 * 이벤트: RCEC 에 AER 또는 PME 인터럽트
 *         -> aer.c / pme.c 가 "누구의 이벤트인가" 를 알아야 한다
 *            -> [이 파일] pcie_walk_rcec() 로 담당 RCiEP 들에 콜백 적용
 *
 * 반대 방향: 특정 RCiEP 의 AER 을 켜려면 그 담당 RCEC 를 알아야 한다
 *         -> [이 파일] pcie_link_rcec() 가 RCiEP 의 rcec 포인터를 채운다
 *
 * 실행 컨텍스트: 열거와 이벤트 처리 모두 프로세스/스레드 컨텍스트.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거), pcie/aer.c(오류 이벤트), pcie/pme.c(전원 이벤트),
 *   pcie/portdrv.c(RCEC 도 포트 서비스를 갖는다).
 * 아래쪽: bus.c 의 버스 순회, access.c 의 config 접근.
 * 공유 상태: struct pci_dev 의 rcec_ea(RCEC 쪽: 담당 범위 캐시)와
 *   rcec(RCiEP 쪽: 자기를 담당하는 RCEC 포인터).
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 *
 * 관련이 생기는 경우는 NVMe 컨트롤러가 RCiEP 로 구현된 시스템이다.
 * SoC 에 NVMe 컨트롤러를 통합한 임베디드 플랫폼이나 일부 서버 칩셋에서
 * 그런 구성이 나타난다. 그 경우 NVMe 의 AER 오류와 PME 신호가 RCEC 를
 * 거쳐 보고되므로, 이 파일이 없으면 그 이벤트가 커널에 닿지 않는다.
 *
 * 다만 흔한 구성은 아니다. 카드 형태나 M.2 로 꽂는 NVMe SSD 는
 * PCI_EXP_TYPE_ENDPOINT 이고 자기 링크를 갖는다(access.c 의
 * pcie_cap_has_lnkctl 주석 참고). 그런 장치는 RCEC 와 무관하다.
 *
 * (기존 주석은 "NVMe 장치의 surprise removal" 이 RCEC 를 거친다고
 *  적었으나, RCiEP 는 링크가 없어 물리적으로 뽑을 수 없다. surprise
 *  removal 은 슬롯이 있는 하류 포트에서만 일어나는 일이다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_rcec_init()       : RCEC 의 Association capability 를 읽어 담당 범위를
 *                         캐시한다. 열거 시 1회.
 * pci_rcec_exit()       : 그 캐시를 해제한다.
 * pcie_link_rcec()      : RCiEP 들을 훑으며 각자의 rcec 포인터를 채운다.
 *                         RCEC 가 등록될 때 불린다.
 * pcie_walk_rcec()      : 이 RCEC 가 담당하는 모든 RCiEP 에 콜백을 적용한다.
 *                         AER/PME 이벤트 처리의 진입점이다.
 * walk_rcec_helper()    : 위의 실제 순회 로직. 비트맵과 버스 범위를 해석해
 *                         담당 여부를 판정한다.
 * rcec_assoc_rciep()    : 특정 RCiEP 가 이 RCEC 의 담당인지 판정한다.
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>

#include "../pci.h"

struct walk_rcec_data {
	struct pci_dev *rcec;
	int (*user_callback)(struct pci_dev *dev, void *data);
	void *user_data;
};

static bool rcec_assoc_rciep(struct pci_dev *rcec, struct pci_dev *rciep)
{
	unsigned long bitmap = rcec->rcec_ea->bitmap;
	unsigned int devn;

	/* An RCiEP found on a different bus in range */
	if (rcec->bus->number != rciep->bus->number)
		return true;

	/* Same bus, so check bitmap */
	for_each_set_bit(devn, &bitmap, 32)
		if (devn == PCI_SLOT(rciep->devfn))
			return true;

	return false;
}

static int link_rcec_helper(struct pci_dev *dev, void *data)
{
	struct walk_rcec_data *rcec_data = data;
	struct pci_dev *rcec = rcec_data->rcec;

	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&
	    rcec_assoc_rciep(rcec, dev)) {
		dev->rcec = rcec;
		pci_dbg(dev, "PME & error events signaled via %s\n",
			pci_name(rcec));
	}

	return 0;
}

static int walk_rcec_helper(struct pci_dev *dev, void *data)
{
	struct walk_rcec_data *rcec_data = data;
	struct pci_dev *rcec = rcec_data->rcec;

	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&
	    rcec_assoc_rciep(rcec, dev))
		rcec_data->user_callback(dev, rcec_data->user_data);

	return 0;
}

static void walk_rcec(int (*cb)(struct pci_dev *dev, void *data),
		      void *userdata)
{
	struct walk_rcec_data *rcec_data = userdata;
	struct pci_dev *rcec = rcec_data->rcec;
	u8 nextbusn, lastbusn;
	struct pci_bus *bus;
	unsigned int bnr;

	if (!rcec->rcec_ea)
		return;

	/* Walk own bus for bitmap based association */
	pci_walk_bus(rcec->bus, cb, rcec_data);

	nextbusn = rcec->rcec_ea->nextbusn;
	lastbusn = rcec->rcec_ea->lastbusn;

	/* All RCiEP devices are on the same bus as the RCEC */
	if (nextbusn == 0xff && lastbusn == 0x00)
		return;

	for (bnr = nextbusn; bnr <= lastbusn; bnr++) {
		/* No association indicated (PCIe 5.0-1, 7.9.10.3) */
		if (bnr == rcec->bus->number)
			continue;

		bus = pci_find_bus(pci_domain_nr(rcec->bus), bnr);
		if (!bus)
			continue;

		/* Find RCiEP devices on the given bus ranges */
		pci_walk_bus(bus, cb, rcec_data);
	}
}

/**
 * pcie_link_rcec - Link RCiEP devices associated with RCEC.
 * @rcec: RCEC whose RCiEP devices should be linked.
 *
 * Link the given RCEC to each RCiEP device found.
 */
void pcie_link_rcec(struct pci_dev *rcec)
{
	struct walk_rcec_data rcec_data;

	if (!rcec->rcec_ea)
		return;

	rcec_data.rcec = rcec;
	rcec_data.user_callback = NULL;
	rcec_data.user_data = NULL;

	walk_rcec(link_rcec_helper, &rcec_data);
}

/**
 * pcie_walk_rcec - Walk RCiEP devices associating with RCEC and call callback.
 * @rcec:	RCEC whose RCiEP devices should be walked
 * @cb:		Callback to be called for each RCiEP device found
 * @userdata:	Arbitrary pointer to be passed to callback
 *
 * Walk the given RCEC. Call the callback on each RCiEP found.
 *
 * If @cb returns anything other than 0, break out.
 */
void pcie_walk_rcec(struct pci_dev *rcec, int (*cb)(struct pci_dev *, void *),
		    void *userdata)
{
	struct walk_rcec_data rcec_data;

	if (!rcec->rcec_ea)
		return;

	rcec_data.rcec = rcec;
	rcec_data.user_callback = cb;
	rcec_data.user_data = userdata;

	walk_rcec(walk_rcec_helper, &rcec_data);
}

void pci_rcec_init(struct pci_dev *dev)
{
	struct rcec_ea *rcec_ea;
	u32 rcec, hdr, busn;
	u8 ver;

	/* Only for Root Complex Event Collectors */
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_RC_EC)
		return;

	rcec = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_RCEC);
	if (!rcec)
		return;

	rcec_ea = kzalloc_obj(*rcec_ea);
	if (!rcec_ea)
		return;

	pci_read_config_dword(dev, rcec + PCI_RCEC_RCIEP_BITMAP,
			      &rcec_ea->bitmap);

	/* Check whether RCEC BUSN register is present */
	pci_read_config_dword(dev, rcec, &hdr);
	ver = PCI_EXT_CAP_VER(hdr);
	if (ver >= PCI_RCEC_BUSN_REG_VER) {
		pci_read_config_dword(dev, rcec + PCI_RCEC_BUSN, &busn);
		rcec_ea->nextbusn = PCI_RCEC_BUSN_NEXT(busn);
		rcec_ea->lastbusn = PCI_RCEC_BUSN_LAST(busn);
	} else {
		/* Avoid later ver check by setting nextbusn */
		rcec_ea->nextbusn = 0xff;
		rcec_ea->lastbusn = 0x00;
	}

	dev->rcec_ea = rcec_ea;
}

void pci_rcec_exit(struct pci_dev *dev)
{
	kfree(dev->rcec_ea);
	dev->rcec_ea = NULL;
}
