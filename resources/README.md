# Model and English phrases

| Path | Contents |
| --- | --- |
| [`models/en-1/model.pwake`](models/en-1/model.pwake) | The `en-1` acoustic model (minGRU, 6 layers, 162 units, int8 weights) |
| [`keywords/en.json`](keywords/en.json) | A keyword file with five English phrases, bound to `en-1` by its SHA-256 |

The repository gate pins the hashes of both files. The packager copies them into the model packages
(`packaging/model-node`, `packaging/model-python`). See [phrases and keyword files](../docs/keywords.md).
