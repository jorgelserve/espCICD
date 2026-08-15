import pytest

from esp32oled_ci.errors import ErrorCode, Esp32OledError
from esp32oled_ci.models import (
    Capabilities,
    DeploymentResult,
    DeviceInfo,
    FirmwareAsset,
    FirmwareManifest,
    ReleaseInfo,
)

SHA = "a" * 64


def make_capabilities(**overrides):
    defaults = {
        "wifi_profiles": 5,
        "ble_provisioning": True,
        "fallback_softap": True,
        "ota": False,
    }
    defaults.update(overrides)
    return Capabilities(**defaults)


def make_manifest(**overrides):
    defaults = {
        "schema": 1,
        "project": "esp32oled",
        "version": "1.1.0",
        "channel": "stable",
        "board": "esp32-c3-devkitm-1",
        "chip": "esp32c3",
        "framework": "arduino",
        "partition_scheme": "ota",
        "firmware_asset": "esp32oled-esp32-c3-devkitm-1-1.1.0.bin",
        "firmware_sha256": SHA,
        "firmware_size": 123456,
        "capabilities": make_capabilities(),
    }
    defaults.update(overrides)
    return FirmwareManifest(**defaults)


class TestErrorContract:
    def test_all_required_error_codes_exist(self):
        expected = {
            "NO_RELEASE",
            "RELEASE_NOT_FOUND",
            "ASSET_NOT_FOUND",
            "MANIFEST_INVALID",
            "BOARD_MISMATCH",
            "CHECKSUM_MISMATCH",
            "SIGNATURE_INVALID",
            "DEVICE_NOT_FOUND",
            "FLASH_FAILED",
            "VERIFY_FAILED",
            "UNSUPPORTED_OTA",
        }
        actual = {code.name for code in ErrorCode}
        assert expected <= actual

    def test_exit_codes_are_distinct_and_nonzero(self):
        exit_codes = [code.exit_code for code in ErrorCode]
        assert len(exit_codes) == len(set(exit_codes))
        assert all(exit_code != 0 for exit_code in exit_codes)

    def test_error_carries_code_and_exit_code(self):
        error = Esp32OledError(ErrorCode.CHECKSUM_MISMATCH, "digest mismatch")
        assert error.error_code is ErrorCode.CHECKSUM_MISMATCH
        assert error.exit_code == ErrorCode.CHECKSUM_MISMATCH.exit_code
        assert "digest mismatch" in str(error)


class TestCapabilities:
    def test_valid_capabilities(self):
        caps = make_capabilities()
        assert caps.wifi_profiles == 5
        assert caps.ble_provisioning is True
        assert caps.ota is False

    def test_capabilities_are_immutable(self):
        caps = make_capabilities()
        with pytest.raises(AttributeError):
            caps.ota = True

    @pytest.mark.parametrize("wifi_profiles", [0, -1])
    def test_rejects_non_positive_wifi_profiles(self, wifi_profiles):
        with pytest.raises(Esp32OledError) as excinfo:
            make_capabilities(wifi_profiles=wifi_profiles)
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_rejects_non_bool_flags(self):
        with pytest.raises(Esp32OledError):
            make_capabilities(ble_provisioning=1)


class TestFirmwareManifest:
    def test_valid_manifest(self):
        manifest = make_manifest()
        assert manifest.version == "1.1.0"
        assert manifest.board == "esp32-c3-devkitm-1"

    def test_manifest_is_immutable(self):
        manifest = make_manifest()
        with pytest.raises(AttributeError):
            manifest.version = "9.9.9"

    def test_rejects_unsupported_schema(self):
        with pytest.raises(Esp32OledError) as excinfo:
            make_manifest(schema=2)
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_rejects_missing_firmware_asset(self):
        with pytest.raises(Esp32OledError):
            make_manifest(firmware_asset="")

    @pytest.mark.parametrize("version", ["1.1", "v1.1.0", "latest", "1.1.0.0", ""])
    def test_rejects_invalid_semver(self, version):
        with pytest.raises(Esp32OledError) as excinfo:
            make_manifest(version=version)
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    @pytest.mark.parametrize("version", ["1.1.0", "0.0.1", "2.13.99"])
    def test_accepts_valid_semver(self, version):
        assert make_manifest(version=version).version == version

    @pytest.mark.parametrize(
        "digest",
        ["", "a" * 63, "a" * 65, "z" * 64, "A" * 64, "0123456789abcdef" * 4 + "!"],
    )
    def test_rejects_malformed_sha256(self, digest):
        with pytest.raises(Esp32OledError) as excinfo:
            make_manifest(firmware_sha256=digest)
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_accepts_well_formed_sha256(self):
        digest = "0123456789abcdef" * 4
        assert make_manifest(firmware_sha256=digest).firmware_sha256 == digest

    @pytest.mark.parametrize("size", [0, -1])
    def test_rejects_non_positive_size(self, size):
        with pytest.raises(Esp32OledError):
            make_manifest(firmware_size=size)

    def test_rejects_empty_board_or_chip(self):
        with pytest.raises(Esp32OledError):
            make_manifest(board="")
        with pytest.raises(Esp32OledError):
            make_manifest(chip="")

    def test_factory_image_requires_bootloader_and_partitions(self):
        manifest = make_manifest(
            bootloader_asset="bootloader.bin",
            bootloader_sha256=SHA,
            bootloader_size=2048,
            partitions_asset="partitions.bin",
            partitions_sha256=SHA,
            partitions_size=3072,
        )
        assert manifest.is_factory_image is True
        assert make_manifest().is_factory_image is False

    @pytest.mark.parametrize(
        "overrides",
        [
            {"bootloader_sha256": None},
            {"bootloader_size": None},
            {"partitions_sha256": None},
            {"partitions_size": None},
        ],
    )
    def test_factory_image_without_asset_metadata_is_invalid(self, overrides):
        kwargs = {
            "bootloader_asset": "bootloader.bin",
            "bootloader_sha256": SHA,
            "bootloader_size": 2048,
            "partitions_asset": "partitions.bin",
            "partitions_sha256": SHA,
            "partitions_size": 3072,
        }
        kwargs.update(overrides)
        with pytest.raises(Esp32OledError) as excinfo:
            make_manifest(**kwargs)
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    @pytest.mark.parametrize(
        "overrides",
        [
            {"bootloader_sha256": SHA},
            {"bootloader_size": 2048},
            {"partitions_sha256": SHA},
            {"partitions_size": 3072},
        ],
    )
    def test_asset_metadata_without_declared_asset_is_invalid(self, overrides):
        with pytest.raises(Esp32OledError) as excinfo:
            make_manifest(**overrides)
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_factory_image_rejects_malformed_bootloader_sha256(self):
        with pytest.raises(Esp32OledError) as excinfo:
            make_manifest(
                bootloader_asset="bootloader.bin",
                bootloader_sha256="nope",
                bootloader_size=2048,
                partitions_asset="partitions.bin",
                partitions_sha256=SHA,
                partitions_size=3072,
            )
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    @pytest.mark.parametrize("size", [0, -1])
    def test_factory_image_rejects_non_positive_partitions_size(self, size):
        with pytest.raises(Esp32OledError) as excinfo:
            make_manifest(
                bootloader_asset="bootloader.bin",
                bootloader_sha256=SHA,
                bootloader_size=2048,
                partitions_asset="partitions.bin",
                partitions_sha256=SHA,
                partitions_size=size,
            )
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID


class TestFirmwareAsset:
    def test_valid_asset(self):
        asset = FirmwareAsset(
            name="fw.bin", url="https://example.com/fw.bin", size=10, sha256=SHA
        )
        assert asset.name == "fw.bin"

    def test_asset_is_immutable(self):
        asset = FirmwareAsset(
            name="fw.bin", url="https://example.com/fw.bin", size=10, sha256=SHA
        )
        with pytest.raises(AttributeError):
            asset.size = 11

    def test_rejects_non_https_url(self):
        with pytest.raises(Esp32OledError):
            FirmwareAsset(
                name="fw.bin", url="http://example.com/fw.bin", size=10, sha256=SHA
            )

    def test_rejects_non_positive_size(self):
        with pytest.raises(Esp32OledError):
            FirmwareAsset(
                name="fw.bin", url="https://example.com/fw.bin", size=0, sha256=SHA
            )

    def test_rejects_malformed_digest(self):
        with pytest.raises(Esp32OledError):
            FirmwareAsset(
                name="fw.bin", url="https://example.com/fw.bin", size=10, sha256="nope"
            )


class TestReleaseInfo:
    def test_valid_release(self):
        asset = FirmwareAsset(
            name="fw.bin", url="https://example.com/fw.bin", size=10, sha256=SHA
        )
        release = ReleaseInfo(
            tag="v1.1.0",
            name="1.1.0",
            channel="stable",
            assets=(asset,),
            html_url="https://github.com/o/r/releases/v1.1.0",
        )
        assert release.tag == "v1.1.0"
        assert release.asset_named("fw.bin") is asset

    def test_release_is_immutable(self):
        asset = FirmwareAsset(
            name="fw.bin", url="https://example.com/fw.bin", size=10, sha256=SHA
        )
        release = ReleaseInfo(
            tag="v1.1.0",
            name="1.1.0",
            channel="stable",
            assets=(asset,),
            html_url="https://github.com/o/r/releases/v1.1.0",
        )
        with pytest.raises(AttributeError):
            release.tag = "v2.0.0"

    def test_asset_named_missing_returns_none(self):
        release = ReleaseInfo(
            tag="v1.1.0",
            name="1.1.0",
            channel="stable",
            assets=(),
            html_url="https://github.com/o/r/releases/v1.1.0",
        )
        assert release.asset_named("missing.bin") is None


class TestDeviceInfo:
    def test_minimal_device_info(self):
        device = DeviceInfo(port="/dev/ttyACM0")
        assert device.port == "/dev/ttyACM0"
        assert device.vid is None
        assert device.serial_number is None

    def test_device_info_is_immutable(self):
        device = DeviceInfo(port="/dev/ttyACM0")
        with pytest.raises(AttributeError):
            device.port = "/dev/ttyUSB0"

    def test_rejects_empty_port(self):
        with pytest.raises(Esp32OledError):
            DeviceInfo(port="")


class TestDeploymentResult:
    def test_success_result_exit_code_zero(self):
        result = DeploymentResult.success(
            version="1.1.0",
            board="esp32-c3-devkitm-1",
            port="/dev/ttyACM0",
            checksum_prefix="aabbccdd",
        )
        assert result.ok is True
        assert result.exit_code == 0
        assert result.error_code is None

    def test_failure_result_uses_error_exit_code(self):
        result = DeploymentResult.failed(
            ErrorCode.VERIFY_FAILED, "serial reported wrong version"
        )
        assert result.ok is False
        assert result.exit_code == ErrorCode.VERIFY_FAILED.exit_code
        assert result.error_code is ErrorCode.VERIFY_FAILED

    def test_result_is_immutable(self):
        result = DeploymentResult.success(
            version="1.1.0",
            board="esp32-c3-devkitm-1",
            port="/dev/ttyACM0",
            checksum_prefix="aabbccdd",
        )
        with pytest.raises(AttributeError):
            result.ok = False
