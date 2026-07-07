// SPDX-License-Identifier: GPL-2.0

/*
 * [한국어 설명] OpenFirmware(디바이스 트리) 고정 파티션 파서 (block/partitions/of.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 온디스크 파티션 테이블(MBR/GPT 등)이 전혀 없는 블록 장치에 대해,
 * 부트펌웨어가 Device Tree(디바이스 트리)에 심어 둔 "fixed-partitions" 호환
 * 노드를 읽어 고정된 파티션 레이아웃을 소프트웨어적으로 만들어내는 파티션
 * 프로버(prober)다. eMMC 컨트롤러, raw NAND/NOR 플래시, 임베디드 ARM/PowerPC
 * 보드의 온보드 스토리지처럼 파티션 테이블을 둘 저장공간 여유가 없거나
 * 부트로더가 절대 오프셋으로 커널/루트파일시스템에 접근해야 하는 장치에서
 * 주로 쓰인다(Kconfig 도움말 원문: "mainly for eMMC"). 문법은 MTD(Memory
 * Technology Device) 서브시스템의 fixed-partition 바인딩과 동일한 스키마
 * (reg = <offset size>, label 또는 name, read-only)를 그대로 재사용하므로,
 * MTD 파티셔닝에 익숙한 보드 개발자가 블록 장치에도 같은 방식으로 파티션을
 * 기술할 수 있다. block/partitions/cmdline.c(커널 커맨드라인 기반)와 "온디스크
 * 테이블 없이 파티션 레이아웃을 외부에서 주입한다"는 목적은 같지만, 정보의
 * 출처가 커맨드라인 문자열이 아니라 펌웨어가 구성해 둔 디바이스 트리라는
 * 점이 다르다. validate_of_partition()이 모든 파티션 서브노드의 오프셋/크기가
 * SECTOR_SIZE 경계에 정렬돼 있는지 미리 전수 검사하여, 정렬이 깨진 정의가
 * 하나라도 있으면 등록 단계로 넘어가지 않고 전체 스캔을 실패 처리한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 계층이 새 디스크(gendisk)를 등록하거나 재스캔하면 block/partitions/
 * core.c의 check_partition()이 check_part[] 배열에 등록된 여러 파티션 포맷
 * 프로버를 순서대로 시도한다(호출 체인: blk_add_partitions() ->
 * check_partition() -> check_part[i](state)). CONFIG_OF_PARTITION이 켜져
 * 있으면 이 배열에 of_partition()이 포함되며, check.h의 선언 순서상
 * cmdline_partition() 다음, efi_partition()보다는 앞서 시도된다. 이 파일의
 * 유일한 외부 진입점인 of_partition()은 디스크마다 한 번씩, 디스크 프로브/
 * 파티션 스캔 경로의 프로세스 컨텍스트에서 호출된다. cmdline.c와 달리 이
 * 파일에는 부팅 파라미터를 지연 파싱해 두는 전역 캐시가 없다 - 매 호출마다
 * disk_to_dev(state->disk)->of_node를 통해 그 자리에서 디바이스 트리를 두 번
 * 순회한다(1차: validate_of_partition()으로 전수 검증만, 2차: add_of_partition()
 * 으로 실제 등록). validate_of_partition()/add_of_partition()은 static
 * 헬퍼로 오직 of_partition()을 정점으로 하는 호출 트리 안에서만 쓰인다.
 * 실행 컨텍스트는 시스템 부팅 시 블록 디바이스 프로브 경로 또는 이후 파티션
 * 재스캔(BLKRRPART ioctl 등) 경로이며, 인터럽트 컨텍스트나 DMA 완료 경로와는
 * 무관하다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일이 의존하는 모듈: block/partitions/check.h는 struct parsed_partitions
 * (파티션 스캔 상태)와 put_partition()(슬롯에 파티션 등록) 등 파티션 프로버
 * 공용 인터페이스를 제공한다(단, cmdline.c와 마찬가지로 이 파일도
 * read_part_sector()는 쓰지 않는다 - 파티션 위치 정보 자체가 디스크가 아니라
 * 디바이스 트리에서 오기 때문이다). include/linux/of.h는 of_get_property(),
 * of_read_number(), of_n_addr_cells()/of_n_size_cells(), of_device_is_compatible(),
 * for_each_child_of_node, of_node_get()/of_node_put() 등 디바이스 트리 파싱과
 * 참조 카운트 관리 API를 제공한다. include/linux/blkdev.h는 struct gendisk,
 * disk_to_dev() 등을 제공해 gendisk로부터 그 기반 struct device(및 그 device의
 * of_node)를 얻을 수 있게 한다. 이 파일에 의존하는(호출하는) 쪽은
 * block/partitions/core.c 하나이며, check_part[] 테이블을 통해 함수 포인터로
 * 간접 호출한다. 데이터 흐름은 다음과 같다: (1) 보드 펌웨어/부트로더가
 * 디바이스 트리에서 대상 블록 장치 노드의 자식으로 "fixed-partitions" 호환
 * 노드와 그 아래 파티션별 서브노드(각각 reg와 label 또는 name, 필요시
 * read-only 프로퍼티)를 미리 기술해 둠 -> (2) 커널이 부팅 시 이 디바이스
 * 트리를 struct device_node 트리로 메모리에 올리고, 블록 장치 드라이버가
 * 자신의 struct device의 of_node를 그 노드에 연결 -> (3) 디스크 파티션 스캔
 * 시점에 of_partition()이 disk_to_dev(state->disk)->of_node에서
 * "fixed-partitions" 자식 노드를 찾음 -> (4) validate_of_partition()으로 모든
 * 파티션 서브노드의 reg 정렬을 사전 검증 -> (5) add_of_partition()이 검증된
 * 값을 섹터 단위로 변환해 parsed_partitions.parts[]에 기록 -> (6) core.c가 이
 * 배열을 읽어 실제 파티션 block_device를 생성한다. 공유하는 핵심 자료구조는
 * check.h가 정의하는 struct parsed_partitions와, 커널 공통의
 * struct device_node(디바이스 트리 노드 표현)이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - of_partition(): 이 파일의 유일한 공개 진입점. 대상 디스크의 of_node
 *   아래 "fixed-partitions" 노드를 찾아, 모든 자식을 먼저 검증하고(1차 루프)
 *   그 다음에야 등록하는(2차 루프) 2-패스(two-pass) 방식으로 처리한다.
 * - validate_of_partition(): 파티션 서브노드 하나의 reg 프로퍼티 길이가
 *   (주소 셀 + 크기 셀) 수와 일치하고, 오프셋/크기가 SECTOR_SIZE로
 *   나누어떨어지는지 검사한다.
 * - add_of_partition(): 검증이 끝난 파티션 서브노드 하나를 실제
 *   parsed_partitions.parts[slot]에 등록한다 - 바이트를 섹터로 변환하고,
 *   read-only 플래그를 반영하고, label/name을 volname으로 복사한다.
 * - 이 파일이 정의하는 구조체나 전역 변수는 없다 - 모든 상태는 인자로 전달되는
 *   struct parsed_partitions *state와 지역 변수(struct device_node *np 등)에만
 *   존재하며, cmdline.c와 달리 부팅 시점에 미리 파싱해 캐싱해 두는 전역 리스트도
 *   없다(매 호출이 곧바로 디바이스 트리를 직접 순회).
 */

#include <linux/blkdev.h>	/* [한국어] struct gendisk, disk_to_dev() 등 블록 계층 공용 타입/헬퍼 선언 - gendisk로부터 그 기반 struct device(및 of_node)를 얻는 데 필요. */
#include <linux/major.h>	/* [한국어] 이 파일 자체에서 매크로/상수를 직접 쓰지는 않지만, 블록 디바이스 major 번호 정의 헤더를 관례적으로 포함(다른 파티션 프로버들과 통일된 include 세트, 추정). */
#include <linux/of.h>	/* [한국어] of_get_property(), of_read_number(), of_n_addr_cells()/of_n_size_cells(), of_device_is_compatible(), for_each_child_of_node, of_node_get()/of_node_put() 등 디바이스 트리 파싱/순회/참조카운트 API 전체 선언. 이 파일의 핵심 의존 헤더. */
#include <linux/string.h>	/* [한국어] strscpy(): label/name 프로퍼티 문자열을 partition_meta_info.volname 고정 크기 버퍼에 안전하게 NUL 종단 복사할 때 사용. */
#include "check.h"	/* [한국어] 로컬(비공개) 헤더: struct parsed_partitions, put_partition(), SECTOR_SIZE, ADDPART_FLAG_READONLY(block/blk.h 경유), 그리고 이 파일이 구현하는 of_partition()의 프로토타입을 선언. */

/*
 * [한국어]
 * validate_of_partition - 파티션 서브노드 하나의 reg 프로퍼티가 유효한지 검증한다.
 *
 * @np: 검증할 파티션 서브노드(디바이스 트리에서 "fixed-partitions" 노드의
 *      자식 하나, 예: partition@100000). 이 함수는 np의 참조 카운트를 직접
 *      건드리지 않고 프로퍼티만 읽는다 - of_node_get()/of_node_put()은
 *      호출자(of_partition)의 책임이다.
 * @slot: 이 노드가 등록될 예정인 parsed_partitions.parts[] 슬롯 번호. 함수
 *        본문 어디에서도 실제로 읽히지 않는 미사용 파라미터다 - 검증 로직은
 *        np의 reg 프로퍼티만으로 판단하며, slot은 아마도 add_of_partition()
 *        과 호출 시그니처를 맞춰 두 함수를 대칭적으로 다루기 위해 남아 있는
 *        것으로 보인다(추정).
 * @return: 0이면 이 노드의 reg가 유효함(주소/크기 셀 길이 일치, 오프셋/크기
 *          모두 SECTOR_SIZE 정렬). -EINVAL이면 reg 프로퍼티 형식이 잘못됐거나
 *          섹터 정렬이 깨진 경우.
 *
 * 이 함수가 왜 필요한가: 디바이스 트리의 reg 프로퍼티는 임의의 바이트 단위
 * 값을 담을 수 있지만, 블록 계층은 항상 섹터(SECTOR_SIZE, 통상 512바이트)
 * 단위로 파티션 경계를 표현한다. 정렬이 깨진(비정렬) 오프셋/크기를 그대로
 * 섹터로 나누어 등록하면 실제 파티션 경계가 펌웨어가 의도한 바이트 위치에서
 * 어긋나므로, add_of_partition()이 변환을 수행하기 전에 이 함수가 먼저 모든
 * 파티션 서브노드를 훑어 하나라도 정렬이 깨져 있으면 전체 스캔 자체를
 * 포기시킨다(of_partition()의 1차 루프).
 * 동작 순서: (1) "reg" 프로퍼티와 그 바이트 길이(len)를 얻고, 이 노드의
 * reg를 해석하는 데 필요한 주소 셀 수(a_cells)와 크기 셀 수(s_cells)를 구함
 * -> (2) len이 (a_cells + s_cells) * sizeof(__be32)와 일치하는지 확인(불일치
 * 시 -EINVAL) -> (3) 앞쪽 a_cells개 32비트 셀을 빅엔디언 오프셋(바이트)으로
 * 해석해 SECTOR_SIZE로 나누어떨어지는지 확인 -> (4) 뒤쪽 s_cells개 셀을
 * 크기(바이트)로 해석해 0이 아니면서 SECTOR_SIZE로 나누어떨어지는지 확인.
 * 실행 컨텍스트: 호출자(of_partition)와 동일한 디스크 파티션 스캔 단일
 * 프로세스 컨텍스트에서 실행된다. np를 읽기만 하고 전역 상태를 건드리지
 * 않으므로 재진입/동시성 문제가 없다.
 * 호출자: of_partition()의 1차(검증) 루프가 "fixed-partitions" 노드의 모든
 * 자식마다 한 번씩 호출한다.
 * 피호출자: of_get_property(), of_n_addr_cells(), of_n_size_cells(),
 * of_read_number().
 * 에러 경로: -EINVAL을 반환하면 of_partition()의 1차 루프가 즉시 현재 자식과
 * 부모 "fixed-partitions" 노드 양쪽의 참조를 of_node_put()으로 반납하고 -1을
 * 반환해, check_partition()이 이 프로버 전체를 실패로 처리하게 한다(일부
 * 파티션만 등록된 반쪽짜리 결과를 만들지 않기 위한 전수 검증 설계).
 *
 * 호출 체인:
 *   of_partition(1차 검증 루프) -> [이 함수]
 */
static int validate_of_partition(struct device_node *np, int slot)
{
	u64 offset, size;	/* [한국어] offset/size: reg에서 뽑아낼 파티션 시작 오프셋과 길이(둘 다 바이트 단위) - 아래에서 of_read_number()로 채워진 뒤 SECTOR_SIZE 정렬 여부만 검사하고 버려진다(변환 결과 자체는 add_of_partition()이 다시 계산). */
	int len;	/* [한국어] len: 아래 of_get_property()가 돌려주는 "reg" 프로퍼티의 바이트 길이 - a_cells/s_cells와 대조해 형식 검증에 쓰인다. */

	const __be32 *reg = of_get_property(np, "reg", &len);	/* [한국어] "reg" 프로퍼티(빅엔디언 __be32 배열)의 시작 포인터와 바이트 길이를 얻는다 - 값이 없으면 reg는 NULL, len은 정의되지 않을 수 있음(호출자가 이 두 값을 아래에서 즉시 검증). */
	int a_cells = of_n_addr_cells(np);	/* [한국어] of_n_addr_cells(np)는 np 자신이 아니라 np->parent("fixed-partitions" 노드, 필요시 그 위 조상까지)의 "#address-cells" 프로퍼티를 읽어, np의 reg 앞부분이 몇 개의 32비트 셀로 인코딩됐는지 알려준다(OF reg 인코딩 표준 관례: 셀 크기는 자신이 아니라 부모가 선언). */
	int s_cells = of_n_size_cells(np);	/* [한국어] of_n_size_cells(np)도 마찬가지로 np->parent의 "#size-cells" 프로퍼티를 읽어, reg 뒷부분(크기)이 몇 개의 32비트 셀인지 알려준다. */

	/* Make sure reg len match the expected addr and size cells */
	/* [한국어] len(바이트) / sizeof(*reg)(=4바이트, __be32 하나 크기)가 곧 reg 배열이 담고 있는 32비트 셀의 총 개수 - 이 값이 (a_cells + s_cells)와 다르면 reg가 "<주소... 크기...>" 형식을 따르지 않는 손상되거나 잘못 작성된 디바이스 트리로 간주. */
	if (len / sizeof(*reg) != a_cells + s_cells)	/* [한국어] 위에서 계산한 셀 개수 불일치 검사 - reg가 없어 len이 정의되지 않은 극단적 경우도 이 나눗셈 결과가 우연히 일치하지 않는 한 사실상 여기서 걸러진다. */
		return -EINVAL;	/* [한국어] 형식이 맞지 않는 reg는 더 이상 해석할 수 없으므로 즉시 -EINVAL(Invalid argument) 반환 - 호출자(of_partition)가 전체 스캔을 실패 처리하게 만든다. */

	/* Validate offset conversion from bytes to sectors */
	/* [한국어] reg의 앞부분(a_cells개 셀)을 실제 오프셋 값으로 변환해 섹터 정렬 여부를 검사하는 구간 시작. */
	offset = of_read_number(reg, a_cells);	/* [한국어] of_read_number(reg, a_cells)가 reg가 가리키는 빅엔디언 32비트 셀 a_cells개를 순서대로 이어붙여 하나의 u64 바이트 오프셋으로 합성한다(각 셀을 상위 32비트 쪽으로 시프트하며 OR). */
	if (offset % SECTOR_SIZE)	/* [한국어] 오프셋이 SECTOR_SIZE(통상 512바이트)의 배수인지 검사 - 나머지가 0이 아니면 섹터 경계에 맞지 않는 값. */
		return -EINVAL;	/* [한국어] 섹터 미정렬 오프셋은 블록 계층이 표현할 수 없으므로 -EINVAL 반환. */

	/* Validate size conversion from bytes to sectors */
	/* [한국어] reg의 뒷부분(a_cells 다음 s_cells개 셀)을 크기 값으로 변환해 검사하는 구간 시작. */
	size = of_read_number(reg + a_cells, s_cells);	/* [한국어] reg + a_cells는 __be32 포인터 산술이므로 a_cells * sizeof(__be32) 바이트만큼 전진 - 정확히 reg 배열에서 주소 셀들 다음, 크기 셀들이 시작하는 지점을 가리킨다. of_read_number()가 그 s_cells개 셀을 합성해 바이트 크기(u64)를 만든다. */
	if (!size || size % SECTOR_SIZE)	/* [한국어] 크기가 0이거나(파티션이 아무 공간도 차지하지 않음) SECTOR_SIZE의 배수가 아니면(섹터 경계 미정렬) 유효하지 않은 파티션 정의로 간주. */
		return -EINVAL;	/* [한국어] 위 두 조건 중 하나라도 참이면 -EINVAL 반환. */

	return 0;	/* [한국어] 여기까지 도달했다면 reg 형식과 섹터 정렬이 모두 유효함 - 검증 통과를 뜻하는 0을 반환. */
}

/*
 * [한국어]
 * add_of_partition - 검증된 파티션 서브노드 하나를 parsed_partitions에 등록한다.
 *
 * @state: 현재 디스크의 파티션 스캔 상태(parsed_partitions). state->parts[slot]과
 *         state->pp_buf(로그 버퍼)가 이 함수에 의해 갱신되는 대상이다.
 * @slot: 등록할 parts[] 슬롯 번호(파티션 번호). 호출자 of_partition()이 1부터
 *        시작해 자식 노드를 순회하며 증가시켜 넘겨준다.
 * @np: 등록할 파티션 서브노드. validate_of_partition()을 이미 통과해 reg가
 *      유효함이 보장된 노드다(같은 스캔 안의 1차 루프에서 이미 검증됨).
 * @return: 없음(void). 이 함수 자체는 실패를 보고하지 않는다 - reg 파싱이
 *          실패할 가능성은 validate_of_partition()이 이미 걸러냈다는 전제다.
 *
 * 이 함수가 왜 필요한가: validate_of_partition()이 "이 노드의 reg가 섹터
 * 정렬된 유효한 값"임을 보장한 뒤, 이 함수가 실제로 그 값을 섹터 단위로
 * 변환해 parsed_partitions.parts[slot]에 채워 넣고, read-only 플래그와
 * 파티션 이름(label 또는 폴백으로 노드 이름)까지 마저 채워 완전한 파티션
 * 엔트리 하나를 완성한다.
 * 동작 순서: (1) reg 프로퍼티를 다시 읽어 옴(검증 단계와는 별개의 두 번째
 * of_get_property() 호출 - 검증 결과를 캐싱해 전달받지 않고 재조회한다) ->
 * (2) 오프셋/크기를 SECTOR_SIZE로 나누어 섹터 단위로 변환 -> (3)
 * put_partition()으로 state->parts[slot].from/size에 기록 -> (4) "read-only"
 * 프로퍼티가 있으면 ADDPART_FLAG_READONLY 플래그를 OR로 세팅 -> (5) MTD 레이블
 * 관례를 따라 "label" 프로퍼티를 먼저 찾고, 없으면 "name"(노드 이름 프로퍼티)
 * 으로 폴백 -> (6) strscpy()로 volname 버퍼에 복사 -> (7) 로그 버퍼에
 * "(이름)"을 덧붙인다.
 * 실행 컨텍스트: 호출자와 동일한 디스크 파티션 스캔 단일 프로세스 컨텍스트.
 * 호출자: of_partition()의 2차(등록) 루프가 슬롯 한도(state->limit) 이내인
 * 각 자식 노드마다 한 번씩 호출한다.
 * 피호출자: of_get_property(), of_n_addr_cells(), of_n_size_cells(),
 * of_read_number(), put_partition(), of_property_read_bool(), strscpy(),
 * seq_buf_printf().
 * 에러 경로: 명시적 에러 반환이 없다. 다만 partname이 "label"과 "name" 둘
 * 다 없어 NULL로 남는 극단적 경우, 아래 strscpy(info->volname, partname, ...)
 * 호출에 NULL이 그대로 전달될 수 있는데 이는 이 함수도 validate_of_partition()
 * 도 검증하지 않는 부분이다 - 디바이스 트리 자체가 두 프로퍼티 중 최소 하나는
 * 제공한다는 암묵적 전제에 의존하는 것으로 보인다(추정).
 *
 * 호출 체인:
 *   of_partition(2차 등록 루프) -> [이 함수] -> put_partition
 */
static void add_of_partition(struct parsed_partitions *state, int slot,
			     struct device_node *np)
{
	struct partition_meta_info *info;	/* [한국어] info: 아래에서 state->parts[slot].info의 주소를 받아 label/volname을 채우는 데 쓸 로컬 포인터. */
	const char *partname;	/* [한국어] partname: "label" 또는 "name" 프로퍼티에서 얻어 올 파티션 이름 문자열 포인터 - 디바이스 트리 자체가 소유한 버퍼를 가리키며 이 함수가 복사본을 만들지는 않는다(strscpy 호출 전까지는 참조만). */
	int len;	/* [한국어] len: of_get_property() 호출들이 돌려주는 프로퍼티 바이트 길이를 받는 공용 변수 - 이 함수에서는 검증에 쓰이지 않고 단지 API 시그니처가 요구하는 출력 인자로만 쓰인다. */

	const __be32 *reg = of_get_property(np, "reg", &len);	/* [한국어] validate_of_partition()에서와 동일한 방식으로 "reg" 프로퍼티를 다시 조회 - 이미 검증을 통과했으므로 여기서는 형식 오류 가능성을 별도로 재확인하지 않는다. */
	int a_cells = of_n_addr_cells(np);	/* [한국어] 이 노드의 reg를 해석할 주소 셀 수 - validate_of_partition()과 동일하게 np->parent의 "#address-cells"에서 유래. */
	int s_cells = of_n_size_cells(np);	/* [한국어] 이 노드의 reg를 해석할 크기 셀 수 - np->parent의 "#size-cells"에서 유래. */

	/* Convert bytes to sector size */
	/* [한국어] validate_of_partition()이 검증만 하고 버렸던 값을, 이번에는 실제로 블록 계층이 쓸 섹터 단위로 확정해 지역 변수 offset/size에 담는다. */
	u64 offset = of_read_number(reg, a_cells) / SECTOR_SIZE;	/* [한국어] 오프셋(바이트) = of_read_number(reg, a_cells) -> 그 결과를 SECTOR_SIZE(통상 512)로 나누어 절대 시작 섹터 번호(LBA)로 변환. 이미 validate_of_partition()이 나누어떨어짐을 보장했으므로 나눗셈에서 나머지가 버려지는 손실은 없다. */
	u64 size = of_read_number(reg + a_cells, s_cells) / SECTOR_SIZE;	/* [한국어] 크기(바이트) = of_read_number(reg + a_cells, s_cells) -> SECTOR_SIZE로 나누어 파티션 길이(섹터 수)로 변환. */

	put_partition(state, slot, offset, size);	/* [한국어] check.h의 put_partition()을 호출해 state->parts[slot].from = offset, .size = size를 기록하고, 로그 버퍼(pp_buf)에 " <디스크이름><slot>" 형태의 접미사를 이어붙인다(put_partition 내부 구현 참고, slot이 state->limit 미만일 때만 실제로 채워짐). */

	if (of_property_read_bool(np, "read-only"))	/* [한국어] 이 파티션 서브노드에 boolean 프로퍼티 "read-only"가 존재하는지 검사(값 유무만 확인하는 of_property_read_bool - 존재 자체가 참). */
		state->parts[slot].flags |= ADDPART_FLAG_READONLY;	/* [한국어] 존재하면 block layer 공용 플래그 ADDPART_FLAG_READONLY(block/blk.h 정의)를 이 슬롯의 flags에 OR로 세팅 - 이후 이 파티션이 읽기 전용 block_device로 등록되게 만든다. */

	/*
	 * Follow MTD label logic, search for label property,
	 * fallback to node name if not found.
	 */
	/* [한국어] 위 원본 주석 보강: MTD(Memory Technology Device) fixed-partition 바인딩의 관례를 그대로 따른다는 뜻 - "label" 프로퍼티가 있으면 그것을 파티션 이름으로 쓰고, 없으면 디바이스 트리 노드 자체의 이름(예: "partition@100000"의 "@" 앞부분, of_get_property(np, "name", ...)이 돌려주는 값)으로 대체한다. */
	info = &state->parts[slot].info;	/* [한국어] 방금 put_partition()이 채운 슬롯의 partition_meta_info(uuid/volname 등 확장 메타데이터) 필드 주소를 잡아 info에 저장 - 아래에서 반복 접근할 때 편의를 위한 로컬 별칭. */
	partname = of_get_property(np, "label", &len);	/* [한국어] 우선순위 1순위: "label" 프로퍼티를 조회 - 있으면 partname이 그 문자열을 가리키게 됨(len에는 문자열 바이트 길이가 부수적으로 채워짐, 이 함수에서는 사용 안 함). */
	if (!partname)	/* [한국어] "label"이 없어 partname이 NULL인 경우(of_get_property()는 프로퍼티가 없으면 NULL을 반환) - 폴백 분기로 진입. */
		partname = of_get_property(np, "name", &len);	/* [한국어] 우선순위 2순위: 디바이스 트리 노드 자체의 "name" 프로퍼티(관례상 노드의 유닛 이름과 동일)로 대체. */
	strscpy(info->volname, partname, sizeof(info->volname));	/* [한국어] strscpy()로 partname이 가리키는 문자열을 info->volname(고정 크기 버퍼, partition_meta_info 정의)에 NUL 종단 복사 - 원본 길이가 버퍼보다 길면 잘린다. label과 name 모두 없다면 partname은 여전히 NULL이며, 이 경우는 위 함수 docblock의 에러 경로 설명대로 전제(디바이스 트리가 최소 하나는 제공)에 의존한다(추정). */

	seq_buf_printf(&state->pp_buf, "(%s)", info->volname);	/* [한국어] 파티션 스캔 결과 로그 버퍼(check_partition()이 나중에 printk로 한 줄 출력)에 "(이름)" 형태를 이어붙임 - put_partition()이 이미 붙인 " <디스크이름><slot>" 뒤에 붙어 예를 들어 " mmcblk0p1(rootfs)"처럼 보이게 된다. */
}

/*
 * [한국어]
 * of_partition - block layer 파티션 스캐너가 호출하는 이 파일의 유일한 진입점.
 *
 * @state: 스캔 대상 디스크에 대한 parsed_partitions 상태(core.c의
 *         check_partition()이 할당해 넘겨준다). state->disk로 대상 gendisk에
 *         접근하고, state->limit으로 최대 등록 가능한 파티션 수(DISK_MAX_PARTS)
 *         를 안다.
 * @return: 0이면 이 디스크에 "fixed-partitions" 호환 노드가 없음(내 파티션
 *          테이블이 아님, check_partition()이 다른 프로버를 계속 시도해야
 *          함). -1이면 파티션 서브노드 중 하나라도 reg 검증에 실패한 치명적
 *          오류(check.h 공용 계약상 음수는 통상 I/O 오류를 뜻하지만, 여기서는
 *          "디바이스 트리 형식 오류"를 나타내는 데 재사용된다). 1이면 성공
 *          (등록된 파티션이 0개일 수도 있다 - 자식이 없는 "fixed-partitions"
 *          노드라면 두 루프 모두 아무 것도 하지 않고 그대로 1을 반환한다).
 *
 * 이 함수가 왜 필요한가: block/partitions/core.c의 check_part[] 배열은
 * 파티션 포맷마다 "int (*)(struct parsed_partitions *)" 시그니처의 프로버
 * 함수를 등록해 순서대로 호출한다. 이 함수가 그 배열에 들어가는, 디바이스
 * 트리 fixed-partitions 방식을 대표하는 프로버다. 온디스크 파티션 테이블이
 * 전혀 없는 장치(eMMC, 임베디드 ARM/PowerPC 보드의 온보드 플래시 등)에서도
 * 부트펌웨어가 심어 둔 디바이스 트리 노드만으로 "<디스크이름>p1" 같은
 * 파티션 block_device를 만들 수 있게 한다.
 * 동작 순서: (1) disk_to_dev(state->disk)로 gendisk의 기반 struct device를
 * 얻고, 그 of_node를 of_node_get()으로 참조 카운트를 올려 가져옴 -> (2) 노드가
 * 없거나 "fixed-partitions"와 호환되지 않으면 즉시 0 반환(이 디스크는 대상이
 * 아님) -> (3) 1차 루프: 슬롯 1부터 시작해 모든 자식 노드를 순회하며
 * validate_of_partition()으로 전수 검증 - 하나라도 실패하면 지금 보고 있던
 * 자식과 부모 두 노드의 참조를 of_node_put()으로 반납하고 -1 반환(부분 등록
 * 방지) -> (4) 2차 루프: 슬롯을 다시 1부터 시작해 자식 노드를 순회하며
 * state->limit을 넘지 않는 한 add_of_partition()으로 실제 등록 -> (5) 로그
 * 버퍼를 개행으로 마무리하고 1 반환.
 * 두 번 순회하는 이유(2-pass 설계): 등록(add_of_partition)은 되돌리기 어려운
 * 부수효과(parts[] 갱신, 로그 버퍼 누적)를 일으키므로, 파티션 집합 전체가
 * 유효하다는 것을 먼저 확인한 뒤에야 실제 등록을 시작해 "일부만 등록된 채로
 * 중간에 실패"하는 상황을 피한다.
 * 실행 컨텍스트: 디스크 프로브/파티션 (재)스캔 경로의 프로세스 컨텍스트.
 * 디스크마다 한 번씩 호출된다.
 * 호출자: block/partitions/core.c의 check_partition() - check_part[] 테이블을
 * 통한 함수 포인터 호출.
 * 피호출자: disk_to_dev(), of_node_get(), of_device_is_compatible(),
 * for_each_child_of_node(매크로, 내부적으로 of_get_next_child() 호출),
 * validate_of_partition(), of_node_put(), add_of_partition(), seq_buf_puts().
 * 에러 경로: 1차 루프에서 검증 실패 시 -1을 반환해 check_partition()이 이를
 * 기억해 두었다가 다른 모든 프로버도 실패하면 최종 에러로 승격시킨다. DT
 * 노드 자체가 없거나 호환되지 않으면 정상적인 "내 파티션 테이블 아님"으로
 * 간주해 0을 반환한다. 2차 루프에서 state->limit에 도달하면 에러가 아니라
 * 현재 자식 노드에 대해서만 of_node_put()한 뒤 조용히 break한다.
 * 참조 카운트 관찰(추정): for_each_child_of_node 매크로는 내부적으로
 * of_get_next_child()가 이전 노드를 of_node_put()하고 다음 노드를
 * of_node_get()하므로, 루프가 정상적으로 끝까지(NULL 반환) 순회를 마치면 각
 * np는 자동으로 정리된다 - 그래서 두 루프의 정상 종료 경로에는 np에 대한
 * 별도 of_node_put()이 없다. 다만 최초에 of_node_get(ddev->of_node)로 얻은
 * partitions_np 자신은, 1차 루프에서 검증 실패로 조기 반환하는 경로에서만
 * of_node_put(partitions_np)로 명시적으로 반납되고, 정상 종료(return 1) 및
 * "slot >= state->limit" 조기 break 경로에서는 반납되지 않는다 - 이 두 경로가
 * 호출될 때마다 partitions_np의 참조 카운트가 하나씩 남는 것으로 보인다(이
 * 파일만으로는 100% 확정할 수 없어 추정으로 남긴다).
 *
 * 호출 체인:
 *   check_partition (block/partitions/core.c) -> [이 함수] ->
 *     validate_of_partition
 *     add_of_partition -> put_partition
 */
int of_partition(struct parsed_partitions *state)
{
	struct device *ddev = disk_to_dev(state->disk);	/* [한국어] disk_to_dev(): gendisk를 감싸는 struct device 포인터를 얻는다 - gendisk 자체가 아니라 그 기반 struct device에 of_node(디바이스 트리 노드 포인터) 필드가 있기 때문에 필요한 변환. */
	struct device_node *np;	/* [한국어] np: 아래 두 for_each_child_of_node 루프가 매 반복마다 현재 자식 노드를 담을 순회용 지역 변수. */
	int slot;	/* [한국어] slot: parsed_partitions.parts[] 슬롯 인덱스 - 1차/2차 루프에서 각각 1부터 다시 시작해 사용된다(아래에서 초기화). */

	struct device_node *partitions_np = of_node_get(ddev->of_node);	/* [한국어] ddev->of_node(이 디스크의 기반 struct device에 연결된 디바이스 트리 노드)의 참조 카운트를 of_node_get()으로 하나 올리며 로컬 포인터 partitions_np에 저장 - 이 함수 실행 동안 노드가 해제되지 않도록 보장. ddev->of_node가 NULL이면 of_node_get(NULL)은 NULL을 그대로 반환한다(관례). */

	if (!partitions_np ||	/* [한국어] partitions_np가 NULL이면(디바이스 트리 자체에 이 디바이스용 노드가 없음) 아래 두 번째 조건은 단락 평가(||)로 검사되지 않고 바로 참으로 판정됨. */
	    !of_device_is_compatible(partitions_np, "fixed-partitions"))	/* [한국어] of_device_is_compatible()로 partitions_np의 "compatible" 프로퍼티에 "fixed-partitions" 문자열이 포함돼 있는지 검사 - 이 문자열이 없으면 이 노드는 우리가 다룰 대상이 아니다. */
		return 0;	/* [한국어] 두 조건 중 하나라도 참이면(노드 없음 또는 호환성 불일치) 이 디스크는 디바이스 트리 fixed-partitions 대상이 아니라는 뜻 - 0을 반환해 check_partition()이 다른 프로버를 계속 시도하게 한다. 이 경로에서는 partitions_np에 대한 of_node_put()이 없는데, partitions_np가 NULL이면 애초에 참조를 얻지 않았으므로 문제 없고, non-NULL인데 compatible만 불일치한 경우는 여기서도 참조가 반납되지 않는다(추정 - 위 함수 docblock의 참조 카운트 관찰 참고). */

	slot = 1;	/* [한국어] 1차(검증) 루프에서 쓸 슬롯 카운터를 1로 초기화 - 슬롯 0은 관례적으로 whole-disk 자리라 파티션 번호는 1부터 매겨진다. */
	/* Validate parition offset and size */
	/* [한국어] 위 원본 주석 보강("Validate parition offset and size", 원문 그대로 "parition" 오탈자 포함): 실제 등록(add_of_partition)에 앞서 모든 자식 노드의 reg가 유효한지부터 전수 검사하는 1차 루프 시작. */
	for_each_child_of_node(partitions_np, np) {	/* [한국어] for_each_child_of_node 매크로 - partitions_np의 모든 자식을 순회하며 np에 대입한다. 내부적으로 of_get_next_child()가 이전 np를 of_node_put()하고 다음 노드를 of_node_get()하므로, 루프 밖에서 별도로 np를 해제할 필요가 없다(단, 중간에 break/return하면 현재 np는 수동으로 반납해야 함 - 아래 참고). */
		if (validate_of_partition(np, slot)) {	/* [한국어] 이번 자식 노드의 reg가 유효한지 validate_of_partition()에 위임 - 0이 아니면(즉 -EINVAL이면) 검증 실패. */
			of_node_put(np);	/* [한국어] 검증 실패로 루프를 조기 종료하므로, 자동 정리(위 for_each_child_of_node 설명)가 일어나지 않는 현재 np의 참조를 수동으로 반납. */
			of_node_put(partitions_np);	/* [한국어] 부모 노드 partitions_np의 참조도 반납 - 이 함수가 얻어 두었던 참조를 여기서 확실히 되돌린다(정상 종료 경로와 대비되는 지점, 위 docblock 참고). */

			return -1;	/* [한국어] check.h 공용 계약상 음수(-1)를 반환해 check_partition()에게 "이 프로버가 치명적으로 실패했다"고 알림 - 이후 다른 프로버들의 시도는 계속되지만, 모두 실패하면 이 음수가 최종 에러로 승격될 수 있다. */
		}

		slot++;	/* [한국어] 이번 자식은 검증을 통과했으므로 다음 반복에서 쓸 슬롯 번호를 하나 증가 - 실제 등록은 하지 않고 슬롯 계산만 미리 해 두는 것(2차 루프에서 동일한 순서로 다시 순회하며 그대로 재사용됨). */
	}

	slot = 1;	/* [한국어] 2차(등록) 루프를 위해 슬롯 카운터를 다시 1로 리셋 - 1차 루프에서 이미 증가시켜 둔 값을 버리고 동일한 자식 목록을 처음부터 재순회하기 위함. */
	for_each_child_of_node(partitions_np, np) {	/* [한국어] partitions_np의 자식들을 다시 처음부터 순회 - 1차 루프에서 이미 유효성이 확인된 동일한 노드 집합이므로, 이번에는 검증 없이 바로 등록만 수행한다. */
		if (slot >= state->limit) {	/* [한국어] 이번 슬롯이 parsed_partitions가 허용하는 최대치(state->limit, DISK_MAX_PARTS)에 도달했는지 검사 - 파티션 서브노드 수가 배열 한도를 넘는 비정상적인 디바이스 트리를 방어. */
			of_node_put(np);	/* [한국어] 한도 초과로 루프를 조기 종료(break)하므로, for_each_child_of_node의 자동 정리가 일어나지 않는 현재 np의 참조를 수동으로 반납. */
			break;	/* [한국어] 더 이상 등록할 슬롯이 없으므로 나머지 자식 노드들은 포기하고 루프를 종료 - 에러가 아니라 "그만 등록하라"는 조용한 신호(반환값에 반영되지 않음, 아래에서 그대로 1을 반환). */
		}

		add_of_partition(state, slot, np);	/* [한국어] 실제 등록을 add_of_partition()에 위임 - state->parts[slot]에 from/size/flags/volname을 채우고 로그 버퍼에 이름을 덧붙인다. */

		slot++;	/* [한국어] 다음 반복(다음 자식 노드)에서 쓸 슬롯 번호를 하나 증가. */
	}

	seq_buf_puts(&state->pp_buf, "\n");	/* [한국어] check_partition()이 나중에 printk로 출력할 로그 버퍼(pp_buf)를 개행 문자로 마무리 - 지금까지 " <디스크이름>p1(이름1) p2(이름2)..." 형태로 누적돼 있었다. */

	return 1;	/* [한국어] 성공 - 이 프로버가 파티션 테이블을(비어 있을 수도 있지만) 제공했음을 core.c에 알린다. */
}
