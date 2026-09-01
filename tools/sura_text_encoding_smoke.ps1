param(
    [string]$RepoRoot = "."
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$utf8Strict = New-Object System.Text.UTF8Encoding($false, $true)

$documentationFiles = @(
    "COMPATIBILITY.md",
    "CONTRIBUTING.md",
    "SECURITY.md",
    "SECURITY_AUDIT.md",
    "GUIDE.md",
    "SCOPE.md",
    "Guide/GUIDE.md",
    "Guide/AI.md",
    "Guide/AUTOGRAD.md",
    "Guide/ECOSYSTEM.md",
    "Guide/GPU_AND_SCALE.md",
    "Guide/INSTALL.md",
    "Guide/TRANSFORMER.md",
    "Guide/WORLD_CLASS_ROADMAP.md",
    "benchmarks/README.md",
    "reference.html",
    "sura-vscode/CHANGELOG.md",
    "sura-vscode/README.md",
    "tools/sura_reference_generate.mjs",
    "tools/sura_reference_freshness_smoke.ps1",
    "tools/sura_security_audit_bundle.ps1",
    "tools/sura_security_audit_bundle_smoke.ps1"
)

# Diagnostics are user-facing text too. Scan every root-level engine/package
# source file so new diagnostics cannot bypass this gate through an allowlist.
$sourceFiles = Get-ChildItem -LiteralPath $root -File | Where-Object {
    $_.Extension -in @(".cpp", ".hpp")
} | Sort-Object -Property Name | ForEach-Object { $_.Name }

$files = @($documentationFiles) + @($sourceFiles) | Select-Object -Unique

$failures = New-Object System.Collections.Generic.List[string]

foreach ($rel in $files) {
    $path = Join-Path $root $rel
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("${rel}: missing file") | Out-Null
        continue
    }

    try {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $text = $utf8Strict.GetString($bytes)
    } catch {
        $failures.Add("${rel}: invalid UTF-8 bytes") | Out-Null
        continue
    }

    $lines = $text -split "`r?`n"
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        $line = $lines[$lineIndex]
        $lineNo = $lineIndex + 1

        if ([regex]::IsMatch($line, '\?[\uAC00-\uD7A3]')) {
            $failures.Add("${rel}:${lineNo}: suspicious question-mark plus Hangul mojibake") | Out-Null
            continue
        }

        foreach ($ch in $line.ToCharArray()) {
            $code = [int][char]$ch
            if ($code -eq 0xFFFD) {
                $failures.Add("${rel}:${lineNo}: Unicode replacement character U+FFFD") | Out-Null
                break
            }
            if ($code -ge 0x80 -and $code -le 0x9F) {
                $failures.Add(("{0}:{1}: C1 control U+{2:X4}" -f $rel, $lineNo, $code)) | Out-Null
                break
            }
            if ($code -ge 0x4E00 -and $code -le 0x9FFF) {
                $failures.Add(("{0}:{1}: CJK mojibake candidate U+{2:X4}" -f $rel, $lineNo, $code)) | Out-Null
                break
            }
        }
    }
}

# Windows PowerShell 5.1 reads a BOM-less script in the machine's ANSI
# codepage, so a .ps1 holding Hangul or any other non-ASCII text is decoded
# into mojibake and can fail to even parse (a broken quote pair takes the rest
# of the file with it). Requiring the BOM keeps such scripts runnable under
# both 5.1 and PowerShell 7, on any codepage.
$scriptFiles = @(Get-ChildItem -LiteralPath (Join-Path $root "tools") -File -Filter *.ps1) +
               @(Get-ChildItem -LiteralPath $root -File -Filter *.ps1)
foreach ($script in ($scriptFiles | Sort-Object FullName)) {
    $bytes = [System.IO.File]::ReadAllBytes($script.FullName)
    $hasBom = $bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF
    if ($hasBom) { continue }
    try { $text = $utf8Strict.GetString($bytes) } catch {
        $failures.Add((Split-Path -Leaf $script.FullName) + ": invalid UTF-8 bytes") | Out-Null
        continue
    }
    if ($text.ToCharArray() | Where-Object { [int][char]$_ -gt 127 } | Select-Object -First 1) {
        $relative = $script.FullName.Substring($root.Length).TrimStart('\', '/')
        $failures.Add("${relative}: non-ASCII PowerShell script needs a UTF-8 BOM") | Out-Null
    }
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    throw "text encoding smoke found likely mojibake"
}

Write-Host "sura_text_encoding_smoke: PASS"

# Verified passing; state the exit code rather than inheriting it.
exit 0
