"""Which ticket owns each difference in the list: the one whose missing behaviour causes it, which empties it.

A line of the list names its owner only where that is not the ticket of the test's part of M8, such as #283 for a test
of `encoding/` that needs XHR. A ticket closes once it owns no difference.
"""

import dataclasses
from collections.abc import Iterable, Sequence

from tools.wpt.differences import TICKET, Difference
from tools.wpt.parts import part_of

__all__ = ["NO_OWNER", "keep_owners", "move", "owner_of"]

NO_OWNER = "none"
"""The owner of a difference of a test in no part of M8, which no line has given to a ticket."""


def owner_of(difference: Difference) -> str:
    """The ticket that owns a difference: the one its line names, else the ticket of its test's part, else NO_OWNER."""
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
    listed: Sequence[Difference], ticket: str, prefixes: Sequence[str] = (), outcome: str | None = None
) -> tuple[list[Difference], int]:
    """Give a ticket the differences of the tests whose address starts with one of the prefixes, if any, and whose
    outcome in a Page contains `outcome`, if given. Moving them to the ticket of their test's part takes away the owner
    that their lines named. Return the list and how many differences moved.

    Raises:
        ValueError: The ticket is not one such as "#283", or neither prefixes nor an outcome say what to move.
    """
    if not TICKET.fullmatch(ticket):
        raise ValueError(f'{ticket!r} is not a ticket such as "#283"')
    if not prefixes and outcome is None:
        raise ValueError("say which differences to move: the start of their tests' addresses, an outcome, or both")
    moved = 0
    result: list[Difference] = []
    for difference in listed:
        chosen = (not prefixes or difference.test.startswith(tuple(prefixes))) and (
            outcome is None or outcome in (difference.mitto or "")
        )
        if chosen and owner_of(difference) != ticket:
            difference = _as_listed(difference, ticket)
            moved += 1
        result.append(difference)
    return result, moved
