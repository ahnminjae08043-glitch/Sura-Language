# Sura 호환성과 지원 정책

이 정책은 Sura 1.11.1부터 적용한다. 그 이전 버전의 모든 동작을 소급해서
보장한다는 뜻은 아니다. 자동 검사는 `compatibility.json`과
`tests/compat/1.11`을 기준으로 한다. 별도로 `tests/compat/1.10`은 보관된
1.10.0 Windows x64 엔진과 현재 엔진이 함께 실행하는 역사 검증 자료다. 이
자료는 실제 교차 버전 동작을 기록하지만 1.10 계열에 새 지원 보장을 소급해서
만들지는 않는다.

## 버전 규칙

Sura는 `major.minor.patch` 형식을 사용한다.

- 패치 버전은 공개 문법, 기존 표준 라이브러리 호출, 패키지 매니페스트와
  C ABI를 의도적으로 깨지 않는다. 버그 수정 때문에 잘못된 동작이 바뀌는
  경우에는 변경 기록과 회귀 테스트를 함께 추가한다.
- 마이너 버전에서 기존 기능을 없애거나 의미를 바꾸려면 먼저 지원 중단을
  알리고 최소 한 번의 마이너 버전 동안 기존 동작을 유지한다.
- 메이저 버전은 호환되지 않는 변경을 허용하지만 마이그레이션 문서와
  자동 검사 가능한 이전·이후 예제가 필요하다.
- 지원 중단 예정 기능은 경고, 대체 API, 제거 예정 버전을 문서와 변경
  기록에 함께 적는다. 예고 없이 삭제하지 않는다.

## 지원 기간

지원 정책은 2026-07-16부터 적용한다. 1.11 계열은 1.11.1을 첫 보장 패치로
삼고 최소 2027-07-16까지 유지한다. 다음 안정 마이너 버전이 출시되면 1.11은
그 출시일로부터 최소 6개월 동안 함께 지원한다. 따라서 실제 지원 종료일은
2027-07-16과 다음 안정 마이너 출시 6개월 뒤 중 더 늦은 날짜보다 앞설 수 없다.

여기서 유지는 안정 등급의 호환성, 정확성, 보안 문제를 고칠 수 있는 릴리스를
뜻한다. 새 기능의 이전 버전 역이식이나 정해진 답변 시간까지 보장한다는 뜻은
아니다. 실제 지원 상태와 날짜는 `compatibility.json`의 `support`가 기준이다.

지원 중단 항목은 같은 파일의 `deprecations`에 식별자, 최초 예고 버전, 가장
이른 제거 버전, 대체 기능과 경고 근거를 기록해야 한다. 현재 등록된 지원 중단
항목은 없다.

## 현재 호환 계약

| 영역 | 현재 계약 |
| --- | --- |
| 소스 | 1.11.1에서 만든 `tests/compat/1.11` 프로그램을 이후 1.x 릴리스의 VM과 JIT fallback에서 계속 검사 |
| 역사 검증 | 1.10.0 공개 ZIP과 내부 엔진의 크기·SHA-256을 공개 manifest와 대조하고 `tests/compat/1.10` 세 파일을 1.10.0·현재 VM/JIT fallback 양쪽에서 실행 |
| 안정 표준 라이브러리 API | `array`, `dict`, `fs`, `json`, `string`, `test`의 112개 이름·서명을 1.11 기준으로 검사 |
| 바이트코드 | 형식 3 생성, 형식 2와 3 읽기. 1.10.0 엔진이 만든 core fixture 바이트코드를 현재 엔진에서 직접 실행 |
| 보호 릴리스 패키지 | 형식 5 생성, 형식 1~5 읽기 |
| Plugin ABI | 1.1.0 |
| Embedding FFI ABI | 1.2.0 |

바이트코드와 보호 릴리스 패키지는 장기 보관용 소스 형식을 대신하지 않는다.
지원 목록에서 빠지는 형식은 먼저 마이너 버전 지원 중단 절차를 거쳐야 한다.

## 안정 API 스냅샷

`tests/compat/1.11/stable-api.json`은 안정 등급 표준 라이브러리 6개 모듈의
공개 이름, 종류와 호출 서명을 저장한다. `surapkg docs`가 생성한 현재 API와
비교해 기존 항목의 삭제나 서명 변경을 차단한다. 기존 서명을 그대로 둔 새 API
추가는 허용한다. 이 검사는 함수 내부 동작 전체를 증명하지 않으므로 동작
호환성은 `tests/compat/1.11` fixture가 별도로 검사한다.

`tests/compat/1.10`은 안정 API 스냅샷이 아니다. 보관된 1.10.0 엔진이 실제로
받아들이는 핵심 문법·객체·표준 라이브러리 교집합을 현재 엔진도 계속 실행할
수 있는지 검사하는 역사 probe다. Windows x64에서는 보관 ZIP과 엔진 해시를
확인한 다음 이전 VM·JIT fallback 실행과 이전 바이트코드의 현재 엔진 로드를
검사한다. 다른 플랫폼의 보고서는 보관 Windows 실행을 `not_applicable`로
기록하고 현재 런타임 회귀만 검사한다.

## 기능 지원 등급

기능 수가 많다는 이유만으로 모든 기능을 같은 완성도로 표시하지 않는다.

### 안정

- 네이티브 Sura 문법과 register VM
- 핵심 값, 제어문, 함수, 클래스와 예외 처리
- 파일, 문자열, 배열, 딕셔너리, JSON과 테스트 API
- `surapkg`의 프로젝트 생성, 실행, 테스트와 잠금 파일
- C FFI와 versioned plugin ABI

안정 등급은 패치 버전 호환 규칙과 교차 플랫폼 CI의 적용을 받는다.

### 플랫폼 한정

- 네이티브 JIT: Win64 x86-64는 기존 부분 컴파일러를 사용하고, Linux
  x86-64는 상수·이동·정적으로 숫자임이 증명된 `+`, `-`, `*`, 단항
  `-`, 비교 6종, 0이 아닌 제수로 증명된 `/`와 반환을 처리하는
  helper/예외 없는 System V baseline을 사용한다. Linux x86-64 baseline은
  여기에 가드된 전역 읽기(함수 바인딩은 아이덴티티, 숫자는 태그 검사)와
  순수 숫자 함수 사이의 네이티브 직접 호출을 더하며, 가드 실패나 재귀
  깊이 예산 소진은 register VM으로 되돌아가 같은 오류를 낸다. little-endian
  Windows·Linux·macOS ARM64는 전역·호출을 제외한 같은 범위의 AAPCS64
  baseline을 사용한다. 0이거나 정적으로 확인할 수 없는 제수, 동적 비교를
  포함한 그 밖의 bytecode와 macOS x86-64는 register VM에서 실행한다.
- CUDA: 호환 NVIDIA 드라이버와 구현된 resident 연산만 지원
- 미디어: FFmpeg가 설치되었거나 명시적으로 지정된 환경에서 지원
- Windows 설치와 Microsoft Store 패키지: Windows x64 배포 경로

플랫폼 한정 기능은 지원하지 않는 환경에서 VM 또는 명시적인 오류로
전환해야 하며, 조용히 다른 성능을 약속하지 않는다.

### 실험

- JavaScript와 WebAssembly 변환 타깃
- Transformer 확장 기능과 분산 학습
- bounded byte-level BPE tokenizer와 version 2 tokenizer 파일
- CPU ONNX IR 3~10·opset 7~18의 bounded graph 실행 subset
- 보호 릴리스의 고급 정책과 외부 레지스트리 운영 기능

실험 기능은 문법과 데이터 형식이 마이너 버전에서 바뀔 수 있다. 안정 등급으로
올리려면 지원 플랫폼, 실패 동작, 호환 fixture와 CI 검사를 먼저 추가한다.

## 자동 검증

다음 명령은 버전, 지원 기간, 안정 API, 문법 fixture, VM/JIT 결과,
바이트코드 형식과 Plugin/FFI ABI가 이 문서와 맞는지 검사한다. Windows x64에서는
1.10.0 공개 ZIP·엔진 해시, 이전 런타임 실행과 이전 바이트코드 forward load도
같은 명령에 포함된다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_compatibility_gate.ps1
```

스냅샷의 정렬과 현재 생성 결과가 같은지, 게이트가 잘못된 지원 기간과 API
변경을 실제로 거부하는지는 다음 명령으로 검사한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_compatibility_api_snapshot.ps1 -Check
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_compatibility_gate_smoke.ps1
```

검사 결과는 `artifacts/compatibility_report.json`에 기록한다. 새 기능이 기존
계약을 깨면 릴리스하기 전에 fixture를 유지하거나 정책에 따른 지원 중단
절차를 밟아야 한다.
