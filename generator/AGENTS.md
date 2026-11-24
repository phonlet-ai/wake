# Generator rules

Read the repository's [AGENTS.md](../AGENTS.md) first; its rules apply here too.

- **Thresholds are a contract.** For the embedded bundle the generator gives exactly the Python
  reference's values. Any change to enrollment code runs `cargo test --release` (the parity
  suite). A changed output needs goldens regenerated from the reference, never hand-edited
  expected values.
- **Assets are verified.** `build.rs` checks every file in `assets/` against its manifest's
  SHA-256 and fails the build on a mismatch. A new bundle replaces `assets/bundle/` as a whole.
- **The output is the keyword file** every SDK reads as it is (`../docs/keywords.md`); keep the
  `keyword_file_layout` test in step with it.
- **WebAssembly** is `src/wasm.rs`; `src/par.rs` runs the threaded tasks one after another on
  wasm32, with identical results.
