import json
from pathlib import Path
import numpy as np
import pytest
from phonlet_wake import Detector, Model, Session, OutputSpec
from phonlet_wake.runtime._ffi import library, ptr

KEYWORDS=[{'name':name,'pronunciations':[[1,2]],'threshold_loose':-2.,'threshold_strict':-.5,'endpoint':False} for name in ('first','second')]

@pytest.mark.parametrize('mode',['independent','production_arbitrated'])
def test_uniform_values_match_grid_and_grid_is_not_reinterpreted(mode):
    scores=np.tile(np.array([[-1000,-1000]],np.int16),(72,1))
    with Detector(KEYWORDS,sensitivities=[.5],mode=mode) as grid, Detector(KEYWORDS,keyword_sensitivities=[.5,.5],mode=mode) as per:
        assert np.array_equal(grid.replay_scores(scores).fires,per.replay_scores(scores).fires)
    with Detector(KEYWORDS,sensitivities=[0,1],mode=mode) as grid:
        assert grid.replay_scores(scores).fires.shape==(72,2,2)

def test_mixed_threshold_arbitration_debounce_and_ties():
    scores=np.tile(np.array([[-1000,-1000]],np.int16),(52,1))
    for sens,winner in [([0,1],1),([1,0],0),([.5,.5],1)]:
        with Detector(KEYWORDS,keyword_sensitivities=sens,mode='production_arbitrated') as d:
            result=d.replay_scores(scores)
            assert result.fires.shape==(52,1,2)
            assert np.argwhere(result.fires).tolist()==[[16,0,winner],[33,0,winner],[50,0,winner]]
            assert all(e['sensitivity']==sens[winner] for e in result.events)

def test_native_rejects_wrong_count_and_invalid_values():
    lib=library()
    for values in ([.5],[-.1,.5],[.5,float('nan')],[1.1,.5]):
        s=np.array(values,np.float64)
        assert not lib.kws_detector_create_per_keyword(json.dumps(KEYWORDS).encode(),ptr(s),len(s),1,40,b'en')

def test_real_model_per_keyword_chunking_and_ownership():
    root=Path(__file__).resolve().parents[1]
    kws=json.loads((root/'resources/keywords/en.json').read_text())['keywords'][:2]
    pcm=((np.arange(9001,dtype=np.int64)*7919%65536)-32768).astype(np.int16)
    model=Model.from_file(root/'resources/models/en-1/model.pwake')
    options=dict(keywords=kws,keyword_sensitivities=[0,1],mode='production_arbitrated',outputs=OutputSpec(scores=True,events=True))
    with Session(model,**options) as s: whole=s.run_pcm(pcm)
    with Session(model,**options) as s:
        model.close() # Stream retains immutable weights.
        chunks=[s.push_pcm(pcm[:113]),s.push_pcm(pcm[113:600]),s.push_pcm(pcm[600:])]
        assert s.finish()['discarded_samples']==297
    for field in ('scores','fires'):
        assert np.array_equal(getattr(whole,field),np.concatenate([getattr(c,field) for c in chunks]))

def test_nested_phrase_wins_by_margin_and_fires_once():
    # "assistant" inside "hey assistant": on the longer phrase's audio the shorter one often has
    # the higher raw score, but each phrase has its own thresholds, so the winner is the phrase
    # furthest above its own threshold, and no other phrase fires during the debounce after it.
    kws=[{'name':'hey assistant','pronunciations':[[1,2,3]],'threshold_loose':-2.,'threshold_strict':-1.,'endpoint':False},
         {'name':'assistant','pronunciations':[[2,3]],'threshold_loose':-1.,'threshold_strict':-.5,'endpoint':False}]
    scores=np.full((40,2),-3000,np.int16)
    scores[20:23]=[-900,-500]   # thresholds at 0.5: -1536 and -768; margins 636 and 268
    scores[23:31,1]=-300        # "assistant" stays above its threshold within the debounce
    with Detector(kws,keyword_sensitivities=[.5,.5],mode='production_arbitrated') as d:
        assert np.argwhere(d.replay_scores(scores).fires).tolist()==[[20,0,0]]
    with Detector(kws,keyword_sensitivities=[.5,.5],mode='independent') as d:
        assert np.argwhere(d.replay_scores(scores).fires).tolist()==[[20,0,0],[20,0,1]]
