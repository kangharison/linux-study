// SPDX-License-Identifier: GPL-2.0	/* [한국어] GPL-2.0: 리눅스 블록 계층(block/) 공통 라이선스 그대로 유지 */

/*
 * [한국어 설명] blkcg(block cgroup)에 FC(Fibre Channel) NVMe-oF용 app_id 문자열을
 * 기록/조회하는 두 개의 export 함수만 담은 작은 헬퍼 파일이다 (blk-cgroup-fc-appid.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 struct blkcg 에 CONFIG_BLK_CGROUP_FC_APPID 로 조건부 컴파일되는
 * fc_app_id[FC_APPID_LEN] 필드(block/blk-cgroup.h:640)를 "쓰는" 함수
 * blkcg_set_fc_appid() 와 "읽는" 함수 blkcg_get_fc_appid() 두 개만 제공한다.
 * VM/컨테이너 관리자가 임의의 문자열(app_id)을 특정 cgroup id 에 결부시켜
 * 두면, 그 cgroup 소속으로 발행되는 모든 bio 는 이 app_id 를 들고 다니게
 * 되고, NVMe-oF FC(FC-NVMe) 트랜스포트가 이를 FC 프레임에 실어 보내 스토리지
 * 패브릭/타겟이 "이 I/O 는 어느 VM 것인가"를 식별할 수 있게 한다. block/Kconfig
 * 의 BLK_CGROUP_FC_APPID 도움말(depends on BLK_CGROUP && NVME_FC)이 이 목적을
 * 그대로 설명한다: "Fabric 과 스토리지 타겟이 VM 태그 기반으로 FC 트래픽을
 * 식별·모니터링·처리할 수 있게 한다." 이 파일 자체는 FC-NVMe 트랜스포트에
 * 한정되며, PCIe NVMe(로컬 SSD)의 SQ/CQ/도어벨/PRP 경로와는 무관하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 빌드 시점: block/Makefile:63 에 `obj-$(CONFIG_BLK_CGROUP_FC_APPID) +=
 * blk-cgroup-fc-appid.o` 로 등록되어, FC-NVMe 지원과 cgroup 이 모두 켜진
 * 커널에서만 컴파일된다(Kconfig: depends on BLK_CGROUP && NVME_FC). 실행
 * 흐름은 "설정 경로"와 "조회 경로" 두 갈래다.
 *   (설정) 사용자/libvirt 가 nvme-fc 컨트롤러 device 의 sysfs 속성
 *   appid_store 에 "<cgroup id 16진수>:<app_id 문자열>" 형식으로 write
 *   -> drivers/nvme/host/fc.c::fc_appid_store() 가 문자열을 파싱
 *   -> blkcg_set_fc_appid(app_id, cgrp_id, sizeof(app_id)) [이 파일] 호출.
 *   (조회) NVMe-oF FC 호스트가 실제 I/O 를 FC 프레임으로 구성할 때
 *   -> drivers/nvme/host/fc.c::nvme_fc_io_getuuid(req) 가
 *   -> blkcg_get_fc_appid(rq->bio) [이 파일] 를 호출해 그 bio 가 속한
 *   cgroup 의 app_id 문자열을 얻어, FC LLDD(예: lpfc 같은 FC HBA 드라이버 -
 *   본 소스 트리에는 미포함)가 FC 프레임에 실을 수 있게 반환한다.
 * 실행 컨텍스트: 두 함수 모두 프로세스 컨텍스트에서 동작하는 일반 커널
 * 함수다. blkcg_set_fc_appid() 는 sysfs write 시스템 호출 경로(struct
 * device_attribute.store, 유저 프로세스 컨텍스트)에서 드물게 호출되고,
 * blkcg_get_fc_appid() 는 I/O 제출 경로(블록 계층/FC 트랜스포트)에서 각
 * bio 마다 반복 호출될 수 있는 hot path 함수다.
 *
 * === 타 모듈과의 연결 ===
 * - 의존하는 모듈: block/blk-cgroup.h 가 정의하는 struct blkcg(및 그 안의
 *   fc_app_id 필드, FC_APPID_LEN 상수), struct cgroup/cgroup_subsys_state
 *   와 cgroup_get_from_id()/cgroup_get_e_css()/cgroup_put()/css_put() 같은
 *   cgroup 코어 API(kernel/cgroup/), struct bio 와 bio->bi_blkg(블록 계층의
 *   cgroup 연결 고리).
 * - 이 파일에 의존하는 모듈: drivers/nvme/host/fc.c 가 CONFIG_BLK_CGROUP_FC_APPID
 *   가 켜졌을 때 fc_appid_store() 와 nvme_fc_io_getuuid() 두 곳에서 각각
 *   blkcg_set_fc_appid(), blkcg_get_fc_appid() 를 호출한다(모두
 *   EXPORT_SYMBOL_GPL 로 노출되어 있어 별도 모듈에서도 링크 가능).
 * - 데이터 흐름: 사용자 입력 문자열(app_id) -> fc_appid_store() 파싱 ->
 *   blkcg_set_fc_appid() -> blkcg->fc_app_id[](cgroup 마다 하나씩 존재) ->
 *   bio->bi_blkg->blkcg->fc_app_id 경로로 각 bio 에서 접근 가능 ->
 *   blkcg_get_fc_appid() -> nvme_fc_io_getuuid() -> FC LLDD 가 FC 프레임에
 *   삽입 -> 패브릭/타겟이 VM 단위로 식별.
 * - 공유 핵심 자료구조: struct blkcg(block/blk-cgroup.h) 의
 *   fc_app_id[FC_APPID_LEN] 필드가 이 파일과 nvme-fc 호스트 드라이버가
 *   공유하는 유일한 상태다. 이 파일은 그 필드를 감싸는 두 개의 접근자일 뿐,
 *   자체 자료구조를 정의하지 않는다.
 *
 * === 주요 함수/구조체 요약 ===
 * - blkcg_set_fc_appid(app_id, cgrp_id, app_id_len): cgroup id 로 blkcg 를
 *   찾아 app_id 문자열을 그 blkcg->fc_app_id 에 strscpy() 로 복사(설정 경로,
 *   락 없이 best-effort 로 동작 - 아래 함수 주석 참고).
 * - blkcg_get_fc_appid(bio): bio->bi_blkg 를 거쳐 소속 blkcg 의 fc_app_id
 *   를 반환, 없으면 NULL(조회 경로, I/O 제출 hot path 에서 호출).
 * - 이 파일이 정의하는 구조체/enum 은 없다. 다루는 필드는 오직
 *   struct blkcg.fc_app_id[FC_APPID_LEN](정의: block/blk-cgroup.h) 하나뿐이며,
 *   FC_APPID_LEN 자체의 #define(관례적으로 129) 은 본 소스 트리에는 포함되지
 *   않은 include/linux/blk-cgroup.h 에 있다.
 */

#include "blk-cgroup.h"	/* [한국어] struct blkcg/cgroup, css_to_blkcg(), cgroup_get_from_id() 등 이 파일이 쓰는 모든 타입/헬퍼 선언 포함 */

/*
 * [한국어]
 * blkcg_set_fc_appid - cgroup id 로 지정된 blkcg 에 FC app_id 문자열을 기록한다.
 *
 * @app_id: 기록할 응용/VM 식별 문자열. 호출자(drivers/nvme/host/fc.c::
 *          fc_appid_store())가 사용자 write 문자열 "cgrpid:app_id" 를 파싱해
 *          만든 로컬 버퍼(char app_id[FC_APPID_LEN])를 넘긴다.
 * @cgrp_id: app_id 를 결부시킬 대상 cgroup 의 id(u64). cgroup_get_from_id() 로
 *           조회하는 키이며, 잘못된 값이면 cgroup_get_from_id() 가 ERR_PTR 반환.
 * @app_id_len: app_id 버퍼의 길이. 유일한 호출자는 항상 sizeof(로컬 app_id
 *              버퍼)(=FC_APPID_LEN) 를 넘기므로, 사실상 상한 검증용 값이자
 *              strscpy() 의 count 인자로도 그대로 재사용된다.
 * @return: 성공 시 0. app_id_len 이 FC_APPID_LEN 을 넘으면 -EINVAL, cgrp_id 가
 *          가리키는 cgroup 을 찾지 못하면 cgroup_get_from_id() 의 오류를 그대로
 *          PTR_ERR() 로 전달, io cgroup css 를 얻지 못하면 -ENOENT.
 *
 * 배경: NVMe-oF FC(FC-NVMe) 환경에서는 하나의 호스트 N_Port 뒤에 여러 VM 이
 * 물려 있을 수 있어, 스토리지 타겟/패브릭 입장에서는 어느 I/O 가 어느 VM 의
 * 것인지 구분할 방법이 없다. 이 함수는 그 구분자(app_id)를 cgroup(보통 VM
 * 하나에 대응)에 미리 붙여 두는 "설정" 동작을 담당한다.
 *
 * 동작 단계:
 *   1) app_id_len 이 FC_APPID_LEN 을 넘는지 우선 검사해 버퍼 오버플로를 방지.
 *   2) cgroup_get_from_id() 로 cgrp_id 에 해당하는 struct cgroup 참조를 획득.
 *   3) cgroup_get_e_css() 로 io cgroup 서브시스템(io_cgrp_subsys)의 유효
 *      (effective) css 를 얻는다 - 해당 cgroup 에 io 컨트롤러가 아직 활성화
 *      되지 않았다면 상위 계층의 css 가 반환될 수 있다(cgroup v2 위임 모델).
 *   4) css_to_blkcg() 로 css 를 struct blkcg 로 캐스팅.
 *   5) strscpy() 로 app_id 를 blkcg->fc_app_id 에 복사 - 원본 주석대로 "약간의
 *      경쟁 조건"을 의도적으로 허용(락을 걸지 않음): 이 값을 읽는 I/O 제출
 *      경로(blkcg_get_fc_appid())가 새 값/이전 값 중 하나를 보게 되더라도,
 *      애초에 fabric 에서 vmid 를 얻어오는 동안에도 I/O 가 흘러가는 것을
 *      허용하는 FC-NVMe 프로토콜 특성상 문제가 되지 않는다는 설계 판단.
 *   6) css_put()/cgroup_put() 으로 3),2)에서 얻은 참조 카운트를 반환.
 *
 * 실행 컨텍스트: sysfs 속성 write 시스템 호출을 처리하는 프로세스 컨텍스트
 * 에서 호출된다(잠들 수 있는 cgroup_get_from_id()/cgroup_get_e_css() 를 쓰므로
 * 인터럽트 컨텍스트에서는 호출 불가). 여러 프로세스가 서로 다른 cgroup 에
 * 대해 동시에 호출할 수 있으나, 같은 blkcg->fc_app_id 에 대한 동시 write
 * 보호는 하지 않는다(원본 주석이 명시하는 의도적 설계).
 *
 * caller: drivers/nvme/host/fc.c::fc_appid_store() - nvme-fc 컨트롤러
 *         device 의 "appid_store" sysfs 속성 store 콜백.
 * callee: cgroup_get_from_id(), cgroup_get_e_css(), css_to_blkcg(), strscpy(),
 *         css_put(), cgroup_put() - 모두 cgroup 코어/blkcg 코어 API.
 * 에러 경로: FC_APPID_LEN 초과는 어떤 자원도 잡기 전에 조기 반환(-EINVAL).
 * cgroup 조회 실패는 cgrp 참조를 잡기 전이므로 별도 해제 없이 바로 반환.
 * css 조회 실패(-ENOENT)는 이미 잡은 cgrp 참조만 out_cgrp_put 레이블에서 해제.
 *
 * 호출 체인:
 *   fc_appid_store(sysfs write) -> [blkcg_set_fc_appid] -> strscpy(blkcg->fc_app_id)
 */

/**
 * blkcg_set_fc_appid - set the fc_app_id field associted to blkcg
 * @app_id: application identifier
 * @cgrp_id: cgroup id
 * @app_id_len: size of application identifier
 */
int blkcg_set_fc_appid(char *app_id, u64 cgrp_id, size_t app_id_len)	/* [한국어] app_id/cgrp_id/app_id_len 은 모두 fc_appid_store() 가 sysfs 입력 문자열을 파싱해 만든 값 */
{
	struct cgroup *cgrp;		/* [한국어] cgroup_get_from_id() 가 반환할 대상 cgroup 참조 포인터(참조 카운트 보유) */
	struct cgroup_subsys_state *css;	/* [한국어] io cgroup 서브시스템(io_cgrp_subsys)의 effective css - blkcg 로 캐스팅하기 위한 중간 핸들 */
	struct blkcg *blkcg;		/* [한국어] css_to_blkcg() 로 얻는 최종 목적지: fc_app_id 를 실제로 기록할 구조체 */
	int ret  = 0;			/* [한국어] 반환값(원본 코드의 두 칸 공백은 커널 원문 그대로 유지) - 성공 기본값 0으로 초기화 */

	if (app_id_len > FC_APPID_LEN)	/* [한국어] 호출자가 넘긴 길이가 fc_app_id[] 버퍼 크기를 넘는지 사전 검사 */
		return -EINVAL;		/* [한국어] 버퍼 오버플로 방지를 위해 자원 획득 전에 즉시 실패 반환 */

	cgrp = cgroup_get_from_id(cgrp_id);	/* [한국어] cgrp_id(u64) 로 cgroup 코어에서 struct cgroup 참조 획득(참조 카운트 +1) */
	if (IS_ERR(cgrp))		/* [한국어] 존재하지 않는 cgroup id 등으로 조회에 실패한 경우 */
		return PTR_ERR(cgrp);	/* [한국어] cgrp 를 아직 잡지 않았으므로 별도 해제 없이 오류 코드만 반환 */
	css = cgroup_get_e_css(cgrp, &io_cgrp_subsys); /* [한국어] io 컨트롤러(io_cgrp_subsys)의 유효 css 획득 - cgroup v2 위임 시 상위 css 일 수 있음 */
	if (!css) {			/* [한국어] io cgroup 서브시스템이 이 cgroup 계층에 활성화되어 있지 않은 경우 */
		ret = -ENOENT;		/* [한국어] blkcg 에 접근할 수단이 없으므로 실패 코드를 저장해 둠 */
		goto out_cgrp_put;	/* [한국어] css 는 얻지 못했지만 cgrp 참조는 잡았으므로 해제 경로로 점프 */
	}
	blkcg = css_to_blkcg(css);		/* [한국어] container_of 스타일 캐스팅: css 를 포함하는 struct blkcg 로 변환 */
	/*
	 * There is a slight race condition on setting the appid.
	 * Worst case an I/O may not find the right id.
	 * This is no different from the I/O we let pass while obtaining
	 * the vmid from the fabric.
	 * Adding the overhead of a lock is not necessary.
	 */
	/* [한국어] 위 원문 주석: appid 설정에는 경미한 경쟁 조건이 존재함을 명시.
	 * 최악의 경우 진행 중인 I/O 가 새 id 를 못 보고 지나칠 수 있으나, 이는
	 * fabric 에서 vmid 를 얻어오는 동안에도 I/O 통과를 허용하는 것과 마찬가지
	 * 수준이라 락 오버헤드를 추가할 필요가 없다는 설계 판단이다. */
	strscpy(blkcg->fc_app_id, app_id, app_id_len); /* [한국어] app_id 를 blkcg->fc_app_id[FC_APPID_LEN] 에 최대 app_id_len 바이트까지 NUL 종료 복사 */
	css_put(css);			/* [한국어] cgroup_get_e_css() 가 잡은 css 참조 카운트 반환 */
out_cgrp_put:
	cgroup_put(cgrp);		/* [한국어] cgroup_get_from_id() 가 잡은 cgrp 참조 카운트 반환 - css 실패 경로도 이 지점으로 합류 */
	return ret;			/* [한국어] 0(성공) 또는 -EINVAL/-ENOENT/PTR_ERR(cgrp) 중 하나를 fc_appid_store() 로 전달 */
}
EXPORT_SYMBOL_GPL(blkcg_set_fc_appid);	/* [한국어] GPL 호환 코드(drivers/nvme/host/fc.c 등)에서 이 심볼을 사용할 수 있도록 노출 */

/*
 * [한국어]
 * blkcg_get_fc_appid - bio 가 속한 blkcg 에 설정된 FC app_id 문자열을 조회한다.
 *
 * @bio: app_id 를 조회할 대상 bio. 호출자(nvme_fc_io_getuuid())는 NVMe 요청
 *       (struct request)에 매달린 rq->bio 를 그대로 넘긴다.
 * @return: bio->bi_blkg 가 가리키는 blkcg 의 fc_app_id 문자열 포인터(성공,
 *          blkcg_set_fc_appid() 로 이미 설정된 값). bio 에 blkg 가 아직
 *          연결되지 않았거나(blkcg 미활성/미부착) fc_app_id 가 빈 문자열이면
 *          NULL.
 *
 * 배경: blkcg_set_fc_appid() 가 "쓰기"였다면, 이 함수는 그 값을 실제 I/O
 * 발행 경로에서 "읽어" FC 프레임에 실을 수 있게 돌려주는 짝 함수다. bio
 * 자체는 어떤 cgroup 이 발행했는지 문자열로 알지 못하고 bio->bi_blkg(블록
 * 계층 cgroup 연결 포인터, 블록 계층 core 가 bio 생성 시점에 채워 둠) 로만
 * 알고 있으므로, 이 함수가 그 간접 참조를 대신 풀어 문자열을 꺼내준다.
 *
 * 동작 단계:
 *   1) bio->bi_blkg 가 NULL 인지(이 bio 가 어떤 blkcg 에도 귀속되지 않음,
 *      예: cgroup 컨트롤러 비활성 상태) 확인하고, 아니면 그 blkcg->fc_app_id
 *      의 첫 바이트가 NUL 인지(아직 blkcg_set_fc_appid() 로 설정된 적 없음)
 *      확인 - 둘 중 하나라도 참이면 NULL 반환.
 *   2) 두 조건 모두 거짓이면 bio->bi_blkg->blkcg->fc_app_id 포인터를 그대로
 *      반환(복사가 아니라 blkcg 내부 버퍼를 직접 가리키는 포인터 반환).
 *
 * 실행 컨텍스트: 블록 계층 I/O 제출 경로(NVMe-FC 트랜스포트가 각 명령을
 * 구성하는 시점)에서 호출되는 hot path 함수 - 잠들지 않고 락도 잡지 않는
 * 단순 포인터 역참조라 반복 호출 비용이 낮다. blkcg_set_fc_appid() 가
 * lock-free 로 fc_app_id 를 갱신하는 것과 짝을 이루므로, 이 함수는 항상
 * "설정 중이거나 방금 설정된" 값 중 하나를 일관되지 않게 볼 수 있음을
 * 전제로 설계되었다(위 함수 주석의 race 설명 참고).
 *
 * caller: drivers/nvme/host/fc.c::nvme_fc_io_getuuid() - CONFIG_BLK_CGROUP_FC_APPID
 *         가 켜지고 rq/rq->bio 가 유효할 때만 호출.
 * callee: 없음 - bio->bi_blkg->blkcg 포인터 체인만 역참조하는 순수 조회 함수.
 * 에러 경로: 실패를 errno 로 알리지 않고 NULL 포인터로만 표현 - 호출자
 * nvme_fc_io_getuuid() 는 NULL 을 "이 VM 에 app_id 가 설정되지 않음"으로
 * 해석해 FC 프레임에 아무 것도 싣지 않는다(정상적인 미설정 상태로 취급).
 *
 * 호출 체인:
 *   nvme_fc_io_getuuid(rq->bio) -> [blkcg_get_fc_appid] -> bio->bi_blkg->blkcg->fc_app_id
 */

/**
 * blkcg_get_fc_appid - get the fc app identifier associated with a bio
 * @bio: target bio
 *
 * On success return the fc_app_id, on failure return NULL
 */
char *blkcg_get_fc_appid(struct bio *bio)	/* [한국어] 반환형 char* 는 blkcg 내부 버퍼를 직접 가리키는 포인터 - 호출자는 이를 읽기 전용으로만 취급해야 함 */
{
	if (!bio->bi_blkg || bio->bi_blkg->blkcg->fc_app_id[0] == '\0')	/* [한국어] blkg 미연결이거나 fc_app_id 가 빈 문자열(미설정)인 두 경우를 함께 검사 */
		return NULL;		/* [한국어] 위 두 조건 중 하나라도 참이면 "설정된 app_id 없음"으로 NULL 반환 */
	return bio->bi_blkg->blkcg->fc_app_id; /* [한국어] 설정되어 있는 fc_app_id 문자열의 포인터를 그대로 반환(복사 없음) */
}
EXPORT_SYMBOL_GPL(blkcg_get_fc_appid);	/* [한국어] nvme_fc_io_getuuid() 등 GPL 호환 코드에서 이 심볼을 사용할 수 있도록 노출 */
