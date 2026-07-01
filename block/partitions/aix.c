// SPDX-License-Identifier: GPL-2.0
/*
 *  fs/partitions/aix.c
 *
 *  Copyright (C) 2012-2013 Philippe De Muyter <phdm@macqel.be>
 */

/*
 * AIX LVM 파티션 탐지기 (block/partitions/aix.c)
 *
 * 이 파일은 IBM AIX Logical Volume Manager(LVM) 형식의 디스크 파티션을
 * 탐지하고 blk_layer가 인식할 수 있도록 파티션 테이블을 구성한다.
 * NVMe SSD 관점에서 본다면, 사용자가 submit_bio -> blk_mq_submit_bio ->
 * blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell) 경로로
 * NVMe 명령을 날리기 전, 커널 부팅/디스크 탐색 단계에서 디스크 메타데이터를
 * 읽어 NVMe LBA(논리 블록 주소) 공간을 파티션으로 나누는 선행 단계다.
 * read_part_sector() 호출은 최종적으로 NVMe 드라이버의 READ 명령으로
 * 매핑될 수 있다(추정).
 * 이 파일은 block/partitions/check.c, msdos.c, efi.c 등과 함께
 * 커널의 파티션 탐지 계열을 이루며, NVMe I/O 경로보다 상위에서 동작한다.
 */

#include "check.h"

/*
 * LVM 레코드 (LBA 7에 위치).
 * NVMe SSD가 부팅 시 처음으로 IDENTIFY/READ를 통해 노출하는 LBA 중
 * 7번째 섹터에서 이 레코드를 읽어 AIX LVM 존재 여부를 판별한다.
 */
struct lvm_rec {
	char lvm_id[4]; /* "_LVM" 매직, offset 0, NVMe LBA 7의 첫 4B */
	char reserved4[16]; /* offset 4, NVMe media reserved 영역 */
	__be32 lvmarea_len; /* offset 20, big-endian, NVMe READ 후 byte-swap */
	__be32 vgda_len; /* offset 24, VGDA 섹터 길이(추정) */
	__be32 vgda_psn[2]; /* offset 28, VGDA 시작 NVMe LBA(상위/하위 32bit) */
	char reserved36[10]; /* offset 36 */
	__be16 pp_size; /* offset 46, log2(pp_size) */
	char reserved46[12]; /* offset 48 */
	__be16 version; /* offset 60, AIX LVM 버전(매직/서명 검증 대신 사용) */
	};

/*
 * VGDA(Volume Group Descriptor Area).
 * AIX 볼륨 그룹의 메타데이터로, 이 볼륨 그룹에 속한 논리 볼륨 개수(numlvs),
 * 최대 개수(maxlvs), 물리 파티션 크기(pp_size) 등을 담는다.
 * NVMe 입장에서는 이 영역을 READ 하기 위해 추가 NVMe 명령(CID 할당 -> SQ
 * 삽입 -> doorbell 갱신)이 발생할 수 있다(추정).
 */
struct vgda {
	__be32 secs; /* offset 0, timestamp 초, NVMe media raw -> cpu endian 변환 */
	__be32 usec; /* offset 4, timestamp 마이크로초 */
	char reserved8[16]; /* offset 8 */
	__be16 numlvs; /* offset 24, 이 NVMe namespace 위 논리 볼륨 수 */
	__be16 maxlvs; /* offset 26, 최대 논리 볼륨 수 */
	__be16 pp_size; /* offset 28, 물리 파티션 크기(섹터 단위 log2) */
	__be16 numpvs; /* offset 30, 볼륨 그룹 내 물리 볼륨 수 */
	__be16 total_vgdas; /* offset 32, 총 VGDA 복사본 수 */
	__be16 vgda_size; /* offset 34, VGDA 섹터 수 */
	};

/*
 * LVD(Logical Volume Descriptor).
 * 각 논리 볼륨의 속성을 기술한다. lv_state, mirror_policy 등은 NVMe
 * 멀티네임스페이스나 레플리카와는 직접 매핑되지 않으며, 단지 AIX LVM
 * 낭비의 레이아웃 정볼만 제공한다(추정).
 */
struct lvd {
	__be16 lv_ix; /* offset 0, 논리 볼륨 인덱스 */
	__be16 res2; /* offset 2 */
	__be16 res4; /* offset 4 */
	__be16 maxsize; /* offset 6 */
	__be16 lv_state; /* offset 8 */
	__be16 mirror; /* offset 10 */
	__be16 mirror_policy; /* offset 12 */
	__be16 num_lps; /* offset 14, 이 LV의 논리 파티션 수 -> NVMe LBA 범위 계산에 사용 */
	__be16 res10[8]; /* offset 16 */
	};

/*
 * 논리 볼륨 이름. 파티션 탐지 결과 seq_buf_printf로 사용자에게 표시된다.
 * NVMe namespace label과는 별개다.
 */
struct lvname {
	char name[64]; /* offset 0, 64B 이름, NVMe media에서 16KiB 블록으로 256개 연속 저장 */
	};

/*
 * PPE(Physical Partition Entry).
 * 물리 파티션이 어느 논리 볼륨(lv_ix)의 어느 논리 파티션(lp_ix)에
 * 속하는지를 나타낸다. NVMe LBA 공간을 AIX LVM이 이차적으로 분할한
 * 정보다.
 */
struct ppe {
	__be16 lv_ix; /* offset 0, 이 PP가 속한 LV 인덱스(1-based) */
	unsigned short res2; /* offset 2 */
	unsigned short res4; /* offset 4 */
	__be16 lp_ix; /* offset 6, 이 PP가 LV 내 어느 논리 파티션인지 -> NVMe LBA 연속성 판단 */
	unsigned short res8[12]; /* offset 8 */
	};

/*
 * PVD(Physical Volume Descriptor).
 * 물리 볼륨 전체의 메타데이터와 1016개의 ppe 배열을 담는다.
 * pp_count는 이 물리 볼륨 내 물리 파티션 총개수이며, psn_part1은
 * 실제 데이터 영역이 시작하는 NVMe LBA(섹터 단위) 오프셋이다.
 */
struct pvd {
	char reserved0[16]; /* offset 0 */
	__be16 pp_count; /* offset 16, 이 PV의 총 physical partition 수 -> PPE loop 횟수 */
	char reserved18[2]; /* offset 18 */
	__be32 psn_part1; /* offset 20, 파티션 데이터 시작 NVMe LBA(섹터 오프셋) */
	char reserved24[8]; /* offset 24 */
	struct ppe ppe[1016]; /* offset 32, 각 entry 32B -> NVMe media에서 읽은 PPE 배열 */
	};

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
 *
 * NVMe 관점:
 *   파티션 탐색기가 NVMe namespace의 LBA lba부터 count 바이트를 읽는다.
 *   실제 I/O 경로는 read_part_sector -> bdev/bio -> submit_bio ->
 *   blk_mq_submit_bio -> blk_mq_get_request -> nvme_queue_rq ->
 *   nvme_submit_cmd(doorbell) 순으로 전달된다(추정).
 *   여기서는 512바이트 단위로 섹터를 읽어오며, NVMe PRP/SGL의
 *   세그먼트 정렬과는 직접 관련이 없다(상위 레이어가 캐시 버퍼 관리).
 */
static size_t read_lba(struct parsed_partitions *state, u64 lba, u8 *buffer,
			size_t count)
{
	size_t totalreadcount = 0;	/* NVMe media에서 누적 읽은 바이트 */

	/* NVMe namespace 총 용량을 넘어서는 읽기는 차단한다. */
	if (!buffer || lba + count / 512 > get_capacity(state->disk) - 1ULL)
		return 0;

	while (count) {	/* 남은 바이트마다 512바이트 NVMe READ 반복(추정) */
		int copied = 512;	/* NVMe namespace LBA data unit == 512B(추정) */
		Sector sect;
		/* read_part_sector 한 번 호출 == NVMe READ 1섹터(추정). */
		unsigned char *data = read_part_sector(state, lba++, &sect);
		if (!data)
			break;	/* NVMe CQE error 또는 메모리 부족으로 READ 실패 시 중단 */
		if (copied > count)
			copied = count;	/* 마지막 섹터는 부분 바이트만 복사 */
		memcpy(buffer, data, copied); /* 버퍼로 복사 후 섹터 해제. */
		put_dev_sector(sect);
		buffer += copied;	/* 목표 버퍼 포인터 전진 */
		totalreadcount += copied;	/* 누적 읽기 바이트 갱신 */
		count -= copied;	/* 남은 읽기 요구량 감소 */
	}
	return totalreadcount;	/* 성공 시 NVMe media로부터 복사된 총 바이트 */
}

/**
 * alloc_pvd(): reads physical volume descriptor
 * @state
 * @lba
 *
 * Description: Returns pvd on success,  NULL on error.
 * Allocates space for pvd and fill it with disk blocks at @lba
 * Notes: remember to free pvd when you're done!
 *
 * NVMe 관점:
 *   AIX LVM의 PVD를 메모리에 할당한 뒤 read_lba()로 NVMe LBA @lba부터
 *   읽어온다. kmalloc으로 할당된 버퍼는 NVMe DMA 대상이 될 수 있으며,
 *   read_part_sector 하위에서 필요 시 bounce buffer로 복사될 수 있다(추정).
 */
static struct pvd *alloc_pvd(struct parsed_partitions *state, u32 lba)
{
	size_t count = sizeof(struct pvd);	/* sizeof(pvd)만큼 NVMe READ 발생 가능 */
	struct pvd *p;

	p = kmalloc(count, GFP_KERNEL);
	if (!p)
		return NULL;	/* 메모리 부족 시 NVMe media 탐색 중단 */

	if (read_lba(state, lba, (u8 *) p, count) < count) {
		kfree(p);
		return NULL;	/* NVMe READ가 PVD 전체를 읽지 못하면 해제 후 실패 */
	}
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
 *
 * NVMe 관점:
 *   논리 볼륨 이름 테이블을 NVMe LBA @lba에서 읽어온다.
 *   LVM_MAXLVS(256)개 이름을 저장하므로 최대 16KiB 읽기가 발생할 수 있고,
 *   이는 read_lba 내부의 while 루프에서 여러 NVMe READ 명령으로 분할될 수
 *   있다(추정).
 */
static struct lvname *alloc_lvn(struct parsed_partitions *state, u32 lba)
{
	size_t count = sizeof(struct lvname) * LVM_MAXLVS;	/* 256 * 64B = 16KiB */
	struct lvname *p;

	p = kmalloc(count, GFP_KERNEL);
	if (!p)
		return NULL;	/* 메모리 부족 시 파티션 이름 탐색 불가 */

	if (read_lba(state, lba, (u8 *) p, count) < count) {
		kfree(p);
		return NULL;	/* NVMe READ 미완료 시 이름 테이블 폐기 */
	}
	return p;
}

/**
 * aix_partition() - AIX LVM 파티션 탐지의 진입점.
 * @state: 파티션 파싱 상태 (디스크, 출력 버퍼, 제한 등 포함).
 *
 * 목적:
 *   NVMe namespace가 AIX LVM으로 포맷되어 있는지 확인하고, 발견된
 *   논리 볼륨을 파티션으로 등록한다.
 *
 * 호출 경로 (NVMe 기준 상위 -> 하위):
 *   rescan_partitions -> check_partition -> aix_partition (이 파일)
 *   낭부 읽기는 read_part_sector -> submit_bio -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)로
 *   전달된다(추정).
 *
 * NVMe 연결점:
 *   - LBA 7의 lvm_rec 읽기: NVMe READ 명령 1회 발생 가능.
 *   - VGDA/LVD/PVD/lvname 읽기: 추가 NVMe READ 명령 발생 가능.
 *   - put_partition()으로 파티션 등록: 이후 사용자 I/O가 NVMe READ/WRITE
 *     명령으로 변환될 때 파티션 시작 LBA가 bio의 시작 오프셋에 더해진다.
 */
int aix_partition(struct parsed_partitions *state)
{
	int ret = 0;	/* 파티션 발견 여부, 1이면 NVMe 위에 block device 생성됨 */
	Sector sect;	/* read_part_sector가 반환한 캐시 섹터 핸들 */
	unsigned char *d;	/* NVMe media에서 읽어온 섹터 데이터 포인터 */
	u32 pp_bytes_size;	/* physical partition 크기(바이트) */
	u32 pp_blocks_size = 0;	/* physical partition 크기(512B 섹터 수) -> NVMe LBA 단위 */
	u32 vgda_sector = 0;	/* VGDA가 위치한 NVMe LBA */
	u32 vgda_len = 0;	/* VGDA 길이(섹터 단위) */
	int numlvs = 0;		/* 이 볼륨 그룹의 논리 볼륨 개수 */
	struct pvd *pvd = NULL;	/* NVMe LBA에서 읽어온 physical volume descriptor */
	struct lv_info {
		unsigned short pps_per_lv;
		unsigned short pps_found;
		unsigned char lv_is_contiguous;
	} *lvip;	/* 각 파티션의 연속성/완성도 추적 버퍼 */
	struct lvname *n = NULL;	/* NVMe media에서 읽어온 논리 볼륨 이름 배열 */

	/* NVMe LBA 7에서 LVM 레코드를 읽는다. */
	d = read_part_sector(state, 7, &sect);
	if (d) {
		struct lvm_rec *p = (struct lvm_rec *)d;	/* LBA 7의 on-disk lvm_rec 매핑 */
		u16 lvm_version = be16_to_cpu(p->version);

		/* AIX LVM은 전통적인 매직 시그니처(lvm_id == "_LVM") 대신
		 * version 필드가 1인지만 검사한다. NVMe CQE 성공 후에도
		 * on-disk 레이아웃이 AIX LVM인지는 이 버전 체크가 유일한
		 * 검증 경로다(추정).
		 */
		if (lvm_version == 1) {
			int pp_size_log2 = be16_to_cpu(p->pp_size);

			pp_bytes_size = 1 << pp_size_log2;		/* AIX physical partition 크기, NVMe LBA 정렬 단위(추정) */
			pp_blocks_size = pp_bytes_size / 512;	/* 바이트 -> 512B 섹터(LBA) 단위 변환 */
			seq_buf_printf(&state->pp_buf,
				       " AIX LVM header version %u found\n",
				       lvm_version);
			vgda_len = be32_to_cpu(p->vgda_len);	/* VGDA가 차지하는 NVMe 섹터 수 */
			vgda_sector = be32_to_cpu(p->vgda_psn[0]);	/* VGDA 시작 NVMe LBA */
		} else {
			seq_buf_printf(&state->pp_buf,
				       " unsupported AIX LVM version %d found\n",
				       lvm_version);
		}
		put_dev_sector(sect);
	}
	/* VGDA 첫 섹터를 읽어 논리 볼륨 개수(numlvs)를 확인한다. */
	if (vgda_sector && (d = read_part_sector(state, vgda_sector, &sect))) {
		struct vgda *p = (struct vgda *)d;

		numlvs = be16_to_cpu(p->numlvs);	/* NVMe namespace를 분할할 논리 볼륨 개수 */
		put_dev_sector(sect);
	}
	/* 탐지 가능한 최대 파티션 수(state->limit)만큼 lv_info 할당. */
	lvip = kzalloc_objs(struct lv_info, state->limit);
	if (!lvip)
		return 0;	/* 메모리 부족 시 AIX LVM 파티션 탐색 전체 중단 */
	/* VGDA 두 번째 섹터에서 LVD, 이름 테이블, PVD를 순차 읽는다. */
	if (numlvs && (d = read_part_sector(state, vgda_sector + 1, &sect))) {
		struct lvd *p = (struct lvd *)d;	/* LVD 배열이 들어있는 섹터 매핑 */
		int i;

		/* 이름 테이블은 VGDA 마지막에서 33섹터 앞에 위치(추정). */
		n = alloc_lvn(state, vgda_sector + vgda_len - 33);
		if (n) {
			int foundlvs = 0;	/* 실제 존재하는 논리 볼륨 카운터 */

			/* numlvs만큼 반복, state->limit은 NVMe namespace 위 등록 가능한 최대 block device 수 */
			for (i = 0; foundlvs < numlvs && i < state->limit; i += 1) {
				lvip[i].pps_per_lv = be16_to_cpu(p[i].num_lps);
				if (lvip[i].pps_per_lv)
					foundlvs += 1;		/* 실제 볼륨 발견 시 카운트 */
			}
			/* pvd loops depend on n[].name and lvip[].pps_per_lv */
			pvd = alloc_pvd(state, vgda_sector + 17);	/* NVMe LBA vgda_sector+17에서 PVD READ */
		}
		put_dev_sector(sect);
	}
	if (pvd) {
		int numpps = be16_to_cpu(pvd->pp_count);	/* 이 물리 볼륨의 총 physical partition 수 */
		int psn_part1 = be32_to_cpu(pvd->psn_part1);	/* 실제 데이터 영역 시작 NVMe LBA 오프셋 */
		int i;
		int cur_lv_ix = -1;		/* 현재 추적 중인 논리 볼륨 인덱스 */
		int next_lp_ix = 1;		/* 기대하는 다음 논리 파티션 번호 */
		int lp_ix;

		/*
		 * PPE 배열을 순회하며 연속된 논리 파티션들을 하나의 파티션으로
		 * 등록한다. NVMe LBA 계산은 (물리 파티션 인덱스 * 블록 크기 +
		 * psn_part1) 형태로 이루어진다.
		 * AIX LVM에는 msdos의 extended partition chain이 없으므로,
		 * 추가 NVMe READ/WRITE 폭증 없이 PVD 한 번 읽기로 모든 파티션
		 * 레이아웃을 파악할 수 있다(추정).
		 */
		for (i = 0; i < numpps; i += 1) {	/* 1016개 PPE entry 순회, 각 entry는 NVMe media의 한 PPE이다 */
			struct ppe *p = pvd->ppe + i;	/* 현재 physical partition entry */
			unsigned int lv_ix;

			lp_ix = be16_to_cpu(p->lp_ix);	/* 이 PP가 속한 논리 파티션 번호 */
			if (!lp_ix) {
				next_lp_ix = 1;
				continue;	/* 미사용 PP -> 다음 NVMe LBA로 이동 */
			}
			lv_ix = be16_to_cpu(p->lv_ix) - 1;	/* 1-based -> 0-based 인덱스 변환 */
			if (lv_ix >= state->limit) {
				cur_lv_ix = -1;
				continue;	/* 등록 한도 초과 시 무시, NVMe queue 리소스 보호 */
			}
			lvip[lv_ix].pps_found += 1;	/* 발견된 PP 수 누적 */
			/*
			 * lp_ix == 1이면 새로운 논리 볼륨의 시작으로 간주.
			 * 그렇지 않으면 이전 lp_ix와 연속성을 검사한다.
			 */
			if (lp_ix == 1) {
				cur_lv_ix = lv_ix;
				next_lp_ix = 1;
			} else if (lv_ix != cur_lv_ix || lp_ix != next_lp_ix) {
				next_lp_ix = 1;
				continue;	/* 연속성 깨짐 -> 다음 PP로 스킵 */
			}
			/*
			 * 마지막 lp_ix까지 연속이면 put_partition으로 NVMe namespace
			 * LBA 공간에 파티션을 등록한다.
			 */
			if (lp_ix == lvip[lv_ix].pps_per_lv) {
				put_partition(state, lv_ix + 1,	/* block device 번호 */
				  (i + 1 - lp_ix) * pp_blocks_size + psn_part1,	/* 파티션 시작 NVMe LBA */
				  lvip[lv_ix].pps_per_lv * pp_blocks_size);	/* 파티션 섹터 수 */
				seq_buf_printf(&state->pp_buf, " <%s>\n",
					       n[lv_ix].name);
				lvip[lv_ix].lv_is_contiguous = 1;
				ret = 1;	/* NVMe 위 block device 생성 성공 */
				next_lp_ix = 1;
			} else
				next_lp_ix += 1;	/* 다음 lp_ix 기대 */
		}
		/* 연속되지 않은 볼륨은 경고만 출력하고 파티션으로 등록하지 않는다. */
		for (i = 0; i < state->limit; i += 1)
			if (lvip[i].pps_found && !lvip[i].lv_is_contiguous) {
				char tmp[sizeof(n[i].name) + 1]; // null char

				snprintf(tmp, sizeof(tmp), "%s", n[i].name);
				pr_warn("partition %s (%u pp's found) is "
					"not contiguous\n",
					tmp, lvip[i].pps_found);
			}
		kfree(pvd);	/* NVMe media에서 복사된 메타데이터 해제 */
	}
	kfree(n);	/* NVMe media에서 복사된 논리 볼륨 이름 해제 */
	kfree(lvip);	/* 파티션 추적 버퍼 해제 */
	return ret;	/* 0이면 NVMe 위에 AIX 파티션 block device 미생성, 1이면 생성됨 */
}

/* NVMe 관점 핵심 요약 */
/*
 * - 본 파일은 NVMe READ/WRITE 경로보다 상위에 위치한 파티션 탐색기로,
 *   부팅/디스크 재스캔 시에만 실행되며 I/O 핫패스는 아니다.
 * - read_part_sector -> submit_bio -> blk_mq_submit_bio ->
 *   blk_mq_get_request -> nvme_queue_rq -> nvme_submit_cmd(doorbell)
 *   순으로 NVMe 명령이 발생할 수 있다(추정).
 * - AIX LVM 메타데이터(LVM 레코드, VGDA, LVD, PVD, lvname)를 NVMe LBA
 *   공간에서 읽어 논리 볼륨을 파티션으로 등록한다.
 * - put_partition()으로 등록된 파티션은 이후 사용자 I/O의 bio 오프셋에
 *   더해져 NVMe 명령의 SLBA가 결정된다.
 * - block/partitions/check.c, efi.c, msdos.c 등과 함께 파티션 탐지
 *   서브시스템을 구성한다.
 */
