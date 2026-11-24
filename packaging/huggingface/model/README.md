---
license: other
license_name: polyform-noncommercial-1.0.0
license_link: https://polyformproject.org/licenses/noncommercial/1.0.0
language:
- en
library_name: phonlet-wake
pipeline_tag: audio-classification
tags:
- wake-word
- keyword-spotting
- hotword
- speech
- on-device
- offline
- tinyml
- esp32
- webassembly
- mingru
datasets:
- openslr/librispeech_asr
- speechcolab/gigaspeech
---

# Phonlet Wake `en-1`

![Phonlet Wake](https://phonletai.com/og.png)

On-device wake-word detection for any English phrase you can type. This one model of about 1 MB
listens for several phrases at once, offline, with a sensitivity for each. A new phrase is made from
text in seconds by the threshold generator: no recordings, no training run, no cloud.

**[Try it in this browser](https://huggingface.co/spaces/phonletai/wake)** ·
[Website](https://phonletai.com) · [Docs](https://phonletai.com/docs/) ·
[Source and SDKs](https://github.com/phonlet-ai/wake) ·
[Instructions for AI coding agents](https://phonletai.com/agents.md)

## Footprint

| | |
| --- | --- |
| Model | 1,013,825 bytes of int8 weights, the same file for every phrase |
| Memory while listening | about 1.05 MB (C API, Linux x64, four phrases); each extra phrase about 1.5 KB |
| Laptop | 0.7% of one core natively (AMD Ryzen 7 7735HS); about 8% in a browser tab (WebAssembly) |
| ESP32-S3 | 19% of one 240 MHz core; 75 KB of internal RAM plus 934 KB of PSRAM |

## How it works

The engine turns 16 kHz mono audio into a 40-band log-mel spectrogram, runs a streaming minGRU
acoustic model (6 layers, 162 units, int8 weights) that outputs the probabilities of 39 English
phones every 32 ms, and scores each phrase's pronunciations against them. A phrase fires when its
score passes the threshold set by its sensitivity.

A phrase is data, not a model: its pronunciations (phone ids) and two calibrated thresholds, kept
in a keyword file. The threshold generator makes that file from text: it looks words up in a
394,409-word dictionary, predicts pronunciations for the rest, and calibrates each phrase's
thresholds to a false-alarm target, from about 10 per hour of speech at sensitivity 1 to 0.1 at
sensitivity 0.

## Use it

The `.pwake` file runs with the Phonlet Wake engine, not with Transformers. Every SDK reads the
model and a keyword file.

1. **Make a keyword file.** Type your phrases in the
   [generator](https://phonletai.com/generator/) (it runs in your browser) or use the
   command-line generator from the [release](https://github.com/phonlet-ai/wake/releases/latest):

   ```sh
   ./wake-threshold-0.2.0-x86_64-unknown-linux-musl "hey nova" "okay kettle" --out keywords.json
   ```

2. **Install an SDK** and listen, for example in Python:

   ```sh
   pip install huggingface_hub \
     https://github.com/phonlet-ai/wake/releases/download/v0.1.0/phonlet_wake-0.1.0-py3-none-linux_x86_64.whl
   ```

   ```python
   from huggingface_hub import snapshot_download
   from phonlet_wake import Wake

   repo = snapshot_download("phonletai/wake-en-1")  # the model, its config and English phrases
   with Wake.create(model=f"{repo}/phonlet-wake-en-1.pwake", keywords="keywords.json",
                    sensitivity=0.5) as wake:
       for pcm in microphone_chunks():  # 16 kHz mono int16, any chunk size
           for hit in wake.process(pcm):
               print("Wake word:", hit.keyword, hit.time_seconds)
   ```

SDKs: Python, Node.js, the browser (WebAssembly), React, C / C++ (Linux x64 and Windows x64
libraries), Java, Android, .NET and ESP32-S3. Code for each is in the
[docs](https://phonletai.com/docs/).

## Files

| File | What it is |
| --- | --- |
| `phonlet-wake-en-1.pwake` | The model, SHA-256 `8be3687d060f953d0069480cf53d1fd0aa66e04551c2a4b600823a992a864af8` |
| `phonlet-wake-en-1-keywords.json` | Five English phrases as a keyword file: hey phonlet, ok phonlet, hey assistant, assistant, hey wake |
| `config.json` | What the model expects and outputs: audio format, frontend, architecture, classes |
| `LICENSE` | PolyForm Noncommercial License 1.0.0 |
| `THIRD_PARTY_NOTICES.md` | The data the model was trained on and the credit each source asks for |

## Training data

The model was trained from random initialisation, with no pretrained or teacher model. It contains
no audio, transcripts or features from this material:

| Corpus | Hours | Terms |
| --- | ---: | --- |
| Mozilla Common Voice 22.0, English, train | 1,521.2 | CC0 1.0 |
| LibriSpeech, train-clean-100, train-clean-360, train-other-500 | 961.1 | CC BY 4.0 |
| GigaSpeech, L subset | 2,368.8 | SpeechColab terms of access: non-commercial research and educational use |
| MUSAN music, speech and noise (augmentation) | 109.3 | CC BY 4.0 |
| Room impulse responses (augmentation) | 100,000 rooms | Simulated by Phonlet AI |

Pronunciations come from the LibriSpeech lexicon, the CMU Pronouncing Dictionary and g2p_en. Full
credits and licence texts are in `THIRD_PARTY_NOTICES.md`.

## Evaluation and limitations

- No recognition benchmark is published yet. Each phrase comes with its calibrated false-alarm
  target and an expected calibration error; how well it is heard depends on the phrase, the
  speaker and the room, so test yours in the [Space](https://huggingface.co/spaces/phonletai/wake).
- English only. Short or common words ("the", "read") fire too often; good phrases have two to
  four syllables and sounds that are rare in everyday speech.
- Audio must be 16 kHz mono 16-bit PCM; the SDKs never resample.

## Licence

PolyForm Noncommercial License 1.0.0: free for personal use, study, research with no commercial
aim, charities, schools, public research and government. A product you sell, a device you ship or
a service your company runs needs a commercial licence: write to contact@phonletai.com.
