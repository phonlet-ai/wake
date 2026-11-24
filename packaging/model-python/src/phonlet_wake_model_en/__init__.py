"""Explicit model assets; importing this package does not download or load an engine."""
from pathlib import Path
root = Path(__file__).resolve().parent
# The en-1 model and its English keyword file (which names the model by SHA-256).
english = {"model": root/"model.pwake", "keywords": root/"keywords.json"}
