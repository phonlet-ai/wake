# @phonlet-ai/wake-react

React hook for on-device wake-word detection, wrapping the
[Web SDK](../web/README.md) (`@phonlet-ai/wake-web`). The hook owns exactly one
capture manager: unmount closes it, `restart()` stops and restarts with fresh
stream state, and the wake options are snapshotted on first start.

```jsx
import {useWake} from '@phonlet-ai/wake-react';
import {english} from '@phonlet-ai/wake-model-en/web';

function WakeButton() {
  const {state, error, start, stop} = useWake({
    model: english.model,         // URL of the en-1 model
    keywords: english.keywords,   // URL of a keyword file, the parsed file, or an array of phrases
    sensitivity: 0.6,
    onDetection: (event) => console.log(event.keywordId, event.timeSeconds),
  });
  return (
    <>
      <button onClick={() => (state === 'listening' ? stop() : start())}>
        {state === 'listening' ? 'Stop' : 'Start'}
      </button>
      {error && <p role="alert">{error.message}</p>}
    </>
  );
}
```

Install with the Web SDK and an English model package; `react` 18 or newer is
a peer dependency. Audio is mono 16 kHz PCM from the microphone; all
processing stays on-device in the Web SDK's worker. Lifecycle rules and event
fields are the [Web SDK's](../web/README.md); the hook adds React state
binding only — no second detection path, no resampling, no scoring in the
adapter.

The lifecycle behavior (single manager, idempotent start, stop/restart reuse,
unmount cleanup, error surfacing, no post-unmount updates) is covered by
`test/react.test.mjs`; run `bash test/install-deps.sh && node --test test/`.
