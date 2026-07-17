param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_undefined_variable_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Engine {
    param([string[]]$EngineArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $enginePath @EngineArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $hello = -join ([char[]](0xC548, 0xB155))

    $quotedScript = Join-Path $temp "quoted_korean.sura"
    Write-Text $quotedScript @"
b is "$hello"
print b
"@
    $quoted = Run-Engine -EngineArgs @($quotedScript)
    if ($quoted.Code -ne 0 -or $quoted.Output -notmatch [regex]::Escape($hello)) {
        Write-Output $quoted.Output
        throw "expected quoted Korean string to print without diagnostics"
    }

    $bareKoreanScript = Join-Path $temp "bare_korean.sura"
    Write-Text $bareKoreanScript @"
a is 10
b is $hello
print b
"@
    $bareKorean = Run-Engine -EngineArgs @($bareKoreanScript)
    if ($bareKorean.Code -eq 0 -or
        $bareKorean.Output -notmatch "\[E100\]" -or
        $bareKorean.Output -notmatch [regex]::Escape($hello) -or
        $bareKorean.Output -match "(?m)^nil$") {
        Write-Output $bareKorean.Output
        throw "expected bare Korean identifier to fail as undefined instead of becoming nil"
    }

    $bareAsciiScript = Join-Path $temp "bare_ascii.sura"
    Write-Text $bareAsciiScript @"
a is 10
b is hello
print b
"@
    $bareAscii = Run-Engine -EngineArgs @($bareAsciiScript)
    if ($bareAscii.Code -eq 0 -or
        $bareAscii.Output -notmatch "\[E100\]" -or
        $bareAscii.Output -notmatch "hello" -or
        $bareAscii.Output -match "(?m)^nil$") {
        Write-Output $bareAscii.Output
        throw "expected bare ASCII identifier to fail as undefined instead of becoming nil"
    }

    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $replOut = @("a is 10", "print b", "quit") | & $enginePath --repl 2>&1 | ForEach-Object { "$_" }
    $replCode = $LASTEXITCODE
    $ErrorActionPreference = $old
    $replText = $replOut -join "`n"
    if ($replCode -ne 0 -or
        $replText -notmatch "\[E100\]" -or
        $replText -notmatch "'b'" -or
        $replText -match "(?m)^nil$") {
        Write-Output $replText
        throw "expected REPL missing global lookup to fail as undefined instead of becoming nil"
    }

    "undefined_variable_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
