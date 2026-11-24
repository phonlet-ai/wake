# wake-threshold: the Phonlet Wake threshold generator

One self-contained executable that turns wake-phrase text into a keyword file: pronunciations
and calibrated thresholds for the Phonlet Wake `en-1` model, in the layout every Phonlet Wake SDK
reads as it is. No Python, compiler, model file, corpus or server is needed: the evidence bundle,
lexicon and G2P fallback are embedded and verified at build time. The same code also builds as
WebAssembly for a generator web page with no backend ([WebAssembly](#webassembly)).

It is a Rust port of Phonlet's Python enrollment reference and gives the same values (see
[Parity](#parity)). Prebuilt: a static Linux x64 binary and the WebAssembly module in the
[latest release](https://github.com/phonlet-ai/wake/releases/latest); build any other platform
from this directory with Rust.

## Use

```sh
wake-threshold "hey computer"                      # keyword file to standard output
wake-threshold "hey computer" jarvis -o keywords.json
wake-threshold "snowboy=S N OW B OY|S N OW B OW"     # explicit ARPAbet pronunciations
wake-threshold --file phrases.txt                  # one phrase per line ('-' = stdin)
wake-threshold "alexa" --endpoint --rate-loose 3 --rate-strict 0.03
wake-threshold pronounce snowboy                   # pronunciations only (fast)
wake-threshold info                                # model, bundle and policy identities
```

`enroll` is the default command. Options: `-o/--out FILE`, `--compact` (one-line JSON),
`--endpoint`, `--rate-loose`/`--rate-strict` (false alarms per hour of speech at sensitivity 1 / 0;
default 10 and 0.1; `--rate-a`/`--rate-b` still work), `--threads`, `--strict`, `--min-phonemes`,
`--max-phonemes` (see [Phrase quality](#phrase-quality)). Exit status: 0 when at least one phrase
was enrolled, 1 when none could be, 2 on a usage or input error. Phrases that cannot be enrolled
are listed in `metadata.unknown` and repeated on standard error.

Output, a keyword file (abridged):

```json
{
  "model": {"id": "en-1", "sha256": "8be3687d…4af8"},
  "keywords": [
    {"name": "hey computer",
     "pronunciations": [[16, 13, 20, 3, 22, 27, 37, 34, 31, 12]],
     "threshold_loose": -1.4384765625, "threshold_strict": -0.9716796875, "endpoint": false,
     "metadata": {"source": "lexicon",
                  "expected_error": {"threshold_loose": 34.9, "threshold_strict": 61.2, "default": 44.3},
                  "fa_factor_at_default": 1.52, "flag": false, "warnings": []}}
  ],
  "metadata": {"generator": "wake-threshold 0.2.0", "rate_loose": 10.0, "rate_strict": 0.1,
               "unknown": [], "resolved": [{"text": "hey computer", "source": "lexicon", …}]}
}
```

Pass the file to any SDK: `Wake.create(model="model.pwake", keywords="keywords.json")` in Python,
the same in Node.js and the browser, or its text to the C API. Thresholds are in engine units
divided by 1024; `threshold_loose` applies at sensitivity 1, `threshold_strict` at 0. `model`
names the `en-1` model file (`8be3687d…`), which carries the weights of the source model the
evidence bundle pins (`77164377…`, shown by `info`). In each phrase's `metadata`, `source` says
where its pronunciations came from (`lexicon`, `g2p` for a predicted one, `explicit`), and `flag`
marks phrases whose expected threshold error at the default sensitivity exceeds 80 units.

## Phrase quality

Every enrolled phrase is checked; problems are listed in the phrase's `metadata.warnings` (and on
standard error). The thresholds themselves stay exactly as the reference produces them.

| Code | When | Why it matters |
| --- | --- | --- |
| `too_short` | a pronunciation has fewer than 4 phonemes (`--min-phonemes`) | occurs inside ordinary words and speech |
| `too_long` | a pronunciation has more than 12 phonemes (`--max-phonemes`) | beyond the calibration phrases (mean 9.5 phonemes); thresholds are extrapolated |
| `too_common` | `threshold_loose` above -0.75 | the phrase is so frequent that it fires whenever it is said, even at full sensitivity |
| `strict_at_ceiling` | `threshold_strict` is 0 | at sensitivity 0 only a perfect match fires |
| `uncertain` | `flag`: expected threshold error over 80 units at the default sensitivity | the thresholds may be far off |

By default warned phrases are still enrolled. With `--strict` they are rejected instead: they
move to `metadata.unknown` with the reasons, and the exit status is 1 if nothing remains.
Examples: "the", "a" and "read" are `too_short`, `too_common` and `uncertain`; "hey there my good
computer friend" (23 phonemes) is `too_long` with `strict_at_ceiling`; "hey computer", "alexa" and
"okay google" pass.

These limits are this product's policy, not part of the reference calibration.

## How it works

1. Each word is looked up in the frozen 394,409-word training lexicon; absent words go to the
   pinned `g2p_en` 2.1.0 fallback (its homograph table, then its neural model; its CMUdict copy is
   left out, since every CMUdict word the generator can look up is in the lexicon but 16 misspellings).
2. The calibration text (72,656 utterances, 2 draws, 104.7 h) is realised from the two evidence
   stores with NumPy's exact random streams, once per run (about 0.5 s).
3. Each pronunciation is composed over that text; the false-alarm curve feeds the saved ridge
   calibration, giving a threshold ladder over 0.03–30 FA/h, interpolated at the policy rates.

Typical cost: about 1 s for one phrase, 3 s for five, about 400 MB peak memory.

## Build

```sh
cargo build --release                                       # target/release/wake-threshold
cargo build --release --target x86_64-unknown-linux-musl    # static Linux binary
cargo test --release                                        # parity suite
```

`build.rs` checks every file in `assets/` against the bundle and G2P manifests (a mismatch fails
the build) and packs what the binary needs into deflated blobs. Windows x64 and macOS arm64
binaries are built by the manually triggered workflow `.github/workflows/generator.yml`.

## WebAssembly

```sh
cargo rustc --lib --release --target wasm32-unknown-unknown --crate-type cdylib
# target/wasm32-unknown-unknown/release/wake_threshold.wasm (about 18.7 MB, data included)
```

`src/wasm.rs` is the whole interface, with no bindings library: `wt_alloc(n)` gives a buffer for
a UTF-8 JSON request, `wt_call(ptr, n)` runs it and returns the response length, `wt_result()`
points at the response. Requests are `{"command": "enroll", "phrases": [...]}` with the CLI's
options (`endpoint`, `rate_loose`, `rate_strict`, `strict`, `min_phonemes`, `max_phonemes`), or
the commands `pronounce`, `info` and `load` (loads the calibration data ahead of the first phrase).
The response is the CLI's output for the same command, or `{"error": "..."}`. WebAssembly has no
threads, so the tasks that run on threads natively run one after another (`src/par.rs`); results
are identical. In Node 24 the first request takes about 3.5 s, then about 2.5 s per phrase, with
about 370 MB of memory. Run it in a Web Worker.

All 26 recorded enrollment responses give identical thresholds and pronunciations in
WebAssembly. One diagnostic differs in its last digit: `fa_factor_at_default` for the 23-phoneme
"hey there my good computer friend" (5.4e16), because WebAssembly brings its own `exp()`.

## Release

The generator ships in the Phonlet Wake release: the static Linux binary
(`cargo build --release --target x86_64-unknown-linux-musl`) and the WebAssembly module, listed
in `SHA256SUMS`. For Windows x64 and macOS arm64 binaries, run the workflow with the release tag;
each platform runs the parity suite, builds and smoke-tests its binary, and uploads it to that
release:

```sh
gh workflow run generator.yml -f tag=v0.1.0
```

Without a tag the workflow only keeps the binaries as run artifacts.

## Parity

`tests/parity.rs` compares against `tests/fixtures/goldens.json`, recorded from the Python
reference implementation (its commit and environment are in the fixture).
Exact: SeedSequence/PCG64 streams, `integers`, `random`, `choice`; all 4,028,806 slot records and
the text hours; composed scores, curves and features; 4,019 G2P fallback words; resolver results
and error messages; 26 complete enrollment responses (compared in the reference's field names;
the warnings, pronunciation sources and generator name are this product's additions). The ridge ladder and uncertainty match to 1e-9 relative (NumPy's BLAS may sum in another order); every reported field matches exactly.

Known differences: error messages for exotic non-ASCII digits may name a different reason (both
reject the phrase); a phrase with more than 2^128 pronunciation combinations is rejected.

## Updating the bundle

Calibration bundles come from Phonlet's training pipeline. A new bundle replaces `assets/bundle/`
as a whole, with goldens regenerated from the reference implementation, and the parity suite must
pass before it ships.

## Licence

`wake-threshold` and its embedded data are licensed under the [PolyForm Noncommercial License
1.0.0](../LICENSE); commercial use needs a separate licence, see [COMMERCIAL.md](../COMMERCIAL.md) or write
to contact@phonletai.com. Third-party components and data are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
