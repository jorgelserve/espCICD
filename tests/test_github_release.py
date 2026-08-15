import json

import httpx
import pytest

from esp32oled_ci.errors import ErrorCode, Esp32OledError
from esp32oled_ci.github_release import (
    DEFAULT_REPOSITORY,
    GitHubReleaseClient,
    resolve_repository,
)

DIGEST = "a" * 64


def asset_payload(**overrides):
    defaults = {
        "name": "fw.bin",
        "browser_download_url": (
            "https://github.com/owner/repo/releases/download/v1.1.0/fw.bin"
        ),
        "size": 10,
        "digest": f"sha256:{DIGEST}",
    }
    defaults.update(overrides)
    return defaults


def release_payload(**overrides):
    defaults = {
        "tag_name": "v1.1.0",
        "name": "1.1.0",
        "prerelease": False,
        "html_url": "https://github.com/owner/repo/releases/v1.1.0",
        "assets": [asset_payload()],
    }
    defaults.update(overrides)
    return defaults


def client_with(handler) -> GitHubReleaseClient:
    transport = httpx.MockTransport(handler)
    return GitHubReleaseClient(httpx.Client(transport=transport), "owner/repo")


class TestResolveRepository:
    def test_explicit_argument_wins(self):
        assert resolve_repository("a/b", {"ESP32OLED_REPO": "c/d"}) == "a/b"

    def test_environment_variable_used_when_no_argument(self):
        assert resolve_repository(None, {"ESP32OLED_REPO": "c/d"}) == "c/d"

    def test_default_repository_when_unconfigured(self):
        assert resolve_repository(None, {}) == DEFAULT_REPOSITORY
        assert DEFAULT_REPOSITORY == "jorgelserve/espCICD"

    @pytest.mark.parametrize("repo", ["", "nodashes", "/b", "a/", "a/b/c"])
    def test_invalid_repository_raises(self, repo):
        with pytest.raises(Esp32OledError) as excinfo:
            resolve_repository(repo, {})
        assert excinfo.value.error_code is ErrorCode.GITHUB_API


class TestLatestRelease:
    def test_returns_parsed_release(self):
        seen = []

        def handler(request: httpx.Request) -> httpx.Response:
            seen.append(request.url.path)
            return httpx.Response(200, json=release_payload())

        release = client_with(handler).get("latest")
        assert seen == ["/repos/owner/repo/releases/latest"]
        assert release.tag == "v1.1.0"
        assert release.channel == "stable"
        assert release.html_url.endswith("v1.1.0")
        (asset,) = release.assets
        assert asset.name == "fw.bin"
        assert asset.size == 10
        assert asset.sha256 == DIGEST
        assert asset.url.startswith("https://")

    def test_asset_without_digest_has_empty_sha256(self):
        payload = release_payload(assets=[asset_payload(digest=None)])

        release = client_with(lambda request: httpx.Response(200, json=payload)).get(
            "latest"
        )
        assert release.assets[0].sha256 == ""

    def test_no_releases_published_is_no_release(self):
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(404, json={"message": "Not Found"})

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("latest")
        assert excinfo.value.error_code is ErrorCode.NO_RELEASE

    def test_api_error_is_surfaced(self):
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(500, text="boom")

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("latest")
        assert excinfo.value.error_code is ErrorCode.GITHUB_API

    def test_malformed_json_is_api_error(self):
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, text="<html>not json</html>")

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("latest")
        assert excinfo.value.error_code is ErrorCode.GITHUB_API

    def test_payload_without_tag_name_is_api_error(self):
        payload = release_payload()
        payload.pop("tag_name")

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, json=payload)

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("latest")
        assert excinfo.value.error_code is ErrorCode.GITHUB_API

    @pytest.mark.parametrize(
        "assets", ["fw.bin", 42, True, {"name": "fw.bin"}, None]
    )
    def test_non_array_assets_is_api_error(self, assets):
        payload = release_payload(assets=assets)

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, json=payload)

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("latest")
        assert excinfo.value.error_code is ErrorCode.GITHUB_API

    @pytest.mark.parametrize(
        "entry", ["fw.bin", 42, True, ["fw.bin"], None]
    )
    def test_non_object_asset_entry_is_api_error(self, entry):
        payload = release_payload(assets=[entry])

        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, json=payload)

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("latest")
        assert excinfo.value.error_code is ErrorCode.GITHUB_API

    @pytest.mark.parametrize(
        "payload", ["v1.1.0", 42, True, ["v1.1.0"], None]
    )
    def test_non_object_release_payload_is_api_error(self, payload):
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(200, json=payload)

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("latest")
        assert excinfo.value.error_code is ErrorCode.GITHUB_API


class TestExplicitTag:
    def test_fetches_tag_endpoint(self):
        seen = []

        def handler(request: httpx.Request) -> httpx.Response:
            seen.append(request.url.path)
            return httpx.Response(200, json=release_payload(tag_name="v1.2.0"))

        release = client_with(handler).get("v1.2.0")
        assert seen == ["/repos/owner/repo/releases/tags/v1.2.0"]
        assert release.tag == "v1.2.0"

    def test_missing_tag_is_release_not_found(self):
        def handler(request: httpx.Request) -> httpx.Response:
            return httpx.Response(404, json={"message": "Not Found"})

        with pytest.raises(Esp32OledError) as excinfo:
            client_with(handler).get("v9.9.9")
        assert excinfo.value.error_code is ErrorCode.RELEASE_NOT_FOUND


class TestChannelFiltering:
    def list_handler(self, releases):
        payload = json.dumps(releases).encode()

        def handler(request: httpx.Request) -> httpx.Response:
            assert request.url.path == "/repos/owner/repo/releases"
            return httpx.Response(
                200, content=payload, headers={"content-type": "application/json"}
            )

        return handler

    def test_stable_channel_skips_prereleases(self):
        releases = [
            release_payload(tag_name="v2.0.0-beta.1", prerelease=True),
            release_payload(tag_name="v1.1.0"),
        ]
        release = client_with(self.list_handler(releases)).find(channel="stable")
        assert release.tag == "v1.1.0"

    def test_prerelease_channel_picks_prerelease(self):
        releases = [
            release_payload(tag_name="v2.0.0-beta.1", prerelease=True),
            release_payload(tag_name="v1.1.0"),
        ]
        release = client_with(self.list_handler(releases)).find(channel="beta")
        assert release.tag == "v2.0.0-beta.1"

    def test_empty_release_list_is_no_release(self):
        with pytest.raises(Esp32OledError) as excinfo:
            client_with(self.list_handler([])).find(channel="stable")
        assert excinfo.value.error_code is ErrorCode.NO_RELEASE

    def test_only_prereleases_and_stable_requested_is_no_release(self):
        releases = [release_payload(tag_name="v2.0.0-beta.1", prerelease=True)]
        with pytest.raises(Esp32OledError) as excinfo:
            client_with(self.list_handler(releases)).find(channel="stable")
        assert excinfo.value.error_code is ErrorCode.NO_RELEASE

    @pytest.mark.parametrize("entry", ["v1.1.0", 42, True, None])
    def test_non_object_release_entry_is_api_error(self, entry):
        with pytest.raises(Esp32OledError) as excinfo:
            client_with(self.list_handler([entry])).find(channel="stable")
        assert excinfo.value.error_code is ErrorCode.GITHUB_API
