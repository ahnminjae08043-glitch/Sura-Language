param(
    [string]$Compiler = "",
    [string]$CublasLibrary = "",
    [switch]$Benchmark
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($Compiler)) {
    if ($env:SURA_CXX) { $Compiler = $env:SURA_CXX }
    else { $Compiler = "C:\msys64\mingw64\bin\g++.exe" }
}
$compilerPath = (Resolve-Path -LiteralPath $Compiler).Path
$source = (Resolve-Path -LiteralPath (Join-Path $root "tests/cuda_cublas_backend_test.cpp")).Path

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$work = Join-Path $tempRoot ("sura-cublas-smoke-" + [guid]::NewGuid().ToString("N"))
$exe = Join-Path $work "cuda_cublas_backend_test.exe"
$previousLibrary = $env:SURA_CUBLAS_LIBRARY
$previousCublasDisable = $env:SURA_CUBLAS_DISABLE
$previousCudaDisable = $env:SURA_CUDA_DISABLE

function Invoke-Harness {
    param([string]$Mode)
    $output = & $exe $Mode 2>&1 | ForEach-Object { "$_" }
    if ($LASTEXITCODE -ne 0) {
        throw "CUDA/cuBLAS harness mode '$Mode' failed:`n$($output -join [Environment]::NewLine)"
    }
    $output | ForEach-Object { Write-Host $_ }
}

try {
    New-Item -ItemType Directory -Path $work | Out-Null
    & $compilerPath -std=c++17 -O2 -Wall $source -o $exe
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "failed to compile CUDA/cuBLAS backend harness"
    }

    $env:SURA_CUDA_DISABLE = "1"
    $env:SURA_CUBLAS_DISABLE = $null
    $env:SURA_CUBLAS_LIBRARY = $null
    Invoke-Harness "cuda-unavailable"

    $env:SURA_CUDA_DISABLE = $null
    $env:SURA_CUBLAS_DISABLE = "1"
    $env:SURA_CUBLAS_LIBRARY = $null
    Invoke-Harness "reference"

    # An explicit but absent library must retain the resident PTX path; it may
    # never turn into a CPU fallback or a successful cuBLAS counter.
    $env:SURA_CUBLAS_DISABLE = $null
    $env:SURA_CUBLAS_LIBRARY = Join-Path $work "missing-cublas.dll"
    Invoke-Harness "reference"

    if (-not [string]::IsNullOrWhiteSpace($CublasLibrary)) {
        $resolvedCublas = (Resolve-Path -LiteralPath $CublasLibrary).Path
        $env:SURA_CUBLAS_LIBRARY = $resolvedCublas
        Invoke-Harness "cublas"
        if ($Benchmark) { Invoke-Harness "benchmark" }
    } elseif ($Benchmark) {
        throw "-Benchmark requires -CublasLibrary so the measured backend is explicit"
    }

    Write-Host "sura_cuda_cublas_smoke: PASS"
}
finally {
    $env:SURA_CUBLAS_LIBRARY = $previousLibrary
    $env:SURA_CUBLAS_DISABLE = $previousCublasDisable
    $env:SURA_CUDA_DISABLE = $previousCudaDisable
    $resolvedWork = [IO.Path]::GetFullPath($work)
    if ($resolvedWork.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolvedWork).StartsWith("sura-cublas-smoke-") -and
        (Test-Path -LiteralPath $resolvedWork -PathType Container)) {
        Remove-Item -LiteralPath $resolvedWork -Recurse -Force
    }
}
