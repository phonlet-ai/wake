import json
import numpy as np
import pytest
from phonlet_wake import Wake, WakeError
from test_keywords import ENGLISH, MODEL, english, model_bytes


def phrases(*names):
    return [k for k in english()['keywords'] if k['name'] in names]


def test_facade_lifecycle_and_frozen_configuration():
    chosen = phrases('hey phonlet', 'assistant')
    chosen[0]['sensitivity'] = 0
    with Wake.create(model=MODEL, keywords=chosen, sensitivity=.7) as wake:
        chosen[0]['sensitivity'] = 1
        assert [k['sensitivity'] for k in wake.keywords] == [0, .7]
        assert wake.process(np.empty(0, np.int16)) == []
        assert wake.process(np.zeros(16031, np.int16)) == []
        assert dict(wake.finish()) == {'frames': 31, 'discarded_samples': 159}
        assert wake.finish() is wake.finish()
        with pytest.raises(WakeError, match='finished'): wake.process(np.empty(0, np.int16))
    wake.close()
    with pytest.raises(WakeError, match='closed'): wake.finish()


def test_failure_does_not_poison_new_session():
    with pytest.raises(WakeError): Wake.create(model=MODEL, keywords=[])
    with Wake.create(model=MODEL, keywords=phrases('hey phonlet')) as wake:
        with pytest.raises(ValueError): wake.process(np.zeros(5, np.float32))
        wake.process(np.zeros(513, np.int16))
        assert wake.finish()['discarded_samples'] == 1


def test_every_form_of_keywords_detects_the_same():
    # A file path, the parsed file, and the phrases written in code give identical sessions.
    pcm = ((np.arange(16000 * 4, dtype=np.int64) * 7919 % 65536) - 32768).astype(np.int16)
    forms = [ENGLISH, str(ENGLISH), english(), english()['keywords']]
    sessions = [Wake.create(model=MODEL, keywords=form, sensitivity=1) for form in forms]
    try:
        assert len({json.dumps([dict(k) for k in s.keywords]) for s in sessions}) == 1
        assert len({s.info['keywords_sha256'] for s in sessions}) == 1
        for start in range(0, len(pcm), 1000):
            results = [s.process(pcm[start:start + 1000]) for s in sessions]
            assert all(r == results[0] for r in results)
    finally:
        for s in sessions: s.close()


def test_model_as_bytes_and_binding():
    with Wake.create(model=model_bytes(), keywords=ENGLISH) as wake:
        assert wake.info['model_sha256'] == english()['model']['sha256']
    with pytest.raises(WakeError, match='made for model') as error:
        Wake.create(model=b'x' + model_bytes()[1:], keywords=ENGLISH)
    assert error.value.code == 'INCOMPATIBLE_ARTIFACT'


def test_phrases_are_validated_and_copied():
    [phrase] = phrases('assistant')
    with pytest.raises(TypeError):
        Wake.create(keywords=[phrase])
    with pytest.raises(WakeError, match='unknown'):
        Wake.create(model=MODEL, keywords=[{**phrase, 'threshold': -1.0}])
    with pytest.raises(WakeError, match='missing.*threshold_strict'):
        Wake.create(model=MODEL, keywords=[{k: v for k, v in phrase.items() if k != 'threshold_strict'}])
    with pytest.raises(WakeError, match='finite'):
        Wake.create(model=MODEL, keywords=[{**phrase, 'threshold_loose': float('nan')}])
    with pytest.raises(WakeError, match='phone IDs'):
        Wake.create(model=MODEL, keywords=[{**phrase, 'pronunciations': [[0]]}])
    with Wake.create(model=MODEL, keywords=[{**phrase, 'sensitivity': 0.2}]) as wake:
        phrase['pronunciations'][0].append(99)
        assert dict(wake.keywords[0]) == {'id': 'assistant', 'name': 'assistant', 'sensitivity': 0.2}
