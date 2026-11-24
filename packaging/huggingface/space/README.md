---
title: Phonlet Wake
emoji: 🎙️
colorFrom: red
colorTo: gray
sdk: static
app_file: index.html
pinned: true
license: other
short_description: Type any wake word, hear it detected live in your browser
tags:
- wake-word
- keyword-spotting
- hotword
- webassembly
- on-device
models:
- phonletai/wake-en-1
---

# Phonlet Wake lab

Type any phrase: the threshold generator calibrates it in your browser (WebAssembly), and the
Phonlet Wake engine listens for it on your microphone with the 1 MB `en-1` model. Audio never
leaves the tab. Download the keyword file and use it with any Phonlet Wake SDK.

Model: [phonletai/wake-en-1](https://huggingface.co/phonletai/wake-en-1) · Source and SDKs:
[phonlet-ai/wake](https://github.com/phonlet-ai/wake) · Website: [phonletai.com](https://phonletai.com)

Built from `packaging/huggingface/space/` in the wake repository: the engine and the generator are
the files of Phonlet Wake release v0.1.0, and the model loads from
[phonletai/wake-en-1](https://huggingface.co/phonletai/wake-en-1). Licence: PolyForm
Noncommercial 1.0.0.
