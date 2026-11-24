# Phrases and keyword files

[Documentation](README.md) / Phrases

A wake phrase is data, not a model. The engine needs, for each phrase:

| Field | Meaning |
| --- | --- |
| `name` | The phrase as text, reported with each detection |
| `pronunciations` | One or more sequences of phone ids (1 to 39, see the [inventory](../engine/inventories/EN.json)) |
| `threshold_loose` | Threshold at sensitivity 1 (most responsive) |
| `threshold_strict` | Threshold at sensitivity 0 (fewest false alarms) |
| `endpoint` | `true` to report the phrase only after the speaker pauses |

Optional per phrase: `metadata` (any object, never read by the engine), `id` (defaults to
`name`), and in the Node.js, browser and Python SDKs `sensitivity` (overrides the shared default).

The thresholds are calibrated for one model and one phrase. Pronunciations without thresholds, or
thresholds from another model, do not work.

## The keyword file

The threshold generator (`wake-threshold`, or the generator page on the website) turns typed
phrases into a keyword file. Every SDK reads that file as it is:

```json
{
  "model": {"id": "en-1", "sha256": "8be3687d060f953d0069480cf53d1fd0aa66e04551c2a4b600823a992a864af8"},
  "keywords": [
    {"name": "hey kettle",
     "pronunciations": [[16, 13, 20, 11, 31, 3, 21]],
     "threshold_loose": -1.21875,
     "threshold_strict": -0.8740234375,
     "endpoint": false,
     "metadata": {"source": "lexicon", "fa_factor_at_default": 1.94, "...": "..."}}
  ],
  "metadata": {"generator": "wake-threshold 0.2.0", "rate_loose": 10.0, "rate_strict": 0.1}
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `keywords` | yes | The phrases, in the order detections report them |
| `model` | no | `id` and `sha256` of the model the thresholds belong to |
| `metadata` | no | Anything; the generator records its version, rates and diagnostics here |

The Node.js, browser and Python SDKs refuse a file whose `model.sha256` is not the hash of the
model they were given. The C API and the SDKs built on it read only `keywords`. Unknown keys are
errors in the Node.js, browser and Python SDKs, so a typo never silently changes a setting. The
[JSON schema](contracts/keywords.schema.json) is the formal definition.

## Three ways to pass phrases

Every SDK takes the same data in whichever form suits the program:

1. **A file:** the path (or URL in the browser) of a keyword file.
2. **An object:** the parsed keyword file.
3. **In code:** just the list of phrases, written in the program. Nothing checks which model they
   belong to, so keep them with the model they were made for.

| SDK | How |
| --- | --- |
| Python | `Wake.create(model="model.pwake", keywords="keywords.json")`, or `keywords=` the parsed file or a list of phrases |
| Node.js | `Wake.create({model: 'model.pwake', keywords: 'keywords.json'})`, or `keywords:` the parsed file or an array of phrases |
| Browser | `Wake.create({model: '/model.pwake', keywords: '/keywords.json'})` (URLs or bytes), or the parsed file or an array of phrases |
| C / C++ | `phonlet_wake_init(model_path, json, sensitivities, count, &wake)` with the file's text (or a phrase array); `phonlet_wake_keyword_count(json)` gives `count`, `phonlet_wake_keyword_name(wake, i)` the names |
| Java, Android | `Wake.create(modelBytes, json, sensitivities...)` |
| .NET | `Wake.Create(modelBytes, json, sensitivities)`, or `Wake.Create(modelPath, keywordFilePath, ...)` |
| ESP32-S3 | `esp_wake_begin(model, size, json, sensitivity, &wake)`, typically with the file embedded in flash |

In the C-based SDKs give one sensitivity per phrase, in file order. In Node.js, browser and Python
give a shared default and optionally a `sensitivity` on any phrase.

To use only some phrases of a file, filter its `keywords` list and pass the result (keep `model`
to keep the check):

```python
import json
file = json.load(open("keywords.json"))
file["keywords"] = [k for k in file["keywords"] if k["name"] in ("hey kettle", "assistant")]
wake = Wake.create(model="model.pwake", keywords=file)
```

## Sensitivity

Sensitivity, from 0 to 1, chooses a threshold between the phrase's strict and loose ends. The
generator's default calibration aims sensitivity 1 at about 10 false alarms per hour of speech, 0.5 at
about 1, and 0 at about 0.1. It is a control, not a confidence: a detection at sensitivity 0.9 is
not "90% sure". Sensitivities are fixed for an instance; create a new one to change them.

## Phrases the dictionary does not know

The generator looks each word up in a 394,000-word pronunciation dictionary. A word that is not
there (a product name, a made-up word) gets a pronunciation predicted from its spelling, and the
phrase's `metadata.source` says `g2p`. Predicted pronunciations are usually right for words that
are spelled the way they sound; for anything else, give the phones yourself:
`wake-threshold "zorblax=Z AO R B L AE K S"`.

## The English phrases in this repository

[`resources/keywords/en.json`](../resources/keywords/en.json) is a keyword file with five phrases
(hey phonlet, ok phonlet, hey assistant, assistant, hey wake) for
[`resources/models/en-1/model.pwake`](../resources/models/en-1/model.pwake). Their pronunciations
were chosen by hand, so they differ slightly from what the generator writes for the same text. The
model packages (`packaging/model-node`, `packaging/model-python`) ship both files.
