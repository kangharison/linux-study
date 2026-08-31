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

/*
 * [한국어 설명] 장치를 아무것도 하지 않는 드라이버로 붙잡아 두는 도구 (pci-stub.c)
 *
 * === 파일의 역할 ===
 * 하는 일이 없는 것이 목적인 드라이버다. probe 에서 0 을 돌려주고 끝이며,
 * 하드웨어를 전혀 건드리지 않는다.
 *
 * 왜 그런 것이 필요한가. 장치를 게스트 VM 에 넘기거나 userspace 드라이버로
 * 쓰려면, 먼저 커널의 진짜 드라이버가 그 장치를 잡지 못하게 해야 한다.
 * 아무 드라이버도 없으면 나중에 모듈이 로드될 때 잡힐 수 있으므로,
 * "아무것도 안 하는 드라이버" 를 미리 붙여 자리를 차지하게 하는 것이다.
 *
 * 사용법은 모듈 파라미터로 ID 를 주는 것이다:
 *   modprobe pci-stub ids=8086:10f5,144d:a804
 * 그러면 그 ID 의 장치들이 이 드라이버에 바인딩되고, 진짜 드라이버는
 * 붙지 못한다. 이후 sysfs 로 unbind 해서 VFIO 에 넘긴다.
 *
 * VFIO 가 나온 뒤로는 vfio-pci 를 직접 바인딩하는 방식이 일반적이라
 * 쓰임이 줄었지만, "드라이버가 없는 상태" 와 "이 장치는 의도적으로
 * 비워 두었다" 를 구분한다는 점에서 여전히 유용하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * modprobe pci-stub ids=...
 *   -> [이 파일] pci_stub_init()
 *      -> ids 문자열을 파싱해 pci_add_dynid() [pci-driver.c] 로 등록
 *      -> 드라이버 코어가 매칭되는 장치를 찾아 probe 를 부른다
 *         -> [이 파일] pci_stub_probe() — 아무것도 하지 않고 0 반환
 *
 * 실행 컨텍스트: 모듈 로드 시점(프로세스 컨텍스트).
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: 사용자의 modprobe, 그리고 sysfs 의 bind/unbind.
 * 아래쪽: pci-driver.c 의 pci_add_dynid, pci_register_driver.
 * 공유 상태: 없다.
 *
 * === NVMe 관점 ===
 * NVMe 와 직접 관련은 없지만, NVMe 를 userspace 드라이버로 실험할 때
 * 첫 단계로 쓸 수 있다. 커널 nvme 드라이버가 잡기 전에 이것으로
 * 붙잡아 두면, 그 드라이브는 /dev/nvme0n1 로 나타나지 않고
 * SPDK 나 VFIO 에 넘길 수 있는 상태가 된다.
 *
 * 다만 실무에서는 커널 파라미터로 nvme 드라이버 자체를 막거나
 * vfio-pci 를 직접 바인딩하는 편이 흔하다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_stub_probe()  : 아무것도 하지 않고 0 을 돌려준다. 그것이 전부다.
 * pci_stub_init()   : ids 파라미터를 파싱해 dynid 로 등록한다.
 * pci_stub_exit()   : 드라이버를 등록 해제한다.
 * stub_driver       : struct pci_driver. id_table 이 비어 있고 dynid 로만
 *                     장치를 잡는다는 점이 특이하다.
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
	/* [한국어] p = ids 문자열을 자르며 나아갈 커서, id = 잘라 낸 항목 하나.
	 * strsep() 이 두 포인터를 함께 쓰는 관용구다 — 구분자를 널로 바꾸고
	 * 커서를 다음으로 옮기면서 잘라 낸 앞부분을 돌려준다. */
	char *p, *id;
	int rc;	/* [한국어] pci_register_driver 와 pci_add_dynid 의 결과 */

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
