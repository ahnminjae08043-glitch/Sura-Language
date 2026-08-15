param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_pkg_scaffold_" + [System.Guid]::NewGuid().ToString("N"))

function Run-Pkg {
    param([string[]]$PkgArgs, [string]$WorkDir)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location $WorkDir
    try {
        $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
        $ErrorActionPreference = $old
    }
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    $initDir = Join-Path $temp "init_project"
    New-Item -ItemType Directory -Force -Path $initDir | Out-Null
    $init = Run-Pkg -WorkDir $initDir -PkgArgs @("init", "hello_init", "--json", "init-report.json")
    if ($init.Code -ne 0 -or $init.Output -notmatch "initialized package hello_init") {
        Write-Output $init.Output
        throw "expected init --json to pass"
    }
    foreach ($path in @("sura.pkg.json", "src/hello_init.sura", "init-report.json")) {
        if (-not (Test-Path -LiteralPath (Join-Path $initDir $path))) {
            throw "expected init file: $path"
        }
    }
    $initReport = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $initDir "init-report.json") | ConvertFrom-Json
    $initFiles = @($initReport.files)
    if ($initReport.schema -ne "sura.package.init.v1" -or
        -not $initReport.passed -or
        $initReport.package -ne "hello_init" -or
        $initReport.main -notmatch "src/hello_init\.sura" -or
        [int]$initReport.file_count -ne 2 -or
        -not ($initFiles | Where-Object { $_.kind -eq "manifest" -and $_.path -match "sura\.pkg\.json" }) -or
        -not ($initFiles | Where-Object { $_.kind -eq "main" -and $_.path -match "src/hello_init\.sura" })) {
        Get-Content -Raw -Encoding UTF8 -Path (Join-Path $initDir "init-report.json") | Write-Output
        throw "expected init JSON report to describe scaffold"
    }

    $createDir = Join-Path $temp "create_workspace"
    New-Item -ItemType Directory -Force -Path $createDir | Out-Null
    $create = Run-Pkg -WorkDir $createDir -PkgArgs @("create", "hello_create", "--json", "create-report.json")
    if ($create.Code -ne 0 -or $create.Output -notmatch "created package skeleton hello_create") {
        Write-Output $create.Output
        throw "expected create --json to pass"
    }
    foreach ($path in @("hello_create/sura.pkg.json", "hello_create/src/hello_create.sura", "create-report.json")) {
        if (-not (Test-Path -LiteralPath (Join-Path $createDir $path))) {
            throw "expected create file: $path"
        }
    }
    $createReport = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $createDir "create-report.json") | ConvertFrom-Json
    $createFiles = @($createReport.files)
    if ($createReport.schema -ne "sura.package.create.v1" -or
        -not $createReport.passed -or
        $createReport.package -ne "hello_create" -or
        $createReport.root -notmatch "hello_create" -or
        $createReport.main -notmatch "hello_create/src/hello_create\.sura" -or
        [int]$createReport.file_count -ne 2 -or
        -not ($createFiles | Where-Object { $_.kind -eq "manifest" -and $_.path -match "hello_create/sura\.pkg\.json" }) -or
        -not ($createFiles | Where-Object { $_.kind -eq "main" -and $_.path -match "hello_create/src/hello_create\.sura" })) {
        Get-Content -Raw -Encoding UTF8 -Path (Join-Path $createDir "create-report.json") | Write-Output
        throw "expected create JSON report to describe scaffold"
    }

    $newDir = Join-Path $temp "new_workspace"
    New-Item -ItemType Directory -Force -Path $newDir | Out-Null
    $new = Run-Pkg -WorkDir $newDir -PkgArgs @("new", "hello_starter", "--json", "new-report.json")
    if ($new.Code -ne 0 -or $new.Output -notmatch "created starter project hello_starter") {
        Write-Output $new.Output
        throw "expected new --json to create the starter project"
    }
    $starterFiles = @(
        "hello_starter/sura.pkg.json",
        "hello_starter/src/hello_starter.sura",
        "hello_starter/src/greeting.sura",
        "hello_starter/tests/greeting_test.sura",
        "hello_starter/README.md",
        "hello_starter/.gitignore",
        "hello_starter/.vscode/extensions.json",
        "hello_starter/.vscode/settings.json",
        "hello_starter/.vscode/launch.json",
        "hello_starter/.vscode/tasks.json",
        "new-report.json"
    )
    foreach ($path in $starterFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $newDir $path))) {
            throw "expected starter file: $path"
        }
    }
    $newReport = Get-Content -Raw -Encoding UTF8 -Path (Join-Path $newDir "new-report.json") | ConvertFrom-Json
    $newFiles = @($newReport.files)
    if ($newReport.schema -ne "sura.package.new.v1" -or
        -not $newReport.passed -or
        $newReport.package -ne "hello_starter" -or
        [int]$newReport.file_count -ne 10 -or
        -not ($newFiles | Where-Object { $_.kind -eq "main" -and $_.path -match "hello_starter/src/hello_starter\.sura" }) -or
        -not ($newFiles | Where-Object { $_.kind -eq "test" -and $_.path -match "hello_starter/tests/greeting_test\.sura" }) -or
        @($newFiles | Where-Object { $_.kind -eq "vscode" }).Count -ne 4) {
        Get-Content -Raw -Encoding UTF8 -Path (Join-Path $newDir "new-report.json") | Write-Output
        throw "expected new JSON report to describe the complete starter project"
    }

    $run = Run-Pkg -WorkDir (Join-Path $newDir "hello_starter") -PkgArgs @("run", "--no-jit", "--", "Codex")
    if ($run.Code -ne 0 -or $run.Output -notmatch "Hello, Codex!") {
        Write-Output $run.Output
        throw "expected generated starter project to run"
    }
    $test = Run-Pkg -WorkDir (Join-Path $newDir "hello_starter") -PkgArgs @("test", "--no-jit")
    if ($test.Code -ne 0 -or $test.Output -notmatch "1 passed, 0 failed") {
        Write-Output $test.Output
        throw "expected generated starter project tests to pass"
    }

    $help = Run-Pkg -WorkDir $temp -PkgArgs @("init", "--help")
    if ($help.Code -ne 0 -or $help.Output -notmatch "init \[name\] \[--json report\.json\]") {
        Write-Output $help.Output
        throw "expected init help to mention --json"
    }
    $help = Run-Pkg -WorkDir $temp -PkgArgs @("create", "--help")
    if ($help.Code -ne 0 -or $help.Output -notmatch "create <name> \[--json report\.json\]") {
        Write-Output $help.Output
        throw "expected create help to mention --json"
    }
    $help = Run-Pkg -WorkDir $temp -PkgArgs @("new", "--help")
    if ($help.Code -ne 0 -or $help.Output -notmatch "new <name> \[--json report\.json\]") {
        Write-Output $help.Output
        throw "expected new help to mention --json"
    }

    "pkg_scaffold_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# Verified passing; state the exit code rather than inheriting it.
exit 0
