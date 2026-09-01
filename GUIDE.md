# Sura 언어 가이드

최신 공식 문법 가이드는 [Guide/GUIDE.md](Guide/GUIDE.md)에 있습니다. 이 루트 문서는 빠른 요약이고, 실제 기준은 현재 저장소의 표준 실행 파일 `SuraLanguage.exe`와 `tools/sura_guide_syntax_smoke.ps1`로 검증되는 공식 가이드입니다. 예제는 “앞으로 넣고 싶은 문법”이 아니라 현재 엔진이 파싱하고 실행하는 문법만 기준으로 둡니다.

Sura만으로 신경망을 학습하는 방법은 [Guide/AI.md](Guide/AI.md)에 있습니다. 연속 메모리 Tensor와 사용자 정의 자동미분 그래프는 [Guide/AUTOGRAD.md](Guide/AUTOGRAD.md)를 참고하세요.

Sura는 `.sura` 파일을 직접 작성해서 실행하는 독립 언어입니다. Python 코드를 자동으로 Sura로 번역하는 언어가 아니며, Python/JavaScript/C/FFI 연동은 기존 생태계를 호출하기 위한 선택 기능입니다.

현재 문법 기준은 저장소의 최신 `SuraLanguage.exe`입니다. 설치된 `sura` 명령이 오래된 파일을 가리키면 `grid_init`, `win_init`, `readkey_timeout`, `sleep` 같은 최신 API가 없다고 나올 수 있으니 `surapkg doctor`로 PATH를 확인하세요.

최종 업데이트: 2026-07-10. 가이드의 `sura` 코드 블록은 `tools/sura_guide_syntax_smoke.ps1`에서 실제 엔진 파서로 검사합니다.

## 빠른 실행

```powershell
sura app.sura
sura --jit app.sura
sura --check app.sura
sura --repl
surapkg init my_app
```

저장소에서 직접 실행:

```powershell
.\SuraLanguage.exe .\app.sura
.\SuraLanguage.exe --check .\app.sura
```

PowerShell에서 경로에 공백이나 한글이 있으면:

```powershell
& "C:\Users\user\AppData\Local\Programs\Sura\bin\SuraLanguage.exe" "C:\path\app.sura"
```

한국어 진단:

```powershell
sura --lang ko app.sura
$env:SURA_LANG = "ko"
sura app.sura
```

## 현재 지원 범위

- 변수, 숫자, 문자열, bool, nil, 배열, 딕셔너리
- `if/elif/else`, `else if`, 한 줄 `if ... then ... else ...`, `while`, `repeat`, `for`, `break`, `continue`
- `func name(args) do ... end`, `func(args) do ... end`, 타입 힌트, `return`, 함수 값, 클로저
- `class`, `new`, `self`, `super`, `struct`, `enum`
- 문자열 보간 `{player["name"]}`, 필드 접근, 인덱싱, 간단한 표현식
- `try/catch/finally`, `throw`, `match`, `when`
- 밀리초 대기: `wait(ms)`, `sleep_ms(ms)`, `sleep(ms)`, `sleep ms`
- `console`, `math`, `array`, `dict`, `random`, `test`, `vector`, `grid_*`, `win_*`, `mouse_*` API
- 딕셔너리 API: `keys`, `values`, `items`, `merge`, `pick`, `omit`

권장 문법은 함수 호출형입니다.

```sura
name is "Sura"
score is 42

print("hello {name}")
print("score = {score}")
```

블록은 `then`/`do`와 `end`를 씁니다.

```sura
if score > 0 then
  print("positive")
end

for n in 1 to 3 do
  print(n)
end

try
  throw "failed"
catch e
  print(e)
finally do
  print("done")
end
```

호환용 명령형 호출도 일부 지원하지만 새 코드에서는 함수 호출형을 기준으로 쓰세요.

```sura
print "hello"
grid_init 80 25
```

지원하지 않는 문법:

- `let`, `var`, `const`
- Python의 `def ...:`와 들여쓰기 블록
- JavaScript/C의 `{ ... }` 문장 블록
- `try`나 `catch` 뒤에 `do`를 붙이는 예외 처리 블록
- Python 자동 번역 문법

JS/WASM 변환기는 별도 타깃입니다. 네이티브 Sura 문법이 된다고 해서 모든 동적 기능이 JS/WASM에서도 완전 동일하게 lowering된다는 뜻은 아닙니다.
