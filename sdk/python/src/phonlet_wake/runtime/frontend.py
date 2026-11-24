import numpy as np

from ._ffi import Handle, array, checked, library, ptr


class Frontend(Handle):
    """Stateful native 256-sample hops, no model or training dependencies."""

    _destroy = "kws_frontend_destroy"

    def __init__(self):
        self._lib = library()
        self._handle = checked(self._lib.kws_frontend_create())
        self._pending = 0

    @staticmethod
    def identity():
        return library().kws_frontend_id()

    def reset(self):
        self._check()
        checked(self._lib.kws_frontend_reset(self._handle))
        self._pending = 0

    def push(self, pcm):
        self._check()
        x = array(pcm, np.int16)
        n = (self._pending + len(x)) // 256
        out = np.empty((n, 40), np.int16)
        checked(self._lib.kws_frontend_push(self._handle, ptr(x), len(x), ptr(out), n))
        self._pending = (self._pending + len(x)) % 256
        return out

    def finish(self, finalize="complete_hops"):
        self._check()
        if finalize not in {"complete_hops", "training_nearest_hop"}:
            raise ValueError("unknown frontend finalization policy")
        out = np.empty((1, 40), np.int16)
        n = checked(
            self._lib.kws_frontend_finish(
                self._handle, finalize == "training_nearest_hop", ptr(out)
            )
        )
        self._pending = 0
        return out[:n]

    @classmethod
    def extract(cls, pcm, finalize="training_nearest_hop"):
        with cls() as stream:
            a = stream.push(pcm)
            b = stream.finish(finalize)
            return np.concatenate([a, b]) if len(b) else a
