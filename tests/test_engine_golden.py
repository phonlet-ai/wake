import hashlib
import json
from pathlib import Path
import numpy as np
from phonlet_wake import Frontend, Detector, OutputSpec

def test_frontend_and_fire_policy_golden():
    expected=json.loads((Path(__file__).parent/'fixtures/engine_golden.json').read_text())
    pcm=((np.arange(8192,dtype=np.int64)*7919%65536)-32768).astype(np.int16)
    mel=Frontend.extract(pcm,finalize='complete_hops')
    assert f'{Frontend.identity():08x}'==expected['frontend_id']
    assert hashlib.sha256(mel.tobytes()).hexdigest()==expected['mel_sha256']
    q=np.full((120,40),10000,np.int32)
    q[:,0]=2**31-1
    for start in (10,40,85):
        for j,phone in enumerate((16,13,20,3,22)):
            q[start+j*3:start+j*3+2]=10000
            q[start+j*3:start+j*3+2,phone]=2**31-1
    kw=[{'name':'synthetic','pronunciations':[[16,13,20,3,22]],'threshold_loose':-1.5,'threshold_strict':-.75,'endpoint':False}]
    with Detector(kw,outputs=OutputSpec(events=True,scores=True,gates=True)) as detector:
        result=detector.push_posteriors(q)
    assert int(result.fires.sum())==expected['fires']
    assert hashlib.sha256(result.scores.tobytes()).hexdigest()==expected['scores_sha256']
    assert hashlib.sha256(result.fires.tobytes()).hexdigest()==expected['fires_sha256']
