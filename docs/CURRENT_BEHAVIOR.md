# Current upstream behavior mapping

Embedded upstream: `Flowseal/zapret-discord-youtube` 1.10.1, imported from its
immutable release asset. Exact release id, commit, URL and archive SHA-256 are in
`resources/upstream/provenance.json`.

All matching `general*.bat` strategies are discovered dynamically. The importer
normalizes supported BAT variables into `%BIN%`, `%LISTS%`, `%USER_LISTS%`,
`%GAME_TCP%` and `%GAME_UDP%`, while preserving argument order, repeated
`--new`, explicit `=!` resets, filters, fake payloads and quoting semantics.
Unknown shell syntax or variables fail the import/build.

The catalog size is discovered from the embedded upstream files at build time.
Fidelity is verified independently for every discovered strategy in
off/all/TCP/UDP GameFilter modes (`N × 4` command lines); no strategy total is
maintained by hand.

Vendor `bin/` and `lists/` files are immutable runtime data and always hash
verified. User list additions live in `%ProgramData%\CHEBURNET\user\lists`, so
an engine update cannot overwrite them. CHEBURNET does not invoke upstream BAT
scripts or raw updater logic at runtime.
