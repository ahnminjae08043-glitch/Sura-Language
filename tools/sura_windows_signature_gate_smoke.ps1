param(
    [string]$RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Gate = (Join-Path $PSScriptRoot "sura_windows_signature_gate.ps1")
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path -LiteralPath $RepoRoot).Path
$gatePath = (Resolve-Path -LiteralPath $Gate).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_signature_gate_" + [Guid]::NewGuid().ToString("N"))
$powershell = (Get-Command powershell -ErrorAction Stop).Source

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $reportPath = Join-Path $temp "signature.json"
    & $powershell -NoProfile -ExecutionPolicy Bypass -File $gatePath -RepoRoot $root -JsonOut $reportPath | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "informational Windows signature gate failed" }
    $report = [System.IO.File]::ReadAllText($reportPath, [System.Text.Encoding]::UTF8) | ConvertFrom-Json
    if ($report.schema -ne "sura.windows.signature.report.v1" -or
        $report.status -ne "pass" -or
        @($report.files).Count -lt 3 -or
        [int]$report.valid_count + [int]$report.unsigned_count -gt @($report.files).Count) {
        $report | ConvertTo-Json -Depth 7
        throw "unexpected Windows signature report"
    }

    $strictOut = Join-Path $temp "strict.stdout.txt"
    $strictErr = Join-Path $temp "strict.stderr.txt"
    $strict = Start-Process -FilePath $powershell -ArgumentList @(
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $gatePath,
        "-RepoRoot", $root, "-RequireSigned"
    ) -RedirectStandardOutput $strictOut -RedirectStandardError $strictErr -Wait -PassThru -WindowStyle Hidden
    if ([int]$report.unsigned_count -gt 0 -and $strict.ExitCode -eq 0) {
        throw "strict Windows signature gate should reject unsigned direct downloads"
    }
    if ([int]$report.unsigned_count -eq 0 -and $strict.ExitCode -ne 0) {
        throw "strict Windows signature gate should accept fully valid signatures"
    }

    $signerText = [System.IO.File]::ReadAllText((Join-Path $root "tools/sura_sign_windows.ps1"), [System.Text.Encoding]::UTF8)
    foreach ($required in @("SURA_CODESIGN_PFX_PASSWORD", "Set-AuthenticodeSignature", "TimestampServer", "self-signed certificates are test-only")) {
        if (-not $signerText.Contains($required)) { throw "Windows signer is missing: $required" }
    }

    "sura_windows_signature_gate_smoke: PASS (valid=$($report.valid_count), unsigned=$($report.unsigned_count))"
}
finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
