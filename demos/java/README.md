# Java file demo

Built and run by `sdk/java/build.sh` (needs a JDK and a built engine):

```sh
bash sdk/java/build.sh
```

`FileDemo` streams a headerless mono 16 kHz PCM16 little-endian file through
the public API with one sensitivity per catalog keyword and prints detections
plus the discarded tail, mirroring `demos/native`.
