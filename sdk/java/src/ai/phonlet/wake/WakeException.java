package ai.phonlet.wake;

/** Engine or argument failure from the native wake-word engine. */
public final class WakeException extends RuntimeException {
  public WakeException(String message) { super(message); }
}
