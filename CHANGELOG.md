# Changelog

## 0.1.0 (2026-11-24)

- C++17 engine with a public C API: streaming log-mel frontend, the `en-1` minGRU acoustic model
  (int8 weights, about 1 MB), keyword scorer and detector with a sensitivity per phrase.
  `phonlet_wake_keyword_count` and `phonlet_wake_keyword_name` read a keyword file's phrases.
- Phrases are data: pronunciations plus two calibrated thresholds in one keyword file, which
  every SDK takes as a path or URL, a parsed object, or phrases written in code. A file names its
  model by SHA-256; the Node.js, browser and Python SDKs refuse it for any other model.
- SDKs for Python, Node.js, the browser (WebAssembly, scalar and SIMD), React, C / C++, Java,
  Android, .NET and ESP32-S3; prebuilt libraries for Linux x64 and Windows x64.
- ESP32-S3: an acoustic engine on the chip's vector instructions, about 19% of one core with the
  default four-frame batch; the batch is a menuconfig option trading CPU for delay.
- `wake-threshold` 0.2.0, the threshold generator: phrases in, keyword file out, with a quality
  check per phrase; a static Linux binary and a WebAssembly module.
- English keyword file with five phrases.
