# Update security model

## Trusted inputs

The launcher trusts only keys compiled from
`resources/update/release-public-key.json`, its built-in resource manifest, and
protected state/files below `%ProgramData%\CHEBURNET`. GitHub transport alone is
not a trust root. The current key id is `cheburnet-release-2026`, algorithm
ECDSA P-256 with SHA-256.

The client fetches an exact manifest and detached Base64 signature over HTTPS,
strictly parses the envelope only to select `key_id`, verifies the exact bytes
through Windows CNG, and only then trusts versions, HTTPS URLs, sizes, hashes or
compatibility fields. Signature failure has one result: `UPDATE REJECTED`; there
is no unsigned fallback.

## Downloads and activation

Native WinHTTP uses standard certificate validation/system proxy policy,
HTTPS-only redirects, bounded redirects/timeouts/content size, cancellation,
real byte progress and ETag support. Downloads go to a protected staging root
using random `CREATE_NEW` temporary files and atomic activation. SHA-256 and
exact size must match the signed manifest.

Payloads use CHEBURNET's narrow `.cbpkg` format rather than raw upstream ZIP.
Each regular file has an offset, size and SHA-256; paths are normalized and
allowlisted to engine binaries, vendor lists, strategy catalog and provenance.
Absolute/parent/UNC/drive/ADS paths, duplicates, reparse/symlink metadata,
trailing data, truncation, excessive files and sizes are rejected.

A payload is extracted into a new `runtime\<version>` directory. Every file,
the strict strategy catalog, provider/version and schema are rechecked. If a
trusted engine is running, activation stops only its verified pid/creation-time/
image identity, starts the candidate transactionally, waits for stabilization
and checks identity/liveness. State commits only after health succeeds. Any
candidate failure attempts the protected previous-known-good runtime. Interrupted
`pending` state is never promoted at startup.

User config/list overlays are outside the immutable runtime and survive updates.
At most current, previous-known-good, pending and built-in versions are retained
by cleanup.

## Launcher updates

The launcher artifact has its own version/hash/size/minimum-version state
machine. This release intentionally uses the safer specification fallback:
download and verify `CHEBURNET-new.exe`, then explicitly open its protected
location. It does not overwrite or execute a new elevated binary silently.

## Availability, downgrade and compatibility

Network failure never blocks a known-good runtime. Stable mode rejects
prereleases, all remote downgrades, unknown payload/strategy schemas and payloads
requiring a newer launcher. No developer downgrade override is enabled in
Release builds.

## Key rotation and release signing

Add a new public key to a reviewed launcher release before signing manifests
with it. Maintain an overlap period; revoke a compromised key by shipping a new
launcher through the normal independently verified release channel. Private CNG
key blobs are provided to CI only as the masked
`CHEBURNET_SIGNING_KEY_CNG_BLOB_B64` secret or kept DPAPI-protected offline.

To reproduce public hashes:

```powershell
Get-FileHash .\CHEBURNET.exe -Algorithm SHA256
Get-FileHash .\cheburnet-payload-<engine-version>.cbpkg -Algorithm SHA256
```

The detached signature covers the manifest file byte-for-byte; newline or
whitespace changes invalidate it.
