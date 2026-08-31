# SuraOS 운영·장애 대응 런북

이 문서는 가상 시스템을 오래 실행하거나 새 기능을 시험할 때 확인할 순서를 정리한다. 모든 조치는 SuraOS 객체에만 적용되며 호스트 운영체제를 복구하거나 변경하지 않는다.

## 1. 정상 상태 기준

정상 부팅 직후 기대값:

- bootloader stage가 `READY`다.
- bootloader checks_failed가 0이다.
- `kernel.booted`가 true다.
- `kernel.panic`이 nil이다.
- `kernel_health().healthy`가 true다.
- memory와 filesystem validation이 true다.
- 기본 프로그램이 hello, counter, cooperative 세 개다.
- 장치는 console, keyboard, clock, random, null, net0 여섯 개다.
- 네트워크 lo가 up이고 `127.0.0.0/8` route가 있다.
- 서비스 unit 세 개와 package 세 개가 repository에 있다.
- 모니터 기본 rule 여덟 개가 활성화되어 있다.

셸 점검 묶음:

```text
health
boot-report
mem
disk
devices
services
pkg stats
ip address
ip route
monitor dashboard
log 30
```

## 2. 부팅 실패

### configuration 실패

`boot_config_validate`의 errors를 확인한다. kernel/init은 절대 가상 경로여야 하고 root는 `/`여야 한다. max_boot_ticks는 양수다.

### memory-probe 실패

다음 순서로 조사한다.

1. `mem_validate`에서 overlap과 counter mismatch를 확인한다.
2. probe allocation이 64바이트를 예약했는지 확인한다.
3. segment permissions에 read와 write가 모두 있는지 확인한다.
4. PID 0과 kernel_mode 인자가 올바른지 확인한다.
5. probe가 끝난 뒤 segment가 해제됐는지 확인한다.

### filesystem 실패

`fs_validate` errors에는 missing parent, missing child, size mismatch, used-byte mismatch가 기록된다. 데이터가 중요하면 mutation을 추가로 수행하지 말고 snapshot의 filesystem export와 비교한다.

### device 실패

device manager의 필드와 bootloader의 expected name을 함께 확인한다. null 장치 필드명은 `null_device`이고 `/dev/null` 노드가 가리키는 논리 이름은 `null`이다.

### security 실패

기본 root와 user가 존재하고 그룹이 둘 이상인지 확인한다. 부팅 인증은 user/sura를 사용한다. 연속 실패로 잠긴 사용자는 root 관리 작업으로만 풀어야 한다.

## 3. 커널 panic

panic 발생 시 커널은 shutdown_requested를 설정하고 새 작업 진행을 멈춘다.

수집할 정보:

- panic message와 tick
- 당시 scheduler current_pid
- 최근 ERROR/FATAL log
- 해당 process의 CPU snapshot과 last_error
- memory map과 validation errors
- filesystem validation errors
- 직전 snapshot id

`bootloader_recovery_scan`은 읽기 전용 진단으로 memory, filesystem, panic에 맞는 권장 행동을 반환한다.

## 4. 프로세스가 끝나지 않음

1. `ps`에서 RUNNING, READY, WAITING, STOPPED를 구분한다.
2. WAITING이면 wake_tick과 wait_reason을 확인한다.
3. STOPPED면 CONT signal을 보낸다.
4. ready queue에 PID가 들어 있는지 확인한다.
5. PC가 instruction 범위 안인지 확인한다.
6. 반복문에 종료 CMP와 branch가 있는지 disasm으로 본다.
7. cycles 제한이 너무 작은지 확인한다.
8. 필요하면 TERM 후 KILL을 사용한다.

무한 루프 자체는 kernel panic이 아니다. scheduler cycle budget으로 제어한다.

## 5. 메모리 pressure 또는 fault

`monitor memory-pressure`는 reserved 비율이 85% 이상일 때 활성화된다.

대응:

- 종료한 프로세스를 reap한다.
- PID별 segment를 `mem_map`에서 확인한다.
- heap allocation 목록과 heap cursor를 확인한다.
- 중복 free라면 오류를 숨기지 말고 호출자를 수정한다.
- 다른 PID 접근이라면 syscall boundary에서 pointer 소유권을 검사한다.
- snapshot restore는 data-plane memory 전체를 교체하므로 root와 정지된 workload에서만 쓴다.

## 6. 파일시스템 pressure 또는 손상

디스크 사용량은 regular file content만 계산한다. directory와 device node는 size 0이다.

공간 확보 순서:

1. `/tmp`의 불필요한 파일을 제거한다.
2. `/var/log`의 오래된 가상 로그를 정리한다.
3. 사용하지 않는 package를 제거한다.
4. 삭제 전에 `pkg_required_by`를 확인한다.
5. filesystem validation을 다시 수행한다.

recursive remove는 반드시 명시적으로 요청한다. 루트 `/` 삭제는 언제나 거부된다.

## 7. 서비스 실패

`service status NAME`에서 다음을 확인한다.

- program이 installed program registry에 있는가
- unit이 enabled인가
- dependencies_ready가 true인가
- last_exit_code와 last_error는 무엇인가
- restart_count가 max_restarts에 도달했는가
- next_start_tick이 현재 tick보다 큰가

`on-failure`는 0이 아닌 종료에만 재시작한다. `always`는 정상 종료도 재시작한다. 반복 실패 시 unit을 disable하고 프로그램을 직접 `run`하여 fault를 분리한다.

## 8. 패키지 문제

### dependency를 찾지 못함

repository에 이름과 constraint를 만족하는 버전이 있는지 본다. `^1.2.0`은 같은 major, `~1.2.0`은 같은 major/minor 범위다.

### integrity verification 실패

missing file, missing checksum, changed file을 구분한다. 변경된 파일이 사용자 설정인지 package 소유 파일인지 manifest를 확인한다. 교육용 checksum은 손상 감지용이며 암호학적 서명이 아니다.

### 제거 거부

다른 설치 package의 dependencies에 이름이 있으면 정상적인 보호다. 의존 package부터 제거하거나 명시적 force 정책을 별도로 검토한다.

## 9. 네트워크 문제

패킷이 전달되지 않을 때:

1. interface가 up인가
2. destination을 포함하는 route가 있는가
3. longest prefix route의 interface가 올바른가
4. outbound firewall이 허용하는가
5. payload가 MTU 이하인가
6. server socket이 같은 protocol/address/port에 bind됐는가
7. inbound firewall이 허용하는가
8. `net_poll`이 호출됐는가
9. receive queue가 비어 있는가

net0는 외부 NIC가 아니다. `127.0.0.1` 밖으로 실제 packet을 보내지 않는다.

## 10. 모니터 경고

경고는 값이 threshold를 넘을 때 한 번 open되고 정상 범위로 돌아오면 resolve된다. 같은 상태에서 sample을 반복해도 alert를 무한 생성하지 않는다.

경고 조사 시 현재값 하나만 보지 말고 `monitor_average`와 history range를 함께 본다. counter형 metric은 누적값이므로 threshold 설계에서 delta가 필요한지 고려한다.

## 11. 스냅샷 절차

권장 capture 시점:

- 부팅 직후
- package 설치 전후
- 중요한 filesystem mutation 전
- 재현 가능한 fault 직전
- scheduler policy 실험의 시작과 끝

셸 예:

```text
snapshot list
snapshot stats
snapshot create baseline clean boot
snapshot protect 1
snapshot create after-demo workload complete
```

코드 API의 `snapshot_diff`는 process, file, memory, kernel counter 차이를 만든다.

`restore-data`는 filesystem과 memory만 복원한다. live process/CPU를 복원하지 않는 이유는 ready queue, PID 관계, 열린 FD, 서비스 pid가 함께 원자적으로 바뀌어야 하기 때문이다. 복원 결과의 `restored_processes`가 false인지 반드시 확인한다.

## 12. 정기 검증 체크리스트

매 변경 후:

- 정적 check
- lint
- format-check
- smoke
- integration test
- demo 0 failures
- line count와 파일 inventory
- 새 파일 경로가 전용 디렉터리 내부인지 확인

릴리스 후보에서는 같은 명령을 두 번 실행해 결정적 결과인지 확인한다. random seed, clock tick, ready queue 순서가 의도 없이 달라지면 회귀로 본다.

## 13. 안전 경계

- 가상 shell에서 host command를 실행하지 않는다.
- package file path를 canonicalize한다.
- `/boot`와 `/dev`는 package가 쓰지 못한다.
- snapshot restore는 root만 수행한다.
- shutdown과 signal은 capability를 확인한다.
- host persistence는 명시적으로 요청된 API에서만 허용한다.
- 실제 암호, API key, 개인정보를 가상 이미지에 넣지 않는다.
- 이 교육용 인증 hash를 실서비스에 재사용하지 않는다.

## 14. 장애 보고 템플릿

```text
SuraOS version:
execution mode: demo / smoke / interactive / test
host Sura runtime version:
bootloader stage:
kernel tick:
current process:
health result:
memory validation:
filesystem validation:
active alerts:
last 30 log entries:
reproduction commands:
expected behavior:
actual behavior:
snapshot ids:
```

재현 절차는 가상 셸 명령과 SVM32 source를 포함하되 호스트 개인 경로나 비밀 값은 제거한다.

## 영구 디스크 운영 절차

기본 대화형 실행은 `suraos.disk.json`을 자동으로 로드·저장한다. 운영용 이미지는 이름을 명시해 분리한다.

```powershell
..\..\..\SuraLanguage.exe main.sura -- --interactive --disk production.disk.json
```

셸에서 아래를 주기적으로 확인한다.

```text
disk status
disk usage
health
```

- `dirty=true`: 마지막 저장 뒤 변경이 있음
- `pressure=warning`: 사용률 85% 이상
- `pressure=critical`: 사용률 95% 이상
- `pressure=full`: 남은 공간 0바이트
- `last=save-failed`: 호스트 이미지 저장 실패

저장은 같은 폴더의 `.tmp` 이미지에 먼저 쓰고 다시 읽어 검증한 다음 정식 이미지로 이동한다. 호스트 용량 부족이나 JSON 쓰기 오류가 발생하면 dirty 상태를 유지하며 오류를 반환한다. 공간 확보 후 `disk save`를 재시도한다.

게스트 용량 부족은 데이터 손상으로 처리하지 않는다. 파일 쓰기·언어 설치·패키지 설치가 사전 검사에서 `NO_SPACE`로 중단된다. 불필요한 가상 파일을 삭제한 후 작업을 다시 실행한다.
