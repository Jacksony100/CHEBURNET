# Security policy

## Reporting

Please report suspected vulnerabilities privately through the repository's
GitHub Security Advisories feature. Do not include private keys, credentials,
packet captures, or personal logs in a public issue. Include the CHEBURNET
version, Windows version, reproducible steps, and the smallest redacted log
excerpt needed to explain the issue.

Supported releases are the latest public stable/RC release only. Security fixes
will be documented in release notes after users have had a reasonable upgrade
window.

## Boundaries

CHEBURNET runs elevated because its embedded upstream engine uses WinDivert.
Update manifests are authenticated with ECDSA P-256 and embedded public keys;
unsigned manifests, HTTP redirects, downgrades, incompatible schemas and unsafe
packages fail closed. Release signing private keys must never be committed.

Do not request or propose Defender exclusions, SmartScreen bypasses, injection,
driver tampering, certificate-validation bypasses or raw remote-script execution.
These are outside the project's security model.

Third-party engine vulnerabilities should also be coordinated with the relevant
upstream project; see `THIRD_PARTY_NOTICES.md` for exact provenance.
