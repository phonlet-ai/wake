import numpy as np

from ._ffi import Handle, array, checked, library, ptr
from .artifacts import OutputSpec, Result
from .detector import SENSITIVITIES, buffers, enrollment_args


class Session(Handle):
    """Each push returns owned arrays; finish drops incomplete PCM, never pads EOF.

    Use independent Sessions across threads. A Session is not reentrant. Cached
    mel cannot reproduce frontend resets and is labelled separately from PCM.
    """

    _destroy = "kws_session_destroy"

    def __init__(
        self, model, *, keywords=None, sensitivities=SENSITIVITIES, keyword_sensitivities=None, outputs=None, mode="independent"
    ):
        model._check()
        self._lib = library()
        self.model = model
        self.keywords = list(keywords or [])
        for keyword in self.keywords:
            model.language.validate_enrollment(keyword)
        self.outputs = outputs or OutputSpec(
            posteriors=not self.keywords, events=bool(self.keywords)
        )
        if not self.keywords and any(
            [self.outputs.scores, self.outputs.raw_scores, self.outputs.gates]
        ):
            raise ValueError("detector outputs require keywords")
        self.per_keyword = keyword_sensitivities is not None
        if self.per_keyword:
            if tuple(sensitivities) != SENSITIVITIES:
                raise ValueError("grid and per-keyword sensitivity are mutually exclusive")
            from ..keywords import sensitivity
            sensitivities = [sensitivity(s) for s in keyword_sensitivities]
            if len(sensitivities) != len(self.keywords):
                raise ValueError("per-keyword sensitivity count must match keywords")
        encoded, self.sensitivities, self.mode = enrollment_args(self.keywords, sensitivities, mode)
        self.sensitivity_rows = 1 if self.per_keyword else len(self.sensitivities)
        self._handle = checked(
            (self._lib.kws_session_create_per_keyword if self.per_keyword else self._lib.kws_session_create)(
                model._handle, encoded, ptr(self.sensitivities), len(self.sensitivities), self.mode
            )
        )
        self.offset = 0
        self.pending = 0
        self.finished = False
        self.input_mode = None

    def _push(self, value, melmode):
        self._check()
        if self.finished:
            raise RuntimeError("session finished")
        x = array(value, np.int16, 80 if melmode else None)
        n = len(x) if melmode else (self.pending + len(x)) // 512
        spec = self.outputs
        if not self.keywords:
            spec = OutputSpec(posteriors=spec.posteriors, events=False)
        out = buffers(n, len(self.keywords), self.sensitivity_rows, spec, self.model.n_classes)
        resets = np.empty(n, np.uint8)
        count = checked(
            self._lib.kws_session_push(
                self._handle,
                ptr(x),
                len(x),
                melmode,
                n,
                ptr(out["posteriors"]),
                ptr(resets),
                ptr(out["scores"]),
                ptr(out["raw_scores"]),
                ptr(out["fires"]),
                ptr(out["gates"]),
            )
        )
        assert count == n
        if not melmode:
            self.pending = (self.pending + len(x)) % 512
        self.input_mode = "cached_mel" if melmode else "pcm"
        result = Result(
            **out,
            reset_frames=np.flatnonzero(resets) + self.offset,
            frame_offset=self.offset,
            frames=n,
            metadata={
                "runtime": self._lib.identity,
                "frontend_id": f"{self._lib.kws_frontend_id():08x}",
                "model": self.model.identity,
                "lang": self.model.language.code,
                "inventory_id": self.model.language.inventory_id,
                "n_classes": self.model.n_classes,
                "input_mode": self.input_mode,
                "cpu_kernel": self._lib.kws_cpu_kernel().decode(),
                "finalization": "complete_model_frames_no_eof_padding",
                "reset_policy": "1876_consecutive_high_blank",
                "sensitivities": self.sensitivities.tolist(),
                "sensitivity_layout": "per_keyword" if self.per_keyword else "grid",
                "keyword_mode": "independent" if self.mode == 0 else "production_arbitrated",
            },
        )
        self.offset += n
        return result

    def push_pcm(self, pcm):
        return self._push(pcm, False)

    def push_mel(self, mel):
        return self._push(mel, True)

    def finish(self, pad=False):
        self._check()
        if pad:
            raise ValueError("deployment sessions never append synthetic EOF silence")
        discarded = checked(self._lib.kws_session_finish(self._handle))
        self.finished = True
        return {
            "frames": self.offset,
            "discarded_samples": discarded,
            "input_mode": self.input_mode,
        }

    def run_pcm(self, pcm):
        if self.offset or self.pending or self.finished:
            raise RuntimeError("run_pcm requires a fresh session; use push_pcm to continue")
        result = self.push_pcm(pcm)
        result.metadata.update(self.finish())
        return result

    def run_mel(self, mel):
        if self.offset or self.pending or self.finished:
            raise RuntimeError("run_mel requires a fresh session; use push_mel to continue")
        result = self.push_mel(mel)
        result.metadata.update(self.finish())
        return result
