import hashlib
import json
from pathlib import Path
import numpy as np
from phonlet_wake import Model, Session, Frontend, OutputSpec

ROOT=Path(__file__).resolve().parents[1]
MODEL=ROOT/'resources/models/en-1/model.pwake'

def test_model_container_and_streaming_contract():
    assert hashlib.sha256(MODEL.read_bytes()).hexdigest()=='8be3687d060f953d0069480cf53d1fd0aa66e04551c2a4b600823a992a864af8'
    pcm=((np.arange(16031,dtype=np.int64)*7919 % 65536)-32768).astype(np.int16)
    keywords=json.loads((ROOT/'resources/keywords/en.json').read_text())['keywords'][:3]
    with Model.from_file(MODEL) as model:
        outputs=OutputSpec(posteriors=True,scores=True,events=True)
        with Session(model,keywords=keywords,outputs=outputs) as s:
            whole=s.run_pcm(pcm)
        with Session(model,keywords=keywords,outputs=outputs) as s:
            pieces=[s.push_pcm(pcm[:113]),s.push_pcm(pcm[113:5555]),s.push_pcm(pcm[5555:])]
            final=s.finish()
        assert final['discarded_samples']==159
        for field in ('posteriors','scores','fires'):
            assert np.array_equal(getattr(whole,field),np.concatenate([getattr(p,field) for p in pieces]))
