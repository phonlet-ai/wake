# Phonlet Wake Android SDK

Android archive (AAR) packaging the shared Java surface (`sdk/java/src`, the
same classes verified on the JVM) and the engine compiled natively per ABI
(`arm64-v8a`, `armeabi-v7a`, `x86_64`) from the shared engine sources with the
same strict floating-point flags. The JNI bridge is the same
`sdk/java/native/wake_jni.c` used by the desktop JVM SDK.

```kotlin
// from app assets:
val model = assets.open("model.pwake").readBytes()
val keywords = assets.open("keywords.json").bufferedReader().readText()
Wake.create(model, keywords, 0.5, 0.5, 0.5, 0.5, 0.5).use { wake ->
    val index = wake.process(pcm, 0, pcm.length)   // -1 when nothing fired
}
```

`Wake` implements `AutoCloseable`; one instance is one continuous mono 16 kHz
PCM16 stream. Record with `AudioRecord` at 16 kHz mono and feed chunks of any
size (see `demos/android`).

## Building

Requires the Android SDK (platform 35, NDK, CMake 3.22.1) and a JDK 17+:

```sh
gradle -p sdk/android :wake:assembleRelease
```

The CI workflow `.github/workflows/sdk-android.yml` builds the AAR and the
demo app on demand (manual dispatch during the publishing program).
