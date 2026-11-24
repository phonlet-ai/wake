# ESP32-S3 INMP441 microphone check

A tiny firmware (no engine, no model) that proves the I2S microphone path
before the wake demo. Wiring and board are the same as
[`../esp32`](../esp32/README.md).

It reads both I2S slots (32-bit slots, as the INMP441 needs 64 bit clocks per
sample) and prints four lines a second on the native USB console:

```text
[  28] L SIGNAL      rms  -26.0 dBFS peak  -13.7 dBFS dc -1749 | R NO DATA rms -200.0 dBFS | OK: microphone on the left slot (L/R to GND)
```

- `SIGNAL` on L and `NO DATA` on R: wired correctly.
- `SIGNAL` on R only: L/R is tied high; tie it to GND.
- `NO DATA` or `STUCK` on both: check VDD (3V3), GND, SD (GPIO 6), SCK (GPIO 4), WS (GPIO 5).

Without a screen, the RGB LED on GPIO 48 shows the level: off in silence,
green and brighter as you speak, red when no audio arrives.

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Verified 2026-10-30 on an ESP32-S3-N16R8: left slot carries speech at about
-25 dBFS rms (peaks about -12 dBFS) with a -50 dBFS room floor, right slot
silent. The wake demo uses no extra gain as a result.
