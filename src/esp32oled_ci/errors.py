"""Error taxonomy with distinct process exit codes."""

import enum
from collections.abc import Mapping


class ErrorCode(enum.Enum):
    """Stable error categories surfaced to the CLI user."""

    NO_RELEASE = "NO_RELEASE"
    RELEASE_NOT_FOUND = "RELEASE_NOT_FOUND"
    ASSET_NOT_FOUND = "ASSET_NOT_FOUND"
    MANIFEST_INVALID = "MANIFEST_INVALID"
    BOARD_MISMATCH = "BOARD_MISMATCH"
    CHECKSUM_MISMATCH = "CHECKSUM_MISMATCH"
    SIGNATURE_INVALID = "SIGNATURE_INVALID"
    DEVICE_NOT_FOUND = "DEVICE_NOT_FOUND"
    FLASH_FAILED = "FLASH_FAILED"
    VERIFY_FAILED = "VERIFY_FAILED"
    UNSUPPORTED_OTA = "UNSUPPORTED_OTA"
    GITHUB_API = "GITHUB_API"

    @property
    def exit_code(self) -> int:
        return _EXIT_CODES[self]


_EXIT_CODES: Mapping[ErrorCode, int] = {
    ErrorCode.NO_RELEASE: 10,
    ErrorCode.RELEASE_NOT_FOUND: 11,
    ErrorCode.ASSET_NOT_FOUND: 12,
    ErrorCode.MANIFEST_INVALID: 13,
    ErrorCode.BOARD_MISMATCH: 14,
    ErrorCode.CHECKSUM_MISMATCH: 15,
    ErrorCode.SIGNATURE_INVALID: 16,
    ErrorCode.DEVICE_NOT_FOUND: 17,
    ErrorCode.FLASH_FAILED: 18,
    ErrorCode.VERIFY_FAILED: 19,
    ErrorCode.UNSUPPORTED_OTA: 20,
    ErrorCode.GITHUB_API: 21,
}


class Esp32OledError(Exception):
    """Domain error carrying a stable ErrorCode and its process exit code."""

    def __init__(self, error_code: ErrorCode, message: str) -> None:
        self.error_code = error_code
        self.message = message
        super().__init__(f"{error_code.name}: {message}")

    @property
    def exit_code(self) -> int:
        return _EXIT_CODES[self.error_code]
