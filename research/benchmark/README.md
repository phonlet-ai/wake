# Benchmark: Picovoice's wake-word benchmark

`wwb.py` runs Phonlet Wake on [Picovoice's public wake-word benchmark](https://github.com/Picovoice/wake-word-benchmark).
`report.py` turns a run into the tables in [`docs/benchmark.md`](../../docs/benchmark.md) and the chart.

## The test

- **Words:** alexa, computer, jarvis, smart mirror, snowboy, view glass: Picovoice's crowd-sourced
  recordings from more than 50 speakers. Decodable with libsndfile: alexa 315 (14 of its 329 files
  cannot be read), computer 411, jarvis 384, smart mirror 369, snowboy 401,
  view glass 399.
- **Background:** LibriSpeech test-clean, without the utterances whose transcript contains the word.
- **Noise:** channel 1 of DEMAND's 17 environments in the 16 kHz release, at 10 dB SNR; and a clean
  condition without noise.
- **Stream:** 24.7 to 24.95 hours per word and condition, mixed with the recipe of Picovoice's
  `mixer.py` and `dataset.py` and seed 778, with the two corrections of the published reimplementation
  in [Hark's](https://github.com/pioborgelt/hark) `scripts/wwb_eval.py`: a background utterance is
  excluded when its upper-case transcript contains the upper-case word (upstream compares a lower-
  case word with upper-case transcripts, so its exclusion does nothing), and each keyword counts as
  detected at most once, within its window from start to end plus 0.5 s. About a fifth of the
  background time carries LibriSpeech speech; the rest is noise alone (or silence when clean), as in
  Picovoice's stream. DEMAND recordings are 300 s long and repeat: each environment
  comes up about 17 times per stream.
- **Metric:** hit rate at no more than 0.1 false alarms per hour of stream (1 per 10 hours, the
  benchmark's operating point: at most 2 false alarms per word and condition), per word, then the
  mean of the six words.

## Phonlet Wake in the test

- Each word is a keyword file written by the threshold generator from text alone (`wake-threshold
  0.2.0`, the release binary). No recording of any keyword was used, and nothing was tuned on the
  test except the sensitivity.
- Detection is the Phonlet Wake engine through its C API (`libkws_engine`). One pass scores 51
  sensitivities from 0 to 1 (`kws_session_create` with production arbitration); `wwb.py --check`
  proves it fires on exactly the frames the public API (`phonlet_wake_process`) fires on.
- Numbers per condition: the best operating point per word with the threshold swept as for the
  other engines; the best within the shipped sensitivity range; one sensitivity for all six words;
  and sensitivity 0, the calibrated setting that aims at 0.1 false alarms per hour of speech, with no
  tuning at all.
- The shipped range stops at sensitivity 0. Where a word cannot meet the cap there, the sweep is
  extended with the generator's documented stricter target (`--rate-strict 0.001`, so that its
  sensitivity 1 equals the default file's sensitivity 0) and the same stream is scored again
  (`report.py --extra`). Values from that range are marked.

## What the model had seen

- The `en-1` acoustic model was trained on LibriSpeech's train splits, Common Voice and GigaSpeech,
  with MUSAN noise and simulated rooms. LibriSpeech test-clean, DEMAND and Picovoice's recordings
  were never used in training.
- The threshold generator's calibration statistics come in part from LibriSpeech dev and test
  speech: one half of their speakers, chosen by `sha256("libri_heldout:<speaker>") % 2 == 0`. For
  test-clean that is 14 of its 40 speakers (61, 237, 908, 1089, 1320, 2300, 3570, 4446, 4507, 5105,
  6829, 7021, 7176, 7729) and 959 of its 2,620 utterances, among the 72,656 utterances of those
  statistics. That can only matter for the sensitivity-0 number, whose thresholds were set without
  this test but on statistics that include some of its background speakers; the sweep numbers
  choose their operating point on the test itself, as every engine's do.
- The benchmark words were not chosen by us, and nothing about them was tuned: the generator wrote
  each from its text (snowboy's pronunciation is predicted; the rest come from its dictionary).

## Run it

Data (not included; read-only, any location): Picovoice's repository (its `audio/` folder only;
none of its code runs), LibriSpeech test-clean from OpenSLR, DEMAND 16 kHz from Zenodo
(record 1227121), laid out as

```
CORPORA/picovoice-wake-word-benchmark/repo/audio/<word>/*.wav
CORPORA/librispeech/LibriSpeech/test-clean/<speaker>/<chapter>/*.flac
CORPORA/demand-16k/<ENVIRONMENT>/ch01.wav
```

```sh
wake-threshold alexa computer jarvis "smart mirror" snowboy "view glass" --out research/data/wwb/keywords.json
python research/benchmark/wwb.py --corpora CORPORA --keywords research/data/wwb/keywords.json \
  --lib build/native/libkws_engine.so --model resources/models/en-1/model.pwake \
  --out research/runs/wwb/run.json --check          # the grid equals the public API
python research/benchmark/wwb.py ... --out research/runs/wwb/run.json --workers 4   # about 10 min per stream
# a word that cannot meet the cap at sensitivity 0: extend its sweep with a stricter target
wake-threshold "view glass" --rate-loose 0.1 --rate-strict 0.001 --out research/data/wwb/view-glass-strict.json
python research/benchmark/wwb.py ... --keywords research/data/wwb/view-glass-strict.json \
  --words "view glass" --snrs 10 --out research/runs/wwb/run-strict.json
python research/benchmark/report.py research/runs/wwb/run.json --extra research/runs/wwb/run-strict.json \
  --markdown out.md --svg chart.svg --json summary.json
```

Needs NumPy and soundfile. Each stream is about 5.7 GB of float32 while it is scored; it is deleted
afterwards and rebuilt identically from the seed.
