# Contributing

Use a focused branch and keep changes reviewable. Never commit release private
keys, credentials, generated secrets, user logs, or arbitrary upstream binary
snapshots.

Before submitting a change:

```powershell
scripts\build-release.ps1 -BuildDir build
scripts\run-tests.ps1 -BuildDir build
scripts\validate-licenses.ps1
scripts\package.ps1 -BuildDir build -OutDir dist-test -SkipBuild
```

Upstream changes must go through `scripts\sync-upstream.ps1`, which imports an
immutable stable release asset into isolation and rejects unsupported BAT
syntax. Review the provenance, binary diff, strategy catalog, licensing and
fidelity output. Do not auto-merge an upstream update.

New C++ must remain C++20, dependency-light and clean under `/W4 /WX`. Privileged
filesystem/network/update code must fail closed and include negative regression
tests. Avoid shelling out at runtime; the launcher updater uses native WinHTTP
and Windows CNG.
