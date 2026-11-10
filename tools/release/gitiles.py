"""Reading Chromium's repositories without a checkout, from the JSON that their Gitiles servers give.

Chromium's own repository and every dependency that it holds as a git submodule have a server on googlesource.com, which
answers what changed between two commits or tags, file by file, and what a file holds at a tag.
"""

import base64
import binascii
import re
import urllib.parse
from dataclasses import dataclass
from typing import Any, cast

from tools.release.web import Fetch, WebError, json_answer

__all__ = ["CHROMIUM_URL", "Change", "Commit", "commits_between", "submodule_urls"]

CHROMIUM_URL = "https://chromium.googlesource.com/chromium/src"

# Gitiles puts this before its JSON, so that a page cannot load the answer as a script.
_XSSI_PREFIX = b")]}'"

# The mode that git gives a submodule, a commit of another repository, in a tree.
_SUBMODULE_MODE = 0o160000

# Far more pages than a patch build of one milestone takes, so that an answer that never ends stops.
_MAX_PAGES = 20
_PAGE_SIZE = 1000

# A line of a commit message that names the bugs it fixes, such as "Bug: 578797822,572055997" or, in the description
# that a cherry-pick quotes, "> Fixed: 572055997".
_BUG_LINE = re.compile(r"^[> ]*(?:Bug|Fixed):(.*)$", re.MULTILINE)
_BUG_NUMBER = re.compile(r"\b\d{5,}\b")

# A revert says which commit it reverts on a line of its own. Its subject starts with Revert "...", after any tags such
# as [M157], once for a revert and twice for a revert of a revert, which brings the change back.
_REVERT_LINE = re.compile(r"^This reverts commit [0-9a-f]{7,40}\b", re.MULTILINE)
_TAGS = re.compile(r"^(?:\[[^\]]*\]\s*)*")
_REVERT_PREFIX = 'Revert "'

_SUBMODULE = re.compile(r"^\s*path\s*=\s*(\S+)\s*\n\s*url\s*=\s*(\S+)\s*$", re.MULTILINE)


@dataclass(frozen=True, slots=True)
class Change:
    """What a commit does to one path of its repository.

    Attributes:
        path: The path after the commit, or before it if the commit deletes it.
        old_path: The path before the commit, which differs from path for a rename.
        old_id: What the path held before, or all zeros if the commit adds it. For a submodule, its commit.
        new_id: What the path holds after, or all zeros if the commit deletes it. For a submodule, its commit.
        submodule: Whether the path is a submodule, so that its ids are commits of another repository.
    """

    path: str
    old_path: str
    old_id: str
    new_id: str
    submodule: bool


@dataclass(frozen=True, slots=True)
class Commit:
    """A commit, and what it changes.

    Attributes:
        id: Its hash.
        subject: The first line of its message.
        bugs: The bugs that its message names on Bug: or Fixed: lines, in their order.
        changes: What it changes, path by path.
        reverts: Whether it takes a change out again, so that the bugs it names are those of the change it reverts.
    """

    id: str
    subject: str
    bugs: tuple[str, ...]
    changes: tuple[Change, ...]
    reverts: bool = False


def commits_between(repository: str, old: str, new: str, fetch: Fetch) -> list[Commit]:
    """Return the commits that new has and old has not, newest first, with what each changes.

    Args:
        repository: The repository's address on a Gitiles server, such as CHROMIUM_URL.
        old: A commit or tag.
        new: A commit or tag.
        fetch: What asks the server.

    Raises:
        WebError: The server did not answer, or its answer cannot be read.
    """
    found: list[Commit] = []
    start = ""
    for _ in range(_MAX_PAGES):
        query = {"format": "JSON", "name-status": "1", "n": str(_PAGE_SIZE)} | ({"s": start} if start else {})
        url = f"{_address(repository)}/+log/{old}..{new}?{urllib.parse.urlencode(query)}"
        answer = _gitiles_json(fetch(url), url)
        entries: Any = cast(dict[str, Any], answer).get("log") if isinstance(answer, dict) else None
        if not isinstance(entries, list):
            raise WebError(f"{url} did not give a log")
        found += [_commit(entry, url) for entry in cast(list[Any], entries)]
        start = cast(dict[str, Any], answer).get("next") or ""
        if not isinstance(start, str):
            raise WebError(f"{url} gave a next page that is not text: {start!r}")
        if not start:
            return found
    raise WebError(f"{repository} gave more than {_MAX_PAGES} pages of commits between {old} and {new}")


def submodule_urls(repository: str, ref: str, fetch: Fetch) -> dict[str, str]:
    """Return the address of each submodule that a commit or tag of a repository names, by its path.

    Raises:
        WebError: The server did not answer, or its answer is not a .gitmodules file.
    """
    url = f"{_address(repository)}/+/{ref}/.gitmodules?format=TEXT"
    try:
        text = base64.b64decode(fetch(url), validate=False).decode()
    except (binascii.Error, UnicodeDecodeError) as error:
        raise WebError(f"{url} did not give a file: {error}") from error
    urls = {path: _address(address) for path, address in _SUBMODULE.findall(text)}
    if not urls:
        raise WebError(f"{url} names no submodule")
    return urls


def _address(repository: str) -> str:
    """Return a repository's address as its Gitiles server takes it, without a trailing .git."""
    if urllib.parse.urlsplit(repository).scheme != "https" or not repository.split("/")[2].endswith(
        ".googlesource.com"
    ):
        raise WebError(f"{repository} is not a repository on googlesource.com, whose servers answer in Gitiles' JSON")
    return repository.removesuffix("/").removesuffix(".git")


def _gitiles_json(body: bytes, url: str) -> Any:
    return json_answer(body.removeprefix(_XSSI_PREFIX), url)


def _commit(entry: Any, url: str) -> Commit:
    if not isinstance(entry, dict):
        raise WebError(f"{url} gave a commit that is not an object: {entry!r}")
    entry = cast(dict[str, Any], entry)
    commit, message, tree_diff = entry.get("commit"), entry.get("message"), entry.get("tree_diff", [])
    if not isinstance(commit, str) or not isinstance(message, str) or not isinstance(tree_diff, list):
        raise WebError(f"{url} gave a commit without its hash, message or changes: {entry.get('commit')!r}")
    bugs = [number for line in _BUG_LINE.findall(message) for number in _BUG_NUMBER.findall(line)]
    subject = message.split("\n", 1)[0]
    return Commit(
        id=commit,
        subject=subject,
        bugs=tuple(dict.fromkeys(bugs)),
        changes=tuple(_change(change, url) for change in cast(list[Any], tree_diff)),
        reverts=_reverts(subject, message),
    )


def _reverts(subject: str, message: str) -> bool:
    """Return whether a commit takes a change out: a revert, but not a revert of a revert."""
    if not _REVERT_LINE.search(message):
        return False
    rest, prefixes = _TAGS.sub("", subject, count=1), 0
    while rest.startswith(_REVERT_PREFIX):
        rest, prefixes = rest.removeprefix(_REVERT_PREFIX), prefixes + 1
    return prefixes % 2 == 1 or prefixes == 0


def _change(entry: Any, url: str) -> Change:
    fields = cast(dict[str, Any], entry) if isinstance(entry, dict) else {}
    kind, old_path, new_path = fields.get("type"), fields.get("old_path"), fields.get("new_path")
    old_id, new_id = fields.get("old_id"), fields.get("new_id")
    if not all(isinstance(value, str) for value in (kind, old_path, new_path, old_id, new_id)):
        raise WebError(f"{url} gave a change that is not a path's change: {entry!r}")
    modes = (fields.get("old_mode"), fields.get("new_mode"))
    return Change(
        path=cast(str, old_path if kind == "delete" else new_path),
        old_path=cast(str, old_path),
        old_id=cast(str, old_id),
        new_id=cast(str, new_id),
        submodule=_SUBMODULE_MODE in modes,
    )
