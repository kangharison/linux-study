// SPDX-License-Identifier: GPL-2.0	/* GPL-2.0: NVMe over FC 호스트/타겟 드라이버와 동일 라이선스 하에 커널 빌드됨 */

/*
 * blk-cgroup-fc-appid.c - blkcg에 Fibre Channel(FC) 기반 NVMe-oF 호스트가
 * 사용할 응용 프로그램 식별자(fc_app_id)를 설정하고 조회하는 헬퍼 파일이다.
 *
 * NVMe 스택에서의 위치:
 *   응용/VM 단의 cgroup에 appid를 기록하면, NVMe over FC 호스트 드라이버
 *   (drivers/nvme/host/fc.c)가 blkcg_get_fc_appid()를 통해 이 값을 꺼내
 *   NVMe SSD(타겟)로 전송되는 I/O 요청과 연결한다(추정).
 *
 * 대표 흐름 (추정):
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_fc_fcp_op -> nvme_submit_cmd(doorbell)
 *   이때 rq->bio -> bi_blkg -> blkcg->fc_app_id 를 통해 appid가 CID/SQ/CQ
 *   단위로 매핑될 수 있다(추정).
 *
 * 연계 파일:
 *   - block/blk-cgroup.c: blkcg 구조체 및 cgroup 연동 정의
 *   - block/blk-cgroup.h: fc_app_id 필드 선언 (CONFIG_BLK_CGROUP_FC_APPID)
 *   - drivers/nvme/host/fc.c: nvme_fc_io_getuuid(), fc_appid_store() 등에서 사용
 */

#include "blk-cgroup.h"	/* blkcg, cgroup, css_to_blkcg 등 block cgroup 정의 포함 (추정: include/linux/blk-cgroup.h 경로) */

/*
 * 주요 데이터 구조와 NVMe 동작의 관계
 *
 * struct blkcg:
 *   @fc_app_id[FC_APPID_LEN]: cgroup에 소속된 응용/VM의 식별자.
 *       NVMe over FC 호스트가 I/O 단위로 이 값을 읽어, NVMe SSD 타겟이
 *       어떤 cgroup/VM의 I/O인지 식별하도록 한다(추정).
 *
 * struct bio:
 *   @bi_blkg: bio가 속한 blkcg 그룹.
 *       NVMe 요청 경로에서 rq->bio->bi_blkg->blkcg->fc_app_id 순으로
 *       접근해 fc_app_id를 획득한다.
 *
 * FC_APPID_LEN: fc_app_id 버퍼의 최대 길이. 이 값을 초과하면 -EINVAL.
 */

/*
 * blkcg_set_fc_appid - cgroup의 blkcg에 fc_app_id를 기록한다.
 * @app_id: 설정할 응용 프로그램 식별자
 * @cgrp_id: 대상 cgroup id
 * @app_id_len: app_id 길이
 *
 * 목적:
 *   사용자 공간(sysfs)에서 VM/응용 별 appid를 입력하면, 해당 cgroup의
 *   blkcg->fc_app_id에 복사해 둔다.
 *
 * 호출 경로 (추정):
 *   drivers/nvme/host/fc.c::fc_appid_store ->
 *   blkcg_set_fc_appid(app_id, cgrp_id, app_id_len)
 *
 * NVMe 연결점:
 *   이 값은 이후 blkcg_get_fc_appid() -> nvme_fc_io_getuuid() 경로로
 *   NVMe over FC I/O 요청과 연결되어, NVMe SSD 타겟이 I/O 소속을 식별하는
 *   데 사용될 수 있다(추정).
 */

/**
 * blkcg_set_fc_appid - set the fc_app_id field associted to blkcg
 * @app_id: application identifier
 * @cgrp_id: cgroup id
 * @app_id_len: size of application identifier
 */
int blkcg_set_fc_appid(char *app_id, u64 cgrp_id, size_t app_id_len)	/* (추정) app_id_len이 FC_APPID_LEN보다 크면 NVMe SSD에 복사 불가 */
{
	struct cgroup *cgrp;		/* cgroup ID로 조회한 cgroup 객체 (NVMe QoS 정책과 1:1 매핑되는 단위) */
	struct cgroup_subsys_state *css;	/* io cgroup 하위 시스템 상태: blkcg로 변환하기 위한 핸들 */
	struct blkcg *blkcg;		/* 최종적으로 fc_app_id를 기록할 block cgroup 구조체 */
	int ret  = 0;			/* 반환값 초기화: 성공 시 0, NVMe-oF 호스트에 성공 전달 */

	if (app_id_len > FC_APPID_LEN)	/* (추정) 버퍼 초과 시 NVMe SSD에 전달 불가 */
		return -EINVAL;		/* (추정) 잘못된 appid는 NVMe 명령 생성 전 단계에서 거부, CID/SQ에 반영 안 됨 */

	cgrp = cgroup_get_from_id(cgrp_id);	/* cgrp_id에 해당하는 cgroup 획득 */
	if (IS_ERR(cgrp))		/* 유효하지 않은 cgroup id 처리 */
		return PTR_ERR(cgrp);	/* (추정) cgroup 조회 실패 시 NVMe I/O는 기본(blkg 없음) appid로 진행됨 */
	css = cgroup_get_e_css(cgrp, &io_cgrp_subsys); /* io cgroup 하위 시스템의 css 획득 */
	if (!css) {			/* css가 없으면 blkcg에 접근 불가 */
		ret = -ENOENT;		/* (추정) blkcg 미연결 상태: NVMe SQ/CQ 선택에 appid 미반영 */
		goto out_cgrp_put;	/* cgroup 참조 해제 후 복귀 */
	}
	blkcg = css_to_blkcg(css);		/* css를 blkcg로 변환 */
	/*
	 * There is a slight race condition on setting the appid.
	 * Worst case an I/O may not find the right id.
	 * This is no different from the I/O we let pass while obtaining
	 * the vmid from the fabric.
	 * Adding the overhead of a lock is not necessary.
	 */
	strscpy(blkcg->fc_app_id, app_id, app_id_len); /* app_id를 blkcg->fc_app_id에 복사 (추정: race 허용, lock 미사용) */
	/*
	 * (추정) strscpy 시점에 병렬 NVMe 완료 경로(nvme_fc_io_getuuid)가
	 * blkcg_get_fc_appid()로 읽을 수 있으므로, 일부 CID는 이전/새 appid 중
	 * 하나를 얻을 수 있음. NVMe 타겟의 QoS 분류에 미세한 불일치 허용.
	 */
	css_put(css);			/* css 참조 카운트 감소 */
out_cgrp_put:
	cgroup_put(cgrp);		/* cgroup 참조 카운트 감소 */
	return ret;			/* 0 또는 오류 코드: NVMe-oF 호스트가 sysfs store 결과로 수신 */
}
EXPORT_SYMBOL_GPL(blkcg_set_fc_appid);	/* (추정) nvme-fc 모듈이 모듈 로드 시 이 심볼을 동적으로 연결하여 sysfs store 호출 */

/*
 * blkcg_get_fc_appid - bio가 속한 blkcg의 fc_app_id를 반환한다.
 * @bio: 대상 bio
 *
 * 목적:
 *   NVMe over FC 호스트 드라이버가 I/O 요청(rq->bio)으로부터 어떤
 *   응용/VM의 appid가 연결되어 있는지 조회한다.
 *
 * 호출 경로 (추정):
 *   drivers/nvme/host/fc.c::nvme_fc_io_getuuid(req) ->
 *   blkcg_get_fc_appid(rq->bio)
 *
 * NVMe 연결점:
 *   반환된 appid는 nvme_fc_fcp_op 구조와 연결된 후, NVMe SSD 타겟에
 *   전송되는 SQ 엔트리(CID, PRP/SGL 등)와 함께 사용될 수 있다(추정).
 */

/**
 * blkcg_get_fc_appid - get the fc app identifier associated with a bio
 * @bio: target bio
 *
 * On success return the fc_app_id, on failure return NULL
 */
char *blkcg_get_fc_appid(struct bio *bio)	/* NVMe 요청 경로에서 bio->bi_blkg 유효성 먼저 판단 */
{
	/* blkcg가 없거나 fc_app_id가 비어 있으면 NVMe SSD에 식별자 미전달 */
	if (!bio->bi_blkg || bio->bi_blkg->blkcg->fc_app_id[0] == '\0')
		return NULL;		/* (추정) NULL이면 NVMe SQ 엔트리의 appid 필드가 0/기본값으로 채워짐 */
	return bio->bi_blkg->blkcg->fc_app_id; /* 설정된 fc_app_id 반환 */
	/* (추정) 반환된 문자열은 nvme_fc_fcp_op->{app_id,uuid}에 복사되어
	 * NVMe CID 단위로 타겟에 전달되며, NVMe SSD의 admission control/SQ
	 * 라우팅에 참조될 수 있음 */
}
EXPORT_SYMBOL_GPL(blkcg_get_fc_appid);	/* (추정) drivers/nvme/host/fc.c 모듈이 I/O 발행 시 이 심볼 호출 */

/* NVMe 관점 핵심 요약
 *
 * - 이 파일은 blkcg->fc_app_id를 읽기/쓰기만 한다. 실제 NVMe 명령 생성은
 *   drivers/nvme/host/fc.c -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   경로에서 이루어진다(추정).
 * - fc_app_id는 bio->bi_blkg->blkcg->fc_app_id 순으로 접근되며, NVMe over FC
 *   I/O 요청의 소속 cgroup/VM 식별에 사용된다(추정).
 * - blkcg_set_fc_appid()는 sysfs 기반 설정 경로(fc_appid_store)에서 호출되고,
 *   blkcg_get_fc_appid()는 nvme_fc_io_getuuid()에서 호출된다.
 * - FC_APPID_LEN 초과 식별자는 -EINVAL로 거부되어 NVMe SSD로 잘못된 메타데이터가
 *   전달되는 것을 방지한다(추정).
 */
