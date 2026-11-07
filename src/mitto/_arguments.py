"""Checks of what a caller passes, so that an argument of the wrong type raises a TypeError that names it.

Nothing is converted, as ADR-0004 leaves the Python layer only to describe requests. A message names the argument, what it
takes and the type it got, and leaves out the value, which may be a secret.
"""

import os
from types import UnionType
from typing import cast

__all__ = ["require_path", "require_type"]


def require_type(argument: str, value: object, expected: type | UnionType, what: str) -> None:
    """Refuse `value` as `argument` unless it is an instance of `expected`, which `what` says in words.

    Raises:
        TypeError: If it is not.
    """
    if not isinstance(value, expected):
        raise TypeError(f"{argument} has to be {what}, not {type(value).__name__}")


def require_path(argument: str, value: object, *, or_none: bool = False) -> None:
    """Refuse `value` as `argument` unless it is text or a path to text, which pathlib takes, or None if `or_none`.

    Raises:
        TypeError: If it is not.
    """
    if value is None and or_none:
        return
    if not _is_text_path(value):
        what = "text, a path or None" if or_none else "text or a path"
        raise TypeError(f"{argument} has to be {what}, not {type(value).__name__}")


def _is_text_path(value: object) -> bool:
    """Return whether `value` is text, or an os.PathLike whose path is text and not bytes."""
    if isinstance(value, os.PathLike):
        value = os.fspath(cast(os.PathLike[str] | os.PathLike[bytes], value))
    return isinstance(value, str)
