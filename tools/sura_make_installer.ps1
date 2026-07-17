param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$OutDir = "",
    [string]$Version = "",
    [string]$ZipPath = "",
    [string]$JsonOut = "",
    [string]$StandaloneExePath = "",
    [string]$LogoPath = "",
    [switch]$SkipStandalone,
    [switch]$SkipZip
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Resolve-RepoPath {
    param([string]$Path)
    return (Resolve-Path -LiteralPath $Path).Path
}

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Copy-RequiredFile {
    param([string]$Source, [string]$Destination)
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "required installer payload file not found: $Source"
    }
    $parent = Split-Path -Parent $Destination
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Write-SquareLogoPng {
    param([string]$Source, [string]$Destination)
    if (-not (Test-Path -LiteralPath $Source)) {
        throw "required installer logo file not found: $Source"
    }
    $parent = Split-Path -Parent $Destination
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }

    Add-Type -AssemblyName System.Drawing
    $sourceImage = $null
    $bitmap = $null
    $graphics = $null
    try {
        $sourceImage = [System.Drawing.Image]::FromFile($Source)
        $side = [Math]::Max($sourceImage.Width, $sourceImage.Height)
        $bitmap = New-Object System.Drawing.Bitmap($side, $side, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.Clear([System.Drawing.Color]::Transparent)
        $graphics.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
        $graphics.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
        $graphics.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
        $x = [int](($side - $sourceImage.Width) / 2)
        $y = [int](($side - $sourceImage.Height) / 2)
        $graphics.DrawImage($sourceImage, $x, $y, $sourceImage.Width, $sourceImage.Height)
        $bitmap.Save($Destination, [System.Drawing.Imaging.ImageFormat]::Png)
    } finally {
        if ($null -ne $graphics) { $graphics.Dispose() }
        if ($null -ne $bitmap) { $bitmap.Dispose() }
        if ($null -ne $sourceImage) { $sourceImage.Dispose() }
    }
}

function Get-DirectoryByteSize {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return 0L }
    $total = 0L
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -ErrorAction SilentlyContinue) {
        $total += [int64]$file.Length
    }
    return $total
}

function Find-CSharpCompiler {
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($env:WINDIR)) {
        $candidates += (Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe")
        $candidates += (Join-Path $env:WINDIR "Microsoft.NET\Framework\v4.0.30319\csc.exe")
    }
    $cmd = Get-Command csc -ErrorAction SilentlyContinue
    if ($cmd) {
        $candidates += $cmd.Source
    }
    foreach ($candidate in $candidates) {
        if (-not [string]::IsNullOrWhiteSpace($candidate) -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return ""
}

function Build-GuiSetupExe {
    param([string]$SourcePath, [string]$ExePath, [string]$IconPath = "")
    $compiler = Find-CSharpCompiler
    if ([string]::IsNullOrWhiteSpace($compiler)) {
        return [ordered]@{
            created = $false
            compiler = ""
        }
    }

    $args = @(
        "/nologo",
        "/target:winexe",
        "/platform:anycpu",
        "/reference:System.Windows.Forms.dll",
        "/reference:System.Drawing.dll",
        "/out:$ExePath",
        $SourcePath
    )
    if (-not [string]::IsNullOrWhiteSpace($IconPath) -and (Test-Path -LiteralPath $IconPath)) {
        $args += "/win32icon:$IconPath"
    }
    $output = & $compiler @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Output $_ }
        throw "failed to compile SuraSetup.exe with $compiler"
    }
    return [ordered]@{
        created = (Test-Path -LiteralPath $ExePath)
        compiler = $compiler
    }
}

function Escape-CSharpString {
    param([string]$Text)
    if ($null -eq $Text) { return "" }
    return $Text.Replace("\", "\\").Replace('"', '\"').Replace("`r", "\r").Replace("`n", "\n")
}

function Build-StandaloneSetupExe {
    param(
        [string]$SourcePath,
        [string]$ExePath,
        [string]$KitRoot,
        [string]$IconPath = ""
    )

    $compiler = Find-CSharpCompiler
    if ([string]::IsNullOrWhiteSpace($compiler)) {
        return [ordered]@{
            created = $false
            compiler = ""
            size_bytes = 0L
        }
    }

    $kitRootFull = [System.IO.Path]::GetFullPath($KitRoot).TrimEnd('\', '/')
    $exeFull = [System.IO.Path]::GetFullPath($ExePath)
    $sourceFull = [System.IO.Path]::GetFullPath($SourcePath)
    $embeddedFiles = @(
        Get-ChildItem -LiteralPath $kitRootFull -Recurse -File |
            Where-Object {
                $full = [System.IO.Path]::GetFullPath($_.FullName)
                -not [string]::Equals($full, $exeFull, [System.StringComparison]::OrdinalIgnoreCase) -and
                -not [string]::Equals($full, $sourceFull, [System.StringComparison]::OrdinalIgnoreCase)
            } |
            Sort-Object FullName
    )
    if ($embeddedFiles.Count -eq 0) {
        throw "standalone installer has no files to embed: $kitRootFull"
    }

    $resourceArgs = @()
    $entryLines = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $embeddedFiles.Count; $i++) {
        $file = $embeddedFiles[$i]
        $relative = [System.IO.Path]::GetFullPath($file.FullName).Substring($kitRootFull.Length).TrimStart('\', '/')
        $relative = $relative.Replace('\', '/')
        $resourceName = "sura_installer_payload_$i"
        $resourceArgs += ("/resource:{0},{1}" -f $file.FullName, $resourceName)
        $entryLines.Add(('        new EmbeddedFile("{0}", "{1}")' -f (Escape-CSharpString $resourceName), (Escape-CSharpString $relative)))
    }

    $entries = [string]::Join(",`r`n", $entryLines.ToArray())
    $source = @"
using System;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Windows.Forms;

[assembly: AssemblyVersion("$Version")]
[assembly: AssemblyFileVersion("$Version")]
[assembly: AssemblyInformationalVersion("$Version")]

internal sealed class EmbeddedFile
{
    public readonly string ResourceName;
    public readonly string RelativePath;

    public EmbeddedFile(string resourceName, string relativePath)
    {
        ResourceName = resourceName;
        RelativePath = relativePath;
    }
}

internal static class Program
{
    private static readonly EmbeddedFile[] Files = new EmbeddedFile[]
    {
$entries
    };

    [STAThread]
    private static void Main(string[] args)
    {
        try
        {
            Environment.Exit(Run(args));
        }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "Sura Language Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            Environment.Exit(1);
        }
    }

    private static int Run(string[] args)
    {
        string extractOnly = GetArgValue(args, "--extract-only");
        if (!String.IsNullOrEmpty(extractOnly))
        {
            ExtractTo(Path.GetFullPath(extractOnly));
            return 0;
        }

        string quietInstall = GetArgValue(args, "--quiet-install");
        if (!String.IsNullOrEmpty(quietInstall))
        {
            string tempInstall = CreateTempDir();
            try
            {
                ExtractTo(tempInstall);
                return RunQuietInstall(tempInstall, Path.GetFullPath(quietInstall));
            }
            finally
            {
                DeleteTreeBestEffort(tempInstall);
            }
        }

        string temp = CreateTempDir();
        try
        {
            ExtractTo(temp);
            string setupExe = Path.Combine(temp, "SuraSetup.exe");
            if (!File.Exists(setupExe))
            {
                throw new InvalidOperationException("Embedded SuraSetup.exe was not found.");
            }

            ProcessStartInfo psi = new ProcessStartInfo(setupExe);
            psi.WorkingDirectory = temp;
            psi.UseShellExecute = true;
            psi.Arguments = JoinArguments(args);
            Process process = Process.Start(psi);
            process.WaitForExit();
            return process.ExitCode;
        }
        finally
        {
            DeleteTreeBestEffort(temp);
        }
    }

    private static string CreateTempDir()
    {
        string path = Path.Combine(Path.GetTempPath(), "sura_setup_" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(path);
        return path;
    }

    private static void ExtractTo(string directory)
    {
        Directory.CreateDirectory(directory);
        Assembly assembly = Assembly.GetExecutingAssembly();
        foreach (EmbeddedFile file in Files)
        {
            string outputPath = Path.Combine(directory, file.RelativePath.Replace('/', Path.DirectorySeparatorChar));
            string parent = Path.GetDirectoryName(outputPath);
            if (!String.IsNullOrEmpty(parent))
            {
                Directory.CreateDirectory(parent);
            }

            using (Stream input = assembly.GetManifestResourceStream(file.ResourceName))
            {
                if (input == null)
                {
                    throw new InvalidOperationException("Missing embedded installer resource: " + file.ResourceName);
                }
                using (FileStream output = new FileStream(outputPath, FileMode.Create, FileAccess.Write))
                {
                    input.CopyTo(output);
                }
            }
        }
    }

    private static int RunQuietInstall(string extractedRoot, string installDir)
    {
        string script = Path.Combine(Path.Combine(extractedRoot, "support"), "SuraSetup.ps1");
        if (!File.Exists(script))
        {
            throw new InvalidOperationException("Embedded SuraSetup.ps1 was not found.");
        }

        ProcessStartInfo psi = new ProcessStartInfo(PowerShellPath());
        psi.WorkingDirectory = extractedRoot;
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File " + Quote(script) + " -InstallDir " + Quote(installDir) + " -NoPath -Quiet";
        Process process = Process.Start(psi);
        process.WaitForExit();
        return process.ExitCode;
    }

    private static string GetArgValue(string[] args, string name)
    {
        for (int i = 0; i < args.Length; i++)
        {
            if (String.Equals(args[i], name, StringComparison.OrdinalIgnoreCase))
            {
                if (i + 1 >= args.Length)
                {
                    throw new ArgumentException(name + " requires a path.");
                }
                return args[i + 1];
            }
            if (args[i].StartsWith(name + "=", StringComparison.OrdinalIgnoreCase))
            {
                return args[i].Substring(name.Length + 1);
            }
        }
        return "";
    }

    private static string PowerShellPath()
    {
        string ps = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell\\v1.0\\powershell.exe");
        if (File.Exists(ps))
        {
            return ps;
        }
        return "powershell.exe";
    }

    private static string JoinArguments(string[] args)
    {
        string[] quoted = new string[args.Length];
        for (int i = 0; i < args.Length; i++)
        {
            quoted[i] = Quote(args[i]);
        }
        return String.Join(" ", quoted);
    }

    private static string Quote(string value)
    {
        if (value == null)
        {
            value = "";
        }
        return "\"" + value.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
    }

    private static void DeleteTreeBestEffort(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, true);
            }
        }
        catch
        {
        }
    }
}
"@

    Write-Text $SourcePath $source
    $parent = Split-Path -Parent $ExePath
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    if (Test-Path -LiteralPath $ExePath) {
        Remove-Item -LiteralPath $ExePath -Force
    }

    $args = @(
        "/nologo",
        "/target:winexe",
        "/platform:anycpu",
        "/reference:System.Windows.Forms.dll",
        "/out:$ExePath",
        $SourcePath
    ) + $resourceArgs
    if (-not [string]::IsNullOrWhiteSpace($IconPath) -and (Test-Path -LiteralPath $IconPath)) {
        $args += "/win32icon:$IconPath"
    }
    $output = & $compiler @args 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Output $_ }
        throw "failed to compile standalone SuraSetup-Windows-x64.exe with $compiler"
    }

    $size = 0L
    if (Test-Path -LiteralPath $ExePath) {
        $size = [int64](Get-Item -LiteralPath $ExePath).Length
    }
    return [ordered]@{
        created = (Test-Path -LiteralPath $ExePath)
        compiler = $compiler
        size_bytes = $size
    }
}

$root = Resolve-RepoPath $RepoRoot
if ([string]::IsNullOrWhiteSpace($Version)) {
    $versionPath = Join-Path $root "version.json"
    if (-not (Test-Path -LiteralPath $versionPath)) {
        throw "version contract was not found: $versionPath"
    }
    $versionContract = [System.IO.File]::ReadAllText($versionPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($versionContract.schema -ne "sura.version.v1" -or [string]$versionContract.version -notmatch '^\d+\.\d+\.\d+$') {
        throw "version.json does not satisfy sura.version.v1"
    }
    $Version = [string]$versionContract.version
}
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $root "dist/SuraLanguage-windows-x64"
}
if ([string]::IsNullOrWhiteSpace($ZipPath)) {
    $ZipPath = Join-Path (Split-Path -Parent $OutDir) "SuraLanguage-$Version-windows-x64.zip"
}

$engine = Join-Path $root "SuraLanguage.exe"
$surapkg = Join-Path $root "surapkg.exe"
$examples = Join-Path $root "examples"
if (-not (Test-Path -LiteralPath $engine)) {
    throw "Sura Language engine was not found: $engine"
}
if (-not (Test-Path -LiteralPath $examples -PathType Container)) {
    throw "Sura example gallery was not found: $examples"
}
$engineVersionText = (& $engine --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $engineVersionText -notmatch '^Sura Language ([0-9]+\.[0-9]+\.[0-9]+)$') {
    throw "could not read a semantic version from the Sura Language engine: $engineVersionText"
}
$engineVersion = $Matches[1]
if ($Version -ne "dev" -and $Version -ne $engineVersion) {
    throw "installer version $Version does not match engine version $engineVersion; rebuild the engine or pass the matching -Version"
}
$outRoot = [System.IO.Path]::GetFullPath($OutDir)
if ([string]::IsNullOrWhiteSpace($StandaloneExePath)) {
    $StandaloneExePath = Join-Path (Split-Path -Parent $outRoot) "SuraLanguageSetup-$Version.exe"
}
$standaloneExeFull = if ($SkipStandalone) { "" } else { [System.IO.Path]::GetFullPath($StandaloneExePath) }
$standaloneFileName = if ($SkipStandalone) {
    "SuraLanguageSetup-$Version.exe"
} else {
    [System.IO.Path]::GetFileName($StandaloneExePath)
}
$payload = Join-Path $outRoot "payload"
$support = Join-Path $outRoot "support"
$defaultLogoPath = Join-Path $root "assets/sura-logo.png"
if ([string]::IsNullOrWhiteSpace($LogoPath)) {
    $LogoPath = $defaultLogoPath
}
$resolvedLogoPath = if (Test-Path -LiteralPath $LogoPath) { (Resolve-Path -LiteralPath $LogoPath).Path } else { "" }
$resolvedIconPath = ""
if (-not [string]::IsNullOrWhiteSpace($resolvedLogoPath)) {
    $candidateIcon = [System.IO.Path]::ChangeExtension($resolvedLogoPath, ".ico")
    if (Test-Path -LiteralPath $candidateIcon) {
        $resolvedIconPath = (Resolve-Path -LiteralPath $candidateIcon).Path
    }
}

$outRootBoundary = $outRoot.TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
$payloadFull = [System.IO.Path]::GetFullPath($payload)
if (-not $payloadFull.StartsWith($outRootBoundary, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "refusing to clean installer payload outside output directory: $payloadFull"
}
if (Test-Path -LiteralPath $payloadFull) {
    Remove-Item -LiteralPath $payloadFull -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $payloadFull | Out-Null
if (Test-Path -LiteralPath $support) {
    Remove-Item -LiteralPath $support -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $support | Out-Null
foreach ($oldRootHelper in @("SuraSetup.ps1", "SuraSetup-GUI.ps1", "SuraSetup.cmd", "SuraSetup.cs")) {
    $oldPath = Join-Path $outRoot $oldRootHelper
    if (Test-Path -LiteralPath $oldPath) {
        Remove-Item -LiteralPath $oldPath -Force
    }
}
Copy-RequiredFile $engine (Join-Path $payload "SuraLanguage.exe")
Copy-RequiredFile $surapkg (Join-Path $payload "surapkg.exe")
Copy-Item -LiteralPath $examples -Destination (Join-Path $payload "examples") -Recurse -Force
if (-not [string]::IsNullOrWhiteSpace($resolvedLogoPath)) {
    Write-SquareLogoPng $resolvedLogoPath (Join-Path $support "SuraLogo.png")
}
if (-not [string]::IsNullOrWhiteSpace($resolvedIconPath)) {
    Copy-RequiredFile $resolvedIconPath (Join-Path $support "SuraLogo.ico")
}
$payloadBytes = Get-DirectoryByteSize $payload
$estimatedInstallBytes = $payloadBytes + 1048576

$setupScript = @'
param(
    [string]$InstallDir = "",
    [switch]$NoPath,
    [switch]$Uninstall,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:InstallLogPath = ""

function Initialize-InstallLog {
    $base = if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        Join-Path $env:LOCALAPPDATA "Sura\Logs"
    } else {
        Join-Path ([System.IO.Path]::GetTempPath()) "Sura\Logs"
    }
    New-Item -ItemType Directory -Force -Path $base | Out-Null
    $script:InstallLogPath = Join-Path $base ("install-{0}-{1}.log" -f [DateTime]::Now.ToString("yyyyMMdd-HHmmss"), $PID)
    [System.IO.File]::WriteAllText(
        $script:InstallLogPath,
        ("Sura Language installer {0}`r`nStarted: {1}`r`n" -f "__SURA_LANGUAGE_VERSION__", [DateTime]::Now.ToString("o")),
        $utf8NoBom
    )
}

function Write-InstallLog {
    param([string]$Message)
    if ([string]::IsNullOrWhiteSpace($script:InstallLogPath)) { return }
    Add-Content -LiteralPath $script:InstallLogPath -Encoding UTF8 -Value ("[{0}] {1}" -f [DateTime]::Now.ToString("o"), $Message)
}

trap {
    $message = $_.Exception.Message
    Write-InstallLog ("FAILED: {0}`r`n{1}" -f $message, ($_ | Out-String).Trim())
    [Console]::Error.WriteLine("Sura Language installation failed: $message")
    if (-not [string]::IsNullOrWhiteSpace($script:InstallLogPath)) {
        [Console]::Error.WriteLine("Installation log: $script:InstallLogPath")
    }
    exit 1
}

function Write-Step {
    param([string]$Message)
    Write-InstallLog $Message
    if (-not $Quiet) { Write-Host $Message }
}

function Assert-FileReplaceable {
    param([string]$Path, [string]$DisplayName)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return }

    $lastError = ""
    for ($attempt = 0; $attempt -lt 12; $attempt++) {
        $stream = $null
        try {
            $stream = [System.IO.File]::Open(
                $Path,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None
            )
            return
        } catch {
            $lastError = $_.Exception.Message
            if ($attempt -lt 11) { Start-Sleep -Milliseconds 250 }
        } finally {
            if ($null -ne $stream) { $stream.Dispose() }
        }
    }

    $processText = ""
    try {
        $fullPath = [System.IO.Path]::GetFullPath($Path)
        $matches = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
            try {
                -not [string]::IsNullOrWhiteSpace($_.Path) -and
                    [string]::Equals([System.IO.Path]::GetFullPath($_.Path), $fullPath, [System.StringComparison]::OrdinalIgnoreCase)
            } catch {
                $false
            }
        } | ForEach-Object { "{0} (PID {1})" -f $_.ProcessName, $_.Id })
        if ($matches.Count -gt 0) {
            $processText = " Running: " + ($matches -join ", ") + "."
        }
    } catch {
        $processText = ""
    }

    Write-InstallLog ("Locked file: {0}; detail: {1}" -f $Path, $lastError)
    throw "$DisplayName is in use and cannot be updated.$processText Close Sura terminals, the Sura REPL, and VS Code windows using Sura, then retry the installer."
}

function Normalize-PathText {
    param([string]$Path)
    if ([string]::IsNullOrWhiteSpace($Path)) { return "" }
    return ([System.IO.Path]::GetFullPath($Path)).TrimEnd('\', '/')
}

function Get-DefaultInstallDir {
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        return (Join-Path $env:LOCALAPPDATA "Programs\Sura")
    }
    return (Join-Path $HOME ".sura")
}

function Get-DirectoryByteSize {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return 0L }
    $total = 0L
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -ErrorAction SilentlyContinue) {
        $total += [int64]$file.Length
    }
    return $total
}

function Format-ByteSize {
    param([int64]$Bytes)
    if ($Bytes -ge 1GB) { return ("{0:N1} GB" -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ("{0:N1} MB" -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ("{0:N1} KB" -f ($Bytes / 1KB)) }
    return ("{0} bytes" -f $Bytes)
}

function Get-UserPathParts {
    $raw = [Environment]::GetEnvironmentVariable("Path", "User")
    if ([string]::IsNullOrWhiteSpace($raw)) { return @() }
    return @($raw -split ";" | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
}

function Set-UserPathParts {
    param([string[]]$Parts)
    [Environment]::SetEnvironmentVariable("Path", ($Parts -join ";"), "User")
}

function Add-UserPathEntry {
    param([string]$Entry)
    $want = Normalize-PathText $Entry
    $parts = @(Get-UserPathParts | Where-Object {
        -not [string]::Equals((Normalize-PathText $_), $want, [System.StringComparison]::OrdinalIgnoreCase)
    })
    $parts = @($Entry) + $parts
    Set-UserPathParts $parts
}

function Remove-UserPathEntry {
    param([string]$Entry)
    $want = Normalize-PathText $Entry
    $parts = @(Get-UserPathParts | Where-Object {
        -not [string]::Equals((Normalize-PathText $_), $want, [System.StringComparison]::OrdinalIgnoreCase)
    })
    Set-UserPathParts $parts
}

function Write-CmdLauncher {
    param([string]$Path, [string]$Target)
    $content = "@echo off`r`n`"%~dp0$Target`" %*`r`n"
    [System.IO.File]::WriteAllText($Path, $content, [System.Text.Encoding]::ASCII)
}

function Resolve-PayloadDir {
    $sourceRoot = if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
        $PSScriptRoot
    } else {
        Split-Path -Parent $PSCommandPath
    }
    foreach ($candidate in @(
        (Join-Path $sourceRoot "payload"),
        (Join-Path (Split-Path -Parent $sourceRoot) "payload")
    )) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return (Join-Path $sourceRoot "payload")
}

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = Get-DefaultInstallDir
}
Initialize-InstallLog
$payloadBytes = Get-DirectoryByteSize (Join-Path $PSScriptRoot "payload")
$estimatedInstallBytes = $payloadBytes + 1MB
$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)
$binDir = Join-Path $InstallDir "bin"

if ($Uninstall) {
    if (-not $NoPath) {
        Remove-UserPathEntry $binDir
    }
    if (Test-Path -LiteralPath $InstallDir) {
        Remove-Item -LiteralPath $InstallDir -Recurse -Force
    }
    Write-Step "Sura Language uninstalled from $InstallDir"
    return
}

$payload = Resolve-PayloadDir
$engineSource = Join-Path $payload "SuraLanguage.exe"
$pkgSource = Join-Path $payload "surapkg.exe"
$examplesSource = Join-Path $payload "examples"
if (-not (Test-Path -LiteralPath $engineSource)) {
    throw "installer payload is missing the Sura Language engine"
}

New-Item -ItemType Directory -Force -Path $binDir | Out-Null
$engineTarget = Join-Path $binDir "SuraLanguage.exe"
$pkgTarget = Join-Path $binDir "surapkg.exe"
Assert-FileReplaceable $engineTarget "The installed Sura Language engine"
Assert-FileReplaceable $pkgTarget "The installed Sura package manager"
foreach ($legacyInstalledName in @("SuraFinal.exe", "SuraEngine.exe")) {
    $legacyInstalledEngine = Join-Path $binDir $legacyInstalledName
    if (Test-Path -LiteralPath $legacyInstalledEngine -PathType Leaf) {
        Assert-FileReplaceable $legacyInstalledEngine "The legacy Sura engine"
        Remove-Item -LiteralPath $legacyInstalledEngine -Force
    }
}
Copy-Item -LiteralPath $engineSource -Destination $engineTarget -Force
if (Test-Path -LiteralPath $pkgSource) {
    Copy-Item -LiteralPath $pkgSource -Destination $pkgTarget -Force
}
if (Test-Path -LiteralPath $examplesSource -PathType Container) {
    $examplesTarget = Join-Path $InstallDir "examples"
    $installBoundary = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
    $examplesTargetFull = [System.IO.Path]::GetFullPath($examplesTarget)
    if (-not $examplesTargetFull.StartsWith($installBoundary, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "refusing to replace an examples directory outside the Sura installation: $examplesTargetFull"
    }
    if (Test-Path -LiteralPath $examplesTargetFull) {
        Remove-Item -LiteralPath $examplesTargetFull -Recurse -Force
    }
    Copy-Item -LiteralPath $examplesSource -Destination $examplesTargetFull -Recurse -Force
}

Write-CmdLauncher (Join-Path $binDir "sura.cmd") "SuraLanguage.exe"
if (Test-Path -LiteralPath (Join-Path $binDir "surapkg.exe")) {
    Write-CmdLauncher (Join-Path $binDir "surapkg.cmd") "surapkg.exe"
}

$installInfo = [ordered]@{
    schema = "sura.installation.v1"
    product_name = "Sura Language"
    version = "__SURA_LANGUAGE_VERSION__"
    installed_utc = [DateTime]::UtcNow.ToString("o")
    install_dir = $InstallDir
    bin_dir = $binDir
    commands = @("sura", "surapkg")
    engine = "SuraLanguage.exe"
    examples_dir = (Join-Path $InstallDir "examples")
    path_added = (-not $NoPath)
}
[System.IO.File]::WriteAllText(
    (Join-Path $InstallDir "installation.json"),
    ($installInfo | ConvertTo-Json -Depth 4),
    $utf8NoBom
)

if (-not $NoPath) {
    Add-UserPathEntry $binDir
    Write-Step "Added $binDir to the front of the user PATH. Open a new terminal before running sura."
}

Write-Step "Sura Language installed to $InstallDir"
Write-Step "Run: sura --help"
'@
$setupScript = $setupScript.Replace("__SURA_LANGUAGE_VERSION__", $Version)

$guiScript = @'
param(
    [string]$InstallDir = ""
)

$ErrorActionPreference = "Stop"

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

function Get-DefaultInstallDir {
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        return (Join-Path $env:LOCALAPPDATA "Programs\Sura")
    }
    return (Join-Path $HOME ".sura")
}

function Get-PowerShellRunner {
    $pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
    if ($pwsh) { return $pwsh.Source }
    $powershell = Get-Command powershell -ErrorAction SilentlyContinue
    if ($powershell) { return $powershell.Source }
    throw "PowerShell runner not found"
}

function Get-DirectoryByteSize {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return 0L }
    $total = 0L
    foreach ($file in Get-ChildItem -LiteralPath $Path -Recurse -File -ErrorAction SilentlyContinue) {
        $total += [int64]$file.Length
    }
    return $total
}

function Format-ByteSize {
    param([int64]$Bytes)
    if ($Bytes -ge 1GB) { return ("{0:N1} GB" -f ($Bytes / 1GB)) }
    if ($Bytes -ge 1MB) { return ("{0:N1} MB" -f ($Bytes / 1MB)) }
    if ($Bytes -ge 1KB) { return ("{0:N1} KB" -f ($Bytes / 1KB)) }
    return ("{0} bytes" -f $Bytes)
}

function Resolve-PayloadDir {
    foreach ($candidate in @(
        (Join-Path $PSScriptRoot "payload"),
        (Join-Path (Split-Path -Parent $PSScriptRoot) "payload")
    )) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return (Join-Path $PSScriptRoot "payload")
}

function Resolve-SetupScript {
    foreach ($candidate in @(
        (Join-Path $PSScriptRoot "SuraSetup.ps1"),
        (Join-Path $PSScriptRoot "support\SuraSetup.ps1")
    )) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return (Join-Path $PSScriptRoot "SuraSetup.ps1")
}

function Resolve-LogoPath {
    foreach ($candidate in @(
        (Join-Path $PSScriptRoot "SuraLogo.png"),
        (Join-Path $PSScriptRoot "support\SuraLogo.png")
    )) {
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return ""
}

$setupScript = Resolve-SetupScript
if (-not (Test-Path -LiteralPath $setupScript)) {
    [System.Windows.Forms.MessageBox]::Show("SuraSetup.ps1 was not found next to this installer.", "Sura Language Setup", "OK", "Error") | Out-Null
    return
}
if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = Get-DefaultInstallDir
}
$payloadBytes = Get-DirectoryByteSize (Resolve-PayloadDir)
$estimatedInstallBytes = $payloadBytes + 1MB

$form = New-Object System.Windows.Forms.Form
$form.Text = "Sura Language Setup"
$form.StartPosition = "CenterScreen"
$form.FormBorderStyle = "FixedDialog"
$form.MaximizeBox = $false
$form.MinimizeBox = $false
$form.ClientSize = New-Object System.Drawing.Size(560, 462)
$form.Font = New-Object System.Drawing.Font("Segoe UI", 9)

$logoImage = $null
$logoPath = Resolve-LogoPath
if (-not [string]::IsNullOrWhiteSpace($logoPath)) {
    $logoImage = [System.Drawing.Image]::FromFile($logoPath)
    $logo = New-Object System.Windows.Forms.PictureBox
    $logo.Image = $logoImage
    $logo.SizeMode = "Zoom"
    $logo.Location = New-Object System.Drawing.Point(424, 18)
    $logo.Size = New-Object System.Drawing.Size(112, 112)
    $form.Controls.Add($logo)
    $form.Add_FormClosed({
        if ($null -ne $logoImage) {
            $logoImage.Dispose()
        }
    })
}

$title = New-Object System.Windows.Forms.Label
$title.Text = "Install Sura"
$title.Font = New-Object System.Drawing.Font("Segoe UI", 18, [System.Drawing.FontStyle]::Bold)
$title.Location = New-Object System.Drawing.Point(20, 18)
$title.Size = New-Object System.Drawing.Size(390, 36)
$form.Controls.Add($title)

$subtitle = New-Object System.Windows.Forms.Label
$subtitle.Text = "Set up Sura Language, its package manager, and command launchers."
$subtitle.Location = New-Object System.Drawing.Point(22, 58)
$subtitle.Size = New-Object System.Drawing.Size(390, 24)
$form.Controls.Add($subtitle)

$sizeInfo = New-Object System.Windows.Forms.Label
$sizeInfo.Text = "Required space: about $(Format-ByteSize $estimatedInstallBytes)  |  Package payload: $(Format-ByteSize $payloadBytes)"
$sizeInfo.Location = New-Object System.Drawing.Point(22, 92)
$sizeInfo.Size = New-Object System.Drawing.Size(390, 20)
$form.Controls.Add($sizeInfo)

$licenseLabel = New-Object System.Windows.Forms.Label
$licenseLabel.Text = "Installation actions"
$licenseLabel.Font = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
$licenseLabel.Location = New-Object System.Drawing.Point(22, 136)
$licenseLabel.Size = New-Object System.Drawing.Size(200, 22)
$form.Controls.Add($licenseLabel)

$license = New-Object System.Windows.Forms.TextBox
$license.Multiline = $true
$license.ReadOnly = $true
$license.ScrollBars = "Vertical"
$license.Location = New-Object System.Drawing.Point(24, 160)
$license.Size = New-Object System.Drawing.Size(512, 108)
$license.Text = "Sura Language is installed as a per-user runtime. The installer copies the language engine and package manager, creates sura and surapkg command launchers, and can add the bin directory to your user PATH. You can uninstall it from this package with SuraSetup.ps1 -Uninstall."
$form.Controls.Add($license)

$accept = New-Object System.Windows.Forms.CheckBox
$accept.Text = "I reviewed these installation actions"
$accept.Location = New-Object System.Drawing.Point(24, 282)
$accept.Size = New-Object System.Drawing.Size(300, 24)
$form.Controls.Add($accept)

$pathLabel = New-Object System.Windows.Forms.Label
$pathLabel.Text = "Install location"
$pathLabel.Font = New-Object System.Drawing.Font("Segoe UI", 10, [System.Drawing.FontStyle]::Bold)
$pathLabel.Location = New-Object System.Drawing.Point(22, 318)
$pathLabel.Size = New-Object System.Drawing.Size(200, 22)
$form.Controls.Add($pathLabel)

$pathBox = New-Object System.Windows.Forms.TextBox
$pathBox.Location = New-Object System.Drawing.Point(24, 342)
$pathBox.Size = New-Object System.Drawing.Size(410, 24)
$pathBox.Text = [System.IO.Path]::GetFullPath($InstallDir)
$form.Controls.Add($pathBox)

$browse = New-Object System.Windows.Forms.Button
$browse.Text = "Browse..."
$browse.Location = New-Object System.Drawing.Point(444, 341)
$browse.Size = New-Object System.Drawing.Size(92, 26)
$form.Controls.Add($browse)

$addPath = New-Object System.Windows.Forms.CheckBox
$addPath.Text = "Add Sura Language to my user PATH"
$addPath.Checked = $true
$addPath.Location = New-Object System.Drawing.Point(24, 376)
$addPath.Size = New-Object System.Drawing.Size(260, 24)
$form.Controls.Add($addPath)

$status = New-Object System.Windows.Forms.Label
$status.Text = "Ready."
$status.Location = New-Object System.Drawing.Point(24, 408)
$status.Size = New-Object System.Drawing.Size(320, 24)
$form.Controls.Add($status)

$install = New-Object System.Windows.Forms.Button
$install.Text = "Install"
$install.Enabled = $false
$install.Location = New-Object System.Drawing.Point(350, 406)
$install.Size = New-Object System.Drawing.Size(86, 30)
$form.Controls.Add($install)

$cancel = New-Object System.Windows.Forms.Button
$cancel.Text = "Cancel"
$cancel.Location = New-Object System.Drawing.Point(450, 406)
$cancel.Size = New-Object System.Drawing.Size(86, 30)
$form.Controls.Add($cancel)

$accept.Add_CheckedChanged({
    $install.Enabled = $accept.Checked
})

$browse.Add_Click({
    $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $dialog.Description = "Choose where Sura Language should be installed"
    $dialog.SelectedPath = $pathBox.Text
    if ($dialog.ShowDialog($form) -eq [System.Windows.Forms.DialogResult]::OK) {
        $pathBox.Text = $dialog.SelectedPath
    }
})

$cancel.Add_Click({
    $form.Close()
})

$install.Add_Click({
    try {
        $chosen = [System.IO.Path]::GetFullPath($pathBox.Text)
        $install.Enabled = $false
        $browse.Enabled = $false
        $cancel.Enabled = $false
        $accept.Enabled = $false
        $addPath.Enabled = $false
        $status.Text = "Installing..."
        $form.Refresh()

        $runner = Get-PowerShellRunner
        $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $setupScript, "-InstallDir", $chosen, "-Quiet")
        if (-not $addPath.Checked) { $args += "-NoPath" }
        $previousErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = "Continue"
            $installerOutput = @(& $runner @args 2>&1 | ForEach-Object { "$_" })
            $installerExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
        if ($installerExitCode -ne 0) {
            $detail = ($installerOutput | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }) -join [Environment]::NewLine
            if ([string]::IsNullOrWhiteSpace($detail)) { $detail = "Installer exited with code $installerExitCode." }
            throw $detail
        }

        $status.Text = "Installed."
        [System.Windows.Forms.MessageBox]::Show("Sura Language was installed successfully. Open a new terminal before using the sura command.", "Sura Language Setup", "OK", "Information") | Out-Null
        $form.Close()
    } catch {
        $status.Text = "Install failed."
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, "Sura Language Setup", "OK", "Error") | Out-Null
        $install.Enabled = $accept.Checked
        $browse.Enabled = $true
        $cancel.Enabled = $true
        $accept.Enabled = $true
        $addPath.Enabled = $true
    }
})

[void]$form.ShowDialog()
'@

$guiExeSource = @'
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Windows.Forms;

[assembly: AssemblyVersion("__SURA_LANGUAGE_VERSION__")]
[assembly: AssemblyFileVersion("__SURA_LANGUAGE_VERSION__")]
[assembly: AssemblyInformationalVersion("__SURA_LANGUAGE_VERSION__")]

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SetupForm());
    }
}

internal sealed class SetupForm : Form
{
    private readonly TextBox installPath;
    private readonly CheckBox acceptLicense;
    private readonly CheckBox addToPath;
    private readonly Button installButton;
    private readonly Button browseButton;
    private readonly Button cancelButton;
    private readonly Label statusLabel;
    private Image logoImage;

    public SetupForm()
    {
        Text = "Sura Language Setup";
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        ClientSize = new Size(560, 462);
        Font = new Font("Segoe UI", 9F);

        Icon setupIcon = LoadInstallerIcon();
        if (setupIcon != null)
        {
            Icon = setupIcon;
        }

        logoImage = LoadInstallerLogo();
        if (logoImage != null)
        {
            PictureBox logo = new PictureBox();
            logo.Image = logoImage;
            logo.SizeMode = PictureBoxSizeMode.Zoom;
            logo.Location = new Point(424, 18);
            logo.Size = new Size(112, 112);
            Controls.Add(logo);
        }

        Label title = new Label();
        title.Text = "Install Sura";
        title.Font = new Font("Segoe UI", 18F, FontStyle.Bold);
        title.Location = new Point(20, 18);
        title.Size = new Size(390, 36);
        Controls.Add(title);

        Label subtitle = new Label();
        subtitle.Text = "Set up Sura Language, its package manager, and command launchers.";
        subtitle.Location = new Point(22, 58);
        subtitle.Size = new Size(390, 24);
        Controls.Add(subtitle);

        long payloadBytes = DirectorySize(PayloadDir());
        long estimatedInstallBytes = payloadBytes + 1048576L;
        Label sizeInfo = new Label();
        sizeInfo.Text = "Required space: about " + FormatByteSize(estimatedInstallBytes) + "  |  Package payload: " + FormatByteSize(payloadBytes);
        sizeInfo.Location = new Point(22, 92);
        sizeInfo.Size = new Size(390, 20);
        Controls.Add(sizeInfo);

        Label licenseLabel = new Label();
        licenseLabel.Text = "Installation actions";
        licenseLabel.Font = new Font("Segoe UI", 10F, FontStyle.Bold);
        licenseLabel.Location = new Point(22, 136);
        licenseLabel.Size = new Size(200, 22);
        Controls.Add(licenseLabel);

        TextBox license = new TextBox();
        license.Multiline = true;
        license.ReadOnly = true;
        license.ScrollBars = ScrollBars.Vertical;
        license.Location = new Point(24, 160);
        license.Size = new Size(512, 108);
        license.Text = "Sura Language is installed as a per-user runtime. The installer copies the language engine and package manager, creates sura and surapkg command launchers, and can add the bin directory to your user PATH. You can uninstall it from this package with SuraSetup.ps1 -Uninstall.";
        Controls.Add(license);

        acceptLicense = new CheckBox();
        acceptLicense.Text = "I reviewed these installation actions";
        acceptLicense.Location = new Point(24, 282);
        acceptLicense.Size = new Size(300, 24);
        acceptLicense.CheckedChanged += delegate { installButton.Enabled = acceptLicense.Checked; };
        Controls.Add(acceptLicense);

        Label pathLabel = new Label();
        pathLabel.Text = "Install location";
        pathLabel.Font = new Font("Segoe UI", 10F, FontStyle.Bold);
        pathLabel.Location = new Point(22, 318);
        pathLabel.Size = new Size(200, 22);
        Controls.Add(pathLabel);

        installPath = new TextBox();
        installPath.Location = new Point(24, 342);
        installPath.Size = new Size(410, 24);
        installPath.Text = DefaultInstallDir();
        Controls.Add(installPath);

        browseButton = new Button();
        browseButton.Text = "Browse...";
        browseButton.Location = new Point(444, 341);
        browseButton.Size = new Size(92, 26);
        browseButton.Click += BrowseClicked;
        Controls.Add(browseButton);

        addToPath = new CheckBox();
        addToPath.Text = "Add Sura Language to my user PATH";
        addToPath.Checked = true;
        addToPath.Location = new Point(24, 376);
        addToPath.Size = new Size(260, 24);
        Controls.Add(addToPath);

        statusLabel = new Label();
        statusLabel.Text = "Ready.";
        statusLabel.Location = new Point(24, 408);
        statusLabel.Size = new Size(320, 24);
        Controls.Add(statusLabel);

        installButton = new Button();
        installButton.Text = "Install";
        installButton.Enabled = false;
        installButton.Location = new Point(350, 406);
        installButton.Size = new Size(86, 30);
        installButton.Click += InstallClicked;
        Controls.Add(installButton);

        cancelButton = new Button();
        cancelButton.Text = "Cancel";
        cancelButton.Location = new Point(450, 406);
        cancelButton.Size = new Size(86, 30);
        cancelButton.Click += delegate { Close(); };
        Controls.Add(cancelButton);
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        if (logoImage != null)
        {
            logoImage.Dispose();
            logoImage = null;
        }
        base.OnFormClosed(e);
    }

    private static string SupportFilePath(string fileName)
    {
        string rootFile = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, fileName);
        if (File.Exists(rootFile))
        {
            return rootFile;
        }
        return Path.Combine(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "support"), fileName);
    }

    private static Image LoadInstallerLogo()
    {
        string path = SupportFilePath("SuraLogo.png");
        if (File.Exists(path))
        {
            return Image.FromFile(path);
        }
        return null;
    }

    private static Icon LoadInstallerIcon()
    {
        string path = SupportFilePath("SuraLogo.ico");
        if (File.Exists(path))
        {
            return new Icon(path);
        }
        return null;
    }

    private static string DefaultInstallDir()
    {
        string local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (!String.IsNullOrEmpty(local))
        {
            return Path.Combine(Path.Combine(local, "Programs"), "Sura");
        }
        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (String.IsNullOrEmpty(home))
        {
            home = AppDomain.CurrentDomain.BaseDirectory;
        }
        return Path.Combine(home, ".sura");
    }

    private static string SetupScriptPath()
    {
        string rootScript = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "SuraSetup.ps1");
        if (File.Exists(rootScript))
        {
            return rootScript;
        }
        return Path.Combine(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "support"), "SuraSetup.ps1");
    }

    private static string PayloadDir()
    {
        string rootPayload = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "payload");
        if (Directory.Exists(rootPayload))
        {
            return rootPayload;
        }
        return Path.Combine(Path.GetDirectoryName(AppDomain.CurrentDomain.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)), "payload");
    }

    private static long DirectorySize(string path)
    {
        if (String.IsNullOrEmpty(path) || !Directory.Exists(path))
        {
            return 0L;
        }
        long total = 0L;
        foreach (string file in Directory.GetFiles(path, "*", SearchOption.AllDirectories))
        {
            total += new FileInfo(file).Length;
        }
        return total;
    }

    private static string FormatByteSize(long bytes)
    {
        if (bytes >= 1073741824L)
        {
            return String.Format("{0:N1} GB", bytes / 1073741824.0);
        }
        if (bytes >= 1048576L)
        {
            return String.Format("{0:N1} MB", bytes / 1048576.0);
        }
        if (bytes >= 1024L)
        {
            return String.Format("{0:N1} KB", bytes / 1024.0);
        }
        return bytes + " bytes";
    }

    private static string PowerShellPath()
    {
        string ps = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell\\v1.0\\powershell.exe");
        if (File.Exists(ps))
        {
            return ps;
        }
        return "powershell.exe";
    }

    private static string Quote(string value)
    {
        if (value == null)
        {
            value = "";
        }
        return "\"" + value.Replace("\"", "\\\"") + "\"";
    }

    private void BrowseClicked(object sender, EventArgs e)
    {
        using (FolderBrowserDialog dialog = new FolderBrowserDialog())
        {
            dialog.Description = "Choose where Sura Language should be installed";
            if (Directory.Exists(installPath.Text))
            {
                dialog.SelectedPath = installPath.Text;
            }
            if (dialog.ShowDialog(this) == DialogResult.OK)
            {
                installPath.Text = dialog.SelectedPath;
            }
        }
    }

    private void InstallClicked(object sender, EventArgs e)
    {
        try
        {
            string script = SetupScriptPath();
            if (!File.Exists(script))
            {
                throw new InvalidOperationException("SuraSetup.ps1 was not found next to this installer.");
            }

            string chosenPath = Path.GetFullPath(installPath.Text.Trim());
            SetBusy(true);
            statusLabel.Text = "Installing...";
            Refresh();

            string args = "-NoProfile -ExecutionPolicy Bypass -File " + Quote(script) + " -InstallDir " + Quote(chosenPath) + " -Quiet";
            if (!addToPath.Checked)
            {
                args += " -NoPath";
            }

            ProcessStartInfo psi = new ProcessStartInfo(PowerShellPath(), args);
            psi.WorkingDirectory = AppDomain.CurrentDomain.BaseDirectory;
            psi.UseShellExecute = false;
            psi.CreateNoWindow = true;
            psi.RedirectStandardOutput = true;
            psi.RedirectStandardError = true;
            Process process = Process.Start(psi);
            StringBuilder output = new StringBuilder();
            process.OutputDataReceived += delegate(object outputSender, DataReceivedEventArgs outputEvent)
            {
                if (outputEvent.Data != null)
                {
                    lock (output) output.AppendLine(outputEvent.Data);
                }
            };
            process.ErrorDataReceived += delegate(object errorSender, DataReceivedEventArgs errorEvent)
            {
                if (errorEvent.Data != null)
                {
                    lock (output) output.AppendLine(errorEvent.Data);
                }
            };
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            process.WaitForExit();
            if (process.ExitCode != 0)
            {
                string detail = output.ToString().Trim();
                if (detail.Length == 0)
                {
                    detail = "Installer exited with code " + process.ExitCode + ".";
                }
                throw new InvalidOperationException(detail);
            }

            statusLabel.Text = "Installed.";
            MessageBox.Show(this, "Sura Language was installed successfully. Open a new terminal before using the sura command.", "Sura Language Setup", MessageBoxButtons.OK, MessageBoxIcon.Information);
            Close();
        }
        catch (Exception ex)
        {
            statusLabel.Text = "Install failed.";
            MessageBox.Show(this, ex.Message, "Sura Language Setup", MessageBoxButtons.OK, MessageBoxIcon.Error);
            SetBusy(false);
        }
    }

    private void SetBusy(bool busy)
    {
        installButton.Enabled = !busy && acceptLicense.Checked;
        browseButton.Enabled = !busy;
        cancelButton.Enabled = !busy;
        acceptLicense.Enabled = !busy;
        addToPath.Enabled = !busy;
        installPath.Enabled = !busy;
    }
}
'@
$guiExeSource = $guiExeSource.Replace("__SURA_LANGUAGE_VERSION__", $Version)

$guiLauncher = @'
@echo off
if exist "%~dp0..\SuraSetup.exe" (
  "%~dp0..\SuraSetup.exe" %*
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0SuraSetup-GUI.ps1" %*
)
'@

$readme = @'
# Sura Language Windows Installer

This package installs Sura Language as a normal per-user language runtime.

## GUI Install

Normal users should download and double-click the single-file installer:

```text
__SURA_LANGUAGE_INSTALLER_FILE__
```

That one file extracts its internal installer kit, opens the setup window, and cleans up the temporary files after setup exits. The setup window shows required disk space, the installation actions, install path selection, and the user PATH option.

The folder installer entry point below is for developers and release verification:

```text
dist\SuraLanguage-windows-x64\SuraSetup.exe
```

The `payload` and `support` folders are installer internals.

## Command-Line Install

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\support\SuraSetup.ps1
```

The installer copies the Sura Language engine and `surapkg.exe` to:

```text
%LOCALAPPDATA%\Programs\Sura\bin
```

It also creates `sura.cmd` and `surapkg.cmd`, then adds the bin directory to the user PATH. Open a new terminal after installation.

## Use

```powershell
sura app.sura
sura --repl
surapkg new my_app
cd my_app
surapkg run
surapkg test
```

No Python, Node.js, CMake, or compiler is required for normal Sura scripts. Those tools are only needed for optional developer features such as Python interop, JavaScript/WASM target tests, native plugins, embedding, or benchmarks.

## Uninstall

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\support\SuraSetup.ps1 -Uninstall
```
'@
$readme = $readme.Replace("__SURA_LANGUAGE_INSTALLER_FILE__", $standaloneFileName)

$manifest = [ordered]@{
    schema = "sura.installer.manifest.v1"
    product_name = "Sura Language"
    version = $Version
    engine_version = $engineVersion
    platform = "windows-x64"
    installer_modes = @("single-exe", "gui-exe", "gui-powershell", "cli")
    single_file_installer = $standaloneFileName
    commands = @("sura", "surapkg")
    default_install_dir = "%LOCALAPPDATA%\Programs\Sura"
    normal_use_dependencies = @("Windows x64")
    installer_dependencies = @("Windows x64", ".NET Framework CLR", "Windows PowerShell or PowerShell")
    optional_runtime_dependencies = @("FFmpeg for media video decoding")
    optional_developer_dependencies = @("node", "python", "c++ compiler", "cmake")
    branding = [ordered]@{
        logo = "support/SuraLogo.png"
        icon = "support/SuraLogo.ico"
        logo_aspect_ratio = "1:1"
    }
    payload_size_bytes = $payloadBytes
    estimated_install_size_bytes = $estimatedInstallBytes
}

Write-Text (Join-Path $support "SuraSetup.ps1") $setupScript
Write-Text (Join-Path $support "SuraSetup-GUI.ps1") $guiScript
Write-Text (Join-Path $support "SuraSetup.cs") $guiExeSource
$guiExePath = Join-Path $outRoot "SuraSetup.exe"
$guiExeBuild = Build-GuiSetupExe (Join-Path $support "SuraSetup.cs") $guiExePath (Join-Path $support "SuraLogo.ico")
Write-Text (Join-Path $support "SuraSetup.cmd") $guiLauncher
Write-Text (Join-Path $outRoot "README_INSTALL.md") $readme
Write-Text (Join-Path $outRoot "installer-manifest.json") ($manifest | ConvertTo-Json -Depth 5)

$standaloneBuild = [ordered]@{
    created = $false
    compiler = ""
    size_bytes = 0L
}
if (-not $SkipStandalone) {
    $standaloneBuild = Build-StandaloneSetupExe (Join-Path $support "SuraStandaloneSetup.cs") $standaloneExeFull $outRoot (Join-Path $support "SuraLogo.ico")
}

$zipCreated = $false
if (-not $SkipZip) {
    $zipParent = Split-Path -Parent $ZipPath
    if ($zipParent) { New-Item -ItemType Directory -Force -Path $zipParent | Out-Null }
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    $zipItems = @(Get-ChildItem -LiteralPath $outRoot -Force | ForEach-Object { $_.FullName })
    if ($zipItems.Count -eq 0) {
        throw "installer output directory is empty: $outRoot"
    }
    Compress-Archive -LiteralPath $zipItems -DestinationPath $ZipPath -Force
    $zipCreated = Test-Path -LiteralPath $ZipPath
}

$report = [ordered]@{
    schema = "sura.installer.pack.v1"
    product_name = "Sura Language"
    generated_utc = [DateTime]::UtcNow.ToString("o")
    version = $Version
    engine_version = $engineVersion
    installer_dir = $outRoot
    setup_script = (Join-Path $support "SuraSetup.ps1")
    gui_setup_script = (Join-Path $support "SuraSetup-GUI.ps1")
    gui_setup_source = (Join-Path $support "SuraSetup.cs")
    gui_setup_exe = $guiExePath
    gui_setup_exe_created = [bool]$guiExeBuild.created
    gui_setup_exe_compiler = [string]$guiExeBuild.compiler
    gui_launcher = (Join-Path $support "SuraSetup.cmd")
    branding_logo = (Join-Path $support "SuraLogo.png")
    branding_icon = (Join-Path $support "SuraLogo.ico")
    branding_logo_aspect_ratio = "1:1"
    single_file_installer = $standaloneExeFull
    single_file_installer_created = [bool]$standaloneBuild.created
    single_file_installer_compiler = [string]$standaloneBuild.compiler
    single_file_installer_size_bytes = [int64]$standaloneBuild.size_bytes
    zip_path = $(if ($SkipZip) { "" } else { [System.IO.Path]::GetFullPath($ZipPath) })
    zip_created = $zipCreated
    payload_size_bytes = $payloadBytes
    estimated_install_size_bytes = $estimatedInstallBytes
    payload = @("SuraLanguage.exe", "surapkg.exe", "examples/")
    user_commands = @("sura", "surapkg")
}

if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    Write-Text $JsonOut ($report | ConvertTo-Json -Depth 5)
}

Write-Host ("sura_make_installer: PASS ({0})" -f $outRoot)
if (-not $SkipStandalone) {
    Write-Host ("single_file_installer: {0}" -f $standaloneExeFull)
}
if (-not $SkipZip) {
    Write-Host ("installer_zip: {0}" -f ([System.IO.Path]::GetFullPath($ZipPath)))
}
