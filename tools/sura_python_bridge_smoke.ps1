param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$EnginePath = (Resolve-Path $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_python_bridge_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Find-Python {
    $candidates = @(
        $env:SURA_PYTHON,
        "python",
        "python3",
        "C:\msys64\mingw64\bin\python.exe",
        "C:\msys64\ucrt64\bin\python.exe"
    ) | Where-Object { $_ }
    foreach ($candidate in $candidates) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $out = & $candidate --version 2>&1 | ForEach-Object { "$_" }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $old
        if ($code -eq 0 -and (($out -join " ") -match "Python\s+\d+\.\d+")) {
            return (Get-Command $candidate -ErrorAction SilentlyContinue).Source
        }
    }
    return ""
}

$oldPython = $env:SURA_PYTHON
$oldPythonPath = $env:PYTHONPATH
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $python = Find-Python
    if (-not $python) {
        throw "Python executable not found for bridge smoke"
    }
    $env:SURA_PYTHON = $python
    $pathSeparator = [System.IO.Path]::PathSeparator
    $env:PYTHONPATH = if ($oldPythonPath) { "$temp$pathSeparator$oldPythonPath" } else { $temp }

    Write-Text (Join-Path $temp "sura_py_pkg.py") @"
def score(name, value):
    return {"name": name, "score": value * 2}

def join_words(*items, sep="-"):
    return sep.join(items)
"@

    $script = Join-Path $temp "python_bridge_smoke.sura"
    Write-Text $script @"
use python

assert(python.available())
assert(contains(python.executable(), "python"))

assert_eq(python.call_json("math", "sqrt", [81]), 9)
assert_eq(python.call_json("operator", "add", [20, 22]), 42)

score is python.call_json("sura_py_pkg", "score", ["agent", 7])
assert_eq(score.name, "agent")
assert_eq(score.score, 14)

joined is python.call_json("sura_py_pkg", "join_words", ["sura", "python"], {sep: ":"})
assert_eq(joined, "sura:python")

raw is python.call("json", "dumps", [{"ok": true}])
assert(contains(raw, "ok"))

print "python_bridge_smoke: PASS"
"@

    $oldAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $EnginePath --jit $script 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $oldAction
    $text = $out -join "`n"
    if ($code -ne 0 -or $text -notmatch "python_bridge_smoke: PASS") {
        Write-Output $text
        throw "expected python bridge smoke to pass"
    }

    "python_bridge_smoke: PASS"
}
finally {
    $env:SURA_PYTHON = $oldPython
    $env:PYTHONPATH = $oldPythonPath
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
