<picture>
  <source media="(prefers-color-scheme: dark)" srcset="../assets/brand/phonlet-ai-horizontal-dark.svg">
  <img src="../assets/brand/phonlet-ai-horizontal-light.svg" alt="Phonlet AI" width="240">
</picture>

# Phonlet Wake documentation

| I want to… | Read |
| --- | --- |
| Understand the model, scorer and detector | [How it works](engine.md) |
| Describe a phrase, or load a keyword file | [Phrases and keyword files](keywords.md) |
| Use an SDK | The README in its directory under [`sdk/`](../sdk/), listed in the [main README](../README.md#platforms) |
| Run an example | [Demos](../demos/README.md) |
| Know the exact API rules every SDK follows | [SDK contract](contracts/SDK_CONTRACT.md) |
| Build, test or package the repository | [Development guide](development.md) |

## Essentials

- Audio is mono, 16 kHz, signed 16-bit PCM. The engine does not resample.
- One instance is one continuous audio stream. Finish and close it when the stream ends; create a
  new instance to start again or to change phrases or sensitivities.
- A phrase always comes with its thresholds, calibrated for the model it runs with.
- Everything runs on the device. No network access, account or licence key is used at runtime.
