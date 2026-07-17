param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
[Console]::InputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$EnginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_console_game_api_" + [System.Guid]::NewGuid().ToString("N"))

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    $Text = $Text -replace "`r`n", "`n"
    if (-not $Text.EndsWith("`n")) { $Text += "`n" }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $script = Join-Path $temp "console_game_api_smoke.sura"
    $hangulCell = [char]0xD55C
    Write-Text $script (@'
pressed_fn is key_down("space")
assert_eq(type(pressed_fn), "bool")

key_down "space" pressed_cmd
assert_eq(type(pressed_cmd), "bool")

key_value is readkey_timeout(1)
assert_eq(type(key_value), "string")

readkey_timeout timed_key 1
assert_eq(type(timed_key), "string")

sleep 1
assert_eq(type(sleep(1)), "nil")

mouse_status is mouse_pos()
assert_eq(type(mouse_status), "dict")
assert_eq(type(mouse_status.x), "number")
assert_eq(type(mouse_status.y), "number")

mouse_pos mouse_x_value mouse_y_value
assert_eq(type(mouse_x_value), "number")
assert_eq(type(mouse_y_value), "number")

left_fn is mouse_down("left")
assert_eq(type(left_fn), "bool")

mouse_down "left" left_cmd
assert_eq(type(left_cmd), "bool")

grid_clear
grid_init 6 3
grid_clear
grid_set 0 0 "G"
grid_set 1 0 "C" "cyan"
grid_set 2 0 "{0}" "yellow"
grid_draw
console_raw("\n")

window_ready is win_init(160, 100, "Sura Smoke")
assert_eq(type(window_ready), "bool")
if window_ready then
    win_clear(12, 18, 24)
    win_rect(12, 12, 50, 28, 40, 160, 220)
    win_circle(96, 46, 18, 240, 90, 90)
    win_line(0, 0, 159, 99, 250, 250, 250)
    win_text("Sura", 16, 68, 255, 255, 255)
    assert_eq(type(win_poll()), "bool")
    assert_eq(type(win_update()), "bool")
    win_close()
end

print "console_game_api_smoke: PASS"
'@ -f $hangulCell)

    $output = & $EnginePath $script 2>&1
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $output | Out-String | Write-Host
        throw "console_game_api_smoke failed with exit code $code"
    }
    $text = ($output | Out-String)
    if ($text -notmatch "console_game_api_smoke: PASS") {
        $text | Write-Host
        throw "console_game_api_smoke did not print PASS"
    }
    Write-Host "console_game_api_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}
