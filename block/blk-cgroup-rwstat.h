/* SPDX-License-Identifier: GPL-2.0
 *
 * Legacy blkg rwstat helpers enabled by CONFIG_BLK_CGROUP_RWSTAT.
 * Do not use in new code.
 *
 * [한국어 설명] blkcg(블록 I/O cgroup)의 read/write(및 sync/async,
 * discard) 방향별 누적 통계를 percpu 카운터로 관리하는 레거시 통계
 * 유틸리티 헤더 (blk-cgroup-rwstat.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 CONFIG_BLK_CGROUP_RWSTAT 커널 옵션이 켜졌을 때만 컴파일
 * 되는 "레거시" blkg(struct blkcg_gq — block cgroup과 request_queue를
 * 잇는 자료구조) 통계 헬퍼 모음이다. 하나의 blkg에 대해 발행되는
 * I/O를 READ/WRITE/SYNC/ASYNC/DISCARD 5개 방향(class)으로 나누어
 * percpu_counter(CPU별 로컬 카운터 + 필요 시 전역 합산)로 누적하고,
 * blkg가 제거될 때 사라지는 값을 상위로 이관하는 aux_cnt까지 포함한다.
 * blk-throttle, BFQ 등 legacy 정책이 "이 cgroup이 지금까지 얼마나
 * read/write/discard 했는가"를 계량하기 위한 회계 장부 역할을 한다.
 * 파일 최상단 원본 주석 "Do not use in new code"가 시사하듯, 최신
 * blkcg 통계는 blk-cgroup.h의 blkg_iostat_set 계열로 이관되었고, 이
 * rwstat 계열은 아직 이관되지 않은 일부 legacy 정책을 위해 유지된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 제출(쓰기) 경로: submit_bio() -> blk_mq_submit_bio() -> bio가
 * request로 변환되는 과정에서 blk_account_io_start() 또는 각 정책의
 * bio 훅(policy->pd_bio_fn 유사 경로) -> blkg_rwstat_add() 순서로
 * 호출되어, 방금 제출된 bio/request의 방향 플래그(opf)에 따라 알맞은
 * 카운터가 갱신된다. 조회(읽기) 경로: 사용자가 cgroupfs의
 * /sys/fs/cgroup/<cgroup>/io.stat 류 파일을 read(2)하면
 * cgroup_seqfile_show -> blkcg_policy->pd_stat 혹은 legacy
 * blkcg_print_blkgs -> blkg_prfill_rwstat()/blkg_rwstat_recursive_sum()
 * -> blkg_rwstat_read_counter()/__blkg_prfill_rwstat() 순으로 이
 * 파일의 함수가 호출되어 seq_file에 텍스트로 기록된다. 실행 컨텍스트는
 * 항상 커널 내부(블록 계층)이며, 제출 경로는 I/O를 낸 프로세스/워크큐
 * 컨텍스트에서, 조회 경로는 cgroupfs를 read()하는 유저 프로세스의
 * 시스템 콜 컨텍스트(커널 진입 후)에서 각각 실행된다. NVMe 스택
 * 관점에서는 nvme_queue_rq()가 SQ(Submission Queue)에 커맨드를 쓰기
 * 이전, 공통 blk-mq 계층에서 이미 blkg_rwstat_add()가 호출되어 방향별
 * 바이트/섹터 수가 누적된 뒤이므로, 이 파일은 NVMe 드라이버와 직접
 * 링크되지는 않고 모든 블록 디바이스가 공유하는 계량 지점이다.
 *
 * === 타 모듈과의 연결 ===
 * 이 헤더는 block/blk-cgroup.h에 정의된 struct blkcg_gq(블록 그룹과
 * 큐의 결합), struct blkcg_policy, struct blkcg_policy_data 위에서
 * 동작하며, 그 구조체들의 실제 정의는 blk-cgroup.h가 소유하고 이
 * 파일은 그 위에 얹히는 통계 "필드 타입"(struct blkg_rwstat)과 조작
 * 함수만 제공한다. 여기 선언된 non-inline 함수(blkg_rwstat_init,
 * blkg_rwstat_exit, __blkg_prfill_rwstat, blkg_prfill_rwstat,
 * blkg_rwstat_recursive_sum)의 실제 구현 본체는 짝이 되는
 * block/blk-cgroup-rwstat.c에 있다. 소비자(caller) 측에서는
 * block/bfq-iosched.c, block/blk-throttle.c 등 legacy 정책이 자신의
 * policy_data 구조체 안에 struct blkg_rwstat 필드를 내장하고
 * blkg_rwstat_add()로 갱신하며, blkg_prfill_rwstat()이나
 * blkg_rwstat_recursive_sum()으로 io.stat류 파일에 값을 노출한다.
 * 데이터 흐름: "bio/request의 opf 플래그 -> blkg_rwstat_add(rwstat,
 * opf, val) -> percpu_counter 누적 -> (blkg 제거 시)
 * blkg_rwstat_add_aux()로 aux_cnt 이관 -> blkg_rwstat_read_counter()
 * 또는 blkg_rwstat_recursive_sum()으로 합산 -> seq_file 문자열 ->
 * 유저 공간" 순서로 이어진다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct blkg_rwstat: percpu_counter cpu_cnt[5] + atomic64_t
 *   aux_cnt[5]. 하나의 blkg가 갖는 방향별(R/W/SYNC/ASYNC/DISCARD)
 *   통계 본체.
 * - struct blkg_rwstat_sample: cpu_cnt를 합산해 담아두는 읽기 전용
 *   스냅숏(u64 cnt[5]).
 * - blkg_rwstat_init()/blkg_rwstat_exit(): percpu_counter 자원의
 *   할당/해제 — blkg policy_data 생성/소멸 시점에 대응한다.
 *   이 트리의 실제 사용처는 BFQ(block/bfq-cgroup.c의 bfq_stat_init/free
 *   경로)이며, blk-throttle은 출력 헬퍼(blkg_prfill_rwstat)만 쓴다.
 * - blkg_rwstat_add(): opf(REQ_OP + 플래그)를 보고 알맞은 카운터에
 *   val을 누적하는 유일한 "쓰기" 진입점.
 * - blkg_rwstat_read()/blkg_rwstat_total(): 현재 값을 스냅숏하거나
 *   Read+Write 합계만 뽑아내는 조회 헬퍼.
 * - blkg_rwstat_reset(): 모든 카운터를 0으로 재설정(cgroup 재구성
 *   시 사용).
 * - blkg_rwstat_add_aux(): 제거되는 blkg의 통계를 상위(부모) blkg의
 *   aux_cnt로 이관해 누적값 유실을 막는다.
 * - blkg_prfill_rwstat()/__blkg_prfill_rwstat()/
 *   blkg_rwstat_recursive_sum(): cgroupfs seq_file 출력과 하위 트리
 *   재귀 합산을 담당하는 조회/출력 계층.
 *
 * === NVMe 관점 보충 설명 ===
 * 이 헤더는 블록 계층 공통 코드이지만, 카운터가 분류하는 기준이 결국
 * REQ_OP_* 연산 코드이므로 NVMe 커맨드와의 대응이 결정적으로 정해진다.
 * drivers/nvme/host/core.c의 nvme_setup_cmd() switch 문에서 확인할 수 있는
 * 대응 관계는 다음과 같다:
 *   REQ_OP_READ         → nvme_setup_rw(..., nvme_cmd_read)   = 옵코드 0x02
 *   REQ_OP_WRITE        → nvme_setup_rw(..., nvme_cmd_write)  = 옵코드 0x01
 *   REQ_OP_WRITE_ZEROES → nvme_setup_write_zeroes()           = 옵코드 0x08
 *   REQ_OP_DISCARD      → nvme_setup_discard()  = Dataset Management 0x09
 *   REQ_OP_FLUSH        → nvme_setup_flush()                  = 옵코드 0x00
 *
 * 그리고 blkg_rwstat 쪽 분류는 op_is_write()/op_is_discard()/op_is_sync()가
 * 담당하므로 결과적으로:
 *   BLKG_RWSTAT_READ    ← NVMe Read(0x02)
 *   BLKG_RWSTAT_WRITE   ← NVMe Write(0x01), Write Zeroes(0x08)
 *   BLKG_RWSTAT_DISCARD ← NVMe Dataset Management(0x09, Deallocate 속성)
 *   BLKG_RWSTAT_SYNC / _ASYNC ← REQ_SYNC 플래그 유무에 따라 이중 누적
 * (Compare 0x05는 블록 계층의 일반 I/O 경로로 오지 않고 passthrough로만
 *  발행되므로 이 통계에 잡히지 않는다.)
 *
 * per-CPU 카운터 구조의 목적은 명확하다: 여러 CPU가 동시에 같은 blkg
 * (예: 하나의 NVMe 네임스페이스에 대한 한 cgroup의 I/O)에 제출할 때,
 * 공유 카운터 하나를 원자적으로 갱신하면 그 캐시라인이 CPU 사이를 계속
 * 오가며(false sharing) 제출 경로의 병목이 된다. CPU마다 자기 카운터를
 * 갱신하고 읽을 때만 합산하면 이 경합이 사라진다. 수백만 IOPS를 내는
 * NVMe에서는 이 차이가 실측으로 나타난다.
 *
 * 연관 파일: block/blk-cgroup.h (blkcg 구조체 및 정책 정의),
 * block/blk-cgroup-rwstat.c (본 헤더 선언 함수들의 실제 구현체)
 */
#ifndef _BLK_CGROUP_RWSTAT_H
#define _BLK_CGROUP_RWSTAT_H

/* 이 파일은 block/blk-cgroup.h에 정의된 blkcg 구조체/정책을 기반으로
 * I/O 통계 헬퍼를 제공한다. */
#include "blk-cgroup.h"

/*
 * [한국어]
 * enum blkg_rwstat_type - struct blkg_rwstat의 카운터 배열을 인덱싱하는
 * 방향(class) 상수 모음
 *
 * 이 열거형은 struct blkg_rwstat.cpu_cnt[]/aux_cnt[] 두 배열의 첨자로만
 * 쓰이는 순수 인덱스 상수들이다. blkg_rwstat_add()가 bio/request의
 * opf(REQ_OP + 플래그)를 해석해 이 중 하나(READ/WRITE/DISCARD)와,
 * 별도로 SYNC/ASYNC 중 하나에 이중으로 값을 누적시킨다. 즉 하나의
 * I/O는 "방향(R/W/D)" 카운터 1개 + "동기성(SYNC/ASYNC)" 카운터 1개,
 * 총 2개의 슬롯에 동시에 반영된다. BLKG_RWSTAT_NR는 배열 크기이자
 * for 루프의 상한으로 쓰이고, BLKG_RWSTAT_TOTAL은 "전체 카운터 개수"
 * 라는 의미로 NR의 별칭(alias)이다.
 */
enum blkg_rwstat_type {
	BLKG_RWSTAT_READ,
	/* [한국어] READ 방향 통계 슬롯의 배열 인덱스(값 0).
	 * 설정자: blkg_rwstat_add()가 op_is_discard()/op_is_write()가 모두
	 *   거짓일 때(즉 REQ_OP_READ 계열) 이 인덱스의 cpu_cnt[]에 val을 더함.
	 * 읽는 자: blkg_rwstat_read()/blkg_rwstat_total()/
	 *   blkg_rwstat_recursive_sum()이 순회하며 참조.
	 * 값 범위: 컴파일 타임 상수 0 (배열 첨자 전용, 동적으로 변하지 않음).
	 * 동기화: 인덱스 상수 자체는 동기화 대상이 아니며, 가리키는
	 *   percpu_counter의 동기화는 percpu_counter 자체 규약을 따름.
	 * NVMe 관점: NVMe Read(옵코드 0x02) / Compare(0x05) 등
	 *   "읽기 계열" 커맨드가 이 인덱스로 집계된다. */
	BLKG_RWSTAT_WRITE,
	/* [한국어] WRITE 방향 통계 슬롯의 배열 인덱스(값 1).
	 * 설정자: blkg_rwstat_add()가 op_is_write(opf)가 참일 때 이 인덱스의
	 *   cpu_cnt[]에 val을 더함.
	 * 읽는 자: blkg_rwstat_read()/blkg_rwstat_total()/
	 *   blkg_rwstat_recursive_sum()이 순회하며 참조 — blkg_rwstat_total()은
	 *   READ와 이 WRITE 두 인덱스만 합산해 반환한다.
	 * 값 범위: 컴파일 타임 상수 1 (배열 첨자 전용).
	 * 동기화: 위 READ 항목과 동일 — percpu_counter 규약을 따름.
	 * NVMe 관점: NVMe Write(0x01) 및 Write Zeroes(0x08)가 이
	 *   인덱스로 집계된다. */
	BLKG_RWSTAT_SYNC,
	/* [한국어] SYNC(동기 제출) 통계 슬롯의 배열 인덱스(값 2).
	 * 설정자: blkg_rwstat_add()가 op_is_sync(opf)가 참일 때(REQ_SYNC
	 *   플래그가 설정된 제출) 이 인덱스의 cpu_cnt[]에 val을 더함 — READ/
	 *   WRITE/DISCARD 누적과는 독립적인 "두 번째" 누적으로, 같은 I/O가
	 *   방향 카운터와 SYNC 카운터에 동시에 반영될 수 있다.
	 * 읽는 자: blkg_rwstat_read()가 스냅숏에 포함시키지만,
	 *   blkg_rwstat_total()은 이 값을 사용하지 않는다(READ+WRITE만 합산).
	 * 값 범위: 컴파일 타임 상수 2.
	 * 동기화: percpu_counter 규약을 따름.
	 * NVMe 관점: REQ_SYNC가 실려 오는 제출 — 예를 들어 O_DIRECT
	 *   동기 쓰기나 fsync 유발 Flush(옵코드 0x00) 관련 경로의 특성을
	 *   반영한다고 볼 수 있다. */
	BLKG_RWSTAT_ASYNC,
	/* [한국어] ASYNC(비동기 제출) 통계 슬롯의 배열 인덱스(값 3).
	 * 설정자: blkg_rwstat_add()가 op_is_sync(opf)가 거짓일 때 이
	 *   인덱스의 cpu_cnt[]에 val을 더함 — SYNC와 상호 배타적인 짝.
	 * 읽는 자: blkg_rwstat_read()가 스냅숏에 포함; blkg_rwstat_total()은
	 *   사용하지 않음.
	 * 값 범위: 컴파일 타임 상수 3.
	 * 동기화: percpu_counter 규약을 따름.
	 * NVMe 관점: REQ_SYNC 플래그가 없는 일반 buffered Read/Write —
	 *   완료가 인터럽트(CQ, Completion Queue) 콜백으로 비동기 통지되는
	 *   전형적인 경로를 반영한다고 볼 수 있다. */
	BLKG_RWSTAT_DISCARD,
	/* [한국어] DISCARD(폐기/TRIM) 통계 슬롯의 배열 인덱스(값 4).
	 * 설정자: blkg_rwstat_add()가 op_is_discard(opf)가 참일 때 이
	 *   인덱스의 cpu_cnt[]에 val을 더함 — 이 분기가 else-if 체인의
	 *   최우선 조건이므로 discard는 write/read 인덱스에는 반영되지 않음.
	 * 읽는 자: blkg_rwstat_read()가 스냅숏에 포함하지만,
	 *   blkg_rwstat_total()은 discard를 제외한 R+W만 반환한다는 점에
	 *   주의(파일 하단 함수 주석 참고).
	 * 값 범위: 컴파일 타임 상수 4.
	 * 동기화: percpu_counter 규약을 따름.
	 * NVMe 관점: NVMe Dataset Management 커맨드(Deallocate 속성,
	 *   옵코드 0x09) — 리눅스 REQ_OP_DISCARD가 NVMe로 변환될 때
	 *   nvme_setup_discard()을 거쳐 도달하는 TRIM 계열 요청이 이 인덱스로
	 *   집계된다. */

	BLKG_RWSTAT_NR,
	/* [한국어] 위 5개 방향 상수의 총 개수(=5)이자 cpu_cnt[]/aux_cnt[]
	 * 배열의 실제 크기. 열거형 관례상 마지막 유효 값 다음에 오는 항목이
	 * 자동으로 "개수"를 나타내도록 값이 부여된다(명시적 초기화 없음 ->
	 * BLKG_RWSTAT_DISCARD + 1 = 5).
	 * 설정자: 없음(컴파일 타임 상수, 런타임에 변경 불가).
	 * 읽는 자: struct blkg_rwstat/blkg_rwstat_sample의 배열 선언
	 *   (cpu_cnt[BLKG_RWSTAT_NR] 등)과, blkg_rwstat_read()/reset()/
	 *   add_aux() 등 모든 순회 for 루프의 상한(i < BLKG_RWSTAT_NR)으로
	 *   사용됨.
	 * 값 범위: 컴파일 타임 상수 5, 향후 방향 클래스가 추가되면 자동
	 *   증가한다.
	 * 동기화: 해당 없음(상수). */
	BLKG_RWSTAT_TOTAL = BLKG_RWSTAT_NR,
	/* [한국어] BLKG_RWSTAT_NR의 의미상 별칭 — "배열 크기"라는 구현
	 * 관점 대신 "전체 카운터 개수"라는 조회자 관점의 이름을 제공하기
	 * 위한 상수. 값 자체는 BLKG_RWSTAT_NR과 완전히 동일(5)하다.
	 * 설정자: 없음(컴파일 타임 상수).
	 * 읽는 자: 이 헤더 안에서는 직접 참조하는 코드가 없고, blk-cgroup
	 *   관련 다른 파일이 "카운터가 몇 개인지"를 물을 때 BLKG_RWSTAT_NR 대신
	 *   이 이름을 쓸 수 있도록 제공된다. 현재 이 트리에서는
	 *   block/blk-cgroup-rwstat.c의 설명 주석에서만 언급되고 실제로
	 *   참조하는 코드는 없다 — API 대칭성을 위해 남겨 둔 이름이다.
	 * 값 범위: 컴파일 타임 상수 5.
	 * 동기화: 해당 없음(상수). */
};

/*
 * [한국어]
 * struct blkg_rwstat - blkcg 그룹(blkg) 하나의 방향별 I/O 통계 본체
 *
 * 하나의 struct blkg_rwstat 인스턴스는 보통 어떤 legacy blkcg_policy의
 * policy_data(pd) 구조체 안에 값 형태로 내장되어, 그 blkg가 지금까지
 * 발행한 I/O를 READ/WRITE/SYNC/ASYNC/DISCARD 5방향으로 나누어 추적한다.
 * cpu_cnt[]는 "현재 이 blkg에 실시간으로 누적 중인" 값이고, aux_cnt[]는
 * "이미 사라진 하위(자식) blkg로부터 승계받은" 값으로, 두 값을 더해야
 * (blkg_rwstat_read_counter() 참고) recursive(재귀적, 하위 트리 포함)
 * 통계의 정확한 합이 된다.
 *
 * 아래 원본 커널 주석이 이 구분을 설명한다:
 *   blkg_[rw]stat->aux_cnt is excluded for local stats but included for
 *   recursive.  Used to carry stats of dead children.
 * (한국어 번역: aux_cnt는 "local"(이 blkg 자신만의) 통계 조회에서는
 * 제외되고, "recursive"(하위 트리를 포함한) 통계 조회에서만 포함되며,
 * 이미 죽은(제거된) 자식 blkg들의 통계를 실어 나르는 용도로 쓰인다.)
 */
struct blkg_rwstat {
	struct percpu_counter		cpu_cnt[BLKG_RWSTAT_NR];
	/* [한국어] 현재 살아 있는 이 blkg 자신의 방향별 per-CPU 통계
	 * 카운터 배열. 인덱스는 enum blkg_rwstat_type(BLKG_RWSTAT_READ 등)
	 * 이며 BLKG_RWSTAT_NR(5)개의 percpu_counter가 들어 있다.
	 * 설정자: blkg_rwstat_add()가 percpu_counter_add_batch()로 값을
	 *   누적하고, blkg_rwstat_init()이 percpu_counter_init()으로 최초
	 *   할당하며(blk-cgroup-rwstat.c), blkg_rwstat_reset()이
	 *   percpu_counter_set(..., 0)으로 0 초기화한다.
	 * 읽는 자: blkg_rwstat_read_counter()/blkg_rwstat_read()가
	 *   percpu_counter_sum_positive()로 모든 CPU의 로컬 카운트를
	 *   합산해서 읽는다. blkg_rwstat_add_aux()도 자식->부모 이관 시
	 *   이 배열을 먼저 합산한 뒤 부모의 aux_cnt에 더한다.
	 * 값 범위: 논리적으로는 누적 바이트/섹터/요청 수(0 이상)이지만,
	 *   percpu_counter의 내부 표현이 배치(batch) 단위로 CPU 로컬에
	 *   더해지는 signed 값이라 중간 계산 과정에서 일시적으로 음수로
	 *   보일 수 있어, 읽을 때는 반드시 percpu_counter_sum_positive()
	 *   (음수를 0으로 clamp)를 사용한다.
	 * 동기화: percpu_counter 자체가 "CPU별 로컬 카운트 + 배치 임계값을
	 *   넘을 때만 갱신되는 전역 스핀락 보호 카운트"로 구성되어 있어,
	 *   여러 CPU가 동시에 percpu_counter_add_batch()를 호출해도 별도의
	 *   외부 락이 필요 없다. NVMe 관점에서는 여러 CPU가 동시에
	 *   같은 namespace(같은 blkg)에 I/O를 제출할 때 이 percpu 구조가
	 *   카운터 캐시라인 경합을 줄여준다. */

	atomic64_t			aux_cnt[BLKG_RWSTAT_NR];
	/* [한국어] 이미 제거된(하위 트리에서 사라진) 자식 blkg들의 통계를
	 * 실어 날라 보관하는 보조(auxiliary) 카운터 배열. 인덱스는 위
	 * cpu_cnt[]와 동일하게 enum blkg_rwstat_type을 사용한다.
	 * 설정자: blkg_rwstat_add_aux()가 atomic64_add()로 "자식의 현재
	 *   cpu_cnt 합산값 + 자식의 aux_cnt 값"을 더해 넣는다(자식이
	 *   제거될 때 BFQ의 정책 오프라인 경로에서 호출된다). blkg_rwstat_reset()이
	 *   atomic64_set(..., 0)으로 0 초기화한다.
	 * 읽는 자: blkg_rwstat_read_counter()가 percpu 합산값과 이 값을
	 *   더해 반환한다. 즉 aux_cnt는 이 함수를 거치는 모든 조회에 항상
	 *   포함되며, "local 값만" 보고 싶은 경로는 이 함수 대신
	 *   percpu_counter만 직접 합산하는 방식으로 구분한다.
	 *   트리 전체 합계가 필요한 경로는
	 *   blkg_rwstat_recursive_sum()(block/blk-cgroup-rwstat.c:544)이며,
	 *   하위 blkg들을 순회하며 이 함수를 반복 호출한다.
	 * 값 범위: 0 이상의 누적값. atomic64_t이므로 단일 값에 대한
	 *   원자적 read-modify-write가 보장된다.
	 * 동기화: atomic64_* 연산이 lock-free 원자성을 제공하므로, blkg
	 *   트리를 여러 경로에서 동시에 순회/삭제하며 부모의 aux_cnt를
	 *   갱신해도 데이터 레이스 없이 안전하다. NVMe 관점에서는
	 *   namespace/controller가 hot-remove 되거나 해당 blkg의 cgroup이
	 *   삭제될 때, 그때까지 누적된 SQ 제출 통계가 이 필드를 통해 상위
	 *   cgroup으로 승계되어 유실되지 않는다. */
};

/*
 * [한국어]
 * struct blkg_rwstat_sample - blkg_rwstat의 읽기 전용 스냅숏(snapshot)
 *
 * struct blkg_rwstat는 percpu_counter/atomic64_t로 구성되어 "살아있는"
 * 카운터이지만, cgroupfs로 값을 내보내거나(io.stat) 재귀 합산을 계산할
 * 때는 그 순간의 값을 일반 u64 배열로 떠서(snapshot) 고정시켜 두는 것이
 * 편리하다. 이 구조체가 그 스냅숏의 형태를 정의한다.
 */
struct blkg_rwstat_sample {
	u64				cnt[BLKG_RWSTAT_NR];
	/* [한국어] READ/WRITE/SYNC/ASYNC/DISCARD 5방향 통계를 한 시점에
	 * 고정한 스냅숏 배열. 인덱스는 enum blkg_rwstat_type을 그대로
	 * 사용한다.
	 * 설정자: blkg_rwstat_read()가 percpu_counter_sum_positive()의
	 *   결과를 이 배열에 채우고, blkg_rwstat_recursive_sum()이 하위
	 *   트리 전체를 순회하며 이 배열(호출자가 넘긴 sum 인자)에 값을
	 *   누적한다. 스택에 { } 초기화 후 채워 쓰는 것이 일반적 사용법
	 *   (blkg_rwstat_total() 참고).
	 * 읽는 자: blkg_rwstat_total()이 READ/WRITE 두 인덱스만 더해
	 *   반환하고, __blkg_prfill_rwstat()이 전체 5개 값을 seq_file에
	 *   포맷팅해 /sys/fs/cgroup/.../io.* 형태로 노출한다.
	 * 값 범위: 0 이상의 u64 누적값(스냅숏 시점의 고정값이므로 이후
	 *   원본 카운터가 바뀌어도 이 배열은 변하지 않는다).
	 * 동기화: 이 구조체 자체는 스냅숏이 채워진 이후에는 단일
	 *   스레드/스택 로컬 데이터로 취급되어 별도 동기화가 필요 없다 —
	 *   동기화가 필요한 시점은 스냅숏을 "뜨는" 순간(percpu_counter
	 *   읽기)뿐이며 그 책임은 원본 percpu_counter가 진다. */
};

/*
 * [한국어]
 * blkg_rwstat_read_counter - rwstat의 특정 방향 인덱스 하나에 대해
 * "현재 살아있는 값 + 승계받은 aux 값"을 합산해 읽는다
 *
 * @rwstat: 값을 읽을 대상 struct blkg_rwstat 포인터. blkg의
 *   policy_data 안에 내장된 인스턴스가 보통 전달된다.
 * @idx: enum blkg_rwstat_type 중 하나(BLKG_RWSTAT_READ 등). 배열
 *   경계를 벗어나지 않는 유효한 인덱스여야 하며(0 ~ BLKG_RWSTAT_NR-1),
 *   호출자가 그 유효성을 보장해야 한다(함수 내부에 범위 검사 없음).
 * @return: idx가 가리키는 방향의 누적 카운트(u64). 죽은 자식으로부터
 *   승계된 aux_cnt[idx]와, 현재 살아있는 CPU들의 percpu_counter 합을
 *   더한 값이므로 recursive(재귀적) 관점의 부분 합에 해당한다.
 *
 * 이 함수는 struct blkg_rwstat의 두 저장소(cpu_cnt, aux_cnt)를 하나의
 * u64 값으로 합쳐 보여주는 가장 기본적인 "단일 인덱스 읽기" 헬퍼다.
 * atomic64_read()로 aux_cnt를 먼저 읽고, percpu_counter_sum_positive()
 * 로 모든 CPU에 흩어진 cpu_cnt를 순회 합산한 뒤 더해서 반환한다.
 * percpu_counter_sum_positive()는 이름 그대로 합산 결과가 음수이면
 * 0으로 clamp하는 안전한 버전이라, 카운터가 배치 갱신 중간 상태라도
 * 호출자가 이상한 음수 통계를 보는 일이 없다. 실행 컨텍스트는
 * cgroupfs를 read()하는 유저 프로세스의 시스템 콜 컨텍스트(커널
 * 진입 후)이며, 원자적 읽기 연산들의 조합이라 별도의 락 없이도
 * 안전하지만 "정확히 그 순간"의 완벽한 스냅숏이라기보다는 근사치에
 * 가깝다(percpu_counter_sum이 CPU를 순회하는 동안 다른 CPU가 계속
 * 값을 갱신할 수 있으므로).
 * 이 트리에서 이 함수를 직접 호출하는 곳은 blkg_rwstat_recursive_sum()
 * (block/blk-cgroup-rwstat.c:544)이며, 하위 blkg들을 순회하며 각 카운터를
 * 합산할 때 쓴다. 하위로는 atomic64_read()와 percpu_counter_sum_positive()
 * 두 커널 API만 호출한다. 에러 경로는 없다 — 두 읽기 연산 모두 실패를
 * 반환하지 않는 단순 조회이기 때문이다.
 *
 * 호출 체인:
 *   cgroupfs read → policy->pd_stat / blkcg_print_blkgs
 *     → blkg_rwstat_recursive_sum → [blkg_rwstat_read_counter]
 *     → atomic64_read() / percpu_counter_sum_positive()
 *
 * NVMe 연결: /sys/fs/cgroup/.../io.* 파일에서 namespace 단위로
 * 보고되는 Read/Write/Discard 바이트/섹터 값 중 "한 방향"의 값이 이
 * 합산 결과를 그대로 사용한다.
 */
static inline u64 blkg_rwstat_read_counter(struct blkg_rwstat *rwstat,
		unsigned int idx)
{
	return atomic64_read(&rwstat->aux_cnt[idx]) +		/* [한국어] 제거된 자식 cgroup에서 넘겨받은 누적값(aux_cnt)을 먼저 읽는다.
		 * atomic64_read는 단일 값의 원자적 읽기라 락이 필요 없다. */
		percpu_counter_sum_positive(&rwstat->cpu_cnt[idx]);	/* [한국어] 살아 있는 blkg의 per-CPU 카운터를 전 CPU에 걸쳐 합산한다.
		 * _positive 변형은 음수 결과를 0으로 보정하는데, per-CPU 카운터는
		 * 갱신 중 순간적으로 음수가 보일 수 있기 때문이다. */
}

/*
 * [한국어]
 * blkg_rwstat_init - blkg_rwstat의 per-CPU 카운터 자원을 할당/초기화한다
 *
 * @rwstat: 초기화할 대상 struct blkg_rwstat 포인터. 보통 blkg의
 *   policy_data를 할당한 직후 그 안에 내장된 인스턴스를 넘긴다.
 * @gfp: percpu_counter_init() 등 내부 할당에 쓰일 GFP 플래그. 정책의
 *   BFQ의 bfq_pd_alloc() 경로에서 호출되며 GFP_KERNEL이 전달된다.
 * @return: 0이면 성공, 음수 errno(예: -ENOMEM)이면 percpu 메모리 할당
 *   실패. 호출자는 실패 시 방금 만든 policy_data 자체를 되돌리고 에러를
 *   전파해야 한다.
 *
 * blk-cgroup-rwstat.c에 구현된 non-inline 함수로, cpu_cnt[]의 각
 * percpu_counter를 percpu_counter_init() 등으로 초기화하고 aux_cnt[]를
 * 0으로 설정한다(구현은 block/blk-cgroup-rwstat.c).
 * 실행 컨텍스트는 blkg 또는 policy_data가 새로 생성되는 시점 —
 * 예를 들어 어떤 cgroup에 처음으로 이 legacy 정책이 활성화되거나,
 * 새 request_queue에 대해 blkg가 새로 생성될 때(blkg_alloc() 계열
 * 경로)이며, 이 시점은 아직 다른 코드가 이 rwstat을 참조하지 않으므로
 * 별도의 동기화가 필요 없다. 호출하는 상위 함수는 각 legacy
 * blkcg_policy(BFQ, blk-throttle 등)의 pd_alloc_fn/pd_init_fn류
 * 콜백이다 — 이 트리에서는 BFQ의 bfqg_stats_init()(block/bfq-cgroup.c:1278)이
 * 유일한 호출자다. 하위로는 percpu_counter_init() 계열 커널 API를
 * 호출한다. 에러 발생 시(-ENOMEM) 호출자는 이미 할당한 policy_data
 * 메모리를 해제하고 전체 blkg 생성 절차를 실패로 되돌린다.
 *
 * 호출 체인:
 *   blkcg_policy->pd_alloc_fn(BFQ/throttle 등) -> [blkg_rwstat_init]
 *   -> percpu_counter_init()
 *
 * NVMe 연결: nvme 디바이스를 뒷단으로 갖는 request_queue에 대해
 * blkg 또는 policy 데이터가 생성될 때 호출되어, 이후 해당
 * namespace/controller에 대한 SQ 제출 통계 누적을 위한 자원을
 * 마련한다.
 */
int blkg_rwstat_init(struct blkg_rwstat *rwstat, gfp_t gfp);	/* [한국어] BFQ 정책 데이터 생성 시 percpu_counter를 할당한다. */

/*
 * [한국어]
 * blkg_rwstat_exit - blkg_rwstat_init()이 할당한 per-CPU 카운터
 * 자원을 해제한다
 *
 * @rwstat: 해제할 대상 struct blkg_rwstat 포인터. blkg_rwstat_init()과
 *   반드시 짝을 이루어야 한다(생성자/소멸자 대칭).
 * @return: 없음(void). percpu_counter_destroy()만 호출하므로 실패 경로가 없다.
 *
 * blkg_rwstat_init()에서 percpu_counter_init()으로 할당했던 내부
 * 자원(각 CPU별 카운터 메모리 등)을 percpu_counter_destroy() 계열
 * API로 되돌리는 소멸자 역할의 함수다. 실행 컨텍스트는 blkg 또는
 * policy_data가 소멸되는 시점 — 예를 들어 cgroup이 삭제되거나
 * request_queue가 해제될 때(blkg_free() 계열 경로)이며, 이 시점에는
 * 더 이상 다른 경로에서 이 rwstat을 갱신하지 않는다고 가정하고
 * 동작한다(즉 호출자가 quiescence를 보장해야 함). 호출하는 상위
 * 함수는 BFQ의 bfqg_stats_exit()(block/bfq-cgroup.c)이며, 하위로
 * percpu_counter_destroy() 계열 API를 호출한다. 별도의 에러 반환
 * 경로는 없다.
 *
 * 호출 체인:
 *   blkcg_policy->pd_free_fn(BFQ/throttle 등) -> [blkg_rwstat_exit]
 *   -> percpu_counter_destroy()
 *
 * NVMe 연결: namespace가 제거되거나 controller가 hot-unplug
 * 되어 대응하는 blkg/policy_data가 소멸될 때 호출되어, 더 이상 필요
 * 없는 통계 자원을 회수한다. 단, 통계 "값" 자체는 이 시점 이전에
 * blkg_rwstat_add_aux()로 상위 cgroup에 이관되어 살아남는다.
 */
void blkg_rwstat_exit(struct blkg_rwstat *rwstat);		/* [한국어] blkcg_gq 소멸 시 percpu_counter 자원을 회수한다. */

/*
 * [한국어]
 * __blkg_prfill_rwstat - 이미 채워진 rwstat 스냅숏을 seq_file 출력
 * 포맷으로 기록한다
 *
 * @sf: 출력 대상 seq_file. cgroupfs가 /sys/fs/cgroup/.../io.* 파일을
 *   read(2)할 때 커널이 마련해 주는 시퀀스 파일 컨텍스트.
 * @pd: 이 통계가 속한 blkcg_policy_data. blkg(어느 디바이스/큐에
 *   대한 그룹인지)를 식별하는 데 쓰인다(예: 디바이스 번호 출력).
 * @rwstat: 이미 blkg_rwstat_read() 또는 blkg_rwstat_recursive_sum()
 *   등으로 채워진 struct blkg_rwstat_sample 스냅숏(읽기 전용).
 * @return: 이 blkg가 출력한 바이트 수(seq_file 관례상 총 누적값을
 *   반환하는 경우가 많음) 또는 0. 정확한 의미는 정책별
 *   prfill 콜백 관례를 따른다.
 *
 * 이 함수는 이미 방향별로 채워진 스냅숏(cnt[BLKG_RWSTAT_NR])을 받아
 * "rbytes=... wbytes=... dbytes=..." 형태의 사람이 읽을 수 있는
 * 텍스트로 변환해 seq_file에 쓰는 포맷팅 전용 함수다. 값을 새로
 * 계산하지 않고 순수하게 "이미 있는 스냅숏을 텍스트로 직렬화"하는
 * 책임만 가진다는 점이 이름의 "__"(내부/저수준) 접두사와 대칭되는
 * blkg_prfill_rwstat()(오프셋으로부터 스냅숏을 만들어 이 함수를
 * 호출하는 상위 래퍼)의 차이다. 실행 컨텍스트는 cgroupfs
 * read() 시스템 콜 컨텍스트이며, seq_file API 자체가 동시 read를
 * 직렬화하므로 이 함수 내부에서 별도 락이 필요 없다. 호출하는 상위
 * 함수는 blkg_prfill_rwstat() 또는 각 정책의 stat_show 콜백이며,
 * 하위로 seq_printf() 계열 API를 호출한다. 에러 경로는
 * 사실상 없다(출력 실패는 seq_file이 내부적으로 오버플로 처리).
 *
 * 호출 체인:
 *   blkcg_stat_show/policy->stat_show -> blkg_prfill_rwstat ->
 *   [__blkg_prfill_rwstat] -> seq_printf()
 *
 * NVMe 연결: 사용자가 /sys/fs/cgroup/.../io.stat 등을 읽을 때
 * namespace/controller별 Read/Write/Discard 바이트 합계가 최종
 * 텍스트로 변환되는 지점이다.
 */
u64 __blkg_prfill_rwstat(struct seq_file *sf, struct blkg_policy_data *pd,
			 const struct blkg_rwstat_sample *rwstat);	/* [한국어] rwstat 스냅숏을 "Read ... Write ... Discard ..." 텍스트로 포맷한다. */

/*
 * [한국어]
 * blkg_prfill_rwstat - blkg policy 데이터의 지정된 오프셋에서 rwstat을
 * 읽어 스냅숏을 만들고 __blkg_prfill_rwstat()로 포맷팅한다
 *
 * @sf: 출력 대상 seq_file (위와 동일한 cgroupfs 읽기 컨텍스트).
 * @pd: 이 blkg의 policy_data. 이 안 어딘가에 struct blkg_rwstat
 *   필드가 내장되어 있다.
 * @off: pd 구조체 시작 주소로부터 struct blkg_rwstat 필드까지의
 *   바이트 오프셋. 정책마다 policy_data 레이아웃이 다르므로, 어떤
 *   rwstat 필드를 읽을지는 이 오프셋으로 지정한다(offsetof() 로
 *   계산되어 호출자가 넘긴다).
 * @return: __blkg_prfill_rwstat()의 반환값을 그대로 전달.
 *
 * 이 함수는 "오프셋으로 필드를 찾아 읽고 -> 포맷팅"까지 한 번에
 * 처리하는 상위 래퍼로, cftype(cgroup 파일 타입)의 prfill 콜백
 * 시그니처에 바로 맞출 수 있는 형태다. pd 포인터에 off를 더해
 * struct blkg_rwstat*를 얻은 뒤(내부에서 (void *)pd + off 형태의
 * 포인터 산술을 한다), blkg_rwstat_read()로 스냅숏을 뜨고
 * __blkg_prfill_rwstat()에 넘겨 실제 출력을 위임한다. 실행 컨텍스트는
 * cgroupfs read() 시스템 콜 컨텍스트이며, 내부에서 호출하는
 * blkg_rwstat_read()가 percpu_counter를 순회하는 동안 특별한 락 없이
 * 근사치를 읽는다. 호출하는 상위 함수는 blkcg_print_blkgs() 같은
 * cgroup 통계 순회 루틴이며, 하위로 blkg_rwstat_read()와
 * __blkg_prfill_rwstat()을 호출한다. 에러 경로는 없다(off가 잘못되면
 * 정의되지 않은 동작이므로 이는 정책 구현자의 책임).
 *
 * 호출 체인:
 *   blkcg_print_blkgs -> [blkg_prfill_rwstat] -> blkg_rwstat_read()
 *   -> __blkg_prfill_rwstat -> seq_printf()
 */
u64 blkg_prfill_rwstat(struct seq_file *sf, struct blkg_policy_data *pd,
		       int off);		/* [한국어] blkcg_print_blkgs()가 blkg마다 호출하는 prfill 콜백.
 * 사용처: block/bfq-cgroup.c:2703, block/blk-throttle.c:2112 */

/*
 * [한국어]
 * blkg_rwstat_recursive_sum - 지정된 blkg를 루트로 하는 하위 cgroup
 * 트리 전체를 재귀적으로 순회하며 rwstat을 합산한다
 *
 * @blkg: 순회를 시작할 루트 blkg. 이 blkg 자신과 그 모든 자손
 *   cgroup에 대응하는 blkg들이 합산 대상이 된다.
 * @pol: 어떤 blkcg_policy의 rwstat을 합산할지 지정. blkg마다 여러
 *   정책의 policy_data가 붙어 있을 수 있으므로 정책을 특정해야 한다.
 * @off: blkg_prfill_rwstat()과 동일한 의미 — policy_data 내에서
 *   struct blkg_rwstat 필드까지의 바이트 오프셋.
 * @sum: 결과를 누적해 담을 출력 파라미터. 호출자가 보통
 *   `struct blkg_rwstat_sample sum = { }`로 0 초기화한 스택 변수의
 *   주소를 넘긴다.
 * @return: 없음(void). 결과는 @sum out-parameter를 통해 반환된다.
 *
 * cgroup 계층은 트리 구조이고, 상위 cgroup의 "recursive"(재귀) 통계는
 * 그 자신과 모든 하위 cgroup들의 합이어야 한다. 이 함수는 @blkg를
 * 루트로 트리를 내려가며 각 노드의 cpu_cnt(현재 값)와 aux_cnt(이미
 * 죽은 자식으로부터 승계된 값)를 모두 @sum에 더해, 트리 전체의 정확한
 * 합을 만든다. 자손 blkg를 찾기 위해 blkg->blkcg->css(cgroup_subsys_
 * state) 트리를 내려가는 css_for_each_descendant_pre() 계열 순회를
 * 쓴다. 실행 컨텍스트는 cgroupfs read() 시스템 콜
 * 컨텍스트이며, RCU 보호 하에 blkg 트리를 순회한다(cgroup
 * 서브시스템 관례). 호출하는 상위 함수는 각 정책의 stat_show 콜백
 * 이며, 하위로 blkg_rwstat_read_counter() 또는 유사한 합산 헬퍼와
 * cgroup 트리 순회 API를 호출한다. 에러 경로는 명시적으로 없으나,
 * 트리 순회 중 blkg가 사라지는 경쟁 상태는 RCU/참조 카운트로
 * 방어된다.
 *
 * 호출 체인:
 *   blkcg_stat_show/policy->stat_show -> [blkg_rwstat_recursive_sum]
 *   -> css_for_each_descendant_pre() -> blkg_rwstat_read_counter()
 *
 * NVMe 연결: 상위 cgroup 아래 여러 nvme_ctrl/namespace가
 * 자식 cgroup으로 걸려 있을 때, 이 함수가 그 전체를 통합해 상위
 * cgroup 단위의 Read/Write/Discard 총 사용량을 보고하는 데 쓰인다.
 */
void blkg_rwstat_recursive_sum(struct blkcg_gq *blkg, struct blkcg_policy *pol,
		int off, struct blkg_rwstat_sample *sum);	/* [한국어] 하위 cgroup 트리를 재귀 순회하며 통계를 누적한다.
 * 사용처: block/bfq-cgroup.c:2733, 2928 */


/*
 * [한국어]
 * blkg_rwstat_add - bio/request의 opf(REQ_OP + 플래그)를 보고 val을
 * 알맞은 방향 카운터 두 곳(방향 1개 + 동기성 1개)에 누적한다
 *
 * @rwstat: 값을 누적할 대상 struct blkg_rwstat. 보통 이 I/O가 속한
 *   blkg의 policy_data 안에 내장된 인스턴스.
 * @opf: blk_opf_t 타입의 REQ_OP(요청 오퍼레이션 종류) + 각종 REQ_*
 *   플래그 조합. op_is_discard()/op_is_write()/op_is_sync() 등 헬퍼로
 *   해석한다.
 * @val: 누적할 값 — 보통 이 I/O의 크기(바이트 또는 섹터 수). 커널
 *   원본 kernel-doc에는 "value to add"라고만 되어 있어 단위는
 *   호출자(정책)가 통일해서 넘긴다.
 * @return: 없음(void).
 *
 * 이 함수는 struct blkg_rwstat에 대한 사실상 유일한 "쓰기" 진입점
 * 이다. 하나의 I/O에 대해 이 함수가 호출되면 두 번의
 * percpu_counter_add_batch()가 일어난다 — 첫 번째는 방향 계열
 * (DISCARD/WRITE/READ 중 하나), 두 번째는 동기성 계열(SYNC/ASYNC 중
 * 하나)이다. 즉 하나의 write I/O는 WRITE 카운터와 SYNC 또는 ASYNC
 * 카운터 양쪽에 동시에 반영되어, 조회 시 "쓰기 총량"과 "동기 쓰기
 * 총량"을 각각 뽑아낼 수 있게 된다. 커널 원본 kernel-doc이 명시하듯
 * "The caller is responsible for synchronizing calls to this
 * function"이지만, 실제로는 percpu_counter_add_batch()가 CPU-로컬
 * 배치 갱신을 사용하므로 여러 CPU가 서로 다른 I/O에 대해 동시에 호출
 * 해도 데이터가 깨지지 않는다 — 이 주석이 말하는 "동기화"는 오히려
 * 상위 계층(blk-mq)이 같은 request/bio에 대해 중복 호출하지 않도록
 * 보장해야 한다는 의미에 가깝다. 실행 컨텍스트는 I/O를 제출한
 * 프로세스 또는 blk-mq 소프트웨어 큐 처리 컨텍스트이며, 인터럽트
 * 컨텍스트에서는 호출되지 않는다 — 이 트리의 호출자는 BFQ의 제출/완료
 * 경로(block/bfq-cgroup.c:733, 738, 1040, 1042)뿐이다.
 * 이 함수를 호출하는 상위 함수는 blk_account_io_start() 계열의 I/O
 * 계정(accounting) 훅이며, 하위로는 op_is_discard()/op_is_write()/
 * op_is_sync() 판별 헬퍼와 percpu_counter_add_batch()를 호출한다.
 * 별도의 에러 반환 경로는 없다(void 함수이며 실패할 수 없는 카운터
 * 누적 연산이기 때문).
 *
 * 호출 체인:
 *   blk_mq_submit_bio -> blk_mq_get_request ->
 *   blkcg_bio_issue_check()/blk_account_io_start() -> [blkg_rwstat_add]
 *   -> percpu_counter_add_batch()
 *
 * NVMe 연결:
 * - op_is_discard(): NVMe Dataset Management(Deallocate) 명령
 * - op_is_write():  NVMe Write 명령
 * - 그 외:          NVMe Read 명령
 * - op_is_sync():   SYNC 플래그가 설정된 submission (poll/flush 관련
 *                   동작 특성 반영)
 */
/**
 * blkg_rwstat_add - add a value to a blkg_rwstat
 * @rwstat: target blkg_rwstat
 * @opf: REQ_OP and flags
 * @val: value to add
 *
 * Add @val to @rwstat.  The counters are chosen according to @rw.  The
 * caller is responsible for synchronizing calls to this function.
 */
static inline void blkg_rwstat_add(struct blkg_rwstat *rwstat,
				   blk_opf_t opf, uint64_t val)
{
	struct percpu_counter *cnt;							/* 선택된 NVMe 명령 유형별 per-CPU 카운터 포인터 */

	/* NVMe Deallocate(Discard) 명령이면 DISCARD 카운터를 선택 */
	if (op_is_discard(opf))								/* REQ_OP_DISCARD: -> nvme_setup_discard() -> Dataset Management(0x09) */
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_DISCARD];		/* discard/deallocate 통계 누적 대상 설정 */
	/* NVMe Write 명령이면 WRITE 카운터를 선택 */
	else if (op_is_write(opf))							/* REQ_OP_WRITE / REQ_OP_WRITE_ZEROES: -> nvme_setup_rw() */
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_WRITE];			/* NVMe Write(0x01) 또는 Write Zeroes(0x08) 통계 누적 대상 */
	/* 그 외는 NVMe Read로 간주하여 READ 카운터를 선택 */
	else
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_READ];			/* [한국어] READ 카운터 선택. NVMe Read(옵코드 0x02)가 여기 해당한다.
		 * (Compare 0x05는 passthrough로만 발행되어 이 경로로 오지 않는다.) */

	/* 해당 방향 카운터에 요청량(섹터/바이트)을 누적 */
	percpu_counter_add_batch(cnt, val, BLKG_STAT_CPU_BATCH);	/* [한국어] per-CPU 배치 누적. BLKG_STAT_CPU_BATCH만큼 로컬에 모았다가
	 * 전역 카운터로 옮기므로, 공유 캐시라인 접근 빈도가 그만큼 줄어든다. */

	/* SYNC/ASYNC 플래그에 따라 추가로 Sync 또는 Async 카운터 누적 */
	/* [한국어] REQ_SYNC 여부로 두 번째 분류를 한다. 같은 I/O가 READ/WRITE/DISCARD
	 * 중 하나와 SYNC/ASYNC 중 하나에 "이중" 누적되므로, 다섯 카운터의 총합은
	 * 실제 I/O 수의 두 배가 된다. 조회 측이 이 규약을 알고 해석해야 한다. */
	if (op_is_sync(opf))
		/* [한국어] 동기 I/O — 제출자가 완료를 기다리는 요청(O_DIRECT 읽기,
		 * fsync 유발 쓰기 등). 지연에 민감하므로 별도로 집계한다. */
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_SYNC];
	else
		cnt = &rwstat->cpu_cnt[BLKG_RWSTAT_ASYNC];			/* async I/O: poll / interrupt CQ 완료와 연계된 통계 */

	percpu_counter_add_batch(cnt, val, BLKG_STAT_CPU_BATCH);	/* Sync/Async per-CPU 배치 누적: CQ 인터럽트 핸들러와 동일 cacheline 최소화 */
}

/*
 * [한국어]
 * blkg_rwstat_read - blkg_rwstat의 현재(cpu_cnt만의) 값을 @result에
 * 스냅숏으로 복사한다
 *
 * @rwstat: 읽을 대상 struct blkg_rwstat.
 * @result: 결과를 채워 넣을 struct blkg_rwstat_sample 출력 버퍼.
 *   호출 전 값은 무시되고 함수 안에서 5개 인덱스가 모두 덮어써진다.
 * @return: 없음(void). 결과는 @result out-parameter로 반환된다.
 *
 * 이 함수는 blkg_rwstat_read_counter()와 달리 aux_cnt는 더하지 않고
 * cpu_cnt[]만(즉 "local" 관점의 현재 값만) 스냅숏으로 뜬다는 점에
 * 유의해야 한다 — 이 헤더 상단 원본 주석의 "aux_cnt is excluded for
 * local stats"라는 설명이 여기서 실제로 구현된다. BLKG_RWSTAT_NR(5)
 * 개 인덱스를 순회하며 각각 percpu_counter_sum_positive()를 호출해
 * 모든 CPU의 로컬 카운트를 합산한다. 실행 컨텍스트는 이 함수를
 * 호출하는 상위 함수의 컨텍스트를 그대로 물려받으며(대개 cgroupfs
 * read() 시스템 콜 컨텍스트), percpu_counter_sum_positive() 자체가
 * 내부적으로 필요한 동기화(전역 스핀락 또는 RCU에 준하는 보호, 커널
 * 버전에 따라 다름)를 처리하므로 이 함수 밖에서 별도 락을 잡을
 * 필요는 없다. 다만 5개 인덱스를 순차적으로 읽는 동안 각 인덱스의
 * "시점"이 완전히 동일하지는 않으므로, 결과는 엄밀한 원자적 스냅숏
 * 이라기보다 근사치에 가깝다.
 * 이 함수를 호출하는 상위 함수는 blkg_rwstat_total()과 cgroup stat
 * show 루틴(blkg_prfill_rwstat() 등)이며, 하위로
 * percpu_counter_sum_positive()를 호출한다. 에러 경로는 없다.
 *
 * 호출 체인:
 *   blkg_rwstat_total()/blkg_prfill_rwstat() -> [blkg_rwstat_read]
 *   -> percpu_counter_sum_positive()
 *
 * NVMe 연결: namespace별로 수집된 Read/Write/Discard/Sync/Async
 * 카운터를 사용자 공간으로 낼 때, 혹은 R+W 합계를 구할 때 가장 먼저
 * 호출되어 원시 스냅숏을 만든다.
 */
/**
 * blkg_rwstat_read - read the current values of a blkg_rwstat
 * @rwstat: blkg_rwstat to read
 * @result: where to put the current values
 *
 * Read the current snapshot of @rwstat and return it in the @result counts.
 */
static inline void blkg_rwstat_read(struct blkg_rwstat *rwstat,
		struct blkg_rwstat_sample *result)
{
	int i;												/* NVMe 명령 클래스 인덱스 반복자 */

	/* BLKG_RWSTAT_READ/WRITE/SYNC/ASYNC/DISCARD 전체 복사 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)					/* READ/WRITE/SYNC/ASYNC/DISCARD 순회: namespace별 5개 통계 클래스 */
		result->cnt[i] =								/* 스냅숏 배열에 복사: io.stat 노출 전 일관성 확보 */
			percpu_counter_sum_positive(&rwstat->cpu_cnt[i]);	/* 모든 SQ submission CPU + CQ 완료 CPU 카운터 합산 */
}

/*
 * [한국어]
 * blkg_rwstat_total - I/O 방향과 무관하게 READ + WRITE 카운터의
 * 합계만 반환한다
 *
 * @rwstat: 읽을 대상 struct blkg_rwstat.
 * @return: BLKG_RWSTAT_READ와 BLKG_RWSTAT_WRITE 두 인덱스의 스냅숏
 *   값을 더한 u64. DISCARD/SYNC/ASYNC는 이 합계에 포함되지 않는다.
 *
 * blk-throttle이나 legacy cfq 같은 "Read/Write 대역폭" 중심의 스로틀
 * 정책은 discard까지 포함한 전체 합보다 순수 데이터 I/O(R+W)만의
 * 합계가 필요한 경우가 많다. 이 함수는 그런 용도로, 내부적으로
 * blkg_rwstat_read()를 호출해 5개 인덱스 전체를 스택 변수 tmp에 먼저
 * 스냅숏 뜬 뒤 READ/WRITE 두 인덱스만 더해서 반환한다. 커널 원본
 * kernel-doc이 "This function can be called without synchronization
 * and takes care of u64 atomicity"라고 명시하듯, u64 값 자체의
 * 원자성은 percpu_counter_sum_positive() 내부에서 보장되므로 호출자가
 * 별도 락을 잡을 필요가 없다. 실행 컨텍스트는 호출자의 컨텍스트를
 * 그대로 물려받는다(주로 스로틀 정책의 bio 제출 경로 또는 limit 재계산
 * 루틴). 이 함수를 호출하는 상위 함수는 blk-throttle/cfq 등 legacy
 * 정책의 limit 계산/스로틀 판단 루틴이며, 하위로 blkg_rwstat_read()를
 * 호출한다. 에러 경로는 없다.
 *
 * 호출 체인:
 *   blk-throttle/cfq limit 계산 루틴 -> [blkg_rwstat_total] ->
 *   blkg_rwstat_read()
 *
 * NVMe 연결: NVMe namespace에 대한 Read/Write 처리량 기반
 * 스로틀링 판단(예: 초당 바이트 제한 검사) 시 참조값으로 쓰인다.
 */
/**
 * blkg_rwstat_total - read the total count of a blkg_rwstat
 * @rwstat: blkg_rwstat to read
 *
 * Return the total count of @rwstat regardless of the IO direction.  This
 * function can be called without synchronization and takes care of u64
 * atomicity.
 */
static inline uint64_t blkg_rwstat_total(struct blkg_rwstat *rwstat)
{
	struct blkg_rwstat_sample tmp = { };					/* namespace 단위 스냅숏 버퍼: stack 기반 */

	/* 스냅숏을 먼저 떠서 Read/Write 합계만 반환 (Discard 제외) */
	blkg_rwstat_read(rwstat, &tmp);							/* READ/WRITE/SYNC/ASYNC/DISCARD 5종 스냅숏 생성 */
	return tmp.cnt[BLKG_RWSTAT_READ] + tmp.cnt[BLKG_RWSTAT_WRITE];	/* [한국어] READ와 WRITE만 더한다. SYNC/ASYNC는 같은 I/O를 다른 축으로
	 * 이중 집계한 값이라 함께 더하면 두 배가 되고, DISCARD는 데이터를
	 * 전송하지 않아 대역폭 계산에서 제외해야 하기 때문이다. */
}

/*
 * [한국어]
 * blkg_rwstat_reset - blkg_rwstat의 모든 per-CPU 카운터와 aux
 * 카운터를 0으로 되돌린다
 *
 * @rwstat: 초기화할 대상 struct blkg_rwstat.
 * @return: 없음(void).
 *
 * BLKG_RWSTAT_NR(5)개 인덱스 전체를 순회하며 percpu_counter_set()으로
 * cpu_cnt[i]를, atomic64_set()으로 aux_cnt[i]를 각각 0으로 되돌린다.
 * "reset"이라는 이름 그대로 이 blkg의 통계 이력을 완전히 지우는
 * 연산이므로, cgroup 재구성이나 통계 카운터를 사용자가 명시적으로
 * 리셋 요청(예: cgroupfs의 일부 write-only reset 인터페이스)했을 때
 * 호출된다. 실행 컨텍스트는 그러한 재설정을 트리거하는
 * 유저 요청의 시스템 콜 컨텍스트이며, percpu_counter_set()/
 * atomic64_set()은 각각 해당 API가 제공하는 원자성만 보장할 뿐, 이
 * 함수 자체가 "5개 인덱스 전체를 하나의 원자적 트랜잭션으로 리셋"
 * 하는 것은 아니라는 점에 유의해야 한다 — 리셋 도중 다른 CPU가
 * blkg_rwstat_add()를 호출하면 일부 인덱스는 새 값이 반영된 채로
 * 남을 수 있다 — 호출자가 이런 경쟁을 배제해야 한다.
 * 이 함수를 호출하는 상위 함수는 cgroup 통계 리셋 경로이며,
 * 하위로 percpu_counter_set()과 atomic64_set()을 호출한다. 에러
 * 경로는 없다.
 *
 * 호출 체인:
 *   cgroup 통계 리셋 인터페이스 -> [blkg_rwstat_reset] ->
 *   percpu_counter_set() / atomic64_set()
 *
 * NVMe 연결: namespace/controller를 다른 cgroup으로 옮기거나
 * 통계를 명시적으로 리셋할 때 기존 누적값을 제거해 새 측정 구간을
 * 시작할 수 있게 한다.
 */
/**
 * blkg_rwstat_reset - reset a blkg_rwstat
 * @rwstat: blkg_rwstat to reset
 */
static inline void blkg_rwstat_reset(struct blkg_rwstat *rwstat)
{
	int i;												/* NVMe 통계 클래스 반복자 */

	for (i = 0; i < BLKG_RWSTAT_NR; i++) {					/* READ/WRITE/SYNC/ASYNC/DISCARD 5개 클래스 전체 초기화 */
		percpu_counter_set(&rwstat->cpu_cnt[i], 0);			/* per-CPU SQ/CQ 누적값 0으로 설정: cgroup 이동 시 이전 namespace 값 제거 */
		atomic64_set(&rwstat->aux_cnt[i], 0);				/* aux 제거 통계 0으로 설정: 상위 cgroup 누적값 재조정 */
	}
}

/*
 * [한국어]
 * blkg_rwstat_add_aux - @from의 (현재 값 + 기존 aux) 전체를 @to의
 * aux 카운터에 원자적으로 합산한다
 *
 * @to: 통계를 승계받을 목적지 struct blkg_rwstat. 보통 삭제되는
 *   blkg의 부모(상위) cgroup에 대응하는 blkg의 policy_data 안 인스턴스.
 * @from: 통계를 넘겨줄 원본 struct blkg_rwstat. 보통 방금
 *   제거되거나 이동되는 blkg의 인스턴스.
 * @return: 없음(void).
 *
 * cgroup 트리에서 하위 노드(blkg)가 사라지면 그 blkg가 그동안
 * 쌓아온 통계도 함께 사라지기 마련이지만, "이 상위 cgroup 아래에서
 * 지금까지 얼마나 I/O가 있었는가"라는 recursive 통계는 사라진
 * 자식의 몫까지 포함해야 정확하다. 이 함수는 그 승계를 구현한다 —
 * 먼저 @from의 cpu_cnt[]를 percpu_counter_sum_positive()로 인덱스별
 * 합산해 임시 배열 sum[]에 담고, 그 값에 @from의 기존 aux_cnt[]
 * (즉 @from이 그 이전에 또 다른 죽은 자손으로부터 승계받았던 값)까지
 * 더해서, 최종적으로 @to의 aux_cnt[]에 atomic64_add()로 얹는다. 이렇게
 * "합산값 + 기존 aux"를 함께 넘기므로 여러 세대에 걸쳐 자식이
 * 제거되어도(자식의 자식이 다시 제거되는 경우 등) 통계가 누적되어
 * 손실 없이 최상위까지 전달될 수 있다. 실행 컨텍스트는 blkg가
 * 제거되거나 다른 cgroup으로 이동하는 절차(blkg 소멸 경로) 내부이며,
 * @to에 대한 atomic64_add()는 원자적이지만, @from의 cpu_cnt[] 합산과
 * @to에 대한 더하기 사이에는 원자성이 없으므로 호출자가 @from이
 * 더 이상 갱신되지 않는 시점(quiescent 상태)에 이 함수를 호출해야
 * 한다. 이 함수를 호출하는 상위 함수는 blkg 제거/이동 절차이며
 * (이 트리에서는 BFQ의 정책 오프라인 경로), 하위로
 * percpu_counter_sum_positive(), atomic64_read(), atomic64_add()를
 * 호출한다. 에러 경로는 없다.
 *
 * 호출 체인:
 *   blkg_destroy()/blkg 이동 절차 -> [blkg_rwstat_add_aux] ->
 *   percpu_counter_sum_positive() / atomic64_add()
 *
 * NVMe 연결: 제거된 nvme_queue 또는 namespace에 대응하는
 * blkg가 사라질 때, 그 기록을 상위(또는 이동 대상) cgroup의
 * aux_cnt로 보존해 recursive 통계에서 누적치가 사라지지 않게 한다.
 */
/**
 * blkg_rwstat_add_aux - add a blkg_rwstat into another's aux count
 * @to: the destination blkg_rwstat
 * @from: the source
 *
 * Add @from's count including the aux one to @to's aux count.
 */
static inline void blkg_rwstat_add_aux(struct blkg_rwstat *to,
				       struct blkg_rwstat *from)
{
	u64 sum[BLKG_RWSTAT_NR];								/* from per-CPU 합산 임시 버퍼: 5개 NVMe 클래스 */
	int i;												/* 통계 클래스 반복자 */

	/* 먼저 from의 per-CPU 카운터를 각 항목별로 합산 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)					/* from 하위 cgroup의 READ/WRITE/SYNC/ASYNC/DISCARD 순회 */
		sum[i] = percpu_counter_sum_positive(&from->cpu_cnt[i]);	/* 삭제되는 nvme_queue/namespace의 per-CPU SQ/CQ 통계 합산 */

	/* per-CPU 합계와 from의 aux 값을 to의 aux에 원자 추가 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)					/* 대상 cgroup의 aux_cnt에 병합: 하위 트리 nvme 통계 보존 */
		atomic64_add(sum[i] + atomic64_read(&from->aux_cnt[i]),
			     &to->aux_cnt[i]);						/* atomic64_add: CQ 완료 순서와 독립적으로 상위 cgroup 누적값 갱신 */
}

/* NVMe 관점 핵심 요약
 *
 * - blkg_rwstat은 NVMe Read/Write/Discard/Sync/Async 요청량을
 *   cgroup 단위로 추적하며, per-CPU 카운터는 doorbell 경합을
 *   줄이는 캐시 구조다.
 * - aux_cnt는 제거된 nvme_queue/namespace의 통계를 상위 cgroup에
 *   보존해 누적값이 유실되지 않게 한다.
 * - blkg_rwstat_add는 bio/req opf 플래그를 NVMe 명령 유형으로
 *   매핑: discard -> Deallocate, write -> Write, 나머지 -> Read.
 * - blkg_rwstat_recursive_sum은 다중 namespace/controller를 거느린
 *   cgroup의 전체 I/O를 집계해 /sys/fs/cgroup/.../io.* 노출에
 *   사용된다.
 * - 이 파일은 block/blk-cgroup.h의 blkcg_policy/blkcg_gq 위에서
 *   동작하는 통계 계층으로, NVMe 드라이버와 직접 연결되지는
 *   않으나 SQ/CQ 기반 I/O 제어 흐름의 계량 지표를 제공한다.
 */
#endif	/* _BLK_CGROUP_RWSTAT_H */
