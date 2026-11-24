"""Python helpers for engine-multi additions (use with KWS_ENGINE_LIBRARY set to
the engine-multi build; phonlet_wake.runtime covers everything else).

    batch_run(model, pcms, keywords, q31=True) -> per stream dict(scores, q31, resets)
    model_info(model) -> dict
    scorers() -> list
"""
import ctypes as C
import json

import numpy as np

from phonlet_wake.runtime._ffi import checked, library


def _lib():
    lib = library()
    if not hasattr(lib, "_multi"):
        lib.kws_batch_run.restype = C.c_int
        lib.kws_batch_run.argtypes = [C.c_void_p, C.c_int, C.c_void_p, C.c_void_p, C.c_char_p,
                                      C.c_void_p, C.c_int64, C.c_int, C.c_void_p, C.c_void_p,
                                      C.c_void_p]
        lib.kws_model_info.restype = C.c_char_p
        lib.kws_model_info.argtypes = [C.c_void_p]
        lib.kws_scorers.restype = C.c_char_p
        lib._multi = True
    return lib


def model_info(model):
    return json.loads(_lib().kws_model_info(model._handle).decode())


def scorers():
    return json.loads(_lib().kws_scorers().decode())


def batch_run(model, pcms, keywords=None, q31=False, scores=True, sensitivities=(0.0,), mode=0):
    """Run whole int16 PCM streams in lockstep. keywords: enrollment dicts."""
    lib = _lib()
    k = len(pcms)
    pcms = [np.ascontiguousarray(p, np.int16) for p in pcms]
    frames = [len(p) // 512 for p in pcms]
    ncls = model.n_classes
    out = []
    for n in frames:
        out.append({"q31": np.empty((n, ncls), np.int32) if q31 else None,
                    "scores": np.empty((n, len(keywords)), np.int16) if (keywords and scores) else None,
                    "resets": np.zeros(n, np.uint8)})
    P = lambda arrs: (C.c_void_p * k)(*[a.ctypes.data if a is not None else None for a in arrs])
    pcm_p = P(pcms)
    samples = (C.c_int64 * k)(*[len(p) for p in pcms])
    sens = np.ascontiguousarray(sensitivities, np.float64)
    enroll = json.dumps(keywords).encode() if keywords else None
    checked(lib.kws_batch_run(model._handle, k, pcm_p, samples, enroll, sens.ctypes.data, len(sens),
                              mode, P([o["q31"] for o in out]), P([o["scores"] for o in out]),
                              P([o["resets"] for o in out])))
    return out
