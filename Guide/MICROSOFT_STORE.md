# Microsoft Store 배포

Sura Language는 Microsoft Store 승인을 통과해 공개된 무료 Windows 앱입니다. 2026-07-15에 `winget show --id 9P5JFKSWTP0P --source msstore`로 공개 제품, 게시자 `SuraTeam`, 가격 `Free`를 확인했습니다.

- 제품 이름: `Sura Language`
- Store ID: `9P5JFKSWTP0P`
- Package Identity Name: `SuraTeam.SuraLanguage`
- Package Identity Publisher: `CN=7D09337E-F8F3-4455-BD86-A6928DC8F552`
- Publisher Display Name: `SuraTeam`
- Package Family Name: `SuraTeam.SuraLanguage_skvn0agb8ca3y`
- 공개 주소: `https://apps.microsoft.com/detail/9P5JFKSWTP0P`

## MSIX 만들기

필수 도구는 Microsoft WinApp CLI입니다.

```powershell
winget install --id Microsoft.WinAppCli --source winget
```

저장소 루트에서 다음 명령을 실행합니다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_store_msix.ps1 -RunSmoke
```

출력 파일:

```text
dist\store-msix\SuraLanguage-1.11.1.0-x64.msix
dist\store-msix\SuraLanguage-1.11.1.0-x64.msix.json
```

패키지는 `sura.exe`와 `surapkg.exe` App Execution Alias를 등록합니다. 로컬 스모크 검사는 loose MSIX layout을 임시 등록하여 두 명령을 실행한 뒤 등록을 제거합니다.

Store 제출용 MSIX에는 공개 배포 인증서를 직접 붙이지 않아도 됩니다. Microsoft Store 인증을 통과한 패키지는 Microsoft가 다시 서명합니다. 웹사이트의 기본 설치 버튼은 Store 주소를 사용합니다. 기존 unsigned EXE 직접 다운로드는 Store 배포와 별개이므로 SmartScreen 경고가 계속 나타날 수 있습니다.
