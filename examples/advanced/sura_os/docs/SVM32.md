# SVM32 명령어와 시스템콜

## 레지스터

| 이름 | 의미 |
|---|---|
| `R0` | 반환값과 첫 시스템콜 인자 |
| `R1` | 두 번째 인자와 범용 값 |
| `R2` | 세 번째 인자와 범용 값 |
| `R3`~`R7` | 범용 값 |
| `PC` | 다음 명령어 인덱스 |
| `SP` | 가상 스택 포인터 |
| `BP` | 가상 베이스 포인터 |

플래그는 zero, negative, greater, equal, carry, overflow를 보관한다. 현재 분기 명령은 비교 결과의 zero/negative/greater/equal을 주로 사용한다.

## 소스 형식

```text
.name program-name
.entry start
.data message Hello world
.number limit 10

start:
MOVI R0 @message
SYSCALL PRINT
MOVI R0 0
SYSCALL EXIT
```

주석은 `#` 뒤에 쓴다. 레이블은 별도 줄에서 콜론으로 끝난다. `@name`은 data 또는 label 심볼을 참조한다.

## 데이터 이동

| 명령 | 피연산자 | 동작 |
|---|---:|---|
| `NOP` | 0 | 아무 상태도 바꾸지 않는다 |
| `MOV dst src` | 2 | 레지스터 값을 복사한다 |
| `MOVI dst value` | 2 | 즉시값이나 data 심볼을 넣는다 |
| `LOAD dst address` | 2 | 가상 메모리 값을 읽는다 |
| `STORE address value` | 2 | 가상 메모리에 쓴다 |

어셈블러는 `MOV`/`MOVI`를 소스 피연산자 종류에 따라 정규화한다.

## 산술

| 명령 | 동작 |
|---|---|
| `ADD dst value` | 덧셈 |
| `ADDI dst value` | 즉시값 덧셈 |
| `SUB dst value` | 뺄셈 |
| `SUBI dst value` | 즉시값 뺄셈 |
| `MUL dst value` | 곱셈 |
| `DIV dst value` | 정수 나눗셈 |
| `MOD dst value` | 나머지 |
| `INC reg` | 1 증가 |
| `DEC reg` | 1 감소 |

0으로 나누면 CPU fault가 발생하고 커널은 해당 프로세스를 비정상 종료한다.

## 비트 연산

| 명령 | 동작 |
|---|---|
| `AND dst value` | 비트 AND |
| `OR dst value` | 비트 OR |
| `XOR dst value` | 비트 XOR |
| `NOT reg` | 비트 NOT |
| `SHL dst count` | 왼쪽 이동 |
| `SHR dst count` | 오른쪽 이동 |

## 비교와 분기

`CMP left right`가 플래그를 갱신한다.

| 명령 | 조건 |
|---|---|
| `JMP label` | 항상 |
| `JZ label` | zero |
| `JNZ label` | not zero |
| `JG label` | greater |
| `JGE label` | greater or equal |
| `JL label` | less |
| `JLE label` | less or equal |

PC는 instruction index다. 레이블은 2패스의 첫 단계에서 instruction index로 변환된다.

## 스택과 호출

| 명령 | 동작 |
|---|---|
| `PUSH value` | CPU의 논리 스택에 값을 넣는다 |
| `POP reg` | 마지막 값을 레지스터로 꺼낸다 |
| `CALL label` | 반환 PC를 push하고 분기한다 |
| `RET` | 반환 PC를 pop한다 |

빈 스택에서 POP/RET를 실행하면 stack-underflow fault다.

## 커널 경계

| 명령 | 동작 |
|---|---|
| `SYSCALL number-or-name` | 커널 시스템콜 처리기로 진입 |
| `TRAP message` | 명시적 fault 발생 |
| `HALT` | CPU 정지 |

## 시스템콜 표

| 번호 | 이름 | 입력 | 결과 |
|---:|---|---|---|
| 0 | `EXIT` | R0=종료 코드 | 프로세스 종료 |
| 1 | `PRINT` | R0=문자열 | R0=출력 길이 |
| 2 | `PRINT_NUMBER` | R0=숫자 | R0 유지 |
| 3 | `GET_PID` | 없음 | R0=PID |
| 4 | `SLEEP` | R0=tick | WAITING 전환 |
| 5 | `YIELD` | 없음 | 퀀텀 반납 |
| 6 | `OPEN` | R0=path, R1=mode | R0=fd 또는 오류 |
| 7 | `READ` | R0=fd, R1=길이 | R0=문자열 또는 오류 |
| 8 | `WRITE` | R0=fd, R1=data | R0=길이 또는 오류 |
| 9 | `CLOSE` | R0=fd | R0=결과 |
| 10 | `SPAWN` | R0=프로그램 이름 | R0=PID |
| 11 | `WAIT` | R0=PID | 종료까지 block |
| 12 | `ALLOC` | R0=크기 | R0=가상 주소 |
| 13 | `FREE` | R0=주소 | R0=결과 |
| 14 | `GET_TIME` | 없음 | R0=가상 tick |
| 15 | `SEND` | R0=PID, R1=값 | R0=message id |
| 16 | `RECEIVE` | 없음 | R0=message 또는 would-block |
| 17 | `LIST_DIR` | R0=path | R0=항목 배열 |
| 18 | `GET_USER` | 없음 | R0=username |
| 19 | `RANDOM` | R0=max | R0=결정적 난수 |

오류는 음수 코드다. 대표적으로 not-found -2, permission -3, invalid -4, no-memory -5, busy -6, bad-fd -10, fault -14가 있다.

## 예제: 합계

```text
.name sum
.entry start
start:
MOVI R0 0
MOVI R1 1
MOVI R2 100
loop:
ADD R0 R1
INC R1
CMP R1 R2
JLE loop
SYSCALL PRINT_NUMBER
MOVI R0 0
SYSCALL EXIT
```

## 어셈블러 진단

진단에는 source line, 코드, 메시지, 원문이 들어 있다. 첫 단계는 레이블 중복과 식별자를 검사하고, 두 번째 단계는 지시어·피연산자·레지스터·시스템콜을 검사한다. 오류가 하나라도 있으면 `assembled`는 false가 되어 커널 등록이 거부된다.

