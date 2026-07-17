# SuraOS 아키텍처

## 1. 실행 모델

SuraOS는 Sura 런타임 위에서 실행되는 호스티드 가상 운영체제다. 호스트 Windows는 Sura 실행 파일과 메모리를 제공하지만, SuraOS 프로그램은 호스트 API를 직접 보지 않는다. SVM32 프로그램은 가상 CPU 명령과 시스템콜만 사용하고, 시스템콜 경계에서 커널 객체에 접근한다.

```text
SVM32 user program
        │ instruction / syscall
        ▼
virtual CPU ── trap ──> SuraOS kernel
        │                  │
        │                  ├─ process + scheduler
        │                  ├─ protected sparse memory
        │                  ├─ virtual filesystem
        │                  ├─ users + capabilities
        │                  └─ device manager
        │                         │
        └─────────────────────────┴─ console / clock / random / loopback
```

## 2. 부팅 순서

부트로더는 실제 펌웨어 대신 일관된 상태 검사를 수행한다.

1. `FIRMWARE`: 부트 설정의 경로, 호스트 이름, 제한값을 검증한다.
2. `HARDWARE-DISCOVERY`: 가상 메모리 크기와 여섯 장치를 탐지한다.
3. `memory-probe`: 64바이트 세그먼트를 예약하고 `0x12345678`을 왕복한다.
4. `FILESYSTEM`: inode 관계, 사용량 카운터, 필수 디렉터리를 검증한다.
5. `boot-files`: `/boot/boot.cfg`, 커널 표식, init 표식을 만든다.
6. `SECURITY`: 기본 사용자와 그룹 데이터베이스를 확인한다.
7. `KERNEL`: 로그인 세션과 내장 프로그램을 설치한다.
8. `USERSPACE`: 서비스와 셸이 사용할 수 있는 상태로 인계한다.

각 검사는 구조화된 check와 log entry를 남긴다. 실패 시 단계는 `FAILED`가 되고 커널 실행을 시작하지 않는다.

## 3. 커널 객체

커널은 전역 상태를 한 곳에 모으는 조정자다.

| 필드 | 역할 |
|---|---|
| `clock` | 결정적 가상 tick |
| `memory` | 희소 메모리 셀과 보호 세그먼트 |
| `fs` | 가상 디스크 노드와 파일 디스크립터 |
| `devices` | 콘솔, 키보드, 시계, 난수, null, net0 |
| `security` | 사용자, 그룹, 세션, capability |
| `scheduler` | ready queue, 퀀텀, 통계 |
| `processes` | PID를 키로 하는 PCB 사전 |
| `programs` | 설치된 SVM32 실행 이미지 |
| `services` | 서비스 unit과 target |
| `packages` | 저장소, 설치 DB, 트랜잭션 |
| `network` | 인터페이스, 경로, 소켓, 방화벽 |
| `monitor` | 시계열 표본과 경고 규칙 |
| `log` | 커널 구조화 진단 |

## 4. 프로세스 생명주기

```text
NEW → READY → RUNNING → READY
                 │  │
                 │  ├→ WAITING → READY
                 │  ├→ STOPPED → READY
                 │  └→ TERMINATED → ZOMBIE/reap
                 └──── quantum expiry
```

PCB에는 CPU 문맥, 메모리 레이아웃, 부모 PID, 자식 목록, 사용자, cwd, 열린 파일, 메시지 큐, 신호 큐, 통계가 들어 있다. 상태 변경은 허용된 전이인지 먼저 검사한다.

## 5. 스케줄링

우선순위는 0에서 10까지다. ready queue는 라운드로빈으로 순환하고, 프로세스마다 기본 퀀텀만큼 명령을 실행한다. 오래 기다린 프로세스는 aging을 받아 기아를 줄인다. `YIELD` 시스템콜은 남은 퀀텀을 자발적으로 반납한다. `SLEEP`은 wake tick까지 WAITING 상태로 보낸다.

스케줄러는 다음 값을 누적한다.

- 전체 tick과 idle tick
- 문맥 교환 횟수
- 완료한 프로세스 수
- 선점, yield, block 횟수
- 최대 ready queue 깊이
- PID별 선택 횟수

## 6. 메모리

메모리는 큰 연속 배열이 아니라 주소가 실제로 쓰일 때만 저장되는 sparse cell 사전이다. 별도 segment 목록이 주소 범위, PID 소유권, 권한, 종류를 기록한다.

프로세스 레이아웃은 code, data, heap, stack 세그먼트로 구성된다. code는 읽기·실행, data/heap/stack은 읽기·쓰기를 기본으로 한다. 사용자 모드 접근은 PID와 권한을 검사하며, 커널 모드는 명시적 인자로만 우회한다.

검증기는 다음 불변식을 확인한다.

- 세그먼트가 음수 주소나 메모리 끝을 넘지 않는다.
- 세그먼트가 서로 겹치지 않는다.
- 예약 바이트 카운터와 실제 합이 같다.
- sparse cell 주소가 유효 범위 안에 있다.

## 7. 파일시스템

노드는 path를 키로 하는 사전에 저장된다. 디렉터리는 child name 배열을 갖고, 일반 파일은 문자열 content와 size를 갖는다. 장치 노드는 `/dev` 아래에서 장치 이름을 가리킨다.

주요 불변식:

- 루트 `/`가 존재한다.
- 루트가 아닌 모든 노드의 parent가 존재한다.
- 디렉터리의 child가 실제 노드와 대응한다.
- 파일 size가 content 길이와 같다.
- 전체 파일 크기 합이 `fs.used`와 같다.

파일 디스크립터는 프로세스별로 3부터 증가한다. mode, position, inode, 열린 tick, 읽기·쓰기 횟수를 추적한다.

## 8. 서비스와 패키지

서비스 unit은 program, user, priority, dependencies, restart policy, target을 갖는다. 시작 시 의존성을 재귀적으로 올린 뒤 커널에 프로그램을 spawn한다. 종료 코드를 관찰하고 `always` 또는 `on-failure` 정책에 따라 지연 재시작한다.

패키지 manifest는 이름, semantic version, 파일 사전, 의존성 제약, checksum을 갖는다. resolver는 cycle을 탐지하면서 dependency-first 설치 순서를 만든다. 설치는 가상 파일시스템에만 쓰고, 파일별 checksum을 설치 DB에 남긴다.

## 9. 네트워크

네트워크 계층은 IPv4/CIDR을 해석하고 longest-prefix와 metric으로 route를 고른다. 방화벽은 순서대로 direction, protocol, source, destination, port를 비교한다. 기본 인터페이스 `lo`는 `127.0.0.1/8`이며 외부 전송 기능은 없다.

소켓 상태는 `open`, `bound`, `listening`, `connected`, `closed`로 모델링한다. UDP와 TCP 이름을 지원하지만 TCP 흐름 제어 전체를 구현한 것은 아니다. packet은 결정적 wire queue를 통해 loopback delivery 된다.

## 10. 관찰과 복구

모니터는 CPU load, 메모리, 디스크, 프로세스, syscall, 네트워크, 서비스, 패키지 값을 표본으로 만든다. 규칙은 숫자 비교 연산자와 severity를 가지며 활성·해제 전이를 기록한다.

`kernel_health`는 panic, 메모리 검증, 파일시스템 검증을 합친 빠른 상태다. `bootloader_recovery_scan`은 상태를 수정하지 않고 권장 복구 행동만 반환한다. 스냅샷 역시 라이브 상태를 바꾸지 않는 직렬화용 값이다.

