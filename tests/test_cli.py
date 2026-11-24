import json
import sys
import wave
from pathlib import Path
import numpy as np
import phonlet_wake
from phonlet_wake import Detector, OutputSpec
from phonlet_wake.cli import main

def test_detect_cli_reports_the_fired_keyword_not_the_sensitivity(tmp_path,monkeypatch,capsys):
    keywords=[{'name':name,'pronunciations':[phones],'threshold_loose':-1.5,'threshold_strict':-.75,'endpoint':False}
              for name,phones in [('first',[1,2,3,4,5]),('second',[16,13,20,3,22])]]
    q=np.full((120,40),10000,np.int32)
    q[:,0]=2**31-1
    for start in (10,40,85):
        for j,phone in enumerate((16,13,20,3,22)):
            q[start+j*3:start+j*3+2]=10000
            q[start+j*3:start+j*3+2,phone]=2**31-1
    with Detector(keywords,sensitivities=[0.5],outputs=OutputSpec(events=True)) as detector:
        result=detector.push_posteriors(q)
    assert result.fires[:,:,1].sum()>0 and result.fires[:,:,0].sum()==0
    class Session:
        def __init__(self,*args,**kwargs): pass
        def __enter__(self): return self
        def __exit__(self,*args): pass
        def run_pcm(self,pcm): return result
    monkeypatch.setattr(phonlet_wake,'Session',Session)
    wav=tmp_path/'input.wav'
    with wave.open(str(wav),'wb') as handle:
        handle.setnchannels(1);handle.setsampwidth(2);handle.setframerate(16000)
        handle.writeframes(np.zeros(512,np.int16).tobytes())
    kw=tmp_path/'keywords.json';kw.write_text(json.dumps(keywords))
    monkeypatch.setattr(sys,'argv',['run.py','detect',str(wav),'--keywords',str(kw)])
    main()
    out=json.loads(capsys.readouterr().out)
    assert out['events'] and all(event['keyword']=='second' for event in out['events'])
