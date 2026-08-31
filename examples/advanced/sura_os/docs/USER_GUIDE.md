# SuraOS 사용자 안내서

## 시작 모드

`--demo`는 고정된 명령 묶음을 실행하고 종료한다. 인자를 생략해도 demo다. `--smoke`는 핵심 불변식과 실행 경로를 검사한다. `--interactive`는 입력을 받는 가상 셸을 연다.

부팅 메시지는 가상 콘솔 버퍼에 기록된다. 기본 데모에서는 셸 출력만 호스트 터미널에 echo한다.

## 경로와 파일

시작 디렉터리는 일반 사용자에게 `/home/user`다.

```text
pwd
ls -l /
tree /etc
cat /etc/os-release
write notes.txt first line
append notes.txt second line
cat notes.txt
mkdir -p projects/demo
cp notes.txt projects/demo/copy.txt
mv projects/demo/copy.txt projects/demo/final.txt
stat projects/demo/final.txt
rm projects/demo/final.txt
```

이 명령은 모두 가상 디스크만 바꾼다. 호스트의 현재 디렉터리 파일을 읽거나 지우지 않는다.

권한은 0~7의 압축된 owner mode를 사용한다. read=4, write=2, execute=1이다.

```text
chmod 4 notes.txt
```

## 프로그램 실행

```text
programs
disasm counter
run hello
run counter 2000
run cooperative
ps
top
```

`run`은 등록된 SVM32 이미지를 spawn하고 지정한 최대 cycle까지 스케줄러를 돌린다. 종료한 프로세스도 교육용 관찰을 위해 PCB 목록에 남는다.

가상 파일시스템의 `.sasm` 파일을 조립할 수도 있다.

```text
cat /bin/counter.sasm
assemble /bin/counter.sasm my-counter
disasm my-counter
run my-counter
```

호스트 `programs` 폴더의 소스는 참고·확장용이다. 대화형 셸에서 쓰려면 내용을 가상 파일로 작성하거나 부팅 설치 코드에 등록한다.

## 프로세스 제어

```text
ps
nice 3 8
kill 3 STOP
kill 3 CONT
kill 3 TERM
```

일반 사용자는 자신의 프로세스만 신호로 제어한다. root는 모든 프로세스를 제어한다. `KILL`은 즉시 종료하고 `TERM`은 종료 의사를 전달한다.

## 메모리와 디스크

```text
mem
disk
health
```

`mem`은 segment address range, permissions, PID, kind를 출력한다. 프로세스 종료 시 해당 PID의 세그먼트가 해제된다. `disk`는 파일 content 크기의 합, 용량, 노드 수, I/O 카운터를 보여준다.

## 서비스

```text
services
service status welcome
```

상태 변경은 root 작업이다.

```text
service start welcome
service stop welcome
service restart cooperative-worker
service disable counter-worker
service enable counter-worker
```

기본 target:

| target | unit |
|---|---|
| default | welcome, cooperative-worker |
| multi-user | welcome, counter-worker, cooperative-worker |
| rescue | welcome |

one-shot 예제는 프로그램이 0으로 종료하면 inactive가 된다. 실패 재시작 unit은 delay와 최대 횟수를 지킨다.

## 패키지

조회는 일반 사용자도 가능하다.

```text
pkg list
pkg search manual
pkg stats
pkg verify
```

설치·삭제는 root 전용이다.

```text
pkg install sura-manual ^1.0.0
pkg install sura-demos
pkg verify sura-manual
pkg remove sura-manual
```

`sura-manual`은 `sura-base >=1.0.0`에 의존한다. resolver가 base를 먼저 설치한다. 다른 설치 패키지가 base를 요구하면 강제 옵션 없이 base를 제거할 수 없다.

## 네트워크

```text
ip address
ip route
ip socket
ip firewall
ip stats
```

기본 주소는 `127.0.0.1/8` 하나다. 외부 인터넷 연결은 없다. 낮은 수준의 기존 장치 데모는 다음 명령을 사용한다.

```text
net-send 9000 hello
net-recv 9000
```

통합 테스트는 상위 소켓 계층에서 UDP server/client를 만들고, route와 firewall을 거쳐 loopback packet을 전달한다.

## 모니터링

```text
monitor dashboard
monitor sample
monitor stats
monitor rules
monitor alerts
```

기본 경고는 메모리 85%, 디스크 90%, protection fault, 디스크 오류, 실패 프로세스, 실패 서비스, 패킷 drop, 커널 error log를 감시한다.

대시보드는 CPU, 메모리, 디스크를 30칸 텍스트 막대로 그리고 프로세스·네트워크·커널·경고 수치를 덧붙인다.

## 사용자와 환경

```text
whoami
id
users
env
export EDITOR sura-edit
echo hello virtual world
history
```

셸 환경은 가상 셸 객체에만 저장된다. 호스트 환경 변수를 변경하지 않는다.

## 진단과 부팅

```text
boot-report
log 30
health
devices
```

boot report는 단계별 PASS/FAIL, 탐지 메모리, 장치 수, 사용자 수, 설치 프로그램 수를 보여준다. log는 커널의 최근 구조화 로그를 시간 순으로 표시한다.

## 종료

`exit`는 셸만 닫는다. `shutdown`은 capability가 있는 사용자만 커널 종료를 요청할 수 있다. 기본 일반 사용자는 거부되고 root는 살아 있는 모든 프로세스를 정리한 뒤 종료 상태를 기록한다.

## 자주 생기는 문제

### import 실패와 한글 Windows 경로

현재 Sura 런타임의 중첩 import는 Windows 한글 절대 경로에서 제한이 있다. 이 예제는 main과 test가 필요한 모듈을 직접 import하여 회피한다. 반드시 위 빠른 실행 예처럼 `sura_os` 또는 `tests` 디렉터리에서 상대 경로로 실행한다.

### package가 설치되지 않음

일반 사용자라면 조회만 가능하다. root 셸을 만들거나 코드 수준 테스트에서 관리자 API를 사용한다.

### 프로그램이 계속 TERMINATED로 보임

의도된 관찰 동작이다. `kernel_reap_all`을 호출하거나 자동 reap 설정을 켜면 제거할 수 있다.

### 실제 OS로 부팅하고 싶음

이 프로젝트는 호스티드 모델이다. 실제 부팅에는 Sura 네이티브 freestanding target, 링커 스크립트, UEFI/BIOS loader, 물리 메모리·인터럽트·드라이버 ABI가 추가로 필요하다. 현재 Sura 런타임 위 예제와는 별도 프로젝트가 되어야 한다.

## 자동 저장과 복원

프로젝트 루트에서 대화형 모드를 실행하면 `suraos.disk.json`이 기본 디스크 이미지가 된다.

```powershell
..\..\..\SuraLanguage.exe main.sura -- --interactive
```

첫 실행은 새 디스크로 시작한다. `exit`할 때 dirty 파일시스템을 자동 저장하며, 다음 실행은 해당 이미지를 먼저 검증한 후 복원한다. 다른 이미지를 쓰거나 저장을 끌 수도 있다.

```powershell
..\..\..\SuraLanguage.exe main.sura -- --interactive --disk school.disk.json
..\..\..\SuraLanguage.exe main.sura -- --interactive --no-autosave
..\..\..\SuraLanguage.exe main.sura -- --interactive --ephemeral
```

```text
disk usage
disk status
disk save
disk load
disk autosave off
disk autosave on
```

`--no-autosave`는 자동 저장만 끄므로 `disk save`는 사용할 수 있다. `--ephemeral`은 이미지 로드와 저장을 모두 끈다. 디스크 이미지에는 가상 파일과 언어 설치 표식이 저장된다. 실행 중 프로세스, CPU 레지스터, RAM 전체를 재개하는 최대 절전 이미지는 아니다.

## 가상 파일에 프로그램 작성하고 실행하기

### Sura Mini

```text
lang available
lang install sura-mini
edit hello.sura
```

편집기에 다음을 입력하고 마지막 줄에 `.save`를 입력한다.

```text
set name SuraOS
print Hello $name
set value 6
mul value 7
print answer=$value
write answer.txt $value
.save
```

```text
runfile hello.sura
cat answer.txt
```

한 줄로도 만들 수 있다. 세미콜론은 Sura Mini 문장을 나누며, 셸에서 전체 소스를 따옴표로 감싸야 한다.

```text
write hello.sura "set value 6; mul value 7; print answer=$value"
runfile hello.sura
```

지원 문장은 `print`, `set`/`let`, `add`, `sub`, `mul`, `div`, `repeat`, `read`, `write`, `append`, `assert`, `disk`, `exit`이다. 프로그램은 최대 10,000단계, `repeat`는 한 문장당 최대 1,000회로 제한된다. 접근 가능한 대상도 현재 사용자 권한의 가상 파일뿐이다.

### SVM32

```text
lang install svm32
runfile /home/user/.local/languages/svm32/examples/hello.sasm
```

`.sasm` 파일은 어셈블된 뒤 실제 SuraOS PCB를 가진 가상 프로세스로 실행된다. 기존 `assemble`, `run`, `disasm` 명령도 계속 사용할 수 있다.

## 디스크가 가득 찼을 때

```text
disk usage
tree /home/user
rm old-file.txt
disk usage
```

85%부터 `warning`, 95%부터 `critical`, 남은 바이트가 0이면 `full`로 표시된다. 새 파일, 기존 파일 확장, 언어 설치, 패키지 설치는 필요한 공간을 먼저 계산한다. 부족하면 `NO_SPACE` 오류를 내고 새 빈 파일이나 반쯤 설치된 언어를 남기지 않는다. 같은 크기 덮어쓰기와 파일 축소·삭제는 가능하다.

호스트 Windows 드라이브가 가득 차면 임시 이미지 쓰기 단계에서 저장이 실패한다. 마지막으로 성공한 정식 이미지에는 손대지 않으며, 공간을 확보한 뒤 현재 세션에서 `disk save`를 다시 실행해야 한다. 저장하지 않고 프로세스를 끝내면 마지막 성공 저장 이후 변경은 사라진다.
