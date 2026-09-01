param(
    [string]$Engine = ".\SuraLanguage.exe",
    [string]$Cxx = "",
    [string]$JsonOut = "artifacts\native_perf.json",
    [string]$SummaryOut = "artifacts\native_perf.md",
    [int]$SuraRuns = 5,
    [double]$MaxSuraNativeRatio = 0.0
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Resolve-OutputPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path (Resolve-Path ".").Path $Path
}

function Ensure-ParentDirectory {
    param([string]$Path)
    $parent = Split-Path -Parent $Path
    if ($parent) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
}

function Find-Cxx {
    param([string]$Preferred)
    $candidates = @()
    if (-not [string]::IsNullOrWhiteSpace($Preferred)) { $candidates += $Preferred }
    $candidates += @("g++", "c++", "clang++", "C:\msys64\mingw64\bin\g++.exe")
    foreach ($candidate in $candidates) {
        try {
            $out = & $candidate --version 2>&1 | Out-String
            if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($out)) {
                return $candidate
            }
        } catch {
        }
    }
    return $null
}

function Extract-Ms {
    param([string]$Text, [string]$Kind, [string]$Dimension = "vec2")
    $script:LastExtractSource = ""
    $script:LastExtractScope = ""
    $pattern = if ($Dimension -eq "vec3") {
        'physics 3d step\s+(\S+)\s*x\s+([0-9.]+)\s+ms'
    } else {
        'physics step\s+(\S+)\s*x\s+([0-9.]+)\s+ms'
    }
    $loop = [regex]::Match($Text, $pattern)
    if ($loop.Success) {
        $script:LastExtractSource = if ($Kind -eq "native") { "native_loop" } else { "script_loop" }
        $script:LastExtractScope = $loop.Groups[1].Value
        return [double]$loop.Groups[2].Value
    }
    $avg = [regex]::Match($Text, 'avg \(\d+ runs\):\s+([0-9.]+) ms')
    if ($avg.Success) {
        $script:LastExtractSource = "native_avg"
        return [double]$avg.Groups[1].Value
    }
    $execute = [regex]::Match($Text, 'Execute:\s+([0-9.]+) ms')
    if ($execute.Success) {
        $script:LastExtractSource = "sura_execute"
        return [double]$execute.Groups[1].Value
    }
    return $null
}

function Extract-FinalAxis {
    param([string]$Text, [string]$Axis)
    $match = [regex]::Match($Text, "final pos\.$([regex]::Escape($Axis)):\s*(?:\r?\n)?\s*(-?[0-9]+(?:\.[0-9]+)?)")
    if ($match.Success) { return [double]$match.Groups[1].Value }
    return $null
}

function Average-Values {
    param($Values)
    if ($Values.Count -eq 0) { return $null }
    $sum = 0.0
    foreach ($value in $Values) { $sum += [double]$value }
    return $sum / $Values.Count
}

function Invoke-SuraBaselineRuns {
    param(
        [string]$EnginePath,
        [string]$SuraBench,
        [string]$Dimension,
        [string]$Axis,
        [int]$Runs
    )
    $runMs = @()
    $timeSources = @()
    $scopes = @()
    $finals = @()
    $code = 0
    for ($i = 0; $i -lt $Runs; $i++) {
        $suraOut = & $EnginePath --jit --bench $SuraBench 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { $code = $LASTEXITCODE }
        $ms = Extract-Ms $suraOut "sura" $Dimension
        if ($null -ne $ms) {
            $runMs += [double]$ms
            $timeSources += $script:LastExtractSource
            $scopes += $script:LastExtractScope
        }
        $final = Extract-FinalAxis $suraOut $Axis
        if ($null -ne $final) {
            $finals += [double]$final
        }
    }
    return [pscustomobject]@{
        code = $code
        run_ms = $runMs
        avg_ms = Average-Values $runMs
        time_sources = $timeSources
        scopes = $scopes
        finals = $finals
    }
}

function New-BaselineRecord {
    param(
        [string]$Id,
        [string]$Benchmark,
        [string]$SuraFile,
        [string]$NativeFile,
        [string]$Dimension,
        [string]$Axis,
        [string]$Workload,
        [double]$ExpectedFinal,
        [string]$NativeOut,
        $SuraRunsResult,
        [int]$NativeCode,
        [int]$SuraRunsRequested,
        [string]$CompilerFlags,
        [double]$MaxSuraNativeRatio
    )

    $nativeMs = Extract-Ms $NativeOut "native" $Dimension
    $nativeTimeSource = $script:LastExtractSource
    $nativeScope = $script:LastExtractScope
    $nativeFinal = Extract-FinalAxis $NativeOut $Axis
    $suraMs = $SuraRunsResult.avg_ms
    $ratio = if ($nativeMs -and $nativeMs -gt 0 -and $suraMs) { [double]$suraMs / [double]$nativeMs } else { $null }
    $scopeOk = ($nativeScope -eq "100k" -and
        $SuraRunsResult.scopes.Count -eq $SuraRunsResult.run_ms.Count -and
        -not ($SuraRunsResult.scopes | Where-Object { $_ -ne "100k" }) -and
        $nativeTimeSource -eq "native_loop" -and
        -not ($SuraRunsResult.time_sources | Where-Object { $_ -ne "script_loop" }) -and
        $null -ne $nativeFinal -and [Math]::Abs([double]$nativeFinal - $ExpectedFinal) -lt 0.01 -and
        $SuraRunsResult.finals.Count -eq $SuraRunsResult.run_ms.Count -and
        -not ($SuraRunsResult.finals | Where-Object { [Math]::Abs([double]$_ - $ExpectedFinal) -ge 0.01 }))
    $withinLimit = ($MaxSuraNativeRatio -le 0.0 -or ($null -ne $ratio -and $ratio -le $MaxSuraNativeRatio))
    $passed = ($NativeCode -eq 0 -and
        $SuraRunsResult.code -eq 0 -and
        $null -ne $nativeMs -and
        $null -ne $suraMs -and
        $scopeOk -and
        $withinLimit)
    $warnings = @()
    if (-not $scopeOk) {
        $warnings += "$Id native baseline fairness check failed: Sura and C++ must time the same 100k inner loop and produce the same final position"
    }
    if ($null -ne $ratio -and $ratio -gt 100.0) {
        $warnings += "$Id Sura JIT is more than 100x slower than the native C++ baseline"
    }
    if (-not $withinLimit) {
        $warnings += "$Id Sura/native ratio exceeds MaxSuraNativeRatio=$MaxSuraNativeRatio"
    }

    return [ordered]@{
        id = $Id
        benchmark = $Benchmark
        sura_file = $SuraFile
        native_file = $NativeFile
        dimension = $Dimension
        measurement_scope = [ordered]@{
            timed_region = "inner physics loop only"
            workload = $Workload
            steps = 100000
            step_label = "100k"
            sura_runs = $SuraRunsResult.run_ms.Count
            native_runs = $SuraRunsRequested
            sura_timer = "Sura clock() around 100k script loop"
            native_timer = "std::chrono around 100k C++ loop"
            source_loop_guard = "$SuraFile`: while i < 100000"
            cxx_flags = $CompilerFlags
            comparison_note = "C++ receives the same fixed inner loop scope and native compiler optimization range; this ratio is evidence, not a claim that Sura is faster than C++."
        }
        fair_scope_passed = $scopeOk
        expected_final_axis = $Axis
        expected_final_value = $ExpectedFinal
        sura_final_runs = $SuraRunsResult.finals
        native_final_value = $nativeFinal
        sura_jit_ms = $suraMs
        sura_jit_time_source = if ($SuraRunsResult.time_sources.Count -gt 0) { ($SuraRunsResult.time_sources | Select-Object -Unique) -join "," } else { "" }
        sura_loop_scope = if ($SuraRunsResult.scopes.Count -gt 0) { ($SuraRunsResult.scopes | Select-Object -Unique) -join "," } else { "" }
        sura_jit_runs = $SuraRunsResult.run_ms
        sura_jit_run_count = $SuraRunsResult.run_ms.Count
        native_ms = $nativeMs
        native_time_source = $nativeTimeSource
        native_loop_scope = $nativeScope
        sura_native_ratio = $ratio
        native_faster_by = if ($ratio -and $ratio -gt 0) { $ratio } else { $null }
        max_sura_native_ratio = $MaxSuraNativeRatio
        passed = $passed
        warnings = $warnings
    }
}

function Write-Utf8 {
    param([string]$Path, [string]$Text)
    Ensure-ParentDirectory $Path
    [System.IO.File]::WriteAllText($Path, $Text, (New-Object System.Text.UTF8Encoding($false)))
}

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$root = Split-Path -Parent $PSScriptRoot
$suraBenchVec2 = Join-Path $root "bench_physics.sura"
$suraBenchVec3 = Join-Path $root "bench_physics3d.sura"
if (-not (Test-Path -LiteralPath $suraBenchVec2)) {
    throw "Sura physics benchmark not found: $suraBenchVec2"
}
if (-not (Test-Path -LiteralPath $suraBenchVec3)) {
    throw "Sura 3D physics benchmark not found: $suraBenchVec3"
}
$suraBenchVec2Text = [System.IO.File]::ReadAllText($suraBenchVec2, [System.Text.Encoding]::UTF8)
$suraBenchVec3Text = [System.IO.File]::ReadAllText($suraBenchVec3, [System.Text.Encoding]::UTF8)
$physicsSteps = 100000
$physicsStepLabel = "100k"
if ($suraBenchVec2Text -notmatch 'while\s+i\s*<\s*100000\b' -or $suraBenchVec2Text -notmatch 'physics step 100k x') {
    throw "bench_physics.sura must keep the native baseline workload at physics step 100k x"
}
if ($suraBenchVec3Text -notmatch 'while\s+i\s*<\s*100000\b' -or $suraBenchVec3Text -notmatch 'physics 3d step 100k x') {
    throw "bench_physics3d.sura must keep the native baseline workload at physics 3d step 100k x"
}

$compiler = Find-Cxx $Cxx
if (-not $compiler) {
    throw "C++ compiler not found; install g++, c++, or clang++"
}

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_native_perf_" + [System.Guid]::NewGuid().ToString("N"))
$jsonPath = Resolve-OutputPath $JsonOut
$summaryPath = Resolve-OutputPath $SummaryOut

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $cppPath = Join-Path $temp "bench_physics_native.cpp"
    $exeSuffix = if ($IsWindows -or $env:OS -eq "Windows_NT") { ".exe" } else { "" }
    $exePath = Join-Path $temp ("bench_physics_native" + $exeSuffix)
    Write-Utf8 $cppPath @"
#include <chrono>
#include <iomanip>
#include <iostream>

struct Vec2 {
    double x;
    double y;
    Vec2(double x_ = 0.0, double y_ = 0.0) : x(x_), y(y_) {}
    Vec2 add(const Vec2& other) const { return Vec2(x + other.x, y + other.y); }
    Vec2 scale(double k) const { return Vec2(x * k, y * k); }
};

static inline Vec2 step(const Vec2& pos, const Vec2& vel, double dt) {
    return pos.add(vel.scale(dt));
}

struct Vec3 {
    double x;
    double y;
    double z;
    Vec3(double x_ = 0.0, double y_ = 0.0, double z_ = 0.0) : x(x_), y(y_), z(z_) {}
    Vec3 add(const Vec3& other) const { return Vec3(x + other.x, y + other.y, z + other.z); }
    Vec3 scale(double k) const { return Vec3(x * k, y * k, z * k); }
    Vec3 cross(const Vec3& other) const {
        return Vec3(
            y * other.z - z * other.y,
            z * other.x - x * other.z,
            x * other.y - y * other.x
        );
    }
};

static inline Vec3 step3(const Vec3& pos, const Vec3& vel, const Vec3& gravity, const Vec3& wind, double dt) {
    Vec3 swirl = vel.cross(wind).scale(0.001);
    Vec3 next_vel = vel.add(gravity.scale(dt)).add(swirl);
    return pos.add(next_vel.scale(dt));
}

int main() {
    const int runs = $SuraRuns;
    const int steps = $physicsSteps;
    double total_vec2_ms = 0.0;
    double total_vec3_ms = 0.0;
    double final_x = 0.0;
    double final_z = 0.0;
    for (int r = 0; r < runs; ++r) {
        Vec2 p(0.0, 0.0);
        Vec2 v(1.0, 2.0);
        const double dt = 0.016;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < steps; ++i) {
            p = step(p, v, dt);
        }
        auto end = std::chrono::high_resolution_clock::now();
        total_vec2_ms += std::chrono::duration<double, std::milli>(end - start).count();
        final_x = p.x;
    }
    for (int r = 0; r < runs; ++r) {
        Vec3 p(0.0, 0.0, 0.0);
        Vec3 v(1.0, 2.0, 3.0);
        Vec3 gravity(0.0, -9.8, 0.0);
        Vec3 wind(0.2, 0.1, 0.4);
        const double dt = 0.016;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < steps; ++i) {
            p = step3(p, v, gravity, wind, dt);
        }
        auto end = std::chrono::high_resolution_clock::now();
        total_vec3_ms += std::chrono::duration<double, std::milli>(end - start).count();
        final_z = p.z;
    }
    std::cout << std::setprecision(15);
    std::cout << "physics step $physicsStepLabel x  " << (total_vec2_ms / runs) << " ms\n";
    std::cout << "final pos.x:\n" << final_x << "\n";
    std::cout << "physics 3d step $physicsStepLabel x  " << (total_vec3_ms / runs) << " ms\n";
    std::cout << "final pos.z:\n" << final_z << "\n";
    std::cout << "avg (" << runs << " runs): " << (total_vec2_ms / runs) << " ms\n";
    std::cout << "avg 3d (" << runs << " runs): " << (total_vec3_ms / runs) << " ms\n";
    return 0;
}
"@

    $compileFlags = @("-O3", "-DNDEBUG", "-std=c++17", "-march=native")
    $compileOut = & $compiler @compileFlags $cppPath -o $exePath 2>&1 | ForEach-Object { "$_" }
    $compileCode = $LASTEXITCODE
    $compileFlagsUsed = $compileFlags
    if ($compileCode -ne 0 -or -not (Test-Path -LiteralPath $exePath)) {
        $compileFlags = @("-O3", "-DNDEBUG", "-std=c++17")
        $compileOut = & $compiler @compileFlags $cppPath -o $exePath 2>&1 | ForEach-Object { "$_" }
        $compileCode = $LASTEXITCODE
        $compileFlagsUsed = $compileFlags
    }
    if ($compileCode -ne 0 -or -not (Test-Path -LiteralPath $exePath)) {
        Write-Output ($compileOut -join "`n")
        throw "native C++ baseline compile failed"
    }

    if ($SuraRuns -lt 1) { $SuraRuns = 1 }

    $nativeOut = & $exePath 2>&1 | Out-String
    $nativeCode = $LASTEXITCODE
    $compileFlagsText = ($compileFlagsUsed -join " ")

    $suraVec2 = Invoke-SuraBaselineRuns -EnginePath $enginePath -SuraBench $suraBenchVec2 -Dimension "vec2" -Axis "x" -Runs $SuraRuns
    $suraVec3 = Invoke-SuraBaselineRuns -EnginePath $enginePath -SuraBench $suraBenchVec3 -Dimension "vec3" -Axis "z" -Runs $SuraRuns
    $vec2Baseline = New-BaselineRecord `
        -Id "vec2" `
        -Benchmark "game physics Vec2 loop" `
        -SuraFile "bench_physics.sura" `
        -NativeFile "bench_physics_native.cpp" `
        -Dimension "vec2" `
        -Axis "x" `
        -Workload "Vec2 p = step(p, v, dt)" `
        -ExpectedFinal 1600.0 `
        -NativeOut $nativeOut `
        -SuraRunsResult $suraVec2 `
        -NativeCode $nativeCode `
        -SuraRunsRequested $SuraRuns `
        -CompilerFlags $compileFlagsText `
        -MaxSuraNativeRatio $MaxSuraNativeRatio
    $vec3Baseline = New-BaselineRecord `
        -Id "vec3" `
        -Benchmark "game physics Vec3 loop" `
        -SuraFile "bench_physics3d.sura" `
        -NativeFile "bench_physics_native.cpp" `
        -Dimension "vec3" `
        -Axis "z" `
        -Workload "Vec3 p = step3(p, v, gravity, wind, dt)" `
        -ExpectedFinal 4799.52 `
        -NativeOut $nativeOut `
        -SuraRunsResult $suraVec3 `
        -NativeCode $nativeCode `
        -SuraRunsRequested $SuraRuns `
        -CompilerFlags $compileFlagsText `
        -MaxSuraNativeRatio $MaxSuraNativeRatio

    $baselines = @($vec2Baseline, $vec3Baseline)
    $warnings = @($baselines | ForEach-Object { $_.warnings } | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) })
    $passed = ($compileCode -eq 0 -and $nativeCode -eq 0 -and -not ($baselines | Where-Object { -not $_.passed }))
    $primary = $vec2Baseline
    $native3d = $vec3Baseline
    $engineItem = Get-Item -LiteralPath $enginePath
    $engineSha256 = (Get-FileHash -LiteralPath $enginePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $engineVersion = (& $enginePath --version 2>&1 | Out-String).Trim()
    $compilerVersion = (& $compiler --version 2>&1 | Select-Object -First 1 | Out-String).Trim()
    $cpuName = ""
    if ($env:OS -eq "Windows_NT") {
        try { $cpuName = [string](Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name) } catch {}
    }

    $report = [ordered]@{
        schema = "sura.native.performance.v1"
        generated_utc = [DateTime]::UtcNow.ToString("yyyy-MM-ddTHH:mm:ssZ")
        passed = $passed
        engine = [ordered]@{
            file = $engineItem.Name
            version_output = $engineVersion
            bytes = $engineItem.Length
            sha256 = $engineSha256
        }
        host = [ordered]@{
            os = [Environment]::OSVersion.VersionString
            architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
            cpu = $cpuName
        }
        compiler = $compiler
        compiler_version = $compilerVersion
        compiler_flags = $compileFlagsText
        compiler_output = ($compileOut -join "`n")
        baseline_count = $baselines.Count
        primary_baseline_id = "vec2"
        three_d_baseline_id = "vec3"
        baselines = $baselines
        benchmark = $primary.benchmark
        sura_file = $primary.sura_file
        native_file = $primary.native_file
        measurement_scope = $primary.measurement_scope
        fair_scope_passed = $primary.fair_scope_passed
        expected_final_x = $primary.expected_final_value
        sura_final_x_runs = $primary.sura_final_runs
        native_final_x = $primary.native_final_value
        sura_jit_ms = $primary.sura_jit_ms
        sura_jit_time_source = $primary.sura_jit_time_source
        sura_loop_scope = $primary.sura_loop_scope
        sura_jit_runs = $primary.sura_jit_runs
        sura_jit_run_count = $primary.sura_jit_run_count
        native_ms = $primary.native_ms
        native_time_source = $primary.native_time_source
        native_loop_scope = $primary.native_loop_scope
        sura_native_ratio = $primary.sura_native_ratio
        native_faster_by = $primary.native_faster_by
        native_3d_available = $native3d.passed
        native_3d_sura_jit_ms = $native3d.sura_jit_ms
        native_3d_ms = $native3d.native_ms
        native_3d_sura_native_ratio = $native3d.sura_native_ratio
        native_3d_fair_scope_passed = $native3d.fair_scope_passed
        max_sura_native_ratio = $MaxSuraNativeRatio
        warnings = $warnings
    }
    Write-Utf8 $jsonPath ($report | ConvertTo-Json -Depth 8)

    $ratioText = if ($null -ne $primary.sura_native_ratio) { "{0:N2}x" -f $primary.sura_native_ratio } else { "n/a" }
    $nativeText = if ($null -ne $primary.native_ms) { "{0:N3} ms" -f $primary.native_ms } else { "n/a" }
    $suraText = if ($null -ne $primary.sura_jit_ms) { "{0:N3} ms" -f $primary.sura_jit_ms } else { "n/a" }
    $vec3RatioText = if ($null -ne $native3d.sura_native_ratio) { "{0:N2}x" -f $native3d.sura_native_ratio } else { "n/a" }
    $vec3NativeText = if ($null -ne $native3d.native_ms) { "{0:N3} ms" -f $native3d.native_ms } else { "n/a" }
    $vec3SuraText = if ($null -ne $native3d.sura_jit_ms) { "{0:N3} ms" -f $native3d.sura_jit_ms } else { "n/a" }
    $summaryLines = New-Object System.Collections.Generic.List[string]
    $summaryLines.Add("# Sura Native Performance Baseline")
    $summaryLines.Add("")
    $summaryLines.Add("- Engine: $engineVersion")
    $summaryLines.Add("- Engine SHA-256: $engineSha256")
    $summaryLines.Add("- Engine bytes: $($engineItem.Length)")
    $summaryLines.Add("- CPU: $cpuName")
    $summaryLines.Add("- Compiler: $compilerVersion")
    $summaryLines.Add("")
    $summaryLines.Add("- Generated UTC: $($report.generated_utc)")
    $summaryLines.Add("- Compiler: $compiler")
    $summaryLines.Add("- C++ flags: $($report.compiler_flags)")
    $summaryLines.Add("- Timed region: inner physics loop only")
    $summaryLines.Add("- Steps: $physicsSteps ($physicsStepLabel)")
    $summaryLines.Add("- Baselines: Vec2 and 3D Vec3")
    $summaryLines.Add("- Primary Sura/native ratio: $ratioText")
    $summaryLines.Add("- 3D Sura/native ratio: $vec3RatioText")
    $summaryLines.Add("- Sura time source: $($primary.sura_jit_time_source)")
    $summaryLines.Add("- Native time source: $($primary.native_time_source)")
    $summaryLines.Add("- Fair scope check: $($report.fair_scope_passed)")
    $summaryLines.Add("- 3D fair scope check: $($report.native_3d_fair_scope_passed)")
    $summaryLines.Add("")
    $summaryLines.Add("| Benchmark | Sura JIT loop avg | Native C++ avg | Sura/native ratio | Fair scope | Final axis |")
    $summaryLines.Add("| --- | ---: | ---: | ---: | ---: | --- |")
    foreach ($baseline in $baselines) {
        $baselineSura = if ($null -ne $baseline.sura_jit_ms) { "{0:N3} ms" -f $baseline.sura_jit_ms } else { "n/a" }
        $baselineNative = if ($null -ne $baseline.native_ms) { "{0:N3} ms" -f $baseline.native_ms } else { "n/a" }
        $baselineRatio = if ($null -ne $baseline.sura_native_ratio) { "{0:N2}x" -f $baseline.sura_native_ratio } else { "n/a" }
        $summaryLines.Add("| $($baseline.benchmark) | $baselineSura | $baselineNative | $baselineRatio | $($baseline.fair_scope_passed) | $($baseline.expected_final_axis)=$($baseline.expected_final_value) |")
    }
    $summaryLines.Add("")
    $summaryLines.Add("This is evidence, not a claim that Sura is faster than C++. The Sura and C++ timings both cover the same fixed inner physics loop scope; use the ratio trend to drive JIT/runtime optimization work.")
    $summary = $summaryLines -join "`n"
    Write-Utf8 $summaryPath ($summary + "`n")

    if (-not $passed) {
        throw "Native performance baseline failed"
    }
    Write-Host ("native_perf_baseline: PASS (Vec2 Sura {0}, C++ {1}, Sura/native ratio {2}; Vec3 Sura {3}, C++ {4}, Sura/native ratio {5})" -f $suraText, $nativeText, $ratioText, $vec3SuraText, $vec3NativeText, $vec3RatioText)
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
