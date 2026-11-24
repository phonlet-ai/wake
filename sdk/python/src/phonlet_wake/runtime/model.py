import hashlib
from pathlib import Path

import numpy as np

from ._ffi import Handle, array, checked, library, ptr
from .language import language


class Model(Handle):
    """Immutable native weights. Sessions retain weights even after model.close()."""

    _destroy = "kws_model_destroy"

    @classmethod
    def from_file(cls, path, *, lang="en"):
        return cls.from_bytes(Path(path).read_bytes(), lang=lang)

    @classmethod
    def from_bytes(cls, raw, *, lang="en"):
        preset = language(lang)
        raw = bytes(raw)
        obj = cls()
        obj._lib = library()
        obj._handle = checked(
            obj._lib.kws_model_load_bytes(np.frombuffer(raw, np.uint8).ctypes.data, len(raw), preset.code.encode())
        )
        obj.identity = hashlib.sha256(raw + preset.inventory_id.encode()).hexdigest()
        obj.language = preset
        obj.n_classes = preset.n_classes
        obj.metadata = {
            "source": "container",
            "container_sha256": hashlib.sha256(raw).hexdigest(),
            "model_id": obj.identity,
            "lang": preset.code,
            "inventory_id": preset.inventory_id,
        }
        return obj
