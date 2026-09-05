// SPDX-License-Identifier: GPL-2.0-only
/*
 *
 * Copyright (C) 2013 Freescale Semiconductor, Inc.
 * Author: Varun Sethi <varun.sethi@freescale.com>
 */

/*
 * [한국어 설명] PAMU를 리눅스 IOMMU API에 붙이는 도메인 계층 (fsl_pamu_domain.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 Freescale PAMU 하드웨어(fsl_pamu.c가 다룬다)를 리눅스의
 * `struct iommu_ops` 추상화에 연결하는 어댑터다. 즉 "iommu_domain에 디바이스를
 * attach 한다"는 일반적인 요청을, PAMU의 세계에서는 "이 디바이스의 LIODN들을
 * 찾아 PAACE를 구성하고 유효화한다"로 번역하는 것이 전부다.
 * 여기서 중요한 사실은, 이 드라이버가 실제로는 주소 변환을 전혀 하지 않는다는
 * 점이다. `.map`/`.unmap` 콜백이 아예 없고, iova_to_phys()는 항등 함수다.
 * 파일 안의 두 FIXME 주석이 그 사정을 솔직하게 밝히고 있다 — UNMANAGED 도메인을
 * 표방하지만 페이징 도메인의 요구조건을 만족하지 않으며, 이 드라이버가
 * 존재하는 실질적인 이유는 drivers/soc/fsl/qbman/qman_portal.c가
 * fsl_pamu_configure_l1_stash()를 호출할 통로를 제공하기 위해서다.
 * 다시 말해 이 파일의 진짜 목적은 DMA 격리가 아니라 **스태시(stash) 목적지
 * 설정**이다 — QMan 포털의 DMA 데이터를 특정 CPU의 L1 캐시로 밀어 넣어
 * 네트워크 처리 지연을 줄이는 DPAA 최적화가 그것이다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [IOMMU 코어] iommu_attach_device() / iommu_domain_alloc()
 *        ↓ iommu_ops 콜백
 *   [이 파일] fsl_pamu_attach_device(), fsl_pamu_domain_alloc(), ...
 *        ↓ 디바이스 트리에서 "fsl,liodn" 읽기 → LIODN 번호 목록
 *   [fsl_pamu.c] pamu_config_ppaace(), pamu_enable_liodn(), pamu_update_paace_stash()
 *        ↓ PAACT 메모리 기록 + PAMU 캐시 무효화
 *   [PAMU 하드웨어] 이후 그 디바이스의 DMA를 허용/차단, 스태시 목적지 적용
 *
 * 또 하나의 진입 경로가 있다:
 *   [qman_portal.c] fsl_pamu_configure_l1_stash(domain, cpu)
 *        → update_domain_stash() → update_liodn_stash() → pamu_update_paace_stash()
 *
 * 실행 컨텍스트: 전부 프로세스 컨텍스트다. 다만 스핀락을 irqsave로 잡으므로
 * 락 구간 안에서는 잠들 수 없고, attach_device()가 GFP_ATOMIC으로 할당하는
 * 이유도 그 때문이다.
 *
 * === 타 모듈과의 연결 ===
 * - fsl_pamu_domain.h: struct fsl_dma_domain(도메인당 상태)과
 *   struct device_domain_info(도메인-디바이스 연결 노드) 정의, 그리고
 *   fsl_pamu_configure_l1_stash() 선언.
 * - fsl_pamu.h / fsl_pamu.c: PAACE 조작 함수들(pamu_config_ppaace 등)과
 *   PAACE_NUMBER_ENTRIES 같은 하드웨어 한계값.
 * - linux/platform_device.h: 플랫폼 디바이스의 of_node에서 "fsl,liodn"
 *   프로퍼티를 읽기 위해 필요하다.
 * - sysdev/fsl_pci.h: PCI_FSL_BRR1 등 Freescale PCI 컨트롤러의 블록 리비전
 *   레지스터 정의. PCIe 엔드포인트를 개별 그룹으로 나눌 수 있는 실리콘인지
 *   판별하는 데 쓴다.
 * - drivers/soc/fsl/qbman/qman_portal.c: 이 파일이 노출하는 유일한 외부
 *   함수 fsl_pamu_configure_l1_stash()의 호출자.
 * 데이터 흐름: u-boot이 디바이스 트리에 "fsl,liodn" 프로퍼티를 심어 둔다 →
 * attach 시 이 파일이 그 배열을 읽어 LIODN 목록을 얻는다 → 각 LIODN마다
 * device_domain_info 노드를 만들어 도메인의 devices 리스트에 매단다 →
 * PAACE를 구성하고 활성화한다. detach는 그 역순이다.
 *
 * === 주요 함수/구조체 요약 ===
 * - fsl_pamu_attach_device(): 디바이스 트리의 LIODN 배열을 순회하며
 *   attach_device() + pamu_set_liodn() + pamu_enable_liodn()을 반복한다.
 * - fsl_pamu_platform_attach(): "기본(platform) 도메인으로 돌아가기" 콜백.
 *   실제로는 UNMANAGED 도메인에서 나올 때만 detach를 수행하는 우회책이며,
 *   FIXME가 지적하듯 결과는 IDENTITY가 아니라 BLOCKING에 가깝다.
 * - fsl_pamu_configure_l1_stash(): 이 드라이버의 존재 이유. 도메인에 속한
 *   모든 LIODN의 스태시 목적지를 주어진 CPU의 L1 캐시로 바꾼다.
 * - fsl_pamu_device_group(): 플랫폼 디바이스는 개별 그룹, PCIe는 실리콘
 *   리비전에 따라 개별 그룹 또는 컨트롤러 단위 공유 그룹으로 묶는다.
 * - attach_device()/detach_device()/remove_device_ref(): 도메인-디바이스
 *   연결 리스트를 관리하는 내부 헬퍼 3인방.
 * - iommu_lock / device_domain_lock / dma_domain->domain_lock: 세 겹의 락.
 *   각각 PAMU 하드웨어 설정, dev_iommu_priv 포인터, 도메인의 디바이스 리스트를
 *   보호한다.
 */

/* [한국어] 이 파일의 모든 pr_debug/pr_err 출력 앞에 "fsl-pamu-domain: <함수명>: "
 * 접두사를 자동으로 붙인다. LIODN 설정 실패 같은 메시지가 어느 단계에서 났는지
 * 로그만 보고 알 수 있게 하려는 것이다. */
#define pr_fmt(fmt)    "fsl-pamu-domain: %s: " fmt, __func__

/* [한국어] struct fsl_dma_domain, struct device_domain_info 정의와
 * fsl_pamu_configure_l1_stash() 선언. 이 헤더가 다시 fsl_pamu.h를 포함하므로
 * PAACE 관련 상수와 함수도 함께 들어온다. */
#include "fsl_pamu_domain.h"

/* [한국어] 플랫폼 디바이스와 of_node 접근용. "fsl,liodn" 프로퍼티를
 * of_get_property()로 읽는 데 필요하다. */
#include <linux/platform_device.h>
/* [한국어] Freescale PCI 컨트롤러 정의 — PCI_FSL_BRR1(블록 리비전 레지스터)과
 * pci_controller 구조체. PCIe 엔드포인트 파티셔닝 가능 여부를 판별하는 데 쓴다. */
#include <sysdev/fsl_pci.h>

/*
 * Global spinlock that needs to be held while
 * configuring PAMU.
 */
/* [한국어] PAMU 하드웨어 설정을 직렬화하는 전역 스핀락.
 * 보호 대상: PAACT 엔트리 기록과 그에 뒤따르는 PAMU 캐시 무효화 시퀀스.
 * 왜 전역인가: PAACT는 시스템 전체가 공유하는 단일 테이블이고, 엔트리 기록과
 *              캐시 무효화가 원자적으로 이뤄져야 하드웨어가 반쯤 갱신된
 *              엔트리를 보지 않는다. 도메인별 락으로는 서로 다른 도메인이
 *              동시에 PAACT를 건드리는 것을 막을 수 없다.
 * 획득 방식: 인터럽트 컨텍스트에서도 PAMU 접근이 있을 수 있어 irqsave를 쓴다.
 * 락 순서: domain_lock → iommu_lock (attach 경로가 이 순서로 잡는다). */
static DEFINE_SPINLOCK(iommu_lock);

/* [한국어] struct fsl_dma_domain 전용 슬랩 캐시.
 * 설정자: iommu_init_mempool()이 부팅 시 생성.
 * 읽는 자: fsl_pamu_domain_alloc()/free()가 도메인을 할당/반납할 때.
 * 왜 전용 캐시인가: SLAB_HWCACHE_ALIGN으로 캐시라인 정렬을 보장해,
 *                   자주 접근하는 도메인 구조체가 false sharing을 겪지 않게 한다. */
static struct kmem_cache *fsl_pamu_domain_cache;
/* [한국어] struct device_domain_info 전용 슬랩 캐시.
 * 설정자: iommu_init_mempool(). 읽는 자: attach_device()/remove_device_ref().
 * 이 구조체는 attach마다(정확히는 LIODN마다) 하나씩 만들어지므로 할당 빈도가
 * 높아 전용 캐시가 의미가 있다. */
static struct kmem_cache *iommu_devinfo_cache;
/* [한국어] 디바이스의 dev_iommu_priv 포인터와 devinfo 슬랩 객체의 수명을
 * 보호하는 전역 스핀락.
 * 보호 대상: dev_iommu_priv_get()/set()으로 읽고 쓰는 "이 디바이스가 지금
 *            어느 도메인에 붙어 있는가" 포인터.
 * 왜 별도의 락인가: 이 포인터는 디바이스 단위 상태라 도메인 락으로는 보호할 수
 *                   없다. 서로 다른 도메인에 동시에 attach를 시도하는 경쟁을
 *                   막으려면 디바이스 쪽에 걸린 락이 필요하다.
 * 락 순서 주의: attach_device()는 이 락을 놓았다가 detach_device()를 호출하고
 *               다시 잡는다 — detach가 domain_lock을 잡기 때문에 락 순서
 *               역전(deadlock)을 피하려는 조치다. */
static DEFINE_SPINLOCK(device_domain_lock);

/* [한국어] IOMMU 코어에 이 드라이버를 대표해 등록되는 핸들.
 * 설정자: pamu_domain_init()이 iommu_device_sysfs_add()/register()로 초기화.
 * 읽는 자: fsl_pamu_probe_device()가 "이 디바이스는 내가 담당한다"는 뜻으로
 *          이 포인터를 반환한다. 코어는 이것으로 iommu_ops를 찾아간다.
 * 값 범위: 시스템에 PAMU 인스턴스가 여러 개여도 IOMMU 코어에는 하나로 보인다
 *          — LIODN 공간이 전역이라 굳이 나눌 이유가 없기 때문이다.
 * 동기화: 부팅 시 한 번 등록되고 해제되지 않는다.
 * static이 아닌 이유: fsl_pamu.c가 오류 처리 경로에서 참조한다. */
struct iommu_device pamu_iommu;	/* IOMMU core code handle */

/*
 * [한국어]
 * to_fsl_dma_domain - 일반 iommu_domain 포인터를 PAMU 전용 도메인으로 되돌린다
 *
 * @dom: IOMMU 코어가 넘겨준 일반 도메인 포인터.
 * @return: 그 도메인을 감싸고 있는 struct fsl_dma_domain 포인터.
 *
 * 왜 필요한가: IOMMU 코어는 드라이버 내부 구조를 모르므로 항상
 * struct iommu_domain 포인터만 주고받는다. 드라이버는 그것을 자기 구조체의
 * 멤버로 임베드해 두고, container_of로 바깥 구조체를 복원한다 — 리눅스에서
 * 상속을 흉내 내는 표준 관용구다.
 *
 * 실행 컨텍스트: 모든 iommu_ops 콜백의 첫 줄에서 호출된다. 순수 포인터 산술이라
 * 락도 필요 없고 실패할 수도 없다.
 *
 * 호출 체인:
 *   fsl_pamu_domain_free()/attach_device()/configure_l1_stash() → [to_fsl_dma_domain]
 */
static struct fsl_dma_domain *to_fsl_dma_domain(struct iommu_domain *dom)
{
	/* [한국어] iommu_domain 멤버의 주소에서 그 멤버가 구조체 안에서 갖는
	 * 오프셋만큼 빼면 바깥 구조체의 시작 주소가 나온다. 컴파일 타임에
	 * 계산되는 상수 뺄셈이라 런타임 비용이 사실상 없다. */
	return container_of(dom, struct fsl_dma_domain, iommu_domain);
}

/*
 * [한국어]
 * iommu_init_mempool - 이 드라이버가 쓰는 두 개의 슬랩 캐시를 만든다
 *
 * @return: 0 성공, -ENOMEM(캐시 생성 실패).
 *
 * 왜 필요한가: 도메인 구조체와 디바이스-도메인 연결 노드는 attach/detach마다
 * 반복 할당되므로, 범용 kmalloc 대신 크기가 고정된 전용 슬랩 캐시를 쓰는 편이
 * 단편화와 캐시 지역성 양쪽에서 유리하다. SLAB_HWCACHE_ALIGN으로 캐시라인
 * 정렬까지 보장한다.
 *
 * 동작 과정:
 *  1) fsl_pamu_domain_cache 생성 — 실패하면 즉시 -ENOMEM.
 *  2) iommu_devinfo_cache 생성 — 실패하면 1번에서 만든 캐시를 되돌리고 -ENOMEM.
 *
 * 실행 컨텍스트: 부팅 초기(__init), pamu_domain_init()의 첫 단계.
 * 에러 경로: 두 번째 생성이 실패하면 첫 번째를 반드시 파괴해야 누수가 없다.
 *
 * 호출 체인:
 *   fsl_pamu.c의 probe → pamu_domain_init() → [iommu_init_mempool]
 *   → kmem_cache_create()
 */
static int __init iommu_init_mempool(void)
{
	/* [한국어] 도메인 구조체용 슬랩 캐시를 만든다. 이름 "fsl_pamu_domain"은
	 * /proc/slabinfo에 그대로 나타나 메모리 사용량 추적에 쓰인다.
	 * 세 번째 인자 0은 오프셋(정렬 시작 위치)이 기본값이라는 뜻이고,
	 * SLAB_HWCACHE_ALIGN은 객체를 캐시라인 경계에 맞춰 배치하라는 요청,
	 * 마지막 NULL은 객체 생성자 콜백이 없다는 뜻이다. */
	fsl_pamu_domain_cache = kmem_cache_create("fsl_pamu_domain",
						  sizeof(struct fsl_dma_domain),
						  0,
						  SLAB_HWCACHE_ALIGN,
						  NULL);
	/* [한국어] 캐시 생성 실패는 사실상 메모리 고갈뿐이다. */
	if (!fsl_pamu_domain_cache) {
		/* [한국어] 부팅 단계라 pr_err가 아닌 pr_debug인 것이 다소 아쉽지만,
		 * 원본 그대로 둔다 — 아래 반환값으로 probe가 실패하므로 상위에서
		 * 별도 메시지가 나온다. */
		pr_debug("Couldn't create fsl iommu_domain cache\n");
		return -ENOMEM;	/* [한국어] 도메인 캐시를 만들지 못했으니 메모리 부족을 알린다. */
	}

	/* [한국어] 디바이스-도메인 연결 노드용 슬랩 캐시. attach 한 번에 LIODN
	 * 개수만큼 할당되므로 도메인 캐시보다 객체 수가 훨씬 많다. */
	iommu_devinfo_cache = kmem_cache_create("iommu_devinfo",
						sizeof(struct device_domain_info),
						0,
						SLAB_HWCACHE_ALIGN,
						NULL);
	/* [한국어] 두 번째 캐시 생성 실패 — 첫 번째를 되돌려야 한다. */
	if (!iommu_devinfo_cache) {
		pr_debug("Couldn't create devinfo cache\n");
		/* [한국어] 앞서 만든 도메인 캐시를 파괴해 슬랩 누수를 막는다.
		 * 이 정리를 빼먹으면 probe 실패 후에도 빈 캐시가 영원히 남는다. */
		kmem_cache_destroy(fsl_pamu_domain_cache);
		return -ENOMEM;	/* [한국어] devinfo 캐시 실패 — 앞서 만든 캐시를 되돌린 뒤 메모리 부족을 알린다. */
	}

	/* [한국어] 두 캐시 모두 준비 완료. */
	return 0;
}

/*
 * [한국어]
 * update_liodn_stash - LIODN 하나의 스태시 목적지를 하드웨어에 반영한다
 *
 * @liodn: 대상 LIODN 번호(PAACT 인덱스).
 * @dma_domain: 소속 도메인. 현재 구현에서는 실제로 쓰이지 않는다(아래 참고).
 * @val: 새 스태시 캐시 ID — PAACE의 impl_attr CID 필드에 들어갈 값.
 * @return: 0 성공, pamu_update_paace_stash()가 낸 음수 errno.
 *
 * 왜 필요한가: 스태시 목적지 갱신은 PAACT를 건드리는 작업이므로 전역
 * iommu_lock으로 직렬화해야 한다. 이 함수는 그 락 처리와 실패 로깅만
 * 담당하는 얇은 래퍼다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자가 이미 domain_lock을 irqsave로
 * 잡은 상태에서 다시 iommu_lock을 잡으므로, 락 순서는 항상
 * domain_lock → iommu_lock이다. 이 순서를 뒤집으면 데드락이 난다.
 *
 * 참고: @dma_domain 인자는 받기만 하고 쓰지 않는다 — 과거 서브윈도 처리
 * 코드가 제거되면서 남은 흔적이며, 시그니처는 호출부 호환을 위해 유지된다.
 *
 * 호출 체인:
 *   fsl_pamu_configure_l1_stash() → update_domain_stash() → [update_liodn_stash]
 *   → pamu_update_paace_stash() (fsl_pamu.c)
 */
static int update_liodn_stash(int liodn, struct fsl_dma_domain *dma_domain,
			      u32 val)
{
	/* [한국어] 하드웨어 갱신 결과를 담는다. 0으로 초기화하는 것은 방어적 습관이다. */
	int ret = 0;
	/* [한국어] irqsave가 저장할 인터럽트 플래그. */
	unsigned long flags;

	/* [한국어] PAACT 접근을 직렬화한다. irqsave인 이유는 PAMU 관련 처리가
	 * 인터럽트 컨텍스트에서도 일어날 수 있어, 같은 CPU에서의 재진입까지
	 * 막아야 하기 때문이다. */
	spin_lock_irqsave(&iommu_lock, flags);
	/* [한국어] PAACE의 impl_attr CID 필드만 새 값으로 바꾸고 PAMU 캐시를
	 * 무효화한다. 전체 PAACE를 다시 쓰지 않는 이유는 나머지 설정(윈도, 권한)이
	 * 그대로 유지되어야 하기 때문이다. */
	ret = pamu_update_paace_stash(liodn, val);
	/* [한국어] 실패 시(LIODN 범위 초과 등) 락을 반드시 풀고 나간다. */
	if (ret) {
		/* [한국어] 메시지가 "SPAACE"라고 되어 있지만 실제로 갱신하는 것은
		 * Primary PAACE다 — 서브윈도 코드가 있던 시절의 흔적이다. */
		pr_debug("Failed to update SPAACE for liodn %d\n", liodn);
		spin_unlock_irqrestore(&iommu_lock, flags);	/* [한국어] 실패 경로에서도 락을 반드시 푼다. */
		return ret;	/* [한국어] 하드웨어 갱신 오류를 그대로 호출자에게 전달한다. */
	}

	/* [한국어] 성공 경로의 락 해제. */
	spin_unlock_irqrestore(&iommu_lock, flags);

	/* [한국어] 여기까지 왔으면 ret은 0이다. */
	return ret;
}

/* Set the geometry parameters for a LIODN */
/*
 * [한국어]
 * pamu_set_liodn - LIODN 하나의 PAACE를 이 도메인에 맞게 구성한다
 *
 * @dma_domain: 이 LIODN이 속할 도메인. stash_id를 여기서 가져온다.
 * @dev: 대상 디바이스. OME 인덱스를 고르는 데 쓴다(PCI인지 여부에 따라 달라짐).
 * @liodn: 구성할 LIODN 번호.
 * @return: 0 성공, pamu_disable_liodn()/pamu_config_ppaace()가 낸 음수 errno.
 *
 * 왜 필요한가: PAACE를 안전하게 갈아 끼우려면 순서가 중요하다. 먼저 무효화해
 * 하드웨어가 그 엔트리를 보지 않게 만든 뒤에 내용을 바꿔야, 반쯤 갱신된
 * 상태로 DMA가 통과하는 사고를 막을 수 있다.
 *
 * 동작 과정:
 *  1) get_ome_index()로 이 디바이스에 맞는 OME 인덱스를 구한다. 디바이스
 *     종류(PCI/플랫폼)에 따라 정해지는 정적인 값이라 여기서 한 번만 계산한다.
 *  2) 전역 iommu_lock을 잡는다.
 *  3) pamu_disable_liodn()으로 기존 엔트리를 무효화한다.
 *  4) pamu_config_ppaace()를 prot=0(접근 전면 거부)으로 한 번 호출해
 *     윈도/변환 모드/스태시 등 기본 골격을 세운다.
 *  5) 다시 pamu_config_ppaace()를 QUERY|UPDATE(읽기+쓰기 허용)로 호출해
 *     권한을 연다. omi를 ~0으로 넘기는 것은 "OME 설정은 4단계에서 이미
 *     끝났으니 건드리지 말라"는 뜻이다.
 *
 * 왜 두 번 호출하는가: 4단계에서 OME 인덱스를 포함한 전체 구성을 권한 없이
 * 먼저 세우고, 5단계에서 권한만 여는 2단계 구성이다. 이렇게 하면 구성이
 * 완전히 끝나기 전에 접근이 허용되는 구간이 생기지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자(fsl_pamu_attach_device)가 이미
 * domain_lock을 잡은 상태이며, 여기서 iommu_lock을 추가로 잡는다.
 * 에러 경로: 어느 단계에서 실패하든 goto out_unlock으로 모여 락을 풀고
 * 실패 메시지를 남긴다. 실패 시 PAACE는 무효화된 상태로 남으므로 안전하다.
 *
 * 호출 체인:
 *   fsl_pamu_attach_device() → [pamu_set_liodn]
 *   → get_ome_index(), pamu_disable_liodn(), pamu_config_ppaace()
 */
static int pamu_set_liodn(struct fsl_dma_domain *dma_domain, struct device *dev,
			  int liodn)
{
	/* [한국어] OME 인덱스를 담을 변수. ~0으로 초기화하는 것은 "아직 정해지지
	 * 않음/OMT 미사용"을 뜻하는 이 드라이버의 관례값이다. */
	u32 omi_index = ~(u32)0;
	/* [한국어] irqsave용 플래그 저장소. */
	unsigned long flags;
	/* [한국어] 각 단계의 반환값. */
	int ret;

	/*
	 * Configure the omi_index at the geometry setup time.
	 * This is a static value which depends on the type of
	 * device and would not change thereafter.
	 */
	/* [한국어] 디바이스 종류에 따라 사용할 OME(오퍼레이션 매핑 엔트리)를 고른다.
	 * PCI 디바이스와 플랫폼 디바이스는 스태시 동작이 달라 서로 다른 OME를 쓴다.
	 * 디바이스 종류는 변하지 않으므로 attach 시점에 한 번만 계산하면 된다. */
	get_ome_index(&omi_index, dev);

	/* [한국어] PAACT 조작을 직렬화한다. 아래 세 번의 하드웨어 호출이
	 * 하나의 원자적 재구성으로 보여야 한다. */
	spin_lock_irqsave(&iommu_lock, flags);
	/* [한국어] 1단계: 기존 엔트리를 무효화(V 비트 내림)해 하드웨어가 이 LIODN의
	 * DMA를 차단하게 만든다. 재구성 중에 접근이 통과하는 것을 막는 안전장치다. */
	ret = pamu_disable_liodn(liodn);
	/* [한국어] 무효화조차 실패하면 더 진행하면 안 된다(LIODN 범위 오류 등). */
	if (ret)
		goto out_unlock;
	/* [한국어] 2단계: 권한을 0(전면 거부)으로 둔 채 나머지 구성을 모두 세운다.
	 * omi_index로 스태시용 OME를, dma_domain->stash_id로 목적지 캐시를 지정한다.
	 * stash_id가 아직 ~0이면(스태시 미설정 상태) 하드웨어는 스태시를 하지 않는다. */
	ret = pamu_config_ppaace(liodn, omi_index, dma_domain->stash_id, 0);
	if (ret)	/* [한국어] 무효화가 실패하면 재구성을 이어갈 수 없다. */
		goto out_unlock;
	/* [한국어] 3단계: 이제 읽기(QUERY)와 쓰기(UPDATE) 권한을 연다.
	 * omi에 ~0을 넘기는 것은 "OME 설정은 이미 끝났으니 다시 쓰지 말라"는 신호다.
	 * 이 호출이 성공해야 비로소 디바이스의 DMA가 통과할 수 있는 상태가 된다. */
	ret = pamu_config_ppaace(liodn, ~(u32)0, dma_domain->stash_id,
				 PAACE_AP_PERMS_QUERY | PAACE_AP_PERMS_UPDATE);
/* [한국어] 성공/실패가 모두 모이는 공통 종료 지점. 락 해제를 한 곳에 모아
 * 어느 경로에서도 락이 새지 않도록 보장한다. */
out_unlock:
	spin_unlock_irqrestore(&iommu_lock, flags);
	/* [한국어] 어느 단계든 실패했다면 어떤 LIODN이 문제였는지 남긴다.
	 * 호출자는 이 값을 보고 attach 루프를 중단한다. */
	if (ret) {
		pr_debug("PAACE configuration failed for liodn %d\n",	/* [한국어] 어떤 LIODN에서 실패했는지 남겨 디버깅을 돕는다. */
			 liodn);
	}
	/* [한국어] 마지막 단계의 결과를 그대로 호출자에게 전달한다. */
	return ret;
}

/*
 * [한국어]
 * remove_device_ref - 디바이스-도메인 연결 노드 하나를 완전히 정리한다
 *
 * @info: 제거할 연결 노드. 도메인의 devices 리스트에 매달려 있다.
 * @return: 없음.
 *
 * 왜 필요한가: 연결을 끊을 때 해야 할 일이 세 가지다 — 리스트에서 빼고,
 * 하드웨어에서 LIODN을 비활성화하고, 디바이스의 priv 포인터를 지우며 노드를
 * 반납하는 것. 세 가지가 서로 다른 락 아래에서 이뤄져야 해서 별도 함수로
 * 분리되어 있다.
 *
 * 동작 과정:
 *  1) list_del() — 호출자가 이미 domain_lock을 잡고 있어 별도 락이 없다.
 *  2) iommu_lock 아래에서 pamu_disable_liodn() — 하드웨어 차단.
 *  3) device_domain_lock 아래에서 dev_iommu_priv_set(NULL) + 슬랩 반납.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트, 호출자가 domain_lock을 irqsave로 잡은 상태.
 * 락 순서: domain_lock(호출자) → iommu_lock → device_domain_lock. 두 하위 락을
 * 겹쳐 잡지 않고 순차적으로 잡았다 푸는 점에 주목 — 락 중첩을 최소화한 설계다.
 *
 * 호출 체인:
 *   detach_device() → [remove_device_ref] → pamu_disable_liodn(), kmem_cache_free()
 */
static void remove_device_ref(struct device_domain_info *info)
{
	/* [한국어] irqsave용 플래그. 아래에서 두 번 재사용한다. */
	unsigned long flags;

	/* [한국어] 도메인의 devices 리스트에서 이 노드를 뺀다. 호출자가
	 * domain_lock을 잡고 list_for_each_entry_safe로 순회 중이므로
	 * 순회 도중 삭제해도 안전하다. */
	list_del(&info->link);
	/* [한국어] 하드웨어에서 이 LIODN을 비활성화한다. 이 시점 이후로 해당
	 * 디바이스의 DMA는 access violation으로 거부된다. */
	spin_lock_irqsave(&iommu_lock, flags);
	pamu_disable_liodn(info->liodn);	/* [한국어] 이 LIODN의 V 비트를 내려 하드웨어가 DMA를 차단하게 만든다. */
	spin_unlock_irqrestore(&iommu_lock, flags);
	/* [한국어] 디바이스 쪽 상태 정리는 device_domain_lock 아래에서 한다.
	 * priv 포인터를 NULL로 만들어야 다음 attach가 "붙어 있는 도메인 없음"으로
	 * 올바르게 판단한다. */
	spin_lock_irqsave(&device_domain_lock, flags);
	dev_iommu_priv_set(info->dev, NULL);
	/* [한국어] 연결 노드를 슬랩 캐시에 반납한다. priv를 먼저 지운 뒤에
	 * 해제해야, 다른 CPU가 해제된 노드를 priv로 집어 드는 일이 없다. */
	kmem_cache_free(iommu_devinfo_cache, info);
	spin_unlock_irqrestore(&device_domain_lock, flags);	/* [한국어] 디바이스 쪽 정리가 끝났으니 락을 푼다. */
}

/*
 * [한국어]
 * detach_device - 도메인에서 디바이스(또는 전체 디바이스)를 떼어낸다
 *
 * @dev: 떼어낼 디바이스. NULL이면 도메인의 모든 디바이스를 떼어낸다.
 * @dma_domain: 대상 도메인.
 * @return: 없음.
 *
 * 왜 필요한가: 두 가지 상황을 하나의 함수로 처리한다 — (1) 도메인이 해제될 때
 * 남은 모든 연결을 정리(dev == NULL), (2) 특정 디바이스만 떼어내기.
 * 한 디바이스가 여러 LIODN을 가질 수 있어, 같은 dev를 가진 노드가 여러 개
 * 매달려 있을 수 있다. 그래서 리스트 전체를 순회하며 조건에 맞는 것을 모두
 * 제거한다(첫 개를 찾고 멈추지 않는다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. domain_lock을 irqsave로 잡고 순회한다.
 * _safe 순회를 쓰는 이유는 순회 도중 remove_device_ref()가 노드를 리스트에서
 * 빼고 해제하기 때문이다 — tmp가 다음 노드를 미리 붙들어 준다.
 *
 * 호출 체인:
 *   fsl_pamu_domain_free() / fsl_pamu_platform_attach() / attach_device()
 *   → [detach_device] → remove_device_ref()
 */
static void detach_device(struct device *dev, struct fsl_dma_domain *dma_domain)
{
	/* [한국어] 순회 커서와, 현재 노드가 해제되어도 다음으로 넘어갈 수 있게
	 * 미리 붙들어 두는 보조 포인터. */
	struct device_domain_info *info, *tmp;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] 도메인의 devices 리스트를 보호한다. 이 락 안에서
	 * remove_device_ref()가 iommu_lock과 device_domain_lock을 차례로 잡으므로,
	 * 락 순서는 항상 domain_lock이 가장 바깥이다. */
	spin_lock_irqsave(&dma_domain->domain_lock, flags);
	/* Remove the device from the domain device list */
	/* [한국어] 순회 중 삭제가 일어나므로 반드시 _safe 변형을 써야 한다. */
	list_for_each_entry_safe(info, tmp, &dma_domain->devices, link) {
		/* [한국어] dev가 NULL이면 조건 없이 전부 제거(도메인 해제 경로),
		 * 아니면 해당 디바이스의 노드만 제거한다. 한 디바이스에 LIODN이
		 * 여러 개면 여러 노드가 매칭되어 모두 제거된다. */
		if (!dev || (info->dev == dev))
			remove_device_ref(info);
	}
	spin_unlock_irqrestore(&dma_domain->domain_lock, flags);	/* [한국어] 리스트 순회가 끝났으니 도메인 락을 푼다. */
}

/*
 * [한국어]
 * attach_device - LIODN 하나에 대한 디바이스-도메인 연결 노드를 만든다
 *
 * @dma_domain: 붙일 도메인.
 * @liodn: 이 연결이 담당할 LIODN 번호.
 * @dev: 대상 디바이스(PCI라면 이미 컨트롤러의 부모 디바이스로 치환된 상태).
 * @return: 없음.
 *
 * 왜 필요한가: 도메인은 자신에게 붙은 LIODN 목록을 알아야 한다 —
 * fsl_pamu_configure_l1_stash()가 "이 도메인의 모든 LIODN"을 순회해야 하기
 * 때문이다. 그 목록이 dma_domain->devices 리스트이고, 이 함수가 노드를
 * 하나 추가한다.
 *
 * 동작 과정:
 *  1) device_domain_lock 아래에서 디바이스가 이미 다른 도메인에 붙어 있는지 확인.
 *  2) 붙어 있고 그것이 다른 도메인이면, 락을 놓고 detach_device()를 부른 뒤
 *     다시 잡는다 — detach가 domain_lock을 잡으므로 락 순서 역전을 피하려는 조치다.
 *  3) 연결 노드를 GFP_ATOMIC으로 할당(스핀락 안이라 잠들 수 없다).
 *  4) 노드를 채우고 도메인의 devices 리스트에 매단다.
 *  5) 디바이스의 priv가 아직 비어 있을 때만 이 노드를 priv로 등록한다 —
 *     LIODN이 여러 개인 디바이스는 첫 번째 노드만 대표로 삼는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자(fsl_pamu_attach_device)가 이미
 * domain_lock을 잡고 있고, 여기서 device_domain_lock을 추가로 잡는다.
 *
 * 주의: kmem_cache_zalloc()의 반환값을 검사하지 않는다 — GFP_ATOMIC 할당이
 * 실패하면 곧바로 NULL 역참조로 이어지는 기존 결함이다. 코드를 고치지 않는
 * 것이 이 주석 작업의 원칙이므로 사실만 기록해 둔다.
 *
 * 호출 체인:
 *   fsl_pamu_attach_device() → [attach_device] → detach_device()(필요 시),
 *   kmem_cache_zalloc(), dev_iommu_priv_set()
 */
static void attach_device(struct fsl_dma_domain *dma_domain, int liodn, struct device *dev)
{
	/* [한국어] 새로 만들 연결 노드와, 기존에 붙어 있던 연결 노드. */
	struct device_domain_info *info, *old_domain_info;
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;

	/* [한국어] dev_iommu_priv 포인터를 읽고 쓰는 구간을 보호한다. */
	spin_lock_irqsave(&device_domain_lock, flags);
	/*
	 * Check here if the device is already attached to domain or not.
	 * If the device is already attached to a domain detach it.
	 */
	/* [한국어] 이 디바이스가 지금 붙어 있는 도메인을 조회한다.
	 * NULL이면 아직 어디에도 붙어 있지 않다는 뜻이다. */
	old_domain_info = dev_iommu_priv_get(dev);
	/* [한국어] 다른 도메인에 붙어 있다면 먼저 그 연결을 끊어야 한다.
	 * 같은 도메인이면 아무것도 하지 않는다(LIODN이 여러 개인 디바이스가
	 * 두 번째 LIODN으로 다시 들어오는 정상 경우). */
	if (old_domain_info && old_domain_info->domain != dma_domain) {
		/* [한국어] detach_device()가 domain_lock을 잡으므로, 여기서
		 * device_domain_lock을 쥔 채 부르면 락 순서가 역전되어 데드락
		 * 위험이 생긴다. 그래서 일부러 놓았다가 다시 잡는다. */
		spin_unlock_irqrestore(&device_domain_lock, flags);
		detach_device(dev, old_domain_info->domain);	/* [한국어] 이전 도메인과의 연결을 끊는다 — 이 호출이 domain_lock을 잡는다. */
		spin_lock_irqsave(&device_domain_lock, flags);	/* [한국어] detach가 끝났으니 다시 device_domain_lock을 잡고 이어간다. */
	}

	/* [한국어] 연결 노드를 0으로 초기화해 할당한다. 스핀락을 쥔 상태라
	 * 잠들 수 없으므로 GFP_ATOMIC을 쓴다(할당 실패 가능성이 그만큼 높아진다). */
	info = kmem_cache_zalloc(iommu_devinfo_cache, GFP_ATOMIC);

	/* [한국어] 어느 디바이스의 연결인지 기록 — detach 시 매칭 기준이 된다. */
	info->dev = dev;
	/* [한국어] 이 연결이 담당하는 LIODN. 스태시 갱신과 비활성화의 대상이다. */
	info->liodn = liodn;
	/* [한국어] 소속 도메인 역참조 — attach_device()가 "다른 도메인인지"
	 * 판별할 때 이 포인터를 본다. */
	info->domain = dma_domain;

	/* [한국어] 도메인의 devices 리스트 앞쪽에 매단다. 순서는 의미가 없고
	 * (전체 순회만 하므로) list_add가 가장 저렴하다. */
	list_add(&info->link, &dma_domain->devices);
	/*
	 * In case of devices with multiple LIODNs just store
	 * the info for the first LIODN as all
	 * LIODNs share the same domain
	 */
	/* [한국어] 디바이스의 대표 연결 노드를 등록한다. 이미 등록되어 있다면
	 * (LIODN이 여러 개인 디바이스의 두 번째 이후 호출) 덮어쓰지 않는다.
	 * 모든 LIODN이 같은 도메인을 가리키므로 어느 것을 대표로 삼아도 무방하고,
	 * 첫 번째를 유지하는 편이 detach 시 일관성이 좋다. */
	if (!dev_iommu_priv_get(dev))
		dev_iommu_priv_set(dev, info);
	spin_unlock_irqrestore(&device_domain_lock, flags);	/* [한국어] 연결 등록이 끝났으니 락을 푼다. */
}

/*
 * [한국어]
 * fsl_pamu_iova_to_phys - IOVA를 물리 주소로 변환한다(사실상 항등 함수)
 *
 * @domain: 대상 도메인. aperture 범위 확인에만 쓴다.
 * @iova: 변환할 I/O 가상 주소.
 * @return: 범위 안이면 iova 그대로, 범위 밖이면 0(변환 실패).
 *
 * 왜 이런 모습인가: PAMU는 페이지 테이블 기반 변환을 하지 않는다. 이 드라이버는
 * PAACE의 변환 모드를 PAACE_ATM_NO_XLATE로 두어 디바이스가 낸 주소를 그대로
 * 물리 주소로 쓴다. 따라서 "변환"이란 곧 "허용 범위 안인지 확인"이 전부다.
 * 범위는 도메인 생성 시 0 ~ 64GB-1로 고정된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 락이 없고 상태를 바꾸지 않는다.
 *
 * 호출 체인:
 *   iommu_iova_to_phys() → domain_ops->iova_to_phys → [fsl_pamu_iova_to_phys]
 */
static phys_addr_t fsl_pamu_iova_to_phys(struct iommu_domain *domain,
					 dma_addr_t iova)
{
	/* [한국어] 도메인의 aperture(허용 주소 범위) 밖이면 유효한 매핑이 아니다.
	 * IOMMU API 규약상 실패는 0으로 표현한다. */
	if (iova < domain->geometry.aperture_start ||
	    iova > domain->geometry.aperture_end)
		return 0;
	/* [한국어] 범위 안이면 변환이 항등이므로 입력을 그대로 돌려준다. */
	return iova;
}

/*
 * [한국어]
 * fsl_pamu_capable - 이 IOMMU가 특정 기능을 지원하는지 알린다
 *
 * @dev: 질의 대상 디바이스. 이 구현에서는 보지 않는다(모든 디바이스가 동일).
 * @cap: 질의하는 기능(enum iommu_cap).
 * @return: 캐시 코히런시 질의면 true, 그 외에는 false.
 *
 * 왜 필요한가: DMA API와 VFIO 등이 "이 IOMMU를 거친 DMA가 CPU 캐시와 자동으로
 * 일관성을 유지하는가"를 물을 때 답해야 한다. PAMU는 PAACE의
 * coherency_required 비트로 접근을 코히런시 도메인에 참여시키므로 true다.
 * 그 밖의 기능(예: IOMMU_CAP_NOEXEC, IOMMU_CAP_INTR_REMAP)은 지원하지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 순수 함수로 락이 필요 없다.
 *
 * 호출 체인:
 *   device_iommu_capable() → iommu_ops->capable → [fsl_pamu_capable]
 */
static bool fsl_pamu_capable(struct device *dev, enum iommu_cap cap)
{
	/* [한국어] 지원하는 기능이 캐시 코히런시 하나뿐이라 단순 비교로 끝난다. */
	return cap == IOMMU_CAP_CACHE_COHERENCY;
}

/*
 * [한국어]
 * fsl_pamu_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인(IOMMU 코어가 넘긴 일반 포인터).
 * @return: 없음.
 *
 * 왜 필요한가: 도메인을 그냥 반납하면 아직 붙어 있는 디바이스의 LIODN이
 * 하드웨어에서 활성 상태로 남고, 연결 노드도 누수된다. 그래서 반납 전에
 * 모든 연결을 끊는다.
 *
 * 동작 과정:
 *  1) detach_device(NULL, ...) — dev를 NULL로 넘겨 리스트의 모든 연결을 정리.
 *     각 연결마다 LIODN이 하드웨어에서 비활성화되고 노드가 반납된다.
 *  2) 도메인 구조체 자체를 슬랩 캐시에 반납.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. detach_device()가 내부에서 필요한 락을
 * 모두 잡으므로 여기서는 락을 잡지 않는다.
 *
 * 호출 체인:
 *   iommu_domain_free() → domain_ops->free → [fsl_pamu_domain_free]
 *   → detach_device(), kmem_cache_free()
 */
static void fsl_pamu_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 일반 도메인 포인터를 PAMU 전용 구조체로 복원한다. */
	struct fsl_dma_domain *dma_domain = to_fsl_dma_domain(domain);

	/* remove all the devices from the device list */
	/* [한국어] 첫 인자 NULL은 "조건 없이 전부"를 뜻한다. 남아 있던 모든 LIODN이
	 * 여기서 하드웨어적으로 차단되고 연결 노드가 해제된다. */
	detach_device(NULL, dma_domain);
	/* [한국어] 이제 참조가 없으므로 도메인 구조체를 슬랩에 반납한다. */
	kmem_cache_free(fsl_pamu_domain_cache, dma_domain);
}

/*
 * [한국어]
 * fsl_pamu_domain_alloc - 새 IOMMU 도메인을 만든다
 *
 * @type: 요청된 도메인 종류(IOMMU_DOMAIN_UNMANAGED 등).
 * @return: 새 도메인의 iommu_domain 포인터, 실패하거나 지원하지 않는 종류면 NULL.
 *
 * 왜 필요한가: IOMMU 코어가 도메인을 만들 때 부르는 콜백이다. 다만 원본
 * FIXME 주석이 밝히듯, 이 드라이버가 만드는 UNMANAGED 도메인은 진짜 페이징
 * 도메인이 아니다 — map/unmap이 없어 __IOMMU_DOMAIN_PAGING의 요건을
 * 충족하지 못한다. 이 도메인의 존재 이유는 qman_portal.c가
 * fsl_pamu_configure_l1_stash()를 호출할 대상을 갖게 하는 것뿐이다.
 *
 * 동작 과정:
 *  1) UNMANAGED가 아닌 요청은 거부(NULL).
 *  2) 슬랩에서 0 초기화 할당.
 *  3) stash_id를 ~0("스태시 미설정")으로, 디바이스 리스트와 락을 초기화.
 *  4) aperture를 0 ~ 64GB-1로 고정하고 force_aperture를 켠다.
 *
 * 왜 64GB인가: PowerPC QorIQ의 물리 주소 공간이 36비트라, 그 전체를 덮는
 * 값이 곧 "제한 없음"을 뜻한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. GFP_KERNEL 할당이므로 잠들 수 있다.
 *
 * 호출 체인:
 *   iommu_domain_alloc() → iommu_ops->domain_alloc → [fsl_pamu_domain_alloc]
 */
static struct iommu_domain *fsl_pamu_domain_alloc(unsigned type)
{
	/* [한국어] 새로 만들 PAMU 전용 도메인 구조체. */
	struct fsl_dma_domain *dma_domain;

	/*
	 * FIXME: This isn't creating an unmanaged domain since the
	 * default_domain_ops do not have any map/unmap function it doesn't meet
	 * the requirements for __IOMMU_DOMAIN_PAGING. The only purpose seems to
	 * allow drivers/soc/fsl/qbman/qman_portal.c to do
	 * fsl_pamu_configure_l1_stash()
	 */
	/* [한국어] UNMANAGED 외의 종류(DMA, IDENTITY 등)는 지원하지 않는다.
	 * 위 FIXME가 지적하듯 이 UNMANAGED조차 이름값을 못 하지만, 기존
	 * 사용자(qman_portal)를 깨뜨리지 않으려고 그대로 유지되고 있다. */
	if (type != IOMMU_DOMAIN_UNMANAGED)
		return NULL;

	/* [한국어] 도메인 구조체를 0으로 채워 할당한다. 여기서는 스핀락을 잡고
	 * 있지 않으므로 GFP_KERNEL(잠들 수 있는 할당)을 써도 된다. */
	dma_domain = kmem_cache_zalloc(fsl_pamu_domain_cache, GFP_KERNEL);
	/* [한국어] 메모리 부족 — IOMMU 코어에 NULL로 실패를 알린다. */
	if (!dma_domain)
		return NULL;

	/* [한국어] 스태시 목적지를 "미설정"으로 초기화한다. 이 값이면
	 * pamu_config_ppaace()가 CID를 유효한 캐시로 지정하지 않아 스태시가
	 * 일어나지 않는다. fsl_pamu_configure_l1_stash()가 나중에 실제 값을 채운다. */
	dma_domain->stash_id = ~(u32)0;
	/* [한국어] 이 도메인에 붙을 디바이스 연결 노드들의 리스트 헤드를 초기화한다. */
	INIT_LIST_HEAD(&dma_domain->devices);
	/* [한국어] 그 리스트를 보호할 도메인별 스핀락을 초기화한다.
	 * 락 계층에서 가장 바깥쪽에 위치한다(domain_lock → iommu_lock/device_domain_lock). */
	spin_lock_init(&dma_domain->domain_lock);

	/* default geometry 64 GB i.e. maximum system address */
	/* [한국어] aperture 시작을 0으로 둔다. PAMU는 주소 변환을 하지 않으므로
	 * 이 값은 "어느 범위의 물리 주소까지 허용하는가"를 뜻한다.
	 * 대입문 안의 여분 공백은 원본 그대로다. */
	dma_domain->iommu_domain. geometry.aperture_start = 0;
	/* [한국어] aperture 끝을 2^36 - 1로 둔다 — QorIQ의 36비트 물리 주소
	 * 공간 전체(64GB)를 덮는 값이라 사실상 제한이 없다는 뜻이다.
	 * 1ULL로 쓴 이유는 32비트 시프트 오버플로를 피하기 위함이다. */
	dma_domain->iommu_domain.geometry.aperture_end = (1ULL << 36) - 1;
	/* [한국어] force_aperture를 켜면 IOMMU 코어가 이 범위를 강제한다 —
	 * 범위 밖 매핑 요청을 코어 단계에서 거부하게 만든다. */
	dma_domain->iommu_domain.geometry.force_aperture = true;

	/* [한국어] IOMMU 코어에는 임베드된 일반 도메인 포인터를 돌려준다.
	 * 코어는 이 포인터만 알고, 나중에 to_fsl_dma_domain()으로 복원된다. */
	return &dma_domain->iommu_domain;
}

/* Update stash destination for all LIODNs associated with the domain */
/*
 * [한국어]
 * update_domain_stash - 도메인에 속한 모든 LIODN의 스태시 목적지를 갱신한다
 *
 * @dma_domain: 대상 도메인.
 * @val: 새 스태시 캐시 ID.
 * @return: 0 전부 성공, 첫 실패의 음수 errno.
 *
 * 왜 필요한가: 스태시 목적지는 도메인 단위 속성이지만, 하드웨어에서는 LIODN
 * 단위(PAACE의 CID 필드)로 저장된다. 그래서 도메인의 모든 연결을 돌며 같은
 * 값을 개별적으로 써 넣어야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 호출자(fsl_pamu_configure_l1_stash)가
 * domain_lock을 잡은 상태라 리스트 순회가 안전하다.
 * 에러 처리: 하나라도 실패하면 즉시 루프를 중단하고 그 오류를 반환한다.
 * 이미 갱신된 LIODN을 되돌리지는 않아, 부분 적용 상태가 남을 수 있다.
 *
 * 호출 체인:
 *   fsl_pamu_configure_l1_stash() → [update_domain_stash] → update_liodn_stash()
 */
static int update_domain_stash(struct fsl_dma_domain *dma_domain, u32 val)
{
	/* [한국어] 리스트 순회 커서. 여기서는 삭제가 없어 _safe가 필요 없다. */
	struct device_domain_info *info;
	/* [한국어] 결과값. 리스트가 비어 있으면 0(성공)이 그대로 반환된다. */
	int ret = 0;

	/* [한국어] 이 도메인에 붙은 모든 (디바이스, LIODN) 연결을 순회한다.
	 * 한 디바이스가 LIODN을 여러 개 가지면 노드도 여러 개라 모두 갱신된다. */
	list_for_each_entry(info, &dma_domain->devices, link) {
		/* [한국어] LIODN 하나의 PAACE CID 필드를 갱신한다(내부에서
		 * iommu_lock을 잡고 캐시 무효화까지 수행). */
		ret = update_liodn_stash(info->liodn, dma_domain, val);
		/* [한국어] 첫 실패에서 중단한다. 이미 바꾼 것들은 그대로 남지만,
		 * 스태시는 성능 최적화라 부분 적용이 정합성을 깨지는 않는다. */
		if (ret)
			break;
	}

	/* [한국어] 마지막 결과(전부 성공이면 0)를 호출자에게 전달한다. */
	return ret;
}

/*
 * [한국어]
 * fsl_pamu_attach_device - 디바이스를 이 도메인에 붙인다 (핵심 진입점)
 *
 * @domain: 붙일 대상 도메인.
 * @dev: 붙일 디바이스.
 * @old: 직전에 붙어 있던 도메인. 이 구현에서는 사용하지 않는다.
 * @return: 0 성공, -ENODEV(fsl,liodn 프로퍼티 없음/LIODN 범위 초과),
 *          PAACE 구성 실패 시 그 errno.
 *
 * 왜 필요한가: IOMMU API의 "attach"를 PAMU 세계로 번역하는 핵심 함수다.
 * PAMU에는 페이지 테이블이 없으므로, attach란 곧 "이 디바이스의 LIODN들을
 * 찾아 PAACE를 구성하고 활성화하는 것"이다.
 *
 * 동작 과정:
 *  1) PCI 디바이스면 dev를 PCI 컨트롤러의 부모 플랫폼 디바이스로 바꿔치기한다.
 *     LIODN은 개별 PCI 기능이 아니라 컨트롤러 단위로 u-boot이 부여하기 때문이다.
 *  2) 디바이스 트리에서 "fsl,liodn" 프로퍼티(u32 배열)를 읽는다. 없으면 -ENODEV.
 *  3) domain_lock을 잡고 배열의 각 LIODN에 대해:
 *     a. 범위 검증(PAACE_NUMBER_ENTRIES 미만인지) — PAACT 밖을 가리키면 거부.
 *     b. attach_device()로 연결 노드를 만들어 리스트에 추가.
 *     c. pamu_set_liodn()으로 PAACE 구성.
 *     d. pamu_enable_liodn()으로 V 비트를 세워 활성화.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. 루프 전체를 domain_lock(irqsave)으로
 * 감싸므로, 그 안에서 호출되는 attach_device()가 GFP_ATOMIC을 써야 한다.
 * 에러 경로: 어느 LIODN에서 실패하든 break로 루프를 벗어나 그 오류를 반환한다.
 * 이미 활성화된 앞쪽 LIODN들을 되돌리지는 않는다 — 상위 계층이 attach 실패 후
 * 도메인을 해제하면 detach_device()가 일괄 정리한다.
 *
 * 호출 체인:
 *   iommu_attach_device() → domain_ops->attach_dev → [fsl_pamu_attach_device]
 *   → of_get_property(), attach_device(), pamu_set_liodn(), pamu_enable_liodn()
 */
static int fsl_pamu_attach_device(struct iommu_domain *domain,
				  struct device *dev, struct iommu_domain *old)
{
	/* [한국어] 일반 도메인 포인터를 PAMU 전용 구조체로 복원한다. */
	struct fsl_dma_domain *dma_domain = to_fsl_dma_domain(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] len은 프로퍼티의 바이트 길이, ret은 결과, i는 LIODN 인덱스. */
	int len, ret = 0, i;
	/* [한국어] 디바이스 트리에서 읽은 LIODN 배열(빅엔디언 u32 배열).
	 * of_get_property는 트리 안의 원본 데이터를 그대로 가리키므로 해제하지 않는다. */
	const u32 *liodn;
	/* [한국어] PCI 디바이스일 때만 쓰이는 임시 포인터. */
	struct pci_dev *pdev = NULL;
	/* [한국어] PCI 컨트롤러(호스트 브리지) 구조체 — 부모 플랫폼 디바이스를
	 * 얻는 통로다. */
	struct pci_controller *pci_ctl;

	/*
	 * Use LIODN of the PCI controller while attaching a
	 * PCI device.
	 */
	/* [한국어] PCI 디바이스는 자기 자신의 of_node에 fsl,liodn을 갖지 않는다.
	 * LIODN은 PCIe 컨트롤러 단위로 부여되므로, 프로퍼티를 읽을 대상을
	 * 컨트롤러의 부모 디바이스로 바꿔야 한다. */
	if (dev_is_pci(dev)) {
		/* [한국어] 일반 device를 PCI device로 캐스팅한다. */
		pdev = to_pci_dev(dev);
		/* [한국어] 이 PCI 버스를 소유한 호스트 컨트롤러를 찾는다. */
		pci_ctl = pci_bus_to_host(pdev->bus);
		/*
		 * make dev point to pci controller device
		 * so we can get the LIODN programmed by
		 * u-boot.
		 */
		/* [한국어] dev를 컨트롤러의 부모(플랫폼 디바이스)로 바꾼다.
		 * 이후 of_get_property()와 attach_device()가 모두 이 디바이스를
		 * 대상으로 동작한다 — 즉 같은 컨트롤러 아래 모든 PCI 기능이
		 * 하나의 LIODN을 공유하게 된다. */
		dev = pci_ctl->parent;
	}

	/* [한국어] u-boot이 심어 둔 LIODN 배열을 읽는다. len에는 바이트 길이가
	 * 들어오므로 아래에서 sizeof(u32)로 나눠 개수를 구한다. */
	liodn = of_get_property(dev->of_node, "fsl,liodn", &len);
	/* [한국어] 프로퍼티가 없으면 이 디바이스는 PAMU가 관리할 수 없다.
	 * %pOF는 device_node를 사람이 읽을 수 있는 경로로 출력하는 포맷이다. */
	if (!liodn) {
		pr_debug("missing fsl,liodn property at %pOF\n", dev->of_node);	/* [한국어] u-boot이 LIODN을 심어 주지 않은 디바이스라 관리할 수 없다. */
		return -ENODEV;	/* [한국어] PAMU 대상이 아님을 코어에 알린다. */
	}

	/* [한국어] 도메인의 디바이스 리스트를 보호한다. 루프 전체를 한 번에
	 * 감싸므로 여러 LIODN 등록이 원자적으로 보인다. */
	spin_lock_irqsave(&dma_domain->domain_lock, flags);
	/* [한국어] 프로퍼티에 담긴 LIODN 개수만큼 반복한다.
	 * 한 디바이스가 여러 LIODN을 갖는 경우(예: 읽기/쓰기 채널이 분리된
	 * 가속기)가 실제로 존재한다. */
	for (i = 0; i < len / sizeof(u32); i++) {
		/* Ensure that LIODN value is valid */
		/* [한국어] LIODN이 PAACT 크기를 넘으면 테이블 밖 메모리를 PAACE로
		 * 해석하게 되므로 반드시 걸러야 한다. u-boot 설정 오류에 대한 방어다. */
		if (liodn[i] >= PAACE_NUMBER_ENTRIES) {
			pr_debug("Invalid liodn %d, attach device failed for %pOF\n",	/* [한국어] PAACT 범위를 벗어난 LIODN이라는 것을 로그로 남긴다. */
				 liodn[i], dev->of_node);
			ret = -ENODEV;	/* [한국어] 잘못된 디바이스 트리 설정이므로 attach를 실패로 끝낸다. */
			break;
		}

		/* [한국어] 소프트웨어 쪽 연결을 먼저 만든다 — 이후 단계가 실패해도
		 * 도메인 해제 시 detach_device()가 이 노드를 찾아 정리할 수 있다. */
		attach_device(dma_domain, liodn[i], dev);
		/* [한국어] PAACE를 이 도메인의 설정(스태시 ID, OME 인덱스)으로
		 * 구성한다. 아직 V 비트는 서지 않은 상태다. */
		ret = pamu_set_liodn(dma_domain, dev, liodn[i]);
		/* [한국어] 구성 실패 시 이후 LIODN도 의미가 없으니 중단한다. */
		if (ret)
			break;
		/* [한국어] 마지막으로 V 비트를 세워 하드웨어가 이 LIODN을 인정하게
		 * 만든다. 이 호출이 성공한 순간부터 디바이스의 DMA가 통과한다. */
		ret = pamu_enable_liodn(liodn[i]);
		/* [한국어] 활성화 실패도 즉시 중단 사유다. */
		if (ret)
			break;
	}
	spin_unlock_irqrestore(&dma_domain->domain_lock, flags);
	/* [한국어] 성공이면 0, 아니면 중단을 유발한 오류를 그대로 올려 보낸다. */
	return ret;
}

/*
 * FIXME: fsl/pamu is completely broken in terms of how it works with the iommu
 * API. Immediately after probe the HW is left in an IDENTITY translation and
 * the driver provides a non-working UNMANAGED domain that it can switch over
 * to. However it cannot switch back to an IDENTITY translation, instead it
 * switches to what looks like BLOCKING.
 */
/*
 * [한국어]
 * fsl_pamu_platform_attach - 기본(platform) 도메인으로 되돌리는 콜백
 *
 * @platform_domain: 이 드라이버의 정적 기본 도메인(fsl_pamu_platform_domain).
 * @dev: 대상 디바이스.
 * @old: 직전에 붙어 있던 도메인. 이 함수의 판단은 전적으로 이 값에 달려 있다.
 * @return: 항상 0. 실패를 알릴 수단이 없다(그리고 실패할 일도 없다).
 *
 * 왜 필요한가: IOMMU 코어는 디바이스가 항상 어떤 도메인에는 속해 있기를
 * 요구하고, 그 기본값이 default_domain이다. 이 드라이버는
 * IOMMU_DOMAIN_PLATFORM 타입의 정적 도메인 하나를 기본값으로 등록해 두고,
 * "그 기본 도메인으로 돌아간다"는 요청을 여기서 처리한다.
 *
 * 위 FIXME가 지적하는 문제: probe 직후 하드웨어는 IDENTITY(=u-boot이 열어 둔
 * 상태)지만, 한 번 UNMANAGED 도메인으로 갔다가 돌아오면 이 함수가 detach를
 * 수행해 LIODN을 비활성화한다. 그 결과는 IDENTITY가 아니라 모든 DMA가 막히는
 * BLOCKING에 가깝다. 되돌릴 방법이 없다는 것이 이 드라이버의 구조적 한계다.
 *
 * 동작 과정:
 *  1) old가 없거나, 자기 자신이거나, UNMANAGED가 아니면 아무 일도 하지 않는다
 *     — 즉 "UNMANAGED에서 나오는 경우"에만 실제 작업이 있다.
 *  2) PCI면 dev를 컨트롤러의 부모로 바꾼다(attach와 대칭).
 *  3) fsl,liodn 프로퍼티가 있으면 detach_device()로 연결을 모두 끊는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. detach_device()가 필요한 락을 스스로 잡는다.
 *
 * 호출 체인:
 *   iommu_detach_device()/도메인 전환 → domain_ops->attach_dev
 *   → [fsl_pamu_platform_attach] → detach_device()
 */
static int fsl_pamu_platform_attach(struct iommu_domain *platform_domain,
				    struct device *dev,
				    struct iommu_domain *old)
{
	/* [한국어] old를 복원해 담을 포인터. UNMANAGED일 때만 유효하게 쓰인다. */
	struct fsl_dma_domain *dma_domain;
	/* [한국어] fsl,liodn 프로퍼티 존재 확인용 포인터(값은 쓰지 않는다). */
	const u32 *prop;
	/* [한국어] 프로퍼티 길이를 받는 변수. 역시 값 자체는 쓰지 않는다. */
	int len;
	/* [한국어] PCI 경로에서만 쓰이는 임시 포인터. */
	struct pci_dev *pdev = NULL;
	/* [한국어] PCI 호스트 컨트롤러 포인터. */
	struct pci_controller *pci_ctl;

	/*
	 * Hack to keep things working as they always have, only leaving an
	 * UNMANAGED domain makes it BLOCKING.
	 */
	/* [한국어] 세 가지 경우에 그냥 성공만 반환하고 끝낸다.
	 * (1) old == platform_domain: 이미 기본 도메인이라 할 일이 없다.
	 * (2) !old: 최초 attach라 끊을 연결이 없다.
	 * (3) old->type != UNMANAGED: 이 드라이버가 만든 도메인이 아니므로
	 *     to_fsl_dma_domain()으로 복원하면 안 된다(엉뚱한 메모리 해석).
	 * 즉 실제 detach는 "UNMANAGED 도메인에서 빠져나올 때"만 일어난다. */
	if (old == platform_domain || !old ||
	    old->type != IOMMU_DOMAIN_UNMANAGED)
		return 0;

	/* [한국어] 여기까지 왔다면 old는 확실히 이 드라이버가 만든 도메인이다. */
	dma_domain = to_fsl_dma_domain(old);

	/*
	 * Use LIODN of the PCI controller while detaching a
	 * PCI device.
	 */
	/* [한국어] attach와 대칭으로, PCI 디바이스는 컨트롤러의 부모 디바이스를
	 * 기준으로 연결이 등록되어 있으므로 같은 치환을 해야 매칭이 된다. */
	if (dev_is_pci(dev)) {
		/* [한국어] PCI device로 캐스팅. */
		pdev = to_pci_dev(dev);
		/* [한국어] 소속 호스트 컨트롤러를 찾는다. */
		pci_ctl = pci_bus_to_host(pdev->bus);
		/*
		 * make dev point to pci controller device
		 * so we can get the LIODN programmed by
		 * u-boot.
		 */
		/* [한국어] detach 대상 디바이스를 컨트롤러의 부모로 치환한다.
		 * attach 때 이 디바이스로 등록했으므로 동일하게 맞춰야 한다. */
		dev = pci_ctl->parent;
	}

	/* [한국어] 이 디바이스가 애초에 PAMU 관리 대상이었는지 프로퍼티로 확인한다.
	 * 값은 필요 없고 존재 여부만 본다. */
	prop = of_get_property(dev->of_node, "fsl,liodn", &len);
	/* [한국어] 프로퍼티가 있으면 이 디바이스의 모든 연결을 끊는다 —
	 * 각 LIODN이 하드웨어에서 비활성화되고(=BLOCKING) 노드가 반납된다. */
	if (prop)
		detach_device(dev, dma_domain);
	else
		/* [한국어] 프로퍼티가 없다면 attach도 되지 않았을 것이므로
		 * 경고만 남기고 조용히 넘어간다. */
		pr_debug("missing fsl,liodn property at %pOF\n", dev->of_node);
	/* [한국어] 이 콜백은 실패를 표현하지 않는다 — 항상 성공으로 보고한다. */
	return 0;
}

/* [한국어] 기본(platform) 도메인의 연산 테이블. attach_dev 하나뿐인 이유는
 * 이 도메인이 실제로는 "아무 도메인에도 속하지 않은 상태"를 표현하는
 * 자리표시자이기 때문이다 — 해제할 것도, 변환할 것도 없다. */
static struct iommu_domain_ops fsl_pamu_platform_ops = {
	/* [한국어] 기본 도메인으로 돌아올 때 UNMANAGED 연결을 끊는 콜백. */
	.attach_dev = fsl_pamu_platform_attach,
};

/* [한국어] 이 드라이버의 정적 기본 도메인. 동적으로 할당하지 않고 하나를
 * 공유하는 이유는 상태가 전혀 없기 때문이다 — 모든 디바이스가 같은 "빈"
 * 도메인을 가리켜도 문제가 없다.
 * IOMMU_DOMAIN_PLATFORM 타입은 "이 플랫폼의 기본 동작을 따른다"는 뜻으로,
 * 코어가 map/unmap을 요구하지 않는다. */
static struct iommu_domain fsl_pamu_platform_domain = {
	/* [한국어] 도메인 종류 — 위에서 UNMANAGED와 구분하는 판별 기준이 된다. */
	.type = IOMMU_DOMAIN_PLATFORM,
	/* [한국어] 위에서 정의한 attach_dev 하나짜리 연산 테이블. */
	.ops = &fsl_pamu_platform_ops,
};

/* Set the domain stash attribute */
/*
 * [한국어]
 * fsl_pamu_configure_l1_stash - 도메인의 스태시 목적지를 특정 CPU의 L1으로 설정한다
 *
 * @domain: 대상 도메인(qman_portal이 만들어 둔 UNMANAGED 도메인).
 * @cpu: 스태시 목적지가 될 CPU 번호.
 * @return: 0 성공, -EINVAL(그 CPU의 L1 스태시 ID를 찾을 수 없음),
 *          하드웨어 갱신 실패 시 그 errno.
 *
 * 왜 필요한가: 이것이 이 드라이버 전체가 존재하는 실질적인 이유다.
 * DPAA의 QMan 포털은 특정 CPU가 전담해 처리하는데, 그 포털의 DMA 데이터를
 * 미리 그 CPU의 L1 캐시에 밀어 넣어 두면 패킷 처리 지연이 크게 줄어든다.
 * drivers/soc/fsl/qbman/qman_portal.c가 CPU 바인딩을 정한 뒤 이 함수를 불러
 * 하드웨어에 그 결정을 반영한다.
 *
 * 동작 과정:
 *  1) domain_lock을 잡는다(리스트 순회와 stash_id 갱신을 보호).
 *  2) get_stash_id(PAMU_ATTR_CACHE_L1, cpu)로 그 CPU의 L1 캐시 ID를 구한다.
 *     디바이스 트리의 캐시 노드에서 cache-stash-id를 읽는 방식이다.
 *  3) ~0이 나오면 그 CPU의 스태시 정보를 찾지 못한 것이라 -EINVAL.
 *  4) update_domain_stash()로 도메인의 모든 LIODN에 새 ID를 써 넣는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트. domain_lock을 irqsave로 잡은 상태에서
 * 하위 함수가 iommu_lock을 추가로 잡는다(정해진 락 순서).
 * static이 아닌 이유: qman_portal.c가 직접 호출하는 유일한 외부 API다.
 *
 * 호출 체인:
 *   qman_portal.c(init_pcfg/portal CPU 바인딩) → [fsl_pamu_configure_l1_stash]
 *   → get_stash_id(), update_domain_stash() → update_liodn_stash()
 */
int fsl_pamu_configure_l1_stash(struct iommu_domain *domain, u32 cpu)
{
	/* [한국어] 일반 도메인 포인터를 PAMU 전용 구조체로 복원한다. */
	struct fsl_dma_domain *dma_domain = to_fsl_dma_domain(domain);
	/* [한국어] irqsave용 플래그. */
	unsigned long flags;
	/* [한국어] 하드웨어 갱신 결과. */
	int ret;

	/* [한국어] stash_id 필드와 디바이스 리스트를 함께 보호한다 —
	 * 두 가지가 일관되게 갱신되어야 하기 때문이다. */
	spin_lock_irqsave(&dma_domain->domain_lock, flags);
	/* [한국어] 주어진 CPU의 L1 캐시에 대응하는 하드웨어 스태시 ID를 구해
	 * 도메인 상태에 기록한다. 이후 새로 attach되는 LIODN도
	 * pamu_set_liodn()에서 이 값을 읽어 쓰게 된다. */
	dma_domain->stash_id = get_stash_id(PAMU_ATTR_CACHE_L1, cpu);
	/* [한국어] ~0은 이 드라이버의 "찾지 못함" 표식이다. 디바이스 트리에
	 * 해당 CPU의 캐시 스태시 ID가 없을 때 이 경로를 탄다. */
	if (dma_domain->stash_id == ~(u32)0) {
		pr_debug("Invalid stash attributes\n");	/* [한국어] 디바이스 트리에 그 CPU의 캐시 스태시 ID가 없다는 뜻이다. */
		spin_unlock_irqrestore(&dma_domain->domain_lock, flags);	/* [한국어] 실패 경로의 락 해제. */
		return -EINVAL;	/* [한국어] 잘못된 인자로 보고한다 — 호출자(qman_portal)가 스태시를 포기한다. */
	}
	/* [한국어] 이미 붙어 있는 모든 LIODN의 PAACE에도 새 목적지를 반영한다.
	 * 도메인 상태만 바꾸면 기존 LIODN은 옛 목적지로 계속 스태시하게 된다. */
	ret = update_domain_stash(dma_domain, dma_domain->stash_id);
	spin_unlock_irqrestore(&dma_domain->domain_lock, flags);

	/* [한국어] 전부 성공했으면 0, 하나라도 실패했으면 그 오류를 반환한다. */
	return ret;
}

/*
 * [한국어]
 * check_pci_ctl_endpt_part - PCIe 컨트롤러가 엔드포인트 파티셔닝을 지원하는지 본다
 *
 * @pci_ctl: 검사할 PCI 호스트 컨트롤러.
 * @return: 리비전이 0x204 이상이면 true(엔드포인트별 분리 가능), 아니면 false.
 *
 * 왜 필요한가: IOMMU 그룹은 "함께 격리될 수밖에 없는 디바이스 묶음"이다.
 * 오래된 Freescale PCIe 컨트롤러는 엔드포인트별로 서로 다른 LIODN을 붙일 수
 * 없어, 그 컨트롤러 아래 모든 디바이스가 한 그룹이 되어야 한다. 리비전
 * 0x204 이상의 컨트롤러는 엔드포인트를 개별 파티션으로 나눌 수 있어 디바이스
 * 하나당 그룹 하나를 줄 수 있다.
 *
 * 동작 과정: BRR1(Block Revision Register 1)을 컨피그 공간 창을 통해 읽고,
 * 버전 필드만 마스킹해 0x204와 비교한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 probe/그룹 결정 시).
 * in_be32()를 쓰는 이유: PowerPC의 이 레지스터는 빅엔디언이므로 명시적
 * 빅엔디언 MMIO 읽기가 필요하다.
 * (PCI_FSL_BRR1 >> 2)로 나누는 이유: cfg_addr가 u32 포인터라 바이트 오프셋을
 * 워드 인덱스로 바꿔야 하기 때문이다.
 *
 * 호출 체인:
 *   fsl_pamu_device_group() → [check_pci_ctl_endpt_part] → in_be32()
 *
 * 참고: 함수 정의의 "static  bool"에 공백이 두 칸인 것은 원본 그대로다.
 */
static  bool check_pci_ctl_endpt_part(struct pci_controller *pci_ctl)
{
	/* [한국어] BRR1에서 읽어 온 리비전 값을 담는다. */
	u32 version;

	/* Check the PCI controller version number by readding BRR1 register */
	/* [한국어] 컨트롤러의 컨피그 주소 창을 통해 BRR1을 빅엔디언으로 읽는다.
	 * cfg_addr가 u32* 타입이라 바이트 오프셋 PCI_FSL_BRR1을 4로 나눠
	 * (>> 2) 워드 인덱스로 변환한다. */
	version = in_be32(pci_ctl->cfg_addr + (PCI_FSL_BRR1 >> 2));
	/* [한국어] 레지스터에는 리비전 외의 필드도 있으므로 버전 비트만 남긴다. */
	version &= PCI_FSL_BRR1_VER;
	/* If PCI controller version is >= 0x204 we can partition endpoints */
	/* [한국어] 0x204가 엔드포인트 파티셔닝이 도입된 리비전이다. 이 이상이면
	 * 디바이스별 IOMMU 그룹을 줄 수 있다. */
	return version >= 0x204;
}

/*
 * [한국어]
 * fsl_pamu_device_group - 디바이스가 속할 IOMMU 그룹을 결정한다
 *
 * @dev: 그룹을 정할 디바이스.
 * @return: 그룹 포인터(참조 카운트가 증가된 상태), 실패 시 ERR_PTR.
 *
 * 왜 필요한가: IOMMU 그룹은 격리의 최소 단위다 — 같은 그룹의 디바이스들은
 * 서로 분리할 수 없으므로 VFIO 등에 함께 넘겨야 한다. PAMU에서 격리 단위는
 * LIODN이고, LIODN을 누가 공유하느냐가 곧 그룹 경계가 된다.
 *
 * 세 가지 경우:
 *  1) 플랫폼 디바이스: 각자 고유한 LIODN을 가지므로 디바이스마다 별도 그룹.
 *  2) 최신 PCIe 컨트롤러(리비전 >= 0x204): 엔드포인트를 파티션할 수 있으므로
 *     표준 PCI 그룹 규칙(ACS 등)을 따른다.
 *  3) 구형 PCIe 컨트롤러: 컨트롤러 하나에 LIODN 하나뿐이라 그 아래 모든
 *     디바이스가 한 그룹이어야 한다. 컨트롤러의 부모 플랫폼 디바이스가 이미
 *     가진 그룹을 그대로 재사용한다.
 *
 * 3번이 성립하는 이유(원본 주석의 요지): fsl_pamu_init()이 fsl_pci_init()보다
 * 먼저 실행되도록 초기화 순서가 잡혀 있어, PCI 디바이스가 나타날 시점에는
 * 컨트롤러의 부모 플랫폼 디바이스가 이미 이 드라이버에 바인딩되어 그룹을
 * 갖고 있음이 보장된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 probe 경로).
 * 참조 카운트: 반환하는 그룹은 참조가 하나 잡힌 상태여야 한다 —
 * iommu_group_get()이 그 역할을 하고, generic/pci 헬퍼도 마찬가지다.
 *
 * 호출 체인:
 *   iommu_group_get_for_dev() → iommu_ops->device_group → [fsl_pamu_device_group]
 *   → generic_device_group() / pci_device_group() / iommu_group_get()
 */
static struct iommu_group *fsl_pamu_device_group(struct device *dev)
{
	/* [한국어] 재사용할 기존 그룹을 담을 포인터(구형 PCIe 경로에서만 쓴다). */
	struct iommu_group *group;
	/* [한국어] PCI 경로에서 쓰는 임시 포인터. */
	struct pci_dev *pdev;

	/*
	 * For platform devices we allocate a separate group for each of the
	 * devices.
	 */
	/* [한국어] 플랫폼 디바이스는 저마다 fsl,liodn을 갖고 있어 개별 격리가
	 * 가능하므로, 코어의 범용 헬퍼로 디바이스당 새 그룹을 만든다. */
	if (!dev_is_pci(dev))
		return generic_device_group(dev);

	/*
	 * We can partition PCIe devices so assign device group to the device
	 */
	/* [한국어] PCI device로 캐스팅한다. */
	pdev = to_pci_dev(dev);
	/* [한국어] 컨트롤러 리비전이 충분히 높으면 엔드포인트를 개별 파티션으로
	 * 다룰 수 있으므로, 표준 PCI 그룹 결정 로직(ACS/브리지 관계 고려)에 맡긴다. */
	if (check_pci_ctl_endpt_part(pci_bus_to_host(pdev->bus)))
		return pci_device_group(&pdev->dev);

	/*
	 * All devices connected to the controller will share the same device
	 * group.
	 *
	 * Due to ordering between fsl_pamu_init() and fsl_pci_init() it is
	 * guaranteed that the pci_ctl->parent platform_device will have the
	 * iommu driver bound and will already have a group set. So we just
	 * re-use this group as the group for every device in the hose.
	 */
	/* [한국어] 구형 컨트롤러: 컨트롤러의 부모 플랫폼 디바이스가 이미 가진
	 * 그룹을 가져온다. iommu_group_get()은 참조를 하나 올려 주므로,
	 * 호출자가 나중에 put 할 때 균형이 맞는다. */
	group = iommu_group_get(pci_bus_to_host(pdev->bus)->parent);
	/* [한국어] 초기화 순서 보장이 깨졌다면 그룹이 없을 수 있다. 이론상
	 * 일어나지 않아야 하는 상황이라 WARN_ON으로 스택 트레이스를 남긴다. */
	if (WARN_ON(!group))
		return ERR_PTR(-EINVAL);
	/* [한국어] 컨트롤러 아래 모든 PCI 디바이스가 이 그룹을 공유하게 된다. */
	return group;
}

/*
 * [한국어]
 * fsl_pamu_probe_device - 이 디바이스를 PAMU가 담당할지 결정한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당하면 &pamu_iommu, 담당하지 않으면 ERR_PTR(-ENODEV).
 *
 * 왜 필요한가: IOMMU 코어는 새 디바이스가 나타날 때마다 등록된 모든 IOMMU
 * 드라이버에 "이 디바이스 당신 것이오?"라고 묻는다. 이 함수가 그 대답이다.
 * PAMU가 관리할 수 있는 조건은 단순하다 — PCI 디바이스이거나(컨트롤러를 통해
 * 간접적으로 LIODN을 갖는다), 디바이스 트리에 fsl,liodn 프로퍼티가 있어야 한다.
 * 그 프로퍼티는 u-boot이 심어 주므로, 없다면 애초에 PAMU 대상이 아니다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트(디바이스 추가 경로).
 *
 * 호출 체인:
 *   iommu_probe_device() → iommu_ops->probe_device → [fsl_pamu_probe_device]
 */
static struct iommu_device *fsl_pamu_probe_device(struct device *dev)
{
	/*
	 * uboot must fill the fsl,liodn for platform devices to be supported by
	 * the iommu.
	 */
	/* [한국어] PCI가 아니면서 fsl,liodn도 없다면 이 디바이스에 부여된 LIODN이
	 * 없다는 뜻이므로 PAMU가 관리할 방법이 없다. PCI 디바이스는 컨트롤러의
	 * 프로퍼티를 쓰므로 여기서 통과시킨다(실제 확인은 attach 때 이뤄진다). */
	if (!dev_is_pci(dev) &&
	    !of_property_present(dev->of_node, "fsl,liodn"))
		return ERR_PTR(-ENODEV);

	/* [한국어] 이 드라이버의 iommu_device 핸들을 돌려주면, 코어가 그것으로
	 * fsl_pamu_ops를 찾아 이후 콜백을 이 파일로 보낸다. */
	return &pamu_iommu;
}

/* [한국어] IOMMU 코어에 노출하는 이 드라이버의 연산 테이블.
 * 눈에 띄는 것은 없는 항목들이다 — map/unmap이 아예 없다. 그래서 이 드라이버는
 * 진짜 페이징 IOMMU가 아니며, 파일 상단 FIXME들이 지적하는 문제의 뿌리가 여기다. */
static const struct iommu_ops fsl_pamu_ops = {
	/* [한국어] 디바이스가 아무 도메인에도 명시적으로 붙지 않았을 때 놓일
	 * 기본 도메인. 정적 인스턴스 하나를 모두가 공유한다. */
	.default_domain = &fsl_pamu_platform_domain,
	/* [한국어] 기능 질의 — 캐시 코히런시만 true를 반환한다. */
	.capable	= fsl_pamu_capable,
	/* [한국어] 도메인 생성 — UNMANAGED만 지원한다. */
	.domain_alloc	= fsl_pamu_domain_alloc,
	/* [한국어] 디바이스 담당 여부 판정 — fsl,liodn 유무로 결정한다. */
	.probe_device	= fsl_pamu_probe_device,
	/* [한국어] IOMMU 그룹 결정 — 플랫폼/신형 PCIe/구형 PCIe 세 갈래. */
	.device_group   = fsl_pamu_device_group,
	/* [한국어] 일반 도메인(여기서는 UNMANAGED)의 연산 테이블.
	 * 익명 const 구조체를 그 자리에서 정의하는 관용구로, 별도 전역 심볼을
	 * 만들지 않아 이름 공간을 깨끗하게 유지한다. */
	.default_domain_ops = &(const struct iommu_domain_ops) {
		/* [한국어] LIODN을 찾아 PAACE를 구성/활성화하는 핵심 콜백. */
		.attach_dev	= fsl_pamu_attach_device,
		/* [한국어] 항등 변환 + 범위 검사만 하는 주소 조회 콜백. */
		.iova_to_phys	= fsl_pamu_iova_to_phys,
		/* [한국어] 모든 연결을 끊고 도메인을 반납하는 콜백. */
		.free		= fsl_pamu_domain_free,
	}
};

/*
 * [한국어]
 * pamu_domain_init - 도메인 계층을 초기화하고 IOMMU 코어에 등록한다
 *
 * @return: 0 성공, 음수 errno(슬랩 캐시 생성/sysfs 등록/코어 등록 실패).
 *
 * 왜 필요한가: fsl_pamu.c가 하드웨어를 켜고 테이블을 준비한 뒤, 마지막으로
 * 이 함수를 불러 리눅스 IOMMU 서브시스템에 자신을 노출한다. 이 호출이
 * 성공해야 비로소 디바이스들의 probe_device 콜백이 들어오기 시작한다.
 *
 * 동작 과정:
 *  1) iommu_init_mempool() — 슬랩 캐시 두 개 생성.
 *  2) iommu_device_sysfs_add() — /sys/class/iommu/iommu0 노드 생성.
 *  3) iommu_device_register() — 코어에 fsl_pamu_ops 등록. 이 시점부터
 *     콜백이 들어온다.
 *
 * 실행 컨텍스트: 부팅 초기(__init), fsl_pamu.c의 probe 경로.
 * 에러 경로: 3번이 실패하면 2번에서 만든 sysfs 노드를 되돌린다. 다만 1번에서
 * 만든 슬랩 캐시는 어느 실패 경로에서도 파괴되지 않는다 — 부팅 시 한 번뿐인
 * 경로라 실무상 문제가 되지 않지만, 엄밀히는 누수다(코드는 고치지 않는다).
 *
 * 호출 체인:
 *   fsl_pamu.c의 fsl_pamu_probe() → [pamu_domain_init]
 *   → iommu_init_mempool(), iommu_device_sysfs_add(), iommu_device_register()
 */
int __init pamu_domain_init(void)
{
	/* [한국어] 각 단계의 결과를 담는다. */
	int ret = 0;

	/* [한국어] 1단계: 도메인/연결 노드용 슬랩 캐시를 만든다. */
	ret = iommu_init_mempool();
	/* [한국어] 메모리 부족이면 등록 자체가 무의미하므로 즉시 반환한다. */
	if (ret)
		return ret;

	/* [한국어] 2단계: sysfs에 이 IOMMU를 표현하는 노드를 만든다.
	 * 이름 "iommu0"으로 /sys/class/iommu/iommu0가 생기며, 사용자 공간이
	 * 디바이스와 IOMMU의 관계를 조회할 수 있게 된다. */
	ret = iommu_device_sysfs_add(&pamu_iommu, NULL, NULL, "iommu0");
	/* [한국어] sysfs 등록 실패 — 슬랩 캐시는 그대로 남지만(위 주석 참조)
	 * 부팅이 계속되지 않으므로 실무적 영향은 없다. */
	if (ret)
		return ret;

	/* [한국어] 3단계: IOMMU 코어에 연산 테이블을 등록한다. 이 호출이 반환된
	 * 직후부터 버스에 있는 디바이스들에 대해 probe_device 콜백이 시작된다. */
	ret = iommu_device_register(&pamu_iommu, &fsl_pamu_ops, NULL);
	/* [한국어] 등록 실패 시 앞서 만든 sysfs 노드를 반드시 되돌린다 —
	 * 남겨 두면 존재하지 않는 IOMMU가 sysfs에 보이게 된다. */
	if (ret) {
		iommu_device_sysfs_remove(&pamu_iommu);	/* [한국어] 등록에 실패했으니 앞서 만든 sysfs 노드를 되돌린다. */
		pr_err("Can't register iommu device\n");	/* [한국어] 부팅 로그에 남겨야 하는 치명적 실패라 pr_err를 쓴다. */
	}

	/* [한국어] 성공이면 0, 실패면 그 오류를 fsl_pamu.c의 probe로 올려 보낸다. */
	return ret;
}
