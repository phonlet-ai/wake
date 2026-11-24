"""Language presets are supplied by the native library, including English defaults."""

import json
from dataclasses import dataclass
from functools import cache

from ._ffi import library


@dataclass(frozen=True)
class Language:
    code: str
    symbols: tuple[str, ...]
    inventory_id: str
    blank: int = 0

    @property
    def n_classes(self):
        return len(self.symbols)

    def validate_enrollment(self, keyword):
        if "lang" in keyword and keyword["lang"].lower() != self.code:
            raise ValueError("enrollment language differs from selected preset")
        if "inventory_id" in keyword and keyword["inventory_id"] != self.inventory_id:
            raise ValueError("enrollment phone inventory mismatch")


@cache
def language(code="en"):
    value = library().kws_language(code.encode())
    if value is None:
        raise ValueError(library().kws_error().decode())
    data = json.loads(value)
    return Language(data["code"], tuple(data["symbols"]), data["inventory_id"], data["blank"])
