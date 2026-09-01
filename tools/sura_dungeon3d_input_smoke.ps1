param(
    [string]$Engine = ".\SuraLanguage.exe",
    [string]$Script = ".\examples/games/dungeon3d.sura"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
[Console]::InputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$root = Split-Path -Parent $PSScriptRoot
$engineCandidate = if ([System.IO.Path]::IsPathRooted($Engine)) { $Engine } else { Join-Path $root $Engine }
$EnginePath = (Resolve-Path -LiteralPath $engineCandidate).Path
$scriptCandidate = if ([System.IO.Path]::IsPathRooted($Script)) { $Script } else { Join-Path $root $Script }
$script = (Resolve-Path -LiteralPath $scriptCandidate).Path

Add-Type -Namespace SuraSmoke -Name DungeonInput -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool PostMessageW(System.IntPtr hWnd, uint msg, System.IntPtr wParam, System.IntPtr lParam);

private delegate bool EnumWindowsProc(System.IntPtr hWnd, System.IntPtr lParam);

[System.Runtime.InteropServices.DllImport("user32.dll")]
private static extern bool EnumWindows(EnumWindowsProc callback, System.IntPtr lParam);

[System.Runtime.InteropServices.DllImport("user32.dll")]
private static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint processId);

[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
private static extern int GetClassNameW(System.IntPtr hWnd, System.Text.StringBuilder className, int maxCount);

[System.Runtime.InteropServices.DllImport("user32.dll")]
private static extern bool SetForegroundWindow(System.IntPtr hWnd);

public static System.IntPtr FindSuraWindowForProcess(uint processId) {
    System.IntPtr found = System.IntPtr.Zero;
    EnumWindows(delegate(System.IntPtr hWnd, System.IntPtr lParam) {
        uint windowProcessId;
        GetWindowThreadProcessId(hWnd, out windowProcessId);
        if (windowProcessId != processId) return true;
        System.Text.StringBuilder className = new System.Text.StringBuilder(256);
        GetClassNameW(hWnd, className, className.Capacity);
        if (className.ToString() == "SuraRuntimeWindow") {
            found = hWnd;
            return false;
        }
        return true;
    }, System.IntPtr.Zero);
    return found;
}

public static void Focus(System.IntPtr hWnd) {
    SetForegroundWindow(hWnd);
}
"@

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $EnginePath
$psi.Arguments = "`"$script`" -- --input-smoke"
$psi.WorkingDirectory = $root
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true

$proc = [System.Diagnostics.Process]::Start($psi)
try {
    $hwnd = [IntPtr]::Zero
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while ([DateTime]::UtcNow -lt $deadline -and $hwnd -eq [IntPtr]::Zero) {
        Start-Sleep -Milliseconds 50
        if ($proc.HasExited) {
            $earlyOutput = ($proc.StandardOutput.ReadToEnd() + $proc.StandardError.ReadToEnd())
            $earlyOutput | Write-Host
            throw "dungeon3d_input_smoke exited before creating the window with exit code $($proc.ExitCode)"
        }
        $hwnd = [SuraSmoke.DungeonInput]::FindSuraWindowForProcess([uint32]$proc.Id)
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        throw "Sura dungeon window was not created"
    }

    [SuraSmoke.DungeonInput]::Focus($hwnd)
    $wmKeyDown = 0x0100
    $wmKeyUp = 0x0101
    $vkW = 0x57
    [void][SuraSmoke.DungeonInput]::PostMessageW($hwnd, $wmKeyDown, [IntPtr]$vkW, [IntPtr]0)
    Start-Sleep -Milliseconds 1000
    [void][SuraSmoke.DungeonInput]::PostMessageW($hwnd, $wmKeyUp, [IntPtr]$vkW, [IntPtr]0)

    if (-not $proc.WaitForExit(8000)) {
        try { $proc.Kill() } catch {}
        throw "dungeon3d_input_smoke timed out"
    }

    $output = ($proc.StandardOutput.ReadToEnd() + $proc.StandardError.ReadToEnd())
    if ($proc.ExitCode -ne 0 -or $output -notmatch "dungeon3d_input_smoke: PASS") {
        $output | Write-Host
        throw "dungeon3d_input_smoke failed with exit code $($proc.ExitCode)"
    }
    Write-Host "dungeon3d_input_smoke: PASS"
} finally {
    if (-not $proc.HasExited) {
        try { $proc.Kill() } catch {}
    }
}
