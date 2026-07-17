# Sura Benchmark Release Notes

## Evidence

- Generated UTC: 2026-05-18T00:00:55.1624918Z
- Engine: C:\\\\Users\\\\user\\\\OneDrive\\\\문서\\\\Project\\\\Sura-Language\\\\SuraFinal.exe
- Python: python
- Benchmark cases: 19
- Average JIT speedup: 7.77x
- Python comparison cases: 18
- Faster-than-Python cases: 18
- Average Sura/Python ratio: 7.07x
- Best Python comparison: game physics Vec2 loop (65.10x)
- Native C++ baseline: Sura/native 1.08x (0.052 ms Sura JIT vs 0.048 ms native)

## Native C++ Baseline

| Benchmark | Sura JIT ms | Native C++ ms | Sura/native ratio | Evidence |
| --- | ---: | ---: | ---: | --- |
| game physics Vec2 loop | 0.0517285714285714 | 0.04776 | 1.08x | bench_physics.sura vs bench_physics_native.cpp |

## Python Comparison Highlights

| Case | Sura faster by | Python ms | Sura JIT ms | Evidence |
| --- | ---: | ---: | ---: | --- |
| game physics Vec2 loop | 65.10x | 43.16 | 0.663 | bench_physics.sura vs bench_physics.py |
| game physics in-place Vec2 loop | 6.19x | 16.75 | 2.705 | bench_physics_inplace.sura vs bench_physics_inplace.py |
| fib(30) | 5.37x | 107.82 | 20.09392 | bench_fib.sura vs bench_python.py |
| AI tool routing scheduler | 4.88x | 100.22 | 20.5575 | bench_tool_routing.sura vs bench_tool_routing.py |
| AI guardrail event scoring | 4.38x | 133.7 | 30.53418 | bench_guardrail.sura vs bench_guardrail.py |

## Coverage

Dashboard JSON includes 19 benchmark records, 18 Python comparison records plus native C++ baseline evidence for release evidence.
