# ESP32-S3 wake-word demo

Streams an INMP441 I2S microphone through the Phonlet Wake engine on an
ESP32-S3 with octal PSRAM. The RGB LED glows dim blue while listening and
flashes orange on each detection; the console (native USB port) prints the
phrase and, every five seconds, the compute time per 32 ms frame.

Target board: ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM, WS2812 RGB LED
on GPIO 48), for example a DevKitC-1 N16R8.

## Wiring (INMP441, 6 pins)

| INMP441 | ESP32-S3 |
| --- | --- |
| VDD | 3V3 (never 5 V) |
| GND | GND |
| SCK | GPIO 4 |
| WS | GPIO 5 |
| SD | GPIO 6 |
| L/R | GND (left slot) |

Prove the microphone path first with [`../esp32-mic-check`](../esp32-mic-check/README.md).

## Prepare assets

The model and the keyword file are embedded into the firmware:

```sh
cp resources/models/en-1/model.pwake demos/esp32/main/
cp resources/keywords/en.json demos/esp32/main/keywords.json
```

## Build and flash (ESP-IDF v5.4)

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

## Status (2026-10-31, ESP32-S3 QFN56 rev 0.2, N16R8, ESP-IDF v5.4.4)

- **Live voice test:** six phrases (computer, alexa, jarvis, hey kettle, hey
  captain, okay toaster) were each detected when spoken at arm's length, with
  no detection without a spoken phrase over two minutes (one speaker, one room;
  not a counted accuracy test).
- **CPU:** 6.2 ms per 32 ms frame on average (19% of one core) with the
  default batch of 4, down from 45.2 ms with the portable float kernel.
- **Delay:** detections reach the console (batch - 1) x 32 ms after the audio,
  96 ms by default. Set the batch under menuconfig -> Phonlet Wake to trade CPU
  for response time (batch 1: 46% of a core, no extra delay).
- **Memory:** weights 75 KB internal RAM + 934 KB PSRAM; 53 KB internal RAM free.

See [the component README](../../sdk/esp32/wake/README.md) for the engine and
the measurements.

## CI cross-compile

`.github/workflows/esp32.yml` builds this demo for ESP32-S3 in the ESP-IDF
container on demand (no hardware attached).
