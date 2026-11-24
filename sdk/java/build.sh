#!/usr/bin/env bash
# Build and test the Java SDK against a built engine (build/native).
# JDK 17+ required; JAVA_HOME must point at a JDK (not a JRE).
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
jdk="${JAVA_HOME:-$(dirname "$(dirname "$(command -v javac || true)")")}"
[ -x "$jdk/bin/javac" ] || { echo "JAVA_HOME must point at a JDK with javac" >&2; exit 1; }
# A JDK image may omit the launcher (javac-only extraction); fall back to PATH.
java_bin="$jdk/bin/java"; [ -x "$java_bin" ] || java_bin="$(command -v java)"
[ -x "$java_bin" ] || { echo "no java runtime found" >&2; exit 1; }
lib="$root/build/native"
[ -f "$lib/libkws_engine.so" ] || { echo "missing $lib/libkws_engine.so — build the engine first" >&2; exit 1; }
out="$root/build/java"
mkdir -p "$out/native" "$out/classes"
cc "$root/sdk/java/native/wake_jni.c" -O2 -fPIC -shared \
   -I"$root/include" -I"$jdk/include" -I"$jdk/include/linux" \
   -L"$lib" -lkws_engine -Wl,-rpath,"\$ORIGIN" -o "$out/native/libwakejni.so"
cp -P "$lib/libkws_engine.so" "$lib/libkws_engine.so".* "$out/native/" 2>/dev/null || cp "$lib/libkws_engine.so" "$out/native/"
"$jdk/bin/javac" -d "$out/classes" "$root"/sdk/java/src/ai/phonlet/wake/*.java "$root"/demos/java/*.java
(cd "$out" && "$jdk/bin/jar" --create --file wake-java.jar -C classes .)
# Deterministic 2 s test PCM and the count of catalog sensitivities.
python3 - "$out/test-input.s16le" <<'PY'
import struct, sys, json
pcm = struct.pack('<%dh' % 32000, *[((i * 7919) % 65536) - 32768 for i in range(32000)])
open(sys.argv[1], 'wb').write(pcm)
catalog = json.load(open('resources/keywords/en.json'))
print(len(catalog['keywords']))
PY
count=$(python3 -c "import json;print(len(json.load(open('$root/resources/keywords/en.json'))['keywords']))")
sens=$(python3 -c "print(' '.join(['0.5']*$count))")
LD_LIBRARY_PATH="$out/native" "$java_bin" -cp "$out/classes" ai.phonlet.wake.demos.FileDemo \
  "$root/resources/models/en-1/model.pwake" "$root/resources/keywords/en.json" \
  "$out/test-input.s16le" $sens
