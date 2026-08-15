"""Streaming HTTPS downloader with atomic publication of verified files."""

import hashlib
import os
import tempfile
from pathlib import Path

import httpx

from esp32oled_ci.errors import ErrorCode, Esp32OledError

DEFAULT_MAX_BYTES = 8 * 1024 * 1024
_CHUNK_SIZE = 65536


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(_CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download(
    client: httpx.Client,
    url: str,
    destination: Path,
    *,
    expected_size: int | None = None,
    expected_sha256: str | None = None,
    max_bytes: int = DEFAULT_MAX_BYTES,
) -> Path:
    """Download ``url`` to ``destination`` atomically.

    Bytes stream into a temporary file next to the destination. The file is
    renamed into place only after size and digest verification succeed, so a
    failed or tampered download never replaces an existing verified file.
    """
    if not isinstance(url, str) or not url.startswith("https://"):
        raise ValueError(f"only HTTPS URLs are supported: {url!r}")

    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(
        dir=destination.parent, prefix=f".{destination.name}.", suffix=".part"
    )
    temp_path = Path(temp_name)
    digest = hashlib.sha256()
    total = 0

    try:
        with os.fdopen(fd, "wb") as handle, client.stream("GET", url) as response:
            if response.url.scheme != "https":
                raise Esp32OledError(
                    ErrorCode.GITHUB_API,
                    f"refusing download from non-HTTPS URL: {response.url}",
                )
            if response.status_code >= 400:
                raise Esp32OledError(
                    ErrorCode.GITHUB_API,
                    f"download failed with HTTP {response.status_code}",
                )
            for chunk in response.iter_bytes(chunk_size=_CHUNK_SIZE):
                total += len(chunk)
                if total > max_bytes:
                    raise Esp32OledError(
                        ErrorCode.GITHUB_API,
                        f"asset exceeds maximum allowed size {max_bytes}",
                    )
                digest.update(chunk)
                handle.write(chunk)

        if expected_size is not None and total != expected_size:
            raise Esp32OledError(
                ErrorCode.CHECKSUM_MISMATCH,
                f"downloaded {total} bytes, manifest declares {expected_size}",
            )
        actual_digest = digest.hexdigest()
        if expected_sha256 is not None and actual_digest != expected_sha256:
            raise Esp32OledError(
                ErrorCode.CHECKSUM_MISMATCH,
                f"SHA-256 mismatch: got {actual_digest}, expected {expected_sha256}",
            )

        os.replace(temp_path, destination)
        return destination
    except httpx.HTTPError as exc:
        raise Esp32OledError(ErrorCode.GITHUB_API, f"download failed: {exc}") from exc
    finally:
        temp_path.unlink(missing_ok=True)
