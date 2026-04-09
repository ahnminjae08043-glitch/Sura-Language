# Sura Language Support for VS Code

Sura 프로그래밍 언어를 위한 완벽한 VS Code 통합 지원입니다.

## ✨ 기능

### 🎯 기본 기능
- ✅ **문법 강조** - 모든 Sura 키워드 및 함수 인식
- ✅ **자동완성** - 키워드, 함수, 라이브러리 자동 완성
- ✅ **스니펫** - if, while, for, func, class 등 코드 템플릿
- ✅ **정의 이동** - F12로 함수/클래스 정의로 이동
- ✅ **호버 정보** - 함수 위에 마우스 올리면 설명 표시
- ✅ **자동 포맷팅** - 들여쓰기 자동 정렬
- ✅ **괄호 매칭** - 자동 괄호 완성 및 하이라이트
- ✅ **린팅** - 문법 오류 실시간 감지

### 🎮 실행 기능
- ✅ `Ctrl+Shift+R` - 현재 파일 실행
- ✅ `Ctrl+Shift+L` - 코드 포맷팅
- ✅ Sura REPL 열기

### 📚 자동완성 항목

#### 키워드 (14개)
```
is if then else end while do repeat for in to step
break continue return func class extends try catch throw use new self
print input type true false nil
```

#### 내장 함수 (30+개)
```
배열: arr_len, arr_get, arr_set, arr_push, arr_pop, arr_clear
문자열: str_len, str_sub, str_upper, str_lower, str_find
수학: abs, sqrt, pow, floor, ceil, min, max
파일: file_save, file_load, file_exists
게임: win_init, win_clear, win_draw, win_rect, key_down
시간: sleep, now, random
v3.0+: http_get, http_post, json_parse, json_stringify, csv_load, env_get, system_exec
```

#### 라이브러리
```
math, game, string, system, time
web (HTTP/JSON), data (CSV/통계), os (파일/환경)
```

## 🚀 설치

### VS Code 마켓플레이스에서
```
Sura Language Support 검색 → 설치
```

### 수동 설치
```bash
git clone https://github.com/sura-lang/vscode-extension
cd vscode-extension
npm install
npm run esbuild
```

## 📖 사용법

### 자동완성
파일을 편집하면 자동으로 제안됩니다:
```sura
pr   → print (제안)
arr_ → arr_get, arr_push, ... (제안)
use  → math, game, web (라이브러리 제안)
```

### 정의 이동
함수명에서 `F12` 또는 우클릭 → "정의로 이동":
```sura
calculate  ← F12 누르면 함수 정의로 이동
```

### 호버 정보
함수나 키워드에 마우스를 올리면 설명 표시:
```
print ← 마우스 올림 → "값을 출력합니다"
```

### 스니펫 (코드 템플릿)
입력 후 Tab키:
```
if     → if 조건 then ... end
while  → while 조건 do ... end
for    → for i in 1 to 10 do ... end
func   → func 이름(인자) do ... end
class  → class 이름 do ... end
try    → try ... catch 오류 ... end
```

### 포맷팅
- 자동: 파일 저장 시 자동 포맷팅 (설정에서 enableAutoFormat)
- 수동: `Ctrl+Shift+L` 또는 `Ctrl+A` → `Ctrl+K Ctrl+F`

### 린팅 (오류 감지)
실시간으로 문법 오류 감지:
- 괄호 미매칭
- 주석 위치 경고
- 타입 불일치 (계획)

## ⚙️ 설정

VS Code 설정 (`settings.json`)에서:

```json
{
  "sura.enginePath": "SuraEngine2.exe",        // Sura 엔진 경로
  "sura.linting.enabled": true,                // 린팅 활성화
  "sura.autoFormat": true,                     // 저장 시 자동 포맷팅
  "sura.hoverDocumentation": true              // 호버 정보 표시
}
```

## 🎮 예제

### 간단한 게임
```sura
# Sura에서 자동완성 + 린팅 + 정의 이동 지원
use game

func main do
    win_init 800 600 "내 게임"
    
    x is 400
    y is 300
    
    while 1 do
        win_clear 0 0 0
        key_down "a" left
        key_down "d" right
        
        if left then x - 5
        if right then x + 5
        
        win_rect x y 50 50 255 0 0
        win_draw
    end
end

main
```

### 웹 API 호출
```sura
use web

func fetch_github_user do
    url is "https://api.github.com/users/octocat"
    http_get url response
    json_parse response user
    print user
end

fetch_github_user
```

### 데이터 분석
```sura
use data

func analyze do
    csv_load "data.csv" data
    mean data average
    print "평균: {average}"
end

analyze
```

## 🐛 문제 해결

### 자동완성이 안 나타남
- 재시작: `Ctrl+Shift+P` → "개발자: 창 다시 로드"
- 확인: `Ctrl+,` → "Sura" 검색

### 색상이 이상함
- 테마 확인: 다른 테마 시도
- 문법 강조 설정: `settings.json`에서 `editor.tokenColorCustomizations` 설정

### 엔진을 찾을 수 없음
- 경로 확인: `settings.json`에서 `sura.enginePath` 설정
- 예: `"sura.enginePath": "C:\\path\\to\\SuraEngine2.exe"`

## 📝 버전 히스토리

### v1.2.0 (2024년)
- ✅ 완전한 자동완성 (50+ 함수)
- ✅ 스니펫 (6개)
- ✅ 정의 이동
- ✅ 호버 정보
- ✅ 자동 포맷팅
- ✅ 기본 린팅
- ✅ 웹/데이터/시스템 함수 지원

### v1.5.0 (계획)
- 디버거 통합
- 개선된 린팅 (타입 검사)
- REPL 통합 터미널

### v2.0.0 (계획)
- Language Server Protocol (LSP)
- 테스트 러너

## 🤝 기여

버그 리포트 및 기능 제안은 GitHub Issues에서:
https://github.com/sura-lang/sura/issues

## 📄 라이센스

MIT License - 자유롭게 사용, 수정, 배포 가능

## 🔗 더 알아보기

- [Sura 공식 사이트](https://sura-lang.org)
- [GitHub 저장소](https://github.com/sura-lang/sura)
- [공식 문서](https://docs.sura-lang.org)
- [Discord 커뮤니티](https://discord.gg/sura)

---

**Sura를 사랑해주셔서 감사합니다!** ❤️
