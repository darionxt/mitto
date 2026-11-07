"""Which Windows stable build of each Chrome Release a recording uses: the newest that Google's version history can pin.

Asked for a Chrome Release, Google's update server gives the newest build that can be pinned, so a recording uses that
one. A newer build of a partial rollout is not it, and a Chrome Release that has only partial rollouts so far has none.
See docs/baselines.md.
"""

import urllib.parse
from collections.abc import Iterable
from typing import Any, cast

from tools.release.web import Fetch, WebError, fetch_from_the_network, json_answer, version_parts

__all__ = ["RELEASES_URL", "newest_pinnable_builds"]

RELEASES_URL = "https://versionhistory.googleapis.com/v1/chrome/platforms/win64/channels/stable/versions/all/releases"

# Far more pages than three Chrome Releases' pinnable builds take, so that an answer that never ends stops.
_MAX_PAGES = 20


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
