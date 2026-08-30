// SPDX-License-Identifier: GPL-2.0
/*
 * Simple stub driver to reserve a PCI device
 *
 * Copyright (C) 2008 Red Hat, Inc.
 * Author:
 *	Chris Wright
 *
 * Usage is simple, allocate a new id to the stub driver and bind the
 * device to it.  For example:
 *
 * # echo "8086 10f5" > /sys/bus/pci/drivers/pci-stub/new_id
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/e1000e/unbind
 * # echo -n 0000:00:19.0 > /sys/bus/pci/drivers/pci-stub/bind
 * # ls -l /sys/bus/pci/devices/0000:00:19.0/driver
 * .../0000:00:19.0/driver -> ../../../bus/pci/drivers/pci-stub
 */

#include <linux/module.h>	/* PCI/NVMe: module_init/exit, module_param_string 등
				 * 커널 모듈 메타데이터 정의에 사용. */
#include <linux/pci.h>		/* PCI/NVMe: struct pci_dev, struct pci_driver,
				 * pci_register_driver(), pci_add_dynid() 등
				 * NVMe PCIe 호스트(drivers/nvme/host/pci.c)가 의존하는
				 * 동일한 PCI 코어 헤더. */

static char ids[1024] __initdata;	/* PCI/NVMe: __initdata 섹션에 둔 모듈 파라미터
					 * 버퍼. NVMe 장치를 PCI-stub에 바인딩할
					 * 때 전달하는 "vendor:device:..." 문자열을
					 * 저장. */

module_param_string(ids, ids, sizeof(ids), 0);
	/* PCI/NVMe: insmod pci-stub.ko ids="..." 형태로 NVMe/PCI 장치의
	 * vendor/device/class를 동적으로 등록할 수 있게 한다. */
MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the stub driver, format is "
			"\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
			" and multiple comma separated entries can be specified");
	/* PCI/NVMe: 형식은 PCI_DEVICE() 매칭과 동일하며, NVMe SSD를
	 * 다른 드라이버(nvme, vfio-pci 등)에서 미리 분리해 예약할 때
	 * 사용한다. */

static int pci_stub_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	/* PCI/NVMe: struct pci_dev는 NVMe 호스트 드라이버에서
	 * pci_set_drvdata(), pci_enable_device_mem(),
	 * pci_request_regions(), pci_iomap() 등을 호출하기 전
	 * PCI 코어가 채워주는 열거 결과물이다. */
	pci_info(dev, "claimed by stub\n");
		/* PCI/NVMe: stub가 해당 PCI 함수(예: NVMe SSD의 PCIe
		 * function)를 점유했음을 로그로 남긴다. 실제 NVMe
		 * 드라이버라면 여기서 PCI BAR 매핑, DMA 마스크 설정,
		 * MSI-X enable, ASPM 조정 등을 수행한다. */
	return 0;
		/* PCI/NVMe: probe 성공. PCI 코어는 이후 pci_dev->driver
		 * 를 pci-stub로 기록하고, NVMe 호스트 관점에서는
		 * 해당 디바이스가 nvme 드라이버에 노출되지 않게 된다. */
}

static struct pci_driver stub_driver = {
	.name		= "pci-stub",
		/* PCI/NVMe: /sys/bus/pci/drivers/pci-stub 아래에 노출되는
		 * 드라이버 이름. NVMe 장치의 driver 심볼릭 링크가 이
		 * 이름을 가리키게 된다. */
	.id_table	= NULL,	/* only dynamic id's */
		/* PCI/NVMe: 정적 ID 테이블은 비어 있고, new_id/sysfs
		 * 동적 ID만으로 매칭. NVMe SSD의 class-code 0x010802
		 * 를 여기에 추가할 수 있다. */
	.probe		= pci_stub_probe,
		/* PCI/NVMe: probe 콜백. PCI 코어가 NVMe/PCI 장치를
		 * pci-stub에 바인딩할 때 호출. */
	.driver_managed_dma = true,
		/* PCI/NVMe: DMA 매핑을 드라이버가 직접 관리함을 표시.
		 * VFIO 등을 통해 NVMe 장치를 게스트에 패스스루할 때
		 * IOMMU 그룹과 dma_ops 처리가 이 플래그에 영향을 받는다. */
};

static int __init pci_stub_init(void)
{
	char *p, *id;
	int rc;

	rc = pci_register_driver(&stub_driver);
		/* PCI/NVMe: PCI 버스에 stub_driver를 등록. 등록 후
		 * /sys/bus/pci/drivers/pci-stub 디렉터리가 생성되고
		 * NVMe/PCI 장치의 동적 바인딩이 가능해진다. */
	if (rc)
		return rc;
		/* PCI/NVMe: 드라이버 등록 실패 시 모듈 로딩을 중단. */

	/* no ids passed actually */
	if (ids[0] == '\0')
		return 0;
		/* PCI/NVMe: ids 모듈 파라미터가 없으면 동적 ID 추가 없이
		 * 종료. 사용자가 이후 echo > new_id로 NVMe 장치를
		 * 예약할 수 있다. */

	/* add ids specified in the module parameter */
	p = ids;
	while ((id = strsep(&p, ","))) {
			/* PCI/NVMe: 쉼표로 구분된 여러 PCI ID 문자열을
			 * 하나씩 파싱. 여러 NVMe 컨트롤러를 한 번에
			 * 예약할 때 유용하다. */
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
			/* PCI/NVMe: PCI_ANY_ID(0xFFFFFFFF)는 wildcard.
			 * NVMe class-code 0x010802를 class에, class_mask에
			 * 0xFFFFFF를 주면 모든 NVMe 컨트롤러를 매칭할 수
			 * 있다. */
		int fields;

		if (!strlen(id))
			continue;
			/* PCI/NVMe: 연속 쉼표 등으로 빈 항목은 무시. */

		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				&vendor, &device, &subvendor, &subdevice,
				&class, &class_mask);
			/* PCI/NVMe: "vendor:device[:subvendor:subdevice:
			 * class:class_mask]" 16진수 파싱. NVMe PCIe
			 * 장치를 정확히 지정하거나 class 기반으로
			 * 일괄 예약한다. */

		if (fields < 2) {
			pr_warn("pci-stub: invalid ID string \"%s\"\n", id);
			continue;
			/* PCI/NVMe: vendor/device는 필수. 형식 오류 시
			 * 경고 후 다음 ID로 진행. */
		}

		pr_info("pci-stub: add %04X:%04X sub=%04X:%04X cls=%08X/%08X\n",
		       vendor, device, subvendor, subdevice, class, class_mask);
			/* PCI/NVMe: 추가하려는 PCI ID 정보를 로그로 남김.
			 * NVMe 장치의 VID/DID(class 0x010802)를 확인할
			 * 때 참고. */

		rc = pci_add_dynid(&stub_driver, vendor, device,
				   subvendor, subdevice, class, class_mask, 0);
			/* PCI/NVMe: stub_driver의 동적 ID 테이블에 항목
			 * 추가. 이후 해당 vendor/device/class의 NVMe/PCI
			 * 장치가 드라이버 바인딩 시 pci_stub_probe()를
			 * 호출. */
		if (rc)
			pr_warn("pci-stub: failed to add dynamic ID (%d)\n",
				rc);
			/* PCI/NVMe: 동적 ID 등록 실패 시 경고. 실패핏도
			 * 다른 ID 처리는 계속. */
	}

	return 0;
		/* PCI/NVMe: init 완료. 등록된 동적 ID에 매칭되는
		 * NVMe/PCI 장치는 pci-stub에 의해 점유된다. */
}

static void __exit pci_stub_exit(void)
{
	pci_unregister_driver(&stub_driver);
		/* PCI/NVMe: PCI 버스에서 stub_driver 등록 해제. 바인딩된
		 * NVMe/PCI 장치는 probe 콜백 없이 드라이버에서 분리되어
		 * 다시 nvme 등 다른 드라이버에 매칭될 수 있다. */
}

module_init(pci_stub_init);
	/* PCI/NVMe: 모듈 로딩 시 pci_stub_init() 실행. */
module_exit(pci_stub_exit);
	/* PCI/NVMe: 모듈 언로드 시 pci_stub_exit() 실행. */

MODULE_DESCRIPTION("VM device assignment stub driver");
	/* PCI/NVMe: VFIO/VM 패스스루용 stub 드라이버임을 명시.
	 * NVMe 장치를 게스트에 직접 할당하기 전 호스트 드라이버로
	 * 사용되는 경우가 많다. */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Wright <chrisw@sous-sol.org>");
