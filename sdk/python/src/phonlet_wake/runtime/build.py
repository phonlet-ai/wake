"""Content-addressed native builds; never replaces the pinned build-prod output."""

from __future__ import annotations

import hashlib
import json
import runpy
import subprocess
import tempfile
from pathlib import Path

FLAGS = ["-O3", "-std=c++17", "-fPIC", "-ffp-contract=off", "-fno-fast-math", "-pthread"]


def build_engine(force=False, root=None) -> Path:
    root = Path(root) if root is not None else (Path(__file__).resolve().parents[3] / "_engine" if (Path(__file__).resolve().parents[3] / "_engine").is_dir() else Path(__file__).resolve().parents[5] / "engine")
    if not root.is_dir():
        raise RuntimeError(
            "Install libkws_engine and set KWS_ENGINE_LIBRARY, or build from a source checkout"
        )
    sources = [root / name for name in (root / "sources.txt").read_text().splitlines() if name]
    headers = sorted(
        p for d in ["include", "src"] for p in (root / d).rglob("*") if p.suffix in {".h", ".hpp"}
    ) + sorted(p for p in (root.parent / "include").glob("*.h"))
    inventories = sorted((root / "inventories").glob("*.json"))
    generator = root / "tools/generate_languages.py"
    compiler = subprocess.check_output(["g++", "--version"], text=True).splitlines()[0]
    record = {
        "compiler": compiler,
        "flags": FLAGS,
        "sources": {
            str(p.relative_to(root.parent)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sources
            + headers
            + inventories
            + [generator, root / "sources.txt"]
        },
    }
    identity = hashlib.sha256(json.dumps(record, sort_keys=True).encode()).hexdigest()
    output = root / "build" / identity[:20]
    library = output / "libkws_engine.so"
    if not force and library.is_file() and (output / "build.json").is_file():
        meta = json.loads((output / "build.json").read_text())
        if meta.get("library_sha256") == hashlib.sha256(library.read_bytes()).hexdigest():
            return library
    output.mkdir(parents=True, exist_ok=True)
    import fcntl

    with (output / ".lock").open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        includes = ["-I" + str(root / d) for d in ["include/kws/detail", "include", "src/session"]]
        includes.append("-I" + str(root.parent / "include"))
        with tempfile.TemporaryDirectory(dir=output) as temporary:
            tmp = Path(temporary)
            runpy.run_path(str(generator))["generate"](
                root / "inventories", tmp / "languages.generated.h"
            )
            build_includes = [*includes, "-I" + str(tmp)]
            subprocess.run(
                [
                    "g++",
                    *FLAGS,
                    *build_includes,
                    "-shared",
                    *map(str, sources),
                    "-Wl,-soname,libkws_engine.so",
                    "-o",
                    str(tmp / library.name),
                ],
                check=True,
            )
            (tmp / library.name).replace(library)
            record.update(
                build_id=identity,
                abi=1,
                model_kernel="runtime-selectable portable/avx2; portable default",
                library_sha256=hashlib.sha256(library.read_bytes()).hexdigest(),
            )
            pending = tmp / "build.json"
            pending.write_text(json.dumps(record, indent=2) + "\n")
            pending.replace(output / "build.json")
    return library
