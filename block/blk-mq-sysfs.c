// SPDX-License-Identifier: GPL-2.0
/*
 * [한국어 설명] blk-mq 하드웨어 큐(hctx)/소프트웨어 큐(ctx)를 sysfs로 노출하는 레이어 (blk-mq-sysfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 blk-mq.h가 정의하는 struct blk_mq_hw_ctx(이하 hctx, 하드웨어 큐)와
 * struct blk_mq_ctx(이하 ctx, 소프트웨어 큐)를 sysfs kobject 트리로 등록/해제하는
 * 역할만을 전담한다. 구체적으로는 (1) /sys/block/<disk>/mq/ 아래에 hctx 번호별
 * 디렉터리와 그 아래 cpu<N> 서브디렉터리를 kobject_add()로 생성/삭제하고,
 * (2) hctx당 "nr_tags", "nr_reserved_tags", "cpu_list" 세 개의 읽기 전용 sysfs
 * 속성 파일의 show 콜백을 정의하며, (3) blk_mq_ctxs/blk_mq_ctx/blk_mq_hw_ctx 세
 * 자료구조 각각의 kobj_type.release 콜백을 정의해 "kobject 참조 카운트가 0이
 * 되는 순간 누가 무엇을 kfree 하는가"를 이 한 파일에 모아 둔다. bio 제출이나
 * dispatch 같은 I/O 핫 패스에는 전혀 관여하지 않으며, 순수하게 사용자 공간에
 * 큐 구조를 보여주는 관찰(observability) 창구이자, kobject의 refcount 메커니즘을
 * 빌려 hctx/ctx/ctxs 메모리 수명을 관리하는 역할을 겸한다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 실행 컨텍스트: 이 파일의 함수 대부분은 디스크 등록/해제, hw 큐 개수 변경처럼
 * 드물게 일어나는 관리 경로에서 프로세스 컨텍스트로 실행된다(인터럽트/softirq
 * 컨텍스트에서 호출되지 않는다). show 콜백(blk_mq_hw_sysfs_show 등)은 사용자가
 * "cat /sys/block/<disk>/mq/<N>/nr_tags" 등을 실행할 때 VFS read 경로에서
 * 호출된다.
 *
 * [블록 계층 sysfs 등록/해제 흐름과 이 파일의 위치]
 *   디스크 등록: add_disk() -> blk_mq_sysfs_register()(본 파일)
 *     -> blk_mq_register_hctx()(본 파일, hctx마다 반복)
 *     -> kobject_add(hctx->kobj) + kobject_add(ctx->kobj) (cpu<N> 서브디렉터리)
 *   디스크 해제: del_gendisk() -> blk_mq_sysfs_unregister()(본 파일)
 *     -> blk_mq_unregister_hctx()(본 파일) -> kobject_del()
 *   hw 큐 개수 변경: blk_mq_update_nr_hw_queues()(blk-mq.c)
 *     -> blk_mq_sysfs_unregister_hctxs()/blk_mq_sysfs_register_hctxs()(본 파일)
 *   큐 자체의 생성/소멸: blk_mq_init_allocated_queue() -> blk_mq_sysfs_init()
 *     (본 파일, kobject_init만 수행 — 아직 sysfs에 실제로 나타나지는 않음)
 *     blk_mq_exit_queue()/blk_cleanup_queue() -> blk_mq_sysfs_deinit()(본 파일)
 *   사용자 read: cat /sys/block/<disk>/mq/<N>/nr_tags -> VFS -> sysfs_kf_seq_show
 *     -> blk_mq_hw_sysfs_ops.show == blk_mq_hw_sysfs_show()(본 파일)
 *     -> blk_mq_hw_sysfs_nr_tags_show() 등 개별 show 콜백
 *
 * === 타 모듈과의 연결 ===
 * 의존(포함/참조):
 *   blk-mq.h : struct blk_mq_ctxs/blk_mq_ctx/blk_mq_hw_ctx 의 필드(kobj, queue_ctx,
 *              ctxs, tags, cpumask, ctx_map, nr_ctx, queue_num 등)를 직접 참조하고,
 *              hctx_for_each_ctx()/queue_for_each_hw_ctx() 순회 매크로를 사용한다.
 *   blk.h    : blk_queue_registered() 등 request_queue 플래그 헬퍼.
 *   include/linux/blkdev.h : struct request_queue 의 mq_kobj, elevator_lock,
 *              tag_set->tag_list_lock 필드를 사용한다.
 * 피의존(이 파일을 호출하는 쪽):
 *   blk-mq.c : blk_mq_init_allocated_queue()/blk_mq_exit_queue()/
 *              blk_mq_update_nr_hw_queues() 등 큐·hctx 생명주기 전 구간에서
 *              본 파일이 제공하는 6개 공개 함수(blk_mq_sysfs_init/deinit/
 *              register/unregister/register_hctxs/unregister_hctxs,
 *              blk_mq_hctx_kobj_init)를 호출한다.
 *   gendisk 코어(genhd.c) : add_disk()/del_gendisk() 가 blk_mq_sysfs_register()/
 *              blk_mq_sysfs_unregister() 를 호출해 디스크 등록/해제와 sysfs
 *              트리 노출을 맞춘다.
 * 데이터 흐름: hctx/ctx 객체 자체는 blk-mq.c가 할당·초기화하며, 본 파일은 그
 * 위에 kobject만 얹거나 떼어낼 뿐 hctx/ctx의 내용을 바꾸지 않는다(단, show
 * 콜백은 hctx->tags->nr_tags 등을 읽기만 한다). 공유 핵심 자료구조는
 * struct blk_mq_ctxs(전체 per-CPU ctx 컨테이너), struct blk_mq_ctx(소프트웨어
 * 큐), struct blk_mq_hw_ctx(하드웨어 큐) 세 가지이며, 이들의 kobj 필드가 바로
 * 이 파일이 다루는 대상이다.
 *
 * === 주요 함수/구조체 요약 ===
 *   blk_mq_sysfs_release()/blk_mq_ctx_sysfs_release()/blk_mq_hw_sysfs_release() :
 *     각각 blk_mq_ctxs/blk_mq_ctx/blk_mq_hw_ctx 의 kobj_type.release 콜백 —
 *     마지막 kobject 참조가 사라질 때 실제 메모리를 반납한다.
 *   blk_mq_hw_sysfs_show()               : hctx의 sysfs 속성 read() 진입점 —
 *     elevator_lock을 잡고 개별 show 콜백을 호출한다.
 *   blk_mq_hw_sysfs_nr_tags_show()/blk_mq_hw_sysfs_nr_reserved_tags_show()/
 *   blk_mq_hw_sysfs_cpus_show()          : "nr_tags"/"nr_reserved_tags"/
 *     "cpu_list" 세 sysfs 파일의 실제 내용을 만드는 콜백.
 *   blk_mq_register_hctx()/blk_mq_unregister_hctx() : hctx 하나와 그 아래
 *     cpu<N> ctx들을 sysfs에 등록/삭제하는 핵심 루틴 — 실패 시 부분 등록을
 *     롤백한다.
 *   blk_mq_sysfs_register()/blk_mq_sysfs_unregister() : 디스크 전체의 "mq"
 *     sysfs 트리를 만들고 없앤다(gendisk kobject 아래 mq_kobj 부착/제거).
 *   struct blk_mq_hw_ctx_sysfs_entry     : 개별 sysfs 속성(attr) 하나와 그
 *     show 콜백을 짝짓는 구조체 — nr_tags/nr_reserved_tags/cpu_list 3개
 *     인스턴스가 default_hw_ctx_attrs[] 배열로 묶여 default_hw_ctx_groups
 *     ATTRIBUTE_GROUPS를 이룬다.
 */
#include <linux/kernel.h>	/* [한국어] 커널 기본 타입/매크로(printk, min/max 등) — 이 파일 전반의 기반 */
#include <linux/module.h>	/* [한국어] 모듈 매크로류 — blk-mq가 내장/모듈 어느 쪽으로 빌드되어도 필요한 공통 헤더 */
#include <linux/backing-dev.h>	/* [한국어] bdi(backing_dev_info) 관련 선언 — request_queue가 포함하는 bdi 필드 접근에 필요 */
#include <linux/bio.h>		/* [한국어] struct bio 정의 — 본 파일이 직접 bio를 다루지는 않지만 blkdev.h가 전제하는 타입 */
#include <linux/blkdev.h>	/* [한국어] struct request_queue, struct gendisk, blk_queue_registered() 등 block 계층 핵심 타입 선언 */
#include <linux/mm.h>		/* [한국어] PAGE_SIZE 등 메모리 관련 매크로 — blk_mq_hw_sysfs_cpus_show()의 버퍼 크기 계산에 사용 */
#include <linux/init.h>		/* [한국어] __init 등 초기화 관련 매크로 — 커널 공통 관례상 포함 */
#include <linux/slab.h>		/* [한국어] kfree() 등 슬랩 할당자 API — 각 release 콜백에서 구조체 메모리 반납에 사용 */
#include <linux/workqueue.h>	/* [한국어] 워크큐 타입 선언 — struct blk_mq_hw_ctx 정의(run_work 필드)가 이를 전제하므로 blk-mq.h 포함을 위해 필요 */
#include <linux/smp.h>		/* [한국어] for_each_possible_cpu 등 per-CPU/SMP 헬퍼 — blk_mq_sysfs_init/deinit이 모든 CPU를 순회할 때 사용 */

#include "blk.h"		/* [한국어] block 계층 내부 전용 헤더 — blk_queue_registered() 등 request_queue 플래그 매크로 정의 */
#include "blk-mq.h"		/* [한국어] blk-mq 내부 전용 헤더 — blk_mq_ctxs/blk_mq_ctx/blk_mq_hw_ctx 구조체와 hctx_for_each_ctx()/queue_for_each_hw_ctx() 매크로가 여기 있다 */

/*
 * [한국어]
 * blk_mq_sysfs_release - blk_mq_ctxs(전체 per-CPU ctx 컨테이너) kobject 해제 콜백
 *
 * @kobj: 참조 카운트가 0이 되어 해제되는 kobject. struct blk_mq_ctxs::kobj로
 *        임베딩되어 있던 바로 그 kobject 포인터가 전달된다.
 * @return: 없음(void) — kobject 서브시스템의 release 콜백은 kref가 0이 될 때
 *          한 번만 호출되는 통보이므로 반환값을 갖지 않는다.
 *
 * struct blk_mq_ctxs는 request_queue 하나가 소유하는, CPU 개수만큼의
 * blk_mq_ctx(소프트웨어 큐)를 담는 per-CPU 배열(queue_ctx)의 상위 컨테이너다
 * (정의는 block/blk-mq.h). 이 컨테이너 자체도 자신의 kobj를 갖고 있어,
 * blk_mq_sysfs_init()이 만든 각 per-CPU ctx의 kobj가 kobject_get(q->mq_kobj)로
 * 이 컨테이너의 부모 kobj(mq_kobj)에 대한 참조를 하나씩 쥐고 있다가,
 * blk_mq_sysfs_deinit()이 모든 ctx의 kobj를 kobject_put()할 때마다 이 참조가
 * 줄어들고, 마지막 참조가 사라지는 순간 이 콜백이 호출되어 queue_ctx 배열과
 * 컨테이너 자체를 해제한다. 즉 "몇 개의 CPU가 있든 상관없이 마지막 CPU의 ctx가
 * kobj를 반환할 때 딱 한 번" 실행되도록 kobject의 kref 메커니즘을 빌려 온
 * 설계다.
 * 실행 컨텍스트: kobject_put()이 마지막 참조를 되돌리는 임의의 컨텍스트에서
 * 커널 kobject 서브시스템에 의해 동기적으로 호출된다(보통은
 * blk_mq_sysfs_deinit()이 실행되는 프로세스 컨텍스트).
 * 에러 경로: 이 함수는 실패할 수 없다(free_percpu/kfree는 반환값이 없음).
 *
 * 호출 체인:
 *   blk_mq_sysfs_deinit()(본 파일, for_each_possible_cpu 루프의 마지막 반복에서
 *   kobject_put(&ctx->kobj) 호출) -> kobject 서브시스템 내부 kref_put()
 *   -> [blk_mq_sysfs_release]
 */
static void blk_mq_sysfs_release(struct kobject *kobj)
{
	/* [한국어] container_of: kobj가 struct blk_mq_ctxs 안에서 차지하는
	 * 오프셋을 역산해 상위 컨테이너 구조체의 시작 주소를 복원한다. kobject
	 * 자체는 자신이 어떤 구조체에 임베딩되어 있는지 모르므로, 이렇게
	 * "역참조"하는 것이 kobject 콜백에서 원래 구조체를 얻는 표준 관용구다. */
	struct blk_mq_ctxs *ctxs = container_of(kobj, struct blk_mq_ctxs, kobj);

	/* [한국어] free_percpu: blk_mq_alloc_ctxs()(blk-mq.c)가 alloc_percpu()로
	 * 할당했던 per-CPU blk_mq_ctx 배열(ctxs->queue_ctx)을 해제한다. 이 시점
	 * 이전에 각 per-CPU 슬롯(ctx)의 kobj는 이미 blk_mq_ctx_sysfs_release()를
	 * 거쳐 자신의 kobj 참조를 반환했으므로, 여기서는 순수 메모리 회수만
	 * 남아 있다. */
	free_percpu(ctxs->queue_ctx);
	/* [한국어] kfree: 컨테이너 구조체(struct blk_mq_ctxs) 자체의 메모리를
	 * 반납한다. queue_ctx를 먼저 해제한 뒤 컨테이너를 해제하는 순서를
	 * 지켜, ctxs 구조체 안의 포인터 필드를 먼저 정리하고 구조체 자체를
	 * 없애는 일반적인 해제 순서를 따른다. */
	kfree(ctxs);
}

/*
 * [한국어]
 * blk_mq_ctx_sysfs_release - 개별 blk_mq_ctx(소프트웨어 큐) kobject 해제 콜백
 *
 * @kobj: 참조 카운트가 0이 되어 해제되는 kobject. struct blk_mq_ctx::kobj로
 *        임베딩되어 있던 kobject 포인터.
 * @return: 없음(void).
 *
 * blk_mq_ctx는 CPU 하나에 대응하는 소프트웨어 큐로, request_queue가 소유한
 * per-CPU 배열(blk_mq_ctxs->queue_ctx)의 한 슬롯이다. 이 슬롯 자체는
 * free_percpu() 한 번으로 배열 전체가 통째로 해제되므로, 이 release 콜백이
 * "ctx 하나의 메모리"를 직접 kfree 하지는 않는다 — 대신, 이 ctx가 살아있는
 * 동안 쥐고 있던 상위 컨테이너(ctxs)에 대한 kobject 참조 하나를 반환하는
 * 역할만 한다. 모든 CPU의 ctx가 이 콜백을 거쳐 참조를 반환하고 나서야
 * ctxs->kobj의 refcount가 0이 되어 blk_mq_sysfs_release()가 실제 메모리를
 * 해제한다. 즉 이 함수는 "이 ctx는 이제 sysfs에서 완전히 빠졌다"는 것을
 * 상위 컨테이너에게 통보하는 카운트다운 역할이다.
 * 실행 컨텍스트: kobject_put(&ctx->kobj)이 마지막 참조를 되돌리는 컨텍스트
 * (blk_mq_sysfs_deinit()의 for_each_possible_cpu 루프, 또는 sysfs 등록 실패
 * 롤백 경로).
 *
 * 호출 체인:
 *   blk_mq_sysfs_deinit()(본 파일) -> kobject_put(&ctx->kobj) -> kref_put()
 *   -> [blk_mq_ctx_sysfs_release] -> kobject_put(&ctx->ctxs->kobj)
 *   (상위 컨테이너 참조 반환, 필요하면 연쇄적으로 blk_mq_sysfs_release() 유발)
 */
static void blk_mq_ctx_sysfs_release(struct kobject *kobj)
{
	/* [한국어] container_of: kobj로부터 이 kobj를 품고 있던 blk_mq_ctx
	 * 구조체(즉 이 kobj에 대응하는 CPU의 소프트웨어 큐)를 복원한다. */
	struct blk_mq_ctx *ctx = container_of(kobj, struct blk_mq_ctx, kobj);

	/* ctx->ctxs won't be released until all ctx are freed */
	/* [한국어] 원본 영어 주석: "ctx->ctxs는 모든 ctx가 해제되기 전까지는
	 * 해제되지 않는다" — 즉 아래 kobject_put() 한 번이 곧바로 ctxs 전체를
	 * 없애지는 않고, ctxs->kobj의 kref를 하나 줄일 뿐이다. 마지막 CPU의
	 * ctx가 이 함수를 통과할 때 비로소 ctxs->kobj의 refcount가 0이 되어
	 * blk_mq_sysfs_release()가 호출된다. */
	kobject_put(&ctx->ctxs->kobj);
}

/*
 * [한국어]
 * blk_mq_hw_sysfs_release - blk_mq_hw_ctx(하드웨어 큐, hctx) kobject 해제 콜백
 *
 * @kobj: 참조 카운트가 0이 되어 해제되는 kobject. struct blk_mq_hw_ctx::kobj로
 *        임베딩되어 있던 kobject 포인터.
 * @return: 없음(void).
 *
 * hctx는 blk-mq가 드라이버에 노출하는 "하드웨어 큐" 단위 자료구조로, 자신이
 * 담당하는 소프트웨어 큐 배열(ctxs), pending 여부를 나타내는 비트맵
 * (ctx_map, struct sbitmap), 그리고 이 hctx가 실행될 수 있는 CPU 집합
 * (cpumask, cpumask_var_t)을 함께 소유한다. hctx의 kobj 참조가 모두
 * 반환되면(sysfs에서 kobject_del() 된 뒤 마지막 kobject_put()이 일어나면)
 * 이 콜백이 호출되어, hctx가 소유한 위 세 가지 부속 자원을 순서대로 해제하고
 * 마지막으로 hctx 구조체 자체를 kfree 한다.
 * 실행 컨텍스트: kobject_put(&hctx->kobj)이 마지막 참조를 되돌리는 컨텍스트
 * (hw 큐 개수 축소, 큐 소멸 등 관리 경로의 프로세스 컨텍스트).
 *
 * 호출 체인:
 *   (hctx 배열 해제 경로, blk-mq.c) -> kobject_put(&hctx->kobj) -> kref_put()
 *   -> [blk_mq_hw_sysfs_release]
 */
static void blk_mq_hw_sysfs_release(struct kobject *kobj)
{
	/* [한국어] container_of: kobj로부터 이 kobj를 품고 있던 blk_mq_hw_ctx
	 * (하드웨어 큐) 구조체를 복원한다. 정의(struct blk_mq_hw_ctx,
	 * container_of의 두 번째 인자)가 두 번째 줄까지 이어지는 이유는 단지
	 * 80컬럼 개행 스타일 때문이다. */
	struct blk_mq_hw_ctx *hctx = container_of(kobj, struct blk_mq_hw_ctx,
						  kobj);

	/* [한국어] sbitmap_free: hctx->ctx_map(각 소프트웨어 큐별로 "이
	 * 소프트웨어 큐에 아직 처리 안 된 request가 있는가"를 나타내는 비트맵,
	 * struct sbitmap)이 내부적으로 들고 있던 워드 배열 메모리를 해제한다. */
	sbitmap_free(&hctx->ctx_map);
	/* [한국어] free_cpumask_var: hctx->cpumask(이 하드웨어 큐가 실행 가능한
	 * CPU 집합, CONFIG_CPUMASK_OFFSTACK 여부에 따라 별도 할당되거나 인라인
	 * 배열일 수 있음)를 해제한다. cpumask_var_t를 alloc_cpumask_var()로
	 * 할당했을 때만 실제로 free가 일어나고, 그렇지 않은 빌드 구성에서는
	 * 아무 일도 하지 않는 매크로로 컴파일된다. */
	free_cpumask_var(hctx->cpumask);
	/* [한국어] kfree: hctx->ctxs(이 hctx에 매핑된 blk_mq_ctx 포인터 배열,
	 * hctx_for_each_ctx()가 순회하는 대상) 배열 자체의 메모리를 해제한다.
	 * 배열이 가리키는 blk_mq_ctx 구조체들 자체는 여기서 해제되지 않는다 —
	 * 그것들은 request_queue의 per-CPU queue_ctx에 속하며 별도로
	 * blk_mq_sysfs_release()/free_percpu()가 관리한다. */
	kfree(hctx->ctxs);
	/* [한국어] kfree: hctx 구조체 자신의 메모리를 마지막으로 반납한다.
	 * 위에서 hctx의 부속 자원(ctx_map, cpumask, ctxs)을 모두 정리한
	 * 뒤이므로, 이 시점 이후로는 hctx에 대한 어떤 참조도 남아있지 않아야
	 * 한다(use-after-free를 피하려면 이 콜백에 도달하기 전 모든 접근자가
	 * kobject 참조를 통해 수명을 보장했어야 한다). */
	kfree(hctx);
}

/*
 * [한국어] struct blk_mq_hw_ctx_sysfs_entry - hctx 하나의 sysfs 속성(attribute)
 * 파일 하나를 표현하는 구조체.
 *
 * sysfs의 struct attribute는 파일 이름과 권한만 담을 뿐 "값을 어떻게
 * 만들어낼지"는 알지 못한다. 이 구조체는 struct attribute를 첫 필드로
 * 포함시켜(container_of로 역참조 가능하게) 그 뒤에 hctx 전용 show 함수
 * 포인터를 붙임으로써, "이 sysfs 파일을 읽으면 이 콜백을 호출하라"는 연결을
 * 만든다. 이 구조체의 인스턴스 3개(blk_mq_hw_sysfs_nr_tags/
 * nr_reserved_tags/cpus)가 default_hw_ctx_attrs[] 배열로 묶여 모든 hctx의
 * sysfs 디렉터리(/sys/block/<disk>/mq/<N>/)에 공통으로 부착된다.
 */
struct blk_mq_hw_ctx_sysfs_entry {
	struct attribute attr;
	/* [한국어] sysfs에 노출될 파일의 이름(attr.name)과 권한(attr.mode)을
	 * 담는 커널 표준 attribute 구조체.
	 * 설정자: 아래에 정의되는 세 개의 정적 인스턴스(blk_mq_hw_sysfs_nr_tags
	 *   등)가 초기화 시점에 .attr = {.name = "...", .mode = 0444 } 형태로
	 *   직접 채운다.
	 * 읽는 자: kobject_add()가 default_hw_ctx_groups를 통해 sysfs 디렉터리
	 *   엔트리를 만들 때 이름/권한 정보로 사용하고, blk_mq_hw_sysfs_show()가
	 *   container_of_const(attr, ...)로 역참조해 어느 엔트리인지 식별한다.
	 * 값 범위: mode는 항상 0444(모든 사용자 읽기 전용) — 이 파일이 정의하는
	 *   세 속성은 모두 읽기 전용이며 store 콜백이 없다.
	 * 동기화: 이 필드 자체는 모듈 로드 시 정적으로 초기화된 뒤 변경되지
	 *   않으므로(구조체 인스턴스가 static const) 런타임 동기화가 필요 없다. */

	ssize_t (*show)(struct blk_mq_hw_ctx *, char *);
	/* [한국어] 이 sysfs 파일을 read()할 때 실제로 값을 만들어내는 콜백
	 * 함수 포인터. 시그니처는 (hctx, 출력 버퍼) -> 기록한 바이트 수.
	 * 설정자: 세 개의 정적 인스턴스가 각각 blk_mq_hw_sysfs_nr_tags_show/
	 *   blk_mq_hw_sysfs_nr_reserved_tags_show/blk_mq_hw_sysfs_cpus_show로
	 *   초기화한다.
	 * 읽는 자: blk_mq_hw_sysfs_show()가 entry->show(hctx, page) 형태로
	 *   호출하며, 호출 전 반드시 NULL 여부를 검사한다(현재는 세 인스턴스
	 *   모두 항상 채워져 있어 실질적으로 NULL이 될 일은 없지만, 구조체
	 *   설계상 store 전용 attribute가 추가될 가능성을 열어 둔 방어 코드).
	 * 값 범위: 유효한 함수 포인터 또는(이론상) NULL.
	 * 동기화: 함수 포인터 자체는 불변이며, 실제 실행 시 동기화는
	 *   blk_mq_hw_sysfs_show()가 잡는 q->elevator_lock에 의존한다(아래
	 *   blk_mq_hw_sysfs_show 설명 참고). */
};

/*
 * [한국어]
 * blk_mq_hw_sysfs_show - hctx sysfs 속성 파일의 공통 read() 진입점
 *
 * @kobj:  read 대상 파일이 속한 kobject. hctx->kobj가 전달된다.
 * @attr:  read 대상 struct attribute. default_hw_ctx_attrs[]의 세 엔트리 중
 *         하나(nr_tags/nr_reserved_tags/cpu_list의 .attr)가 전달된다.
 * @page:  결과 문자열을 써 넣을 버퍼. sysfs 계층이 PAGE_SIZE 크기로 미리
 *         할당해 전달한다.
 * @return: 실제로 @page에 기록한 바이트 수(양수), show 콜백이 없으면
 *          -EIO(입출력 오류로 사용자에게 read() 실패를 알림).
 *
 * 이 함수는 struct sysfs_ops.show로 등록되어, 이 kobj_type(blk_mq_hw_ktype)에
 * 속한 모든 attribute 파일의 공통 진입점 역할을 한다. attr에서 구체적인
 * blk_mq_hw_ctx_sysfs_entry를 역참조하고, kobj에서 hctx를 역참조한 뒤,
 * q->elevator_lock을 잡고 개별 show 콜백을 호출한다. 이 락이 필요한 이유는
 * include/linux/blkdev.h의 elevator_lock 주석에 명시되어 있는데, nr_hw_queues
 * 변경 경로가 hctx의 tags/reserved-tags/cpumask를 갱신할 수 있으므로, 이
 * 락으로 그 갱신과 sysfs read 사이의 경합을 막는다(즉 여기서 읽는 값들이
 * "갱신 도중의 어중간한 상태"가 아니라 일관된 스냅샷이 되도록 보장).
 * 실행 컨텍스트: 사용자가 sysfs 파일을 read()하는 프로세스 컨텍스트
 * (VFS -> sysfs_kf_seq_show 경유), I/O 핫 패스가 아니다.
 * 에러 경로: entry->show가 설정되지 않은 경우(현재 코드상 발생하지 않지만
 * 방어적으로) -EIO를 반환해 사용자 read() 시스템 호출이 실패로 끝나게 한다.
 *
 * 호출 체인:
 *   사용자 cat /sys/block/<disk>/mq/<N>/nr_tags -> VFS read ->
 *   sysfs_kf_seq_show -> blk_mq_hw_sysfs_ops.show ==
 *   [blk_mq_hw_sysfs_show] -> entry->show(hctx, page) (예:
 *   blk_mq_hw_sysfs_nr_tags_show)
 */
static ssize_t blk_mq_hw_sysfs_show(struct kobject *kobj,
				    struct attribute *attr, char *page)
{
	struct blk_mq_hw_ctx_sysfs_entry *entry;
	/* [한국어] 선언: 아래에서 attr로부터 역참조해 채울 구체적인 sysfs
	 * 엔트리(이름/권한/show 콜백 묶음) 포인터. */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 선언: 아래에서 kobj로부터 역참조해 채울 대상 하드웨어 큐. */
	struct request_queue *q;
	/* [한국어] 선언: hctx가 속한 request_queue — elevator_lock을 잡기 위해
	 * 필요하다. */
	ssize_t res;
	/* [한국어] 선언: entry->show()의 반환값(기록된 바이트 수 또는 음수
	 * errno)을 임시로 담아 두었다가 락 해제 후 그대로 반환하기 위한
	 * 변수 — 락을 쥔 채로 바로 return하지 않는 이유는 mutex_unlock()을
	 * 반드시 거치도록 하기 위함이다. */

	entry = container_of_const(attr, struct blk_mq_hw_ctx_sysfs_entry, attr);
	/* [한국어] container_of_const: attr이 임베딩된 blk_mq_hw_ctx_sysfs_entry
	 * 전체를 역참조한다. _const 버전은 attr이 const 포인터로 들어오면
	 * 반환 포인터도 const로 유지해 타입 안전성을 보존하지만, 여기서는
	 * attr이 비const로 선언되어 있으므로 일반 container_of와 동일하게
	 * 동작한다. */
	hctx = container_of(kobj, struct blk_mq_hw_ctx, kobj);
	/* [한국어] container_of: kobj로부터 이 sysfs 디렉터리가 속한 하드웨어
	 * 큐(hctx) 자체를 역참조한다. */
	q = hctx->queue;
	/* [한국어] hctx->queue: 이 hctx를 소유한 request_queue로의 역참조 —
	 * elevator_lock이 request_queue 단위로 존재하므로 이를 통해 락 객체를
	 * 얻는다. */

	if (!entry->show)
		/* [한국어] 조건: 이 attribute에 show 콜백이 연결되어 있지 않은
		 * 경우 — 현재 정의된 세 엔트리는 모두 show를 채워 두므로 실제로는
		 * 도달하지 않지만, 구조체 설계상 store 전용 attribute가 추가될
		 * 가능성에 대비한 방어적 분기다. */
		return -EIO;
	/* [한국어] -EIO 반환: "이 sysfs 파일은 읽을 수 없다"는 뜻으로 VFS에
	 * 전달되어 사용자의 read() 시스템 호출이 -EIO로 실패한다. */

	mutex_lock(&q->elevator_lock);
	/* [한국어] mutex_lock: q->elevator_lock을 획득 — 이 락은 I/O 스케줄러
	 * 전환뿐 아니라 nr_hw_queues 변경 시 hctx의 tags/reserved-tags/cpumask
	 * 갱신도 보호하므로(include/linux/blkdev.h 주석), 아래 show 콜백이
	 * 이 필드들을 읽는 동안 다른 컨텍스트가 값을 바꾸지 못하도록 막는다. */
	res = entry->show(hctx, page);
	/* [한국어] 개별 show 콜백 호출: blk_mq_hw_sysfs_nr_tags_show 등 실제
	 * attribute별 구현으로 위임해 page 버퍼에 값을 기록하고 그 길이를
	 * 받는다. */
	mutex_unlock(&q->elevator_lock);
	/* [한국어] mutex_unlock: 값을 다 읽었으므로 즉시 락을 반환해 다른
	 * 컨텍스트(예: nr_hw_queues 변경 경로)가 진행할 수 있게 한다. */
	return res;
	/* [한국어] entry->show()가 반환한 값(기록 바이트 수 또는 에러)을
	 * 그대로 호출자(VFS)에게 전달. */
}

/*
 * [한국어]
 * blk_mq_hw_sysfs_nr_tags_show - "nr_tags" sysfs 파일의 내용을 만드는 콜백
 *
 * @hctx: 값을 조회할 대상 하드웨어 큐.
 * @page: 결과를 기록할 버퍼(PAGE_SIZE 크기, blk_mq_hw_sysfs_show가 전달).
 * @return: sprintf()가 기록한 바이트 수.
 *
 * hctx->tags->nr_tags(struct blk_mq_tags::nr_tags, include/linux/blk-mq.h)는
 * 이 하드웨어 큐가 드라이버로부터 부여받은 전체 tag(요청 슬롯) 개수다 —
 * 드라이버가 큐 하나에 동시에 발행 가능한 명령의 최대 개수를 의미한다.
 * 이 값을 10진수 문자열 + 개행으로 그대로 사용자에게 노출한다.
 * 실행 컨텍스트: blk_mq_hw_sysfs_show()가 q->elevator_lock을 쥔 채로 호출.
 *
 * 호출 체인:
 *   blk_mq_hw_sysfs_show() -> [blk_mq_hw_sysfs_nr_tags_show]
 */
static ssize_t blk_mq_hw_sysfs_nr_tags_show(struct blk_mq_hw_ctx *hctx,
					    char *page)
{
	return sprintf(page, "%u\n", hctx->tags->nr_tags);
	/* [한국어] sprintf: hctx->tags->nr_tags(unsigned int)를 10진수 문자열로
	 * 변환해 page에 기록하고 뒤에 개행을 붙인다 — sysfs 관례상 한 줄짜리
	 * 값 파일은 값 뒤에 "\n"을 붙이는 것이 표준이다. 반환값은 널 종료
	 * 문자를 제외한 기록 바이트 수. */
}

/*
 * [한국어]
 * blk_mq_hw_sysfs_nr_reserved_tags_show - "nr_reserved_tags" sysfs 파일의
 * 내용을 만드는 콜백
 *
 * @hctx: 값을 조회할 대상 하드웨어 큐.
 * @page: 결과를 기록할 버퍼.
 * @return: sprintf()가 기록한 바이트 수.
 *
 * hctx->tags->nr_reserved_tags는 전체 tag(nr_tags) 중에서 예약(reserved)
 * 용도로 떼어 둔 개수다. blk-mq는 내부적으로 bitmap_tags(일반 요청용)와
 * breserved_tags(예약 요청용, struct blk_mq_tags 정의 참고) 두 개의
 * sbitmap_queue로 tag 공간을 나누는데, 이 값이 그 경계를 결정한다. 예약
 * tag는 보통 flush 요청처럼 일반 I/O와 별도로 반드시 진행되어야 하는
 * 특수 요청에 쓰인다.
 * 실행 컨텍스트: blk_mq_hw_sysfs_show()가 q->elevator_lock을 쥔 채로 호출.
 *
 * 호출 체인:
 *   blk_mq_hw_sysfs_show() -> [blk_mq_hw_sysfs_nr_reserved_tags_show]
 */
static ssize_t blk_mq_hw_sysfs_nr_reserved_tags_show(struct blk_mq_hw_ctx *hctx,
						     char *page)
{
	return sprintf(page, "%u\n", hctx->tags->nr_reserved_tags);
	/* [한국어] sprintf: hctx->tags->nr_reserved_tags 값을 10진수 문자열 +
	 * 개행으로 page에 기록. */
}

/*
 * [한국어]
 * blk_mq_hw_sysfs_cpus_show - "cpu_list" sysfs 파일의 내용을 만드는 콜백
 *
 * @hctx: 값을 조회할 대상 하드웨어 큐 — hctx->cpumask(cpumask_var_t)를 읽는다.
 * @page: 결과를 기록할 버퍼(PAGE_SIZE 크기).
 * @return: @page에 기록한 총 바이트 수.
 *
 * hctx->cpumask에 속한 CPU 번호들을 "0, 2, 4"처럼 콤마+공백으로 구분한
 * 한 줄로 나열한다. 이 hctx가 어떤 CPU들로부터의 제출을 받을 수 있는지(즉
 * blk_mq_map_queue()가 이 hctx를 반환하게 되는 CPU 집합)를 사용자에게
 * 보여주는 진단용 파일이다. 버퍼 오버플로우를 피하기 위해 PAGE_SIZE-1
 * 바이트를 넘지 않도록 매 반복마다 남은 공간을 확인하고, 공간이 부족하면
 * 나머지 CPU 번호는 잘라내고 루프를 중단한다(마지막에 개행만 최대한
 * 붙여 마무리).
 * 실행 컨텍스트: blk_mq_hw_sysfs_show()가 q->elevator_lock을 쥔 채로 호출.
 * 에러 경로: 없음 — CPU 목록이 버퍼보다 길면 조용히 잘라내며 별도의
 * 에러를 반환하지 않는다(sysfs read는 실패하지 않고 잘린 목록을 보여줌).
 *
 * 호출 체인:
 *   blk_mq_hw_sysfs_show() -> [blk_mq_hw_sysfs_cpus_show] -> for_each_cpu()
 *   (hctx->cpumask 순회)
 */
static ssize_t blk_mq_hw_sysfs_cpus_show(struct blk_mq_hw_ctx *hctx, char *page)
{
	const size_t size = PAGE_SIZE - 1;
	/* [한국어] 선언+초기화: 실제로 쓸 수 있는 최대 바이트 수. PAGE_SIZE
	 * 전체가 아니라 -1인 이유는, 원본 데이터를 이 크기까지만 채우고
	 * 마지막에 반드시 널 종료 여유 한 바이트를 남겨 두기 위함이다
	 * (snprintf가 자체적으로 널 종료를 보장하긴 하지만, 여기서는 뒤이어
	 * 붙는 "\n" 기록을 위한 여유 공간 계산에도 이 size가 기준이 된다). */
	unsigned int i, first = 1;
	/* [한국어] 선언+초기화: i는 for_each_cpu 순회 변수(현재 CPU 번호),
	 * first는 "지금 쓰려는 항목이 목록의 첫 항목인가"를 나타내는 플래그 —
	 * 첫 항목 앞에는 구분자 ", "를 붙이지 않기 위해 사용한다. */
	int ret = 0, pos = 0;
	/* [한국어] 선언+초기화: ret은 매 snprintf() 호출이 반환하는 "기록하려
	 * 했던(잘리지 않았다면) 바이트 수", pos는 지금까지 page에 실제로 쓴
	 * 누적 바이트 수(다음 쓰기 위치의 오프셋). */

	for_each_cpu(i, hctx->cpumask) {
	/* [한국어] for_each_cpu: hctx->cpumask에 설정된 비트(=이 하드웨어
	 * 큐가 실행 가능한 CPU)들을 오름차순으로 순회한다. */
		if (first)
			/* [한국어] 조건: 아직 아무 CPU 번호도 쓰지 않은 첫 반복인
			 * 경우, 구분자 없이 숫자만 기록해 "0, 2, 4" 형태에서
			 * 맨 앞에 불필요한 ", "가 붙지 않게 한다. */
			ret = snprintf(pos + page, size - pos, "%u", i);
			/* [한국어] snprintf: page+pos 위치부터 남은 공간
			 * (size-pos)만큼만 CPU 번호를 기록. 반환값은 널 종료를
			 * 제외하고 "기록하려 했던" 바이트 수(공간이 부족하면
			 * 실제 쓴 것보다 클 수 있음 — 이는 snprintf의 표준
			 * 동작이며 바로 아래 오버플로우 검사에 활용된다). */
		else
			/* [한국어] 조건: 두 번째 이후 CPU 번호는 앞의 값과
			 * 구분하기 위해 ", " 접두어를 붙여 기록한다. */
			ret = snprintf(pos + page, size - pos, ", %u", i);
			/* [한국어] snprintf: 위와 동일하되 ", " 접두어 포함. */

		if (ret >= size - pos)
			/* [한국어] 조건: 방금 기록(혹은 기록하려 한) 바이트 수가
			 * 남은 공간 이상이면 실제로는 잘려서 기록되었거나 공간이
			 * 정확히 소진된 상태 — 더 이상 안전하게 이어 쓸 수
			 * 없으므로 루프를 중단한다. */
			break;
			/* [한국어] break: for_each_cpu 루프 탈출 — 이후 CPU
			 * 번호는 출력에서 누락된다(버퍼 한계로 인한 의도된
			 * 잘림). */

		first = 0;
		/* [한국어] 대입: 첫 항목 처리를 마쳤으므로 이후 반복에서는
		 * ", " 구분자를 쓰도록 플래그를 내린다. */
		pos += ret;
		/* [한국어] 대입: 방금 기록한 만큼 누적 오프셋을 전진시켜, 다음
		 * snprintf가 이어지는 위치에 쓰도록 한다(위 오버플로우 검사를
		 * 통과했으므로 ret 값은 실제로 쓰인 바이트 수와 같음이 보장됨). */
	}

	ret = snprintf(pos + page, size + 1 - pos, "\n");
	/* [한국어] snprintf: CPU 목록 뒤에 개행 문자를 추가. 이번에는 size가
	 * 아니라 size+1(=PAGE_SIZE)을 상한으로 써서, 앞서 -1 해 두었던 널
	 * 종료 여유 한 바이트까지 이 개행에 활용할 수 있게 한다. */
	return pos + ret;
	/* [한국어] 반환: CPU 목록까지 쓴 누적 바이트 수(pos)에 방금 기록한
	 * 개행의 길이(ret, 정상 상황에서는 1)를 더해 총 기록 바이트 수를
	 * sysfs 계층에 알린다. */
}

/*
 * [한국어] default_hw_ctx_attrs[]에 들어갈 세 개의 struct
 * blk_mq_hw_ctx_sysfs_entry 정적 인스턴스 — 각각 "nr_tags",
 * "nr_reserved_tags", "cpu_list" 파일 하나씩을 정의한다. 세 파일 모두
 * 모드 0444(모든 사용자 읽기 전용)이며 store 콜백이 없어 사용자가 값을
 * 쓸 수 없다.
 */
static const struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_nr_tags = {
	.attr = {.name = "nr_tags", .mode = 0444 },
	/* [한국어] .attr 초기화: sysfs 파일명을 "nr_tags", 권한을 0444(rw-r--r--
	 * 아님, 실제로는 소유자/그룹/기타 모두 읽기만 가능 = r--r--r--)로 지정. */
	.show = blk_mq_hw_sysfs_nr_tags_show,
	/* [한국어] .show 초기화: 이 파일을 읽을 때 호출될 콜백을
	 * blk_mq_hw_sysfs_nr_tags_show로 연결. */
};
static const struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_nr_reserved_tags = {
	.attr = {.name = "nr_reserved_tags", .mode = 0444 },
	/* [한국어] .attr 초기화: sysfs 파일명 "nr_reserved_tags", 읽기 전용. */
	.show = blk_mq_hw_sysfs_nr_reserved_tags_show,
	/* [한국어] .show 초기화: blk_mq_hw_sysfs_nr_reserved_tags_show 연결. */
};
static const struct blk_mq_hw_ctx_sysfs_entry blk_mq_hw_sysfs_cpus = {
	.attr = {.name = "cpu_list", .mode = 0444 },
	/* [한국어] .attr 초기화: sysfs 파일명 "cpu_list", 읽기 전용. */
	.show = blk_mq_hw_sysfs_cpus_show,
	/* [한국어] .show 초기화: blk_mq_hw_sysfs_cpus_show 연결. */
};

static const struct attribute *const default_hw_ctx_attrs[] = {
	&blk_mq_hw_sysfs_nr_tags.attr,
	/* [한국어] 위 세 정적 인스턴스의 attr 필드 주소를 모아 NULL 종료
	 * 배열을 구성 — sysfs 그룹(attribute_group) API가 요구하는 형식으로,
	 * 이 배열이 그대로 모든 hctx 디렉터리에 부착될 공통 파일 목록이 된다. */
	&blk_mq_hw_sysfs_nr_reserved_tags.attr,
	&blk_mq_hw_sysfs_cpus.attr,
	NULL,
	/* [한국어] NULL 종료자 — attribute_group 관련 코드가 배열 길이를
	 * 알지 못한 채 순회할 때 끝을 판별하는 관례적 sentinel. */
};
ATTRIBUTE_GROUPS(default_hw_ctx);
/* [한국어] ATTRIBUTE_GROUPS 매크로: default_hw_ctx_attrs[] 하나만 담은
 * "default_hw_ctx_groups"라는 이름의 struct attribute_group 배열(과 그
 * 그룹 하나)을 자동 생성한다. kobj_type.default_groups는 attribute 배열이
 * 아니라 attribute_group 배열을 요구하므로, 이 매크로가 그 변환 보일러
 * 플레이트를 대신 만들어 준다. 아래 blk_mq_hw_ktype.default_groups가 이
 * 결과물(default_hw_ctx_groups)을 사용한다. */

static const struct sysfs_ops blk_mq_hw_sysfs_ops = {
	.show	= blk_mq_hw_sysfs_show,
	/* [한국어] .show 초기화: 이 kobj_type에 속한 모든 attribute 파일의
	 * read() 요청이 공통적으로 blk_mq_hw_sysfs_show()를 거치도록 지정.
	 * .store 필드가 없으므로(생략 시 NULL) 어떤 hctx 속성도 write()로
	 * 값을 바꿀 수 없다 — 이 구조체 전체가 읽기 전용 sysfs 계층임을
	 * 의미한다. */
};

static const struct kobj_type blk_mq_ktype = {
	.release	= blk_mq_sysfs_release,
	/* [한국어] .release 초기화: 이 kobj_type을 사용하는 kobject
	 * (request_queue->mq_kobj가 가리키는 blk_mq_ctxs::kobj)의 참조
	 * 카운트가 0이 되면 blk_mq_sysfs_release()가 호출되도록 연결.
	 * .sysfs_ops/.default_groups가 없으므로 이 kobj_type 자체는 sysfs
	 * 파일을 직접 노출하지 않는, 순수히 생명주기 관리 목적의 타입이다. */
};

static const struct kobj_type blk_mq_ctx_ktype = {
	.release	= blk_mq_ctx_sysfs_release,
	/* [한국어] .release 초기화: 개별 blk_mq_ctx::kobj(소프트웨어 큐 한 개
	 * 단위)의 참조 카운트가 0이 되면 blk_mq_ctx_sysfs_release()가
	 * 호출되도록 연결. 이 타입도 sysfs_ops/default_groups가 없어 자체
	 * attribute 파일은 없다 — cpu<N> 디렉터리는 부모(hctx)의 default
	 * attribute만 상속하지 않고 별도 파일을 갖지 않는다는 뜻이다. */
};

/*
 * [한국어]
 * blk_mq_hw_ktype - hctx kobject의 kobj_type — 위 두 ktype과 달리 실제
 * sysfs 속성 파일(.sysfs_ops/.default_groups)까지 함께 정의한다.
 *
 * .sysfs_ops       : read() 요청을 blk_mq_hw_sysfs_show()로 라우팅.
 * .default_groups  : kobject_add() 시 자동으로 nr_tags/nr_reserved_tags/
 *                    cpu_list 세 파일을 함께 생성하도록 지정하는 그룹.
 * .release         : 참조 카운트 0 시 blk_mq_hw_sysfs_release() 호출.
 */
static const struct kobj_type blk_mq_hw_ktype = {
	.sysfs_ops	= &blk_mq_hw_sysfs_ops,
	/* [한국어] .sysfs_ops 초기화: 위에서 정의한 show 전용 sysfs_ops를
	 * 연결 — 이 hctx의 어떤 attribute를 읽든 blk_mq_hw_sysfs_show()를
	 * 거치게 된다. */
	.default_groups = default_hw_ctx_groups,
	/* [한국어] .default_groups 초기화: ATTRIBUTE_GROUPS(default_hw_ctx)가
	 * 만들어 준 그룹 배열을 지정 — kobject_add()가 이 hctx의 kobject를
	 * sysfs에 등록하는 즉시 nr_tags/nr_reserved_tags/cpu_list 세 파일이
	 * 자동으로 함께 생성된다(개별적으로 sysfs_create_file()을 호출할
	 * 필요가 없다). */
	.release	= blk_mq_hw_sysfs_release,
	/* [한국어] .release 초기화: hctx->kobj의 참조 카운트가 0이 되면
	 * blk_mq_hw_sysfs_release()가 호출되어 ctx_map/cpumask/ctxs/hctx
	 * 자체를 해제하도록 연결. */
};

/*
 * [한국어]
 * blk_mq_unregister_hctx - hctx 하나와 그 아래 모든 ctx의 sysfs 노드를 삭제
 *
 * @hctx: sysfs에서 제거할 하드웨어 큐.
 * @return: 없음(void).
 *
 * blk_mq_register_hctx()의 역작업이다. hctx 아래에 매달린 각 소프트웨어
 * 큐(cpu<N> 디렉터리)를 먼저 kobject_del()로 지우고, 마지막으로 hctx 자신의
 * 디렉터리(숫자 이름의 상위 디렉터리)를 지운다 — 자식 디렉터리를 먼저
 * 지우는 순서를 지켜야 sysfs 트리 구조상 자연스럽다. kobject_del()은
 * kobject_put()과 달리 메모리를 해제하지 않고 sysfs에서만 노드를 제거하며
 * 참조 카운트도 건드리지 않으므로, 이후 실제 메모리 해제는 여전히 각
 * release 콜백(kobject 참조가 0이 될 때)에 맡겨진다. hctx->nr_ctx가 0이면
 * (이 hctx에 매핑된 소프트웨어 큐가 하나도 없으면)애초에 등록된 적이 없는
 * 것으로 보고 곧바로 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크 해제, hw 큐 개수 축소 등 관리
 * 경로).
 *
 * 호출 체인:
 *   blk_mq_sysfs_unregister()/blk_mq_sysfs_unregister_hctxs()(본 파일,
 *   각 hctx에 대해 반복) -> [blk_mq_unregister_hctx]
 *   blk_mq_sysfs_register()(본 파일, 등록 실패 롤백 경로) ->
 *   [blk_mq_unregister_hctx]
 */
static void blk_mq_unregister_hctx(struct blk_mq_hw_ctx *hctx)
{
	struct blk_mq_ctx *ctx;
	/* [한국어] 선언: 아래 hctx_for_each_ctx 루프에서 순회 중인 소프트웨어
	 * 큐를 가리킬 포인터. */
	int i;
	/* [한국어] 선언: hctx_for_each_ctx 매크로가 사용하는 순회 인덱스. */

	if (!hctx->nr_ctx)
		/* [한국어] 조건: 이 hctx에 매핑된 소프트웨어 큐 개수(nr_ctx)가
		 * 0이면, 애초에 blk_mq_register_hctx()에서도 등록을 건너뛴
		 * hctx이므로 여기서도 아무것도 삭제할 것이 없다. */
		return;
		/* [한국어] 조기 반환: 미등록 hctx에 대한 불필요한 kobject_del
		 * 호출(및 잘못된 상태 접근)을 피한다. */

	hctx_for_each_ctx(hctx, ctx, i)
		/* [한국어] hctx_for_each_ctx: hctx->ctxs[0..nr_ctx-1]에 저장된
		 * 모든 소프트웨어 큐를 순회 — 각 ctx가 이 hctx 아래
		 * cpu<N> 디렉터리로 등록되어 있었다면 지운다. */
		if (ctx->kobj.state_in_sysfs)
			/* [한국어] 조건: kobject의 state_in_sysfs 비트 필드로
			 * "이 kobj가 실제로 kobject_add()를 거쳐 sysfs에
			 * 나타나 있는가"를 확인 — register 단계에서 일부만
			 * 성공하고 실패한 경우를 대비해, 실제 등록된 것만
			 * 골라 지우기 위한 방어적 검사다. */
			kobject_del(&ctx->kobj);
			/* [한국어] kobject_del: 이 ctx의 cpu<N> sysfs 디렉터리
			 * 노드를 제거(메모리는 아직 해제하지 않음, 참조
			 * 카운트가 이후 별도로 kobject_put()되어야 실제
			 * 해제됨). */

	if (hctx->kobj.state_in_sysfs)
		/* [한국어] 조건: hctx 자신의 sysfs 등록 여부도 동일하게 검사 —
		 * 자식(ctx) 디렉터리를 모두 지운 뒤에야 부모(hctx) 디렉터리를
		 * 지우는 순서를 지킨다. */
		kobject_del(&hctx->kobj);
		/* [한국어] kobject_del: hctx의 숫자 이름 sysfs 디렉터리
		 * (/sys/block/<disk>/mq/<N>/) 노드를 제거. */
}

/*
 * [한국어]
 * blk_mq_register_hctx - hctx 하나와 그 아래 모든 ctx를 sysfs에 등록
 *
 * @hctx: /sys/block/<disk>/mq/<hctx->queue_num>/ 디렉터리로 등록할 하드웨어
 *        큐.
 * @return: 0 성공, 그 외 kobject_add()가 반환한 음수 errno.
 *
 * hctx->nr_ctx가 0이면(이 hctx에 매핑된 소프트웨어 큐가 없다면) 등록할
 * 필요가 없다고 보고 즉시 0(성공)을 반환한다. 그렇지 않으면 먼저 hctx
 * 자신을 부모 mq_kobj 아래 숫자 이름(queue_num) 디렉터리로 kobject_add()
 * 하고, 성공하면 이어서 hctx에 매핑된 모든 소프트웨어 큐(ctx)를 그 아래
 * "cpu<ctx->cpu>" 이름의 서브디렉터리로 하나씩 kobject_add() 한다. 중간에
 * 어느 ctx 등록이 실패하면, 이미 성공적으로 추가된 ctx들만 골라
 * kobject_del()로 되돌리고 hctx 디렉터리 자체도 제거한 뒤 에러를
 * 반환한다 — "일부만 등록된 hctx"가 sysfs에 남아 사용자를 혼란시키는
 * 것을 방지하는 all-or-nothing 롤백이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(디스크 등록, hw 큐 개수 증가 등 관리
 * 경로), q->tag_set->tag_list_lock을 호출자가 이미 잡고 있는 상태에서
 * 불린다(blk_mq_sysfs_register() 참고).
 * 에러 경로: kobject_add() 실패 시 그 지점까지의 부분 등록을 롤백하고
 * 실패 코드를 그대로 전파한다.
 *
 * 호출 체인:
 *   blk_mq_sysfs_register()(본 파일, queue_for_each_hw_ctx 루프) ->
 *   [blk_mq_register_hctx]
 *   blk_mq_sysfs_register_hctxs()(본 파일, queue_for_each_hw_ctx 루프) ->
 *   [blk_mq_register_hctx]
 */
static int blk_mq_register_hctx(struct blk_mq_hw_ctx *hctx)
{
	struct request_queue *q = hctx->queue;
	/* [한국어] 선언+초기화: hctx가 속한 request_queue — 이 큐의
	 * mq_kobj를 hctx 디렉터리의 부모로 사용하기 위해 필요하다. */
	struct blk_mq_ctx *ctx;
	/* [한국어] 선언: 아래 hctx_for_each_ctx 루프에서 순회할 소프트웨어
	 * 큐 포인터. */
	int i, j, ret;
	/* [한국어] 선언: i는 등록(성공) 루프의 인덱스, j는 실패 시 롤백
	 * 루프의 인덱스(i까지만 되돌리기 위해 별도로 둠), ret은
	 * kobject_add()의 반환값 겸 이 함수의 최종 반환값. */

	if (!hctx->nr_ctx)
		/* [한국어] 조건: 매핑된 소프트웨어 큐가 없는 hctx — hw 큐 개수가
		 * CPU 개수보다 많아 일부 hctx가 어떤 CPU에도 매핑되지 않은
		 * 경우(unused hctx) 등록을 생략한다. */
		return 0;
		/* [한국어] 성공(0) 반환: 등록할 대상이 없으므로 실패가 아니라
		 * "할 일 없음"으로 처리. */

	ret = kobject_add(&hctx->kobj, q->mq_kobj, "%u", hctx->queue_num);
	/* [한국어] kobject_add: hctx->kobj를 부모 q->mq_kobj 아래
	 * "<queue_num>"이라는 숫자 이름의 디렉터리로 등록 —
	 * /sys/block/<disk>/mq/<queue_num>/ 이 이 시점에 실제로 나타난다.
	 * hctx->kobj의 ktype(blk_mq_hw_ktype)에 연결된 default_groups
	 * 덕분에 nr_tags 등 세 파일도 함께 생성된다. */
	if (ret)
		/* [한국어] 조건: kobject_add()가 음수 errno를 반환(디렉터리
		 * 생성 실패, 예: 이름 충돌, 메모리 부족) — 이 시점에는 아직
		 * ctx 서브디렉터리를 하나도 만들지 않았으므로 롤백할 것이
		 * 없다. */
		return ret;
		/* [한국어] 실패 코드 즉시 반환: hctx 디렉터리 자체가 만들어지지
		 * 않았으므로 별도 정리 없이 그대로 에러 전파. */

	hctx_for_each_ctx(hctx, ctx, i) {
	/* [한국어] hctx_for_each_ctx: hctx->ctxs[0..nr_ctx-1]의 각 소프트웨어
	 * 큐를 순회하며 하나씩 sysfs에 추가한다. i는 성공적으로 처리된
	 * 개수(정확히는 마지막으로 시도한 인덱스)를 추적해 실패 시 롤백
	 * 범위를 정하는 데 쓰인다. */
		ret = kobject_add(&ctx->kobj, &hctx->kobj, "cpu%u", ctx->cpu);
		/* [한국어] kobject_add: 이 ctx를 방금 만든 hctx 디렉터리 아래
		 * "cpu<ctx->cpu>" 이름의 서브디렉터리로 등록 —
		 * /sys/block/<disk>/mq/<queue_num>/cpu<N>/ 이 나타난다. 이
		 * 이름이 "이 CPU에서 나가는 I/O가 이 hctx로 매핑된다"는
		 * 관계를 사용자에게 직접 보여준다. */
		if (ret)
			/* [한국어] 조건: 이 ctx의 kobject_add()가 실패한 경우
			 * (예: 극히 드문 이름 충돌이나 메모리 부족) — 이미
			 * 추가된 이전 ctx들과 hctx 자체를 되돌려야 한다. */
			goto out;
			/* [한국어] goto out: 아래 롤백 블록으로 점프 — 이
			 * 함수는 all-or-nothing 등록을 보장하므로 부분 성공을
			 * 그대로 남겨두지 않는다. */
	}

	return 0;
	/* [한국어] 성공 반환: hctx와 그 아래 모든 ctx가 예외 없이 등록되었음을
	 * 의미. */
out:
	/* [한국어] 롤백 레이블: 위에서 goto out으로 도달 — 지금까지 성공적으로
	 * 추가된 ctx들만 골라 되돌리고 hctx 디렉터리도 제거한다. */
	hctx_for_each_ctx(hctx, ctx, j) {
	/* [한국어] hctx_for_each_ctx: 동일한 hctx->ctxs[]를 처음부터 다시
	 * 순회하되, 이번에는 실패 이전에 성공했던 인덱스(j < i)만 골라내
	 * 지우기 위해 별도의 인덱스 변수 j를 사용한다(i는 바깥 스코프에서
	 * 실패 시점의 값을 그대로 보존하고 있음). */
		if (j < i)
			/* [한국어] 조건: 현재 순회 위치 j가 실패가 발생한
			 * 인덱스 i보다 작으면, 그 ctx는 앞서 kobject_add()에
			 * 성공해 실제로 sysfs에 남아 있는 상태이므로 되돌려야
			 * 한다. j == i인 실패 당사자 자신은 애초에 kobject_add
			 * 가 실패해 sysfs에 등록되지 않았으므로 kobject_del
			 * 대상에서 제외된다. */
			kobject_del(&ctx->kobj);
			/* [한국어] kobject_del: 이미 등록되어 있던 이 ctx의
			 * cpu<N> sysfs 노드를 제거해 부분 등록 상태를
			 * 되돌린다. */
	}
	kobject_del(&hctx->kobj);
	/* [한국어] kobject_del: hctx 자신의 sysfs 디렉터리도 제거해, 이
	 * 함수가 실패로 끝날 때 hctx 관련 sysfs 노드가 하나도 남지 않도록
	 * 한다. */
	return ret;
	/* [한국어] 실패 코드 반환: 호출자(blk_mq_sysfs_register() 등)가 이
	 * 값을 보고 자신의 등록 루프도 함께 롤백하도록 전파한다. */
}

/*
 * [한국어]
 * blk_mq_hctx_kobj_init - hctx의 kobject를 초기화(아직 sysfs에 등록하지는
 * 않음)
 *
 * @hctx: kobject를 초기화할 하드웨어 큐.
 * @return: 없음(void).
 *
 * kobject_init()은 kobject의 내부 상태(kref=1, ktype 포인터 등)를 세팅할
 * 뿐, 이름을 붙이거나 부모 아래에 매다는 작업(kobject_add())은 하지 않는다.
 * 이 함수는 hctx->kobj에 blk_mq_hw_ktype(즉 nr_tags 등 sysfs 속성과 release
 * 콜백 묶음)을 연결해, 이후 blk_mq_register_hctx()가 kobject_add()를 호출할
 * 때 이미 올바른 타입 정보를 갖도록 사전 준비하는 역할이다. hctx가 실제로
 * sysfs에 나타나는 시점은 이 함수가 아니라 blk_mq_register_hctx()다.
 * 실행 컨텍스트: 프로세스 컨텍스트(hctx 할당 직후, 큐 초기화 또는 hw 큐
 * 개수 변경 경로).
 *
 * 호출 체인:
 *   blk_mq_sysfs_register_hctxs()/hctx 할당 경로(blk-mq.c) ->
 *   [blk_mq_hctx_kobj_init]
 */
void blk_mq_hctx_kobj_init(struct blk_mq_hw_ctx *hctx)
{
	kobject_init(&hctx->kobj, &blk_mq_hw_ktype);
	/* [한국어] kobject_init: hctx->kobj의 kref를 1로 설정하고
	 * ktype 포인터를 &blk_mq_hw_ktype으로 지정 — 이 kobject가 나중에
	 * kobject_put()으로 참조를 반환할 때 blk_mq_hw_sysfs_release()가
	 * 호출되도록, 그리고 sysfs 파일 read 시 blk_mq_hw_sysfs_show()가
	 * 쓰이도록 연결하는 최초 준비 단계다. 이 시점에는 아직
	 * kobject_add()가 호출되지 않았으므로 실제 sysfs 디렉터리는
	 * 존재하지 않는다. */
}

/*
 * [한국어]
 * blk_mq_sysfs_deinit - request_queue의 per-CPU ctx kobject와 mq_kobj 참조를
 * 반환
 *
 * @q: 해제 중인 request_queue.
 * @return: 없음(void).
 *
 * blk_mq_sysfs_init()의 정확한 역작업이다. 모든 가능한 CPU를 순회하며 각
 * per-CPU blk_mq_ctx의 kobj 참조를 kobject_put()으로 하나씩 반환한다 —
 * 이 과정에서 (blk_mq_ctx_sysfs_release()를 통해) ctxs->kobj의 참조도 하나씩
 * 줄어들고, 마지막 CPU 차례에 그 참조가 0이 되면 blk_mq_sysfs_release()가
 * 연쇄적으로 호출되어 queue_ctx 배열과 컨테이너가 해제된다. 마지막으로
 * q->mq_kobj 자체의 참조도 kobject_put()으로 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(request_queue 소멸 경로 후반부, 이미
 * freeze/quiesce 되어 새 I/O가 들어오지 않는 상태).
 *
 * 호출 체인:
 *   blk_mq_exit_queue()/blk_cleanup_queue() 계열(blk-mq.c) ->
 *   [blk_mq_sysfs_deinit] -> kobject_put(&ctx->kobj) (CPU마다) ->
 *   blk_mq_ctx_sysfs_release() -> kobject_put(&ctxs->kobj) (연쇄) ->
 *   (마지막 참조 시) blk_mq_sysfs_release()
 */
void blk_mq_sysfs_deinit(struct request_queue *q)
{
	struct blk_mq_ctx *ctx;
	/* [한국어] 선언: 아래 루프에서 매 CPU마다 가져올 소프트웨어 큐
	 * 포인터. */
	int cpu;
	/* [한국어] 선언: for_each_possible_cpu 순회 변수. */

	for_each_possible_cpu(cpu) {
	/* [한국어] for_each_possible_cpu: 현재 온라인 여부와 무관하게 시스템이
	 * "가질 수 있는" 모든 CPU 번호를 순회 — per-CPU 배열은 possible CPU
	 * 개수만큼 할당되어 있으므로, 오프라인 CPU의 슬롯도 빠짐없이
	 * 정리해야 한다. */
		ctx = per_cpu_ptr(q->queue_ctx, cpu);
		/* [한국어] per_cpu_ptr: per-CPU 포인터 q->queue_ctx에서 이번
		 * 반복의 cpu에 해당하는 실제 blk_mq_ctx 슬롯 주소를 얻는다. */
		kobject_put(&ctx->kobj);
		/* [한국어] kobject_put: 이 ctx의 kobj 참조 카운트를 1 감소 —
		 * 0이 되면 blk_mq_ctx_sysfs_release()가 호출되어 상위
		 * ctxs->kobj 참조를 반환한다(ctx 슬롯 자체의 메모리는 이후
		 * free_percpu()가 배열 전체를 한 번에 해제하므로 여기서
		 * 개별적으로 kfree되지 않는다). */
	}
	kobject_put(q->mq_kobj);
	/* [한국어] kobject_put: request_queue의 mq 루트 kobject(mq_kobj)에
	 * 대한 참조를 반환 — 이 kobject는 blk_mq_ktype을 쓰므로, 참조가
	 * 0이 되면 blk_mq_sysfs_release()가 호출되어 queue_ctx 배열과
	 * blk_mq_ctxs 컨테이너를 해제한다(단, 위 루프에서 모든 ctx가 이미
	 * 자신의 참조를 반환했어야 실제로 0에 도달한다). */
}

/*
 * [한국어]
 * blk_mq_sysfs_init - request_queue의 mq_kobj와 per-CPU ctx kobject를 초기화
 *
 * @q: 초기화 대상 request_queue.
 * @return: 없음(void).
 *
 * 아직 실제 sysfs 디렉터리를 만들지는 않고(kobject_add() 없음), kobject의
 * 내부 상태(kref, ktype)만 세팅해 이후 blk_mq_sysfs_register()가
 * kobject_add()를 호출할 준비를 갖춘다. q->mq_kobj 자체를 blk_mq_ktype으로
 * 초기화한 뒤, 모든 가능한 CPU에 대해 그 CPU의 blk_mq_ctx를 blk_mq_ctx_ktype
 * 으로 초기화하면서, 각 ctx가 mq_kobj에 대한 참조를 하나씩 미리 쥐도록
 * kobject_get(q->mq_kobj)을 호출해 둔다 — 이 참조들이 나중에
 * blk_mq_sysfs_deinit()에서 하나씩 반환되며 mq_kobj/컨테이너의 수명을
 * 결정하게 된다.
 * 실행 컨텍스트: 프로세스 컨텍스트(request_queue 생성 초기 단계,
 * blk_mq_init_allocated_queue() 경로).
 *
 * 호출 체인:
 *   blk_mq_init_allocated_queue()(blk-mq.c) -> [blk_mq_sysfs_init]
 */
void blk_mq_sysfs_init(struct request_queue *q)
{
	struct blk_mq_ctx *ctx;
	/* [한국어] 선언: 아래 루프에서 매 CPU마다 초기화할 소프트웨어 큐
	 * 포인터. */
	int cpu;
	/* [한국어] 선언: for_each_possible_cpu 순회 변수. */

	kobject_init(q->mq_kobj, &blk_mq_ktype);
	/* [한국어] kobject_init: q->mq_kobj(blk_mq_ctxs::kobj를 가리키는
	 * 포인터)의 kref를 1로 설정하고 ktype을 blk_mq_ktype으로 지정 —
	 * 참조가 0이 되면 blk_mq_sysfs_release()가 호출되도록 연결. */

	for_each_possible_cpu(cpu) {
	/* [한국어] for_each_possible_cpu: 시스템이 가질 수 있는 모든 CPU에
	 * 대해 그 CPU 전용 소프트웨어 큐(ctx)를 초기화한다. */
		ctx = per_cpu_ptr(q->queue_ctx, cpu);
		/* [한국어] per_cpu_ptr: 이번 반복의 cpu에 해당하는 blk_mq_ctx
		 * 슬롯 주소를 얻는다. */

		kobject_get(q->mq_kobj);
		/* [한국어] kobject_get: mq_kobj의 참조 카운트를 1 증가 —
		 * "이 ctx가 살아있는 동안 mq_kobj(및 그 상위 blk_mq_ctxs
		 * 컨테이너)도 함께 살아있어야 한다"는 의존 관계를 참조
		 * 카운트로 표현한다. 이렇게 미리 쥐어 둔 참조는
		 * blk_mq_ctx_sysfs_release()가 이 ctx의 kobj를 해제할 때
		 * kobject_put(&ctx->ctxs->kobj)로 반환된다. */
		kobject_init(&ctx->kobj, &blk_mq_ctx_ktype);
		/* [한국어] kobject_init: 이 ctx 자신의 kobj를 kref=1,
		 * ktype=blk_mq_ctx_ktype으로 초기화 — 이후 필요 시(hctx 등록
		 * 시점) kobject_add()로 cpu<N> 디렉터리 이름을 부여받고 실제
		 * sysfs에 나타난다. */
	}
}

/*
 * [한국어]
 * blk_mq_sysfs_register - gendisk 아래 "mq" sysfs 트리 전체를 생성하고
 * 모든 hctx를 등록
 *
 * @disk: mq 트리를 노출할 대상 gendisk(블록 디바이스).
 * @return: 0 성공, 그 외 kobject_add()/blk_mq_register_hctx()가 반환한
 *          음수 errno.
 *
 * 먼저 q->mq_kobj를 disk의 kobject(disk_to_dev(disk)->kobj) 아래 "mq"라는
 * 이름으로 kobject_add()해 /sys/block/<disk>/mq/ 디렉터리 자체를 만들고,
 * udev 등 사용자 공간에 알리는 KOBJ_ADD uevent를 보낸다. 그다음
 * q->tag_set->tag_list_lock을 잡은 채(하나의 tag_set을 여러 request_queue가
 * 공유할 수 있는 경우를 포함해 hctx 목록 자체가 재계산되는 것을 막기 위함)
 * 모든 hctx에 대해 blk_mq_register_hctx()를 호출한다. 중간에 어느 hctx
 * 등록이 실패하면, 그때까지 등록에 성공한 hctx들만 blk_mq_unregister_hctx()
 * 로 되돌리고, "mq" 디렉터리 자체도 KOBJ_REMOVE uevent와 함께 제거한 뒤
 * 에러를 반환한다 — 부분적으로만 노출된 mq 트리가 남지 않도록 하는
 * all-or-nothing 등록이다.
 * 실행 컨텍스트: 프로세스 컨텍스트(add_disk() 경로).
 * 에러 경로: kobject_add() 실패 시 그 자리에서 즉시 반환(아직 아무것도
 * 등록되지 않았으므로 롤백 불필요). blk_mq_register_hctx() 실패 시
 * out_unreg 레이블로 점프해 부분 등록을 롤백.
 *
 * 호출 체인:
 *   add_disk()(genhd.c) -> [blk_mq_sysfs_register] -> blk_mq_register_hctx()
 *   (hctx마다 반복)
 */
int blk_mq_sysfs_register(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	/* [한국어] 선언+초기화: 이 디스크가 사용하는 request_queue —
	 * mq_kobj/tag_set 등 이후 모든 등록 작업의 대상. */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 선언: 아래 queue_for_each_hw_ctx 루프에서 순회할 하드웨어
	 * 큐 포인터. */
	unsigned long i, j;
	/* [한국어] 선언: i는 등록 루프 인덱스(실패 시 그 값을 롤백 범위
	 * 상한으로 사용), j는 롤백 전용 루프 인덱스. */
	int ret;
	/* [한국어] 선언: kobject_add()/blk_mq_register_hctx()의 반환값을
	 * 담아 최종적으로 이 함수의 반환값이 되는 변수. */

	ret = kobject_add(q->mq_kobj, &disk_to_dev(disk)->kobj, "mq");
	/* [한국어] kobject_add: q->mq_kobj를 gendisk에 대응하는 struct
	 * device의 kobject 아래 "mq"라는 이름으로 등록 —
	 * /sys/block/<disk>/mq/ 디렉터리가 이 시점에 실제로 나타난다. */
	if (ret < 0)
		/* [한국어] 조건: mq 루트 디렉터리 생성 자체가 실패한 경우(예:
		 * 이름 충돌, 메모리 부족) — 아직 아무 hctx도 건드리지 않았으므로
		 * 별다른 정리 없이 즉시 실패를 알린다. */
		return ret;
		/* [한국어] 실패 코드 즉시 반환. */

	kobject_uevent(q->mq_kobj, KOBJ_ADD);
	/* [한국어] kobject_uevent: udev 등 사용자 공간 데몬에게 "mq_kobj가
	 * 새로 추가되었다"는 netlink 이벤트(KOBJ_ADD)를 보낸다 — 이를 받은
	 * udev가 필요하다면 규칙(rule)을 적용할 수 있다. */

	mutex_lock(&q->tag_set->tag_list_lock);
	/* [한국어] mutex_lock: 이 request_queue가 속한 tag_set의
	 * tag_list_lock을 획득 — 같은 tag_set을 공유하는 여러
	 * request_queue의 hctx 목록이 이 등록 과정 중간에 변경(hw 큐 개수
	 * 조정 등)되는 것을 막기 위함이다. */
	queue_for_each_hw_ctx(q, hctx, i) {
	/* [한국어] queue_for_each_hw_ctx: q->queue_hw_ctx[0..nr_hw_queues-1]
	 * 의 모든 하드웨어 큐를 순회하며 하나씩 sysfs에 등록한다. i는
	 * 마지막으로 시도한 인덱스를 남겨 실패 시 롤백 범위를 정하는 데
	 * 쓰인다. */
		ret = blk_mq_register_hctx(hctx);
		/* [한국어] 이 hctx와 그 아래 모든 ctx를 sysfs에 등록(위
		 * blk_mq_register_hctx() 참고, 실패 시 해당 hctx 내부에서
		 * 이미 자체 롤백까지 마친 상태로 에러를 반환한다). */
		if (ret)
			/* [한국어] 조건: 이 hctx 등록이 실패한 경우 — 이전까지
			 * 등록에 성공한 hctx들을 되돌려야 한다. */
			goto out_unreg;
			/* [한국어] goto out_unreg: 아래 롤백 블록으로 점프. */
	}
	mutex_unlock(&q->tag_set->tag_list_lock);
	/* [한국어] mutex_unlock: 모든 hctx 등록에 성공했으므로 tag_list_lock을
	 * 반환. */
	return 0;
	/* [한국어] 성공 반환: mq 트리 전체(루트 + 모든 hctx + 모든 ctx)가
	 * 예외 없이 등록되었음을 의미. */

out_unreg:
	/* [한국어] 롤백 레이블: 위에서 goto out_unreg로 도달 — 지금까지 등록에
	 * 성공한 hctx들만 되돌리고 mq 트리 전체를 제거한다. */
	queue_for_each_hw_ctx(q, hctx, j) {
	/* [한국어] queue_for_each_hw_ctx: 동일한 hctx 배열을 처음부터 다시
	 * 순회하되, 실패 이전 인덱스(j < i)만 골라 롤백하기 위해 별도의
	 * 인덱스 변수 j를 사용한다. */
		if (j < i)
			/* [한국어] 조건: j가 실패가 발생한 인덱스 i보다 작으면
			 * 그 hctx는 이미 등록에 성공해 sysfs에 남아 있는
			 * 상태이므로 등록 해제 대상이다. */
			blk_mq_unregister_hctx(hctx);
			/* [한국어] blk_mq_unregister_hctx: 그 hctx와 아래
			 * ctx들의 sysfs 노드를 제거해 부분 등록 상태를
			 * 되돌린다. */
	}
	mutex_unlock(&q->tag_set->tag_list_lock);
	/* [한국어] mutex_unlock: 롤백 루프까지 마쳤으므로 tag_list_lock을
	 * 반환. */

	kobject_uevent(q->mq_kobj, KOBJ_REMOVE);
	/* [한국어] kobject_uevent: mq 트리 전체를 제거할 것이므로 사용자
	 * 공간에 KOBJ_REMOVE 이벤트를 먼저 통보한다(kobject_del 이전에
	 * 알리는 것이 sysfs 관례). */
	kobject_del(q->mq_kobj);
	/* [한국어] kobject_del: mq 루트 디렉터리(/sys/block/<disk>/mq/) 노드
	 * 자체를 제거 — 등록 실패 시 mq 트리 흔적을 sysfs에 전혀 남기지
	 * 않는다. */
	return ret;
	/* [한국어] 실패 코드 반환: 호출자(add_disk() 등)에게 등록 실패를
	 * 알린다. */
}

/*
 * [한국어]
 * blk_mq_sysfs_unregister - gendisk 아래 "mq" sysfs 트리 전체를 제거
 *
 * @disk: mq 트리를 제거할 대상 gendisk.
 * @return: 없음(void).
 *
 * blk_mq_sysfs_register()의 정상 경로 역작업이다. tag_list_lock을 잡은 채
 * 모든 hctx에 대해 blk_mq_unregister_hctx()를 호출해 각 hctx/ctx sysfs
 * 노드를 지운 뒤, 락을 반환하고 mq 루트 디렉터리 자체도 uevent와 함께
 * 제거한다. (blk_mq_sysfs_register()의 실패 롤백 경로와 달리 이 함수는
 * "이미 정상적으로 다 등록되어 있던" 상태를 전제로 하므로 j<i 같은 부분
 * 롤백 로직이 없다 — 항상 전체를 지운다.)
 * 실행 컨텍스트: 프로세스 컨텍스트(del_gendisk() 경로).
 *
 * 호출 체인:
 *   del_gendisk()(genhd.c) -> [blk_mq_sysfs_unregister] ->
 *   blk_mq_unregister_hctx() (hctx마다 반복)
 */
void blk_mq_sysfs_unregister(struct gendisk *disk)
{
	struct request_queue *q = disk->queue;
	/* [한국어] 선언+초기화: 이 디스크가 사용하는 request_queue. */
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 선언: 아래 queue_for_each_hw_ctx 루프에서 순회할 하드웨어
	 * 큐 포인터. */
	unsigned long i;
	/* [한국어] 선언: queue_for_each_hw_ctx 순회 인덱스. */

	mutex_lock(&q->tag_set->tag_list_lock);
	/* [한국어] mutex_lock: hctx 목록이 이 제거 과정 중간에 바뀌지 않도록
	 * tag_set의 tag_list_lock을 잡는다. */
	queue_for_each_hw_ctx(q, hctx, i)
		/* [한국어] queue_for_each_hw_ctx: 모든 하드웨어 큐를 순회하며
		 * 각각의 sysfs 노드(hctx 디렉터리 + 그 아래 모든 ctx)를
		 * 제거한다. */
		blk_mq_unregister_hctx(hctx);
		/* [한국어] blk_mq_unregister_hctx: 이 hctx와 그 아래 ctx들의
		 * kobject_del()을 수행. */
	mutex_unlock(&q->tag_set->tag_list_lock);
	/* [한국어] mutex_unlock: 모든 hctx 제거를 마쳤으므로 락 반환. */

	kobject_uevent(q->mq_kobj, KOBJ_REMOVE);
	/* [한국어] kobject_uevent: mq 루트 자체를 제거하기 전에 사용자
	 * 공간에 KOBJ_REMOVE를 통보. */
	kobject_del(q->mq_kobj);
	/* [한국어] kobject_del: /sys/block/<disk>/mq/ 디렉터리 자체를 제거 —
	 * (kobject_put()이 아니므로 mq_kobj의 참조 카운트/메모리 해제는
	 * 별도로 blk_mq_sysfs_deinit()이 담당한다). */
}

/*
 * [한국어]
 * blk_mq_sysfs_unregister_hctxs - request_queue에 속한 hctx들의 sysfs 노드만
 * 제거(mq 루트 디렉터리는 유지)
 *
 * @q: 대상 request_queue.
 * @return: 없음(void).
 *
 * blk_mq_sysfs_unregister()와 달리 mq_kobj 자체는 건드리지 않고 hctx들만
 * 제거한다 — blk_mq_update_nr_hw_queues()가 hw 큐 개수를 바꾸기 위해 기존
 * hctx sysfs 노드를 모두 지운 뒤, 재구성된 hctx 배열로 다시
 * blk_mq_sysfs_register_hctxs()를 호출하는 시나리오에서 쓰인다. 이 함수
 * 진입 전에 blk_queue_registered(q)로 "이 큐가 이미 gendisk에 등록되어
 * mq_kobj가 실제로 sysfs에 나타나 있는 상태인지"를 확인해, 아직 등록되지
 * 않은 큐(예: probe 초기 단계)에 대해서는 애초에 지울 sysfs 노드가 없으므로
 * 곧바로 반환한다.
 * 실행 컨텍스트: 프로세스 컨텍스트(hw 큐 개수 변경 경로).
 *
 * 호출 체인:
 *   blk_mq_update_nr_hw_queues()(blk-mq.c) ->
 *   [blk_mq_sysfs_unregister_hctxs] -> blk_mq_unregister_hctx() (hctx마다
 *   반복)
 */
void blk_mq_sysfs_unregister_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 선언: 아래 queue_for_each_hw_ctx 루프에서 순회할 하드웨어
	 * 큐 포인터. */
	unsigned long i;
	/* [한국어] 선언: queue_for_each_hw_ctx 순회 인덱스. */

	if (!blk_queue_registered(q))
		/* [한국어] 조건: blk_queue_registered(q)는
		 * test_bit(QUEUE_FLAG_REGISTERED, &q->queue_flags)로 정의되며,
		 * 이 큐가 add_disk()를 거쳐 실제로 sysfs에 등록된 적이
		 * 있는지를 검사한다. 아직 등록되지 않았다면(예: 큐 생성 직후,
		 * 또는 이미 del_gendisk()로 완전히 내려간 뒤) hctx sysfs
		 * 노드도 존재하지 않는다. */
		return;
		/* [한국어] 조기 반환: 존재하지 않는 sysfs 노드를 지우려는
		 * 불필요한 시도를 막는다. */

	queue_for_each_hw_ctx(q, hctx, i)
		/* [한국어] queue_for_each_hw_ctx: 모든 하드웨어 큐를 순회하며
		 * 각각의 sysfs 노드만 제거한다(mq 루트 kobj 자체는 그대로
		 * 유지). */
		blk_mq_unregister_hctx(hctx);
		/* [한국어] blk_mq_unregister_hctx: 이 hctx와 그 아래 ctx들의
		 * kobject_del() 수행. */
}

/*
 * [한국어]
 * blk_mq_sysfs_register_hctxs - request_queue에 속한 hctx들을 sysfs에 다시
 * 등록
 *
 * @q: 대상 request_queue.
 * @return: 0 성공, 그 외 blk_mq_register_hctx()가 반환한 음수 errno.
 *
 * blk_mq_sysfs_unregister_hctxs()의 짝이 되는 함수로, hw 큐 개수 변경 등으로
 * 재구성된 hctx 배열을 다시 sysfs에 노출한다. 역시 blk_queue_registered(q)로
 * "이 큐가 gendisk에 등록되어 mq 루트가 이미 존재하는 상태인지"를 먼저
 * 확인하며, 등록되지 않은 상태면 새로 만들 mq 루트가 없으므로 곧바로 out으로
 * 건너뛰어 초기값 ret=0을 반환한다(할 일이 없다는 뜻이지 에러가 아니다).
 * 등록된 상태라면 모든 hctx에 대해 blk_mq_register_hctx()를 호출하고, 어느
 * hctx가 실패하면 그 자리에서 루프를 멈추고 그 에러 코드를 반환한다 — 이
 * 함수는 blk_mq_sysfs_register()와 달리 실패한 hctx 이전에 이미 등록된
 * hctx들을 롤백하지 않는다(호출자인 blk_mq_update_nr_hw_queues()가 더 상위
 * 수준에서 전체 실패를 어떻게 처리할지 결정하는 구조로 보인다, 추정).
 * 실행 컨텍스트: 프로세스 컨텍스트(hw 큐 개수 변경 경로).
 *
 * 호출 체인:
 *   blk_mq_update_nr_hw_queues()(blk-mq.c) ->
 *   [blk_mq_sysfs_register_hctxs] -> blk_mq_register_hctx() (hctx마다 반복)
 */
int blk_mq_sysfs_register_hctxs(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	/* [한국어] 선언: 아래 queue_for_each_hw_ctx 루프에서 순회할 하드웨어
	 * 큐 포인터. */
	unsigned long i;
	/* [한국어] 선언: queue_for_each_hw_ctx 순회 인덱스. */
	int ret = 0;
	/* [한국어] 선언+초기화: 이 함수의 반환값. 등록된 큐가 아니어서 아무
	 * 작업도 하지 않는 경우 이 0(성공)이 그대로 반환된다. */

	if (!blk_queue_registered(q))
		/* [한국어] 조건: blk_queue_registered(q) — 이 큐가 아직
		 * gendisk에 등록되어 있지 않다면(mq 루트 디렉터리 자체가
		 * 없다면) hctx sysfs를 새로 만들 대상이 없다. */
		goto out;
		/* [한국어] goto out: 아무 작업 없이 바로 반환 경로로 이동 —
		 * ret은 위에서 초기화한 0을 그대로 유지. */

	queue_for_each_hw_ctx(q, hctx, i) {
	/* [한국어] queue_for_each_hw_ctx: 모든 하드웨어 큐를 순회하며 하나씩
	 * 다시 등록을 시도한다. */
		ret = blk_mq_register_hctx(hctx);
		/* [한국어] 이 hctx와 그 아래 ctx들을 sysfs에 등록(위
		 * blk_mq_register_hctx() 참고). */
		if (ret)
			/* [한국어] 조건: 이 hctx 등록이 실패한 경우 — 더 이상
			 * 나머지 hctx를 시도하지 않고 즉시 루프를 빠져나간다. */
			break;
			/* [한국어] break: queue_for_each_hw_ctx 루프 탈출 —
			 * ret에는 실패한 hctx의 에러 코드가 남아 있다. */
	}

out:
	/* [한국어] 공통 반환 레이블: "등록 대상 없음"과 "루프 도중 실패"
	 * 두 경로 모두 여기로 모여 ret 값을 그대로 반환한다. */
	return ret;
	/* [한국어] 반환: 성공(0), 미등록 큐로 인한 조기 종료(0), 또는 실패한
	 * hctx의 에러 코드(음수) 중 하나. */
}
