"""GitHub release lookup for firmware packages."""

from collections.abc import Mapping

import httpx

from esp32oled_ci.errors import ErrorCode, Esp32OledError
from esp32oled_ci.models import FirmwareAsset, ReleaseInfo

DEFAULT_REPOSITORY = "jorgelserve/espCICD"
REPOSITORY_ENV_VAR = "ESP32OLED_REPO"

_API_BASE = "https://api.github.com"
_ACCEPT_HEADERS = {"Accept": "application/vnd.github+json"}


def resolve_repository(repo: str | None, env: Mapping[str, str]) -> str:
    """Resolve the target repository from an explicit value, env, or default."""
    raw = repo if repo is not None else env.get(REPOSITORY_ENV_VAR)
    if raw is None:
        return DEFAULT_REPOSITORY
    parts = raw.split("/")
    if len(parts) != 2 or not all(part.strip() for part in parts):
        raise Esp32OledError(
            ErrorCode.GITHUB_API,
            f"invalid GitHub repository {raw!r}; expected 'owner/name'",
        )
    return raw


def parse_release(payload: object) -> ReleaseInfo:
    """Parse a GitHub API release payload into a ReleaseInfo."""
    if not isinstance(payload, dict):
        raise Esp32OledError(ErrorCode.GITHUB_API, "release payload is not an object")
    tag = payload.get("tag_name")
    if not isinstance(tag, str) or not tag.strip():
        raise Esp32OledError(ErrorCode.GITHUB_API, "release payload lacks 'tag_name'")

    raw_assets = payload.get("assets", [])
    if not isinstance(raw_assets, list):
        raise Esp32OledError(
            ErrorCode.GITHUB_API, "'assets' is not an array"
        )
    assets = []
    try:
        for entry in raw_assets:
            if not isinstance(entry, dict):
                raise Esp32OledError(
                    ErrorCode.GITHUB_API, "release asset entry is not an object"
                )
            digest = entry.get("digest")
            sha256 = (
                digest.split(":", 1)[1]
                if isinstance(digest, str) and digest.startswith("sha256:")
                else ""
            )
            assets.append(
                FirmwareAsset(
                    name=entry.get("name", ""),
                    url=entry.get("browser_download_url", ""),
                    size=entry.get("size", 0),
                    sha256=sha256,
                )
            )
    except (Esp32OledError, KeyError, TypeError) as exc:
        raise Esp32OledError(
            ErrorCode.GITHUB_API, f"malformed release asset payload: {exc}"
        ) from exc

    prerelease = bool(payload.get("prerelease"))
    return ReleaseInfo(
        tag=tag,
        name=payload.get("name") or tag,
        channel="beta" if prerelease else "stable",
        assets=tuple(assets),
        html_url=payload.get("html_url", ""),
    )


def require_asset(release: ReleaseInfo, name: str) -> FirmwareAsset:
    """Return the named asset of a release or raise ASSET_NOT_FOUND."""
    asset = release.asset_named(name)
    if asset is None:
        raise Esp32OledError(
            ErrorCode.ASSET_NOT_FOUND,
            f"release {release.tag} has no asset named {name!r}",
        )
    return asset


class GitHubReleaseClient:
    """Read-only access to the GitHub Releases API for one repository."""

    def __init__(self, client: httpx.Client, repository: str) -> None:
        self._client = client
        self._repository = repository

    def get(self, version: str = "latest") -> ReleaseInfo:
        if version == "latest":
            return self.get_latest()
        return self.get_by_tag(version)

    def get_latest(self) -> ReleaseInfo:
        response = self._request(f"/repos/{self._repository}/releases/latest")
        if response.status_code == 404:
            raise Esp32OledError(
                ErrorCode.NO_RELEASE, f"repository {self._repository} has no releases"
            )
        return parse_release(self._ok_payload(response))

    def get_by_tag(self, tag: str) -> ReleaseInfo:
        response = self._request(f"/repos/{self._repository}/releases/tags/{tag}")
        if response.status_code == 404:
            raise Esp32OledError(
                ErrorCode.RELEASE_NOT_FOUND, f"release {tag!r} not found"
            )
        return parse_release(self._ok_payload(response))

    def find(self, channel: str = "stable") -> ReleaseInfo:
        """Pick the newest release matching a channel ('stable' or pre-release)."""
        response = self._request(f"/repos/{self._repository}/releases")
        releases = self._ok_payload(response)
        if not isinstance(releases, list):
            raise Esp32OledError(ErrorCode.GITHUB_API, "release list is not an array")
        want_prerelease = channel != "stable"
        for payload in releases:
            if not isinstance(payload, dict):
                raise Esp32OledError(
                    ErrorCode.GITHUB_API, "release list entry is not an object"
                )
            if bool(payload.get("prerelease")) is want_prerelease:
                return parse_release(payload)
        raise Esp32OledError(
            ErrorCode.NO_RELEASE, f"no {channel} release in {self._repository}"
        )

    def _request(self, path: str) -> httpx.Response:
        try:
            return self._client.get(f"{_API_BASE}{path}", headers=_ACCEPT_HEADERS)
        except httpx.HTTPError as exc:
            raise Esp32OledError(
                ErrorCode.GITHUB_API, f"GitHub API request failed: {exc}"
            ) from exc

    def _ok_payload(self, response: httpx.Response) -> object:
        if response.status_code >= 400:
            raise Esp32OledError(
                ErrorCode.GITHUB_API,
                f"GitHub API returned HTTP {response.status_code}",
            )
        try:
            return response.json()
        except ValueError as exc:
            raise Esp32OledError(
                ErrorCode.GITHUB_API, "GitHub API returned malformed JSON"
            ) from exc
