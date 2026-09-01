param(
    [string]$Engine = ""
)

$ErrorActionPreference = "Stop"

$script:SyntaxCaseId = 0
$script:SyntaxStats = [ordered]@{
    markdown_documents = 0
    markdown_sura_blocks = 0
    reference_sura_blocks = 0
    website_sura_examples = 0
    published_example_files = 0
    mirrored_files = 0
}

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Engine)) {
    $localEngine = Join-Path $root "SuraLanguage.exe"
    if (Test-Path -LiteralPath $localEngine) {
        $Engine = $localEngine
    } else {
        $cmd = Get-Command sura -ErrorAction SilentlyContinue
        if (-not $cmd) { throw "Sura engine not found; pass -Engine" }
        $Engine = $cmd.Source
    }
}

function Test-SuraRuntimeProbe {
    param(
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][string]$TempRoot
    )

    $probePath = Join-Path $TempRoot "guide_runtime_probe.sura"
    $outPath = Join-Path $TempRoot "guide_runtime_probe.out"
    $probeSource = @'
name is "Sura"
score is 10
player is {name: "Ari", hp: 100}
items is ["red", "green"]

items.insert(1, "yellow")
removed_item is items.remove(2)

assert_eq("name={player["name"]}, hp={player.hp}", "name=Ari, hp=100")
assert_eq(items.join(","), "red,yellow")
assert_eq(removed_item, "green")
assert_eq(items.remove(99), nil)

use array
use dict
numbers is [3, 1, 2]
numbers_copy is array.clone(numbers)
numbers_copy[0] is 9
numbers_copy_alias is array.copy(numbers)
numbers_copy_alias[1] is 8
assert_eq(numbers.join(","), "3,1,2")
assert_eq(numbers_copy.join(","), "9,1,2")
assert_eq(numbers_copy_alias.join(","), "3,8,2")
assert_eq(array.sort(array.clone(numbers)).join(","), "1,2,3")
assert_eq(array.range(2, 7, 2).join(","), "2,4,6")
assert_eq(array.unique([1, 1, 2]).join(","), "1,2")

meta is {name: "sura", kind: "lang", score: 9}
assert_eq(meta.keys().contains("name"), true)
assert_eq(dict.values({a: 1, b: 2}).contains(1), true)
dict_item is dict.items({a: 1})[0]
assert_eq(dict_item.key, "a")
assert_eq(dict_item.value, 1)
merged_meta is dict.merge({a: 1}, {b: 2}, {a: 3})
assert_eq(merged_meta.a, 3)
assert_eq(merged_meta.b, 2)
picked_meta is dict.pick(meta, ["name", "missing"])
assert_eq(picked_meta.name, "sura")
omitted_meta is dict.omit(meta, ["kind"])
assert_eq(omitted_meta.name, "sura")
assert_eq(omitted_meta.score, 9)

total is 0
for n in 1 to 3 do
  total += n
end
assert_eq(total, 6)

func add(a, b) do
  return a + b
end
assert_eq(add(2, 3), 5)

class Box do
  func init(value) do
    self.value is value
  end
end
box is new Box("ok")
assert_eq(box.value, "ok")

label is ""
match score
  when 10 then
    label is "ten"
  when _ then
    label is "other"
end
assert_eq(label, "ten")

caught is ""
done is false
try
  throw "failed"
catch e
  caught is e
finally do
  done is true
end
assert_eq(caught, "failed")
assert_eq(done, true)
sleep 1
assert_eq(type(sleep(1)), "nil")
'@

    [System.IO.File]::WriteAllText($probePath, $probeSource, [System.Text.UTF8Encoding]::new($false))
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $EnginePath $probePath *> $outPath
        $probeExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($probeExitCode -ne 0) {
        $output = [System.IO.File]::ReadAllText($outPath, [System.Text.Encoding]::UTF8)
        throw "guide runtime probe failed with exit code $probeExitCode.`n$output"
    }
}

function Invoke-SuraSourceCheck {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][string]$TempRoot
    )

    $script:SyntaxCaseId++
    $outPath = Join-Path $TempRoot ("syntax_case_{0:D4}.out" -f $script:SyntaxCaseId)
    $previousPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $EnginePath --check $SourcePath *> $outPath
        $sourceExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($sourceExitCode -ne 0) {
        $output = [System.IO.File]::ReadAllText($outPath, [System.Text.Encoding]::UTF8)
        throw "$Label does not parse and typecheck with the current engine.`n$output"
    }
}

function Test-SuraSourceText {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][string]$TempRoot
    )

    $sourcePath = Join-Path $TempRoot ("syntax_source_{0:D4}.sura" -f ($script:SyntaxCaseId + 1))
    [System.IO.File]::WriteAllText($sourcePath, $Source, [System.Text.UTF8Encoding]::new($false))
    Invoke-SuraSourceCheck -SourcePath $sourcePath -Label $Label -EnginePath $EnginePath -TempRoot $TempRoot
}

function Test-SuraDocCodeBlocks {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][string]$TempRoot
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "guide document not found: $Path"
    }

    $docText = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    $blocks = [regex]::Matches($docText, '(?ms)^```sura\s*\r?\n(.*?)^```\s*$')
    $relative = Resolve-Path -LiteralPath $Path -Relative
    $script:SyntaxStats.markdown_documents++
    for ($i = 0; $i -lt $blocks.Count; $i++) {
        $blockText = $blocks[$i].Groups[1].Value
        Test-SuraSourceText -Source $blockText -Label "Sura code block $i in $relative" -EnginePath $EnginePath -TempRoot $TempRoot
        $script:SyntaxStats.markdown_sura_blocks++
    }
}

function Test-SuraReferenceCodeBlocks {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][string]$TempRoot
    )

    $html = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    $expectations = @(
        @{ Section = "lexical"; Prefix = "# comment" },
        @{ Section = "control"; Prefix = "if score >= 90 then" },
        @{ Section = "control"; Prefix = "while ready do" },
        @{ Section = "control"; Prefix = "match status" },
        @{ Section = "functions"; Prefix = "func add(" },
        @{ Section = "functions"; Prefix = "func make_counter() do" },
        @{ Section = "objects"; Prefix = "class Animal do" },
        @{ Section = "objects"; Prefix = "struct Vec2 do" },
        @{ Section = "errors"; Prefix = "try" },
        @{ Section = "async"; Prefix = "use async" },
        @{ Section = "ai"; Prefix = "use autograd" },
        @{ Section = "media"; Prefix = "use media" }
    )

    foreach ($expectation in $expectations) {
        $sectionPattern = '(?is)<section\s+id=[''"]' + [regex]::Escape($expectation.Section) + '[''"]>(.*?)</section>'
        $sectionMatch = [regex]::Match($html, $sectionPattern)
        if (-not $sectionMatch.Success) {
            throw "reference section is missing: $($expectation.Section)"
        }

        $matchingBlocks = @()
        foreach ($block in [regex]::Matches($sectionMatch.Groups[1].Value, '(?is)<pre><code(?:\s+[^>]*)?>(.*?)</code></pre>')) {
            $decoded = [System.Net.WebUtility]::HtmlDecode($block.Groups[1].Value).TrimStart()
            if ($decoded.StartsWith($expectation.Prefix, [System.StringComparison]::Ordinal)) {
                $matchingBlocks += $decoded
            }
        }
        if ($matchingBlocks.Count -ne 1) {
            throw "reference Sura example must appear exactly once: section=$($expectation.Section), prefix=$($expectation.Prefix), found=$($matchingBlocks.Count)"
        }

        Test-SuraSourceText -Source $matchingBlocks[0] -Label "reference example $($expectation.Section)/$($expectation.Prefix)" -EnginePath $EnginePath -TempRoot $TempRoot
        $script:SyntaxStats.reference_sura_blocks++
    }
}

function Test-SuraWebsiteExamples {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][string]$TempRoot
    )

    $jsx = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    $declaredCount = [regex]::Matches($jsx, 'filename:\s*"[^"\r\n]+\.sura"').Count
    $examples = [regex]::Matches(
        $jsx,
        '(?ms)filename:\s*"(?<name>[^"\r\n]+\.sura)"\s*,.*?code:\s*\[(?<body>.*?)\]\.join\("\\n"\)'
    )
    if ($declaredCount -eq 0 -or $examples.Count -ne $declaredCount) {
        throw "website Sura example extraction is incomplete: declared=$declaredCount, extracted=$($examples.Count)"
    }

    foreach ($example in $examples) {
        $sourceLines = New-Object System.Collections.Generic.List[string]
        foreach ($line in [regex]::Matches($example.Groups["body"].Value, '(?m)^\s*(?<json>"(?:\\.|[^"\\])*")\s*,?\s*$')) {
            $sourceLines.Add(($line.Groups["json"].Value | ConvertFrom-Json))
        }
        if ($sourceLines.Count -eq 0) {
            throw "website Sura example contains no static source lines: $($example.Groups['name'].Value)"
        }

        $source = $sourceLines -join "`n"
        Test-SuraSourceText -Source $source -Label "website example $($example.Groups['name'].Value)" -EnginePath $EnginePath -TempRoot $TempRoot
        $script:SyntaxStats.website_sura_examples++
    }
}

function Test-SuraPublishedExamples {
    param(
        [Parameter(Mandatory = $true)][string[]]$Roots,
        [Parameter(Mandatory = $true)][string]$EnginePath,
        [Parameter(Mandatory = $true)][string]$TempRoot
    )

    $files = @($Roots | ForEach-Object {
        if (-not (Test-Path -LiteralPath $_)) { throw "published example root is missing: $_" }
        Get-ChildItem -LiteralPath $_ -Filter "*.sura" -File -Recurse
    } | Sort-Object FullName -Unique)
    if ($files.Count -eq 0) { throw "no published Sura examples were found" }

    foreach ($file in $files) {
        $relative = Resolve-Path -LiteralPath $file.FullName -Relative
        Invoke-SuraSourceCheck -SourcePath $file.FullName -Label "published example $relative" -EnginePath $EnginePath -TempRoot $TempRoot
        $script:SyntaxStats.published_example_files++
    }
}

function Test-MirroredFileSet {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$MirrorRoot,
        [Parameter(Mandatory = $true)][string[]]$Names
    )

    foreach ($name in $Names) {
        $sourcePath = Join-Path $SourceRoot $name
        $mirrorPath = Join-Path $MirrorRoot $name
        if (-not (Test-Path -LiteralPath $sourcePath) -or -not (Test-Path -LiteralPath $mirrorPath)) {
            throw "mirrored public file is missing: $name"
        }
        $sourceHash = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash
        $mirrorHash = (Get-FileHash -LiteralPath $mirrorPath -Algorithm SHA256).Hash
        if ($sourceHash -ne $mirrorHash) {
            throw "mirrored public file differs from its source: $name"
        }
        $script:SyntaxStats.mirrored_files++
    }
}

$tmpRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_guide_syntax_" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $tmpRoot | Out-Null
$scriptPath = Join-Path $tmpRoot "guide_syntax.sura"

$source = @'
name is "Sura"
count is 3
enabled is true
missing is nil
score is 10
score += 5

print("name = {name}")
print("score = {score}")

player is {name: "Ari", score: 10}
print("name={player["name"]}, score={player.score}")

if score > 10 then
  print("large")
elif score == 10 then
  print("same")
else
  print("small")
end

i is 0
while i < 2 do
  i += 1
end

repeat 2 do
  score += 1
end

for n in 1 to 3 do
  score += n
end

for n in 2 to 6 step 2 do
  score += n
end

for n in 1 ~ 3 do
  score += n
end

items is ["red", "green", "blue"]
items.insert(1, "yellow")
removed_item is items.remove(2)
assert_eq(items.join(","), "red,yellow,blue")
assert_eq(removed_item, "green")
assert_eq(items.remove(99), nil)

for item in items do
  print(item)
end

for index, item in items do
  print("{index}: {item}")
end

empty is []
for item in empty do
  score += 100
end

profile is {city: "Seoul", level: 1}
for key, value in profile do
  print("{key} = {value}")
end

use dict
meta is {name: "sura", kind: "lang", score: 9}
print(meta.keys().contains("name"))
print(dict.values({a: 1, b: 2}).contains(1))

dict_item is dict.items({a: 1})[0]
print("{dict_item.key}={dict_item.value}")

merged_meta is dict.merge({a: 1}, {b: 2}, {a: 3})
picked_meta is dict.pick(meta, ["name", "missing"])
omitted_meta is dict.omit(meta, ["kind"])

print(merged_meta.a)
print(picked_meta.name)
print(omitted_meta.score)

loop_check is 0
while true do
  loop_check += 1
  if loop_check == 2 then continue
  if loop_check == 4 then break
end

text is "  Sura Language  "
print(text.trim())
print(text.lower())
print(text.upper())
print(text.contains("Lang"))
print(text.starts_with("  Su"))
print(text.ends_with("  "))
parts is text.split(" ")
print(parts.len())

func add(a, b) do
  return a + b
end

func sign(n) do
  if n > 0 then return 1
  if n < 0 then return -1
  return 0
end

handler is add
print(handler(2, 3))

class Player do
  func init(name) do
    self.name is name
    self.hp is 100
  end

  func damage(amount) do
    self.hp -= amount
    return self.hp
  end
end

p is new Player("hero")
print(p.damage(20))

struct Vec2 do
  x
  y

  func add(other) do
    return Vec2(self.x + other.x, self.y + other.y)
  end
end

v is Vec2(3, 4)
print(v.x)
print(v.add(Vec2(1, 2)).y)

enum Mode do
  EASY
  HARD
end

mode is Mode.EASY
print(mode)

match score
  when 1 then
    print("one")
  when _ then
    print("other")
end

when score do
in 1 ~ 100 then
  print("range")
else then
  print("outside")
end

try
  throw "failed"
catch e
  print("error: {e}")
finally do
  print("done")
end

console.log("ready", 1)
console.warn("check config")
console.write("progress")
console.write_line(" 10%")
console.json({ok: true, score: 10})

pressed is key_down("space")
last_key is readkey_timeout(16)
key_down "w" w_pressed
readkey_timeout input_key 16

grid_init 10 5
grid_clear
grid_set 2 2 "@" "green"
grid_draw

grid_init(10, 5)
grid_clear()
grid_set(2, 2, "#", "cyan")
grid_draw()

mouse is mouse_pos()
mouse_pos mx my
left_down is mouse_down("left")
mouse_down "left" left_pressed

if win_init(320, 200, "Guide Smoke") then
  win_focus()
  win_clear(8, 12, 20)
  win_rect(20, 20, 40, 30, 50, 160, 220)
  win_circle(80, 60, 10, 240, 90, 90)
  win_line(0, 0, 319, 199, 255, 255, 255)
  win_text("Sura", 10, 10, 255, 255, 255)
  win_poll()
  win_update()
end

use math
use os
use random
use test
use vector

print(math.clamp(120, 0, 100))
random.seed(42)
print(random.int(1, 100))
print(vector.cross([1, 0, 0], [0, 1, 0]))
assert_eq(1 + 1, 2)
assert_contains("sura language", "sura")
assert_type({ok: true}, "dict")
wait(1)
sleep_ms(1)
sleep(1)
sleep 1
'@

[System.IO.File]::WriteAllText($scriptPath, $source, [System.Text.UTF8Encoding]::new($false))

try {
    & $Engine --check $scriptPath
    if ($LASTEXITCODE -ne 0) { throw "guide syntax check failed with exit code $LASTEXITCODE" }

    $rootDocs = @(
        "README.md",
        "GUIDE.md",
        "COMPREHENSIVE_GUIDE_V3.md",
        "PACKAGE_MANAGER.md",
        "CONTRIBUTING.md",
        "EXTERNAL_LIBRARIES_GUIDE.md",
        "COMPATIBILITY.md",
        "SCOPE.md"
    ) | ForEach-Object { Join-Path $root $_ }
    $guideDocs = @(Get-ChildItem -LiteralPath (Join-Path $root "Guide") -Filter "*.md" -File | Select-Object -ExpandProperty FullName)
    $otherDocs = @(
        (Join-Path $root "examples\starter\README.md"),
        (Join-Path $root "sura-vscode\README.md"),
        (Join-Path $root "sura_presentation\public\examples\starter\README.md")
    )
    $publicDocs = @($rootDocs + $guideDocs + $otherDocs | Sort-Object -Unique)
    foreach ($docPath in $publicDocs) {
        Test-SuraDocCodeBlocks -Path $docPath -EnginePath $Engine -TempRoot $tmpRoot
    }

    Test-SuraReferenceCodeBlocks -Path (Join-Path $root "reference.html") -EnginePath $Engine -TempRoot $tmpRoot
    Test-SuraWebsiteExamples -Path (Join-Path $root "sura_presentation\src\main.jsx") -EnginePath $Engine -TempRoot $tmpRoot
    Test-SuraPublishedExamples -Roots @(
        (Join-Path $root "examples"),
        (Join-Path $root "sura_presentation\public\examples")
    ) -EnginePath $Engine -TempRoot $tmpRoot

    Test-MirroredFileSet `
        -SourceRoot (Join-Path $root "examples\starter") `
        -MirrorRoot (Join-Path $root "sura_presentation\public\examples\starter") `
        -Names @(
            "01_hello.sura", "02_values.sura", "03_control_flow.sura", "04_functions.sura",
            "05_collections.sura", "06_classes.sura", "07_files.sura", "08_json.sura",
            "09_http_helpers.sura", "10_async.sura", "11_errors.sura", "12_testing.sura",
            "README.md"
        )
    Test-MirroredFileSet `
        -SourceRoot $root `
        -MirrorRoot (Join-Path $root "sura_presentation\public") `
        -Names @("reference.html")

    $siteSource = [System.IO.File]::ReadAllText((Join-Path $root "sura_presentation\src\main.jsx"), [System.Text.Encoding]::UTF8)
    $siteRelease = Get-Content -LiteralPath (Join-Path $root "sura_presentation\src\release.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    $versionContract = Get-Content -LiteralPath (Join-Path $root "version.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($siteRelease.version -ne $versionContract.version) {
        throw "website release metadata does not match version.json"
    }
    $releaseVerification = Get-Content `
        -LiteralPath (Join-Path $root "sura_presentation\public\downloads\verification-$($siteRelease.version).json") `
        -Raw -Encoding UTF8 | ConvertFrom-Json
    $vmResult = [string]$releaseVerification.results.stable_vm -replace " PASS$", ""
    $jitResult = [string]$releaseVerification.results.stable_jit -replace " PASS$", ""
    foreach ($expectedSummary in @("VM $vmResult", "JIT $jitResult")) {
        if (-not $siteSource.Contains($expectedSummary)) {
            throw "website verification summary does not match the public release manifest: $expectedSummary"
        }
    }
    $bytecodeRow = [regex]::Match($siteSource, '(?m)\["Bytecode validation",\s*"(?<claim>[^"]+)"\]')
    if ([string]$releaseVerification.results.bytecode_validation -like "not rerun*" -and
        (-not $bytecodeRow.Success -or
         -not $bytecodeRow.Groups["claim"].Value.Contains("Stable VM/JIT") -or
         $bytecodeRow.Groups["claim"].Value.Contains("19/19"))) {
        throw "website must not claim a standalone bytecode result that the public release manifest did not verify"
    }
    $ffiRow = [regex]::Match($siteSource, '(?m)\["Async / FFI",\s*"(?<claim>[^"]+)"\]')
    if ([string]$releaseVerification.results.ffi_safety -like "not rerun*" -and
        (-not $ffiRow.Success -or
         -not $ffiRow.Groups["claim"].Value.Contains("Stable VM/JIT") -or
         $ffiRow.Groups["claim"].Value.Contains("safety suite PASS"))) {
        throw "website must not claim a standalone FFI result that the public release manifest did not verify"
    }

    Test-SuraRuntimeProbe -EnginePath $Engine -TempRoot $tmpRoot

    $docChecks = @(
        @{ Path = (Join-Path $root "COMPREHENSIVE_GUIDE_V3.md"); Forbidden = @("SuraEngine2.exe", "http_get url response", "arr_len data rows", "arr_get data i row", "win_draw", "Sura v3.0", "이제 모든 것이 가능합니다", "# Sura 최신 문법 가이드") },
        @{ Path = (Join-Path $root "reference.html"); Forbidden = @("print type([1, 2])", "inc score`ndec score", "<code>try do", "<code>catch err do") }
    )

    foreach ($docCheck in $docChecks) {
        if (-not (Test-Path -LiteralPath $docCheck.Path)) {
            throw "guide document not found: $($docCheck.Path)"
        }
        $docText = [System.IO.File]::ReadAllText($docCheck.Path, [System.Text.Encoding]::UTF8)
        foreach ($forbidden in $docCheck.Forbidden) {
            if ($docText.Contains($forbidden)) {
                throw "stale guide syntax remains in $($docCheck.Path): $forbidden"
            }
        }
    }
} finally {
    Remove-Item -LiteralPath $tmpRoot -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("sura_guide_syntax_smoke: PASS ({0} Markdown documents, {1} Sura fences, {2} reference examples, {3} website examples, {4} published files, {5} mirrored files)" -f `
    $script:SyntaxStats.markdown_documents,
    $script:SyntaxStats.markdown_sura_blocks,
    $script:SyntaxStats.reference_sura_blocks,
    $script:SyntaxStats.website_sura_examples,
    $script:SyntaxStats.published_example_files,
    $script:SyntaxStats.mirrored_files)

# Verified passing; state the exit code rather than inheriting it.
exit 0
