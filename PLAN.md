# Plan

Phonlet Wake 0.1.0: the engine, the `en-1` model, the English keyword file, the SDKs and the
threshold generator. The README's [platform table](README.md#platforms) is the current status of
each SDK. This file holds the decisions that shape the code and the open work, lowest first.

## Decisions

| Subject | Decision |
| --- | --- |
| Model files | `phonletwake` container (11-byte magic, then `kind` u32 and `version` u32), extension `.pwake`. A model id `<language>-<generation>` (`en-1`) names one trained artifact; any retrain or different width is a new id with its own calibration |
| Keyword file | One format everywhere, exactly what the generator writes: `{"model": {"id", "sha256"}, "keywords": [...], "metadata": {...}}`; a bare phrase array is the same file without `model`. Every SDK takes it as a path or URL, an object, or phrases in code; Node.js, browser and Python refuse a file made for another model |
| Phrase fields | `name`, `pronunciations`, `threshold_loose` (sensitivity 1), `threshold_strict` (sensitivity 0), `endpoint`; optional `id`, `metadata`, `sensitivity` |
| Public C API | `include/phonlet_wake.h`: `phonlet_wake_*` functions, `pw_*` types. It is the only contract SDKs, demos and tests depend on; `kws_*` is internal |
| Engine backends | One engine build per target (native per platform, WebAssembly scalar and SIMD); the container header selects the kernel path, never a compile flag |
| Prebuilt targets | C API libraries for Linux x64 and Windows x64, the WebAssembly engine, the generator for Linux x64 and as WebAssembly, Python and Node.js packages for Linux x64, the Android AAR. Other targets build from source |
| Licence | PolyForm Noncommercial 1.0.0 for code, model and generator; commercial use by separate licence |
| CI | GitHub Actions workflows run on manual dispatch only; the local suite (`scripts/test.py`) is the merge gate |

## Open work

- [ ] Run each CI workflow once on GitHub.
- [ ] Generator binaries for Windows x64 and macOS arm64 in the release (`generator.yml` with the tag).
- [ ] Publish to package registries (npm, PyPI, Maven, NuGet) with a .NET package that carries the
  native libraries.
- [ ] macOS libraries, manylinux wheels and more Node.js targets.
- [ ] A recognition benchmark with physical microphones (the public six-word benchmark is published,
  see Done).
- [ ] Cross-platform maths: the float model's `tanh`, `log1p` and `exp` differ in the last bits
  between glibc, the Windows runtime and Emscripten (about 1e-7 relative in posteriors; detections
  identical on every recording tested). Bit-identical results need the engine's own
  implementations, which changes the numbers (new goldens, possibly a new model id).
- [ ] Optional: lower the generator's peak memory (about 400 MB) by streaming slot draws.

## Done

- [x] 2026-11-27 The public six-word wake-word benchmark ([`docs/benchmark.md`](docs/benchmark.md),
  `research/benchmark/`): 92.2% at 10 dB noise and 98.6% clean, hit rate at 0.1 false alarms per
  hour, six words typed as text. Verified: the harness reviewed against the benchmark's own code and
  its published reimplementation; the engine's sensitivity grid proven identical to
  `phonlet_wake_process`.
- [x] 0.1.0: engine and C API (`phonlet_wake_keyword_count`, `phonlet_wake_keyword_name`), the
  `en-1` model, one keyword-file format in every SDK, SDKs for Python, Node.js, the browser,
  React, C / C++, Java, Android, .NET and ESP32-S3, the generator 0.2.0 (command line and
  WebAssembly), Windows x64 and WebAssembly builds in the package scripts. Verified with the full
  local suite, the generator's parity suite, and device runs listed in the README's platform table.
