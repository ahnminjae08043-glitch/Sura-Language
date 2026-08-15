param(
    [string]$Surapkg = ".\surapkg.exe"
)

$ErrorActionPreference = "Stop"

$SurapkgPath = (Resolve-Path $Surapkg).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_clean_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Write-Text {
    param([string]$Path, [string]$Text)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function Run-Pkg {
    param([string[]]$PkgArgs)
    $old = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $out = & $SurapkgPath @PkgArgs 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $old
    return @{ Code = $code; Output = ($out -join "`n") }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $root = Join-Path $temp "project"
    New-Item -ItemType Directory -Force -Path $root | Out-Null

    Write-Text (Join-Path $root "build_output.txt") "compiler output"
    Write-Text (Join-Path $root "surapkg_build_output.txt") "package compiler output"
    Write-Text (Join-Path $root "sura_world_log_123.jsonl") "{}"
    Write-Text (Join-Path $root "sura_world_tool_tmp.txt") "tool"
    Write-Text (Join-Path $root "src/main.sura") "print `"keep`""
    Write-Text (Join-Path $root "bench_full.sura") "print `"keep benchmark`""
    Write-Text (Join-Path $root "sura_walk_123/nested/item.txt") "walk"

    $dryReport = Join-Path $temp "clean-dry.json"
    $dry = Run-Pkg -PkgArgs @("clean", $root, "--dry-run", "--json", $dryReport)
    if ($dry.Code -ne 0 -or
        $dry.Output -notmatch "clean report written" -or
        $dry.Output -notmatch "clean would remove 5 item") {
        Write-Output $dry.Output
        throw "expected clean dry-run to report five generated items"
    }
    foreach ($rel in @("build_output.txt", "surapkg_build_output.txt", "sura_world_log_123.jsonl", "sura_world_tool_tmp.txt", "sura_walk_123")) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $rel))) {
            throw "dry-run should not remove $rel"
        }
    }
    $dryJson = Get-Content -Raw -Path $dryReport | ConvertFrom-Json
    if ($dryJson.schema -ne "sura.package.clean.v1" -or
        $dryJson.dry_run -ne $true -or
        $dryJson.matched -ne 5 -or
        $dryJson.removed -ne 0 -or
        @($dryJson.items).Count -ne 5 -or
        -not ($dryJson.items | Where-Object { $_.path -eq "sura_walk_123" -and $_.kind -eq "directory" -and $_.removed -eq $false })) {
        $dryJson | ConvertTo-Json -Depth 8
        throw "unexpected clean dry-run JSON report"
    }

    $reportPath = Join-Path $temp "clean.json"
    $clean = Run-Pkg -PkgArgs @("clean", $root, "--json=$reportPath")
    if ($clean.Code -ne 0 -or
        $clean.Output -notmatch "clean removed 5 item" -or
        -not (Test-Path -LiteralPath $reportPath)) {
        Write-Output $clean.Output
        throw "expected clean to remove generated items and write JSON"
    }
    foreach ($rel in @("build_output.txt", "surapkg_build_output.txt", "sura_world_log_123.jsonl", "sura_world_tool_tmp.txt", "sura_walk_123")) {
        if (Test-Path -LiteralPath (Join-Path $root $rel)) {
            throw "clean should remove $rel"
        }
    }
    foreach ($rel in @("src/main.sura", "bench_full.sura")) {
        if (-not (Test-Path -LiteralPath (Join-Path $root $rel))) {
            throw "clean should preserve source file $rel"
        }
    }
    $report = Get-Content -Raw -Path $reportPath | ConvertFrom-Json
    if ($report.schema -ne "sura.package.clean.v1" -or
        $report.dry_run -ne $false -or
        $report.matched -ne 5 -or
        $report.removed -ne 5 -or
        -not ($report.items | Where-Object { $_.path -eq "build_output.txt" -and $_.kind -eq "file" -and $_.removed -eq $true })) {
        $report | ConvertTo-Json -Depth 8
        throw "unexpected clean JSON report"
    }

    $again = Run-Pkg -PkgArgs @("clean", $root)
    if ($again.Code -ne 0 -or $again.Output -notmatch "clean removed 0 item") {
        Write-Output $again.Output
        throw "expected second clean to be a no-op"
    }

    "clean_smoke: PASS"
} finally {
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

# Verified passing; state the exit code rather than inheriting it.
exit 0
