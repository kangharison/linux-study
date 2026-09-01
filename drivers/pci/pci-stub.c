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

/* [한국어] 모듈 파라미터 ids 를 문자열로 등록한다. 권한이 0 이라 sysfs 에 노출되지 않고
 * 부팅/적재 시점에만 지정할 수 있다 — 나중에 장치를 더하려면 new_id 를 쓰라는 뜻이다. */
module_param_string(ids, ids, sizeof(ids), 0);

MODULE_PARM_DESC(ids, "Initial PCI IDs to add to the stub driver, format is "
			"\"vendor:device[:subvendor[:subdevice[:class[:class_mask]]]]\""
			" and multiple comma separated entries can be specified");

/* [한국어]
 * pci_stub_probe - 장치를 소유만 하고 아무 일도 하지 않는다
 *
 * @dev: 동적으로 등록된 ID 와 매칭된 장치.
 * @id: 매칭에 쓰인 항목. 사용하지 않는다.
 * @return: 항상 0(성공).
 *
 * 왜 필요한가: 장치를 게스트 VM 에 통째로 넘기려면(VFIO 패스스루) 호스트
 * 커널의 실제 드라이버가 그 장치를 잡고 있으면 안 된다. 그렇다고 아무 드라이버도
 * 없으면 부팅 중 원래 드라이버가 먼저 바인딩되어 버린다. 그래서 "소유권만
 * 주장하고 하드웨어는 건드리지 않는" 드라이버가 필요하고, 이 함수가 그 본체다.
 * pci-pf-stub 과 목적이 비슷하지만 대상이 다르다 — 이쪽은 사용자가 지정한
 * 임의의 장치이고, 저쪽은 SR-IOV PF 다.
 *
 * 동작 과정: 정보 로그 한 줄을 남기고 성공을 돌려준다. 레지스터를 읽지도
 * 인터럽트를 등록하지도 않으므로, 이 드라이버가 붙은 장치는 완전히 조용하다.
 *
 * 실행 컨텍스트: PCI 코어의 드라이버 바인딩 경로, 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   사용자가 new_id 쓰기 또는 모듈 파라미터 ids → pci_add_dynid()
 *     → PCI 코어의 재매칭 → pci_device_probe() → [pci_stub_probe] */
static int pci_stub_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	pci_info(dev, "claimed by stub\n");
	return 0;
}

/* [한국어] PCI 드라이버 서술. 이 구조체의 특이점은 id_table 이 NULL 이라는 것이다. */
static struct pci_driver stub_driver = {
	/* [한국어] 드라이버 이름 — sysfs 의 /sys/bus/pci/drivers/pci-stub 경로가 된다. */
	.name		= "pci-stub",
	.id_table	= NULL,	/* only dynamic id's */
	.probe		= pci_stub_probe,
	.driver_managed_dma = true,
};

/* [한국어]
 * pci_stub_init - 드라이버를 등록하고 모듈 파라미터의 ID 목록을 풀어 넣는다
 *
 * @return: 0 = 성공. 음수 = pci_register_driver() 실패.
 *      ID 하나를 추가하다 실패해도 전체를 실패로 만들지 않는다.
 *
 * 왜 파라미터로 ID 를 받는가: stub_driver 의 id_table 은 NULL 이라 정적으로
 * 매칭되는 장치가 하나도 없다. 잡을 장치를 사용자가 정해야 하기 때문이며,
 * 그 경로가 둘이다 — 부팅 시 모듈 파라미터 ids, 그리고 나중에 sysfs 의
 * new_id 파일. 이 함수는 앞의 것을 처리한다.
 *
 * 동작 과정:
 *   1) 드라이버를 등록한다. 이 시점에는 매칭되는 장치가 없으므로 아무 일도
 *      일어나지 않는다.
 *   2) ids 문자열이 비어 있으면 그대로 성공.
 *   3) strsep() 으로 쉼표 단위로 잘라 가며 순회한다. strsep 은 구분자를
 *      널로 바꾸고 커서를 다음으로 옮기면서 잘라 낸 앞부분을 돌려주는 방식이라,
 *      원본 배열을 그 자리에서 쪼갠다.
 *   4) 각 항목을 sscanf 로 최대 6개 필드로 파싱한다. vendor 와 device 는
 *      필수이고 나머지 넷은 선택이라, fields < 2 면 경고만 남기고 건너뛴다.
 *      지정하지 않은 subvendor/subdevice 는 PCI_ANY_ID 로 남아 어떤 값과도
 *      매칭되고, class/class_mask 는 0 이라 클래스 조건이 없다는 뜻이 된다.
 *   5) pci_add_dynid() 로 동적 ID 를 등록한다. 이 호출이 곧바로 재매칭을
 *      유발하므로, 해당 장치가 이미 다른 드라이버에 잡혀 있지 않다면
 *      이 시점에 pci_stub_probe() 가 불린다.
 *
 * 실행 컨텍스트: 모듈 적재 경로(__init), 프로세스 컨텍스트.
 *
 * 에러 경로: 드라이버 등록 실패만 전체 실패로 만든다. 개별 ID 파싱·추가
 * 실패는 경고를 남기고 계속한다 — 잘못된 항목 하나 때문에 나머지 전부를
 * 버리지 않겠다는 판단이다. 다만 그렇게 실패한 뒤에도 0 을 돌려주므로,
 * 사용자는 dmesg 를 봐야만 어떤 ID 가 등록되지 않았는지 알 수 있다.
 *
 * 호출 체인:
 *   module_init → [pci_stub_init] → pci_register_driver()
 *     → strsep()/sscanf() → pci_add_dynid() → (매칭되면) pci_stub_probe() */
static int __init pci_stub_init(void)
{
	/* [한국어] p = ids 문자열을 자르며 나아갈 커서, id = 잘라 낸 항목 하나.
	 * strsep() 이 두 포인터를 함께 쓰는 관용구다 — 구분자를 널로 바꾸고
	 * 커서를 다음으로 옮기면서 잘라 낸 앞부분을 돌려준다. */
	char *p, *id;
	int rc;	/* [한국어] pci_register_driver 와 pci_add_dynid 의 결과 */

	rc = pci_register_driver(&stub_driver);
	if (rc)
		/* [한국어] 드라이버 등록 실패는 되돌릴 것이 없으므로 그대로 전달한다. */
		return rc;

	/* no ids passed actually */
	if (ids[0] == '\0')
		return 0;

	/* add ids specified in the module parameter */
	p = ids;
	/* [한국어] 쉼표 단위로 잘라 가며 순회한다. strsep 은 p 가 가리키는 커서를 직접 옮기고
	 * 구분자를 널로 바꾸므로, 원본 ids 배열을 그 자리에서 쪼갠다.
	 * strtok 과 달리 전역 상태를 쓰지 않아 재진입에 안전하다. */
	while ((id = strsep(&p, ","))) {
		/* [한국어] 파싱 결과를 담을 변수들. subvendor/subdevice 를 PCI_ANY_ID 로 초기화해 두면,
		 * 사용자가 그 필드를 생략했을 때 "어떤 값이든 매칭"이 된다. */
		unsigned int vendor, device, subvendor = PCI_ANY_ID,
			subdevice = PCI_ANY_ID, class = 0, class_mask = 0;
		/* [한국어] sscanf 가 채운 필드 개수. */
		int fields;

		/* [한국어] 쉼표가 연달아 나오면 빈 문자열이 생긴다. */
		if (!strlen(id))
			continue;

		/* [한국어] 최대 6개 필드를 16진수로 읽는다. 앞의 둘(vendor, device)만 필수이고
		 * 나머지는 있으면 쓰고 없으면 초기값을 유지한다. */
		fields = sscanf(id, "%x:%x:%x:%x:%x:%x",
				&vendor, &device, &subvendor, &subdevice,
				&class, &class_mask);

		/* [한국어] 필수 두 필드조차 못 읽었다면 형식이 잘못된 것이다. */
		if (fields < 2) {
			/* [한국어] 어떤 문자열이 잘못됐는지 그대로 찍어 사용자가 고칠 수 있게 한다. */
			pr_warn("pci-stub: invalid ID string \"%s\"\n", id);
			continue;
		}

		/* [한국어] 실제로 등록할 값을 모두 찍는다. 생략된 필드가 어떤 기본값으로 채워졌는지
		 * 확인할 수 있어 진단에 유용하다. */
		pr_info("pci-stub: add %04X:%04X sub=%04X:%04X cls=%08X/%08X\n",
		       vendor, device, subvendor, subdevice, class, class_mask);

		/* [한국어] 동적 ID 를 드라이버에 추가한다. 마지막 인자 0 은 driver_data 로,
		 * probe 가 id 를 쓰지 않으므로 의미가 없다.
		 * 이 호출이 곧바로 재매칭을 유발해, 조건에 맞는 장치가 이미 있고 다른
		 * 드라이버에 잡혀 있지 않다면 여기서 pci_stub_probe() 가 불린다. */
		rc = pci_add_dynid(&stub_driver, vendor, device,
				   subvendor, subdevice, class, class_mask, 0);
		/* [한국어] 추가 실패 검사. */
		if (rc)
			/* [한국어] [상류 코드 관찰] 경고만 남기고 계속한다. 잘못된 항목 하나 때문에 나머지를
			 * 버리지 않겠다는 판단이지만, 최종 반환은 0 이므로 사용자는 dmesg 를 봐야만
			 * 어떤 ID 가 등록되지 않았는지 알 수 있다. */
			pr_warn("pci-stub: failed to add dynamic ID (%d)\n",
				rc);
	}

	return 0;
}

/* [한국어]
 * pci_stub_exit - 드라이버를 해제한다
 *
 * pci_unregister_driver() 한 줄이 전부다. 그 안에서 이 드라이버가 잡고 있던
 * 모든 장치의 언바인드가 일어나고, pci_add_dynid() 로 등록해 둔 동적 ID 목록도
 * 함께 해제된다 — 그래서 이 파일에 ID 해제 코드가 따로 없다.
 *
 * 언바인드된 장치는 다시 매칭 대상이 되므로, 원래 드라이버가 적재되어 있으면
 * 그쪽이 장치를 가져간다. stub 을 내리는 것이 곧 "장치를 돌려주는" 일이다.
 *
 * 실행 컨텍스트: 모듈 해제 경로(__exit), 프로세스 컨텍스트.
 * 장치들의 remove 콜백이 연쇄로 불리므로 잠들 수 있다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   module_exit → [pci_stub_exit] → pci_unregister_driver()
 *     → 각 장치 언바인드 → 동적 ID 목록 해제 */
static void __exit pci_stub_exit(void)
{
	pci_unregister_driver(&stub_driver);
}

module_init(pci_stub_init);
module_exit(pci_stub_exit);

MODULE_DESCRIPTION("VM device assignment stub driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Wright <chrisw@sous-sol.org>");
