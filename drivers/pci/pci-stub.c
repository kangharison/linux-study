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

#include <linux/module.h>
#include <linux/pci.h>
static char ids[1024] __initdata;

module_param_string(ids, ids, sizeof(ids), 0);

MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the stub driver, format is "
			"\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
			" and multiple comma separated entries can be specified");

static int pci_stub_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	pci_info(dev, "claimed by stub\n");
	return 0;
}

static struct pci_driver stub_driver = {
	.name		= "pci-stub",
	.id_table	= NULL,	/* only dynamic id's */
	.probe		= pci_stub_probe,
	.driver_managed_dma = true,
};

static int __init pci_stub_init(void)
{
	/* [한국어] p = ids 문자열을 자르며 나아갈 커서, id = 잘라 낸 항목 하나.
	 * strsep() 이 두 포인터를 함께 쓰는 관용구다 — 구분자를 널로 바꾸고
	 * 커서를 다음으로 옮기면서 잘라 낸 앞부분을 돌려준다. */
	char *p, *id;
	int rc;	/* [한국어] pci_register_driver 와 pci_add_dynid 의 결과 */

	rc = pci_register_driver(&stub_driver);
	if (rc)
		return rc;

	/* no ids passed actually */
	if (ids[0] == '\0')
		return 0;

	/* add ids specified in the module parameter */
	p = ids;
	while ((id = strsep(&p, ","))) {
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
		int fields;

		if (!strlen(id))
			continue;

		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				&vendor, &device, &subvendor, &subdevice,
				&class, &class_mask);

		if (fields < 2) {
			pr_warn("pci-stub: invalid ID string \"%s\"\n", id);
			continue;
		}

		pr_info("pci-stub: add %04X:%04X sub=%04X:%04X cls=%08X/%08X\n",
		       vendor, device, subvendor, subdevice, class, class_mask);

		rc = pci_add_dynid(&stub_driver, vendor, device,
				   subvendor, subdevice, class, class_mask, 0);
		if (rc)
			pr_warn("pci-stub: failed to add dynamic ID (%d)\n",
				rc);
	}

	return 0;
}

static void __exit pci_stub_exit(void)
{
	pci_unregister_driver(&stub_driver);
}

module_init(pci_stub_init);
module_exit(pci_stub_exit);

MODULE_DESCRIPTION("VM device assignment stub driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Wright <chrisw@sous-sol.org>");
