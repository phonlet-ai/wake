"""Owned arrays and explicit execution provenance, independent of measurements."""

from dataclasses import dataclass, field

import numpy as np


@dataclass(frozen=True)
class OutputSpec:
    posteriors: bool = False
    scores: bool = False
    events: bool = True
    raw_scores: bool = False
    gates: bool = False


@dataclass
class Result:
    posteriors: np.ndarray | None = None
    scores: np.ndarray | None = None
    raw_scores: np.ndarray | None = None
    fires: np.ndarray | None = None
    gates: np.ndarray | None = None
    reset_frames: np.ndarray = field(default_factory=lambda: np.empty(0, dtype=np.int64))
    frame_offset: int = 0
    frames: int = 0
    metadata: dict = field(default_factory=dict)

    @property
    def events(self):
        if self.fires is None:
            return []
        return [
            {
                "frame": int(t) + self.frame_offset,
                "time": (int(t) + self.frame_offset + 1) * 0.032,
                "sensitivity": self.metadata["sensitivities"][int(k) if self.metadata.get("sensitivity_layout") == "per_keyword" else int(s)],
                "keyword": int(k),
            }
            for t, s, k in np.argwhere(self.fires)
        ]
