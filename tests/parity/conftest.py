"""The sample Baselines that the tests of Parity Checks use, because they must work before real Baselines exist."""

import json
import shutil
from collections.abc import Callable
from pathlib import Path
from typing import Any

import pytest

from tests.capture import samples

SAMPLE_RELEASE = "156"


def sample_form(stem: str) -> dict[str, Any]:
    """Return a new copy of a real normal form of Chrome 156 on macOS. The stem is http1, http2 or http3."""
    return json.loads((samples.DATA / f"chrome_{stem}.normal.json").read_text())


type ChangeBaseline = Callable[[str, Callable[[dict[str, Any]], None]], None]
"""Changes the sample Baseline of a stem, in its file, with a function that edits a normal form in place."""


@pytest.fixture
def sample_baselines(tmp_path: Path) -> Path:
    """Return a directory of Baselines in the repository's layout, with no known-gaps list."""
    release = tmp_path / SAMPLE_RELEASE
    release.mkdir()
    for stem in ("http1", "http2"):
        shutil.copy(samples.DATA / f"chrome_{stem}.normal.json", release / f"{stem}.json")
    return tmp_path


@pytest.fixture
def change_baseline(sample_baselines: Path) -> ChangeBaseline:
    def change(stem: str, edit: Callable[[dict[str, Any]], None]) -> None:
        path = sample_baselines / SAMPLE_RELEASE / f"{stem}.json"
        form = json.loads(path.read_text())
        edit(form)
        path.write_text(json.dumps(form))

    return change
