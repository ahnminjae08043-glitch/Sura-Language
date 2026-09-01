param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8NoBom
[Console]::InputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_console_" + [System.Guid]::NewGuid().ToString("N"))

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
    $script = Join-Path $temp "console_smoke.sura"
    Write-Text $script @'
console.log("console", "api", 1)
console.info("info", "한글")
console.warn("warn", "message")
console.warning("warning", "alias")
console.error("error", "message")
console.exception("exception", "message")
console.raw("raw", "-", "api")
console.line()
console.flush()
console.json({status: "ok", score: 7})
inspect_text is console.inspect({status: "ok", score: 7})
assert_eq(type(inspect_text), "string")
assert_contains(inspect_text, "\"status\"")
assert_contains(inspect_text, "\"score\"")
terminal_status_now is console.status()
assert_eq(type(terminal_status_now.group_depth), "number")
assert_eq(type(terminal_status_now.timers), "number")
hr_ms is console.hrtime()
assert_eq(type(hr_ms), "number")
assert(hr_ms >= 0)
console.beep()
console.print("print", "alias")
console.write("write", "api")
console.write_line(" done")
console.println("println", "alias")
console.line("line", "alias")
console.debug("debug", true)
console.assert(true, "hidden")
console.assert(false, "failed", 7)

first_count is console.count("tick")
second_count is console.count("tick")
console.count_reset("tick")
reset_count is console.count("tick")
assert_eq(first_count, 1)
assert_eq(second_count, 2)
assert_eq(reset_count, 1)

console.time("phase")
wait(1)
mid_elapsed is console.time_log("phase", "checkpoint")
elapsed is console.time_end("phase")
stamp_ms is console.time_stamp("phase mark")
assert_eq(type(elapsed), "number")
assert_eq(type(mid_elapsed), "number")
assert_eq(type(stamp_ms), "number")
assert(elapsed >= 0)

console.time("camel phase")
wait(1)
camel_mid_elapsed is console.timeLog("camel phase", "checkpoint")
camel_elapsed is console.timeEnd("camel phase")
camel_stamp_ms is console.timeStamp("camel phase mark")
assert_eq(type(camel_mid_elapsed), "number")
assert_eq(type(camel_elapsed), "number")
assert_eq(type(camel_stamp_ms), "number")

console.table([{name: "sura", score: 10}, {name: "console", score: 20}])
console.dir({ok: true, text: "한글"})
console.dirxml("dirxml text")
console.group("outer")
console.log("nested", "message")
console.group_end()
console.group_collapsed("collapsed")
console.info("collapsed nested")
console.groupEnd()
console.groupCollapsed("camel collapsed")
console.warn("camel nested")
console.groupEnd()
console.trace("trace", "message")
console.profile("smoke profile")
wait(1)
profile_elapsed is console.profile_end("smoke profile")
assert_eq(type(profile_elapsed), "number")
console.profile("camel profile")
wait(1)
camel_profile_elapsed is console.profileEnd("camel profile")
assert_eq(type(camel_profile_elapsed), "number")

styled_text is console.style("styled", ["bold", "green"])
colored_text is console.color("color", "cyan")
assert_eq(console.strip_ansi(styled_text), "styled")
assert_eq(console.stripAnsi(colored_text), "color")
assert_eq(console_strip_ansi(console_style("direct-style", ["underline", "bright_blue"])), "direct-style")
assert_eq(console_strip_ansi(console_color("direct-color", "yellow")), "direct-color")
console.set_color("default")
console.resetColor()
console_set_color("default")
console_reset_color()
console_info("styled", "ok")
console_info("color", "ok")
terminal_size is console.size()
assert_eq(type(console.is_tty()), "bool")
assert_eq(type(console.isTTY()), "bool")
assert_eq(type(console.width()), "number")
assert_eq(type(console.height()), "number")
assert_eq(type(terminal_size), "dict")
assert_eq(type(terminal_size.width), "number")
assert_eq(type(terminal_size.height), "number")
console_info("terminal", "size", "ok")

use console
console.log("module", "still", "works")
console_log("direct", "works")
console_print("direct", "print")
console_table({direct: "table"})
console_json({direct: true, score: 8})
direct_inspect_text is console_inspect({direct: true, score: 8})
assert_eq(type(direct_inspect_text), "string")
console_raw("direct", "-", "raw")
console_line()
console_flush()
direct_status_now is console_status()
assert_eq(type(direct_status_now.counters), "number")
direct_hr_ms is console_hrtime()
assert_eq(type(direct_hr_ms), "number")
console_beep()
console_write("direct", "write")
console_write_line(" done")
console_println("direct", "println")
console_line("direct", "line")
console_dirxml("direct dirxml")
direct_stamp_ms is console_time_stamp("direct mark")
assert_eq(type(direct_stamp_ms), "number")
console_profile("direct profile")
wait(1)
direct_profile_elapsed is console_profile_end("direct profile")
assert_eq(type(direct_profile_elapsed), "number")
print("console_smoke: PASS")
'@

    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    Push-Location $temp
    try {
        $out = & $EnginePath $script 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
    } finally {
        Pop-Location
        $ErrorActionPreference = $old
    }
    $output = $out -join "`n"
    if ($code -ne 0) {
        Write-Output $output
        throw "console smoke exited with code $code"
    }
    foreach ($needle in @(
        "console api 1",
        "info 한글",
        "warn message",
        "warning alias",
        "error message",
        "exception message",
        "raw-api",
        '"status": "ok"',
        '"score": 7',
        "print alias",
        "write api done",
        "println alias",
        "line alias",
        "debug true",
        "Assertion failed: failed 7",
        "tick: 1",
        "tick: 2",
        "phase:",
        "checkpoint",
        "Timestamp phase mark:",
        "camel phase:",
        "Timestamp camel phase mark:",
        "(index)",
        "score",
        '"text": "한글"',
        '"ok": true',
        "dirxml text",
        "outer",
        "  nested message",
        "collapsed",
        "  collapsed nested",
        "camel collapsed",
        "  camel nested",
        "Trace: trace message",
        "Profile 'smoke profile' started",
        "Profile 'smoke profile':",
        "Profile 'camel profile' started",
        "Profile 'camel profile':",
        "styled ok",
        "color ok",
        "terminal size ok",
        "module still works",
        "direct works",
        "direct print",
        '"direct": true',
        '"score": 8',
        "direct-raw",
        "direct write done",
        "direct println",
        "direct line",
        "direct dirxml",
        "Timestamp direct mark:",
        "Profile 'direct profile' started",
        "Profile 'direct profile':",
        "direct",
        "table",
        "console_smoke: PASS"
    )) {
        if ($output -notlike "*$needle*") {
            Write-Output $output
            throw "console smoke output missing: $needle"
        }
    }
    if ($output -like "*hidden*") {
        Write-Output $output
        throw "console.assert(true, ...) should not emit output"
    }

    "sura_console_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
