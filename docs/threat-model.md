# Threat model

`esp32oledCI` downloads firmware over the network and writes it to a device.
This document records what the client defends against in the current phase
(release acquisition and verification) and what is explicitly out of scope.

## Assets and channels

- Firmware packages and manifests: downloaded over HTTPS from GitHub Release
  assets only. Plain HTTP is refused.
- GitHub API responses: untrusted input. Malformed payloads surface as
  distinct errors; they are never interpreted as firmware.
- Credentials: GitHub tokens if configured. Never logged or embedded in
  subprocess output.

## Threats addressed (current phase)

| Threat | Mitigation |
|---|---|
| Attacker substitutes firmware bytes in transit | HTTPS plus SHA-256 verification against the manifest before any file is published to its final name |
| Truncated or oversized download | Size checking and configurable maximum during streaming; partial downloads stay in temp files and are deleted |
| Manifest describes a different asset than the one downloaded | Asset names, sizes, and digests are cross-checked; mismatches abort before flashing |
| Package targets another board/chip | `BOARD_MISMATCH` before any device interaction |
| Path traversal via asset names | Only plain file names inside the package directory are accepted |
| Corrupted existing verified package overwritten by a bad download | Rename happens only after verification; failed downloads never touch the destination |
| Source archive mistaken for firmware | Archives (`zip`/`tar.gz`) are rejected |

## Out of scope (later phases)

- **Authenticity.** SHA-256 provides integrity only. Until manifests carry a
  signature verified against a public key pinned in the firmware, a
  compromised GitHub account or release can still publish malicious firmware
  that hashes consistently. Signed OTA is a Phase 5 requirement.
- **Rollback protection.** Version anti-rollback policy is deferred with OTA.
- **Physical supply chain.** The client trusts the connected USB device
  enumerator once flashing is implemented.

## Trust anchors

GitHub TLS, the GitHub account publishing releases, and the manifest digest.
Compromise of any of these is outside the current mitigation scope and is the
reason signatures are required before OTA.
