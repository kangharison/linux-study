// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023 Advanced Micro Devices, Inc.
 */

/*
 * [한국어 설명] 장치가 낸 페이지 폴트를 받아 처리로 넘기는 경로 (ppr.c)
 *
 * === 파일의 역할 ===
 * PPR(Peripheral Page Request)은 장치가 "이 주소가 매핑되어 있지 않으니
 * 가져다 달라"고 IOMMU 를 통해 요청하는 기능이다. 이 파일은 그 요청이
 * 쌓이는 로그를 관리하고, 쌓인 것을 꺼내 커널의 공용 페이지 폴트 처리
 * 계층(IOPF)에 넘긴다.
 *
 * 이것이 SVA 의 나머지 절반이다. pasid.c 가 "장치가 프로세스의 페이지
 * 테이블을 본다"를 만들었다면, 여기는 "그 페이지가 아직 없을 때 어떻게
 * 되는가"를 담당한다. 이 경로가 없으면 SVA 는 프로세스의 모든 페이지가
 * 미리 상주해 있을 때만 동작한다.
 *
 * 반드시 지켜야 하는 규칙이 하나 있다: 들어온 요청에는 예외 없이 응답을
 * 보내야 한다. 응답하지 않은 요청이 하나라도 있으면 그 장치는 그 자리에서
 * 영원히 멈춘다. 그래서 이 파일의 오류 경로는 모두 "거절"로 끝나지
 * "무시"로 끝나지 않는다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 아래로는 하드웨어의 PPR 로그를, 위로는 커널의 IOPF 계층을 향한다.
 * 인터럽트가 오면 amd_iommu_poll_ppr_log 가 로그를 비우고, 각 항목을
 * iommu_report_device_fault 로 올린다. 그 뒤 실제 페이지를 가져오는 일은
 * IOPF 워커가 잠들 수 있는 문맥에서 하고, 결과가
 * amd_iommu_page_response 로 되돌아와 장치에 전달된다.
 *
 * 실행 컨텍스트가 둘로 갈린다: 로그를 읽는 쪽은 인터럽트 스레드(잠들 수
 * 없다), 응답을 보내는 쪽은 IOPF 워커(잠들 수 있다). 그 경계가 이 파일의
 * 설계를 정한다 — 여기서는 페이지를 가져오지 않고 큐에 넣기만 한다.
 *
 * 호출 체인:
 *   PPR 인터럽트 → amd_iommu_int_thread_pprlog() → amd_iommu_poll_ppr_log()
 *     → iommu_call_iopf_notifier() → iommu_report_device_fault()
 *   IOPF 워커가 페이지를 처리한 뒤
 *     → amd_iommu_page_response() → amd_iommu_complete_ppr()
 *
 * === 타 모듈과의 연결 ===
 * iommu-pages.h 의 페이지 할당, linux/amd-iommu.h, 그리고 코어의 IOPF
 * 인터페이스(iopf_queue_*, iommu_report_device_fault). 응답은 iommu.c 의
 * amd_iommu_complete_ppr 이 명령으로 만들어 하드웨어에 보낸다.
 *
 * === 주요 함수/구조체 요약 ===
 * - amd_iommu_alloc_ppr_log()/enable_ppr_log(): 로그 버퍼를 잡고 하드웨어에 건다.
 * - amd_iommu_poll_ppr_log(): 쌓인 요청을 꺼내는 소비자. 하드웨어 버그
 *   두 가지를 여기서 흡수한다.
 * - iommu_call_iopf_notifier(): 요청 하나를 검증해 IOPF 계층에 올리거나,
 *   올릴 수 없으면 즉시 거절 응답을 보낸다.
 * - ppr_is_valid(): 처리할 수 없는 요청을 걸러 낸다.
 * - amd_iommu_iopf_init()/add_device(): 유닛별 폴트 처리 큐와 장치 등록.
 * - amd_iommu_page_response(): 처리 결과를 장치에 되돌린다.
 */
#define pr_fmt(fmt)     "AMD-Vi: " fmt	/* [한국어] 이 파일의 로그 접두사 */
#define dev_fmt(fmt)    pr_fmt(fmt)	/* [한국어] dev_dbg 계열도 같은 접두사 */

#include <linux/amd-iommu.h>	/* [한국어] 드라이버 외부 인터페이스(amd_iommu_snp_en 등) */
#include <linux/delay.h>	/* [한국어] udelay — 항목이 메모리에 도착하기를 기다리는 데 쓴다 */
#include <linux/mmu_notifier.h>	/* [한국어] SVA 도메인과 함께 쓰이는 타입 */

#include <asm/iommu.h>	/* [한국어] 아키텍처별 IOMMU 정의 */

#include "amd_iommu.h"	/* [한국어] 드라이버 내부 함수 선언 */
#include "amd_iommu_types.h"	/* [한국어] PPR 항목의 비트 배치 */

#include "../iommu-pages.h"	/* [한국어] 하드웨어용 페이지 할당기 */

/*
 * [한국어]
 * amd_iommu_alloc_ppr_log - PPR 로그 버퍼를 잡는다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공, -ENOMEM 이면 할당 실패.
 *
 * 하드웨어가 직접 쓰는 링 버퍼라 물리적으로 연속이어야 하고, 유닛과 같은
 * NUMA 노드에서 잡아야 접근 지연이 작다. iommu_alloc_4k_pages 가 그 둘을
 * 맡는다.
 *
 * __GFP_ZERO 로 0 초기화하는 것이 중요하다. poll 경로가 "요청 종류가 0 이면
 * 아직 안 쓰인 항목"이라는 규칙으로 하드웨어 버그를 흡수하는데, 그 규칙이
 * 성립하려면 처음이 0 이어야 한다.
 *
 * 호출 체인:
 *   유닛 초기화 → [이 함수] → iommu_alloc_4k_pages()
 */
int __init amd_iommu_alloc_ppr_log(struct amd_iommu *iommu)
{
	iommu->ppr_log = iommu_alloc_4k_pages(iommu, GFP_KERNEL | __GFP_ZERO,	/* [한국어] 유닛과 같은 NUMA 노드에서 연속 페이지를 잡는다. 0 초기화는 poll 경로의 "아직 안 쓰임" 판별의 전제다 */
					      PPR_LOG_SIZE);	/* [한국어] 항목 16바이트 × 512개 */
	return iommu->ppr_log ? 0 : -ENOMEM;	/* [한국어] 실패하면 PPR 을 켜지 않는다 */
}

/*
 * [한국어]
 * amd_iommu_enable_ppr_log - 로그를 하드웨어에 걸고 켠다
 *
 * @iommu: 대상 유닛.
 *
 * 순서가 정해져 있다.
 *  1) CONTROL_PPR_EN 으로 PPR 기능 자체를 먼저 켠다.
 *  2) 로그의 물리 주소와 크기를 레지스터에 쓴다.
 *  3) 머리/꼬리를 0 으로 되돌린다. 원 주석이 "수동으로"라고 밝히듯,
 *     하드웨어가 알아서 초기화해 주지 않는다. 남은 값이 있으면 하드웨어가
 *     엉뚱한 위치에서 쓰기 시작한다.
 *  4) 인터럽트와 로그 기록을 켠다.
 *
 * 3번이 4번보다 먼저여야 하는 이유가 분명하다 — 켠 뒤에 머리/꼬리를
 * 만지면 그 사이에 하드웨어가 쓴 항목을 잃는다.
 *
 * memcpy_toio 를 쓰는 이유: 64비트 값을 MMIO 에 통째로 써야 하는데, 일부
 * 플랫폼에서 writeq 가 없거나 분할된다.
 *
 * 호출 체인:
 *   유닛 초기화 → [이 함수] → iommu_feature_enable()
 */
void amd_iommu_enable_ppr_log(struct amd_iommu *iommu)
{
	u64 entry;	/* [한국어] 레지스터에 쓸 주소+크기 워드 */

	if (iommu->ppr_log == NULL)	/* [한국어] 버퍼가 없으면 켤 수 없다 */
		return;	/* [한국어] 할 일이 없다 */

	iommu_feature_enable(iommu, CONTROL_PPR_EN);	/* [한국어] 먼저 PPR 기능 자체를 켠다 */

	entry = iommu_virt_to_phys(iommu->ppr_log) | PPR_LOG_SIZE_512;	/* [한국어] 물리 주소에 크기 인코딩을 합친다. SME 비트도 함께 붙는다 */

	memcpy_toio(iommu->mmio_base + MMIO_PPR_LOG_OFFSET,	/* [한국어] 64비트를 MMIO 에 통째로 쓴다. 일부 플랫폼에서 writeq 가 분할될 수 있어서 */
		    &entry, sizeof(entry));	/* [한국어] 주소와 크기를 하드웨어에 알린다 */

	/* set head and tail to zero manually */
	writel(0x00, iommu->mmio_base + MMIO_PPR_HEAD_OFFSET);	/* [한국어] 머리를 0 으로 (원 주석: 수동으로 초기화). 하드웨어가 해 주지 않는다 */
	writel(0x00, iommu->mmio_base + MMIO_PPR_TAIL_OFFSET);	/* [한국어] 꼬리도 0 으로. 켜기 전에 해야 그 사이 기록을 잃지 않는다 */

	iommu_feature_enable(iommu, CONTROL_PPRINT_EN);	/* [한국어] 요청이 쌓이면 인터럽트를 내게 한다 */
	iommu_feature_enable(iommu, CONTROL_PPRLOG_EN);	/* [한국어] 마지막으로 기록을 켠다. 이 순간부터 장치의 폴트가 쌓이기 시작한다 */
}

/*
 * [한국어]
 * amd_iommu_free_ppr_log - 로그 버퍼를 놓는다
 *
 * @iommu: 대상 유닛.
 *
 * 초기화 실패 경로에서만 불린다(__init). 정상 동작 중에는 로그를 해제하지
 * 않는다 — 해제하면 하드웨어가 없는 메모리에 요청을 쓰게 된다.
 *
 * NULL 검사가 없는 이유: iommu_free_pages 가 NULL 을 받아들인다.
 *
 * 호출 체인:
 *   유닛 초기화 실패 경로 → [이 함수]
 */
void __init amd_iommu_free_ppr_log(struct amd_iommu *iommu)
{
	iommu_free_pages(iommu->ppr_log);	/* [한국어] 초기화 실패 경로 전용. 동작 중에 놓으면 하드웨어가 없는 메모리에 쓴다 */
}

/*
 * This function restarts ppr logging in case the IOMMU experienced
 * PPR log overflow.
 */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_restart_ppr_log - 넘친 로그를 되살린다
 *
 * @iommu: 대상 유닛.
 *
 * 로그가 넘쳤다는 것은 그 사이의 페이지 요청을 잃어버렸다는 뜻이고, 잃은
 * 요청은 응답받지 못하므로 그 장치들이 멈춘다. 되살리는 것으로 이미 잃은
 * 요청을 회복할 수는 없지만, 최소한 이후의 요청은 다시 받을 수 있다.
 *
 * 세 로그(이벤트/PPR/GA)가 같은 절차를 쓰므로 공통 함수에 인자만 달리해
 * 넘긴다 — 어떤 제어 비트를 끄고 켤지, 어떤 상태 비트를 봐야 하는지.
 *
 * 호출 체인:
 *   인터럽트 핸들러가 오버플로 비트를 발견 → [이 함수] → amd_iommu_restart_log()
 */
void amd_iommu_restart_ppr_log(struct amd_iommu *iommu)
{
	amd_iommu_restart_log(iommu, "PPR", CONTROL_PPRINT_EN,	/* [한국어] 세 로그가 같은 절차를 쓰므로 어떤 비트를 다룰지만 인자로 넘긴다 */
			      CONTROL_PPRLOG_EN, MMIO_STATUS_PPR_RUN_MASK,	/* [한국어] 끄고 켤 제어 비트와 확인할 상태 비트 */
			      MMIO_STATUS_PPR_OVERFLOW_MASK);	/* [한국어] 오버플로 표시를 지울 비트 */
}

/*
 * [한국어]
 * ppr_flag_to_fault_perm - PPR 플래그를 코어의 폴트 권한 표현으로 옮긴다
 *
 * @flag: PPR 항목의 플래그 필드.
 * @return: IOMMU_FAULT_PERM_* 조합.
 *
 * 하드웨어 형식과 커널 공용 형식 사이의 번역이다. 앞의 셋(읽기/쓰기/실행)은
 * 그대로 대응하지만, 마지막 하나는 뒤집힌다.
 *
 * PPR_FLAG_US 는 "사용자 권한 접근"을 뜻한다. 그것이 없으면 커널 권한의
 * 접근이라는 뜻이므로, PERM_PRIV 를 세운다. 이 반전이 필요한 이유는 두
 * 형식이 서로 반대 기준을 택했기 때문이다 — 하나는 사용자를 표시하고
 * 다른 하나는 특권을 표시한다.
 *
 * 호출 체인:
 *   iommu_call_iopf_notifier() → [이 함수]
 */
static inline u32 ppr_flag_to_fault_perm(u16 flag)
{
	int perm = 0;	/* [한국어] 코어 형식의 권한 조합 */

	if (flag & PPR_FLAG_READ)	/* [한국어] 읽기를 요청했는가 */
		perm |= IOMMU_FAULT_PERM_READ;	/* [한국어] 그대로 대응 */
	if (flag & PPR_FLAG_WRITE)	/* [한국어] 쓰기를 요청했는가 */
		perm |= IOMMU_FAULT_PERM_WRITE;	/* [한국어] 그대로 대응 */
	if (flag & PPR_FLAG_EXEC)	/* [한국어] 실행을 요청했는가 */
		perm |= IOMMU_FAULT_PERM_EXEC;	/* [한국어] 그대로 대응 */
	if (!(flag & PPR_FLAG_US))	/* [한국어] US 가 없으면 커널 권한의 접근이라는 뜻 — 여기만 뒤집힌다 */
		perm |= IOMMU_FAULT_PERM_PRIV;	/* [한국어] 두 형식이 서로 반대 기준(사용자 표시 vs 특권 표시)을 택했기 때문이다 */

	return perm;	/* [한국어] 코어가 아는 형식으로 */
}

/*
 * [한국어]
 * ppr_is_valid - 이 페이지 요청을 처리할 수 있는지 판별한다
 *
 * @iommu: 요청이 온 유닛.
 * @raw: 로그 항목 두 워드.
 * @return: 처리 가능하면 참.
 *
 * 두 가지를 걸러 낸다.
 *
 * GN=0: PASID 가 유효하지 않다는 뜻이다. 그러면 이 요청이 어느 주소 공간의
 * 것인지 알 수 없어 어느 페이지를 가져와야 할지 정할 수 없다. 처리할 방법이
 * 없으므로 거절한다.
 *
 * RVSD: 예약 비트가 0 이 아니었다. 장치가 스펙에 맞지 않는 요청을 낸
 * 것이며, 해석하면 위험하다.
 *
 * 두 경우 모두 dev_dbg 로만 남기는 이유: 고장난 장치는 이런 요청을 초당
 * 수천 개 낼 수 있어, 기본 로그 레벨로 찍으면 시스템이 멈춘다.
 *
 * 거짓을 돌려주면 호출자가 거절 응답을 보낸다 — 조용히 버리지 않는 것이
 * 이 파일 전체의 규칙이다.
 *
 * 호출 체인:
 *   iommu_call_iopf_notifier() → [이 함수]
 */
static bool ppr_is_valid(struct amd_iommu *iommu, u64 *raw)
{
	struct device *dev = iommu->iommu.dev;	/* [한국어] 로그 메시지에 쓸 장치 */
	u16 devid = PPR_DEVID(raw[0]);	/* [한국어] 요청을 낸 장치의 id */

	if (!(PPR_FLAGS(raw[0]) & PPR_FLAG_GN)) {	/* [한국어] GN 이 없으면 PASID 가 유효하지 않다 */
		dev_dbg(dev, "PPR logged [Request ignored due to GN=0 (device=%04x:%02x:%02x.%x "	/* [한국어] 어느 주소 공간의 요청인지 알 수 없어 처리할 방법이 없다 */
			"pasid=0x%05llx address=0x%llx flags=0x%04llx tag=0x%03llx]\n",
			iommu->pci_seg->id, PCI_BUS_NUM(devid), PCI_SLOT(devid), PCI_FUNC(devid),
			PPR_PASID(raw[0]), raw[1], PPR_FLAGS(raw[0]), PPR_TAG(raw[0]));
		return false;	/* [한국어] 호출자가 거절 응답을 보낸다 */
	}

	if (PPR_FLAGS(raw[0]) & PPR_FLAG_RVSD) {	/* [한국어] 예약 비트가 0 이 아니다 */
		dev_dbg(dev, "PPR logged [Invalid request format (device=%04x:%02x:%02x.%x "	/* [한국어] 스펙에 맞지 않는 요청이라 해석하면 위험하다 */
			"pasid=0x%05llx address=0x%llx flags=0x%04llx tag=0x%03llx]\n",
			iommu->pci_seg->id, PCI_BUS_NUM(devid), PCI_SLOT(devid), PCI_FUNC(devid),
			PPR_PASID(raw[0]), raw[1], PPR_FLAGS(raw[0]), PPR_TAG(raw[0]));
		return false;	/* [한국어] 거절 */
	}

	return true;	/* [한국어] 처리할 수 있는 요청 */
}

/*
 * [한국어]
 * iommu_call_iopf_notifier - 요청 하나를 커널의 폴트 처리 계층에 올린다
 *
 * @iommu: 요청이 온 유닛.
 * @raw: 로그 항목 두 워드.
 *
 * 하드웨어 형식의 요청을 코어가 아는 struct iopf_fault 로 옮겨 담아
 * iommu_report_device_fault 로 넘기는 것이 전부다. 다만 그 앞에 검증이
 * 여럿 있고, 검증에 걸리면 반드시 거절 응답을 보낸다 — out 레이블이
 * "아무도 받지 않았으니 실패로 답한다"인 이유다.
 *
 * 검증 순서에 뜻이 있다.
 *  - 요청 종류가 페이지 폴트가 아니면: 응답할 대상조차 특정할 수 없어
 *    로그만 남기고 돌아간다(장치를 못 찾은 것과 같은 상황).
 *  - 장치를 찾지 못하면: 응답을 보낼 곳이 없다. 이미 사라진 장치의
 *    뒤늦은 요청이다.
 *  - 형식이 잘못됐거나 PASID 가 범위를 벗어나면: 거절 응답을 보낸다.
 *
 * grpid 를 태그의 하위 9비트로 잡는 이유: PPR 태그는 10비트이고 그중
 * 최상위가 "그룹의 마지막 요청"을 나타내는 플래그다. 나머지 9비트가
 * 그룹 번호가 된다.
 *
 * NEEDS_PASID 플래그를 항상 세우는 이유: AMD 의 응답 명령은 PASID 를
 * 함께 실어야 장치가 짝을 찾을 수 있다.
 *
 * 실행 컨텍스트: 인터럽트 스레드. 여기서 페이지를 가져오지 않고 큐에
 * 넣기만 하는 것이 그 때문이다.
 *
 * 호출 체인:
 *   amd_iommu_poll_ppr_log() → [이 함수] → iommu_report_device_fault()
 *     또는 amd_iommu_complete_ppr() (거절)
 */
static void iommu_call_iopf_notifier(struct amd_iommu *iommu, u64 *raw)
{
	struct iommu_dev_data *dev_data;	/* [한국어] PASID 범위 확인에 쓸 장치 상태 */
	struct iopf_fault event;	/* [한국어] 코어에 넘길 폴트 서술 */
	struct pci_dev *pdev;	/* [한국어] 요청을 낸 장치 */
	u16 devid = PPR_DEVID(raw[0]);	/* [한국어] 그 장치의 id */

	if (PPR_REQ_TYPE(raw[0]) != PPR_REQ_FAULT) {	/* [한국어] 페이지 폴트가 아닌 종류 */
		pr_info_ratelimited("Unknown PPR request received\n");	/* [한국어] 응답할 대상조차 특정할 수 없어 로그만 남긴다 */
		return;	/* [한국어] 돌아간다 */
	}

	pdev = pci_get_domain_bus_and_slot(iommu->pci_seg->id,	/* [한국어] id 로 실제 PCI 장치를 찾는다 */
					   PCI_BUS_NUM(devid), devid & 0xff);	/* [한국어] 세그먼트+버스+devfn 으로 */
	if (!pdev)	/* [한국어] 이미 사라진 장치의 뒤늦은 요청 */
		return;	/* [한국어] 응답을 보낼 곳이 없다 */

	if (!ppr_is_valid(iommu, raw))	/* [한국어] 처리할 수 있는 요청인가 */
		goto out;	/* [한국어] 아니면 거절 응답을 보낸다 */

	memset(&event, 0, sizeof(struct iopf_fault));	/* [한국어] 명시하지 않는 필드를 0 으로 */

	event.fault.type = IOMMU_FAULT_PAGE_REQ;	/* [한국어] 페이지 요청 종류 */
	event.fault.prm.perm = ppr_flag_to_fault_perm(PPR_FLAGS(raw[0]));	/* [한국어] 요청한 권한을 코어 형식으로 */
	event.fault.prm.addr = (u64)(raw[1] & PAGE_MASK);	/* [한국어] 폴트 주소. 페이지 오프셋은 뗀다 */
	event.fault.prm.pasid = PPR_PASID(raw[0]);	/* [한국어] 두 조각으로 나뉜 PASID 를 합쳐 복원한 값 */
	event.fault.prm.grpid = PPR_TAG(raw[0]) & 0x1FF;	/* [한국어] 태그 10비트 중 하위 9비트가 그룹 번호. 최상위는 아래에서 별도로 본다 */

	/*
	 * PASID zero is used for requests from the I/O device without
	 * a PASID
	 */
	dev_data = dev_iommu_priv_get(&pdev->dev);	/* [한국어] (원 주석: PASID 0 은 PASID 없는 요청에 쓰인다) */
	if (event.fault.prm.pasid == 0 ||	/* [한국어] 예약값이거나 */
	    event.fault.prm.pasid >= dev_data->max_pasids) {	/* [한국어] 장치가 광고한 범위를 벗어났다 */
		pr_info_ratelimited("Invalid PASID : 0x%x, device : 0x%x\n",	/* [한국어] 고장난 장치가 로그를 채우지 않도록 속도를 제한한다 */
				    event.fault.prm.pasid, pdev->dev.id);
		goto out;	/* [한국어] 거절 응답 */
	}

	event.fault.prm.flags |= IOMMU_FAULT_PAGE_RESPONSE_NEEDS_PASID;	/* [한국어] AMD 의 응답 명령은 PASID 를 함께 실어야 장치가 짝을 찾는다 */
	event.fault.prm.flags |= IOMMU_FAULT_PAGE_REQUEST_PASID_VALID;	/* [한국어] PASID 필드가 의미 있음을 코어에 알린다 */
	if (PPR_TAG(raw[0]) & 0x200)	/* [한국어] 태그의 최상위 비트 = 그룹의 마지막 요청 */
		event.fault.prm.flags |= IOMMU_FAULT_PAGE_REQUEST_LAST_PAGE;	/* [한국어] 응답은 그룹의 마지막 요청에만 보내면 된다 */

	/* Submit event */
	iommu_report_device_fault(&pdev->dev, &event);	/* [한국어] 큐에 넣는다. 실제 페이지 처리는 잠들 수 있는 워커가 한다 (원 주석: 이벤트 제출) */

	return;	/* [한국어] 정상 경로 끝 */

out:	/* [한국어] 검증에 걸린 모든 경로가 여기로 모여 거절 응답을 보낸다 */
	/* Nobody cared, abort */
	amd_iommu_complete_ppr(&pdev->dev, PPR_PASID(raw[0]),	/* [한국어] (원 주석: 아무도 받지 않았으니 중단) — 조용히 버리지 않고 반드시 답한다 */
			       IOMMU_PAGE_RESP_FAILURE,	/* [한국어] 실패로 답해야 장치가 멈춘 요청을 포기하고 진행한다 */
			       PPR_TAG(raw[0]) & 0x1FF);	/* [한국어] 같은 그룹 번호로 짝을 맞춘다 */
}

/*
 * [한국어]
 * amd_iommu_poll_ppr_log - 쌓인 페이지 요청을 모두 꺼내 처리로 넘긴다
 *
 * @iommu: 대상 유닛.
 *
 * 링 버퍼의 소비자다. 머리와 꼬리가 같아질 때까지 항목을 하나씩 꺼낸다.
 * 그 안에 하드웨어 결함 두 가지에 대한 대응이 들어 있어 코드가 단순하지 않다.
 *
 * 첫째, 원 주석이 밝히는 경쟁: 인터럽트가 항목이 메모리에 쓰이기 전에
 * 도착할 수 있다. 그래서 요청 종류가 0(=아직 안 쓰임)인 동안 짧게 기다린다.
 * 버퍼를 0 으로 초기화해 두는 것이 이 판별의 전제다.
 *
 * 둘째, errata 733: 항목을 읽은 뒤 0 으로 되돌려 놓아야 한다. 다만 SNP 를
 * 켠 시스템에서는 이 결함이 없고 버퍼가 쓰기 불가라, 그 경우에는 건너뛴다.
 *
 * 항목을 지역 변수로 복사한 뒤 처리하는 것도 의도적이다. 머리 포인터를
 * 먼저 진행시켜 하드웨어가 그 자리를 재사용할 수 있게 하고, 그다음에
 * 복사본으로 처리한다 — 처리가 오래 걸려도 링이 막히지 않는다.
 *
 * 실행 컨텍스트: 인터럽트 스레드.
 *
 * 호출 체인:
 *   amd_iommu_int_thread_pprlog() → [이 함수] → iommu_call_iopf_notifier()
 */
void amd_iommu_poll_ppr_log(struct amd_iommu *iommu)
{
	u32 head, tail;	/* [한국어] 링의 소비 지점과 생산 지점 */

	if (iommu->ppr_log == NULL)	/* [한국어] PPR 을 쓰지 않는 유닛 */
		return;	/* [한국어] 할 일이 없다 */

	head = readl(iommu->mmio_base + MMIO_PPR_HEAD_OFFSET);	/* [한국어] 우리가 어디까지 읽었는지 */
	tail = readl(iommu->mmio_base + MMIO_PPR_TAIL_OFFSET);	/* [한국어] 하드웨어가 어디까지 썼는지 */

	while (head != tail) {	/* [한국어] 둘이 같아질 때까지 = 링을 다 비울 때까지 */
		volatile u64 *raw;	/* [한국어] 하드웨어가 쓰는 메모리라 volatile — 컴파일러가 읽기를 최적화로 없애면 안 된다 */
		u64 entry[2];	/* [한국어] 지역 복사본 */
		int i;	/* [한국어] 대기 루프 카운터 */

		raw = (u64 *)(iommu->ppr_log + head);	/* [한국어] 현재 항목의 위치 */

		/*
		 * Hardware bug: Interrupt may arrive before the entry is
		 * written to memory. If this happens we need to wait for the
		 * entry to arrive.
		 */
		for (i = 0; i < LOOP_TIMEOUT; ++i) {	/* [한국어] (원 주석: 인터럽트가 항목이 메모리에 쓰이기 전에 올 수 있다) */
			if (PPR_REQ_TYPE(raw[0]) != 0)	/* [한국어] 종류가 0 이 아니면 도착한 것이다 — 버퍼를 0 으로 초기화해 두는 이유 */
				break;	/* [한국어] 도착했다 */
			udelay(1);	/* [한국어] 짧게 기다린다. 상한이 있어 영원히 돌지 않는다 */
		}

		/* Avoid memcpy function-call overhead */
		entry[0] = raw[0];	/* [한국어] (원 주석: memcpy 호출 오버헤드를 피한다) */
		entry[1] = raw[1];	/* [한국어] 두 워드를 지역 변수로 복사 */

		/*
		 * To detect the hardware errata 733 we need to clear the
		 * entry back to zero. This issue does not exist on SNP
		 * enabled system. Also this buffer is not writeable on
		 * SNP enabled system.
		 */
		if (!amd_iommu_snp_en)	/* [한국어] (원 주석: errata 733 감지를 위해 0 으로 되돌린다. SNP 시스템에는 이 결함이 없고 버퍼가 쓰기 불가다) */
			raw[0] = raw[1] = 0UL;	/* [한국어] 다음에 이 자리가 "아직 안 쓰임"으로 보이게 한다 */

		/* Update head pointer of hardware ring-buffer */
		head = (head + PPR_ENTRY_SIZE) % PPR_LOG_SIZE;	/* [한국어] 다음 항목으로. 링이라 끝에서 되돌아온다 */
		writel(head, iommu->mmio_base + MMIO_PPR_HEAD_OFFSET);	/* [한국어] 머리를 먼저 진행시켜 하드웨어가 그 자리를 재사용할 수 있게 한다 (원 주석) */

		/* Handle PPR entry */
		iommu_call_iopf_notifier(iommu, entry);	/* [한국어] 그다음 복사본으로 처리한다. 처리가 오래 걸려도 링이 막히지 않는다 */
	}
}

/**************************************************************
 *
 * IOPF handling stuff
 */

/* Setup per-IOMMU IOPF queue if not exist. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_iopf_init - 유닛별 페이지 폴트 처리 큐를 만든다
 *
 * @iommu: 대상 유닛.
 * @return: 0 성공(이미 있으면 그대로 0), -ENOMEM 이면 실패.
 *
 * 왜 큐가 필요한가: 폴트를 처리하려면 페이지를 실제로 가져와야 하고 그것은
 * 잠들 수 있는 일인데, 로그를 읽는 쪽은 인터럽트 문맥이라 잠들 수 없다.
 * 큐가 그 경계를 잇는다.
 *
 * 이름에 세그먼트+BDF 를 넣는 이유: 워커 스레드 이름으로 보이므로, 여러
 * 유닛이 있는 시스템에서 어느 유닛의 큐인지 구별할 수 있어야 한다.
 *
 * 이미 있으면 그냥 성공을 돌려준다 — 장치가 붙을 때마다 불릴 수 있다.
 *
 * 호출 체인:
 *   장치 probe → [이 함수] → iopf_queue_alloc()
 */
int amd_iommu_iopf_init(struct amd_iommu *iommu)
{
	int ret = 0;	/* [한국어] 기본은 성공 */

	if (iommu->iopf_queue)	/* [한국어] 장치가 붙을 때마다 불릴 수 있다 */
		return ret;	/* [한국어] 이미 있으면 그대로 성공 */

	snprintf(iommu->iopfq_name, sizeof(iommu->iopfq_name), "amdvi-%#x",	/* [한국어] 워커 스레드 이름이 되므로 유닛을 구별할 수 있어야 한다 */
		 PCI_SEG_DEVID_TO_SBDF(iommu->pci_seg->id, iommu->devid));	/* [한국어] 세그먼트+BDF 로 유일한 이름을 만든다 */

	iommu->iopf_queue = iopf_queue_alloc(iommu->iopfq_name);	/* [한국어] 인터럽트 문맥과 잠들 수 있는 문맥을 잇는 큐 */
	if (!iommu->iopf_queue)	/* [한국어] 할당 실패 */
		ret = -ENOMEM;	/* [한국어] 이 유닛의 장치들은 폴트를 처리할 수 없게 된다 */

	return ret;	/* [한국어] 결과 보고 */
}

/* Destroy per-IOMMU IOPF queue if no longer needed. */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_iopf_uninit - 그 큐를 없앤다
 *
 * @iommu: 대상 유닛.
 *
 * iopf_queue_free 가 진행 중인 작업까지 정리해 준다. 포인터를 NULL 로
 * 되돌리는 것이 중요하다 — 그러지 않으면 init 이 "이미 있다"고 착각한다.
 *
 * 호출 체인:
 *   유닛 해제 → [이 함수]
 */
void amd_iommu_iopf_uninit(struct amd_iommu *iommu)
{
	iopf_queue_free(iommu->iopf_queue);	/* [한국어] 진행 중인 작업까지 정리해 준다 */
	iommu->iopf_queue = NULL;	/* [한국어] init 이 "이미 있다"고 착각하지 않게 */
}

/*
 * [한국어]
 * amd_iommu_page_response - 폴트 처리 결과를 장치에 되돌린다 (코어 콜백)
 *
 * @dev: 요청을 냈던 장치.
 * @evt: 원래의 폴트 이벤트(여기서는 쓰지 않는다).
 * @resp: 처리 결과 — 성공인지 실패인지, 어느 PASID/그룹의 답인지.
 *
 * IOPF 워커가 페이지를 가져왔거나 실패했을 때 코어가 부른다. 이 응답이
 * 장치에 도달해야 장치가 멈춰 있던 요청을 재개한다 — 응답하지 않으면
 * 그 장치는 영원히 멈춘 채로 남는다.
 *
 * grpid 로 짝을 찾는다. 한 그룹의 여러 요청에 대해 마지막 하나에만
 * 응답하면 되고, 그 규칙은 코어가 관리한다.
 *
 * 호출 체인:
 *   IOPF 워커 → 코어 → [이 함수] → amd_iommu_complete_ppr()
 */
void amd_iommu_page_response(struct device *dev, struct iopf_fault *evt,
			     struct iommu_page_response *resp)
{
	amd_iommu_complete_ppr(dev, resp->pasid, resp->code, resp->grpid);	/* [한국어] 처리 결과를 명령으로 만들어 장치에 보낸다. 이것이 도달해야 장치가 재개한다 */
}

/*
 * [한국어]
 * amd_iommu_iopf_add_device - 장치를 폴트 처리 큐에 등록한다
 *
 * @iommu: 그 장치를 담당하는 유닛.
 * @dev_data: 장치의 IOMMU 상태.
 * @return: 0 성공(PRI 를 안 쓰면 그대로 0), 음수면 실패.
 *
 * PRI 를 켜지 않은 장치는 페이지 폴트를 낼 수 없으므로 등록할 이유가 없다.
 * 그래서 조용히 성공을 돌려준다 — 오류가 아니라 해당 없음이다.
 *
 * 큐가 없으면 -EINVAL 인 이유: PRI 를 켠 장치인데 처리 경로가 없다는 것은
 * 초기화 순서가 잘못됐다는 뜻이고, 그대로 두면 그 장치의 첫 폴트에서
 * 멈춘다.
 *
 * dev_data->ppr 을 마지막에 세우는 이유: 등록이 성공한 뒤에야 "이 장치는
 * 폴트를 낼 수 있다"가 참이 된다. 먼저 세우면 해제 경로가 등록되지 않은
 * 장치를 빼려 한다.
 *
 * 호출 체인:
 *   장치를 도메인에 붙일 때 → [이 함수] → iopf_queue_add_device()
 */
int amd_iommu_iopf_add_device(struct amd_iommu *iommu,
			      struct iommu_dev_data *dev_data)
{
	int ret = 0;	/* [한국어] 기본은 성공 */

	if (!dev_data->pri_enabled)	/* [한국어] PRI 를 켜지 않은 장치는 폴트를 낼 수 없다 */
		return ret;	/* [한국어] 오류가 아니라 해당 없음 */

	if (!iommu->iopf_queue)	/* [한국어] PRI 를 켰는데 처리 경로가 없다 — 초기화 순서가 잘못됐다 */
		return -EINVAL;	/* [한국어] 그대로 두면 첫 폴트에서 멈춘다 */

	ret = iopf_queue_add_device(iommu->iopf_queue, dev_data->dev);	/* [한국어] 이 장치의 폴트를 받을 준비 */
	if (ret)	/* [한국어] 등록 실패 */
		return ret;	/* [한국어] 상태를 바꾸지 않고 돌아간다 */

	dev_data->ppr = true;	/* [한국어] 등록이 성공한 뒤에야 참이 된다 — 먼저 세우면 해제가 없는 등록을 빼려 한다 */
	return 0;	/* [한국어] 이제 이 장치는 폴트를 낼 수 있다 */
}

/* Its assumed that caller has verified that device was added to iopf queue */
/*
 * [한국어]
 * (위 영어 주석에 이어)
 * amd_iommu_iopf_remove_device - 장치를 폴트 처리 큐에서 뺀다
 *
 * @iommu: 담당 유닛.
 * @dev_data: 장치의 IOMMU 상태.
 *
 * 원 주석이 밝히듯 "이 장치가 큐에 등록되어 있다"는 것을 호출자가 이미
 * 확인했다고 가정한다. 그래서 dev_data->ppr 검사가 없다.
 *
 * iopf_queue_remove_device 는 그 장치의 대기 중인 폴트를 모두 정리하고
 * 각각에 실패 응답을 보낸다 — 응답 없이 사라지는 요청이 없도록.
 *
 * 호출 체인:
 *   장치를 도메인에서 뗄 때 → [이 함수] → iopf_queue_remove_device()
 */
void amd_iommu_iopf_remove_device(struct amd_iommu *iommu,
				  struct iommu_dev_data *dev_data)
{
	iopf_queue_remove_device(iommu->iopf_queue, dev_data->dev);	/* [한국어] 대기 중인 폴트를 모두 정리하고 각각에 실패 응답을 보낸다 — 답 없이 사라지는 요청이 없도록 */
	dev_data->ppr = false;	/* [한국어] 더 이상 폴트를 받지 않는다 */
}
