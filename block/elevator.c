// SPDX-License-Identifier: GPL-2.0
/*
 *  Block device elevator/IO-scheduler.
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 *
 * 30042000 Jens Axboe <axboe@kernel.dk> :
 *
 * Split the elevator a bit so that it is possible to choose a different
 * one or even write a new "plug in". There are three pieces:
 * - elevator_fn, inserts a new request in the queue list
 * - elevator_merge_fn, decides whether a new buffer can be merged with
 *   an existing request
 * - elevator_dequeue_fn, called when a request is taken off the active list
 *
 * 20082000 Dave Jones <davej@suse.de> :
 * Removed tests for max-bomb-segments, which was breaking elvtune
 *  when run without -bN
 *
 * Jens:
 * - Rework again to work with bio instead of buffer_heads
 * - loose bi_dev comparisons, partition handling is right now
 * - completely modularize elevator setup and teardown
 *
 */
/*
 * [한국어 설명] 블록 계층 IO 스케줄러(elevator) 핵심 구현 (elevator.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 리눅스 블록 계층에서 IO 스케줄러(elevator)의 공통 인터페이스와
 * 핵심 자료구조 관리를 담당한다. elevator는 응용이 발행한 bio/request를
 * mq-deadline·BFQ·kyber·none 등의 스케줄러 플러그인에 연결하고, 요청 병합
 * (merge)·LBA 기반 해시·RB-tree를 통해 NVMe SQ로 내려가기 전 단계에서 요청을
 * 정렬·병합·스케줄링한다. 스케줄러 등록/해제, sysfs 연동, 동적 교체(switch)
 * 로직도 이 파일에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * IO 경로에서 elevator는 blk-mq와 실제 드라이버(nvme_queue_rq) 사이에 위치한다:
 *
 *   [응용] write(2) → submit_bio()
 *       ↓
 *   [blk-mq] blk_mq_submit_bio()
 *       ↓  → elv_merge(): 기존 request와 병합 시도 (back/front/discard)
 *       ↓  → elv_attempt_insert_merge(): 신규 request를 해시에서 찾은 후보와 병합
 *       ↓  → 스케줄러 ops.insert_requests(): mq-deadline/BFQ 큐에 삽입
 *       ↓
 *   [blk-mq dispatch] ops.dispatch_request(): 스케줄러가 최적 request 선택
 *       ↓
 *   [NVMe 드라이버] nvme_queue_rq() → SQ doorbell → CQ 완료
 *
 * elevator가 없을 때("none" 선택 시): blk-mq가 직접 request를 드라이버에 전달.
 * 실행 컨텍스트: 대부분 프로세스 컨텍스트(bio 제출 경로); elevator_release는
 * kobject_put() 경로이므로 어떤 컨텍스트에서도 호출될 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * 의존 모듈:
 *   - block/blk-mq.c: blk_mq_submit_bio()→elv_merge() 호출, blk_mq_init_sched()
 *   - block/blk-mq-sched.c: blk_mq_init_sched()·blk_mq_exit_sched() — elevator
 *     초기화·해제 시 tag_set과 연결
 *   - block/mq-deadline.c, bfq-iosched.c, kyber-iosched.c: elv_register()로
 *     elv_list에 등록; ops 콜백(insert/dispatch/allow_merge 등) 제공
 *   - block/blk-merge.c: blk_attempt_req_merge()·blk_try_merge() 구현
 *   - block/blk-ioc.c: ioc_clear_queue() — elevator 종료 시 io_context 정리
 * 공유 자료구조:
 *   - struct elevator_queue (elevator.h): type·kobj·hash·flags·elevator_data
 *   - struct elevator_type (elevator.h): ops vtable·elevator_name·icq_cache
 *   - elv_list (전역): 등록된 스케줄러 목록, elv_list_lock으로 보호
 *
 * === 주요 함수/구조체 요약 ===
 * elv_merge()              - bio가 기존 request와 병합 가능한지 결정; 해시·캐시·스케줄러 순으로 탐색
 * elv_attempt_insert_merge() - 신규 request를 해시에서 찾은 후보에 연속 back-merge; SQ 엔트리 수 감소
 * elv_register/unregister()  - 스케줄러 모듈이 전역 elv_list에 등록/해제
 * elevator_change()          - sysfs/내부 요청으로 elevator를 동적 교체; queue freeze→switch→unfreeze
 * elevator_set_default()     - 장치 등록 시 mq-deadline(단일큐) 또는 none(멀티큐) 기본 적용
 * elv_rqhash_{add/del/find}()- 끝 섹터 기반 해시로 back-merge 후보를 O(1) 탐색
 * elv_rb_{add/del/find}()    - LBA 기반 RB-tree로 front-merge·dispatch 순서를 O(log N) 탐색
 * struct elevator_queue      - 디바이스 큐(request_queue)에 붙는 elevator 상태; type·hash·kobj
 * struct elevator_type       - 스케줄러 플러그인 설명자; ops vtable·이름·icq 캐시
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/compiler.h>
#include <linux/blktrace_api.h>
#include <linux/hash.h>
#include <linux/uaccess.h>
#include <linux/pm_runtime.h>

#include <trace/events/block.h>

#include "elevator.h"
#include "blk.h"
#include "blk-mq-sched.h"
#include "blk-pm.h"
#include "blk-wbt.h"
#include "blk-cgroup.h"

/* [한국어] 전역 스케줄러 등록 목록(elv_list)을 보호하는 스핀락.
 * 보호 대상: elv_list 자체의 연결 구조와, 그 위에서 수행하는 elevator_tryget().
 * 경쟁 상황: mq-deadline/bfq/kyber는 모듈로 빌드될 수 있어, 한 CPU가
 *   sysfs로 스케줄러를 바꾸는 중에 다른 CPU가 rmmod로 그 스케줄러를 목록에서
 *   빼려 할 수 있다. 락 없이는 이미 해제된 elevator_type을 참조하게 된다.
 * 범위: 시스템 전역(큐별이 아님). 스케줄러 "종류"는 모든 블록 장치가 공유하는
 *   전역 자원이기 때문이다. 큐별 인스턴스는 q->elevator가 따로 갖는다. */
static DEFINE_SPINLOCK(elv_list_lock);
/* [한국어] 커널에 등록된 모든 I/O 스케줄러(struct elevator_type)의 목록.
 * 설정자: elv_register()가 모듈 로드 시 추가, elv_unregister()가 언로드 시 제거.
 * 읽는 자: __elevator_find()(이름으로 조회), elv_iosched_show()(sysfs에서
 *   선택 가능한 목록 출력).
 * NVMe와의 관계: /sys/block/nvme0n1/queue/scheduler를 cat하면 이 목록이
 *   나오고, echo로 쓰면 여기서 이름을 찾아 큐에 붙인다. NVMe 기본값은
 *   "none"(스케줄러 없음)인데, 하드웨어 큐가 여러 개라 소프트웨어에서
 *   순서를 다시 정할 이유가 적기 때문이다. */
static LIST_HEAD(elv_list);

/*
 * Merge hash stuff.
 */
#define rq_hash_key(rq)		(blk_rq_pos(rq) + blk_rq_sectors(rq))

/*
 * [한국어] rq_hash_key - request의 "끝나는 섹터"를 해시 키로 만든다
 *
 * blk_rq_pos(rq)는 시작 섹터, blk_rq_sectors(rq)는 길이이므로 둘을 더하면
 * 이 request가 끝나는 지점(= 다음 섹터의 시작 위치)이 된다.
 *
 * 왜 "끝"을 키로 쓰는가:
 *   back merge는 "새 bio의 시작 == 기존 request의 끝"일 때 성립한다. 따라서
 *   request를 끝 섹터로 해시해 두면, 새 bio가 왔을 때 그 bio의 시작 섹터로
 *   해시를 한 번 조회하는 것만으로 O(1)에 병합 후보를 찾을 수 있다.
 *   시작 섹터로 해시했다면 후보를 찾기 위해 전체를 뒤져야 했을 것이다.
 *
 * front merge는 이 해시로 찾을 수 없다(반대 방향이므로). 그래서 elv_merge()는
 * 해시 조회가 실패하면 스케줄러의 request_merge 콜백으로 넘겨, 정렬된
 * rb-tree에서 front 후보까지 찾게 한다.
 *
 * NVMe 관점: 이 해시 한 번의 조회가 성공할 때마다 SQ 엔트리 하나, CID 하나,
 * 완료 처리 한 번이 절약된다. 순차 워크로드에서 적중률이 매우 높다.
 */
/*
 * Query io scheduler to see if the current process issuing bio may be
 * merged with rq.
 */
/*
 * elv_iosched_allow_bio_merge - IO 스케줄러가 @bio를 @rq와 병합할 수 있는지 확인
 *   호출 경로: elv_bio_merge_ok -> elv_iosched_allow_bio_merge
 *            -> (스케줄러별 allow_merge)
 *   NVMe 연결: mq-deadline/bfq 등에서 정책상 병합을 허용하면, 이후
 *              blk_try_merge() 또는 blk_attempt_req_merge()로 연결되어
 *              SQ에 들어갈 PRP/SGL 체인 길이를 줄일 수 있다.
 */
static bool elv_iosched_allow_bio_merge(struct request *rq, struct bio *bio)
{
	/* [한국어] request가 속한 큐. 스케줄러 콜백이 큐 컨텍스트를 필요로 한다. */
	struct request_queue *q = rq->q;
	/* [한국어] 이 큐에 붙은 스케줄러 인스턴스. 이 함수는 스케줄러가 반드시
	 * 존재하는 경로(elv_merge 등)에서만 호출되므로 NULL 검사를 하지 않는다.
	 * 스케줄러가 없는 NVMe 기본 구성("none")에서는 이 경로 자체를 타지 않고
	 * blk_attempt_plug_merge()가 병합을 처리한다. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] allow_merge 콜백은 선택 사항이다. 구현한 스케줄러:
	 *   BFQ  - bfq_allow_bio_merge(). 두 I/O가 같은 bfq_queue(대략 같은
	 *          프로세스)에 속하는지 확인한다. 다른 프로세스의 I/O를 합치면
	 *          완료 시 서비스 시간을 누구에게 청구할지 모호해져, BFQ의
	 *          가중치 기반 공정성이 무너진다.
	 *   mq-deadline / kyber - 구현하지 않음. 지연 목표만 관리하고 프로세스
	 *          단위 공정성은 추구하지 않으므로 병합을 막을 이유가 없다. */
	if (e->type->ops.allow_merge)
		return e->type->ops.allow_merge(q, rq, bio);

	/* [한국어] 콜백이 없으면 허용이 기본값 — 병합은 거의 항상 이득이므로
	 * "명시적으로 막지 않는 한 허용"이 올바른 기본 정책이다. */
	return true;
}

/*
 * can we safely merge with this request?
 */
/*
 * elv_bio_merge_ok - 요청 병합의 기본 안전성 검사
 *   blk_rq_merge_ok()로 장치/방향/기타 제약을 확인하고,
 *   스케줄러 정책 허용 여부를 elv_iosched_allow_bio_merge()로 재확인한다.
 *   NVMe 연결: 병합에 성공하면 여러 bio가 하나의 struct request로 묶이고,
 *              nvme_setup_rw()가 그것을 SLBA/NLB가 확장된 커맨드 하나로
 *              변환한다. 그 결과 SQ 엔트리 하나, Command ID 하나, 완료 CQ
 *              엔트리 하나가 절약된다.
 *
 * === 두 단계 검사인 이유 ===
 * blk_rq_merge_ok()는 "하드웨어와 데이터 정합성상 합쳐도 되는가"(연산 종류,
 * cgroup, PI, 암호화, 우선순위)를 보고, elv_iosched_allow_bio_merge()는
 * "스케줄러 정책상 합쳐도 되는가"를 본다. 후자가 필요한 이유는 BFQ 같은
 * 공정성 스케줄러 때문이다 — 서로 다른 프로세스의 I/O를 합치면 그 request가
 * 완료될 때 누구의 몫으로 계산할지 모호해져 공정성 보장이 무너진다.
 * mq-deadline은 allow_merge 콜백이 없어 항상 허용한다.
 */
bool elv_bio_merge_ok(struct request *rq, struct bio *bio)
{
	/* [한국어] 1단계 — 하드웨어·정합성 검사(block/blk-merge.c). 연산 종류,
	 * cgroup 소속, 무결성(PI) 설정, 인라인 암호화 컨텍스트, write hint,
	 * write stream, I/O 우선순위, 원자성 요구가 모두 호환되어야 한다.
	 * 하나라도 어긋나면 합쳐진 커맨드가 잘못된 결과를 내므로 즉시 거부한다. */
	if (!blk_rq_merge_ok(rq, bio))
		return false;

	/* [한국어] 2단계 — 스케줄러 정책 검사. 하드웨어적으로는 가능하지만
	 * 스케줄러가 원하지 않을 수 있다(BFQ의 프로세스 간 격리 등).
	 * 순서가 중요하다: 싸고 자주 걸리는 하드웨어 검사를 먼저 해서, 대부분의
	 * 거부를 콜백 호출 없이 처리한다. */
	if (!elv_iosched_allow_bio_merge(rq, bio))
		return false;

	/* [한국어] 두 관문 통과 — 병합 가능. 호출자(elv_merge, elv_rqhash_find
	 * 이후 경로)가 blk_try_merge()로 방향(back/front/discard)을 정한다. */
	return true;
}
EXPORT_SYMBOL(elv_bio_merge_ok);

/**
 * elevator_match - Check whether @e's name or alias matches @name
 * @e: Scheduler to test
 * @name: Elevator name to test
 *
 * Return true if the elevator @e's name or alias matches @name.
 */
/*
 * [한국어]
 * elevator_match - 스케줄러의 정식 이름 또는 별칭이 @name과 일치하는지 확인
 *
 * @e:    검사할 스케줄러 타입
 * @name: 찾는 이름. 보통 사용자가 /sys/block/nvme0n1/queue/scheduler에 쓴 문자열.
 * @return: true = 이 스케줄러가 요청된 것
 *
 * 별칭(elevator_alias)이 존재하는 이유는 하위 호환이다. 예를 들어 예전
 * 단일 큐 시절의 "deadline"이라는 이름으로 쓰는 스크립트가 많은데, 지금의
 * 구현은 "mq-deadline"이다. 별칭 덕분에 옛 스크립트가 그대로 동작한다.
 *
 * 실행 컨텍스트: elv_list_lock을 쥔 상태(호출자 __elevator_find가 잡는다).
 * 순수 문자열 비교로 부수 효과가 없다.
 *
 * 호출 체인:
 *   elv_iosched_store(sysfs) → elevator_find_get → __elevator_find
 *     → [elevator_match]
 */
static bool elevator_match(const struct elevator_type *e, const char *name)
{
	/* [한국어] 정식 이름이 일치하거나, 별칭이 등록되어 있고 그것이 일치하면 참.
	 * 단락 평가 순서상 정식 이름을 먼저 보므로, 별칭 검사는 정식 이름이
	 * 다를 때만 수행된다. elevator_alias NULL 검사를 먼저 하는 것은
	 * strcmp에 NULL을 넘기는 것을 막기 위한 필수 가드다. */
	return !strcmp(e->elevator_name, name) ||
		(e->elevator_alias && !strcmp(e->elevator_alias, name));
}

/*
 * [한국어]
 * __elevator_find - 등록 목록에서 이름으로 스케줄러를 찾는다(참조 획득 없음)
 *
 * @name: 찾을 스케줄러 이름
 * @return: 찾은 elevator_type, 없으면 NULL
 *
 * 앞의 밑줄 두 개는 커널 관례로 "락을 이미 잡은 상태에서 호출하라"는 뜻이다.
 * 호출자 elevator_find_get()이 elv_list_lock을 쥐고 이 함수를 부른 뒤,
 * 같은 락 안에서 참조까지 획득한다. 찾기와 참조 획득이 원자적으로 이루어져야
 * 그 사이에 모듈이 언로드되는 것을 막을 수 있기 때문이다.
 *
 * 실행 컨텍스트: elv_list_lock을 반드시 쥔 상태.
 *
 * 호출 체인:
 *   elevator_find_get → [__elevator_find] → elevator_match
 *   elv_register (중복 등록 검사) → [__elevator_find]
 */
static struct elevator_type *__elevator_find(const char *name)
{
	struct elevator_type *e;

	/* [한국어] 등록된 스케줄러를 선형 탐색한다. 목록에 있는 항목이 보통 3~5개
	 * (none은 목록에 없고, mq-deadline/bfq/kyber 정도)라 해시 같은 자료구조를
	 * 쓸 이유가 없다. 게다가 이 경로는 sysfs로 스케줄러를 바꿀 때만 실행되는
	 * 냉경로(cold path)라 성능이 문제되지 않는다. */
	list_for_each_entry(e, &elv_list, list)
		if (elevator_match(e, name))
			return e;
	/* [한국어] 없음 — 사용자가 존재하지 않는 스케줄러 이름을 썼거나, 해당
	 * 모듈이 아직 로드되지 않았다. 호출자가 request_module()로 자동 로드를
	 * 시도한 뒤 다시 찾기도 한다. */
	return NULL;
}

/*
 * [한국어]
 * elevator_find_get - 이름으로 스케줄러를 찾고 모듈 참조까지 안전하게 획득
 *
 * @name: 찾을 스케줄러 이름
 * @return: 참조를 획득한 elevator_type. 없거나 획득 실패면 NULL.
 *          호출자는 다 쓴 뒤 elevator_put()으로 반드시 반납해야 한다.
 *
 * === 찾기와 참조 획득이 같은 락 안에 있어야 하는 이유 ===
 * 만약 락을 풀고 나서 참조를 잡으려 하면 그 사이에 다른 CPU가 rmmod로
 * 모듈을 내려 elevator_type 구조체 자체가 해제될 수 있다. 그러면 이미
 * 해제된 메모리에 대해 참조를 잡으려 시도하는 use-after-free가 된다.
 * 그래서 락 안에서 "찾기 → 참조 획득"을 원자적으로 끝낸다.
 *
 * elevator_tryget()이 실패할 수 있는 이유: 모듈 언로드가 이미 시작되어
 * 모듈 참조 카운트를 더 올릴 수 없는 상태다. 아직 목록에서 제거되지는
 * 않았지만 곧 사라질 것이므로, 새로 쓰겠다고 잡아서는 안 된다.
 *
 * NVMe 관점: 사용자가 /sys/block/nvme0n1/queue/scheduler에 "mq-deadline"을
 * 쓰면 이 함수로 스케줄러를 확보한 뒤 큐에 붙인다. 참조를 잡아 두므로
 * 그 스케줄러를 사용하는 큐가 하나라도 있는 한 모듈을 내릴 수 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(sysfs write 또는 큐 초기화).
 * 내부에서 스핀락을 잡으므로 그 구간에서는 잠들 수 없다.
 *
 * 호출 체인:
 *   elv_iosched_store / elevator_init_mq → [elevator_find_get]
 *     → __elevator_find → elevator_tryget
 */
static struct elevator_type *elevator_find_get(const char *name)
{
	struct elevator_type *e;

	/* [한국어] 목록 보호 락 획득. 이 구간 안에서 찾기와 참조 획득을 모두 끝낸다. */
	spin_lock(&elv_list_lock);
	/* [한국어] 이름으로 조회. 락을 이미 쥐었으므로 __ 접두사 버전을 쓴다. */
	e = __elevator_find(name);
	/* [한국어] 찾았다면 모듈 참조를 시도한다. try_module_get() 기반이라
	 * 언로드가 진행 중이면 실패한다. 실패 시 e를 NULL로 만들어 "못 찾았다"와
	 * 같은 결과로 취급한다 — 호출자 입장에서 "쓸 수 없다"는 점은 동일하다. */
	if (e && (!elevator_tryget(e)))
		e = NULL;
	spin_unlock(&elv_list_lock);
	/* [한국어] 참조를 쥔 포인터 또는 NULL을 반환한다. 반환값이 NULL이 아니면
	 * 호출자가 elevator_put()으로 반납할 책임을 진다. */
	return e;
}

static const struct kobj_type elv_ktype;

/*
 * struct elevator_queue 주요 필드 (NVMe 관점)
 *   type          : 등록된 IO 스케줄러(mq-deadline/bfq/none)의 ops 테이블.
 *                   NVMe request_queue는 이 ops를 통해 삽입/디스패치/병합 정책을 따름.
 *   kobj/sysfs_lock: /sys/block/<disk>/queue/iosched sysfs 노출용.
 *                   NVMe 드라이버/관리자는 이 경로로 스케줄러 파라미터를 변경.
 *   hash          : 요청 끝 섹터 기반 해시, 연속 LBA bio의 back-merge 탐색에 사용.
 *   et            : blk_mq_sched가 할당한 스케줄러 전용 tag/자원 정보.
 *   elevator_data : 스케줄러 사설 데이터(e.g. mq-deadline의 fifo/deadline 라인).
 *   flags         : ELEVATOR_FLAG_REGISTERED/DYING 등.
 *                   NVMe 장치 제거 시 DYING 플래그로 스케줄러 종료를 제어.
 *
 * elevator_alloc - request_queue에 elevator_queue를 할당/초기화
 *   호출 경로: blk_mq_init_sched -> elevator_alloc
 *   NVMe 연결: NVMe namespace의 request_queue 생성 시 tag_set과 함께
 *              스케줄러 자원이 할당되며, 이후 nvme_queue_rq()가 이 elevator를
 *              경유해 요청을 꺼낸다.
 */
struct elevator_queue *elevator_alloc(struct request_queue *q,
		struct elevator_type *e, struct elevator_resources *res)
{
	struct elevator_queue *eq;

/* NVMe request_queue node에 맞춰 elevator 상태 메모리 할당 */
	eq = kzalloc_node(sizeof(*eq), GFP_KERNEL, q->node);
	if (unlikely(!eq))
		return NULL;

	__elevator_get(e);
/* type: 사용할 스케줄러(mq-deadline/bfq/none)의 ops 등록 */
	eq->type = e;
	kobject_init(&eq->kobj, &elv_ktype);
/* kobj: /sys/block/<disk>/queue/iosched 노드 초기화 */
	mutex_init(&eq->sysfs_lock);
/* sysfs_lock: iosched tunable 동시 접근 보호 */
	hash_init(eq->hash);
/* hash: back-merge를 위한 끝 섹터 해시 초기화 */
	eq->et = res->et;
/* et: blk_mq_sched가 준비한 tag/스케줄러 자원 참조 */
	eq->elevator_data = res->data;
/* elevator_data: 스케줄러 사설 상태(e.g. mq-deadline 큐) 저장 */

	return eq;
}

/*
 * [한국어]
 * elevator_release - elevator_queue의 kobject 참조가 0이 됐을 때 메모리 해제
 *
 * @kobj: 해제할 elevator_queue에 내장된 kobject 포인터
 *
 * kobject_put()이 참조 카운트를 0으로 만들면 kobj_type.release로 등록된
 * 이 함수가 호출된다. elevator_queue는 직접 kfree할 수 없고 반드시 kobject
 * 생명주기를 통해 해제해야 하는데, 이 함수가 그 최종 단계이다.
 * elevator_exit() → kobject_del() → kobject_put() → elevator_release() 순.
 *
 * 호출 체인:
 *   elevator_exit/elv_exit_and_release → kobject_put(&e->kobj) → [elevator_release]
 */
static void elevator_release(struct kobject *kobj)
{
	struct elevator_queue *e;

/* kobj에서 상위 elevator_queue 포인터 역산: kobject는 구조체 내장 멤버 */
	e = container_of(kobj, struct elevator_queue, kobj);
/* 스케줄러 타입(elevator_type) 모듈 참조 해제: 이 시점이 마지막 사용자일 수 있음 */
	elevator_put(e->type);
/* elevator_queue 구조체 자체 해제 */
	kfree(e);
}

/*
 * elevator_exit - request_queue에서 elevator를 분리하고 정리
 *   호출 경로: elevator_switch -> elevator_exit -> blk_mq_exit_sched
 *   NVMe 연결: NVMe 컨트롤러 리셋/제거 시 queue freeze 후 호출되며,
 *              더 이상 nvme_queue_rq()가 이 elevator의 dispatch를 요구하지 않도록
 *              스케줄러 상태를 해제한다.
 */
static void elevator_exit(struct request_queue *q)
{
	struct elevator_queue *e = q->elevator;

	lockdep_assert_held(&q->elevator_lock);

	ioc_clear_queue(q);

	mutex_lock(&e->sysfs_lock);
/* blk_mq_exit_sched: NVMe tag-set과 연결된 스케줄러 자원 해제 */
	blk_mq_exit_sched(q, e);
	mutex_unlock(&e->sysfs_lock);
}

/*
 * __elv_rqhash_del - 요청을 섹터 기반 해시에서 제거
 *   RQF_HASHED 플래그를 클리어하여 NVMe 디스패치 경로에서 중복 제거됨을 표시.
 */
static inline void __elv_rqhash_del(struct request *rq)
{
/* 해시에서 제거: 이미 NVMe SQ에 나간 요청은 병합 후보에서 제외 */
	hash_del(&rq->hash);
/* RQF_HASHED 클리어 */
	rq->rq_flags &= ~RQF_HASHED;
}

/*
 * [한국어]
 * elv_rqhash_del - request를 병합 후보 해시에서 안전하게 제거
 *
 * @q:  request가 속한 큐 (현재 구현에서는 쓰이지 않지만 API 일관성상 유지)
 * @rq: 해시에서 뺄 request
 * @return: 없음
 *
 * request가 드라이버로 dispatch되거나 병합으로 소멸하면 더 이상 병합 대상이
 * 아니므로 해시에서 빼야 한다. 남겨 두면 이미 NVMe SQ에 실린(또는 해제된)
 * request에 새 bio를 붙이려는 시도가 발생한다.
 *
 * ELV_ON_HASH() 검사가 필요한 이유: 이 함수는 여러 경로에서 호출되고, 그중
 * 일부는 해당 request가 애초에 해시에 들어간 적이 없다(스케줄러를 거치지 않은
 * passthrough, 이미 다른 경로에서 제거된 경우). 해시에 없는 노드를
 * hash_del()하면 리스트 포인터가 손상되므로 반드시 확인해야 한다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 상태. 해시는 큐 단위 자료구조라 보호가 필요하다.
 *
 * 호출 체인:
 *   elv_merge_requests / 스케줄러의 dispatch·완료 경로
 *     (dd_finish_request, bfq_finish_requeue_request 등) → [elv_rqhash_del]
 */
void elv_rqhash_del(struct request_queue *q, struct request *rq)
{
	/* [한국어] RQF_HASHED 플래그로 "실제로 해시에 들어 있는지"를 확인한다.
	 * 이 검사 덕분에 호출자가 중복 호출하거나 해시에 없는 request를 넘겨도
	 * 안전하다(멱등성). */
	if (ELV_ON_HASH(rq))
		__elv_rqhash_del(rq);
}
EXPORT_SYMBOL_GPL(elv_rqhash_del);

/*
 * elv_rqhash_add - 요청을 끝 섹터 키로 해시에 삽입
 *   호출 경로: elv_attempt_insert_merge, elv_merged_request 등
 *   NVMe 연결: 연속된 LBA를 가진 새로운 bio가 들어왔을 때 back-merge 후보를
 *              빠르게 찾아 CID를 아끼고 PRP/SGL 개수를 줄일 수 있다.
 */
void elv_rqhash_add(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* BUG_ON: 이미 해시에 있는 request를 중복 삽입하면 안 됨 */
	BUG_ON(ELV_ON_HASH(rq));
/* 끝 섹터(rq_hash_key)를 키로 해시 추가 */
	hash_add(e->hash, &rq->hash, rq_hash_key(rq));
/* RQF_HASHED 설정: 병합 후보 탐색 가능 표시 */
	rq->rq_flags |= RQF_HASHED;
}
EXPORT_SYMBOL_GPL(elv_rqhash_add);

/*
 * elv_rqhash_reposition - 병합 등으로 요청 크기가 달라졌을 때 해시 재배치
 *   병합 후 rq_hash_key가 변경되면 다시 해시에 넣어 NVMe merge 후보 탐색이
 *   정확히 동작하도록 한다.
 */
void elv_rqhash_reposition(struct request_queue *q, struct request *rq)
{
	__elv_rqhash_del(rq);
	elv_rqhash_add(q, rq);
}

/*
 * elv_rqhash_find - 끝 섹터가 @offset인 merge 후보 request 탐색
 *   호출 경로: elv_merge -> elv_rqhash_find
 *   NVMe 연결: 연속 LBA bio가 들어오면 이 함수로 후보를 찾아
 *              blk_try_merge()로 병합; 실패 시 request_merge ops로 폴백(fallback).
 */
struct request *elv_rqhash_find(struct request_queue *q, sector_t offset)
{
	/* [한국어] 해시 테이블은 스케줄러 인스턴스(elevator_queue)마다 하나씩 있다.
	 * 큐마다 독립적이므로 다른 NVMe 네임스페이스의 request가 섞이지 않는다. */
	struct elevator_queue *e = q->elevator;
	/* [한국어] _safe 순회에 필요한 다음 노드 보관용. 순회 도중 현재 노드를
	 * 삭제할 수 있기 때문에 반드시 필요하다(아래 __elv_rqhash_del 참고). */
	struct hlist_node *next;
	struct request *rq;

	/* [한국어] offset을 키로 같은 버킷에 있는 request들을 순회한다.
	 * 해시 충돌 때문에 키가 다른 request도 같은 버킷에 있을 수 있어,
	 * 아래에서 rq_hash_key를 한 번 더 비교해야 한다.
	 * _safe 변형인 이유: 루프 안에서 __elv_rqhash_del()로 현재 노드를
	 * 제거하는데, 일반 순회 매크로는 제거된 노드의 next 포인터를 읽어
	 * use-after-free를 일으킨다. */
	hash_for_each_possible_safe(e->hash, rq, next, hash, offset) {
		/* [한국어] 해시 버킷에 들어 있는데 RQF_HASHED가 없다면 자료구조가
		 * 손상된 것이다. 조용히 넘어가면 이후 병합에서 잘못된 request를
		 * 건드리므로 즉시 커널을 멈춰 디버깅을 강제한다. */
		BUG_ON(!ELV_ON_HASH(rq));

		/* [한국어] 이 request가 더 이상 병합 가능한 상태가 아니라면(REQ_NOMERGE가
		 * 붙었거나 이미 dispatch되었거나 FLUSH 계열이면) 해시에 남아 있을
		 * 이유가 없다. 찾는 김에 청소하는 지연 제거(lazy deletion) 전략으로,
		 * 별도의 청소 순회를 돌 필요를 없앤다.
		 * continue로 같은 버킷의 다음 후보를 계속 본다. */
		if (unlikely(!rq_mergeable(rq))) {
			__elv_rqhash_del(rq);
			continue;
		}

		/* [한국어] 해시 충돌을 걸러내는 최종 확인. rq_hash_key(rq)는 이
		 * request가 끝나는 섹터이고, offset은 새 bio가 시작하는 섹터다.
		 * 둘이 같다는 것은 두 영역이 정확히 맞닿는다는 뜻이므로 back merge
		 * 후보가 확정된다. 호출자 elv_merge()가 이어서 blk_try_merge()로
		 * 방향을 확정하고 실제 병합을 수행한다. */
		if (rq_hash_key(rq) == offset)
			return rq;
	}

	/* [한국어] 인접한 request가 없다 — 이 bio는 새 request가 되거나, 호출자가
	 * 스케줄러의 request_merge 콜백으로 front merge까지 찾아본다. */
	return NULL;
}

/*
 * RB-tree support functions for inserting/lookup/removal of requests
 * in a sorted RB tree.
 */
/*
 * [한국어]
 * elv_rb_add - request를 시작 LBA 기준으로 정렬된 RB-tree에 삽입
 *
 * @root: 삽입 대상 RB-tree 루트. 스케줄러가 자기 자료구조 안에 갖고 있다
 *        (mq-deadline의 dd_per_prio.sort_list[], BFQ의 bfq_queue.sort_list).
 * @rq:   삽입할 request. blk_rq_pos(rq)(시작 섹터)를 정렬 키로 쓴다.
 * @return: 없음
 *
 * === 왜 LBA로 정렬된 트리가 필요한가 ===
 * 해시(elv_rqhash_find)는 "정확히 맞닿는" back merge 후보만 O(1)에 찾을 수
 * 있다. 그런데 스케줄러는 두 가지를 더 해야 한다:
 *   1) front merge 후보 찾기 — "이 bio 바로 뒤에서 시작하는 request"를
 *      찾으려면 정렬된 구조에서 이분 탐색을 해야 한다.
 *   2) dispatch 순서 정하기 — 다음에 내보낼 request를 LBA 순으로 고르면
 *      장치 입장에서 접근이 순차적으로 보인다.
 * 이 두 용도를 위해 스케줄러는 해시와 별개로 RB-tree를 유지한다.
 *
 * === NVMe에서도 LBA 정렬이 의미가 있는가 ===
 * 회전 디스크만큼 극적이지는 않지만 여전히 이득이 있다. SSD 내부의 FTL은
 * 논리적으로 인접한 LBA를 같은 물리 블록/채널에 배치하는 경향이 있어,
 * 순차 접근이 읽기 증폭과 GC 부담을 줄인다. 또 LBA 순으로 나가면 블록 계층의
 * 병합 기회 자체가 늘어나 커맨드 수가 줄어든다.
 * 다만 이 이득이 스케줄러의 락 경합 비용보다 작기 때문에, 멀티큐 NVMe의
 * 기본값은 여전히 "none"이다(elevator_set_default 참고).
 *
 * === 중복 키 처리 ===
 * 같은 시작 LBA를 가진 request가 이미 있으면 오른쪽 서브트리로 내려간다.
 * 즉 중복을 허용하며, 같은 키끼리는 나중에 삽입된 것이 뒤에 온다. 두 요청이
 * 같은 LBA에서 시작하는 상황은 서로 다른 길이의 겹치는 I/O일 때 발생한다.
 *
 * 실행 컨텍스트: 스케줄러 락(dd->lock, bfqd->lock)을 쥔 상태.
 *
 * 호출 체인:
 *   dd_insert_request / bfq_add_request → [elv_rb_add]
 *     → rb_link_node → rb_insert_color(레드-블랙 균형 복원)
 */
void elv_rb_add(struct rb_root *root, struct request *rq)
{
	/* [한국어] p는 "새 노드를 매달 위치를 가리키는 포인터의 포인터"다.
	 * 이중 포인터를 쓰는 이유: 트리가 비었을 때는 root->rb_node에, 아니면
	 * 부모의 rb_left 또는 rb_right에 써야 하는데, 이중 포인터로 두면
	 * 세 경우를 분기 없이 동일하게 처리할 수 있다. */
	struct rb_node **p = &root->rb_node;
	/* [한국어] 새 노드의 부모가 될 노드. 루프가 끝나면 마지막으로 들른 노드가
	 * 담긴다. 트리가 비어 있으면 NULL로 남아 새 노드가 루트가 된다. */
	struct rb_node *parent = NULL;
	/* [한국어] 순회 중 비교 대상 request. 이름 앞의 밑줄은 인자 rq와
	 * 구분하기 위한 관례다. */
	struct request *__rq;

	/* [한국어] 루트에서 시작해 리프까지 내려가며 삽입 위치를 찾는다.
	 * *p가 NULL이 되는 지점이 빈 자리다. 평균 O(log n). */
	while (*p) {
		/* [한국어] 현재 노드를 부모 후보로 기억한다. */
		parent = *p;
		/* [한국어] rb_node 포인터에서 그것을 품고 있는 request 구조체 주소를
		 * 역산한다(container_of 매크로 기반). 커널의 내장형 자료구조 관용구로,
		 * 별도의 노드 객체를 할당하지 않아 메모리와 캐시 미스를 아낀다. */
		__rq = rb_entry(parent, struct request, rb_node);

		/* [한국어] 새 request의 시작 LBA가 더 작으면 왼쪽 서브트리로 내려간다.
		 * 그 결과 트리를 중위 순회하면 LBA 오름차순이 된다. */
		if (blk_rq_pos(rq) < blk_rq_pos(__rq))
			p = &(*p)->rb_left;
		/* [한국어] 크거나 같으면 오른쪽. >= 이므로 중복 키는 뒤쪽에 쌓인다.
		 * (else if로 쓰였지만 앞 조건의 부정과 같아 항상 참이다 — 명시적으로
		 *  조건을 적어 의도를 드러내는 스타일이다.) */
		else if (blk_rq_pos(rq) >= blk_rq_pos(__rq))
			p = &(*p)->rb_right;
	}

	/* [한국어] 찾은 빈 자리에 새 노드를 물리적으로 연결한다. 부모 포인터를
	 * 설정하고, *p에 새 노드 주소를 써 넣는다. 이 시점의 트리는 연결은
	 * 되었지만 레드-블랙 속성이 깨져 있을 수 있다. */
	rb_link_node(&rq->rb_node, parent, p);
	/* [한국어] 색칠과 회전으로 레드-블랙 속성을 복원해 트리 높이를 O(log n)로
	 * 유지한다. 이 두 단계(연결 → 균형)를 분리하는 것이 커널 RB-tree API의
	 * 표준 사용법이다. */
	rb_insert_color(&rq->rb_node, root);
}
EXPORT_SYMBOL(elv_rb_add);

/*
 * [한국어]
 * elv_rb_del - LBA 기준 RB-tree에서 request를 제거
 *
 * @root: 스케줄러 내부의 RB-tree 루트
 * @rq:   제거할 request
 *
 * dispatch된 request 또는 병합으로 사라지는 request를 스케줄러 큐에서 제거.
 * RB_EMPTY_NODE: 이미 제거된 노드를 다시 제거하면 커널 패닉이 일어나므로
 * BUG_ON으로 사전 방어한다. rb_erase 후 RB_CLEAR_NODE로 "트리 밖" 상태 명시.
 *
 * 호출 체인:
 *   스케줄러 ops.dispatch_request / ops.requests_merged → [elv_rb_del]
 */
void elv_rb_del(struct rb_root *root, struct request *rq)
{
/* 이미 RB-tree에서 제거된 노드인지 확인: 중복 제거는 트리 구조 파괴 */
	BUG_ON(RB_EMPTY_NODE(&rq->rb_node));
/* RB-tree에서 노드 제거 및 재균형(rebalance) */
	rb_erase(&rq->rb_node, root);
/* rb_node를 "빈 노드"로 초기화: 이후 ELV_ON_HASH 등의 검사와 일관성 유지 */
	RB_CLEAR_NODE(&rq->rb_node);
}
EXPORT_SYMBOL(elv_rb_del);

/*
 * [한국어]
 * elv_rb_find - 특정 @sector(LBA)로 시작하는 request를 RB-tree에서 탐색
 *
 * @root:   스케줄러 내부의 RB-tree 루트
 * @sector: 찾을 LBA(논리 블록 주소)
 * @return: 정확히 @sector에서 시작하는 request; 없으면 NULL
 *
 * BFQ·mq-deadline이 front-merge 후보(뒤쪽이 @sector인 request) 탐색,
 * 또는 dispatch 순서 결정에 O(log N) 탐색으로 사용한다.
 * 주의: back-merge 후보는 끝 섹터 기준 해시(elv_rqhash_find)로 찾는다.
 *
 * 호출 체인:
 *   스케줄러 ops.request_merge → [elv_rb_find]
 */
struct request *elv_rb_find(struct rb_root *root, sector_t sector)
{
/* RB-tree 루트에서 시작해 이진 탐색 */
	struct rb_node *n = root->rb_node;
	struct request *rq;

	while (n) {
/* 현재 노드의 request 포인터 복원 */
		rq = rb_entry(n, struct request, rb_node);

		if (sector < blk_rq_pos(rq))
/* 찾는 LBA가 더 작음 → 왼쪽 서브트리(더 작은 LBA들) 탐색 */
			n = n->rb_left;
		else if (sector > blk_rq_pos(rq))
/* 찾는 LBA가 더 큼 → 오른쪽 서브트리(더 큰 LBA들) 탐색 */
			n = n->rb_right;
		else
/* 정확히 일치: 이 request의 시작 LBA가 @sector */
			return rq;
	}

/* 트리에 없음: 해당 LBA에서 시작하는 기존 request 없음 */
	return NULL;
}
EXPORT_SYMBOL(elv_rb_find);

/*
 * elv_merge - 상위 bio가 기존 request와 병합할 수 있는지 결정
 *   호출 경로:
 *     blk_mq_submit_bio -> blk_mq_get_request -> elv_merge
 *   NVMe 연결:
 *     - blk_queue_nomerges 시 ELEVATOR_NO_MERGE를 반환하여 bio를 그대로
 *       별도 request로 전달; nvme_queue_rq()에서 개별 SQ 엔트리가 됨.
 *     - q->last_merge 캐시 히트 시 blk_try_merge()로 빠른 병합.
 *     - 해시/스케줄러 병합 실패 시 ELEVATOR_NO_MERGE 반환.
 *   반환: ELEVATOR_BACK_MERGE / ELEVATOR_FRONT_MERGE / ELEVATOR_DISCARD_MERGE
 */
enum elv_merge elv_merge(struct request_queue *q, struct request **req,
		struct bio *bio)
{
	struct elevator_queue *e = q->elevator;
	struct request *__rq;

	/*
	 * Levels of merges:
	 * 	nomerges:  No merges at all attempted
	 * 	noxmerges: Only simple one-hit cache try
	 * 	merges:	   All merge tries attempted
	 */
/* nomerges 또는 bio 병합 불가: NVMe에도 bio별 request로 전달 */
	if (blk_queue_nomerges(q) || !bio_mergeable(bio))
		return ELEVATOR_NO_MERGE;

	/*
	 * First try one-hit cache.
	 */
	if (q->last_merge && elv_bio_merge_ok(q->last_merge, bio)) {
		/* [한국어] one-hit 캐시 적중. q->last_merge는 "가장 최근에 병합이
		 * 성공한 request" 하나만 기억하는 극단적으로 단순한 캐시다.
		 * 이것만으로도 효과가 큰 이유: 순차 I/O에서는 같은 request에 연달아
		 * 붙는 경우가 압도적이라, 해시 조회조차 하지 않고 포인터 비교 한 번으로
		 * 끝나는 경우가 대부분이다. 실패하면 아래 해시 조회로 넘어간다. */
		enum elv_merge ret = blk_try_merge(q->last_merge, bio);

		if (ret != ELEVATOR_NO_MERGE) {
			*req = q->last_merge;
			return ret;
		}
	}

	if (blk_queue_noxmerges(q))
/* noxmerges: 단순 캐시만 시도; NVMe random 워크로드에서 오버헤드 감소 */
		return ELEVATOR_NO_MERGE;

	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	__rq = elv_rqhash_find(q, bio->bi_iter.bi_sector);
/* LBA 연속 후보 탐색: 해시에서 back-merge 대상을 찾음 */
	if (__rq && elv_bio_merge_ok(__rq, bio)) {
		*req = __rq;

		if (blk_discard_mergable(__rq))
/* discard 병합: NVMe Deallocate/Trim SQ 엔트리 수 감소 */
			return ELEVATOR_DISCARD_MERGE;
		return ELEVATOR_BACK_MERGE;
	}

	if (e->type->ops.request_merge)
/* 스케줄러별 request_merge: 해시 실패 후 폴백(fallback) (e.g. bfq front-merge) */
		return e->type->ops.request_merge(q, req, bio);

	return ELEVATOR_NO_MERGE;
}

/*
 * Attempt to do an insertion back merge. Only check for the case where
 * we can append 'rq' to an existing request, so we can throw 'rq' away
 * afterwards.
 *
 * Returns true if we merged, false otherwise. 'free' will contain all
 * requests that need to be freed.
 */
/*
 * elv_attempt_insert_merge - 새 request를 기존 request 뒤에 붙여 제거
 *   호출 경로: blk_mq_get_request -> ... -> elv_attempt_insert_merge
 *   NVMe 연결: bio를 신규 request에 할당한 직후, 기존 request에 back-merge해
 *              free 목록에 넣고, 최종적으로 nvme_queue_rq()가 처리할
 *              request 수를 줄인다. 연속 병합이 여러 번 일어날 수 있음.
 */
bool elv_attempt_insert_merge(struct request_queue *q, struct request *rq,
			      struct list_head *free)
{
	/* [한국어] 해시에서 찾아낸 병합 상대. 루프마다 새로 갱신된다. */
	struct request *__rq;
	/* [한국어] 한 번이라도 병합이 성공했는지. 호출자는 이 값으로 rq가
	 * 살아 있는지(false) 소멸했는지(true) 판단한다. */
	bool ret;

	/* [한국어] 관리자가 /sys/block/<dev>/queue/nomerges에 2를 써서 병합을 전면
	 * 금지한 경우. 병합 판정 자체가 CPU를 쓰므로, 랜덤 워크로드처럼 병합이
	 * 절대 성공하지 않는 상황에서는 끄는 편이 빠르다. */
	if (blk_queue_nomerges(q))
		return false;

	/*
	 * First try one-hit cache.
	 */
	/* [한국어] 가장 최근에 병합에 성공한 request와 먼저 시도한다. 순차 I/O에서는
	 * 이 한 번의 시도로 끝나는 경우가 대부분이라, 해시 조회 비용조차 아낀다.
	 * 성공하면 rq는 last_merge에 흡수되어 사라지므로 free 목록에 넣는다. */
	if (q->last_merge && blk_attempt_req_merge(q, q->last_merge, rq)) {
		/* [한국어] 소멸한 rq를 호출자가 나중에 해제하도록 free 리스트에 넣는다.
		 * 여기서 직접 해제하지 않는 이유는 호출자가 스케줄러 락을 쥔 상태라,
		 * 락을 푼 뒤에 해제해야 락 순서 역전을 피할 수 있기 때문이다. */
		list_add(&rq->queuelist, free);
		return true;
	}

	/* [한국어] nomerges에 1을 쓰면 noxmerges — "단순 캐시만 시도하고 복잡한
	 * 탐색은 하지 말라"는 중간 설정이다. 캐시는 이미 위에서 시도했으므로
	 * 여기서 종료한다. 해시 조회와 반복 병합의 CPU 비용을 아끼면서 가장
	 * 쉬운 병합 기회는 놓치지 않는 절충안이다. */
	if (blk_queue_noxmerges(q))
		return false;

	ret = false;
	/*
	 * See if our hash lookup can find a potential backmerge.
	 */
	/* [한국어] ★ 연쇄 병합 루프 ★
	 * 한 번 병합에 성공하면 결과 request가 더 커지고, 그 커진 request가
	 * 또 다른 이웃과 맞닿을 수 있다. 예를 들어 [200~300]이 들어왔을 때
	 * [100~200]에 흡수되어 [100~300]이 되면, 이제 [300~400]과도 인접해진다.
	 * 더 이상 병합할 것이 없을 때까지 반복해 최대한 큰 request를 만든다.
	 * NVMe 관점에서 이 루프가 도는 횟수만큼 SQ 엔트리와 Command ID가 절약된다. */
	while (1) {
		/* [한국어] 현재 rq의 "시작 섹터"로 해시를 조회한다. 해시는 request의
		 * "끝 섹터"를 키로 하므로, 이 조회는 "rq 바로 앞에서 끝나는 request"를
		 * 찾는 것이다. 즉 rq를 그 뒤에 붙이는 back merge 후보를 찾는다. */
		__rq = elv_rqhash_find(q, blk_rq_pos(rq));
		/* [한국어] 후보가 없거나 실제 병합에 실패하면(하드웨어 한계 등) 종료한다.
		 * blk_attempt_req_merge()가 blk-merge.c의 attempt_merge()를 불러
		 * 세그먼트 수·크기·virt boundary를 모두 검사한다. */
		if (!__rq || !blk_attempt_req_merge(q, __rq, rq))
			break;

		/* [한국어] rq가 __rq에 흡수되어 소멸했다. 해제 목록에 넣는다. */
		list_add(&rq->queuelist, free);
		/* The merged request could be merged with others, try again */
		/* [한국어] 최소 한 번 병합했음을 기록. 이후 루프에서 실패해도 이
		 * 값은 유지되어 호출자에게 "원래 rq는 사라졌다"고 알린다. */
		ret = true;
		/* [한국어] 이제 커진 __rq를 새로운 기준으로 삼아 다시 시도한다.
		 * 다음 반복에서는 __rq의 시작 섹터로 조회하므로, 더 앞쪽으로
		 * 이어지는 request를 찾게 된다. */
		rq = __rq;
	}

	/* [한국어] true = 원래 rq는 병합되어 사라졌다(free 목록에 있음).
	 * false = 병합 실패, rq는 그대로 살아 있으니 호출자가 큐에 삽입해야 한다. */
	return ret;
}

/*
 * [한국어]
 * elv_merged_request - bio 병합 직후 스케줄러 자료구조와 캐시를 일관되게 갱신
 *
 * @q:    병합이 일어난 request_queue
 * @rq:   bio를 흡수해 커진 request
 * @type: 병합 방향(ELEVATOR_BACK_MERGE / ELEVATOR_FRONT_MERGE 등)
 * @return: 없음
 *
 * request의 위치나 크기가 바뀌면 그것을 색인하고 있던 세 자료구조가 모두
 * 낡은 상태가 된다. 이 함수가 그 셋을 한꺼번에 맞춘다:
 *   1) 스케줄러 내부 색인 — request_merged 콜백(mq-deadline이면 정렬 rb-tree의
 *      키가 바뀌었으므로 재삽입)
 *   2) 병합 후보 해시 — back merge면 끝 섹터가 바뀌었으므로 재배치
 *   3) one-hit 캐시 — 방금 병합에 성공한 request를 캐시에 올려 다음 시도를 가속
 *
 * === 왜 back merge에서만 해시를 재배치하는가 ===
 * 해시 키는 rq_hash_key(rq) = 시작 섹터 + 길이 = "끝 섹터"다.
 *   back merge  : 뒤에 붙였으므로 끝 섹터가 바뀐다 → 재배치 필요
 *   front merge : 앞에 붙였으므로 시작 섹터와 길이가 함께 바뀌지만, 끝 섹터는
 *                 그대로다 → 해시 키가 변하지 않아 재배치 불필요
 * 이 비대칭은 "끝 섹터를 키로 삼은" 설계에서 자연스럽게 따라 나온다.
 *
 * 실행 컨텍스트: 스케줄러 락을 쥔 상태(호출자가 이미 잡고 있다).
 *
 * 호출 체인:
 *   blk_mq_sched_try_merge → [elv_merged_request]
 *     → ops.request_merged (dd_request_merged / bfq_request_merged)
 *     → elv_rqhash_reposition
 */
void elv_merged_request(struct request_queue *q, struct request *rq,
		enum elv_merge type)
{
	/* [한국어] 이 큐의 스케줄러 인스턴스. 콜백 테이블 접근용. */
	struct elevator_queue *e = q->elevator;

	/* [한국어] 스케줄러에게 "이 request가 커졌다"고 알린다. 콜백은 선택 사항이며,
	 * mq-deadline은 dd_request_merged()에서 정렬 rb-tree에 다시 넣는다
	 * (시작 LBA가 바뀌는 front merge에서 특히 중요하다). 콜백이 없는
	 * 스케줄러는 위치 색인을 유지하지 않으므로 할 일이 없다. */
	if (e->type->ops.request_merged)
		e->type->ops.request_merged(q, rq, type);

	/* [한국어] back merge라 끝 섹터가 바뀌었다 — 병합 후보 해시에서 뺐다가
	 * 새 키로 다시 넣는다. 이걸 빠뜨리면 이후 조회가 옛 끝 섹터로 이 request를
	 * 찾아내, 실제로는 인접하지 않은 bio를 붙이려 시도하게 된다. */
	if (type == ELEVATOR_BACK_MERGE)
		elv_rqhash_reposition(q, rq);

/* last_merge 캐시 갱신 */
	q->last_merge = rq;
}

/*
 * [한국어] (아래 상세 블록 참고) 두 request 병합을 스케줄러에 통지하는 진입점.
 * 병합 결과 살아남은 request 하나가 nvme_setup_rw()에서 SLBA/NLB가 확장된
 * 커맨드 하나로 변환되므로, SQ 엔트리·Command ID·완료 CQ 엔트리가 각각
 * 하나씩 절약된다.
 */
/*
 * [한국어]
 * elv_merge_requests - request 두 개가 병합됐음을 스케줄러에 알리고 자료구조 갱신
 *
 * @q:    병합이 일어난 request_queue
 * @rq:   병합 결과로 남을 request (next를 흡수한 쪽)
 * @next: 병합되어 사라질 request (rq에 흡수됨)
 *
 * blk_attempt_req_merge()가 두 request를 하나로 합친 직후에 호출된다.
 * 스케줄러별 requests_merged 콜백으로 내부 큐 상태(예: BFQ의 두 rq 간
 * 연결)를 갱신하고, rq의 끝 섹터가 바뀌었으므로 해시를 재배치하여
 * 이후 back-merge 탐색이 올바른 위치에서 일어나도록 한다.
 *
 * 호출 체인:
 *   blk_attempt_req_merge → [elv_merge_requests]
 */
void elv_merge_requests(struct request_queue *q, struct request *rq,
			     struct request *next)
{
/* elevator_queue: 현재 활성 스케줄러 ops 접근 */
	struct elevator_queue *e = q->elevator;

/* requests_merged: BFQ·mq-deadline 등이 두 rq 간 내부 연결을 정리하는 콜백 */
	if (e->type->ops.requests_merged)
		e->type->ops.requests_merged(q, rq, next);

/* rq가 next를 흡수했으므로 끝 섹터가 바뀜 → 해시 키 재계산 후 재배치 */
	elv_rqhash_reposition(q, rq);
/* last_merge 캐시: 다음 one-hit 캐시 시도에서 이 rq가 first candidate */
	q->last_merge = rq;
}

/*
 * [한국어]
 * elv_latter_request - 스케줄러 dispatch 순서에서 @rq 다음에 올 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 선택한 다음 request; 없으면 NULL
 *
 * 스케줄러 내부 자료구조(BFQ의 B-WF2Q 큐, mq-deadline의 fifo/RB-tree 등)에서
 * @rq 바로 뒤에 위치한 요청을 찾는다. front-merge 시 merge 체인을 따라갈
 * 때 사용된다.
 *
 * 호출 체인:
 *   blk_try_req_merge → [elv_latter_request] → 스케줄러 ops.next_request
 */
struct request *elv_latter_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* 스케줄러별 next_request 콜백: BFQ/mq-deadline 내부 dispatch 순서에서 다음 후보 */
	if (e->type->ops.next_request)
		return e->type->ops.next_request(q, rq);

	return NULL;
}

/*
 * [한국어]
 * elv_former_request - 스케줄러 dispatch 순서에서 @rq 이전에 올 request 반환
 *
 * @q:  request_queue
 * @rq: 기준 request
 * @return: 스케줄러가 선택한 이전 request; 없으면 NULL
 *
 * @rq 앞의 요청을 찾아 front-merge 후보를 탐색하거나 dispatch 순서를
 * 역방향으로 추적할 때 사용된다.
 *
 * 호출 체인:
 *   blk_try_req_merge → [elv_former_request] → 스케줄러 ops.former_request
 */
struct request *elv_former_request(struct request_queue *q, struct request *rq)
{
	struct elevator_queue *e = q->elevator;

/* 스케줄러별 former_request 콜백: dispatch 순서에서 @rq 앞의 request 반환 */
	if (e->type->ops.former_request)
		return e->type->ops.former_request(q, rq);

	return NULL;
}

#define to_elv(atr) container_of_const((atr), struct elv_fs_entry, attr)

/*
 * [한국어]
 * elv_attr_show - /sys/block/<disk>/queue/iosched/<attr> 읽기 핸들러
 *
 * @kobj: elevator_queue에 내장된 kobject (iosched sysfs 노드)
 * @attr: 읽을 sysfs attribute (elv_fs_entry로 캐스팅)
 * @page: 출력 버퍼 (PAGE_SIZE 크기)
 * @return: 쓴 바이트 수 또는 에러 코드
 *
 * sysfs kobject ops.show로 등록되어 사용자가 scheduler 파라미터를 읽을 때
 * 호출된다. sysfs_lock을 잡아 elevator 구조체와의 race를 막고,
 * ELEVATOR_FLAG_DYING이면(장치 제거 중) -ENODEV를 반환해 dangling 접근을 차단.
 *
 * 호출 체인:
 *   sysfs read → elv_sysfs_ops.show → [elv_attr_show] → entry->show(e, page)
 */
static ssize_t
elv_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
/* attr을 elv_fs_entry로 캐스팅: elevator 스케줄러별 sysfs 파일 설명자 */
	const struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
/* -ENODEV: DYING 상태이거나 show 콜백 없는 경우의 기본 에러 */
	ssize_t error = -ENODEV;

/* show 콜백 없으면 읽기 불가 */
	if (!entry->show)
		return -EIO;

/* kobj로부터 상위 elevator_queue 복원 */
	e = container_of(kobj, struct elevator_queue, kobj);
/* sysfs_lock: elevator 구조체와 elevator_exit() 간 동시 접근 보호 */
	mutex_lock(&e->sysfs_lock);
/* ELEVATOR_FLAG_DYING: 장치 제거/elevator 교체 중 → 파라미터 접근 차단 */
	if (!test_bit(ELEVATOR_FLAG_DYING, &e->flags))
		error = entry->show(e, page);
	mutex_unlock(&e->sysfs_lock);
	return error;
}

/*
 * [한국어]
 * elv_attr_store - /sys/block/<disk>/queue/iosched/<attr> 쓰기 핸들러
 *
 * @kobj:   elevator_queue에 내장된 kobject
 * @attr:   쓸 sysfs attribute
 * @page:   사용자 입력 버퍼
 * @length: 입력 길이
 * @return: 소비한 바이트 수 또는 에러 코드
 *
 * 사용자가 scheduler 파라미터(예: mq-deadline의 read_expire, write_expire)를
 * 변경할 때 호출된다. sysfs_lock으로 elevator_exit()와의 race를 막고,
 * DYING 상태이면 변경을 거부한다.
 *
 * 호출 체인:
 *   sysfs write → elv_sysfs_ops.store → [elv_attr_store] → entry->store(e, page, length)
 */
static ssize_t
elv_attr_store(struct kobject *kobj, struct attribute *attr,
	       const char *page, size_t length)
{
/* attr을 elv_fs_entry로 캐스팅 */
	const struct elv_fs_entry *entry = to_elv(attr);
	struct elevator_queue *e;
/* -ENODEV: DYING 상태 기본값 */
	ssize_t error = -ENODEV;

/* store 콜백 없으면 쓰기 불가 */
	if (!entry->store)
		return -EIO;

/* kobj에서 elevator_queue 복원 */
	e = container_of(kobj, struct elevator_queue, kobj);
/* sysfs_lock: elevator_exit()와의 race 방지 — 파라미터 변경 중 elevator 해제 금지 */
	mutex_lock(&e->sysfs_lock);
/* DYING 상태(장치 제거 중)에서는 파라미터 쓰기 차단 */
	if (!test_bit(ELEVATOR_FLAG_DYING, &e->flags))
		error = entry->store(e, page, length);
	mutex_unlock(&e->sysfs_lock);
	return error;
}

static const struct sysfs_ops elv_sysfs_ops = {
	.show	= elv_attr_show,
	.store	= elv_attr_store,
};

static const struct kobj_type elv_ktype = {
	.sysfs_ops	= &elv_sysfs_ops,
	.release	= elevator_release,
};

/*
 * [한국어]
 * elv_register_queue - 스케줄러를 sysfs/debugfs에 노출해 사용자에게 공개
 *
 * @q:      스케줄러가 붙은 request_queue
 * @e:      노출할 스케줄러 인스턴스
 * @uevent: udev에 KOBJ_ADD 이벤트를 보낼지 여부. 부팅 초기의 기본 스케줄러
 *          설정처럼 udev가 반응할 필요가 없는 경우에는 false를 넘긴다.
 * @return: 0 성공, 음수 errno 실패
 *
 * 스케줄러를 큐에 붙이는 실제 동작(elevator_switch)이 끝난 뒤, 그것을 사용자
 * 공간에서 보고 조정할 수 있게 만드는 마지막 단계다. 세 가지를 만든다:
 *   1) /sys/block/nvme0n1/queue/iosched/ 디렉터리
 *   2) 그 안의 튜너블 파일들 (mq-deadline이면 read_expire, write_expire,
 *      fifo_batch, writes_starved 등)
 *   3) debugfs의 스케줄러 상태 덤프
 *
 * ELEVATOR_FLAG_REGISTERED를 마지막에 세우는 순서가 중요하다. 이 비트가
 * 서 있어야 elv_unregister_queue()가 실제로 정리 작업을 수행하므로,
 * 등록이 완료된 뒤에 세워야 부분적으로 등록된 상태를 해제하려다 꼬이는 일이 없다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. sysfs 조작은 잠들 수 있다.
 * q->elevator_lock을 쥔 상태에서 호출된다.
 *
 * 에러 경로: kobject_add()가 실패하면 그대로 반환하고, 호출자
 * elevator_change_done()이 스케줄러를 해제한다. 개별 튜너블 파일 생성 실패는
 * 치명적이지 않다고 보고 루프만 중단한다(아래 참고).
 *
 * 호출 체인:
 *   elevator_change → elevator_change_done → [elv_register_queue]
 *     → kobject_add → sysfs_create_file → blk_mq_sched_reg_debugfs
 */
static int elv_register_queue(struct request_queue *q,
			      struct elevator_queue *e,
			      bool uevent)
{
	int error;

	/* [한국어] 큐의 sysfs 디렉터리(/sys/block/nvme0n1/queue/) 아래에 "iosched"
	 * 라는 이름으로 kobject를 등록한다. 이 호출이 성공해야 디렉터리가 생긴다. */
	error = kobject_add(&e->kobj, &q->disk->queue_kobj, "iosched");
	if (!error) {
		/* [한국어] 스케줄러가 노출할 튜너블 목록. NULL로 끝나는 배열이며,
		 * 스케줄러마다 다르다(none은 아예 없고, mq-deadline은 5~6개). */
		const struct elv_fs_entry *attr = e->type->elevator_attrs;
		if (attr) {
			/* [한국어] 배열 끝(name == NULL)까지 순회하며 파일을 만든다. */
			while (attr->attr.name) {
				/* [한국어] 파일 생성에 실패하면 루프를 중단하되 함수 전체는
				 * 성공으로 처리한다. 튜너블 하나가 없어도 스케줄러 자체는
				 * 정상 동작하므로, 여기서 실패시켜 I/O를 못 하게 만드는 것보다
				 * 일부 튜너블 없이 진행하는 편이 낫다는 판단이다. */
				if (sysfs_create_file(&e->kobj, &attr->attr))
					break;
				attr++;
			}
		}
		/* [한국어] udev에 "새 kobject가 생겼다"고 알린다. udev 규칙이 스케줄러
		 * 변경에 반응해 추가 튜닝을 하도록 만들 수 있다. 부팅 시 기본 스케줄러
		 * 설정(elevator_set_default)은 no_uevent=true로 이를 건너뛴다 —
		 * 아직 udev가 준비되지 않았고 불필요한 이벤트 폭주를 막기 위해서다. */
		if (uevent)
			kobject_uevent(&e->kobj, KOBJ_ADD);

		/*
		 * Sched is initialized, it is ready to export it via
		 * debugfs
		 */
		/* [한국어] debugfs에 스케줄러 상태를 노출한다.
		 * /sys/kernel/debug/block/nvme0n1/sched/ 아래에 dispatch 큐 내용,
		 * 각 우선순위별 FIFO 상태 등이 덤프되어, NVMe I/O가 스케줄러에서
		 * 얼마나 대기하는지 직접 관찰할 수 있다. */
		blk_mq_sched_reg_debugfs(q);
		/* [한국어] 등록 완료 표시. 이 비트를 마지막에 세우는 이유는 위
		 * 함수 주석 참고 — 해제 경로가 이 비트를 보고 동작하기 때문이다. */
		set_bit(ELEVATOR_FLAG_REGISTERED, &e->flags);
	}
	/* [한국어] kobject_add의 결과를 그대로 반환. 실패하면 호출자가 롤백한다. */
	return error;
}

/*
 * [한국어]
 * elv_unregister_queue - request_queue의 elevator sysfs/debugfs 노드 제거
 *
 * @q: elevator가 연결된 request_queue
 * @e: 제거할 elevator_queue; NULL이면 no-op
 *
 * ELEVATOR_FLAG_REGISTERED를 원자적으로 클리어하고(test_and_clear_bit), sysfs의
 * iosched kobject를 제거한다. 이 시점부터 /sys/block/<disk>/queue/iosched는
 * 접근 불가하다. elevator_exit() 또는 elevator_change_done() 안에서 호출된다.
 *
 * 호출 체인:
 *   elevator_change_done / elv_exit_and_release → [elv_unregister_queue]
 */
static void elv_unregister_queue(struct request_queue *q,
				 struct elevator_queue *e)
{
/* REGISTERED 비트를 원자적으로 클리어; 이미 클리어됐으면(중복 호출) no-op */
	if (e && test_and_clear_bit(ELEVATOR_FLAG_REGISTERED, &e->flags)) {
/* KOBJ_REMOVE uevent: udev 등이 /sys/block/.../queue/iosched 제거를 인지 */
		kobject_uevent(&e->kobj, KOBJ_REMOVE);
/* sysfs kobject 제거: /sys/block/<disk>/queue/iosched 디렉토리 삭제 */
		kobject_del(&e->kobj);

		/* unexport via debugfs before exiting sched */
/* debugfs 노출 해제: blk-mq sched 디버그 정보 제거 */
		blk_mq_sched_unreg_debugfs(q);
	}
}

/*
 * struct elevator_type 주요 필드 (NVMe 관점)
 *   elevator_name/alias : "mq-deadline", "bfq", "none" 등 사용자가 sysfs에서 선택하는 이름.
 *   ops                 : insert_requests, dispatch_request, allow_merge 등 콜백.
 *                         NVMe request_queue는 이 콜백을 통해 bio를 SQ에 제출하기 전
 *                         정렬/병합/디스패치를 수행한다.
 *   icq_size/icq_align  : io_cq(ICQ) 캐시 크기/정렬; cgroup 기반 스케줄링 상태.
 *   elevator_attrs      : /sys/block/<disk>/queue/iosched 아래의 tunable.
 *   icq_cache           : per-cgroup IO context 캐시.
 *   list                : elv_list 연결 리스트 엔트리.
 *
 * elv_register - 새 IO 스케줄러를 전역 elv_list에 등록
 *   호출 경로: 스케줄러 모듈 initcall -> elv_register
 *   NVMe 연결: mq-deadline, bfq, kyber 등이 등록되며,
 *              NVMe 장치 초기화 시 elevator_set_default()에서 이 리스트를
 *              조회해 적합한 스케줄러를 선택한다.
 */
int elv_register(struct elevator_type *e)
{
	/* finish request is mandatory */
	/* [한국어] 필수 콜백 검사 1 — finish_request.
	 * request가 완료될 때(성공이든 실패든) 스케줄러가 내부 상태를 정리할
	 * 기회다. 이것이 없으면 스케줄러가 자기가 발행한 request를 회수하지 못해
	 * 자료구조가 계속 부풀어 오른다. WARN_ON_ONCE로 개발자에게 스택 트레이스를
	 * 남기는 이유: 이것은 사용자 오류가 아니라 스케줄러 구현 버그이므로
	 * 조용히 거부하기보다 명확히 알려야 한다. */
	if (WARN_ON_ONCE(!e->ops.finish_request))
		return -EINVAL;
	/* insert_requests and dispatch_request are mandatory */
	/* [한국어] 필수 콜백 검사 2 — 요청의 입구와 출구.
	 *   insert_requests  : blk-mq가 request를 스케줄러에 맡기는 경로
	 *   dispatch_request : 스케줄러가 다음에 내보낼 request를 고르는 경로
	 * 이 둘이 스케줄러 존재 이유 자체이므로 없으면 등록할 수 없다.
	 * NVMe 관점에서 dispatch_request가 고른 request가 곧 다음 SQ 엔트리가 된다. */
	if (WARN_ON_ONCE(!e->ops.insert_requests || !e->ops.dispatch_request))
		return -EINVAL;

	/* create icq_cache if requested */
	/* [한국어] icq(io_cq, I/O Context Queue)를 쓰는 스케줄러라면 전용 slab
	 * 캐시를 만든다. icq는 "프로세스 × 큐" 조합마다 하나씩 만들어지는 객체로,
	 * 그 프로세스가 이 장치에 대해 갖는 스케줄링 상태(BFQ의 가중치, 대기
	 * 이력 등)를 담는다. BFQ만 이것을 쓰고 mq-deadline/kyber는 쓰지 않는다.
	 * 자주 할당·해제되는 고정 크기 객체라 전용 slab 캐시가 효율적이다. */
	if (e->icq_size) {
		/* [한국어] icq는 struct io_cq를 첫 멤버로 품어야 한다(상속 관용구).
		 * 크기나 정렬이 그보다 작으면 공통 코드가 구조체 밖을 침범한다. */
		if (WARN_ON(e->icq_size < sizeof(struct io_cq)) ||
		    WARN_ON(e->icq_align < __alignof__(struct io_cq)))
			return -EINVAL;

		/* [한국어] "bfq_io_cq" 같은 캐시 이름을 만든다. /proc/slabinfo에
		 * 이 이름으로 나타나 메모리 사용량을 추적할 수 있다. */
		snprintf(e->icq_cache_name, sizeof(e->icq_cache_name),
			 "%s_io_cq", e->elevator_name);
		/* [한국어] slab 캐시 생성. 생성자(마지막 인자 NULL)를 두지 않으므로
		 * 할당 시점에 스케줄러가 직접 초기화한다. */
		e->icq_cache = kmem_cache_create(e->icq_cache_name, e->icq_size,
						 e->icq_align, 0, NULL);
		if (!e->icq_cache)
			return -ENOMEM;
	}

	/* register, don't allow duplicate names */
	/* [한국어] 전역 목록을 보호하며 등록한다. 검사와 삽입이 같은 락 안에 있어야
	 * 두 모듈이 동시에 같은 이름으로 등록하는 경쟁을 막을 수 있다. */
	spin_lock(&elv_list_lock);
	/* [한국어] 이름 중복 검사. 같은 이름이 둘이면 elevator_find_get()이
	 * 어느 것을 돌려줄지 불확정해지고, 사용자가 sysfs로 고른 스케줄러가
	 * 의도한 것이 아닐 수 있다. */
	if (__elevator_find(e->elevator_name)) {
		spin_unlock(&elv_list_lock);
		/* [한국어] 위에서 만든 slab 캐시를 되돌린다. icq_size가 0이었다면
		 * icq_cache는 NULL이고, kmem_cache_destroy(NULL)은 안전한 no-op다. */
		kmem_cache_destroy(e->icq_cache);
		return -EBUSY;
	}
	/* [한국어] 목록 끝에 추가한다. 이 순간부터 다른 CPU가 이 스케줄러를
	 * 조회하고 큐에 붙일 수 있다. tail에 넣는 이유는 등록 순서를 유지해
	 * /sys/.../queue/scheduler 출력이 안정적으로 보이게 하기 위해서다. */
	list_add_tail(&e->list, &elv_list);
	spin_unlock(&elv_list_lock);

	/* [한국어] dmesg에 등록 사실을 남긴다. 부팅 로그에서 어떤 스케줄러가
	 * 사용 가능한지 확인할 수 있다. */
	printk(KERN_INFO "io scheduler %s registered\n", e->elevator_name);

	return 0;
}
EXPORT_SYMBOL_GPL(elv_register);

/*
 * elv_unregister - IO 스케줄러 모듈 제거
 *   호출 경로: 모듈 exit -> elv_unregister
 *   NVMe 연결: 스케줄러 모듈이 제거되면 NVMe 장치는 더 이상 해당 elevator를
 *              사용할 수 없으므로, 등록 해제 전 rcu_barrier()로 진행 중인
 *              nvme_queue_rq() 경로의 참조가 완료되도록 보장.
 */
void elv_unregister(struct elevator_type *e)
{
	/* unregister */
/* elv_list에서 제거: 이후 NVMe 장치는 해당 스케줄러를 찾을 수 없음 */
	spin_lock(&elv_list_lock);
	list_del_init(&e->list);
	spin_unlock(&elv_list_lock);

	/*
	 * Destroy icq_cache if it exists.  icq's are RCU managed.  Make
	 * sure all RCU operations are complete before proceeding.
	 */
	if (e->icq_cache) {
/* rcu_barrier: NVMe 완료 경로의 RCU readers 종료 대기 */
		rcu_barrier();
		kmem_cache_destroy(e->icq_cache);
		e->icq_cache = NULL;
	}
}
EXPORT_SYMBOL_GPL(elv_unregister);

/*
 * Switch to new_e io scheduler.
 *
 * If switching fails, we are most likely running out of memory and not able
 * to restore the old io scheduler, so leaving the io scheduler being none.
 */
/*
 * elevator_switch - request_queue의 IO 스케줄러를 @ctx->name으로 교체
 *   호출 경로: elevator_change -> elevator_switch
 *           -> blk_mq_quiesce_queue / blk_mq_init_sched
 *   NVMe 연결: NVMe 컨트롤러의 queue_depth/hw_queue 변화 또는 사용자 sysfs
 *              변경 시 queue를 freeze/quiesce하고, 기존 elevator를 내린 뒤
 *              새 elevator를 연결. 이 동안 nvme_queue_rq()는 중단된다.
 */
static int elevator_switch(struct request_queue *q, struct elv_change_ctx *ctx)
{
	/* [한국어] 새로 붙일 스케줄러 타입. "none"으로 전환하는 경우 NULL로 남는다. */
	struct elevator_type *new_e = NULL;
	int ret = 0;

	/* [한국어] 큐가 freeze된 상태여야 한다는 불변식 검사. freeze는 "새 요청
	 * 진입을 막고 진행 중인 요청이 모두 끝나기를 기다린" 상태를 뜻한다.
	 * 스케줄러를 바꾸는 동안 그것을 참조하는 request가 살아 있으면 해제된
	 * 자료구조를 건드리게 되므로, 호출자가 반드시 먼저 freeze해야 한다. */
	WARN_ON_ONCE(q->mq_freeze_depth == 0);
	/* [한국어] 스케줄러 교체 직렬화 락도 쥐고 있어야 한다. freeze는 I/O를
	 * 막을 뿐, 두 스레드가 동시에 스케줄러를 바꾸는 것은 막지 못한다. */
	lockdep_assert_held(&q->elevator_lock);

	/* [한국어] "none"이 아니면 실제 스케줄러 모듈을 찾아 참조를 획득한다.
	 * strncmp의 반환값이 0이 아닐 때(= 이름이 "none"이 아닐 때) 진입한다.
	 * "none"은 등록 목록에 없는 특별한 이름으로, "스케줄러를 붙이지 않음"을
	 * 뜻한다. NVMe 멀티큐의 기본 상태가 바로 이것이다. */
	if (strncmp(ctx->name, "none", 4)) {
		new_e = elevator_find_get(ctx->name);
		/* [한국어] 없는 스케줄러 이름 — 사용자에게 -EINVAL을 돌려준다.
		 * 아직 아무것도 바꾸지 않았으므로 롤백할 것이 없다. */
		if (!new_e)
			return -EINVAL;
	}

	/* [한국어] freeze에 더해 quiesce까지 건다. 둘의 차이가 중요하다:
	 *   freeze  - 새 요청이 큐에 "들어오는 것"을 막는다.
	 *   quiesce - 이미 큐에 있는 요청이 드라이버로 "나가는 것"을 막는다.
	 * 스케줄러 교체는 dispatch 경로가 완전히 멈춰야 안전하므로 둘 다 필요하다.
	 * 내부적으로 SRCU 유예 기간을 기다려 실행 중인 dispatch가 끝나게 한다. */
	blk_mq_quiesce_queue(q);

	if (q->elevator) {
		/* [한국어] 기존 스케줄러 포인터를 ctx에 보관한다. 여기서 바로 해제하지
		 * 않는 이유는, 해제에 sysfs 조작이 포함되어 있어 지금 쥐고 있는 락
		 * 아래에서 하면 락 순서 역전이 발생하기 때문이다. 호출자
		 * elevator_change_done()이 락을 푼 뒤 정리한다. */
		ctx->old = q->elevator;
		/* [한국어] 스케줄러의 exit_sched 콜백을 부르고 q->elevator를 떼어낸다.
		 * 이 시점 이후 큐는 잠시 스케줄러가 없는 상태가 된다. */
		elevator_exit(q);
	}

	if (new_e) {
		/* [한국어] 새 스케줄러를 초기화해 큐에 연결한다. 내부에서 스케줄러
		 * 전용 태그 세트(sched_tags)를 할당하는데, 이것이 드라이버 태그와
		 * 별개인 이유는 스케줄러가 "장치에 보낼 수 있는 것보다 많은" 요청을
		 * 큐에 담아야 재정렬할 여지가 생기기 때문이다. 보통 드라이버 태그
		 * 개수의 2배를 잡는다. NVMe에서 이는 CID 공간과는 별개의 논리적
		 * 슬롯이며, 실제 CID는 dispatch 시점에 따로 획득한다. */
		ret = blk_mq_init_sched(q, new_e, &ctx->res);
		/* [한국어] 초기화 실패(대개 메모리 부족) — 큐는 스케줄러 없는 상태로
		 * 남는다. 위 영문 주석이 밝히듯 옛 스케줄러를 되살리는 것도 메모리를
		 * 요구하므로, 그냥 "none"으로 두는 것이 가장 안전한 폴백이다. */
		if (ret)
			goto out_unfreeze;
		/* [한국어] 새 인스턴스를 ctx에 기록. 호출자가 나중에 sysfs 등록을 한다. */
		ctx->new = q->elevator;
	} else {
		/* [한국어] "none"으로 전환하는 경로.
		 * QUEUE_FLAG_SQ_SCHED는 "단일 큐 스케줄러가 붙어 있다"는 표시로,
		 * dispatch 경로가 스케줄러를 경유할지 결정한다. 해제하면 이후
		 * request가 소프트웨어 큐에서 하드웨어 큐로 직행한다. */
		blk_queue_flag_clear(QUEUE_FLAG_SQ_SCHED, q);
		q->elevator = NULL;
		/* [한국어] 큐 깊이를 드라이버 태그 개수로 되돌린다. 스케줄러가 있을
		 * 때는 sched_tags 크기(보통 2배)를 쓰지만, 없으면 실제 드라이버가
		 * 받을 수 있는 만큼만 담는 것이 맞다. NVMe에서 tag_set->queue_depth는
		 * I/O 큐 하나의 깊이(nvme_dev의 q_depth, 보통 1024)에서 온다. */
		q->nr_requests = q->tag_set->queue_depth;
		/* [한국어] 비동기 요청 전용 깊이 제한도 함께 되돌린다. 스케줄러가
		 * 있을 때는 async 쓰기가 태그를 독점해 동기 읽기를 굶기지 않도록
		 * 더 낮은 값을 두지만, 스케줄러가 없으면 그런 제한이 무의미하다. */
		q->async_depth = q->tag_set->queue_depth;
	}
	/* [한국어] blktrace 로그에 전환 사실을 남긴다. I/O 성능이 특정 시점부터
	 * 달라졌을 때 스케줄러 변경이 원인인지 추적하는 데 쓴다. */
	blk_add_trace_msg(q, "elv switch: %s", ctx->name);

out_unfreeze:
	/* [한국어] dispatch를 재개한다. 이 시점부터 새 스케줄러(또는 none)를
	 * 통해 요청이 드라이버로 흐른다. freeze 해제는 호출자가 담당한다. */
	blk_mq_unquiesce_queue(q);

	if (ret) {
		/* [한국어] 실패를 사용자에게 알린다. 큐는 "none" 상태로 남아 I/O는
		 * 계속 동작하므로 치명적이지 않다. new_e는 여기 도달할 때 반드시
		 * NULL이 아니다(ret이 0이 아닌 유일한 경로가 blk_mq_init_sched
		 * 실패이고, 그것은 new_e가 있을 때만 실행되므로). */
		pr_warn("elv: switch to \"%s\" failed, falling back to \"none\"\n",
			new_e->elevator_name);
	}

	/* [한국어] elevator_find_get()으로 잡았던 모듈 참조를 반납한다. 성공했다면
	 * blk_mq_init_sched()가 자체 참조를 이미 잡았으므로 여기서 놓아도 안전하다. */
	if (new_e)
		elevator_put(new_e);
	return ret;
}

/*
 * [한국어]
 * elv_exit_and_release - elevator를 완전히 내리고 자원을 해제하는 rollback 경로
 *
 * @ctx: elevator 전환 컨텍스트 (전환 중 할당한 res/type 정보)
 * @q:   elevator를 제거할 request_queue
 *
 * elevator_change_done()에서 새 elevator의 sysfs 등록이 실패하면 이 함수로
 * rollback한다. queue를 freeze해 진행 중인 I/O를 멈추고, elevator_exit()으로
 * 스케줄러 상태를 해제한 뒤, sched_res와 kobject를 최종 반환한다.
 * 이 경로를 거치면 request_queue는 elevator가 없는("none") 상태가 된다.
 *
 * 호출 체인:
 *   elevator_change_done → elv_register_queue 실패 → [elv_exit_and_release]
 */
static void elv_exit_and_release(struct elv_change_ctx *ctx,
		struct request_queue *q)
{
	struct elevator_queue *e;
/* memflags: 메모리 압박 상황을 freeze 이전/이후에 복원하기 위해 저장 */
	unsigned memflags;

/* queue freeze: I/O submit/dispatch를 모두 중단 — elevator 해제 중 race 방지 */
	memflags = blk_mq_freeze_queue(q);
	mutex_lock(&q->elevator_lock);
/* 현재 elevator 포인터 보관 (elevator_exit 후 q->elevator는 NULL이 됨) */
	e = q->elevator;
/* elevator_exit: ioc 정리 + blk_mq_exit_sched(tag_set 연결 해제) */
	elevator_exit(q);
	mutex_unlock(&q->elevator_lock);
/* unfreeze: I/O 경로 재개 (elevator 없는 "none" 상태로) */
	blk_mq_unfreeze_queue(q, memflags);
	if (e) {
/* 전환 중 사전 할당한 sched_res(tag/hw_ctx 자원) 반환 */
		blk_mq_free_sched_res(&ctx->res, ctx->type, q->tag_set);
/* kobject_put: 참조 카운트가 0이 되면 elevator_release()가 kfree 수행 */
		kobject_put(&e->kobj);
	}
}

/*
 * [한국어]
 * elevator_change_done - 락 밖에서 해야 하는 뒷정리: 옛 스케줄러 해제 + 새 것 등록
 *
 * @q:   전환이 일어난 request_queue
 * @ctx: 전환 컨텍스트. ctx->old(내려간 것)와 ctx->new(올라온 것)가 채워져 있다.
 * @return: 0 성공, 음수 errno(새 스케줄러 sysfs 등록 실패)
 *
 * === 왜 elevator_switch()에서 다 하지 않고 이 함수로 미뤘는가 ===
 * elevator_switch()는 q->elevator_lock을 쥐고 큐가 freeze/quiesce된 상태에서
 * 실행된다. 그런데 이 함수가 하는 두 가지 일은 그 상태에서 하면 안 된다:
 *   - sysfs kobject 조작(생성/제거)은 내부적으로 자체 락을 잡는데, 그것이
 *     elevator_lock과 반대 순서로 잡히는 경로가 있어 데드락 위험이 있다.
 *   - kobject_put()으로 인한 최종 해제는 잠들 수 있다.
 * 그래서 elevator_switch()는 포인터만 ctx에 담아 두고, 호출자가 락을 푼 뒤
 * 이 함수를 불러 실제 정리를 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. q->elevator_lock을 쥐지 않은 상태이며,
 * 큐도 이미 unfreeze되어 I/O가 흐르고 있다. 옛 스케줄러는 이미 큐에서
 * 분리되었으므로 지금 해제해도 아무도 참조하지 않는다.
 *
 * 에러 경로: 새 스케줄러의 sysfs 등록이 실패하면 elv_exit_and_release()로
 * 롤백해 큐를 "none" 상태로 만든다. I/O는 계속 동작한다.
 *
 * 호출 체인:
 *   elevator_change → [elevator_change_done]
 *     → elv_unregister_queue / blk_mq_free_sched_res / kobject_put (옛 것)
 *     → elv_register_queue (새 것) → 실패 시 elv_exit_and_release
 */
static int elevator_change_done(struct request_queue *q,
				struct elv_change_ctx *ctx)
{
	int ret = 0;

	/* [한국어] 옛 스케줄러가 있었다면(none → X 전환이 아니라면) 먼저 정리한다.
	 * 순서가 중요하다 — 옛 것을 완전히 치우고 나서 새 것을 등록해야
	 * /sys/.../queue/iosched 라는 같은 이름의 kobject가 충돌하지 않는다. */
	if (ctx->old) {
		/* [한국어] 해제할 자원 정보를 스택 구조체에 미리 복사해 둔다.
		 * elv_unregister_queue()나 kobject_put()이 진행되면서 ctx->old의
		 * 필드가 무효해질 수 있으므로, 그 전에 필요한 값(et = 스케줄러 태그
		 * 세트, data = 스케줄러 사설 데이터)을 안전한 곳으로 옮긴다. */
		struct elevator_resources res = {
			.et = ctx->old->et,
			.data = ctx->old->elevator_data
		};

		/* [한국어] sysfs에서 iosched 디렉터리와 튜너블 파일들을 제거한다.
		 * 이 시점부터 사용자 공간은 옛 스케줄러의 파라미터에 접근할 수 없다. */
		elv_unregister_queue(q, ctx->old);
		/* [한국어] 스케줄러 전용 태그 세트(sched_tags)와 사설 데이터를 해제한다.
		 * sched_tags는 하드웨어 큐마다 하나씩 있으므로 tag_set 정보가 필요하다.
		 * NVMe에서 이 태그들은 드라이버 태그(= Command ID)와 별개의 논리적
		 * 슬롯이므로, 해제해도 컨트롤러 쪽 상태에는 영향이 없다. */
		blk_mq_free_sched_res(&res, ctx->old->type, q->tag_set);
		/* [한국어] kobject 참조를 놓는다. 마지막 참조였다면 elevator_release()가
		 * 호출되어 elevator_queue 구조체 자체가 kfree된다. sysfs를 통해 누군가
		 * 아직 파일을 열고 있다면 그가 닫을 때까지 해제가 지연된다 —
		 * 참조 계수 방식이 주는 안전성이다. */
		kobject_put(&ctx->old->kobj);
	}
	/* [한국어] 새 스케줄러가 올라왔다면("none"으로의 전환이 아니라면) 사용자
	 * 공간에 노출한다. */
	if (ctx->new) {
		/* [한국어] no_uevent를 뒤집어 uevent 인자로 넘긴다. 부팅 시 기본
		 * 스케줄러 설정은 no_uevent=true라 udev 이벤트를 보내지 않고,
		 * 사용자가 sysfs로 명시적으로 바꾼 경우에는 이벤트를 보낸다. */
		ret = elv_register_queue(q, ctx->new, !ctx->no_uevent);
		/* [한국어] sysfs 등록 실패 — 스케줄러는 이미 큐에 붙어 동작 중이지만
		 * 사용자가 제어할 수 없는 반쪽 상태다. 그런 상태로 두느니 완전히
		 * 내려서 "none"으로 만드는 편이 낫다. */
		if (ret)
			elv_exit_and_release(ctx, q);
	}
	return ret;
}

/*
 * Switch this queue to the given IO scheduler.
 */
/*
 * elevator_change - 사용자/sysfs 요청에 따라 elevator를 변경
 *   호출 경로: elevator_set_default / elv_iosched_store -> elevator_change
 *   NVMe 연결:
 *     - queue freeze -> blk_mq_cancel_work_sync -> elevator_switch ->
 *       blk_mq_init_sched -> blk_mq_unquiesce_queue.
 *     - 이 과정에서 NVMe SQ/CID는 그대로 두고, 상위 request_queue의
 *       스케줄링 레이어만 교체한다.
 */
static int elevator_change(struct request_queue *q, struct elv_change_ctx *ctx)
{
	unsigned int memflags;
	struct blk_mq_tag_set *set = q->tag_set;
	int ret = 0;

	lockdep_assert_held(&set->update_nr_hwq_lock);

	/* [한국어] "none"이 아니라면 스케줄러 자원을 미리 할당한다.
	 * 왜 미리 하는가: 아래에서 큐를 freeze한 뒤에 할당하면, 메모리 회수가
	 * 이 큐로의 write-back I/O를 필요로 할 때 데드락이 발생한다(freeze된 큐는
	 * I/O를 받지 않으므로 회수가 영원히 끝나지 않는다). freeze 전에 할당하면
	 * 이 재귀를 피할 수 있다.
	 * nr_hw_queues만큼 할당하는 이유: 스케줄러 태그 세트는 하드웨어 큐마다
	 * 하나씩 필요하기 때문이다. NVMe에서는 SQ/CQ 쌍 개수만큼 만들어진다. */
	if (strncmp(ctx->name, "none", 4)) {
		ret = blk_mq_alloc_sched_res(q, ctx->type, &ctx->res,
				set->nr_hw_queues);
		/* [한국어] 메모리 부족 — 아직 아무것도 바꾸지 않았으므로 그대로 반환.
		 * 기존 스케줄러가 계속 동작한다. */
		if (ret)
			return ret;
	}

	/* [한국어] 큐를 freeze한다. 새 요청 진입을 막고 진행 중인 요청이 모두
	 * 완료되기를 기다린다. 반환값 memflags는 freeze 동안 설정된
	 * PF_MEMALLOC_NOIO 상태를 담고 있어, unfreeze 시 원복하는 데 쓴다.
	 * 이 플래그가 필요한 이유: freeze 구간에서 메모리 할당이 I/O를 유발하면
	 * 자기 자신이 막아 놓은 큐를 기다리는 데드락이 되므로, 커널에 "이 구간에서는
	 * I/O를 유발하는 회수를 하지 말라"고 알려 둔다. */
	memflags = blk_mq_freeze_queue(q);
	/*
	 * May be called before adding disk, when there isn't any FS I/O,
	 * so freezing queue plus canceling dispatch work is enough to
	 * drain any dispatch activities originated from passthrough
	 * requests, then no need to quiesce queue which may add long boot
	 * latency, especially when lots of disks are involved.
	 *
	 * Disk isn't added yet, so verifying queue lock only manually.
	 */
	/* [한국어] 예약된 dispatch 워크(run_work, requeue_work)를 취소하고 이미
	 * 실행 중인 것이 끝나기를 기다린다. freeze는 새 요청을 막을 뿐, 워커가
	 * 이미 큐에 있던 요청을 드라이버로 내보내는 것은 막지 못한다. 스케줄러를
	 * 교체하는 동안 워커가 옛 스케줄러 자료구조를 만지면 안 되므로 필요하다.
	 * 위 영문 주석이 설명하듯, freeze + 워크 취소만으로 충분해서 quiesce까지
	 * 걸지 않는다 — 디스크가 여러 개인 시스템에서 quiesce의 SRCU 대기가
	 * 부팅 지연을 크게 늘리기 때문이다. */
	blk_mq_cancel_work_sync(q);
	/* [한국어] 스케줄러 교체를 직렬화하는 뮤텍스. 두 스레드가 동시에 sysfs로
	 * 스케줄러를 바꾸는 상황을 막는다. */
	mutex_lock(&q->elevator_lock);
	/* [한국어] 요청된 스케줄러가 이미 붙어 있으면 아무것도 하지 않는다.
	 * 조건을 풀어 읽으면 "현재 스케줄러가 있고 그 이름이 요청과 일치하는"
	 * 경우가 아닐 때만 전환한다. 같은 스케줄러로 다시 바꾸는 것은 무의미한
	 * freeze/quiesce 비용만 발생시키므로 걸러 낸다.
	 * q->elevator가 NULL("none")인데 ctx->name도 "none"인 경우는 이 조건에
	 * 걸리지 않아 elevator_switch()가 호출되지만, 그쪽에서 다시 무해하게
	 * 처리된다. */
	if (!(q->elevator && elevator_match(q->elevator->type, ctx->name)))
		ret = elevator_switch(q, ctx);
	mutex_unlock(&q->elevator_lock);
	/* [한국어] 큐를 다시 열어 I/O를 재개한다. memflags로 freeze 전의 메모리
	 * 할당 컨텍스트를 원복한다. 이 시점부터 새 스케줄러로 요청이 흐른다. */
	blk_mq_unfreeze_queue(q, memflags);
	/* [한국어] 전환이 성공했다면 락 밖에서 해야 하는 뒷정리(옛 스케줄러 해제,
	 * 새 스케줄러 sysfs 등록)를 수행한다. 실패했다면 정리할 것이 없다. */
	if (!ret)
		ret = elevator_change_done(q, ctx);

	/*
	 * Free sched resource if it's allocated but we couldn't switch elevator.
	 */
	/* [한국어] ctx->new가 NULL이면 새 스케줄러가 큐에 붙지 못했다는 뜻이다
	 * (전환 실패, 또는 "none"으로 전환, 또는 이미 같은 스케줄러라 생략).
	 * 그렇다면 위에서 미리 할당해 둔 자원이 주인 없이 남으므로 해제한다.
	 * 반대로 ctx->new가 있으면 그 자원은 스케줄러가 소유하고 있으므로
	 * 여기서 건드리면 안 된다. */
	if (!ctx->new)
		blk_mq_free_sched_res(&ctx->res, ctx->type, set);

	return ret;
}

/*
 * The I/O scheduler depends on the number of hardware queues, this forces a
 * reattachment when nr_hw_queues changes.
 */
/*
 * elv_update_nr_hw_queues - hw_queue 수(nr_hw_queues) 변경 시 elevator 재부착
 *   호출 경로: blk_mq_update_nr_hw_queues -> elv_update_nr_hw_queues
 *   NVMe 연결:
 *     - NVMe 멀티큐 컨트롤러가 nr_io_queues를 변경하면 각 blk_mq_hw_ctx와
 *       tag_set이 갱신되므로, elevator도 동일한 hw_queue 수에 맞춰
 *       다시 초기화해야 함.
 *     - elevator_switch -> elevator_change_done 순으로 처리.
 */
void elv_update_nr_hw_queues(struct request_queue *q,
		struct elv_change_ctx *ctx)
{
	struct blk_mq_tag_set *set = q->tag_set;
	int ret = -ENODEV;

	WARN_ON_ONCE(q->mq_freeze_depth == 0);
/* queue freeze 상태여야 함: NVMe hw_queue 변경 중 I/O 정지 보장 */

	if (ctx->type && !blk_queue_dying(q) && blk_queue_registered(q)) {
		mutex_lock(&q->elevator_lock);
		/* force to reattach elevator after nr_hw_queue is updated */
/* dying/registered가 아니면 elevator 재부착 */
		ret = elevator_switch(q, ctx);
		mutex_unlock(&q->elevator_lock);
	}
/* unfreeze: NVMe I/O 재개 */
	blk_mq_unfreeze_queue_nomemrestore(q);
	if (!ret)
		WARN_ON_ONCE(elevator_change_done(q, ctx));

	/*
	 * Free sched resource if it's allocated but we couldn't switch elevator.
	 */
	if (!ctx->new)
/* 재부착 실패 시 자원 해제 */
		blk_mq_free_sched_res(&ctx->res, ctx->type, set);
}

/*
 * Use the default elevator settings. If the chosen elevator initialization
 * fails, fall back to the "none" elevator (no elevator).
 */
/*
 * elevator_set_default - 장치 등록 시 기본 IO 스케줄러를 연결
 *   호출 경로: add_disk -> blk_register_queue -> elevator_set_default
 *   NVMe 연결:
 *     - NVMe namespace가 디스크로 등록될 때 호출.
 *     - 단일 hw_queue이거나 shared tags인 경우에만 mq-deadline을 시도하고,
 *       그 외(멀티큐)에는 "none"을 그대로 둔다.
 *
 * === NVMe가 보통 "none"이 되는 실제 이유 ===
 * 조건은 코드 그대로 (q->nr_hw_queues == 1 || blk_mq_is_shared_tags(...)) 이다.
 * NVMe PCIe는 CPU 수에 맞춰 여러 개의 하드웨어 큐(SQ/CQ 쌍)를 만들므로
 * nr_hw_queues > 1 이고 shared tags도 쓰지 않아, 이 조건에 걸리지 않아
 * "none"으로 남는다. NVMe 드라이버가 BLK_MQ_F_NO_SCHED_BY_DEFAULT를 설정해서가
 * 아니다(설정하지 않는다).
 *
 * 왜 멀티큐에 스케줄러를 붙이지 않는가:
 *   I/O 스케줄러의 본래 목적은 회전 디스크에서 탐색(seek)을 줄이려고 요청을
 *   재정렬하는 것이었다. NVMe SSD는 탐색이 없고 내부적으로 수십 개의 채널을
 *   병렬 처리하므로 재정렬 이득이 없다. 반면 스케줄러는 큐 전역 락을 쓰기
 *   때문에 여러 CPU가 동시에 제출할 때 심각한 경합을 만든다. 즉 멀티큐 NVMe에서
 *   스케줄러는 이득 없이 비용만 발생시킨다.
 *   다만 지연 격리나 cgroup 대역폭 제어가 필요하면 사용자가 sysfs로
 *   mq-deadline이나 bfq를 명시적으로 붙일 수 있다.
 *
 * 하드웨어 큐가 하나뿐인 장치(SATA AHCI, 일부 eMMC/UFS, 큐를 하나만 만든
 * NVMe 구성)는 어차피 락이 하나로 직렬화되므로 스케줄러의 재정렬 이득이
 * 비용을 넘어선다. 그래서 mq-deadline이 기본값이 된다.
 */
void elevator_set_default(struct request_queue *q)
{
	struct elv_change_ctx ctx = {
		.name = "mq-deadline",
/* 기본 스케줄러: mq-deadline */
		.no_uevent = true,
	};
	int err;

	/* now we allow to switch elevator */
/* elevator 전환 허용: NVMe 장치도 sysfs로 변경 가능 */
	blk_queue_flag_clear(QUEUE_FLAG_NO_ELV_SWITCH, q);

	/* [한국어] 드라이버가 "기본적으로 스케줄러를 붙이지 말라"고 명시한 경우
	 * 즉시 반환해 "none" 상태를 유지한다. 스케줄링이 오히려 해로운 장치
	 * (예: 자체적으로 순서를 최적화하는 가상 장치)를 위한 탈출구다.
	 * 참고: NVMe PCIe 드라이버는 이 플래그를 설정하지 않는다. NVMe가 보통
	 * "none"이 되는 이유는 이 플래그가 아니라 아래의 nr_hw_queues 조건이다. */
	if (q->tag_set->flags & BLK_MQ_F_NO_SCHED_BY_DEFAULT)
		return;

	/*
	 * For single queue devices, default to using mq-deadline. If we
	 * have multiple queues or mq-deadline is not available, default
	 * to "none".
	 */
	ctx.type = elevator_find_get(ctx.name);
/* elv_list에서 mq-deadline 조회; 없으면 none 유지 */
	if (!ctx.type)
		return;

	if ((q->nr_hw_queues == 1 ||
/* 단일 큐/shared tags일 때만 mq-deadline 시도; 멀티큐 NVMe는 none 선호 */
			blk_mq_is_shared_tags(q->tag_set->flags))) {
		err = elevator_change(q, &ctx);
/* elevator_change 실패 시 none으로 폴백(fallback) */
		if (err < 0)
			pr_warn("\"%s\" elevator initialization, failed %d, falling back to \"none\"\n",
					ctx.name, err);
	}
	elevator_put(ctx.type);
/* 참조 해제: 이미 request_queue에 연결되었거나 실패함 */
}

/*
 * elevator_set_none - 스케줄러를 "none"으로 변경
 *   호출 경로: 사용자 sysfs "none" 쓰기 -> elv_iosched_store -> elevator_set_none
 *   NVMe 연결: NVMe 고성능 장치에서 software scheduling 오버헤드를 제거하고
 *              bio/request를 직접 blk-mq로 통과시켜 nvme_queue_rq()로 빠르게
 *              전달한다.
 */
void elevator_set_none(struct request_queue *q)
{
	struct elv_change_ctx ctx = {
		.name	= "none",
	};
	int err;

	err = elevator_change(q, &ctx);
	if (err < 0)
		pr_warn("%s: set none elevator failed %d\n", __func__, err);
}

/*
 * [한국어]
 * elv_iosched_load_module - 아직 로드되지 않은 스케줄러 모듈을 자동 로드
 *
 * @elevator_name: 로드할 스케줄러 이름 (예: "bfq", "mq-deadline")
 *
 * 사용자가 sysfs로 특정 스케줄러를 요청했을 때, 해당 스케줄러가 아직
 * elv_list에 없으면 "<name>-iosched" 모듈을 request_module()로 자동 로드한다.
 * 이렇게 하면 "bfq-iosched.ko"를 명시적으로 insmod하지 않아도 sysfs 쓰기만으로
 * BFQ를 활성화할 수 있다. 모듈 로드 이후 elv_list에 등록되므로
 * 이후 elevator_find_get()이 성공한다.
 *
 * 호출 체인:
 *   elv_iosched_store → [elv_iosched_load_module] → request_module()
 */
static void elv_iosched_load_module(const char *elevator_name)
{
	struct elevator_type *found;

/* elv_list_lock 짧게 획득: 모듈 로드 여부만 확인하므로 빠르게 해제 */
	spin_lock(&elv_list_lock);
/* 이미 등록된 스케줄러라면 모듈 로드 불필요 */
	found = __elevator_find(elevator_name);
	spin_unlock(&elv_list_lock);

	if (!found)
/* 미등록 스케줄러: "<name>-iosched" 패턴으로 커널 모듈 자동 로드 */
		request_module("%s-iosched", elevator_name);
}

/*
 * elv_iosched_store - /sys/block/<disk>/queue/scheduler 쓰기 처리
 *   호출 경로: sysfs write -> elv_iosched_store
 *   NVMe 연결:
 *     - 사용자가 "none", "mq-deadline", "bfq" 등을 선택하면,
 *       elevator_change()를 통해 NVMe request_queue의 스케줄러가 변경됨.
 *     - 모듈 자동 로드 후 update_nr_hwq_lock을 획득하여 race 방지.
 */
ssize_t elv_iosched_store(struct gendisk *disk, const char *buf,
			  size_t count)
{
	char elevator_name[ELV_NAME_MAX];
	struct elv_change_ctx ctx = {};
	int ret;
	struct request_queue *q = disk->queue;
	struct blk_mq_tag_set *set = q->tag_set;

	/* Make sure queue is not in the middle of being removed */
	/* [한국어] 큐가 sysfs에 등록된 상태인지 확인한다. 장치 제거(del_gendisk)가
	 * 진행 중이면 이 플래그가 이미 내려가 있어, 사라지는 중인 큐의 스케줄러를
	 * 바꾸려는 무의미하고 위험한 시도를 막는다. NVMe 컨트롤러가 갑자기
	 * 뽑히는(surprise removal) 상황에서 실제로 발생할 수 있는 경쟁이다. */
	if (!blk_queue_registered(q))
		return -ENOENT;

	/*
	 * If the attribute needs to load a module, do it before freezing the
	 * queue to ensure that the module file can be read when the request
	 * queue is the one for the device storing the module file.
	 */
	/* [한국어] 사용자 입력을 고정 크기 스택 버퍼로 복사한다. strscpy는 항상
	 * NUL로 끝맺고 넘치면 잘라 내므로, sysfs로 들어온 임의 길이 입력에
	 * 안전하다(strcpy나 strncpy와 달리). */
	strscpy(elevator_name, buf, sizeof(elevator_name));
	/* [한국어] 앞뒤 공백과 개행을 제거한다. `echo mq-deadline > scheduler`는
	 * 끝에 '\n'을 붙여 쓰므로 이 처리가 없으면 이름 비교가 항상 실패한다.
	 * strstrip은 버퍼 안에서 앞쪽 공백을 건너뛴 포인터를 돌려주고 뒤쪽은
	 * NUL로 잘라 낸다. */
	ctx.name = strstrip(elevator_name);

	/* [한국어] 필요하면 스케줄러 모듈을 먼저 로드한다. 위 영문 주석이 밝히듯
	 * 이것을 큐 freeze "전에" 해야 한다 — 모듈 파일이 바로 이 블록 장치에
	 * 저장되어 있을 수 있는데, freeze된 큐에서 모듈을 읽으려 하면 자기 자신을
	 * 기다리는 데드락이 된다. */
	elv_iosched_load_module(ctx.name);
	/* [한국어] 이름으로 스케줄러 타입을 찾고 모듈 참조를 획득한다.
	 * "none"이면 NULL이 반환되는데 이는 오류가 아니라 정상 경로다. */
	ctx.type = elevator_find_get(ctx.name);

	/*
	 * Use trylock to avoid circular lock dependency with kernfs active
	 * reference during concurrent disk deletion:
	 *   update_nr_hwq_lock -> kn->active (via del_gendisk -> kobject_del)
	 *   kn->active -> update_nr_hwq_lock (via this sysfs write path)
	 */
	/* [한국어] 하드웨어 큐 수 변경(blk_mq_update_nr_hw_queues)과의 동시 실행을
	 * 막는 읽기 락을 잡는다. 스케줄러 자원은 hw 큐 개수만큼 할당되므로,
	 * 할당 도중에 개수가 바뀌면 크기가 어긋난다.
	 *
	 * 일반 down_read가 아니라 trylock을 쓰는 이유는 위 영문 주석의 순환 의존
	 * 때문이다:
	 *   디스크 삭제 경로: update_nr_hwq_lock → kn->active (kobject_del)
	 *   이 sysfs 쓰기 경로: kn->active(이미 보유) → update_nr_hwq_lock
	 * 두 경로가 락을 반대 순서로 잡으므로 서로 기다리면 데드락이다.
	 * trylock으로 즉시 포기하고 -EBUSY를 돌려주면 사용자가 재시도하면 된다.
	 * NVMe 컨트롤러 리셋 중에 스케줄러를 바꾸려 하면 실제로 이 -EBUSY를 본다. */
	if (!down_read_trylock(&set->update_nr_hwq_lock)) {
		ret = -EBUSY;
		goto out;
	}
	/* [한국어] QUEUE_FLAG_NO_ELV_SWITCH는 "지금은 스케줄러를 바꾸지 말라"는
	 * 일시적 금지 표시로, 디스크 등록 전이나 하드웨어 큐 재구성 중에 설정된다.
	 * 이 플래그가 없을 때만 실제 전환을 수행한다. */
	if (!blk_queue_no_elv_switch(q)) {
		ret = elevator_change(q, &ctx);
		/* [한국어] sysfs write 핸들러의 관례상 "소비한 바이트 수"를 반환해야
		 * 한다. 성공했으면 입력 전체를 소비한 것이므로 count를 돌려준다.
		 * 그렇지 않으면 사용자 공간의 write()가 부분 쓰기로 오해해 무한 재시도한다. */
		if (!ret)
			ret = count;
	} else {
		ret = -ENOENT;
	}
	/* [한국어] 읽기 락 해제. 이 시점부터 하드웨어 큐 수 변경이 가능해진다. */
	up_read(&set->update_nr_hwq_lock);

out:
	if (ctx.type)
/* 스케줄러 타입 참조 해제 */
		elevator_put(ctx.type);
	return ret;
}

/*
 * elv_iosched_show - /sys/block/<disk>/queue/scheduler 읽기 처리
 *   호출 경로: sysfs read -> elv_iosched_show
 *   NVMe 연결: 현재 NVMe 장치에 적용된 스케줄러를 대괄호로 표시하며,
 *              등록된 스케줄러 목록을 반환한다.
 */
ssize_t elv_iosched_show(struct gendisk *disk, char *name)
{
	struct request_queue *q = disk->queue;
	struct elevator_type *cur = NULL, *e;
	int len = 0;

/* elevator_lock: q->elevator 포인터를 안정적으로 읽기 위해 획득 */
	mutex_lock(&q->elevator_lock);
	if (!q->elevator) {
/* elevator 없음("none"): 현재 선택을 대괄호로 표시 */
		len += sprintf(name+len, "[none] ");
	} else {
/* elevator 있음: "none"은 대괄호 없이, 현재 스케줄러는 뒤에서 대괄호 */
		len += sprintf(name+len, "none ");
/* cur: 현재 활성 스케줄러 타입, 이후 목록 출력 시 대괄호 판단에 사용 */
		cur = q->elevator->type;
	}

/* elv_list_lock: 등록된 스케줄러 목록을 안정적으로 순회하기 위해 획득 */
	spin_lock(&elv_list_lock);
/* elv_list 순회: 등록된 모든 스케줄러를 공백으로 구분해 출력 */
	list_for_each_entry(e, &elv_list, list) {
		if (e == cur)
/* 현재 활성 스케줄러: 대괄호로 강조 (예: [mq-deadline]) */
			len += sprintf(name+len, "[%s] ", e->elevator_name);
		else
/* 비활성 스케줄러: 이름만 출력 */
			len += sprintf(name+len, "%s ", e->elevator_name);
	}
	spin_unlock(&elv_list_lock);

/* 줄바꿈으로 출력 종료 */
	len += sprintf(name+len, "\n");
	mutex_unlock(&q->elevator_lock);

	return len;
}

/*
 * [한국어]
 * elv_rb_former_request - LBA 기준 RB-tree에서 @rq 바로 앞의 request 반환
 *
 * @q:  request_queue (미사용이지만 인터페이스 일관성을 위해 유지)
 * @rq: 기준 request
 * @return: LBA가 바로 작은 request; 없으면 NULL
 *
 * mq-deadline·BFQ 등의 스케줄러가 front-merge 후보를 찾거나 dispatch 순서를
 * 역방향으로 추적할 때 사용한다. rb_prev()는 O(log N)이다.
 * elv_former_request()의 실제 구현체이며 스케줄러가 ops.former_request로
 * 이 함수를 등록한다.
 *
 * 호출 체인:
 *   elv_former_request → 스케줄러 ops.former_request → [elv_rb_former_request]
 */
struct request *elv_rb_former_request(struct request_queue *q,
				      struct request *rq)
{
/* rb_prev: RB-tree에서 LBA 기준 직전 노드 — front-merge의 빈번한 탐색 경로 */
	struct rb_node *rbprev = rb_prev(&rq->rb_node);

/* 이전 노드가 있으면 해당 request 반환; 없으면 NULL (첫 번째 LBA가 가장 작음) */
	if (rbprev)
		return rb_entry_rq(rbprev);

	return NULL;
}
EXPORT_SYMBOL(elv_rb_former_request);

/*
 * [한국어]
 * elv_rb_latter_request - LBA 기준 RB-tree에서 @rq 바로 뒤의 request 반환
 *
 * @q:  request_queue (미사용이지만 인터페이스 일관성을 위해 유지)
 * @rq: 기준 request
 * @return: LBA가 바로 큰 request; 없으면 NULL
 *
 * mq-deadline·BFQ 등의 스케줄러가 back-merge 후보 및 dispatch 방향 탐색에 사용.
 * elv_latter_request()의 실제 구현체이며 ops.next_request로 등록된다.
 *
 * 호출 체인:
 *   elv_latter_request → 스케줄러 ops.next_request → [elv_rb_latter_request]
 */
struct request *elv_rb_latter_request(struct request_queue *q,
				      struct request *rq)
{
/* rb_next: RB-tree에서 LBA 기준 직후 노드 — back-merge·dispatch 연속성 탐색 */
	struct rb_node *rbnext = rb_next(&rq->rb_node);

/* 다음 노드가 있으면 해당 request 반환; 없으면 NULL (마지막 LBA) */
	if (rbnext)
		return rb_entry_rq(rbnext);

	return NULL;
}
EXPORT_SYMBOL(elv_rb_latter_request);

/*
 * elevator_setup - "elevator=" 커널 파라미터 처리(더 이상 효과 없음)
 *   NVMe 연결: 현재 NVMe 장치는 sysfs/udev에서 per-device scheduler를
 *              설정하므로, 이 파라미터는 무시됨.
 */
static int __init elevator_setup(char *str)
{
	pr_warn("Kernel parameter elevator= does not have any effect anymore.\n"
		"Please use sysfs to set IO scheduler for individual devices.\n");
	return 1;
}

__setup("elevator=", elevator_setup);

/* NVMe 관점 핵심 요약 */
/*
 *   - elevator.c는 bio/request가 nvme_queue_rq()를 통해 SQ/CID로 변환되기 전의
 *     마지막 software scheduling 단계이다.
 *   - elv_merge() 계열 함수는 연속 LBA bio를 병합하여 PRP/SGL 체인 길이와
 *     doorbell 횟수를 줄이는 데 기여한다.
 *   - struct elevator_queue는 스케줄러 상태(tag, hash, elevator_data)를
 *     관리하며, NVMe 멀티큐 환경에서도 blk_mq_hw_ctx 단위로 동작한다.
 *   - elevator_change/switch/update_nr_hw_queues는 queue freeze를 이용해
 *     NVMe I/O를 일시 중단하고 스케줄러를 안전하게 교체한다.
 *   - "none" 스케줄러를 선택하면 software scheduling 오버헤드가 사라지고,
 *     bio는 곧바로 blk-mq -> nvme_queue_rq() 경로로 흐른다.
 */
