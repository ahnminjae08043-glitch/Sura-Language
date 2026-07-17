# Sura Benchmark Summary

- Generated UTC: 2026-05-18T00:00:55.1624918Z
- Engine: C:\\\\Users\\\\user\\\\OneDrive\\\\문서\\\\Project\\\\Sura-Language\\\\SuraFinal.exe
- Python: python

## Summary

| Metric | Value |
| --- | ---: |
| Benchmarks | 19 |
| Average JIT speedup | 7.77x |
| Python comparison cases | 18 |
| Cases faster than Python | 18 |
| Average Sura/Python ratio | 7.07x |
| Best Python comparison | game physics Vec2 loop (65.10x) |
| Native C++ comparison | Sura/native 1.08x |

## Native C++ Baseline

| Metric | Value |
| --- | ---: |
| Status | PASS |
| Benchmark | game physics Vec2 loop |
| Sura JIT loop avg | 0.052 ms |
| Native C++ avg | 0.048 ms |
| Sura/native ratio | 1.08x |
| Evidence | bench_physics.sura vs bench_physics_native.cpp |

## JIT Benchmarks

| Benchmark | Interpreter ms | JIT ms | Speedup |
| --- | ---: | ---: | ---: |
| bench_jit.sura | 60.487 | 16.799 | 3.60x |
| bench_fib.sura | 426.079 | 95.396 | 4.47x |
| bench_agent_scoring.sura | 61.796 | 31.615 | 1.95x |
| bench_ai_schema.sura | 2344.28 | 2346.78 | 1.00x |
| bench_api_log_etl.sura | 422.1 | 305.152 | 1.38x |
| bench_rag_vector.sura | 150.919 | 121.772 | 1.24x |
| bench_tool_routing.sura | 183.76 | 104.427 | 1.76x |
| bench_policy_gate.sura | 552.081 | 247.797 | 2.23x |
| bench_guardrail.sura | 315.423 | 191.928 | 1.64x |
| bench_dependency_resolver.sura | 2543.18 | 1051.1 | 2.42x |
| bench_workflow_scheduler.sura | 7531.98 | 4927.21 | 1.53x |
| bench_order_etl.sura | 595.202 | 537.011 | 1.11x |
| bench_telemetry_window.sura | 953.286 | 707.099 | 1.35x |
| bench_log_regex.sura | 5757.65 | 3546.54 | 1.62x |
| bench_fraud_scoring.sura | 376.979 | 215.724 | 1.75x |
| bench_feature_flags.sura | 517.975 | 330.091 | 1.57x |
| bench_physics.sura | 51.011 | 0.477 | 106.94x |
| bench_physics_inplace.sura | 15.605 | 2.695 | 5.79x |
| bench_market.sura | 76.06 | 18.095 | 4.20x |

## Python Comparison

| Case | Python ms | Sura JIT ms | Sura faster by |
| --- | ---: | ---: | ---: |
| fib(30) | 107.82 | 20.09392 | 5.37x |
| AI agent task scoring | 25.39 | 6.18324 | 4.11x |
| AI JSON/schema validation | 575.92 | 477.32646 | 1.21x |
| API log ETL aggregation | 180.34 | 59.6623 | 3.02x |
| RAG vector ranking | 71.51 | 23.47112 | 3.05x |
| AI tool routing scheduler | 100.22 | 20.5575 | 4.88x |
| AI tool policy gate | 213.15 | 58.95626 | 3.62x |
| AI guardrail event scoring | 133.7 | 30.53418 | 4.38x |
| dependency resolver hot loop | 936.34 | 230.79822 | 4.06x |
| automation workflow scheduler | 3396.72 | 1016.75126 | 3.34x |
| order CSV normalization ETL | 299.87 | 118.76554 | 2.52x |
| telemetry rolling window | 420.84 | 126.96266 | 3.31x |
| regex log summarization | 1079.17 | 716.32694 | 1.51x |
| payment fraud scoring | 169.59 | 46.86022 | 3.62x |
| feature flag rollout | 217.93 | 55.64602 | 3.92x |
| game physics Vec2 loop | 43.16 | 0.663 | 65.10x |
| game physics in-place Vec2 loop | 16.75 | 2.705 | 6.19x |
| market simulation objects | 67.78 | 16.388 | 4.14x |
