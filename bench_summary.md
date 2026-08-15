# Sura Benchmark Summary

> These are repository-owned historical measurements for regression tracking. They are not independent validation or a general language-performance claim. Reproduce the exact workload, versions, hashes, hardware, and raw samples before citing a ratio.

- Generated UTC: 2026-08-15T05:16:05.8652153Z
- Engine: C:\\\\Users\\\\user\\\\OneDrive\\\\문서\\\\Project\\\\Sura-Language\\\\SuraLanguage.exe
- Engine SHA-256: 3cb621f13dea8ae5abbf70077bba184ee24b57db96ac87c88f9c79caf1e83e5e (8925149 bytes)
- Python: python

## Summary

| Metric | Value |
| --- | ---: |
| Benchmarks | 23 |
| Median JIT speedup | 2.96x |
| Average JIT speedup | 23.35x |
| Python comparison cases | 19 |
| Cases faster than Python | 19 |
| Median Sura/Python ratio | 4.77x |
| Average Sura/Python ratio | 16.18x |
| Best Python comparison | game physics Vec3 loop (170.63x) |
| Native C++ comparison | Sura/native 1.42x |
| Native C++ 3D comparison | Sura/native 1.79x |

## Native C++ Baseline

| Benchmark | Timed region | Steps | Sura JIT ms | Native C++ ms | Sura/native ratio | Fair scope | Evidence |
| --- | --- | ---: | ---: | ---: | ---: | ---: | --- |
| game physics Vec2 loop | inner physics loop only | 100000 | 0.0714599969796836 | 0.05026 | 1.42x | True | bench_physics.sura vs bench_physics_native.cpp |
| game physics Vec3 loop | inner physics loop only | 100000 | 0.0899600039701909 | 0.05012 | 1.79x | True | bench_physics3d.sura vs bench_physics_native.cpp |

## JIT Benchmarks

| Benchmark | Interpreter ms | JIT ms | Speedup |
| --- | ---: | ---: | ---: |
| bench_jit.sura | 52.452 | 9.579 | 5.48x |
| bench_fib.sura | 412.041 | 106.542 | 3.87x |
| bench_agent_scoring.sura | 44.036 | 9.375 | 4.70x |
| bench_ai_schema.sura | 2031.24 | 2060.21 | 0.99x |
| bench_api_log_etl.sura | 371.795 | 125.457 | 2.96x |
| bench_rag_vector.sura | 141.182 | 47.658 | 2.96x |
| bench_tool_routing.sura | 169.773 | 53.096 | 3.20x |
| bench_policy_gate.sura | 489.86 | 182.613 | 2.68x |
| bench_guardrail.sura | 254.518 | 98.857 | 2.57x |
| bench_dependency_resolver.sura | 2240.09 | 947.378 | 2.36x |
| bench_workflow_scheduler.sura | 6091.58 | 2466.38 | 2.47x |
| bench_order_etl.sura | 588.792 | 264.024 | 2.23x |
| bench_telemetry_window.sura | 813.6 | 362.99 | 2.24x |
| bench_log_regex.sura | 4619.73 | 3096.85 | 1.49x |
| bench_fraud_scoring.sura | 345.839 | 137.045 | 2.52x |
| bench_feature_flags.sura | 437.998 | 179.164 | 2.44x |
| bench_physics.sura | 90.381 | 0.988 | 91.48x |
| bench_physics_inplace.sura | 11.492 | 2.881 | 3.99x |
| bench_physics3d.sura | 306.596 | 0.805 | 380.86x |
| bench_market.sura | 80.152 | 24.303 | 3.30x |
| bench_division.sura | 95.914 | 19.28 | 4.97x |
| bench_use_toplevel.sura | 87.919 | 19.595 | 4.49x |
| bench_index_get.sura | 72.956 | 25.447 | 2.87x |

## Python Comparison

| Case | Python ms | Sura JIT ms | Sura faster by |
| --- | ---: | ---: | ---: |
| fib(30) | 87.76 | 21.342259994708 | 4.11x |
| AI agent task scoring | 24.97 | 1.94234000518918 | 12.86x |
| AI JSON/schema validation | 455.87 | 419.03822000022 | 1.09x |
| API log ETL aggregation | 145.34 | 24.011020018952 | 6.05x |
| RAG vector ranking | 60.71 | 8.69601999293081 | 6.98x |
| AI tool routing scheduler | 70.86 | 10.5932800099254 | 6.69x |
| AI tool policy gate | 171.65 | 39.2432599968743 | 4.37x |
| AI guardrail event scoring | 102.67 | 19.2226800019853 | 5.34x |
| dependency resolver hot loop | 745.82 | 180.217899993295 | 4.14x |
| automation workflow scheduler | 2123.18 | 471.322399994824 | 4.50x |
| order CSV normalization ETL | 218.78 | 54.3716799933463 | 4.02x |
| telemetry rolling window | 346.94 | 72.6835399982519 | 4.77x |
| regex log summarization | 955.05 | 617.420439998386 | 1.55x |
| payment fraud scoring | 141.43 | 27.5772000080906 | 5.13x |
| feature flag rollout | 170.16 | 34.1559399967082 | 4.98x |
| game physics Vec2 loop | 39.79 | 0.748 | 53.20x |
| game physics in-place Vec2 loop | 12.47 | 2.85 | 4.38x |
| game physics Vec3 loop | 130.7 | 0.766 | 170.63x |
| market simulation objects | 59.45 | 23.423 | 2.54x |
