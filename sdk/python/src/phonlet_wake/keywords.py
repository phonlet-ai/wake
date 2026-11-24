"""Keyword files and phrases. Validation never enrolls text or changes thresholds.

A keyword file is what the threshold generator writes:

    {"model": {"id": "en-1", "sha256": "..."},     optional: the model the thresholds belong to
     "keywords": [phrase, ...],
     "metadata": {...}}                             optional, never read by the engine

`Wake.create(keywords=...)` takes that file as a path, as the parsed object, or just the list of
phrases. Each phrase carries its pronunciations and calibrated thresholds.
"""
import copy
import hashlib
import json
import math
import os

INVENTORY = "2017adc0505b3f530eef5b66a5b36019a434f04a4bd93a32b72f5070ee46baad"
FRONTEND = "3f047efa"
PHRASE_FIELDS = ("name", "pronunciations", "threshold_loose", "threshold_strict", "endpoint")


class WakeError(ValueError):
    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def fail(message, code="INVALID_ARGUMENT"):
    raise WakeError(code, message)


def fields(value, required, optional=(), label="object"):
    if not isinstance(value, dict):
        fail(f"{label} must be an object")
    missing = set(required) - value.keys()
    unknown = value.keys() - set(required) - set(optional)
    if missing or unknown:
        fail(f"{label}: missing {sorted(missing)}, unknown {sorted(unknown)}")


def sensitivity(value):
    if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value <= 1:
        fail("sensitivity must be a finite number in [0,1]")
    return float(value)


def integer(value):
    return type(value) in (int, float) and math.isfinite(value) and value == int(value)


def sha(value, label):
    if not isinstance(value, str) or len(value) != 64 or any(c not in "0123456789abcdef" for c in value):
        fail(f"invalid {label} SHA-256")


def read_keywords(keywords):
    """A keyword file as a dict, from a path, a parsed file or a list of phrases."""
    if isinstance(keywords, (str, os.PathLike)):
        try:
            with open(keywords, "rb") as handle:
                keywords = json.loads(handle.read(), parse_constant=lambda c: fail("invalid JSON constant " + c, "CORRUPT_ARTIFACT"))
        except OSError as error:
            raise WakeError("CORRUPT_ARTIFACT", str(error)) from error
        except (ValueError, UnicodeError) as error:
            if isinstance(error, WakeError):
                raise
            raise WakeError("CORRUPT_ARTIFACT", f"keyword file is not valid JSON: {error}") from error
    if isinstance(keywords, (list, tuple)):
        keywords = {"keywords": list(keywords)}
    fields(keywords, ("keywords",), ("model", "metadata"), "keyword file")
    if not isinstance(keywords["keywords"], list) or not keywords["keywords"]:
        fail("keywords must be a nonempty list of phrases")
    if "model" in keywords:
        fields(keywords["model"], ("id", "sha256"), (), "keyword file model")
        if not isinstance(keywords["model"]["id"], str) or not keywords["model"]["id"].strip():
            fail("keyword file model id must be a nonempty string")
        sha(keywords["model"]["sha256"], "model")
    if "metadata" in keywords and not isinstance(keywords["metadata"], dict):
        fail("keyword file metadata must be an object")
    return keywords


def validate_phrases(phrases):
    """The phrase layout shared by keyword files and phrases written in code."""
    if not isinstance(phrases, list) or not phrases:
        fail("keywords must be a nonempty list of phrases")
    seen = set()
    for kw in phrases:
        fields(kw, PHRASE_FIELDS, ("id", "metadata", "sensitivity"), "phrase")
        ident = kw.get("id", kw["name"])
        for label, value in (("name", kw["name"]), ("id", ident)):
            if not isinstance(value, str) or not value.strip():
                fail(f"phrase {label} must be a nonempty string")
        if ident in seen:
            fail(f"duplicate phrase: {ident}")
        seen.add(ident)
        if type(kw["endpoint"]) is not bool:
            fail("phrase endpoint must be true or false")
        if "metadata" in kw and not isinstance(kw["metadata"], dict):
            fail("phrase metadata must be an object")
        for key in ("threshold_loose", "threshold_strict"):
            if type(kw[key]) not in (int, float) or not math.isfinite(kw[key]):
                fail(f"{key} must be finite")
        if kw["threshold_loose"] > kw["threshold_strict"]:
            fail("threshold_loose must be <= threshold_strict")
        if not isinstance(kw["pronunciations"], list) or not kw["pronunciations"]:
            fail("phrase needs at least one pronunciation")
        for variant in kw["pronunciations"]:
            if not isinstance(variant, list) or not 1 <= len(variant) <= 256:
                fail("a pronunciation must contain 1–256 phone IDs")
            if any(not integer(p) or not 1 <= p < 40 for p in variant):
                fail("phone IDs must be integers in [1,39]")
    return phrases


def select_phrases(keywords, model_bytes, default=0.5):
    """Validated private copies of the phrases, each with its resolved sensitivity. A keyword
    file that names its model must name this one."""
    default = sensitivity(default)
    document = read_keywords(keywords)
    if "model" in document:
        actual = hashlib.sha256(model_bytes).hexdigest()
        if document["model"]["sha256"] != actual:
            fail(f"these keywords were made for model {document['model']['id']} "
                 f"({document['model']['sha256'][:12]}), not this model ({actual[:12]})", "INCOMPATIBLE_ARTIFACT")
    phrases = validate_phrases(copy.deepcopy(document["keywords"]))
    selected = []
    for kw in phrases:
        value = sensitivity(kw["sensitivity"]) if "sensitivity" in kw else default
        selected.append({"id": kw.get("id", kw["name"]), **{k: kw[k] for k in PHRASE_FIELDS},
                         "metadata": kw.get("metadata", {}), "sensitivity": value})
    return selected
