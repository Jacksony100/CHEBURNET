# Privacy

CHEBURNET has no telemetry, analytics, traffic collection, crash upload, remote
log upload or advertising. It does not inspect or retain browser data or
credentials. Local operational logs are written only under
`%ProgramData%\CHEBURNET\logs` and contain versions, update outcomes, process
identity/status and errors—not packet contents or command-line payload data.

When update checks are enabled, the program sends ordinary HTTPS GET requests
through the standard Windows WinHTTP proxy policy to the configured official
CHEBURNET release endpoints. The User-Agent contains the CHEBURNET version.
GitHub and the user's network provider may therefore observe normal request
metadata under their own policies. No stable user identifier is added.

Set update mode to `disabled` or turn off startup checks in Settings to prevent
automatic update requests. Local logs and runtime/user data are removed only by
the user; CHEBURNET does not remotely delete or upload them.
