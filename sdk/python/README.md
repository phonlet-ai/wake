# Phonlet Wake for Python

Install the platform wheel and the separate model wheel from the [release](https://github.com/phonlet-ai/wake/releases/tag/v0.1.0):

```sh
python -m pip install \
  https://github.com/phonlet-ai/wake/releases/download/v0.1.0/phonlet_wake-0.1.0-py3-none-linux_x86_64.whl \
  https://github.com/phonlet-ai/wake/releases/download/v0.1.0/phonlet_wake_model_en-0.1.0-py3-none-any.whl
```

```python
import numpy as np
from phonlet_wake import Wake
from phonlet_wake_model_en import english

# english = {"model": ..., "keywords": ...}: the en-1 model and its English keyword file.
with Wake.create(model=english["model"], keywords=english["keywords"], sensitivity=0.5) as wake:
    for event in wake.process(np.zeros(512, dtype=np.int16)):
        print(event.keyword, event.time_seconds)
    print(dict(wake.finish()))
```

The wheel bundles its native engine and uses NumPy; it needs no compiler, checkout, lexicon or network at inference time. This release is qualified on CPython 3.12/Linux x64 only. Its `linux_x86_64` tag intentionally makes no manylinux compatibility claim. Other Python/platform combinations need qualification.

PCM is mono, 16 kHz, signed native-endian int16. Arbitrary chunks are buffered into 512-sample frames. `finish()` is idempotent and reports the discarded incomplete frame; use a fresh instance to restart. The context manager closes on both success and exceptions. Events are immutable and contain keyword ID/index, frame/sample indices, end timestamp and the keyword's effective sensitivity. Advanced `Model`, `Session`, `Detector`, `Frontend` and evaluation-grid APIs remain available.

`keywords` is a keyword file as the threshold generator writes it: its path, the parsed dict, or just the list of phrases written in code. Each phrase carries its pronunciations and calibrated thresholds (`id` defaults to `name`; `metadata` and a per-phrase `sensitivity` are optional):

```python
wake = Wake.create(model="model.pwake", keywords="keywords.json")   # a file
wake = Wake.create(model="model.pwake", keywords=[{                   # or in code
    "name": "hey kettle", "pronunciations": [[16, 13, 20, 11, 31, 3, 21]],
    "threshold_loose": -1.21875, "threshold_strict": -0.8740234375, "endpoint": True}])
```

A keyword file that names its model (`model.sha256`) is refused for any other model; a bare list is not checked, so keep it with the model it was made for. Unknown keys are errors. `read_keywords(path)` returns a checked keyword file to filter or edit. See [phrases and keyword files](../../docs/keywords.md).

Source builds require CMake, a C++17 compiler and Python. `python -m pip install .` from this directory builds the owned engine. The sdist includes all engine build inputs. `KWS_ENGINE_LIBRARY` explicitly overrides the bundled library; overrides must implement the per-keyword ABI additions.

The microphone example uses the optional `mic` extra (`sounddevice`, with PortAudio installed on the host). Core inference has no microphone dependency.
