"""Run from a fresh wheel environment, without checkout imports or compiler PATH."""
import json, sys
from pathlib import Path
import numpy as np
from phonlet_wake import Wake, read_keywords
from phonlet_wake_model_en import english
from phonlet_wake.runtime._ffi import library

chosen = [k for k in read_keywords(english['keywords'])['keywords'] if k['name'] in ('hey phonlet', 'assistant')]
chosen[0]['sensitivity'] = 0
with Wake.create(model=english['model'], keywords=chosen, sensitivity=.7) as wake:
    assert wake.process(np.zeros(16031, np.int16)) == []
    assert dict(wake.finish()) == {'frames': 31, 'discarded_samples': 159}
    assert '_native' in library().path.parts and 'site-packages' in library().path.parts
    info = dict(wake.info)
with Wake.create(model=english['model'], keywords=english['keywords']) as wake:
    assert [k['name'] for k in wake.keywords][0] == 'hey phonlet'
fixture = json.loads(Path(sys.argv[1]).read_text())
with Wake.create(model=english['model'], keywords=fixture['keywords']) as wake:
    found = []
    for i in range(0, fixture['samples'], 997): found.extend(wake.process(np.zeros(min(997, fixture['samples'] - i), np.int16)))
    assert [e.frame_index for e in found] == fixture['expected_frames']
    assert all(e.keyword_index == 1 and e.sensitivity == 1 for e in found)
    assert dict(wake.finish()) == {'frames': 70, 'discarded_samples': 17}
print(json.dumps({'python': sys.version.split()[0], 'info': info, 'positive_frames': fixture['expected_frames'], 'installed': 'passed'}))
