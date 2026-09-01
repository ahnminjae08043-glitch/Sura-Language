param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$EnginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_async_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $script = Join-Path $temp "async_smoke.sura"
    Write-Text $script @'
use async
use fs

fs.write("async_a.txt", "alpha")
fs.write("async_b.txt", "bravo")
fs.write("async_c.txt", "charlie")
fs.write("async_d.txt", "delta")
fs.write("async_e.txt", "echo")
fs.write("async_f.txt", "foxtrot")
fs.write("async_g.txt", "golf")
fs.write("async_h.txt", "hotel")
fs.write("async_i.txt", "india")
fs.write("async_j.txt", "juliet")

async_base_dir is fs.abs(".").replace("\\", "/")
url_a is "file://" + async_base_dir + "/async_a.txt"
url_b is "file://" + async_base_dir + "/async_b.txt"
url_c is "file://" + async_base_dir + "/async_c.txt"
url_d is "file://" + async_base_dir + "/async_d.txt"
url_e is "file://" + async_base_dir + "/async_e.txt"
url_f is "file://" + async_base_dir + "/async_f.txt"
url_g is "file://" + async_base_dir + "/async_g.txt"
url_h is "file://" + async_base_dir + "/async_h.txt"
url_i is "file://" + async_base_dir + "/async_i.txt"
url_j is "file://" + async_base_dir + "/async_j.txt"

id1 is async.http_get(url_a)
id2 is async.http_request({method: "GET", url: url_b})
id3 is async.http_get(url_c)
id4 is async.http_get(url_d)
id5 is async.http_get(url_e)
id6 is async.http_get(url_f)
id7 is async.http_get(url_g)
id8 is async.http_get(url_h)
id9 is async.http_get(url_i)
id10 is async.http_get(url_j)

status1 is async.status(id1)
assert(status1.known)
assert(length(async.pending()) >= 1)
out1 is async.await(id1)
assert_contains(out1, "alpha")
assert(not async.status(id1).known)

all_outputs is async.all([id2, id3])
assert_eq(length(all_outputs), 2)
assert_contains(all_outputs[0], "bravo")
assert_contains(all_outputs[1], "charlie")

first is async.any([id4, id5], 1000, {timeout: true})
assert(first.id == id4 or first.id == id5)
assert(first.index == 0 or first.index == 1)
assert(first.output.contains("delta") or first.output.contains("echo"))
if first.id == id4 then
  rest is async.await(id5)
  assert_contains(rest, "echo")
else
  rest is async.await(id4)
  assert_contains(rest, "delta")
end

out6 is async.await_timeout(id6, 1000, "timeout")
assert_contains(out6, "foxtrot")

timer1 is async.sleep(250)
assert(async.status(timer1).running)
assert_eq(async.await_timeout(timer1, 1, "{}"), "{}")
assert(async.status(timer1).known)
sleep_ms(300)
assert(async.ready(timer1))
assert_eq(async.await(timer1), "")
assert(not async.status(timer1).known)

timer2 is async_sleep(1)
assert_eq(async_await(timer2), "")

wait(1)
sleep_ms(50)
assert(async.ready_all([id7, id8]))
out78 is async.all_timeout([id7, id8], 1000, ["timeout"])
assert_eq(length(out78), 2)
assert_contains(out78[0], "golf")
assert_contains(out78[1], "hotel")

sleep_ms(50)
assert(async.forget(id9))
assert(not async.status(id9).known)

sleep_ms(50)
removed is async.cleanup()
assert(removed >= 1)
assert(not async.status(id10).known)

print "async smoke: PASS"
'@

    Push-Location $temp
    try {
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $out = & $EnginePath --jit $script 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previousPreference
        }
    }
    finally {
        Pop-Location
    }

    $text = $out -join "`n"
    if ($code -ne 0 -or $text -notmatch "async smoke: PASS") {
        Write-Output $text
        throw "expected async smoke to pass"
    }

    "async_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
