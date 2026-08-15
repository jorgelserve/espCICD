# Firmware package contract

`esp32oledCI` deploys versioned firmware packages published as GitHub Release
assets. The firmware itself lives in the firmware repository (`esp32oled`,
referenced locally by `esp32ToneHub`, which is never modified by this client).
This document defines what a release must contain so the client can verify it.

## Manifest

Each release must attach a JSON manifest declaring the package contents:

```json
{
  "schema": 1,
  "project": "esp32oled",
  "version": "1.1.0",
  "channel": "stable",
  "board": "esp32-c3-devkitm-1",
  "chip": "esp32c3",
  "framework": "arduino",
  "partition_scheme": "ota",
  "firmware_asset": "esp32oled-esp32-c3-devkitm-1-1.1.0.bin",
  "firmware_sha256": "...",
  "firmware_size": 123456,
  "capabilities": {
    "wifi_profiles": 5,
    "ble_provisioning": true,
    "fallback_softap": true,
    "ota": false
  }
}
```

Rules enforced by the client:

- `schema` must be `1`. Unknown schemas are rejected, never guessed.
- `version` must be semantic (`X.Y.Z`).
- `firmware_sha256` must be a lowercase 64-character hex digest of
  `firmware_asset`, and `firmware_size` must match the downloaded file.
- `board` and `chip` must match the deployment target exactly.
- `capabilities` declares firmware features. The client verifies these
  declarations; it does not implement them. The required firmware
  capabilities are multiple persisted Wi-Fi profiles, automatic connection,
  BLE provisioning, and fallback SoftAP.
- Asset names must be plain file names. Path traversal is rejected.

## Factory vs application-only images

A USB factory package additionally declares `bootloader_asset` and
`partitions_asset`. A manifest without them is an application-only image and
must never be flashed as a factory image.

## Non-goals

Source archives (`Source code (zip)` / `.tar.gz`) are not firmware packages
and are rejected. OTA requires signed packages and an OTA partition scheme;
SHA-256 alone provides integrity, not authenticity (see
[threat-model.md](threat-model.md)).
