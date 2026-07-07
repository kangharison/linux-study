// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/aix.c
 *
 *  Copyright (C) 2012-2013 Philippe De Muyter <phdm@macqel.be>
 */

/*
 * [한국어 설명] AIX LVM(Logical Volume Manager) 파티션 인식기 (block/partitions/aix.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 IBM AIX 운영체제가 사용하는 LVM(Logical Volume Manager) 온디스크
 * 포맷을 해석하여, 리눅스 커널의 파티션 테이블 프로버(probe) 체계에서 AIX
 * 논리 볼륨(Logical Volume, LV)들을 리눅스 파티션으로 등록해 주는 레거시
 * 파티션 검출 드라이버다. AIX는 전통적인 MBR/GPT 파티션 테이블 대신, 디스크
 * 전체를 하나의 물리 볼륨(Physical Volume, PV)으로 보고 VGDA(Volume Group
 * Descriptor Area)라는 별도의 메타데이터 영역에 볼륨 그룹/논리 볼륨 구성을
 * 기록한다. 이 파일은 그 VGDA를 순서대로 읽어 나가며 각 논리 볼륨이 물리
 * 디스크 상에서 연속된 영역(physical partition들이 끊김 없이 이어짐)을
 * 차지하는지 판정하고, 연속된 경우에만 리눅스 파티션(/dev/sdX1 등)으로
 * 등록한다. 비연속(non-contiguous) 논리 볼륨은 단일 (시작 LBA, 길이) 쌍으로
 * 표현할 수 없으므로 등록하지 않고 커널 로그에 경고만 남긴다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 커널이 블록 디바이스(gendisk)를 등록할 때(device_add_disk 경로) 또는
 * BLKRRPART ioctl로 파티션을 재스캔할 때, block/partitions/core.c의
 * check_partition()이 block/partitions/check.h에 선언된 check_part[] 표를
 * 순서대로 순회하며 각 포맷별 프로버 함수를 호출한다. 이 파일의 진입점인
 * aix_partition()도 그 표에 등록되어 있으며, 다른 프로버들과 마찬가지로
 * "이 디스크가 내가 아는 포맷이 맞는지" 스스로 판정한 뒤 파티션을 등록한다.
 * 대략적인 호출 체인은 다음과 같다:
 *   device_add_disk()/blkdev_ioctl(BLKRRPART)
 *     -> bdev_disk_changed() -> check_partition()
 *     -> aix_partition() (이 파일)
 *     -> read_part_sector() -> read_mapping_folio()
 *     -> (page cache 미스 시) submit_bio -> blk_mq_submit_bio -> ...
 *        -> 하부 블록 드라이버(NVMe/SCSI/virtio-blk 등)의 실제 READ 명령 처리
 * 이 코드는 디바이스 프로브/재스캔이라는 아주 드문 경로에서만 실행되는
 * 콜드 패스(cold path)이며, 정상적인 파일 시스템 I/O 핫패스와는 무관하다.
 * 실행 컨텍스트는 프로세스 컨텍스트(디바이스 등록 커널 스레드 또는 ioctl을
 * 호출한 유저 프로세스의 시스템 콜 컨텍스트)이며, 인터럽트 컨텍스트가 아니다.
 *
 * === 타 모듈과의 연결 ===
 * 이 파일은 block/partitions/check.h가 정의하는 struct parsed_partitions와
 * read_part_sector()/put_dev_sector()/put_partition() 인프라에 전적으로
 * 의존한다. read_part_sector()는 다시 block/partitions/core.c를 거쳐 대상
 * 블록 디바이스의 page cache(read_mapping_folio)와 그 아래의 submit_bio
 * 경로로 연결된다. 이 파일 자체는 특정 저장장치 종류(NVMe/SATA/가상디스크 등)
 * 에 의존하지 않으며, struct parsed_partitions가 감싸고 있는 gendisk의 원시
 * 섹터를 바이트 단위로 읽을 뿐이다. 데이터 흐름은 "디스크 원시 바이트 ->
 * read_lba()/read_part_sector()로 커널 버퍼에 복사 -> lvm_rec/vgda/lvd/
 * lvname/pvd 구조체로 캐스팅해 빅엔디안 필드를 해석 -> put_partition()으로
 * state->parts[]에 파티션 시작 LBA/길이 기록 -> seq_buf_printf()로
 * state->pp_buf에 사람이 읽는 로그 문자열을 누적" 순서로 진행된다.
 * 공유되는 핵심 자료구조는 struct parsed_partitions(state)이며, 이 파일이
 * 새로 정의하는 lvm_rec/vgda/lvd/lvname/ppe/pvd는 AIX LVM 온디스크 레이아웃을
 * 그대로 옮긴 것으로 이 파일 밖에서는 쓰이지 않는 지역적(local) 자료구조다.
 *
 * === 주요 함수/구조체 요약 ===
 * - read_lba(): 임의 바이트 길이를 여러 섹터에 걸쳐 이어서 읽는 공용 헬퍼.
 * - alloc_pvd(): PVD(물리 볼륨 서술자) 전체를 kmalloc 후 read_lba()로 채움.
 * - alloc_lvn(): 논리 볼륨 이름 테이블(최대 256개)을 kmalloc 후 read_lba()로 채움.
 * - aix_partition(): 이 파일의 유일한 공개 진입점. LBA 7의 lvm_rec ->
 *   VGDA 헤더 -> LVD 배열/이름 테이블 -> PVD/PPE 배열 순서로 읽어가며
 *   물리적으로 연속된 논리 볼륨을 리눅스 파티션으로 등록한다.
 * - struct lvm_rec: LBA 7에 위치한 LVM 식별/버전/VGDA 위치 헤더.
 * - struct vgda: 볼륨 그룹 전체의 논리/물리 볼륨 개수 등을 담은 헤더.
 * - struct lvd: 논리 볼륨 1개당 하나씩 존재하는 서술자(논리 파티션 개수 등).
 * - struct lvname: 논리 볼륨 이름(64바이트) 배열.
 * - struct ppe: 물리 파티션 1개가 어느 논리 볼륨/논리 파티션에 속하는지 기록.
 * - struct pvd: 물리 볼륨 서술자. pp_count/psn_part1 및 ppe[] 배열 보유.
 */

#include "check.h"


/*
 * [한국어] LVM 레코드(LVM Record).
 * AIX LVM 규약상 물리 볼륨(PV)의 고정된 위치인 LBA(섹터) 7에 위치하며,
 * 이 PV가 AIX LVM으로 초기화되어 있는지, 그리고 VGDA(Volume Group
 * Descriptor Area)가 어디서 시작하는지를 알려주는 최초 진입점 헤더다.
 * 모든 멀티바이트 정수 필드는 AIX(POWER, 빅엔디안)가 기록한 그대로 빅엔디안
 * 순서이므로, 이 드라이버는 반드시 be16_to_cpu()/be32_to_cpu()로 변환해서
 * 읽어야 한다.
 */
struct lvm_rec {
	char lvm_id[4]; /* "_LVM" */
	/* [한국어] AIX LVM 매직 시그니처. 정상적인 AIX LVM PV라면 "_LVM" 4글자가
	 * 그대로 들어있어야 한다.
	 * 설정자: AIX의 LVM 초기화 도구(mkvg 등)가 PV를 생성할 때 기록.
	 * 읽는 자: 이 드라이버(aix_partition)는 실제로는 이 필드를 검사하지
	 * 않는다 - 대신 version 필드가 1인지만으로 AIX LVM 여부를 판정한다.
	 * 값 범위: ASCII "_LVM" 고정 문자열(검증되지 않음).
	 * 동기화: 디스크에서 한 번 읽어온 읽기 전용 스냅샷, 락 불필요. */
	char reserved4[16];
	/* [한국어] 예약 영역(16바이트). on-disk 레이아웃에서 lvm_id 뒤 다음
	 * 필드(lvmarea_len)까지의 간격을 맞추기 위한 패딩.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 절대 참조하지 않는다.
	 * 값 범위: AIX가 기록한 임의 값(의미 불명, 무시해도 안전).
	 * 동기화: 해당 없음. */
	__be32 lvmarea_len;
	/* [한국어] LVM 정보 영역(LVM area) 전체 길이(섹터 단위, 빅엔디안 32비트).
	 * 이 PV에서 lvm_rec + VGDA 사본들 등 LVM 메타데이터가 차지하는 총
	 * 섹터 수로 추정된다.
	 * 설정자: AIX LVM 초기화 도구.
	 * 읽는 자: 이 드라이버는 읽지 않는다(오프셋 정렬을 위해서만 구조체에
	 * 존재) - 실제로 사용되는 길이 필드는 아래의 vgda_len 뿐이다.
	 * 값 범위: 섹터 단위 양의 정수.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be32 vgda_len;
	/* [한국어] VGDA(Volume Group Descriptor Area) 한 사본의 길이(섹터 단위,
	 * 빅엔디안 32비트).
	 * 설정자: AIX LVM 초기화 도구가 VG(Volume Group) 생성 시 기록.
	 * 읽는 자: aix_partition()이 be32_to_cpu(p->vgda_len)으로 읽어
	 * vgda_len 지역변수에 저장하고, 이후 이름 테이블 위치
	 * (vgda_sector + vgda_len - 33) 계산에 사용한다.
	 * 값 범위: 섹터 단위 양의 정수(전형적으로 VG 크기에 비례).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be32 vgda_psn[2];
	/* [한국어] VGDA 사본들의 시작 물리 섹터 번호(PSN, Physical Sector Number)
	 * 배열. AIX는 신뢰성을 위해 VGDA를 PV당 보통 2개 사본(주/보조)으로
	 * 복제해 서로 다른 위치에 저장한다. vgda_psn[0]이 1차 사본, vgda_psn[1]
	 * 이 2차 사본의 시작 섹터로 추정된다.
	 * 설정자: AIX LVM 초기화 도구.
	 * 읽는 자: aix_partition()은 vgda_psn[0]만 be32_to_cpu()로 읽어
	 * vgda_sector로 사용한다 - 2차 사본(vgda_psn[1])은 이중화용이며 이
	 * 드라이버는 참조하지 않는다(주 사본이 손상된 경우의 폴백 로직 없음).
	 * 값 범위: 섹터 단위 절대 PSN.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	char reserved36[10];
	/* [한국어] 예약 영역(10바이트). pp_size 필드 앞의 온디스크 패딩.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be16 pp_size; /* log2(pp_size) */
	/* [한국어] 물리 파티션(Physical Partition, PP) 크기의 log2 값
	 * (섹터 단위, 빅엔디안 16비트). 실제 PP 크기(바이트) = 512 << pp_size.
	 * 설정자: AIX LVM 초기화 도구가 VG 생성 시(사용자가 지정한 PP 크기로) 기록.
	 * 읽는 자: aix_partition()이 be16_to_cpu(p->pp_size)로 읽어
	 * pp_bytes_size = 1 << pp_size_log2, pp_blocks_size = pp_bytes_size/512
	 * 를 계산 - 이 값이 이후 모든 파티션 시작/길이 계산의 섹터 단위가 된다.
	 * 값 범위: 보통 수 ~ 십수(log2), 즉 PP 크기가 수백KB ~ 수십MB 수준.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	char reserved46[12];
	/* [한국어] 예약 영역(12바이트). version 필드 앞의 온디스크 패딩.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be16 version;
	/* [한국어] LVM 온디스크 포맷 버전(빅엔디안 16비트).
	 * 설정자: AIX LVM 초기화 도구/커널이 온디스크 포맷을 기록할 때 결정.
	 * 읽는 자: aix_partition()이 be16_to_cpu(p->version)으로 읽어
	 * lvm_version에 저장하고, 이 값이 정확히 1인지만으로 "지원하는 AIX LVM
	 * 인지"를 판정한다 - lvm_id 매직 문자열 검사는 하지 않으므로, 사실상
	 * 이 필드가 이 드라이버의 유일한 시그니처 검증 지점이다.
	 * 값 범위: 1(지원) 또는 그 외(미지원 - seq_buf_printf로 경고만 남기고
	 * 이후 모든 처리를 건너뜀).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	};


/*
 * [한국어] VGDA(Volume Group Descriptor Area) 헤더.
 * 볼륨 그룹(Volume Group, VG) 전체에 대한 메타데이터 헤더로,
 * lvm_rec->vgda_psn[0] 이 가리키는 섹터에 위치한다. 이 헤더 자체에서
 * 이 드라이버가 실제로 읽는 필드는 numlvs 하나뿐이며, 나머지 필드들은
 * numlvs가 정확한 바이트 오프셋(24)에 위치하도록 온디스크 레이아웃을
 * 그대로 반영하기 위해서만 구조체에 존재한다.
 */
struct vgda {
	__be32 secs;
	/* [한국어] 이 VGDA가 마지막으로 갱신된 시각의 초(seconds, 빅엔디안
	 * 32비트, Unix epoch 기준으로 추정).
	 * 설정자: AIX LVM이 VG 구성을 변경할 때(mklv/extendvg 등) 갱신.
	 * 읽는 자: 이 드라이버는 참조하지 않는다(오프셋 정렬용).
	 * 값 범위: Unix epoch 초 값.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be32 usec;
	/* [한국어] 위 secs와 짝을 이루는 마이크로초(microseconds, 빅엔디안 32비트).
	 * 설정자: AIX LVM 갱신 시각 기록.
	 * 읽는 자: 이 드라이버는 참조하지 않는다(오프셋 정렬용).
	 * 값 범위: 0~999999.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	char reserved8[16];
	/* [한국어] 예약 영역(16바이트). timestamp 뒤 numlvs 앞의 온디스크 패딩.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be16 numlvs;
	/* [한국어] 이 VG에 현재 정의되어 있는 논리 볼륨(LV) 개수(빅엔디안 16비트).
	 * 이 구조체에서 이 드라이버가 실제로 읽는 유일한 필드다.
	 * 설정자: AIX LVM이 LV를 생성/삭제할 때마다 갱신(mklv/rmlv 등).
	 * 읽는 자: aix_partition()이 be16_to_cpu(p->numlvs)로 읽어 numlvs
	 * 지역변수에 저장 - 이후 LVD 순회 루프의 종료 조건
	 * (foundlvs < numlvs)으로 쓰인다.
	 * 값 범위: 0 ~ maxlvs(아래 필드). 0이면 VG는 있으나 LV가 없는 상태.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 maxlvs;
	/* [한국어] 이 VG가 생성될 때 지정된 최대 논리 볼륨 개수(빅엔디안 16비트,
	 * 전통적으로 256 = LVM_MAXLVS와 동일한 값을 갖는 경우가 많다).
	 * 설정자: VG 생성 시(mkvg) 결정, 이후 변경 불가.
	 * 읽는 자: 이 드라이버는 참조하지 않는다(대신 코드 전역에서
	 * LVM_MAXLVS 매크로 상수와 state->limit을 상한으로 사용).
	 * 값 범위: 통상 256.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 pp_size;
	/* [한국어] 물리 파티션 크기(log2, 빅엔디안 16비트) - lvm_rec.pp_size의
	 * 사본으로 추정된다(같은 정보를 VGDA에도 중복 기록해 두는 형태).
	 * 설정자: VG 생성 시 mkvg가 결정.
	 * 읽는 자: 이 드라이버는 참조하지 않는다 - 실제 PP 크기 계산은
	 * lvm_rec.pp_size 쪽만 사용한다.
	 * 값 범위: lvm_rec.pp_size와 동일할 것으로 추정.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 numpvs;
	/* [한국어] 이 VG에 속한 물리 볼륨(PV) 개수(빅엔디안 16비트).
	 * 설정자: VG에 PV를 추가/제거할 때(extendvg/reducevg) 갱신.
	 * 읽는 자: 이 드라이버는 참조하지 않는다 - 이 파일은 단일 PV 관점에서만
	 * 파싱을 수행하며 멀티 PV 볼륨 그룹의 다른 PV는 다루지 않는다.
	 * 값 범위: 1 이상.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 total_vgdas;
	/* [한국어] 이 VG 전체에 분산 저장된 VGDA 사본의 총 개수(빅엔디안 16비트,
	 * PV별 쿼럼(quorum) 판정에 쓰이는 것으로 추정).
	 * 설정자: VG 생성/PV 추가 시 결정.
	 * 읽는 자: 이 드라이버는 참조하지 않는다(단일 PV/단일 VGDA 사본만 읽음).
	 * 값 범위: PV 개수에 비례하는 양의 정수.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 vgda_size;
	/* [한국어] 이 VGDA가 차지하는 섹터 수(빅엔디안 16비트) -
	 * lvm_rec.vgda_len과 유사한 정보의 또 다른 사본으로 추정된다.
	 * 설정자: VG 생성 시 mkvg가 결정.
	 * 읽는 자: 이 드라이버는 참조하지 않는다 - 실제 길이 계산은
	 * lvm_rec.vgda_len 쪽만 사용한다.
	 * 값 범위: 섹터 단위 양의 정수.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	};


/*
 * [한국어] LVD(Logical Volume Descriptor).
 * 논리 볼륨(LV) 1개당 하나씩 존재하는 고정 크기(32바이트) 레코드로,
 * VGDA 헤더 바로 다음 섹터(vgda_sector + 1)에서부터 배열 형태로 연속
 * 저장된다. 이 드라이버는 이 배열을 "배열 인덱스 i가 곧 (LV 인덱스-1)"
 * 이라는 가정 하에 순회하며, 구조체 내의 lv_ix 필드 자체는 읽지 않는다 -
 * 오직 num_lps(해당 LV의 논리 파티션 총 개수)만 실제로 사용한다.
 */
struct lvd {
	__be16 lv_ix;
	/* [한국어] 이 LVD가 기술하는 논리 볼륨의 1-based 인덱스(빅엔디안 16비트).
	 * 설정자: AIX LVM이 LV를 생성할 때(mklv) 기록.
	 * 읽는 자: 이 드라이버는 실제로 읽지 않는다 - 대신 aix_partition()의
	 * LVD 순회 루프(for i)에서 배열 인덱스 i를 그대로 (LV 인덱스-1)로
	 * 취급한다. 반면 struct ppe.lv_ix(물리 파티션 엔트리쪽)는 실제로
	 * 읽혀서 lvip[]/이름 배열 인덱스로 쓰인다는 점과 혼동하지 말 것.
	 * 값 범위: 1 ~ maxlvs.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 res2;
	/* [한국어] 예약 필드(빅엔디안 16비트). 온디스크 패딩/미사용 영역.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be16 res4;
	/* [한국어] 예약 필드(빅엔디안 16비트). 온디스크 패딩/미사용 영역.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be16 maxsize;
	/* [한국어] 이 LV가 가질 수 있는 최대 크기(빅엔디안 16비트, 단위는
	 * 논리 파티션 개수로 추정 - AIX의 LV 최대 확장 한도).
	 * 설정자: LV 생성 시(mklv -x 옵션 등) 결정.
	 * 읽는 자: 이 드라이버는 참조하지 않는다(현재 크기는 num_lps로 판단).
	 * 값 범위: num_lps 이상의 양의 정수로 추정.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 lv_state;
	/* [한국어] LV의 상태(빅엔디안 16비트, 예: defined/closed/open 등으로 추정).
	 * 설정자: AIX LVM이 LV를 열거나 닫을 때 갱신.
	 * 읽는 자: 이 드라이버는 참조하지 않는다 - 파티션 등록 여부는 오직
	 * 물리 파티션의 연속성(lp_ix 순서)만으로 판정한다.
	 * 값 범위: AIX 내부 상태 코드(이 드라이버 관점에서는 불투명 값).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 mirror;
	/* [한국어] 이 LV의 미러(복제) 카피 개수(빅엔디안 16비트).
	 * 설정자: LV 생성/미러 구성 시(mklvcopy 등) 결정.
	 * 읽는 자: 이 드라이버는 참조하지 않는다 - 미러링된 LV라도 이 파일은
	 * PVD의 물리적 연속성만으로 판단하므로 미러 카피 자체를 별도 처리하지
	 * 않는다.
	 * 값 범위: 1 이상(1이면 미러 없음).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 mirror_policy;
	/* [한국어] 미러 쓰기 정책(빅엔디안 16비트, 예: 순차/병렬 쓰기 등으로 추정).
	 * 설정자: 미러 구성 시 결정.
	 * 읽는 자: 이 드라이버는 참조하지 않는다.
	 * 값 범위: AIX 내부 정책 코드(불투명 값).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 num_lps;
	/* [한국어] 이 LV가 갖는 논리 파티션(Logical Partition, LP) 총 개수
	 * (빅엔디안 16비트). 이 구조체에서 이 드라이버가 실제로 읽는 유일한
	 * 필드다.
	 * 설정자: LV 생성/확장(mklv/extendlv) 시마다 갱신.
	 * 읽는 자: aix_partition()의 LVD 순회 루프가 be16_to_cpu(p[i].num_lps)
	 * 로 읽어 lvip[i].pps_per_lv에 저장한다. 이후 PVD/PPE 순회에서
	 * "lp_ix가 이 값과 같아지면 그 LV의 마지막 논리 파티션"으로 판정하는
	 * 종료 기준이 된다.
	 * 값 범위: 1 ~ maxsize. 0이면(비어 있는 슬롯이면) 해당 인덱스에는
	 * 실제 LV가 없다는 뜻으로 취급된다(foundlvs 카운트에서 제외).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	__be16 res10[8];
	/* [한국어] 예약 영역(16바이트, __be16 8개). 다음 LVD 엔트리와의 간격을
	 * 맞추기 위한 온디스크 패딩(구조체 전체 크기를 32바이트로 고정).
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	};


/*
 * [한국어] 논리 볼륨 이름 테이블의 엔트리(고정 64바이트 문자열 1개).
 * VGDA 영역의 끝 부분(vgda_sector + vgda_len - 33 섹터 지점)에서부터
 * LVM_MAXLVS(256)개가 배열로 연속 저장되며, 인덱스는 LVD 배열과 마찬가지로
 * (LV 인덱스 - 1)에 대응한다고 가정한다. 파티션 등록 로그와 "비연속" 경고
 * 메시지에 사람이 읽을 이름으로 사용된다.
 */
struct lvname {
	char name[64];
	/* [한국어] 논리 볼륨 이름(ASCII, 최대 64바이트). AIX 온디스크 포맷은
	 * 이름이 64바이트를 꽉 채우면 널 종단(\0)을 보장하지 않을 수 있다.
	 * 설정자: LV 생성 시(mklv -y <name>) AIX LVM이 기록.
	 * 읽는 자: aix_partition()이 파티션을 등록할 때
	 * seq_buf_printf(" <%s>\n", n[lv_ix].name)로 로그에 출력하고,
	 * 비연속 LV 경고 시에는 snprintf()로 널 종단을 보장한 임시 버퍼(tmp)에
	 * 복사한 뒤 pr_warn()에 사용한다 - 원본 name 배열을 %s로 직접 넘기지
	 * 않는 이유가 바로 이 널 종단 미보장 때문이다.
	 * 값 범위: 임의의 ASCII 문자열(널 종단 여부 불확실).
	 * 동기화: 읽기 전용 스냅샷, 단일 스레드에서만 접근하므로 락 불필요. */
	};


/*
 * [한국어] PPE(Physical Partition Entry).
 * 물리 볼륨(PV)의 물리 파티션(Physical Partition, PP) 1개마다 존재하는
 * 매핑 엔트리로, "이 PP가 어느 논리 볼륨(lv_ix)의 몇 번째 논리
 * 파티션(lp_ix)으로 매핑되어 있는가"를 기록한다. struct pvd.ppe[] 배열의
 * 원소 타입이며, aix_partition()의 핵심 순회 루프가 이 배열 전체를 인덱스
 * 순서(=물리 디스크상의 순서)대로 훑으면서 논리 파티션 번호(lp_ix)가
 * 1부터 끊김없이 증가하는 구간을 찾아 "물리적으로 연속된 LV"를 검출한다.
 */
struct ppe {
	__be16 lv_ix;
	/* [한국어] 이 물리 파티션이 속한 논리 볼륨의 1-based 인덱스(빅엔디안
	 * 16비트). struct lvd.lv_ix와 이름은 같지만 이쪽은 실제로 읽힌다.
	 * 설정자: 물리 파티션을 LV에 할당할 때(mklv/extendlv) AIX LVM이 기록.
	 * 읽는 자: aix_partition()의 PPE 순회 루프가
	 * lv_ix = be16_to_cpu(p->lv_ix) - 1 로 1-based -> 0-based 변환한 뒤
	 * lvip[lv_ix], n[lv_ix] 인덱스로 사용한다.
	 * 값 범위: 1 ~ maxlvs. 참고로 "이 PP가 미사용인지"는 이 필드가 아니라
	 * 같은 엔트리의 lp_ix 필드가 0인지로 판별하며, lp_ix가 0인 경우
	 * aix_partition()은 이 lv_ix 값을 아예 검사하지 않고 건너뛴다.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	unsigned short res2;
	/* [한국어] 예약 필드. 온디스크 패딩(빅엔디안이 아닌 순수
	 * unsigned short로 선언되어 있어 바이트스왑 없이도 크기만 맞추는
	 * 용도임을 알 수 있다).
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	unsigned short res4;
	/* [한국어] 예약 필드. 온디스크 패딩.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be16 lp_ix;
	/* [한국어] 이 물리 파티션이 소속 LV 내에서 몇 번째 논리
	 * 파티션(Logical Partition, LP)에 매핑되는지(빅엔디안 16비트, 1-based).
	 * 이 구조체에서 가장 핵심적인 필드로, 연속성 판정의 기준이 된다.
	 * 설정자: 물리 파티션을 LV에 순서대로 할당할 때 AIX LVM이 기록.
	 * 읽는 자: aix_partition()의 PPE 순회 루프가
	 * lp_ix = be16_to_cpu(p->lp_ix)로 읽어, 0이면 미사용 PP로 간주해
	 * 건너뛰고, next_lp_ix(기대하는 다음 lp_ix)와 비교해 물리적 연속성을
	 * 검사하며, pps_per_lv(=num_lps)와 같아지면 해당 LV의 마지막 PP로
	 * 판정해 put_partition()을 호출한다.
	 * 값 범위: 0(미사용) 또는 1 ~ num_lps.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	unsigned short res8[12];
	/* [한국어] 예약 필드(24바이트, unsigned short 12개). 다음 PPE
	 * 엔트리와의 간격을 맞추는 온디스크 패딩(엔트리 전체 크기를
	 * 32바이트로 고정해 struct pvd.ppe[1016] 배열의 인덱싱을 단순하게
	 * 만든다).
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	};


/*
 * [한국어] PVD(Physical Volume Descriptor).
 * 물리 볼륨(PV) 전체에 대한 메타데이터로, VGDA 영역 내 고정 오프셋인
 * (vgda_sector + 17) 섹터에 위치한다(오프셋 17은 AIX LVM 온디스크
 * 레이아웃 규약에 따른 매직 넘버로, VGDA 헤더 + LVD 배열 + 기타 예약
 * 영역을 지난 지점으로 추정된다). 물리 파티션 총 개수(pp_count), 실제
 * 데이터 영역이 시작하는 절대 섹터(psn_part1), 그리고 각 물리 파티션의
 * LV/LP 매핑 정보를 담은 ppe[] 배열을 갖는다. sizeof(struct pvd)가 약
 * 32KB에 달해 함수 스택에 두기엔 크므로 항상 alloc_pvd()로 힙에 할당된다.
 */
struct pvd {
	char reserved0[16];
	/* [한국어] 예약 영역(16바이트). pp_count 필드 앞의 온디스크 패딩.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be16 pp_count;
	/* [한국어] 이 물리 볼륨(PV)의 전체 물리 파티션(PP) 개수(빅엔디안 16비트).
	 * 설정자: PV가 VG에 추가될 때(PV 용량 / PP 크기로) AIX LVM이 계산해 기록.
	 * 읽는 자: aix_partition()이 be16_to_cpu(pvd->pp_count)로 읽어 numpps에
	 * 저장하고, 이후 for (i = 0; i < numpps; ...) 루프에서 ppe[] 배열 순회
	 * 횟수로 사용한다.
	 * 값 범위: 1 ~ 1016(아래 ppe[] 배열 크기 이하여야 함).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	char reserved18[2];
	/* [한국어] 예약 영역(2바이트). psn_part1 필드와의 정렬을 위한 패딩.
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	__be32 psn_part1;
	/* [한국어] 이 PV에서 첫 번째 물리 파티션 데이터가 시작하는 절대 물리
	 * 섹터 번호(PSN, 빅엔디안 32비트).
	 * 설정자: PV가 VG에 추가될 때(LVM 메타데이터 영역 크기에 따라) 결정.
	 * 읽는 자: aix_partition()이 be32_to_cpu(pvd->psn_part1)로 읽어
	 * psn_part1에 저장하고, 파티션 시작 LBA를
	 * (i + 1 - lp_ix) * pp_blocks_size + psn_part1 형태로 계산할 때
	 * 기준(0번째 물리 파티션의 시작 섹터)으로 사용한다.
	 * 값 범위: 절대 섹터 오프셋(양의 정수).
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	char reserved24[8];
	/* [한국어] 예약 영역(8바이트). ppe[] 배열 앞의 온디스크 패딩(pvd 헤더
	 * 전체를 32바이트로 맞춰 이후 ppe[] 배열이 정확히 오프셋 32부터
	 * 시작하도록 정렬한다).
	 * 설정자/읽는 자: 없음 - 이 드라이버는 참조하지 않는다.
	 * 값 범위: 의미 불명, 무시해도 안전.
	 * 동기화: 해당 없음. */
	struct ppe ppe[1016];
	/* [한국어] 물리 파티션 매핑 엔트리 배열. 인덱스 i(0-based)는 물리
	 * 파티션 번호와 그대로 대응하며, ppe[i]는 "i번째 물리 파티션이 어느
	 * LV의 몇 번째 LP인가"를 담는다. 최대 1016개로 고정 크기 선언되어
	 * 있는데, 이는 전통적인 AIX LVM PVD 섹터 레이아웃의 상한으로 추정된다.
	 * 설정자: PV의 물리 파티션들이 LV에 할당될 때마다 AIX LVM이 기록.
	 * 읽는 자: aix_partition()의 메인 순회 루프가
	 * struct ppe *p = pvd->ppe + i; 형태로 포인터 산술을 통해 각 엔트리에
	 * 접근하며, pp_count(=numpps)개만 유효한 것으로 간주하고 순회한다.
	 * 값 범위: 배열 크기는 고정 1016이지만 유효 엔트리 수는 pp_count.
	 * 동기화: 읽기 전용 스냅샷, 락 불필요. */
	};


/*
 * [한국어] 이 드라이버가 다루는 볼륨 그룹의 논리 볼륨(LV) 최대 개수 상한.
 * AIX LVM 전통적인 기본값(256)과 일치하며, alloc_lvn()이 할당하는 이름
 * 테이블(struct lvname 배열)의 크기를 결정하는 데 쓰인다. VGDA 헤더의
 * maxlvs 필드 값을 직접 읽어 쓰지 않고 이 고정 상수를 사용하므로, 만약
 * 실제 VG의 maxlvs가 이 값보다 크게 구성되어 있다면 이름 테이블 일부가
 * 잘릴 수 있음에 유의해야 한다(단, aix_partition()의 LV 순회 자체는
 * state->limit으로도 별도로 제한되므로 실질적인 영향은 크지 않다).
 */
#define LVM_MAXLVS 256

/**
 * read_lba(): Read bytes from disk, starting at given LBA
 * @state
 * @lba
 * @buffer
 * @count
 *
 * Description:  Reads @count bytes from @state->disk into @buffer.
 * Returns number of bytes read on success, 0 on error.
 */

/*
 * [한국어]
 * read_lba() - 디스크의 임의 바이트 범위를 여러 섹터에 걸쳐 이어서 읽는다.
 *
 * @state: 파티션 스캔 상태. state->disk를 통해 대상 gendisk와 용량 정보에
 *         접근하기 위해 read_part_sector()/get_capacity()에 그대로 전달된다.
 * @lba: 읽기 시작 위치(섹터/LBA 번호, 512바이트 단위 0-based).
 * @buffer: 읽은 내용을 채워 넣을 커널 버퍼. 호출자가 미리 최소 @count 바이트
 *          이상을 확보해 두어야 한다(alloc_pvd/alloc_lvn이 kmalloc으로 준비).
 * @count: 읽어야 할 총 바이트 수. 512의 배수가 아니어도 되며, 마지막 섹터는
 *         일부 바이트만 복사된다.
 * @return: 실제로 복사에 성공한 총 바이트 수. 정상 종료 시 @count와 같아야
 *          완전한 성공이며, 그보다 작으면 중간에 read_part_sector()가
 *          실패했다는 뜻이다(호출자는 반환값과 @count를 비교해 실패를
 *          판정한다). buffer가 NULL이거나 요청 범위가 디스크 용량을
 *          벗어나면 아무것도 읽지 않고 0을 반환한다.
 *
 * alloc_pvd()/alloc_lvn()이 여러 섹터에 걸친 큰 구조체(PVD 약 32KB, 이름
 * 테이블 16KB)를 한 번에 읽어오기 위해 사용하는 공용 하위 헬퍼다.
 * read_part_sector()는 한 번에 정확히 한 섹터(512B)만 읽을 수 있으므로,
 * 이 함수가 필요한 섹터 수만큼 read_part_sector()를 반복 호출하며 버퍼
 * 포인터를 전진시켜 임의 길이의 연속 읽기를 흉내낸다.
 * 동작 과정: 먼저 (lba + count/512)가 디스크 용량(get_capacity)을 넘는지
 * 검사해 범위를 벗어난 요청을 조기에 차단한다. 이후 count가 0이 될 때까지
 * 반복하며, 매 반복마다 read_part_sector()로 한 섹터를 읽고, 섹터 캐시
 * 핸들(Sector sect)을 즉시 put_dev_sector()로 반환한다. 마지막 섹터에서는
 * count가 512보다 작을 수 있으므로 copied를 count로 낮춰 초과 복사를 막는다.
 * 실행 컨텍스트: aix_partition()과 동일하게 디바이스 프로브/파티션 재스캔
 * 경로에서 단일 스레드로 실행되며, 재진입이나 동시 호출을 고려하지 않는다.
 * 호출자: alloc_pvd(), alloc_lvn().
 * 피호출자: get_capacity(), read_part_sector(), memcpy(), put_dev_sector().
 * 에러 경로: read_part_sector()가 NULL을 반환하면(섹터 읽기 실패) while
 * 루프를 즉시 break로 탈출하고, 그때까지 누적된 totalreadcount만 반환한다 -
 * 이 값은 요청한 count보다 작으므로 호출자가 실패로 인식한다.
 *
 * 호출 체인:
 *   aix_partition() -> alloc_pvd()/alloc_lvn() -> [read_lba] -> read_part_sector()
 */
static size_t read_lba(struct parsed_partitions *state, u64 lba, u8 *buffer,
			size_t count)
{
	/* [한국어] 누적 복사 바이트 수 초기화 - while 루프가 반복될 때마다 이번에 복사한 양만큼 증가하며, 함수의 최종 반환값이 된다. */
	size_t totalreadcount = 0;

	/* [한국어] 요청 범위 유효성 검사: buffer가 NULL이면 즉시 실패, 또는
	 * (시작 lba + count 바이트를 섹터로 환산한 값)이 디스크 용량-1을
	 * 넘어서면(디스크 끝을 벗어나는 읽기) 실패로 처리해 범위를 벗어난
	 * read_part_sector() 호출 자체를 조기에 차단한다. */
	if (!buffer || lba + count / 512 > get_capacity(state->disk) - 1ULL)
		/* [한국어] 위 조건에 해당하면 아무 것도 읽지 않고 0을 반환한다 - 이
		 * 값은 read_lba()의 모든 호출자가 "실패"로 해석하는 신호이다. */
		return 0;

	/* [한국어] 남은 요청 바이트(count)가 있는 동안 한 섹터씩 반복해서 읽는다. */
	while (count) {
		/* [한국어] 이번 반복에서 복사할 바이트 수의 기본값(한 섹터 = 512B)으로 초기화. */
		int copied = 512;
		/* [한국어] 이번 read_part_sector() 호출이 반환하는 섹터 캐시(folio) 핸들을 담을 지역 변수. */
		Sector sect;
		/* [한국어] 현재 lba 섹터를 읽는다 - 후위 증가(lba++) 이므로 이번 호출에는
		 * 증가 전의 lba 값이 쓰이고, 다음 반복을 위해 lba는 1 증가한다. */
		unsigned char *data = read_part_sector(state, lba++, &sect);
		/* [한국어] read_part_sector()가 NULL을 반환했다면 이 섹터를 읽는 데 실패한 것이다. */
		if (!data)
			/* [한국어] 더 이상 읽을 수 없으므로 루프를 즉시 종료하고, 지금까지
			 * 누적된 totalreadcount만 그대로 반환한다(부분 읽기). */
			break;
		/* [한국어] 마지막 조각인지 검사: 남은 요청량(count)이 한 섹터(512B)보다 작으면 */
		if (copied > count)
			/* [한국어] 마지막 조각 크기(count)만큼만 복사하도록 copied를 줄인다 - 섹터
			 * 버퍼 너머의 쓰레기 데이터를 호출자 버퍼로 복사하는 것을 방지. */
			copied = count;
		/* [한국어] 이번 섹터에서 읽어온 데이터(data)를 호출자 버퍼(buffer)로 copied 바이트만큼 복사. */
		memcpy(buffer, data, copied);
		/* [한국어] 방금 읽은 섹터의 캐시 참조(folio)를 해제 - 더 이상 필요 없음. */
		put_dev_sector(sect);
		/* [한국어] 다음 조각을 쓸 위치로 목적지 버퍼 포인터를 전진시킨다. */
		buffer += copied;
		/* [한국어] 누적 복사 바이트 수(totalreadcount)를 이번 조각만큼 증가 - 최종 반환값에 반영. */
		totalreadcount += copied;
		/* [한국어] 남은 요청 바이트 수(count)를 이번 조각만큼 감소 - 0이 되면 while 루프가 종료된다. */
		count -= copied;
	}
	/* [한국어] 성공(또는 부분 성공) 시 실제로 복사된 총 바이트 수를 반환 - 호출자가 count와 비교해 완전 성공 여부를 판정한다. */
	return totalreadcount;
}

/**
 * alloc_pvd(): reads physical volume descriptor
 * @state
 * @lba
 *
 * Description: Returns pvd on success,  NULL on error.
 * Allocates space for pvd and fill it with disk blocks at @lba
 * Notes: remember to free pvd when you're done!
 */

/*
 * [한국어]
 * alloc_pvd() - 물리 볼륨 서술자(Physical Volume Descriptor, PVD)를
 * 통째로 읽어 온다.
 *
 * @state: 파티션 스캔 상태. read_lba()에 그대로 전달되어 디스크 접근에 쓰인다.
 * @lba: PVD가 시작하는 섹터 번호. aix_partition()에서 vgda_sector + 17로
 *       계산해 전달한다(고정 오프셋 17은 AIX LVM 온디스크 레이아웃 규약).
 * @return: 새로 kmalloc()한 struct pvd 포인터(디스크 내용으로 채워짐).
 *          메모리 할당 실패 또는 read_lba()가 sizeof(struct pvd)만큼
 *          채우지 못하면(디스크 읽기 실패) NULL.
 *
 * struct pvd 하나는 ppe[1016] 배열을 포함해 약 32KB에 달해 스택에 두기엔
 * 너무 크므로 항상 힙에 kmalloc으로 할당해야 한다. 이 함수는 그 할당과,
 * read_lba()를 통한 디스크 읽기, 실패 시의 정리(kfree)를 한 곳에
 * 캡슐화한다.
 * 동작 과정: sizeof(struct pvd) 크기만큼 kmalloc 후, read_lba()로 @lba
 * 부터 그 바이트 수만큼 채운다. read_lba()의 반환값이 요청한 count보다
 * 작으면(디스크 읽기가 도중에 실패했다는 의미) 이미 할당한 메모리를
 * kfree()로 즉시 반환하고 NULL을 돌려준다.
 * 실행 컨텍스트: aix_partition()과 동일한 단일 스레드 프로브 경로.
 * 호출자: aix_partition() (VGDA의 LVD/이름 테이블을 성공적으로 읽은
 * 뒤에만 호출된다).
 * 피호출자: kmalloc(), read_lba(), kfree().
 * 에러 경로: kmalloc 실패 시 즉시 NULL, read_lba 결과 부족 시 kfree 후
 * NULL - 두 경우 모두 호출자(aix_partition)는 pvd가 NULL이면 이후 PPE
 * 순회 블록 전체(if (pvd) { ... })를 건너뛴다.
 *
 * 호출 체인:
 *   aix_partition() -> [alloc_pvd] -> read_lba() -> read_part_sector()
 */
static struct pvd *alloc_pvd(struct parsed_partitions *state, u32 lba)
{
	/* [한국어] PVD 구조체 전체 크기(약 32KB) - kmalloc 요청 크기이자 read_lba()에 넘길 목표 바이트 수. */
	size_t count = sizeof(struct pvd);
	struct pvd *p;

	/* [한국어] PVD 전체를 담을 버퍼를 힙에 할당 - GFP_KERNEL: 프로세스 컨텍스트이므로 필요 시 슬리핑 허용. */
	p = kmalloc(count, GFP_KERNEL);
	/* [한국어] 할당 실패 검사. */
	if (!p)
		/* [한국어] 메모리 부족 시 디스크 읽기를 시도하지 않고 즉시 NULL 반환. */
		return NULL;

	/* [한국어] 방금 할당한 버퍼에 lba 섹터부터 count 바이트를 채운다 - 반환값이 count보다 작으면 읽기 실패. */
	if (read_lba(state, lba, (u8 *) p, count) < count) {
		/* [한국어] 읽기 실패 시 이미 할당한 버퍼를 즉시 해제 - 메모리 누수 방지. */
		kfree(p);
		/* [한국어] 실패를 호출자(aix_partition)에게 NULL로 알린다. */
		return NULL;
	}
	/* [한국어] 성공: 디스크 내용으로 채워진 PVD 버퍼 포인터를 반환 - 호출자가 사용 후 반드시 kfree() 해야 한다. */
	return p;
}

/**
 * alloc_lvn(): reads logical volume names
 * @state
 * @lba
 *
 * Description: Returns lvn on success,  NULL on error.
 * Allocates space for lvn and fill it with disk blocks at @lba
 * Notes: remember to free lvn when you're done!
 */

/*
 * [한국어]
 * alloc_lvn() - 논리 볼륨 이름 테이블을 통째로 읽어 온다.
 *
 * @state: 파티션 스캔 상태. read_lba()에 그대로 전달.
 * @lba: 이름 테이블이 시작하는 섹터 번호. aix_partition()에서
 *       vgda_sector + vgda_len - 33 으로 계산해 전달한다(VGDA 영역이
 *       끝나는 지점에서 33섹터 앞이 이름 테이블의 시작이라는 AIX LVM
 *       온디스크 레이아웃 규약 - 이름 테이블 자체는 256 * 64B = 16KB
 *       = 32섹터이므로, 33섹터는 그 앞에 1섹터 분량의 여유/헤더가 더
 *       있다는 뜻으로 추정된다).
 * @return: 새로 kmalloc()한 struct lvname 배열(LVM_MAXLVS(256)개) 포인터.
 *          메모리 할당 실패 또는 디스크 읽기 실패 시 NULL.
 *
 * 논리 볼륨 이름은 사람이 읽는 로그 출력(예: " <lv00>")과 비연속 볼륨
 * 경고 메시지에 쓰인다. 이 함수는 그 이름 테이블 전체(256개 * 64바이트 =
 * 16KiB)를 힙에 할당하고 read_lba()로 채운다.
 * 동작 과정: alloc_pvd()와 동일한 패턴 - kmalloc 후 read_lba()로 채우고,
 * 부족하면 kfree() 후 NULL 반환.
 * 실행 컨텍스트: aix_partition()과 동일한 단일 스레드 프로브 경로.
 * 호출자: aix_partition() (VGDA 두 번째 섹터에서 LVD 배열을 읽은 직후).
 * 피호출자: kmalloc(), read_lba(), kfree().
 * 에러 경로: 실패 시 NULL을 반환하며, 호출자는 n이 NULL이면 이후 LVD/PVD
 * 처리 블록 전체(foundlvs 계산, alloc_pvd 호출 포함)를 건너뛴다.
 *
 * 호출 체인:
 *   aix_partition() -> [alloc_lvn] -> read_lba() -> read_part_sector()
 */
static struct lvname *alloc_lvn(struct parsed_partitions *state, u32 lba)
{
	/* [한국어] 이름 테이블 전체 크기 = 엔트리 하나(64B) * 최대 LV 개수(LVM_MAXLVS=256) = 16KiB. */
	size_t count = sizeof(struct lvname) * LVM_MAXLVS;
	struct lvname *p;

	/* [한국어] 이름 테이블 전체를 담을 버퍼를 힙에 할당 - GFP_KERNEL: 프로세스 컨텍스트이므로 슬리핑 허용. */
	p = kmalloc(count, GFP_KERNEL);
	/* [한국어] 할당 실패 검사. */
	if (!p)
		/* [한국어] 메모리 부족 시 디스크 읽기를 시도하지 않고 즉시 NULL 반환. */
		return NULL;

	/* [한국어] 방금 할당한 버퍼에 lba 섹터부터 count 바이트를 채운다 - 반환값이 count보다 작으면 읽기 실패. */
	if (read_lba(state, lba, (u8 *) p, count) < count) {
		/* [한국어] 읽기 실패 시 이미 할당한 버퍼를 즉시 해제 - 메모리 누수 방지. */
		kfree(p);
		/* [한국어] 실패를 호출자(aix_partition)에게 NULL로 알린다. */
		return NULL;
	}
	/* [한국어] 성공: 디스크 내용으로 채워진 이름 테이블 포인터를 반환 - 호출자가 사용 후 반드시 kfree() 해야 한다. */
	return p;
}


/*
 * [한국어]
 * aix_partition() - AIX LVM 파티션 검출의 진입점.
 *
 * @state: 파티션 스캔 상태(대상 gendisk, parts[] 배열, 로그 버퍼 pp_buf,
 *         등록 가능한 최대 파티션 수 limit 등을 담음). block/partitions/
 *         core.c의 check_partition()이 미리 할당해 전달한다.
 * @return: 하나 이상의 논리 볼륨을 리눅스 파티션으로 등록했으면 1(true),
 *          AIX LVM 시그니처(버전)를 찾지 못했거나 등록 가능한 논리 볼륨이
 *          하나도 없으면 0(false). check_part[] 표를 순회하는
 *          check_partition()은 0을 반환받으면 다음 포맷 프로버로 넘어간다.
 *
 * 이 함수는 block/partitions/check.h의 check_part[] 표에 등록된 여러
 * 파티션 포맷 프로버 중 하나로, 디스크가 AIX LVM으로 초기화되어 있는지
 * 확인하고, 그 위에 정의된 논리 볼륨들 중 물리적으로 연속된 것들만 리눅스
 * 파티션으로 노출한다. AIX LVM은 MBR/GPT와 달리 파티션 테이블이 디스크
 * 맨 앞의 고정 오프셋에 있지 않고, LBA 7의 lvm_rec가 가리키는 VGDA
 * 영역에 계층적으로 흩어져 있으므로, 이 함수는 총 4단계로 나누어 순차적
 * 으로 디스크를 읽는다.
 *
 * 동작 과정:
 *   1) LBA 7에서 struct lvm_rec를 읽어 version 필드로 지원 여부를
 *      판정하고, pp_size(물리 파티션 크기)와 VGDA 시작 위치(vgda_sector)
 *      /길이(vgda_len)를 얻는다. 버전이 1이 아니면 vgda_sector가 0으로
 *      남아 이후 모든 단계가 건너뛰어진다(사실상 조기 실패).
 *   2) vgda_sector에서 struct vgda를 읽어 이 볼륨 그룹의 논리 볼륨
 *      개수(numlvs)를 얻는다.
 *   3) vgda_sector+1에서 struct lvd 배열을 읽어 각 논리 볼륨의 논리
 *      파티션 개수(num_lps)를 lvip[] 배열에 채우고, alloc_lvn()으로
 *      이름 테이블을, alloc_pvd()로 물리 볼륨 서술자(PVD)를 읽는다.
 *   4) PVD의 ppe[] 배열(물리 파티션 1개당 1 entry)을 순서대로 순회
 *      하며, 같은 논리 볼륨에 속한 논리 파티션 번호(lp_ix)가 1부터
 *      끊김 없이 증가하는 구간을 찾는다. 그 구간이 해당 LV의 전체
 *      논리 파티션 수(num_lps)까지 도달하면, 그 구간 전체를 하나의
 *      연속된 리눅스 파티션(put_partition)으로 등록한다. 중간에 다른
 *      LV로 바뀌거나 lp_ix 순서가 끊기면 해당 LV는 "비연속"으로 남고,
 *      함수 마지막에서 발견은 됐지만 등록되지 못한 LV들에 대해
 *      pr_warn()으로 경고만 남긴다.
 *
 * 실행 컨텍스트: 디바이스 프로브(device_add_disk) 또는 BLKRRPART ioctl에
 * 의한 파티션 재스캔 시, 단일 프로세스 컨텍스트에서 한 디스크당 한 번씩만
 * 실행된다. 인터럽트 컨텍스트가 아니며, 동일 gendisk에 대한 중복 실행은
 * 상위(core.c)에서 직렬화되는 것으로 추정된다(이 파일 범위 밖).
 *
 * 호출자: block/partitions/core.c의 check_partition() (check_part[] 표를
 * 통해 호출).
 * 피호출자: read_part_sector(), put_dev_sector(), be16_to_cpu()/
 * be32_to_cpu(), kzalloc_objs(), alloc_lvn(), alloc_pvd(), put_partition(),
 * seq_buf_printf(), pr_warn(), snprintf(), kfree().
 *
 * 에러 경로: 각 단계에서 필요한 섹터를 읽지 못하거나(read_part_sector가
 * NULL) 메모리 할당에 실패하면(kzalloc_objs/kmalloc이 NULL) 해당 단계
 * 이후 로직을 건너뛰고 결국 ret=0으로 반환한다. 부분적으로 성공한 경우
 * (예: 일부 LV만 연속)에는 성공한 LV만 등록하고 ret=1을 반환하며, 나머지는
 * 경고만 남긴다 - 즉 이 함수는 "전부 성공 아니면 전부 실패"가 아니라 부분
 * 성공을 허용한다.
 *
 * 호출 체인:
 *   check_partition() -> [aix_partition] -> read_part_sector()/alloc_lvn()/
 *   alloc_pvd() -> read_lba() -> read_part_sector()
 */
int aix_partition(struct parsed_partitions *state)
{
	/* [한국어] 파티션을 하나라도 등록했는지 여부 - 함수의 최종 반환값. 0=미발견, 1=발견. */
	int ret = 0;
	/* [한국어] read_part_sector()가 반환하는 섹터 캐시(folio) 핸들 - 매 read_part_sector 호출마다 재사용되는 지역 변수. */
	Sector sect;
	/* [한국어] read_part_sector()가 반환한, 현재 읽은 섹터의 원시 바이트 포인터. */
	unsigned char *d;
	/* [한국어] 물리 파티션(PP) 하나의 크기(바이트 단위) - lvm_rec.pp_size로부터 계산됨. */
	u32 pp_bytes_size;
	/* [한국어] 물리 파티션(PP) 하나의 크기(512B 섹터 단위) - 파티션 시작/길이 계산의 기본 단위. 아직 계산 전이므로 0으로 초기화. */
	u32 pp_blocks_size = 0;
	/* [한국어] VGDA가 위치한 섹터 번호(lvm_rec.vgda_psn[0]에서 얻음) - 아직 읽기 전이므로 0(= "VGDA 없음/미발견" 상태)으로 초기화. */
	u32 vgda_sector = 0;
	/* [한국어] VGDA 영역의 길이(섹터 단위) - 이름 테이블 위치 계산에 쓰임. 아직 읽기 전이므로 0으로 초기화. */
	u32 vgda_len = 0;
	/* [한국어] 이 볼륨 그룹에 정의된 논리 볼륨(LV) 개수 - VGDA 헤더에서 읽음. 아직 읽기 전이므로 0으로 초기화. */
	int numlvs = 0;
	/* [한국어] alloc_pvd()로 읽어올 물리 볼륨 서술자(PVD) 포인터 - 아직 할당 전이므로 NULL로 초기화, 함수 끝에서 kfree() 대상. */
	struct pvd *pvd = NULL;
	/* [한국어] 각 논리 볼륨(LV)마다 파티션 연속성 판정에 필요한 상태를 추적하기 위한 지역(함수 내부 전용) 구조체 정의.
	 * lvip[]는 아래에서 state->limit개 만큼 배열로 할당된다. */
	struct lv_info {
		unsigned short pps_per_lv;
		/* [한국어] 이 LV가 가져야 할 전체 논리 파티션(LP) 개수 - struct lvd.num_lps에서 복사되어 옴.
		 * 설정자: LVD 순회 루프가 be16_to_cpu(p[i].num_lps)로 채움.
		 * 읽는 자: PPE 순회에서 lp_ix가 이 값과 같아지면 해당 LV의 마지막 LP(=연속 구간의 끝)로 판정.
		 * 값 범위: 0(빈 슬롯, 실제 LV 없음) 또는 1 이상.
		 * 동기화: 함수 로컬 배열, 단일 스레드 내에서만 접근되므로 락 불필요. */
		unsigned short pps_found;
		/* [한국어] 실제로 PPE 배열 순회 중 이 LV에 속한 것으로 발견된 물리 파티션(PP) 개수 누적치.
		 * 설정자: PPE 순회 루프의 lvip[lv_ix].pps_found += 1 로 매 발견마다 증가.
		 * 읽는 자: 함수 말미의 비연속 경고 루프가 "pps_found가 0보다 크지만 lv_is_contiguous는 0"인 경우를 찾는 데 사용.
		 * 값 범위: 0 이상, 최대 numpps.
		 * 동기화: 함수 로컬 배열, 단일 스레드 내에서만 접근되므로 락 불필요. */
		unsigned char lv_is_contiguous;
		/* [한국어] 이 LV가 물리적으로 완전히 연속된 상태로 발견되어 파티션으로 등록되었는지 여부.
		 * 설정자: PPE 순회에서 lp_ix가 pps_per_lv(마지막 LP 번호)에 도달했을 때 1로 설정(kzalloc_objs가 처음엔 전부 0으로 초기화).
		 * 읽는 자: 함수 말미 경고 루프가 이 값이 0인데 pps_found는 0보다 큰 LV를 "비연속"으로 판정해 pr_warn().
		 * 값 범위: 0(미등록/비연속) 또는 1(등록됨/연속).
		 * 동기화: 함수 로컬 배열, 단일 스레드 내에서만 접근되므로 락 불필요. */
	/* [한국어] 위 익명 struct lv_info 타입의 배열을 가리키는 포인터 변수 선언 - 실제 배열은 아래에서 kzalloc_objs()로 state->limit개 만큼 할당됨. */
	} *lvip;
	/* [한국어] alloc_lvn()으로 읽어올 논리 볼륨 이름 테이블 포인터 - 아직 할당 전이므로 NULL로 초기화, 함수 끝에서 kfree() 대상. */
	struct lvname *n = NULL;

	/* [한국어] 1단계: AIX LVM 규약상 LVM 레코드가 항상 위치하는 고정 섹터(LBA 7)를 읽는다. */
	d = read_part_sector(state, 7, &sect);
	/* [한국어] 읽기 성공 시(d가 NULL이 아니면) 레코드를 해석한다. */
	if (d) {
		/* [한국어] 원시 바이트 포인터 d를 struct lvm_rec 레이아웃으로 캐스팅해 필드 접근을 가능하게 한다. */
		struct lvm_rec *p = (struct lvm_rec *)d;
		/* [한국어] 빅엔디안으로 기록된 version 필드를 호스트 바이트 순서로 변환해 읽는다. */
		u16 lvm_version = be16_to_cpu(p->version);

		/* [한국어] 이 드라이버가 이해하는 유일한 온디스크 버전(1)인지 검사 - 매직 문자열(lvm_id) 검사는 하지 않는다. */
		if (lvm_version == 1) {
			/* [한국어] 물리 파티션 크기의 log2 값을 빅엔디안 변환해서 읽는다. */
			int pp_size_log2 = be16_to_cpu(p->pp_size);

			/* [한국어] 2의 pp_size_log2 제곱 = 물리 파티션(PP) 하나의 바이트 크기. */
			pp_bytes_size = 1 << pp_size_log2;
			/* [한국어] 바이트 크기를 512로 나눠 섹터 단위로 변환 - 이후 모든 LBA 계산의 기본 단위가 됨. */
			pp_blocks_size = pp_bytes_size / 512;
			/* [한국어] 커널 로그(사용자에게 보여줄 파티션 스캔 결과 문자열)에 "AIX LVM 헤더 버전 N 발견" 메시지를 누적. */
			seq_buf_printf(&state->pp_buf,
				       " AIX LVM header version %u found\n",
				       lvm_version);
			/* [한국어] VGDA 한 사본의 길이(섹터 단위)를 빅엔디안 변환해서 읽는다 - 이름 테이블 위치 계산에 쓰임. */
			vgda_len = be32_to_cpu(p->vgda_len);
			/* [한국어] VGDA 1차 사본의 시작 섹터(vgda_psn[0])를 빅엔디안 변환해서 읽는다 - 2차 사본(vgda_psn[1])은 사용하지 않음. */
			vgda_sector = be32_to_cpu(p->vgda_psn[0]);
		/* [한국어] 버전이 1이 아니면(미지원 포맷) 아래 else 블록으로 진입 - vgda_sector는 0으로 남아 이후 단계가 모두 건너뛰어진다. */
		} else {
			/* [한국어] 커널 로그에 "지원하지 않는 AIX LVM 버전" 경고 메시지를 누적. */
			seq_buf_printf(&state->pp_buf,
				       " unsupported AIX LVM version %d found\n",
				       lvm_version);
		}
		/* [한국어] LBA 7 섹터의 캐시 참조(folio)를 해제 - 더 이상 필요 없음. */
		put_dev_sector(sect);
	}
	/* [한국어] 2단계: 1단계에서 VGDA 위치를 찾았다면(vgda_sector != 0), 그 섹터를 읽어 VGDA 헤더를 확인한다. */
	if (vgda_sector && (d = read_part_sector(state, vgda_sector, &sect))) {
		/* [한국어] 원시 바이트 포인터를 struct vgda 레이아웃으로 캐스팅. */
		struct vgda *p = (struct vgda *)d;

		/* [한국어] 이 볼륨 그룹에 정의된 논리 볼륨 개수를 빅엔디안 변환해서 읽는다 - VGDA 헤더에서 이 드라이버가 실제로 쓰는 유일한 필드. */
		numlvs = be16_to_cpu(p->numlvs);
		/* [한국어] VGDA 헤더 섹터의 캐시 참조를 해제. */
		put_dev_sector(sect);
	}
	/* [한국어] 이후 LV별 연속성 추적에 쓸 lvip[] 배열을 state->limit(등록 가능한 최대 파티션 수)개만큼 0으로 초기화 할당. */
	lvip = kzalloc_objs(struct lv_info, state->limit);
	/* [한국어] 할당 실패 검사. */
	if (!lvip)
		/* [한국어] 메모리 부족 시 더 진행할 수 없으므로 "파티션 미발견"으로 즉시 반환(다른 자원은 아직 할당되지 않았으므로 해제할 것이 없음). */
		return 0;
	/* [한국어] 3단계: numlvs가 0보다 크면(실제 LV가 존재하면) VGDA 다음 섹터(LVD 배열이 있는 위치)를 읽는다. */
	if (numlvs && (d = read_part_sector(state, vgda_sector + 1, &sect))) {
		/* [한국어] 원시 바이트 포인터를 struct lvd 배열로 캐스팅 - p[i]로 i번째 LVD 엔트리에 접근 가능. */
		struct lvd *p = (struct lvd *)d;
		/* [한국어] 아래 for 루프에서 쓰일 반복 인덱스 선언. */
		int i;

		/* [한국어] 이름 테이블 시작 섹터(VGDA 끝에서 33섹터 앞)를 계산해 alloc_lvn()으로 통째로 읽어온다. */
		n = alloc_lvn(state, vgda_sector + vgda_len - 33);
		/* [한국어] 이름 테이블 읽기에 성공했을 때만(n != NULL) LVD/PVD 처리를 계속 진행한다. */
		if (n) {
			/* [한국어] 지금까지 실제로 존재하는 것으로 확인된 LV 개수 카운터 - 0부터 시작. */
			int foundlvs = 0;

			/* [한국어] numlvs개를 모두 찾을 때까지(foundlvs < numlvs), 그리고 lvip 배열 상한(state->limit) 이내에서 LVD 배열을 순회. */
			for (i = 0; foundlvs < numlvs && i < state->limit; i += 1) {
				/* [한국어] i번째 LVD 엔트리의 num_lps(이 LV의 논리 파티션 총 개수)를 빅엔디안 변환해 lvip[i].pps_per_lv에 저장. */
				lvip[i].pps_per_lv = be16_to_cpu(p[i].num_lps);
				/* [한국어] 값이 0보다 크면(빈 슬롯이 아니라 실제 LV가 존재하면). */
				if (lvip[i].pps_per_lv)
					/* [한국어] 발견된 LV 카운터를 증가 - 바깥 for 루프의 종료 조건(foundlvs < numlvs)에 반영됨. */
					foundlvs += 1;
			}
			/* pvd loops depend on n[].name and lvip[].pps_per_lv */
			/* [한국어] LVD/이름 테이블을 모두 확보했으므로, 이제 물리 볼륨 서술자(PVD)를 고정 오프셋(vgda_sector+17)에서 읽어온다. */
			pvd = alloc_pvd(state, vgda_sector + 17);
		}
		/* [한국어] LVD 배열이 담긴 섹터의 캐시 참조를 해제. */
		put_dev_sector(sect);
	}
	/* [한국어] 4단계: PVD를 성공적으로 읽었다면(pvd != NULL) 물리 파티션 배열을 순회하며 연속된 LV를 찾아 파티션으로 등록한다. */
	if (pvd) {
		/* [한국어] 이 PV의 전체 물리 파티션(PP) 개수를 빅엔디안 변환해서 읽음 - 아래 순회 루프의 반복 횟수가 된다. */
		int numpps = be16_to_cpu(pvd->pp_count);
		/* [한국어] 데이터 영역이 시작하는 절대 섹터(PSN)를 빅엔디안 변환해서 읽음 - 파티션 시작 LBA 계산의 기준점. */
		int psn_part1 = be32_to_cpu(pvd->psn_part1);
		/* [한국어] 아래 for 루프에서 쓰일 반복 인덱스 선언. */
		int i;
		/* [한국어] 현재 "연속 구간 추적 중"인 LV의 인덱스(0-based) - -1은 "추적 중인 LV 없음"을 뜻한다. */
		int cur_lv_ix = -1;
		/* [한국어] 다음 PPE에서 기대하는 lp_ix 값 - 1로 초기화(다음 PP가 어떤 LV든 그 LV의 첫 번째 LP이어야 연속 시작으로 인정됨). */
		int next_lp_ix = 1;
		/* [한국어] 현재 PPE 엔트리에서 읽은 lp_ix 값을 담을 변수 선언(루프 밖에서도 값이 남아있을 필요는 없지만 편의상 밖에 선언됨). */
		int lp_ix;

		/* [한국어] PV의 물리 파티션(PP) 전체(numpps개)를 인덱스 순서(=디스크상의 물리적 순서)대로 순회. */
		for (i = 0; i < numpps; i += 1) {
			/* [한국어] i번째 PPE 엔트리를 가리키는 포인터 - ppe[] 배열에 대한 포인터 산술. */
			struct ppe *p = pvd->ppe + i;
			/* [한국어] 이번 PPE가 가리키는 LV 인덱스(0-based로 변환된 값)를 담을 변수 선언. */
			unsigned int lv_ix;

			/* [한국어] 이 PP가 속한 논리 파티션 번호(lp_ix)를 빅엔디안 변환해서 읽는다. */
			lp_ix = be16_to_cpu(p->lp_ix);
			/* [한국어] lp_ix가 0이면 "이 PP는 어떤 LV에도 할당되지 않은 미사용 슬롯"이라는 뜻이다. */
			if (!lp_ix) {
				/* [한국어] 연속성 추적 상태를 리셋 - 다음에 나올 PP는 다시 "어떤 LV든 그 첫 LP"부터 시작해야 연속으로 인정. */
				next_lp_ix = 1;
				/* [한국어] 이 PPE는 더 처리할 것이 없으므로 다음 반복(i+1)으로 건너뜀. */
				continue;
			}
			/* [한국어] PPE의 lv_ix(1-based)를 빅엔디안 변환 후 1을 빼 0-based 인덱스로 변환 - lvip[]/n[] 배열 인덱싱에 사용. */
			lv_ix = be16_to_cpu(p->lv_ix) - 1;
			/* [한국어] 변환된 lv_ix가 state->limit(등록 가능한 최대 파티션 수)을 넘는지 검사 - lvip[]/parts[] 배열 범위 밖 접근 방지. */
			if (lv_ix >= state->limit) {
				/* [한국어] 추적 중이던 LV 인덱스를 무효화(-1) - 범위를 벗어난 LV는 더 이상 연속 구간으로 취급하지 않는다. */
				cur_lv_ix = -1;
				/* [한국어] lvip[]에 안전하게 접근할 수 없으므로 이 PPE는 건너뛰고 다음(i+1)으로 진행. */
				continue;
			}
			/* [한국어] 범위 내 유효한 lv_ix이므로, 이 LV가 실제로 PPE에서 발견된 것으로 카운트를 증가시킨다. */
			lvip[lv_ix].pps_found += 1;
			/* [한국어] lp_ix가 1이면(해당 LV의 첫 번째 논리 파티션이면) 새로운 연속 구간의 시작으로 간주. */
			if (lp_ix == 1) {
				/* [한국어] 지금부터 이 LV(lv_ix)를 "현재 추적 중인 LV"로 설정. */
				cur_lv_ix = lv_ix;
				/* [한국어] 다음에 기대할 lp_ix를 1로 설정 - 아래 else if의 next_lp_ix 비교(else 분기에서 갱신되는 값)와 일관성을 맞추는 초기값. */
				next_lp_ix = 1;
			/* [한국어] lp_ix가 1이 아닌 경우: 이전과 다른 LV로 바뀌었거나(lv_ix != cur_lv_ix), 기대하던 순번이 아니면(lp_ix != next_lp_ix) 연속성이 깨진 것. */
			} else if (lv_ix != cur_lv_ix || lp_ix != next_lp_ix) {
				/* [한국어] 연속성이 깨졌으므로 추적 상태를 리셋 - 다음 PP부터 다시 새 연속 구간을 찾는다. */
				next_lp_ix = 1;
				/* [한국어] 이 PPE는 더 이상 유효한 연속 구간의 일부가 아니므로 완료 판정 없이 다음(i+1)으로 건너뜀. */
				continue;
			}
			/* [한국어] lp_ix가 이 LV의 전체 논리 파티션 개수(pps_per_lv=num_lps)와 같다면, 지금까지의 연속 구간이 이 LV의 전체 범위를 완주한 것. */
			if (lp_ix == lvip[lv_ix].pps_per_lv) {
				/* [한국어] 파티션을 등록: 파티션 번호는 lv_ix+1(1-based, 리눅스 파티션 넘버링). */
				put_partition(state, lv_ix + 1,
				  (i + 1 - lp_ix) * pp_blocks_size + psn_part1,
				  lvip[lv_ix].pps_per_lv * pp_blocks_size);
				/* [한국어] 커널 로그에 등록된 파티션의 논리 볼륨 이름을 " <이름>" 형태로 덧붙인다. */
				seq_buf_printf(&state->pp_buf, " <%s>\n",
					       n[lv_ix].name);
				/* [한국어] 이 LV가 연속으로 확인되어 파티션 등록까지 완료됐음을 표시 - 함수 말미 경고 루프가 이 플래그로 "비연속" 여부를 판단. */
				lvip[lv_ix].lv_is_contiguous = 1;
				/* [한국어] 하나 이상의 파티션을 등록했으므로 함수 최종 반환값을 1(성공)로 설정. */
				ret = 1;
				/* [한국어] 이 LV 구간 처리가 끝났으므로 다음 PPE를 위해 기대값을 다시 1로 리셋(다른 LV의 시작을 기다림). */
				next_lp_ix = 1;
			/* [한국어] 아직 이 LV의 마지막 LP에 도달하지 못했다면(연속은 유지되고 있지만 미완주). */
			} else
				/* [한국어] 다음 PPE에서는 lp_ix가 1 증가한 값이어야 연속으로 인정되므로 next_lp_ix를 증가시켜 둔다. */
				next_lp_ix += 1;
		}
		/* [한국어] PPE 순회가 끝난 후, lvip[] 전체(state->limit개)를 훑으며 "PP는 발견됐지만 연속으로 확정되지 않은" LV를 찾는다. */
		for (i = 0; i < state->limit; i += 1)
			/* [한국어] pps_found > 0(실제로 PP가 발견됨)이면서 lv_is_contiguous가 0(파티션으로 등록되지 못함)인 경우. */
			if (lvip[i].pps_found && !lvip[i].lv_is_contiguous) {
				/* [한국어] 이름을 널 종단 보장해서 담을 임시 스택 버퍼 - name[64]가 널 종단되어 있지 않을 수 있으므로 +1 바이트 여유를 둠. */
				char tmp[sizeof(n[i].name) + 1]; // null char

				/* [한국어] name 배열에서 최대 64바이트를 복사하되 snprintf가 널 종단을 보장하도록 함 - pr_warn에 안전하게 %s로 넘기기 위함. */
				snprintf(tmp, sizeof(tmp), "%s", n[i].name);
				pr_warn("partition %s (%u pp's found) is "
					"not contiguous\n",
					tmp, lvip[i].pps_found);
			}
		/* [한국어] PVD 버퍼(약 32KB)를 해제 - alloc_pvd()에서 kmalloc된 메모리. */
		kfree(pvd);
	}
	/* [한국어] 이름 테이블 버퍼를 해제 - alloc_lvn()에서 kmalloc된 메모리(n이 NULL이어도 kfree(NULL)은 안전하게 no-op). */
	kfree(n);
	/* [한국어] lvip[] 배열을 해제 - kzalloc_objs()로 할당된 메모리. */
	kfree(lvip);
	/* [한국어] 최종 결과(하나 이상의 파티션을 등록했으면 1, 아니면 0)를 호출자(check_partition)에게 반환. */
	return ret;
}
