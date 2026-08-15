import subprocess
import sys

import esp32oled_ci


def test_package_exposes_version() -> None:
    assert isinstance(esp32oled_ci.__version__, str)
    assert esp32oled_ci.__version__ == "0.1.0"


def test_cli_help_exits_zero() -> None:
    result = subprocess.run(
        [sys.executable, "-m", "esp32oled_ci.cli", "--help"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert result.returncode == 0
    assert "usage" in result.stdout.lower()
