"""Small deployment facade; all numerical detection runs in the owned engine."""
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
from types import MappingProxyType
from .keywords import WakeError, FRONTEND, INVENTORY, select_phrases
from .runtime import Model, Session, OutputSpec
from .runtime._ffi import library


@dataclass(frozen=True)
class Detection:
    keyword_id: str
    keyword: str
    keyword_index: int
    frame_index: int
    sample_index: int
    time_seconds: float
    sensitivity: float


def _model_bytes(model):
    """The model container from a path or bytes."""
    if isinstance(model, (bytes, bytearray, memoryview)):
        return bytes(model)
    try:
        return Path(model).read_bytes()
    except TypeError as error:
        raise WakeError("INVALID_ARGUMENT", "model must be a path or bytes") from error
    except OSError as error:
        raise WakeError("CORRUPT_ARTIFACT", str(error)) from error


class Wake:
    sample_rate = 16000
    frame_length = 512

    @classmethod
    def create(cls, *, model, keywords, sensitivity=0.5, keyword_mode="production_arbitrated"):
        """A model (path or bytes) and its phrases: a keyword file path, the parsed keyword file,
        or a list of phrases written in code. Each phrase carries its own pronunciations and
        calibrated thresholds; a keyword file that names its model must name this one."""
        if keyword_mode not in ("production_arbitrated", "independent"):
            raise WakeError("INVALID_ARGUMENT", "invalid keyword_mode")
        model_bytes = _model_bytes(model)
        selected = select_phrases(keywords, model_bytes, sensitivity)
        canonical = json.dumps([{k: v for k, v in kw.items() if k != "sensitivity"} for kw in selected], sort_keys=True)
        keywords_sha256 = hashlib.sha256(canonical.encode()).hexdigest()
        lib = library()
        if f"{lib.kws_frontend_id():08x}" != FRONTEND:
            raise WakeError("INCOMPATIBLE_ARTIFACT", "engine frontend mismatch")
        model = Model.from_bytes(model_bytes)
        try:
            if model.language.inventory_id != INVENTORY:
                raise WakeError("INCOMPATIBLE_ARTIFACT", "engine inventory mismatch")
            session = Session(model, keywords=selected, keyword_sensitivities=[k["sensitivity"] for k in selected],
                              mode=keyword_mode, outputs=OutputSpec(events=True))
        except BaseException:
            model.close()
            raise
        obj = cls()
        obj._session, obj._model = session, model
        obj._selected = tuple(MappingProxyType({"id": k["id"], "name": k["name"], "sensitivity": k["sensitivity"]}) for k in selected)
        obj._state, obj._final = "ready", None
        obj._info = MappingProxyType({"sdk_version": "0.1.0", "engine_abi": 1, "backend": "native",
            "engine_sha256": lib.identity, "frontend_id": FRONTEND, "inventory_id": INVENTORY,
            "model_sha256": model.metadata["container_sha256"],
            "keywords_sha256": keywords_sha256,
            "keyword_mode": keyword_mode, "scorer": "production", "scorer_options": ""})
        return obj

    @property
    def state(self): return self._state
    @property
    def keywords(self): return self._selected
    @property
    def info(self): return self._info

    def process(self, pcm):
        if self._state != "ready": raise WakeError("INVALID_STATE", f"Wake is {self._state}")
        try:
            result = self._session.push_pcm(pcm)
        except ValueError as error:
            raise WakeError("INVALID_ARGUMENT", str(error)) from error
        except RuntimeError as error:
            self.close()
            raise WakeError("ENGINE_FAILURE", str(error)) from error
        return [Detection(self._selected[e["keyword"]]["id"], self._selected[e["keyword"]]["name"],
            e["keyword"], e["frame"], (e["frame"]+1)*512, (e["frame"]+1)*512/16000, e["sensitivity"])
            for e in result.events]

    def finish(self):
        if self._state == "closed": raise WakeError("INVALID_STATE", "Wake is closed")
        if self._final is None:
            result = self._session.finish()
            self._final = MappingProxyType({"frames": result["frames"], "discarded_samples": result["discarded_samples"]})
            self._state = "finished"
        return self._final

    def close(self):
        if self._state != "closed":
            self._session.close()
            self._model.close()
            self._state = "closed"

    def __enter__(self):
        if self._state == "closed": raise WakeError("INVALID_STATE", "Wake is closed")
        return self

    def __exit__(self, *_): self.close()
