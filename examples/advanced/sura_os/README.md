# SuraOS

SuraOS는 Sura 언어로 작성한 교육용 호스티드 가상 운영체제입니다. 실제 PC의 부트 섹터를 교체하거나 하드웨어를 직접 제어하는 베어메탈 OS는 아닙니다. 대신 Sura 프로세스 안에 CPU, 메모리, 커널, 프로세스, 파일시스템, 장치, 네트워크, 서비스, 패키지 관리자를 구현해 운영체제의 핵심 구조를 안전하게 실험할 수 있습니다.

프로젝트는 기존 Sura-Language 파일을 수정하지 않고 `examples/advanced/sura_os`라는 새 디렉터리 안에 독립적으로 추가되었습니다.

## 빠른 실행

저장소 루트에서 다음처럼 실행합니다.

```powershell
cd examples\advanced\sura_os
..\..\..\SuraLanguage.exe main.sura -- --demo
```

자동 검증용 스모크 모드:

```powershell
..\..\..\SuraLanguage.exe main.sura -- --smoke
```

대화형 가상 셸:

```powershell
..\..\..\SuraLanguage.exe main.sura -- --interactive
```

통합 테스트는 한글이 포함된 Windows 절대 경로의 import 제약을 피하기 위해 테스트 디렉터리에서 직접 실행합니다.

```powershell
cd tests
..\..\..\..\SuraLanguage.exe test_sura_os.sura
```

## 구현 범위

- SVM32 가상 CPU: 범용 레지스터 8개, PC, SP, BP, 플래그, 사용자/커널 모드
- 35개 명령어: 이동, 산술, 비트 연산, 분기, 스택, 호출, 시스템콜, 트랩
- 2패스 어셈블러: 지시어, 레이블, 데이터 심볼, 진단, 역어셈블
- 희소 가상 메모리: 세그먼트, 권한, 정렬, 프로세스 레이아웃, 힙, 보호 오류
- 프로세스: 상태 전이, 부모·자식, 신호, 메시지, 대기, 종료, 회수
- 우선순위 라운드로빈 스케줄러: 퀀텀, 에이징, 휴면 깨우기, 통계
- 시스템콜: 출력, 프로세스, 파일, 메모리, 시간, IPC, 디렉터리, 난수
- 계층형 메모리 파일시스템: 파일·디렉터리·장치 노드, 권한, FD, 복사·이동
- 가상 장치: 콘솔, 키보드, 시계, 결정적 난수, null, 루프백 네트워크
- 사용자와 보안: 사용자·그룹, 로그인 세션, capability, 감사 결정
- 단계형 부트로더: 설정, 메모리 패턴, 장치, 파일시스템, 보안, 커널 인계
- 서비스 관리자: 의존성, target, 시작·중지·재시작, 재시작 정책, 이벤트
- 패키지 관리자: 버전 제약, 의존성 해결, 트랜잭션, 설치·삭제·검증
- IPv4 계층: 인터페이스, CIDR, 라우팅, 방화벽, DNS, UDP/TCP 소켓 모델
- 시스템 모니터: 시계열 샘플, 임계치 규칙, 경고, 텍스트 대시보드
- 가상 셸: 파일, 프로세스, 프로그램, 장치, 서비스, 패키지, 네트워크 명령
- 스냅샷: 메모리, 디스크, 프로세스, 스케줄러 상태를 직렬화 가능한 값으로 표현

## 디렉터리

```text
sura_os/
├── main.sura                 실행 진입점
├── README.md                 이 문서
├── src/
│   ├── constants.sura        ABI와 공용 상수
│   ├── util.sura             컬렉션·문자열·경로 도우미
│   ├── diagnostics.sura      구조화 로그
│   ├── bootloader.sura       단계형 부팅과 복구 진단
│   ├── memory.sura           희소 보호 메모리
│   ├── cpu.sura              SVM32 실행기
│   ├── assembler.sura        2패스 어셈블러
│   ├── process.sura          PCB, 신호, IPC
│   ├── scheduler.sura        우선순위 라운드로빈
│   ├── filesystem.sura       가상 디스크와 FD
│   ├── devices.sura          가상 장치
│   ├── security.sura         사용자와 capability
│   ├── services.sura         서비스 관리자
│   ├── packages.sura         패키지 저장소와 설치기
│   ├── network.sura          IPv4·소켓·방화벽 모델
│   ├── monitor.sura          메트릭과 경고
│   ├── kernel.sura           커널 조정자와 시스템콜
│   └── shell.sura            명령 셸
├── programs/                 SVM32 어셈블리 예제
├── tests/                    통합 테스트
└── docs/                     설계와 사용 설명
```

## 데모 계정

| 사용자 | 암호 | 용도 |
|---|---|---|
| `root` | `root` | 시스템 관리 데모 |
| `user` | `sura` | 기본 일반 사용자 |

암호 해시는 학습용 결정적 구현입니다. 실제 인증 시스템에 사용하면 안 됩니다.

## 셸 명령 묶음

기본 정보:

```text
help  about  uname  uptime  health  boot-report  shutdown
```

파일시스템:

```text
pwd  cd  ls  tree  cat  write  append  touch  mkdir
rm  cp  mv  stat  chmod  disk
```

프로세스와 프로그램:

```text
ps  top  kill  nice  programs  run  assemble  disasm  mem
```

운영 관리:

```text
services  service  pkg  ip  monitor  devices  users  log
```

환경과 장치:

```text
whoami  id  env  export  history  random
net-send  net-recv  echo  clear  exit
```

## 설계 원칙

1. 호스트 안전성: 가상 셸은 호스트 셸에 명령을 전달하지 않습니다.
2. 결정성: 기본 난수 seed와 데모 작업량은 고정되어 재현할 수 있습니다.
3. 관찰 가능성: 각 계층은 통계, 검증 함수, 구조화 결과를 제공합니다.
4. 실패 명시성: 작업 결과는 대체로 `{ok, value, code, message}` 형태입니다.
5. 교육 가능성: 복잡한 네이티브 최적화보다 상태 전이와 경계를 코드로 드러냅니다.
6. 독립성: 이 예제의 모든 파일은 전용 새 폴더 아래에만 존재합니다.

## 제한 사항

- 실제 BIOS/UEFI, 부트 섹터, MMU, 인터럽트 컨트롤러를 제어하지 않습니다.
- 네트워크는 결정적 루프백 모델이며 외부 인터넷에 연결하지 않습니다.
- demo·smoke는 메모리 디스크를 쓰고, 대화형 모드는 기본적으로 검증된 JSON 디스크 이미지를 자동 저장·복원합니다.
- 권한 모델은 압축된 교육용 모델이며 POSIX ACL 전체를 재현하지 않습니다.
- SVM32는 실습용 ISA로 x86, ARM, RISC-V 바이너리를 실행하지 않습니다.
- 암호 저장 방식은 실제 보안용이 아닙니다.

상세 설계는 [ARCHITECTURE.md](docs/ARCHITECTURE.md), 명령어와 시스템콜은 [SVM32.md](docs/SVM32.md), 사용 방법은 [USER_GUIDE.md](docs/USER_GUIDE.md), 확장 방법은 [DEVELOPMENT.md](docs/DEVELOPMENT.md)를 참고하세요.

## 영구 저장과 게스트 언어

대화형 모드는 기본적으로 프로젝트 폴더의 `suraos.disk.json`을 사용한다. 정상적으로 `exit`하면 변경된 가상 파일을 자동 저장하고, 다음 실행에서 같은 파일을 복원한다.

```powershell
..\..\..\SuraLanguage.exe main.sura -- --interactive
..\..\..\SuraLanguage.exe main.sura -- --interactive --disk my-machine.disk.json
..\..\..\SuraLanguage.exe main.sura -- --interactive --ephemeral
```

셸에서는 `disk usage`, `disk status`, `disk save`, `disk load`, `disk autosave on`, `disk autosave off`를 사용할 수 있다. 저장 대상 호스트 경로는 시작 옵션으로만 정하며, 게스트 프로그램이 임의의 Windows 파일을 열 수는 없다.

설치 가능한 게스트 언어는 다음처럼 확인하고 실행한다.

```text
lang available
lang install sura-mini
write hello.sura "set name SuraOS; print Hello $name"
runfile hello.sura

lang install svm32
runfile /home/user/.local/languages/svm32/examples/hello.sasm
```

`sura-mini`는 변수, 정수 사칙연산, 출력, 반복, 검사, 가상 파일 읽기·쓰기를 제공하는 안전한 교육용 부분 언어다. 전체 호스트 Sura 컴파일러를 게스트에 복제한 것은 아니다. `svm32`는 기존 가상 CPU에서 어셈블리 파일을 프로세스로 실행한다. 설치 파일과 작성한 소스는 가상 디스크에 남는다.

기본 가상 디스크 용량은 16 MiB이고 파일 하나의 최대 크기는 1 MiB다. 용량이 부족하면 쓰기와 설치가 `NO_SPACE`로 거부되며, 기존 파일과 설치 전 상태를 보존한다. `disk usage`로 남은 공간을 확인하고 `rm`으로 불필요한 파일을 지우면 다시 쓸 수 있다. Windows 실제 드라이브가 가득 차 자동 저장이 실패하면 마지막으로 성공한 디스크 이미지는 유지되고 현재 세션은 dirty 상태로 남는다. 이때 Windows 공간을 확보한 뒤 종료 전에 `disk save`를 다시 실행해야 한다.
