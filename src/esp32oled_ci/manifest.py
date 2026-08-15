"""Offline firmware manifest parsing and package verification."""

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from esp32oled_ci.downloader import file_sha256
from esp32oled_ci.errors import ErrorCode, Esp32OledError
from esp32oled_ci.models import Capabilities, FirmwareManifest

MANIFEST_ASSET_NAME = "manifest.json"

_ARCHIVE_SUFFIXES = (".zip", ".tar.gz", ".tgz", ".tar.bz2")


@dataclass(frozen=True, slots=True)
class CapabilityRequirements:
    """Minimum firmware capabilities a deployment requires."""

    min_wifi_profiles: int | None = None
    require_ble_provisioning: bool = False
    require_fallback_softap: bool = False
    require_ota: bool = False


@dataclass(frozen=True, slots=True)
class VerifiedPackage:
    """A package whose files match its manifest and target board."""

    manifest: FirmwareManifest
    package_dir: Path
    firmware_path: Path
    bootloader_path: Path | None
    partitions_path: Path | None
    verified_sha256: str
    image_type: str


def _invalid(message: str) -> Esp32OledError:
    return Esp32OledError(ErrorCode.MANIFEST_INVALID, message)


def _require_plain_asset_name(name: str) -> None:
    if (
        not name
        or "/" in name
        or "\\" in name
        or name in (".", "..")
        or name.startswith(".")
    ):
        raise _invalid(f"asset name {name!r} must be a plain file name")
    lowered = name.lower()
    if lowered.endswith(_ARCHIVE_SUFFIXES) or "source code" in lowered:
        raise _invalid(f"asset {name!r} is a source archive, not a firmware package")


def _capabilities_from_dict(payload: Any) -> dict:
    if not isinstance(payload, dict):
        raise _invalid("'capabilities' must be an object")
    capabilities = dict(payload)
    capabilities.setdefault("wifi_profiles", 1)
    capabilities.setdefault("ble_provisioning", False)
    capabilities.setdefault("fallback_softap", False)
    capabilities.setdefault("ota", False)
    return Capabilities(**capabilities)


def parse_manifest(data: str | bytes) -> FirmwareManifest:
    """Parse and validate a manifest document. No network access needed."""
    if isinstance(data, bytes):
        data = data.decode("utf-8", errors="replace")
    try:
        payload = json.loads(data)
    except ValueError as exc:
        raise _invalid(f"manifest is not valid JSON: {exc}") from exc
    if not isinstance(payload, dict):
        raise _invalid("manifest must be a JSON object")

    for field in ("firmware_asset", "bootloader_asset", "partitions_asset"):
        asset_name = payload.get(field)
        if asset_name is not None:
            _require_plain_asset_name(asset_name)

    payload["capabilities"] = _capabilities_from_dict(payload.get("capabilities"))
    try:
        return FirmwareManifest(
            schema=payload.get("schema"),
            project=payload.get("project", ""),
            version=payload.get("version", ""),
            channel=payload.get("channel", ""),
            board=payload.get("board", ""),
            chip=payload.get("chip", ""),
            framework=payload.get("framework", ""),
            partition_scheme=payload.get("partition_scheme", ""),
            firmware_asset=payload.get("firmware_asset", ""),
            firmware_sha256=payload.get("firmware_sha256", ""),
            firmware_size=payload.get("firmware_size", 0),
            capabilities=payload["capabilities"],
            bootloader_asset=payload.get("bootloader_asset"),
            partitions_asset=payload.get("partitions_asset"),
            bootloader_sha256=payload.get("bootloader_sha256"),
            bootloader_size=payload.get("bootloader_size"),
            partitions_sha256=payload.get("partitions_sha256"),
            partitions_size=payload.get("partitions_size"),
        )
    except Esp32OledError:
        raise
    except TypeError as exc:
        raise _invalid(f"manifest has invalid field types: {exc}") from exc


def verify_package(
    package_dir: Path,
    manifest: FirmwareManifest,
    *,
    image_type: str,
    expected_board: str,
    expected_chip: str,
    requirements: CapabilityRequirements | None = None,
) -> VerifiedPackage:
    """Verify package files against the manifest. Works fully offline."""
    if manifest.board != expected_board or manifest.chip != expected_chip:
        raise Esp32OledError(
            ErrorCode.BOARD_MISMATCH,
            f"package targets {manifest.board}/{manifest.chip}, "
            f"expected {expected_board}/{expected_chip}",
        )
    if image_type not in ("factory", "ota"):
        raise _invalid(f"unknown image type {image_type!r}")
    if image_type == "ota":
        raise Esp32OledError(
            ErrorCode.UNSUPPORTED_OTA,
            "OTA deployment is not supported until signed packages exist",
        )
    if not manifest.is_factory_image:
        raise _invalid(
            "application-only image cannot be used for a factory deployment; "
            "the manifest must declare bootloader and partition assets"
        )

    _require_plain_asset_name(manifest.firmware_asset)
    firmware_path = package_dir / manifest.firmware_asset
    _require_existing_file(firmware_path)
    bootloader_path = _declared_asset(package_dir, manifest.bootloader_asset)
    partitions_path = _declared_asset(package_dir, manifest.partitions_asset)

    actual_digest = _verify_asset_file(
        firmware_path, manifest.firmware_sha256, manifest.firmware_size, "firmware"
    )
    _verify_asset_file(
        bootloader_path,
        manifest.bootloader_sha256,
        manifest.bootloader_size,
        "bootloader",
    )
    _verify_asset_file(
        partitions_path,
        manifest.partitions_sha256,
        manifest.partitions_size,
        "partitions",
    )

    if requirements is not None:
        _verify_requirements(manifest, requirements)

    return VerifiedPackage(
        manifest=manifest,
        package_dir=package_dir,
        firmware_path=firmware_path,
        bootloader_path=bootloader_path,
        partitions_path=partitions_path,
        verified_sha256=actual_digest,
        image_type=image_type,
    )


def _declared_asset(package_dir: Path, asset_name: str | None) -> Path | None:
    if asset_name is None:
        return None
    _require_plain_asset_name(asset_name)
    path = package_dir / asset_name
    _require_existing_file(path)
    return path


def _verify_asset_file(
    path: Path, expected_sha256: str, expected_size: int, label: str
) -> str:
    actual_size = path.stat().st_size
    if actual_size != expected_size:
        raise Esp32OledError(
            ErrorCode.CHECKSUM_MISMATCH,
            f"{label} file is {actual_size} bytes, manifest declares "
            f"{expected_size}",
        )
    actual_digest = file_sha256(path)
    if actual_digest != expected_sha256:
        raise Esp32OledError(
            ErrorCode.CHECKSUM_MISMATCH,
            f"{label} SHA-256 mismatch: got {actual_digest}",
        )
    return actual_digest


def _require_existing_file(path: Path) -> None:
    if not path.is_file():
        raise Esp32OledError(
            ErrorCode.ASSET_NOT_FOUND, f"declared asset missing: {path.name}"
        )


def _verify_requirements(
    manifest: FirmwareManifest, requirements: CapabilityRequirements
) -> None:
    capabilities = manifest.capabilities
    if (
        requirements.min_wifi_profiles is not None
        and capabilities.wifi_profiles < requirements.min_wifi_profiles
    ):
        raise _invalid(
            f"firmware declares {capabilities.wifi_profiles} Wi-Fi profiles, "
            f"deployment requires {requirements.min_wifi_profiles}"
        )
    if requirements.require_ble_provisioning and not capabilities.ble_provisioning:
        raise _invalid("firmware does not declare BLE provisioning")
    if requirements.require_fallback_softap and not capabilities.fallback_softap:
        raise _invalid("firmware does not declare fallback SoftAP")
    if requirements.require_ota and not capabilities.ota:
        raise _invalid("firmware does not declare OTA capability")
