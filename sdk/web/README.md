# Phonlet Wake for browsers

`phonlet-ai-wake-web-0.1.0.tgz` in the [release](https://github.com/phonlet-ai/wake/releases/tag/v0.1.0) contains the worker, scalar/SIMD WASM runtimes, TypeScript declarations and optional capture helper. The English model is a separate package. Copy package assets to your server, preserving their relative layout.

```js
import {Wake} from './vendor/wake/index.mjs';
const wake = await Wake.create({model:'./assets/model.pwake', keywords:'./assets/keywords.json', sensitivity:0.5});
try {
  console.log(await wake.process(new Int16Array(512)));
  console.log(await wake.finish());
} finally { await wake.close(); }
```

Use localhost or HTTPS, serve `.mjs` as JavaScript and `.wasm` as `application/wasm`, and allow same-origin workers in CSP (`worker-src 'self'`; WASM execution may require `'wasm-unsafe-eval'`). No CDN, activation service, lexicon or enrollment endpoint is contacted. The default backend is scalar; `simd:true` explicitly requests the SIMD build. Await initialization before submitting mono, 16 kHz PCM16.

Worker processing is serialized and each promise resolves after processing. The limit is **4096 unacknowledged samples, including the in-flight call**; split large files into chunks and await each. Input is copied before transferring to the worker. Overload rejects explicitly. `finish()` drains accepted work and discards the partial frame; `close()` releases/terminates the worker. Both are idempotent.

The `./capture` export provides `WakeManager({wake, onDetection, onError, audioContext?, stream?})`. Call `start()`, `stop()`, `close()` and await each. Capture requires an actual 16 kHz AudioContext. Browser downmixing uses one channel; float samples are clamped/rounded to PCM16. Requested echo cancellation, noise suppression and automatic gain control are disabled. There is no SDK resampler. The helper stops owned tracks and closes owned contexts; borrowed resources stay open. Callbacks must be synchronous, and callback failures stop capture through `onError`. Restart creates fresh stream state. Physical microphones and other browsers still need device qualification.

`model` is a URL or bytes. `keywords` is a keyword file as the threshold generator writes it: its URL, the parsed object, or just the array of phrases written in code, each with its pronunciations and calibrated thresholds (a per-phrase `sensitivity` is optional). URLs resolve against the page. A keyword file that names its model (`model.sha256`) is refused for any other model. See [phrases and keyword files](../../docs/keywords.md).
