// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple DART (Device Address Resolution Table) IOMMU driver
 *
 * Copyright (C) 2021 The Asahi Linux Contributors
 *
 * Based on arm/arm-smmu/arm-ssmu.c and arm/arm-smmu-v3/arm-smmu-v3.c
 *  Copyright (C) 2013 ARM Limited
 *  Copyright (C) 2015 ARM Limited
 * and on exynos-iommu.c
 *  Copyright (c) 2011,2016 Samsung Electronics Co., Ltd.
 */

/*
 * [한국어 설명] Apple DART IOMMU 드라이버 (apple-dart.c)
 *
 * === 파일의 역할 ===
 * Apple Silicon(M1/M2 계열, 그리고 그 이전 iOS SoC)에 들어 있는
 * DART(Device Address Resolution Table)를 리눅스 IOMMU API에 붙인다.
 * 페이지 테이블 자체를 만드는 일은 io-pgtable의 APPLE_DART/APPLE_DART2
 * 포맷 드라이버가 맡고, 이 파일은 **하드웨어를 그 테이블에 연결하고
 * 스트림 단위로 격리를 관리한다.**
 *
 * DART를 이해하려면 개념 다섯을 잡아야 한다.
 *
 * (1) **스트림(SID)이 격리의 단위다.** 각 DART는 최대 256개의
 *     스트림을 갖고, 스트림마다 독립된 TCR(변환 제어) 레지스터와
 *     TTBR 묶음을 갖는다. 그래서 이 드라이버의 거의 모든 하드웨어
 *     조작이 "sidmap 비트맵을 훑으며 스트림마다 레지스터를 쓴다"는
 *     형태다.
 *
 * (2) **스트림은 전부 켜 두고 TCR로 제어한다.** 리셋에서
 *     enable_streams 레지스터에 전부 1을 써 버린다. 실제 격리는
 *     스트림별 TCR을 "변환/우회/차단" 중 무엇으로 두느냐로 이뤄진다.
 *     그래서 차단 도메인은 TCR을 0으로 쓰는 것이 전부다.
 *
 * (3) **TTBR이 여러 개일 수 있고, 그것이 사실상 한 단계 더 깊은
 *     테이블 역할을 한다.** T8020/T6000은 스트림마다 TTBR 4개를
 *     갖고, io-pgtable이 최상위 테이블을 4조각으로 나눠 각 TTBR에
 *     하나씩 걸어 준다. T8110은 TTBR 하나에 4단계 워크를 지원한다.
 *
 * (4) **한 디바이스가 최대 3개의 DART 뒤에 있을 수 있다.**
 *     (MAX_DARTS_PER_DEVICE) 그런 디바이스의 DMA는 어느 DART를
 *     지날지 알 수 없으므로, 모든 DART를 **똑같이** 설정해야 한다.
 *     stream_maps 배열이 그 목록이고, for_each_stream_map 매크로가
 *     그것을 훑는 관용구다.
 *
 * (5) **도메인 완성이 미뤄진다.** domain_alloc_paging은 디바이스
 *     없이도 불릴 수 있는데, 그러면 페이지 크기도 주소 폭도 모른다.
 *     그래서 실제 io-pgtable 생성은 attach 시점의
 *     apple_dart_finalize_domain()에서 init_lock 아래 한 번만
 *     일어난다.
 *
 * === 전체 아키텍처에서의 위치 ===
 *
 *   [디바이스 트리] iommus = <&dart 스트림번호>
 *        ↓ of_xlate
 *   [이 파일] master_cfg에 (DART, SID) 쌍을 쌓는다 — 최대 3쌍
 *        ↓ device_group
 *   [이 파일] 같은 스트림을 공유하는 디바이스를 한 그룹으로 묶는다
 *        ↓ attach_dev
 *   [이 파일] finalize_domain → io-pgtable 생성 → TTBR/TCR 설정
 *
 *   [디바이스 드라이버] dma_map_*()
 *        ↓
 *   [dma-iommu] IOVA 할당
 *        ↓
 *   [이 파일] map_pages → 그대로 io-pgtable에 위임
 *        ↓ iotlb_sync_map
 *   [이 파일] 도메인에 붙은 모든 스트림의 TLB를 비운다
 *        ↑ 폴트
 *   [이 파일] 세대별 irq 핸들러가 오류 코드를 이름으로 옮겨 찍는다
 *
 * 실행 컨텍스트: map/unmap은 io-pgtable에 그대로 넘긴다. 하드웨어
 * 명령(TLB 무효화)은 dart->lock(irqsave) 아래에서 폴링하므로
 * atomic 문맥에서도 안전하다. 도메인 완성만 mutex를 쓴다.
 *
 * === 타 모듈과의 연결 ===
 * - io-pgtable.h와 io-pgtable-dart.c: 실제 페이지 테이블. 이 파일은
 *   cfg를 채워 alloc_io_pgtable_ops를 부르고, 돌아온 ttbr 배열을
 *   레지스터에 옮겨 심는 일만 한다.
 * - dma-iommu.h: MSI 도어벨 예약 영역(Apple PCIe의 고정 주소).
 * - linux/bitfield.h: 레지스터 필드 추출/조립(FIELD_GET/FIELD_PREP).
 * - linux/iopoll.h: 명령 완료를 기다리는 원자적 폴링.
 * 데이터 흐름: 디바이스 트리의 (DART, SID) → master_cfg → 도메인의
 * stream_maps(원자적) → TCR/TTBR 레지스터 → 하드웨어 변환.
 *
 * === 주요 함수/구조체 요약 ===
 * - struct apple_dart_hw: 세대별 레지스터 오프셋과 비트 위치를 모은
 *   표. T8020/T6000/T8110의 차이가 전부 이 구조체로 흡수된다.
 * - struct apple_dart: DART 하드웨어 하나. 스트림 수, 주소 폭,
 *   그리고 절전 복원용 TCR/TTBR 사본.
 * - struct apple_dart_domain: 도메인 하나. io-pgtable 핸들과,
 *   붙어 있는 (DART, 스트림 비트맵) 쌍들.
 * - apple_dart_finalize_domain(): 도메인을 실제로 완성한다 —
 *   페이지 테이블을 만들고 기하 정보를 확정한다.
 * - apple_dart_setup_translation(): TTBR들을 심고 TCR로 변환을 켠 뒤
 *   TLB를 비운다. 하드웨어를 도메인에 연결하는 지점이다.
 * - apple_dart_device_group(): 스트림을 공유하는 디바이스들을 같은
 *   그룹으로 묶는다. sid2group이 그 장부다.
 */

/* [한국어] 도메인의 스트림 비트맵을 락 없이 갱신하기 위한 원자 연산. */
#include <linux/atomic.h>
/* [한국어] FIELD_GET/FIELD_PREP — 레지스터 필드를 다루는 표준 방식. */
#include <linux/bitfield.h>
/* [한국어] DART마다 딸린 클럭들을 켜고 끈다. */
#include <linux/clk.h>
/* [한국어] dev_err 등 디바이스 로그. */
#include <linux/dev_printk.h>
/* [한국어] DMA_BIT_MASK 등 주소 폭 계산. */
#include <linux/dma-mapping.h>
/* [한국어] ERR_PTR/IS_ERR. */
#include <linux/err.h>
/* [한국어] 폴트 인터럽트 등록(IRQF_SHARED). */
#include <linux/interrupt.h>
/* [한국어] 실제 페이지 테이블을 맡기는 계층. 이 드라이버는 테이블을
 * 직접 만들지 않는다. */
#include <linux/io-pgtable.h>
/* [한국어] IOMMU 코어 계약. */
#include <linux/iommu.h>
/* [한국어] readl_poll_timeout_atomic — 명령의 busy 비트가 내려갈
 * 때까지 기다린다. */
#include <linux/iopoll.h>
/* [한국어] 모듈로 빌드된다. */
#include <linux/module.h>
/* [한국어] 디바이스 트리 매치 데이터. */
#include <linux/of.h>
/* [한국어] 주소 변환 도우미. */
#include <linux/of_address.h>
/* [한국어] IOMMU와 디바이스 트리의 연결. */
#include <linux/of_iommu.h>
/* [한국어] of_find_device_by_node — of_xlate가 DART를 찾을 때. */
#include <linux/of_platform.h>
/* [한국어] PCI 디바이스 그룹 판별과 MSI 예약 영역 처리. */
#include <linux/pci.h>
/* [한국어] 플랫폼 드라이버 모델. */
#include <linux/platform_device.h>
/* [한국어] kzalloc/kfree. */
#include <linux/slab.h>
/* [한국어] 바이트 순서 도우미. */
#include <linux/swab.h>
/* [한국어] 기본 타입. */
#include <linux/types.h>

/* [한국어] iommu_dma_get_resv_regions — DMA 계층이 요구하는 예약 영역. */
#include "dma-iommu.h"

/* [한국어] 한 DART가 가질 수 있는 스트림의 최대 개수.
 * 비트맵과 sid2group 배열의 크기를 정한다. 실제 개수는
 * 하드웨어가 알려 주며(num_streams) 이 값을 넘으면 probe가 거부한다. */
#define DART_MAX_STREAMS 256
/* [한국어] 스트림 하나가 가질 수 있는 TTBR의 최대 개수.
 * T8020 계열이 4개, T8110이 1개다. 절전 복원용 배열 크기이기도 하다. */
#define DART_MAX_TTBR 4
/* [한국어] 한 디바이스가 동시에 속할 수 있는 DART의 최대 개수.
 * 원본 주석이 밝히듯 실제로 관측된 최악의 경우가 2개라, 3이면
 * 정적 배열로 충분하다. */
#define MAX_DARTS_PER_DEVICE 3

/* Common registers */

/* [한국어] 세대 공통 파라미터 레지스터 1. 페이지 크기를 알려 준다. */
#define DART_PARAMS1 0x00
/* [한국어] 이 DART가 쓰는 페이지 크기의 지수. 1 << 이 값이 페이지
 * 크기이며, 커널 페이지보다 크면 세밀한 매핑이 불가능해
 * def_domain_type이 통과 모드를 강제한다. */
#define DART_PARAMS1_PAGE_SHIFT GENMASK(27, 24)

/* [한국어] 세대 공통 파라미터 레지스터 2. 우회 지원 여부를 알려 준다. */
#define DART_PARAMS2 0x04
/* [한국어] 이 DART가 우회(bypass) 모드를 지원한다는 표시.
 * 지원하지 않으면 identity 도메인을 만들 수 없어, def_domain_type이
 * DMA 도메인을 강제한다. */
#define DART_PARAMS2_BYPASS_SUPPORT BIT(0)

/* T8020/T6000 registers */

/* [한국어] T8020 계열의 스트림 명령 레지스터.
 * STREAM_SELECT로 대상을 고른 뒤 여기에 명령을 쓴다. */
#define DART_T8020_STREAM_COMMAND 0x20
/* [한국어] 명령이 진행 중임을 뜻하는 비트. 내려갈 때까지 폴링한다. */
#define DART_T8020_STREAM_COMMAND_BUSY BIT(2)
/* [한국어] 선택된 스트림들의 TLB를 무효화하라는 명령. */
#define DART_T8020_STREAM_COMMAND_INVALIDATE BIT(20)

/* [한국어] 명령을 적용할 스트림들의 비트맵을 쓰는 레지스터.
 * 32비트씩 여러 개로 이어져 있어 스트림이 32개를 넘으면
 * 4바이트씩 나눠 쓴다. */
#define DART_T8020_STREAM_SELECT 0x34

/* [한국어] T8020 계열의 오류 상태 레지스터. */
#define DART_T8020_ERROR 0x40
/* [한국어] 오류를 낸 스트림의 번호. */
#define DART_T8020_ERROR_STREAM GENMASK(27, 24)
/* [한국어] 오류의 원인 코드. 아래 비트들 중 하나가 선다. */
#define DART_T8020_ERROR_CODE GENMASK(11, 0)
/* [한국어] 유효한 오류가 기록되어 있다는 표시. 이 비트가 없으면
 * 인터럽트 핸들러가 남의 인터럽트로 보고 물러난다. */
#define DART_T8020_ERROR_FLAG BIT(31)

/* [한국어] 읽기 권한이 없는 곳을 읽으려 했다. */
#define DART_T8020_ERROR_READ_FAULT BIT(4)
/* [한국어] 쓰기 권한이 없는 곳에 쓰려 했다. */
#define DART_T8020_ERROR_WRITE_FAULT BIT(3)
/* [한국어] 최말단 페이지 엔트리가 없다 — 매핑되지 않은 주소. */
#define DART_T8020_ERROR_NO_PTE BIT(2)
/* [한국어] 중간 단계 엔트리가 없다 — 더 큰 영역이 통째로 비어 있다. */
#define DART_T8020_ERROR_NO_PMD BIT(1)
/* [한국어] TTBR 자체가 유효하지 않다 — 이 스트림에 테이블이
 * 걸리지 않았다는 뜻으로, 보통 attach 누락이다. */
#define DART_T8020_ERROR_NO_TTBR BIT(0)

/* [한국어] T8020 계열의 설정 레지스터. 잠금 비트를 담고 있다. */
#define DART_T8020_CONFIG 0x60
/* [한국어] 부트로더가 이 DART를 잠갔다는 표시.
 * 잠기면 재부팅 전까지 TTBR/TCR을 바꿀 수 없어, probe가 실패한다. */
#define DART_T8020_CONFIG_LOCK BIT(15)

/* [한국어] 명령의 busy 비트가 내려가기를 기다리는 최대 시간(마이크로초). */
#define DART_STREAM_COMMAND_BUSY_TIMEOUT 100

/* [한국어] 오류가 난 주소의 상위 32비트. */
#define DART_T8020_ERROR_ADDR_HI 0x54
/* [한국어] 오류가 난 주소의 하위 32비트. 둘을 합쳐 64비트 주소를 만든다. */
#define DART_T8020_ERROR_ADDR_LO 0x50

/* [한국어] 스트림 전체 활성화 레지스터.
 * 리셋에서 전부 1을 써 둔다 — 격리는 스트림 활성화가 아니라
 * 스트림별 TCR로 제어하기 때문이다. */
#define DART_T8020_STREAMS_ENABLE 0xfc

/* [한국어] 스트림별 변환 제어 레지스터의 시작 오프셋.
 * 스트림 번호 × 4가 각 스트림의 자리다. */
#define DART_T8020_TCR                  0x100
/* [한국어] 이 스트림에 대해 변환을 수행하라. */
#define DART_T8020_TCR_TRANSLATE_ENABLE BIT(7)
/* [한국어] 이 스트림의 DART 변환을 건너뛴다(주소를 그대로 통과). */
#define DART_T8020_TCR_BYPASS_DART      BIT(8)
/* [한국어] 접근 권한 필터(DAPF)도 건너뛴다. 우회 모드에서는
 * 둘 다 꺼야 완전한 통과가 된다. */
#define DART_T8020_TCR_BYPASS_DAPF      BIT(12)

/* [한국어] 스트림별 TTBR 배열의 시작 오프셋.
 * (스트림 번호 × TTBR 개수 + 인덱스) × 4가 각 자리다. */
#define DART_T8020_TTBR       0x200
/* [한국어] USB4 전용 DART는 TTBR 배열이 다른 오프셋에 있다.
 * 스트림이 64개로 많아 자리가 옮겨진 것이다. */
#define DART_T8020_USB4_TTBR  0x400
/* [한국어] 이 TTBR이 유효한 테이블을 가리킨다는 표시. */
#define DART_T8020_TTBR_VALID BIT(31)
/* [한국어] 주소 필드가 레지스터 안에서 시작하는 비트 위치.
 * T8020은 0이라 시프트가 필요 없다. */
#define DART_T8020_TTBR_ADDR_FIELD_SHIFT 0
/* [한국어] 물리 주소를 이 비트 수만큼 오른쪽으로 밀어 레지스터에
 * 담는다. 즉 테이블이 4KB 정렬이어야 한다는 뜻이기도 하다. */
#define DART_T8020_TTBR_SHIFT 12

/* T8110 registers */

/* [한국어] T8110의 파라미터 레지스터 3. 주소 폭과 버전을 담는다.
 * T8020과 달리 주소 폭이 하드코딩이 아니라 하드웨어가 알려 준다. */
#define DART_T8110_PARAMS3 0x08
/* [한국어] 출력(물리) 주소의 비트 폭. */
#define DART_T8110_PARAMS3_PA_WIDTH GENMASK(29, 24)
/* [한국어] 입력(IOVA) 주소의 비트 폭. 36을 넘으면 4단계 워크가 필요하다. */
#define DART_T8110_PARAMS3_VA_WIDTH GENMASK(21, 16)
/* [한국어] 하드웨어 버전(주). */
#define DART_T8110_PARAMS3_VER_MAJ GENMASK(15, 8)
/* [한국어] 하드웨어 버전(부). */
#define DART_T8110_PARAMS3_VER_MIN GENMASK(7, 0)

/* [한국어] T8110의 파라미터 레지스터 4. 스트림 수를 알려 준다. */
#define DART_T8110_PARAMS4 0x0c
/* [한국어] 이 DART에 붙을 수 있는 클라이언트의 수. */
#define DART_T8110_PARAMS4_NUM_CLIENTS GENMASK(24, 16)
/* [한국어] 실제 스트림(SID)의 개수. probe가 이 값을 읽어 쓴다. */
#define DART_T8110_PARAMS4_NUM_SIDS GENMASK(8, 0)

/* [한국어] T8110의 TLB 명령 레지스터.
 * T8020과 달리 대상 스트림을 명령 안에 넣으므로 SELECT 레지스터가 없다. */
#define DART_T8110_TLB_CMD              0x80
/* [한국어] 명령 진행 중 표시. 최상위 비트인 점이 T8020과 다르다. */
#define DART_T8110_TLB_CMD_BUSY         BIT(31)
/* [한국어] 명령 종류가 들어가는 필드. */
#define DART_T8110_TLB_CMD_OP           GENMASK(10, 8)
/* [한국어] 전체 TLB를 비운다. 이 드라이버는 쓰지 않는다. */
#define DART_T8110_TLB_CMD_OP_FLUSH_ALL 0
/* [한국어] 지정한 스트림의 TLB만 비운다. 이쪽을 쓴다. */
#define DART_T8110_TLB_CMD_OP_FLUSH_SID 1
/* [한국어] 대상 스트림 번호가 들어가는 필드. */
#define DART_T8110_TLB_CMD_STREAM       GENMASK(7, 0)

/* [한국어] T8110의 오류 상태 레지스터. */
#define DART_T8110_ERROR 0x100
/* [한국어] 오류를 낸 스트림 번호. 스트림이 256개라 T8020보다
 * 필드가 넓다. */
#define DART_T8110_ERROR_STREAM GENMASK(27, 20)
/* [한국어] 오류 원인 코드. */
#define DART_T8110_ERROR_CODE GENMASK(14, 0)
/* [한국어] 유효한 오류가 있다는 표시. */
#define DART_T8110_ERROR_FLAG BIT(31)

/* [한국어] 오류 인터럽트 마스크. 리셋에서 0을 써 모두 허용한다. */
#define DART_T8110_ERROR_MASK 0x104

/* [한국어] 읽기 권한 위반. */
#define DART_T8110_ERROR_READ_FAULT BIT(5)
/* [한국어] 쓰기 권한 위반. */
#define DART_T8110_ERROR_WRITE_FAULT BIT(4)
/* [한국어] 말단 페이지 엔트리 없음. */
#define DART_T8110_ERROR_NO_PTE BIT(3)
/* [한국어] 중간 단계 엔트리 없음. */
#define DART_T8110_ERROR_NO_PMD BIT(2)
/* [한국어] 최상위 단계 엔트리 없음. T8110만 4단계를 지원해
 * 이 코드가 추가로 존재한다. */
#define DART_T8110_ERROR_NO_PGD BIT(1)
/* [한국어] TTBR 자체가 유효하지 않음. */
#define DART_T8110_ERROR_NO_TTBR BIT(0)

/* [한국어] 오류 주소의 하위 32비트. */
#define DART_T8110_ERROR_ADDR_LO 0x170
/* [한국어] 오류 주소의 상위 32비트. */
#define DART_T8110_ERROR_ADDR_HI 0x174

/* [한국어] 오류를 낸 스트림들의 비트맵.
 * 인터럽트 핸들러가 처리 후 전부 1을 써 지운다. */
#define DART_T8110_ERROR_STREAMS 0x1c0

/* [한국어] T8110의 보호(잠금) 상태 레지스터. */
#define DART_T8110_PROTECT 0x200
/* [한국어] 보호를 푸는 레지스터. 이 드라이버는 쓰지 않는다. */
#define DART_T8110_UNPROTECT 0x204
/* [한국어] 보호 설정 자체를 잠그는 레지스터. */
#define DART_T8110_PROTECT_LOCK 0x208
/* [한국어] TTBR과 TCR이 보호되어 있다는 비트.
 * 세워져 있으면 부트로더가 잠근 것이라 probe가 실패한다 —
 * T8020의 CONFIG_LOCK에 해당한다. */
#define DART_T8110_PROTECT_TTBR_TCR BIT(0)

/* [한국어] 스트림 전체 활성화 레지스터(T8110). */
#define DART_T8110_ENABLE_STREAMS  0xc00
/* [한국어] 스트림 비활성화 레지스터. 이 드라이버는 쓰지 않는다 —
 * 격리는 TCR로 하기 때문이다. */
#define DART_T8110_DISABLE_STREAMS 0xc20

/* [한국어] T8110의 스트림별 TCR 시작 오프셋. */
#define DART_T8110_TCR                  0x1000
/* [한국어] 스트림 번호를 다른 번호로 바꿔 매핑하는 기능. 쓰지 않는다. */
#define DART_T8110_TCR_REMAP            GENMASK(11, 8)
/* [한국어] 그 재매핑을 활성화하는 비트. */
#define DART_T8110_TCR_REMAP_EN         BIT(7)
/* [한국어] 3단계 대신 4단계 워크를 쓰라는 비트.
 * IOVA 폭이 36비트를 넘을 때 필요하다. */
#define DART_T8110_TCR_FOUR_LEVEL       BIT(3)
/* [한국어] 접근 권한 필터를 건너뛴다. */
#define DART_T8110_TCR_BYPASS_DAPF      BIT(2)
/* [한국어] DART 변환을 건너뛴다. */
#define DART_T8110_TCR_BYPASS_DART      BIT(1)
/* [한국어] 변환을 수행하라. T8020과 비트 위치가 완전히 다르므로
 * hw 구조체가 이 차이를 감싼다. */
#define DART_T8110_TCR_TRANSLATE_ENABLE BIT(0)

/* [한국어] T8110의 TTBR 시작 오프셋. */
#define DART_T8110_TTBR       0x1400
/* [한국어] 유효 비트. T8020은 최상위 비트, T8110은 최하위 비트다. */
#define DART_T8110_TTBR_VALID BIT(0)
/* [한국어] 주소 필드가 비트 2부터 시작한다 — 하위 두 비트를
 * 유효 비트 등이 쓰기 때문이다. */
#define DART_T8110_TTBR_ADDR_FIELD_SHIFT 2
/* [한국어] 물리 주소를 14비트 밀어 담는다. 즉 테이블이 16KB
 * 정렬이어야 한다 — T8110의 페이지 크기가 16KB인 것과 맞물린다. */
#define DART_T8110_TTBR_SHIFT 14

/* [한국어] 스트림 번호로 그 스트림의 TCR 레지스터 주소를 구한다.
 * 세대별 시작 오프셋은 hw 구조체가 알려 주고, 스트림마다 4바이트다. */
#define DART_TCR(dart, sid) ((dart)->hw->tcr + ((sid) << 2))

/* [한국어] 스트림 번호와 인덱스로 그 TTBR 레지스터 주소를 구한다.
 * 스트림마다 ttbr_count개가 연속으로 놓여 있는 2차원 배열이라,
 * (스트림 × 개수 + 인덱스) × 4로 자리를 잡는다. */
#define DART_TTBR(dart, sid, idx) ((dart)->hw->ttbr + \
				   (((dart)->hw->ttbr_count * (sid)) << 2) + \
				   ((idx) << 2))

/* [한국어] 스트림 맵 구조체의 전방 선언.
 * 아래 hw 구조체의 콜백 시그니처에 필요하지만, 정의는 그보다
 * 뒤에 오기 때문이다. */
struct apple_dart_stream_map;

/* [한국어] DART 하드웨어의 세대 구분.
 * T8020과 T6000은 레지스터 배치가 같고 주소 폭과 페이지 테이블
 * 포맷만 다르며, T8110은 배치 자체가 다르다. */
enum dart_type {
	DART_T8020,
	/* [한국어] M1 이전 세대(T8103 등). 36비트 물리 주소, APPLE_DART 포맷. */

	DART_T6000,
	/* [한국어] M1 Pro/Max 세대. 42비트 물리 주소, APPLE_DART2 포맷.
	 * 레지스터 배치는 T8020과 같다. */

	DART_T8110,
	/* [한국어] 최신 세대. 레지스터 배치가 완전히 다르고, 주소 폭과
	 * 스트림 수를 하드웨어가 알려 주며, 4단계 워크를 지원한다. */
};

/* [한국어] 세대별 하드웨어 차이를 한곳에 모은 표.
 * 이 구조체가 있어서 나머지 코드가 세대를 거의 의식하지 않는다. */
struct apple_dart_hw {
	enum dart_type type;
	/* [한국어] 어느 세대인가.
	 * 읽는 자: probe가 주소 폭을 어떻게 알아낼지 정할 때, 그리고
	 *          리셋이 T8110 전용 레지스터를 만질지 정할 때. */

	irqreturn_t (*irq_handler)(int irq, void *dev);
	/* [한국어] 이 세대의 폴트 인터럽트 핸들러.
	 * 왜 세대별인가: 오류 레지스터의 위치와 코드 값이 다르다. */

	int (*invalidate_tlb)(struct apple_dart_stream_map *stream_map);
	/* [한국어] 이 세대의 TLB 무효화 함수.
	 * 왜 세대별인가: T8020은 스트림 비트맵을 레지스터에 써 두고
	 *                명령을 내지만, T8110은 스트림마다 명령을 낸다. */

	u32 oas;
	/* [한국어] 출력(물리) 주소 폭. T8020/T6000에서만 쓰인다 —
	 * T8110은 하드웨어가 직접 알려 주기 때문이다. */

	enum io_pgtable_fmt fmt;
	/* [한국어] 이 세대가 쓰는 페이지 테이블 포맷.
	 * 읽는 자: finalize_domain의 alloc_io_pgtable_ops.
	 * 값 범위: APPLE_DART(36비트) 또는 APPLE_DART2(더 넓은 주소). */

	int max_sid_count;
	/* [한국어] 이 세대의 스트림 최대 개수.
	 * T8020/T6000에서는 이 값이 곧 실제 스트림 수가 된다. */

	u32 lock;
	/* [한국어] 잠금 상태를 담은 레지스터의 오프셋.
	 * 읽는 자: hw_reset이 부트로더가 잠갔는지 확인할 때. */

	u32 lock_bit;
	/* [한국어] 그 레지스터에서 잠금을 뜻하는 비트.
	 * 세워져 있으면 재부팅 전까지 이 DART를 쓸 수 없다. */

	u32 error;
	/* [한국어] 오류 상태 레지스터의 오프셋.
	 * 읽는 자: hw_reset이 밀린 오류를 지울 때와 인터럽트 핸들러. */

	u32 enable_streams;
	/* [한국어] 스트림 전체 활성화 레지스터의 오프셋.
	 * 리셋에서 전부 1을 쓴다 — 격리는 TCR이 담당한다. */

	u32 tcr;
	/* [한국어] 스트림별 TCR 배열의 시작 오프셋. */

	u32 tcr_enabled;
	/* [한국어] "변환을 수행하라"에 해당하는 TCR 값. */

	u32 tcr_disabled;
	/* [한국어] "아무것도 하지 마라"(= DMA 차단)에 해당하는 값.
	 * 두 세대 모두 0이며, 차단 도메인이 이 값을 쓴다. */

	u32 tcr_bypass;
	/* [한국어] "변환 없이 통과시켜라"에 해당하는 값.
	 * 값 범위: 0이면 이 하드웨어가 우회를 지원하지 않는다는 뜻이다
	 *          (USB4 DART가 그렇다). */

	u32 tcr_4level;
	/* [한국어] 4단계 워크를 요구하는 비트. T8110에만 있다.
	 * 읽는 자: enable_translation이 io-pgtable이 알려 준 단계 수를
	 *          보고 이 비트를 얹을지 정한다. */

	u32 ttbr;
	/* [한국어] 스트림별 TTBR 배열의 시작 오프셋. */

	u32 ttbr_valid;
	/* [한국어] TTBR이 유효함을 뜻하는 비트. 세대마다 위치가 다르다. */

	u32 ttbr_addr_field_shift;
	/* [한국어] 레지스터 안에서 주소 필드가 시작하는 비트 위치.
	 * T8020은 0, T8110은 2다. */

	u32 ttbr_shift;
	/* [한국어] 물리 주소를 레지스터에 담기 전에 오른쪽으로 밀 비트 수.
	 * 곧 테이블에 요구되는 정렬이기도 하다(T8020은 4KB, T8110은 16KB).
	 * 읽는 자: hw_set_ttbr가 정렬을 검사하고 값을 만들 때. */

	int ttbr_count;
	/* [한국어] 스트림 하나가 갖는 TTBR의 개수.
	 * 값 범위: T8020 계열 4개, T8110 1개.
	 * 왜 중요한가: T8020에서는 이 4개가 사실상 최상위 테이블을
	 *              네 조각으로 나눈 것이라, io-pgtable이 조각마다
	 *              주소를 하나씩 돌려준다. */
};

/*
 * Private structure associated with each DART device.
 *
 * @dev: device struct
 * @hw: SoC-specific hardware data
 * @regs: mapped MMIO region
 * @irq: interrupt number, can be shared with other DARTs
 * @clks: clocks associated with this DART
 * @num_clks: number of @clks
 * @lock: lock for hardware operations involving this dart
 * @pgsize: pagesize supported by this DART
 * @supports_bypass: indicates if this DART supports bypass mode
 * @sid2group: maps stream ids to iommu_groups
 * @iommu: iommu core device
 */
/* [한국어] DART 하드웨어 하나의 상태. */
struct apple_dart {
	struct device *dev;
	/* [한국어] 이 DART의 플랫폼 디바이스.
	 * 읽는 자: 로그, devm 할당, io-pgtable의 iommu_dev, 디바이스 링크. */

	const struct apple_dart_hw *hw;
	/* [한국어] 이 세대의 레지스터 배치와 콜백.
	 * 설정자: probe가 디바이스 트리 매치 데이터에서 얻는다. */

	void __iomem *regs;
	/* [한국어] 매핑된 MMIO 영역.
	 * 값 범위: 최소 0x4000 바이트여야 한다 — T8110의 TCR 배열이
	 *          0x1000부터 시작하기 때문이다. */

	int irq;
	/* [한국어] 폴트 인터럽트 번호.
	 * 주의: 여러 DART가 이 인터럽트를 공유할 수 있어, 핸들러가
	 *       오류 플래그를 보고 자기 것인지 판별해야 한다. */

	struct clk_bulk_data *clks;
	/* [한국어] 이 DART에 딸린 클럭들.
	 * 설정자: probe의 devm_clk_bulk_get_all — 디바이스 트리에 있는
	 *          것을 전부 가져온다. */

	int num_clks;
	/* [한국어] 그 클럭의 개수. */

	spinlock_t lock;
	/* [한국어] 이 DART의 하드웨어 명령을 직렬화하는 락.
	 * 읽는 자: TLB 명령 함수 둘. 명령 레지스터가 하나뿐이라
	 *          두 CPU가 동시에 쓰면 서로의 명령을 덮어쓴다.
	 * 동기화: irqsave — 무효화가 atomic 문맥에서 불릴 수 있다. */

	u32 ias;
	/* [한국어] 입력(IOVA) 주소 폭.
	 * 설정자: T8020 계열은 32으로 고정, T8110은 하드웨어가 알려 준다.
	 * 읽는 자: io-pgtable 설정과 도메인 aperture 계산. */

	u32 oas;
	/* [한국어] 출력(물리) 주소 폭.
	 * 설정자: T8020 계열은 hw 표의 값, T8110은 하드웨어에서 읽는다. */

	u32 pgsize;
	/* [한국어] 이 DART가 쓰는 페이지 크기.
	 * 값 범위: 4KB 또는 16KB.
	 * 왜 중요한가: 커널 페이지보다 크면 한 페이지만 매핑해도 이웃이
	 *              함께 열려 안전하지 않다 — 그래서 그런 DART는
	 *              통과 모드로만 쓴다. */

	u32 num_streams;
	/* [한국어] 이 DART의 실제 스트림 개수.
	 * 값 범위: DART_MAX_STREAMS(256)를 넘으면 probe가 거부한다.
	 * 읽는 자: 모든 비트맵 순회의 상한. */

	u32 supports_bypass : 1;
	/* [한국어] 이 하드웨어가 우회 모드를 지원하는가.
	 * 설정자: probe가 PARAMS2에서 읽는다.
	 * 읽는 자: master_cfg가 여러 DART의 값을 AND 해 최종 판단을 한다. */

	u32 four_level : 1;
	/* [한국어] 4단계 워크를 써야 하는가(T8110에서 IOVA 폭이 36 초과).
	 * 읽는 자: enable_translation이 io-pgtable의 단계 수와 대조해
	 *          WARN을 낸다. */

	struct iommu_group *sid2group[DART_MAX_STREAMS];
	/* [한국어] 스트림 번호 → IOMMU 그룹의 대응표.
	 * 설정자: device_group이 그룹을 정한 뒤 채우고,
	 *          release_group이 지운다.
	 * 왜 필요한가: 같은 스트림을 쓰는 디바이스들은 서로 격리할 수
	 *              없으므로 반드시 한 그룹이어야 한다. 이 표가
	 *              "이 스트림은 이미 어느 그룹에 속했나"를 답한다.
	 * 동기화: apple_dart_groups_lock. */

	struct iommu_device iommu;
	/* [한국어] IOMMU 코어에 등록되는 부분(임베드).
	 * 읽는 자: probe_device가 이 주소를 코어에 돌려준다. */

	u32 save_tcr[DART_MAX_STREAMS];
	/* [한국어] 절전 진입 시 저장해 두는 스트림별 TCR.
	 * 왜 필요한가: DART는 전원이 끊기면 레지스터 내용을 잃는다.
	 *              도메인 정보에서 재구성하는 대신 값을 통째로
	 *              떠 두었다가 되돌리는 편이 단순하다. */

	u32 save_ttbr[DART_MAX_STREAMS][DART_MAX_TTBR];
	/* [한국어] 마찬가지로 저장해 두는 스트림별 TTBR 값들.
	 * 설정자/읽는 자: suspend가 채우고 resume이 되돌린다. */
};

/*
 * Convenience struct to identify streams.
 *
 * The normal variant is used inside apple_dart_master_cfg which isn't written
 * to concurrently.
 * The atomic variant is used inside apple_dart_domain where we have to guard
 * against races from potential parallel calls to attach/detach_device.
 * Note that even inside the atomic variant the apple_dart pointer is not
 * protected: This pointer is initialized once under the domain init mutex
 * and never changed again afterwards. Devices with different dart pointers
 * cannot be attached to the same domain.
 *
 * @dart dart pointer
 * @sid stream id bitmap
 */
/* [한국어] "어느 DART의 어느 스트림들"을 가리키는 쌍.
 * 이 드라이버의 거의 모든 하드웨어 함수가 이것을 인자로 받는다. */
struct apple_dart_stream_map {
	struct apple_dart *dart;
	/* [한국어] 대상 DART.
	 * 값 범위: NULL이면 배열의 끝을 뜻한다 — for_each_stream_map의
	 *          종료 조건이 바로 이것이다. */

	DECLARE_BITMAP(sidmap, DART_MAX_STREAMS);
	/* [한국어] 그 DART 안에서 대상이 되는 스트림들의 비트맵.
	 * 설정자: of_xlate가 디바이스 트리의 SID마다 비트를 세운다.
	 * 읽는 자: 모든 하드웨어 조작이 이 비트맵을 훑는다.
	 * 동기화: master_cfg 안에서는 동시 접근이 없어 평범한 비트맵이면
	 *         충분하다. 도메인 쪽은 아래 원자적 변형을 쓴다. */
};
/* [한국어] 위와 같지만 원자적으로 갱신되는 변형.
 * 도메인에 붙는 스트림 집합은 attach/detach가 병렬로 일어날 수 있어
 * 락 없이 안전하게 비트를 세우고 지울 수 있어야 한다.
 * 원본 주석이 밝히듯 dart 포인터 자체는 보호되지 않는다 —
 * 도메인 완성 시 한 번 정해지고 다시 바뀌지 않기 때문이다. */
struct apple_dart_atomic_stream_map {
	struct apple_dart *dart;
	/* [한국어] 대상 DART. init_lock 아래에서 한 번만 설정된다. */

	atomic_long_t sidmap[BITS_TO_LONGS(DART_MAX_STREAMS)];
	/* [한국어] 원자적으로 갱신되는 스트림 비트맵.
	 * 설정자: mod_streams가 atomic_long_or/and로 켜고 끈다.
	 * 읽는 자: domain_flush_tlb가 읽어 평범한 비트맵으로 복사한 뒤
	 *          하드웨어 함수에 넘긴다. */
};

/*
 * This structure is attached to each iommu domain handled by a DART.
 *
 * @pgtbl_ops: pagetable ops allocated by io-pgtable
 * @finalized: true if the domain has been completely initialized
 * @init_lock: protects domain initialization
 * @stream_maps: streams attached to this domain (valid for DMA/UNMANAGED only)
 * @domain: core iommu domain pointer
 */
/* [한국어] IOMMU 도메인 하나. */
struct apple_dart_domain {
	struct io_pgtable_ops *pgtbl_ops;
	/* [한국어] 이 도메인의 페이지 테이블 연산.
	 * 설정자: finalize_domain의 alloc_io_pgtable_ops.
	 * 값 범위: 아직 완성되지 않은 도메인에서는 NULL이라,
	 *          map/iova_to_phys가 그것을 확인한다.
	 * 왜 위임하는가: 테이블 형식은 io-pgtable-dart.c가 이미 알고
	 *                있으므로 이 파일은 하드웨어 연결만 하면 된다. */

	bool finalized;
	/* [한국어] 도메인이 완성되었는가.
	 * 설정자: finalize_domain이 성공 시 세운다.
	 * 왜 필요한가: 같은 도메인에 여러 디바이스가 붙을 수 있는데,
	 *              페이지 테이블은 한 번만 만들어야 한다.
	 * 동기화: init_lock. */

	struct mutex init_lock;
	/* [한국어] 도메인 완성 과정을 보호하는 뮤텍스.
	 * 왜 스핀락이 아닌가: io-pgtable 생성이 잠들 수 있는 할당을
	 *                     하기 때문이다. attach 경로라 허용된다. */

	struct apple_dart_atomic_stream_map stream_maps[MAX_DARTS_PER_DEVICE];
	/* [한국어] 이 도메인에 붙어 있는 (DART, 스트림들) 쌍의 배열.
	 * 설정자: finalize_domain이 DART 포인터를 정하고, 이후
	 *          mod_streams가 비트만 원자적으로 갱신한다.
	 * 읽는 자: 무효화가 이 목록의 모든 스트림에 명령을 낸다.
	 * 값 범위: dart가 NULL인 항목부터가 배열의 끝이다. */

	struct iommu_domain domain;
	/* [한국어] 코어가 보는 도메인 부분(임베드).
	 * 설정자: finalize_domain이 pgsize_bitmap과 geometry를 채운다 —
	 *          완성 전까지는 비어 있다는 점에 유의. */
};

/*
 * This structure is attached to devices with dev_iommu_priv_set() on of_xlate
 * and contains a list of streams bound to this device.
 * So far the worst case seen is a single device with two streams
 * from different darts, such that this simple static array is enough.
 *
 * @streams: streams for this device
 */
/* [한국어] 마스터 디바이스에 매다는 설정. 이 디바이스가 어느
 * DART의 어느 스트림들로 DMA를 내는지가 담긴다. */
struct apple_dart_master_cfg {
	/* Intersection of DART capabilitles */
	u32 supports_bypass : 1;
	/* [한국어] 이 디바이스가 우회 모드를 쓸 수 있는가.
	 * 설정자: of_xlate가 true로 시작해 DART마다 AND 한다.
	 * 왜 교집합인가: 여러 DART 뒤에 있는 디바이스는 그중 하나라도
	 *                우회를 못 하면 전체가 우회할 수 없기 때문이다. */

	struct apple_dart_stream_map stream_maps[MAX_DARTS_PER_DEVICE];
	/* [한국어] 이 디바이스가 쓰는 (DART, 스트림들) 쌍의 배열.
	 * 설정자: of_xlate가 디바이스 트리 항목마다 채운다.
	 * 읽는 자: attach가 이 목록의 모든 DART를 똑같이 설정한다.
	 * 동기화: 동시 갱신이 없어 원자 연산이 필요 없다. */
};

/*
 * Helper macro to iterate over apple_dart_master_cfg.stream_maps and
 * apple_dart_domain.stream_maps
 *
 * @i int used as loop variable
 * @base pointer to base struct (apple_dart_master_cfg or apple_dart_domain)
 * @stream pointer to the apple_dart_streams struct for each loop iteration
 */
/* [한국어] 스트림 맵 배열을 훑는 관용구.
 * master_cfg와 domain 양쪽에 쓸 수 있는 것은 두 구조체가 모두
 * stream_maps라는 이름의 배열을 갖고 있어 매크로 치환이 통하기
 * 때문이다(타입은 다르지만 dart 필드가 첫 멤버로 같은 자리에 있다).
 * 종료 조건이 둘인 점에 유의: 배열 끝에 닿거나, dart가 NULL인
 * 빈 항목을 만나면 멈춘다. */
#define for_each_stream_map(i, base, stream_map)                               \
	for (i = 0, stream_map = &(base)->stream_maps[0];                      \
	     i < MAX_DARTS_PER_DEVICE && stream_map->dart;                     \
	     stream_map = &(base)->stream_maps[++i])

/* [한국어] 이 파일 끝에 정의되는 플랫폼 드라이버의 전방 선언. */
static struct platform_driver apple_dart_driver;
/* [한국어] 연산 테이블의 전방 선언. probe가 정의보다 앞에서 참조한다. */
static const struct iommu_ops apple_dart_iommu_ops;

/*
 * [한국어]
 * to_dart_domain - iommu_domain에서 바깥 apple_dart_domain을 복원한다
 *
 * @dom: 코어가 넘겨준 도메인.
 * @return: 드라이버 쪽 도메인.
 *
 * 실행 컨텍스트: 모든 도메인 콜백의 첫 줄.
 *
 * 호출 체인:
 *   각종 iommu_domain_ops 콜백 → [to_dart_domain]
 */
static struct apple_dart_domain *to_dart_domain(struct iommu_domain *dom)
{
	/* [한국어] 임베드 멤버의 주소에서 바깥 구조체를 역산한다. */
	return container_of(dom, struct apple_dart_domain, domain);
}

/*
 * [한국어]
 * apple_dart_hw_enable_translation - 스트림들의 TCR을 "변환 수행"으로 만든다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @levels: io-pgtable이 실제로 쓰는 워크 단계 수(3 또는 4).
 * @return: 없음.
 *
 * 이 드라이버의 하드웨어 조작 관용구가 여기서 처음 나온다:
 * **sidmap의 켜진 비트를 훑으며 스트림마다 레지스터를 쓴다.**
 * 스트림 하나하나가 독립된 TCR을 갖기 때문이다.
 *
 * 단계 수를 인자로 받는 이유는 T8110의 4단계 비트 때문이다.
 * io-pgtable이 주소 폭을 보고 몇 단계를 쓸지 정하면, 그 결정을
 * 하드웨어에도 알려 줘야 워크가 맞아떨어진다. 두 WARN은
 * 그 값이 앞뒤가 맞는지 확인한다.
 *
 * 실행 컨텍스트: attach 경로. 클럭이 켜져 있다.
 *
 * 호출 체인:
 *   apple_dart_setup_translation() → [apple_dart_hw_enable_translation]
 */
static void
apple_dart_hw_enable_translation(struct apple_dart_stream_map *stream_map, int levels)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = stream_map->dart;
	/* [한국어] 세대별 "변환 수행" 비트에서 시작한다. */
	u32 tcr = dart->hw->tcr_enabled;
	/* [한국어] 스트림 순회 인덱스. */
	int sid;

	/* [한국어] 4단계 워크라면 그것을 하드웨어에도 알려야 한다.
	 * T8020 계열은 tcr_4level이 0이라 이 OR가 아무 효과가 없다. */
	if (levels == 4)
		tcr |= dart->hw->tcr_4level;

	/* [한국어] 이 하드웨어가 아는 단계 수는 3과 4뿐이다. */
	WARN_ON(levels != 3 && levels != 4);
	/* [한국어] 4단계를 지원하지 않는 하드웨어에 4단계 테이블을
	 * 걸면 워크가 어긋난다 — io-pgtable 설정이 잘못된 것이다. */
	WARN_ON(levels == 4 && !dart->four_level);
	/* [한국어] 대상 스트림마다 TCR을 써 변환을 켠다. */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(tcr, dart->regs + DART_TCR(dart, sid));
}

/*
 * [한국어]
 * apple_dart_hw_disable_dma - 스트림들의 DMA를 차단한다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @return: 없음.
 *
 * TCR에 tcr_disabled(두 세대 모두 0)를 쓰면 변환도 우회도 하지
 * 않아 결과적으로 DMA가 막힌다. 차단 도메인의 구현이자,
 * 리셋에서 모든 스트림을 안전한 상태로 만드는 수단이기도 하다.
 *
 * 실행 컨텍스트: 차단 attach와 하드웨어 리셋.
 *
 * 호출 체인:
 *   apple_dart_attach_dev_blocked() / apple_dart_hw_reset()
 *   → [apple_dart_hw_disable_dma]
 */
static void apple_dart_hw_disable_dma(struct apple_dart_stream_map *stream_map)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = stream_map->dart;
	/* [한국어] 스트림 순회 인덱스. */
	int sid;

	/* [한국어] 변환도 우회도 아닌 값을 써 DMA를 막는다. */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(dart->hw->tcr_disabled, dart->regs + DART_TCR(dart, sid));
}

/*
 * [한국어]
 * apple_dart_hw_enable_bypass - 스트림들을 통과 모드로 만든다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @return: 없음.
 *
 * TCR에 우회 비트들을 쓰면 이 스트림의 DMA가 변환도 권한 검사도
 * 없이 그대로 나간다. tcr_bypass가 0인 하드웨어(USB4 DART)는
 * 이 모드를 쓸 수 없어, 호출 전에 supports_bypass를 확인해야 한다 —
 * WARN이 그 계약을 지킨다.
 *
 * 실행 컨텍스트: identity attach.
 *
 * 호출 체인:
 *   apple_dart_attach_dev_identity() → [apple_dart_hw_enable_bypass]
 */
static void
apple_dart_hw_enable_bypass(struct apple_dart_stream_map *stream_map)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = stream_map->dart;
	/* [한국어] 스트림 순회 인덱스. */
	int sid;

	/* [한국어] 우회를 지원하지 않는 DART에 이 함수를 부르면
	 * 잘못된 TCR 값을 쓰게 된다 — 호출자의 계약 위반이다. */
	WARN_ON(!stream_map->dart->supports_bypass);
	/* [한국어] 대상 스트림마다 우회 값을 쓴다. */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(dart->hw->tcr_bypass,
		       dart->regs + DART_TCR(dart, sid));
}

/*
 * [한국어]
 * apple_dart_hw_set_ttbr - 스트림들의 idx번째 TTBR에 테이블 주소를 심는다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @idx: TTBR 인덱스(T8020은 0~3, T8110은 0뿐).
 * @paddr: 걸 테이블의 물리 주소.
 * @return: 없음.
 *
 * 레지스터 값 만들기가 두 단계다: 주소를 ttbr_shift만큼 오른쪽으로
 * 밀어 하위 비트를 버리고, 그것을 다시 addr_field_shift만큼 왼쪽으로
 * 밀어 레지스터 안의 제자리에 놓은 뒤, 유효 비트를 얹는다.
 * 세대마다 이 세 값이 달라 hw 구조체가 감싼다.
 *
 * WARN은 정렬을 검사한다. ttbr_shift만큼 밀어 버릴 하위 비트가
 * 0이 아니면 그 정보가 소실되어 엉뚱한 테이블을 가리키게 된다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   apple_dart_setup_translation() → [apple_dart_hw_set_ttbr]
 */
static void apple_dart_hw_set_ttbr(struct apple_dart_stream_map *stream_map,
				   u8 idx, phys_addr_t paddr)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = stream_map->dart;
	/* [한국어] 스트림 순회 인덱스. */
	int sid;

	/* [한국어] 시프트로 버려질 하위 비트가 0인지 확인한다 —
	 * 0이 아니면 정렬 요구를 어긴 테이블이다. */
	WARN_ON(paddr & ((1 << dart->hw->ttbr_shift) - 1));
	/* [한국어] 스트림마다 같은 테이블 주소를 심는다. 주소를 밀어
	 * 압축하고 필드 위치로 옮긴 뒤 유효 비트를 얹는다. */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(dart->hw->ttbr_valid |
		       (paddr >> dart->hw->ttbr_shift) << dart->hw->ttbr_addr_field_shift,
		       dart->regs + DART_TTBR(dart, sid, idx));
}

/*
 * [한국어]
 * apple_dart_hw_clear_ttbr - 스트림들의 idx번째 TTBR을 지운다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @idx: TTBR 인덱스.
 * @return: 없음.
 *
 * 0을 쓰면 유효 비트도 함께 0이 되어 그 TTBR이 무효해진다.
 * io-pgtable이 요구하는 개수보다 하드웨어의 TTBR이 많을 때
 * 남는 자리를 정리하는 데 쓰인다.
 *
 * 실행 컨텍스트: attach와 리셋 경로.
 *
 * 호출 체인:
 *   apple_dart_setup_translation() / hw_clear_all_ttbrs()
 *   → [apple_dart_hw_clear_ttbr]
 */
static void apple_dart_hw_clear_ttbr(struct apple_dart_stream_map *stream_map,
				     u8 idx)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = stream_map->dart;
	/* [한국어] 스트림 순회 인덱스. */
	int sid;

	/* [한국어] 0을 쓰면 유효 비트가 내려가 이 TTBR이 무효해진다. */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams)
		writel(0, dart->regs + DART_TTBR(dart, sid, idx));
}

/*
 * [한국어]
 * apple_dart_hw_clear_all_ttbrs - 스트림들의 모든 TTBR을 지운다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @return: 없음.
 *
 * 리셋에서 이전 상태(부트로더가 걸어 둔 테이블 등)를 지우는 데 쓴다.
 *
 * 실행 컨텍스트: 하드웨어 리셋.
 *
 * 호출 체인:
 *   apple_dart_hw_reset() → [apple_dart_hw_clear_all_ttbrs]
 */
static void
apple_dart_hw_clear_all_ttbrs(struct apple_dart_stream_map *stream_map)
{
	/* [한국어] TTBR 인덱스 순회 변수. */
	int i;

	/* [한국어] 이 세대가 가진 TTBR 개수만큼 반복해 전부 지운다. */
	for (i = 0; i < stream_map->dart->hw->ttbr_count; ++i)
		apple_dart_hw_clear_ttbr(stream_map, i);
}

/*
 * [한국어]
 * apple_dart_t8020_hw_stream_command - T8020 계열에 스트림 명령을 낸다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @command: 명령 비트(현재는 무효화뿐).
 * @return: 0 성공, -ETIMEDOUT.
 *
 * T8020의 명령 모델: **대상 스트림들을 SELECT 레지스터에 비트맵으로
 * 써 두고, 명령을 한 번 낸다.** 그래서 여러 스트림을 한 번의 명령으로
 * 처리할 수 있다. T8110은 이와 달리 스트림마다 명령을 내야 한다.
 *
 * SELECT 레지스터가 32비트씩 여러 개로 이어져 있어, 스트림이 32개를
 * 넘으면 4바이트 간격으로 나눠 쓴다.
 *
 * 락을 잡는 이유: 명령 레지스터가 하나뿐이라 두 CPU가 동시에
 * SELECT를 쓰면 서로의 대상이 뒤섞인다.
 *
 * 실행 컨텍스트: 무효화 경로. atomic 폴링이라 인터럽트가 꺼진
 * 문맥에서도 안전하다.
 *
 * 호출 체인:
 *   apple_dart_t8020_hw_invalidate_tlb() → [apple_dart_t8020_hw_stream_command]
 */
static int
apple_dart_t8020_hw_stream_command(struct apple_dart_stream_map *stream_map,
			     u32 command)
{
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 폴링 결과와 SELECT 레지스터 순회 인덱스. */
	int ret, i;
	/* [한국어] 폴링이 읽은 명령 레지스터 값. */
	u32 command_reg;

	/* [한국어] SELECT와 COMMAND를 쓰는 구간이 갈라지면 안 된다. */
	spin_lock_irqsave(&stream_map->dart->lock, flags);

	/* [한국어] 대상 스트림 비트맵을 SELECT 레지스터들에 옮겨 쓴다.
	 * 32비트마다 레지스터가 하나씩이라 4바이트 간격으로 나눈다. */
	for (i = 0; i < BITS_TO_U32(stream_map->dart->num_streams); i++)
		writel(stream_map->sidmap[i],
		       stream_map->dart->regs + DART_T8020_STREAM_SELECT + 4 * i);
	/* [한국어] 선택된 스트림들에 대해 명령을 실행시킨다. */
	writel(command, stream_map->dart->regs + DART_T8020_STREAM_COMMAND);

	/* [한국어] busy 비트가 내려갈 때까지 1us 간격으로 기다린다.
	 * atomic 변형이라 잠들지 않는다 — 락을 쥔 채이므로 필수다. */
	ret = readl_poll_timeout_atomic(
		stream_map->dart->regs + DART_T8020_STREAM_COMMAND, command_reg,
		!(command_reg & DART_T8020_STREAM_COMMAND_BUSY), 1,
		DART_STREAM_COMMAND_BUSY_TIMEOUT);

	/* [한국어] 명령이 끝났으니 락을 놓는다. 로그는 락 밖에서 찍는다. */
	spin_unlock_irqrestore(&stream_map->dart->lock, flags);

	if (ret) {
		/* [한국어] 하드웨어가 명령을 끝내지 못했다 — 무효화가
		 * 이뤄지지 않았을 수 있어 심각한 상황이다. */
		dev_err(stream_map->dart->dev,
			"busy bit did not clear after command %x for streams %lx\n",
			command, stream_map->sidmap[0]);
		return ret;	/* [한국어] 무효화가 실패했음을 호출자에게 전한다. */
	}

	return 0;	/* [한국어] 명령이 정상적으로 끝났다. */
}

/*
 * [한국어]
 * apple_dart_t8110_hw_tlb_command - T8110에 TLB 명령을 낸다
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @command: 명령 종류(FLUSH_SID 등).
 * @return: 0 성공, -ETIMEDOUT.
 *
 * T8110의 명령 모델은 T8020과 반대다: **대상 스트림을 명령 워드
 * 안에 넣는다.** 그래서 SELECT 레지스터가 없는 대신, 스트림마다
 * 명령을 하나씩 내고 각각의 완료를 기다려야 한다.
 *
 * 루프 안에서 폴링하므로 스트림이 많으면 락을 오래 쥔다.
 * 그래도 락 안에서 해야 하는 이유는 명령 레지스터가 하나이기 때문이다.
 *
 * sid 변수를 루프 밖의 로그에서 쓰는 점에 유의 — break로 빠져나온
 * 시점의 값이라 어느 스트림에서 실패했는지 알 수 있다.
 *
 * 실행 컨텍스트: 무효화 경로. atomic 폴링.
 *
 * 호출 체인:
 *   apple_dart_t8110_hw_invalidate_tlb() → [apple_dart_t8110_hw_tlb_command]
 */
static int
apple_dart_t8110_hw_tlb_command(struct apple_dart_stream_map *stream_map,
				u32 command)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = stream_map->dart;
	/* [한국어] 인터럽트 상태 저장용. */
	unsigned long flags;
	/* [한국어] 폴링 결과. 루프를 한 번도 안 돌 수 있어 0으로 시작한다. */
	int ret = 0;
	/* [한국어] 현재 처리 중인 스트림. 실패 시 로그에 쓰인다. */
	int sid;

	/* [한국어] 명령 레지스터를 독점한다. */
	spin_lock_irqsave(&dart->lock, flags);

	/* [한국어] 대상 스트림마다 명령을 하나씩 낸다 — T8110은
	 * 한 번에 여러 스트림을 지정할 수 없다. */
	for_each_set_bit(sid, stream_map->sidmap, dart->num_streams) {
		/* [한국어] 명령 종류와 대상 스트림을 한 워드에 조립한다. */
		u32 val = FIELD_PREP(DART_T8110_TLB_CMD_OP, command) |
			FIELD_PREP(DART_T8110_TLB_CMD_STREAM, sid);
		writel(val, dart->regs + DART_T8110_TLB_CMD);

		/* [한국어] 이 명령이 끝나야 다음 명령을 낼 수 있다. */
		ret = readl_poll_timeout_atomic(
			dart->regs + DART_T8110_TLB_CMD, val,
			!(val & DART_T8110_TLB_CMD_BUSY), 1,
			DART_STREAM_COMMAND_BUSY_TIMEOUT);

		/* [한국어] 하나가 멈추면 나머지도 될 리 없으므로 그만둔다.
		 * sid가 이 값으로 남아 아래 로그가 어느 스트림인지 알린다. */
		if (ret)
			break;

	}

	spin_unlock_irqrestore(&dart->lock, flags);	/* [한국어] 명령 레지스터 독점이 끝났으니 락을 놓는다. */

	if (ret) {
		/* [한국어] 어느 스트림에서 멈췄는지 남긴다. */
		dev_err(stream_map->dart->dev,
			"busy bit did not clear after command %x for stream %d\n",
			command, sid);
		return ret;	/* [한국어] 어느 스트림에서 멈췄는지 알린 뒤 실패를 전한다. */
	}

	return 0;	/* [한국어] 모든 스트림의 명령이 끝났다. */
}

/*
 * [한국어]
 * apple_dart_t8020_hw_invalidate_tlb - T8020 계열의 TLB 무효화
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @return: 0 성공, 음수 오류.
 *
 * hw->invalidate_tlb에 걸리는 콜백이다. 세대별 명령 함수에
 * 무효화 명령을 얹는 얇은 껍데기다.
 *
 * 실행 컨텍스트: 무효화 경로.
 *
 * 호출 체인:
 *   hw->invalidate_tlb 를 통해 간접 호출
 *   → [apple_dart_t8020_hw_invalidate_tlb]
 */
static int
apple_dart_t8020_hw_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	/* [한국어] 선택된 스트림 전부에 대해 무효화 명령을 한 번 낸다. */
	return apple_dart_t8020_hw_stream_command(
		stream_map, DART_T8020_STREAM_COMMAND_INVALIDATE);
}

/*
 * [한국어]
 * apple_dart_t8110_hw_invalidate_tlb - T8110의 TLB 무효화
 *
 * @stream_map: 대상 (DART, 스트림들).
 * @return: 0 성공, 음수 오류.
 *
 * 전체 비우기(FLUSH_ALL)가 아니라 스트림별 비우기를 쓴다.
 * 다른 스트림의 캐시를 버리면 무관한 디바이스의 성능이 떨어지기
 * 때문이다.
 *
 * 실행 컨텍스트: 무효화 경로.
 *
 * 호출 체인:
 *   hw->invalidate_tlb 를 통해 간접 호출
 *   → [apple_dart_t8110_hw_invalidate_tlb]
 */
static int
apple_dart_t8110_hw_invalidate_tlb(struct apple_dart_stream_map *stream_map)
{
	/* [한국어] 대상 스트림들만 비운다 — 전체 비우기는 무관한
	 * 디바이스까지 느리게 만든다. */
	return apple_dart_t8110_hw_tlb_command(
		stream_map, DART_T8110_TLB_CMD_OP_FLUSH_SID);
}

/*
 * [한국어]
 * apple_dart_hw_reset - DART를 알려진 초기 상태로 만든다
 *
 * @dart: 대상 DART.
 * @return: 0 성공, -EINVAL(잠김), 음수 무효화 오류.
 *
 * probe와 resume, remove에서 불린다. 하는 일이 다섯이다.
 *
 *  1) 부트로더가 잠갔는지 확인한다. 잠겼다면 TTBR/TCR을 바꿀 수
 *     없으므로 이 DART를 쓸 수 없다 — 재부팅 전까지는 방법이 없다.
 *  2) 모든 스트림의 DMA를 차단하고 TTBR을 지운다. 이전 상태
 *     (부트로더가 걸어 둔 테이블)를 완전히 없애는 것이다.
 *  3) **모든 스트림을 전역적으로 활성화한다.** 원본 주석이 밝히듯
 *     격리는 TCR로 하기 때문이다. 스트림이 켜져 있어도 TCR이
 *     차단이면 DMA는 나가지 못한다.
 *  4) 밀린 오류를 지운다. 인터럽트를 열기 전에 해야 옛 오류로
 *     즉시 인터럽트가 터지지 않는다.
 *  5) TLB를 비운다.
 *
 * 임시 stream_map을 스택에 만들어 "모든 스트림"을 표현하는 점이
 * 이 함수의 관용구다.
 *
 * 실행 컨텍스트: probe/resume/remove. 클럭이 켜져 있어야 한다.
 *
 * 호출 체인:
 *   apple_dart_probe() / resume() / remove() → [apple_dart_hw_reset]
 */
static int apple_dart_hw_reset(struct apple_dart *dart)
{
	/* [한국어] 잠금 레지스터에서 읽은 값. */
	u32 config;
	/* [한국어] "이 DART의 모든 스트림"을 뜻하는 임시 맵. */
	struct apple_dart_stream_map stream_map;
	/* [한국어] 활성화 레지스터 순회 인덱스. */
	int i;

	/* [한국어] 부트로더가 이 DART를 잠갔는지 확인한다. */
	config = readl(dart->regs + dart->hw->lock);
	if (config & dart->hw->lock_bit) {
		/* [한국어] 잠긴 DART는 TTBR도 TCR도 바꿀 수 없어 쓸 수 없다.
		 * 재부팅 말고는 푸는 방법이 없다. */
		dev_err(dart->dev, "DART is locked down until reboot: %08x\n",
			config);
		return -EINVAL;	/* [한국어] 테이블 주소를 얻지 못했으니 이 DART를 쓸 수 없다. */
	}

	/* [한국어] 이 DART를 대상으로 삼는다. */
	stream_map.dart = dart;
	/* [한국어] 비트맵을 비운 뒤 실제 스트림 수만큼 전부 세운다 —
	 * "모든 스트림"을 뜻하는 맵이 된다. */
	bitmap_zero(stream_map.sidmap, DART_MAX_STREAMS);
	bitmap_set(stream_map.sidmap, 0, dart->num_streams);
	/* [한국어] 모든 스트림의 DMA를 막는다. */
	apple_dart_hw_disable_dma(&stream_map);
	/* [한국어] 부트로더가 걸어 둔 테이블 주소를 모두 지운다. */
	apple_dart_hw_clear_all_ttbrs(&stream_map);

	/* enable all streams globally since TCR is used to control isolation */
	/* [한국어] 스트림을 전부 켠다. 격리는 위에서 차단해 둔 TCR이
	 * 담당하므로, 스트림 활성화 자체는 안전하다. */
	for (i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		writel(U32_MAX, dart->regs + dart->hw->enable_streams + 4 * i);

	/* clear any pending errors before the interrupt is unmasked */
	/* [한국어] 읽은 값을 그대로 되쓰는 것이 오류를 지우는 방식이다
	 * (write-1-to-clear). 인터럽트를 열기 전에 해야 옛 오류로
	 * 즉시 인터럽트가 터지지 않는다. */
	writel(readl(dart->regs + dart->hw->error), dart->regs + dart->hw->error);

	/* [한국어] T8110에만 있는 오류 마스크를 열어 둔다.
	 * T8020 계열에는 이 레지스터가 없다. */
	if (dart->hw->type == DART_T8110)
		writel(0,  dart->regs + DART_T8110_ERROR_MASK);

	/* [한국어] 마지막으로 모든 스트림의 TLB를 비워, 지워 버린
	 * 테이블의 잔재가 남지 않게 한다. */
	return dart->hw->invalidate_tlb(&stream_map);
}

/*
 * [한국어]
 * apple_dart_domain_flush_tlb - 도메인에 붙은 모든 스트림의 TLB를 비운다
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * 이 드라이버의 모든 무효화가 결국 이 함수로 모인다.
 * 도메인의 원자적 스트림 맵을 평범한 비트맵으로 **복사한 뒤**
 * 하드웨어 함수에 넘기는 것이 요점이다. 하드웨어 함수들은
 * 평범한 비트맵을 기대하고, 복사 시점의 스냅숏이면 충분하기
 * 때문이다(그 사이 스트림이 추가되면 그쪽 attach가 따로 비운다).
 *
 * 도메인이 여러 DART에 걸쳐 있을 수 있어 바깥 루프가 필요하다.
 *
 * 실행 컨텍스트: 무효화 경로. atomic일 수 있다.
 *
 * 호출 체인:
 *   flush_iotlb_all() / iotlb_sync() / iotlb_sync_map()
 *   → [apple_dart_domain_flush_tlb] → hw->invalidate_tlb()
 */
static void apple_dart_domain_flush_tlb(struct apple_dart_domain *domain)
{
	/* [한국어] DART 순회 인덱스와 비트맵 워드 인덱스. */
	int i, j;
	/* [한국어] 도메인 쪽 원자적 스트림 맵. */
	struct apple_dart_atomic_stream_map *domain_stream_map;
	/* [한국어] 하드웨어 함수에 넘길 평범한 스냅숏. */
	struct apple_dart_stream_map stream_map;

	/* [한국어] 이 도메인이 걸쳐 있는 DART마다 반복한다. */
	for_each_stream_map(i, domain, domain_stream_map) {
		/* [한국어] 대상 DART를 스냅숏에 옮긴다. */
		stream_map.dart = domain_stream_map->dart;

		/* [한국어] 원자적 비트맵을 워드 단위로 읽어 복사한다.
		 * 한 번에 읽을 수 없으므로 완전한 원자 스냅숏은 아니지만,
		 * 무효화는 더 많이 비우는 쪽이 안전한 방향이라 문제가 없다. */
		for (j = 0; j < BITS_TO_LONGS(stream_map.dart->num_streams); j++)
			stream_map.sidmap[j] = atomic_long_read(&domain_stream_map->sidmap[j]);

		/* [한국어] 세대에 맞는 무효화 함수를 부른다. */
		stream_map.dart->hw->invalidate_tlb(&stream_map);
	}
}

/*
 * [한국어]
 * apple_dart_flush_iotlb_all - 코어의 전체 무효화 요청을 처리한다
 *
 * @domain: 대상 도메인.
 * @return: 없음.
 *
 * DART의 TLB 무효화는 스트림 단위가 최소 단위라, 범위 무효화와
 * 전체 무효화의 구현이 결국 같다.
 *
 * 실행 컨텍스트: 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 flush_iotlb_all → [apple_dart_flush_iotlb_all]
 */
static void apple_dart_flush_iotlb_all(struct iommu_domain *domain)
{
	/* [한국어] 도메인의 모든 스트림을 비운다. */
	apple_dart_domain_flush_tlb(to_dart_domain(domain));
}

/*
 * [한국어]
 * apple_dart_iotlb_sync - 모아 둔 해제 범위를 무효화한다
 *
 * @domain: 대상 도메인.
 * @gather: 코어가 모은 범위(이 하드웨어는 쓸 수 없다).
 * @return: 없음.
 *
 * gather를 무시하는 이유: DART에는 특정 IOVA 범위만 비우는 명령이
 * 없다. 스트림 단위 비우기가 가장 좁은 단위다.
 *
 * 실행 컨텍스트: 해제 후 무효화 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync → [apple_dart_iotlb_sync]
 */
static void apple_dart_iotlb_sync(struct iommu_domain *domain,
				  struct iommu_iotlb_gather *gather)
{
	/* [한국어] 범위와 무관하게 스트림 전체를 비운다. */
	apple_dart_domain_flush_tlb(to_dart_domain(domain));
}

/*
 * [한국어]
 * apple_dart_iotlb_sync_map - 새 매핑을 하드웨어에 반영한다
 *
 * @domain: 대상 도메인.
 * @iova: 새 매핑의 시작(쓰지 않는다).
 * @size: 새 매핑의 크기(쓰지 않는다).
 * @return: 항상 0.
 *
 * 매핑을 **추가**한 뒤에도 비워야 하는 이유: DART의 TLB는 "이 주소에
 * 매핑이 없다"는 부정 결과도 캐시할 수 있어, 그것을 지워 주지 않으면
 * 새 매핑이 보이지 않는다.
 *
 * 실행 컨텍스트: 매핑 직후.
 *
 * 호출 체인:
 *   IOMMU 코어 iotlb_sync_map → [apple_dart_iotlb_sync_map]
 */
static int apple_dart_iotlb_sync_map(struct iommu_domain *domain,
				     unsigned long iova, size_t size)
{
	/* [한국어] 부정 캐시가 남아 새 매핑을 가리지 않도록 비운다. */
	apple_dart_domain_flush_tlb(to_dart_domain(domain));
	return 0;	/* [한국어] 미완성 도메인에는 반영할 매핑이 없다는 뜻이 아니라, 비우기가 항상 성공한다는 뜻이다. */
}

/*
 * [한국어]
 * apple_dart_iova_to_phys - IOVA를 물리 주소로 변환한다
 *
 * @domain: 대상 도메인.
 * @iova: 변환할 IOVA.
 * @return: 물리 주소, 매핑이 없거나 도메인이 미완성이면 0.
 *
 * io-pgtable에 그대로 넘긴다. pgtbl_ops가 NULL일 수 있는 것은
 * 도메인 완성이 attach까지 미뤄지기 때문이다 — 아직 아무도 붙지
 * 않은 도메인에는 테이블이 없다.
 *
 * 실행 컨텍스트: 조회 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 iova_to_phys → [apple_dart_iova_to_phys]
 */
static phys_addr_t apple_dart_iova_to_phys(struct iommu_domain *domain,
					   dma_addr_t iova)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	/* [한국어] 페이지 테이블 연산. */
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	/* [한국어] 아직 완성되지 않은 도메인이라 테이블이 없다. */
	if (!ops)
		return 0;

	/* [한국어] 실제 워크는 io-pgtable-dart.c가 한다. */
	return ops->iova_to_phys(ops, iova);
}

/*
 * [한국어]
 * apple_dart_map_pages - IOVA 범위에 물리 페이지를 매핑한다
 *
 * @domain: 대상 도메인.
 * @iova: 매핑할 IOVA.
 * @paddr: 물리 주소.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @prot: 권한.
 * @gfp: 할당 플래그.
 * @mapped: 매핑된 바이트 수를 돌려줄 곳.
 * @return: 0 성공, -ENODEV(미완성 도메인), io-pgtable의 오류.
 *
 * 이 드라이버는 테이블을 직접 만지지 않는다 — 전부 io-pgtable의
 * 일이다. 여기서 하는 유일한 판단은 "도메인이 완성되었는가"뿐이다.
 *
 * 실행 컨텍스트: DMA 매핑 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 map_pages → [apple_dart_map_pages] → io-pgtable
 */
static int apple_dart_map_pages(struct iommu_domain *domain, unsigned long iova,
				phys_addr_t paddr, size_t pgsize,
				size_t pgcount, int prot, gfp_t gfp,
				size_t *mapped)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	/* [한국어] 페이지 테이블 연산. */
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	/* [한국어] 아직 완성되지 않은 도메인에는 매핑할 수 없다. */
	if (!ops)
		return -ENODEV;

	/* [한국어] 실제 매핑은 io-pgtable-dart.c가 수행한다. */
	return ops->map_pages(ops, iova, paddr, pgsize, pgcount, prot, gfp,
			      mapped);
}

/*
 * [한국어]
 * apple_dart_unmap_pages - IOVA 범위의 매핑을 해제한다
 *
 * @domain: 대상 도메인.
 * @iova: 해제할 IOVA.
 * @pgsize: 페이지 크기.
 * @pgcount: 페이지 수.
 * @gather: 무효화 모으기(io-pgtable에 그대로 넘긴다).
 * @return: 해제된 바이트 수.
 *
 * map과 달리 ops가 NULL인지 검사하지 않는다. 매핑이 있었다면
 * 도메인은 이미 완성된 상태이므로, 해제 요청이 왔다는 것 자체가
 * ops의 존재를 함의한다.
 *
 * 실행 컨텍스트: DMA 해제 경로.
 *
 * 호출 체인:
 *   IOMMU 코어 unmap_pages → [apple_dart_unmap_pages] → io-pgtable
 */
static size_t apple_dart_unmap_pages(struct iommu_domain *domain,
				     unsigned long iova, size_t pgsize,
				     size_t pgcount,
				     struct iommu_iotlb_gather *gather)
{
	/* [한국어] 드라이버 쪽 도메인. */
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);
	/* [한국어] 페이지 테이블 연산. 해제 요청이 왔다면 반드시 있다. */
	struct io_pgtable_ops *ops = dart_domain->pgtbl_ops;

	/* [한국어] 실제 해제는 io-pgtable-dart.c가 수행한다. */
	return ops->unmap_pages(ops, iova, pgsize, pgcount, gather);
}

/*
 * [한국어]
 * apple_dart_setup_translation - 스트림들에 도메인의 테이블을 걸고 변환을 켠다
 *
 * @domain: 걸 도메인(완성된 상태여야 한다).
 * @stream_map: 대상 (DART, 스트림들).
 * @return: 없음.
 *
 * 이 드라이버에서 소프트웨어 도메인과 하드웨어가 실제로 연결되는
 * 지점이다. 세 단계다.
 *
 *  1) io-pgtable이 알려 준 TTBR들을 순서대로 심는다. T8020 계열은
 *     최상위 테이블이 네 조각으로 나뉘어 있어 n_ttbrs가 4까지 갈 수
 *     있고, T8110은 1이다.
 *  2) **남는 TTBR 자리를 지운다.** 이전 도메인이 더 많은 TTBR을
 *     썼다면 그 잔재가 남아 하드웨어가 엉뚱한 테이블을 읽을 수 있다.
 *  3) TCR로 변환을 켜고 TLB를 비운다. 비우기가 마지막인 이유는
 *     그 전에 하드웨어가 새 설정으로 캐시를 채울 수 있기 때문이다.
 *
 * 실행 컨텍스트: attach 경로. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   apple_dart_attach_dev_paging() → [apple_dart_setup_translation]
 *   → apple_dart_hw_set_ttbr() → apple_dart_hw_enable_translation()
 */
static void
apple_dart_setup_translation(struct apple_dart_domain *domain,
			     struct apple_dart_stream_map *stream_map)
{
	/* [한국어] TTBR 인덱스 순회 변수. 두 루프가 이어서 쓴다. */
	int i;
	/* [한국어] io-pgtable이 만든 설정. TTBR 주소들과 단계 수가 여기 있다. */
	struct io_pgtable_cfg *pgtbl_cfg =
		&io_pgtable_ops_to_pgtable(domain->pgtbl_ops)->cfg;

	/* [한국어] io-pgtable이 알려 준 만큼 TTBR을 심는다.
	 * T8020에서는 최상위 테이블이 네 조각이라 여러 개가 될 수 있다. */
	for (i = 0; i < pgtbl_cfg->apple_dart_cfg.n_ttbrs; ++i)
		apple_dart_hw_set_ttbr(stream_map, i,
				       pgtbl_cfg->apple_dart_cfg.ttbr[i]);
	/* [한국어] 남는 자리는 반드시 지운다 — 이전 도메인의 테이블
	 * 주소가 남아 있으면 하드웨어가 그것을 유효하게 여긴다.
	 * i가 앞 루프에서 이어지는 점에 유의. */
	for (; i < stream_map->dart->hw->ttbr_count; ++i)
		apple_dart_hw_clear_ttbr(stream_map, i);

	/* [한국어] 단계 수를 함께 알리며 변환을 켠다. */
	apple_dart_hw_enable_translation(stream_map,
					 pgtbl_cfg->apple_dart_cfg.n_levels);
	/* [한국어] 마지막으로 TLB를 비운다. 옛 설정으로 채워진 항목이
	 * 남아 있으면 새 테이블이 무시된다. */
	stream_map->dart->hw->invalidate_tlb(stream_map);
}

/*
 * [한국어]
 * apple_dart_finalize_domain - 도메인을 실제로 완성한다
 *
 * @dart_domain: 완성할 도메인.
 * @cfg: 이 도메인을 쓸 디바이스의 설정(어느 DART들인지 안다).
 * @return: 0 성공, -EINVAL(페이지가 너무 큼), -ENOMEM.
 *
 * 이 드라이버의 지연 초기화가 이뤄지는 곳이다. domain_alloc_paging은
 * 디바이스 없이도 불릴 수 있어 페이지 크기도 주소 폭도 모르는
 * 껍데기만 만든다. 실제 페이지 테이블은 첫 attach에서 디바이스가
 * 어느 DART에 속하는지 알게 된 뒤에야 만들 수 있다.
 *
 * finalized 플래그와 init_lock이 짝을 이뤄 "여러 디바이스가 같은
 * 도메인에 붙어도 테이블은 한 번만 만든다"를 보장한다.
 *
 * 페이지 크기가 커널 페이지보다 크면 거부하는 이유: 한 페이지만
 * 매핑하려 해도 그보다 넓은 영역이 열려, 인접한 커널 메모리가
 * 디바이스에 노출된다.
 *
 * coherent_walk = 1인 점도 중요하다. DART는 테이블을 캐시 일관적으로
 * 읽으므로, io-pgtable이 엔트리마다 캐시를 밀어낼 필요가 없다 —
 * rockchip 같은 드라이버와 대조적이다.
 *
 * 실행 컨텍스트: attach 또는 도메인 생성. 프로세스 컨텍스트
 * (뮤텍스와 잠들 수 있는 할당).
 *
 * 호출 체인:
 *   apple_dart_attach_dev_paging() / domain_alloc_paging()
 *   → [apple_dart_finalize_domain] → alloc_io_pgtable_ops()
 */
static int apple_dart_finalize_domain(struct apple_dart_domain *dart_domain,
				      struct apple_dart_master_cfg *cfg)
{
	/* [한국어] 대표 DART. 여러 DART에 걸쳐 있어도 페이지 크기와
	 * 주소 폭이 같음을 of_xlate가 보장하므로 첫 번째면 충분하다. */
	struct apple_dart *dart = cfg->stream_maps[0].dart;
	/* [한국어] io-pgtable에 넘길 설정. */
	struct io_pgtable_cfg pgtbl_cfg;
	/* [한국어] 결과 코드. 이미 완성된 경우 0으로 돌아간다. */
	int ret = 0;
	/* [한국어] DART 순회 인덱스와 비트맵 워드 인덱스. */
	int i, j;

	/* [한국어] 하드웨어 페이지가 커널 페이지보다 크면 세밀한 매핑이
	 * 불가능하다 — 이웃 메모리가 함께 노출되므로 거부한다. */
	if (dart->pgsize > PAGE_SIZE)
		return -EINVAL;

	/* [한국어] 완성 과정을 직렬화한다. 여러 디바이스가 같은 도메인에
	 * 동시에 붙을 수 있기 때문이다. */
	mutex_lock(&dart_domain->init_lock);

	/* [한국어] 이미 다른 디바이스가 완성해 두었다 — 테이블을 두 번
	 * 만들면 안 된다. */
	if (dart_domain->finalized)
		goto done;

	/* [한국어] 디바이스의 (DART, 스트림) 목록을 도메인으로 복사한다.
	 * 이 시점 이후 dart 포인터는 바뀌지 않으므로, 이후의 갱신은
	 * 비트맵에 대해서만 원자적으로 이뤄진다. */
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		dart_domain->stream_maps[i].dart = cfg->stream_maps[i].dart;	/* [한국어] 대상 DART를 도메인에 확정한다 — 이후 바뀌지 않는다. */
		for (j = 0; j < BITS_TO_LONGS(dart->num_streams); j++)	/* [한국어] 스트림 비트맵을 워드 단위로 옮긴다. */
			atomic_long_set(&dart_domain->stream_maps[i].sidmap[j],
					cfg->stream_maps[i].sidmap[j]);
	}

	/* [한국어] 하드웨어에서 알아낸 값들로 페이지 테이블 설정을 만든다. */
	pgtbl_cfg = (struct io_pgtable_cfg){
		.pgsize_bitmap = dart->pgsize,
		/* [한국어] 이 DART가 지원하는 단일 페이지 크기. */

		.ias = dart->ias,
		/* [한국어] IOVA 주소 폭 — 워크 단계 수를 여기서 결정한다. */

		.oas = dart->oas,
		/* [한국어] 물리 주소 폭 — 엔트리에 담을 수 있는 상한. */

		.coherent_walk = 1,
		/* [한국어] DART는 테이블을 캐시 일관적으로 읽는다.
		 * 그래서 io-pgtable이 엔트리마다 캐시를 밀어내지 않아도 된다. */

		.iommu_dev = dart->dev,
		/* [한국어] 테이블 페이지 할당의 기준 디바이스. */
	};

	/* [한국어] 세대에 맞는 포맷으로 페이지 테이블을 만든다.
	 * 돌아온 cfg에 TTBR 주소들과 단계 수가 채워진다. */
	dart_domain->pgtbl_ops = alloc_io_pgtable_ops(dart->hw->fmt, &pgtbl_cfg,
						      &dart_domain->domain);
	if (!dart_domain->pgtbl_ops) {	/* [한국어] 페이지 테이블을 만들지 못했다. */
		ret = -ENOMEM;	/* [한국어] 도메인을 완성할 수 없다. */
		goto done;	/* [한국어] finalized를 세우지 않은 채 락을 풀러 간다. */
	}

	/* [한국어] io-pgtable이 확정한 페이지 크기를 코어에 알린다. */
	dart_domain->domain.pgsize_bitmap = pgtbl_cfg.pgsize_bitmap;
	/* [한국어] IOVA 공간은 0부터 시작한다. */
	dart_domain->domain.geometry.aperture_start = 0;
	/* [한국어] 상한은 입력 주소 폭이 정한다. */
	dart_domain->domain.geometry.aperture_end =
		(dma_addr_t)DMA_BIT_MASK(pgtbl_cfg.ias);
	/* [한국어] 코어가 이 범위를 벗어난 IOVA를 주지 않게 강제한다. */
	dart_domain->domain.geometry.force_aperture = true;

	/* [한국어] 이제 다른 디바이스가 붙어도 다시 만들지 않는다. */
	dart_domain->finalized = true;

/* [한국어] 이미 완성됐거나 방금 완성했거나 실패했거나 — 모두 여기로. */
done:
	mutex_unlock(&dart_domain->init_lock);	/* [한국어] 완성 구간이 끝났으니 뮤텍스를 놓는다. */
	return ret;	/* [한국어] 완성 결과를 전한다 — 이미 완성돼 있었다면 0이다. */
}

/*
 * [한국어]
 * apple_dart_mod_streams - 도메인의 스트림 집합에 디바이스의 스트림을 더하거나 뺀다
 *
 * @domain_maps: 도메인 쪽 (원자적) 스트림 맵 배열.
 * @master_maps: 디바이스 쪽 스트림 맵 배열.
 * @add_streams: 참이면 더하고, 거짓이면 뺀다.
 * @return: 0 성공, -EINVAL(DART 구성이 다름).
 *
 * 먼저 두 배열의 DART 포인터가 완전히 일치하는지 확인한다.
 * 다르다면 그 디바이스는 이 도메인에 붙을 수 없다 — 도메인의
 * 페이지 테이블은 특정 DART 구성을 전제로 만들어졌기 때문이다.
 *
 * 비트 갱신을 atomic_long_or/and로 하는 이유: 같은 도메인에
 * 여러 디바이스가 병렬로 붙고 떨어질 수 있는데, 그때마다 락을
 * 잡는 대신 비트 연산의 원자성에 기대는 것이다.
 *
 * 실행 컨텍스트: attach 경로. 락을 잡지 않는다.
 *
 * 호출 체인:
 *   apple_dart_domain_add_streams() → [apple_dart_mod_streams]
 */
static int
apple_dart_mod_streams(struct apple_dart_atomic_stream_map *domain_maps,
		       struct apple_dart_stream_map *master_maps,
		       bool add_streams)
{
	/* [한국어] DART 순회 인덱스와 비트맵 워드 인덱스. */
	int i, j;

	/* [한국어] 도메인과 디바이스의 DART 구성이 정확히 같아야 한다.
	 * NULL 자리까지 포함해 전부 비교하므로, DART 개수가 달라도 걸린다. */
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (domain_maps[i].dart != master_maps[i].dart)	/* [한국어] DART 구성이 하나라도 다르면 이 도메인에 붙을 수 없다. */
			return -EINVAL;
	}

	/* [한국어] 실제로 존재하는 DART마다 비트를 갱신한다. */
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		/* [한국어] NULL을 만나면 배열의 끝이다. */
		if (!domain_maps[i].dart)
			break;
		/* [한국어] 비트맵을 워드 단위로 갱신한다. */
		for (j = 0; j < BITS_TO_LONGS(domain_maps[i].dart->num_streams); j++) {
			/* [한국어] 더하기: 디바이스의 스트림 비트를 켠다. */
			if (add_streams)
				atomic_long_or(master_maps[i].sidmap[j],
					       &domain_maps[i].sidmap[j]);
			else
				/* [한국어] 빼기: 그 비트들을 끈다.
				 * 현재 이 경로를 쓰는 호출자는 없다. */
				atomic_long_and(~master_maps[i].sidmap[j],
						&domain_maps[i].sidmap[j]);
		}
	}

	return 0;	/* [한국어] 스트림 집합 갱신이 끝났다. */
}

/*
 * [한국어]
 * apple_dart_domain_add_streams - 디바이스의 스트림을 도메인에 등록한다
 *
 * @domain: 대상 도메인.
 * @cfg: 디바이스 설정.
 * @return: 0 성공, -EINVAL.
 *
 * mod_streams에 "더하기"를 지정한 얇은 껍데기다. 이 등록이 있어야
 * 이후 무효화가 이 디바이스의 스트림에도 전달된다.
 *
 * 실행 컨텍스트: attach 경로.
 *
 * 호출 체인:
 *   apple_dart_attach_dev_paging() → [apple_dart_domain_add_streams]
 */
static int apple_dart_domain_add_streams(struct apple_dart_domain *domain,
					 struct apple_dart_master_cfg *cfg)
{
	/* [한국어] 디바이스의 스트림 비트를 도메인 집합에 더한다. */
	return apple_dart_mod_streams(domain->stream_maps, cfg->stream_maps,
				      true);
}

/*
 * [한국어]
 * apple_dart_attach_dev_paging - 디바이스를 페이징 도메인에 붙인다
 *
 * @domain: 붙일 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(쓰지 않는다).
 * @return: 0 성공, 음수 오류.
 *
 * 세 단계로 이뤄진다. 도메인을 완성하고(첫 attach라면 페이지 테이블이
 * 여기서 생긴다), 스트림 집합에 등록하고, 디바이스가 속한 모든
 * DART에 테이블을 건다.
 *
 * 이전 도메인에서 명시적으로 떼어 내는 단계가 없는 점에 유의.
 * TCR과 TTBR을 덮어쓰는 것이 곧 전환이기 때문이다 — 스트림마다
 * 상태가 독립적이라 중간에 어중간한 상태가 남지 않는다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev → [apple_dart_attach_dev_paging]
 *   → apple_dart_finalize_domain() → apple_dart_setup_translation()
 */
static int apple_dart_attach_dev_paging(struct iommu_domain *domain,
					struct device *dev,
					struct iommu_domain *old)
{
	/* [한국어] 단계별 결과와 DART 순회 인덱스. */
	int ret, i;
	/* [한국어] 순회 커서. */
	struct apple_dart_stream_map *stream_map;
	/* [한국어] 이 디바이스의 (DART, 스트림) 목록. */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	/* [한국어] 붙일 도메인. */
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);

	/* [한국어] 아직 완성되지 않았다면 여기서 페이지 테이블이 만들어진다. */
	ret = apple_dart_finalize_domain(dart_domain, cfg);
	if (ret)	/* [한국어] 도메인을 완성하지 못했다. */
		return ret;

	/* [한국어] 이 디바이스의 스트림들을 도메인의 무효화 대상에 넣는다. */
	ret = apple_dart_domain_add_streams(dart_domain, cfg);
	if (ret)	/* [한국어] 스트림 등록에 실패했다 — DART 구성이 도메인과 다르다. */
		return ret;

	/* [한국어] 디바이스가 속한 모든 DART에 같은 테이블을 건다.
	 * DMA가 어느 DART를 지날지 알 수 없으므로 전부 같아야 한다. */
	for_each_stream_map(i, cfg, stream_map)
		apple_dart_setup_translation(dart_domain, stream_map);
	return 0;	/* [한국어] 모든 DART에 테이블을 걸었다. */
}

/*
 * [한국어]
 * apple_dart_attach_dev_identity - 디바이스를 통과 모드로 만든다
 *
 * @domain: 정적 identity 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(쓰지 않는다).
 * @return: 0 성공, -EINVAL(우회를 지원하지 않음).
 *
 * TCR을 우회 값으로 쓰는 것이 전부다. 테이블도 스트림 등록도
 * 필요 없다 — 통과 모드에는 매핑이라는 개념이 없기 때문이다.
 *
 * cfg->supports_bypass는 이 디바이스가 속한 모든 DART의 능력을
 * AND 한 값이다. 하나라도 우회를 못 하면 전체가 불가능하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(identity) → [apple_dart_attach_dev_identity]
 *   → apple_dart_hw_enable_bypass()
 */
static int apple_dart_attach_dev_identity(struct iommu_domain *domain,
					  struct device *dev,
					  struct iommu_domain *old)
{
	/* [한국어] 이 디바이스의 DART 목록과 우회 가능 여부. */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	/* [한국어] 순회 커서. */
	struct apple_dart_stream_map *stream_map;
	/* [한국어] DART 순회 인덱스. */
	int i;

	/* [한국어] 속한 DART 중 하나라도 우회를 못 하면 통과 모드를
	 * 쓸 수 없다 — 그 DART를 지나는 DMA가 막히기 때문이다. */
	if (!cfg->supports_bypass)
		return -EINVAL;

	/* [한국어] 모든 DART의 해당 스트림을 우회로 설정한다. */
	for_each_stream_map(i, cfg, stream_map)
		apple_dart_hw_enable_bypass(stream_map);
	return 0;	/* [한국어] 모든 스트림을 우회로 돌렸다. */
}

/* [한국어] identity 도메인의 연산 테이블. 붙이기 하나뿐이다 —
 * 통과 모드에는 매핑도 무효화도 없다. */
static const struct iommu_domain_ops apple_dart_identity_ops = {
	.attach_dev = apple_dart_attach_dev_identity,
	/* [한국어] TCR을 우회 값으로 쓰는 콜백. */
};

/* [한국어] 시스템 전체가 공유하는 정적 통과 도메인.
 * 상태를 갖지 않으므로 인스턴스가 하나면 충분하다. */
static struct iommu_domain apple_dart_identity_domain = {
	.type = IOMMU_DOMAIN_IDENTITY,
	/* [한국어] 코어에 통과 모드임을 알리는 종류 표시. */

	.ops = &apple_dart_identity_ops,
	/* [한국어] 위의 연산 테이블. */
};

/*
 * [한국어]
 * apple_dart_attach_dev_blocked - 디바이스의 DMA를 전면 차단한다
 *
 * @domain: 정적 차단 도메인.
 * @dev: 대상 디바이스.
 * @old: 직전 도메인(쓰지 않는다).
 * @return: 항상 0.
 *
 * TCR을 tcr_disabled(0)로 쓰면 변환도 우회도 하지 않아 DMA가 막힌다.
 * identity와 달리 하드웨어 지원 여부를 확인할 필요가 없다 —
 * 차단은 어떤 DART에서든 가능하다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 attach_dev(blocked) → [apple_dart_attach_dev_blocked]
 *   → apple_dart_hw_disable_dma()
 */
static int apple_dart_attach_dev_blocked(struct iommu_domain *domain,
					 struct device *dev,
					 struct iommu_domain *old)
{
	/* [한국어] 이 디바이스의 DART 목록. */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	/* [한국어] 순회 커서. */
	struct apple_dart_stream_map *stream_map;
	/* [한국어] DART 순회 인덱스. */
	int i;

	/* [한국어] 모든 DART의 해당 스트림에서 DMA를 막는다.
	 * 어느 하나라도 열려 있으면 차단이 아니다. */
	for_each_stream_map(i, cfg, stream_map)
		apple_dart_hw_disable_dma(stream_map);
	return 0;	/* [한국어] 차단은 실패할 수 없다. */
}

/* [한국어] 차단 도메인의 연산 테이블. */
static const struct iommu_domain_ops apple_dart_blocked_ops = {
	.attach_dev = apple_dart_attach_dev_blocked,
	/* [한국어] TCR을 0으로 써 DMA를 막는 콜백. */
};

/* [한국어] 시스템 전체가 공유하는 정적 차단 도메인. */
static struct iommu_domain apple_dart_blocked_domain = {
	.type = IOMMU_DOMAIN_BLOCKED,
	/* [한국어] 코어에 차단 도메인임을 알리는 종류 표시. */

	.ops = &apple_dart_blocked_ops,
	/* [한국어] 위의 연산 테이블. */
};

/*
 * [한국어]
 * apple_dart_probe_device - 디바이스를 이 IOMMU에 등록한다
 *
 * @dev: 검사할 디바이스.
 * @return: 담당 iommu_device, 담당하지 않으면 ERR_PTR(-ENODEV).
 *
 * of_xlate가 설정을 만들어 두었는지로 담당 여부를 판단하고,
 * 속한 모든 DART와 전원 의존 링크를 만든다. 디바이스가 깨어날 때
 * 그 앞의 DART들이 모두 먼저 깨어나야 하기 때문이다.
 *
 * AUTOREMOVE_SUPPLIER는 DART가 사라지면 링크도 자동으로 정리하라는
 * 뜻이다. 그래서 release_device가 링크를 따로 지우지 않는다.
 *
 * 여러 DART에 속해 있어도 코어에는 첫 번째 것만 돌려준다.
 * 코어는 "이 디바이스를 담당하는 IOMMU 인스턴스" 하나만 알면 되고,
 * 나머지 DART는 이 드라이버가 cfg를 통해 알아서 다룬다.
 *
 * 실행 컨텍스트: 디바이스 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 probe_device → [apple_dart_probe_device]
 */
static struct iommu_device *apple_dart_probe_device(struct device *dev)
{
	/* [한국어] of_xlate가 만들어 둔 설정. */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	/* [한국어] 순회 커서. */
	struct apple_dart_stream_map *stream_map;
	/* [한국어] DART 순회 인덱스. */
	int i;

	/* [한국어] 설정이 없으면 디바이스 트리에 iommus 항목이 없었다는
	 * 뜻이라 우리 담당이 아니다. */
	if (!cfg)
		return ERR_PTR(-ENODEV);

	/* [한국어] 속한 모든 DART가 이 디바이스보다 먼저 깨어나도록
	 * 전원 의존을 건다. AUTOREMOVE_SUPPLIER 덕분에 DART가 사라지면
	 * 링크도 자동으로 정리된다. */
	for_each_stream_map(i, cfg, stream_map)
		device_link_add(
			dev, stream_map->dart->dev,
			DL_FLAG_PM_RUNTIME | DL_FLAG_AUTOREMOVE_SUPPLIER);

	/* [한국어] 코어에는 대표로 첫 DART의 인스턴스를 알린다.
	 * 나머지는 cfg를 통해 이 드라이버가 직접 다룬다. */
	return &cfg->stream_maps[0].dart->iommu;
}

/*
 * [한국어]
 * apple_dart_release_device - 디바이스의 설정을 해제한다
 *
 * @dev: 대상 디바이스.
 * @return: 없음.
 *
 * of_xlate가 만든 cfg를 해제한다. 디바이스 링크는
 * AUTOREMOVE_SUPPLIER로 자동 정리되므로 손대지 않는다.
 *
 * 실행 컨텍스트: 디바이스 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 release_device → [apple_dart_release_device]
 */
static void apple_dart_release_device(struct device *dev)
{
	/* [한국어] of_xlate가 만든 설정. */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);

	/* [한국어] 설정만 해제한다 — 링크는 코어가 자동으로 정리한다. */
	kfree(cfg);
}

/*
 * [한국어]
 * apple_dart_domain_alloc_paging - 페이징 도메인을 만든다
 *
 * @dev: 이 도메인을 쓸 디바이스. NULL일 수 있다.
 * @return: 새 도메인, 실패하면 NULL 또는 ERR_PTR.
 *
 * **dev가 NULL일 수 있다**는 것이 이 함수의 핵심이다. 코어가
 * 특정 디바이스와 무관하게 도메인을 미리 만들 수 있는데, 그러면
 * 페이지 크기도 주소 폭도 알 수 없어 테이블을 만들 수 없다.
 * 그래서 껍데기만 만들어 돌려주고, 실제 완성은 첫 attach로 미룬다.
 *
 * dev가 주어졌다면 지금 완성해 두는 것이 낫다 — 실패를 일찍
 * 알리고, attach 경로를 짧게 만든다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 domain_alloc_paging → [apple_dart_domain_alloc_paging]
 *   → apple_dart_finalize_domain()
 */
static struct iommu_domain *apple_dart_domain_alloc_paging(struct device *dev)
{
	/* [한국어] 만들 도메인. */
	struct apple_dart_domain *dart_domain;

	/* [한국어] 0으로 초기화해 받는다 — finalized가 거짓이고
	 * stream_maps의 dart가 모두 NULL인 상태로 시작한다. */
	dart_domain = kzalloc_obj(*dart_domain);
	if (!dart_domain)	/* [한국어] 도메인 껍데기를 잡지 못했다. */
		return NULL;

	/* [한국어] 완성 과정을 보호할 뮤텍스. */
	mutex_init(&dart_domain->init_lock);

	/* [한국어] 디바이스를 알면 지금 완성해 둔다. 모르면 껍데기인
	 * 채로 돌려주고 첫 attach가 완성한다. */
	if (dev) {
		/* [한국어] 그 디바이스의 DART 구성. */
		struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
		/* [한국어] 완성 결과. */
		int ret;

		ret = apple_dart_finalize_domain(dart_domain, cfg);	/* [한국어] 디바이스를 알므로 지금 완성해 둔다. */
		if (ret) {
			/* [한국어] 완성에 실패하면 껍데기도 남기지 않는다. */
			kfree(dart_domain);
			return ERR_PTR(ret);	/* [한국어] 완성 실패 이유를 오류 포인터로 전한다. */
		}
	}
	return &dart_domain->domain;	/* [한국어] 코어에는 임베드된 부분만 돌려준다. */
}

/*
 * [한국어]
 * apple_dart_domain_free - 도메인을 해제한다
 *
 * @domain: 해제할 도메인.
 * @return: 없음.
 *
 * 페이지 테이블은 io-pgtable이 정리한다. 미완성 도메인이라
 * pgtbl_ops가 NULL이어도 free_io_pgtable_ops가 안전하게 처리한다.
 *
 * 하드웨어를 건드리지 않는 점에 유의: 도메인이 해제된다는 것은
 * 이미 모든 디바이스가 다른 도메인으로 옮겨 갔다는 뜻이다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 free → [apple_dart_domain_free]
 */
static void apple_dart_domain_free(struct iommu_domain *domain)
{
	/* [한국어] 해제할 도메인. */
	struct apple_dart_domain *dart_domain = to_dart_domain(domain);

	/* [한국어] 페이지 테이블 전체를 io-pgtable이 정리한다.
	 * NULL이어도 안전하다. */
	free_io_pgtable_ops(dart_domain->pgtbl_ops);

	kfree(dart_domain);	/* [한국어] 도메인 구조체를 해제한다. */
}

/*
 * [한국어]
 * apple_dart_of_xlate - 디바이스 트리의 iommus 항목을 설정에 쌓는다
 *
 * @dev: 마스터 디바이스.
 * @args: "iommus = <&dart SID>"에서 파싱된 인자.
 * @return: 0 성공, -EINVAL/-ENOMEM.
 *
 * 디바이스 트리 항목마다 한 번씩 불리므로, **여러 번 불려도
 * 누적되도록** 만들어져 있다. 이미 설정이 있으면 그것에 더하고,
 * 없으면 새로 만든다.
 *
 * 하는 일이 셋이다.
 *  1) 같은 디바이스에 묶이는 DART들이 페이지 크기와 주소 폭이
 *     같은지 확인한다. 다르면 하나의 페이지 테이블로 둘을 다룰 수
 *     없다.
 *  2) 우회 가능 여부를 AND로 누적한다.
 *  3) 이미 등록된 DART면 그 비트맵에 SID를 더하고, 처음 보는
 *     DART면 빈 자리를 찾아 등록한다.
 *
 * 실행 컨텍스트: 디바이스 트리 파싱. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 of_xlate → [apple_dart_of_xlate]
 */
static int apple_dart_of_xlate(struct device *dev,
			       const struct of_phandle_args *args)
{
	/* [한국어] 이미 쌓아 둔 설정(첫 호출이면 NULL). */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	/* [한국어] 참조가 가리키는 DART의 플랫폼 디바이스. */
	struct platform_device *iommu_pdev = of_find_device_by_node(args->np);
	/* [한국어] 그 DART의 드라이버 상태. */
	struct apple_dart *dart = platform_get_drvdata(iommu_pdev);
	/* [한국어] 이미 등록된 DART(호환성 검사용). */
	struct apple_dart *cfg_dart;
	/* [한국어] 순회 인덱스와, 이번에 등록할 스트림 번호. */
	int i, sid;

	/* [한국어] 찾기가 올린 참조를 곧바로 놓는다 — DART의 수명은
	 * 디바이스 링크가 보장한다. */
	put_device(&iommu_pdev->dev);

	/* [한국어] DART 참조는 스트림 번호 하나를 인자로 받는다. */
	if (args->args_count != 1)
		return -EINVAL;
	sid = args->args[0];

	/* [한국어] 첫 호출이면 설정을 새로 만든다. */
	if (!cfg) {
		cfg = kzalloc_obj(*cfg);	/* [한국어] 첫 호출이므로 설정을 새로 만든다. */
		if (!cfg)	/* [한국어] 설정을 잡지 못했다. */
			return -ENOMEM;
		/* Will be ANDed with DART capabilities */
		/* [한국어] 참으로 시작해 DART마다 AND 한다 — 교집합을
		 * 구하는 관용구다. */
		cfg->supports_bypass = true;
	}
	/* [한국어] 매번 설정해도 무해하다(같은 포인터). */
	dev_iommu_priv_set(dev, cfg);

	/* [한국어] 이미 등록된 DART가 있으면 새 DART와 호환되는지 본다. */
	cfg_dart = cfg->stream_maps[0].dart;
	if (cfg_dart) {
		/* [한국어] 페이지 크기가 다르면 하나의 페이지 테이블로
		 * 둘을 다룰 수 없다. */
		if (cfg_dart->pgsize != dart->pgsize)
			return -EINVAL;
		/* [한국어] 입력 주소 폭이 다르면 워크 단계 수가 달라진다. */
		if (cfg_dart->ias != dart->ias)
			return -EINVAL;
	}

	/* [한국어] 우회 가능 여부는 모든 DART의 교집합이다. */
	cfg->supports_bypass &= dart->supports_bypass;

	/* [한국어] 이미 등록된 DART라면 비트만 더하면 된다 —
	 * 한 디바이스가 같은 DART의 여러 스트림을 쓸 수 있다. */
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (cfg->stream_maps[i].dart == dart) {	/* [한국어] 이미 등록된 DART를 다시 만났다. */
			set_bit(sid, cfg->stream_maps[i].sidmap);	/* [한국어] 그 DART의 비트맵에 이번 스트림을 더한다. */
			return 0;	/* [한국어] 이 항목의 등록이 끝났다. */
		}
	}
	/* [한국어] 처음 보는 DART라면 빈 자리를 찾아 등록한다. */
	for (i = 0; i < MAX_DARTS_PER_DEVICE; ++i) {
		if (!cfg->stream_maps[i].dart) {	/* [한국어] 아직 쓰이지 않은 자리를 찾았다. */
			cfg->stream_maps[i].dart = dart;	/* [한국어] 그 자리에 이 DART를 등록한다. */
			set_bit(sid, cfg->stream_maps[i].sidmap);	/* [한국어] 첫 스트림 비트를 세운다. */
			return 0;	/* [한국어] 새 DART의 등록이 끝났다. */
		}
	}

	/* [한국어] 빈 자리가 없다 — 이 디바이스가 3개를 넘는 DART에
	 * 속한다는 뜻으로, 현재 하드웨어에는 없는 구성이다. */
	return -EINVAL;
}

/* [한국어] IOMMU 그룹 조작 전체를 직렬화하는 전역 뮤텍스.
 * sid2group 표를 여러 DART에 걸쳐 일관되게 갱신해야 해서,
 * DART별 락이 아니라 전역 하나를 쓴다. */
static DEFINE_MUTEX(apple_dart_groups_lock);

/*
 * [한국어]
 * apple_dart_release_group - 그룹이 사라질 때 sid2group 표를 정리한다
 *
 * @iommu_data: 그룹에 매달아 둔 master_cfg 사본.
 * @return: 없음.
 *
 * 그룹의 참조 계수가 0이 되면 코어가 부른다. 그 그룹이 차지하고
 * 있던 스트림들을 표에서 지워, 나중에 같은 스트림이 새 그룹에
 * 배정될 수 있게 한다. 지우지 않으면 해제된 그룹을 가리키는
 * 포인터가 남아 use-after-free가 된다.
 *
 * 실행 컨텍스트: 그룹 해제. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어(그룹 참조 계수 0) → [apple_dart_release_group]
 */
static void apple_dart_release_group(void *iommu_data)
{
	/* [한국어] DART 순회 인덱스와 스트림 번호. */
	int i, sid;
	/* [한국어] 순회 커서. */
	struct apple_dart_stream_map *stream_map;
	/* [한국어] 그룹에 매달려 있던 설정 사본. */
	struct apple_dart_master_cfg *group_master_cfg = iommu_data;

	/* [한국어] sid2group 표를 만지는 동안 다른 그룹 조작을 막는다. */
	mutex_lock(&apple_dart_groups_lock);

	/* [한국어] 이 그룹이 차지하던 모든 스트림을 표에서 지운다.
	 * 그러지 않으면 해제된 그룹을 가리키는 포인터가 남는다. */
	for_each_stream_map(i, group_master_cfg, stream_map)
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams)
			stream_map->dart->sid2group[sid] = NULL;

	/* [한국어] 설정 사본을 해제한다. */
	kfree(iommu_data);
	mutex_unlock(&apple_dart_groups_lock);	/* [한국어] 표 정리가 끝났으니 뮤텍스를 놓는다. */
}

/*
 * [한국어]
 * apple_dart_merge_master_cfg - 두 설정의 스트림 집합을 합친다
 *
 * @dst: 그룹에 이미 매달려 있는 설정(여기에 합친다).
 * @src: 새로 합류하는 디바이스의 설정.
 * @return: 0 성공, -EINVAL(구성이 가정을 벗어남).
 *
 * PCI 디바이스가 같은 그룹으로 묶일 때 쓰인다. 그룹 하나가
 * 여러 디바이스의 스트림을 모두 알아야, release_group이 표를
 * 빠짐없이 정리할 수 있기 때문이다.
 *
 * 원본 주석이 밝히듯 **단일 DART만 가정한다.** Apple Silicon에서
 * 같은 PCIe 버스의 디바이스들이 여러 DART에 흩어지는 구성이
 * 없기 때문이며, 그 가정이 깨지면 세 검사가 잡아낸다.
 *
 * 실행 컨텍스트: 그룹 결정. groups_lock을 잡은 상태.
 *
 * 호출 체인:
 *   apple_dart_device_group() → [apple_dart_merge_master_cfg]
 */
static int apple_dart_merge_master_cfg(struct apple_dart_master_cfg *dst,
				       struct apple_dart_master_cfg *src)
{
	/*
	 * We know that this function is only called for groups returned from
	 * pci_device_group and that all Apple Silicon platforms never spread
	 * PCIe devices from the same bus across multiple DARTs such that we can
	 * just assume that both src and dst only have the same single DART.
	 */
	/* [한국어] 새 디바이스가 두 번째 DART를 갖는다면 가정 밖이다. */
	if (src->stream_maps[1].dart)
		return -EINVAL;
	/* [한국어] 기존 그룹도 마찬가지다. */
	if (dst->stream_maps[1].dart)
		return -EINVAL;
	/* [한국어] 둘의 DART가 다르면 스트림 비트맵을 합칠 수 없다 —
	 * 같은 번호라도 서로 다른 하드웨어의 스트림이기 때문이다. */
	if (src->stream_maps[0].dart != dst->stream_maps[0].dart)
		return -EINVAL;

	/* [한국어] 두 스트림 집합의 합집합을 그룹의 설정에 남긴다. */
	bitmap_or(dst->stream_maps[0].sidmap,
		  dst->stream_maps[0].sidmap,
		  src->stream_maps[0].sidmap,
		  dst->stream_maps[0].dart->num_streams);
	return 0;	/* [한국어] 두 스트림 집합을 합쳤다. */
}

/*
 * [한국어]
 * apple_dart_device_group - 이 디바이스가 속할 IOMMU 그룹을 정한다
 *
 * @dev: 대상 디바이스.
 * @return: 그룹, 실패하면 ERR_PTR.
 *
 * DART에서 격리의 단위는 스트림이다. 따라서 **같은 스트림을 쓰는
 * 디바이스들은 서로 격리할 수 없어 반드시 한 그룹**이어야 한다.
 * sid2group 표가 그 판단의 근거다.
 *
 * 흐름은 이렇다.
 *  1) 이 디바이스의 스트림들이 이미 어느 그룹에 속해 있는지 본다.
 *     서로 다른 그룹에 흩어져 있다면 모순이므로 오류다.
 *  2) 이미 그룹이 있으면 그것을 참조해 돌려준다.
 *  3) 없으면 새로 만든다. PCI는 ACS와 토폴로지를 따져야 해
 *     전용 함수를 쓴다.
 *  4) 그룹에 이 디바이스의 설정 사본을 매달거나, 이미 있으면 합친다.
 *     release_group이 표를 정리할 때 이 사본을 근거로 삼는다.
 *  5) 마지막으로 표를 갱신한다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 device_group → [apple_dart_device_group]
 *   → pci_device_group() / generic_device_group()
 */
static struct iommu_group *apple_dart_device_group(struct device *dev)
{
	/* [한국어] DART 순회 인덱스와 스트림 번호. */
	int i, sid;
	/* [한국어] 이 디바이스의 (DART, 스트림) 목록. */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);
	/* [한국어] 순회 커서. */
	struct apple_dart_stream_map *stream_map;
	/* [한국어] 그룹에 매달 설정 사본. */
	struct apple_dart_master_cfg *group_master_cfg;
	/* [한국어] 이 디바이스의 스트림들이 이미 속한 그룹. */
	struct iommu_group *group = NULL;
	/* [한국어] 최종 결과. 오류로 시작해 성공 시 덮어쓴다. */
	struct iommu_group *res = ERR_PTR(-EINVAL);

	/* [한국어] 표를 읽고 쓰는 전 구간을 잠근다. */
	mutex_lock(&apple_dart_groups_lock);

	/* [한국어] 이 디바이스가 쓰는 모든 스트림을 훑으며, 이미
	 * 배정된 그룹이 있는지 본다. */
	for_each_stream_map(i, cfg, stream_map) {
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams) {
			/* [한국어] 이 스트림이 속한 그룹(없으면 NULL). */
			struct iommu_group *stream_group =
				stream_map->dart->sid2group[sid];

			/* [한국어] 한 디바이스의 스트림들이 서로 다른 그룹에
			 * 흩어져 있다면 격리 관계가 모순이다 — 있을 수 없다. */
			if (group && group != stream_group) {
				res = ERR_PTR(-EINVAL);	/* [한국어] 한 디바이스의 스트림이 서로 다른 그룹에 흩어져 있다 — 모순이다. */
				goto out;	/* [한국어] 락을 풀고 나간다. */
			}

			group = stream_group;	/* [한국어] 지금까지 본 그룹으로 기억해 둔다(첫 스트림이면 NULL일 수 있다). */
		}
	}

	/* [한국어] 이미 그룹이 있으면 참조를 올려 그대로 쓴다.
	 * 같은 스트림을 쓰는 다른 디바이스가 먼저 만든 것이다. */
	if (group) {
		res = iommu_group_ref_get(group);	/* [한국어] 그룹 참조를 하나 올려 호출자에게 넘긴다. */
		goto out;	/* [한국어] 표는 이미 이 그룹을 가리키므로 갱신할 것이 없다. */
	}

#ifdef CONFIG_PCI
	/* [한국어] PCI는 ACS 능력과 브리지 구조에 따라 여러 함수가 한
	 * 그룹으로 묶일 수 있어 전용 판별이 필요하다. */
	if (dev_is_pci(dev))
		group = pci_device_group(dev);
	else
#endif
		/* [한국어] 그 밖에는 디바이스마다 독립 그룹으로 둔다. */
		group = generic_device_group(dev);

	/* [한국어] 아래 실패 경로들이 이 값을 쓰도록 미리 설정한다. */
	res = ERR_PTR(-ENOMEM);
	if (!group)	/* [한국어] 그룹을 만들지 못했다. */
		goto out;

	/* [한국어] 그룹에 이미 다른 디바이스의 설정이 매달려 있는지 본다. */
	group_master_cfg = iommu_group_get_iommudata(group);
	if (group_master_cfg) {
		/* [한국어] 합치기 결과. */
		int ret;

		/* [한국어] 이 디바이스의 스트림을 그룹의 집합에 더한다.
		 * PCI가 우리와 무관한 이유로 두 디바이스를 묶은 경우다. */
		ret = apple_dart_merge_master_cfg(group_master_cfg, cfg);
		if (ret) {
			/* [한국어] 가정을 벗어난 구성이다 — 그룹을 놓고 나간다. */
			dev_err(dev, "Failed to merge DART IOMMU groups.\n");
			iommu_group_put(group);	/* [한국어] 합칠 수 없으므로 방금 얻은 그룹 참조를 놓는다. */
			res = ERR_PTR(ret);	/* [한국어] 합치기 실패 이유를 전한다. */
			goto out;	/* [한국어] 락을 풀고 나간다. */
		}
	} else {
		/* [한국어] 이 그룹의 첫 디바이스다 — 설정을 복사해 매단다.
		 * 사본을 쓰는 이유: 디바이스가 사라져도 그룹은 남을 수 있고,
		 * release_group이 그때 표를 정리해야 하기 때문이다. */
		group_master_cfg = kmemdup(cfg, sizeof(*group_master_cfg),
					   GFP_KERNEL);
		if (!group_master_cfg) {	/* [한국어] 설정 사본을 만들지 못했다. */
			iommu_group_put(group);	/* [한국어] 그룹 참조를 놓는다. */
			goto out;	/* [한국어] 미리 설정해 둔 -ENOMEM으로 나간다. */
		}

		/* [한국어] 사본과 함께 해제 콜백을 등록한다. */
		iommu_group_set_iommudata(group, group_master_cfg,
			apple_dart_release_group);
	}

	/* [한국어] 이 디바이스의 모든 스트림을 이 그룹으로 표시한다.
	 * 이후 같은 스트림을 쓰는 디바이스가 오면 위의 검색이
	 * 이 그룹을 찾아낸다. */
	for_each_stream_map(i, cfg, stream_map)
		for_each_set_bit(sid, stream_map->sidmap, stream_map->dart->num_streams)
			stream_map->dart->sid2group[sid] = group;

	res = group;

/* [한국어] 성공/실패 공통 정리 지점. */
out:
	mutex_unlock(&apple_dart_groups_lock);	/* [한국어] 표 갱신이 끝났으니 뮤텍스를 놓는다. */
	return res;	/* [한국어] 그룹 또는 오류를 코어에 돌려준다. */
}

/*
 * [한국어]
 * apple_dart_def_domain_type - 이 디바이스의 기본 도메인 종류를 정한다
 *
 * @dev: 대상 디바이스.
 * @return: 강제할 도메인 종류, 또는 0(코어의 기본을 따름).
 *
 * 하드웨어 제약을 도메인 정책으로 옮기는 함수다. 두 가지 강제가 있다.
 *
 *  - 페이지 크기가 커널 페이지보다 크면 통과 모드를 강제한다.
 *    세밀한 매핑이 불가능해 격리가 성립하지 않으므로, 어설프게
 *    매핑하느니 통과시키는 편이 낫다는 판단이다.
 *  - 우회를 지원하지 않으면 DMA 도메인을 강제한다. 통과 모드로
 *    갈 수 없으니 반드시 변환을 써야 한다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후.
 *
 * 호출 체인:
 *   IOMMU 코어 def_domain_type → [apple_dart_def_domain_type]
 */
static int apple_dart_def_domain_type(struct device *dev)
{
	/* [한국어] 이 디바이스의 DART 구성. */
	struct apple_dart_master_cfg *cfg = dev_iommu_priv_get(dev);

	/* [한국어] 하드웨어 페이지가 커널 페이지보다 크면 세밀한 매핑이
	 * 불가능하다 — 격리를 흉내 내느니 통과가 정직하다. */
	if (cfg->stream_maps[0].dart->pgsize > PAGE_SIZE)
		return IOMMU_DOMAIN_IDENTITY;
	/* [한국어] 우회를 못 하는 하드웨어라면 변환을 쓸 수밖에 없다. */
	if (!cfg->supports_bypass)
		return IOMMU_DOMAIN_DMA;

	/* [한국어] 제약이 없으면 코어의 기본 정책을 따른다. */
	return 0;
}

/* [한국어] Apple PCIe의 MSI 도어벨 주소. CONFIG_PCI_APPLE이 꺼져 있으면
 * 이 상수가 정의되지 않아 컴파일이 깨지므로 0으로 채워 둔다.
 * 원본 주석이 밝히듯 순전히 빌드를 위한 장치다. */
#ifndef CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR
/* Keep things compiling when CONFIG_PCI_APPLE isn't selected */
#define CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR	0
#endif
/* [한국어] 그 도어벨 주소를 페이지 경계로 내린 값.
 * 예약 영역은 페이지 단위여야 하기 때문이다. */
#define DOORBELL_ADDR	(CONFIG_PCIE_APPLE_MSI_DOORBELL_ADDR & PAGE_MASK)

/*
 * [한국어]
 * apple_dart_get_resv_regions - 이 디바이스의 예약 영역을 알린다
 *
 * @dev: 대상 디바이스.
 * @head: 예약 영역을 매달 목록.
 * @return: 없음.
 *
 * PCI 디바이스에게는 MSI 도어벨이 놓인 페이지를 알려 준다.
 * Apple의 PCIe 컨트롤러는 도어벨 주소가 컴파일 시 고정이라,
 * 다른 드라이버처럼 하드웨어에 물어볼 필요 없이 상수를 쓴다.
 *
 * IOMMU_RESV_MSI(소프트웨어 MSI가 아니라)인 점에 유의 — 그 주소가
 * 이미 정해져 있으니 커널이 임의로 고를 자리가 아니라는 뜻이다.
 *
 * 실행 컨텍스트: 디바이스 probe 이후. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   IOMMU 코어 get_resv_regions → [apple_dart_get_resv_regions]
 *   → iommu_dma_get_resv_regions()
 */
static void apple_dart_get_resv_regions(struct device *dev,
					struct list_head *head)
{
	/* [한국어] Apple PCIe 컨트롤러가 있는 시스템의 PCI 디바이스만
	 * 해당한다. 그 밖에는 도어벨 예약이 필요 없다. */
	if (IS_ENABLED(CONFIG_PCIE_APPLE) && dev_is_pci(dev)) {
		/* [한국어] 만들 예약 영역. */
		struct iommu_resv_region *region;
		/* [한국어] 도어벨의 권한 — 쓰기만 필요하고 실행은 막으며 MMIO다. */
		int prot = IOMMU_WRITE | IOMMU_NOEXEC | IOMMU_MMIO;

		/* [한국어] 고정된 도어벨 주소의 한 페이지를 MSI 영역으로
		 * 예약한다. 주소가 이미 정해져 있으므로 SW_MSI가 아니다. */
		region = iommu_alloc_resv_region(DOORBELL_ADDR,
						 PAGE_SIZE, prot,
						 IOMMU_RESV_MSI, GFP_KERNEL);
		if (!region)	/* [한국어] 예약 영역을 만들지 못했다. */
			return;

		list_add_tail(&region->list, head);	/* [한국어] 코어의 목록에 매단다. */
	}

	/* [한국어] DMA 계층이 요구하는 예약 영역도 덧붙인다. */
	iommu_dma_get_resv_regions(dev, head);
}

/* [한국어] 이 드라이버가 IOMMU 코어에 제공하는 연산 테이블.
 * 통과와 차단 도메인을 모두 정적으로 제공하는 점이 특징인데,
 * 둘 다 상태 없이 TCR 값 하나로 표현되기 때문이다. */
static const struct iommu_ops apple_dart_iommu_ops = {
	.identity_domain = &apple_dart_identity_domain,
	/* [한국어] 통과 모드용 정적 도메인. TCR을 우회 값으로 쓴다. */

	.blocked_domain = &apple_dart_blocked_domain,
	/* [한국어] 차단용 정적 도메인. TCR을 0으로 쓴다. */

	.domain_alloc_paging = apple_dart_domain_alloc_paging,
	/* [한국어] 페이징 도메인 생성. 디바이스를 모르면 껍데기만 만든다. */

	.probe_device = apple_dart_probe_device,
	/* [한국어] 디바이스 등록. 속한 모든 DART와 전원 링크를 만든다. */

	.release_device = apple_dart_release_device,
	/* [한국어] 디바이스 제거. 설정을 해제한다. */

	.device_group = apple_dart_device_group,
	/* [한국어] 스트림을 공유하는 디바이스를 한 그룹으로 묶는다. */

	.of_xlate = apple_dart_of_xlate,
	/* [한국어] 디바이스 트리의 (DART, SID)를 설정에 쌓는다. */

	.def_domain_type = apple_dart_def_domain_type,
	/* [한국어] 하드웨어 제약을 도메인 정책으로 옮긴다. */

	.get_resv_regions = apple_dart_get_resv_regions,
	/* [한국어] MSI 도어벨 페이지를 예약으로 알린다. */

	.owner = THIS_MODULE,
	/* [한국어] 모듈 참조 계수의 주인. */

	.default_domain_ops = &(const struct iommu_domain_ops) {
		.attach_dev	= apple_dart_attach_dev_paging,
		/* [한국어] 붙이기. 도메인 완성과 TTBR/TCR 설정을 한다. */

		.map_pages	= apple_dart_map_pages,
		/* [한국어] 매핑. io-pgtable에 그대로 위임한다. */

		.unmap_pages	= apple_dart_unmap_pages,
		/* [한국어] 해제. 역시 io-pgtable에 위임한다. */

		.flush_iotlb_all = apple_dart_flush_iotlb_all,
		/* [한국어] 전체 무효화. 스트림 단위 비우기로 구현된다. */

		.iotlb_sync	= apple_dart_iotlb_sync,
		/* [한국어] 해제 후 무효화. 범위를 좁힐 수 없어 전체와 같다. */

		.iotlb_sync_map	= apple_dart_iotlb_sync_map,
		/* [한국어] 매핑 후 무효화. 부정 캐시를 지우기 위해 필요하다. */

		.iova_to_phys	= apple_dart_iova_to_phys,
		/* [한국어] 조회. io-pgtable에 위임한다. */

		.free		= apple_dart_domain_free,
		/* [한국어] 도메인 해제. 페이지 테이블을 정리한다. */
	}
};

/*
 * [한국어]
 * apple_dart_t8020_irq - T8020/T6000 계열의 폴트 인터럽트 핸들러
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev: 등록 시 넘긴 apple_dart.
 * @return: 우리 오류였으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * 인터럽트를 여러 DART가 공유할 수 있으므로(IRQF_SHARED),
 * 오류 플래그가 서 있을 때만 자기 것으로 인정한다.
 *
 * 오류 코드를 == 로 비교하는 점이 눈에 띈다. 원본 주석이 밝히듯
 * 원래 비트 하나만 서지만, 확실히 하기 위해 정확히 그 값일 때만
 * 이름을 붙인다. 여러 비트가 동시에 서면 "unknown"이 되어
 * 원시 값을 그대로 보게 되는데, 그 편이 잘못된 이름보다 낫다.
 *
 * 오류 코드는 워크가 어느 단계에서 끊겼는지를 알려 준다:
 * NO_TTBR이면 스트림에 테이블이 걸리지 않은 것(attach 누락),
 * NO_PMD/NO_PTE면 매핑되지 않은 주소, READ/WRITE FAULT면 권한 문제다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [apple_dart_t8020_irq]
 */
static irqreturn_t apple_dart_t8020_irq(int irq, void *dev)
{
	/* [한국어] 등록 시 넘긴 DART. */
	struct apple_dart *dart = dev;
	/* [한국어] 오류 코드를 옮길 사람이 읽을 이름. */
	const char *fault_name = NULL;
	/* [한국어] 오류 상태 레지스터 값. */
	u32 error = readl(dart->regs + DART_T8020_ERROR);
	/* [한국어] 그중 원인 코드 부분. */
	u32 error_code = FIELD_GET(DART_T8020_ERROR_CODE, error);
	/* [한국어] 오류가 난 주소의 하위 절반. */
	u32 addr_lo = readl(dart->regs + DART_T8020_ERROR_ADDR_LO);
	/* [한국어] 상위 절반. */
	u32 addr_hi = readl(dart->regs + DART_T8020_ERROR_ADDR_HI);
	/* [한국어] 둘을 합쳐 64비트 주소를 만든다. */
	u64 addr = addr_lo | (((u64)addr_hi) << 32);
	/* [한국어] 오류를 낸 스트림의 번호. */
	u8 stream_idx = FIELD_GET(DART_T8020_ERROR_STREAM, error);

	/* [한국어] 유효한 오류가 없다 — 인터럽트를 공유하는 다른
	 * DART의 것이다. */
	if (!(error & DART_T8020_ERROR_FLAG))
		return IRQ_NONE;

	/* there should only be a single bit set but let's use == to be sure */
	/* [한국어] 읽기 권한이 없는 곳을 읽으려 했다. */
	if (error_code == DART_T8020_ERROR_READ_FAULT)
		fault_name = "READ FAULT";
	/* [한국어] 쓰기 권한이 없는 곳에 쓰려 했다. */
	else if (error_code == DART_T8020_ERROR_WRITE_FAULT)
		fault_name = "WRITE FAULT";
	/* [한국어] 말단 엔트리가 없다 — 매핑되지 않은 페이지. */
	else if (error_code == DART_T8020_ERROR_NO_PTE)
		fault_name = "NO PTE FOR IOVA";
	/* [한국어] 중간 엔트리가 없다 — 더 큰 영역이 통째로 비어 있다. */
	else if (error_code == DART_T8020_ERROR_NO_PMD)
		fault_name = "NO PMD FOR IOVA";
	/* [한국어] TTBR이 유효하지 않다 — 이 스트림에 도메인이 붙지
	 * 않았다는 뜻으로, 보통 attach 누락이다. */
	else if (error_code == DART_T8020_ERROR_NO_TTBR)
		fault_name = "NO TTBR FOR IOVA";
	else
		/* [한국어] 여러 비트가 동시에 섰거나 모르는 코드다.
		 * 잘못된 이름을 붙이느니 원시 값을 그대로 보이는 편이 낫다. */
		fault_name = "unknown";

	/* [한국어] 잘못된 드라이버가 폴트를 쏟아낼 수 있어 로그를 제한한다.
	 * 원시 값과 해석을 모두 남겨 두 방향의 진단이 가능하게 한다. */
	dev_err_ratelimited(
		dart->dev,
		"translation fault: status:0x%x stream:%d code:0x%x (%s) at 0x%llx",
		error, stream_idx, error_code, fault_name, addr);

	/* [한국어] 읽은 값을 되써 오류를 지우고 인터럽트를 다시 무장시킨다
	 * (write-1-to-clear). 지우지 않으면 같은 인터럽트가 반복된다. */
	writel(error, dart->regs + DART_T8020_ERROR);
	return IRQ_HANDLED;	/* [한국어] 우리 DART의 오류였음을 커널에 알린다. */
}

/*
 * [한국어]
 * apple_dart_t8110_irq - T8110의 폴트 인터럽트 핸들러
 *
 * @irq: 인터럽트 번호(쓰지 않는다).
 * @dev: 등록 시 넘긴 apple_dart.
 * @return: 우리 오류였으면 IRQ_HANDLED, 아니면 IRQ_NONE.
 *
 * T8020 판과 구조가 같고 두 가지가 다르다. 4단계 워크를 지원하므로
 * NO_PGD 코드가 하나 더 있고, 마지막에 **오류 스트림 비트맵도
 * 지워야 한다.** T8110은 오류 상태와 별개로 어느 스트림들이
 * 오류를 냈는지를 따로 기록하기 때문이다.
 *
 * 실행 컨텍스트: 인터럽트 문맥.
 *
 * 호출 체인:
 *   커널 인터럽트 코어 → [apple_dart_t8110_irq]
 */
static irqreturn_t apple_dart_t8110_irq(int irq, void *dev)
{
	/* [한국어] 등록 시 넘긴 DART. */
	struct apple_dart *dart = dev;
	/* [한국어] 오류 코드의 사람이 읽을 이름. */
	const char *fault_name = NULL;
	/* [한국어] 오류 상태 레지스터 값. */
	u32 error = readl(dart->regs + DART_T8110_ERROR);
	/* [한국어] 그중 원인 코드 부분. */
	u32 error_code = FIELD_GET(DART_T8110_ERROR_CODE, error);
	/* [한국어] 오류 주소의 하위 절반. */
	u32 addr_lo = readl(dart->regs + DART_T8110_ERROR_ADDR_LO);
	/* [한국어] 상위 절반. */
	u32 addr_hi = readl(dart->regs + DART_T8110_ERROR_ADDR_HI);
	/* [한국어] 합쳐 만든 64비트 주소. */
	u64 addr = addr_lo | (((u64)addr_hi) << 32);
	/* [한국어] 오류를 낸 스트림 번호. */
	u8 stream_idx = FIELD_GET(DART_T8110_ERROR_STREAM, error);

	/* [한국어] 유효한 오류가 없으면 남의 인터럽트다. */
	if (!(error & DART_T8110_ERROR_FLAG))
		return IRQ_NONE;

	/* there should only be a single bit set but let's use == to be sure */
	/* [한국어] 읽기 권한 위반. */
	if (error_code == DART_T8110_ERROR_READ_FAULT)
		fault_name = "READ FAULT";
	/* [한국어] 쓰기 권한 위반. */
	else if (error_code == DART_T8110_ERROR_WRITE_FAULT)
		fault_name = "WRITE FAULT";
	/* [한국어] 말단 엔트리 없음. */
	else if (error_code == DART_T8110_ERROR_NO_PTE)
		fault_name = "NO PTE FOR IOVA";
	/* [한국어] 중간 엔트리 없음. */
	else if (error_code == DART_T8110_ERROR_NO_PMD)
		fault_name = "NO PMD FOR IOVA";
	/* [한국어] 최상위 엔트리 없음 — 4단계 워크를 하는 T8110에만
	 * 있는 코드다. */
	else if (error_code == DART_T8110_ERROR_NO_PGD)
		fault_name = "NO PGD FOR IOVA";
	/* [한국어] TTBR이 유효하지 않음 — attach 누락의 신호. */
	else if (error_code == DART_T8110_ERROR_NO_TTBR)
		fault_name = "NO TTBR FOR IOVA";
	else
		/* [한국어] 모르는 코드이거나 여러 비트가 함께 섰다. */
		fault_name = "unknown";

	/* [한국어] 제한된 속도로 진단 정보를 남긴다. */
	dev_err_ratelimited(
		dart->dev,
		"translation fault: status:0x%x stream:%d code:0x%x (%s) at 0x%llx",
		error, stream_idx, error_code, fault_name, addr);

	/* [한국어] 오류 상태를 지운다. */
	writel(error, dart->regs + DART_T8110_ERROR);
	/* [한국어] T8110은 오류를 낸 스트림 비트맵도 따로 기록하므로
	 * 그것까지 지워야 인터럽트가 다시 무장된다. */
	for (int i = 0; i < BITS_TO_U32(dart->num_streams); i++)
		writel(U32_MAX, dart->regs + DART_T8110_ERROR_STREAMS + 4 * i);

	return IRQ_HANDLED;	/* [한국어] 우리 DART의 오류였음을 커널에 알린다. */
}

/*
 * [한국어]
 * apple_dart_probe - DART 하드웨어 하나를 초기화한다
 *
 * @pdev: 플랫폼 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 순서에 이유가 있다.
 *
 *  1) 레지스터를 매핑하고 **크기를 검사한다.** T8110의 TCR 배열이
 *     0x1000부터 시작하므로 0x4000 미만이면 디바이스 트리가
 *     잘못된 것이다.
 *  2) 클럭을 켠다. 이후 모든 레지스터 접근의 전제다.
 *  3) 공통 파라미터에서 페이지 크기와 우회 지원 여부를 읽는다.
 *  4) 세대별로 주소 폭과 스트림 수를 정한다. T8020 계열은
 *     하드코딩, T8110은 하드웨어가 알려 준다.
 *  5) 리셋으로 알려진 상태를 만든다. 여기서 잠금 검사도 이뤄진다.
 *  6) 인터럽트를 등록한 **뒤** 코어에 등록한다.
 *
 * 클럭을 끄지 않고 끝나는 점에 유의 — DART는 마스터가 DMA를 내는
 * 동안 계속 살아 있어야 한다.
 *
 * 실행 컨텍스트: 플랫폼 probe. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [apple_dart_probe] → apple_dart_hw_reset()
 *   → iommu_device_register()
 */
static int apple_dart_probe(struct platform_device *pdev)
{
	/* [한국어] 단계별 결과. */
	int ret;
	/* [한국어] 하드웨어 파라미터 레지스터에서 읽은 값들. */
	u32 dart_params[4];
	/* [한국어] 레지스터 자원(크기 검사에 쓴다). */
	struct resource *res;
	/* [한국어] 만들 DART 상태. */
	struct apple_dart *dart;
	/* [한국어] 로그와 devm의 기준 디바이스. */
	struct device *dev = &pdev->dev;

	/* [한국어] 디바이스 수명에 묶어 상태를 잡는다. */
	dart = devm_kzalloc(dev, sizeof(*dart), GFP_KERNEL);
	if (!dart)	/* [한국어] 상태 구조체를 잡지 못했다. */
		return -ENOMEM;

	/* [한국어] 로그와 io-pgtable의 기준 디바이스. */
	dart->dev = dev;
	/* [한국어] 디바이스 트리 매치가 알려 준 세대별 레지스터 배치. */
	dart->hw = of_device_get_match_data(dev);
	/* [한국어] 하드웨어 명령을 직렬화할 락. */
	spin_lock_init(&dart->lock);

	/* [한국어] 레지스터 영역을 매핑하고 자원 정보도 받는다. */
	dart->regs = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(dart->regs))	/* [한국어] 레지스터 영역을 매핑하지 못했다. */
		return PTR_ERR(dart->regs);

	/* [한국어] T8110의 TCR 배열이 0x1000부터 시작하므로 이보다
	 * 작은 영역이면 레지스터에 닿지 못한다 — 디바이스 트리 오류다. */
	if (resource_size(res) < 0x4000) {
		dev_err(dev, "MMIO region too small (%pr)\n", res);	/* [한국어] 디바이스 트리의 reg 크기가 잘못되었다. */
		return -EINVAL;	/* [한국어] 레지스터에 닿을 수 없으므로 이 DART를 쓸 수 없다. */
	}

	/* [한국어] 폴트 인터럽트 번호를 얻는다. */
	dart->irq = platform_get_irq(pdev, 0);
	if (dart->irq < 0)	/* [한국어] 폴트 인터럽트를 얻지 못했다. */
		return -ENODEV;

	/* [한국어] 디바이스 트리에 있는 클럭을 전부 가져온다.
	 * 개수가 세대와 인스턴스마다 달라 이름을 지정하지 않는다. */
	ret = devm_clk_bulk_get_all(dev, &dart->clks);
	if (ret < 0)	/* [한국어] 클럭을 얻지 못했다. */
		return ret;
	dart->num_clks = ret;

	/* [한국어] 클럭을 켠다. 이후 레지스터 접근의 전제이며,
	 * DART가 살아 있는 동안 계속 켜 둔다. */
	ret = clk_bulk_prepare_enable(dart->num_clks, dart->clks);
	if (ret)	/* [한국어] 클럭을 켜지 못하면 레지스터를 읽을 수 없다. */
		return ret;

	/* [한국어] 세대 공통 파라미터를 읽는다. */
	dart_params[0] = readl(dart->regs + DART_PARAMS1);
	dart_params[1] = readl(dart->regs + DART_PARAMS2);
	/* [한국어] 페이지 크기는 지수로 주어진다. */
	dart->pgsize = 1 << FIELD_GET(DART_PARAMS1_PAGE_SHIFT, dart_params[0]);
	/* [한국어] 우회 지원 여부. of_xlate가 이 값을 AND로 누적한다. */
	dart->supports_bypass = dart_params[1] & DART_PARAMS2_BYPASS_SUPPORT;

	/* [한국어] 주소 폭과 스트림 수를 알아내는 방법이 세대마다 다르다. */
	switch (dart->hw->type) {
	/* [한국어] 구세대는 하드웨어가 알려 주지 않아 상수로 정한다. */
	case DART_T8020:
	case DART_T6000:
		/* [한국어] IOVA는 32비트 고정. */
		dart->ias = 32;
		/* [한국어] 물리 주소 폭은 세대 표의 값(36 또는 42비트). */
		dart->oas = dart->hw->oas;
		/* [한국어] 스트림 수도 세대 표가 정한다. */
		dart->num_streams = dart->hw->max_sid_count;
		break;

	/* [한국어] T8110은 자신의 능력을 레지스터로 알려 준다. */
	case DART_T8110:
		dart_params[2] = readl(dart->regs + DART_T8110_PARAMS3);	/* [한국어] 주소 폭이 담긴 파라미터 레지스터. */
		dart_params[3] = readl(dart->regs + DART_T8110_PARAMS4);
		/* [한국어] 입력 주소 폭. */
		dart->ias = FIELD_GET(DART_T8110_PARAMS3_VA_WIDTH, dart_params[2]);
		/* [한국어] 출력 주소 폭. */
		dart->oas = FIELD_GET(DART_T8110_PARAMS3_PA_WIDTH, dart_params[2]);
		/* [한국어] 실제 스트림 개수. */
		dart->num_streams = FIELD_GET(DART_T8110_PARAMS4_NUM_SIDS, dart_params[3]);
		/* [한국어] IOVA가 36비트를 넘으면 3단계로 덮을 수 없어
		 * 4단계 워크가 필요하다. */
		dart->four_level = dart->ias > 36;
		break;
	}

	/* [한국어] 비트맵과 sid2group 배열이 정적 크기라, 그 한계를
	 * 넘는 하드웨어는 다룰 수 없다. */
	if (dart->num_streams > DART_MAX_STREAMS) {
		dev_err(&pdev->dev, "Too many streams (%d > %d)\n",	/* [한국어] 정적 배열의 한계를 넘는 하드웨어다. */
			dart->num_streams, DART_MAX_STREAMS);
		ret = -EINVAL;	/* [한국어] 다룰 수 없는 구성이다. */
		goto err_clk_disable;	/* [한국어] 켜 둔 클럭을 되돌리러 간다. */
	}

	/* [한국어] 알려진 초기 상태로 만든다. 부트로더가 잠갔다면
	 * 여기서 실패한다. */
	ret = apple_dart_hw_reset(dart);
	if (ret)	/* [한국어] 리셋에 실패했다 — 잠겼거나 명령이 먹히지 않는다. */
		goto err_clk_disable;

	/* [한국어] 폴트 핸들러를 등록한다. 여러 DART가 인터럽트를
	 * 공유할 수 있어 SHARED다. devm이 아닌 이유는 remove에서
	 * 리셋 뒤에 명시적 순서로 해제해야 하기 때문이다. */
	ret = request_irq(dart->irq, dart->hw->irq_handler, IRQF_SHARED,
			  "apple-dart fault handler", dart);
	if (ret)	/* [한국어] 폴트 핸들러를 등록하지 못했다. */
		goto err_clk_disable;

	/* [한국어] of_xlate가 이 값으로 DART를 찾으므로, 코어에
	 * 등록하기 전에 설정해 둔다. */
	platform_set_drvdata(pdev, dart);

	/* [한국어] sysfs에 IOMMU 인스턴스를 노출한다. */
	ret = iommu_device_sysfs_add(&dart->iommu, dev, NULL, "apple-dart.%s",
				     dev_name(&pdev->dev));
	if (ret)	/* [한국어] sysfs 등록에 실패했다. */
		goto err_free_irq;

	/* [한국어] 코어에 등록한다. 이 순간부터 마스터들의
	 * probe_device가 불리기 시작한다. */
	ret = iommu_device_register(&dart->iommu, &apple_dart_iommu_ops, dev);
	if (ret)	/* [한국어] 코어 등록에 실패했다. */
		goto err_sysfs_remove;

	/* [한국어] 알아낸 능력을 한 줄로 남긴다. "bypass forced"는
	 * 페이지가 커널 페이지보다 커서 통과 모드밖에 쓸 수 없다는
	 * 뜻으로, def_domain_type의 판단과 같은 조건이다. */
	dev_info(
		&pdev->dev,
		"DART [pagesize %x, %d streams, bypass support: %d, bypass forced: %d, AS %d -> %d] initialized\n",
		dart->pgsize, dart->num_streams, dart->supports_bypass,
		dart->pgsize > PAGE_SIZE, dart->ias, dart->oas);
	return 0;

/* [한국어] 코어 등록 실패 — sysfs부터 되돌린다. */
err_sysfs_remove:
	iommu_device_sysfs_remove(&dart->iommu);
/* [한국어] 인터럽트를 등록한 뒤의 실패 — 핸들러를 뗀다. */
err_free_irq:
	free_irq(dart->irq, dart);
/* [한국어] 클럭을 켠 뒤의 실패 — 클럭을 내린다. */
err_clk_disable:
	clk_bulk_disable_unprepare(dart->num_clks, dart->clks);	/* [한국어] 켜 둔 클럭을 내린다. dart 자체는 devm이라 자동 해제된다. */

	return ret;	/* [한국어] 실패 이유를 플랫폼 코어에 전한다. */
}

/*
 * [한국어]
 * apple_dart_remove - DART를 걷어낸다
 *
 * @pdev: 대상 플랫폼 디바이스.
 * @return: 없음.
 *
 * **리셋을 먼저 한다**는 점이 중요하다. 모든 스트림의 DMA를 막고
 * TTBR을 지운 뒤에 인터럽트를 뗀다. 순서가 반대면 아직 DMA를 낼
 * 수 있는 상태에서 폴트를 처리할 핸들러가 사라진다.
 *
 * 실행 컨텍스트: 드라이버 제거. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   플랫폼 코어 → [apple_dart_remove] → apple_dart_hw_reset()
 */
static void apple_dart_remove(struct platform_device *pdev)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = platform_get_drvdata(pdev);

	/* [한국어] 먼저 모든 DMA를 막고 테이블을 떼어 낸다 —
	 * 그래야 이후 폴트가 날 일이 없다. */
	apple_dart_hw_reset(dart);
	/* [한국어] 그다음 핸들러를 뗀다. */
	free_irq(dart->irq, dart);

	/* [한국어] 코어에서 빼고 sysfs를 정리한다. */
	iommu_device_unregister(&dart->iommu);
	iommu_device_sysfs_remove(&dart->iommu);

	/* [한국어] 마지막으로 클럭을 내린다. */
	clk_bulk_disable_unprepare(dart->num_clks, dart->clks);
}

/* [한국어] T8103(M1) 세대의 하드웨어 표.
 * 36비트 물리 주소, 스트림 16개, TTBR 4개짜리 구성이다. */
static const struct apple_dart_hw apple_dart_hw_t8103 = {
	.type = DART_T8020,
	/* [한국어] T8020 계열의 레지스터 배치를 쓴다. */

	.irq_handler = apple_dart_t8020_irq,
	/* [한국어] T8020 형식의 오류 레지스터를 읽는 핸들러. */

	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	/* [한국어] SELECT + COMMAND 방식의 무효화. */

	.oas = 36,
	/* [한국어] 물리 주소 36비트. 하드웨어가 알려 주지 않아 상수다. */

	.fmt = APPLE_DART,
	/* [한국어] 1세대 페이지 테이블 포맷. */

	.max_sid_count = 16,
	/* [한국어] 스트림 16개. */

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	/* [한국어] 리셋에서 전부 켤 레지스터. */

	.lock = DART_T8020_CONFIG,
	/* [한국어] 잠금 상태를 담은 레지스터. */

	.lock_bit = DART_T8020_CONFIG_LOCK,
	/* [한국어] 그 안의 잠금 비트. */

	.error = DART_T8020_ERROR,
	/* [한국어] 오류 상태 레지스터. */

	.tcr = DART_T8020_TCR,
	/* [한국어] 스트림별 TCR 배열의 시작. */

	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	/* [한국어] "변환 수행"에 해당하는 값. */

	.tcr_disabled = 0,
	/* [한국어] "아무것도 하지 않음" = DMA 차단. */

	.tcr_bypass = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,
	/* [한국어] 완전한 통과를 위해 변환과 권한 필터를 모두 건너뛴다. */

	.ttbr = DART_T8020_TTBR,
	/* [한국어] 스트림별 TTBR 배열의 시작. */

	.ttbr_valid = DART_T8020_TTBR_VALID,
	/* [한국어] 유효 비트(최상위). */

	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	/* [한국어] 주소 필드가 비트 0부터 시작한다. */

	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	/* [한국어] 주소를 12비트 밀어 담는다 = 4KB 정렬 요구. */

	.ttbr_count = 4,
	/* [한국어] 스트림마다 TTBR 4개 — 최상위 테이블이 네 조각이다. */
};

/* [한국어] T8103의 USB4 전용 DART.
 * 스트림이 64개로 많아 TTBR 배열의 위치가 다르고,
 * **우회를 지원하지 않는다**(tcr_bypass가 0). */
static const struct apple_dart_hw apple_dart_hw_t8103_usb4 = {
	.type = DART_T8020,
	/* [한국어] 레지스터 배치는 T8020 계열과 같다. */

	.irq_handler = apple_dart_t8020_irq,
	/* [한국어] 같은 오류 형식을 쓴다. */

	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	/* [한국어] 같은 무효화 방식. */

	.oas = 36,
	/* [한국어] 물리 주소 36비트. */

	.fmt = APPLE_DART,
	/* [한국어] 1세대 포맷. */

	.max_sid_count = 64,
	/* [한국어] 스트림이 64개로 많다 — Thunderbolt 터널마다
	 * 스트림이 필요하기 때문이다. */

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	/* [한국어] 스트림 활성화 레지스터. */

	.lock = DART_T8020_CONFIG,
	/* [한국어] 잠금 레지스터. */

	.lock_bit = DART_T8020_CONFIG_LOCK,
	/* [한국어] 잠금 비트. */

	.error = DART_T8020_ERROR,
	/* [한국어] 오류 레지스터. */

	.tcr = DART_T8020_TCR,
	/* [한국어] TCR 배열의 시작. */

	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	/* [한국어] 변환 수행 값. */

	.tcr_disabled = 0,
	/* [한국어] 차단 값. */

	.tcr_bypass = 0,
	/* [한국어] 0이므로 이 하드웨어는 통과 모드를 쓸 수 없다.
	 * 외부에서 꽂는 장치를 다루는 DART라 우회가 허용되지 않는
	 * 것으로, 보안상 합리적인 제약이다. */

	.ttbr = DART_T8020_USB4_TTBR,
	/* [한국어] 스트림이 많아 TTBR 배열이 다른 자리에 있다. */

	.ttbr_valid = DART_T8020_TTBR_VALID,
	/* [한국어] 유효 비트. */

	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	/* [한국어] 주소 필드 위치. */

	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	/* [한국어] 4KB 정렬 요구. */

	.ttbr_count = 4,
	/* [한국어] 스트림마다 TTBR 4개. */
};

/* [한국어] T6000(M1 Pro/Max) 세대의 하드웨어 표.
 * 레지스터 배치는 T8103과 같고, 물리 주소가 42비트로 넓어져
 * 페이지 테이블 포맷이 2세대다. */
static const struct apple_dart_hw apple_dart_hw_t6000 = {
	.type = DART_T6000,
	/* [한국어] 레지스터 배치는 T8020과 같지만 세대를 구분한다. */

	.irq_handler = apple_dart_t8020_irq,
	/* [한국어] 오류 형식이 같아 같은 핸들러를 쓴다. */

	.invalidate_tlb = apple_dart_t8020_hw_invalidate_tlb,
	/* [한국어] 무효화 방식도 같다. */

	.oas = 42,
	/* [한국어] 물리 주소 42비트 — 이것이 T8103과의 실질적 차이다. */

	.fmt = APPLE_DART2,
	/* [한국어] 넓어진 주소를 담을 수 있는 2세대 포맷. */

	.max_sid_count = 16,
	/* [한국어] 스트림 16개. */

	.enable_streams = DART_T8020_STREAMS_ENABLE,
	/* [한국어] 스트림 활성화 레지스터. */

	.lock = DART_T8020_CONFIG,
	/* [한국어] 잠금 레지스터. */

	.lock_bit = DART_T8020_CONFIG_LOCK,
	/* [한국어] 잠금 비트. */

	.error = DART_T8020_ERROR,
	/* [한국어] 오류 레지스터. */

	.tcr = DART_T8020_TCR,
	/* [한국어] TCR 배열의 시작. */

	.tcr_enabled = DART_T8020_TCR_TRANSLATE_ENABLE,
	/* [한국어] 변환 수행 값. */

	.tcr_disabled = 0,
	/* [한국어] 차단 값. */

	.tcr_bypass = DART_T8020_TCR_BYPASS_DAPF | DART_T8020_TCR_BYPASS_DART,
	/* [한국어] 통과 값. */

	.ttbr = DART_T8020_TTBR,
	/* [한국어] TTBR 배열의 시작. */

	.ttbr_valid = DART_T8020_TTBR_VALID,
	/* [한국어] 유효 비트. */

	.ttbr_addr_field_shift = DART_T8020_TTBR_ADDR_FIELD_SHIFT,
	/* [한국어] 주소 필드 위치. */

	.ttbr_shift = DART_T8020_TTBR_SHIFT,
	/* [한국어] 4KB 정렬 요구. */

	.ttbr_count = 4,
	/* [한국어] 스트림마다 TTBR 4개. */
};

/* [한국어] T8110 세대의 하드웨어 표.
 * 레지스터 배치가 완전히 다르고, 주소 폭과 스트림 수를 하드웨어가
 * 알려 주며, TTBR 하나로 최대 4단계 워크를 한다. */
static const struct apple_dart_hw apple_dart_hw_t8110 = {
	.type = DART_T8110,
	/* [한국어] 새 레지스터 배치를 쓴다. */

	.irq_handler = apple_dart_t8110_irq,
	/* [한국어] NO_PGD 코드와 오류 스트림 비트맵을 다루는 핸들러. */

	.invalidate_tlb = apple_dart_t8110_hw_invalidate_tlb,
	/* [한국어] 스트림마다 명령을 내는 방식의 무효화. */

	.fmt = APPLE_DART2,
	/* [한국어] 2세대 포맷.
	 * oas가 없는 점에 유의 — 하드웨어가 직접 알려 주기 때문이다. */

	.max_sid_count = 256,
	/* [한국어] 표상의 상한. 실제 개수는 PARAMS4에서 읽는다. */

	.enable_streams = DART_T8110_ENABLE_STREAMS,
	/* [한국어] 스트림 활성화 레지스터(위치가 다르다). */

	.lock = DART_T8110_PROTECT,
	/* [한국어] T8110에서는 "보호" 레지스터가 잠금 역할을 한다. */

	.lock_bit = DART_T8110_PROTECT_TTBR_TCR,
	/* [한국어] TTBR과 TCR이 보호되어 있음을 뜻하는 비트. */

	.error = DART_T8110_ERROR,
	/* [한국어] 오류 레지스터. */

	.tcr = DART_T8110_TCR,
	/* [한국어] TCR 배열의 시작(0x1000). */

	.tcr_enabled = DART_T8110_TCR_TRANSLATE_ENABLE,
	/* [한국어] 변환 수행 값. 비트 위치가 T8020과 완전히 다르다. */

	.tcr_disabled = 0,
	/* [한국어] 차단 값. */

	.tcr_bypass = DART_T8110_TCR_BYPASS_DAPF | DART_T8110_TCR_BYPASS_DART,
	/* [한국어] 통과 값. */

	.tcr_4level = DART_T8110_TCR_FOUR_LEVEL,
	/* [한국어] 4단계 워크를 요구하는 비트. 이 세대에만 있다. */

	.ttbr = DART_T8110_TTBR,
	/* [한국어] TTBR 배열의 시작(0x1400). */

	.ttbr_valid = DART_T8110_TTBR_VALID,
	/* [한국어] 유효 비트가 최하위로 옮겨졌다. */

	.ttbr_addr_field_shift = DART_T8110_TTBR_ADDR_FIELD_SHIFT,
	/* [한국어] 주소 필드가 비트 2부터 시작한다. */

	.ttbr_shift = DART_T8110_TTBR_SHIFT,
	/* [한국어] 14비트 시프트 = 16KB 정렬 요구. */

	.ttbr_count = 1,
	/* [한국어] TTBR이 하나뿐이다 — 네 조각으로 나누는 대신
	 * 단계를 하나 더 두는 방식으로 바뀌었다. */
};

/*
 * [한국어]
 * apple_dart_suspend - 절전 진입 시 하드웨어 상태를 떠 둔다
 *
 * @dev: 대상 디바이스.
 * @return: 항상 0.
 *
 * DART는 절전에서 레지스터 내용을 잃는다. 도메인 정보로부터
 * 재구성하는 대신, 모든 스트림의 TCR과 TTBR을 통째로 읽어
 * 배열에 떠 둔다. 어느 스트림이 어느 도메인에 속하는지 추적할
 * 필요가 없어 훨씬 단순한 접근이다.
 *
 * 실행 컨텍스트: 시스템 절전. 프로세스 컨텍스트, 클럭은 아직 켜져 있다.
 *
 * 호출 체인:
 *   PM 코어 → [apple_dart_suspend]
 */
static __maybe_unused int apple_dart_suspend(struct device *dev)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = dev_get_drvdata(dev);
	/* [한국어] 스트림과 TTBR 순회 인덱스. */
	unsigned int sid, idx;

	/* [한국어] 모든 스트림의 상태를 그대로 떠 둔다. */
	for (sid = 0; sid < dart->num_streams; sid++) {
		/* [한국어] 이 스트림의 변환 제어 값. */
		dart->save_tcr[sid] = readl(dart->regs + DART_TCR(dart, sid));
		/* [한국어] 이 스트림의 모든 TTBR 값. */
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			dart->save_ttbr[sid][idx] =
				readl(dart->regs + DART_TTBR(dart, sid, idx));
	}

	return 0;	/* [한국어] 상태를 떠 두는 일은 실패하지 않는다. */
}

/*
 * [한국어]
 * apple_dart_resume - 절전에서 깨어나 하드웨어 상태를 되돌린다
 *
 * @dev: 대상 디바이스.
 * @return: 0 성공, 음수 오류.
 *
 * 순서가 중요하다: **리셋을 먼저 하고** 값을 되돌린다. 리셋이
 * 스트림 활성화와 오류 마스크 같은 전역 설정을 되살리고,
 * 그다음 스트림별 값을 복원하는 구조다.
 *
 * TTBR을 TCR보다 먼저 쓰는 것도 의도적이다. TCR로 변환을 켠
 * 순간 하드웨어가 테이블을 읽기 시작하므로, 테이블 주소가
 * 먼저 제자리에 있어야 한다.
 *
 * 실행 컨텍스트: 시스템 재개. 프로세스 컨텍스트.
 *
 * 호출 체인:
 *   PM 코어 → [apple_dart_resume] → apple_dart_hw_reset()
 */
static __maybe_unused int apple_dart_resume(struct device *dev)
{
	/* [한국어] 대상 DART. */
	struct apple_dart *dart = dev_get_drvdata(dev);
	/* [한국어] 스트림과 TTBR 순회 인덱스. */
	unsigned int sid, idx;
	/* [한국어] 리셋 결과. */
	int ret;

	/* [한국어] 먼저 전역 설정(스트림 활성화, 오류 마스크 등)을
	 * 되살린다. 이 단계가 실패하면 복원해 봐야 소용이 없다. */
	ret = apple_dart_hw_reset(dart);
	if (ret) {	/* [한국어] 리셋에 실패한 경우. */
		dev_err(dev, "Failed to reset DART on resume\n");	/* [한국어] 복원해 봐야 소용없음을 알린다. */
		return ret;	/* [한국어] 재개 실패를 PM 코어에 전한다. */
	}

	/* [한국어] 떠 둔 스트림별 값을 되돌린다. */
	for (sid = 0; sid < dart->num_streams; sid++) {
		/* [한국어] TTBR을 먼저 채운다 — 변환을 켜기 전에 테이블
		 * 주소가 제자리에 있어야 한다. */
		for (idx = 0; idx < dart->hw->ttbr_count; idx++)
			writel(dart->save_ttbr[sid][idx],
			       dart->regs + DART_TTBR(dart, sid, idx));
		/* [한국어] 그다음 TCR을 써 변환을 되살린다. */
		writel(dart->save_tcr[sid], dart->regs + DART_TCR(dart, sid));
	}

	return 0;	/* [한국어] 모든 스트림의 상태를 되돌렸다. */
}

/* [한국어] 시스템 절전 콜백 묶음. 런타임 PM은 쓰지 않는다 —
 * DART는 마스터가 살아 있는 동안 항상 켜져 있어야 한다. */
static DEFINE_SIMPLE_DEV_PM_OPS(apple_dart_pm_ops, apple_dart_suspend, apple_dart_resume);

/* [한국어] 디바이스 트리 호환 문자열과 하드웨어 표의 대응.
 * 같은 SoC 안에서도 USB4 DART는 별도 문자열을 쓴다 — 스트림 수와
 * 우회 지원 여부가 달라 다른 표가 필요하기 때문이다. */
static const struct of_device_id apple_dart_of_match[] = {
	{ .compatible = "apple,t8103-dart", .data = &apple_dart_hw_t8103 },
	/* [한국어] M1의 일반 DART. */

	{ .compatible = "apple,t8103-usb4-dart", .data = &apple_dart_hw_t8103_usb4 },
	/* [한국어] M1의 Thunderbolt/USB4 DART — 우회 불가. */

	{ .compatible = "apple,t8110-dart", .data = &apple_dart_hw_t8110 },
	/* [한국어] 새 레지스터 배치의 세대. */

	{ .compatible = "apple,t6000-dart", .data = &apple_dart_hw_t6000 },
	/* [한국어] M1 Pro/Max의 DART — 42비트 물리 주소. */

	{},
	/* [한국어] 표의 끝을 알리는 빈 항목. */
};
/* [한국어] 위 표를 모듈 별칭으로 내보내 자동 로딩이 되게 한다. */
MODULE_DEVICE_TABLE(of, apple_dart_of_match);

/* [한국어] 플랫폼 드라이버 서술자. */
static struct platform_driver apple_dart_driver = {
	.driver	= {	/* [한국어] 플랫폼 코어가 보는 드라이버 속성들. */
		.name			= "apple-dart",
		/* [한국어] 드라이버 이름. */

		.of_match_table		= apple_dart_of_match,
		/* [한국어] 위의 호환 문자열 대응표. */

		.suppress_bind_attrs    = true,
		/* [한국어] sysfs로 바인딩을 풀 수 없게 막는다.
		 * DART를 임의로 떼어 내면 그 뒤의 마스터들이 갑자기
		 * 변환 없이 DMA를 내게 되어 위험하다. */

		.pm			= pm_sleep_ptr(&apple_dart_pm_ops),
		/* [한국어] 절전 콜백. CONFIG_PM_SLEEP이 꺼져 있으면
		 * pm_sleep_ptr이 NULL로 접혀 코드가 통째로 빠진다. */
	},
	.probe	= apple_dart_probe,
	/* [한국어] 하드웨어 초기화 진입점. */

	.remove = apple_dart_remove,
	/* [한국어] 하드웨어 정리 진입점. */
};

/* [한국어] 모듈 init/exit 상용구를 생성한다. */
module_platform_driver(apple_dart_driver);

/* [한국어] 모듈 정보. */
MODULE_DESCRIPTION("IOMMU API for Apple's DART");
/* [한국어] 원저자(Asahi Linux 프로젝트). */
MODULE_AUTHOR("Sven Peter <sven@svenpeter.dev>");
/* [한국어] 라이선스. */
MODULE_LICENSE("GPL v2");
