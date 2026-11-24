# Phonlet Wake browser demo

Choose wake words, set each sensitivity, and use your microphone or a WAV file. The demo uses the official Phonlet artwork and processes audio locally.

## Prebuilt demo

Extract `wake-demo-web-0.1.0.tar.gz`, change into `wake-demo-web`, then:

```sh
python -m http.server 8080 --bind 127.0.0.1
```

Open `http://localhost:8080`. The archive includes the scripts, scalar/SIMD WASM runtimes, model, keyword pack, styles, logos, favicon and license. It needs no CDN or enrollment service. The included `README.txt` gives the same standalone instructions.

## Build from this checkout

Install the Web SDK and model archives from the release into this directory:

```sh
npm install --ignore-scripts /path/to/phonlet-ai-wake-web-0.1.0.tgz /path/to/phonlet-ai-wake-model-en-0.1.0.tgz
npm run build
npm start
```

The build copies the owned artwork from `assets/brand/` at the repository root. `build.mjs` also accepts explicit SDK, model-package and brand directories as its three positional arguments for isolated consumer builds. It replaces only its generated `dist/` output on every build, so removed files do not linger in a release.

## Controls

Select one or more words. The default sensitivity applies until you adjust a word's slider. **Reset overrides** returns all sliders to that default. Higher sensitivity can increase false activations; it is not a confidence score. Settings are fixed while processing.

Stop releases the capture worker, context and tracks owned by the demo. Start creates a fresh session. The list retains the latest 100 detections. The optional SIMD checkbox is under **Browser requirements & options**.

Microphone capture requires localhost/HTTPS and a 16 kHz AudioContext. Files must be mono 16 kHz PCM16 WAV. Errors are shown in the page. No audio is uploaded by this demo.

The demo offers the English pack's five phrases.
