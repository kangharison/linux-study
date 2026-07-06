// SPDX-License-Identifier: GPL-2.0
/*
 * Bad block management
 *
 * - Heavily based on MD badblocks code from Neil Brown
 *
 * Copyright (c) 2015, Intel Corporation.
 */

/*
 * [한국어 설명] 범용 불량 블록(bad block) 관리 엔진 (badblocks.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 특정 하드웨어(NVMe/SATA/SCSI 등)에 종속되지 않는, 블록 계층
 * (block layer) 공용의 "불량 섹터 소프트 추적" 엔진이다. struct badblocks
 * 하나가 디스크(또는 RAID 멤버 디스크) 한 개에 대응하며, 그 안의 page
 * 필드는 PAGE_SIZE 바이트를 8바이트(u64) 단위로 쪼갠 최대 MAX_BADBLOCKS
 * (= PAGE_SIZE/8)개의 "불량 구간(bad extent)" 슬롯 배열이다. 각 슬롯은
 * BB_MAKE()/BB_OFFSET()/BB_LEN()/BB_ACK()/BB_END() 매크로(정의는
 * include/linux/badblocks.h)로 (시작 섹터, 길이 1~512, ack 1비트)를 u64
 * 하나에 압축 인코딩한다. badblocks_check()는 이진 탐색으로 주어진 [s,
 * s+sectors) 범위가 이 배열과 겹치는지 판정하고, badblocks_set()/
 * badblocks_clear()는 새 불량 구간을 등록/해제하면서 기존 구간과의 병합
 * (merge)·덮어쓰기(overwrite)·분할(split)을 상황별로 처리한다. 커널이
 * 하드웨어 재매핑(bad sector remapping)을 신뢰할 수 없거나, RAID처럼
 * 여러 디스크 중 특정 섹터만 우회해야 하는 상위 계층에 "소프트웨어가
 * 관리하는 불량 목록"을 제공하는 것이 이 파일의 존재 이유다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 이 트리에서 실제로 확인되는 호출자는 두 곳이다.
 *   1) block/genhd.c: struct gendisk의 선택적 필드 `struct badblocks *bb`
 *      (include/linux/blkdev.h 참고)가 설정되어 있으면,
 *      /sys/block/<disk>/badblocks sysfs 속성의 읽기/쓰기 핸들러인
 *      disk_badblocks_show()/disk_badblocks_store()가 이 파일의
 *      badblocks_show()/badblocks_store()를 그대로 호출한다. 이 경로는
 *      드라이버 종류와 무관한 범용 sysfs 인터페이스다.
 *   2) drivers/block/null_blk (main.c, zoned.c): 테스트/벤치마크용 가상
 *      블록 드라이버인 null_blk가 디바이스별로 struct badblocks를 내장
 *      (badblocks_init())하고, I/O 처리 경로(null_handle_badblocks())에서
 *      badblocks_check()로 특정 LBA를 의도적으로 실패시켜 상위 계층(파일
 *      시스템, MD, device-mapper)의 오류 처리 경로를 검증하는 데 쓴다.
 *      configfs를 통한 badblocks_store 속성 쓰기로 badblocks_set()/
 *      badblocks_clear()가 호출되고, 디바이스 해제 시 badblocks_exit()가
 *      호출된다.
 *   3) (이 스터디 트리에는 소스가 포함되어 있지 않은 범용 커널 지식) 실제
 *      리눅스에서 이 엔진의 가장 대표적인 사용처는 MD RAID이다.
 *      struct md_rdev(drivers/md/md.h)의 badblocks 필드가 RAID1/RAID10/
 *      RAID5/6의 멤버 디스크마다 존재하여, 특정 멤버에서만 발생한 불량
 *      섹터를 기록해두고 재동기화(resync)·재구성(rebuild) 시 해당 섹터를
 *      우회하거나 다른 미러/패리티로부터 복구한다.
 *   grep으로 확인한 결과 drivers/nvme/host/ 트리는 badblocks_* 심볼을
 *   전혀 참조하지 않는다 — 즉 이 파일은 NVMe 미디어 오류·SQ/CQ·PRP/SGL과
 *   직접적인 연결이 없다(이전 버전 주석의 "NVMe 관점" 서술은 부정확했다).
 *   실행 컨텍스트 측면에서 badblocks_check()는 I/O 완료 콜백(bi_end_io류)
 *   등 지연에 민감한 빠른 경로에서 호출될 수 있어 seqlock의 읽기 측
 *   (read_seqbegin/read_seqretry, 락 없는 낙관적 읽기)을 사용하고,
 *   badblocks_set()/_clear()/sysfs store는 드물게 호출되는 관리 경로이므로
 *   seqlock의 쓰기 측(write_seqlock_irq(save)/write_sequnlock_irq(restore))
 *   을 사용해 인터럽트 컨텍스트와의 경합도 배제한다.
 *
 * === 타 모듈과의 연결 ===
 * 의존하는 모듈: linux/seqlock.h(seqlock_t와 read/write seqlock API 제공,
 * 이 파일 동시성의 핵심), linux/device.h(devm_kzalloc/devm_kfree — device
 * 생명주기에 종속된 메모리 관리), linux/slab.h(kzalloc/kfree — device가
 * 없는 badblocks_init() 경로), linux/badblocks.h(struct badblocks/
 * struct badblocks_context 정의와 BB_* 매크로, badblocks_full/_empty/
 * set_changed/clear_changed 인라인 헬퍼가 여기 선언됨).
 * 이 파일에 의존하는 모듈: block/genhd.c(sysfs), drivers/block/null_blk
 * (fault injection), 그리고 범용 지식상 MD RAID.
 * 데이터 흐름: 호출자가 넘긴 (시작 섹터 s, 길이 sectors)가 badblocks_set()/
 * _clear()/_check() 공개 API로 진입 → 내부 정적 함수 _badblocks_set/
 * _clear/_check가 struct badblocks_context에 "이번 루프에서 처리할 부분
 * 범위"를 담아 prev_badblocks()로 기준 슬롯을 찾고 → can_merge_front/
 * can_combine_front/can_front_overwrite/overlap_front/overlap_behind 등의
 * 판정 함수로 상황을 분류 → front_merge/front_combine/front_overwrite/
 * insert_at/front_clear/front_splitting_clear 등으로 bb->page 배열을
 * 실제로 갱신 → 결과(bool 성공 여부, 또는 -1/0/1 판정과 first_bad/
 * bad_sectors out-파라미터)가 호출자에게 반환된다.
 * 공유 핵심 자료구조: struct badblocks(디스크 1개당 1개, page 배열이
 * 실제 상태)와 struct badblocks_context(단일 API 호출 내에서 "현재 처리
 * 중인 부분 범위"를 함수 간에 전달하는 임시 컨테이너, 영속 상태 아님).
 *
 * === 주요 함수/구조체 요약 ===
 * - badblocks_check()/_badblocks_check(): [s,s+sectors) 범위가 bad table과
 *   겹치는지 이진 탐색으로 판정. 0=불량 없음, 1=겹치지만 모두 acked,
 *   -1=unacked 불량 포함. seqlock 읽기 측 사용, 결과 불변 시까지 retry.
 * - badblocks_set()/_badblocks_set(): 범위를 등록. 파일 하단의 방대한
 *   영어 주석(아래)에 설명된 merge/overwrite/split 규칙에 따라 처리하며,
 *   테이블이 가득 차면 try_adjacent_combine()으로 공간을 회수해 재시도.
 * - badblocks_clear()/_badblocks_clear(): 범위를 해제. front_clear()(머리/
 *   꼬리 절단 또는 전체 삭제)와 front_splitting_clear()(중앙 절단 시 항목
 *   을 둘로 분할)로 나뉘어 처리.
 * - prev_badblocks()/prev_by_hint(): "시작 섹터가 s 이하인 마지막 항목의
 *   인덱스"를 찾는 공통 탐색 헬퍼. 직전 루프의 인덱스를 hint로 주면 짧은
 *   선형 탐색으로 끝내고, 실패하면 O(log n) 이진 탐색으로 폴백한다.
 * - badblocks_init()/devm_init_badblocks()/badblocks_exit(): page 배열을
 *   위한 메모리 할당/해제와 seqlock 초기화. devm_* 변형은 소유 device의
 *   생명주기에 맞춰 자동 해제된다.
 *
 * - struct badblocks (정의는 include/linux/badblocks.h, 이 파일에서는
 *   포인터로만 사용):
 *     struct device *dev;
 *       이 badblocks 인스턴스가 devm_init_badblocks()로 초기화됐을 때
 *       그 device를 가리킨다. 설정자: __badblocks_init(). 읽는 자:
 *       badblocks_exit()가 devm_kfree(dev, page) 대 kfree(page)를
 *       선택하는 기준으로 사용. 값 범위: device-managed 경로면 유효한
 *       struct device*, badblocks_init()(비-devm 경로)이면 NULL.
 *       동기화: 초기화 시 한 번만 쓰이고 이후 읽기 전용이라 별도 락 불요.
 *     int count;
 *       현재 bad table에 채워진 슬롯 개수(0..MAX_BADBLOCKS). 설정자:
 *       insert_at/front_clear/front_splitting_clear/front_combine/
 *       try_adjacent_combine 등 배열을 넣거나 빼는 모든 헬퍼가 ++/--.
 *       읽는 자: badblocks_full()/badblocks_empty(), 모든 탐색·삽입
 *       루프의 상한. 값 범위: 0 이상 MAX_BADBLOCKS 이하. 동기화: 쓰기는
 *       항상 write_seqlock 보유 중에만 일어나고, 읽기는 seqlock read
 *       측 재시도 루프(read_seqretry)로 일관성을 확인한다.
 *     int unacked_exist;
 *       테이블에 아직 상위 계층이 확인(acknowledge)하지 않은 불량 구간이
 *       "있을 수도 있다"는 힌트 플래그(정확한 카운트가 아니라 보수적
 *       상한선). 설정자: _badblocks_set()이 unacked 구간을 새로 등록할
 *       때 1로 세팅; badblocks_update_acked()/badblocks_show()가 실제로
 *       스캔해 없음을 확인하면 0으로 내림. 읽는 자: badblocks_show()가
 *       unack 전용 조회 시 참고. 값 범위: 0/1. 동기화: write_seqlock
 *       보유 구간에서만 변경.
 *     int shift;
 *       섹터 단위 LBA를 bad table 슬롯 단위로 정렬하기 위한 좌측 시프트
 *       값. 0이면 섹터=슬롯 단위(정렬 없음), 양수면 2^shift 섹터 단위로
 *       올림/내림 정렬해 더 큰 디스크를 같은 512슬롯으로 커버, 음수이면
 *       "이 badblocks 인스턴스 자체가 비활성화"를 의미한다. 설정자:
 *       __badblocks_init()(0 또는 -1), cmpxchg로 호출자가 활성화하는
 *       경우도 있음(예: null_blk). 읽는 자: 거의 모든 공개 API가 진입
 *       시 shift < 0을 검사해 조기 반환. 값 범위: -1(비활성) 또는 0
 *       이상. 동기화: 별도 락 없이 초기화 시 한 번 결정되는 것이 일반적.
 *     u64 *page;
 *       실제 bad table 저장소. PAGE_SIZE 바이트를 devm_kzalloc/kzalloc으로
 *       할당하고 u64 슬롯 배열로 사용(BB_MAKE로 인코딩된 항목이 시작
 *       섹터 오름차순으로 정렬 유지됨 — 이진 탐색의 전제조건). 설정자:
 *       __badblocks_init()이 할당, badblocks_exit()이 해제 후 NULL.
 *       읽는 자/쓰는 자: 이 파일의 거의 모든 함수. 값 범위: 유효 포인터
 *       또는 (미초기화/해제 후) NULL. 동기화: seqlock으로 보호된 임계
 *       구역 내에서만 변경(쓰기 측) 또는 재시도 루프 내에서 읽음(읽기 측).
 *     int changed;
 *       마지막으로 확인된 이후 테이블 내용이 바뀌었음을 나타내는 플래그
 *       (예: MD 등 호출자가 영속 메타데이터에 flush해야 함을 인지하는
 *       용도). 설정자: set_changed()(테이블을 수정하는 모든 경로),
 *       ack_all_badblocks() 진입 조건으로도 검사됨. 읽는 자: 이 파일
 *       외부의 호출자(clear_changed() 등 인라인 헬퍼, badblocks.h).
 *       값 범위: 0/1. 동기화: write_seqlock 구간에서 설정.
 *     seqlock_t lock;
 *       이 인스턴스 전체를 보호하는 시퀀스 락. "쓰기는 드물고 읽기(I/O
 *       완료 경로)는 잦다"는 이 엔진의 접근 패턴에 맞춰, 읽기 측은 락을
 *       걸지 않고 시퀀스 번호만 비교해 재시도하는 낙관적 방식이다.
 *       설정자: seqlock_init()(__badblocks_init()에서 1회). 읽는 자/
 *       쓰는 자: badblocks_check()가 read_seqbegin/read_seqretry, 그
 *       외 모든 수정 함수가 write_seqlock_irq(save)/
 *       write_sequnlock_irq(restore). 동기화: 바로 이 필드 자체가
 *       동기화 메커니즘이다.
 *     sector_t sector;
 *     sector_t size;
 *       (이 파일 안에서는 참조되지 않음 — grep으로 확인) badblocks.h에
 *       선언은 되어 있으나 badblocks.c의 어떤 함수도 bb->sector/bb->size
 *       를 읽거나 쓰지 않는다. 호출자가 자신의 문맥(예: 이 badblocks
 *       인스턴스가 커버하는 디스크 상의 시작 위치/크기)을 기록해두는
 *       용도로 남겨진 것으로 보이며, 이 파일의 알고리즘에는 영향이
 *       없다.
 *
 * - struct badblocks_context (정의는 include/linux/badblocks.h, 이 파일
 *   에서 실제 정의를 사용하는 지역/스택 변수로 인스턴스화됨):
 *     sector_t start;
 *       현재 진행 중인 set/clear/check 루프에서 "이번 한 번의 반복이
 *       다루는 부분 범위"의 시작 섹터. 설정자: _badblocks_set/_clear/
 *       _check의 re_insert/re_clear/re_check 레이블 진입부에서 매
 *       반복마다 s 값으로 갱신. 읽는 자: overlap_front/overlap_behind/
 *       can_merge_front/can_front_overwrite 등 모든 판정 헬퍼가 bad->start
 *       로 참조. 값 범위: 원래 요청 범위 [s, s+sectors) 내부, 반복이
 *       진행될수록 증가. 동기화: 호출 스레드의 스택 지역 변수이므로
 *       동시성 보호 불필요(호출자가 이미 seqlock을 잡고 있음).
 *     sector_t len;
 *       위 start로부터 이번 반복에서 처리하려는 길이(섹터 수). 판정
 *       함수들이 상황에 맞춰 이 값을 더 작게 줄인 뒤(예: 다음 불량
 *       항목과 겹치지 않도록 제한) 실제 처리 함수(insert_at 등)에
 *       넘긴다. 설정자/읽는 자: start와 동일한 함수 집합. 값 범위:
 *       1 이상, 원래 남은 sectors 이하. 동기화: start와 동일(지역
 *       변수).
 *     int ack;
 *       이번 반복에서 적용할 acknowledged 플래그(호출자가 badblocks_set
 *       ()에 넘긴 acknowledged 인자, 또는 badblocks_clear()에서는 항상
 *       true로 고정). 설정자: _badblocks_set/_clear 진입부에서 1회
 *       고정. 읽는 자: can_merge_front/can_combine_front/
 *       can_front_overwrite 등이 기존 슬롯의 BB_ACK()와 비교해 병합/
 *       덮어쓰기 가능 여부를 판단하는 핵심 조건. 값 범위: 0(unacked)
 *       또는 1(acked)의 bool 의미. 동기화: 지역 변수.
 *
 * (참고) badblocks.h에 정의된 BB_* 매크로:
 *   BB_OFFSET(x): 슬롯 x의 시작 섹터를 추출. BB_LEN(x): 슬롯 x의 길이
 *   (1~512)를 추출. BB_ACK(x): ack 비트(0/1) 추출. BB_END(x): 시작+길이
 *   = 끝 섹터(배타적 상한). BB_MAKE(a,l,ack): (시작 a, 길이 l, ack)를
 *   한 u64 슬롯으로 인코딩. BB_MAX_LEN(=512): 슬롯 하나가 표현 가능한
 *   최대 길이 — 이보다 긴 범위는 여러 슬롯으로 쪼개 저장해야 한다.
 */

#include <linux/badblocks.h> /* struct badblocks/struct badblocks_context 정의, BB_* 인코딩 매크로, badblocks_full/_empty/set_changed/clear_changed 인라인 헬퍼 제공 — 이 파일의 자료구조 계약 자체 */
#include <linux/seqlock.h> /* seqlock_t와 read_seqbegin/read_seqretry(읽기 측), write_seqlock_irq(save)/write_sequnlock_irq(restore)(쓰기 측) — 이 파일 동시성의 근간 */
#include <linux/device.h> /* struct device, devm_kzalloc/devm_kfree — device 생명주기에 종속된 bad table 메모리 관리(devm_init_badblocks 경로) */
#include <linux/kernel.h> /* min_t 등 커널 공통 매크로 — 범위 클램핑 계산(front_merge/insert_at 등)에 사용 */
#include <linux/module.h> /* EXPORT_SYMBOL_GPL — 이 파일의 공개 API를 다른 모듈(드라이버)에서 링크해 쓸 수 있도록 심볼 노출 */
#include <linux/stddef.h> /* NULL 등 표준 정의 — 포인터 유효성 검사에 사용 */
#include <linux/types.h> /* sector_t, u64 등 커널 고정 크기/도메인 타입 — 섹터 주소와 bad table 슬롯 표현에 사용 */
#include <linux/slab.h> /* kzalloc/kfree — device가 없는(비-devm) badblocks_init() 경로에서 bad table 메모리 할당/해제 */

/*
 * The purpose of badblocks set/clear is to manage bad blocks ranges which are
 * identified by LBA addresses.
 *
 * When the caller of badblocks_set() wants to set a range of bad blocks, the
 * setting range can be acked or unacked. And the setting range may merge,
 * overwrite, skip the overlapped already set range, depends on who they are
 * overlapped or adjacent, and the acknowledgment type of the ranges. It can be
 * more complicated when the setting range covers multiple already set bad block
 * ranges, with restrictions of maximum length of each bad range and the bad
 * table space limitation.
 *
 * It is difficult and unnecessary to take care of all the possible situations,
 * for setting a large range of bad blocks, we can handle it by dividing the
 * large range into smaller ones when encounter overlap, max range length or
 * bad table full conditions. Every time only a smaller piece of the bad range
 * is handled with a limited number of conditions how it is interacted with
 * possible overlapped or adjacent already set bad block ranges. Then the hard
 * complicated problem can be much simpler to handle in proper way.
 *
 * When setting a range of bad blocks to the bad table, the simplified situations
 * to be considered are, (The already set bad blocks ranges are naming with
 *  prefix E, and the setting bad blocks range is naming with prefix S)
 *
 * 1) A setting range is not overlapped or adjacent to any other already set bad
 *    block range.
 *                         +--------+
 *                         |    S   |
 *                         +--------+
 *        +-------------+               +-------------+
 *        |      E1     |               |      E2     |
 *        +-------------+               +-------------+
 *    For this situation if the bad blocks table is not full, just allocate a
 *    free slot from the bad blocks table to mark the setting range S. The
 *    result is,
 *        +-------------+  +--------+   +-------------+
 *        |      E1     |  |    S   |   |      E2     |
 *        +-------------+  +--------+   +-------------+
 * 2) A setting range starts exactly at a start LBA of an already set bad blocks
 *    range.
 * 2.1) The setting range size < already set range size
 *        +--------+
 *        |    S   |
 *        +--------+
 *        +-------------+
 *        |      E      |
 *        +-------------+
 * 2.1.1) If S and E are both acked or unacked range, the setting range S can
 *    be merged into existing bad range E. The result is,
 *        +-------------+
 *        |      S      |
 *        +-------------+
 * 2.1.2) If S is unacked setting and E is acked, the setting will be denied, and
 *    the result is,
 *        +-------------+
 *        |      E      |
 *        +-------------+
 * 2.1.3) If S is acked setting and E is unacked, range S can overwrite on E.
 *    An extra slot from the bad blocks table will be allocated for S, and head
 *    of E will move to end of the inserted range S. The result is,
 *        +--------+----+
 *        |    S   | E  |
 *        +--------+----+
 * 2.2) The setting range size == already set range size
 * 2.2.1) If S and E are both acked or unacked range, the setting range S can
 *    be merged into existing bad range E. The result is,
 *        +-------------+
 *        |      S      |
 *        +-------------+
 * 2.2.2) If S is unacked setting and E is acked, the setting will be denied, and
 *    the result is,
 *        +-------------+
 *        |      E      |
 *        +-------------+
 * 2.2.3) If S is acked setting and E is unacked, range S can overwrite all of
      bad blocks range E. The result is,
 *        +-------------+
 *        |      S      |
 *        +-------------+
 * 2.3) The setting range size > already set range size
 *        +-------------------+
 *        |          S        |
 *        +-------------------+
 *        +-------------+
 *        |      E      |
 *        +-------------+
 *    For such situation, the setting range S can be treated as two parts, the
 *    first part (S1) is as same size as the already set range E, the second
 *    part (S2) is the rest of setting range.
 *        +-------------+-----+        +-------------+       +-----+
 *        |    S1       | S2  |        |     S1      |       | S2  |
 *        +-------------+-----+  ===>  +-------------+       +-----+
 *        +-------------+              +-------------+
 *        |      E      |              |      E      |
 *        +-------------+              +-------------+
 *    Now we only focus on how to handle the setting range S1 and already set
 *    range E, which are already explained in 2.2), for the rest S2 it will be
 *    handled later in next loop.
 * 3) A setting range starts before the start LBA of an already set bad blocks
 *    range.
 *        +-------------+
 *        |      S      |
 *        +-------------+
 *             +-------------+
 *             |      E      |
 *             +-------------+
 *    For this situation, the setting range S can be divided into two parts, the
 *    first (S1) ends at the start LBA of already set range E, the second part
 *    (S2) starts exactly at a start LBA of the already set range E.
 *        +----+---------+             +----+      +---------+
 *        | S1 |    S2   |             | S1 |      |    S2   |
 *        +----+---------+      ===>   +----+      +---------+
 *             +-------------+                     +-------------+
 *             |      E      |                     |      E      |
 *             +-------------+                     +-------------+
 *    Now only the first part S1 should be handled in this loop, which is in
 *    similar condition as 1). The rest part S2 has exact same start LBA address
 *    of the already set range E, they will be handled in next loop in one of
 *    situations in 2).
 * 4) A setting range starts after the start LBA of an already set bad blocks
 *    range.
 * 4.1) If the setting range S exactly matches the tail part of already set bad
 *    blocks range E, like the following chart shows,
 *            +---------+
 *            |   S     |
 *            +---------+
 *        +-------------+
 *        |      E      |
 *        +-------------+
 * 4.1.1) If range S and E have same acknowledge value (both acked or unacked),
 *    they will be merged into one, the result is,
 *        +-------------+
 *        |      S      |
 *        +-------------+
 * 4.1.2) If range E is acked and the setting range S is unacked, the setting
 *    request of S will be rejected, the result is,
 *        +-------------+
 *        |      E      |
 *        +-------------+
 * 4.1.3) If range E is unacked, and the setting range S is acked, then S may
 *    overwrite the overlapped range of E, the result is,
 *        +---+---------+
 *        | E |    S    |
 *        +---+---------+
 * 4.2) If the setting range S stays in middle of an already set range E, like
 *    the following chart shows,
 *             +----+
 *             | S  |
 *             +----+
 *        +--------------+
 *        |       E      |
 *        +--------------+
 * 4.2.1) If range S and E have same acknowledge value (both acked or unacked),
 *    they will be merged into one, the result is,
 *        +--------------+
 *        |       S      |
 *        +--------------+
 * 4.2.2) If range E is acked and the setting range S is unacked, the setting
 *    request of S will be rejected, the result is also,
 *        +--------------+
 *        |       E      |
 *        +--------------+
 * 4.2.3) If range E is unacked, and the setting range S is acked, then S will
 *    inserted into middle of E and split previous range E into two parts (E1
 *    and E2), the result is,
 *        +----+----+----+
 *        | E1 |  S | E2 |
 *        +----+----+----+
 * 4.3) If the setting bad blocks range S is overlapped with an already set bad
 *    blocks range E. The range S starts after the start LBA of range E, and
 *    ends after the end LBA of range E, as the following chart shows,
 *            +-------------------+
 *            |          S        |
 *            +-------------------+
 *        +-------------+
 *        |      E      |
 *        +-------------+
 *    For this situation the range S can be divided into two parts, the first
 *    part (S1) ends at end range E, and the second part (S2) has rest range of
 *    origin S.
 *            +---------+---------+            +---------+      +---------+
 *            |    S1   |    S2   |            |    S1   |      |    S2   |
 *            +---------+---------+  ===>      +---------+      +---------+
 *        +-------------+                  +-------------+
 *        |      E      |                  |      E      |
 *        +-------------+                  +-------------+
 *     Now in this loop the setting range S1 and already set range E can be
 *     handled as the situations 4.1), the rest range S2 will be handled in next
 *     loop and ignored in this loop.
 * 5) A setting bad blocks range S is adjacent to one or more already set bad
 *    blocks range(s), and they are all acked or unacked range.
 * 5.1) Front merge: If the already set bad blocks range E is before setting
 *    range S and they are adjacent,
 *                +------+
 *                |  S   |
 *                +------+
 *        +-------+
 *        |   E   |
 *        +-------+
 * 5.1.1) When total size of range S and E <= BB_MAX_LEN, and their acknowledge
 *    values are same, the setting range S can front merges into range E. The
 *    result is,
 *        +--------------+
 *        |       S      |
 *        +--------------+
 * 5.1.2) Otherwise these two ranges cannot merge, just insert the setting
 *    range S right after already set range E into the bad blocks table. The
 *    result is,
 *        +--------+------+
 *        |   E    |   S  |
 *        +--------+------+
 * 6) Special cases which above conditions cannot handle
 * 6.1) Multiple already set ranges may merge into less ones in a full bad table
 *        +-------------------------------------------------------+
 *        |                           S                           |
 *        +-------------------------------------------------------+
 *        |<----- BB_MAX_LEN ----->|
 *                                 +-----+     +-----+   +-----+
 *                                 | E1  |     | E2  |   | E3  |
 *                                 +-----+     +-----+   +-----+
 *     In the above example, when the bad blocks table is full, inserting the
 *     first part of setting range S will fail because no more available slot
 *     can be allocated from bad blocks table. In this situation a proper
 *     setting method should be go though all the setting bad blocks range and
 *     look for chance to merge already set ranges into less ones. When there
 *     is available slot from bad blocks table, re-try again to handle more
 *     setting bad blocks ranges as many as possible.
 *        +------------------------+
 *        |          S3            |
 *        +------------------------+
 *        |<----- BB_MAX_LEN ----->|
 *                                 +-----+-----+-----+---+-----+--+
 *                                 |       S1        |     S2     |
 *                                 +-----+-----+-----+---+-----+--+
 *     The above chart shows although the first part (S3) cannot be inserted due
 *     to no-space in bad blocks table, but the following E1, E2 and E3 ranges
 *     can be merged with rest part of S into less range S1 and S2. Now there is
 *     1 free slot in bad blocks table.
 *        +------------------------+-----+-----+-----+---+-----+--+
 *        |           S3           |       S1        |     S2     |
 *        +------------------------+-----+-----+-----+---+-----+--+
 *     Since the bad blocks table is not full anymore, re-try again for the
 *     origin setting range S. Now the setting range S3 can be inserted into the
 *     bad blocks table with previous freed slot from multiple ranges merge.
 * 6.2) Front merge after overwrite
 *    In the following example, in bad blocks table, E1 is an acked bad blocks
 *    range and E2 is an unacked bad blocks range, therefore they are not able
 *    to merge into a larger range. The setting bad blocks range S is acked,
 *    therefore part of E2 can be overwritten by S.
 *                      +--------+
 *                      |    S   |                             acknowledged
 *                      +--------+                         S:       1
 *              +-------+-------------+                   E1:       1
 *              |   E1  |    E2       |                   E2:       0
 *              +-------+-------------+
 *     With previous simplified routines, after overwriting part of E2 with S,
 *     the bad blocks table should be (E3 is remaining part of E2 which is not
 *     overwritten by S),
 *                                                             acknowledged
 *              +-------+--------+----+                    S:       1
 *              |   E1  |    S   | E3 |                   E1:       1
 *              +-------+--------+----+                   E3:       0
 *     The above result is correct but not perfect. Range E1 and S in the bad
 *     blocks table are all acked, merging them into a larger one range may
 *     occupy less bad blocks table space and make badblocks_check() faster.
 *     Therefore in such situation, after overwriting range S, the previous range
 *     E1 should be checked for possible front combination. Then the ideal
 *     result can be,
 *              +----------------+----+                        acknowledged
 *              |       E1       | E3 |                   E1:       1
 *              +----------------+----+                   E3:       0
 * 6.3) Behind merge: If the already set bad blocks range E is behind the setting
 *    range S and they are adjacent. Normally we don't need to care about this
 *    because front merge handles this while going though range S from head to
 *    tail, except for the tail part of range S. When the setting range S are
 *    fully handled, all the above simplified routine doesn't check whether the
 *    tail LBA of range S is adjacent to the next already set range and not
 *    merge them even it is possible.
 *        +------+
 *        |  S   |
 *        +------+
 *               +-------+
 *               |   E   |
 *               +-------+
 *    For the above special situation, when the setting range S are all handled
 *    and the loop ends, an extra check is necessary for whether next already
 *    set range E is right after S and mergeable.
 * 6.3.1) When total size of range E and S <= BB_MAX_LEN, and their acknowledge
 *    values are same, the setting range S can behind merges into range E. The
 *    result is,
 *        +--------------+
 *        |       S      |
 *        +--------------+
 * 6.3.2) Otherwise these two ranges cannot merge, just insert the setting range
 *     S in front of the already set range E in the bad blocks table. The result
 *     is,
 *        +------+-------+
 *        |  S   |   E   |
 *        +------+-------+
 *
 * All the above 5 simplified situations and 3 special cases may cover 99%+ of
 * the bad block range setting conditions. Maybe there is some rare corner case
 * is not considered and optimized, it won't hurt if badblocks_set() fails due
 * to no space, or some ranges are not merged to save bad blocks table space.
 *
 * Inside badblocks_set() each loop starts by jumping to re_insert label, every
 * time for the new loop prev_badblocks() is called to find an already set range
 * which starts before or at current setting range. Since the setting bad blocks
 * range is handled from head to tail, most of the cases it is unnecessary to do
 * the binary search inside prev_badblocks(), it is possible to provide a hint
 * to prev_badblocks() for a fast path, then the expensive binary search can be
 * avoided. In my test with the hint to prev_badblocks(), except for the first
 * loop, all rested calls to prev_badblocks() can go into the fast path and
 * return correct bad blocks table index immediately.
 *
 *
 * Clearing a bad blocks range from the bad block table has similar idea as
 * setting does, but much more simpler. The only thing needs to be noticed is
 * when the clearing range hits middle of a bad block range, the existing bad
 * block range will split into two, and one more item should be added into the
 * bad block table. The simplified situations to be considered are, (The already
 * set bad blocks ranges in bad block table are naming with prefix E, and the
 * clearing bad blocks range is naming with prefix C)
 *
 * 1) A clearing range is not overlapped to any already set ranges in bad block
 *    table.
 *    +-----+         |          +-----+         |          +-----+
 *    |  C  |         |          |  C  |         |          |  C  |
 *    +-----+         or         +-----+         or         +-----+
 *            +---+   |   +----+         +----+  |  +---+
 *            | E |   |   | E1 |         | E2 |  |  | E |
 *            +---+   |   +----+         +----+  |  +---+
 *    For the above situations, no bad block to be cleared and no failure
 *    happens, simply returns 0.
 * 2) The clearing range hits middle of an already setting bad blocks range in
 *    the bad block table.
 *            +---+
 *            | C |
 *            +---+
 *     +-----------------+
 *     |         E       |
 *     +-----------------+
 *    In this situation if the bad block table is not full, the range E will be
 *    split into two ranges E1 and E2. The result is,
 *     +------+   +------+
 *     |  E1  |   |  E2  |
 *     +------+   +------+
 * 3) The clearing range starts exactly at same LBA as an already set bad block range
 *    from the bad block table.
 * 3.1) Partially covered at head part
 *         +------------+
 *         |     C      |
 *         +------------+
 *         +-----------------+
 *         |         E       |
 *         +-----------------+
 *    For this situation, the overlapped already set range will update the
 *    start LBA to end of C and shrink the range to BB_LEN(E) - BB_LEN(C). No
 *    item deleted from bad block table. The result is,
 *                      +----+
 *                      | E1 |
 *                      +----+
 * 3.2) Exact fully covered
 *         +-----------------+
 *         |         C       |
 *         +-----------------+
 *         +-----------------+
 *         |         E       |
 *         +-----------------+
 *    For this situation the whole bad blocks range E will be cleared and its
 *    corresponded item is deleted from the bad block table.
 * 4) The clearing range exactly ends at same LBA as an already set bad block
 *    range.
 *                   +-------+
 *                   |   C   |
 *                   +-------+
 *         +-----------------+
 *         |         E       |
 *         +-----------------+
 *    For the above situation, the already set range E is updated to shrink its
 *    end to the start of C, and reduce its length to BB_LEN(E) - BB_LEN(C).
 *    The result is,
 *         +---------+
 *         |    E    |
 *         +---------+
 * 5) The clearing range is partially overlapped with an already set bad block
 *    range from the bad block table.
 * 5.1) The already set bad block range is front overlapped with the clearing
 *    range.
 *         +----------+
 *         |     C    |
 *         +----------+
 *              +------------+
 *              |      E     |
 *              +------------+
 *   For such situation, the clearing range C can be treated as two parts. The
 *   first part ends at the start LBA of range E, and the second part starts at
 *   same LBA of range E.
 *         +----+-----+               +----+   +-----+
 *         | C1 | C2  |               | C1 |   | C2  |
 *         +----+-----+         ===>  +----+   +-----+
 *              +------------+                 +------------+
 *              |      E     |                 |      E     |
 *              +------------+                 +------------+
 *   Now the first part C1 can be handled as condition 1), and the second part C2 can be
 *   handled as condition 3.1) in next loop.
 * 5.2) The already set bad block range is behind overlaopped with the clearing
 *   range.
 *                 +----------+
 *                 |     C    |
 *                 +----------+
 *         +------------+
 *         |      E     |
 *         +------------+
 *   For such situation, the clearing range C can be treated as two parts. The
 *   first part C1 ends at same end LBA of range E, and the second part starts
 *   at end LBA of range E.
 *                 +----+-----+                 +----+    +-----+
 *                 | C1 | C2  |                 | C1 |    | C2  |
 *                 +----+-----+  ===>           +----+    +-----+
 *         +------------+               +------------+
 *         |      E     |               |      E     |
 *         +------------+               +------------+
 *   Now the first part clearing range C1 can be handled as condition 4), and
 *   the second part clearing range C2 can be handled as condition 1) in next
 *   loop.
 *
 *   All bad blocks range clearing can be simplified into the above 5 situations
 *   by only handling the head part of the clearing range in each run of the
 *   while-loop. The idea is similar to bad blocks range setting but much
 *   simpler.
 */

/*
 * [한국어] 위 영어 주석(원본 커널 문서)이 설명하는 set/clear 알고리즘 요약:
 * badblocks_set()은 새로 등록할 범위 S를 이미 등록된 범위들(E1, E2, ...)과
 * 비교해가며 머리부터 조금씩 처리한다 — 겹치지 않으면 새 슬롯을 삽입(1),
 * S가 E의 시작과 일치하면 ack가 같을 때 병합하고 다르면 acked 쪽이 unacked
 * 쪽을 덮어씀(2), S가 E보다 앞서 시작하면 앞부분만 이번 루프에서 처리하고
 * 나머지는 다음 루프로 미룸(3), S가 E 뒤에서 시작해 꼬리/중간과 겹치면
 * ack 비교 후 병합 또는 분할(4), 테이블이 가득 차 새 슬롯을 못 만들 때는
 * 먼저 인접한 기존 항목들을 병합해 공간을 만든 뒤 재시도(6.1), 덮어쓰기로
 * 인해 새로 생긴 acked 항목이 앞의 acked 항목과 다시 병합 가능해지는 경우
 * (6.2)까지 처리한다. badblocks_clear()는 이보다 단순해서, 해제 범위가
 * 기존 항목의 머리/꼬리/전체/중앙 중 어디와 겹치는지만 구분하면 된다.
 * 아래 각 헬퍼 함수는 이 상황 분류표의 한 조각씩을 담당한다.
 */

/*
 * [한국어]
 * prev_by_hint() - hint 인덱스 부근에서 시작 섹터가 s 이하인 마지막 슬롯을
 * 짧은 선형 탐색으로 찾는다 (prev_badblocks()의 빠른 경로).
 *
 * @bb: 탐색 대상 bad table을 담은 badblocks 인스턴스.
 * @s: 기준이 되는 섹터 값 — "이 값 이하에서 시작하는 마지막 슬롯"을 찾는다.
 * @hint: 직전 호출(prev_badblocks())이 돌려준 인덱스. 이번에도 그 근처일
 *        것이라는 가정하에 탐색 시작점으로 삼는다. 호출자가 hint < 0을
 *        넘기면 prev_badblocks()가 애초에 이 함수를 부르지 않는다.
 * @return: 조건을 만족하는 슬롯 인덱스, 못 찾으면(윈도우 안에서 실패) -1.
 *          -1은 실패가 아니라 "이 짧은 탐색으로는 못 찾았으니 이진 탐색으로
 *          폴백하라"는 신호로 prev_badblocks()가 해석한다.
 *
 * badblocks_set()/_clear()/_check()는 모두 요청 범위를 머리부터 순서대로
 * 처리하는 while 루프 구조라서, 매 반복마다 필요한 "기준 슬롯"이 직전
 * 반복의 기준 슬롯과 같거나 바로 다음 슬롯인 경우가 대부분이다. 이 국소성
 * (locality)을 이용해 O(log n) 이진 탐색을 O(1)에 가까운 2슬롯 이내의
 * 선형 확인으로 대체하는 것이 이 함수의 존재 이유다 — prev_badblocks()의
 * 주석에 있듯, 첫 반복을 제외하면 거의 항상 이 빠른 경로가 성공한다.
 * 동작 과정: hint부터 시작해 최대 hint+2 미만까지, "이 슬롯의 시작 섹터가
 * s 이하이고, 다음 슬롯은 s보다 뒤에서 시작하거나 테이블 끝"인 슬롯을
 * 찾으면 그 인덱스를 반환한다. 그 조건에 맞는 슬롯을 찾지 못하고 윈도우를
 * 벗어나면 -1을 반환해 폴백을 유도한다.
 * 실행 컨텍스트: 호출자가 이미 seqlock(읽기 또는 쓰기 측)을 잡고 있는
 * 상태에서 호출되는 순수 조회 함수이며 별도의 동기화나 재진입 문제가 없다.
 * 호출자: prev_badblocks() (hint >= 0일 때만). 호출 대상: 없음(리프 함수).
 * 에러 경로: 실패를 나타내는 특별한 에러 코드는 없고, -1 반환이 곧
 * "폴백 필요"라는 정상적인 제어 흐름의 일부다.
 *
 * 호출 체인:
 *   badblocks_set()/_clear()/_check() → prev_badblocks() → [prev_by_hint()]
 */
static int prev_by_hint(struct badblocks *bb, sector_t s, int hint) /* bad table 페이지와 힌트를 받아 짧은 선형 탐색만 수행 */
{
	int hint_end = hint + 2; /* 탐색 윈도우 상한 - hint, hint+1 두 슬롯만 확인해 비용을 O(1)로 제한 */
	u64 *p = bb->page; /* bad table 배열 베이스 포인터 - BB_OFFSET() 등으로 슬롯을 해석 */
	int ret = -1; /* 기본값 - 윈도우 안에서 조건을 만족하는 슬롯을 못 찾으면 이 값 그대로 반환되어 이진 탐색 폴백을 유도 */

	while ((hint < hint_end) && ((hint + 1) <= bb->count) &&
	       (BB_OFFSET(p[hint]) <= s)) { /* hint가 윈도우 안이고, 다음 슬롯 인덱스가 테이블 범위 내이며, 현재 슬롯 시작 섹터가 s 이하인 동안 반복 - 세 조건 중 하나라도 깨지면 이 자리에서 탐색 종료 */
		if ((hint + 1) == bb->count || BB_OFFSET(p[hint + 1]) > s) { /* 다음 슬롯이 없거나(테이블 끝) 다음 슬롯이 s보다 뒤에서 시작하면, 지금 슬롯이 "s 이하에서 시작하는 마지막 슬롯"이 확정됨 */
			ret = hint; /* 조건을 만족하는 슬롯 인덱스를 결과로 확정 */
			break; /* 더 볼 필요 없이 즉시 반환 - 정렬된 배열이므로 이 이후 슬롯은 모두 조건 불만족 */
		}
		hint++; /* 다음 슬롯 인덱스로 이동 - 정렬 순서를 따라 오름차순으로만 진행 */
	}

	return ret; /* 찾은 슬롯 인덱스 또는 -1(폴백 신호) */
}

/*
 * [한국어]
 * prev_badblocks() - 시작 섹터가 bad->start 이하인 마지막 슬롯의 인덱스를
 * 찾는다 (hint 선형 탐색 우선, 실패 시 이진 탐색 폴백).
 *
 * @bb: 탐색 대상 badblocks 인스턴스.
 * @bad: bad->start(기준 섹터)만 이 함수에서 참조된다. bad->len/ack은 이
 *       함수의 관심사가 아니며 호출자가 이후 단계에서 사용한다.
 * @hint: 직전 반복이 돌려준 인덱스, 없으면 -1. -1이면 이진 탐색으로 바로
 *        진입한다.
 * @return: 조건(시작 섹터 <= bad->start)을 만족하는 마지막 슬롯 인덱스,
 *          그런 슬롯이 아예 없으면(모든 슬롯이 bad->start보다 뒤에서
 *          시작) -1.
 *
 * 이 함수는 badblocks_set()/_clear()/_check() 세 엔진이 공통으로 필요로
 * 하는 "기준 슬롯 찾기"를 담당한다. 먼저 prev_by_hint()로 값싼 선형
 * 탐색을 시도해 hit하면 그대로 반환하고(대부분의 반복에서 여기서 끝남),
 * miss하면 전체 테이블에 대해 O(log n) 이진 탐색을 수행한다. 이진 탐색에
 * 들어가기 전에 "첫 슬롯보다도 앞" / "마지막 슬롯 이하"인 경계 케이스를
 * 먼저 걸러 불필요한 반복을 줄인다.
 * 실행 컨텍스트: 호출자가 이미 seqlock을 잡은 상태에서 호출되는 순수
 * 조회 함수. 재진입/동시성 문제 없음.
 * 호출자: _badblocks_set(), _badblocks_clear(), _badblocks_check() 모두
 * 각자의 루프(re_insert/re_clear/re_check) 매 반복마다 호출한다.
 * 호출 대상: prev_by_hint()(hint 있을 때만), BB_OFFSET() 매크로.
 * 에러 경로: 없음 — -1은 에러가 아니라 "s 이전에 슬롯이 없다"는 정상적
 * 의미다(예: badblocks_set()에서는 이후 "테이블 맨 앞에 삽입" 분기로
 * 이어진다).
 *
 * 호출 체인:
 *   _badblocks_set()/_badblocks_clear()/_badblocks_check() → [prev_badblocks()] → prev_by_hint()
 */
static int prev_badblocks(struct badblocks *bb, struct badblocks_context *bad, /* bad->start 이하에서 시작하는 마지막 슬롯 인덱스를 찾는 공용 탐색 헬퍼 */
			  int hint)
{
	sector_t s = bad->start; /* 탐색 기준 섹터 - bad 컨텍스트에서 시작 값만 꺼내 지역 변수로 고정 */
	int ret = -1; /* 기본 반환값 - "s 이하에서 시작하는 슬롯 없음"을 의미, 아래에서 조건 만족 시 갱신됨 */
	int lo, hi; /* 이진 탐색 구간 [lo, hi) - 슬롯 인덱스 범위를 반씩 좁혀감 */
	u64 *p; /* bad table 베이스 포인터 - 이진 탐색 구간에서 재사용 */

	if (!bb->count) /* 테이블이 비어 있으면(슬롯 0개) 비교할 대상 자체가 없음 */
		goto out; /* ret=-1 그대로 반환 - 빈 테이블에서는 항상 "이전 슬롯 없음" */

	if (hint >= 0) { /* 직전 반복이 유효한 힌트를 남겼으면 우선 값싼 경로부터 시도 */
		ret = prev_by_hint(bb, s, hint); /* hint 주변 최대 2슬롯만 확인하는 O(1) 탐색 */
		if (ret >= 0) /* 힌트 탐색이 성공(폴백 신호 -1이 아님) */
			goto out; /* 이진 탐색을 건너뛰고 즉시 반환 - 대부분의 반복은 여기서 종료 */
	}

	lo = 0; /* 이진 탐색 하한 인덱스 초기화 */
	hi = bb->count; /* 이진 탐색 상한(배타적) - 테이블에 실제로 채워진 슬롯 수 */
	p = bb->page; /* 이진 탐색에서 사용할 테이블 베이스 포인터 재확보 */

	/* The following bisect search might be unnecessary */
	if (BB_OFFSET(p[lo]) > s) /* 첫 슬롯의 시작 섹터조차 s보다 크면, 모든 슬롯이 s보다 뒤에서 시작 */
		return -1; /* s 이하에서 시작하는 슬롯이 원천적으로 없음 - 이진 탐색 없이 조기 반환 */
	if (BB_OFFSET(p[hi - 1]) <= s) /* 마지막 슬롯조차 s 이하에서 시작하면, 그 마지막 슬롯이 곧 답 */
		return hi - 1; /* 전체 테이블이 s 이하이므로 이진 탐색 없이 마지막 인덱스 확정 반환 */

	/* Do bisect search in bad table */
	while (hi - lo > 1) { /* 구간 폭이 1 슬롯으로 좁혀질 때까지 반복 - O(log n) 종료 보장 */
		int mid = (lo + hi)/2; /* 현재 구간의 중간 인덱스 계산 */
		sector_t a = BB_OFFSET(p[mid]); /* mid 슬롯의 시작 섹터 - 비교 기준값 */

		if (a == s) { /* 정확히 s에서 시작하는 슬롯을 바로 찾은 경우 */
			ret = mid; /* 정확 일치 슬롯을 결과로 확정 */
			goto out; /* 더 좁힐 필요 없이 즉시 반환 */
		}

		if (a < s) /* mid 슬롯이 s보다 앞에서 시작 - 답이 mid 또는 그 오른쪽에 있음 */
			lo = mid; /* 하한을 mid로 올려 오른쪽(더 큰 섹터) 절반으로 구간 축소 */
		else /* mid 슬롯이 s보다 뒤에서 시작 - 답은 mid보다 왼쪽에 있음 */
			hi = mid; /* 상한을 mid로 내려 왼쪽(더 작은 섹터) 절반으로 구간 축소 */
	}

	if (BB_OFFSET(p[lo]) <= s) /* 루프 종료 후 남은 lo 슬롯이 조건(s 이하)을 만족하는지 최종 확인 */
		ret = lo; /* 조건을 만족하면 lo를 최종 결과로 채택 */
out:
	return ret; /* 확정된 인덱스(또는 초기값 -1) 반환 - 호출자는 이 값으로 front/overlap 여부를 추가 판단 */
}

/*
 * [한국어]
 * can_merge_front() - bad 범위를 prev 슬롯 앞쪽(끝부분)에 이어붙일 수
 * 있는지(front merge) 판정한다.
 *
 * @bb: 대상 badblocks 인스턴스 (page 배열만 참조).
 * @prev: 병합 후보인 기존 슬롯의 인덱스 (prev_badblocks()가 찾아준 기준
 *        슬롯).
 * @bad: 병합하려는 새 범위. bad->start와 ack만 검사에 쓰인다.
 * @return: true면 front_merge() 호출이 안전, false면 병합 불가(다른 처리
 *          경로로 가야 함).
 *
 * ack 플래그가 다른 두 범위를 병합하면 "이미 상위에 확인된 불량"과
 * "아직 확인되지 않은 불량"이 구분 불가능해지므로, ack가 같을 때만
 * 병합을 허용한다. 그리고 실제로 이어붙일 수 있으려면 새 범위의 시작이
 * prev의 끝보다 앞(중첩)이거나, 정확히 prev의 끝과 맞닿아 있으면서(인접)
 * prev의 길이가 아직 BB_MAX_LEN(슬롯 하나가 표현 가능한 최대 512섹터)
 * 미만이어야 한다 — 이미 꽉 찬 슬롯에 더 붙이면 인코딩이 불가능하다.
 * 실행 컨텍스트: 호출자가 seqlock 쓰기 측을 잡은 상태의 순수 판정 함수.
 * 호출자: _badblocks_set()이 삽입 위치 직전에 병합 가능 여부를 물어볼 때.
 * 호출 대상: 없음(매크로만 사용).
 * 에러 경로: 없음 — bool 판정 결과만 반환.
 *
 * 호출 체인:
 *   _badblocks_set() → [can_merge_front()] (true면 이어서 front_merge() 호출)
 */
static bool can_merge_front(struct badblocks *bb, int prev, /* prev 슬롯 끝에 bad 범위를 이어붙일 수 있는지(ack 일치 + 인접/중첩) 판정 */
			    struct badblocks_context *bad)
{
	sector_t s = bad->start; /* 병합 판정 기준이 되는 새 범위의 시작 섹터 */
	u64 *p = bb->page; /* bad table 베이스 포인터 */

	if (BB_ACK(p[prev]) == bad->ack && /* 기존 슬롯과 새 범위의 ack 상태가 같아야만 병합 허용 - 다르면 acked/unacked 구분이 뭉개짐 */
	    (s < BB_END(p[prev]) || /* 새 범위 시작이 prev 끝보다 앞 - 두 범위가 겹침 */
	     (s == BB_END(p[prev]) && (BB_LEN(p[prev]) < BB_MAX_LEN)))) /* 또는 정확히 인접하면서 prev가 아직 최대 길이(512섹터) 미만이라 확장 여지가 있음 */
		return true; /* 병합 가능 - front_merge()로 실제 확장 수행 가능 */
	return false; /* 병합 불가 - ack 불일치 또는 인접/중첩 조건 미충족, 별도 슬롯 필요 */
}

/*
 * [한국어]
 * front_merge() - bad 범위를 prev 슬롯 끝에 실제로 이어붙여 확장한다
 * (can_merge_front()가 true를 반환한 뒤에만 호출되어야 함).
 *
 * @bb: 대상 badblocks 인스턴스.
 * @prev: 확장 대상 슬롯 인덱스.
 * @bad: 병합할 새 범위(start/len/ack). bad->ack는 병합 후 prev 슬롯의 ack로
 *       그대로 채택된다(can_merge_front가 이미 prev와 같음을 보장했으므로
 *       사실상 값 유지).
 * @return: 실제로 병합(흡수)된 섹터 수. 요청한 bad->len 전부를 흡수하지
 *          못할 수 있으므로(BB_MAX_LEN 한도 또는 다음 슬롯과의 간격 한도),
 *          호출자는 이 반환값만큼만 s/sectors를 전진시켜야 한다.
 *
 * 새 범위가 prev와 겹치는 경우(중첩)에는 "이미 prev가 커버하는 부분"이므로
 * 실제로 늘어나는 길이 없이 겹친 만큼만 흡수 처리하고 prev 자체는 바꾸지
 * 않는다(WARN_ON으로 s가 prev 끝을 넘어서는 불가능한 상태만 방어). 겹치지
 * 않고 정확히 인접한 경우에는 실제로 prev의 길이를 늘리되, BB_MAX_LEN을
 * 넘지 않도록, 그리고 다음 슬롯(prev+1)이 존재하면 그 슬롯 시작 전까지만
 * 늘어나도록 제한한다 — 그렇지 않으면 두 슬롯이 표현하는 섹터 구간이
 * 겹쳐버려 정렬/비중첩 불변식이 깨진다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중에만 호출되는 테이블 수정 함수.
 * 호출자: _badblocks_set() — can_merge_front()가 true를 반환한 직후.
 * 호출 대상: min_t(), BB_MAKE()/BB_OFFSET()/BB_LEN()/BB_END() 매크로.
 * 에러 경로: WARN_ON 조건은 호출자가 이미 겹침을 확인했다는 전제가 깨진
 * 방어적 경고일 뿐 정상 흐름에서는 발생하지 않는다.
 *
 * 호출 체인:
 *   _badblocks_set() → can_merge_front() → [front_merge()]
 */
static int front_merge(struct badblocks *bb, int prev, struct badblocks_context *bad) /* can_merge_front()가 허용한 병합을 실제로 수행 - prev 슬롯을 확장하거나 중첩분을 흡수 처리 */
{
	sector_t sectors = bad->len; /* 병합하려는 요청 길이(섹터 수) */
	sector_t s = bad->start; /* 병합 기준 시작 섹터 */
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	int merged = 0; /* 실제로 병합 처리된 섹터 수 - 함수 종료 시 반환됨 */

	WARN_ON(s > BB_END(p[prev])); /* 방어적 검증 - s가 prev 끝보다 뒤라면 애초에 can_merge_front가 true를 반환할 수 없는 모순 상태 */

	if (s < BB_END(p[prev])) { /* 새 범위 시작이 prev 끝보다 앞 - 두 범위가 실제로 겹치는 경우 */
		merged = min_t(sector_t, sectors, BB_END(p[prev]) - s); /* 겹치는 구간 길이만큼만 "흡수 처리"(prev는 이미 이 구간을 커버하므로 슬롯 확장 불필요) */
	} else {
		merged = min_t(sector_t, sectors, BB_MAX_LEN - BB_LEN(p[prev])); /* 겹치지 않고 인접한 경우 - prev를 늘리되 슬롯 하나의 최대 길이(512섹터)를 넘지 않도록 제한 */
		if ((prev + 1) < bb->count && /* 다음 슬롯이 실제로 존재하면 */
		    merged > (BB_OFFSET(p[prev + 1]) - BB_END(p[prev]))) { /* 늘어날 길이가 다음 슬롯 시작까지의 간격보다 크면 */
			merged = BB_OFFSET(p[prev + 1]) - BB_END(p[prev]); /* 다음 슬롯과 겹치지 않도록 간격만큼으로 재제한 - 정렬/비중첩 불변식 유지 */
		}

		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* 시작 섹터는 그대로 유지 */
				  BB_LEN(p[prev]) + merged, bad->ack); /* 길이를 merged만큼 늘리고 ack는 병합 요청 값으로 재인코딩 - 슬롯을 새 u64로 교체 */
	}

	return merged; /* 실제로 처리된 섹터 수 - 호출자(_badblocks_set)가 이 값만큼 진행 상태(s/sectors)를 전진 */
}

/*
 * [한국어]
 * can_combine_front() - prev 슬롯이 bad->start에서 정확히 시작할 때, 그
 * 바로 앞(prev-1) 슬롯과 prev를 하나로 합칠 수 있는지 판정한다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @prev: 검사 기준 슬롯 인덱스. prev-1이 그 직전 슬롯이다.
 * @bad: bad->start가 정확히 prev의 시작과 같은지 확인하는 데 쓰인다.
 * @return: true면 front_combine() 호출로 두 슬롯을 실제로 합칠 수 있음.
 *
 * 이 함수는 front_merge()와는 다른 시나리오를 다룬다 — front_merge()는
 * "새로 들어오는 범위"와 기존 슬롯을 합치지만, can_combine_front()는
 * "이미 테이블에 있는 두 슬롯"(prev-1과 prev)이 새 범위 처리로 인해
 * 서로 인접하게 됐을 때 그 둘을 합치는 것이다(예: prev가 방금
 * front_overwrite()로 새 시작점을 갖게 되어 prev-1과 딱 맞닿은 경우).
 * 조건은 prev-1이 존재하고(prev>0), prev가 정확히 bad->start에서
 * 시작하며, prev-1의 끝이 prev의 시작과 정확히 같고(인접), 합친 길이가
 * BB_MAX_LEN 이하이며, 둘의 ack가 같아야 한다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 순수 판정 함수.
 * 호출자: _badblocks_set()이 여러 지점(첫 삽입 직전, overwrite 직후)에서
 * "혹시 앞과 합쳐질 수 있는지" 확인할 때.
 * 호출 대상: 없음(매크로만 사용).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   _badblocks_set() → [can_combine_front()] (true면 이어서 front_combine() 호출)
 */
static bool can_combine_front(struct badblocks *bb, int prev, /* prev-1과 prev 두 기존 슬롯을 하나로 합칠 수 있는지(인접+ack 일치+길이 한도) 판정 */
			      struct badblocks_context *bad)
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */

	if ((prev > 0) && /* prev-1이 실제로 존재하는 인덱스인지 먼저 확인 - prev==0이면 합칠 앞 슬롯 자체가 없음 */
	    (BB_OFFSET(p[prev]) == bad->start) && /* prev 슬롯의 시작이 지금 처리 중인 bad->start와 정확히 일치 */
	    (BB_END(p[prev - 1]) == BB_OFFSET(p[prev])) && /* prev-1의 끝과 prev의 시작이 정확히 맞닿아(gap 없이) 있어야 결합 가능 */
	    (BB_LEN(p[prev - 1]) + BB_LEN(p[prev]) <= BB_MAX_LEN) && /* 두 길이를 합쳐도 슬롯 하나의 표현 한도(512섹터)를 넘지 않아야 함 */
	    (BB_ACK(p[prev - 1]) == BB_ACK(p[prev]))) /* 두 슬롯의 ack 상태가 같아야 함 - 다르면 합쳤을 때 의미가 뭉개짐 */
		return true; /* 결합 가능 */
	return false; /* 결합 불가 - 조건 중 하나라도 불만족 */
}

/*
 * [한국어]
 * front_combine() - can_combine_front()가 true를 반환한 prev-1, prev
 * 두 슬롯을 실제로 하나로 합치고 빈 슬롯을 제거한다.
 *
 * @bb: 대상 badblocks 인스턴스 (bb->count는 이 함수 밖에서 호출자가
 *      감소시킨다 — 이 함수 자체는 count를 건드리지 않음에 주의).
 * @prev: 합칠 두 슬롯 중 뒤쪽 인덱스(앞쪽은 prev-1).
 * @return: 없음(void).
 *
 * prev-1 슬롯을 "시작은 prev-1 그대로, 길이는 둘의 합, ack는 prev의 것"
 * (둘이 이미 같음이 보장됨)으로 재인코딩해 병합된 슬롯으로 만들고, prev
 * 뒤에 남아있는 슬롯들을 한 칸씩 앞으로 당겨(memmove) prev 위치에 있던
 * "이제 중복된" 슬롯을 배열에서 제거한다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 테이블 수정 함수.
 * 호출자: _badblocks_set()이 can_combine_front() 성공 직후 호출하고, 그
 * 직후 자신이 bb->count--를 수행해 실제 슬롯 수 감소를 반영한다.
 * 호출 대상: BB_MAKE()/BB_OFFSET()/BB_LEN()/BB_ACK() 매크로, memmove().
 * 에러 경로: 없음 — 호출자가 이미 조건을 검증했다는 전제 위에서 무조건
 * 실행되는 갱신 함수.
 *
 * 호출 체인:
 *   _badblocks_set() → can_combine_front() → [front_combine()] (복귀 후 호출자가 bb->count--)
 */
static void front_combine(struct badblocks *bb, int prev) /* prev-1과 prev 두 슬롯을 하나로 재인코딩하고 뒤 슬롯들을 당겨 배열을 압축 */
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */

	p[prev - 1] = BB_MAKE(BB_OFFSET(p[prev - 1]), /* 시작은 앞쪽(prev-1) 슬롯의 시작 그대로 유지 */
			      BB_LEN(p[prev - 1]) + BB_LEN(p[prev]), /* 길이는 두 슬롯 길이의 합으로 확장 */
			      BB_ACK(p[prev])); /* ack는 prev의 값 채택(이미 prev-1과 동일함이 can_combine_front에서 보장됨) */
	if ((prev + 1) < bb->count) /* prev 뒤에 아직 유효한 슬롯이 남아있으면 */
		memmove(p + prev, p + prev + 1, (bb->count - prev - 1) * 8); /* prev 위치부터 뒤 슬롯들을 한 칸씩 앞으로 당겨 "합쳐져서 비게 된" prev 슬롯 자리를 제거 - 8은 u64 슬롯 하나의 바이트 크기 */
}

/*
 * [한국어]
 * overlap_front() - bad->start가 front 슬롯이 커버하는 [시작,끝) 구간
 * 안에 들어가는지(front 슬롯과 "앞쪽으로" 겹치는지) 검사한다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @front: 비교할 기존 슬롯의 인덱스(대개 prev_badblocks()가 찾은 기준
 *         슬롯).
 * @bad: bad->start만 비교에 사용된다.
 * @return: front 슬롯이 bad->start 지점을 포함하면 true.
 *
 * "겹침"의 세 가지 유형(앞쪽/뒤쪽/중간) 중 "새 범위의 시작점이 기존
 * 슬롯 안에 있는지"만 판정하는 좁은 헬퍼다. 이 조건이 참이면 새 범위는
 * 이 기존 슬롯과 어떤 형태로든(병합/덮어쓰기/스킵) 상호작용해야 하고,
 * 거짓이면 이 슬롯은 완전히 무시하고 다음 슬롯(overlap_behind)만 보면
 * 된다.
 * 실행 컨텍스트: seqlock 보유 중 호출되는 순수 판정 함수 — set/clear/
 * check 세 엔진 모두에서 사용된다.
 * 호출자: _badblocks_set(), _badblocks_clear(), _badblocks_check(),
 * can_front_overwrite()(WARN_ON을 통한 전제조건 재확인).
 * 호출 대상: 없음(매크로만 사용).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   _badblocks_set()/_clear()/_check() → [overlap_front()]
 */
static bool overlap_front(struct badblocks *bb, int front, /* bad->start가 front 슬롯의 [시작,끝) 구간 안에 있는지만 검사 */
			  struct badblocks_context *bad)
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */

	if (bad->start >= BB_OFFSET(p[front]) && /* bad 시작이 front 슬롯 시작 이상 - 슬롯 앞쪽 경계를 벗어나지 않음 */
	    bad->start < BB_END(p[front])) /* 그리고 bad 시작이 front 슬롯 끝보다 앞 - 슬롯 구간 내부에 위치 */
		return true; /* bad->start가 front 슬롯 구간 안에 있음 - 겹침 확정 */
	return false; /* front 슬롯과는 겹치지 않음 */
}

/*
 * [한국어]
 * overlap_behind() - bad 범위가 behind 슬롯보다 앞서 시작해서, 끝이
 * behind 슬롯의 시작점을 지나치는지(즉 "뒤쪽으로" 걸치는지) 검사한다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @bad: bad->start와 bad->len(끝 계산에 사용)이 모두 쓰인다.
 * @behind: 비교할 기존 슬롯의 인덱스(대개 prev+1, 즉 기준 슬롯 바로
 *          다음).
 * @return: bad 범위의 끝이 behind 슬롯 시작점을 넘어서면 true.
 *
 * overlap_front()가 "새 범위 시작점이 기존 슬롯 안에 있는지"를 본다면,
 * 이 함수는 반대로 "새 범위의 끝이 다음 기존 슬롯의 시작을 침범하는지"
 * 를 본다. 이 조건이 참이면 새 범위를 그 다음 슬롯 시작 지점까지만
 * 잘라서 처리하고 나머지는 다음 반복으로 넘겨야 한다(정렬/비중첩
 * 불변식을 지키기 위함).
 * 실행 컨텍스트: seqlock 보유 중 호출되는 순수 판정 함수.
 * 호출자: _badblocks_set(), _badblocks_clear(), _badblocks_check() —
 * 모두 "front와는 안 겹치는데, 다음 슬롯과는 겹치는지" 확인하는 용도로.
 * 호출 대상: 없음(매크로만 사용).
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   _badblocks_set()/_clear()/_check() → [overlap_behind()]
 */
static bool overlap_behind(struct badblocks *bb, struct badblocks_context *bad, /* bad 범위의 끝이 behind 슬롯 시작점을 지나치는지만 검사 */
			   int behind)
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */

	if (bad->start < BB_OFFSET(p[behind]) && /* bad 시작이 behind 슬롯 시작보다 앞 - behind 슬롯 자체와는 아직 안 겹침 */
	    (bad->start + bad->len) > BB_OFFSET(p[behind])) /* 그런데 bad의 끝(start+len)이 behind 슬롯 시작을 넘어섬 - 뒤쪽으로 침범 */
		return true; /* bad 범위가 behind 슬롯 시작점을 걸치고 있음 */
	return false; /* behind 슬롯과는 겹치지 않음(완전히 앞에서 끝나거나 애초에 behind보다 뒤에서 시작) */
}

/*
 * [한국어]
 * can_front_overwrite() - bad 범위(acked)가 prev 슬롯(unacked)을 덮어쓸
 * 수 있는지 판정하고, 덮어쓸 경우 추가로 필요한 슬롯 수(extra: 0/1/2)를
 * 계산한다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @prev: 덮어쓰기 대상 후보 슬롯(overlap_front()가 이미 참임을 확인한
 *        슬롯이어야 함 — WARN_ON으로 전제조건 재확인).
 * @bad: 덮어쓰려는 범위. 이 함수 내부에서 bad->len이 "실제로 덮어쓸 수
 *       있는 길이"로 축소될 수 있다(out 파라미터처럼 동작).
 * @extra: [출력] 덮어쓰기가 실제로 일어날 때 prev 슬롯 하나가 몇 개의
 *         슬롯으로 늘어나는지 - 0(전체 교체, 슬롯 수 불변), 1(head 또는
 *         tail 한쪽만 남아 분할, 슬롯 +1), 2(중간을 덮어써 head/tail
 *         모두 남아 분할, 슬롯 +2).
 * @return: true면 front_overwrite() 호출이 안전, false면 (ack가 이미
 *          같거나 더 높아 우선순위상 덮어쓸 수 없거나, 테이블에 extra
 *          만큼의 여유 슬롯이 없어서) 덮어쓰기 불가.
 *
 * badblocks 엔진의 핵심 정책은 "acked(상위 계층이 이미 확인한) 정보가
 * unacked(아직 미확인) 정보보다 우선한다"는 것이다. 그래서 새로 들어온
 * 범위의 ack가 기존 prev 슬롯의 ack보다 "더 클"(unacked=0 < acked=1)
 * 때만 덮어쓰기를 허용한다. 덮어쓸 실제 길이와 필요한 추가 슬롯 수는
 * bad 범위가 prev 슬롯을 어디까지 덮는지(끝까지/그 이상 vs prev 안쪽만)
 * 와 어디서 시작하는지(prev 시작과 일치 vs 중간)의 조합으로 결정된다.
 * 마지막으로 계산된 extra만큼의 여유가 테이블에 없으면(MAX_BADBLOCKS
 * 초과) 덮어쓰기 자체를 거부해 배열 오버플로를 막는다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 판정 함수.
 * 호출자: _badblocks_set() — overlap_front()가 true인 상황에서
 * can_merge_front()도 실패했을 때 다음 시도로 호출.
 * 호출 대상: overlap_front()(WARN_ON 검증용), BB_* 매크로.
 * 에러 경로: false 반환 시 호출자는 "겹치는 만큼 스킵하고 다음 반복으로"
 * 폴백한다(공간 부족 시) 또는 애초에 이 분기에 들어오지 않는다(ack 부족).
 *
 * 호출 체인:
 *   _badblocks_set() → [can_front_overwrite()] (true면 이어서 front_overwrite() 호출)
 */
static bool can_front_overwrite(struct badblocks *bb, int prev, /* prev 슬롯을 bad 범위(acked)로 덮어쓸 수 있는지 판정하고 필요 슬롯 증가분(extra)을 계산 */
				struct badblocks_context *bad, int *extra)
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	int len; /* 실제로 덮어쓸 길이 - 상황에 따라 계산 후 bad->len에 반영 */

	WARN_ON(!overlap_front(bb, prev, bad)); /* 전제조건 재확인 - 이 함수는 반드시 overlap_front()가 참인 prev에 대해서만 호출되어야 함 */

	if (BB_ACK(p[prev]) >= bad->ack) /* 기존 슬롯이 이미 acked이거나 새 범위와 ack가 같으면(우선순위 낮거나 동일) */
		return false; /* 덮어쓰기 불가 - acked 정보를 unacked/동일 정보로 되돌릴 수 없음 */

	if (BB_END(p[prev]) <= (bad->start + bad->len)) { /* 새 범위가 prev의 끝까지 도달하거나 그 너머까지 이어짐 */
		len = BB_END(p[prev]) - bad->start; /* 실제 덮어쓸 길이는 prev 끝까지로 제한(그 너머는 다음 반복에서 별도 처리) */
		if (BB_OFFSET(p[prev]) == bad->start) /* 새 범위 시작이 prev 시작과 일치 - prev 전체가 덮어써짐 */
			*extra = 0; /* 슬롯 교체만 일어나고 개수는 그대로 */
		else /* 시작점이 다르면 prev 앞부분(head)이 원래 ack 상태로 남음 */
			*extra = 1; /* head 슬롯 + 덮어쓴 슬롯, 총 1개 증가 */

		bad->len = len; /* 호출자가 실제로 처리(진행)할 길이를 이 함수가 확정해 되돌려줌 */
	} else { /* 새 범위가 prev 안쪽에만 머무름(prev 끝에 도달하지 못함) */
		if (BB_OFFSET(p[prev]) == bad->start) /* 새 범위 시작이 prev 시작과 일치 - prev 앞부분을 덮어씀 */
			*extra = 1; /* prev tail이 원래 ack 상태로 남아 1개 증가 */
		else /* 시작점도 다르고 끝에도 못 미침 - prev 중간을 덮어씀 */
		/*
		 * prev range will be split into two, beside the overwritten
		 * one, an extra slot needed from bad table.
		 */
			*extra = 2; /* head와 tail이 모두 원래 ack 상태로 남아 총 2개 증가(head+새범위+tail=3슬롯) */
	}

	if ((bb->count + (*extra)) > MAX_BADBLOCKS) /* 늘어날 슬롯 수를 더했을 때 테이블 용량(MAX_BADBLOCKS)을 초과하면 */
		return false; /* 공간 부족으로 덮어쓰기 자체를 포기 - 배열 오버플로 방지 */

	return true; /* 덮어쓰기 가능 - extra에 계산된 슬롯 증가분과 함께 반환 */
}

/*
 * [한국어]
 * front_overwrite() - can_front_overwrite()가 계산한 extra(0/1/2)에 따라
 * prev 슬롯을 실제로 bad 범위로 덮어쓴다(교체 또는 1~2개로 분할).
 *
 * @bb: 대상 badblocks 인스턴스.
 * @prev: 덮어쓰기 대상 슬롯 인덱스.
 * @bad: 덮어쓸 범위(start/len/ack) - can_front_overwrite()가 이미 len을
 *       "실제로 덮어쓸 길이"로 조정해 놓은 상태로 전달되어야 한다.
 * @extra: can_front_overwrite()가 계산한 값 그대로(0/1/2) - 이 값에 따라
 *         switch 분기로 처리 방식이 갈린다.
 * @return: 덮어쓴 길이(bad->len 그대로) - 호출자가 진행 상태(s/sectors)를
 *          전진시키는 데 사용.
 *
 * extra==0(전체 교체)이면 시작/길이는 그대로 두고 ack 비트만 새 값으로
 * 바꿔 슬롯을 재인코딩한다. extra==1(한쪽만 분할)이면, 새 범위가 prev의
 * 시작과 일치하는지 여부로 "머리가 없고 꼬리만 남는" 경우와 "머리만
 * 남는" 경우를 나눠, 뒤 슬롯들을 한 칸씩 밀어(memmove) 새로 생기는 슬롯
 * 자리를 확보한 뒤 두 슬롯(덮어쓴 범위 + 남은 머리/꼬리)을 채운다.
 * extra==2(양쪽 분할)이면 뒤 슬롯들을 두 칸 밀어 head/새범위/tail 세
 * 슬롯을 위한 자리를 만든다. 남는 머리/꼬리 부분은 항상 원래 ack 상태
 * (orig_ack)를 유지하고, 새로 덮어쓴 부분만 bad->ack를 적용한다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 테이블 수정 함수.
 * 호출자: _badblocks_set() — can_front_overwrite()가 true를 반환한 직후,
 * 호출 후 bb->count += extra로 실제 슬롯 수 증가를 반영한다.
 * 호출 대상: BB_MAKE()/BB_OFFSET()/BB_LEN()/BB_ACK()/BB_END() 매크로,
 * memmove().
 * 에러 경로: default 분기는 도달 불가능(extra는 항상 0/1/2 중 하나) -
 * 방어적으로 아무 것도 하지 않고 지나감.
 *
 * 호출 체인:
 *   _badblocks_set() → can_front_overwrite() → [front_overwrite()] (복귀 후 호출자가 bb->count += extra)
 */
static int front_overwrite(struct badblocks *bb, int prev, /* can_front_overwrite()가 허용한 덮어쓰기를 extra(0/1/2)에 따라 실제로 수행 */
			   struct badblocks_context *bad, int extra)
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	sector_t orig_end = BB_END(p[prev]); /* 분할 후 tail 길이 계산에 필요한 - 덮어쓰기 전 prev의 원래 끝 섹터 보존 */
	int orig_ack = BB_ACK(p[prev]); /* 남는 head/tail에 그대로 적용할 - 덮어쓰기 전 prev의 원래 ack 보존 */

	switch (extra) { /* can_front_overwrite()가 계산한 슬롯 증가분에 따라 처리 방식 분기 */
	case 0: /* prev 전체가 새 범위로 덮어써지는 경우 - 슬롯 개수 변화 없음 */
		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), BB_LEN(p[prev]), /* 시작과 길이는 prev 그대로 유지 */
				  bad->ack); /* ack 비트만 새 값(acked)으로 갱신해 재인코딩 */
		break;
	case 1: /* prev가 머리 또는 꼬리 한쪽만 남기고 분할되는 경우 - 슬롯 1개 증가 */
		if (BB_OFFSET(p[prev]) == bad->start) { /* 새 범위 시작이 prev 시작과 일치 - 머리는 안 남고 꼬리만 남음 */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* prev 자리를 그대로 새 범위의 시작 위치로 재사용 */
					  bad->len, bad->ack); /* prev 슬롯을 통째로 "덮어쓴 새 범위"로 교체 */
			memmove(p + prev + 2, p + prev + 1, /* prev+1 자리를 비우기 위해 그 뒤(prev+1 이후) 슬롯들을 한 칸(prev+2로) 밀기 */
				(bb->count - prev - 1) * 8); /* 밀어야 할 슬롯 개수 * 슬롯 크기(8바이트) */
			p[prev + 1] = BB_MAKE(bad->start + bad->len, /* 꼬리 시작 섹터 = 덮어쓴 범위의 끝 */
					      orig_end - BB_END(p[prev]), /* 꼬리 길이 = 원래 prev 끝 - 덮어쓴 범위(현재 p[prev])의 끝 */
					      orig_ack); /* 꼬리는 덮어쓰기 이전의 원래 ack 상태를 그대로 유지 */
		} else { /* 새 범위 시작이 prev 시작과 다름 - 머리만 남고 꼬리는 안 남음(새 범위가 prev 끝까지 이어짐) */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* prev 시작은 그대로, 길이만 줄여 머리 부분만 남김 */
					  bad->start - BB_OFFSET(p[prev]), /* 머리 길이 = 새 범위 시작 - prev 원래 시작 */
					  orig_ack); /* 머리는 덮어쓰기 이전의 원래 ack 상태를 유지 */
			/*
			 * prev +2 -> prev + 1 + 1, which is for,
			 * 1) prev + 1: the slot index of the previous one
			 * 2) + 1: one more slot for extra being 1.
			 */
			memmove(p + prev + 2, p + prev + 1, /* 새로 삽입할 슬롯(prev+1) 자리를 만들기 위해 뒤 슬롯들을 한 칸 밀기 */
				(bb->count - prev - 1) * 8); /* 밀어야 할 슬롯 개수 * 슬롯 크기(8바이트) */
			p[prev + 1] = BB_MAKE(bad->start, bad->len, bad->ack); /* 비워진 prev+1 자리에 실제 덮어쓴 새 범위를 삽입 */
		}
		break;
	case 2: /* prev 중간을 덮어써 머리와 꼬리가 모두 남는 경우 - 슬롯 2개 증가(head/새범위/tail 총 3개) */
		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* prev 시작은 그대로 유지 */
				  bad->start - BB_OFFSET(p[prev]), /* 머리 길이 = 새 범위 시작 - prev 원래 시작 */
				  orig_ack); /* 머리는 원래 ack 상태 유지 */
		/*
		 * prev + 3 -> prev + 1 + 2, which is for,
		 * 1) prev + 1: the slot index of the previous one
		 * 2) + 2: two more slots for extra being 2.
		 */
		memmove(p + prev + 3, p + prev + 1, /* 새 범위 슬롯과 꼬리 슬롯 두 개를 넣을 자리를 만들기 위해 뒤 슬롯들을 두 칸 밀기 */
			(bb->count - prev - 1) * 8); /* 밀어야 할 슬롯 개수 * 슬롯 크기(8바이트) */
		p[prev + 1] = BB_MAKE(bad->start, bad->len, bad->ack); /* 비워진 prev+1 자리에 실제 덮어쓴 새 범위를 삽입 */
		p[prev + 2] = BB_MAKE(BB_END(p[prev + 1]), /* 꼬리 시작 섹터 = 방금 삽입한 새 범위의 끝 */
				      orig_end - BB_END(p[prev + 1]), /* 꼬리 길이 = 원래 prev 끝 - 새 범위 끝 */
				      orig_ack); /* 꼬리는 원래 ack 상태 유지 */
		break;
	default:
		break;
	}

	return bad->len; /* 실제로 덮어쓴 길이 반환 - 호출자가 이 값만큼 s/sectors를 전진 */
}

/*
 * [한국어]
 * insert_at() - bad 범위를 테이블의 인덱스 'at' 위치에 새 슬롯으로
 * 삽입한다(겹침/병합이 전혀 필요 없는 완전히 새로운 구간을 위한 경로).
 *
 * @bb: 대상 badblocks 인스턴스 (bb->count는 호출자가 이 함수 호출 후
 *      직접 증가시킨다 — 이 함수는 count를 건드리지 않음).
 * @at: 삽입할 배열 인덱스. 정렬 불변식을 지키기 위해 호출자가 미리
 *      "이 위치가 옳다"고 계산해서 넘겨야 한다(대개 prev+1 또는 0).
 * @bad: 삽입할 범위(start/len/ack).
 * @return: 실제로 삽입된 길이. bad->len이 BB_MAX_LEN(512)을 넘으면 그
 *          한도로 잘려서 삽입되므로, 호출자는 이 반환값만큼만 진행
 *          상태를 전진시키고 나머지는 다음 반복에서 새 슬롯으로 다시
 *          삽입해야 한다.
 *
 * 이 함수는 배열에 새 슬롯 하나를 끼워 넣는 가장 기본적인 동작만
 * 담당한다 — 병합/덮어쓰기 판단은 호출자(_badblocks_set)가 이미 끝낸
 * 뒤에만 호출된다. WARN_ON으로 "테이블이 이미 가득 찬 상태에서 삽입을
 * 시도하는" 모순을 방어적으로 검출한다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 테이블 수정 함수.
 * 호출자: _badblocks_set() — 겹치는 기존 슬롯이 전혀 없을 때(빈 테이블,
 * 맨 앞 삽입, 또는 prev/prev+1 사이 삽입).
 * 호출 대상: badblocks_full()(WARN_ON 조건), min_t(), memmove(),
 * BB_MAKE().
 * 에러 경로: 없음(WARN_ON은 커널 로그만 남기고 계속 진행 — 호출자가
 * 이미 badblocks_full()을 확인했다는 전제이므로 정상 흐름에서는 발생
 * 안 함).
 *
 * 호출 체인:
 *   _badblocks_set() → [insert_at()]
 */
static int insert_at(struct badblocks *bb, int at, struct badblocks_context *bad) /* bad 범위를 배열의 at 인덱스에 새 슬롯으로 삽입(겹치는 기존 슬롯 없음이 전제) */
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	int len; /* 실제로 삽입할 길이 - BB_MAX_LEN 한도로 클램핑됨 */

	WARN_ON(badblocks_full(bb)); /* 방어적 검증 - 호출자가 badblocks_full()을 미리 확인했어야 하는 전제가 깨졌는지 경고 */

	len = min_t(sector_t, bad->len, BB_MAX_LEN); /* 슬롯 하나가 표현 가능한 최대 길이(512섹터)로 제한 - 넘는 부분은 다음 반복에서 별도 슬롯으로 */
	if (at < bb->count) /* 삽입 위치가 배열 끝이 아니라 중간이면 */
		memmove(p + at + 1, p + at, (bb->count - at) * 8); /* at 위치부터 끝까지의 기존 슬롯들을 한 칸 뒤로 밀어 삽입 공간 확보 - 8은 u64 슬롯 크기 */
	p[at] = BB_MAKE(bad->start, len, bad->ack); /* 비워진 at 위치에 새 슬롯을 (시작,길이,ack)로 인코딩해 기록 */

	return len; /* 실제로 삽입된 길이 반환 - 호출자가 이 값만큼 진행 상태를 전진 */
}

/*
 * [한국어]
 * badblocks_update_acked() - unacked_exist 힌트 플래그를 실제 테이블
 * 내용과 다시 맞춰(재계산해) 필요 없으면 내린다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @return: 없음(void).
 *
 * unacked_exist는 "acked 아닌 슬롯이 있을 수도 있다"는 보수적 힌트일 뿐
 * 정확한 카운트가 아니다(주석 있는 그대로: "This is only cleared when a
 * read discovers none" — 즉 1로 세팅하기는 쉽게 하되 0으로 내리는 것은
 * 실제로 확인했을 때만 한다). 이 함수는 플래그가 이미 0이면 스캔조차
 * 하지 않고(값싼 조기 반환), 1일 때만 테이블 전체를 스캔해 정말로
 * unacked 슬롯이 하나도 없으면 0으로 내린다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 테이블 스캔 함수.
 * 호출자: _badblocks_set()(acked 등록 처리 후), _badblocks_clear()(해제
 * 후 항상).
 * 호출 대상: BB_ACK() 매크로.
 * 에러 경로: 없음.
 *
 * 호출 체인:
 *   _badblocks_set()/_badblocks_clear() → [badblocks_update_acked()]
 */
static void badblocks_update_acked(struct badblocks *bb) /* unacked_exist 힌트 플래그를 실제 테이블을 스캔해 재계산(내림) */
{
	bool unacked = false; /* 스캔 중 unacked 슬롯을 실제로 찾았는지 여부 */
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	int i; /* 스캔 루프 인덱스 */

	if (!bb->unacked_exist) /* 이미 "unacked 없음"으로 표시돼 있으면 */
		return; /* 스캔할 필요조차 없이 조기 반환 - 값싼 경로 */

	for (i = 0; i < bb->count ; i++) { /* 테이블에 채워진 슬롯 전체를 순회 */
		if (!BB_ACK(p[i])) { /* 이 슬롯의 ack 비트가 0(unacked)이면 */
			unacked = true; /* 적어도 하나의 unacked 슬롯이 실제로 존재함을 확인 */
			break; /* 하나만 찾아도 충분하므로 더 볼 필요 없이 종료 */
		}
	}

	if (!unacked) /* 스캔 결과 unacked 슬롯을 하나도 못 찾았으면 */
		bb->unacked_exist = 0; /* 이제서야 플래그를 정확히 0으로 내림 - "실제로 확인했을 때만 내린다"는 정책 */
}

/*
 * [한국어]
 * try_adjacent_combine() - prev와 prev+1 두 슬롯이 정확히 인접하고 ack가
 * 같으며 합친 길이가 BB_MAX_LEN 이하이면 하나로 합친다(꼬리 병합).
 *
 * @bb: 대상 badblocks 인스턴스.
 * @prev: 병합을 시도할 앞쪽 슬롯의 인덱스(-1이면 애초에 조건에서 걸러짐).
 * @return: 실제로 병합했으면 true(이 경우 bb->count가 이 함수 내부에서
 *          직접 1 감소한다 — 다른 헬퍼들과 달리 count 갱신을 스스로
 *          수행), 조건 불충족이면 false(count 불변).
 *
 * front_merge()/front_combine()이 "새로 들어오는 범위 처리 중" 병합을
 * 다룬다면, 이 함수는 그와 별개로 "이미 테이블에 있는 인접한 두 슬롯"을
 * 병합하는 범용 헬퍼다. 두 용도로 쓰인다 — (1) _badblocks_set()이 요청
 * 범위를 다 처리한 뒤 마지막으로 "방금 처리한 마지막 슬롯이 그 다음
 * 기존 슬롯과 우연히 인접하게 됐는지"(behind merge, 위 대형 영어 주석의
 * 6.3절) 확인할 때, (2) ack_all_badblocks()가 모든 슬롯을 acked로 바꾼
 * 뒤 인접한 슬롯들을 최대한 압축할 때(while 루프로 반복 호출).
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 테이블 수정 함수.
 * 호출자: _badblocks_set()(루프 종료 후 1회), ack_all_badblocks()(성공
 * 하는 동안 반복 호출).
 * 호출 대상: BB_* 매크로, memmove().
 * 에러 경로: 없음 — false 반환은 정상적인 "병합 대상 아님" 결과.
 *
 * 호출 체인:
 *   _badblocks_set()/ack_all_badblocks() → [try_adjacent_combine()]
 */
static bool try_adjacent_combine(struct badblocks *bb, int prev) /* 이미 테이블에 있는 prev와 prev+1 슬롯이 인접+ack일치+길이한도 조건을 만족하면 하나로 병합 */
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */

	if (prev >= 0 && (prev + 1) < bb->count && /* prev가 유효한 인덱스이고 prev+1도 배열 범위 안에 있어야 비교 가능 */
	    BB_END(p[prev]) == BB_OFFSET(p[prev + 1]) && /* 두 슬롯 사이에 gap이 전혀 없이 정확히 맞닿아 있어야 함 */
	    (BB_LEN(p[prev]) + BB_LEN(p[prev + 1])) <= BB_MAX_LEN && /* 합친 길이가 슬롯 하나의 표현 한도(512섹터)를 넘지 않아야 함 */
	    BB_ACK(p[prev]) == BB_ACK(p[prev + 1])) { /* 두 슬롯의 ack 상태가 같아야 병합해도 의미가 유지됨 */
		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* 시작은 앞쪽(prev) 슬롯 그대로 유지 */
				  BB_LEN(p[prev]) + BB_LEN(p[prev + 1]), /* 길이는 두 슬롯의 합으로 확장 */
				  BB_ACK(p[prev])); /* ack는 그대로(둘이 이미 동일함) */

		if ((prev + 2) < bb->count) /* prev+1 뒤에 아직 유효한 슬롯이 더 있으면 */
			memmove(p + prev + 1, p + prev + 2, /* prev+1 자리를 없애기 위해 그 뒤 슬롯들을 한 칸 앞으로 당김 */
				(bb->count -  (prev + 2)) * 8); /* 당겨야 할 슬롯 개수 * 슬롯 크기(8바이트) */
		bb->count--; /* 두 슬롯이 하나로 합쳐졌으므로 전체 슬롯 수를 1 감소 - 이 함수가 직접 갱신 */
		return true; /* 병합 성공 */
	}
	return false; /* 인접/ack/길이 조건 중 하나라도 불만족 - 병합하지 않음 */
}

/*
 * [한국어]
 * _badblocks_set() - badblocks_set()의 실제 구현. 요청 범위를 머리부터
 * 반복적으로 잘라가며 병합/결합/덮어쓰기/삽입 중 맞는 경로로 처리하는
 * 상태 기계(state machine)다. 이 파일 상단의 방대한 영어 주석이 설명하는
 * 규칙표를 그대로 코드로 옮긴 것이다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @s: 등록할 범위의 시작 섹터.
 * @sectors: 등록할 범위의 길이(섹터 수).
 * @acknowledged: 이 범위를 acked(1)로 등록할지 unacked(0)로 등록할지.
 * @return: true면 요청 범위 전체가 성공적으로 등록됨. false면 테이블이
 *          가득 차서 일부(또는 전부)를 등록하지 못함 — 부분 성공도
 *          실패로 취급한다(함수 상단 공개 API 문서 참고).
 *
 * 동작 과정: (1) shift>0이면 요청 범위를 정렬 단위로 내림/올림해 그
 * 단위의 배수로 맞춘다. (2) write_seqlock_irqsave로 쓰기 잠금을 건다
 * (인터럽트 컨텍스트에서의 호출 가능성을 대비해 irqsave 변형 사용).
 * (3) re_insert 레이블부터 시작하는 while 루프(goto로 구현)를 돈다 —
 * 매 반복마다 남은 [s, s+sectors) 중 "이번에 처리 가능한 앞부분"만
 * badblocks_context bad에 담아 아래 순서로 시도한다: 테이블이 꽉 찼으면
 * 포기(out) → 테이블이 비었으면 그냥 삽입 → prev_badblocks()로 기준
 * 슬롯을 찾아 (a) 모든 기존 슬롯보다 앞이면 첫 슬롯 앞에 삽입 (b) prev-1
 * 과 prev를 합칠 수 있으면 combine (c) prev와 합칠 수 있으면 merge (d)
 * prev와 겹치면 overwrite 시도(안 되면 겹치는 만큼 스킵) (e) 그 무엇도
 * 아니면 prev 뒤에(다음 슬롯과 겹치지 않을 만큼만 잘라) 새로 삽입.
 * (4) 각 분기는 처리한 길이(len)만큼 s/sectors를 전진시키고 sectors>0
 * 이면 re_insert로 돌아간다. (5) 루프가 끝나면 try_adjacent_combine()으로
 * 마지막에 처리한 슬롯이 그 다음 기존 슬롯과 우연히 인접해졌는지 한 번
 * 더 확인해 병합한다(위 대형 주석 6.3절, "behind merge"). (6) 뭔가
 * 등록했으면(added>0) changed와 unacked_exist를 갱신한다.
 * 실행 컨텍스트: 어떤 컨텍스트에서 호출될 수 있는지는 호출자에 달려
 * 있으나(sysfs store, null_blk configfs store, 혹은 MD 등 관리 경로),
 * 인터럽트를 비활성화하는 write_seqlock_irqsave를 쓰는 것으로 보아 이
 * 호출자 중 일부는 인터럽트 컨텍스트와 경합할 수 있음을 전제한다.
 * 호출자: badblocks_set()(그대로 위임). 호출 대상: prev_badblocks(),
 * can_combine_front(), can_merge_front(), front_merge(), front_combine(),
 * overlap_front(), can_front_overwrite(), front_overwrite(),
 * overlap_behind(), insert_at(), try_adjacent_combine(),
 * badblocks_update_acked(), set_changed().
 * 에러 경로: badblocks_full() 조기 발견 시 또는 can_front_overwrite()가
 * 공간 부족으로 false를 반환할 때 out으로 점프해 지금까지 처리한
 * 부분만 반영하고 나머지는 처리하지 못한 채(sectors > 0) 종료 —
 * 반환값 false로 호출자에게 알린다.
 *
 * 호출 체인:
 *   badblocks_set() → [_badblocks_set()] → prev_badblocks()/can_merge_front()/front_merge()/
 *                      can_combine_front()/front_combine()/overlap_front()/can_front_overwrite()/
 *                      front_overwrite()/overlap_behind()/insert_at()/try_adjacent_combine()/
 *                      badblocks_update_acked()
 */
static bool _badblocks_set(struct badblocks *bb, sector_t s, sector_t sectors, /* badblocks_set()의 실제 구현 - merge/combine/overwrite/insert 상태 기계 */
			   int acknowledged)
{
	int len = 0, added = 0; /* len: 이번 반복에서 실제 처리된 길이, added: 지금까지 등록/병합이 한 번이라도 일어났는지(0/양수) 카운트 */
	struct badblocks_context bad; /* 이번 반복에서 다루는 부분 범위(start/len)와 등록할 ack 값을 담는 임시 컨테이너 */
	int prev = -1, hint = -1; /* prev: prev_badblocks()가 찾은 기준 슬롯 인덱스, hint: 다음 반복에 넘길 탐색 힌트 - 둘 다 초기엔 "없음" */
	unsigned long flags; /* write_seqlock_irqsave가 저장하는 이전 인터럽트 상태 - unlock 시 복원용 */
	u64 *p; /* bad table 베이스 포인터 */

	if (bb->shift < 0) /* 이 badblocks 인스턴스 자체가 비활성화 상태(shift<0)이면 */
		/* badblocks are disabled */
		return false; /* 아무 것도 하지 않고 실패로 반환 - 비활성화된 인스턴스에는 등록할 수 없음 */

	if (sectors == 0) /* 길이 0인 요청은 의미가 없음 */
		/* Invalid sectors number */
		return false; /* 잘못된 인자로 간주해 조기 반환 */

	if (bb->shift) { /* 정렬 단위(shift)가 설정돼 있으면(0 초과) */
		/* round the start down, and the end up */
		sector_t next = s + sectors; /* 정렬 전 원래 끝 섹터를 미리 계산 */

		rounddown(s, 1 << bb->shift); /* 시작 섹터를 2^shift 단위로 내림 - 정렬 단위보다 앞부분까지 포함해 "불량일 수도 있는" 영역을 넓게 잡음(보수적) */
		roundup(next, 1 << bb->shift); /* 끝 섹터를 2^shift 단위로 올림 - 마찬가지로 뒷부분도 넓게 포함 */
		sectors = next - s; /* 정렬된 시작/끝으로부터 실제 처리할 길이를 다시 계산 */
	}

	write_seqlock_irqsave(&bb->lock, flags); /* seqlock 쓰기 측 획득 + 인터럽트 비활성화 - 이 인스턴스를 인터럽트 컨텍스트의 동시 접근으로부터도 보호 */

	bad.ack = acknowledged; /* 이번 호출 전체에 걸쳐 등록할 ack 값 고정 */
	p = bb->page; /* bad table 베이스 포인터 확보 */

re_insert: /* 요청 범위 중 아직 처리 못한 나머지를 다루는 반복 진입점 */
	bad.start = s; /* 이번 반복이 다룰 부분 범위의 시작 - 남은 범위의 현재 시작점 */
	bad.len = sectors; /* 이번 반복이 다룰 부분 범위의 길이 - 아래 분기들이 필요시 더 줄임 */
	len = 0; /* 이번 반복에서 실제 처리된 길이를 아직 모르므로 0으로 초기화 */

	if (badblocks_full(bb)) /* 슬롯이 이미 MAX_BADBLOCKS만큼 다 찼으면 */
		goto out; /* 더 이상 새 슬롯을 만들 수 없으므로 지금까지 처리한 것만 반영하고 종료 */

	if (badblocks_empty(bb)) { /* 테이블이 완전히 비어있으면(슬롯 0개) */
		len = insert_at(bb, 0, &bad); /* 비교할 기존 슬롯이 없으므로 바로 0번 위치에 삽입 */
		bb->count++; /* 슬롯 수 1 증가 */
		added++; /* 등록 발생 표시 */
		goto update_sectors; /* 이번 반복 종료 - 진행 상태 갱신으로 */
	}

	prev = prev_badblocks(bb, &bad, hint); /* bad->start 이하에서 시작하는 마지막 기존 슬롯 인덱스를 탐색(hint로 빠른 경로 우선) */

	/* start before all badblocks */
	if (prev < 0) { /* 그런 슬롯이 없음 - 즉 새 범위가 테이블의 모든 기존 슬롯보다 앞에서 시작 */
		/* insert on the first */
		if (bad.len > (BB_OFFSET(p[0]) - bad.start)) /* 이번에 처리하려는 길이가 첫 슬롯 시작 전까지의 간격보다 크면 */
			bad.len = BB_OFFSET(p[0]) - bad.start; /* 첫 슬롯 시작 지점까지만 처리하도록 길이를 잘라 - 겹침 방지 */
		len = insert_at(bb, 0, &bad); /* 배열 맨 앞(0번)에 새 슬롯으로 삽입 */
		bb->count++; /* 슬롯 수 증가 */
		added++; /* 등록 발생 표시 */
		hint = ++prev; /* prev를 0으로 만들고(원래 -1이었으므로 ++prev==0) 다음 반복의 탐색 힌트로 사용 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* in case p[prev-1] can be merged with p[prev] */
	if (can_combine_front(bb, prev, &bad)) { /* 새 범위 처리와 무관하게, prev-1과 prev 두 기존 슬롯 자체가 결합 가능한 상태이면(우연히 이번 bad->start와 prev 시작이 일치) */
		front_combine(bb, prev); /* 둘을 하나로 재인코딩하고 배열 압축 */
		bb->count--; /* 슬롯 하나가 사라졌으므로 카운트 감소 */
		added++; /* 변경 발생 표시(실제 새 섹터를 추가한 건 아니지만 테이블이 바뀌었으므로) */
		hint = prev; /* 다음 반복 힌트를 prev로(결합 후에도 유효한 인덱스) */
		goto update_sectors; /* len은 0인 채로 진행 - 이 분기는 길이를 처리한 게 아니라 기존 슬롯끼리 정리한 것이므로 다음 반복에서 같은 bad->start를 다시 검사하게 됨 */
	}

	if (can_merge_front(bb, prev, &bad)) { /* 새 범위를 prev 슬롯 끝에 이어붙일 수 있으면(ack 일치 + 인접/중첩) */
		len = front_merge(bb, prev, &bad); /* 실제로 병합(흡수) 수행 - 병합된 섹터 수를 len으로 받음 */
		added++; /* 등록 발생 표시 */
		hint = prev; /* 다음 반복 힌트 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	if (overlap_front(bb, prev, &bad)) { /* 새 범위의 시작점이 prev 슬롯 내부에 있으면(겹침) */
		int extra = 0; /* can_front_overwrite()가 계산해줄 추가 슬롯 필요 수 - 기본 0으로 초기화 */

		if (!can_front_overwrite(bb, prev, &bad, &extra)) { /* prev가 이미 acked이거나(우선순위 낮음) 분할에 필요한 여유 슬롯이 없으면 덮어쓰기 불가 */
			if (extra > 0) /* extra가 0 초과로 설정됐다는 것은 "ack 조건은 통과했지만 공간이 부족해서" 실패했다는 의미 */
				goto out; /* 공간 부족은 이번 호출 전체를 실패로 처리하고 즉시 종료(부분 처리분만 반영) */

			len = min_t(sector_t, /* ack 우선순위가 낮아 덮어쓸 수 없는 경우 - 이미 acked인 기존 범위와 겹치는 만큼은 그냥 건너뜀(성공으로 취급) */
				    BB_END(p[prev]) - s, sectors); /* 겹치는 길이만큼만 스킵 처리 - prev 끝까지와 남은 sectors 중 작은 쪽 */
			hint = prev; /* 다음 반복 힌트 */
			goto update_sectors; /* len만큼 건너뛰고 다음 반복에서 이어서 처리 */
		}

		len = front_overwrite(bb, prev, &bad, extra); /* 덮어쓰기 가능 판정을 받았으므로 실제로 prev를 교체/분할해 덮어씀 */
		added++; /* 등록 발생 표시 */
		bb->count += extra; /* front_overwrite()가 늘린 슬롯 수(0/1/2)를 카운트에 반영 */

		if (can_combine_front(bb, prev, &bad)) { /* 덮어쓴 결과 prev가 그 앞(prev-1)과 다시 인접/ack일치하게 됐는지 재확인(위 대형 주석 6.2절 "덮어쓰기 후 앞 병합") */
			front_combine(bb, prev); /* 가능하면 즉시 합쳐 공간을 더 절약 */
			bb->count--; /* 합쳐진 만큼 슬롯 수 회수 */
		}

		hint = prev; /* 다음 반복 힌트 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* cannot merge and there is space in bad table */
	if ((prev + 1) < bb->count && /* prev 다음 슬롯이 실제로 존재하고 */
	    overlap_behind(bb, &bad, prev + 1)) /* 새 범위의 끝이 그 다음 슬롯 시작을 침범하면 */
		bad.len = min_t(sector_t,
				bad.len, BB_OFFSET(p[prev + 1]) - bad.start); /* 다음 슬롯 시작 지점까지만 처리하도록 길이를 잘라 - 겹침 방지 */

	len = insert_at(bb, prev + 1, &bad); /* 앞의 모든 특수 케이스에 해당하지 않는 "완전히 새로운 구간" - prev 바로 뒤에 새 슬롯으로 삽입 */
	bb->count++; /* 슬롯 수 증가 */
	added++; /* 등록 발생 표시 */
	hint = ++prev; /* prev를 새로 삽입한 위치로 갱신해 다음 반복 힌트로 사용 */

update_sectors: /* 이번 반복에서 처리한 길이(len)만큼 진행 상태를 전진시키는 공통 지점 */
	s += len; /* 처리된 만큼 시작 섹터를 전진 */
	sectors -= len; /* 남은 길이를 그만큼 감소 */

	if (sectors > 0) /* 아직 처리하지 못한 부분이 남아있으면 */
		goto re_insert; /* 다음 반복으로 - 남은 [s, s+sectors)에 대해 처음부터 다시 판정 */

	/*
	 * Check whether the following already set range can be
	 * merged. (prev < 0) condition is not handled here,
	 * because it's already complicated enough.
	 */
	try_adjacent_combine(bb, prev); /* 요청 범위를 모두 처리한 뒤, 마지막으로 다룬 prev 슬롯이 그 다음 기존 슬롯과 우연히 인접해졌다면 마지막으로 한 번 더 병합 시도(behind merge) */

out: /* badblocks_full() 조기 발견 또는 공간 부족으로 인한 조기 종료 지점이기도 함 */
	if (added) { /* 이번 호출에서 테이블에 어떤 형태로든 변경이 있었으면 */
		set_changed(bb); /* "테이블이 바뀌었다" 플래그를 세워 호출자가 영속 메타데이터 동기화 필요성을 알 수 있게 함 */

		if (!acknowledged) /* 이번에 등록한 것이 unacked 범위였다면 */
			bb->unacked_exist = 1; /* "unacked 슬롯이 있을 수 있다" 힌트를 세팅(정확한 재계산은 badblocks_update_acked()가 나중에) */
		else /* acked로 등록했다면 */
			badblocks_update_acked(bb); /* 방금 등록으로 인해 기존 unacked_exist 힌트가 이제 거짓이 됐을 수 있으니 재계산 */
	}

	write_sequnlock_irqrestore(&bb->lock, flags); /* seqlock 쓰기 잠금 해제 + 저장해둔 인터럽트 상태 복원 - 이 시점 이후 read_seqretry가 변경을 감지 가능 */

	return sectors == 0; /* 남은 처리분이 0이면(요청 전체 처리 완료) true, 조기 종료로 남은 게 있으면 false */
}

/*
 * [한국어]
 * front_clear() - 해제(clear) 범위가 prev 슬롯의 머리와 일치하거나
 * 꼬리 쪽에서 겹칠 때 해당 부분을 제거한다(중앙 겹침은 다루지 않음 -
 * 그 경우는 front_splitting_clear()의 몫이며 여기선 BUG()로 방어).
 *
 * @bb: 대상 badblocks 인스턴스.
 * @prev: 겹치는 기존 슬롯의 인덱스.
 * @bad: 해제하려는 범위(start/len). ack는 이 함수에서 쓰이지 않는다
 *       (clear는 항상 acked=true로 호출되지만, 지우는 대상 슬롯의 ack는
 *       그대로 유지되고 새 ack가 대입되는 게 아니기 때문).
 * @deleted: [출력] prev 슬롯 전체가 삭제되어 배열에서 사라졌으면 1,
 *           머리/꼬리 일부만 잘려 슬롯이 남아있으면 0.
 * @return: 실제로 해제(제거)된 길이.
 *
 * 세 가지 경우를 처리한다: (1) 해제 시작이 prev 시작과 일치하고 prev가
 * 더 길면 → prev의 시작을 뒤로 밀고 길이를 줄여 "머리를 자른" 형태로
 * 남긴다. (2) 해제 시작이 prev 시작과 일치하고 해제 길이가 prev 전체를
 * 덮으면 → prev 슬롯 자체를 배열에서 삭제(memmove로 뒤 슬롯 당김).
 * (3) 해제 시작이 prev 시작보다 뒤이면서 해제 범위가 prev 끝까지
 * 이어지면 → prev의 길이만 줄여 "꼬리를 자른" 형태로 남�다. 그 외
 * (해제 범위가 prev의 순수한 중앙만 덮어 머리/꼬리가 모두 남는 경우)는
 * 이 함수의 처리 범위가 아니므로 BUG()로 잡는다 — 호출자
 * (_badblocks_clear)가 이 경우를 미리 걸러내 front_splitting_clear()로
 * 보내야 한다는 불변식이 깨진 것이므로 커널 패닉으로 조기 발견한다.
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 테이블 수정 함수.
 * 호출자: _badblocks_clear() — overlap_front()가 참이고 "중앙 분할"
 * 조건은 아닐 때.
 * 호출 대상: BB_* 매크로, memmove(), BUG().
 * 에러 경로: 위에서 설명한 대로 BUG()는 불변식 위반을 나타내는 방어적
 * 코드이며 정상 흐름에서는 도달하지 않는다.
 *
 * 호출 체인:
 *   _badblocks_clear() → [front_clear()]
 */
static int front_clear(struct badblocks *bb, int prev, /* prev 슬롯의 머리 또는 꼬리와 겹치는 해제 범위를 실제로 잘라내거나 슬롯 전체를 삭제 */
		       struct badblocks_context *bad, int *deleted)
{
	sector_t sectors = bad->len; /* 해제하려는 길이 */
	sector_t s = bad->start; /* 해제 시작 섹터 */
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	int cleared = 0; /* 실제로 해제된 길이 - 함수 종료 시 반환됨 */

	*deleted = 0; /* 기본값 - "슬롯이 삭제되지 않고 남아있음"으로 초기화, 아래에서 필요시 1로 갱신 */
	if (s == BB_OFFSET(p[prev])) { /* 해제 시작이 prev 슬롯 시작과 정확히 일치 - 머리 쪽 케이스 */
		if (BB_LEN(p[prev]) > sectors) { /* prev가 해제 범위보다 길면(prev의 일부만 지워짐) */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]) + sectors, /* 새 시작 = 해제 범위 끝 지점 */
					  BB_LEN(p[prev]) - sectors, /* 새 길이 = 원래 길이 - 해제된 길이 */
					  BB_ACK(p[prev])); /* ack는 원래 그대로 유지 - "지워진 머리" 이후 나머지는 여전히 같은 불량 구간 */
			cleared = sectors; /* 요청한 길이 전부가 실제로 해제됨 */
		} else { /* prev 길이가 해제 범위 이하 - prev 전체가 해제 대상 */
			/* BB_LEN(p[prev]) <= sectors */
			cleared = BB_LEN(p[prev]); /* prev의 전체 길이만큼만 실제로 해제(요청이 더 길어도 이 슬롯에서는 이만큼만) */
			if ((prev + 1) < bb->count) /* 뒤에 슬롯이 더 있으면 */
				memmove(p + prev, p + prev + 1, /* prev 슬롯 자리를 없애기 위해 뒤 슬롯들을 한 칸 앞으로 당김 */
				       (bb->count - prev - 1) * 8); /* 당겨야 할 슬롯 개수 * 슬롯 크기(8바이트) */
			*deleted = 1; /* prev 슬롯이 배열에서 완전히 제거됐음을 호출자에게 알림 - 호출자가 count를 줄여야 함 */
		}
	} else if (s > BB_OFFSET(p[prev])) { /* 해제 시작이 prev 시작보다 뒤 - 꼬리 쪽 또는 중앙 케이스 */
		if (BB_END(p[prev]) <= (s + sectors)) { /* 해제 범위가 prev의 끝까지(또는 그 너머까지) 이어짐 - 꼬리 쪽 케이스 */
			cleared = BB_END(p[prev]) - s; /* 실제로 해제되는 길이는 prev 끝까지만 */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* 시작은 그대로 유지 */
					  s - BB_OFFSET(p[prev]), /* 길이를 줄여 해제 시작 지점 이전까지만 남김 */
					  BB_ACK(p[prev])); /* ack는 원래 그대로 유지 */
		} else { /* 해제 범위가 prev 끝에 못 미침 - 즉 prev의 순수한 중앙만 덮음 */
			/* Splitting is handled in front_splitting_clear() */
			BUG(); /* 이 경우는 이 함수가 처리할 수 없음 - 호출자가 front_splitting_clear()로 미리 분기했어야 하는 불변식 위반이므로 즉시 패닉으로 조기 발견 */
		}
	}

	return cleared; /* 실제로 해제된 길이 - 호출자가 진행 상태를 전진시키는 데 사용 */
}

/*
 * [한국어]
 * front_splitting_clear() - 해제 범위가 prev 슬롯의 순수한 중앙(머리도
 * 꼬리도 아닌 안쪽)만 덮을 때, prev 하나를 머리/꼬리 두 슬롯으로
 * 분할한다.
 *
 * @bb: 대상 badblocks 인스턴스 (bb->count는 호출자가 이 함수 호출 후
 *      직접 +1 한다 — 이 함수는 count를 건드리지 않음).
 * @prev: 분할 대상 슬롯 인덱스.
 * @bad: 해제할 범위(start/len) - 반드시 prev 내부에 완전히 포함되어
 *       머리와 꼬리 모두 남는 경우여야 한다(그렇지 않으면 front_clear()
 *       가 처리해야 할 케이스).
 * @return: 해제된 길이(= bad->len 그대로, 이 케이스는 항상 요청한 만큼
 *          전부 해제 가능).
 *
 * prev 슬롯을 (원래 시작 ~ 해제 시작)의 "머리"로 축소하고, 그 뒤에 새
 * 슬롯 하나를 끼워 넣어 (해제 끝 ~ 원래 끝)의 "꼬리"로 채운다. 머리/
 * 꼬리 모두 원래의 ack 값을 그대로 물려받는다(해제는 ack를 바꾸는
 * 연산이 아니라 순수하게 구간을 지우는 연산이므로).
 * 실행 컨텍스트: seqlock 쓰기 측 보유 중 호출되는 테이블 수정 함수.
 * 호출자: _badblocks_clear() — 해제 범위가 prev의 순수한 중앙만 덮고,
 * 테이블에 분할용 여유 슬롯이 있을 때만.
 * 호출 대상: BB_* 매크로, memmove().
 * 에러 경로: 없음 — 호출자가 이미 여유 슬롯 존재를 확인한 뒤에만 호출.
 *
 * 호출 체인:
 *   _badblocks_clear() → [front_splitting_clear()] (복귀 후 호출자가 bb->count++)
 */
static int front_splitting_clear(struct badblocks *bb, int prev, /* prev 슬롯의 순수한 중앙을 해제 - 머리/꼬리 두 슬롯으로 분할 */
				  struct badblocks_context *bad)
{
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	u64 end = BB_END(p[prev]); /* 분할 후 꼬리 길이 계산에 필요한 - prev의 원래 끝 섹터 보존 */
	int ack = BB_ACK(p[prev]); /* 머리/꼬리 모두에 물려줄 - prev의 원래 ack 보존 */
	sector_t sectors = bad->len; /* 해제할 길이 */
	sector_t s = bad->start; /* 해제 시작 섹터 */

	p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* 시작은 prev 원래 시작 그대로 유지 */
			  s - BB_OFFSET(p[prev]), /* 길이를 줄여 해제 시작 지점 이전까지만("머리")으로 축소 */
			  ack); /* 머리는 원래 ack 상태 유지 */
	memmove(p + prev + 2, p + prev + 1, (bb->count - prev - 1) * 8); /* 꼬리를 담을 prev+1 자리를 만들기 위해 뒤 슬롯들을 한 칸 밀기 */
	p[prev + 1] = BB_MAKE(s + sectors, end - s - sectors, ack); /* 꼬리 슬롯 = (해제 끝 ~ prev 원래 끝), ack는 원래 값 유지 */
	return sectors; /* 요청한 길이 전부를 해제했으므로 그대로 반환 */
}

/*
 * [한국어]
 * _badblocks_clear() - badblocks_clear()의 실제 구현. 요청 범위를
 * 머리부터 반복하며 "겹치지 않음(성공 처리)/머리·꼬리 겹침/중앙 겹침
 * (분할)" 세 경우로 분류해 처리하는, _badblocks_set()보다 단순한 상태
 * 기계다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @s: 해제할 범위의 시작 섹터.
 * @sectors: 해제할 범위의 길이.
 * @return: true면 (일부라도) 성공적으로 처리(해제 또는 원래 불량이
 *          아니었음을 확인). false는 오직 "테이블이 꽉 차서 중앙 분할이
 *          필요한데 분할 슬롯을 못 만들어 완전히 아무 것도 못한" 극히
 *          드문 경우에만 발생 — 대부분의 경로는 "해제할 불량이 없었다"
 *          도 성공(cleared++)으로 취급한다는 점이 badblocks_set()과의
 *          중요한 차이다.
 *
 * 동작 과정: (1) shift가 있으면 이번엔 반대 방향으로 정렬한다 — 시작은
 * 올리고 끝은 내려서(rounddown 대신 roundup) 실제로 지울 영역을 원래
 * 요청보다 "보수적으로 좁게" 잡는다(주석에 설명된 대로: 불량이 아닌
 * 블록을 불량이라 여기는 것이, 불량인 블록을 정상이라 여기는 것보다
 * 안전하기 때문). (2) write_seqlock_irq로 쓰기 잠금(이 함수는 irqsave가
 * 아닌 irq만 사용 — 반환 전 항상 인터럽트를 켠 상태로 호출된다는 다른
 * 전제). (3) re_clear 루프에서 매 반복 prev_badblocks()로 기준 슬롯을
 * 찾아 순서대로 검사: 테이블이 비었으면 전체가 성공적으로 "정상"
 * → 모든 슬롯보다 앞이면(prev<0) 첫 슬롯 전까지만 정상 처리 → 모든
 * 슬롯보다 뒤이면 전체가 정상 → 테이블이 꽉 찬 상태에서 분할이
 * 필요하면(중앙 겹침인데 여유 슬롯 없음) 억지로 통째로 "처리한 것"으로
 * 넘김(불량 정보가 소실될 수 있는 유일한 손실 경로) → prev와 겹치면
 * (중앙이면 front_splitting_clear, 아니면 front_clear) → prev와는 안
 * 겹치는데 다음 슬롯과 겹치면 그 시작 전까지만 정상 처리 → 그 무엇도
 * 아니면(bad table에 해당 구간 자체가 없음) 전체가 정상. (4) 처리한
 * 길이(len)만큼 진행하고 남았으면 재반복. (5) 뭔가 지웠으면(cleared>0)
 * unacked_exist와 changed를 갱신.
 * 실행 컨텍스트: 호출자에 따라 다르나 seqlock 쓰기 측(인터럽트 비활성화
 * 포함, 단 irqsave가 아니므로 반환 시 무조건 인터럽트 활성 상태 가정)에서
 * 실행.
 * 호출자: badblocks_clear()(그대로 위임). 호출 대상: prev_badblocks(),
 * overlap_behind(), overlap_front(), badblocks_full(), badblocks_empty(),
 * front_splitting_clear(), front_clear(), badblocks_update_acked(),
 * set_changed().
 * 에러 경로: "테이블이 꽉 차서 분할 불가"인 경우를 제외하면 이 함수는
 * 사실상 실패하지 않는다 — "지울 불량이 없었다"도 정상 성공 결과다.
 *
 * 호출 체인:
 *   badblocks_clear() → [_badblocks_clear()] → prev_badblocks()/overlap_front()/overlap_behind()/
 *                        front_clear()/front_splitting_clear()/badblocks_update_acked()
 */
static bool _badblocks_clear(struct badblocks *bb, sector_t s, sector_t sectors) /* badblocks_clear()의 실제 구현 - 정상/머리·꼬리 겹침/중앙 분할 세 갈래로 분류해 처리 */
{
	struct badblocks_context bad; /* 이번 반복에서 다루는 부분 해제 범위(start/len)와 고정 ack(true)를 담는 임시 컨테이너 */
	int prev = -1, hint = -1; /* prev: 기준 슬롯 인덱스, hint: 다음 반복 탐색 힌트 */
	int len = 0, cleared = 0; /* len: 이번 반복 처리 길이, cleared: 지금까지 "성공"으로 처리된 반복 횟수(0이면 완전 실패) */
	u64 *p; /* bad table 베이스 포인터 */

	if (bb->shift < 0) /* 이 인스턴스가 비활성화 상태이면 */
		/* badblocks are disabled */
		return false; /* 처리할 수 없음을 알림 */

	if (sectors == 0) /* 길이 0 요청은 의미 없음 */
		/* Invalid sectors number */
		return false; /* 조기 반환 */

	if (bb->shift) { /* 정렬 단위가 설정돼 있으면 */
		sector_t target; /* 정렬된 끝 섹터를 담을 지역 변수 */

		/* When clearing we round the start up and the end down.
		 * This should not matter as the shift should align with
		 * the block size and no rounding should ever be needed.
		 * However it is better the think a block is bad when it
		 * isn't than to think a block is not bad when it is.
		 */
		target = s + sectors; /* 정렬 전 원래 끝 섹터 계산 */
		roundup(s, 1 << bb->shift); /* 시작을 올림 - badblocks_set()과 반대 방향으로, 실제 해제 영역을 원래 요청보다 좁게 잡아 "지우지 말아야 할 불량"까지 지우는 실수를 방지 */
		rounddown(target, 1 << bb->shift); /* 끝을 내림 - 마찬가지로 보수적으로 좁게 */
		sectors = target - s; /* 정렬된 시작/끝으로부터 실제 처리할 길이 재계산 */
	}

	write_seqlock_irq(&bb->lock); /* seqlock 쓰기 측 획득(인터럽트 비활성화, irqsave 아님 - 이전 인터럽트 상태를 저장하지 않고 그냥 켠다는 전제) */

	bad.ack = true; /* clear는 개념적으로 "언제나 확실한(acked) 연산" - can_front_overwrite 류의 ack 비교에서 항상 우선순위를 가짐 */
	p = bb->page; /* bad table 베이스 포인터 확보 */

re_clear: /* 남은 해제 요청을 반복 처리하는 진입점 */
	bad.start = s; /* 이번 반복이 다룰 시작 섹터 */
	bad.len = sectors; /* 이번 반복이 다룰 남은 길이 */

	if (badblocks_empty(bb)) { /* 테이블이 완전히 비어있으면 */
		len = sectors; /* 지울 대상 자체가 없으므로 요청 전체를 "이미 정상"으로 간주 */
		cleared++; /* 지울 불량이 없는 것도 성공으로 카운트 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	prev = prev_badblocks(bb, &bad, hint); /* bad->start 이하에서 시작하는 마지막 기존 슬롯 탐색 */

	/* Start before all badblocks */
	if (prev < 0) { /* 그런 슬롯이 없음 - 새 범위가 모든 기존 슬롯보다 앞에서 시작 */
		if (overlap_behind(bb, &bad, 0)) { /* 그래도 범위 끝이 첫 슬롯 시작을 침범하면 */
			len = BB_OFFSET(p[0]) - s; /* 첫 슬롯 시작 전까지만 "정상 영역 해제"로 처리 */
			hint = 0; /* 다음 반복 힌트 */
		} else {
			len = sectors; /* 첫 슬롯과도 안 겹치면 요청 전체가 정상 영역 */
		}
		/*
		 * Both situations are to clear non-bad range,
		 * should be treated as successful
		 */
		cleared++; /* 두 경우 모두 "정상 영역을 해제한 것"이므로 성공 처리 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* Start after all badblocks */
	if ((prev + 1) >= bb->count && !overlap_front(bb, prev, &bad)) { /* prev가 마지막 슬롯이고 그 슬롯과도 안 겹치면 - 요청 범위가 모든 불량 뒤에 있음 */
		len = sectors; /* 전체가 정상 영역 */
		cleared++; /* 성공 처리 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* Clear will split a bad record but the table is full */
	if (badblocks_full(bb) && (BB_OFFSET(p[prev]) < bad.start) && /* 테이블이 꽉 찬 상태이고 prev 시작이 해제 시작보다 앞이며 */
	    (BB_END(p[prev]) > (bad.start + sectors))) { /* prev 끝이 해제 범위 끝보다도 뒤 - 즉 prev가 해제 범위를 양쪽으로 완전히 감싸는 "중앙 분할"이 필요한 상황인데 */
		len = sectors; /* 분할할 여유 슬롯이 없으므로 어쩔 수 없이 요청 전체를 "처리한 것"으로 넘김 - 실제로는 해당 구간의 불량 정보가 소실될 수 있는 유일한 손실 경로 */
		goto update_sectors; /* 진행 상태 갱신으로(cleared는 증가시키지 않음에 유의 - 이 분기만 cleared++ 없이 넘어감) */
	}

	if (overlap_front(bb, prev, &bad)) { /* 해제 시작점이 prev 슬롯 안에 있으면 */
		if ((BB_OFFSET(p[prev]) < bad.start) && /* prev 시작이 해제 시작보다 앞이고 */
		    (BB_END(p[prev]) > (bad.start + bad.len))) { /* prev 끝이 해제 범위 끝보다 뒤 - 즉 해제 범위가 prev의 순수한 중앙만 덮음(양쪽 다 남음) */
			/* Splitting */
			if ((bb->count + 1) <= MAX_BADBLOCKS) { /* 슬롯 하나를 더 만들 여유가 있으면 */
				len = front_splitting_clear(bb, prev, &bad); /* prev를 머리/꼬리 두 슬롯으로 분할 */
				bb->count += 1; /* 슬롯 하나 증가 반영 */
				cleared++; /* 성공 처리 */
			} else {
				/* No space to split, give up */
				len = sectors; /* 분할할 여유가 없으므로 이번 반복은 전체를 그냥 넘김(불량 정보 유지, 해제는 실패) - 위 badblocks_full() 분기와 달리 여기선 count+1만 넘는 미세한 경우 */
			}
		} else {
			int deleted = 0; /* front_clear()가 슬롯을 완전히 지웠는지 여부를 받을 출력 변수 */

			len = front_clear(bb, prev, &bad, &deleted); /* 머리 또는 꼬리 겹침을 실제로 잘라내거나(또는 슬롯 전체 삭제) 처리 */
			bb->count -= deleted; /* 슬롯이 삭제됐다면(deleted==1) 카운트 감소 반영 */
			cleared++; /* 성공 처리 */
			hint = prev; /* 다음 반복 힌트 */
		}

		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* Not front overlap, but behind overlap */
	if ((prev + 1) < bb->count && overlap_behind(bb, &bad, prev + 1)) { /* prev와는 안 겹치지만 그 다음 슬롯과는 겹치면 */
		len = BB_OFFSET(p[prev + 1]) - bad.start; /* 다음 슬롯 시작 전까지만 "정상 영역 해제"로 처리 */
		hint = prev + 1; /* 다음 반복 힌트 */
		/* Clear non-bad range should be treated as successful */
		cleared++; /* 성공 처리 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* Not cover any badblocks range in the table */
	len = sectors; /* 어떤 기존 슬롯과도 겹치지 않음 - 요청 전체가 테이블에 없는 정상 영역 */
	/* Clear non-bad range should be treated as successful */
	cleared++; /* 성공 처리 */

update_sectors: /* 이번 반복 처리 길이(len)만큼 진행 상태를 전진시키는 공통 지점 */
	s += len; /* 시작 섹터 전진 */
	sectors -= len; /* 남은 길이 감소 */

	if (sectors > 0) /* 아직 남은 부분이 있으면 */
		goto re_clear; /* 다음 반복으로 */

	if (cleared) { /* 이번 호출에서 한 번이라도 "성공" 처리가 있었으면 */
		badblocks_update_acked(bb); /* 슬롯 삭제/분할로 인해 unacked_exist 힌트가 바뀌었을 수 있으니 재계산 */
		set_changed(bb); /* 테이블 변경 플래그 세팅 */
	}

	write_sequnlock_irq(&bb->lock); /* seqlock 쓰기 잠금 해제 */

	if (!cleared) /* 반복 전체에서 단 한 번도 성공 처리가 없었으면(테이블 꽉 참으로 인한 조기 포기만 반복된 극단적 경우) */
		return false; /* 실패로 반환 */

	return true; /* 적어도 한 번은 성공적으로 처리됨 */
}

/*
 * [한국어]
 * _badblocks_check() - badblocks_check()의 실제 구현. [s, s+sectors)
 * 범위를 머리부터 반복하며 테이블과의 겹침 여부/ack 상태를 누적 집계해
 * 최종 판정(0/1/-1)과 첫 번째로 겹친 불량 구간을 알아낸다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @s: 검사할 범위의 시작 섹터.
 * @sectors: 검사할 범위의 길이.
 * @first_bad: [출력] 겹친 첫 불량 구간의 시작 섹터(겹침이 하나도 없으면
 *             건드리지 않음 - 호출자는 반환값이 0이 아닐 때만 참고).
 * @bad_sectors: [출력] 그 첫 불량 구간의 길이.
 * @return: 0(겹치는 불량 없음), 1(겹치는 불량이 모두 acked), -1(겹치는
 *          불량 중 unacked가 하나라도 있음 - 상위 계층이 아직 확인하지
 *          않은 결함이므로 더 보수적으로 처리해야 함을 시사).
 *
 * badblocks_set()의 상태 기계와 뼈대는 비슷하지만(반복하며 prev_badblocks
 * 로 기준 슬롯을 찾고 앞/뒤 겹침을 분류), 이 함수는 테이블을 변경하지
 * 않는 순수 조회이며 대신 acked_badblocks/unacked_badblocks 두 카운터를
 * 누적한다는 점이 다르다. 겹치는 슬롯을 만날 때마다 그 슬롯의 ack 값에
 * 따라 두 카운터 중 하나를 증가시키고, "처음 만난" 불량 구간만
 * first_bad/bad_sectors에 기록한다(set 플래그로 최초 1회만 기록). 루프가
 * 끝난 뒤 unacked_badblocks가 하나라도 있으면 unacked를 우선해 -1을,
 * 없고 acked만 있으면 1을, 아예 없으면 0을 반환한다 — 이는 "확인되지
 * 않은 결함 가능성이 하나라도 있으면 안전한 쪽으로 판단한다"는 정책이다.
 * 실행 컨텍스트: 호출자(badblocks_check())가 seqlock 읽기 측(락 없는
 * 낙관적 읽기) 안에서 호출하는 순수 조회 함수 — 이 함수 자체는 그 어떤
 * 락도 잡지 않으므로, 실행 도중 테이블이 바뀌면 badblocks_check()가
 * read_seqretry로 감지해 전체를 재시도한다.
 * 호출자: badblocks_check()(그대로 위임). 호출 대상: prev_badblocks(),
 * overlap_front(), overlap_behind(), badblocks_empty().
 * 에러 경로: WARN_ON(sectors < len)은 계산 로직이 요청 길이보다 더 많이
 * "처리"해버리는 불변식 위반을 잡는 방어적 코드 — 정상 흐름에서는
 * 발생하지 않는다.
 *
 * 호출 체인:
 *   badblocks_check() → [_badblocks_check()] → prev_badblocks()/overlap_front()/overlap_behind()
 */
static int _badblocks_check(struct badblocks *bb, sector_t s, sector_t sectors, /* badblocks_check()의 실제 구현 - 겹침 여부와 ack 상태를 누적 집계해 0/1/-1 판정 */
			    sector_t *first_bad, sector_t *bad_sectors)
{
	int prev = -1, hint = -1, set = 0; /* prev: 기준 슬롯, hint: 다음 반복 탐색 힌트, set: first_bad/bad_sectors를 이미 기록했는지(최초 1회 제한) */
	struct badblocks_context bad; /* 이번 반복에서 검사하는 부분 범위(start/len) - ack는 이 구조체에서 쓰이지 않음(조회 전용이므로) */
	int unacked_badblocks = 0; /* 겹친 슬롯 중 unacked(ack=0)였던 횟수 누적 */
	int acked_badblocks = 0; /* 겹친 슬롯 중 acked(ack=1)였던 횟수 누적 */
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	int len, rv; /* len: 이번 반복에서 검사 완료로 처리된 길이, rv: 최종 반환값(0/1/-1) */

re_check: /* 남은 검사 범위를 반복 처리하는 진입점 */
	bad.start = s; /* 이번 반복이 검사할 시작 섹터 */
	bad.len = sectors; /* 이번 반복이 검사할 남은 길이 */

	if (badblocks_empty(bb)) { /* 테이블이 완전히 비어있으면 */
		len = sectors; /* 비교할 불량 자체가 없으므로 요청 전체를 "정상"으로 확정 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	prev = prev_badblocks(bb, &bad, hint); /* bad->start 이하에서 시작하는 마지막 기존 슬롯 탐색 */

	/* start after all badblocks */
	if ((prev >= 0) && /* 그런 슬롯이 실제로 존재하고 */
	    ((prev + 1) >= bb->count) && !overlap_front(bb, prev, &bad)) { /* 그 슬롯이 마지막 슬롯이면서 그 슬롯과도 안 겹치면 - 검사 범위가 모든 불량 뒤에 있음 */
		len = sectors; /* 전체가 정상 영역 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* Overlapped with front badblocks record */
	if ((prev >= 0) && overlap_front(bb, prev, &bad)) { /* 검사 범위 시작점이 prev 슬롯 안에 있으면(겹침) */
		if (BB_ACK(p[prev])) /* 그 슬롯이 acked 상태이면 */
			acked_badblocks++; /* acked 겹침 카운터 증가 */
		else /* unacked 상태이면 */
			unacked_badblocks++; /* unacked 겹침 카운터 증가 - 이 카운터가 하나라도 있으면 최종 판정이 -1이 됨 */

		if (BB_END(p[prev]) >= (s + sectors)) /* prev 슬롯이 남은 검사 범위 전체를 덮으면 */
			len = sectors; /* 이번 반복에서 남은 부분 전체를 처리 완료로 간주 */
		else /* prev 슬롯이 검사 범위 일부만 덮으면(끝이 prev 안에서 끝남) */
			len = BB_END(p[prev]) - s; /* prev 슬롯 끝까지만 처리 완료 - 나머지는 다음 반복에서 재검사 */

		if (set == 0) { /* 아직 첫 번째 겹친 불량 구간을 기록하지 않았으면 */
			*first_bad = BB_OFFSET(p[prev]); /* 호출자에게 알려줄 첫 겹침 구간의 시작 섹터 */
			*bad_sectors = BB_LEN(p[prev]); /* 그 구간의 길이 */
			set = 1; /* 이후 반복에서는 다시 덮어쓰지 않도록 표시 */
		}
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* Not front overlap, but behind overlap */
	if ((prev + 1) < bb->count && overlap_behind(bb, &bad, prev + 1)) { /* prev와는 안 겹치지만 그 다음 슬롯과는 겹치면 */
		len = BB_OFFSET(p[prev + 1]) - bad.start; /* 다음 슬롯 시작 전까지만 "정상"으로 처리 완료 */
		hint = prev + 1; /* 다음 반복 탐색 힌트 */
		goto update_sectors; /* 진행 상태 갱신으로 */
	}

	/* not cover any badblocks range in the table */
	len = sectors; /* 어떤 기존 슬롯과도 겹치지 않음 - 남은 부분 전체가 정상 영역 */

update_sectors: /* 이번 반복 처리 길이(len)만큼 진행 상태를 전진시키는 공통 지점 */
	/* This situation should never happen */
	WARN_ON(sectors < len); /* 방어적 검증 - 처리했다고 계산한 길이가 남은 요청 길이보다 클 수 없다는 불변식 확인 */

	s += len; /* 시작 섹터 전진 */
	sectors -= len; /* 남은 길이 감소 */

	if (sectors > 0) /* 아직 검사할 부분이 남아있으면 */
		goto re_check; /* 다음 반복으로 */

	if (unacked_badblocks > 0) /* 겹친 불량 중 unacked가 하나라도 있었으면 */
		rv = -1; /* "아직 확인되지 않은 불량 있음" - 가장 보수적인 판정을 우선 적용 */
	else if (acked_badblocks > 0) /* unacked는 없고 acked 겹침만 있었으면 */
		rv = 1; /* "확인된 불량만 있음" */
	else /* 겹침이 전혀 없었으면 */
		rv = 0; /* "불량 없음" */

	return rv; /* 최종 판정 반환 */
}

/**
 * badblocks_check() - check a given range for bad sectors
 * @bb:		the badblocks structure that holds all badblock information
 * @s:		sector (start) at which to check for badblocks
 * @sectors:	number of sectors to check for badblocks
 * @first_bad:	pointer to store location of the first badblock
 * @bad_sectors: pointer to store number of badblocks after @first_bad
 *
 * We can record which blocks on each device are 'bad' and so just
 * fail those blocks, or that stripe, rather than the whole device.
 * Entries in the bad-block table are 64bits wide.  This comprises:
 * Length of bad-range, in sectors: 0-511 for lengths 1-512
 * Start of bad-range, sector offset, 54 bits (allows 8 exbibytes)
 *  A 'shift' can be set so that larger blocks are tracked and
 *  consequently larger devices can be covered.
 * 'Acknowledged' flag - 1 bit. - the most significant bit.
 *
 * Locking of the bad-block table uses a seqlock so badblocks_check
 * might need to retry if it is very unlucky.
 * We will sometimes want to check for bad blocks in a bi_end_io function,
 * so we use the write_seqlock_irq variant.
 *
 * When looking for a bad block we specify a range and want to
 * know if any block in the range is bad.  So we binary-search
 * to the last range that starts at-or-before the given endpoint,
 * (or "before the sector after the target range")
 * then see if it ends after the given start.
 *
 * Return:
 *  0: there are no known bad blocks in the range
 *  1: there are known bad block which are all acknowledged
 * -1: there are bad blocks which have not yet been acknowledged in metadata.
 * plus the start/length of the first bad section we overlap.
 */
/*
 * [한국어] badblocks_check() - 공개 API. seqlock 읽기 측(락 없는 낙관적
 * 읽기) 안에서 _badblocks_check()를 호출하는 얇은 래퍼.
 * 동작: shift 정렬 후 read_seqbegin()으로 시퀀스 번호를 얻고,
 * _badblocks_check()로 실제 판정을 수행한 뒤 read_seqretry()로 그 사이
 * 쓰기측(_badblocks_set/_clear)이 테이블을 바꾸지 않았는지 확인한다.
 * 바뀌었으면(read_seqretry가 true) 전체를 처음부터 재시도한다 — 이것이
 * seqlock 읽기 측의 표준 패턴이며, 락을 아예 잡지 않으므로 이 함수는
 * 인터럽트/빠른 I/O 경로에서 호출해도 쓰기 측을 블록하지 않는다.
 * 호출자: block/genhd.c는 이 함수를 직접 쓰지 않지만(show/store만 감쌈),
 * drivers/block/null_blk의 null_handle_badblocks()가 I/O 처리 경로에서
 * 직접 호출한다. 호출 대상: _badblocks_check(), read_seqbegin/
 * read_seqretry.
 * 호출 체인: null_handle_badblocks() → [badblocks_check()] → _badblocks_check()
 */
int badblocks_check(struct badblocks *bb, sector_t s, sector_t sectors, /* seqlock 읽기 측 안에서 _badblocks_check()를 호출하는 공개 API 래퍼 */
			sector_t *first_bad, sector_t *bad_sectors)
{
	unsigned int seq; /* read_seqbegin()이 돌려주는 시퀀스 번호 - read_seqretry에 그대로 전달해 변경 여부 비교 */
	int rv; /* _badblocks_check()의 반환값을 담아 그대로 돌려줄 지역 변수 */

	WARN_ON(bb->shift < 0 || sectors == 0); /* 전제조건 검증 - 비활성화된 인스턴스나 길이 0 요청은 호출자 버그로 간주해 경고만 남기고 계속 진행(그대로 진행하면 아래에서 자연히 "불량 없음" 등으로 처리됨) */

	if (bb->shift > 0) { /* 정렬 단위가 설정돼 있으면(shift 0 초과 - shift<0은 위에서 이미 WARN_ON 대상) */
		/* round the start down, and the end up */
		sector_t target = s + sectors; /* 정렬 전 원래 끝 섹터 계산 */

		rounddown(s, 1 << bb->shift); /* 시작을 내림 - badblocks_set()과 동일한 방향(보수적으로 넓게) */
		roundup(target, 1 << bb->shift); /* 끝을 올림 - 마찬가지로 넓게 */
		sectors = target - s; /* 정렬된 범위로부터 길이 재계산 */
	}

retry: /* seqlock 읽기 측 재시도 진입점 */
	seq = read_seqbegin(&bb->lock); /* 현재 시퀀스 번호를 읽어옴 - 이 값이 바뀌지 않아야 아래 판정이 일관된 스냅샷에 대한 것임이 보장됨 */
	rv = _badblocks_check(bb, s, sectors, first_bad, bad_sectors); /* 락 없이 테이블을 조회 - 동시에 쓰기 측이 테이블을 바꾸고 있을 수도 있음을 전제 */
	if (read_seqretry(&bb->lock, seq)) /* 조회하는 동안 쓰기 측이 개입해 시퀀스가 바뀌었는지 확인 */
		goto retry; /* 바뀌었으면 방금 읽은 결과는 신뢰할 수 없으므로 처음부터 재시도 */

	return rv; /* 검증된(일관된 스냅샷에 대한) 판정 결과 반환 */
}
EXPORT_SYMBOL_GPL(badblocks_check);

/**
 * badblocks_set() - Add a range of bad blocks to the table.
 * @bb:		the badblocks structure that holds all badblock information
 * @s:		first sector to mark as bad
 * @sectors:	number of sectors to mark as bad
 * @acknowledged: weather to mark the bad sectors as acknowledged
 *
 * This might extend the table, or might contract it if two adjacent ranges
 * can be merged. We binary-search to find the 'insertion' point, then
 * decide how best to handle it.
 *
 * Return:
 *  true: success
 *  false: failed to set badblocks (out of space). Parital setting will be
 *  treated as failure.
 */
/*
 * [한국어] badblocks_set() - 공개 API. _badblocks_set()으로 그대로
 * 위임하는 얇은 래퍼(seqlock 쓰기 측 획득/해제는 _badblocks_set() 내부
 * 에서 수행됨). 호출자: drivers/block/null_blk의 configfs badblocks
 * 속성 store 핸들러(nullb_device_badblocks_store()), 그리고 이 파일의
 * badblocks_store()(사용자가 sysfs로 불량 구간을 수동 등록할 때).
 * 호출 체인: nullb_device_badblocks_store()/badblocks_store() → [badblocks_set()] → _badblocks_set()
 */
bool badblocks_set(struct badblocks *bb, sector_t s, sector_t sectors, /* 공개 API - _badblocks_set()으로 그대로 위임 */
		   int acknowledged)
{
	return _badblocks_set(bb, s, sectors, acknowledged); /* 실제 상태 기계(merge/combine/overwrite/insert)는 내부 엔진에 위임 */
}
EXPORT_SYMBOL_GPL(badblocks_set);

/**
 * badblocks_clear() - Remove a range of bad blocks to the table.
 * @bb:		the badblocks structure that holds all badblock information
 * @s:		first sector to mark as bad
 * @sectors:	number of sectors to mark as bad
 *
 * This may involve extending the table if we spilt a region,
 * but it must not fail.  So if the table becomes full, we just
 * drop the remove request.
 *
 * Return:
 *  true: success
 *  false: failed to clear badblocks
 */
/*
 * [한국어] badblocks_clear() - 공개 API. _badblocks_clear()로 그대로
 * 위임하는 얇은 래퍼. 호출자: drivers/block/null_blk의
 * null_handle_badblocks()(badblocks_once 옵션이 켜져 있을 때, 한 번
 * 실패시킨 불량 구간을 스스로 지워 다음 I/O부터는 정상 처리되게 함),
 * configfs store 핸들러, badblocks_store().
 * 호출 체인: null_handle_badblocks()/nullb_device_badblocks_store() → [badblocks_clear()] → _badblocks_clear()
 */
bool badblocks_clear(struct badblocks *bb, sector_t s, sector_t sectors) /* 공개 API - _badblocks_clear()로 그대로 위임 */
{
	return _badblocks_clear(bb, s, sectors); /* 실제 상태 기계(정상/머리·꼬리 겹침/중앙 분할)는 내부 엔진에 위임 */
}
EXPORT_SYMBOL_GPL(badblocks_clear);

/**
 * ack_all_badblocks() - Acknowledge all bad blocks in a list.
 * @bb:		the badblocks structure that holds all badblock information
 *
 * This only succeeds if ->changed is clear.  It is used by
 * in-kernel metadata updates
 */
/*
 * [한국어]
 * ack_all_badblocks() - 테이블의 모든 unacked 슬롯을 acked로 일괄
 * 전환하고, 그로 인해 새로 인접·ack일치하게 된 슬롯들을 최대한 압축한다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @return: 없음(void) - 실패해도 조용히 아무 일도 하지 않고 반환한다
 *          (커널 문서 그대로: "->changed가 clear일 때만 성공").
 *
 * "in-kernel metadata updates"(커널 문서 원문)가 이 함수를 쓰는 전형적인
 * 시나리오는, 호출자(예: MD)가 자신의 영속 메타데이터에 현재 unacked
 * 불량들을 방금 다 기록했고, 이제 badblocks 테이블 쪽의 unacked 표시도
 * 지워서 "이 정보는 이미 안전하게 저장됐다"를 반영하는 것이다. bb->page
 * 가 아직 할당되지 않았거나(초기화 전) bb->changed가 이미 1이면(다른
 * 경로가 동시에 테이블을 바꾸는 중이라 지금 acked로 바꿔도 changed가
 * 다시 세워질 것이므로) 시도할 가치가 없어 조기 반환한다. 그 외의
 * 경우엔 모든 unacked 슬롯을 acked로 다시 인코딩한 뒤, 배열 전체를
 * 순회하며 try_adjacent_combine()을 성공하는 동안 반복 호출해 이제
 * ack가 같아진 인접 슬롯들을 최대한 하나로 압축한다(같은 인덱스에서
 * while로 반복하는 이유: 병합 후에도 그 자리의 새 슬롯이 또 그 다음과
 * 합쳐질 수 있기 때문).
 * 실행 컨텍스트: seqlock 쓰기 측(write_seqlock_irq) 보유 중 실행되는
 * 테이블 전체 수정 함수.
 * 호출자: 이 트리에는 직접 호출하는 코드가 없음(EXPORT_SYMBOL_GPL로
 * 외부 모듈에 공개된 API - 범용 지식상 MD가 메타데이터 flush 후 호출).
 * 호출 대상: BB_* 매크로, try_adjacent_combine().
 * 에러 경로: 조건 불충족 시(bb->page==NULL 또는 bb->changed!=0) 아무
 * 것도 하지 않고 반환 - 별도의 실패 반환값은 없다(void).
 *
 * 호출 체인:
 *   (외부: MD 등 메타데이터 동기화 완료 후) → [ack_all_badblocks()] → try_adjacent_combine()
 */
void ack_all_badblocks(struct badblocks *bb) /* 모든 unacked 슬롯을 acked로 전환하고 인접 슬롯을 재압축 */
{
	if (bb->page == NULL || bb->changed) /* 아직 초기화되지 않았거나(page==NULL) 이미 다른 변경이 대기 중이면(changed!=0) */
		/* no point even trying */
		return; /* 시도할 가치가 없으므로 조용히 반환 */
	write_seqlock_irq(&bb->lock); /* seqlock 쓰기 측 획득(인터럽트 비활성화) */

	if (bb->changed == 0 && bb->unacked_exist) { /* 락을 잡은 뒤 다시 확인 - changed가 여전히 0이고 unacked가 있을 가능성이 있으면(이중 검사) */
		u64 *p = bb->page; /* bad table 베이스 포인터 */
		int i; /* 순회 인덱스 */

		for (i = 0; i < bb->count ; i++) { /* 테이블에 채워진 슬롯 전체를 순회 */
			if (!BB_ACK(p[i])) { /* unacked 슬롯을 발견하면 */
				sector_t start = BB_OFFSET(p[i]); /* 재인코딩에 필요한 - 그 슬롯의 시작 섹터 보존 */
				int len = BB_LEN(p[i]); /* 재인코딩에 필요한 - 그 슬롯의 길이 보존 */

				p[i] = BB_MAKE(start, len, 1); /* 시작/길이는 그대로, ack만 1(acked)로 바꿔 재인코딩 */
			}
		}

		for (i = 0; i < bb->count ; i++) /* 방금 ack를 통일시킨 슬롯들 중 인접한 것들을 압축 */
			while (try_adjacent_combine(bb, i)) /* i와 i+1이 병합 가능한 동안 반복 - 병합 후 같은 자리에서 또 다음과 합쳐질 수 있으므로 while */
				; /* 병합 자체는 try_adjacent_combine() 내부에서 수행되고 여기선 반복 조건만 확인 */

		bb->unacked_exist = 0; /* 모든 슬롯을 acked로 바꿨으므로 이제 unacked_exist 힌트를 확실히 0으로 */
	}
	write_sequnlock_irq(&bb->lock); /* seqlock 쓰기 측 해제 */
}
EXPORT_SYMBOL_GPL(ack_all_badblocks);

/**
 * badblocks_show() - sysfs access to bad-blocks list
 * @bb:		the badblocks structure that holds all badblock information
 * @page:	buffer received from sysfs
 * @unack:	weather to show unacknowledged badblocks
 *
 * Return:
 *  Length of returned data
 */
/*
 * [한국어]
 * badblocks_show() - /sys/block/<disk>/badblocks(genhd.c 경유) 또는
 * null_blk configfs의 badblocks 속성 읽기 핸들러가 호출하는 실제 구현.
 * 테이블 내용을 "시작섹터 길이\n" 형식의 텍스트로 page 버퍼에 채운다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @page: 커널이 넘겨준 출력 버퍼(최대 PAGE_SIZE) - sysfs read 관례상
 *        한 페이지 크기로 고정되어 있음.
 * @unack: 0이면 모든 슬롯 출력, 0이 아니면 unacked 슬롯만 출력.
 * @return: 실제로 page에 쓴 바이트 수.
 *
 * seqlock 읽기 측(read_seqbegin/read_seqretry) 안에서 테이블을 순회하며
 * (unack 모드면 acked 슬롯은 건너뛰고) snprintf로 각 슬롯을 한 줄씩
 * 기록한다. shift가 적용된 인스턴스라면 내부 슬롯 값(섹터/슬롯 단위)에
 * 다시 shift를 곱해(<< bb->shift) 사용자에게는 실제 섹터 단위로 보여준다.
 * PAGE_SIZE를 넘어서면 그 지점에서 순회를 멈춘다(sysfs 페이지 크기
 * 제약). unack 모드로 조회했는데 출력이 하나도 없었다면, 그 사실 자체가
 * "실제로 unacked 슬롯이 없다"는 확인이므로 이 기회에 unacked_exist
 * 힌트를 0으로 내린다(badblocks_update_acked()의 스캔과 같은 효과를
 * 부수적으로 얻는 셈).
 * 실행 컨텍스트: sysfs read(사용자 컨텍스트) 또는 configfs read에서
 * 호출되는, seqlock 읽기 측을 쓰는 조회 함수.
 * 호출자: block/genhd.c의 disk_badblocks_show(), drivers/block/null_blk
 * 의 nullb_device_badblocks_show().
 * 호출 대상: read_seqbegin/read_seqretry, BB_* 매크로, snprintf().
 * 에러 경로: shift<0(비활성화)이면 즉시 0을 반환해 빈 출력을 만든다.
 *
 * 호출 체인:
 *   disk_badblocks_show()/nullb_device_badblocks_show() → [badblocks_show()]
 */
ssize_t badblocks_show(struct badblocks *bb, char *page, int unack) /* sysfs/configfs 읽기 핸들러 - 테이블 내용을 텍스트로 직렬화 */
{
	size_t len; /* 지금까지 page에 쓴 바이트 수(출력 오프셋 겸 반환값) */
	int i; /* 테이블 순회 인덱스 */
	u64 *p = bb->page; /* bad table 베이스 포인터 */
	unsigned seq; /* read_seqbegin()이 돌려주는 시퀀스 번호 */

	if (bb->shift < 0) /* 이 인스턴스가 비활성화 상태이면 */
		return 0; /* 출력할 것이 없으므로 빈 문자열(길이 0)로 처리 */

retry: /* seqlock 읽기 측 재시도 진입점 */
	seq = read_seqbegin(&bb->lock); /* 조회 시작 시점의 시퀀스 번호 확보 */

	len = 0; /* 출력 오프셋을 매 시도마다 처음부터 다시 시작 */
	i = 0; /* 순회 인덱스도 매 시도마다 리셋 */

	while (len < PAGE_SIZE && i < bb->count) { /* 출력 버퍼가 아직 여유 있고 순회할 슬롯이 남아있는 동안 */
		sector_t s = BB_OFFSET(p[i]); /* 이 슬롯의 시작 섹터(내부 슬롯 단위, shift 적용 전) */
		unsigned int length = BB_LEN(p[i]); /* 이 슬롯의 길이(섹터, shift 적용 전) */
		int ack = BB_ACK(p[i]); /* 이 슬롯의 ack 상태 */

		i++; /* 다음 슬롯으로 - continue/정상 진행 모두에 앞서 미리 전진시켜 무한루프 방지 */

		if (unack && ack) /* unack 전용 조회 모드인데 이 슬롯은 이미 acked이면 */
			continue; /* 이 슬롯은 건너뛰고 다음 슬롯 검사로 */

		len += snprintf(page+len, PAGE_SIZE-len, "%llu %u\n", /* 남은 버퍼 공간에 "시작 길이\n" 한 줄을 기록하고 그 길이만큼 오프셋 전진 */
				(unsigned long long)s << bb->shift, /* 내부 슬롯 단위 시작 섹터를 shift만큼 다시 곱해 실제 섹터 단위로 환산해 출력 */
				length << bb->shift); /* 길이도 동일하게 실제 섹터 단위로 환산해 출력 */
	}
	if (unack && len == 0) /* unack 전용 모드로 조회했는데 출력이 하나도 없었으면 */
		bb->unacked_exist = 0; /* 실제로 unacked 슬롯이 없음을 이 조회를 통해 확인했으므로 힌트를 확실히 0으로 내림 */

	if (read_seqretry(&bb->lock, seq)) /* 조회하는 동안 테이블이 바뀌었는지 확인 */
		goto retry; /* 바뀌었으면 지금까지 채운 내용은 신뢰할 수 없으므로 처음부터 재시도 */

	return len; /* 검증된 스냅샷에 대해 실제로 쓴 바이트 수 반환 */
}
EXPORT_SYMBOL_GPL(badblocks_show);

/**
 * badblocks_store() - sysfs access to bad-blocks list
 * @bb:		the badblocks structure that holds all badblock information
 * @page:	buffer received from sysfs
 * @len:	length of data received from sysfs
 * @unack:	weather to show unacknowledged badblocks
 *
 * Return:
 *  Length of the buffer processed or -ve error.
 */
/*
 * [한국어]
 * badblocks_store() - /sys/block/<disk>/badblocks(genhd.c 경유) 또는
 * null_blk configfs badblocks 속성 쓰기 핸들러가 호출하는 실제 구현.
 * 사용자가 "시작섹터 길이\n" 텍스트를 쓰면 파싱해 badblocks_set()으로
 * 등록한다.
 *
 * @bb: 대상 badblocks 인스턴스.
 * @page: 사용자 공간이 write()한 원본 텍스트 버퍼.
 * @len: page의 바이트 길이(성공 시 그대로 반환해 "len바이트 모두
 *       처리했다"는 sysfs/configfs 관례를 지킴).
 * @unack: 0이 아니면 등록할 범위를 unacked로, 0이면 acked로 표시
 *         (badblocks_set()의 acknowledged 인자는 !unack로 반전해서 전달).
 * @return: 성공 시 len 그대로, 실패 시 음수 errno(-EINVAL 파싱 오류,
 *          -ENOSPC 테이블 공간 부족).
 *
 * sscanf로 "시작 길이[개행]" 형식을 파싱한다 — 세 필드가 모두 파싱되면
 * 마지막 문자가 정확히 개행이어야 하고(그렇지 않으면 형식 오류),
 * 길이는 항상 양수여야 한다. 파싱에 성공하면 badblocks_set()을 호출해
 * 실제로 테이블에 등록하고, 공간 부족으로 실패하면 -ENOSPC를 반환한다.
 * 실행 컨텍스트: sysfs/configfs write(사용자 컨텍스트)에서 호출.
 * 호출자: block/genhd.c의 disk_badblocks_store(), drivers/block/null_blk
 * 의 nullb_device_badblocks_store().
 * 호출 대상: sscanf(), badblocks_set().
 * 에러 경로: 파싱 실패/개행 누락/길이 0 이하는 -EINVAL, badblocks_set()
 * 실패(테이블 공간 부족)는 -ENOSPC로 각각 사용자에게 write() 실패로
 * 보고된다.
 *
 * 호출 체인:
 *   disk_badblocks_store()/nullb_device_badblocks_store() → [badblocks_store()] → badblocks_set()
 */
ssize_t badblocks_store(struct badblocks *bb, const char *page, size_t len, /* sysfs/configfs 쓰기 핸들러 - 텍스트를 파싱해 badblocks_set() 호출 */
			int unack)
{
	unsigned long long sector; /* 파싱된 시작 섹터 */
	int length; /* 파싱된 길이 */
	char newline; /* 세 번째 필드로 파싱을 시도할 개행 문자(형식 검증용) */

	switch (sscanf(page, "%llu %d%c", &sector, &length, &newline)) { /* "시작 길이[개행]" 형식을 파싱 - 반환값은 실제로 파싱에 성공한 필드 개수 */
	case 3: /* 세 필드(시작, 길이, 개행 문자)까지 모두 파싱됨 */
		if (newline != '\n') /* 세 번째 문자가 개행이 아니면(예: 추가 쓰레기 문자) */
			return -EINVAL; /* 형식 오류로 거부 */
		fallthrough; /* 개행 검증까지 통과했으면 아래 길이 검증도 마저 수행 */
	case 2: /* 시작과 길이 두 필드만 파싱됨(개행 없이 끝난 입력도 허용) */
		if (length <= 0) /* 길이가 0 이하이면 */
			return -EINVAL; /* 의미 없는 범위이므로 거부 */
		break;
	default: /* 0개 또는 1개 필드만 파싱됨 - 형식 자체가 잘못됨 */
		return -EINVAL; /* 파싱 실패로 거부 */
	}

	if (!badblocks_set(bb, sector, length, !unack)) /* 파싱된 범위를 실제로 등록 시도(unack 인자를 반전해 acknowledged로 전달) */
		return -ENOSPC; /* 등록 실패(테이블 공간 부족)를 sysfs 쓰기 실패로 알림 */

	return len; /* 관례대로 입력 버퍼 전체를 처리했다는 의미로 len 그대로 반환 */
}
EXPORT_SYMBOL_GPL(badblocks_store);

/*
 * [한국어]
 * __badblocks_init() - badblocks_init()/devm_init_badblocks() 공통의
 * 실제 초기화 로직. bad table 메모리를 할당하고 seqlock을 초기화한다.
 *
 * @dev: device-managed 할당을 쓸 소유 device, 또는 일반 kzalloc을 쓸
 *       경우 NULL.
 * @bb: 초기화할 badblocks 인스턴스(호출자가 소유한 메모리 - 이 함수는
 *      그 내용만 채움).
 * @enable: 0이 아니면 활성화(shift=0)로, 0이면 비활성화(shift=-1)로
 *          시작.
 * @return: 0(성공), -ENOMEM(bad table용 페이지 할당 실패).
 *
 * dev 유무에 따라 devm_kzalloc(device 생명주기에 묶임, 이 경우 bb->dev도
 * 기록해 badblocks_exit()이 devm_kfree를 쓰도록 함) 또는 kzalloc(일반
 * 커널 메모리)로 PAGE_SIZE 크기의 bad table을 0으로 채워 할당한다.
 * 할당에 실패하면 이 인스턴스를 즉시 비활성화(shift=-1)시켜, 이후
 * 어떤 badblocks_* API를 호출해도 shift<0 조기 반환 경로로 안전하게
 * 빠지도록 만든다. 성공하면 seqlock_init()으로 동시성 메커니즘을
 * 준비한다.
 * 실행 컨텍스트: 디바이스/구조체 생성 경로에서 1회 호출되는 초기화
 * 함수 - 아직 다른 스레드가 이 bb를 참조하지 않는다고 가정.
 * 호출자: badblocks_init()(dev=NULL 고정), devm_init_badblocks()
 * (enable=1 고정).
 * 호출 대상: devm_kzalloc()/kzalloc(), seqlock_init().
 * 에러 경로: 메모리 할당 실패 시 -ENOMEM 반환 및 shift=-1로 안전한
 * "비활성화" 상태를 남김(호출자가 반환값을 무시해도 이후 API가
 * 안전하게 아무 일도 안 하도록 방어).
 *
 * 호출 체인:
 *   badblocks_init()/devm_init_badblocks() → [__badblocks_init()]
 */
static int __badblocks_init(struct device *dev, struct badblocks *bb, /* badblocks_init()/devm_init_badblocks() 공통 초기화 - 메모리 할당 + seqlock 준비 */
		int enable)
{
	bb->dev = dev; /* 소유 device 기록(NULL이면 비-devm 경로) - badblocks_exit()이 해제 방식을 결정하는 데 사용 */
	bb->count = 0; /* 갓 초기화된 상태이므로 채워진 슬롯 없음 */
	if (enable) /* 활성화 요청이면 */
		bb->shift = 0; /* shift 0 = 섹터 단위 그대로 추적(정렬 없음) - 활성 상태 */
	else
		bb->shift = -1; /* 비활성화 상태로 시작 - 이후 모든 badblocks_* API가 조기 반환 */
	if (dev) /* 소유 device가 있으면 */
		bb->page = devm_kzalloc(dev, PAGE_SIZE, GFP_KERNEL); /* device 생명주기에 자동 연동되는 0-초기화 메모리 할당 */
	else /* 소유 device가 없으면(범용 badblocks_init() 경로) */
		bb->page = kzalloc(PAGE_SIZE, GFP_KERNEL); /* 일반 커널 힙에서 0-초기화 메모리 할당 - 해제는 badblocks_exit()이 kfree로 직접 수행 */
	if (!bb->page) { /* 두 할당 경로 중 어느 쪽이든 실패(메모리 부족)하면 */
		bb->shift = -1; /* 페이지가 없는데 활성 상태로 두면 이후 접근이 NULL 역참조로 이어지므로 즉시 비활성화 */
		return -ENOMEM; /* 호출자에게 메모리 부족을 알림 */
	}
	seqlock_init(&bb->lock); /* 할당이 성공한 뒤에야 seqlock 초기화 - 이 시점부터 이 인스턴스가 동시 접근에 안전해짐 */

	return 0; /* 초기화 성공 */
}

/**
 * badblocks_init() - initialize the badblocks structure
 * @bb:		the badblocks structure that holds all badblock information
 * @enable:	weather to enable badblocks accounting
 *
 * Return:
 *  0: success
 *  -ve errno: on error
 */
/*
 * [한국어] badblocks_init() - 공개 API. device와 무관한(비-devm) 일반
 * badblocks 인스턴스를 초기화한다. __badblocks_init(NULL, ...)으로
 * 그대로 위임 - dev를 NULL로 고정하므로 메모리는 kzalloc으로 할당되고
 * badblocks_exit()도 kfree로 해제하게 된다.
 * 호출 체인: (범용 지식: MD 등이 자체 device 생명주기를 따로 관리할 때)
 * → [badblocks_init()] → __badblocks_init()
 */
int badblocks_init(struct badblocks *bb, int enable) /* 공개 API - dev=NULL로 고정한 비-devm 초기화 */
{
	return __badblocks_init(NULL, bb, enable); /* 소유 device 없이(비-devm) 활성화 여부만 enable로 전달 */
}
EXPORT_SYMBOL_GPL(badblocks_init);

/*
 * [한국어]
 * devm_init_badblocks() - device-managed 방식으로 badblocks 인스턴스를
 * 초기화한다(dev의 생명주기에 자동으로 묶임).
 *
 * @dev: 이 badblocks 인스턴스를 소유할 device. devm_kfree가 dev 해제
 *       시점에 자동으로 bb->page를 해제하도록 device-managed 리소스로
 *       등록된다.
 * @bb: 초기화할 badblocks 인스턴스.
 * @return: 0(성공), -EINVAL(bb가 NULL), 또는 __badblocks_init()이 돌려준
 *          -ENOMEM.
 *
 * badblocks_init()과 달리 dev를 반드시 넘기고 enable을 항상 1(활성화)로
 * 고정한다 — device-managed 사용처는 대개 "생성 시점부터 badblocks를
 * 쓰겠다"는 의도이기 때문. bb가 NULL이면 __badblocks_init()을 부르기도
 * 전에 -EINVAL로 조기에 실패시켜 그 안에서의 NULL 역참조를 막는다.
 * 실행 컨텍스트: 디바이스 프로브(probe) 경로에서 1회 호출.
 * 호출자: drivers/block/null_blk의 디바이스 생성 경로
 * (badblocks_init(&dev->badblocks, 0) 형태로 이 트리의 null_blk는 실제로
 * devm이 아닌 badblocks_init()을 직접 쓰지만, 이 함수는 device-managed
 * 해제를 원하는 다른 서브시스템을 위해 공개돼 있다).
 * 호출 대상: __badblocks_init().
 * 에러 경로: bb==NULL이면 -EINVAL, 메모리 할당 실패면 __badblocks_init()
 * 이 반환한 -ENOMEM을 그대로 전달.
 *
 * 호출 체인:
 *   (디바이스 프로브 경로) → [devm_init_badblocks()] → __badblocks_init()
 */
int devm_init_badblocks(struct device *dev, struct badblocks *bb) /* 공개 API - device-managed(자동 해제) 초기화, enable=1 고정 */
{
	if (!bb) /* 널 포인터 방어 */
		return -EINVAL; /* 잘못된 인자로 조기 거부 */
	return __badblocks_init(dev, bb, 1); /* dev를 소유자로 등록하고 항상 활성화 상태로 초기화 */
}
EXPORT_SYMBOL_GPL(devm_init_badblocks);

/**
 * badblocks_exit() - free the badblocks structure
 * @bb:		the badblocks structure that holds all badblock information
 */
/*
 * [한국어]
 * badblocks_exit() - badblocks 인스턴스가 소유한 bad table 메모리를
 * 해제한다(devm_exit_badblocks()의 인라인 래퍼를 통해서도 호출됨,
 * badblocks.h 참고).
 *
 * @bb: 해제할 badblocks 인스턴스(NULL이면 아무 것도 안 함).
 * @return: 없음(void).
 *
 * __badblocks_init()에서 dev 유무로 갈렸던 할당 방식(devm_kzalloc vs
 * kzalloc)에 대칭되게, dev가 설정돼 있으면 devm_kfree로, 아니면
 * kfree로 해제한다. 해제 후 page를 NULL로 만들어 두어, 혹시라도 이
 * 인스턴스가 재사용되거나 다시 접근되었을 때 댕글링 포인터
 * 역참조 대신 명확한 NULL 역참조(더 안전하게 조기 발견 가능)로
 * 이어지게 한다.
 * 실행 컨텍스트: 디바이스/구조체 해제 경로에서 1회 호출 - 이 시점
 * 이후로는 이 bb에 대한 다른 badblocks_* 호출이 없다고 가정(그렇지
 * 않으면 page==NULL이므로 대부분 안전하게 실패하긴 하지만 정의된
 * 동작은 아님).
 * 호출자: drivers/block/null_blk의 디바이스 해제 경로
 * (badblocks_exit(&dev->badblocks)), badblocks.h의 인라인 함수
 * devm_exit_badblocks()(dev 소유권 일치를 확인한 뒤 이 함수를 호출).
 * 호출 대상: devm_kfree()/kfree().
 * 에러 경로: bb==NULL이면 아무 것도 하지 않고 조용히 반환 - 이중 해제
 * 방어의 일부.
 *
 * 호출 체인:
 *   (디바이스 해제 경로)/devm_exit_badblocks() → [badblocks_exit()]
 */
void badblocks_exit(struct badblocks *bb) /* badblocks 인스턴스의 bad table 메모리 해제 */
{
	if (!bb) /* 널 포인터 방어(이중 해제 등에도 안전) */
		return; /* 아무 것도 하지 않고 반환 */
	if (bb->dev) /* device-managed 방식으로 할당됐으면(__badblocks_init에서 dev가 기록됨) */
		devm_kfree(bb->dev, bb->page); /* devm_kzalloc과 짝을 이루는 devm_kfree로 해제 */
	else /* 일반 kzalloc으로 할당됐으면 */
		kfree(bb->page); /* 일반 kfree로 해제 */
	bb->page = NULL; /* 해제 후 포인터를 무효화 - 댕글링 포인터를 통한 오사용을 조기에 NULL 역참조로 드러냄 */
}
EXPORT_SYMBOL_GPL(badblocks_exit);

/*
 * [한국어]
 * ============================================================================
 * 요약: 이 파일이 실제로 연결되는 곳
 * ----------------------------------------------------------------------------
 * - 이 파일은 특정 하드웨어에 종속되지 않는 block layer 범용 불량 섹터
 *   추적 엔진이다. 파일 상단 4-섹션 요약에서 설명한 대로, 이 트리에서
 *   실제로 확인되는 연결점은 block/genhd.c(gendisk->bb를 통한 범용 sysfs
 *   /sys/block/<disk>/badblocks)와 drivers/block/null_blk(fault-injection
 *   테스트 드라이버)이다. drivers/nvme/host/ 트리는 badblocks_* 심볼을
 *   전혀 참조하지 않는다(grep으로 확인) - 이 파일과 NVMe SQ/CQ, doorbell,
 *   CID, PRP/SGL 사이에는 직접적인 연결이 없다.
 * - 핵심 동시성 설계는 seqlock이다: badblocks_check()/badblocks_show()
 *   같은 "자주 호출되는 조회"는 락을 걸지 않는 read_seqbegin/
 *   read_seqretry 낙관적 읽기를 쓰고, badblocks_set()/_clear()/
 *   ack_all_badblocks() 같은 "드물게 호출되는 갱신"만 write_seqlock류로
 *   실제 배타적 락을 건다.
 * - badblocks_set()은 병합(merge)/결합(combine)/덮어쓰기(overwrite)/
 *   분할(split)/삽입(insert)을 상황별로 처리하는 상태 기계이고,
 *   badblocks_clear()는 그보다 단순한 정상/머리·꼬리 겹침/중앙 분할 세
 *   갈래 분류이며, badblocks_check()는 순수 조회로 겹침 여부와 ack
 *   상태를 집계해 0/1/-1을 판정한다 - 이 세 엔진 모두 prev_badblocks()
 *   (필요시 이진 탐색)로 찾은 기준 슬롯을 중심으로 앞/뒤 겹침을 분류하는
 *   공통 뼈대를 공유한다.
 * - (범용 지식) 실제 리눅스에서 이 엔진의 대표적 소비자는 MD RAID의
 *   struct md_rdev->badblocks이며, 멤버 디스크별 불량 섹터를 추적해
 *   resync/rebuild 시 우회 대상으로 삼는다.
 * ============================================================================
 */
