/* SPDX-License-Identifier: GPL-2.0 */

/*
 * block/blk-ioprio.h
 *
 * 이 파일은 블록 계층의 I/O 우선순위(ioprio) 인터페이스를 정의하는 헤더 파일이다.
 * cgroup 기반 I/O 우선순위(BLK_CGROUP_IOPRIO)가 설정되면, 각 bio에 우선순위
 * 힌트를 부착하여 이후 blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq
 * -> nvme_submit_cmd(doorbell) 경로로 전달된다. NVMe 입장에서는 이 우선순위가
 * WRR(Weighted Round Robin) Arbitration 및 AC(Arbitration Configuration)에
 * 반영될 수 있는 힌트로 해석될 수 있다.
 */

#ifndef _BLK_IOPRIO_H_		/* NVMe SQ/CID 배정 전 bio 우선순위 태깅 인터페이스의 중복 포함 방지 */
#define _BLK_IOPRIO_H_		/* guard 정의: 이후 request_queue/bio 전방 선언 및 blkcg_set_ioprio() 선언이 NVMe 경로에 노출됨 */

#include <linux/kconfig.h>	/* CONFIG_BLK_CGROUP_IOPRIO 컴파일타임 분기: 꺼지면 NVMe WRR Arbitration 클래스 힌트는 기본값으로 고정됨 */

/*
 * request_queue: NVMe 하드웨어 큐(nvme_queue / blk_mq_hw_ctx)와 연결된
 *                블록 레벨 큐. 실제 우선순위 적용은 이 큐를 통해 처리된다.
 * bio: 사용자 I/O 요청의 기본 단위. blkcg_set_ioprio()는 이 bio에
 *      cgroup의 ioprio 값을 기록하여 후속 NVMe 명령 CID/SQ 배정에 참고되게 한다.
 */
struct request_queue;		/* request_queue -> blk_mq_hw_ctx -> nvme_queue: 우선순위 힌트가 최종적으로 NVMe SQ 선택에 반영될 수 있는 블록 레벨 큐 (추정) */
struct bio;			/* bio->bi_ioprio 캐리어: blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq 명령 조립 경로로 전달 */

#ifdef CONFIG_BLK_CGROUP_IOPRIO	/* blk-cgroup ioprio 기능이 켜진 경우에만 NVMe WRR Arbitration 힌트 태깅 활성화 */
/*
 * blkcg_set_ioprio - cgroup의 I/O 우선순위를 bio에 설정한다.
 *
 * 목적:
 *   bio가 속한 blk-cgroup의 ioprio 값을 읽어 bio->bi_ioprio에 저장한다.
 *
 * 호출 경로 (NVMe 관점):
 *   submit_bio_noacct -> blk_mq_submit_bio -> blkcg_set_ioprio(bio)
 *   이후 bio는 blk_mq_get_request를 거쳐 request로 변환되고,
 *   nvme_queue_rq에서 nvme_submit_cmd(doorbell)로 NVMe SQ/CID에 배정된다.
 *
 * NVMe 연결점:
 *   - NVMe 컨트롤러가 WRR(Weighted Round Robin) Arbitration을 지원하면,
 *     이 ioprio 값이 URGENT/HIGH/MEDIUM/LOW 클래스 중 하나로 매핑될 수 있다.
 *   - 정확한 매핑은 nvme 드라이버 및 컨트롤러의 Arbitration Burst 설정에
 *     의존한다 (추정).
 */
void blkcg_set_ioprio(struct bio *bio);	/* bio에 우선순위 기록 -> blk_mq_get_request()에서 request로 승격 -> nvme_queue_rq()에서 CID/tag 및 SQ 배정 시 참고 가능 (추정) */
#else
/*
 * CONFIG_BLK_CGROUP_IOPRIO가 비활성화된 경우, 컴파일 타임에 우선순위 설정
 * 함수를 빈 인라인 함수로 대체하여 호출 부분의 오버헤드를 제거한다.
 * NVMe 스택에서는 이 경우 모든 bio가 동일한 기본 우선순위로 처리된다.
 */
static inline void blkcg_set_ioprio(struct bio *bio)	/* NVMe 경로에서 우선순위 힌트를 받지 않으므로 모든 I/O는 컨트롤러 기본 Arbitration 정책을 따름 */
{
}				/* 의도적 no-op: doorbell 타이밍이나 SQ/CQ 선택에 영향을 주지 않으며 NVMe queue depth/CID 할당도 변화 없음 */
#endif				/* CONFIG_BLK_CGROUP_IOPRIO: NVMe WRR 힌트 사용 여부를 결정하는 컴파일 스위치 */

/*
 * NVMe 관점 핵심 요약
 *
 * - block/blk-ioprio.h는 blk-cgroup 우선순위를 bio에 태깅하는 진입점이다.
 * - 태깅된 ioprio는 blk_mq_get_request를 통해 request 구조체로 전파되고,
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로에서 NVMe SQ/CID
 *   배정 및 WRR Arbitration에 참고될 수 있다 (추정).
 * - 이 헤더는 block/blk-cgroup.h, include/linux/ioprio.h 등의 상위 정책
 *   인프라와 연결되며, 실제 NVMe 큐 매핑은 drivers/nvme/host/pci.c 등에서
 *   처리된다.
 * - CONFIG_BLK_CGROUP_IOPRIO가 꺼져 있으면 우선순위 힌트는 무시되며,
 *   NVMe 컨트롤러의 기본 Arbitration 정책이 적용된다.
 */

#endif /* _BLK_IOPRIO_H_ */	/* 헤더 guard 종료: NVMe bio -> request -> SQ/CID 우선순위 흐름 관련 선언 끝 */
