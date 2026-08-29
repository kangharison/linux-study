/* SPDX-License-Identifier: GPL-2.0 */

/*
 * [한국어 설명] blkcg 기반 I/O 우선순위(io.prio.class) 강제 정책의 공개 인터페이스 (blk-ioprio.h)
 *
 * === 파일의 역할 ===
 * 이 헤더는 block/blk-ioprio.c에 구현되어 있는 blkcg(Block I/O Control Group)
 * 정책 중 "io.prio.class" 서브파일을 통해 REQ_OP 우선순위 클래스(RT=Real Time,
 * BE=Best Effort, IDLE)를 bio 단위로 강제(override)하는 기능의, 외부에 노출되는
 * 유일한 진입점 blkcg_set_ioprio() 하나를 선언한다. 이 파일 자체에는 실제 정책
 * 판단 로직(cgroup 계층을 따라 올라가며 prio_policy를 찾는 로직, IOPRIO_CLASS_*
 * 매핑 규칙)이 전혀 들어있지 않다 — 그런 로직은 모두 block/blk-ioprio.c에 있고,
 * 이 헤더는 그 함수를 "CONFIG_BLK_CGROUP_IOPRIO가 켜져 있을 때만 실제로 존재하고,
 * 꺼져 있으면 빈 인라인 함수로 대체되는" 형태로 호출부에 제공하는 얇은 어댑터
 * 역할만 한다. 즉 이 파일의 존재 이유는 "blkcg I/O 우선순위 정책이 빌드에
 * 포함되어 있는지 여부"라는 컴파일 타임 조건을 호출부(block/blk-mq.c 등)가
 * #ifdef 없이도 신경 쓰지 않도록 캡슐화하는 것이다.
 * 파일이 짧다고 해서 역할이 가벼운 것은 아니다 — 이 헤더가 선언하는 단 하나의
 * 함수가 모든 blkcg 우선순위 강제 정책이 블록 계층에 연결되는 유일한 관문이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 블록 I/O 제출 경로의 아주 앞 단계, 즉 사용자/파일시스템이 만든 bio가 아직
 * request로 승격되기 이전 단계에 위치한다. 대략적인 호출 체인은 다음과 같다:
 *   (파일시스템/direct I/O 등에서 bio 생성)
 *   -> submit_bio_noacct() -> submit_bio_noacct_nocheck()
 *   -> blkcg_set_ioprio(bio)   [이 헤더가 선언하는 지점]
 *   -> (bio->bi_ioprio가 cgroup 설정값으로 갱신될 수 있음)
 *   -> blk_mq_submit_bio() -> blk_mq_get_request() (bio -> request 승격,
 *      request->ioprio에 bio->bi_ioprio가 복사됨)
 *   -> I/O 스케줄러(mq-deadline/bfq/kyber)의 삽입/디스패치 순서 결정
 *   -> (NVMe 드라이버가 붙어 있다면) nvme_queue_rq() -> nvme_sq_copy_cmd/nvme_write_sq_db()로
 *      SQ(Submission Queue)에 커맨드가 올라가고 도어벨(doorbell)이 눌린다.
 * 이 함수는 "아직 스케줄러 정책이 개입하기 전, 순수 bio 단계에서 우선순위를
 * 덮어쓰는 마지막 지점"이라는 점이 중요하다 — 이후 단계(스케줄러 삽입, 디스패치,
 * NVMe 커맨드 조립)는 이미 정해진 bi_ioprio/ioprio 값을 그대로 참고만 할 뿐,
 * 다시 cgroup을 조회해 값을 재계산하지 않는다.
 * 실행 컨텍스트는 I/O를 제출하는 프로세스(태스크)의 컨텍스트다 — bio 제출은
 * 항상 프로세스 컨텍스트에서 시작되므로, 이 헤더가 선언하는 함수도 인터럽트
 * 컨텍스트나 소프트인터럽트(softirq)에서 호출되지 않는다.
 *
 * === 타 모듈과의 연결 ===
 * - block/blk-ioprio.c: 이 헤더가 선언만 하는 blkcg_set_ioprio()의 실제 구현체.
 *   cgroup 계층에 걸려 있는 blkcg_policy_data(struct ioprio_blkcg의 prio_policy)를
 *   조회해 IOPRIO_CLASS_RT/BE/IDLE 중 하나를 bio->bi_ioprio에 기록한다.
 * - include/linux/ioprio.h: bio->bi_ioprio 필드의 비트 레이아웃(상위 비트의
 *   클래스 필드 + 하위 비트의 우선순위 레벨)과 IOPRIO_PRIO_VALUE() 등 인코딩
 *   매크로를 정의하는 상위 헤더. blk-ioprio.c는 이 매크로들을 이용해 값을 조립한다.
 * - block/blk-cgroup.h, block/blk-cgroup.c: bio가 속한 cgroup(blkg/blkcg)을
 *   찾아내는 상위 인프라. blkcg_set_ioprio()는 그 결과로 얻어지는 blkcg를
 *   전제로 정책 데이터를 조회한다.
 * - block/blk-mq.c, block/elevator.c 및 I/O 스케줄러 구현체
 *   (block/mq-deadline.c, block/bfq-iosched.c, block/kyber-iosched.c):
 *   bi_ioprio가 request->ioprio로 전파된 뒤, 디스패치 순서를 정할 때 이 값을
 *   참고하는 대표적인 소비자들이다.
 * - drivers/nvme/host/*.c: NVMe 드라이버 관점에서는 request->ioprio가 최종적으로
 *   NVMe 커맨드를 SQ에 넣는 시점까지 살아남을 수 있고, 컨트롤러가
 *   WRR(Weighted Round Robin) Arbitration을 지원한다면 URGENT/HIGH/MEDIUM/LOW
 *   클래스 힌트로 이어질 가능성이 있다. 다만 이는 NVMe 스펙 자체의 커맨드
 *   우선순위/Arbitration 메커니즘과는 별개의 계층이며, 정확한 매핑 여부는 이
 *   블록 계층 코드가 아니라 개별 NVMe 드라이버/컨트롤러 구현에 달려 있다 (추정).
 * 데이터 흐름을 한 줄로 요약하면: cgroup 설정 파일(io.prio.class) 기록 ->
 * blkcg_policy_data 갱신 -> (다음 I/O 제출 시) blkcg_set_ioprio(bio) 호출 ->
 * bio->bi_ioprio 갱신 -> request->ioprio로 전파 -> 스케줄러/드라이버가 소비.
 *
 * === 주요 함수/구조체 요약 ===
 * - blkcg_set_ioprio(struct bio *bio): 이 파일이 선언하는 유일한 함수.
 *   CONFIG_BLK_CGROUP_IOPRIO=y이면 block/blk-ioprio.c의 실제 구현이 링크되고,
 *   =n이면 바로 아래(§else 분기)의 아무 일도 하지 않는 인라인 함수가 대신
 *   사용된다. 두 경우 모두 호출부(blk-mq.c 등)의 소스 코드는 동일하다.
 * - struct request_queue (전방 선언, 12번째 줄 부근): 이 헤더에서는 이름만
 *   참조될 뿐 필드 정의는 없는 불완전 타입(incomplete type)이다. 이 헤더
 *   자체의 함수 시그니처에는 등장하지 않지만, blk-ioprio.c 등 이 헤더를
 *   포함하는 번역 단위에서 포인터 타입으로 쓰일 수 있도록 미리 알려주는
 *   전방 선언이다.
 * - struct bio (전방 선언): blkcg_set_ioprio()의 유일한 매개변수 타입. 이
 *   헤더는 bio의 내부 필드 구조(bi_ioprio 등)를 알 필요가 없으므로, 완전한
 *   정의(struct 본문)를 include하지 않고 불완전 타입으로만 선언해 헤더
 *   의존성을 최소화한다.
 */

#ifndef _BLK_IOPRIO_H_		/* [한국어] 중복 include 방지 시작 - 이 헤더가 여러 소스 파일(blk-mq.c, blk-ioprio.c 등)에서 반복 include돼도 blkcg_set_ioprio 선언이 중복 정의되지 않도록 막는다 */
#define _BLK_IOPRIO_H_		/* [한국어] 가드 매크로 정의 - 이 지점 이후 다시 이 헤더가 include되면 #ifndef가 거짓이 되어 본문 전체(전방 선언, 함수 선언/정의)가 스킵된다 */

#include <linux/kconfig.h>	/* [한국어] IS_ENABLED()/CONFIG_* 매크로 계열을 쓰기 위한 include - 이 헤더에서는 바로 아래 #ifdef CONFIG_BLK_CGROUP_IOPRIO 분기가 이 컴파일 스위치에 의존하며, 꺼지면 blkcg_set_ioprio()가 no-op으로 대체된다 */

/*
 * [한국어] 아래 두 전방 선언(forward declaration)은 이 헤더가 request_queue와
 * bio의 "완전한 정의"를 몰라도 함수 시그니처를 선언할 수 있게 해 준다.
 * - struct request_queue: NVMe 하드웨어 큐(nvme_queue)나 blk_mq_hw_ctx와
 *   최종적으로 연결되는 블록 레벨 큐. 이 헤더의 함수들이 직접 사용하진
 *   않지만, 이 헤더를 include하는 다른 코드가 request_queue 포인터를 쓸 수
 *   있도록 이름을 미리 알려준다.
 * - struct bio: 사용자 I/O 요청을 표현하는 기본 단위. blkcg_set_ioprio()는
 *   이 bio 안의 bi_ioprio 필드에 cgroup의 우선순위 클래스 값을 기록하며,
 *   그 값은 이후 request로 변환되는 과정에서 request->ioprio로 복사된다.
 */
struct request_queue;		/* [한국어] 불완전 타입 전방 선언 - request_queue의 전체 필드는 알 필요 없이 "이런 타입이 존재한다"만 컴파일러에 알려주는 선언. 이 헤더의 함수 시그니처에는 직접 쓰이지 않는다 */
struct bio;			/* [한국어] 불완전 타입 전방 선언 - blkcg_set_ioprio(struct bio *bio)의 매개변수 타입으로 쓰인다. bio의 내부 레이아웃(bi_ioprio 위치 등)은 include/linux/blk_types.h에 정의되어 있으며 이 헤더는 그것을 몰라도 된다 */

#ifdef CONFIG_BLK_CGROUP_IOPRIO	/* [한국어] blk-cgroup의 io.prio.class 정책 기능이 커널 설정(config)에서 활성화된 경우에만 진짜 구현으로 연결 - 꺼져 있으면 47번째 줄 #else 분기의 no-op이 대신 쓰인다 */
/*
 * [한국어]
 * blkcg_set_ioprio() - bio가 속한 blkcg(Block I/O cgroup)의 io.prio.class
 * 설정값을 읽어 bio->bi_ioprio에 반영(강제 기록)한다.
 *
 * @bio: 우선순위를 기록할 대상 bio. 아직 request로 승격되지 않은 상태여야
 *       하며, bio->bi_blkg(또는 bio_blkcg() 헬퍼)를 통해 소속 cgroup을 알아낼
 *       수 있어야 한다. 호출자가 유효한(널이 아닌) bio를 넘기는 것을 전제로
 *       하며, 이 함수는 널 체크를 하지 않는다(구현은 blk-ioprio.c 참고).
 * @return: 없음(void). 성공/실패를 구분해 반환하지 않는 이유는, 이 함수가
 *          "cgroup에 우선순위 정책이 설정되어 있으면 반영하고, 없으면 아무
 *          것도 하지 않는" best-effort(최선 노력) 방식으로 설계되어 있기
 *          때문이다 — 정책 부재는 에러가 아니라 정상적인 "기본값 유지" 경로다.
 *
 * 배경(Why): blkcg는 io.prio.class 파일을 통해 "이 cgroup에 속한 모든 I/O를
 * RT(Real Time)/BE(Best Effort)/IDLE 중 하나의 REQ_OP 우선순위 클래스로
 * 강제한다"는 정책을 사용자에게 노출한다. 그러나 그 정책이 cgroup 설정 파일
 * 수준에만 머물러 있으면 I/O 스케줄러나 하위 드라이버는 그 존재를 알 방법이
 * 없다. 이 함수는 그 정책을 실제 bio 데이터(bi_ioprio 필드)로 "번역"해 넣는
 * 유일한 지점이며, 이 번역이 없으면 io.prio.class는 아무 효과도 내지 못한다.
 * 동작 단계(What, 실제 구현은 blk-ioprio.c에 있으나 이 헤더의 호출 계약을
 * 기준으로 요약):
 *   1) bio가 속한 blkcg를 bio_blkcg(bio) 등으로 찾는다.
 *   2) 그 blkcg에 연결된 blk-ioprio 정책 데이터(prio_policy)를 조회한다.
 *      cgroup 계층 구조를 따라 상위로 올라가며 값을 상속받을 수 있다.
 *   3) 정책이 IOPRIO_CLASS_RT/BE/IDLE 중 하나로 설정되어 있으면 그 값으로
 *      bio->bi_ioprio를 덮어쓴다. 정책이 "설정 없음"이면 bio가 원래 갖고
 *      있던 우선순위(예: 태스크의 ioprio)를 그대로 둔다.
 * 실행 컨텍스트(Execution context): I/O를 제출한 프로세스(태스크) 컨텍스트에서
 * 동기적으로 실행되며, 인터럽트 컨텍스트나 별도 커널 스레드에서 호출되지
 * 않는다. 하나의 bio는 단일 제출 경로에서만 다뤄지므로 이 함수 자체가 bio를
 * 놓고 다른 스레드와 경쟁할 일은 없다(Synchronization) — 다만 내부에서
 * 조회하는 blkcg/정책 데이터 구조 자체의 동시 접근(다른 태스크가 동시에
 * io.prio.class를 쓰는 경우 등)에 대한 동기화는 blk-cgroup 코어의 RCU/참조
 * 카운트 규칙을 따른다(추정 — 이 헤더 범위 밖).
 * 호출자(Who calls): block/blk-core.c 또는 block/blk-mq.c의 bio 제출 초기
 * 경로(submit_bio_noacct_nocheck() 부근, 커널 버전에 따라 위치가 이동할 수
 * 있음 — 추정)에서, request로 변환되기 전에 호출된다.
 * 피호출자(Who is called): bio_blkcg(), blkg 조회 함수 등 blk-cgroup 인프라
 * 함수들을 내부적으로 호출한다. 실제 호출 목록은 blk-ioprio.c 구현에 있으며
 * 이 헤더에는 노출되지 않는다.
 * 에러 경로(Error path): 이 함수는 실패를 반환하지 않는 fail-open 설계다 —
 * cgroup/정책 조회에 실패하거나 정책 자체가 없으면 그냥 아무 것도 하지 않고
 * 반환한다. 즉 "정책 없음"과 "에러"를 구분하지 않는다.
 * 공유 상태(Shared state): bio->bi_ioprio(수정 대상)와, blkcg가 보유한
 * prio_policy(읽기 대상)가 이 함수를 통해 연결되는 공유 상태다.
 *
 * 호출 체인:
 *   submit_bio_noacct_nocheck() → [blkcg_set_ioprio] → (bio->bi_ioprio 갱신)
 *   → blk_mq_submit_bio() → blk_mq_get_request() (request->ioprio로 전파)
 *   → I/O 스케줄러(mq-deadline/bfq/kyber) 디스패치 순서 결정
 *   → (NVMe 드라이버 경로라면) nvme_queue_rq() → nvme_sq_copy_cmd → nvme_write_sq_db (SQ 기록 후 doorbell)
 */
void blkcg_set_ioprio(struct bio *bio);	/* [한국어] 실제 구현 선언 - block/blk-ioprio.c에 정의되어 있으며, 이 헤더를 include하는 쪽(예: block/blk-mq.c)은 링크 타임에 이 심볼을 연결받는다 */
#else
/*
 * [한국어]
 * blkcg_set_ioprio() (CONFIG_BLK_CGROUP_IOPRIO=n 대체 구현) - 아무 동작도
 * 하지 않는 빈 인라인 함수.
 *
 * @bio: 위쪽(§ifdef 분기) 실제 구현과 동일한 시그니처를 맞추기 위한 매개변수.
 *       이 구현에서는 실제로 읽거나 쓰지 않으므로 어떤 값이 들어와도(심지어
 *       극단적으로는 유효하지 않은 상태라도) 동작에 영향이 없다 — 다만
 *       호출자는 여전히 유효한 bio를 넘긴다고 가정하고 코드를 작성한다.
 * @return: 없음(void). 항상 즉시 반환하며 부작용이 전혀 없다.
 *
 * 배경(Why): CONFIG_BLK_CGROUP_IOPRIO가 꺼져 있으면 block/blk-ioprio.c 자체가
 * 빌드 대상에서 빠진다. 그런데 block/blk-mq.c 등 호출부는 커널 설정 여부와
 * 무관하게 동일한 소스 코드를 유지해야 하므로, 헤더 수준에서 "설정이 꺼졌을
 * 때는 이렇게 동작한다"는 대체 구현을 제공해 호출부마다 #ifdef를 반복하지
 * 않도록 한다 — 이는 리눅스 커널 전반에서 흔히 쓰이는 "config-off 시 no-op
 * 인라인 스텁" 관용구(idiom)다.
 * 동작(What): static inline이므로 컴파일러가 호출 지점에 그대로 펼쳐 넣을 수
 * 있고, 함수 바디가 완전히 비어 있으므로 최적화가 켜진 빌드에서는 호출 자체가
 * 통째로 사라진다(코드 크기/실행 비용 사실상 0).
 * 실행 컨텍스트(Execution context): 호출자와 동일한 컨텍스트에서 실행되며
 * (인라인이므로 별도의 문맥 전환이 없다), 아무 상태도 건드리지 않으므로
 * 별도의 동시성 이슈나 동기화(Synchronization) 요구가 존재하지 않는다.
 * 호출자(Who calls): CONFIG_BLK_CGROUP_IOPRIO=n으로 빌드된 커널에서, 위쪽
 * 실제 구현과 동일한 호출부(submit_bio_noacct_nocheck() 등)가 그대로 이
 * no-op 버전을 호출한다.
 * 피호출자(Who is called): 없음 — 함수 바디가 비어 있어 어떤 하위 함수도
 * 호출하지 않는다.
 * 에러 경로(Error path): 해당 없음 — 실패할 수 있는 동작 자체가 없다.
 * NVMe 관점: 이 분기가 선택된 빌드에서는 모든 bio가 cgroup과 무관하게 동일한
 * 기본 우선순위로 처리되며, NVMe 컨트롤러 쪽에서도 WRR(Weighted Round Robin)
 * Arbitration 클래스 힌트를 받지 못해 컨트롤러 기본 Arbitration 정책을 그대로
 * 따르게 된다(추정).
 *
 * 호출 체인:
 *   submit_bio_noacct_nocheck() → [blkcg_set_ioprio (no-op)] → (bio 변경 없음)
 *   → blk_mq_submit_bio()로 그대로 진행 (하위 스케줄러/NVMe 드라이버는 cgroup
 *     우선순위 힌트 없이 기본 정책으로 동작)
 */
static inline void blkcg_set_ioprio(struct bio *bio)	/* [한국어] 대체(fallback) 정의 - 매개변수 bio는 시그니처 호환을 위해서만 존재하며 함수 내부에서 실제로 참조되지 않는다 */
{
}				/* [한국어] 의도적인 빈 함수 바디 - bio->bi_ioprio를 포함해 어떤 상태도 바꾸지 않는 명시적 no-op이며, 컴파일러 최적화 시 호출부에서 완전히 제거될 수 있다 */
#endif				/* [한국어] CONFIG_BLK_CGROUP_IOPRIO 분기 종료 - 이 지점까지가 "정책 있음/없음" 두 구현 중 하나를 고르는 조건부 컴파일 영역이다 */

/*
 * [한국어] 부록: 한눈에 보는 요약(TL;DR)
 *
 * - 이 헤더가 세상에 노출하는 것은 blkcg_set_ioprio(struct bio *) 단 하나뿐이다.
 * - CONFIG_BLK_CGROUP_IOPRIO=y: block/blk-ioprio.c의 실제 구현이 cgroup의
 *   io.prio.class 값을 bio->bi_ioprio에 반영한다.
 * - CONFIG_BLK_CGROUP_IOPRIO=n: 이 헤더 안의 빈 인라인 함수가 대신 쓰이고,
 *   모든 bio는 cgroup과 무관한 기본 우선순위로 처리된다.
 * - 이 값은 request->ioprio로 전파되어 mq-deadline/bfq/kyber 등 I/O
 *   스케줄러의 디스패치 순서에 영향을 주고, NVMe 드라이버까지 내려가면
 *   컨트롤러의 WRR(Weighted Round Robin) Arbitration 힌트로 이어질 수도
 *   있다(추정) — 다만 이는 NVMe 자체의 커맨드 우선순위 메커니즘과는 별개
 *   계층이다.
 */

#endif /* _BLK_IOPRIO_H_ */	/* [한국어] 헤더 가드 종료 - 14번째 줄 #ifndef에서 시작한 중복 include 방지 블록이 여기서 끝난다 */
