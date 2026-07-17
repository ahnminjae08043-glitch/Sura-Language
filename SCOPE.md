# Sura 기능 범위

Sura의 기본 목표는 자체 문법을 register bytecode와 register VM으로 실행하고,
프로젝트 생성·테스트·패키징까지 한 도구 체계로 제공하는 것이다. 기능 수 자체를
완성도의 근거로 사용하지 않는다. 현재 지원 등급의 단일 기준은
`compatibility.json`이며 사람이 읽는 규칙은 `COMPATIBILITY.md`에 있다.

## 핵심 범위

- lexer, parser, strict-by-default type checker, register bytecode와 register VM
- 값, 제어문, 함수, class·struct·enum, 예외와 module import
- 파일·문자열·배열·dictionary·JSON·test API
- `surapkg` 프로젝트 생성, 실행, 테스트, lockfile과 package 검증
- C embedding FFI와 versioned plugin ABI
- 실제 문법과 동작에서 생성되는 레퍼런스, LSP와 VS Code 확장

핵심 범위의 공개 동작은 패치 버전 호환 규칙, 교차 플랫폼 빌드와 회귀 테스트의
적용을 받는다.

## 조건부 범위

다음 기능은 핵심 문법보다 좁은 플랫폼 또는 외부 프로그램 조건을 가진다.

- Win64 x86-64 partial JIT, Linux x86-64 System V baseline과 little-endian
  Windows/Linux/macOS ARM64 AAPCS64 baseline
- CUDA resident 연산
- FFmpeg 기반 media 기능
- Windows x64 설치기와 Microsoft Store package
- Python, Node.js, curl과 C/C++ toolchain을 호출하는 bridge·도구 기능

조건이 맞지 않을 때는 문서화된 register VM fallback이나 명시적인 오류가 있어야
한다. 조건부 기능의 결과를 모든 플랫폼의 결과로 표현하지 않는다.

## 실험 범위

- JavaScript와 WebAssembly 변환 타깃
- Transformer 확장, 분산 학습과 아직 구현하지 않은 AI 상호운용 기능
- 보호 릴리스의 고급 정책과 외부 registry 운영 기능

실험 기능은 마이너 버전에서 바뀔 수 있다. 안정 API와 같은 호환성을 약속하지
않으며, 공개 레퍼런스에는 현재 제한과 fallback을 함께 적는다.

## 새 기능의 기본 규칙

새 public syntax, builtin, module, CLI option이나 file format은 처음에는 실험으로
분류한다. 다음 조건을 모두 만족해야 안정 또는 플랫폼 한정 등급으로 올릴 수 있다.

1. 지원 플랫폼과 외부 의존성이 정해져 있다.
2. 성공 동작과 실패·fallback 동작을 자동 테스트한다.
3. 실제 Sura 문법을 사용한 예제와 public reference가 있다.
4. 기존 source·bytecode·ABI 계약에 미치는 영향을 기록한다.
5. Windows benchmark workflow와 cross-platform workflow 중 적용 가능한 경로에
   검사가 연결되어 있다.

기존 핵심 기능의 오류, 호환성 회귀, 안전성 문제와 문서 불일치는 새로운 public
surface보다 먼저 처리한다. 구현되지 않은 기능은 roadmap 항목으로만 기록하고
현재 지원 기능 목록에는 넣지 않는다.

## 범위 변경

기능 제안은 `.github/ISSUE_TEMPLATE/feature_request.yml`의 지원 등급, 사용 사례,
fallback과 검증 계획을 작성한다. public 동작을 바꾸는 pull request는
`COMPATIBILITY.md`, 관련 fixture, 레퍼런스 생성 데이터와 CI 검사를 함께 갱신한다.
