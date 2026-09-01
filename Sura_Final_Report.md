# Sura 프로젝트 통합 파일 통계 (Total Inventory)

> 이 문서의 모든 수치는 저장소 소스에서 직접 측정한 값입니다.
> 성능 주장에 대해서는 `bench_summary.md`의 조건과 한계를 함께 읽으십시오.
> 지원 범위의 기준 문서는 `README.md`와 `compatibility.json`이며, 충돌 시 그쪽이 우선합니다.

Sura 프로젝트의 C++ 소스는 루트 `*.hpp`/`*.cpp` 기준 **약 83,900라인** 규모입니다.

### 1. 핵심 엔진 (C++ Core)

| 분류 | 파일명 | 라인 수 | 역할 |
| :--- | :--- | ---: | :--- |
| **Stdlib** | `stdlib.hpp` | 16,314 | 내장 함수 테이블 전체 |
| **Package** | `surapkg.cpp` | 15,766 | 독자적인 패키지 관리 시스템 |
| **Driver** | `main.cpp` | 5,770 | CLI 진입점, REPL, 파이프라인, 진단 |
| **VM** | `jit_vm.hpp` | 4,805 | 레지스터 기반 가상 머신 실행 루프 |
| **JIT** | `jit_native.hpp` | 3,377 | 네이티브 코드 에미터 |
| **Parsing** | `parser.hpp` | 1,722 | 구문 분석 및 문법 제어 핵심 |
| **JIT** | `jit_compiler.hpp` | 1,375 | AST → 레지스터 바이트코드 |
| **Runtime** | `value.hpp` | 1,178 | NaN-Boxing 값 표현 및 GC |
| **Serialize** | `bytecode_io.hpp` | 1,128 | 버전 있는 바이트코드 직렬화 |
| **AST** | `ast.hpp` | 825 | 구문 트리 구조 및 디버그 인프라 |
| **Logic** | `typechecker.hpp` | 724 | 정적 분석 및 오타 추천 로직 |
| **Lexer** | `lexer.hpp` | 491 | 토큰 스캐닝 및 어휘 분석 |

### 2. 표준 라이브러리 및 도구 (Standard Library & Tooling)

| 분류 | 파일/폴더 | 규모 | 상태 |
| :--- | :--- | ---: | :--- |
| **Stdlib (내장)** | `stdlib.hpp` | 고유 이름 664개 | `table()`의 등록 이름 실측. `length`/`array_len`처럼 같은 함수의 별칭 포함 |
| **Stdlib (.sura)** | `stdlib/*.sura` | 683라인 / 8모듈 | 구형 문법 기반 — 갱신 필요 |
| **에디터 확장** | `sura-vscode/` | 2,480라인 | 문법 강조, 디버그 어댑터, 워크스루 |
| **LSP** | `main.cpp --lsp` | 엔진 내장 | 언어 서버 모드. 확장이 `vscode-languageclient`로 기동 |
| **테스트** | `tests/*.sura` | 129개 파일 | `run_stable_tests.ps1`로 실행 |

### 3. 핵심 엔진 기술

1. **NaN-Boxing** — 64비트 double의 NaN 공간에 타입 태그와 포인터를 패킹해 값을 8바이트로 표현 (`value.hpp`).
2. **레지스터 바이트코드 VM** — 스택 VM 대비 명령어 수가 적은 레지스터 머신. GCC/Clang에서는 computed-goto 디스패치, 반복(iterative) 프레임 방식이라 Sura 함수 호출에 C++ 재귀를 쓰지 않음 (`jit_vm.hpp`).
3. **부분 네이티브 JIT** — 상수/이동/증명된 산술·비교·점프 등 **옵코드 부분집합만** 기계어로 컴파일하고, 나머지는 VM으로 폴백 (`jit_native.hpp`). 전면 JIT가 아니며 플랫폼별 지원 범위는 `jit_target.hpp`와 `README.md`에 명시.

### 4. 알려진 한계

- 네이티브 JIT는 옵코드 부분집합만 처리하며, Linux x86-64 / ARM64는 baseline 수준입니다.
- JS/WASM 타깃과 외부 레지스트리 운영은 `compatibility.json` 기준 **experimental**입니다.
- Sura는 OS 샌드박스가 아닙니다. 신뢰되지 않은 코드를 실행하지 마십시오 (`SECURITY.md`).
- 미해결 항목은 `REMAINING_ISSUES.md`를 참조하십시오.

---

## 언어 문법 요약 (Cheat Sheet)

- **할당**: `x is 10`
- **조건**: `if ... then ... elif ... else ... end`
- **반복**: `repeat 5 do ... end`, `while ... do ... end`, `for i in 1 to 10 do ... end` (`step -2` 지원)
- **함수**: `func add(a: number, b: number) -> number do ... end` (타입 어노테이션은 선택)
- **객체 지향**: `class Animal do ... end`, `class Dog extends Animal do ... end`, `super.method()`
- **예외**: `try ... catch e ... end` (`finally` 지원), `throw "메시지"`
- **패턴 매칭**: 대상은 `when`, 분기는 `is` / `in` / `else`

  ```
  when x do
      is 1 then print "one"
      in 2 to 10 then print "range"
      else then print "other"
  end
  ```
- **문자열 보간**: `"안녕, {name}!"`
- **연산**: 비트 연산(`&`, `|`, `^`, `~`, `<<`, `>>`) 및 2진수/16진수 리터럴 지원
