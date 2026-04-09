# Sura 패키지 관리 시스템 (SURAPKG)

## 개요

**SURAPKG**는 Sura 언어를 위한 패키지 관리자입니다. 다른 프로그래밍 언어의 npm, pip, cargo와 유사한 역할을 합니다.

---

## 설치

### 1단계: surapkg 바이너리 설치
```bash
# Windows
> surapkg --version

# Linux/macOS
$ surapkg --version
```

### 2단계: 프로젝트 초기화
```bash
$ surapkg init my-project
$ cd my-project
```

생성되는 파일:
- `sura.pkg.yaml` - 패키지 메타데이터
- `surapkg.lock.yaml` - 의존성 잠금 파일 (버전 관리)
- `.surapkg/` - 로컬 패키지 캐시

---

## sura.pkg.yaml 구조

```yaml
name: my-awesome-app
version: 1.0.0
author: Your Name
license: MIT
description: 내 첫 Sura 애플리케이션

dependencies:
  http-client: ^2.0.0      # 메이저 버전 호환
  data-utils: ~1.5.0       # 마이너 버전 호환
  game-physics: 1.0.0      # 정확한 버전

dev-dependencies:
  test-framework: ^1.0.0

scripts:
  start: main.sura
  build: scripts/build.sura
  test: scripts/test.sura
```

---

## 패키지 명령어

### 의존성 설치
```bash
$ surapkg install           # sura.pkg.yaml 기반 설치
$ surapkg install http-client   # 특정 패키지 설치
$ surapkg install http-client@2.1.0  # 버전 지정
```

### 패키지 제거
```bash
$ surapkg remove http-client
$ surapkg uninstall http-client
```

### 패키지 검색
```bash
$ surapkg search http
$ surapkg search "data processing"
```

### 패키지 정보 보기
```bash
$ surapkg info http-client
```

### 버전 업그레이드
```bash
$ surapkg update                    # 모든 패키지 업데이트
$ surapkg update http-client        # 특정 패키지 업데이트
$ surapkg update http-client@3.0.0  # 특정 버전으로
```

### 전역 설치 (모든 프로젝트에서 사용)
```bash
$ surapkg install -g math-advanced
```

---

## 패키지 저장소 (Repository)

### 공식 레지스트리
```
https://registry.sura-lang.org
```

패키지는 JSON 형식으로 관리됩니다:

```json
{
  "name": "http-client",
  "version": "2.1.0",
  "author": "Developer",
  "description": "HTTP 클라이언트 라이브러리",
  "repository": "https://github.com/user/http-client",
  "main": "lib/http.sura",
  "dependencies": {
    "url-parser": "^1.0.0"
  },
  "tags": ["web", "http", "networking"]
}
```

### 프라이빗 레지스트리
```bash
$ surapkg config registry https://private.company.com/registry
```

---

## 패키지 생성 및 배포

### 1단계: 패키지 디렉토리 구조
```
my-http-lib/
├── sura.pkg.yaml
├── README.md
├── lib/
│   └── http.sura
├── examples/
│   └── usage.sura
└── tests/
    └── test_http.sura
```

### 2단계: sura.pkg.yaml 작성
```yaml
name: http-client
version: 2.1.0
author: Your Name <your@email.com>
license: MIT
repository: https://github.com/user/http-client
main: lib/http.sura
description: 간단한 HTTP 클라이언트 라이브러리

dependencies:
  url-parser: ^1.0.0

exports:
  - http.sura
  - utils/request.sura

keywords:
  - http
  - web
  - networking
```

### 3단계: 계정 등록 및 배포
```bash
$ surapkg login
Username: your_username
Password: ****
Email: your@email.com

$ surapkg publish
Publishing http-client@2.1.0...
✓ Package published successfully!
```

---

## 패키지 사용 방법

### 로컬 패키지 사용

```sura
# main.sura
use http-client      # 자동으로 .surapkg/http-client 에서 로드

func main do
    http_request "GET" "https://api.example.com/data" result
    print result
end

main
```

### 특정 함수만 임포트
```sura
use http-client { send_request, parse_response }

func main do
    send_request "https://example.com" response
end
```

---

## 버전 관리 규칙

Sura는 **Semantic Versioning** (SemVer) 를 따릅니다:

```
MAJOR.MINOR.PATCH
1.2.3
│   │   │
│   │   └─ 버그 수정
│   └───── 새로운 기능 (역호환 유지)
└───────── 호환 불가 변경
```

### 버전 범위 지정

```
1.0.0      # 정확한 버전
^1.2.3     # 호환 가능한 메이저 버전 (1.x.x)
~1.2.3     # 호환 가능한 마이너 버전 (1.2.x)
>=1.0.0    # 1.0.0 이상
<2.0.0     # 2.0.0 미만
1.0.0 - 2.0.0  # 범위
```

---

## 로컬 디렉토리 패키지

다른 로컬 디렉토리의 패키지를 사용할 수 있습니다:

```yaml
dependencies:
  my-local-lib: file:../my-lib
  dev-toolkit: file:~/projects/dev-tools
```

---

## 스크립트 실행

패키지의 커스텀 스크립트를 정의할 수 있습니다:

```bash
$ surapkg run start          # main.sura 실행
$ surapkg run build          # scripts/build.sura 실행
$ surapkg run test           # scripts/test.sura 실행
```

또는:
```bash
$ surapkg start
$ surapkg build
$ surapkg test
```

---

## 의존성 그래프 분석

```bash
$ surapkg ls              # 설치된 패키지 목록
$ surapkg tree            # 의존성 트리 보기
$ surapkg outdated        # 업데이트 가능한 패키지
$ surapkg audit           # 보안 취약점 확인
```

---

## 캐시 관리

```bash
$ surapkg cache ls         # 캐시 목록
$ surapkg cache clean      # 캐시 정리
$ surapkg cache verify     # 캐시 검증
```

---

## 환경 설정

```bash
$ surapkg config set registry https://registry.sura-lang.org
$ surapkg config set prefix ~/.surapkg
$ surapkg config get registry
$ surapkg config list
```

---

## 예제: 웹 크롤러 프로젝트

### 1. 프로젝트 초기화
```bash
$ surapkg init web-crawler
$ cd web-crawler
```

### 2. 의존성 설치
```bash
$ surapkg install http-client
$ surapkg install data-utils
$ surapkg install json-parser
```

### 3. main.sura
```sura
use http-client
use data-utils
use json-parser

func fetch_data(url) do
    http_request "GET" url response
    json_parse response data
    return data
end

func main do
    url is "https://api.example.com/articles"
    articles is fetch_data(url)
    
    arr_len articles count
    print "총 {count}개 기사 발견"
    
    for i in 0 to count - 1 do
        arr_get articles i article
        print article
    end
end

main
```

### 4. 실행
```bash
$ surapkg start
```

---

## 주요 공식 패키지

| 패키지 | 설명 | 버전 |
|--------|------|------|
| `http-client` | HTTP 요청 | ^2.0.0 |
| `data-utils` | 데이터 처리 | ^1.5.0 |
| `json-parser` | JSON 파싱 | ^1.0.0 |
| `csv-handler` | CSV 처리 | ^1.2.0 |
| `game-physics` | 2D 물리 엔진 | ^3.0.0 |
| `graphics-lib` | 그래픽 렌더링 | ^2.1.0 |
| `test-framework` | 단위 테스트 | ^1.0.0 |
| `logging` | 로깅 시스템 | ^1.3.0 |

---

## 문제 해결

### 패키지 설치 실패
```bash
$ surapkg install --verbose http-client
$ surapkg cache clean
$ surapkg install http-client
```

### 버전 충돌
```bash
$ surapkg ls
# 충돌하는 패키지 확인 후 버전 조정
```

### 로컬 개발 모드
```bash
$ surapkg link ../my-lib
# sura.pkg.yaml에 자동 추가됨
```

---

## 더 알아보기

- [공식 문서](https://docs.sura-lang.org)
- [패키지 레지스트리](https://registry.sura-lang.org)
- [커뮤니티 포럼](https://forum.sura-lang.org)
