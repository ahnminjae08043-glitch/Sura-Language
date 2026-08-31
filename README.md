# Sura Language

Sura는 C++로 구현된 독립 프로그래밍 언어입니다. `.sura` 소스를 register bytecode로 변환해 register VM에서 실행하며, Windows x64·Linux x86-64와 little-endian Windows/Linux/macOS ARM64에서는 지원 범위가 서로 다른 자체 JIT를 선택적으로 적용합니다.

## 빠른 시작

```powershell
surapkg new hello_sura
cd hello_sura
surapkg run
surapkg test
```

처음 사용하는 경우 [10분 시작 가이드](Guide/START_HERE.md)를 읽으세요. 문법과 구현 범위는 [공식 가이드](Guide/GUIDE.md), 전체 API는 [HTML 레퍼런스](reference.html)에 정리되어 있습니다. 1.11.1부터의 변경·지원 기준은 [호환성과 지원 정책](COMPATIBILITY.md)을 따릅니다.

새 기능의 범위와 안정 승격 조건은 [기능 범위](SCOPE.md), 빌드·테스트·패키지 기여 절차는 [기여 안내](CONTRIBUTING.md)에 있습니다.

현재 안정화 우선순위와 검증된 기술 부채는 [안정화 백로그](REMAINING_ISSUES.md)에 있습니다. 테스트 결과는 통과·건너뜀·실패를 구분하며, CUDA 등 실제 하드웨어 검증이 필요한 릴리스 레인에서는 `-FailOnSkip`을 사용합니다. Sura 런타임은 운영체제 샌드박스가 아니므로 신뢰하지 않는 코드를 실행하기 전에는 [보안 정책](SECURITY.md)의 신뢰 경계를 확인하세요.

## 저장소 구성

- `main.cpp`와 런타임 헤더: 실행기, register VM, JIT, 표준 라이브러리
- `surapkg.cpp`: 프로젝트, 테스트, 패키지, 배포 도구
- `examples/starter`: 외부 의존성 없는 입문 예제 12개
- `tests`: 언어와 런타임 테스트
- `sura-vscode`: 공식 VS Code 확장
- `sura_presentation`: 공식 웹사이트

## 현재 플랫폼 경계

- 기본 VM: Windows, Linux, macOS 빌드 경로
- 네이티브 JIT: Win64 x86-64 부분 컴파일, Linux x86-64 System V baseline, little-endian Windows/Linux/macOS ARM64 AAPCS64 baseline. 두 baseline은 상수·이동·숫자로 증명된 `+`, `-`, `*`, 단항 `-`, 비교 6종과 0이 아닌 제수로 증명된 `/`를 처리하며, 그 밖의 bytecode는 register VM으로 fallback
- JavaScript·WebAssembly: 별도 변환 타깃이며 네이티브 런타임 전체와 동일하지 않음
- FFmpeg, CUDA, Python, Node.js: 해당 기능을 사용할 때만 필요한 선택 의존성
