"""Installed-SDK WAV/microphone example. All audio processing stays local."""
import argparse
from dataclasses import asdict
import json
import queue
import wave
import numpy as np
from phonlet_wake import Wake
from phonlet_wake_model_en import english

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    source=parser.add_mutually_exclusive_group(required=True)
    source.add_argument('--file');source.add_argument('--mic',action='store_true')
    parser.add_argument('--keyword',action='append',help='Catalog ID or ID=sensitivity; repeat for several keywords')
    parser.add_argument('--sensitivity',type=float,default=.5)
    parser.add_argument('--device',help='sounddevice device name/index')
    args=parser.parse_args()
    keywords=[]
    for value in args.keyword or ['hey phonlet']:
        ident,sep,sens=value.rpartition('=')
        keywords.append({'id':ident,'sensitivity':float(sens)} if sep else value)
    with Wake.create(assets=english,keywords=keywords,sensitivity=args.sensitivity) as wake:
        def process(pcm):
            for event in wake.process(pcm):print(json.dumps(asdict(event)),flush=True)
        if args.file:
            with wave.open(args.file,'rb') as audio:
                if (audio.getnchannels(),audio.getsampwidth(),audio.getframerate(),audio.getcomptype())!=(1,2,16000,'NONE'):
                    parser.error('audio must be mono 16 kHz signed PCM16 WAV')
                while data:=audio.readframes(4096):process(np.frombuffer(data,dtype='<i2').astype(np.int16,copy=False))
        else:
            import sounddevice as sd
            chunks=queue.Queue(maxsize=8) # 4096 samples at 512/sample blocks.
            errors=queue.SimpleQueue()
            def callback(indata,frames,time,status):
                if status:
                    errors.put(RuntimeError(f'Capture failed: {status}'));raise sd.CallbackAbort
                try:chunks.put_nowait(indata[:,0].copy())
                except queue.Full:
                    errors.put(RuntimeError('Capture queue exceeded 4096 samples'));raise sd.CallbackAbort
            try:
                with sd.InputStream(samplerate=16000,channels=1,dtype='int16',blocksize=512,device=args.device,callback=callback):
                    print('Listening locally; Ctrl+C stops.',flush=True)
                    while True:
                        if not errors.empty():raise errors.get()
                        try:process(chunks.get(timeout=.1))
                        except queue.Empty:pass
            except KeyboardInterrupt:pass
            while not chunks.empty():process(chunks.get_nowait())
            if not errors.empty():raise errors.get()
        print(json.dumps(dict(wake.finish())))

if __name__=='__main__':main()
