# Sura 패키지 관리자

`surapkg`는 Sura 프로젝트 생성, 실행, 테스트, 의존성 잠금과 배포 검사를
담당하는 명령행 도구다. 이 문서는 Sura 1.11.1 실행 파일의 `surapkg --help`와
스모크 테스트에서 확인하는 기능만 설명한다.

## 새 프로젝트

```powershell
surapkg new hello_sura
cd hello_sura
surapkg run
surapkg test
```

`surapkg new hello_sura`가 만드는 기본 구조는 다음과 같다.

```text
hello_sura/
├── .vscode/
├── src/
│   ├── hello_sura.sura
│   └── greeting.sura
├── tests/
│   └── greeting_test.sura
├── .gitignore
├── README.md
└── sura.pkg.json
```

기본 매니페스트는 YAML이 아니라 JSON이다.

```json
{
  "name": "hello_sura",
  "version": "0.1.0",
  "main": "src/hello_sura.sura",
  "description": "Sura starter project",
  "dependencies": {}
}
```

생성된 메인 파일은 상대 경로 모듈을 가져와 바로 실행할 수 있다.

```sura
import "./greeting.sura"

args is argv()
name is "Sura"
if length(args) > 0 then
  name is args[0]
end

print(greet(name))
```

## 프로젝트 명령

```powershell
surapkg init my_project
surapkg create my_library
surapkg run
surapkg test
surapkg check
surapkg lint
surapkg format --check
surapkg docs
surapkg quality
surapkg ci
```

- `init`은 현재 디렉터리에 `sura.pkg.json`과 소스 파일을 만든다.
- `create`는 패키지 골격 디렉터리를 만든다.
- `run`은 매니페스트의 `main` 파일을 실행한다.
- `test`는 패키지 테스트를 찾고 실행한다.
- `check`, `lint`, `format`은 릴리스 전에 소스 품질을 검사한다.
- `docs`, `quality`, `ci`는 문서·테스트·감사 결과를 배포용 보고서로 만든다.

각 명령은 지원되는 경우 `--json <파일>`을 받아 CI에서 읽을 수 있는 결과를
기록한다. 정확한 인자는 설치된 버전의 `surapkg --help`를 기준으로 한다.

## 의존성

```powershell
surapkg install ..\my_local_package
surapkg install package-name@1.2.3
surapkg install package-name@^1.2.0
surapkg restore
surapkg lock
surapkg list --json
surapkg tree --json
surapkg why package-name --json
surapkg outdated
surapkg update package-name
surapkg remove package-name
```

로컬 경로, 번들 파일, 로컬 또는 HTTP 레지스트리의 이름·버전 참조를 설치할
수 있다. 해결된 직접·전이 의존성은 `sura.lock.json`에 기록한다. 버전 제약은
정확한 버전, `^`, `~`와 비교 연산자 범위를 지원한다.

기본 로컬 레지스트리 위치는 `SURA_REGISTRY`, HTTP 레지스트리는
`SURA_REGISTRY_URL`로 지정한다. 저장소에 실제로 운영하지 않는 공개 레지스트리
주소를 문서에 가정해서 적지 않는다.

## 게시와 공급망 검사

```powershell
surapkg publish . --dry-run --json publish-check.json
surapkg sign . --json sign.json
surapkg verify . --json verify.json
surapkg audit . --json audit.json --sarif audit.sarif
surapkg release . --dry-run --json release.json
```

HTTP 게시에는 `SURA_REGISTRY_TOKEN`이 필요하다. 서명, 잠금 파일, 레지스트리
메타데이터와 보안 권고는 서로 다른 검사이므로 게시 전에 `verify`, `audit`,
`quality`를 모두 실행한다. `publish --dry-run`과 `release --dry-run`은 외부
레지스트리를 변경하지 않고 제출 가능 여부를 검사한다.

## 버전과 호환성

```powershell
surapkg version
surapkg version . patch
surapkg version . minor
surapkg version . 2.0.0
```

패키지 버전은 `major.minor.patch` 형식을 사용한다. 언어와 런타임 자체의
하위 호환 기준, 지원 중단 기간, 바이트코드와 ABI 계약은
[Sura 호환성과 지원 정책](COMPATIBILITY.md)을 따른다.

## 진단

설치 경로, PATH 충돌, 컴파일러와 레지스트리 설정을 확인하려면 다음을
실행한다.

```powershell
surapkg doctor --json doctor.json
```

문제가 발생하면 `doctor.json`, 사용한 Sura 버전, 운영체제와 실패한 명령을
함께 남겨야 다른 환경에서도 재현할 수 있다.
