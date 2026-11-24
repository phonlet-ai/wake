package ai.phonlet.wake;

/** JNI surface over the public phonlet_wake_* C API; used by Wake. */
final class WakeEngine {
  private WakeEngine() {}
  static native int nativeInit(String model, String enrollments, double[] sensitivities, long[] handle);
  static native int nativeInitBytes(byte[] model, String enrollments, double[] sensitivities, long[] handle);
  static native int nativeProcess(long handle, short[] pcm, int offset, int samples, int[] index);
  static native int nativeFinish(long handle, int[] discarded);
  static native void nativeDelete(long handle);
  static native String nativeError();
  static native void throwLastError(int status);
  static native int nativeVersion();
  static native int nativeFrameLength();
  static native int nativeSampleRate();
}
