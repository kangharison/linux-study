// SPDX-License-Identifier: GPL-2.0
/*
 * Bad block management
 *
 * - Heavily based on MD badblocks code from Neil Brown
 *
 * Copyright (c) 2015, Intel Corporation.
 */

/*
 * ============================================================================
 * NVMe SSD 관점 파일 요약:
 * ----------------------------------------------------------------------------
 * 이 파일은 리눅스 블록 계층(block layer)의 범용 불량 블록(bad block) 관리
 * 엔진이다. NVMe SSD에서는 NAND 플래시의 결함, wear-out, ECC 오류 등으로
 * 발생한 결함 영역을 namespace의 논리 블록 주소(LBA) 단위로 추적한다.
 * 상위 I/O 경로는 submit_bio() -> blk_mq_submit_bio() -> blk_mq_get_request
 * -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 순으로 흐를 수 있으며,
 * badblocks_check()를 통해 해당 LBA 범위가 NVMe 미디어 결함 영역과
 * 겹치는지를 사전에 확인할 수 있다.
 * 이 파일은 blk-mq, bio, request 등 block layer 핵심 파일 위에서 동작하고,
 * NVMe 하드웨어의 SQ/CQ, CID, PRP/SGL 등과는 직접 연결되지 않으며 LBA
 * 레벨의 미디어 정보만을 다룬다.
 * ============================================================================
 */

#include <linux/badblocks.h>
#include <linux/seqlock.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/slab.h>

/*
 * struct badblocks (include/linux/badblocks.h 참고) 필드별 NVMe 연결:
 * - page:    u64 배열로 저장된 불량 LBA 범위 항목들. NVMe namespace의
 *            특정 LBA 구간이 미디어 결함으로 매핑된 상태를 표현한다.
 * - count:   현재 bad table에 저장된 항목 수. 즉, namespace 내에서
 *            추적 중인 불량 영역의 개수이다.
 * - shift:   LBA/섹터 정렬 단위. NVMe format에서 정의한 논리 블록 크기
 *            및 단위 정렬과 연관된다.
 * - lock:    seqlock. I/O 완료 경로(badblocks_check)와 sysfs/MD 메타데이터
 *            갱신이 동시에 접근하는 것을 보호한다.
 * - dev:     이 badblocks 구조체를 소유하는 device. NVMe namespace의
 *            device 구조체가 연결된다.
 * - changed: 메타데이터 동기화가 필요함을 알리는 플래그. NVMe 환경에서는
 *            상위 RAID/MD 레이어가 변경사항을 persistent metadata에
 *            기록해야 할 때 사용된다 (추정).
 * - unacked_exist: 아직 메타데이터에 반영되지 않은(unack) 불량 블록이
 *                  존재함을 나타낸다. NVMe 미디어 오류가 발생했으나
 *                  상위 계층이 이를 인지하지 못한 상태로 볼 수 있다.
 *
 * struct badblocks_context (함수 내부 루프에서 사용):
 * - start: 현재 처리 중인 LBA. NVMe namespace LBA 공간의 오프셋.
 * - len:   현재 처리 중인 연속 섹터 수.
 * - ack:   acknowledged 플래그. NVMe에서 발생한 미디어 오류가 상위
 *          레이어(예: MD)의 메타데이터에 반영되었는지 여부.
 *
 * BB_* 매크로 (include/linux/badblocks.h 참고)의 NVMe 의미:
 * - BB_OFFSET(p[i]): i번째 항목의 시작 LBA. NVMe namespace 내 논리 주소.
 * - BB_LEN(p[i]):    i번째 항목의 길이(섹터 수).
 * - BB_END(p[i]):    i번째 항목의 끝 LBA (시작 + 길이).
 * - BB_ACK(p[i]):    메타데이터 반영(ack) 여부. 0=unack, 1=ack.
 * - BB_MAX_LEN:      단일 항목이 표현할 수 있는 최대 섹터 수.
 * - BB_MAKE(start, len, ack): 새로운 NVMe 불량 항목을 u64로 인코딩.
 */

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
 * prev_by_hint(): hint 인덱스부터 시작하여 bad table에서 's' LBA보다 작거나
 * 같은 위치에서 시작하는 마지막 불량 범위를 선형 검색한다.
 * 호출 경로: badblocks_set -> _badblocks_set -> prev_badblocks -> prev_by_hint.
 * NVMe에서는 I/O 경로(badblocks_check)에서 LBA가 namespace 전체를 커버할
 * 수 있으므로, 이전 루프에서 계산된 hint를 재활용해 이진 탐색 비용을
 * 피하는 것이 중요하다.
 */
static int prev_by_hint(struct badblocks *bb, sector_t s, int hint) /* NVMe namespace LBA s 주변의 직전 불량 항목을 힌트부터 선형 탐색; blk-mq I/O latency 경감 목적 */
{
	int hint_end = hint + 2; /* 탐색 윈도우: 직전 루프 힌트로부터 2슬롯만 확인, NVMe SQ/CQ 선택 직전의 빠른 LBA 조회에 사용 */
	u64 *p = bb->page; /* bad table 페이지: NVMe namespace의 불량 LBA 범위가 u64 단위로 인코딩됨 */
	int ret = -1; /* 기본값 -1은 s 이전에 시작하는 NVMe 불량 항목이 없음을 의미 */

	while ((hint < hint_end) && ((hint + 1) <= bb->count) &&
	       (BB_OFFSET(p[hint]) <= s)) { /* hint 범위 내에서 s 이상 시작하는 항목 탐색 */ /* NVMe LBA 오름차순으로 정렬된 bad table이므로 연속 슬롯만 확인필도 안전 */
		if ((hint + 1) == bb->count || BB_OFFSET(p[hint + 1]) > s) { /* 다음 항목이 s보다 뒤에서 시작하거나 테이블 끝이면 현재 hint가 기준 항목 */
			ret = hint; /* s보다 작거나 같은 시작 LBA를 가진 마지막 항목 */ /* 이 인덱스는 이후 _badblocks_set/_check에서 다음 루프 힌트로 재사용되어 NVMe I/O 경로의 이진 탐색을 회피 */
			break; /* 선형 힌트 히트: NVMe SQ/CQ doorbell latency에 민감한 경로의 탐색 비용 절감 */
		}
		hint++; /* 다음 bad table 슬롯으로 이동; NVMe LBA는 오름차순 정렬됨 */
	}

	return ret; /* 탐색된 직전 NVMe 불량 항목 인덱스 반환, -1이면 s 앞에 불량 없음 */
}

/*
 * prev_badblocks(): bad table에서 bad->start보다 작거나 같은 시작 LBA를
 * 가진 항목을 찾는다. hint가 주어지면 먼저 선형 탐색을 시도하고, 실패하면
 * 이진 탐색을 수행한다.
 * 호출 경로: _badblocks_set / _badblocks_clear / _badblocks_check.
 * NVMe 관점: namespace 전체 LBA 범위에서 빠르게 결함 영역을 찾아 I/O
 * 지연(latency)을 줄이는 핵심 경로이다.
 */
static int prev_badblocks(struct badblocks *bb, struct badblocks_context *bad, /* NVMe namespace LBA s보다 작거나 같은 시작 LBA를 가진 마지막 불량 항목 탐색; hint miss 시 이진 탐색 */
			  int hint)
{
	sector_t s = bad->start; /* 현재 처리 중인 NVMe I/O/갱신 대상의 시작 LBA */
	int ret = -1; /* 기본 반환값: s 이전 NVMe 불량 항목 없음 */
	int lo, hi; /* 이진 탐색 범위: NVMe namespace 전체 LBA 공간을 슬롯 인덱스로 색인 */
	u64 *p; /* bad table 포인터: NVMe 불량 항목 배열 */

	if (!bb->count) /* bad table이 비어 있으면 NVMe namespace에 등록된 불량 없음 */
		goto out; /* 빈 테이블이면 out으로 직진, 이후 NVMe I/O는 정상 경로로 진행 (추정) */

	if (hint >= 0) { /* 이전 루프가 남긴 hint가 있으면 선형 탐색 먼저 시도; blk-mq dispatch batch처럼 지역성을 활용 (추정) */
		ret = prev_by_hint(bb, s, hint); /* O(1) 윈도우 탐색: NVMe SQ/CQ doorbell latency에 영향을 주지 않도록 빠르게 */
		if (ret >= 0) /* 힌트에서 기준 항목 발견 */
			goto out; /* 이진 탐색 생략: NVMe I/O 경로에서 CPU 사이클 절약 */
	}

	lo = 0; /* 이진 탐색 하한 */
	hi = bb->count; /* 상한(미포함): bad table 슬롯 수, NVMe bad 항목 개수 */
	p = bb->page; /* 테이블 베이스 */

	/* The following bisect search might be unnecessary */
	if (BB_OFFSET(p[lo]) > s) /* 최소 LBA 항목이 s보다 뒤면 s 이전 불량 없음 */
		return -1; /* 모든 NVMe 불량 항목이 s보다 뒤에 있음 */ /* submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq 이전 badblocks_check에서 정상 LBA로 판단 가능 (추정) */
	if (BB_OFFSET(p[hi - 1]) <= s) /* 마지막 항목이 s에서 시작하거나 이전이면 전체가 기준 */
		return hi - 1; /* 마지막 불량 항목이 s 이전에 시작 */ /* NVMe I/O 시작 LBA가 마지막 불량 범위 이전에 있음 */

	/* Do bisect search in bad table */
	while (hi - lo > 1) { /* 이진 탐색 루프: O(log n), NVMe namespace 크기와 무관 */
		int mid = (lo + hi)/2; /* 중간 인덱스 */
		sector_t a = BB_OFFSET(p[mid]); /* mid 위치의 NVMe 불량 시작 LBA */ /* 이진 탐색은 LBA 범위를 반씩 줄여 NVMe SQ/CQ 선택 직전의 LBA 조회 지연을 낮춤 */

		if (a == s) { /* 정확히 s에서 시작하는 NVMe 불량 항목 발견 */
			ret = mid; /* 정확히 s에서 시작하는 항목 발견 */ /* CID/tag 할당 전 LBA 상태를 정확히 식별 */
			goto out; /* 정확히 일치하면 탐색 종료 */
		}

		if (a < s) /* mid 항목이 s보다 앞에 있음 */
			lo = mid; /* s보다 작으면 우측 반으로 좁힘 */ /* 우측(더 큰 LBA) 반으로 탐색 범위 축소 */
		else /* mid 항목이 s보다 뒤에 있음 */
			hi = mid; /* s보다 크면 좌측 반으로 좁힘 */ /* 좌측(더 작은 LBA) 반으로 탐색 범위 축소 */
	}

	if (BB_OFFSET(p[lo]) <= s) /* lo 항목이 s에서 시작하거나 이전이면 반환 */
		ret = lo; /* s보다 작거나 같은 시작 LBA를 가진 항목 */ /* NVMe I/O 시작 LBA 직전/현재의 불량 범위 인덱스 확정 */
out:
	return ret; /* 반환값은 이후 front/overlap 판단의 기준 */
}

/*
 * can_merge_front(): bad table의 prev 인덱스 항목과 bad가 가리키는 범위가
 * 앞쪽(front)으로 병합 가능한지 판단한다. ack 값이 같고 LBA가 연속 또는
 * 중첩되면 병합된다.
 * NVMe: 연속된 NAND 불량 블록을 하나의 큰 범위로 합쳐 bad table 공간을
 * 절약하고, 이후 badblocks_check() 탐색을 효율화한다.
 */
static bool can_merge_front(struct badblocks *bb, int prev, /* prev 항목과 bad 범위가 앞쪽으로 병합 가능한지 판단; ack 일치 및 LBA 연속 필요 */
			    struct badblocks_context *bad)
{
	sector_t s = bad->start; /* NVMe I/O/갱신 대상의 시작 LBA */
	u64 *p = bb->page; /* bad table */

	if (BB_ACK(p[prev]) == bad->ack && /* ack 상태가 같아야 병합 가능 */ /* ack 상태가 같아야 병합 가능; NVMe 상위 메타데이터 반영 상태 일치 의미 */
	    (s < BB_END(p[prev]) || /* 중첩된 경우 */ /* s가 prev 범위 낭부이면 NVMe I/O 범위가 불량 영역과 중첩 */
	     (s == BB_END(p[prev]) && (BB_LEN(p[prev]) < BB_MAX_LEN)))) /* 인접하고 최대 길이 미만 */ /* 인접하며 단일 항목 최대 길이 미만일 때만 병합: NVMe request segment 병합의 논리적 대응 (추정) */
		return true; /* front 병합 가능: 연속된 NVMe 불량 LBA를 하나의 큰 범위로 통합 */
	return false; /* 병합 불가: 별도 bad table 슬롯 필요 */
}

/*
 * front_merge(): bad table의 prev 항목 앞으로 bad 범위를 병합한다.
 * NVMe: bad->len만큼의 섹터를 prev 범위에 흡수시키며, NVMe namespace
 * 내에서 연속된 결함을 하나의 항목으로 통합한다.
 */
static int front_merge(struct badblocks *bb, int prev, struct badblocks_context *bad) /* prev 항목 앞으로 bad 범위를 병합; NVMe 연속 불량 LBA 통합 */
{
	sector_t sectors = bad->len; /* 병합할 NVMe LBA 길이(섹터 수) */
	sector_t s = bad->start; /* 병합 기준 NVMe 시작 LBA */
	u64 *p = bb->page; /* bad table */
	int merged = 0; /* 실제 병합된 섹터 수 */

	WARN_ON(s > BB_END(p[prev])); /* 불가능한 상태: s가 prev 끝보다 뒤면 논리 오류 */

	if (s < BB_END(p[prev])) { /* 중첩된 경우 */
		merged = min_t(sector_t, sectors, BB_END(p[prev]) - s); /* 기존 불량 범위와 중첩된 섹터 수 */ /* 중첩분만큼만 병합: NVMe LBA 범위가 이미 불량으로 커버됨 */
	} else {
		merged = min_t(sector_t, sectors, BB_MAX_LEN - BB_LEN(p[prev])); /* BB_MAX_LEN 초과 방지 */ /* 단일 bad table 항목 최대 길이까지만 확장: NVMe max segment 크기 제한과 유사 (추정) */
		if ((prev + 1) < bb->count && /* 다음 항목이 존재하면 */
		    merged > (BB_OFFSET(p[prev + 1]) - BB_END(p[prev]))) { /* 병합 범위가 다음 불량 항목과 겹치지 않도록 제한 */
			merged = BB_OFFSET(p[prev + 1]) - BB_END(p[prev]); /* 다음 항목과 겹치지 않도록 제한 */ /* 인접 gap만 병합: NVMe namespace에서 분리된 두 불량 영역 사이 정상 LBA 보존 */
		}

		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* prev 항목 확장 */
				  BB_LEN(p[prev]) + merged, bad->ack); /* prev 항목을 확장하여 병합 완료 */ /* ack 상태 유지하며 NVMe 불량 범위 갱신 */
	}

	return merged; /* 병합된 섹터 수 반환, _badblocks_set에서 s/sectors 갱신에 사용 */
}

/*
 * can_combine_front(): 'prev' 항목이 bad->start에서 정확히 시작하고,
 * 그 앞의 prev-1 항목이 prev의 시작과 정확히 맞닿아 있으며, ack가 같고
 * 길이 합이 BB_MAX_LEN 이하일 때 두 항목을 결합할 수 있다.
 * NVMe: 인접한 두 개의 작은 불량 영역을 하나로 합쳐 공간을 절약한다.
 */
static bool can_combine_front(struct badblocks *bb, int prev, /* prev-1과 prev 항목을 prev 기준으로 결합 가능한지 판단 */
			      struct badblocks_context *bad)
{
	u64 *p = bb->page; /* bad table */

	if ((prev > 0) && /* 결합할 두 항목 존재 */
	    (BB_OFFSET(p[prev]) == bad->start) && /* prev 항목이 bad 시작과 정확히 일치 */
	    (BB_END(p[prev - 1]) == BB_OFFSET(p[prev])) && /* prev-1 끝과 prev 시작이 인접 */
	    (BB_LEN(p[prev - 1]) + BB_LEN(p[prev]) <= BB_MAX_LEN) && /* 결합 후 단일 항목 최대 길이 초과 안함 */
	    (BB_ACK(p[prev - 1]) == BB_ACK(p[prev]))) /* ack 상태 일치 */
		return true; /* 결합 가능: NVMe bad table 공간 절약 */
	return false; /* 결합 불가 */
}

/*
 * front_combine(): bad table의 prev와 prev-1 항목을 하나로 결합한다.
 * NVMe: 결합 후 bb->count가 감소하며, namespace 불량 항목 수를 줄인다.
 */
static void front_combine(struct badblocks *bb, int prev) /* prev와 prev-1을 하나로 결합; bad table compaction */
{
	u64 *p = bb->page; /* bad table */

	p[prev - 1] = BB_MAKE(BB_OFFSET(p[prev - 1]), /* prev-1 항목 확장 */
			      BB_LEN(p[prev - 1]) + BB_LEN(p[prev]), /* 길이 합 */
			      BB_ACK(p[prev])); /* ack 상태 */
	if ((prev + 1) < bb->count) /* 뒤에 항목이 있으면 */
		memmove(p + prev, p + prev + 1, (bb->count - prev - 1) * 8); /* 결합 후 빈 슬롯 제거, NVMe bad table 압축 */
}

/*
 * overlap_front(): bad 범위가 bad table의 front 인덱스 항목과 정확히
 * 앞쪽으로 중첩되는지 확인한다. 즉, bad->start가 항목 범위 내에 있는지
 * 검사한다.
 * NVMe: NVMe I/O의 시작 LBA가 이미 알려진 불량 범위 안에 들어가는지
 * 확인하는 핵심 조건이다.
 */
static bool overlap_front(struct badblocks *bb, int front, /* bad 시작 LBA가 front 항목 범위 내에 있는지 확인 */
			  struct badblocks_context *bad)
{
	u64 *p = bb->page; /* bad table */

	if (bad->start >= BB_OFFSET(p[front]) && /* bad 시작이 front 항목 시작 이후 */ /* NVMe I/O 시작 LBA가 불량 항목 시작 이상 */
	    bad->start < BB_END(p[front])) /* bad 시작이 front 항목 끝 이전 */ /* NVMe I/O 시작 LBA가 불량 항목 끝 미만 */
		return true; /* NVMe I/O 시작 LBA가 불량 범위 안에 있음 */ /* 이 I/O는 NVMe media error 영역을 포함하므로 SQ/CQ 제출 전/후 별도 처리 필요 (추정) */
	return false; /* front 중첩 없음 */
}

/*
 * overlap_behind(): bad 범위가 bad table의 behind 인덱스 항목과 뒤쪽으로
 * 중첩되는지 확인한다. 즉, bad 범위가 behind 항목의 시작보다 앞서
 * 시작하고 끝이 behind 항목을 덮치는 경우이다.
 * NVMe: I/O 범위가 다음 불량 범위를 걸치는 경우를 탐지한다.
 */
static bool overlap_behind(struct badblocks *bb, struct badblocks_context *bad, /* bad 범위가 behind 항목과 뒤쪽으로 중첩되는지 확인 */
			   int behind)
{
	u64 *p = bb->page; /* bad table */

	if (bad->start < BB_OFFSET(p[behind]) && /* bad 시작이 behind 항목 시작 이전 */ /* NVMe I/O 시작 LBA가 다음 불량 항목보다 앞섬 */
	    (bad->start + bad->len) > BB_OFFSET(p[behind])) /* bad 끝이 behind 항목 시작을 지남 */ /* NVMe I/O 끝 LBA가 다음 불량 항목 시작을 지나감 */
		return true; /* NVMe I/O 범위가 다음 불량 범위를 걸침 */ /* I/O 범위가 연속된 NVMe 불량 영역을 교차: bio 분할 또는 request 재구성 가능 (추정) */
	return false; /* behind 중첩 없음 */
}

/*
 * can_front_overwrite(): bad 범위가 bad table의 prev 항목을 덮어쓸 수
 * 있는지 판단한다. 덮어쓰려면 bad->ack가 기존 항목의 ack보다 커야 하며,
 * 추가로 분할될 항목 수(extra)를 계산한다.
 * NVMe: acked(상위 메타데이터에 반영된) 불량 정보가 unacked(임시) 정보를
 * 덮어쓸 수 있어, NVMe namespace의 결함 상태를 최신으로 유지한다.
 */
static bool can_front_overwrite(struct badblocks *bb, int prev, /* prev 항목을 bad 범위로 덮어쓸 수 있는지 판단 */
				struct badblocks_context *bad, int *extra)
{
	u64 *p = bb->page; /* bad table */
	int len; /* 덮어쓸 길이 */

	WARN_ON(!overlap_front(bb, prev, bad)); /* overlap_front 조건이어야 정당 */

	if (BB_ACK(p[prev]) >= bad->ack) /* 기존 ack가 더 높거나 같으면 */
		return false; /* 기존 ack가 더 높으면 NVMe 불량 정보 덮어쓰기 불가 */ /* acked 메타데이터가 unacked 임시 오류를 우선: NVMe 상위 RAID 동기화 정책 반영 (추정) */

	if (BB_END(p[prev]) <= (bad->start + bad->len)) { /* bad가 prev 끝까지 또는 넘어 덮음 */
		len = BB_END(p[prev]) - bad->start; /* 덮어쓸 범위를 prev 항목 끝으로 제한 */ /* prev 항목 범위 내에서만 덮어씀 */
		if (BB_OFFSET(p[prev]) == bad->start) /* prev 전체 덮어쓰기 */
			*extra = 0; /* prev 전체를 덮어씀 */ /* 슬롯 수 변화 없음: NVMe bad table in-place 갱신 */
		else /* prev head 남음 */
			*extra = 1; /* prev head가 남아 하나 분할 필요 */ /* head 슬롯 + 새 범위 슬롯 필요 */

		bad->len = len; /* 실제 덮어쓸 NVMe LBA 길이 확정 */
	} else { /* bad가 prev 안에만 있음 */
		if (BB_OFFSET(p[prev]) == bad->start) /* prev 앞부분 덮어쓰기 */
			*extra = 1; /* prev tail이 남아 하나 분할 필요 */ /* 새 범위 + tail 슬롯 필요 */
		else /* prev 중간 덮어쓰기 */
		/*
		 * prev range will be split into two, beside the overwritten
		 * one, an extra slot needed from bad table.
		 */
			*extra = 2; /* prev head/tail 모두 남아 두 개 분할 필요 */ /* head + 새 범위 + tail로 총 3슬롯 필요 */
	}

	if ((bb->count + (*extra)) > MAX_BADBLOCKS) /* bad table 가득 참 */
		return false; /* NVMe bad table 공간 부족으로 덮어쓰기 불가 */ /* 공간 부족 시 NVMe 미디어 오류 등록 실패: 상위 레이어가 abort/requeue 결정 (추정) */

	return true; /* 덮어쓰기 가능 */
}

/*
 * front_overwrite(): bad 범위를 bad table의 prev 항목에 실제로 덮어쓴다.
 * extra 값(0/1/2)에 따라 항목을 교체하거나 분할한다.
 * NVMe: NVMe SSD에서 새로 보고된(acked) 불량 영역이 기존 임시(unacked)
 * 범위를 대체하거나 분할하는 작업을 수행한다.
 */
static int front_overwrite(struct badblocks *bb, int prev, /* prev 항목에 bad 범위 덮어쓰기 수행 */
			   struct badblocks_context *bad, int extra)
{
	u64 *p = bb->page; /* bad table */
	sector_t orig_end = BB_END(p[prev]); /* 원래 prev 끝 LBA 보존 */
	int orig_ack = BB_ACK(p[prev]); /* 원래 prev ack 보존 */

	switch (extra) { /* extra 값에 따른 분기 */
	case 0: /* 전체 교체 */
		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), BB_LEN(p[prev]), /* 같은 범위, ack만 갱신 */
				  bad->ack); /* prev 전체 ack만 변경 */ /* NVMe 상위 메타데이터 상태를 반영 */
		break;
	case 1: /* 하나 분할 */
		if (BB_OFFSET(p[prev]) == bad->start) { /* head 없이 tail만 남음 */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* prev 자리에 새 범위 */
					  bad->len, bad->ack); /* prev head를 새 범위로 교체 */ /* NVMe 불량 LBA 범위 직접 갱신 */
			memmove(p + prev + 2, p + prev + 1, /* 뒤 슬롯 한 칸 밀기 */
				(bb->count - prev - 1) * 8); /* 뒤 항목들을 한 칸 밀기 */ /* tail 슬롯 공간 확보 */
			p[prev + 1] = BB_MAKE(bad->start + bad->len, /* tail 시작 */
					      orig_end - BB_END(p[prev]), /* tail 길이 */
					      orig_ack); /* prev tail을 새 항목으로 */ /* 원래 ack 유지 */
		} else { /* head 남음 */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* head 축소 */
					  bad->start - BB_OFFSET(p[prev]), /* head 길이 */
					  orig_ack); /* prev head는 orig_ack 유지 */ /* 덮어쓰기 전 범위는 원래 ack 상태로 */
			/*
			 * prev +2 -> prev + 1 + 1, which is for,
			 * 1) prev + 1: the slot index of the previous one
			 * 2) + 1: one more slot for extra being 1.
			 */
			memmove(p + prev + 2, p + prev + 1, /* 뒤 슬롯 한 칸 밀기 */
				(bb->count - prev - 1) * 8); /* 뒤 항목들을 한 칸 밀기 */ /* 새 범위 삽입 공간 확보 */
			p[prev + 1] = BB_MAKE(bad->start, bad->len, bad->ack); /* 새 범위 삽입 */ /* NVMe bad table에 새 불량 항목 기록 */
		}
		break;
	case 2: /* 두 개 분할 */
		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* head */
				  bad->start - BB_OFFSET(p[prev]), /* head 길이 */
				  orig_ack); /* prev head는 orig_ack 유지 */ /* 앞쪽은 원래 ack 상태 유지 */
		/*
		 * prev + 3 -> prev + 1 + 2, which is for,
		 * 1) prev + 1: the slot index of the previous one
		 * 2) + 2: two more slots for extra being 2.
		 */
		memmove(p + prev + 3, p + prev + 1, /* 뒤 슬롯 두 칸 밀기 */
			(bb->count - prev - 1) * 8); /* 뒤 항목들을 두 칸 밀기 */ /* head, 새 범위, tail 공간 확보 */
		p[prev + 1] = BB_MAKE(bad->start, bad->len, bad->ack); /* 새 범위 삽입 */ /* NVMe bad table 중간에 새 불량 항목 삽입 */
		p[prev + 2] = BB_MAKE(BB_END(p[prev + 1]), /* tail 시작 */
				      orig_end - BB_END(p[prev + 1]), /* tail 길이 */
				      orig_ack); /* prev tail을 새 항목으로 */ /* 뒤쪽은 원래 ack 상태 유지 */
		break;
	default:
		break;
	}

	return bad->len; /* 실제 덮어쓴 NVMe LBA 길이 반환 */
}

/*
 * insert_at(): bad 범위를 bad table의 'at' 위치에 명시적으로 삽입한다.
 * NVMe: 새로운 불량 LBA 범위를 namespace bad table에 추가할 때 사용된다.
 */
static int insert_at(struct badblocks *bb, int at, struct badblocks_context *bad) /* bad 범위를 at 위치에 삽입 */
{
	u64 *p = bb->page; /* bad table */
	int len; /* 삽입할 길이 */

	WARN_ON(badblocks_full(bb)); /* 가득 찬 상태에서 삽입 시도하면 버그 */ /* NVMe bad table overflow 방지 */

	len = min_t(sector_t, bad->len, BB_MAX_LEN); /* 단일 항목 최대 길이 제한 */ /* NVMe logical block/섹터 단위로 최대 표현 길이 제한 */
	if (at < bb->count) /* 중간 삽입 */
		memmove(p + at + 1, p + at, (bb->count - at) * 8); /* 삽입 위치 뒤 항목들을 한 칸 밀기 */ /* LBA 오름차순 유지: NVMe namespace 불량 목록 정렬 */
	p[at] = BB_MAKE(bad->start, len, bad->ack); /* NVMe bad table에 새 불량 항목 기록 */ /* NVMe SQ/CQ 제출 직전의 LBA 상태 기록 */

	return len; /* 삽입된 NVMe LBA 길이 반환 */
}

/*
 * badblocks_update_acked(): unacked_exist 플래그를 다시 계산한다.
 * NVMe: NVMe 미디어 오류 중 아직 상위 메타데이터에 반영되지 않은
 * unack 항목이 남아 있는지 갱신한다.
 */
static void badblocks_update_acked(struct badblocks *bb) /* unacked_exist 플래그 재계산 */
{
	bool unacked = false; /* unack 항목 존재 플래그 */
	u64 *p = bb->page; /* bad table */
	int i; /* 루프 인덱스 */

	if (!bb->unacked_exist) /* 이미 unack 없음 */
		return; /* 이미 unack 항목이 없다고 표시됨 */ /* NVMe 상위 메타데이터가 모두 동기화된 상태 */

	for (i = 0; i < bb->count ; i++) { /* bad table 전체 순회: NVMe namespace 불량 ack 상태 점검 */
		if (!BB_ACK(p[i])) { /* unack 항목 발견 */
			unacked = true; /* 아직 메타데이터에 미반영된 NVMe 불량 발견 */ /* NVMe 미디어 오류가 상위 RAID/MD에 아직 전파되지 않음 (추정) */
			break; /* 하나라도 있으면 플래그 설정 */
		}
	}

	if (!unacked) /* 모두 acked */
		bb->unacked_exist = 0; /* 모든 NVMe 불량이 acked 상태로 갱신됨 */ /* NVMe 상위 메타데이터 동기화 완료, 더 이상 unack 경고 불필요 */
}

/*
 * try_adjacent_combine(): prev와 prev+1 항목이 인접하고 ack가 같으며
 * 길이 합이 BB_MAX_LEN 이하일 때 병합을 시도한다.
 * NVMe: tail 병합으로 NVMe namespace 끝단의 연속 불량 범위를 하나로
 * 묶어 공간을 절약한다.
 */
static bool try_adjacent_combine(struct badblocks *bb, int prev) /* prev와 prev+1 인접 항목 병합 시도 */
{
	u64 *p = bb->page; /* bad table */

	if (prev >= 0 && (prev + 1) < bb->count && /* 두 항목 존재 */
	    BB_END(p[prev]) == BB_OFFSET(p[prev + 1]) && /* 두 항목이 정확히 인접 */ /* NVMe LBA 공간에서 인접한 불량 영역 */
	    (BB_LEN(p[prev]) + BB_LEN(p[prev + 1])) <= BB_MAX_LEN && /* 최대 길이 제한 만족 */ /* NVMe max segment 제한과 유사 (추정) */
	    BB_ACK(p[prev]) == BB_ACK(p[prev + 1])) { /* ack 상태 일치 */ /* 상위 메타데이터 동기화 상태 일치해야 병합 */
		p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* 앞 항목 확장 */
				  BB_LEN(p[prev]) + BB_LEN(p[prev + 1]), /* 길이 합 */
				  BB_ACK(p[prev])); /* 인접 NVMe 불량 범위를 하나로 통합 */ /* tail 병합으로 NVMe bad table 공간 회수 */

		if ((prev + 2) < bb->count) /* 뒤 항목 존재 */
			memmove(p + prev + 1, p + prev + 2, /* 슬롯 앞당기기 */
				(bb->count -  (prev + 2)) * 8); /* 뒤 항목들을 앞으로 당기기 */ /* 병합으로 생긴 빈 슬롯 제거 */
		bb->count--; /* 항목 수 감소 */
		return true; /* 병합 성공 */
	}
	return false; /* 병합 불가 */
}

/*
 * _badblocks_set(): bad block table에 실제로 불량 범위를 설정하는 내부
 * 엔진이다. 병합, 덮어쓰기, 분할, 삽입 등을 반복적으로 처리한다.
 * 호출 경로: badblocks_set -> _badblocks_set.
 *          (NVMe 관점) nvme_queue_rq 이전에 bio가 badblocks_check로
 *          검사될 수 있고, 그 결과에 따라 nvme_end_io -> end_bio
 *          경로에서 badblocks_set이 호출될 수 있다 (추정).
 * NVMe: NVMe SSD의 NAND 미디어 오류 발생 시 해당 LBA를 bad table에
 * 등록하여 추후 동일 LBA에 대한 I/O를 처리할 때 참조한다.
 */
static bool _badblocks_set(struct badblocks *bb, sector_t s, sector_t sectors, /* 불량 범위 설정 낸부 엔진 */
			   int acknowledged)
{
	int len = 0, added = 0; /* len: 이번 루프 처리 길이, added: 등록/병합 횟수 */
	struct badblocks_context bad; /* 현재 처리 중인 NVMe LBA 범위 및 ack 상태 */
	int prev = -1, hint = -1; /* prev: 기준 항목 인덱스, hint: 탐색 힌트 */
	unsigned long flags; /* irqsave flags: NVMe I/O 완료 경로(softirq/ISR)에서의 동시성 보호 */
	u64 *p; /* bad table */

	if (bb->shift < 0) /* 기능 비활성화: NVMe namespace에 대해 badblocks 추적 안함 */
		/* badblocks are disabled */
		return false; /* NVMe 결함 추적이 비활성화된 상태 (추정) */ /* QUEUE_FLAG_DYING 등과 유사하게 해당 namespace의 badblocks 상태 갱신 중단 (추정) */

	if (sectors == 0) /* 길이 0 */
		/* Invalid sectors number */
		return false; /* NVMe I/O 길이가 0이면 처리 불가 */ /* blk-mq request 길이 0은 정상 제출되지 않음 */

	if (bb->shift) { /* LBA 정렬 단위가 있음 */
		/* round the start down, and the end up */
		sector_t next = s + sectors; /* 현재 NVMe I/O 끝 LBA */

		rounddown(s, 1 << bb->shift); /* NVMe LBA 시작을 shift 단위로 내림 */ /* NVMe logical block 크기/PRP/SGL 경계 정렬과 연관 (추정) */
		roundup(next, 1 << bb->shift); /* NVMe LBA 끝을 shift 단위로 올림 */ /* 정렬 단위 경계를 넘는 DMA/PRP 조각화 방지 목적 (추정) */
		sectors = next - s; /* 정렬 후 실제 처리할 섹터 수 */ /* 정렬 후 처리 길이 */
	}

	write_seqlock_irqsave(&bb->lock, flags); /* NVMe I/O 경로와의 동시 접근 보호 */ /* seqlock write: NVMe completion 경로의 read_seqbegin/read_seqretry와 짝을 이루어 메모리 순서 보장 (추정) */

	bad.ack = acknowledged; /* 설정할 ack 상태 */
	p = bb->page; /* bad table */

re_insert: /* 다음 NVMe LBA 조각 처리 재진입점 */
	bad.start = s; /* 이번 루프에서 처리할 시작 LBA */
	bad.len = sectors; /* 이번 루프에서 처리할 남은 길이 */
	len = 0; /* 이번 루프 실제 처리 길이 초기화 */

	if (badblocks_full(bb)) /* 테이블 가득 참 */
		goto out; /* NVMe bad table이 가득 찼으면 더 이상 등록 불가 */ /* queue depth 초과와 유사: 더 이상 NVMe 불량 정보를 수용할 수 없음 (추정) */

	if (badblocks_empty(bb)) { /* 빈 테이블 */
		len = insert_at(bb, 0, &bad); /* 첫 항목 삽입 */
		bb->count++; /* 항목 수 증가 */
		added++; /* 등록 발생 */
		goto update_sectors; /* 남은 범위 갱신 */
	}

	prev = prev_badblocks(bb, &bad, hint); /* NVMe namespace LBA에서 이전 불량 항목 탐색 */ /* hint를 통한 O(1) 또는 이진 탐색: NVMe SQ/CQ doorbell 지연에 미치지 않도록 */

	/* start before all badblocks */
	if (prev < 0) { /* s가 모든 불량보다 앞 */
		/* insert on the first */
		if (bad.len > (BB_OFFSET(p[0]) - bad.start)) /* 첫 항목 시작 전 영역으로 제한 */
			bad.len = BB_OFFSET(p[0]) - bad.start; /* 첫 불량 항목 전까지만 삽입 */ /* NVMe namespace에서 다음 불량 영역 전까지만 새 항목 등록 */
		len = insert_at(bb, 0, &bad); /* 맨 앞 삽입 */
		bb->count++; /* 항목 수 증가 */
		added++; /* 등록 발생 */
		hint = ++prev; /* 다음 루프 힌트 */
		goto update_sectors; /* 남은 범위 갱신 */
	}

	/* in case p[prev-1] can be merged with p[prev] */
	if (can_combine_front(bb, prev, &bad)) { /* prev-1과 prev 결합 가능 */
		front_combine(bb, prev); /* 결합 수행 */
		bb->count--; /* 항목 수 감소 */
		added++; /* 등록 처리 */
		hint = prev; /* 힌트 갱신 */
		goto update_sectors; /* 남은 범위 갱신 */
	}

	if (can_merge_front(bb, prev, &bad)) { /* prev와 병합 가능 */
		len = front_merge(bb, prev, &bad); /* 병합 수행 */
		added++; /* 등록 처리 */
		hint = prev; /* 힌트 갱신 */
		goto update_sectors; /* 남은 범위 갱신 */
	}

	if (overlap_front(bb, prev, &bad)) { /* prev와 중첩 */
		int extra = 0; /* 분할 시 필요한 추가 슬롯 수 */

		if (!can_front_overwrite(bb, prev, &bad, &extra)) { /* 덮어쓰기 불가 */
			if (extra > 0)
				goto out; /* 공간 부족으로 NVMe 불량 등록 실패 */ /* NVMe command abort 또는 상위 레이어 재처리 가능 (추정) */

			len = min_t(sector_t, /* 기존 불량과 겹치는 만큼 스킵 */
				    BB_END(p[prev]) - s, sectors); /* 기존 불량 범위와 겹치는 만큼만 스킵 */ /* 중복 등록 회피: NVMe LBA 범위의 이미 커버된 부분은 스킵 */
			hint = prev; /* 힌트 */
			goto update_sectors; /* 다음 조각 처리 */
		}

		len = front_overwrite(bb, prev, &bad, extra); /* 덮어쓰기 수행 */
		added++; /* 등록 처리 */
		bb->count += extra; /* 추가 슬롯 반영 */

		if (can_combine_front(bb, prev, &bad)) { /* 덮어쓴 후 앞 항목과 결합 가능 */
			front_combine(bb, prev); /* 결합 */
			bb->count--; /* 슬롯 회수 */
		}

		hint = prev; /* 힌트; 남은 범위 갱신 */
		goto update_sectors;
	}

	/* cannot merge and there is space in bad table */
	if ((prev + 1) < bb->count && /* 뒤 항목 존재 */
	    overlap_behind(bb, &bad, prev + 1)) /* 뒤 항목과 중첩 */
		bad.len = min_t(sector_t,
				bad.len, BB_OFFSET(p[prev + 1]) - bad.start); /* 뒤 불량 항목 전까지만 삽입 */ /* NVMe namespace에서 다음 불량 영역과 겹치지 않도록 삽입 길이 조정 */

	len = insert_at(bb, prev + 1, &bad); /* prev 뒤에 삽입 */
	bb->count++; /* 항목 수 증가 */
	added++; /* 등록 */
	hint = ++prev; /* 힌트 */

update_sectors: /* NVMe LBA 진행 상황 갱신 */
	s += len; /* 처리된 만큼 시작 LBA 전진 */
	sectors -= len; /* 남은 길이 감소 */

	if (sectors > 0) /* 아직 처리할 범위 남음 */
		goto re_insert; /* NVMe LBA 범위의 남은 부분을 다음 루프에서 처리 */ /* submit_bio로 들어온 큰 NVMe I/O를 BB_MAX_LEN 단위로 분할 처리 (추정) */

	/*
	 * Check whether the following already set range can be
	 * merged. (prev < 0) condition is not handled here,
	 * because it's already complicated enough.
	 */
	try_adjacent_combine(bb, prev); /* tail 병합: namespace 끝단 연속 불량 항목 통합 */ /* dispatch batch 마지막에 인접 request 병합하는 것과 유사 (추정) */

out:
	if (added) { /* 변경 발생 */
		set_changed(bb); /* 메타데이터 동기화 필요 플래그 */

		if (!acknowledged) /* 이번 등록이 unacked */
			bb->unacked_exist = 1; /* NVMe 미디어 오류가 아직 메타데이터에 미반영 */ /* NVMe end-io 경로에서 unack 미디어 오류가 있음을 상위 MD/RAID에 알리는 신호 (추정) */
		else /* acked 등록 */
			badblocks_update_acked(bb); /* unacked_exist 재검사 */
	}

	write_sequnlock_irqrestore(&bb->lock, flags); /* seqlock 해제: NVMe completion 경로의 read_seqretry가 변경을 인지 */

	return sectors == 0; /* 전체 범위 등록 성공 여부 */
}

/*
 * front_clear(): bad table의 prev 항목과 앞쪽으로 중첩된 영역을
 * 해제(clear)한다. head가 잘리거나, 항목 전체가 삭제되거나, tail이
 * 잘리는 경우를 처리한다.
 * NVMe: NVMe SSD에서 오진(false positive)이나 복구된 영역을 bad table에서
 * 제거할 때 사용된다 (드묾).
 */
static int front_clear(struct badblocks *bb, int prev, /* prev 항목과 중첩된 영역 해제 */
		       struct badblocks_context *bad, int *deleted)
{
	sector_t sectors = bad->len; /* 클리어할 NVMe LBA 길이 */
	sector_t s = bad->start; /* 클리어 시작 LBA */
	u64 *p = bb->page; /* bad table */
	int cleared = 0; /* 클리어된 섹터 수 */

	*deleted = 0; /* 삭제 여부 초기화 */
	if (s == BB_OFFSET(p[prev])) { /* 클리어 시작이 prev 항목 시작과 일치 */
		if (BB_LEN(p[prev]) > sectors) { /* 일부만 클리어 */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]) + sectors, /* head 시작 이동 */
					  BB_LEN(p[prev]) - sectors, /* head 길이 */
					  BB_ACK(p[prev])); /* prev head를 잘라냄 */ /* NVMe namespace에서 정상화된 앞부분 제거 */
			cleared = sectors; /* 클리어 길이 */
		} else { /* prev 전체 클리어 */
			/* BB_LEN(p[prev]) <= sectors */
			cleared = BB_LEN(p[prev]); /* prev 전체가 클리어 대상 */ /* 해당 NVMe 불량 항목 전체 제거 */
			if ((prev + 1) < bb->count) /* 뒤 항목 존재 */
				memmove(p + prev, p + prev + 1, /* 슬롯 앞당기기 */
				       (bb->count - prev - 1) * 8); /* 뒤 항목들을 앞으로 당기기 */ /* bad table compaction */
			*deleted = 1; /* 항목 삭제 표시 */
		}
	} else if (s > BB_OFFSET(p[prev])) { /* 중간 또는 tail 클리어 */
		if (BB_END(p[prev]) <= (s + sectors)) { /* tail 클리어 */
			cleared = BB_END(p[prev]) - s; /* tail 길이 */
			p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* head 길이 축소 */
					  s - BB_OFFSET(p[prev]), /* head 길이 */
					  BB_ACK(p[prev])); /* prev tail을 잘라냄 */ /* NVMe 불량 범위 끝부분 정상화 */
		} else { /* 중간 클리어: 별도 함수에서 처리 */
			/* Splitting is handled in front_splitting_clear() */
			BUG(); /* 중앙 클리어는 별도 함수에서 처리해야 함 */ /* front_splitting_clear로 분기되지 않으면 NVMe bad table 불변식 위반 */
		}
	}

	return cleared;
}

/*
 * front_splitting_clear(): 해제 범위가 기존 불량 범위의 정중앙을 덮칠
 * 때, 기존 범위를 두 개로 분할한다.
 * NVMe: NVMe namespace에서 부분적으로 복구/정상화된 영역을 제거할 때
 * bad table 항목을 둘로 나누어 저장한다.
 */
static int front_splitting_clear(struct badblocks *bb, int prev, /* 중앙 클리어 시 prev 분할 */
				  struct badblocks_context *bad)
{
	u64 *p = bb->page; /* bad table */
	u64 end = BB_END(p[prev]); /* 원래 끝 LBA */
	int ack = BB_ACK(p[prev]); /* 원래 ack */
	sector_t sectors = bad->len; /* 클리어할 길이 */
	sector_t s = bad->start; /* 클리어 시작 */

	p[prev] = BB_MAKE(BB_OFFSET(p[prev]), /* head */
			  s - BB_OFFSET(p[prev]), /* head 길이 */
			  ack); /* prev를 클리어 시작 전까지의 head로 축소 */ /* NVMe 불량 범위 앞부분 유지 */
	memmove(p + prev + 2, p + prev + 1, (bb->count - prev - 1) * 8); /* 뒤 항목들을 한 칸 밀기 */ /* tail 슬롯 공간 확보 */
	p[prev + 1] = BB_MAKE(s + sectors, end - s - sectors, ack); /* 클리어 끝 이후의 tail을 새 항목으로 */ /* NVMe 불량 범위 뒷부분 유지 */
	return sectors; /* 클리어된 길이 */
}

/*
 * _badblocks_clear(): bad table에서 불량 범위를 제거하는 내부 엔진이다.
 * 호출 경로: badblocks_clear -> _badblocks_clear.
 * NVMe: NVMe SSD의 특정 LBA 범위가 정상화되었거나 잘못 등록되었을 때
 * bad table에서 해당 범위를 제거한다.
 */
static bool _badblocks_clear(struct badblocks *bb, sector_t s, sector_t sectors) /* bad table에서 불량 범위 제거 */
{
	struct badblocks_context bad; /* 현재 클리어 대상 NVMe LBA 범위 */
	int prev = -1, hint = -1; /* 기준 항목 및 힌트 */
	int len = 0, cleared = 0; /* 처리 길이 및 클리어 성공 카운트 */
	u64 *p; /* bad table */

	if (bb->shift < 0) /* 비활성화 */
		/* badblocks are disabled */
		return false; /* NVMe 결함 추적 비활성화 상태 (추정) */ /* 해당 NVMe namespace에 대해서는 clear 동작 무의미 */

	if (sectors == 0) /* 길이 0 */
		/* Invalid sectors number */
		return false; /* 처리 불가 */

	if (bb->shift) { /* 정렬 단위 존재 */
		sector_t target; /* 끝 LBA */

		/* When clearing we round the start up and the end down.
		 * This should not matter as the shift should align with
		 * the block size and no rounding should ever be needed.
		 * However it is better the think a block is bad when it
		 * isn't than to think a block is not bad when it is.
		 */
		target = s + sectors; /* 원래 끝 */
		roundup(s, 1 << bb->shift); /* NVMe LBA 시작을 shift 단위로 올림 */ /* NVMe format logical block 경계에 맞춤 (추정) */
		rounddown(target, 1 << bb->shift); /* NVMe LBA 끝을 shift 단위로 내림 */ /* DMA/PRP/SGL 단위보다 작은 불량 블록은 제거 대상에서 제외 (추정) */
		sectors = target - s; /* 정렬 후 길이 */
	}

	write_seqlock_irq(&bb->lock); /* NVMe I/O 경로와의 동시 접근 보호 (irq 버전) */ /* bi_end_io / NVMe ISR 컨텍스트에서 호출될 수 있어 irqsave 사용 (추정) */

	bad.ack = true; /* clear는 항상 acked 동작 */
	p = bb->page; /* bad table */

re_clear: /* 다음 NVMe LBA 조각 처리 */
	bad.start = s; /* 이번 루프 시작 */
	bad.len = sectors; /* 이번 루프 남은 길이 */

	if (badblocks_empty(bb)) { /* 빈 테이블 */
		len = sectors; /* 전체 정상 영역으로 처리 */
		cleared++; /* 해제할 불량이 없으므로 성공으로 처리 */ /* NVMe namespace에 등록된 불량이 없음 */
		goto update_sectors; /* 진행 */
	}

	prev = prev_badblocks(bb, &bad, hint); /* NVMe namespace LBA에서 기준 항목 탐색 */

	/* Start before all badblocks */
	if (prev < 0) {
		if (overlap_behind(bb, &bad, 0)) { /* 첫 항목과 중첩 */
			len = BB_OFFSET(p[0]) - s; /* 첫 불량 항목 시작 전까지만 클리어 */ /* NVMe namespace 첫 불량 영역 이전만 정상 영역으로 간주 */
			hint = 0; /* 다음 힌트 */
		} else {
			len = sectors; /* 불량 범위와 전혀 겹치지 않음 */ /* NVMe LBA 범위 전체가 정상 */
		}
		/*
		 * Both situations are to clear non-bad range,
		 * should be treated as successful
		 */
		cleared++; /* 성공 처리 */
		goto update_sectors; /* 진행 */
	}

	/* Start after all badblocks */
	if ((prev + 1) >= bb->count && !overlap_front(bb, prev, &bad)) { /* 마지막 불량 이후 */
		len = sectors; /* 마지막 불량 항목 이후는 정상 영역 */ /* NVMe I/O 범위가 마지막 불량 이후 */
		cleared++; /* 성공 */
		goto update_sectors; /* 진행 */
	}

	/* Clear will split a bad record but the table is full */
	if (badblocks_full(bb) && (BB_OFFSET(p[prev]) < bad.start) && /* 가득 찬 상태에서 중앙 클리어 */
	    (BB_END(p[prev]) > (bad.start + sectors))) { /* prev가 clear 범위를 완전히 덮음 */
		len = sectors; /* 분할 공간 없이 전체를 클리어 처리 (불량 정보 손실 가능, 추정) */ /* NVMe bad table overflow 방지를 위해 강제 제거: 하위 RAID 레이어가 재동기화 필요 (추정) */
		goto update_sectors; /* 진행 */
	}

	if (overlap_front(bb, prev, &bad)) { /* prev와 중첩 */
		if ((BB_OFFSET(p[prev]) < bad.start) && /* 중앙 클리어 */
		    (BB_END(p[prev]) > (bad.start + bad.len))) { /* prev가 clear 범위를 양쪽으로 덮음 */
			/* Splitting */
			if ((bb->count + 1) <= MAX_BADBLOCKS) { /* 분할 공간 있음 */
				len = front_splitting_clear(bb, prev, &bad); /* 분할 클리어 */
				bb->count += 1; /* 항목 수 증가 */
				cleared++; /* 성공 */
			} else {
				/* No space to split, give up */
				len = sectors; /* NVMe bad table 공간 부족으로 클리어 포기 */
			}
		} else {
			int deleted = 0; /* 삭제 여부 */

			len = front_clear(bb, prev, &bad, &deleted); /* 앞/뒤 클리어 */
			bb->count -= deleted; /* 삭제 시 항목 수 감소 */
			cleared++; /* 성공 */
			hint = prev; /* 힌트 */
		}

		goto update_sectors;
	}

	/* Not front overlap, but behind overlap */
	if ((prev + 1) < bb->count && overlap_behind(bb, &bad, prev + 1)) { /* 뒤 항목과 중첩 */
		len = BB_OFFSET(p[prev + 1]) - bad.start; /* 다음 불량 항목 시작 전까지만 클리어 */ /* NVMe namespace 다음 불량 영역 시작 전까지만 정상 영역으로 처리 */
		hint = prev + 1; /* 힌트 */
		/* Clear non-bad range should be treated as successful */
		cleared++; /* 성공 */
		goto update_sectors;
	}

	/* Not cover any badblocks range in the table */
	len = sectors; /* bad table에 해당 LBA 범위 없음 */ /* NVMe LBA 범위에 불량 항목 없음 */
	/* Clear non-bad range should be treated as successful */
	cleared++; /* 성공 */

update_sectors: /* 진행 */
	s += len; /* 시작 전진 */
	sectors -= len; /* 남은 길이 감소 */

	if (sectors > 0) /* 남음 */
		goto re_clear; /* NVMe LBA 범위의 남은 부분을 다음 루프에서 처리 */ /* 큰 NVMe LBA 범위를 shift/BB_MAX_LEN 단위로 분할 처리 (추정) */

	if (cleared) { /* 변경 발생 */
		badblocks_update_acked(bb); /* unacked_exist 갱신 */
		set_changed(bb); /* 메타데이터 변경 플래그 */
	}

	write_sequnlock_irq(&bb->lock); /* seqlock 해제 */

	if (!cleared) /* 아무것도 클리어 못함 */
		return false; /* 실패 */

	return true; /* 성공 */
}

/*
 * _badblocks_check(): 주어진 LBA 범위가 bad table에 등록된 불량 범위와
 * 겹치는지 검사한다. acked/unacked 여부에 따라 1/-1/0을 반환한다.
 * 호출 경로: badblocks_check -> _badblocks_check.
 *          (NVMe 관점) submit_bio -> blk_mq_submit_bio -> ... ->
 *          nvme_queue_rq 이전이나 이후에 상위 레이어(예: MD)가 호출
 *          가능하다 (추정).
 * NVMe: NVMe SSD의 특정 LBA 범위로 I/O를 본내기 전에, 해당 범위가
 * 불량인지 확인하여 불필요한 하드웨어 명령 제출(SQ/CQ, doorbell)을
 * 피하거나, 미디어 오류 처리 경로를 사전에 선택한다.
 */
static int _badblocks_check(struct badblocks *bb, sector_t s, sector_t sectors, /* LBA 범위 불량 검사 */
			    sector_t *first_bad, sector_t *bad_sectors)
{
	int prev = -1, hint = -1, set = 0; /* prev: 기준, hint, set: first_bad 설정 여부 */
	struct badblocks_context bad; /* 검사 대상 NVMe LBA 범위 */
	int unacked_badblocks = 0; /* unack 불량 카운트 */
	int acked_badblocks = 0; /* ack 불량 카운트 */
	u64 *p = bb->page; /* bad table */
	int len, rv; /* len: 이번 루프 길이, rv: 반환값 */

re_check: /* 다음 NVMe LBA 조각 검사 */
	bad.start = s; /* 이번 루프 시작 */
	bad.len = sectors; /* 이번 루프 남은 길이 */

	if (badblocks_empty(bb)) { /* 빈 테이블 */
		len = sectors; /* bad table이 비었으면 NVMe LBA 전체가 정상 */ /* submit_bio -> blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq 이전 badblocks_check에서 정상 LBA로 판단 가능 (추정) */
		goto update_sectors; /* 진행 */
	}

	prev = prev_badblocks(bb, &bad, hint); /* NVMe namespace LBA에서 기준 항목 탐색 */ /* hint로 O(1) 또는 이진 탐색, NVMe CID/tag 할당 전 LBA 상태 확인 */

	/* start after all badblocks */
	if ((prev >= 0) && /* 기준 항목 존재 */
	    ((prev + 1) >= bb->count) && !overlap_front(bb, prev, &bad)) { /* 마지막 항목 이후 */
		len = sectors; /* 마지막 불량 항목 이후는 정상 영역 */ /* NVMe I/O 범위가 마지막 불량 이후 */
		goto update_sectors; /* 진행 */
	}

	/* Overlapped with front badblocks record */
	if ((prev >= 0) && overlap_front(bb, prev, &bad)) { /* prev와 중첩 */
		if (BB_ACK(p[prev])) /* acked 여부 */
			acked_badblocks++; /* 메타데이터에 반영된 NVMe 불량 */ /* NVMe 상위 RAID/MD가 이미 인지한 불량 */
		else /* unacked */
			unacked_badblocks++; /* 아직 메타데이터에 미반영된 NVMe 불량 */ /* NVMe ISR/completion 경로에서 새로 보고된 미디어 오류 (추정) */

		if (BB_END(p[prev]) >= (s + sectors)) /* prev가 I/O 전체 덮음 */
			len = sectors; /* 현재 불량 항목이 I/O 범위 전체를 덮음 */ /* NVMe I/O 전체가 불량 영역 내: doorbell 제출 자체를 피하거나 에러 완료 처리 가능 (추정) */
		else /* 일부만 덮음 */
			len = BB_END(p[prev]) - s; /* 불량 항목 끝까지만 겹침 */ /* NVMe I/O 중 불량 영역까지의 길이; 나머지는 다음 루프에서 재검사 */

		if (set == 0) { /* 첫 불량 아직 기록 안함 */
			*first_bad = BB_OFFSET(p[prev]); /* NVMe I/O에서 첫 불량 LBA */ /* nvme_end_request -> bio_endio 체인에서 보고될 첫 미디어 오류 LBA (추정) */
			*bad_sectors = BB_LEN(p[prev]); /* 첫 불량 범위 길이 */ /* NVMe namespace 불량 범위 길이 */
			set = 1; /* 기록 완료 */
		}
		goto update_sectors; /* 진행 */
	}

	/* Not front overlap, but behind overlap */
	if ((prev + 1) < bb->count && overlap_behind(bb, &bad, prev + 1)) { /* 뒤 항목과 중첩 */
		len = BB_OFFSET(p[prev + 1]) - bad.start; /* 다음 불량 항목 시작 전까지 정상 */ /* NVMe I/O 중 다음 불량 영역 시작 전까지는 정상 LBA로 처리 */
		hint = prev + 1; /* 힌트 */
		goto update_sectors;
	}

	/* not cover any badblocks range in the table */
	len = sectors; /* bad table에 해당 NVMe LBA 범위 없음 */ /* NVMe I/O 전체가 bad table에 없는 정상 LBA */

update_sectors: /* 진행 */
	/* This situation should never happen */
	WARN_ON(sectors < len);

	s += len; /* 시작 전진 */
	sectors -= len; /* 남은 길이 감소 */

	if (sectors > 0) /* 남음 */
		goto re_check; /* NVMe I/O 범위의 남은 부분을 다음 루프에서 검사 */ /* 큰 NVMe I/O를 단위 길이로 나누어 acked/unacked 상태별로 분류 검사 (추정) */

	if (unacked_badblocks > 0) /* unack 존재 */
		rv = -1; /* unack NVMe 불량 존재: 상위 메타데이터 동기화 필요 */ /* NVMe SQ/CQ 완료 후 상위 MD/RAID가 즉시 메타데이터 갱신해야 함 (추정) */
	else if (acked_badblocks > 0) /* acked만 존재 */
		rv = 1; /* acked NVMe 불량만 존재 */ /* 메타데이터에 이미 반영된 NVMe 불량 */
	else /* 없음 */
		rv = 0; /* NVMe LBA 범위 내 불량 없음 */ /* 정상 NVMe I/O 경로 */

	return rv; /* 검사 결과 반환 */
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
 * NVMe 연결: 이 함수는 NVMe SSD namespace의 특정 LBA 범위에 대해
 * badblocks_check()를 수행한다. seqlock을 사용하여 NVMe I/O 완료
 * 경로(bi_end_io)와의 동시 접근에 안전하며, read_seqretry로 일관성을
 * 보장한다. 반환값 1은 acked 불량(메타데이터에 기록됨), -1은 unacked
 * 불량(아직 NVMe 상위 레이어가 인식하지 못함)을 의미한다.
 */
int badblocks_check(struct badblocks *bb, sector_t s, sector_t sectors, /* LBA 범위 불량 검사 외부 인터페이스 */
			sector_t *first_bad, sector_t *bad_sectors)
{
	unsigned int seq; /* seqlock 시퀀스 번호 */
	int rv; /* 반환값 */

	WARN_ON(bb->shift < 0 || sectors == 0); /* 전제조건 위반 */

	if (bb->shift > 0) { /* 정렬 단위 존재 */
		/* round the start down, and the end up */
		sector_t target = s + sectors; /* 끝 LBA */

		rounddown(s, 1 << bb->shift); /* 시작 내림 */
		roundup(target, 1 << bb->shift); /* 끝 올림 */
		sectors = target - s; /* 정렬 후 길이 */
	}

retry: /* seqlock retry */
	seq = read_seqbegin(&bb->lock); /* NVMe I/O 검사 시작 전 seqlock 읽기 */ /* read_seqbegin은 이후 _badblocks_check 내 접근에 대한 메모리 순서 보장 (추정) */
	rv = _badblocks_check(bb, s, sectors, first_bad, bad_sectors); /* 실제 검사 */
	if (read_seqretry(&bb->lock, seq)) /* seqlock 갱신 감지 */
		goto retry; /* seqlock 갱신 시 NVMe bad table이 바뀌었으므로 재시도 */ /* NVMe completion 경로의 write_sequnlock과 짝을 이루어 일관성 보장 (추정) */

	return rv; /* 결과 반환 */
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
 * NVMe 연결: NVMe SSD에서 미디어 오류가 보고된 LBA 범위를 bad table에
 * 추가한다. NVMe end-io 경로에서 nvme_end_request -> bio_endio 체인을
 * 통해 호출될 수 있으며(추정), unacked 상태로 등록되면 상위 MD/RAID
 * 메타데이터 갱신을 유도한다.
 */
bool badblocks_set(struct badblocks *bb, sector_t s, sector_t sectors, /* 불량 범위 추가 외부 인터페이스 */
		   int acknowledged)
{
	return _badblocks_set(bb, s, sectors, acknowledged); /* 낸부 엔진 위임 */
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
 * NVMe 연결: NVMe SSD의 특정 LBA 범위가 정상화되었거나 오진된 경우
 * bad table에서 제거한다. table이 가득 찬 상태에서 분할이 필요하면
 * 요청을 포기할 수 있다.
 */
bool badblocks_clear(struct badblocks *bb, sector_t s, sector_t sectors) /* 불량 범위 제거 외부 인터페이스 */
{
	return _badblocks_clear(bb, s, sectors); /* 낸부 엔진 위임 */
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
 * NVMe 연결: NVMe SSD에서 발생한 모든 unack 불량 블록을 acked 상태로
 * 일괄 전환한다. MD 등 상위 메타데이터 업데이트가 완료된 후 호출되며,
 * 이후 badblocks_check()는 해당 범위를 메타데이터에 반영된 불량으로
 * 인식한다.
 */
void ack_all_badblocks(struct badblocks *bb) /* 모든 불량 ack */
{
	if (bb->page == NULL || bb->changed) /* 초기화/변경 중 */
		/* no point even trying */
		return; /* 메타데이터가 변경 중이면 NVMe 불량 ack 일괄 전환 불가 */ /* NVMe 상위 RAID 동기화 충돌 방지 */
	write_seqlock_irq(&bb->lock); /* seqlock 획득 */

	if (bb->changed == 0 && bb->unacked_exist) { /* 변경 없고 unack 존재 */
		u64 *p = bb->page; /* bad table */
		int i; /* 루프 인덱스 */

		for (i = 0; i < bb->count ; i++) { /* bad table 전체 순회 */
			if (!BB_ACK(p[i])) { /* unack 항목 */
				sector_t start = BB_OFFSET(p[i]); /* 시작 LBA */
				int len = BB_LEN(p[i]); /* 길이 */

				p[i] = BB_MAKE(start, len, 1); /* unack -> acked 전환 */ /* NVMe 미디어 오류를 상위 메타데이터에 반영 완료 */
			}
		}

		for (i = 0; i < bb->count ; i++) /* 인접 항목 병합 */
			while (try_adjacent_combine(bb, i)) /* NVMe bad table compaction */
				; /* ack 통합 후 인접 NVMe 불량 범위 병합 */

		bb->unacked_exist = 0; /* unack 플래그 해제 */
	}
	write_sequnlock_irq(&bb->lock); /* seqlock 해제 */
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
 * NVMe 연결: NVMe namespace 디바이스의 sysfs를 통해 bad block 목록을
 * 사용자 공간에 노출한다. 관리자가 NVMe SSD의 미디어 결함 상태를
 * 모니터링하는 데 사용된다.
 */
ssize_t badblocks_show(struct badblocks *bb, char *page, int unack) /* sysfs 조회 */
{
	size_t len; /* 출력 길이 */
	int i; /* 루프 인덱스 */
	u64 *p = bb->page; /* bad table */
	unsigned seq; /* seqlock 시퀀스 */

	if (bb->shift < 0) /* 비활성화 */
		return 0; /* NVMe badblocks 비활성화 시 빈 sysfs 출력 */ /* 해당 NVMe namespace는 badblocks 미사용 */

retry: /* retry label */
	seq = read_seqbegin(&bb->lock); /* sysfs 읽기 시 seqlock 획득 */ /* NVMe I/O 경로의 write_seqlock과 짝을 이루는 read-side 임계구간 */

	len = 0; /* 출력 오프셋 초기화 */
	i = 0; /* 인덱스 초기화 */

	while (len < PAGE_SIZE && i < bb->count) { /* bad table 순회 출력 */
		sector_t s = BB_OFFSET(p[i]); /* NVMe namespace 불량 시작 LBA */ /* 사용자 공간에 노출될 NVMe LBA */
		unsigned int length = BB_LEN(p[i]); /* 불량 범위 길이(섹터) */ /* NVMe namespace 불량 범위 길이 */
		int ack = BB_ACK(p[i]); /* 메타데이터 반영 여부 */ /* NVMe 상위 메타데이터 동기화 여부 */

		i++; /* 다음 항목 */

		if (unack && ack) /* unack-only 모드에서 acked 스킵 */
			continue; /* unack-only 모드에서는 acked 항목 스킵 */ /* 아직 상위 메타데이터에 반영되지 않은 NVMe 불량만 노출 */

		len += snprintf(page+len, PAGE_SIZE-len, "%llu %u\n", /* sysfs 버퍼 기록 */
				(unsigned long long)s << bb->shift, /* shift 적용 시작 */
				length << bb->shift); /* shift 적용 길이 */
	}
	if (unack && len == 0) /* unack-only인데 출력 없음 */
		bb->unacked_exist = 0; /* sysfs 읽기로 unack 항목이 없음을 확인 */ /* NVMe 상위 메타데이터가 이미 동기화된 상태로 간주 */

	if (read_seqretry(&bb->lock, seq)) /* 변경 감지 */
		goto retry; /* NVMe bad table 변경 시 재시도 */ /* sysfs 읽기 중 NVMe completion 경로가 갱신하면 일관성 위해 재시도 */

	return len; /* 출력 길이 */
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
 * NVMe 연결: 사용자가 NVMe namespace의 sysfs 인터페이스를 통해
 * 불량 블록을 수동으로 등록할 때 호출된다. NVMe SSD 관리 도구에서
 * 미디어 결함 LBA를 주입하거나 복구 상태를 갱신하는 데 쓰인다 (추정).
 */
ssize_t badblocks_store(struct badblocks *bb, const char *page, size_t len, /* sysfs 등록 */
			int unack)
{
	unsigned long long sector; /* 사용자 입력 시작 섹터 */
	int length; /* 사용자 입력 길이 */
	char newline; /* 개행 문자 */

	switch (sscanf(page, "%llu %d%c", &sector, &length, &newline)) { /* sysfs 입력 파싱 */
	case 3:
		if (newline != '\n') /* 개행 체크 */
			return -EINVAL; /* 줄바꿈 형식 오류 */ /* NVMe 관리 도구 입력 형식 오류 */
		fallthrough;
	case 2:
		if (length <= 0) /* 길이 체크 */
			return -EINVAL; /* NVMe 불량 길이는 양수여야 함 */ /* NVMe LBA 범위는 양의 길이 필요 */
		break;
	default:
		return -EINVAL; /* 파싱 실패 */
	}

	if (!badblocks_set(bb, sector, length, !unack)) /* 등록 시도 */
		return -ENOSPC; /* NVMe bad table 공간 부족 */ /* NVMe namespace bad table이 가득 참: 관리자가 table 크기 확보 필요 (추정) */

	return len; /* 처리된 바이트 수 */
}
EXPORT_SYMBOL_GPL(badblocks_store);

/*
 * __badblocks_init(): badblocks 구조체를 내부적으로 초기화한다.
 * NVMe: NVMe namespace 생성 시 해당 namespace의 미디어 결함 추적
 * 객체를 초기화하는 데 사용된다 (추정).
 */
static int __badblocks_init(struct device *dev, struct badblocks *bb, /* 낸부 초기화 */
		int enable)
{
	bb->dev = dev; /* NVMe namespace device 연결 */ /* device 생명주기에 따른 메모리 관리 */
	bb->count = 0; /* 초기에는 추적 중인 NVMe 불량 항목 없음 */ /* NVMe namespace 생성 시 불량 정보 없음 */
	if (enable) /* 활성화 여부 */
		bb->shift = 0; /* 512바이트 섹터 단위로 NVMe LBA 추적 */ /* NVMe logical block 크기 512B 가정 기본값 (추정) */
	else
		bb->shift = -1; /* NVMe badblocks 기능 비활성화 */ /* badblocks 추적 끔 */
	if (dev)
		bb->page = devm_kzalloc(dev, PAGE_SIZE, GFP_KERNEL); /* device-managed 메모리 할당 */
	else /* 일반 메모리 할당 */
		bb->page = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!bb->page) { /* 할당 실패 */
		bb->shift = -1; /* 메모리 할당 실패 시 NVMe 결함 추적 비활성화 */ /* 메모리 부족으로 NVMe 불량 관리 기능을 안전하게 비활성화 */
		return -ENOMEM; /* 메모리 부족 */
	}
	seqlock_init(&bb->lock); /* NVMe I/O 경로와 sysfs 동시 접근 보호 초기화 */ /* NVMe ISR/completion과 sysfs/admin 사이의 RCU-like 일관성 메커니즘 (추정) */

	return 0;
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
 * NVMe 연결: NVMe 드라이버 외부의 서브시스템(예: MD)에서 badblocks
 * 구조체를 초기화할 때 사용된다.
 */
int badblocks_init(struct badblocks *bb, int enable) /* 외부 초기화 */
{
	return __badblocks_init(NULL, bb, enable); /* 낸부 초기화 호출 */
}
EXPORT_SYMBOL_GPL(badblocks_init);

/*
 * devm_init_badblocks(): device-managed 방식으로 badblocks를 초기화한다.
 * NVMe: NVMe namespace의 device 생명주기에 따라 자동 해제되는 badblocks
 * 객체를 초기화할 때 사용될 수 있다 (추정).
 */
int devm_init_badblocks(struct device *dev, struct badblocks *bb) /* device-managed 초기화 */
{
	if (!bb) /* null 체크 */
		return -EINVAL; /* 인자 오류 */
	return __badblocks_init(dev, bb, 1); /* device-managed 초기화 */
}
EXPORT_SYMBOL_GPL(devm_init_badblocks);

/**
 * badblocks_exit() - free the badblocks structure
 * @bb:		the badblocks structure that holds all badblock information
 */
/*
 * NVMe 연결: NVMe namespace 제거 시 해당 namespace의 bad block table
 * 메모리를 해제한다.
 */
void badblocks_exit(struct badblocks *bb) /* 종료 */
{
	if (!bb) /* null 체크 */
		return; /* 아무것도 안함 */
	if (bb->dev) /* device-managed 여부 */
		devm_kfree(bb->dev, bb->page); /* device-managed 메모리 해제 */ /* NVMe namespace 제거 시 메모리 자동 해제 */
	else
		kfree(bb->page); /* 일반 메모리 해제 */ /* NVMe namespace 메모리 해제 */
	bb->page = NULL; /* NVMe bad table 포인터 무효화 */ /* dangling pointer 방지 */
}
EXPORT_SYMBOL_GPL(badblocks_exit);

/*
 * ============================================================================
 * NVMe 관점 핵심 요약
 * ----------------------------------------------------------------------------
 * - 이 파일은 NVMe SSD namespace의 LBA 단위 불량 블록을 관리하는 block layer
 *   범용 엔진이다. 실제 NVMe SQ/CQ, doorbell, CID, PRP/SGL 처리와는 독립적이다.
 * - 상위 I/O 흐름 submit_bio -> blk_mq_submit_bio -> blk_mq_get_request
 *   -> nvme_queue_rq -> nvme_submit_cmd(doorbell)에서, badblocks_check()는
 *   NVMe 명령 제출 전/후 해당 LBA 범위의 미디어 상태를 확인하는 데 쓰인다.
 * - badblocks_set()은 NVMe 미디어 오류 발생 시 unacked/acked 상태로
 *   불량 LBA를 등록하며, ack_all_badblocks()는 상위 메타데이터 동기화 후
 *   unack 항목을 acked로 일괄 전환한다.
 * - 본 파일은 block/bio.c, block/blk-mq.c, block/blk-core.c 등의 block layer
 *   핵심 파일 위에서 동작하며, NVMe 드라이버(drivers/nvme/host/core.c 등)가
 *   이 인터페이스를 사용하여 namespace 미디어 상태를 추적한다 (추정).
 * ============================================================================
 */
