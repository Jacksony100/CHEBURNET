# CHEBURNET architecture

CHEBURNET preserves a one-EXE user experience around Flowseal's distribution of
bol-van `winws.exe`. It does not reimplement the network engine.

## Build-time supply chain

`scripts/sync-upstream.ps1` selects an immutable, non-draft stable GitHub release
asset, validates its published SHA-256, required layout and tag commit, and works
inside an isolated temporary tree. The strict BAT importer rejects unsupported
syntax, generates a typed strategy catalog, and the resource generator records
every payload size/hash. Production inputs are replaced only after validation.

The latest imported provenance is embedded into About and Diagnostics.

## Runtime modules

- `main.cpp` performs DLL-search hardening, single-instance serialization,
  elevation confirmation and no-logger protected-tree bootstrap.
- `SecureFs` validates no-reparse path components, owner/protected DACL, hard
  links, random atomic files and descendant-only cleanup.
- `ResourceExtractor` verifies embedded immutable resources and external
  runtime manifests; persistent user overlays are separate.
- `Strategies` holds generated typed arguments, strict external catalog parsing
  and four TCP/UDP GameFilter modes.
- `ProcessManager` creates a suspended child with an explicit inherited-handle
  list, assigns a Job, verifies image/creation identity, stabilizes, writes the
  process record, and rolls back every partial failure.
- `UpdateManager` separates signed launcher discovery/download from payload
  package installation and versioned activation.
- `UiContext` owns and restores console codepages, modes, cursor, attributes,
  window/buffer and style state.

## Protected data layout

```text
%ProgramData%\CHEBURNET\
  runtime\<version>\bin|lists|strategies
  user\config.json
  user\lists\*.txt
  updates\
  logs\
  active-runtime.json
  winws-run.txt
```

SYSTEM and Administrators have full access; standard Users have read/execute.
Privileged writes use 128-bit random `CREATE_NEW` temporary names,
`FILE_FLAG_OPEN_REPARSE_POINT`, exact write/flush, mandatory ACL, hash/size
verification, atomic rename and post-rename reopen verification.

## Update state machines

Launcher updates authenticate metadata and artifact independently, enforce
anti-downgrade, then stage a verified EXE for explicit manual replacement.

Payload updates authenticate metadata, download a bounded `.cbpkg`, validate its
allowlisted table, extract into a new version directory, verify every entry and
strategy, then run preflight → trusted stop → transactional start → stabilization
and identity health → protected state commit. Failure restores the prior
known-good version. A `pending` candidate is never treated as current after a
crash.

See `docs/UPDATE_SECURITY.md` for trust and key-rotation details.
