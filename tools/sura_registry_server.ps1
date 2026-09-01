param(
    [string]$Root = (Join-Path (Split-Path -Parent $PSScriptRoot) "registry"),
    [int]$Port = 8765,
    [string]$Token = "dev-token",
    [string]$AdminToken = "",
    [switch]$Static
)

if (-not (Test-Path -LiteralPath $Root)) {
    New-Item -ItemType Directory -Path $Root | Out-Null
}

$resolvedRoot = (Resolve-Path -LiteralPath $Root).Path

$node = Get-Command node -ErrorAction SilentlyContinue
if ($node -and -not $Static) {
    $server = Join-Path $PSScriptRoot "sura_registry_api.js"
    Write-Host "[OK] Starting tokenized Sura registry API"
    $args = @($server, "--root", $resolvedRoot, "--port", "$Port", "--token", $Token)
    if ($AdminToken) { $args += @("--admin-token", $AdminToken) }
    & $node.Source @args
    exit $LASTEXITCODE
}

$pythonCandidates = @("python", "py", "C:\msys64\mingw64\bin\python.exe")
$python = $null

foreach ($candidate in $pythonCandidates) {
    try {
        $version = & $candidate --version 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0 -and $version -match "Python") {
            $python = $candidate
            break
        }
    } catch {
    }
}

if (-not $python) {
    Write-Error "Python is required to serve the registry"
    exit 1
}

Write-Host "[OK] Sura static registry serving $resolvedRoot"
Write-Host "[OK] Set SURA_REGISTRY_URL=http://localhost:$Port"
& $python -m http.server $Port --directory $resolvedRoot
