# 마스터 프롬프트 — Sura JIT 성능 최적화 세션

**목표:** Sura 언어의 네이티브 JIT을 "의미적으로 완전"에서 "**성능적으로 완전**"으로 끌어올린다.

---

## 현재 상태 (세션 시작 전 반드시 읽을 것)

### 프로젝트 경로
- **원본:** `C:\Users\user\OneDrive\문서\Project\Sura-Language`
- **워크트리:** `.claude\worktrees\nifty-wilbur-629e49` (사용자가 따로 지정하지 않으면 여기서 작업)
- **GitHub:** `https://github.com/ahnminjae0804-source/Sura-Language` 브랜치 `claude/nifty-wilbur-629e49`

### 빌드 환경
- Windows + MinGW-w64 (`C:\msys64\mingw64\bin\g++.exe`)
- 빌드 명령 (PowerShell):
  ```powershell
  cd "C:\Users\user\OneDrive\문서\Project\Sura-Language"
  $env:PATH = "C:\msys64\mingw64\bin;" + $env:PATH
  cmd /c "g++ -O2 -std=c++17 main.cpp gc.cpp -static-libgcc -static-libstdc++ -lkernel32 -o SuraEngine_new.exe 2>err.txt"
  if (Test-Path SuraEngine_new.exe) { Remove-Item SuraEngine.exe -Force -ErrorAction SilentlyContinue; Rename-Item SuraEngine_new.exe SuraEngine.exe }
  ```
- 실행: `.\SuraEngine.exe <파일.sura>` 또는 `.\SuraEngine.exe --jit <파일.sura>`

### ⚠️ 알려진 환경 이슈 (반드시 준수)
1. **한글 경로 + MinGW**: `std::filesystem::weakly_canonical`이 한글 경로에서 throw할 수 있음. 이미 `jit_compiler.hpp`의 IMPORT 처리에 try/catch 방어 있음. 유사 코드 작성 시 같은 패턴 사용.
2. **`windows.h` 피하기**: `TRUE`, `FALSE`, `IN`, `NEW`, `ERROR` 등 매크로가 `TT` enum과 충돌함. `main.cpp`처럼 `extern "C" __declspec(dllimport)` 직접 선언으로 해결.
3. **`inst` 식별자 금지**: `jit_vm.hpp`에 `#define inst (*cur)` 매크로 있음. 파라미터/변수 이름으로 `inst` 쓰지 말 것. `ins`, `instr`, `ii` 등 사용.
4. **PowerShell UTF-8 BOM**: `Set-Content -Encoding utf8`은 BOM 추가함. BOM 피하려면 `[System.IO.File]::WriteAllText($path, $content, (New-Object System.Text.UTF8Encoding $false))` 사용.
5. **OneDrive 파일 락**: 가끔 `SuraEngine.exe` 삭제/교체가 락으로 실패. 해결: `SuraEngine_new.exe`로 빌드 후 rename.

### 현재 JIT 아키텍처 (핵심 파일)
| 파일 | 역할 |
|-----|------|
| `jit_alloc.hpp` | VirtualAlloc 기반 RWX 메모리 (ExecCode 클래스) |
| `jit_x64.hpp` | x86-64 명령어 인코더 (X64Emitter) |
| `jit_native.hpp` | 바이트코드 → 기계어 컴파일러 (NativeCompiler::compile) |
| `jit_vm.hpp` | VM + C 트램펄린 (sura_jit_call, sura_jit_dot_get 등) |
| `jit_op.hpp` | JitOp enum, JitInst/Chunk/FuncInfo/MethodInfo 구조체 |

### 네이티브 함수 ABI (Win64)
- 서명: `uint64_t fn(JitVM* vm, Value* R, const Value* consts)`
- 입력 레지스터: RCX=vm, RDX=R, R8=consts
- 반환: RAX에 Value 비트 (`Value::raw_bits()`)
- 프롤로그에서 callee-saved로 이동: RBX=R, R12=consts, R13=vm
- **R[i] 접근**: `[RBX + i*8]` — 모든 바이트코드 레지스터는 메모리 기반
- **상수**: `[R12 + i*8]`
- 3개 push(RBX/R12/R13) 후 RSP는 16바이트 정렬됨 → CALL 전에 `sub rsp, 48` 같은 짝수 바이트 조정 필수

### 성능 현실 (벤치 결과)
| 워크로드 | 인터프리터 | JIT | 배율 |
|---------|-----------|-----|------|
| `fib(30)` 순수 숫자 재귀 | ~68 ms | ~46 ms | **1.47x** ✅ |
| `bench_physics.sura` (Vec2 add/scale 100k x) | ~75 ms | ~85 ms | **0.88x** ❌ |

**fib는 빠르지만 OO 코드는 wash 또는 저하.** 이게 이번 세션이 해결할 문제.

---

## 🎯 이번 세션 목표

### 성공 기준 (하나라도 달성 시 성공)
1. **`bench_physics.sura`에서 JIT이 인터프리터 대비 1.5x 이상 빠를 것** (wash 해결)
2. 기존 `fib(30)` speedup 유지 (1.4x 이상)
3. 모든 기존 테스트 정확성 유지 (`test_truthy_jit.sura`, `test_method_jit.sura`, `market_tycoon.sura`)

### 우선순위 작업 (위에서부터)

#### 1. 인라인 IC — DOT_GET/DOT_SET (가장 큰 이득)
**현재 문제:** DOT_GET은 `sura_jit_dot_get` C 헬퍼 호출 → 함수 호출 오버헤드 + 해시테이블 lookup. 인터프리터(computed-goto + `ic_cache`)보다 6-10배 느림.

**해결:** 네이티브 코드에 직접 인라인 IC 삽입:
```
; R[a] = R[b].prop (인라인 IC)
mov rax, [rbx + b*8]          ; obj Value bits
; 가드 1: bit 63 set (obj 포인터)
test rax, rax
jns .slow_path                ; 음수 아니면 obj 아님
; 가드 2: 캐시된 class 확인
; GCInstance 메모리 레이아웃:
;   offset 0: ObjType obj_type (enum, 4 bytes)
;   offset 4: bool marked (1 byte + padding)
;   offset ?: std::string class_name (libstdc++ = 32 bytes with SSO)
;   offset ?: std::vector<Value> fields (24 bytes: 3 pointers)
;   ※ 정확한 오프셋은 런타임에 결정. 컴파일러에 constexpr 도우미 필요
; 간단한 접근: GCInstance* 자체를 cache key로 쓰지 말고,
;   class_name의 data() 포인터 혹은 rt_classes의 안정된 포인터 사용
mov rcx, 0x0000FFFFFFFFFFFF
and rax, rcx                  ; GCInstance*
mov rdx, [rax + <CLASS_NAME_OFFSET>]  ; string.data() or equivalent
cmp rdx, <CACHED_CLASS_PTR>
jne .slow_path
; fast: 필드 벡터에서 offset 로드
mov rcx, [rax + <FIELDS_BASE_OFFSET>]  ; vector<Value>._M_start 포인터
mov rax, [rcx + <FIELD_OFFSET>*8]       ; R[a] = fields[offset]
mov [rbx + oa], rax
jmp .done
.slow_path:
  ; 기존 헬퍼 호출 경로 (동일)
.done:
```

**캐시 채움 전략 (옵션):**
- **옵션 A**: 컴파일 시점의 `inst.ic_cache` 값을 즉치(immediate)로 박음. 첫 실행은 interpreter가 채워야 하므로 "cold JIT"과 "warm JIT" 구분 필요.
- **옵션 B**: 자가 수정 코드 (self-modifying). 첫 호출 때 slow_path가 코드의 imm32 바이트를 덮어씀. Windows에서 `VirtualProtect` + `FlushInstructionCache` 필요.
- **옵션 C**: Inline data slot. 코드에 placeholder 둬서 helper가 바이트 수정. B와 유사.

**MVP 권장:** 옵션 A — JIT 컴파일을 LAZY로 늦춰서 이미 interpreter가 돈 후의 `inst.ic_cache` 값을 사용. JIT 함수 호출 횟수 > N (예: 10) 일 때 컴파일. 이때 대부분 ic_cache가 이미 채워져 있음.

**클래스 지문 (class fingerprint):**
- `GCInstance::class_name` 의 주소나 내용 비교 — 단순 문자열 비교는 비쌈
- 해결: `JitClassInfo*` 포인터를 `GCInstance`에 직접 저장 (멤버 추가). `value.hpp` 수정 필요.
- 또는 `rt_classes` 맵에 안정된 key 포인터 유지.

**GCInstance 레이아웃 조사:**
```cpp
// jit_native.hpp에 offset 상수 추가
static constexpr size_t INST_CLASS_NAME_OFFSET = offsetof(GCInstance, class_name);
static constexpr size_t INST_FIELDS_OFFSET     = offsetof(GCInstance, fields);
// vector<Value>의 첫 원소 포인터 오프셋 (libstdc++ 기준 0)
static constexpr size_t VECTOR_DATA_OFFSET     = 0;
```

#### 2. 인라인 arithmetic + 타입 가드
**현재:** ADD/SUB/MUL/DIV는 이미 `addsd` 등으로 네이티브 인라인됨. **단, operand가 숫자라는 가정** (UB risk).

**해결:** 가드 추가:
```
; R[a] = R[b] + R[c]  (type-guarded)
mov rax, [rbx + ob]
mov rcx, rax
shr rcx, 48
cmp cx, 0x7FFC                ; NaN-boxed sentinel
jae .mixed                    ; non-number
mov rdx, [rbx + oc]
mov rcx, rdx
shr rcx, 48
cmp cx, 0x7FFC
jae .mixed
; fast path: both are numbers
movq xmm0, rax
movq xmm1, rdx
addsd xmm0, xmm1
movq rax, xmm0
mov [rbx + oa], rax
jmp .done
.mixed:
  ; 헬퍼 호출 또는 deopt
  ; 간단: sura_jit_arith_slow(vm, R, inst*)
.done:
```

#### 3. 모노모픽 METHOD_CALL 인라인
**현재:** `sura_jit_method_call` 헬퍼 호출 → find_method lookup → execute_frame.

**해결:** 프로파일러 데이터 활용. 콜 사이트가 단일 `(class, method)` 페어로 모노모픽이면:
- class 가드 후 바로 `NativeFunc*`로 CALL
- 미스 시 헬퍼 폴백

필요한 수정:
- `JitInst`에 modular cache slot 추가 or 별도 inline cache 구조
- Profiler가 이미 수집하는 mono/poly 데이터 접근

---

## 단계별 계획

### Phase 1 (필수 — 성공 기준 1 달성)
1. `GCInstance`에 `JitClassInfo*` 포인터 필드 추가 (value.hpp 수정). 인스턴스 생성 시 rt_classes 참조로 설정.
2. `jit_x64.hpp`에 필요한 새 명령어 추가:
   - `test_rr` (REX.W 85 /r)
   - `cmp_mem_imm32` (short form for [reg+disp] vs imm32)
   - 기타 jump cond 변형
3. `jit_native.hpp`의 `O::DOT_GET` 케이스 재작성:
   - Fast path: 인라인 IC 체크 + 직접 필드 로드
   - Slow path: 기존 헬퍼 호출
   - Cache: 컴파일 시점의 `inst.ic_cache` 및 수신자 클래스 확정
4. 벤치: `bench_physics.sura`로 1.5x+ 확인

### Phase 2 (있으면 좋음)
5. 인라인 arithmetic 타입 가드 추가 (Phase 1이 충분히 빠르면 스킵 가능)
6. 모노모픽 METHOD_CALL 인라인

### Phase 3 (검증)
7. 회귀 테스트: `test_truthy_jit.sura`, `test_method_jit.sura`, `market_tycoon.sura`, `bench_full.sura`
8. JIT 끈 상태와 켠 상태 결과 100% 일치 확인
9. 커밋 각 phase 별로 (최소 2-3개 커밋)

---

## 커밋 규칙
- **각 Phase 끝날 때마다 즉시 커밋 + push** (이전 세션에서 강조됨)
- 커밋 메시지 형식 (한국어):
  ```
  perf(jit): <한 줄 요약>

  <상세>

  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  ```
- Push: `git push` — 브랜치 `claude/nifty-wilbur-629e49` 사용

---

## 참고: 기존 커밋 히스토리 (이전 세션 작업)
```
3d3d46d9 feat(jit): DOT_GET / DOT_SET + 클래스 메서드 JIT
24335194 feat(jit): METHOD_CALL 네이티브 지원
f7eb5778 feat(jit): 일반 값 truthiness
257e7791 feat(jit): CALL_FUNC / LOAD_GLOBAL / STORE_GLOBAL
88a6871f feat: struct + import/export
a895367f feat: enum + Unity FFI
7ebfa23a fix(compiler): 레지스터 충돌 버그
09fff2a5 feat: 표준 라이브러리 + 스택 트레이스
ef39e0f8 feat: 바이트코드 I/O + 프로파일러 + 람다 + 패턴매칭 + JIT 기본
```

---

## 주의사항
- **완벽한 JIT 한 세션에 못 만든다.** Phase 1만 완성해도 큰 진전.
- 실패해도 되는 범위 / 디옵트 경로 항상 유지 — 정확성 > 성능.
- x86-64 바이트 인코딩 실수 시 크래시 (디스어셈블러 없는 환경). **기존 `jit_x64.hpp` 패턴을 그대로 따를 것.**
- 사용자 언어: **한국어**. 기술 용어는 영어 OK. 설명은 짧게.

---

## 첫 메시지 후 할 일
1. 이 파일 읽기
2. 현재 `bench_physics.sura` 돌려서 baseline 확인
3. `GCInstance` 레이아웃 (`value.hpp`) 확인
4. `jit_native.hpp::emit_op`의 `O::DOT_GET` 현재 코드 확인
5. Phase 1 시작

**시작 명령어:**
```
Sura JIT 성능 최적화 세션 시작. MASTER_PROMPT_JIT_PERF.md 읽고 Phase 1 진행.
```
