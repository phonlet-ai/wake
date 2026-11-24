# ESP-IDF component: Phonlet Wake for ESP32-S3

Compiles the shared engine (frontend, keyword scorer, fire policy) and adds an
ESP32-S3 acoustic engine in [`s3/`](s3/) that replaces the float model's
fp32 fused multiply-adds with the S3 vector unit. `esp_wake_begin` self-tests
the SIMD kernel against plain C and enables the S3 engine; on a mismatch, or
for a model it does not support, the engine's float path runs instead.

## Audio input

`esp_wake_feed` takes one mono stream of 16 kHz, 16-bit PCM, any number of
samples per call. Microphone handling before that is the application's:

- **One I2S microphone** (e.g. INMP441): see [`demos/esp32`](../../../demos/esp32)
  for 32-bit slots and the gain shift, and `demos/esp32-mic-check` to verify the
  wiring.
- **Two microphones:** both share SCK, WS and SD, one with L/R tied to GND and
  the other to 3.3 V; read the I2S port in stereo and turn the two channels
  into one before feeding it (average them, pick the louder one, or run a
  beamformer such as Espressif's audio front end, ESP-SR AFE, which costs
  extra CPU of its own). The engine runs once on the result.
- **Voice activity, echo cancellation, noise suppression:** also upstream of the
  engine if a product needs them.

## S3 acoustic engine

- **Weights:** the model's int8 values, unchanged, repacked once into 16-byte
  rows (zero padding), internal SRAM first, PSRAM for the rest.
- **Activations:** each matrix input is quantised per vector to 16 bits and
  split into two signed bytes, so `EE.VMULAS.S8.ACCX` (16 int8 products per
  instruction) keeps about 15 bits of precision. Integer sums are exact.
- **Batching:** B frames per pass over the weights (menuconfig: Phonlet Wake ->
  `ESP_WAKE_S3_BATCH`, 1 to 8, default 4), so each row is fetched from PSRAM
  once per batch. Posteriors leave with a fixed (B - 1) x 32 ms delay; see
  [Choosing the batch](#choosing-the-batch).
- **Preloading:** while the vector unit works on 4 KB of weight rows, the data
  cache preloads the next 4 KB from PSRAM (`Cache_Start_DCache_Preload`, as
  ESP-IDF's RGB LCD driver does for frame buffers).
- **Float work:** norms, gates, recurrence and softmax stay fp32. Sigmoid and
  tanh come from an 8 KB interpolated table in internal RAM (error under
  2e-5); exp and 1/x in the softmax are division-free (the S3 FPU has no divide
  instruction). The working buffers sit in internal RAM, not PSRAM.
- **Scorer and frontend:** the shared engine's log quantiser is a table built
  once at startup (no double-precision maths per frame, no function-local
  static, which takes a lock on every call under ESP-IDF); results are
  bit-identical on every input.

Measured on an ESP32-S3-N16R8 (240 MHz, octal PSRAM 80 MHz) with the demo's
six keywords:

| Engine | ms per 32 ms frame | share of one core |
| --- | --- | --- |
| float model (portable kernel), 2026-10-30 | 45.2 | 141% (not real time) |
| S3 engine, batch 4, 2026-10-30 | 11.9 (acoustic 9.4, of which SIMD kernels 4.8) | 37% |
| S3 engine, batch 4, 2026-10-31 | 6.2 (acoustic 5.4, of which SIMD kernels 3.1) | 19% |

The 2026-10-31 steps, each measured on the board: scorer and frontend log
table 13.2 -> 10.1 ms, sigmoid table 10.1 -> 8.0, internal working buffers
8.0 -> 7.4, PSRAM preload 7.4 -> 6.2. Worst frame (the batch frame) 22.5 ms.
Memory: weights 75 KB internal + 934 KB PSRAM; 53 KB internal RAM left free
for the application (81 KB before the table and buffers). A larger instruction
cache and quad flash were tried and changed nothing (the hot code stays cached).
The frontend's FFT loads each twiddle once per stage (ESP-DSP's loop order,
same arithmetic, golden mel hash unchanged): 0.42 -> 0.38 ms per frame.

## Choosing the batch

Less CPU or a faster response: measured on the same board with the demo's six
keywords, 2026-10-31 (ms per 32 ms frame):

| `ESP_WAKE_S3_BATCH` | extra detection delay | average | share of one core | slowest frame |
| --- | --- | --- | --- | --- |
| 1 | 0 ms | 14.6 | 46% | 15.1 |
| 2 | 32 ms | 8.9 | 28% | 17.2 |
| 3 | 64 ms | 7.1 | 22% | 19.8 |
| 4 (default) | 96 ms | 6.1 | 19% | 22.5 |
| 6 | 160 ms | 5.4 | 17% | 29.1 |
| 8 | 224 ms | 5.3 | 16% | 38.4 |

Detections do not depend on the batch: on the host check below, batches 1, 2,
4 and 8 fire on the same frames (each shifted by its delay). The slowest frame
is the one that runs the whole batch, so the I2S DMA buffer must hold at least
that long (the demo buffers 128 ms).

## Accuracy check (host)

`test/compare.cc` runs WAV files through full sessions with the float model
and with the portable C build of the S3 engine (the same integers as the chip)
and reports posterior, score and detection differences. On 180 keyword
recordings (alexa, computer, jarvis, smart mirror, snowboy, view glass) and 37
speech clips without keywords, 16-bit activations with batch 4 detect the same
179 of 180 keywords on the same frames (shifted by the 3-frame delay), with no
false alarms in either engine; 8-bit activations move 26 detection frames and
are not used. With the sigmoid table (2026-10-31; the first 30 recordings of
each keyword plus the same 37 clips): 178 detections in both engines on the same
frames, posterior argmax agreement 99.99% (100.00% before), largest keyword
score difference 69 int16 units (26 before).

```sh
python3 engine/tools/generate_languages.py engine/inventories build/languages.generated.h
gcc -O2 -c sdk/esp32/wake/s3/s3_kernels.c -o build/s3_kernels.o
g++ -std=c++17 -O2 -ffp-contract=off -fno-fast-math -Iengine/include -Iinclude \
  -Iengine/include/kws/detail -Iengine/src/session -Ibuild -Isdk/esp32/wake/s3 \
  sdk/esp32/wake/test/compare.cc sdk/esp32/wake/s3/s3_acoustic.cc build/s3_kernels.o \
  $(sed 's|^|engine/|' engine/sources.txt) -o build/s3-compare
build/s3-compare resources/models/en-1/model.pwake keywords.json audio/*.wav
```
