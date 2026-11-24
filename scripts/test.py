"""Single local entry point for the complete verification suite.

Stages (run in order; the first failure stops the suite):

  gate        repository layout, links, asset hashes, banned-terms scan
  tests       Python engine/SDK regressions (pytest)
  web-smoke   WASM scalar and SIMD model/streaming parity (node)
  package     build all release archives (native + WASM + packages)
  install     verify archives from independent consumer directories

Usage:
  python scripts/test.py             # full suite
  python scripts/test.py --quick     # gate + tests + web-smoke only
  python scripts/test.py --stage package          # one stage
  python scripts/test.py --skip-wasm              # reuse built WASM outputs

The package stage requires the git worktree to have no untracked files
(stage or ignore them first) and a pinned-Emscripten checkout unless
--skip-wasm is given. CI workflows run exactly these commands.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PYTHON = sys.executable
NODE = shutil.which('node')


def stage_gate(skip_wasm=False):
    subprocess.run([PYTHON, ROOT / 'scripts/check-repository.py'], check=True)


def stage_tests(skip_wasm=False):
    subprocess.run([PYTHON, '-m', 'pytest', '-q'], cwd=ROOT, check=True)


def stage_web_smoke(skip_wasm=False):
    if NODE is None:
        raise RuntimeError('node is required for the web smoke stage')
    for extra in ([], ['--simd']):
        subprocess.run([NODE, ROOT / 'sdk/web/test/smoke.mjs', *extra], check=True)


def stage_react(skip_wasm=False):
    if NODE is None:
        raise RuntimeError('node is required for the react stage')
    react = ROOT / 'sdk/react'
    if not (react / 'node_modules/react/package.json').is_file():
        print('react adapter tests: dev dependencies not installed '
              '(bash sdk/react/test/install-deps.sh) — skipping')
        return
    subprocess.run([NODE, '--test', 'test/react.test.mjs'], cwd=react, check=True)


def stage_java(skip_wasm=False):
    import os
    javac = shutil.which('javac')
    jdk = os.environ.get('JAVA_HOME') or (str(Path(javac).resolve().parents[1]) if javac else None)
    if not jdk or not (Path(jdk) / 'bin/javac').is_file():
        print('java SDK tests: no JDK with javac found (set JAVA_HOME) — skipping')
        return
    if not (ROOT / 'build/native/libkws_engine.so').is_file():
        print('java SDK tests: native engine not built yet — skipping')
        return
    subprocess.run(['bash', ROOT / 'sdk/java/build.sh'], check=True, env={**os.environ, 'JAVA_HOME': jdk})


def stage_package(skip_wasm=False):
    command = [PYTHON, ROOT / 'scripts/package.py']
    if skip_wasm:
        command.append('--skip-wasm')
    subprocess.run(command, check=True)


def stage_install(skip_wasm=False):
    subprocess.run([PYTHON, ROOT / 'scripts/verify-install.py', '--types'], check=True)


STAGES = [
    ('gate', stage_gate),
    ('tests', stage_tests),
    ('web-smoke', stage_web_smoke),
    ('react', stage_react),
    ('java', stage_java),
    ('package', stage_package),
    ('install', stage_install),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quick', action='store_true',
                        help='gate + tests + web-smoke + react only (no packaging/install)')
    parser.add_argument('--stage', choices=[name for name, _ in STAGES],
                        help='run a single stage')
    parser.add_argument('--skip-wasm', action='store_true',
                        help='pass through to packaging: reuse built WASM outputs')
    args = parser.parse_args()

    selected = [name for name, _ in STAGES]
    if args.stage:
        selected = [args.stage]
    elif args.quick:
        selected = ['gate', 'tests', 'web-smoke', 'react']

    failures = []
    for name, function in STAGES:
        if name not in selected:
            continue
        started = time.monotonic()
        print(f'==> {name}', flush=True)
        try:
            function(skip_wasm=args.skip_wasm)
        except subprocess.CalledProcessError as error:
            failures.append((name, error.returncode))
            print(f'==> {name} FAILED (exit {error.returncode}) after '
                  f'{time.monotonic() - started:.1f}s', flush=True)
            break
        else:
            print(f'==> {name} passed ({time.monotonic() - started:.1f}s)', flush=True)

    if failures:
        raise SystemExit(f'suite failed at: {failures[0][0]}')
    print(f'suite passed: {", ".join(selected)}')


if __name__ == '__main__':
    main()
