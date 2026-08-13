# Changelog

## 1.0.0-rc.1 — 2026-08-13

- Imported immutable Flowseal `zapret-discord-youtube` 1.10.1 release artifact.
- Replaced the fixed strategy table with strict dynamic discovery and
  four-mode GameFilter fidelity verification.
- Closed public-release P1/P2 filesystem, logging, extraction, ACL, process,
  stale-PID, console-state and JSON-parser findings.
- Added signed fail-closed launcher/payload update infrastructure, protected
  staging, allowlisted packages, versioned activation and rollback.
- Split immutable runtime data from persistent user configuration/list overlays.
- Added public licenses, security/privacy/update documentation, CI and release
  workflows.
- Fixed protected-file ACL verification on NTFS: regular-file ACEs are now
  validated without directory-only inheritance flags. This restores secure
  logger/state/process-record writes while preserving the exact protected DACL.
- Early startup and CLI messages now use native UTF-16 console output, avoiding
  question-mark mojibake under the default Windows CRT locale.
