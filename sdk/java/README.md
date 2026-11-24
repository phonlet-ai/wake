# Phonlet Wake Java SDK

Wake-word detection over the public `phonlet_wake_*` C ABI. One `Wake`
instance is one continuous mono 16 kHz PCM16 stream; chunks of any size;
`AutoCloseable` with idempotent close.

```java
try (Wake wake = Wake.create(Path.of("model.pwake"), Path.of("keywords.json"), 0.5, 0.35)) {
  int index = wake.process(pcm, 0, pcm.length); // -1 when nothing fired
  int discarded = wake.finish();
}
```

Sensitivities are one value in `[0,1]` per phrase, in the keyword file's order.
The JSON is a keyword file as the threshold generator writes it, or its bare
`keywords` array (`name`, `pronunciations`, `threshold_loose`,
`threshold_strict`, `endpoint`). This thin binding does not check the file's
`model` hash (the Node/Web/Python SDKs do); engine errors surface as
`WakeException`.

## Building and testing

Requires a JDK 17+ (`JAVA_HOME`) and a built engine (`build/native`):

```sh
bash sdk/java/build.sh
```

The script compiles the JNI bridge (`libwakejni`, linked against
`libkws_engine` with `$ORIGIN` rpath), the SDK classes and the file demo, then
runs the demo against the real `en-1` model and catalog. `sdk/java/native/`
holds the bridge source; `scripts/test.py` runs this stage as `--stage java`
when a JDK is available.

The JVM loads only `libwakejni`; override its location with
`-Dai.phonlet.wake.engine=/path/to/libwakejni.so`.
