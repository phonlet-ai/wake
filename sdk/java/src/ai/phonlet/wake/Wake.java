package ai.phonlet.wake;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * Wake-word detection over the public Phonlet Wake C API.
 *
 * One instance is one continuous audio stream: mono 16 kHz signed PCM16 in
 * chunks of any size, at most one detection per 512-sample frame (production
 * arbitration). Implements AutoCloseable; closing is idempotent.
 */
public final class Wake implements AutoCloseable {

  private static boolean loaded = false;

  // Every entry point that reaches native code loads the library first, including the static
  // getters, which an app may call before creating a detector.
  private static synchronized void ensureLoaded() {
    // libwakejni links libkws_engine (rpath $ORIGIN); one load site suffices.
    if (loaded) return;
    String override = System.getProperty("ai.phonlet.wake.engine");
    if (override != null && !override.isEmpty()) System.load(override);
    else System.loadLibrary("wakejni");
    loaded = true;
  }

  private long handle;
  // Reused result slot: no allocation per audio chunk (instances are single-threaded).
  private final int[] index = new int[1];

  private Wake(long handle) { this.handle = handle; }

  /** Load a model container from memory (e.g. an Android asset or firmware image). */
  public static Wake create(byte[] model, String enrollmentsJson, double... sensitivities) {
    if (model == null || model.length == 0) throw new IllegalArgumentException("model bytes are required");
    if (enrollmentsJson == null || enrollmentsJson.isEmpty()) throw new IllegalArgumentException("enrollments JSON is required");
    ensureLoaded();
    long[] handle = new long[1];
    int status = WakeEngine.nativeInitBytes(model, enrollmentsJson, sensitivities, handle);
    if (status != 0) WakeEngine.throwLastError(status);
    return new Wake(handle[0]);
  }

  /** Load a model container and phrases from a keyword file (or its bare phrase array) as JSON. */
  public static Wake create(Path model, Path enrollments, double... sensitivities)
      throws IOException {
    if (model == null) throw new IllegalArgumentException("model path is required");
    if (enrollments == null) throw new IllegalArgumentException("enrollments path is required");
    ensureLoaded();
    String json = new String(Files.readAllBytes(enrollments), StandardCharsets.UTF_8);
    long[] handle = new long[1];
    int status = WakeEngine.nativeInit(model.toString(), json, sensitivities, handle);
    if (status != 0) WakeEngine.throwLastError(status);
    return new Wake(handle[0]);
  }

  /**
   * Process PCM; returns the index (in enrollment order) of the first
   * detection in this call, or -1 when none fired.
   */
  public int process(short[] pcm, int offset, int samples) {
    requireOpen();
    if (pcm == null || offset < 0 || samples < 0 || offset + samples > pcm.length) {
      throw new IllegalArgumentException("invalid PCM range");
    }
    index[0] = -1;
    int status = WakeEngine.nativeProcess(handle, pcm, offset, samples, index);
    if (status != 0) WakeEngine.throwLastError(status);
    return index[0];
  }

  /** Close the stream; returns the discarded incomplete-tail sample count. */
  public int finish() {
    requireOpen();
    int[] discarded = new int[]{-1};
    int status = WakeEngine.nativeFinish(handle, discarded);
    if (status != 0) WakeEngine.throwLastError(status);
    return discarded[0];
  }

  @Override public void close() {
    if (handle != 0) {
      WakeEngine.nativeDelete(handle);
      handle = 0;
    }
  }

  public static int version() { ensureLoaded(); return WakeEngine.nativeVersion(); }
  public static int frameLength() { ensureLoaded(); return WakeEngine.nativeFrameLength(); }
  public static int sampleRate() { ensureLoaded(); return WakeEngine.nativeSampleRate(); }

  private void requireOpen() {
    if (handle == 0) throw new WakeException("wake instance is closed");
  }
}
