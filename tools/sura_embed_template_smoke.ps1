param(
    [string]$Surapkg = ".\surapkg.exe",
    [string]$Cxx = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = Split-Path -Parent $PSScriptRoot
$SurapkgPath = (Resolve-Path $Surapkg).Path

function Resolve-Cxx {
    param([string]$Requested)
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Requested)) { $candidates += $Requested }
    if ($IsWindows -or $env:OS -eq "Windows_NT") { $candidates += "C:\msys64\mingw64\bin\g++.exe" }
    $candidates += @("c++", "g++", "clang++")
    foreach ($candidate in $candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) { continue }
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    throw "C++ compiler not found. Pass -Cxx or install c++/g++/clang++."
}

$CxxPath = Resolve-Cxx $Cxx
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_embed_template_" + [System.Guid]::NewGuid().ToString("N"))

function Run-Checked {
    param(
        [string]$Label,
        [scriptblock]$Command,
        [string]$Expect
    )
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Stop"
    $threw = $false
    try {
        $global:LASTEXITCODE = 0
        $out = & $Command 2>&1 | ForEach-Object { "$_" }
    }
    catch {
        $threw = $true
        $out = @("$($_.Exception.Message)")
    }
    finally {
        $ErrorActionPreference = $old
    }
    $text = $out -join "`n"
    if ($threw -or ($Expect -and $text -notmatch $Expect)) {
        Write-Output $text
        throw "expected $Label to pass"
    }
    return $text
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    Push-Location $temp
    try {
        Run-Checked "surapkg embed" {
            & $SurapkgPath embed game_embed --json embed-report.json
            if ($LASTEXITCODE -ne 0) { throw "surapkg embed failed with exit code $LASTEXITCODE" }
        } "created native embed host project" | Out-Null
        $project = Join-Path $temp "game_embed"
        $embedReportPath = Join-Path $temp "embed-report.json"
        $hostFile = Join-Path $project "host/main.cpp"
        $script = Join-Path $project "scripts/tick.sura"
        $build = Join-Path $project "build.ps1"
        $run = Join-Path $project "run.ps1"
        $cmakeFile = Join-Path $project "CMakeLists.txt"
        $readme = Join-Path $project "README.md"
        if (-not (Test-Path -LiteralPath $embedReportPath)) {
            throw "expected embed JSON report to be created"
        }
        $embedReport = Get-Content -Raw -Encoding UTF8 -Path $embedReportPath | ConvertFrom-Json
        $embedFiles = @($embedReport.files)
        if ($embedReport.schema -ne "sura.package.embed.v1" -or
            -not $embedReport.passed -or
            $embedReport.package -ne "game_embed" -or
            $embedReport.host -notmatch "host/main\.cpp" -or
            $embedReport.script -notmatch "scripts/tick\.sura" -or
            $embedReport.build_script -notmatch "build\.ps1" -or
            $embedReport.run_script -notmatch "run\.ps1" -or
            $embedReport.cmake -notmatch "CMakeLists\.txt" -or
            [int]$embedReport.file_count -lt 7 -or
            -not ($embedFiles | Where-Object { $_.kind -eq "host" -and $_.path -match "host/main\.cpp" }) -or
            -not ($embedFiles | Where-Object { $_.kind -eq "script" -and $_.path -match "scripts/tick\.sura" }) -or
            -not ($embedFiles | Where-Object { $_.kind -eq "build_script" -and $_.path -match "build\.ps1" }) -or
            -not ($embedFiles | Where-Object { $_.kind -eq "cmake" -and $_.path -match "CMakeLists\.txt" })) {
            Get-Content -Raw -Encoding UTF8 -Path $embedReportPath | Write-Output
            throw "expected embed JSON report to describe generated native host files"
        }

        foreach ($path in @($hostFile, $script, $build, $run, $cmakeFile, $readme, (Join-Path $project "sura.pkg.json"))) {
            if (-not (Test-Path -LiteralPath $path)) {
                throw "expected generated embed file: $path"
            }
        }

        $hostText = Get-Content -Raw -Encoding UTF8 -Path $hostFile
        $scriptText = Get-Content -Raw -Encoding UTF8 -Path $script
        $cmakeText = Get-Content -Raw -Encoding UTF8 -Path $cmakeFile
        $readmeText = Get-Content -Raw -Encoding UTF8 -Path $readme
        if ($hostText -notmatch "sura_new" -or
            $hostText -notmatch "sura_set_number" -or
            $hostText -notmatch "sura_run" -or
            $hostText -notmatch "sura_get_number") {
            throw "generated host should use the Sura C ABI"
        }
        if ($scriptText -notmatch "func update_state" -or
            $scriptText -notmatch "next_x" -or
            $scriptText -notmatch "state_label") {
            throw "generated script should expose host-readable outputs"
        }
        if ($cmakeText -notmatch "SURA_ROOT" -or
            $cmakeText -notmatch "sura_ffi\.cpp" -or
            $cmakeText -notmatch "gc\.cpp" -or
            $cmakeText -notmatch "target_include_directories" -or
            $cmakeText -notmatch "target_link_libraries") {
            throw "generated CMakeLists.txt should describe a portable native host build"
        }
        if ($readmeText -notmatch "SURA_ROOT" -or
            $readmeText -notmatch "sura_ffi" -or
            $readmeText -notmatch "CMake") {
            throw "generated README should explain native embedding build inputs"
        }

        Push-Location $project
        try {
            Run-Checked "embed template build" { & $build -SuraRoot $root -Cxx $CxxPath } "built" | Out-Null
            $runText = Run-Checked "embed template run" { & $run } "embed_template:\s+PASS"
            if ($runText -notmatch "next_x=104" -or
                $runText -notmatch "state=arena:104" -or
                $runText -notmatch "score=1047") {
                Write-Output $runText
                throw "generated embed host should return expected script outputs"
            }

            $cmake = Get-Command cmake -ErrorAction SilentlyContinue
            if ($cmake) {
                $cmakeBuild = Join-Path $project "build-cmake"
                $configureArgs = @("-S", ".", "-B", $cmakeBuild, "-DSURA_ROOT=$root")
                $canConfigure = $true
                $isWindowsHost = $IsWindows -or $env:OS -eq "Windows_NT"
                if (Get-Command ninja -ErrorAction SilentlyContinue) {
                    $configureArgs += @("-G", "Ninja", "-DCMAKE_CXX_COMPILER=$CxxPath")
                }
                elseif ($isWindowsHost -and (Get-Command mingw32-make -ErrorAction SilentlyContinue)) {
                    $configureArgs += @("-G", "MinGW Makefiles", "-DCMAKE_CXX_COMPILER=$CxxPath")
                }
                elseif (-not $isWindowsHost) {
                    $configureArgs += @("-DCMAKE_CXX_COMPILER=$CxxPath")
                }
                elseif ($CxxPath -notmatch "g\+\+|clang\+\+") {
                    $configureArgs += @("-DCMAKE_CXX_COMPILER=$CxxPath")
                }
                else {
                    $canConfigure = $false
                    Write-Output "embed_template_smoke: skipping CMake compile; no Ninja or MinGW Makefiles generator helper found"
                }

                if ($canConfigure) {
                    Run-Checked "embed template cmake configure" {
                        & $cmake.Source @configureArgs
                        if ($LASTEXITCODE -ne 0) { throw "cmake configure failed with exit code $LASTEXITCODE" }
                    } "Build files" | Out-Null
                    Run-Checked "embed template cmake build" {
                        & $cmake.Source --build $cmakeBuild --config Release
                        if ($LASTEXITCODE -ne 0) { throw "cmake build failed with exit code $LASTEXITCODE" }
                    } "" | Out-Null
                    $exeName = if ($isWindowsHost) { "game_embed_host.exe" } else { "game_embed_host" }
                    $cmakeExe = @(
                        (Join-Path $cmakeBuild $exeName),
                        (Join-Path (Join-Path $cmakeBuild "Release") $exeName),
                        (Join-Path (Join-Path $cmakeBuild "Debug") $exeName),
                        (Join-Path (Join-Path $cmakeBuild "RelWithDebInfo") $exeName)
                    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
                    if (-not $cmakeExe) {
                        throw "expected CMake build output executable"
                    }
                    $cmakeRunText = Run-Checked "embed template cmake run" {
                        & $cmakeExe "scripts/tick.sura"
                        if ($LASTEXITCODE -ne 0) { throw "cmake host run failed with exit code $LASTEXITCODE" }
                    } "embed_template:\s+PASS"
                    if ($cmakeRunText -notmatch "next_x=104" -or
                        $cmakeRunText -notmatch "state=arena:104" -or
                        $cmakeRunText -notmatch "score=1047") {
                        Write-Output $cmakeRunText
                        throw "generated CMake host should return expected script outputs"
                    }
                }
            }
        }
        finally {
            Pop-Location
        }
    }
    finally {
        Pop-Location
    }

    "embed_template_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
