import json
from pathlib import Path
import numpy as np
from phonlet_wake import Wake
from test_keywords import MODEL


def test_positive_binding_contract_through_real_model():
    fixture = json.loads((Path(__file__).parent / 'fixtures/conformance.json').read_text())
    with Wake.create(model=MODEL, keywords=fixture['keywords']) as wake:
        found = []
        for i in range(0, fixture['samples'], 997):
            found.extend(wake.process(np.zeros(min(997, fixture['samples'] - i), np.int16)))
        assert [e.frame_index for e in found] == fixture['expected_frames']
        assert all(e.keyword_index == 1 and e.sensitivity == 1 and e.sample_index == (e.frame_index + 1) * 512 for e in found)
        assert dict(wake.finish()) == {'frames': 70, 'discarded_samples': 17}
