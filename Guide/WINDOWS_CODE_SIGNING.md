# Windows 코드 서명

웹사이트에서 직접 배포하는 EXE의 Windows 게시자 신뢰는 HTTPS나 SHA-256만으로 만들어지지 않습니다. 직접 다운로드 파일의 경고를 줄이려면 공개적으로 신뢰되는 코드 서명 인증서로 실행 파일을 Authenticode 서명하고 타임스탬프를 남겨야 합니다. 새 인증서나 다운로드 수가 적은 파일은 유효하게 서명돼도 평판 기반 경고가 즉시 사라진다고 보장할 수 없습니다.

자체 서명 인증서는 로컬 시험에는 쓸 수 있지만 다른 사용자의 Windows가 기본으로 신뢰하지 않으므로 공개 배포 경고의 해결책이 아닙니다. 공개 코드 서명 인증서는 발급 기관의 상품이며 무료라고 가정하지 않습니다. 인증서 비용과 유효기간은 선택한 발급 기관의 현재 조건을 확인합니다.

무료 배포 경로는 Microsoft Store 인증입니다. Store 제출용 MSIX가 인증을 통과하면 Microsoft가 배포 패키지를 서명합니다. 이 서명은 웹사이트에서 따로 제공하는 unsigned EXE에는 적용되지 않습니다. Store 제품이 실제 공개된 뒤 웹사이트 기본 버튼을 Store 주소로 바꿉니다.

## 직접 배포 파일 서명

PFX 비밀번호는 명령줄에 쓰지 않고 환경 변수로 전달합니다.

```powershell
$version = (Get-Content .\version.json -Raw | ConvertFrom-Json).version
$env:SURA_CODESIGN_PFX_PASSWORD = "인증서 비밀번호"
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_sign_windows.ps1 `
  -PfxPath C:\secure\publisher-code-signing.pfx `
  -Files .\SuraLanguage.exe,.\surapkg.exe,".\dist\SuraLanguageSetup-$version.exe" `
  -JsonOut .\artifacts\windows_signing.json
```

서명 후 공개 배포 게이트를 엄격 모드로 실행합니다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_windows_signature_gate.ps1 `
  -Files .\SuraLanguage.exe,.\surapkg.exe,".\dist\SuraLanguageSetup-$version.exe" `
  -ReleaseManifest ".\sura_presentation\public\downloads\release-$version.json" `
  -RequireSigned `
  -JsonOut .\artifacts\windows_signature.json
```

게이트는 각 파일의 상태, 서명자, 인증서 지문, 타임스탬프와 SHA-256을 기록합니다. 서명 뒤에는 파일 해시와 `release-<version>.json`, `SHA256SUMS.txt`를 다시 생성해야 합니다.
