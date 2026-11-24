#!/usr/bin/env bash
# Dev-only React/jsdom deps for the adapter lifecycle tests. Not shipped.
# The private web-SDK dependency is satisfied from the locally built tarball
# (python scripts/package.py builds it into dist/).
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
web="$root/dist/phonlet-ai-wake-web-0.1.0.tgz"
if [ ! -f "$web" ]; then echo "missing $web — run: python scripts/package.py" >&2; exit 1; fi
cd "$(dirname "$0")/.."
npm install --no-audit --no-fund --no-save react@19 react-dom@19 jsdom "@phonlet-ai/wake-web@file:$web"
