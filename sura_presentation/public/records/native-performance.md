# Sura Native Performance Baseline

- Engine: Sura Language 1.11.1
- Engine SHA-256: 4a78ca28fe2b500e6d7dfc46347b981b355e43cb7a57cd63bd8a8618dc16a1de
- Engine bytes: 8651612
- CPU: 12th Gen Intel(R) Core(TM) i5-12400F
- Compiler: g++.exe (Rev13, Built by MSYS2 project) 15.2.0

- Generated UTC: 2026-07-16T13:48:07Z
- Compiler: C:\msys64\mingw64\bin\g++.exe
- C++ flags: -O3 -DNDEBUG -std=c++17 -march=native
- Timed region: inner physics loop only
- Steps: 100000 (100k)
- Baselines: Vec2 and 3D Vec3
- Primary Sura/native ratio: 1.80x
- 3D Sura/native ratio: 2.08x
- Sura time source: script_loop
- Native time source: native_loop
- Fair scope check: True
- 3D fair scope check: True

| Benchmark | Sura JIT loop avg | Native C++ avg | Sura/native ratio | Fair scope | Final axis |
| --- | ---: | ---: | ---: | ---: | --- |
| game physics Vec2 loop | 0.090 ms | 0.050 ms | 1.80x | True | x=1600 |
| game physics Vec3 loop | 0.104 ms | 0.050 ms | 2.08x | True | z=4799.52 |

This is evidence, not a claim that Sura is faster than C++. The Sura and C++ timings both cover the same fixed inner physics loop scope; use the ratio trend to drive JIT/runtime optimization work.
