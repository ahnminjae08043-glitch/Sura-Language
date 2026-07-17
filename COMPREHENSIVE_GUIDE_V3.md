# Sura 현재 문법 종합 가이드

최종 업데이트: 2026-05-22

이 파일 이름에는 `V3`가 남아 있지만, 내용은 예전 v3 문서가 아닙니다. 현재 저장소의 표준 실행 파일인 `SuraLanguage.exe`가 실제로 파싱하고 실행하는 문법을 기준으로 다시 정리한 요약 문서입니다. 공식 문법 기준은 [Guide/GUIDE.md](Guide/GUIDE.md)이며, 해당 문서의 Sura 코드 블록은 `tools/sura_guide_syntax_smoke.ps1`로 검사합니다. 문법 가이드는 계획 중인 기능이 아니라 현재 네이티브 엔진 기준으로만 씁니다.

## 기준

Sura는 독립 언어입니다. Python이나 JavaScript 코드를 자동 번역해서 실행하는 문법은 공식 범위가 아닙니다. Python, FFI, plugin, JS/WASM 변환은 선택 기능 또는 별도 타깃입니다.

권장 호출 방식:

```sura
print("hello")
grid_init(80, 25)
```

호환용 명령형 호출:

```sura
print "hello"
grid_init 80 25
```

지원하지 않는 문법:

- `let`, `var`, `const`
- Python의 `def name(...):`
- JavaScript/C의 `{ ... }` 문장 블록
- `try`나 `catch` 뒤에 `do`를 붙이는 예외 처리 블록

## 현재 지원 범위

- 값: 숫자, 문자열, `true`, `false`, `nil`, 배열, 딕셔너리
- 변수: `name is value`, `name: type is value`, 재대입, 복합 대입
- 제어: `if/elif/else`, `else if`, 한 줄 `if ... then ... else ...`, `while`, `repeat`, `for`, `break`, `continue`
- 함수: `func name(args) do ... end`, `func(args) do ... end`, 타입 힌트, `return`, 함수 값, 클로저
- 객체: `class`, `new`, `self`, `extends`, `super`, `struct`, `enum`
- 컬렉션: `items[0]`, `dict.key`, `dict["key"]`, 필드/인덱스 대입
- 문자열: `"hello {name}"`, 보간 안의 필드/인덱스/간단한 표현식
- 예외: `try`, `catch e`, 선택적 `finally do`, `throw`
- 게임 API: `key_down`, `readkey_timeout`, `grid_*`, `mouse_*`, `win_*`

시간 대기는 현재 엔진에서 함수형과 호환용 명령형을 모두 지원합니다.

```sura
wait(16)
sleep_ms(16)
sleep(16)
sleep 16
```

## 실행

```powershell
sura app.sura
sura --jit app.sura
sura --profile app.sura
sura --check app.sura
sura --repl
```

한국어 진단:

```powershell
sura --lang ko app.sura
$env:SURA_LANG = "ko"
sura app.sura
```

## 기본 예제

```sura
name is "Sura"
score is 10

if score > 0 then
  print("positive {name}")
end

if score == 0 then print("zero") else print("non-zero")

func add(a, b) do
  return a + b
end

print(add(2, 3))

double is func(value) do
  return value * 2
end

print(double(4))
```

## 컬렉션

```sura
player is {name: "Ari", hp: 100}
items is ["sword", "shield"]

print("player={player["name"]}, hp={player.hp}")
print(items[0])
```

```sura
use array

numbers is [3, 1, 2]
print(array.sort(array.clone(numbers)).join(","))
print(array.range(2, 7, 2).join(","))
print(array.unique([1, 1, 2]).join(","))
print(array.flatten([[1, 2], [3]]).join(","))
```

```sura
use dict

meta is {name: "sura", kind: "lang", score: 9}
print(meta.keys().contains("name"))
print(dict.values({a: 1, b: 2}).contains(1))

picked is dict.pick(meta, ["name"])
omitted is dict.omit(meta, ["kind"])
merged is dict.merge({a: 1}, {a: 3, b: 2})

print(picked.name)
print(omitted.score)
print(merged.a)
```

## 반복문

```sura
total is 0

for n in 1 to 5 do
  total += n
end

for n in 1 ~ 3 do
  total += n
end

while total < 20 do
  total += 1
end

for n in 5 to 1 step -2 do
  total += n
end
```

## 클래스

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

## 예외 처리

```sura
try
  throw "failed"
catch e
  print("error: {e}")
finally do
  print("done")
end
```

## 콘솔 게임 API

```sura
pressed is key_down("space")
last_key is readkey_timeout(16)

grid_init(10, 5)
grid_clear()
grid_set(2, 2, "@", "green")
grid_draw()
```

윈도우 그래픽은 먼저 `win_init(...)`이 성공해야 합니다.

```sura
if win_init(320, 200, "Sura Window") then
  win_clear(8, 12, 20)
  win_rect(20, 20, 40, 30, 50, 160, 220)
  win_text("Sura", 10, 10, 255, 255, 255)
  win_update()
end
```

3D는 별도 3D 문법이 아니라 `vector`/`graphics3d` 보조 API와 `win_*` 렌더링 조합입니다. 자세한 기준은 [Guide/GUIDE.md](Guide/GUIDE.md)를 보세요.
