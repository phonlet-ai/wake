# Node demos

Install the SDK, native target and English model tarballs listed in the Node SDK README into this directory. They can be installed without running package lifecycle scripts. Neither Python nor browser assets are needed.

```sh
node file.mjs input.wav --keyword computer=0.7 --keyword jarvis=0.35
node microphone.mjs --keyword computer=0.7 --device default
```

Files must be mono 16 kHz PCM16 WAV. The Linux microphone example requires ALSA `arecord` (usually `alsa-utils`). It requests mono PCM16 at 16 kHz; ALSA device conversion is explicit in those capture parameters. Ctrl+C terminates only the recorder process owned by this example, drains accepted audio and closes Wake. Capture backlog/device errors stop the example visibly; audio is not uploaded. Tested with WAV files; not yet with a physical microphone.
