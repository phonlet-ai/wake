import copy
import hashlib
import json
from pathlib import Path
import pytest
from phonlet_wake.keywords import WakeError, read_keywords, select_phrases, validate_phrases

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / 'resources/models/en-1/model.pwake'
ENGLISH = ROOT / 'resources/keywords/en.json'


def model_bytes():
    return MODEL.read_bytes()


def english():
    return json.loads(ENGLISH.read_text())


def test_english_file_names_its_model():
    doc = english()
    assert doc['model'] == {'id': 'en-1', 'sha256': hashlib.sha256(model_bytes()).hexdigest()}
    assert [k['name'] for k in doc['keywords']] == ['hey phonlet', 'ok phonlet', 'hey assistant', 'assistant', 'hey wake']


def test_path_object_and_list_give_the_same_phrases():
    doc = english()
    by_path = select_phrases(ENGLISH, model_bytes(), 0.7)
    by_str = select_phrases(str(ENGLISH), model_bytes(), 0.7)
    by_object = select_phrases(doc, model_bytes(), 0.7)
    by_list = select_phrases(doc['keywords'], model_bytes(), 0.7)
    assert by_path == by_str == by_object == by_list
    assert [k['id'] for k in by_path] == [k['name'] for k in doc['keywords']]
    assert all(k['sensitivity'] == 0.7 for k in by_path)


def test_per_phrase_sensitivity_and_private_copies():
    doc = english()
    doc['keywords'][0]['sensitivity'] = 0
    selected = select_phrases(doc, model_bytes(), 0.7)
    assert [k['sensitivity'] for k in selected][:2] == [0, 0.7]
    doc['keywords'][0]['pronunciations'][0].append(99)
    assert 99 not in selected[0]['pronunciations'][0]
    for bad in [None, True, '0.5', float('nan'), float('inf'), -0.1, 1.1]:
        doc = english()
        doc['keywords'][0]['sensitivity'] = bad
        with pytest.raises(WakeError):
            select_phrases(doc, model_bytes())


def test_model_binding():
    doc = english()
    doc['model']['sha256'] = '0' * 64
    with pytest.raises(WakeError, match='made for model en-1') as error:
        select_phrases(doc, model_bytes())
    assert error.value.code == 'INCOMPATIBLE_ARTIFACT'
    # Phrases without a "model" block are not checked.
    select_phrases(english()['keywords'], b'any model bytes')


@pytest.mark.parametrize('patch', [
    {'unknown': True},
    {'model': {'id': 'en-1'}},
    {'model': {'id': '', 'sha256': '0' * 64}},
    {'model': {'id': 'en-1', 'sha256': 'abc'}},
    {'metadata': []},
    {'keywords': []},
], ids=lambda p: json.dumps(p)[:40])
def test_bad_keyword_files(patch):
    with pytest.raises(WakeError):
        read_keywords({**english(), **patch})


@pytest.mark.parametrize('patch', [
    {'endpoint': 'true'},
    {'pronunciations': []},
    {'pronunciations': [[0]]},
    {'pronunciations': [[40]]},
    {'threshold_loose': 10, 'threshold_strict': 0},
    {'threshold_loose': float('nan')},
    {'name': ''},
    {'metadata': 'note'},
    {'threshold': -1.0},
], ids=lambda p: json.dumps(p)[:40])
def test_bad_phrases(patch):
    phrases = english()['keywords']
    phrases[0].update(patch)
    with pytest.raises(WakeError):
        validate_phrases(phrases)


def test_missing_field_and_duplicates():
    phrases = english()['keywords']
    del phrases[0]['threshold_strict']
    with pytest.raises(WakeError, match='missing.*threshold_strict'):
        validate_phrases(phrases)
    phrases = english()['keywords']
    phrases.append(copy.deepcopy(phrases[0]))
    with pytest.raises(WakeError, match='duplicate'):
        validate_phrases(phrases)


def test_unreadable_files(tmp_path):
    with pytest.raises(WakeError) as error:
        read_keywords(tmp_path / 'missing.json')
    assert error.value.code == 'CORRUPT_ARTIFACT'
    broken = tmp_path / 'broken.json'
    broken.write_text('{"keywords": [')
    with pytest.raises(WakeError, match='not valid JSON'):
        read_keywords(broken)
