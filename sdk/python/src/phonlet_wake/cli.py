"""Build, inspect or run the shipped wake-word engine."""
import argparse
import json
from pathlib import Path
import wave
import numpy as np

ROOT=Path(__file__).resolve().parents[4]
MODEL=ROOT/'resources/models/en-1/model.pwake'
KEYWORDS=ROOT/'resources/keywords/en.json'
if not MODEL.is_file():
    try:
        from phonlet_wake_model_en import english
        MODEL, KEYWORDS = english['model'], english['keywords']
    except ImportError:
        MODEL = KEYWORDS = None

def main():
    p=argparse.ArgumentParser(description=__doc__)
    sub=p.add_subparsers(dest='command',required=True)
    sub.add_parser('build')
    for name in ('info','detect'):
        c=sub.add_parser(name)
        c.add_argument('--model',type=Path,default=MODEL)
        if name=='detect':
            c.add_argument('wav',type=Path)
            c.add_argument('--keywords',type=Path,default=KEYWORDS)
            c.add_argument('--sensitivity',type=float,default=0.5)
    a=p.parse_args()
    if a.command=='build':
        from .runtime.build import build_engine
        print(build_engine())
        return
    if a.model is None or (a.command == "detect" and a.keywords is None):
        p.error("install phonlet-wake-model-en or provide explicit --model and --keywords paths")
    from . import Model, Session, OutputSpec, model_info
    with Model.from_file(a.model) as model:
        if a.command=='info':
            print(json.dumps({'sha256':model.metadata["container_sha256"],**model_info(model)},indent=2))
        else:
            with wave.open(str(a.wav),'rb') as w:
                if (w.getnchannels(),w.getsampwidth(),w.getframerate()) != (1,2,16000):
                    p.error('audio must be mono, 16 kHz, signed PCM16 WAV')
                pcm=np.frombuffer(w.readframes(w.getnframes()),dtype='<i2')
            keywords=json.loads(a.keywords.read_text())
            if isinstance(keywords,dict):
                keywords=keywords['keywords']
            with Session(model,keywords=keywords,sensitivities=[a.sensitivity],
                         mode='production_arbitrated',outputs=OutputSpec(events=True,posteriors=False)) as session:
                result=session.run_pcm(pcm)
                hits=np.argwhere(result.fires)
                print(json.dumps({'model':model.metadata["container_sha256"],'frames':result.frames,
                    'events':[{'frame':int(h[0]),'keyword':keywords[int(h[2])]['name'],
                               'seconds':float((h[0]+1)*0.032)} for h in hits]},indent=2))

if __name__ == "__main__":
    main()
