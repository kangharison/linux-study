/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2009-2010 Advanced Micro Devices, Inc.
 * Author: Joerg Roedel <jroedel@suse.de>
 */

/*
 * [한국어 설명] AMD IOMMU 드라이버의 내부 인터페이스 (amd_iommu.h)
 *
 * === 파일의 역할 ===
 * 이 드라이버의 소스 파일들(init.c, iommu.c, ppr.c, pasid.c, nested.c,
 * debugfs.c, quirks.c)이 서로를 부르는 함수 선언과, 여러 파일이 함께 쓰는
 * 짧은 인라인 도우미가 모여 있다. 하드웨어 정의는 amd_iommu_types.h 에
 * 있고, 여기는 "코드끼리의 약속"만 담는다.
 *
 * 파일이 이렇게 나뉜 이유: 하드웨어 정의는 값이 바뀌지 않지만 함수 선언은
 * 구현이 옮겨 다닐 때마다 바뀐다. 둘을 섞으면 헤더 하나가 계속 흔들려
 * 전체가 재컴파일된다.
 *
 * 인라인 함수들은 대부분 "포인터를 되짚는" 변환이다 — struct device 에서
 * struct amd_iommu 로, struct iommu_domain 에서 protection_domain 으로.
 * 코어 IOMMU 계층이 벤더 구조체를 모르기 때문에, 콜백마다 이 되짚기가
 * 필요하다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 드라이버 내부에서만 쓰인다. 바깥으로 나가는 인터페이스는
 * include/linux/amd-iommu.h 이고, 코어와의 접점은 amd_iommu_ops 다.
 *
 * 선언이 기능별로 묶여 있어, 그 묶음이 곧 이 드라이버의 구성이기도 하다:
 * 인터럽트 핸들러와 로그 관리 / 도메인 조작 / SVA·PASID / IOPF(페이지 폴트)
 * / GCR3 / PPR / 캐시 무효화 / 인터럽트 재매핑 / DTE 조작 / 중첩 변환.
 *
 * 호출 체인:
 *   iommu.c 의 amd_iommu_ops ← 코어 IOMMU 계층
 *   init.c ← 부팅 경로. 여기 선언된 amd_iommu_prepare/enable 을 x86 인터럽트
 *     재매핑 코드가 부른다.
 *
 * === 타 모듈과의 연결 ===
 * amd_iommu_types.h 의 구조체를 전제로 하고, linux/iommu.h 의 코어 타입을
 * 쓴다. iommu_get_iommu_dev, iommu_domain, iopf_fault 같은 코어 개념이
 * 여기 선언의 인자로 그대로 나타난다.
 *
 * 데이터 흐름: 코어 → amd_iommu_ops → 여기 선언된 함수들 → 하드웨어.
 * 반대로 하드웨어의 로그(이벤트/PPR/GA)는 인터럽트 핸들러를 통해 올라온다.
 *
 * === 주요 함수/구조체 요약 ===
 * - amd_iommu_int_thread_*(): 세 종류 로그의 인터럽트 처리.
 * - amd_iommu_prepare/enable/disable/reenable(): 인터럽트 재매핑 초기화의
 *   네 단계. x86 코드가 이 순서로 부른다.
 * - amd_iommu_set_gcr3()/clear_gcr3(): PASID 별 페이지 테이블 연결.
 * - amd_iommu_flush_all_caches()/domain_flush_pages(): 무효화의 두 축.
 * - get_amd_iommu_from_dev()/to_pdomain(): 코어 타입에서 벤더 구조체로의 되짚기.
 * - amd_iommu_make_clear_dte(): DTE 를 초기 상태로 되돌리되, IVRS 가 지정한
 *   비트는 되살린다.
 * - iommu_virt_to_phys()/phys_to_virt(): SME(메모리 암호화) 비트를 다루는
 *   주소 변환.
 */
#ifndef AMD_IOMMU_H	/* [한국어] 헤더 중복 포함 방지 */
#define AMD_IOMMU_H

#include <linux/iommu.h>	/* [한국어] 코어 IOMMU 계층의 타입 — 아래 선언의 인자로 그대로 나타난다 */

#include "amd_iommu_types.h"	/* [한국어] 하드웨어 정의와 드라이버 구조체 */

irqreturn_t amd_iommu_int_thread(int irq, void *data);	/* [한국어] 세 로그를 모두 확인하는 통합 인터럽트 스레드. 로그별 인터럽트를 따로 잡지 못한 경우에 쓴다 */
irqreturn_t amd_iommu_int_thread_evtlog(int irq, void *data);	/* [한국어] 이벤트 로그 전용 스레드 — 변환 실패와 하드웨어 오류가 여기로 온다 */
irqreturn_t amd_iommu_int_thread_pprlog(int irq, void *data);	/* [한국어] PPR 로그 전용 — 장치가 낸 페이지 폴트 */
irqreturn_t amd_iommu_int_thread_galog(int irq, void *data);	/* [한국어] GA 로그 전용 — 게스트에 전달하지 못한 인터럽트 */
void amd_iommu_restart_log(struct amd_iommu *iommu, const char *evt_type,	/* [한국어] 로그가 넘쳤을 때 되살리는 공통 절차. 세 로그가 같은 순서를 따르므로 인자로 갈라 재사용한다 */
			   u8 cntrl_intr, u8 cntrl_log,
			   u32 status_run_mask, u32 status_overflow_mask);
void amd_iommu_restart_event_logging(struct amd_iommu *iommu);	/* [한국어] 이벤트 로그를 되살린다 */
void amd_iommu_restart_ga_log(struct amd_iommu *iommu);	/* [한국어] GA 로그를 되살린다 */
void amd_iommu_restart_ppr_log(struct amd_iommu *iommu);	/* [한국어] PPR 로그를 되살린다. 넘친 동안의 요청은 잃어버리므로 장치가 멈출 수 있다 */
void amd_iommu_set_rlookup_table(struct amd_iommu *iommu, u16 devid);	/* [한국어] 장치 id → 담당 유닛 대응을 기록한다. 이후 모든 조회가 이 표를 쓴다 */
void iommu_feature_enable(struct amd_iommu *iommu, u8 bit);	/* [한국어] 제어 레지스터의 비트 하나를 켠다. 초기화가 기능을 하나씩 활성화하는 통로 */
void *__init iommu_alloc_4k_pages(struct amd_iommu *iommu,	/* [한국어] 하드웨어가 읽을 표를 잡는다. 유닛과 같은 NUMA 노드에서 할당해 접근 지연을 줄인다 */
				  gfp_t gfp, size_t size);

#ifdef CONFIG_AMD_IOMMU_DEBUGFS	/* [한국어] debugfs 를 켠 커널에서만 실제 구현이 있다 */
void amd_iommu_debugfs_setup(void);	/* [한국어] debugfs 파일을 만든다 */
#else
static inline void amd_iommu_debugfs_setup(void) {}	/* [한국어] debugfs 를 끈 커널에서는 빈 함수 — 호출부를 #ifdef 로 감싸지 않기 위해서다 */
#endif

/* Needed for interrupt remapping */
int amd_iommu_prepare(void);	/* [한국어] 인터럽트 재매핑을 켜기 전 사전 조건 확인과 자원 준비 (원 주석: Needed for interrupt remapping) */
int amd_iommu_enable(void);	/* [한국어] 준비된 유닛들의 재매핑을 실제로 켠다 */
void amd_iommu_disable(void);	/* [한국어] 재매핑을 끈다. 서스펜드와 kexec 경로 */
int amd_iommu_reenable(int mode);	/* [한국어] 레주메 후 같은 설정으로 되살린다 */
int amd_iommu_enable_faulting(unsigned int cpu);	/* [한국어] 폴트 보고 인터럽트를 켠다. CPU 마다 불린다 */
extern int amd_iommu_guest_ir;	/* [한국어] 게스트 인터럽트 전달 모드 (enum amd_iommu_intr_mode_type) */
extern enum protection_domain_mode amd_iommu_pgtable;	/* [한국어] 기본으로 쓸 페이지 테이블 형식(v1/v2) */
extern int amd_iommu_gpt_level;	/* [한국어] 게스트 페이지 테이블의 레벨 수(4 또는 5) */
extern u8 amd_iommu_hpt_level;	/* [한국어] 호스트 페이지 테이블의 레벨 수 */
extern unsigned long amd_iommu_pgsize_bitmap;	/* [한국어] 코어에 알릴 지원 페이지 크기 비트맵. 형식에 따라 달라진다 */
extern bool amd_iommu_hatdis;	/* [한국어] 펌웨어가 호스트 주소 변환을 쓰지 말라고 지시했는지 */

/* Protection domain ops */
void amd_iommu_init_identity_domain(void);	/* [한국어] 항등(패스스루) 도메인을 만든다 (원 주석: Protection domain ops) */
struct protection_domain *protection_domain_alloc(void);	/* [한국어] 빈 도메인 하나를 만든다 */
struct iommu_domain *amd_iommu_domain_alloc_sva(struct device *dev,	/* [한국어] 프로세스의 mm 을 그대로 쓰는 SVA 도메인을 만든다 */
						struct mm_struct *mm);
void amd_iommu_domain_free(struct iommu_domain *dom);	/* [한국어] 도메인과 그 페이지 테이블을 놓는다 */
int iommu_sva_set_dev_pasid(struct iommu_domain *domain,	/* [한국어] 장치의 한 PASID 를 SVA 도메인에 붙인다 */
			    struct device *dev, ioasid_t pasid,
			    struct iommu_domain *old);
void amd_iommu_remove_dev_pasid(struct device *dev, ioasid_t pasid,	/* [한국어] 그 연결을 끊는다 */
				struct iommu_domain *domain);

/* SVA/PASID */
bool amd_iommu_pasid_supported(void);	/* [한국어] 이 시스템에서 PASID 를 쓸 수 있는가 (원 주석: SVA/PASID) */

/* IOPF */
int amd_iommu_iopf_init(struct amd_iommu *iommu);	/* [한국어] 페이지 폴트 처리 큐를 만든다 (원 주석: IOPF) */
void amd_iommu_iopf_uninit(struct amd_iommu *iommu);	/* [한국어] 그 큐를 놓는다 */
void amd_iommu_page_response(struct device *dev, struct iopf_fault *evt,	/* [한국어] 페이지 폴트 처리 결과를 장치에 알린다. 응답하지 않으면 장치가 영원히 멈춘다 */
			     struct iommu_page_response *resp);
int amd_iommu_iopf_add_device(struct amd_iommu *iommu,	/* [한국어] 장치를 폴트 처리 큐에 등록한다 */
			      struct iommu_dev_data *dev_data);
void amd_iommu_iopf_remove_device(struct amd_iommu *iommu,	/* [한국어] 등록을 해제한다 */
				  struct iommu_dev_data *dev_data);

/* GCR3 setup */
int amd_iommu_set_gcr3(struct iommu_dev_data *dev_data,	/* [한국어] PASID 하나에 페이지 테이블 루트를 연결한다 (원 주석: GCR3 setup) */
		       ioasid_t pasid, unsigned long gcr3);
int amd_iommu_clear_gcr3(struct iommu_dev_data *dev_data, ioasid_t pasid);	/* [한국어] 그 연결을 끊는다 */

/* PPR */
int __init amd_iommu_alloc_ppr_log(struct amd_iommu *iommu);	/* [한국어] PPR 로그 버퍼를 잡는다 (원 주석: PPR) */
void __init amd_iommu_free_ppr_log(struct amd_iommu *iommu);	/* [한국어] 그 버퍼를 놓는다 */
void amd_iommu_enable_ppr_log(struct amd_iommu *iommu);	/* [한국어] PPR 로그를 하드웨어에 걸고 켠다 */
void amd_iommu_poll_ppr_log(struct amd_iommu *iommu);	/* [한국어] 쌓인 페이지 요청을 꺼내 처리한다 */
int amd_iommu_complete_ppr(struct device *dev, u32 pasid, int status, int tag);	/* [한국어] 요청 하나에 응답을 보낸다. tag 로 장치가 어느 요청의 답인지 안다 */

/*
 * This function flushes all internal caches of
 * the IOMMU used by this driver.
 */
void amd_iommu_flush_all_caches(struct amd_iommu *iommu);	/* [한국어] (위 영어 주석에 이어) 이 유닛의 모든 캐시를 비운다. 초기화와 상태를 알 수 없는 상황에서 쓴다 */
void amd_iommu_domain_flush_pages(struct protection_domain *domain,	/* [한국어] 도메인의 특정 주소 범위만 무효화한다. unmap 의 정상 경로 */
				  u64 address, size_t size);
void amd_iommu_dev_flush_pasid_pages(struct iommu_dev_data *dev_data,	/* [한국어] 장치의 특정 PASID 에 대해서만 범위를 무효화한다 */
				     ioasid_t pasid, u64 address, size_t size);

#ifdef CONFIG_IRQ_REMAP	/* [한국어] 인터럽트 재매핑을 켠 커널에서만 도메인을 만든다 */
int amd_iommu_create_irq_domain(struct amd_iommu *iommu);	/* [한국어] 이 유닛의 인터럽트 재매핑 도메인을 만든다 */
#else
/*
 * [한국어]
 * amd_iommu_create_irq_domain (빈 구현) - 인터럽트 재매핑을 끈 커널용
 *
 * @iommu: 무시된다.
 * @return: 항상 0.
 *
 * 재매핑 기능 자체가 컴파일되지 않으므로 만들 도메인이 없다. 성공을
 * 돌려주는 이유: 유닛 초기화 경로가 이 호출의 실패를 치명적으로 다루기
 * 때문에, 여기서 오류를 내면 재매핑을 끈 커널이 부팅하지 못한다.
 *
 * 빈 함수를 두는 덕에 호출부는 #ifdef 없이 한 가지로 쓸 수 있다.
 */
static inline int amd_iommu_create_irq_domain(struct amd_iommu *iommu)
{
	return 0;	/* [한국어] 재매핑을 끈 커널에서는 아무것도 하지 않고 성공을 돌려준다 */
}
#endif

/*
 * [한국어]
 * is_rd890_iommu - 이 IOMMU 가 RD890 칩셋의 것인지 판별한다
 *
 * @pdev: IOMMU 의 PCI 장치.
 * @return: RD890 이면 참.
 *
 * RD890 은 레주메 때 BIOS 가 IOMMU 레지스터를 복원해 주지 않아, 드라이버가
 * 직접 저장했다가 되쓰는 특별 처리가 필요하다. struct amd_iommu 의
 * stored_l1/stored_l2 배열이 그 때문에 존재한다.
 *
 * 벤더/디바이스 id 로 판별하는 것이 유일한 방법이다 — 이 결함을 알려 주는
 * 능력 비트가 없다.
 *
 * 호출 체인:
 *   서스펜드/레주메 경로 → [이 함수]
 */
static inline bool is_rd890_iommu(struct pci_dev *pdev)
{
	return (pdev->vendor == PCI_VENDOR_ID_ATI) &&	/* [한국어] 벤더가 ATI 이고 */
	       (pdev->device == PCI_DEVICE_ID_RD890_IOMMU);	/* [한국어] 디바이스 id 가 RD890 의 IOMMU 인가. 결함을 알려 주는 능력 비트가 없어 id 로 판별할 수밖에 없다 */
}

/*
 * [한국어]
 * check_feature - 확장 기능 비트가 서 있는지 확인한다
 *
 * @mask: FEATURE_* 상수.
 * @return: 그 기능을 쓸 수 있으면 참.
 *
 * 전역 amd_iommu_efr 을 보는 것이 핵심이다. 유닛마다 능력이 다를 수 있지만,
 * 드라이버는 모든 유닛이 공통으로 지원하는 것만 쓴다 — 유닛에 따라 되고
 * 안 되는 기능은 상위 계층이 일관되게 다룰 수 없기 때문이다. 초기화가
 * 유닛들의 EFR 을 AND 해 이 전역값을 만든다.
 *
 * 호출 체인:
 *   초기화와 기능 판별 경로 전반 → [이 함수]
 */
static inline bool check_feature(u64 mask)
{
	return (amd_iommu_efr & mask);	/* [한국어] 모든 유닛이 공통으로 지원하는 기능만 담긴 전역값을 본다 */
}

/*
 * [한국어]
 * check_feature2 - 두 번째 확장 기능 워드의 비트를 확인한다
 *
 * @mask: FEATURE_* (EFR2 쪽) 상수.
 * @return: 지원하면 참.
 *
 * check_feature 와 같은 규칙이다. 기능 비트가 64개를 넘어 워드가 하나 더
 * 생겼을 뿐이다.
 */
static inline bool check_feature2(u64 mask)
{
	return (amd_iommu_efr2 & mask);	/* [한국어] 두 번째 기능 워드의 공통값 */
}

/*
 * [한국어]
 * amd_iommu_v2_pgtbl_supported - v2(x86-64 형식) 페이지 테이블을 쓸 수 있는가
 *
 * @return: 두 기능이 모두 있으면 참.
 *
 * 두 비트를 함께 요구하는 이유:
 *  - GIOSUP: 게스트 I/O 프로텍션. v2 형식 자체의 지원을 뜻한다.
 *  - GT: 게스트 변환. PASID 별로 다른 페이지 테이블을 쓸 수 있어야 한다.
 *
 * v2 가 되어야 프로세스의 페이지 테이블을 그대로 IOMMU 에 걸 수 있고, 그것이
 * SVA 의 성립 조건이다. 그래서 이 판별이 곧 "이 기계에서 SVA 가 가능한가"의
 * 첫 관문이다.
 *
 * 호출 체인:
 *   amd_iommu_gt_ppr_supported()/도메인 형식 결정 → [이 함수]
 */
static inline bool amd_iommu_v2_pgtbl_supported(void)
{
	return (check_feature(FEATURE_GIOSUP) && check_feature(FEATURE_GT));	/* [한국어] v2 형식 자체와 PASID 별 변환을 모두 지원해야 한다 */
}

/*
 * [한국어]
 * amd_iommu_gt_ppr_supported - SVA 에 필요한 것이 모두 갖춰졌는가
 *
 * @return: 세 조건이 모두 참일 때만 참.
 *
 * SVA 는 "장치가 프로세스의 주소 공간을 그대로 쓰고, 페이지가 없으면
 * 폴트를 내어 커널이 가져다준다"는 것이다. 그러려면 세 가지가 다 있어야 한다.
 *  - v2 페이지 테이블: 프로세스의 테이블을 그대로 걸 수 있어야 한다.
 *  - PPR: 장치가 페이지 폴트를 보고할 수 있어야 한다. 없으면 매핑되지
 *    않은 페이지에 닿는 순간 그냥 실패한다.
 *  - EPHSUP: 향상된 PPR 처리. 응답 프로토콜이 제대로 갖춰져 있어야 한다.
 *
 * 하나라도 없으면 SVA 를 광고하지 않는다 — 반쯤 되는 상태로 두면 장치가
 * 폴트를 내고 영원히 멈춘다.
 *
 * 호출 체인:
 *   amd_iommu_pasid_supported()/초기화 → [이 함수]
 */
static inline bool amd_iommu_gt_ppr_supported(void)
{
	return (amd_iommu_v2_pgtbl_supported() &&	/* [한국어] 프로세스의 페이지 테이블을 걸 수 있고 */
		check_feature(FEATURE_PPR) &&	/* [한국어] 장치가 페이지 폴트를 보고할 수 있고 */
		check_feature(FEATURE_EPHSUP));	/* [한국어] 응답 프로토콜까지 갖춰졌을 때만 SVA 를 광고한다 */
}

/*
 * [한국어]
 * iommu_virt_to_phys - 커널 주소를 하드웨어가 쓸 물리 주소로 바꾼다
 *
 * @vaddr: 커널 가상 주소.
 * @return: SME 암호화 비트가 붙은 물리 주소.
 *
 * 평범한 virt_to_phys 로 끝나지 않는 이유가 __sme_set 이다. AMD 의 메모리
 * 암호화(SME)가 켜져 있으면 물리 주소의 특정 비트가 "이 접근은 암호화된
 * 메모리"를 뜻한다. IOMMU 가 읽을 표는 커널이 쓴 것과 같은 방식으로
 * 암호화되어 있으므로, 하드웨어에 주소를 알릴 때 그 비트를 붙여야 한다.
 *
 * 붙이지 않으면 하드웨어가 암호문을 평문으로 읽어 쓰레기 값을 표로 해석한다.
 *
 * 호출 체인:
 *   장치 테이블·페이지 테이블·명령 버퍼의 주소를 하드웨어에 알리는 모든 곳
 */
static inline u64 iommu_virt_to_phys(void *vaddr)
{
	return (u64)__sme_set(virt_to_phys(vaddr));	/* [한국어] 물리 주소에 SME 암호화 비트를 붙인다. 빼면 하드웨어가 암호문을 평문으로 읽는다 */
}

/*
 * [한국어]
 * iommu_phys_to_virt - 하드웨어가 쓰던 물리 주소를 커널 주소로 되돌린다
 *
 * @paddr: SME 비트가 붙어 있을 수 있는 물리 주소.
 * @return: 커널 가상 주소.
 *
 * iommu_virt_to_phys 의 역이다. __sme_clr 로 암호화 비트를 떼어 내야
 * phys_to_virt 가 올바른 주소를 준다 — 그 비트가 남아 있으면 물리 주소가
 * 범위를 한참 벗어난 값이 된다.
 *
 * 호출 체인:
 *   표를 순회하며 하위 단계로 내려가는 경로
 */
static inline void *iommu_phys_to_virt(unsigned long paddr)
{
	return phys_to_virt(__sme_clr(paddr));	/* [한국어] 암호화 비트를 떼야 올바른 물리 주소가 된다 */
}

/*
 * [한국어]
 * get_pci_sbdf_id - PCI 장치의 세그먼트+BDF 조회 키를 만든다
 *
 * @pdev: 대상 장치.
 * @return: 세그먼트(상위 16비트) + BDF(하위 16비트).
 *
 * 세그먼트가 여럿인 시스템에서는 BDF 만으로 장치를 특정할 수 없다. 드라이버
 * 내부의 조회는 모두 이 합친 키를 쓰므로, 그 변환을 한 곳에 모아 둔 것이다.
 *
 * 호출 체인:
 *   장치를 찾거나 등록하는 경로 → [이 함수]
 */
static inline int get_pci_sbdf_id(struct pci_dev *pdev)
{
	int seg = pci_domain_nr(pdev->bus);	/* [한국어] 이 장치가 속한 PCI 세그먼트 */
	u16 devid = pci_dev_id(pdev);	/* [한국어] 버스/장치/기능을 합친 16비트 id */

	return PCI_SEG_DEVID_TO_SBDF(seg, devid);	/* [한국어] 둘을 합쳐 드라이버 내부의 조회 키를 만든다 */
}

bool amd_iommu_ht_range_ignore(void);	/* [한국어] HyperTransport 주소 범위를 변환에서 제외하지 않아도 되는지. 일부 플랫폼에서 그 범위가 정상 메모리다 */

/*
 * This must be called after device probe completes. During probe
 * use rlookup_amd_iommu() get the iommu.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * get_amd_iommu_from_dev - 장치를 담당하는 IOMMU 유닛을 얻는다
 *
 * @dev: 대상 장치.
 * @return: 그 장치를 담당하는 struct amd_iommu.
 *
 * 코어가 장치에 붙여 둔 iommu_device 포인터에서 벤더 구조체를 되짚는다.
 *
 * 원 주석이 강조하는 제약이 중요하다: probe 가 끝난 뒤에만 쓸 수 있다.
 * probe 중에는 아직 그 연결이 만들어지지 않아, 그때는 rlookup_amd_iommu()
 * 로 장치 id 를 통해 찾아야 한다. 두 함수가 나뉜 이유가 그것이다.
 *
 * 호출 체인:
 *   iommu_ops 콜백 전반 → [이 함수]
 */
static inline struct amd_iommu *get_amd_iommu_from_dev(struct device *dev)
{
	return iommu_get_iommu_dev(dev, struct amd_iommu, iommu);	/* [한국어] 코어가 장치에 붙여 둔 iommu_device 에서 벤더 구조체로 되짚는다 */
}

/* This must be called after device probe completes. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * get_amd_iommu_from_dev_data - dev_data 에서 담당 유닛을 얻는다
 *
 * @dev_data: 장치의 IOMMU 쪽 상태.
 * @return: 담당 유닛.
 *
 * 위 함수와 같고 인자만 다르다. 드라이버 내부 경로는 struct device 보다
 * dev_data 를 들고 다니는 경우가 많아 두 입구를 둔 것이다.
 *
 * 같은 제약이 적용된다 — probe 완료 후에만 유효하다.
 */
static inline struct amd_iommu *get_amd_iommu_from_dev_data(struct iommu_dev_data *dev_data)
{
	return iommu_get_iommu_dev(dev_data->dev, struct amd_iommu, iommu);	/* [한국어] dev_data 가 들고 있는 장치를 거쳐 같은 되짚기를 한다 */
}

/*
 * [한국어]
 * to_pdomain - 코어의 iommu_domain 에서 AMD 도메인으로 되짚는다
 *
 * @dom: 코어가 넘겨준 도메인.
 * @return: 그것을 품고 있는 struct protection_domain.
 *
 * 코어 IOMMU 계층은 벤더 구조체를 모르므로 콜백에 iommu_domain 만 넘긴다.
 * 이 드라이버의 거의 모든 도메인 콜백이 첫 줄에서 이 변환을 한다.
 *
 * protection_domain 의 첫 union 안에 domain 이 있어 오프셋이 0 이지만,
 * container_of 를 쓰면 나중에 배치가 바뀌어도 안전하다.
 */
static inline struct protection_domain *to_pdomain(struct iommu_domain *dom)
{
	return container_of(dom, struct protection_domain, domain);	/* [한국어] 코어가 넘긴 도메인을 품고 있는 AMD 구조체로 */
}

bool translation_pre_enabled(struct amd_iommu *iommu);	/* [한국어] 커널 진입 전부터 변환이 켜져 있었는가. kdump 경로의 판단 근거 */
int __init add_special_device(u8 type, u8 id, u32 *devid, bool cmd_line);	/* [한국어] IOAPIC/HPET 같은 비 PCI 장치의 id 대응을 등록한다 */

int amd_iommu_pdom_id_alloc(void);	/* [한국어] 새 도메인 id 를 할당한다 */
int amd_iommu_pdom_id_reserve(u16 id, gfp_t gfp);	/* [한국어] 특정 id 를 예약한다. kdump 에서 물려받은 id 를 그대로 쓸 때 필요하다 */
void amd_iommu_pdom_id_free(int id);	/* [한국어] id 를 반납한다 */
void amd_iommu_pdom_id_destroy(void);	/* [한국어] 할당기 자체를 정리한다 */

#ifdef CONFIG_DMI	/* [한국어] DMI 를 켠 커널에서만 메인보드별 우회가 적용된다 */
void amd_iommu_apply_ivrs_quirks(void);
#else
/* [한국어] DMI 를 끈 커널에서는 아무것도 하지 않는다.
 * 메인보드별 IVRS 우회는 DMI 정보로 기종을 판별해야 가능하므로,
 * 그것 없이는 적용할 방법 자체가 없다. */
static inline void amd_iommu_apply_ivrs_quirks(void) { }
#endif
struct dev_table_entry *amd_iommu_get_ivhd_dte_flags(u16 segid, u16 devid);

void amd_iommu_domain_set_pgtable(struct protection_domain *domain,	/* [한국어] 도메인에 페이지 테이블 루트와 레벨 수를 설정한다 */
				  u64 *root, int mode);
struct dev_table_entry *get_dev_table(struct amd_iommu *iommu);	/* [한국어] 이 유닛이 쓸 장치 테이블을 얻는다. kdump 에서는 물려받은 사본을 돌려줄 수 있다 */
struct iommu_dev_data *search_dev_data(struct amd_iommu *iommu, u16 devid);	/* [한국어] 장치 id 로 그 장치의 IOMMU 상태를 찾는다 */

void amd_iommu_set_dte_v1(struct iommu_dev_data *dev_data,	/* [한국어] v1 페이지 테이블용 DTE 내용을 조립한다 */
			  struct protection_domain *domain, u16 domid,
			  struct pt_iommu_amdv1_hw_info *pt_info,
			  struct dev_table_entry *new);
void amd_iommu_update_dte(struct amd_iommu *iommu,	/* [한국어] 조립된 DTE 를 실제 테이블에 원자적으로 반영하고 캐시를 무효화한다 */
			  struct iommu_dev_data *dev_data,
			  struct dev_table_entry *new);

/*
 * [한국어]
 * amd_iommu_make_clear_dte - DTE 를 "비어 있지만 유효한" 초기 상태로 만든다
 *
 * @dev_data: 대상 장치.
 * @new: 채울 DTE 사본.
 *
 * 장치를 도메인에서 뗄 때 DTE 를 0 으로 밀어 버리면 안 된다. 두 가지 이유가
 * 있다.
 *
 * 첫째, 원 주석이 밝히듯 존재하는 DTE 는 V 비트가 서 있어야 한다. V 가
 * 없는 항목은 "이 장치는 아예 모른다"는 뜻이고, 그 상태의 요청은 이벤트
 * 로그를 채우는 오류가 된다. V 는 있고 TV(변환 유효)가 없는 상태가 곧
 * "차단"이다.
 *
 * 둘째, IVRS 표가 이 장치에 대해 지정한 플래그가 있다. NMI 를 통과시키라거나
 * 시스템 관리 메시지를 특별히 다루라는 펌웨어의 지시인데, 그것까지 지우면
 * 플랫폼이 오동작한다. 그래서 amd_iommu_get_ivhd_dte_flags 로 그 비트를
 * 다시 얹는다.
 *
 * 결과적으로 이 함수는 "장치를 차단하되 플랫폼의 요구는 지킨다"를 만든다.
 *
 * 호출 체인:
 *   장치를 도메인에서 뗄 때 → [이 함수] → amd_iommu_get_ivhd_dte_flags()
 */
static inline void
amd_iommu_make_clear_dte(struct iommu_dev_data *dev_data, struct dev_table_entry *new)
{
	struct dev_table_entry *initial_dte;	/* [한국어] IVRS 가 지정한 초기 플래그 */
	struct amd_iommu *iommu = get_amd_iommu_from_dev(dev_data->dev);	/* [한국어] 세그먼트 id 를 얻기 위해 담당 유닛을 찾는다 */

	/* All existing DTE must have V bit set */
	new->data128[0] = DTE_FLAG_V;	/* [한국어] V 만 세운다. V 없이 두면 요청이 오류가 되어 이벤트 로그를 채운다. V 는 있고 TV 가 없는 상태가 곧 "차단"이다 */
	new->data128[1] = 0;	/* [한국어] 나머지 절반은 비운다 */

	/*
	 * Restore cached persistent DTE bits, which can be set by information
	 * in IVRS table. See set_dev_entry_from_acpi().
	 */
	initial_dte = amd_iommu_get_ivhd_dte_flags(iommu->pci_seg->id, dev_data->devid);	/* [한국어] 펌웨어가 이 장치에 지정한 플래그가 있는지 */
	if (initial_dte) {	/* [한국어] 있다면 */
		new->data128[0] |= initial_dte->data128[0];	/* [한국어] 그 비트를 되살린다 — 지우면 NMI 통과 같은 플랫폼 요구가 깨진다 */
		new->data128[1] |= initial_dte->data128[1];	/* [한국어] 나머지 절반도 같은 이유로 */
	}
}

/* NESTED */
struct iommu_domain *	/* [한국어] (원 주석: NESTED) 게스트의 1단계 변환을 감싸는 중첩 도메인을 만든다 */
amd_iommu_alloc_domain_nested(struct iommufd_viommu *viommu, u32 flags,
			      const struct iommu_user_data *user_data);
#endif /* AMD_IOMMU_H */
