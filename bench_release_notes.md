# Sura Benchmark Release Notes

> These are repository-owned historical measurements, not independent validation. Ratios apply only to the recorded workloads and environment.

## Evidence

- Generated UTC: 2026-08-15T05:16:05.8652153Z
- Engine: C:\\\\Users\\\\user\\\\OneDrive\\\\문서\\\\Project\\\\Sura-Language\\\\SuraLanguage.exe
- Python: python
- Benchmark cases: 23
- Average JIT speedup: 23.35x
- Python comparison cases: 19
- Faster-than-Python cases: 19
- Average Sura/Python ratio: 16.18x
- Best Python comparison: game physics Vec3 loop (170.63x)
- Native C++ baseline: Sura/native 1.42x (0.071 ms Sura JIT vs 0.050 ms native)
- Native C++ 3D baseline: Sura/native 1.79x (0.090 ms Sura JIT vs 0.050 ms native)

## Native C++ Baseline

| Benchmark | Scope | Sura JIT ms | Native C++ ms | Sura/native ratio | Evidence |
| --- | --- | ---: | ---: | ---: | --- |
| game physics Vec2 loop | inner physics loop only, 100000 steps | 0.0714599969796836 | 0.05026 | 1.42x | bench_physics.sura vs bench_physics_native.cpp |
| game physics Vec3 loop | inner physics loop only, 100000 steps | 0.0899600039701909 | 0.05012 | 1.79x | bench_physics3d.sura vs bench_physics_native.cpp |

## Python Comparison Highlights

| Case | Sura faster by | Python ms | Sura JIT ms | Evidence |
| --- | ---: | ---: | ---: | --- |
| game physics Vec3 loop | 170.63x | 130.7 | 0.766 | bench_physics3d.sura vs bench_physics3d.py |
| game physics Vec2 loop | 53.20x | 39.79 | 0.748 | bench_physics.sura vs bench_physics.py |
| AI agent task scoring | 12.86x | 24.97 | 1.94234000518918 | bench_agent_scoring.sura vs bench_agent_scoring.py |
| RAG vector ranking | 6.98x | 60.71 | 8.69601999293081 | bench_rag_vector.sura vs bench_rag_vector.py |
| AI tool routing scheduler | 6.69x | 70.86 | 10.5932800099254 | bench_tool_routing.sura vs bench_tool_routing.py |

## Coverage

Dashboard JSON includes 23 benchmark records, 19 Python comparison records plus native C++ baseline evidence for release evidence.
