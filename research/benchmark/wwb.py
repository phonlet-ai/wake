"""Phonlet Wake on Picovoice's public wake-word benchmark, the way Hark reports it.

Data: the keyword recordings of github.com/Picovoice/wake-word-benchmark, LibriSpeech test-clean as
background speech, and DEMAND noise (channel 1 of the 17 environments of the 16 kHz release) at
10 dB SNR, plus a clean condition. The mixing follows Hark's reimplementation of Picovoice's
mixer.py and dataset.py (scripts/wwb_eval.py in github.com/pioborgelt/hark), with the same seed
(778) and the same two deviations from upstream: a background utterance is excluded when its
upper-case transcript contains the upper-case word, and each keyword counts as detected at most
once. No downloaded code runs: this file is our own implementation of that recipe.

Detection runs in the Phonlet Wake engine (libkws_engine, C API) with the keyword file the
threshold generator writes from text. The engine scores a grid of sensitivities in one pass
(production arbitration, the mode of the public API; check_api() proves the two agree).

Metric: hit rate at no more than 0.1 false alarms per hour of stream (1 per 10 hours), per word,
best over the sensitivity grid, then the mean over the six words. Also reported: one sensitivity
for every word, and sensitivity 0, the calibrated setting that aims at 0.1 false alarms per hour.

Usage (paths are arguments; nothing is downloaded):
  python research/benchmark/wwb.py --corpora DIR --keywords FILE --lib LIB --model MODEL --out FILE
"""
import argparse
import ctypes
import json
import os
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
import soundfile as sf

RATE = 16000
FRAME = 512  # engine frame and the mixer's energy frame
WORDS = ['alexa', 'computer', 'jarvis', 'smart mirror', 'snowboy', 'view glass']
DEMAND = ['DKITCHEN', 'DLIVING', 'DWASHING', 'NFIELD', 'NPARK', 'NRIVER', 'OHALLWAY', 'OMEETING',
          'OOFFICE', 'PCAFETER', 'PRESTO', 'PSTATION', 'SPSQUARE', 'STRAFFIC', 'TBUS', 'TCAR', 'TMETRO']
SENSITIVITIES = [round(i / 50, 2) for i in range(51)]
FA_PER_HOUR = 0.1


# ---------- The stream (Hark's build(), reimplemented) ----------
class Files:
    """Sorted paths with their own RNG, seeded like upstream's Dataset."""

    def __init__(self, paths):
        self.paths = sorted(paths)
        self.rng = np.random.RandomState(778)

    def get(self, i):
        x, rate = sf.read(self.paths[i], dtype='float32')
        assert rate == RATE, self.paths[i]
        return x

    def random(self):
        return self.get(self.rng.randint(low=0, high=len(self.paths)))


def readable(path):
    """Some recordings cannot be decoded; they are dropped and the keyword count reflects it."""
    try:
        return path.is_file() and sf.read(str(path), dtype='float32')[0].size > 0
    except Exception:
        return False


def background_files(root, word):
    out = []
    for dirpath, _, names in os.walk(root):
        for name in names:
            if name.endswith('.trans.txt'):
                for line in open(Path(dirpath) / name):
                    stem, text = line.split(' ', 1)
                    if word.upper() not in text:
                        out.append(str(Path(dirpath) / f'{stem}.flac'))
    return out


def energy(x):
    n = x.size // FRAME
    return float((x[:n * FRAME].reshape(n, FRAME) ** 2).sum(1).max()) if n else 0.0


def build(word, corpora, hours, snr, path):
    """Writes the mixed stream as float32 to `path` (peak 1) and returns keyword windows in seconds."""
    rng = np.random.RandomState(778)
    keywords = Files([str(p) for p in (corpora / 'picovoice-wake-word-benchmark/repo/audio' / word).iterdir()
                      if readable(p)])
    background = Files(background_files(corpora / 'librispeech/LibriSpeech/test-clean', word))
    noise = Files([str(corpora / 'demand-16k' / env / 'ch01.wav') for env in DEMAND])
    each = int(hours * 3600 * RATE) // (len(keywords.paths) + 1)

    def part_list():
        parts, total = [], 0
        while total < each:
            x = background.random()
            total += x.size
            parts.append(x / max(np.abs(x).max(), 1e-9) if rng.uniform() < 0.2 else x.size)
        return parts

    size = lambda p: p.size if isinstance(p, np.ndarray) else p
    parts = part_list()
    position = sum(size(p) for p in parts)
    windows = []
    for i in rng.permutation(np.arange(len(keywords.paths))):
        x = keywords.get(i)
        windows.append((position / RATE, (position + x.size) / RATE + 0.5))
        position += x.size
        parts.append(x / max(np.abs(x).max(), 1e-9))
        more = part_list()
        parts += more
        position += sum(size(p) for p in more)

    out = np.lib.format.open_memmap(path, mode='w+', dtype=np.float32, shape=(position,))
    filled = 0
    while filled < position:  # noise, each recording peak-normalised, cut to the stream length
        x = noise.random()
        x = x / max(np.abs(x).max(), 1e-9)
        n = min(x.size, position - filled)
        out[filled:filled + n] = x[:n] if snr < 99 else 0.0
        filled += n
    start = 0
    for p in parts:
        if isinstance(p, np.ndarray):
            segment = out[start:start + p.size]
            speech = energy(p)
            if snr >= 99:
                segment += p * 0.3
            elif speech > 0:
                segment += p * np.sqrt(energy(segment) * 10 ** (snr / 10) / speech)
            start += p.size
        else:
            start += p
    peak = float(max(np.abs(out[i:i + RATE * 600]).max() for i in range(0, position, RATE * 600)))
    out.flush()
    return windows, peak, len(background.paths), len(keywords.paths)


# ---------- The engine ----------
def engine(lib_path):
    lib = ctypes.CDLL(lib_path)
    p, i64, c = ctypes.c_void_p, ctypes.c_int64, ctypes.c_char_p
    lib.kws_error.restype = c
    lib.kws_model_load.restype, lib.kws_model_load.argtypes = p, [c, c]
    lib.kws_session_create.restype = p
    lib.kws_session_create.argtypes = [p, c, ctypes.POINTER(ctypes.c_double), i64, ctypes.c_int]
    lib.kws_session_push.restype = i64
    lib.kws_session_push.argtypes = [p, p, i64, ctypes.c_int, i64, p, p, p, p, p, p]
    lib.kws_session_finish.restype, lib.kws_session_finish.argtypes = i64, [p]
    lib.kws_session_destroy.argtypes = [p]
    lib.kws_model_destroy.argtypes = [p]
    lib.phonlet_wake_init.restype = ctypes.c_int
    lib.phonlet_wake_init.argtypes = [c, c, ctypes.POINTER(ctypes.c_double), ctypes.c_int32,
                                      ctypes.POINTER(p)]
    lib.phonlet_wake_process.restype = ctypes.c_int
    lib.phonlet_wake_process.argtypes = [p, p, ctypes.c_int32, ctypes.POINTER(ctypes.c_int32)]
    lib.phonlet_wake_delete.argtypes = [p]
    lib.phonlet_wake_error.restype = c
    return lib


def enrollment(keyword):
    keys = ('name', 'pronunciations', 'threshold_loose', 'threshold_strict', 'endpoint')
    return json.dumps([{k: keyword[k] for k in keys}]).encode()


def pcm_chunks(stream, peak, chunk):
    for i in range(0, stream.size, chunk):
        yield np.ascontiguousarray(np.clip(np.rint(stream[i:i + chunk] / peak * 32767), -32768, 32767)
                                   .astype(np.int16))


def grid_fires(lib, model, keyword, stream, peak, sensitivities):
    """Frames at which each sensitivity fires: list of arrays, one per sensitivity."""
    values = (ctypes.c_double * len(sensitivities))(*sensitivities)
    session = lib.kws_session_create(model, enrollment(keyword), values, len(sensitivities), 1)
    if not session:
        raise RuntimeError(lib.kws_error().decode())
    fires = [[] for _ in sensitivities]
    frame = 0
    try:
        for pcm in pcm_chunks(stream, peak, FRAME * 2000):
            capacity = pcm.size // FRAME + 1
            out = np.zeros((capacity, len(sensitivities), 1), dtype=np.uint8)
            n = lib.kws_session_push(session, pcm.ctypes.data, pcm.size, 0, capacity, None, None, None,
                                     None, out.ctypes.data, None)
            if n < 0:
                raise RuntimeError(lib.kws_error().decode())
            for s in range(len(sensitivities)):
                fires[s].extend((np.nonzero(out[:n, s, 0])[0] + frame).tolist())
            frame += n
        lib.kws_session_finish(session)
    finally:
        lib.kws_session_destroy(session)
    return [np.asarray(f, dtype=np.int64) for f in fires]


def score(frames, windows):
    """Hark's score(): an event inside a keyword window [start, end + 0.5 s] is a hit (each keyword
    once); any other event is a false alarm. A frame's event time is the end of the frame."""
    seen, fa, j = set(), 0, 0
    for f in frames:
        t = (f + 1) * FRAME / RATE
        while j < len(windows) and windows[j][1] < t:
            j += 1
        if j < len(windows) and windows[j][0] <= t <= windows[j][1]:
            seen.add(j)
        else:
            fa += 1
    return len(seen), fa


def run_one(task):
    word, snr, a = task
    corpora, data = Path(a['corpora']), Path(a['data'])
    keyword = next(k for k in json.loads(Path(a['keywords']).read_text())['keywords'] if k['name'] == word)
    path = data / f"{word.replace(' ', '_')}_{'clean' if snr >= 99 else f'{snr:g}dB'}.npy"
    t0 = time.monotonic()
    windows, peak, n_bg, n_kw = build(word, corpora, a['hours'], snr, path)
    built = time.monotonic() - t0
    stream = np.load(path, mmap_mode='r')
    hours = stream.size / RATE / 3600
    lib = engine(a['lib'])
    model = lib.kws_model_load(a['model'].encode(), b'en')
    if not model:
        raise RuntimeError(lib.kws_error().decode())
    t1 = time.monotonic()
    fires = grid_fires(lib, model, keyword, stream, peak, a['sensitivities'])
    lib.kws_model_destroy(model)
    rows = []
    for s, frames in zip(a['sensitivities'], fires):
        hits, fa = score(frames, windows)
        rows.append({'sensitivity': s, 'hits': hits, 'false_alarms': fa})
    del stream
    os.remove(path)  # streams are rebuilt deterministically; keep the disk free
    return {'word': word, 'snr': snr, 'hours': hours, 'keywords': n_kw, 'background_files': n_bg,
            'source': keyword['metadata'].get('source'), 'build_s': round(built),
            'engine_s': round(time.monotonic() - t1), 'rows': rows}


def check_api(a, seconds=1800):
    """The grid session and the public API (phonlet_wake_init/process) fire on the same frames."""
    corpora, data = Path(a['corpora']), Path(a['data'])
    keyword = next(k for k in json.loads(Path(a['keywords']).read_text())['keywords'] if k['name'] == 'jarvis')
    path = data / 'check.npy'
    build('jarvis', corpora, 0.5, 10.0, path)
    stream = np.load(path, mmap_mode='r')[:seconds * RATE]
    peak = float(np.abs(np.load(path, mmap_mode='r')).max())
    lib = engine(a['lib'])
    model = lib.kws_model_load(a['model'].encode(), b'en')
    checks = [0.0, 0.5, 1.0]
    grid = grid_fires(lib, model, keyword, stream, peak, checks)
    lib.kws_model_destroy(model)
    for s, expected in zip(checks, grid):
        wake = ctypes.c_void_p()
        value = (ctypes.c_double * 1)(s)
        status = lib.phonlet_wake_init(a['model'].encode(), enrollment(keyword), value, 1, ctypes.byref(wake))
        assert status == 0, lib.phonlet_wake_error().decode()
        frames, frame, index = [], 0, ctypes.c_int32()
        for pcm in pcm_chunks(stream, peak, FRAME):  # one frame per call, so a fire maps to a frame
            status = lib.phonlet_wake_process(wake, pcm.ctypes.data, pcm.size, ctypes.byref(index))
            assert status == 0, lib.phonlet_wake_error().decode()
            if index.value >= 0:
                frames.append(frame)
            frame += 1
        lib.phonlet_wake_delete(wake)
        assert frames == expected.tolist(), (s, frames[:10], expected[:10].tolist())
        print(json.dumps({'check': 'public API equals grid', 'sensitivity': s, 'fires': len(frames)}), flush=True)
    os.remove(path)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--corpora', required=True)
    p.add_argument('--keywords', required=True, help='keyword file from the threshold generator')
    p.add_argument('--lib', required=True, help='libkws_engine.so')
    p.add_argument('--model', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--data', default='research/data/wwb')
    p.add_argument('--hours', type=float, default=24.0)
    p.add_argument('--snrs', default='10,99', help='dB; 99 means clean')
    p.add_argument('--words', default=','.join(WORDS))
    p.add_argument('--workers', type=int, default=3)
    p.add_argument('--check', action='store_true', help='only prove the grid equals the public API')
    a = vars(p.parse_args())
    a['sensitivities'] = SENSITIVITIES
    Path(a['data']).mkdir(parents=True, exist_ok=True)
    if a['check']:
        check_api(a)
        return
    tasks = [(w, float(s), a) for s in a['snrs'].split(',') for w in a['words'].split(',')]
    results = []
    with ProcessPoolExecutor(a['workers']) as pool:
        for result in pool.map(run_one, tasks):
            print(json.dumps({k: result[k] for k in ('word', 'snr', 'hours', 'keywords', 'build_s', 'engine_s')}),
                  flush=True)
            results.append(result)
    Path(a['out']).parent.mkdir(parents=True, exist_ok=True)
    Path(a['out']).write_text(json.dumps({'protocol': __doc__.strip().splitlines()[0], 'sensitivities': SENSITIVITIES,
                                          'fa_per_hour': FA_PER_HOUR, 'results': results}, indent=1) + '\n')
    print('wrote', a['out'])


if __name__ == '__main__':
    main()
