package ai.phonlet.wake.demos;

import ai.phonlet.wake.Wake;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;

/** File demo: model container, keyword file, one sensitivity per
 * keyword, headerless mono 16 kHz PCM16 little-endian input. */
public final class FileDemo {
  public static void main(String[] args) throws IOException {
    if (args.length < 4) {
      System.err.println("usage: FileDemo model.pwake keywords.json audio.s16le sensitivity [sensitivity ...]");
      System.exit(2);
    }
    double[] sensitivities = new double[args.length - 3];
    for (int i = 0; i < sensitivities.length; i++) sensitivities[i] = Double.parseDouble(args[3 + i]);
    ByteBuffer bytes = ByteBuffer.wrap(Files.readAllBytes(Path.of(args[2]))).order(ByteOrder.LITTLE_ENDIAN);
    short[] pcm = new short[bytes.remaining() / 2];
    for (int i = 0; i < pcm.length; i++) pcm[i] = bytes.getShort();
    int frames = 0;
    // Before any detector exists: the static getters must load the native library themselves.
    System.out.printf("version=%d frameLength=%d sampleRate=%d%n",
        Wake.version(), Wake.frameLength(), Wake.sampleRate());
    try (Wake wake = Wake.create(Path.of(args[0]), Path.of(args[1]), sensitivities)) {
      int offset = 0;
      while (offset < pcm.length) {
        // Frame-sized reads for readable timing; the engine buffers any tail.
        int samples = Math.min(Wake.frameLength(), pcm.length - offset);
        int index = wake.process(pcm, offset, samples);
        offset += samples;
        frames = offset / Wake.frameLength();
        if (index >= 0) {
          System.out.printf("keyword=%d frame=%d seconds=%.3f sensitivity=%.3f%n",
              index, frames, frames * Wake.frameLength() / 16000.0, sensitivities[index]);
        }
      }
      System.out.printf("frames=%d discarded_samples=%d%n", frames, wake.finish());
    }
  }
}
