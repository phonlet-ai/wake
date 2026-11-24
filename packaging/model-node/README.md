# English model pack

The `en-1` acoustic model with the English pack's five phrases (hey phonlet, ok phonlet, hey
assistant, assistant, hey wake), as `model.pwake` and `keywords.json` (a keyword file that names the model by SHA-256).

Node.js: import `english` from `@phonlet-ai/wake-model-en` and pass it as `assets` to
`Wake.create`. Browser: import the URL descriptor from `@phonlet-ai/wake-model-en/web`, or copy the
three files to your own origin and pass their URLs. Keep the three files together; the SDK checks
their hashes. No engine is included.
