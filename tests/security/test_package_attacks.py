import hashlib
import json

import httpx
import pytest

from esp32oled_ci.downloader import download
from esp32oled_ci.errors import ErrorCode, Esp32OledError
from esp32oled_ci.github_release import GitHubReleaseClient, require_asset
from esp32oled_ci.manifest import parse_manifest, verify_package

BOARD = "esp32-c3-devkitm-1"
CHIP = "esp32c3"
GOOD_FIRMWARE = b"authentic application image"
GOOD_SHA = hashlib.sha256(GOOD_FIRMWARE).hexdigest()
TAMPERED_FIRMWARE = b"attacker-controlled image"
TAMPERED_SHA = hashlib.sha256(TAMPERED_FIRMWARE).hexdigest()
BOOTLOADER = b"bootloader"
BOOTLOADER_SHA = hashlib.sha256(BOOTLOADER).hexdigest()
PARTITIONS = b"partitions"
PARTITIONS_SHA = hashlib.sha256(PARTITIONS).hexdigest()


def manifest_bytes(**overrides) -> bytes:
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
        "firmware_sha256": GOOD_SHA,
        "firmware_size": len(GOOD_FIRMWARE),
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
    return json.dumps(manifest).encode()


def verify(tmp_path, manifest_payload: bytes):
    manifest = parse_manifest(manifest_payload)
    return verify_package(
        tmp_path,
        manifest,
        image_type="factory",
        expected_board=BOARD,
        expected_chip=CHIP,
    )


def client_with(handler) -> httpx.Client:
    return httpx.Client(transport=httpx.MockTransport(handler))


def serve_files(files: dict[bytes | str, bytes], request_log: list | None = None):
    def handler(request: httpx.Request) -> httpx.Response:
        path = request.url.path
        if request_log is not None:
            request_log.append(path)
        for name, body in files.items():
            if path.endswith(str(name)):
                return httpx.Response(200, content=body)
        return httpx.Response(404, text="not found")

    return handler


class TestRequireAsset:
    def test_returns_named_asset(self):
        payload = {
            "tag_name": "v1.1.0",
            "assets": [
                {
                    "name": "manifest.json",
                    "browser_download_url": "https://github.com/o/r/1/manifest.json",
                    "size": 10,
                }
            ],
        }
        client = client_with(
            lambda request: httpx.Response(200, json=payload)
        )
        release = GitHubReleaseClient(client, "o/r").get("latest")
        asset = require_asset(release, "manifest.json")
        assert asset.url.endswith("manifest.json")

    def test_release_without_assets_is_asset_not_found(self):
        payload = {"tag_name": "v1.1.0", "assets": []}
        client = client_with(lambda request: httpx.Response(200, json=payload))
        release = GitHubReleaseClient(client, "o/r").get("latest")
        with pytest.raises(Esp32OledError) as excinfo:
            require_asset(release, "manifest.json")
        assert excinfo.value.error_code is ErrorCode.ASSET_NOT_FOUND

    def test_release_missing_named_asset_is_asset_not_found(self):
        payload = {
            "tag_name": "v1.1.0",
            "assets": [
                {
                    "name": "unrelated.bin",
                    "browser_download_url": "https://github.com/o/r/1/unrelated.bin",
                    "size": 10,
                }
            ],
        }
        client = client_with(lambda request: httpx.Response(200, json=payload))
        release = GitHubReleaseClient(client, "o/r").get("latest")
        with pytest.raises(Esp32OledError) as excinfo:
            require_asset(release, "manifest.json")
        assert excinfo.value.error_code is ErrorCode.ASSET_NOT_FOUND


class TestManifestPointsToDifferentAsset:
    def test_digest_of_other_file_is_rejected(self, tmp_path):
        (tmp_path / "fw.bin").write_bytes(TAMPERED_FIRMWARE)
        (tmp_path / "bootloader.bin").write_bytes(BOOTLOADER)
        (tmp_path / "partitions.bin").write_bytes(PARTITIONS)
        with pytest.raises(Esp32OledError) as excinfo:
            verify(tmp_path, manifest_bytes())
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH

    def test_swapped_asset_name_with_foreign_digest_is_rejected(self, tmp_path):
        (tmp_path / "fw.bin").write_bytes(TAMPERED_FIRMWARE)
        (tmp_path / "bootloader.bin").write_bytes(BOOTLOADER)
        (tmp_path / "partitions.bin").write_bytes(PARTITIONS)
        with pytest.raises(Esp32OledError) as excinfo:
            verify(tmp_path, manifest_bytes(firmware_sha256=TAMPERED_SHA[:63] + "0"))
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH


class TestTruncatedAsset:
    def test_truncated_download_is_rejected_and_cleaned_up(self, tmp_path):
        truncated = GOOD_FIRMWARE[: len(GOOD_FIRMWARE) // 2]
        handler = serve_files({"manifest.json": manifest_bytes(), "fw.bin": truncated})
        manifest_path = download(
            client_with(handler), "https://gh/manifest.json", tmp_path / "manifest.json"
        )
        manifest = parse_manifest(manifest_path.read_bytes())
        with pytest.raises(Esp32OledError) as excinfo:
            download(
                client_with(handler),
                "https://gh/fw.bin",
                tmp_path / "fw.bin",
                expected_size=manifest.firmware_size,
                expected_sha256=manifest.firmware_sha256,
            )
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH
        assert not (tmp_path / "fw.bin").exists()
        assert list(tmp_path.iterdir()) == [tmp_path / "manifest.json"]


class TestAssetReplacedAfterManifestDownload:
    def test_server_substitution_is_rejected_and_destination_kept(self, tmp_path):
        request_count = {"n": 0}

        def handler(request: httpx.Request) -> httpx.Response:
            if request.url.path.endswith("fw.bin"):
                request_count["n"] += 1
                body = GOOD_FIRMWARE if request_count["n"] == 1 else TAMPERED_FIRMWARE
                return httpx.Response(200, content=body)
            return httpx.Response(200, content=manifest_bytes())

        dest = tmp_path / "fw.bin"

        # First download succeeds and publishes the verified file.
        download(
            client_with(handler),
            "https://gh/fw.bin",
            dest,
            expected_size=len(GOOD_FIRMWARE),
            expected_sha256=GOOD_SHA,
        )
        assert dest.read_bytes() == GOOD_FIRMWARE

        # The server now serves substituted bytes for the same URL.
        with pytest.raises(Esp32OledError) as excinfo:
            download(
                client_with(handler),
                "https://gh/fw.bin",
                dest,
                expected_size=len(GOOD_FIRMWARE),
                expected_sha256=GOOD_SHA,
            )
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH
        assert dest.read_bytes() == GOOD_FIRMWARE
        assert list(tmp_path.iterdir()) == [dest]

    def test_files_swapped_on_disk_before_verify_are_rejected(self, tmp_path):
        (tmp_path / "fw.bin").write_bytes(GOOD_FIRMWARE)
        (tmp_path / "bootloader.bin").write_bytes(BOOTLOADER)
        (tmp_path / "partitions.bin").write_bytes(PARTITIONS)
        verified = verify(tmp_path, manifest_bytes())
        assert verified.verified_sha256 == GOOD_SHA

        (tmp_path / "fw.bin").write_bytes(TAMPERED_FIRMWARE)
        with pytest.raises(Esp32OledError) as excinfo:
            verify(tmp_path, manifest_bytes())
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH


class TestBoardAndChipDeclarations:
    def test_wrong_board_package_is_rejected(self, tmp_path):
        (tmp_path / "fw.bin").write_bytes(GOOD_FIRMWARE)
        (tmp_path / "bootloader.bin").write_bytes(BOOTLOADER)
        (tmp_path / "partitions.bin").write_bytes(PARTITIONS)
        with pytest.raises(Esp32OledError) as excinfo:
            verify(tmp_path, manifest_bytes(board="esp32-s3-devkitc-1"))
        assert excinfo.value.error_code is ErrorCode.BOARD_MISMATCH

    def test_wrong_chip_package_is_rejected(self, tmp_path):
        (tmp_path / "fw.bin").write_bytes(GOOD_FIRMWARE)
        (tmp_path / "bootloader.bin").write_bytes(BOOTLOADER)
        (tmp_path / "partitions.bin").write_bytes(PARTITIONS)
        with pytest.raises(Esp32OledError) as excinfo:
            verify(tmp_path, manifest_bytes(chip="esp32s3"))
        assert excinfo.value.error_code is ErrorCode.BOARD_MISMATCH


class TestMalformedSha256Format:
    def test_uppercase_digest_is_rejected(self, tmp_path):
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest(manifest_bytes(firmware_sha256=GOOD_SHA.upper()))
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID

    def test_short_digest_is_rejected(self):
        with pytest.raises(Esp32OledError) as excinfo:
            parse_manifest(manifest_bytes(firmware_sha256="abcd"))
        assert excinfo.value.error_code is ErrorCode.MANIFEST_INVALID
