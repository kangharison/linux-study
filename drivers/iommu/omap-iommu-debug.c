// SPDX-License-Identifier: GPL-2.0-only
/*
 * omap iommu: debugfs interface
 *
 * Copyright (C) 2008-2009 Nokia Corporation
 *
 * Written by Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 */

/*
 * [한국어 설명] OMAP IOMMU 의 debugfs 창구 (omap-iommu-debug.c)
 *
 * === 파일의 역할 ===
 * 이 MMU 의 속을 사용자 공간에서 들여다볼 수 있게 debugfs 파일 몇 개를
 * 만든다. 레지스터 값, TLB 항목 목록, 그리고 페이지 테이블 전체를
 * 텍스트로 덤프한다.
 *
 * 왜 이런 것이 필요한가. DMA 가 엉뚱한 주소를 건드릴 때, 문제가 매핑을
 * 거는 코드에 있는지 테이블 자체가 망가진 것인지 가리기 어렵다. 표를
 * 직접 눈으로 보면 "커널이 의도한 매핑"과 "하드웨어가 실제로 보는 매핑"을
 * 견줄 수 있어, 그 경계를 곧바로 좁힐 수 있다.
 *
 * 셋 다 읽기 전용이고 소유자만 읽을 수 있게 열려 있다 — 페이지 테이블은
 * 물리 주소 배치를 그대로 드러내므로 아무나 읽게 두면 안 된다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 파일 배치는 이렇다:
 *
 *   /sys/kernel/debug/omap_iommu/<mmu 이름>/
 *     nr_tlb_entries  — 이 MMU 의 TLB 항목 수 (그냥 값 하나)
 *     regs            — 레지스터 덤프
 *     tlb             — TLB 항목 목록 (CAM, RAM, 잠김 여부)
 *     pagetable       — 페이지 테이블 전체 덤프
 *
 * MMU 가 프로브될 때 omap_iommu_debugfs_add() 가 그 디렉터리를 만들고,
 * 사라질 때 remove 가 지운다. 뿌리 디렉터리는 드라이버 초기화 때 한 번
 * 만들어진다.
 *
 * 실행 컨텍스트: 사용자가 파일을 읽을 때의 프로세스 문맥. 잠들 수 있다.
 *
 * === 타 모듈과의 연결 ===
 * - omap-iommu.h: MMU 구조체와 레지스터 정의, 그리고 TLB 를 읽는
 *   __iotlb_read_cr / iotlb_lock_get / iotlb_lock_set — 이 파일을 위해
 *   본체가 밖으로 열어 둔 함수들이다.
 * - omap-iopgtable.h: 페이지 테이블을 걷는 매크로들.
 * - omap-iommu.c: 여기 정의된 add/remove/init/exit 를 부른다.
 * - pm_runtime: MMU 는 쓰지 않을 때 꺼져 있어, 레지스터를 읽기 전에
 *   깨워야 한다.
 *
 * === 주요 함수/구조체 요약 ===
 * - debug_read_regs(): 레지스터를 이름과 함께 찍는다. pr_reg 매크로가
 *   그 반복을 줄여 준다.
 * - tlb_show(): TLB 항목을 훑어 유효한 것만 찍는다. 읽는 동안 잠금
 *   레지스터를 건드리므로, 원래 값을 보관했다 되돌린다.
 * - pagetable_show()/dump_ioptable(): 표를 두 단계로 걸어가며 비어 있지
 *   않은 항목만 찍는다. 1단계 섹션과 2단계 페이지를 앞의 숫자로 구분한다.
 * - omap_iommu_debugfs_add()/remove(): MMU 하나의 디렉터리를 만들고 지운다.
 */

#include <linux/err.h>	/* [한국어] 오류 코드. */
#include <linux/io.h>	/* [한국어] 레지스터 접근. */
#include <linux/slab.h>	/* [한국어] 덤프 버퍼를 잡는다. */
#include <linux/uaccess.h>	/* [한국어] 사용자 공간으로 내보내는 헬퍼. */
#include <linux/pm_runtime.h>	/* [한국어] 꺼져 있는 MMU 를 깨워야 레지스터를 읽을 수 있다. */
#include <linux/debugfs.h>	/* [한국어] 파일과 디렉터리를 만든다. */
#include <linux/platform_data/iommu-omap.h>	/* [한국어] 이 하드웨어의 플랫폼 자료 정의. */

#include "omap-iopgtable.h"	/* [한국어] 페이지 테이블을 걷는 매크로들. */
#include "omap-iommu.h"	/* [한국어] MMU 구조체와 레지스터 정의. */

static DEFINE_MUTEX(iommu_debug_lock);	/* [한국어] 덤프끼리 겹치지 않게 막는 락. TLB 잠금 레지스터를 건드리는 덤프가 둘 겹치면 서로의 자리를 어긋나게 만든다. */

static struct dentry *iommu_debug_root;	/* [한국어] 모든 MMU 의 디렉터리를 담는 뿌리. NULL 이면 debugfs 가 준비되지 않은 것이다. */

/*
 * [한국어]
 * is_omap_iommu_detached - 이 MMU 가 어느 도메인에도 붙어 있지 않은가
 *
 * @obj: 검사할 MMU.
 * @return: 붙어 있지 않으면 참.
 *
 * 붙어 있지 않으면 페이지 테이블도 없고 레지스터를 읽어도 뜻이 없다.
 * 그래서 모든 덤프가 먼저 이것을 확인하고 거부한다.
 *
 * 실행 컨텍스트: 덤프 경로. 잠들지 않는다.
 *
 * 호출 체인:
 *   debug_read_regs()/tlb_show()/pagetable_show() → [이 함수]
 */
static inline bool is_omap_iommu_detached(struct omap_iommu *obj)
{
	return !obj->domain;	/* [한국어] 도메인이 없으면 떨어져 있는 것이다. */
}

/* [한국어] 레지스터 하나를 이름과 함께 찍는 매크로.
 *
 * 레지스터가 열여섯 개라 같은 코드를 반복해야 하는데, 이름 문자열과
 * 레지스터 상수를 매크로 인자 하나에서 함께 만들어 낸다 —
 * __stringify 로 이름을, MMU_##name 으로 상수를 얻는다.
 *
 * 버퍼가 모자라면 goto 로 빠져나가는 것이 눈에 띈다. 매크로 안에서
 * 바깥의 레이블로 뛰는 방식이라 호출부가 반드시 out: 을 갖고 있어야
 * 하지만, 그 대신 매번 길이를 검사하는 코드를 쓰지 않아도 된다. */
#define pr_reg(name)							\
	do {								\
		ssize_t bytes;						\
		const char *str = "%20s: %08x\n";			\
		const int maxcol = 32;					\
		if (len < maxcol)					\
			goto out;					\
		bytes = scnprintf(p, maxcol, str, __stringify(name),	\
				 iommu_read_reg(obj, MMU_##name));	\
		p += bytes;						\
		len -= bytes;						\
	} while (0)	/* [한국어] do-while 로 감싸 조건문 안에서도 한 문장처럼 쓸 수 있게 한다. */

/*
 * [한국어]
 * omap2_iommu_dump_ctx - 레지스터를 모두 읽어 버퍼에 찍는다
 *
 * @obj: 대상 MMU.
 * @buf: 담을 버퍼.
 * @len: 그 크기.
 * @return: 실제로 쓴 바이트 수.
 *
 * 위 매크로를 레지스터마다 한 번씩 부른다. 버퍼가 모자라면 매크로가
 * out 레이블로 뛰어, 거기까지 쓴 만큼만 돌려준다.
 *
 * 순서가 레지스터 배치 순서와 같아, 덤프를 규격서와 나란히 놓고 보기 좋다.
 *
 * 실행 컨텍스트: 덤프 경로, MMU 가 깨어 있는 상태. 잠들지 않는다.
 *
 * 호출 체인:
 *   omap_iommu_dump_ctx() → [이 함수] → iommu_read_reg()
 */
static ssize_t
omap2_iommu_dump_ctx(struct omap_iommu *obj, char *buf, ssize_t len)
{
	char *p = buf;	/* [한국어] 쓰기 커서 — 매크로가 이 이름을 그대로 쓴다. */

	pr_reg(REVISION);	/* [한국어] 하드웨어 개정 번호. */
	pr_reg(IRQSTATUS);	/* [한국어] 지금 걸려 있는 인터럽트 원인 — 폴트를 진단할 때 첫 단서다. */
	pr_reg(IRQENABLE);	/* [한국어] 어느 원인을 받고 있는가. */
	pr_reg(WALKING_ST);	/* [한국어] 지금 표를 걷는 중인가. */
	pr_reg(CNTL);	/* [한국어] MMU 가 켜져 있는지, 표 걷기를 쓰는지. */
	pr_reg(FAULT_AD);	/* [한국어] 폴트가 난 주소 — 어느 접근이 실패했는지 알려 준다. */
	pr_reg(TTB);	/* [한국어] 하드웨어가 보고 있는 페이지 테이블 뿌리 — 커널이 의도한 값과 다르면 그 자체가 버그다. */
	pr_reg(LOCK);	/* [한국어] TLB 잠금 설정. */
	pr_reg(LD_TLB);	/* [한국어] 항목 넣기 명령 레지스터. */
	pr_reg(CAM);	/* [한국어] 넣을 항목의 찾기 쪽 워드. */
	pr_reg(RAM);	/* [한국어] 넣을 항목의 결과 쪽 워드. */
	pr_reg(GFLUSH);	/* [한국어] 전체 비우기 명령. */
	pr_reg(FLUSH_ENTRY);	/* [한국어] 항목 하나 비우기 명령. */
	pr_reg(READ_CAM);	/* [한국어] 읽어 온 항목의 찾기 쪽 워드. */
	pr_reg(READ_RAM);	/* [한국어] 읽어 온 항목의 결과 쪽 워드. */
	pr_reg(EMU_FAULT_AD);	/* [한국어] 에뮬레이션 접근이 실패한 주소. */
out:	/* [한국어] 버퍼가 모자라 매크로가 뛰어오는 자리. */
	return p - buf;	/* [한국어] 커서가 움직인 만큼이 쓴 길이다. */
}

/*
 * [한국어]
 * omap_iommu_dump_ctx - MMU 를 깨워 레지스터를 덤프한다
 *
 * @obj: 대상 MMU.
 * @buf: 담을 버퍼.
 * @bytes: 그 크기.
 * @return: 쓴 바이트 수, 또는 -EINVAL.
 *
 * 레지스터를 읽으려면 MMU 가 켜져 있어야 한다. 이 SoC 들은 전력을
 * 아끼려고 쓰지 않는 블록을 꺼 두므로, 읽기 전에 깨우고 끝나면 다시
 * 놓아 준다. 꺼진 상태에서 레지스터를 읽으면 버스 오류가 나거나
 * 쓰레기 값이 나온다.
 *
 * 실행 컨텍스트: 덤프 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   debug_read_regs() → [이 함수] → omap2_iommu_dump_ctx()
 */
static ssize_t omap_iommu_dump_ctx(struct omap_iommu *obj, char *buf,
				   ssize_t bytes)
{
	if (!obj || !buf)	/* [한국어] 인자가 잘못됐다면. */
		return -EINVAL;

	pm_runtime_get_sync(obj->dev);	/* [한국어] 꺼져 있던 MMU 를 깨운다 — 이 전에 레지스터를 읽으면 버스 오류가 난다. */

	bytes = omap2_iommu_dump_ctx(obj, buf, bytes);	/* [한국어] 실제 덤프. */

	pm_runtime_put_sync(obj->dev);	/* [한국어] 다시 놓아 준다 — 아무도 쓰지 않으면 꺼진다. */

	return bytes;	/* [한국어] 쓴 길이를 그대로 돌려준다. */
}

/*
 * [한국어]
 * debug_read_regs - regs 파일을 읽을 때의 처리
 *
 * @file: 열린 파일 (private_data 에 MMU 포인터가 들어 있다).
 * @userbuf: 사용자 공간 버퍼.
 * @count: 요청한 길이.
 * @ppos: 파일 오프셋.
 * @return: 읽은 바이트 수, 또는 음수 오류.
 *
 * 커널 버퍼에 덤프를 만든 뒤 사용자 공간으로 옮긴다. 사용자 버퍼에
 * 직접 쓰지 않는 이유는, 그 쓰기가 페이지 폴트를 낼 수 있어 락을 쥔
 * 채로는 위험하기 때문이다.
 *
 * 요청한 길이만큼 버퍼를 잡는 것이 눈에 띈다 — 사용자가 큰 값을 주면
 * 그만큼 잡히므로 이 파일이 소유자 전용인 이유이기도 하다.
 *
 * 실행 컨텍스트: 사용자 읽기. 잠들 수 있다.
 *
 * 호출 체인:
 *   VFS read → .read = [이 함수] → omap_iommu_dump_ctx()
 */
static ssize_t debug_read_regs(struct file *file, char __user *userbuf,
			       size_t count, loff_t *ppos)
{
	struct omap_iommu *obj = file->private_data;	/* [한국어] 파일을 만들 때 붙여 둔 MMU. */
	char *p, *buf;	/* [한국어] 커널 쪽 덤프 버퍼와 그 커서. */
	ssize_t bytes;	/* [한국어] 결과. */

	if (is_omap_iommu_detached(obj))	/* [한국어] 어느 도메인에도 붙어 있지 않다면. */
		return -EPERM;	/* [한국어] 읽어도 뜻이 없다 — 꺼져 있을 수도 있다. */

	buf = kmalloc(count, GFP_KERNEL);	/* [한국어] 요청한 만큼 잡는다. */
	if (!buf)	/* [한국어] 버퍼를 못 잡았으면. */
		return -ENOMEM;
	p = buf;	/* [한국어] 커서를 처음에 놓는다. */

	mutex_lock(&iommu_debug_lock);	/* [한국어] 다른 덤프와 겹치지 않게 막는다. */

	bytes = omap_iommu_dump_ctx(obj, p, count);	/* [한국어] 커널 버퍼에 덤프를 만든다. */
	if (bytes < 0)	/* [한국어] 실패했다면. */
		goto err;
	bytes = simple_read_from_buffer(userbuf, count, ppos, buf, bytes);	/* [한국어] 오프셋을 고려해 사용자 공간으로 옮긴다 — 여러 번 나눠 읽어도 이어진다. */

err:	/* [한국어] 성공·실패 모두 이 자리를 지난다. */
	mutex_unlock(&iommu_debug_lock);	/* [한국어] 덤프가 끝났으니 락을 놓는다. */
	kfree(buf);	/* [한국어] 커널 버퍼를 놓는다. */

	return bytes;	/* [한국어] 옮긴 길이 또는 오류를 돌려준다. */
}

/*
 * [한국어]
 * __dump_tlb_entries - TLB 항목을 모두 읽어 배열에 담는다
 *
 * @obj: 대상 MMU.
 * @crs: 담을 배열.
 * @num: 훑을 자리 수.
 * @return: 실제로 담은 개수 (유효한 항목만 센다).
 *
 * TLB 를 읽는 일이 순수한 읽기가 아니라는 점이 이 함수의 요점이다.
 * 항목을 읽으려면 잠금 레지스터로 그 자리를 가리켜야 하고, 그 레지스터는
 * 매핑을 거는 경로도 쓴다. 그래서 원래 값을 보관해 두었다가 끝나면
 * 반드시 되돌려야 한다 — 그러지 않으면 다음 매핑이 엉뚱한 자리를 덮어쓴다.
 *
 * 실행 컨텍스트: 덤프 경로. 잠들 수 있다(MMU 를 깨운다).
 *
 * 호출 체인:
 *   omap_dump_tlb_entries() → [이 함수] → __iotlb_read_cr()
 */
static int
__dump_tlb_entries(struct omap_iommu *obj, struct cr_regs *crs, int num)
{
	int i;	/* [한국어] TLB 자리 반복자. */
	struct iotlb_lock saved;	/* [한국어] 원래 잠금 설정 — 끝나면 되돌린다. */
	struct cr_regs tmp;	/* [한국어] 읽어 온 항목 (매크로가 이 이름에 담는다). */
	struct cr_regs *p = crs;	/* [한국어] 담을 자리를 가리키는 커서. */

	pm_runtime_get_sync(obj->dev);	/* [한국어] 레지스터를 읽으려면 깨워야 한다. */
	iotlb_lock_get(obj, &saved);	/* [한국어] 원래 설정을 보관해 둔다 — 아래에서 자리를 옮겨 가며 읽기 때문이다. */

	for_each_iotlb_cr(obj, num, i, tmp) {	/* [한국어] 자리를 옮겨 가며 항목을 읽는다. */
		if (!iotlb_cr_valid(&tmp))	/* [한국어] 빈 자리라면. */
			continue;	/* [한국어] 담지 않는다. */
		*p++ = tmp;	/* [한국어] 유효한 항목만 배열에 담는다. */
	}

	iotlb_lock_set(obj, &saved);	/* [한국어] 원래 설정으로 되돌린다 — 빠뜨리면 다음 매핑이 엉뚱한 자리를 덮어쓴다. */
	pm_runtime_put_sync(obj->dev);	/* [한국어] 다시 놓아 준다. */

	return  p - crs;	/* [한국어] 커서가 움직인 만큼이 담긴 개수다. */
}

/*
 * [한국어]
 * iotlb_dump_cr - TLB 항목 하나를 한 줄로 찍는다
 *
 * @obj: 대상 MMU (쓰지 않는다).
 * @cr: 찍을 항목.
 * @s: 출력할 seq 파일.
 * @return: 항상 0.
 *
 * 두 워드를 16진수로 그대로 찍고, 잠긴 항목인지만 따로 표시한다.
 * 비트를 풀어 보여 주지 않는 이유는 규격서와 대조하기에는 원본이
 * 낫기 때문이다.
 *
 * 실행 컨텍스트: 덤프 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   omap_dump_tlb_entries() → [이 함수]
 */
static ssize_t iotlb_dump_cr(struct omap_iommu *obj, struct cr_regs *cr,
			     struct seq_file *s)
{
	seq_printf(s, "%08x %08x %01x\n", cr->cam, cr->ram,	/* [한국어] 두 워드를 원본 그대로 — 규격서와 대조하기 좋다. */
		   (cr->cam & MMU_CAM_P) ? 1 : 0);	/* [한국어] 잠긴 항목인지만 풀어 보여 준다 — 밀려나지 않는 항목이라 눈에 띄어야 한다. */
	return 0;	/* [한국어] 항목 하나를 찍었다. */
}

/*
 * [한국어]
 * omap_dump_tlb_entries - TLB 를 읽어 모두 찍는다
 *
 * @obj: 대상 MMU.
 * @s: 출력할 seq 파일.
 * @return: 항상 0.
 *
 * 읽기와 찍기를 나눈 것이 요점이다. 읽는 동안에는 하드웨어 잠금
 * 레지스터를 건드리므로 그 구간을 짧게 유지해야 하고, 찍는 일은
 * seq 파일 계층이 잠들 수 있는 자리라 그 구간에서 하면 안 된다.
 * 그래서 배열에 먼저 담고 나중에 찍는다.
 *
 * 실행 컨텍스트: 덤프 경로. 잠들 수 있다.
 *
 * 호출 체인:
 *   tlb_show() → [이 함수] → __dump_tlb_entries()
 */
static size_t omap_dump_tlb_entries(struct omap_iommu *obj, struct seq_file *s)
{
	int i, num;	/* [한국어] 반복자와 항목 수. */
	struct cr_regs *cr;	/* [한국어] 읽어 담을 배열. */

	num = obj->nr_tlb_entries;	/* [한국어] 이 MMU 의 TLB 자리 수. */

	cr = kzalloc_objs(*cr, num);	/* [한국어] 최악의 경우(모두 유효)를 담을 만큼 잡는다. */
	if (!cr)	/* [한국어] 메모리가 없으면. */
		return 0;	/* [한국어] 아무것도 찍지 않는다 — 덤프는 실패해도 시스템에 영향이 없다. */

	num = __dump_tlb_entries(obj, cr, num);	/* [한국어] 하드웨어를 읽는 구간 — 짧게 끝낸다. */
	for (i = 0; i < num; i++)	/* [한국어] 담긴 항목을. */
		iotlb_dump_cr(obj, cr + i, s);	/* [한국어] 하나씩 찍는다 — 이 구간은 잠들 수 있다. */
	kfree(cr);	/* [한국어] 배열을 놓는다. */

	return 0;	/* [한국어] 찍기가 끝났다. */
}

/*
 * [한국어]
 * tlb_show - tlb 파일을 읽을 때의 처리
 *
 * @s: seq 파일 (private 에 MMU 포인터가 들어 있다).
 * @data: 쓰지 않는다.
 * @return: 0 성공, -EPERM 붙어 있지 않음.
 *
 * 머리글을 찍고 TLB 덤프를 이어 붙인다. 어느 매핑이 캐시에 올라와 있는지
 * 보여 주므로, 무효화가 제대로 되고 있는지 확인하는 데 쓴다 — 풀었는데도
 * 여기 남아 있으면 무효화가 빠진 것이다.
 *
 * 실행 컨텍스트: 사용자 읽기. 잠들 수 있다.
 *
 * 호출 체인:
 *   VFS read → seq_file → [이 함수] → omap_dump_tlb_entries()
 */
static int tlb_show(struct seq_file *s, void *data)
{
	struct omap_iommu *obj = s->private;	/* [한국어] 파일을 만들 때 붙여 둔 MMU. */

	if (is_omap_iommu_detached(obj))	/* [한국어] 붙어 있지 않으면. */
		return -EPERM;	/* [한국어] 읽어도 뜻이 없다. */

	mutex_lock(&iommu_debug_lock);	/* [한국어] 다른 덤프와 겹치면 잠금 레지스터가 어긋난다. */

	seq_printf(s, "%8s %8s\n", "cam:", "ram:");	/* [한국어] 두 열의 머리글. */
	seq_puts(s, "-----------------------------------------\n");	/* [한국어] 눈으로 읽기 좋게 구분선을 넣는다. */
	omap_dump_tlb_entries(obj, s);	/* [한국어] 항목들을 찍는다. */

	mutex_unlock(&iommu_debug_lock);	/* [한국어] 덤프가 끝났으니 락을 놓는다. */

	return 0;	/* [한국어] 출력이 완성됐다. */
}

/*
 * [한국어]
 * dump_ioptable - 페이지 테이블을 두 단계로 걸어가며 찍는다
 *
 * @s: seq 파일 (private 에 MMU 포인터가 들어 있다).
 *
 * 표를 처음부터 끝까지 훑되 비어 있지 않은 항목만 찍는다. 4096개의
 * 1단계 항목을 모두 돌지만 대부분 비어 있어 출력은 짧다.
 *
 * 각 줄 앞의 숫자가 단계를 알려 준다. 1 이면 1단계 항목이 직접 매핑한
 * 것(섹션 또는 슈퍼섹션)이고, 2 면 2단계 표를 거친 페이지다. 그 구분이
 * 중요한 이유는, 큰 매핑이 의도대로 큰 단위로 걸렸는지 확인할 수 있기
 * 때문이다 — 정렬이 어긋나 잘게 쪼개졌다면 성능이 크게 떨어진다.
 *
 * 실행 컨텍스트: 덤프 경로. 스핀락을 잡는다.
 *
 * 호출 체인:
 *   pagetable_show() → [이 함수]
 */
static void dump_ioptable(struct seq_file *s)
{
	int i, j;	/* [한국어] 1단계와 2단계 반복자. */
	u32 da;	/* [한국어] 그 항목이 담당하는 장치 주소. */
	u32 *iopgd, *iopte;	/* [한국어] 1단계와 2단계 항목을 가리키는 커서. */
	struct omap_iommu *obj = s->private;	/* [한국어] 대상 MMU. */

	spin_lock(&obj->page_table_lock);	/* [한국어] 훑는 동안 표가 바뀌면 안 된다 — 2단계 표가 사라지면 널 포인터를 밟는다. */

	iopgd = iopgd_offset(obj, 0);	/* [한국어] 표의 처음부터. */
	for (i = 0; i < PTRS_PER_IOPGD; i++, iopgd++) {	/* [한국어] 1단계 항목 4096개를 모두 본다. */
		if (!*iopgd)	/* [한국어] 비어 있으면. */
			continue;	/* [한국어] 찍을 것이 없다 — 대부분이 이쪽이라 출력은 짧다. */

		if (!(*iopgd & IOPGD_TABLE)) {	/* [한국어] 2단계 표를 가리키는 것이 아니라면 — 곧 섹션이나 슈퍼섹션이라면. */
			da = i << IOPGD_SHIFT;	/* [한국어] 첨자에서 그 항목이 담당하는 주소를 되구한다. */
			seq_printf(s, "1: 0x%08x 0x%08x\n", da, *iopgd);	/* [한국어] 앞의 1 이 "1단계에서 끝났다"는 뜻 — 큰 매핑이 걸렸음을 보여 준다. */
			continue;	/* [한국어] 아래로 내려갈 표가 없다. */
		}

		iopte = iopte_offset(iopgd, 0);	/* [한국어] 그 항목이 가리키는 2단계 표의 처음부터. */
		for (j = 0; j < PTRS_PER_IOPTE; j++, iopte++) {	/* [한국어] 2단계 항목 256개를 모두 본다. */
			if (!*iopte)	/* [한국어] 비어 있으면 건너뛴다. */
				continue;

			da = (i << IOPGD_SHIFT) + (j << IOPTE_SHIFT);	/* [한국어] 두 첨자를 합쳐 그 항목이 담당하는 주소를 되구한다. */
			seq_printf(s, "2: 0x%08x 0x%08x\n", da, *iopte);	/* [한국어] 앞의 2 가 "2단계까지 내려갔다"는 뜻이다. */
		}
	}

	spin_unlock(&obj->page_table_lock);	/* [한국어] 훑기가 끝났으니 락을 놓는다. */
}

/*
 * [한국어]
 * pagetable_show - pagetable 파일을 읽을 때의 처리
 *
 * @s: seq 파일.
 * @data: 쓰지 않는다.
 * @return: 0 성공, -EPERM 붙어 있지 않음.
 *
 * 머리글을 찍고 표 덤프를 이어 붙인다. 커널이 의도한 매핑과 표에 실제로
 * 적힌 것을 견줄 수 있어, DMA 문제를 가릴 때 가장 직접적인 증거가 된다.
 *
 * 실행 컨텍스트: 사용자 읽기. 잠들 수 있다.
 *
 * 호출 체인:
 *   VFS read → seq_file → [이 함수] → dump_ioptable()
 */
static int pagetable_show(struct seq_file *s, void *data)
{
	struct omap_iommu *obj = s->private;	/* [한국어] 대상 MMU. */

	if (is_omap_iommu_detached(obj))	/* [한국어] 붙어 있지 않으면 표도 없다. */
		return -EPERM;

	mutex_lock(&iommu_debug_lock);	/* [한국어] 다른 덤프와 겹치지 않게 막는다. */

	seq_printf(s, "L: %8s %8s\n", "da:", "pte:");	/* [한국어] 단계·주소·항목 세 열의 머리글. */
	seq_puts(s, "--------------------------\n");	/* [한국어] 구분선. */
	dump_ioptable(s);	/* [한국어] 표를 걸어가며 찍는다. */

	mutex_unlock(&iommu_debug_lock);	/* [한국어] 덤프가 끝났으니 락을 놓는다. */

	return 0;	/* [한국어] 출력이 완성됐다. */
}

/* [한국어] 읽기 전용 debugfs 파일의 연산표를 만드는 매크로.
 *
 * seq_file 을 쓰지 않는 파일(regs)이 하나뿐이라 이 매크로도 한 번만
 * 쓰이지만, 이름 규칙을 코드로 못 박아 두는 뜻이 있다. */
#define DEBUG_FOPS_RO(name)						\
	static const struct file_operations name##_fops = {	        \
		.open = simple_open,					\
		.read = debug_read_##name,				\
		.llseek = generic_file_llseek,				\
	}

DEBUG_FOPS_RO(regs);	/* [한국어] regs 파일의 연산표 — 위 debug_read_regs 를 건다. */
DEFINE_SHOW_ATTRIBUTE(tlb);	/* [한국어] tlb 파일의 연산표 — seq_file 뼈대가 tlb_show 를 부르게 한다. */
DEFINE_SHOW_ATTRIBUTE(pagetable);	/* [한국어] pagetable 파일도 같은 방식. */

/*
 * [한국어]
 * omap_iommu_debugfs_add - MMU 하나의 debugfs 항목을 만든다
 *
 * @obj: 대상 MMU.
 *
 * 그 MMU 이름의 디렉터리를 만들고 그 아래 파일 넷을 단다. 모두 0400 —
 * 소유자만 읽을 수 있다. 페이지 테이블이 물리 주소 배치를 그대로
 * 드러내므로 아무나 읽게 두면 안 되기 때문이다.
 *
 * 뿌리 디렉터리가 없으면 조용히 물러난다 — debugfs 를 끄고 빌드했거나
 * 마운트되지 않은 경우이며, 실패로 볼 이유가 없다.
 *
 * 실행 컨텍스트: MMU 프로브. 잠들 수 있다.
 *
 * 호출 체인:
 *   omap_iommu_probe() → [이 함수] → debugfs_create_*()
 */
void omap_iommu_debugfs_add(struct omap_iommu *obj)
{
	struct dentry *d;	/* [한국어] 만든 디렉터리. */

	if (!iommu_debug_root)	/* [한국어] 뿌리가 없으면 — debugfs 가 없거나 초기화 전이다. */
		return;	/* [한국어] 조용히 물러난다. */

	d = debugfs_create_dir(obj->name, iommu_debug_root);	/* [한국어] MMU 이름으로 디렉터리를 만든다. */
	obj->debug_dir = d;	/* [한국어] 나중에 통째로 지울 수 있게 기억해 둔다. */

	debugfs_create_u32("nr_tlb_entries", 0400, d, &obj->nr_tlb_entries);	/* [한국어] 값 하나짜리 파일 — 아래 tlb 덤프를 해석하는 기준이 된다. */
	debugfs_create_file("regs", 0400, d, obj, &regs_fops);	/* [한국어] 레지스터 덤프. */
	debugfs_create_file("tlb", 0400, d, obj, &tlb_fops);	/* [한국어] TLB 덤프. */
	debugfs_create_file("pagetable", 0400, d, obj, &pagetable_fops);	/* [한국어] 페이지 테이블 덤프 — 물리 주소가 그대로 드러나 소유자 전용이다. */
}

/*
 * [한국어]
 * omap_iommu_debugfs_remove - MMU 하나의 debugfs 항목을 지운다
 *
 * @obj: 대상 MMU.
 *
 * 디렉터리를 통째로 지운다. 그 아래 파일들도 함께 사라지므로 하나씩
 * 지울 필요가 없다.
 *
 * 실행 컨텍스트: MMU 해제. 잠들 수 있다.
 *
 * 호출 체인:
 *   omap_iommu_remove() → [이 함수] → debugfs_remove_recursive()
 */
void omap_iommu_debugfs_remove(struct omap_iommu *obj)
{
	if (!obj->debug_dir)	/* [한국어] 만든 적이 없다면. */
		return;	/* [한국어] 지울 것도 없다. */

	debugfs_remove_recursive(obj->debug_dir);	/* [한국어] 디렉터리와 그 아래 파일을 한 번에 지운다. */
}

/*
 * [한국어]
 * omap_iommu_debugfs_init - 모든 MMU 를 담을 뿌리 디렉터리를 만든다
 *
 * 드라이버가 올라올 때 한 번 불린다. 실패해도 검사하지 않는데,
 * 실패하면 뿌리가 NULL 로 남아 위 add 가 조용히 물러나기 때문이다 —
 * debugfs 가 없다고 드라이버가 동작하지 않을 이유는 없다.
 *
 * 실행 컨텍스트: 드라이버 초기화(__init). 잠들 수 있다.
 *
 * 호출 체인:
 *   omap_iommu_init() → [이 함수] → debugfs_create_dir()
 */
void __init omap_iommu_debugfs_init(void)
{
	iommu_debug_root = debugfs_create_dir("omap_iommu", NULL);	/* [한국어] debugfs 뿌리 아래에 만든다. 실패하면 NULL 이 남아 이후 add 가 조용히 넘어간다. */
}

/*
 * [한국어]
 * omap_iommu_debugfs_exit - 그 뿌리 디렉터리를 지운다
 *
 * 드라이버가 내려갈 때 불린다. 이 시점에는 각 MMU 의 디렉터리가 이미
 * 지워져 있어 뿌리 하나만 지우면 된다.
 *
 * 실행 컨텍스트: 드라이버 해제(__exit). 잠들 수 있다.
 *
 * 호출 체인:
 *   omap_iommu_exit() → [이 함수] → debugfs_remove()
 */
void __exit omap_iommu_debugfs_exit(void)
{
	debugfs_remove(iommu_debug_root);	/* [한국어] 아래가 비어 있으므로 재귀 판이 필요 없다. */
}
