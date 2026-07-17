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

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Host $_ }
    throw "text encoding smoke found likely mojibake"
}

Write-Host "sura_text_encoding_smoke: PASS"
