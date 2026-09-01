# 빌드 출처 증명 (Provenance Manifest)

이 문서는 REMAINING_ISSUES.md의 클린 릴리스 기준 4번
("모든 산출물의 커밋·컴파일러·플래그·바이트 수·SHA-256 기록")을 이행한다.
릴리스마다 이 표를 갱신하고, 검증자는 아래 재현 절차로 같은 해시를
직접 계산할 수 있어야 한다.

## 현재 검증 상태 (pre-1.12, 2026-08-31)

| 항목 | 값 |
| --- | --- |
| 소스 커밋 | `cb4f079` (`agent/sura-os-freestanding`) |
| 컴파일러 | g++ (Rev13, Built by MSYS2 project) 15.2.0 |
| 엔진 플래그 | `-std=c++17 -O3 -DNDEBUG -Wall -static -Wl,--no-insert-timestamp -lgdi32` |
| surapkg 플래그 | `-std=c++17 -O2 -Wall -static -Wl,--no-insert-timestamp` |
| 외부 의존성 | 없음 (정적 링크, 소스 트리 외 라이브러리 불사용) |

| 산출물 | 바이트 | SHA-256 |
| --- | ---: | --- |
| `SuraLanguage.exe` | 8,925,149 | `3cb621f13dea8ae5abbf70077bba184ee24b57db96ac87c88f9c79caf1e83e5e` |
| `surapkg.exe` | 5,417,571 | `1919d82579d09822b1692b7e171e8a383a047a9ea2f3aaf83e6623d3d1773a70` |

## 재현 절차

```powershell
git clone <repo> sura-verify
cd sura-verify
git checkout cb4f079
.\build.bat portable
Get-FileHash .\SuraLanguage.exe -Algorithm SHA256
Get-FileHash .\surapkg.exe -Algorithm SHA256
```

PE 헤더 타임스탬프는 `-Wl,--no-insert-timestamp`로 고정되므로 동일
소스·툴체인에서 반복 빌드는 바이트 단위로 일치한다.

## 이 커밋에서 함께 검증된 것

- 클린 클론 빌드의 엔진 해시가 `bench_summary.md`(2026-08-15)에 기록된
  해시와 일치 — 기록된 벤치마크의 엔진 출처가 제3자 재현 가능
- `run_stable_tests.ps1` (JIT 레인): 151 passed / 0 skipped / 0 failed
- 온보딩 경로: 클린 클론의 `surapkg new` → `run` → `test` 전부 성공

## 기록 규칙

- 산출물 해시는 항상 클린 체크아웃 빌드에서 계산한다. 작업 트리 빌드의
  해시를 릴리스 증거로 쓰지 않는다.
- 툴체인을 바꾸면(컴파일러 버전 포함) 해시가 달라지므로 위 표의
  컴파일러 항목도 반드시 함께 갱신한다.
- 과거 릴리스의 표는 지우지 않고 아래에 절 단위로 보존한다.
