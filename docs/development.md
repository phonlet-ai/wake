# Developing and packaging Wake

[Documentation](README.md) / Development

Engine source, SDK code, tests, the model and brand assets all live in this repository; builds never import another repository. [AGENTS.md](../AGENTS.md) lists the rules.

## Toolchain

The packager targets Linux x64, and adds the Windows x64 library when it runs in WSL on a Windows host with Visual Studio (see below). Install Python 3.11+, NumPy, pytest, setuptools and wheel, CMake, a C++17 compiler, Node 22+ with Node-API headers, and Emscripten 6.0.9 to rebuild the WebAssembly engine. Tests have been run on Node 24 and CPython 3.12.

```sh
python -m pip install numpy pytest setuptools wheel
```

`engine/wasm/build.sh` looks for Emscripten in `~/tools/emsdk`; point `EMSDK` elsewhere if needed. Node headers are resolved from the Node installation; override their directory with `NODE_API_INCLUDE` when necessary. Builds use at most two compiler workers.

## Windows x64 library

From WSL on a Windows host with Visual Studio 2022 or later (C++ x64 tools), `python scripts/build-windows.py` writes `kws_engine.dll`, its import library `kws_engine.lib`, `phonlet_wake.h` and `build.json` to `build/windows-x64/`. It compiles `engine/sources.txt` with MSVC (`/O2 /fp:strict`, static C runtime, `/utf-8`) in a work directory on the Windows side (`%TEMP%\phonlet-build\wake` unless `--work-dir` or `WAKE_WINDOWS_WORK` names another). It exports the `phonlet_wake_*` and `kws_*` functions, and fails if the DLL imports anything besides KERNEL32 or the compiled code contains fused multiply-add instructions. A Windows SDK registered with Visual Studio is used when present, otherwise the NuGet layout at `C:\_tools\windows-sdk` (`--windows-sdk` or `WAKE_WINDOWS_SDK`). `python scripts/package.py` builds it and adds `dist/wake-native-0.1.0-windows-x64.zip` when WSL reaches such a host (`--windows require` or `--windows skip` to decide explicitly).

## Check and build

The complete suite has one entry point, which the CI workflows run as well:

```sh
python scripts/test.py            # full suite: gate, tests, web smoke, packaging, install
python scripts/test.py --quick    # gate + tests + web smoke only
python scripts/test.py --stage package   # a single stage
```

The individual commands behind those stages:

```sh
python scripts/check-repository.py
python -m pytest -q
python scripts/package.py
node sdk/web/test/smoke.mjs
node sdk/web/test/smoke.mjs --simd
python scripts/verify-install.py --types
```

The repository check catches broken local documentation and image links, empty source directories, changes to the brand assets or the production model, and banned words (kept as hashes in the script, so the repository never spells them out). It never changes files.

The packager emits separate SDK, model, native, demo and source archives in `dist/`, with a hash/size manifest in `dist/release.json`. It never publishes packages. `--skip-wasm` reuses the checked-in WASM binaries; use a full build after engine changes. New files must be tracked before making a source archive, and packaging rejects untracked source files.

`verify-install.py` uses independent consumer directories. It installs local npm archives without lifecycle scripts, installs Python wheels and checks inference with no compiler on PATH, and builds the browser demo against installed packages. It may fetch declared Python dependencies and, with `--types`, TypeScript.

## Hugging Face

`scripts/publish-huggingface.sh` publishes the model repo `phonletai/wake-en-1`, the lab Space
`phonletai/wake` and the organisation card from the files of a GitHub release (checked against its
`SHA256SUMS`) and `packaging/huggingface/`. `--stage DIR` builds both trees without uploading; serve
`DIR/space` to test the lab locally. Uploading needs `hf auth login` with write access to the
`phonletai` organisation.

## Focused development

For Python, use `python -m pip install -e ./sdk/python`. From a checkout, `python run.py info` and `python run.py detect input.wav` run the Python SDK's command-line tool without installing it.

For native code:

```sh
cmake -S engine -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native -j2
```

For the browser demo, a full package build prepares `demos/web/dist/`. Serve it on localhost:

```sh
python -m http.server 8080 --bind 127.0.0.1 --directory demos/web/dist
```

For an SDK-only change, follow that SDK's [Node](../sdk/nodejs/README.md), [Web](../sdk/web/README.md) or [Python](../sdk/python/README.md) guide. Keep shared JavaScript validation in `sdk/shared/contracts.mjs`; `scripts/sync-js.py` produces the owned copies shipped in the Node/Web packages.

## Source files and generated files

| Path | Treatment |
| --- | --- |
| `engine/`, `sdk/`, `demos/`, `packaging/` | Owned source and package recipes |
| `sdk/web/dist/` | **Tracked** scalar/SIMD WASM and loaders used by the Web SDK |
| `resources/` | The `en-1` model and the English keyword file (the gate pins their hashes) |
| `assets/brand/` | Phonlet logos and design tokens (the gate pins their hashes) |
| `build/`, `engine/build/`, `engine/wasm/build/` | Ignored compiler and package staging output |
| `sdk/python/build/`, `*.egg-info/` | Ignored Python packaging output |
| `demos/*/dist/` | Ignored standalone demo builds |
| `dist/` | Ignored local release archives and their receipt |

To inspect or remove reproducible build intermediates:

```sh
python scripts/clean.py
python scripts/clean.py --apply
```

Cleanup keeps `dist/`, the tracked WebAssembly runtimes, the source and the model and phrase files.

## Changes

Keep commits small and say what verified each one. Engine scoring, the frontend and the model are a
numerical contract: changes to them must leave the golden tests unchanged, or come with a new model
id. Never store per-frame probabilities, and never add training code or data to this repository. A
platform counts as tested only once its SDK has run real audio on that platform.
