// SPDX-License-Identifier: GPL-2.0
/*
 * Sysfs interface for the NVMe core driver.
 *
 * Copyright (c) 2011-2014, Intel Corporation.
 */

/*
 * [한국어 설명] NVMe 호스트 코어 sysfs 속성 구현 (sysfs.c)
 *
 * === 파일의 역할 ===
 * 이 파일은 NVMe 서브시스템·컨트롤러·네임스페이스를 사용자 공간에 노출하는
 * sysfs 속성(attribute) 그룹과 show/store 콜백을 모두 구현한다. 실제 I/O
 * 데이터 경로는 다루지 않으며, 관리·관측·튜닝 인터페이스만 담당한다.
 * 대표 노출 경로:
 *   - /sys/class/nvme/nvmeX/ … 컨트롤러 장치 속성
 *       (model, serial, state, transport, reset_controller, delete_controller,
 *        hostnqn, ctrl_loss_tmo, dhchap_secret, passthru_err_log_enabled 등)
 *   - /sys/block/nvmeXnY/ … 네임스페이스 블록 장치 속성
 *       (wwid, uuid, nguid, eui, nsid, csi, metadata_bytes, nuse,
 *        multipath/⋆, io passthru 에러 로그 스위치 등)
 *   - /sys/class/nvme-subsystem/nvme-subsysX/ … 서브시스템 공통 속성
 *       (subsysnqn, model, serial, firmware_rev, iopolicy 등)
 *
 * show 는 대부분 ctrl/ns_head/subsys 구조체 필드를 sysfs_emit 으로 포맷하고,
 * store 는 kstrto* 로 파싱한 뒤 옵션을 갱신하거나 nvme_reset_ctrl_sync /
 * nvme_delete_ctrl_sync / nvme_queue_scan / 인증 워크 등을 트리거한다.
 * is_visible 콜백으로 트랜스포트·기능에 없는 속성을 숨긴다(예: PCIe 에
 * hostnqn 없음, fabrics 전용 loss_tmo, TLS 그룹은 tcp 만).
 *
 * === 전체 아키텍처에서의 위치 ===
 * 디바이스 모델 계층의 “표현 계층”이다. core.c 가 nvme_ctrl / nvme_ns /
 * nvme_subsystem 을 등록할 때 이 파일이 export 하는 attribute_group 배열을
 * 장치 타입/디스크에 붙인다. 유저 흐름 예:
 *   echo 1 > /sys/class/nvme/nvme0/reset_controller
 *     → nvme_sysfs_reset → nvme_reset_ctrl_sync → 전송 계층 리셋 상태머신
 *   cat /sys/block/nvme0n1/wwid
 *     → wwid_show → ns_head ids / subsys 식별자로 안정적 WWID 문자열 생성
 *
 * 실행 컨텍스트: sysfs 읽기/쓰기는 프로세스 컨텍스트이며, store 가 리셋·삭제
 * ·Identify 를 호출하면 sleep 한다. nuse_show 는 rate limit 후 Identify NS 를
 * 발행해 최신 NUSE 를 갱신한다(관리 경로 I/O). multipath head 의 경로 의존
 * 속성은 SRCU + nvme_find_path 로 현재 경로를 고른다.
 *
 * === 타 모듈과의 연결 ===
 * - nvme.h / core.c: 구조체 필드, nvme_reset_ctrl(_sync), nvme_delete_ctrl_sync,
 *   nvme_queue_scan, nvme_identify_ns, nvme_ctrl_state, quirk 이름 등
 * - fabrics.h / fabrics.c: nvmf_ctrl_options (reconnect, loss_tmo, host nqn/id,
 *   dhchap, tls_key, transport 이름) — fabrics 컨트롤러에만 opts 가 존재
 * - multipath.c: ana_*, queue_depth, numa_nodes, delayed_removal, iopolicy
 *   속성 심볼(동일 그룹에 포인터로 연결). head vs path 가시성 분기
 * - auth.c (CONFIG_NVME_HOST_AUTH): dhchap secret 파싱/재인증 워크
 * - nvme-auth / TLS (CONFIG_NVME_TCP_TLS): tls_key, configured_key, keyring
 * - block/genhd, fs/sysfs: gendisk/device kobject 아래에 그룹 등록
 * - ioctl.c 의 passthru_err_log_enabled 와 대응: Admin 은 ctrl, I/O 는 ns_head
 *
 * === 주요 심볼 요약 ===
 * NS: nvme_ns_attr_groups, wwid/uuid/…/nuse_show, dev_to_ns_head,
 *     ns_*_update_nuse, multipath 그룹(더미 attr 로 group 가시성)
 * CTRL: nvme_dev_attrs_group, reset/rescan/delete, state/transport,
 *       fabrics 튜너블, cntrltype/dctype/quirks, auth/TLS 그룹
 * SUBSYS: nvme_subsys_attrs_groups, nqn/type/model/serial/fw
 * 매크로: nvme_show_str_function, nvme_show_int_function,
 *         nvme_subsys_show_str_function, SUBSYS_ATTR_RO
 */

#include <linux/nvme-auth.h>
/* [한국어] DH-HMAC-CHAP/TLS 관련 인증 API 선언 — secret store 경로의
 * nvme_auth_parse_key, nvme_auth_stop/negotiate/wait, free_key 등 */

#include "nvme.h"
/* [한국어] nvme_ctrl/ns/ns_head/subsystem, 상태 enum, identify/reset/scan,
 * multipath 헬퍼, quirk 이름, 속성 그룹 extern 짝 구현 */
#include "fabrics.h"
/* [한국어] struct nvmf_ctrl_options 및 fabrics 연결 옵션 필드 접근 —
 * hostnqn, reconnect_delay, ctrl_loss_tmo, tls 등 show/store 에 필요 */

/*
 * [한국어]
 * nvme_sysfs_reset - "reset_controller" store: 동기 컨트롤러 리셋
 *
 * @dev: nvme 클래스 컨트롤러 device (drvdata = nvme_ctrl).
 * @attr/@buf/@count: sysfs store 표준 인자(내용은 무시, 쓰기 행위 자체가 트리거).
 * @return: 성공 시 count, 실패 시 음수 errno.
 *
 * 왜 필요한가: 운영자가 드라이버 상태머신 전체를 재시작(큐 재생성, Identify
 * 재수행)해야 할 때 nvme-cli reset 과 동일한 커널 경로를 sysfs 로 제공.
 * 호출: 유저 echo → device_attribute store → nvme_reset_ctrl_sync.
 * 컨텍스트: 프로세스, sleep 가능. 권한은 sysfs 파일 모드(S_IWUSR)로 제한.
 *
 * 호출 체인:
 *   sysfs write → [nvme_sysfs_reset] → nvme_reset_ctrl_sync → 리셋 워크/완료
 */
static ssize_t nvme_sysfs_reset(struct device *dev,	/* [한국어] sysfs device — drvdata=nvme_ctrl */
				struct device_attribute *attr, const char *buf,	/* [한국어] 쓰기 트리거; 내용은 무시 */
				size_t count)	/* [한국어] 성공 시 그대로 반환할 바이트 수 */
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 이 sysfs 노드에 묶인 컨트롤러 인스턴스 */
	int ret;	/* [한국어] 리셋 결과 errno 누적 */

	ret = nvme_reset_ctrl_sync(ctrl);	/* [한국어] 비동기 리셋을 시작하고 완료/실패까지 대기 */
	if (ret < 0)
		return ret;	/* [한국어] 리셋 실패 errno 를 sysfs 쓰기에 그대로 전달 */
	return count;	/* [한국어] sysfs 규약: 성공 시 소비한 바이트 수 반환 */
}
static DEVICE_ATTR(reset_controller, S_IWUSR, NULL, nvme_sysfs_reset);	/* [한국어] WO reset_controller 속성 객체 */
/* [한국어] /sys/class/nvme/nvmeX/reset_controller — root 쓰기 전용, show 없음 */

/*
 * [한국어]
 * nvme_sysfs_rescan - "rescan_controller" store: 네임스페이스 재스캔 요청
 *
 * Identify 기반 NS 목록을 다시 읽어 추가/제거/갱신 워크를 큐에 넣는다.
 * 즉시 완료를 기다리지 않고 count 반환(비동기 scan).
 * 호출 체인: sysfs → [nvme_sysfs_rescan] → nvme_queue_scan
 */
static ssize_t nvme_sysfs_rescan(struct device *dev,	/* [한국어] 컨트롤러 device */
				struct device_attribute *attr, const char *buf,	/* [한국어] 트리거 전용 store */
				size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 스캔 대상 컨트롤러 */

	nvme_queue_scan(ctrl);	/* [한국어] scan 워크를 nvme_wq 에 스케줄 — 블로킹 없이 반환 */
	return count;	/* [한국어] 비동기 스캔이므로 즉시 store 성공 */
}
static DEVICE_ATTR(rescan_controller, S_IWUSR, NULL, nvme_sysfs_rescan);	/* [한국어] WO rescan_controller 속성 객체 */
/* [한국어] 쓰기 전용 rescan 트리거 속성 */

/*
 * [한국어]
 * nvme_adm_passthru_err_log_enabled_show/store
 * - 컨트롤러 Admin 패스스루 실패 시 상세 에러 로그 on/off
 *
 * ioctl.c 패스스루 경로가 ctrl->passthru_err_log_enabled 를 보고 dev_err 수준
 * 로그를 남길지 결정. 디버깅 시 on, 프로덕션 노이즈 줄일 때 off.
 * 속성 이름은 공통 "passthru_err_log_enabled" 이나 Admin 용 별도 device_attr
 * 심볼(dev_attr_adm_…)로 컨트롤러 그룹에 붙는다.
 */
static ssize_t nvme_adm_passthru_err_log_enabled_show(struct device *dev,	/* [한국어] 컨트롤러 class device */
		struct device_attribute *attr, char *buf)	/* [한국어] PAGE_SIZE sysfs 출력 버퍼 */
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] Admin 로그 스위치 소유 컨트롤러 */

	return sysfs_emit(buf,
			  ctrl->passthru_err_log_enabled ? "on\n" : "off\n");	/* [한국어] bool 을 사람이 읽는 on/off 문자열로 */
}

static ssize_t nvme_adm_passthru_err_log_enabled_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 토글 대상 컨트롤러 */
	bool passthru_err_log_enabled;	/* [한국어] kstrtobool 이 파싱할 목표 값 */
	int err;	/* [한국어] 파싱 errno */

	err = kstrtobool(buf, &passthru_err_log_enabled);	/* [한국어] "0/1/y/n/on/off" 등 허용 */
	if (err)
		return -EINVAL;	/* [한국어] 파싱 실패를 EINVAL 로 정규화 */

	ctrl->passthru_err_log_enabled = passthru_err_log_enabled;	/* [한국어] Admin 패스스루 로그 스위치 즉시 반영(락 없음 — 단일 bool) */

	return count;	/* [한국어] store 성공 바이트 수 */
}

/*
 * [한국어]
 * dev_to_ns_head - block device sysfs 노드에서 nvme_ns_head 를 얻음
 *
 * @dev: gendisk 의 device ( /sys/block/nvme… ).
 * @return: multipath head 디스크면 private_data 가 head, 단일 경로 디스크면
 *          ns->head.
 *
 * NS 속성(wwid, nsid 등)은 경로가 아니라 논리 네임스페이스(head) 단위로
 * 동일해야 하므로 항상 head 로 정규화한다.
 */
static inline struct nvme_ns_head *dev_to_ns_head(struct device *dev)
{
	struct gendisk *disk = dev_to_disk(dev);	/* [한국어] block device kobj → gendisk */

	if (nvme_disk_is_ns_head(disk))
		return disk->private_data;	/* [한국어] multipath 가상 디스크: private_data = ns_head */
	return nvme_get_ns_from_dev(dev)->head;	/* [한국어] 경로 디스크: ns 에서 공유 head 로 */
}

/*
 * [한국어]
 * I/O 패스스루 에러 로그 스위치 — ns_head 단위
 * Admin 과 이름 같으나 head->passthru_err_log_enabled 를 다룸(NS I/O ioctl).
 */
static ssize_t nvme_io_passthru_err_log_enabled_show(struct device *dev,	/* [한국어] NS 블록 device */
		struct device_attribute *attr, char *buf)
{
	struct nvme_ns_head *head = dev_to_ns_head(dev);	/* [한국어] 경로 정규화 후 head */

	return sysfs_emit(buf, head->passthru_err_log_enabled ? "on\n" : "off\n");	/* [한국어] I/O 패스스루 로그 on/off */
}

static ssize_t nvme_io_passthru_err_log_enabled_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ns_head *head = dev_to_ns_head(dev);	/* [한국어] 공유 head 필드 갱신 */
	bool passthru_err_log_enabled;	/* [한국어] 파싱 목표 bool */
	int err;	/* [한국어] kstrtobool 결과 */

	err = kstrtobool(buf, &passthru_err_log_enabled);	/* [한국어] "on/off/0/1" 파싱 */
	if (err)
		return -EINVAL;	/* [한국어] 잘못된 토글 문자열 */
	head->passthru_err_log_enabled = passthru_err_log_enabled;	/* [한국어] 동일 head 를 공유하는 multipath 경로 전체에 적용 */

	return count;	/* [한국어] store 성공 */
}

static struct device_attribute dev_attr_adm_passthru_err_log_enabled = \
	__ATTR(passthru_err_log_enabled, S_IRUGO | S_IWUSR, \
	nvme_adm_passthru_err_log_enabled_show, nvme_adm_passthru_err_log_enabled_store);	/* [한국어] Admin 패스스루 로그 attr */
/* [한국어] 컨트롤러 장치용 passthru_err_log_enabled 속성 객체 */

static struct device_attribute dev_attr_io_passthru_err_log_enabled = \
	__ATTR(passthru_err_log_enabled, S_IRUGO | S_IWUSR, \
	nvme_io_passthru_err_log_enabled_show, nvme_io_passthru_err_log_enabled_store);	/* [한국어] I/O 패스스루 로그 attr */
/* [한국어] NS 블록 장치용 동명 속성 — 다른 show/store 로 head 필드 조작 */

/*
 * [한국어]
 * wwid_show - 네임스페이스의 안정적 World Wide ID 문자열 생성
 *
 * 우선순위: UUID → NGUID → EUI64 → 없으면 vendor+serial+model+nsid 합성.
 * multipath/udev/컨테이너가 장치 경로가 바뀌어도 동일 볼륨을 식별하는 키.
 * serial/model 은 트레일링 스페이스·NUL 을 잘라 읽기 좋은 형태로.
 * 호출 체인: cat wwid → [wwid_show] → sysfs_emit
 */
static ssize_t wwid_show(struct device *dev, struct device_attribute *attr,
		char *buf)	/* [한국어] sysfs 페이지 버퍼에 WWID 한 줄 기록 */
{
	struct nvme_ns_head *head = dev_to_ns_head(dev);	/* [한국어] 경로 독립 식별자는 head 에 존재 */
	struct nvme_ns_ids *ids = &head->ids;	/* [한국어] Identify 로 채운 uuid/nguid/eui64/csi */
	struct nvme_subsystem *subsys = head->subsys;	/* [한국어] 합성 WWID 용 vendor/serial/model */
	int serial_len = sizeof(subsys->serial);	/* [한국어] 고정 배열 길이에서 유효 문자만 남기기 위한 커서 */
	int model_len = sizeof(subsys->model);	/* [한국어] 모델 문자열 유효 길이 커서 */

	if (!uuid_is_null(&ids->uuid))
		return sysfs_emit(buf, "uuid.%pU\n", &ids->uuid);	/* [한국어] 최우선: NVMe Namespace UUID */

	if (memchr_inv(ids->nguid, 0, sizeof(ids->nguid)))
		return sysfs_emit(buf, "eui.%16phN\n", ids->nguid);	/* [한국어] NGUID 16바이트를 eui. 접두 헥스로(콜론 없음) */

	if (memchr_inv(ids->eui64, 0, sizeof(ids->eui64)))
		return sysfs_emit(buf, "eui.%8phN\n", ids->eui64);	/* [한국어] 구형 EUI-64 */

	while (serial_len > 0 && (subsys->serial[serial_len - 1] == ' ' ||
				  subsys->serial[serial_len - 1] == '\0'))
		serial_len--;	/* [한국어] Identify 문자열 패딩 공백/NUL 제거 */
	while (model_len > 0 && (subsys->model[model_len - 1] == ' ' ||
				 subsys->model[model_len - 1] == '\0'))
		model_len--;	/* [한국어] 모델 패딩 트리밍 — 합성 WWID 안정성 */

	return sysfs_emit(buf, "nvme.%04x-%*phN-%*phN-%08x\n", subsys->vendor_id,
		serial_len, subsys->serial, model_len, subsys->model,
		head->ns_id);	/* [한국어] 최후 수단 합성 WWID — VID+SN+MN+NSID 로 전역 유일성 근사 */
}
static DEVICE_ATTR_RO(wwid);	/* [한국어] 읽기 전용 wwid */

/*
 * [한국어] nguid_show — Namespace Globally Unique Identifier 16바이트를 UUID 포맷으로 출력
 */
static ssize_t nguid_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return sysfs_emit(buf, "%pU\n", dev_to_ns_head(dev)->ids.nguid);	/* [한국어] 16B NGUID 를 UUID 텍스트로 */
}
static DEVICE_ATTR_RO(nguid);	/* [한국어] 읽기 전용 nguid */

/*
 * [한국어]
 * uuid_show - Namespace UUID. 없으면 과거 호환으로 NGUID 를 출력하며 1회 경고
 *
 * 초기 구현이 uuid 속성에 NGUID 를 넣었던 ABI 를 깨지 않기 위한 호환.
 * 신규 사용자는 nguid/wwid 를 쓰는 것이 명확하다.
 */
static ssize_t uuid_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct nvme_ns_ids *ids = &dev_to_ns_head(dev)->ids;	/* [한국어] head 공유 식별자 집합 */

	/* For backward compatibility expose the NGUID to userspace if
	 * we have no UUID set
	 */
	if (uuid_is_null(&ids->uuid)) {
		dev_warn_once(dev,
			"No UUID available providing old NGUID\n");	/* [한국어] 잘못된 필드 사용을 알리는 1회 경고 */
		return sysfs_emit(buf, "%pU\n", ids->nguid);	/* [한국어] 레거시: uuid 파일에 NGUID 내용 */
	}
	return sysfs_emit(buf, "%pU\n", &ids->uuid);	/* [한국어] 정식 Namespace UUID */
}
static DEVICE_ATTR_RO(uuid);	/* [한국어] 읽기 전용 uuid (ABI 호환) */

/*
 * [한국어] eui_show — 8바이트 EUI-64 를 헥스 덤프
 */
static ssize_t eui_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return sysfs_emit(buf, "%8ph\n", dev_to_ns_head(dev)->ids.eui64);	/* [한국어] EUI-64 8바이트 헥스 */
}
static DEVICE_ATTR_RO(eui);	/* [한국어] 읽기 전용 eui */

/*
 * [한국어] nsid_show — NVMe Namespace ID (컨트롤러 스코프 정수)
 */
static ssize_t nsid_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return sysfs_emit(buf, "%d\n", dev_to_ns_head(dev)->ns_id);	/* [한국어] 논리 NSID (head 공유) */
}
static DEVICE_ATTR_RO(nsid);	/* [한국어] 읽기 전용 nsid */

/*
 * [한국어] csi_show — Command Set Identifier (NVM/ZNS/Key-Value 등)
 */
static ssize_t csi_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return sysfs_emit(buf, "%u\n", dev_to_ns_head(dev)->ids.csi);	/* [한국어] CSI — 블록 ops 분기 키 */
}
static DEVICE_ATTR_RO(csi);	/* [한국어] 읽기 전용 csi */

/*
 * [한국어] metadata_bytes_show — LBA 당 메타데이터 바이트(ms). PI/확장 LBA 구성 파악용
 */
static ssize_t metadata_bytes_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%u\n", dev_to_ns_head(dev)->ms);	/* [한국어] Identify 기반 ms 캐시 */
}
static DEVICE_ATTR_RO(metadata_bytes);	/* [한국어] 읽기 전용 metadata_bytes */

/*
 * [한국어]
 * ns_head_update_nuse - multipath head 에서 Identify 로 NUSE 갱신
 *
 * @head: 논리 네임스페이스.
 * @return: 0, rate limit 중 0, 경로 없으면 -EWOULDBLOCK, identify 실패 errno.
 *
 * NUSE(Namespace Utilization)는 thin 프로비저닝 사용량. 매번 Identify 하면
 * Admin 부하가 커서 head->rs_nuse ratelimit 로 빈도 제한. SRCU 하 find_path
 * 로 한 경로 컨트롤러에 Identify NS 발행 후 head->nuse 캐시 갱신.
 *
 * 호출 체인:
 *   nuse_show → [ns_head_update_nuse] → nvme_identify_ns
 */
static int ns_head_update_nuse(struct nvme_ns_head *head)
{
	struct nvme_id_ns *id;	/* [한국어] Identify Namespace 데이터 구조(힙 할당, 호출 후 free) */
	struct nvme_ns *ns;	/* [한국어] Identify 를 실어 보낼 하위 경로 */
	int srcu_idx, ret = -EWOULDBLOCK;	/* [한국어] 경로 부재 시 기본 에러 */

	/* Avoid issuing commands too often by rate limiting the update */
	if (!__ratelimit(&head->rs_nuse))
		return 0;	/* [한국어] 최근에 갱신함 — 캐시된 head->nuse 재사용 신호(성공) */

	srcu_idx = srcu_read_lock(&head->srcu);	/* [한국어] 경로 리스트 안정적 순회 */
	ns = nvme_find_path(head);	/* [한국어] Identify 를 실을 live 경로 선택 */
	if (!ns)
		goto out_unlock;	/* [한국어] 모든 경로 불가 — EWOULDBLOCK */

	ret = nvme_identify_ns(ns->ctrl, head->ns_id, &id);	/* [한국어] Admin Identify CNS=NS 로 최신 디스크립터 */
	if (ret)
		goto out_unlock;	/* [한국어] Identify 실패 — nuse 캐시 유지 */

	head->nuse = le64_to_cpu(id->nuse);	/* [한국어] LE NUSE 필드를 호스트 엔디안 캐시에 저장 */
	kfree(id);	/* [한국어] identify 헬퍼가 할당한 버퍼 해제 */

out_unlock:
	srcu_read_unlock(&head->srcu, srcu_idx);	/* [한국어] 경로 리스트 SRCU 종료 */
	return ret;	/* [한국어] 0 또는 Identify/경로 errno */
}

/*
 * [한국어]
 * ns_update_nuse - 단일 경로 ns 디스크에서 NUSE 갱신
 *
 * head 를 공유하는 ratelimit 을 쓰고, 해당 ns->ctrl 로 Identify.
 * multipath 가 아닌 gendisk 의 nuse_show 분기에서 호출.
 */
static int ns_update_nuse(struct nvme_ns *ns)	/* [한국어] 경로 디스크 private_data 경로 */
{
	struct nvme_id_ns *id;	/* [한국어] Identify 결과 버퍼 */
	int ret;	/* [한국어] Admin 명령 결과 */

	/* Avoid issuing commands too often by rate limiting the update. */
	if (!__ratelimit(&ns->head->rs_nuse))
		return 0;	/* [한국어] 동일 head 기준 전역 rate limit */

	ret = nvme_identify_ns(ns->ctrl, ns->head->ns_id, &id);	/* [한국어] 이 경로 컨트롤러로 Identify */
	if (ret)
		return ret;	/* [한국어] Admin 실패 전파 */

	ns->head->nuse = le64_to_cpu(id->nuse);	/* [한국어] 공유 head 캐시 갱신 — 다른 경로 sysfs 에도 반영 */
	kfree(id);	/* [한국어] Identify 버퍼 해제 */
	return 0;	/* [한국어] 캐시 갱신 완료 */
}

/*
 * [한국어]
 * nuse_show - Namespace Utilization 출력(필요 시 선행 갱신)
 *
 * head 디스크면 ns_head_update_nuse, 경로 디스크면 ns_update_nuse.
 * 갱신 실패 시 그 errno 를 sysfs 읽기 에러로 반환.
 */
static ssize_t nuse_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct nvme_ns_head *head = dev_to_ns_head(dev);	/* [한국어] 캐시 nuse 소유 head */
	struct gendisk *disk = dev_to_disk(dev);	/* [한국어] head vs path 분기용 */
	int ret;	/* [한국어] 선행 갱신 결과 */

	if (nvme_disk_is_ns_head(disk))
		ret = ns_head_update_nuse(head);	/* [한국어] multipath 가상 노드 */
	else
		ret = ns_update_nuse(disk->private_data);	/* [한국어] private_data = nvme_ns */
	if (ret)
		return ret;	/* [한국어] Identify/경로 실패를 sysfs 읽기 에러로 */

	return sysfs_emit(buf, "%llu\n", head->nuse);	/* [한국어] LBA 단위 사용량(스펙 NUSE) */
}
static DEVICE_ATTR_RO(nuse);	/* [한국어] 읽기 전용 nuse */

/* [한국어] NS 블록 장치에 붙는 기본 속성 포인터 테이블. multipath 전용 속성은
 * multipath.c 에 정의된 device_attr_* 를 여기서 참조(링크 시 해석). */
static struct attribute *nvme_ns_attrs[] = {
	&dev_attr_wwid.attr,	/* [한국어] 안정적 볼륨 WWID */
	&dev_attr_uuid.attr,	/* [한국어] Namespace UUID (레거시 NGUID 폴백) */
	&dev_attr_nguid.attr,	/* [한국어] NGUID 16B */
	&dev_attr_eui.attr,	/* [한국어] EUI-64 */
	&dev_attr_csi.attr,	/* [한국어] Command Set Identifier */
	&dev_attr_nsid.attr,	/* [한국어] Namespace ID */
	&dev_attr_metadata_bytes.attr,	/* [한국어] LBA 메타데이터 바이트 */
	&dev_attr_nuse.attr,	/* [한국어] Namespace Utilization */
#ifdef CONFIG_NVME_MULTIPATH
	&dev_attr_ana_grpid.attr,	/* [한국어] 경로 전용: ANA 그룹 ID */
	&dev_attr_ana_state.attr,	/* [한국어] 경로 전용: optimized/non-optimized/… */
	&dev_attr_queue_depth.attr,	/* [한국어] 경로 큐 깊이 */
	&dev_attr_numa_nodes.attr,	/* [한국어] 경로 NUMA 친화도 */
	&dev_attr_delayed_removal_secs.attr,	/* [한국어] head 전용: 경로 손실 후 지연 제거 */
#endif
	&dev_attr_io_passthru_err_log_enabled.attr,	/* [한국어] I/O 패스스루 에러 로그 토글 */
	NULL,	/* [한국어] 속성 배열 센티널 */
};

/*
 * [한국어]
 * nvme_ns_attrs_are_visible - NS 속성 가시성 필터
 *
 * UUID/NGUID/EUI 가 전부 0 이면 해당 파일 숨김(무의미한 0 덤프 방지).
 * multipath: ANA 속성은 head 디스크·ANA 미사용 컨트롤러에서 숨김.
 * queue_depth/numa 는 head 에서 숨김(경로별 의미).
 * delayed_removal 은 head 에서만 표시.
 * @return: 0 이면 숨김, 아니면 a->mode.
 */
static umode_t nvme_ns_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);	/* [한국어] 블록 device kobj */
	struct nvme_ns_ids *ids = &dev_to_ns_head(dev)->ids;	/* [한국어] 가시성 판정용 식별자 */

	if (a == &dev_attr_uuid.attr) {
		if (uuid_is_null(&ids->uuid) &&
		    !memchr_inv(ids->nguid, 0, sizeof(ids->nguid)))
			return 0;	/* [한국어] uuid 도 nguid 도 없으면 uuid 파일 숨김 */
	}
	if (a == &dev_attr_nguid.attr) {
		if (!memchr_inv(ids->nguid, 0, sizeof(ids->nguid)))
			return 0;	/* [한국어] NGUID 전 0 → 속성 비표시 */
	}
	if (a == &dev_attr_eui.attr) {
		if (!memchr_inv(ids->eui64, 0, sizeof(ids->eui64)))
			return 0;	/* [한국어] EUI 전 0 → 숨김 */
	}
#ifdef CONFIG_NVME_MULTIPATH
	if (a == &dev_attr_ana_grpid.attr || a == &dev_attr_ana_state.attr) {
		/* per-path attr */
		if (nvme_disk_is_ns_head(dev_to_disk(dev)))
			return 0;	/* [한국어] 가상 head 에는 경로 ANA 상태 없음 */
		if (!nvme_ctrl_use_ana(nvme_get_ns_from_dev(dev)->ctrl))
			return 0;	/* [한국어] 컨트롤러가 ANA 미사용 */
	}
	if (a == &dev_attr_queue_depth.attr || a == &dev_attr_numa_nodes.attr) {
		if (nvme_disk_is_ns_head(dev_to_disk(dev)))
			return 0;	/* [한국어] 경로 전용 메트릭 */
	}
	if (a == &dev_attr_delayed_removal_secs.attr) {
		struct gendisk *disk = dev_to_disk(dev);	/* [한국어] head vs path 판정 */

		if (!nvme_disk_is_ns_head(disk))
			return 0;	/* [한국어] head 정책 속성 — 개별 경로 디스크에선 숨김 */
	}
#endif
	return a->mode;	/* [한국어] 기본 모드 그대로 노출 */
}

static const struct attribute_group nvme_ns_attr_group = {
	.attrs		= nvme_ns_attrs,	/* [한국어] NS 블록 장치 속성 테이블 */
	.is_visible	= nvme_ns_attrs_are_visible,	/* [한국어] 동적 가시성 */
};

#ifdef CONFIG_NVME_MULTIPATH
/*
 * NOTE: The dummy attribute does not appear in sysfs. It exists solely to allow
 * control over the visibility of the multipath sysfs node. Without at least one
 * attribute defined in nvme_ns_mpath_attrs[], the sysfs implementation does not
 * invoke the multipath_sysfs_group_visible() method. As a result, we would not
 * be able to control the visibility of the multipath sysfs node.
 */
/* [한국어] multipath 서브디렉터리 자체의 표시 여부를 group is_visible 로
 * 제어하려면 attrs 가 최소 1개 필요. dummy 는 attr 단위에서 항상 숨김. */
static struct attribute dummy_attr = {
	.name = "dummy",	/* [한국어] 그룹 가시성 훅용 더미 이름 */
};

static struct attribute *nvme_ns_mpath_attrs[] = {
	&dummy_attr,	/* [한국어] attrs 비어 있으면 group is_visible 미호출 방지 */
	NULL,	/* [한국어] 센티널 */
};

/*
 * [한국어] multipath_sysfs_group_visible - head 디스크에만 multipath/ 디렉터리 표시
 */
static bool multipath_sysfs_group_visible(struct kobject *kobj)
{
	struct device *dev = container_of(kobj, struct device, kobj);

	return nvme_disk_is_ns_head(dev_to_disk(dev));	/* [한국어] 경로 디스크에는 multipath/ 불필요 */
}

/*
 * [한국어] multipath_sysfs_attr_visible - 그룹 안 개별 파일은 전부 숨김(더미 포함)
 */
static bool multipath_sysfs_attr_visible(struct kobject *kobj,
		struct attribute *attr, int n)
{
	return false;	/* [한국어] 디렉터리 존재만 의미 있을 때 쓰는 패턴 — 실제 파일은 상위/다른 그룹 */
}

DEFINE_SYSFS_GROUP_VISIBLE(multipath_sysfs)
/* [한국어] group/attr visible 헬퍼를 is_visible 콜백 형태로 결합하는 매크로 */

const struct attribute_group nvme_ns_mpath_attr_group = {
	.name           = "multipath",	/* [한국어] /sys/block/.../multipath/ 서브그룹 이름 */
	.attrs		= nvme_ns_mpath_attrs,	/* [한국어] dummy 포함 — 그룹 가시성 훅 활성화 */
	.is_visible     = SYSFS_GROUP_VISIBLE(multipath_sysfs),	/* [한국어] head 디스크에만 디렉터리 표시 */
};
#endif

/* [한국어] gendisk 등록 시 전달되는 NS 속성 그룹 배열(센티널 NULL 종료) */
const struct attribute_group *nvme_ns_attr_groups[] = {
	&nvme_ns_attr_group,	/* [한국어] wwid/nsid/… 기본 NS 속성 */
#ifdef CONFIG_NVME_MULTIPATH
	&nvme_ns_mpath_attr_group,	/* [한국어] multipath/ 디렉터리 가시성 그룹 */
#endif
	NULL,	/* [한국어] 그룹 배열 센티널 */
};

/*
 * [한국어]
 * nvme_show_str_function(field) - 컨트롤러 subsys 문자열 필드 RO 속성 생성 매크로
 *
 * model/serial/firmware_rev 처럼 Identify Controller 문자 배열을
 * "%.*s" 로 길이 제한 출력하는 show + DEVICE_ATTR 를 한 번에 정의.
 * 널 종료 보장이 약한 고정 배열에 안전.
 */
#define nvme_show_str_function(field)						\
static ssize_t  field##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)		\
{										\
        struct nvme_ctrl *ctrl = dev_get_drvdata(dev);				\
        return sysfs_emit(buf, "%.*s\n",					\
		(int)sizeof(ctrl->subsys->field), ctrl->subsys->field);		\
}										\
static DEVICE_ATTR(field, S_IRUGO, field##_show, NULL);

nvme_show_str_function(model);	/* [한국어] /sys/class/nvme/nvmeX/model */
nvme_show_str_function(serial);	/* [한국어] serial number */
nvme_show_str_function(firmware_rev);	/* [한국어] 펌웨어 리비전 문자열 */

/*
 * [한국어]
 * nvme_show_int_function(field) - ctrl 정수 필드 RO 속성 매크로
 * cntlid, numa_node, queue_count, sqsize, kato 등.
 */
#define nvme_show_int_function(field)						\
static ssize_t  field##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)		\
{										\
        struct nvme_ctrl *ctrl = dev_get_drvdata(dev);				\
        return sysfs_emit(buf, "%d\n", ctrl->field);				\
}										\
static DEVICE_ATTR(field, S_IRUGO, field##_show, NULL);

nvme_show_int_function(cntlid);	/* [한국어] Controller ID (Identify) */
nvme_show_int_function(numa_node);	/* [한국어] 장치에 묶인 NUMA 노드 */
nvme_show_int_function(queue_count);	/* [한국어] 생성된 큐 개수 */
nvme_show_int_function(sqsize);	/* [한국어] SQ 크기(도어벨 항목 수 관련) */
nvme_show_int_function(kato);	/* [한국어] Keep Alive Timeout (초 단위 설정값 필드) */

/*
 * [한국어]
 * nvme_sysfs_delete - "delete_controller" store: 컨트롤러 인스턴스 삭제
 *
 * STARTED_ONCE 가 아니면 아직 초기화 중 → -EBUSY.
 * device_remove_file_self 로 자기 속성 제거와 경합을 직렬화한 뒤에만
 * nvme_delete_ctrl_sync 호출(파일을 지우는 중 콜백 재진입 방지 패턴).
 * fabrics 연결 끊기, PCIe detach 유사 정리에 사용.
 *
 * 호출 체인:
 *   echo > delete_controller → [nvme_sysfs_delete] → nvme_delete_ctrl_sync
 */
static ssize_t nvme_sysfs_delete(struct device *dev,	/* [한국어] 삭제 대상 컨트롤러 device */
				struct device_attribute *attr, const char *buf,	/* [한국어] 트리거 store */
				size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 수명 종료할 인스턴스 */

	if (!test_bit(NVME_CTRL_STARTED_ONCE, &ctrl->flags))
		return -EBUSY;	/* [한국어] 한 번도 LIVE 에 못 오른 초기화 중 삭제 거부 */

	if (device_remove_file_self(dev, attr))
		nvme_delete_ctrl_sync(ctrl);	/* [한국어] 이 태스크가 파일을 제거한 주체일 때만 동기 삭제 수행 */
	return count;	/* [한국어] 경쟁에서 진 쪽도 count 반환 — 이미 다른 쪽이 삭제 진행 */
}
static DEVICE_ATTR(delete_controller, S_IWUSR, NULL, nvme_sysfs_delete);	/* [한국어] WO delete_controller */

/*
 * [한국어] transport show — 전송 계층 이름("pcie", "tcp", "rdma", "fc", …)
 * ctrl->ops->name 은 각 전송 모듈이 등록한 nvme_ctrl_ops 에 고정 문자열.
 */
static ssize_t nvme_sysfs_show_transport(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] ops 테이블 소유 컨트롤러 */

	return sysfs_emit(buf, "%s\n", ctrl->ops->name);	/* [한국어] 트랜스포트 모듈이 등록한 고정 이름 */
}
static DEVICE_ATTR(transport, S_IRUGO, nvme_sysfs_show_transport, NULL);	/* [한국어] RO transport */

/*
 * [한국어]
 * nvme_sysfs_show_state - 컨트롤러 상태머신 문자열
 *
 * nvme_ctrl_state() 원자 스냅샷을 new/live/resetting/…/dead 으로 매핑.
 * 모니터링·자동화의 핵심 관측 포인트.
 */
static ssize_t nvme_sysfs_show_state(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);
	unsigned state = (unsigned)nvme_ctrl_state(ctrl);	/* [한국어] 동시 갱신에 안전한 상태 읽기 헬퍼 */
	static const char *const state_name[] = {
		[NVME_CTRL_NEW]		= "new",	/* [한국어] 할당 직후 초기화 전 */
		[NVME_CTRL_LIVE]	= "live",	/* [한국어] I/O 가능 정상 */
		[NVME_CTRL_RESETTING]	= "resetting",	/* [한국어] 리셋 진행 */
		[NVME_CTRL_CONNECTING]	= "connecting",	/* [한국어] fabrics 재연결 등 */
		[NVME_CTRL_DELETING]	= "deleting",	/* [한국어] 삭제 중(아직 I/O 정리 가능) */
		[NVME_CTRL_DELETING_NOIO]= "deleting (no IO)",	/* [한국어] I/O 중단 후 삭제 */
		[NVME_CTRL_DEAD]	= "dead",	/* [한국어] 복구 불가 최종 */
	};

	if (state < ARRAY_SIZE(state_name) && state_name[state])
		return sysfs_emit(buf, "%s\n", state_name[state]);	/* [한국어] 알려진 상태 문자열 */

	return sysfs_emit(buf, "unknown state\n");	/* [한국어] 미래 enum 값 방어 */
}

static DEVICE_ATTR(state, S_IRUGO, nvme_sysfs_show_state, NULL);	/* [한국어] RO state */

/*
 * [한국어] subsysnqn — 서브시스템 NVMe Qualified Name (연결 대상 식별)
 */
static ssize_t nvme_sysfs_show_subsysnqn(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 소속 서브시스템 조회 앵커 */

	return sysfs_emit(buf, "%s\n", ctrl->subsys->subnqn);	/* [한국어] Connect/디스커버리 대상 NQN */
}
static DEVICE_ATTR(subsysnqn, S_IRUGO, nvme_sysfs_show_subsysnqn, NULL);	/* [한국어] RO subsysnqn */

/*
 * [한국어] hostnqn — 호스트 NQN (fabrics opts 에만 존재, PCIe 는 is_visible 로 숨김)
 */
static ssize_t nvme_sysfs_show_hostnqn(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] opts 있는 fabrics 컨트롤러만 노출 */

	return sysfs_emit(buf, "%s\n", ctrl->opts->host->nqn);	/* [한국어] Connect 에 실린 Host NQN */
}
static DEVICE_ATTR(hostnqn, S_IRUGO, nvme_sysfs_show_hostnqn, NULL);	/* [한국어] RO hostnqn */

/*
 * [한국어] hostid — 호스트 UUID (Connect 시 호스트 식별)
 */
static ssize_t nvme_sysfs_show_hostid(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] opts->host 앵커 */

	return sysfs_emit(buf, "%pU\n", &ctrl->opts->host->id);	/* [한국어] Host ID UUID 텍스트 */
}
static DEVICE_ATTR(hostid, S_IRUGO, nvme_sysfs_show_hostid, NULL);	/* [한국어] RO hostid */

/*
 * [한국어] address — 전송 주소 문자열. ops->get_address 가 PCIe BDF, TCP ip:port 등 포맷
 */
static ssize_t nvme_sysfs_show_address(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 전송 ops 디스패치 대상 */

	return ctrl->ops->get_address(ctrl, buf, PAGE_SIZE);	/* [한국어] 전송별 구현 — 반환값은 쓴 바이트 수 */
}
static DEVICE_ATTR(address, S_IRUGO, nvme_sysfs_show_address, NULL);	/* [한국어] RO address */

/*
 * [한국어]
 * ctrl_loss_tmo show/store - 컨트롤러 손실 타임아웃(초)
 *
 * max_reconnects * reconnect_delay 로 표시. max_reconnects==-1 이면 "off"
 * (무한 재시도). store 시 음수면 무한, 아니면 delay 로 나눠 재시도 횟수 설정.
 * fabrics 연결이 끊긴 뒤 포기하기 전까지의 운영 정책 노브.
 */
static ssize_t nvme_ctrl_loss_tmo_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] fabrics 재연결 정책 조회 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] max_reconnects·delay */

	if (ctrl->opts->max_reconnects == -1)
		return sysfs_emit(buf, "off\n");	/* [한국어] 무제한 재연결 */
	return sysfs_emit(buf, "%d\n",
			  opts->max_reconnects * opts->reconnect_delay);	/* [한국어] 총 대기 상한(초) 근사 */
}

static ssize_t nvme_ctrl_loss_tmo_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] fabrics opts 소유 컨트롤러 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] 재연결 정책 저장소 */
	int ctrl_loss_tmo, err;	/* [한국어] 초 단위 입력 / 파싱 결과 */

	err = kstrtoint(buf, 10, &ctrl_loss_tmo);	/* [한국어] 10진 정수 파싱 */
	if (err)
		return -EINVAL;	/* [한국어] 형식 오류 */

	if (ctrl_loss_tmo < 0)
		opts->max_reconnects = -1;	/* [한국어] 음수 입력 = 무한 재시도 정책 */
	else
		opts->max_reconnects = DIV_ROUND_UP(ctrl_loss_tmo,
						opts->reconnect_delay);	/* [한국어] 초 → 재시도 횟수로 환산(올림) */
	return count;	/* [한국어] 다음 재연결 루프부터 반영 */
}
static DEVICE_ATTR(ctrl_loss_tmo, S_IRUGO | S_IWUSR,
	nvme_ctrl_loss_tmo_show, nvme_ctrl_loss_tmo_store);	/* [한국어] RW 손실 타임아웃 */

/*
 * [한국어] reconnect_delay — 재연결 시도 간격(초). -1 이면 show "off"
 */
static ssize_t nvme_ctrl_reconnect_delay_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] opts 조회 */

	if (ctrl->opts->reconnect_delay == -1)
		return sysfs_emit(buf, "off\n");	/* [한국어] 재연결 간격 비활성 표시 */
	return sysfs_emit(buf, "%d\n", ctrl->opts->reconnect_delay);	/* [한국어] 초 단위 간격 */
}

static ssize_t nvme_ctrl_reconnect_delay_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 정책 갱신 대상 */
	unsigned int v;	/* [한국어] 새 간격(초) */
	int err;	/* [한국어] 파싱 결과 */

	err = kstrtou32(buf, 10, &v);	/* [한국어] 부호 없는 10진 파싱 */
	if (err)
		return err;	/* [한국어] kstrtou32 의 errno 그대로 */

	ctrl->opts->reconnect_delay = v;	/* [한국어] 다음 재연결 스케줄부터 반영 */
	return count;	/* [한국어] store 성공 */
}
static DEVICE_ATTR(reconnect_delay, S_IRUGO | S_IWUSR,
	nvme_ctrl_reconnect_delay_show, nvme_ctrl_reconnect_delay_store);	/* [한국어] RW 재연결 간격 */

/*
 * [한국어]
 * fast_io_fail_tmo — I/O 를 빨리 실패시키기 전 대기(초). multipath 가 다른
 * 경로로 넘기기 전 fabrics 경로의 “빠른 실패” 정책. -1 = off.
 */
static ssize_t nvme_ctrl_fast_io_fail_tmo_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] multipath 빠른 실패 정책 조회 */

	if (ctrl->opts->fast_io_fail_tmo == -1)
		return sysfs_emit(buf, "off\n");	/* [한국어] 빠른 실패 비활성 */
	return sysfs_emit(buf, "%d\n", ctrl->opts->fast_io_fail_tmo);	/* [한국어] 초 단위 임계 */
}

static ssize_t nvme_ctrl_fast_io_fail_tmo_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 정책 소유 컨트롤러 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] fabrics 옵션 블록 */
	int fast_io_fail_tmo, err;	/* [한국어] 입력 초 / 파싱 errno */

	err = kstrtoint(buf, 10, &fast_io_fail_tmo);	/* [한국어] 부호 있는 정수 — 음수=off */
	if (err)
		return -EINVAL;	/* [한국어] 파싱 실패 */

	if (fast_io_fail_tmo < 0)
		opts->fast_io_fail_tmo = -1;	/* [한국어] 빠른 I/O 실패 비활성 */
	else
		opts->fast_io_fail_tmo = fast_io_fail_tmo;	/* [한국어] 경로 오류 시 조기 fail 대기 */
	return count;	/* [한국어] store 성공 */
}
static DEVICE_ATTR(fast_io_fail_tmo, S_IRUGO | S_IWUSR,
	nvme_ctrl_fast_io_fail_tmo_show, nvme_ctrl_fast_io_fail_tmo_store);	/* [한국어] RW fast_io_fail_tmo */

/*
 * [한국어]
 * cntrltype_show - 컨트롤러 유형 io / discovery / admin
 * Identify CNTRLTYPE. discovery 는 fabrics 디스커버리 컨트롤러.
 */
static ssize_t cntrltype_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	static const char * const type[] = {
		[NVME_CTRL_IO] = "io\n",	/* [한국어] 일반 I/O 컨트롤러 */
		[NVME_CTRL_DISC] = "discovery\n",	/* [한국어] 디스커버리 전용 */
		[NVME_CTRL_ADMIN] = "admin\n",	/* [한국어] 관리 전용 */
	};
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] cntrltype 필드 소스 */

	if (ctrl->cntrltype > NVME_CTRL_ADMIN || !type[ctrl->cntrltype])
		return sysfs_emit(buf, "reserved\n");	/* [한국어] 스펙 예약 값 방어 */

	return sysfs_emit(buf, type[ctrl->cntrltype]);	/* [한국어] Identify CNTRLTYPE 문자열 */
}
static DEVICE_ATTR_RO(cntrltype);	/* [한국어] RO cntrltype */

/*
 * [한국어]
 * dctype_show - Discovery Controller Type: none / ddc / cdc
 * 중앙/직접 디스커버리 토폴로지 구분용.
 */
static ssize_t dctype_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	static const char * const type[] = {
		[NVME_DCTYPE_NOT_REPORTED] = "none\n",	/* [한국어] 미보고 */
		[NVME_DCTYPE_DDC] = "ddc\n",	/* [한국어] Direct Discovery Controller */
		[NVME_DCTYPE_CDC] = "cdc\n",	/* [한국어] Central Discovery Controller */
	};
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] dctype 필드 소스 */

	if (ctrl->dctype > NVME_DCTYPE_CDC || !type[ctrl->dctype])
		return sysfs_emit(buf, "reserved\n");	/* [한국어] 예약 값 방어 */

	return sysfs_emit(buf, type[ctrl->dctype]);	/* [한국어] 디스커버리 토폴로지 타입 */
}
static DEVICE_ATTR_RO(dctype);	/* [한국어] RO dctype */

/*
 * [한국어]
 * quirks_show - 이 컨트롤러에 적용 중인 드라이버 quirk 비트 목록
 *
 * 비트마다 nvme_quirk_name() 으로 이름 한 줄씩. 없으면 "none".
 * 벤더 특이동작 우회가 켜졌는지 현장 디버깅에 유용.
 */
static ssize_t quirks_show(struct device *dev, struct device_attribute *attr,
                char *buf)
{
	int count = 0, i;	/* [한국어] 출력 오프셋 / 비트 인덱스 */
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] quirks 비트마스크 소유 */
	unsigned long quirks = ctrl->quirks;	/* [한국어] 로컬 복사 후 시프트로 비트 순회 */

	if (!quirks)
		return sysfs_emit(buf, "none\n");	/* [한국어] quirk 없음 */

	for (i = 0; quirks; ++i) {
		if (quirks & 1) {
			count += sysfs_emit_at(buf, count, "%s\n",
					nvme_quirk_name(BIT(i)));	/* [한국어] 켜진 비트 i 의 심볼릭 이름 append */
		}
		quirks >>= 1;	/* [한국어] 다음 비트 검사 */
	}

	return count;	/* [한국어] 총 기록 바이트 */
}
static DEVICE_ATTR_RO(quirks);	/* [한국어] RO quirks */

#ifdef CONFIG_NVME_HOST_AUTH
/*
 * [한국어]
 * dhchap_secret show/store - 호스트 DH-HMAC-CHAP 시크릿
 *
 * show: 설정 문자열 또는 "none".
 * store: "DHHC-1:" 접두 필수, 기존 secret 이 있을 때만 교체 가능.
 * 변경 시 파싱→host_key 교체(mutex)→auth 워크로 재인증.
 * 동일 문자열이면 키 재파싱 없이 재인증만.
 *
 * 호출 체인:
 *   sysfs store → [dhchap_secret_store] → nvme_auth_parse_key → auth_work
 */
static ssize_t nvme_ctrl_dhchap_secret_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] fabrics opts 소유 컨트롤러 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] 시크릿 문자열 저장소 */

	if (!opts->dhchap_secret)
		return sysfs_emit(buf, "none\n");	/* [한국어] 미설정 표시 */
	return sysfs_emit(buf, "%s\n", opts->dhchap_secret);	/* [한국어] 설정된 DHHC-1 표현 그대로 */
}

static ssize_t nvme_ctrl_dhchap_secret_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 재인증 대상 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] 문자열·키 쌍 소유 */
	char *dhchap_secret;	/* [한국어] 새로 할당할 시크릿 문자열 버퍼 */

	if (!ctrl->opts->dhchap_secret)
		return -EINVAL;	/* [한국어] 최초에 secret 없이 만든 연결은 sysfs 로 신규 도입 불가 */
	if (count < 7)
		return -EINVAL;	/* [한국어] "DHHC-1:" 최소 길이 미달 */
	if (memcmp(buf, "DHHC-1:", 7))
		return -EINVAL;	/* [한국어] 스펙 키 표현 접두 강제 */

	dhchap_secret = kzalloc(count + 1, GFP_KERNEL);	/* [한국어] +1 로 NUL 종료 여유 */
	if (!dhchap_secret)
		return -ENOMEM;	/* [한국어] 시크릿 버퍼 할당 실패 */
	memcpy(dhchap_secret, buf, count);	/* [한국어] sysfs 버퍼는 개행 포함 가능 — 파서가 처리 */
	nvme_auth_stop(ctrl);	/* [한국어] 진행 중 인증 시퀀스 중단 후 교체 */
	if (strcmp(dhchap_secret, opts->dhchap_secret)) {
		struct nvme_dhchap_key *key, *host_key;	/* [한국어] 신·구 이진 키 */
		int ret;	/* [한국어] 파싱 결과 */

		ret = nvme_auth_parse_key(dhchap_secret, &key);	/* [한국어] 문자열 → 이진 키 자료구조 */
		if (ret) {
			kfree(dhchap_secret);	/* [한국어] 파싱 실패 시 임시 문자열 폐기 */
			return ret;	/* [한국어] 키 형식 오류 전파 */
		}
		kfree(opts->dhchap_secret);	/* [한국어] 이전 문자열 해제 */
		opts->dhchap_secret = dhchap_secret;	/* [한국어] 새 표현 소유권 이전 */
		host_key = ctrl->host_key;	/* [한국어] 옛 키 포인터 스냅샷 */
		mutex_lock(&ctrl->dhchap_auth_mutex);	/* [한국어] 인증 경로와 host_key 공유 직렬화 */
		ctrl->host_key = key;	/* [한국어] 핫 경로가 읽을 호스트 키 교체 */
		mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] 키 포인터 게시 완료 */
		nvme_auth_free_key(host_key);	/* [한국어] 락 밖에서 옛 키 메모리 해제 */
	} else
		kfree(dhchap_secret);	/* [한국어] 내용 동일 — 임시 버퍼만 버리고 재인증 */
	/* Start re-authentication */
	dev_info(ctrl->device, "re-authenticating controller\n");	/* [한국어] 운영자 가시 재인증 시작 */
	queue_work(nvme_wq, &ctrl->dhchap_auth_work);	/* [한국어] 비동기 CHAP 재협상 */

	return count;	/* [한국어] store 성공 — 워크가 완료를 담당 */
}

static DEVICE_ATTR(dhchap_secret, S_IRUGO | S_IWUSR,
	nvme_ctrl_dhchap_secret_show, nvme_ctrl_dhchap_secret_store);	/* [한국어] RW 호스트 CHAP 시크릿 */

/*
 * [한국어]
 * dhchap_ctrl_secret — 컨트롤러 측 시크릿(양방향 CHAP). 호스트 secret 과
 * 대칭 구조이나 ctrl_key / dhchap_ctrl_secret 필드를 갱신.
 */
static ssize_t nvme_ctrl_dhchap_ctrl_secret_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 양방향 CHAP 설정 조회 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] ctrl secret 문자열 */

	if (!opts->dhchap_ctrl_secret)
		return sysfs_emit(buf, "none\n");	/* [한국어] 단방향 전용 연결 */
	return sysfs_emit(buf, "%s\n", opts->dhchap_ctrl_secret);	/* [한국어] 컨트롤러 시크릿 표현 */
}

static ssize_t nvme_ctrl_dhchap_ctrl_secret_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] ctrl_key 교체 대상 */
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] 문자열 저장소 */
	char *dhchap_secret;	/* [한국어] 신규 시크릿 버퍼 */

	if (!ctrl->opts->dhchap_ctrl_secret)
		return -EINVAL;	/* [한국어] 연결 생성 시 ctrl secret 이 있었던 경우만 교체 허용 */
	if (count < 7)
		return -EINVAL;	/* [한국어] 접두 최소 길이 */
	if (memcmp(buf, "DHHC-1:", 7))
		return -EINVAL;	/* [한국어] DHHC-1 접두 강제 */

	dhchap_secret = kzalloc(count + 1, GFP_KERNEL);	/* [한국어] NUL 여유 할당 */
	if (!dhchap_secret)
		return -ENOMEM;	/* [한국어] 할당 실패 */
	memcpy(dhchap_secret, buf, count);	/* [한국어] 사용자 입력 복사 */
	nvme_auth_stop(ctrl);	/* [한국어] 진행 중 핸드셰이크 중단 */
	if (strcmp(dhchap_secret, opts->dhchap_ctrl_secret)) {
		struct nvme_dhchap_key *key, *ctrl_key;	/* [한국어] 신·구 컨트롤러 키 */
		int ret;	/* [한국어] 파싱 결과 */

		ret = nvme_auth_parse_key(dhchap_secret, &key);	/* [한국어] 문자열→키 */
		if (ret) {
			kfree(dhchap_secret);	/* [한국어] 실패 정리 */
			return ret;	/* [한국어] 형식 오류 */
		}
		kfree(opts->dhchap_ctrl_secret);	/* [한국어] 옛 문자열 해제 */
		opts->dhchap_ctrl_secret = dhchap_secret;	/* [한국어] 새 표현 게시 */
		ctrl_key = ctrl->ctrl_key;	/* [한국어] 옛 키 스냅샷 */
		mutex_lock(&ctrl->dhchap_auth_mutex);	/* [한국어] ctrl_key 직렬화 */
		ctrl->ctrl_key = key;	/* [한국어] 컨트롤러 검증용 키 교체 */
		mutex_unlock(&ctrl->dhchap_auth_mutex);	/* [한국어] 게시 완료 */
		nvme_auth_free_key(ctrl_key);	/* [한국어] 옛 키 해제 */
	} else
		kfree(dhchap_secret);	/* [한국어] 동일 내용 — 재인증만 */
	/* Start re-authentication */
	dev_info(ctrl->device, "re-authenticating controller\n");	/* [한국어] 재인증 로그 */
	queue_work(nvme_wq, &ctrl->dhchap_auth_work);	/* [한국어] CHAP 워크 스케줄 */

	return count;	/* [한국어] store 성공 */
}

static DEVICE_ATTR(dhchap_ctrl_secret, S_IRUGO | S_IWUSR,
	nvme_ctrl_dhchap_ctrl_secret_show, nvme_ctrl_dhchap_ctrl_secret_store);	/* [한국어] RW 컨트롤러 CHAP 시크릿 */
#endif

/* [한국어] 컨트롤러 클래스 장치(/sys/class/nvme/nvmeX) 기본 속성 목록.
 * is_visible 이 트랜스포트·opts 유무에 따라 fabrics 전용 항목을 가린다. */
static struct attribute *nvme_dev_attrs[] = {
	&dev_attr_reset_controller.attr,	/* [한국어] 동기 리셋 트리거 */
	&dev_attr_rescan_controller.attr,	/* [한국어] NS 재스캔 트리거 */
	&dev_attr_model.attr,	/* [한국어] Identify 모델명 */
	&dev_attr_serial.attr,	/* [한국어] 시리얼 */
	&dev_attr_firmware_rev.attr,	/* [한국어] 펌웨어 리비전 */
	&dev_attr_cntlid.attr,	/* [한국어] Controller ID */
	&dev_attr_delete_controller.attr,	/* [한국어] 인스턴스 삭제 (ops 있을 때) */
	&dev_attr_transport.attr,	/* [한국어] pcie/tcp/rdma/fc 이름 */
	&dev_attr_subsysnqn.attr,	/* [한국어] 서브시스템 NQN */
	&dev_attr_address.attr,	/* [한국어] 전송 주소 문자열 */
	&dev_attr_state.attr,	/* [한국어] 상태머신 live/resetting/… */
	&dev_attr_numa_node.attr,	/* [한국어] 선호 NUMA 노드 */
	&dev_attr_queue_count.attr,	/* [한국어] 생성 큐 수 */
	&dev_attr_sqsize.attr,	/* [한국어] SQ 깊이 */
	&dev_attr_hostnqn.attr,	/* [한국어] Host NQN (fabrics) */
	&dev_attr_hostid.attr,	/* [한국어] Host UUID (fabrics) */
	&dev_attr_ctrl_loss_tmo.attr,	/* [한국어] 손실 타임아웃(초) */
	&dev_attr_reconnect_delay.attr,	/* [한국어] 재연결 간격 */
	&dev_attr_fast_io_fail_tmo.attr,	/* [한국어] 빠른 I/O 실패 타임아웃 */
	&dev_attr_kato.attr,	/* [한국어] Keep-Alive 타임아웃 */
	&dev_attr_cntrltype.attr,	/* [한국어] io/discovery/admin */
	&dev_attr_dctype.attr,	/* [한국어] discovery controller type */
	&dev_attr_quirks.attr,	/* [한국어] 활성 quirk 이름 목록 */
#ifdef CONFIG_NVME_HOST_AUTH
	&dev_attr_dhchap_secret.attr,	/* [한국어] 호스트 CHAP 시크릿 */
	&dev_attr_dhchap_ctrl_secret.attr,	/* [한국어] 컨트롤러 CHAP 시크릿 */
#endif
	&dev_attr_adm_passthru_err_log_enabled.attr,	/* [한국어] Admin 패스스루 에러 로그 */
	NULL	/* [한국어] 속성 배열 센티널 */
};

/*
 * [한국어]
 * nvme_dev_attrs_are_visible - 컨트롤러 속성 가시성
 *
 * delete_ctrl ops 없는 전송은 delete_controller 숨김.
 * get_address 없으면 address 숨김.
 * opts 없는 PCIe 등은 fabrics 전용(hostnqn, loss_tmo, dhchap 등) 숨김.
 */
static umode_t nvme_dev_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);	/* [한국어] kobj→device */
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 가시성 판정 컨텍스트 */

	if (a == &dev_attr_delete_controller.attr && !ctrl->ops->delete_ctrl)
		return 0;	/* [한국어] 예: 일부 전송은 사용자 delete 미지원 */
	if (a == &dev_attr_address.attr && !ctrl->ops->get_address)
		return 0;	/* [한국어] get_address 미구현 전송 */
	if (a == &dev_attr_hostnqn.attr && !ctrl->opts)
		return 0;	/* [한국어] PCIe 로컬 컨트롤러 — fabrics opts 없음 */
	if (a == &dev_attr_hostid.attr && !ctrl->opts)
		return 0;	/* [한국어] fabrics Host ID 전용 */
	if (a == &dev_attr_ctrl_loss_tmo.attr && !ctrl->opts)
		return 0;	/* [한국어] 재연결 정책은 fabrics 만 */
	if (a == &dev_attr_reconnect_delay.attr && !ctrl->opts)
		return 0;	/* [한국어] 재연결 간격 fabrics 전용 */
	if (a == &dev_attr_fast_io_fail_tmo.attr && !ctrl->opts)
		return 0;	/* [한국어] 빠른 I/O 실패 fabrics 전용 */
#ifdef CONFIG_NVME_HOST_AUTH
	if (a == &dev_attr_dhchap_secret.attr && !ctrl->opts)
		return 0;	/* [한국어] CHAP 시크릿은 fabrics opts 필요 */
	if (a == &dev_attr_dhchap_ctrl_secret.attr && !ctrl->opts)
		return 0;	/* [한국어] 양방향 CHAP 도 opts 필요 */
#endif

	return a->mode;	/* [한국어] 기본 파일 모드로 노출 */
}

const struct attribute_group nvme_dev_attrs_group = {
	.attrs		= nvme_dev_attrs,	/* [한국어] 컨트롤러 속성 포인터 테이블 */
	.is_visible	= nvme_dev_attrs_are_visible,	/* [한국어] 전송·opts 기반 동적 가시성 */
};
EXPORT_SYMBOL_GPL(nvme_dev_attrs_group);
/* [한국어] 전송 모듈(pci/tcp/…)이 컨트롤러 device 등록 시 재사용 export */

#ifdef CONFIG_NVME_TCP_TLS
/*
 * [한국어]
 * TLS 관련 속성: 협상된 PSK id, configured key serial, keyring 이름.
 * TCP fabrics + tls/concat 옵션일 때만 의미 있음.
 */
static ssize_t tls_key_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] 협상된 PSK id 소유 */

	if (!ctrl->tls_pskid)
		return 0;	/* [한국어] 아직 협상 전 — 빈 읽기 */
	return sysfs_emit(buf, "%08x\n", ctrl->tls_pskid);	/* [한국어] 현재 세션 PSK 키 ID */
}
static DEVICE_ATTR_RO(tls_key);	/* [한국어] RO tls_key */

static ssize_t tls_configured_key_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] opts.tls_key 조회 */
	struct key *key = ctrl->opts->tls_key;	/* [한국어] 사용자 설정 TLS 키 객체 */

	return sysfs_emit(buf, "%08x\n", key_serial(key));	/* [한국어] 설정된 TLS 키의 keyring serial */
}

/*
 * [한국어]
 * tls_configured_key_store - 현재는 "0" 만 허용: 새 키 생성 후 재협상·리셋
 *
 * concat 모드에서 nvme_auth_negotiate/wait 후 컨트롤러 리셋으로 TLS 세션 재수립.
 * 실패 시에도 리셋 시도해 반쯤 협상된 상태를 정리.
 */
static ssize_t tls_configured_key_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] concat TLS 재협상 대상 */
	int error, qid;	/* [한국어] 결과 / 사용자 입력(0=새 키) */

	error = kstrtoint(buf, 10, &qid);	/* [한국어] 정수 파싱 — 현재 0만 유효 */
	if (error)
		return error;	/* [한국어] 파싱 실패 errno */

	/*
	 * We currently only allow userspace to write a `0` indicating
	 * generate a new key.
	 */
	if (qid)
		return -EINVAL;	/* [한국어] 미래 확장용 qid — 지금은 0(생성)만 */

	if (!ctrl->opts || !ctrl->opts->concat)
		return -EOPNOTSUPP;	/* [한국어] secure concat 미사용 연결 */

	error = nvme_auth_negotiate(ctrl, 0);	/* [한국어] qid 0 Admin 큐 경로로 키 협상 시작 */
	if (error < 0) {
		nvme_reset_ctrl(ctrl);	/* [한국어] 실패 정리: 비동기 리셋 스케줄 */
		return error;	/* [한국어] 협상 시작 실패 */
	}

	error = nvme_auth_wait(ctrl, 0);	/* [한국어] 협상 완료 대기 */
	if (error < 0) {
		nvme_reset_ctrl(ctrl);	/* [한국어] 반쯤 협상된 세션 정리 리셋 */
		return error;	/* [한국어] 협상 완료 실패 */
	}

	/*
	 * We need to reset the TLS connection, so let's just
	 * reset the controller.
	 */
	/* [한국어] 새 키를 전송 계층에 적용하려면 세션 재수립 = 컨트롤러 리셋 */
	nvme_reset_ctrl(ctrl);	/* [한국어] TLS 세션 재수립 트리거 */

	return count;	/* [한국어] store 성공 — 리셋은 비동기 */
}
static DEVICE_ATTR_RW(tls_configured_key);	/* [한국어] RW configured TLS key */

/*
 * [한국어] tls_keyring_show — 검색에 쓰는 keyring 의 description 문자열
 */
static ssize_t tls_keyring_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);	/* [한국어] opts.keyring 조회 */
	struct key *keyring = ctrl->opts->keyring;	/* [한국어] PSK 검색 키링 */

	return sysfs_emit(buf, "%s\n", keyring->description);	/* [한국어] 키링 description 문자열 */
}
static DEVICE_ATTR_RO(tls_keyring);	/* [한국어] RO tls_keyring */

static struct attribute *nvme_tls_attrs[] = {
	&dev_attr_tls_key.attr,	/* [한국어] 협상된 세션 PSK id */
	&dev_attr_tls_configured_key.attr,	/* [한국어] 설정 키 serial / 재생성 store */
	&dev_attr_tls_keyring.attr,	/* [한국어] 검색 keyring description */
	NULL,	/* [한국어] 센티널 */
};

/*
 * [한국어] TLS 그룹 가시성: transport 가 "tcp" 이고 관련 opts 가 있을 때만 각 파일 표시
 */
static umode_t nvme_tls_attrs_are_visible(struct kobject *kobj,
		struct attribute *a, int n)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct nvme_ctrl *ctrl = dev_get_drvdata(dev);

	if (!ctrl->opts || strcmp(ctrl->opts->transport, "tcp"))
		return 0;	/* [한국어] TCP 가 아니면 TLS sysfs 전체 무의미 */

	if (a == &dev_attr_tls_key.attr &&
	    !ctrl->opts->tls && !ctrl->opts->concat)
		return 0;	/* [한국어] TLS/concat 미사용 시 세션 키 없음 */
	if (a == &dev_attr_tls_configured_key.attr &&
	    !ctrl->opts->concat)
		return 0;	/* [한국어] configured key 교체는 concat 전용 */
	if (a == &dev_attr_tls_keyring.attr &&
	    !ctrl->opts->keyring)
		return 0;	/* [한국어] keyring= 미지정 연결 */

	return a->mode;	/* [한국어] 조건 충족 시 기본 모드 노출 */
}

static const struct attribute_group nvme_tls_attrs_group = {
	.attrs		= nvme_tls_attrs,	/* [한국어] TLS 속성 포인터 */
	.is_visible	= nvme_tls_attrs_are_visible,	/* [한국어] tcp+tls/concat 일 때만 노출 */
};
#endif

/* [한국어] 컨트롤러 device 에 등록하는 그룹 배열 — core/전송이 공통 사용 */
const struct attribute_group *nvme_dev_attr_groups[] = {
	&nvme_dev_attrs_group,	/* [한국어] 기본 관리·관측 속성 */
#ifdef CONFIG_NVME_TCP_TLS
	&nvme_tls_attrs_group,	/* [한국어] TCP TLS 전용 하위 그룹 */
#endif
	NULL,	/* [한국어] 그룹 배열 센티널 */
};

/*
 * [한국어] SUBSYS_ATTR_RO — 서브시스템 device 전용 device_attribute 심볼 이름
 * (subsys_attr_##_name)으로 정의해 nvme_dev 쪽 DEVICE_ATTR 와 이름 충돌 방지
 */
#define SUBSYS_ATTR_RO(_name, _mode, _show)			\
	struct device_attribute subsys_attr_##_name = \
		__ATTR(_name, _mode, _show, NULL)

/*
 * [한국어] nvme_subsys_show_nqn — /sys/class/nvme-subsystem/.../subsysnqn
 * container_of 로 subsystem.dev 에서 부모 구조체 복원.
 */
static ssize_t nvme_subsys_show_nqn(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);	/* [한국어] 임베디드 dev→subsystem */

	return sysfs_emit(buf, "%s\n", subsys->subnqn);	/* [한국어] 서브시스템 NQN */
}
static SUBSYS_ATTR_RO(subsysnqn, S_IRUGO, nvme_subsys_show_nqn);	/* [한국어] RO subsysnqn */

/*
 * [한국어]
 * nvme_subsys_show_type - 서브시스템 유형 discovery vs nvm
 * NQN subtype 에 따라 디스커버리 서브시스템과 일반 NVM 서브시스템 구분.
 */
static ssize_t nvme_subsys_show_type(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct nvme_subsystem *subsys =
		container_of(dev, struct nvme_subsystem, dev);	/* [한국어] subtype 필드 소스 */

	switch (subsys->subtype) {
	case NVME_NQN_DISC:
		return sysfs_emit(buf, "discovery\n");	/* [한국어] 디스커버리 전용 서브시스템 */
	case NVME_NQN_NVME:
		return sysfs_emit(buf, "nvm\n");	/* [한국어] 일반 스토리지 */
	default:
		return sysfs_emit(buf, "reserved\n");	/* [한국어] 예약 subtype 방어 */
	}
}
static SUBSYS_ATTR_RO(subsystype, S_IRUGO, nvme_subsys_show_type);	/* [한국어] RO subsystype */

/*
 * [한국어]
 * nvme_subsys_show_str_function — 서브시스템 model/serial/firmware_rev RO 매크로
 * 컨트롤러의 동일 이름 속성과 값이 같아야 하지만 kobj 가 다르므로 별도 show.
 */
#define nvme_subsys_show_str_function(field)				\
static ssize_t subsys_##field##_show(struct device *dev,		\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct nvme_subsystem *subsys =					\
		container_of(dev, struct nvme_subsystem, dev);		\
	return sysfs_emit(buf, "%.*s\n",				\
			   (int)sizeof(subsys->field), subsys->field);	\
}									\
static SUBSYS_ATTR_RO(field, S_IRUGO, subsys_##field##_show);

nvme_subsys_show_str_function(model);	/* [한국어] 서브시스템 레벨 모델명 */
nvme_subsys_show_str_function(serial);	/* [한국어] 서브시스템 시리얼 */
nvme_subsys_show_str_function(firmware_rev);	/* [한국어] 서브시스템 펌웨어 리비전 */

/* [한국어] 서브시스템 클래스 장치 속성. iopolicy 는 multipath.c 정의 */
static struct attribute *nvme_subsys_attrs[] = {
	&subsys_attr_model.attr,	/* [한국어] 서브시스템 모델 (Identify 공유) */
	&subsys_attr_serial.attr,	/* [한국어] 시리얼 */
	&subsys_attr_firmware_rev.attr,	/* [한국어] 펌웨어 리비전 */
	&subsys_attr_subsysnqn.attr,	/* [한국어] 서브시스템 NQN */
	&subsys_attr_subsystype.attr,	/* [한국어] discovery vs nvm */
#ifdef CONFIG_NVME_MULTIPATH
	&subsys_attr_iopolicy.attr,	/* [한국어] numa/round-robin 등 I/O 정책 선택 */
#endif
	NULL,	/* [한국어] 센티널 */
};

static const struct attribute_group nvme_subsys_attrs_group = {
	.attrs = nvme_subsys_attrs,	/* [한국어] 서브시스템 kobj 에 붙는 attr 테이블 */
};

/* [한국어] core 가 nvme_subsystem 등록 시 사용하는 그룹 배열 */
const struct attribute_group *nvme_subsys_attrs_groups[] = {
	&nvme_subsys_attrs_group,	/* [한국어] 서브시스템 model/serial/nqn/iopolicy 그룹 */
	NULL,	/* [한국어] attribute_group 배열 센티널 */
};


