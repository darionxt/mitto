"""Which Windows stable build of each Chrome Release a recording uses: the newest that Google's version history can pin.

Asked for a Chrome Release, Google's update server gives the newest build that can be pinned, so a recording uses that
one. A newer build of a partial rollout is not it, and a Chrome Release that has only partial rollouts so far has none.
See docs/baselines.md.
"""

import urllib.parse
from collections.abc import Iterable
from dataclasses import dataclass
from datetime import date, datetime
from typing import Any, cast

from tools.release.web import Fetch, WebError, fetch_from_the_network, json_answer, version_parts

__all__ = ["RELEASES_URL", "StableBuild", "newest_pinnable_builds", "stable_builds"]

RELEASES_URL = "https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions/all/releases"

# Far more pages than three Chrome Releases' pinnable builds take, so that an answer that never ends stops.
_MAX_PAGES = 20


@dataclass(frozen=True, slots=True)
class StableBuild:
    """A Windows stable build of a Chrome Release, as Google's version history gives it.

    Attributes:
        version: The build, such as "157.0.8103.39".
        first_served: The first day that stable Chrome got it.
        rollout: The largest share of stable installations it has gone to, from 0 to 1.
        pinnable: Whether it can be pinned, so that Google's update server gives it for its Chrome Release.
    """

    version: str
    first_served: date
    rollout: float
    pinnable: bool


def newest_pinnable_builds(releases: Iterable[str], fetch: Fetch = fetch_from_the_network) -> dict[str, str | None]:
    """Return the newest Windows stable build of each Chrome Release that can be pinned, or None if it has none.

    Args:
        releases: The Chrome Releases, such as ["156", "157", "158"].
        fetch: What asks Google's version history.

    Raises:
        WebError: The version history did not answer, or its answer cannot be read.
    """
    wanted = sorted({int(release) for release in releases})
    query = {"filter": f"version>={wanted[0]},version<{wanted[-1] + 1},pinnable=true"}
    newest: dict[int, tuple[int, ...]] = {}
    for entry in _releases(query, fetch):
        version = entry.get("version")
        if not isinstance(version, str):
            raise WebError(f"{RELEASES_URL} gave a release without a version: {entry}")
        parts = version_parts(version, RELEASES_URL)
        if entry.get("pinnable") is True and parts[0] in wanted:
            newest[parts[0]] = max(newest.get(parts[0], parts), parts)
    return {
        str(release): ".".join(map(str, newest[release])) if release in newest else None
        for release in sorted(int(release) for release in releases)
    }


def stable_builds(release: str, fetch: Fetch = fetch_from_the_network) -> list[StableBuild]:
    """Return every Windows stable build of a Chrome Release, partial rollouts included, oldest first.

    Raises:
        WebError: The version history did not answer, or its answer cannot be read.
    """
    query = {"filter": f"version>={release},version<{int(release) + 1}"}
    builds: dict[tuple[int, ...], list[dict[str, Any]]] = {}
    for entry in _releases(query, fetch):
        version = entry.get("version")
        if not isinstance(version, str):
            raise WebError(f"{RELEASES_URL} gave a release without a version: {entry}")
        parts = version_parts(version, RELEASES_URL)
        if parts[0] == int(release):
            builds.setdefault(parts, []).append(entry)
    return [_stable_build(".".join(map(str, parts)), builds[parts]) for parts in sorted(builds)]


def _stable_build(version: str, releases: list[dict[str, Any]]) -> StableBuild:
    """Return what the version history's releases of one build say of it."""
    starts: list[date] = []
    fractions: list[float] = []
    for release in releases:
        serving: Any = release.get("serving")
        start: Any = cast(dict[str, Any], serving).get("startTime") if isinstance(serving, dict) else None
        fraction: Any = release.get("fraction", 0)
        try:
            starts.append(datetime.fromisoformat(cast(str, start)).date())
        except (TypeError, ValueError) as error:
            raise WebError(f"{RELEASES_URL} gave a release of {version} without its start: {release}") from error
        if not isinstance(fraction, int | float):
            raise WebError(f"{RELEASES_URL} gave a release of {version} whose share is not a number: {release}")
        fractions.append(float(fraction))
    return StableBuild(
        version=version,
        first_served=min(starts),
        rollout=max(fractions),
        pinnable=any(release.get("pinnable") is True for release in releases),
    )


def _releases(query: dict[str, str], fetch: Fetch) -> list[dict[str, Any]]:
    """Return every release that the version history gives for a query, over all of its pages."""
    found: list[dict[str, Any]] = []
    token = ""
    for _ in range(_MAX_PAGES):
        url = f"{RELEASES_URL}?{urllib.parse.urlencode({**query, 'pageToken': token} if token else query)}"
        answer = json_answer(fetch(url), url)
        entries: Any = cast(dict[str, Any], answer).get("releases", []) if isinstance(answer, dict) else None
        if not isinstance(entries, list) or not all(isinstance(entry, dict) for entry in cast(list[Any], entries)):
            raise WebError(f"{url} did not give a list of releases")
        found += cast(list[dict[str, Any]], entries)
        token = cast(dict[str, Any], answer).get("nextPageToken") or ""
        if not isinstance(token, str):
            raise WebError(f"{url} gave a next page that is not text: {token!r}")
        if not token:
            return found
    raise WebError(f"{RELEASES_URL} gave more than {_MAX_PAGES} pages for {query['filter']}")
