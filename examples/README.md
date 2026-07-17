# Sura Example Gallery

This directory is a runnable gallery for learning Sura through complete programs.
The original examples are kept as-is; the folders below add focused examples for
games, AI/ML, simulations, and algorithms.

## Run an example

Installed Sura distributions include this gallery. List it or turn an example
into a standalone package without locating the installation directory:

```powershell
surapkg examples
surapkg examples algorithms --json
surapkg example algorithms/word_frequency my_word_demo
cd my_word_demo
surapkg check
surapkg run
```

`surapkg examples` reports optional requirements such as `windows-graphics`,
`ffmpeg`, or `cuda`. Generated projects preserve the selected source byte for
byte and write `sura.example.json` with its gallery id and SHA-256 provenance.

From the repository root:

```powershell
.\SuraLanguage.exe examples\algorithms\astar_pathfinding.sura
.\SuraLanguage.exe examples\ai_ml\linear_regression.sura
.\SuraLanguage.exe examples\games_2d\pong.sura
```

Window examples accept `--smoke`. This runs only a few frames, closes the window,
and is useful in CI or when checking a new build.

```powershell
.\SuraLanguage.exe examples\games_2d\pong.sura -- --smoke
.\SuraLanguage.exe examples\games_3d\wireframe_cube.sura -- --smoke
```

Check every example without running it:

```powershell
.\SuraLanguage.exe --check examples
```

## Catalog

| Folder | Examples | Topics |
| --- | ---: | --- |
| `games_2d` | 6 | Pong, Breakout, platforming, racing, defense, asteroid dodging |
| `games_3d` | 5 | Perspective projection, ray casting, star flight, terrain, solar system |
| `ai_ml` | 10 | Native neural nets, pretraining, clustering, RAG, recommenders, tensors |
| `simulations` | 6 | Boids, gravity, epidemics, traffic, ecosystems, cellular automata |
| `algorithms` | 5 | A*, maze generation, Sudoku, scheduling, word frequency |

AI examples are intentionally small enough to run on a normal CPU. The
`pretraining_pipeline.sura` example demonstrates the same tokenizer, dataset,
autograd, next-token loss, optimizer, and checkpoint stages used by larger model
training, but it is a teaching-scale model rather than a production LLM.
