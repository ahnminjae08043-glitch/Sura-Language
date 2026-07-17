param(
    [string]$RepoRoot = ".",
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

$root = (Resolve-Path -LiteralPath $RepoRoot).Path

function Resolve-SuraEngine([string]$Candidate) {
    if (-not [string]::IsNullOrWhiteSpace($Candidate)) {
        return (Resolve-Path -LiteralPath $Candidate).Path
    }
    $preferred = Join-Path $root "SuraLanguage.exe"
    if (Test-Path -LiteralPath $preferred) { return $preferred }
    $fallback = Join-Path $root "SuraEngine.exe"
    if (Test-Path -LiteralPath $fallback) { return $fallback }
    throw "Sura engine executable not found. Build first or pass -Engine."
}

function Invoke-SuraCapture([string]$EnginePath, [string[]]$EngineArgs) {
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $text = (& $EnginePath @EngineArgs 2>&1 | Out-String)
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    [pscustomobject]@{
        ExitCode = $LASTEXITCODE
        Output = $text
    }
}

$enginePath = Resolve-SuraEngine $Engine
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_lang_policy_" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
$koRuntimeError = [string]::Concat([char[]]@(0xB7F0, 0xD0C0, 0xC784, 0x0020, 0xC624, 0xB958))
$koParseError = [string]::Concat([char[]]@(0xC218, 0xB77C, 0x0020, 0xD30C, 0xC11C, 0x0020, 0xC624, 0xB958))
$koNilDeref = [string]::Concat([char[]]@(0xC5ED, 0xCC38, 0xC870))
$koLine = [string]::Concat([char[]]@(0xBC88, 0xC9F8, 0x0020, 0xC904))
$koColumn = [string]::Concat([char[]]@(0xBC88, 0xC9F8, 0x0020, 0xCE78))

try {
    $program = Join-Path $temp "nil_deref.sura"
    [System.IO.File]::WriteAllText($program, "x is nil`nprint x.name`n", [System.Text.Encoding]::UTF8)

    $defaultRun = Invoke-SuraCapture $enginePath @($program)
    if ($defaultRun.ExitCode -eq 0 -or
        $defaultRun.Output -notmatch "Runtime Error" -or
        $defaultRun.Output -notmatch "nil dereference" -or
        $defaultRun.Output -notmatch "line 2" -or
        $defaultRun.Output -notmatch "col" -or
        $defaultRun.Output -match $koNilDeref -or
        $defaultRun.Output -match $koLine) {
        Write-Host $defaultRun.Output
        throw "Default diagnostics should be English and searchable"
    }

    $koreanRun = Invoke-SuraCapture $enginePath @("--lang", "ko", $program)
    if ($koreanRun.ExitCode -eq 0 -or
        $koreanRun.Output -notmatch $koRuntimeError -or
        $koreanRun.Output -notmatch ("nil " + $koNilDeref) -or
        $koreanRun.Output -notmatch $koLine -or
        $koreanRun.Output -notmatch $koColumn) {
        Write-Host $koreanRun.Output
        throw "--lang ko diagnostics should stay Korean"
    }

    $oldLang = $env:SURA_LANG
    try {
        $env:SURA_LANG = "ko"
        $envRun = Invoke-SuraCapture $enginePath @($program)
    } finally {
        if ($null -eq $oldLang) { Remove-Item Env:SURA_LANG -ErrorAction SilentlyContinue }
        else { $env:SURA_LANG = $oldLang }
    }
    if ($envRun.ExitCode -eq 0 -or
        $envRun.Output -notmatch $koRuntimeError -or
        $envRun.Output -notmatch $koLine) {
        Write-Host $envRun.Output
        throw "SURA_LANG=ko should select Korean diagnostics"
    }

    $parseProgram = Join-Path $temp "parse_error.sura"
    [System.IO.File]::WriteAllText($parseProgram, "value is`npritn 1`n", [System.Text.Encoding]::UTF8)
    $parseRun = Invoke-SuraCapture $enginePath @("--lang", "en", "--check", $parseProgram)
    if ($parseRun.ExitCode -eq 0 -or
        $parseRun.Output -notmatch "expected expression" -or
        $parseRun.Output -notmatch "is not a known (statement keyword|command)" -or
        $parseRun.Output -notmatch "did you mean 'print'" -or
        $parseRun.Output -match $koLine -or
        $parseRun.Output -match $koRuntimeError) {
        Write-Host $parseRun.Output
        throw "Default parser diagnostics should be English and recover across lines"
    }

    $parseKoRun = Invoke-SuraCapture $enginePath @("--lang", "ko", "--check", $parseProgram)
    if ($parseKoRun.ExitCode -eq 0 -or
        $parseKoRun.Output -notmatch $koParseError -or
        $parseKoRun.Output -notmatch $koLine -or
        $parseKoRun.Output -notmatch $koColumn -or
        $parseKoRun.Output -match [char]0xFFFD) {
        Write-Host $parseKoRun.Output
        throw "--lang ko parser diagnostics should use valid UTF-8 Korean labels"
    }

    $helpRun = Invoke-SuraCapture $enginePath @("--help")
    if ($helpRun.ExitCode -ne 0 -or
        $helpRun.Output -notmatch "--lang en\|ko" -or
        $helpRun.Output -notmatch "SURA_LANG") {
        Write-Host $helpRun.Output
        throw "Help output should document --lang and SURA_LANG"
    }

    Write-Host "sura_language_policy_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
