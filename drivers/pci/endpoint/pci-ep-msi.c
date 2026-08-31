// SPDX-License-Identifier: GPL-2.0
/*
 * PCI Endpoint *Controller* (EPC) MSI library
 *
 * Copyright (C) 2025 NXP
 * Author: Frank Li <Frank.Li@nxp.com>
 */

/*
 * [한국어 설명] 엔드포인트가 호스트를 깨우는 초인종 (pci-ep-msi.c)
 *
 * === 파일의 역할 ===
 * PCI 엔드포인트 모드에서 쓰는 도어벨(doorbell)을 만든다. 100줄 남짓의
 * 작은 파일이고 공개 함수도 둘뿐이지만, 방향을 뒤집어 생각해야 해서
 * 처음에는 헷갈리기 쉽다.
 *
 * 먼저 엔드포인트 모드가 무엇인지부터. 보통 리눅스가 도는 기계는 PCIe
 * 호스트(루트 컴플렉스)라 NVMe 나 랜카드를 꽂아 쓴다. 그런데 SoC 를
 * 반대로 설정해 다른 컴퓨터의 슬롯에 꽂히는 장치처럼 보이게 할 수도
 * 있다. 그것이 엔드포인트 모드이며, DPU 나 SmartNIC 이 그렇게 동작한다.
 *
 * 그때 문제가 되는 것이 "장치가 호스트에게 알리는 방법" 이다. 보통은
 * 엔드포인트가 MSI 를 호스트에 보내면 되지만, 여기서 다루는 것은 그
 * 반대 방향이다 — 호스트가 엔드포인트 안에서 도는 리눅스를 깨우는 것.
 *
 * 방법이 재미있다. 엔드포인트가 자기 SoC 의 인터럽트 컨트롤러에서 MSI
 * 주소를 하나 받아, 그 주소를 BAR 를 통해 호스트에게 노출한다. 호스트가
 * 그 주소에 아무 값이나 쓰면 SoC 의 인터럽트 컨트롤러가 그것을 MSI 쓰기로
 * 인식해 인터럽트를 발생시킨다. 문자 그대로 초인종을 누르는 것이다.
 *
 * 그래서 이 파일이 하는 일은 "SoC 의 플랫폼 MSI 를 여러 개 받아 두고,
 * 각각의 주소를 엔드포인트 함수가 호스트에게 알려 줄 수 있게 보관하는
 * 것" 이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 엔드포인트 함수 드라이버(예: functions/pci-epf-mhi.c)의 bind 단계
 *   -> [이 파일] pci_epf_alloc_doorbell()
 *      -> 디바이스 트리에서 EPC 의 MSI 도메인을 찾는다
 *      -> platform_device_msi_init_and_alloc_irqs() 로 벡터 num_db 개 확보
 *         -> 각 벡터의 주소·데이터가 정해질 때마다
 *            [이 파일] pci_epf_write_msi_msg() 콜백이 불려 값을 보관
 *      -> 각 벡터의 가상 IRQ 번호를 epf->db_msg[i].virq 에 기록
 *   그 뒤 함수 드라이버가 보관된 주소를 BAR 에 실어 호스트에게 노출한다
 *   (그 부분은 이 파일이 아니라 각 함수 드라이버의 몫이다).
 *
 * 실행 컨텍스트: 두 공개 함수 모두 프로세스 컨텍스트. 콜백
 * pci_epf_write_msi_msg() 는 MSI 계층이 벡터를 구성하며 부른다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: endpoint/functions/ 의 함수 드라이버들.
 * 아래쪽: 커널 플랫폼 MSI 계층(platform_device_msi_*), IRQ 도메인,
 *   그리고 디바이스 트리의 msi-parent 연결.
 * 옆쪽: pci-epc-core.c(EPC 조회), pci-epf-core.c(struct pci_epf 정의).
 * 공유 상태: struct pci_epf 의 db_msg 배열과 num_db.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 부르지 않는다(drivers/nvme 트리
 * 전수 확인 — 호출 0건).
 *
 * 그럼에도 NVMe 를 공부하는 사람에게 낯익은 구조다. NVMe 의 도어벨
 * 레지스터가 바로 이 발상이기 때문이다 — 호스트가 SQ 에 명령을 넣고
 * 도어벨에 꼬리 인덱스를 쓰면 컨트롤러가 깨어난다. 여기서는 그 도어벨을
 * MSI 주소로 구현했을 뿐, "약속된 주소에 쓰기 = 상대를 깨움" 이라는
 * 뼈대는 같다.
 *
 * 실제로 이 방식이 쓰이는 곳이 NVMe-oF 타깃을 엔드포인트 모드 SoC 에서
 * 돌리는 구성이다. 다만 그 구현은 이 트리 안에 없으므로 여기서는
 * 인프라만 다룬다.
 *
 * === 주요 함수/구조체 요약 ===
 * pci_epf_write_msi_msg() : MSI 계층이 벡터의 주소·데이터를 정하면 불리는
 *                           콜백. 그 값을 epf->db_msg 에 보관한다.
 *                           이 파일이 존재하는 이유의 절반이 이 보관이다.
 * pci_epf_alloc_doorbell() : 도어벨 num_db 개를 확보한다. 도메인 검증부터
 *                           벡터 할당, virq 기록까지.
 * pci_epf_free_doorbell()  : 그 반대.
 * struct pci_epf_doorbell_msg : 도어벨 하나의 MSI 메시지와 가상 IRQ 번호.
 *                           정의는 include/linux/pci-ep-msi.h 에 있는데,
 *                           이 트리는 부분 체크아웃이라 그 헤더가 없어
 *                           필드 구성을 직접 확인하지는 못했다.
 */

/* [한국어] struct device 와 dev_err 등. epf->dev 와 epc->dev 를 다룬다. */
#include <linux/device.h>
/* [한국어] EXPORT_SYMBOL_GPL. 두 공개 함수를 모듈에서 쓸 수 있게 한다. */
#include <linux/export.h>
/* [한국어] irq_domain_is_msi_parent 등 IRQ 도메인 질의 함수.
 * 아래에서 이 SoC 의 MSI 컨트롤러가 조건을 만족하는지 검사하는 데 쓴다. */
#include <linux/irqdomain.h>
/* [한국어] 모듈 관련 매크로. */
#include <linux/module.h>
/* [한국어] struct msi_desc, struct msi_msg, msi_get_virq 등 MSI 핵심.
 * 이 파일의 주제 자체다. */
#include <linux/msi.h>
/* [한국어] of_msi_map_get_device_domain — 디바이스 트리의 msi-parent
 * 연결을 따라가 이 EPC 가 쓸 MSI 도메인을 찾는다. */
#include <linux/of_irq.h>
/* [한국어] struct pci_epc(엔드포인트 컨트롤러)와 pci_epc_get/put. */
#include <linux/pci-epc.h>
/* [한국어] struct pci_epf(엔드포인트 함수). db_msg 와 num_db 가 여기 있다. */
#include <linux/pci-epf.h>
/* [한국어] configfs 기반 엔드포인트 설정 인터페이스의 선언.
 * 이 파일이 직접 쓰는 심볼은 확인되지 않았으나, 엔드포인트 관련 헤더
 * 묶음으로 함께 포함되어 있다. */
#include <linux/pci-ep-cfs.h>
/* [한국어] 이 파일이 구현하는 두 함수의 선언과
 * struct pci_epf_doorbell_msg 정의. */
#include <linux/pci-ep-msi.h>
/* [한국어] kzalloc_objs / kfree — db_msg 배열 할당. */
#include <linux/slab.h>

/* [한국어]
 * pci_epf_write_msi_msg - 확정된 MSI 주소·데이터를 도어벨 표에 보관한다
 *
 * @desc: 어느 벡터인지. desc->msi_index 가 몇 번째 도어벨인지 알려 준다.
 * @msg: MSI 계층이 정한 주소와 데이터.
 * @return: 없음.
 *
 * MSI 벡터를 할당하면 인터럽트 컨트롤러가 "이 주소에 이 값을 쓰면
 * 그 인터럽트가 발생한다" 를 정해 알려 준다. 이 콜백이 그 통지를 받는
 * 자리다.
 *
 * 보통의 PCI 장치라면 커널이 이 값을 장치의 MSI capability 레지스터에
 * 직접 써 넣는다. 그런데 여기서는 그럴 수 없다. 이 주소를 실제로 쓸
 * 주체가 커널이 아니라 저 건너편의 호스트이기 때문이다.
 *
 * 그래서 레지스터에 쓰는 대신 epf->db_msg 배열에 보관해 둔다. 나중에
 * 엔드포인트 함수 드라이버가 이 값을 BAR 에 실어 호스트에게 알려 주고,
 * 호스트가 그 주소에 쓰면 인터럽트가 발생한다.
 *
 * epc->pci_epf 목록의 첫 항목만 보는 것은 아직 EPF 하나만 지원하기
 * 때문이다(아래 alloc 의 TODO 주석 참고).
 *
 * 실행 컨텍스트: MSI 계층이 벡터를 구성하는 중에 부른다. 아래
 * platform_device_msi_init_and_alloc_irqs() 호출 안에서 num_db 번 불린다.
 *
 * 에러 경로: EPC 를 못 찾거나 조건이 안 맞으면 조용히 아무것도 하지
 *   않는다. void 콜백이라 오류를 전할 방법이 없고, 그 경우 db_msg 가
 *   비어 있어 호스트가 도어벨을 쓸 수 없게 될 뿐이다.
 *
 * 호출 체인:
 *   pci_epf_alloc_doorbell() → platform_device_msi_init_and_alloc_irqs()
 *     → (MSI 계층) → [이 함수]
 */
static void pci_epf_write_msi_msg(struct msi_desc *desc, struct msi_msg *msg)
{
	struct pci_epc *epc;
	struct pci_epf *epf;

	/* [한국어] 이 MSI 를 소유한 device 의 이름으로 EPC 를 찾는다.
	 * 콜백은 desc 만 받으므로 여기서 거슬러 올라가야 한다.
	 * pci_epc_get 은 참조 카운트를 올리므로 아래에서 반드시 put 해야 한다. */
	epc = pci_epc_get(dev_name(msi_desc_to_dev(desc)));
	if (IS_ERR(epc))
		return;

	/* [한국어] 그 EPC 에 붙은 첫 엔드포인트 함수를 가져온다.
	 * 아직 EPF 하나만 지원하므로 첫 항목이 곧 그 함수다. */
	epf = list_first_entry_or_null(&epc->pci_epf, struct pci_epf, list);

	/* [한국어] 세 조건을 모두 확인한다.
	 *   epf        — 붙은 함수가 있는가
	 *   epf->db_msg — 도어벨 배열이 할당되어 있는가
	 *   msi_index < num_db — 이 벡터가 우리가 요청한 범위 안인가
	 * 마지막 검사가 배열 범위를 벗어난 쓰기를 막는다. 이 콜백은 MSI
	 * 계층이 부르는 것이라, 우리가 요청하지 않은 벡터에 대해 불릴
	 * 가능성을 배제할 수 없다. */
	if (epf && epf->db_msg && desc->msi_index < epf->num_db)
		/* [한국어] 주소와 데이터를 그대로 복사해 둔다. 나중에 함수
		 * 드라이버가 이 값을 호스트에게 노출한다. */
		memcpy(&epf->db_msg[desc->msi_index].msg, msg, sizeof(*msg));

	/* [한국어] 위 get 이 올린 참조를 되돌린다. 조건이 안 맞아 아무것도
	 * 하지 않았더라도 반드시 거쳐야 하는 경로다. */
	pci_epc_put(epc);
}

/* [한국어]
 * pci_epf_alloc_doorbell - 호스트가 누를 초인종 num_db 개를 마련한다
 *
 * @epf: 이 도어벨을 쓸 엔드포인트 함수.
 * @num_db: 필요한 도어벨 개수.
 * @return: 0 이면 성공. -EINVAL(EPF 가 여럿), -EBUSY(이미 할당됨),
 *   -ENODEV(MSI 도메인 조건 불만족), -ENOMEM, 그 밖에 MSI 할당 실패값.
 *
 * 하는 일이 네 단계다.
 *   1) 전제 확인 — EPF 가 하나인지, 이미 할당하지 않았는지.
 *   2) MSI 도메인 찾기와 검증 — 이 SoC 의 인터럽트 컨트롤러가 이 용도로
 *      쓸 수 있는지 세 가지를 확인한다(아래 각 검사의 주석 참고).
 *   3) 벡터 할당 — 그 과정에서 위 콜백이 num_db 번 불려 주소가 보관된다.
 *   4) 가상 IRQ 번호 기록 — 함수 드라이버가 request_irq 할 때 쓴다.
 *
 * 2단계의 검증이 이 함수에서 가장 덜 자명한 부분이다. 도어벨의 MSI
 * 주소는 호스트에게 알려 준 뒤 오래 유지되어야 하는데, 커널이 그 주소를
 * 나중에 바꿔 버리면(예: CPU 친화도 변경으로 벡터를 옮기면) 호스트가
 * 가진 주소가 낡은 것이 된다. 호스트에게 다시 알려 줄 방법이 없으므로
 * 주소가 절대 바뀌지 않는 컨트롤러여야만 한다. immutable 검사가 그것이다.
 *
 * 실행 컨텍스트: 함수 드라이버의 bind 단계 — 프로세스 컨텍스트.
 *   메모리 할당과 IRQ 할당이 있어 잠들 수 있다.
 *
 * 에러 경로: 각 단계에서 되돌릴 것만 되돌리고 나간다. 특히 MSI 할당이
 *   실패하면 앞서 잡은 배열과 기록해 둔 num_db 까지 되돌린다 —
 *   db_msg 가 NULL 이 아닌 채로 남으면 다음 호출이 -EBUSY 로 막힌다.
 *
 * 호출 체인:
 *   엔드포인트 함수 드라이버의 bind → [이 함수]
 *     → of_msi_map_get_device_domain() → platform_device_msi_init_and_alloc_irqs()
 *       → pci_epf_write_msi_msg() (벡터마다)
 */
int pci_epf_alloc_doorbell(struct pci_epf *epf, u16 num_db)
{
	/* [한국어] 이 함수가 붙어 있는 엔드포인트 컨트롤러. MSI 도메인은
	 * EPC 의 부모 device(실제 하드웨어)에 딸려 있다. */
	struct pci_epc *epc = epf->epc;
	/* [한국어] 오류 메시지를 낼 device. 함수 쪽 이름이 찍히도록 epf->dev 를 쓴다. */
	struct device *dev = &epf->dev;
	/* [한국어] 이 EPC 하드웨어가 쓸 MSI 도메인. 아래에서 디바이스 트리로 찾는다. */
	struct irq_domain *domain;
	/* [한국어] 도어벨 배열. void * 로 받았다가 epf->db_msg 에 대입한다. */
	void *msg;
	/* [한국어] MSI 할당 결과이자 이 함수의 반환값. */
	int ret;
	/* [한국어] virq 기록 루프의 인덱스. */
	int i;

	/* TODO: Multi-EPF support */
	/* [한국어] 상류 주석대로 아직 EPF 하나만 지원한다. 위 콜백이
	 * 목록의 첫 항목만 보기 때문에, 두 번째 함수가 도어벨을 요청하면
	 * 첫 함수의 배열에 값이 들어가는 사고가 난다. 그래서 여기서 막는다. */
	if (list_first_entry_or_null(&epc->pci_epf, struct pci_epf, list) != epf) {
		dev_err(dev, "MSI doorbell doesn't support multiple EPF\n");
		return -EINVAL;
	}

	/* [한국어] 이미 할당되어 있으면 거절한다. 두 번 할당하면 앞의 배열이
	 * 새는 데다, 호스트가 아는 주소와 어긋나게 된다. */
	if (epf->db_msg)
		return -EBUSY;

	/* [한국어] 디바이스 트리의 msi-parent 연결을 따라가 이 EPC 하드웨어가
	 * 쓸 MSI 도메인을 찾는다. DOMAIN_BUS_PLATFORM_MSI 를 지정하는 것은
	 * PCI MSI 가 아니라 플랫폼 MSI 를 원한다는 뜻이다 — 여기서 인터럽트를
	 * 받는 것은 PCI 장치가 아니라 SoC 자신이기 때문이다. */
	domain = of_msi_map_get_device_domain(epc->dev.parent, 0,
					      DOMAIN_BUS_PLATFORM_MSI);
	if (!domain) {
		/* [한국어] 디바이스 트리에 이 EPC 노드의 msi-parent 가 없거나
		 * 그 대상이 아직 준비되지 않은 경우다. 설정 문제일 가능성이
		 * 높아 메시지를 남긴다. */
		dev_err(dev, "Can't find MSI domain for EPC\n");
		return -ENODEV;
	}

	/* [한국어] 찾은 도메인이 MSI 부모 도메인인지 확인한다. 아니라면
	 * 그 아래에 벡터를 요청할 수 없다. 오류 메시지 없이 물러나는데,
	 * 디바이스 트리 구성 문제라기보다 이 SoC 가 애초에 해당하지 않는
	 * 경우라 보기 때문으로 읽힌다. */
	if (!irq_domain_is_msi_parent(domain))
		return -ENODEV;

	/* [한국어] 이 함수에서 가장 중요한 검사다. immutable 은 한 번 정해진
	 * MSI 주소와 데이터가 이후 바뀌지 않음을 뜻한다.
	 *
	 * 왜 그래야 하는가 — 이 주소를 저 건너편 호스트에게 알려 주고 나면
	 * 커널이 그것을 회수하거나 갱신할 방법이 없다. 보통의 MSI 는 인터럽트
	 * 친화도를 바꾸면 주소가 달라질 수 있는데, 그러면 호스트는 여전히
	 * 옛 주소에 쓰게 되고 초인종이 울리지 않는다.
	 * 그런 컨트롤러는 이 용도로 쓸 수 없으므로 여기서 거절한다. */
	if (!irq_domain_is_msi_immutable(domain)) {
		dev_err(dev, "Mutable MSI controller not supported\n");
		return -ENODEV;
	}

	/* [한국어] 찾은 도메인을 EPC 하드웨어 device 에 연결한다. 이렇게 해야
	 * 아래 platform_device_msi_* 호출이 이 도메인에서 벡터를 가져온다. */
	dev_set_msi_domain(epc->dev.parent, domain);

	/* [한국어] 도어벨 num_db 개분의 배열을 잡는다. 0 으로 초기화하는
	 * kzalloc 계열이라, 콜백이 채우기 전에는 주소가 0 으로 남는다. */
	msg = kzalloc_objs(struct pci_epf_doorbell_msg, num_db);
	if (!msg)
		return -ENOMEM;

	/* [한국어] 아래 벡터 할당 중에 콜백이 불리는데, 그 콜백이 num_db 와
	 * db_msg 를 보고 판단하므로 반드시 할당 전에 채워 두어야 한다.
	 * 순서가 뒤바뀌면 콜백이 매번 조건에 걸려 아무것도 보관하지 못한다. */
	epf->num_db = num_db;
	epf->db_msg = msg;

	/* [한국어] 실제로 벡터 num_db 개를 할당한다. 세 번째 인자가 위 콜백이며,
	 * 벡터마다 주소가 정해질 때 불려 db_msg 에 값을 채운다. */
	ret = platform_device_msi_init_and_alloc_irqs(epc->dev.parent, num_db,
						      pci_epf_write_msi_msg);
	if (ret) {
		dev_err(dev, "Failed to allocate MSI\n");
		/* [한국어] 방금 잡은 배열을 해제하고, 위에서 미리 채워 둔 두
		 * 필드도 되돌린다. db_msg 를 NULL 로 되돌리지 않으면 다음
		 * 호출이 -EBUSY 로 막혀 영영 도어벨을 만들 수 없게 된다. */
		kfree(msg);
		epf->db_msg = NULL;
		epf->num_db = 0;
		return ret;
	}

	/* [한국어] 각 벡터의 가상 IRQ 번호를 기록한다. 함수 드라이버가
	 * request_irq() 로 핸들러를 걸 때 이 번호를 쓴다.
	 * 주소는 호스트가 쓸 것이고, virq 는 이쪽에서 받을 것이라
	 * 도어벨 하나에 두 정보가 다 필요하다. */
	for (i = 0; i < num_db; i++)
		epf->db_msg[i].virq = msi_get_virq(epc->dev.parent, i);

	/* [한국어] 여기까지 왔으면 ret 은 0 이다. 성공 경로에서 ret 을 그대로
	 * 돌려주는 형태라, 위 실패 검사를 통과했다는 사실이 곧 성공을 뜻한다. */
	return ret;
}
EXPORT_SYMBOL_GPL(pci_epf_alloc_doorbell);

/* [한국어]
 * pci_epf_free_doorbell - 도어벨을 전부 반납한다
 *
 * @epf: 도어벨을 쓰던 엔드포인트 함수.
 * @return: 없음.
 *
 * alloc 의 반대다. 순서가 중요한데, 벡터를 먼저 반납한 뒤에 배열을
 * 해제해야 한다. 반대로 하면 MSI 계층이 정리 중에 콜백을 부를 경우
 * 이미 해제된 배열을 건드리게 된다.
 *
 * 참고로 이 함수는 도어벨이 할당되지 않은 상태에서 불려도 큰 문제가
 * 없다 — kfree(NULL) 은 무해하고, free_irqs_all 도 벡터가 없으면
 * 할 일이 없다. 다만 이 트리에서 그렇게 부르는 곳은 확인되지 않았다.
 *
 * 실행 컨텍스트: 함수 드라이버의 unbind 단계 — 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   엔드포인트 함수 드라이버의 unbind → [이 함수]
 *     → platform_device_msi_free_irqs_all()
 */
void pci_epf_free_doorbell(struct pci_epf *epf)
{
	/* [한국어] 벡터를 먼저 반납한다. 이 호출이 끝나야 콜백이 더는
	 * 불리지 않으므로, 그다음에 배열을 해제해도 안전하다. */
	platform_device_msi_free_irqs_all(epf->epc->dev.parent);

	/* [한국어] 보관하던 주소 배열을 해제한다. */
	kfree(epf->db_msg);
	/* [한국어] NULL 로 되돌려 "도어벨 없음" 상태로 만든다.
	 * 이렇게 해야 나중에 alloc 을 다시 부를 수 있다(그쪽의 -EBUSY 검사). */
	epf->db_msg = NULL;
	/* [한국어] 개수도 0 으로. 콜백의 범위 검사가 이 값을 보므로,
	 * 남겨 두면 해제된 배열에 쓰는 경로가 열릴 수 있다. */
	epf->num_db = 0;
}
EXPORT_SYMBOL_GPL(pci_epf_free_doorbell);
