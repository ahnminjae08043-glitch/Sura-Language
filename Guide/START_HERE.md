# Sura 10분 시작 가이드

이 가이드는 Windows x64에서 Sura를 설치하고, 첫 프로젝트를 실행하고, 테스트하는 가장 짧은 경로입니다.

## 1. 설치

`SuraLanguageSetup-1.11.1.exe`를 실행합니다. 설치가 끝나면 기존 PowerShell을 닫고 새 PowerShell을 엽니다.

```powershell
sura --version
```

정상 설치라면 `Sura Language 1.11.1`이 표시됩니다.

## 2. 프로젝트 생성

프로젝트를 둘 폴더에서 다음 명령을 실행합니다.

```powershell
surapkg new hello_sura
cd hello_sura
```

생성되는 주요 파일은 다음과 같습니다.

```text
hello_sura/
├─ sura.pkg.json
├─ src/
│  ├─ hello_sura.sura
│  └─ greeting.sura
├─ tests/
│  └─ greeting_test.sura
├─ .vscode/
│  ├─ extensions.json
│  ├─ launch.json
│  ├─ settings.json
│  └─ tasks.json
└─ README.md
```

## 3. 실행

```powershell
surapkg run
```

예상 출력:

```text
Hello, Sura!
```

프로그램에 이름을 전달할 수도 있습니다.

```powershell
surapkg run -- Minjae
```

## 4. 테스트

```powershell
surapkg test
```

생성된 `tests/greeting_test.sura`가 실행되고 `1 passed, 0 failed`가 표시됩니다.

## 5. 코드 수정

`src/greeting.sura`를 열어 반환 문자열을 바꿉니다.

```sura
func greet(name: string) -> string do
  return "반가워요, {name}!"
end
```

다시 실행하고 테스트합니다.

```powershell
surapkg run -- Sura
surapkg test
```

## 6. VS Code

VS Code에서 `hello_sura` 폴더를 엽니다. 공식 `Sura Language Support` 확장이 설치되어 있으면 다음 기능을 사용할 수 있습니다.

- `.sura` 문법 강조와 자동완성
- 파일 위의 Run·Debug 동작
- `Run and Debug`의 `Debug Sura Starter` 구성
- `Terminal > Run Task`의 패키지 실행·테스트 작업

확장 명령 `Sura: Create Starter Project`로 같은 프로젝트를 만들 수도 있습니다.

## 문제 해결

설치 경로나 명령 충돌을 확인합니다.

```powershell
surapkg doctor
```

영상 예제에서만 FFmpeg가 필요합니다. 일반 문법, 프로젝트 생성, 실행, 테스트에는 FFmpeg가 필요하지 않습니다.

12개 입문 예제는 [`examples/starter`](../examples/starter/README.md)에 있습니다.
