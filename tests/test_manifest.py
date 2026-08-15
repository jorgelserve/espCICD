import hashlib
import json

import pytest

from esp32oled_ci.errors import ErrorCode, Esp32OledError
from esp32oled_ci.manifest import (
    CapabilityRequirements,
    parse_manifest,
    verify_package,
)

FIRMWARE = b"application-image-bytes"
FIRMWARE_SHA = hashlib.sha256(FIRMWARE).hexdigest()
BOOTLOADER = b"bootloader-bytes"
BOOTLOADER_SHA = hashlib.sha256(BOOTLOADER).hexdigest()
PARTITIONS = b"partitions-bytes"
PARTITIONS_SHA = hashlib.sha256(PARTITIONS).hexdigest()
BOARD = "esp32-c3-devkitm-1"
CHIP = "esp32c3"


def manifest_dict(**overrides) -> dict:
    manifest = {
        "schema": 1,
        "project": "esp32oled",
        "version": "1.1.0",
        "channel": "stable",
        "board": BOARD,
        "chip": CHIP,
        "framework": "arduino",
        "partition_scheme": "ota",
        "firmware_asset": "fw.bin",
        "firmware_sha256": FIRMWARE_SHA,
        "firmware_size": len(FIRMWARE),
        "capabilities": {
            "wifi_profiles": 5,
            "ble_provisioning": True,
            "fallback_softap": True,
            "ota": False,
        },
        "bootloader_asset": "bootloader.bin",
        "bootloader_sha256": BOOTLOADER_SHA,
        "bootloader_size": len(BOOTLOADER),
        "partitions_asset": "partitions.bin",
        "partitions_sha256": PARTITIONS_SHA,
        "partitions_size": len(PARTITIONS),
    }
    manifest.update(overrides)
    return manifest


def write_package(tmp_path, manifest: dict | None = None, *, firmware: bytes = FIRMWARE):
    manifest = manifest_dict() if manifest is None else manifest
    (tmp_path / "fw.bin").write_bytes(firmware)
    (tmp_path / "bootloader.bin").write_bytes(BOOTLOADER)
    (tmp_path / "partitions.bin").write_bytes(PARTITIONS)
    return parse_manifest(json.dumps(manifest))


class TestParseManifest:
    def test_parses_valid_manifest(self):
        manifest = parse_manifest(json.dumps(manifest_dict()))
        assert manifest.version == "1.1.0"
        assert manifest.board == BOARD
        assert manifest.is_factory_image

    def test_rejects_malformed_json(self):
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest("{not json")
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_rejects_non_object_payload(self):
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest("[]")
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_rejects_missing_required_field(self):
        payload = manifest_dict()
        payload.pop("firmware_sha256")
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest(json.dumps(payload))
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_rejects_wrong_field_type(self):
        payload = manifest_dict(firmware_size="123456")
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest(json.dumps(payload))
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    @pytest.mark.parametrize(
        "asset_name", ["../evil.bin", "nested/fw.bin", "/abs/fw.bin", "back\\slash.bin"]
    )
    def test_rejects_non_plain_asset_names(self, asset_name):
        payload = manifest_dict(firmware_asset=asset_name)
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest(json.dumps(payload))
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    @pytest.mark.parametrize(
        "asset_name", ["Source code (zip)", "fw.zip", "fw.tar.gz", "repo-master.zip"]
    )
    def test_rejects_source_archives(self, asset_name):
        payload = manifest_dict(firmware_asset=asset_name)
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest(json.dumps(payload))
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID


    @pytest.mark.parametrize(
        "missing_field",
        [
            "bootloader_sha256",
            "bootloader_size",
            "partitions_sha256",
            "partitions_size",
        ],
    )
    def test_factory_manifest_requires_asset_metadata(self, missing_field):
        payload = manifest_dict()
        payload.pop(missing_field)
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest(json.dumps(payload))
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID


class TestVerifyPackage:
    def test_accepts_valid_factory_package(self, tmp_path):
        manifest = write_package(tmp_path)
        package = verify_package(
            tmp_path,
            manifest,
            image_type="factory",
            expected_board=BOARD,
            expected_chip=CHIP,
        )
        assert package.firmware_path == tmp_path / "fw.bin"
        assert package.bootloader_path == tmp_path / "bootloader.bin"
        assert package.partitions_path == tmp_path / "partitions.bin"
        assert package.verified_sha256 == FIRMWARE_SHA

    def test_board_mismatch_is_detected(self, tmp_path):
        manifest = write_package(tmp_path, manifest_dict(board="esp32-s3-devkitc-1"))
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.BOARD_MISMATCH

    def test_chip_mismatch_is_detected(self, tmp_path):
        manifest = write_package(tmp_path, manifest_dict(chip="esp32s3"))
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.BOARD_MISMATCH

    def test_missing_firmware_file_is_asset_not_found(self, tmp_path):
        manifest = write_package(tmp_path)
        (tmp_path / "fw.bin").unlink()
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.ASSET_NOT_FOUND

    def test_missing_declared_bootloader_is_asset_not_found(self, tmp_path):
        manifest = write_package(tmp_path)
        (tmp_path / "bootloader.bin").unlink()
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.ASSET_NOT_FOUND

    def test_size_mismatch_is_checksum_error(self, tmp_path):
        manifest = write_package(tmp_path, firmware=FIRMWARE + b"extra")
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH

    def test_digest_mismatch_is_checksum_error(self, tmp_path):
        payload = manifest_dict(
            firmware_sha256=hashlib.sha256(b"different bytes").hexdigest()
        )
        manifest = write_package(tmp_path, payload)
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH

    def test_application_only_image_rejected_for_factory(self, tmp_path):
        payload = manifest_dict()
        for field in (
            "bootloader_asset",
            "bootloader_sha256",
            "bootloader_size",
            "partitions_asset",
            "partitions_sha256",
            "partitions_size",
        ):
            payload.pop(field)
        manifest = write_package(tmp_path, payload)
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_ota_image_type_is_not_supported_yet(self, tmp_path):
        manifest = write_package(tmp_path)
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path, manifest, image_type="ota", expected_board=BOARD,
                expected_chip=CHIP,
            )
        assert excinfo.value.error_code is ErrorCode.UNSUPPORTED_OTA


class TestCapabilityRequirements:
    def test_accepts_met_requirements(self, tmp_path):
        manifest = write_package(tmp_path)
        package = verify_package(
            tmp_path,
            manifest,
            image_type="factory",
            expected_board=BOARD,
            expected_chip=CHIP,
            requirements=CapabilityRequirements(
                min_wifi_profiles=5, require_ble_provisioning=True
            ),
        )
        assert package.manifest.capabilities.wifi_profiles == 5

    def test_rejects_insufficient_wifi_profiles(self, tmp_path):
        payload = manifest_dict()
        payload["capabilities"]["wifi_profiles"] = 3
        manifest = write_package(tmp_path, payload)
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
                requirements=CapabilityRequirements(min_wifi_profiles=5),
            )
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_rejects_missing_ble_provisioning(self, tmp_path):
        payload = manifest_dict()
        payload["capabilities"]["ble_provisioning"] = False
        manifest = write_package(tmp_path, payload)
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
                requirements=CapabilityRequirements(require_ble_provisioning=True),
            )
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_rejects_missing_fallback_softap(self, tmp_path):
        payload = manifest_dict()
        payload["capabilities"]["fallback_softap"] = False
        manifest = write_package(tmp_path, payload)
        with pytest.raises(Esp32OledError) as excinfo:
            verify_package(
                tmp_path,
                manifest,
                image_type="factory",
                expected_board=BOARD,
                expected_chip=CHIP,
                requirements=CapabilityRequirements(require_fallback_softap=True),
            )
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID
