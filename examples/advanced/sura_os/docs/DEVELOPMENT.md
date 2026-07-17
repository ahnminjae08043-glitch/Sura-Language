# SuraOS 확장 안내

## 변경 범위

SuraOS를 확장할 때는 이 디렉터리 아래에서만 작업한다. 상위 저장소의 컴파일러나 기존 예제를 고쳐야 하는 기능은 별도 제안으로 분리한다.

## 새 CPU 명령 추가

1. `constants.sura`에 opcode 문자열을 정의한다.
2. `suraos_opcode_valid` 목록에 넣는다.
3. `cpu_execute_instruction`에 상태 변경을 구현한다.
4. `assembler.sura`에 피연산자 수와 파싱 규칙을 넣는다.
5. `asm_validate_program`에 target 검증이 필요하면 추가한다.
6. `programs`에 성공·경계 예제를 만든다.
7. 통합 테스트에 결과와 fault 경우를 추가한다.

명령은 PC 증가 규칙, flag 변경 여부, 32비트 정수 정규화를 명확히 해야 한다.

## 새 시스템콜 추가

1. 번호를 기존 마지막 번호 뒤에 할당한다.
2. 이름↔번호 함수 양쪽에 매핑한다.
3. `kernel_handle_syscall`에서 R0~R2 입력을 읽는다.
4. 권한이 필요하면 security capability를 검사한다.
5. 성공은 R0에 값, 실패는 음수 오류를 돌려준다.
6. block/yield/halt 상태를 scheduler에 정확히 알린다.
7. 사용자 프로그램과 테스트를 추가한다.

시스템콜 처리기가 호스트 셸이나 임의 호스트 파일을 호출하게 만들지 않는다.

## 새 가상 장치 추가

장치는 create, read/write 또는 send/receive, stats, reset API를 갖는 작은 dict로 만든다. device manager에 필드를 넣고 `device_get`, `device_stats`, bootloader device check를 갱신한다. `/dev` 노드를 추가하려면 `fs_format_default`에서 mount한다.

입력은 queue에 넣고 출력은 buffer와 counter에 남기는 방식이 결정적 테스트에 유리하다.

## 파일시스템 확장

현재 파일 내용은 문자열이다. binary block을 추가할 경우 size, used counter, export/import schema, checksum, descriptor read/write가 모두 같은 단위를 사용해야 한다.

모든 mutation 뒤에는 다음을 확인한다.

- parent.children과 node.parent가 일치하는가
- `fs.used`가 파일 크기 합과 같은가
- owner와 permissions 검사가 create/remove 경계에 있는가
- recursive remove가 descendant부터 제거하는가
- host 저장은 명시적 관리자 API에만 있는가

## 스케줄러 정책 실험

새 policy는 ready queue 선택 함수로 격리한다. 같은 seed와 workload에 동일 결과가 나와야 한다. 비교할 지표는 completion tick, context switches, waiting time, response time, starvation, idle tick이다.

가능한 실험:

- shortest remaining time
- multilevel feedback queue
- earliest deadline first
- lottery scheduling
- per-user fair share

## 서비스 unit 추가

`service_define`에 program과 options를 전달한다. dependencies는 service name 배열이고, wanted_by는 target 이름 배열이다. restart는 `no`, `always`, `on-failure` 중 하나다.

의존성 cycle, 없는 프로그램, disabled unit, 최대 재시작 횟수를 테스트해야 한다. 서비스가 종료되면 반드시 `service_tick`이 상태를 관찰해야 한다.

## 패키지 추가

manifest 파일 경로는 절대 canonical path여야 하며 `/boot`, `/dev`를 쓸 수 없다.

```text
files is {"/opt/demo/readme.txt": "hello"}
dependencies is {"sura-base": ">=1.0.0"}
manifest is pkg_manifest("demo", "1.0.0", "Demo", files, dependencies, {})
pkg_manifest_finalize(manifest)
pkg_repository_add(manager, manifest)
```

설치 파일은 가능한 `/usr/share`, `/opt`, `/var/lib` 아래에 둔다. 제거 시 다른 package가 요구하는지 검사한다.

## 네트워크 실험

현재 wire queue는 net0 장치 통계를 유지하면서 구조화 packet을 보존한다. 새 protocol은 다음 경계를 지켜야 한다.

- address와 port 검증
- route lookup
- outbound firewall
- MTU 검사
- device send
- inbound firewall
- socket receive queue

실제 host socket 연결을 추가하지 않는다. 필요하다면 별도 opt-in adapter와 명확한 보안 정책을 설계한다.

## 모니터 메트릭 추가

`monitor_capture`에 숫자 필드를 넣고 `monitor_metric` path에 매핑한다. 기본 rule을 추가하기 전에 정상 idle 상태에서 오경보가 나지 않는 threshold인지 확인한다.

history는 bounded array다. 무제한 log나 sample을 쌓지 않는다.

## 코드 스타일 주의

현재 Sura 문법에서 이 프로젝트가 지키는 규칙:

- 배열과 사전 literal은 한 줄에 작성한다.
- loop body는 다음 줄에 두고 명시적으로 `end`한다.
- 긴 if/elif chain은 branch body를 여러 줄로 쓴다.
- `object.child.field is value` 같은 중첩 lvalue 대신 child를 지역 변수로 받는다.
- string `<` 비교 대신 `osutil_string_compare`를 쓴다.
- `end`, `from` 같은 예약어를 dict key identifier로 쓰지 않는다.
- 결과가 실패할 수 있으면 `{ok, value, code, message}`를 반환한다.

## 검증 절차

저장소 루트에서 정적 검사:

```powershell
.\SuraLanguage.exe --check examples\advanced\sura_os
.\SuraLanguage.exe --lint examples\advanced\sura_os
.\SuraLanguage.exe --format-check examples\advanced\sura_os
```

실행 검사:

```powershell
cd examples\advanced\sura_os
..\..\..\SuraLanguage.exe main.sura -- --smoke
..\..\..\SuraLanguage.exe main.sura -- --demo
cd tests
..\..\..\..\SuraLanguage.exe test_sura_os.sura
```

최소 통과 조건:

- 모든 `.sura`가 parse/typecheck를 통과한다.
- smoke가 PASS를 출력한다.
- integration test가 마지막 문구까지 실행된다.
- demo 명령 실패 수가 0이다.
- `kernel_health().healthy`가 true다.
- `fs_validate`와 `mem_validate`가 true다.
- 변경 파일이 `examples/advanced/sura_os` 밖에 없다.

## 다음 단계 아이디어

- window/compositor를 가상 framebuffer 위에 구현
- inode link와 mount table 확장
- executable package에서 프로그램 자동 등록
- pipe와 stream IPC
- socket accept와 TCP 상태 전이
- capability를 시스템콜별로 세분화
- snapshot restore와 replay debugger
- instruction trace 기반 profiler
- virtual multiprocessor와 per-core run queue
- Sura로 작성한 SVM32 compiler backend

