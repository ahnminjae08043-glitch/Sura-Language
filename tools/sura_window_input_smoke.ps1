param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$root = Split-Path -Parent $PSScriptRoot
$engineCandidate = if ([System.IO.Path]::IsPathRooted($Engine)) { $Engine } else { Join-Path $root $Engine }
$EnginePath = (Resolve-Path -LiteralPath $engineCandidate).Path
$scriptPath = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_window_input_smoke_{0}.sura" -f ([guid]::NewGuid().ToString("N")))
$title = "Sura Input Smoke"

@"
if not win_init(320, 120, "$title") then
  print "window_input_smoke: win_init failed"
  exit(1)
end

key is readkey_timeout(2000)
next_key is readkey()
win_close()

if key == "w" and next_key == "d" then
  print "window_input_smoke: PASS"
else
  print "window_input_smoke: FAIL got=" + key + "," + next_key
  exit(1)
end
"@ | Set-Content -LiteralPath $scriptPath -Encoding UTF8

Add-Type -Namespace SuraSmoke -Name NativeInput -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr FindWindowW(string className, string windowName);

[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool PostMessageW(System.IntPtr hWnd, uint msg, System.IntPtr wParam, System.IntPtr lParam);

private delegate bool EnumWindowsProc(System.IntPtr hWnd, System.IntPtr lParam);

[System.Runtime.InteropServices.DllImport("user32.dll")]
private static extern bool EnumWindows(EnumWindowsProc callback, System.IntPtr lParam);

[System.Runtime.InteropServices.DllImport("user32.dll")]
private static extern uint GetWindowThreadProcessId(System.IntPtr hWnd, out uint processId);

[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
private static extern int GetClassNameW(System.IntPtr hWnd, System.Text.StringBuilder className, int maxCount);

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
"@

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $EnginePath
$psi.Arguments = "`"$scriptPath`""
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
            throw "window_input_smoke exited before creating the window with exit code $($proc.ExitCode)"
        }
        $hwnd = [SuraSmoke.NativeInput]::FindSuraWindowForProcess([uint32]$proc.Id)
    }
    if ($hwnd -eq [IntPtr]::Zero) {
        throw "Sura smoke window was not created"
    }

    $wmKeyDown = 0x0100
    $wmKeyUp = 0x0101
    $vkW = 0x57
    $vkD = 0x44
    [void][SuraSmoke.NativeInput]::PostMessageW($hwnd, $wmKeyDown, [IntPtr]$vkW, [IntPtr]0)
    Start-Sleep -Milliseconds 50
    [void][SuraSmoke.NativeInput]::PostMessageW($hwnd, $wmKeyUp, [IntPtr]$vkW, [IntPtr]0)
    Start-Sleep -Milliseconds 50
    [void][SuraSmoke.NativeInput]::PostMessageW($hwnd, $wmKeyDown, [IntPtr]$vkD, [IntPtr]0)
    Start-Sleep -Milliseconds 50
    [void][SuraSmoke.NativeInput]::PostMessageW($hwnd, $wmKeyUp, [IntPtr]$vkD, [IntPtr]0)

    if (-not $proc.WaitForExit(7000)) {
        try { $proc.Kill() } catch {}
        throw "window_input_smoke timed out"
    }

    $output = ($proc.StandardOutput.ReadToEnd() + $proc.StandardError.ReadToEnd())
    if ($proc.ExitCode -ne 0 -or $output -notmatch "window_input_smoke: PASS") {
        $output | Write-Host
        throw "window_input_smoke failed with exit code $($proc.ExitCode)"
    }
    Write-Host "window_input_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue
}
