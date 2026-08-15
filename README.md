# esp32oledCI

ESP32-C3 firmware that updates itself from GitHub Releases. The device only
needs Wi-Fi: it polls `jorgelserve/espCICD` releases, compares semver tags,
downloads `firmware.bin` and flashes it over OTA — no PC, no cables.

## What runs on the device (`firmware/`)

- **OledDisplay** — status UI (mode, SSID, IP).
- **WifiManager** — up to 5 Wi-Fi profiles in NVS; if none connect, starts AP
  `esp32oled-ci` with a captive portal at `192.168.4.1`.
- **WebPortal** — scan/pick/save networks, status JSON, HTTP OTA endpoint.
- **TelnetLogger** — logs and commands (`check`, `status`) on port 23.
- **GitHubUpdater** — [SafeGithubOTA](firmware/lib/SafeGithubOTA)-based
  pull OTA (vendored; public repo, no PAT). Auto-check every 6 h.

## CI/CD (`.github/workflows/`)

- `ci.yml` — builds the firmware on every push/PR to `main`.
- `release.yml` — on tag `vX.Y.Z`, builds and publishes `firmware.bin` as a
  GitHub Release; the device picks it up on its next check.

```bash
cd firmware
pio run          # build
pio run -t upload  # flash over USB
```

See [docs/firmware-contract.md](docs/firmware-contract.md) and
[docs/threat-model.md](docs/threat-model.md).
