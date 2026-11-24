# Python demos

Install the SDK and model wheels listed in the Python SDK README. For microphone input also install `sounddevice` and the host's PortAudio library.

```sh
python wake_demo.py --file input.wav --keyword computer=0.7 --keyword jarvis=0.35
python wake_demo.py --mic --keyword computer=0.7
```

Files must be mono 16 kHz PCM16 WAV. Microphone input requests that same format in 512-sample blocks. The queue is bounded to 4096 samples; overflow/device errors stop processing rather than joining dropped audio. Ctrl+C stops capture, drains accepted blocks and releases resources. Physical microphone qualification remains pending.
