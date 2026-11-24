using System;
using System.IO;

namespace Phonlet.Wake.Demos
{
    /// File demo: model container, keyword file, one sensitivity
    /// per keyword, headerless mono 16 kHz PCM16 little-endian input.
    public static class FileDemo
    {
        public static int Main(string[] args)
        {
            if (args.Length < 4)
            {
                Console.Error.WriteLine("usage: FileDemo model.pwake keywords.json audio.s16le sensitivity [sensitivity ...]");
                return 2;
            }
            double[] sensitivities = new double[args.Length - 3];
            for (int i = 0; i < sensitivities.Length; i++) sensitivities[i] = double.Parse(args[3 + i]);
            byte[] bytes = File.ReadAllBytes(args[2]);
            short[] pcm = new short[bytes.Length / 2];
            for (int i = 0; i < pcm.Length; i++) pcm[i] = (short)(bytes[2 * i] | (bytes[2 * i + 1] << 8));
            int frames = 0;
            using (Wake wake = Wake.Create(args[0], args[1], sensitivities))
            {
                Console.WriteLine($"version={Wake.Version} frameLength={Wake.FrameLength} sampleRate={Wake.SampleRate}");
                for (int offset = 0; offset < pcm.Length; offset += Wake.FrameLength)
                {
                    int samples = Math.Min(Wake.FrameLength, pcm.Length - offset);
                    int index = wake.Process(pcm, offset, samples);
                    frames = (offset + samples) / Wake.FrameLength;
                    if (index >= 0)
                        Console.WriteLine($"keyword={index} frame={frames} seconds={frames * Wake.FrameLength / 16000.0:F3} sensitivity={sensitivities[index]:F3}");
                }
                Console.WriteLine($"frames={frames} discarded_samples={wake.Finish()}");
            }
            return 0;
        }
    }
}
