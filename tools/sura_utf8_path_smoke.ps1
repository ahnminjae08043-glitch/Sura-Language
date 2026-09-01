param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_utf8_path_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $script = Join-Path $temp "utf8_path.sura"
    $koDir = -join ([char[]](0xD55C, 0xAE00, 0x005F, 0xACBD, 0xB85C))
    $koFile = -join ([char[]](0xD30C, 0xC77C, 0x002E, 0x0074, 0x0078, 0x0074))
    $koCopy = -join ([char[]](0xBCF5, 0xC0AC, 0x002E, 0x0074, 0x0078, 0x0074))
    $koMoved = -join ([char[]](0xC774, 0xB3D9, 0x002E, 0x0074, 0x0078, 0x0074))
    $koContent = -join ([char[]](0xB0B4, 0xC6A9))
    $koAppend = -join ([char[]](0xCD94, 0xAC00))
    Write-Text $script @"
dir is "$koDir"
name is path_join(dir, "$koFile")
copy is path_join(dir, "$koCopy")
moved is path_join(dir, "$koMoved")
mkdir(dir)
file_write(name, "$koContent")
file_append(name, "\n$koAppend")
assert_eq(file_read(name), "$koContent\n$koAppend")
assert(file_size(name) > 0)
assert_contains(file_list(dir), "$koFile")
assert_eq(path_basename(file_walk(dir, ".txt")[0]), "$koFile")
info is file_info(name)
assert_eq(info.name, "$koFile")
assert_eq(info.stem, "$($koFile.Substring(0, 2))")
assert(info.is_file)
assert_eq(path_relative(path_abs(name), cwd()), name)
assert(file_copy(name, copy))
assert(file_move(copy, moved))
assert(file_exists(moved))
assert(file_delete(name))
assert(file_delete(moved))
assert(file_delete(dir))
print "utf8_path_smoke: PASS"
"@

    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $Engine $script 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    $text = $out -join "`n"
    if ($code -ne 0 -or $text -notmatch "utf8_path_smoke: PASS") {
        Write-Output $text
        throw "expected UTF-8 path smoke to pass"
    }

    # Second scenario: `import` from inside a non-ASCII directory. The block
    # above passed for the entire time `import` was broken there, because it
    # only exercises the stdlib file APIs - the compiler resolves import paths
    # on its own and used fs::path's ANSI narrow constructor, so any project
    # under a directory like 문서\ could not import at all. Keep this covered
    # separately from the stdlib case; they fail independently.
    $importDir = Join-Path $temp (-join ([char[]](0xD55C, 0xAE00, 0xD3F4, 0xB354)))
    $libPath = Join-Path $importDir "lib.sura"
    $mainPath = Join-Path $importDir "main.sura"
    Write-Text $libPath @"
func lib_value() do
  return 7
end
"@
    Write-Text $mainPath @"
import "lib.sura"

assert_eq(lib_value(), 7)
print "utf8_import_smoke: PASS"
"@

    $ErrorActionPreference = "Continue"
    $importOut = & $Engine $mainPath 2>&1 | ForEach-Object { "$_" }
    $importCode = $LASTEXITCODE
    $ErrorActionPreference = $old
    $importText = $importOut -join "`n"
    if ($importCode -ne 0 -or $importText -notmatch "utf8_import_smoke: PASS") {
        Write-Output $importText
        throw "expected import from a non-ASCII directory to resolve"
    }

    "utf8_path_smoke: PASS"
}
finally {
    if (Test-Path $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# Verified passing; state the exit code rather than inheriting it.
exit 0
