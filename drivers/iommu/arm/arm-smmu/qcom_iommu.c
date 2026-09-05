// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU API for QCOM secure IOMMUs.  Somewhat based on arm-smmu.c
 *
 * Copyright (C) 2013 ARM Limited
 * Copyright (C) 2017 Red Hat
 */

/*
 * [한국어 설명] 퀄컴 보안 IOMMU 드라이버 (qcom_iommu.c)
 *
 * === 파일의 역할 ===
 * 위 영어 주석대로 arm-smmu.c 를 바탕으로 하되 따로 떨어져 나온
 * 드라이버다. 옛 퀄컴 SoC(msm-iommu-v1/v2)를 다루는데, 그 하드웨어는
 * 보안 세계가 상당 부분을 쥐고 있어 규격 드라이버로는 다룰 수 없다.
 *
 * 무엇이 다른가.
 *
 * 첫째, 스트림 매칭이 없다. 장치 트리가 장치마다 ASID 를 직접 적어 주고,
 * 그 ASID 가 곧 컨텍스트 뱅크 번호다. 그래서 SMR/S2CR 을 다루는 코드가
 * 통째로 없다.
 *
 * 둘째, 컨텍스트 뱅크가 각자 별도의 플랫폼 장치다. 장치 트리에서 IOMMU
 * 노드의 자식으로 나열되고, 그 각각을 qcom_iommu_ctx_driver 가 잡는다.
 *
 * 셋째, 보안 문맥이 있다. 그런 뱅크는 커널이 레지스터를 건드릴 수 없고,
 * 보안 세계가 설정을 관리한다. 커널은 SCM 호출로 "복원해 달라"고 부탁할
 * 뿐이다.
 *
 * 넷째, 보안 세계가 쓸 페이지 테이블 메모리를 커널이 잡아 준다. 그것이
 * qcom_iommu_sec_ptbl_init 이 하는 일이다.
 *
 * 전원 관리가 곳곳에 스며 있는 것도 특징이다. 원 주석이 여러 곳에서
 * 밝히듯, 해제는 클라이언트 장치가 꺼진 뒤에도 불릴 수 있어(GPU 나
 * dma-buf) 장치 링크에 기댈 수 없다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치 드라이버의 DMA API → iommu 코어 → qcom_iommu_ops
 *   → 이 파일 → io-pgtable(ARM 32비트 LPAE) → 페이지 테이블
 *
 * probe: IOMMU 노드 → qcom_iommu_device_probe → 자식 노드 채우기
 *   → qcom_iommu_ctx_probe(뱅크마다) → 코어에 등록
 *
 * 실행 컨텍스트: 대부분 프로세스 문맥. 매핑 경로가 스핀락 아래에서
 * 돌아 원자적 할당을 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * 위: iommu 코어.
 * 옆: qcom_scm — 보안 세계 호출.
 * 아래: io-pgtable 의 ARM 32비트 LPAE 구현, arm-smmu.h 의 레지스터 정의.
 *
 * === 주요 함수/구조체 요약 ===
 * struct qcom_iommu_dev: IOMMU 하나. ASID 를 첨자로 하는 뱅크 배열을 든다.
 * struct qcom_iommu_ctx: 뱅크 하나. 별도의 플랫폼 장치다.
 * qcom_iommu_init_domain: 도메인의 표를 만들고 관련된 모든 뱅크를 세운다.
 * qcom_iommu_tlb_* : 한 도메인이 여러 뱅크에 걸쳐 있어 모두 훑는다.
 * qcom_iommu_of_xlate: 장치 트리의 ASID 를 받아 검증한다.
 * qcom_iommu_sec_ptbl_init: 보안 세계가 쓸 표 메모리를 마련한다.
 */
#include <linux/atomic.h>	/* [한국어] 원자 연산. */
#include <linux/bitfield.h>	/* [한국어] FIELD_PREP — 레지스터 필드를 다룬다. */
#include <linux/clk.h>	/* [한국어] 클럭을 켜고 끈다. */
#include <linux/delay.h>	/* [한국어] 지연 대기. */
#include <linux/dma-mapping.h>	/* [한국어] 보안 세계가 쓸 표 메모리를 잡는다. */
#include <linux/err.h>	/* [한국어] 오류 포인터 관용구. */
#include <linux/interrupt.h>	/* [한국어] 뱅크마다의 오류 인터럽트. */
#include <linux/io.h>	/* [한국어] MMIO 접근. */
#include <linux/io-64-nonatomic-hi-lo.h>	/* [한국어] 64비트 레지스터를 32비트씩 나눠 접근한다. */
#include <linux/io-pgtable.h>	/* [한국어] ARM 32비트 LPAE 페이지 테이블 구현. */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 도메인과 연산표. */
#include <linux/iopoll.h>	/* [한국어] readl_poll_timeout — 무효화 완료를 기다린다. */
#include <linux/kconfig.h>	/* [한국어] 설정 매크로. */
#include <linux/init.h>	/* [한국어] 초기화 표시자. */
#include <linux/mutex.h>	/* [한국어] 도메인 초기화를 직렬화한다. */
#include <linux/of.h>	/* [한국어] 장치 트리 속성. */
#include <linux/of_platform.h>	/* [한국어] 자식 노드를 장치로 만든다. */
#include <linux/platform_device.h>	/* [한국어] 플랫폼 드라이버 등록. */
#include <linux/pm.h>	/* [한국어] 전원 관리 연산표. */
#include <linux/pm_runtime.h>	/* [한국어] 실행 중 전원 관리. 이 파일 곳곳에 스며 있다. */
#include <linux/firmware/qcom/qcom_scm.h>	/* [한국어] 보안 세계 호출. 이 드라이버의 핵심 의존이다. */
#include <linux/slab.h>	/* [한국어] kzalloc / kfree. */
#include <linux/spinlock.h>	/* [한국어] 표 조작을 직렬화한다. */

#include "arm-smmu.h"	/* [한국어] 레지스터 정의를 규격 드라이버와 함께 쓴다. */

#define SMMU_INTR_SEL_NS     0x2000	/* [한국어] 인터럽트를 보안·비보안 중 어디로 보낼지 정하는 레지스터. 모두 1 을 쓰면 비보안 쪽으로 간다. */

/* [한국어] 이 IOMMU 가 받는 클럭들. 한 번에 켜고 끈다. */
enum qcom_iommu_clk {
	CLK_IFACE,
	/* [한국어] 인터페이스 클럭 — 레지스터에 접근하려면 이것이 켜져 있어야 한다.
	 * 설정자: 프로브가 devm_clk_bulk_get() 으로 장치 트리에서 얻어 이 첨자 자리에 담는다.
	 * 읽는 자: 컨트롤러를 켜고 끌 때 clk_bulk_prepare_enable/disable 이 배열 전체를
	 *   한 번에 다룬다 — 개별로 켜고 끄는 자리는 없다.
	 * 이 클럭이 꺼진 채 레지스터를 읽으면 버스가 멈추거나 쓰레기가 읽힌다.
	 *   그래서 어떤 레지스터 접근보다도 먼저 켜져야 하는 클럭이다.
	 * 이 enum 은 값이 아니라 clks 배열의 첨자다. 순서를 바꾸면 장치 트리에서
	 *   읽어 온 클럭과 이름이 어긋난다. */
	CLK_BUS,
	/* [한국어] 버스 클럭 — 하드웨어가 페이지 테이블을 읽으려면 필요하다.
	 * 설정자: 프로브가 devm_clk_bulk_get() 으로 장치 트리에서 얻어 이 첨자 자리에 담는다.
	 * 읽는 자: 컨트롤러를 켜고 끌 때 clk_bulk_prepare_enable/disable 이 배열 전체를
	 *   한 번에 다룬다 — 개별로 켜고 끄는 자리는 없다.
	 * 위 iface 가 CPU 쪽 접근을 위한 것이라면, 이쪽은 IOMMU 자신이 메모리를
	 *   향해 나가는 경로를 위한 것이다. 둘 다 켜져야 변환이 동작한다.
	 * 이 enum 은 값이 아니라 clks 배열의 첨자다. 순서를 바꾸면 장치 트리에서
	 *   읽어 온 클럭과 이름이 어긋난다. */
	CLK_TBU,
	/* [한국어] 변환 버퍼 유닛(TBU) 클럭 — 없는 SoC 도 있다.
	 * 설정자: 프로브가 devm_clk_bulk_get() 으로 장치 트리에서 얻어 이 첨자 자리에 담는다.
	 * 읽는 자: 컨트롤러를 켜고 끌 때 clk_bulk_prepare_enable/disable 이 배열 전체를
	 *   한 번에 다룬다 — 개별로 켜고 끄는 자리는 없다.
	 * TBU 는 SMMU 본체에서 떨어져 나와 마스터 가까이에 놓인 TLB 다. 이것을
	 *   따로 두지 않는 SoC 에서는 이 클럭이 장치 트리에 없고, clk_bulk 가
	 *   선택적 클럭으로 다뤄 NULL 을 담는다.
	 * 이 enum 은 값이 아니라 clks 배열의 첨자다. 순서를 바꾸면 장치 트리에서
	 *   읽어 온 클럭과 이름이 어긋난다. */
	CLK_NUM,
	/* [한국어] 클럭의 개수 — 배열 크기이자 순회 상한이다.
	 * 설정자: enum 의 마지막 값이라 컴파일러가 자동으로 정한다.
	 * 읽는 자: clks 배열의 선언과 clk_bulk_* 호출에 넘기는 개수.
	 * 왜 이 관용구를 쓰는가: 클럭을 하나 추가하면 위에 이름을 넣기만 하면 되고
	 *   배열 크기와 개수가 자동으로 따라간다. 숫자를 따로 적으면 둘이 어긋나
	 *   배열 밖을 다루게 된다.
	 * 값 범위: 현재 3. */
};

struct qcom_iommu_ctx;	/* [한국어] 앞선 구조체가 이것을 포인터로 참조해 미리 선언해 둔다. */

/*
 * [한국어] IOMMU 하나.
 *
 * 컨텍스트 뱅크를 ASID 첨자의 배열로 든다. 스트림 매칭이 없어 장치가
 * 어느 뱅크를 쓸지 장치 트리가 직접 정하고, 그 번호가 곧 ASID 다.
 */
struct qcom_iommu_dev {
	/* IOMMU core code handle */
	/* [한국어] iommu 코어에 등록하는 손잡이.
	 *  읽는 자: 코어가 이것으로 드라이버를 되짚는다. */
	struct iommu_device	 iommu;
	/* [한국어] 이 IOMMU 의 플랫폼 장치. */
	struct device		*dev;
	/* [한국어] 받는 클럭들. 한 번에 켜고 끈다.
	 *  설정자: probe. 읽는 자: 전원 관리 콜백. */
	struct clk_bulk_data clks[CLK_NUM];
	/* [한국어] 전역 레지스터 창(없을 수 있다).
	 *  인터럽트를 비보안 쪽으로 보내는 설정에만 쓴다. */
	void __iomem		*local_base;
	/* [한국어] 보안 세계가 이 IOMMU 를 가리키는 번호.
	 *  설정자: probe 가 장치 트리에서 읽는다.
	 *  읽는 자: SCM 호출마다 이 값을 넘긴다. */
	u32			 sec_id;
	/* [한국어] 가장 큰 ASID. 아래 배열의 상한이다.
	 *  설정자: probe 가 자식 노드를 훑어 정한다.
	 *  읽는 자: of_xlate 가 범위를 검증할 때. */
	u8			 max_asid;
	/* [한국어] (위 영어 주석 참고) ASID 를 첨자로 하는 뱅크 배열.
	 *  설정자: 뱅크마다의 ctx_probe 가 자기를 넣는다.
	 *  읽는 자: to_ctx 가 ASID 로 뱅크를 찾을 때.
	 *  가변 길이 배열이라 구조체와 함께 잡는다. */
	struct qcom_iommu_ctx	*ctxs[];   /* indexed by asid */
};

/*
 * [한국어] 컨텍스트 뱅크 하나. 별도의 플랫폼 장치다.
 *
 * 장치 트리에서 IOMMU 노드의 자식으로 나열되고, 각자 레지스터 창과
 * 인터럽트를 갖는다.
 */
struct qcom_iommu_ctx {
	/* [한국어] 이 뱅크의 플랫폼 장치. 로그와 인터럽트 등록에 쓴다. */
	struct device		*dev;
	/* [한국어] 이 뱅크의 레지스터 창.
	 *  뱅크마다 창이 따로라 페이지 번호 계산이 없다. */
	void __iomem		*base;
	/* [한국어] 보안 세계에 설정 복원을 부탁했는가.
	 *  설정자·읽는 자: init_domain. 한 번만 하면 된다. */
	bool			 secure_init;
	/* [한국어] 보안 세계가 관리하는 문맥인가.
	 *  설정자: ctx_probe 가 compatible 로 정한다.
	 *  참이면 커널이 레지스터를 건드릴 수 없어 프로그래밍을 건너뛴다. */
	bool			 secured_ctx;
	/* [한국어] (위 영어 주석 참고) 이 뱅크의 ASID.
	 *  설정자: ctx_probe 가 노드 주소에서 계산한다.
	 *  뱅크 번호와 1:1 이라 배열 첨자로도 쓰인다. */
	u8			 asid;      /* asid and ctx bank # are 1:1 */
	/* [한국어] 지금 이 뱅크가 속한 도메인.
	 *  설정자: init_domain 과 identity_attach.
	 *  읽는 자: 오류 처리기가 상위 계층에 알릴 때. */
	struct iommu_domain	*domain;
};

/*
 * [한국어] 한 주소 공간.
 *
 * 한 도메인이 여러 뱅크에 걸칠 수 있다 — 장치가 여러 ASID 를 쓰면
 * 그 모두를 같은 표로 세운다. 그래서 fwspec 을 들고 있다.
 */
struct qcom_iommu_domain {
	/* [한국어] 페이지 테이블 조작 함수들.
	 *  설정자: init_domain 이 마지막에 공개한다.
	 *  NULL 이면 아직 세워지지 않은 도메인이다. */
	struct io_pgtable_ops	*pgtbl_ops;
	/* [한국어] 표 조작을 직렬화하는 스핀락.
	 *  매핑 경로가 인터럽트 문맥에서도 불릴 수 있어 스핀락을 쓴다.
	 *  그 탓에 표 할당이 원자적이어야 한다. */
	spinlock_t		 pgtbl_lock;
	/* [한국어] (위 영어 주석 참고) iommu 포인터를 지킨다.
	 *  첫 붙임 한 번만 직렬화하면 되어 뮤텍스로 충분하다. */
	struct mutex		 init_mutex; /* Protects iommu pointer */
	/* [한국어] 코어가 보는 도메인. */
	struct iommu_domain	 domain;
	/* [한국어] 이 도메인이 매인 IOMMU.
	 *  NULL 이면 아직 어느 장치도 붙지 않았다는 뜻이다. */
	struct qcom_iommu_dev	*iommu;
	/* [한국어] 이 도메인이 쓰는 ASID 목록.
	 *  설정자: init_domain. 읽는 자: 무효화 함수들이 뱅크를 훑을 때.
	 *  한 도메인이 여러 뱅크에 걸칠 수 있어 필요하다. */
	struct iommu_fwspec	*fwspec;
};

/*
 * [한국어]
 * to_qcom_iommu_domain - 코어 도메인에서 이 드라이버의 도메인으로 되짚는다
 *
 * @dom: 코어가 준 도메인.
 * @return: 그것을 품은 구조체.
 */
static struct qcom_iommu_domain *to_qcom_iommu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct qcom_iommu_domain, domain);	/* [한국어] 코어 도메인을 품은 구조체로 되짚는다. */
}

static const struct iommu_ops qcom_iommu_ops;	/* [한국어] 아래에서 정의하는 연산표의 전방 선언. */

/*
 * [한국어]
 * to_ctx - ASID 로 컨텍스트 뱅크를 찾는다
 *
 * @d: 대상 도메인.
 * @asid: 찾는 ASID.
 * @return: 그 뱅크, 아직 IOMMU 가 정해지지 않았으면 NULL.
 *
 * ASID 가 곧 배열 첨자다 — 스트림 매칭이 없는 이 하드웨어의 단순함이
 * 여기서 드러난다.
 */
static struct qcom_iommu_ctx * to_ctx(struct qcom_iommu_domain *d, unsigned asid)
{
	struct qcom_iommu_dev *qcom_iommu = d->iommu;	/* [한국어] 이 도메인이 매인 IOMMU. */
	if (!qcom_iommu)	/* [한국어] 아직 정해지지 않았으면 */
		return NULL;	/* [한국어] 뱅크도 없다. */
	return qcom_iommu->ctxs[asid];	/* [한국어] ASID 가 곧 배열 첨자다. */
}

static inline void	/* [한국어] 반환형이 다음 줄의 함수 이름과 나뉘어 있다. */
/*
 * [한국어]
 * iommu_writel - 뱅크 레지스터에 32비트로 쓴다
 *
 * @ctx: 대상 뱅크.
 * @reg: 그 안의 오프셋.
 * @val: 쓸 값.
 *
 * 뱅크마다 레지스터 창이 따로라 페이지 번호 계산이 필요 없다.
 */
iommu_writel(struct qcom_iommu_ctx *ctx, unsigned reg, u32 val)
{
	writel_relaxed(val, ctx->base + reg);	/* [한국어] 뱅크마다 레지스터 창이 따로라 오프셋만 더하면 된다. */
}

static inline void	/* [한국어] 반환형이 다음 줄의 함수 이름과 나뉘어 있다. */
/*
 * [한국어]
 * iommu_writeq - 뱅크 레지스터에 64비트로 쓴다
 *
 * @ctx: 대상 뱅크.
 * @reg: 오프셋.
 * @val: 쓸 값.
 */
iommu_writeq(struct qcom_iommu_ctx *ctx, unsigned reg, u64 val)
{
	writeq_relaxed(val, ctx->base + reg);	/* [한국어] TTBR 처럼 64비트인 레지스터에 쓴다. */
}

static inline u32	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iommu_readl - 뱅크 레지스터를 32비트로 읽는다
 *
 * @ctx: 대상 뱅크.
 * @reg: 오프셋.
 * @return: 읽은 값.
 */
iommu_readl(struct qcom_iommu_ctx *ctx, unsigned reg)
{
	return readl_relaxed(ctx->base + reg);	/* [한국어] 32비트로 읽는다. */
}

static inline u64	/* [한국어] 반환형이 다음 줄과 나뉘어 있다. */
/*
 * [한국어]
 * iommu_readq - 뱅크 레지스터를 64비트로 읽는다
 *
 * @ctx: 대상 뱅크.
 * @reg: 오프셋.
 * @return: 읽은 값.
 */
iommu_readq(struct qcom_iommu_ctx *ctx, unsigned reg)
{
	return readq_relaxed(ctx->base + reg);	/* [한국어] 64비트로 읽는다. */
}

/*
 * [한국어]
 * qcom_iommu_tlb_sync - 이 도메인의 모든 뱅크에서 무효화 완료를 기다린다
 *
 * @cookie: io-pgtable 에 등록해 둔 도메인.
 *
 * 한 도메인이 여러 뱅크에 걸치므로 모두 훑어야 한다.
 *
 * 5초까지 기다리는 것이 눈에 띈다 — 규격 드라이버의 1초보다 훨씬 길다.
 * 보안 세계가 끼어 있어 응답이 느릴 수 있기 때문이다.
 */
static void qcom_iommu_tlb_sync(void *cookie)
{
	struct qcom_iommu_domain *qcom_domain = cookie;	/* [한국어] io-pgtable 에 등록해 둔 도메인. */
	struct iommu_fwspec *fwspec = qcom_domain->fwspec;	/* [한국어] 이 도메인이 쓰는 ASID 목록. */
	unsigned i;	/* [한국어] 순회 첨자. */

	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 한 도메인이 여러 뱅크에 걸쳐 모두 훑는다. */
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);	/* [한국어] 그 ASID 의 뱅크. */
		unsigned int val, ret;	/* [한국어] 읽은 값과 결과. */

		iommu_writel(ctx, ARM_SMMU_CB_TLBSYNC, 0);	/* [한국어] 동기화를 시작시킨다. */

		ret = readl_poll_timeout(ctx->base + ARM_SMMU_CB_TLBSTATUS, val,	/* [한국어] 상태 비트가 내려갈 때까지 */
					 (val & 0x1) == 0, 0, 5000000);	/* [한국어] 5초까지 기다린다. 규격 드라이버보다 긴 것은 보안 세계가 끼어 응답이 느릴 수 있어서다. */
		if (ret)	/* [한국어] 시간이 다했으면 */
			dev_err(ctx->dev, "timeout waiting for TLB SYNC\n");	/* [한국어] 알린다. 더 할 수 있는 일이 없다. */
	}
}

/*
 * [한국어]
 * qcom_iommu_tlb_inv_context - 이 도메인의 모든 뱅크를 통째로 무효화한다
 *
 * @cookie: 대상 도메인.
 *
 * 뱅크마다 자기 ASID 로 비운 뒤 한 번에 기다린다.
 */
static void qcom_iommu_tlb_inv_context(void *cookie)
{
	struct qcom_iommu_domain *qcom_domain = cookie;	/* [한국어] 대상 도메인. */
	struct iommu_fwspec *fwspec = qcom_domain->fwspec;	/* [한국어] ASID 목록. */
	unsigned i;	/* [한국어] 순회 첨자. */

	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 모든 뱅크에서 */
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);	/* [한국어] 그 뱅크를 찾아 */
		iommu_writel(ctx, ARM_SMMU_CB_S1_TLBIASID, ctx->asid);	/* [한국어] 자기 ASID 를 통째로 비운다. */
	}

	qcom_iommu_tlb_sync(cookie);	/* [한국어] 모두 끝나기를 한 번에 기다린다. */
}

/*
 * [한국어]
 * qcom_iommu_tlb_inv_range_nosync - 구간을 무효화하되 기다리지 않는다
 *
 * @iova: 시작 주소.
 * @size: 길이.
 * @granule: 비울 단위.
 * @leaf: 마지막 단계만 비울 것인가.
 * @cookie: 대상 도메인.
 *
 * 32비트 형식이라 주소의 하위 12비트 자리에 ASID 를 넣는다. 페이지
 * 정렬이라 그 자리가 비어 있다.
 *
 * 뱅크마다 크기를 새로 세는 데 주의 — 안쪽 고리가 size 를 깎으므로
 * 지역 변수에 복사해 쓴다.
 */
static void qcom_iommu_tlb_inv_range_nosync(unsigned long iova, size_t size,
					    size_t granule, bool leaf, void *cookie)
{
	struct qcom_iommu_domain *qcom_domain = cookie;	/* [한국어] 대상 도메인. */
	struct iommu_fwspec *fwspec = qcom_domain->fwspec;	/* [한국어] ASID 목록. */
	unsigned i, reg;	/* [한국어] 순회 첨자와 쓸 레지스터. */

	reg = leaf ? ARM_SMMU_CB_S1_TLBIVAL : ARM_SMMU_CB_S1_TLBIVA;	/* [한국어] 잎만 비울지 표 순회 캐시까지 비울지에 따라 다른 레지스터를 쓴다. */

	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 모든 뱅크에서 */
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);	/* [한국어] 그 뱅크를 찾아 */
		size_t s = size;	/* [한국어] 안쪽 고리가 깎으므로 뱅크마다 새로 센다. */

		iova = (iova >> 12) << 12;	/* [한국어] 하위 12비트를 지워 */
		iova |= ctx->asid;	/* [한국어] 그 자리에 ASID 를 넣는다. 32비트 형식의 방식이다. */
		do {
			iommu_writel(ctx, reg, iova);	/* [한국어] 한 페이지씩 무효화한다. */
			iova += granule;	/* [한국어] 다음 페이지로. */
		} while (s -= granule);	/* [한국어] 구간을 다 덮을 때까지. */
	}
}

/*
 * [한국어]
 * qcom_iommu_tlb_flush_walk - 표 순회 캐시까지 비우고 기다린다
 *
 * @iova: 시작 주소.
 * @size: 길이.
 * @granule: 비울 단위.
 * @cookie: 대상 도메인.
 */
static void qcom_iommu_tlb_flush_walk(unsigned long iova, size_t size,
				      size_t granule, void *cookie)
{
	qcom_iommu_tlb_inv_range_nosync(iova, size, granule, false, cookie);	/* [한국어] 표 순회 캐시까지 비운다. */
	qcom_iommu_tlb_sync(cookie);	/* [한국어] 끝나기를 기다린다. */
}

/*
 * [한국어]
 * qcom_iommu_tlb_add_page - 잎 페이지 하나를 무효화한다
 *
 * @gather: 무효화 목록(쓰지 않는다).
 * @iova: 그 주소.
 * @granule: 그 크기.
 * @cookie: 대상 도메인.
 *
 * 기다리지 않는다 — 완료 대기는 나중에 한 번만 한다.
 */
static void qcom_iommu_tlb_add_page(struct iommu_iotlb_gather *gather,
				    unsigned long iova, size_t granule,
				    void *cookie)
{
	qcom_iommu_tlb_inv_range_nosync(iova, granule, granule, true, cookie);	/* [한국어] 잎 항목만 비운다. 기다림은 나중에 한 번만 한다. */
}

/*
 * [한국어] io-pgtable 이 표를 고칠 때 부를 무효화 함수표.
 */
static const struct iommu_flush_ops qcom_flush_ops = {
	.tlb_flush_all	= qcom_iommu_tlb_inv_context,	/* [한국어] ASID 통째로 비운다. */
	.tlb_flush_walk = qcom_iommu_tlb_flush_walk,	/* [한국어] 표 순회 캐시까지 비운다. */
	.tlb_add_page	= qcom_iommu_tlb_add_page,	/* [한국어] 잎 항목만 비운다. */
};

/*
 * [한국어]
 * qcom_iommu_fault - 뱅크의 오류 인터럽트 처리기
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev: 등록할 때 넘긴 뱅크.
 * @return: IRQ_HANDLED 또는 IRQ_NONE.
 *
 * 규격 드라이버와 달리 뱅크마다 인터럽트가 따로 등록되어, 어느 뱅크인지
 * 곧바로 안다.
 *
 * 마지막에 늘 트랜잭션을 끝내는 데 주의. 이 드라이버는 폴트를 되살리는
 * 길을 제공하지 않는다.
 */
static irqreturn_t qcom_iommu_fault(int irq, void *dev)
{
	struct qcom_iommu_ctx *ctx = dev;	/* [한국어] 등록할 때 넘긴 뱅크. */
	u32 fsr, fsynr;	/* [한국어] 오류 상태와 부가 정보. */
	u64 iova;	/* [한국어] 오류가 난 주소. */

	fsr = iommu_readl(ctx, ARM_SMMU_CB_FSR);	/* [한국어] 오류 상태를 읽는다. */

	if (!(fsr & ARM_SMMU_CB_FSR_FAULT))	/* [한국어] 오류 비트가 없으면 */
		return IRQ_NONE;	/* [한국어] 우리 것이 아니다. */

	fsynr = iommu_readl(ctx, ARM_SMMU_CB_FSYNR0);	/* [한국어] 부가 정보. */
	iova = iommu_readq(ctx, ARM_SMMU_CB_FAR);	/* [한국어] 오류가 난 주소. */

	if (!report_iommu_fault(ctx->domain, ctx->dev, iova, 0)) {	/* [한국어] 상위 계층이 다루지 못했으면 */
		dev_err_ratelimited(ctx->dev,	/* [한국어] 로그로 찍는다. */
				    "Unhandled context fault: fsr=0x%x, "
				    "iova=0x%016llx, fsynr=0x%x, cb=%d\n",
				    fsr, iova, fsynr, ctx->asid);	/* [한국어] ASID 가 곧 뱅크 번호라 어느 문맥인지 알 수 있다. */
	}

	iommu_writel(ctx, ARM_SMMU_CB_FSR, fsr);	/* [한국어] 읽은 값을 되써서 지운다. */
	iommu_writel(ctx, ARM_SMMU_CB_RESUME, ARM_SMMU_RESUME_TERMINATE);	/* [한국어] 늘 끝낸다 — 이 드라이버는 폴트를 되살리는 길을 주지 않는다. */

	return IRQ_HANDLED;	/* [한국어] 처리했다. */
}

/*
 * [한국어]
 * qcom_iommu_init_domain - 표를 만들고 관련된 모든 뱅크를 세운다
 *
 * @domain: 세울 도메인.
 * @qcom_iommu: 그 IOMMU.
 * @dev: 첫 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 첫 장치를 붙일 때 한 번만 돈다.
 *
 * 주소 폭이 32/40 으로 고정인 것이 눈에 띈다 — 이 하드웨어는 그 형식
 * 하나만 쓴다. 규격 드라이버처럼 능력을 살펴 고를 것이 없다.
 *
 * 뱅크마다 보안 초기화를 먼저 부른다. 보안 세계가 그 뱅크의 기본 설정을
 * 복원해 주어야 커널이 이어서 프로그래밍할 수 있다.
 *
 * 보안 문맥은 커널이 건드릴 수 없어 도메인만 연결하고 넘어간다.
 *
 * 멈춰 세우기(CFCFG)를 늘 켜는 것도 규격 드라이버와 다르다 — 폴트를
 * 처리할 기회를 주려는 것이다.
 */
static int qcom_iommu_init_domain(struct iommu_domain *domain,
				  struct qcom_iommu_dev *qcom_iommu,
				  struct device *dev)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] 이 장치가 쓰는 ASID 목록. */
	struct io_pgtable_ops *pgtbl_ops;	/* [한국어] 만들 표 조작 함수들. */
	struct io_pgtable_cfg pgtbl_cfg;	/* [한국어] 표를 만들 설정. */
	int i, ret = 0;	/* [한국어] 순회 첨자와 결과. */
	u32 reg;	/* [한국어] 제어 레지스터 값. */

	mutex_lock(&qcom_domain->init_mutex);	/* [한국어] 첫 붙임을 직렬화한다. */
	if (qcom_domain->iommu)	/* [한국어] 이미 세워졌으면 */
		goto out_unlock;	/* [한국어] 할 일이 없다. */

	pgtbl_cfg = (struct io_pgtable_cfg) {	/* [한국어] 표를 만들 설정을 한 번에 짓는다. */
		.pgsize_bitmap	= domain->pgsize_bitmap,	/* [한국어] 4KB 하나뿐이다. */
		.ias		= 32,	/* [한국어] 입력 주소 32비트. */
		.oas		= 40,	/* [한국어] 출력 40비트. 이 하드웨어는 이 형식 하나만 쓴다. */
		.tlb		= &qcom_flush_ops,	/* [한국어] 표를 고칠 때 부를 무효화 함수들. */
		.iommu_dev	= qcom_iommu->dev,	/* [한국어] 표를 담을 메모리를 어느 장치 앞으로 잡을지. */
	};

	qcom_domain->iommu = qcom_iommu;	/* [한국어] 이 순간 도메인이 이 IOMMU 에 매인다. */
	qcom_domain->fwspec = fwspec;	/* [한국어] ASID 목록을 기억한다. 무효화가 이것으로 뱅크를 훑는다. */

	pgtbl_ops = alloc_io_pgtable_ops(ARM_32_LPAE_S1, &pgtbl_cfg, qcom_domain);	/* [한국어] 32비트 긴 서술자 표를 만든다. */
	if (!pgtbl_ops) {	/* [한국어] 만들지 못했으면 */
		dev_err(qcom_iommu->dev, "failed to allocate pagetable ops\n");	/* [한국어] 알리고 */
		ret = -ENOMEM;	/* [한국어] 실패로 */
		goto out_clear_iommu;	/* [한국어] 매인 것을 되돌린다. */
	}

	domain->geometry.aperture_end = (1ULL << pgtbl_cfg.ias) - 1;	/* [한국어] 쓸 수 있는 주소 범위. */
	domain->geometry.force_aperture = true;	/* [한국어] 그 밖의 매핑을 코어가 거절하게 한다. */

	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 이 장치가 쓰는 뱅크마다 */
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);	/* [한국어] 그 뱅크를 찾는다. */

		if (!ctx->secure_init) {	/* [한국어] 보안 초기화를 아직 안 했으면 */
			ret = qcom_scm_restore_sec_cfg(qcom_iommu->sec_id, ctx->asid);	/* [한국어] 보안 세계에 그 뱅크의 기본 설정 복원을 부탁한다. */
			if (ret) {	/* [한국어] 앞선 걸음이 실패했다. */
				dev_err(qcom_iommu->dev, "secure init failed: %d\n", ret);	/* [한국어] 실패하면 알리고 */
				goto out_clear_iommu;	/* [한국어] 되돌린다. */
			}
			ctx->secure_init = true;	/* [한국어] 한 번만 하면 된다. */
		}

		/* Secured QSMMU-500/QSMMU-v2 contexts cannot be programmed */
		if (ctx->secured_ctx) {	/* [한국어] 보안 문맥이면 */
			ctx->domain = domain;	/* [한국어] 도메인만 연결하고 */
			continue;	/* [한국어] 레지스터는 건드리지 않는다 — 보안 세계가 관리한다. */
		}

		/* Disable context bank before programming */
		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, 0);	/* [한국어] 프로그래밍 전에 뱅크를 끈다. */

		/* Clear context bank fault address fault status registers */
		iommu_writel(ctx, ARM_SMMU_CB_FAR, 0);	/* [한국어] 오류 주소를 지우고 */
		iommu_writel(ctx, ARM_SMMU_CB_FSR, ARM_SMMU_CB_FSR_FAULT);	/* [한국어] 오류 상태도 지운다. */

		/* TTBRs */
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR0,	/* [한국어] 표 주소와 */
				pgtbl_cfg.arm_lpae_s1_cfg.ttbr |
				FIELD_PREP(ARM_SMMU_TTBRn_ASID, ctx->asid));	/* [한국어] ASID 를 한 레지스터에 함께 담는다. */
		iommu_writeq(ctx, ARM_SMMU_CB_TTBR1, 0);	/* [한국어] 상위 주소 공간은 쓰지 않는다. */

		/* TCR */
		iommu_writel(ctx, ARM_SMMU_CB_TCR2,	/* [한국어] 변환 제어의 상위 부분. */
				arm_smmu_lpae_tcr2(&pgtbl_cfg));
		iommu_writel(ctx, ARM_SMMU_CB_TCR,	/* [한국어] TCR 에 확장 주소 비트를 더해 쓴다 — 32비트 형식에서 긴 서술자를 고르는 비트다. */
			     arm_smmu_lpae_tcr(&pgtbl_cfg) | ARM_SMMU_TCR_EAE);

		/* MAIRs (stage-1 only) */
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR0,	/* [한국어] 속성 표 하위. */
				pgtbl_cfg.arm_lpae_s1_cfg.mair);
		iommu_writel(ctx, ARM_SMMU_CB_S1_MAIR1,	/* [한국어] 상위. 64비트 값을 둘로 나눠 쓴다. */
				pgtbl_cfg.arm_lpae_s1_cfg.mair >> 32);

		/* SCTLR */
		reg = ARM_SMMU_SCTLR_CFIE | ARM_SMMU_SCTLR_CFRE |	/* [한국어] 오류 보고와 인터럽트를 켜고, */
		      ARM_SMMU_SCTLR_AFE | ARM_SMMU_SCTLR_TRE |	/* [한국어] 접근 플래그와 TEX 재매핑을 쓰고, */
		      ARM_SMMU_SCTLR_M | ARM_SMMU_SCTLR_S1_ASIDPNE |	/* [한국어] 변환을 켜고 ASID 를 뱅크마다 따로 쓰며, */
		      ARM_SMMU_SCTLR_CFCFG;	/* [한국어] 폴트 때 멈춰 세운다 — 처리할 기회를 주려는 것이다. */

		if (IS_ENABLED(CONFIG_CPU_BIG_ENDIAN))	/* [한국어] 빅 엔디언 커널이면 */
			reg |= ARM_SMMU_SCTLR_E;	/* [한국어] 표도 그 순서로 읽게 한다. */

		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, reg);	/* [한국어] 마지막에 쓴다 — 이 순간 뱅크가 살아난다. */

		ctx->domain = domain;	/* [한국어] 이 뱅크가 어느 도메인에 속하는지 기록한다. 오류 처리기가 쓴다. */
	}

	mutex_unlock(&qcom_domain->init_mutex);	/* [한국어] 락을 놓는다. */

	/* Publish page table ops for map/unmap */
	qcom_domain->pgtbl_ops = pgtbl_ops;	/* [한국어] 락 밖에서 공개한다 — 이 포인터가 보이는 순간 매핑 경로가 열린다. */

	return 0;	/* [한국어] 성공. */

out_clear_iommu:	/* [한국어] 표를 만들지 못한 경로. */
	qcom_domain->iommu = NULL;	/* [한국어] 매인 적 없는 상태로 되돌린다. */
out_unlock:	/* [한국어] 모든 경로가 합류한다. */
	mutex_unlock(&qcom_domain->init_mutex);	/* [한국어] 락을 놓고 */
	return ret;	/* [한국어] 결과를 올린다. */
}

/*
 * [한국어]
 * qcom_iommu_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 요청한 장치(쓰지 않는다).
 * @return: 만든 도메인, 실패하면 NULL.
 *
 * 원 주석대로 실제 자원은 첫 장치를 붙일 때 잡는다.
 *
 * 페이지 크기를 4KB 하나로 고정한다 — 이 하드웨어는 그것만 쓴다.
 */
static struct iommu_domain *qcom_iommu_domain_alloc_paging(struct device *dev)
{
	struct qcom_iommu_domain *qcom_domain;	/* [한국어] 만들 도메인. */

	/*
	 * Allocate the domain and initialise some of its data structures.
	 * We can't really do anything meaningful until we've added a
	 * master.
	 */
	qcom_domain = kzalloc_obj(*qcom_domain);	/* [한국어] 구조체를 잡는다. */
	if (!qcom_domain)	/* [한국어] 메모리가 없다. */
		return NULL;	/* [한국어] 실패. */

	mutex_init(&qcom_domain->init_mutex);	/* [한국어] 첫 붙임을 직렬화할 뮤텍스. */
	spin_lock_init(&qcom_domain->pgtbl_lock);	/* [한국어] 표 조작을 직렬화할 스핀락. */
	qcom_domain->domain.pgsize_bitmap = SZ_4K;	/* [한국어] 이 하드웨어는 4KB 하나뿐이다. */

	return &qcom_domain->domain;	/* [한국어] 코어가 보는 도메인. */
}

/*
 * [한국어]
 * qcom_iommu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 *
 * 원 주석이 전원을 켜는 이유를 밝힌다 — 해제는 클라이언트 장치가 꺼진
 * 뒤에도 불릴 수 있다(GPU 나 dma-buf). 그래서 장치 링크에 기댈 수 없고,
 * 무효화 경로가 클럭 없이 레지스터를 건드리지 않게 손수 켠다.
 */
static void qcom_iommu_domain_free(struct iommu_domain *domain)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */

	if (qcom_domain->iommu) {	/* [한국어] 세워진 적이 있으면 */
		/*
		 * NOTE: unmap can be called after client device is powered
		 * off, for example, with GPUs or anything involving dma-buf.
		 * So we cannot rely on the device_link.  Make sure the IOMMU
		 * is on to avoid unclocked accesses in the TLB inv path:
		 */
		pm_runtime_get_sync(qcom_domain->iommu->dev);	/* [한국어] 원 주석대로 전원을 손수 켠다 — 표 해제가 무효화를 부르고, 그것이 레지스터를 건드린다. */
		free_io_pgtable_ops(qcom_domain->pgtbl_ops);	/* [한국어] 표를 해제한다. */
		pm_runtime_put_sync(qcom_domain->iommu->dev);	/* [한국어] 전원 참조를 놓는다. */
	}

	kfree(qcom_domain);	/* [한국어] 구조체를 해제한다. */
}

/*
 * [한국어]
 * qcom_iommu_attach_dev - 장치를 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 붙는 장치.
 * @old: 옛 도메인(쓰지 않는다).
 * @return: 0 성공, 음수면 실패.
 *
 * 이 하드웨어는 스트림 매칭이 없어, 붙이기가 곧 "그 ASID 의 뱅크를
 * 이 표로 세우기"다.
 *
 * 서로 다른 IOMMU 에 걸친 도메인은 지원하지 않는다.
 */
static int qcom_iommu_attach_dev(struct iommu_domain *domain,
				 struct device *dev, struct iommu_domain *old)
{
	struct qcom_iommu_dev *qcom_iommu = dev_iommu_priv_get(dev);	/* [한국어] of_xlate 가 매달아 둔 IOMMU. */
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	int ret;	/* [한국어] 결과 코드. */

	if (!qcom_iommu) {	/* [한국어] 이 IOMMU 아래 장치가 아니면 */
		dev_err(dev, "cannot attach to IOMMU, is it on the same bus?\n");	/* [한국어] 알리고 */
		return -ENXIO;	/* [한국어] 거절한다. */
	}

	/* Ensure that the domain is finalized */
	pm_runtime_get_sync(qcom_iommu->dev);	/* [한국어] 뱅크를 프로그래밍하려면 켜야 한다. */
	ret = qcom_iommu_init_domain(domain, qcom_iommu, dev);	/* [한국어] 아직 세워지지 않았으면 여기서 세운다. */
	pm_runtime_put_sync(qcom_iommu->dev);	/* [한국어] 전원 참조를 놓는다. */
	if (ret < 0)	/* [한국어] 세우지 못했으면 */
		return ret;	/* [한국어] 붙일 수 없다. */

	/*
	 * Sanity check the domain. We don't support domains across
	 * different IOMMUs.
	 */
	if (qcom_domain->iommu != qcom_iommu)	/* [한국어] 다른 IOMMU 에 매인 도메인이면 */
		return -EINVAL;	/* [한국어] 거절한다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * qcom_iommu_identity_attach - 장치를 항등 도메인에 붙인다
 *
 * @identity_domain: 항등 도메인.
 * @dev: 붙는 장치.
 * @old: 붙어 있던 도메인.
 * @return: 0 성공, 음수면 실패.
 *
 * 이 하드웨어에서 항등은 곧 "뱅크를 끄는 것"이다. 변환이 꺼진 뱅크는
 * 주소를 그대로 통과시킨다.
 *
 * 이미 항등이거나 아무것도 붙어 있지 않으면 할 일이 없다.
 */
static int qcom_iommu_identity_attach(struct iommu_domain *identity_domain,
				      struct device *dev,
				      struct iommu_domain *old)
{
	struct qcom_iommu_domain *qcom_domain;	/* [한국어] 옛 도메인. */
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);	/* [한국어] ASID 목록. */
	struct qcom_iommu_dev *qcom_iommu = dev_iommu_priv_get(dev);	/* [한국어] 매달아 둔 IOMMU. */
	unsigned int i;	/* [한국어] 순회 첨자. */

	if (old == identity_domain || !old)	/* [한국어] 이미 항등이거나 붙어 있지 않으면 */
		return 0;	/* [한국어] 할 일이 없다. */

	qcom_domain = to_qcom_iommu_domain(old);	/* [한국어] 옛 도메인으로 되짚는다. */
	if (WARN_ON(!qcom_domain->iommu))	/* [한국어] 세워지지 않은 도메인에 붙어 있었다면 버그다. */
		return -EINVAL;	/* [한국어] 거절. */

	pm_runtime_get_sync(qcom_iommu->dev);	/* [한국어] 레지스터를 쓰려면 켜야 한다. */
	for (i = 0; i < fwspec->num_ids; i++) {	/* [한국어] 이 장치가 쓰던 뱅크마다 */
		struct qcom_iommu_ctx *ctx = to_ctx(qcom_domain, fwspec->ids[i]);	/* [한국어] 그 뱅크를 찾아 */

		/* Disable the context bank: */
		iommu_writel(ctx, ARM_SMMU_CB_SCTLR, 0);	/* [한국어] 끈다. 변환이 꺼진 뱅크는 주소를 그대로 통과시킨다. */

		ctx->domain = NULL;	/* [한국어] 어느 도메인에도 속하지 않게 한다. */
	}
	pm_runtime_put_sync(qcom_iommu->dev);	/* [한국어] 전원 참조를 놓는다. */
	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어] 항등 도메인의 연산표. 붙이기만 있으면 된다.
 */
static struct iommu_domain_ops qcom_iommu_identity_ops = {
	.attach_dev = qcom_iommu_identity_attach,	/* [한국어] 뱅크를 꺼서 주소를 그대로 통과시킨다. */
};

/*
 * [한국어] 하나뿐인 항등 도메인. 정적으로 두어 실패하지 않게 한다.
 */
static struct iommu_domain qcom_iommu_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,	/* [한국어] 항등 종류. */
	.ops = &qcom_iommu_identity_ops,
};

/*
 * [한국어]
 * qcom_iommu_map - 매핑을 만든다
 *
 * @domain: 대상 도메인.
 * @iova: 시작 주소.
 * @paddr: 물리 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 그 개수.
 * @prot: 권한.
 * @gfp: 할당 플래그(무시한다).
 * @mapped: 실제로 매핑한 바이트 수.
 * @return: 0 성공, 음수면 실패.
 *
 * 스핀락 아래에서 도는 것이 규격 드라이버와 다르다. 그래서 호출자가
 * 무엇을 주든 GFP_ATOMIC 으로 바꿔 넘긴다 — 잠들 수 없기 때문이다.
 */
static int qcom_iommu_map(struct iommu_domain *domain, unsigned long iova,
			  phys_addr_t paddr, size_t pgsize, size_t pgcount,
			  int prot, gfp_t gfp, size_t *mapped)
{
	int ret;	/* [한국어] 결과 코드. */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장. */
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;	/* [한국어] 표 조작 함수들. */

	if (!ops)	/* [한국어] 아직 세워지지 않았으면 */
		return -ENODEV;	/* [한국어] 매핑할 곳이 없다. */

	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);	/* [한국어] 표 조작을 직렬화한다. 인터럽트 문맥에서도 불릴 수 있어 막는다. */
	ret = ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, GFP_ATOMIC, mapped);	/* [한국어] 스핀락 아래라 잠들 수 없어, 호출자가 무엇을 주든 원자적 할당으로 바꾼다. */
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);	/* [한국어] 락 해제. */
	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * qcom_iommu_unmap - 매핑을 푼다
 *
 * @domain: 대상 도메인.
 * @iova: 시작 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 그 개수.
 * @gather: 무효화 목록.
 * @return: 실제로 푼 바이트 수.
 *
 * 원 주석대로 전원을 손수 켠다 — 해제는 클라이언트가 꺼진 뒤에도
 * 불릴 수 있어 장치 링크에 기댈 수 없다.
 */
static size_t qcom_iommu_unmap(struct iommu_domain *domain, unsigned long iova,
			       size_t pgsize, size_t pgcount,
			       struct iommu_iotlb_gather *gather)
{
	size_t ret;	/* [한국어] 푼 바이트 수. */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장. */
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;	/* [한국어] 표 조작 함수들. */

	if (!ops)	/* [한국어] 세워지지 않았으면 */
		return 0;	/* [한국어] 푼 것이 없다. */

	/* NOTE: unmap can be called after client device is powered off,
	 * for example, with GPUs or anything involving dma-buf.  So we
	 * cannot rely on the device_link.  Make sure the IOMMU is on to
	 * avoid unclocked accesses in the TLB inv path:
	 */
	pm_runtime_get_sync(qcom_domain->iommu->dev);	/* [한국어] 원 주석대로 클라이언트가 꺼진 뒤에도 불릴 수 있어 손수 켠다. */
	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);	/* [한국어] 표 조작을 직렬화한다. */
	ret = ops->unmap_pages(ops, iova, pgsize, pgcount, gather);	/* [한국어] 해제와 무효화는 io-pgtable 이 이 파일의 함수를 불러 처리한다. */
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);	/* [한국어] 락 해제. */
	pm_runtime_put_sync(qcom_domain->iommu->dev);	/* [한국어] 전원 참조를 놓는다. */

	return ret;	/* [한국어] 푼 크기. */
}

/*
 * [한국어]
 * qcom_iommu_flush_iotlb_all - 모아 둔 무효화의 완료를 기다린다
 *
 * @domain: 대상 도메인.
 *
 * 이름과 달리 새로 비우지는 않는다 — 무효화 명령은 이미 나갔고,
 * 여기서는 끝나기를 기다린다.
 */
static void qcom_iommu_flush_iotlb_all(struct iommu_domain *domain)
{
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct io_pgtable *pgtable = container_of(qcom_domain->pgtbl_ops,	/* [한국어] 조작 함수에서 표 객체로 되짚는다. */
						  struct io_pgtable, ops);
	if (!qcom_domain->pgtbl_ops)	/* [한국어] 세워지지 않았으면 */
		return;	/* [한국어] 기다릴 것이 없다. */

	pm_runtime_get_sync(qcom_domain->iommu->dev);	/* [한국어] 레지스터를 읽으려면 켜야 한다. */
	qcom_iommu_tlb_sync(pgtable->cookie);	/* [한국어] 모아 둔 무효화의 완료를 기다린다. */
	pm_runtime_put_sync(qcom_domain->iommu->dev);	/* [한국어] 전원 참조를 놓는다. */
}

/*
 * [한국어]
 * qcom_iommu_iotlb_sync - 위와 같다
 *
 * @domain: 대상 도메인.
 * @gather: 무효화 목록(쓰지 않는다).
 */
static void qcom_iommu_iotlb_sync(struct iommu_domain *domain,
				  struct iommu_iotlb_gather *gather)
{
	qcom_iommu_flush_iotlb_all(domain);	/* [한국어] 두 콜백이 하는 일이 같다. */
}

/*
 * [한국어]
 * qcom_iommu_iova_to_phys - IOVA 를 물리 주소로 옮긴다
 *
 * @domain: 대상 도메인.
 * @iova: 물어볼 주소.
 * @return: 그 물리 주소, 없으면 0.
 *
 * 하드웨어에 물어보는 길이 없어 늘 소프트웨어로 표를 따라간다.
 */
static phys_addr_t qcom_iommu_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	phys_addr_t ret;	/* [한국어] 결과. */
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장. */
	struct qcom_iommu_domain *qcom_domain = to_qcom_iommu_domain(domain);	/* [한국어] 이 드라이버의 도메인. */
	struct io_pgtable_ops *ops = qcom_domain->pgtbl_ops;	/* [한국어] 표 조작 함수들. */

	if (!ops)	/* [한국어] 세워지지 않았으면 */
		return 0;	/* [한국어] 매핑이 없다. */

	spin_lock_irqsave(&qcom_domain->pgtbl_lock, flags);	/* [한국어] 표를 읽는 동안 바뀌지 않게 한다. */
	ret = ops->iova_to_phys(ops, iova);	/* [한국어] 소프트웨어로 표를 따라간다. 하드웨어에 물어보는 길이 없다. */
	spin_unlock_irqrestore(&qcom_domain->pgtbl_lock, flags);	/* [한국어] 락 해제. */

	return ret;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * qcom_iommu_capable - 능력을 답한다
 *
 * @dev: 물어보는 장치(쓰지 않는다).
 * @cap: 물어보는 능력.
 * @return: 갖췄으면 참.
 *
 * 원 주석대로 이 SMMU 는 늘 일관성 있는 요청을 낸다.
 */
static bool qcom_iommu_capable(struct device *dev, enum iommu_cap cap)
{
	switch (cap) {	/* [한국어] 물어보는 능력. */
	case IOMMU_CAP_CACHE_COHERENCY:	/* [한국어] 캐시 일관성. */
		/*
		 * Return true here as the SMMU can always send out coherent
		 * requests.
		 */
		return true;	/* [한국어] 원 주석대로 이 SMMU 는 늘 일관성 있는 요청을 낸다. */
	case IOMMU_CAP_NOEXEC:	/* [한국어] 실행 금지 매핑. */
		return true;	/* [한국어] 지원한다. */
	default:	/* [한국어] 그 밖에는 */
		return false;	/* [한국어] 지원하지 않는다. */
	}
}

/*
 * [한국어]
 * qcom_iommu_probe_device - 이 장치를 맡는다
 *
 * @dev: 검사할 장치.
 * @return: 이 IOMMU 의 손잡이, 아니면 오류 포인터.
 *
 * of_xlate 가 이미 IOMMU 를 매달아 두었으므로 그것이 있는지만 본다.
 *
 * 원 주석대로 장치 링크를 만들어, 그 장치가 쓰일 때 IOMMU 도 함께
 * 깨어나게 한다.
 */
static struct iommu_device *qcom_iommu_probe_device(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_iommu_priv_get(dev);	/* [한국어] of_xlate 가 매달아 둔 IOMMU. */
	struct device_link *link;	/* [한국어] 만들 장치 링크. */

	if (!qcom_iommu)	/* [한국어] 이 IOMMU 아래 장치가 아니면 */
		return ERR_PTR(-ENODEV);	/* [한국어] 맡지 않는다. */

	/*
	 * Establish the link between iommu and master, so that the
	 * iommu gets runtime enabled/disabled as per the master's
	 * needs.
	 */
	link = device_link_add(dev, qcom_iommu->dev, DL_FLAG_PM_RUNTIME);	/* [한국어] 원 주석대로 클라이언트가 쓰일 때 IOMMU 도 함께 깨어나게 한다. */
	if (!link) {	/* [한국어] 만들지 못했으면 */
		dev_err(qcom_iommu->dev, "Unable to create device link between %s and %s\n",	/* [한국어] 알리고 */
			dev_name(qcom_iommu->dev), dev_name(dev));
		return ERR_PTR(-ENODEV);	/* [한국어] 맡지 않는다 — 전원 관리가 어긋나면 위험하다. */
	}

	return &qcom_iommu->iommu;	/* [한국어] 이 장치를 맡을 IOMMU 의 손잡이. */
}

/*
 * [한국어]
 * qcom_iommu_of_xlate - 장치 트리의 ASID 를 받아 검증한다
 *
 * @dev: 대상 장치.
 * @args: iommus 속성의 인자들.
 * @return: 0 성공, 음수면 실패.
 *
 * 인자가 ASID 하나뿐이다 — 스트림 매칭이 없어 마스크가 필요 없다.
 *
 * 원 주석대로 여기서 검증해 두면 다른 곳에서 다시 확인할 필요가 없다.
 * 배열 첨자로 곧장 쓰이므로 범위를 벗어나면 위험하다.
 *
 * 여러 뱅크는 괜찮지만 여러 IOMMU 는 안 된다는 원 주석의 구분이 중요하다 —
 * 한 장치가 여러 ASID 를 쓸 수 있지만, 그것들이 같은 IOMMU 에 속해야 한다.
 */
static int qcom_iommu_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	struct qcom_iommu_dev *qcom_iommu;	/* [한국어] 찾아낼 IOMMU. */
	struct platform_device *iommu_pdev;	/* [한국어] 그 플랫폼 장치. */
	unsigned asid = args->args[0];	/* [한국어] 장치 트리가 적어 준 ASID. */

	if (args->args_count != 1) {	/* [한국어] 인자는 ASID 하나뿐이어야 한다. */
		dev_err(dev, "incorrect number of iommu params found for %s "	/* [한국어] 아니면 트리가 잘못된 것이다. */
			"(found %d, expected 1)\n",
			args->np->full_name, args->args_count);
		return -EINVAL;	/* [한국어] 거절. */
	}

	iommu_pdev = of_find_device_by_node(args->np);	/* [한국어] 그 노드의 IOMMU 장치를 찾는다. */
	if (WARN_ON(!iommu_pdev))	/* [한국어] 없으면 트리가 어긋난 것이다. */
		return -EINVAL;	/* [한국어] 거절. */

	qcom_iommu = platform_get_drvdata(iommu_pdev);	/* [한국어] 그 드라이버 상태가 곧 IOMMU 다. */

	put_device(&iommu_pdev->dev);	/* [한국어] 조회용 참조를 놓는다. 플랫폼 버스가 들고 있어 사라지지 않는다. */

	/* make sure the asid specified in dt is valid, so we don't have
	 * to sanity check this elsewhere:
	 */
	if (WARN_ON(asid > qcom_iommu->max_asid) ||	/* [한국어] 원 주석대로 여기서 검증해 두면 다른 곳에서 다시 볼 필요가 없다. */
	    WARN_ON(qcom_iommu->ctxs[asid] == NULL))	/* [한국어] 배열 첨자로 곧장 쓰이므로 범위를 벗어나면 위험하다. */
		return -EINVAL;	/* [한국어] 거절. */

	if (!dev_iommu_priv_get(dev)) {	/* [한국어] 아직 매달린 것이 없으면 */
		dev_iommu_priv_set(dev, qcom_iommu);	/* [한국어] 이 IOMMU 를 매단다. */
	} else {	/* [한국어] 이미 있으면 */
		/* make sure devices iommus dt node isn't referring to
		 * multiple different iommu devices.  Multiple context
		 * banks are ok, but multiple devices are not:
		 */
		if (WARN_ON(qcom_iommu != dev_iommu_priv_get(dev)))	/* [한국어] 원 주석대로 여러 뱅크는 괜찮지만 여러 IOMMU 는 안 된다. */
			return -EINVAL;	/* [한국어] 거절. */
	}

	return iommu_fwspec_add_ids(dev, &asid, 1);	/* [한국어] ASID 를 목록에 담는다. */
}

/*
 * [한국어] iommu 코어에 등록하는 연산표.
 *
 * 항등 도메인만 정적으로 두고, 차단 도메인은 없다 — 이 하드웨어에는
 * 그것을 흉내 낼 방법이 마땅치 않다.
 */
static const struct iommu_ops qcom_iommu_ops = {
	.identity_domain = &qcom_iommu_identity_domain,	/* [한국어] 정적으로 둔 항등 도메인. */
	.capable	= qcom_iommu_capable,	/* [한국어] 능력 조회. */
	.domain_alloc_paging = qcom_iommu_domain_alloc_paging,	/* [한국어] 페이징 도메인 생성. */
	.probe_device	= qcom_iommu_probe_device,	/* [한국어] 장치를 맡을지 정한다. */
	.device_group	= generic_device_group,	/* [한국어] 장치마다 그룹 하나. 스트림 매칭이 없어 서로 간섭할 일이 없다. */
	.of_xlate	= qcom_iommu_of_xlate,	/* [한국어] 장치 트리의 ASID 를 받는다. */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= qcom_iommu_attach_dev,	/* [한국어] 도메인 위에서 실제 매핑을 다루는 함수들이 여기 모인다. */
		.map_pages	= qcom_iommu_map,
		.unmap_pages	= qcom_iommu_unmap,
		.flush_iotlb_all = qcom_iommu_flush_iotlb_all,
		.iotlb_sync	= qcom_iommu_iotlb_sync,
		.iova_to_phys	= qcom_iommu_iova_to_phys,
		.free		= qcom_iommu_domain_free,
	}
};

/*
 * [한국어]
 * qcom_iommu_sec_ptbl_init - 보안 세계가 쓸 페이지 테이블 메모리를 마련한다
 *
 * @dev: 요청하는 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 보안 세계는 스스로 메모리를 잡을 수 없어 커널이 대신 잡아 준다.
 * 크기도 보안 세계에 물어본다.
 *
 * NO_KERNEL_MAPPING 이 요점이다 — 커널이 그 메모리를 볼 수 없게 해야
 * 보안 경계가 지켜진다.
 *
 * static 표시로 한 번만 하는 것에 주의. 이 메모리는 시스템 전체가
 * 공유한다.
 */
static int qcom_iommu_sec_ptbl_init(struct device *dev)
{
	size_t psize = 0;	/* [한국어] 보안 세계가 요구하는 크기. */
	unsigned int spare = 0;	/* [한국어] 예약 인자. 지금은 쓰이지 않는다. */
	void *cpu_addr;	/* [한국어] 잡은 메모리의 커널 주소(실제로는 매핑하지 않는다). */
	dma_addr_t paddr;	/* [한국어] 그 물리 주소. 보안 세계에 알려 준다. */
	unsigned long attrs;	/* [한국어] 할당 속성. */
	static bool allocated = false;	/* [한국어] 시스템 전체가 공유하므로 한 번만 한다. */
	int ret;	/* [한국어] 결과 코드. */

	if (allocated)	/* [한국어] 이미 했으면 */
		return 0;	/* [한국어] 다시 하지 않는다. */

	ret = qcom_scm_iommu_secure_ptbl_size(spare, &psize);	/* [한국어] 얼마나 필요한지 보안 세계에 묻는다. */
	if (ret) {	/* [한국어] 앞선 걸음이 실패했다. */
		dev_err(dev, "failed to get iommu secure pgtable size (%d)\n",	/* [한국어] 묻지 못했으면 알리고 */
			ret);
		return ret;	/* [한국어] 포기한다. */
	}

	dev_info(dev, "iommu sec: pgtable size: %zu\n", psize);	/* [한국어] 얼마를 잡는지 남긴다. */

	attrs = DMA_ATTR_NO_KERNEL_MAPPING;	/* [한국어] 커널이 그 메모리를 볼 수 없게 한다 — 보안 경계를 지키는 요점이다. */

	cpu_addr = dma_alloc_attrs(dev, psize, &paddr, GFP_KERNEL, attrs);	/* [한국어] 물리적으로 이어진 메모리를 잡는다. */
	if (!cpu_addr) {	/* [한국어] 메모리가 없으면 */
		dev_err(dev, "failed to allocate %zu bytes for pgtable\n",	/* [한국어] 알리고 */
			psize);
		return -ENOMEM;	/* [한국어] 포기한다. */
	}

	ret = qcom_scm_iommu_secure_ptbl_init(paddr, psize, spare);	/* [한국어] 보안 세계에 그 메모리를 넘긴다. */
	if (ret) {	/* [한국어] 앞선 걸음이 실패했다. */
		dev_err(dev, "failed to init iommu pgtable (%d)\n", ret);	/* [한국어] 넘기지 못했으면 알리고 */
		goto free_mem;	/* [한국어] 메모리를 되돌린다. */
	}

	allocated = true;	/* [한국어] 다시 하지 않게 표시한다. */
	return 0;	/* [한국어] 성공. */

free_mem:	/* [한국어] 실패 경로. */
	dma_free_attrs(dev, psize, cpu_addr, paddr, attrs);	/* [한국어] 메모리를 해제한다. */
	return ret;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * (위 영어 주석에 이어)
 * get_asid - 뱅크 노드에서 ASID 를 알아낸다
 *
 * @np: 그 노드.
 * @return: ASID, 실패하면 음수.
 *
 * 원 주석대로 뱅크가 0x1000 간격이라 그 주소를 나누면 번호가 나온다.
 * 다만 그 규칙이 맞지 않는 경우가 있어, 장치 트리가 직접 적어 줄 수도
 * 있게 했다.
 */
static int get_asid(const struct device_node *np)
{
	u32 reg, val;	/* [한국어] 읽어 온 주소와 명시된 값. */
	int asid;	/* [한국어] 계산한 ASID. */

	/* read the "reg" property directly to get the relative address
	 * of the context bank, and calculate the asid from that:
	 */
	if (of_property_read_u32_index(np, "reg", 0, &reg))	/* [한국어] 뱅크의 상대 주소를 읽는다. */
		return -ENODEV;	/* [한국어] 없으면 다룰 수 없다. */

	/*
	 * Context banks are 0x1000 apart but, in some cases, the ASID
	 * number doesn't match to this logic and needs to be passed
	 * from the DT configuration explicitly.
	 */
	if (!of_property_read_u32(np, "qcom,ctx-asid", &val))	/* [한국어] 원 주석대로 규칙이 맞지 않는 경우가 있어 */
		asid = val;	/* [한국어] 트리가 직접 적어 줄 수 있게 했다. */
	else
		asid = reg / 0x1000;	/* [한국어] 기본은 뱅크 간격으로 나눈 값이다. */

	return asid;	/* [한국어] 결과. */
}

/*
 * [한국어]
 * qcom_iommu_ctx_probe - 컨텍스트 뱅크 장치를 잡는다
 *
 * @pdev: 그 플랫폼 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 뱅크가 각자 별도의 장치라 이렇게 따로 잡는다. 부모가 IOMMU 장치다.
 *
 * 원 주석대로 부트로더가 남긴 오류를 미리 지운다 — 처리기를 걸자마자
 * 옛 오류가 터지면 혼란스럽다.
 *
 * 보안 문맥은 레지스터를 건드릴 수 없어 그 지우기를 건너뛴다.
 */
static int qcom_iommu_ctx_probe(struct platform_device *pdev)
{
	struct qcom_iommu_ctx *ctx;	/* [한국어] 만들 뱅크 구조체. */
	struct device *dev = &pdev->dev;	/* [한국어] 그 장치. */
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev->parent);	/* [한국어] 부모가 IOMMU 장치다. */
	int ret, irq;	/* [한국어] 결과와 인터럽트 번호. */

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);	/* [한국어] 구조체를 잡는다. */
	if (!ctx)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */

	ctx->dev = dev;	/* [한국어] 장치를 기억한다. */
	platform_set_drvdata(pdev, ctx);	/* [한국어] 장치에서 이 구조체를 찾을 수 있게 한다. */

	ctx->base = devm_platform_ioremap_resource(pdev, 0);	/* [한국어] 뱅크의 레지스터 창을 매핑한다. */
	if (IS_ERR(ctx->base))	/* [한국어] 실패하면 */
		return PTR_ERR(ctx->base);	/* [한국어] 포기한다. */

	irq = platform_get_irq(pdev, 0);	/* [한국어] 이 뱅크의 오류 인터럽트. */
	if (irq < 0)	/* [한국어] 없으면 */
		return irq;	/* [한국어] 포기한다. */

	if (of_device_is_compatible(dev->of_node, "qcom,msm-iommu-v2-sec"))	/* [한국어] 보안 문맥이면 */
		ctx->secured_ctx = true;	/* [한국어] 커널이 레지스터를 건드릴 수 없다고 표시한다. */

	/* clear IRQs before registering fault handler, just in case the
	 * boot-loader left us a surprise:
	 */
	if (!ctx->secured_ctx)	/* [한국어] 비보안 문맥일 때만 */
		iommu_writel(ctx, ARM_SMMU_CB_FSR, iommu_readl(ctx, ARM_SMMU_CB_FSR));	/* [한국어] 원 주석대로 부트로더가 남긴 오류를 미리 지운다. */

	ret = devm_request_irq(dev, irq,	/* [한국어] 오류 처리기를 건다. */
			       qcom_iommu_fault,	/* [한국어] 이 파일의 처리기. */
			       IRQF_SHARED,	/* [한국어] 다른 뱅크와 인터럽트를 나눠 쓸 수 있다. */
			       "qcom-iommu-fault",
			       ctx);
	if (ret) {	/* [한국어] 걸지 못했으면 */
		dev_err(dev, "failed to request IRQ %u\n", irq);	/* [한국어] 알리고 */
		return ret;	/* [한국어] 포기한다. */
	}

	ret = get_asid(dev->of_node);	/* [한국어] 이 뱅크의 ASID 를 알아낸다. */
	if (ret < 0) {	/* [한국어] 알아내지 못했으면 */
		dev_err(dev, "missing reg property\n");	/* [한국어] 알리고 */
		return ret;	/* [한국어] 포기한다. */
	}

	ctx->asid = ret;	/* [한국어] ASID 를 기억한다. 이것이 곧 뱅크 번호다. */

	dev_dbg(dev, "found asid %u\n", ctx->asid);	/* [한국어] 디버그 로그. */

	qcom_iommu->ctxs[ctx->asid] = ctx;	/* [한국어] 부모의 배열에 자기를 넣는다. 이제 of_xlate 가 이 뱅크를 찾을 수 있다. */

	return 0;	/* [한국어] 성공. */
}

/*
 * [한국어]
 * qcom_iommu_ctx_remove - 뱅크 장치를 걷어 낸다
 *
 * @pdev: 그 플랫폼 장치.
 *
 * 부모의 배열에서 자기를 뺀다. 그 뒤로는 아무도 이 뱅크를 찾지 못한다.
 */
static void qcom_iommu_ctx_remove(struct platform_device *pdev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(pdev->dev.parent);	/* [한국어] 부모 IOMMU. */
	struct qcom_iommu_ctx *ctx = platform_get_drvdata(pdev);	/* [한국어] 이 뱅크. */

	platform_set_drvdata(pdev, NULL);	/* [한국어] 장치와의 연결을 끊고 */

	qcom_iommu->ctxs[ctx->asid] = NULL;	/* [한국어] 부모의 배열에서도 뺀다. 그 뒤로는 아무도 이 뱅크를 찾지 못한다. */
}

/*
 * [한국어] 컨텍스트 뱅크 노드의 이름들.
 *
 * ns 는 비보안, sec 은 보안 문맥이다. 보안 쪽은 커널이 레지스터를
 * 건드릴 수 없다.
 */
static const struct of_device_id ctx_of_match[] = {
	{ .compatible = "qcom,msm-iommu-v1-ns" },	/* [한국어] ns 는 비보안, sec 은 보안 문맥이다. */
	{ .compatible = "qcom,msm-iommu-v1-sec" },
	{ .compatible = "qcom,msm-iommu-v2-ns" },
	{ .compatible = "qcom,msm-iommu-v2-sec" },
	{ /* sentinel */ }
};

/*
 * [한국어] 컨텍스트 뱅크를 잡는 플랫폼 드라이버.
 *
 * IOMMU 본체와 별개로 등록되고, 부모-자식 관계로 이어진다.
 */
static struct platform_driver qcom_iommu_ctx_driver = {
	.driver	= {	/* [한국어] 드라이버 코어에 알릴 정보. */
		.name		= "qcom-iommu-ctx",	/* [한국어] 뱅크 드라이버의 이름. */
		.of_match_table	= ctx_of_match,
	},
	.probe	= qcom_iommu_ctx_probe,
	.remove = qcom_iommu_ctx_remove,
};

/*
 * [한국어]
 * qcom_iommu_has_secure_context - 보안 문맥이 하나라도 있는가
 *
 * @qcom_iommu: 대상 IOMMU.
 * @return: 있으면 참.
 *
 * 있으면 보안 세계가 쓸 페이지 테이블 메모리를 마련해야 한다.
 * 없으면 그 비싼 할당을 건너뛴다.
 */
static bool qcom_iommu_has_secure_context(struct qcom_iommu_dev *qcom_iommu)
{
	for_each_child_of_node_scoped(qcom_iommu->dev->of_node, child) {	/* [한국어] 자식 노드를 훑는다. scoped 판이라 참조가 저절로 놓인다. */
		if (of_device_is_compatible(child, "qcom,msm-iommu-v1-sec") ||	/* [한국어] 1판 보안 문맥이거나 */
		    of_device_is_compatible(child, "qcom,msm-iommu-v2-sec"))	/* [한국어] 2판 보안 문맥이면 */
			return true;	/* [한국어] 있다. */
	}

	return false;	/* [한국어] 하나도 없다. */
}

/*
 * [한국어]
 * qcom_iommu_device_probe - IOMMU 하드웨어를 찾아 세운다
 *
 * @pdev: 그 플랫폼 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 자식 노드를 먼저 훑어 가장 큰 ASID 를 알아낸다 — 그만큼 배열을 잡아야
 * 하기 때문이다.
 *
 * 자식 장치를 채우는 것이 이 함수의 요점이다. 그 순간 뱅크마다
 * ctx_probe 가 불려 배열이 채워진다.
 *
 * 마지막의 인터럽트 선택 레지스터 쓰기는 모든 인터럽트를 비보안 쪽으로
 * 보내라는 뜻이다 — 그러지 않으면 커널이 오류를 볼 수 없다.
 */
static int qcom_iommu_device_probe(struct platform_device *pdev)
{
	struct device_node *child;	/* [한국어] 자식 노드를 훑을 변수. */
	struct qcom_iommu_dev *qcom_iommu;	/* [한국어] 만들 IOMMU 구조체. */
	struct device *dev = &pdev->dev;	/* [한국어] 그 장치. */
	struct resource *res;	/* [한국어] 레지스터 창의 자원 정보. */
	struct clk *clk;	/* [한국어] 얻어 올 클럭. */
	int ret, max_asid = 0;	/* [한국어] 결과와 가장 큰 ASID. */

	/* find the max asid (which is 1:1 to ctx bank idx), so we know how
	 * many child ctx devices we have:
	 */
	for_each_child_of_node(dev->of_node, child)	/* [한국어] 원 주석대로 자식이 몇 개인지 알아야 배열을 잡을 수 있다. */
		max_asid = max(max_asid, get_asid(child));	/* [한국어] ASID 가 곧 첨자이므로 그 최댓값이 필요하다. */

	qcom_iommu = devm_kzalloc(dev, struct_size(qcom_iommu, ctxs, max_asid + 1),	/* [한국어] 그만큼의 배열을 뒤에 붙여 잡는다. */
				  GFP_KERNEL);
	if (!qcom_iommu)	/* [한국어] 메모리가 없다. */
		return -ENOMEM;	/* [한국어] 실패. */
	qcom_iommu->max_asid = max_asid;	/* [한국어] 검증에 쓸 상한. */
	qcom_iommu->dev = dev;	/* [한국어] 장치를 기억한다. */

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);	/* [한국어] 전역 레지스터 창(없을 수도 있다). */
	if (res) {	/* [한국어] 있으면 */
		qcom_iommu->local_base = devm_ioremap_resource(dev, res);	/* [한국어] 매핑한다. */
		if (IS_ERR(qcom_iommu->local_base))	/* [한국어] 실패하면 */
			return PTR_ERR(qcom_iommu->local_base);	/* [한국어] 포기한다. */
	}

	clk = devm_clk_get(dev, "iface");	/* [한국어] 인터페이스 클럭. 레지스터 접근에 필요하다. */
	if (IS_ERR(clk)) {	/* [한국어] 없으면 */
		dev_err(dev, "failed to get iface clock\n");	/* [한국어] 알리고 */
		return PTR_ERR(clk);	/* [한국어] 포기한다. */
	}
	qcom_iommu->clks[CLK_IFACE].clk = clk;	/* [한국어] 한꺼번에 켤 목록에 담는다. */

	clk = devm_clk_get(dev, "bus");	/* [한국어] 버스 클럭. 표 순회에 필요하다. */
	if (IS_ERR(clk)) {	/* [한국어] 그 클럭이 없다. */
		dev_err(dev, "failed to get bus clock\n");	/* [한국어] 없으면 알리고 */
		return PTR_ERR(clk);	/* [한국어] 포기한다. */
	}
	qcom_iommu->clks[CLK_BUS].clk = clk;	/* [한국어] 목록에 담는다. */

	clk = devm_clk_get_optional(dev, "tbu");	/* [한국어] TBU 클럭은 없을 수도 있어 optional 판을 쓴다. */
	if (IS_ERR(clk)) {	/* [한국어] 그 클럭이 없다. */
		dev_err(dev, "failed to get tbu clock\n");	/* [한국어] 오류면 알리고 */
		return PTR_ERR(clk);	/* [한국어] 포기한다. */
	}
	qcom_iommu->clks[CLK_TBU].clk = clk;	/* [한국어] 목록에 담는다. */

	if (of_property_read_u32(dev->of_node, "qcom,iommu-secure-id",	/* [한국어] 보안 세계가 이 IOMMU 를 가리키는 번호. */
				 &qcom_iommu->sec_id)) {
		dev_err(dev, "missing qcom,iommu-secure-id property\n");	/* [한국어] 없으면 SCM 호출을 할 수 없다. */
		return -ENODEV;	/* [한국어] 포기한다. */
	}

	if (qcom_iommu_has_secure_context(qcom_iommu)) {	/* [한국어] 보안 문맥이 있으면 */
		ret = qcom_iommu_sec_ptbl_init(dev);	/* [한국어] 보안 세계가 쓸 표 메모리를 마련한다. */
		if (ret) {	/* [한국어] 앞선 걸음이 실패했다. */
			dev_err(dev, "cannot init secure pg table(%d)\n", ret);	/* [한국어] 실패하면 알리고 */
			return ret;	/* [한국어] 포기한다. */
		}
	}

	platform_set_drvdata(pdev, qcom_iommu);	/* [한국어] 자식이 부모에서 이 구조체를 찾을 수 있게 한다. */

	pm_runtime_enable(dev);	/* [한국어] 전원 관리를 켠다. 자식을 채우기 전이어야 그쪽이 부모를 깨울 수 있다. */

	/* register context bank devices, which are child nodes: */
	ret = devm_of_platform_populate(dev);	/* [한국어] 자식 노드를 장치로 만든다 — 그 순간 뱅크마다 ctx_probe 가 불린다. */
	if (ret) {	/* [한국어] 앞선 걸음이 실패했다. */
		dev_err(dev, "Failed to populate iommu contexts\n");	/* [한국어] 실패하면 알리고 */
		goto err_pm_disable;	/* [한국어] 전원 관리를 되돌린다. */
	}

	ret = iommu_device_sysfs_add(&qcom_iommu->iommu, dev, NULL,	/* [한국어] sysfs 항목을 만든다. */
				     dev_name(dev));
	if (ret) {	/* [한국어] 앞선 걸음이 실패했다. */
		dev_err(dev, "Failed to register iommu in sysfs\n");	/* [한국어] 실패하면 알리고 */
		goto err_pm_disable;	/* [한국어] 되돌린다. */
	}

	ret = iommu_device_register(&qcom_iommu->iommu, &qcom_iommu_ops, dev);	/* [한국어] 코어에 등록한다. 이 순간부터 장치가 붙기 시작한다. */
	if (ret) {	/* [한국어] 앞선 걸음이 실패했다. */
		dev_err(dev, "Failed to register iommu\n");	/* [한국어] 실패하면 알리고 */
		goto err_pm_disable;	/* [한국어] 되돌린다. */
	}

	if (qcom_iommu->local_base) {	/* [한국어] 전역 레지스터 창이 있으면 */
		pm_runtime_get_sync(dev);	/* [한국어] 켜고 */
		writel_relaxed(0xffffffff, qcom_iommu->local_base + SMMU_INTR_SEL_NS);	/* [한국어] 모든 인터럽트를 비보안 쪽으로 보낸다 — 그러지 않으면 커널이 오류를 볼 수 없다. */
		pm_runtime_put_sync(dev);	/* [한국어] 전원 참조를 놓는다. */
	}

	return 0;	/* [한국어] 성공. */

err_pm_disable:	/* [한국어] 모든 실패가 합류한다. */
	pm_runtime_disable(dev);	/* [한국어] 전원 관리를 되돌리고 */
	return ret;	/* [한국어] 실패를 올린다. */
}

/*
 * [한국어]
 * qcom_iommu_device_remove - 드라이버를 걷어 낸다
 *
 * @pdev: 그 플랫폼 장치.
 *
 * 먼저 전원을 강제로 내려, 이후 정리가 클럭 없이 레지스터를 건드리지
 * 않게 한다.
 */
static void qcom_iommu_device_remove(struct platform_device *pdev)
{
	struct qcom_iommu_dev *qcom_iommu = platform_get_drvdata(pdev);	/* [한국어] 이 장치의 IOMMU. */

	pm_runtime_force_suspend(&pdev->dev);	/* [한국어] 먼저 전원을 내려, 이후 정리가 클럭 없이 레지스터를 건드리지 않게 한다. */
	platform_set_drvdata(pdev, NULL);	/* [한국어] 장치와의 연결을 끊는다. */
	iommu_device_sysfs_remove(&qcom_iommu->iommu);	/* [한국어] sysfs 항목을 걷고 */
	iommu_device_unregister(&qcom_iommu->iommu);	/* [한국어] 코어 등록을 푼다. */
}

/*
 * [한국어]
 * qcom_iommu_resume - 전원이 돌아왔을 때
 *
 * @dev: IOMMU 장치.
 * @return: 0 성공, 음수면 실패.
 *
 * 클럭을 켜고, 전원 도메인이 있으면 보안 세계에 설정 복원을 부탁한다 —
 * 전원이 끊겼다면 보안 쪽 상태도 사라졌기 때문이다.
 */
static int __maybe_unused qcom_iommu_resume(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev);	/* [한국어] 이 장치의 IOMMU. */
	int ret;	/* [한국어] 결과 코드. */

	ret = clk_bulk_prepare_enable(CLK_NUM, qcom_iommu->clks);	/* [한국어] 클럭을 모두 켠다. */
	if (ret < 0)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 포기한다. */

	if (dev->pm_domain)	/* [한국어] 전원 도메인이 있으면 전원이 실제로 끊겼을 수 있다. */
		return qcom_scm_restore_sec_cfg(qcom_iommu->sec_id, 0);	/* [한국어] 보안 세계에 설정 복원을 부탁한다. */

	return ret;	/* [한국어] 그 밖에는 클럭만 켜면 된다. */
}

/*
 * [한국어]
 * qcom_iommu_suspend - 전원을 끌 때
 *
 * @dev: IOMMU 장치.
 * @return: 늘 0.
 *
 * 클럭만 끄면 된다.
 */
static int __maybe_unused qcom_iommu_suspend(struct device *dev)
{
	struct qcom_iommu_dev *qcom_iommu = dev_get_drvdata(dev);	/* [한국어] 이 장치의 IOMMU. */

	clk_bulk_disable_unprepare(CLK_NUM, qcom_iommu->clks);	/* [한국어] 클럭을 모두 끈다. */

	return 0;	/* [한국어] 늘 성공. */
}

/*
 * [한국어] 전원 관리 연산표.
 *
 * 시스템 절전은 실행 중 전원 관리를 그대로 빌려 쓴다 — 두 경우에
 * 할 일이 같기 때문이다.
 */
static const struct dev_pm_ops qcom_iommu_pm_ops = {
	SET_RUNTIME_PM_OPS(qcom_iommu_suspend, qcom_iommu_resume, NULL)	/* [한국어] 실행 중 전원 관리. 마지막 NULL 은 유휴 콜백을 두지 않는다는 뜻이다. */
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,	/* [한국어] 시스템 절전은 실행 중 관리를 그대로 빌려 쓴다. */
				pm_runtime_force_resume)
};

/*
 * [한국어] IOMMU 노드의 이름들.
 */
static const struct of_device_id qcom_iommu_of_match[] = {
	{ .compatible = "qcom,msm-iommu-v1" },	/* [한국어] IOMMU 본체 노드의 이름들. */
	{ .compatible = "qcom,msm-iommu-v2" },
	{ /* sentinel */ }
};

/*
 * [한국어] IOMMU 본체를 잡는 플랫폼 드라이버.
 */
static struct platform_driver qcom_iommu_driver = {
	.driver	= {	/* [한국어] 드라이버 코어에 알릴 정보. */
		.name		= "qcom-iommu",	/* [한국어] 본체 드라이버의 이름. */
		.of_match_table	= qcom_iommu_of_match,
		.pm		= &qcom_iommu_pm_ops,
	},
	.probe	= qcom_iommu_device_probe,
	.remove = qcom_iommu_device_remove,
};

/*
 * [한국어]
 * qcom_iommu_init - 두 드라이버를 등록한다
 *
 * @return: 0 성공, 음수면 실패.
 *
 * 뱅크 드라이버를 먼저 등록해야 한다 — 본체가 자식 장치를 채울 때
 * 그것이 이미 있어야 곧바로 잡힌다.
 *
 * device_initcall 로 등록하는 데 주의. 모듈이 아니라 커널에 붙박이로
 * 들어가는 드라이버다.
 */
static int __init qcom_iommu_init(void)
{
	int ret;	/* [한국어] 결과 코드. */

	ret = platform_driver_register(&qcom_iommu_ctx_driver);	/* [한국어] 뱅크 드라이버를 먼저 등록해야 본체가 자식을 채울 때 곧바로 잡힌다. */
	if (ret)	/* [한국어] 실패하면 */
		return ret;	/* [한국어] 포기한다. */

	ret = platform_driver_register(&qcom_iommu_driver);	/* [한국어] 본체를 등록한다. */
	if (ret)	/* [한국어] 실패하면 */
		platform_driver_unregister(&qcom_iommu_ctx_driver);	/* [한국어] 뱅크 쪽도 되돌린다. */

	return ret;	/* [한국어] 결과. */
}
device_initcall(qcom_iommu_init);	/* [한국어] 모듈이 아니라 커널에 붙박이로 들어가는 드라이버다. */
