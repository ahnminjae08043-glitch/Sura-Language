# Sura Benchmark Summary

- Generated UTC: 2026-07-16T15:13:22.9121342Z
- Engine: .\\\\SuraLanguage.exe
- Python: python

## Summary

| Metric | Value |
| --- | ---: |
| Benchmarks | 20 |
| Average JIT speedup | 21.48x |
| Python comparison cases | 19 |
| Cases faster than Python | 19 |
| Average Sura/Python ratio | 12.99x |
| Best Python comparison | game physics Vec3 loop (134.99x) |
| Native C++ comparison | Sura/native 1.80x |
| Native C++ 3D comparison | Sura/native 2.08x |

## Native C++ Baseline

| Benchmark | Timed region | Steps | Sura JIT ms | Native C++ ms | Sura/native ratio | Fair scope | Evidence |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| game physics Vec2 loop | inner physics loop only | 100000 | 0.090380001347512 | 0.0501 | 1.80x | True | bench_physics.sura vs bench_physics_native.cpp |
| game physics Vec3 loop | inner physics loop only | 100000 | 0.104260000807699 | 0.05016 | 2.08x | True | bench_physics3d.sura vs bench_physics_native.cpp |

## JIT Benchmarks

| Benchmark | Interpreter ms | JIT ms | Speedup |
| --- | ---: | ---: | ---: |
| bench_jit.sura | 62.34 | 11.118 | 5.61x |
| bench_fib.sura | 507.383 | 114.241 | 4.44x |
| bench_agent_scoring.sura | 51.047 | 28.504 | 1.79x |
| bench_ai_schema.sura | 2161.75 | 2169.84 | 1.00x |
| bench_api_log_etl.sura | 457.012 | 306.985 | 1.49x |
| bench_rag_vector.sura | 177.185 | 131.482 | 1.35x |
| bench_tool_routing.sura | 197.333 | 111.468 | 1.77x |
| bench_policy_gate.sura | 538.853 | 284.3 | 1.90x |
| bench_guardrail.sura | 348.093 | 178.837 | 1.95x |
| bench_dependency_resolver.sura | 2679.88 | 1189 | 2.25x |
| bench_workflow_scheduler.sura | 8043.59 | 4965.2 | 1.62x |
| bench_order_etl.sura | 694.191 | 609.434 | 1.14x |
| bench_telemetry_window.sura | 1054.87 | 686.4 | 1.54x |
| bench_log_regex.sura | 5374.09 | 4368.23 | 1.23x |
| bench_fraud_scoring.sura | 446.635 | 246.262 | 1.81x |
| bench_feature_flags.sura | 542.872 | 301.172 | 1.80x |
| bench_physics.sura | 105.877 | 0.78 | 135.74x |
| bench_physics_inplace.sura | 12.609 | 3.332 | 3.78x |
| bench_physics3d.sura | 354.16 | 1.396 | 253.70x |
| bench_market.sura | 93.489 | 25.861 | 3.62x |

## Python Comparison

| Case | Python ms | Sura JIT ms | Sura faster by |
| --- | ---: | ---: | ---: |
| fib(30) | 88.95 | 23.3775399959995 | 3.80x |
| AI agent task scoring | 26.72 | 5.51395999791566 | 4.85x |
| AI JSON/schema validation | 458.9 | 442.715840000892 | 1.04x |
| API log ETL aggregation | 171.92 | 66.0396400009631 | 2.60x |
| RAG vector ranking | 61.84 | 24.1099200007739 | 2.56x |
| AI tool routing scheduler | 77.89 | 22.5279200007208 | 3.46x |
| AI tool policy gate | 200.84 | 61.5462599991588 | 3.26x |
| AI guardrail event scoring | 113.47 | 32.149699999718 | 3.53x |
| dependency resolver hot loop | 848.43 | 235.055060000741 | 3.61x |
| automation workflow scheduler | 3052.52 | 1261.15953999979 | 2.42x |
| order CSV normalization ETL | 382.86 | 124.803339998471 | 3.07x |
| telemetry rolling window | 366.09 | 132.219839999743 | 2.77x |
| regex log summarization | 1182.42 | 710.108039999614 | 1.67x |
| payment fraud scoring | 149.17 | 45.5505800011451 | 3.27x |
| feature flag rollout | 207.68 | 58.6748200003058 | 3.54x |
| game physics Vec2 loop | 40.65 | 0.674 | 60.31x |
| game physics in-place Vec2 loop | 13.1 | 3.613 | 3.63x |
| game physics Vec3 loop | 143.22 | 1.061 | 134.99x |
| market simulation objects | 62.73 | 24.838 | 2.53x |
