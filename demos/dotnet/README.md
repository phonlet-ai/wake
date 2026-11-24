# .NET file demo

Run with the .NET SDK and a built engine on the library path:

```sh
dotnet run --project demos/dotnet -- ../resources/models/en-1/model.pwake \
  ../resources/keywords/en.json input.s16le 0.5 0.5 0.5 0.5 0.5
```

Streams headerless mono 16 kHz PCM16 through the public API, one sensitivity
per catalog keyword.
