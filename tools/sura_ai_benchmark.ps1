param(
    [string]$Engine = "",
    [string]$JsonOut = "",
    [string]$MarkdownOut = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Engine)) {
    $Engine = Join-Path $root "SuraLanguage.exe"
}
$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$benchmarkPath = Join-Path $root "benchmarks/ai_backend_benchmark.sura"
if ([string]::IsNullOrWhiteSpace($JsonOut)) {
    $JsonOut = Join-Path $root "artifacts/ai_benchmark.json"
}
if ([string]::IsNullOrWhiteSpace($MarkdownOut)) {
    $MarkdownOut = Join-Path $root "artifacts/ai_benchmark.md"
}

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$inputPath = Join-Path $tempRoot ("sura-ai-bench-" + [guid]::NewGuid().ToString("N") + ".safetensors")
$previousInput = $env:SURA_BENCH_INPUTS
try {
    $env:SURA_BENCH_INPUTS = $inputPath
    $suraOutput = & $enginePath $benchmarkPath 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        throw "Sura AI benchmark failed:`n$($suraOutput -join [Environment]::NewLine)"
    }
    $suraLine = $suraOutput | Where-Object { $_.TrimStart().StartsWith("{") } | Select-Object -Last 1
    if (-not $suraLine) { throw "Sura benchmark did not emit JSON" }
    $sura = $suraLine | ConvertFrom-Json

    $torch = [ordered]@{ status = "unavailable"; reason = "python executable not found" }
    $python = Get-Command python -ErrorAction SilentlyContinue
    if ($python) {
        $torchScript = Join-Path $root "benchmarks/ai_backend_benchmark_torch.py"
        $previousPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $torchOutput = & $python.Source $torchScript $inputPath --warmup 3 --runs 12 2>&1 |
                ForEach-Object { "$_" }
            $torchExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        if ($torchExitCode -eq 0) {
            $torchLine = $torchOutput | Where-Object { $_.TrimStart().StartsWith("{") } | Select-Object -Last 1
            if ($torchLine) { $torch = $torchLine | ConvertFrom-Json }
        } else {
            $torch = [ordered]@{ status = "error"; reason = ($torchOutput -join [Environment]::NewLine) }
        }
    }

    $cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
    $os = Get-CimInstance Win32_OperatingSystem
    $gpu = Get-CimInstance Win32_VideoController |
        Where-Object { $_.Name -match "NVIDIA" } | Select-Object -First 1
    $report = [ordered]@{
        schema = "sura.ai-benchmark-report.v1"
        generated_at_utc = [DateTime]::UtcNow.ToString("o")
        fairness = [ordered]@{
            same_hardware = $true
            same_inputs_file = $true
            process_startup_excluded = $true
            cuda_api_includes_transfer_and_synchronize = $true
            sura_cpu_threads = 1
            pytorch_cpu_threads = $(if ($torch.status -eq "ok") { [int]$torch.cpu_threads } else { $null })
            claim_scope = "rank-2 float32 forward matmul only"
        }
        hardware = [ordered]@{
            cpu = [string]$cpu.Name
            logical_processors = [int]$cpu.NumberOfLogicalProcessors
            memory_bytes = [int64]$os.TotalVisibleMemorySize * 1024
            gpu = [string]$gpu.Name
            gpu_driver = [string]$gpu.DriverVersion
        }
        runtime = [ordered]@{
            engine = [IO.Path]::GetFileName($enginePath)
            sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $enginePath).Hash.ToLowerInvariant()
        }
        sura = $sura
        pytorch = $torch
    }
    if ($torch.status -eq "ok") {
        $report.fairness["sura_pytorch_cpu_checksum_abs_error"] = [Math]::Abs(
            [double]$sura.cpu.checksum - [double]$torch.cpu.checksum)
    }

    $jsonParent = Split-Path -Parent $JsonOut
    $markdownParent = Split-Path -Parent $MarkdownOut
    if ($jsonParent) { New-Item -ItemType Directory -Force -Path $jsonParent | Out-Null }
    if ($markdownParent) { New-Item -ItemType Directory -Force -Path $markdownParent | Out-Null }
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($JsonOut, ($report | ConvertTo-Json -Depth 10), $utf8)

    $cudaSummary = if ($sura.cuda.status -eq "ok") {
        "Sura CUDA median $([Math]::Round([double]$sura.cuda.median_ms, 4)) ms"
    } else { "Sura CUDA unavailable" }
    $torchSummary = if ($torch.status -eq "ok") {
        "PyTorch CPU median $([Math]::Round([double]$torch.cpu.median_ms, 4)) ms"
    } else { "PyTorch unavailable: $($torch.reason)" }
    $checksumSummary = if ($torch.status -eq "ok") {
        "Sura/PyTorch CPU checksum absolute error: $($report.fairness.sura_pytorch_cpu_checksum_abs_error)"
    } else { "Sura/PyTorch checksum comparison unavailable" }
    $threadSummary = if ($torch.status -eq "ok") {
        "CPU threads: Sura 1; PyTorch $($torch.cpu_threads)"
    } else { "CPU threads: Sura 1; PyTorch unavailable" }
    $markdown = @"
# Sura AI Benchmark

- Scope: rank-2 float32 forward matmul, matrix $($sura.matrix_size)x$($sura.matrix_size)
- Hardware: $($cpu.Name); $($gpu.Name)
- $threadSummary
- Sura CPU median: $([Math]::Round([double]$sura.cpu.median_ms, 4)) ms
- $cudaSummary
- Maximum CPU/CUDA absolute error: $($sura.cuda.max_abs_error)
- $torchSummary
- $checksumSummary

The benchmark uses the same generated Safetensors input file when PyTorch is installed. Process startup is excluded. Sura's current CUDA public API timing includes host/device copies and synchronization. Results do not represent full Transformer training.
"@
    [IO.File]::WriteAllText($MarkdownOut, $markdown, $utf8)
    Write-Host "sura_ai_benchmark: PASS"
    Write-Host "json: $([IO.Path]::GetFullPath($JsonOut))"
    Write-Host "markdown: $([IO.Path]::GetFullPath($MarkdownOut))"
}
finally {
    $env:SURA_BENCH_INPUTS = $previousInput
    $resolved = [IO.Path]::GetFullPath($inputPath)
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith("sura-ai-bench-") -and
        (Test-Path -LiteralPath $resolved)) {
        Remove-Item -LiteralPath $resolved -Force
    }
}
