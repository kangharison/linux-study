/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * omap iommu: main structures
 *
 * Copyright (C) 2008-2009 Nokia Corporation
 *
 * Written by Hiroshi DOYU <Hiroshi.DOYU@nokia.com>
 */

/*
 * [한국어 설명] TI OMAP IOMMU 의 자료 구조와 레지스터 정의 (omap-iommu.h)
 *
 * === 파일의 역할 ===
 * TI OMAP 계열 SoC 에 들어가는 IOMMU(칩 안에서는 그냥 "MMU"라 부른다)의
 * 커널 자료 구조와 레지스터 배치를 모아 둔 헤더다. 드라이버 본체와
 * debugfs 파일이 이것을 함께 쓴다.
 *
 * 이 하드웨어의 성격이 다른 IOMMU 와 꽤 다르다. 요즘 IOMMU 는 하드웨어가
 * 페이지 테이블을 스스로 걸어가고 TLB 는 그 캐시일 뿐인데, 이 MMU 는
 * 그 두 방식을 모두 쓸 수 있다. TWL(Table Walking Logic)을 켜면 표를
 * 스스로 걷고, 끄면 소프트웨어가 TLB 항목을 직접 채워 넣어야 한다.
 * 그래서 이 헤더에 TLB 항목을 손으로 다루는 구조체(cr_regs, iotlb_lock)와
 * 그 잠금 장치가 함께 들어 있다.
 *
 * TLB 항목은 CAM 과 RAM 두 워드로 되어 있다. CAM 은 "어느 주소를 찾는가"
 * (가상 주소 태그와 페이지 크기), RAM 은 "어디로 보내는가"(물리 주소와
 * 엔디안·요소 크기 같은 속성)를 담는다. 이름 그대로 내용 주소화 메모리와
 * 그에 딸린 자료 메모리다.
 *
 * 페이지 크기는 네 가지(4KB/64KB/1MB/16MB)이며, 그 형식은 짝이 되는
 * omap-iopgtable.h 에 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 장치가 낸 DMA 는 이렇게 흘러간다:
 *
 *   장치 주소(da)
 *     → MMU 의 TLB 에서 찾기 (CAM 태그 비교)
 *     → 맞으면 RAM 이 알려 주는 물리 주소로
 *     → 없으면 TWL 이 페이지 테이블을 걷거나, 소프트웨어에 폴트를 알린다
 *
 * 커널 쪽에서는 iommu 코어 → omap-iommu.c 의 연산표 → 이 헤더의 레지스터
 * 접근자 → 하드웨어 순으로 내려간다. 페이지 테이블은 io-pgtable 을 쓰지
 * 않고 드라이버가 직접 짓는다 — 이 드라이버가 그만큼 오래됐기 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * - omap-iommu.c: 여기 정의된 구조체와 레지스터를 실제로 다루는 본체.
 * - omap-iopgtable.h: 페이지 테이블 형식 — 표를 걷는 매크로들이 거기 있다.
 * - omap-iommu-debug.c: debugfs 로 TLB 와 표를 덤프한다. 위 for_each_iotlb_cr
 *   매크로와 __iotlb_read_cr 이 그것을 위해 밖으로 노출되어 있다.
 * - iommu 코어: iommu_device 와 iommu_domain 을 통해 만난다.
 * - regmap(syscfg): DRA7xx 의 DSP MMU 는 별도 시스템 레지스터로 켜야 해서
 *   그쪽 접근 통로를 함께 들고 있다.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct omap_iommu: MMU 하드웨어 하나. 레지스터 창, 페이지 테이블 뿌리,
 *   TLB 항목 수, 절전 문맥 저장 자리를 쥔다.
 * - struct omap_iommu_domain: 주소 공간 하나. 여러 MMU 가 한 도메인을
 *   공유할 수 있어 배열로 들고 있다.
 * - struct cr_regs: TLB 항목 하나 (CAM 과 RAM 두 워드).
 * - struct iotlb_lock: TLB 의 어느 자리를 다음에 덮어쓸지 정하는 값.
 * - MMU_* 매크로: 레지스터 오프셋과 비트 정의.
 * - iopgsz_max / bytes_to_iopgsz / iopgsz_to_bytes: 크기와 하드웨어 코드
 *   사이를 오가는 변환.
 * - iommu_read_reg / iommu_write_reg: 레지스터 접근자.
 */

#ifndef _OMAP_IOMMU_H	/* [한국어] 이 헤더가 두 번 펼쳐지는 것을 막는 보호 매크로. */
#define _OMAP_IOMMU_H	/* [한국어] 처음 펼쳐질 때 표시를 남긴다. */

#include <linux/bitops.h>	/* [한국어] 아래 비트 정의가 쓰는 BIT() 매크로. */
#include <linux/iommu.h>	/* [한국어] iommu 코어의 iommu_device 와 iommu_domain. */

/* [한국어] TLB 항목을 처음부터 끝까지 훑는 매크로.
 *
 * 조건식 안에서 항목을 읽어 cr 에 담는 것이 요령이다 — 쉼표 연산자로
 * 읽기를 끼워 넣고 언제나 참을 돌려주어, 반복 조건과 읽기를 한 자리에
 * 묶었다. debugfs 가 TLB 를 덤프할 때 쓴다. */
#define for_each_iotlb_cr(obj, n, __i, cr)				\
	for (__i = 0;							\
	     (__i < (n)) && (cr = __iotlb_read_cr((obj), __i), true);	\
	     __i++)

/* [한국어] TLB 에 넣을 항목 하나를 사람이 다루기 좋은 형태로 펼친 구조.
 *
 * 하드웨어의 CAM/RAM 두 워드는 비트가 빽빽이 들어차 있어 그대로 다루기
 * 어렵다. 그래서 매핑을 걸 때는 이 구조로 채운 뒤 하드웨어 형식으로
 * 짜 넣는다. */
struct iotlb_entry {
	/* [한국어] 장치가 내는 주소 (device address).
	 * 설정자: 매핑을 걸 때 호출자가 준 iova.
	 * 읽는 자: CAM 워드의 태그 자리에 담긴다.
	 * 값 범위: 페이지 크기에 맞춰 정렬되어야 한다.
	 * 동기화: 지역 변수로만 쓰여 공유되지 않는다. */
	u32 da;
	/* [한국어] 그 주소가 이어질 물리 주소.
	 * 설정자: 위와 같은 자리.
	 * 읽는 자: RAM 워드의 주소 자리에 담긴다.
	 * 값 범위: 역시 정렬되어야 한다.
	 * 동기화: 없음. */
	u32 pa;
	/* [한국어] 페이지 크기 코드, 예약 여부, 유효 여부.
	 * 설정자: 매핑을 걸 때 크기에서 코드를 구해 채운다.
	 * 읽는 자: CAM 워드에 함께 담긴다 — prsvd 가 서면 그 항목은
	 *         TLB 에서 밀려나지 않는다.
	 * 값 범위: pgsz 는 MMU_CAM_PGSZ_* 중 하나.
	 * 동기화: 없음. */
	u32 pgsz, prsvd, valid;
	/* [한국어] 엔디안, 요소 크기, 혼합 여부.
	 * 설정자: 매핑을 걸 때 정한다 — 이 MMU 는 변환하면서 바이트 순서까지
	 *         바꿔 줄 수 있다.
	 * 읽는 자: RAM 워드에 담긴다.
	 * 값 범위: MMU_RAM_ENDIAN_* / MMU_RAM_ELSZ_* 중 하나.
	 * 동기화: 없음.
	 *         (요즘 IOMMU 에는 없는 기능으로, DSP 처럼 바이트 순서가
	 *          다른 코어와 메모리를 나눠 쓰던 시절의 흔적이다.) */
	u32 endian, elsz, mixed;
};

/**
 * struct omap_iommu_device - omap iommu device data
 * @pgtable:	page table used by an omap iommu attached to a domain
 * @iommu_dev:	pointer to store an omap iommu instance attached to a domain
 */
/* [한국어] 도메인 하나에 붙은 MMU 하나의 정보. (위 영어 kernel-doc 참고)
 *
 * 한 도메인에 여러 MMU 가 붙을 수 있어 — 예컨대 DSP 와 IPU 가 같은
 * 주소 공간을 나눠 쓰는 경우 — 그 각각을 이 구조로 담아 배열로 둔다. */
struct omap_iommu_device {
	/* [한국어] 그 MMU 가 쓰는 페이지 테이블의 뿌리.
	 * 설정자: 도메인에 붙일 때 잡아 채운다.
	 * 읽는 자: 매핑을 걸고 풀 때, 그리고 하드웨어에 알릴 때.
	 * 값 범위: 16KB 정렬된 커널 주소.
	 * 동기화: 도메인의 lock 아래에서 다룬다. */
	u32 *pgtable;
	/* [한국어] 그 MMU 하드웨어.
	 * 설정자: 도메인에 붙일 때.
	 * 읽는 자: 레지스터를 만지거나 무효화를 낼 때.
	 * 값 범위: NULL 이면 이 자리는 아직 비어 있다.
	 * 동기화: 도메인의 lock 아래. */
	struct omap_iommu *iommu_dev;
};

/**
 * struct omap_iommu_domain - omap iommu domain
 * @num_iommus: number of iommus in this domain
 * @iommus:	omap iommu device data for all iommus in this domain
 * @dev:	Device using this domain.
 * @lock:	domain lock, should be taken when attaching/detaching
 * @domain:	generic domain handle used by iommu core code
 */
/* [한국어] 주소 공간 하나. (위 영어 kernel-doc 참고)
 *
 * 여러 MMU 를 배열로 들고 있는 것이 이 드라이버의 특징이다. 한 장치가
 * 여러 MMU 뒤에 있을 수 있어 — DSP 처럼 코어와 주변 장치가 각각 MMU 를
 * 가진 경우 — 도메인 하나가 그것들을 모두 아울러야 한다. */
struct omap_iommu_domain {
	/* [한국어] 이 도메인에 속한 MMU 의 수.
	 * 설정자: 장치를 붙일 때 그 장치가 가진 MMU 수만큼 정한다.
	 * 읽는 자: 아래 배열을 훑는 반복문의 한계.
	 * 값 범위: 1 이상.
	 * 동기화: 붙이기 이후 불변. */
	u32 num_iommus;
	/* [한국어] 그 MMU 들의 정보 배열.
	 * 설정자: 장치를 붙일 때 잡아 채운다.
	 * 읽는 자: 매핑을 걸거나 무효화를 낼 때 모두 훑는다 — 같은 매핑을
	 *         모든 MMU 에 걸어야 하기 때문이다.
	 * 값 범위: num_iommus 개.
	 * 동기화: lock 아래. */
	struct omap_iommu_device *iommus;
	/* [한국어] 이 도메인을 쓰는 장치.
	 * 설정자: 붙일 때.
	 * 읽는 자: 로그와, 이미 다른 장치가 쓰고 있는지 검사할 때 —
	 *         이 드라이버는 도메인 하나에 장치 하나만 허용한다.
	 * 값 범위: NULL 이면 아무도 쓰지 않는다.
	 * 동기화: lock 아래. */
	struct device *dev;
	/* [한국어] 이 도메인의 상태를 지키는 락.
	 * 설정자: 도메인을 만들 때 초기화한다.
	 * 읽는 자: 붙이고 떼는 두 경로.
	 * 값 범위: 스핀락.
	 * 동기화: 이 필드 자체가 동기화 장치다. */
	spinlock_t lock;
	/* [한국어] iommu 코어가 아는 도메인 몸통.
	 * 설정자: 도메인을 만들 때 코어의 규칙에 따라 채운다.
	 * 읽는 자: container_of 로 이 구조를 되찾는 모든 곳.
	 * 값 범위: 코어가 정한 종류.
	 * 동기화: 코어의 수명 규칙. */
	struct iommu_domain domain;
};

/* [한국어] MMU 하드웨어 하나.
 *
 * 절전 문맥을 저장할 자리를 들고 있는 것이 눈에 띈다 — 이 SoC 들은
 * 전력을 아끼려고 MMU 를 통째로 꺼 버리므로, 깨어날 때 TLB 항목까지
 * 복원해야 한다. */
struct omap_iommu {
	/* [한국어] 이 MMU 의 이름 (로그와 debugfs 에 쓰인다).
	 * 설정자: 프로브가 장치 이름에서 가져온다.
	 * 읽는 자: 로그와 debugfs 디렉터리 이름.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. */
	const char	*name;
	/* [한국어] 레지스터 창이 매핑된 커널 주소.
	 * 설정자: 프로브가 ioremap 으로 얻는다.
	 * 읽는 자: 아래 접근자들의 기준점.
	 * 값 범위: NULL 이면 프로브가 실패한 것이다.
	 * 동기화: 불변. */
	void __iomem	*regbase;
	/* [한국어] DSP MMU 를 켜고 끄는 별도 시스템 레지스터로 가는 통로.
	 * 설정자: 프로브가 장치 트리의 참조로 얻는다.
	 * 읽는 자: DRA7xx 의 DSP MMU 를 켤 때만.
	 * 값 범위: 그런 MMU 가 아니면 NULL.
	 * 동기화: 불변. */
	struct regmap	*syscfg;
	/* [한국어] 이 MMU 의 커널 장치.
	 * 설정자: 프로브.
	 * 읽는 자: 로그와 자원 관리.
	 * 값 범위: NULL 일 수 없다.
	 * 동기화: 불변. */
	struct device	*dev;
	/* [한국어] 지금 이 MMU 에 걸려 있는 도메인.
	 * 설정자: 붙이고 뗄 때.
	 * 읽는 자: 폴트가 났을 때 어느 도메인의 일인지 되짚을 때.
	 * 값 범위: NULL 이면 아무 도메인에도 붙어 있지 않다.
	 * 동기화: 도메인의 lock 아래. */
	struct iommu_domain *domain;
	/* [한국어] 이 MMU 의 debugfs 디렉터리.
	 * 설정자: debugfs 등록 때.
	 * 읽는 자: 항목을 더하거나 지울 때.
	 * 값 범위: debugfs 를 끄고 빌드하면 쓰이지 않는다.
	 * 동기화: 불변. */
	struct dentry	*debug_dir;

	spinlock_t	iommu_lock;	/* global for this whole object */
	/* [한국어] (위 영어 주석 참고) 이 MMU 전체를 지키는 락.
	 * 설정자: 프로브에서 초기화.
	 * 읽는 자: TLB 를 만지는 모든 경로 — TLB 잠금 레지스터가 하나뿐이라
	 *         두 CPU 가 동시에 항목을 넣으면 서로의 자리를 덮어쓴다.
	 * 값 범위: 스핀락.
	 * 동기화: 이 필드 자체가 동기화 장치다. */

	/*
	 * We don't change iopgd for a situation like pgd for a task,
	 * but share it globally for each iommu.
	 */
	/* [한국어] (위 영어 주석 참고) CPU 의 페이지 테이블은 프로세스마다 바뀌지만,
	 * 이 MMU 의 표는 한 번 걸면 그대로 둔다. 그래서 도메인을 바꿔도 표
	 * 자체를 갈아 끼우는 일이 없다.
	 * 설정자: 도메인에 붙을 때 그 도메인의 표를 가리키게 한다.
	 * 읽는 자: 매핑을 걸고 풀 때.
	 * 값 범위: 16KB 정렬된 커널 주소.
	 * 동기화: 아래 page_table_lock 아래에서 다룬다. */
	u32		*iopgd;
	spinlock_t	page_table_lock; /* protect iopgd */
	/* [한국어] (위 영어 주석 참고) 페이지 테이블을 지키는 락.
	 * 설정자: 프로브에서 초기화.
	 * 읽는 자: 매핑을 걸고 푸는 경로 — 2단계 표를 새로 다는 일이
	 *         겹치면 안 된다.
	 * 값 범위: 스핀락.
	 * 동기화: 이 필드 자체가 동기화 장치다. */
	/* [한국어] 페이지 테이블 뿌리의 장치 쪽 주소.
	 * 설정자: 표를 잡아 dma 로 매핑할 때.
	 * 읽는 자: 하드웨어의 TTB 레지스터에 알릴 때.
	 * 값 범위: 16KB 정렬.
	 * 동기화: 도메인에 붙어 있는 동안 불변. */
	dma_addr_t	pd_dma;

	/* [한국어] 이 MMU 의 TLB 항목 수.
	 * 설정자: 프로브가 하드웨어 능력이나 장치 트리에서 얻는다.
	 * 읽는 자: TLB 를 훑거나 자리를 고를 때의 한계.
	 * 값 범위: 하드웨어마다 다르다 (보통 32).
	 * 동기화: 불변. */
	int		nr_tlb_entries;

	void *ctx; /* iommu context: registres saved area */
	/* [한국어] (위 영어 주석 참고) 절전 때 레지스터를 저장해 둘 자리.
	 * 설정자: 프로브가 잡고, 절전 진입 때 채운다.
	 * 읽는 자: 절전 복귀 때 되쓴다.
	 * 값 범위: 레지스터 창 크기만큼.
	 * 동기화: 절전 경로는 다른 CPU 가 멈춘 상태에서 돈다. */

	/* [한국어] 절전 때 저장해 둘 TLB 항목들.
	 * 설정자: 프로브가 잡고, 절전 진입 때 읽어 채운다.
	 * 읽는 자: 복귀 때 하드웨어에 되넣는다 — TLB 는 전원이 끊기면
	 *         사라지므로, 소프트웨어가 채워 넣던 항목은 되살려야 한다.
	 * 값 범위: nr_tlb_entries 개까지.
	 * 동기화: 절전 경로. */
	struct cr_regs *cr_ctx;
	/* [한국어] 그중 실제로 저장된 개수.
	 * 설정자: 절전 진입 때 유효한 항목만 세어 담는다.
	 * 읽는 자: 복귀 때 되넣을 개수.
	 * 값 범위: 0 ~ nr_tlb_entries.
	 * 동기화: 절전 경로. */
	u32 num_cr_ctx;

	/* [한국어] 버스 오류를 장치에게 되돌려 줄 수 있는 하드웨어인가.
	 * 설정자: 프로브가 장치 트리에서 읽는다.
	 * 읽는 자: 그 기능을 켜는 레지스터 쓰기.
	 * 값 범위: 0/1.
	 * 동기화: 불변.
	 *         (이 기능이 없으면 폴트 때 장치가 멈춘 채로 남을 수 있다.) */
	int		has_bus_err_back;
	/* [한국어] 이 MMU 의 번호.
	 * 설정자: 프로브.
	 * 읽는 자: 로그와 구분.
	 * 값 범위: 0부터.
	 * 동기화: 불변. */
	u32 id;

	/* [한국어] iommu 코어가 아는 장치 몸통.
	 * 설정자: 코어에 등록하며 채운다.
	 * 읽는 자: 코어가 이 드라이버를 부를 때의 기준점.
	 * 값 범위: 등록 전에는 비어 있다.
	 * 동기화: 코어의 규칙. */
	struct iommu_device iommu;
	/* [한국어] 이 MMU 를 iommu 코어에 등록했는가.
	 * 설정자: 등록에 성공하면 참으로 놓는다.
	 * 읽는 자: 해제 때 등록을 풀지 판단할 때 — 등록하지 않은 것을
	 *         풀려 하면 안 되기 때문이다.
	 * 값 범위: true/false.
	 * 동기화: 프로브·해제 경로. */
	bool has_iommu_driver;

	/* [한국어] 절전 상태.
	 * 설정자: 절전 진입 때 기록한다.
	 * 읽는 자: 복귀 때 어느 상태에서 돌아왔는지 판단할 때 —
	 *         깊은 절전이었다면 문맥을 되살려야 한다.
	 * 값 범위: 전력 도메인이 정의한 상태 값.
	 * 동기화: 절전 경로. */
	u8 pwrst;
};

/**
 * struct omap_iommu_arch_data - omap iommu private data
 * @iommu_dev: handle of the OMAP iommu device
 *
 * This is an omap iommu private data object, which binds an iommu user
 * to its iommu device. This object should be placed at the iommu user's
 * dev_archdata so generic IOMMU API can be used without having to
 * utilize omap-specific plumbing anymore.
 */
/* [한국어] 장치에 붙여 두는 이 드라이버의 사설 자료. (위 영어 kernel-doc 참고)
 *
 * 장치에서 "너를 담당하는 MMU 는 누구인가"를 되짚는 통로다. 이것이
 * 있어서 상위 코드가 OMAP 고유의 배선을 거치지 않고 일반 IOMMU API 만
 * 써도 되게 된다. */
struct omap_iommu_arch_data {
	/* [한국어] 그 장치를 담당하는 MMU.
	 * 설정자: 장치를 프로브할 때 채운다.
	 * 읽는 자: 그 장치의 매핑을 다루는 모든 경로.
	 * 값 범위: NULL 이면 아직 짝이 지어지지 않았다.
	 * 동기화: 프로브 이후 불변. */
	struct omap_iommu *iommu_dev;
};

/* [한국어] TLB 항목 하나의 하드웨어 형식 — 두 워드가 전부다.
 *
 * 이름이 CAM/RAM 인 것에 뜻이 있다. CAM(내용 주소화 메모리)은 "찾는
 * 주소가 여기 있는가"를 병렬로 비교하는 부분이고, RAM 은 그 비교가
 * 맞았을 때 꺼내 오는 자료다. TLB 라는 하드웨어의 구조가 그대로 이름에
 * 드러난 것이다. */
struct cr_regs {
	/* [한국어] 찾기 쪽 워드 — 가상 주소 태그, 페이지 크기, 유효·예약 비트.
	 * 설정자: 항목을 넣을 때 iotlb_entry 에서 짜 넣는다.
	 * 읽는 자: 하드웨어가 주소를 찾을 때, 그리고 debugfs 가 덤프할 때.
	 * 값 범위: MMU_CAM_* 비트 조합.
	 * 동기화: iommu_lock 아래에서 다룬다. */
	u32 cam;
	/* [한국어] 결과 쪽 워드 — 물리 주소와 엔디안·요소 크기 속성.
	 * 설정자·읽는 자: 위와 같다.
	 * 값 범위: MMU_RAM_* 비트 조합.
	 * 동기화: 위와 같다. */
	u32 ram;
};

/* [한국어] TLB 의 어느 자리를 다루고 있는지 알려 주는 값.
 *
 * 이 MMU 는 TLB 항목을 소프트웨어가 직접 채울 수 있는데, 그러려면
 * "다음에 어느 자리에 넣을 것인가"를 정해야 한다. 그 자리를 정하는
 * 레지스터의 내용을 펼친 구조다. */
struct iotlb_lock {
	/* [한국어] 이 자리부터 아래는 밀려나지 않게 잠근다.
	 * 설정자: 항목을 고정해 두고 싶을 때 올린다.
	 * 읽는 자: 하드웨어가 자리를 고를 때 이 아래는 건드리지 않는다.
	 * 값 범위: 0 ~ nr_tlb_entries.
	 * 동기화: iommu_lock 아래. */
	short base;
	/* [한국어] 다음에 덮어쓸 자리.
	 * 설정자: 항목을 넣기 전에 정하고, 넣은 뒤 하드웨어가 올린다.
	 * 읽는 자: 항목을 읽거나 넣을 때 어느 자리인지 정한다.
	 * 값 범위: base ~ nr_tlb_entries-1.
	 * 동기화: iommu_lock 아래. */
	short vict;
};
/*
 * MMU Register offsets
 */
#define MMU_REVISION		0x00	/* [한국어] 하드웨어 개정 번호 — 로그에 찍어 어느 세대인지 알린다. */
#define MMU_IRQSTATUS		0x18	/* [한국어] 지금 걸려 있는 인터럽트 원인 비트들. 처리한 뒤 되써서 지운다. */
#define MMU_IRQENABLE		0x1c	/* [한국어] 어느 원인을 인터럽트로 받을지 고르는 마스크. */
#define MMU_WALKING_ST		0x40	/* [한국어] 표를 걷는 중인지 알려 준다 — 설정을 바꾸기 전에 이 값이 내려가기를 기다린다. */
#define MMU_CNTL		0x44	/* [한국어] MMU 를 켜고 끄고, 표 걷기와 소프트웨어 TLB 를 고르는 제어 레지스터. */
#define MMU_FAULT_AD		0x48	/* [한국어] 폴트가 난 장치 주소 — 어느 접근이 실패했는지 알려 주는 유일한 단서다. */
#define MMU_TTB			0x4c	/* [한국어] 페이지 테이블 뿌리의 물리 주소를 알리는 자리. */
#define MMU_LOCK		0x50	/* [한국어] TLB 의 어느 자리를 다음에 쓸지, 어디까지 잠글지 정한다. */
#define MMU_LD_TLB		0x54	/* [한국어] 아래 CAM/RAM 에 담아 둔 값을 TLB 항목으로 넣으라는 명령. */
#define MMU_CAM			0x58	/* [한국어] 넣을 항목의 찾기 쪽 워드를 담아 두는 자리. */
#define MMU_RAM			0x5c	/* [한국어] 넣을 항목의 결과 쪽 워드를 담아 두는 자리. */
#define MMU_GFLUSH		0x60	/* [한국어] TLB 를 통째로 비우라는 명령. */
#define MMU_FLUSH_ENTRY		0x64	/* [한국어] 특정 주소의 항목만 비우라는 명령 — 통째로 비우는 것보다 훨씬 싸다. */
#define MMU_READ_CAM		0x68	/* [한국어] 지금 가리키는 TLB 자리의 찾기 쪽 워드를 읽는다 — debugfs 가 덤프에 쓴다. */
#define MMU_READ_RAM		0x6c	/* [한국어] 그 자리의 결과 쪽 워드를 읽는다. */
#define MMU_EMU_FAULT_AD	0x70	/* [한국어] 에뮬레이션 모드에서 폴트가 난 주소. */
#define MMU_GP_REG		0x88	/* [한국어] 범용 설정 — 버스 오류 되돌리기 같은 부가 기능을 켠다. */

#define MMU_REG_SIZE		256	/* [한국어] 레지스터 창의 크기 — 절전 때 이만큼을 통째로 저장한다. */

/*
 * MMU Register bit definitions
 */
/* IRQSTATUS & IRQENABLE */
#define MMU_IRQ_MULTIHITFAULT	BIT(4)	/* [한국어] TLB 에 같은 주소의 항목이 둘 이상 있다 — 소프트웨어가 항목을 잘못 넣었다는 뜻이다. */
#define MMU_IRQ_TABLEWALKFAULT	BIT(3)	/* [한국어] 표를 걷다 넘어졌다 — 표가 놓인 메모리가 잘못됐다. */
#define MMU_IRQ_EMUMISS		BIT(2)	/* [한국어] 에뮬레이션 접근이 매핑을 찾지 못했다. */
#define MMU_IRQ_TRANSLATIONFAULT	BIT(1)	/* [한국어] 매핑이 없는 주소를 건드렸다 — 가장 흔한 폴트다. */
#define MMU_IRQ_TLBMISS		BIT(0)	/* [한국어] TLB 에 없어 표를 걸어야 한다 — 표 걷기를 끄고 소프트웨어가 채우는 방식에서만 인터럽트가 된다. */

#define __MMU_IRQ_FAULT		\
	(MMU_IRQ_MULTIHITFAULT | MMU_IRQ_EMUMISS | MMU_IRQ_TRANSLATIONFAULT)	/* [한국어] 세 원인이 모두 "접근이 실패했다"는 뜻이라 하나로 묶었다. */
#define MMU_IRQ_MASK		\
	(__MMU_IRQ_FAULT | MMU_IRQ_TABLEWALKFAULT | MMU_IRQ_TLBMISS)	/* [한국어] 받을 수 있는 모든 원인. */
#define MMU_IRQ_TWL_MASK	(__MMU_IRQ_FAULT | MMU_IRQ_TABLEWALKFAULT)	/* [한국어] 표 걷기를 켤 때 받을 원인들 — TLB 미스는 하드웨어가 알아서 처리하므로 뺀다. */
#define MMU_IRQ_TLB_MISS_MASK	(__MMU_IRQ_FAULT | MMU_IRQ_TLBMISS)	/* [한국어] 소프트웨어가 TLB 를 채우는 방식에서 받을 원인들 — 미스가 와야 채워 넣을 수 있다. */

/* MMU_CNTL */
#define MMU_CNTL_SHIFT		1	/* [한국어] 제어 필드가 놓인 자리. */
#define MMU_CNTL_MASK		(7 << MMU_CNTL_SHIFT)	/* [한국어] 그 필드 세 비트를 고르는 마스크. */
#define MMU_CNTL_EML_TLB	BIT(3)	/* [한국어] 에뮬레이션 TLB 를 쓴다. */
#define MMU_CNTL_TWL_EN		BIT(2)	/* [한국어] 표 걷기를 켠다 — 끄면 소프트웨어가 TLB 를 직접 채워야 한다. */
#define MMU_CNTL_MMU_EN		BIT(1)	/* [한국어] MMU 자체를 켠다 — 이 비트가 서야 변환이 시작된다. */

/* CAM */
#define MMU_CAM_VATAG_SHIFT	12	/* [한국어] 가상 주소 태그가 놓인 자리 — 아래 12비트는 페이지 안 오프셋이라 태그에 들어가지 않는다. */
#define MMU_CAM_VATAG_MASK \
	((~0UL >> MMU_CAM_VATAG_SHIFT) << MMU_CAM_VATAG_SHIFT)	/* [한국어] 그 태그 부분만 고르는 마스크. */
#define MMU_CAM_P		BIT(3)	/* [한국어] 이 항목을 잠근다 — TLB 가 가득 차도 밀려나지 않는다. */
#define MMU_CAM_V		BIT(2)	/* [한국어] 이 항목이 유효하다 — 꺼져 있으면 빈 자리로 본다. */
#define MMU_CAM_PGSZ_MASK	3	/* [한국어] 페이지 크기 코드가 놓인 두 비트. */
#define MMU_CAM_PGSZ_1M		(0 << 0)	/* [한국어] 1MB 페이지. */
#define MMU_CAM_PGSZ_64K	(1 << 0)	/* [한국어] 64KB 페이지. */
#define MMU_CAM_PGSZ_4K		(2 << 0)	/* [한국어] 4KB 페이지 — 가장 흔하다. */
#define MMU_CAM_PGSZ_16M	(3 << 0)	/* [한국어] 16MB 페이지 — TLB 항목 하나로 넓은 범위를 덮는다. */

/* RAM */
#define MMU_RAM_PADDR_SHIFT	12	/* [한국어] 물리 주소가 놓인 자리 — 역시 아래 12비트는 오프셋이다. */
#define MMU_RAM_PADDR_MASK \
	((~0UL >> MMU_RAM_PADDR_SHIFT) << MMU_RAM_PADDR_SHIFT)	/* [한국어] 그 주소 부분만 고르는 마스크. */

#define MMU_RAM_ENDIAN_SHIFT	9	/* [한국어] 엔디안 비트가 놓인 자리. */
#define MMU_RAM_ENDIAN_MASK	BIT(MMU_RAM_ENDIAN_SHIFT)	/* [한국어] 그 비트를 고르는 마스크. */
#define MMU_RAM_ENDIAN_LITTLE	(0 << MMU_RAM_ENDIAN_SHIFT)	/* [한국어] 바이트 순서를 그대로 둔다. */
#define MMU_RAM_ENDIAN_BIG	BIT(MMU_RAM_ENDIAN_SHIFT)	/* [한국어] 변환하면서 바이트 순서를 뒤집는다 — 순서가 다른 코어와 메모리를 나눠 쓸 때 쓴다. */

#define MMU_RAM_ELSZ_SHIFT	7	/* [한국어] 요소 크기 필드가 놓인 자리 — 엔디안을 뒤집을 단위를 정한다. */
#define MMU_RAM_ELSZ_MASK	(3 << MMU_RAM_ELSZ_SHIFT)	/* [한국어] 그 두 비트를 고르는 마스크. */
#define MMU_RAM_ELSZ_8		(0 << MMU_RAM_ELSZ_SHIFT)	/* [한국어] 8비트 단위 — 뒤집을 것이 없다. */
#define MMU_RAM_ELSZ_16		(1 << MMU_RAM_ELSZ_SHIFT)	/* [한국어] 16비트 단위로 뒤집는다. */
#define MMU_RAM_ELSZ_32		(2 << MMU_RAM_ELSZ_SHIFT)	/* [한국어] 32비트 단위로 뒤집는다. */
#define MMU_RAM_ELSZ_NONE	(3 << MMU_RAM_ELSZ_SHIFT)	/* [한국어] 변환하지 않는다. */
#define MMU_RAM_MIXED_SHIFT	6	/* [한국어] 혼합 크기 비트가 놓인 자리. */
#define MMU_RAM_MIXED_MASK	BIT(MMU_RAM_MIXED_SHIFT)	/* [한국어] 그 비트를 고르는 마스크. */
#define MMU_RAM_MIXED		MMU_RAM_MIXED_MASK	/* [한국어] 접근 크기를 가리지 않고 그대로 통과시킨다. */

#define MMU_GP_REG_BUS_ERR_BACK_EN	0x1	/* [한국어] 폴트 때 버스 오류를 장치에게 되돌려 준다 — 이것이 없으면 장치가 멈춘 채 남을 수 있다. */

#define get_cam_va_mask(pgsz)				\
	(((pgsz) == MMU_CAM_PGSZ_16M) ? 0xff000000 :	\
	 ((pgsz) == MMU_CAM_PGSZ_1M)  ? 0xfff00000 :	\
	 ((pgsz) == MMU_CAM_PGSZ_64K) ? 0xffff0000 :	\
	 ((pgsz) == MMU_CAM_PGSZ_4K)  ? 0xfffff000 : 0)	/* [한국어] 알 수 없는 코드면 0 — 어떤 주소와도 맞지 않게 된다. */

/*
 * DSP_SYSTEM registers and bit definitions (applicable only for DRA7xx DSP)
 */
#define DSP_SYS_REVISION		0x00	/* [한국어] DSP 시스템 레지스터의 개정 번호. */
#define DSP_SYS_MMU_CONFIG		0x18	/* [한국어] DSP 의 MMU 를 켜고 끄는 시스템 레지스터 — MMU 자기 레지스터가 아니라 밖에 있다. */
#define DSP_SYS_MMU_CONFIG_EN_SHIFT	4	/* [한국어] 그 안에서 켜기 비트가 놓인 자리. */

/*
 * utilities for super page(16MB, 1MB, 64KB and 4KB)
 */

#define iopgsz_max(bytes)			\
	(((bytes) >= SZ_16M) ? SZ_16M :		\
	 ((bytes) >= SZ_1M)  ? SZ_1M  :		\
	 ((bytes) >= SZ_64K) ? SZ_64K :		\
	 ((bytes) >= SZ_4K)  ? SZ_4K  :	0)	/* [한국어] 4KB 에도 못 미치면 0 — 매핑할 수 없는 크기다. */

#define bytes_to_iopgsz(bytes)				\
	(((bytes) == SZ_16M) ? MMU_CAM_PGSZ_16M :	\
	 ((bytes) == SZ_1M)  ? MMU_CAM_PGSZ_1M  :	\
	 ((bytes) == SZ_64K) ? MMU_CAM_PGSZ_64K :	\
	 ((bytes) == SZ_4K)  ? MMU_CAM_PGSZ_4K  : -1)	/* [한국어] 네 크기 중 어느 것도 아니면 -1 — 아래 iopgsz_ok 가 그것으로 판정한다. */

#define iopgsz_to_bytes(iopgsz)				\
	(((iopgsz) == MMU_CAM_PGSZ_16M)	? SZ_16M :	\
	 ((iopgsz) == MMU_CAM_PGSZ_1M)	? SZ_1M  :	\
	 ((iopgsz) == MMU_CAM_PGSZ_64K)	? SZ_64K :	\
	 ((iopgsz) == MMU_CAM_PGSZ_4K)	? SZ_4K  : 0)	/* [한국어] 알 수 없는 코드면 0. */

#define iopgsz_ok(bytes) (bytes_to_iopgsz(bytes) >= 0)	/* [한국어] 그 크기를 하드웨어가 다룰 수 있는가 — 변환이 -1 을 내지 않으면 된다. */

/*
 * global functions
 */

/*
 * [한국어]
 * __iotlb_read_cr - TLB 의 n 번째 항목을 읽어 온다
 *
 * @obj: 대상 MMU.
 * @n: 읽을 자리.
 * @return: 그 항목의 두 워드.
 *
 * 읽으려면 먼저 잠금 레지스터로 그 자리를 가리켜야 해서, 순수한 읽기가
 * 아니라 하드웨어 상태를 건드리는 동작이다. 그래서 호출자가 iommu_lock 을
 * 쥐고 있어야 한다.
 *
 * debugfs 가 TLB 를 덤프할 때 쓰려고 밖으로 열어 두었다.
 *
 * 실행 컨텍스트: iommu_lock 아래. 잠들지 않는다.
 *
 * 호출 체인:
 *   for_each_iotlb_cr 매크로 / debugfs → [이 함수]
 */
struct cr_regs __iotlb_read_cr(struct omap_iommu *obj, int n);
/*
 * [한국어]
 * iotlb_lock_get - 지금 TLB 잠금 설정을 읽어 온다
 *
 * @obj: 대상 MMU.
 * @l: 읽은 값을 담을 자리.
 *
 * 어디까지 잠겨 있고 다음에 어느 자리를 쓸지를 알아낸다. 항목을 읽거나
 * 넣기 전에 지금 설정을 보관해 두었다가, 끝난 뒤 되돌리는 데 쓴다.
 *
 * 실행 컨텍스트: iommu_lock 아래. 잠들지 않는다.
 *
 * 호출 체인:
 *   TLB 를 다루는 경로 / debugfs → [이 함수]
 */
void iotlb_lock_get(struct omap_iommu *obj, struct iotlb_lock *l);
/*
 * [한국어]
 * iotlb_lock_set - TLB 잠금 설정을 바꾼다
 *
 * @obj: 대상 MMU.
 * @l: 새로 쓸 값.
 *
 * 다음에 다룰 TLB 자리를 지정한다. 항목을 읽거나 넣기 전에 이 함수로
 * 자리를 가리키고, 끝난 뒤 원래 값으로 되돌리는 것이 이 드라이버의
 * 관용구다.
 *
 * 실행 컨텍스트: iommu_lock 아래. 잠들지 않는다.
 *
 * 호출 체인:
 *   TLB 를 다루는 경로 / debugfs → [이 함수]
 */
void iotlb_lock_set(struct omap_iommu *obj, struct iotlb_lock *l);

/* [한국어] debugfs 지원을 켜고 빌드했을 때만 실제 함수가 있다.
 *
 * 끄고 빌드하면 아래 #else 의 빈 껍데기가 대신 들어가, 호출부에
 * #ifdef 를 뿌리지 않아도 된다. */
#ifdef CONFIG_OMAP_IOMMU_DEBUG
void omap_iommu_debugfs_init(void);	/* [한국어] 이 드라이버의 debugfs 뿌리 디렉터리를 만든다. */
void omap_iommu_debugfs_exit(void);	/* [한국어] 그 디렉터리를 지운다. */

void omap_iommu_debugfs_add(struct omap_iommu *obj);	/* [한국어] MMU 하나의 항목들(레지스터·TLB·페이지 테이블 덤프)을 만든다. */
void omap_iommu_debugfs_remove(struct omap_iommu *obj);	/* [한국어] 그 항목들을 지운다. */
#else
/* [한국어] (아래 넷은 debugfs 를 끈 빌드의 빈 껍데기다. 호출부가 조건문
 * 없이 그대로 컴파일되도록 두었고, 컴파일러가 통째로 걷어 낸다.) */
static inline void omap_iommu_debugfs_init(void) { }	/* [한국어] 만들 디렉터리가 없다. */
static inline void omap_iommu_debugfs_exit(void) { }	/* [한국어] 지울 것도 없다. */

static inline void omap_iommu_debugfs_add(struct omap_iommu *obj) { }	/* [한국어] 더할 항목이 없다. */
static inline void omap_iommu_debugfs_remove(struct omap_iommu *obj) { }	/* [한국어] 지울 항목이 없다. */
#endif

/*
 * register accessors
 */
/*
 * [한국어]
 * iommu_read_reg - MMU 레지스터 하나를 읽는다
 *
 * @obj: 대상 MMU.
 * @offs: 레지스터 오프셋.
 * @return: 읽은 값.
 *
 * 원시(__raw) 판을 쓰는 것이 눈에 띈다 — 엔디안 변환도 순서 보장도 하지
 * 않는다. 이 드라이버가 순서가 필요한 자리마다 장벽을 따로 넣기 때문이며,
 * 옛 ARM 드라이버에서 흔한 방식이다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   omap-iommu.c 의 모든 레지스터 접근 → [이 함수]
 */
static inline u32 iommu_read_reg(struct omap_iommu *obj, size_t offs)
{
	return __raw_readl(obj->regbase + offs);	/* [한국어] 매핑된 창의 기준점에서 오프셋만큼 옮긴 자리를 읽는다. */
}

/*
 * [한국어]
 * iommu_write_reg - MMU 레지스터 하나에 쓴다
 *
 * @obj: 대상 MMU.
 * @val: 쓸 값.
 * @offs: 레지스터 오프셋.
 *
 * 위 읽기와 짝을 이루는 원시 쓰기다. 순서가 필요한 자리에서는 호출자가
 * 장벽을 직접 넣어야 한다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   omap-iommu.c 의 모든 레지스터 쓰기 → [이 함수]
 */
static inline void iommu_write_reg(struct omap_iommu *obj, u32 val, size_t offs)
{
	__raw_writel(val, obj->regbase + offs);	/* [한국어] 그 자리에 값을 쓴다. */
}

/*
 * [한국어]
 * iotlb_cr_valid - 그 TLB 항목이 유효한가
 *
 * @cr: 검사할 항목.
 * @return: 유효하면 0 이 아닌 값, 항목이 NULL 이면 -EINVAL.
 *
 * 찾기 쪽 워드의 유효 비트 하나를 보는 것이 전부다. TLB 를 훑을 때
 * 빈 자리를 건너뛰는 데 쓴다.
 *
 * 반환형이 int 인 것이 눈에 띄는데, "유효 여부"와 "인자가 잘못됨"을
 * 한 값으로 겸하기 때문이다.
 *
 * 실행 컨텍스트: 어디서나. 잠들지 않는다.
 *
 * 호출 체인:
 *   TLB 를 훑는 경로 / debugfs → [이 함수]
 */
static inline int iotlb_cr_valid(struct cr_regs *cr)
{
	if (!cr)	/* [한국어] 항목이 없다면. */
		return -EINVAL;	/* [한국어] 유효 여부를 답할 수 없다. */

	return cr->cam & MMU_CAM_V;	/* [한국어] 찾기 쪽 워드의 유효 비트 하나가 답이다. */
}

#endif /* _OMAP_IOMMU_H */
