"""Immutable domain models for firmware releases, manifests, and deployments."""

import re
from dataclasses import dataclass, field

from esp32oled_ci.errors import ErrorCode, Esp32OledError

SUPPORTED_MANIFEST_SCHEMA = 1

_SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$")
_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _invalid(message: str) -> Esp32OledError:
    return Esp32OledError(ErrorCode.MANIFEST_INVALID, message)


def _require_text(value: object, field_name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise _invalid(f"field '{field_name}' must be a non-empty string")
    return value


def _require_sha256(value: object, field_name: str) -> str:
    if not isinstance(value, str) or not _SHA256_RE.fullmatch(value):
        raise _invalid(f"field '{field_name}' must be a lowercase 64-char hex SHA-256")
    return value


def _require_positive_int(value: object, field_name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise _invalid(f"field '{field_name}' must be a positive integer")
    return value


def _require_bool(value: object, field_name: str) -> bool:
    if not isinstance(value, bool):
        raise _invalid(f"capability '{field_name}' must be a boolean")
    return value


def _validate_asset_metadata(
    label: str, asset: str | None, sha256: object, size: object
) -> None:
    if asset is None:
        if sha256 is not None or size is not None:
            raise _invalid(
                f"fields '{label}_sha256'/'{label}_size' are only allowed when "
                f"'{label}_asset' is declared"
            )
        return
    _require_sha256(sha256, f"{label}_sha256")
    _require_positive_int(size, f"{label}_size")


@dataclass(frozen=True, slots=True)
class Capabilities:
    """Firmware capability declarations verified by the client."""

    wifi_profiles: int
    ble_provisioning: bool
    fallback_softap: bool
    ota: bool

    def __post_init__(self) -> None:
        _require_positive_int(self.wifi_profiles, "wifi_profiles")
        _require_bool(self.ble_provisioning, "ble_provisioning")
        _require_bool(self.fallback_softap, "fallback_softap")
        _require_bool(self.ota, "ota")


@dataclass(frozen=True, slots=True)
class FirmwareAsset:
    """A single downloadable artifact attached to a release."""

    name: str
    url: str
    size: int
    sha256: str = ""

    def __post_init__(self) -> None:
        _require_text(self.name, "asset name")
        if not isinstance(self.url, str) or not self.url.startswith("https://"):
            raise _invalid(f"asset '{self.name}' must use an HTTPS URL")
        _require_positive_int(self.size, "asset size")
        if self.sha256:
            _require_sha256(self.sha256, "asset sha256")


@dataclass(frozen=True, slots=True)
class FirmwareManifest:
    """Validated firmware package manifest (schema 1)."""

    schema: int
    project: str
    version: str
    channel: str
    board: str
    chip: str
    framework: str
    partition_scheme: str
    firmware_asset: str
    firmware_sha256: str
    firmware_size: int
    capabilities: Capabilities
    bootloader_asset: str | None = None
    partitions_asset: str | None = None
    bootloader_sha256: str | None = None
    bootloader_size: int | None = None
    partitions_sha256: str | None = None
    partitions_size: int | None = None

    def __post_init__(self) -> None:
        if self.schema != SUPPORTED_MANIFEST_SCHEMA:
            raise _invalid(f"unsupported manifest schema {self.schema!r}")
        for name in ("project", "channel", "board", "chip", "framework", "partition_scheme"):
            _require_text(getattr(self, name), name)
        if not isinstance(self.version, str) or not _SEMVER_RE.fullmatch(self.version):
            raise _invalid(f"field 'version' must be semantic (X.Y.Z), got {self.version!r}")
        _require_text(self.firmware_asset, "firmware_asset")
        _require_sha256(self.firmware_sha256, "firmware_sha256")
        _require_positive_int(self.firmware_size, "firmware_size")
        if not isinstance(self.capabilities, Capabilities):
            raise _invalid("field 'capabilities' must be a Capabilities instance")
        _validate_asset_metadata(
            "bootloader",
            self.bootloader_asset,
            self.bootloader_sha256,
            self.bootloader_size,
        )
        _validate_asset_metadata(
            "partitions",
            self.partitions_asset,
            self.partitions_sha256,
            self.partitions_size,
        )

    @property
    def is_factory_image(self) -> bool:
        """A factory image bundles bootloader and partition table assets."""
        return self.bootloader_asset is not None and self.partitions_asset is not None


@dataclass(frozen=True, slots=True)
class ReleaseInfo:
    """A GitHub release with its downloadable assets."""

    tag: str
    name: str
    channel: str = "stable"
    assets: tuple[FirmwareAsset, ...] = ()
    html_url: str = ""

    def __post_init__(self) -> None:
        _require_text(self.tag, "release tag")
        _require_text(self.channel, "release channel")
        object.__setattr__(self, "assets", tuple(self.assets))

    def asset_named(self, name: str) -> FirmwareAsset | None:
        for asset in self.assets:
            if asset.name == name:
                return asset
        return None


@dataclass(frozen=True, slots=True)
class DeviceInfo:
    """A serial port candidate for deployment (Phase 2)."""

    port: str
    vid: int | None = None
    pid: int | None = None
    serial_number: str | None = None
    description: str | None = None

    def __post_init__(self) -> None:
        _require_text(self.port, "device port")


@dataclass(frozen=True, slots=True)
class DeploymentResult:
    """Outcome of a deployment attempt, mappable to a process exit code."""

    ok: bool
    version: str | None = None
    board: str | None = None
    port: str | None = None
    checksum_prefix: str = ""
    message: str = ""
    error_code: ErrorCode | None = field(default=None)

    @property
    def exit_code(self) -> int:
        if self.ok or self.error_code is None:
            return 0
        return self.error_code.exit_code

    @classmethod
    def success(
        cls,
        *,
        version: str,
        board: str,
        port: str,
        checksum_prefix: str,
        message: str = "",
    ) -> "DeploymentResult":
        return cls(
            ok=True,
            version=version,
            board=board,
            port=port,
            checksum_prefix=checksum_prefix,
            message=message,
        )

    @classmethod
    def failed(
        cls,
        error_code: ErrorCode,
        message: str,
        *,
        version: str | None = None,
        board: str | None = None,
        port: str | None = None,
    ) -> "DeploymentResult":
        return cls(
            ok=False,
            version=version,
            board=board,
            port=port,
            message=message,
            error_code=error_code,
        )
