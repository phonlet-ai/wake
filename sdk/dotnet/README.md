# Phonlet Wake .NET SDK

P/Invoke binding over the public `phonlet_wake_*` C ABI (`libkws_engine` on
the library path). One `Wake` instance is one continuous mono 16 kHz PCM16
stream; `IDisposable` with idempotent dispose.

```csharp
// A phrase is its pronunciations and two calibrated thresholds.
string phrases = "[{\"name\": \"hey kettle\", \"pronunciations\": [[16, 13, 20, 11, 31, 3, 21]], " +
                 "\"threshold_loose\": -1.21875, \"threshold_strict\": -0.8740234375, \"endpoint\": true}]";

using (var wake = Wake.Create(File.ReadAllBytes("model.pwake"), phrases, 0.5)) // one sensitivity per phrase
{
    int index = wake.Process(pcm, 0, pcm.Length); // -1 when nothing fired
    int discarded = wake.Finish();
}
```

`Wake.Create("model.pwake", "keywords.json", 0.5)` reads the same array from files. Strings cross
to the engine as UTF-8, and `Process` passes a window of your array without copying it.

The engine library (`kws_engine.dll` on Windows, `libkws_engine.so` on Linux) must be on the
library search path, for example next to the application. Tested on .NET 8, Windows x64, with the
engine built by MSVC (`cl /LD /std:c++17 /O2 /fp:strict` over `engine/sources.txt`, exporting the
`phonlet_wake_*` functions).

Run the file demo:

```sh
dotnet run --project demos/dotnet -- model.pwake keywords.json audio.s16le 0.5
```

Engine failures surface as `WakeException` with the engine's message.
