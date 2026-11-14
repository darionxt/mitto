"""Where the repository keeps the copy of web-platform-tests, Chrome's results and the list of differences."""

from pathlib import Path

__all__ = ["CHROME_FILE", "DIFFERENCES_FILE", "TESTS_FILE", "WPT_DIRECTORY", "WPT_ROOT"]

WPT_DIRECTORY = Path(__file__).parents[2] / "tests" / "wpt"

WPT_ROOT = WPT_DIRECTORY / "wpt"
"""The copy: a small root of web-platform-tests, with the tools that serve it."""

TESTS_FILE = WPT_DIRECTORY / "tests.json"
"""The copy's testharness.js tests, which the copy tool lists."""

CHROME_FILE = WPT_DIRECTORY / "chrome.json"
"""Chrome's results for them, which a recording on the Windows machine writes."""

DIFFERENCES_FILE = WPT_DIRECTORY / "differences.json"
"""Where a Page's results differ from Chrome's: the list that a run holds the Page to."""
