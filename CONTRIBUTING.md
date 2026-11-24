# Contributing to Phonlet Wake

Thank you for helping. Read [AGENTS.md](AGENTS.md) first: it describes the layout, the rules and
how changes are verified. By contributing you agree that your contribution is licensed under the
repository's [licence](LICENSE).

## Ground rules

- The model, the frontend and the scorer are a numerical contract. Changes must leave the golden
  tests unchanged, or come with a new model id.
- The generator must keep giving the reference's thresholds; its parity suite must pass.
- Never store per-frame probabilities, and never add training code or data here.

## Workflow

1. Verify locally with one entry point:

   ```sh
   python scripts/test.py          # everything
   python scripts/test.py --quick  # gate, Python tests, WebAssembly smoke test
   (cd generator && cargo test --release)
   ```

2. CI workflows run on manual dispatch only.

3. `scripts/check-repository.py` checks links, asset hashes and banned words. Fix the cause, never
   the gate.

4. Package with `python scripts/package.py` once the full suite passes; artifacts land in the
   ignored `dist/`.

Report security problems privately, as described in [SECURITY.md](SECURITY.md).
