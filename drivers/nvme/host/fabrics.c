// SPDX-License-Identifier: GPL-2.0
/*
 * NVMe over Fabrics common host code.
 * Copyright (c) 2015-2016 HGST, a Western Digital Company.
 */

/*
 * [한국어 설명] NVMe over Fabrics 공통 호스트 라이브러리 (fabrics.c)
 *
 * === 파일의 역할 ===
 * PCIe 가 아닌 네트워크 트랜스포트(tcp/rdma/fc) 위에서 NVMe 컨트롤러를
 * "연결(Connect)·속성(Property Get/Set)·재연결 정책·옵션 파싱·호스트 정체성
 * (Host NQN/ID)" 관점에서 공통 처리하는 미들웨어다. 실제 소켓/RDMA/FC 프레임
 * 송수신은 tcp.c/rdma.c/fc.c 가 담당하고, 이 파일은 트랜스포트 독립적인
 * 세션 성립 프로토콜과 /dev/nvme-fabrics 사용자 인터페이스를 제공한다.
 *
 * 핵심 흐름:
 *  1) 사용자 공간(nvme-cli) 이 /dev/nvme-fabrics 에 "transport=...,nqn=..."
 *     문자열 write → nvmf_parse_options → transport ops->create_ctrl
 *  2) 트랜스포트가 fabrics_q 를 만든 뒤 nvmf_connect_admin_queue 로 Admin
 *     Connect (cntlid=0xffff 동적 할당) → 선택적 DH-HMAC-CHAP(auth.c)
 *  3) I/O 큐마다 nvmf_connect_io_queue (할당된 cntlid 재사용)
 *  4) CC/CSTS 등 원격 "레지스터" 는 MMIO 대신 Property Get/Set 캡슐
 *
 * === 전체 아키텍처에서의 위치 ===
 *   nvme-cli → miscdev write → nvmf_create_ctrl → tcp/rdma/fc create_ctrl
 *     → connect admin/io → core enable/scan → LIVE
 * 재연결: 트랜스포트 error_recovery → nvmf_should_reconnect 로 재시도 여부
 * 결정(DNR·키 거부·max_reconnects). multipath 와 결합 시 경로 단위 재연결.
 *
 * === 타 모듈과의 연결 ===
 * - fabrics.h : nvmf_ctrl_options, nvmf_transport_ops, 공개 API 선언
 * - tcp.c/rdma.c/fc.c : nvmf_register_transport 로 ops 등록, create_ctrl 구현
 * - auth.c : Connect 결과 AUTHREQ 시 negotiate/wait
 * - core.c : fabrics_q/connect_q 제출, 리셋/재연결 상태기계
 * - nvme-keyring : TLS PSK/키링 조회
 *
 * === 주요 심볼 ===
 * Host: nvmf_host_add/default/put — NQN↔UUID 유일성
 * Capsule: nvmf_reg_read32/64, nvmf_reg_write32, connect_admin/io_queue
 * Policy: nvmf_should_reconnect, nvmf_set_io_queues, nvmf_map_queues
 * UI: /dev/nvme-fabrics misc + class nvme-fabrics
 *
 * === 주요 함수/구조체 요약 ===
 * - nvmf_create_ctrl: /dev/nvme-fabrics 에 쓰인 연결 문자열을 파싱해 해당 트랜스포트의
 *   create_ctrl 을 부르는 진입점. 사용자 공간의 'nvme connect' 가 여기로 들어온다.
 * - nvmf_parse_options / nvmf_free_options: 연결 옵션 문자열을 struct nvmf_ctrl_options
 *   로 옮기고 되돌린다. transport, traddr, nqn, keep-alive, 큐 개수 등이 여기서 정해진다.
 * - nvmf_connect_admin_queue / nvmf_connect_io_queue: Fabrics Connect 명령을 보내
 *   큐를 세운다. admin 쪽이 컨트롤러 ID 를 받아 오고 I/O 쪽은 그것을 재사용한다.
 * - nvmf_reg_read32 / nvmf_reg_read64 / nvmf_reg_write32: PCIe 의 MMIO 레지스터 접근을
 *   Property Get/Set 명령으로 대신한다. Fabrics 에는 BAR 가 없기 때문이다.
 * - nvmf_register_transport / nvmf_unregister_transport: rdma/tcp/fc 트랜스포트가
 *   자신을 등록하는 창구. nvmf_create_ctrl 이 이 목록에서 이름으로 찾는다.
 * - nvmf_should_reconnect: 재연결을 더 시도할지 판정. 남은 시도 횟수와 지연을 본다.
 * - struct nvmf_ctrl_options: 파싱된 연결 옵션 전체.
 * - struct nvmf_transport_ops: 트랜스포트가 제공해야 하는 vtable(name, create_ctrl 등).
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt	/* [한국어] 로그 접두를 모듈명으로 통일 */
#include <linux/init.h>	/* [한국어] module_init/exit 매크로 */
#include <linux/miscdevice.h>	/* [한국어] /dev/nvme-fabrics misc 장치 등록 */
#include <linux/module.h>	/* [한국어] 모듈 라이프사이클·try_module_get */
#include <linux/mutex.h>	/* [한국어] hosts 리스트·dev 직렬화 뮤텍스 */
#include <linux/parser.h>	/* [한국어] match_token 기반 connect 옵션 파싱 */
#include <linux/seq_file.h>	/* [한국어] misc read 시 instance/cntlid seq 출력 */
#include "nvme.h"	/* [한국어] nvme_ctrl, 제출 헬퍼, 상태 상수 */
#include "fabrics.h"	/* [한국어] nvmf_ctrl_options, transport_ops, 공개 API */
#include <linux/nvme-keyring.h>	/* [한국어] TLS 키 조회 nvme_tls_key_lookup */

static LIST_HEAD(nvmf_transports);	/* [한국어] 등록된 트랜스포트 ops 전역 리스트 (tcp/rdma/fc) */
static DECLARE_RWSEM(nvmf_transports_rwsem);	/* [한국어] 트랜스포트 등록/조회 직렬화 — 조회는 read, 등록은 write */

static LIST_HEAD(nvmf_hosts);	/* [한국어] Host NQN+ID 인스턴스 전역 목록 */
static DEFINE_MUTEX(nvmf_hosts_mutex);	/* [한국어] hosts 리스트·kref 경계 직렬화 */

static struct nvmf_host *nvmf_default_host;	/* [한국어] 부팅 시 생성되는 기본 Host — 옵션에 hostnqn 없을 때 사용 */

/*
 * [한국어]
 * nvmf_host_alloc - Host NQN 과 UUID 로 nvmf_host 객체 할당·초기화
 *
 * @hostnqn: 타깃이 인식하는 Host NQN 문자열
 * @id: Host Identifier (UUID)
 * @return: 새 host 또는 NULL (ENOMEM)
 *
 * 리스트 삽입은 호출자(add/default)가 hosts_mutex 하에서 수행. kref=1 시작.
 */
static struct nvmf_host *nvmf_host_alloc(const char *hostnqn, uuid_t *id)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvmf_host *host;	/* [한국어] 신규 host 인스턴스 */

	host = kmalloc_obj(*host);	/* [한국어] host 구조체 슬랩 할당 */
	if (!host)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return NULL;	/* [한국어] 메모리 부족 */

	kref_init(&host->ref);	/* [한국어] 참조 카운트 1 — put 시 destroy */
	uuid_copy(&host->id, id);	/* [한국어] Host ID 복사 */
	strscpy(host->nqn, hostnqn, NVMF_NQN_SIZE);	/* [한국어] NQN 안전 복사 (널 종단 보장) */

	return host;	/* [한국어] 호출 결과 반환 */
}

/*
 * [한국어]
 * nvmf_host_add - 전역 hosts 목록에서 (NQN,ID) 조회 또는 신규 등록
 *
 * 타깃 관점의 "host" 는 NQN 과 ID 쌍으로 유일하게 식별되어야 한다.
 * 같은 NQN+ID 면 kref_get 재사용. NQN 또는 ID 만 겹치면 -EINVAL (모호성 금지).
 * 락: nvmf_hosts_mutex.
 * 호출: nvmf_parse_options 끝에서 opts->host 설정.
 */
static struct nvmf_host *nvmf_host_add(const char *hostnqn, uuid_t *id)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvmf_host *host;	/* [한국어] 순회/결과 host */

	mutex_lock(&nvmf_hosts_mutex);	/* [한국어] hosts 목록 직렬화 */

	/*
	 * We have defined a host as how it is perceived by the target.
	 * Therefore, we don't allow different Host NQNs with the same Host ID.
	 * Similarly, we do not allow the usage of the same Host NQN with
	 * different Host IDs. This'll maintain unambiguous host identification.
	 */
	/* [한국어] 타깃이 보는 host 정체성: NQN↔ID 1:1 불변식 강제 */
	list_for_each_entry(host, &nvmf_hosts, list) {	/* [한국어] Fabrics 공통 라이브러리 */
		bool same_hostnqn = !strcmp(host->nqn, hostnqn);	/* [한국어] NQN 문자열 일치 */
		bool same_hostid = uuid_equal(&host->id, id);	/* [한국어] Host UUID 일치 */

		if (same_hostnqn && same_hostid) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			kref_get(&host->ref);	/* [한국어] 기존 host 재사용 — 참조 증가 */
			goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
		}
		if (same_hostnqn) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			pr_err("found same hostnqn %s but different hostid %pUb\n",	/* [한국어] 진단 로그 */
			       hostnqn, id);	/* [한국어] NQN 충돌·ID 불일치 — 모호한 host */
			host = ERR_PTR(-EINVAL);	/* [한국어] host 상수 — 상위 enum 역할 참고 */
			goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
		}
		if (same_hostid) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			pr_err("found same hostid %pUb but different hostnqn %s\n",	/* [한국어] 진단 로그 */
			       id, hostnqn);	/* [한국어] ID 충돌·NQN 불일치 */
			host = ERR_PTR(-EINVAL);	/* [한국어] host 상수 — 상위 enum 역할 참고 */
			goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
		}
	}

	host = nvmf_host_alloc(hostnqn, id);	/* [한국어] 신규 host 할당 */
	if (!host) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		host = ERR_PTR(-ENOMEM);	/* [한국어] 할당 실패를 ERR_PTR 로 통일 */
		goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
	}

	list_add_tail(&host->list, &nvmf_hosts);	/* [한국어] 전역 목록에 등록 */
out_unlock:
	mutex_unlock(&nvmf_hosts_mutex);	/* [한국어] Fabrics 공통 라이브러리 */
	return host;	/* [한국어] host 포인터 또는 ERR_PTR */
}

/*
 * [한국어]
 * nvmf_host_default - 모듈 로드 시 기본 Host 생성 (UUID 기반 NQN)
 *
 * 스펙 관례: nqn.2014-08.org.nvmexpress:uuid:<HostID>
 * 사용자 공간 hostnqn 미지정 connect 가 이 기본 host 를 사용.
 */
static struct nvmf_host *nvmf_host_default(void)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvmf_host *host;	/* [한국어] host — 함수/구조 문맥의 상태 */
	char nqn[NVMF_NQN_SIZE];	/* [한국어] 생성할 기본 Host NQN 버퍼 */
	uuid_t id;	/* [한국어] 랜덤 Host ID */

	uuid_gen(&id);	/* [한국어] 부팅마다 새 UUID — 영속 hostnqn 은 사용자가 지정 */
	snprintf(nqn, NVMF_NQN_SIZE,	/* [한국어] 포맷 작성 */
		"nqn.2014-08.org.nvmexpress:uuid:%pUb", &id);	/* [한국어] 스펙 권장 UUID 형식 NQN */

	host = nvmf_host_alloc(nqn, &id);	/* [한국어] host 상수 — 상위 enum 역할 참고 */
	if (!host)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return NULL;	/* [한국어] 모듈 init 실패 유발 */

	mutex_lock(&nvmf_hosts_mutex);	/* [한국어] Fabrics 공통 라이브러리 */
	list_add_tail(&host->list, &nvmf_hosts);	/* [한국어] 기본 host 도 전역 목록에 등록 */
	mutex_unlock(&nvmf_hosts_mutex);	/* [한국어] Fabrics 공통 라이브러리 */

	return host;	/* [한국어] 호출 결과 반환 */
}

/*
 * [한국어]
 * nvmf_host_destroy - kref 0 시 host 를 목록에서 제거하고 메모리 해제
 *
 * 호출: kref_put → 여기. hosts_mutex 로 list_del 직렬화.
 */
static void nvmf_host_destroy(struct kref *ref)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvmf_host *host = container_of(ref, struct nvmf_host, ref);	/* [한국어] kref → host */

	mutex_lock(&nvmf_hosts_mutex);	/* [한국어] Fabrics 공통 라이브러리 */
	list_del(&host->list);	/* [한국어] 전역 목록에서 제거 */
	mutex_unlock(&nvmf_hosts_mutex);	/* [한국어] Fabrics 공통 라이브러리 */

	kfree(host);	/* [한국어] host 구조체 해제 */
}

/*
 * [한국어]
 * nvmf_host_put - host 참조 감소 (NULL-safe)
 *
 * opts 해제·모듈 exit 에서 호출.
 */
static void nvmf_host_put(struct nvmf_host *host)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (host)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		kref_put(&host->ref, nvmf_host_destroy);	/* [한국어] 마지막 put 이면 destroy */
}

/**
 * nvmf_get_address() -  Get address/port
 * @ctrl:	Host NVMe controller instance which we got the address
 * @buf:	OUTPUT parameter that will contain the address/port
 * @size:	buffer size
 */
/*
 * [한국어]
 * nvmf_get_address - 컨트롤러 연결 주소 옵션을 sysfs/문자열로 직렬화
 *
 * traddr/trsvcid/host_traddr/host_iface 중 mask 에 있는 항목만 콤마 구분 출력.
 * 호출: core/sysfs address 속성 show.
 */
int nvmf_get_address(struct nvme_ctrl *ctrl, char *buf, int size)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	int len = 0;	/* [한국어] 누적 출력 바이트 */

	if (ctrl->opts->mask & NVMF_OPT_TRADDR)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		len += scnprintf(buf, size, "traddr=%s", ctrl->opts->traddr);	/* [한국어] 원격 전송 주소 */
	if (ctrl->opts->mask & NVMF_OPT_TRSVCID)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		len += scnprintf(buf + len, size - len, "%strsvcid=%s",	/* [한국어] 포맷 작성 */
				(len) ? "," : "", ctrl->opts->trsvcid);	/* [한국어] 원격 서비스/포트 ID */
	if (ctrl->opts->mask & NVMF_OPT_HOST_TRADDR)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		len += scnprintf(buf + len, size - len, "%shost_traddr=%s",	/* [한국어] 포맷 작성 */
				(len) ? "," : "", ctrl->opts->host_traddr);	/* [한국어] 로컬 바인드 주소 */
	if (ctrl->opts->mask & NVMF_OPT_HOST_IFACE)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		len += scnprintf(buf + len, size - len, "%shost_iface=%s",	/* [한국어] 포맷 작성 */
				(len) ? "," : "", ctrl->opts->host_iface);	/* [한국어] 로컬 인터페이스 이름 */
	len += scnprintf(buf + len, size - len, "\n");	/* [한국어] sysfs 관례 개행 */

	return len;	/* [한국어] 호출 결과 반환 */
}
EXPORT_SYMBOL_GPL(nvmf_get_address);	/* [한국어] 트랜스포트/sysfs 모듈 공유 */

/**
 * nvmf_reg_read32() -  NVMe Fabrics "Property Get" API function.
 * ... (kernel-doc preserved above) ...
 */
/*
 * [한국어]
 * nvmf_reg_read32 - Fabrics Property Get 으로 원격 32-bit "레지스터" 읽기
 *
 * PCIe BAR MMIO 의 fabrics 대응. CAP/CSTS 등 offset 을 캡슐로 조회.
 * fabrics_q 동기 제출, 결과는 CQE result 64bit 하위. attrib=0 (32-bit).
 * 호출 체인: enable/disable_ctrl 등 → ctrl->ops->reg_read32 → [여기]
 */
int nvmf_reg_read32(struct nvme_ctrl *ctrl, u32 off, u32 *val)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_command cmd = { };	/* [한국어] Property Get 캡슐 SQE */
	union nvme_result res;	/* [한국어] CQE result 필드 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	cmd.prop_get.opcode = nvme_fabrics_command;	/* [한국어] Fabrics opcode */
	cmd.prop_get.fctype = nvme_fabrics_type_property_get;	/* [한국어] fctype = Property Get */
	cmd.prop_get.offset = cpu_to_le32(off);	/* [한국어] 대상 property 오프셋 (예: CSTS) */

	ret = __nvme_submit_sync_cmd(ctrl->fabrics_q, &cmd, &res, NULL, 0,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			NVME_QID_ANY, NVME_SUBMIT_RESERVED);	/* [한국어] fabrics_q 동기 제출; RESERVED 태그 사용 */

	if (ret >= 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		*val = le64_to_cpu(res.u64);	/* [한국어] 성공/NVMe status 시에도 result 에 값이 올 수 있음 */
	if (unlikely(ret != 0))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Property Get error: %d, offset %#x\n",
			ret > 0 ? ret & ~NVME_STATUS_DNR : ret, off);	/* [한국어] DNR 비트 제거한 가독 로그 */

	return ret;	/* [한국어] 결과 코드 전파 */
}
EXPORT_SYMBOL_GPL(nvmf_reg_read32);	/* [한국어] Fabrics 공통 라이브러리 */

/**
 * nvmf_reg_read64() -  NVMe Fabrics "Property Get" API function (64-bit).
 */
/*
 * [한국어]
 * nvmf_reg_read64 - Property Get 64-bit (attrib=1) — CAP 등 8바이트 property
 *
 * 32-bit 버전과 동일 경로이나 attrib 로 폭을 지정. 스펙 fabrics property 모델.
 */
int nvmf_reg_read64(struct nvme_ctrl *ctrl, u32 off, u64 *val)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_command cmd = { };	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	union nvme_result res;	/* [한국어] res — 함수/구조 문맥의 상태 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	cmd.prop_get.opcode = nvme_fabrics_command;	/* [한국어] nvme_fabrics_command — 함수/구조 문맥의 상태 */
	cmd.prop_get.fctype = nvme_fabrics_type_property_get;	/* [한국어] nvme_fabrics_type_property_get — 함수/구조 문맥의 상태 */
	cmd.prop_get.attrib = 1;	/* [한국어] 1 = 64-bit property 크기 */
	cmd.prop_get.offset = cpu_to_le32(off);	/* [한국어] LE 온와이어 엔디안 변환 */

	ret = __nvme_submit_sync_cmd(ctrl->fabrics_q, &cmd, &res, NULL, 0,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			NVME_QID_ANY, NVME_SUBMIT_RESERVED);

	if (ret >= 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		*val = le64_to_cpu(res.u64);	/* [한국어] 64-bit property 값 */
	if (unlikely(ret != 0))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Property Get error: %d, offset %#x\n",
			ret > 0 ? ret & ~NVME_STATUS_DNR : ret, off);
	return ret;	/* [한국어] 결과 코드 전파 */
}
EXPORT_SYMBOL_GPL(nvmf_reg_read64);	/* [한국어] Fabrics 공통 라이브러리 */

/**
 * nvmf_reg_write32() -  NVMe Fabrics "Property Write" API function.
 */
/*
 * [한국어]
 * nvmf_reg_write32 - Property Set 으로 원격 32-bit 레지스터 기록 (CC 등)
 *
 * enable 시 CC.EN=1, shutdown 시 SHN 등. fabrics_q 동기. attrib=0 (32-bit).
 */
int nvmf_reg_write32(struct nvme_ctrl *ctrl, u32 off, u32 val)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_command cmd = { };	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	cmd.prop_set.opcode = nvme_fabrics_command;	/* [한국어] nvme_fabrics_command — 함수/구조 문맥의 상태 */
	cmd.prop_set.fctype = nvme_fabrics_type_property_set;	/* [한국어] fctype = Property Set */
	cmd.prop_set.attrib = 0;	/* [한국어] 32-bit 기록 */
	cmd.prop_set.offset = cpu_to_le32(off);	/* [한국어] LE 온와이어 엔디안 변환 */
	cmd.prop_set.value = cpu_to_le64(val);	/* [한국어] 스펙상 value 는 64-bit 필드에 담아 전송 */

	ret = __nvme_submit_sync_cmd(ctrl->fabrics_q, &cmd, NULL, NULL, 0,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			NVME_QID_ANY, NVME_SUBMIT_RESERVED);
	if (unlikely(ret))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Property Set error: %d, offset %#x\n",
			ret > 0 ? ret & ~NVME_STATUS_DNR : ret, off);
	return ret;	/* [한국어] 결과 코드 전파 */
}
EXPORT_SYMBOL_GPL(nvmf_reg_write32);	/* [한국어] Fabrics 공통 라이브러리 */

/*
 * [한국어]
 * nvmf_subsystem_reset - NSSR 에 서브시스템 리셋 매직 기록 후 리셋 스케줄
 *
 * nvme_wait_reset 으로 리셋 권한 획득. reg_write32(NSSR) 후 try_sched_reset.
 */
int nvmf_subsystem_reset(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	if (!nvme_wait_reset(ctrl))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return -EBUSY;	/* [한국어] 이미 다른 리셋/상태 전이 진행 중 */

	ret = ctrl->ops->reg_write32(ctrl, NVME_REG_NSSR, NVME_SUBSYS_RESET);	/* [한국어] 원격 NSSR 에 리셋 시그니처 기록 */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return ret;	/* [한국어] 결과 코드 전파 */

	return nvme_try_sched_reset(ctrl);	/* [한국어] 호스트 측 리셋 워크 스케줄 */
}
EXPORT_SYMBOL_GPL(nvmf_subsystem_reset);	/* [한국어] Fabrics 공통 라이브러리 */

/**
 * nvmf_log_connect_error() - Error-parsing-diagnostic print out function for
 * 				connect() errors.
 * @ctrl:	The specific /dev/nvmeX device that had the error.
 * @errval:	Error code to be decoded in a more human-friendly
 * 		printout.
 * @offset:	For use with the NVMe error code
 * 		NVME_SC_CONNECT_INVALID_PARAM.
 * @cmd:	This is the SQE portion of a submission capsule.
 * @data:	This is the "Data" portion of a submission capsule.
 */
/*
 * [한국어]
 * nvmf_log_connect_error - Connect 실패를 스펙 offset/status 별로 해석 로그
 *
 * INVALID_PARAM 시 CQE result 의 offset 이 SQE 필드인지 data 필드인지
 * (상위 16비트)로 갈라 잘못된 파라미터를 명시. 인증·권한·busy 등 공통 코드 처리.
 */
static void nvmf_log_connect_error(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		int errval, int offset, struct nvme_command *cmd,	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
		struct nvmf_connect_data *data)
{
	int err_sctype = errval & ~NVME_STATUS_DNR;	/* [한국어] DNR 제거한 SC — 분기 비교용 */

	if (errval < 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Connect command failed, errno: %d\n", errval);	/* [한국어] 호스트측 errno (타임아웃·전송 실패 등) */
		return;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}

	switch (err_sctype) {	/* [한국어] 상태/유형 디스패치 */
	case NVME_SC_CONNECT_INVALID_PARAM:	/* [한국어] 잘못된 Connect 파라미터 — offset 으로 필드 식별 */
		if (offset >> 16) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			char *inv_data = "Connect Invalid Data Parameter";	/* [한국어] data 페이로드 쪽 오류 */

			switch (offset & 0xffff) {	/* [한국어] 상태/유형 디스패치 */
			case (offsetof(struct nvmf_connect_data, cntlid)):	/* [한국어] Fabrics 공통 라이브러리 */
				dev_err(ctrl->device,	/* [한국어] 진단 로그 */
					"%s, cntlid: %d\n",
					inv_data, data->cntlid);	/* [한국어] 잘못된 컨트롤러 ID */
				break;	/* [한국어] 루프/스위치 종료 */
			case (offsetof(struct nvmf_connect_data, hostnqn)):	/* [한국어] Fabrics 공통 라이브러리 */
				dev_err(ctrl->device,	/* [한국어] 진단 로그 */
					"%s, hostnqn \"%s\"\n",
					inv_data, data->hostnqn);	/* [한국어] 거부/형식 오류 Host NQN */
				break;	/* [한국어] 루프/스위치 종료 */
			case (offsetof(struct nvmf_connect_data, subsysnqn)):	/* [한국어] Fabrics 공통 라이브러리 */
				dev_err(ctrl->device,	/* [한국어] 진단 로그 */
					"%s, subsysnqn \"%s\"\n",
					inv_data, data->subsysnqn);	/* [한국어] 알 수 없는/거부된 서브시스템 NQN */
				break;	/* [한국어] 루프/스위치 종료 */
			default:	/* [한국어] 예약/미지 값 방어 */
				dev_err(ctrl->device,	/* [한국어] 진단 로그 */
					"%s, starting byte offset: %d\n",
				       inv_data, offset & 0xffff);	/* [한국어] 기타 data 오프셋 */
				break;	/* [한국어] 루프/스위치 종료 */
			}
		} else {	/* [한국어] 대안 경로 */
			char *inv_sqe = "Connect Invalid SQE Parameter";	/* [한국어] SQE 쪽 오류 */

			switch (offset) {	/* [한국어] 상태/유형 디스패치 */
			case (offsetof(struct nvmf_connect_command, qid)):	/* [한국어] Fabrics 공통 라이브러리 */
				dev_err(ctrl->device,	/* [한국어] 진단 로그 */
				       "%s, qid %d\n",
					inv_sqe, cmd->connect.qid);	/* [한국어] 잘못된 큐 ID */
				break;	/* [한국어] 루프/스위치 종료 */
			default:	/* [한국어] 예약/미지 값 방어 */
				dev_err(ctrl->device,	/* [한국어] 진단 로그 */
					"%s, starting byte offset: %d\n",
					inv_sqe, offset);
			}
		}
		break;	/* [한국어] 루프/스위치 종료 */
	case NVME_SC_CONNECT_INVALID_HOST:	/* [한국어] 다중 분기 케이스 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Connect for subsystem %s is not allowed, hostnqn: %s\n",
			data->subsysnqn, data->hostnqn);	/* [한국어] ACL/호스트 허용 목록 거부 */
		break;	/* [한국어] 루프/스위치 종료 */
	case NVME_SC_CONNECT_CTRL_BUSY:	/* [한국어] 다중 분기 케이스 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Connect command failed: controller is busy or not available\n");	/* [한국어] 리소스 고갈·일시 불가 */
		break;	/* [한국어] 루프/스위치 종료 */
	case NVME_SC_CONNECT_FORMAT:	/* [한국어] 다중 분기 케이스 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Connect incompatible format: %d",
			cmd->connect.recfmt);	/* [한국어] 레코드 포맷 버전 불일치 */
		break;	/* [한국어] 루프/스위치 종료 */
	case NVME_SC_HOST_PATH_ERROR:	/* [한국어] 다중 분기 케이스 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Connect command failed: host path error\n");	/* [한국어] 호스트 경로 계층 오류 */
		break;	/* [한국어] 루프/스위치 종료 */
	case NVME_SC_AUTH_REQUIRED:	/* [한국어] 다중 분기 케이스 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Connect command failed: authentication required\n");	/* [한국어] 인증 필수인데 협상 실패/미지원 */
		break;	/* [한국어] 루프/스위치 종료 */
	default:	/* [한국어] 예약/미지 값 방어 */
		dev_err(ctrl->device,	/* [한국어] 진단 로그 */
			"Connect command failed, error wo/DNR bit: %d\n",
			err_sctype);	/* [한국어] 기타 NVMe status */
		break;	/* [한국어] 루프/스위치 종료 */
	}
}

/*
 * [한국어]
 * nvmf_connect_data_prep - Connect 데이터 페이로드 (hostid/cntlid/NQN) 구성
 *
 * Admin Connect 는 cntlid=0xffff(동적 할당 요청), I/O Connect 는 할당된 cntlid.
 */
static struct nvmf_connect_data *nvmf_connect_data_prep(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		u16 cntlid)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvmf_connect_data *data;	/* [한국어] data — 함수/구조 문맥의 상태 */

	data = kzalloc_obj(*data);	/* [한국어] 1024B Connect data 제로 할당 */
	if (!data)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return NULL;	/* [한국어] 대상 없음 */

	uuid_copy(&data->hostid, &ctrl->opts->host->id);	/* [한국어] Host Identifier */
	data->cntlid = cpu_to_le16(cntlid);	/* [한국어] Admin: 0xffff / I/O: 할당 cntlid */
	strscpy(data->subsysnqn, ctrl->opts->subsysnqn, NVMF_NQN_SIZE);	/* [한국어] 대상 서브시스템 NQN */
	strscpy(data->hostnqn, ctrl->opts->host->nqn, NVMF_NQN_SIZE);	/* [한국어] Host NQN */

	return data;	/* [한국어] 호출 결과 반환 */
}

/*
 * [한국어]
 * nvmf_connect_cmd_prep - Connect SQE 공통 필드 (qid/sqsize/kato/cattr) 채움
 *
 * Admin(qid=0): AQ depth-1, KATO(ms). I/O: ctrl->sqsize. disable_sqflow 옵션 반영.
 */
static void nvmf_connect_cmd_prep(struct nvme_ctrl *ctrl, u16 qid,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvme_command *cmd)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	cmd->connect.opcode = nvme_fabrics_command;	/* [한국어] nvme_fabrics_command — 함수/구조 문맥의 상태 */
	cmd->connect.fctype = nvme_fabrics_type_connect;	/* [한국어] fctype = Connect */
	cmd->connect.qid = cpu_to_le16(qid);	/* [한국어] 0=Admin, 그 외 I/O 큐 번호 */

	if (qid) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		cmd->connect.sqsize = cpu_to_le16(ctrl->sqsize);	/* [한국어] I/O SQ 크기 (0-based) */
	} else {	/* [한국어] 대안 경로 */
		cmd->connect.sqsize = cpu_to_le16(NVME_AQ_DEPTH - 1);	/* [한국어] Admin 큐 깊이 0-based */

		/*
		 * set keep-alive timeout in seconds granularity (ms * 1000)
		 */
		/* [한국어] KATO 를 밀리초 단위로 캡슐에 실어 타깃 keep-alive 정책과 동기화 */
		cmd->connect.kato = cpu_to_le32(ctrl->kato * 1000);	/* [한국어] LE 온와이어 엔디안 변환 */
	}

	if (ctrl->opts->disable_sqflow)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		cmd->connect.cattr |= NVME_CONNECT_DISABLE_SQFLOW;	/* [한국어] SQ 흐름제어 비활성 요청 비트 */
}

/**
 * nvmf_connect_admin_queue() - NVMe Fabrics Admin Queue "Connect"
 */
/*
 * [한국어]
 * nvmf_connect_admin_queue - Admin 큐 Connect: 원격 cntlid 할당 + Admin 세션 성립
 *
 * cntlid=0xffff 로 동적 할당 요청. 성공 시 result 하위 16비트가 cntlid.
 * AUTHREQ 비트 시 auth.c 로 DH-HMAC-CHAP 협상. ASCR 는 concat 옵션 필수.
 * 제출: fabrics_q, AT_HEAD|NOWAIT|RESERVED — 연결 초기 전용 경로.
 * 호출: tcp/rdma/fc 의 setup_ctrl / reconnect 초기 단계.
 */
int nvmf_connect_admin_queue(struct nvme_ctrl *ctrl)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_command cmd = { };	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	union nvme_result res;	/* [한국어] res — 함수/구조 문맥의 상태 */
	struct nvmf_connect_data *data;	/* [한국어] data — 함수/구조 문맥의 상태 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
	u32 result;	/* [한국어] CQE result: cntlid | AUTHREQ 플래그 */

	nvmf_connect_cmd_prep(ctrl, 0, &cmd);	/* [한국어] Admin qid=0 SQE 준비 */

	data = nvmf_connect_data_prep(ctrl, 0xffff);	/* [한국어] 동적 cntlid 할당 요청 매직 */
	if (!data)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */

	ret = __nvme_submit_sync_cmd(ctrl->fabrics_q, &cmd, &res,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			data, sizeof(*data), NVME_QID_ANY,
			NVME_SUBMIT_AT_HEAD |
			NVME_SUBMIT_NOWAIT |
			NVME_SUBMIT_RESERVED);	/* [한국어] 연결 큐 헤드 우선·예약 태그·대기 없이 동기 완료 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvmf_log_connect_error(ctrl, ret, le32_to_cpu(res.u32),	/* [한국어] Fabrics 공통 라이브러리 */
				       &cmd, data);	/* [한국어] 실패 원인 상세 로그 */
		goto out_free_data;	/* [한국어] out_free_data — 함수/구조 문맥의 상태 */
	}

	result = le32_to_cpu(res.u32);	/* [한국어] result 상수 — 상위 enum 역할 참고 */
	ctrl->cntlid = result & 0xFFFF;	/* [한국어] 타깃이 할당한 컨트롤러 ID 저장 — I/O Connect 에 재사용 */
	if (result & (NVME_CONNECT_AUTHREQ_ATR | NVME_CONNECT_AUTHREQ_ASCR)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* Check for secure concatenation */
		if ((result & NVME_CONNECT_AUTHREQ_ASCR) &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		    !ctrl->opts->concat) {
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid 0: secure concatenation is not supported\n");	/* [한국어] 타깃 ASCR 요구 but 호스트 concat 미설정 */
			ret = -EOPNOTSUPP;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out_free_data;	/* [한국어] out_free_data — 함수/구조 문맥의 상태 */
		}
		/* Authentication required */
		ret = nvme_auth_negotiate(ctrl, 0);	/* [한국어] Admin 큐 DH-HMAC-CHAP 워크 시작 */
		if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid 0: authentication setup failed\n");
			goto out_free_data;	/* [한국어] out_free_data — 함수/구조 문맥의 상태 */
		}
		ret = nvme_auth_wait(ctrl, 0);	/* [한국어] auth_work 완료 대기 후 민감 버퍼 정리 */
		if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid 0: authentication failed, error %d\n",
				 ret);
		} else	/* [한국어] 대안 경로 */
			dev_info(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid 0: authenticated\n");	/* [한국어] Admin 인증 성공 */
	}
out_free_data:
	kfree(data);	/* [한국어] Connect data 페이로드 해제 */
	return ret;	/* [한국어] 결과 코드 전파 */
}
EXPORT_SYMBOL_GPL(nvmf_connect_admin_queue);	/* [한국어] Fabrics 공통 라이브러리 */

/**
 * nvmf_connect_io_queue() - NVMe Fabrics I/O Queue "Connect"
 */
/*
 * [한국어]
 * nvmf_connect_io_queue - 이미 할당된 cntlid 로 I/O 큐 Connect
 *
 * connect_q 에 qid 지정 제출. I/O 큐 ASCR(secure concat) 는 미구현(-EOPNOTSUPP).
 * ATR 만 지원 시 큐별 auth negotiate/wait.
 */
int nvmf_connect_io_queue(struct nvme_ctrl *ctrl, u16 qid)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_command cmd = { };	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
	struct nvmf_connect_data *data;	/* [한국어] data — 함수/구조 문맥의 상태 */
	union nvme_result res;	/* [한국어] res — 함수/구조 문맥의 상태 */
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */
	u32 result;	/* [한국어] result — 함수/구조 문맥의 상태 */

	nvmf_connect_cmd_prep(ctrl, qid, &cmd);	/* [한국어] I/O qid SQE */

	data = nvmf_connect_data_prep(ctrl, ctrl->cntlid);	/* [한국어] Admin 에서 받은 cntlid 재사용 */
	if (!data)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */

	ret = __nvme_submit_sync_cmd(ctrl->connect_q, &cmd, &res,	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			data, sizeof(*data), qid,
			NVME_SUBMIT_AT_HEAD |
			NVME_SUBMIT_RESERVED |
			NVME_SUBMIT_NOWAIT);	/* [한국어] 해당 I/O qid 컨텍스트로 Connect 제출 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvmf_log_connect_error(ctrl, ret, le32_to_cpu(res.u32),	/* [한국어] Fabrics 공통 라이브러리 */
				       &cmd, data);
		goto out_free_data;	/* [한국어] out_free_data — 함수/구조 문맥의 상태 */
	}
	result = le32_to_cpu(res.u32);	/* [한국어] result 상수 — 상위 enum 역할 참고 */
	if (result & (NVME_CONNECT_AUTHREQ_ATR | NVME_CONNECT_AUTHREQ_ASCR)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* Secure concatenation is not implemented */
		if (result & NVME_CONNECT_AUTHREQ_ASCR) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: secure concatenation is not supported\n", qid);	/* [한국어] I/O 큐 ASCR 미지원 */
			ret = -EOPNOTSUPP;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out_free_data;	/* [한국어] out_free_data — 함수/구조 문맥의 상태 */
		}
		/* Authentication required */
		ret = nvme_auth_negotiate(ctrl, qid);	/* [한국어] 이 I/O 큐 인증 워크 */
		if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %d: authentication setup failed\n", qid);
			goto out_free_data;	/* [한국어] out_free_data — 함수/구조 문맥의 상태 */
		}
		ret = nvme_auth_wait(ctrl, qid);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			dev_warn(ctrl->device,	/* [한국어] 진단 로그 */
				 "qid %u: authentication failed, error %d\n",
				 qid, ret);
		}
	}
out_free_data:
	kfree(data);	/* [한국어] 동적 메모리 해제 */
	return ret;	/* [한국어] 결과 코드 전파 */
}
EXPORT_SYMBOL_GPL(nvmf_connect_io_queue);	/* [한국어] Fabrics 공통 라이브러리 */

/*
 * Evaluate the status information returned by the transport in order to decided
 * if a reconnect attempt should be scheduled.
 *
 * Do not retry when:
 *
 * - the DNR bit is set and the specification states no further connect
 *   attempts with the same set of parameters should be attempted.
 *
 * - when the authentication attempt fails, because the key was invalid.
 *   This error code is set on the host side.
 */
/*
 * [한국어]
 * nvmf_should_reconnect - 재연결 시도 여부 정책
 *
 * DNR status → 동일 파라미터 재시도 금지. -EKEYREJECTED/-ENOKEY → 키 문제라
 * 재시도 무의미. max_reconnects==-1 이면 무한, 아니면 nr_reconnects 비교.
 * 호출: 트랜스포트 error_recovery / reconnect 워크.
 */
bool nvmf_should_reconnect(struct nvme_ctrl *ctrl, int status)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (status > 0 && (status & NVME_STATUS_DNR))	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return false;	/* [한국어] 스펙 Do Not Retry — 동일 파라미터로 재 Connect 금지 */

	if (status == -EKEYREJECTED || status == -ENOKEY)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return false;	/* [한국어] 호스트측 키 거부/부재 — 재시도해도 동일 실패 */

	if (ctrl->opts->max_reconnects == -1 ||	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
	    ctrl->nr_reconnects < ctrl->opts->max_reconnects)	/* [한국어] 인자/선언 연속행 */
		return true;	/* [한국어] 무한 또는 남은 재시도 횟수 있음 */

	return false;	/* [한국어] ctrl_loss_tmo 환산 재시도 한도 소진 */
}
EXPORT_SYMBOL_GPL(nvmf_should_reconnect);	/* [한국어] Fabrics 공통 라이브러리 */

/**
 * nvmf_register_transport() - NVMe Fabrics Library registration function.
 */
/*
 * [한국어]
 * nvmf_register_transport - tcp/rdma/fc 모듈이 ops 를 공통 라이브러리에 등록
 *
 * create_ctrl 필수. 모듈 init 에서 호출. nvmf_create_ctrl 이 이름으로 조회.
 */
int nvmf_register_transport(struct nvmf_transport_ops *ops)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	if (!ops->create_ctrl)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return -EINVAL;	/* [한국어] create_ctrl 없는 ops 는 무의미 */

	down_write(&nvmf_transports_rwsem);	/* [한국어] 등록은 배타 write 락 */
	list_add_tail(&ops->entry, &nvmf_transports);	/* [한국어] 전역 트랜스포트 목록 추가 */
	up_write(&nvmf_transports_rwsem);	/* [한국어] Fabrics 공통 라이브러리 */

	return 0;	/* [한국어] 성공/no-op 완료 */
}
EXPORT_SYMBOL_GPL(nvmf_register_transport);	/* [한국어] Fabrics 공통 라이브러리 */

/**
 * nvmf_unregister_transport() - NVMe Fabrics Library unregistration function.
 */
/*
 * [한국어]
 * nvmf_unregister_transport - 모듈 exit 시 ops 목록에서 제거
 */
void nvmf_unregister_transport(struct nvmf_transport_ops *ops)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	down_write(&nvmf_transports_rwsem);	/* [한국어] Fabrics 공통 라이브러리 */
	list_del(&ops->entry);	/* [한국어] 등록 해제 — 이후 create_ctrl 조회 실패 */
	up_write(&nvmf_transports_rwsem);	/* [한국어] Fabrics 공통 라이브러리 */
}
EXPORT_SYMBOL_GPL(nvmf_unregister_transport);	/* [한국어] Fabrics 공통 라이브러리 */

/*
 * [한국어]
 * nvmf_lookup_transport - opts->transport 이름("tcp" 등)으로 ops 검색
 *
 * 전제: nvmf_transports_rwsem read 보유.
 */
static struct nvmf_transport_ops *nvmf_lookup_transport(	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvmf_ctrl_options *opts)
{
	struct nvmf_transport_ops *ops;	/* [한국어] ops — 함수/구조 문맥의 상태 */

	lockdep_assert_held(&nvmf_transports_rwsem);	/* [한국어] 조회 중 등록/해제 경합 방지 전제 */

	list_for_each_entry(ops, &nvmf_transports, entry) {	/* [한국어] Fabrics 공통 라이브러리 */
		if (strcmp(ops->name, opts->transport) == 0)	/* [한국어] 메모리/문자열 연산 */
			return ops;	/* [한국어] 이름 일치 트랜스포트 발견 */
	}

	return NULL;	/* [한국어] 미로드 모듈 또는 오타 — 호출자가 request_module 후 재시도 가능 */
}

/*
 * [한국어]
 * nvmf_parse_key - 키링 serial 로 TLS 키 객체 조회
 *
 * CONFIG_NVME_TCP_TLS 필수. keyring=/tls_key= 옵션 파싱에서 사용.
 */
static struct key *nvmf_parse_key(int key_id)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct key *key;	/* [한국어] key — 함수/구조 문맥의 상태 */

	if (!IS_ENABLED(CONFIG_NVME_TCP_TLS)) {	/* [한국어] Kconfig 게이트 */
		pr_err("TLS is not supported\n");	/* [한국어] 빌드에 TLS 없음 */
		return ERR_PTR(-EINVAL);	/* [한국어] 에러 포인터 규약 */
	}

	key = nvme_tls_key_lookup(key_id);	/* [한국어] key_id 로 커널 키링 조회 */
	if (IS_ERR(key))	/* [한국어] 에러 포인터 규약 */
		pr_err("key id %08x not found\n", key_id);	/* [한국어] 사용자 공간 미등록 키 */
	else
		pr_debug("Using key id %08x\n", key_id);	/* [한국어] 진단 로그 */
	return key;	/* [한국어] 호출 결과 반환 */
}

/* [한국어] /dev/nvme-fabrics write 문자열 토큰 테이블 — match_token 이 비트마스크 token 반환 */
static const match_table_t opt_tokens = {
	{ NVMF_OPT_TRANSPORT,		"transport=%s"		},	/* [한국어] tcp|rdma|fc 등 트랜스포트 이름 */
	{ NVMF_OPT_TRADDR,		"traddr=%s"		},	/* [한국어] 원격 주소 */
	{ NVMF_OPT_TRSVCID,		"trsvcid=%s"		},	/* [한국어] 원격 포트/서비스 ID */
	{ NVMF_OPT_NQN,			"nqn=%s"		},	/* [한국어] 서브시스템 NQN (필수) */
	{ NVMF_OPT_QUEUE_SIZE,		"queue_size=%d"		},	/* [한국어] SQ/CQ 깊이 */
	{ NVMF_OPT_NR_IO_QUEUES,	"nr_io_queues=%d"	},	/* [한국어] I/O 큐 개수 상한 */
	{ NVMF_OPT_RECONNECT_DELAY,	"reconnect_delay=%d"	},	/* [한국어] 재연결 간격(초) */
	{ NVMF_OPT_CTRL_LOSS_TMO,	"ctrl_loss_tmo=%d"	},	/* [한국어] 컨트롤러 손실 타임아웃 → max_reconnects */
	{ NVMF_OPT_KATO,		"keep_alive_tmo=%d"	},	/* [한국어] Keep-Alive 타임아웃(초) */
	{ NVMF_OPT_HOSTNQN,		"hostnqn=%s"		},	/* [한국어] 사용자 지정 Host NQN */
	{ NVMF_OPT_HOST_TRADDR,		"host_traddr=%s"	},	/* [한국어] 로컬 바인드 주소 */
	{ NVMF_OPT_HOST_IFACE,		"host_iface=%s"		},	/* [한국어] 로컬 인터페이스 */
	{ NVMF_OPT_HOST_ID,		"hostid=%s"		},	/* [한국어] Host UUID 문자열 */
	{ NVMF_OPT_DUP_CONNECT,		"duplicate_connect"	},	/* [한국어] 동일 대상 중복 연결 허용 */
	{ NVMF_OPT_DISABLE_SQFLOW,	"disable_sqflow"	},	/* [한국어] SQ 흐름제어 비활성 */
	{ NVMF_OPT_HDR_DIGEST,		"hdr_digest"		},	/* [한국어] TCP 헤더 digest */
	{ NVMF_OPT_DATA_DIGEST,		"data_digest"		},	/* [한국어] TCP 데이터 digest */
	{ NVMF_OPT_NR_WRITE_QUEUES,	"nr_write_queues=%d"	},	/* [한국어] 전용 write 큐 수 */
	{ NVMF_OPT_NR_POLL_QUEUES,	"nr_poll_queues=%d"	},	/* [한국어] 폴링 큐 수 */
	{ NVMF_OPT_TOS,			"tos=%d"		},	/* [한국어] IP Type-of-Service */
#ifdef CONFIG_NVME_TCP_TLS
	{ NVMF_OPT_KEYRING,		"keyring=%d"		},	/* [한국어] 키 조회용 키링 id */
	{ NVMF_OPT_TLS_KEY,		"tls_key=%d"		},	/* [한국어] 사전 공유 TLS 키 id */
#endif
	{ NVMF_OPT_FAIL_FAST_TMO,	"fast_io_fail_tmo=%d"	},	/* [한국어] 재연결 중 I/O 빠른 실패 시간 */
	{ NVMF_OPT_DISCOVERY,		"discovery"		},	/* [한국어] 디스커버리 컨트롤러 강제 */
#ifdef CONFIG_NVME_HOST_AUTH
	{ NVMF_OPT_DHCHAP_SECRET,	"dhchap_secret=%s"	},	/* [한국어] 호스트 DH-HMAC-CHAP 시크릿 */
	{ NVMF_OPT_DHCHAP_CTRL_SECRET,	"dhchap_ctrl_secret=%s"	},	/* [한국어] 양방향 인증용 컨트롤러 시크릿 */
#endif
#ifdef CONFIG_NVME_TCP_TLS
	{ NVMF_OPT_TLS,			"tls"			},	/* [한국어] TLS 암호화 세션 요청 */
	{ NVMF_OPT_CONCAT,		"concat"		},	/* [한국어] DH-CHAP→TLS PSK 보안 채널 결합 */
#endif
	{ NVMF_OPT_ERR,			NULL			}	/* [한국어] 매칭 실패 센티널 */
};

/*
 * [한국어]
 * nvmf_parse_options - connect 문자열을 nvmf_ctrl_options 로 파싱
 *
 * @opts: 출력 옵션 (호출자가 할당, mask/필드 채움)
 * @buf: "transport=tcp,traddr=...,nqn=..." 콤마/개행 구분
 * @return: 0 또는 음수 errno
 *
 * 기본값 설정 → strsep 토큰 루프 → discovery/kato/ctrl_loss/concat 교차 검증
 * → nvmf_host_add. 파괴적 파싱을 위해 kstrdup 사본 사용.
 * 호출: nvmf_create_ctrl.
 */
static int nvmf_parse_options(struct nvmf_ctrl_options *opts,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		const char *buf)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	substring_t args[MAX_OPT_ARGS];	/* [한국어] match_token 인자 슬롯 */
	char *options, *o, *p;	/* [한국어] 복제 버퍼 / 순회 커서 / 현재 토큰 */
	int token, ret = 0;	/* [한국어] 매칭 토큰 비트 / 누적 에러 */
	size_t nqnlen  = 0;	/* [한국어] NQN 길이 검증용 */
	int ctrl_loss_tmo = NVMF_DEF_CTRL_LOSS_TMO, key_id;	/* [한국어] 손실 TMO 기본 600초 / 키 serial */
	uuid_t hostid;	/* [한국어] 파싱 중 Host ID (기본 host 로 시드) */
	char hostnqn[NVMF_NQN_SIZE];	/* [한국어] 파싱 중 Host NQN */
	struct key *key;	/* [한국어] TLS 키 조회 결과 */

	/* Set defaults */
	/* [한국어] 사용자 미지정 필드 기본값 — 트랜스포트·운영 관례 */
	opts->queue_size = NVMF_DEF_QUEUE_SIZE;	/* [한국어] 기본 큐 깊이 128 */
	opts->nr_io_queues = num_online_cpus();	/* [한국어] CPU 수만큼 I/O 큐 기본 요청 */
	opts->reconnect_delay = NVMF_DEF_RECONNECT_DELAY;	/* [한국어] 기본 재연결 간격 10초 */
	opts->kato = 0;	/* [한국어] 0=아래에서 discovery 가 아니면 DEFAULT_KATO 로 채움 */
	opts->duplicate_connect = false;	/* [한국어] false — 함수/구조 문맥의 상태 */
	opts->fast_io_fail_tmo = NVMF_DEF_FAIL_FAST_TMO;	/* [한국어] 기본 -1: fail-fast 비활성 */
	opts->hdr_digest = false;	/* [한국어] false — 함수/구조 문맥의 상태 */
	opts->data_digest = false;	/* [한국어] false — 함수/구조 문맥의 상태 */
	opts->tos = -1; /* < 0 == use transport default */	/* [한국어] 음수=트랜스포트 기본 ToS */
	opts->tls = false;	/* [한국어] false — 함수/구조 문맥의 상태 */
	opts->tls_key = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	opts->keyring = NULL;	/* [한국어] NULL — 함수/구조 문맥의 상태 */
	opts->concat = false;	/* [한국어] false — 함수/구조 문맥의 상태 */

	options = o = kstrdup(buf, GFP_KERNEL);	/* [한국어] strsep 가 버퍼를 파괴하므로 사본 필요 */
	if (!options)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */

	/* use default host if not given by user space */
	/* [한국어] hostnqn/hostid 미지정 시 모듈 기본 host 정체성 사용 */
	uuid_copy(&hostid, &nvmf_default_host->id);	/* [한국어] Fabrics 공통 라이브러리 */
	strscpy(hostnqn, nvmf_default_host->nqn, NVMF_NQN_SIZE);	/* [한국어] Fabrics 공통 라이브러리 */

	while ((p = strsep(&o, ",\n")) != NULL) {	/* [한국어] 콤마/개행으로 옵션 토큰 분리 */
		if (!*p)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			continue;	/* [한국어] 빈 토큰 스킵 */

		token = match_token(p, opt_tokens, args);	/* [한국어] 패턴 매칭 → NVMF_OPT_* 비트 */
		opts->mask |= token;	/* [한국어] 지정된 옵션 비트 누적 — required/allowed 검사에 사용 */
		switch (token) {	/* [한국어] 상태/유형 디스패치 */
		case NVMF_OPT_TRANSPORT:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] transport= 문자열 복제 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->transport);	/* [한국어] 중복 지정 시 이전 값 해제 */
			opts->transport = p;	/* [한국어] tcp/rdma/fc 등 — lookup_transport 키 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_NQN:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->subsysnqn);	/* [한국어] 동적 메모리 해제 */
			opts->subsysnqn = p;	/* [한국어] 연결 대상 서브시스템 NQN */
			nqnlen = strlen(opts->subsysnqn);	/* [한국어] nqnlen 상수 — 상위 enum 역할 참고 */
			if (nqnlen >= NVMF_NQN_SIZE) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("%s needs to be < %d bytes\n",	/* [한국어] 진단 로그 */
					opts->subsysnqn, NVMF_NQN_SIZE);	/* [한국어] 스펙 NQN 최대 길이 초과 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			opts->discovery_nqn =
				!(strcmp(opts->subsysnqn,	/* [한국어] 메모리/문자열 연산 */
					 NVME_DISC_SUBSYS_NAME));	/* [한국어] well-known discovery NQN 이면 디스커버리 모드 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_TRADDR:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->traddr);	/* [한국어] 동적 메모리 해제 */
			opts->traddr = p;	/* [한국어] p — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_TRSVCID:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->trsvcid);	/* [한국어] 동적 메모리 해제 */
			opts->trsvcid = p;	/* [한국어] p — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_QUEUE_SIZE:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (token < NVMF_MIN_QUEUE_SIZE ||	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			    token > NVMF_MAX_QUEUE_SIZE) {
				pr_err("Invalid queue_size %d\n", token);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			opts->queue_size = token;	/* [한국어] token — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_NR_IO_QUEUES:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (token <= 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("Invalid number of IOQs %d\n", token);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (opts->discovery_nqn) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_debug("Ignoring nr_io_queues value for discovery controller\n");	/* [한국어] 진단 로그 */
				break;	/* [한국어] 루프/스위치 종료 */
			}

			opts->nr_io_queues = min_t(unsigned int,	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
					num_online_cpus(), token);
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_KATO:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}

			if (token < 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("Invalid keep_alive_tmo %d\n", token);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			} else if (token == 0 && !opts->discovery_nqn) {	/* [한국어] 대안 조건 경로 */
				/* Allowed for debug */
				pr_warn("keep_alive_tmo 0 won't execute keep alives!!!\n");	/* [한국어] 진단 로그 */
			}
			opts->kato = token;	/* [한국어] token — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_CTRL_LOSS_TMO:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}

			if (token < 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_warn("ctrl_loss_tmo < 0 will reconnect forever\n");	/* [한국어] 진단 로그 */
			ctrl_loss_tmo = token;	/* [한국어] ctrl_loss_tmo 상수 — 상위 enum 역할 참고 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_FAIL_FAST_TMO:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}

			if (token >= 0)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_warn("I/O fail on reconnect controller after %d sec\n",	/* [한국어] 진단 로그 */
					token);
			else
				token = -1;	/* [한국어] token 상수 — 상위 enum 역할 참고 */

			opts->fast_io_fail_tmo = token;	/* [한국어] token — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_HOSTNQN:	/* [한국어] 다중 분기 케이스 */
			if (opts->host) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("hostnqn already user-assigned: %s\n",	/* [한국어] 진단 로그 */
				       opts->host->nqn);
				ret = -EADDRINUSE;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			nqnlen = strlen(p);	/* [한국어] nqnlen 상수 — 상위 enum 역할 참고 */
			if (nqnlen >= NVMF_NQN_SIZE) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("%s needs to be < %d bytes\n",	/* [한국어] 진단 로그 */
					p, NVMF_NQN_SIZE);
				kfree(p);	/* [한국어] 동적 메모리 해제 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			strscpy(hostnqn, p, NVMF_NQN_SIZE);	/* [한국어] 메모리/문자열 연산 */
			kfree(p);	/* [한국어] 동적 메모리 해제 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_RECONNECT_DELAY:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (token <= 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("Invalid reconnect_delay %d\n", token);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			opts->reconnect_delay = token;	/* [한국어] token — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_HOST_TRADDR:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->host_traddr);	/* [한국어] 동적 메모리 해제 */
			opts->host_traddr = p;	/* [한국어] p — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_HOST_IFACE:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->host_iface);	/* [한국어] 동적 메모리 해제 */
			opts->host_iface = p;	/* [한국어] p — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_HOST_ID:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			ret = uuid_parse(p, &hostid);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("Invalid hostid %s\n", p);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				kfree(p);	/* [한국어] 동적 메모리 해제 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(p);	/* [한국어] 동적 메모리 해제 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_DUP_CONNECT:	/* [한국어] 다중 분기 케이스 */
			opts->duplicate_connect = true;	/* [한국어] true — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_DISABLE_SQFLOW:	/* [한국어] 다중 분기 케이스 */
			opts->disable_sqflow = true;	/* [한국어] true — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_HDR_DIGEST:	/* [한국어] 다중 분기 케이스 */
			opts->hdr_digest = true;	/* [한국어] true — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_DATA_DIGEST:	/* [한국어] 다중 분기 케이스 */
			opts->data_digest = true;	/* [한국어] true — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_NR_WRITE_QUEUES:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (token <= 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("Invalid nr_write_queues %d\n", token);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			opts->nr_write_queues = token;	/* [한국어] token — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_NR_POLL_QUEUES:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (token <= 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("Invalid nr_poll_queues %d\n", token);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			opts->nr_poll_queues = token;	/* [한국어] token — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_TOS:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &token)) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (token < 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_err("Invalid type of service %d\n", token);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (token > 255) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				pr_warn("Clamping type of service to 255\n");	/* [한국어] 진단 로그 */
				token = 255;	/* [한국어] token 상수 — 상위 enum 역할 참고 */
			}
			opts->tos = token;	/* [한국어] token — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_KEYRING:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &key_id) || key_id <= 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			key = nvmf_parse_key(key_id);	/* [한국어] key 상수 — 상위 enum 역할 참고 */
			if (IS_ERR(key)) {	/* [한국어] 에러 포인터 규약 */
				ret = PTR_ERR(key);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			key_put(opts->keyring);	/* [한국어] 키링 수명/조회 */
			opts->keyring = key;	/* [한국어] key — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_TLS_KEY:	/* [한국어] 다중 분기 케이스 */
			if (match_int(args, &key_id) || key_id <= 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			key = nvmf_parse_key(key_id);	/* [한국어] key 상수 — 상위 enum 역할 참고 */
			if (IS_ERR(key)) {	/* [한국어] 에러 포인터 규약 */
				ret = PTR_ERR(key);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			key_put(opts->tls_key);	/* [한국어] 키링 수명/조회 */
			opts->tls_key = key;	/* [한국어] key — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_DISCOVERY:	/* [한국어] 다중 분기 케이스 */
			opts->discovery_nqn = true;	/* [한국어] true — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_DHCHAP_SECRET:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (strlen(p) < 11 || strncmp(p, "DHHC-1:", 7)) {	/* [한국어] 메모리/문자열 연산 */
				pr_err("Invalid DH-CHAP secret %s\n", p);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->dhchap_secret);	/* [한국어] 동적 메모리 해제 */
			opts->dhchap_secret = p;	/* [한국어] p — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_DHCHAP_CTRL_SECRET:	/* [한국어] 다중 분기 케이스 */
			p = match_strdup(args);	/* [한국어] p 상수 — 상위 enum 역할 참고 */
			if (!p) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
				ret = -ENOMEM;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			if (strlen(p) < 11 || strncmp(p, "DHHC-1:", 7)) {	/* [한국어] 메모리/문자열 연산 */
				pr_err("Invalid DH-CHAP secret %s\n", p);	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			kfree(opts->dhchap_ctrl_secret);	/* [한국어] 동적 메모리 해제 */
			opts->dhchap_ctrl_secret = p;	/* [한국어] p — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_TLS:	/* [한국어] 다중 분기 케이스 */
			if (!IS_ENABLED(CONFIG_NVME_TCP_TLS)) {	/* [한국어] Kconfig 게이트 */
				pr_err("TLS is not supported\n");	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			opts->tls = true;	/* [한국어] true — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		case NVMF_OPT_CONCAT:	/* [한국어] 다중 분기 케이스 */
			if (!IS_ENABLED(CONFIG_NVME_TCP_TLS)) {	/* [한국어] Kconfig 게이트 */
				pr_err("TLS is not supported\n");	/* [한국어] 진단 로그 */
				ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
			}
			opts->concat = true;	/* [한국어] true — 함수/구조 문맥의 상태 */
			break;	/* [한국어] 루프/스위치 종료 */
		default:	/* [한국어] 예약/미지 값 방어 */
			pr_warn("unknown parameter or missing value '%s' in ctrl creation request\n",	/* [한국어] 진단 로그 */
				p);
			ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
		}
	}

	if (opts->discovery_nqn) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		opts->nr_io_queues = 0;	/* [한국어] 디스커버리는 Admin 큐만 — I/O 큐 불필요 */
		opts->nr_write_queues = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		opts->nr_poll_queues = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		opts->duplicate_connect = true;	/* [한국어] 다중 디스커버리 세션 허용 */
	} else {	/* [한국어] 대안 경로 */
		if (!opts->kato)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			opts->kato = NVME_DEFAULT_KATO;	/* [한국어] 일반 컨트롤러 기본 keep-alive */
	}
	if (ctrl_loss_tmo < 0) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		opts->max_reconnects = -1;	/* [한국어] 음수 loss tmo = 무한 재연결 */
	} else {	/* [한국어] 대안 경로 */
		opts->max_reconnects = DIV_ROUND_UP(ctrl_loss_tmo,	/* [한국어] 올림 환산 */
						opts->reconnect_delay);	/* [한국어] 총 허용 시간을 재시도 횟수로 환산 */
		if (ctrl_loss_tmo < opts->fast_io_fail_tmo)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			pr_warn("failfast tmo (%d) larger than controller loss tmo (%d)\n",	/* [한국어] 진단 로그 */
				opts->fast_io_fail_tmo, ctrl_loss_tmo);	/* [한국어] fail-fast 가 loss 보다 크면 설정 모순 경고 */
	}
	if (opts->concat) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		if (opts->tls) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			pr_err("Secure concatenation over TLS is not supported\n");	/* [한국어] concat 과 사전 TLS 동시 사용 금지 */
			ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
		}
		if (opts->tls_key) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			pr_err("Cannot specify a TLS key for secure concatenation\n");	/* [한국어] concat 은 DH-CHAP 으로 PSK 유도 — 사전 키 불필요 */
			ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
		}
		if (!opts->dhchap_secret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			pr_err("Need to enable DH-CHAP for secure concatenation\n");	/* [한국어] concat 전제: 호스트 DH-CHAP 시크릿 */
			ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
			goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
		}
	}

	opts->host = nvmf_host_add(hostnqn, &hostid);	/* [한국어] 파싱된 NQN/ID 로 host 객체 확보 */
	if (IS_ERR(opts->host)) {	/* [한국어] 에러 포인터 규약 */
		ret = PTR_ERR(opts->host);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		opts->host = NULL;	/* [한국어] free_options 가 안전한 NULL put 하도록 */
		goto out;	/* [한국어] out — 함수/구조 문맥의 상태 */
	}

out:
	kfree(options);	/* [한국어] strsep 사본 해제 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvmf_set_io_queues - 협상된 총 I/O 큐 수를 DEFAULT/READ/POLL 타입별로 배분
 *
 * write_queues 요청이 있고 여유가 있으면 READ 와 DEFAULT(write) 분리.
 * 아니면 DEFAULT 공유. 남는 큐를 POLL 에 할당.
 * 호출: 트랜스포트가 실제 생성 가능 큐 수를 안 뒤 map 직전.
 */
void nvmf_set_io_queues(struct nvmf_ctrl_options *opts, u32 nr_io_queues,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
			u32 io_queues[HCTX_MAX_TYPES])	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	if (opts->nr_write_queues && opts->nr_io_queues < nr_io_queues) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/*
		 * separate read/write queues
		 * hand out dedicated default queues only after we have
		 * sufficient read queues.
		 */
		/* [한국어] 읽기 전용 큐를 먼저 확보한 뒤 나머지를 write(DEFAULT) 에 */
		io_queues[HCTX_TYPE_READ] = opts->nr_io_queues;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		nr_io_queues -= io_queues[HCTX_TYPE_READ];	/* [한국어] io_queues — 함수/구조 문맥의 상태 */
		io_queues[HCTX_TYPE_DEFAULT] =
			min(opts->nr_write_queues, nr_io_queues);
		nr_io_queues -= io_queues[HCTX_TYPE_DEFAULT];	/* [한국어] io_queues — 함수/구조 문맥의 상태 */
	} else {	/* [한국어] 대안 경로 */
		/*
		 * shared read/write queues
		 * either no write queues were requested, or we don't have
		 * sufficient queue count to have dedicated default queues.
		 */
		/* [한국어] read/write 공유 DEFAULT 맵 */
		io_queues[HCTX_TYPE_DEFAULT] =
			min(opts->nr_io_queues, nr_io_queues);
		nr_io_queues -= io_queues[HCTX_TYPE_DEFAULT];	/* [한국어] io_queues — 함수/구조 문맥의 상태 */
	}

	if (opts->nr_poll_queues && nr_io_queues) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* map dedicated poll queues only if we have queues left */
		io_queues[HCTX_TYPE_POLL] =
			min(opts->nr_poll_queues, nr_io_queues);	/* [한국어] 잔여 큐를 폴링 hctx 에 */
	}
}
EXPORT_SYMBOL_GPL(nvmf_set_io_queues);	/* [한국어] Fabrics 공통 라이브러리 */

/*
 * [한국어]
 * nvmf_map_queues - blk-mq tagset map[] 에 큐 오프셋·개수 설정 후 CPU 매핑
 *
 * 분리 모드: DEFAULT(write) 먼저, READ 가 그 다음 오프셋.
 * 공유 모드: DEFAULT 와 READ 가 동일 큐 집합. POLL 은 맨 뒤 오프셋.
 */
void nvmf_map_queues(struct blk_mq_tag_set *set, struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		     u32 io_queues[HCTX_MAX_TYPES])	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct nvmf_ctrl_options *opts = ctrl->opts;	/* [한국어] Fabrics 공통 라이브러리 */

	if (opts->nr_write_queues && io_queues[HCTX_TYPE_READ]) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* separate read/write queues */
		set->map[HCTX_TYPE_DEFAULT].nr_queues =
			io_queues[HCTX_TYPE_DEFAULT];	/* [한국어] write 쪽 DEFAULT 맵 큐 수 */
		set->map[HCTX_TYPE_DEFAULT].queue_offset = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		set->map[HCTX_TYPE_READ].nr_queues =
			io_queues[HCTX_TYPE_READ];
		set->map[HCTX_TYPE_READ].queue_offset =
			io_queues[HCTX_TYPE_DEFAULT];	/* [한국어] read 큐는 write 큐 뒤에 배치 */
	} else {	/* [한국어] 대안 경로 */
		/* shared read/write queues */
		set->map[HCTX_TYPE_DEFAULT].nr_queues =
			io_queues[HCTX_TYPE_DEFAULT];
		set->map[HCTX_TYPE_DEFAULT].queue_offset = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		set->map[HCTX_TYPE_READ].nr_queues =
			io_queues[HCTX_TYPE_DEFAULT];	/* [한국어] 공유: READ 맵도 동일 큐 집합 */
		set->map[HCTX_TYPE_READ].queue_offset = 0;	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
	}

	blk_mq_map_queues(&set->map[HCTX_TYPE_DEFAULT]);	/* [한국어] CPU→hctx 기본 매핑 */
	blk_mq_map_queues(&set->map[HCTX_TYPE_READ]);	/* [한국어] blk-mq/큐 계층 API */
	if (opts->nr_poll_queues && io_queues[HCTX_TYPE_POLL]) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		/* map dedicated poll queues only if we have queues left */
		set->map[HCTX_TYPE_POLL].nr_queues = io_queues[HCTX_TYPE_POLL];	/* [한국어] io_queues — 함수/구조 문맥의 상태 */
		set->map[HCTX_TYPE_POLL].queue_offset =
			io_queues[HCTX_TYPE_DEFAULT] +
			io_queues[HCTX_TYPE_READ];	/* [한국어] poll 큐는 default+read 다음 오프셋 */
		blk_mq_map_queues(&set->map[HCTX_TYPE_POLL]);	/* [한국어] blk-mq/큐 계층 API */
	}

	dev_info(ctrl->device,	/* [한국어] 진단 로그 */
		"mapped %d/%d/%d default/read/poll queues.\n",
		io_queues[HCTX_TYPE_DEFAULT],
		io_queues[HCTX_TYPE_READ],
		io_queues[HCTX_TYPE_POLL]);	/* [한국어] 운영자 가시 큐 매핑 요약 */
}
EXPORT_SYMBOL_GPL(nvmf_map_queues);	/* [한국어] Fabrics 공통 라이브러리 */

/*
 * [한국어]
 * nvmf_check_required_opts - mask 에 필수 옵션 비트가 모두 있는지 검증
 *
 * 누락 시 패턴 문자열을 경고로 출력해 nvme-cli 사용자가 고치기 쉽게 함.
 */
static int nvmf_check_required_opts(struct nvmf_ctrl_options *opts,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned int required_opts)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	if ((opts->mask & required_opts) != required_opts) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		unsigned int i;	/* [한국어] i — 함수/구조 문맥의 상태 */

		for (i = 0; i < ARRAY_SIZE(opt_tokens); i++) {	/* [한국어] 정적 배열 크기 */
			if ((opt_tokens[i].token & required_opts) &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			    !(opt_tokens[i].token & opts->mask)) {
				pr_warn("missing parameter '%s'\n",	/* [한국어] 진단 로그 */
					opt_tokens[i].pattern);	/* [한국어] 누락 필수 옵션 이름 힌트 */
			}
		}

		return -EINVAL;	/* [한국어] 인자·프로토콜 불변식 위반 */
	}

	return 0;	/* [한국어] 성공/no-op 완료 */
}

/*
 * [한국어]
 * nvmf_ip_options_match - 기존 컨트롤러와 새 옵션이 동일 엔드포인트인지 비교
 *
 * 중복 connect 거부/재사용 판단. 베이스(NQN/host) + traddr/trsvcid +
 * host_traddr/iface 대칭성(한쪽만 지정되면 불일치).
 */
bool nvmf_ip_options_match(struct nvme_ctrl *ctrl,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		struct nvmf_ctrl_options *opts)
{
	if (!nvmf_ctlr_matches_baseopts(ctrl, opts) ||	/* [한국어] Fabrics 공통 라이브러리 */
	    strcmp(opts->traddr, ctrl->opts->traddr) ||	/* [한국어] 메모리/문자열 연산 */
	    strcmp(opts->trsvcid, ctrl->opts->trsvcid))	/* [한국어] 메모리/문자열 연산 */
		return false;	/* [한국어] 서브시스템/host/원격 주소 불일치 */

	/*
	 * Checking the local address or host interfaces is rough.
	 *
	 * In most cases, none is specified and the host port or
	 * host interface is selected by the stack.
	 *
	 * Assume no match if:
	 * -  local address or host interface is specified and address
	 *    or host interface is not the same
	 * -  local address or host interface is not specified but
	 *    remote is, or vice versa (admin using specific
	 *    host_traddr/host_iface when it matters).
	 */
	/* [한국어] 로컬 바인드 옵션은 "둘 다 없음" 또는 "둘 다 같고 동일 값" 만 매치 */
	if ((opts->mask & NVMF_OPT_HOST_TRADDR) &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
	    (ctrl->opts->mask & NVMF_OPT_HOST_TRADDR)) {
		if (strcmp(opts->host_traddr, ctrl->opts->host_traddr))	/* [한국어] 메모리/문자열 연산 */
			return false;	/* [한국어] 로컬 주소 문자열 불일치 */
	} else if ((opts->mask & NVMF_OPT_HOST_TRADDR) ||	/* [한국어] 대안 조건 경로 */
		   (ctrl->opts->mask & NVMF_OPT_HOST_TRADDR)) {
		return false;	/* [한국어] 한쪽만 host_traddr 지정 — 다른 바인딩으로 간주 */
	}

	if ((opts->mask & NVMF_OPT_HOST_IFACE) &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
	    (ctrl->opts->mask & NVMF_OPT_HOST_IFACE)) {
		if (strcmp(opts->host_iface, ctrl->opts->host_iface))	/* [한국어] 메모리/문자열 연산 */
			return false;	/* [한국어] 음성 판정 */
	} else if ((opts->mask & NVMF_OPT_HOST_IFACE) ||	/* [한국어] 대안 조건 경로 */
		   (ctrl->opts->mask & NVMF_OPT_HOST_IFACE)) {
		return false;	/* [한국어] host_iface 대칭성 위반 */
	}

	return true;	/* [한국어] 동일 연결 엔드포인트 */
}
EXPORT_SYMBOL_GPL(nvmf_ip_options_match);	/* [한국어] Fabrics 공통 라이브러리 */

/*
 * [한국어]
 * nvmf_check_allowed_opts - 공통+트랜스포트 허용 목록 밖 옵션 거부
 */
static int nvmf_check_allowed_opts(struct nvmf_ctrl_options *opts,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		unsigned int allowed_opts)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	if (opts->mask & ~allowed_opts) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		unsigned int i;	/* [한국어] i — 함수/구조 문맥의 상태 */

		for (i = 0; i < ARRAY_SIZE(opt_tokens); i++) {	/* [한국어] 정적 배열 크기 */
			if ((opt_tokens[i].token & opts->mask) &&	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			    (opt_tokens[i].token & ~allowed_opts)) {
				pr_warn("invalid parameter '%s'\n",	/* [한국어] 진단 로그 */
					opt_tokens[i].pattern);	/* [한국어] 이 트랜스포트에 부적합한 옵션 */
			}
		}

		return -EINVAL;	/* [한국어] 인자·프로토콜 불변식 위반 */
	}

	return 0;	/* [한국어] 성공/no-op 완료 */
}

/*
 * [한국어]
 * nvmf_free_options - connect 옵션 소유 자원 전부 해제
 *
 * host kref, 키링 참조, 문자열, 민감 시크릿(kfree_sensitive).
 */
void nvmf_free_options(struct nvmf_ctrl_options *opts)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	nvmf_host_put(opts->host);	/* [한국어] host 참조 반환 */
	key_put(opts->keyring);	/* [한국어] 키링 참조 (NULL-safe key_put) */
	key_put(opts->tls_key);	/* [한국어] TLS PSK 키 참조 반환 */
	kfree(opts->transport);	/* [한국어] "tcp"/"rdma"/"fc" 문자열 */
	kfree(opts->traddr);	/* [한국어] 원격 주소 문자열 */
	kfree(opts->trsvcid);	/* [한국어] 서비스 ID/포트 문자열 */
	kfree(opts->subsysnqn);	/* [한국어] 대상 서브시스템 NQN */
	kfree(opts->host_traddr);	/* [한국어] 로컬 바인드 주소 */
	kfree(opts->host_iface);	/* [한국어] 로컬 인터페이스 이름 */
	kfree_sensitive(opts->dhchap_secret);	/* [한국어] 시크릿 잔존 방지 해제 */
	kfree_sensitive(opts->dhchap_ctrl_secret);	/* [한국어] 컨트롤러 시크릿 안전 해제 */
	kfree(opts);	/* [한국어] 옵션 구조체 자체 */
}
EXPORT_SYMBOL_GPL(nvmf_free_options);	/* [한국어] Fabrics 공통 라이브러리 */

/* [한국어] 모든 트랜스포트 공통 필수: transport= 와 nqn= */
#define NVMF_REQUIRED_OPTS	(NVMF_OPT_TRANSPORT | NVMF_OPT_NQN)	/* [한국어] NVMF_REQUIRED_OPTS 매크로 — 상위 섹션 계약 참고 */
/* [한국어] 공통 허용 옵션 — 트랜스포트별 allowed/required 와 OR */
#define NVMF_ALLOWED_OPTS	(NVMF_OPT_QUEUE_SIZE | NVMF_OPT_NR_IO_QUEUES | \
				 NVMF_OPT_KATO | NVMF_OPT_HOSTNQN | \
				 NVMF_OPT_HOST_ID | NVMF_OPT_DUP_CONNECT |\
				 NVMF_OPT_DISABLE_SQFLOW | NVMF_OPT_DISCOVERY |\
				 NVMF_OPT_FAIL_FAST_TMO | NVMF_OPT_DHCHAP_SECRET |\
				 NVMF_OPT_DHCHAP_CTRL_SECRET)	/* [한국어] 인자/선언 연속행 */

/*
 * [한국어]
 * nvmf_create_ctrl - 옵션 문자열로 컨트롤러 생성 전체 파이프라인
 *
 * parse → request_module(nvme-$transport) → 필수 검사 → lookup ops →
 * module_get → 트랜스포트 필수/허용 검사 → ops->create_ctrl.
 * 성공 시 opts 소유권은 컨트롤러가 가져감(실패 시 free_options).
 */
static struct nvme_ctrl *
nvmf_create_ctrl(struct device *dev, const char *buf)	/* [한국어] class ctl 장치 + connect 문자열 */
{
	struct nvmf_ctrl_options *opts;	/* [한국어] 파싱 결과 소유 — 성공 시 ctrl 이 인수 */
	struct nvmf_transport_ops *ops;	/* [한국어] 등록된 tcp/rdma/fc vtable */
	struct nvme_ctrl *ctrl;	/* [한국어] 트랜스포트 create_ctrl 결과 */
	int ret;	/* [한국어] 파싱·검사·생성 errno */

	opts = kzalloc_obj(*opts);	/* [한국어] 옵션 구조체 제로 할당 */
	if (!opts)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return ERR_PTR(-ENOMEM);	/* [한국어] 옵션 메모리 부족 */

	ret = nvmf_parse_options(opts, buf);	/* [한국어] write 버퍼 → opts 필드 */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto out_free_opts;	/* [한국어] 토큰 파싱 실패 — 부분 할당 정리 */


	request_module("nvme-%s", opts->transport);	/* [한국어] 예: nvme-tcp 자동 로드 시도 */

	/*
	 * Check the generic options first as we need a valid transport for
	 * the lookup below.  Then clear the generic flags so that transport
	 * drivers don't have to care about them.
	 */
	/* [한국어] 공통 필수 먼저 확인 후 mask 에서 제거 — 트랜스포트는 자기 옵션만 봄 */
	ret = nvmf_check_required_opts(opts, NVMF_REQUIRED_OPTS);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto out_free_opts;	/* [한국어] out_free_opts — 함수/구조 문맥의 상태 */
	opts->mask &= ~NVMF_REQUIRED_OPTS;	/* [한국어] transport/nqn 비트 소비 처리 */

	down_read(&nvmf_transports_rwsem);	/* [한국어] 조회 중 등록 안정성 */
	ops = nvmf_lookup_transport(opts);	/* [한국어] ops 상수 — 상위 enum 역할 참고 */
	if (!ops) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		pr_info("no handler found for transport %s.\n",	/* [한국어] 진단 로그 */
			opts->transport);	/* [한국어] 모듈 미로드 또는 이름 오타 */
		ret = -EINVAL;	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
	}

	if (!try_module_get(ops->module)) {	/* [한국어] 모듈 수명/로드 */
		ret = -EBUSY;	/* [한국어] 언로드 경합 — 모듈 핀 실패 */
		goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
	}
	up_read(&nvmf_transports_rwsem);	/* [한국어] Fabrics 공통 라이브러리 */

	ret = nvmf_check_required_opts(opts, ops->required_opts);	/* [한국어] 트랜스포트 고유 필수 (예: traddr) */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto out_module_put;	/* [한국어] out_module_put — 함수/구조 문맥의 상태 */
	ret = nvmf_check_allowed_opts(opts, NVMF_ALLOWED_OPTS |	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
				ops->allowed_opts | ops->required_opts);	/* [한국어] 공통∪트랜스포트 허용 집합 밖 거부 */
	if (ret)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		goto out_module_put;	/* [한국어] out_module_put — 함수/구조 문맥의 상태 */

	ctrl = ops->create_ctrl(dev, opts);	/* [한국어] tcp/rdma/fc 가 실제 컨트롤러 인스턴스 생성 */
	if (IS_ERR(ctrl)) {	/* [한국어] 에러 포인터 규약 */
		ret = PTR_ERR(ctrl);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		goto out_module_put;	/* [한국어] out_module_put — 함수/구조 문맥의 상태 */
	}

	module_put(ops->module);	/* [한국어] create 동안만 핀 — 이후 ctrl 이 모듈 수명 보유 */
	return ctrl;	/* [한국어] 호출 결과 반환 */

out_module_put:
	module_put(ops->module);	/* [한국어] 모듈 수명/로드 */
	goto out_free_opts;	/* [한국어] out_free_opts — 함수/구조 문맥의 상태 */
out_unlock:
	up_read(&nvmf_transports_rwsem);	/* [한국어] Fabrics 공통 라이브러리 */
out_free_opts:
	nvmf_free_options(opts);	/* [한국어] 실패 시 옵션 전부 해제 */
	return ERR_PTR(ret);	/* [한국어] 에러 포인터 규약 */
}

static const struct class nvmf_class = {	/* [한국어] Fabrics 공통 라이브러리 */
	.name = "nvme-fabrics",	/* [한국어] sysfs class: /sys/class/nvme-fabrics */
};

static struct device *nvmf_device;	/* [한국어] class 하위 ctl 장치 — create_ctrl 부모 */
static DEFINE_MUTEX(nvmf_dev_mutex);	/* [한국어] misc fd 당 한 컨트롤러 write/read 직렬화 */

/*
 * [한국어]
 * nvmf_dev_write - /dev/nvme-fabrics 에 connect 문자열 write → 컨트롤러 생성
 *
 * 한 fd 에 한 번만 성공 write (seq_file->private 에 ctrl 보관).
 * 성공 시 count 반환, 실패 시 errno. nvme-cli connect 의 커널 진입점.
 */
static ssize_t nvmf_dev_write(struct file *file, const char __user *ubuf,	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
		size_t count, loff_t *pos)	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */
{
	struct seq_file *seq_file = file->private_data;	/* [한국어] single_open 이 만든 seq_file */
	struct nvme_ctrl *ctrl;	/* [한국어] ctrl — 함수/구조 문맥의 상태 */
	const char *buf;	/* [한국어] buf — 함수/구조 문맥의 상태 */
	int ret = 0;	/* [한국어] 지역/멤버 상태 — 상위 함수·구조 아키텍처 참고 */

	if (count > PAGE_SIZE)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		return -ENOMEM;	/* [한국어] 옵션 문자열 상한 — 페이지 단위 */

	buf = memdup_user_nul(ubuf, count);	/* [한국어] 유저 버퍼 커널 복사 + NUL 종단 */
	if (IS_ERR(buf))	/* [한국어] 에러 포인터 규약 */
		return PTR_ERR(buf);	/* [한국어] 에러 포인터 규약 */

	mutex_lock(&nvmf_dev_mutex);	/* [한국어] fd 당 단일 컨트롤러 불변식 */
	if (seq_file->private) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		ret = -EINVAL;	/* [한국어] 이미 connect 된 fd 에 재 write 금지 */
		goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
	}

	ctrl = nvmf_create_ctrl(nvmf_device, buf);	/* [한국어] 파싱·트랜스포트 create 전체 */
	if (IS_ERR(ctrl)) {	/* [한국어] 에러 포인터 규약 */
		ret = PTR_ERR(ctrl);	/* [한국어] create 실패 errno */
		goto out_unlock;	/* [한국어] 옵션/모듈 정리 후 반환 */
	}

	seq_file->private = ctrl;	/* [한국어] read 시 instance/cntlid, release 시 put_ctrl */

out_unlock:
	mutex_unlock(&nvmf_dev_mutex);	/* [한국어] fd 직렬화 해제 */
	kfree(buf);	/* [한국어] 유저 복사 옵션 문자열 해제 */
	return ret ? ret : count;	/* [한국어] 성공 시 소비 바이트, 실패 시 errno */
}

/*
 * [한국어]
 * __nvmf_concat_opt_tokens - connect 전 read 응답: 지원 옵션 패턴 나열
 *
 * instance=-1,cntlid=-1 로 "아직 컨트롤러 없음" 신호 후 토큰 패턴 덤프.
 */
static void __nvmf_concat_opt_tokens(struct seq_file *seq_file)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	const struct match_token *tok;	/* [한국어] tok — 함수/구조 문맥의 상태 */
	int idx;	/* [한국어] idx — 함수/구조 문맥의 상태 */

	/*
	 * Add dummy entries for instance and cntlid to
	 * signal an invalid/non-existing controller
	 */
	seq_puts(seq_file, "instance=-1,cntlid=-1");	/* [한국어] 미연결 센티널 */
	for (idx = 0; idx < ARRAY_SIZE(opt_tokens); idx++) {	/* [한국어] 정적 배열 크기 */
		tok = &opt_tokens[idx];	/* [한국어] tok 상수 — 상위 enum 역할 참고 */
		if (tok->token == NVMF_OPT_ERR)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
			continue;	/* [한국어] ERR 센티널 패턴 제외 */
		seq_putc(seq_file, ',');	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
		seq_puts(seq_file, tok->pattern);	/* [한국어] 사용자 공간 도움용 옵션 문법 */
	}
	seq_putc(seq_file, '\n');	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
}

/*
 * [한국어]
 * nvmf_dev_show - misc read: 연결 후면 instance/cntlid, 전이면 옵션 목록
 */
static int nvmf_dev_show(struct seq_file *seq_file, void *private)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct nvme_ctrl *ctrl;	/* [한국어] ctrl — 함수/구조 문맥의 상태 */

	mutex_lock(&nvmf_dev_mutex);	/* [한국어] Fabrics 공통 라이브러리 */
	ctrl = seq_file->private;	/* [한국어] ctrl 상수 — 상위 enum 역할 참고 */
	if (!ctrl) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		__nvmf_concat_opt_tokens(seq_file);	/* [한국어] connect 전 도움 출력 */
		goto out_unlock;	/* [한국어] out_unlock — 함수/구조 문맥의 상태 */
	}

	seq_printf(seq_file, "instance=%d,cntlid=%d\n",	/* [한국어] 실행 단계 — 주변 함수 한국어 블록과 함께 해석 */
			ctrl->instance, ctrl->cntlid);	/* [한국어] 생성된 컨트롤러 식별자 — nvme-cli 가 후속 조작에 사용 */

out_unlock:
	mutex_unlock(&nvmf_dev_mutex);	/* [한국어] Fabrics 공통 라이브러리 */
	return 0;	/* [한국어] 성공/no-op 완료 */
}

/*
 * [한국어]
 * nvmf_dev_open - misc open: single_open 으로 show 콜백 연결
 */
static int nvmf_dev_open(struct inode *inode, struct file *file)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	/*
	 * The miscdevice code initializes file->private_data, but doesn't
	 * make use of it later.
	 */
	file->private_data = NULL;	/* [한국어] misc 가 넣은 값을 single_open 이 덮어쓰도록 클리어 */
	return single_open(file, nvmf_dev_show, NULL);	/* [한국어] seq_file 인프라 설치 */
}

/*
 * [한국어]
 * nvmf_dev_release - fd close 시 생성했던 컨트롤러 참조 반환
 */
static int nvmf_dev_release(struct inode *inode, struct file *file)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	struct seq_file *seq_file = file->private_data;	/* [한국어] open 시 설치한 seq_file */
	struct nvme_ctrl *ctrl = seq_file->private;	/* [한국어] write 가 저장한 컨트롤러 또는 NULL */

	if (ctrl)	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		nvme_put_ctrl(ctrl);	/* [한국어] write 가 잡은 ctrl 참조 균형 */
	return single_release(inode, file);	/* [한국어] seq_file 정리 */
}

static const struct file_operations nvmf_dev_fops = {	/* [한국어] Fabrics 공통 라이브러리 */
	.owner		= THIS_MODULE,	/* [한국어] 모듈 수명 */
	.write		= nvmf_dev_write,	/* [한국어] connect 문자열 수신 */
	.read		= seq_read,	/* [한국어] seq_file 표준 read */
	.open		= nvmf_dev_open,
	.release	= nvmf_dev_release,
};

static struct miscdevice nvmf_misc = {	/* [한국어] Fabrics 공통 라이브러리 */
	.minor		= MISC_DYNAMIC_MINOR,	/* [한국어] 동적 minor 할당 */
	.name           = "nvme-fabrics",	/* [한국어] /dev/nvme-fabrics */
	.fops		= &nvmf_dev_fops,
};

/*
 * [한국어]
 * nvmf_init - 기본 host·class·ctl device·misc 등록
 *
 * 모듈 로드 시 사용자 공간 connect 경로를 연다.
 */
static int __init nvmf_init(void)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	int ret;	/* [한국어] ret — 함수/구조 문맥의 상태 */

	nvmf_default_host = nvmf_host_default();	/* [한국어] 기본 Host NQN/ID 생성 */
	if (!nvmf_default_host)	/* [한국어] Fabrics 공통 라이브러리 */
		return -ENOMEM;	/* [한국어] 할당 실패 전파 */

	ret = class_register(&nvmf_class);	/* [한국어] sysfs class 등록 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		pr_err("couldn't register class nvme-fabrics\n");	/* [한국어] 진단 로그 */
		goto out_free_host;	/* [한국어] out_free_host — 함수/구조 문맥의 상태 */
	}

	nvmf_device =	/* [한국어] nvmf_device 상수 — 상위 enum 역할 참고 */
		device_create(&nvmf_class, NULL, MKDEV(0, 0), NULL, "ctl");	/* [한국어] create_ctrl 부모 장치 */
	if (IS_ERR(nvmf_device)) {	/* [한국어] Fabrics 공통 라이브러리 */
		pr_err("couldn't create nvme-fabrics device!\n");	/* [한국어] 진단 로그 */
		ret = PTR_ERR(nvmf_device);	/* [한국어] ret 상수 — 상위 enum 역할 참고 */
		goto out_destroy_class;	/* [한국어] out_destroy_class — 함수/구조 문맥의 상태 */
	}

	ret = misc_register(&nvmf_misc);	/* [한국어] /dev/nvme-fabrics 노드 */
	if (ret) {	/* [한국어] 아키텍처 가드 — 함수 헤드 문맥 참고 */
		pr_err("couldn't register misc device: %d\n", ret);	/* [한국어] 진단 로그 */
		goto out_destroy_device;	/* [한국어] out_destroy_device — 함수/구조 문맥의 상태 */
	}

	return 0;	/* [한국어] 성공/no-op 완료 */

out_destroy_device:
	device_destroy(&nvmf_class, MKDEV(0, 0));	/* [한국어] Fabrics 공통 라이브러리 */
out_destroy_class:
	class_unregister(&nvmf_class);	/* [한국어] Fabrics 공통 라이브러리 */
out_free_host:
	nvmf_host_put(nvmf_default_host);	/* [한국어] Fabrics 공통 라이브러리 */
	return ret;	/* [한국어] 결과 코드 전파 */
}

/*
 * [한국어]
 * nvmf_exit - misc/class/host 역순 해제 + 캡슐 구조체 크기 컴파일 타임 검증
 *
 * BUILD_BUG_ON 은 스펙 고정 레이아웃이 깨지면 빌드 실패로 조기 발견.
 */
static void __exit nvmf_exit(void)	/* [한국어] 함수 시그니처 — 직전 한국어 함수 블록 계약 */
{
	misc_deregister(&nvmf_misc);	/* [한국어] /dev/nvme-fabrics 제거 */
	device_destroy(&nvmf_class, MKDEV(0, 0));	/* [한국어] class ctl 장치 제거 */
	class_unregister(&nvmf_class);	/* [한국어] nvme-fabrics class 해제 */
	nvmf_host_put(nvmf_default_host);	/* [한국어] 기본 host NQN 참조 반환 */

	BUILD_BUG_ON(sizeof(struct nvmf_common_command) != 64);	/* [한국어] Fabrics 공통 SQE 64B */
	BUILD_BUG_ON(sizeof(struct nvmf_connect_command) != 64);	/* [한국어] Connect SQE 64B 고정 */
	BUILD_BUG_ON(sizeof(struct nvmf_property_get_command) != 64);	/* [한국어] Property Get SQE */
	BUILD_BUG_ON(sizeof(struct nvmf_property_set_command) != 64);	/* [한국어] Property Set SQE */
	BUILD_BUG_ON(sizeof(struct nvmf_auth_send_command) != 64);	/* [한국어] Auth Send SQE */
	BUILD_BUG_ON(sizeof(struct nvmf_auth_receive_command) != 64);	/* [한국어] Auth Receive SQE */
	BUILD_BUG_ON(sizeof(struct nvmf_connect_data) != 1024);	/* [한국어] Connect data 페이지 크기 */
	BUILD_BUG_ON(sizeof(struct nvmf_auth_dhchap_negotiate_data) != 8);	/* [한국어] DH-CHAP 메시지 고정 헤더 크기 */
	BUILD_BUG_ON(sizeof(struct nvmf_auth_dhchap_challenge_data) != 16);	/* [한국어] Challenge 고정 헤더 */
	BUILD_BUG_ON(sizeof(struct nvmf_auth_dhchap_reply_data) != 16);	/* [한국어] Reply 고정 헤더 */
	BUILD_BUG_ON(sizeof(struct nvmf_auth_dhchap_success1_data) != 16);	/* [한국어] Success1 고정 헤더 */
	BUILD_BUG_ON(sizeof(struct nvmf_auth_dhchap_success2_data) != 16);	/* [한국어] Success2 고정 헤더 */
}

MODULE_LICENSE("GPL v2");	/* [한국어] GPL v2 라이선스 표기 */
MODULE_DESCRIPTION("NVMe host fabrics library");	/* [한국어] modinfo 설명 */

module_init(nvmf_init);	/* [한국어] 모듈 로드 진입 */
module_exit(nvmf_exit);	/* [한국어] 모듈 언로드 진입 */
