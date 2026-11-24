"""ctypes.CDLL releases the GIL for native block calls. No callbacks are used."""

import ctypes as C
import hashlib
import os
import sys
from functools import lru_cache
from pathlib import Path

import numpy as np

P, I, L = C.c_void_p, C.c_int, C.c_int64


@lru_cache(maxsize=1)
def library():
    configured = os.environ.get("KWS_ENGINE_LIBRARY")
    if configured:
        path = Path(configured).resolve()
    else:
        filename = "kws_engine.dll" if sys.platform == "win32" else "libkws_engine.dylib" if sys.platform == "darwin" else "libkws_engine.so"
        path = Path(__file__).resolve().parents[1] / "_native" / filename
        if not path.is_file():
            from .build import build_engine
            path = build_engine()
    lib = C.CDLL(str(path))
    declarations = {
        "abi_version": (C.c_uint32, []),
        "frontend_id": (C.c_uint32, []),
        "error": (C.c_char_p, []),
        "cpu_kernel": (C.c_char_p, []),
        "language": (C.c_char_p, [C.c_char_p]),
        "model_load": (P, [C.c_char_p, C.c_char_p]),
        "model_load_bytes": (P, [P, L, C.c_char_p]),
        "detector_create_per_keyword": (P, [C.c_char_p, P, L, I, I, C.c_char_p]),
        "session_create_per_keyword": (P, [P, C.c_char_p, P, L, I]),
        "model_destroy": (None, [P]),
        "frontend_create": (P, []),
        "frontend_destroy": (None, [P]),
        "frontend_reset": (I, [P]),
        "frontend_push": (L, [P, P, L, P, L]),
        "frontend_finish": (L, [P, I, P]),
        "detector_create": (P, [C.c_char_p, P, L, I, I, C.c_char_p]),
        "detector_destroy": (None, [P]),
        "detector_replay_scores": (I, [P, P, L, P]),
        "detector_push": (I, [P, P, L, P, P, P, P, P]),
        "session_create": (P, [P, C.c_char_p, P, L, I]),
        "session_destroy": (None, [P]),
        "session_finish": (L, [P]),
        "session_push": (L, [P, P, L, I, L, P, P, P, P, P, P]),
        "replay_scores": (I, [P, L, L, P, L, I, P]),
    }
    for name, (result, args) in declarations.items():
        fn = getattr(lib, "kws_" + name)
        fn.restype = result
        fn.argtypes = args
    if lib.kws_abi_version() != 1:
        raise RuntimeError("Unsupported kws native ABI")
    lib.identity = hashlib.sha256(path.read_bytes()).hexdigest()
    lib.path = path
    return lib


def checked(value):
    if value is None or value < 0:
        raise RuntimeError(library().kws_error().decode())
    return value


def ptr(array):
    return None if array is None else array.ctypes.data


def array(value, dtype, width=None):
    x = np.asarray(value)
    if x.dtype != np.dtype(dtype) or x.ndim != (1 if width is None else 2):
        raise ValueError(f"expected {dtype} " + ("vector" if width is None else f"[T,{width}]"))
    if width is not None and x.shape[1] != width:
        raise ValueError(f"expected {width} columns")
    return np.require(x, requirements=["C", "A"])


class Handle:
    _handle = None

    def _check(self):
        if not self._handle:
            raise RuntimeError(f"{type(self).__name__} is closed")

    def close(self):
        if self._handle:
            getattr(self._lib, self._destroy)(self._handle)
            self._handle = None

    def __enter__(self):
        self._check()
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        self.close()
