"""Summarises a wwb.py run: per-word hit rates at no more than 0.1 false alarms per hour, the
comparison with published numbers for other engines, and the chart.

  python research/benchmark/report.py RESULTS.json --markdown OUT.md --svg OUT.svg --json OUT.json

Phonlet Wake numbers come from RESULTS.json (our engine on our run). Every other number is copied
from its source, named in PUBLISHED; we did not run those engines.
"""
import argparse
import json
from pathlib import Path

WORDS = ['alexa', 'computer', 'jarvis', 'smart mirror', 'snowboy', 'view glass']
HARK = 'Hark docs/BENCHMARKS.md, github.com/pioborgelt/hark at 45a01d9 (2026-11-20)'
# Hit rate (%) at <= 0.1 false alarms per hour, 10 dB DEMAND noise, 6-word mean, as published.
PUBLISHED = [
    {'engine': 'Picovoice Porcupine v4', 'kind': 'a model per word (closed)',
     'mean': 97.30, 'source': f'published by Picovoice with its original scoring, as listed in {HARK}'},
    {'engine': 'Hark v0.3', 'kind': 'typed, no training', 'mean': 78.14, 'clean': 91.01,
     'words': {'alexa': 78.41, 'computer': 64.23, 'jarvis': 78.39, 'smart mirror': 85.09,
               'snowboy': 83.54, 'view glass': 79.20},
     'source': HARK},
    {'engine': 'Kitt.ai Snowboy', 'kind': 'recorded templates', 'mean': 68.10,
     'source': f'published by Picovoice with its original scoring, as listed in {HARK}'},
    {'engine': 'sherpa-onnx KWS 3.3M', 'kind': 'typed, no training', 'mean': 66.00,
     'words': {'alexa': 82.9, 'computer': 63.3, 'jarvis': 57.6, 'smart mirror': 66.1,
               'snowboy': 69.3, 'view glass': 57.1},
     'source': HARK},
    {'engine': 'CMU PocketSphinx 5.1.1', 'kind': 'dictionary lookup', 'mean': 48.00,
     'source': f'published by Picovoice with its original scoring, as listed in {HARK}'},
    {'engine': 'Vosk 0.3.45', 'kind': 'speech recognizer grammar', 'mean': 32.69,
     'words': {'alexa': 0.0, 'computer': 0.0, 'jarvis': 0.0, 'smart mirror': 98.6,
               'snowboy': 97.5, 'view glass': 0.0},
     'source': HARK},
]
# Fixed pre-trained single-word models exist for some words only.
PARTIAL = [
    {'engine': 'openWakeWord 0.6.0', 'words': {'alexa': 87.9, 'jarvis': 63.3, 'computer': 0.0}, 'source': HARK},
    {'engine': 'microWakeWord 2.5.0', 'words': {'alexa': 89.5}, 'source': HARK},
]


def best_of(rows, cap, keywords):
    ok = [row for row in rows if row['false_alarms'] <= cap]
    best = max(ok, key=lambda row: (row['hits'], -row['false_alarms']), default=None)
    if best is None:
        return {'sensitivity': None, 'hit': 0.0, 'false_alarms': None, 'range': None}
    return {'sensitivity': best['sensitivity'], 'hit': 100 * best['hits'] / keywords,
            'false_alarms': best['false_alarms'], 'range': best.get('range', 'shipped')}


def summarise(results, extra=()):
    """Per condition: the best sensitivity per word, one sensitivity for all words, and sensitivity 0.

    `extra` holds runs of the same streams with a stricter calibration target (the generator's
    --rate-strict): their operating points extend the sweep below sensitivity 0, as the other
    engines' thresholds are swept freely. 'best' uses every operating point; 'best_shipped' only
    the shipped sensitivity range of the default keyword file."""
    more = {(r['word'], r['snr']): r for r in extra}
    by = {}
    for r in results:
        cap = 0.1 * r['hours']
        rows = {row['sensitivity']: row for row in r['rows']}
        stricter = more.get((r['word'], r['snr']))
        if stricter:
            assert stricter['keywords'] == r['keywords'] and abs(stricter['hours'] - r['hours']) < 1e-9, 'not the same stream'
        points = r['rows'] + ([{**row, 'range': 'stricter'} for row in stricter['rows']] if stricter else [])
        by.setdefault(r['snr'], {})[r['word']] = {
            'keywords': r['keywords'], 'hours': r['hours'], 'cap': cap, 'rows': rows, 'source': r['source'],
            'best': best_of(points, cap, r['keywords']),
            'best_shipped': best_of(r['rows'], cap, r['keywords']),
            'calibrated': {'hit': 100 * rows[0.0]['hits'] / r['keywords'],
                           'false_alarms': rows[0.0]['false_alarms'],
                           'fa_per_hour': rows[0.0]['false_alarms'] / r['hours']},
        }
    out = {}
    for snr, words in by.items():
        sens = sorted(next(iter(words.values()))['rows'])
        valid = [s for s in sens if all(w['rows'][s]['false_alarms'] <= w['cap'] for w in words.values())]
        mean_at = lambda s: sum(100 * w['rows'][s]['hits'] / w['keywords'] for w in words.values()) / len(words)
        single = max(valid, key=mean_at) if valid else None
        out[snr] = {
            'words': {k: {key: v[key] for key in ('keywords', 'hours', 'source', 'best', 'best_shipped', 'calibrated')}
                      for k, v in words.items()},
            'best_mean': sum(w['best']['hit'] for w in words.values()) / len(words),
            'best_shipped_mean': sum(w['best_shipped']['hit'] for w in words.values()) / len(words),
            'single': {'sensitivity': single, 'mean': mean_at(single) if single is not None else None,
                       'words': {k: 100 * w['rows'][single]['hits'] / w['keywords'] for k, w in words.items()}
                       if single is not None else None},
            'calibrated_mean': sum(w['calibrated']['hit'] for w in words.values()) / len(words),
            'calibrated_fa_per_hour': sum(w['calibrated']['false_alarms'] for w in words.values())
            / sum(w['hours'] for w in words.values()),
        }
    return out


HARK_URL = 'https://github.com/pioborgelt/hark/blob/45a01d974c03937d0f581dbdd0c4c08ad17618e0/docs/BENCHMARKS.md'


def markdown(summary):
    """The results tables: engines at 10 dB, then Phonlet Wake per word next to the published values."""
    noisy, clean = summary[10.0], summary.get(99.0)
    for condition in (noisy, clean):
        if condition and set(condition['words']) != set(WORDS):
            raise SystemExit('the report needs all six words in every condition')
    lines = ['| Engine | How a wake word is made | Hit rate, 10 dB |', '| --- | --- | ---: |']
    rows = [{'engine': 'Phonlet Wake 0.1.0', 'kind': 'typed, no training', 'mean': noisy['best_mean'],
             'ours': True}] + PUBLISHED
    for r in sorted(rows, key=lambda r: -r['mean']):
        if r.get('ours'):
            lines.append(f"| **{r['engine']}** | {r['kind']} | **{r['mean']:.1f}%** |")
        else:
            lines.append(f"| {r['engine']} | {r['kind']} | {r['mean']:.1f}% |")
    lines += ['', '| Word | Phonlet Wake, 10 dB | Phonlet Wake, clean | sherpa-onnx, 10 dB | Hark v0.3, 10 dB |'
              ' openWakeWord, 10 dB | microWakeWord, 10 dB |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: |']
    hark, sherpa = PUBLISHED[1]['words'], PUBLISHED[3]['words']
    oww, mww = PARTIAL[0]['words'], PARTIAL[1]['words']
    fmt = lambda d, w: f'{d[w]:.1f}%' if w in d else 'n/a'
    mark = lambda b: f"{b['hit']:.1f}%" + (' *' if b['range'] == 'stricter' else '')
    for w in WORDS:
        c = mark(clean['words'][w]['best']) if clean else ''
        lines.append(f"| {w} | {mark(noisy['words'][w]['best'])} | {c} | {fmt(sherpa, w)} | {fmt(hark, w)} | "
                     f'{fmt(oww, w)} | {fmt(mww, w)} |')
    c = f"**{clean['best_mean']:.1f}%**" if clean else ''
    lines.append(f"| **mean** | **{noisy['best_mean']:.1f}%** | {c} | 66.0% | 78.1% | partial | partial |")
    if any(c and any(w['best']['range'] == 'stricter' for w in c['words'].values()) for c in (noisy, clean)):
        lines += ['', '\\* With a stricter calibration target (`wake-threshold --rate-strict`). Within the default '
                  f"sensitivity range the 10 dB mean is {noisy['best_shipped_mean']:.1f}%."]
    return '\n'.join(lines) + '\n'


def svg(summary):
    """Horizontal bars, one per engine, sorted; Phonlet Wake in the brand orange, the rest neutral."""
    rows = [{'engine': 'Phonlet Wake 0.1.0', 'kind': 'typed, no training', 'mean': summary[10.0]['best_mean'],
             'ours': True}]
    for r in PUBLISHED:
        rows.append({'engine': r['engine'], 'kind': r['kind'], 'mean': r['mean'], 'ours': False})
    rows.sort(key=lambda r: -r['mean'])
    width, left, right, top, bar, gap = 810, 300, 70, 64, 22, 26  # gutter fits DejaVu Sans at 11 px
    plot = width - left - right
    height = top + len(rows) * (bar + gap) + 40
    esc = lambda s: s.replace('&', '&amp;').replace('<', '&lt;')
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" '
           f'height="{height}" role="img" aria-labelledby="t d">',
           '<title id="t">Hit rate at no more than 0.1 false alarms per hour, 10 dB noise</title>',
           '<desc id="d">' + esc('; '.join(f"{r['engine']} {r['mean']:.1f}%" for r in rows)) + '</desc>',
           '<style>text{font:13px system-ui,-apple-system,Segoe UI,sans-serif;fill:#3d3f37}'
           '.k{font-size:11px;fill:#6b6d63}.h{font-size:15px;font-weight:600;fill:#1d1f19}'
           '.v{font-weight:600}.grid{stroke:#d9d6cc;stroke-width:1}.bar{fill:#a7a69c}.ours{fill:#c94322}'
           '@media (prefers-color-scheme:dark){text{fill:#d6d2c6}.k{fill:#a6a294}.h{fill:#f3efe6}'
           '.grid{stroke:#3a3b33}.bar{fill:#6f6e66}.ours{fill:#ec6a44}}</style>',
           '<text class="h" x="0" y="20">Picovoice wake-word benchmark: hit rate at ≤ 0.1 false alarms/hour</text>',
           '<text class="k" x="0" y="40">Mean of six words, 10 dB noise. Higher is better.</text>']
    for tick in range(0, 101, 25):
        x = left + plot * tick / 100
        out.append(f'<line class="grid" x1="{x:.1f}" y1="{top - 6}" x2="{x:.1f}" y2="{height - 34}"/>')
        out.append(f'<text class="k" x="{x:.1f}" y="{height - 18}" text-anchor="middle">{tick}%</text>')
    for i, r in enumerate(rows):
        y = top + i * (bar + gap)
        w = max(plot * r['mean'] / 100, 2)
        cls = 'ours' if r['ours'] else 'bar'
        strong = ' class="v"' if r['ours'] else ''
        out.append(f'<text x="{left - 12}" y="{y + 10}" text-anchor="end"{strong}>{esc(r["engine"])}</text>')
        out.append(f'<text class="k" x="{left - 12}" y="{y + 24}" text-anchor="end">{esc(r["kind"])}</text>')
        out.append(f'<rect class="{cls}" x="{left}" y="{y}" width="{w:.1f}" height="{bar}" rx="4"/>')
        out.append(f'<text class="v" x="{left + w + 8:.1f}" y="{y + 16}">{r["mean"]:.1f}%</text>')
    out.append('</svg>')
    return '\n'.join(out) + '\n'


def main():
    p = argparse.ArgumentParser()
    p.add_argument('results')
    p.add_argument('--extra', nargs='*', default=[], help='runs with a stricter calibration target')
    p.add_argument('--markdown')
    p.add_argument('--svg')
    p.add_argument('--json')
    a = p.parse_args()
    extra = [r for path in a.extra for r in json.loads(Path(path).read_text())['results']]
    summary = summarise(json.loads(Path(a.results).read_text())['results'], extra)
    md = markdown(summary)
    print(md)
    if a.markdown:
        Path(a.markdown).write_text(md)
    if a.svg:
        Path(a.svg).write_text(svg(summary))
    if a.json:
        Path(a.json).write_text(json.dumps({str(k): v for k, v in summary.items()}, indent=1) + '\n')


if __name__ == '__main__':
    main()
