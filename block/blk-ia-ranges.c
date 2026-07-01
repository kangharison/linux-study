// SPDX-License-Identifier: GPL-2.0
/*
 *  Block device concurrent positioning ranges.
 *
 *  Copyright (C) 2021 Western Digital Corporation or its Affiliates.
 */

/*
 * =================================================================
 * 파일 상단 요약 (NVMe SSD 관점)
 * =================================================================
 * 이 파일은 block layer의 "독립 접근 범위(Independent Access Ranges, IAR)"를
 * 다룬다. NVMe SSD에서는 하나의 namespace가 물리적으로 여러 그룹의 플래시 다이나
 * 쌍(die/way)으로 구성될 수 있으며, 서로 다른 LBA 범위에 대해 병렬로 입출력을
 * 처리할 수 있다. 이 범위 정보를 커널이 sysfs에 노출하고, upper layer(예:
 * multipath, device-mapper, RAID)나 사용자 공간 스케줄러가 NVMe queue/PRP/SGL
 * 배치 전략을 세울 때 참고할 수 있게 한다.
 *
 * 호출 맥락 예시:
 * nvme_revalidate_disk -> disk_set_independent_access_ranges -> disk_check_ia_ranges
 *   -> disk_register_independent_access_ranges (sysfs 노출)
 * blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *
 * 관련 파일: block/blk-mq.c(요청 할당/큐잉), block/blk-sysfs.c(큐 속성),
 *          drivers/nvme/host/core.c(디스커버리 및 등록)
 * =================================================================
 */

#include <linux/kernel.h>	/* 커널 기본 매크로; NVMe doorbell/register 접근 시
				 * 쓰이는 barrier/atomic 정책의 상위 헤더
				 * 경로 중 하나 (추정) */
#include <linux/blkdev.h>	/* block layer 핵심: struct request_queue,
				 * gendisk, queue depth, BLK_MQ_F_* 플래그 등
				 * NVMe SQ/CQ 매핑의 출발점 */
#include <linux/slab.h>		/* kzalloc_node/kfree: NVMe range 메타데이터를
				 * queue 노드에 맞춰 할당하여 NUMA locality 확보 */
#include <linux/init.h>

#include "blk.h"		/* block layer 내부 함수 선언; blk_queue_registered()
				 * 등 queue 상태 전이 함수 포함 */

/*
 * sysfs에서 "sector" 속성을 읽을 때 호출된다.
 * NVMe에서는 이 값이 namespace LBA 단위로 해석되며, CQ(completion queue)로
 * 반환되는 LBA와 동일한 주소 공간을 가진다.
 */
static ssize_t
blk_ia_range_sector_show(struct blk_independent_access_range *iar,
			 char *buf)
{
	/* buf는 sysfs 페이지 크기 버퍼; 이 결과가 userspace scheduler가 SQ 선택
	 * 시 사용하는 LBA 시작점이 된다 (추정). */
	return sprintf(buf, "%llu\n", iar->sector);
}

/*
 * sysfs에서 "nr_sectors" 속성을 읽을 때 호출된다.
 * NVMe sector 크기(기본 512B, 또는 LBAF에 따른 형식)를 곱하면 바이트 단위
 * 범위가 되며, PRP/SGL 리스트를 구성할 때 최대 전송 범위와 연결된다.
 */
static ssize_t
blk_ia_range_nr_sectors_show(struct blk_independent_access_range *iar,
			     char *buf)
{
	/* iar->nr_sectors * logical_block_size 바이트가 한 concurrency group의
	 * 연속 영역; NVMe command의 NLB(number of logical blocks) 상한과 간접
	 * 연결될 수 있다 (추정). */
	return sprintf(buf, "%llu\n", iar->nr_sectors);
}

/*
 * sysfs 항목 하나를 나타낸다. attr은 sysfs_ops에서 식별하는 이름/모드이며,
 * show 콜백이 실제 NVMe LBA 범위 값을 사용자 공간에 반환한다.
 */
struct blk_ia_range_sysfs_entry {
	struct attribute attr;		/* "sector" 또는 "nr_sectors" 이름/권한 */
	ssize_t (*show)(struct blk_independent_access_range *iar, char *buf);
};

static const struct blk_ia_range_sysfs_entry blk_ia_range_sector_entry = {
	.attr = { .name = "sector", .mode = 0444 },
	.show = blk_ia_range_sector_show,
};

static const struct blk_ia_range_sysfs_entry blk_ia_range_nr_sectors_entry = {
	.attr = { .name = "nr_sectors", .mode = 0444 },
	.show = blk_ia_range_nr_sectors_show,
};

static const struct attribute *const blk_ia_range_attrs[] = {
	&blk_ia_range_sector_entry.attr,	/* -> sysfs read -> blk_ia_range_sector_show
						 * -> NVMe LBA 시작점 */
	&blk_ia_range_nr_sectors_entry.attr,	/* -> sysfs read -> range 크기,
						 * PRP/SGL 최대 전송 영역
						 * 추정 */
	NULL,
};
ATTRIBUTE_GROUPS(blk_ia_range);

/*
 * blk_ia_range_sysfs_show - sysfs read 경로의 진입점
 * @kobj: independent access range 항목의 kobject
 * @attr: 읽으려는 속성("sector" 또는 "nr_sectors")
 * @buf : 사용자 공간으로 복사할 버퍼
 *
 * 목적:
 *   sysfs_ops->show가 호출될 때, container_of()로 연결된
 *   struct blk_independent_access_range의 sector/nr_sectors 값을 반환한다.
 *
 * 호출 경로 (NVMe 연결):
 *   userspace read("/sys/block/nvmeXnY/queue/independent_access_ranges/N/sector")
 *   -> kernfs_fop_read -> blk_ia_range_sysfs_show
 *   -> blk_ia_range_sector_show -> sprintf(iar->sector)
 *
 * NVMe 연결점:
 *   이 sysfs 트리는 disk_register_independent_access_ranges()에서 등록되며,
 *   NVMe 드라이버가 revalidate 단계에서 채우는 LBA 범위 정보가 노출된다.
 */
static ssize_t blk_ia_range_sysfs_show(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	struct blk_ia_range_sysfs_entry *entry =
		container_of(attr, struct blk_ia_range_sysfs_entry, attr);
	/* attr 이름("sector"/"nr_sectors")을 entry로 역참조; 이 이름이 userspace
	 * 에서 NVMe queue depth 분산 전략을 세울 때 읽히는 키가 된다. */
	struct blk_independent_access_range *iar =
		container_of(kobj, struct blk_independent_access_range, kobj);
	/* kobj -> iar 역참조; iar는 NVMe namespace 내 특정 concurrency group의
	 * LBA 범위를 나타낸다. */

	return entry->show(iar, buf);
	/* 반환값은 sysfs에 복사될 바이트 수; userspace는 이 값으로 SQ/CQ별 LBA
	 * affinity를 계산할 수 있다 (추정). */
}

static const struct sysfs_ops blk_ia_range_sysfs_ops = {
	.show	= blk_ia_range_sysfs_show,
};

/*
 * Independent access range entries are not freed individually, but alltogether
 * with struct blk_independent_access_ranges and its array of ranges. Since
 * kobject_add() takes a reference on the parent kobject contained in
 * struct blk_independent_access_ranges, the array of independent access range
 * entries cannot be freed until kobject_del() is called for all entries.
 * So we do not need to do anything here, but still need this no-op release
 * operation to avoid complaints from the kobject code.
 */
/*
 * 개별 range kobject release는 아무 일도 하지 않는다.
 * 실제 메모리 해제는 상위 struct blk_independent_access_ranges의 release
 * (blk_ia_ranges_sysfs_release)에서 한 번에 수행된다.
 */
static void blk_ia_range_sysfs_nop_release(struct kobject *kobj)
{
	/* NVMe tear-down 경로에서 kobject reference가 0이 되어도, 메모리 해제는
	 * 상위 iars release로 지연된다; 이는 NVMe controller reset/namespace
	 * 제거 시 race를 완화하는 패턴과 유사하다 (추정). */
}

static const struct kobj_type blk_ia_range_ktype = {
	.sysfs_ops	= &blk_ia_range_sysfs_ops,	/* sysfs read -> show callback */
	.default_groups	= blk_ia_range_groups,		/* "sector", "nr_sectors" */
	.release	= blk_ia_range_sysfs_nop_release,
};

/*
 * This will be executed only after all independent access range entries are
 * removed with kobject_del(), at which point, it is safe to free everything,
 * including the array of ranges.
 */
/*
 * 상위 kobject의 release: 모든 하위 range kobject가 kobject_del()된 뒤에
 * 호출되므로, 이 시점에서 ia_range[] 배열 전체를 포함한 iars를 kfree()로
 * 해제해도 안전하다.
 */
static void blk_ia_ranges_sysfs_release(struct kobject *kobj)
{
	struct blk_independent_access_ranges *iars =
		container_of(kobj, struct blk_independent_access_ranges, kobj);
	/* 상위 kobj를 통해 NVMe range 집합 컨테이너 획득; 이 컨테이너는 Identify
	 * 기반 병렬 그룹 정보를 모두 담고 있다. */

	kfree(iars);
	/* 마지막 reference 소멸 시 메모리 반납; NVMe controller detach/namespace
	 * 삭제 후에도 sysfs 사용자가 남아있다면 지연될 수 있다. */
}

static const struct kobj_type blk_ia_ranges_ktype = {
	.release	= blk_ia_ranges_sysfs_release,
};

/**
 * disk_register_independent_access_ranges - register with sysfs a set of
 *		independent access ranges
 * @disk:	Target disk
 *
 * Register with sysfs a set of independent access ranges for @disk.
 */
/*
 * disk_register_independent_access_ranges - IAR sysfs 트리 등록
 * @disk: 대상 gendisk (NVMe namespace에 해당)
 *
 * 목적:
 *   struct blk_independent_access_ranges에 저장된 각 LBA 범위를
 *   /sys/block/<disk>/queue/independent_access_ranges/<idx>/{sector,nr_sectors}
 *   로 노출한다.
 *
 * 호출 경로:
 *   nvme_revalidate_disk (또는 유사한 revalidate 콜백)
 *     -> disk_set_independent_access_ranges
 *       -> (변경 감지 및 유효성 검사 통과 후)
 *       -> disk_register_independent_access_ranges
 *
 * NVMe 연결점:
 *   NVMe Identify Namespace/Controller 데이터에서 도출한 range 수(nr_ia_ranges),
 *   시작 LBA(sector), 크기(nr_sectors)가 사용자 공간으로 드러난다.
 *   upper layer는 이를 참고해 여러 CQ(completion queue) 또는 SQ(submission queue)
 *   간의 LBA 병렬성을 판단할 수 있다.
 *
 * 주의:
 *   호출자는 q->sysfs_lock을 잡아야 한다.
 */
int disk_register_independent_access_ranges(struct gendisk *disk)
{
	struct blk_independent_access_ranges *iars = disk->ia_ranges;
	/* disk->ia_ranges: NVMe revalidate에서 채워진 병렬 그룹 테이블 */
	struct request_queue *q = disk->queue;
	/* q: NVMe namespace의 request_queue; queue depth, hctx 수, SCHED flags 등이
	 * nvme_queue_rq -> nvme_submit_cmd doorbell 경로에 전달된다. */
	int i, ret;

	lockdep_assert_held(&q->sysfs_lock);
	/* sysfs_lock은 request_queue 등록/해제 시 NVMe 드라이버와 sysfs 사이의
	 * range 정보 일관성을 보장; RCU read-side가 아닌 mutex 기반이므로
	 * write-side에서만 수정된다. */

	if (!iars)
		return 0;
	/* iars가 NULL이면 NVMe가 병렬 range를 보고하지 않은 것이며, 단일 SQ/CQ
	 * affinity로 평범하게 동작한다. */

	/*
	 * At this point, iars is the new set of sector access ranges that needs
	 * to be registered with sysfs.
	 */
	/* 이 시점에서 iars는 sysfs에 새로 등록해야 할 LBA 접근 범위 집합이다. */
	WARN_ON(iars->sysfs_registered);
	/* 이미 등록된 상태에서 재등록 시도는 버그; NVMe revalidate 중에도
	 * disk_unregister_independent_access_ranges()로 먼저 정리한 뒤 호출. */
	ret = kobject_init_and_add(&iars->kobj, &blk_ia_ranges_ktype,
				   &disk->queue_kobj, "%s",
				   "independent_access_ranges");
	if (ret) {
		disk->ia_ranges = NULL;
		/* 등록 실패: NVMe range 정보가 userspace에 노출되지 않도록
		 * disk에서 끊는다; 이후 I/O는 병렬성 힌트 없이 진행될 수 있다. */
		kobject_put(&iars->kobj);
		/* 마지막 reference 해제; release에서 메모리 반납 여부 결정. */
		return ret;
		/* 오류 반환 -> NVMe revalidate 경로에서 caller가 처리; 일반적으로
		 * controller reset 또는 namespace disable 후속 조치로 이어질 수
		 * 있다 (추정). */
	}

	/* 각 range마다 하위 kobject를 만들고 sysfs에 속성을 노출한다. */
	for (i = 0; i < iars->nr_ia_ranges; i++) {
		/* i: NVMe concurrency group 인덱스; 이 인덱스는 userspace가
		 * SQ/CQ 번호와 매핑할 때 참고할 수 있다 (추정). */
		ret = kobject_init_and_add(&iars->ia_range[i].kobj,
					   &blk_ia_range_ktype, &iars->kobj,
					   "%d", i);
		if (ret) {
			/* 등록 실패 시 이미 추가한 하위 kobject를 역순으로 제거한다. */
			while (--i >= 0)
				kobject_del(&iars->ia_range[i].kobj);
			/* 부분적으로 추가된 range를 되돌려 sysfs 일관성 유지;
			 * NVMe 입장에서는 일부 그룹만 노출되면 잘못된 queue
			 * affinity 결정을 막기 위함. */
			kobject_del(&iars->kobj);
			kobject_put(&iars->kobj);
			return ret;
			/* 실패 시 NVMe 드라이버는 새 range 집합을 버리고 이전
			 * 상태(또는 NULL)로 남는다. */
		}
	}

	iars->sysfs_registered = true;
	/* sysfs 노출 완료; 이 시점부터 userspace/upper layer는 IAR을 읽어
	 * NVMe SQ/CQ 배치나 CID/tag 분산 전략을 갱신할 수 있다 (추정). */

	return 0;
}

/*
 * disk_unregister_independent_access_ranges - IAR sysfs 트리 등록 해제
 * @disk: 대상 gendisk
 *
 * 목적:
 *   sysfs에서 independent_access_ranges 디렉터리와 하위 항목들을 제거한다.
 *   이미 등록된 상태이면 kobject_del()/put()으로 정리하고, 등록되지 않은
 *   임시 상태이면 직접 메모리를 해제한다.
 *
 * 호출 경로:
 *   disk_set_independent_access_ranges(변경/해제 시)
 *     -> disk_unregister_independent_access_ranges
 *   또는 드라이버의 tear-down 경로에서 호출될 수 있다.
 *
 * NVMe 연결점:
 *   NVMe namespace가 제거되거나 revalidate로 인해 range 정보가 바뀔 때,
 *   기존 sysfs 정보를 깨끗이 정리해야 새로운 PRP/SGL 기반 병렬성 정보를
 *   안전하게 재등록할 수 있다.
 */
void disk_unregister_independent_access_ranges(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	struct blk_independent_access_ranges *iars = disk->ia_ranges;
	int i;

	lockdep_assert_held(&q->sysfs_lock);
	/* sysfs write-side와 NVMe revalidate/teardown 간 동기화. */

	if (!iars)
		return;
	/* 등록할 IAR이 없으면 정리할 것도 없음; NVMe namespace가 단일 그룹
	 * 디바이스인 경우. */

	if (iars->sysfs_registered) {
		/* 하위 kobject부터 순서대로 삭제한 뒤 상위 kobject를 제거한다. */
		for (i = 0; i < iars->nr_ia_ranges; i++)
			kobject_del(&iars->ia_range[i].kobj);
		/* 각 range sysfs 항목 삭제; userspace가 더 이상 오래된 NVMe
		 * group 정보로 SQ/CQ affinity를 계산하지 못하게 차단. */
		kobject_del(&iars->kobj);
		kobject_put(&iars->kobj);
		/* 마지막 put -> release -> kfree(iars); NVMe tear-down 시
		 * controller가 완전히 정지된 뒤에도 안전하게 메모리 반납. */
	} else {
		/* sysfs에 노출되지 않은 임시 객체는 직접 해제한다. */
		kfree(iars);
		/* NVMe revalidate 중 검증/변경 단계에서 버려진 중간 객체. */
	}

	disk->ia_ranges = NULL;
	/* disk에서 분리; 이후 nvme_queue_rq 등 I/O 경로는 더 이상 이 구조체를
	 * 직접 보지 않지만, 사용자 공간이 갱신된 정보를 재읽을 때까지는 이전
	 * 값을 캐시할 수 있다 (추정). */
}

/*
 * disk_find_ia_range - 주어진 sector가 속한 IAR를 검색
 * @iars: independent access ranges 집합
 * @sector: 찾을 LBA (NVMe namespace 기준 논리 블록 주소)
 *
 * 반환:
 *   sector가 포함된 struct blk_independent_access_range 포인터, 없으면 NULL.
 *
 * NVMe 연결점:
 *   NVMe I/O가 들어온 LBA가 어느 독립 병렬 그룹에 속하는지 빠르게 파악할 때
 *   사용한다. 이는 SQ 선택, CQ 분산, 또는 PRP/SGL 매핑을 그룹별로 최적화하는
 *   데 참고할 수 있다.
 */
static struct blk_independent_access_range *
disk_find_ia_range(struct blk_independent_access_ranges *iars,
		  sector_t sector)
{
	struct blk_independent_access_range *iar;
	int i;

	for (i = 0; i < iars->nr_ia_ranges; i++) {
		/* iars->nr_ia_ranges: NVMe Identify에서 보고한 concurrency group
		 * 수; hctx 개수나 SQ/CQ 쌍 수와 대응될 수 있다 (추정). */
		iar = &iars->ia_range[i];
		/* iar: 특정 NVMe 병렬 그룹의 [sector, sector+nr_sectors) */
		if (sector >= iar->sector &&
		    sector < iar->sector + iar->nr_sectors)
			return iar;
		/* LBA가 범위 내에 있으면 해당 그룹 반환; NVMe SQ/CQ affinity
		 * 결정의 입력값. */
	}

	return NULL;
	/* 범위를 벗어난 LBA는 NVMe namespace 용량 밖이거나 잘못된 range 정보. */
}

/*
 * disk_check_ia_ranges - IAR 집합의 무결성 검사 및 정렬
 * @disk: 대상 gendisk
 * @iars: 검사할 independent access ranges
 *
 * 목적:
 *   (1) 모든 sector가 정확히 하나의 range에 속하고 겹치지 않는지 확인
 *   (2) hole이 없이 LBA 순서대로 연속되는지 확인
 *   (3) 정렬이 필요하면 range 배열을 오름차순 LBA로 재배열
 *   (4) 전체 range 합계가 디스크 용량과 일치하는지 확인
 *
 * 반환: true(유효), false(무효)
 *
 * NVMe 연결점:
 *   NVMe Identify Namespace/Controller 데이터에서 보고된 range 정보가 커널 낮은 block layer에서
 *   신뢰할 수 있는지 검증한다. 잘못된 range는 병렬 큐잉 최적화에 혼란을
 *   주거나, CID(command identifier)를 SQ에 부적절히 분배하는 문제를 일으킬 수
 *   있다.
 */
static bool disk_check_ia_ranges(struct gendisk *disk,
				struct blk_independent_access_ranges *iars)
{
	struct blk_independent_access_range *iar, *tmp;
	sector_t capacity = get_capacity(disk);
	/* capacity: NVMe namespace 전체 sector 수; range 합계와 비교. */
	sector_t sector = 0;
	/* 현재 검사 중인 LBA; 0부터 시작하여 연속성을 확인. */
	int i;

	if (WARN_ON_ONCE(!iars->nr_ia_ranges))
		return false;
	/* NVMe가 0개의 range를 보고한 것은 caller의 버그 또는 잘못된 Identify
	 * 데이터를 의미; 이 경우 병렬성 힌트를 폐기. */

	/*
	 * While sorting the ranges in increasing LBA order, check that the
	 * ranges do not overlap, that there are no sector holes and that all
	 * sectors belong to one range.
	 */
	/*
	 * LBA 오름차순으로 정렬하면서 다음을 동시에 검사한다:
	 *  - range 간 overlap이 없는지
	 *  - sector hole이 없는지
	 *  - 모든 sector가 정확히 하나의 range에 속하는지
	 */
	for (i = 0; i < iars->nr_ia_ranges; i++) {
		/* i번째 위치에 오름차순으로 올 range를 찾는 selection sort 루프;
		 * NVMe controller가 보고한 순서가 LBA 순서와 다를 수 있다. */
		tmp = disk_find_ia_range(iars, sector);
		if (!tmp || tmp->sector != sector) {
			pr_warn("Invalid non-contiguous independent access ranges\n");
			return false;
			/* LBA 공간에 hole/overlap이 있으면 NVMe queue affinity
			 * 분산이 잘못될 수 있으므로 폐기. */
		}

		iar = &iars->ia_range[i];
		if (tmp != iar) {
			/* 현재 위치에 오름차순으로 올 range가 아니면 교환한다. */
			swap(iar->sector, tmp->sector);
			swap(iar->nr_sectors, tmp->nr_sectors);
			/* 교환 후 iars->ia_range[]는 LBA 오름차순; userspace가
			 * 인덱스 i를 SQ/CQ 번호로 간주할 때 일관된 LBA->queue
			 * 매핑을 제공한다 (추정). */
		}

		/* 다음 연속 LBA로 이동: 현재 range 끝 직후. */
		sector += iar->nr_sectors;
		/* 모든 range의 nr_sectors 누적값이 capacity와 같아야 한다. */
	}

	if (sector != capacity) {
		pr_warn("Independent access ranges do not match disk capacity\n");
		return false;
		/* NVMe namespace 용량과 range 총합이 불일치하면 Identify 정보가
		 * 신뢰할 수 없음; 병렬 그룹 힌트 사용 불가. */
	}

	return true;
	/* 검증 통과: NVMe queue/CQ affinity를 userspace에 안전하게 노출. */
}

/*
 * disk_ia_ranges_changed - 기존 IAR과 새 IAR이 다른지 비교
 * @disk: 대상 gendisk
 * @new : 새로 제안된 independent access ranges
 *
 * 반환: true(변경됨 또는 기존 없음), false(동일)
 *
 * NVMe 연결점:
 *   NVMe revalidate(예: namespace 형식 변경, 용량 변경, 멀티그룹 구성 변경)
 *   후에 range 정보가 실제로 달라졌을 때만 sysfs 등록/해제 오버헤드를
 *   피하기 위해 사용한다.
 */
static bool disk_ia_ranges_changed(struct gendisk *disk,
				   struct blk_independent_access_ranges *new)
{
	struct blk_independent_access_ranges *old = disk->ia_ranges;
	int i;

	if (!old)
		return true;
	/* 기존 range가 없으면 무조건 변경으로 처리; NVMe namespace가 처음
	 * 등록되거나 이전에 병렬 그룹을 보고하지 않은 경우. */

	if (old->nr_ia_ranges != new->nr_ia_ranges)
		return true;
	/* 병렬 그룹 수가 바뀌면 SQ/CQ 쌍 수나 hctx 매핑에 영향을 줄 수 있음;
	 * NVMe revalidate 후 새로 등록 필요. */

	for (i = 0; i < old->nr_ia_ranges; i++) {
		if (new->ia_range[i].sector != old->ia_range[i].sector ||
		    new->ia_range[i].nr_sectors != old->ia_range[i].nr_sectors)
			return true;
		/* LBA 경계가 바뀌면 NVMe queue affinity가 달라지므로 sysfs를
		 * 갱신해야 userspace/upper layer가 올바른 SQ 선택을 한다. */
	}

	return false;
	/* 동일하면 기존 sysfs 항목을 재사용; NVMe revalidate overhead 감소. */
}

/**
 * disk_alloc_independent_access_ranges - Allocate an independent access ranges
 *                                        data structure
 * @disk:		target disk
 * @nr_ia_ranges:	Number of independent access ranges
 *
 * Allocate a struct blk_independent_access_ranges structure with @nr_ia_ranges
 * access range descriptors.
 */
/*
 * disk_alloc_independent_access_ranges - IAR 컨테이너 할당
 * @disk:          대상 gendisk
 * @nr_ia_ranges:  독립 접근 범위 개수 (NVMe Identify에서 보고한 그룹 수)
 *
 * 목적:
 *   struct blk_independent_access_ranges와 뒤따르는 ia_range[] 배열을 한 번의
 *   할당으로 만든다. NUMA 노드는 disk->queue->node에 맞춘다.
 *
 * NVMe 연결점:
 *   NVMe 드라이버가 Identify Namespace/Controller로부터 concurrency group 수를
 *   파악한 뒤, 그 개수만큼 range 슬롯을 할당할 때 사용한다.
 */
struct blk_independent_access_ranges *
disk_alloc_independent_access_ranges(struct gendisk *disk, int nr_ia_ranges)
{
	struct blk_independent_access_ranges *iars;

	iars = kzalloc_node(struct_size(iars, ia_range, nr_ia_ranges),
			    GFP_KERNEL, disk->queue->node);
	/* queue node에 맞춘 zeroed 할당; NVMe SQ/CQ memory, PRP list와 마찬가지로
	 * NUMA locality를 고려한 메모리 배치. */
	if (iars)
		iars->nr_ia_ranges = nr_ia_ranges;
	/* 할당 성공 시 group 수 기록; 이 값은 이후 for-loop 반복 횟수 및
	 * sysfs 항목 개수의 상한이 된다. */
	return iars;
}
EXPORT_SYMBOL_GPL(disk_alloc_independent_access_ranges);

/**
 * disk_set_independent_access_ranges - Set a disk independent access ranges
 * @disk:	target disk
 * @iars:	independent access ranges structure
 *
 * Set the independent access ranges information of the request queue
 * of @disk to @iars. If @iars is NULL and the independent access ranges
 * structure already set is cleared. If there are no differences between
 * @iars and the independent access ranges structure already set, @iars
 * is freed.
 */
/*
 * disk_set_independent_access_ranges - 디스크에 IAR 설정
 * @disk: 대상 gendisk
 * @iars: 설정할 independent access ranges (NULL이면 기존 설정 제거)
 *
 * 목적:
 *   NVMe 드라이버 등이 새로 획득한 range 정보를 gendisk에 연결하고,
 *   필요시 sysfs에 등록/재등록한다.
 *
 * 호출 경로 (NVMe 기준 예시):
 *   nvme_revalidate_disk
 *     -> nvme_update_ns_info (또는 유사)
 *       -> disk_set_independent_access_ranges
 *         -> disk_check_ia_ranges      (무결성/정렬 검사)
 *         -> disk_ia_ranges_changed    (변경 여부 확인)
 *         -> disk_unregister_independent_access_ranges (기존 제거)
 *         -> disk_register_independent_access_ranges   (새 것 등록)
 *
 * NVMe 연결점:
 *   revalidate 도중에는 q->sysfs_lock 아래에서 동작하며, 이미 request_queue가
 *   등록된 경우엔 sysfs 노출까지 갱신한다. NVMe queue_rq 경로
 *   (blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd)
 *   는 이 구조체를 직접 참조하지는 않지만, userspace/upper layer가 range
 *   정보를 읽어 SQ/CQ 배치나 doorbell 분산 전략을 세울 수 있게 한다.
 */
void disk_set_independent_access_ranges(struct gendisk *disk,
				struct blk_independent_access_ranges *iars)
{
	struct request_queue *q = disk->queue;
	/* q: NVMe namespace의 request_queue; QUEUE_FLAG_* 상태와 hctx가
	 * NVMe SQ/CQ doorbell 경로에 연결된다. */

	mutex_lock(&q->sysfs_lock);
	/* sysfs write-side 잠금; NVMe revalidate 중에 userspace가 동시에
	 * independent_access_ranges를 읽는 것을 배제하여 일관된 뷰 제공. */
	if (iars && !disk_check_ia_ranges(disk, iars)) {
		/* 무결성 검사 실패: 잘못된 NVMe range 정보이므로 폐기한다. */
		kfree(iars);
		/* 잘못된 Identify 데이터로 인한 메모리 반납; 이 경우 NVMe는
		 * 병렬 그룹 정보 없이 평범한 싱글 affinity로 동작. */
		iars = NULL;
	}
	if (iars && !disk_ia_ranges_changed(disk, iars)) {
		/* 변경이 없으면 새 구조체를 할당 해제하고 기존 설정을 유지한다. */
		kfree(iars);
		/* NVMe revalidate가 range에 영향을 주지 않았을 때 불필요한
		 * sysfs 등록/해제 오버헤드 제거. */
		goto unlock;
	}

	/*
	 * This may be called for a registered queue. E.g. during a device
	 * revalidation. If that is the case, we need to unregister the old
	 * set of independent access ranges and register the new set. If the
	 * queue is not registered, registration of the device request queue
	 * will register the independent access ranges.
	 */
	/*
	 * 이미 등록된 request_queue에서 revalidation 중에 호출될 수 있다.
	 * 이 경우 기존 IAR을 해제하고 새로 등록해야 한다.
	 * queue가 아직 등록되지 않았다면, 추후 request_queue 등록 시 IAR도
	 * 함께 등록된다.
	 */
	disk_unregister_independent_access_ranges(disk);
	/* 기존 sysfs 항목 제거; NVMe namespace format 변경 등으로 range가
	 * 달라지기 전에 userspace가 오래된 값을 보지 않도록 차단. */
	disk->ia_ranges = iars;
	/* 새로운 NVMe 병렬 그룹 테이블을 gendisk에 연결. */
	if (blk_queue_registered(q))
		disk_register_independent_access_ranges(disk);
	/* queue가 이미 등록되어 있으면 즉시 sysfs에 새 range 노출;
	 * blk_queue_registered()는 QUEUE_FLAG_REGISTERED 비트를 검사하는
	 * 매크로이며, 이 플래그는 request_queue 등록/해제 상태 전이를
	 * 나타낸다. NVMe에서 대응하는 개념은 controller state(예: NVME_CTRL_LIVE)
	 * 와 유사하지만, 이 파일의 직접적인 상태는 아니다 (추정). */
unlock:
	mutex_unlock(&q->sysfs_lock);
}
EXPORT_SYMBOL_GPL(disk_set_independent_access_ranges);

/*
 * =========================================================================
 * NVMe 관점 핵심 요약
 * =========================================================================
 * - 이 파일은 NVMe SSD의 물리적/논리적 병렬 그룹(독립 접근 범위, IAR)을 커널
 *   block layer에서 sysfs로 노출하는 인프라를 제공한다.
 *
 * - NVMe 드라이버는 Identify 정보에서 파악한 range 수(nr_ia_ranges)와 각
 *   범위의 시작 sector, 크기(nr_sectors)를 disk_alloc_independent_access_ranges()
 *   와 disk_set_independent_access_ranges()를 통해 등록한다.
 *
 * - disk_check_ia_ranges()는 NVMe 컨트롤러가 보고한 range가 LBA 공간을
 *   중복/누락 없이 완전히 커버하는지 검증하여, 잘못된 병렬성 정보가 upper
 *   layer로 전파되지 않도록 한다.
 *
 * - NVMe 입출력 처리 경로(blk_mq_submit_bio -> blk_mq_get_request ->
 *   nvme_queue_rq -> nvme_submit_cmd(doorbell))는 이 파일을 직접 사용하지
 *   않지만, 사용자 공간/upper layer가 IAR을 읽어 SQ/CQ 배치, CID 할당,
 *   PRP/SGL 기반 queue depth 분산 전략을 최적화할 수 있게 한다 (추정).
 *
 * - 관련 파일: block/blk-mq.c(요청 큐잉), block/blk-sysfs.c(큐 속성),
 *   drivers/nvme/host/core.c(디스커버리 및 namespace 재검증).
 * =========================================================================
 */
