# Android demo app

Foreground microphone demo: `AudioRecord` at 16 kHz mono feeds the shared
`Wake` classes; each detection appears on screen with the phrase and the time.
Tested on Android 16 (arm64). Assets (model + keyword
array) are copied from `resources/` at build time and never committed.

Build and install (Android SDK with platform 35, NDK, CMake 3.22.1, JDK 17+):

```sh
gradle -p demos/android :app:assembleDebug
adb install demos/android/app/build/outputs/apk/debug/app-debug.apk
```

The microphone permission is requested at runtime.
