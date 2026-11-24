#!/usr/bin/env python3
"""Embed the engine's ordered language inventories into a native build."""
import argparse
import hashlib
import json
import re
from pathlib import Path


def generate(source, output):
    presets = []
    for path in sorted(Path(source).glob('*.json')):
        preset = json.loads(path.read_text(encoding='utf-8'))
        fields = {'schema_version', 'code', 'name', 'blank', 'symbols'}
        if not isinstance(preset, dict) or set(preset) != fields:
            raise ValueError(f'invalid preset fields: {path}; expected {sorted(fields)}')
        if type(preset['schema_version']) is not int or preset['schema_version'] != 1:
            raise ValueError(f'unsupported preset schema_version: {path}')
        code = preset['code']
        if not isinstance(code, str) or not re.fullmatch('[a-z][a-z0-9_]*', code) or code != path.stem.lower():
            raise ValueError(f'preset code must match filename: {path}')
        if any(p['code'] == code for p in presets):
            raise ValueError(f'duplicate preset code: {code}')
        if not isinstance(preset['name'], str) or not preset['name'].strip():
            raise ValueError(f'invalid preset name: {path}')
        if type(preset['blank']) is not int or preset['blank'] != 0:
            raise ValueError(f'blank must be class 0: {path}')
        symbols = preset['symbols']
        if (not isinstance(symbols, list) or len(symbols) < 2
                or any(not isinstance(s, str) or not s or s != s.strip() for s in symbols)
                or symbols[0] != 'blank' or len(symbols) != len(set(symbols))):
            raise ValueError(f'invalid ordered symbols: {path}')
        inventory_id = hashlib.sha256(json.dumps(symbols, ensure_ascii=False, separators=(',', ':')).encode()).hexdigest()
        presets.append(dict(preset, n_classes=len(symbols), inventory_id=inventory_id))
    if not any(p['code'] == 'en' for p in presets):
        raise ValueError('English default inventory is missing')
    lines = ['#pragma once', '#include "kws/language.hpp"', 'namespace kws::detail {',
             'inline const std::vector<LanguagePreset> language_presets = {']
    for p in presets:
        symbols = ','.join(json.dumps(s, ensure_ascii=False) for s in p['symbols'])
        metadata = json.dumps(p, ensure_ascii=False, separators=(',', ':'))
        lines.append('{' + json.dumps(p['code']) + ',{' + symbols + '},' +
                     json.dumps(p['inventory_id']) + ',' + json.dumps(metadata, ensure_ascii=False) + '},')
    lines += ['};', '}']
    Path(output).write_text('\n'.join(lines) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inventories', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    generate(args.inventories, args.output)
