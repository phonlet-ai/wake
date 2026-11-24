# Phonlet Wake for Node.js

Install the SDK, its Linux x64 native add-on and the English model from the [release](https://github.com/phonlet-ai/wake/releases/tag/v0.1.0):

```sh
npm install --ignore-scripts \
  https://github.com/phonlet-ai/wake/releases/download/v0.1.0/phonlet-ai-wake-node-0.1.0.tgz \
  https://github.com/phonlet-ai/wake/releases/download/v0.1.0/phonlet-ai-wake-node-linux-x64-gnu-0.1.0.tgz \
  https://github.com/phonlet-ai/wake/releases/download/v0.1.0/phonlet-ai-wake-model-en-0.1.0.tgz
```

```js
import { Wake } from '@phonlet-ai/wake-node';
import { english } from '@phonlet-ai/wake-model-en';
// english = {model, keywords}: the en-1 model and its English keyword file.
const wake = await Wake.create({model: english.model, keywords: english.keywords, sensitivity: 0.5});
try {
  console.log(wake.process(new Int16Array(512))); // replace silence with your audio
  console.log(wake.finish());
} finally { wake.close(); }
```

CommonJS exposes the same named exports. TypeScript declarations are included. PCM is signed 16-bit, mono, 16 kHz; arbitrary chunks are buffered into 512-sample frames. `finish()` drops an incomplete frame and is idempotent. `close()` releases native resources and is idempotent. Audio-stream timestamps mark a frame's exclusive end. Processing is synchronous; run your instance in a Node worker for applications that cannot block the event loop.

The native package contains one Node-API v8 addon; installation does not compile or download binaries. Missing/unsupported targets fail explicitly. This release is tested on Linux x64 with Node 24 and needs glibc 2.32 and the libstdc++ of GCC 11 or newer (Ubuntu 22.04, Debian 12, RHEL 9 or later); it is not a manylinux or cross-platform release. `build.json` in the native add-on package records the exact build.

`keywords` is a keyword file as the threshold generator writes it: its path, the parsed object, or just the array of phrases written in code. Each phrase carries its pronunciations and calibrated thresholds (`id` defaults to `name`; `metadata` and a per-phrase `sensitivity` are optional):

```js
const wake = await Wake.create({
  model: 'model.pwake',
  keywords: [{name: 'hey kettle', pronunciations: [[16, 13, 20, 11, 31, 3, 21]],
              threshold_loose: -1.21875, threshold_strict: -0.8740234375, endpoint: true, sensitivity: 0.6}],
});
```

A keyword file that names its model (`model.sha256`) is refused for any other model; a bare array is not checked, so keep it with the model it was made for. Unknown keys are errors. Detection needs no lexicon or network. See [phrases and keyword files](../../docs/keywords.md).
