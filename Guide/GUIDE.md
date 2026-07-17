# Sura 문법 가이드

최종 업데이트: 2026-07-15

처음 설치하고 프로젝트를 만드는 과정은 [10분 시작 가이드](START_HERE.md)를 먼저 읽으세요. 네이티브 신경망 생성·학습·평가·저장은 [Sura 네이티브 AI 가이드](AI.md), 연속 메모리 Tensor와 사용자 정의 gradient 그래프는 [Tensor와 자동미분 가이드](AUTOGRAD.md), 영상의 ASCII/UTF-8 문자 프레임 변환은 [미디어 가이드](MEDIA.md)를 참고하세요.

이 문서는 현재 저장소의 표준 실행 파일 `SuraLanguage.exe`가 실제로 파싱하고 실행하는 문법만 기준으로 정리한 공식 가이드입니다. 예전 문서의 v3 홍보 문구, 아직 구현되지 않은 문법, Python 자동 번역처럼 오해될 수 있는 설명은 제외했습니다. 문법, 표준 라이브러리 API, JS/WASM 변환 범위는 서로 다르므로 이 문서에서는 먼저 네이티브 Sura 문법을 기준으로 설명합니다.

기존 프로그램을 어느 버전까지 유지하는지와 안정·플랫폼 한정·실험 기능의 구분은 [호환성과 지원 정책](../COMPATIBILITY.md)에 명시합니다. 새 기능을 문서에 추가할 때는 해당 등급과 자동 검증 범위도 함께 갱신해야 합니다.

검증 기준은 네이티브 Sura 실행기입니다. 문서의 `sura` 코드 블록은 `tools/sura_guide_syntax_smoke.ps1`에서 `SuraLanguage.exe`로 검사하고, 핵심 런타임 예제는 같은 스모크에서 실제 실행까지 확인합니다.

설치된 `sura` 명령이 오래된 실행 파일을 가리키면 `grid_init`, `win_init`, `readkey_timeout`, `sleep` 같은 최신 API가 없다고 나올 수 있습니다. 이때는 `surapkg doctor`로 PATH 충돌을 확인하거나 최신 설치 경로의 `SuraLanguage.exe`를 직접 실행하세요.

## 기준

Sura는 `.sura` 파일을 직접 작성해서 실행하는 독립 언어입니다. Python 코드를 Sura로 자동 번역해서 실행하는 언어가 아닙니다. Python, JavaScript, C, FFI, plugin 연동은 기존 생태계를 호출하거나 별도 타깃으로 내보내기 위한 선택 기능입니다.

새 코드에서는 함수 호출형을 기본으로 씁니다.

```sura
print("hello")
grid_init(80, 25)
```

일부 내장 명령은 기존 스크립트 호환을 위해 공백 기반 명령형 호출도 지원합니다.

```sura
print "hello"
grid_init 80 25
```

지원하지 않는 문법:

- `let`, `var`, `const`
- Python의 `def name(...):`와 들여쓰기 블록
- JavaScript/C의 `{ ... }` 문장 블록
- `try`나 `catch` 뒤에 `do`를 붙이는 예외 처리 블록
- Python 코드를 자동 번역해서 실행하는 문법

## 실행

```powershell
sura app.sura
sura --jit app.sura
sura --profile app.sura
sura --check app.sura
sura --repl
surapkg new my_app
cd my_app
surapkg run
surapkg test
```

설치본에 포함된 실행 예제를 찾거나 독립 프로젝트로 복사할 수 있습니다.

```powershell
surapkg examples
surapkg examples games_3d --json
surapkg example games_3d/wireframe_cube my_3d_demo
```

목록에는 `windows-graphics`, `ffmpeg`, `cuda` 같은 선택 요구 사항이 함께 표시됩니다. 생성된 프로젝트의 `sura.example.json`에는 원본 예제 ID와 SHA-256이 기록됩니다.

저장소에서 직접 실행:

```powershell
.\SuraLanguage.exe .\app.sura
.\SuraLanguage.exe --check .\app.sura
```

PowerShell에서 경로에 공백이나 한글이 있으면 호출 연산자 `&`와 따옴표를 같이 씁니다.

```powershell
& "C:\Users\user\AppData\Local\Programs\Sura\bin\SuraLanguage.exe" "C:\path\app.sura"
```

한국어 진단:

```powershell
sura --lang ko app.sura
$env:SURA_LANG = "ko"
sura app.sura
```

환경 변수 이름은 `SURA_LANG`입니다. 한국어 진단 설정은 `SURA_LANG=ko`이고, 기본 영어 진단은 비워 두거나 영어 설정을 사용합니다. 문서와 테스트 파일은 UTF-8로 저장하세요.

## 지원 문법 요약

| 범위 | 실제 지원 문법 |
| --- | --- |
| 값 | 숫자, 문자열, `true`, `false`, `nil`, 배열, 딕셔너리 |
| 변수 | `name is value`, `name: type is value`, 재대입, 복합 대입 |
| 연산 | 산술, 비교, 논리, 포함 검사, 비트/시프트, 삼항식 |
| 블록 | `if/elif/else`, `else if`, 한 줄 `if ... then ... else ...`, `while`, `repeat`, `for`, `break`, `continue` |
| 함수 | `func name(args) do ... end`, `func(args) do ... end`, 타입 힌트, `return`, 함수 값, 클로저 |
| 객체 | `class`, `new`, `self`, `extends`, `super`, `struct`, `enum` |
| 컬렉션 | `items[0]`, `dict.key`, `dict["key"]`, 필드/인덱스 대입 |
| 문자열 | `"hello {name}"`, 보간 안의 변수/필드/인덱스/간단한 표현식 |
| 제어 | `match`, `when`, `try/catch/finally`, `throw` |
| 시간 | `wait(ms)`, `sleep_ms(ms)`, `sleep(ms)`, `sleep ms` |
| 게임 API | `key_down`, `readkey_timeout`, `grid_*`, `mouse_*`, `win_*` |

## 실제 문법 형태

Sura 블록은 Python 들여쓰기나 JavaScript 중괄호가 아니라 `then`/`do`로 시작하고 `end`로 닫습니다.

```sura
if ready then
  print("ready")
end

while running do
  break
end

func add(a, b) do
  return a + b
end
```

실제로 지원되는 주요 문장 형태:

```sura
name is "Sura"
name: string is "Sura"
name is "Next"

if score > 0 then print("positive") else print("zero")

for n in 1 to 5 do
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

현재 문법이 아닌 형태:

```text
let name = "Sura"
def add(a, b):
if (score > 0) { print(score) }
try-with-do block
catch-with-do block
```

## 주석

```sura
# 한 줄 주석
// 한 줄 주석
```

## 값과 변수

변수 선언과 대입에는 `is`를 씁니다. 따옴표가 없는 한글은 문자열이 아니라 변수 이름으로 해석되므로 문자열은 따옴표로 감싸야 합니다.

```sura
name is "Sura"
score is 42
enabled is true
missing is nil

print("hello {name}")
print("score = {score}")
```

타입 힌트는 파서, AST, 검사 경로에서 보존됩니다. 타입 오류는 기본 실행,
`--check`, 테스트, 컴파일에서 실행 차단 오류입니다. 오래된 동적 코드를
마이그레이션하는 동안에만 `--legacy-types`를 명시하면 예전처럼 경고 후
실행할 수 있습니다. 바이트코드 컴파일(`--compile`)과 보호 릴리스
(`--release`)에는 이 우회 옵션을 사용할 수 없습니다.

```sura
count: number is 10
title: string is "guide"

func square(x: number) -> number do
  return x * x
end
```

대표 타입 이름:

- `number`
- `int`, `float`, `double` (`number`와 같은 런타임 숫자 표현의 별칭)
- `string`
- `bool`
- `nil`
- `array`
- `dict`
- `function`
- `object`

현재 선언 타입은 `nil`을 허용하며, `if`, `while`, `and`, `or`, `not`은
런타임과 동일하게 숫자·문자열·컬렉션의 truthy 규칙을 사용합니다. 문자열과
다른 값을 `+` 또는 `+=`로 결합하면 다른 값이 문자열로 변환됩니다.

## 대입과 갱신

```sura
score is 10
score += 5
score -= 2
score *= 3
score /= 2
score %= 5
```

필드와 인덱스에도 대입할 수 있습니다.

```sura
player is {name: "Ari", hp: 100}
items is ["sword", "shield"]

player.hp is 80
player["name"] is "Rin"
items[0] is "staff"
items[1] += " item"
```

## 출력과 문자열 보간

```sura
player is {name: "Ari", score: 10}
items is ["sword", "shield"]

print("name={player["name"]}, score={player.score}")
print("first item: {items[0]}")
print("next score: {player.score + 5}")
```

문자열 보간 `{...}` 안에서는 변수, 필드 접근, 인덱싱, 간단한 표현식을 사용할 수 있습니다.

## 연산자

```sura
x is 10 + 3
y is x * 2
z is y % 5

print(x > 3 and x < 100)
print(not false)
print(true or false)
```

주요 연산:

- 산술: `+`, `-`, `*`, `/`, `%`
- 비교: `==`, `!=`, `<`, `<=`, `>`, `>=`
- 논리: `and`, `or`, `not`
- 포함 검사: `"key" in dict`, `"a" in "abc"`
- 비트/시프트: `&`, `|`, `^`, `~`, `<<`, `>>`
- 삼항식: 조건식 기반 값 선택

## 조건문

블록 조건문은 `then`과 `end`를 씁니다.

```sura
score is 85

if score >= 90 then
  print("A")
elif score >= 80 then
  print("B")
else
  print("C")
end
```

짧은 한 줄 조건문도 지원합니다.

```sura
score is 10
if score > 0 then print("positive")
if score == 0 then print("zero") else print("non-zero")
if score < 0 then print("negative") else if score > 0 then print("positive") else print("zero")
```

`elif`와 `else if`는 모두 지원됩니다. 새 코드에서는 짧게 쓸 때만 한 줄 `if`를 쓰고, 문장이 길어지면 블록 형태를 권장합니다.

## 반복문

`while`:

```sura
i is 0
while i < 3 do
  i += 1
end
```

`repeat`:

```sura
count is 0
repeat 3 do
  count += 1
end
```

범위 `for`:

```sura
total is 0
for n in 1 to 5 do
  total += n
end
```

`step`:

```sura
total is 0
for n in 2 to 6 step 2 do
  total += n
end
```

음수 `step`도 지원합니다.

```sura
total is 0
for n in 5 to 1 step -2 do
  total += n
end
```

틸드 범위:

```sura
total is 0
for n in 1 ~ 3 do
  total += n
end
```

배열 순회:

```sura
items is ["red", "green", "blue"]

for item in items do
  print(item)
end

for index, item in items do
  print("{index}: {item}")
end
```

딕셔너리 순회:

```sura
profile is {city: "Seoul", level: 1}

for key, value in profile do
  print("{key} = {value}")
end
```

`break`와 `continue`:

```sura
i is 0
while true do
  i += 1
  if i == 2 then continue
  if i == 4 then break
end
```

## 함수

```sura
func add(a, b) do
  return a + b
end

print(add(2, 3))
```

함수를 값으로 저장할 수도 있습니다.

```sura
double is func(value) do
  return value * 2
end

handler is double
print(handler(10))
```

짧은 조기 반환:

```sura
func sign(n) do
  if n > 0 then return 1
  if n < 0 then return -1
  return 0
end
```

함수 값과 클로저:

```sura
func make_counter() do
  count is 0
  func next() do
    count += 1
    return count
  end
  return next
end

counter is make_counter()
print(counter())
```

## 배열

```sura
items is ["red", "green", "blue"]

print(items.len())
print(items[0])
items.push("black")
items.insert(1, "yellow")
removed is items.remove(2)
print(items.join(","))
```

배열 모듈:

```sura
use array

numbers is [3, 1, 2]
copy is array.clone(numbers)

print(array.sort(copy).join(","))
print(array.range(2, 7, 2).join(","))
print(array.unique([1, 1, 2]).join(","))
print(array.zip([1, 2], [3, 4]).len())
print(array.flatten([[1, 2], [3]]).join(","))
```

## 딕셔너리

```sura
player is {name: "Ari", hp: 100}

print(player.name)
print(player["hp"])
player.hp is 90
player["name"] is "Rin"
```

딕셔너리 키가 동적이면 `[]`를 씁니다.

```sura
key is "hp"
player is {name: "Ari", hp: 100}
print(player[key])
```

딕셔너리 모듈:

```sura
use dict

meta is {name: "sura", kind: "lang", score: 9}

print(meta.keys().contains("name"))
print(dict.values({a: 1, b: 2}).contains(1))

item is dict.items({a: 1})[0]
print("{item.key}={item.value}")

merged is dict.merge({a: 1}, {b: 2}, {a: 3})
picked is dict.pick(meta, ["name", "missing"])
omitted is dict.omit(meta, ["kind"])

print(merged.a)
print(picked.name)
print(omitted.score)
```

`keys()`, `values()`, `items()`의 반환 순서는 런타임 딕셔너리 구현에 따라 달라질 수 있습니다. 순서가 필요한 로직은 결과 배열을 직접 정렬하거나 별도 배열로 관리하세요.

## 문자열

```sura
text is "  Sura Language  "

print(text.trim())
print(text.lower())
print(text.upper())
print(text.contains("Lang"))
print(text.starts_with("  Su"))
print(text.ends_with("  "))
print(text.split(" ").len())
print(text.trim().repeat(2))
print("7".pad_left(3, "0"))
print("go".pad_right(4, "!"))
```

## 클래스와 객체

```sura
class Player do
  func init(name) do
    self.name is name
    self.hp is 100
  end

  func damage(amount) do
    self.hp -= amount
    return self.hp
  end
end

p is new Player("hero")
print(p.damage(20))
```

상속과 `super`:

```sura
class Animal do
  func init(name) do
    self.name is name
  end

  func speak() do
    return "..."
  end
end

class Dog extends Animal do
  func speak() do
    return self.name + ": bark"
  end
end

dog is new Dog("Badu")
print(dog.speak())
```

## 구조체

```sura
struct Vec2 do
  x
  y

  func add(other) do
    return Vec2(self.x + other.x, self.y + other.y)
  end
end

v is Vec2(3, 4)
print(v.add(Vec2(1, 2)).y)
```

## enum

```sura
enum Mode do
  EASY
  HARD
end

mode is Mode.EASY
print(mode)
```

값을 직접 줄 수도 있습니다.

```sura
enum Code do
  OK is 200
  FAIL is 500
end

print(Code.OK)
```

## match

```sura
score is 10

match score
  when 10 then
    print("ten")
  when _ then
    print("other")
end
```

## when

```sura
score is 42

when score do
in 1 ~ 100 then
  print("range")
else then
  print("outside")
end
```

## 예외 처리

공식 예외 처리 문법은 `try`, `catch e`, 선택적 `finally do`, `end`입니다.

```sura
try
  throw "failed"
catch e
  print("error: {e}")
finally do
  print("done")
end
```

`try`나 `catch` 뒤에 `do`를 붙이는 예외 처리 블록은 공식 문법이 아닙니다.

## 모듈

```sura
use math
use random
use vector

print(math.clamp(120, 0, 100))
random.seed(42)
print(random.int(1, 100))
print(vector.cross([1, 0, 0], [0, 1, 0]))
```

자주 쓰는 내장 모듈:

- `math`
- `array`
- `dict`
- `string`
- `random`
- `test`
- `os`
- `console`
- `vector`
- `json`
- `fs`
- `http`
- `async`
- `autograd`
- `nn`
- `rag`
- `llm`
- `tool`

`autograd`, `nn`, `rag`, `llm`, `tool`은 언어 문법이 아니라 선택 표준 라이브러리/도구 연동 모듈입니다. `autograd`는 CPU packed dtype과 제한된 resident CUDA f32/f16/bf16 storage, typed matmul, f32 master optimizer와 checkpoint v3를 제공하며 JS/WASM으로 자동 lowering되지 않습니다.

## import

다른 `.sura` 파일을 불러올 수 있습니다.

```sura
import "./lib.sura"
```

## 테스트와 assert

```sura
assert(true)
assert_eq(1 + 1, 2)
assert_ne(1, 2)
assert_contains("sura language", "sura")
assert_type({ok: true}, "dict")
```

테스트 러너는 기본적으로 `tests/*.sura`를 찾습니다. 없으면 `test_*.sura`, `*_test.sura`, `*.test.sura`도 찾습니다.

## console API

```sura
use console

console.log("ready", 1)
console.warn("check config")
console.write("progress")
console.write_line(" 10%")
console.json({ok: true, score: 10})
```

## 시간 대기

```sura
wait(16)
sleep_ms(16)
sleep(16)
sleep 16
```

밀리초 대기는 `wait(ms)`, `sleep_ms(ms)`, `sleep(ms)`를 모두 지원합니다. 기존 콘솔 게임 스크립트 호환을 위해 `sleep 16` 같은 명령형 호출도 지원하지만, 새 문서 예제에서는 함수 호출형을 우선합니다.

## 콘솔 게임 API

키 입력:

```sura
pressed is key_down("space")
last_key is readkey_timeout(16)

key_down "w" w_pressed
readkey_timeout input_key 16
```

터미널 그리드:

```sura
grid_init(10, 5)
grid_clear()
grid_set(2, 2, "@", "green")
grid_draw()
```

명령형 호출도 지원합니다.

```sura
grid_init 10 5
grid_clear
grid_set 2 2 "@" "green"
grid_draw
```

마우스:

```sura
mouse is mouse_pos()
left_down is mouse_down("left")

mouse_pos mx my
mouse_down "left" left_pressed
```

윈도우 그래픽:

```sura
if win_init(320, 200, "Sura Window") then
  win_focus()
  win_clear(8, 12, 20)
  win_rect(20, 20, 40, 30, 50, 160, 220)
  win_circle(80, 60, 10, 240, 90, 90)
  win_line(0, 0, 319, 199, 255, 255, 255)
  win_text("Sura", 10, 10, 255, 255, 255)
  win_poll()
  win_update()
end
```

`win_rect`, `win_circle`, `win_line`, `win_text`, `win_update`는 먼저 `win_init(...)`이 성공한 뒤 호출해야 합니다.

## 3D 상태

Sura에 별도 3D 엔진 문법이 있는 것은 아닙니다. 현재 3D 예제는 `vector`/`graphics3d` 보조 API, 직접 투영 계산, `win_*` 2D 렌더링을 조합해서 만듭니다.

```sura
use vector

forward is [0, 0, 1]
right is [1, 0, 0]
normal is vector.cross(forward, right)
print(normal)
```

## JS/WASM 변환 범위

일반 실행과 JIT가 기본 경로입니다. JavaScript/WASM 변환기는 별도 타깃이며, 모든 동적 기능이 네이티브 VM과 완전히 같은 범위로 지원된다고 보지 않습니다.

- JS 타깃은 많은 문법을 따라가지만, VM과 완전 동일하다고 보장하지 않습니다.
- WASM 타깃은 계속 확장 중이며, 일부 동적 AST/바이트코드 lowering은 아직 목표 작업으로 남아 있습니다.
- 문법 지원 여부와 타깃 변환 지원 여부는 구분합니다.

## Release package

Release package는 `.sura` source를 SURB v3 register bytecode payload로 compile해 SRP v5 container에 저장합니다.

```powershell
sura --release app.sura --out app.sura.srp
surapkg protect --exe
```

SRP v5에는 bytecode·literal·constant payload, randomized nonce와 integrity seal이 들어갑니다. line·local·function·parameter debug name은 제거되고, 실행에 필요한 class name·method map key·global name·constant는 남습니다. loader는 seal과 선택한 release key·license·id·expiry 조건을 실행 전에 검사합니다. `surapkg protect-verify`는 package와 launcher에서 source 전체, source line, string literal, key/license byte pattern을 검사합니다.
