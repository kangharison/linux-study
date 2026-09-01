// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Microsoft Corporation.
 *
 * Author:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *
 * This small module is a helper driver allows other drivers to
 * have a common interface with the Hyper-V PCI frontend driver.
 */

/* [한국어] 커널 로그 접두사 지정 매크로. pr_err()/pr_warn() 등이 출력하는 문자열 앞에
 * KBUILD_MODNAME(= 이 파일이 빌드되는 모듈 이름 pci-hyperv-intf)과 ": "를 자동으로 붙인다.
 * 왜 필요한가: Hyper-V 게스트에는 pci-hyperv, pci-hyperv-intf 두 모듈이 함께 올라오므로
 * 로그만 보고 어느 쪽이 찍었는지 구분해야 한다. 단 현재 이 파일에는 pr_* 호출이 하나도
 * 없어서 실질 효과는 없고, 향후 로그를 추가할 때를 대비한 관례적 선언이다. */
/*
 * [한국어 설명] Hyper-V VPCI config block 공용 인터페이스 심(shim) 모듈 (pci-hyperv-intf.c)
 *
 * === 파일의 역할 ===
 * Hyper-V 게스트에서 PCI 패스스루 디바이스에 접근하는 프론트엔드 드라이버
 * (drivers/pci/controller/pci-hyperv.c)가 제공하는 "config block" 읽기/쓰기 기능을,
 * 그 드라이버와 직접 링크되지 않은 제3의 드라이버가 호출할 수 있도록 중계하는
 * 아주 얇은 간접 층(shim)이다. Hyper-V 의 VPCI(가상 PCI) 프로토콜에는 표준 PCI
 * config space 와 별개로, 하이퍼바이저와 디바이스 벤더가 합의한 논리 "블록" 단위의
 * 벤더 전용 데이터 채널이 존재한다. 이 채널은 VMBus 채널을 소유한 pci-hyperv.c 만
 * 접근할 수 있으므로, 소비자 드라이버가 직접 부르려면 두 모듈이 강하게 결합된다.
 * 이 파일은 함수 포인터 3개짜리 전역 구조체 hvpci_block_ops 의 실체를 정의해 export 하고,
 * 그 위에 NULL 검사만 하는 래퍼 3개를 씌워 export 한다. 그 결과 pci-hyperv 모듈이
 * 로드되어 있지 않아도 소비자 드라이버의 심볼 해석은 성공하고, 런타임에 -EOPNOTSUPP 를
 * 받아 해당 기능만 조용히 비활성화하면 된다. 코드가 60줄이 되지 않는 이유는 이 파일의
 * 존재 목적 자체가 "로직"이 아니라 "모듈 경계"이기 때문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * PCI 서브시스템 계층에서 이 파일은 컨트롤러 드라이버 계층(drivers/pci/controller/)에
 * 속하지만, 실제로 버스를 열거하거나 하드웨어 레지스터를 만지지는 않는다. 열거·config
 * 접근은 전적으로 이웃 파일 pci-hyperv.c 가 담당하고, 이 파일은 그 위에 얹힌
 * 심볼 재수출 계층이다. 정방향 호출 체인은
 *   소비자 드라이버 -> [이 파일의 hyperv_*_cfg_blk()] -> hvpci_block_ops.<콜백>
 *   -> pci-hyperv.c 의 hv_read_config_block()/hv_write_config_block()
 *   -> VMBus 패킷 송신 -> Hyper-V 호스트 -> 물리 디바이스
 * 이고, 역방향(무효화 통지) 체인은
 *   호스트 -> VMBus 인터럽트 -> pci-hyperv.c -> 소비자가 등록해 둔 block_invalidate() 콜백
 * 이다. 실행 컨텍스트는 전부 게스트 커널 공간(로드 가능 모듈)이며, 세 래퍼는 하위
 * 구현이 VMBus 응답을 기다리며 잠들 수 있으므로 프로세스 컨텍스트에서만 호출해야 한다.
 * 초기화 순서 관점에서는 이 모듈이 먼저 올라와 hvpci_block_ops 를 0(NULL)으로 노출하고,
 * pci-hyperv 모듈이 나중에 init_hv_pci_drv() 에서 세 필드를 채운다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 쪽: include/linux/hyperv.h — struct hyperv_pci_block_ops 의 정의(:1739),
 * hvpci_block_ops 의 extern 선언(:1749), 그리고 이 파일이 구현하는 세 함수의
 * 프로토타입(:1731~1737)이 모두 그 헤더에 있다. 즉 이 파일은 그 선언들의 유일한 구현체다.
 * 이 파일에 의존하는 쪽: (1) pci-hyperv.c 가 전역 hvpci_block_ops 에 콜백을 설치하고
 * (:4183~4185) 언로드 시 NULL 로 되돌린다(:4163~4165). (2) Hyper-V 상에서 동작하는
 * 소비자 드라이버(대표적으로 가상 함수 NIC 드라이버)가 hyperv_read_cfg_blk() 계열을
 * 호출한다 — 다만 이 소스 트리는 drivers/pci 중심의 부분 체크아웃이라 소비자 쪽
 * 호출부는 여기에 존재하지 않는다.
 * 데이터 흐름: 소비자의 버퍼(buf) 포인터와 블록 번호(block_id)가 이 파일을 아무 가공 없이
 * 통과해 pci-hyperv.c 로 전달되고, 거기서 VMBus 패킷 페이로드로 복사되어 호스트로 나간다.
 * 읽기 응답은 역순으로 buf 에 채워지고 실제 길이는 bytes_returned 로 돌아온다.
 * 공유 상태: 전역 변수 hvpci_block_ops 단 하나. 이 변수가 두 모듈 사이의 유일한
 * 결합점이자 이 파일이 존재하는 이유 그 자체다.
 *
 * === 주요 함수/구조체 요약 ===
 * - hvpci_block_ops (전역 변수): read_block/write_block/reg_blk_invalidate 세 함수
 *   포인터를 담는다. BSS 초기값 NULL 이 곧 "pci-hyperv 미로드 = 기능 없음"을 뜻한다.
 * - hyperv_read_cfg_blk(): 블록 읽기 래퍼. NULL 검사 후 위임하며, 가변 길이 응답이므로
 *   호출자는 buf_len 이 아니라 출력 인자 bytes_returned 를 신뢰해야 한다.
 * - hyperv_write_cfg_blk(): 블록 쓰기 래퍼. 부분 성공 개념이 없어 반환 길이 인자가 없다.
 * - hyperv_reg_block_invalidate(): 블록 내용 변경 통지 콜백 등록 래퍼. 콜백이 받는
 *   block_mask 는 비트 N = block_id N 이 무효화되었음을 뜻하는 비트마스크다.
 * - 세 함수 모두 새로 만드는 오류는 -EOPNOTSUPP 하나뿐이고 나머지는 하위 반환값을
 *   그대로 통과시키는 순수 전달 구조다.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

/* [한국어] container_of/기본 타입·printk 계열 등 커널 공통 정의. 아래 함수들이 쓰는
 * int/unsigned int 계열 커널 기본형과 -EOPNOTSUPP 같은 errno 상수 경로를 끌어온다. */
#include <linux/kernel.h>
/* [한국어] 이 파일을 로드 가능한 커널 모듈로 만들기 위한 정의. 아래 EXPORT_SYMBOL_GPL()과
 * MODULE_LICENSE()/MODULE_DESCRIPTION() 매크로가 여기서 온다. */
#include <linux/module.h>
/* [한국어] Hyper-V VMBus 관련 공용 헤더. 이 파일의 핵심인 struct hyperv_pci_block_ops
 * (include/linux/hyperv.h:1739)와 hvpci_block_ops extern 선언(:1749),
 * 그리고 hyperv_read_cfg_blk()/hyperv_write_cfg_blk()/hyperv_reg_block_invalidate()의
 * 프로토타입(:1731~1737)이 모두 이 헤더에 있다. 즉 이 파일은 그 선언들의 유일한 구현체다. */
#include <linux/hyperv.h>
/* [한국어] EXPORT_SYMBOL_GPL() 매크로 정의. 커널이 export 매크로를 별도 헤더로 분리한 뒤
 * module.h만으로는 심볼 export가 되지 않으므로 명시적으로 포함해야 한다. */
#include <linux/export.h>

/* [한국어] Hyper-V PCI "config block" 접근 함수 포인터 3개를 담는 전역 구조체 실체(definition).
 * 설정자: pci-hyperv.c의 init_hv_pci_drv()가 모듈 로드 시 세 필드를 각각
 *   hv_read_config_block / hv_write_config_block / hv_register_block_invalidate 로 채우고
 *   (drivers/pci/controller/pci-hyperv.c:4183~4185), exit_hv_pci_drv()가 언로드 시 셋 다
 *   NULL 로 되돌린다(:4163~4165).
 * 읽는 자: 바로 아래 세 래퍼 함수가 NULL 검사 후 역참조한다.
 * 값 범위: 전역 변수이므로 BSS 에서 0(=NULL)으로 시작한다 — 즉 pci-hyperv 모듈이 아직
 *   로드되지 않은 상태가 곧 "기능 없음"으로 자연스럽게 표현된다.
 * 동기화: 별도 락이 없다. 모듈 로드/언로드 시점에만 쓰이고 그 사이에는 읽기 전용이라는
 *   전제를 두며, 언로드와 호출이 겹치는 경합은 소비자 드라이버가 pci-hyperv 모듈
 *   참조를 잡고 있다는 전제로 회피한다. */
struct hyperv_pci_block_ops hvpci_block_ops;
/* [한국어] 위 전역 구조체 자체를 GPL 모듈에 export. pci-hyperv.c 가 이 변수에 직접 대입해야
 * 하므로 함수뿐 아니라 변수도 export 대상이다. 이 심볼이 여기(별도 모듈)에 있기 때문에
 * pci-hyperv.c 와 소비자 드라이버 사이의 순환 모듈 의존이 끊어진다. */
EXPORT_SYMBOL_GPL(hvpci_block_ops);

/* [한국어]
 * hyperv_read_cfg_blk - Hyper-V VPCI 벤더 전용 "config block"에서 데이터를 읽는다
 *
 * @dev: 읽으려는 PCI 디바이스. Hyper-V 게스트에서 VPCI(가상 PCI) 버스에 열거된
 *      패스스루 디바이스여야 한다. pci-hyperv.c 는 이 pci_dev 로부터 자신이 관리하는
 *      hv_pcibus_device/hv_pci_dev 를 역추적해 어느 VMBus 채널로 요청을 보낼지 정한다.
 * @buf: 읽어온 바이트를 담을 호출자 소유 버퍼. 커널 가상 주소여야 하며
 *      (하위 구현이 VMBus 패킷에 복사하므로) 사용자 공간 포인터는 안 된다.
 * @buf_len: buf 의 크기(바이트). 요청한 블록이 이보다 크면 하위 구현이 실패를 돌려준다.
 * @block_id: 읽을 블록 번호. 표준 PCI config space 오프셋이 아니라, 하이퍼바이저와
 *      디바이스 벤더가 합의한 논리 블록 식별자다(0..63 범위를 아래 block_mask 비트가 표현).
 * @bytes_returned: 실제로 채워진 바이트 수를 돌려받을 출력 인자. 블록 크기가
 *      가변이므로 호출자는 buf_len 이 아니라 이 값을 신뢰해야 한다.
 * @return: 성공 시 0. pci-hyperv 모듈이 없으면 -EOPNOTSUPP.
 *      그 밖의 음수는 하위 hv_read_config_block() 이 돌려준 VMBus/호스트 오류를 그대로 전달한다.
 *
 * 왜 필요한가: Hyper-V 패스스루 환경에서 VF 드라이버는 물리 디바이스의 벤더 전용
 * 설정 데이터를 읽어야 하는데, 게스트는 그 데이터에 PCI config space 로 직접 접근할 수
 * 없다. 접근 경로는 오직 pci-hyperv.c 가 소유한 VMBus 채널뿐이다. 그렇다고 VF 드라이버가
 * pci-hyperv.c 의 내부 함수를 직접 부르면 두 모듈이 강하게 결합되고, pci-hyperv 가
 * 로드되지 않은 시스템에서는 심볼 해석 자체가 실패해 VF 드라이버가 아예 올라오지 못한다.
 * 이 파일이 별도 모듈로 분리되어 항상 존재하는 "간접 층"을 제공함으로써 그 문제를 없앤다.
 *
 * 동작 과정:
 * 1) 전역 hvpci_block_ops.read_block 이 설치되어 있는지 확인한다(= pci-hyperv 로드 여부).
 * 2) 없으면 -EOPNOTSUPP 로 즉시 반환 — NULL 함수 포인터 역참조를 막는 유일한 방어선이다.
 * 3) 있으면 다섯 인자를 가공 없이 그대로 넘겨 호출하고, 그 반환값을 그대로 돌려준다.
 *
 * 실행 컨텍스트: 게스트 커널의 프로세스 컨텍스트. 하위 구현이 VMBus 응답을 기다리며
 * 잠들 수 있으므로 인터럽트 핸들러나 spinlock 보유 구간에서 호출하면 안 된다.
 * 재진입은 안전하다 — 이 함수 자체는 상태를 갖지 않으며, 직렬화는 하위 구현의 몫이다.
 *
 * 에러 경로: 여기서 새로 만드는 오류는 -EOPNOTSUPP 하나뿐이고, 나머지는 전부 하위에서
 * 올라온 값을 투명하게 통과시킨다.
 *
 * 호출 체인:
 *   소비자 드라이버(예: Hyper-V 상의 VF 드라이버)
 *     -> [hyperv_read_cfg_blk]
 *     -> hvpci_block_ops.read_block == hv_read_config_block() (drivers/pci/controller/pci-hyperv.c)
 *     -> VMBus 패킷 송신 -> Hyper-V 호스트 -> 물리 디바이스
 */
int hyperv_read_cfg_blk(struct pci_dev *dev, void *buf, unsigned int buf_len,
			unsigned int block_id, unsigned int *bytes_returned)
{
	/* [한국어] 핵심 방어 로직: pci-hyperv 모듈이 아직 로드되지 않았거나 이미 언로드되어
	 * read_block 이 NULL 이면 역참조 없이 빠져나간다. */
	if (!hvpci_block_ops.read_block)
		/* [한국어] "이 플랫폼/구성에서는 지원하지 않는 연산"을 뜻하는 표준 errno.
		 * -ENODEV(장치 없음)가 아니라 -EOPNOTSUPP 인 이유는 장치는 있는데 Hyper-V VPCI
		 * 블록 채널만 없는 상황이기 때문이며, 소비자 드라이버는 이 값을 보고 해당 기능만
		 * 조용히 비활성화한다. */
		return -EOPNOTSUPP;

	/* [한국어] 실제 동작은 전적으로 pci-hyperv.c 의 hv_read_config_block() 에 위임한다.
	 * 그 함수는 VMBus 채널로 블록 읽기 요청 패킷을 보내고 호스트 응답을 기다리므로
	 * 잠들 수 있다 — 따라서 이 래퍼도 atomic 컨텍스트에서 호출하면 안 된다.
	 * 인자는 하나도 가공하지 않고 그대로 넘기는 순수 전달(pass-through)이다. */
	return hvpci_block_ops.read_block(dev, buf, buf_len, block_id,
					  bytes_returned);
}
/* [한국어] 소비자 드라이버(예: Hyper-V 상의 Mellanox VF 드라이버)가 모듈 경계를 넘어
 * 호출할 수 있도록 GPL 심볼로 공개. */
EXPORT_SYMBOL_GPL(hyperv_read_cfg_blk);

/* [한국어]
 * hyperv_write_cfg_blk - Hyper-V VPCI 벤더 전용 "config block"에 데이터를 쓴다
 *
 * @dev: 쓰려는 PCI 디바이스. 읽기 경로와 마찬가지로 VPCI 버스에 열거된 디바이스여야 한다.
 * @buf: 호스트로 보낼 바이트가 담긴 호출자 소유 버퍼(커널 가상 주소).
 * @len: buf 에서 실제로 보낼 바이트 수. 읽기와 달리 부분 성공 개념이 없어
 *      "len 전체 성공" 아니면 실패다 — 그래서 bytes_returned 같은 출력 인자가 없다.
 * @block_id: 쓸 블록 번호. 읽기의 block_id 와 같은 논리 블록 번호 공간을 쓴다.
 * @return: 성공 시 0, pci-hyperv 모듈 부재 시 -EOPNOTSUPP,
 *      그 밖의 음수는 하위 hv_write_config_block() 의 오류를 그대로 전달.
 *
 * 왜 필요한가: 읽기 래퍼와 정확히 같은 이유 — 소비자 드라이버와 pci-hyperv 모듈을
 * 느슨하게 결합하기 위한 간접 층이다. 쓰기 경로는 게스트가 호스트 측 디바이스 상태를
 * 바꾸므로, 미지원 환경에서 조용히 무시되지 않고 -EOPNOTSUPP 로 명확히 실패하는 것이 중요하다.
 *
 * 동작 과정:
 * 1) hvpci_block_ops.write_block 설치 여부 확인.
 * 2) 미설치면 -EOPNOTSUPP 반환.
 * 3) 설치되어 있으면 네 인자를 그대로 위임하고 반환값을 그대로 전달.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 잠들 수 있음. 인터럽트/atomic 구간 금지.
 *
 * 에러 경로: -EOPNOTSUPP 외에는 전부 하위 반환값 그대로.
 *
 * 호출 체인:
 *   소비자 드라이버
 *     -> [hyperv_write_cfg_blk]
 *     -> hvpci_block_ops.write_block == hv_write_config_block() (pci-hyperv.c)
 *     -> VMBus 패킷 송신 -> Hyper-V 호스트
 */
int hyperv_write_cfg_blk(struct pci_dev *dev, void *buf, unsigned int len,
			 unsigned int block_id)
{
	/* [한국어] write_block 미설치 검사. read 쪽과 동일한 이유로 NULL 역참조를 막는다. */
	if (!hvpci_block_ops.write_block)
		/* [한국어] 읽기 경로와 같은 반환값으로 통일해, 소비자가 읽기/쓰기 미지원을 한 가지
		 * 코드로 처리할 수 있게 한다. */
		return -EOPNOTSUPP;

	/* [한국어] pci-hyperv.c 의 hv_write_config_block() 로 위임. 읽기와 달리 반환 길이가 없으므로
	 * bytes_returned 인자도 없다 — 쓰기는 요청한 len 전체가 성공 아니면 실패다. */
	return hvpci_block_ops.write_block(dev, buf, len, block_id);
}
/* [한국어] 쓰기 래퍼도 GPL 심볼로 공개. */
EXPORT_SYMBOL_GPL(hyperv_write_cfg_blk);

/* [한국어]
 * hyperv_reg_block_invalidate - config block 무효화 통지 콜백을 등록한다
 *
 * @dev: 통지를 받고자 하는 PCI 디바이스.
 * @context: 콜백이 호출될 때 첫 인자로 되돌려받을 불투명 포인터. 보통 소비자
 *      드라이버의 디바이스별 private 구조체를 넘겨, 콜백에서 어떤 인스턴스인지 복원한다.
 *      이 파일은 내용을 해석하지 않고 그대로 전달만 한다.
 * @block_invalidate: 호스트가 블록 내용 변경을 알릴 때 호출될 콜백 함수 포인터.
 *      두 번째 인자 block_mask 는 비트마스크로, 비트 N 이 1 이면 block_id N 의 내용이
 *      더 이상 유효하지 않으니 다시 읽으라는 뜻이다. 한 번의 통지로 여러 블록을
 *      한꺼번에 알릴 수 있어 통지 횟수를 줄인다.
 * @return: 성공 시 0, pci-hyperv 모듈 부재 시 -EOPNOTSUPP,
 *      그 밖의 음수는 하위 hv_register_block_invalidate() 의 오류.
 *
 * 왜 필요한가: 블록 데이터는 호스트/물리 디바이스 쪽 사정으로 언제든 바뀔 수 있다.
 * 폴링으로 매번 다시 읽으면 VMBus 왕복 비용이 크므로, 변경 시에만 통지를 받는
 * 푸시 모델이 필요하다. 이 함수가 그 통지 채널을 여는 진입점이다.
 *
 * 동작 과정:
 * 1) hvpci_block_ops.reg_blk_invalidate 설치 여부 확인.
 * 2) 미설치면 -EOPNOTSUPP — 소비자는 통지 없이 동작하는 대체 경로로 넘어간다.
 * 3) 설치되어 있으면 dev/context/콜백을 그대로 위임한다.
 *
 * 실행 컨텍스트: 등록 자체는 프로세스 컨텍스트에서 수행된다. 단, 나중에 실제로
 * 불려오는 block_invalidate 콜백은 pci-hyperv 의 VMBus 처리 문맥에서 실행되므로,
 * 소비자는 그 콜백 안에서 오래 잠들거나 무거운 작업을 하지 않도록 설계해야 한다.
 *
 * 에러 경로: -EOPNOTSUPP 외에는 하위 반환값 그대로. 등록 실패 시 콜백은 결코
 * 호출되지 않으므로 소비자는 해제 처리를 따로 할 필요가 없다.
 *
 * 호출 체인:
 *   소비자 드라이버
 *     -> [hyperv_reg_block_invalidate]
 *     -> hvpci_block_ops.reg_blk_invalidate == hv_register_block_invalidate() (pci-hyperv.c)
 *   이후 역방향 통지:
 *   Hyper-V 호스트 -> VMBus 인터럽트 -> pci-hyperv.c -> block_invalidate(context, block_mask)
 */
int hyperv_reg_block_invalidate(struct pci_dev *dev, void *context,
				void (*block_invalidate)(void *context,
							 u64 block_mask))
{
	/* [한국어] reg_blk_invalidate 미설치 검사. 콜백 등록은 뒤에 해제 경로가 따라오므로,
	 * 여기서 실패를 명확히 돌려주는 것이 특히 중요하다. */
	if (!hvpci_block_ops.reg_blk_invalidate)
		/* [한국어] 등록 자체가 불가능하므로 소비자는 무효화 통지 없이 폴링 방식으로 동작하도록
		 * 대체 경로를 택하게 된다. */
		return -EOPNOTSUPP;

	/* [한국어] pci-hyperv.c 의 hv_register_block_invalidate() 로 위임.
	 * context 는 소비자가 나중에 콜백에서 돌려받을 불투명 포인터이고,
	 * block_invalidate 는 호스트가 "이 블록들의 내용이 바뀌었다"고 알릴 때 호출될 콜백이다.
	 * block_mask 비트 하나가 블록 ID 하나에 대응하므로 한 번의 통지로 여러 블록을 알린다. */
	return hvpci_block_ops.reg_blk_invalidate(dev, context,
						  block_invalidate);
}
/* [한국어] 무효화 콜백 등록 래퍼도 GPL 심볼로 공개. */
EXPORT_SYMBOL_GPL(hyperv_reg_block_invalidate);

/* [한국어] modinfo 에 노출되는 모듈 설명. 이 모듈이 독립적으로 로드되는 실체임을 보여준다. */
MODULE_DESCRIPTION("Hyper-V PCI Interface");
/* [한국어] 라이선스 선언. GPL 계열이어야 위 EXPORT_SYMBOL_GPL 심볼들을 실제로 쓸 수 있고,
 * 커널이 taint 플래그를 세우지 않는다. */
MODULE_LICENSE("GPL v2");
