param(
    [string]$RepoRoot = ".",
    [string]$Cxx = "",
    [switch]$Sanitize
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$candidates = @()
if (-not [string]::IsNullOrWhiteSpace($Cxx)) { $candidates += $Cxx }
if (-not [string]::IsNullOrWhiteSpace($env:SURA_CXX)) { $candidates += $env:SURA_CXX }
if ($env:OS -eq "Windows_NT") { $candidates += "C:\msys64\mingw64\bin\g++.exe" }
$candidates += @("c++", "g++", "clang++")

$compiler = $null
foreach ($candidate in $candidates | Select-Object -Unique) {
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $compiler = (Resolve-Path -LiteralPath $candidate).Path
        break
    }
    $command = Get-Command $candidate -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) {
        $compiler = $command.Source
        break
    }
}
if (-not $compiler) { throw "C++ compiler not found for untrusted-input smoke" }

$isWindows = $env:OS -eq "Windows_NT"
$isLinux = [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
    [System.Runtime.InteropServices.OSPlatform]::Linux)
$suffix = if ($isWindows) { ".exe" } else { "" }
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_untrusted_input_" + [Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $tests = @(
        @{ Source = "tests/parser_untrusted_input_test.cpp"; Name = "parser_untrusted_input_test"; Marker = "parser untrusted input: PASS" },
        @{ Source = "tests/bytecode_validation_test.cpp"; Name = "bytecode_validation_test"; Marker = "bytecode validation:" }
    )

    foreach ($test in $tests) {
        $binary = Join-Path $temp ($test.Name + $suffix)
        $compileArgs = @("-std=c++17", "-DNDEBUG", "-Wall", "-Wextra", "-pedantic", "-I.")
        if ($Sanitize) {
            $compileArgs += @("-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined")
        } else {
            $compileArgs += "-O2"
        }
        $compileArgs += @($test.Source, "platform.cpp", "gc.cpp", "-o", $binary)
        if ($Sanitize) { $compileArgs += "-fsanitize=address,undefined" }
        if ($isLinux) { $compileArgs += "-ldl" }

        Push-Location $root
        try {
            & $compiler @compileArgs
            if ($LASTEXITCODE -ne 0) { throw "$($test.Name) compilation failed" }
        } finally {
            Pop-Location
        }

        $output = & $binary 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $text = $output -join "`n"
        $output | Write-Host
        if ($code -ne 0 -or $text -notmatch [regex]::Escape([string]$test.Marker)) {
            throw "$($test.Name) failed (exit=$code)"
        }
    }

    Write-Host "sura_untrusted_input_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
# Verified passing before this line was added. A gate that prints PASS
# states its exit code rather than inheriting the last command's.
exit 0
