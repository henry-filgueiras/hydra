# Working guidelines for this repo

## Per-exchange checklist
An "exchange" is one user request, start to finish. Before declaring it done:

1. Update `DIRECTORS_NOTES.md` (see below).
2. Commit the work + notes update together. Smaller intermediate commits are fine if each has a meaningful message.
3. Do **not** `git push`.

## DIRECTORS_NOTES.md
Living design doc at repo root, two sections:

**Current Canon** — architecture, invariants, present-state truth. Edit in place to keep it reconciled with reality. When a fact stops being true, move the old text **verbatim** into the archive below instead of editing or deleting it in place.

**Resolved Dragons and Pivots** — append-only devlog of discoveries, fixes, pivots, and entries demoted from Canon. Never edit past entries.

### Entry format
Prefix each new entry with ISO date and AI friendly name, e.g. `2026-04-17 — Claude Opus 4.7`. If no friendly-name mapping is available, use the model ID (e.g. `claude-opus-4-7`).
