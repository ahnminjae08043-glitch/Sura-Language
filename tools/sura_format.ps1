param(
    [Parameter(Mandatory=$true)][string]$Path,
    [switch]$Check
)

$lines = Get-Content -LiteralPath $Path
$indent = 0
$out = New-Object System.Collections.Generic.List[string]

foreach ($raw in $lines) {
    $trim = $raw.Trim()
    if ($trim -match '^(end|else|elif|catch)\b') {
        $indent = [Math]::Max(0, $indent - 1)
    }
    if ($trim.Length -eq 0) {
        $out.Add("")
    } else {
        $out.Add(("  " * $indent) + $trim)
    }
    if ($trim -match '\b(do|then)\s*$' -or $trim -match '^(try|else|catch\b)') {
        $indent++
    }
}

$formatted = ($out -join [Environment]::NewLine) + [Environment]::NewLine
$original = (Get-Content -LiteralPath $Path -Raw)

if ($Check) {
    if ($formatted -ne $original) {
        Write-Error "$Path is not formatted"
        exit 1
    }
    Write-Host "[OK] formatted: $Path"
    exit 0
}

Set-Content -LiteralPath $Path -Value $formatted -Encoding UTF8
Write-Host "[OK] formatted: $Path"
