# Sura Example Gallery

This directory is a runnable gallery for learning Sura through complete programs.
Every example here runs against the current engine: `SuraLanguage --check examples`
parses all of them, and the window examples render under `-- --smoke`. Examples
that needed a file you had to supply yourself, or that were written for older
syntax, were removed rather than left to fail on first run.

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

`surapkg examples` reports optional requirements such as `windows-graphics` or
`cuda`, so you can tell before running which examples need more than the engine.
Generated projects preserve the selected source byte for byte and write
`sura.example.json` with its gallery id and SHA-256 provenance.

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
| `starter` | 12 | Values, control flow, functions, collections, classes, files, JSON, async, errors, testing |
| `showcase` | 5 | Self-verifying tours of the language, including a JIT-heavy particle loop |
| `games_2d` | 6 | Pong, Breakout, platforming, racing, defense, asteroid dodging |
| `games_3d` | 5 | Perspective projection, ray casting, star flight, terrain, solar system |
| `games` | 6 | Text RPG, raycasting dungeon, 3D shooter variants, mouse input |
| `ai_ml` | 10 | Native neural nets, pretraining, clustering, RAG, recommenders, tensors |
| `simulations` | 6 | Boids, gravity, epidemics, traffic, ecosystems, cellular automata |
| `algorithms` | 5 | A*, maze generation, Sudoku, scheduling, word frequency |

Eight more sit at the top level because they each stand alone: `bad_apple_ascii`
and `frame_to_ascii` (character-frame rendering with no video file or FFmpeg),
`starblaster_3d`, `native_autograd`, `native_xor_ai`, `tiny_transformer`,
`tokenizer_dataset_training`, and `cuda_language_model_training` (which reports
and exits cleanly when no CUDA device is present).

AI examples are intentionally small enough to run on a normal CPU. The
`pretraining_pipeline.sura` example demonstrates the same tokenizer, dataset,
autograd, next-token loss, optimizer, and checkpoint stages used by larger model
training, but it is a teaching-scale model rather than a production LLM.
