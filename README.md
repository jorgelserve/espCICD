# esp32oledCI

Deployment client for ESP32-C3 firmware releases published on GitHub. It
downloads a firmware package from `jorgelserve/espCICD` (or a configured
repository), verifies its manifest and SHA-256 digest, and prepares it for
deployment. USB flashing, serial verification, and OTA are later phases; this
repository currently implements release acquisition and package verification
only.

## Repository boundaries

- `esp32oledCI` **consumes** firmware releases. It does not implement the
  firmware network stack, Wi-Fi provisioning, BLE, or SoftAP.
- `/home/jlsernav/hardware/esp32ToneHub` and `jorgelserve/esp32oled` are
  read-only firmware references. This client never modifies them.
- GitHub source archives (`zip`/`tar.gz`) are never treated as firmware.

The firmware must declare its capabilities in a release manifest. See
[docs/firmware-contract.md](docs/firmware-contract.md) and
[docs/threat-model.md](docs/threat-model.md).

## Development

Requires Python 3.11 and [uv](https://docs.astral.sh/uv/).

```bash
uv sync
uv run pytest -q
uv run ruff check .
uv run esp32oled-ci --help
```
