# Sura에 기여하기

이 저장소의 문법과 현재 지원 범위는 `Guide/GUIDE.md`, `reference.html`,
`SCOPE.md`, `COMPATIBILITY.md`를 기준으로 한다. 문서에 없는 동작을 추측해서
구현하거나 실제로 Python·JavaScript 문법을 섞지 않는다.

## 실제 Sura 문법

```sura
func twice(value: number) -> number do
    result is value * 2
    return result
end

assert_eq(twice(21), 42)
```

변수 선언과 대입은 `is`, 함수는 `func ... do ... end`, 반복은
`for item in items do ... end`를 사용한다. `let`, `var`, `const`, `fn`은 Sura
문법이 아니다.

## 빌드와 검증

Windows x64 portable build:

```powershell
.\build.bat portable
.\SuraLanguage.exe --version
powershell -NoProfile -ExecutionPolicy Bypass -File .\run_stable_tests.ps1
```

Linux와 macOS:

```sh
make clean
make
./SuraLanguage --version
```

변경 범위에 맞는 좁은 smoke test를 먼저 실행하고, 제출 전에는 다음 계약을 확인한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_compatibility_gate.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_guide_syntax_smoke.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_ci_coverage_gate.ps1
```

parser, bytecode loader, VM, JIT, GC 또는 FFI 경계를 바꾸면 관련 안전성 smoke와
sanitizer workflow도 갱신한다. Windows의 MinGW 설치에는 ASan/UBSan library가 없을
수 있으므로 최종 sanitizer 검증은 Ubuntu CI가 담당한다.

## 패키지 기여

```powershell
surapkg new my_package
surapkg test .\my_package
surapkg quality .\my_package --json quality.json
surapkg publish .\my_package --dry-run --json publish-check.json
```

`publish --dry-run`은 registry를 수정하지 않는다. 현재 저장소의 local registry
index는 `registry/index.json`이며, 외부 registry URL은 환경 설정에 따라 달라진다.
공개 package 제안에는 source, test, README, license, version과 dry-run 결과를 포함한다.

## Pull request 확인 항목

- 변경 이유와 사용자가 보게 되는 동작을 설명한다.
- 새 동작과 실패 동작을 재현하는 test를 추가한다.
- 문법·API·CLI가 바뀌면 guide, reference generator와 VS Code metadata를 확인한다.
- source·bytecode·release format·Plugin/FFI ABI가 바뀌면 호환성 계약을 갱신한다.
- 성능 주장은 같은 입력, warm-up, 반복 회수, 원시 결과가 있는 benchmark로만 한다.
- 구현하지 않은 기능이나 실행하지 않은 검증을 완료한 것으로 적지 않는다.

## gate나 smoke script를 쓸 때

**PASS를 출력하는 script는 `exit 0`을 명시한다.** 명시하지 않으면 script의 exit
code는 마지막 명령이 남긴 값이 되고, 사람이 읽는 verdict와 CI가 행동하는 근거가
갈린다.

실제 사례: `sura_build_contract_smoke.ps1`은 마지막 검사로 `build.bat`을 잘못된
mode로 호출해 거부를 확인한다. `build.bat`은 그때 정확히 2로 끝난다. `exit 0`이
없어서 gate가 **PASS를 출력하면서 2로 종료**했고, CI는 통과한 gate를 실패로 읽었다.

```
.\tools\sura_gate_exit_audit.ps1
```

exit code를 명시하지 않은 script를 찾아준다. 현재 169개 중 **64개**가 마지막
명령에 맡기고 있으며, 대부분은 그 명령이 우연히 0을 반환해서 동작할 뿐이다.
**새 gate를 추가할 때 이 목록을 늘리지 않는다.**

이론적인 걱정이 아니다. **18개가 실제로 PASS를 출력하면서 0이 아닌 code로
끝나고 있었다** — CI에서 통과한 gate가 실패로 보고되고 있었다는 뜻이고, 전체
suite의 대략 9분의 1이다: `build_contract`, `pkg_version`, `pkg_tree`, `policy`,
`tool_policy_audit`, `plugin_manifest_audit`, `public_signature`,
`target_lowering_audit`, `registry_verify`, `quality`, `compatibility_gate`,
`ci_coverage_gate`, `pkg_check`, `pkg_lint`, `engine_lint`,
`security_audit_bundle`, `release_evidence_gate`, `engine_test`.

18개 모두 같은 형태다 — gate의 마지막 native 명령이 "무언가가 제대로 거부되는지"
확인하는 **부정 검사**이고, 그것이 설계대로 0이 아닌 code로 끝난다. 그래서
**검증·정책·감사 계열 gate가 남은 것을 찾을 확률이 가장 높다.** 그중 두 개는
다른 gate를 검사하는 smoke test였다 — 결함이 이미 그것을 단속해야 할 층까지
올라와 있었다는 뜻이다.

**반드시 먼저 실행한다.** `sura_wasm_memory_safety_smoke.ps1`은 이 machine에서
실제로 실패한다(node runner가 `RuntimeError: unreachable`). 실행 없이 `exit 0`을
붙였다면 위 18개를 고치는 것과 **똑같은 한 줄**이 진짜 실패를 영구히 초록불로
바꿨을 것이다 — 지금 상태보다 나쁜 유일한 결과다.

먼저 실행한 덕분에 **engine bug 2개**도 나왔다. `sura_pkg_profile_smoke.ps1`이
그냥 exit code만 틀린 게 아니라 실제로 실패하고 있었다.

- **profiler는 JIT 코드를 보지 못했다.** counter는 전부 interpreter dispatch에서
  올라가므로 native로 실행된 부분은 보고서에 한 줄도 남기지 않는다. 해당 loop에서
  interpreter는 arithmetic 3 / branch 2 / call 1인데 JIT은 1 / 0 / 0을 보고했다.
  branch 0은 "분기가 없는 program"과 구별되지 않으므로 이건 불완전한 게 아니라
  **틀린** 보고서다. `jit_vm.hpp`의 `native_allowed()`로 고쳤다 — profiler가
  붙으면 native 컴파일을 하지 않는다. **emitter 범위를 넓히면 profiler가 나빠진다**는
  결합이 이 session에서 처음 드러났다.
- **한글 경로에서 `import`가 아예 동작하지 않았다.** compiler가 `fs::path`의 narrow
  생성자로 import 경로를 만들었는데 Windows에서 그건 ANSI codepage로 해석된다.
  Sura source는 UTF-8이므로 `문서\` 같은 directory 아래 project는 import를 쓸 수
  없었다 — **이 저장소 자신을 포함해서**. `jit_compiler.hpp`에서 `fs::u8path` /
  `u8string()`로 고쳤다. 경로를 다루는 새 code를 쓸 때 narrow `std::string`을
  `fs::path`에 그대로 넣지 않는다.

`sura_utf8_path_smoke.ps1`은 두 번째 bug가 살아있는 내내 통과했다 — stdlib file
API만 확인하고 `import`는 건드리지 않았기 때문이다. 지금은 둘 다 covers한다.

기존 backlog는 검증된 batch 단위로 정리한다 — gate를 실행해 통과를 확인하고,
`exit 0`을 넣고, 다시 실행해 여전히 통과하며 0으로 끝나는지 확인한다. 확인 없이
일괄로 붙이지 않는다. QEMU·registry service·특정 hardware가 필요한 gate는
"환경 의존"으로 분류할 일이지, 무조건 0을 반환하게 만들 일이 아니다 — 그것이
지금 상태보다 나쁜 유일한 결과다.

## 문서 file을 PowerShell로 다시 쓰지 않는다

`Get-Content -Raw`는 PowerShell 5.1에서 file을 **system ANSI codepage**(한국어
Windows에서는 CP949)로 읽는다. UTF-8로 저장된 이 저장소의 문서를 그렇게 읽으면
한글이 이미 그 시점에 깨지고, `Set-Content -Encoding utf8`로 되쓰면 깨진 상태가
그대로 굳는다. 되돌릴 수도 없다 — CP949로 mapping되지 않는 byte는 U+FFFD로
바뀌어 정보가 사라진다.

문서를 고칠 때는 편집기나 `sed`처럼 byte를 그대로 다루는 도구를 쓴다. PowerShell을
꼭 써야 한다면 `[IO.File]::ReadAllText($p, [Text.Encoding]::UTF8)`와
`[IO.File]::WriteAllText($p, $s, (New-Object Text.UTF8Encoding $false))`로 encoding을
양쪽 다 명시한다.

## Bug를 보고할 때

```
.\tools\sura_bug_report.ps1 -Repro .\minimal.sura -Out issue-42.md
```

engine version뿐 아니라 **SHA-256과 byte 수**, `--jit-info-json` target 보고,
host 정보(OS, CPU 수, C++ compiler, GPU), 그리고 재현 결과를 담은 Markdown을
만든다. version 문자열만으로는 무엇을 돌았는지 특정하지 못한다 — 같은 1.11.1이라도
보고하는 binary는 rebuild될 때마다 달라진다.

`-Repro`로 넘긴 file은 **register VM과 `--jit` 양쪽에서** 실행하고 두 출력을
모두 기록한다. 둘이 다르면 그 자체가 보고할 내용이다.

환경 변수는 수집하지 않고, `-Repro`로 지정한 file 외에는 어떤 file도 읽지 않으며,
user profile 경로는 `~`로 치환한다. **재현 file은 그대로 embed되므로 공개 issue에
올리기 전에 내용을 확인한다.**

## LSP나 editor 통합을 건드린다면

```
.\tools\sura_lsp_smoke.ps1
```

engine을 `--lsp`로 띄워 VS Code extension과 같은 순서로 몰아본다 — `initialize` →
`initialized` → `didOpen` → 각 request → `shutdown`/`exit`, 실제 stdio와
Content-Length framing 위에서. `.sura` test suite는 program을 실행할 뿐이라
editor 경로 전체가 여기 없이는 검증되지 않는다.

`hover`, `completion`, `definition`, `references`, `signatureHelp`,
`documentSymbol`, `formatting`, `codeAction`, `rename`, `workspace/symbol`을
확인한다. **JSON-RPC `error` 응답은 통과가 아니라 실패로 센다** — 그 method들은
server가 `initialize` capabilities에 스스로 광고한 것이므로, error로 답하는 것은
client와의 약속을 깨는 것이다.

hover 문구 같은 내용이 아니라 framing과 envelope을 본다. message를 개선할 때마다
test가 깨지면 쓸 수 없기 때문이다.

## 반복 개발 시 build 시간

correctness만 확인하는 rebuild에 release build를 쓸 필요는 없다:

```
.\build.bat dev
```

engine만 `-O1`로 build하고 package manager는 건너뛴다. 이 tree에서 측정한 값
(g++ 15.2.0):

| 대상 | 시간 |
| --- | ---: |
| `main.cpp` @ `-O3` | 89초 |
| `main.cpp` @ `-O1` | 53초 |
| `main.cpp` @ `-O0` | 40초 (**link 실패**) |
| `surapkg.cpp` | 35초 |
| `gc.cpp` + `platform.cpp` | 2초 |
| `build.bat dev` 전체 | **55초** |
| `build.bat portable` 전체 | 약 145초 |

`-O0`은 더 빠르지만 이 toolchain에서 link되지 않는다 — `SuraStd`의 inline
accessor 안 `thread_local` static들이 `__emutls_t` undefined reference를 낸다.
`-O1`이 실제로 binary를 만드는 가장 빠른 level이다.

**dev build로 benchmark나 timing을 재지 않는다.** 성능 측정과 release에는 반드시
`portable`을 쓴다.

engine이 사실상 하나의 거대한 translation unit이라 나머지 바닥은 약 16,000줄
`stdlib.hpp` 자체의 parse/instantiate 비용이다. 50초 아래로 내리려면 flag가
아니라 그 header를 나눠 병렬 compile하는 수밖에 없다.

## JIT codegen 변경 시 (jit_native.hpp, jit_x64.hpp, jit_op.hpp)

`jit_native.hpp`는 기계어를 직접 emit한다. emitter가 틀리면 대개 crash가 아니라
**다른 값을 계산한 채 정상 종료**한다. `run_stable_tests.ps1`은 engine 구성 하나만
돌리고 test가 PASS를 출력하는지만 보므로, 잘못 compile된 program이 PASS를 찍으면
정상과 구분되지 않는다.

```
.\tools\sura_jit_differential.ps1 -Path tests -FailOnNoNative
```

같은 program을 `--jit` 유무로 각각 실행해 출력이 바이트 단위로 일치하는지 대조한다.
register VM이 기준(reference)이다. 판정 규칙:

- `DIVERGED`는 codegen 결함이다. 예외 없이 수정 대상이다.
- `NONDET`은 clock/random/uuid를 쓰는 program이라 oracle로 쓸 수 없어 제외된 것이다.
  VM으로 두 번 돌려 자동 검출하므로 목록을 손으로 관리하지 않는다.
- `No native compilation`은 **통과가 아니다.** emitter를 하나도 건드리지 않고
  실행됐으므로 JIT 정확성의 증거가 되지 않는다. opcode coverage를 넓혔다면 이 값이
  줄고 `Verified`가 늘어야 한다. 그렇지 않다면 새 경로가 실제로 실행되지 않은 것이다.

engine을 `--jit`으로 실행하면 `[JIT-OPS]` 줄에 그 실행이 실제로 방출한 opcode
목록이 나온다. 차등 script는 이를 전체 실행에 대해 합집합으로 집계해
`Emitters exercised`로 보고한다. **"test 몇 개가 JIT를 타는가"가 아니라 "어떤
emitter가 검증됐는가"가 옳은 질문이다.**

```
[JIT-OPS] 18 opcode(s) emitted: LOAD_CONST MOVE ADD SUB MUL MOD CMP_EQ ... JUMP_IF_TRUE ...
```

emitter를 추가하거나 수정했다면 그 opcode가 이 목록에 들어오는지 확인한다.
들어오지 않으면 새 code는 한 번도 실행되지 않은 것이고, test가 통과해도 의미가
없다.

이 계측으로 실제로 나온 것들:

- `BIT_NOT`, `BIT_XOR`, `LOGICAL_NOT`, `LOAD_NIL`은 emitter가 있는데도 suite
  전체에서 한 번도 방출되지 않고 있었다. `BIT_AND`/`BIT_OR`/`LSHIFT`/`RSHIFT`는
  우연히 함께 들어 있어 "비트 연산은 test된다"고 착각하기 쉬웠다.
  `74_jit_bitwise_differential.sura`가 남은 넷을 덮는다.
- `DIV`는 emit_op에 case가 있으면서도 그 안에서 `return false`로 bail하고
  있었다. 나눗셈이 **하나라도** 있으면 그 함수 전체가 JIT에서 탈락해 나머지
  연산까지 register VM에서 돌았다. `sura_jit_checked_div` guarded helper로
  해결했으며(`MOD`와 같은 형태 — 0 나눗셈은 infinity가 아니라 `[E202]`를
  raise해야 하므로 inline SSE 경로를 쓸 수 없다), `bench_division.sura` 기준
  10/10 round에서 **21.01ms → 3.50ms (6.0배)**로 개선됐다.
- `if/else`만으로는 `JUMP_IF_FALSE`만 방출된다. `JUMP_IF_TRUE`에 도달하려면
  `or` 단축 평가가 필요하다.
- 최상위 `main` chunk는 `native_funcs`에 저장되지 않는 지역 `NativeFunc`로
  compile된다. 그래서 `[JIT] N function(s), M method(s)` 집계에도, 초기의
  opcode 집계에도 잡히지 않았고, `DEF_CLASS`/`HALT`/`STORE_GLOBAL`이 검증되지
  않은 것처럼 보였다. `JitVM::main_emitted_ops`가 이를 따로 든다.
  **계측을 추가할 때 top-level 경로를 빠뜨리지 않았는지 확인한다.**
- `USE_LIB`도 `DIV`과 같은 무조건 bail이었다. 최상위는 all-or-nothing으로
  compile되므로 `use` 한 줄이 program 전체를 interpreter로 되돌렸다.
  `sura_jit_use_lib` helper로 해결했고 `bench_use_toplevel.sura` 기준
  10/10 round에서 **15.26ms → 2.63ms (5.8배)**다. 이 opcode는 순수하지 않고
  module을 global에 bind하는 **부수 효과**가 있으므로, helper는 interpreter와
  같은 순서로 같은 bind를 수행해야 한다. 그러지 않으면 관찰 가능한 state가
  "top-level이 compile됐는지"에 따라 달라진다.

차등 script는 native 실행 여부를 `[JIT] N function(s)` 개수가 아니라 방출된
opcode 유무로 판정한다. 전자는 main을 세지 않으므로, main만 compile된 program을
"native 없음"으로 잘못 분류했다.

## 무엇에 emitter를 붙일지 고를 때

engine은 여기서 막힌 opcode를 보고한다:

```
[JIT-BAIL] top level not compiled: NEW_INSTANCE at ip=22
```

차등 script가 전체 run에 대해 집계해 `Top-level compiles blocked by opcode`로
순위를 낸다. top-level은 all-or-nothing이므로 이 수치는 **program 개수**다 — 그
opcode 하나가 그만큼의 program에서 native code를 통째로 앗아갔다는 뜻이다.
`DIV`와 `USE_LIB`은 짐작이 아니라 이 순위로 찾았다.

읽을 때 주의할 점 두 가지. 이 단위는 "몇 개의 program을 막는가"이지 "그 program이
막힌 구간에서 시간을 얼마나 쓰는가"가 아니다. 그리고 program은 자신이 멈춘
**첫 번째** opcode만 보고하므로, 1위에 emitter를 붙여도 그 program이 전환되지
않을 수 있다 — 바로 다음 opcode에서 다시 bail한다. **emitter를 추가할 때마다
순위를 다시 뽑는다.**

## 최적화 검증 (engine 성능 변경 시)

engine 두 벌을 빌드해 단순 비교하지 않는다. 이 하드웨어에서는 **같은 바이너리**가
몇 분 간격으로 97ms와 140ms로 측정된 사례가 있어, 단일 측정값 비교로는 십몇 %의
개선을 판정할 수 없다.

```
.\tools\sura_ab_bench.ps1 -EngineA .\SuraLanguage.exe -EngineB .\SuraNew.exe -Script bench_fib.sura -Rounds 10
```

**JIT codegen을 바꿨다면 `-Jit`을 반드시 붙인다.** 이 flag 없이 측정하면 양쪽 다
register VM에서만 돌아 codegen 변경이 noise로 보인다. 실제 사례: DIV emitter를
추가한 뒤 `-Jit` 없이 재면 9%(사실상 무의미한 차이)로 나왔지만, 붙여서 재면
10/10 round에서 6.0배였다. 출력 상단의 `Mode` 줄로 어느 쪽을 쟀는지 확인한다.

이 script는 두 build를 라운드마다 교차 실행하고 순서를 ABBA로 교대해 thermal
drift 영향을 제거한 뒤, 최솟값과 승률을 함께 보고한다. **승률이 갈리면
`INCONCLUSIVE`이며, 최솟값이 좋아졌더라도 개선으로 읽지 않는다.** 최솟값과 승률을
둘 다 기록한다.
