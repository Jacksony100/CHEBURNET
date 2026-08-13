# Public RC smoke test

Run on clean, snapshotted Windows 10 x64 and Windows 11 x64 VMs. Use the exact
release artifact/hash and record OS build, console host, result and log path for
every item. Do not run destructive network tests on a development host.

## Install, UI and runtime

1. Verify `CHEBURNET.exe.sha256`; start from a path containing Cyrillic/spaces.
2. Reject UAC: launcher exits cleanly without creating a weak ProgramData tree.
3. Accept UAC: protected bootstrap/logging succeeds; first embedded extraction
   reports all hashes/ACLs valid.
4. Check branding, mascot, Unicode and `--ascii-only`; resize/minimize; verify
   console input/output codepages, modes, cursor, attributes and window style are
   restored on exit.
5. Connect with `general`; confirm 100% only after winws stabilization and pid /
   creation time / canonical image validation. Disconnect.
6. Select several dynamically listed strategies and test GameFilter off, all,
   TCP and UDP. Confirm user config persists across restart.
7. Add harmless entries to each `user\lists` overlay; restart/update and confirm
   they remain while vendor lists are restored if modified.
8. Exercise diagnostics, local logs and About provenance/credits. Confirm no
   sensitive environment, full winws argv or packet data is logged.

## Conflict and recovery

9. Leave a valid CHEBURNET winws running, restart launcher and verify ownership
   recognition. Create a stale pid record and verify it is not exempted.
10. Run a foreign `winws.exe` and an active zapret service separately; verify
    CHEBURNET refuses to duplicate or terminate them.
11. Corrupt an embedded runtime binary/list and verify exact re-extraction.
    Preplant a junction/hardlink and verify fail-closed behavior.
12. Reboot with a valid process record; verify safe recovery. Exercise old-runtime
    cleanup and confirm current, previous-known-good and pending versions survive.

## Update matrix

13. Offline startup/manual check: show OFFLINE without blocking connection.
14. Online current response: signature VERIFIED and current versions shown.
15. Tamper manifest/signature, offer HTTP redirect, wrong hash/size, truncated
    package and incompatible/downgrade/prerelease metadata: each must show
    `UPDATE REJECTED` with no unsigned fallback or state change.
16. Cancel a real payload download and confirm incomplete staging never activates.
17. Apply a valid newer payload while disconnected, then connect and verify it.
18. Apply while connected: confirm explicit approval, protected new directory,
    stop/start/stabilization/health, committed `active-runtime.json` and preserved
    config/user lists.
19. Simulate candidate start and health failure; verify `[ UPDATE ROLLED BACK ]`,
    previous engine running and state restored. Kill during pending and restart;
    pending must not be promoted.
20. Offer a valid launcher update: verify `CHEBURNET-new.exe` is downloaded and
    its folder opens, while the running executable is not overwritten/executed.
    Replace manually, restart and verify new version.

## Platform/release checks

21. Test Windows Terminal and classic conhost, DPI 100/125/150/200%, Defender
    behavior, SmartScreen presentation and optional real Authenticode signature.
    Do not add AV exclusions.
22. Verify release notices/LICENSES/source links, SHA-256, and that public release
    contains one end-user `CHEBURNET.exe` plus documentation artifacts.
23. Uninstall: disconnect/close, remove `%ProgramData%\CHEBURNET`, remove portable
    EXE; confirm no service, scheduled task or telemetry residue was created.

Until this checklist is executed on clean elevated VMs, the correct verdict is
`READY_FOR_WINDOWS_RC_SMOKE_TEST`, not `PUBLIC_READY`.
