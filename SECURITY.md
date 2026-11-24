# Security policy

## Reporting a vulnerability

Report security issues privately to contact@phonletai.com. Do not open a
public issue for security problems. Include reproduction steps and affected
versions; we will acknowledge reports and coordinate fixes before any public
disclosure.

## Scope

- The native engine and its WASM build, all SDK bindings, the packaging
  scripts and the shipped model/keyword artifacts.
- Audio is processed locally; the engine performs no network access. Reports
  of unexpected outbound traffic are in scope.
- Malformed model/keyword artifacts must fail validation cleanly (the SDKs
  verify hashes and schema before processing); crashes or memory-safety
  issues in artifact parsing are in scope.

## Non-goals

Deliberate false activations of the wake-word model (adversarial audio) are
an accuracy research topic, not a vulnerability, unless combined with a code
defect.
