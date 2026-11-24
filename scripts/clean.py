"""Preview removal of reproducible build intermediates; --apply performs it.

Release archives in dist/, tracked Web runtimes and all model/source assets remain.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
BUILD_DIRS = (
    'build', 'engine/build', 'engine/wasm/build', 'sdk/python/build',
    'sdk/python/dist', 'packaging/model-python/build', 'demos/web/dist', '.pytest_cache',
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apply', action='store_true', help='Remove the listed generated directories.')
    args = parser.parse_args()
    targets = {ROOT / name for name in BUILD_DIRS if (ROOT / name).exists()}
    for current, directories, _ in os.walk(ROOT):
        base = Path(current)
        for name in list(directories):
            path = base / name
            if path in targets or name in {'.git', '.venv', 'node_modules', 'dist'}:
                directories.remove(name)
            elif name in {'__pycache__', '.pytest_cache'} or name.endswith('.egg-info'):
                targets.add(path)
                directories.remove(name)
    for path in sorted(targets):
        if path.is_symlink() or not path.resolve().is_relative_to(ROOT):
            raise SystemExit(f'Refusing linked/outside directory: {path}')
        if (ROOT / '.git').exists():
            tracked = subprocess.check_output(['git', 'ls-files', '--', str(path)], cwd=ROOT)
            if tracked:
                raise SystemExit(f'Refusing to remove tracked files under {path}')
    for path in sorted(targets):
        print(('Remove ' if args.apply else 'Would remove ') + str(path.relative_to(ROOT)))
        if args.apply:
            shutil.rmtree(path)
    print('Release archives, tracked WASM, source and model assets preserved.')
    if not args.apply:
        print('Dry run only. Add --apply to remove these generated intermediates.')


if __name__ == '__main__':
    main()
