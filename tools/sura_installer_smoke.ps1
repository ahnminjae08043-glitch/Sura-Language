param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Maker = (Join-Path $PSScriptRoot "sura_make_installer.ps1"),
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"

function Get-PowerShellRunner {
    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($pwsh) { return $pwsh.Source }
    $powershell = Get-Command powershell -ErrorAction SilentlyContinue
    if ($powershell) { return $powershell.Source }
    throw "PowerShell runner not found"
}

function Assert-No-MinGWRuntimeImports {
    param([string]$Path)
    $image = [System.Text.Encoding]::ASCII.GetString(
        [System.IO.File]::ReadAllBytes($Path)).ToLowerInvariant()
    foreach ($runtime in @(
        "libgcc_s_seh-1.dll",
        "libstdc++-6.dll",
        "libwinpthread-1.dll"
    )) {
        if ($image.Contains($runtime)) {
            throw "portable installer payload depends on external MinGW runtime: $runtime ($Path)"
        }
    }
}

$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$versionContract = [System.IO.File]::ReadAllText((Join-Path $root "version.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
if ($versionContract.schema -ne "sura.version.v1" -or [string]$versionContract.version -notmatch '^\d+\.\d+\.\d+$') {
    throw "version.json does not satisfy sura.version.v1"
}
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = [string]$versionContract.version
} elseif ($Version -ne [string]$versionContract.version) {
    throw "installer smoke version $Version does not match version.json $($versionContract.version)"
}
$makerPath = (Resolve-Path -LiteralPath $Maker).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_installer_" + [System.Guid]::NewGuid().ToString("N"))
$ps = Get-PowerShellRunner

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $outDir = Join-Path $temp "kit"
    $zip = Join-Path $temp "sura-windows-x64.zip"
    $standalone = Join-Path $temp "SuraSetup-Windows-x64.exe"
    $reportPath = Join-Path $temp "installer-report.json"

    & $ps -NoProfile -ExecutionPolicy Bypass -File $makerPath -RepoRoot $root -OutDir $outDir -Version $Version -ZipPath $zip -StandaloneExePath $standalone -JsonOut $reportPath | Out-Host
    if ($LASTEXITCODE -ne 0) {
        throw "sura_make_installer failed with exit code $LASTEXITCODE"
    }
    foreach ($path in @(
        (Join-Path $outDir "SuraSetup.exe"),
        (Join-Path $outDir "support/SuraSetup.ps1"),
        (Join-Path $outDir "support/SuraSetup-GUI.ps1"),
        (Join-Path $outDir "support/SuraSetup.cs"),
        (Join-Path $outDir "support/SuraStandaloneSetup.cs"),
        (Join-Path $outDir "support/SuraSetup.cmd"),
        (Join-Path $outDir "support/SuraLogo.png"),
        (Join-Path $outDir "support/SuraLogo.ico"),
        (Join-Path $outDir "README_INSTALL.md"),
        (Join-Path $outDir "installer-manifest.json"),
        (Join-Path $outDir "payload/SuraLanguage.exe"),
        (Join-Path $outDir "payload/surapkg.exe"),
        (Join-Path $outDir "payload/examples/starter/01_hello.sura"),
        $standalone,
        $zip,
        $reportPath
    )) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "installer smoke expected file: $path"
        }
    }
    foreach ($portableExe in @(
        (Join-Path $outDir "payload/SuraLanguage.exe"),
        (Join-Path $outDir "payload/surapkg.exe")
    )) {
        Assert-No-MinGWRuntimeImports $portableExe
    }
    if (Test-Path -LiteralPath (Join-Path $outDir "payload/SuraFinal.exe")) {
        throw "installer payload still contains the removed SuraFinal.exe alias"
    }

    $report = [System.IO.File]::ReadAllText($reportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($report.schema -ne "sura.installer.pack.v1" -or
        $report.product_name -ne "Sura Language" -or
        $report.version -ne $Version -or
        $report.zip_created -ne $true -or
        [string]::IsNullOrWhiteSpace([string]$report.gui_setup_script) -or
        [string]::IsNullOrWhiteSpace([string]$report.gui_setup_source) -or
        [string]::IsNullOrWhiteSpace([string]$report.gui_setup_exe) -or
        [string]::IsNullOrWhiteSpace([string]$report.gui_launcher) -or
        [string]::IsNullOrWhiteSpace([string]$report.branding_logo) -or
        [string]::IsNullOrWhiteSpace([string]$report.branding_icon) -or
        $report.branding_logo_aspect_ratio -ne "1:1" -or
        [string]::IsNullOrWhiteSpace([string]$report.single_file_installer) -or
        $report.single_file_installer_created -ne $true -or
        [int64]$report.single_file_installer_size_bytes -le [int64]$report.payload_size_bytes -or
        -not ($report.user_commands -contains "sura") -or
        -not ($report.user_commands -contains "surapkg") -or
        -not ($report.payload -contains "examples/")) {
        $report | ConvertTo-Json -Depth 6
        throw "unexpected installer pack report"
    }
    foreach ($rootHelper in @("SuraSetup.ps1", "SuraSetup-GUI.ps1", "SuraSetup.cmd", "SuraSetup.cs")) {
        if (Test-Path -LiteralPath (Join-Path $outDir $rootHelper)) {
            throw "only SuraSetup.exe should be a root installer entry point; found $rootHelper"
        }
    }
    if ([int64]$report.payload_size_bytes -le 0 -or [int64]$report.estimated_install_size_bytes -lt [int64]$report.payload_size_bytes) {
        $report | ConvertTo-Json -Depth 6
        throw "installer report should include payload and estimated install size"
    }
    if ($report.gui_setup_exe_created -eq $true -and -not (Test-Path -LiteralPath ([string]$report.gui_setup_exe))) {
        throw "installer report says SuraSetup.exe was created but it is missing"
    }
    if (-not (Test-Path -LiteralPath ([string]$report.single_file_installer))) {
        throw "installer report says SuraSetup-Windows-x64.exe was created but it is missing"
    }

    $manifest = [System.IO.File]::ReadAllText((Join-Path $outDir "installer-manifest.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($manifest.schema -ne "sura.installer.manifest.v1" -or
        $manifest.product_name -ne "Sura Language" -or
        $manifest.version -ne $Version -or
        $manifest.engine_version -ne $Version -or
        $manifest.version -ne $report.version -or
        -not ($manifest.installer_modes -contains "single-exe") -or
        -not ($manifest.installer_modes -contains "gui-exe") -or
        -not ($manifest.installer_modes -contains "gui-powershell") -or
        -not ($manifest.installer_modes -contains "cli") -or
        $manifest.single_file_installer -ne "SuraSetup-Windows-x64.exe" -or
        $manifest.branding.logo -ne "support/SuraLogo.png" -or
        $manifest.branding.icon -ne "support/SuraLogo.ico" -or
        $manifest.branding.logo_aspect_ratio -ne "1:1" -or
        -not ($manifest.normal_use_dependencies -contains "Windows x64") -or
        -not ($manifest.installer_dependencies -contains ".NET Framework CLR") -or
        -not ($manifest.installer_dependencies -contains "Windows PowerShell or PowerShell") -or
        -not ($manifest.optional_runtime_dependencies -contains "FFmpeg for media video decoding") -or
        [int64]$manifest.payload_size_bytes -le 0 -or
        [int64]$manifest.estimated_install_size_bytes -lt [int64]$manifest.payload_size_bytes -or
        $manifest.default_install_dir -notmatch "LOCALAPPDATA") {
        $manifest | ConvertTo-Json -Depth 6
        throw "unexpected installer manifest"
    }

    Add-Type -AssemblyName System.Drawing
    $logoImage = $null
    try {
        $logoImage = [System.Drawing.Image]::FromFile((Join-Path $outDir "support/SuraLogo.png"))
        if ($logoImage.Width -ne $logoImage.Height) {
            throw "installer logo should be square 1:1; got $($logoImage.Width)x$($logoImage.Height)"
        }
    } finally {
        if ($null -ne $logoImage) {
            $logoImage.Dispose()
        }
    }

    $guiPath = Join-Path $outDir "support/SuraSetup-GUI.ps1"
    $tokens = $null
    $errors = $null
    $null = [System.Management.Automation.Language.Parser]::ParseFile($guiPath, [ref]$tokens, [ref]$errors)
    if ($errors.Count -gt 0) {
        $errors | ForEach-Object { Write-Output $_.Message }
        throw "GUI installer script has PowerShell parse errors"
    }
    $guiText = [System.IO.File]::ReadAllText($guiPath, [System.Text.Encoding]::UTF8)
    $setupText = [System.IO.File]::ReadAllText((Join-Path $outDir "support/SuraSetup.ps1"), [System.Text.Encoding]::UTF8)
    $guiSourceText = [System.IO.File]::ReadAllText((Join-Path $outDir "support/SuraSetup.cs"), [System.Text.Encoding]::UTF8)
    $standaloneSourceText = [System.IO.File]::ReadAllText((Join-Path $outDir "support/SuraStandaloneSetup.cs"), [System.Text.Encoding]::UTF8)
    $launcherText = [System.IO.File]::ReadAllText((Join-Path $outDir "support/SuraSetup.cmd"), [System.Text.Encoding]::ASCII)
    if ($guiText -notmatch "System\.Windows\.Forms" -or
        $guiText -notmatch "I reviewed these installation actions" -or
        $guiText -notmatch "Required space" -or
        $guiText -notmatch "Browse\.\.\." -or
        $guiText -notmatch "SuraLogo\.png" -or
        $guiText -notmatch "System\.Drawing\.Size\(112, 112\)" -or
        $guiSourceText -notmatch "FolderBrowserDialog" -or
        $guiSourceText -notmatch "I reviewed these installation actions" -or
        $guiSourceText -notmatch ([regex]::Escape(('AssemblyFileVersion("{0}")' -f $Version))) -or
        $guiSourceText -notmatch "Required space" -or
        $guiSourceText -notmatch "PictureBox" -or
        $guiSourceText -notmatch "SuraLogo\.png" -or
        $guiSourceText -notmatch "SuraLogo\.ico" -or
        $guiSourceText -notmatch "new Size\(112, 112\)" -or
        $guiSourceText -notmatch "RedirectStandardOutput = true" -or
        $guiSourceText -notmatch "BeginErrorReadLine" -or
        $guiText -notmatch "installerOutput" -or
        $setupText -notmatch "front of the user PATH" -or
        $setupText -notmatch '\$parts = @\(\$Entry\) \+ \$parts' -or
        $setupText -notmatch "Assert-FileReplaceable" -or
        $setupText -notmatch "Installation log:" -or
        $setupText -notmatch "Close Sura terminals, the Sura REPL, and VS Code windows using Sura" -or
        $standaloneSourceText -notmatch "--quiet-install" -or
        $standaloneSourceText -notmatch ([regex]::Escape(('AssemblyFileVersion("{0}")' -f $Version))) -or
        $standaloneSourceText -notmatch "GetManifestResourceStream" -or
        $standaloneSourceText -notmatch "DeleteTreeBestEffort" -or
        $launcherText -notmatch "SuraSetup\.exe" -or
        $launcherText -notmatch "SuraSetup-GUI\.ps1") {
        throw "installer should expose required space, installation actions, version metadata, path selection, single-file wrapper, exe launcher, and PowerShell fallback"
    }
    foreach ($exePath in @((Join-Path $outDir "SuraSetup.exe"), $standalone)) {
        $bytes = [System.IO.File]::ReadAllBytes($exePath)
        if ($bytes.Length -lt 2 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
            throw "$exePath is not a valid Windows executable"
        }
        $versionInfo = (Get-Item -LiteralPath $exePath).VersionInfo
        if ($versionInfo.FileVersion -ne $Version -or $versionInfo.ProductVersion -ne $Version) {
            throw "$exePath version metadata should be $Version; got file=$($versionInfo.FileVersion), product=$($versionInfo.ProductVersion)"
        }
    }

    $extractedStandalone = Join-Path $temp "single-exe-extracted"
    $extractProcess = Start-Process -FilePath $standalone -ArgumentList @("--extract-only", $extractedStandalone) -Wait -PassThru -WindowStyle Hidden
    if ($extractProcess.ExitCode -ne 0) {
        throw "SuraSetup-Windows-x64.exe extract-only failed with exit code $($extractProcess.ExitCode)"
    }
    foreach ($path in @(
        (Join-Path $extractedStandalone "SuraSetup.exe"),
        (Join-Path $extractedStandalone "support/SuraSetup.ps1"),
        (Join-Path $extractedStandalone "support/SuraLogo.png"),
        (Join-Path $extractedStandalone "support/SuraLogo.ico"),
        (Join-Path $extractedStandalone "payload/SuraLanguage.exe"),
        (Join-Path $extractedStandalone "payload/surapkg.exe"),
        (Join-Path $extractedStandalone "payload/examples/starter/01_hello.sura")
    )) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "single-file installer extraction expected file: $path"
        }
    }

    $installDir = Join-Path $temp "installed"
    $legacyInstalledEngines = @(
        (Join-Path $installDir "bin/SuraFinal.exe"),
        (Join-Path $installDir "bin/SuraEngine.exe")
    )
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $legacyInstalledEngines[0]) | Out-Null
    foreach ($legacyInstalledEngine in $legacyInstalledEngines) {
        [System.IO.File]::WriteAllText($legacyInstalledEngine, "legacy-engine", [System.Text.Encoding]::ASCII)
    }
    & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $outDir "support/SuraSetup.ps1") -InstallDir $installDir -NoPath -Quiet
    if ($LASTEXITCODE -ne 0) {
        throw "SuraSetup.ps1 install failed with exit code $LASTEXITCODE"
    }
    foreach ($legacyInstalledEngine in $legacyInstalledEngines) {
        if (Test-Path -LiteralPath $legacyInstalledEngine) {
            throw "installer upgrade left a legacy engine alias in place: $legacyInstalledEngine"
        }
    }

    $suraCmd = Join-Path $installDir "bin/sura.cmd"
    $surapkgCmd = Join-Path $installDir "bin/surapkg.cmd"
    foreach ($path in @(
        (Join-Path $installDir "bin/SuraLanguage.exe"),
        (Join-Path $installDir "bin/surapkg.exe"),
        $suraCmd,
        $surapkgCmd,
        (Join-Path $installDir "examples/starter/01_hello.sura"),
        (Join-Path $installDir "installation.json")
    )) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "installer smoke expected installed file: $path"
        }
    }

    $installedEngine = Join-Path $installDir "bin/SuraLanguage.exe"
    $engineHashBeforeLockTest = (Get-FileHash -LiteralPath $installedEngine -Algorithm SHA256).Hash
    $lockStream = $null
    try {
        $lockStream = [System.IO.File]::Open(
            $installedEngine,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read
        )
        $lockedStdout = Join-Path $temp "locked-install.stdout.txt"
        $lockedStderr = Join-Path $temp "locked-install.stderr.txt"
        $lockedProcess = Start-Process -FilePath $ps -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", (Join-Path $outDir "support/SuraSetup.ps1"),
            "-InstallDir", $installDir,
            "-NoPath",
            "-Quiet"
        ) -RedirectStandardOutput $lockedStdout -RedirectStandardError $lockedStderr -Wait -PassThru -WindowStyle Hidden
        $lockedInstallExitCode = $lockedProcess.ExitCode
        $lockedInstallOut = @()
        if (Test-Path -LiteralPath $lockedStdout) { $lockedInstallOut += [System.IO.File]::ReadAllText($lockedStdout, [System.Text.Encoding]::UTF8) }
        if (Test-Path -LiteralPath $lockedStderr) { $lockedInstallOut += [System.IO.File]::ReadAllText($lockedStderr, [System.Text.Encoding]::UTF8) }
    } finally {
        if ($null -ne $lockStream) { $lockStream.Dispose() }
    }
    $lockedInstallText = $lockedInstallOut -join "`n"
    if ($lockedInstallExitCode -eq 0 -or
        $lockedInstallText -notmatch "is in use and cannot be updated" -or
        $lockedInstallText -notmatch "Close Sura terminals, the Sura REPL, and VS Code windows using Sura" -or
        $lockedInstallText -notmatch "Installation log:") {
        Write-Output $lockedInstallText
        throw "installer should report a locked engine with an actionable message and log path"
    }
    $engineHashAfterLockTest = (Get-FileHash -LiteralPath $installedEngine -Algorithm SHA256).Hash
    if ($engineHashAfterLockTest -ne $engineHashBeforeLockTest) {
        throw "locked upgrade changed the installed engine before reporting failure"
    }

    $program = Join-Path $temp "hello.sura"
    [System.IO.File]::WriteAllText($program, "print `"install-ok: 설치`"`n", [System.Text.Encoding]::UTF8)
    $runOut = & $suraCmd $program 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0 -or (($runOut -join "`n") -notmatch "install-ok")) {
        Write-Output ($runOut -join "`n")
        throw "installed sura command did not run a UTF-8 script"
    }

    $helpOut = & $surapkgCmd 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0 -or (($helpOut -join "`n") -notmatch "Sura package manager")) {
        Write-Output (($helpOut | Select-Object -First 12) -join "`n")
        throw "installed surapkg command did not run"
    }

    $examplesOut = & $surapkgCmd examples --json 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        Write-Output ($examplesOut -join "`n")
        throw "installed surapkg could not discover the bundled examples"
    }
    $examplesReport = ($examplesOut -join "`n") | ConvertFrom-Json
    if ($examplesReport.schema -ne "sura.package.examples.v1" -or
        [int]$examplesReport.total_count -lt 40 -or
        -not ($examplesReport.examples | Where-Object { $_.id -eq "starter/01_hello" })) {
        $examplesReport | ConvertTo-Json -Depth 6
        throw "installed example gallery report was incomplete"
    }

    $installInfo = [System.IO.File]::ReadAllText((Join-Path $installDir "installation.json"), [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($installInfo.schema -ne "sura.installation.v1" -or
        $installInfo.product_name -ne "Sura Language" -or
        $installInfo.version -ne $Version -or
        $installInfo.engine -ne "SuraLanguage.exe" -or
        $installInfo.examples_dir -ne (Join-Path $installDir "examples") -or
        $installInfo.path_added -ne $false -or
        -not ($installInfo.commands -contains "sura")) {
        $installInfo | ConvertTo-Json -Depth 6
        throw "unexpected installation.json"
    }

    $standaloneInstallDir = Join-Path $temp "standalone-installed"
    $standaloneInstallProcess = Start-Process -FilePath $standalone -ArgumentList @("--quiet-install", $standaloneInstallDir) -Wait -PassThru -WindowStyle Hidden
    if ($standaloneInstallProcess.ExitCode -ne 0) {
        throw "SuraSetup-Windows-x64.exe quiet install failed with exit code $($standaloneInstallProcess.ExitCode)"
    }
    foreach ($path in @(
        (Join-Path $standaloneInstallDir "bin/SuraLanguage.exe"),
        (Join-Path $standaloneInstallDir "bin/surapkg.exe"),
        (Join-Path $standaloneInstallDir "bin/sura.cmd"),
        (Join-Path $standaloneInstallDir "examples/starter/01_hello.sura"),
        (Join-Path $standaloneInstallDir "installation.json")
    )) {
        if (-not (Test-Path -LiteralPath $path)) {
            throw "single-file installer expected installed file: $path"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $standaloneInstallDir "bin/SuraFinal.exe")) {
        throw "single-file installer recreated the removed SuraFinal.exe alias"
    }

    & $ps -NoProfile -ExecutionPolicy Bypass -File (Join-Path $outDir "support/SuraSetup.ps1") -InstallDir $installDir -NoPath -Uninstall -Quiet
    if ($LASTEXITCODE -ne 0) {
        throw "SuraSetup.ps1 uninstall failed with exit code $LASTEXITCODE"
    }
    if (Test-Path -LiteralPath $installDir) {
        throw "installer uninstall did not remove install directory"
    }

    "sura_installer_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
