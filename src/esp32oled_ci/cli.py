"""esp32oled-ci command line interface."""

import argparse
import sys

from esp32oled_ci import __version__


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="esp32oled-ci",
        description="Download and verify ESP32-C3 firmware packages.",
    )
    parser.add_argument("--version", action="version", version=__version__)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    parser.parse_args(argv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
