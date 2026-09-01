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
 * 엔드포인트다. 칩 내부에 있어 바깥으로 나가는 PCIe 링크가 없다. 그래서
 * 문제가 생긴다 — 오류 보고(AER)와 전원 이벤트(PME)는 상류 포트로 메시지를
 * 보내는 방식인데, 보낼 링크가 없기 때문이다.
 * RCEC(Root Complex Event Collector)가 그 해법이다. RCiEP 들을 대신해
 * 이벤트를 모아 보고해 주는 전용 function 이며, 어느 RCiEP 들을 담당하는지를
 * 자기 capability(RCEC Endpoint Association)에 들고 있다.
 * 이 파일이 다루는 것은 오직 그 담당 관계다. 이벤트 자체를 처리하지는 않고,
 * "이 RCEC 는 누구를 담당하는가"(순회)와 "이 RCiEP 는 누가 담당하는가"(포인터)
 * 두 방향의 연결만 만들어 준다. 실제 처리는 aer.c 와 pme.c 의 몫이다.
 * 담당 관계의 표현이 두 겹이라는 점이 이 파일의 구조를 결정한다.
 * 하나는 버스 범위 — Next Bus / Last Bus 필드가 [시작, 끝] 구간을 정한다.
 * 다른 하나는 비트맵 — RCEC 자기 버스에 한해 32비트로 device 번호를 표시한다.
 * 그래서 판정도 두 단계로 나뉜다. walk_rcec() 이 어느 버스를 훑을지 정하고,
 * rcec_assoc_rciep() 이 그 안에서 어느 장치가 담당인지 정한다.
 * 그 분담을 알고 보면 rcec_assoc_rciep() 의 "버스가 다르면 무조건 담당" 이라는
 * 규칙이 이해된다 — 버스 단위 판정은 순회 쪽이 이미 끝냈기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 열거: probe.c 가 장치를 발견할 때마다
 *         → [이 파일] pci_rcec_init() — RCEC 면 Association capability 를
 *            읽어 dev->rcec_ea 에 캐시한다. 매 이벤트마다 config 를 읽지
 *            않으려는 것으로, 오류 처리 경로의 비용을 줄인다.
 * 연결: RCEC 가 등록될 때
 *         → [이 파일] pcie_link_rcec() → 담당 RCiEP 들의 dev->rcec 를 채운다.
 *            하드웨어에 없는 역방향 관계를 소프트웨어로 만드는 작업이다.
 * 이벤트: RCEC 에 AER 또는 PME 인터럽트
 *         → aer.c / pme.c 가 "누구의 이벤트인가" 를 알아야 한다
 *            → [이 파일] pcie_walk_rcec() 로 담당 RCiEP 들에 콜백 적용
 * 해제: pci_rcec_exit() 이 캐시를 놓고 포인터를 지운다.
 * 실행 컨텍스트: 열거·연결은 프로세스 컨텍스트, 이벤트 처리는 AER 스레드
 * 핸들러나 PME 처리 경로다. 어느 쪽이든 순회 중에는 pci_walk_bus() 가
 * pci_bus_sem 을 읽기로 쥐므로, 사용자 콜백도 그 제약 아래에서 돈다.
 *
 * === 타 모듈과의 연결 ===
 * 위쪽: probe.c(열거), pcie/aer.c(오류 이벤트 — aer_cxl_rch.c 의 CXL 경로도
 * pcie_walk_rcec() 을 쓴다), pcie/pme.c(전원 이벤트), pcie/portdrv.c.
 * 아래쪽: bus.c 의 pci_walk_bus()(그 안의 __pci_walk_bus 는 :506),
 * pci_find_bus(), access.c 의 config 접근, pci_find_ext_capability().
 * 옆쪽: pci.h 의 struct rcec_ea(:2141)와 이 파일 함수들의 원형(:2185 부근).
 * 공유 상태: struct pci_dev 의 두 필드가 방향이 반대인 한 쌍을 이룬다.
 * rcec_ea 는 RCEC 쪽이 들고 있는 담당 범위 캐시이고, rcec 는 RCiEP 쪽이
 * 들고 있는 "나를 담당하는 RCEC" 포인터다. 전자는 이 파일이 config 에서
 * 읽어 만들고, 후자는 이 파일이 순회하며 심는다.
 * 규격 근거: RCEC Endpoint Association capability(PCIe 5.0-1, 7.9.10)와
 * 그 안의 7.9.10.3(버스 범위에 자기 버스가 들어오면 연관 없음).
 *
 * === 주요 함수/구조체 요약 ===
 * - struct walk_rcec_data: 순회 하나를 담는 스택 구조체. rcec 는 두 진입점이
 *   모두 채우고, user_callback/user_data 는 pcie_walk_rcec() 만 채운다.
 *   link_rcec_helper() 가 그 두 필드를 보지 않으므로 NULL 이어도 안전하다.
 * - pci_rcec_init(): 열거 시 1회. 비트맵과 버스 범위를 읽어 캐시한다.
 *   BUSN 레지스터가 없는 구형 capability 에는 nextbusn=0xff, lastbusn=0x00 을
 *   일부러 넣는데, 그 값이 walk_rcec() 의 조기 반환 조건과 정확히 맞아
 *   순회 쪽에서 버전을 다시 확인하지 않아도 된다. rcec_ea 대입을 맨 마지막에
 *   두는 것도 의도적이다 — NULL 이 아님이 곧 "캐시 완성" 의 표시다.
 * - pci_rcec_exit(): 캐시를 놓고 포인터를 지운다. 타입 검사가 없는 이유는
 *   RCEC 가 아니었으면 rcec_ea 가 NULL 이라 kfree(NULL) 로 안전하기 때문이다.
 * - pcie_link_rcec(): RCiEP → RCEC 방향의 포인터를 심는다. 하드웨어에는
 *   그 방향이 없어 소프트웨어로 만들어 두는 것이다.
 * - pcie_walk_rcec(): RCEC → RCiEP 방향으로 콜백을 뿌린다. AER/PME 처리의
 *   진입점이다. 위 함수와 짝을 이룬다.
 * - walk_rcec(): 두 진입점이 공유하는 순회 본체. 자기 버스를 먼저 훑고,
 *   그 다음 [nextbusn, lastbusn] 범위의 버스들을 훑는다. 자기 버스가 범위에
 *   들어오면 건너뛰고, 실재하지 않는 버스 번호도 건너뛴다.
 * - rcec_assoc_rciep(): 담당 판정의 아래쪽 절반. 같은 버스면 비트맵으로
 *   가리고, 다른 버스면 무조건 담당이다(범위 검사는 순회 쪽이 끝냈으므로).
 * - link_rcec_helper() / walk_rcec_helper(): 구조가 같고 마지막 한 줄만
 *   다른 두 콜백. 전자는 포인터를 기록하고 후자는 사용자 콜백을 부른다.
 * - [상류 코드 관찰] pcie_walk_rcec() 의 문서 주석은 "@cb 가 0 이 아닌 값을
 *   반환하면 중단한다" 고 적었으나 그 동작은 구현되어 있지 않다.
 *   pci_walk_bus() 에 전달되는 콜백은 사용자 콜백이 아니라
 *   walk_rcec_helper() 이고, 그것이 사용자 콜백의 반환값을 버린 채 언제나
 *   0 을 돌려주기 때문이다. 게다가 walk_rcec() 은 여러 버스를 도는 루프라
 *   한 버스에서 멈추더라도 다음 버스로 넘어간다. 코드는 고치지 않았다.
 *
 * === NVMe 관점 ===
 * NVMe 드라이버는 이 파일의 함수를 직접 부르지 않는다(전수 확인).
 * 관련이 생기는 경우는 NVMe 컨트롤러가 RCiEP 로 구현된 시스템이다. SoC 에
 * NVMe 컨트롤러를 통합한 임베디드 플랫폼이나 일부 서버 칩셋에서 그런 구성이
 * 나타난다. 그 경우 NVMe 의 AER 오류와 PME 신호가 RCEC 를 거쳐 보고되므로,
 * 이 파일이 담당 관계를 만들어 두지 않으면 그 이벤트가 커널에 닿지 않는다.
 * 다만 흔한 구성은 아니다. 카드 형태나 M.2 로 꽂는 NVMe SSD 는
 * PCI_EXP_TYPE_ENDPOINT 이고 자기 링크를 갖는다. 그런 장치는 RCEC 와 무관하며,
 * 오류도 자기 상류 포트로 직접 보고한다.
 * (기존 주석은 "NVMe 장치의 surprise removal" 이 RCEC 를 거친다고 적었으나,
 *  RCiEP 는 링크가 없어 물리적으로 뽑을 수 없다. surprise removal 은 슬롯이
 *  있는 하류 포트에서만 일어나는 일이다.)
 */

/* [한국어] for_each_set_bit() 등 기본 커널 유틸. */
#include <linux/kernel.h>
/* [한국어] struct pci_dev / pci_bus, PCI_SLOT(), pci_pcie_type(), pci_walk_bus(),
 * pci_find_bus(), pci_find_ext_capability(). */
#include <linux/pci.h>
/* [한국어] PCI_EXT_CAP_ID_RCEC, PCI_RCEC_RCIEP_BITMAP, PCI_RCEC_BUSN,
 * PCI_RCEC_BUSN_VER 계열 등 규격이 정한 오프셋과 필드 매크로. */
#include <linux/pci_regs.h>

/* [한국어] struct rcec_ea(:2141)와 pcie_link_rcec() / pcie_walk_rcec() /
 * pci_rcec_init() / pci_rcec_exit() 의 원형. */
#include "../pci.h"

struct walk_rcec_data {
	/* [한국어] 이번 순회의 주체인 RCEC.
	 * 설정자: pcie_link_rcec() 과 pcie_walk_rcec() 이 스택 위에서 채운다.
	 * 읽는 자: 두 헬퍼가 rcec_assoc_rciep() 에 넘길 기준으로 쓴다.
	 * 값 범위: 언제나 유효한 RCEC. rcec_ea 가 NULL 이 아님도 보장된다 —
	 *   두 진입점이 그것을 먼저 검사하기 때문이다.
	 * 동기화: 스택 변수라 이 순회 하나에만 속한다. 공유되지 않는다. */
	struct pci_dev *rcec;
	/* [한국어] 사용자가 각 RCiEP 에 적용하고자 하는 콜백.
	 * 설정자: pcie_walk_rcec() 은 인자로 받은 콜백을, pcie_link_rcec() 은
	 *   NULL 을 넣는다.
	 * 읽는 자: walk_rcec_helper() 만 부른다. link_rcec_helper() 는 자기 일을
	 *   직접 하므로 이 필드를 보지 않는다 — 그래서 NULL 이어도 안전하다.
	 * 값 범위: 유효한 함수 포인터 또는 NULL.
	 * 동기화: 위와 같다. */
	int (*user_callback)(struct pci_dev *dev, void *data);
	/* [한국어] 그 콜백에 함께 넘길 사용자 데이터.
	 * 설정자/읽는 자: user_callback 과 짝을 이룬다.
	 * 값 범위: 임의의 포인터. 의미는 콜백이 정한다.
	 * 동기화: 위와 같다. */
	void *user_data;
};

/* [한국어]
 * rcec_assoc_rciep - 이 RCiEP 가 그 RCEC 의 담당인지 판정한다
 *
 * @rcec: 기준이 되는 RCEC. rcec_ea 캐시가 채워져 있어야 한다.
 * @rciep: 판정할 RCiEP.
 * @return: true = 이 RCEC 가 담당함, false = 아님.
 *
 * 담당 관계 판정의 실제 규칙이 들어 있는 함수다.
 *
 * 두 갈래인데, 첫째가 언뜻 이상해 보인다 — 버스가 다르면 무조건 true 다.
 * 근거는 호출 구조에 있다. 이 함수를 부르는 콜백들은 walk_rcec() 이 훑는
 * 버스에서만 만나는 장치를 넘겨받고, walk_rcec() 은 자기 버스와
 * [nextbusn, lastbusn] 범위 안의 버스만 훑는다. 그러므로 "다른 버스에서
 * 만났다" 는 것은 곧 "범위 안의 버스다" 라는 뜻이다. 버스 단위 판정은
 * 순회 쪽이 이미 끝냈고, 이 함수는 같은 버스일 때만 추가로 판정한다.
 *
 * 둘째 갈래가 그 추가 판정이다. RCEC 와 같은 버스에 있는 RCiEP 는 비트맵으로
 * 가린다. Association Bitmap 의 비트 n 이 device 번호 n 을 뜻한다는 규격의
 * 약속을 for_each_set_bit 순회로 구현한다.
 *
 * 실행 컨텍스트: 두 순회 헬퍼 안. pci_bus_sem 을 읽기로 쥔 상태이며
 * 잠들지 않는다.
 *
 * 에러 경로: 없다. rcec->rcec_ea 가 NULL 이 아님은 두 진입점과 walk_rcec()
 * 이 이미 보장한다.
 *
 * 호출 체인:
 *   link_rcec_helper() / walk_rcec_helper() → [이 함수] → for_each_set_bit()
 */
static bool rcec_assoc_rciep(struct pci_dev *rcec, struct pci_dev *rciep)
{
	/* [한국어] 담당 범위 비트맵을 지역 변수로 옮긴다. for_each_set_bit() 이
	 * unsigned long 포인터를 받기 때문에 타입을 맞추려는 것이다. */
	unsigned long bitmap = rcec->rcec_ea->bitmap;
	/* [한국어] 비트맵을 순회할 때 쓸 device 번호. */
	unsigned int devn;

	/* An RCiEP found on a different bus in range */
	/* [한국어] RCEC 와 다른 버스에 있는 장치라면, */
	if (rcec->bus->number != rciep->bus->number)
		/* [한국어] 옆의 원문 주석대로 무조건 담당이다. 언뜻 이상해 보이지만 근거가 있다 —
		 * 이 함수를 부르는 walk_rcec() 이 애초에 [nextbusn, lastbusn] 범위 안의
		 * 버스만 훑기 때문에, 다른 버스에서 만났다는 것 자체가 범위 안이라는 뜻이다.
		 * 즉 버스 단위 판정은 순회 쪽이 이미 끝냈고, 이 함수는 같은 버스일 때만
		 * 추가 판정을 한다. */
		return true;

	/* Same bus, so check bitmap */
	/* [한국어] 같은 버스라면 비트맵을 봐야 한다. 32비트를 순회하며 세워진 비트만 훑는다. */
	for_each_set_bit(devn, &bitmap, 32)
		/* [한국어] 그 비트 번호가 이 장치의 device 번호와 같으면, */
		if (devn == PCI_SLOT(rciep->devfn))
			/* [한국어] 담당이다. 비트맵의 비트 n 이 device 번호 n 을 뜻한다는 규격의 약속이
			 * 이 한 줄에 들어 있다. */
			return true;

	/* [한국어] 어느 비트와도 맞지 않으면 담당이 아니다. */
	return false;
}

/* [한국어]
 * link_rcec_helper - 만난 RCiEP 에 자기를 담당하는 RCEC 포인터를 기록한다
 *
 * @dev: 순회 중 만난 장치.
 * @data: struct walk_rcec_data 포인터.
 * @return: 언제나 0 — 순회를 끝까지 진행시킨다.
 *
 * pcie_link_rcec() 이 쓰는 콜백이다. 하는 일은 한 줄, dev->rcec 에 RCEC 를
 * 기록하는 것이다.
 *
 * 그 한 줄이 방향을 뒤집는다는 점이 중요하다. RCEC 는 자기 capability 로
 * "내가 담당하는 RCiEP 들" 을 알지만, 반대로 RCiEP 에서 출발해 "나를 담당하는
 * RCEC" 를 찾을 방법은 하드웨어에 없다. 이 포인터가 그 역방향을 만들어 주며,
 * 이후 aer.c 나 pme.c 가 특정 RCiEP 의 오류·전원 처리를 준비할 때 따라간다.
 *
 * 디버그 로그를 남기는 이유도 같은 맥락이다. RCiEP 는 링크가 없어 이벤트가
 * 어느 길로 나가는지 눈에 보이지 않으므로, 이 로그가 사실상 유일한 단서다.
 *
 * user_callback 필드를 보지 않는다. 그래서 pcie_link_rcec() 이 그 필드에
 * NULL 을 넣어도 안전하다.
 *
 * 실행 컨텍스트: pci_walk_bus() 안. pci_bus_sem 을 읽기로 쥔 상태다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_link_rcec() → walk_rcec() → pci_walk_bus() → [이 함수]
 *     → rcec_assoc_rciep()
 */
static int link_rcec_helper(struct pci_dev *dev, void *data)
{
	/* [한국어] 순회 데이터를 원래 타입으로 되돌린다. */
	struct walk_rcec_data *rcec_data = data;
	/* [한국어] 기준이 될 RCEC. */
	struct pci_dev *rcec = rcec_data->rcec;

	/* [한국어] 이 장치가 RCiEP 타입이고, */
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&
	    /* [한국어] 이 RCEC 의 담당이면, */
	    rcec_assoc_rciep(rcec, dev)) {
		/* [한국어] 그 장치에 자기를 담당하는 RCEC 를 기록한다. 이 한 줄이 이 헬퍼의
		 * 존재 이유다 — 이후 aer.c 나 pme.c 가 RCiEP 에서 출발해 RCEC 를 찾을 때
		 * 이 포인터를 따라간다. */
		dev->rcec = rcec;
		/* [한국어] 어느 RCEC 를 통해 신호가 오는지 디버그 로그에 남긴다. RCiEP 는 링크가
		 * 없어 이벤트 경로가 눈에 보이지 않으므로, 이 로그가 유일한 단서가 된다. */
		pci_dbg(dev, "PME & error events signaled via %s\n",
			pci_name(rcec));
	}

	/* [한국어] 언제나 0 을 반환해 순회를 끝까지 진행시킨다. 모든 RCiEP 에 포인터를
	 * 채워야 하므로 중간에 멈추면 안 된다. */
	return 0;
}

/* [한국어]
 * walk_rcec_helper - 만난 RCiEP 가 담당이면 사용자 콜백을 적용한다
 *
 * @dev: 순회 중 만난 장치.
 * @data: struct walk_rcec_data 포인터. 사용자 콜백과 데이터가 들어 있다.
 * @return: 언제나 0.
 *
 * pcie_walk_rcec() 이 쓰는 콜백이다. link 쪽과 구조가 같고, 마지막 한 줄만
 * "포인터를 기록한다" 대신 "사용자 콜백을 부른다" 로 다르다.
 *
 * [상류 코드 관찰] 사용자 콜백의 반환값을 버린다. pcie_walk_rcec() 의 상류
 * 문서 주석은 "@cb 가 0 이 아닌 값을 반환하면 순회를 중단한다" 고 적었고,
 * 그 아래 pci_walk_bus() → __pci_walk_bus()(bus.c:506)는 실제로 콜백이 0 이
 * 아니면 순회를 끊는다. 그런데 __pci_walk_bus 에 전달되는 콜백은 사용자
 * 콜백이 아니라 이 함수이고, 이 함수는 사용자 콜백의 값을 받지 않은 채
 * 언제나 0 을 반환한다. 그래서 문서가 말하는 중단은 실제로 일어나지 않는다.
 * 게다가 walk_rcec() 은 여러 버스를 도는 루프라 pci_walk_bus() 가 중간에
 * 멈추더라도 다음 버스로 넘어간다. 코드는 고치지 않고 사실만 기록한다.
 *
 * 실행 컨텍스트: pci_walk_bus() 안. pci_bus_sem 을 읽기로 쥔 상태이므로
 * 사용자 콜백도 그 제약을 받는다.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pcie_walk_rcec() → walk_rcec() → pci_walk_bus() → [이 함수]
 *     → rcec_assoc_rciep() → user_callback()
 */
static int walk_rcec_helper(struct pci_dev *dev, void *data)
{
	/* [한국어] 순회 데이터. */
	struct walk_rcec_data *rcec_data = data;
	/* [한국어] 기준 RCEC. */
	struct pci_dev *rcec = rcec_data->rcec;

	/* [한국어] RCiEP 타입이고, */
	if ((pci_pcie_type(dev) == PCI_EXP_TYPE_RC_END) &&
	    /* [한국어] 담당이면, */
	    rcec_assoc_rciep(rcec, dev))
		/* [한국어] 사용자 콜백을 적용한다. **반환값을 버린다**는 점에 주의할 만하다.
		 * pcie_walk_rcec() 의 상류 주석은 콜백이 0 이 아닌 값을 주면 순회를
		 * 멈춘다고 적었지만, 그 값이 여기서 버려지므로 실제로는 멈추지 않는다.
		 * [상류 코드 관찰] 코드는 고치지 않고 사실만 적는다. */
		rcec_data->user_callback(dev, rcec_data->user_data);

	/* [한국어] 언제나 0 이라 pci_walk_bus() 도 끝까지 돈다. 위 관찰과 같은 이야기다. */
	return 0;
}

/* [한국어]
 * walk_rcec - 이 RCEC 가 담당할 수 있는 모든 버스를 훑으며 콜백을 적용한다
 *
 * @cb: 각 장치에 적용할 헬퍼. link_rcec_helper() 또는 walk_rcec_helper().
 * @userdata: struct walk_rcec_data 포인터. rcec 가 들어 있다.
 *
 * 두 진입점이 공유하는 순회 본체다. 어느 버스를 훑을지 정하는 것이
 * 이 함수의 일이고, 그 버스 안에서 어느 장치가 담당인지는 콜백 안의
 * rcec_assoc_rciep() 이 정한다. 두 단계로 나뉜 판정의 위쪽 절반이다.
 *
 * 순회 범위는 둘로 나뉜다. 먼저 RCEC 자기 버스를 훑는다 — 같은 버스는
 * 비트맵으로 가려진다. 그 다음 [nextbusn, lastbusn] 범위의 버스들을 훑는데,
 * 여기서 만나는 장치는 모두 담당으로 친다.
 *
 * 조기 반환 조건(nextbusn == 0xff && lastbusn == 0x00)이 영리하다.
 * BUSN 레지스터가 없는 구형 capability 에 대해 pci_rcec_init() 이 일부러
 * 이 값을 넣어 두므로, 여기서 capability 버전을 다시 확인할 필요가 없다.
 * 값 자체가 "자기 버스뿐" 이라는 표시가 된다.
 *
 * 루프 안의 두 건너뛰기도 각각 이유가 있다. 자기 버스는 위에서 이미 훑었고,
 * 규격(PCIe 5.0-1, 7.9.10.3)이 이 범위에 자기 버스가 들어오면 연관 없음을
 * 뜻한다고 정한다. 그리고 하드웨어가 선언한 범위가 실제 버스보다 넓을 수
 * 있으므로, 없는 버스는 조용히 넘어간다.
 *
 * 실행 컨텍스트: 열거 경로 또는 AER/PME 이벤트 처리. 프로세스/스레드
 * 컨텍스트이며, pci_walk_bus() 가 안에서 pci_bus_sem 을 읽기로 잡는다.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   pcie_link_rcec() / pcie_walk_rcec() → [이 함수]
 *     → pci_walk_bus(자기 버스) → pci_find_bus() → pci_walk_bus(범위 버스들)
 */
static void walk_rcec(int (*cb)(struct pci_dev *dev, void *data),
		      void *userdata)
{
	/* [한국어] 두 진입점이 스택에 만들어 넘긴 순회 데이터. */
	struct walk_rcec_data *rcec_data = userdata;
	/* [한국어] 주체 RCEC. */
	struct pci_dev *rcec = rcec_data->rcec;
	/* [한국어] 담당 버스 범위의 시작과 끝. */
	u8 nextbusn, lastbusn;
	/* [한국어] 범위 안의 각 버스를 담을 곳. */
	struct pci_bus *bus;
	/* [한국어] 버스 번호 순회 인덱스. */
	unsigned int bnr;

	/* [한국어] Association capability 를 읽어 두지 않았으면 담당 관계를 알 수 없으므로, */
	if (!rcec->rcec_ea)
		/* [한국어] 아무것도 하지 않는다. 두 진입점도 같은 검사를 하므로 이것은 두 번째
		 * 방어선이다. */
		return;

	/* Walk own bus for bitmap based association */
	/* [한국어] 먼저 자기 버스를 훑는다. 옆의 원문 주석대로 같은 버스는 비트맵으로
	 * 판정되며, 그 판정은 콜백 안의 rcec_assoc_rciep() 이 한다. */
	pci_walk_bus(rcec->bus, cb, rcec_data);

	/* [한국어] 담당 버스 범위의 시작과, */
	nextbusn = rcec->rcec_ea->nextbusn;
	/* [한국어] 끝을 꺼낸다. */
	lastbusn = rcec->rcec_ea->lastbusn;

	/* All RCiEP devices are on the same bus as the RCEC */
	/* [한국어] 옆의 원문 주석대로 이 조합은 "모든 RCiEP 가 RCEC 와 같은 버스에 있다"
	 * 는 표시다. pci_rcec_init() 이 BUSN 레지스터가 없는 구형 capability 에
	 * 일부러 넣어 두는 값이기도 하다 — 그렇게 해 두면 여기서 버전을 다시
	 * 확인할 필요가 없다. */
	if (nextbusn == 0xff && lastbusn == 0x00)
		/* [한국어] 자기 버스만 훑고 끝낸다. */
		return;

	/* [한국어] 범위 안의 버스 번호를 하나씩. */
	for (bnr = nextbusn; bnr <= lastbusn; bnr++) {
		/* No association indicated (PCIe 5.0-1, 7.9.10.3) */
		/* [한국어] 자기 버스는 위에서 이미 훑었고, 옆의 원문 주석대로 규격(PCIe 5.0-1,
		 * 7.9.10.3)이 이 범위에 자기 버스가 들어오면 연관 없음을 뜻한다고 정한다. */
		if (bnr == rcec->bus->number)
			/* [한국어] 건너뛴다. */
			continue;

		/* [한국어] 같은 도메인 안에서 그 번호의 버스를 찾는다. */
		bus = pci_find_bus(pci_domain_nr(rcec->bus), bnr);
		/* [한국어] 없으면(그 번호의 버스가 실재하지 않으면), */
		if (!bus)
			/* [한국어] 건너뛴다. 범위는 하드웨어가 선언한 것이라 실제보다 넓을 수 있다. */
			continue;

		/* Find RCiEP devices on the given bus ranges */
		/* [한국어] 그 버스를 훑는다. 여기서 만난 장치는 rcec_assoc_rciep() 이 무조건
		 * 담당으로 판정한다 — 버스 범위 검사를 이 루프가 이미 했기 때문이다. */
		pci_walk_bus(bus, cb, rcec_data);
	}
}

/**
 * pcie_link_rcec - Link RCiEP devices associated with RCEC.
 * @rcec: RCEC whose RCiEP devices should be linked.
 *
 * Link the given RCEC to each RCiEP device found.
 */
/* [한국어]
 * pcie_link_rcec - 담당 RCiEP 들에 이 RCEC 를 가리키는 포인터를 심는다
 *
 * @rcec: 담당 RCiEP 들을 이어 줄 RCEC.
 *
 * RCEC 가 등록될 때 한 번 불려, 담당 RCiEP 각각의 dev->rcec 를 채운다.
 * 하드웨어에 없는 역방향 관계를 소프트웨어로 만들어 두는 작업이다.
 *
 * pcie_walk_rcec() 과 코드가 거의 같고 다른 점이 둘뿐이다. 넘기는 헬퍼가
 * link_rcec_helper() 이고, user_callback 과 user_data 에 NULL 을 넣는다.
 * 그 헬퍼가 두 필드를 보지 않으므로 NULL 이어도 안전하다.
 *
 * rcec_ea 검사가 먼저 오는 것은 Association capability 를 읽지 못한 RCEC 는
 * 담당 관계를 알 수 없기 때문이다. walk_rcec() 도 같은 검사를 하므로
 * 이중 방어인 셈이다.
 *
 * 순회 데이터를 스택에 두어 할당 실패가 없다.
 *
 * 실행 컨텍스트: RCEC 등록 경로. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다. 반환값도 없다.
 *
 * 호출 체인:
 *   RCEC 포트 서비스 등록 → [이 함수] → walk_rcec(link_rcec_helper)
 */
void pcie_link_rcec(struct pci_dev *rcec)
{
	/* [한국어] 순회 데이터를 스택에 만든다. 힙 할당이 없어 실패할 여지가 없다. */
	struct walk_rcec_data rcec_data;

	/* [한국어] 담당 정보가 없으면, */
	if (!rcec->rcec_ea)
		/* [한국어] 이을 것이 없다. */
		return;

	/* [한국어] 주체를 기록하고, */
	rcec_data.rcec = rcec;
	/* [한국어] 사용자 콜백은 쓰지 않는다. link_rcec_helper() 가 이 필드를 보지 않으므로
	 * NULL 이어도 안전하다. */
	rcec_data.user_callback = NULL;
	/* [한국어] 함께 넘길 데이터도 없다. */
	rcec_data.user_data = NULL;

	/* [한국어] 연결 전용 헬퍼로 순회한다. */
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
/* [한국어]
 * pcie_walk_rcec - 이 RCEC 가 담당하는 모든 RCiEP 에 콜백을 적용한다
 *
 * @rcec: 순회할 RCEC.
 * @cb: 각 RCiEP 에 적용할 콜백.
 * @userdata: 콜백에 함께 넘길 임의의 포인터.
 *
 * AER 이나 PME 이벤트가 RCEC 로 왔을 때 "누구의 이벤트인가" 를 풀어 주는
 * 진입점이다. RCiEP 는 링크가 없어 자기 이름으로 이벤트를 보내지 못하므로,
 * RCEC 에 온 이벤트를 담당 RCiEP 전부에 뿌려 각자 확인하게 하는 구조다.
 *
 * pcie_link_rcec() 과 짝을 이룬다. 그쪽이 RCiEP → RCEC 방향의 포인터를
 * 만들어 두는 준비 작업이라면, 이쪽은 RCEC → RCiEP 방향으로 실제 일을 뿌린다.
 *
 * [상류 코드 관찰] 위 문서 주석은 "@cb 가 0 이 아닌 값을 반환하면 중단한다"
 * 고 적었지만 그 동작은 구현되어 있지 않다. 자세한 이유는
 * walk_rcec_helper() 의 설명에 적었다.
 *
 * 실행 컨텍스트: AER 스레드 핸들러나 PME 처리 경로. 프로세스/스레드
 * 컨텍스트이며, 콜백은 pci_bus_sem 을 읽기로 쥔 상태에서 불린다.
 *
 * 에러 경로: 없다. 반환값이 없어 콜백의 실패를 알릴 방법도 없다.
 *
 * 호출 체인:
 *   aer.c(cxl_rch_handle_error, handles_cxl_errors 등) / pme.c → [이 함수]
 *     → walk_rcec(walk_rcec_helper) → pci_walk_bus() → cb()
 */
void pcie_walk_rcec(struct pci_dev *rcec, int (*cb)(struct pci_dev *, void *),
		    void *userdata)
{
	/* [한국어] 순회 데이터. */
	struct walk_rcec_data rcec_data;

	/* [한국어] 담당 정보가 없으면, */
	if (!rcec->rcec_ea)
		/* [한국어] 훑을 대상이 없다. */
		return;

	/* [한국어] 주체를 기록하고, */
	rcec_data.rcec = rcec;
	/* [한국어] 사용자 콜백과, */
	rcec_data.user_callback = cb;
	/* [한국어] 그 데이터를 실어 둔다. 이 둘이 있다는 점만 pcie_link_rcec() 과 다르다. */
	rcec_data.user_data = userdata;

	/* [한국어] 콜백 적용 헬퍼로 순회한다. */
	walk_rcec(walk_rcec_helper, &rcec_data);
}

/* [한국어]
 * pci_rcec_init - RCEC 의 담당 범위를 config 에서 읽어 캐시한다
 *
 * @dev: 열거된 장치. RCEC 가 아니면 아무 일도 하지 않는다.
 *
 * 모든 장치의 열거 경로에서 불리므로 첫 줄에서 RCEC 가 아닌 것을 걸러 낸다.
 *
 * 읽어 두는 것은 두 가지다. 같은 버스의 담당 device 번호를 표시한 32비트
 * 비트맵, 그리고 여러 버스에 걸칠 때의 버스 범위(nextbusn, lastbusn).
 * 매 이벤트마다 config 를 읽지 않으려는 캐시이며, 오류 처리 경로에서
 * config 접근 비용을 줄인다.
 *
 * 버전 분기가 이 함수의 요령이다. BUSN 레지스터는 나중 버전에서 추가된
 * 것이라 구형 capability 에는 없다. 그때 nextbusn = 0xff, lastbusn = 0x00 을
 * 넣어 두는데, 이 값은 walk_rcec() 의 조기 반환 조건과 정확히 맞는다.
 * 함수 안의 원문 주석이 밝히듯, 그렇게 해 두면 순회 쪽에서 버전을 다시
 * 확인할 필요가 없다 — 값 하나가 "자기 버스뿐" 이라는 뜻을 전달한다.
 *
 * dev->rcec_ea 대입이 마지막에 오는 것도 의도된 순서다. 이 포인터가 NULL 이
 * 아니라는 사실 자체가 "캐시가 완전히 채워졌다" 는 표시로 쓰이며,
 * walk_rcec() 과 두 진입점이 모두 그것을 전제로 검사한다.
 *
 * 실행 컨텍스트: 장치 열거. 프로세스 컨텍스트.
 *
 * 에러 경로: 세 갈래 모두 조용히 반환한다. 반환값이 없어 알릴 방법이 없고,
 * rcec_ea 가 NULL 로 남으면 이 RCEC 를 통한 이벤트 전달만 동작하지 않을 뿐
 * 열거 자체는 계속된다.
 *
 * 호출 체인:
 *   pci_scan_device() 계열의 장치 초기화 → [이 함수]
 *     → pci_find_ext_capability(PCI_EXT_CAP_ID_RCEC)
 *     → kzalloc_obj() → pci_read_config_dword() ×3
 */
void pci_rcec_init(struct pci_dev *dev)
{
	/* [한국어] 할당할 캐시. */
	struct rcec_ea *rcec_ea;
	/* [한국어] capability 오프셋, 헤더 값, BUSN 레지스터 값. */
	u32 rcec, hdr, busn;
	/* [한국어] capability 버전. */
	u8 ver;

	/* Only for Root Complex Event Collectors */
	/* [한국어] 옆의 원문 주석대로 RCEC 가 아닌 장치는 대상이 아니다. 이 함수가 모든
	 * 장치의 열거 경로에서 불리므로 여기서 걸러야 한다. */
	if (pci_pcie_type(dev) != PCI_EXP_TYPE_RC_EC)
		/* [한국어] 조용히 돌아간다. */
		return;

	/* [한국어] RCEC Endpoint Association 확장 capability 를 찾는다. */
	rcec = pci_find_ext_capability(dev, PCI_EXT_CAP_ID_RCEC);
	/* [한국어] 없으면(RCEC 타입인데 capability 가 없는 하드웨어), */
	if (!rcec)
		/* [한국어] 돌아간다. rcec_ea 가 NULL 로 남고, 이후 모든 순회가 그것을 보고 물러난다. */
		return;

	/* [한국어] 캐시를 0 초기화해 할당한다. */
	rcec_ea = kzalloc_obj(*rcec_ea);
	/* [한국어] 실패하면, */
	if (!rcec_ea)
		/* [한국어] 조용히 돌아간다. 반환값이 없어 알릴 방법이 없고, rcec_ea 가 NULL 이면
		 * 이 RCEC 를 통한 이벤트 전달이 동작하지 않을 뿐 열거 자체는 계속된다. */
		return;

	/* [한국어] 같은 버스의 담당 device 번호 비트맵을 읽어 캐시한다. 매 이벤트마다
	 * config 를 읽지 않으려는 것으로, 오류 처리 경로의 비용을 줄인다. */
	pci_read_config_dword(dev, rcec + PCI_RCEC_RCIEP_BITMAP,
			      &rcec_ea->bitmap);

	/* Check whether RCEC BUSN register is present */
	/* [한국어] capability 헤더를 읽는다. 버전 필드가 그 안에 있다. */
	pci_read_config_dword(dev, rcec, &hdr);
	/* [한국어] 버전을 뽑아낸다. */
	ver = PCI_EXT_CAP_VER(hdr);
	/* [한국어] 버스 범위 레지스터가 도입된 버전 이상이면, */
	if (ver >= PCI_RCEC_BUSN_REG_VER) {
		/* [한국어] 그 레지스터를 읽어, */
		pci_read_config_dword(dev, rcec + PCI_RCEC_BUSN, &busn);
		/* [한국어] 범위의 시작과, */
		rcec_ea->nextbusn = PCI_RCEC_BUSN_NEXT(busn);
		/* [한국어] 끝을 꺼낸다. */
		rcec_ea->lastbusn = PCI_RCEC_BUSN_LAST(busn);
	} else {
		/* Avoid later ver check by setting nextbusn */
		/* [한국어] 옆의 원문 주석대로, 구형 capability 에는 이 값을 일부러 넣어 둔다.
		 * 0xff > 0x00 이라 아래 walk_rcec() 의 조기 반환 조건에 딱 걸리고,
		 * 덕분에 순회 쪽에서 버전을 다시 확인하지 않아도 된다. */
		rcec_ea->nextbusn = 0xff;
		/* [한국어] 같은 이유의 짝 값. */
		rcec_ea->lastbusn = 0x00;
	}

	/* [한국어] 완성된 캐시를 장치에 매단다. 마지막에 대입하는 것이 중요하다 —
	 * 이 포인터가 NULL 이 아니라는 사실 자체가 "캐시가 완전히 채워졌다" 는
	 * 표시로 쓰이기 때문이다. */
	dev->rcec_ea = rcec_ea;
}

/* [한국어]
 * pci_rcec_exit - 캐시를 해제하고 포인터를 지운다
 *
 * @dev: 해제되는 장치.
 *
 * pci_rcec_init() 의 짝이다. RCEC 가 아니었던 장치는 rcec_ea 가 NULL 이라
 * kfree(NULL) 이 되어 그대로 안전하다 — 그래서 타입 검사가 없다.
 *
 * 포인터를 NULL 로 되돌리는 두 번째 줄이 중요하다. 이 포인터의 NULL 여부가
 * "담당 정보를 쓸 수 있는가" 의 판정 기준이므로, 지워 두어야 해제 이후에
 * 남은 순회 경로가 있더라도 해제된 메모리를 읽지 않고 조기 반환한다.
 *
 * 실행 컨텍스트: 장치 해제. 프로세스 컨텍스트.
 *
 * 에러 경로: 없다.
 *
 * 호출 체인:
 *   pci_release_dev() 계열 → [이 함수] → kfree()
 */
void pci_rcec_exit(struct pci_dev *dev)
{
	/* [한국어] 캐시를 해제한다. NULL 이어도 안전하다. */
	kfree(dev->rcec_ea);
	/* [한국어] 포인터를 지운다. 이렇게 해야 해제 이후에 남은 순회 경로가 있더라도
	 * 해제된 메모리를 읽지 않고 조기 반환한다. */
	dev->rcec_ea = NULL;
}
