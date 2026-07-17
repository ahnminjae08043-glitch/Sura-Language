# Sura에 기여하기

이 저장소의 문법과 현재 지원 범위는 `Guide/GUIDE.md`, `reference.html`,
`SCOPE.md`, `COMPATIBILITY.md`를 기준으로 한다. 문서에 없는 동작을 추측해서
구현하거나 예제에 Python·JavaScript 문법을 섞지 않는다.

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

## 빌드와 검사

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

변경 범위에 맞는 작은 smoke test를 먼저 실행하고, 제출 전에는 다음 계약을
확인한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_compatibility_gate.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_guide_syntax_smoke.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_ci_coverage_gate.ps1
```

parser, bytecode loader, VM, JIT, GC 또는 FFI 경계를 바꾸면 관련 안전성 smoke와
sanitizer workflow도 갱신한다. Windows의 MinGW 설치에 ASan/UBSan library가 없을
수 있으므로 최종 sanitizer 검사는 Ubuntu CI가 담당한다.

## 패키지 기여

```powershell
surapkg new my_package
surapkg test .\my_package
surapkg quality .\my_package --json quality.json
surapkg publish .\my_package --dry-run --json publish-check.json
```

`publish --dry-run`은 registry를 수정하지 않는다. 현재 저장소의 local registry
index는 `registry/index.json`이며, 외부 registry URL은 환경 설정에 따라 달라진다.
공개 package 제안에는 source, test, README, license, version과 dry-run 결과를
포함한다.

## Pull request 확인 항목

- 변경 이유와 사용자가 보게 되는 동작을 설명한다.
- 새 동작과 실패 동작을 재현하는 test를 추가한다.
- 문법·API·CLI가 바뀌면 guide, reference generator와 VS Code metadata를 확인한다.
- source·bytecode·release format·Plugin/FFI ABI가 바뀌면 호환성 계약을 갱신한다.
- 성능 주장은 같은 입력, warm-up, 반복 수와 원시 결과가 있는 benchmark로만 한다.
- 구현하지 않은 기능이나 실행하지 않은 검사를 완료된 것으로 적지 않는다.
