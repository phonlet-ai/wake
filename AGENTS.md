# AGENTS.md — Phonlet Wake

Read this, then [PLAN.md](PLAN.md) (decisions and open work), before changing anything. Using
Phonlet Wake in your own project rather than changing it? Read
[phonletai.com/agents.md](https://phonletai.com/agents.md) instead.

## What this repository is

Phonlet Wake: a C++17 engine that detects wake phrases on the device, the `en-1` acoustic
model, the English keyword file, an SDK per platform, and `wake-threshold`, the generator that
turns typed phrases into keyword files (`generator/`, Rust, also built as WebAssembly). How
detection works is in [docs/engine.md](docs/engine.md); the keyword file is in
[docs/keywords.md](docs/keywords.md).

It holds no training code or training data.

## Layout

| Path | What |
| --- | --- |
| `engine/` | Engine source (`src/`, `include/kws/`), `sources.txt` (the one source list every build uses), CMake, the WebAssembly build (`wasm/`), phone inventories |
| `include/phonlet_wake.h` | The public C API, the only contract SDKs, demos and tests rely on |
| `sdk/<platform>/` | One SDK per platform. `sdk/shared/contracts.mjs` is the one source of the Node.js and browser validation; `scripts/sync-js.py` writes their copies |
| `sdk/esp32/wake/` | ESP-IDF component with its own S3 acoustic engine (`s3/`), a menuconfig option and a host comparison (`test/compare.cc`) |
| `demos/<platform>/` | One runnable example per SDK, plus the ESP32 microphone check |
| `resources/` | `models/en-1/model.pwake` and the English keyword file `keywords/en.json` |
| `generator/` | `wake-threshold`: phrases in, keyword file out; embedded calibration data and its parity tests ([its rules](generator/AGENTS.md)) |
| `packaging/` | Model and native-library package recipes |
| `docs/` | How it works, phrases, the SDK contract, development |
| `scripts/` | `test.py` (the suite), `check-repository.py` (the gate), `package.py`, `build-windows.py`, `verify-install.py`, `sync-js.py`, `clean.py` |
| `tests/` | Python tests, golden fixtures, the Node.js conformance script, the browser check |

## Rules

- **The numbers are a contract.** The frontend, the scorer, the detector and the model must give
  identical results on every platform. Keep the strict floating-point flags
  (`-ffp-contract=off -fno-fast-math`). A change to engine maths must leave the golden tests
  unchanged, or ship as a new model id. The ESP32-S3 acoustic engine is the one approximation; it is
  checked against the float engine with `sdk/esp32/wake/test/compare.cc` on recorded audio.
- **The model and the English phrases are frozen.** The gate pins the SHA-256 of
  `resources/models/en-1/model.pwake` and `resources/keywords/en.json`. A new model gets a new id
  and its own calibrated phrases.
- **SDKs are thin.** They never score keywords, change thresholds or resample audio; they call the
  engine. Every SDK accepts the keyword file (by path or URL, as an object, or as phrases written
  in code); a phrase is never usable without its thresholds.
- **Clean tree.** No history, provenance or receipt files; no other vendors' product names and no
  words about taking software apart. The gate holds the banned words as hashes and reports file and
  line. Fix the text, never the gate. Cite published methods in `docs/engine.md`.
- **No per-frame probabilities on disk**, in the repository or anywhere else.
- **Releases are deliberate.** Push, tag and publish only when the maintainers ask. CI workflows
  run on manual dispatch only; the local suite is the merge gate.
- **Claims match evidence.** A platform is "tested" only after its SDK has run real audio on that
  platform; update the README's platform table and PLAN.md with what was run.

## Verify a change

```sh
python scripts/check-repository.py        # gate: links, pinned hashes, banned words
python -m pytest -q                       # engine golden tests and Python SDK
python scripts/test.py --quick            # gate, tests, WebAssembly smoke test, React
python scripts/test.py                    # also Java, packaging and install checks
```

The suite needs NumPy and pytest (`uv run --no-project --with numpy --with pytest ...` works
without installing them). Per platform:

| Change in | Also run |
| --- | --- |
| `sdk/shared/contracts.mjs` | `python scripts/sync-js.py`, then `tests/conformance.mjs` against the Node.js SDK |
| `sdk/java/` (shared by Android) | `JAVA_HOME=<JDK 17> python scripts/test.py --stage java` |
| `engine/` | The full suite; a WebAssembly rebuild needs Emscripten 6.0.9 (`engine/wasm/build.sh`) |
| `sdk/esp32/` | Build `demos/esp32` with ESP-IDF 5.4; for the S3 engine, the host comparison in its README, and a run on a board |
| `generator/` | `cargo test --release` in `generator/` (the parity suite) |

Device claims need the device: an ESP32-S3 with PSRAM and an I2S microphone (INMP441) for the
ESP32 SDK, an Android phone for Android, Windows with .NET 8 for .NET.

## Commits

Small and focused; the message says what changed and what verified it. Mark finished PLAN.md
items with the date and the verification. Never commit build output (`build/`, `dist/`,
`sdkconfig`, `managed_components/`) or copied demo assets (gitignored).
