"""Repository gate: source layout, local documentation links, asset identities and banned words."""
from pathlib import Path
from urllib.parse import unquote, urlsplit
import hashlib
import json
import os
import re
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
GENERATED = {
    'build', 'dist', 'engine/build', 'engine/wasm/build',
    'sdk/python/build', 'sdk/python/dist', 'packaging/model-python/build',
    'research/data', 'research/runs', 'results',
}
SKIP_NAMES = {'.git', '.venv', '.pytest_cache', '__pycache__', 'node_modules',
              # Gradle, CMake and NDK build trees (Android and JVM builds)
              'build', '.cxx', '.gradle', '.kotlin', 'local.properties',
              # ESP-IDF generated configuration and downloaded components
              'sdkconfig', 'sdkconfig.old', 'managed_components',
              # Cargo build tree (the generator)
              'target'}
# Minified Emscripten loader glue: its browser-API identifiers can collide with the word list.
# The model's training lexicon (verified by SHA-256) is a list of ordinary English words; a few of
# them are on the banned list as words, not as product text.
GENERATED_TEXT_EXEMPT = ('sdk/web/dist/', 'generator/assets/bundle/lexicon.txt')

# Banned words, stored as the first 16 hex digits of their SHA-256 so the repository never
# contains them: other vendors' product names, words about taking software apart, and the
# enrollment tool's own field and file names (only the importer reads those, built from
# fragments). Matching is case-insensitive on whole words, word prefixes, underscore names,
# underscore-name prefixes and one two-word phrase.
WORDS = {'4236d62640f6e422', 'aacf6e0b2157e258', 'f4435ec544de04f8',
         'dabc6391b6c0df71', '6f6fe476d643adaa', '430b05983de9fe8b'}
WORD_PREFIXES = {7: {'cdfbc38622aa09e0'}, 8: {'2b3d4ca3d1124ece'}, 9: {'0473fc62a33394c8'},
                 10: {'c521018fc36a56f8', 'af41d2ad13197196'}}
NAMES = {'e9399ea17a944495', '0e1187a00b70f5e1'}
NAME_PREFIXES = {3: {'6729ccabf8e79971'}}
PHRASES = {'11108fa8a883da89'}  # first word plus the first 8 letters of the next
# The benchmark's vendor and its engine may be named where the benchmark is reported, and nowhere
# else (maintainer's decision, 2026-11-27): these files, and the README's Benchmark section.
BENCHMARK_WORDS = {'4236d62640f6e422', 'aacf6e0b2157e258'}
BENCHMARK_FILES = ('docs/benchmark.md', 'docs/benchmark.svg', 'research/benchmark/')


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()[:16]


def benchmark_section(body):
    """Line numbers of the README's Benchmark section (its heading up to the next one)."""
    lines, inside, section = body.splitlines(), False, set()
    for number, line in enumerate(lines, 1):
        if line.startswith('## '):
            inside = line.strip() == '## Benchmark'
        if inside:
            section.add(number)
    return section


def banned_lines(body, allowed=lambda number: False):
    """Line numbers that contain a banned word; `allowed(number)` admits the benchmark words."""
    found = []
    for number, line in enumerate(body.lower().splitlines(), 1):
        words = re.findall(r'[a-z0-9]+', line)
        names = re.findall(r'[a-z0-9_]+', line)
        banned = WORDS - BENCHMARK_WORDS if allowed(number) else WORDS
        hit = any(digest(w) in banned for w in words)
        hit = hit or any(digest(w[:n]) in hashes for w in words for n, hashes in WORD_PREFIXES.items()
                         if len(w) >= n)
        hit = hit or any(digest(w) in NAMES for w in names)
        hit = hit or any(digest(w[:n]) in hashes for w in names for n, hashes in NAME_PREFIXES.items()
                         if len(w) > n)
        hit = hit or any(digest(a + ' ' + b[:8]) in PHRASES for a, b in zip(words, words[1:]))
        if hit:
            found.append(number)
    return found


def generated(path):
    relative = path.relative_to(ROOT).as_posix()
    return (path.name in SKIP_NAMES or path.name.endswith('.egg-info')
            or relative in GENERATED or (relative.startswith('demos/') and path.name == 'dist'))


def source_files():
    # The walk also reports empty directories, which Git does not record.
    files, errors = [], []
    for current, directories, names in os.walk(ROOT):
        base = Path(current)
        directories[:] = [name for name in directories if not generated(base / name)]
        if base != ROOT and not directories and not names:
            errors.append(f'Empty source directory: {base.relative_to(ROOT)}')
        files.extend(base / name for name in names
                     if not name.endswith(('.pyc', '.pyo')) and not generated(base / name))
    return files, errors


def main():
    files, errors = source_files()
    links = 0
    for path in files:
        if path.suffix != '.md':
            continue
        body = re.sub(r'```.*?```', '', path.read_text(), flags=re.S)
        targets = re.findall(r'\[[^\]]*\]\(([^\s)]+)', body)
        targets += re.findall(r'(?:src|srcset)=["\']([^"\']+)["\']', body)
        for target in targets:
            parsed = urlsplit(target)
            if parsed.scheme or parsed.netloc or not parsed.path:
                continue
            links += 1
            resolved = (path.parent / unquote(parsed.path)).resolve()
            if not resolved.is_relative_to(ROOT) or not resolved.exists():
                errors.append(f'{path.relative_to(ROOT)}: broken local link {target}')

    manifest = json.loads((ROOT / 'assets/brand/manifest.json').read_text())
    for name, entry in manifest['files'].items():
        path = ROOT / 'assets/brand' / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != entry['sha256']:
            errors.append(f'Brand asset hash mismatch: {name}')
            continue
        if path.suffix == '.svg':
            for element in ET.fromstring(path.read_bytes()).iter():
                if element.tag.split('}')[-1] in {'image', 'script', 'text', 'tspan', 'foreignObject'}:
                    errors.append(f'Unexpected non-vector/dependent element in {name}')
                if any(key.split('}')[-1] == 'href' and not value.startswith('#')
                       for key, value in element.attrib.items()):
                    errors.append(f'External SVG dependency in {name}')

    # The production model and the raw English keyword array never change silently.
    frozen = {
        'resources/models/en-1/model.pwake': '8be3687d060f953d0069480cf53d1fd0aa66e04551c2a4b600823a992a864af8',
        'resources/keywords/en.json': '8b551e2fcf5893c9bac10e0939a7dcc9ee175cda795207ad44e756321f351f61',
    }
    for name, expected in frozen.items():
        if hashlib.sha256((ROOT / name).read_bytes()).hexdigest() != expected:
            errors.append(f'Production asset changed: {name}')

    for path in files:
        relative = path.relative_to(ROOT).as_posix()
        if relative.startswith(GENERATED_TEXT_EXEMPT):
            continue
        try:
            body = path.read_text('utf-8')
        except (UnicodeDecodeError, ValueError):
            continue
        if relative.startswith(BENCHMARK_FILES):
            allowed = lambda number: True
        elif relative == 'README.md':
            section = benchmark_section(body)
            allowed = lambda number: number in section
        else:
            allowed = lambda number: False
        for line in banned_lines(body, allowed):
            errors.append(f'Banned word in {relative}:{line}')

    if errors:
        raise SystemExit('\n'.join(errors))
    print(f'Repository check passed: {links} local links, {len(manifest["files"])} brand assets, '
          'production hashes, no empty source directories and no banned words.')


if __name__ == '__main__':
    main()
