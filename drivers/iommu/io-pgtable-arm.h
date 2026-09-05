/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * [한국어 설명] ARM LPAE 의 TCR 레지스터 필드 인코딩 (drivers/iommu/io-pgtable-arm.h)
 *
 * === 파일의 역할 ===
 * TCR/VTCR 레지스터의 필드 값을 정의한 상수 모음이다. io-pgtable-arm.c 가 이 값을
 * 채워 cfg 에 담아 주면, SMMU 드라이버가 그것을 하드웨어 레지스터에 쓴다. 두 파일이
 * 같은 인코딩을 알아야 하므로 헤더로 분리되어 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * io-pgtable-arm.c (값을 채운다) → io_pgtable_cfg → arm-smmu / arm-smmu-v3 (레지스터에 쓴다)
 *
 * === 타 모듈과의 연결 ===
 * - io-pgtable-arm.c: 입도·주소 폭·캐시 정책을 이 인코딩으로 옮긴다.
 * - arm-smmu, arm-smmu-v3: 채워진 값을 컨텍스트 뱅크나 스트림 테이블 항목에 쓴다.
 *
 * === 주요 함수/구조체 요약 ===
 * 함수는 없다. 세 종류의 인코딩만 있다 —
 *  - TG0/TG1: 페이지 입도. TTBR0 와 TTBR1 이 같은 입도에 다른 값을 쓴다.
 *  - SH/RGN : 테이블 접근의 공유 도메인과 캐시 정책.
 *  - PS     : 물리 주소 폭.
 */
#ifndef IO_PGTABLE_ARM_H_	/* [한국어] 중복 포함 방지 */
#define IO_PGTABLE_ARM_H_

#define ARM_LPAE_TCR_TG0_4K		0	/* [한국어] TTBR0 쪽 입도 인코딩. 4K/64K/16K 순서가 뒤섞여 있는 것은 아키텍처가 그렇게 정의했기 때문이며, 하위 호환을 위해 나중에 추가된 16K 가 뒤로 밀렸다 */
#define ARM_LPAE_TCR_TG0_64K		1	/* [한국어] 64K 입도 */
#define ARM_LPAE_TCR_TG0_16K		2	/* [한국어] 16K 입도 */

#define ARM_LPAE_TCR_TG1_16K		1	/* [한국어] TTBR1 쪽은 값 배치가 또 다르다. 같은 입도에 다른 숫자를 쓰므로, 코드가 TTBR0/1 을 구분해 인코딩해야 한다 */
#define ARM_LPAE_TCR_TG1_4K		2	/* [한국어] 4K */
#define ARM_LPAE_TCR_TG1_64K		3	/* [한국어] 64K */

#define ARM_LPAE_TCR_SH_NS		0	/* [한국어] 공유 없음 — 테이블 접근이 캐시 일관성 프로토콜에 참여하지 않는다 */
#define ARM_LPAE_TCR_SH_OS		2	/* [한국어] 외부 공유 */
#define ARM_LPAE_TCR_SH_IS		3	/* [한국어] 내부 공유. 일관성 있는 IOMMU 가 쓰는 값이다 */

#define ARM_LPAE_TCR_RGN_NC		0	/* [한국어] 비캐시. 비일관 IOMMU 가 테이블을 이 속성으로 읽으므로, 소프트웨어가 PTE 를 쓸 때마다 캐시를 밀어내야 한다 */
#define ARM_LPAE_TCR_RGN_WBWA		1	/* [한국어] Write-Back, 읽기·쓰기 할당. 일관성 있는 구성의 기본값 */
#define ARM_LPAE_TCR_RGN_WT		2	/* [한국어] Write-Through */
#define ARM_LPAE_TCR_RGN_WB		3	/* [한국어] Write-Back, 할당 없음 */

#define ARM_LPAE_TCR_PS_32_BIT		0x0ULL	/* [한국어] 물리 주소 폭 인코딩. 4GB */
#define ARM_LPAE_TCR_PS_36_BIT		0x1ULL	/* [한국어] 64GB */
#define ARM_LPAE_TCR_PS_40_BIT		0x2ULL	/* [한국어] 1TB */
#define ARM_LPAE_TCR_PS_42_BIT		0x3ULL	/* [한국어] 4TB */
#define ARM_LPAE_TCR_PS_44_BIT		0x4ULL	/* [한국어] 16TB */
#define ARM_LPAE_TCR_PS_48_BIT		0x5ULL	/* [한국어] 256TB — 대부분의 시스템이 여기까지다 */
#define ARM_LPAE_TCR_PS_52_BIT		0x6ULL	/* [한국어] 4PB. 64K 입도에서만 쓸 수 있는데, 서술자의 남는 하위 비트에 상위 주소를 접어 넣는 방식이라 그 입도에서만 자리가 생기기 때문이다 */

#endif /* IO_PGTABLE_ARM_H_ */
