"""Which ticket owns each difference in the list: the one whose missing behaviour causes it, which empties it.

A line of the list names its owner only where that is not the ticket of the test's part of M8, such as #283 for a test
of `encoding/` that needs XHR. A ticket closes once it owns no difference. A difference whose behaviour the roadmap or
#259 puts in a later milestone belongs to that milestone, such as "M9", and one whose behaviour the roadmap leaves out
belongs to "out": these stay listed past M8, and count apart from M8's tickets.
"""

import dataclasses
from collections.abc import Iterable, Sequence

from tools.wpt.differences import OWNER, TICKET, Difference
from tools.wpt.parts import part_of

__all__ = ["LEFT_OUT", "NO_OWNER", "is_ticket", "keep_owners", "move", "owner_of"]

NO_OWNER = "none"
"""The owner of a difference of a test in no part of M8, which no line has given to a ticket."""

LEFT_OUT = "out"
"""The owner of a difference whose behaviour the roadmap leaves out, such as media playback or service workers."""


def is_ticket(owner: str) -> bool:
    """Whether an owner is one of the tickets, such as "#283", rather than a later milestone, "out" or NO_OWNER."""
    return TICKET.fullmatch(owner) is not None


def owner_of(difference: Difference) -> str:
    """What owns a difference: the owner its line names, else the ticket of its test's part, else NO_OWNER."""
    if difference.owner is not None:
        return difference.owner
    part = part_of(difference.test)
    return NO_OWNER if part is None else part.ticket


def _as_listed(difference: Difference, owner: str | None) -> Difference:
    """A difference with an owner, which its line names only where that is not the ticket of the test's part."""
    part = part_of(difference.test)
    if owner is not None and part is not None and owner == part.ticket:
        owner = None
    return dataclasses.replace(difference, owner=owner)


def keep_owners(written: Iterable[Difference], listed: Iterable[Difference]) -> list[Difference]:
    """Give each difference of a list that is written again the owner of the same line in the list as it was, else of
    the line it replaces, for the same subtest, else none of its own, so that it has the ticket of its test's part."""
    exact: dict[Difference, str] = {}
    by_place: dict[tuple[str, str | None], set[str]] = {}
    for difference in listed:
        if difference.owner is not None:
            exact[difference] = difference.owner
            by_place.setdefault((difference.test, difference.subtest), set()).add(difference.owner)
    kept: list[Difference] = []
    for difference in written:
        owner = exact.get(difference)
        if owner is None:
            owners = by_place.get((difference.test, difference.subtest), set())
            # A whole test has two places, its harness status and its number of subtests: only one owner is certain.
            owner = next(iter(owners)) if len(owners) == 1 else None
        kept.append(_as_listed(difference, owner))
    return kept


def move(
    listed: Sequence[Difference], owner: str, prefixes: Sequence[str] = (), outcome: str | None = None
) -> tuple[list[Difference], int]:
    """Give an owner, a ticket, a later milestone or "out", the differences of the tests whose address starts with one
    of the prefixes, if any, and whose outcome in a Page contains `outcome`, if given. Moving them to the ticket of
    their test's part takes away the owner that their lines named. Return the list and how many differences moved.

    Raises:
        ValueError: The owner is not one such as "#283", "M9" or "out", or neither prefixes nor an outcome say what to
            move.
    """
    if not OWNER.fullmatch(owner):
        raise ValueError(f'{owner!r} is not an owner such as "#283", "M9" or "out"')
    if not prefixes and outcome is None:
        raise ValueError("say which differences to move: the start of their tests' addresses, an outcome, or both")
    moved = 0
    result: list[Difference] = []
    for difference in listed:
        chosen = (not prefixes or difference.test.startswith(tuple(prefixes))) and (
            outcome is None or outcome in (difference.mitto or "")
        )
        if chosen and owner_of(difference) != owner:
            difference = _as_listed(difference, owner)
            moved += 1
        result.append(difference)
    return result, moved
