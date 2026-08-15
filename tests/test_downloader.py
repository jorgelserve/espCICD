import hashlib

import httpx
import pytest

from esp32oled_ci.downloader import download, file_sha256
from esp32oled_ci.errors import ErrorCode, Esp32OledError

PAYLOAD = b"firmware-bytes-0123456789"
PAYLOAD_SHA = hashlib.sha256(PAYLOAD).hexdigest()


def client_with(handler) -> httpx.Client:
    return httpx.Client(transport=httpx.MockTransport(handler))


def redirect_client(handler) -> httpx.Client:
    return httpx.Client(
        transport=httpx.MockTransport(handler), follow_redirects=True
    )


def ok_handler(body: bytes = PAYLOAD):
    def handler(request: httpx.Request) -> httpx.Response:
        assert request.url.scheme == "https"
        return httpx.Response(200, content=body)

    return handler


class TestFileSha256:
    def test_hashes_file_contents(self, tmp_path):
        target = tmp_path / "fw.bin"
        target.write_bytes(PAYLOAD)
        assert file_sha256(target) == PAYLOAD_SHA


class TestDownload:
    def test_publishes_verified_file_atomically(self, tmp_path):
        dest = tmp_path / "fw.bin"
        result = download(
            client_with(ok_handler()),
            "https://example.com/fw.bin",
            dest,
            expected_size=len(PAYLOAD),
            expected_sha256=PAYLOAD_SHA,
        )
        assert result == dest
        assert dest.read_bytes() == PAYLOAD

    def test_no_temp_files_left_behind(self, tmp_path):
        download(
            client_with(ok_handler()),
            "https://example.com/fw.bin",
            tmp_path / "fw.bin",
            expected_size=len(PAYLOAD),
            expected_sha256=PAYLOAD_SHA,
        )
        assert [p.name for p in tmp_path.iterdir()] == ["fw.bin"]

    def test_rejects_http_url_before_any_request(self, tmp_path):
        def handler(request: httpx.Request) -> httpx.Response:
            raise AssertionError("no request must be made")

        with pytest.raises(ValueError, match="HTTPS"):
            download(
                client_with(handler), "http://example.com/fw.bin", tmp_path / "fw.bin"
            )
        assert list(tmp_path.iterdir()) == []

    def test_rejects_redirect_from_https_to_http(self, tmp_path):
        def handler(request: httpx.Request) -> httpx.Response:
            if request.url.scheme == "https":
                return httpx.Response(
                    302, headers={"Location": "http://insecure.example/fw.bin"}
                )
            return httpx.Response(200, content=PAYLOAD)

        with pytest.raises(Esp32OledError) as excinfo:
            download(
                redirect_client(handler),
                "https://example.com/fw.bin",
                tmp_path / "fw.bin",
            )
        assert excinfo.value.error_code is ErrorCode.GITHUB_API
        assert list(tmp_path.iterdir()) == []

    def test_follows_https_to_https_redirect(self, tmp_path):
        def handler(request: httpx.Request) -> httpx.Response:
            if request.url.host == "example.com":
                return httpx.Response(
                    301, headers={"Location": "https://cdn.example/fw.bin"}
                )
            return httpx.Response(200, content=PAYLOAD)

        dest = tmp_path / "fw.bin"
        result = download(
            redirect_client(handler),
            "https://example.com/fw.bin",
            dest,
            expected_size=len(PAYLOAD),
            expected_sha256=PAYLOAD_SHA,
        )
        assert result == dest
        assert dest.read_bytes() == PAYLOAD

    def test_checksum_mismatch_keeps_destination_intact(self, tmp_path):
        dest = tmp_path / "fw.bin"
        dest.write_bytes(b"previous verified content")
        with pytest.raises(Esp32OledError) as excinfo:
            download(
                client_with(ok_handler(b"tampered")),
                "https://example.com/fw.bin",
                dest,
                expected_size=len(b"tampered"),
                expected_sha256=PAYLOAD_SHA,
            )
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH
        assert dest.read_bytes() == b"previous verified content"
        assert [p.name for p in tmp_path.iterdir()] == ["fw.bin"]

    def test_size_mismatch_against_manifest_is_checksum_error(self, tmp_path):
        with pytest.raises(Esp32OledError) as excinfo:
            download(
                client_with(ok_handler(PAYLOAD)),
                "https://example.com/fw.bin",
                tmp_path / "fw.bin",
                expected_size=len(PAYLOAD) - 1,
                expected_sha256=PAYLOAD_SHA,
            )
        assert excinfo.value.error_code is ErrorCode.CHECKSUM_MISMATCH

    def test_oversized_download_aborts_and_cleans_up(self, tmp_path):
        with pytest.raises(Esp32OledError) as excinfo:
            download(
                client_with(ok_handler()),
                "https://example.com/fw.bin",
                tmp_path / "fw.bin",
                max_bytes=len(PAYLOAD) - 1,
            )
        assert excinfo.value.error_code is ErrorCode.GITHUB_API
        assert list(tmp_path.iterdir()) == []

    def test_http_error_is_surfaced(self, tmp_path):
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(503, text="unavailable")

        with pytest.raises(Esp32OledError) as excinfo:
            download(
                client_with(handler), "https://example.com/fw.bin", tmp_path / "fw.bin"
            )
        assert excinfo.value.error_code is ErrorCode.GITHUB_API
        assert list(tmp_path.iterdir()) == []

    def test_network_error_is_surfaced_and_cleaned_up(self, tmp_path):
        def handler(request: httpx.Request) -> httpx.Response:
            raise httpx.ConnectError("connection refused")

        with pytest.raises(Esp32OledError) as excinfo:
            download(
                client_with(handler), "https://example.com/fw.bin", tmp_path / "fw.bin"
            )
        assert excinfo.value.error_code is ErrorCode.GITHUB_API
        assert list(tmp_path.iterdir()) == []

    def test_uses_streaming_api(self, tmp_path, monkeypatch):
        calls = []
        original_stream = httpx.Client.stream

        def spy(client, method, url, **kwargs):
            calls.append(url)
            return original_stream(client, method, url, **kwargs)

        monkeypatch.setattr(httpx.Client, "stream", spy)
        body = bytes(range(256)) * 40
        download(
            client_with(ok_handler(body)),
            "https://example.com/fw.bin",
            tmp_path / "fw.bin",
            expected_size=len(body),
            expected_sha256=hashlib.sha256(body).hexdigest(),
        )
        assert calls == ["https://example.com/fw.bin"]

    def test_does_not_replace_destination_before_verification(self, tmp_path):
        """A failed download must leave the old destination byte-identical."""
        dest = tmp_path / "fw.bin"
        dest.write_bytes(PAYLOAD)
        with pytest.raises(Esp32OledError):
            download(
                client_with(ok_handler(b"other-bytes")),
                "https://example.com/fw.bin",
                dest,
                expected_size=len(b"other-bytes"),
                expected_sha256="0" * 64,
            )
        assert dest.read_bytes() == PAYLOAD

    def test_verified_redownload_may_replace_destination(self, tmp_path):
        dest = tmp_path / "fw.bin"
        dest.write_bytes(b"old but previously verified")
        download(
            client_with(ok_handler()),
            "https://example.com/fw.bin",
            dest,
            expected_size=len(PAYLOAD),
            expected_sha256=PAYLOAD_SHA,
        )
        assert dest.read_bytes() == PAYLOAD
