# C and C++ consumers

Unpack the native engine archive to a prefix, then build these independent consumers against its installed public headers and CMake target:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/wake-native-0.1.0
cmake --build build -j2
./build/wake-c model.pwake keywords-array.json audio.s16le 0.7 0.35
./build/wake-cpp model.pwake keywords-array.json audio.s16le 0.7 0.35
```

`wake-c` uses the public `<phonlet_wake.h>` API (`phonlet_wake_init/process/finish/delete`); `wake-cpp` shows the advanced C++ source API. Pass a keyword file as the threshold generator writes it (or its bare `keywords` array) and exactly one sensitivity per phrase, in order; `phonlet_wake_keyword_count()` gives the count. The C API does not check the file's `model` hash; keep the file with its model. Audio is headerless mono 16 kHz PCM16, little-endian on the qualified Linux x64 host. Use an explicit conversion tool if your input has another format. The examples report EOF tail loss and release resources. They do not upload audio or enroll text.

The versioned ABI promise applies to the C exports. The C++ interface is a source API; rebuild C++ consumers when updating the engine.
