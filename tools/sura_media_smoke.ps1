param(
    [string]$Engine = ".\SuraLanguage.exe",
    [string]$Cxx = ""
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
# SURA_CXX wins: CI installs MSYS2 under the runner temp directory, so a
# hardcoded C:\msys64 path is absent there. Fall back to PATH, not a guess.
$Cxx = if ($Cxx) { $Cxx } elseif ($env:SURA_CXX) { $env:SURA_CXX } elseif ($IsWindows -or $env:OS -eq "Windows_NT") { if (Test-Path -LiteralPath "C:\msys64\mingw64\bin\g++.exe") { "C:\msys64\mingw64\bin\g++.exe" } else { "g++" } } else { "c++" }
$enginePath = (Resolve-Path -LiteralPath (Join-Path $root $Engine)).Path
$sourcePath = Join-Path $root "tests\media_fake_ffmpeg.cpp"
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura media & argv " + [guid]::NewGuid().ToString("N"))
$fake = Join-Path $temp "fake ffmpeg.exe"
$sentinelName = "SURA_MEDIA_PWNED_" + [guid]::NewGuid().ToString("N")
$sentinel = Join-Path $root $sentinelName
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Sura-Path([string]$Path) {
    return $Path.Replace('\', '/').Replace('"', '\"')
}

function Write-Mode([string]$Path, [string]$Mode) {
    [System.IO.File]::WriteAllText($Path, $Mode, $utf8NoBom)
}

function Run-Positive([string]$Script) {
    foreach ($args in @(@($Script), @("--jit", $Script))) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & $enginePath @args 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $old
        }
        if ($code -ne 0) {
            throw "media positive case failed: $($output -join "`n")"
        }
        if (($output -join "`n") -notmatch "media_smoke: PASS") {
            throw "media positive case did not report PASS: $($output -join "`n")"
        }
    }
}

function Run-Failure([string]$Name, [string]$Mode, [string]$Expected) {
    $input = Join-Path $temp ($Name + ".mp4")
    Write-Mode $input $Mode
    $script = Join-Path $temp ($Name + ".sura")
    $source = @"
use media
media.ascii_frames("$(Sura-Path $input)", {ffmpeg: "$(Sura-Path $fake)", width: 4, height: 2, fps: 2, max_frames: 2, charset: " .#@", timeout_ms: 1000})
"@
    [System.IO.File]::WriteAllText($script, $source, $utf8NoBom)
    foreach ($args in @(@($script), @("--jit", $script))) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & $enginePath @args 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $old
        }
        $text = $output -join "`n"
        if ($code -eq 0 -or $text -notmatch $Expected) {
            throw "$Name should fail with /$Expected/: $text"
        }
    }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    & $Cxx -std=c++17 -O2 $sourcePath -o $fake
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $fake)) {
        throw "failed to build media fake ffmpeg"
    }

    $input = Join-Path $temp ("video & mkdir " + $sentinelName + " & rem.mp4")
    Write-Mode $input "ok"
    $script = Join-Path $temp "media positive.sura"
    $source = @"
use media

decoder is "$(Sura-Path $fake)"
source is "$(Sura-Path $input)"
assert_eq(media.available(decoder), true)
clip is media.ascii_frames(source, {ffmpeg: decoder, width: 4, height: 2, fps: 2, max_frames: 2, charset: " .#@", timeout_ms: 5000})
assert_eq(clip.format, "sura.text-video.v1")
assert_eq(clip.backend, "ffmpeg-pgm")
assert_eq(clip.width, 4)
assert_eq(clip.height, 2)
assert_eq(clip.frame_count, 2)
assert_eq(clip.truncated, true)
assert_eq(clip.timestamps[0], 0)
assert_eq(clip.timestamps[1], 0.5)
assert_eq(clip.frames[0], " .#@\n@#. ")
assert_eq(clip.frames[1], "    \n@#.#")

alias_clip is media_video_text_frames(source, {ffmpeg: decoder, width: 4, height: 2, fps: 2, max_frames: 5, charset: " .#@"})
assert_eq(alias_clip.frame_count, 3)
assert_eq(alias_clip.truncated, false)
print "media_smoke: PASS"
"@
    [System.IO.File]::WriteAllText($script, $source, $utf8NoBom)
    Run-Positive $script
    if (Test-Path -LiteralPath $sentinel) {
        throw "media decoder invocation passed through a shell"
    }

    Run-Failure "bad_magic" "badmagic" "not binary PGM"
    Run-Failure "truncated_pixels" "truncated" "pixel payload is truncated"
    Run-Failure "decoder_failure" "fail" "ffmpeg failed with exit code 7"
    Run-Failure "decoder_timeout" "sleep" "exceeded timeout_ms"

    "media_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $sentinel -PathType Container) {
        Remove-Item -LiteralPath $sentinel -Force
    }
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
