# 수라(SURA) 언어 가이드 v2.0

수라는 한국어 친화적 게임 개발용 스크립팅 언어입니다.
C++로 만들어진 인터프리터 엔진 위에서 실행됩니다.

---

## 실행 방법

```bash
# 빌드
make
# 또는
g++ main2.cpp -o SuraEngine2.exe -std=c++17

# 스크립트 실행
./SuraEngine2.exe 파일명.sura

# 실시간 모드 (REPL)
./SuraEngine2.exe

# 종료
exit
```

---

## 기본 문법

### 변수 대입
```
x is 10
이름 is "수라"
켜짐 is true
없음 is nil
```

### 산술 연산
```
결과 is x + 5
결과 is (x + 3) * 2    // 괄호 지원
x + 1                  // 인플레이스 (x = x + 1)
x - 1
x * 2
x / 2
x % 3                  // 나머지
```

### 출력
```
print "안녕하세요"
print x
print "점수: {score}"         // 문자열 보간
print "안녕 {이름}님!"
print "결과: {x + y}"         // 표현식도 가능
print "중괄호: {{리터럴}}"    // {{ }} 로 리터럴 중괄호
print                          // 빈 줄 출력
```

### 입력
```
input 변수명
input "이름을 입력하세요: " 변수명
```

### 증감 단축
```
inc x    // x = x + 1
dec x    // x = x - 1
```

### 주석
```
# 이것은 주석입니다
// 이것도 주석입니다
```

---

## 타입 시스템

수라의 값은 4가지 타입을 가집니다.

| 타입 | 예시 | 설명 |
|------|------|------|
| number | 42, 3.14 | 숫자 |
| string | "안녕" | 문자열 |
| bool | true, false | 불리언 |
| nil | nil | 없음 |

### 타입 확인
```
x is 42
type x 결과       // 결과 = "number"

y is "안녕"
type y 결과       // 결과 = "string"
```

### 암시적 변환 규칙
```
"10" + 5      → 15         // 숫자 문자열 자동 변환
"안녕" + "!"  → "안녕!"   // 문자열 연결
true + 1      → 2          // bool → number
"abc" - 1     → 타입 오류  // 변환 불가
```

---

## 조건문

### 인라인 (한 줄)
```
if x > 5 then print "크다"
if x == 0 then running is 0
```

### 블록
```
if x > 5 then
    print "크다"
end

if x > 5 then
    print "크다"
else
    print "작다"
end
```

### 비교 연산자
```
==   같다
!=   다르다
>    크다
<    작다
>=   크거나 같다
<=   작거나 같다
```

### 논리 연산자
```
if x > 0 and y > 0 then print "둘 다 양수"
if x == 0 or y == 0 then print "하나는 0"
if not 켜짐 then print "꺼짐"
```

---

## 반복문

### while
```
while x > 0 do
    print x
    x - 1
end
```

### repeat (N번 반복)
```
repeat 5 do
    print "안녕"
end
```

### for (범위 반복)
```
// 기본
for i in 1 to 10 do
    print i
end

// ~ 문법도 동일
for i in 0 ~ 9 do
    print i
end

// step (증감값 지정)
for i in 0 to 100 step 10 do
    print i
end

// 역순
for i in 10 to 1 step -1 do
    print i
end
```

### 반복 제어
```
// break: 반복 탈출
while 1 do
    input "입력: " 값
    if 값 == "q" then break
end

// continue: 다음 순환으로 건너뜀
for i in 1 to 10 do
    if i % 2 == 0 then continue
    print i    // 홀수만 출력
end
```

---

## 함수

### 정의 및 호출
```
// 매개변수 없음
func 인사 do
    print "안녕하세요!"
end
인사

// 매개변수 있음
func 더하기(a, b) do
    return a + b
end

// 표현식으로 호출 (반환값 사용)
결과 is 더하기(3, 5)
print 결과    // 8

// 문장으로 호출
더하기(10, 20)
```

### return
```
func 최댓값(a, b) do
    if a > b then
        return a
    end
    return b
end

결과 is 최댓값(10, 25)
print 결과    // 25
```

### 재귀
```
func 팩토리얼(n) do
    if n <= 1 then return 1
    return n * 팩토리얼(n - 1)
end

print 팩토리얼(5)    // 120
```

---

## 에러 처리

### try / catch
```
try
    print 없는변수
catch 오류
    print "오류 발생: {오류}"
end
```

### throw
```
func 나누기(a, b) do
    if b == 0 then
        throw "0으로 나눌 수 없습니다"
    end
    return a / b
end

try
    결과 is 나누기(10, 0)
catch 오류
    print "잡힘: {오류}"
end
```

### 스택 트레이스
오류 발생 시 호출 경로를 자동으로 출력합니다:
```
[수라 런타임 오류] 정의되지 않은 변수: 'x'
  호출 경로:
    [2] 함수B (5줄)
    [1] 함수A (10줄)
```

---

## 배열

### 배열 리터럴
```
과일 is ["사과", "바나나", "딸기"]
숫자 is [1, 2, 3, 4, 5]
```

### 배열 조작
```
arr_push 목록 "포도"        // 추가
arr_pop  목록               // 마지막 제거
arr_len  목록 길이          // 길이 → 길이 변수
arr_get  목록 0 값          // 인덱스 읽기 → 값 변수
arr_set  목록 0 "딸기"      // 인덱스 수정
arr_clear 목록              // 전체 비우기
```

### 배열 순회
```
점수 is [90, 75, 88, 95, 60]
arr_len 점수 n
합계 is 0
for i in 0 to n - 1 do
    arr_get 점수 i 값
    합계 + 값
end
print "평균: {합계 / n}"
```

---

## 딕셔너리 (객체)

```
플레이어.이름 is "용사"
플레이어.체력 is 100
플레이어.공격력 is 25

print 플레이어.이름
플레이어.체력 - 10
```

---

## 클래스

### 정의
```
class 몬스터 do
    이름 is "몬스터"
    체력 is 100
    공격력 is 10

    func init(이름, 체력) do
        self.이름 is 이름
        self.체력 is 체력
    end

    func 공격 do
        print "{self.이름}이 {self.공격력} 데미지!"
    end

    func 피해(양) do
        self.체력 - 양
        if self.체력 <= 0 then
            print "{self.이름} 쓰러짐!"
        end
    end

    func 상태 do
        print "{self.이름} HP: {self.체력}"
    end
end
```

### 인스턴스 생성 및 사용
```
슬라임 is new 몬스터("슬라임", 50)
드래곤 is new 몬스터("드래곤", 300)

슬라임.공격()
드래곤.피해(80)
드래곤.상태()

// 필드 직접 접근
print 슬라임.체력
슬라임.이름 is "킹 슬라임"
```

### 상속
```
class 보스 extends 몬스터 do
    패턴 is 1

    // 메서드 오버라이드
    func 피해(양) do
        self.체력 - 양 / 2    // 절반만 받음
        print "{self.이름}이 저항!"
    end

    func 분노 do
        self.패턴 is 2
        print "{self.이름} 분노!"
    end
end

보스몹 is new 보스("드래곤", 500)
보스몹.피해(100)   // 오버라이드된 메서드
보스몹.상태()      // 상속된 메서드
보스몹.분노()
```

---

## 문자열

```
str_len  "안녕" 결과              // 길이 → 결과
str_sub  "안녕하세요" 0 2 결과   // 부분 문자열
str_upper "hello" 결과            // 대문자
str_lower "HELLO" 결과            // 소문자
str_find  "안녕하세요" "하세" 결과 // 위치 (-1이면 없음)
```

---

## 수학

```
abs   -5 결과          // 절댓값  → 5
sqrt  16 결과          // 제곱근  → 4
pow   2 10 결과        // 거듭제곱 → 1024
floor 3.7 결과         // 내림    → 3
ceil  3.2 결과         // 올림    → 4
min   3 7 결과         // 최솟값  → 3
max   3 7 결과         // 최댓값  → 7
```

---

## 랜덤

```
random 1 ~ 100 결과    // 1~100 사이 랜덤 숫자
```

---

## 시간

```
sleep 1000             // 1000ms (1초) 대기
now 현재시각           // 유닉스 타임스탬프
```

---

## 화면 / 그리드

```
cls                    // 화면 지우기
silent on              // 출력 억제 (게임 루프용)
silent off             // 출력 다시 켜기

grid_init 30 16        // 30x16 그리드 생성
grid_clear             // 그리드 초기화
grid_set 5 3 "O"       // (5, 3) 위치에 "O"
grid_get 5 3 값        // (5, 3) 위치 값 읽기
grid_draw              // 그리드 출력
```

---

## 키 입력 (게임용)

```
readkey 키             // 키 입력 → 소문자로 저장
```

---

## 파일 입출력

```
file_save "저장.txt" 내용    // 파일에 저장
file_load "저장.txt" 내용    // 파일에서 불러오기
```

---

## 표준 라이브러리

```
use math      // clamp, sign
use game      // collide, in_bounds, manhattan, lerp, dice
use string    // repeat_str, starts_with
use system    // save_data, load_data
use time      // time_now, time_wait, fps_wait
```

---

## 오류 메시지

| 오류 종류 | 예시 |
|-----------|------|
| 렉서 오류 | `알 수 없는 문자: '@'` |
| 파서 오류 | `'then' 필요 — 대신 'do' 있음` |
| 타입 오류 | `'-' 연산에는 숫자 필요` |
| 런타임 오류 | `정의되지 않은 변수: 'x'` |

---

## 엔진 내부 구조

```
소스코드 (.sura)
    ↓  lexer.hpp       문자열 → 토큰
    ↓  parser.hpp      토큰 → AST 트리
    ↓  ast.hpp         트리 노드 구조
    ↓  value.hpp       타입 시스템 (number/string/bool/nil)
    ↓  interpreter.hpp AST 실행
    ↓  platform.hpp    크로스플랫폼 (Windows/macOS/Linux)
    ↓  main2.cpp       진입점 (REPL + 파일실행 + LSP)
```

---

## 전체 예제: RPG 전투

```
class 캐릭터 do
    이름 is "캐릭터"
    체력 is 100
    공격력 is 20

    func init(이름, 체력, 공격력) do
        self.이름 is 이름
        self.체력 is 체력
        self.공격력 is 공격력
    end

    func 공격(대상) do
        random 1 ~ self.공격력 피해량
        print "{self.이름} → {대상.이름} {피해량} 데미지"
        대상.체력 - 피해량
    end

    func 살아있음 do
        return self.체력 > 0
    end
end

용사 is new 캐릭터("용사", 100, 25)
슬라임 is new 캐릭터("슬라임", 40, 10)

print "=== 전투 시작 ==="

while 용사.살아있음() and 슬라임.살아있음() do
    용사.공격(슬라임)
    if slime.살아있음() then
        슬라임.공격(용사)
    end
end

if 용사.살아있음() then
    print "용사 승리! 남은 체력: {용사.체력}"
else
    print "슬라임 승리!"
end
```
