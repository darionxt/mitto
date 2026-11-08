"""What the release workflow checks before it builds, and the notes of the GitHub release it makes. See docs/building.md.

A tag vX.Y.Z publishes version X.Y.Z, which must be the version in pyproject.toml. A dry run publishes a pre-release to
TestPyPI instead, under a version of its own, because an index never takes the same version twice. The notes of a version
hold its entry in CHANGELOG.md, which says what changes for callers, as docs/releasing.md says.
"""

import re
import tomllib
from pathlib import Path
from typing import Any
from urllib.parse import urljoin

from mitto._persona import ENGINE_RELEASE_PERSONA, WINDOWS_PERSONAS
from tools.release.window import recorded_build

__all__ = ["REPOSITORY", "PublishError", "dry_run_version", "project_version", "release_notes", "tag_version"]

REPOSITORY = Path(__file__).parents[2]

_PYPROJECT = Path("pyproject.toml")
_CHROMIUM_TAG = Path("tools/workspace/chromium_tag")
_BASELINES = Path("tests/baselines")
_CHANGELOG = Path("CHANGELOG.md")

_NUMBER = r"(?:0|[1-9][0-9]*)"
_RELEASE = rf"{_NUMBER}\.{_NUMBER}\.{_NUMBER}"
_TAG = re.compile(rf"v(?P<version>{_RELEASE})")
# PEP 440's normal form, in which the names of the files hold the version as it was given.
_PRE_RELEASE = re.compile(rf"(?P<release>{_RELEASE})(?P<pre>(?:a|b|rc){_NUMBER})?(?P<dev>\.dev{_NUMBER})?")
# A heading of CHANGELOG.md, which starts the entry of the version it names.
_ENTRY_HEADING = re.compile(r"(?m)^## (.*)$")
# An inline link of Markdown, whose target the notes resolve from CHANGELOG.md at the version's tag.
_LINK = re.compile(r"\]\((?P<target>[^)\s]+)\)")


class PublishError(Exception):
    """What a release would publish is not right, so nothing should build."""


def project_version(root: Path = REPOSITORY) -> str:
    """Return the version in pyproject.toml.

    Raises:
        PublishError: The file names no version.
    """
    version = _project(root).get("version")
    if not isinstance(version, str):
        raise PublishError(f"{_PYPROJECT} names no version of the project")
    return version


def tag_version(tag: str, root: Path = REPOSITORY) -> str:
    """Return the version that a tag publishes.

    Raises:
        PublishError: The tag is not vX.Y.Z, or not the version in pyproject.toml.
    """
    found = _TAG.fullmatch(tag)
    if found is None:
        raise PublishError(f"the tag {tag} is not vX.Y.Z")
    version = project_version(root)
    if found["version"] != version:
        raise PublishError(f"the tag {tag} is not the version in {_PYPROJECT}, {version}")
    return version


def dry_run_version(version: str) -> str:
    """Return the version that a dry run publishes, once it is known to be a pre-release, such as 0.1.0rc1 or 0.1.0.dev1.

    Raises:
        PublishError: The version is not a pre-release, or not written in PEP 440's normal form.
    """
    found = _PRE_RELEASE.fullmatch(version)
    if found is None or not (found["pre"] or found["dev"]):
        raise PublishError(f"a dry run publishes a pre-release, such as 0.1.0rc1 or 0.1.0.dev1, and not {version}")
    return version


def release_notes(version: str, root: Path = REPOSITORY) -> str:
    """Return the notes of the GitHub release of a version, or of the release that a pre-release comes before.

    They name the Supported Window and the Engine Release, and hold the version's entry in CHANGELOG.md, which says what
    changes for callers. A relative link in the entry leads to the repository at the version's tag.

    Raises:
        PublishError: The version is not one, CHANGELOG.md has no entry for it or a heading that does not name one
            version once, the Engine Release is not the newest Chrome Release, the engine is built from another
            milestone, or the Baselines of a Chrome Release do not name their build.
    """
    changes = _changes(version, root)
    window = sorted(WINDOWS_PERSONAS, key=int)
    engine = next(release for release, persona in WINDOWS_PERSONAS.items() if persona == ENGINE_RELEASE_PERSONA)
    if engine != window[-1]:
        raise PublishError(f"the Engine Release is Chrome {engine}, and the newest Persona is of Chrome {window[-1]}")
    engine_build = (root / _CHROMIUM_TAG).read_text(encoding="utf-8").strip()
    if engine_build.split(".")[0] != engine:
        raise PublishError(f"the engine is built from Chromium {engine_build}, which is not Chrome {engine}")
    builds: dict[str, str] = {}
    for release in window:
        build = recorded_build(release, root / _BASELINES)
        if build is None:
            raise PublishError(f"the Baselines of Chrome {release} do not name the build that recorded them")
        builds[release] = build
    at_tag = f"{_project(root)['urls']['Source']}/blob/v{version}/"
    building = urljoin(at_tag, "docs/building.md#build-a-wheel")
    changelog = urljoin(at_tag, _CHANGELOG.as_posix())

    # GitHub shows each line of a release's notes on a line of its own, so a paragraph is one line.
    paragraphs = [
        f"mitto {version} presents Chrome {', '.join(window[:-1])} and {window[-1]}, its Supported Window, "
        f"with Chrome {engine} as its Engine Release. Its network code is built from Chromium {engine_build}.",
        _LINK.sub(lambda link: f"]({urljoin(changelog, link['target'])})", changes),
        "Each Persona presents the build of Chrome that recorded its Baselines:\n\n"
        + "\n".join(f"- Chrome {release}: {build}" for release, build in builds.items()),
        "Install it on macOS on arm64 or Linux on x86_64, with Python 3.12 or later:",
        f"```bash\npip install mitto=={version}\n```",
        "The wheels here are the ones on PyPI. Each symbols archive holds what stripping removed from the library in "
        "the wheel of the same name, to turn an address from a crash report into a function name "
        f"([building.md]({building})). `SHA256SUMS` holds the SHA-256 of each file.",
    ]
    return "\n\n".join(paragraphs) + "\n"


def _changes(version: str, root: Path) -> str:
    """Return the entry of CHANGELOG.md for a version, or for the release that a pre-release comes before.

    Raises:
        PublishError: The version is not one, CHANGELOG.md has no entry for it, or a heading of CHANGELOG.md is not a
            version or names one twice.
    """
    found = _PRE_RELEASE.fullmatch(version)
    if found is None:
        raise PublishError(f"{version} is not a version, such as 0.1.0 or 0.1.0rc1")
    release = found["release"]
    parts = _ENTRY_HEADING.split((root / _CHANGELOG).read_text(encoding="utf-8"))
    entries: dict[str, str] = {}
    # The text before the first heading says what the file is, and belongs to no version.
    for heading, entry in zip(parts[1::2], parts[2::2], strict=True):
        if re.fullmatch(_RELEASE, heading) is None:
            raise PublishError(f"{_CHANGELOG} has a heading {heading!r}, where each names a version, such as 0.1.0")
        if heading in entries:
            raise PublishError(f"{_CHANGELOG} has two entries for {heading}")
        entries[heading] = entry.strip()
    if not entries.get(release):
        raise PublishError(
            f"{_CHANGELOG} has no entry for {release}, and every version needs one that says what changes for "
            "callers (docs/releasing.md)"
        )
    return entries[release]


def _project(root: Path) -> dict[str, Any]:
    with (root / _PYPROJECT).open("rb") as file:
        return tomllib.load(file).get("project", {})
