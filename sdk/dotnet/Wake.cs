using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace Phonlet.Wake
{
    /// <summary>Wake-word detection over the public phonlet_wake C ABI.</summary>
    /// <remarks>
    /// One instance is one continuous mono 16 kHz PCM16 stream; chunks of any
    /// size are accepted and the engine buffers incomplete 512-sample frames.
    /// Dispose is idempotent.
    /// </remarks>
    public sealed class Wake : IDisposable
    {
        private const string Library = "kws_engine";

        // Strings cross the boundary as NUL-terminated UTF-8 bytes: the default string marshalling
        // uses the Windows code page, which would corrupt non-ASCII phrase names.
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern int phonlet_wake_init_bytes(byte[] model, int modelSize, byte[] enrollmentsJson,
            double[] sensitivities, int count, out IntPtr wake);

        // `ref short` pins the caller's array for the call, so a window into it needs no copy.
        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern int phonlet_wake_process(IntPtr wake, ref short pcm, int samples, out int keywordIndex);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern int phonlet_wake_finish(IntPtr wake, out int discardedSamples);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern void phonlet_wake_delete(IntPtr wake);

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern IntPtr phonlet_wake_error();

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern int phonlet_wake_version();

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern int phonlet_wake_frame_length();

        [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
        private static extern int phonlet_wake_sample_rate();

        private IntPtr _handle;
        private short _empty;

        private Wake(IntPtr handle) => _handle = handle;

        /// <summary>
        /// Create a detector from the model container bytes and a JSON array of phrases, each with
        /// its pronunciations and its loose and strict thresholds; one sensitivity per phrase.
        /// </summary>
        public static Wake Create(byte[] model, string enrollmentsJson, params double[] sensitivities)
        {
            if (model is null || model.Length == 0) throw new ArgumentException("model bytes are required", nameof(model));
            if (string.IsNullOrEmpty(enrollmentsJson)) throw new ArgumentException("enrollments JSON is required", nameof(enrollmentsJson));
            if (sensitivities is null || sensitivities.Length == 0) throw new ArgumentException("at least one sensitivity is required", nameof(sensitivities));
            byte[] json = Utf8(enrollmentsJson);
            int status = phonlet_wake_init_bytes(model, model.Length, json, sensitivities, sensitivities.Length, out IntPtr handle);
            if (status != 0) throw new WakeException($"init failed ({status}): {ErrorMessage()}");
            return new Wake(handle);
        }

        /// <summary>Create a detector from a model file and a JSON file of phrases.</summary>
        public static Wake Create(string modelPath, string enrollmentsJsonPath, params double[] sensitivities)
        {
            if (string.IsNullOrEmpty(modelPath)) throw new ArgumentException("model path is required", nameof(modelPath));
            if (string.IsNullOrEmpty(enrollmentsJsonPath)) throw new ArgumentException("enrollments path is required", nameof(enrollmentsJsonPath));
            return Create(File.ReadAllBytes(modelPath), File.ReadAllText(enrollmentsJsonPath), sensitivities);
        }

        /// <summary>Process PCM; returns the enrollment-order index of the first detection in this call, or -1.</summary>
        public int Process(short[] pcm, int offset, int samples)
        {
            ThrowIfDisposed();
            if (pcm is null) throw new ArgumentNullException(nameof(pcm));
            if (offset < 0 || samples < 0 || offset + samples > pcm.Length) throw new ArgumentOutOfRangeException(nameof(offset));
            int status = samples == 0
                ? phonlet_wake_process(_handle, ref _empty, 0, out int index)
                : phonlet_wake_process(_handle, ref pcm[offset], samples, out index);
            if (status != 0) throw new WakeException($"process failed ({status}): {ErrorMessage()}");
            return index;
        }

        /// <summary>Close the audio stream; returns the discarded incomplete-tail sample count.</summary>
        public int Finish()
        {
            ThrowIfDisposed();
            int status = phonlet_wake_finish(_handle, out int discarded);
            if (status != 0) throw new WakeException($"finish failed ({status}): {ErrorMessage()}");
            return discarded;
        }

        public static int Version => phonlet_wake_version();
        public static int FrameLength => phonlet_wake_frame_length();
        public static int SampleRate => phonlet_wake_sample_rate();

        public void Dispose()
        {
            if (_handle != IntPtr.Zero)
            {
                phonlet_wake_delete(_handle);
                _handle = IntPtr.Zero;
            }
        }

        private static byte[] Utf8(string text)
        {
            byte[] encoded = Encoding.UTF8.GetBytes(text);
            byte[] terminated = new byte[encoded.Length + 1];
            Array.Copy(encoded, terminated, encoded.Length);
            return terminated;
        }

        private static string ErrorMessage()
        {
            IntPtr message = phonlet_wake_error();
            if (message == IntPtr.Zero) return "no details";
            int length = 0;
            while (Marshal.ReadByte(message, length) != 0) length++;
            byte[] bytes = new byte[length];
            Marshal.Copy(message, bytes, 0, length);
            return Encoding.UTF8.GetString(bytes);
        }

        private void ThrowIfDisposed()
        {
            if (_handle == IntPtr.Zero) throw new ObjectDisposedException(nameof(Wake));
        }
    }

    public sealed class WakeException : Exception
    {
        public WakeException(string message) : base(message) { }
    }
}
