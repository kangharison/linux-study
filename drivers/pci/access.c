// SPDX-License-Identifier: GPL-2.0
/* [한국어] struct pci_dev / struct pci_bus / struct pci_ops 정의와,
 * 이 파일이 구현하는 pci_read_config_* 계열의 원형이 들어 있다.
 * PCI_EXP_* (PCI Express Capability 오프셋)와 PCIBIOS_* (반환 코드)도 여기서 온다. */
#include <linux/pci.h>
/* [한국어] EXPORT_SYMBOL / EXPORT_SYMBOL_GPL 매크로. 이 파일은 30개 심볼을
 * 내보내므로 반드시 필요하다 — config 접근은 모듈로 빌드된 드라이버가
 * 가장 먼저 쓰는 기능이다. */
#include <linux/module.h>
/* [한국어] 이 파일 자체는 kmalloc 계열을 한 번도 부르지 않는다(주석을 제거하고
 * 검색해 확인). 다른 헤더가 간접적으로 필요로 하거나, 과거에 쓰던 코드가
 * 사라진 뒤 남은 include 로 보인다. 원본 그대로 두되, "여기서 메모리를 할당한다"
 * 는 오해를 남기지 않기 위해 사실만 적어 둔다. */
#include <linux/slab.h>
/* [한국어] 마찬가지로 이 파일에는 resource/IORESOURCE_* 사용이 없다.
 * BAR 자원 요청은 이 파일이 아니라 setup-res.c 와 pci.c 가 담당한다. */
#include <linux/ioport.h>
/* [한국어] DECLARE_WAIT_QUEUE_HEAD 와 wait_event. config 접근이 차단된 동안
 * 태스크를 재우는 pci_wait_cfg() 가 이 둘을 쓴다 — 이 파일에서 실제로
 * 사용되는 것이 확인된 헤더다. */
#include <linux/wait.h>

/* [한국어] PCI 서브시스템 내부 전용 헤더(drivers/pci/pci.h). 바깥에 노출하지
 * 않는 선언들이 들어 있다 — 이 파일에서는 pci_lock 의 extern 선언,
 * pcie_downstream_port(), pci_dev_is_disconnected() 등이 여기서 온다.
 * include/linux/pci.h 와 이름은 같지만 완전히 다른 파일이며, 따옴표 include 라
 * 컴파일러가 이 소스와 같은 디렉터리에서 먼저 찾는다. */
#include "pci.h"

/*
 * [한국어 설명] PCI config space 접근의 관문 (drivers/pci/access.c)
 *
 * === 파일의 역할 ===
 * PCI 장치의 설정 공간(configuration space)을 읽고 쓰는 모든 경로가 이 파일을
 * 지난다. 커널 어디에서든 pci_read_config_word() 같은 함수를 부르면 여기로 와서,
 * 유효성 검사를 거친 뒤 그 버스를 담당하는 호스트 컨트롤러의 콜백
 * (bus->ops->read / ->write)으로 내려간다. 실제로 하드웨어에 접근하는 방법은
 * 아키텍처와 컨트롤러마다 다르므로(x86 의 0xCF8/0xCFC 포트, ECAM MMIO,
 * 각 SoC 의 전용 창 등) 이 파일은 그 차이를 감추는 얇은 중간층이다.
 * 아울러 config 접근을 일시적으로 막아 두는 장치(pci_cfg_access_lock)와,
 * PCIe capability 레지스터를 안전하게 다루는 헬퍼도 여기 있다.
 *
 * === 전체 아키텍처에서의 위치 ===
 * 위로는 probe.c(장치 발견), pci.c(활성화·전원), 각 드라이버가 이 파일을 부른다.
 * 아래로는 drivers/pci/controller/ 의 호스트 컨트롤러 드라이버가 등록한
 * struct pci_ops 의 read/write 콜백이 있다. 이 파일은 그 둘을 잇는다.
 *
 * 접근이 실패했을 때의 규약이 중요하다. 응답하지 않는 장치를 읽으면 버스는
 * 모든 비트가 1 인 값(all-ones)을 돌려준다. 그래서 Vendor ID 가 0xFFFF 면
 * "그 자리에 장치가 없다"는 뜻이 되고, probe.c 의 장치 발견과 pci.c 의
 * pci_device_is_present() 가 모두 이 규약 위에 서 있다. NVMe 드라이버가
 * readl(CSTS) == -1 로 컨트롤러 유실을 판정하는 것도 같은 원리다.
 *
 * === 타 모듈과의 연결 ===
 * pci_lock(raw spinlock)이 모든 config 접근을 직렬화한다. raw 를 쓰는 이유는
 * 인터럽트 문맥과 -RT 커널에서도 잠들지 않아야 하기 때문이다.
 * 그 위에 pci_cfg_access_lock() 계열이 얹혀 있는데, 이쪽은 리셋이나 장치 제거처럼
 * "잠시 config 접근을 아예 막아야 하는" 구간에서 쓴다 — block_cfg_access 플래그를
 * 세우고, 그 사이 들어온 접근은 pci_wait_cfg() 에서 대기시킨다.
 * PCIe capability 접근 헬퍼는 pcie_cap_version()/pcie_capability_reg_implemented()
 * 로 "이 장치·이 capability 버전에 그 레지스터가 실제로 존재하는가"를 먼저 따진다.
 * 없는 레지스터를 읽으면 쓰레기가 나오므로, 읽기는 0 을 채우고 쓰기는 조용히
 * 무시하는 방어가 필요하다.
 *
 * === NVMe 드라이버가 실제로 쓰는 것 (drivers/nvme/ 전수 확인) ===
 * 주석을 제거한 drivers/nvme/ 전체를 검색해 확인한 결과, NVMe 드라이버가 이 파일의
 * 함수 중 직접 호출하는 것은 **pci_read_config_word() 하나뿐**이다.
 * 나머지는 전부 간접 경로다 — pci_enable_device_mem(), pci_save_state(),
 * pci_set_master() 같은 상위 API 가 내부적으로 이 파일을 부른다.
 *
 * (이전 주석은 "NVMe 가 pcie_capability_read/write_*, pci_write_config_* 를 써서
 *  MSI-X table 을 설정하고 ASPM·링크 제어를 한다"고 적어 두었으나 그런 직접 호출은
 *  없었다. MSI-X 설정은 msi/ 가, ASPM 은 pcie/aspm.c 가 각각 장치 드라이버를
 *  대신해 수행한다. 위 검증 결과로 대체했다.)
 *
 * === 주요 함수/구조체 요약 ===
 * pci_read_config_{byte,word,dword}()  : 크기별 읽기 진입점. PCI_OP_READ 매크로가
 *   세 벌을 한꺼번에 만들어 낸다. 실패 시 출력값을 all-ones 로 채운다.
 * pci_write_config_{byte,word,dword}() : 쓰기 진입점(PCI_OP_WRITE 로 생성).
 * pci_generic_config_read/write()      : ECAM 처럼 메모리 매핑된 config 공간을
 *   쓰는 컨트롤러들이 그대로 가져다 쓰는 기본 구현.
 * pci_cfg_access_lock/unlock/trylock() : config 접근을 일시 차단·해제한다.
 *   리셋 중에 다른 코드가 레지스터를 읽어 엉뚱한 값을 보는 것을 막는다.
 * pcie_capability_read/write_{word,dword}() : PCIe capability 상대 오프셋으로
 *   접근하는 헬퍼. 존재 여부 검사를 내장해 없는 레지스터를 안전하게 다룬다.
 * pci_lock                              : 위 모든 접근을 직렬화하는 raw spinlock.
 */
/*
 * This interrupt-safe spinlock protects all accesses to PCI
 * configuration space.
 */

/* [한국어] 시스템 전체의 config space 접근을 직렬화하는 단 하나의 락.
 * 장치별/버스별이 아니라 전역인 이유: 0xCF8/0xCFC 방식(x86 레거시)은 주소
 * 레지스터와 데이터 레지스터가 시스템에 한 쌍뿐이라, 애초에 전역 직렬화 말고는
 * 방법이 없다. ECAM 방식에서는 이론상 더 잘게 쪼갤 수 있지만, config 접근이
 * 성능 경로가 아니어서 굳이 복잡하게 만들지 않았다(ECAM 전용 시스템을 위한
 * 우회로가 아래 CONFIG_PCI_LOCKLESS_CONFIG 다).
 *
 * raw_ 접두사가 붙은 이유가 중요하다. PREEMPT_RT 커널에서는 보통의 spinlock 이
 * 잠들 수 있는 뮤텍스로 바뀌는데, config 접근은 인터럽트 문맥에서도 일어나므로
 * 그렇게 되면 안 된다. raw_spinlock 은 -RT 에서도 진짜 스핀락으로 남는다. */
DEFINE_RAW_SPINLOCK(pci_lock);

/*
 * Wrappers for all PCI configuration access functions.  They just check
 * alignment, do locking and call the low-level functions pointed to
 * by pci_dev->ops.
 */

/* [한국어] config 접근의 정렬 검사. 아래 PCI_OP_READ/WRITE 매크로가
 * `if (PCI_##size##_BAD)` 형태로 펼쳐, size 에 byte/word/dword 중 무엇이
 * 들어오느냐에 따라 서로 다른 검사가 되게 하는 토큰 붙이기(##) 기법이다.
 *
 * 왜 정렬을 강제하는가: PCI config 접근은 하드웨어 수준에서 DWORD(4바이트)
 * 단위로 이뤄지고, 그 안에서 어느 바이트를 쓸지는 별도 신호로 고른다.
 * 정렬되지 않은 오프셋은 그 신호로 표현할 수 없어 컨트롤러마다 결과가 달라진다.
 * 그래서 하드웨어에 내려보내기 전에 여기서 거른다. */
#define PCI_byte_BAD 0 /* [한국어] 바이트 접근은 어느 오프셋이든 정렬 문제가 없다 */
#define PCI_word_BAD (pos & 1)  /* [한국어] 2바이트 접근은 짝수 오프셋이어야 한다 */
#define PCI_dword_BAD (pos & 3) /* [한국어] 4바이트 접근은 4의 배수 오프셋이어야 한다 */

/* [한국어] config 접근 직렬화를 켜고 끄는 스위치.
 *
 * CONFIG_PCI_LOCKLESS_CONFIG 는 "이 아키텍처의 config 접근은 하드웨어 수준에서
 * 이미 원자적이다" 라고 선언하는 옵션이다. ECAM 처럼 config space 가 통째로
 * 메모리에 매핑돼 있으면 읽기·쓰기 한 번이 곧 하나의 MMIO 트랜잭션이라,
 * 두 CPU 가 서로 다른 장치를 동시에 건드려도 섞일 여지가 없다. 그런 경우
 * 전역 락은 순수한 병목일 뿐이므로 아예 없앤다.
 *
 * 반대로 x86 레거시의 0xCF8/0xCFC 방식은 "주소를 쓰고 -> 데이터를 읽는" 2단계라
 * 그 사이가 갈라지면 엉뚱한 장치를 읽는다. 그래서 이 옵션을 켤 수 없다.
 *
 * do { (void)(f); } while (0) 형태인 이유가 두 가지 있다.
 *   - (void)(f) 는 flags 변수를 "썼다" 고 표시해, 락이 사라진 빌드에서
 *     "set but not used" 경고가 나지 않게 한다.
 *   - do/while(0) 로 감싸는 것은 매크로를 하나의 문장처럼 만들어,
 *     `if (x) pci_lock_config(f); else ...` 같은 자리에서도 안전하게 하려는
 *     C 매크로의 표준 관용구다.
 *
 * 주의: 이 스위치는 PCI_OP_READ/WRITE 계열(커널 내부 저수준 경로)에만 적용된다.
 * pci_user_read/write_config_* 와 pci_cfg_access_lock 계열은 pci_lock 을 직접
 * 잡으므로 이 옵션과 무관하게 항상 직렬화된다 — 그쪽은 하드웨어 원자성이 아니라
 * block_cfg_access 플래그를 보호하는 것이 목적이기 때문이다. */
#ifdef CONFIG_PCI_LOCKLESS_CONFIG
# define pci_lock_config(f)	do { (void)(f); } while (0)	/* [한국어] 락 없음 */
# define pci_unlock_config(f)	do { (void)(f); } while (0)	/* [한국어] 락 없음 */
#else
# define pci_lock_config(f)	raw_spin_lock_irqsave(&pci_lock, f)	/* [한국어] 인터럽트를 끄고 전역 락 획득 */
# define pci_unlock_config(f)	raw_spin_unlock_irqrestore(&pci_lock, f)	/* [한국어] 락 해제 + 인터럽트 상태 복원 */
#endif

/*
 * [한국어]
 * PCI_OP_READ / PCI_OP_WRITE - config space 접근 함수 6개를 찍어내는 틀
 *
 * @size:  함수 이름과 정렬 검사 매크로에 붙일 토큰 — byte / word / dword 중 하나.
 *         ##(토큰 붙이기) 연산자로 pci_bus_read_config_##size 와
 *         PCI_##size##_BAD 두 곳에 동시에 끼워진다.
 * @type:  호출자에게 돌려줄(또는 받을) C 타입 — u8 / u16 / u32.
 * @len:   버스 드라이버에게 넘길 바이트 수 — 1 / 2 / 4.
 *
 * 읽기 3종 + 쓰기 3종, 도합 6개 함수가 본문이 완전히 같고 폭만 다르다.
 * 그 여섯 벌을 손으로 복사해 두면 락 규칙이나 에러 처리 하나를 고칠 때
 * 여섯 군데를 다 고쳐야 하고, 한 군데를 빠뜨리면 폭에 따라 동작이 달라지는
 * 재현 어려운 버그가 된다. 그래서 틀 하나로 찍어낸다.
 *
 * 여기서 만들어지는 것은 "버스 + devfn" 을 직접 받는 저수준 판이다.
 * 드라이버가 실제로 부르는 pci_read_config_word(dev, ...) 같은 함수는
 * 이 파일 뒤쪽에서 dev->bus 와 dev->devfn 을 꺼내 이 함수들에게 넘긴다.
 *
 * noinline 인 이유: config 접근은 성능 경로가 아니고, 인라인되면 호출자마다
 * 코드가 불어나며 스택 트레이스에서 접근 지점이 사라져 디버깅이 어려워진다.
 *
 * 실행 컨텍스트: pci_lock_config() 이 spin_lock_irqsave 이므로 인터럽트 문맥에서도
 *   부를 수 있다. 대신 이 락 안에서는 잠들 수 없다.
 * 피호출자: bus->ops->read / write — 호스트 브리지 드라이버가 채워 넣은 함수
 *   포인터다. x86 이면 포트 0xCF8/0xCFC 방식이거나 ECAM(MMIO) 방식이고,
 *   ARM SoC 면 그 SoC 의 PCIe 컨트롤러 드라이버가 제공한다.
 *
 * 호출 체인:
 *   nvme_probe -> ... -> pci_read_config_word -> [pci_bus_read_config_word] -> bus->ops->read
 */
/* [한국어] 읽기 3종을 찍어내는 틀. 아래 PCI_OP_READ(byte,u8,1) 같은 한 줄이
 * 이 본문 전체로 펼쳐진다. 매 물리 줄 끝의 백슬래시는 "다음 줄도 이 매크로의
 * 일부"라는 표시이므로 하나라도 빠지면 정의가 그 자리에서 끊긴다. */
#define PCI_OP_READ(size, type, len) \
/* [한국어] 함수 이름에 size 토큰을 붙여 pci_bus_read_config_byte/word/dword 를 만든다 */ \
int noinline pci_bus_read_config_##size \
	/* [한국어] bus+devfn 으로 장치를 지목하고, pos(=config space 안의 바이트 오프셋)에서 \
	 * 읽어 value 가 가리키는 곳에 넣는다. dev 포인터가 아니라 bus/devfn 을 받는 이유는 \
	 * 아직 struct pci_dev 가 만들어지기 전인 열거(enumeration) 단계에서도 써야 하기 때문이다. */ \
	(struct pci_bus *bus, unsigned int devfn, int pos, type *value)	\
{									\
	unsigned long flags;	/* [한국어] spin_lock_irqsave 가 저장해 둘 인터럽트 플래그 */ \
	/* [한국어] 버스 드라이버는 폭과 무관하게 항상 u32 에 담아 돌려준다. \
	 * 0 으로 초기화해 두는 것은, 드라이버가 실패 경로에서 data 를 건드리지 않고 \
	 * 돌아가더라도 초기화되지 않은 스택 값이 새어 나가지 않게 하기 위함이다. */ \
	u32 data = 0;							\
	int res;		/* [한국어] bus->ops->read 가 준 PCIBIOS_* 결과 코드 */ \
									\
	/* [한국어] 정렬 검사. size 에 word 가 들어오면 PCI_word_BAD, 즉 (pos & 1) 로 \
	 * 펼쳐진다. PCI config 트랜잭션은 4바이트 워드와 바이트 활성 신호로 표현되므로 \
	 * 정렬되지 않은 오프셋은 애초에 표현할 수 없고, 그대로 내려보내면 컨트롤러마다 \
	 * 결과가 달라진다. 그래서 버스에 닿기 전에 여기서 잘라낸다. */ \
	if (PCI_##size##_BAD)						\
		return PCIBIOS_BAD_REGISTER_NUMBER;			\
									\
	/* [한국어] config 접근을 전역 pci_lock 으로 직렬화한다. 이유는 두 가지다. \
	 * (1) 0xCF8/0xCFC 방식은 주소 레지스터에 쓰고 데이터 레지스터를 읽는 2단계라, \
	 *     그 사이에 다른 CPU 가 주소를 덮어쓰면 엉뚱한 장치를 읽는다. \
	 * (2) 리셋 중 접근 차단(block_cfg_access)을 검사·대기하는 지점과 동기화해야 한다. \
	 *     _irqsave 판이라 인터럽트 핸들러 안에서 불려도 안전하다. */ \
	pci_lock_config(flags);						\
	/* [한국어] 실제 하드웨어 접근. 호스트 브리지 드라이버의 read 콜백으로 내려간다. \
	 * len 은 1/2/4 중 하나이며, 성공하면 data 에 그 폭만큼의 값이 담긴다. */ \
	res = bus->ops->read(bus, devfn, pos, len, &data);		\
	if (res)							\
		/* [한국어] 실패 시 출력 버퍼를 all-ones 로 채운다. 0 이 아니라 all-ones 인 \
		 * 이유: 응답 없는 장치를 읽으면 버스가 실제로 all-ones 를 돌려주므로, \
		 * 소프트웨어 실패와 하드웨어 무응답이 같은 모습이 되어 호출자가 한 가지 \
		 * 방법으로만 판정하면 된다. "Vendor ID 가 0xFFFF 면 장치 없음" 이라는 \
		 * PCI 관용구가 여기서 나온다. 반환값을 안 보는 호출자도 값만 보고 \
		 * 이상을 알아챌 수 있다는 점이 이 설계의 핵심이다. */ \
		PCI_SET_ERROR_RESPONSE(value);				\
	else								\
		/* [한국어] 성공. u32 로 받은 것을 요청 폭으로 잘라 담는다. \
		 * u8 이면 하위 8비트만 남는 축소 변환이 일어난다. */ \
		*value = (type)data;					\
	pci_unlock_config(flags);					\
									\
	/* [한국어] PCIBIOS_SUCCESSFUL(0) 또는 PCIBIOS_* 오류 코드. \
	 * 커널 errno 가 아니므로 호출자는 pcibios_err_to_errno() 로 변환해 쓰기도 한다. */ \
	return res;							\
}

/* [한국어] 쓰기 3종을 찍어내는 틀. 읽기 판과 거의 같지만 두 가지가 다르다.
 * (1) value 를 포인터가 아니라 값으로 받는다 — 돌려줄 것이 없기 때문이다.
 * (2) 실패해도 채워 줄 버퍼가 없으므로 PCI_SET_ERROR_RESPONSE 에 해당하는 처리가 없다.
 *     쓰기가 실패했는지는 오직 반환값으로만 알 수 있다. */
#define PCI_OP_WRITE(size, type, len) \
/* [한국어] pci_bus_write_config_byte/word/dword 를 만든다 */ \
int noinline pci_bus_write_config_##size \
	/* [한국어] bus+devfn 이 가리키는 장치의 config space 오프셋 pos 에 value 를 쓴다. */ \
	(struct pci_bus *bus, unsigned int devfn, int pos, type value)	\
{									\
	unsigned long flags;	/* [한국어] 인터럽트 플래그 저장용 */ \
	int res;		/* [한국어] bus->ops->write 의 PCIBIOS_* 결과 */ \
									\
	/* [한국어] 읽기와 같은 정렬 검사. 정렬되지 않은 쓰기는 인접 레지스터를 \
	 * 함께 망가뜨릴 수 있으므로 읽기보다 오히려 더 위험하다. */ \
	if (PCI_##size##_BAD)						\
		return PCIBIOS_BAD_REGISTER_NUMBER;			\
									\
	/* [한국어] 읽기와 같은 이유로 직렬화. 특히 쓰기는 read-modify-write 로 \
	 * 이어지는 경우가 많아(예: Command 레지스터의 한 비트만 켜기) \
	 * 락 없이는 두 CPU 의 갱신이 서로를 지운다. */ \
	pci_lock_config(flags);						\
	/* [한국어] 실제 하드웨어 쓰기. NVMe 초기화 중 pci_enable_device 가 \
	 * Command 레지스터의 Memory Space Enable/Bus Master Enable 비트를 켜는 것이 \
	 * 이 경로를 타는 대표적인 예다. */ \
	res = bus->ops->write(bus, devfn, pos, len, value);		\
	pci_unlock_config(flags);					\
									\
	return res;							\
}

/* [한국어] 여기서 틀을 실제 함수로 펼친다. 이 6줄이 곧 6개 함수의 정의다.
 * byte/word/dword 각각에 대해 (이름 토큰, C 타입, 바이트 수)를 짝지어 넘긴다. */
PCI_OP_READ(byte, u8, 1)	/* [한국어] pci_bus_read_config_byte — 1바이트. Capability ID/Next 포인터,
				 * Revision ID, Interrupt Line 같은 바이트 폭 레지스터에 쓰인다 */
PCI_OP_READ(word, u16, 2)	/* [한국어] pci_bus_read_config_word — 2바이트. Vendor ID, Device ID,
				 * Command, Status 등 PCI 헤더의 주력 폭이다 */
PCI_OP_READ(dword, u32, 4)	/* [한국어] pci_bus_read_config_dword — 4바이트. BAR, Class Code,
				 * PCIe capability 의 각 레지스터가 이 폭이다 */
PCI_OP_WRITE(byte, u8, 1)	/* [한국어] pci_bus_write_config_byte */
PCI_OP_WRITE(word, u16, 2)	/* [한국어] pci_bus_write_config_word — Command 레지스터 갱신이 대표 용례 */
PCI_OP_WRITE(dword, u32, 4)	/* [한국어] pci_bus_write_config_dword — BAR 크기 측정 때 all-ones 를
				 * 써 넣는 동작이 이 함수로 이뤄진다 */

/* [한국어] 아래 6개 EXPORT_SYMBOL 은 이 함수들을 모듈에서도 부를 수 있게 심볼 테이블에
 * 올린다. GPL 전용(_GPL)이 아닌 보통 EXPORT_SYMBOL 인 것은 오래전부터 공개돼 온
 * 기본 API 이기 때문이다. 다만 드라이버가 직접 이 bus 판을 부르는 일은 드물고,
 * 보통은 pci_dev 를 받는 wrapper(pci_read_config_word 등)를 쓴다. */
EXPORT_SYMBOL(pci_bus_read_config_byte);
EXPORT_SYMBOL(pci_bus_read_config_word);
EXPORT_SYMBOL(pci_bus_read_config_dword);
EXPORT_SYMBOL(pci_bus_write_config_byte);
EXPORT_SYMBOL(pci_bus_write_config_word);
EXPORT_SYMBOL(pci_bus_write_config_dword);

/*
 * [한국어]
 * pci_generic_config_read - ECAM 방식 호스트 브리지가 그대로 쓸 수 있는 표준 read 콜백
 *
 * @bus:   대상 버스. bus->ops->map_bus 를 통해 이 버스의 주소 변환 규칙을 얻는다.
 * @devfn: 버스 안에서 장치를 지목하는 8비트 값. 상위 5비트가 슬롯, 하위 3비트가 함수.
 * @where: config space 안의 바이트 오프셋(0~4095).
 * @size:  1 / 2 / 4 바이트.
 * @val:   읽은 값을 담아 돌려줄 곳. 항상 u32 폭이며 상위 비트는 0 으로 채워진다.
 * @return: PCIBIOS_SUCCESSFUL(0) 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 이 함수는 struct pci_ops 의 .read 자리에 그대로 꽂아 쓰라고 만들어진 것이다.
 * ECAM(Enhanced Configuration Access Mechanism)을 쓰는 플랫폼에서는 config space 가
 * 통째로 물리 메모리에 매핑돼 있어서, "장치+오프셋 -> 가상 주소" 변환만 SoC 마다
 * 다르고 나머지 읽기 동작은 완전히 같다. 그래서 다른 부분만 map_bus 콜백으로
 * 떼어내고 공통 부분을 여기 한 벌 두었다. 호스트 브리지 드라이버 수십 개가
 * 같은 코드를 각자 복사하는 것을 막는다.
 *
 * ECAM 의 주소 계산 규칙 자체는 PCIe 스펙에 정의돼 있다 —
 * base + (bus << 20) + (devfn << 12) + offset. map_bus 구현이 이 계산을 한다.
 *
 * 실행 컨텍스트: 호출자(PCI_OP_READ 로 만들어진 함수)가 pci_lock 을 쥔 상태로
 *   들어온다. 따라서 여기서는 잠들 수 없고, 추가 락도 필요 없다.
 * 호출자: bus->ops->read 간접 호출 — 즉 pci_bus_read_config_word 등.
 * 피호출자: bus->ops->map_bus, readb/readw/readl.
 *
 * 호출 체인:
 *   pci_read_config_word -> pci_bus_read_config_word -> [pci_generic_config_read] -> readw
 */
int pci_generic_config_read(struct pci_bus *bus, unsigned int devfn,
			    int where, int size, u32 *val)
{
	void __iomem *addr;	/* [한국어] map_bus 가 돌려줄, 그 레지스터에 대응하는 가상 주소 */

	/* [한국어] 장치+오프셋을 CPU 가 접근할 수 있는 가상 주소로 바꾼다.
	 * 이 콜백이 SoC 별 차이를 전부 흡수하는 지점이다. */
	addr = bus->ops->map_bus(bus, devfn, where);
	/* [한국어] NULL 은 "이 버스 번호/장치 번호는 이 브리지가 다루는 범위 밖" 이라는 뜻이다.
	 * 예컨대 루트 포트 아래 링크에는 장치 0 만 존재할 수 있으므로, map_bus 가
	 * devfn != 0 을 걸러내 존재하지 않는 장치를 열거하지 않게 한다. */
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 요청 폭에 맞는 MMIO 읽기를 고른다. readb/readw/readl 은 단순한
	 * 포인터 역참조가 아니라 컴파일러 재배치를 막고 아키텍처별 배리어를 포함한다.
	 * MMIO 는 읽는 것만으로 부작용(RW1C 클리어 등)이 있을 수 있어 순서가 중요하다. */
	if (size == 1)
		*val = readb(addr);	/* [한국어] 1바이트 — Capability ID/Next 등 */
	else if (size == 2)
		*val = readw(addr);	/* [한국어] 2바이트 — Vendor/Device ID, Command, Status */
	else
		*val = readl(addr);	/* [한국어] 4바이트 — BAR, Class Code 등. size 검증은
					 * 이미 호출자(PCI_OP_READ)가 했으므로 여기서는 else 로 받는다 */

	return PCIBIOS_SUCCESSFUL;	/* [한국어] MMIO 읽기는 실패를 알릴 방법이 없다.
					 * 장치가 없으면 all-ones 가 읽히고, 그것이 곧 신호다 */
}
/* [한국어] ECAM 계열 호스트 브리지 드라이버들이 모듈로 빌드될 수 있어 export 한다.
 * _GPL 인 것은 비교적 근래에 추가된 내부 인프라이기 때문이다. */
EXPORT_SYMBOL_GPL(pci_generic_config_read);

/*
 * [한국어]
 * pci_generic_config_write - ECAM 방식 호스트 브리지가 그대로 쓸 수 있는 표준 write 콜백
 *
 * @bus:   대상 버스.
 * @devfn: 슬롯(상위 5비트) + 함수(하위 3비트).
 * @where: config space 바이트 오프셋.
 * @size:  1 / 2 / 4 바이트.
 * @val:   쓸 값. 폭보다 큰 상위 비트는 무시된다.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * read 판의 거울상이다. 하드웨어가 바이트/워드 단위 쓰기를 그대로 지원하므로
 * 요청한 폭 그대로 한 번의 MMIO 쓰기를 내보낸다 — 아래 write32 판과 달리
 * read-modify-write 가 필요 없고, 따라서 인접 RW1C 비트를 건드릴 위험이 없다.
 *
 * 이 경로를 타는 대표적인 동작:
 *   - pci_enable_device 가 Command 레지스터의 Memory Space Enable 비트를 켤 때
 *   - pci_set_master 가 Bus Master Enable 비트를 켤 때(DMA 를 허용한다는 뜻이라,
 *     NVMe 처럼 스스로 메모리를 읽고 쓰는 장치에는 필수다)
 *   - BAR 크기를 재려고 BAR 에 all-ones 를 써 넣을 때
 *
 * 실행 컨텍스트: 호출자가 pci_lock 을 쥔 상태. 잠들 수 없다.
 * 호출자: bus->ops->write 간접 호출.
 * 피호출자: bus->ops->map_bus, writeb/writew/writel.
 *
 * 호출 체인:
 *   pci_write_config_word -> pci_bus_write_config_word -> [pci_generic_config_write] -> writew
 */
int pci_generic_config_write(struct pci_bus *bus, unsigned int devfn,
			     int where, int size, u32 val)
{
	void __iomem *addr;	/* [한국어] 쓸 레지스터의 가상 주소 */

	/* [한국어] read 판과 동일한 주소 변환. */
	addr = bus->ops->map_bus(bus, devfn, where);
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;	/* [한국어] 없는 장치에 쓰기를 내보내면
							 * 버스 오류가 될 수 있으므로 여기서 막는다 */

	/* [한국어] 요청 폭 그대로 내보낸다. 인자 순서가 write(값, 주소)로
	 * read(주소)와 반대라는 점에 주의 — 커널 MMIO API 의 오랜 관례다. */
	if (size == 1)
		writeb(val, addr);	/* [한국어] 1바이트 */
	else if (size == 2)
		writew(val, addr);	/* [한국어] 2바이트 — Command 레지스터 갱신이 대표 용례 */
	else
		writel(val, addr);	/* [한국어] 4바이트 — BAR 설정 등 */

	return PCIBIOS_SUCCESSFUL;	/* [한국어] posted write 라 완료를 기다리지 않는다.
					 * 쓰기가 실제로 반영됐는지 확인하려면 되읽어야 한다 */
}
EXPORT_SYMBOL_GPL(pci_generic_config_write);	/* [한국어] 호스트 브리지 드라이버용 export */

/*
 * [한국어]
 * pci_generic_config_read32 - 32비트 접근만 되는 하드웨어용 read 콜백
 *
 * @bus:   대상 버스.
 * @devfn: 슬롯 + 함수.
 * @where: config space 바이트 오프셋. 정렬돼 있지 않아도 된다.
 * @size:  1 / 2 / 4 바이트.
 * @val:   추출한 값을 담을 곳.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 일부 PCIe 컨트롤러는 config space 창에 대해 4바이트 접근만 받아들인다.
 * 바이트/워드 읽기를 그대로 내보내면 버스 오류가 나거나 엉뚱한 값이 나온다.
 * 그래서 이 판은 항상 dword 로 읽은 뒤, 원하는 바이트를 소프트웨어에서 골라낸다.
 *
 * 읽기에서는 이 흉내가 완전히 안전하다. 읽는 폭이 넓어진 것뿐이고, 넘치게 읽은
 * 바이트는 그냥 버리기 때문이다. (쓰기 쪽은 사정이 다르다 — 아래
 * pci_generic_config_write32 의 주석 참고.)
 *
 * 실행 컨텍스트: 호출자가 pci_lock 을 쥔 상태.
 * 호출자: bus->ops->read 간접 호출.
 *
 * 호출 체인:
 *   pci_read_config_byte -> pci_bus_read_config_byte -> [pci_generic_config_read32] -> readl
 */
int pci_generic_config_read32(struct pci_bus *bus, unsigned int devfn,
			      int where, int size, u32 *val)
{
	void __iomem *addr;	/* [한국어] dword 로 내림 정렬된 주소 */

	/* [한국어] where & ~0x3 은 하위 2비트를 잘라 4의 배수로 내린다.
	 * 예: 오프셋 0x0A(Status) 를 요청받으면 0x08 을 읽는다. */
	addr = bus->ops->map_bus(bus, devfn, where & ~0x3);
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 폭과 무관하게 항상 4바이트를 읽는다. 이 하드웨어가 허용하는
	 * 유일한 접근 폭이기 때문이다. */
	*val = readl(addr);

	/* [한국어] 1/2바이트 요청이면 읽어 온 dword 에서 해당 바이트를 뽑아낸다.
	 * where & 3 = 정렬 지점으로부터의 바이트 오프셋 -> 8을 곱해 비트 오프셋.
	 * 예: where=0x0A, size=2 이면 (0x0A & 3)=2 -> 16비트 오른쪽 시프트로
	 * 상위 워드를 끌어내리고, ((1<<16)-1)=0xFFFF 로 마스크해 상위를 지운다.
	 * 리틀엔디언 config space 를 전제로 한 계산이다 — PCI 는 스펙상 리틀엔디언이고
	 * readl() 이 그것을 CPU 바이트 순서로 이미 변환해 주므로 이 산술이 성립한다.
	 * size==4 면 시프트도 마스크도 필요 없어 건너뛴다(그리고 1<<32 는
	 * u32 에서 정의되지 않은 동작이므로, 이 분기 조건이 그것도 함께 막는다). */
	if (size <= 2)
		*val = (*val >> (8 * (where & 3))) & ((1 << (size * 8)) - 1);

	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_generic_config_read32);	/* [한국어] 32비트 전용 컨트롤러 드라이버용 */

/*
 * [한국어]
 * pci_generic_config_write32 - 32비트 접근만 되는 하드웨어용 write 콜백 (부분 쓰기 흉내)
 *
 * @bus:   대상 버스.
 * @devfn: 슬롯 + 함수.
 * @where: config space 바이트 오프셋. 정렬돼 있지 않아도 된다.
 * @size:  1 / 2 / 4 바이트.
 * @val:   쓸 값.
 * @return: PCIBIOS_SUCCESSFUL 또는 PCIBIOS_DEVICE_NOT_FOUND.
 *
 * 읽기 판(read32)의 짝이지만, 이쪽은 근본적으로 안전하지 않다. 4바이트 쓰기만
 * 되는 하드웨어에서 2바이트를 쓰려면 "dword 읽기 -> 원하는 2바이트만 갈아끼우기
 * -> dword 쓰기" 를 해야 하는데, 이때 건드릴 의도가 없던 나머지 바이트까지
 * 다시 써 넣게 된다.
 *
 * 그 나머지 바이트에 RW1C(Write-1-to-Clear) 비트가 있으면 사고가 난다. RW1C 는
 * "1 을 쓰면 그 비트가 지워진다" 는 규약으로, 에러 상태 비트들이 이 방식이다.
 * 읽을 때 그 비트가 1 이었다면(=에러가 기록돼 있었다면) 되쓰기가 그 1 을 다시
 * 써 넣어 에러 기록을 지워 버린다. 아무도 그 에러를 보지 못하게 되는 것이다.
 * PCI_STATUS 레지스터(오프셋 0x06)가 바로 그런 경우로, Master Data Parity Error,
 * Signaled Target Abort, Detected Parity Error 같은 비트가 모두 RW1C 다.
 *
 * 그래서 이 함수는 부분 쓰기가 실제로 일어날 때 한 번 경고를 남긴다. 스펙을
 * 지키지 않는 하드웨어라는 사실을 로그에 남겨 두려는 것이지, 문제를 고치지는
 * 못한다 — 하드웨어가 4바이트 쓰기만 받는 이상 다른 방법이 없다.
 *
 * 실행 컨텍스트: 호출자가 pci_lock 을 쥔 상태. 이 락이 read-modify-write 구간을
 *   원자적으로 만들어 준다 — 그래서 최소한 다른 CPU 의 config 쓰기와는 겹치지 않는다.
 *   하지만 하드웨어가 그 사이에 스스로 상태 비트를 세우는 것까지는 막을 수 없다.
 * 호출자: bus->ops->write 간접 호출.
 *
 * 호출 체인:
 *   pci_write_config_word -> pci_bus_write_config_word -> [pci_generic_config_write32] -> readl+writel
 */
int pci_generic_config_write32(struct pci_bus *bus, unsigned int devfn,
			       int where, int size, u32 val)
{
	void __iomem *addr;	/* [한국어] dword 정렬된 주소 */
	u32 mask, tmp;		/* [한국어] mask = 보존할 비트를 1로 둔 마스크,
				 * tmp = 기존 값과 새 값을 합성할 작업용 변수 */

	/* [한국어] read32 와 같은 내림 정렬. */
	addr = bus->ops->map_bus(bus, devfn, where & ~0x3);
	if (!addr)
		return PCIBIOS_DEVICE_NOT_FOUND;

	/* [한국어] 4바이트 요청이면 read-modify-write 가 필요 없다. 그대로 쓰고
	 * 빠져나간다 — 아래 경고도, RW1C 위험도 발생하지 않는 유일한 경로다.
	 * (where 가 정렬돼 있지 않은데 size==4 인 경우는 호출자 쪽 PCI_dword_BAD
	 * 검사에서 이미 걸러졌다.) */
	if (size == 4) {
		writel(val, addr);
		return PCIBIOS_SUCCESSFUL;
	}

	/*
	 * In general, hardware that supports only 32-bit writes on PCI is
	 * not spec-compliant.  For example, software may perform a 16-bit
	 * write.  If the hardware only supports 32-bit accesses, we must
	 * do a 32-bit read, merge in the 16 bits we intend to write,
	 * followed by a 32-bit write.  If the 16 bits we *don't* intend to
	 * write happen to have any RW1C (write-one-to-clear) bits set, we
	 * just inadvertently cleared something we shouldn't have.
	 */
	/* [한국어] 버스마다 딱 한 번만 경고한다. config 쓰기는 부팅 중에도 수시로
	 * 일어나므로, 매번 찍으면 로그가 같은 줄로 뒤덮여 정작 중요한 메시지가 묻힌다.
	 * unsafe_warn 은 struct pci_bus 의 1비트 플래그이며, 이 갱신은 pci_lock
	 * 안에서 일어나므로 두 CPU 가 동시에 경고를 찍을 일은 없다. */
	if (!bus->unsafe_warn) {
		/* [한국어] 어느 장치의 어느 오프셋에서 위험한 접근이 있었는지 남긴다.
		 * %04x:%02x:%02x.%d 는 커널이 PCI 장치를 표기하는 표준 형식으로,
		 * 도메인:버스:슬롯.함수 순이다(예: 0000:01:00.0 — NVMe SSD 가 흔히 갖는 주소).
		 * PCI_SLOT/PCI_FUNC 는 devfn 을 각각 (devfn >> 3) & 0x1f 와 devfn & 7 로 쪼갠다. */
		dev_warn(&bus->dev, "%d-byte config write to %04x:%02x:%02x.%d offset %#x may corrupt adjacent RW1C bits\n",
			 size, pci_domain_nr(bus), bus->number,
			 PCI_SLOT(devfn), PCI_FUNC(devfn), where);
		bus->unsafe_warn = 1;	/* [한국어] 이 버스에 대해서는 다시 찍지 않는다 */
	}

	/* [한국어] 갈아끼울 자리를 0 으로, 보존할 자리를 1 로 만든 마스크.
	 * 단계별로: (1<<(size*8))-1 이 폭만큼의 1 (size=2 면 0x0000FFFF),
	 * << ((where&3)*8) 로 그 1들을 목표 바이트 위치로 옮기고(where=0x0A 면 16비트 왼쪽),
	 * ~ 로 뒤집어 0x0000FFFF 를 얻는다 — 즉 상위 워드는 보존, 하위 워드는 교체. */
	mask = ~(((1 << (size * 8)) - 1) << ((where & 0x3) * 8));
	/* [한국어] 현재 값을 읽어 보존할 부분만 남긴다. 바로 이 읽기와 아래 쓰기 사이가
	 * RW1C 사고가 나는 구간이다 — 읽어 온 1 이 그대로 다시 써 나간다. */
	tmp = readl(addr) & mask;
	/* [한국어] 새 값을 목표 바이트 위치로 옮겨 끼워 넣는다. val 의 폭이 이미
	 * size 바이트라고 전제하므로 마스크 없이 OR 해도 이웃을 침범하지 않는다. */
	tmp |= val << ((where & 0x3) * 8);
	/* [한국어] 합성된 dword 를 한 번에 써 넣는다. 하드웨어가 받아들이는
	 * 유일한 폭이다. */
	writel(tmp, addr);

	return PCIBIOS_SUCCESSFUL;
}
EXPORT_SYMBOL_GPL(pci_generic_config_write32);	/* [한국어] 32비트 전용 컨트롤러 드라이버용 */

/**
 * pci_bus_set_ops - Set raw operations of pci bus
 * @bus:	pci bus struct
 * @ops:	new raw operations
 *
 * Return previous raw operations
 */
/*
 * [한국어]
 * pci_bus_set_ops - 버스의 config 접근 방법을 통째로 바꿔 끼운다
 *
 * @bus: 대상 버스
 * @ops: 새로 꽂을 pci_ops (read/write 두 콜백을 가진 구조체)
 * @return: 바꾸기 직전에 꽂혀 있던 ops. 호출자가 나중에 원복할 수 있게 돌려준다.
 *
 * bus->ops 는 이 버스의 config space 에 어떻게 도달하는지를 정의한다. 보통은
 * 부팅 중 한 번 정해지고 끝이지만, 몇 가지 상황에서 런타임에 갈아끼워야 한다.
 * 대표적으로 특정 칩셋의 버그를 우회하려고 read/write 를 감싸는 판으로 바꾸거나,
 * 초기 열거 단계에서 임시 접근 방식을 쓰다가 본래 방식으로 되돌리는 경우다.
 *
 * 락이 필요한 이유가 미묘하다. 포인터 대입 자체는 원자적이지만, 다른 CPU 가
 * 바로 이 순간 bus->ops->read 를 호출하는 중일 수 있다. pci_lock 은 이 파일의
 * 모든 config 접근이 쥐는 락이므로, 이 락을 잡으면 "진행 중인 접근이 하나도
 * 없는 시점" 이 보장되고, 그때 포인터를 바꿔야 반쪽짜리 전환이 생기지 않는다.
 *
 * 실행 컨텍스트: irqsave 판이라 인터럽트 문맥에서도 부를 수 있지만, 실제
 *   호출자는 대개 초기화 코드다.
 * 호출자: 칩셋 quirk, 일부 아키텍처의 PCI 초기화 코드.
 *
 * 호출 체인:
 *   <아키텍처/quirk 초기화> -> [pci_bus_set_ops]
 */
struct pci_ops *pci_bus_set_ops(struct pci_bus *bus, struct pci_ops *ops)
{
	struct pci_ops *old_ops;	/* [한국어] 호출자에게 돌려줄 이전 ops */
	unsigned long flags;		/* [한국어] 인터럽트 플래그 저장용 */

	/* [한국어] 진행 중인 config 접근이 없음을 보장하는 지점. irqsave 판을 쓰는
	 * 것은 이 함수가 인터럽트가 열린 문맥에서도 불릴 수 있기 때문이다. */
	raw_spin_lock_irqsave(&pci_lock, flags);
	old_ops = bus->ops;	/* [한국어] 갈아끼우기 전 값을 먼저 챙긴다 */
	bus->ops = ops;		/* [한국어] 전환. 이 시점 이후의 모든 접근은 새 ops 를 탄다 */
	raw_spin_unlock_irqrestore(&pci_lock, flags);
	return old_ops;		/* [한국어] quirk 가 원래 동작을 감싸는 경우, 새 read 안에서
				 * 이 포인터를 통해 원래 read 를 부르는 식으로 쓴다 */
}
EXPORT_SYMBOL(pci_bus_set_ops);

/*
 * The following routines are to prevent the user from accessing PCI config
 * space when it's unsafe to do so.  Some devices require this during BIST and
 * we're required to prevent it during D-state transitions.
 *
 * We have a bit per device to indicate it's blocked and a global wait queue
 * for callers to sleep on until devices are unblocked.
 */
/* [한국어] config 접근이 차단된 동안 기다리는 태스크들이 매달릴 대기열.
 * 장치마다 하나씩 두지 않고 전역 하나만 두는 것이 이 설계의 선택이다.
 * 차단이 걸리는 일 자체가 드물어서(리셋/BIST/D-state 전환 정도), 전역 대기열의
 * 비용 — 어느 장치가 풀리든 잠든 태스크가 모두 깨어나 조건을 다시 확인하는
 * thundering herd — 보다 struct pci_dev 마다 대기열 헤드를 넣는 메모리 비용이
 * 더 크다고 본 것이다. 대신 각 장치의 차단 여부는 dev->block_cfg_access 라는
 * 장치별 1비트가 들고 있고, 깨어난 태스크는 그 비트를 다시 확인한다. */
static DECLARE_WAIT_QUEUE_HEAD(pci_cfg_wait);

/*
 * [한국어]
 * pci_wait_cfg - config 접근이 차단 해제될 때까지 기다린다
 *
 * @dev: 접근하려는 장치
 * @return: 없음. 돌아올 때는 반드시 pci_lock 을 다시 쥔 상태다.
 *
 * 리셋이나 장치 제거처럼 config space 를 건드리면 안 되는 구간에서는
 * pci_cfg_access_lock() 이 dev->block_cfg_access 를 세워 둔다. 그 사이에 들어온
 * 접근을 -EBUSY 로 실패시키지 않고 여기서 재우는 것이 이 함수의 역할이다.
 * 덕분에 호출자(pci_read_config_word 등)는 "지금 리셋 중일 수도 있다"는 사정을
 * 전혀 몰라도 되고, 리셋이 끝나면 자기 요청이 정상 수행된다.
 *
 * 락을 놓았다 다시 잡는 구조가 이 함수의 핵심이다. 호출자는 pci_lock(raw
 * spinlock)을 쥔 채로 들어오는데, 스핀락을 쥔 상태로는 잠들 수 없다.
 * 그래서 풀고 -> 자고 -> 다시 잡는다. 그 사이에 상태가 또 바뀔 수 있으므로
 * do/while 로 다시 확인한다 — 잠에서 깼다고 조건이 참이라고 믿어서는 안 된다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. 잠들 수 있으므로 인터럽트 문맥이나
 *   atomic 구간에서 이 경로에 들어와서는 안 된다.
 * 호출자: pci_read_config_ 계열과 pci_write_config_ 계열이 block_cfg_access 를 보고 부른다.
 * 피호출자: wait_event() — 커널 대기열 pci_cfg_wait 에 자신을 매단다.
 * 깨우는 쪽: pci_cfg_access_unlock() 이 block_cfg_access 를 0 으로 만들고
 *   wake_up_all(&pci_cfg_wait) 로 여기 잠든 태스크를 모두 깨운다.
 *
 * 호출 체인:
 *   nvme_reset_work -> ... -> pci_read_config_word -> [pci_wait_cfg] -> wait_event
 */
static noinline void pci_wait_cfg(struct pci_dev *dev)
	/* [한국어] sparse 어노테이션 — 호출자가 pci_lock 을 쥐고 들어와야 하고
	 * 돌아갈 때도 쥔 상태여야 함을 정적 분석기에 알린다. 함수 안에서 락을
	 * 풀었다 잡으므로 사람 눈에는 균형이 깨져 보이지만 실제로는 맞다. */
	__must_hold(&pci_lock)
{
	do {
		/* [한국어] 잠들기 전에 반드시 놓는다. 스핀락을 쥔 채 자면 그 락을
		 * 기다리는 다른 CPU 가 무한정 회전하며 시스템 전체가 멈춘다.
		 * _irq 판이므로 인터럽트도 함께 다시 열린다 — 그래야 깨우는 쪽의
		 * IPI/타이머가 이 CPU 에 도달할 수 있다. */
		raw_spin_unlock_irq(&pci_lock);
		/* [한국어] block_cfg_access 가 0 이 될 때까지 잠든다. wait_event 는
		 * 잠들기 전에 조건을 먼저 확인하므로, 이미 풀렸다면 자지 않고 빠져나온다. */
		wait_event(pci_cfg_wait, !dev->block_cfg_access);
		/* [한국어] 호출자에게 제어를 돌려주기 전에 락 상태를 원래대로 되돌린다. */
		raw_spin_lock_irq(&pci_lock);
	} while (dev->block_cfg_access);	/* [한국어] 깨어난 뒤 재확인. 락을 놓았다 다시 잡는
						 * 사이에 또 다른 태스크가 차단을 걸었을 수 있다 */
}

/* Returns 0 on success, negative values indicate error. */
/*
 * [한국어]
 * PCI_USER_READ_CONFIG - userspace 를 대신한 config 읽기 함수 3개를 찍어내는 틀
 *
 * @size: 이름/정렬검사 토큰 (byte / word / dword)
 * @type: 결과 타입 (u8 / u16 / u32)
 *
 * 앞의 PCI_OP_READ 판과 코드가 비슷해 보이지만, 쓰임과 계약이 다르다.
 *
 * 1) 차단 상태를 존중한다. PCI_OP_READ 판은 block_cfg_access 를 보지 않고
 *    바로 하드웨어로 내려간다 — 커널 자신이 리셋을 수행하는 중에도 config 를
 *    읽어야 하기 때문이다. 반면 이 판은 차단 중이면 pci_wait_cfg() 로 잠든다.
 *    userspace 가 리셋 한복판의 장치를 들여다보는 것을 막기 위해서다.
 *    다시 말해 "차단" 은 커널 내부 접근이 아니라 바깥 접근을 겨냥한 장치다.
 *
 * 2) 반환값 규약이 다르다. PCI_OP_READ 판은 PCIBIOS_* 코드를 그대로 주지만
 *    이 판은 pcibios_err_to_errno() 로 음수 errno 로 바꿔 준다. 최종적으로
 *    read(2)/write(2) 의 반환값이 되어야 하므로 커널 표준 errno 여야 한다.
 *
 * 3) dev 포인터를 받는다. bus/devfn 을 따로 받던 저수준 판과 달리, 이 시점에는
 *    struct pci_dev 가 이미 존재한다(userspace 가 sysfs 로 접근하려면 장치가
 *    등록돼 있어야 하니 당연하다).
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용. pci_wait_cfg() 로 잠들 수 있다.
 * 호출자: sysfs 의 config 속성(pci-sysfs.c), /proc/bus/pci(proc.c),
 *   VFIO 의 config space 에뮬레이션. NVMe 학습 관점에서는 lspci 가
 *   NVMe 장치의 capability 를 훑을 때 실제로 타는 경로다.
 *
 * 호출 체인:
 *   lspci -> read(sysfs config) -> pci_read_config -> [pci_user_read_config_dword]
 *     -> dev->bus->ops->read
 */
#define PCI_USER_READ_CONFIG(size, type) \
int pci_user_read_config_##size \
	(struct pci_dev *dev, int pos, type *val) \
{ \
	/* [한국어] -1 로 초기화한다. 즉 all-ones. 아래 실패 경로가 val 을 따로 \
	 * 채우기는 하지만, 버스 드라이버가 성공을 반환하면서 data 를 건드리지 \
	 * 않는 경우까지 대비한 두 겹의 안전장치다. */ \
	u32 data = -1;								\
	int ret;								\
										\
	/* [한국어] 저수준 판과 같은 정렬 검사. 다만 여기서는 PCIBIOS 코드가 아니라 \
	 * -EINVAL 을 낸다 — 이 값이 그대로 userspace 의 errno 가 되기 때문이다. */ \
	if (PCI_##size##_BAD)							\
		return -EINVAL;							\
										\
	raw_spin_lock_irq(&pci_lock);	/* [한국어] config 접근 직렬화 */	\
	/* [한국어] 리셋/BIST 등으로 차단 중이면 여기서 잠든다. unlikely() 는 \
	 * "거의 항상 거짓" 이라는 힌트로, 분기 예측과 코드 배치를 정상 경로에 \
	 * 유리하게 만든다. pci_wait_cfg 는 락을 놓았다 다시 잡고 돌아온다. */ \
	if (unlikely(dev->block_cfg_access))					\
		pci_wait_cfg(dev);						\
	/* [한국어] 실제 하드웨어 읽기. sizeof(type) 이 곧 요청 폭이다. */ \
	ret = dev->bus->ops->read(dev->bus, dev->devfn,				\
				  pos, sizeof(type), &data);			\
	raw_spin_unlock_irq(&pci_lock);						\
	if (ret)								\
		/* [한국어] 실패 시 all-ones. userspace 도 "0xFFFF 면 없는 것" 이라는 \
		 * 같은 관용구로 판정할 수 있게 하기 위해서다. */ \
		PCI_SET_ERROR_RESPONSE(val);					\
	else									\
		*val = (type)data;	/* [한국어] 요청 폭으로 축소 */		\
										\
	/* [한국어] PCIBIOS_* -> 음수 errno 변환. 예: PCIBIOS_DEVICE_NOT_FOUND -> -ENODEV. \
	 * 성공(0)은 그대로 0 이다. */ \
	return pcibios_err_to_errno(ret);					\
}										\
EXPORT_SYMBOL_GPL(pci_user_read_config_##size);

/* Returns 0 on success, negative values indicate error. */
/*
 * [한국어]
 * PCI_USER_WRITE_CONFIG - userspace 를 대신한 config 쓰기 함수 3개를 찍어내는 틀
 *
 * @size: 이름/정렬검사 토큰 (byte / word / dword)
 * @type: 인자 타입 (u8 / u16 / u32)
 *
 * 읽기 판의 거울상이다. 차단 대기와 errno 변환이라는 두 가지 특징을 그대로 갖는다.
 * 돌려줄 값이 없으므로 실패 시 버퍼를 채우는 처리는 없고, 결과는 오직 반환값으로만
 * 전달된다.
 *
 * userspace 가 config space 에 쓰는 것은 위험한 동작이라, 이 경로에 닿기까지
 * 여러 겹의 관문이 있다 — sysfs config 속성은 CAP_SYS_ADMIN 을 요구하고,
 * VFIO 는 어느 오프셋을 쓸 수 있는지 자체 필터를 갖고 있다. 이 매크로 자체는
 * 그런 정책을 판단하지 않고, 정렬과 차단 여부만 확인한 뒤 하드웨어로 내려보낸다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용(잠들 수 있음).
 * 호출자: pci-sysfs.c, proc.c, VFIO.
 *
 * 호출 체인:
 *   setpci -> write(sysfs config) -> pci_write_config -> [pci_user_write_config_word]
 *     -> dev->bus->ops->write
 */
#define PCI_USER_WRITE_CONFIG(size, type) \
int pci_user_write_config_##size \
	(struct pci_dev *dev, int pos, type val) \
{ \
	int ret;								\
										\
	/* [한국어] 정렬 검사. 잘못 정렬된 쓰기는 인접 레지스터까지 망가뜨릴 수 \
	 * 있으므로 읽기보다 더 엄격히 막아야 한다. */ \
	if (PCI_##size##_BAD)							\
		return -EINVAL;							\
										\
	raw_spin_lock_irq(&pci_lock);						\
	/* [한국어] 리셋 중인 장치에 userspace 쓰기가 끼어들면 리셋 절차 자체가 \
	 * 어긋나므로, 차단이 풀릴 때까지 기다린다. */ \
	if (unlikely(dev->block_cfg_access))					\
		pci_wait_cfg(dev);						\
	/* [한국어] 실제 하드웨어 쓰기. */ \
	ret = dev->bus->ops->write(dev->bus, dev->devfn,			\
				   pos, sizeof(type), val);			\
	raw_spin_unlock_irq(&pci_lock);						\
										\
	return pcibios_err_to_errno(ret);	/* [한국어] userspace 용 errno 로 변환 */ \
}										\
EXPORT_SYMBOL_GPL(pci_user_write_config_##size);

/* [한국어] 여기서 두 틀을 실제 함수 6개로 펼친다. 저수준 판(PCI_OP_*)과 달리
 * 세 번째 인자(바이트 수)가 없는데, sizeof(type) 으로 폭을 알아내기 때문이다. */
PCI_USER_READ_CONFIG(byte, u8)		/* [한국어] pci_user_read_config_byte */
PCI_USER_READ_CONFIG(word, u16)		/* [한국어] pci_user_read_config_word */
PCI_USER_READ_CONFIG(dword, u32)	/* [한국어] pci_user_read_config_dword */
PCI_USER_WRITE_CONFIG(byte, u8)		/* [한국어] pci_user_write_config_byte */
PCI_USER_WRITE_CONFIG(word, u16)	/* [한국어] pci_user_write_config_word */
PCI_USER_WRITE_CONFIG(dword, u32)	/* [한국어] pci_user_write_config_dword */

/**
 * pci_cfg_access_lock - Lock PCI config reads/writes
 * @dev:	pci device struct
 *
 * When access is locked, any userspace reads or writes to config
 * space and concurrent lock requests will sleep until access is
 * allowed via pci_cfg_access_unlock() again.
 */
/*
 * [한국어]
 * pci_cfg_access_lock - 이 장치의 userspace config 접근을 잠근다
 *
 * @dev: 잠글 장치
 * @return: 없음. 반드시 성공한다(필요하면 잠들어서 기다린다).
 *
 * 여기서 말하는 "잠금" 은 스핀락이 아니라 dev->block_cfg_access 라는 장치별
 * 1비트 플래그를 세우는 일이다. 이 비트가 서 있는 동안 pci_user_read/write_config_*
 * 경로 — 즉 sysfs, /proc/bus/pci, VFIO 를 통한 접근 — 가 pci_wait_cfg() 에서 잠든다.
 * 커널 내부 접근(pci_read_config_word 등)은 영향을 받지 않는다는 점이 중요하다.
 * 리셋을 수행하는 코드 자신이 config 를 읽고 써야 하기 때문이다.
 *
 * 왜 필요한가: 장치 리셋(FLR, Secondary Bus Reset)이나 D-state 전환 중에는
 * config space 가 일시적으로 무의미하거나 접근 자체가 위험하다. BIST 를 도는
 * 장치도 마찬가지다. 이때 lspci 하나가 끼어들어 읽는 것만으로도 절차가
 * 어그러질 수 있어, 그 구간을 명시적으로 봉인한다.
 *
 * 재귀 잠금은 허용되지 않는다. 이미 잠겨 있으면 pci_wait_cfg() 로 기다린 뒤
 * 자기가 잠근다 — 즉 두 번째 호출자는 첫 호출자가 풀 때까지 블록된다.
 * 같은 스레드가 두 번 부르면 자기 자신을 기다리는 교착이 되므로, 호출자는
 * lock/unlock 짝을 반드시 맞춰야 한다.
 *
 * 실행 컨텍스트: 프로세스 컨텍스트 전용 — might_sleep() 이 그것을 못박는다.
 *   원자 문맥에서 필요하면 아래 pci_cfg_access_trylock() 을 써야 한다.
 * 호출자: pci_reset_function 계열, pci_dev_save_and_disable, 전원 상태 전환 코드.
 *   NVMe 컨트롤러가 응답하지 않아 nvme_reset_work 가 리셋을 걸 때 이 경로를 탄다.
 * 짝: 반드시 pci_cfg_access_unlock() 으로 풀어야 한다.
 *
 * 호출 체인:
 *   nvme_reset_work -> pci_reset_function -> [pci_cfg_access_lock]
 */
void pci_cfg_access_lock(struct pci_dev *dev)
{
	/* [한국어] "이 함수는 잠들 수 있다" 는 선언. CONFIG_DEBUG_ATOMIC_SLEEP 이
	 * 켜져 있으면 원자 문맥에서 불렸을 때 즉시 경고를 띄운다. 실제로 잠드는
	 * 것은 아래 pci_wait_cfg 뿐이라 평소에는 지나가지만, 드물게만 잠드는
	 * 경로일수록 이런 선언이 있어야 잘못된 호출이 조용히 묻히지 않는다. */
	might_sleep();

	raw_spin_lock_irq(&pci_lock);	/* [한국어] block_cfg_access 를 읽고 쓰는 구간 보호 */
	/* [한국어] 이미 누군가 잠가 두었으면 그가 풀 때까지 기다린다.
	 * pci_wait_cfg 는 락을 놓았다 다시 잡고 돌아오므로, 아래 줄에서는
	 * 다시 락을 쥔 상태이고 block_cfg_access 는 0 임이 보장된다. */
	if (dev->block_cfg_access)
		pci_wait_cfg(dev);
	dev->block_cfg_access = 1;	/* [한국어] 이제부터 이 장치에 대한 userspace 접근은 잠든다 */
	raw_spin_unlock_irq(&pci_lock);
}
EXPORT_SYMBOL_GPL(pci_cfg_access_lock);

/**
 * pci_cfg_access_trylock - try to lock PCI config reads/writes
 * @dev:	pci device struct
 *
 * Same as pci_cfg_access_lock, but will return 0 if access is
 * already locked, 1 otherwise. This function can be used from
 * atomic contexts.
 */
/*
 * [한국어]
 * pci_cfg_access_trylock - 잠기지 않은 경우에만 잠근다 (기다리지 않는 판)
 *
 * @dev: 잠글 장치
 * @return: true = 내가 잠갔다(나중에 반드시 unlock 해야 한다).
 *          false = 이미 다른 쪽이 잠가 두었다(내가 unlock 하면 안 된다).
 *
 * pci_cfg_access_lock() 과 하는 일은 같지만 기다리지 않는다. 그래서 잠들 수 없는
 * 문맥 — 인터럽트 핸들러, 스핀락 안, 에러 복구 경로 — 에서도 쓸 수 있다.
 *
 * 대표 용례는 AER(Advanced Error Reporting) 처리다. PCIe 에러가 보고되면
 * 인터럽트 문맥에서 상황을 수습해야 하는데, 그 사이 userspace 가 config 를
 * 건드리지 못하게 막고 싶다. 하지만 이미 다른 리셋이 진행 중이라면 기다리는
 * 대신 그 사실을 알고 다르게 대응해야 한다 — 그 판단을 반환값으로 넘긴다.
 *
 * 반환값을 무시하면 안 된다. false 를 받고도 unlock 을 부르면 남의 잠금을
 * 풀어 버려, 리셋 한복판에 userspace 접근이 열린다.
 *
 * 실행 컨텍스트: 어디서나. irqsave 판을 쓰므로 인터럽트가 꺼진 상태에서 불려도
 *   플래그가 올바르게 복원된다(_irq 판을 쓰는 lock() 과 다른 점이다 —
 *   lock() 은 프로세스 컨텍스트 전용이라 인터럽트를 무조건 열어도 되지만,
 *   trylock() 은 이미 꺼져 있던 상태를 그대로 되돌려야 한다).
 * 호출자: AER/DPC 등 에러 복구 경로, pci_dev_trylock.
 *
 * 호출 체인:
 *   AER 인터럽트 -> pcie_do_recovery -> [pci_cfg_access_trylock]
 */
bool pci_cfg_access_trylock(struct pci_dev *dev)
{
	unsigned long flags;	/* [한국어] 원래 인터럽트 상태를 담아 둘 곳 */
	bool locked = true;	/* [한국어] 성공을 기본값으로 두고, 실패 조건에서만 뒤집는다 */

	/* [한국어] irqsave 판 — 인터럽트가 이미 꺼진 문맥에서 불릴 수 있으므로
	 * 무조건 켜는 _irq 판을 쓰면 안 된다. 그렇게 하면 인터럽트를 꺼 둔 채
	 * 무언가를 하던 호출자의 전제가 깨진다. */
	raw_spin_lock_irqsave(&pci_lock, flags);
	if (dev->block_cfg_access)
		locked = false;			/* [한국어] 남이 이미 잠갔다 — 건드리지 않고 물러난다 */
	else
		dev->block_cfg_access = 1;	/* [한국어] 비어 있었다 — 내가 잠근다.
						 * 검사와 설정이 같은 락 안에 있어야 두 CPU 가
						 * 동시에 "내가 잠갔다" 고 믿는 일이 없다 */
	raw_spin_unlock_irqrestore(&pci_lock, flags);

	return locked;	/* [한국어] 호출자는 이 값이 true 일 때만 unlock 을 부를 책임을 진다 */
}
EXPORT_SYMBOL_GPL(pci_cfg_access_trylock);

/**
 * pci_cfg_access_unlock - Unlock PCI config reads/writes
 * @dev:	pci device struct
 *
 * This function allows PCI config accesses to resume.
 */
/*
 * [한국어]
 * pci_cfg_access_unlock - 잠금을 풀고 기다리던 접근들을 깨운다
 *
 * @dev: 풀 장치
 * @return: 없음.
 *
 * block_cfg_access 를 0 으로 되돌리고, pci_cfg_wait 대기열에 잠든 태스크를
 * 모두 깨운다. 반드시 pci_cfg_access_lock() 이나, true 를 돌려받은
 * pci_cfg_access_trylock() 과 짝을 이뤄야 한다.
 *
 * 순서가 중요하다: 플래그를 0 으로 만든 뒤 락을 놓고, 그 다음에 깨운다.
 * 깨우기를 락 안에서 하면 깨어난 태스크가 곧바로 같은 락을 잡으려다
 * 튕겨 나가 불필요하게 회전한다. 반대로 플래그보다 먼저 깨우면 깨어난
 * 태스크가 여전히 1인 플래그를 보고 도로 잠들어 버린다.
 *
 * 실행 컨텍스트: 어디서나(irqsave 판). wake_up_all 은 잠들지 않는다.
 * 호출자: lock/trylock 을 부른 바로 그 코드. 리셋 완료 지점.
 *
 * 호출 체인:
 *   pci_reset_function -> ... -> [pci_cfg_access_unlock] -> wake_up_all -> pci_wait_cfg 가 깨어남
 */
void pci_cfg_access_unlock(struct pci_dev *dev)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */

	raw_spin_lock_irqsave(&pci_lock, flags);

	/*
	 * This indicates a problem in the caller, but we don't need
	 * to kill them, unlike a double-block above.
	 */
	/* [한국어] 잠근 적 없는데 푸는 것은 호출자의 짝 맞추기 오류다. 다만
	 * 시스템을 죽일 만한 일은 아니다 — 이미 0 인 것을 0 으로 만드는 것뿐이고,
	 * 최악의 결과는 남의 잠금을 대신 풀어 주는 정도다. 그래서 BUG_ON 이 아니라
	 * WARN_ON 으로 스택 트레이스만 남기고 계속 진행한다. 위 원문 주석의
	 * "double-block" 은 이미 잠긴 것을 또 잠그려는 경우를 가리키며,
	 * 그쪽은 pci_wait_cfg 에서 교착으로 이어져 더 치명적이다. */
	WARN_ON(!dev->block_cfg_access);

	dev->block_cfg_access = 0;	/* [한국어] 봉인 해제. 이후 들어오는 userspace 접근은 통과한다 */
	raw_spin_unlock_irqrestore(&pci_lock, flags);	/* [한국어] 깨우기 전에 먼저 놓는다 */

	/* [한국어] 대기열은 전역이므로 다른 장치를 기다리던 태스크까지 함께 깨어난다.
	 * 그들은 자기 dev->block_cfg_access 를 다시 확인하고 여전히 1이면 도로 잠든다.
	 * wake_up(하나만) 이 아니라 wake_up_all 인 이유가 여기 있다 — 하나만 깨우면
	 * 하필 다른 장치를 기다리던 태스크가 깨어나 도로 자 버리고, 정작 이 장치를
	 * 기다리던 태스크는 영원히 못 깨어날 수 있다. */
	wake_up_all(&pci_cfg_wait);
}
EXPORT_SYMBOL_GPL(pci_cfg_access_unlock);

/*
 * [한국어]
 * pcie_cap_version - 이 장치의 PCI Express Capability 구조 버전을 뽑아낸다
 *
 * @dev: 대상 장치 (PCIe 장치여야 한다)
 * @return: 버전 번호. 1 또는 2 이며, 2 이상이면 DEVCTL2/LNKCTL2 같은
 *          "2세대" 레지스터들이 존재한다.
 *
 * PCI Express Capability 구조는 시간이 지나며 뒤에 레지스터가 덧붙어 왔다.
 * 어디까지 있는지는 구조 맨 앞 PCI_EXP_FLAGS(오프셋 0x02) 의 하위 4비트에
 * 적혀 있고, 이 함수가 그 필드만 떼어 낸다. 이 값을 먼저 확인하지 않고
 * DEVCTL2 를 읽으면, 버전 1 장치에서는 그 자리에 아무것도 없어 쓰레기 값이나
 * 다른 capability 의 내용이 읽힌다.
 *
 * 왜 별도 함수인가: 아래 pcie_capability_reg_implemented() 의 여러 case 가
 * 이 판단을 반복해서 필요로 하기 때문이다. inline 이라 호출 비용은 없다.
 *
 * 실행 컨텍스트: 제약 없음. pcie_caps_reg() 는 하드웨어를 읽지 않고
 *   dev->pcie_flags_reg 라는 캐시된 값을 돌려주므로 락도 필요 없다.
 *   이 캐시는 열거 단계에서 pci_pcie_init 이 한 번 채운다.
 * 호출자: pcie_cap_has_lnkctl2(), pcie_capability_reg_implemented().
 *
 * 호출 체인:
 *   pcie_capability_read_word -> pcie_capability_reg_implemented -> [pcie_cap_version]
 */
static inline int pcie_cap_version(const struct pci_dev *dev)
{
	/* [한국어] PCI_EXP_FLAGS_VERS 는 0x000f — Capabilities Register 의 하위 4비트가
	 * Capability Version 필드다(PCIe Base Spec, PCI Express Capabilities Register).
	 * 나머지 비트는 Device/Port Type, Slot Implemented 등이라 마스크로 걸러낸다. */
	return pcie_caps_reg(dev) & PCI_EXP_FLAGS_VERS;
}

/*
 * [한국어]
 * pcie_cap_has_lnkctl - 이 function 이 Link Capability/Control/Status 를 갖는가
 *
 * @dev: 대상 장치
 * @return: true 면 LNKCAP/LNKCTL/LNKSTA 세 레지스터가 존재한다.
 *
 * PCIe function 은 종류에 따라 갖는 레지스터가 다르다. "링크" 레지스터는
 * 이름 그대로 실제 PCIe 링크의 한쪽 끝을 담당하는 function 만 갖는다.
 * 반대로 갖지 않는 대표적인 경우가 Root Complex Integrated Endpoint(RCiEP)와
 * Root Complex Event Collector 다 — 이들은 칩 내부에 박혀 있어 바깥으로
 * 나가는 링크가 아예 없으므로 링크 속도나 ASPM 을 말할 대상이 없다.
 *
 * NVMe 학습 관점: 카드 형태의 NVMe SSD 는 PCI_EXP_TYPE_ENDPOINT 라 여기서
 * true 가 된다. 그래서 ASPM(pcie/aspm.c)이 LNKCTL 을 읽고 써서 전력 상태를
 * 제어할 수 있고, 링크 속도(Gen3/Gen4/Gen5)와 폭(x4 등)을 LNKSTA 로 알 수 있다.
 * 반면 SoC 에 통합된 NVMe 컨트롤러가 RCiEP 로 노출되면 이 함수가 false 가 되고,
 * 링크 관련 기능은 전부 건너뛴다.
 *
 * 실행 컨텍스트: 제약 없음(캐시된 값만 읽는다).
 * 호출자: pcie_capability_reg_implemented(), pcie_cap_has_lnkctl2(),
 *   그리고 이 파일 밖의 링크 관련 코드.
 */
bool pcie_cap_has_lnkctl(const struct pci_dev *dev)
{
	/* [한국어] Capabilities Register 의 Device/Port Type 필드(4비트).
	 * 이 값 하나로 아래 판정이 전부 결정된다. */
	int type = pci_pcie_type(dev);

	/* [한국어] 아래 일곱 종류가 링크의 끝점을 갖는 function 이다.
	 * 여기 없는 것은 RCiEP(PCI_EXP_TYPE_RC_END)와 RC Event Collector(RC_EC)로,
	 * 둘 다 Root Complex 내부에 통합돼 바깥 링크가 없다. */
	return type == PCI_EXP_TYPE_ENDPOINT ||		/* [한국어] 일반 PCIe 엔드포인트 — NVMe SSD 카드가 여기 */
	       type == PCI_EXP_TYPE_LEG_END ||		/* [한국어] Legacy 엔드포인트 (PCI 호환 동작 모드) */
	       type == PCI_EXP_TYPE_ROOT_PORT ||	/* [한국어] Root Port — NVMe 가 꽂히는 슬롯의 상위 포트 */
	       type == PCI_EXP_TYPE_UPSTREAM ||		/* [한국어] 스위치의 상류 포트 */
	       type == PCI_EXP_TYPE_DOWNSTREAM ||	/* [한국어] 스위치의 하류 포트 */
	       type == PCI_EXP_TYPE_PCI_BRIDGE ||	/* [한국어] PCIe -> PCI/PCI-X 브리지 */
	       type == PCI_EXP_TYPE_PCIE_BRIDGE;	/* [한국어] PCI/PCI-X -> PCIe 브리지 */
}

/*
 * [한국어]
 * pcie_cap_has_lnkctl2 - 2세대 링크 레지스터(LNKCAP2/LNKCTL2/LNKSTA2)가 있는가
 *
 * @dev: 대상 장치
 * @return: true 면 LNKCAP2/LNKCTL2/LNKSTA2 가 존재한다.
 *
 * 조건이 둘 다 필요하다는 점이 핵심이다. 링크 레지스터를 갖는 종류여야 하고
 * (그래야 링크라는 것이 존재하고), 동시에 Capability 구조가 버전 2 이상이어야
 * 한다(그래야 구조 안에 그 자리가 실제로 있다). 하나만 만족해서는 안 된다.
 *
 * 이 2세대 레지스터들은 PCIe Gen2 에서 링크 속도를 소프트웨어가 지정할 수
 * 있게 되면서 추가됐다. LNKCTL2 의 Target Link Speed 필드로 "이 속도로
 * 재협상하라" 고 지시할 수 있고, 링크가 불안정할 때 커널이 속도를 낮추는
 * 복구 동작이 이 레지스터를 쓴다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pcie_capability_reg_implemented(), 링크 속도 제어 코드.
 */
bool pcie_cap_has_lnkctl2(const struct pci_dev *dev)
{
	/* [한국어] 두 조건의 AND. && 는 단축 평가라 링크가 없는 장치에서는
	 * 버전 확인조차 하지 않는다. */
	return pcie_cap_has_lnkctl(dev) && pcie_cap_version(dev) > 1;
}

/*
 * [한국어]
 * pcie_cap_has_sltctl - Slot Capability/Control/Status 레지스터가 있는가
 *
 * @dev: 대상 장치
 * @return: true 면 SLTCAP/SLTCTL/SLTSTA 가 존재한다.
 *
 * "슬롯" 은 물리적으로 카드를 꽂을 수 있는 자리를 뜻한다. 그러므로 이
 * 레지스터들은 하류 포트(Root Port 또는 스위치의 Downstream Port)이면서,
 * 그 포트에 실제로 커넥터가 달려 있다고 펌웨어가 표시한 경우에만 존재한다.
 * 두 조건을 각각 확인하는 이유가 여기 있다 — 하류 포트라도 보드에 칩이
 * 납땜돼 있으면 슬롯이 아니다.
 *
 * 이 레지스터들이 하는 일이 곧 hotplug 다. Presence Detect(카드가 꽂혔는가),
 * Attention/Power Indicator(표시등), Power Controller(슬롯 전원 On/Off),
 * Manually-operated Retention Latch(걸쇠 상태) 가 여기 모여 있다.
 * NVMe 학습 관점에서는 U.2/EDSFF 백플레인의 핫스왑이 이 레지스터로 이뤄진다 —
 * 드라이브를 뽑으면 Presence Detect Changed 인터럽트가 나고, pciehp 가 그것을
 * 받아 nvme 드라이버의 remove 를 부른다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pcie_capability_reg_implemented().
 */
static inline bool pcie_cap_has_sltctl(const struct pci_dev *dev)
{
	/* [한국어] 조건 1 — Root Port 이거나 스위치의 Downstream Port 여야 한다.
	 * 엔드포인트나 상류 포트에는 슬롯이라는 개념이 없다. */
	return pcie_downstream_port(dev) &&
	       /* [한국어] 조건 2 — Capabilities Register 의 Slot Implemented 비트(0x0100).
		* 하드웨어가 "이 포트에는 물리 슬롯이 달려 있다" 고 스스로 밝힌 것이다.
		* & 가 && 보다 우선순위가 낮아 보이지만 실제로는 && 가 더 낮으므로
		* 이 식은 (하류포트) && (caps & SLOT) 으로 묶인다. */
	       pcie_caps_reg(dev) & PCI_EXP_FLAGS_SLOT;
}

/*
 * [한국어]
 * pcie_cap_has_rtctl - Root Control/Capability/Status 레지스터가 있는가
 *
 * @dev: 대상 장치
 * @return: true 면 RTCTL/RTCAP/RTSTA 가 존재한다.
 *
 * "Root" 레지스터들은 계층 구조의 꼭대기에서만 의미가 있다. 아래로부터
 * 올라온 메시지 — 에러 보고(ERR_COR/ERR_NONFATAL/ERR_FATAL)와 전원 관리
 * 이벤트(PME) — 를 받아서 인터럽트로 바꿔 줄지 결정하는 것이 이 레지스터의
 * 역할이기 때문이다. 그 일은 Root Port 와 Root Complex Event Collector 만 한다.
 *
 * NVMe 학습 관점: NVMe 장치가 정정 불가능한 에러를 만나면 ERR_FATAL 메시지를
 * 상류로 보내고, 그것을 받은 Root Port 가 RTCTL 설정에 따라 AER 인터럽트를
 * 올린다. 그 인터럽트가 drivers/pci/pcie/aer.c 로 들어와 복구 절차가 시작되고,
 * 결국 NVMe 드라이버의 error_detected/slot_reset 콜백이 불린다.
 * 또 D3 에 들어간 NVMe 가 깨어나야 할 때 보내는 PME 메시지도 이 경로를 탄다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: pcie_capability_reg_implemented().
 */
bool pcie_cap_has_rtctl(const struct pci_dev *dev)
{
	int type = pci_pcie_type(dev);	/* [한국어] Device/Port Type 필드 */

	return type == PCI_EXP_TYPE_ROOT_PORT ||	/* [한국어] Root Port — 계층의 꼭대기 */
	       type == PCI_EXP_TYPE_RC_EC;		/* [한국어] Root Complex Event Collector —
							 * RCiEP 들이 링크가 없어 스스로 에러/PME 를
							 * 보고할 수 없으므로, 그들을 대신해 모아
							 * 보고해 주는 전용 function */
}

/*
 * [한국어]
 * pcie_capability_reg_implemented - 이 장치에 그 레지스터가 실제로 존재하는가
 *
 * @dev: 대상 장치
 * @pos: PCI Express Capability 구조 안에서의 상대 오프셋 (PCI_EXP_DEVCTL 등).
 *       config space 절대 오프셋이 아니라는 점에 주의 — 절대 위치는 장치마다
 *       다르고, 실제 접근 시 pci_pcie_cap(dev) 을 더해서 구한다.
 * @return: true 면 읽고 써도 되는 레지스터, false 면 존재하지 않는다.
 *
 * 이 파일에서 가장 중요한 판정 함수다. PCI Express Capability 구조는 고정
 * 길이가 아니라, function 의 종류(엔드포인트/포트/브리지)와 Capability 버전에
 * 따라 어느 레지스터가 존재하는지가 달라진다. 존재하지 않는 자리를 읽으면
 * 그 뒤에 이어진 다른 capability 의 내용이나 정의되지 않은 값이 나오고,
 * 쓰면 남의 레지스터를 망가뜨린다.
 *
 * 그래서 pcie_capability_read/write_* 는 항상 이 함수를 먼저 통과시킨다.
 * 덕분에 호출하는 쪽은 "이 장치가 Root Port 인가, Capability 버전이 몇인가"를
 * 하나하나 따지지 않고 그냥 원하는 레지스터를 요청하면 된다 — 없으면
 * 읽기는 0 을 돌려주고 쓰기는 조용히 무시된다.
 *
 * 판정 근거는 PCIe Base Spec 의 PCI Express Capability Structure 정의다.
 * 크게 네 갈래로 갈린다: (1) 항상 존재, (2) 링크가 있어야 존재,
 * (3) 슬롯이 있어야 존재, (4) 계층 꼭대기여야 존재, 그리고 여기에
 * "버전 2 이상이어야 존재" 가 곱해진다.
 *
 * 실행 컨텍스트: 제약 없음(캐시된 값만 본다).
 * 호출자: pcie_capability_read_word/dword, pcie_capability_write_word/dword.
 *
 * 호출 체인:
 *   pcie_capability_read_word -> [pcie_capability_reg_implemented] -> pcie_cap_has_lnkctl 등
 */
static bool pcie_capability_reg_implemented(struct pci_dev *dev, int pos)
{
	/* [한국어] PCI Express Capability 자체가 없는 장치 — 순수 PCI/PCI-X 장치 —
	 * 라면 어떤 오프셋도 의미가 없다. pci_is_pcie 는 dev->pcie_cap 이 0 이 아닌지를
	 * 본다(열거 단계에서 capability 목록을 훑어 채워 둔 값). */
	if (!pci_is_pcie(dev))
		return false;

	switch (pos) {
	/* [한국어] Capabilities Register(0x02). 구조의 머리이므로 PCIe 장치라면
	 * 무조건 존재한다. 나머지 판정의 근거가 되는 버전/타입 정보가 여기 들어 있다. */
	case PCI_EXP_FLAGS:
		return true;
	/* [한국어] Device 계열(0x04/0x08/0x0a)도 모든 PCIe function 에 존재한다.
	 * DEVCTL 에는 Max Payload Size, Max Read Request Size, Relaxed Ordering,
	 * No Snoop 같은 것들이 있어 NVMe 의 DMA 성능에 직접 영향을 준다. */
	case PCI_EXP_DEVCAP:
	case PCI_EXP_DEVCTL:
	case PCI_EXP_DEVSTA:
		return true;
	/* [한국어] Link 계열(0x0c/0x10/0x12) — 실제 링크의 끝점인 function 만 갖는다.
	 * LNKCTL 이 ASPM L0s/L1 제어를, LNKSTA 가 현재 협상된 속도와 폭을 담는다. */
	case PCI_EXP_LNKCAP:
	case PCI_EXP_LNKCTL:
	case PCI_EXP_LNKSTA:
		return pcie_cap_has_lnkctl(dev);
	/* [한국어] Slot 계열(0x14/0x18/0x1a) — 물리 슬롯이 달린 하류 포트만 갖는다.
	 * hotplug 의 근간이다. */
	case PCI_EXP_SLTCAP:
	case PCI_EXP_SLTCTL:
	case PCI_EXP_SLTSTA:
		return pcie_cap_has_sltctl(dev);
	/* [한국어] Root 계열(0x1c/0x1e/0x20) — Root Port 와 RC Event Collector 만 갖는다.
	 * 아래에서 올라온 에러/PME 메시지를 인터럽트로 바꿀지 여기서 정한다. */
	case PCI_EXP_RTCTL:
	case PCI_EXP_RTCAP:
	case PCI_EXP_RTSTA:
		return pcie_cap_has_rtctl(dev);
	/* [한국어] Device 2세대(0x24/0x28) — Capability 버전 2 이상에서만 존재한다.
	 * 종류 제한은 없다(모든 function 이 갖는다). DEVCTL2 에 ARI Forwarding,
	 * AtomicOp Requester Enable, LTR Enable, 그리고 Completion Timeout 설정이 있다. */
	case PCI_EXP_DEVCAP2:
	case PCI_EXP_DEVCTL2:
		return pcie_cap_version(dev) > 1;
	/* [한국어] Link 2세대(0x2c/0x30/0x32) — 링크가 있고 버전도 2 이상이어야 한다.
	 * 두 조건을 모두 확인하는 것이 pcie_cap_has_lnkctl2 다. */
	case PCI_EXP_LNKCAP2:
	case PCI_EXP_LNKCTL2:
	case PCI_EXP_LNKSTA2:
		return pcie_cap_has_lnkctl2(dev);
	/* [한국어] 목록에 없는 오프셋은 모두 "없다" 로 본다. 보수적인 기본값이다 —
	 * 잘못 허용해 엉뚱한 레지스터를 건드리는 것보다, 잘못 막아 기능이 동작하지
	 * 않는 편이 낫다(후자는 금방 눈에 띄지만 전자는 조용히 하드웨어를 망친다).
	 * Slot 2세대(SLTCTL2)와 Root 2세대(RTSTA2) 가 목록에 없는 것은 스펙에서
	 * 그 자리가 예약(RsvdP)으로 남아 실제 정의가 없기 때문이다. */
	default:
		return false;
	}
}

/*
 * Note that these accessor functions are only for the "PCI Express
 * Capability" (see PCIe spec r3.0, sec 7.8).  They do not apply to the
 * other "PCI Express Extended Capabilities" (AER, VC, ACS, MFVC, etc.)
 */

/*
 * [한국어]
 * pcie_capability_read_word - PCI Express Capability 안의 16비트 레지스터를 읽는다
 *
 * @dev: 대상 장치
 * @pos: Capability 구조 안에서의 상대 오프셋 (PCI_EXP_LNKCTL 등)
 * @val: 결과를 담을 곳. 어떤 경로로 빠져나가든 반드시 채워진다.
 * @return: 0 = 성공(존재하지 않는 레지스터를 요청한 경우도 0),
 *          PCIBIOS_BAD_REGISTER_NUMBER = 정렬 오류,
 *          그 외 PCIBIOS_* = 하드웨어 접근 실패.
 *
 * 이 함수의 계약이 독특하다. "존재하지 않는 레지스터" 를 에러로 보지 않고
 * 0 을 채워 성공으로 돌려준다. 스펙이 그렇게 정하고 있기 때문이다 —
 * 구현하지 않은 레지스터 자리는 0 으로 하드와이어돼야 한다. 그래서 호출자는
 * 장치 종류를 따지지 않고 그냥 읽으면 되고, 없는 기능은 자연히 0 으로 보인다.
 *
 * 값을 채우는 규칙이 세 겹이다:
 *   1) 함수 진입 즉시 *val = 0. 어떤 이유로 빠져나가도 미초기화 값이 새지 않는다.
 *   2) 하드웨어 읽기가 실패하면 다시 0 으로 되돌린다. 실패 시 하위 계층이
 *      all-ones(0xFFFF)를 채워 넣기 때문이다 — 그 값을 그대로 두면 호출자가
 *      "모든 기능이 켜져 있다" 고 오해한다. 여기서는 all-ones 관용구가
 *      오히려 해가 되므로 0 으로 덮는다.
 *   3) 존재하지 않는 레지스터면 0 을 유지하되, 딱 한 가지 예외를 적용한다
 *      (아래 Presence Detect 주석 참고).
 *
 * NVMe 학습 관점: NVMe 장치의 LNKSTA 를 읽어 현재 링크 속도와 폭을 알아내는
 * 것이 이 경로다. 다만 앞선 검증대로 NVMe 드라이버가 직접 부르지는 않고,
 * pcie/aspm.c 와 pci-sysfs.c 가 대신 부른다(sysfs 의 current_link_speed 등).
 *
 * 실행 컨텍스트: 하위 pci_read_config_word 가 pci_lock 을 잡으므로 인터럽트
 *   문맥에서도 안전하다.
 *
 * 호출 체인:
 *   pcie_aspm_init_link_state -> [pcie_capability_read_word] -> pci_read_config_word
 */
int pcie_capability_read_word(struct pci_dev *dev, int pos, u16 *val)
{
	int ret;	/* [한국어] 하위 config 읽기의 PCIBIOS_* 결과 */

	/* [한국어] 규칙 1 — 무조건 먼저 0 으로. 아래 어느 return 을 타든
	 * 호출자가 쓰레기 값을 보는 일이 없다. */
	*val = 0;
	/* [한국어] 16비트 접근이므로 짝수 오프셋이어야 한다. Capability 구조 안의
	 * 상대 오프셋도 절대 오프셋과 같은 정렬 규칙을 따른다(구조 자체가 dword
	 * 정렬된 자리에 놓이기 때문에 상대 오프셋의 정렬이 곧 절대 정렬이다). */
	if (pos & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 존재하는 레지스터인 경우에만 실제 하드웨어를 건드린다. */
	if (pcie_capability_reg_implemented(dev, pos)) {
		/* [한국어] pci_pcie_cap(dev) 이 이 장치의 PCI Express Capability 가
		 * config space 어디에 놓였는지(절대 오프셋)를 준다. 장치마다 다르며,
		 * 열거 단계에서 capability 연결 리스트를 따라가 찾아 캐시해 둔 값이다.
		 * 거기에 상대 오프셋을 더해야 최종 위치가 된다. */
		ret = pci_read_config_word(dev, pci_pcie_cap(dev) + pos, val);
		/*
		 * Reset *val to 0 if pci_read_config_word() fails; it may
		 * have been written as 0xFFFF (PCI_ERROR_RESPONSE) if the
		 * config read failed on PCI.
		 */
		/* [한국어] 규칙 2 — 실패 시 all-ones 를 0 으로 덮는다. 저수준 계층은
		 * "장치 없음" 을 all-ones 로 표현하지만, capability 레지스터에서는
		 * 그 값이 곧 "모든 비트가 1" 이라는 의미 있는 값으로 읽히므로
		 * 위험하다. 여기서는 0(=기능 없음)이 안전한 기본값이다. */
		if (ret)
			*val = 0;
		return ret;
	}

	/*
	 * For Functions that do not implement the Slot Capabilities,
	 * Slot Status, and Slot Control registers, these spaces must
	 * be hardwired to 0b, with the exception of the Presence Detect
	 * State bit in the Slot Status register of Downstream Ports,
	 * which must be hardwired to 1b.  (PCIe Base Spec 3.0, sec 7.8)
	 */
	/* [한국어] 규칙 3 의 예외. 슬롯 레지스터를 구현하지 않은 하류 포트라도
	 * Slot Status 의 Presence Detect State 비트만은 1 로 하드와이어돼야 한다고
	 * 스펙이 정한다. 이유는 이렇다 — 슬롯 레지스터가 없다는 것은 커넥터가
	 * 없다는 뜻이고, 커넥터가 없다면 장치는 보드에 납땜돼 있어 "항상 존재" 한다.
	 * 그러니 존재 감지 비트는 영원히 1 이어야 논리적으로 맞다.
	 *
	 * 이 흉내를 소프트웨어가 대신 내주지 않으면, hotplug 코드가 이런 포트에서
	 * "카드가 빠졌다" 고 판단해 멀쩡히 동작 중인 장치를 제거해 버린다. */
	if (pci_is_pcie(dev) && pcie_downstream_port(dev) &&
	    pos == PCI_EXP_SLTSTA)
		*val = PCI_EXP_SLTSTA_PDS;

	/* [한국어] 존재하지 않는 레지스터를 읽은 것도 성공이다. 값은 0(또는 위
	 * 예외에 해당하면 PDS 비트만 1)이고, 그것이 스펙이 정한 올바른 결과다. */
	return 0;
}
EXPORT_SYMBOL(pcie_capability_read_word);

/*
 * [한국어]
 * pcie_capability_read_dword - PCI Express Capability 안의 32비트 레지스터를 읽는다
 *
 * @dev: 대상 장치
 * @pos: Capability 구조 안에서의 상대 오프셋 (PCI_EXP_DEVCAP, PCI_EXP_LNKCAP 등)
 * @val: 결과를 담을 곳
 * @return: word 판과 동일한 규약.
 *
 * word 판의 32비트 버전이며 계약도 완전히 같다 — 없는 레지스터는 에러가 아니라
 * 0, 실패 시 all-ones 를 0 으로 덮기, 진입 즉시 초기화.
 *
 * NVMe 학습 관점에서 중요한 레지스터가 이 폭에 몰려 있다:
 *   - DEVCAP(0x04): Max Payload Size Supported, 그리고 Function Level Reset
 *     지원 여부. NVMe 컨트롤러 리셋 시 FLR 을 쓸 수 있는지가 여기서 결정된다.
 *   - LNKCAP(0x0c): 이 링크가 낼 수 있는 최대 속도와 폭. NVMe SSD 의
 *     "Gen4 x4" 같은 사양이 이 레지스터에 적혀 있다.
 *   - DEVCAP2(0x24): AtomicOp 지원 여부, LTR 지원 여부.
 *
 * 실행 컨텍스트: word 판과 동일.
 *
 * 호출 체인:
 *   pcie_get_speed_cap -> [pcie_capability_read_dword] -> pci_read_config_dword
 */
int pcie_capability_read_dword(struct pci_dev *dev, int pos, u32 *val)
{
	int ret;	/* [한국어] 하위 config 읽기의 결과 */

	*val = 0;	/* [한국어] 어떤 경로로 나가도 미초기화 값이 새지 않게 */
	/* [한국어] 32비트 접근이므로 4의 배수 오프셋이어야 한다. */
	if (pos & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	if (pcie_capability_reg_implemented(dev, pos)) {
		/* [한국어] Capability 절대 위치 + 상대 오프셋. */
		ret = pci_read_config_dword(dev, pci_pcie_cap(dev) + pos, val);
		/*
		 * Reset *val to 0 if pci_read_config_dword() fails; it may
		 * have been written as 0xFFFFFFFF (PCI_ERROR_RESPONSE) if
		 * the config read failed on PCI.
		 */
		/* [한국어] 실패 시 0xFFFFFFFF 를 0 으로 덮는다. word 판과 같은 이유다. */
		if (ret)
			*val = 0;
		return ret;
	}

	/* [한국어] word 판과 같은 Presence Detect 예외.
	 * PCI_EXP_SLTSTA(0x1a)는 원래 16비트 레지스터라 dword 로 읽을 일이 없어
	 * 보이지만(그리고 0x1a 는 4의 배수가 아니어서 위 정렬 검사에 걸린다),
	 * 두 함수의 동작을 똑같이 유지해 두는 편이 나중에 어긋날 여지를 없앤다. */
	if (pci_is_pcie(dev) && pcie_downstream_port(dev) &&
	    pos == PCI_EXP_SLTSTA)
		*val = PCI_EXP_SLTSTA_PDS;

	return 0;	/* [한국어] 없는 레지스터를 읽은 것도 성공 */
}
EXPORT_SYMBOL(pcie_capability_read_dword);

/*
 * [한국어]
 * pcie_capability_write_word - PCI Express Capability 안의 16비트 레지스터에 쓴다
 *
 * @dev: 대상 장치
 * @pos: Capability 구조 안에서의 상대 오프셋
 * @val: 쓸 값
 * @return: 0 = 성공(존재하지 않아 아무것도 하지 않은 경우 포함),
 *          PCIBIOS_BAD_REGISTER_NUMBER = 정렬 오류, 그 외 = 하드웨어 접근 실패.
 *
 * 읽기 판과 대칭이다. 존재하지 않는 레지스터에 쓰려 하면 에러가 아니라
 * 조용히 무시하고 0 을 돌려준다. 스펙상 그 자리는 하드와이어된 0 이라
 * 쓰기가 아무 효과를 내지 못하는 것이 정상 동작이기 때문이다.
 *
 * 이 "조용한 무시" 는 의도된 설계다. 예컨대 ASPM 코드가 모든 장치에 대해
 * 일괄적으로 LNKCTL 을 설정하려 할 때, 링크가 없는 RCiEP 에서 에러가 나면
 * 상위 코드가 그것을 실패로 처리해 초기화 전체가 중단될 수 있다. 대신
 * 아무 일도 일어나지 않게 두는 편이 맞다.
 *
 * 실행 컨텍스트: 하위 pci_write_config_word 가 락을 잡는다.
 * 호출자: pcie/aspm.c 의 링크 제어, pcie_capability_clear_and_set_word_unlocked.
 *
 * 호출 체인:
 *   pcie_config_aspm_link -> [pcie_capability_write_word] -> pci_write_config_word
 */
int pcie_capability_write_word(struct pci_dev *dev, int pos, u16 val)
{
	/* [한국어] 16비트 쓰기이므로 짝수 오프셋. 정렬을 어기면 인접 레지스터까지
	 * 함께 갈아엎을 수 있어 읽기보다 위험하다. */
	if (pos & 1)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 존재하지 않으면 하드웨어를 건드리지 않고 성공으로 돌아간다.
	 * 여기서 막지 않으면 Capability 구조 뒤에 이어진 전혀 다른 capability 의
	 * 레지스터를 덮어쓰게 된다 — 조용히 시스템을 망가뜨리는 종류의 버그다. */
	if (!pcie_capability_reg_implemented(dev, pos))
		return 0;

	/* [한국어] Capability 절대 위치 + 상대 오프셋으로 실제 쓰기. */
	return pci_write_config_word(dev, pci_pcie_cap(dev) + pos, val);
}
EXPORT_SYMBOL(pcie_capability_write_word);

/*
 * [한국어]
 * pcie_capability_write_dword - PCI Express Capability 안의 32비트 레지스터에 쓴다
 *
 * @dev: 대상 장치
 * @pos: Capability 구조 안에서의 상대 오프셋
 * @val: 쓸 값
 * @return: word 판과 동일한 규약.
 *
 * word 판의 32비트 버전. 계약과 이유가 모두 같다.
 *
 * 실행 컨텍스트: 하위 pci_write_config_dword 가 락을 잡는다.
 * 호출자: pcie_capability_clear_and_set_dword 등.
 */
int pcie_capability_write_dword(struct pci_dev *dev, int pos, u32 val)
{
	/* [한국어] 32비트 쓰기이므로 4의 배수 오프셋. */
	if (pos & 3)
		return PCIBIOS_BAD_REGISTER_NUMBER;

	/* [한국어] 없는 레지스터면 조용히 성공. */
	if (!pcie_capability_reg_implemented(dev, pos))
		return 0;

	/* [한국어] Capability 절대 위치 + 상대 오프셋으로 실제 쓰기. */
	return pci_write_config_dword(dev, pci_pcie_cap(dev) + pos, val);
}
EXPORT_SYMBOL(pcie_capability_write_dword);

/*
 * [한국어]
 * pcie_capability_clear_and_set_word_unlocked - 일부 비트만 골라 바꾼다 (락 없는 판)
 *
 * @dev:   대상 장치
 * @pos:   Capability 구조 안에서의 상대 오프셋
 * @clear: 0 으로 만들 비트들의 마스크
 * @set:   1 로 만들 비트들의 마스크
 * @return: 0 = 성공, 그 외 = 읽기 또는 쓰기 실패.
 *
 * config 레지스터는 여러 기능의 비트가 한 워드에 섞여 있다. 그중 하나만
 * 바꾸려면 읽어서 → 고쳐서 → 다시 쓰는 read-modify-write 가 필요하다.
 * 그 세 단계를 매번 손으로 쓰면 마스크 실수로 남의 비트를 지우기 쉬워서
 * 이렇게 한 함수로 묶어 두었다.
 *
 * clear 를 먼저 적용하고 set 을 나중에 적용한다. 그래서 같은 비트가 양쪽에
 * 들어 있으면 최종 결과는 1 이다 — set 이 이긴다. 이 순서는 규약이므로
 * 호출자가 기대해도 된다.
 *
 * 이름의 "unlocked" 가 뜻하는 것: 이 함수는 dev->pcie_cap_lock 을 잡지 않는다.
 * 따라서 읽기와 쓰기 사이에 다른 CPU 가 같은 레지스터를 바꾸면 그 변경이
 * 사라진다(lost update). 그래도 이 판이 존재하는 이유는 두 가지다.
 *   - 이미 그 락을 쥐고 있는 호출자가 재귀 교착에 빠지지 않도록.
 *   - 초기화 단계처럼 경쟁이 원천적으로 없는 곳에서 락 비용을 피하려고.
 * 경쟁 가능성이 조금이라도 있으면 아래 _locked 판을 써야 한다.
 *
 * 실행 컨텍스트: 제약 없음. 다만 원자성이 없다는 사실을 호출자가 알고 있어야 한다.
 * 호출자: 이미 pcie_cap_lock 을 쥔 코드, 그리고 _locked 판 자신.
 *
 * 호출 체인:
 *   [pcie_capability_clear_and_set_word_unlocked]
 *     -> pcie_capability_read_word -> pcie_capability_write_word
 */
int pcie_capability_clear_and_set_word_unlocked(struct pci_dev *dev, int pos,
						u16 clear, u16 set)
{
	int ret;	/* [한국어] 읽기/쓰기 결과 */
	u16 val;	/* [한국어] 현재 값을 담아 고칠 작업 변수 */

	/* [한국어] 1단계 — 현재 값을 읽는다. 없는 레지스터라면 0 이 들어오고
	 * 아래 계산은 무의미해지지만, 어차피 쓰기도 무시되므로 문제되지 않는다. */
	ret = pcie_capability_read_word(dev, pos, &val);
	if (ret)
		return ret;	/* [한국어] 읽지 못했으면 쓰지 않는다. 모르는 값 위에
				 * 덮어쓰면 건드리면 안 될 비트까지 바꾸게 된다 */

	/* [한국어] 2단계 — 지울 비트를 내린다. ~clear 는 clear 의 반대이므로,
	 * AND 하면 clear 에 표시된 자리만 0 이 되고 나머지는 그대로 남는다. */
	val &= ~clear;
	/* [한국어] 3단계 — 세울 비트를 올린다. OR 이므로 set 에 표시된 자리만
	 * 1 이 되고 나머지는 유지된다. clear 뒤에 오므로 겹치면 set 이 이긴다. */
	val |= set;
	/* [한국어] 4단계 — 합성된 값을 다시 쓴다. */
	return pcie_capability_write_word(dev, pos, val);
}
EXPORT_SYMBOL(pcie_capability_clear_and_set_word_unlocked);

/*
 * [한국어]
 * pcie_capability_clear_and_set_word_locked - 일부 비트만 골라 바꾼다 (락 있는 판)
 *
 * @dev:   대상 장치
 * @pos:   Capability 구조 안에서의 상대 오프셋
 * @clear: 0 으로 만들 비트 마스크
 * @set:   1 로 만들 비트 마스크
 * @return: 0 = 성공, 그 외 = 실패.
 *
 * unlocked 판을 dev->pcie_cap_lock 으로 감싼 것이 전부다. 이 락이 read-modify-write
 * 전체를 하나의 원자적 동작으로 만든다.
 *
 * 왜 pci_lock 으로는 부족한가: pci_lock 은 config 접근 "한 번" 을 보호할 뿐이다.
 * 읽기가 끝나 락이 풀린 순간부터 쓰기가 락을 잡기 전까지의 틈은 보호하지 못한다.
 * 그 틈에 다른 CPU 가 같은 레지스터의 다른 비트를 바꿔 놓으면, 이쪽이 오래된
 * 값을 바탕으로 합성한 값을 써 넣으면서 그 변경을 지워 버린다.
 *
 * 실제로 이런 경쟁이 일어나는 곳이 LNKCTL 이다. ASPM 정책 변경, 링크 재훈련,
 * Clock Power Management 설정이 모두 같은 레지스터의 서로 다른 비트를 건드리고,
 * 이들은 서로 다른 문맥(sysfs 쓰기, hotplug 워커, 전원 관리)에서 동시에 올 수 있다.
 * 그래서 이 락은 장치별(struct pci_dev 안)로 존재한다 — 전역일 필요는 없고,
 * 경쟁은 같은 장치의 같은 레지스터에서만 일어나기 때문이다.
 *
 * 실행 컨텍스트: irqsave 판이라 인터럽트 문맥에서도 안전하다.
 * 호출자: pcie_capability_clear_and_set_word() 매크로가 pos 에 따라
 *   이 판과 unlocked 판 중 하나를 고른다(LNKCTL 처럼 경쟁이 있는 곳만 locked).
 *
 * 호출 체인:
 *   pcie_capability_clear_and_set_word -> [.._locked] -> [.._unlocked] -> read/write
 */
int pcie_capability_clear_and_set_word_locked(struct pci_dev *dev, int pos,
					      u16 clear, u16 set)
{
	unsigned long flags;	/* [한국어] 인터럽트 상태 저장용 */
	int ret;		/* [한국어] RMW 결과 */

	/* [한국어] 이 장치 전용 락. 읽기와 쓰기 사이의 틈까지 함께 막는다.
	 * irqsave 판인 것은 인터럽트 문맥의 링크 이벤트 처리에서도 불리기 때문이다. */
	spin_lock_irqsave(&dev->pcie_cap_lock, flags);
	/* [한국어] 실제 작업은 unlocked 판에 위임한다. 두 판이 로직을 공유하므로
	 * 마스크 처리 규칙이 어긋날 여지가 없다. */
	ret = pcie_capability_clear_and_set_word_unlocked(dev, pos, clear, set);
	spin_unlock_irqrestore(&dev->pcie_cap_lock, flags);

	return ret;
}
EXPORT_SYMBOL(pcie_capability_clear_and_set_word_locked);

/*
 * [한국어]
 * pcie_capability_clear_and_set_dword - 32비트 capability 레지스터의 일부 비트만 바꾼다
 *
 * @dev:   대상 장치
 * @pos:   Capability 구조 안에서의 상대 오프셋
 * @clear: 0 으로 만들 비트 마스크
 * @set:   1 로 만들 비트 마스크
 * @return: 0 = 성공, 그 외 = 읽기/쓰기 실패.
 *
 * word 판의 32비트 버전이다. 다만 word 와 달리 locked/unlocked 두 벌이 없고
 * 이것 하나뿐이다 — 32비트 capability 레지스터 중 여러 문맥이 동시에 건드리는
 * 것이 없어 락이 필요한 경우가 생기지 않았기 때문이다.
 *
 * clear 먼저, set 나중이라는 순서 규약은 word 판과 같다.
 *
 * 실행 컨텍스트: 제약 없음. 다만 원자적이지 않다.
 * 호출자: DEVCTL2 의 Completion Timeout 설정, LTR/OBFF 제어 등.
 *
 * 호출 체인:
 *   pci_enable_ltr -> [pcie_capability_clear_and_set_dword]
 *     -> pcie_capability_read_dword -> pcie_capability_write_dword
 */
int pcie_capability_clear_and_set_dword(struct pci_dev *dev, int pos,
					u32 clear, u32 set)
{
	int ret;	/* [한국어] 읽기/쓰기 결과 */
	u32 val;	/* [한국어] 현재 값을 담아 고칠 작업 변수 */

	/* [한국어] 1단계 — 읽기. 실패하면 아예 쓰지 않는다. */
	ret = pcie_capability_read_dword(dev, pos, &val);
	if (ret)
		return ret;

	val &= ~clear;	/* [한국어] 2단계 — 지정된 비트를 내린다 */
	val |= set;	/* [한국어] 3단계 — 지정된 비트를 올린다 (겹치면 set 이 이긴다) */
	/* [한국어] 4단계 — 합성 결과를 되쓴다. */
	return pcie_capability_write_dword(dev, pos, val);
}
EXPORT_SYMBOL(pcie_capability_clear_and_set_dword);

/*
 * [한국어]
 * pci_read_config_byte / _word / _dword - 드라이버가 실제로 쓰는 읽기 진입점
 *
 * @dev:   struct pci_dev 포인터 (bus/devfn 을 여기서 꺼낸다)
 * @where: config space 절대 바이트 오프셋 (0~4095)
 * @val:   결과를 담을 곳
 * @return: PCIBIOS_SUCCESSFUL(0) 또는 PCIBIOS_* 오류 코드.
 *
 * 이 파일 앞쪽의 pci_bus_read_config_* 가 저수준 판이라면, 이 셋이 바깥에서
 * 부르는 얼굴이다. 하는 일은 두 가지 — (1) 장치가 이미 사라졌는지 확인하고,
 * (2) dev 에서 bus 와 devfn 을 꺼내 저수준 판에 넘긴다.
 *
 * disconnected 검사가 핵심이다. hotplug 로 장치를 뽑았거나 surprise removal 이
 * 일어나면 pci_dev_is_disconnected() 가 참이 된다. 이때 하드웨어 접근을
 * 시도하면 플랫폼에 따라 머신 체크 예외나 버스 오류로 커널이 죽을 수 있다.
 * 그래서 아예 버스에 닿기 전에 잘라낸다. 그러면서도 val 에는 all-ones 를
 * 채워, 이 사실을 모르는 호출자도 "0xFFFF 면 없는 것" 이라는 평소의 관용구로
 * 자연스럽게 판정하게 된다.
 *
 * NVMe 학습 관점: 앞선 전수 조사 결과, drivers/nvme/ 가 이 파일에서 직접
 * 부르는 함수는 pci_read_config_word() 하나뿐이었다. 나머지 접근은 모두
 * pci_enable_device_mem(), pci_save_state(), pci_set_master() 같은 상위 API 를
 * 거쳐 간접적으로 이뤄진다. NVMe 컨트롤러가 응답을 멈춰 nvme_timeout 이
 * 상황을 진단할 때, 여기서 all-ones 가 돌아오면 "장치가 사라졌다" 로,
 * 정상 값이 돌아오면 "장치는 살아 있는데 명령만 안 끝난다" 로 갈린다.
 *
 * 실행 컨텍스트: 하위 pci_lock 이 irqsave 판이라 인터럽트 문맥에서도 안전하다.
 * 피호출자: pci_bus_read_config_* (PCI_OP_READ 매크로가 만든 함수).
 *
 * 호출 체인:
 *   nvme_pci_enable -> [pci_read_config_word] -> pci_bus_read_config_word
 *     -> bus->ops->read
 */
int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val)
{
	/* [한국어] 장치가 이미 뽑혔거나 제거 중이면 하드웨어를 건드리지 않는다.
	 * 이 플래그는 hotplug/제거 경로가 세우며, 한번 서면 다시 내려가지 않는다. */
	if (pci_dev_is_disconnected(dev)) {
		PCI_SET_ERROR_RESPONSE(val);	/* [한국어] 무응답 장치와 같은 모습(all-ones)으로 만든다 */
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	/* [한국어] dev 에서 bus/devfn 을 꺼내 저수준 판에 넘긴다. 이 두 값을
	 * 매번 손으로 꺼내지 않아도 되게 하는 것이 이 래퍼의 편의 기능이다. */
	return pci_bus_read_config_byte(dev->bus, dev->devfn, where, val);
}
EXPORT_SYMBOL(pci_read_config_byte);

/* [한국어] word 판. NVMe 드라이버가 이 파일에서 직접 부르는 유일한 함수다.
 * Vendor ID, Device ID, Command, Status 가 모두 이 폭이라 가장 많이 쓰인다. */
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val)
{
	if (pci_dev_is_disconnected(dev)) {	/* [한국어] 사라진 장치 조기 차단 */
		PCI_SET_ERROR_RESPONSE(val);	/* [한국어] 0xFFFF 로 채운다 */
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	return pci_bus_read_config_word(dev->bus, dev->devfn, where, val);
}
EXPORT_SYMBOL(pci_read_config_word);

/* [한국어] dword 판. BAR(0x10~0x24), Class Code, 그리고 확장 capability 의
 * 레지스터들이 이 폭이다. NVMe 의 BAR0(컨트롤러 레지스터 창)를 읽는 것도
 * 결국 이 경로다 — 다만 드라이버가 직접 부르지 않고 pci_resource_start() 가
 * 열거 때 저장해 둔 값을 쓴다. */
int pci_read_config_dword(const struct pci_dev *dev, int where,
			  u32 *val)
{
	if (pci_dev_is_disconnected(dev)) {	/* [한국어] 사라진 장치 조기 차단 */
		PCI_SET_ERROR_RESPONSE(val);	/* [한국어] 0xFFFFFFFF 로 채운다 */
		return PCIBIOS_DEVICE_NOT_FOUND;
	}
	return pci_bus_read_config_dword(dev->bus, dev->devfn, where, val);
}
EXPORT_SYMBOL(pci_read_config_dword);

/*
 * [한국어]
 * pci_write_config_byte / _word / _dword - 드라이버가 실제로 쓰는 쓰기 진입점
 *
 * @dev:   struct pci_dev 포인터
 * @where: config space 절대 바이트 오프셋
 * @val:   쓸 값
 * @return: PCIBIOS_SUCCESSFUL(0) 또는 PCIBIOS_* 오류 코드.
 *
 * 읽기 판의 거울상이다. 다른 점은 하나뿐 — 실패 시 채워 줄 버퍼가 없으므로
 * PCI_SET_ERROR_RESPONSE 에 해당하는 처리가 없고, 결과를 알 방법은 반환값뿐이다.
 * 그래서 읽기와 달리 반환값을 무시하면 실패가 완전히 조용히 묻힌다.
 *
 * disconnected 검사가 읽기보다 더 중요하다. 사라진 장치에 쓰기를 내보내면
 * 응답 없는 트랜잭션이 되어, 플랫폼에 따라 Completion Timeout 이나 머신 체크로
 * 이어질 수 있다. 읽기는 all-ones 로 무해하게 끝나는 경우가 많지만 쓰기는
 * 그런 안전망이 없다.
 *
 * NVMe 학습 관점: NVMe 드라이버는 이 함수들을 직접 부르지 않는다. 대신
 * pci_enable_device_mem() 이 Command 레지스터의 Memory Space Enable 을,
 * pci_set_master() 가 Bus Master Enable 을 켜면서 내부적으로 이 경로를 탄다.
 * 두 비트가 없으면 NVMe 컨트롤러는 BAR 접근도, DMA 도 하지 못한다.
 *
 * 실행 컨텍스트: 하위 락이 irqsave 판이라 인터럽트 문맥에서도 안전하다.
 * 피호출자: pci_bus_write_config_* (PCI_OP_WRITE 매크로가 만든 함수).
 *
 * 호출 체인:
 *   nvme_pci_enable -> pci_set_master -> [pci_write_config_word]
 *     -> pci_bus_write_config_word -> bus->ops->write
 */
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val)
{
	/* [한국어] 사라진 장치에는 쓰지 않는다. 돌려줄 버퍼가 없으므로
	 * 호출자가 반환값을 봐야만 실패를 알 수 있다. */
	if (pci_dev_is_disconnected(dev))
		return PCIBIOS_DEVICE_NOT_FOUND;
	return pci_bus_write_config_byte(dev->bus, dev->devfn, where, val);
}
EXPORT_SYMBOL(pci_write_config_byte);

/* [한국어] word 판. Command 레지스터(0x04) 갱신이 가장 흔한 용례다 —
 * Memory Space Enable, Bus Master Enable, INTx Disable 이 모두 이 워드에 있다. */
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val)
{
	if (pci_dev_is_disconnected(dev))	/* [한국어] 사라진 장치 조기 차단 */
		return PCIBIOS_DEVICE_NOT_FOUND;
	return pci_bus_write_config_word(dev->bus, dev->devfn, where, val);
}
EXPORT_SYMBOL(pci_write_config_word);

/* [한국어] dword 판. BAR 설정, 그리고 BAR 크기를 재려고 all-ones 를 써 넣는
 * 동작이 이 함수로 이뤄진다(써 넣고 되읽으면 하드웨어가 무시한 하위 비트로
 * 크기를 역산할 수 있다). */
int pci_write_config_dword(const struct pci_dev *dev, int where,
			   u32 val)
{
	if (pci_dev_is_disconnected(dev))	/* [한국어] 사라진 장치 조기 차단 */
		return PCIBIOS_DEVICE_NOT_FOUND;
	return pci_bus_write_config_dword(dev->bus, dev->devfn, where, val);
}
EXPORT_SYMBOL(pci_write_config_dword);

/*
 * [한국어]
 * pci_clear_and_set_config_dword - config space dword 의 일부 비트만 바꾼다
 *
 * @dev:   대상 장치
 * @pos:   config space 절대 바이트 오프셋 (4의 배수여야 한다)
 * @clear: 0 으로 만들 비트 마스크
 * @set:   1 로 만들 비트 마스크
 * @return: 없음 — 실패를 알릴 방법이 없다.
 *
 * 앞의 pcie_capability_clear_and_set_dword 와 이름이 비슷하지만 대상이 다르다.
 * 이쪽은 PCI Express Capability 안이 아니라 config space 전체를 대상으로 하는
 * 절대 오프셋 판이며, capability 존재 여부 검사도 하지 않는다.
 *
 * void 반환이라는 점이 눈에 띈다. 읽기가 실패하면 val 에는 all-ones 가 들어오고,
 * 그것을 바탕으로 합성한 값을 그대로 써 넣게 된다 — 즉 실패가 조용히 잘못된
 * 쓰기로 이어진다. 그럼에도 이렇게 둔 것은 호출자들이 모두 "이미 존재가 확인된
 * 장치의, 존재가 확인된 레지스터" 를 다루는 자리이기 때문이다. 사라진 장치라면
 * 아래 write 도 함께 실패하므로 실제 하드웨어에 잘못된 값이 닿지는 않는다.
 *
 * 락도 없다. 따라서 읽기와 쓰기 사이에 다른 CPU 의 변경이 끼면 그것을 지운다.
 * 원자성이 필요한 자리에서는 이 함수를 쓰면 안 된다.
 *
 * 실행 컨텍스트: 제약 없음.
 * 호출자: 확장 capability 레지스터의 비트를 토글하는 코드들.
 *
 * 호출 체인:
 *   [pci_clear_and_set_config_dword] -> pci_read_config_dword -> pci_write_config_dword
 */
void pci_clear_and_set_config_dword(const struct pci_dev *dev, int pos,
				    u32 clear, u32 set)
{
	u32 val;	/* [한국어] 현재 값을 담아 고칠 작업 변수 */

	/* [한국어] 1단계 — 읽기. 반환값을 확인하지 않는다는 점에 주의.
	 * 실패하면 val 은 all-ones 가 되고, 그 위에 마스크를 적용한 값이 써진다. */
	pci_read_config_dword(dev, pos, &val);
	val &= ~clear;	/* [한국어] 2단계 — 지정된 비트를 내린다 */
	val |= set;	/* [한국어] 3단계 — 지정된 비트를 올린다 (겹치면 set 이 이긴다) */
	/* [한국어] 4단계 — 되쓴다. 장치가 사라진 상태라면 이 쓰기도 실패하므로
	 * 잘못된 값이 실제 하드웨어에 닿지는 않는다. */
	pci_write_config_dword(dev, pos, val);
}
EXPORT_SYMBOL(pci_clear_and_set_config_dword);
