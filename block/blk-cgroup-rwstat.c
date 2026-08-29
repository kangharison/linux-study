/* SPDX-License-Identifier: GPL-2.0
 *
 * Legacy blkg rwstat helpers enabled by CONFIG_BLK_CGROUP_RWSTAT.
 * Do not use in new code.
 */

/*
 * [한국어 설명] blkcg(블록 I/O cgroup)의 read/write(및 sync/async,
 * discard) 방향별 누적 통계 헬퍼 — block/blk-cgroup-rwstat.h가 선언만
 * 해 둔 non-inline 함수 5개의 실제 구현체 (blk-cgroup-rwstat.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 block/blk-cgroup-rwstat.h가 원형(prototype)만 선언해 둔
 * blkg_rwstat_init(), blkg_rwstat_exit(), __blkg_prfill_rwstat(),
 * blkg_prfill_rwstat(), blkg_rwstat_recursive_sum() 5개 함수의 실제
 * 본체(definition)를 제공하는 번역 단위(translation unit)다. 헤더에는
 * blkg_rwstat_add()/blkg_rwstat_read()/blkg_rwstat_read_counter()/
 * blkg_rwstat_reset()/blkg_rwstat_total()/blkg_rwstat_add_aux()처럼
 * 호출 빈도가 매우 높은 "쓰기·경량 조회" 헬퍼가 static inline으로
 * 정의되어 있는 반면(제출 경로 오버헤드 최소화 목적), 이 파일에는
 * 상대적으로 드물게 호출되는 "자원 생성/해제"와 "cgroupfs 출력/재귀
 * 집계" 계열만 모아 별도 컴파일 단위로 둔다. 다섯 함수는 역할상 두
 * 그룹으로 나뉜다 — (1) percpu_counter 자원의 생성자/소멸자인
 * blkg_rwstat_init()/blkg_rwstat_exit(), (2) cgroupfs로 값을 내보내는
 * 조회·출력 계층인 __blkg_prfill_rwstat()/blkg_prfill_rwstat()/
 * blkg_rwstat_recursive_sum(). 파일 최상단 원본 주석 "Do not use in
 * new code"가 명시하듯, 이 코드는 아직 최신 blkg_iostat_set 계열로
 * 이관되지 않은 legacy 정책 — 구체적으로 BFQ의 cgroup 확장
 * (Kconfig 심볼 BFQ_GROUP_IOSCHED, block/bfq-cgroup.c)과 bio 스로틀링
 * (Kconfig 심볼 BLK_DEV_THROTTLING, block/blk-throttle.c) — 전용의
 * 회계(accounting) 인프라다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트는 뚜렷이 두 가지로 나뉜다.
 * (1) 초기화/해제 경로: blkg_rwstat_init()은 block/blk-throttle.c의
 * throtl_pd_alloc()(blkcg_activate_policy() -> pd_alloc_fn 콜백에서
 * 호출)과 block/bfq-cgroup.c의 bfqg_stats_init()(bfq_pd_alloc()에서
 * 호출)이 각각 새 cgroup이 특정 request_queue(예: NVMe 네임스페이스의
 * 큐)에 대해 처음 활성화될 때 호출한다. 이 시점은 아직 다른 CPU가
 * 이 rwstat을 갱신할 수 없는 "생성 중" 컨텍스트라 별도 락이 필요
 * 없다. 반대편의 blkg_rwstat_exit()은 대응하는 throtl_pd_free()/
 * bfqg_stats_exit()이 cgroup 비활성화·blkg 소멸 시점에 호출한다.
 * (2) 조회/출력 경로: 사용자가 /sys/fs/cgroup/<cgroup>/blkio.throttle.*
 * 같은 cgroupfs 파일을 read(2)하면, kernfs seq_show -> cftype이 등록한
 * prfill 콜백(block/bfq-cgroup.c의 bfqg_print_rwstat() 등) ->
 * blkcg_print_blkgs()가 활성화된 각 blkg를 순회하며 blkg_prfill_rwstat()
 * 을 호출하거나(block/bfq-cgroup.c:2703, block/blk-throttle.c:2112
 * 부근에서 실제로 이 패턴이 쓰인다), 재귀(하위 트리 포함) 통계가
 * 필요한 stat_show 콜백은 blkg_rwstat_recursive_sum()과
 * __blkg_prfill_rwstat()을 직접 순서대로 호출한다(block/bfq-cgroup.c:
 * 2733, block/blk-throttle.c:2132-2134). 이 경로는 cgroupfs를 읽는
 * 유저 프로세스의 시스템 콜 컨텍스트(커널 진입 후)에서 실행된다.
 * NVMe 스택 관점에서는 nvme_queue_rq()가 SQ(Submission Queue)에
 * 커맨드를 쓰기 이전, 공통 blk-mq 계층에서 헤더의 인라인 함수
 * blkg_rwstat_add()가 이미 방향별 값을 누적해 둔 뒤이므로, 이 파일의
 * 함수들은 NVMe 드라이버를 직접 호출하지도, NVMe로부터 직접 호출되지도
 * 않는 순수 소프트웨어 회계/출력 계층이다(실제 호출자는
 * 위에 명시한 bfq-cgroup.c/blk-throttle.c 코드로 확인됨).
 *
 * === 타 모듈과의 연결 ===
 * 의존(依存) 대상: block/blk-cgroup-rwstat.h(struct blkg_rwstat/
 * blkg_rwstat_sample 타입, enum blkg_rwstat_type, 그리고
 * blkg_rwstat_read()/blkg_rwstat_read_counter() 등 인라인 헬퍼),
 * block/blk-cgroup.h(struct blkg_policy_data, struct blkcg_gq,
 * struct blkcg_policy, blkg_to_pd(), blkg_dev_name(),
 * blkg_for_each_descendant_pre() 매크로 — 내부적으로
 * css_for_each_descendant_pre()로 cgroup_subsys_state 트리를 RCU
 * 보호 하에 순회한다), lib/percpu_counter.c의
 * percpu_counter_init_many()/percpu_counter_destroy_many()/
 * percpu_counter_sum_positive(), fs/seq_file.c의 seq_printf().
 * 피의존(被依存) 대상: block/bfq-cgroup.c(struct bfqg_stats 안에
 * blkg_rwstat 필드 6개 — bytes/ios/merged/service_time/wait_time/
 * queued — 를 내장하고 이 파일의 5개 함수를 모두 사용),
 * block/blk-throttle.c(struct throtl_grp 안에 stat_bytes/stat_ios
 * 두 blkg_rwstat 필드를 내장). 데이터 흐름: "bio/request opf 플래그
 * -> (헤더의 인라인) blkg_rwstat_add() -> percpu_counter 누적"까지는
 * 이 파일 밖에서 일어나고, 이 파일은 그 뒤를 이어 "생성 시점의
 * percpu_counter 자원 할당(blkg_rwstat_init) -> ... -> 소멸 시점의
 * 자원 회수(blkg_rwstat_exit)"라는 생애주기 관리와, "누적된 값을
 * 스냅숏(blkg_rwstat_read, 헤더의 인라인 함수) -> 트리 재귀 합산
 * (blkg_rwstat_recursive_sum, 이 파일) -> 텍스트 포맷(
 * __blkg_prfill_rwstat, 이 파일) -> seq_file -> 유저 공간"이라는
 * 출력 파이프라인을 담당한다.
 *
 * [중요 발견 — 헤더-구현 타입 불일치] block/blk-cgroup-rwstat.h의
 * 현재 커밋된 선언(__blkg_prfill_rwstat, blkg_prfill_rwstat 두 함수)
 * 은 두 번째 매개변수 타입을 `struct blkcg_policy_data *pd`로 적고
 * 있으나, 이 .c 파일의 실제 정의(및 주석 작업 이전 순정 커널 원본
 * 스냅숏 커밋 1f0e418bb6의 .h/.c 모두)는 일관되게
 * `struct blkg_policy_data *pd`를 쓴다. blkg_policy_data(blkg 단위
 * 정책 사설 데이터)와 blkcg_policy_data(blkcg 단위 정책 사설
 * 데이터)는 block/blk-cgroup.h에 각각 별도로 정의된 서로 다른
 * 구조체이므로, 헤더 선언과 이 .c 파일 정의의 매개변수 타입이
 * 일치하지 않는 상태다. git blame으로 추적한 결과 이 불일치는 순정
 * 코드에는 없었고, "Add Korean annotations: block layer" 커밋
 * (ea5479b64c)이 헤더에 한국어 주석을 추가하는 과정에서 실수로 함수
 * 선언부의 실제 타입 토큰까지 바꿔버린 것으로 보인다(주석만 추가하고
 * 코드는 건드리지 않는다는 원칙을 위반한 사례). 이 .c 파일은 그
 * 원칙에 따라 절대 수정하지 않았으며, 아래 함수들의 실제 정의부는
 * 여전히 정상적으로 `blkg_policy_data`를 사용한다 — 헤더 쪽 선언을
 * `blkg_policy_data`로 되돌리는 별도 수정이 필요해 보인다(이 작업의
 * 범위는 .c 파일 주석에 한정되므로 헤더는 건드리지 않았다).
 *
 * === 주요 함수/구조체 요약 ===
 * - blkg_rwstat_init(): struct blkg_rwstat의 cpu_cnt[BLKG_RWSTAT_NR]
 *   percpu_counter를 percpu_counter_init_many()로 한 번에 할당하고
 *   aux_cnt[BLKG_RWSTAT_NR]를 0으로 초기화하는 생성자. 실패 시
 *   -ENOMEM류 errno를 반환.
 * - blkg_rwstat_exit(): blkg_rwstat_init()이 할당한 percpu_counter
 *   BLKG_RWSTAT_NR개를 percpu_counter_destroy_many()로 해제하는
 *   소멸자.
 * - __blkg_prfill_rwstat(): 이미 채워진 blkg_rwstat_sample 스냅숏을
 *   "<dev> Read/Write/Sync/Async/Discard <count>" + "<dev> Total
 *   <count>" 형태의 텍스트로 seq_file에 출력하는 저수준 포맷터.
 *   Total은 READ+WRITE+DISCARD 합계이며 SYNC/ASYNC는 제외된다.
 * - blkg_prfill_rwstat(): policy_data 시작 주소 + 오프셋에서
 *   blkg_rwstat을 찾아 blkg_rwstat_read()로 스냅숏을 뜬 뒤
 *   __blkg_prfill_rwstat()에 위임하는, cftype의 prfill 콜백 시그니처에
 *   바로 맞는 상위 래퍼.
 * - blkg_rwstat_recursive_sum(): 지정된 blkg를 루트로 online 상태인
 *   하위 cgroup 트리 전체를 blkg_for_each_descendant_pre()로 순회하며
 *   각 노드의 cpu_cnt 합산값 + aux_cnt를 재귀적으로 더해 @sum에
 *   반환한다. 호출자가 queue_lock을 들고 있어야 하며
 *   lockdep_assert_held()로 이를 검증한다.
 * - (구조체/enum 자체의 정의와 필드별 상세 주석은 이 파일이 아니라
 *   block/blk-cgroup-rwstat.h에 있다 — struct blkg_rwstat,
 *   struct blkg_rwstat_sample, enum blkg_rwstat_type 참고.)
 */
#include "blk-cgroup-rwstat.h"		/* [한국어] struct blkg_rwstat/blkg_rwstat_sample 타입 정의와 blkg_rwstat_read()/blkg_rwstat_read_counter() 등 인라인 헬퍼를 가져옴 - 이 파일의 모든 함수가 이 타입들을 매개변수/반환값으로 사용하므로 반드시 먼저 포함되어야 함 */

/*
 * [한국어] 참고: 이 .c 파일의 함수들이 다루는 핵심 자료구조 요약
 * (정의와 필드별 완전한 주석은 모두 block/blk-cgroup-rwstat.h에 있음 —
 * 아래는 함수 본문을 읽을 때 바로 참고할 수 있도록 압축한 재진술이며,
 * "설정자/읽는 자/값 범위/동기화" 같은 완전한 설명은 헤더 쪽이 원본이다)
 *
 * struct blkg_rwstat { cpu_cnt[BLKG_RWSTAT_NR]; aux_cnt[BLKG_RWSTAT_NR]; }
 *   - cpu_cnt: 현재 살아있는 이 blkg의 percpu_counter 5개
 *     (READ/WRITE/SYNC/ASYNC/DISCARD 순서).
 *   - aux_cnt: 이미 제거된 하위 blkg로부터 승계받은 atomic64_t 카운터 5개.
 * struct blkg_rwstat_sample { cnt[BLKG_RWSTAT_NR]; }
 *   - 어느 한 시점에 고정된 u64 스냅숏 배열. cgroupfs 출력 및 재귀 합산의
 *     중간 결과를 담는 용도.
 * enum blkg_rwstat_type: BLKG_RWSTAT_READ/WRITE/SYNC/ASYNC/DISCARD
 *   (+ BLKG_RWSTAT_NR, BLKG_RWSTAT_TOTAL)
 *   - 위 두 구조체의 배열 cpu_cnt[]/aux_cnt[]/cnt[]를 인덱싱하는 상수.
 */

/*
 * [한국어]
 * blkg_rwstat_init - struct blkg_rwstat 내부의 percpu_counter 배열과
 * aux_cnt 배열을 새로 할당/초기화한다
 *
 * @rwstat: 초기화할 대상 struct blkg_rwstat 포인터. 보통 blkg의
 *   policy_data(struct blkg_policy_data, blk-cgroup.h의 blkg_to_pd()로
 *   얻는 포인터) 안에 값으로 내장된 인스턴스의 주소가 전달된다 —
 *   예를 들어 struct throtl_grp.stat_bytes/stat_ios, struct
 *   bfqg_stats.bytes/ios 등.
 * @gfp: percpu_counter_init_many()에 그대로 전달되는 GFP 할당 플래그.
 *   호출자인 throtl_pd_alloc()/bfqg_stats_init()이 blkcg_activate_policy()
 *   / bfq_pd_alloc()로부터 전달받은 gfp를 그대로 넘기며, 이 경로는
 *   보통 잠들 수 있는(sleep 가능) 프로세스 컨텍스트이므로 실제로는
 *   GFP_KERNEL 계열이 흔히 쓰인다.
 * @return: 0이면 성공. percpu_counter_init_many()가 실패(-ENOMEM 등)를
 *   반환하면 그 값을 그대로 돌려주며, 이 경우 aux_cnt는 초기화되지
 *   않은 채로 함수가 종료된다 — 호출자(throtl_pd_alloc/bfqg_stats_init)
 *   는 실패 시 rwstat을 더 이상 사용하지 말고 자신을 감싼 policy_data
 *   전체를 해제해야 한다(실제로 throtl_pd_alloc()은 err_free_tg 레이블로,
 *   bfqg_stats_init()은 error 레이블로 점프해 롤백한다).
 *
 * blkg_rwstat 하나는 percpu_counter BLKG_RWSTAT_NR(5)개(READ/WRITE/
 * SYNC/ASYNC/DISCARD)로 구성되는데, percpu_counter는 자체적으로 CPU별
 * 로컬 메모리를 동적 할당해야 하므로(percpu 영역 확보) 사용 전에 반드시
 * 이 초기화 함수를 거쳐야 한다. 동작 순서는 (1)
 * percpu_counter_init_many()로 cpu_cnt[0..4]를 한 번의 호출로 할당하고
 * (개별 percpu_counter_init()을 5번 부르는 것보다 할당 오버헤드가
 * 적다), (2) 그 호출이 성공하면 aux_cnt[0..4]를 atomic64_set()으로
 * 0으로 채운다. percpu_counter_init_many()가 실패하면 aux_cnt 초기화
 * 단계로 가지 않고 즉시 에러를 반환해, "cpu_cnt는 실패했는데 aux_cnt만
 * 초기화된" 애매한 절반 상태를 만들지 않는다. 실행 컨텍스트는 blkg
 * 또는 정책의 policy_data가 새로 생성되는 시점 — 어떤 cgroup에 처음
 * 이 legacy 정책이 적용되거나, 새 request_queue에 blkg가 처음 생성될
 * 때이며, 이 시점에는 아직 다른 CPU가 이 rwstat을 갱신할 수 없으므로
 * 별도의 락 없이 안전하다. 호출하는 상위 함수는
 * block/blk-throttle.c의 throtl_pd_alloc()과 block/bfq-cgroup.c의
 * bfqg_stats_init()이며, 하위로는 percpu_counter_init_many()와
 * atomic64_set()을 호출한다. 에러 발생 시(-ENOMEM) 호출자가 이미
 * 할당된 policy_data 메모리를 되돌리고 blkg/policy 활성화 절차 전체를
 * 실패로 처리한다.
 *
 * 호출 체인:
 *   blkcg_activate_policy() -> throtl_pd_alloc()/bfq_pd_alloc() ->
 *   [blkg_rwstat_init] -> percpu_counter_init_many() / atomic64_set()
 */
int blkg_rwstat_init(struct blkg_rwstat *rwstat, gfp_t gfp)
/* [한국어] 함수 시그니처: @rwstat 자리에 blkg_rwstat 인스턴스, @gfp 자리에 percpu_counter 할당 플래그를 받는다 - 반환형 int는 percpu_counter_init_many()의 성공/실패(errno)를 그대로 실어 나름 */
{
	int i, ret;
	/* [한국어] i: 아래 aux_cnt 초기화 for 루프에서 쓰는 BLKG_RWSTAT_NR(5, READ/WRITE/SYNC/ASYNC/DISCARD) 범위의 인덱스. ret: percpu_counter_init_many()의 반환값(0=성공, 음수=실패한 errno)을 임시 저장 */

	/* per-CPU 카운터 BLKG_RWSTAT_NR개(READ/WRITE/SYNC/ASYNC/DISCARD) 할당 */
	ret = percpu_counter_init_many(rwstat->cpu_cnt, 0, gfp, BLKG_RWSTAT_NR);
	/* [한국어] rwstat->cpu_cnt[0..BLKG_RWSTAT_NR-1] 배열 전체를 초깃값 0, @gfp 플래그로 한 번에 할당/초기화 - percpu_counter_init()을 5번 개별 호출하는 대신 _many 변형을 사용해 percpu 메모리 할당 왕복 횟수를 줄임. 이후 blkg_rwstat_add()(헤더 인라인 함수)가 이 5개 카운터에 CPU-로컬 배치(batch) 방식으로 값을 누적함 */
	if (ret)
	/* [한국어] percpu_counter_init_many()가 음수 errno(대표적으로 -ENOMEM)를 반환했다면 percpu 메모리 할당 실패로 판단 */
		return ret;
		/* [한국어] aux_cnt 초기화 단계를 건너뛰고 즉시 실패를 호출자(throtl_pd_alloc/bfqg_stats_init)에게 전파 - 이 시점에서는 cpu_cnt조차 온전히 만들어지지 않았으므로 rwstat 자체를 더 사용하면 안 되고, 호출자가 자신이 할당한 policy_data 메모리를 되돌려야 함 */

	/* 사라진 하위 cgroup의 잔여 통계를 담을 aux_cnt를 0으로 초기화 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)
	/* [한국어] BLKG_RWSTAT_READ(0)부터 BLKG_RWSTAT_DISCARD(4)까지 5개 방향/동기성 인덱스를 순차 순회 */
		atomic64_set(&rwstat->aux_cnt[i], 0);
		/* [한국어] 각 방향의 aux_cnt를 atomic64_set()으로 원자적으로 0 설정 - 이 rwstat은 방금 새로 만들어졌으므로 아직 어떤 자식 blkg도 이 인스턴스로 통계를 이관(blkg_rwstat_add_aux())한 적이 없어 초기값은 항상 0이어야 함 */
	return 0;
	/* [한국어] cpu_cnt/aux_cnt 모두 정상 초기화 완료 - 호출자에게 성공을 알리고, 이제부터 blkg_rwstat_add()로 값을 누적할 준비가 된 상태임을 의미 */
}
EXPORT_SYMBOL_GPL(blkg_rwstat_init);
/* [한국어] 이 심볼을 GPL 라이선스 모듈에 한해 외부(다른 .o/모듈)로 노출 - block/bfq-cgroup.c, block/blk-throttle.c 등 이 함수를 호출하는 소비자 코드가 별도 커널 모듈로 빌드되더라도 링크해 쓸 수 있게 함(BLK_CGROUP_RWSTAT 자체는 bool이라 이 파일은 항상 built-in이지만, 소비자 쪽은 다른 설정일 수 있음을 대비) */

/*
 * [한국어]
 * blkg_rwstat_exit - blkg_rwstat_init()이 할당한 per-CPU 카운터
 * 자원을 해제한다
 *
 * @rwstat: 해제할 대상 struct blkg_rwstat 포인터. blkg_rwstat_init()과
 *   반드시 짝을 이루어야 하는 생성자/소멸자 대칭 관계다.
 * @return: 없음(void). percpu_counter_destroy_many()는 실패할 수 없는
 *   해제 연산이다(메모리 해제는 실패 경로가
 *   없음).
 *
 * blkg_rwstat_init()에서 percpu_counter_init_many()로 할당했던 CPU별
 * 카운터 메모리를 percpu_counter_destroy_many()로 되돌리는 소멸자
 * 역할의 함수다. 실행 컨텍스트는 blkg 또는 policy_data가 소멸되는
 * 시점 — block/blk-throttle.c의 throtl_pd_free()(cgroup이 삭제되거나
 * request_queue가 소멸될 때)와 block/bfq-cgroup.c의 bfqg_stats_exit()
 * (bfq_pd_free() 또는 bfqg_stats_init()의 에러 롤백 경로)가 이 함수를
 * 호출한다. 이 시점에는 더 이상 다른 경로에서 이 rwstat을 갱신하지
 * 않는다고 가정하고 동작한다 — 즉 호출자가 quiescence(정지 상태)를
 * 보장해야 한다. 실제로 throtl_pd_free()는 이 함수를 부르기 전에
 * timer_delete_sync()로 관련 타이머를 동기적으로 멈춰 더 이상 새
 * 통계 갱신이 발생하지 않게 만든다. 하위로는
 * percpu_counter_destroy_many()만 호출하며 별도의 에러 반환 경로는
 * 없다.
 *
 * 호출 체인:
 *   blkcg_deactivate_policy()/blkg 소멸 절차 -> throtl_pd_free()/
 *   bfqg_stats_exit() -> [blkg_rwstat_exit] ->
 *   percpu_counter_destroy_many()
 */
void blkg_rwstat_exit(struct blkg_rwstat *rwstat)
/* [한국어] @rwstat: 해제할 blkg_rwstat 인스턴스 - 이 호출 이후 해당 인스턴스에 대한 blkg_rwstat_add()/read() 등은 이미 해제된 percpu 메모리를 건드리므로 절대 호출되어서는 안 됨(use-after-free 방지는 호출자의 책임) */
{
	/* [한국어] per-CPU 카운터 해제 — 이후 이 rwstat에 blkg_rwstat_add()를 호출하면 안 된다 */
	percpu_counter_destroy_many(rwstat->cpu_cnt, BLKG_RWSTAT_NR);
	/* [한국어] rwstat->cpu_cnt[0..BLKG_RWSTAT_NR-1] 배열의 percpu_counter BLKG_RWSTAT_NR(5)개를 한 번에 해제 - blkg_rwstat_init()의 percpu_counter_init_many()와 정확히 짝을 이루는 해제 호출. aux_cnt는 atomic64_t라 별도 percpu 메모리를 갖지 않으므로 여기서 해제할 대상이 없음(단순 정수 필드라 rwstat 자체를 담은 policy_data가 kfree될 때 함께 사라짐) */
}
EXPORT_SYMBOL_GPL(blkg_rwstat_exit);
/* [한국어] blkg_rwstat_init()과 동일한 이유로 GPL 모듈에 이 심볼을 노출 - throtl_pd_free()/bfqg_stats_exit() 등 소비자 측에서 링크해 호출할 수 있도록 함 */

/**
 * __blkg_prfill_rwstat - prfill helper for a blkg_rwstat
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @rwstat: rwstat to print
 *
 * Print @rwstat to @sf for the device assocaited with @pd.
 */
/*
 * [한국어]
 * __blkg_prfill_rwstat - 이미 채워진 blkg_rwstat_sample 스냅숏을
 * "<장치명> <방향> <값>" 형태의 텍스트로 seq_file에 출력한다
 *
 * @sf: 출력 대상 seq_file. cgroupfs가 /sys/fs/cgroup/.../io.* 또는
 *   blkio.throttle.* 류 파일을 read(2)할 때 커널이 마련해 주는 시퀀스
 *   파일 컨텍스트.
 * @pd: 이 통계가 속한 blkg의 정책 사설 데이터(struct blkg_policy_data).
 *   pd->blkg를 통해 blkg_dev_name()에 넘겨 "이 통계가 어느 블록
 *   디바이스(예: nvme0n1)에 대한 것인지" 이름을 얻는 데만 쓰인다.
 * @rwstat: 이미 blkg_rwstat_read() 또는 blkg_rwstat_recursive_sum()
 *   등으로 채워진 struct blkg_rwstat_sample 스냅숏(읽기 전용, 이
 *   함수는 값을 새로 계산하지 않는다).
 * @return: 이 blkg가 출력한 R+W+DISCARD 총합(u64). blkg_prfill_rwstat()
 *   같은 상위 prfill 콜백이 그대로 반환값으로 전달하며,
 *   blkcg_print_blkgs()는 이 반환값을 각 blkg별 결과로 취급한다(용도는
 *   호출자에 따라 다를 수 있다).
 *
 * 이 함수는 이미 방향별로 채워진 스냅숏(cnt[BLKG_RWSTAT_NR])을 받아
 * "Read/Write/Sync/Async/Discard" 각 줄 + "Total" 한 줄을 사람이 읽을
 * 수 있는 텍스트로 변환해 seq_file에 쓰는 포맷팅 전용 함수다. 값을
 * 새로 계산하지 않고 순수하게 "이미 있는 스냅숏을 텍스트로
 * 직렬화"하는 책임만 가진다는 점이 이름의 "__"(내부/저수준) 접두사와
 * 대칭되는 blkg_prfill_rwstat()(오프셋으로부터 스냅숏을 만들어 이
 * 함수를 호출하는 상위 래퍼)의 차이다. 동작 순서는 (1)
 * blkg_dev_name()으로 디바이스 이름을 얻고 이름이 없으면(디바이스가
 * 아직 등록 중이거나 이미 제거된 경우) 0을 반환해 조용히 스킵, (2)
 * BLKG_RWSTAT_NR(5)개 방향 전체를 순회하며 "<dev> <방향명>
 * <값>\n" 줄을 출력, (3) READ+WRITE+DISCARD 세 값만 더해 "<dev> Total
 * <합계>\n" 줄을 추가 출력. 실행 컨텍스트는 cgroupfs read() 시스템
 * 콜 컨텍스트이며, seq_file API 자체가 동시 read를 직렬화하므로 이
 * 함수 내부에서 별도 락이 필요 없다. 호출하는 상위 함수는
 * blkg_prfill_rwstat()(이 파일)과 block/bfq-cgroup.c의 재귀 합산 계열
 * prfill 콜백(라인 2735 부근)이며, block/blk-throttle.c의 대응
 * 콜백(라인 2134 부근)도 동일한 패턴으로 이 함수를 직접 호출한다.
 * 하위로는 blkg_dev_name()과 seq_printf()를 호출한다. 에러 경로는
 * 사실상 없다 — dname이 NULL인 경우만 조기 반환하며, 출력 실패는
 * seq_file이 내부적으로 버퍼 오버플로 상태로 처리한다.
 *
 * 호출 체인:
 *   blkcg_print_blkgs()/stat_show 콜백 -> blkg_prfill_rwstat() 또는
 *   bfqg_prfill_rwstat_recursive() 계열 -> [__blkg_prfill_rwstat] ->
 *   blkg_dev_name() / seq_printf()
 *
 * NVMe 연결: 사용자가 /sys/fs/cgroup/.../blkio.throttle.io_service_bytes
 * 등을 읽을 때 namespace/디바이스별 Read/Write/Discard 바이트(또는
 * 요청 수) 합계가 최종 텍스트로 변환되는 지점이다.
 */
u64 __blkg_prfill_rwstat(struct seq_file *sf, struct blkg_policy_data *pd,
			 const struct blkg_rwstat_sample *rwstat)
/* [한국어] 매개변수 타입은 실제 blk-cgroup.h의 struct blkg_policy_data(주의: 헤더 blk-cgroup-rwstat.h의 현재 선언은 이와 다른 blkcg_policy_data로 적혀 있는 불일치가 있음 - 파일 상단 "중요 발견" 참고, 이 정의부 자체는 순정 코드와 동일함) */
{
	static const char *rwstr[] = {
	/* [한국어] static const 지역 배열 - 함수 호출마다 재생성되지 않고 프로그램 전체에서 단 하나만 존재하는 읽기 전용 문자열 테이블. 인덱스는 enum blkg_rwstat_type을 그대로 사용해 rwstat->cnt[i]와 rwstr[i]가 나란히 짝지어짐 */
		[BLKG_RWSTAT_READ]	= "Read",
		/* [한국어] NVMe NVM command set 기준 READ(옵코드 0x02) 계열 방향의 cgroup 누적값에 붙는 출력 레이블 */
		[BLKG_RWSTAT_WRITE]	= "Write",
		/* [한국어] NVMe WRITE(옵코드 0x01)/Write Zeroes(0x08) 방향의 cgroup 누적값에 붙는 출력 레이블 */
		[BLKG_RWSTAT_SYNC]	= "Sync",
		/* [한국어] REQ_SYNC 플래그가 설정된 제출(동기 I/O)의 누적값 레이블 - Read/Write와 독립적인 두 번째 축이므로 Total 계산에는 포함되지 않음 */
		[BLKG_RWSTAT_ASYNC]	= "Async",
		/* [한국어] REQ_SYNC 미설정(비동기 제출)의 누적값 레이블 - SYNC와 상호 배타적인 짝 */
		[BLKG_RWSTAT_DISCARD]	= "Discard",
		/* [한국어] discard 방향의 누적값 레이블. NVMe에서는 Dataset Management(0x09)로 변환된다 */
	};
	const char *dname = blkg_dev_name(pd->blkg); /* [한국어] pd가 속한 blkcg_gq(pd->blkg)로부터 블록 디바이스의 표시 이름(예: "nvme0n1")을 얻음 - blk-cgroup.c/blkg_dev_name() 참고, 디바이스가 아직 probe 중이거나 이미 제거된 경우 NULL을 반환할 수 있음 */
	u64 v;
	/* [한국어] READ+WRITE+DISCARD 세 방향의 합산 결과를 담을 변수 - SYNC/ASYNC는 제외되므로 "총 데이터 전송량"에 가까운 값이며, 단위(바이트 또는 요청 수)는 이 rwstat을 채운 정책(blk-throttle은 바이트, BFQ는 상황에 따라 다름)에 따라 달라짐 */
	int i;
	/* [한국어] 아래 for 루프에서 BLKG_RWSTAT_NR(5)개 방향/동기성 항목을 순회하는 인덱스 */

	if (!dname)
	/* [한국어] blkg_dev_name()이 NULL을 반환했다면 이 blkg가 가리키는 디바이스 이름을 알 수 없는 상태(디바이스 probe 중/제거됨 등) */
		return 0; /* [한국어] 이 blkg에 대해서는 아무 것도 출력하지 않고 0을 반환 - 에러가 아니라 "출력할 것이 없음"을 뜻하며 호출자(blkg_prfill_rwstat 등)는 이 seq_file 출력을 건너뛴 채로 다음 blkg 처리를 계속함 */

	/* "nvme0n1 Read 12345" 형태로 READ/WRITE/SYNC/ASYNC/DISCARD 출력 */
	for (i = 0; i < BLKG_RWSTAT_NR; i++)
	/* [한국어] BLKG_RWSTAT_READ(0)부터 BLKG_RWSTAT_DISCARD(4)까지 5개 방향/동기성 항목을 순서대로 출력 */
		seq_printf(sf, "%s %s %llu\n", dname, rwstr[i],
			   rwstat->cnt[i]);
		/* [한국어] "<디바이스명> <방향명(Read/Write/Sync/Async/Discard)> <누적값>\n" 한 줄을 seq_file 버퍼에 기록 - rwstat->cnt[i]는 호출자가 이미 blkg_rwstat_read()나 blkg_rwstat_recursive_sum()으로 percpu_counter+aux_cnt를 합산해 둔 스냅숏 값 */

	/* READ+WRITE+DISCARD 합산: SYNC/ASYNC는 Total에 미포함 */
	v = rwstat->cnt[BLKG_RWSTAT_READ] +
	/* [한국어] READ 방향 누적값을 합산 시작값으로 사용 */
		rwstat->cnt[BLKG_RWSTAT_WRITE] +
		/* [한국어] WRITE 방향 누적값을 더함 */
		rwstat->cnt[BLKG_RWSTAT_DISCARD];
		/* [한국어] DISCARD(TRIM/Deallocate 계열) 방향 누적값을 마저 더해 v를 확정 - SYNC/ASYNC 두 인덱스는 의도적으로 이 합산에서 제외됨(둘은 R/W/D와 별개의 "동기성" 축이라 중복 집계가 되기 때문) */
	seq_printf(sf, "%s Total %llu\n", dname, v);
	/* [한국어] "<디바이스명> Total <v>\n" 요약 줄을 추가로 출력 - 개별 방향 5줄과 별개로, 순수 데이터 전송 계열(R/W/D)의 총합을 한눈에 보여주는 편의 라인 */
	return v;
	/* [한국어] 계산한 R+W+D 합계를 호출자에게 반환 - blkg_prfill_rwstat()이 이 반환값을 그대로 자신의 반환값으로 넘기며, blkcg_print_blkgs() 등 상위 순회 루틴이 blkg별 누적/후처리에 사용할 수 있음(정확한 용도는 호출자 구현에 따라 다름) */
}
EXPORT_SYMBOL_GPL(__blkg_prfill_rwstat);
/* [한국어] BFQ/blk-throttle 등의 prfill 콜백 구현이 이 저수준 포맷터를 직접 호출할 수 있도록 GPL 모듈에 심볼을 노출 */

/**
 * blkg_prfill_rwstat - prfill callback for blkg_rwstat
 * @sf: seq_file to print to
 * @pd: policy private data of interest
 * @off: offset to the blkg_rwstat in @pd
 *
 * prfill callback for printing a blkg_rwstat.
 */
/*
 * [한국어]
 * blkg_prfill_rwstat - blkg_policy_data 내부 오프셋의 blkg_rwstat을
 * 읽어 스냅숏을 만들고 __blkg_prfill_rwstat()으로 포맷팅한다
 *
 * @sf: 출력 대상 seq_file (위 __blkg_prfill_rwstat()와 동일한 cgroupfs
 *   읽기 컨텍스트).
 * @pd: 이 blkg의 정책 사설 데이터(struct blkg_policy_data). 이 구조체
 *   내부 어딘가에 struct blkg_rwstat 필드가 값으로 내장되어 있다(예:
 *   struct throtl_grp.stat_bytes).
 * @off: pd 구조체 시작 주소로부터 struct blkg_rwstat 필드까지의 바이트
 *   오프셋. 정책마다 policy_data 레이아웃이 다르므로, 어떤 rwstat
 *   필드를 읽을지는 이 오프셋으로 지정한다 — 호출자가 보통
 *   offsetof(struct throtl_grp, stat_bytes)처럼 offsetof()로 계산해
 *   넘긴다.
 * @return: __blkg_prfill_rwstat()의 반환값을 그대로 전달(R+W+D 합계).
 *
 * 이 함수는 "오프셋으로 필드를 찾아 읽고 -> 포맷팅"까지 한 번에
 * 처리하는 상위 래퍼로, cftype(cgroup 파일 타입)의 prfill 콜백
 * 시그니처(u64 (*)(struct seq_file *, struct blkg_policy_data *, int))
 * 에 바로 맞출 수 있는 형태다. 동작은 (1) pd 포인터에 off를 바이트
 * 단위로 더해 struct blkg_rwstat*를 얻고(포인터를 (void *)로 캐스팅한
 * 뒤 산술 연산), (2) blkg_rwstat_read()(헤더의 인라인 함수)로 그
 * 위치의 rwstat을 스냅숏 떠서 지역 변수 rwstat에 담고, (3)
 * __blkg_prfill_rwstat()에 넘겨 실제 출력을 위임한다. 실행 컨텍스트는
 * cgroupfs read() 시스템 콜 컨텍스트이며, 내부에서 호출하는
 * blkg_rwstat_read()가 percpu_counter를 순회하는 동안 특별한 락 없이
 * 근사치를 읽는다(percpu_counter_sum_positive() 자체의 동기화 규약에
 * 의존). 호출하는 상위 함수는 block/bfq-cgroup.c의
 * bfqg_print_rwstat()(blkcg_print_blkgs()에 이 함수를 prfill 콜백으로
 * 넘김, 라인 2703 부근)과 block/blk-throttle.c의 대응 stat_show
 * 콜백(라인 2112 부근)이며, 하위로 blkg_rwstat_read()와
 * __blkg_prfill_rwstat()을 호출한다. 에러 경로는 없다(off가 잘못되면
 * 정의되지 않은 동작이 되므로 이는 이 함수가 아니라 정책 구현자의
 * 책임이다).
 *
 * 호출 체인:
 *   cftype->seq_show -> blkcg_print_blkgs() -> [blkg_prfill_rwstat] ->
 *   blkg_rwstat_read() -> __blkg_prfill_rwstat() -> seq_printf()
 */
u64 blkg_prfill_rwstat(struct seq_file *sf, struct blkg_policy_data *pd,
		       int off)
/* [한국어] @off는 blkg_policy_data 내부에서 struct blkg_rwstat까지의 바이트 오프셋 - block/blk-throttle.c는 throtl_grp.stat_bytes/stat_ios, block/bfq-cgroup.c는 bfqg_stats의 각 필드를 이 방식으로 지정해 호출함 */
{
	struct blkg_rwstat_sample rwstat = { };
	/* [한국어] 스택에 놓인 임시 스냅숏 버퍼 - "= { }"로 BLKG_RWSTAT_NR개 cnt[] 원소를 모두 0으로 초기화한 뒤, 바로 다음 줄에서 blkg_rwstat_read()가 실제 값으로 덮어씀(초기화 자체는 아래 read 호출 전 방어적 조치) */

	/* @off 위치의 blkg_rwstat에서 READ/WRITE/DISCARD/SYNC/ASYNC 스냅샷 읽기 */
	blkg_rwstat_read((void *)pd + off, &rwstat);
	/* [한국어] (void *)pd + off: pd를 바이트 포인터로 캐스팅한 뒤 off만큼 이동해 정책별 policy_data 내부에 내장된 struct blkg_rwstat의 주소를 계산 - blkg_rwstat_read()(헤더의 인라인 함수)가 그 위치의 cpu_cnt[] 5개를 percpu_counter_sum_positive()로 각각 합산해 &rwstat에 채움(aux_cnt는 여기서는 포함되지 않는 "local" 관점의 값) */
	return __blkg_prfill_rwstat(sf, pd, &rwstat);
	/* [한국어] 방금 채운 스냅숏을 저수준 포맷터에 넘겨 실제 seq_file 출력을 위임하고, 그 반환값(R+W+D 합계)을 그대로 이 함수의 반환값으로 전달 */
}
EXPORT_SYMBOL_GPL(blkg_prfill_rwstat);
/* [한국어] cftype.seq_show 구현이나 blkcg_print_blkgs() 호출부가 함수 포인터로 참조할 수 있도록 GPL 모듈에 심볼을 노출 */

/**
 * blkg_rwstat_recursive_sum - collect hierarchical blkg_rwstat
 * @blkg: blkg of interest
 * @pol: blkcg_policy which contains the blkg_rwstat
 * @off: offset to the blkg_rwstat in blkg_policy_data or @blkg
 * @sum: blkg_rwstat_sample structure containing the results
 *
 * Collect the blkg_rwstat specified by @blkg, @pol and @off and all its
 * online descendants and their aux counts.  The caller must be holding the
 * queue lock for online tests.
 *
 * If @pol is NULL, blkg_rwstat is at @off bytes into @blkg; otherwise, it
 * is at @off bytes into @blkg's blkg_policy_data of the policy.
 */
/*
 * [한국어]
 * blkg_rwstat_recursive_sum - 지정한 blkg를 루트로 하위 online cgroup
 * 트리 전체의 rwstat을 재귀적으로 합산한다
 *
 * @blkg: 순회를 시작할 루트 blkcg_gq. 이 blkg 자신과 그 모든 자손
 *   cgroup에 대응하는 blkg들이 합산 대상이 된다.
 * @pol: 어떤 blkcg_policy(예: &blkcg_policy_bfq, &blkcg_policy_throtl)
 *   의 rwstat을 합산할지 지정. blkg마다 여러 정책의 policy_data가
 *   동시에 붙어 있을 수 있으므로 정책을 특정해야 한다. NULL이면
 *   blkg_policy_data가 아니라 struct blkcg_gq 자체 내부에서 @off를
 *   찾는다(커널 원본 kernel-doc 참고).
 * @off: @pol이 NULL이 아니면 blkg_to_pd(pos_blkg, pol)이 반환하는
 *   policy_data 시작 주소로부터, @pol이 NULL이면 blkg 자체 시작
 *   주소로부터 struct blkg_rwstat 필드까지의 바이트 오프셋.
 * @sum: 결과를 누적해 담을 출력 파라미터. 호출자가 보통
 *   `struct blkg_rwstat_sample sum = { }`로 0 초기화한 스택 변수의
 *   주소를 넘긴다 — 이 함수 내부에서도 memset()으로 다시 한 번 0
 *   초기화하므로 호출자가 초기화하지 않고 넘겨도 안전하다.
 * @return: 없음(void). 결과는 @sum out-parameter를 통해 반환된다.
 *
 * cgroup 계층은 트리 구조이고, 상위 cgroup의 "recursive"(재귀) 통계는
 * 그 자신과 모든 하위 cgroup들의 합이어야 한다. 이 함수는 @blkg를
 * 루트로 트리를 내려가며 각 노드의 cpu_cnt(현재 값, percpu_counter를
 * 합산)와 aux_cnt(이미 죽은 자식으로부터 승계된 값)를 모두 @sum에
 * 더해, 트리 전체의 정확한 합을 만든다. 동작 순서는 (1)
 * lockdep_assert_held()로 호출자가 queue_lock을 들고 있는지 디버그
 * 검증(락 자체를 여기서 잡지는 않음 — 커널 원본 kernel-doc의 "The
 * caller must be holding the queue lock for online tests"라는 요구를
 * 그대로 반영), (2) @sum을 memset()으로 0 초기화, (3)
 * rcu_read_lock()으로 cgroup 트리 순회 임계구간에 진입, (4)
 * blkg_for_each_descendant_pre()(내부적으로
 * css_for_each_descendant_pre()를 사용하는 매크로)로 @blkg 자신부터
 * 시작해 선순위(pre-order)로 모든 자손 blkg를 순회, (5) 각 노드마다
 * pos_blkg->online이 거짓(이미 오프라인 처리된 blkg)이면 건너뛰고,
 * (6) @pol 유무에 따라 policy_data 내부 또는 blkg 자체 내부에서
 * blkg_rwstat 위치를 포인터 산술로 계산, (7)
 * blkg_rwstat_read_counter()(헤더의 인라인 함수, percpu_counter 합산
 * + aux_cnt를 더한 값)를 BLKG_RWSTAT_NR개 인덱스 모두에 대해 호출해
 * @sum에 누적, (8) 순회가 끝나면 rcu_read_unlock()으로 임계구간
 * 종료. 실행 컨텍스트는 cgroupfs read() 시스템 콜 컨텍스트이며, RCU
 * 보호 하에 blkg 트리를 순회하므로 순회 도중 다른 CPU가 blkg를
 * 제거해도 use-after-free 없이 안전하게 순회를 마칠 수 있다(단,
 * queue_lock을 호출자가 미리 잡아 두어야 pos_blkg->online 판정이
 * 안정적임을 원본 주석이 요구한다). 호출하는 상위 함수는
 * block/bfq-cgroup.c의 재귀 통계 prfill 콜백(라인 2733 부근)과
 * block/blk-throttle.c의 대응 콜백(라인 2132 부근)이며, 하위로
 * lockdep_assert_held(), memset(), rcu_read_lock()/unlock(),
 * blkg_for_each_descendant_pre(), blkg_to_pd(),
 * blkg_rwstat_read_counter()를 호출한다. 명시적 에러 반환 경로는
 * 없으나, 트리 순회 중 blkg가 사라지는 경쟁 상태는 RCU와 online 플래그
 * 검사로 방어된다.
 *
 * 호출 체인:
 *   cftype->seq_show -> stat_show 콜백(bfq-cgroup.c/blk-throttle.c) ->
 *   [blkg_rwstat_recursive_sum] -> blkg_for_each_descendant_pre() ->
 *   blkg_to_pd() / blkg_rwstat_read_counter()
 */
void blkg_rwstat_recursive_sum(struct blkcg_gq *blkg, struct blkcg_policy *pol,
		int off, struct blkg_rwstat_sample *sum)
/* [한국어] @blkg: 합산을 시작할 루트 blkcg_gq, @pol: rwstat이 속한 blkcg_policy(NULL이면 blkg 자체 내부 오프셋 사용), @off: policy_data 또는 blkg 내부 rwstat 바이트 오프셋, @sum: 하위 트리까지 합산된 결과를 담을 출력 버퍼 */
{
	struct blkcg_gq *pos_blkg;
	/* [한국어] blkg_for_each_descendant_pre() 순회 중 "현재 방문 중인" 하위(자신 포함) blkcg_gq를 가리키는 커서 변수 */
	struct cgroup_subsys_state *pos_css;
	/* [한국어] cgroup_subsys_state 기반 트리 순회에 쓰이는 내부 커서 - blkg_for_each_descendant_pre() 매크로가 css_for_each_descendant_pre()를 감싸면서 pos_blkg <-> pos_css 사이를 자동으로 변환해 주므로 호출자는 pos_css의 내부 구조를 직접 다룰 필요는 없음 */
	unsigned int i;
	/* [한국어] BLKG_RWSTAT_NR(5, READ/WRITE/SYNC/ASYNC/DISCARD) 범위를 순회하며 각 하위 blkg의 값을 sum에 누적할 때 쓰는 인덱스 */

	/* queue_lock 보유 확인: NVMe 큐 상태와 통계 뷰의 동기화 가정 */
	lockdep_assert_held(&blkg->q->queue_lock);
	/* [한국어] @blkg가 속한 request_queue(blkg->q)의 queue_lock을 호출자가 이미 들고 있는지 lockdep으로 디버그 검증만 함(락을 여기서 새로 잡지는 않음) - 커널 원본 kernel-doc이 "caller must be holding the queue lock for online tests"라고 명시한 계약을 런타임에 강제하는 역할. 이 락이 없으면 순회 도중 pos_blkg->online 값이 다른 CPU에 의해 바뀌어 일관되지 않은 스냅숏이 만들어질 수 있음 */

	memset(sum, 0, sizeof(*sum)); /* 상위 집계값 초기화 */
	/* [한국어] sum->cnt[BLKG_RWSTAT_NR] 전체를 0바이트로 채워, 이후 루프에서 순수하게 "더하기만" 하면 되도록 깨끗한 시작 상태를 보장 - 호출자가 미리 { } 초기화를 했더라도 이중으로 안전하게 재초기화 */

	rcu_read_lock();
	/* [한국어] 아래 blkg_for_each_descendant_pre() 순회 구간을 RCU read-side critical section으로 감쌈 - 순회 도중 다른 CPU가 하위 blkg를 제거(kfree)하더라도 RCU grace period가 끝나기 전까지는 메모리가 실제로 해제되지 않으므로 use-after-free 없이 안전하게 트리를 따라갈 수 있음 */
	blkg_for_each_descendant_pre(pos_blkg, pos_css, blkg) {
	/* [한국어] @blkg 자신부터 시작해 그 모든 자손 blkg까지 선순위(pre-order, 부모를 자식보다 먼저 방문)로 순회 - 매크로 내부는 css_for_each_descendant_pre(pos_css, &blkg->blkcg->css) 위에 pos_css로부터 pos_blkg를 역산하는 형태로 구현되어 있다고 추정됨 */
		struct blkg_rwstat *rwstat;
		/* [한국어] 이번 순회 차례의 pos_blkg가 갖고 있는 blkg_rwstat 인스턴스를 가리킬 지역 포인터 - 아래에서 @pol 유무에 따라 다른 경로로 대입됨 */

		if (!pos_blkg->online)
		/* [한국어] pos_blkg->online이 거짓이면 이 blkg는 이미 blkcg_deactivate_policy() 등으로 오프라인 처리된 상태 - 그런 blkg는 더 이상 새 I/O를 받지 않으며(대응하는 자식의 통계는 이미 blkg_rwstat_add_aux()로 부모의 aux_cnt에 이관되었을 것으로 기대됨), 중복 집계를 피하기 위해 건너뜀 */
			continue; /* offline cgroup은 NVMe I/O 미발행으로 간주 */

		/* 정책별 또는 blkg 내부의 struct blkg_rwstat 위치 계산 */
		if (pol)
		/* [한국어] @pol이 NULL이 아니면, rwstat은 blkg가 아니라 그 blkg에 대응하는 특정 정책의 policy_data(blkg_policy_data) 안에 내장되어 있다는 뜻 */
			rwstat = (void *)blkg_to_pd(pos_blkg, pol) + off;
			/* [한국어] blkg_to_pd(pos_blkg, pol): pos_blkg->pd[pol->plid] 배열에서 이 정책의 blkg_policy_data 포인터를 얻음(blk-cgroup.h) - 그 주소에 @off를 바이트 단위로 더해 정책 구조체(예: throtl_grp) 내부에 값으로 내장된 struct blkg_rwstat의 실제 주소를 계산 */
		else
		/* [한국어] @pol이 NULL이면 rwstat은 policy_data가 아니라 blkcg_gq(pos_blkg) 자체 안에 직접 내장되어 있다는 뜻(일부 정책이 policy_data 없이 blkg 확장 형태로 rwstat을 두는 경우를 위한 대안 경로로 추정) */
			rwstat = (void *)pos_blkg + off;
			/* [한국어] pos_blkg를 바이트 포인터로 캐스팅한 뒤 @off만큼 이동해 blkg 자체 내부의 blkg_rwstat 주소를 계산 */

		/* percpu_counter와 aux_cnt를 합산해 상위 카운터에 누적 */
		for (i = 0; i < BLKG_RWSTAT_NR; i++)
		/* [한국어] READ/WRITE/SYNC/ASYNC/DISCARD 5개 인덱스 각각에 대해 이번 pos_blkg의 값을 sum에 누적 */
			sum->cnt[i] += blkg_rwstat_read_counter(rwstat, i);
			/* [한국어] blkg_rwstat_read_counter(rwstat, i) = atomic64_read(&rwstat->aux_cnt[i]) + percpu_counter_sum_positive(&rwstat->cpu_cnt[i])(헤더의 인라인 함수) - 이 pos_blkg 하나가 "현재 살아있는 값 + 이미 승계받은 값"을 합쳐 가진 방향별 카운트를 sum->cnt[i]에 더함. 트리의 모든 온라인 노드를 거치며 이 누적이 반복되므로 최종적으로 sum은 @blkg 이하 전체 하위 트리의 합이 됨 */
	}
	rcu_read_unlock();
	/* [한국어] 트리 순회 종료 - RCU read-side critical section을 빠져나가 이 시점부터는 더 이상 pos_blkg 등 순회 중 얻은 포인터들의 유효성을 RCU가 보장해 주지 않음(순회 밖에서는 이미 사용하지 않으므로 문제 없음) */
}
EXPORT_SYMBOL_GPL(blkg_rwstat_recursive_sum);
/* [한국어] block/bfq-cgroup.c, block/blk-throttle.c의 재귀 통계 stat_show 콜백이 이 함수를 직접 호출할 수 있도록 GPL 모듈에 심볼을 노출 */

/* NVMe 관점 핵심 요약 */
/*
 * - 이 파일은 blk-mq를 통해 NVMe 등 블록 디바이스로 I/O가 전달되기
 *   전에(또는 그 회계와 독립적으로) cgroup별 READ/WRITE/DISCARD/SYNC/
 *   ASYNC 계수를 누적·조회하는 legacy 회계(accounting) 계층이다.
 * - 실제 NVMe SQ/CQ/doorbell 발행은 이 파일과 별개이며, 이 파일은
 *   소프트웨어 카운터(percpu_counter, atomic64_t)만 다룬다.
 * - blkg_rwstat_init()/blkg_rwstat_exit()은 percpu_counter 기반
 *   cpu_cnt의 생애주기를 관리해 고속 I/O 경로에서의 통계 수집
 *   오버헤드를 줄인다.
 * - __blkg_prfill_rwstat()/blkg_prfill_rwstat()은 스냅숏을 cgroupfs
 *   텍스트로 직렬화하고, blkg_rwstat_recursive_sum()은 여러 하위
 *   cgroup이 공유하는 디바이스의 계층적 사용량을 집계한다.
 * - 실제 소비자는 block/bfq-cgroup.c(BFQ_GROUP_IOSCHED)와
 *   block/blk-throttle.c(BLK_DEV_THROTTLING)이며, NVMe 드라이버
 *   자체는 이 파일을 직접 호출하지 않는다.
 * - block/blk-cgroup-rwstat.h의 __blkg_prfill_rwstat()/
 *   blkg_prfill_rwstat() 선언에 남아있는 blkcg_policy_data vs
 *   blkg_policy_data 타입 불일치는 이 .c 파일의 버그가 아니라 헤더
 *   쪽의 과거 주석 작업(커밋 ea5479b64c)에서 생긴 것으로 보이며,
 *   별도로 헤더를 바로잡는 후속 조치가 필요하다(파일 상단 "중요
 *   발견" 섹션 참고).
 */
