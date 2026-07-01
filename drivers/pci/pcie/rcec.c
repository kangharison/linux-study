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
 * ===================================================================
 * NVMe PCIe 호스트 드라이버 관점 파일 요약
 * -------------------------------------------------------------------
 * 본 파일(drivers/pci/pcie/rcec.c)은 Root Complex Event Collector(RCEC)를
 * 관리하고, RCEC와 연관된 Root Complex Integrated Endpoint(RCiEP) 장치들을
 * 연결/순회하는 기능을 제공한다.
 *
 * NVMe SSD 입장에서 RCEC는 다음과 같은 맥락에서 중요하다.
 *   - 일부 SoC/서버 플랫폼에서 NVMe 컨트롤러가 RCiEP 형태로 Root Complex에
 *     내장되어 있을 수 있다. 이러한 NVMe 장치의 PME(Power Management Event),
 *     AER(Advanced Error Reporting), EDR(Error Disconnect Recover) 등의 이벤트는
 *     연결된 RCEC를 통해 보고된다.
 *   - RCEC는 RCiEP가 발생시킨 에러/전원 이벤트를 수집하여 운영체제에 전달한다.
 *     따라서 NVMe 장치의 surprise removal, link down, uncorrectable error 등의
 *     처리 과정에서 RCEC 링크 정보가 사용될 수 있다.
 *   - pcie_link_rcec()는 NVMe RCiEP의 pci_dev->rcec 포인터를 채워서, 추후
 *     에러 핸들러나 EDR 콜백이 해당 NVMe를 찾아갈 수 있게 한다.
 *   - pcie_walk_rcec()는 특정 RCEC에 연결된 모든 RCiEP(잠재적 NVMe 포함)를
 *     순회하며 사용자 콜백을 호출한다. 이는 AER 서비스 routine에서
 *     pcie_do_recovery() 등과 연결된다.
 *
 * NVMe 드라이버와의 대표적 연결 경로:
 *   nvme_probe -> pci_enable_device -> ...
 *   (장치가 RCiEP로 인식된 경우)
 *   pci_rcec_init() (probe 시 RCEC capability 파싱)
 *   pcie_link_rcec() (해당 NVMe RCiEP를 RCEC에 연결)
 *   AER/EDR 발생 시 pcie_walk_rcec()로 연관 NVMe 순회 후 복구 수행
 *
 * 본 파일은 PCI core의 device probe/remove 흐름과 AER/EDR 서브시스템에서
 * 간접적으로 사용되며, NVMe 드라이버가 직접 호출하지는 않지만 NVMe RCiEP의
 * 에러/전원 이벤트 라우팅과 복구 경로 설정에 근간이 된다.
 * ===================================================================
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/pci_regs.h>

#include "../pci.h"

/*
 * walk_rcec_data:
 *   RCEC 순회 작업에 필요한 컨텍스트를 담는 구조체이다.
 *   NVMe 관련 처리 시 이 구조체를 통해 대상 RCEC, 사용자 콜백, 콜백 데이터를
 *   전달한다.
 */
struct walk_rcec_data {
	struct pci_dev *rcec;			/* NVMe: 순회 대상 RCEC 디바이스 포인터. */
	int (*user_callback)(struct pci_dev *dev, void *data);	/* NVMe: 각 RCiEP(NVMe 포함)마다 호출할 사용자 콜백 함수 포인터. */
	void *user_data;			/* NVMe: 사용자 콜백에 전달할 컨텍스트 데이터. */
};

/*
 * rcec_assoc_rciep:
 *   주어진 RCEC와 RCiEP가 RCEC Associativity 정보에 따라 실제로 연관되어
 *   있는지 판단한다. NVMe 장치가 RCiEP로 노출된 경우, 해당 NVMe가 어떤
 *   RCEC의 이벤트를 수신하는지 결정할 때 이 함수가 사용된다.
 */
static bool rcec_assoc_rciep(struct pci_dev *rcec, struct pci_dev *rciep)
{
	unsigned long bitmap = rcec->rcec_ea->bitmap;			/* NVMe: RCEC가 관리하는 RCiEP 비트맵을 읽어온다. */
	unsigned int devn;						/* NVMe: 비트맵에서 검사할 장치 번호 변수. */

	/* An RCiEP found on a different bus in range */
	if (rcec->bus->number != rciep->bus->number)			/* NVMe: NVMe RCiEP와 RCEC가 서로 다른 bus 번호에 있으면 */
		return true;						/* NVMe: 연관된 것으로 간주하고 true 반환. */

	/* Same bus, so check bitmap */
	for_each_set_bit(devn, &bitmap, 32)				/* NVMe: 32비트 비트맵에서 설정된 비트(연관된 장치 번호)를 하나씩 순회한다. */
		if (devn == PCI_SLOT(rciep->devfn))			/* NVMe: 현재 비트에 해당하는 장치 번호가 NVMe RCiEP의 장치 번호와 일치하면 */
			return true;					/* NVMe: 연관된 RCiEP로 판정한다. */

	return false;								/* NVMe: 비트맵에도 없고 같은 bus도 아니면 연관되지 않음. */
}

/*
 * link_rcec_helper:
 *   pci_walk_bus()가 각 PCI 장치를 순회할 때 호출되는 콜백이다.
 *   NVMe RCiEP를 발견하면 해당 pci_dev->rcec 포인터를 연결하여, 추후
 *   PME/AER/EDR 처리 시 어떤 RCEC가 이 NVMe의 이벤트를 담당하는지
 *   빠르게 찾을 수 있게 한다.
 */
static int link_rcec_helper(struct pci_dev *dev, void *data)
{
	struct walk_rcec_data *rcec_data = data;			/* NVMe: walk_rcec_data 컨텍스트를 data에서 꺼낸다. */
	struct pci_dev *rcec = rcec_data->rcec;			/* NVMe: 연결하려는 대상 RCEC 디바이스. */

	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&		/* NVMe: 현재 장치가 Root Complex Integrated Endpoint(RCiEP)인지 확인, 일부 NVMe는 이 타입이다. */
	    rcec_assoc_rciep(rcec, dev)) {				/* NVMe: 해당 NVMe RCiEP가 대상 RCEC와 연관되어 있는지 검사. */
		dev->rcec = rcec;					/* NVMe: NVMe pci_dev의 rcec 포인터를 연결, 에러/전원 이벤트 라우팅에 사용. */
		pci_dbg(dev, "PME & error events signaled via %s\n",
			pci_name(rcec));				/* NVMe: NVMe의 PME 및 에러 이벤트가 어떤 RCEC로 보고되는지 디버그 로그 출력. */
	}

	return 0;								/* NVMe: 순회를 계속 진행하기 위해 0 반환. */
}

/*
 * walk_rcec_helper:
 *   pcie_walk_rcec()에서 사용하는 날쌘 콜백 헬퍼이다.
 *   연관된 NVMe RCiEP를 발견하면 사용자가 등록한 콜백(예: AER/EDR 복구
 *   루틴)을 호출한다.
 */
static int walk_rcec_helper(struct pci_dev *dev, void *data)
{
	struct walk_rcec_data *rcec_data = data;			/* NVMe: walk context를 data에서 꺼낸다. */
	struct pci_dev *rcec = rcec_data->rcec;			/* NVMe: 순회 기준이 되는 RCEC. */

	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&		/* NVMe: 현재 장치가 RCiEP 타입인지 확인. */
	    rcec_assoc_rciep(rcec, dev))				/* NVMe: RCEC와 연관된 NVMe 장치인지 확인. */
		rcec_data->user_callback(dev, rcec_data->user_data);	/* NVMe: 연관된 NVMe에 대해 사용자 콜백(예: EDR 복구)을 호출한다. */

	return 0;								/* NVMe: 정상 반환, 순회 계속. */
}

/*
 * walk_rcec:
 *   RCEC와 연관된 RCiEP를 버스 단위로 순회하는 날 구현이다.
 *   NVMe 관점에서는 RCEC가 담당하는 NVMe 장치들을 찾아 콜백을 적용하는
 *   핵심 루프이다.
 */
static void walk_rcec(int (*cb)(struct pci_dev *dev, void *data),
		      void *userdata)
{
	struct walk_rcec_data *rcec_data = userdata;			/* NVMe: 사용자가 전달한 walk context 포인터. */
	struct pci_dev *rcec = rcec_data->rcec;			/* NVMe: 순회 기준 RCEC. */
	u8 nextbusn, lastbusn;						/* NVMe: RCEC가 커버하는 bus 번호 범위 변수. */
	struct pci_bus *bus;						/* NVMe: 탐색 중인 PCI bus 임시 포인터. */
	unsigned int bnr;						/* NVMe: bus 번호 루프 인덱스. */

	if (!rcec->rcec_ea)							/* NVMe: RCEC associativity 정보가 없으면 */
		return;								/* NVMe: 순회할 대상이 없으므로 즉시 반환. */

	/* Walk own bus for bitmap based association */
	pci_walk_bus(rcec->bus, cb, rcec_data);				/* NVMe: RCEC가 속한 동일 bus에서 NVMe RCiEP를 bitmap 기준으로 순회. */

	nextbusn = rcec->rcec_ea->nextbusn;				/* NVMe: RCEC가 관리하는 시작 bus 번호를 읽는다. */
	lastbusn = rcec->rcec_ea->lastbusn;				/* NVMe: RCEC가 관리하는 마지막 bus 번호를 읽는다. */

	/* All RCiEP devices are on the same bus as the RCEC */
	if (nextbusn == 0xff && lastbusn == 0x00)			/* NVMe: BUSN 레지스터가 없어 모든 RCiEP가 RCEC와 동일 bus에 있다면 */
		return;								/* NVMe: 추가 bus 탐색이 불필요하므로 종료. */

	for (bnr = nextbusn; bnr <= lastbusn; bnr++) {			/* NVMe: RCEC가 커버하는 추가 bus 범위를 순회하며 NVMe를 찾는다. */
		/* No association indicated (PCIe 5.0-1, 7.9.10.3) */
		if (bnr == rcec->bus->number)				/* NVMe: 이미 탐색한 RCEC의 자신 bus 번호는 건다. */
			continue;						/* NVMe: 중복 탐색 회피. */

		bus = pci_find_bus(pci_domain_nr(rcec->bus), bnr);	/* NVMe: 같은 PCI domain 내에서 해당 bus 번호의 pci_bus 구조체를 검색. */
		if (!bus)							/* NVMe: 해당 번호의 bus가 존재하지 않으면 */
			continue;						/* NVMe: 다음 bus로 넘어간다. */

		/* Find RCiEP devices on the given bus ranges */
		pci_walk_bus(bus, cb, rcec_data);			/* NVMe: 발견한 bus 위에서 NVMe RCiEP를 모두 순회한다. */
	}
}

/**
 * pcie_link_rcec - Link RCiEP devices associated with RCEC.
 * @rcec: RCEC whose RCiEP devices should be linked.
 *
 * Link the given RCEC to each RCiEP device found.
 */
/*
 * pcie_link_rcec:
 *   시스템 내에서 주어진 RCEC와 연관된 모든 RCiEP를 찾아, 각 RCiEP의
 *   pci_dev->rcec 포인터에 연결한다. NVMe 장치가 RCiEP로 존재할 경우
 *   이 링크를 통해 NVMe의 PME/AER/EDR 이벤트가 올바른 RCEC로 라우팅된다.
 */
void pcie_link_rcec(struct pci_dev *rcec)
{
	struct walk_rcec_data rcec_data;				/* NVMe: 순회에 사용할 로컬 컨텍스트 구조체. */

	if (!rcec->rcec_ea)							/* NVMe: RCEC associativity 정보가 없으면 */
		return;								/* NVMe: 연결할 NVMe가 없으므로 즉시 반환. */

	rcec_data.rcec = rcec;						/* NVMe: 컨텍스트에 대상 RCEC를 저장. */
	rcec_data.user_callback = NULL;					/* NVMe: 링크 작업에는 사용자 콜백이 필요 없음. */
	rcec_data.user_data = NULL;					/* NVMe: 사용자 데이터도 사용하지 않음. */

	walk_rcec(link_rcec_helper, &rcec_data);			/* NVMe: RCEC 연결 헬퍼를 순회하며 NVMe RCiEP들을 연결. */
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
/*
 * pcie_walk_rcec:
 *   특정 RCEC에 연관된 RCiEP(NVMe 포함)를 순회하며 사용자 콜백을 호출한다.
 *   AER(Advanced Error Reporting)나 EDR(Error Disconnect Recover) 처리
 *   경로에서, RCEC에 연결된 NVMe 장치들에 대해 복구 콜백을 실행할 때
 *   사용된다.
 */
void pcie_walk_rcec(struct pci_dev *rcec, int (*cb)(struct pci_dev *, void *),
		    void *userdata)
{
	struct walk_rcec_data rcec_data;				/* NVMe: 순회 컨텍스트 구조체. */

	if (!rcec->rcec_ea)							/* NVMe: RCEC associativity capability가 없으면 */
		return;								/* NVMe: 순회할 연관 NVMe가 없으므로 반환. */

	rcec_data.rcec = rcec;						/* NVMe: 순회 기준 RCEC 저장. */
	rcec_data.user_callback = cb;					/* NVMe: NVMe 장치마다 호출할 복구/처리 콜백 저장. */
	rcec_data.user_data = userdata;					/* NVMe: 콜백에 전달할 컨텍스트(예: EDR 상태) 저장. */

	walk_rcec(walk_rcec_helper, &rcec_data);			/* NVMe: 연관된 NVMe RCiEP를 찾아 사용자 콜백을 실행. */
}

/*
 * pci_rcec_init:
 *   PCI 장치가 RCEC 타입일 경우 RCEC Extended Associativity capability를
 *   파싱하여 rcec_ea 구조체를 채운다. NVMe 장치가 RCiEP로 연결되기 전에
 *   RCEC 자체가 먼저 초기화되어야 하며, 이 함수는 device probe 시
 *   호출된다.
 */
void pci_rcec_init(struct pci_dev *dev)
{
	struct rcec_ea *rcec_ea;					/* NVMe: RCEC associativity 정보를 저장할 구조체 포인터. */
	u32 rcec, hdr, busn;						/* NVMe: RCEC 확장 capability 오프셋, 헤더, BUSN 레지스터 값. */
	u8 ver;								/* NVMe: RCEC 확장 capability 버전. */

	/* Only for Root Complex Event Collectors */
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_RC_EC)			/* NVMe: 현재 장치가 RCEC 타입이 아니면 */
		return;								/* NVMe: NVMe와 무관한 일반 장치이므로 초기화 불필요. */

	rcec = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_RCEC);	/* NVMe: RCEC 확장 capability를 PCI config space에서 검색. */
	if (!rcec)								/* NVMe: RCEC capability가 없으면 */
		return;								/* NVMe: 이 장치는 NVMe RCiEP의 이벤트를 수집하지 않음. */

	rcec_ea = kzalloc_obj(*rcec_ea);				/* NVMe: rcec_ea 구조체를 위한 메모리를 커널에서 할당. */
	if (!rcec_ea)								/* NVMe: 메모리 할당 실패 시 */
		return;								/* NVMe: RCEC 초기화를 중단, NVMe RCiEP 연결 불가. */

	pci_read_config_dword(dev, rcec + PCI_RCEC_RCIEP_BITMAP,
			      &rcec_ea->bitmap);					/* NVMe: RCIEP Bitmap 레지스터를 읽어, 동일 bus 내 연관 RCiEP(NVMe) 비트맵 저장. */

	/* Check whether RCEC BUSN register is present */
	pci_read_config_dword(dev, rcec, &hdr);				/* NVMe: RCEC 확장 capability 헤더를 읽는다. */
	ver = PCI_EXT_CAP_VER(hdr);					/* NVMe: 헤더에서 capability 버전을 추출. */
	if (ver >= PCI_RCEC_BUSN_REG_VER) {				/* NVMe: BUSN 레지스터가 존재하는 버전이면 */
		pci_read_config_dword(dev, rcec + PCI_RCEC_BUSN, &busn);	/* NVMe: RCEC BUSN 레지스터 값을 읽는다. */
		rcec_ea->nextbusn = PCI_RCEC_BUSN_NEXT(busn);		/* NVMe: RCEC가 커버하는 시작 bus 번호 저장. */
		rcec_ea->lastbusn = PCI_RCEC_BUSN_LAST(busn);		/* NVMe: RCEC가 커버하는 마지막 bus 번호 저장. */
	} else {
		/* Avoid later ver check by setting nextbusn */
		rcec_ea->nextbusn = 0xff;				/* NVMe: BUSN 레지스터가 없음을 표시하기 위해 시작 bus를 0xff로 설정. */
		rcec_ea->lastbusn = 0x00;				/* NVMe: 마지막 bus를 0x00으로 설정하여 추가 bus 순회를 막는다. */
	}

	dev->rcec_ea = rcec_ea;						/* NVMe: RCEC pci_dev에 파싱한 associativity 정보를 연결, 이후 NVMe RCiEP 링크에 사용. */
}

/*
 * pci_rcec_exit:
 *   RCEC 장치가 제거될 때 rcec_ea 메모리를 해제하고 포인터를 정리한다.
 *   NVMe RCiEP가 제거되거나 hot-unplug되기 전에 RCEC 정리가 필요한 경우
 *   호출된다.
 */
void pci_rcec_exit(struct pci_dev *dev)
{
	kfree(dev->rcec_ea);						/* NVMe: RCEC associativity 구조체 메모리 해제. */
	dev->rcec_ea = NULL;						/* NVMe: 포인터를 NULL로 설정해 dangling reference 방지. */
}
