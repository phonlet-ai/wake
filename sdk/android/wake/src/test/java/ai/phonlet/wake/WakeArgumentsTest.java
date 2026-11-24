package ai.phonlet.wake;

import org.junit.Test;

/** Argument validation runs before any native library is loaded. */
public class WakeArgumentsTest {
  @Test(expected = IllegalArgumentException.class)
  public void nullModelBytesRejected() { Wake.create((byte[]) null, "[]", 0.5); }

  @Test(expected = IllegalArgumentException.class)
  public void emptyModelBytesRejected() { Wake.create(new byte[0], "[]", 0.5); }

  @Test(expected = IllegalArgumentException.class)
  public void nullEnrollmentsRejected() { Wake.create(new byte[16], null, 0.5); }

  @Test(expected = IllegalArgumentException.class)
  public void emptyEnrollmentsRejected() { Wake.create(new byte[16], "", 0.5); }
}
