import json

import numpy as np

from ._ffi import Handle, array, checked, library, ptr
from .artifacts import OutputSpec, Result
from .language import language

SENSITIVITIES = (0.0, 0.25, 0.5, 0.75, 1.0)
MODES = {"independent": 0, "production_arbitrated": 1}


def enrollment_args(keywords, sensitivities, mode):
    if mode not in MODES:
        raise ValueError("mode must be independent or production_arbitrated")
    sens = np.asarray(sensitivities, dtype=np.float64)
    if (
        sens.ndim != 1
        or not len(sens)
        or not np.isfinite(sens).all()
        or np.any((sens < 0) | (sens > 1))
    ):
        raise ValueError("sensitivities must be a nonempty vector in [0,1]")
    return (
        json.dumps(keywords, allow_nan=False).encode() if keywords else None,
        np.ascontiguousarray(sens),
        MODES[mode],
    )


def buffers(n, k, s, outputs, n_classes=40):
    return {
        "posteriors": np.empty((n, n_classes), np.int32) if outputs.posteriors else None,
        "scores": np.empty((n, k), np.int16) if outputs.scores else None,
        "raw_scores": np.empty((n, k), np.int16) if outputs.raw_scores else None,
        "fires": np.empty((n, s, k), np.uint8) if outputs.events else None,
        "gates": np.empty(n, np.uint8) if outputs.gates else None,
    }


class Detector(Handle):
    _destroy = "kws_detector_destroy"

    def __init__(
        self,
        enrollment,
        *,
        sensitivities=SENSITIVITIES,
        keyword_sensitivities=None,
        mode="independent",
        outputs=None,
        lang="en",
        n_classes=None,
        inventory_id=None,
    ):
        self.language = language(lang)
        self.n_classes = n_classes = self.language.n_classes if n_classes is None else n_classes
        self.inventory_id = inventory_id = inventory_id or self.language.inventory_id
        if not isinstance(n_classes, int) or n_classes < 2:
            raise ValueError("invalid posterior class count")
        self.keywords = [enrollment] if isinstance(enrollment, dict) else list(enrollment)
        for keyword in self.keywords:
            if inventory_id == self.language.inventory_id:
                self.language.validate_enrollment(keyword)
            declared = keyword.get("inventory_id")
            if declared is not None and declared != inventory_id:
                raise ValueError("enrollment phone inventory mismatch")
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
        self._lib = library()
        self._handle = checked(
            (self._lib.kws_detector_create_per_keyword if self.per_keyword else self._lib.kws_detector_create)(
                encoded,
                ptr(self.sensitivities),
                len(self.sensitivities),
                self.mode,
                self.n_classes,
                self.language.code.encode()
                if self.inventory_id == self.language.inventory_id
                else None,
            )
        )
        self.outputs = outputs or OutputSpec(scores=True, events=True)
        self.offset = 0

    def push_posteriors(self, q31, *, reset_frames=None):
        self._check()
        q = array(q31, np.int32, self.n_classes)
        n = len(q)
        resets = None
        if reset_frames is not None:
            indices = np.asarray(reset_frames, dtype=np.int64) - self.offset
            if np.any((indices < 0) | (indices >= n)) or len(np.unique(indices)) != len(indices):
                raise ValueError("reset frames must be unique absolute indices in this block")
            resets = np.zeros(n, np.uint8)
            resets[indices] = 1
        out = buffers(n, len(self.keywords), self.sensitivity_rows, self.outputs, self.n_classes)
        checked(
            self._lib.kws_detector_push(
                self._handle,
                ptr(q),
                n,
                ptr(resets),
                ptr(out["scores"]),
                ptr(out["raw_scores"]),
                ptr(out["fires"]),
                ptr(out["gates"]),
            )
        )
        out["posteriors"] = q.copy() if self.outputs.posteriors else None
        result = Result(
            **out,
            frame_offset=self.offset,
            frames=n,
            reset_frames=np.asarray(reset_frames if reset_frames is not None else [], np.int64),
            metadata={
                "sensitivities": self.sensitivities.tolist(),
                "sensitivity_layout": "per_keyword" if self.per_keyword else "grid",
                "runtime": self._lib.identity,
                "input_mode": "posteriors",
                "n_classes": self.n_classes,
                "lang": self.language.code,
                "inventory_id": self.inventory_id,
            },
        )
        self.offset += n
        return result

    def run_posteriors(self, q31, *, reset_frames=None):
        # A run starts a new file; push_posteriors explicitly continues state.
        with Detector(
            self.keywords,
            sensitivities=SENSITIVITIES if self.per_keyword else self.sensitivities,
            keyword_sensitivities=self.sensitivities if self.per_keyword else None,
            mode="independent" if self.mode == 0 else "production_arbitrated",
            outputs=self.outputs,
            lang=self.language.code,
            n_classes=self.n_classes,
            inventory_id=self.inventory_id,
        ) as stream:
            return stream.push_posteriors(q31, reset_frames=reset_frames)

    def replay_scores(self, scores):
        self._check()
        scores = array(scores, np.int16, len(self.keywords))
        out = np.empty((len(scores), self.sensitivity_rows, len(self.keywords)), np.uint8)
        checked(
            self._lib.kws_detector_replay_scores(self._handle, ptr(scores), len(scores), ptr(out))
        )
        return Result(
            scores=scores,
            fires=out,
            frames=len(scores),
            metadata={"sensitivities": self.sensitivities.tolist(), "sensitivity_layout": "per_keyword" if self.per_keyword else "grid"},
        )
