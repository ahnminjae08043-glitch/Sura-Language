param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$buildBatPath = Join-Path $root "build.bat"
$makefilePath = Join-Path $root "Makefile"

function Read-Utf8Text {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "missing build file: $Path"
    }
    return [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
}

$buildBat = Read-Utf8Text $buildBatPath
$makefile = Read-Utf8Text $makefilePath
$buildTest = Read-Utf8Text (Join-Path $root "build_test.bat")
$compileScript = Read-Utf8Text (Join-Path $root "compile.ps1")
$playScript = Read-Utf8Text (Join-Path $root "play.bat")

if ($buildBat -notmatch '(?i)BUILD_MODE' -or
    $buildBat -notmatch '(?i)portable' -or
    $buildBat -notmatch '(?i)native' -or
    $buildBat -notmatch '(?i)SURA_CXX') {
    throw "build.bat must expose portable/native modes and the SURA_CXX compiler override"
}
if ($makefile -notmatch '(?m)^TARGET\s*=\s*SuraLanguage\$\(EXEEXT\)\s*$' -or
    $makefile -notmatch '(?m)^PKG_TARGET\s*=\s*surapkg\$\(EXEEXT\)\s*$' -or
    $makefile -notmatch '(?m)^ENGINE_SOURCES\s*=\s*main\.cpp\s+gc\.cpp\s+platform\.cpp\s*$' -or
    $makefile -match 'SuraFinal') {
    throw "Makefile must use the canonical SuraLanguage and surapkg build graph"
}
if ($makefile -match 'main2\.cpp' -or $makefile -match 'SuraEngine2') {
    throw "Makefile still references a legacy engine entry point or artifact"
}
foreach ($entry in @(
    @{ Name = "build.bat"; Text = $buildBat },
    @{ Name = "build_test.bat"; Text = $buildTest },
    @{ Name = "compile.ps1"; Text = $compileScript },
    @{ Name = "play.bat"; Text = $playScript }
)) {
    if ($entry.Text -match 'main2\.cpp' -or
        $entry.Text -match 'SuraEngine2' -or
        $entry.Text -match 'SuraFinal' -or
        $entry.Text -notmatch 'SuraLanguage\.exe') {
        throw "$($entry.Name) must use the canonical SuraLanguage runtime"
    }
}
if ($makefile -notmatch '(?m)^NATIVE\s*\?=\s*0\s*$' -or
    $makefile -notmatch '(?m)^ifeq \(\$\(NATIVE\),1\)\s*$') {
    throw "Makefile native optimization must be explicit opt-in"
}

# Exercise batch argument handling with a compiler shim. This proves that the
# default is portable and that invalid modes fail without compiling, while
# avoiding two redundant full C++ builds in CI.
if ($env:OS -eq "Windows_NT") {
    $temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_build_contract_" + [System.Guid]::NewGuid().ToString("N"))
    $oldCxx = $env:SURA_CXX
    $oldLog = $env:SURA_FAKE_CXX_LOG

    function Invoke-BuildMode {
        param([AllowNull()][string]$Mode)

        if (Test-Path -LiteralPath $env:SURA_FAKE_CXX_LOG) {
            Remove-Item -LiteralPath $env:SURA_FAKE_CXX_LOG -Force
        }
        $arguments = @('/d', '/c', $buildCopy)
        if (-not [string]::IsNullOrWhiteSpace($Mode)) {
            $arguments += $Mode
        }
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $output = (& $env:ComSpec @arguments 2>&1) | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $oldPreference
        $compilerLog = ""
        if (Test-Path -LiteralPath $env:SURA_FAKE_CXX_LOG) {
            $compilerLog = [System.IO.File]::ReadAllText($env:SURA_FAKE_CXX_LOG, [System.Text.Encoding]::UTF8)
        }
        return [pscustomobject]@{
            Code = $code
            Output = ($output -join "`n")
            CompilerLog = $compilerLog
        }
    }

    try {
        New-Item -ItemType Directory -Force -Path $temp | Out-Null
        $buildCopy = Join-Path $temp "build.bat"
        Copy-Item -LiteralPath $buildBatPath -Destination $buildCopy
        $fakeCompiler = Join-Path $temp "fake-cxx.cmd"
        $fakeLog = Join-Path $temp "compiler-args.log"
        [System.IO.File]::WriteAllText(
            $fakeCompiler,
            "@echo off`r`n>> `"%SURA_FAKE_CXX_LOG%`" echo %*`r`nexit /b 0`r`n",
            [System.Text.Encoding]::ASCII
        )
        $env:SURA_CXX = $fakeCompiler
        $env:SURA_FAKE_CXX_LOG = $fakeLog

        Push-Location $temp
        try {
            $portable = Invoke-BuildMode $null
            if ($portable.Code -ne 0 -or
                $portable.Output -notmatch 'Sura build mode:\s*portable' -or
                $portable.CompilerLog -match '(?<!\S)-march=native(?!\S)' -or
                $portable.CompilerLog -notmatch 'main\.cpp gc\.cpp platform\.cpp -o SuraLanguage\.exe' -or
                $portable.CompilerLog -notmatch 'surapkg\.cpp -o surapkg\.exe') {
                $portable | Format-List | Out-String | Write-Host
                throw "default build.bat invocation must be a canonical portable build"
            }

            $native = Invoke-BuildMode "native"
            if ($native.Code -ne 0 -or
                $native.Output -notmatch 'Sura build mode:\s*native' -or
                $native.CompilerLog -notmatch '(?<!\S)-march=native(?!\S)') {
                $native | Format-List | Out-String | Write-Host
                throw "build.bat native must explicitly enable -march=native"
            }

            $invalid = Invoke-BuildMode "unsupported-mode"
            if ($invalid.Code -eq 0 -or
                $invalid.Output -notmatch 'Usage:\s*build\.bat' -or
                -not [string]::IsNullOrWhiteSpace($invalid.CompilerLog)) {
                $invalid | Format-List | Out-String | Write-Host
                throw "build.bat must reject unknown build modes before compiling"
            }
        }
        finally {
            Pop-Location
        }
    }
    finally {
        $env:SURA_CXX = $oldCxx
        $env:SURA_FAKE_CXX_LOG = $oldLog
        if (Test-Path -LiteralPath $temp) {
            Remove-Item -LiteralPath $temp -Recurse -Force
        }
    }
}

Write-Host "sura_build_contract_smoke: PASS"
